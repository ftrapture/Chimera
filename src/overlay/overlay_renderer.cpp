#include "overlay/overlay_renderer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

#include <d3dcompiler.h>

#include "common/time.h"
#include "platform/win32/process_utils.h"

namespace chimera::overlay {

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

[[nodiscard]] std::array<std::uint8_t, 7> GetGlyph(char c) {
    switch (c) {
        case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
        case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
        case 'D': return {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C};
        case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
        case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
        case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
        case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
        case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
        case 'J': return {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
        case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
        case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
        case 'N': return {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11};
        case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
        case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
        case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
        case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
        case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
        case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
        case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
        case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
        case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
        case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
        case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
        case '1': return {0x04, 0x0C, 0x14, 0x04, 0x04, 0x04, 0x1F};
        case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
        case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
        case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
        case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
        case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
        case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
        case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
        case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
        case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
        case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
        case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
        case '>': return {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10};
        case '/': return {0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00};
        case '|': return {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        case ' ': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        default: return {0x1F, 0x11, 0x02, 0x04, 0x08, 0x00, 0x08};
    }
}

void UppercaseInPlace(std::string& text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
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

Result<void> OverlayRenderer::Initialize(ID3D12Device* device, const DXGI_FORMAT output_format) {
    const auto shaders_dir_res = chimera::platform::win32::GetShadersDirectory();
    if (!shaders_dir_res.ok()) {
        return shaders_dir_res.status();
    }
    const auto shader_root = shaders_dir_res.value() / "debug";
    auto vs = CompileShaderFromFile(shader_root / "overlay_vs.hlsl", "VSMain", "vs_5_1");
    if (!vs.ok()) {
        return vs.status();
    }
    auto ps = CompileShaderFromFile(shader_root / "overlay_ps.hlsl", "PSMain", "ps_5_1");
    if (!ps.ok()) {
        return ps.status();
    }

    D3D12_ROOT_PARAMETER root_param{};
    root_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_param.Descriptor.ShaderRegister = 0U;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 1U;
    root_desc.pParameters = &root_param;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signature_blob{};
    Microsoft::WRL::ComPtr<ID3DBlob> error_blob{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, signature_blob.GetAddressOf(), error_blob.GetAddressOf()), "D3D12SerializeRootSignature(overlay)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateRootSignature(0U, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(root_signature_.ReleaseAndGetAddressOf())), "CreateRootSignature(overlay)"));

    D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"COLOR", 0U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 8U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };

    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.BlendEnable = TRUE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root_signature_.Get();
    pso_desc.InputLayout = {input_layout, 2U};
    pso_desc.VS = {vs.value()->GetBufferPointer(), vs.value()->GetBufferSize()};
    pso_desc.PS = {ps.value()->GetBufferPointer(), ps.value()->GetBufferSize()};
    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    pso_desc.BlendState.RenderTarget[0] = blend;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState = DefaultRasterizerDesc();
    pso_desc.DepthStencilState = DisabledDepthStencilDesc();
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1U;
    pso_desc.RTVFormats[0] = output_format;
    pso_desc.SampleDesc.Count = 1U;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(pipeline_state_.ReleaseAndGetAddressOf())), "CreateGraphicsPipelineState(overlay)"));

    max_vertices_ = 131072U;
    staging_vertices_.reserve(max_vertices_);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC vb_desc{};
    vb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    vb_desc.Width = static_cast<UINT64>(max_vertices_) * static_cast<UINT64>(sizeof(OverlayVertex));
    vb_desc.Height = 1U;
    vb_desc.DepthOrArraySize = 1U;
    vb_desc.MipLevels = 1U;
    vb_desc.SampleDesc.Count = 1U;
    vb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &vb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(vertex_buffer_.ReleaseAndGetAddressOf())), "CreateCommittedResource(overlay vertex buffer)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(vertex_buffer_->Map(0U, nullptr, reinterpret_cast<void**>(&mapped_vertices_)), "Map(overlay vertex buffer)"));

    D3D12_RESOURCE_DESC cb_desc = vb_desc;
    cb_desc.Width = 65536U;
    CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(constant_buffer_.ReleaseAndGetAddressOf())), "CreateCommittedResource(overlay constant buffer)"));
    CHIMERA_RETURN_IF_ERROR(HrCheck(constant_buffer_->Map(0U, nullptr, reinterpret_cast<void**>(&mapped_constants_)), "Map(overlay constant buffer)"));

    return {};
}

void OverlayRenderer::EmitQuad(const float x0, const float y0, const float x1, const float y1, const float color[4]) {
    if (staging_vertices_.size() + 6U > max_vertices_) {
        return;
    }

    staging_vertices_.push_back({{x0, y0}, {color[0], color[1], color[2], color[3]}});
    staging_vertices_.push_back({{x1, y0}, {color[0], color[1], color[2], color[3]}});
    staging_vertices_.push_back({{x1, y1}, {color[0], color[1], color[2], color[3]}});
    staging_vertices_.push_back({{x0, y0}, {color[0], color[1], color[2], color[3]}});
    staging_vertices_.push_back({{x1, y1}, {color[0], color[1], color[2], color[3]}});
    staging_vertices_.push_back({{x0, y1}, {color[0], color[1], color[2], color[3]}});
}

void OverlayRenderer::EmitText(float x, const float y, const char* text, const float color[4], const float scale) {
    constexpr float glyph_width = 5.0F;
    constexpr float glyph_height = 7.0F;
    constexpr float glyph_spacing = 1.0F;

    for (const char* ptr = text; *ptr != '\0'; ++ptr) {
        const auto glyph = GetGlyph(*ptr);
        for (std::size_t row = 0; row < glyph.size(); ++row) {
            for (int column = 0; column < 5; ++column) {
                const auto mask = static_cast<std::uint8_t>(1U << (4 - column));
                if ((glyph[row] & mask) == 0U) {
                    continue;
                }
                const auto px = x + static_cast<float>(column) * scale;
                const auto py = y + static_cast<float>(row) * scale;
                EmitQuad(px, py, px + scale, py + scale, color);
            }
        }
        x += (glyph_width + glyph_spacing) * scale;
        (void)glyph_height;
    }
}

void OverlayRenderer::BuildGeometry(const OverlaySnapshot& snapshot) {
    staging_vertices_.clear();

    static constexpr float background[4] = {0.04F, 0.06F, 0.10F, 0.78F};
    static constexpr float heading[4] = {0.92F, 0.95F, 0.98F, 1.0F};
    static constexpr float accent[4] = {0.43F, 0.88F, 0.73F, 1.0F};
    static constexpr float accent_red[4] = {0.92F, 0.38F, 0.32F, 1.0F};
    static constexpr float body[4] = {0.82F, 0.86F, 0.90F, 1.0F};
    static constexpr float fi_on[4] = {0.30F, 0.92F, 0.50F, 1.0F};
    static constexpr float fi_off[4] = {0.70F, 0.70F, 0.70F, 1.0F};
    static constexpr float fps_color[4] = {1.0F, 0.85F, 0.25F, 1.0F};

    EmitQuad(16.0F, 16.0F, 540.0F, 220.0F, background);

    char line0[64]{};
    std::snprintf(line0, sizeof(line0), "PROJECT CHIMERA");

    char line1[96]{};
    std::string tier_text{chimera::common::ToString(snapshot.tier)};
    UppercaseInPlace(tier_text);
    const bool tsr_on = snapshot.tier != chimera::common::CapabilityTier::kTierC;
    std::snprintf(line1, sizeof(line1), "%s  TSR %s", tier_text.c_str(), tsr_on ? "ON" : "OFF");

    char line2[96]{};
    std::snprintf(line2, sizeof(line2), "RENDER %ux%u > DISPLAY %ux%u",
        snapshot.render_width, snapshot.render_height,
        snapshot.display_width, snapshot.display_height);
    std::string line2_upper{line2};
    UppercaseInPlace(line2_upper);

    char line3[128]{};
    std::snprintf(line3, sizeof(line3), "SCENE %.2FMS  SR %.2FMS  OVL %.2FMS",
        snapshot.scene_cpu_ms, snapshot.sr_cpu_ms, snapshot.overlay_cpu_ms);
    std::string line3_upper{line3};
    UppercaseInPlace(line3_upper);

    // FI status line
    char line_fi[128]{};
    if (snapshot.fi_enabled) {
        std::snprintf(line_fi, sizeof(line_fi), "FI ON  FLOW %.2FMS  WARP %.2FMS  LAT %.1FMS",
            snapshot.flow_cpu_ms, snapshot.fi_cpu_ms, snapshot.pacing_latency_ms);
    } else {
        std::snprintf(line_fi, sizeof(line_fi), "FI OFF");
    }
    std::string line_fi_upper{line_fi};
    UppercaseInPlace(line_fi_upper);

    // FPS line
    char line_fps[96]{};
    if (snapshot.fi_enabled && snapshot.effective_fps > 0.01F) {
        std::snprintf(line_fps, sizeof(line_fps), "FPS %.0F > %.0F  -  2X FRAME GEN",
            snapshot.real_fps, snapshot.effective_fps);
    } else if (snapshot.real_fps > 0.01F) {
        std::snprintf(line_fps, sizeof(line_fps), "FPS %.0F", snapshot.real_fps);
    } else {
        std::snprintf(line_fps, sizeof(line_fps), "FPS ---");
    }
    std::string line_fps_upper{line_fps};
    UppercaseInPlace(line_fps_upper);

    char line_signals[128]{};
    std::snprintf(line_signals, sizeof(line_signals),
        "SIGNALS C%.2F D%.2F M%.2F J%.2F U%.2F",
        snapshot.signals.color.confidence,
        snapshot.signals.depth.confidence,
        snapshot.signals.motion.confidence,
        snapshot.signals.jitter.confidence,
        snapshot.signals.ui.confidence);
    std::string line_signals_upper{line_signals};
    UppercaseInPlace(line_signals_upper);

    // VSync + Quality mode line
    char line_vsync_qual[128]{};
    std::string quality_str{chimera::common::ToString(snapshot.quality_mode)};
    UppercaseInPlace(quality_str);
    std::snprintf(line_vsync_qual, sizeof(line_vsync_qual), "VSYNC %s  QUALITY %s",
        snapshot.vsync_enabled ? "ON" : "OFF", quality_str.c_str());

    float y = 28.0F;
    const float dy = 22.0F;
    EmitText(28.0F, y, line0, heading, 2.0F); y += dy;
    EmitText(28.0F, y, line1, accent, 2.0F); y += dy;
    EmitText(28.0F, y, line2_upper.c_str(), body, 2.0F); y += dy;
    EmitText(28.0F, y, line3_upper.c_str(), body, 2.0F); y += dy;
    EmitText(28.0F, y, line_fi_upper.c_str(), snapshot.fi_enabled ? fi_on : fi_off, 2.0F); y += dy;
    EmitText(28.0F, y, line_fps_upper.c_str(), fps_color, 2.0F); y += dy;
    EmitText(28.0F, y, line_vsync_qual, snapshot.vsync_enabled ? fi_on : fi_off, 2.0F); y += dy;
    EmitText(28.0F, y, line_signals_upper.c_str(), body, 2.0F); y += dy;

    char line_keys[80]{};
    std::snprintf(line_keys, sizeof(line_keys), "F2:FI  F3:QUALITY  F4:VSYNC  F6:DLSS");
    EmitText(28.0F, y, line_keys, body, 2.0F);
}

Result<float> OverlayRenderer::Draw(
    ID3D12GraphicsCommandList* command_list,
    const OverlaySnapshot& snapshot,
    const D3D12_CPU_DESCRIPTOR_HANDLE output_rtv,
    const std::uint32_t viewport_width,
    const std::uint32_t viewport_height) {
    common::Stopwatch timer{};
    BuildGeometry(snapshot);
    if (staging_vertices_.empty()) {
        return 0.0F;
    }

    std::memcpy(mapped_vertices_, staging_vertices_.data(), staging_vertices_.size() * sizeof(OverlayVertex));
    mapped_constants_->viewport_size[0] = static_cast<float>(viewport_width);
    mapped_constants_->viewport_size[1] = static_cast<float>(viewport_height);

    D3D12_VERTEX_BUFFER_VIEW vb_view{};
    vb_view.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
    vb_view.SizeInBytes = static_cast<UINT>(staging_vertices_.size() * sizeof(OverlayVertex));
    vb_view.StrideInBytes = sizeof(OverlayVertex);

    const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(viewport_width), static_cast<float>(viewport_height), 0.0F, 1.0F};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height)};

    command_list->SetGraphicsRootSignature(root_signature_.Get());
    command_list->SetPipelineState(pipeline_state_.Get());
    command_list->RSSetViewports(1U, &viewport);
    command_list->RSSetScissorRects(1U, &scissor);
    command_list->OMSetRenderTargets(1U, &output_rtv, FALSE, nullptr);
    command_list->SetGraphicsRootConstantBufferView(0U, constant_buffer_->GetGPUVirtualAddress());
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0U, 1U, &vb_view);
    command_list->DrawInstanced(static_cast<UINT>(staging_vertices_.size()), 1U, 0U, 0U);

    return static_cast<float>(timer.ElapsedMilliseconds());
}

}  // namespace chimera::overlay
