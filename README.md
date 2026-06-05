# Project Chimera: Neural Temporal Super Resolution & Frame Interpolation Runtime

Project Chimera is a user-mode Windows runtime designed to bring Neural Temporal Super Resolution (NTSR) and Guarded Frame Interpolation (FI) capabilities to DirectX 12 (with Vulkan support on the active roadmap). Written in C++23, it features an analytical temporal baseline with budget-aware neural refinement passes, optimized compute shaders, and a robust launcher-based injection flow for offline validation.

> [!NOTE]
> **Project Status (Vulkan Roadmap)**: Vulkan support is scheduled for Phase 9 of the development milestones (see [project_chimera.md](file:///e:/projects/DLSS/docs/architecture/project_chimera.md)). The `src/vulkan` directory is currently an empty placeholder; DirectX 12 is the only active and fully implemented graphics backend in the current release.

The design philosophy is centered around safe attachment, performance predictability, and graceful degradation.

---

## Key System Objectives & Constraints

* **User-Mode Only Execution:** No kernel drivers, anti-cheat evasion, or DRM-bypass mechanisms.
* **Opt-In Policy Verification:** Utilizes a strict verification gate (`PolicyGate`) that blocks injection into store launchers, protected processes, and titles with active anti-cheat integrations.
* **Tiered Capability Degradation:** Refuses to apply unstable algorithms if the input signal confidence is low. Automatically falls back from temporal super-resolution to spatial upscaling when required motion vectors, depth buffers, or camera jitter signals are missing or malformed.
* **Deterministic Performance:** Employs an analytic temporal baseline first. Neural refinement residuals run as optional, budget-bounded compute passes to guarantee a stable frame pacing profile.

---

## System Architecture

```
Launcher / Stub CLI 
       │
       ▼ (Suspended CreateProcess)
   PolicyGate  ──────[Refuse Unsafe Targets]──────► Abort
       │
       ▼ (Inject chimera_runtime.dll)
  AttachSession ◄─────[Propagates to Child Processes via IAT Hooks]
       │
       ▼ (Resumes thread -> GetCommandLine IAT Redirection)
  MinHook Hooking Engine (D3D12 / DXGI)
       │
       ├─► Device Context & Swapchain Proxy
       │
       ├─► Command List Capture & Pipeline Interception
       │
       ▼
  Resource Inspection & Signal Inspector
       │
       ├─► Evaluates Color/Depth bind history, resolution, format families
       ├─► Calculates signal confidence (Cs)
       │
       ▼
  TierDecision & FrameGraph Dispatcher
       │
       ├─► Tier A (Full Engine Signals)   ──► Temporal SR + Guarded FI
       ├─► Tier B (Intercepted Signals)  ──► Temporal SR + Pacing/UI-Checked FI
       └─► Tier C (Spatial Fallback)     ──► Spatial Upscale + FI Disabled
```

---

## Capability Tiers & Signal Confidence

To adapt to varying levels of engine integration and signal availability, Project Chimera operates under a structured tiering system:

### 1. Capability Tiers
* **Tier A (Direct Engine Integration):** The target process provides native high-quality color buffers, depth buffers, motion vectors, sub-pixel jitter offsets, exposure scalars, and transparency/reactive masks. Full temporal super-resolution and guarded frame interpolation are active.
* **Tier B (Intercepted Engine Runtime):** Signals are discovered dynamically at the API level. Features confidence-gated Temporal SR. Frame interpolation is only allowed if the user interface (UI) can be separated and frame pacing remains stable.
* **Tier C (Spatial Upscale Fallback):** Activated when motion or depth signals are absent or fall below confidence thresholds. The runtime drops back to spatial upscaling (bilinear filtering + Contrast Adaptive Sharpening) and disables frame interpolation entirely.

### 2. Signal Confidence Calculation
Per-signal confidence ($C_s$) is evaluated continuously to detect camera cuts, screen transitions, and UI overlays:

$$C_s = \text{clamp}\left(\sum (w_i \cdot f_i) - \text{penalties}, 0, 1\right)$$

Where $w_i$ represents the weight of signal features (e.g., format compatibility, binding frequency, temporal coherence) and $f_i$ represents the feature status. 

* **Temporal SR Activation Thresholds:** Color $\ge 0.90$, Depth $\ge 0.75$, Motion $\ge 0.75$, Jitter $\ge 0.60$.
* **Frame Interpolation Activation Thresholds:** Color $\ge 0.95$, UI Separation $\ge 0.70$, Depth $\ge 0.80$, Fused Motion/Optical Flow $\ge 0.80$, with stable frame pacing and no active scene changes (menu, pause screen, cutscenes).

---

## Algorithmic Pipeline & Mathematical Model

The upscaling and interpolation pipeline is built as a custom GPU FrameGraph executing via Direct3D 12 compute pipelines (Vulkan support is planned for a future milestone).

### 1. Temporal Reprojection
For each display-space pixel coordinate $x_{\text{cur}}$, the corresponding coordinate in the history buffer $x_{\text{prev}}$ is calculated by subtracting the pixel-space motion vector and adjusting for sub-pixel camera jitter:

$$x_{\text{prev}} = x_{\text{cur}} - \vec{v}_{\text{motion}}(x_{\text{cur}}) - (j_t - j_{t-1})$$

Where:
* $\vec{v}_{\text{motion}}$ is the motion vector scaled to pixel-space.
* $j_t$ and $j_{t-1}$ are the camera jitter offsets at frames $t$ and $t-1$.

### 2. Depth-Based Disocclusion
To prevent ghosting artifacts behind moving objects, disocclusion regions are flagged by comparing reprojected depth values against current depth buffer samples:

$$\text{disoccluded} = \left| z_{\text{prev}}(x_{\text{prev}}) - z_{\text{cur}}(x_{\text{cur}}) \right| > \tau_z(z_{\text{cur}})$$

A 3x3 nearest-depth dilation operator is applied to depth and motion vector inputs to dilate object silhouettes, ensuring thin geometry boundaries are fully covered.

### 3. History Accumulation
Current frame color ($C_{\text{cur}}$) and reprojected history ($H_{\text{prev}}$) are accumulated using an adaptive blending factor $\alpha$:

$$H_{\text{clamped}} = \text{clamp}(H_{\text{prev}}(x_{\text{prev}}), N_{\text{min}}(C_{\text{cur}}), N_{\text{max}}(C_{\text{cur}}))$$

$$C_{\text{out}} = \text{lerp}(C_{\text{cur}}, H_{\text{clamped}}, \alpha \cdot \text{confidence} \cdot (1 - \text{reactive}) \cdot (1 - \text{disoccluded}))$$

Where:
* $N_{\text{min}}, N_{\text{max}}$ are the local neighborhood luma/chroma bounds (neighborhood clamping).
* $\text{reactive}$ represents the transparency/particle mask to prevent trailing on alpha-blended objects.

---

## Hooking & Attachment Lifecycle

To attach to target binaries safely without causing deadlocks (e.g., Loader Lock during `DllMain`), Project Chimera splits its loading process:

```
[DllMain (DLL_PROCESS_ATTACH)]
        │
        ▼
Patch Target EXE's Import Address Table (IAT) for GetCommandLineW / GetCommandLineA
        │
        ▼ (No MinHook initialized yet. Extremely safe, fast, and lock-free)
[Target Application Resumes]
        │
        ▼
Target main thread calls GetCommandLineW/A (CRT startup)
        │
        ▼
Redirected to Detour -> Call MH_Initialize() & Install Hooks (D3D12 / DXGI)
        │
        ▼
MinHook runs safely outside of Loader Lock context on the running main thread
```

### Injection Propagation in Launcher
For wrapper EXEs that launch child shipping processes and exit immediately, the `chimera_launcher` uses IAT hooks on `CreateProcessW` and `CreateProcessA`.
1. The launcher hooks the process creation APIs of the parent wrapper.
2. When the wrapper spawns the game binary, the hook intercepts the call, forces the `CREATE_SUSPENDED` flag on the child process, and injects `chimera_runtime.dll` into the child's address space.
3. The hook performs the IAT patching of `GetCommandLineW/A` in the child process, then calls `ResumeThread` to safely execute the target.

---

## Machine Learning Integration

The ML module is designed around standard inference runtimes (TensorRT and ONNX Runtime) operating on half-precision float tensors (FP16) with compute shader fallbacks.

### 1. Neural Super Resolution (SR) Residual
* **Network Input Shape:** `[1, 15, Hd, Wd]` (consisting of Low-Res HDR Color, Depth, Pixel-Space Motion Vectors, History Luma, Exposure, and Reactive Mask).
* **Recurrent Hidden State:** `[1, 8, Hd/2, Wd/2]` (provides temporal context tracking across frames).
* **Network Output Shape:** RGB residual + updated recurrent state.
* **Behavior:** The neural network infers high-frequency details and temporal blending weights, adding a residual offset to the analytical TSR baseline output.

### 2. Neural Frame Interpolation (FI) Repair
* **Network Input Shape:** `[1, 14, Hd, Wd]` (consisting of bidirectional optical flow, reprojected color frames, occlusion masks, and UI overlays).
* **Network Output Shape:** RGB repair residual + confidence delta.
* **Behavior:** Corrects warping artifacts and disocclusion holes in the analytic warped frame before final UI composition.

---

## Directory Structure

```
├─ docs/                     # Architectural documents and design specifications
├─ shaders/                  # HLSL Compute Shaders
│  ├─ sr/                    # Temporal Super Resolution passes (reprojection, clamping, blending)
│  ├─ fg/                    # Frame Interpolation passes (warping, splatting, repair)
│  └─ debug/                 # HUD telemetry and diagnostic visualizers
├─ src/                      # C++ source code
│  ├─ common/                # Shared interfaces, structured logger, environment parsers
│  ├─ loader/                # Launcher, PolicyGate, Injection utilities, and Runtime IAT detour entries
│  ├─ capture/               # Swapchain wrappers and graphics command list hooks
│  ├─ resource_inspection/   # Dynamic signal discovery and frame history tracking
│  ├─ framegraph/            # Command list execution wrappers, resource pool, barriers
│  ├─ sr/                    # Analytical Temporal Super Resolution logic
│  ├─ fg/                    # Frame Interpolation, motion warping, and occlusion solvers
│  ├─ ml/                    # TensorRT / ONNX inference wrappers
│  ├─ ofa/                    # Optical Flow Acceleration (NVIDIA OFA & Compute Fallback)
│  ├─ pacing/                # Latency governor, present schedulers, and queue monitors
│  ├─ overlay/               # Diagnostic UI telemetry renderer (DirectX 12 / Direct2D)
│  └─ platform/              # OS-dependent process and memory helper utilities
├─ tests/                    # Unit, integration, and image quality test harnesses
├─ tools/                    # Benchmarking, capture playback, and dataset extraction utilities
├─ third_party/              # Managed dependencies (MinHook, JSON, Catch2, etc.)
└─ CMakeLists.txt            # Main CMake project configuration file
```

---

## Getting Started

### Prerequisites
* **OS:** Windows 10 / 11 (64-bit)
* **Compiler:** Visual Studio 2022 (v143 toolset) or MSVC with C++23 support
* **SDKs:** Windows 10 SDK (10.0.22621.0 or newer)
* **Package Manager:** `vcpkg` for dependency resolution

### Building the Project
1. Clone the repository and configure dependencies via `vcpkg`:
   ```powershell
   git clone https://github.com/your-username/ProjectChimera.git
   cd ProjectChimera
   ```
2. Configure CMake and build the project in Release mode:
   ```powershell
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"
   cmake --build build --config Release
   ```

### Running the Sample Harness
To run the in-engine D3D12 sample scene with TSR and Frame Interpolation active:
```powershell
.\build\Release\chimera_sample_app.exe
```

### Launching and Injecting into Offline Titles
To inject the Chimera runtime into a validated offline executable:
```powershell
.\build\Release\chimera_launcher.exe --offline --cwd "D:\Path\To\Game" "D:\Path\To\Game\GameLauncher.exe"
```
* Use the `--offline` flag to confirm you are targeting a self-owned or offline build.
* Use the `--stream-log` option to stream runtime telemetry straight to your terminal.

---

## Profiling & Offline Validation Tools

Project Chimera includes a set of tools to validate rendering quality and profile GPU execution times:

### 1. Dataset Generator (`chimera_dataset_gen`)
Generates paired native-resolution ground truth frames and low-resolution jittered color/depth/motion buffers to train and calibrate neural models:
```powershell
.\build\Release\chimera_dataset_gen.exe --out "./dataset_output" --frames 1000
```

### 2. Capture and Replay (`chimera_capture_replay`)
Records raw D3D12 command lists and buffers during runtime, allowing developers to replay rendering graphs offline for reproducible testing:
```powershell
# Replay captured frames and generate metrics
.\build\Release\chimera_capture_replay.exe --input "./captured_frames.bin"
```

### 3. Benchmark (`chimera_benchmark`)
Measures frame execution timings, memory overhead, present latency, and pacing stability:
```powershell
.\build\Release\chimera_benchmark.exe --runs 500
```

---

## Diagnostic Overlay Controls

While injected, the runtime displays a diagnostic telemetry HUD. The overlay is bound to the following hotkeys by default:

* **F2:** Toggle Frame Interpolation (ON/OFF).
* **F3:** Toggle Super-Resolution quality mode (UltraQuality / Quality / Balanced / Performance).
* **F4:** Toggle V-Sync pacing governoring.

---

## Development Guidelines & Security Boundaries

Project Chimera is built to ensure a clear distinction between graphics modding and anti-cheat tampering. Please adhere to the following guidelines:

1. **Target Validation:** Ensure the `PolicyGate` registry remains updated. We do not support, endorse, or permit attachment to competitive online environments.
2. **Resource Integrity:** Do not attempt to hook system DLLs beyond target IAT blocks and the device interface creation wrappers (`d3d12.dll`/`dxgi.dll`).
3. **Graceful Detach:** All runtime hooks must be unregistered on detach, returning modified virtual memory pages to their original protection flags and state.
