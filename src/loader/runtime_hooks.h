#pragma once

#include <unknwn.h>
#include <dxgi1_6.h>
#include "common/result.h"

namespace chimera::loader::runtime {

[[nodiscard]] chimera::common::Result<void> InstallHooks();
void ShutdownHooks() noexcept;

HRESULT CallOriginalPresent(
    IUnknown* swap_chain,
    UINT sync_interval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* present_parameters);

}  // namespace chimera::loader::runtime
