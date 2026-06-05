#include "ml/neural_sr.h"
#include "ml/neural_weights.h"

#include <filesystem>
#include <sstream>
#include <vector>

#include <d3dcompiler.h>

#include "common/time.h"
#include "common/log.h"
#include "platform/win32/process_utils.h"

namespace chimera::ml {

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

[[nodiscard]] Result<Microsoft::WRL::ComPtr<ID3DBlob>> CompileShaderFromFile(
    const std::filesystem::path& path,
    const char* entry_point,
    const char* shader_target) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    Microsoft::WRL::ComPtr<ID3DBlob> shader_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
    const auto hr = D3DCompileFromFile(
        path.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry_point,
        shader_target,
        flags,
        0U,
        shader_blob.GetAddressOf(),
        error_blob.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream stream;
        stream << "Failed to compile shader " << path.string() << " (" << entry_point << "/" << shader_target << ")";
        if (error_blob) {
            stream << ": " << static_cast<const char*>(error_blob->GetBufferPointer());
        }
        return Status::Error(ErrorCode::kDeviceError, stream.str());
    }

    return shader_blob;
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

D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, const UINT offset, const UINT increment) {
    handle.ptr += static_cast<SIZE_T>(offset) * static_cast<SIZE_T>(increment);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE OffsetGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle, const UINT offset, const UINT increment) {
    handle.ptr += static_cast<UINT64>(offset) * static_cast<UINT64>(increment);
    return handle;
}

} // namespace

Result<void> NeuralSrPipeline::Initialize(ID3D12Device* device, const DXGI_FORMAT display_format) {
    display_format_ = display_format;
    
    CHIMERA_RETURN_IF_ERROR(CreatePipelineObjects(device));

    descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Allocate single descriptor heap for all SRVs and UAVs:
    // 0: Color (LR) SRV
    // 1: Depth (LR) SRV
    // 2: Motion (LR) SRV
    // 3: Weights buffer SRV
    // 4: Features texture array SRV (for Pass 2)
    // 5: Previous history SRV (for Pass 2)
    // 6: Features texture array UAV (for Pass 1)
    // 7: Output backbuffer temp UAV (for Pass 2)
    // 8: Output history UAV (for Pass 2)
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 9U;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(descriptor_heap_.ReleaseAndGetAddressOf())), "CreateDescriptorHeap(Neural Descriptor Heap)"));

    // Constant Buffer Setup
    D3D12_RESOURCE_DESC cb_desc{};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Width = 65536U;
    cb_desc.Height = 1U;
    cb_desc.DepthOrArraySize = 1U;
    cb_desc.MipLevels = 1U;
    cb_desc.Format = DXGI_FORMAT_UNKNOWN;
    cb_desc.SampleDesc.Count = 1U;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES upload_props{};
    upload_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &upload_props,
        D3D12_HEAP_FLAG_NONE,
        &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(constant_buffer_.ReleaseAndGetAddressOf())),
        "CreateCommittedResource(constant buffer)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(constant_buffer_->Map(0U, nullptr, reinterpret_cast<void**>(&mapped_constants_)), "Map(Neural constants)"));

    // Weights Buffer Setup (3795 floats, StructuredBuffer)
    const std::vector<float> default_weights = GetDefaultNeuralWeights();
    D3D12_RESOURCE_DESC weights_desc{};
    weights_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    weights_desc.Width = default_weights.size() * sizeof(float);
    weights_desc.Height = 1U;
    weights_desc.DepthOrArraySize = 1U;
    weights_desc.MipLevels = 1U;
    weights_desc.Format = DXGI_FORMAT_UNKNOWN;
    weights_desc.SampleDesc.Count = 1U;
    weights_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // Create uploaded weights resource directly for simplicity in setup
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &upload_props,
        D3D12_HEAP_FLAG_NONE,
        &weights_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(weights_buffer_.ReleaseAndGetAddressOf())),
        "CreateCommittedResource(weights buffer)"));

    void* mapped_weights = nullptr;
    CHIMERA_RETURN_IF_ERROR(HrCheck(weights_buffer_->Map(0U, nullptr, &mapped_weights), "Map(Weights buffer)"));
    std::memcpy(mapped_weights, default_weights.data(), default_weights.size() * sizeof(float));
    weights_buffer_->Unmap(0U, nullptr);

    // Bind weights buffer SRV to descriptor heap slot 3
    D3D12_SHADER_RESOURCE_VIEW_DESC weights_srv_desc{};
    weights_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    weights_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    weights_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    weights_srv_desc.Buffer.FirstElement = 0U;
    weights_srv_desc.Buffer.NumElements = static_cast<UINT>(default_weights.size());
    weights_srv_desc.Buffer.StructureByteStride = sizeof(float);
    weights_srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    device->CreateShaderResourceView(weights_buffer_.Get(), &weights_srv_desc, OffsetCpuHandle(descriptor_heap_->GetCPUDescriptorHandleForHeapStart(), 3U, descriptor_size_));

    return {};
}

Result<void> NeuralSrPipeline::CreatePipelineObjects(ID3D12Device* device) {
    const auto shaders_dir_res = chimera::platform::win32::GetShadersDirectory();
    if (!shaders_dir_res.ok()) {
        return shaders_dir_res.status();
    }
    const auto shader_root = shaders_dir_res.value() / "ml";
    
    auto pass1_cs = CompileShaderFromFile(shader_root / "neural_sr_pass1.hlsl", "CSMain", "cs_5_1");
    if (!pass1_cs.ok()) {
        return pass1_cs.status();
    }
    
    auto pass2_cs = CompileShaderFromFile(shader_root / "neural_sr_pass2.hlsl", "CSMain", "cs_5_1");
    if (!pass2_cs.ok()) {
        return pass2_cs.status();
    }

    // Pass 1 Root Signature
    D3D12_DESCRIPTOR_RANGE pass1_srv_range{};
    pass1_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pass1_srv_range.NumDescriptors = 4U; // color, depth, motion, weights
    pass1_srv_range.BaseShaderRegister = 0U;

    D3D12_DESCRIPTOR_RANGE pass1_uav_range{};
    pass1_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    pass1_uav_range.NumDescriptors = 1U; // features output
    pass1_uav_range.BaseShaderRegister = 0U;

    D3D12_ROOT_PARAMETER pass1_params[3]{};
    pass1_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    pass1_params[0].Descriptor.ShaderRegister = 0U;
    pass1_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    pass1_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pass1_params[1].DescriptorTable.NumDescriptorRanges = 1U;
    pass1_params[1].DescriptorTable.pDescriptorRanges = &pass1_srv_range;
    pass1_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    pass1_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pass1_params[2].DescriptorTable.NumDescriptorRanges = 1U;
    pass1_params[2].DescriptorTable.pDescriptorRanges = &pass1_uav_range;
    pass1_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC pass1_desc{};
    pass1_desc.NumParameters = 3U;
    pass1_desc.pParameters = pass1_params;
    pass1_desc.NumStaticSamplers = 0U;

    Microsoft::WRL::ComPtr<ID3DBlob> signature_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(&pass1_desc, D3D_ROOT_SIGNATURE_VERSION_1, signature_blob.GetAddressOf(), error_blob.GetAddressOf()), "D3D12SerializeRootSignature(Pass 1)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateRootSignature(0U, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(pass1_root_signature_.ReleaseAndGetAddressOf())), "CreateRootSignature(Pass 1)"));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pass1_pso_desc{};
    pass1_pso_desc.pRootSignature = pass1_root_signature_.Get();
    pass1_pso_desc.CS = {pass1_cs.value()->GetBufferPointer(), pass1_cs.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(&pass1_pso_desc, IID_PPV_ARGS(pass1_pso_.ReleaseAndGetAddressOf())), "CreateComputePipelineState(Pass 1)"));

    // Pass 2 Root Signature
    D3D12_DESCRIPTOR_RANGE pass2_srv_range{};
    pass2_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    pass2_srv_range.NumDescriptors = 6U; // color, depth, motion, weights, features, history
    pass2_srv_range.BaseShaderRegister = 0U;

    D3D12_DESCRIPTOR_RANGE pass2_uav_range{};
    pass2_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    pass2_uav_range.NumDescriptors = 2U; // output color, output history
    pass2_uav_range.BaseShaderRegister = 0U;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0U;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_PARAMETER pass2_params[3]{};
    pass2_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    pass2_params[0].Descriptor.ShaderRegister = 0U;
    pass2_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    pass2_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pass2_params[1].DescriptorTable.NumDescriptorRanges = 1U;
    pass2_params[1].DescriptorTable.pDescriptorRanges = &pass2_srv_range;
    pass2_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    pass2_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    pass2_params[2].DescriptorTable.NumDescriptorRanges = 1U;
    pass2_params[2].DescriptorTable.pDescriptorRanges = &pass2_uav_range;
    pass2_params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC pass2_desc{};
    pass2_desc.NumParameters = 3U;
    pass2_desc.pParameters = pass2_params;
    pass2_desc.NumStaticSamplers = 1U;
    pass2_desc.pStaticSamplers = &sampler;

    signature_blob.Reset();
    error_blob.Reset();
    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(&pass2_desc, D3D_ROOT_SIGNATURE_VERSION_1, signature_blob.GetAddressOf(), error_blob.GetAddressOf()), "D3D12SerializeRootSignature(Pass 2)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateRootSignature(0U, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(pass2_root_signature_.ReleaseAndGetAddressOf())), "CreateRootSignature(Pass 2)"));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pass2_pso_desc{};
    pass2_pso_desc.pRootSignature = pass2_root_signature_.Get();
    pass2_pso_desc.CS = {pass2_cs.value()->GetBufferPointer(), pass2_cs.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(&pass2_pso_desc, IID_PPV_ARGS(pass2_pso_.ReleaseAndGetAddressOf())), "CreateComputePipelineState(Pass 2)"));

    return {};
}

Result<void> NeuralSrPipeline::EnsureOutputResources(ID3D12Device* device, const std::uint32_t width, const std::uint32_t height) {
    if (width == output_width_ && height == output_height_ && history_textures_[0] && history_textures_[1] && features_texture_array_) {
        return {};
    }

    output_width_ = width;
    output_height_ = height;
    history_state_.Reset();

    D3D12_HEAP_PROPERTIES default_props{};
    default_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Create Features Texture Array: 8 channels at high-res
    // We represent 8 channels as a Texture2DArray with 2 slices of R16G16B16A16_FLOAT
    D3D12_RESOURCE_DESC features_desc{};
    features_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    features_desc.Width = output_width_;
    features_desc.Height = output_height_;
    features_desc.DepthOrArraySize = 2U; // 2 slices
    features_desc.MipLevels = 1U;
    features_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    features_desc.SampleDesc.Count = 1U;
    features_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    features_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &default_props,
        D3D12_HEAP_FLAG_NONE,
        &features_desc,
        D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
        nullptr,
        IID_PPV_ARGS(features_texture_array_.ReleaseAndGetAddressOf())),
        "CreateCommittedResource(features texture array)"));

    // Create double-buffered history textures
    D3D12_RESOURCE_DESC history_desc{};
    history_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    history_desc.Width = output_width_;
    history_desc.Height = output_height_;
    history_desc.DepthOrArraySize = 1U;
    history_desc.MipLevels = 1U;
    history_desc.Format = display_format_;
    history_desc.SampleDesc.Count = 1U;
    history_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    history_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    for (auto& history_texture : history_textures_) {
        CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
            &default_props,
            D3D12_HEAP_FLAG_NONE,
            &history_desc,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(history_texture.ReleaseAndGetAddressOf())),
            "CreateCommittedResource(Neural history texture)"));
    }

    // Bind SRVs & UAVs
    const auto heap_cpu_start = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();

    // Bind features SRV (slice 4)
    D3D12_SHADER_RESOURCE_VIEW_DESC features_srv_desc{};
    features_srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    features_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    features_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    features_srv_desc.Texture2DArray.MipLevels = 1U;
    features_srv_desc.Texture2DArray.FirstArraySlice = 0U;
    features_srv_desc.Texture2DArray.ArraySize = 2U;
    device->CreateShaderResourceView(features_texture_array_.Get(), &features_srv_desc, OffsetCpuHandle(heap_cpu_start, 4U, descriptor_size_));

    // Bind features UAV (slot 6 in the single heap)
    D3D12_UNORDERED_ACCESS_VIEW_DESC features_uav_desc{};
    features_uav_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    features_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    features_uav_desc.Texture2DArray.MipSlice = 0U;
    features_uav_desc.Texture2DArray.FirstArraySlice = 0U;
    features_uav_desc.Texture2DArray.ArraySize = 2U;
    device->CreateUnorderedAccessView(features_texture_array_.Get(), nullptr, &features_uav_desc, OffsetCpuHandle(heap_cpu_start, 6U, descriptor_size_));

    write_history_index_ = 0U;
    return {};
}

Result<NeuralSrPassStats> NeuralSrPipeline::Execute(
    ID3D12GraphicsCommandList* command_list,
    const NeuralSrInputs& inputs,
    const D3D12_CPU_DESCRIPTOR_HANDLE output_rtv) {
    (void)output_rtv;
    if (inputs.low_res_color == nullptr || inputs.low_res_depth == nullptr || inputs.low_res_motion == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "NeuralSrPipeline::Execute received null input resources.");
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(inputs.low_res_color->GetDevice(IID_PPV_ARGS(device.GetAddressOf())), "ID3D12Resource::GetDevice"));
    CHIMERA_RETURN_IF_ERROR(EnsureOutputResources(device.Get(), inputs.output_width, inputs.output_height));

    const auto history_was_reset = inputs.reset_history || history_state_.reset_requested || !history_state_.valid;
    if (inputs.reset_history) {
        history_state_.Reset();
    }

    // Update Constant Buffer
    mapped_constants_->input_size[0] = static_cast<float>(inputs.input_width);
    mapped_constants_->input_size[1] = static_cast<float>(inputs.input_height);
    mapped_constants_->output_size[0] = static_cast<float>(inputs.output_width);
    mapped_constants_->output_size[1] = static_cast<float>(inputs.output_height);
    mapped_constants_->current_jitter_pixels[0] = inputs.current_jitter_pixels[0];
    mapped_constants_->current_jitter_pixels[1] = inputs.current_jitter_pixels[1];
    mapped_constants_->previous_jitter_pixels[0] = inputs.previous_jitter_pixels[0];
    mapped_constants_->previous_jitter_pixels[1] = inputs.previous_jitter_pixels[1];
    mapped_constants_->input_texel_size[0] = 1.0F / static_cast<float>(inputs.input_width);
    mapped_constants_->input_texel_size[1] = 1.0F / static_cast<float>(inputs.input_height);
    mapped_constants_->output_texel_size[0] = 1.0F / static_cast<float>(inputs.output_width);
    mapped_constants_->output_texel_size[1] = 1.0F / static_cast<float>(inputs.output_height);
    mapped_constants_->exposure = inputs.exposure;
    mapped_constants_->sharpen_strength = 0.0F;
    mapped_constants_->motion_vector_scale = 1.0F;
    mapped_constants_->history_valid = (history_state_.valid && !history_was_reset) ? 1.0F : 0.0F;
    mapped_constants_->frame_index = static_cast<std::uint32_t>(inputs.frame_index);
    mapped_constants_->input_width = inputs.input_width;
    mapped_constants_->input_height = inputs.input_height;
    mapped_constants_->output_width = inputs.output_width;
    mapped_constants_->output_height = inputs.output_height;

    // Create descriptor table entries for Pass 1 and Pass 2
    const auto heap_cpu_start = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    const auto heap_gpu_start = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();

    // Bind Pass 1 Inputs:
    // 0: color
    // 1: depth
    // 2: motion
    // 3: weights (already bound during initialization)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1U;

    srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(inputs.low_res_color, &srv_desc, OffsetCpuHandle(heap_cpu_start, 0U, descriptor_size_));

    srv_desc.Format = DXGI_FORMAT_R16_FLOAT;
    device->CreateShaderResourceView(inputs.low_res_depth, &srv_desc, OffsetCpuHandle(heap_cpu_start, 1U, descriptor_size_));

    srv_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    device->CreateShaderResourceView(inputs.low_res_motion, &srv_desc, OffsetCpuHandle(heap_cpu_start, 2U, descriptor_size_));

    // Also bind Previous History to SRV heap index 5
    srv_desc.Format = display_format_;
    device->CreateShaderResourceView(PreviousHistoryTexture(), &srv_desc, OffsetCpuHandle(heap_cpu_start, 5U, descriptor_size_));

    // Bind Pass 2 Outputs:
    // We write final color to CurrentHistoryTexture.
    // Slot 7: Output backbuffer temp UAV
    // Slot 8: Output history UAV
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.Format = display_format_;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0U;

    device->CreateUnorderedAccessView(CurrentHistoryTexture(), nullptr, &uav_desc, OffsetCpuHandle(heap_cpu_start, 7U, descriptor_size_));
    device->CreateUnorderedAccessView(CurrentHistoryTexture(), nullptr, &uav_desc, OffsetCpuHandle(heap_cpu_start, 8U, descriptor_size_));

    // State Transitions:
    // Color, Depth, Motion to SHADER_RESOURCE (if they aren't already).
    // Features texture to UAV.
    // Previous History to SHADER_RESOURCE.
    // Current History to UAV.
    TransitionResource(command_list, features_texture_array_.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    TransitionResource(command_list, CurrentHistoryTexture(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    chimera::common::Stopwatch total_gpu_timer{};

    // --- DISPATCH PASS 1 ---
    command_list->SetComputeRootSignature(pass1_root_signature_.Get());
    command_list->SetPipelineState(pass1_pso_.Get());

    ID3D12DescriptorHeap* descriptor_heaps[] = {descriptor_heap_.Get()};
    command_list->SetDescriptorHeaps(1U, descriptor_heaps);

    command_list->SetComputeRootConstantBufferView(0U, constant_buffer_->GetGPUVirtualAddress());
    // Pass 1 SRV Table (first 4 descriptors: color, depth, motion, weights)
    command_list->SetComputeRootDescriptorTable(1U, heap_gpu_start);
    // Pass 1 UAV Table (features UAV at index 6)
    command_list->SetComputeRootDescriptorTable(2U, OffsetGpuHandle(heap_gpu_start, 6U, descriptor_size_));

    // Thread groups: (width + 15) / 16
    UINT thread_groups_x = (inputs.input_width + 15U) / 16U;
    UINT thread_groups_y = (inputs.input_height + 15U) / 16U;
    command_list->Dispatch(thread_groups_x, thread_groups_y, 1U);

    // Sync pass 1 UAV output to Pass 2 SRV read
    TransitionResource(command_list, features_texture_array_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    // --- DISPATCH PASS 2 ---
    command_list->SetComputeRootSignature(pass2_root_signature_.Get());
    command_list->SetPipelineState(pass2_pso_.Get());

    // Reuse descriptor heaps
    command_list->SetDescriptorHeaps(1U, descriptor_heaps);

    command_list->SetComputeRootConstantBufferView(0U, constant_buffer_->GetGPUVirtualAddress());
    // Pass 2 SRV Table (starts at index 0, size 6: color, depth, motion, weights, features, history)
    command_list->SetComputeRootDescriptorTable(1U, heap_gpu_start);
    // Pass 2 UAV Table (starts at index 7, size 2: output color, output history)
    command_list->SetComputeRootDescriptorTable(2U, OffsetGpuHandle(heap_gpu_start, 7U, descriptor_size_));

    UINT hr_thread_groups_x = (inputs.output_width + 15U) / 16U;
    UINT hr_thread_groups_y = (inputs.output_height + 15U) / 16U;
    command_list->Dispatch(hr_thread_groups_x, hr_thread_groups_y, 1U);

    // Sync Current History UAV to Present/Shader resource
    TransitionResource(command_list, CurrentHistoryTexture(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

    const auto inference_ms = static_cast<float>(total_gpu_timer.ElapsedMilliseconds());

    history_state_.Advance(inputs.frame_index, inputs.current_jitter_pixels);
    write_history_index_ = 1U - write_history_index_;

    return NeuralSrPassStats{inference_ms, history_was_reset};
}

} // namespace chimera::ml
