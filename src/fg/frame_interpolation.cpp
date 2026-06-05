#include "fg/frame_interpolation.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include <d3dcompiler.h>

#include "common/log.h"
#include "common/time.h"
#include "platform/win32/process_utils.h"

namespace chimera::fg {

namespace {

using chimera::common::ErrorCode;
using chimera::common::Result;
using chimera::common::Status;

[[nodiscard]] Result<void> HrCheck(const HRESULT hr, const char* context) {
    if (SUCCEEDED(hr)) return {};
    std::ostringstream s;
    s << context << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return Status::Error(ErrorCode::kDeviceError, s.str());
}

[[nodiscard]] Result<Microsoft::WRL::ComPtr<ID3DBlob>> CompileCS(
    const std::filesystem::path& path, const char* entry) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    Microsoft::WRL::ComPtr<ID3DBlob> blob{}, err{};
    const auto hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry, "cs_5_1", flags, 0U, blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream s;
        s << "Compile CS " << path.string() << " (" << entry << ")";
        if (err) s << ": " << static_cast<const char*>(err->GetBufferPointer());
        return Status::Error(ErrorCode::kDeviceError, s.str());
    }
    return blob;
}

[[nodiscard]] Result<Microsoft::WRL::ComPtr<ID3DBlob>> CompilePS(
    const char* source, const char* entry) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    Microsoft::WRL::ComPtr<ID3DBlob> blob{}, err{};
    const auto hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
        entry, "ps_5_1", flags, 0U, blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream s;
        s << "Compile PS " << entry;
        if (err) s << ": " << static_cast<const char*>(err->GetBufferPointer());
        return Status::Error(ErrorCode::kDeviceError, s.str());
    }
    return blob;
}

[[nodiscard]] Result<Microsoft::WRL::ComPtr<ID3DBlob>> CompileVS(
    const char* source, const char* entry) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    Microsoft::WRL::ComPtr<ID3DBlob> blob{}, err{};
    const auto hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
        entry, "vs_5_1", flags, 0U, blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        std::ostringstream s;
        s << "Compile VS " << entry;
        if (err) s << ": " << static_cast<const char*>(err->GetBufferPointer());
        return Status::Error(ErrorCode::kDeviceError, s.str());
    }
    return blob;
}

void UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1U, &b);
}

void TransitionResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1U, &b);
}

Result<void> CreateCB(ID3D12Device* dev, UINT size,
    Microsoft::WRL::ComPtr<ID3D12Resource>& buf, void** mapped) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (std::max)(static_cast<UINT64>(size), static_cast<UINT64>(65536U));
    desc.Height = 1U; desc.DepthOrArraySize = 1U; desc.MipLevels = 1U;
    desc.SampleDesc.Count = 1U; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHIMERA_RETURN_IF_ERROR(HrCheck(dev->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(buf.ReleaseAndGetAddressOf())), "CreateCB(fi)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(buf->Map(0U, nullptr, mapped), "MapCB(fi)"));
    return {};
}

Result<void> CreateUAVTex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
    Microsoft::WRL::ComPtr<ID3D12Resource>& tex) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U; desc.Format = fmt;
    desc.SampleDesc.Count = 1U;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CHIMERA_RETURN_IF_ERROR(HrCheck(dev->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr, IID_PPV_ARGS(tex.ReleaseAndGetAddressOf())), "CreateUAV(fi)"));
    return {};
}

Result<void> CreateComputeRS(ID3D12Device* dev, UINT srvs, UINT uavs,
    Microsoft::WRL::ComPtr<ID3D12RootSignature>& rs) {
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0U;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = srvs;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1U;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE uav_range{};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = uavs;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1U;
    params[2].DescriptorTable.pDescriptorRanges = &uav_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3U; desc.pParameters = params;
    desc.NumStaticSamplers = 1U; desc.pStaticSamplers = &sampler;

    Microsoft::WRL::ComPtr<ID3DBlob> sig{}, err{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, sig.GetAddressOf(), err.GetAddressOf()), "SerializeRS(fi)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(dev->CreateRootSignature(
        0U, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(rs.ReleaseAndGetAddressOf())), "CreateRS(fi)"));
    return {};
}

}  // namespace

Result<void> FrameInterpolationPipeline::Initialize(ID3D12Device* device, const DXGI_FORMAT output_format) {
    output_format_ = output_format;
    CHIMERA_RETURN_IF_ERROR(CreatePipelines(device));
    CHIMERA_RETURN_IF_ERROR(CreateCB(device, sizeof(WarpConstants), cb_warp_, reinterpret_cast<void**>(&mapped_warp_)));
    CHIMERA_RETURN_IF_ERROR(CreateCB(device, sizeof(BlendConstants), cb_blend_, reinterpret_cast<void**>(&mapped_blend_)));

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.NumDescriptors = 32U;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateDescriptorHeap(
        &heap_desc, IID_PPV_ARGS(descriptor_heap_.ReleaseAndGetAddressOf())), "CreateHeap(fi)"));
    descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    common::LogInfo("fg", "FrameInterpolationPipeline initialized.");
    return {};
}

Result<void> FrameInterpolationPipeline::CreatePipelines(ID3D12Device* device) {
    const auto shaders_dir_res = chimera::platform::win32::GetShadersDirectory();
    if (!shaders_dir_res.ok()) {
        return shaders_dir_res.status();
    }
    const auto shader_root = shaders_dir_res.value() / "fg";

    // Warp: 5 SRVs (frameN, frameN1, fwd_flow, bwd_flow, confidence), 3 UAVs (warped_n, warped_n1, occ)
    CHIMERA_RETURN_IF_ERROR(CreateComputeRS(device, 5U, 3U, warp_root_sig_));
    auto warp_blob = CompileCS(shader_root / "fi_warp.hlsl", "CSMain");
    if (!warp_blob.ok()) return warp_blob.status();
    D3D12_COMPUTE_PIPELINE_STATE_DESC warp_desc{};
    warp_desc.pRootSignature = warp_root_sig_.Get();
    warp_desc.CS = {warp_blob.value()->GetBufferPointer(), warp_blob.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(
        &warp_desc, IID_PPV_ARGS(warp_pso_.ReleaseAndGetAddressOf())), "CreatePSO(fi_warp)"));

    // Blend: 6 SRVs (warped_n, warped_n1, occ, frameN, frameN1, confidence), 1 UAV (output)
    CHIMERA_RETURN_IF_ERROR(CreateComputeRS(device, 6U, 1U, blend_root_sig_));
    auto blend_blob = CompileCS(shader_root / "fi_blend.hlsl", "CSMain");
    if (!blend_blob.ok()) return blend_blob.status();
    D3D12_COMPUTE_PIPELINE_STATE_DESC blend_desc{};
    blend_desc.pRootSignature = blend_root_sig_.Get();
    blend_desc.CS = {blend_blob.value()->GetBufferPointer(), blend_blob.value()->GetBufferSize()};
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateComputePipelineState(
        &blend_desc, IID_PPV_ARGS(blend_pso_.ReleaseAndGetAddressOf())), "CreatePSO(fi_blend)"));

    // Blit pass: simple fullscreen copy from UAV result to RTV backbuffer
    static constexpr char kBlitShader[] = R"(
Texture2D<float4> gSource : register(t0);
SamplerState gSampler : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
float4 PSMain(VSOut i) : SV_Target0 { return gSource.SampleLevel(gSampler, i.uv, 0); }
)";
    auto blit_vs = CompileVS(kBlitShader, "VSMain");
    if (!blit_vs.ok()) return blit_vs.status();
    auto blit_ps = CompilePS(kBlitShader, "PSMain");
    if (!blit_ps.ok()) return blit_ps.status();

    D3D12_DESCRIPTOR_RANGE blit_srv_range{};
    blit_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    blit_srv_range.NumDescriptors = 1U;
    D3D12_ROOT_PARAMETER blit_param{};
    blit_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    blit_param.DescriptorTable.NumDescriptorRanges = 1U;
    blit_param.DescriptorTable.pDescriptorRanges = &blit_srv_range;
    blit_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC blit_sampler{};
    blit_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    blit_sampler.AddressU = blit_sampler.AddressV = blit_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blit_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    blit_sampler.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC blit_rs_desc{};
    blit_rs_desc.NumParameters = 1U;
    blit_rs_desc.pParameters = &blit_param;
    blit_rs_desc.NumStaticSamplers = 1U;
    blit_rs_desc.pStaticSamplers = &blit_sampler;
    blit_rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> blit_sig{}, blit_err{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(
        &blit_rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, blit_sig.GetAddressOf(), blit_err.GetAddressOf()), "SerializeRS(blit)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateRootSignature(
        0U, blit_sig->GetBufferPointer(), blit_sig->GetBufferSize(),
        IID_PPV_ARGS(blit_root_sig_.ReleaseAndGetAddressOf())), "CreateRS(blit)"));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC blit_pso_desc{};
    blit_pso_desc.pRootSignature = blit_root_sig_.Get();
    blit_pso_desc.VS = {blit_vs.value()->GetBufferPointer(), blit_vs.value()->GetBufferSize()};
    blit_pso_desc.PS = {blit_ps.value()->GetBufferPointer(), blit_ps.value()->GetBufferSize()};
    blit_pso_desc.SampleMask = UINT_MAX;
    blit_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    blit_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    blit_pso_desc.RasterizerState.DepthClipEnable = TRUE;
    blit_pso_desc.DepthStencilState.DepthEnable = FALSE;
    blit_pso_desc.DepthStencilState.StencilEnable = FALSE;
    for (auto& rt : blit_pso_desc.BlendState.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD; rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    }
    blit_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    blit_pso_desc.NumRenderTargets = 1U;
    blit_pso_desc.RTVFormats[0] = output_format_;
    blit_pso_desc.SampleDesc.Count = 1U;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateGraphicsPipelineState(
        &blit_pso_desc, IID_PPV_ARGS(blit_pso_.ReleaseAndGetAddressOf())), "CreatePSO(blit)"));

    return {};
}

Result<void> FrameInterpolationPipeline::EnsureResources(ID3D12Device* device,
    const std::uint32_t width, const std::uint32_t height) {
    if (width == width_ && height == height_ && warped_from_n_) return {};
    width_ = width; height_ = height;

    CHIMERA_RETURN_IF_ERROR(CreateUAVTex(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, warped_from_n_));
    CHIMERA_RETURN_IF_ERROR(CreateUAVTex(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, warped_from_n1_));
    CHIMERA_RETURN_IF_ERROR(CreateUAVTex(device, width, height, DXGI_FORMAT_R8_UNORM, occlusion_map_));
    CHIMERA_RETURN_IF_ERROR(CreateUAVTex(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, interpolated_));
    return {};
}

Result<FiStats> FrameInterpolationPipeline::Execute(
    ID3D12GraphicsCommandList* command_list,
    const FiInputs& inputs,
    const D3D12_CPU_DESCRIPTOR_HANDLE output_rtv) {

    if (!inputs.frame_n || !inputs.frame_n1 || !inputs.forward_flow) {
        return Status::Error(ErrorCode::kInvalidArgument, "FI: null input.");
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(command_list->GetDevice(IID_PPV_ARGS(device.GetAddressOf())), "GetDevice(fi)"));
    CHIMERA_RETURN_IF_ERROR(EnsureResources(device.Get(), inputs.width, inputs.height));

    // Transition optical flow resources to shader read state
    TransitionResource(command_list, inputs.forward_flow,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (inputs.backward_flow) {
        TransitionResource(command_list, inputs.backward_flow,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (inputs.flow_confidence) {
        TransitionResource(command_list, inputs.flow_confidence,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
    command_list->SetDescriptorHeaps(1U, heaps);

    auto cpu_start = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
    auto gpu_start = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
    auto cpu_h = [&](UINT off) { auto h = cpu_start; h.ptr += static_cast<SIZE_T>(off) * descriptor_size_; return h; };
    auto gpu_h = [&](UINT off) { auto h = gpu_start; h.ptr += static_cast<UINT64>(off) * descriptor_size_; return h; };

    // ── PASS 1: Warp ────────────────────────────────────────────────────
    common::Stopwatch warp_timer{};
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
        color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        color_srv.Format = inputs.color_format;
        color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        color_srv.Texture2D.MipLevels = 1U;

        D3D12_SHADER_RESOURCE_VIEW_DESC flow_srv = color_srv;
        flow_srv.Format = DXGI_FORMAT_R16G16_FLOAT;

        D3D12_SHADER_RESOURCE_VIEW_DESC conf_srv = color_srv;
        conf_srv.Format = DXGI_FORMAT_R8_UNORM;

        device->CreateShaderResourceView(inputs.frame_n, &color_srv, cpu_h(0));
        device->CreateShaderResourceView(inputs.frame_n1, &color_srv, cpu_h(1));
        device->CreateShaderResourceView(inputs.forward_flow, &flow_srv, cpu_h(2));
        device->CreateShaderResourceView(inputs.backward_flow, &flow_srv, cpu_h(3));
        device->CreateShaderResourceView(inputs.flow_confidence, &conf_srv, cpu_h(4));

        D3D12_UNORDERED_ACCESS_VIEW_DESC color_uav{};
        color_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        color_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(warped_from_n_.Get(), nullptr, &color_uav, cpu_h(5));
        device->CreateUnorderedAccessView(warped_from_n1_.Get(), nullptr, &color_uav, cpu_h(6));

        D3D12_UNORDERED_ACCESS_VIEW_DESC occ_uav{};
        occ_uav.Format = DXGI_FORMAT_R8_UNORM;
        occ_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(occlusion_map_.Get(), nullptr, &occ_uav, cpu_h(7));

        mapped_warp_->size[0] = inputs.width;
        mapped_warp_->size[1] = inputs.height;
        mapped_warp_->texel_size[0] = 1.0F / static_cast<float>(inputs.width);
        mapped_warp_->texel_size[1] = 1.0F / static_cast<float>(inputs.height);
        mapped_warp_->interpolation_t = inputs.interpolation_factor;

        command_list->SetComputeRootSignature(warp_root_sig_.Get());
        command_list->SetPipelineState(warp_pso_.Get());
        command_list->SetComputeRootConstantBufferView(0U, cb_warp_->GetGPUVirtualAddress());
        command_list->SetComputeRootDescriptorTable(1U, gpu_h(0));
        command_list->SetComputeRootDescriptorTable(2U, gpu_h(5));
        command_list->Dispatch(
            (inputs.width + 7U) / 8U, (inputs.height + 7U) / 8U, 1U);

        UAVBarrier(command_list, warped_from_n_.Get());
        UAVBarrier(command_list, warped_from_n1_.Get());
        UAVBarrier(command_list, occlusion_map_.Get());
    }
    const auto warp_ms = static_cast<float>(warp_timer.ElapsedMilliseconds());

    // ── PASS 2: Blend + Inpaint ─────────────────────────────────────────
    common::Stopwatch blend_timer{};
    {
        TransitionResource(command_list, warped_from_n_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, warped_from_n1_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionResource(command_list, occlusion_map_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
        color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        color_srv.Format = inputs.color_format;
        color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        color_srv.Texture2D.MipLevels = 1U;

        D3D12_SHADER_RESOURCE_VIEW_DESC warp_srv = color_srv;
        warp_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        D3D12_SHADER_RESOURCE_VIEW_DESC occ_srv = color_srv;
        occ_srv.Format = DXGI_FORMAT_R8_UNORM;

        D3D12_SHADER_RESOURCE_VIEW_DESC conf_srv = color_srv;
        conf_srv.Format = DXGI_FORMAT_R8_UNORM;

        device->CreateShaderResourceView(warped_from_n_.Get(), &warp_srv, cpu_h(8));
        device->CreateShaderResourceView(warped_from_n1_.Get(), &warp_srv, cpu_h(9));
        device->CreateShaderResourceView(occlusion_map_.Get(), &occ_srv, cpu_h(10));
        device->CreateShaderResourceView(inputs.frame_n, &color_srv, cpu_h(11));
        device->CreateShaderResourceView(inputs.frame_n1, &color_srv, cpu_h(12));
        device->CreateShaderResourceView(inputs.flow_confidence, &conf_srv, cpu_h(13));

        D3D12_UNORDERED_ACCESS_VIEW_DESC out_uav{};
        out_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        out_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(interpolated_.Get(), nullptr, &out_uav, cpu_h(14));

        mapped_blend_->size[0] = inputs.width;
        mapped_blend_->size[1] = inputs.height;
        mapped_blend_->texel_size[0] = 1.0F / static_cast<float>(inputs.width);
        mapped_blend_->texel_size[1] = 1.0F / static_cast<float>(inputs.height);
        mapped_blend_->interpolation_t = inputs.interpolation_factor;

        command_list->SetComputeRootSignature(blend_root_sig_.Get());
        command_list->SetPipelineState(blend_pso_.Get());
        command_list->SetComputeRootConstantBufferView(0U, cb_blend_->GetGPUVirtualAddress());
        command_list->SetComputeRootDescriptorTable(1U, gpu_h(8));
        command_list->SetComputeRootDescriptorTable(2U, gpu_h(14));
        command_list->Dispatch(
            (inputs.width + 7U) / 8U, (inputs.height + 7U) / 8U, 1U);
        UAVBarrier(command_list, interpolated_.Get());

        TransitionResource(command_list, warped_from_n_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(command_list, warped_from_n1_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        TransitionResource(command_list, occlusion_map_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    const auto blend_ms = static_cast<float>(blend_timer.ElapsedMilliseconds());

    // ── Blit interpolated result to output RTV ──────────────────────────
    {
        TransitionResource(command_list, interpolated_.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC blit_srv{};
        blit_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        blit_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        blit_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        blit_srv.Texture2D.MipLevels = 1U;
        device->CreateShaderResourceView(interpolated_.Get(), &blit_srv, cpu_h(15));

        const D3D12_VIEWPORT vp{0, 0, static_cast<float>(inputs.width), static_cast<float>(inputs.height), 0, 1};
        const D3D12_RECT sc{0, 0, static_cast<LONG>(inputs.width), static_cast<LONG>(inputs.height)};

        command_list->SetGraphicsRootSignature(blit_root_sig_.Get());
        command_list->SetPipelineState(blit_pso_.Get());
        command_list->RSSetViewports(1U, &vp);
        command_list->RSSetScissorRects(1U, &sc);
        command_list->OMSetRenderTargets(1U, &output_rtv, FALSE, nullptr);
        command_list->SetGraphicsRootDescriptorTable(0U, gpu_h(15));
        command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        command_list->DrawInstanced(3U, 1U, 0U, 0U);

        TransitionResource(command_list, interpolated_.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Transition optical flow resources back to unordered access
    TransitionResource(command_list, inputs.forward_flow,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (inputs.backward_flow) {
        TransitionResource(command_list, inputs.backward_flow,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (inputs.flow_confidence) {
        TransitionResource(command_list, inputs.flow_confidence,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    return FiStats{warp_ms, blend_ms, warp_ms + blend_ms};
}

}  // namespace chimera::fg
