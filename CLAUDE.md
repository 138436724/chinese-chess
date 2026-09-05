# CLAUDE.md

## Project Overview

Real-time 3D Chinese Chess (Xiangqi) visualization — **C++23**, **Vulkan 1.4**. Dual rendering: forward MSAA rasterization + path tracing (NEE direct lighting, Lambertian indirect bounce, Russian roulette, temporal accumulation). Slang shaders, ImGui UI (docking + multi-viewport), OpenColorIO ACES 2.0.

Chinese docs: `README_CN.md`.

> 最近一次全量扫描:2026-09-01(常量/函数修饰符提升 + 着色器优化与质量改进:C++ 侧——常量提升 `constexpr`/`constinit`(`scene_manager.h` `color_format`→`static constexpr`、`scene_material_manager.cpp` `image_format`×2、`vulkan_shader_binding_table.cpp` `raygen_offset`、`shader_compiler.cpp` `global_counter`→`constinit`、`vulkan_recycle_bin.h` `output`→`constexpr`),纯函数 `move_piece`/`location_transform`→`constexpr`;流水线创建处 vk 结构字面量(`bindings`/`push_constant`/`pool_size`、RT `shader_groups` 改 `constexpr std::array`)提升;`scene_camera` 7 个平凡 getter 移入头文件 `constexpr`(值类型)。复核回退:GPU 资源包装类 getter 的 constexpr 头文件化(空洞)与 `owner_less` 局部 constexpr(噪音)。proxy support_* 复核:渲染器现为拥有型 `proxy<manager_render>`(`unordered_map`)+ `proxy_view`,`nothrow` 已是最严(见 Type Erasure 节)。着色器——`lighting.slang` 清理 ~150 行死代码、`sample_ggx_brdf` 内联评估、整数幂改乘法;**GGX 间接弹射改真正 VNDF 采样(Heitz 2018)**(pdf=D·G1(V)/(4·NdotV),value=F·G1(L),方差更低);**firefly 钳制仅累积帧生效**(`frame_index>0`,修首帧/相机移动压暗,RT/RQ 同步);合并重复 `DispatchRaysIndex`);2026-08-31(光线追踪路线 A 落地 + SPIR-V 1.6 升级:新增 `scene_rayquery_render` + `ray_query.slang`、`render_mode` 三模式枚举(光栅化/RT Pipeline/Ray Query);ray query 与 RT pipeline 行为对齐(时域累积/间接弹射/高度雾),A2 实验模式与实验 UI 已按作者要求移除;Slang 2026.7.1 RayQuery 兼容问题(无候选确认 API / CommittedStatus 恒 None / CandidateInstanceID 错映射到自定义索引 / TraceRayInline 参数顺序)已逐一探测并规避,详见 `ray-tracing-extensions-plan.md` §12.6;shader_compiler profile 升 `spirv_1_6`(移除 `VK_KHR_spirv_1_4` 扩展)、启用 `shaderIntegerDotProduct`、RNG 种子改用整数点积 OpUDot;着色器缓存不感知 profile 等编译器选项,改动后需手动删除 `resources\cache\shader_cache.cache`);2026-08-29(着色器缓存实为单一序列化文件 `resources\cache\shader_cache.cache`(非逐文件 .spv)、file_watcher 缓存实为 `hash_cache.cache`、F5 行号修正、swapchain getter noexcept 现状复核、新增 #34;2026-08-29 复核修正:#5(代码无 dummy SSBO,空场景 TLAS 直接 retire)、#8(改判 FIXED;队列族 2026-09 再核为 graphics,见 #8 当前文本)、#27(scale 钳制实为 `[0.001, 10000]`);2026-08-29 代码整理:include 按 IWYU 增删(移除 5 处未使用、补充约 28 处缺失,顺序由 clang-format 保证)、`file_watcher::is_file_modified()` 改 `const` + mutable 缓存;容器下标硬化仅 `record_loader.cpp` 保留 `charAt()`(ICU UnicodeString 的 `operator[]` 与 `charAt` 同实现、零开销),`vulkan_physical_device.h` 的 `.at()` 因编译失败由作者回退为 `[]`);2026-09(Vulkan 合规审计与文档同步,完整条目见根目录 `VULKAN_ISSUES.md`:H2 push constant 全库弃用 RESOLVED、H4 `meshes` 改 `shared_ptr` + `use_count()==1` 剪枝 FIXED、M1 空场景构建 0 实例空 TLAS(期间评估 `VK_KHR_robustness2` 后确认无需并移除)、M2 描述符池 `eUpdateAfterBind` 死标志清除、M3 `upload_buffer`/`upload_image` 显式消费者 stage/access 参数(纹理 `eShaderSampledRead`、AS 输入 `eAccelerationStructureReadKHR`)、M4 swapchain imageCount 钳制修正、M5 F5 编译失败保留旧管线不再 terminate、M6 `wait_idle` 经 `explicit operator bool` 判空守卫、M8 移除 6 项未用特性/扩展;`/W4` 编译告警;渲染器每帧参数由 per-frame 场景 UBO 承载(见 GPU Data Structures))。注意:不要假设 `.slnx`/`.vcxproj` 存在(已删除,CMake 迁移);`PBR` 与 `heap` 分支为实验分支未并入 main,`spectral-rendering-plan.md` 仅存在于 `PBR` 分支。本文档刻意不写提交哈希(作者会改写 git 历史),统一用日期/描述定位改动。

## 工作区布局与检索约定

- 工作区根 = 本文件所在目录,内含**同名嵌套源码目录 `chinese-chess/`**(源码根:window/vulkan_core/scene/ui/tools/resources 均在其下,CLAUDE.md 所述"项目根/工作目录"即它);`CLAUDE.md`/`README.md`/`README_CN.md` 位于工作区根。
- `out/`(CMake 构建产物)、`vcpkg/`(依赖子模块)、`vcpkg_installed/`(vcpkg 安装头/库,工作区根与 `out/build/*/` 下均有)均为**生成/依赖产物,全库检索(grep/glob/文件枚举)一律排除**;检索范围限定 `chinese-chess/` 源码子目录与工作区根级文档。
- 光线追踪扩展/API 选型计划见 `ray-tracing-extensions-plan.md`(光线追踪调研产出,评审稿)。

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
scene/           Scene graph — models, materials, lights, camera, 3 render modes (raster/RT/ray query)
ui/              ImGui panels (docking, proxy pattern), 4 sub-panels
tools/           Utilities: shader compiler, model loader, image I/O, OCIO, font, record parser, file watcher, string helper, RenderDoc capture
```

### Source directory summary

| Dir | Files | Key classes / roles |
|-----|-------|---------------------|
| `window/` | 2 cpp, 1 h | `glfw_window`: GLFW RAII, frame loop, input dispatch, screenshot, RenderDoc capture (debug) |
| `vulkan_core/` | 17 pairs | `vulkan_application` (orchestrator, owns `vulkan_pipeline_cache`), `vulkan_common` (format selection, upload/download QFOT, `MAX_FRAMES_IN_FLIGHT`/`USE_OCIO`/`MSAA_SAMPLE_COUNT`/`DEPTH_FORMAT` 全局), `vulkan_device`, `vulkan_physical_device` (cartesian_product QF selection), `vulkan_swapchain` (Mailbox/FIFO, per-image binary semaphores), `vulkan_buffer`/`vulkan_image` (VMA + state tracking), `vulkan_commandbuffer` (timeline semaphore), `vulkan_queue`, `vulkan_semaphore` (timeline + atomic counter), `vulkan_recycle_bin`, `vulkan_pipeline` (graphics + RT;缓存句柄由参数传入), `vulkan_pipeline_cache` (磁盘缓存加载/保存 + 设备校验), `vulkan_descriptor` (multi-frame variant writes), `vulkan_sampler` (per-type RAII), `vulkan_acceleration_structure` (BLAS/TLAS), `vulkan_shader_binding_table` |
| `scene/` | 12 pairs | `scene_manager` (orchestrator, F5/F6 hot-reload, save_image, `is_render_dirty`), `scene_base` (facades: `manager_base`/`manager_render`), `scene_camera`, `scene_light` (flat struct), `scene_material`, `scene_model` (transform+BLAS, `is_show`), `scene_model_manager` (vertex/index buffers, BLAS/TLAS, indirect draw, model SSBO), `scene_material_manager` (material SSBO, bindless textures, 字体图集压缩, KTX2, F6 热重载), `scene_light_manager` (light SSBO), `scene_rasterization_render`, `scene_raytracing_render`, `scene_rayquery_render` (compute + ray query 渲染器,与 RT pipeline 行为一致) |
| `ui/` | 6 pairs | `ui_manager` (ImGui init/render, 渲染模式切换), `ui_base` (`pro::proxy` facade), `ui_record` (棋谱回放), `ui_camera`, `ui_light`, `ui_node` (materials + models). NOTE: `ui_memory` 已不存在。 |
| `tools/` | 9 cpp, 10 h | `shader_compiler` (Slang→SPIR-V, 依赖感知着色器缓存, `std::expected`), `model_loader` (fastgltf, `std::expected<model_data, std::string>`), `image_helper` (OIIO + KTX2 压缩, `paste_image`/`convert_channels`), `ocio_helper`, `font_loader`, `record_loader` (ICU4C 记谱解析), `file_watcher` (SHA-256, 缓存 `resources\cache\hash_cache.cache`, `is_file_modified()` const + mutable 缓存), `string_helper`, `renderdoc_capture` |

## Shaders (10 Slang files in `resources/shaders/`)

| File | Pipeline | Role |
|------|----------|------|
| `rasterization.slang` | Graphics | Forward MSAA: bindless textures, indirect draw, model/material SSBO. Guards both `material_index` and `texture_index` |
| `ray_tracing.slang` | RT (5 entry points) | Path tracer: NEE(delta 光源 MIS 权重恒 1), GGX(VNDF 采样)/Disney lobe selection, jade transmission, height fog, ambient, Russian roulette, temporal accumulation(firefly 钳制仅累积帧生效), HDR sky |
| `ray_query.slang` | Compute | compute + ray query 路径追踪（与 RT pipeline 行为一致：NEE/间接弹射/高度雾/时域累积；手动最近命中规避 Slang 2026.7.1 兼容问题，见文件头；SPIR-V 1.6 整数点积种子） |
| `lighting.slang` | (module) | Light sampling, BRDFs(evaluate_ggx / VNDF sample_ggx_brdf / Disney), shadow rays, sky sampling, height fog |
| `common.slang` | (module) | Constants, Wang hash RNG, math/color helpers, tangent space, Hammersley, fullscreen triangle vertex |
| `scene_data.slang` | (module) | Shared GPU structures aligned with C++ (`Vertex`, `model_data`, `material_data`, `light_data`) + 场景参数 UBO (`raster_scene_data`/`rt_scene_data`,std140,替代原 push constant) |
| `blend_image.slang` | Graphics | Scene+UI alpha composite; `ocio_conversion()` stub body replaced by `ocio_helper` (self-contained, no imports) |
| `scene_skybox.slang` | Graphics | Legacy, unused: HDR cubemap + Uncharted 2 tone mapping |
| `scene_brdflut.slang` | Graphics | Legacy, unused: BRDF split-sum LUT |
| `scene_cubemap.slang` | Graphics | Legacy, unused: equirect→cubemap (MRT) |

Dependency graph:

```
common.slang ← scene_data.slang ← lighting.slang ← ray_tracing.slang
rasterization.slang uses common / scene_data
ray_query.slang uses common / scene_data / lighting
blend_image.slang is self-contained (no imports)
scene_skybox / scene_brdflut / scene_cubemap are legacy modules (reuse common)
```

Note: `utils.slang` was removed (replaced by `common.slang`).

### GPU Data Structures (shared C++/Slang, std430-style)

Byte sizes re-verified 2026-08-16 (MSVC `sizeof` + SPIR-V `OpMemberDecorate`/`ArrayStride`); C++ 与 GPU 布局完全一致。Slang/std430 将 `float3` 打包为 12B 但 16B 对齐,`alignas(16)` 只提升 offset 不影响 extent。

访问路径(2026-09 起):`model_data`/`material_data`/`light_data` 数组以 GPU 存储缓冲上传(名字沿用 `ssbo`),但渲染期**不再作为 SSBO 描述符绑定**——设备地址与相机矩阵每帧写入场景参数 UBO(见下),shader 经指针成员直接解引用。

- **model_data**: `mat4 model_matrix; uint32_t material_index; Vertex* vertex_address; uint32_t* index_address;` offsets 0/64/72/80, **sizeof 96B**
- **material_data**: `float3 background_color; uint32_t texture_index; float3 foreground_color; float roughness, metallic, opacity, ior, transmission;` offsets 0/12/16/28/32/36/40/44, **sizeof 48B**;RT-only 字段默认 opacity=1, ior=1.5, transmission=0
- **light_data**: `float3 color; uint32_t active_type; float3 direction; float intensity; float3 position; float range; float inner/outer_cone_angle;` offsets 0/12/16/28/32/44/48/52, **sizeof 64B**
- **Vertex** (`scene_data.slang` 唯一共享定义): `{position, normal, uv}` = CPU `model_vertex` (32B);光栅顶点输入仅 `{position, uv}`
- **场景参数 UBO(替代原 push constant,2026-09)**: `scene_data.slang` 定义 `raster_scene_data`(160B)/`rt_scene_data`(176B,std140)——相机矩阵、model/material(/light) 数组设备地址、计数、`texture_count`/`skybox_index`/`frame_index`。三个渲染器各持有 `vulkan_buffer ubo` + `ubo_offset`,容量 = `MAX_FRAMES_IN_FLIGHT` 槽(host 映射 + 每帧 `flush()`),每帧写当前帧槽。渲染器不再声明/推送任何 push constant,`vulkan_pipeline` 保留 `_push_constant` 参数(恒传空 span)为未来用法留接口。原 144B 超限问题随结构体移除而消失(见 Known Issue #1 / VULKAN_ISSUES H2)。

## Key Design Patterns

### Two-Step Init
`vulkan_application`: `init(instance_layers, extensions)` → instance;`create(surface, w, h)` → device, swapchain, pipeline。Surface(依赖 instance)在两步之间创建。

### Physical Device Selection
按 Vulkan 1.4 + 扩展 + feature 谓词过滤,`std::views::cartesian_product` 穷举 (graphics, compute, transfer, present) 队列族组合,10× 加分专用传输队列,最大化独立队列族数量。

### Timeline Semaphore + Recycle Bin
`vulkan_semaphore` = 时间线信号量 + 原子 CPU 计数器;`vulkan_recycle_bin` 将资源与 CPU 计数配对,`release()` 在 GPU 越过该值后销毁。CB 在 submit 时 signal(2026-09 起 **begin_record 不再 wait**——一次性 CB 立即提交,帧槽 pacing 由 `app->wait_frame()`(主循环顶部等该槽上次 signal)承担;回收安全依赖"帧 CB GPU wait 全部 upload"纪律,见 VULKAN_ISSUES H3 降级说明)。帧循环无 `vkDeviceWaitIdle`(仅销毁/resize 调用)。Debug 回收日志由 `output = false` 常量经 `if constexpr` 编译期关闭(2026-08-26;实际声明为 `constexpr inline static bool output = false`,`vulkan_recycle_bin.h:29`,2026-09-01 提升 constexpr)。

### 析构顺序加固 (2026-08-27)
默认析构 = 成员声明逆序,命令缓冲/描述符集/allocator/swapchain 生命期约束声明顺序已满足,此处只修"引用先销毁"隐患:
- **`~vulkan_application()`**: 先 `wait_idle()`(`vulkan_application.cpp:37-41`)——`wait_idle()` 以 `vulkan_device` 的 `explicit operator bool()` 判空(`if (!device) return;`,`:184-192`;**`create()` 中途异常时部分构造对象析构不再对 null raii Device 调 waitIdle**,见 VULKAN_ISSUES M6 / Known #34 FIXED),再 `descriptor.clear_descriptor_info()` 销毁描述符池/集——解除对 scene/ui `render_output`、`ocio_images`/`ocio_samplers`、`image_sampler` 的引用,随后成员逆序析构才安全。
- **`~scene_manager()` / `~ui_manager()`**: 析构开头 `app.wait_idle()`(`scene_manager.cpp:22`、`ui_manager.cpp:32`);原 `destroy()` 已合并进析构(ui_manager 执行 ImGui 三个 Shutdown;scene_manager 执行各 manager `clear()`),`glfw_window` 析构不再调用。要求 app 在 scene/ui 之后析构、GLFW window 在 ui 之后销毁(ImGui_ImplGlfw_Shutdown 需 window 回调仍有效)。
- **`vulkan_acceleration_structure`**: `buffer` 改为在 `acceleration_structure` **之前**声明 → AS 先于 backing buffer 销毁;move 构造/赋值同步。

### Resource State Tracking
`vulkan_buffer`/`vulkan_image` 经 `set_info()` 追踪 stage/access/layout/queue,barrier 基于实际当前状态生成。

### Queue Ownership Transfer (upload_image / download_image)
Upload: 传输队列 release(不改布局)→ 图形 acquire + `eShaderReadOnlyOptimal`。Download: 图形 release → 传输 `eTransferSrcOptimal` 拷贝 → 图形 re-acquire 恢复原布局。

### Manager Architecture (since 2026-07-28)
`scene_model_manager` 持有顶点/索引缓冲、每网格 BLAS、共享 TLAS、间接绘制命令缓冲(`is_show` → instanceCount 0/1;RT mask 0xFF/0)、模型 SSBO;`scene_material_manager` 持有材质 SSBO + bindless 纹理数组(≤1024 槽,含字体图集);`scene_light_manager` 持有灯光 SSBO。各 manager 自查单一 `is_dirty`(`create()`/真删除置脏,`update()` 消费并返回 `bool`),`scene_manager::update()` 折叠为 `any_dirty` 并叠加 `is_render_dirty`(resize/渲染器切换,2026-08-26):`any_dirty || is_render_dirty` 时重建描述符,否则仅重置 RT 累积。UI 经 `need_update()`/`need_camera_update()`/`need_material_update()`/`need_model_update()`/`need_light_update()` 路由。TLAS:实例数不变仅 refit(`eUpdate` + 持久 scratch;实例缓冲每次更新重建局部缓冲,见 #17),增删模型才完全重建。

### Bindless Descriptors
描述符池/集仅在 `any_dirty || is_render_dirty` 时重建;相机-only 更新永不触碰描述符。

### 三渲染模式 (since 2026-08-31)
UI"渲染模式"Combo(光栅化 / RT Pipeline / Ray Query,`ui_manager.cpp:228-237`)调 `scene_manager::set_render_mode(render_mode)`(`render_mode` 枚举于 `scene_manager.h`);`scene_renders` 以 `unordered_map<render_mode, pro::proxy<manager_render>>` 持有 3 个渲染器,`active_render` 为 `proxy_view`,构造默认绑定 `ray_tracing`(scene_manager.cpp:75)。切换 = rebind + `active_render->resize()` + 置 `is_dirty`+`is_render_dirty`(场景数据零重传、仅重建描述符并重置累积,#20)。三模式共享 `R16G16B16A16_SFLOAT` render_output;`resize()` 重建 render_output 并仅 resize active renderer;`recreate()` = F5 仅重编译 active renderer shaders。

### F5 Hot-Reload
`handle(F5)` → `active_render->recreate()`(`manager_render` 约定 `mem_render_recreate`)+ `active_render->update()` 重建描述符(pipeline layout 可能变化)。三个渲染器 `create_pipeline*` 均先**构建新对象**(编译抛异常时旧管线分毫未动),`recreate()` 成功后才 retire 旧 pipeline/SBT 并 move 替换;`scene_manager::handle(F5)` 捕获编译异常,打印 `recreate failed, keep pipeline: …`(`scene_manager.cpp:161`)并保留旧管线(#3 FIXED,见 VULKAN_ISSUES M5)。复用 app 持有的同一 `vulkan_pipeline_cache` 句柄,**不重读盘、不写盘**。

### F6 Texture Hot-Reload (KTX2)
`handle(F6)` → `material_manager.reload_textures(waited_infos)`,返回 `true` 时 `need_material_update()` + `need_model_update()`。**单线程**遍历 `images_cache`:字体图集键是合成路径,`exists` 检查天然排除;对真实文件先 SHA-256 判变,变则内存压缩 → 同步写 UASTC sidecar → `upload_ktx2` 转码上传新 `vulkan_image`,`recycle_bin` 同槽位换图(texture_index 稳定,旧 imageview 在 GPU 越过计数后销毁),失败保留旧纹理并打印错误。`upload_image` 上传前 `clearColorImage` 清空(压缩格式跳过),未写区域恒为 0。

### Pipeline Cache (since 2026-08-26,2026-08-27 定型)
- **归属**: 独立 RAII 类 `vulkan_pipeline_cache`,由 `vulkan_application` 持有(`get_pipeline_cache()`),`pipeline_cache.create(*physical_device, *device)` 在 `vulkan_application::create()` 调一次(`vulkan_application.cpp:72`);`vulkan_pipeline::create`/`create_from_shader` 与两个渲染器均以 `const vk::raii::PipelineCache&` 共享同一句柄——**单实例**。
- **加载**: 读 `resources\cache\pipeline_cache.cache`,以 `vk::PipelineCacheHeaderVersionOne` 校验 vendorID/deviceID/UUID;缺失/不匹配/损坏(捕获 `vk::SystemError`)回退空缓存,不阻塞启动。
- **保存**: 仅 `glfw_window::~glfw_window()` 中 `app->wait_idle()` 后调用一次(`window.cpp:71-72`);**F5 不写盘**。条目按完整 create info(含 SPIR-V)键控,旧 shader 自然 miss。
- **`create_from_shader`**: 编译与创建合并进 `vulkan_pipeline`;`shader_stage_info{name, stage}` 定义于 `vulkan_pipeline.h`;`compile_shader` 失败抛 `std::runtime_error`(含 Slang 诊断)。
- **依赖感知着色器缓存**(`shader_compiler`,2026-08-28): 路径编译先按存的依赖路径逐个重算 SHA-256、排序拼接后与 `fingerprint` 比对,一致 → 直接返回缓存 SPIR-V,**完全不经过 Slang(loadModule/依赖分析)**;任一文件(含入口 shader——Slang 的依赖列表包含模块自身,故无需单独 entry_hash)变化即失配,才走完整路径(`loadModule` 模块名 = stem → `get_dependency_and_fingerprint` 遍历 `getDependencyFileCount/Path` 的 import 链(含 shader 自身)重新收集依赖并编译)。缓存为**单一序列化文件** `resources\cache\shader_cache.cache`(魔数 `shader_cache_header` + 条目 {路径, dependence, fingerprint, SPIR-V 字节};`dependence` 为依赖路径拼接、`fingerprint` 为哈希排序拼接,**两者共用同一分隔符** `'\n'`(`split_char`)),ctor 读入、析构写回(先剔除源文件已不存在的条目)——**不再生成逐 shader 的 .spv 文件**。字符串编译走 `loadModuleFromSourceString`(不缓存);辅助名 `slang_module_to_spv`。

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
`ui_base` = `pro::facade_builder`(P0779R0);`ui_manager` 以 `std::vector<pro::proxy<ui_base>>`(拥有型)持有 ui_camera/ui_light/ui_node/ui_record。`scene_base` 定义 `manager_base`/`manager_render`:`scene_manager` 以拥有型 `pro::proxy<manager_render>` 存于 `unordered_map<render_mode, pro::proxy<manager_render>>`(3 个渲染器),`active_render` 为非拥有 `proxy_view<manager_render>`(由拥有型 proxy 经 `as_view` 转换);三个 manager 仍为 `proxy_view<manager_base>`(非拥有,直接成员)。`ui_base`/`manager_render` 均显式 `support_relocation`/`support_destruction` 为 `constraint_level::nothrow`(2026-08-27 由 nontrivial 收紧;`trivial` 需平凡析构,ui_*/scene_*_render 含 vector/shared_ptr/vk::raii 不满足)。2026-09-01 复核(proxy 4.1.0 头文件):约束检查作用于**存储类型**(`inplace_ptr`/`allocated_ptr`)而非对象本身;`nothrow` 已是可编译前提下的最严——`support_destruction<trivial>` 因堆存储 `allocated_ptr` 有用户析构(deallocate)而 `proxiable` 失败;`support_relocation<trivial>` 空洞(大对象恒走堆、约束落在堆指针上)且会堵死将来 `restrict_layout` 内联存储;`support_copy` 因 scene_*_render 不可拷贝(引用成员 + vk::raii)不可加;facade 默认 relocatability=trivial/destructibility=nothrow,显式 nothrow 为文档化而非放松。

### Factory with Concepts
`scene_manager::create<T>(args...)` 以 concepts 约束 T,分派到对应 manager 私有 `create(type_identity<T>, ...)`。

### 修饰符规范 (2026-08-27 扫描确认)
getter 一律 `[[nodiscard]] const noexcept`;proxy 约定成员(manager_base/manager_render/ui_base 的 update/clear/resize/render/recreate/reset_accumulation/handle)**不加修饰符**(改签名破坏 proxy invocable 检查);可抛函数(容器操作/I/O/vk::* 调用/format)不标 noexcept——错标会 `std::terminate`。**`[[nodiscard]]` 只写在 .h 声明处,成员函数的 .cpp 定义不重复标注**(作者 2026-08-27 手动清理;唯一例外:匿名命名空间内部自由函数无 .h 声明,必须留在定义处)。已移除的错误 noexcept:`vulkan_common` 的 `find_supported_format`/`upload_buffer`/`upload_image`/`download_image`(实际可抛)——异常现可正常传播。`vulkan_swapchain` 的 `get_current_image`/`get_current_imageview` 按 getter 规则**保留 noexcept**(`vulkan_swapchain.h:33-34`,定义同 `vulkan_swapchain.cpp:179/184`),函数体以 `images.at()`/`imageviews.at()` 取当前帧索引——运行期索引恒有效,越界仅理论可能(届时 noexcept 内抛异常 → terminate);作者决定以当前代码为准,保持现状(2026-08-28 复核确认)。2026-09-01 修饰符提升扫描:常量/纯函数优先 `constexpr`/`constinit`(`shader_compiler.cpp` `global_counter`→`constinit`;`record_loader.cpp` `move_piece`、`ui_record.cpp` `location_transform`→`constexpr`;`vulkan_recycle_bin.h` `output`→`constexpr`;`scene_manager.h` `color_format`→`static constexpr`;`scene_material_manager.cpp` 两处 `image_format`→`constexpr`;`vulkan_shader_binding_table.cpp` `raygen_offset`→`constexpr`)。流水线创建处 vk 结构字面量局部(`bindings`/`push_constant`/`pool_size`、RT `shader_groups` 由 `std::vector` 改 `constexpr std::array`、`vulkan_application.cpp` debug 标志位)提升 `constexpr`——vulkan.hpp 结构构造器本身为 constexpr,仅 vk::* 设备调用非编译期。getter 例外:`scene_camera` 7 个平凡 getter 移入头文件并 `constexpr`(值类型,glm 数学 C++23 constexpr,`constexpr scene_camera` 对象可编译期求值);GPU 资源包装类(vulkan_buffer/image/queue/swapchain/AS/semaphore)的值返回 getter **保持 .cpp 定义**——移头文件无编译期价值,2026-09-01 复核后回退,维持 `[[nodiscard]] const noexcept` 约定;`owner_less` 局部 `constexpr` 同理回退(无信息量)。

### mutable 缓存 + const 查询 (2026-08-29)
记忆化缓存属于非逻辑状态,声明 `mutable` 使查询语义的函数保持 `const`:已应用于 `shader_compiler::shader_cache`(既有,`compile_shader_to_spv() const` 内更新)与 `file_watcher::file_watch_cache`(`is_file_modified() const`,2026-08-29 对齐);真正有状态的操作(`create`/`update`/`render`/`clear`/`next`/`set_info`/`retire`/`release`)保持非 const。

### Path Tracing Pipeline (current implementation)

- **NEE**: 对每个光源采样(delta 分布 pdf=1),阴影光线(any-hit + `ACCEPT_FIRST_HIT_AND_END_SEARCH` + `SKIP_CLOSEST_HIT` + 背面剔除;半透明按 `opacity` 概率穿透),贡献权重 `w_light` 恒 1(delta 光源 BRDF 采样不可达,NEE 是唯一采样策略,见 #24);diffuse `(1-kS)(1-metallic)albedo/π` + GGX Cook-Torrance specular;单光 firefly 钳制 `min(luminance*100, 100)`
- **Ambient**: `AMBIENT_LIGHT`(0.25)以 `材质色 × 0.25 × (0.5 + 0.5·NdotV)` 叠加,完全阴影区保持可见
- **间接弹射(波瓣选择)**: 三分支——透射(`opacity×transmission`,玉石折射 + Beer-Lambert 吸收)、GGX 镜面(VNDF 重要性采样,Heitz 2018,2026-09-01 落地)、Disney 漫反射;吞吐量 `×= lobe_value/(lobe_prob·p_continue)`,`MAX_THROUGHPUT=10`
  - 玉石透射: `absorption_density = 12 + 30×texture_weight`,仅进入时 `T = exp(-density·PIECE_THICKNESS)`;`texture_weight`(alpha 字形权重)经 `HitPayload` 透传
- **Height fog**: 解析指数雾,closest-hit 段与 miss 均应用,含太阳散射
- **Russian roulette / max depth**: `p_continue = min(1, luminance)`,< 0.05 截断;`MAX_BOUNCES = 8`(二级 TraceRay 递归)
- **Temporal accumulation**: `alpha = 1/(1 + frame_index/2)`;frame_index 场景更新时重置;jitter `random_float2` 以 `wang_hash(pixel·constants + frame_index)` 种子;accumulated 亮度 4× firefly 钳制**仅 `frame_index > 0` 时生效**(2026-09-01:重置后首帧 alpha=1 整帧替换,拿陈旧均值钳制会把新采样错误压暗)
- **Sky**: `skybox_index != 0xFFFFFFFF` 时采样 HDR equirect(`sample_hdr_sky`),过暗回退程序化渐变天空
- **Chess pieces**: 白玉 `(0.95,0.92,0.85)` + 深红刻字 / 青玉 `(0.15,0.45,0.32)` + 墨绿刻字;`opacity=0.8, ior=1.5, transmission=1.0, roughness=0.3`,材质面板可调
- 主循环未使用: BRDF LUT(`scene_brdflut.slang`);`lighting.slang` 的零引用死代码已移除(统一 BRDF 调度 `evaluate_brdf`/`sample_brdf`/`pdf_brdf`、`sample_hemisphere_uniform`、`sample_uniform_sphere`、`power_heuristic`、`evaluate_height_fog_between` 等,2026-09-01)

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
- **Shaders**: 10 `.slang`(3 模块 `common`/`scene_data`/`lighting` + 7 个 entry 文件:`rasterization`、`ray_tracing`、`ray_query`、`blend_image`、`scene_skybox`、`scene_brdflut`、`scene_cubemap`);编译缓存为 `resources\cache\shader_cache.cache` 单一序列化文件(gitignored,见 Pipeline Cache 段)。

## Required GPU Features

Extensions (from `vulkan_application.cpp`):
`VK_KHR_swapchain`, `VK_KHR_synchronization2`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_KHR_ray_query`

Features (当前实际请求集,谓词/查询链与启用链 1:1;2026-09 审计见 VULKAN_ISSUES M8):
- Base: `samplerAnisotropy`, `multiDrawIndirect`, `shaderInt64`
- Vulkan 1.3: `dynamicRendering`, `synchronization2`, `shaderIntegerDotProduct`(SPIR-V 1.6 整数点积) | 1.2: `bufferDeviceAddress`, `runtimeDescriptorArray`, `scalarBlockLayout`, `timelineSemaphore` | 1.1: `shaderDrawParameters`
- AS: `accelerationStructure`
- RT: `rayTracingPipeline`, `rayTraversalPrimitiveCulling`
- RQ: `rayQuery`

> 2026-09(M8)已移除的"请求并启用但无使用点"项:`VK_KHR_push_descriptor`(扩展)与 `pushDescriptor`(Vulkan 1.4)、`extendedDynamicState`(EXT)、`fillModeNonSolid`、`rayTracingPipelineTraceRaysIndirect`、`accelerationStructureCaptureReplay`、`descriptorBindingAccelerationStructureUpdateAfterBind`——删除后放宽设备选择(见 VULKAN_ISSUES M8/M2)。

着色器目标 profile = `spirv_1_6`(Vulkan 1.3+ 原生支持;`VK_KHR_spirv_1_4` 扩展已移除,SPIR-V 1.5/1.6 无扩展、随核心版本)。ray_query.slang 的 RNG 种子使用整数点积 `OpUDot`(上表 `shaderIntegerDotProduct`)。着色器缓存不感知 profile 等编译器选项,改动后需手动删除 `resources\cache\shader_cache.cache`。

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

扫描基线:2026-08-08(CMake 迁移 + scene_light 重构);Re-scanned 2026-08-12(dirty-gating + TLAS refit,findings 14-23)、2026-08-15(28-31)、2026-08-16(#29 解决)、2026-08-18(#18 修复)、2026-08-26(#20 修复,新增 #32/#33)、2026-08-27(#32 路径更新、#33 解决)、2026-08-28(#3 引用修正、#34 现状记录、缓存文件形态/路径修正、swapchain noexcept 保留确认)、2026-08-29(#5 修正、#8 改判 FIXED、#27 数值修正)、2026-09(#1/#3/#5/#15/#17/#34 状态更新与 M 系列审计,详见根目录 `VULKAN_ISSUES.md`;push constant 弃用改场景 UBO、F5 保旧管线、空场景空 TLAS、显式消费者 stage/access、`wait_idle` 守卫、swapchain imageCount 钳制、未用特性/扩展移除、`/W4`)。

1. ~~**RT push constants 144 > 128 bytes**~~(RESOLVED 2026-09: 渲染器已不再声明/推送 push constant 结构体,`vulkan_pipeline` 保留 `_push_constant` 参数恒传空 span 为未来留接口;144B 超限问题随结构体移除消失。见 VULKAN_ISSUES H2)。
2. ~~texture_index 守卫~~(FIXED 2026-08-09: 双守卫)。Remaining: `ePartiallyBound` 未启用,未填充 bindless 槽位技术上未定义。
3. ~~**F5 热重载编译错误直接终止**~~(FIXED 2026-09,见 VULKAN_ISSUES M5): 三个渲染器 `create_pipeline*` 改为返回新构建对象,`recreate()` 先构建成功再 retire 旧的并 move 替换;`scene_manager::handle(F5)` 捕获编译异常,打印 `recreate failed, keep pipeline: {...}`(`scene_manager.cpp:161`)并保留旧管线,不再 terminate。
4. **OCIO GPU 合成绑定序错位**: layout `[scene, ui, sampler, UBO, tex...]` vs 写入 `[scene, ui, sampler, tex..., UBO]`,自 binding 3 起错位,GPU 变换实际不可用;debug callback 不过滤。(OPEN,见 VULKAN_ISSUES H1)
5. ~~**空容器 null 描述符写入**~~(FIXED 2026-09,见 VULKAN_ISSUES M1): `update_tlas()` 在模型为空时不再 retire TLAS,改为构建 **0 实例空 TLAS**——描述符恒绑有效 AS 句柄,trace 空 TLAS = 0 命中 → miss → 天空盒仍渲染。材质/灯光空容器路径由 shader count 守卫兜底。期间曾评估 `VK_KHR_robustness2` nullDescriptor,实测 0 实例空 TLAS 后确认无需该特性,已移除。
6. **ImGui 多视口验证误报**(1.92.8): 辅助视口未 acquire 即 present → `UNASSIGNED-non-acquired-swapchain-image-used`;debug callback 未过滤。
7. ~~save_image 布局正确性~~(FIXED 2026-08-12: 转换全部在 acquire 侧;release barrier 保持 old_layout)。
8. ~~BLAS/TLAS 构建队列~~(FIXED 2026-08-29 复核,2026-09 再核: **实际在 graphics 队列**): `scene_model_manager::update_meshes()` 与 `update_tlas()` 的命令缓冲均从 `graphic_queue` 的命令池创建并以 `graphic_queue.get_index()` 提交(`scene_model_manager.cpp:188-366`);`compute_queue` 现由 `scene_manager` 持有并创建(`scene_manager.cpp:35`)但全库零提交,为未来保留。
9. ~~每条脏帧都更新两条渲染路径~~(FIXED 2026-08-10: 各 manager 自查 is_dirty)。
10. ~~每次脏更新重建全部描述符~~(FIXED 2026-08-10: 仅 any_dirty 时)。
11. ~~CMake/MSVC flags~~(FIXED 2026-08-11: `/utf-8`)。Remaining: x86/Linux/macOS 预设未维护。
12. ~~遗留着色器与重复代码~~(RESOLVED 2026-08-09: 遗留模块复用 common.slang;`utils.slang` 删除)。
13. **Docs/build 迁移**: 已迁 CMake(`.slnx`/`.vcxproj` 删除,vcpkg 子模块);文档已重扫。
14. ~~TLAS refit scratch 尺寸~~(FIXED 2026-08-12: `max(buildScratchSize, updateScratchSize)`,eUpdate 模式后查询)。NOTE: 不存在 ALLOW_UPDATE *create* flag——更新合法性仅由 build flags 决定(VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03759/03760),代码已满足。
15. ~~**空场景(删光模型)→ null 描述符 + 对已 retire TLAS 追踪**~~(FIXED 2026-09,与 #5 同因,见 VULKAN_ISSUES M1): 现构建 0 实例空 TLAS,描述符恒绑有效 AS;RT 渲染器无条件 trace 安全(空 TLAS = 0 命中)。
16. ~~model 过滤不一致~~(FIXED 2026-08-12): 统一在 `update()` 剪枝(expired/null-model_info),四个消费方(meshes/TLAS/draw/SSBO)看到同一列表,保持 `models.size() == draw_commands.size() == ssbo.size()`。
17. **TLAS refit 实例缓冲**: 保留每次 refit 局部缓冲设计(作者 2026-08-12;持久化方案被回退);scratch 持久。~~Remaining: `upload_buffer` acquire barrier 为 `eShaderRead`,不覆盖 AS 构建输入读~~(FIXED 2026-09,见 VULKAN_ISSUES M3: `upload_buffer`/`upload_image` 增加 `_consumer_stages`/`_consumer_access` 参数,各调用点按真实消费者传参——几何/实例缓冲含 `eAccelerationStructureReadKHR`,draw 含 indirect read,纹理含 `eShaderSampledRead`)。
18. ~~`is_dirty` 在 manager 折叠前清除~~(FIXED 2026-08-18: 移到整个 update 流程成功之后,中途异常保持 dirty 下帧重试)。
19. **Debug RenderDoc 捕获不含 UI uploads**(OPEN): `ui->update()` 先于 begin_capture,UI 发起的 GPU 上传(font atlas/材质纹理)在捕获外。
20. ~~resize/渲染器切换全量重传~~(FIXED 2026-08-26: `is_render_dirty` 标志,场景数据零重传,仅重建描述符)。
21. **`ui_record::restore_board_state()` 用 blanket `need_update()`**(OPEN): 每步棋/选择/加载/初始 resize 都置脏三个 manager;仅 model 的 is_show/matrix 变化。Fix: `need_model_update()`。
22. ~~RT 棋子共享 `custom_index = 2`~~(REFUTED 2026-08-12 — 误报): `InstanceId` 是实例数组索引而非 `instanceCustomIndex`;TLAS 实例数组与 model SSBO 同序,`models[InstanceId()]` 正确。`custom_index = 2u`(`ui_record.cpp:96`)为死数据。残余风险: Slang 升级后需复核 `InstanceIndex()` 映射(BuiltIn 应为 6 而非 5327)。
23. ~~`scene_model_manager::clear()` 未置 dirty~~(FIXED 2026-08-12: 现置 `is_dirty = true`)。
24. ~~delta 光的 MIS 权重 < 1~~(FIXED 2026-08-12: delta 分布 BRDF 采样不可达,正确权重恰为 1.0;旧值系统性变暗)。
25. ~~实例级 `eTriangleCullDisable` 使 ray 级背面剔除失效~~(FIXED 2026-08-12: 移除;透射路径本就在 ray 级关剔除)。
26. ~~合成路径每帧堆分配~~(FIXED 2026-08-12: 单元素 `add_waited_info`/`add_signal_info`)。Remaining(接受): 每帧一次 CPU `vkWaitSemaphores`——loop 顶部 `app->wait_frame()` 等当前帧槽上次 signal(2026-09 起 `begin_record` 不再各自 wait,见 Timeline 节)。
27. ~~UI 数值边界 bug~~(FIXED 2026-08-12: FOV 等角度 UI 以度编辑、应用时 `glm::radians` 转换;scale 钳制实为 `[0.001, 10000]`(`ui_node.cpp:240`),2026-08-29 修正文档数值)。
28. ~~棋盘"楚河汉界"图集渗墨~~(FIXED 2026-08-15: 字体重载加 `_padding`,三图集均传 `board_font_padding = 2`;未写区域由 clearColorImage 清零。UPDATE 2026-08-18: 棋盘图集加入压缩,4 对齐居中,已核验无需重调模型)。
29. ~~KTX2 sidecar 文件名乱码~~(FIXED 2026-08-15: 源纹理重命名 ASCII + `path += ".ktx2"` path 层面拼接)。
30. **vcpkg baseline 升级后 VMA 头布局**(OPEN): `vk_mem_alloc.h` 在 `include/vma/`,hpp 同目录相对 include → C1083;规避手段为 `out/build/*/vcpkg_installed/.../include/vulkan-memory-allocator-hpp/vk_mem_alloc.h` 手工拷贝,全新构建需重新拷贝。需正式修复。
31. **字体图集单层 mip**(设计选择): `mipLevels=1` + font 采样器 `maxLod=0`,缩小时欠采样闪烁;padding 已隔离渗墨。如需平滑需 mip 链(`upload_image` 无 `eTransferSrc`)。
32. ~~`resources/cache/` 未加入 .gitignore~~(FIXED 2026-08-26,2026-08-27 更新路径: pipeline cache 与 file_watch 缓存同目录,已忽略)。
33. ~~多 `vulkan_pipeline` 实例共享缓存文件~~(FIXED 2026-08-27: 单一 `vulkan_pipeline_cache` 实例,读盘 1 次(启动)/写盘 1 次(退出))。
34. ~~**`~vulkan_application()` 对未创建 device 无守卫调 waitIdle**~~(FIXED 2026-09,见 VULKAN_ISSUES M6): `vulkan_device` 增 `explicit operator bool() const noexcept`(`vulkan_device.h:29`,判 `static_cast<vk::Device>(*device) != nullptr`——raii Device 无 operator bool);析构与 resize 经 `wait_idle()` 且开头 `if (!device) return;`(`vulkan_application.cpp:184-192`),`create()` 中途抛异常时部分构造对象析构不再对 null device 调 waitIdle。析构顺序不变(wait_idle 先于 `descriptor.clear_descriptor_info()`,`vulkan_application.cpp:37-41`)。

Note (2026-08-12): MSAA sample count(`pick_msaa_sample_count`)故意取设备最高采样数(开发机实测 8×)——设计选择,非问题。
