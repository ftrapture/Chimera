#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <numeric>

#include <Windows.h>
#include <d3d12.h>
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

    // Copy file data to upload buffer
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

} // namespace

int main() {
    std::cout << "[Chimera Benchmark] Initializing benchmark run..." << std::endl;
    std::filesystem::path dataset_dir = "dataset";

    if (!std::filesystem::exists(dataset_dir)) {
        std::cerr << "Dataset directory does not exist! Please run chimera_dataset_gen.exe first." << std::endl;
        return -1;
    }

    chimera::d3d12::DeviceContext device_context{};
    auto init_res = device_context.Initialize(false);
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

    // Load first frame from dataset for warm cache runs
    std::filesystem::path color_path = dataset_dir / "frame_0_color_lr.raw";
    std::filesystem::path depth_path = dataset_dir / "frame_0_depth_lr.raw";
    std::filesystem::path motion_path = dataset_dir / "frame_0_motion_lr.raw";

    device_context.BeginFrame(0);
    auto lr_color = CreateTextureFromRawFile(device, queue, cmd_list, color_path, lr_width, lr_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    device_context.BeginFrame(0);
    auto lr_depth = CreateTextureFromRawFile(device, queue, cmd_list, depth_path, lr_width, lr_height, DXGI_FORMAT_R16_FLOAT);
    device_context.BeginFrame(0);
    auto lr_motion = CreateTextureFromRawFile(device, queue, cmd_list, motion_path, lr_width, lr_height, DXGI_FORMAT_R16G16_FLOAT);

    if (!lr_color.ok() || !lr_depth.ok() || !lr_motion.ok()) {
        std::cerr << "Failed to upload textures for benchmark frame 0" << std::endl;
        return -1;
    }

    // Initialize Pipelines
    chimera::sr::AnalyticTsrPipeline tsr_pipeline{};
    tsr_pipeline.Initialize(device, DXGI_FORMAT_R16G16B16A16_FLOAT);

    chimera::ml::NeuralSrPipeline neural_pipeline{};
    neural_pipeline.Initialize(device, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Setup temporary upscale targets
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

    // Setup GPU Queries for microsecond-level timing
    ComPtr<ID3D12QueryHeap> query_heap{};
    D3D12_QUERY_HEAP_DESC heap_desc{};
    heap_desc.Count = 2U;
    heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(query_heap.GetAddressOf()));

    D3D12_HEAP_PROPERTIES readback_heap{};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 2U * sizeof(UINT64);
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> query_buffer{};
    device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(query_buffer.GetAddressOf()));

    UINT64 frequency = 0;
    queue->GetTimestampFrequency(&frequency);

    const int iterations = 100;
    std::vector<double> tsr_times;
    std::vector<double> neural_times;

    std::cout << "[Chimera Benchmark] Running " << iterations << " iterations for both pipelines..." << std::endl;

    std::array<float, 2> jitter = {0.0f, 0.0f};

    // --- Benchmark Analytic TSR ---
    for (int i = 0; i < iterations; ++i) {
        device_context.BeginFrame(0);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tsr_target.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_list->ResourceBarrier(1, &barrier);

        cmd_list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        auto tsr_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        tsr_pipeline.Execute(cmd_list, chimera::sr::SrInputs{
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

        cmd_list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        cmd_list->ResourceBarrier(1, &barrier);

        cmd_list->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, query_buffer.Get(), 0);

        device_context.ExecuteCommandList();
        device_context.Flush();

        // Read query data
        void* data = nullptr;
        query_buffer->Map(0, nullptr, &data);
        UINT64* timestamps = reinterpret_cast<UINT64*>(data);
        double delta_ms = static_cast<double>(timestamps[1] - timestamps[0]) * 1000.0 / frequency;
        query_buffer->Unmap(0, nullptr);

        // Discard first frame (warm-up)
        if (i > 0) {
            tsr_times.push_back(delta_ms);
        }
    }

    // --- Benchmark Neural SR ---
    for (int i = 0; i < iterations; ++i) {
        device_context.BeginFrame(0);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = neural_target.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd_list->ResourceBarrier(1, &barrier);

        cmd_list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        auto neural_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        neural_rtv.ptr += rtv_increment;
        neural_pipeline.Execute(cmd_list, chimera::ml::NeuralSrInputs{
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

        cmd_list->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        cmd_list->ResourceBarrier(1, &barrier);

        cmd_list->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, query_buffer.Get(), 0);

        device_context.ExecuteCommandList();
        device_context.Flush();

        // Read query data
        void* data = nullptr;
        query_buffer->Map(0, nullptr, &data);
        UINT64* timestamps = reinterpret_cast<UINT64*>(data);
        double delta_ms = static_cast<double>(timestamps[1] - timestamps[0]) * 1000.0 / frequency;
        query_buffer->Unmap(0, nullptr);

        // Discard first frame (warm-up)
        if (i > 0) {
            neural_times.push_back(delta_ms);
        }
    }

    double tsr_avg = std::accumulate(tsr_times.begin(), tsr_times.end(), 0.0) / tsr_times.size();
    double neural_avg = std::accumulate(neural_times.begin(), neural_times.end(), 0.0) / neural_times.size();

    // Display Results
    std::cout << "\n==============================================" << std::endl;
    std::cout << "          PROJECT CHIMERA BENCHMARK           " << std::endl;
    std::cout << "==============================================" << std::endl;
    std::cout << "Upscaler Input Resolution:  " << lr_width << "x" << lr_height << std::endl;
    std::cout << "Upscaler Output Resolution: " << hr_width << "x" << hr_height << std::endl;
    std::cout << "Iterations Profiled:        " << tsr_times.size() << std::endl;
    std::cout << "----------------------------------------------" << std::endl;
    std::cout << "Analytic TSR (Average GPU): " << tsr_avg << " ms" << std::endl;
    std::cout << "Neural SR (Average GPU):   " << neural_avg << " ms" << std::endl;
    std::cout << "==============================================\n" << std::endl;

    // Save report to benchmark_report.json
    std::ofstream report_file("benchmark_report.json");
    if (report_file) {
        report_file << "{\n";
        report_file << "  \"input_resolution\": \"" << lr_width << "x" << lr_height << "\",\n";
        report_file << "  \"output_resolution\": \"" << hr_width << "x" << hr_height << "\",\n";
        report_file << "  \"analytic_tsr\": {\n";
        report_file << "    \"avg_gpu_ms\": " << tsr_avg << "\n";
        report_file << "  },\n";
        report_file << "  \"neural_sr\": {\n";
        report_file << "    \"avg_gpu_ms\": " << neural_avg << "\n";
        report_file << "  }\n";
        report_file << "}\n";
    }

    return 0;
}
