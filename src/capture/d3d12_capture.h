#pragma once

#include <array>
#include <cstdint>

#include <d3d12.h>
#include <wrl/client.h>

#include "common/result.h"

namespace chimera::capture {

static constexpr std::uint32_t kMaxCapturedFrames = 3U;

struct CapturedFrame final {
    Microsoft::WRL::ComPtr<ID3D12Resource> color{};
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint64_t frame_index{0U};
    bool valid{false};
};

class D3D12FrameCapture final {
public:
    [[nodiscard]] chimera::common::Result<void> Initialize(ID3D12Device* device, DXGI_FORMAT capture_format = DXGI_FORMAT_R8G8B8A8_UNORM);

    [[nodiscard]] chimera::common::Result<void> CaptureFrame(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* source_color,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t frame_index);

    [[nodiscard]] const CapturedFrame& GetFrame(std::uint32_t frames_ago) const noexcept;
    [[nodiscard]] bool HasPreviousFrame() const noexcept;
    void Reset() noexcept;

private:
    [[nodiscard]] chimera::common::Result<void> EnsureResources(
        ID3D12Device* device, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format);

    std::array<CapturedFrame, kMaxCapturedFrames> frames_{};
    std::uint32_t write_index_{0U};
    std::uint32_t captured_count_{0U};
    std::uint32_t width_{0U};
    std::uint32_t height_{0U};
    DXGI_FORMAT format_{DXGI_FORMAT_R8G8B8A8_UNORM};
};

}  // namespace chimera::capture
