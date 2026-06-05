#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

TEST_CASE("D3D12 smoke can create a factory and WARP device", "[integration][d3d12]") {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory{};
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0U, IID_PPV_ARGS(factory.GetAddressOf()))));

    Microsoft::WRL::ComPtr<IDXGIAdapter> warp_adapter{};
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf()))));

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    REQUIRE(SUCCEEDED(D3D12CreateDevice(warp_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.GetAddressOf()))));
}
