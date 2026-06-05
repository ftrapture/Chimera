#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include "common/result.h"
#include "common/log.h"
#include "d3d12/device_context.h"
#include "sr/analytic_tsr.h"
#include "ml/neural_sr.h"

namespace {

using Microsoft::WRL::ComPtr;
using chimera::common::Result;
using chimera::common::Status;
using chimera::common::ErrorCode;

[[nodiscard]] Result<void> HrCheck(const HRESULT hr, const char* context) {
    if (SUCCEEDED(hr)) {
        return {};
    }
    std::ostringstream stream;
    stream << context << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return Status::Error(ErrorCode::kDeviceError, stream.str());
}

Result<ComPtr<ID3D12Resource>> CreateTextureFromRawFile(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* cmd_list,
    const std::filesystem::path& path,
    UINT width,
    UINT height,
    DXGI_FORMAT format) {

    std::ifstream in_file(path, std::ios::binary);
    if (!in_file) {
        return Status::Error(ErrorCode::kInvalidArgument, "Failed to open raw file: " + path.string());
    }

    UINT64 bpp = 16; // 128-bit
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) bpp = 8;
    else if (format == DXGI_FORMAT_R16G16_FLOAT) bpp = 4;
    else if (format == DXGI_FORMAT_R16_FLOAT) bpp = 2;
    else if (format == DXGI_FORMAT_R8G8B8A8_UNORM) bpp = 4;

    UINT64 row_bytes = width * bpp;
    std::vector<char> file_data(row_bytes * height);
    in_file.read(file_data.data(), file_data.size());

    // Create Default Texture
    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> texture{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(texture.GetAddressOf())),
        "CreateDefaultTexture"));

    // Create Upload Buffer
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload_buffer{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &upload_heap,
        D3D12_HEAP_FLAG_NONE,
        &buffer_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(upload_buffer.GetAddressOf())),
        "CreateUploadBuffer"));

    // Copy file data to upload buffer with pitch alignment
    void* data_ptr = nullptr;
    CHIMERA_RETURN_IF_ERROR(HrCheck(upload_buffer->Map(0, nullptr, &data_ptr), "MapUploadBuffer"));

    char* dst_ptr = reinterpret_cast<char*>(data_ptr);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(dst_ptr + y * footprint.Footprint.RowPitch, file_data.data() + y * row_bytes, row_bytes);
    }
    upload_buffer->Unmap(0, nullptr);

    // Copy to default texture
    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = texture.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload_buffer.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = footprint;

    cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // Transition to shader resource state
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(1U, &barrier);

    // Close, execute & wait
    CHIMERA_RETURN_IF_ERROR(HrCheck(cmd_list->Close(), "CloseCommandList"));
    ID3D12CommandList* lists[] = {cmd_list};
    queue->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())), "CreateFence"));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    CloseHandle(fence_event);

    return texture;
}

Result<void> SaveTextureToFile(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* texture,
    const std::filesystem::path& output_path) {

    D3D12_RESOURCE_DESC desc = texture->GetDesc();
    
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total_bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total_bytes);

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = total_bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback_buffer{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &readback_heap,
        D3D12_HEAP_FLAG_NONE,
        &buffer_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(readback_buffer.GetAddressOf())),
        "CreateReadbackBuffer"));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback_buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(1U, &barrier);

    cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    cmd_list->ResourceBarrier(1U, &barrier);

    CHIMERA_RETURN_IF_ERROR(HrCheck(cmd_list->Close(), "CloseCommandList"));
    ID3D12CommandList* lists[] = {cmd_list};
    queue->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())), "CreateFence"));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    CloseHandle(fence_event);

    void* data_ptr = nullptr;
    CHIMERA_RETURN_IF_ERROR(HrCheck(readback_buffer->Map(0, nullptr, &data_ptr), "MapReadbackBuffer"));

    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) {
        readback_buffer->Unmap(0, nullptr);
        return Status::Error(ErrorCode::kInvalidArgument, "Failed to open output file for writing: " + output_path.string());
    }

    const UINT64 row_pitch = footprint.Footprint.RowPitch;
    const UINT64 actual_row_bytes = footprint.Footprint.Width * 8; // R16G16B16A16_FLOAT is 8 bytes
    const char* src_ptr = reinterpret_cast<const char*>(data_ptr);

    for (UINT y = 0; y < footprint.Footprint.Height; ++y) {
        out_file.write(src_ptr + y * row_pitch, actual_row_bytes);
    }

    readback_buffer->Unmap(0, nullptr);
    return {};
}

void PrintDebugMessages(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> info_queue;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
        UINT64 num_messages = info_queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < num_messages; ++i) {
            SIZE_T message_length = 0;
            info_queue->GetMessage(i, nullptr, &message_length);
            std::vector<char> buffer(message_length);
            D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (SUCCEEDED(info_queue->GetMessage(i, message, &message_length))) {
                std::cerr << "[D3D12 Debug] " << message->pDescription << std::endl;
            }
        }
        info_queue->ClearStoredMessages();
    }
}

} // namespace

int main() {
    std::cout << "[Chimera Capture Replay] Initializing..." << std::endl;
    std::filesystem::path dataset_dir = "dataset";
    std::filesystem::path output_dir = "replay_output";
    std::filesystem::create_directories(output_dir);

    if (!std::filesystem::exists(dataset_dir)) {
        std::cerr << "Dataset directory does not exist! Please run chimera_dataset_gen.exe first." << std::endl;
        return -1;
    }

    chimera::d3d12::DeviceContext device_context{};
    auto init_res = device_context.Initialize(true);
    if (!init_res.ok()) {
        std::cerr << "DeviceContext Init failed: " << init_res.status().message << std::endl;
        return -1;
    }

    auto* device = device_context.device();
    auto* queue = device_context.command_queue();
    auto* cmd_list = device_context.command_list();

    const std::uint32_t lr_width = 800U;
    const std::uint32_t lr_height = 450U;
    const std::uint32_t hr_width = 1600U;
    const std::uint32_t hr_height = 900U;

    // Initialize Upscalers
    chimera::sr::AnalyticTsrPipeline tsr_pipeline{};
    auto tsr_init_res = tsr_pipeline.Initialize(device, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!tsr_init_res.ok()) {
        std::cerr << "Analytic TSR Initialize failed: " << tsr_init_res.status().message << std::endl;
        return -1;
    }

    chimera::ml::NeuralSrPipeline neural_pipeline{};
    auto neural_init_res = neural_pipeline.Initialize(device, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!neural_init_res.ok()) {
        std::cerr << "Neural SR Initialize failed: " << neural_init_res.status().message << std::endl;
        return -1;
    }

    // Allocate temporary upscale target textures
    D3D12_RESOURCE_DESC target_desc{};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = hr_width;
    target_desc.Height = hr_height;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    clear_val.Color[0] = 0.0f;
    clear_val.Color[1] = 0.0f;
    clear_val.Color[2] = 0.0f;
    clear_val.Color[3] = 1.0f;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> tsr_target{};
    device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &clear_val, IID_PPV_ARGS(tsr_target.GetAddressOf()));

    ComPtr<ID3D12Resource> neural_target{};
    device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, &clear_val, IID_PPV_ARGS(neural_target.GetAddressOf()));

    // Allocate RTV descriptors
    ComPtr<ID3D12DescriptorHeap> rtv_heap{};
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = 2U;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(rtv_heap.GetAddressOf()));
    UINT rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(tsr_target.Get(), nullptr, rtv_handle);
    rtv_handle.ptr += rtv_increment;
    device->CreateRenderTargetView(neural_target.Get(), nullptr, rtv_handle);

    // Count matching raw files sequentially
    int frame_count = 0;
    while (std::filesystem::exists(dataset_dir / ("frame_" + std::to_string(frame_count) + "_color_lr.raw"))) {
        frame_count++;
    }

    std::cout << "[Chimera Capture Replay] Found " << frame_count << " frames in dataset. Replaying..." << std::endl;

    for (int i = 0; i < frame_count; ++i) {
        std::filesystem::path color_path = dataset_dir / ("frame_" + std::to_string(i) + "_color_lr.raw");
        std::filesystem::path depth_path = dataset_dir / ("frame_" + std::to_string(i) + "_depth_lr.raw");
        std::filesystem::path motion_path = dataset_dir / ("frame_" + std::to_string(i) + "_motion_lr.raw");

        // Upload input textures
        if (const auto r = device_context.BeginFrame(0); !r.ok()) {
            std::cerr << "BeginFrame 0 failed for color: " << r.status().message << std::endl;
            return -1;
        }
        auto lr_color = CreateTextureFromRawFile(device, queue, cmd_list, color_path, lr_width, lr_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        
        if (const auto r = device_context.BeginFrame(0); !r.ok()) {
            std::cerr << "BeginFrame 0 failed for depth: " << r.status().message << std::endl;
            return -1;
        }
        auto lr_depth = CreateTextureFromRawFile(device, queue, cmd_list, depth_path, lr_width, lr_height, DXGI_FORMAT_R16_FLOAT);
        
        if (const auto r = device_context.BeginFrame(0); !r.ok()) {
            std::cerr << "BeginFrame 0 failed for motion: " << r.status().message << std::endl;
            return -1;
        }
        auto lr_motion = CreateTextureFromRawFile(device, queue, cmd_list, motion_path, lr_width, lr_height, DXGI_FORMAT_R16G16_FLOAT);

        if (!lr_color.ok()) {
            std::cerr << "Failed to upload color texture for frame " << i << ": " << lr_color.status().message << std::endl;
            return -1;
        }
        if (!lr_depth.ok()) {
            std::cerr << "Failed to upload depth texture for frame " << i << ": " << lr_depth.status().message << std::endl;
            return -1;
        }
        if (!lr_motion.ok()) {
            std::cerr << "Failed to upload motion texture for frame " << i << ": " << lr_motion.status().message << std::endl;
            return -1;
        }

        std::array<float, 2> jitter = {0.0f, 0.0f};

        // --- Run TSR Pipeline ---
        if (const auto r = device_context.BeginFrame(0); !r.ok()) {
            std::cerr << "BeginFrame 0 failed for pipelines: " << r.status().message << std::endl;
            return -1;
        }
        
        D3D12_RESOURCE_BARRIER target_barriers[2]{};
        target_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        target_barriers[0].Transition.pResource = tsr_target.Get();
        target_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        target_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        target_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        target_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        target_barriers[1].Transition.pResource = neural_target.Get();
        target_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        target_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        target_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_list->ResourceBarrier(2U, target_barriers);

        auto tsr_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        auto tsr_res = tsr_pipeline.Execute(cmd_list, chimera::sr::SrInputs{
            lr_color.value().Get(),
            lr_depth.value().Get(),
            lr_motion.value().Get(),
            lr_width,
            lr_height,
            hr_width,
            hr_height,
            jitter,
            jitter,
            1.0f,
            0.2f,
            1.0f,
            0.5f,
            i == 0,
            static_cast<std::uint64_t>(i)
        }, tsr_rtv);

        if (!tsr_res.ok()) {
            std::cerr << "TSR Pipeline execution failed: " << tsr_res.status().message << std::endl;
            return -1;
        }

        // --- Run Neural SR Pipeline ---
        auto neural_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        neural_rtv.ptr += rtv_increment;
        auto neural_res = neural_pipeline.Execute(cmd_list, chimera::ml::NeuralSrInputs{
            lr_color.value().Get(),
            lr_depth.value().Get(),
            lr_motion.value().Get(),
            lr_width,
            lr_height,
            hr_width,
            hr_height,
            jitter,
            jitter,
            1.0f,
            i == 0,
            static_cast<std::uint64_t>(i)
        }, neural_rtv);

        if (!neural_res.ok()) {
            std::cerr << "Neural SR Pipeline execution failed: " << neural_res.status().message << std::endl;
            return -1;
        }

        // Transition back to shader resource
        target_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        target_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        target_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        target_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        cmd_list->ResourceBarrier(2U, target_barriers);

        if (const auto r = device_context.ExecuteCommandList(); !r.ok()) {
            std::cerr << "ExecuteCommandList failed after pipelines execution: " << r.status().message << std::endl;
            PrintDebugMessages(device);
            return -1;
        }
        if (const auto r = device_context.Flush(); !r.ok()) {
            std::cerr << "Flush failed after pipelines execution: " << r.status().message << std::endl;
            PrintDebugMessages(device);
            return -1;
        }

        // Save upscaled outputs
        std::filesystem::path tsr_out_path = output_dir / ("frame_" + std::to_string(i) + "_tsr_out.raw");
        std::filesystem::path neural_out_path = output_dir / ("frame_" + std::to_string(i) + "_neural_out.raw");

        if (const auto r = device_context.BeginFrame(0); !r.ok()) {
            std::cerr << "BeginFrame 0 failed for tsr save: " << r.status().message << std::endl;
            return -1;
        }
        SaveTextureToFile(device, queue, cmd_list, tsr_target.Get(), tsr_out_path);

        if (const auto r = device_context.BeginFrame(0); !r.ok()) {
            std::cerr << "BeginFrame 0 failed for neural save: " << r.status().message << std::endl;
            return -1;
        }
        SaveTextureToFile(device, queue, cmd_list, neural_target.Get(), neural_out_path);

        std::cout << "Replayed & Saved Frame " << i + 1 << "/" << frame_count << std::endl;
    }

    std::cout << "[Chimera Capture Replay] Offline replay completed successfully under ./replay_output/" << std::endl;
    return 0;
}
