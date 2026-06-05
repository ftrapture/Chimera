#include "sr/analytic_tsr.h"

#include <filesystem>
#include <sstream>

#include <d3dcompiler.h>

#include "common/time.h"

namespace chimera::sr {

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

D3D12_BLEND_DESC DefaultBlendDesc() {
    D3D12_BLEND_DESC desc{};
    desc.AlphaToCoverageEnable = FALSE;
    desc.IndependentBlendEnable = FALSE;
    for (auto& render_target : desc.RenderTarget) {
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
    return desc;
}

D3D12_RASTERIZER_DESC DefaultRasterizerDesc() {
    D3D12_RASTERIZER_DESC desc{};
    desc.FillMode = D3D12_FILL_MODE_SOLID;
    desc.CullMode = D3D12_CULL_MODE_NONE;
    desc.FrontCounterClockwise = FALSE;
    desc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.DepthClipEnable = TRUE;
    desc.MultisampleEnable = FALSE;
    desc.AntialiasedLineEnable = FALSE;
    desc.ForcedSampleCount = 0U;
    desc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return desc;
}

D3D12_DEPTH_STENCIL_DESC DisabledDepthStencilDesc() {
    D3D12_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = FALSE;
    desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.StencilEnable = FALSE;
    desc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.BackFace = desc.FrontFace;
    return desc;
}

}  // namespace

Result<void> AnalyticTsrPipeline::Initialize(ID3D12Device* device, const DXGI_FORMAT display_format) {
    display_format_ = display_format;
    CHIMERA_RETURN_IF_ERROR(CreatePipelineObjects(device));

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.NumDescriptors = 5U;
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(srv_heap_.ReleaseAndGetAddressOf())), "CreateDescriptorHeap(SRV)"));
    srv_descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_RESOURCE_DESC cb_desc{};
    cb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cb_desc.Width = 65536U;
    cb_desc.Height = 1U;
    cb_desc.DepthOrArraySize = 1U;
    cb_desc.MipLevels = 1U;
    cb_desc.Format = DXGI_FORMAT_UNKNOWN;
    cb_desc.SampleDesc.Count = 1U;
    cb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &cb_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(constant_buffer_.ReleaseAndGetAddressOf())),
        "CreateCommittedResource(constant buffer)"));

    CHIMERA_RETURN_IF_ERROR(HrCheck(constant_buffer_->Map(0U, nullptr, reinterpret_cast<void**>(&mapped_constants_)), "ID3D12Resource::Map(constant buffer)"));
    return {};
}

Result<void> AnalyticTsrPipeline::CreatePipelineObjects(ID3D12Device* device) {
    const auto shader_root = std::filesystem::path(CHIMERA_SOURCE_DIR) / "shaders" / "sr";
    auto accumulate_vs = CompileShaderFromFile(shader_root / "accumulate.hlsl", "VSMain", "vs_5_1");
    if (!accumulate_vs.ok()) {
        return accumulate_vs.status();
    }
    auto accumulate_ps = CompileShaderFromFile(shader_root / "accumulate.hlsl", "PSMain", "ps_5_1");
    if (!accumulate_ps.ok()) {
        return accumulate_ps.status();
    }
    auto sharpen_vs = CompileShaderFromFile(shader_root / "sharpen.hlsl", "VSMain", "vs_5_1");
    if (!sharpen_vs.ok()) {
        return sharpen_vs.status();
    }
    auto sharpen_ps = CompileShaderFromFile(shader_root / "sharpen.hlsl", "PSMain", "ps_5_1");
    if (!sharpen_ps.ok()) {
        return sharpen_ps.status();
    }

    D3D12_DESCRIPTOR_RANGE accumulate_range{};
    accumulate_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    accumulate_range.NumDescriptors = 4U;
    accumulate_range.BaseShaderRegister = 0U;
    accumulate_range.RegisterSpace = 0U;
    accumulate_range.OffsetInDescriptorsFromTableStart = 0U;

    D3D12_ROOT_PARAMETER accumulate_params[2]{};
    accumulate_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    accumulate_params[0].Descriptor.ShaderRegister = 0U;
    accumulate_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    accumulate_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    accumulate_params[1].DescriptorTable.NumDescriptorRanges = 1U;
    accumulate_params[1].DescriptorTable.pDescriptorRanges = &accumulate_range;
    accumulate_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0U;
    sampler.RegisterSpace = 0U;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC accumulate_root_desc{};
    accumulate_root_desc.NumParameters = 2U;
    accumulate_root_desc.pParameters = accumulate_params;
    accumulate_root_desc.NumStaticSamplers = 1U;
    accumulate_root_desc.pStaticSamplers = &sampler;
    accumulate_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signature_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(
        D3D12SerializeRootSignature(&accumulate_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, signature_blob.GetAddressOf(), error_blob.GetAddressOf()),
        "D3D12SerializeRootSignature(accumulate)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(
        device->CreateRootSignature(0U, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(accumulate_root_signature_.ReleaseAndGetAddressOf())),
        "CreateRootSignature(accumulate)"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC accumulate_desc{};
    accumulate_desc.pRootSignature = accumulate_root_signature_.Get();
    accumulate_desc.VS = {accumulate_vs.value()->GetBufferPointer(), accumulate_vs.value()->GetBufferSize()};
    accumulate_desc.PS = {accumulate_ps.value()->GetBufferPointer(), accumulate_ps.value()->GetBufferSize()};
    accumulate_desc.BlendState = DefaultBlendDesc();
    accumulate_desc.SampleMask = UINT_MAX;
    accumulate_desc.RasterizerState = DefaultRasterizerDesc();
    accumulate_desc.DepthStencilState = DisabledDepthStencilDesc();
    accumulate_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    accumulate_desc.NumRenderTargets = 1U;
    accumulate_desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    accumulate_desc.SampleDesc.Count = 1U;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateGraphicsPipelineState(&accumulate_desc, IID_PPV_ARGS(accumulate_pso_.ReleaseAndGetAddressOf())), "CreateGraphicsPipelineState(accumulate)"));

    D3D12_DESCRIPTOR_RANGE sharpen_range{};
    sharpen_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    sharpen_range.NumDescriptors = 1U;
    sharpen_range.BaseShaderRegister = 0U;
    sharpen_range.RegisterSpace = 0U;
    sharpen_range.OffsetInDescriptorsFromTableStart = 0U;

    D3D12_ROOT_PARAMETER sharpen_params[2]{};
    sharpen_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    sharpen_params[0].Descriptor.ShaderRegister = 0U;
    sharpen_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sharpen_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    sharpen_params[1].DescriptorTable.NumDescriptorRanges = 1U;
    sharpen_params[1].DescriptorTable.pDescriptorRanges = &sharpen_range;
    sharpen_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC sharpen_root_desc{};
    sharpen_root_desc.NumParameters = 2U;
    sharpen_root_desc.pParameters = sharpen_params;
    sharpen_root_desc.NumStaticSamplers = 1U;
    sharpen_root_desc.pStaticSamplers = &sampler;
    sharpen_root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    signature_blob.Reset();
    error_blob.Reset();
    CHIMERA_RETURN_IF_ERROR(HrCheck(
        D3D12SerializeRootSignature(&sharpen_root_desc, D3D_ROOT_SIGNATURE_VERSION_1, signature_blob.GetAddressOf(), error_blob.GetAddressOf()),
        "D3D12SerializeRootSignature(sharpen)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(
        device->CreateRootSignature(0U, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(sharpen_root_signature_.ReleaseAndGetAddressOf())),
        "CreateRootSignature(sharpen)"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC sharpen_desc{};
    sharpen_desc.pRootSignature = sharpen_root_signature_.Get();
    sharpen_desc.VS = {sharpen_vs.value()->GetBufferPointer(), sharpen_vs.value()->GetBufferSize()};
    sharpen_desc.PS = {sharpen_ps.value()->GetBufferPointer(), sharpen_ps.value()->GetBufferSize()};
    sharpen_desc.BlendState = DefaultBlendDesc();
    sharpen_desc.SampleMask = UINT_MAX;
    sharpen_desc.RasterizerState = DefaultRasterizerDesc();
    sharpen_desc.DepthStencilState = DisabledDepthStencilDesc();
    sharpen_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    sharpen_desc.NumRenderTargets = 1U;
    sharpen_desc.RTVFormats[0] = display_format_;
    sharpen_desc.SampleDesc.Count = 1U;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateGraphicsPipelineState(&sharpen_desc, IID_PPV_ARGS(sharpen_pso_.ReleaseAndGetAddressOf())), "CreateGraphicsPipelineState(sharpen)"));

    return {};
}

Result<void> AnalyticTsrPipeline::EnsureOutputResources(ID3D12Device* device, const std::uint32_t width, const std::uint32_t height) {
    if (width == output_width_ && height == output_height_ && history_textures_[0] && history_textures_[1]) {
        return {};
    }

    output_width_ = width;
    output_height_ = height;
    history_state_.Reset();

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = output_width_;
    texture_desc.Height = output_height_;
    texture_desc.DepthOrArraySize = 1U;
    texture_desc.MipLevels = 1U;
    texture_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture_desc.SampleDesc.Count = 1U;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = texture_desc.Format;
    clear_value.Color[0] = 0.0F;
    clear_value.Color[1] = 0.0F;
    clear_value.Color[2] = 0.0F;
    clear_value.Color[3] = 1.0F;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (auto& history_texture : history_textures_) {
        history_texture.Reset();
        CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &texture_desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clear_value,
            IID_PPV_ARGS(history_texture.ReleaseAndGetAddressOf())),
            "CreateCommittedResource(history texture)"));
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 2U;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(rtv_heap_.ReleaseAndGetAddressOf())), "CreateDescriptorHeap(history RTV)"));
    rtv_descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (const auto& history_texture : history_textures_) {
        device->CreateRenderTargetView(history_texture.Get(), nullptr, rtv_handle);
        rtv_handle.ptr += static_cast<SIZE_T>(rtv_descriptor_size_);
    }

    write_history_index_ = 0U;
    return {};
}

Result<SrPassStats> AnalyticTsrPipeline::Execute(
    ID3D12GraphicsCommandList* command_list,
    const SrInputs& inputs,
    const D3D12_CPU_DESCRIPTOR_HANDLE output_rtv) {
    if (inputs.low_res_color == nullptr || inputs.low_res_depth == nullptr || inputs.low_res_motion == nullptr) {
        return Status::Error(ErrorCode::kInvalidArgument, "AnalyticTsrPipeline::Execute received null input resources.");
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(inputs.low_res_color->GetDevice(IID_PPV_ARGS(device.GetAddressOf())), "ID3D12Resource::GetDevice"));
    CHIMERA_RETURN_IF_ERROR(EnsureOutputResources(device.Get(), inputs.output_width, inputs.output_height));

    const auto history_was_reset = inputs.reset_history || history_state_.reset_requested || !history_state_.valid;
    if (inputs.reset_history) {
        history_state_.Reset();
    }

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
    mapped_constants_->sharpen_strength = inputs.sharpen_strength;
    mapped_constants_->motion_vector_scale = 1.0F;
    mapped_constants_->history_valid = history_state_.valid ? 1.0F : 0.0F;
    mapped_constants_->variance_clip_gamma = inputs.variance_clip_gamma;
    mapped_constants_->reactive_strength = 0.0F;
    mapped_constants_->frame_index = static_cast<std::uint32_t>(inputs.frame_index);
    mapped_constants_->anti_flicker_strength = inputs.anti_flicker_strength;

    const auto srv_cpu_start = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    const auto srv_gpu_start = srv_heap_->GetGPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Texture2D.MipLevels = 1U;

    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv = color_srv;
    depth_srv.Format = DXGI_FORMAT_R16_FLOAT;

    D3D12_SHADER_RESOURCE_VIEW_DESC motion_srv = color_srv;
    motion_srv.Format = DXGI_FORMAT_R16G16_FLOAT;

    D3D12_SHADER_RESOURCE_VIEW_DESC history_srv = color_srv;
    history_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    Microsoft::WRL::ComPtr<ID3D12Device> device_ptr{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->GetDevice(IID_PPV_ARGS(device_ptr.GetAddressOf())), "ID3D12GraphicsCommandList::GetDevice"));
    device_ptr->CreateShaderResourceView(inputs.low_res_color, &color_srv, OffsetCpuHandle(srv_cpu_start, 0U, srv_descriptor_size_));
    device_ptr->CreateShaderResourceView(inputs.low_res_depth, &depth_srv, OffsetCpuHandle(srv_cpu_start, 1U, srv_descriptor_size_));
    device_ptr->CreateShaderResourceView(inputs.low_res_motion, &motion_srv, OffsetCpuHandle(srv_cpu_start, 2U, srv_descriptor_size_));
    device_ptr->CreateShaderResourceView(PreviousHistoryTexture(), &history_srv, OffsetCpuHandle(srv_cpu_start, 3U, srv_descriptor_size_));

    const auto current_rtv = OffsetCpuHandle(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), write_history_index_, rtv_descriptor_size_);
    TransitionResource(command_list, CurrentHistoryTexture(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    common::Stopwatch accumulate_timer{};
    command_list->SetGraphicsRootSignature(accumulate_root_signature_.Get());
    ID3D12DescriptorHeap* descriptor_heaps[] = {srv_heap_.Get()};
    command_list->SetDescriptorHeaps(1U, descriptor_heaps);
    command_list->SetPipelineState(accumulate_pso_.Get());
    const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(inputs.output_width), static_cast<float>(inputs.output_height), 0.0F, 1.0F};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(inputs.output_width), static_cast<LONG>(inputs.output_height)};
    command_list->RSSetViewports(1U, &viewport);
    command_list->RSSetScissorRects(1U, &scissor);
    const float clear_color[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    command_list->ClearRenderTargetView(current_rtv, clear_color, 0U, nullptr);
    command_list->OMSetRenderTargets(1U, &current_rtv, FALSE, nullptr);
    command_list->SetGraphicsRootConstantBufferView(0U, constant_buffer_->GetGPUVirtualAddress());
    command_list->SetGraphicsRootDescriptorTable(1U, srv_gpu_start);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->DrawInstanced(3U, 1U, 0U, 0U);
    TransitionResource(command_list, CurrentHistoryTexture(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const auto accumulate_ms = static_cast<float>(accumulate_timer.ElapsedMilliseconds());

    device_ptr->CreateShaderResourceView(CurrentHistoryTexture(), &history_srv, OffsetCpuHandle(srv_cpu_start, 4U, srv_descriptor_size_));

    common::Stopwatch sharpen_timer{};
    command_list->SetGraphicsRootSignature(sharpen_root_signature_.Get());
    command_list->SetPipelineState(sharpen_pso_.Get());
    command_list->OMSetRenderTargets(1U, &output_rtv, FALSE, nullptr);
    command_list->SetGraphicsRootConstantBufferView(0U, constant_buffer_->GetGPUVirtualAddress());
    command_list->SetGraphicsRootDescriptorTable(1U, OffsetGpuHandle(srv_gpu_start, 4U, srv_descriptor_size_));
    command_list->DrawInstanced(3U, 1U, 0U, 0U);
    const auto sharpen_ms = static_cast<float>(sharpen_timer.ElapsedMilliseconds());

    history_state_.Advance(inputs.frame_index, inputs.current_jitter_pixels);
    write_history_index_ = 1U - write_history_index_;

    return SrPassStats{accumulate_ms, sharpen_ms, history_was_reset};
}

}  // namespace chimera::sr
