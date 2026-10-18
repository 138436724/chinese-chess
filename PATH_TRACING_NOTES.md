# 路径追踪实现与维护约束

本说明对应 `shaders/` 的表面路径积分器。RT Pipeline 与 Ray Query 使用同一套积分逻辑；不新增测试框架，也不恢复 T00。

## 代码职责

- `path_tracing.slang`：`trace_path` 负责路径推进、当前介质、NEE/MIS、俄罗斯轮盘赌和累计辐亮度。`path_backend` 只抽象求交、遮挡和纹理读取。
- `scene_raytracing_render.slang`：raygen 调用积分器；closest-hit 只返回交点；主路径与阴影组共用 `rayAnyHitMain`。所有 `TraceRay` 从 raygen 发出，CPU 创建管线时递归深度为 1。
- `scene_rayquery_render.slang`：强制 non-opaque 候选遍历，选择通过覆盖判定的最近三角形。遮挡查询使用同样的判定。
- `pbr.slang`：连续反射与平滑介质界面的 eval、PDF、采样及权重。
- `shader_utils.slang`：PCG32、采样、稳定 MIS、法线变换、光线偏移和均值更新。

## 不得破坏的约束

1. 不设固定弹射上限。RR 从第三个表面开始，存活概率位于 `[0.05, 0.95]`，存活路径乘 `1/p`。计算概率时排除折射造成的临时 eta 缩放。
2. Delta 反射权重为 1；透射权重为 `(eta_i / eta_t)^2`。分支混合概率与 Fresnel 概率已经约去，不能再次除以 transmission。
3. Beer 吸收属于刚刚传播的介质，不能取目的表面的材质。反射保留介质；透射切换介质。阴影连接也结算当前介质的吸收。
4. `opacity` 是所有光线一致使用的表面覆盖概率，`transmission` 是介质界面分支比例，两者不能互相替代。覆盖随机数按光线种子和实例、图元编号构造，不依赖 BVH 遍历顺序。
5. GGX 的 PDF 必须匹配 VNDF 实际采样分布。掠射角使用代数化简后的稳定 Smith 公式，不通过钳制余弦改变采样密度。
6. 几何法线决定正反面、介质边界和光线起点；逆转置变换后的着色法线用于连续反射。反射方向还必须位于几何表面的有效半球。
7. `NonUniformResourceIndex` 与设备的 `shaderSampledImageArrayNonUniformIndexing` 配套使用。数组索引仍须先做数量守卫。
8. 第 0 个样本直接覆盖累积图，不读取旧值。累积保存无曝光的线性 FP32 均值；输出另乘曝光。取消 2048 样本冻结，样本编号为 `uint64_t`。

## 材质模型边界

连续反射采用能量守恒的凸组合，而非直接叠加两份完整反射能量。令 `m = metallic`、`s = 0.08 * reflectance`、`w = (1-m)*s+m`：漫反射颜色为 `(1-w)*albedo`，镜面为 `w * GGX(f0/w)`；`w=0` 单独处理。基础色限制在反照率有效范围 `[0,1]`，这是材质参数约束，不是路径贡献钳制。

该模型的粗糙非金属使用常量镜面混合比例，不等同于完整 glTF 分层 BSDF。精确 Fresnel 用于独立的平滑介质界面。连续 GGX 的 alpha 下限定义了最小波瓣宽度，不能描述为完全不改变材质的数值保护。

体积吸收要求介质实例是封闭、外法线一致、完整覆盖的网格；不同介质互不相交或严格嵌套。`locate_medium` 通过最近边界朝向判断包含关系，用于相机初始介质和退出界面后的介质恢复。没有固定容量介质栈，但开放网格、相交且未定义优先级的体积、由多个独立实例拼成的未闭合边界不在此模型范围内。部分覆盖表面宜作为薄表面使用，不能据此推断可靠的封闭体积。

当前不包含体积散射、粗糙折射或光谱色散。环境仍采用均匀方向采样，自发光网格仍靠 BSDF 命中，因此高亮环境和小发光面可能收敛很慢。理想点光源经镜面链形成的焦散需要额外的光源侧路径策略；本轮没有实现双向路径追踪。有限精度、光线偏移、有限场景范围也仍有数值近似，不能把取消深度截断等同于任意场景下的完整物理精确性。

## CPU/GPU 接口

两个光追渲染器均绑定：0 TLAS、1 输出图、2 场景 UBO、3 bindless 采样贴图、4 线性累积图。两个图像均为 RGBA32F，由 `scene_manager` 持有和重建；渲染器在读写前记录同步屏障。截图回读支持 float32 EXR。

`vulkan_rt_scene_data` 大小为 192 字节：`frame_index` 偏移 176、`exposure` 偏移 184、`environment_intensity` 偏移 188。修改该结构必须同步 C++ 的 `sizeof/offsetof` 断言、Slang 大小断言，并检查实际 SPIR-V 成员偏移。不要手工添加 padding 字段。

## 不依赖测试框架的核对方法

1. 使用已安装的 `slangc` 编译四个入口模块，RT 和 Ray Query 至少各检查 O0、O3。RT 入口为 `rayGenMain`、`rayClosestHitMain`、`rayMissMain`、`rayShadowMissMain`、`rayAnyHitMain`。
2. 参数沿用项目编译器：`-target spirv-asm -profile spirv_1_6 -capability spvRayQueryKHR -capability nonuniformqualifier -matrix-layout-column-major -fvk-use-c-layout -fvk-use-entrypoint-name`。检查 `NonUniform` 装饰、binding 4、UBO 偏移，以及只有 raygen 路径发出 `OpTraceRayKHR`。
3. 构建当前 CMake 项目；不使用遗漏新源码的旧工程。运行目录必须为 `chinese-chess/`，检查 Vulkan 验证层与进程日志。
4. 手动检查：无吸收玻璃能够透光；增加厚度时吸收增强；相机进入玻璃和嵌套玻璃时 IOR 正确；opacity 为 0 的薄面不挡相机、间接光或阴影；非等比缩放后法线正确；超过 2048 样本仍更新；切换渲染器、改变相机、调整窗口后首样本无历史污染。
5. 编译和启动检查不能代替图像统计收敛验证。比较结果时需记录场景、采样数、曝光和材质模型，不能以单张低样本截图证明无偏。

## 本轮验证记录（2026-10-24）

- Slang 2026.18.3：RT / Ray Query 的 O0、O3 编译通过；光栅化与合成入口编译通过。
- SPIR-V 核对：两个后端包含非一致采样索引 capability 与 NonUniform 装饰；累积图为 binding 4；场景 UBO 成员偏移与 C++ 断言一致；RT 的 TraceRay 指令全部位于 raygen。
- 数值核对：GGX 稳定 PDF 与解析密度、RR 存活补偿、进出玻璃的 eta 平方抵消、随机数上端点、反射混合权重以及 2048 样本后的更新。
- 当前 CMake 工程 Debug 构建通过；短暂启动有响应，日志未见 Vulkan 验证错误或运行异常。
- 独立 `out/build/path-asan` 中的 MSVC Release + AddressSanitizer 构建、短暂启动与关闭通过，退出码 0，未报告 ASan 错误。未执行 UBSan。
- E41012 已修复：编译选项显式声明 `Capability(nonuniformqualifier)`，匹配 Slang 2026.18.3 的 `NonUniformResourceIndex` 标准库要求。该版本将此能力映射到 `sm_5_1`，仅声明 `spvShaderNonUniformEXT` 不足以消除 profile 升级提示；这不表示着色器使用了提示中的全部功能。四个模块的 O0 / O3 均零诊断，修改前后的 SPIR-V `OpCapability` / `OpExtension` 集合一致，非一致索引标记保留。没有关闭 capability 检查或屏蔽警告；选项参与缓存头校验，旧缓存自动失效。
- 尚未完成上述手动场景清单和 GPU 图像统计收敛验证。没有新增测试源文件、CTest 配置或测试框架；验证产物均位于忽略的 `out/` 目录。
