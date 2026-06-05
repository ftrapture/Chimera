#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include "common/log.h"
#include "common/runtime_types.h"
#include "common/time.h"
#include "config/config_loader.h"
#include "d3d12/device_context.h"
#include "d3d12/swap_chain_host.h"
#include "overlay/overlay_renderer.h"
#include "sr/analytic_tsr.h"
#include "ml/neural_sr.h"
#include "ofa/optical_flow.h"
#include "fg/frame_interpolation.h"
#include "pacing/frame_pacer.h"
#include "capture/d3d12_capture.h"

namespace chimera::d3d12 {
namespace {

using chimera::common::ErrorCode;
using chimera::common::FrameSignals;
using chimera::common::Result;
using chimera::common::Status;

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

[[nodiscard]] Result<Microsoft::WRL::ComPtr<ID3DBlob>> CompileShader(
    const char* source,
    const char* entry_point,
    const char* shader_target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> shader_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
    const auto hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry_point,
        shader_target,
        flags,
        0U,
        shader_blob.GetAddressOf(),
        error_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream stream;
        stream << "D3DCompile failed for " << entry_point << "/" << shader_target;
        if (error_blob) {
            stream << ": " << static_cast<const char*>(error_blob->GetBufferPointer());
        }
        return Status::Error(ErrorCode::kDeviceError, stream.str());
    }
    return shader_blob;
}

[[nodiscard]] float Halton(const std::uint64_t index, const int base) {
    float fraction = 1.0F;
    float result = 0.0F;
    auto current = index;
    while (current > 0U) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(current % static_cast<std::uint64_t>(base));
        current /= static_cast<std::uint64_t>(base);
    }
    return result;
}

[[nodiscard]] std::array<float, 2> JitterForFrame(const std::uint64_t frame_index) {
    return {
        Halton(frame_index % 8U + 1U, 2) - 0.5F,
        Halton(frame_index % 8U + 1U, 3) - 0.5F,
    };
}

[[nodiscard]] FrameSignals BuildHarnessSignals() {
    FrameSignals signals{};
    signals.color = {1.0F, true, true, common::SignalSource::kHarness, "direct low-res color"};
    signals.depth = {0.97F, true, true, common::SignalSource::kHarness, "procedural depth"};
    signals.motion = {0.95F, true, true, common::SignalSource::kHarness, "procedural motion"};
    signals.jitter = {1.0F, true, true, common::SignalSource::kHarness, "halton jitter"};
    signals.exposure = {0.90F, true, true, common::SignalSource::kHarness, "constant exposure"};
    signals.ui = {0.90F, true, true, common::SignalSource::kHarness, "overlay composition"};
    return signals;
}

struct WindowState final {
    std::uint32_t width{1600U};
    std::uint32_t height{900U};
    bool pending_resize{false};
    bool minimized{false};
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<WindowState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            const auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(l_param);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create_struct->lpCreateParams));
            return TRUE;
        }
        case WM_SIZE:
            if (state != nullptr) {
                state->width = static_cast<std::uint32_t>(LOWORD(l_param));
                state->height = static_cast<std::uint32_t>(HIWORD(l_param));
                state->pending_resize = true;
                state->minimized = (w_param == SIZE_MINIMIZED);
            }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, message, w_param, l_param);
    }
}

class SampleSceneRenderer final {
public:
    [[nodiscard]] Result<void> Initialize(ID3D12Device* device, std::uint32_t width, std::uint32_t height) {
        device_ = device;
        CHIMERA_RETURN_IF_ERROR(CreatePipeline());
        CHIMERA_RETURN_IF_ERROR(CreateConstantBuffer());
        return Resize(width, height);
    }

    [[nodiscard]] Result<void> Resize(std::uint32_t width, std::uint32_t height) {
        width_ = (std::max)(1U, width);
        height_ = (std::max)(1U, height);
        return CreateRenderTargets();
    }

    [[nodiscard]] float Render(
        ID3D12GraphicsCommandList* command_list,
        const float time_seconds,
        const std::array<float, 2>& current_jitter,
        const std::array<float, 2>& previous_jitter) {
        common::Stopwatch timer{};
        mapped_constants_->render_size[0] = static_cast<float>(width_);
        mapped_constants_->render_size[1] = static_cast<float>(height_);
        mapped_constants_->inv_render_size[0] = 1.0F / static_cast<float>(width_);
        mapped_constants_->inv_render_size[1] = 1.0F / static_cast<float>(height_);
        mapped_constants_->jitter_pixels[0] = current_jitter[0];
        mapped_constants_->jitter_pixels[1] = current_jitter[1];
        mapped_constants_->previous_jitter_pixels[0] = previous_jitter[0];
        mapped_constants_->previous_jitter_pixels[1] = previous_jitter[1];
        mapped_constants_->time_seconds = time_seconds;
        mapped_constants_->camera_offset[0] = std::sin(time_seconds * 0.55F) * 0.06F;
        mapped_constants_->camera_offset[1] = std::cos(time_seconds * 0.31F) * 0.05F;
        mapped_constants_->previous_camera_offset[0] = std::sin((time_seconds - 0.016F) * 0.55F) * 0.06F;
        mapped_constants_->previous_camera_offset[1] = std::cos((time_seconds - 0.016F) * 0.31F) * 0.05F;

        TransitionResource(command_list, color_.Get(), color_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        TransitionResource(command_list, motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        TransitionResource(command_list, depth_.Get(), depth_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        color_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        motion_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        depth_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;

        auto rtv0 = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        auto rtv1 = rtv0;
        auto rtv2 = rtv0;
        rtv1.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
        rtv2.ptr += static_cast<SIZE_T>(rtv_descriptor_size_) * 2U;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = {rtv0, rtv1, rtv2};

        const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_), 0.0F, 1.0F};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        const float clear_color[4] = {0.01F, 0.02F, 0.04F, 1.0F};
        const float clear_motion[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        const float clear_depth[4] = {1.0F, 1.0F, 1.0F, 1.0F};

        command_list->SetGraphicsRootSignature(root_signature_.Get());
        command_list->SetPipelineState(pipeline_state_.Get());
        command_list->RSSetViewports(1U, &viewport);
        command_list->RSSetScissorRects(1U, &scissor);
        command_list->OMSetRenderTargets(3U, rtvs, FALSE, nullptr);
        command_list->ClearRenderTargetView(rtv0, clear_color, 0U, nullptr);
        command_list->ClearRenderTargetView(rtv1, clear_motion, 0U, nullptr);
        command_list->ClearRenderTargetView(rtv2, clear_depth, 0U, nullptr);
        command_list->SetGraphicsRootConstantBufferView(0U, constant_buffer_->GetGPUVirtualAddress());
        command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list->DrawInstanced(3U, 1U, 0U, 0U);

        TransitionResource(command_list, color_.Get(), color_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, motion_.Get(), motion_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, depth_.Get(), depth_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        color_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        motion_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        depth_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        return static_cast<float>(timer.ElapsedMilliseconds());
    }

    [[nodiscard]] ID3D12Resource* color() const noexcept { return color_.Get(); }
    [[nodiscard]] ID3D12Resource* motion() const noexcept { return motion_.Get(); }
    [[nodiscard]] ID3D12Resource* depth() const noexcept { return depth_.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct alignas(256) SceneConstants final {
        float render_size[2];
        float inv_render_size[2];
        float jitter_pixels[2];
        float previous_jitter_pixels[2];
        float time_seconds;
        float padding0;
        float camera_offset[2];
        float previous_camera_offset[2];
    };

    [[nodiscard]] Result<void> CreatePipeline() {
        static constexpr char kShaderSource[] = R"(
cbuffer SceneConstants : register(b0)
{
    float2 renderSize;
    float2 invRenderSize;
    float2 jitterPixels;
    float2 previousJitterPixels;
    float timeSeconds;
    float padding0;
    float2 cameraOffset;
    float2 previousCameraOffset;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

struct PSOutput
{
    float4 color : SV_Target0;
    float4 motion : SV_Target1;
    float4 depth : SV_Target2;
};

PSOutput PSMain(VSOutput input)
{
    const float2 uv = input.uv + jitterPixels * invRenderSize;
    const float2 sceneUv = uv + cameraOffset;
    const float grid = 0.5 + 0.5 * sin((sceneUv.x * 12.0 + timeSeconds * 0.40) * 6.2831853);
    const float band = 0.5 + 0.5 * sin((sceneUv.y * 8.0 - timeSeconds * 0.25) * 6.2831853);
    const float2 centered = (uv - 0.5) * float2(1.25, 1.0) + float2(sin(timeSeconds) * 0.18, cos(timeSeconds * 0.7) * 0.11);
    const float orb = smoothstep(0.34, 0.30, length(centered));

    float3 color = lerp(float3(0.06, 0.10, 0.18), float3(0.12, 0.38, 0.65), grid);
    color += float3(0.45, 0.23, 0.09) * band * 0.35;
    color = lerp(color, float3(0.95, 0.68, 0.28), orb);

    const float depth = saturate(0.25 + (1.0 - orb) * 0.45 + band * 0.15);
    const float2 motion = (cameraOffset - previousCameraOffset) * renderSize;

    PSOutput output;
    output.color = float4(color, 1.0);
    output.motion = float4(motion, 0.0, 1.0);
    output.depth = float4(depth, depth, depth, 1.0);
    return output;
})";

        auto vs = CompileShader(kShaderSource, "VSMain", "vs_5_1");
        if (!vs.ok()) {
            return vs.status();
        }
        auto ps = CompileShader(kShaderSource, "PSMain", "ps_5_1");
        if (!ps.ok()) {
            return ps.status();
        }

        D3D12_ROOT_PARAMETER root_param{};
        root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_param.Descriptor.ShaderRegister = 0U;
        root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 1U;
        root_desc.pParameters = &root_param;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Microsoft::WRL::ComPtr<ID3DBlob> signature_blob{};
        Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
        CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, signature_blob.GetAddressOf(), error_blob.GetAddressOf()), "D3D12SerializeRootSignature(scene)"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateRootSignature(0U, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(root_signature_.ReleaseAndGetAddressOf())), "CreateRootSignature(scene)"));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = root_signature_.Get();
        pso_desc.VS = {vs.value()->GetBufferPointer(), vs.value()->GetBufferSize()};
        pso_desc.PS = {ps.value()->GetBufferPointer(), ps.value()->GetBufferSize()};
        pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
        pso_desc.BlendState.IndependentBlendEnable = FALSE;
        for (auto& render_target : pso_desc.BlendState.RenderTarget) {
            render_target.BlendEnable = FALSE;
            render_target.LogicOpEnable = FALSE;
            render_target.SrcBlend = D3D12_BLEND_ONE;
            render_target.DestBlend = D3D12_BLEND_ZERO;
            render_target.BlendOp = D3D12_BLEND_OP_ADD;
            render_target.SrcBlendAlpha = D3D12_BLEND_ONE;
            render_target.DestBlendAlpha = D3D12_BLEND_ZERO;
            render_target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            render_target.LogicOp = D3D12_LOGIC_OP_NOOP;
            render_target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
        pso_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        pso_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        pso_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.RasterizerState.MultisampleEnable = FALSE;
        pso_desc.RasterizerState.AntialiasedLineEnable = FALSE;
        pso_desc.RasterizerState.ForcedSampleCount = 0U;
        pso_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        pso_desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        pso_desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        pso_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        pso_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        pso_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        pso_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso_desc.DepthStencilState.BackFace = pso_desc.DepthStencilState.FrontFace;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 3U;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pso_desc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        pso_desc.RTVFormats[2] = DXGI_FORMAT_R16_FLOAT;
        pso_desc.SampleDesc.Count = 1U;
        CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(pipeline_state_.ReleaseAndGetAddressOf())), "CreateGraphicsPipelineState(scene)"));
        return {};
    }

    [[nodiscard]] Result<void> CreateConstantBuffer() {
        D3D12_HEAP_PROPERTIES upload_heap{};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC cb_desc{};
        cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cb_desc.Width = 65536U;
        cb_desc.Height = 1U;
        cb_desc.DepthOrArraySize = 1U;
        cb_desc.MipLevels = 1U;
        cb_desc.SampleDesc.Count = 1U;
        cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(constant_buffer_.ReleaseAndGetAddressOf())), "CreateCommittedResource(scene CB)"));
        CHIMERA_RETURN_IF_ERROR(HrCheck(constant_buffer_->Map(0U, nullptr, reinterpret_cast<void**>(&mapped_constants_)), "Map(scene CB)"));
        return {};
    }

    [[nodiscard]] Result<void> CreateRenderTargets() {
        color_.Reset();
        motion_.Reset();
        depth_.Reset();
        rtv_heap_.Reset();

        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.NumDescriptors = 3U;
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(rtv_heap_.ReleaseAndGetAddressOf())), "CreateDescriptorHeap(scene RTV)"));
        rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        auto create_texture = [&](DXGI_FORMAT format, const float clear[4], Microsoft::WRL::ComPtr<ID3D12Resource>& out_resource) -> Result<void> {
            D3D12_RESOURCE_DESC texture_desc{};
            texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture_desc.Width = width_;
            texture_desc.Height = height_;
            texture_desc.DepthOrArraySize = 1U;
            texture_desc.MipLevels = 1U;
            texture_desc.Format = format;
            texture_desc.SampleDesc.Count = 1U;
            texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear_value{};
            clear_value.Format = format;
            clear_value.Color[0] = clear[0];
            clear_value.Color[1] = clear[1];
            clear_value.Color[2] = clear[2];
            clear_value.Color[3] = clear[3];

            CHIMERA_RETURN_IF_ERROR(HrCheck(device_->CreateCommittedResource(
                &default_heap,
                D3D12_HEAP_FLAG_NONE,
                &texture_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                &clear_value,
                IID_PPV_ARGS(out_resource.ReleaseAndGetAddressOf())),
                "CreateCommittedResource(scene RT)"));
            return {};
        };

        const float clear_color[4] = {0.01F, 0.02F, 0.04F, 1.0F};
        const float clear_motion[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        const float clear_depth[4] = {1.0F, 1.0F, 1.0F, 1.0F};
        CHIMERA_RETURN_IF_ERROR(create_texture(DXGI_FORMAT_R16G16B16A16_FLOAT, clear_color, color_));
        CHIMERA_RETURN_IF_ERROR(create_texture(DXGI_FORMAT_R16G16_FLOAT, clear_motion, motion_));
        CHIMERA_RETURN_IF_ERROR(create_texture(DXGI_FORMAT_R16_FLOAT, clear_depth, depth_));

        auto handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(color_.Get(), nullptr, handle);
        handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
        device_->CreateRenderTargetView(motion_.Get(), nullptr, handle);
        handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
        device_->CreateRenderTargetView(depth_.Get(), nullptr, handle);

        color_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        motion_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        depth_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return {};
    }

    ID3D12Device* device_{nullptr};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffer_{};
    SceneConstants* mapped_constants_{nullptr};
    Microsoft::WRL::ComPtr<ID3D12Resource> color_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_{};
    D3D12_RESOURCE_STATES color_state_{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES motion_state_{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    D3D12_RESOURCE_STATES depth_state_{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    UINT rtv_descriptor_size_{0U};
};

[[nodiscard]] std::uint32_t ComputeRenderExtent(const std::uint32_t display_extent, const common::QualityMode quality_mode) {
    return (std::max)(1U, static_cast<std::uint32_t>(std::round(static_cast<float>(display_extent) * common::RenderFraction(quality_mode))));
}

}  // namespace

static void ReportError(HWND hwnd, const char* msg) {
    // Write to file for headless diagnostics
    if (FILE* f = std::fopen("chimera_error.txt", "a")) {
        std::fprintf(f, "%s\n", msg);
        std::fclose(f);
    }
    ::MessageBoxA(hwnd, msg, "Project Chimera", MB_ICONERROR);
}

int RunSampleApp(HINSTANCE instance, const int show_command) {
    WindowState window_state{};

    auto runtime_config = config::MakeDefaultRuntimeConfig();
    if (const auto load_result = config::LoadRuntimeConfigFromFile("chimera.sample.json"); load_result.ok()) {
        runtime_config = load_result.value();
    }

    if (const auto log_result = common::Logger::Get().Initialize({runtime_config.log_file, common::LogLevel::kInfo, true}); !log_result.ok()) {
        ReportError(nullptr, log_result.status().message.c_str());
        return -1;
    }

    const wchar_t* class_name = L"ProjectChimeraSampleWindow";
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    window_class.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    ::RegisterClassExW(&window_class);

    RECT window_rect{0, 0, static_cast<LONG>(runtime_config.sample_harness.window_width), static_cast<LONG>(runtime_config.sample_harness.window_height)};
    ::AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND window_handle = ::CreateWindowExW(
        0U, class_name, L"Project Chimera Sample Harness",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        window_rect.right - window_rect.left,
        window_rect.bottom - window_rect.top,
        nullptr, nullptr, instance, &window_state);

    if (window_handle == nullptr) {
        ReportError(nullptr, "CreateWindowExW failed — window handle is NULL.");
        return -1;
    }

    ::ShowWindow(window_handle, show_command);

    DeviceContext device_context{};
    if (const auto init_result = device_context.Initialize(runtime_config.sample_harness.enable_debug_layer); !init_result.ok()) {
        ReportError(window_handle, init_result.status().message.c_str());
        return -1;
    }

    SwapChainHost swap_chain{};
    if (const auto init_result = swap_chain.Initialize(window_handle, device_context, runtime_config.sample_harness.window_width, runtime_config.sample_harness.window_height); !init_result.ok()) {
        ReportError(window_handle, init_result.status().message.c_str());
        return -1;
    }

    SampleSceneRenderer scene_renderer{};
    auto quality_mode = runtime_config.sample_harness.quality_mode;
    auto render_width = ComputeRenderExtent(runtime_config.sample_harness.window_width, quality_mode);
    auto render_height = ComputeRenderExtent(runtime_config.sample_harness.window_height, quality_mode);
    if (const auto init_result = scene_renderer.Initialize(device_context.device(), render_width, render_height); !init_result.ok()) {
        ReportError(window_handle, init_result.status().message.c_str());
        return -1;
    }

    sr::AnalyticTsrPipeline tsr_pipeline{};
    if (const auto init_result = tsr_pipeline.Initialize(device_context.device(), swap_chain.format()); !init_result.ok()) {
        ReportError(window_handle, init_result.status().message.c_str());
        return -1;
    }

    ml::NeuralSrPipeline neural_sr_pipeline{};
    bool neural_sr_available = false;
    if (const auto init_result = neural_sr_pipeline.Initialize(device_context.device(), swap_chain.format()); init_result.ok()) {
        neural_sr_available = true;
        common::LogInfo("app", "Neural SR (DLSS) pipeline initialized successfully.");
    } else {
        common::LogWarning("app", "Neural SR init failed: ", init_result.status().message, " — using Analytic TSR only.");
    }

    bool use_neural_sr = false;  // F6 toggles this

    overlay::OverlayRenderer overlay_renderer{};
    if (const auto init_result = overlay_renderer.Initialize(device_context.device(), swap_chain.format()); !init_result.ok()) {
        ReportError(window_handle, init_result.status().message.c_str());
        return -1;
    }

    // ── Initialize new subsystems ───────────────────────────────────────
    ofa::OpticalFlowPipeline flow_pipeline{};
    if (const auto init_result = flow_pipeline.Initialize(device_context.device()); !init_result.ok()) {
        common::LogWarning("app", "Optical flow init failed: ", init_result.status().message, " — FI disabled.");
    }

    fg::FrameInterpolationPipeline fi_pipeline{};
    if (const auto init_result = fi_pipeline.Initialize(device_context.device(), swap_chain.format()); !init_result.ok()) {
        common::LogWarning("app", "Frame interpolation init failed: ", init_result.status().message, " — FI disabled.");
    }

    capture::D3D12FrameCapture frame_capture{};
    if (const auto init_result = frame_capture.Initialize(device_context.device()); !init_result.ok()) {
        common::LogWarning("app", "Frame capture init failed: ", init_result.status().message);
    }

    pacing::PacingConfig pacing_config{};
    pacing_config.max_latency_budget_ms = runtime_config.frame_interpolation.max_latency_ms;
    pacing_config.jitter_threshold_ms = runtime_config.frame_interpolation.jitter_threshold_ms;
    pacing_config.stability_window_frames = runtime_config.frame_interpolation.stability_window_frames;
    pacing::FramePacer frame_pacer{pacing_config};

    bool fi_user_enabled = runtime_config.frame_interpolation.enabled;
    bool enable_vsync = runtime_config.sample_harness.enable_vsync;
    float neural_sr_cpu_ms = 0.0F;

    MSG msg{};
    bool running = true;
    std::uint64_t frame_index = 0U;
    float previous_overlay_ms = 0.0F;
    float flow_cpu_ms = 0.0F;
    float fi_cpu_ms = 0.0F;

    // FPS tracking
    LARGE_INTEGER qpc_freq{}, last_fps_time{}, frame_start{};
    ::QueryPerformanceFrequency(&qpc_freq);
    ::QueryPerformanceCounter(&last_fps_time);
    std::uint32_t fps_frame_count = 0U;
    float measured_fps = 0.0F;

    common::LogInfo("app", "Entering main loop. FI=", fi_user_enabled ? "ON" : "OFF");

    while (running) {
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
            }

            // ── Hotkey handling ──────────────────────────────────────
            if (msg.message == WM_KEYDOWN) {
                switch (msg.wParam) {
                    case VK_F2:
                        fi_user_enabled = !fi_user_enabled;
                        common::LogInfo("app", "FI toggled: ", fi_user_enabled ? "ON" : "OFF");
                        if (!fi_user_enabled) {
                            frame_pacer.Reset();
                        }
                        break;
                    case VK_F3: {
                        const auto next_mode = static_cast<common::QualityMode>(
                            (static_cast<int>(quality_mode) + 1) % 4);
                        quality_mode = next_mode;
                        common::LogInfo("app", "Quality mode: ", common::ToString(quality_mode));
                        if (const auto r = scene_renderer.Resize(
                            ComputeRenderExtent(window_state.width, quality_mode),
                            ComputeRenderExtent(window_state.height, quality_mode)); r.ok()) {
                            tsr_pipeline.ResetHistory();
                            frame_capture.Reset();
                            flow_pipeline.Reset();
                            frame_pacer.SetSceneTransition(true);
                        }
                        break;
                    }
                    case VK_F4:
                        enable_vsync = !enable_vsync;
                        common::LogInfo("app", "VSync: ", enable_vsync ? "ON" : "OFF");
                        break;
                    case VK_F6:
                        if (neural_sr_available) {
                            use_neural_sr = !use_neural_sr;
                            common::LogInfo("app", "Upscaler: ", use_neural_sr ? "Neural SR (DLSS)" : "Analytic TSR");
                            if (use_neural_sr) {
                                neural_sr_pipeline.ResetHistory();
                            } else {
                                tsr_pipeline.ResetHistory();
                            }
                            frame_capture.Reset();
                            flow_pipeline.Reset();
                            frame_pacer.SetSceneTransition(true);
                        }
                        break;
                    case VK_ESCAPE:
                        running = false;
                        break;
                }
            }

            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (!running) break;

        if (window_state.minimized) {
            ::Sleep(16U);
            continue;
        }

        ::QueryPerformanceCounter(&frame_start);

        if (window_state.pending_resize && window_state.width > 0U && window_state.height > 0U) {
            window_state.pending_resize = false;
            if (const auto r = device_context.Flush(); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }
            if (const auto r = swap_chain.Resize(device_context, window_state.width, window_state.height); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }
            if (const auto r = scene_renderer.Resize(
                ComputeRenderExtent(window_state.width, quality_mode),
                ComputeRenderExtent(window_state.height, quality_mode)); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }
            tsr_pipeline.ResetHistory();
            if (neural_sr_available) neural_sr_pipeline.ResetHistory();
            frame_capture.Reset();
            flow_pipeline.Reset();
            frame_pacer.Reset();
        }

        if (const auto r = swap_chain.WaitForFrame(device_context); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }
        if (const auto r = device_context.BeginFrame(swap_chain.current_back_buffer_index()); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }

        auto* command_list = device_context.command_list();
        const auto current_jitter = JitterForFrame(frame_index);
        const auto previous_jitter = JitterForFrame(frame_index == 0U ? 0U : frame_index - 1U);
        const auto time_seconds = static_cast<float>(frame_index) / 60.0F;

        TransitionResource(command_list, swap_chain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // ── Render scene at lower resolution ────────────────────────
        const auto scene_cpu_ms = scene_renderer.Render(command_list, time_seconds, current_jitter, previous_jitter);
        const auto signals = BuildHarnessSignals();
        const auto tier = common::EvaluateTierDecision(signals, common::kSceneStateNone, true, runtime_config.tier_thresholds);

        // ── Temporal Super Resolution upscale ───────────────────────
        float sr_total_ms = 0.0F;
        bool sr_history_reset = false;
        neural_sr_cpu_ms = 0.0F;

        if (use_neural_sr && neural_sr_available) {
            const auto neural_result = neural_sr_pipeline.Execute(
                command_list,
                ml::NeuralSrInputs{
                    scene_renderer.color(),
                    scene_renderer.depth(),
                    scene_renderer.motion(),
                    scene_renderer.width(),
                    scene_renderer.height(),
                    swap_chain.width(),
                    swap_chain.height(),
                    current_jitter,
                    previous_jitter,
                    1.0F,
                    frame_index == 0U,
                    frame_index,
                },
                swap_chain.CurrentRtv());
            if (!neural_result.ok()) { ReportError(nullptr, neural_result.status().message.c_str()); break; }
            sr_total_ms = neural_result.value().inference_gpu_ms;
            sr_history_reset = neural_result.value().history_reset;
            neural_sr_cpu_ms = sr_total_ms;
        } else {
            const auto sr_result = tsr_pipeline.Execute(
                command_list,
                sr::SrInputs{
                    scene_renderer.color(),
                    scene_renderer.depth(),
                    scene_renderer.motion(),
                    scene_renderer.width(),
                    scene_renderer.height(),
                    swap_chain.width(),
                    swap_chain.height(),
                    current_jitter,
                    previous_jitter,
                    1.0F,
                    runtime_config.sample_harness.sharpen_strength,
                    1.0F,   // variance_clip_gamma
                    0.50F,  // anti_flicker_strength
                    frame_index == 0U,
                    frame_index,
                },
                swap_chain.CurrentRtv());
            if (!sr_result.ok()) { ReportError(nullptr, sr_result.status().message.c_str()); break; }
            sr_total_ms = sr_result.value().accumulate_cpu_ms + sr_result.value().sharpen_cpu_ms;
            sr_history_reset = sr_result.value().history_reset;
        }

        // ── Frame Capture (store upscaled frame for flow/FI) ────────
        // Capture the current back buffer (which now has the SR output)
        TransitionResource(command_list, swap_chain.CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        (void)frame_capture.CaptureFrame(command_list,
            swap_chain.CurrentBackBuffer(),
            swap_chain.width(), swap_chain.height(), frame_index);
        TransitionResource(command_list, swap_chain.CurrentBackBuffer(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        // ── Frame Pacing decision ───────────────────────────────────
        const auto pacing_decision = frame_pacer.MakeDecision();
        const bool do_fi = fi_user_enabled
            && pacing_decision.insert_interpolated_frame
            && frame_capture.HasPreviousFrame()
            && flow_pipeline.forward_flow() != nullptr;

        // ── Optical Flow (if FI is active) ──────────────────────────
        flow_cpu_ms = 0.0F;
        if (fi_user_enabled && frame_capture.HasPreviousFrame()) {
            const auto& current_frame = frame_capture.GetFrame(0U);
            const auto& previous_frame = frame_capture.GetFrame(1U);
            if (current_frame.valid && previous_frame.valid) {
                // Transition captured frames for compute shader access
                auto flow_result = flow_pipeline.Execute(
                    command_list,
                    ofa::FlowInputs{
                        current_frame.color.Get(),
                        previous_frame.color.Get(),
                        swap_chain.width(),
                        swap_chain.height(),
                        frame_index,
                        false,
                    });
                if (flow_result.ok()) {
                    flow_cpu_ms = flow_result.value().compute_ms;
                }
            }
        }

        // ── Frame Interpolation ─────────────────────────────────────
        fi_cpu_ms = 0.0F;
        if (do_fi && flow_pipeline.forward_flow()) {
            const auto& current_frame = frame_capture.GetFrame(0U);
            const auto& previous_frame = frame_capture.GetFrame(1U);

            if (current_frame.valid && previous_frame.valid) {
                auto fi_result = fi_pipeline.Execute(
                    command_list,
                    fg::FiInputs{
                        previous_frame.color.Get(),
                        current_frame.color.Get(),
                        flow_pipeline.forward_flow(),
                        flow_pipeline.backward_flow(),
                        flow_pipeline.confidence(),
                        0.5F,
                        swap_chain.width(),
                        swap_chain.height(),
                    },
                    swap_chain.CurrentRtv());
                if (fi_result.ok()) {
                    fi_cpu_ms = fi_result.value().total_ms;
                    frame_pacer.RecordFiCost(fi_cpu_ms + flow_cpu_ms);
                }
            }
        }

        // ── Build overlay snapshot ──────────────────────────────────
        overlay::OverlaySnapshot snapshot{};
        snapshot.tier = tier.tier;
        snapshot.render_width = scene_renderer.width();
        snapshot.render_height = scene_renderer.height();
        snapshot.display_width = swap_chain.width();
        snapshot.display_height = swap_chain.height();
        snapshot.scene_cpu_ms = scene_cpu_ms;
        snapshot.sr_cpu_ms = sr_total_ms;
        snapshot.overlay_cpu_ms = previous_overlay_ms;
        snapshot.history_reset = sr_history_reset;
        snapshot.signals = signals;
        snapshot.fi_enabled = do_fi;
        snapshot.flow_cpu_ms = flow_cpu_ms;
        snapshot.fi_cpu_ms = fi_cpu_ms;
        snapshot.real_fps = measured_fps;
        snapshot.effective_fps = do_fi ? measured_fps * 2.0F : measured_fps;
        snapshot.pacing_latency_ms = pacing_decision.estimated_latency_ms;

        const auto overlay_result = overlay_renderer.Draw(command_list, snapshot, swap_chain.CurrentRtv(), swap_chain.width(), swap_chain.height());
        if (!overlay_result.ok()) { ReportError(nullptr, overlay_result.status().message.c_str()); break; }
        previous_overlay_ms = overlay_result.value();

        TransitionResource(command_list, swap_chain.CurrentBackBuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        if (const auto r = device_context.ExecuteCommandList(); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }
        if (const auto r = swap_chain.Present(device_context, enable_vsync); !r.ok()) { ReportError(nullptr, r.status().message.c_str()); break; }

        // ── FPS tracking ────────────────────────────────────────────
        LARGE_INTEGER frame_end{};
        ::QueryPerformanceCounter(&frame_end);
        const auto frame_time_ms = static_cast<float>(
            static_cast<double>(frame_end.QuadPart - frame_start.QuadPart) * 1000.0 /
            static_cast<double>(qpc_freq.QuadPart));
        frame_pacer.RecordFrameTime(frame_time_ms);

        ++fps_frame_count;
        const auto elapsed_since_fps = static_cast<double>(frame_end.QuadPart - last_fps_time.QuadPart) /
            static_cast<double>(qpc_freq.QuadPart);
        if (elapsed_since_fps >= 1.0) {
            measured_fps = static_cast<float>(fps_frame_count) / static_cast<float>(elapsed_since_fps);
            fps_frame_count = 0U;
            last_fps_time = frame_end;
        }

        frame_pacer.SetSceneTransition(false);
        ++frame_index;
    }

    (void)device_context.Flush();
    common::Logger::Get().Shutdown();
    return 0;
}

}  // namespace chimera::d3d12

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    return chimera::d3d12::RunSampleApp(instance, show_command);
}
