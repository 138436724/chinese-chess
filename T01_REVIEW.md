# T01 修复复核与后续执行清单

> 当前维护指令（2026-10-18）：用户要求清除所有测试并暂缓 T00。下文仅保留历史复核、修复和验证事实；其中测试源码、脚本、宏、构建目录和日志已移除，历史命令不能继续执行，也不能按旧清单恢复测试。生产修复保留；当前后续工作只做实现与普通构建检查。

> 最新状态（2026-10-18）：本报告 R01—R04 已修复并验证，T01 为 DONE。随后 T02 已完成，历史交换链同步问题已关闭；Debug/Release 严格窗口测试、跨尺寸截图与 T02 ASan 均通过，详见 T02_IMPLEMENTATION.md。下文原审查失败证据和当时的限制保留。

复核日期：2026-10-18。适用范围：当前工作区的 T01 提交、同步、回收及异常清理实现。此前修复与其他未提交修改均予保留。本次没有修改生产源码或仓库原测试。

原复核结论：**Request Changes，T01 应恢复为 IN_PROGRESS。** 正常提交链和底层回归已有有效证据，但 OCIO 调用端仍有 GPU 在途资源提前销毁的异常窗口；原专项测试已不能构建，截图退出验收也没有通过。主程序可以编译不等于 T01 全部验收完成。

## 1. 必须处理的发现

### R01 / P1：OCIO 上传返回后，调用端局部图像未受异常保护

位置：`chinese-chess/vulkan_core/vulkan_application.cpp` 的 `vulkan_application::create_pipeline`，分别处理普通 LUT 和 3D LUT 的两个 lambda，定位 `vulkan_image image`、`commandbuffer.add_dependency(vulkan_common::upload_image(...))`、`ocio_images.push_back(std::move(image))`。

触发条件：`upload_image` 成功提交并返回，随后 `add_dependency` 的 vector 扩容或 `ocio_images.push_back` 的 vector 扩容失败。当前 `USE_OCIO` 为 true，这两段属于实际启动路径。

问题：成功返回的上传仍可在 GPU 执行。上传函数内部的 `vulkan_submission_scope` 已正常退出；调用端局部 `image` 没有 token 析构等待，也没有声明在它之后的异常保护。异常展开会先析构 lambda 内的图像，再到外层应用的 `wait_idle_for_cleanup`，后者无法补救已经发生的提前销毁。此前纹理槽和网格临时资源的加固没有覆盖这里。

证据级别：静态调用链确认；本次没有在真实 GPU 上故意释放在途图像来制造非法访问。正常 GPU 用例通过不覆盖该分配失败路径。

修复要求：两个 lambda 都必须在局部图像析构之前排空已成功提交的工作。可沿用现有保护，例如：

```cpp
vulkan_image image;
const vulkan_submission_scope failure_scope(graphic_queue, transfer_queue);
const auto uploaded = vulkan_common::upload_image(/* 原有参数，输出到 image */);
commandbuffer.add_dependency(uploaded);
ocio_images.push_back(std::move(image));
```

保护必须位于 lambda 内、局部 `image` 声明之后；只在整个 `create_pipeline` 开头加保护仍会晚于 lambda 局部对象的析构。也可在提交之前把所有权安置到成员容器中，并证明之后所有异常路径都由应用清理等待保护。

验收：通过故障注入分别覆盖普通/3D LUT 的依赖登记失败和容器扩容失败；让上传 token 在失败点仍未完成，检查资源销毁时机。测试自身必须有无条件解除 GPU gate 的清理，避免断言失败造成死锁。

### R02 / P1：专项测试遗漏 token 改名，当前源码无法构建

位置：`tests/t01/t01_sync_tests.cpp` 的 `verify_buffer`、`test_completion`、`test_multiple_readers`。生产类型已是 `vulkan_submission_token`，测试仍引用 `submission_token`。

实测：新目录 `out/t01-review` configure 成功，Debug build 返回 1。首个有效错误为 `verify_buffer` 参数中的旧类型名，随后还有空 token、重复依赖和 span 元素类型的同类错误。对应日志：`out/t01-review/build-debug.log`。

修复：迁移全部旧类型引用，显式包含当前 token 头文件，再从新的构建目录执行原测试。无需为了测试恢复旧类型别名；本项目不要求兼容旧接口。当前新增的 token/scope `.cpp` 只有 include，方法仍在头文件内，本次没有发现其缺席测试 source list 导致链接错误，不能把这一点误报为根因。

验收：原仓库测试在 Debug、Release 下均能从新目录构建并执行，失败返回非零，缺硬件或验证层返回 77。下文的临时适配结果只能支持底层实现，不能替代修复原测试。

### R03 / P2：回收异常仍可能穿过 noexcept，跳过统一清理边界

位置：`scene_texture_manager::destroy`。它仍声明 `noexcept`，调用新的 `retire_after_submitted`；后者分配 token vector、字符串、回收节点和资源持有者，并在异常时排空后重新抛出。

触发条件：清理无引用纹理时，上述分配失败。异常离开 `destroy` 会调用 `std::terminate`，无法到达 `main` 的错误日志及正常析构边界。当前专项故障注入只覆盖 Vulkan dispatcher，不覆盖这类 CPU 分配失败。

修复：同步修改头文件与实现的异常契约，允许错误到达现有顶层边界，或在明确的内部错误边界完成记录与安全退出。还需检查回收调用链上的 `noexcept`。`ui_manager::render_mode_ui` 也在 `noexcept` 中调用可能分配 GPU 资源的模式切换；这是既有异常边界欠账，不能用本次新增顶层 catch 已解决来概括。T04 的全面异常审计仍可单独实施。

验收：在纹理回收分配失败时，进程经过预期错误处理，已发布工作完成或 device lost 后才释放资源；不得无日志直接 terminate。不要通过吞掉异常并继续渲染规避问题。

### R04 / P2：截图退出验收脚本未通过，且 hazard 判定过宽

位置：`tests/t01/run_window_smoke.ps1`。脚本依次投递 C 按下、释放、关闭，再检查文件；没有确认截图请求已被应用接受。按键还受 `ui_manager::wants_keyboard` 控制，不能仅凭消息投递成功就认定后台截图已启动。

实测：本次使用刚构建的 Debug 主程序运行原脚本，返回 1，错误为 `No new nonempty screenshot was produced; capture/exit coverage is unproven`。所以不能将本次窗口退出计作 SYNC-04 的“活动截图时退出”通过，也不能由此断言产品截图功能本身已损坏。日志位于 `out/t01-regression/window-Debug.stdout.log` 和对应 stderr。

另一个判定缺陷：脚本发现任何 `SYNC-HAZARD-*` 都仅打印提示并可返回成功。即使出现新的 buffer/image/AS hazard，也可能被泛化为 T02。它没有限定资源、调用点或完整错误上下文。

修复：建立可观察的截图请求接受/worker 启动条件，再触发关闭；验证新增文件可解码且尺寸正确。默认把同步 hazard 判为失败；确需暂时排除 T02 时，只排除经完整消息确认的既有 swapchain 问题，并单独标记应用仍有未解决 hazard。按 hazard 名称整体放行不足以区分资源问题。

验收：能重复执行截图活动期间关闭；无死锁、悬空 device 或新同步错误。保留原失败证据，新的通过记录注明日期、命令、配置、文件与日志。

## 2. 本次确认有效的部分

- 每个实际队列独立 timeline；图形/传输别名共享 submission state。提交锁内分配 value，`submit2` 成功后才发布 latest，完成信号使用 ALL_COMMANDS。
- 大缓冲重复上传包含 owner → transfer → owner 的 ownership 链；图像 acquire 复用了 release 的 old/new layout 和 family 索引。
- command buffer 重置前等待自身 token，析构也保护已提交命令；回收器检查所有 last-use token，未完成条目不阻塞后面的独立已完成条目。
- scratch/instance 的回收登记发生在 AS build 提交成功之后。异常保护覆盖了底层上传/读回，以及已检查的纹理槽和网格临时资源。
- 本次重新构建主程序 Debug/x64 成功，构建日志未检出 warning/error。

同步和异常析构语义参照 [Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)、[command buffer 规则](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html) 与 [device lost 规则](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html)。这些规则用于校核实现，不替代运行测试。

## 3. 测试证据与局限

| 检查 | 本次结果 | 证据 |
|---|---|---|
| 主程序 Debug 构建 | PASS，退出 0 | `out/t01-review/main-build-debug.log` |
| 原专项测试 configure/build | configure PASS，build FAIL，退出 1 | `out/t01-review/build-debug.log` |
| 临时适配测试 Debug | 1/1 PASS，退出 0，约 0.98 秒 | `out/t01-review-adapted/test-debug.log`；`build/Testing/Temporary/LastTest.log` |
| 原窗口截图退出脚本 | FAIL，退出 1，未生成新截图 | `out/t01-regression/window-Debug.*.log` |
| `git diff --check` | PASS | 本次执行退出 0 |
| 当前版本 Release / ASan / 真实 device lost | 本次未执行 | 不沿用旧构建或旧会话结果作为本次通过证据 |

临时适配只在 `out/t01-review-adapted` 中复制测试与 CMake 文件，将单词 `submission_token` 替换成 `vulkan_submission_token`，将 core 路径改为实际源码目录；生产代码和 `tests/t01` 均未修改。

GPU：NVIDIA GeForce RTX 4060 Ti；driver 原始值 2585198592；graphics family 0，transfer family 5，queue index 均为 0；Khronos validation + sync validation 开启。临时适配测试分别运行 (0,0) 别名和 (0,5) 独立队列配置，两者均为 validation errors=0，覆盖缓冲内联/64 KiB 边界/大缓冲复用、图像往返、多读者回收、CB 重置、AS 临时资源、第二次提交/录制失败和 device-lost query/wait 注入。

这里的 device lost 是 dispatcher 注入；没有真实破坏驱动。AS 测试和低层 marker 测试也不能替代 OCIO 调用端、scene manager 连续替换/删除以及后台截图退出的应用级覆盖。

## 4. 下一会话执行顺序

1. 阅读本报告、`PROJECT_ADJUSTMENT_PLAN.md`、`CLAUDE.md` 与 `git status`，保护现有修改，包括与 T01 无关的删除和未跟踪资源。T00 仍为 TODO。
2. 优先修 R01，在普通与 3D LUT 调用端补齐局部 GPU 所有权异常保护，并补对应分配失败测试。
3. 修 R02，迁移原专项测试的类型名。修 R03 的异常声明及必要上游边界，增加 CPU 分配失败用例。
4. 修 R04，取得可重复的活动截图退出证据。不要扩大为 T02 的交换链重构，也不要把 T02 hazard 混为 T01 已通过。
5. 使用原测试从新目录执行 Debug/Release，主程序也重新构建；更新本报告或追加记录，保留本次失败事实。只有修复及必要验收通过后才恢复 T01 为 DONE。

在 VS 2026 Developer PowerShell、仓库根目录执行原专项测试的可移植命令如下；依赖须已安装到对应 prefix：

```powershell
$prefix = Join-Path $PWD 'out/vcpkg_installed/x64-windows'
cmake -S tests/t01 -B out/t01-verify -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_PREFIX_PATH=$prefix"
cmake --build out/t01-verify --config Debug --parallel
ctest --test-dir out/t01-verify -C Debug --output-on-failure
cmake --build out/t01-verify --config Release --parallel
ctest --test-dir out/t01-verify -C Release --output-on-failure
```

每一步检查退出码，build 失败后不得继续执行目录中遗留的旧二进制。本次没有需要作者进一步澄清的问题；修复范围和验收条件已明确。

## 5. 2026-10-18 后续修复与最终验收

本节覆盖上文原复核状态，不删除历史失败事实。测试执行说明在 `tests/t01/README.md`，可脱离聊天记录使用。

### 实际修复

| 项目 | 完成内容 |
|---|---|
| R01 | 在 `vulkan_application::create_pipeline` 的普通及 3D LUT lambda 内，均将 `vulkan_submission_scope` 放在局部 `image` 声明后面。依赖登记或容器扩容失败时，先停后续提交并排空已发布 token，再析构图像；正常退出不增加等待。 |
| R02 | 原 `tests/t01` 全部迁移为 `vulkan_submission_token` 并显式包含头文件；从新目录构建原测试，不再使用适配副本。新增独立截图解码器。 |
| R03 | 删除纹理内部 `destroy` 和 UI 模式选择函数的错误 noexcept 声明。槽表在 insert 消费资源之前预留 free list 容量，保持 `erase` 无分配，避免在资源回收后因 free list 扩容而 terminate。新增分配失败及世代复用测试。 |
| R04 | 删除键盘消息模拟，新增默认 OFF 的 `CHINESE_CHESS_T01_WINDOW_TEST` CMake 选项。在专用构建中，真实截图 worker ready 后保持等待，窗口进入正常关闭，scene 析构释放 gate，jthread 完成保存并 join。脚本检查握手顺序，使用 OIIO 完整解码并核对尺寸/通道。正常产品构建不包含这些钩子。 |
| 错误分类 | 默认遇到同步 hazard 即失败；仅 `-AllowKnownWsiHazards` 可排除完整消息匹配的既有交换链问题。识别 Debug 自定义回调与 Release 验证层默认输出两种格式；未知格式、同名 buffer hazard、VUID 和未分类验证错误继续失败。脚本使用现代 `VK_LAYER_VALIDATE_SYNC=1`，并恢复自身环境变量。 |

### 新增失败覆盖

`failing_allocation.cpp` 仅链接到测试程序，替换分配器不进入产品。2D/3D 上传后的依赖登记和图像容器扩容各注入一次真实 `std::bad_alloc`，GPU gate 保持 token 未完成，直到生产清理逻辑进入等待才解除。测试观察局部图像析构时 token 已完成；若防护回归，观察器记录失败并先等待，避免测试自身释放真实在途图像。

回收 holder 和队列前沿快照的存储分配失败也已覆盖：异常能传播，原资源所有权保留，已提交使用完成。槽表增长失败保持原条目，erase 无分配，槽位复用仍改变世代。

初次 Debug 分配失败测试曾触发 MSVC STL 的 noexcept 容器默认构造，其调试代理分配会直接 terminate；该失败已定位到堆栈。最终注入按分配尺寸定位真实 holder/快照存储，避免把标准库调试代理当作应用失败点。首次窗口解码器运行曾因独立构建缺少 DLL 搜索路径而失败；脚本现为自身进程补充配置对应的运行库路径。Release 验证层输出格式也曾被严格分类器拒绝；补齐完整格式后通过，未增加按错误名称整体放行的规则。

### 最终结果

| 检查 | 结果与证据 |
|---|---|
| 普通主程序 Debug / Release | 均构建成功；测试选项 OFF，编译 `/W4 /WX`；`out/t01-fixed/main-Debug-build.log`、`main-Release-build.log` |
| 原专项回归 Debug | 1/1 PASS，退出 0，约 1.34 s；`out/t01-fixed/test-debug.log` |
| 原专项回归 Release | 1/1 PASS，退出 0，约 5.57 s；`out/t01-fixed/test-release.log` |
| AddressSanitizer Release | 1/1 PASS，退出 0，约 3.22 s；启用 `alloc_dealloc_mismatch=1:halt_on_error=1`，无 sanitizer 报告；`out/t01-fixed-asan/test-release.log` |
| 窗口专用构建 Debug / Release | 均构建成功；`out/t01-window/build-debug.log`、`build-release.log` |
| Debug 活动截图关闭 | 连续 3/3 PASS；每张 EXR 完整解码为 800×600 RGBA；日志 `out/t01-fixed/window-Debug-20261018-044046-1555/` |
| Release 活动截图关闭 | 连续 3/3 PASS；每张 EXR 完整解码为 800×600 RGBA；日志 `out/t01-fixed/window-Release-20261018-044324-0579/` |
| 严格窗口模式 | 预期失败，退出 1，明确报告 `Synchronization hazard found (strict mode)`；已成功解码截图后拒绝 T02 hazard；日志 `out/t01-fixed/window-Debug-20261018-044156-6826/` |
| 日志分类器回归 | PASS；Debug/Release 已知消息仅在显式排除时接受；buffer、未知格式、未分类验证错误和 VUID 均拒绝 |
| `git diff --check` | PASS |

GPU 与原复核相同：RTX 4060 Ti，driver 2585198592；专项回归同时覆盖 family (0,0) 别名及 (0,5) 独立队列，每队列 index 0，两种配置均 `validation errors=0`。实际应用的窗口验收使用显式 T02 排除选项，输出持续报告 `WRITE-AFTER-READ` / `PRESENT-AFTER-WRITE` 交换链问题，**不能解释成应用同步零错误**（T02 随后已关闭这两类问题）。未删除用户截图或其他已有修改。

### 保留的限制及交接

- device lost 仍是 dispatcher 注入，未实际破坏设备/驱动。MSVC 此配置无可用 UBSan，未执行；ASan 和同步验证均已执行。
- LUT 分配失败用例运行实际 Vulkan 上传和生产 scope，覆盖 OCIO 两处调用端的同一所有权模式；不调用 OCIO 的 LUT 生成功能来制造分配失败。窗口测试运行真实 OCIO 初始化与截图保存。
- 本次没有性能基准，不将测试耗时视为性能结论；正常帧没有新增 device-wide idle。
- T00 保持 TODO；T02 随后已完成并关闭上述交换链错误与窗口压力；本文的 T02 排除只代表复核当时的状态。
- T01 状态：DONE。按实施方案下一项曾为 T02（已完成），之后为 T03；T00 最后。**测试源码、脚本与 README 已按 2026-10-18 用户指令删除**，本文所有复测命令仅作历史记录，不能继续执行，也不得据此自行恢复测试。
