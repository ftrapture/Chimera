# Project Chimera

## 1. Executive Summary

Project Chimera is a user-mode-only Windows runtime for Neural Temporal Super Resolution (NTSR) and optional Frame Interpolation (FI) targeting RTX 30-series GPUs. The public baseline ships analytic temporal super resolution first, adds guarded analytic frame interpolation second, and treats neural refinement as an optional budget-aware enhancement instead of a prerequisite.

The repo is explicitly designed around safe attachment and graceful degradation:

- No anti-cheat evasion, DRM bypass, protected-process tampering, kernel components, stealth injection, or claims of universal title compatibility.
- Opt-in launcher/API flow only.
- D3D12 first, Vulkan second.
- Tiered capability model that refuses unsafe targets and reduces features when signal confidence is weak.

## 2. Reality Constraints And Compatibility Tiers

### Safety Rules

- User-mode only.
- PolicyGate refuses protected processes, anti-cheat, kernel antitamper, and legally uncertain targets.
- Offline/open-source/self-owned harnesses are the primary validation path.
- The runtime never claims official DLSS compatibility; public naming is NTSR and FI.

### Capability Tiers

- Tier A
  - Direct engine/plugin/sample-harness inputs.
  - Full color, depth, motion, jitter, exposure, reactive/transparency, HUD-less scene or separate UI.
  - Temporal SR and guarded FI both allowed.
- Tier B
  - Intercepted D3D12/Vulkan title with strong color/depth plus validated motion or optical flow.
  - Temporal SR allowed with confidence gates.
  - FI allowed only when UI separation, pacing, and scene-state checks all pass.
- Tier C
  - Color-only or weak-signals fallback.
  - Spatial upscale only.
  - TAA-lite only when jitter is stable.
  - FI disabled by default.

### Signal Confidence Model

Per-signal confidence is computed as:

`C_s = clamp(sum(w_i * f_i) - penalties, 0, 1)`

Features include format, dimensions, cadence, binding history, pipeline stage, temporal stability, camera correlation, and visual validation.

Hard caps:

- Motion estimated from image heuristics: `<= 0.60` until validated.
- Inferred jitter: `<= 0.70`.
- UI extraction fallback: `<= 0.65`.

Temporal SR gate:

- `color >= 0.90`
- `depth >= 0.75`
- `motion >= 0.75`
- `jitter >= 0.60`

FI gate:

- `color >= 0.95`
- `ui >= 0.70`
- `depth >= 0.80`
- `fused motion/flow >= 0.80`
- stable pacing
- not menu/loading/pause/cutscene

## 3. System Architecture

```text
Launcher/API -> PolicyGate -> AttachSession -> BackendCapture(D3D12/VK)
             -> ResourceInspection + SignalConfidence -> TierDecision
             -> FrameGraph
                -> SR Prep (jitter/mvec/depth/reactive/exposure/history)
                -> Analytic NTSR
                -> Optional Neural SR Residual
                -> Optional OFA / Compute Optical Flow
                -> Analytic FI (warp+splat/backward warp, occlusion, repair)
                -> Optional Neural FI Repair
             -> UI Composition -> PresentScheduler/LatencyGovernor
             -> Overlay + Telemetry + CaptureReplay
```

### Frame Ownership

- Loader owns attach/detach lifecycle and policy enforcement.
- Capture owns API wrapping and extraction of frame inputs.
- Resource inspection owns scoring and validation of candidate signals.
- Framegraph owns transient lifetime, barriers, queue overlap, and persistent history.
- SR owns temporal reconstruction and sharpening.
- FG owns interpolation, occlusion, repair, and UI-safe composition.
- ML owns model registry plus inference backend selection.
- Pacing owns present timing and FI enable/disable policy.

## 4. Repo Layout

```text
/docs
/src/common
/src/loader
/src/d3d12
/src/vulkan
/src/capture
/src/resource_inspection
/src/framegraph
/src/sr
/src/fg
/src/ml
/src/ofa
/src/pacing
/src/overlay
/src/config
/src/platform/win32
/shaders/include
/shaders/sr
/shaders/fg
/shaders/debug
/tools/capture_replay
/tools/benchmark
/tools/dataset_gen
/tests/unit
/tests/integration
/tests/image_quality
/third_party
```

## 5. Detailed Module Specs

### Common

- Result/status types.
- Structured logging.
- Runtime contracts and capability-tier evaluation.
- Ring buffers and timing helpers.

### Loader

- `PolicyDecision`, `AttachConfig`, attach session lifecycle.
- User-mode only, opt-in launcher started flow.
- Clean refusal and clean unload are mandatory outcomes.

### D3D12

- First shipping backend.
- Sample harness for tranche 1.
- Runtime device/swapchain helpers, sample low-res scene generation, SR invocation, overlay composition.

### Resource Inspection

- Scores signal candidates, enforces confidence caps, and emits tier decisions.
- Never promotes guessed motion before validation.

### SR

- Analytic temporal baseline first.
- History management, jitter support, disocclusion, reactive handling, neighborhood clamp, optional sharpening.
- Neural residual pass only if model is available and under budget.

### FG

- Motion fusion, occlusion, interpolation, repair, and UI-safe composition.
- FI exits early on unstable pacing or weak scene-state confidence.

### ML

- ONNX export path, TensorRT runtime, FP16 first, optional INT8 later.
- Analytic fallback is always available and preferred if neural cost exceeds budget.

### Overlay And Telemetry

- Current tier and why.
- Render/display resolution.
- SR/FI mode.
- Stage times.
- Confidence bars.
- History reset and artifact warnings.

## 6. Algorithms And Math

### Temporal Reprojection

`x_prev = x_cur - mv(x_cur) - (j_t - j_{t-1})`

Where:

- `x_cur` is display-space pixel center.
- `mv` is motion vector at current sample.
- `j_t` and `j_{t-1}` are current and previous jitter offsets in display-space pixels.

### Motion-Vector Convention

- Engine motion vectors may arrive in NDC `[-1, 1]` or pixels.
- Chimera stores a per-title `motionVectorScale`.
- Pixel-space conversion:
  - `mv_pixels = mv_raw * motionVectorScale * render_resolution`
- NDC conversion:
  - `mv_pixels = 0.5 * mv_raw * render_resolution`

### Depth-Based Disocclusion

`disocc = abs(z_prev(x_prev) - z_cur(x_cur)) > tau_z(z_cur)`

With:

- nearest-depth 3x3 dilation for depth/motion
- edge detection and shading-change assists
- hard rejection on camera cuts or jitter discontinuities

### History Accumulation

`Hc = clamp(H_prev(x_prev), nmin(C_cur), nmax(C_cur))`

`C_out = lerp(C_cur, Hc, alpha * hist_conf * (1 - reactive) * (1 - disocc))`

History is rejected or reduced when:

- lock status is invalid
- neighborhood clamp fails
- reactive/transparency mask is high
- scene-change or camera-cut heuristics trigger

### Reactive/Transparency Handling

- Reactive masks bias weight toward the current frame for particles and alpha content.
- Transparency/composition masks lower history lock strength without globally disabling temporal reuse.

### Optical-Flow Fusion

`v_fused = (c_mv * v_mv + c_of * v_of) / (c_mv + c_of + eps)`

Low-confidence regions blend back to the nearest real frame or analytic repair only.

### FI Confidence

`c_fi = c_mv * c_of * (1 - disocc) * (1 - ui_instability) * (1 - scene_change)`

### Frame Pacing Model

`L_total ~= L_game + L_queue + L_present + L_fi`

FI is disabled when queue growth, latency, or cadence variance exceeds policy.

### Neural Tensor Shapes

SR refinement baseline:

- Inputs: `[1, 15, Hd, Wd]`
- Recurrent state: `[1, 8, Hd/2, Wd/2]`
- Outputs: RGB residual + updated recurrent state

FI repair baseline:

- Inputs: `[1, 14, Hd, Wd]`
- Outputs: RGB residual + confidence delta

Normalization:

- color in scene-linear
- motion normalized to `[-1, 1]`
- depth, occlusion, reactive, confidence in `[0, 1]`
- exposure as scalar or log-luma feature

## 7. Training And Inference Plan

### Dataset Generation

- Open-source engines and in-repo renderer bridge.
- Native-resolution ground truth plus low-resolution jittered inputs.
- Export depth, motion, exposure, reactive masks, HUD-less scene, and UI layers.
- Include hard cases: foliage, particles, transparencies, thin geometry, motion blur, dynamic resolution, camera cuts, HDR, low light, animated UI.

### Training Stages

- Analytic baseline first and kept as shipping fallback.
- Train SR residual model against analytic SR output plus native reference.
- Train FI repair/confidence model against analytic interpolation plus real in-between frames.

### Runtime Inference

- ONNX export checked into the tool path.
- TensorRT FP16 engines built per-GPU class where feasible.
- Analytic fallback triggers if model load fails or predicted cost exceeds budget.

## 8. Runtime Integration Plan

### D3D12 First

- Opt-in launcher starts target suspended.
- PolicyGate vets target.
- Injection uses documented user-mode loading only.
- Runtime wraps device creation, swapchain, submissions, and present.
- Clean unload drains presents, fences queues, frees pooled resources, and unregisters hooks.

### Vulkan Second

- Explicit opt-in layer only.
- Intercept `vkCreateInstance`, `vkCreateDevice`, submissions, and `vkQueuePresentKHR`.
- Prefer timeline semaphores, fall back to binary semaphores.

## 9. Frame Pacing And Latency Strategy

- Only insert FI when base fps and frame-time jitter are within policy.
- Predict next VBlank or VRR slot.
- Maintain stable frame-index semantics between real and generated frames.
- Drop generated frames before corrupting cadence.
- Disable FI on queue growth, latency breach, or pacing instability.

## 10. Debug And Telemetry Plan

Overlay shows:

- capability tier
- resource discoveries and confidence
- internal vs display resolution
- SR/FI mode
- per-stage timings
- pacing stats
- history resets
- artifact warnings

Telemetry outputs:

- structured logs
- benchmark CSV/JSON
- PSNR, SSIM, LPIPS, temporal stability
- frame time, P99, VRAM, power if available

## 11. Milestone Plan With Acceptance Tests

### Phase 0

- Files
  - architecture doc, CMake bootstrap, vcpkg manifest, logging/config runtime, D3D12 sample harness
- Acceptance
  - configure/build on VS2022
  - logging/config smoke tests pass
  - sample harness opens a D3D12 window and presents

### Phase 1

- Files
  - capture path, signal inspector, overlay renderer
- Acceptance
  - self-owned app capture replay works
  - overlay shows resources and confidence
  - no GPU validation errors

### Phase 2

- Files
  - analytic TSR, history logic, reprojection/accumulation shaders
- Acceptance
  - 67% render-scale TSR beats bilinear+CAS in harness
  - camera-cut reset works
  - stage time meets 3070 target

### Phase 3

- Files
  - launcher, attach session, policy gate, D3D12 wrappers
- Acceptance
  - attaches to unprotected offline D3D12 title
  - refuses protected or anti-cheat targets
  - unload leaves no leaked refs

### Phase 4

- Files
  - confidence model, validators, title profile system
- Acceptance
  - bad-input scenarios demote tiers
  - guessed motion never trusted pre-validation

### Phase 5

- Files
  - NVOFA provider, analytic FI, occlusion logic
- Acceptance
  - OFA and compute-flow fallback both run
  - FI never activates on menus/loading/cutscenes

### Phase 6

- Files
  - present scheduler, latency governor, UI compositor
- Acceptance
  - stable VRR and non-VRR pacing
  - P99 present jitter stays under policy

### Phase 7

- Files
  - SR model wrapper, TensorRT backend, dataset/benchmark tooling
- Acceptance
  - neural SR beats analytic baseline on LPIPS and temporal stability without budget regression

### Phase 8

- Files
  - FI model wrapper, neural repair path
- Acceptance
  - repair network improves hole and occlusion cases

### Phase 9

- Files
  - Vulkan layer/capture/swapchain proxy
- Acceptance
  - Vulkan sample reaches Tier A/B with clean sync validation

### Phase 10

- Files
  - renderer bridge, ONNX export, training configs
- Acceptance
  - reproducible dataset and export pipeline

### Phase 11

- Files
  - compatibility DB, regression harness, IQ baselines
- Acceptance
  - automated per-title regression from capture replays

### Phase 12

- Files
  - packaging scripts, launcher polish, final docs
- Acceptance
  - release artifacts produced
  - graceful failure paths verified

## 12. First Implementation Tranche

Tranche 1 creates:

- build/bootstrap files
- common runtime contracts
- config loader and logging
- Win32 timing/process helpers
- D3D12 sample harness
- analytic TSR scaffold with history and sharpening
- debug overlay renderer
- unit and integration smoke tests

The public baseline in this tranche does not implement:

- process attachment
- interception hooks
- NVOFA
- TensorRT
- Vulkan capture
- frame interpolation

Those are deliberately deferred rather than faked.
