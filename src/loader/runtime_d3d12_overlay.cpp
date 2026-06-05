#include "loader/runtime_d3d12_overlay.h"
#include "loader/runtime_hooks.h"

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "common/log.h"
#include "common/runtime_types.h"
#include "common/time.h"
#include "overlay/overlay_renderer.h"
#include "resource_inspection/signal_inspector.h"
#include "capture/d3d12_capture.h"
#include "ofa/optical_flow.h"
#include "fg/frame_interpolation.h"
#include "pacing/frame_pacer.h"

namespace chimera::loader::runtime {

namespace {

using chimera::common::CapabilityTier;
using chimera::common::ErrorCode;
using chimera::common::FrameSignals;
using chimera::common::QualityMode;
using chimera::common::Result;
using chimera::common::SignalSource;
using chimera::common::Status;

[[nodiscard]] DXGI_FORMAT ResolveViewFormat(const DXGI_FORMAT resource_format, const DXGI_FORMAT view_format) noexcept {
    return view_format != DXGI_FORMAT_UNKNOWN ? view_format : resource_format;
}

[[nodiscard]] std::string NarrowAscii(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t ch : text) {
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

[[nodiscard]] QualityMode RequestedQualityMode() {
    static const QualityMode mode = [] {
        wchar_t buffer[64]{};
        const auto size = ::GetEnvironmentVariableW(L"CHIMERA_QUALITY_MODE", buffer, static_cast<DWORD>(_countof(buffer)));
        if (size == 0U || size >= _countof(buffer)) {
            return QualityMode::kQuality;
        }

        const auto parse_result = chimera::common::ParseQualityMode(NarrowAscii(buffer));
        if (!parse_result.ok()) {
            return QualityMode::kQuality;
        }
        return parse_result.value();
    }();

    return mode;
}

resource_inspection::SignalInspector g_signal_inspector{};
UINT g_rtv_descriptor_increment_size{0U};
UINT g_dsv_descriptor_increment_size{0U};

void LogD3D12DebugMessages(ID3D12Device* device);

[[nodiscard]] Result<void> HrCheck(const HRESULT hr, const char* context) {
    if (SUCCEEDED(hr)) {
        return {};
    }

    std::ostringstream stream;
    stream << context << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return Status::Error(ErrorCode::kDeviceError, stream.str());
}

void TransitionResource(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after) {
    if (before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &barrier);
}

struct RuntimeSwapChainContext final {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue{};
    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap{};
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> back_buffers{};
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> command_allocators{};
    std::vector<std::uint64_t> fence_values{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list{};
    Microsoft::WRL::ComPtr<ID3D12Fence> fence{};
    overlay::OverlayRenderer overlay_renderer{};
    HANDLE fence_event{nullptr};
    UINT rtv_descriptor_size{0U};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    std::uint64_t next_fence_value{1U};
    std::uint64_t frame_index{0U};
    float last_overlay_ms{0.0F};
    std::uint32_t last_logged_render_width{0U};
    std::uint32_t last_logged_render_height{0U};
    std::string last_logged_depth_note{};
    chimera::common::Stopwatch perf_log_stopwatch{};
    std::uint64_t perf_log_frames{0U};
    float current_fps{0.0F};
    bool initialized{false};

    struct KeyState {
        bool is_down{false};
        bool Pressed(int vk) noexcept {
            bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down && !is_down) {
                is_down = true;
                return true;
            }
            if (!down) {
                is_down = false;
            }
            return false;
        }
    };

    KeyState key_f2{};
    KeyState key_f3{};
    KeyState key_f4{};

    bool fi_user_enabled{true};
    bool enable_vsync{true};
    chimera::common::QualityMode quality_mode{RequestedQualityMode()};

    capture::D3D12FrameCapture frame_capture{};
    ofa::OpticalFlowPipeline flow_pipeline{};
    fg::FrameInterpolationPipeline fi_pipeline{};
    pacing::FramePacer frame_pacer{};
    chimera::common::Stopwatch frame_timer{};
    std::uint64_t fi_frame_counter{0U};  // tracks alternating FI frames

    ~RuntimeSwapChainContext() {
        if (fence_event != nullptr) {
            ::CloseHandle(fence_event);
            fence_event = nullptr;
        }
    }

    [[nodiscard]] Result<void> WaitForFrame(const std::uint32_t index) {
        if (index >= fence_values.size()) {
            return {};
        }

        const auto fence_value = fence_values[index];
        if (fence_value == 0U || fence->GetCompletedValue() >= fence_value) {
            return {};
        }

        CHIMERA_RETURN_IF_ERROR(HrCheck(fence->SetEventOnCompletion(fence_value, fence_event), "ID3D12Fence::SetEventOnCompletion"));
        if (::WaitForSingleObject(fence_event, 5000U) != WAIT_OBJECT_0) {
            return Status::Error(ErrorCode::kTimeout, "Timed out waiting for runtime overlay fence.");
        }
        return {};
    }

    [[nodiscard]] Result<void> Flush() {
        if (!initialized) {
            return {};
        }

        const auto fence_value = next_fence_value++;
        CHIMERA_RETURN_IF_ERROR(HrCheck(command_queue->Signal(fence.Get(), fence_value), "ID3D12CommandQueue::Signal"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(fence->SetEventOnCompletion(fence_value, fence_event), "ID3D12Fence::SetEventOnCompletion"));
        if (::WaitForSingleObject(fence_event, 5000U) != WAIT_OBJECT_0) {
            return Status::Error(ErrorCode::kTimeout, "Timed out flushing runtime overlay queue.");
        }
        return {};
    }

    void ReleaseSizeDependentResources() {
        back_buffers.clear();
        command_allocators.clear();
        fence_values.clear();
        rtv_heap.Reset();
        initialized = false;
    }

    [[nodiscard]] Result<void> Initialize(IUnknown* swap_chain_unknown, IUnknown* device_or_queue) {
        ReleaseSizeDependentResources();

        Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain{};
        CHIMERA_RETURN_IF_ERROR(HrCheck(swap_chain_unknown->QueryInterface(IID_PPV_ARGS(swap_chain.GetAddressOf())), "QueryInterface(IDXGISwapChain3)"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(device_or_queue->QueryInterface(IID_PPV_ARGS(command_queue.ReleaseAndGetAddressOf())), "QueryInterface(ID3D12CommandQueue)"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(command_queue->GetDevice(IID_PPV_ARGS(device.ReleaseAndGetAddressOf())), "ID3D12CommandQueue::GetDevice"));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        CHIMERA_RETURN_IF_ERROR(HrCheck(swap_chain->GetDesc1(&desc), "IDXGISwapChain3::GetDesc1"));
        format = desc.Format;

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = desc.BufferCount;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap.ReleaseAndGetAddressOf())), "CreateDescriptorHeap(runtime RTV)"));
        rtv_descriptor_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        back_buffers.resize(desc.BufferCount);
        command_allocators.resize(desc.BufferCount);
        fence_values.resize(desc.BufferCount, 0U);

        auto rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0U; index < desc.BufferCount; ++index) {
            CHIMERA_RETURN_IF_ERROR(HrCheck(swap_chain->GetBuffer(index, IID_PPV_ARGS(back_buffers[index].ReleaseAndGetAddressOf())), "IDXGISwapChain3::GetBuffer"));
            device->CreateRenderTargetView(back_buffers[index].Get(), nullptr, rtv_handle);
            rtv_handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size);

            CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(command_allocators[index].ReleaseAndGetAddressOf())), "CreateCommandAllocator(runtime)"));
        }

        CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommandList(
            0U,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            command_allocators[0].Get(),
            nullptr,
            IID_PPV_ARGS(command_list.ReleaseAndGetAddressOf())),
            "CreateCommandList(runtime)"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->Close(), "ID3D12GraphicsCommandList::Close(runtime)"));

        CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.ReleaseAndGetAddressOf())), "CreateFence(runtime)"));
        if (fence_event != nullptr) {
            ::CloseHandle(fence_event);
            fence_event = nullptr;
        }
        fence_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            return Status::Error(ErrorCode::kDeviceError, "CreateEventW failed for runtime overlay fence.");
        }

        CHIMERA_RETURN_IF_ERROR(overlay_renderer.Initialize(device.Get(), format));

        // Initialize DLSS/FI pipelines — pass swap chain format so capture textures match
        CHIMERA_RETURN_IF_ERROR(frame_capture.Initialize(device.Get(), format));
        CHIMERA_RETURN_IF_ERROR(flow_pipeline.Initialize(device.Get()));
        CHIMERA_RETURN_IF_ERROR(fi_pipeline.Initialize(device.Get(), format));
        frame_pacer.Reset();

        initialized = true;

        std::ostringstream stream;
        stream << "Runtime D3D12 overlay initialized for " << desc.Width << 'x' << desc.Height << " format=" << static_cast<int>(desc.Format);
        chimera::common::LogInfo("runtime.overlay", stream.str());
        return {};
    }

    [[nodiscard]] Result<PresentOverrideResult> OnPresent(
        IUnknown* swap_chain_unknown,
        const UINT sync_interval_unused,
        const UINT flags,
        const DXGI_PRESENT_PARAMETERS* present_parameters) {
        (void)sync_interval_unused;
        if (!initialized && command_queue != nullptr) {
            CHIMERA_RETURN_IF_ERROR(Initialize(swap_chain_unknown, command_queue.Get()));
        }
        if (!initialized) {
            return PresentOverrideResult{ .handled = false, .overridden_sync_interval = 0U };
        }

        // ── Measure Frame Time ──────────────────────────────────────────
        const float frame_time_ms = static_cast<float>(frame_timer.ElapsedMilliseconds());
        frame_timer.Reset();
        frame_pacer.RecordFrameTime(frame_time_ms);

        // ── Key Press Handler ───────────────────────────────────────────
        if (key_f2.Pressed(VK_F2)) {
            fi_user_enabled = !fi_user_enabled;
            chimera::common::LogInfo("runtime.overlay", fi_user_enabled ? "FI toggled: ON" : "FI toggled: OFF");
            if (!fi_user_enabled) {
                frame_pacer.Reset();
                flow_pipeline.Reset();
                frame_capture.Reset();
            }
        }
        if (key_f3.Pressed(VK_F3)) {
            quality_mode = static_cast<chimera::common::QualityMode>((static_cast<int>(quality_mode) + 1) % 4);
            chimera::common::LogInfo("runtime.overlay", "Quality Mode changed to: " + std::string(chimera::common::ToString(quality_mode)));
        }
        if (key_f4.Pressed(VK_F4)) {
            enable_vsync = !enable_vsync;
            chimera::common::LogInfo("runtime.overlay", enable_vsync ? "VSync toggled: ON" : "VSync toggled: OFF");
        }

        Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain{};
        CHIMERA_RETURN_IF_ERROR(HrCheck(swap_chain_unknown->QueryInterface(IID_PPV_ARGS(swap_chain.GetAddressOf())), "QueryInterface(IDXGISwapChain3)"));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        CHIMERA_RETURN_IF_ERROR(HrCheck(swap_chain->GetDesc1(&desc), "IDXGISwapChain3::GetDesc1"));
        const auto buffer_index = swap_chain->GetCurrentBackBufferIndex();
        if (buffer_index >= back_buffers.size()) {
            return Status::Error(ErrorCode::kInvalidArgument, "Runtime overlay backbuffer index out of range.");
        }

        CHIMERA_RETURN_IF_ERROR(WaitForFrame(buffer_index));
        CHIMERA_RETURN_IF_ERROR(HrCheck(command_allocators[buffer_index]->Reset(), "ID3D12CommandAllocator::Reset(runtime)"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->Reset(command_allocators[buffer_index].Get(), nullptr), "ID3D12GraphicsCommandList::Reset(runtime)"));

        // ── Frame Pacing decision ───────────────────────────────────────
        const auto pacing_decision = frame_pacer.MakeDecision();
        const bool do_fi = fi_user_enabled
            && pacing_decision.insert_interpolated_frame
            && frame_capture.HasPreviousFrame();

        // ── Frame Capture (store back buffer for flow/FI) ──────────────
        TransitionResource(command_list.Get(), back_buffers[buffer_index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)frame_capture.CaptureFrame(command_list.Get(), back_buffers[buffer_index].Get(), desc.Width, desc.Height, frame_index);
        TransitionResource(command_list.Get(), back_buffers[buffer_index].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        auto rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv_handle.ptr += static_cast<SIZE_T>(buffer_index) * static_cast<SIZE_T>(rtv_descriptor_size);

        // ── Optical Flow & Frame Interpolation ─────────────────────────
        float flow_cpu_ms = 0.0F;
        float fi_cpu_ms = 0.0F;
        bool presented_interpolated = false;

        if (do_fi) {
            const auto& current_frame = frame_capture.GetFrame(0U);
            const auto& previous_frame = frame_capture.GetFrame(1U);
            if (current_frame.valid && previous_frame.valid) {
                auto flow_result = flow_pipeline.Execute(
                    command_list.Get(),
                    ofa::FlowInputs{
                        current_frame.color.Get(),
                        previous_frame.color.Get(),
                        desc.Width,
                        desc.Height,
                        frame_index,
                        false,
                        format,  // pass swap chain format for SRV creation
                    });
                if (flow_result.ok()) {
                    flow_cpu_ms = flow_result.value().compute_ms;

                    if (flow_pipeline.forward_flow() != nullptr) {
                        auto fi_result = fi_pipeline.Execute(
                            command_list.Get(),
                            fg::FiInputs{
                                previous_frame.color.Get(),
                                current_frame.color.Get(),
                                flow_pipeline.forward_flow(),
                                flow_pipeline.backward_flow(),
                                flow_pipeline.confidence(),
                                0.5F,
                                desc.Width,
                                desc.Height,
                                format,  // pass swap chain format for SRV creation
                            },
                            rtv_handle);
                        if (fi_result.ok()) {
                            fi_cpu_ms = fi_result.value().total_ms;
                            frame_pacer.RecordFiCost(fi_cpu_ms + flow_cpu_ms);
                            presented_interpolated = true;
                        }
                    }
                }
            }
        }

        const auto inspection = g_signal_inspector.Evaluate(desc.Width, desc.Height, desc.Format);
        FrameSignals signals = inspection.signals;
        const auto tier = chimera::common::EvaluateTierDecision(signals, chimera::common::kSceneStateNone, true);

        if (inspection.suggested_render_width != last_logged_render_width ||
            inspection.suggested_render_height != last_logged_render_height ||
            signals.depth.note != last_logged_depth_note) {
            std::ostringstream stream;
            stream
                << "Inspector render=" << inspection.suggested_render_width << 'x' << inspection.suggested_render_height
                << " display=" << desc.Width << 'x' << desc.Height
                << " color=\"" << signals.color.note << '"'
                << " depth=\"" << signals.depth.note << '"';
            chimera::common::LogInfo("runtime.inspect", stream.str());
            last_logged_render_width = inspection.suggested_render_width;
            last_logged_render_height = inspection.suggested_render_height;
            last_logged_depth_note = signals.depth.note;
        }

        ++perf_log_frames;
        const auto perf_elapsed_ms = perf_log_stopwatch.ElapsedMilliseconds();
        if (perf_elapsed_ms >= 1000.0) {
            const auto present_fps = static_cast<double>(perf_log_frames) * 1000.0 / perf_elapsed_ms;
            current_fps = static_cast<float>(present_fps);
            const auto avg_present_ms = perf_elapsed_ms / static_cast<double>(perf_log_frames);
            std::ostringstream stream;
            stream
                << "FPS=" << present_fps
                << " avgPresentMs=" << avg_present_ms
                << " mode=" << chimera::common::ToString(quality_mode)
                << " renderFraction=" << chimera::common::RenderFraction(quality_mode)
                << " render=" << inspection.suggested_render_width << 'x' << inspection.suggested_render_height
                << " display=" << desc.Width << 'x' << desc.Height
                << " tier=" << chimera::common::ToString(tier.tier)
                << " tsrEligible=" << (tier.allow_temporal_sr ? "yes" : "no")
                << " boost=" << (presented_interpolated ? "2x-frame-gen" : "off")
                << " vsync=" << (enable_vsync ? "on" : "off");
            chimera::common::LogInfo("runtime.perf", stream.str());
            perf_log_frames = 0U;
            perf_log_stopwatch.Reset();
        }

        overlay::OverlaySnapshot snapshot{};
        snapshot.tier = tier.tier == CapabilityTier::kDisabled ? CapabilityTier::kTierC : tier.tier;
        snapshot.render_width = inspection.suggested_render_width;
        snapshot.render_height = inspection.suggested_render_height;
        snapshot.display_width = desc.Width;
        snapshot.display_height = desc.Height;
        snapshot.scene_cpu_ms = 0.0F;
        snapshot.sr_cpu_ms = 0.0F;
        snapshot.overlay_cpu_ms = last_overlay_ms;
        snapshot.history_reset = false;
        snapshot.signals = signals;
        // Show user's FI toggle state in the overlay (independent of pacing decision)
        snapshot.fi_enabled = fi_user_enabled;
        snapshot.flow_cpu_ms = flow_cpu_ms;
        snapshot.fi_cpu_ms = fi_cpu_ms;
        snapshot.real_fps = current_fps;
        snapshot.effective_fps = presented_interpolated ? current_fps * 2.0F : current_fps;
        snapshot.pacing_latency_ms = pacing_decision.estimated_latency_ms;
        snapshot.vsync_enabled = enable_vsync;
        snapshot.quality_mode = quality_mode;

        if (presented_interpolated) {
            // Draw overlay on first frame (interpolated frame)
            auto overlay_draw_result = overlay_renderer.Draw(command_list.Get(), snapshot, rtv_handle, desc.Width, desc.Height);
            if (overlay_draw_result.ok()) {
                last_overlay_ms = overlay_draw_result.value();
            }

            TransitionResource(command_list.Get(), back_buffers[buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->Close(), "ID3D12GraphicsCommandList::Close(runtime first present)"));
            ID3D12CommandList* lists[] = {command_list.Get()};
            command_queue->ExecuteCommandLists(1U, lists);

            const auto fence_value = next_fence_value++;
            CHIMERA_RETURN_IF_ERROR(HrCheck(command_queue->Signal(fence.Get(), fence_value), "ID3D12CommandQueue::Signal(runtime first present)"));
            fence_values[buffer_index] = fence_value;

            // Present the interpolated frame
            (void)CallOriginalPresent(swap_chain.Get(), enable_vsync ? 1U : 0U, flags, present_parameters);

            // Now perform the second present for the real frame on the next backbuffer
            const auto next_buffer_index = swap_chain->GetCurrentBackBufferIndex();
            if (next_buffer_index < back_buffers.size()) {
                CHIMERA_RETURN_IF_ERROR(WaitForFrame(next_buffer_index));
                CHIMERA_RETURN_IF_ERROR(HrCheck(command_allocators[next_buffer_index]->Reset(), "ID3D12CommandAllocator::Reset(second present)"));
                CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->Reset(command_allocators[next_buffer_index].Get(), nullptr), "ID3D12GraphicsCommandList::Reset(second present)"));

                // Transition next backbuffer to COPY_DEST
                TransitionResource(command_list.Get(), back_buffers[next_buffer_index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

                // Copy captured real frame N+1 to next backbuffer
                const auto& current_frame = frame_capture.GetFrame(0U);
                if (current_frame.valid) {
                    TransitionResource(command_list.Get(), current_frame.color.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    command_list->CopyResource(back_buffers[next_buffer_index].Get(), current_frame.color.Get());
                    TransitionResource(command_list.Get(), current_frame.color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                }

                // Transition next backbuffer to RENDER_TARGET
                TransitionResource(command_list.Get(), back_buffers[next_buffer_index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);

                // Draw overlay on next backbuffer
                auto next_rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
                next_rtv_handle.ptr += static_cast<SIZE_T>(next_buffer_index) * static_cast<SIZE_T>(rtv_descriptor_size);

                overlay_draw_result = overlay_renderer.Draw(command_list.Get(), snapshot, next_rtv_handle, desc.Width, desc.Height);
                if (overlay_draw_result.ok()) {
                    last_overlay_ms = overlay_draw_result.value();
                }

                // Transition next backbuffer to PRESENT
                TransitionResource(command_list.Get(), back_buffers[next_buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->Close(), "ID3D12GraphicsCommandList::Close(runtime second present)"));

                ID3D12CommandList* lists2[] = {command_list.Get()};
                command_queue->ExecuteCommandLists(1U, lists2);

                const auto fence_value2 = next_fence_value++;
                CHIMERA_RETURN_IF_ERROR(HrCheck(command_queue->Signal(fence.Get(), fence_value2), "ID3D12CommandQueue::Signal(runtime second present)"));
                fence_values[next_buffer_index] = fence_value2;

                // Present the real frame
                (void)CallOriginalPresent(swap_chain.Get(), enable_vsync ? 1U : 0U, flags, present_parameters);
            }

            ++frame_index;
            LogD3D12DebugMessages(device.Get());
            return PresentOverrideResult{ .handled = true, .overridden_sync_interval = enable_vsync ? 1U : 0U };
        } else {
            auto overlay_draw_result = overlay_renderer.Draw(command_list.Get(), snapshot, rtv_handle, desc.Width, desc.Height);
            if (!overlay_draw_result.ok()) {
                return overlay_draw_result.status();
            }
            last_overlay_ms = overlay_draw_result.value();

            TransitionResource(command_list.Get(), back_buffers[buffer_index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->Close(), "ID3D12GraphicsCommandList::Close(runtime present)"));
            ID3D12CommandList* lists[] = {command_list.Get()};
            command_queue->ExecuteCommandLists(1U, lists);

            const auto fence_value = next_fence_value++;
            CHIMERA_RETURN_IF_ERROR(HrCheck(command_queue->Signal(fence.Get(), fence_value), "ID3D12CommandQueue::Signal(runtime)"));
            fence_values[buffer_index] = fence_value;
            ++frame_index;
            LogD3D12DebugMessages(device.Get());
            return PresentOverrideResult{ .handled = false, .overridden_sync_interval = enable_vsync ? 1U : 0U };
        }
    }
};

void LogD3D12DebugMessages(ID3D12Device* device) {
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> info_queue;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
        UINT64 num_messages = info_queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < num_messages; ++i) {
            SIZE_T message_length = 0;
            info_queue->GetMessage(i, nullptr, &message_length);
            std::vector<char> buffer(message_length);
            D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (SUCCEEDED(info_queue->GetMessage(i, message, &message_length))) {
                std::ostringstream stream;
                stream << "[D3D12 Debug] " << message->pDescription;
                chimera::common::LogWarning("runtime.d3d12.debug", stream.str());
            }
        }
        info_queue->ClearStoredMessages();
    }
}

std::mutex g_context_mutex{};
std::unordered_map<void*, std::unique_ptr<RuntimeSwapChainContext>> g_contexts{};

}  // namespace

void SetD3d12DescriptorIncrementSizes(const UINT rtv_increment_size, const UINT dsv_increment_size) noexcept {
    g_rtv_descriptor_increment_size = rtv_increment_size;
    g_dsv_descriptor_increment_size = dsv_increment_size;
}

void RegisterD3d12RenderTargetView(
    const D3D12_CPU_DESCRIPTOR_HANDLE handle,
    ID3D12Resource* resource,
    const D3D12_RENDER_TARGET_VIEW_DESC* desc) {
    if (resource == nullptr || handle.ptr == 0U) {
        return;
    }

    const auto resource_desc = resource->GetDesc();
    resource_inspection::ObservedDescriptor observed{};
    observed.width = static_cast<std::uint32_t>(resource_desc.Width);
    observed.height = resource_desc.Height;
    observed.format = ResolveViewFormat(resource_desc.Format, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN);
    observed.sample_count = resource_desc.SampleDesc.Count;
    observed.is_depth = false;
    g_signal_inspector.RegisterDescriptor(handle, reinterpret_cast<std::uintptr_t>(resource), observed);
}

void RegisterD3d12DepthStencilView(
    const D3D12_CPU_DESCRIPTOR_HANDLE handle,
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) {
    if (resource == nullptr || handle.ptr == 0U) {
        return;
    }

    const auto resource_desc = resource->GetDesc();
    resource_inspection::ObservedDescriptor observed{};
    observed.width = static_cast<std::uint32_t>(resource_desc.Width);
    observed.height = resource_desc.Height;
    observed.format = ResolveViewFormat(resource_desc.Format, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN);
    observed.sample_count = resource_desc.SampleDesc.Count;
    observed.is_depth = true;
    g_signal_inspector.RegisterDescriptor(handle, reinterpret_cast<std::uintptr_t>(resource), observed);
}

void ObserveD3d12RenderTargets(
    const UINT render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_target_views,
    const bool single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_view) {
    g_signal_inspector.ObserveRenderTargets(
        render_target_count,
        render_target_views,
        single_handle_to_descriptor_range,
        depth_stencil_view,
        g_rtv_descriptor_increment_size);
}

void RegisterD3d12SwapChain(IUnknown* swap_chain, IUnknown* device_or_queue) {
    if (swap_chain == nullptr || device_or_queue == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue{};
    if (FAILED(device_or_queue->QueryInterface(IID_PPV_ARGS(command_queue.GetAddressOf())))) {
        return;
    }

    auto context = std::make_unique<RuntimeSwapChainContext>();
    const auto init_result = context->Initialize(swap_chain, command_queue.Get());
    if (!init_result.ok()) {
        chimera::common::LogWarning("runtime.overlay", init_result.status().message);
        return;
    }

    std::scoped_lock lock(g_context_mutex);
    g_contexts[swap_chain] = std::move(context);
}

PresentOverrideResult HandleD3d12Present(
    IUnknown* swap_chain,
    const UINT sync_interval,
    const UINT flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters) {
    std::scoped_lock lock(g_context_mutex);
    const auto it = g_contexts.find(swap_chain);
    if (it == g_contexts.end()) {
        return PresentOverrideResult{ .handled = false, .overridden_sync_interval = sync_interval };
    }

    const auto render_result = it->second->OnPresent(swap_chain, sync_interval, flags, present_parameters);
    if (!render_result.ok()) {
        chimera::common::LogWarning("runtime.overlay", render_result.status().message);
        return PresentOverrideResult{ .handled = false, .overridden_sync_interval = it->second->enable_vsync ? 1U : 0U };
    }

    return render_result.value();
}

void HandleD3d12ResizeBuffers(IUnknown* swap_chain) {
    std::scoped_lock lock(g_context_mutex);
    const auto it = g_contexts.find(swap_chain);
    if (it == g_contexts.end()) {
        return;
    }

    const auto flush_result = it->second->Flush();
    if (!flush_result.ok()) {
        chimera::common::LogWarning("runtime.overlay", flush_result.status().message);
    }
    it->second->ReleaseSizeDependentResources();
}

void ShutdownD3d12OverlayTracking() noexcept {
    std::scoped_lock lock(g_context_mutex);
    g_contexts.clear();
    g_signal_inspector.Reset();
}

}  // namespace chimera::loader::runtime
