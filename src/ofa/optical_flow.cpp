#include "ofa/optical_flow.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <d3dcompiler.h>

#include "common/log.h"
#include "common/time.h"
#include "platform/win32/process_utils.h"

namespace chimera::ofa {

namespace {

using chimera::common::ErrorCode;
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

[[nodiscard]] Result<Microsoft::WRL::ComPtr<ID3DBlob>> CompileComputeShader(
    const std::filesystem::path& path,
    const char* entry_point) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> shader_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
    const auto hr = D3DCompileFromFile(
        path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry_point, "cs_5_1", flags, 0U,
        shader_blob.GetAddressOf(), error_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream stream;
        stream << "Failed to compile compute shader " << path.string() << " (" << entry_point << ")";
        if (error_blob) {
            stream << ": " << static_cast<const char*>(error_blob->GetBufferPointer());
        }
        return Status::Error(ErrorCode::kDeviceError, stream.str());
    }
    return shader_blob;
}

void UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    cl->ResourceBarrier(1U, &barrier);
}

void TransitionResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1U, &barrier);
}

Result<void> CreateConstantBuffer(ID3D12Device* device, UINT size,
    Microsoft::WRL::ComPtr<ID3D12Resource>& buffer, void** mapped) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (std::max)(static_cast<UINT64>(size), static_cast<UINT64>(65536U));
    desc.Height = 1U;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.SampleDesc.Count = 1U;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(buffer.ReleaseAndGetAddressOf())), "CreateCB(flow)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(buffer->Map(0U, nullptr, mapped), "MapCB(flow)"));
    return {};
}

Result<void> CreateUAVTexture(ID3D12Device* device, UINT width, UINT height,
    DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D12Resource>& texture) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.Format = format;
    desc.SampleDesc.Count = 1U;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())), "CreateUAV(flow)"));
    return {};
}

// Helper to create a root signature for a compute shader with 1 CBV + SRVs + UAVs
Result<void> CreateComputeRootSignature(ID3D12Device* device,
    UINT num_srvs, UINT num_uavs,
    Microsoft::WRL::ComPtr<ID3D12RootSignature>& root_sig) {

    // Root params: [0] = CBV, [1] = SRV table, [2] = UAV table
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0U;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = num_srvs;
    srv_range.BaseShaderRegister = 0U;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1U;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uav_range{};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = num_uavs;
    uav_range.BaseShaderRegister = 0U;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1U;
    params[2].DescriptorTable.pDescriptorRanges = &uav_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3U;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1U;
    desc.pStaticSamplers = &sampler;

    Microsoft::WRL::ComPtr<ID3DBlob> sig_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> err_blob{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, sig_blob.GetAddressOf(), err_blob.GetAddressOf()),
        "SerializeRS(flow)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateRootSignature(
        0U, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        IID_PPV_ARGS(root_sig.ReleaseAndGetAddressOf())), "CreateRS(flow)"));
    return {};
}

}  // namespace

Result<void> OpticalFlowPipeline::Initialize(ID3D12Device* device) {
    CHIMERA_RETURN_IF_ERROR(CreateComputePipelines(device));

    // Create constant buffers
    CHIMERA_RETURN_IF_ERROR(CreateConstantBuffer(device, sizeof(PrefilterConstants),
        cb_prefilter_, reinterpret_cast<void**>(&mapped_prefilter_)));
    CHIMERA_RETURN_IF_ERROR(CreateConstantBuffer(device, sizeof(EstimateConstants),
        cb_estimate_, reinterpret_cast<void**>(&mapped_estimate_)));
    CHIMERA_RETURN_IF_ERROR(CreateConstantBuffer(device, sizeof(RefineConstants),
        cb_refine_, reinterpret_cast<void**>(&mapped_refine_)));
    CHIMERA_RETURN_IF_ERROR(CreateConstantBuffer(device, sizeof(MedianConstants),
        cb_median_, reinterpret_cast<void**>(&mapped_median_)));

    // Create large descriptor heap for all flow operations
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 64U;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateDescriptorHeap(
        &heap_desc, IID_PPV_ARGS(srv_uav_heap_.ReleaseAndGetAddressOf())), "CreateHeap(flow)"));
    descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    common::LogInfo("ofa", "OpticalFlowPipeline initialized.");
    return {};
}

Result<void> OpticalFlowPipeline::CreateComputePipelines(ID3D12Device* device) {
    const auto shaders_dir_res = chimera::platform::win32::GetShadersDirectory();
    if (!shaders_dir_res.ok()) {
        return shaders_dir_res.status();
    }
    const auto shader_root = shaders_dir_res.value() / "fg";

    // Prefilter: 1 SRV (input color / parent luma), 1 UAV (output luma)
    CHIMERA_RETURN_IF_ERROR(CreateComputeRootSignature(device, 1U, 1U, prefilter_root_sig_));
    auto prefilter_blob = CompileComputeShader(shader_root / "flow_prefilter.hlsl", "CSMain");
    if (!prefilter_blob.ok()) return prefilter_blob.status();
    D3D12_COMPUTE_PIPELINE_STATE_DESC prefilter_desc{};
    prefilter_desc.pRootSignature = prefilter_root_sig_.Get();
    prefilter_desc.CS = {prefilter_blob.value()->GetBufferPointer(), prefilter_blob.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(
        &prefilter_desc, IID_PPV_ARGS(prefilter_pso_.ReleaseAndGetAddressOf())), "CreatePSO(prefilter)"));

    // Estimate: 3 SRVs (current luma, prev luma, coarser flow), 1 UAV (output flow)
    CHIMERA_RETURN_IF_ERROR(CreateComputeRootSignature(device, 3U, 1U, estimate_root_sig_));
    auto estimate_blob = CompileComputeShader(shader_root / "flow_estimate.hlsl", "CSMain");
    if (!estimate_blob.ok()) return estimate_blob.status();
    D3D12_COMPUTE_PIPELINE_STATE_DESC estimate_desc{};
    estimate_desc.pRootSignature = estimate_root_sig_.Get();
    estimate_desc.CS = {estimate_blob.value()->GetBufferPointer(), estimate_blob.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(
        &estimate_desc, IID_PPV_ARGS(estimate_pso_.ReleaseAndGetAddressOf())), "CreatePSO(estimate)"));

    // Refine: 2 SRVs (forward flow, backward flow), 2 UAVs (refined flow, confidence)
    CHIMERA_RETURN_IF_ERROR(CreateComputeRootSignature(device, 2U, 2U, refine_root_sig_));
    auto refine_blob = CompileComputeShader(shader_root / "flow_refine.hlsl", "CSMain");
    if (!refine_blob.ok()) return refine_blob.status();
    D3D12_COMPUTE_PIPELINE_STATE_DESC refine_desc{};
    refine_desc.pRootSignature = refine_root_sig_.Get();
    refine_desc.CS = {refine_blob.value()->GetBufferPointer(), refine_blob.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(
        &refine_desc, IID_PPV_ARGS(refine_pso_.ReleaseAndGetAddressOf())), "CreatePSO(refine)"));

    // Median: 2 SRVs (flow, confidence), 1 UAV (filtered flow)
    CHIMERA_RETURN_IF_ERROR(CreateComputeRootSignature(device, 2U, 1U, median_root_sig_));
    auto median_blob = CompileComputeShader(shader_root / "flow_median.hlsl", "CSMain");
    if (!median_blob.ok()) return median_blob.status();
    D3D12_COMPUTE_PIPELINE_STATE_DESC median_desc{};
    median_desc.pRootSignature = median_root_sig_.Get();
    median_desc.CS = {median_blob.value()->GetBufferPointer(), median_blob.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(
        &median_desc, IID_PPV_ARGS(median_pso_.ReleaseAndGetAddressOf())), "CreatePSO(median)"));

    return {};
}

Result<void> OpticalFlowPipeline::EnsureResources(ID3D12Device* device,
    const std::uint32_t width, const std::uint32_t height) {
    if (width == width_ && height == height_ && forward_flow_) {
        return {};
    }
    width_ = width;
    height_ = height;
    has_previous_frame_ = false;

    // Build pyramid dimensions
    for (std::uint32_t level = 0U; level < kPyramidLevels; ++level) {
        pyramid_widths_[level] = (std::max)(1U, width >> level);
        pyramid_heights_[level] = (std::max)(1U, height >> level);
    }

    // Create pyramid textures (R16F for luminance)
    for (std::uint32_t level = 0U; level < kPyramidLevels; ++level) {
        CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device,
            pyramid_widths_[level], pyramid_heights_[level],
            DXGI_FORMAT_R16_FLOAT, current_pyramid_[level]));
        CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device,
            pyramid_widths_[level], pyramid_heights_[level],
            DXGI_FORMAT_R16_FLOAT, previous_pyramid_[level]));
    }

    // Create flow textures per level (RG16F)
    for (std::uint32_t level = 0U; level < kPyramidLevels; ++level) {
        CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device,
            pyramid_widths_[level], pyramid_heights_[level],
            DXGI_FORMAT_R16G16_FLOAT, forward_flow_pyramid_[level]));
        CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device,
            pyramid_widths_[level], pyramid_heights_[level],
            DXGI_FORMAT_R16G16_FLOAT, backward_flow_pyramid_[level]));
    }

    // Final outputs at display resolution
    CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device, width, height,
        DXGI_FORMAT_R16G16_FLOAT, forward_flow_));
    CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device, width, height,
        DXGI_FORMAT_R16G16_FLOAT, backward_flow_));
    CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device, width, height,
        DXGI_FORMAT_R8_UNORM, confidence_));
    CHIMERA_RETURN_IF_ERROR(CreateUAVTexture(device, width, height,
        DXGI_FORMAT_R16G16_FLOAT, temp_flow_));

    return {};
}

Result<FlowOutputs> OpticalFlowPipeline::Execute(
    ID3D12GraphicsCommandList* command_list,
    const FlowInputs& inputs) {

    if (!inputs.current_color || !inputs.previous_color) {
        return Status::Error(ErrorCode::kInvalidArgument, "OpticalFlowPipeline: null input.");
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(
        command_list->GetDevice(IID_PPV_ARGS(device.GetAddressOf())), "GetDevice(flow)"));
    CHIMERA_RETURN_IF_ERROR(EnsureResources(device.Get(), inputs.width, inputs.height));

    if (inputs.reset) {
        has_previous_frame_ = false;
    }

    common::Stopwatch timer{};

    ID3D12DescriptorHeap* heaps[] = {srv_uav_heap_.Get()};
    command_list->SetDescriptorHeaps(1U, heaps);

    auto cpu_start = srv_uav_heap_->GetCPUDescriptorHandleForHeapStart();
    auto gpu_start = srv_uav_heap_->GetGPUDescriptorHandleForHeapStart();

    auto cpu_handle = [&](UINT offset) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = cpu_start;
        h.ptr += static_cast<SIZE_T>(offset) * static_cast<SIZE_T>(descriptor_size_);
        return h;
    };
    auto gpu_handle = [&](UINT offset) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = gpu_start;
        h.ptr += static_cast<UINT64>(offset) * static_cast<UINT64>(descriptor_size_);
        return h;
    };

    // ── Build luminance pyramid for current frame ───────────────────────
    // Level 0: RGB → Luminance
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = inputs.color_format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1U;
        device->CreateShaderResourceView(inputs.current_color, &srv_desc, cpu_handle(0));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = DXGI_FORMAT_R16_FLOAT;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(current_pyramid_[0].Get(), nullptr, &uav_desc, cpu_handle(1));

        mapped_prefilter_->src_size[0] = inputs.width;
        mapped_prefilter_->src_size[1] = inputs.height;
        mapped_prefilter_->dst_size[0] = pyramid_widths_[0];
        mapped_prefilter_->dst_size[1] = pyramid_heights_[0];
        mapped_prefilter_->src_texel_size[0] = 1.0F / static_cast<float>(inputs.width);
        mapped_prefilter_->src_texel_size[1] = 1.0F / static_cast<float>(inputs.height);
        mapped_prefilter_->dst_texel_size[0] = 1.0F / static_cast<float>(pyramid_widths_[0]);
        mapped_prefilter_->dst_texel_size[1] = 1.0F / static_cast<float>(pyramid_heights_[0]);
        mapped_prefilter_->pyramid_level = 0U;

        command_list->SetComputeRootSignature(prefilter_root_sig_.Get());
        command_list->SetPipelineState(prefilter_pso_.Get());
        command_list->SetComputeRootConstantBufferView(0U, cb_prefilter_->GetGPUVirtualAddress());
        command_list->SetComputeRootDescriptorTable(1U, gpu_handle(0));
        command_list->SetComputeRootDescriptorTable(2U, gpu_handle(1));
        command_list->Dispatch(
            (pyramid_widths_[0] + 7U) / 8U,
            (pyramid_heights_[0] + 7U) / 8U, 1U);
        UAVBarrier(command_list, current_pyramid_[0].Get());
    }

    // Build remaining pyramid levels from previous
    for (std::uint32_t level = 1U; level < kPyramidLevels; ++level) {
        // Transition parent to SRV
        TransitionResource(command_list, current_pyramid_[level - 1U].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC parent_srv{};
        parent_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        parent_srv.Format = DXGI_FORMAT_R16_FLOAT;
        parent_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        parent_srv.Texture2D.MipLevels = 1U;
        // Reuse slots 0 and 1 for simplicity (re-bind each level)
        device->CreateShaderResourceView(current_pyramid_[level - 1U].Get(), &parent_srv, cpu_handle(0));

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
        uav_desc.Format = DXGI_FORMAT_R16_FLOAT;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(current_pyramid_[level].Get(), nullptr, &uav_desc, cpu_handle(1));

        mapped_prefilter_->src_size[0] = pyramid_widths_[level - 1U];
        mapped_prefilter_->src_size[1] = pyramid_heights_[level - 1U];
        mapped_prefilter_->dst_size[0] = pyramid_widths_[level];
        mapped_prefilter_->dst_size[1] = pyramid_heights_[level];
        mapped_prefilter_->src_texel_size[0] = 1.0F / static_cast<float>(pyramid_widths_[level - 1U]);
        mapped_prefilter_->src_texel_size[1] = 1.0F / static_cast<float>(pyramid_heights_[level - 1U]);
        mapped_prefilter_->dst_texel_size[0] = 1.0F / static_cast<float>(pyramid_widths_[level]);
        mapped_prefilter_->dst_texel_size[1] = 1.0F / static_cast<float>(pyramid_heights_[level]);
        mapped_prefilter_->pyramid_level = level;

        command_list->SetComputeRootSignature(prefilter_root_sig_.Get());
        command_list->SetPipelineState(prefilter_pso_.Get());
        command_list->SetComputeRootConstantBufferView(0U, cb_prefilter_->GetGPUVirtualAddress());
        command_list->SetComputeRootDescriptorTable(1U, gpu_handle(0));
        command_list->SetComputeRootDescriptorTable(2U, gpu_handle(1));
        command_list->Dispatch(
            (pyramid_widths_[level] + 7U) / 8U,
            (pyramid_heights_[level] + 7U) / 8U, 1U);
        UAVBarrier(command_list, current_pyramid_[level].Get());

        // Transition parent back to UAV for next frame
        TransitionResource(command_list, current_pyramid_[level - 1U].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // If we don't have a previous frame, just swap pyramids and return zero flow
    if (!has_previous_frame_) {
        std::swap(current_pyramid_, previous_pyramid_);
        has_previous_frame_ = true;

        FlowOutputs outputs{};
        outputs.forward_flow = forward_flow_.Get();
        outputs.backward_flow = backward_flow_.Get();
        outputs.confidence = confidence_.Get();
        outputs.compute_ms = static_cast<float>(timer.ElapsedMilliseconds());
        return outputs;
    }

    // ── Coarse-to-fine flow estimation ──────────────────────────────────
    // Process from coarsest to finest pyramid level
    for (int level = static_cast<int>(kPyramidLevels) - 1; level >= 0; --level) {
        const auto lv = static_cast<std::uint32_t>(level);
        const auto w = pyramid_widths_[lv];
        const auto h = pyramid_heights_[lv];

        // Transition pyramid textures to SRV
        TransitionResource(command_list, current_pyramid_[lv].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, previous_pyramid_[lv].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Forward flow: current → previous
        for (std::uint32_t iter = 0U; iter < kIterationsPerLevel; ++iter) {
            // Bind SRVs: slot 2=current, slot 3=previous, slot 4=coarser flow
            D3D12_SHADER_RESOURCE_VIEW_DESC luma_srv{};
            luma_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            luma_srv.Format = DXGI_FORMAT_R16_FLOAT;
            luma_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            luma_srv.Texture2D.MipLevels = 1U;

            device->CreateShaderResourceView(current_pyramid_[lv].Get(), &luma_srv, cpu_handle(2));
            device->CreateShaderResourceView(previous_pyramid_[lv].Get(), &luma_srv, cpu_handle(3));

            // Coarser flow SRV
            D3D12_SHADER_RESOURCE_VIEW_DESC flow_srv{};
            flow_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            flow_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            flow_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            flow_srv.Texture2D.MipLevels = 1U;

            if (iter == 0 && lv < kPyramidLevels - 1U) {
                // Use flow from coarser level (already in SRV-compatible state)
                TransitionResource(command_list, forward_flow_pyramid_[lv + 1U].Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                device->CreateShaderResourceView(forward_flow_pyramid_[lv + 1U].Get(), &flow_srv, cpu_handle(4));
            } else {
                // Self-reference for iterative refinement (or zero at coarsest first iter)
                device->CreateShaderResourceView(forward_flow_pyramid_[lv].Get(), &flow_srv, cpu_handle(4));
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC flow_uav{};
            flow_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
            flow_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(forward_flow_pyramid_[lv].Get(), nullptr, &flow_uav, cpu_handle(5));

            mapped_estimate_->size[0] = w;
            mapped_estimate_->size[1] = h;
            mapped_estimate_->texel_size[0] = 1.0F / static_cast<float>(w);
            mapped_estimate_->texel_size[1] = 1.0F / static_cast<float>(h);
            mapped_estimate_->iteration_index = iter;
            mapped_estimate_->pyramid_level = lv;

            command_list->SetComputeRootSignature(estimate_root_sig_.Get());
            command_list->SetPipelineState(estimate_pso_.Get());
            command_list->SetComputeRootConstantBufferView(0U, cb_estimate_->GetGPUVirtualAddress());
            command_list->SetComputeRootDescriptorTable(1U, gpu_handle(2));
            command_list->SetComputeRootDescriptorTable(2U, gpu_handle(5));
            command_list->Dispatch((w + 7U) / 8U, (h + 7U) / 8U, 1U);
            UAVBarrier(command_list, forward_flow_pyramid_[lv].Get());

            if (iter == 0 && lv < kPyramidLevels - 1U) {
                TransitionResource(command_list, forward_flow_pyramid_[lv + 1U].Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }

        // Backward flow: previous → current (swap inputs)
        for (std::uint32_t iter = 0U; iter < kIterationsPerLevel; ++iter) {
            D3D12_SHADER_RESOURCE_VIEW_DESC luma_srv{};
            luma_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            luma_srv.Format = DXGI_FORMAT_R16_FLOAT;
            luma_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            luma_srv.Texture2D.MipLevels = 1U;

            // Swap: previous as "current", current as "previous"
            device->CreateShaderResourceView(previous_pyramid_[lv].Get(), &luma_srv, cpu_handle(2));
            device->CreateShaderResourceView(current_pyramid_[lv].Get(), &luma_srv, cpu_handle(3));

            D3D12_SHADER_RESOURCE_VIEW_DESC flow_srv{};
            flow_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            flow_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            flow_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            flow_srv.Texture2D.MipLevels = 1U;

            if (iter == 0 && lv < kPyramidLevels - 1U) {
                TransitionResource(command_list, backward_flow_pyramid_[lv + 1U].Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                device->CreateShaderResourceView(backward_flow_pyramid_[lv + 1U].Get(), &flow_srv, cpu_handle(4));
            } else {
                device->CreateShaderResourceView(backward_flow_pyramid_[lv].Get(), &flow_srv, cpu_handle(4));
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC flow_uav{};
            flow_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
            flow_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(backward_flow_pyramid_[lv].Get(), nullptr, &flow_uav, cpu_handle(5));

            mapped_estimate_->size[0] = w;
            mapped_estimate_->size[1] = h;
            mapped_estimate_->texel_size[0] = 1.0F / static_cast<float>(w);
            mapped_estimate_->texel_size[1] = 1.0F / static_cast<float>(h);
            mapped_estimate_->iteration_index = iter;
            mapped_estimate_->pyramid_level = lv;

            command_list->SetComputeRootSignature(estimate_root_sig_.Get());
            command_list->SetPipelineState(estimate_pso_.Get());
            command_list->SetComputeRootConstantBufferView(0U, cb_estimate_->GetGPUVirtualAddress());
            command_list->SetComputeRootDescriptorTable(1U, gpu_handle(2));
            command_list->SetComputeRootDescriptorTable(2U, gpu_handle(5));
            command_list->Dispatch((w + 7U) / 8U, (h + 7U) / 8U, 1U);
            UAVBarrier(command_list, backward_flow_pyramid_[lv].Get());

            if (iter == 0 && lv < kPyramidLevels - 1U) {
                TransitionResource(command_list, backward_flow_pyramid_[lv + 1U].Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
        }

        // Transition pyramid textures back to UAV
        TransitionResource(command_list, current_pyramid_[lv].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(command_list, previous_pyramid_[lv].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ── Copy finest level to output + forward-backward consistency ──────
    // Refine pass: produces final forward_flow_ and confidence_
    {
        TransitionResource(command_list, forward_flow_pyramid_[0].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, backward_flow_pyramid_[0].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC flow_srv{};
        flow_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        flow_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        flow_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        flow_srv.Texture2D.MipLevels = 1U;
        device->CreateShaderResourceView(forward_flow_pyramid_[0].Get(), &flow_srv, cpu_handle(6));
        device->CreateShaderResourceView(backward_flow_pyramid_[0].Get(), &flow_srv, cpu_handle(7));

        D3D12_UNORDERED_ACCESS_VIEW_DESC flow_uav{};
        flow_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        flow_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(forward_flow_.Get(), nullptr, &flow_uav, cpu_handle(8));

        D3D12_UNORDERED_ACCESS_VIEW_DESC conf_uav{};
        conf_uav.Format = DXGI_FORMAT_R8_UNORM;
        conf_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(confidence_.Get(), nullptr, &conf_uav, cpu_handle(9));

        mapped_refine_->size[0] = width_;
        mapped_refine_->size[1] = height_;
        mapped_refine_->texel_size[0] = 1.0F / static_cast<float>(width_);
        mapped_refine_->texel_size[1] = 1.0F / static_cast<float>(height_);
        mapped_refine_->consistency_threshold = 2.0F;

        command_list->SetComputeRootSignature(refine_root_sig_.Get());
        command_list->SetPipelineState(refine_pso_.Get());
        command_list->SetComputeRootConstantBufferView(0U, cb_refine_->GetGPUVirtualAddress());
        command_list->SetComputeRootDescriptorTable(1U, gpu_handle(6));
        command_list->SetComputeRootDescriptorTable(2U, gpu_handle(8));
        command_list->Dispatch((width_ + 7U) / 8U, (height_ + 7U) / 8U, 1U);
        UAVBarrier(command_list, forward_flow_.Get());
        UAVBarrier(command_list, confidence_.Get());

        TransitionResource(command_list, forward_flow_pyramid_[0].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(command_list, backward_flow_pyramid_[0].Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // ── Median filter on final forward flow ─────────────────────────────
    {
        TransitionResource(command_list, forward_flow_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, confidence_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC flow_srv{};
        flow_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        flow_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        flow_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        flow_srv.Texture2D.MipLevels = 1U;
        device->CreateShaderResourceView(forward_flow_.Get(), &flow_srv, cpu_handle(10));

        D3D12_SHADER_RESOURCE_VIEW_DESC conf_srv{};
        conf_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        conf_srv.Format = DXGI_FORMAT_R8_UNORM;
        conf_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        conf_srv.Texture2D.MipLevels = 1U;
        device->CreateShaderResourceView(confidence_.Get(), &conf_srv, cpu_handle(11));

        D3D12_UNORDERED_ACCESS_VIEW_DESC temp_uav{};
        temp_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        temp_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(temp_flow_.Get(), nullptr, &temp_uav, cpu_handle(12));

        mapped_median_->size[0] = width_;
        mapped_median_->size[1] = height_;
        mapped_median_->texel_size[0] = 1.0F / static_cast<float>(width_);
        mapped_median_->texel_size[1] = 1.0F / static_cast<float>(height_);

        command_list->SetComputeRootSignature(median_root_sig_.Get());
        command_list->SetPipelineState(median_pso_.Get());
        command_list->SetComputeRootConstantBufferView(0U, cb_median_->GetGPUVirtualAddress());
        command_list->SetComputeRootDescriptorTable(1U, gpu_handle(10));
        command_list->SetComputeRootDescriptorTable(2U, gpu_handle(12));
        command_list->Dispatch((width_ + 7U) / 8U, (height_ + 7U) / 8U, 1U);
        UAVBarrier(command_list, temp_flow_.Get());

        // Copy filtered flow back to forward_flow_ (swap pointers)
        TransitionResource(command_list, forward_flow_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(command_list, confidence_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::swap(forward_flow_, temp_flow_);
    }

    // Also copy backward flow from finest level to output
    {
        TransitionResource(command_list, backward_flow_pyramid_[0].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(command_list, backward_flow_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        command_list->CopyResource(backward_flow_.Get(), backward_flow_pyramid_[0].Get());
        TransitionResource(command_list, backward_flow_pyramid_[0].Get(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(command_list, backward_flow_.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Swap pyramid for next frame
    std::swap(current_pyramid_, previous_pyramid_);

    const auto compute_ms = static_cast<float>(timer.ElapsedMilliseconds());

    FlowOutputs outputs{};
    outputs.forward_flow = forward_flow_.Get();
    outputs.backward_flow = backward_flow_.Get();
    outputs.confidence = confidence_.Get();
    outputs.compute_ms = compute_ms;
    return outputs;
}

void OpticalFlowPipeline::Reset() noexcept {
    has_previous_frame_ = false;
}

}  // namespace chimera::ofa
