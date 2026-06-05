#include "resource_inspection/signal_inspector.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace chimera::resource_inspection {

namespace {

using chimera::common::SignalSource;

[[nodiscard]] float Clamp01(const float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] bool IsDepthFormat(const DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool IsLikelyColorFormat(const DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool FormatFamilyMatches(const DXGI_FORMAT left, const DXGI_FORMAT right) noexcept {
    if (left == right) {
        return true;
    }

    const auto is_rgba8_family = [](const DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    };
    const auto is_bgra8_family = [](const DXGI_FORMAT format) {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    };

    return
        (is_rgba8_family(left) && is_rgba8_family(right)) ||
        (is_bgra8_family(left) && is_bgra8_family(right));
}

[[nodiscard]] bool DimensionsClose(const std::uint32_t left, const std::uint32_t right) noexcept {
    return std::abs(static_cast<int>(left) - static_cast<int>(right)) <= 4;
}

[[nodiscard]] bool DimensionsMatch(
    const ObservedDescriptor& descriptor,
    const std::uint32_t width,
    const std::uint32_t height) noexcept {
    return DimensionsClose(descriptor.width, width) && DimensionsClose(descriptor.height, height);
}

[[nodiscard]] float RecencyContribution(const std::uint64_t current_tick, const std::uint64_t last_seen_tick) noexcept {
    const auto delta = current_tick >= last_seen_tick ? current_tick - last_seen_tick : 0U;
    if (delta <= 4U) {
        return 0.12F;
    }
    if (delta <= 16U) {
        return 0.07F;
    }
    if (delta <= 64U) {
        return 0.03F;
    }
    return -0.12F;
}

[[nodiscard]] std::string FormatNote(
    const char* label,
    const std::uint32_t width,
    const std::uint32_t height) {
    std::ostringstream stream;
    stream << label << ' ' << width << 'x' << height;
    return stream.str();
}

}  // namespace

void SignalInspector::RegisterDescriptor(
    const D3D12_CPU_DESCRIPTOR_HANDLE handle,
    const std::uintptr_t resource_id,
    const ObservedDescriptor& descriptor) {
    if (handle.ptr == 0U || resource_id == 0U || descriptor.width == 0U || descriptor.height == 0U) {
        return;
    }

    std::scoped_lock lock(mutex_);
    descriptors_[handle.ptr] = DescriptorRecord{resource_id, descriptor};
}

void SignalInspector::ObserveRenderTargets(
    const std::uint32_t render_target_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* render_target_views,
    const bool single_handle_to_descriptor_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth_stencil_view,
    const std::uint32_t rtv_descriptor_increment_size) {
    std::scoped_lock lock(mutex_);
    ++observation_tick_;

    auto observe_handle = [&](const D3D12_CPU_DESCRIPTOR_HANDLE handle, const bool depth_view) {
        const auto descriptor_it = descriptors_.find(handle.ptr);
        if (descriptor_it == descriptors_.end()) {
            return;
        }

        auto& candidates = depth_view ? depth_candidates_ : color_candidates_;
        auto& candidate = candidates[descriptor_it->second.resource_id];
        candidate.descriptor = descriptor_it->second.descriptor;
        candidate.bind_count += 1U;
        candidate.last_seen_tick = observation_tick_;
    };

    if (render_target_views != nullptr) {
        for (std::uint32_t index = 0U; index < render_target_count; ++index) {
            auto handle = render_target_views[single_handle_to_descriptor_range ? 0U : index];
            if (single_handle_to_descriptor_range) {
                handle.ptr += static_cast<SIZE_T>(index) * static_cast<SIZE_T>(rtv_descriptor_increment_size);
            }
            observe_handle(handle, false);
        }
    }

    if (depth_stencil_view != nullptr) {
        observe_handle(*depth_stencil_view, true);
    }
}

InspectionSnapshot SignalInspector::Evaluate(
    const std::uint32_t display_width,
    const std::uint32_t display_height,
    const DXGI_FORMAT swapchain_format) const {
    std::scoped_lock lock(mutex_);

    InspectionSnapshot snapshot{};
    snapshot.suggested_render_width = display_width;
    snapshot.suggested_render_height = display_height;

    const auto current_tick = observation_tick_;
    const auto display_area = static_cast<float>(std::max<std::uint64_t>(1U, static_cast<std::uint64_t>(display_width) * static_cast<std::uint64_t>(display_height)));

    const CandidateRecord* best_subdisplay_color = nullptr;
    float best_subdisplay_score = -std::numeric_limits<float>::infinity();
    const CandidateRecord* best_display_color = nullptr;
    float best_display_score = -std::numeric_limits<float>::infinity();

    for (const auto& [resource_id, candidate] : color_candidates_) {
        (void)resource_id;
        const auto& descriptor = candidate.descriptor;
        const auto is_subdisplay = descriptor.width + 4U < display_width || descriptor.height + 4U < display_height;
        const auto area_ratio = static_cast<float>(descriptor.width) * static_cast<float>(descriptor.height) / display_area;

        float score = is_subdisplay ? 0.46F : 0.18F;
        if (area_ratio >= 0.25F && area_ratio <= 1.05F) {
            score += 0.12F;
        }
        if (area_ratio >= 0.45F && area_ratio <= 0.85F) {
            score += 0.10F;
        }
        if (IsLikelyColorFormat(descriptor.format)) {
            score += 0.08F;
        }
        if (FormatFamilyMatches(descriptor.format, swapchain_format)) {
            score += 0.07F;
        }
        if (descriptor.sample_count == 1U) {
            score += 0.05F;
        } else {
            score -= 0.08F;
        }
        if (candidate.bind_count >= 4U) {
            score += 0.05F;
        }
        score += RecencyContribution(current_tick, candidate.last_seen_tick);

        if (is_subdisplay) {
            score = (std::min)(score, 0.92F);
            if (score > best_subdisplay_score) {
                best_subdisplay_score = score;
                best_subdisplay_color = &candidate;
            }
        } else {
            score = (std::min)(score, 0.72F);
            if (score > best_display_score) {
                best_display_score = score;
                best_display_color = &candidate;
            }
        }
    }

    const auto* chosen_color = best_subdisplay_color != nullptr ? best_subdisplay_color : best_display_color;
    if (best_subdisplay_color != nullptr) {
        snapshot.suggested_render_width = best_subdisplay_color->descriptor.width;
        snapshot.suggested_render_height = best_subdisplay_color->descriptor.height;
        snapshot.signals.color = {
            Clamp01(best_subdisplay_score),
            true,
            best_subdisplay_score >= 0.80F,
            SignalSource::kIntercepted,
            FormatNote("pre-upscale candidate", best_subdisplay_color->descriptor.width, best_subdisplay_color->descriptor.height)};
    } else {
        snapshot.signals.color = {
            1.0F,
            true,
            true,
            SignalSource::kIntercepted,
            "swapchain final"};
    }

    const CandidateRecord* best_depth = nullptr;
    float best_depth_score = -std::numeric_limits<float>::infinity();
    for (const auto& [resource_id, candidate] : depth_candidates_) {
        (void)resource_id;
        const auto& descriptor = candidate.descriptor;

        float score = 0.38F;
        if (descriptor.is_depth || IsDepthFormat(descriptor.format)) {
            score += 0.18F;
        }
        if (DimensionsMatch(descriptor, snapshot.suggested_render_width, snapshot.suggested_render_height)) {
            score += 0.22F;
        } else if (DimensionsMatch(descriptor, display_width, display_height)) {
            score += 0.08F;
        }
        if (candidate.bind_count >= 4U) {
            score += 0.05F;
        }
        score += RecencyContribution(current_tick, candidate.last_seen_tick);
        score = (std::min)(score, 0.86F);

        if (score > best_depth_score) {
            best_depth_score = score;
            best_depth = &candidate;
        }
    }

    if (best_depth != nullptr) {
        snapshot.signals.depth = {
            Clamp01(best_depth_score),
            true,
            chosen_color != nullptr && DimensionsMatch(best_depth->descriptor, snapshot.suggested_render_width, snapshot.suggested_render_height),
            SignalSource::kIntercepted,
            FormatNote("depth candidate", best_depth->descriptor.width, best_depth->descriptor.height)};
    } else {
        snapshot.signals.depth = {0.0F, false, false, SignalSource::kMissing, "missing"};
    }

    snapshot.signals.motion = {0.0F, false, false, SignalSource::kMissing, "not discovered"};
    snapshot.signals.jitter = {0.0F, false, false, SignalSource::kMissing, "not discovered"};
    snapshot.signals.exposure = {0.25F, false, false, SignalSource::kMissing, "unknown"};
    snapshot.signals.ui = {0.25F, false, false, SignalSource::kDerived, "not separated"};
    return snapshot;
}

void SignalInspector::Reset() noexcept {
    std::scoped_lock lock(mutex_);
    descriptors_.clear();
    color_candidates_.clear();
    depth_candidates_.clear();
    observation_tick_ = 0U;
}

}  // namespace chimera::resource_inspection
