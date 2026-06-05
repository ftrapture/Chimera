#include "capture/d3d12_capture.h"

#include <algorithm>
#include <sstream>

#include "common/log.h"

namespace chimera::capture {

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

}  // namespace

Result<void> D3D12FrameCapture::Initialize(ID3D12Device*, const DXGI_FORMAT capture_format) {
    format_ = capture_format;
    common::LogInfo("capture", "D3D12FrameCapture initialized.");
    return {};
}

Result<void> D3D12FrameCapture::EnsureResources(
    ID3D12Device* device,
    const std::uint32_t width,
    const std::uint32_t height,
    const DXGI_FORMAT format) {

    if (width == width_ && height == height_ && format == format_ && frames_[0].color) {
        return {};
    }

    width_ = width;
    height_ = height;
    format_ = format;
    captured_count_ = 0U;
    write_index_ = 0U;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.Format = format_;
    desc.SampleDesc.Count = 1U;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    for (auto& frame : frames_) {
        frame.color.Reset();
        frame.valid = false;
        CHIMERA_RETURN_IF_ERROR(HrCheck(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(frame.color.ReleaseAndGetAddressOf())),
            "CreateCommittedResource(capture)"));
    }

    return {};
}

Result<void> D3D12FrameCapture::CaptureFrame(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source_color,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint64_t frame_index) {

    if (!source_color) {
        return Status::Error(ErrorCode::kInvalidArgument, "CaptureFrame: null source.");
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    CHIMERA_RETURN_IF_ERROR(HrCheck(
        command_list->GetDevice(IID_PPV_ARGS(device.GetAddressOf())), "GetDevice(capture)"));
    CHIMERA_RETURN_IF_ERROR(EnsureResources(device.Get(), width, height, format_));

    auto& target = frames_[write_index_];

    // Transition target to copy dest
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.color.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1U, &barrier);

    // Copy
    command_list->CopyResource(target.color.Get(), source_color);

    // Transition back to SRV
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1U, &barrier);

    target.width = width;
    target.height = height;
    target.frame_index = frame_index;
    target.valid = true;

    write_index_ = (write_index_ + 1U) % kMaxCapturedFrames;
    captured_count_ = (std::min)(captured_count_ + 1U, kMaxCapturedFrames);

    return {};
}

const CapturedFrame& D3D12FrameCapture::GetFrame(const std::uint32_t frames_ago) const noexcept {
    static const CapturedFrame kInvalid{};
    if (frames_ago >= captured_count_) {
        return kInvalid;
    }
    const auto index = (write_index_ + kMaxCapturedFrames - 1U - frames_ago) % kMaxCapturedFrames;
    return frames_[index];
}

bool D3D12FrameCapture::HasPreviousFrame() const noexcept {
    return captured_count_ >= 2U;
}

void D3D12FrameCapture::Reset() noexcept {
    for (auto& frame : frames_) {
        frame.valid = false;
    }
    write_index_ = 0U;
    captured_count_ = 0U;
}

}  // namespace chimera::capture
