#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <filesystem>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"
#include "d3d12/device_context.h"
#include "sr/analytic_tsr.h"
#include "ml/neural_sr.h"

namespace {

using Microsoft::WRL::ComPtr;
using chimera::common::Result;

float HalfToFloat(unsigned short h) {
    unsigned int sign = (h >> 15) & 0x00000001;
    unsigned int exponent = (h >> 10) & 0x0000001f;
    unsigned int significand = h & 0x000003ff;

    if (exponent == 0) {
        if (significand == 0) {
            return (sign == 1) ? -0.0f : 0.0f;
        } else {
            // Denormalized
            return (sign == 1 ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(significand) / 1024.0f, -14);
        }
    } else if (exponent == 31) {
        return (significand == 0) ? (sign == 1 ? -INFINITY : INFINITY) : NAN;
    }

    return (sign == 1 ? -1.0f : 1.0f) * std::ldexp(1.0f + static_cast<float>(significand) / 1024.0f, static_cast<int>(exponent) - 15);
}

std::vector<float> LoadRawTextureToFloat(const std::filesystem::path& path, UINT width, UINT height, int channels) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    UINT64 total_pixels = static_cast<UINT64>(width) * height;
    std::vector<float> float_data(total_pixels * channels);

    if (channels == 4) { // R16G16B16A16_FLOAT
        std::vector<unsigned short> half_data(total_pixels * 4);
        file.read(reinterpret_cast<char*>(half_data.data()), half_data.size() * sizeof(unsigned short));
        for (size_t i = 0; i < float_data.size(); ++i) {
            float_data[i] = HalfToFloat(half_data[i]);
        }
    } else if (channels == 2) { // R16G16_FLOAT
        std::vector<unsigned short> half_data(total_pixels * 2);
        file.read(reinterpret_cast<char*>(half_data.data()), half_data.size() * sizeof(unsigned short));
        for (size_t i = 0; i < float_data.size(); ++i) {
            float_data[i] = HalfToFloat(half_data[i]);
        }
    } else if (channels == 1) { // R16_FLOAT
        std::vector<unsigned short> half_data(total_pixels * 1);
        file.read(reinterpret_cast<char*>(half_data.data()), half_data.size() * sizeof(unsigned short));
        for (size_t i = 0; i < float_data.size(); ++i) {
            float_data[i] = HalfToFloat(half_data[i]);
        }
    }

    return float_data;
}

ComPtr<ID3D12Resource> CreateTextureFromRawData(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmd_list,
    ComPtr<ID3D12Resource>& upload_buffer,
    const std::vector<float>& float_data,
    UINT width,
    UINT height,
    DXGI_FORMAT format) {

    UINT64 bpp = 16;
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) bpp = 8;
    else if (format == DXGI_FORMAT_R16G16_FLOAT) bpp = 4;
    else if (format == DXGI_FORMAT_R16_FLOAT) bpp = 2;

    UINT64 row_bytes = width * bpp;

    // Convert floats back to half
    auto float_to_half = [](float f) -> unsigned short {
        union { float fval; unsigned int ival; } u;
        u.fval = f;
        unsigned int sign = (u.ival >> 16) & 0x8000;
        int exponent = ((u.ival >> 23) & 0xff) - 127;
        unsigned int significand = u.ival & 0x7fffff;

        if (exponent < -24) {
            return static_cast<unsigned short>(sign);
        } else if (exponent < -14) {
            // Denormalized
            unsigned int shift = static_cast<unsigned int>(-14 - exponent);
            significand = (significand | 0x800000) >> shift;
            return static_cast<unsigned short>(sign | (significand >> 13));
        } else if (exponent > 15) {
            // Infinity
            return static_cast<unsigned short>(sign | 0x7c00);
        }

        return static_cast<unsigned short>(sign | ((exponent + 15) << 10) | (significand >> 13));
    };

    std::vector<unsigned short> half_data(float_data.size());
    for (size_t i = 0; i < float_data.size(); ++i) {
        half_data[i] = float_to_half(float_data[i]);
    }

    // Create default texture
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
    device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(texture.GetAddressOf()));

    // Create upload buffer
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

    device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(upload_buffer.GetAddressOf()));

    void* data_ptr = nullptr;
    upload_buffer->Map(0, nullptr, &data_ptr);
    char* dst_ptr = reinterpret_cast<char*>(data_ptr);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(dst_ptr + y * footprint.Footprint.RowPitch, reinterpret_cast<char*>(half_data.data()) + y * row_bytes, row_bytes);
    }
    upload_buffer->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = texture.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload_buffer.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = footprint;

    cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(1, &barrier);

    return texture;
}

std::vector<float> DownloadTextureToFloat(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    ID3D12GraphicsCommandList* cmd_list,
    ID3D12Resource* texture,
    UINT width,
    UINT height) {

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
    device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback_buffer.GetAddressOf()));

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

    cmd_list->Close();
    ID3D12CommandList* lists[] = {cmd_list};
    queue->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence{};
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    CloseHandle(fence_event);

    void* data_ptr = nullptr;
    readback_buffer->Map(0, nullptr, &data_ptr);

    // Read to float array, stripping row pitch padding
    std::vector<float> float_data(static_cast<size_t>(width) * height * 4);
    unsigned short* src_ptr = reinterpret_cast<unsigned short*>(data_ptr);
    UINT64 row_pitch_shorts = footprint.Footprint.RowPitch / 2;

    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) {
                float_data[(static_cast<size_t>(y) * width + x) * 4 + c] = HalfToFloat(src_ptr[y * row_pitch_shorts + x * 4 + c]);
            }
        }
    }

    readback_buffer->Unmap(0, nullptr);
    return float_data;
}

double ComputePSNR(const std::vector<float>& img1, const std::vector<float>& img2, int width, int height) {
    double mse = 0.0;
    size_t count = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < count; ++i) {
        // Compute error on RGB channels only (ignore Alpha)
        double r_err = img1[i * 4 + 0] - img2[i * 4 + 0];
        double g_err = img1[i * 4 + 1] - img2[i * 4 + 1];
        double b_err = img1[i * 4 + 2] - img2[i * 4 + 2];
        mse += (r_err * r_err + g_err * g_err + b_err * b_err);
    }
    mse /= (3.0 * count);

    if (mse < 1e-10) return 99.0;
    return 10.0 * std::log10(1.0 / mse);
}

double ComputeSSIM(const std::vector<float>& img1, const std::vector<float>& img2, int width, int height) {
    // Basic structural similarity index computation
    double mean1 = 0.0, mean2 = 0.0;
    size_t count = static_cast<size_t>(width) * height;

    for (size_t i = 0; i < count; ++i) {
        // Use luminance
        float y1 = 0.25f * img1[i * 4 + 0] + 0.50f * img1[i * 4 + 1] + 0.25f * img1[i * 4 + 2];
        float y2 = 0.25f * img2[i * 4 + 0] + 0.50f * img2[i * 4 + 1] + 0.25f * img2[i * 4 + 2];
        mean1 += y1;
        mean2 += y2;
    }
    mean1 /= count;
    mean2 /= count;

    double var1 = 0.0, var2 = 0.0, covar = 0.0;
    for (size_t i = 0; i < count; ++i) {
        float y1 = 0.25f * img1[i * 4 + 0] + 0.50f * img1[i * 4 + 1] + 0.25f * img1[i * 4 + 2];
        float y2 = 0.25f * img2[i * 4 + 0] + 0.50f * img2[i * 4 + 1] + 0.25f * img2[i * 4 + 2];
        double d1 = y1 - mean1;
        double d2 = y2 - mean2;
        var1 += d1 * d1;
        var2 += d2 * d2;
        covar += d1 * d2;
    }
    var1 /= (count - 1);
    var2 /= (count - 1);
    covar /= (count - 1);

    double c1 = 6.5025e-5; // (0.01 * 1.0)^2
    double c2 = 5.85225e-4; // (0.03 * 1.0)^2

    double num = (2.0 * mean1 * mean2 + c1) * (2.0 * covar + c2);
    double den = (mean1 * mean1 + mean2 * mean2 + c1) * (var1 + var2 + c2);
    return num / den;
}

} // namespace

TEST_CASE("Upscaler Image Quality Verification", "[ml][sr][image_quality]") {
    std::filesystem::path dataset_dir = "dataset";
    
    // Skip if dataset gen wasn't run
    if (!std::filesystem::exists(dataset_dir / "frame_0_color_gt.raw")) {
        std::cout << "[Test Warning] Dataset files not found. Skipping Image Quality asserts." << std::endl;
        return;
    }

    const std::uint32_t lr_width = 800U;
    const std::uint32_t lr_height = 450U;
    const std::uint32_t hr_width = 1600U;
    const std::uint32_t hr_height = 900U;

    std::vector<float> lr_color_data = LoadRawTextureToFloat(dataset_dir / "frame_0_color_lr.raw", lr_width, lr_height, 4);
    std::vector<float> lr_depth_data = LoadRawTextureToFloat(dataset_dir / "frame_0_depth_lr.raw", lr_width, lr_height, 1);
    std::vector<float> lr_motion_data = LoadRawTextureToFloat(dataset_dir / "frame_0_motion_lr.raw", lr_width, lr_height, 2);
    std::vector<float> gt_data = LoadRawTextureToFloat(dataset_dir / "frame_0_color_gt.raw", hr_width, hr_height, 4);

    REQUIRE(!lr_color_data.empty());
    REQUIRE(!lr_depth_data.empty());
    REQUIRE(!lr_motion_data.empty());
    REQUIRE(!gt_data.empty());

    chimera::d3d12::DeviceContext device_context{};
    REQUIRE(device_context.Initialize(false).ok());

    auto* device = device_context.device();
    auto* queue = device_context.command_queue();
    auto* cmd_list = device_context.command_list();

    // Recreate textures on GPU
    device_context.BeginFrame(0);
    ComPtr<ID3D12Resource> upload0{}, upload1{}, upload2{};
    auto lr_color = CreateTextureFromRawData(device, cmd_list, upload0, lr_color_data, lr_width, lr_height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto lr_depth = CreateTextureFromRawData(device, cmd_list, upload1, lr_depth_data, lr_width, lr_height, DXGI_FORMAT_R16_FLOAT);
    auto lr_motion = CreateTextureFromRawData(device, cmd_list, upload2, lr_motion_data, lr_width, lr_height, DXGI_FORMAT_R16G16_FLOAT);

    cmd_list->Close();
    ID3D12CommandList* lists[] = {cmd_list};
    queue->ExecuteCommandLists(1U, lists);

    ComPtr<ID3D12Fence> fence{};
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    CloseHandle(fence_event);

    // Initialize Pipelines
    chimera::sr::AnalyticTsrPipeline tsr_pipeline{};
    REQUIRE(tsr_pipeline.Initialize(device, DXGI_FORMAT_R16G16B16A16_FLOAT).ok());

    chimera::ml::NeuralSrPipeline neural_pipeline{};
    REQUIRE(neural_pipeline.Initialize(device, DXGI_FORMAT_R16G16B16A16_FLOAT).ok());

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

    std::array<float, 2> jitter = {0.0f, 0.0f};

    // Run TSR
    device_context.BeginFrame(0);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = tsr_target.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = neural_target.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd_list->ResourceBarrier(2U, barriers);

    auto tsr_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    REQUIRE(tsr_pipeline.Execute(cmd_list, chimera::sr::SrInputs{
        lr_color.Get(),
        lr_depth.Get(),
        lr_motion.Get(),
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
        true,
        0
    }, tsr_rtv).ok());

    // Run Neural
    auto neural_rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    neural_rtv.ptr += rtv_increment;
    REQUIRE(neural_pipeline.Execute(cmd_list, chimera::ml::NeuralSrInputs{
        lr_color.Get(),
        lr_depth.Get(),
        lr_motion.Get(),
        lr_width,
        lr_height,
        hr_width,
        hr_height,
        jitter,
        jitter,
        1.0f,
        true,
        0
    }, neural_rtv).ok());

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    cmd_list->ResourceBarrier(2U, barriers);

    device_context.ExecuteCommandList();
    device_context.Flush();

    // Download outputs and check metrics
    device_context.BeginFrame(0);
    std::vector<float> tsr_out = DownloadTextureToFloat(device, queue, cmd_list, tsr_target.Get(), hr_width, hr_height);
    
    device_context.BeginFrame(0);
    std::vector<float> neural_out = DownloadTextureToFloat(device, queue, cmd_list, neural_target.Get(), hr_width, hr_height);

    double tsr_psnr = ComputePSNR(tsr_out, gt_data, hr_width, hr_height);
    double neural_psnr = ComputePSNR(neural_out, gt_data, hr_width, hr_height);

    double tsr_ssim = ComputeSSIM(tsr_out, gt_data, hr_width, hr_height);
    double neural_ssim = ComputeSSIM(neural_out, gt_data, hr_width, hr_height);

    std::cout << "\n==============================================" << std::endl;
    std::cout << "          QUALITY SUITE METRICS               " << std::endl;
    std::cout << "==============================================" << std::endl;
    std::cout << "Analytic TSR PSNR: " << tsr_psnr << " dB  (SSIM: " << tsr_ssim << ")" << std::endl;
    std::cout << "Neural SR PSNR:   " << neural_psnr << " dB  (SSIM: " << neural_ssim << ")" << std::endl;
    std::cout << "==============================================\n" << std::endl;

    // Assert that quality is within professional standards (PSNR > 25dB and SSIM > 0.8)
    REQUIRE(tsr_psnr > 25.0);
    REQUIRE(neural_psnr > 25.0);
    REQUIRE(tsr_ssim > 0.80);
    REQUIRE(neural_ssim > 0.80);
}
