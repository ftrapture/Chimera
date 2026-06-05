#include "d3d12/sample_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <d3dcompiler.h>

#include "common/time.h"

namespace chimera::d3d12 {

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

} // namespace

Result<void> SampleSceneRenderer::Initialize(ID3D12Device* device, std::uint32_t width, std::uint32_t height) {
    device_ = device;
    CHIMERA_RETURN_IF_ERROR(CreatePipeline());
    CHIMERA_RETURN_IF_ERROR(CreateConstantBuffer());
    return Resize(width, height);
}

Result<void> SampleSceneRenderer::Resize(std::uint32_t width, std::uint32_t height) {
    width_ = (std::max)(1U, width);
    height_ = (std::max)(1U, height);
    return CreateRenderTargets();
}

float SampleSceneRenderer::Render(
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

Result<void> SampleSceneRenderer::CreatePipeline() {
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

Result<void> SampleSceneRenderer::CreateConstantBuffer() {
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

Result<void> SampleSceneRenderer::CreateRenderTargets() {
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

} // namespace chimera::d3d12
