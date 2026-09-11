# Chinese Chess · 中国象棋

<div align="center">

**基于 C++23 与 Vulkan 1.4 的实时 3D 中国象棋（Xiangqi）渲染引擎**

支持光栅化 / RT Pipeline / Ray Query 三种渲染模式 | 内置路径追踪器（NEE + 间接弹射 + 时域累积） | ACES 2.0 色彩管理

![渲染效果图](chinese-chess/resources/captures/2026_08_31_22_32_39.png)

</div>

---

## 目录

- [项目简介](#项目简介)
- [特性](#特性)
- [系统要求](#系统要求)
- [构建](#构建)
- [使用说明](#使用说明)
- [项目架构](#项目架构)
- [渲染技术](#渲染技术)
- [着色器说明](#着色器说明)
- [依赖项](#依赖项)
- [已知限制与取舍](#已知限制与取舍)
- [许可证](#许可证)

---

## 项目简介

实时 3D 中国象棋可视化引擎：GLB 棋子/棋盘模型 + FreeType 运行时文字图集，32 枚棋子按棋谱逐步回放；场景可自由编辑相机、灯光、材质。渲染侧提供**前向光栅化（MSAA）**、**RT Pipeline 路径追踪**、**compute + Ray Query 路径追踪**三种互切模式，共用同一份场景数据（设备地址直取，切换零重传）。全部 UI 基于 ImGui（docking + 多视口）。

引擎层为自研 **Vulkan 1.4 RAII 封装**（`vulkan.hpp` + VMA），无 render pass 对象（仅动态渲染），着色器用 **Slang** 运行时编译为 SPIR-V 1.6 并做依赖感知磁盘缓存；同步模型为**时间线信号量 + 回收站**的无栅栏帧循环。本文件自包含介绍用法、目录结构与已知限制，可直接对照代码阅读。

> **代码编写说明**：引擎层代码（`vulkan_core` 封装、`scene`/`ui`/`window`/`tools` 与 CMake 构建）由作者手写；`resources/shaders/` 下的 Slang 着色器由 AI 生成，作者负责着色器结构设计、与 C++ 侧的整合联调以及性能与画质调优。GPU 数据结构布局、同步语义等约定以 C++ 侧为准，两端需保持一致、随代码共同维护。

---

## 特性

### 🎮 核心功能

| 功能 | 说明 |
|------|------|
| **3D 棋盘与棋子** | 32 枚象棋棋子（16 红 + 16 黑），GLB 模型；棋盘"楚河汉界"与棋子文字由 FreeType 运行时栅格化（LXGW WenKai GB Medium） |
| **棋谱回放** | ICU4C 自动检测 GB2312/UTF-8 编码，正则解析中文记谱法（含前/中/后与中文数字），逐步播放对局（W/A 上一步，S/D 下一步） |
| **ImGui 控制面板** | 场景设置窗口包含相机、灯光、材质/物体、棋谱 4 个面板（docking + 多视口） |
| **截图保存** | 按 **C** 键保存当前帧（仅场景，不含 UI）；16 位浮点渲染格式 → EXR（HDR），8 位格式 → PNG（LDR） |
| **热重载** | **F5** 仅重编译当前激活渲染器的着色器与 pipeline（复用同一 pipeline cache 句柄、不读写盘；先构建新管线、成功后再替换，编译失败打印错误并保留旧管线，不会退出）；**F6** 热重载磁盘纹理（SHA-256 判变 → 单线程重压缩 sidecar → 同槽位重上传） |
| **字体渲染** | FreeType 逐字栅格化生成字形图集（单字图集 + 棋盘多字图集），宽高 4 对齐、内容居中、`padding=2` 防渗墨；UASTC 压缩为单通道 BC4/ EAC_R11，失败回退无压缩 R8 |
| **RenderDoc 帧捕获** | Debug 构建集成 RenderDoc：UI 操作使场景变脏后，本帧自动开始/结束捕获，保存到 `resources/captures/` |

### 🎨 渲染特性

| 功能 | 说明 |
|------|------|
| **三种渲染模式** | ① 前向光栅化（MSAA + resolve）；② RT Pipeline 路径追踪；③ compute + ray query 路径追踪（与 RT 行为一致）。UI"渲染模式"下拉一键切换，**启动默认 RT Pipeline** |
| **路径追踪器** | 最大 8 次弹射；NEE 直接光照（delta 光源 MIS 权重恒 1）；间接弹射波瓣选择（玉石透射 / GGX 镜面 VNDF（Heitz 2018）/ Disney 漫反射） |
| **俄罗斯轮盘赌** | 基于路径吞吐量自适应终止（`p_continue < 0.05` 截断） |
| **时域累积** | 渐进式渲染，`alpha = 1/(1 + frame_index/2)`；Wang Hash 按帧变化的种子 + 子像素抖动；firefly 钳制仅累积帧（`frame_index > 0`）生效 |
| **三种光源类型** | （仅光线追踪）方向光、点光源（平方衰减 + Range 裁剪）、聚光灯（内外锥角平滑过渡） |
| **阴影光线** | Any-Hit + `ACCEPT_FIRST_HIT_AND_END_SEARCH` + `SKIP_CLOSEST_HIT` + 背面剔除；半透明表面按 `opacity` 概率穿透 |
| **高度雾与天空** | （仅光线追踪）解析指数雾（含太阳散射）；`skybox_index` 有效时采样 HDR 等距矩形天空，过暗回退程序化渐变天空 |
| **ACES 2.0 色彩管理** | OpenColorIO：CPU 截图色彩变换 + GPU 合成 Pass 运行时注入 |

**Bindless 描述符**：场景纹理经单个大型描述符数组访问（最多 1024 个 `eCombinedImageSampler`，光栅 binding 1 / RT、RayQuery binding 3，shader 双守卫 `material_index`/`texture_index`）。模型/材质/灯光数组以 GPU 存储缓冲上传（类名沿用 `ssbo`），渲染期**不绑定为 SSBO 描述符**——数组设备地址与相机矩阵每帧写入场景参数 UBO（`scene_data.slang` 的 `raster_scene_data`/`rt_scene_data`），shader 经指针成员直接解引用。描述符只在 `any_dirty || is_render_dirty`（manager 实际重传/重建 TLAS，或 resize、渲染器切换强制置位）时重建。

### ⚙️ 工程特性

| 功能 | 说明 |
|------|------|
| **现代 C++23** | `std::ranges`、`std::views::cartesian_product`、Concepts、`std::expected` 错误处理（可恢复错误返回 `expected`，初始化失败抛异常，可选工厂返回 `shared_ptr`）、`std::format`/`std::println`、`/W4` 编译告警 |
| **Vulkan RAII 封装** | `vulkan_raii` + VMA RAII；17 组封装类：application / device / physical_device / swapchain / buffer / image / commandbuffer / queue / semaphore / recycle_bin / pipeline(+cache) / descriptor / sampler / acceleration_structure / shader_binding_table / common |
| **时间线信号量 + 回收站** | 单条时间线信号量 + 原子 CPU 计数；资源与 signal 值配对、GPU 越过该值后销毁。一次性 CB 立即提交、`begin_record` 不再 CPU 等待，帧槽 pacing 由主循环顶部 `app->wait_frame()`（等当前帧槽上次 signal）承担；帧循环无 Fence/`vkDeviceWaitIdle`（仅销毁/resize 调用） |
| **智能队列选择** | `cartesian_product` 穷举（图形/计算/传输/呈现）队列族组合，专用传输队列 10× 加分，最大化独立队列族；BLAS/TLAS 构建与全部帧提交在 graphics 队列 |
| **显式同步参数** | `upload_buffer`/`upload_image` 按真实消费者传 stage/access（顶点/索引含 AS 构建读、间接参数 `eDrawIndirect`、纹理 `eShaderSampledRead`），acquire barrier 覆盖实际消费方 |
| **资源状态追踪** | 每个 buffer/image 跟踪当前 Stage/Access/Layout/Queue，barrier 按实际当前状态生成；上传/下载走三段式 QFOT |
| **管理器架构** | scene_model_manager（顶点/索引、BLAS/TLAS、间接绘制、模型数组）/ scene_material_manager（材质数组、bindless 纹理、KTX2、F6）/ scene_light_manager（灯光数组），各 manager 自查 `is_dirty`，scene_manager 折叠 `any_dirty || is_render_dirty` 决定重建描述符或仅重置累积 |
| **回收纪律** | 帧 CB 通过 GPU 侧 wait 覆盖全部 upload，回收站 `release()` 只在帧渲染末尾调用（单线程提交下的安全侧推断） |
| **KTX2 纹理压缩管线** | 磁盘纹理与字体图集统一 **UASTC 中间格式**（内存压缩 → 同步写 sidecar → 加载时按设备转码 BC6H/BC7/BC4/EAC_R11 上传）；F6 同槽位换图，旧 imageview 延迟销毁 |
| **Pipeline 缓存** | 独立 RAII 类单实例持有（启动读盘一次、退出写盘一次、F5 不读写盘），设备校验（vendorID/deviceID/UUID），不匹配/损坏回退空缓存 |
| **着色器缓存** | 依赖感知单一序列化文件 `resources/cache/shader_cache.cache`：按依赖路径逐个重算 SHA-256 与指纹比对，命中即跳过 Slang 编译（依赖列表含入口 shader 自身，无需单独入口哈希） |
| **析构顺序加固** | `~vulkan_application`/`~scene_manager`/`~ui_manager` 析构开头自行 `wait_idle()`（`wait_idle` 带 `explicit operator bool` 判空，`create()` 中途异常也不对 null device 调 waitIdle）；AS 的 backing buffer 在 acceleration_structure 之后析构 |

---

## 系统要求

| 组件 | 最低要求 |
|------|----------|
| **操作系统** | Windows 10/11 x64 |
| **GPU** | 支持 Vulkan 1.4 、硬件光线追踪（`VK_KHR_ray_tracing_pipeline`）和 光线查询（`VK_KHR_ray_query`）|
| **显存** | 4 GB+（建议） |
| **内存** | 8 GB+（建议） |
| **编译器** | Visual Studio 2022（v143 + toolchain） |
| **构建工具** | CMake（≥ 4.0）+ Ninja + vcpkg（manifest 模式） |

### 必需 GPU 特性

设备扩展（`vulkan_application.cpp` 中枚举，谓词/查询链与启用链 1:1）：

```
VK_KHR_swapchain, VK_KHR_synchronization2
VK_KHR_acceleration_structure, VK_KHR_ray_tracing_pipeline, VK_KHR_ray_query
VK_KHR_deferred_host_operations, VK_KHR_buffer_device_address
```

Features2 链特性：`samplerAnisotropy`、`multiDrawIndirect`、`shaderInt64`；`dynamicRendering`/`synchronization2`/`shaderIntegerDotProduct`（Vulkan 1.3，后者用于 SPIR-V 1.6 整数点积 RNG 种子）；`bufferDeviceAddress`/`runtimeDescriptorArray`/`scalarBlockLayout`/`timelineSemaphore`（1.2）；`shaderDrawParameters`（1.1）；`accelerationStructure`（AS）；`rayTracingPipeline` + `rayTraversalPrimitiveCulling`（RT）；`rayQuery`（RQ）。

2026-09 审计后已移除一批"请求并启用但无使用点"项（收窄设备选择）：`VK_KHR_push_descriptor` 扩展、`pushDescriptor`、`fillModeNonSolid`、`extendedDynamicState`、`accelerationStructureCaptureReplay`、`descriptorBindingAccelerationStructureUpdateAfterBind`、`rayTracingPipelineTraceRaysIndirect`。

---

## 构建

### 1. 初始化依赖

```bash
git submodule update --init --recursive
.\vcpkg\bootstrap-vcpkg.bat   # 首次使用 vcpkg 时引导
```

vcpkg 使用 manifest 模式（`vcpkg.json`），工具链由 `CMakeLists.txt` 指向本地 `vcpkg/scripts/buildsystems/vcpkg.cmake`；`vcpkg-configuration.json` 通过 `overlay-ports` 提供自定义 ktx 端口（KTX-Software 5.0.0-rc1，UASTC HDR）。VMA和VMA-HPP版本锁定为3.3.0。默认注册表为 GitHub `microsoft/vcpkg`（国内网络可自行替换为镜像）。

### 2. 配置与构建

```bash
cmake --preset x64-debug     # 或 x64-release
cmake --build out/build/x64-release
```

需要手动修改vcpkg安装目录中`vk_mem_alloc.hpp`中的`vk_mem_alloc.h`为`vma/vk_mem_alloc.h`。

Visual Studio 2026：打开项目文件夹后，在 CMakePresets 中选择 **x64-debug** / **x64-release** 生成即可。`CMakePresets.json` 只定义 configurePresets（无 buildPresets），附带的 x86 与 Linux/macOS 预设未维护，仅 **Windows x64** 经过测试。

### 3. 运行

```bash
# 资源使用相对路径，工作目录必须为 chinese-chess/
.\out\build\x64-release\chinese-chess.exe
```

CMake 已通过 `DEBUGGER_WORKING_DIRECTORY` 将调试工作目录设为 `chinese-chess/`。

### 编译选项

```
C++23（target_compile_features cxx_std_23）
/utf-8（if(WIN32)，源文件无 BOM UTF-8、含中文注释）
/W4（target_compile_options chinese-chess PRIVATE）
NOMINMAX / UNICODE / _UNICODE
VK_USE_PLATFORM_WIN32_KHR / GLFW_INCLUDE_VULKAN
GLM_FORCE_RADIANS / GLM_ENABLE_EXPERIMENTAL / GLM_FORCE_DEPTH_ZERO_TO_ONE
```

> 着色器缓存不感知 profile 等编译器选项：改动 shader_compiler 的 profile/能力后需手动删除 `resources/cache/shader_cache.cache`。

---

## 使用说明

### 基本操作

| 按键 | 功能 |
|------|------|
| **W/A** | 棋谱上一步 |
| **S/D** | 棋谱下一步 |
| **C** | 保存当前帧截图（EXR/PNG，经 CPU OCIO 变换后存至 `resources/captures/`） |
| **F5** | 热重载：仅重新编译当前激活渲染器的着色器与 pipeline（先构建后替换，失败保留旧管线） |
| **F6** | 热重载：检测已加载磁盘纹理的源文件，变更则单线程重压缩 KTX2 sidecar 并同槽位重上传 |
| **ImGui 面板** | 场景设置窗口控制相机、灯光、材质/物体、棋谱参数（相机无鼠标轨道控制，通过面板数值调整） |

### 渲染模式切换

UI"渲染模式"下拉框（`光栅化` / `光线追踪 (RT Pipeline)` / `光线追踪 (Ray Query)`），**启动默认 RT Pipeline**。切换即重绑 active 渲染器并置脏（场景数据零重传、仅重建描述符并重置时域累积）：

- **光栅化**：前向渲染 + MSAA，实时帧率
- **RT Pipeline**：路径追踪 + 时域累积，渐进式收敛
- **Ray Query**：compute + ray query 路径追踪，行为与 RT Pipeline 一致

### RenderDoc 帧捕获（Debug 构建）

1. 安装 RenderDoc（`renderdoc.dll` 在进程 DLL 搜索路径中）
2. 在 UI 中进行任意操作（改材质、移光源等）使场景变脏
3. 本帧自动开始/结束捕获（跳过首帧；`StartFrameCapture` 在 `scene->update()` 前调用，期间的 upload/TLAS 构建也在捕获内）
4. 捕获保存于 `resources/captures/`

### 棋谱回放

1. 将棋谱文件（.txt，中文记谱法）放入 `resources/records/`
2. UI"棋局管理"→"加载棋谱"（支持 GB2312/UTF-8，ICU 自动检测编码）
3. 播放控制逐步查看对局（W/A 上一步，S/D 下一步）

支持记谱格式示例（每回合红黑交替）：

```
炮二平五，马8进7
马二进三，车9平8
...
```

### 截图

按 **C** 保存当前帧，按渲染格式自动选择：
- 16 位浮点格式 → `.exr`（HDR）
- 8 位整数格式 → `.png`（LDR）

保存路径 `resources/captures/`，文件名为时间戳。

---

## 项目架构

```
chinese-chess/
├── window/                           # GLFW 窗口 + 程序入口
│   ├── window.h/cpp                  # 窗口 RAII、帧循环、输入分发、截图、RenderDoc（Debug）
│   └── main.cpp                      # 程序入口
├── vulkan_core/      (17 个类)       # Vulkan 1.4 RAII 封装层
│   ├── vulkan_application            # 编排器：init→create、场景+UI 合成、持有 pipeline cache；wait_frame/wait_idle
│   ├── vulkan_common                 # 格式选择、upload/download（显式消费者 stage/access）、运行期全局（MAX_FRAMES_IN_FLIGHT=3 等）
│   ├── vulkan_device                 # 逻辑设备 RAII（move-only；explicit operator bool 判空）
│   ├── vulkan_physical_device        # 物理设备选择：cartesian_product 队列族组合评分
│   ├── vulkan_swapchain              # 交换链（Mailbox/FIFO、imageCount 钳制、动态重建、逐图像二元信号量）
│   ├── vulkan_pipeline               # 管线创建（graphics + RT/RQ；缓存句柄由参数传入；_push_constant 参数保留恒传空 span）
│   ├── vulkan_pipeline_cache         # 磁盘 pipeline cache（独立 RAII + 设备校验）
│   ├── vulkan_descriptor             # 描述符池/集管理（池容量与 layout 配平）
│   ├── vulkan_buffer / vulkan_image  # 缓冲/图像 + VMA + 状态追踪
│   ├── vulkan_commandbuffer          # 命令缓冲 + timeline 集成（begin_record 不等待）
│   ├── vulkan_queue                  # 队列 + CommandPool 封装
│   ├── vulkan_semaphore              # 时间线信号量 + 原子计数器
│   ├── vulkan_recycle_bin            # 延迟资源回收站（Debug 回收日志编译期关闭）
│   ├── vulkan_sampler                # 采样器 RAII（diffuse/font/sky_box/screen 等）
│   ├── vulkan_acceleration_structure # BLAS/TLAS（0 实例空 TLAS 支持、scratch 对齐）
│   └── vulkan_shader_binding_table   # RT SBT（5 个 Shader Group）
├── scene/            (12 个类)       # 场景管理
│   ├── scene_manager                 # 编排器：3 渲染器 map、F5/F6 热重载、save_image、set_render_mode
│   ├── scene_base                    # pro::proxy facade 定义（manager_base / manager_render）
│   ├── scene_model                   # 模型（BLAS、变换矩阵、is_show、材质引用）
│   ├── scene_material                # PBR 材质（双色混合 + 粗糙度/金属度 + Alpha 贴图 + opacity/ior/transmission）
│   ├── scene_light                   # 光源（扁平结构体：方向光/点光/聚光）
│   ├── scene_camera                  # 透视/正交相机 + 缓存逆矩阵
│   ├── scene_model_manager           # 顶点/索引、BLAS/TLAS、间接绘制命令、模型数组；meshes 以 shared_ptr 持有
│   ├── scene_material_manager        # 材质数组 + bindless 纹理（KTX2、字体图集、F6 热重载）
│   ├── scene_light_manager           # 灯光数组
│   ├── scene_rasterization_render    # 前向 MSAA：间接绘制、动态渲染、is_show
│   ├── scene_raytracing_render       # RT Pipeline 路径追踪：TLAS 引用、每帧场景参数 UBO
│   └── scene_rayquery_render         # compute + ray query 路径追踪（手动最近命中，与 RT Pipeline 行为一致）
├── ui/               (6 个类)        # ImGui 界面
│   ├── ui_manager                    # ImGui 初始化、场景+UI 合成、渲染模式下拉切换
│   ├── ui_base                       # proxy.hpp facade 类型
│   ├── ui_camera                     # 相机编辑面板
│   ├── ui_light                      # 灯光编辑面板
│   ├── ui_node                       # 材质 + 物体管理面板（贴图/模型文件选择）
│   └── ui_record                     # 棋谱加载与逐步回放（ICU4C 正则解析，WASD 键导航）
├── tools/            (9 个模块)      # 工具与加载器
│   ├── shader_compiler               # Slang→SPIR-V（依赖感知缓存 + SHA-256；std::expected）
│   ├── model_loader                  # glTF/GLB 加载（fastgltf）
│   ├── image_helper                  # PNG/EXR/HDR 读写（OIIO）+ KTX2 压缩/读写 + 通道合成
│   ├── ocio_helper                   # OCIO：GPU shader 生成/替换编译、LUT/UBO 上传、CPU 变换
│   ├── font_loader                   # FreeType 字形栅格化
│   ├── record_loader                 # 中文记谱法解析（ICU4C 正则 + 走法规则）
│   ├── file_watcher                  # 文件变更检测（SHA-256 + 持久化缓存）
│   ├── string_helper                 # 编码检测与转换（ICU4C）
│   └── renderdoc_capture             # RenderDoc API 封装（Debug 专用）
└── resources/
    ├── shaders/      (10 个 .slang)  # 见"着色器说明"（编译缓存位于 cache/）
    ├── fonts/        (1 个字体和 1 个协议文件)  # LXGWWenKaiGB-Medium.ttf + OFL.txt 被跟踪
    ├── models/       (4 个 .glb)     # chess_board / chess_board_line / chess_piece
    ├── textures/     (.hdr + .ktx2)  # HDR 天空（cracked ground.hdr，约 90 MB；运行时生成 UASTC sidecar）
    ├── ocios/        (5 个 .ocio)    # ACES 2.0 配置（studio/D60/reference/CG + 转换图）
    ├── records/      (.txt)          # 棋谱文件（棋谱1.txt，GB2312 示例）
    ├── captures/     (.png/.exr/.rdc)# 截图输出 + RenderDoc 捕获
    └── cache/        (pipeline_cache.cache + shader_cache.cache + hash_cache.cache)  # 运行期缓存（gitignored）
```

### 数据流（每帧）

主循环顶部 `app->wait_frame()`（等当前帧槽上次 signal）→ `glfwPollEvents()` → `ui->update()` → `scene->update()`（脏检查：manager 重传数组/重建 BLAS/TLAS/间接绘制，渲染器重建描述符或仅重置累积）→ `ui->render()` / `scene->render()`（各自渲染后 submit 并 signal timeline）→ `app->render({ui_wait, scene_wait})`（回收站 release → acquire swapchain → barrier → blend 全屏三角形合成 scene+UI，OCIO 注入代码作用于 scene 颜色 → present）。需要保存时 `scene->save_image()`（`download_image` 三段式 QFOT → CPU OCIO 变换 → EXR/PNG）。

### 核心设计模式

- **两步初始化**：`vulkan_application` 先 `init` 创建 Instance，再 `create` 创建设备/交换链/管线；两步之间创建 Window Surface
- **队列选择**：`cartesian_product` 穷举队列族组合评分；专用传输队列 10× 加分
- **时间线信号量 + 回收站**：资源与 signal 值配对、GPU 越过该值后销毁；一次性 CB 立即提交、帧槽 pacing 由 `wait_frame()` 承担，帧循环无 Fence/`vkDeviceWaitIdle`
- **资源状态追踪**：每次 barrier 后更新 Stage/Access/Layout/Queue，后续 barrier 基于实际状态生成
- **Bindless + 设备地址**：纹理走大型描述符数组（≤1024）；模型/材质/灯光数组以 BDA 解引用，地址与相机矩阵每帧经场景参数 UBO 下发（替代原 push constant）
- **UI/场景分层合成**：`blend_image.slang` 全屏三角形按 UI alpha 混合两路输出
- **类型擦除**：`ui_base`/`manager_render` 基于 `pro::proxy`（值语义多态）；渲染器存于 `unordered_map<render_mode, proxy>`，`active_render` 为非拥有 `proxy_view`
- **Debug/Release 一致帧序**：均为 `ui->update()` → `scene->update()`；Debug 在两者之间开 RenderDoc 捕获（跳过首帧）

---

## 渲染技术

### 光栅化管线（`rasterization.slang`）

- 前向渲染，单次 `drawIndexedIndirect`（命令由 scene_model_manager 生成）
- MSAA + resolve（自动取设备最高采样数）
- 顶点输入 `{position, uv}`；深度测试 + 背面剔除
- Bindless 纹理 + 模型数组（BDA）；`SV_DrawIndex` 索引模型/材质；fragment 双守卫索引
- 隐藏模型经 `instanceCount=0` 跳过
- 双色混合 `lerp(background, foreground, alpha_map.r)`

### RT Pipeline（`ray_tracing.slang` + `lighting.slang`）

- **NEE**：对每个光源采样（delta 分布），阴影光线带背面剔除（半透明按 `opacity` 概率穿透），delta 光贡献权重恒 1；diffuse `(1-kS)(1-metallic)albedo/π` + GGX Cook-Torrance specular；单光 firefly 钳制
- **环境光**：`AMBIENT_LIGHT`（0.25）以 `材质色 × 0.25 × (0.5 + 0.5·NdotV)` 叠加
- **间接弹射（波瓣选择）**：透射（玉石折射 + Beer-Lambert 吸收）/ GGX 镜面（VNDF 重要性采样，Heitz 2018）/ Disney 漫反射三分支；吞吐量 `×= lobe_value/(lobe_prob·p_continue)`，`MAX_THROUGHPUT=10`
- **高度雾**：解析指数雾（命中段与 miss 均应用，含太阳散射）
- **轮盘赌/最大深度**：`p_continue = min(1, luminance)`，< 0.05 截断；`MAX_BOUNCES = 8`
- **时域累积**：`alpha = 1/(1 + frame_index/2)`，场景更新时重置；firefly 钳制仅 `frame_index > 0` 生效（避免重置首帧被陈旧均值压暗）
- **天空**：`skybox_index` 有效时采样 HDR 等距矩形，过暗回退程序化渐变天空
- **棋子材质**：白玉/青玉双色 + 深红/墨绿刻字；`opacity=0.8, ior=1.5, transmission=1.0, roughness=0.3`

### Ray Query（`ray_query.slang`）

compute + `TraceRayInline` 路径追踪，与 RT Pipeline 行为对齐（NEE/间接弹射/高度雾/时域累积、firefly 门控一致）。因 Slang 2026.7.1 的 RayQuery 兼容限制（无候选确认 API、`CommittedStatus` 恒 None、`CandidateInstanceID` 映射错误、`TraceRayInline` 参数顺序），着色器用手动最近命中遍历规避，候选索引经 `CandidateInstanceIndex()` 获取（相关说明见 shader 文件头注释）。

### 其他着色器

- `blend_image.slang`：场景 + UI Alpha 合成；`ocio_conversion()` 为占位实现，编译期由 `ocio_helper::replace_and_compile` 替换为 OCIO 生成代码

### 色彩管理管线

1. 场景渲染 → `R16G16B16A16_SFLOAT` HDR 图像（color/storage/transfer-src/sampled 复合用途）
2. 截图：CPU 端 OCIO 色彩空间变换（SceneLinear → 默认 display/view）
3. GPU 合成 Pass：`generate_shader_info` + `replace_and_compile` 注入 OCIO 代码并上传 LUT 纹理与 UBO
4. 交换链：`USE_OCIO=true` 时 format 取 `B8G8R8A8_UNORM`，使用 OCIO 进行色彩变换；否则 format 取 `B8G8R8A8_SRGB`和 colorSpace `eSrgbNonlinear`，由 vulkan 进行色彩变换。present 优先 Mailbox、回退 FIFO 。

---

## 着色器说明

| 着色器 | 管线 | 入口点 | 状态 / 功能 |
|--------|------|--------|------|
| `rasterization.slang` | Graphics | `vertMain`, `fragMain` | 使用中：前向 MSAA，Bindless 纹理，间接绘制 |
| `ray_tracing.slang` | RT | `rayGenMain`, `rayClosestHitMain`, `rayMissMain`, `rayShadowMissMain`, `rayShadowAnyHitMain` | 使用中：路径追踪（NEE、GGX VNDF / Disney 波瓣、玉石透射、高度雾、环境光、轮盘赌、时域累积、HDR 天空） |
| `ray_query.slang` | Compute | `rayQueryComputeMain` | 使用中：compute + ray query 路径（与 RT Pipeline 一致；手动最近命中规避 Slang 兼容问题；SPIR-V 1.6 整数点积种子） |
| `lighting.slang` | (module) | — | 使用中：光源采样、BRDF 调度（GGX VNDF 采样 / Disney）、阴影光线、天空采样、高度雾 |
| `common.slang` | (module) | — | 使用中：常量、Wang Hash RNG、数学/颜色工具、Hammersley、全屏三角形 |
| `scene_data.slang` | (module) | — | 使用中：CPU/GPU 共享结构（`Vertex`/`model_data`/`material_data`/`light_data`）+ 场景参数 UBO（`raster_scene_data`/`rt_scene_data`，std140，替代原 push constant） |
| `blend_image.slang` | Graphics | `vertMain`, `fragMain` | 使用中：场景+UI Alpha 合成；`ocio_conversion()` 编译期被 OCIO 代码替换 |

依赖关系：

```
common.slang ← scene_data.slang ← lighting.slang ← ray_tracing.slang
rasterization.slang 使用 common / scene_data
ray_query.slang 使用 common / scene_data / lighting
blend_image.slang 自包含（无 import；ocio_conversion 桩函数体被 ocio_helper 替换）
```

编译：现在采用 SPIR-V 1.6 ，不再使用 `VK_KHR_spirv_1_4` 的扩展。编译时依赖 Slang 分析依赖文件，计算所有文件的 sha256 作为指纹，最后写入磁盘作为缓存加速启动。

---

## 依赖项

使用 **vcpkg** 安装依赖

| 依赖 | 用途 |
|------|------|
| **glfw3** | 窗口创建与输入管理 |
| **glm** | 数学计算 |
| **vulkan-headers** + **vulkan-utility-libraries** | Vulkan API |
| **vulkan-memory-allocator-hpp** | GPU 内存分配（VMA C++ RAII，固定 3.3.0） |
| **imgui** 1.92.8（docking-experimental + glfw/vulkan binding） | 用户界面（多视口支持） |
| **fastgltf** | glTF/GLB 模型加载 |
| **freetype** | 字体字形栅格化 |
| **opencolorio** | ACES 2.0 色彩管理（CPU 变换 + GPU shader/LUT 生成） |
| **yaml-cpp** | OpenColorIO 的依赖 |
| **openimageio** | 图像读写（PNG/EXR/HDR） |
| **openssl** | 文件内容哈希（SHA-256） |
| **proxy** | 多态值类型 facade，用于取代虚函数 |
| **shader-slang** | 编译 Slang 为 SPIR-V |
| **icu** | 字符集检测、编码转换、正则表达式 |
| **ktx**（vcpkg overlay port，KTX-Software 5.0.0-rc1，UASTC HDR） | BC6H/BC7 纹理压缩和加载 |

---

## 已知限制与取舍

- **OCIO 仅验证了单通道图像采样**：blend 描述符写入顺序与管线 layout 不一致（layout 为 `[scene, ui, sampler, UBO, 纹理对…]`，写入为 `[scene, ui, sampler, 纹理对…, UBO]`），自 binding 3 起类型错位，GPU 端 OCIO 采样错数据。CPU 截图色彩变换正常；修复需让写入严格按 layout binding 组织（UBO 先于纹理）或给描述符封装加显式 binding 映射。
- **ImGui 多视口验证层误报**：1.92.8的BUG，拖出窗口外部时触发，与本项目无关。
- **RenderDoc 不捕获第一帧**：捕获第一帧会导致无法启动，暂时未找到解决方案，故捕获时跳过第一帧。
- **每帧一次 CPU 等待**：移除begin_record中的等待，改为由外部手动调用wait等待。
- **VMA-HPP 的编译问题**：vcpkg安装的VMA-HPP查询不到VMA，需要手动修改`vk_mem_alloc.hpp`中的`vk_mem_alloc.h`为`vma/vk_mem_alloc.h`。考虑到编译时报错也会提示，故不修改vcpkg或者cmake而是采取了手动修改文件的方式。
- **bindless 1024 槽**：未启用 `ePartiallyBound`、未查询设备上限（部分移动/集成设备 layout 创建可能失败）；未写满尾部槽位靠 shader 双守卫规避。
- **字体图集单层 mip**（设计选择）：`mipLevels=1` + `maxLod=0`，缩小欠采样闪烁；padding 和创建时清空缓存已隔离渗墨，如需平滑需加 mip 链。
- **棋谱恢复整体置脏**（效率取舍）：回放"恢复局面"用整体 `need_update()`，每步棋/选择/加载都置脏三个 manager（实际只有模型 is_show/矩阵变化）。
- **MSAA 采样数**（设计选择）：取设备最高采样数（开发机 8×）。

---

## 许可证

本项目基于 **MIT License** 开源。详见 [LICENSE.txt](LICENSE.txt)。

使用的第三方资源：
- 字体：**LXGW WenKai GB / 霞鹜文楷 GB**（SIL Open Font License）
- 字体：**Source Han Sans SC / 思源黑体**（SIL Open Font License）
- RenderDoc API 头文件：**RenderDoc**（MIT License, Copyright Baldur Karlsson）
