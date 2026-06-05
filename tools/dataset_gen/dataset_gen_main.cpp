#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"
#include "common/log.h"
#include "d3d12/device_context.h"
#include "d3d12/sample_scene.h"

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

    // Create Readback Buffer
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

    // Copy Texture to Buffer
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback_buffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = texture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    // Transition texture to COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(1U, &barrier);

    cmd_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition back
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmd_list->ResourceBarrier(1U, &barrier);

    // Execute & Flush
    CHIMERA_RETURN_IF_ERROR(HrCheck(cmd_list->Close(), "CloseCommandList"));
    ID3D12CommandList* lists[] = {cmd_list};
    queue->ExecuteCommandLists(1U, lists);

    // Wait for GPU
    ComPtr<ID3D12Fence> fence{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf())), "CreateFence"));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    CloseHandle(fence_event);

    // Map & Write
    void* data_ptr = nullptr;
    CHIMERA_RETURN_IF_ERROR(HrCheck(readback_buffer->Map(0, nullptr, &data_ptr), "MapReadbackBuffer"));

    // Write file
    std::ofstream out_file(output_path, std::ios::binary);
    if (!out_file) {
        readback_buffer->Unmap(0, nullptr);
        return Status::Error(ErrorCode::kInvalidArgument, "Failed to open output file for writing: " + output_path.string());
    }

    // Copy row by row to strip padding alignment in D3D12 row pitches
    const UINT64 row_pitch = footprint.Footprint.RowPitch;
    const UINT64 row_bytes = footprint.Footprint.Width * (total_bytes / (footprint.Footprint.Height * footprint.Footprint.RowPitch));
    
    // Calculate actual pixel size safely
    DXGI_FORMAT format = desc.Format;
    UINT64 bpp = 16; // default 128-bit
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) bpp = 8;
    else if (format == DXGI_FORMAT_R16G16_FLOAT) bpp = 4;
    else if (format == DXGI_FORMAT_R16_FLOAT) bpp = 2;
    else if (format == DXGI_FORMAT_R8G8B8A8_UNORM) bpp = 4;

    const UINT64 actual_row_bytes = footprint.Footprint.Width * bpp;
    const char* src_ptr = reinterpret_cast<const char*>(data_ptr);

    for (UINT y = 0; y < footprint.Footprint.Height; ++y) {
        out_file.write(src_ptr + y * row_pitch, actual_row_bytes);
    }

    readback_buffer->Unmap(0, nullptr);
    return {};
}

} // namespace

int main(int argc, char* argv[]) {
    int frames_to_generate = 10;
    if (argc > 2 && std::string(argv[1]) == "--frames") {
        frames_to_generate = std::stoi(argv[2]);
    }

    std::cout << "[Chimera Dataset Gen] Initializing..." << std::endl;
    std::filesystem::path dataset_dir = "dataset";
    std::filesystem::create_directories(dataset_dir);

    chimera::d3d12::DeviceContext device_context{};
    auto init_res = device_context.Initialize(false);
    if (!init_res.ok()) {
        std::cerr << "DeviceContext Init failed: " << init_res.status().message << std::endl;
        return -1;
    }

    auto* device = device_context.device();
    auto* queue = device_context.command_queue();
    auto* cmd_list = device_context.command_list();

    chimera::d3d12::SampleSceneRenderer scene_renderer{};
    // Render target sizes
    const std::uint32_t lr_width = 800U;
    const std::uint32_t lr_height = 450U;
    const std::uint32_t hr_width = 1600U;
    const std::uint32_t hr_height = 900U;

    std::cout << "[Chimera Dataset Gen] Rendering & Capturing " << frames_to_generate << " frames..." << std::endl;

    for (int i = 0; i < frames_to_generate; ++i) {
        float time_seconds = static_cast<float>(i) / 60.0f;
        std::array<float, 2> current_jitter = {0.0f, 0.0f};
        std::array<float, 2> previous_jitter = {0.0f, 0.0f};

        // 1. Render High-Res Ground Truth
        auto resize_res = scene_renderer.Initialize(device, hr_width, hr_height);
        if (!resize_res.ok()) {
            std::cerr << "SceneRenderer Initialize HR failed: " << resize_res.status().message << std::endl;
            return -1;
        }

        device_context.BeginFrame(0);
        scene_renderer.Render(cmd_list, time_seconds, current_jitter, previous_jitter);
        device_context.ExecuteCommandList();
        device_context.Flush();

        std::filesystem::path gt_path = dataset_dir / ("frame_" + std::to_string(i) + "_color_gt.raw");
        device_context.BeginFrame(0);
        SaveTextureToFile(device, queue, cmd_list, scene_renderer.color(), gt_path);
        device_context.ExecuteCommandList();
        device_context.Flush();

        // 2. Render Low-Res inputs
        resize_res = scene_renderer.Resize(lr_width, lr_height);
        if (!resize_res.ok()) {
            std::cerr << "SceneRenderer Resize LR failed: " << resize_res.status().message << std::endl;
            return -1;
        }

        device_context.BeginFrame(0);
        scene_renderer.Render(cmd_list, time_seconds, current_jitter, previous_jitter);
        device_context.ExecuteCommandList();
        device_context.Flush();

        std::filesystem::path lr_color_path = dataset_dir / ("frame_" + std::to_string(i) + "_color_lr.raw");
        std::filesystem::path lr_depth_path = dataset_dir / ("frame_" + std::to_string(i) + "_depth_lr.raw");
        std::filesystem::path lr_motion_path = dataset_dir / ("frame_" + std::to_string(i) + "_motion_lr.raw");

        device_context.BeginFrame(0);
        SaveTextureToFile(device, queue, cmd_list, scene_renderer.color(), lr_color_path);
        device_context.ExecuteCommandList();
        device_context.Flush();

        device_context.BeginFrame(0);
        SaveTextureToFile(device, queue, cmd_list, scene_renderer.depth(), lr_depth_path);
        device_context.ExecuteCommandList();
        device_context.Flush();

        device_context.BeginFrame(0);
        SaveTextureToFile(device, queue, cmd_list, scene_renderer.motion(), lr_motion_path);
        device_context.ExecuteCommandList();
        device_context.Flush();

        std::cout << "Generated Frame Pair " << i + 1 << "/" << frames_to_generate << std::endl;
    }

    std::cout << "[Chimera Dataset Gen] Dataset generation completed successfully under ./dataset/" << std::endl;
    return 0;
}
