# CLAUDE.md

## 项目速览

实时 3D 中国象棋可视化 —— **C++23 / Vulkan 1.4 / Slang / ImGui(docking + 多视口)/ OpenColorIO ACES 2.0**。**以硬件光追管线为主的蒙特卡洛无偏 PBR 渲染器**:RT Pipeline(`scene_raytracing_render.slang`,参考实现)与 compute + Ray Query(`scene_rayquery_render.slang`)**逐像素同分布**;光栅化分支也做前向 PBR,但没有可见性项 ⇒ **无阴影 / 无 AO**。共 45 个 .cpp TU。

- 运行/调试时**工作目录必须是 `chinese-chess/`**(一切资源路径都是相对的)。
- 中文文档在工作区根的 `README.md`。
- 本文档的约定:**不写提交哈希、不写 `文件:行号`**(都会漂移),一律用符号名定位。

## 命名与分层约定(2026-10-03 定型)

| 前缀 | 含义 | 位置 |
|---|---|---|
| `gpu_*` | 与 Slang **共用同一份声明**的 GPU 布局结构 | `shared/gpu_shared_types.h` |
| `cpu_*` | CPU 侧场景数据(含 `shared_ptr` / `vulkan_image` 等 Slang 表达不了的东西) | `shared/cpu_shared_types.h` |
| `scene_*` | 场景层的管理器 / 渲染器类,以及 `scene_camera` | `scene/` |
| `vulkan_*` | Vulkan RAII 包装(命名空间 `vulkan_shared_utils` 是唯一例外) | `vulkan_core/`、`shared/` |
| `shaders/` 入口 | **与它服务的 C++ 渲染器同名**(`scene_raytracing_render.slang` ↔ `scene_raytracing_render.cpp`;合成那支归 `vulkan_application` ⇒ `vulkan_blend_image.slang`);共享模块按概念名(`pbr`/`lighting`/`path_tracing`/`shader_utils`/`gpu_shared_types`) | `shaders/` |

旧名字只出现在「变更记录」里:`scene_data.h` = 今 `shared/cpu_shared_types.h`;`shaders/gpu_data.h` = 今 `shared/gpu_shared_types.h`;`vulkan_common` = 今 `vulkan_shared_utils`;`scene_light` / `scene_material` / `scene_model` / `model_information` / `model_vertex` = 今 `cpu_light_data` / `cpu_material_data` / `cpu_model_data` / `cpu_model_info` / `gpu_model_vertex`。

## Build

- **Windows x64 / Visual Studio 2026**(MSVC 14.51,平台工具集 v180)/ **CMake ≥ 4 + Ninja + vcpkg manifest mode**。
- 正常路径:`cmake --preset x64-release` → `cmake --build out/build/x64-release`;产物 `out\build\x64-release\chinese-chess.exe`。
- **离线 / configure 失败时**(vcpkg 在 configure 阶段要联网解析依赖):直接用既有的 `out\chinese-chess.vcxproj` 走 MSBuild 做全量真实编译(Release / Debug 都可),**必须加 `/p:TrackFileAccess=false`** —— 沙箱禁止命名管道会让 CL 的文件跟踪器报 `TRK0002`。
- **源文件由 `GLOB_RECURSE` 收集**(`chinese-chess/**/*.{cpp,h,slang}`)⇒ 增删 / 改名文件后**必须重新 configure**(改一下 `CMakeLists.txt` 即可触发),否则新文件不进工程。
- 编译选项 `target_compile_options(... /std:c++23preview /utf-8 /W4 /WX)` —— **任何第三方头的告警都要处理**,不能靠降级警告级别掩盖。
- 宏:`VK_USE_PLATFORM_WIN32_KHR`、`GLFW_INCLUDE_VULKAN`、`GLM_FORCE_RADIANS`、`GLM_ENABLE_EXPERIMENTAL`、`GLM_FORCE_DEPTH_ZERO_TO_ONE`、`NOMINMAX`、`UNICODE`、`_UNICODE`;include 路径 = `chinese-chess/`(项目根,所有 include 相对它)。
- `.gitattributes`:`*.h` / `*.cpp` / `*.slang` **一律 LF**,其余 `* text=auto`(用脚本改文件时当心写成 CRLF)。
- 检索一律**排除** `out/`、`vcpkg/`、`vcpkg_installed/`(生成物与依赖);范围限 `chinese-chess/` 与根级文档。

## 架构地图

```
window/      glfw_window(GLFW RAII / 帧循环 / 输入分发 / 截图 / RenderDoc) + main.cpp
vulkan_core/ 16 对:application(编排器;持有 pipeline cache 与 blend 描述符)、device、physical_device(队列族 cartesian_product 选择)、
             swapchain(Mailbox/FIFO)、buffer / image(VMA + 状态跟踪)、commandbuffer(timeline)、queue、semaphore(时间线 + 原子计数)、
             recycle_bin、pipeline(graphics + RT;缓存句柄由参数传入)、pipeline_cache、descriptor(累加式声明 + owner + 类型化 writer)、
             sampler、acceleration_structure(BLAS/TLAS)、shader_binding_table
scene/       9 对:manager(编排:F5/F6 热重载、render(need_capture) / capture_frame、set_render_mode)、base(manager_base / manager_render facade)、
             camera、light_manager / material_manager / model_manager、rasterization_render / raytracing_render / rayquery_render
ui/          6 对:manager(ImGui 初始化 + 渲染模式下拉)、base(pro::proxy facade)、camera、light、node(材质 / 物体)、record(棋谱回放)
tools/       9 cpp + 10 h:shader_compiler(Slang→SPIR-V + 依赖感知缓存)、model_loader(fastgltf)、image_helper(OIIO + KTX2)、
             ocio_helper、font_loader(FreeType)、record_loader(ICU4C 记谱)、file_watcher(SHA-256 缓存)、string_helper、
             renderdoc_capture(+ vendored renderdoc_app.h)
shared/      3 对:gpu_shared_types.h(C++/Slang 共用 GPU 结构)、cpu_shared_types.h(CPU 场景数据)、vulkan_shared_utils.h(运行期全局 +
             find_supported_format + align_up + glm_matrix_to_vulkan + upload/download QFOT)
shaders/     9 个 .slang(5 模块 + 4 入口),见下
```

- 分层:`window/` 只碰窗口与帧循环;`ui/` 经 `scene_manager&` 操作场景;`scene/` **直接持有**三个 manager 的具体类型,另存 `proxy_view<manager_base>`;三个渲染器是拥有型 `pro::proxy<manager_render>`,活动渲染器是 `proxy_view`。
- 传输路径:`model_loader` → `cpu_model_info`(顶点/索引 + BLAS)→ `scene_model_manager` 传 GPU 缓冲与 TLAS;`cpu_material_data` / `cpu_light_data` 经各自 manager 上传 SSBO;纹理走 bindless 槽(`scene_material_manager`)。

## Shaders(`shaders/`)

| 文件 | 管线 | 入口 | 作用 |
|---|---|---|---|
| `scene_raytracing_render.slang` | RT | `rayGenMain` / `rayClosestHitMain` / `rayMissMain` / `rayShadowMissMain` / `rayShadowAnyHitMain` | **参考实现**:无偏 MC 路径追踪(子像素抖动 + 严格样本均值累积;命中处材质解析 + 介质衰减 + 自发光 + 光源/环境 NEE + 轮盘赌 + BSDF 弹射;any-hit 按 `opacity` 概率忽略命中 = 半透明阴影) |
| `scene_rayquery_render.slang` | Compute | `rayQueryComputeMain`(`[numthreads(8,8,1)]`) | 与 RT 逐像素同分布,只换求交 API(手动最近命中、候选遍历阴影);文件内附 Slang 2026.7.1 RayQuery 兼容性记录 |
| `scene_rasterization_render.slang` | Graphics | `vertMain` / `fragMain`(几何)、`skyVertMain` / `skyFragMain`(天空盒) | 前向 PBR + 天空盒全屏三角形 |
| `vulkan_blend_image.slang` | Graphics | `vertMain` / `fragMain` | scene + UI alpha 合成;`ocio_conversion()` 桩**必须保持单行**(按整行替换注入 OCIO) |
| `path_tracing.slang` | 模块 | — | 两条光追路径共享的上下文(`hit_geometry`/`material_lookup`/`resolved_surface`/`path_vertex`、插值、RR 权重、环境 miss 贡献、累积写入);**不碰全局着色器参数**(RT 与 RayQuery 的求交 API 不能共存于同一模块) |
| `pbr.slang` | 模块 | — | `microfacet`(GGX + 高度相关 Smith + VNDF 采样/eval/pdf)、`pbr_material`、`bsdf_sample`、`shading_context`(eval/pdf/sample 严格一致) |
| `lighting.slang` | 模块 | — | delta 光源采样(方向/点/聚光)、equirect 环境 + 程序化回退 + 均匀球面采样、NEE / MIS 权重 |
| `shader_utils.slang` | 模块 | — | 常量(`MAX_BOUNCES=8`、`RR_START_DEPTH=2`、`RR_MIN_PROBABILITY=0.05`、`MAX_SAMPLES=2048`、`ALPHA_MIN=1e-3`)、`rng`(**64 位种子 + PCG32 流**)、`basis`、采样、`power_heuristic`、`accumulate_sample` |
| `gpu_shared_types.slang` | 模块 | — | **只有一行** `#include "../shared/gpu_shared_types.h"` |

依赖:`gpu_shared_types.slang` ← `pbr` ← `lighting` ← `path_tracing` ← {`scene_raytracing_render.slang`, `scene_rayquery_render.slang`};`scene_rasterization_render.slang` 只 import 前四个;`vulkan_blend_image.slang` 自足。

- **Slang 风格**:有状态/内聚概念写成 `struct` + 成员函数,改自身状态的成员函数标 `[mutating]`;枚举量用**限定名**(`light_type::directional`),从 import 的模块里用非限定名会报 `undefined identifier`。
- **Slang 侧只能有一个 include 点**:模块是独立编译单元,多处 include 同一份声明 → `ambiguous reference`(报错名形如 `ambiguous reference to 'gpu_model_data'`)。
- 本地验证(无需启动程序):`slangc <文件> -target spirv -emit-spirv-directly -profile spirv_1_6 -O3 -matrix-layout-column-major -fvk-use-c-layout -capability spvRayQueryKHR -I chinese-chess\shaders -entry <入口> -o out.spv [-reflection-json out.json]`(`slangc` 在 `out\vcpkg_installed\x64-windows\tools\shader-slang\`)。**改 GPU 结构后**要用 reflection + SPIR-V `OpMemberDecorate` 两边核对。
- `vulkan_blend_image.slang` 与 RT/RQ 的着色器缓存互不影响;`utils.slang` / `scene_skybox` / `scene_brdflut` / `scene_cubemap` 均已删除(无引用)。

## GPU 数据结构(权威定义 `shared/gpu_shared_types.h`)

C++ 直接 `#include "shared/gpu_shared_types.h"`;Slang 只能经 `gpu_shared_types.slang` 间接引入。两侧写法靠宏统一:`GPU_MAT4` / `GPU_VEC4` / `GPU_VEC3` / `GPU_VEC2`(Slang = `float4x4`/`float4`…,C++ = `glm::mat4`/`glm::vec4`…)、`GPU_ADDRRESS(T)`(Slang = `T*`,C++ = `uint64_t`;**名字里的双 R 是笔误,沿用现状**)、`GPU_DEFAULT(...)`(C++ 展开成 `= …` 默认成员初值,Slang 侧为空)。CPU 侧的对应类型(带 `shared_ptr`、`vulkan_image` 等)在 `shared/cpu_shared_types.h`:`cpu_image_data`(`vulkan_image` + `sampler_type`)、`cpu_material_data`(前景/背景色 + `alpha_map` + PBR 参数)、`cpu_model_info`(顶点/索引 + BLAS + 偏移)、`cpu_model_data`(模型矩阵 + `is_show` + `get_blas_instance()`)、`cpu_light_data`。

| 结构 | 大小 | 字段(偏移) |
|---|---|---|
| `gpu_model_vertex` | 32 | position(0) normal(12) uv(24);C++ 侧另有默认 `operator<=>` 与 static `get_binding_description()` / `get_attribute_descriptions()`(location 0/1/2 即成员序) |
| `gpu_model_data` | 88 | model_matrix(0) material_index(64,**默认 `UINT32_MAX`**) vertex_address(72) index_address(80) |
| `gpu_material_data` | 80 | background_color(0) texture_index(12,默认 `UINT32_MAX`) foreground_color(16) roughness(28) emission(32) emission_strength(44) absorption(48) metallic(60) reflectance(64) ior(68) transmission(72) opacity(76) |
| `gpu_light_data` | 56 | color(0) active_type(12) direction(16) intensity(28) position(32) range(44) inner_cone_angle(48) outer_cone_angle(52) |
| `rt_scene_data` | 184 | proj_inv(0) view_inv(64) lights(128) materials(136) models(144) light_count(152) texture_count(156) material_count(160) model_count(164) skybox_index(168) frame_index(172) exposure(176) |
| `raster_scene_data` | 320 | proj(0) proj_inv(64) view(128) view_inv(192) lights(256) materials(264) models(272) light_count(280) texture_count(284) material_count(288) model_count(292) skybox_index(296) view_position(300) exposure(312) |

- `light_type` = `enum class : uint32_t { directional = 0, point = 1, spot = 2 }`,CPU(`cpu_light_data::active_type`)与 GPU 共用(GPU 侧存 `uint32_t`)。
- 材质语义:反照率 = `lerp(background_color, foreground_color, 图案权重)`(`alpha_map` 的 R 通道当权重);`reflectance` → 电介质 F0 = 0.08·reflectance;`absorption` = 介质 σ_a;`opacity` = 阴影透射率;`transmission` = 平滑介质界面分支概率。
- **布局为何能一致**:Slang 默认 std430/natural 会 float3→16 对齐、结构体大小→16 取整,与 MSVC 的 C ABI 不一致(历史真实 bug:`model_data` 88 vs 96、`light_data` 56 vs 64 ⇒ `models[i]`(i≥1)整体错位)。现在给 Slang 加 `CompilerOptionName::ForceCLayout`(`-fvk-use-c-layout`),两侧完全同布局 ⇒ **结构体里不要再加 `_pad` / `alignas`**(`GPU_DEFAULT` 只给默认值、不影响布局)。共享头带 C++ `static_assert(sizeof/offsetof)` 与 Slang 侧 `gpu_data_layout_check`(条件数组维度为负 = 编译错误;Slang 不支持全局 `static_assert`)。
- ⚠️ 自检的边界:Slang 的 `sizeof` **不含**缓冲区的对齐步长 ⇒ 删掉 `ForceCLayout` 时 sizeof 不变、只有 ArrayStride 变,**必须用 reflection 复核**;而且着色器缓存不感知编译选项,改选项后要删 `resources\cache\shader_cache.cache`。
- 访问路径:`gpu_model_data` / `gpu_material_data` / `gpu_light_data` 数组上传到各自 manager 的共享 `ssbo`(成员名沿用 `ssbo`),渲染期**不作为 SSBO 描述符绑定** —— 每帧把设备地址 + 相机矩阵组成 UBO(C 布局镜像,槽宽 `align_up(sizeof, minUniformBufferOffsetAlignment)`,容量 `MAX_FRAMES_IN_FLIGHT`,host 映射 + 每帧 `flush()`)整块 `memcpy`,shader 经指针成员直接解引用。**地址一律取自 manager 的共享 `ssbo`**,所以顶点/索引缓冲重建后地址仍有效。渲染器不声明/不推送 push constant(`vulkan_pipeline` 保留 `_push_constant` 参数、恒传空 span)。

## 关键不变量与约定

### 同步与资源状态
- barrier **只由** `vulkan_buffer` / `vulkan_image::transition_state(stage, access[, layout][, queue][, range])` 生成:src 取追踪状态、dst 取入参,返回值必须交给 `pipelineBarrier2`(`[[nodiscard("Barrier need record by commandbuffer!")]]`),调用即完成状态更新。旧的「手工构造 barrier + `set_info` 回灌」写法已删除(要 src/dst 双写,易漏)。
- 追踪队列与传入 `_queue` 不同时自动写 src/dst queue family;需要 QFOT 但传 `Ignored` 的调用点(upload / download 的 acquire)自己补 `setSrcQueueFamilyIndex` / `setDstQueueFamilyIndex`。
- **上传**:transfer 队列 release(不改布局)→ graphics acquire + `eShaderReadOnlyOptimal`;≤ 64 KiB 且 4 字节对齐走 `vkCmdUpdateBuffer` 内联路径(owner 队列,免 transfer 队列与 staging)。**下载(截图回读)**三段式:graphics release(`eNone/eNone`,只交所有权)→ transfer copy(`eTransferSrcOptimal`)→ graphics re-acquire 并恢复 `old_stage/access/layout`;第一段收 `_waited_info` 并 `add_waited_info`(先等生产者)。
- `upload_buffer` / `upload_image` 的**消费者 stage/access 由调用点传入**——几何/实例缓冲要含 `eAccelerationStructureBuildKHR`,draw 含 `eDrawIndirect`,纹理含 `eShaderSampledRead`;漏传 = 同步不足。
- **VMA**:`create(usage, flags)` 两参数正交,host 可见性**只看 `flags`**(`AUTO*` 只保证 `HOST_VISIBLE` ⇒ `flush()` / `invalidate()` 必须保留);`AUTO*` + `eMapped` 缺 `HOST_ACCESS_*` 会 `VMA_ASSERT` ⇒ 只有 `vulkan_buffer::create` 在 flags 含 HOST_ACCESS 位时补 `eMapped`(image 原样透传,不映射);`get_buffer_address()` 按创建时的 flags 取 `hostAddress` / `deviceAddress`,调用方自己取对成员,判空用 `get_buffer() != nullptr`。
- 物理设备查询一律 `*2`:`getProperties2().properties`、`getQueueFamilyProperties2()`(取 `.queueFamilyProperties.queueFlags`)、`getFormatProperties2(f).formatProperties`;`getProperties2()` 按值返回,**别用 `const&` 绑 `.properties`**(悬空)。WSI 查询保留 `KHR` 形式(升级反而多要扩展、收窄设备选择)。
- 帧节奏:CB submit 时 signal 时间线信号量;**`begin_record` 不再各自等待**,帧槽 pacing 在循环顶部 `app->wait_frame()`;回收安全靠「帧 CB 通过 GPU wait 覆盖全部一次性 CB」的纪律。
- 帧序:`ui->update()` → `scene->update()`(dirty 门控;`any_dirty || is_render_dirty` → `active_render->update()` 重建描述符,否则 `reset_accumulation()`)→ `ui->render()` → `scene->render(need_capture)` → `app->render({ui_wait, scene_wait})`(等时间线 → clear → acquire swapchain(OutOfDate 直接返回)→ barrier(scene+UI → shader read;swapchain → color attachment)→ blend 全屏三角形 → barrier → present)。

### 生命周期与析构顺序
- 默认析构 = 成员声明逆序 ⇒ 「谁先死」全靠**声明顺序**表达:app 必须在 scene/ui 之后析构(device 活到最后);GLFW window 在 ui 之后销毁(ImGui 回调要有效);`vulkan_acceleration_structure` 的 backing buffer 声明在 AS **之前**。
- `~vulkan_application()`:先 `wait_idle()`(经 `vulkan_device::explicit operator bool` 判空,`create()` 中途失败时不会对 null device 调用)再 `descriptor.clear()`(解除对 scene/ui 图像、OCIO 资源的引用)。
- `~scene_manager()` / `~ui_manager()`:开头 `app.wait_idle()`;`std::jthread save_thread` 与后台保存作业在这里隐式 join。原 `destroy()` 已并入析构。

### 描述符 / 管线缓存 / 着色器缓存
- `vulkan_descriptor` = 累加式声明(`add_binding(type, stages, count)` 顺序追加并返回 binding 号)+ 池/集 owner + 按 binding 号类型化写入(`write_image` / `write_images` / `write_buffer` / `write_acceleration_structure`,`check_write()` 对越界/类别不符直接抛);`flush(device)` 每 set 一次 `updateDescriptorSets` 并在此构造 AS 的 `StructureChain`;重建时 `recycle_bin.retire(release_sets(), …)`(声明保留)。四个使用点:app 的 blend 管线 + 三个渲染器。
- 描述符池/集只在 `any_dirty || is_render_dirty` 时重建;相机-only 更新也走这条路(仅重建描述符,**不重传场景数据**)。
- `vulkan_pipeline_cache` **单实例**(app 持有):启动读 `resources\cache\pipeline_cache.cache`,用 `vk::PipelineCacheHeaderVersionOne` 校验 vendor/device/UUID,不匹配或损坏(捕 `vk::SystemError`)回退空缓存;只在 `~glfw_window()` 里 `wait_idle()` 后写盘一次,**F5 不写盘**。
- 着色器缓存 = **单一序列化文件** `resources\cache\shader_cache.cache`(不生成逐 shader `.spv`);键 = **路径 + 入口集合**(缺入口集合会让同一 .slang 的两个管线互相串味,报 `VUID-...-pName-00707`);命中判定:按存的依赖路径重算 SHA-256、排序拼接比对 fingerprint,一致就**完全不经过 Slang**;失配才 `loadModule`(模块名 = stem)重新收集依赖并编译。**不感知编译器选项**(profile / `ForceCLayout` 等)⇒ 改选项后要手动删缓存文件。
- F5 = `active_render->recreate()`:三个渲染器都**先构建新对象**,成功后才 retire 旧管线/SBT 并 move 替换;`scene_manager::handle(F5)` 捕获异常、打印 `recreate failed, keep pipeline: …` 保留旧管线(不再 terminate)。

### 编码约定(直接影响能否编过 / 会不会静默算错)
- **IWYU**:每个 TU **只为自己直接命名的实体** include 声明头,不以「传递可达」为由省略;提供者选**最内层**的头(只用 `gpu_*` 结构就直接 include `shared/gpu_shared_types.h`,不要绕 `cpu_shared_types.h` / `model_loader.h`)。唯一例外 `std::erase_if`(声明头随容器而定)。include 顺序按 `SortIncludes: CaseSensitive`(ASCII 序:大写在前,`_` 在 `.` 之后)。
- `[[nodiscard]]` 只写 .h 声明处(匿名命名空间的自由函数留在定义处);getter 一律 `[[nodiscard]] const noexcept`;proxy 约定成员(update / clear / resize / render / recreate / reset_accumulation / handle)**不加修饰符**(改签名会破坏 proxy invocable 检查);**可抛函数不标 noexcept**(错标 = `std::terminate`):`find_supported_format` / `upload_buffer` / `upload_image` / `download_image` / `vulkan_pipeline_cache::create` / `is_cache_valid` 都因此去掉过。
- 常量与纯函数优先 `constexpr` / `constinit`(`scene_camera` 的平凡 getter 直接放头文件);局部量 `const`;catch 收 `const&`(`[[maybe_unused]]` 保住 Release 下未使用的对象);**转发谓词不要用 `std::forward`**(filter 会反复调用,转发右值会二次移动);`cartesian_product_view` **不可 const 迭代**(`all_kinds` 保持非 const)。
- 算字节数用 `sizeof(decltype(容器)::value_type)`,**不写 `sizeof(容器.front())`** —— `sizeof` 不求值(空容器不 UB),但 `front()` 的返回类型不保证是 `value_type`(`std::vector<bool>` 返回代理对象)⇒ 会**静默**算错。
- 记忆化缓存属于非逻辑状态 ⇒ `mutable` + 查询函数保持 `const`(`shader_compiler::shader_cache`、`file_watcher::file_watch_cache`);真正有状态的操作(`create` / `update` / `render` / `clear` / `next` / `transition_state` / `retire` / `release`)保持非 const。
- 错误处理:可恢复的 data/IO/parse 用 `std::expected<T, std::string>`(`compile_shader_to_spv`、`load_model`、`find_supported_format`、image_helper 系列、`load_font`、`load_records` / `read_record`、`upload_ktx2`);初始化/不变量/不可恢复 throw;「未找到」用 `std::optional`(`get_material_index` / `get_texture_index`);返回 `shared_ptr` 的工厂在边界把 expected 转 `throw std::runtime_error(error)`。
- 解耦:`ui_base` / `manager_base` / `manager_render` 是 `pro::facade_builder` facade;`ui_base` / `manager_render` 显式 `support_relocation` / `support_destruction` 为 `nothrow`(`trivial` 不满足:含 vector/shared_ptr/vk::raii);`scene_manager::create<T>(args...)` 用 concepts 约束四种 `cpu_*` 类型并分派到对应 manager 的 `create(type_identity<T>, …)`。

## 渲染管线

### 估计量(`scene_raytracing_render.slang` 与 `scene_rayquery_render.slang` 共用 `pbr`/`lighting`/`path_tracing` 的全部数学)
- **BSDF**:`(1-transmission)·[Lambert(diffuse_albedo) + GGX(f0)] + transmission·[介质界面 F·δ_reflect + (1-F)·δ_refract]`;`f0 = lerp(0.08·reflectance, albedo, metallic)`,`diffuse_albedo = (1-metallic)·albedo`,`alpha = roughness²`(下限 `ALPHA_MIN` 只是数值保护)。
- **无偏的落点**:`shading_context` 的 eval / pdf / sample 严格一致,连续波瓣权重统一取 `eval·cosθ/pdf`(混合 pdf 有余弦项托底 ⇒ 权重有界,**不需要 firefly 钳制**);轮盘赌从 `RR_START_DEPTH` 起、`p = clamp(max(throughput), RR_MIN_PROBABILITY, 1)` 且**始终按真实概率归一**(不做 `p<阈值直接 return` 的截断);累积是严格样本均值、`MAX_SAMPLES` 后冻结;NEE:delta 光源权重恒 1(不除 pdf),环境光 NEE 与 BSDF 采样做 `power_heuristic` MIS(delta 波瓣与相机主光线权重 1,靠 `path_vertex.has_bsdf_pdf` 区分)。
- **介质**:按段结算 Beer-Lambert `exp(-σ_a·d)`(均匀介质下解析精确);`ray_inside` 由界面折射翻转;**不做背面剔除**(介质内部必须能命中背面才出得来,几何定向交给着色阶段)。
- **已删除**(都是把期望拉离真值的偏差源):firefly 钳制、恒定环境光、吞吐量钳制、截断式轮盘赌、固定厚度玉石近似、高度雾。
- **已知建模近似**(不是估计量偏差,是 BRDF/模型定义):GGX 单次散射无多重散射能量补偿(Disney/glTF 标准选择);法线只做模型矩阵变换而非逆转置(非等比缩放下有误);嵌套透射介质不追踪各自的 σ_a;透射界面 NEE 贡献为 0(焦散只能靠 BSDF 采样,噪声高)。
- **场景默认材质**(`ui_record.cpp`):棋盘 roughness 0.4 / reflectance 0.4,黑色前景 + 字形图集压「楚河汉界」;白玉 `(0.95,0.92,0.85)`、青玉 `(0.15,0.45,0.32)`,roughness 0.25 / transmission 0.35 / ior 1.5,介质吸收 `(1.5,1.2,1.0)` / `(4.0,1.6,1.0)`,opacity 1.0。

### 光栅分支
- 与光追共用 BRDF 与光源采样;环境光用解析式廉价 IBL(漫反射取 `albedo·L(N)`,镜面沿「法线↔镜面反射按粗糙度插值」方向取一次辐亮度,透射沿折射方向取一次)。**没有可见性项 ⇒ 无阴影 / 无 AO**(要阴影得另加 shadow map pass)。
- 顶点输入 `{position(0), normal(1), uv(2)}`(即 `gpu_model_vertex` 的两个 static 描述函数);单次 `drawIndexedIndirect`(命令由 `scene_model_manager` 生成,`is_show` → instanceCount 0/1;RT mask 0xFF/0)。
- **天空盒**:`skyVertMain`/`skyFragMain` 全屏三角形(无顶点输入、深度测试/写入全关 ⇒ 先铺满整屏、几何随后覆盖),方向由 `proj_inv`/`view_inv` 反投影近远平面之差得到(与 `rayGenMain` 同一套,正交也对),**直接采样 equirect 2D HDR,不生成 CubeMap**;天空管线与几何管线 layout 完全相同 ⇒ 共用同一批 descriptor set。

### 截图与异步写盘
- 按 **C** → `render(true)` → `capture_frame(scene CB 的 submit info)`:三段式 QFOT 回读,把「回读末条 CB(owner acquire)的 submit info」当作 `render()` 的返回值交给合成 ⇒ 合成等待链包含整条回读(代价:该帧合成要等回读;单张 `render_output` + 合成前回读的固有代价)。
- 写盘在 `std::jthread save_thread`:等信号量 → `invalidate()` → CPU OCIO → OIIO 写 EXR/PNG(半精度浮点 → EXR,8 位 → PNG);异常只打印不上抛。**连续按键会先 join 上一次作业**(主线程可能短暂卡顿,见待办 #35)。

### KTX2 / 纹理
- 侧车 `.ktx2` 存在且源未变 → `read_ktx2`(UASTC **无条件**转码 BC6H/BC7);否则内存压缩 → **同步写 sidecar**(转码前写,保持可移植中间格式)→ `upload_ktx2` 转码上传。转码映射:BC6H→`KTX_TTF_BC6HU`、BC7→`KTX_TTF_BC7_RGBA`、BC4→`KTX_TTF_BC4_R`、EAC_R11→`KTX_TTF_ETC2_EAC_R11`。
- 字体图集(单字棋子 + 棋盘多字)宽高 4 对齐 + 内容居中,单通道压 BC4(移动端 EAC_R11),压缩失败回退 `convert_channels(atlas, 1)` 还原 R8;棋盘 UV 0-1 整图烘焙 ⇒ 居中扩展不改变字形相对位置,**无需重调 `chess_board.glb`**。
- F6 → `material_manager.reload_textures()`:**单线程**遍历缓存,先 SHA-256 判变(字体图集的合成路径有 `exists` 检查天然排除),变了才重压 + 重写 sidecar + 换图(槽位稳定,旧 imageview 走回收站),失败保留旧纹理并打印。
- `upload_image` 前 `clearColorImage`(压缩格式跳过)⇒ 未写区域恒为 0。字体图集 `mipLevels=1` + `maxLod=0`(缩小时欠采样闪烁,设计选择;要平滑需 mip 链,而 `upload_image` 目前没有 `eTransferSrc`)。

## 资源 / 依赖 / 必需特性

- `resources/models/`:**3 个 tracked `.glb`**(`chess_board`、`chess_board_line`、`chess_piece`);`Box`/`Cube`/`Sphere.glb` 未跟踪;`*.blend*` 已 ignore。**没有 `scene_skybox.glb`** —— 天空来自 `resources/textures/cracked ground.hdr`(`scene_manager` 建 `skybox_image`)。
- `resources/fonts/`:只跟踪 `LXGWWenKaiGB-Medium.ttf`(+`OFL.txt`),运行时也只加载它(ImGui 13px + 两个字形图集)。
- `resources/textures/`:`cracked ground.hdr`(~90 MB,默认天空)与运行时生成的 `.ktx2` 侧车(ignore);`resources/ocios/`:5 个 ACES 2.0 config(all-views studio 用于 CPU 保存与 GPU 合成);`resources/records/`:`棋谱1.txt`(GB2312);`resources/captures/`:截图与 RenderDoc 捕获。
- `resources\cache\`(全 ignore):`pipeline_cache.cache`、`shader_cache.cache` / `shader_cache_debug.cache`(Release / Debug 两个文件名)、`hash_cache.cache`。
- **vcpkg 依赖**:fastgltf、freetype(error-strings)、glfw3、glm、icu、imgui(docking-experimental + glfw/vulkan 绑定,1.92.8)、opencolorio、openimageio、openssl、proxy、shader-slang、vulkan-headers + utility-libraries + memory-allocator-hpp(3.4.0,随 baseline 解析)、ktx(overlay port,KTX 5.0.0-rc1 + UASTC HDR)、yaml-cpp(经 OCIO,显式 find_package)。
- **扩展(5 项)**:`VK_KHR_swapchain`、`VK_KHR_acceleration_structure`、`VK_KHR_ray_tracing_pipeline`、`VK_KHR_ray_query`、`VK_KHR_deferred_host_operations`(sync2 / BDA 已是 1.3 / 1.2 核心,改由 `PhysicalDeviceVulkan13/12Features` 启用)。
- **feature**:`samplerAnisotropy`、`multiDrawIndirect`、`shaderInt64`;1.3 `dynamicRendering`、`synchronization2`、`shaderIntegerDotProduct`;1.2 `bufferDeviceAddress`、`runtimeDescriptorArray`、`scalarBlockLayout`、`timelineSemaphore`;1.1 `shaderDrawParameters`;AS `accelerationStructure`;RT `rayTracingPipeline` + `rayTraversalPrimitiveCulling`;RQ `rayQuery`。谓词/查询链与启用链 1:1,M8 已删掉一批「请求但没用」的项(push descriptor、`extendedDynamicState`、`fillModeNonSolid`、`rayTracingPipelineTraceRaysIndirect`、`accelerationStructureCaptureReplay`、`descriptorBindingAccelerationStructureUpdateAfterBind` 等)以放宽设备选择。
- 着色器 profile = `spirv_1_6`;**RNG = 64 位种子(SplitMix64 扩展)+ PCG32 流**;两条光追路径用**同一表达式**播种 ⇒ 逐像素同种子(换 RNG 只改噪声、不改无偏性)。

## 待办与已知问题

**OPEN**
- #6 ImGui 多视口(1.92.8):辅助视口未 acquire 就 present → `UNASSIGNED-non-acquired-swapchain-image-used`;debug callback 未过滤。
- #19 RenderDoc 捕获不含 UI 侧上传(`ui->update()` 在 `begin_capture` 之前)。
- #21 `ui_record::restore_board_state()` 用 blanket `need_update()`(一次置脏三个 manager,实际只有 model 的 `is_show`/矩阵变)→ 应改 `need_model_update()`。
- #31 字体图集单层 mip(设计选择,见上);`upload_image` 无 `eTransferSrc`。
- #35 连续截图会短暂阻塞主线程(单个 `std::jthread` 的移动赋值先 join;要真无阻塞需作业队列 + staging buffer 排队)。
- #13 离线环境 configure 需联网 ⇒ 只能走既有 `.vcxproj` + MSBuild;x86 / Linux / macOS 预设未维护。
- #2 Remaining:`ePartiallyBound` 未启用,未填充的 bindless 槽位技术上未定义。
- #30 重装 vcpkg 依赖后要复核 VMA-HPP 头路径的手动修改(缺头会在编译期报 C1083)。

**已修复(只留结论)**:#1 push constant 超限 → 渲染器不再推送(改 UBO 承载);#3 F5 编译失败保留旧管线;#4 OCIO 绑定序 bug 随 `vulkan_descriptor` 重写结构性消失(残余:OCIO 纹理 binding 须自 1 起连续);#5 / #15 空场景改成 0 实例空 TLAS;#7 save_image 布局(转换全在 acquire 侧、release 保持 old_layout);#8 BLAS/TLAS 实际在 graphics 队列(`compute_queue` 零提交,为未来保留);#9 / #10 各 manager 自查 `is_dirty`;#11 `/utf-8`;#12 遗留模块并入 `shader_utils.slang`;#14 TLAS refit scratch 取 `max(build, update)`;#16 model 过滤统一在 `update()` 剪枝;#17 实例缓冲每次 refit 重建(scratch 持久);#18 `is_dirty` 在 update 成功后才清;#20 `is_render_dirty`(resize / 切渲染器零重传);#22 `InstanceId` = 实例数组下标(非 `instanceCustomIndex`);#23 `clear()` 置脏;#24 delta 光 MIS 权重 = 1;#25 移除实例级 `eTriangleCullDisable`;#26 合成路径无每帧堆分配(仅每帧一次 `vkWaitSemaphores`);#27 UI 角度以度编辑、scale 钳制 `[0.001, 10000]`;#28 图集 `_padding` + 压缩后已核验;#29 sidecar 路径改 ASCII;#32 `resources/cache/` 已 ignore;#33 单一 pipeline cache 实例;#34 `~vulkan_application()` 的 `waitIdle` 判空守卫。

**历史扫描线**:2026-08-08 基线(CMake 迁移)→ 08-12(dirty-gating + TLAS refit,#14-#23)→ 08-15(#28-#31)→ 08-16 / 08-18(#29 / #18 解决)→ 08-26~08-29(#20 / #32 / #33 修复,#5 / #8 / #27 校正)→ 09(Vulkan 合规审计 M1-M8)→ 09-11(VMA 分配意图 + 查询 `*2`)→ 09-13(异步截图 + 内联上传 + `meshes_dirty`)→ 09-20(`vulkan_descriptor` 重写)→ 09-26(barrier 统一 + 截图并入帧序)→ 10-03(`shared/` 归位 + 命名规范 + IWYU 重扫)→ 10-04(字节数改 `value_type`)。

## 变更记录(压缩版;细节看对应设计节)

- **2026-10-04(Slang 入口改名对齐 C++)**:`ray_tracing.slang` → `scene_raytracing_render.slang`、`ray_query.slang` → `scene_rayquery_render.slang`、`rasterization.slang` → `scene_rasterization_render.slang`、`blend_image.slang` → `vulkan_blend_image.slang`(入口文件与服务的 C++ 渲染器同名,合成那支归 `vulkan_application`);5 个模块也一并统一命名:`common.slang` → `shader_utils.slang`(常量 / RNG / 采样 / 数学;取代原先的垃圾桶名,与 `vulkan_shared_utils` 同构)、`data_type.slang` → `gpu_shared_types.slang`(共用 GPU 声明的 Slang 引入点,与它包装的头文件同名)、`path_tracing.slang` 保留(精确的图形学术语;它一度连写成 `pathtracing` 去对齐 `raytracing`/`rayquery`,但读起来吃力,已改回下划线分开),`pbr.slang` / `lighting.slang` 保留;模块名只出现在 `import` 行,C++ 不引用模块。**入口文件不被任何人 import**,所以 import 链不受影响,改动只涉及 C++ 里的路径字符串与注释;旧路径的着色器缓存自动失效重编。12 个入口已用 `slangc` 逐个复核编译通过。
- **2026-10-04(RNG 换代)**:`shader_utils.slang` 的 `rng` 从「Wang hash + 拿迭代哈希当流」换成 **SplitMix64 播种 + PCG32 流**(64 位状态 + 奇数流号)。动机两条:①哈希是为「单次输入 → 输出打散」设计的,**连续序列**的高维均匀性没有保证(路径追踪里相邻随机数就是相邻弹射维度);②原种子是 32 位线性组合,4K × 2048 帧时「像素 × 帧」的对数(≈1.7×10¹⁰)已超过 2³² ⇒ 不同像素会共用同一条流、噪声出现空间相关,现在种子给满 64 位。附带:新增 `next_uint64()`;`reset` 收 64 位(旧 32 位调用点隐式加宽,照旧可编译);RT 侧 any-hit 的 `shadow_payload::seed` 随之变 64 位,局部流用固定奇数 `inc`(与主路径解耦)。**不改无偏性**(任何非退化流都无偏),12 个入口已复核编译通过。
- **2026-10-04**:字节数一律 `sizeof(decltype(容器)::value_type)`(14 处),不再写 `sizeof(容器.front())`。
- **2026-10-03(两批)**:①**公共文件归位** —— 新增 `shared/`,三份跨层文件迁入并改名(`shaders/gpu_data.h` → `shared/gpu_shared_types.h`、`scene/scene_data.h` → `shared/cpu_shared_types.h`、`vulkan_core/vulkan_common.h` → `shared/vulkan_shared_utils.h`,命名空间 `vulkan_common` → `vulkan_shared_utils`),`scene_*` / `model_*` 结构改名 `cpu_*` / `gpu_*`,顶点输入描述收进 `gpu_model_vertex` 的 static 成员;Slang 侧 `gpu_shared_types.slang` 改相对路径 include(旧着色器缓存自动失效)。②**include 全量 IWYU 重扫 + 修饰符复查** —— 机械试删 + 人工确认,补齐约 250 处被传递包含掩盖的标准库 include;`vulkan_image::create` 的 view info 出参改按值;`pick_msaa_sample_count` / `pick_depth_format` 提为 static 并显式收 `const vulkan_physical_device&`;两处错标 `noexcept` 去掉;验证改走既有 `.vcxproj` + MSBuild。
- **2026-10-01**:改造成以硬件光追为主的 MC 无偏 PBR 渲染器 —— 着色器迁到 `shaders/` 并重写成 9 文件(5 模块 + 4 入口);估计量去偏(删 firefly 钳制 / 恒定环境光 / 吞吐量钳制 / 截断式轮盘赌,累积改严格样本均值,加环境光 NEE + MIS);材质参数扩到 80B;修 GPU 结构步长错位;UBO 扩到 192B / 320B(exposure、光源数组、环境槽、相机世界坐标、两个逆矩阵);光栅也做完整 PBR(补 normal 顶点属性、廉价 IBL、折射环境查询;`scene_light_manager` 的消费者 stage 补 fragment);修介质界面权重(`1/p` → `1`);光栅补天空盒(全屏三角形 + 直接采样 equirect);修着色器缓存键(路径 + 入口集合);GPU 结构改 C++/Slang 共用声明 + `ForceCLayout`。
- **2026-09-26**:barrier 生成统一到 `transition_state`(删 `set_info`);截图保存并入主线帧序;去 `vcpkg.json` 的 overrides;管理器 CPU 侧镜像改名。
- **2026-09-20**:重写 `vulkan_descriptor`(池/集 owner + 类型化 writer,累加式声明,按 binding 号寻址)。
- **2026-09-13**:异步截图保存 + 小缓冲内联上传 + 几何/实例脏标志分离 + 三个渲染器自持描述符池/集。
- **2026-09-11**:VMA 分配意图显式化(`usage` / `flags` 分离);物理设备查询统一 `*2`;扩展收敛为 5 项。
- **2026-09-01**:常量与纯函数 `constexpr` / `constinit` 化;GGX 间接弹射改真 VNDF 采样(Heitz 2018);firefly 钳制仅累积帧生效。
- **2026-08-31**:光追路线 A 落地 + SPIR-V 1.6(新增 Ray Query 渲染器与三模式枚举;规避 Slang 2026.7.1 RayQuery 的若干兼容问题)。
- **2026-08-29**:确认着色器缓存是单一序列化文件、file_watcher 缓存是 `hash_cache.cache`;`is_file_modified()` 改 const + mutable 缓存。
- **2026-09**:Vulkan 合规审计(H2 / M1-M8:push constant 弃用、空场景空 TLAS、显式消费者 stage/access、swapchain imageCount 钳制、F5 保旧管线、移除未用特性扩展、`/W4` 告警清理)。
- **2026-08**:dirty-gating + TLAS refit(#14-#23);KTX2 压缩管线定型(08-15/08-18);CMake 迁移(08-11 起)。
