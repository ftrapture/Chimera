#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <d3d12.h>

#include "resource_inspection/signal_inspector.h"

namespace chimera::resource_inspection {
namespace {

ObservedDescriptor MakeColorDescriptor(const std::uint32_t width, const std::uint32_t height) {
    return ObservedDescriptor{width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1U, false};
}

ObservedDescriptor MakeDepthDescriptor(const std::uint32_t width, const std::uint32_t height) {
    return ObservedDescriptor{width, height, DXGI_FORMAT_D32_FLOAT, 1U, true};
}

}  // namespace

TEST_CASE("Signal inspector prefers sub-display color and matching depth", "[resource_inspection]") {
    SignalInspector inspector{};

    const D3D12_CPU_DESCRIPTOR_HANDLE color_handle{1000U};
    const D3D12_CPU_DESCRIPTOR_HANDLE depth_handle{2000U};
    inspector.RegisterDescriptor(color_handle, 1U, MakeColorDescriptor(1280U, 720U));
    inspector.RegisterDescriptor(depth_handle, 2U, MakeDepthDescriptor(1280U, 720U));

    for (int index = 0; index < 6; ++index) {
        inspector.ObserveRenderTargets(1U, &color_handle, false, &depth_handle, 1U);
    }

    const auto snapshot = inspector.Evaluate(1920U, 1080U, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(snapshot.suggested_render_width == 1280U);
    CHECK(snapshot.suggested_render_height == 720U);
    CHECK(snapshot.signals.color.available);
    CHECK(snapshot.signals.color.confidence > 0.75F);
    CHECK(snapshot.signals.depth.available);
    CHECK(snapshot.signals.depth.confidence > 0.70F);
    CHECK_THAT(snapshot.signals.color.note, Catch::Matchers::ContainsSubstring("pre-upscale candidate"));
}

TEST_CASE("Signal inspector falls back to swapchain color without a sub-display candidate", "[resource_inspection]") {
    SignalInspector inspector{};

    const D3D12_CPU_DESCRIPTOR_HANDLE color_handle{3000U};
    inspector.RegisterDescriptor(color_handle, 11U, MakeColorDescriptor(1920U, 1080U));
    inspector.ObserveRenderTargets(1U, &color_handle, false, nullptr, 1U);

    const auto snapshot = inspector.Evaluate(1920U, 1080U, DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(snapshot.suggested_render_width == 1920U);
    CHECK(snapshot.suggested_render_height == 1080U);
    CHECK(snapshot.signals.color.available);
    CHECK(snapshot.signals.color.validated);
    CHECK(snapshot.signals.color.confidence > 0.99F);
    CHECK(snapshot.signals.depth.available == false);
    CHECK_THAT(snapshot.signals.color.note, Catch::Matchers::ContainsSubstring("swapchain"));
}

}  // namespace chimera::resource_inspection
