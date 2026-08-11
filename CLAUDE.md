# CLAUDE.md

## Project Overview

Real-time 3D Chinese Chess (Xiangqi) visualization — **C++23**, **Vulkan 1.4**. Dual rendering: forward MSAA rasterization + path tracing (NEE direct lighting, Lambertian indirect bounce, Russian roulette, temporal accumulation). Slang shaders, ImGui UI (docking + multi-viewport), OpenColorIO ACES 2.0.

Chinese docs: `README_CN.md`.

> Scanned against the local working tree on 2026-08-08, including the uncommitted CMake migration and the scene_light flat-struct refactor. Do not assume `.slnx`/`.vcxproj` files exist — they were deleted in the working tree.

## Build

- **Platform**: Windows x64, Visual Studio 2022 (v143+), **CMake (requires ≥ 4.0) + Ninja + vcpkg manifest mode**
- Configure: `cmake --preset x64-debug` or `x64-release` (presets only define `configurePresets`; build with `cmake --build out/build/<preset>`)
- Open the folder in VS 2022 and pick the preset, or use the developer command line
- **Binary**: `out\build\x64-release\chinese-chess.exe`; working directory must be `chinese-chess/` (relative resource paths; CMake sets `DEBUGGER_WORKING_DIRECTORY`)
- **vcpkg**: local submodule (`vcpkg/`), toolchain `vcpkg/scripts/buildsystems/vcpkg.cmake`, manifest `vcpkg.json`, dynamic linking
- **C++23**: `target_compile_features(cxx_std_23)` (existing `out/build` scripts use `-std:c++latest /utf-8 /W4 /permissive-`)
- **Preprocessor defines**: `VK_USE_PLATFORM_WIN32_KHR`, `GLFW_INCLUDE_VULKAN`, `GLM_FORCE_RADIANS`, `GLM_ENABLE_EXPERIMENTAL`, `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `NOMINMAX`, `UNICODE`, `_UNICODE`
- **Include path**: `chinese-chess/` (project root; all includes relative to it)
- `/utf-8` is set via `add_compile_options(/utf-8)` under `if(WIN32)` in `CMakeLists.txt`; sources are BOM-less UTF-8 with Chinese literals/comments.

## Architecture

Four layers:

```
window/          GLFW window + entry point (main.cpp)
vulkan_core/     Vulkan 1.4 RAII wrappers — no render pass objects, dynamic rendering only
scene/           Scene graph — models, materials, lights, camera, dual render paths
ui/              ImGui panels (docking, proxy pattern), 4 sub-panels
tools/           Utilities: shader compiler, model loader, image I/O, OCIO, font, record parser, file watcher, string helper, RenderDoc capture
```

### Source directory summary

| Dir | Files | Key classes / roles |
|-----|-------|---------------------|
| `window/` | 2 cpp, 1 h | `glfw_window`: GLFW RAII, frame loop, input dispatch, screenshot, RenderDoc capture (debug) |
| `vulkan_core/` | 15 pairs | `vulkan_application` (orchestrator), `vulkan_device` (RAII), `vulkan_physical_device` (C++23 `cartesian_product` QF selection), `vulkan_swapchain` (Mailbox/FIFO, per-image binary semaphores), `vulkan_buffer`/`vulkan_image` (VMA + state tracking), `vulkan_commandbuffer` (timeline semaphore integration), `vulkan_queue`, `vulkan_semaphore` (timeline + atomic CPU counter), `vulkan_recycle_bin` (deferred destroy), `vulkan_pipeline` (graphics + RT), `vulkan_descriptor` (multi-frame, variant-based writes), `vulkan_acceleration_structure` (BLAS/TLAS), `vulkan_shader_binding_table` |
| `scene/` | 10 pairs | `scene_manager` (orchestrator, F5 hot-reload, save_image QFOT), `scene_camera`, `scene_light` (flat struct, `light_type` discriminator), `scene_material`, `scene_model` (transform+BLAS instance, `is_show`), `scene_model_manager` (vertex/index buffers, BLAS/TLAS, indirect draw commands, model SSBO), `scene_material_manager` (texture index, material SSBO, font atlases), `scene_light_manager` (light SSBO), `scene_rasterization_render` (MSAA indirect draw, bindless), `scene_raytracing_render` (path tracing, accumulation) |
| `ui/` | 6 pairs | `ui_manager` (ImGui init/render, RT toggle), `ui_base` (`pro::proxy` facade), `ui_record` (chess notation playback), `ui_camera`, `ui_light`, `ui_node` (materials + models). NOTE: `ui_memory` no longer exists. |
| `tools/` | 9 cpp, 10 h | `shader_compiler` (Slang→SPIR-V, .spv cache with SHA-256, returns `std::expected` + Slang diagnostics on failure), `model_loader` (glTF/GLB via fastgltf, returns `std::expected<model_data, load_error>` with `to_string`), `image_helper` (OpenImageIO PNG/EXR/HDR), `ocio_helper` (OCIO shader gen + replace_and_compile + LUT/UBO upload; CPU-side `apply_on_image`), `font_loader` (FreeType), `record_loader` (ICU4C regex chess notation parser), `file_watcher` (SHA-256 hot-reload), `string_helper` (ICU charset detection), `renderdoc_capture` (v1.7.0 API, debug-only) |

## Shaders (9 Slang files in `resources/shaders/`)

| File | Pipeline | Role |
|------|----------|------|
| `rasterization.slang` | Graphics | Forward MSAA: bindless textures, indirect draw, model/material SSBO (uses `common` + `scene_data`). Guards both `material_index` and `texture_index` |
| `ray_tracing.slang` | RT (5 entry points) | Path tracer: NEE + MIS direct light, GGX/Disney lobe selection, jade transmission (Beer-Lambert), height fog, ambient light, Russian roulette, temporal accumulation, HDR equirect sky with procedural fallback |
| `lighting.slang` | (module) | Light sampling (directional/point/spot), Lambertian/Disney/GGX BRDFs, shadow rays, MIS power heuristic, sky sampling, height fog. Imports `common` + `scene_data` |
| `common.slang` | (module) | Constants, Wang hash RNG, `random_float[2/3]`, `hash_noise`, math/color helpers, tangent space, generic `interpolate`, Hammersley 2D, fullscreen triangle vertex |
| `scene_data.slang` | (module) | Shared GPU data structures aligned with C++: `Vertex`, `model_data`, `material_data` (+opacity/ior/transmission), `light_data`, raster/RT push constants |
| `blend_image.slang` | Graphics | Full-screen composite: scene+UI alpha lerp; `ocio_conversion()` stub body replaced at compile time by `ocio_helper::replace_and_compile` (when `USE_OCIO=true`) |
| `scene_skybox.slang` | Graphics | Legacy, unused: HDR cubemap + Uncharted 2 tone mapping |
| `scene_brdflut.slang` | Graphics | Legacy, unused: BRDF split-sum LUT (Sascha Willems example) |
| `scene_cubemap.slang` | Graphics | Legacy, unused: equirectangular→6-face cubemap (MRT) |

Dependency graph:

```
common.slang ← scene_data.slang ← lighting.slang ← ray_tracing.slang
blend_image.slang / rasterization.slang use common / scene_data
scene_skybox / scene_brdflut / scene_cubemap are legacy modules (reuse common)
```

Note: all modules now share `common.slang` / `scene_data.slang`; `utils.slang` was removed (replaced by `common.slang`).

### GPU Data Structures (shared C++/Slang, std430-style)

- **model_data** (raster binding 0 vertex / RT binding 2): `mat4 model_matrix; uint32_t material_index; Vertex* vertex_address; uint32_t* index_address;` — C++ struct in `scene_model_manager.cpp` uses explicit `alignas` (mat4 64B, u32+pad 8B, addresses 8B each → 88B)
- **material_data** (raster binding 1 / RT binding 3): `float3 background_color; uint32_t texture_index; float3 foreground_color; float roughness; float metallic; float opacity; float ior; float transmission;` — C++ struct in `scene_material_manager.cpp` uses `alignas(16)` on vec3/float members (80B); `scene_material` exposes the three RT-only fields with defaults (opacity=1, ior=1.5, transmission=0)
- **light_data** (RT binding 4 only): `float3 color; uint32_t active_type; float3 direction; float intensity; float3 position; float range; float inner_cone_angle; float outer_cone_angle;` (C++ struct in `scene_light_manager.cpp`, `alignas(16)` on vec3)
- **Vertex**: single shared definition in `scene_data.slang` = `{position, normal, uv}`, identical to CPU `model_vertex` (32B). Rasterization vertex input uses only `{position, uv}` (matches its bound vertex attributes)
- **Push constants**: rasterization = `proj` + `view` (128B); RT = `invProj` + `invView` + `hdr_skybox_id` + `light_count` + `frame_index` (144B — exceeds 128B spec minimum, see Known Issues #1)

## Key Design Patterns

### Two-Step Init
`vulkan_application`: `init(instance_layers, extensions)` → create instance; `create(surface, w, h)` → device, swapchain, pipeline. Surface (needs instance) created between steps.

### Physical Device Selection
`vulkan_physical_device` filters by Vulkan 1.4 + required extensions + feature predicate, then `std::views::cartesian_product` over (graphics, compute, transfer, present) QF tuples to maximize distinct families; 10× bonus for dedicated transfer-only queue.

### Timeline Semaphore + Recycle Bin
`vulkan_semaphore` wraps timeline semaphore with atomic CPU counter. `vulkan_recycle_bin` pairs resources with CPU counter at retire time; `release()` destroys once GPU passes that value. CBs signal on submit, wait on begin_record. No `vkDeviceWaitIdle` in the frame loop (only window destroy/resize call `waitIdle`). Scene and UI hold separate semaphore + recycle bin instances.

### Resource State Tracking
`vulkan_buffer`/`vulkan_image` track `stage`/`access`/`layout`/`queue` via `set_info()` after barriers. Barriers are generated from actual current state.

### Queue Ownership Transfer (upload_image / download_image)
Upload: transfer CB releases ownership (no layout change — pure transfer queue lacks shader stages), graphics CB acquires and transitions to `eShaderReadOnlyOptimal`. Download: graphics release → transfer CB transitions to `eTransferSrcOptimal`, copies to staging → graphics re-acquires and restores the original layout.

### Manager Architecture (since 2026-07-28)
`scene_model_manager` owns vertices/indices buffers, per-mesh BLAS, the shared TLAS, indirect draw command buffer (`is_show` → instanceCount 0/1; RT instance mask 0xFF/0) and the model SSBO (material index + vertex/index device addresses). `scene_material_manager` owns the material SSBO and bindless texture array (≤1024 slots, font atlases included); `scene_light_manager` owns the light SSBO. Each manager self-tracks a single `is_dirty` flag: `create()` and `update()` (on real pointer removal) set it, `update()` consumes it and returns whether work was done (`bool`), early-returning when nothing changed. `scene_manager::update()` folds the managers` results into `any_dirty`: only then the active renderer rebuilds descriptors, otherwise it just resets the RT accumulation. UI routes changes through `need_update()` (everything) / `need_camera_update()` / `need_material_update()` / `need_model_update()` / `need_light_update()`. TLAS 增量优化：实例数量不变时仅 refit（`update_top_level_acceleration_structure`，`eUpdate` 模式 + 持久 scratch 缓冲；实例缓冲为每次更新重建的局部缓冲，见 Known Issue #17），增删模型才完全重建。

### Bindless Descriptors
Pools + descriptor sets are rebuilt only when `scene_manager::update()` sees `any_dirty` (a manager actually re-uploaded its SSBO / rebuilt TLAS); camera-only updates (`need_camera_update`) never touch descriptors.

### Dual Rendering
`scene_manager` toggles raster/RT at runtime (`use_ray_tracing` checkbox in "场景设置"). Both share model/material/light SSBOs and the `R16G16B16A16_SFLOAT` render output. Note: `scene_manager::update()` gates manager updates and descriptor rebuilds on per-manager dirty flags (see Manager Architecture); camera/view changes only reset the RT accumulation. `resize()` rebuilds `render_output` and resizes only the *active* renderer (via `active_render->resize`), then calls `need_update()` so descriptors are rebuilt against the new image view. There is no `use_ray_tracing` member: the constructor calls `set_use_ray_tracing(true)` (default RT) which binds `active_render`, force-resizes and `need_update()`, then `resize()` applies the real size. `set_use_ray_tracing(bool)` (UI checkbox) rebinds `active_render`, force-resizes (renderers skip no-op when width/height are unchanged) and forces a full update. `recreate_pipeline()` recompiles only the active renderer shaders (F5 hot reload).

### F5 Hot-Reload
`scene_manager::handle(GLFW_KEY_F5)` now only recompiles the *active* renderer pipeline: `active_render->recreate_pipeline()` (multiplexed through the `manager_render` facade, so no `use_ray_tracing` member is needed) then `active_render->update()` rebuilds descriptors (pipeline layout may change with shader bindings). Old pipeline/SBT are retired into the recycle bin before recreation. Compile failure still propagates (`std::terminate` via uncaught exception, Known Issues #3).

### Save Image QFOT
`vulkan_common::download_image` (3 CBs: graphics release → transfer copy → graphics re-acquire/restore layout), then CPU OCIO `apply_on_image` + EXR/PNG write via OpenImageIO.

### RenderDoc Frame Capture (Debug)
Debug build: `ui->update()` runs before `scene->update()` (same order as release). Same-frame capture: after UI updates but before scene consumes the dirty state, `scene->get_need_update()` decides to capture the *current* frame (skipped while the `first_frame` flag is set (RenderDoc cannot start on the very first frame, avoiding init/upload pressure)). `StartFrameCapture` runs before `scene->update()`, so the upload commands submitted inside it (staging copies / barriers / TLAS builds via `vulkan_common::upload_buffer` / `upload_image`) are included in the capture. Capture ends after render/save. Captures saved to `resources\captures\`; failed captures are discarded. Release: no capture.

### Type Erasure
`ui_base` = `pro::facade_builder`-based interface (P0779R0). `ui_manager` stores `std::vector<pro::proxy<ui_base>>` for polymorphic dispatch (ui_camera, ui_light, ui_node, ui_record).

### Factory with Concepts
`scene_manager::create<T>(args...)` constrains T via C++20 concepts, dispatches to the appropriate manager's private `create(type_identity<T>, ...)`.

### Path Tracing Pipeline (current implementation)

- **Direct lighting (NEE)**: for each light in `light_data[]` (count in push constants): sample light (directional/point/spot, all delta pdf=1), cast shadow ray (any-hit + `ACCEPT_FIRST_HIT_AND_END_SEARCH` + `SKIP_CLOSEST_HIT` + back-face cull; translucent materials pass probabilistically by `opacity`), add `(diffuse + specular) * NdotL` weighted by MIS power heuristic (`power_heuristic`)
  - diffuse: `(1-kS) * (1-metallic) * albedo / PI` (Lambertian)
  - specular: GGX Cook-Torrance (`d_ggx` Trowbridge-Reitz NDF, `g_smith` Schlick-GGX geometry, `f_schlick` Fresnel)
  - per-light contribution firefly clamp: `min(luminance(throughput)*100, 100)`
- **Ambient light**: constant `AMBIENT_LIGHT` (default 0.25) added after direct lighting as `material_color * AMBIENT_LIGHT * (0.5 + 0.5*NdotV)`, so fully shadowed areas stay visible
- **Indirect bounce (lobe selection)**: one branch chosen by probability — transmission (`opacity*transmission`, jade refraction + Beer-Lambert absorption), GGX specular (VNDF importance sampling), or Disney diffuse; throughput `*= lobe_value / (lobe_prob * p_continue)` with `MAX_THROUGHPUT=10` clamp
  - jade transmission: `absorption_density = JADE_BASE_ABSORPTION(12) + JADE_ENGRAVE_ABSORPTION(30) * texture_weight`, applied on entry only (`T = exp(-density * PIECE_THICKNESS)`); `texture_weight` (alpha-map glyph weight) is carried in `HitPayload`
- **Height fog**: analytic exponential fog (`evaluate_height_fog`) applied at closest-hit (bounce segment) and miss (infinite distance), with sun scattering
- **Russian roulette / max depth**: `p_continue = min(1, luminance(throughput))`, paths below 0.05 killed; `MAX_BOUNCES = 8` (recursion via secondary `TraceRay`)
- **Temporal accumulation**: `t = 1 / (frame_index + 1)`; frame_index resets on scene update; per-pixel jitter `random_float2` seeded by `wang_hash(pixel * constants + frame_index)`; firefly clamp `4x` accumulated luminance
- **Sky**: if `skybox_index != 0xFFFFFFFF`, sample HDR equirect texture (`sample_hdr_sky`) with fallback to procedural gradient sky when too dark; height fog applied
- **Chess pieces**: jade material (red = white jade `(0.95,0.92,0.85)` + dark red glyphs, black = green jade `(0.15,0.45,0.32)` + dark green glyphs; `opacity=0.8, ior=1.5, transmission=1.0, roughness=0.3`), tunable in the material panel (不透明度/折射率/透射强度 sliders)
- Not used by the main loop: BRDF LUT (`scene_brdflut.slang`, legacy), `sample_hemisphere_uniform`, `evaluate_brdf`/`sample_brdf` dispatch helpers

## Data Flow (per frame)

```
glfwPollEvents() → [debug: UI update → if scene dirty, begin_capture] → scene->update()
  → scene->update()  (dirty-gated: managers retire+reupload SSBOs, rebuild TLAS/draw commands; render.update() rebuilds descriptors)
  → ui->update()     (ImGui NewFrame + panels + Render)
  → ui_wait    = ui->render()     (MSAA resolve → submit, signals timeline)
  → scene_wait = scene->render()  (raster or RT → submit, signals timeline)
  → app->render({ui_wait, scene_wait})
      → recycle_bin.release()
      → CB: wait timeline + clear → acquire swapchain (OutOfDate → end + return)
      → barrier scene+UI→shader read, swapchain→color attachment
      → blend_image full-screen triangle (scene+UI lerp; ocio_conversion body runtime-replaced by ocio_helper)
      → barrier swapchain→present → submit (signals timeline + binary present) → present (Mailbox→FIFO)
  → [if save: scene->save_image() — download_image QFOT → OCIO CPU → EXR/PNG]
  → [debug: maybe end_capture]
```

Debug note: `scene->update()` runs BEFORE `ui->update()` (capture timing); Release runs `ui->update()` then `scene->update()`.

## Resources

- **Models** (`resources/models/`): `chess_board.glb`, `chess_board_line.glb`, `chess_piece.glb`, `scene_skybox.glb`, `chess_all.blend` (+ `.blend1` backup); `Box.glb`/`Cube.glb`/`Sphere.glb` exist in the working tree as untracked test assets.
- **Fonts** (`resources/fonts/`): LXGW WenKai GB (Light/Medium/Regular), LXGW WenKai Mono GB (Light/Medium/Regular), Source Han Sans SC (7 weights), 华文粗楷-SC.ttf — 14 font files + OFL.txt. Runtime currently loads `LXGWWenKaiGB-Medium.ttf` for ImGui (13px) and for board/piece glyph atlases.
- **Textures** (`resources/textures/`): `干裂地面.hdr` (~90 MB) — default HDR skybox.
- **OCIO** (`resources/ocios/`): 5 ACES 2.0 + OCIO v2.4 configs (studio, D60, all-views, reference, CG) + `aces_conversion_graph.svg`. The all-views studio config is used for both CPU save transform and GPU shader generation.
- **Records** (`resources/records/`): `棋谱1.txt` (GB2312 sample).
- **Shaders**: 9 `.slang` files (`common`/`scene_data` modules + 7 entry shaders); compiled `.spv` cache files sit next to them (gitignored).


## Required GPU Features

Extensions (from `vulkan_application.cpp`):
`VK_KHR_swapchain`, `VK_KHR_spirv_1_4`, `VK_KHR_synchronization2`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor`

Features:
- Base: `samplerAnisotropy`, `fillModeNonSolid`, `multiDrawIndirect`, `shaderInt64`
- Vulkan 1.4: `pushDescriptor` | 1.3: `dynamicRendering`, `synchronization2` | 1.2: `bufferDeviceAddress`, `runtimeDescriptorArray`, `scalarBlockLayout`, `timelineSemaphore` | 1.1: `shaderDrawParameters`
- EXT: `extendedDynamicState` (VK_EXT_extended_dynamic_state)
- AS: `accelerationStructure`, `accelerationStructureCaptureReplay`, `descriptorBindingAccelerationStructureUpdateAfterBind`
- RT: `rayTracingPipeline`, `rayTracingPipelineTraceRaysIndirect`, `rayTraversalPrimitiveCulling`

## Key Dependencies (vcpkg.json)

| Dependency | Purpose |
|-----------|---------|
| `fastgltf` | glTF/GLB loading |
| `freetype` (error-strings) | Font rasterization |
| `glfw3` | Window + input |
| `glm` | Math |
| `icu` | Unicode, regex, charset detection |
| `imgui` (docking-experimental + glfw/vulkan binding; installed 1.92.8) | UI |
| `opencolorio` | ACES 2.0 color management |
| `openimageio` | Image I/O |
| `openssl` | SHA-256 hashing |
| `proxy` | Type-erased polymorphic facade (ui_base) |
| `shader-slang` | Slang → SPIR-V |
| `vulkan-headers` / `vulkan-utility-libraries` / `vulkan-memory-allocator-hpp` (pinned 3.3.0) | Vulkan + VMA |
| `yaml-cpp` (transitive via opencolorio; explicitly `find_package` in CMake) | OCIO config parsing |

## Known Issues & TODOs

Verified 2026-08-08 against the local working tree (CMake migration and scene_light refactor included). Re-scanned 2026-08-12 after commits `ca79139` (dirty-gating + TLAS refit) and `4c2e58c` (model filters) were code-reviewed; findings 14-23 below.

1. **RT push constants 144 > 128 bytes** (`scene_raytracing_render.h:48-55`): `2×mat4 + 3×u32` = 144B (measured `sizeof`, earlier docs said 152) — exceeds spec-guaranteed minimum; no `maxPushConstantsSize` query. Fix: pack into UBO or gate device selection.

2. ~~Rasterization lacks texture_index guard~~ (FIXED 2026-08-09: `rasterization.slang` now guards both `material_index` and `texture_index`). Remaining: `ePartiallyBound` is still not enabled on the descriptor layout, so unfilled bindless slots remain technically undefined.

3. **F5 hot-reload terminates on shader error** (`scene_manager.cpp:151`): old render retired BEFORE the new one is constructed; compile error → `std::terminate`. Fix: construct first, retire on success.

4. **OCIO GPU composite binding-order mismatch**: pipeline layout is `[scene, ui, sampler, UBO, texture pairs...]`, but `vulkan_application::bind_image()` appends descriptor infos in order `[scene, ui, sampler, texture pairs..., UBO]`. Descriptor writes use array order as binding index, so everything from binding 3 on is shifted (UBO written into a sampled-image slot and vice versa). `replace_and_compile` injection and LUT/UBO uploads are implemented, but the GPU transform is effectively broken; validation errors are likely. The debug callback prints errors/warnings but does not filter them.

5. ~~**Empty-container null descriptor writes**~~ (PARTIALLY FIXED 2026-08-12, see #15): the model path is fixed (empty scene keeps a valid TLAS + 1-entry dummy model SSBO + renderer skip/clear), but the material/light managers still retire their SSBOs when everything is deleted, so deleting all materials or all lights still writes null buffers. `nullDescriptor` is not enabled — in the current vulkan-headers it only exists under `VK_KHR_robustness2`, so enabling it means adding that extension + feature.

6. **ImGui multi-viewport validation false positive** (imgui 1.92.8): secondary viewport swapchains present without acquire → `UNASSIGNED-non-acquired-swapchain-image-used`. The debug callback currently does NOT filter this (previous docs claiming a filter are stale).

7. ~~**save_image layout correctness**~~ (FIXED — verified 2026-08-12): `vulkan_common::download_image` performs all layout transitions on the acquire side (transfer CB → `eTransferSrcOptimal`, graphics re-acquire → old layout); release barriers keep `old_layout → old_layout` with `eNone/eNone`. No TODO remains in `scene_manager.cpp`.

8. **BLAS/TLAS build queue**: builds use the graphics queue; TODO comments note compute queue (TLAS build requires VK_QUEUE_COMPUTE_BIT-capable queue).

9. ~~Both render paths update every dirty frame~~ (FIXED 2026-08-10): managers self-track `is_dirty`; camera-only changes (`need_camera_update`) skip manager updates, TLAS rebuild and descriptor rebuilds entirely.

10. ~~Descriptor pools/sets fully rebuilt on every dirty update~~ (FIXED 2026-08-10): renderers rebuild descriptors only when managers report actual work (`any_dirty`); view-only changes never touch descriptors.

11. ~~CMake / MSVC flags~~ (FIXED 2026-08-11): `CMakeLists.txt` now sets `add_compile_options(/utf-8)` under `if(WIN32)`. Remaining: x86/Linux/macOS presets exist but are unmaintained.

12. ~~Legacy/unused shaders & code duplication~~ (RESOLVED 2026-08-09): `scene_skybox`/`scene_brdflut`/`scene_cubemap` remain unused but now reuse `common.slang`; `ray_tracing.slang` no longer duplicates helpers; `utils.slang` replaced by `common.slang` + `scene_data.slang`. Known issue #2 (texture_index guard) also fixed.


13. **Docs/build migration**: the project moved to CMake (`.slnx`/`.vcxproj` deleted in the working tree, `vcpkg` added as a submodule). Docs were re-scanned and updated accordingly; vcpkg-configuration now points at the GitHub default registry (the old Gitee-mirror claim is stale).

14. **TLAS refit scratch sizing** (FIXED 2026-08-12): the scratch buffer was sized only from `buildScratchSize` — `updateScratchSize` was never queried, so an `eUpdate`-mode refit could read past the scratch on drivers whose update-scratch demand exceeds build-scratch. Fix in `vulkan_acceleration_structure.cpp`: scratch sized `max(buildScratchSize, updateScratchSize)` — the update-mode size is queried after AS creation with the new AS as `srcAccelerationStructure`, only for updatable ASes (build flags contain `eAllowUpdate`). NOTE: an earlier review claimed `createFlags` also lacked `VK_ACCELERATION_STRUCTURE_CREATE_ALLOW_UPDATE_BIT_KHR` — this was refuted against the official registry (`vk.xml` 1.4.350): no such create flag exists; update legality is governed solely by the build flags (`VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR` on the initial build, and update builds must match them — VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03759/03760), which the code already did.

15. **Empty scene (delete all models) → null descriptors + trace against retired TLAS** (OPEN — proposed fix reverted by the author 2026-08-12): `update_tlas`'s empty branch retires the TLAS, `update_ssbo` skips re-upload when empty, and the RT renderer traces unconditionally → null AS + null SSBO descriptors (invalid without `nullDescriptor`) and trace against a retired AS. Not reachable while `ui_record` holds the board/pieces as `shared_ptr` (the models vector never empties in practice); latent if model removal ever clears it. Proposed fixes (all reverted): keep the last valid TLAS alive + renderer skip/clear + 1-entry dummy SSBO; alternatively enable `nullDescriptor` via `VK_KHR_robustness2`.

16. **Inconsistent model filtering (`4c2e58c`)** (FIXED 2026-08-12): the `filter(expired && model_info != nullptr)` added to `update_draw_commands`/`update_ssbo` was unreachable (`update()` prunes expired weak_ptrs before these run) and broke the `models.size() == draw_commands.size() == ssbo.size()` invariant — `drawIndexedIndirect` uses the unfiltered `get_models_size()` as drawCount, so any filtered-out model would cause GPU-side OOB on both the indirect buffer and the SSBO; `update_tlas` was not filtered and would crash first (`get_blas_instance` derefs `model_info` unconditionally). Fix: unified prune in `update()` (drops expired OR null-`model_info` models, one lock per entry), per-consumer filters removed → all four consumers (meshes, TLAS, draw commands, SSBO) see the same list.

17. **TLAS refit instance buffer: per-refit allocation kept** (author's 2026-08-12 design): instead of my proposed persistent-buffer fix (`vulkan_common::update_buffer` — graphics release → transfer write → graphics acquire, reverted), `update_tlas` now uses a per-update **local** `tlas_instance_buffer` (both branches), retired into the recycle bin after recording the build — the member was removed from the header. Lifetime is safe (retire captures the CPU counter after the upload submits; `release()` destroys only once the GPU counter passes it, i.e. after the build CB completes). Cost: VMA alloc + staging + QFOT churn per refit remains; only the scratch buffer is persistent — the "持久 scratch/instance 缓冲" doc claim was corrected accordingly. Remaining: `upload_buffer`'s acquire barrier sets `eShaderRead`, which does not cover the AS-build input read (`eAccelerationStructureReadKHR`); the build CB relies on the timeline-semaphore wait + driver tolerance (latent sync gap, works in practice — was fixed by the reverted `update_buffer`).

18. **`is_dirty` cleared before the manager fold** (`scene_manager.cpp`): `update()` clears `is_dirty` before the manager proxy fold; an exception inside any manager update (e.g. VMA allocation failure in `upload_buffer`) leaves `is_dirty == false` with the fold aborted → failed uploads are never retried and later managers never run → permanently stale GPU state until the next UI `need_*_update()`. OPEN. Fix: clear `is_dirty` after the fold (or RAII guard).

19. **Debug RenderDoc capture excludes UI uploads** (`window/window.cpp`): in debug builds `ui->update()` now runs before `begin_capture`, so UI-initiated GPU uploads (font atlases, material textures created in `ui_node`/`ui_record` → `upload_image`) are outside the captured frame — the same-frame capture intent documented in the Data Flow section is not met (playback shows the texture without its upload). OPEN.

20. **`resize()` / `set_use_ray_tracing()` trigger a full scene re-upload** (`scene_manager.cpp`): both route through `need_update()`, which re-dirties all three managers → full vertex/index re-upload, BLAS rebuilds, TLAS refit and SSBO re-uploads on window resize or renderer toggle, although none of that data depends on window size or renderer choice. OPEN. Fix: the per-manager `need_*_update()` API already exists — route these through it (only the newly-active renderer's descriptors need rebuilding).

21. **`ui_record::restore_board_state()` uses blanket `need_update()`** (`ui/ui_record.cpp`): every chess step, listbox selection, record load and initial resize re-dirties all three managers, defeating the selective dirty-gating introduced by `ca79139` — only the model manager's `is_show`/`model_matrix` actually changed. OPEN. Fix: call `need_model_update()` (the refit path already handles these).

22. ~~**RT pieces share `custom_index = 2`**~~ (REFUTED 2026-08-12 — false positive): `ray_tracing.slang` indexes `models[InstanceId()]`, and per the SPIR-V/Vulkan spec `InstanceId` is the **index of the intersected instance in the instance array** (not `instanceCustomIndex`; that is `InstanceCustomIndexKHR`, which the compiled SPIR-V does not use). The TLAS instance array and the model SSBO are built from the same ordered `models` vector (`scene_model_manager.cpp:249-251` vs `:336-347`), so `models[InstanceId()]` addresses each piece's own entry — RT rendering is correct. `custom_index = 2u` (`ui_record.cpp:46`) is dead data. Residual risk: correctness depends on Slang mapping `InstanceIndex()` to the legacy `InstanceId` builtin — re-verify after a Slang upgrade (compile + disassemble, or check the BuiltIn enum is 6, not 5327).

23. **`scene_model_manager::clear()` did not set `is_dirty`** (FIXED 2026-08-12): unlike `scene_light_manager::clear()` / `scene_material_manager::clear()`, a live `clear()` left the manager permanently not-dirty while renderers kept binding the retired buffers → use-after-free once the recycle bin frees them. `clear()` now sets `is_dirty = true`.

24. **MIS weight for delta lights was < 1** (FIXED 2026-08-12): `ray_tracing.slang` computed `w_light = power_heuristic(pdf_light, pdf_brdf)` for directional/point/spot lights — all delta distributions (`ls.pdf = 1`), which BRDF importance sampling can never hit, so the correct NEE weight is exactly 1.0; the old value systematically darkened direct lighting (~9%+ on diffuse, worse on glossy/metallic). Fix: `w_light = 1.0f` (restore MIS if area lights are ever added).

25. **Instance-level `eTriangleCullDisable` nullified ray-level back-face culling** (FIXED 2026-08-12): `scene_model::get_blas_instance` set `eTriangleCullDisable` on every instance, which (per spec) makes `RAY_FLAG_CULL_BACK_FACING_TRIANGLES` a no-op → primary and bounce rays did double-sided traversal (≈2× BVH cost, RT is the default renderer) and could hit back faces. Removed; translucent/transmission paths already disable culling at the ray level (`ray_tracing.slang` bounce ray).

26. **Per-frame heap allocations in the composite path** (FIXED 2026-08-12): `vulkan_application::render` built temporary `std::vector`s for waited/signal infos every frame (3 mallocs/frame). Fix: single-element `add_waited_info`/`add_signal_info` overloads; `render` now takes the two `SemaphoreSubmitInfo`s by value. Remaining (accepted): the UI/scene/app `vkWaitSemaphores` triple-wait per frame — waits are a subset of each other on the same queue but protecting each renderer's own CB reuse; not worth restructuring.

27. **UI numeric boundary bugs** (FIXED 2026-08-12): `ui_light.cpp` passed radian values to `SliderAngle`'s degree-based min/max → inverted `min > max` cone ranges; now `glm::degrees`-converted with `inner ≤ outer` constraint. `ui_node.cpp` allowed scaling to 0 → rank-deficient matrix made `glm::decompose` emit NaN into the model matrix uploaded to the GPU; `DragFloat3` now clamps scale to `[0.001, 1000]`.

Note (2026-08-12): MSAA sample count (`vulkan_application::pick_msaa_sample_count`) intentionally picks the device's highest supported sample count (verified 8× on the development GPU) — a design choice, not an issue.
