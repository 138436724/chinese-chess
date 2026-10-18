# T00 设计与基线方案（待实施）

> 状态：设计稿，2026-10-17。尚未修改任何 T00 代码或构建配置。
> 2026-10-18 用户明确要求清除所有测试，T00 尚不到实施阶段。本文件仅保留为远期草案，暂不执行其中任何测试/CTest/脚本建设步骤；恢复必须等待用户另行明确指令。
> 对应 [PROJECT_ADJUSTMENT_PLAN.md](PROJECT_ADJUSTMENT_PLAN.md) 的 T00：建立可重复基线与最小验证入口。

## 1. 目标与边界

T00 只做三件事，不做 T05 的构建分层，也不修 F05：

1. **可重复基线**：一条脚本记录工具链、构建、12 个 shader 入口、CTest 结果、GPU/驱动信息。
2. **最小 CPU 测试入口**：`tests/` + CTest；测试失败必须让 CTest 返回非零，不依赖 `assert`。
3. **F05 修复前复现**：CACHE-01 以“预期失败”的测试登记，等 T03 修好后转绿；不伪装成常驻绿色测试。

不纳入 T00：拆分主 CMake target、CI、正式 GPU 测试矩阵、T05 的 `scripts/check-boundaries.ps1`。

## 2. 现状约束

- 主工程是单 target + `GLOB_RECURSE`，`CMakeLists.txt` 没有 `CONFIGURE_DEPENDS`（CLAUDE.md 的说法与实际不符，基线记录要写明）。
- 新建 CMake configure 会走 vcpkg manifest 并向 GitHub 拉 registry，离线/沙箱会失败；`out/build/x64-release/vcpkg-manifest-install.log` 已记录该失败。
- `out/vcpkg_installed/x64-windows` 是完整的，`out/CMakeCache.txt` 能读到 `VCPKG_INSTALLED_DIR`。
- 既有 `out/chinese-chess.vcxproj` 只编主程序，不会包含新增的 `tests/` 源文件。
- `file_watcher` 只有私有构造 + 进程级静态单例，hash 缓存路径写死为相对路径 `resources\cache\hash_cache.cache`。
- `main.cpp` 不解析命令行，没有“跑固定帧数后退出”的入口。
- **曾有 `tests/t01/`**：独立 CMake 工程 + CTest（`t01_sync`，约 0.8 s），覆盖上传/回收/失败注入/队列别名；**该目录已于 2026-10-18 按用户指令删除**。它不是 T00 的通用框架，T00 恢复时不要重复造同一套；是否重建、何时重建由用户决定。
- T01/T02 已完成：T01 每实际 VkQueue 一条 timeline、显式 token、按插入序回收；T02 统一 frame scheduler / WSI。T00 恢复时只补基线与 CPU/CTest 入口，不重复其 GPU 同步测试。
- 离线 CMake 已可用：`-DVCPKG_MANIFEST_MODE=OFF -DVCPKG_INSTALLED_DIR=<repo>/out/vcpkg_installed` 可绕过 manifest（2026-10-18 实测）。T00 的独立测试工程仍建议用 `CMAKE_PREFIX_PATH`，避免依赖主工程缓存。

结论：测试工程必须能脱离主工程的 vcpkg manifest 独立配置，否则当前环境跑不起来。

## 3. 总体方案

| 块 | 内容 | 产物 |
|---|---|---|
| A. CPU 测试工程 | 独立 `tests/` CMake 工程 + 微型测试框架 + CTest | `tests/`、`out/build/tests` |
| B. 基线脚本 | 环境/构建/12 shader/CTest 记录 | `scripts/baseline.ps1`、`scripts/compile-shaders.ps1`、`tests/baseline/BASELINE.md` |
| C. GPU 基线入口 | `--baseline` 固定流程跑三模式截图并退出；无 GPU 标未取得 | `window` baseline 模式、`tests/baseline/GPU.md` |
| D. 诊断计数 | 提交/上传/AS/描述符/采样/回收计数的轻量结构 | `vulkan_core/vulkan_diagnostics.h` + 少量 hook |

A、B 是硬性验收；C、D 可分步，但接口现在留好。

## 4. 目录与文件布局

```text
tests/
  CMakeLists.txt                 # 独立工程，不继承主工程 vcpkg manifest
  support/
    test_framework.h             # TEST_CASE / CHECK / REQUIRE + 注册表
    test_main.cpp                # --filter / --list / 退出码
    temp_directory.h/.cpp        # 唯一临时目录 + RAII 清理
  cpu/
    file_watcher_tests.cpp       # 哈希稳定性、两次检查语义
    cache01_reload_tests.cpp     # F05 复现（预期失败）
  baseline/
    BASELINE.md                  # 工具链/构建/12 shader/CTest 记录
    GPU.md                       # 三模式截图、默认棋局/棋谱记录
    INPUT.md                     # 输入路径现状（F07 的基线）
  t01/                           # 已存在：T01 专项回归，独立 CMake，不并入本层
scripts/
  build-tests.ps1                # 配置/构建独立测试工程
  compile-shaders.ps1            # 12 个入口，T05 复用
  baseline.ps1                   # 环境/构建/shader/ctest，可 -RunGpu
chinese-chess/vulkan_core/
  vulkan_diagnostics.h           # 仅头文件计数结构（T00 首个切片）
```

不建 `tests.vcxproj`；测试统一走独立 CMake + CTest，避免维护两套工程文件。

## 5. 测试工程与 CTest

### 5.1 独立 project

`tests/CMakeLists.txt` 自己 `project(chinese_chess_tests LANGUAGES CXX)`，配置时：

```powershell
cmake -S tests -B out/build/tests -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_PREFIX_PATH="$repo/out/vcpkg_installed/x64-windows"
```

- 不加载 `vcpkg.cmake`，不触发 manifest 安装，离线可配置。
- 用 VS generator，不要求当前 shell 已有 `cl.exe`。
- `find_package(OpenSSL REQUIRED)` 从 `CMAKE_PREFIX_PATH` 找；找不到就停，不偷偷换库。
- 生成器名从 `out/CMakeCache.txt` 的 `CMAKE_GENERATOR:INTERNAL` 读；读不到才回退 VS 18。

T05 再把 `tests/` 并进主工程，改用 preset + `ctest --preset`。

### 5.2 目标与源文件

T00 只需要 `file_watcher.cpp`：

```cmake
add_executable(chess_cpu_tests
  support/test_main.cpp
  support/temp_directory.cpp
  cpu/file_watcher_tests.cpp
  cpu/cache01_reload_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../chinese-chess/tools/file_watcher.cpp)

target_include_directories(chess_cpu_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../chinese-chess)
target_compile_options(chess_cpu_tests PRIVATE
  /std:c++23preview /utf-8 /W4 /WX /external:anglebrackets /external:W0)
target_compile_definitions(chess_cpu_tests PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(chess_cpu_tests PRIVATE OpenSSL::Crypto)
```

源文件列表显式写，不用 glob；不链接 Vulkan/GLFW/ImGui。

### 5.3 CTest 注册

每个用例一个 CTest，用 `cmake -E chdir` 给独立工作目录（`file_watcher` 的静态缓存会写相对路径）：

```cmake
enable_testing()
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/work/file_watcher)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/work/cache01)

add_test(NAME cpu.file_watcher
  COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}/work/file_watcher
          $<TARGET_FILE:chess_cpu_tests> --filter file_watcher)
add_test(NAME cpu.cache01_reload
  COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_BINARY_DIR}/work/cache01
          $<TARGET_FILE:chess_cpu_tests> --filter cache01_reload)

set_tests_properties(cpu.file_watcher cpu.cache01_reload
  PROPERTIES LABELS "cpu" TIMEOUT 30)
set_tests_properties(cpu.cache01_reload
  PROPERTIES LABELS "cpu;known-failure")
```

`known-failure` 只是标签，不是 `WILL_FAIL`；CTest 仍然红。运行：

```powershell
cmake --build out/build/tests --config Debug
ctest --test-dir out/build/tests -C Debug --output-on-failure --no-tests=error
```

## 6. 微型测试框架

不引入 Catch2/GoogleTest：vcpkg manifest 加依赖会重新触发联网，CTest 已负责注册/过滤/超时。

`test_framework.h` 约定：

```cpp
TEST_CASE(name) { ... }          // 静态注册
CHECK(expr);                     // 记录失败，继续
REQUIRE(expr);                   // 记录失败并中止当前用例
CHECK_EQ(a, b);                  // 打印两边值
```

语义：

- 宏用 `std::source_location::current()` 记录文件/行号；不用 `assert`，Release 不会消失。
- 每个用例包在 `try/catch`：`std::exception` 和 `...` 都算失败，不逃出 `main`。
- `--filter <substr>` 匹配；没有匹配到任何用例返回 2，防止拼错名字后 CTest 假绿。
- `--list` 输出用例名。
- 退出码：全过 0；有失败 1；无匹配 2。
- 输出固定格式：`[PASS] name` / `[FAIL] name: file:line CHECK(...)` / `N passed, M failed`。

`temp_directory`：在 `%TEMP%\chinese_chess_tests\<pid>-<counter>` 建目录，RAII 删除；测试不共享文件，避免并行 CTest 互相踩。

## 7. 测试用例

### 7.1 file_watcher（通过，固定当前语义）

- 同一内容两次 `generate_file_hash` 相同；改内容后哈希不同。
- 第一次 `is_file_modified(file)` 为 true；未改内容第二次为 false。
- 这条测试固定当前“检查即提交”的语义，作为 F05 根因证据；T03 改 API 时同步改。

### 7.2 CACHE-01 复现（当前失败）

用真实 `FILE_WATCHER`，按生产代码两段调用顺序模拟：

```cpp
write(source, "v1");
CHECK(FILE_WATCHER.is_file_modified(source));   // 首次：true

write(source, "v2");
write(sidecar, "old-ktx2");

const bool reload_selected = FILE_WATCHER.is_file_modified(source); // true，且已提交新哈希
const bool sidecar_reused =
    std::filesystem::exists(sidecar) && !FILE_WATCHER.is_file_modified(source);

CHECK(reload_selected);
CHECK_FALSE(sidecar_reused);   // 今天失败：内层看到“未变”，复用旧 sidecar
```

它使用真实 `file_watcher`，只把 `scene_texture_manager` 的 3 行决策在测试里等价展开。当前 CTest 返回非零；T03 修好后把这段模型替换为生产侧纯函数并转绿。

测试名 `cache01_reload`，标签 `cpu;known-failure`；基线文档写明“T03 前保持红”。

### 7.3 后续再加

- `scene_handle_map` 代际/失效（SCENE-01 的 CPU 部分）；
- `submission_token{}` 的空值语义（T01 的 CPU 保护）；
- glTF 生成 fixture（T09）。

## 8. BASE-01：基线脚本

### 8.1 scripts/baseline.ps1

参数：

```powershell
param(
  [ValidateSet('Debug','Release')] [string]$Configuration = 'Release',
  [string]$EvidenceRoot = 'out/baseline',
  [switch]$SkipBuild,
  [switch]$SkipTests,
  [switch]$SkipShaders,
  [switch]$RunGpu
)
```

流程（每步记录命令、退出码、日志路径）：

1. **环境**：日期、主机、OS、CPU、GPU、驱动；`git status --short`、`git rev-parse HEAD`；`cl`/MSBuild/CMake/Ninja/slangc/`vulkaninfo --summary`（没有就 `UNAVAILABLE`）；从 `out/CMakeCache.txt` 读 `VCPKG_INSTALLED_DIR`、`CMAKE_GENERATOR`、vcpkg baseline。
2. **构建**：主程序优先 `out/chinese-chess.vcxproj` + MSBuild（`/p:TrackFileAccess=false`）；能 CMake configure 时再跑 preset 作为 fresh build。测试调用 `scripts/build-tests.ps1`，Debug + Release。
3. **Shader**：调用 `scripts/compile-shaders.ps1`，12 入口逐个编译，产物非空。
4. **CTest**：`ctest --test-dir out/build/tests -C Debug -L cpu --output-on-failure --output-junit ...`；`cache01_reload` 预期失败，不掩盖。
5. **GPU**（`-RunGpu`）：调 `chinese-chess.exe --baseline <dir>`，三模式各一次；记录路径/大小/SHA-256、尺寸、模式、样本数。无 GPU 写 `UNAVAILABLE`。
6. **汇总**：`out/baseline/<timestamp>/summary.md`（可含绝对路径）；结论写入 `tests/baseline/BASELINE.md`（仓库内，不含用户名/绝对路径）。

脚本从 `$PSScriptRoot` 推仓库根；任何一步失败都继续收集，最后统一非零退出。

### 8.2 scripts/compile-shaders.ps1

- 参数：`-SlangPath`、`-OutputDirectory`、可选 `-EntryGroup`。
- 12 入口与计划 9.4 一致（5 RT + 1 RQ + 4 raster + 2 blend）。
- 每个入口：`slangc ... -entry <name> -o <file>`；退出码非零或产物 0 字节即失败。
- 不猜 PATH 里的 slangc：由调用方传入，`baseline.ps1` 从 vcpkg 安装树定位。
- T05 复用，并在 CTest 注册 `shader` 标签。

### 8.3 tests/baseline/BASELINE.md 模板

```markdown
# T00 基线
- 日期：
- 机器 / GPU / 驱动：
- MSVC / MSBuild / CMake / Ninja / Slang：
- vcpkg installed dir：
- 主程序构建：
- 12 shader 入口：
- CTest（Debug / Release）：
  | 用例 | 结果 | 备注 |
  | cpu.file_watcher | PASS | |
  | cpu.cache01_reload | FAIL | 预期，T03 修复 |
- 已知偏差：
  - 新建 CMake configure 因 vcpkg registry 访问失败
  - CLAUDE.md 声称 GLOB 带 CONFIGURE_DEPENDS，实际没有
- 未取得项：
```

## 9. GPU 三模式与输入路径基线

### 9.1 --baseline 运行模式

```text
chinese-chess.exe --baseline <dir> [--frames 120] [--record <path>]
```

行为：

1. 正常创建 window/app/scene/ui，加载默认棋谱。
2. 依次对 `rasterization` / `ray_tracing` / `ray_query`：
   - `scene->set_render_mode(mode)`；
   - 跑 `frames` 帧（RT/RQ 让累积稳定）；
   - 置 `need_capture = true`，再跑一帧，等 `save_thread` 结束；
   - 用显式文件名保存（避免 F15 秒级同名冲突），如 `<dir>/baseline_<mode>.exr`；
   - 记录 `diagnostics.json`。
3. 写 `baseline.json`：模式、分辨率、帧数、文件名、大小、SHA-256、提交计数/上传字节/待回收数。
4. 全部文件存在且非空 → 0，否则 1。

要点：`main.cpp` 解析 argv，`glfw_window` 接收 baseline options；普通启动路径不变；`scene_manager::capture_frame` 增加可选输出路径；不依赖 ImGui 交互。

### 9.2 输入路径基线（tests/baseline/INPUT.md）

| 操作 | 当前观察 | T04 目标 |
|---|---|---|
| 聚焦 ImGui 文本框输入 WASD | 回放推进（F07） | 不推进 |
| 聚焦文本框按 C | 不截图 | 不截图 |
| 视口内按 WASD | 回放推进 | 推进 |
| 视口内按 C | 截图 | 截图 |
| 聚焦文本框按 F5/F6 | 不触发 | 不触发 |

T00 只记录现状，供 T04 对照。

## 10. 诊断计数

`chinese-chess/vulkan_core/vulkan_diagnostics.h`，只依赖 `<atomic>/<cstdint>`：

```cpp
struct vulkan_diagnostics
{
    std::atomic<uint64_t> submit_count{0};
    std::atomic<uint64_t> upload_bytes{0};
    std::atomic<uint64_t> blas_build_count{0};
    std::atomic<uint64_t> blas_update_count{0};
    std::atomic<uint64_t> tlas_build_count{0};
    std::atomic<uint64_t> tlas_update_count{0};
    std::atomic<uint64_t> descriptor_rebuild_count{0};
    std::atomic<uint64_t> sample_dispatch_count{0};
    std::atomic<uint64_t> retired_resource_count{0};
    std::atomic<uint64_t> pending_resource_count{0};

    void reset() noexcept;
};

[[nodiscard]] vulkan_diagnostics& get_diagnostics() noexcept;
```

T00 首个切片只接 3 个：

- `vulkan_queue::submit` → `++submit_count`
- `vulkan_common::upload_buffer/upload_image` → `+= data.size()`
- `vulkan_recycle_bin::retire/release` → `++retired_resource_count`；`release` 末尾把 `pending_resource_count` 设为 `resources.size()`

其余 hook 在 T07/T14 接入，但头文件先定义字段，避免以后改结构。

## 11. 执行顺序

| 阶段 | 内容 | 退出条件 |
|---|---|---|
| T00-a | tests/ 独立工程 + harness + file_watcher 测试 | `ctest -L cpu` 至少一条 PASS |
| T00-b | cache01 复现 + known-failure | CTest 因该用例非零，日志入档 |
| T00-c | compile-shaders + baseline.ps1 | BASE-01 记录完整，12 入口全过 |
| T00-d | --baseline 三模式 + GPU/INPUT 文档 | 有 GPU 则三图+manifest；无则未取得 |
| T00-e | 诊断计数首片 + diagnostics.json | 基线日志有非零计数 |
| T00-f | 更新台账与 BASELINE.md | T00 标 DONE 或 GPU 部分 PENDING |

先做 a/b（风险最高：离线测试工程能否配置），再 c，最后 d/e。

## 12. 验收映射

| 计划验收 | 对应设计 | 判定 |
|---|---|---|
| BASE-01：构建 + 12 入口，区分旧工程与 fresh configure | baseline.ps1 分别记录 MSBuild 与 CMake preset | 日志/退出码/产物 |
| CACHE-01：修复前复现 | cpu.cache01_reload 断言 desired 行为，当前红 | CTest 非零 + 失败行号 |
| 测试失败确实使 CTest 非零 | 不用 WILL_FAIL；`ctest --output-on-failure` 返回 1 | 记录退出码 |
| 不能把预期失败当常驻绿 | known-failure 标签 + 文档写明 T03 前红 | 台账/基线文档 |
| 素材不依赖未跟踪 glb | 测试内写临时 png/ktx2 文本；未来 glTF 代码生成 | 仓库无新二进制资产 |
| GPU 无硬件则待补 | UNAVAILABLE + GPU.md 未取得项 | 文档 |

## 13. 风险与备选

1. **独立测试工程找不到 OpenSSL**：先用 `CMAKE_PREFIX_PATH`；失败再 `-DOPENSSL_ROOT_DIR=<vcpkg>/x64-windows`；仍失败退到 `tests/tests.vcxproj` + `cl.exe` 脚本，CTest 推迟到 T05。失败要记录，不静默换库。
2. **file_watcher 全局单例 + 相对缓存路径**：T00 用 per-test 工作目录隔离；T03 把缓存根改成显式参数后改用局部实例。
3. **cache01 长期红**：有意为之；CI 在 T00–T03 允许红，或用 `-L "cpu & ~known-failure"` 跑其它测试。文档写清。
4. **12 入口硬编码**：脚本集中定义一次，后续只改这一处；T05 移入 CTest。
5. **GPU 基线依赖窗口/驱动**：无 GPU 只记未取得，不用“跑 5 秒没崩”冒充 GPU 验证。
6. **T01 已完成但 T00 未做**：T00 基线记录 T01 当前状态，但不重写 T01 证据。

## 14. 预计改动文件（实施时）

新增：

- `tests/CMakeLists.txt`
- `tests/support/test_framework.h`
- `tests/support/test_main.cpp`
- `tests/support/temp_directory.h/.cpp`
- `tests/cpu/file_watcher_tests.cpp`
- `tests/cpu/cache01_reload_tests.cpp`
- `tests/baseline/BASELINE.md`、`GPU.md`、`INPUT.md`
- `scripts/build-tests.ps1`、`scripts/compile-shaders.ps1`、`scripts/baseline.ps1`
- `chinese-chess/vulkan_core/vulkan_diagnostics.h`

小改（阶段 d/e）：

- `chinese-chess/window/main.cpp`、`window.h/.cpp`：`--baseline` 参数
- `chinese-chess/scene/scene_manager.h/.cpp`：可选截图路径
- `vulkan_queue.cpp`、`vulkan_common.cpp`、`vulkan_recycle_bin.cpp`：计数 hook

不改：主 `CMakeLists.txt` 的 target 结构、根 preset、T05 的边界脚本。
