# CLAUDE.md

## Project Overview

Real-time 3D Chinese Chess (Xiangqi) visualization — **C++23**, **Vulkan 1.4**. Dual rendering: forward MSAA rasterization + path tracing (NEE direct lighting, Lambertian indirect bounce, Russian roulette, temporal accumulation). Slang shaders, ImGui UI (docking + multi-viewport), OpenColorIO ACES 2.0.

Chinese docs: `README_CN.md`.

> 最近一次全量扫描:2026-08-27(vulkan_core 17 pairs、pipeline cache 定稿、析构加固、scene_base 优化、修饰符规范)。注意:不要假设 `.slnx`/`.vcxproj` 存在(已删除,CMake 迁移);`PBR` 与 `heap` 分支为实验分支未并入 main,`spectral-rendering-plan.md` 仅存在于 `PBR` 分支。本文档刻意不写提交哈希(作者会改写 git 历史),统一用日期/描述定位改动。

## Build

- **Platform**: Windows x64, Visual Studio 2022 (v143+), **CMake (≥ 4.0) + Ninja + vcpkg manifest mode**
- Configure: `cmake --preset x64-debug` / `x64-release` (仅 configurePresets);build: `cmake --build out/build/<preset>`
- **Binary**: `out\build\x64-release\chinese-chess.exe`;工作目录必须为 `chinese-chess/`(相对资源路径;CMake 设 `DEBUGGER_WORKING_DIRECTORY`)
- **vcpkg**: 本地子模块(`vcpkg/`),toolchain `vcpkg/scripts/buildsystems/vcpkg.cmake`,manifest `vcpkg.json`,动态链接;`vcpkg-configuration.json` 配置 `overlay-ports: ["./vcpkg-overlays"]`(自定义 ktx port:KTX-Software 5.0.0-rc1,UASTC HDR 支持)
- **C++23**: `target_compile_features(cxx_std_23)`;源文件无 BOM UTF-8(中文注释),`if(WIN32)` 下 `add_compile_options(/utf-8)`
- **Preprocessor defines**: `VK_USE_PLATFORM_WIN32_KHR`, `GLFW_INCLUDE_VULKAN`, `GLM_FORCE_RADIANS`, `GLM_ENABLE_EXPERIMENTAL`, `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `NOMINMAX`, `UNICODE`, `_UNICODE`
- **Include path**: `chinese-chess/`(项目根,所有 include 相对它)
- **Shell**: 优先 PowerShell 7 (`pwsh`),缺现代语法(如 `Join-String`)时才回退 PS 5.1

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
| `vulkan_core/` | 17 pairs | `vulkan_application` (orchestrator, owns `vulkan_pipeline_cache`), `vulkan_common` (format selection, upload/download QFOT, `MAX_FRAMES_IN_FLIGHT`/`USE_OCIO`/`MSAA_SAMPLE_COUNT`/`DEPTH_FORMAT` 全局), `vulkan_device`, `vulkan_physical_device` (cartesian_product QF selection), `vulkan_swapchain` (Mailbox/FIFO, per-image binary semaphores), `vulkan_buffer`/`vulkan_image` (VMA + state tracking), `vulkan_commandbuffer` (timeline semaphore), `vulkan_queue`, `vulkan_semaphore` (timeline + atomic counter), `vulkan_recycle_bin`, `vulkan_pipeline` (graphics + RT;缓存句柄由参数传入), `vulkan_pipeline_cache` (磁盘缓存加载/保存 + 设备校验), `vulkan_descriptor` (multi-frame variant writes), `vulkan_sampler` (per-type RAII), `vulkan_acceleration_structure` (BLAS/TLAS), `vulkan_shader_binding_table` |
| `scene/` | 11 pairs | `scene_manager` (orchestrator, F5/F6 hot-reload, save_image, `is_render_dirty`), `scene_base` (facades: `manager_base`/`manager_render`), `scene_camera`, `scene_light` (flat struct), `scene_material`, `scene_model` (transform+BLAS, `is_show`), `scene_model_manager` (vertex/index buffers, BLAS/TLAS, indirect draw, model SSBO), `scene_material_manager` (material SSBO, bindless textures, 字体图集压缩, KTX2, F6 热重载), `scene_light_manager` (light SSBO), `scene_rasterization_render`, `scene_raytracing_render` |
| `ui/` | 6 pairs | `ui_manager` (ImGui init/render, RT toggle), `ui_base` (`pro::proxy` facade), `ui_record` (棋谱回放), `ui_camera`, `ui_light`, `ui_node` (materials + models). NOTE: `ui_memory` 已不存在。 |
| `tools/` | 9 cpp, 10 h | `shader_compiler` (Slang→SPIR-V, 依赖感知 .spv 缓存, `std::expected`), `model_loader` (fastgltf, `std::expected<model_data, std::string>`), `image_helper` (OIIO + KTX2 压缩, `paste_image`/`convert_channels`), `ocio_helper`, `font_loader`, `record_loader` (ICU4C 记谱解析), `file_watcher` (SHA-256, 缓存 `resources\cache\file_watch_cache.cache`), `string_helper`, `renderdoc_capture` |

## Shaders (9 Slang files in `resources/shaders/`)

| File | Pipeline | Role |
|------|----------|------|
| `rasterization.slang` | Graphics | Forward MSAA: bindless textures, indirect draw, model/material SSBO. Guards both `material_index` and `texture_index` |
| `ray_tracing.slang` | RT (5 entry points) | Path tracer: NEE + MIS, GGX/Disney lobe selection, jade transmission, height fog, ambient, Russian roulette, temporal accumulation, HDR sky |
| `lighting.slang` | (module) | Light sampling, BRDFs, shadow rays, MIS, sky sampling, height fog |
| `common.slang` | (module) | Constants, Wang hash RNG, math/color helpers, tangent space, Hammersley, fullscreen triangle vertex |
| `scene_data.slang` | (module) | Shared GPU structures aligned with C++ (`Vertex`, `model_data`, `material_data`, `light_data`, push constants) |
| `blend_image.slang` | Graphics | Scene+UI alpha composite; `ocio_conversion()` stub body replaced by `ocio_helper` (self-contained, no imports) |
| `scene_skybox.slang` | Graphics | Legacy, unused: HDR cubemap + Uncharted 2 tone mapping |
| `scene_brdflut.slang` | Graphics | Legacy, unused: BRDF split-sum LUT |
| `scene_cubemap.slang` | Graphics | Legacy, unused: equirect→cubemap (MRT) |

Dependency graph:

```
common.slang ← scene_data.slang ← lighting.slang ← ray_tracing.slang
rasterization.slang uses common / scene_data
blend_image.slang is self-contained (no imports)
scene_skybox / scene_brdflut / scene_cubemap are legacy modules (reuse common)
```

Note: `utils.slang` was removed (replaced by `common.slang`).

### GPU Data Structures (shared C++/Slang, std430-style)

Byte sizes re-verified 2026-08-16 (MSVC `sizeof` + SPIR-V `OpMemberDecorate`/`ArrayStride`); C++ 与 GPU 布局完全一致。Slang/std430 将 `float3` 打包为 12B 但 16B 对齐,`alignas(16)` 只提升 offset 不影响 extent。

- **model_data** (raster b0 / RT b2): `mat4 model_matrix; uint32_t material_index; Vertex* vertex_address; uint32_t* index_address;` offsets 0/64/72/80, **sizeof 96B**
- **material_data** (raster b1 / RT b3): `float3 background_color; uint32_t texture_index; float3 foreground_color; float roughness, metallic, opacity, ior, transmission;` offsets 0/12/16/28/32/36/40/44, **sizeof 48B**;RT-only 字段默认 opacity=1, ior=1.5, transmission=0
- **light_data** (RT b4): `float3 color; uint32_t active_type; float3 direction; float intensity; float3 position; float range; float inner/outer_cone_angle;` offsets 0/12/16/28/32/44/48/52, **sizeof 64B**
- **Vertex** (`scene_data.slang` 唯一共享定义): `{position, normal, uv}` = CPU `model_vertex` (32B);光栅顶点输入仅 `{position, uv}`
- **Push constants**: raster = `proj`+`view` (128B);RT = `invProj`+`invView`+`hdr_skybox_id`+`light_count`+`frame_index` (**144B** — 超 128B 规范下限,见 Known Issue #1)

## Key Design Patterns

### Two-Step Init
`vulkan_application`: `init(instance_layers, extensions)` → instance;`create(surface, w, h)` → device, swapchain, pipeline。Surface(依赖 instance)在两步之间创建。

### Physical Device Selection
按 Vulkan 1.4 + 扩展 + feature 谓词过滤,`std::views::cartesian_product` 穷举 (graphics, compute, transfer, present) 队列族组合,10× 加分专用传输队列,最大化独立队列族数量。

### Timeline Semaphore + Recycle Bin
`vulkan_semaphore` = 时间线信号量 + 原子 CPU 计数器;`vulkan_recycle_bin` 将资源与 CPU 计数配对,`release()` 在 GPU 越过该值后销毁。CB 在 submit 时 signal、begin_record 时 wait。帧循环无 `vkDeviceWaitIdle`(仅销毁/resize 调用)。Debug 回收日志由 `constexpr output=false` 编译期关闭(2026-08-26)。

### 析构顺序加固 (2026-08-27)
默认析构 = 成员声明逆序,命令缓冲/描述符集/allocator/swapchain 生命期约束声明顺序已满足,此处只修"引用先销毁"隐患:
- **`~vulkan_application()`**: 先 `device->waitIdle()`(未创建则跳过),再 `descriptor.clear_descriptor_info()` 销毁描述符池/集——解除对 scene/ui `render_output`、`ocio_images`/`ocio_samplers`、`image_sampler` 的引用,随后成员逆序析构才安全。
- **`~scene_manager()` / `~ui_manager()`**: 析构开头 `app.wait()`;原 `destroy()` 已合并进析构(ui_manager 执行 ImGui 三个 Shutdown;scene_manager 执行各 manager `clear()`),`glfw_window` 析构不再调用。要求 app 在 scene/ui 之后析构、GLFW window 在 ui 之后销毁(ImGui_ImplGlfw_Shutdown 需 window 回调仍有效)。
- **`vulkan_acceleration_structure`**: `buffer` 改为在 `acceleration_structure` **之前**声明 → AS 先于 backing buffer 销毁;move 构造/赋值同步。

### Resource State Tracking
`vulkan_buffer`/`vulkan_image` 经 `set_info()` 追踪 stage/access/layout/queue,barrier 基于实际当前状态生成。

### Queue Ownership Transfer (upload_image / download_image)
Upload: 传输队列 release(不改布局)→ 图形 acquire + `eShaderReadOnlyOptimal`。Download: 图形 release → 传输 `eTransferSrcOptimal` 拷贝 → 图形 re-acquire 恢复原布局。

### Manager Architecture (since 2026-07-28)
`scene_model_manager` 持有顶点/索引缓冲、每网格 BLAS、共享 TLAS、间接绘制命令缓冲(`is_show` → instanceCount 0/1;RT mask 0xFF/0)、模型 SSBO;`scene_material_manager` 持有材质 SSBO + bindless 纹理数组(≤1024 槽,含字体图集);`scene_light_manager` 持有灯光 SSBO。各 manager 自查单一 `is_dirty`(`create()`/真删除置脏,`update()` 消费并返回 `bool`),`scene_manager::update()` 折叠为 `any_dirty` 并叠加 `is_render_dirty`(resize/渲染器切换,2026-08-26):`any_dirty || is_render_dirty` 时重建描述符,否则仅重置 RT 累积。UI 经 `need_update()`/`need_camera_update()`/`need_material_update()`/`need_model_update()`/`need_light_update()` 路由。TLAS:实例数不变仅 refit(`eUpdate` + 持久 scratch;实例缓冲每次更新重建局部缓冲,见 #17),增删模型才完全重建。

### Bindless Descriptors
描述符池/集仅在 `any_dirty || is_render_dirty` 时重建;相机-only 更新永不触碰描述符。

### Dual Rendering
运行时 `use_ray_tracing` checkbox 切换光栅/RT,共享 SSBO 与 `R16G16B16A16_SFLOAT` render_output。**无 `use_ray_tracing` 成员**:构造函数直接绑定 `active_render = raytracing`(默认 RT),`resize()` 应用真实尺寸。`set_use_ray_tracing(bool)` rebind + force-resize + 置 `is_dirty`+`is_render_dirty`(场景数据零重传,仅重建描述符,#20)。`resize()` 重建 render_output 并仅 resize active renderer。`recreate()` = F5 仅重编译 active renderer shaders。

### F5 Hot-Reload
`handle(F5)` → `active_render->recreate()`(`manager_render` 约定 `mem_render_recreate`)+ `active_render->update()` 重建描述符(pipeline layout 可能变化);旧 pipeline/SBT retire 进 recycle bin。编译失败仍 `std::terminate`(#3)。复用 app 持有的同一 `vulkan_pipeline_cache` 句柄,**不重读盘、不写盘**。

### F6 Texture Hot-Reload (KTX2)
`handle(F6)` → `material_manager.reload_textures(waited_infos)`,返回 `true` 时 `need_material_update()` + `need_model_update()`。**单线程**遍历 `images_cache`:字体图集键是合成路径,`exists` 检查天然排除;对真实文件先 SHA-256 判变,变则内存压缩 → 同步写 UASTC sidecar → `upload_ktx2` 转码上传新 `vulkan_image`,`recycle_bin` 同槽位换图(texture_index 稳定,旧 imageview 在 GPU 越过计数后销毁),失败保留旧纹理并打印错误。`upload_image` 上传前 `clearColorImage` 清空(压缩格式跳过),未写区域恒为 0。

### Pipeline Cache (since 2026-08-26,2026-08-27 定型)
- **归属**: 独立 RAII 类 `vulkan_pipeline_cache`,由 `vulkan_application` 持有(`get_pipeline_cache()`),`pipeline_cache.create(*physical_device, *device)` 在 `vulkan_application::create()` 调一次(`vulkan_application.cpp:66`);`vulkan_pipeline::create`/`create_from_shader` 与两个渲染器均以 `const vk::raii::PipelineCache&` 共享同一句柄——**单实例**。
- **加载**: 读 `resources\cache\pipeline_cache.cache`,以 `vk::PipelineCacheHeaderVersionOne` 校验 vendorID/deviceID/UUID;缺失/不匹配/损坏(捕获 `vk::SystemError`)回退空缓存,不阻塞启动。
- **保存**: 仅 `glfw_window::~glfw_window()` 中 `app->wait()` 后调用一次(`window.cpp:72`);**F5 不写盘**。条目按完整 create info(含 SPIR-V)键控,旧 shader 自然 miss。
- **`create_from_shader`**: 编译与创建合并进 `vulkan_pipeline`;`shader_stage_info{name, stage}` 定义于 `vulkan_pipeline.h`;`compile_shader` 失败抛 `std::runtime_error`(含 Slang 诊断)。
- **依赖感知 `.spv` 缓存**: 路径编译改 `loadModule`(模块名 = stem),编译前遍历 import 链(`getDependencyFileCount/Path`),任一依赖 SHA-256 判变即重编译。字符串编译走 `loadModuleFromSourceString`;原 `slang_to_slang_module` 已删(现内部辅助名 `slang_module_to_spv`)。

### KTX2 压缩管线 (since 2026-08-15,2026-08-18 定型)
- **磁盘纹理**: sidecar 存在且源未变 → `read_ktx2`(UASTC → 无条件转码 BC6H/BC7);否则 `read_and_compress_to_ktx2` 内存压缩 → **同步** `write_ktx2` 写 sidecar(转码前,保持可移植中间格式)→ `upload_ktx2` 转码上传。设备格式 `compressed_hdr_format`(BC6H)/`compressed_ldr_format`(BC7);转码映射 BC6H→`KTX_TTF_BC6HU`、BC7→`KTX_TTF_BC7_RGBA`、BC4→`KTX_TTF_BC4_R`、EAC_R11→`KTX_TTF_ETC2_EAC_R11`。
- **字体图集**: 单字(棋子)与多字(棋盘"楚河汉界")图集均压缩;宽高 4 对齐 + 内容**居中**;画布 RGBA,1 通道字形经 `paste_image` 1→N 复制;`compress_to_ktx2` → `upload_ktx2` 转码 `compressed_font_format`(桌面 **BC4** / 移动 **EAC_R11**,单通道 4:1,采样 `.x` 与 R8 一致);压缩失败回退 `convert_channels(atlas, 1)` 还原 R8。棋盘 UV 0-1 整图烘焙,居中扩展不改变字形相对位置,**无需重调 `chess_board.glb`**(已核验)。

### std::expected Error Handling (project-wide, since 2026-08-15)
可恢复的 data/IO/parse 错误返回 `std::expected<T, std::string>`(`compile_shader_to_spv`、`load_model`、`find_supported_format`、image_helper 系列、`load_font`、`load_records/read_record`、`upload_ktx2`);初始化/不可恢复错误 throw(`vulkan_application`、swapchain acquire/present);不变量/参数检查 throw(`set_info` 等);**"未找到"不是错误 → `std::optional`**(`get_material_index`/`get_texture_index`)。返回 `shared_ptr` 的工厂在边界把 expected 错误转为 `throw std::runtime_error(error)`。

### Save Image QFOT
`vulkan_common::download_image`(3 CBs:graphics release → transfer copy → graphics re-acquire/restore),然后 CPU OCIO `apply_on_image` + OIIO 写 EXR/PNG。

### RenderDoc Frame Capture (Debug)
Debug 帧序与 Release 相同(`ui->update()` 先于 `scene->update()`)。ui 更新后、scene 消费前按 `scene->get_need_update()` 决定本帧捕获(跳过 `first_frame`);`StartFrameCapture` 在 `scene->update()` 前,upload/TLAS 构建入捕获;render/save 后结束。存 `resources\captures\`,失败丢弃。Release 无捕获。

### Type Erasure
`ui_base` = `pro::facade_builder`(P0779R0);`ui_manager` 以 `std::vector<pro::proxy<ui_base>>` 持有 ui_camera/ui_light/ui_node/ui_record。`scene_base` 定义 `manager_base`/`manager_render` 供 `scene_manager` 以 `pro::proxy_view`(非拥有)持有 manager 与 active renderer;因 `observer_facade` 将 copy/relocate/destruct 固定为 trivial,两 facade 无需 `support_*`(2026-08-27)。`ui_base` 的 `support_relocation`/`support_destruction` 用 `constraint_level::nothrow`(2026-08-27 由 nontrivial 收紧;`trivial` 需平凡析构,ui_* 含 vector/shared_ptr/unordered_map 不满足)。

### Factory with Concepts
`scene_manager::create<T>(args...)` 以 concepts 约束 T,分派到对应 manager 私有 `create(type_identity<T>, ...)`。

### 修饰符规范 (2026-08-27 扫描确认)
getter 一律 `[[nodiscard]] const noexcept`;proxy 约定成员(manager_base/manager_render/ui_base 的 update/clear/resize/render/recreate/reset_accumulation/handle)**不加修饰符**(改签名破坏 proxy invocable 检查);可抛函数(容器操作/I/O/vk::* 调用/format)不标 noexcept——错标会 `std::terminate`。**`[[nodiscard]]` 只写在 .h 声明处,成员函数的 .cpp 定义不重复标注**(作者 2026-08-27 手动清理;唯一例外:匿名命名空间内部自由函数无 .h 声明,必须留在定义处)。已移除的错误 noexcept:`find_supported_format`/`upload_buffer`/`upload_image`/`download_image`(实际可抛)与 `get_current_image`/`get_current_imageview`(`images.at()` 可抛)——异常现可正常传播。

### Path Tracing Pipeline (current implementation)

- **NEE**: 对每个光源采样(delta 分布 pdf=1),阴影光线(any-hit + `ACCEPT_FIRST_HIT_AND_END_SEARCH` + `SKIP_CLOSEST_HIT` + 背面剔除;半透明按 `opacity` 概率穿透),贡献按 MIS 幂启发式加权;diffuse `(1-kS)(1-metallic)albedo/π` + GGX Cook-Torrance specular;单光 firefly 钳制 `min(luminance*100, 100)`
- **Ambient**: `AMBIENT_LIGHT`(0.25)以 `材质色 × 0.25 × (0.5 + 0.5·NdotV)` 叠加,完全阴影区保持可见
- **间接弹射(波瓣选择)**: 三分支——透射(`opacity×transmission`,玉石折射 + Beer-Lambert 吸收)、GGX 镜面(VNDF)、Disney 漫反射;吞吐量 `×= lobe_value/(lobe_prob·p_continue)`,`MAX_THROUGHPUT=10`
  - 玉石透射: `absorption_density = 12 + 30×texture_weight`,仅进入时 `T = exp(-density·PIECE_THICKNESS)`;`texture_weight`(alpha 字形权重)经 `HitPayload` 透传
- **Height fog**: 解析指数雾,closest-hit 段与 miss 均应用,含太阳散射
- **Russian roulette / max depth**: `p_continue = min(1, luminance)`,< 0.05 截断;`MAX_BOUNCES = 8`(二级 TraceRay 递归)
- **Temporal accumulation**: `alpha = 1/(1 + frame_index/2)`;frame_index 场景更新时重置;jitter `random_float2` 以 `wang_hash(pixel·constants + frame_index)` 种子;accumulated 亮度 4× firefly 钳制
- **Sky**: `skybox_index != 0xFFFFFFFF` 时采样 HDR equirect(`sample_hdr_sky`),过暗回退程序化渐变天空
- **Chess pieces**: 白玉 `(0.95,0.92,0.85)` + 深红刻字 / 青玉 `(0.15,0.45,0.32)` + 墨绿刻字;`opacity=0.8, ior=1.5, transmission=1.0, roughness=0.3`,材质面板可调
- 主循环未使用: BRDF LUT(`scene_brdflut.slang`)、`sample_hemisphere_uniform`、`evaluate_brdf`/`sample_brdf` 分派辅助

## Data Flow (per frame)

```
glfwPollEvents()
  → ui->update()     (ImGui NewFrame + panels;UI 操作使场景置脏)
  → [debug: if !first_frame && scene->get_need_update() → begin_capture]
  → scene->update()  (dirty-gated: managers 重传 SSBO/重建 TLAS/draw;render.update() 重建描述符)
  → ui_wait    = ui->render()     (MSAA resolve → submit, signal timeline)
  → scene_wait = scene->render()  (raster 或 RT → submit, signal timeline)
  → app->render({ui_wait, scene_wait})
      → recycle_bin.release()
      → CB: wait timeline + clear → acquire swapchain (OutOfDate → end + return)
      → barrier scene+UI→shader read, swapchain→color attachment
      → blend_image 全屏三角形(scene+UI lerp;ocio_conversion 运行时替换)
      → barrier swapchain→present → submit (signal timeline + binary) → present
  → [if save: scene->save_image() — download_image QFOT → OCIO CPU → EXR/PNG]
  → [debug: if begin_capture → end_capture; first_frame = false]
```

## Resources

- **Models** (`resources/models/`): tracked 4 `.glb`(`chess_board.glb`, `chess_board_line.glb`, `chess_piece.glb`, `scene_skybox.glb`);gitignored 工作文件 `chess_all.blend`(+`.blend1`)、`borad.blend`(文件名拼写即如此);`Box.glb`/`Cube.glb`/`Sphere.glb` 为未跟踪测试资产。
- **Fonts** (`resources/fonts/`): 磁盘 14 个字体文件 + `OFL.txt`,但**仅** `LXGWWenKaiGB-Medium.ttf` + `OFL.txt` 被跟踪(其余被 `.gitignore` 的 `*.ttf`/`*.otf` 忽略)。运行时仅加载 Medium 用于 ImGui(13px)与棋盘/棋子图集。
- **Textures** (`resources/textures/`): `cracked ground.hdr`(~90 MB)默认 HDR 天空;运行时生成 `cracked ground.hdr.ktx2`(**UASTC** sidecar,gitignored,加载时转码 BC6H/BC7)。2026-08-15 由中文名 `干裂地面.hdr` 重命名(解决 sidecar 编码问题,见 #29)。
- **OCIO** (`resources/ocios/`): 5 个 ACES 2.0 + OCIO v2.4 configs(studio, D60, all-views, reference, CG) + `aces_conversion_graph.svg`;all-views studio config 用于 CPU save 与 GPU shader 生成。
- **Records** (`resources/records/`): `棋谱1.txt`(GB2312 示例)。
- **Shaders**: 9 `.slang`(`common`/`scene_data` 模块 + 7 entry);编译 `.spv` 缓存同目录(gitignored)。

## Required GPU Features

Extensions (from `vulkan_application.cpp`):
`VK_KHR_swapchain`, `VK_KHR_spirv_1_4`, `VK_KHR_synchronization2`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_KHR_push_descriptor`

Features:
- Base: `samplerAnisotropy`, `fillModeNonSolid`, `multiDrawIndirect`, `shaderInt64`
- Vulkan 1.4: `pushDescriptor` | 1.3: `dynamicRendering`, `synchronization2` | 1.2: `bufferDeviceAddress`, `runtimeDescriptorArray`, `scalarBlockLayout`, `timelineSemaphore` | 1.1: `shaderDrawParameters`
- EXT: `extendedDynamicState`
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
| `imgui` (docking-experimental + glfw/vulkan binding; 1.92.8) | UI |
| `opencolorio` | ACES 2.0 color management |
| `openimageio` | Image I/O |
| `openssl` | SHA-256 hashing |
| `proxy` | Type-erased facade (ui_base) |
| `shader-slang` | Slang → SPIR-V |
| `vulkan-headers` / `vulkan-utility-libraries` / `vulkan-memory-allocator-hpp` (3.3.0) | Vulkan + VMA |
| `ktx` (overlay port, KTX-Software 5.0.0-rc1, UASTC HDR) | BC6H/BC7 压缩 + 加载 |
| `yaml-cpp` (transitive via opencolorio; 显式 find_package) | OCIO config parsing |

## Known Issues & TODOs

扫描基线:2026-08-08(CMake 迁移 + scene_light 重构);Re-scanned 2026-08-12(dirty-gating + TLAS refit,findings 14-23)、2026-08-15(28-31)、2026-08-16(#29 解决)、2026-08-18(#18 修复)、2026-08-26(#20 修复,新增 #32/#33)、2026-08-27(#32 路径更新、#33 解决)。

1. **RT push constants 144 > 128 bytes**(`scene_raytracing_render.h:49-56`): 超规范保证最小值,未查询 `maxPushConstantsSize`。Fix: pack UBO 或门控设备选择。
2. ~~texture_index 守卫~~(FIXED 2026-08-09: 双守卫)。Remaining: `ePartiallyBound` 未启用,未填充 bindless 槽位技术上未定义。
3. **F5 热重载编译错误直接终止**(`scene_manager.cpp:143-146`): 旧 render 先 retire 再构造新的。Fix: 先构造,成功再 retire。OPEN。
4. **OCIO GPU 合成绑定序错位**: layout `[scene, ui, sampler, UBO, tex...]` vs 写入 `[scene, ui, sampler, tex..., UBO]`,自 binding 3 起错位,GPU 变换实际不可用;debug callback 不过滤。
5. ~~空容器 null 描述符写入~~(PARTIALLY FIXED 2026-08-12): model 路径已修(空场景保持有效 TLAS + 1 条目 dummy SSBO),但删光材质/灯光仍写 null 缓冲;`nullDescriptor` 未启用(仅 `VK_KHR_robustness2` 下存在)。
6. **ImGui 多视口验证误报**(1.92.8): 辅助视口未 acquire 即 present → `UNASSIGNED-non-acquired-swapchain-image-used`;debug callback 未过滤。
7. ~~save_image 布局正确性~~(FIXED 2026-08-12: 转换全部在 acquire 侧;release barrier 保持 old_layout)。
8. **BLAS/TLAS 构建队列**: 目前用 graphics 队列;TODO 注释计划迁移 compute(TLAS 需 VK_QUEUE_COMPUTE_BIT)。
9. ~~每条脏帧都更新两条渲染路径~~(FIXED 2026-08-10: 各 manager 自查 is_dirty)。
10. ~~每次脏更新重建全部描述符~~(FIXED 2026-08-10: 仅 any_dirty 时)。
11. ~~CMake/MSVC flags~~(FIXED 2026-08-11: `/utf-8`)。Remaining: x86/Linux/macOS 预设未维护。
12. ~~遗留着色器与重复代码~~(RESOLVED 2026-08-09: 遗留模块复用 common.slang;`utils.slang` 删除)。
13. **Docs/build 迁移**: 已迁 CMake(`.slnx`/`.vcxproj` 删除,vcpkg 子模块);文档已重扫。
14. ~~TLAS refit scratch 尺寸~~(FIXED 2026-08-12: `max(buildScratchSize, updateScratchSize)`,eUpdate 模式后查询)。NOTE: 不存在 ALLOW_UPDATE *create* flag——更新合法性仅由 build flags 决定(VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03759/03760),代码已满足。
15. **空场景(删光模型)→ null 描述符 + 对已 retire TLAS 追踪**(OPEN,2026-08-12 提议修复被作者回退): RT 渲染器无条件 trace → 无 `nullDescriptor` 时非法。ui_record 持有棋盘/棋子 `shared_ptr`,实际不可达;潜在隐患。提议: 保留最后有效 TLAS + renderer skip/clear + dummy SSBO,或启用 `nullDescriptor`。
16. ~~model 过滤不一致~~(FIXED 2026-08-12): 统一在 `update()` 剪枝(expired/null-model_info),四个消费方(meshes/TLAS/draw/SSBO)看到同一列表,保持 `models.size() == draw_commands.size() == ssbo.size()`。
17. **TLAS refit 实例缓冲**: 保留每次 refit 局部缓冲设计(作者 2026-08-12;持久化方案被回退);scratch 持久。Remaining: `upload_buffer` acquire barrier 为 `eShaderRead`,不覆盖 AS 构建输入读(`eAccelerationStructureReadKHR`)——潜在同步缺口,实际可工作。
18. ~~`is_dirty` 在 manager 折叠前清除~~(FIXED 2026-08-18: 移到整个 update 流程成功之后,中途异常保持 dirty 下帧重试)。
19. **Debug RenderDoc 捕获不含 UI uploads**(OPEN): `ui->update()` 先于 begin_capture,UI 发起的 GPU 上传(font atlas/材质纹理)在捕获外。
20. ~~resize/渲染器切换全量重传~~(FIXED 2026-08-26: `is_render_dirty` 标志,场景数据零重传,仅重建描述符)。
21. **`ui_record::restore_board_state()` 用 blanket `need_update()`**(OPEN): 每步棋/选择/加载/初始 resize 都置脏三个 manager;仅 model 的 is_show/matrix 变化。Fix: `need_model_update()`。
22. ~~RT 棋子共享 `custom_index = 2`~~(REFUTED 2026-08-12 — 误报): `InstanceId` 是实例数组索引而非 `instanceCustomIndex`;TLAS 实例数组与 model SSBO 同序,`models[InstanceId()]` 正确。`custom_index = 2u`(`ui_record.cpp:96`)为死数据。残余风险: Slang 升级后需复核 `InstanceIndex()` 映射(BuiltIn 应为 6 而非 5327)。
23. ~~`scene_model_manager::clear()` 未置 dirty~~(FIXED 2026-08-12: 现置 `is_dirty = true`)。
24. ~~delta 光的 MIS 权重 < 1~~(FIXED 2026-08-12: delta 分布 BRDF 采样不可达,正确权重恰为 1.0;旧值系统性变暗)。
25. ~~实例级 `eTriangleCullDisable` 使 ray 级背面剔除失效~~(FIXED 2026-08-12: 移除;透射路径本就在 ray 级关剔除)。
26. ~~合成路径每帧堆分配~~(FIXED 2026-08-12: 单元素 `add_waited_info`/`add_signal_info`)。Remaining(接受): UI/scene/app 每帧三重 `vkWaitSemaphores`。
27. ~~UI 数值边界 bug~~(FIXED 2026-08-12: SliderAngle 度制转换 + scale 钳制 `[0.001, 1000]` 防 NaN)。
28. ~~棋盘"楚河汉界"图集渗墨~~(FIXED 2026-08-15: 字体重载加 `_padding`,三图集均传 `board_font_padding = 2`;未写区域由 clearColorImage 清零。UPDATE 2026-08-18: 棋盘图集加入压缩,4 对齐居中,已核验无需重调模型)。
29. ~~KTX2 sidecar 文件名乱码~~(FIXED 2026-08-15: 源纹理重命名 ASCII + `path += ".ktx2"` path 层面拼接)。
30. **vcpkg baseline 升级后 VMA 头布局**(OPEN): `vk_mem_alloc.h` 在 `include/vma/`,hpp 同目录相对 include → C1083;规避手段为 `out/build/*/vcpkg_installed/.../include/vulkan-memory-allocator-hpp/vk_mem_alloc.h` 手工拷贝,全新构建需重新拷贝。需正式修复。
31. **字体图集单层 mip**(设计选择): `mipLevels=1` + font 采样器 `maxLod=0`,缩小时欠采样闪烁;padding 已隔离渗墨。如需平滑需 mip 链(`upload_image` 无 `eTransferSrc`)。
32. ~~`resources/cache/` 未加入 .gitignore~~(FIXED 2026-08-26,2026-08-27 更新路径: pipeline cache 与 file_watch 缓存同目录,已忽略)。
33. ~~多 `vulkan_pipeline` 实例共享缓存文件~~(FIXED 2026-08-27: 单一 `vulkan_pipeline_cache` 实例,读盘 1 次(启动)/写盘 1 次(退出))。

Note (2026-08-12): MSAA sample count(`pick_msaa_sample_count`)故意取设备最高采样数(开发机实测 8×)——设计选择,非问题。
