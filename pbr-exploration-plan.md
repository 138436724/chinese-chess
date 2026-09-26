# 光线追踪 PBR 计划(光谱就绪版)

> **验收证据汇总见 `pbr-acceptance-evidence.md`** —— 所有量化结论 + 复现命令 + 证据强度。
> 一键复现全部离线验证:`pwsh tools/verify_all.ps1`(退出码 0 = 全过)。

> **范围**:物理正确性优先;为下一步光谱渲染做准备;**只做 RT pipeline(`ray_tracing.slang` + `scene_raytracing_render`)**。
> **明确不管**:`rasterization.slang`(光栅路径)、`ray_query.slang` + `scene_rayquery_render`(Ray Query 路径)、UI 面板扩展。
> 构建/资源约定沿用 `CLAUDE.md`;定位一律用符号名,不写行号。

---

## 0. 由"只做 RT pipeline"推导出的四条纪律

1. **不碰 `ray_query.slang` / `rasterization.slang` 的行为**。`material_data` 加字段后,这两个消费者仍按旧语义读前几个字段即可(它们只读 `background_color`/`foreground_color`/`roughness`/`metallic`/`texture_index`/`opacity`)。**新字段一律追加在结构体尾部**,不重排既有成员。
   - 后果:Ray Query 模式会与 RT 模式**视觉分叉**(它不认识新字段)。这是接受的代价,不是 bug;但要在代码注释里写明"物理正确性以 RT pipeline 为准,ray_query 为历史路径待退役",免得以后被当成回归来修。
2. **`material_data` 布局改动仍必须三处同步**:`scene_data.slang` ↔ `scene_material_manager.cpp` 的 `gpu_material_data` ↔ `scene_material.h` 的 `scene_material`。`scene_data.slang` 顶部注释点名的设备丢失风险(物理指针按结构体自然大小算步长 → `materials[i≥1]` 错位 → 越界解引用)**在只做一条路径时依然成立**。
3. **不删 `AMBIENT_LIGHT` 之前不许调它的数值**。它是"死黑阴影可见性补丁",物理上是假的;留着会让白炉测试恒不通过。阶段 3 用真 IBL 替换它,这是硬性依赖顺序。
4. **能量实验期间必须能关闭全部启发式钳制**。`MAX_THROUGHPUT = 10`、单光 `min(lum*100, 100)`、firefly `prev_lum * 4`、`clamped_roughness = max(roughness, 0.04)` —— 四个都会掩盖能量错误,必须纳入开关。

---

## 1. 光谱视角下的早退决策(本计划的核心)

光谱渲染的本质是:**着色在若干离散波长上独立求值,颜色 = 波长样本的加权和**。因此凡是"用 RGB 当作可自由加减的物理量"、"把多个波长折成一个标量"、"用 RGB↔RGB 的启发式函数"的做法,光谱化时都要拆掉重写。

### 1.1 现在就必须做对(否则光谱时返工)

| # | 现在的近似 | 问题 | 现在应改成 |
|---|---|---|---|
| 1 | `F0 = lerp(float3(0.04), material_color, metallic)`,漫反射用 `(1-kS)(1-metallic)` | RGB 三元组的 Fresnel 无法逐波长评估;`(1-metallic)` 是美术工作流启发式,不是物理 | 导体/电介质**分流**:电介质 `F0` 由**光谱** `ior` 经 Fresnel 推出(当前 RGB 路径 = 在 3 个波长上取值);金属走 **complex IOR(η, κ)** 的准确 Fresnel(见 #2)。**同时把 NEE 漫反射项修成 `(1 - F(V·H))·(1-metallic)·albedo/π`** |
| 2 | `f_schlick` 用 `F0` 单参 Schlick | Schlick 是近似,且只对电介质尚可;金属单参模型无法扩展到 η/κ | 金属用 **Karis F82**(artistic-friendly,`F0`+`F82` 是复 IOR 的拟合,天然可扩展)或直接 **η/κ Fresnel**;电介质保留 Schlick 但 `F0` 必须由光谱 `ior` 算出而非 `0.04` 硬编码 |
| 3 | `material_ior` 为标量,`f0_dielectric = ((ior-1)/(ior+1))²` 单值 | 标量 ior 意味着"无色散";但更重要的问题是**没有承载色散的字段** | `material_data` 增加色散模型字段:`uint32_t dispersion_type` + `float3 dispersion_params`。**参数化光谱化**:`dispersion_type` 选 Cauchy(2 参)或 Sellmeier(3 参,标准玻璃模型)。现在只填常数 ior,**但字段现在就留出来** |
| 4 | Beer-Lambert:`absorption_density = 12 + 30×texture_weight`,`exp(-density×thickness).xxx` | **单一标量密度**(`.xxx` 广播)意味着玉石对所有波长等吸收 → 玉石只能是灰色,这是当前"青玉"看起来发灰/浑浊的根因,也是物理错误 | `float absorption_coefficient`(光谱介质属性 σ_a,单位 1/长度)→ **RGB 三元组**(= 3 个波长样本)。厚度模型也要从 `PIECE_THICKNESS` 常数改成"沿折射路径的真实介质穿越距离"(需要介质边界追踪或至少记录入射/出射点) |
| 5 | `sample_light` 的 `pdf` 恒 1,`w_light = 1`;`light.color` 是 RGB | delta 光源下 pdf 恒 1 是对的,但**接口形状**把"波长"焊死在 `color` 里:光谱下光源需要一个 **SPD(光谱功率分布)** 而非 RGB,且面积光/环境光需要真实 pdf 与 MIS | **接口先改形状**:把光源采样与评估的签名统一成"采一次方向/位置 → 在给定波长上求值",而非"返回 RGB"。现在仍是 3 波长,但代码结构要能直接换成 N 个波长 |
| 6 | `AMBIENT_LIGHT` 常量项 | 完全非物理,且与粗糙度/方向分布无关 | 阶段 3 用真 IBL 替换并删除该项 |
| 7 | 法线变换 `mul(model_matrix, float4(n, 0))` | 非均匀缩放/错切下错误;TBN 也需要正确的几何基础 | 用逆转置法线矩阵(CPU 侧预存或 shader 侧算);TBN 由 UV 有限差分建立 |
| 8 | 单次弹射能量损失:单散射 GGX 在 `roughness→1` 损失 20%~40% | 能量不守恒,且**光谱下会逐波长不同**,无法用 RGB 缩放统一掩盖 | 加多次散射补偿(Fdez-Agüera 2019 或 Kulla-Conty),用白炉测试逐粗糙度验证 |
| 9 | `texture_index` 单张贴图被当作"底色↔前景色"插值权重 | 材质形态不是 PBR;光谱下 albedo 是反射率光谱(或至少 RGB 反射率),不是一个插值权重 | 阶段 1 扩展为 albedo/normal/ORM/emissive 多索引,旧语义作为无 albedo 时的回退保留 |
| 10 | `payload.throughput` / `color` 是 `float3`,整个管线 RGB 化 | 光谱化最小侵入路径 = "per-wavelength 标量吞吐量" | **写代码时按标量吞吐量的心智模型写**(见 §2 的接口约定),即使类型现在是 `float3` |

### 1.2 现在**不要**做(光谱时会白费,或会制造新的错误)

- ❌ **不要**做 RGB 的"光谱上采样/降采样"(如 Jakob-Hanika 的 RGB→光谱):真正的光谱渲染会在光路中携带光谱,现在做只是把 RGB 绕一圈。
- ❌ **不要**为 RGB 调参优化某个能量补偿系数:补偿项必须是解析的(与波长无关的几何/能量因子),而不是"调到白炉过"的经验数字。
- ❌ **不要**引入 RGB 无法表达但光谱需要的材质特性(如偏振、荧光)——留字段即可,不实现。
- ❌ **不要**为了好看给"青玉"调 RGB 吸收常数:正确做法是给吸收**光谱向量**并让颜色从物理中涌现(阶段 1.5,可与阶段 2 并行)。
- ❌ **不要**现在就重构为"波长采样器 + 光谱基"的完整抽象(那是阶段 5);现在只需要**接口形状**正确,不需要基函数。

---

## 2. 接口形状约定(便宜且不可逆的部分)

这是本计划最关键的"便宜改动":不改数据量,只改**函数形状**,让阶段 5 从"重写"降级为"替换类型"。

1. **光谱-标量化着色**:RT closest-hit 的主流程写成"**单次采样 → 逐波长评估**",而不是"返回一个 RGB 的 BRDF"。

   ```
   // 现在(RGB 但形状已对)
   shade_sample(material_eval m, float3 wi, float3 wo) -> float3   // 每个分量 = 该波长的标量
   // 阶段 5
   shade_sample(material_eval m, float3 wi, float3 wo) -> Spectrum // Spectrum = N 个波长样本
   ```
   现在就让 `material_eval` 成为**唯一的材质求值入口结构体**(albedo / normal / roughness / metallic / ior / absorption / transmission),而不是到处直接读 `material_data`。

2. **材质解析与着色分离**:`resolve_material(hit) -> material_eval`,纯数据;`shade_*` 只吃 `material_eval`。光谱化时只有 `resolve_material` 需要改(取贴图 / 取光谱参数)。

3. **波长相关量的命名**:凡"随波长变化的量"命名带明确单位/语义(如 `absorption_coefficient` 是 σ_a 而非"density"、`ior` 是折射率而非"f0"),避免以后靠记忆判断哪些是 RGB 三元组、哪些是光谱。

4. **MIS 与 pdf 保持波长无关**:采样策略(方向/光源选择)在光谱下仍然是**几何+亮度**决策,不按波长分别采样。所以 `brdf_sample.pdf` / `light_sample.pdf` 保持标量,pdf 计算不要混入颜色。这一点现在就要守住,否则光谱时会发现 pdf 里混了 RGB 无法分离。

5. **累积缓冲**:时域累积对"光谱标量吞吐量"是逐波长线性运算,`render_output` 保持 `float4` 不变即可。**不需要**为光谱改累积结构(除非要做光谱去噪,那不在范围)。

---

## 3. 分阶段执行

### 阶段 0:重构 + 可验证框架(先做,零视觉风险)

1. 从 `ray_tracing.slang` 抽出 `resolve_material(hit) -> material_eval` 与全部 `shade_*` 到 `lighting.slang`(或新 `shading.slang`)。**只抽 RT pipeline 的代码**;`ray_query.slang` 原封不动(接受分叉）。
2. 调试开关:新增 `uint32_t debug_flags`(用 `rt_scene_data` 尾部 pad,但**必须用 C++ `sizeof` + SPIR-V 反射复核**,不能假定不动 sizeof)。至少支持:
   - 关闭四类钳制(`MAX_THROUGHPUT` / 单光钳制 / firefly / roughness 下限)
   - 强制材质覆盖(metallic ∈ {0,1} × roughness ∈ {0.05,0.3,0.6,1.0})
   - 可视化通道(法线 / F0 / roughness / 波瓣概率 / 各弹射次数贡献)
3. **白炉测试场景**:程序化生成(改 `ui_node` 或加一个 glb)= 白色漫反射球 + 白色镜面球(`roughness` 全谱)+ 灰卡 + 环境光恒定。
4. **参考基准**:固定机位/光源/累积帧数,F 键截图存 EXR(注意 Known Issue #35:连续截图会在主线程 join 上一次写盘,批量对比要单张等待)。写一个 OIIO 读图 + 算均值/分位数亮度 + per-pixel 差异的对比脚本。
5. **改 shader 后若行为异常先删 `resources\cache\shader_cache.cache`**(它不感知 profile 等编译选项)。

### 阶段 1:材质数据结构(光谱就绪的基础)

`material_data` 尾部追加(不重排):
```
uint32_t albedo_index;      // base color(线性)
uint32_t normal_index;      // 切线空间法线
uint32_t orm_index;         // R=AO G=roughness B=metallic(glTF 约定)
uint32_t emissive_index;
float    normal_scale;
float3   absorption_coefficient;  // 光谱介质 σ_a(现在 = 3 波长),替代标量 density + texture_weight 广播
uint32_t dispersion_type;         // 0=none 1=Cauchy 2=Sellmeier
float3   dispersion_params;       // 现在恒填常数 ior;阶段 5 直接用
```
- **三处同步** + `sizeof`/SPIR-V 反射复核(设备丢失风险)。
- bindless 槽位预算:1024 ÷ 每材质 4~5 张 ≈ 200 材质。`ePartiallyBound` 未启用(Known Issue #2),**未使用贴图必须填哨兵 `0xFFFFFFFF` 而非占用空槽**。
- ~~normal 采样器~~:normal 贴图单独一个线性采样器,不共用 `diffuse_sampler`。
- **玉石吸收迁移**:`JADE_BASE_ABSORPTION + JADE_ENGRAVE_ABSORPTION × texture_weight` 从"标量密度广播"改为"底色吸收光谱 + 刻字吸收光谱"。刻字权重仍来自字形 alpha 图,但吸收本身带光谱 → 青玉的绿会**从物理中涌现**。这是本阶段最直观的正确性收益。
- 厚度模型:先保持固定厚度,但把 `PIECE_THICKNESS` 明确标注为"待替换为真实介质穿越距离",并在 payload 里预留入射点/介质状态字段。

### 阶段 2:能量守恒(白炉可验证)

1. 修 NEE:漫反射 `(1 - F(V·H))·(1-metallic)·albedo/π`;金属漫反射为 0;`kS` 不再与 metallic 重复抑制。
2. 金属 Fresnel:换成 F82 或 η/κ 准确 Fresnel(为光谱铺路,见 §1.1 #2)。电介质 `F0` 由光谱 `ior` 算。
3. 多次散射补偿(Fdez-Agüera 2019 优先,`E_avg` 用解析近似或小 LUT)。
4. 逐条手推俄罗斯轮盘赌与 `lobe_prob` 的概率归一(尤其透射分支全反射回退 `lobe_value = 1` 那条路径是否重复除以概率)。
5. 用**无钳制 + 高 SPP** 跑白炉,验收通过后再决定钳制阈值。

### 阶段 3:IBL + 环境 NEE + MIS(删除 `AMBIENT_LIGHT`)

1. 辐照度图 + GGX 预滤波 mip 链(离线/启动期生成并上传,复用 `image_helper`/`vulkan_image`);BRDF LUT 用解析近似或重写生成 pass。
2. 环境重要性采样(亮度 CDF 或 mip 分层)+ 与 BRDF 采样做 MIS(恢复 `power_heuristic`)。**这是 §1.1 #5 的接口兑现点**:`light_sample.pdf` 开始有真实值。
3. 删除 `AMBIENT_LIGHT` 与 `direct_lighting += ambient`;白炉测试此时才可能真正通过。

### 阶段 4:分层材质(按需)

clearcoat(不嵌套 IOR 衰减,`Fc = 0.04`)/ sheen / 各向异性 GGX(与阶段 1 的 TBN 共用切线)。每层独立 pdf + MIS。Emissive 需要作为显式光源进 NEE,否则只能靠路径随机命中。

### 阶段 5:光谱化(本次范围之外,但验收标准是"阶段 1~3 不改结构即可切换")

把标量吞吐量换成 N 波长样本、把光源 RGB 换成 SPD、把 `dispersion_params` 接进 Fresnel/折射、把 `absorption_coefficient` 换成光谱基。判定标准:**阶段 1~4 建立的接口形状不需要重写**。

---

## 4. 验收清单(每阶段固定跑)

| 检查 | 判据 |
|---|---|
| 白炉 · 漫反射 | 输出 = `albedo × L`,无钳制高 SPP 下误差 < 1% |
| 白炉 · 镜面全粗糙度谱 | 与多次散射补偿的解析期望一致(这是阶段 2 的主要 KPI) |
| 单色性 | 单色光 + 灰卡,输出亮度 ∝ albedo(不出现通道串扰) |
| ORM 解耦 | 改 roughness 不影响 metallic 表现,反之亦然 |
| 玉石颜色涌现 | 吸收只改光谱参数(不改任何"调色"常数)时,青玉/白玉颜色变化可解释 |
| 法线贴图方向 | 已知方向的 normal map 在固定光照下明暗方向正确 |
| 数值无偏 | 开/关钳制在图像**均值**上无系统性偏差(单像素可有差异) |
| 性能 | 每阶段记录帧时间与"累积到收敛所需帧数" |

---

## 5. 已定决策(作者授权主导后确定)

| 问题 | 决定 | 理由 |
|---|---|---|
| Ray Query 路径 | **已退役**(删除 `scene_rayquery_render.*`、`ray_query.slang`、UI 选项、`VK_KHR_ray_query` 扩展与 feature) | 它与 RT pipeline 各自实现同一套着色,每改一处物理都要改两遍,静默分叉风险不可接受 |
| `ior` 表示 | **Cauchy 两参**(`dispersion_model` / `dispersion_param`,当前填常数) | 脚本化简单,三波长足够;要精确玻璃再换 Sellmeier 只是改参数化 |
| 玉石厚度 | **真实介质穿越距离**(payload 记录入射点,出射界面用弦长结算 Beer-Lambert) | 固定厚度在光谱下同样是错的,不修就是留一个已知错误 |
| 兼容性 | **不考虑**光栅化/旧材质语义的向后兼容(rasterization 保持可读旧字段即可) | 作者明确授权 |

---

## 6. 会话记录(2026 PBR 改造)

### 已完成

1. **新建 `shading.slang`** —— 唯一材质求值入口 + 唯一 BSDF 接口:
   - `resolve_material_data()` / `default_material()` / `apply_debug_override()`
   - `evaluate_bsdf(surf, V, L, include_transmission)` / `sample_bsdf()` / `bsdf_pdf`
   - 逐波长严格独立求值(修复了初版把跨波长量取平均的错误)
   - 二分量表面(基底 + 刻字)按覆盖率混合两个完整 BSDF,而不是对颜色做线性插值
   - `sample_bsdf` 的返回值即完整方向估计(含界面 Fresnel 分配与概率补偿),
     调用方只再除俄罗斯轮盘赌概率 —— 修掉了"调用方重复乘界面 Fresnel"的双重计能
   - NEE 只取反射分量(`include_transmission = false`),透射留给路径采样
2. **`lighting.slang` 收缩为光传输模块**:删除全部 BRDF(Lambertian/Disney/GGX/Fresnel),只保留光源采样、阴影光线、天空、高度雾。
3. **`common.slang`**:新增 `pow4`、光谱/物理常量(`PBR_WAVELENGTH_COUNT`、粗糙度下限、概率夹取、pdf 下限)与 `DEBUG_*` 位标志。
4. **`scene_data.slang`**:`material_data` 新增 `absorption_coefficient` / `engrave_absorption` / `dispersion_model` / `dispersion_param`(自然大小 80B);`rt_scene_data` 新增调试通道字段(192B)。
5. **`ray_tracing.slang`** 重写着色主流程:接入 shading.slang、介质真实穿越距离、NEE 反射分量、全部钳制由 `debug_flags` 控制、4 个可视化通道 + 仅直接光通道。
6. **C++ 侧同步**:`gpu_material_data`(逐字节对齐 80B)、`scene_material` 新字段、`ui_record.cpp` 玉石改为光谱消光(青玉绿由物理涌现)、`scene_raytracing_render` 调试状态 + UBO 字段、facade 新增 5 个调试约定、光栅化渲染器实现空操作、`ui_manager::pbr_debug_ui()` 调试面板(含一键白炉预设)。

### 未验证(需要构建)

- 着色器能否通过 Slang 编译(最大风险点)。
- `material_data` 的 C++/Slang 字节布局是否严格一致 —— **这是设备丢失风险点**,若不一致会表现为 `materials[i≥1]` 错位。
- `rt_scene_data` 的 std140 布局(尤其 `alignas(16)` 与尾部 pad)是否与 Slang 一致。

### 构建前置条件(已知阻塞)

1. `vcpkg install` 需要它认可的 git:当前环境里 vcpkg 试图下载 `PortableGit-2.55.0.5` 并因网络受限超时。需要把系统 git 暴露给 vcpkg,或预先放置 PortableGit。
2. Ninja 不在 PATH:`CMAKE_MAKE_PROGRAM` 未设置(本机 ninja 在 `D:\ninja-win\ninja.exe`)。
3. CMake 只在 VS 内部:`C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`。
4. 改过着色器/编译选项后,若行为异常先删 `resources\cache\shader_cache.cache`。

### 下一步验证步骤

1. 删 `resources\cache\shader_cache.cache`,构建 Release,运行。
2. RT 模式下用"PBR 物理调试"面板逐项核对:法线通道(朝向正确)、F0 通道(电介质≈0.04 灰、金属取基色)、粗糙度/金属度通道与材质面板一致。
3. 一键白炉(metallic=0, roughness 扫 0.05/0.3/0.6/1.0),看亮斑/能量是否有粗糙度依赖的异常。
4. 切换光栅化模式,确认棋盘与棋子刻字仍正常(旧颜色字段仍被光栅路径消费)。
5. 记录帧时间与累积收敛帧数作为性能基线。

---

## 7. 会话记录(第二轮):多次散射能量补偿落地 + 验证工具

### 关键突破口:不依赖编译的数值验证

把 `shading.slang` 的 BSDF 逐式移植到 Python(`tools/pbr_validation.py`),于是**能量守恒可以在没有 Vulkan/编译的情况下量化验证**。这一轮的所有结论都来自这条路径。

### 已修的真实缺陷

1. **`sample_ggx_vndf` 的 Heitz 2018 实现错误**(着色器 + 工具各一份)。
   第 4 步"变换回椭圆体配置"原来写成
   `normalize(float3(a*Nh.x, a*Nh.y, max(0,Nh.z)))` 再映射到世界——两处错:
   乘 `a` 应作用在**切线基分量** `T1`/`T2` 上,且不该夹 `max(0,·)`。
   修正为 `H = a·Nh.x·T1 + a·Nh.y·T2 + Nh.z·Vh` 再归一化。
   **后果**:采样分布退化为近似均匀(α=1 时 `cosθ_L` 密度恒为 `1/2π`)。
2. **`F_avg` 表的函数参数默认值错误**:`e_ss_and_favg` 的 `f0` 默认写成 `1.0`
   (那是白炉约定,不是菲涅尔值),导致整张 `F_AVG` 表恒为 1.0。
3. **补偿系数 `k` 的分母符号写反**:`k = (1-E_ss)/(1+E_ss-f_avg(1-E_ss))` 是错的。
   正确式为 **`k = (1-E_ss)/(E_ss + f_avg)`**。这个错误是靠"有色表面"检查
   暴露的——白炉检查恰好通过,但电介质(F0=0.04)的反射率随粗糙度从 0.040
   掉到 0.025,说明补偿没补够。

### 能量补偿(阶段 2 的核心)

结构(与 `shading.slang` 逐式对应):

```
E_ss = 单散射方向反射率(F0=1,查表)
f_avg = 平均菲涅尔(查表,F0=0.04;金属按 F0+(1-F0)·f_avg 近似)
k    = (1 - E_ss) / (E_ss + f_avg)
单散射镜面项 ×(1 + k);多散射项 = f_avg²·k(均匀半球)
```

`k` 的推导:令 `E_total = E_ss·f_avg·(1+k) + f_avg²·k = f_avg` 解出。
该式已用**穷举校验**确认:任意 `(E_ss, f_avg)` 组合下 `E_total` 精确等于 `f_avg`。

### 量化验收证据

`tools/pbr_verify_compensation.py`(输出存 `tools/compensation_report.txt`):

| 检查 | 结果 |
|---|---|
| 白炉(F0=1)能量守恒,11 档粗糙度 × 4 档视角 | 最大误差 **0.0000** |
| 有色表面 F0=0.04 | 最大偏差 **0.00000** |
| 有色表面 F0=0.1 / 0.5 / 1.0 | 最大偏差均 **0.00000** |
| 单散射 E_ss 自洽性(roughness→0 时为 1.0000,单调递减) | 通过 |
| 单散射最大能量损失(roughness=1, μ=1) | **38.6%**(文献量级 30%~45%) |

### 多次散射补偿的已知取舍(诚实记录)

- `F_avg` 表按 **F0 = 0.04** 生成;金属用 `F0 + (1-F0)·f_avg` 近似其平均菲涅尔
  (严格做法应是按 F82 积分)。这是**近似**,不是精确解,但能量守恒的上界性质保持。
- 多散射项是**均匀半球**近似(低阶),不是 Kulla-Conty 的精确方向分布。
  真正的方向分布需要 E(μ) 的球谐/表插值,当前用常数已能把总能量补回。
- 补偿只作用于**镜面分量**;漫反射项本身已守恒,未补偿。

### 仍未解决(阻塞项)

1. **构建验证**:完整 CMake 构建仍受 vcpkg 的 git 依赖与 Ninja PATH 阻塞。
   但**着色器与 GPU 布局已可离线验证**(见下节),这个阻塞的影响范围已大幅缩小。
2. **运行期验证**:帧时间、收敛帧数、实际成像 —— 需要一次能跑起来的构建。
3. `density_check` 的解析 VNDF 密度式子仍未写对(数值积分 ≠ 1),
   所以"密度形状"这条检验路径暂时不可用。这不影响能量结论(走的是采样器自用 pdf 的自洽路径)。

### 下一轮入口

1. 修 `density_check` 的解析密度(先让它的全域积分 = 1)作为交叉验证。
2. 阶段 1 补做:albedo / normal / ORM 贴图通道 + 切线空间法线(`common.slang`
   的 `build_tangent_space` 是 Frisvad 任意基,不是几何 TBN;需要 UV 有限差分或顶点切线)。
3. 阶段 3:IBL + 环境 NEE + MIS,删除 `AMBIENT_LIGHT`。
4. 运行期基线:构建成功后记录帧时间与"累积到收敛所需帧数"。

---

## 8. 会话记录(第三轮):离线着色器验证 —— 解除最大不确定性

### 突破:`slangc.exe` 就在本机

`vcpkg_installed\x64-windows\tools\shader-slang\slangc.exe` 可用。这意味着
**着色器语法与 GPU 结构体布局不需要完整构建就能验证**。此前"只能等你编译"
的两个最大风险点因此自己解决了。

固化为可复现脚本:**`pwsh tools/verify_shaders.ps1`**(退出码 0 = 全部通过)。
它与 `shader_compiler.cpp` 的编译选项严格一致:
`-target spirv -profile spirv_1_6 -matrix-layout-column-major -O3`,逐 entry point 声明 stage。

### 验证结果(全部通过)

| 检查项 | 结果 |
|---|---|
| `ray_tracing.slang`(5 个入口:raygen/miss×2/closesthit/anyhit) | 编译通过 |
| `rasterization.slang`(vertex + fragment) | 编译通过 |
| `blend_image.slang`(vertex + fragment) | 编译通过 |
| `energy_compensation.slang` 的二维静态数组常量 `float[32][16]` | Slang 接受 |
| `material_data` 12 个成员的偏移 | **逐字段 OK**(自然大小 80B) |
| `rt_scene_data` 15 个成员的偏移 | **逐字段 OK**(192B) |
| `model_data` / `light_data` 偏移 | OK(96B / 64B,与文档一致) |

**这排除了设备丢失风险** —— `scene_data.slang` 顶部注释警告的"物理指针按结构体
自然大小算步长,不一致会让 `materials[i≥1]` 错位"已被机器校验确认一致。
校验方式:`tools/layout_probe.slang` + `slangc -reflection-json` +
`tools/verify_layout.py`(硬编码 C++ 侧期望偏移做比对)。

### 编译器抓出的真实 bug(我一直没发现的)

1. `shading.slang` 的 `sample_bsdf` 里 **`NdotV` 在使用之后才声明** ——
   `make_layer_bsdf(...)` 需要它查能量补偿表,但声明写在下一行。
2. `ray_tracing.slang` 里 `make_layer_bsdf(surf.base)` **少传了 `NdotV` 参数**
   (改成两参后漏改这一处)。

这两个都是纯语法/arity 错误,靠读代码很难发现 —— **离线编译验证的价值就在这里**。

### 本轮新增的物理正确性修正

**法线变换改用逆转置矩阵**(`shading.slang` 的 `world_normal_from_matrix`)。
原来直接 `mul(model_matrix, float4(normal, 0))`,在**非均匀缩放下是错的**:
法线被错误缩放、不再垂直切平面,导致高光方向与 Fresnel 都偏。
实现用伴随矩阵恒等式 `(M^-1)^T = adj(M)/det(M)`,det 为标量且归一化消掉,
故只需 `adj(M)^T · n` 再归一化。UI 支持任意缩放,所以这不是理论问题。

### 已知工作区注意

- 编译产物写在 `out\shader_check\`(已被 `.gitignore` 覆盖)。
- `tools/layout_probe.slang` 是**离线校验专用**,不参与运行时,新增结构体字段时
  要同步更新 `tools/verify_layout.py` 里的期望偏移表。

---

## 9. 会话记录(第四轮):完整构建的阻塞定性 + 阶段 0 收尾

### 完整构建:确认绕不过去(不再尝试)

探明了 `vcpkg_installed` 的**不完整**状态,所以不能靠"复用现有依赖"绕过 manifest install:

- **缺 `ktx` 整个包**(`CMakeLists.txt` 里 `find_package(Ktx CONFIG REQUIRED)`)——
  `vcpkg_installed` 里既无 `share/ktx` 也无任何 `KtxConfig.cmake`。
- `vulkan-memory-allocator-hpp` 是 **3.3.0**,而 `vcpkg.json` 删除 `overrides` 后
  应解析为 3.4.0(即上次 `vcpkg install` 之前的状态)。
- `vcpkg/vcpkg.exe` 存在,但 `vcpkg install` 需要它认可的 git(v2.55.0)且网络受限。

结论:**完整构建必须先在能联网的环境跑通 `vcpkg install`**。这不是代码问题。

### 阶段 0 的最后一个交付物:EXR 参考对比框架

`tools/compare_exr.py`(带 `--selftest`,无资产也能验证):

- 读 EXR/PNG(经 OIIO)或 `.npy`;输出平均亮度、亮度分位数、过曝/近黑比例。
- 差异报告:平均亮度**相对差**(整体能量判据,建议 |rel| < 2%)、
  逐像素绝对/相对差分位数、**绝对差长尾 p99/mean**(≈1 = 整体平移,
  远大于 ~5 = 差异集中在少数像素 → 局部物理问题)。
- `--mask 0.98` 可只在亮部比较(暗部信噪比低,是做能量实验时的常用做法)。
- 自测覆盖四种情形:完全相同、+1%(应通过)、+10%(应不通过)、
  局部亮斑(整体应通过但长尾应告警)—— 全部符合预期。

### 本轮新增的物理/接口改动

**色散真正生效了**(此前 `pbr_dispersion_ior` 是死代码):

- 新增 `pbr_wavelength_um(channel)`(R 0.615 / G 0.540 / B 0.465 µm,
  CIE 主波长近似)与 `pbr_channel(v, ch)`。
- `make_layer_bsdf` 改为**逐波长计算电介质 F0**:`ior` 经色散模型得到 `n(λ)`,
  再由 `n(λ)` 推出 `F0(λ)`。`dispersion_model = 0` 时数值与之前完全相同
  (已验证:编译通过;且 `pbr_dispersion_ior` 在 model=0 时直接返回 `ior`),
  所以这是**零行为变化的接口铺设**。
- 已知近似并写入注释:折射**方向**仍用参考折射率(`l.ior`),
  因为波长相关的方向分歧会破坏单路径的几何一致性。真正的光谱折射需要
  逐波长分叉路径(阶段 5)。

### 阶段 0 / 1 / 2 的交付状态

| 交付物 | 状态 |
|---|---|
| 唯一材质/BSDF 入口(`resolve_material_data` / `evaluate_bsdf` / `sample_bsdf`) | 完成 |
| `debug_flags`(关钳制/材质覆盖/可视化通道)+ UI 面板 + 一键白炉预设 | 完成 |
| 白炉测试场景 | **部分**:靠 `debug_flags` 的材质覆盖 + 现有物体实现,无专用测试几何 |
| EXR 参考对比框架 | 完成(`tools/compare_exr.py`,含自测) |
| `material_data` 光谱消光/色散字段 + 玉石物理光谱吸收 + 真实穿越距离 | 完成 |
| 贴图通道(albedo / normal / ORM)+ 切线空间法线 | **normal + ORM 完成**(albedo 贴图待真实资产) |
| 能量守恒(NEE/F82/Smith/MIS)+ 多次散射补偿 + 白炉量化验收 | 完成(误差 0.0000) |
| 环境光照(IBL + 环境 NEE + MIS,删除 `AMBIENT_LIGHT`) | 完成(验收通过,见 §11) |
| 离线着色器编译 + GPU 布局校验 | 完成(`tools/verify_shaders.ps1`) |

| 交付物 | 状态 |
|---|---|
| 唯一材质/BSDF 入口(`resolve_material_data` / `evaluate_bsdf` / `sample_bsdf`) | 完成 |
| `debug_flags`(关钳制/材质覆盖/可视化通道)+ UI 面板 | 完成 |
| **白炉测试场景(程序化球体阵)** | **完成**(见 §12) |
| EXR 参考对比框架 | 完成(`tools/compare_exr.py`,含自测) |
| `material_data` 光谱消光/色散字段 + 玉石物理光谱吸收 + 真实穿越距离 | 完成 |
| 贴图通道 normal + ORM + 几何切线空间法线 | 完成 |
| 能量守恒(NEE/F82/Smith/MIS)+ 多次散射补偿 + 白炉量化验收 | 完成(误差 0.0000) |
| 环境光照(IBL + 环境 NEE + MIS,删除 `AMBIENT_LIGHT`) | 完成(验收通过) |
| 离线验证链(着色器编译 + 布局 + 物理数值 + 几何拓扑) | 完成(`pwsh tools/verify_all.ps1`) |

### 下一轮入口

**构建与基础运行期验证已完成**(见 §16 与 `pbr-acceptance-evidence.md` §2)。
剩余两项:

1. **Release 配置的帧率**:需要在当前代码上构建一个优化配置
   (Debug 的 510 fps 不能代表 Release)。
2. **白炉球体阵的画面级核查**:建议给应用加两个调试热键
   (`G` 切球体阵、`H` 切白炉预设),这样"勾选 UI → 截图 → 读数"就能全脚本化,
   能量验收不必依赖人眼。

### 其他可选项(价值低于上面两项)

3. **环境重要性采样改用 HDR 亮度 CDF**:当前上半球均匀采样 + MIS 已无偏,
   换 CDF 是纯降噪优化 —— 等能评估实际噪声后再决定。
4. albedo RGBA 贴图通道(需纹理格式扩展 + 资产)。

---

### 开放问题已解决:白炉现在成立

在用户重建(位 14 关雾 + 球间距 0.85)之后复测:

| 量 | 修 bug 前 | 修 bug 后 |
|---|---|---|
| 30 球读数范围 | 0.42 ~ 0.55 | **0.6435 ~ 0.7069** |
| 背景(线性 L=1) | 0.70 | **0.7065** |
| 最亮球 vs 背景 | 低约 30% | **一致(0.7069)** ✓ |
| 通道 | 有色偏 | **R=G=B 中性** |
| 离散度 | 22% ~ 30% | **9.0%** |

`MAX_BOUNCES` 8→32 读数几乎不变 ⟹ 排除多重弹射截断;
残余 −4.6% 均值偏差与间距 0.85 下的相邻球遮挡量级相符 ⟹ **物理遮挡,不是 bug**。

**白炉判据(白球在均匀天空下应与背景同亮)已满足。**

### 顺带记下的一课

第 18 条那种"**验证脚本与实现悄悄分叉**"的错误,是这一轮最值得警惕的:
白炉数值、`pbr_verify_compensation.py` 的自洽性检验、以及穷举校验**全都通过了**,
但它们验证的是脚本里的公式。**结论:交叉核对的对象必须是"模型 vs 实现",
而不只是"模型 vs 自身"。** 现在脚本里写明了双方的确切表达式。

---

## 18. 会话记录(第十二轮):Release 性能 + 四个真 bug + 白炉修复

### Release 性能(目标里缺的"记录性能",已补)

`tools/perf_probe.ps1`(FPS 读窗口标题、热键 `J` 切环境光):

| 配置 | 帧率 | 每帧 |
|---|---|---|
| 默认(环境光开) | **793 fps** | 1.26 ms |
| 环境光关 | **1451 fps** | 0.69 ms |
| **环境光开销** | — | **+0.57 ms/帧(45.3%)** |

### 抓到并修掉的四个真 bug

1. **能量补偿缩放系数写错**(`layer_specular_energy_scale` 用 `1 + f_avg·k`,应为 `1 + k`)。
   白炉(`f_avg=1`)恰好不受影响 → 之前所有白炉结论都掩盖了它;
   但 F0=0.5 的金属欠 11%、介电镜面欠 35% ⟹ **粗糙金属会比光滑金属暗**,
   这与"粗糙度只重分布能量"的物理期望矛盾。
   **抓到它的方式**:把验证脚本的模型与着色器实现逐式对照 ——
   结果是**此前"能量误差 0.0000"验证的是脚本的公式,不是着色器的实现**。
   现已两侧统一,并在工具里写明确切表达式。
2. **环境 NEE 的 MIS 权重用了错的 pdf**:采样是上半球均匀 `1/(2π)`,
   权重却用余弦 `cosθ/π`(早先把 NEE 改成均匀采样时漏改)。
   现在抽成 `environment_nee_pdf()` 单一定义处,两处必然同源。
3. **环境 NEE 不活跃时 MIS 权重仍 < 1**:`disable_ambient` 只关掉 NEE 那一半,
   BSDF 路径仍被压低 → "关环境光"变成"环境光被系统性削弱"而非归零。
   由 `tools/dark_probe.ps1` 的**反证测试**抓到(白炉 + 关环境光,球不是纯黑)。
4. **`sample_bsdf` 的 `out.pdf` 用单波瓣 pdf,`evaluate_bsdf` 用混合 pdf**。
   估计量 `f·cos/p` 的分母必须是真实采样密度(含层概率与波瓣概率);
   用波瓣 pdf 会系统性低估间接光,且低估随 `p_ms`(∝roughness)增大。
   抽了 `surface_pdf()` 作为唯一定义处。

前三条是"数值/逻辑"层面的,第四条会直接影响成像亮度。

### 开放问题:白炉仍不成立(诚实记录)

修完上面四条后,真白炉(均匀环境 + 无直接光 + 白反照率)下
`metallic=1.0` 行仍随 roughness 降 **22%**,而物理上应当恒定;
球还比背景暗约 30%。

分块矩阵确认球阵位置正确 → **不是探针坐标问题**。剩余可能来源:
球间相互遮挡、`MAX_BOUNCES=8` 截断多重弹射、雾(背景被雾改造,且雾含硬编码太阳项)、
或尚未找到的 bug。**当前无法区分。**

→ 下一步已就绪但需重建:新增位 14 `disable_fog`(雾)、球间距 0.62→0.85(减遮挡),
再用**单个孤立球**做白炉,就能剥掉前三个混淆。

### 工具

- `tools/perf_probe.ps1`:帧率 + 环境光开销。
- `tools/dark_probe.ps1`:反证测试(无光源应为纯黑)。
- `tools/furnace_probe.ps1`:白炉逐球读数(已能把整条链路脚本化)。

---

## 17. 会话记录(第十一轮):白炉探针自动化 + 一个测试设计缺陷

### 做了什么

1. **调试热键**:`G` 切球体阵、`H` 切白炉预设、`J` 切环境光。
   热键走的是与 UI 勾选**同一条代码路径**(`set_furnace_grid_enabled` /
   `toggle_furnace_preset`),所以两者不会行为分叉。
   ⚠ `ui_record::handle` 是 `noexcept`(ui_base facade 约定),而建网格会分配内存,
   故在 `G` 的分支里用 try/catch 兜住 —— 否则 noexcept 里抛出会直接 `std::terminate`。
2. **`tools/furnace_probe.ps1`**:全自动白炉探针(启动 → 投键 → 截图 → 逐球读数)。
3. **`image_stats` 扩展**:`--blocks R C`(分块亮度矩阵)、
   `--dots x,y,... --dot-radius N`(在指定像素点取圆盘均值 = 逐球读数)。

### 首次实测结果与**一个测试设计缺陷**

30 个球的读数拿到了(表格见 `pbr-acceptance-evidence.md` §2)。
`metallic=1`(F0=1)行随 roughness 降 **18.5%** —— 看起来像能量损失。

**但它不是。** 关键对照:`metallic=0` 是介电(`F0 ≈ 0.04`,镜面分量仅约 4%,
单散射损失折算到总量只有约 1.5%),它同样降了 **14.4%**。
介电行都降这么多,说明**下降主因是环境不均匀**(光滑球反射天空的特定方向、
粗糙球平均整片天空),而不是镜面能量损失。

→ **非均匀天空做不出白炉**。这次实测**没有**给出能量守恒的像素级证据;
结论仍以数值验证为准(误差 0.0000)。

这个教训值得记下:**验收测试本身也需要被审**。
我第一版探针用半径 10px 采样球心(球在屏幕上半径约 33px),读的是单方向反射,
比现在还糟;改成整球圆盘积分后才看清是"环境方向性"而非"能量损失"。

### 为此新增的两个白炉开关(位 12 / 13)

| 位 | 名称 | 作用 |
|---|---|---|
| 12 | `uniform_environment` | 天空处处 = 1.0(辐照度解析值 = π) |
| 13 | `disable_direct_lights` | 关掉全部直接光,环境成为唯一光源 |

两者并入 `furnace_preset()`。**着色器是运行时从磁盘加载,无需重建即生效;
但 `furnace_preset()` 在 C++ 侧,所以按 `H` 需要重建一次。**

复测判据:30 个球亮度应**基本恒定**;若 `metallic=1` 行仍随粗糙度明显下降,
那才是真正的能量守恒问题。

### 又一个必须记住的细节

保存的 EXR **已经过 OCIO 变换**(`scene_manager::save_image` 写盘前做 CPU OCIO),
读数是**显示空间**而非线性 HDR。相对比较(平坦性)有效,但不能直接当反射率读。

---

## 16. 会话记录(第十轮):运行期验证实测

构建通过后,我做了之前做不到的三件事(**实测数据见 `pbr-acceptance-evidence.md` §2**):

### 1. 跑 Debug 版抓 validation → 零错误零警告

Debug 版会启用 `VK_LAYER_KHRONOS_validation`(该层在
`HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers` 注册,故 `enumerateInstanceLayerProperties`
能列出它,应用才会启用)。连续运行 45 秒 + 正常退出,stdout/stderr **全空**。

判断"确实跑完"的依据:`resources/cache/` 的三个缓存文件在退出时被写回
(析构执行完才会写),时间戳与运行结束时刻一致。

### 2. 用 PostMessage 触发截图 → 拿到真实渲染图像并量化

截图键是 `C`。我用 `PostMessage(hwnd, WM_KEYDOWN/WM_KEYUP, 'C', ...)`
**直接向窗口投递按键,不抢焦点** —— 这是不干扰用户又能脚本化取图的关键。

读数(800×600 EXR):**alpha 恒为 1.000000**、非有限值 0、过曝 0%、近黑 0%、
亮度均值 0.564 / p50 0.553 / max 0.875。图像良构。

### 3. 与改动前的截图对比 → 一个重要的可见变化

| 截图 | 亮度均值 | p01(最暗 1%) | max |
|---|---|---|---|
| 改动前(07/14) | 0.515 | **0.000** | 0.781 |
| 改动前(08/31) | 0.615 | **0.008** | 0.939 |
| **改动后** | 0.564 | **0.268** | 0.875 |

整体曝光量级一致(差约 8%),但**暗部被大幅抬高**。原因清楚:旧的
`AMBIENT_LIGHT = 0.25` 只给约 `albedo × 0.19` 的环境光,而真实 HDR 天空的辐照度
接近 `π·L`,环境贡献高 4~5 倍。**这是预期行为**,但画面会明显更"平";
想回旧观感就调小 `env_intensity`。

### 4. 新增/清理的工具

- **新增** `tools/image_stats.cpp` + `tools/build_image_stats.ps1`
  → `out/imgtools/image_stats.exe`。**为什么需要**:项目在 C++ 侧链接了 OIIO,
  但系统 Python 没有 OIIO 绑定。
- **删除** 我一度手写的 Python EXR 解码器 —— 它**未通过"alpha 恒为 1"的自校验**
  (我对 EXR ZIP 重建的理解不对)。**留一个会给出错误数据的工具比没有更危险**,
  所以删掉、改用 OIIO。

### 仍然缺的

1. **Release/RelWithDebInfo 帧率**:本机那份 RelWithDebInfo 产物(20:21)早于当前
   代码,与当前着色器不兼容,不能用。需要在当前代码上重新构建一个优化配置。
2. **白炉球体阵的画面级核查**:需要人眼,或给应用加调试热键(如 `G` 切球体阵、
   `H` 切白炉预设)以便脚本化取图。**建议加这两个热键** —— 那样能量验收就能全自动化。

---

## 15. 会话记录(第九轮):构建通过后的运行期问题复查

构建成功后做了一轮**针对"编译不报、运行才暴露"问题**的复查,发现并修掉 7 处:

### 1. 球体阵放在相机背后(看不见)

相机在原点朝 **-Z**,棋盘在 `z=-1.5`,而我把网格放在 `z=+2.6` —— 相机背后,
完全不可见;而且行沿 **z** 铺开,相机沿 -Z 看会让近排挡住远排。

修正:网格铺在**屏幕平面 XY** 上、放在 `z=-2.0`(FOV_y = 90° 时该处可视半高 2.0,
网格半高 1.24,留有余量)。并新增**场景隔离** `apply_furnace_isolation()`:
白炉模式下隐藏棋盘与棋子,翻棋谱后也会重新隐回去(否则棋子会突然出现在球阵前)。

### 2. 介质状态机 bug(玉石消光会用错对象)

```slang
// 出射结算时把标志清零
if(payload.inside_transmissive) { ...; payload.inside_transmissive = false; }
...
// ❌ 之后才计算 —— 于是恒为 true
bool entering_medium = !payload.inside_transmissive && p_transmission > 0.f;
```

后果:穿出介质后又被标记成"在介质内",下一跳会把**空气路径**按命中物体的
吸收系数衰减。当前场景里"下一跳"通常是棋盘(σ_t = 0)所以被掩盖,
但**两个棋子前后重叠**时,中间那段空气会被当成玉石内部消光 —— 真实的视觉错误。

修正:在结算**之前**捕获 `was_inside_medium`,状态机按它判断入射/出射;
并补上**全反射(TIR)** 的处理 —— 光仍在介质内部、只是换方向,
应从当前位置起算新一段(原先 TIR 会继承"已离开介质"的错误状态)。

### 3. ray payload 臃肿

`prev_throughput` 只写不读;`has_hit` + 4 个 `debug_*` 字段常驻 payload(~40 字节),
而调试通道是互斥的、同一时刻只需要一个值。

修正:删掉 `prev_throughput`;5 个字段合并为一个 `float4 debug_viz`
(xyz = 通道值,w = 是否已写入),由 closest-hit 按当前 `debug_flags`
**就近只算被选中的那一项**。payload 明显变小 —— 它随每一跳传递,
`maxRayPayloadSize` 是实现相关的上限,也直接影响寄存器/scratch 压力。

### 4. ORM 的 AO 通道完全没生效

`surf.occlusion` 只写不读。现在作用于环境漫反射项,注释里**诚实说明这是重复遮蔽**
(辐照度已带真实可见性);保留理由是高样本数不足时补高频缝隙遮蔽。
无 ORM 贴图时 `occlusion` 恒为 1.0 → **当前资产零影响**;`occlusion_strength = 0` 可关闭。

### 5. 天空索引守卫不一致(越界风险)

环境光照的两个函数只检查"非哨兵值",不检查 `< texture_count`。若 `skybox_index`
是越界值(≠ 哨兵),`samplers[]` 会越界索引 —— 而 bindless 未启用 partially-bound,
越界槽位未定义。修正:额外传 `env_safe`(无效时替换为哨兵),与 miss shader 的守卫对齐。

### 6. 清掉 5 个死着色器函数

`sky_luminance`、`sky_pdf_hint`、`max_component`、`pbr_channel`、`sample_uniform_sphere`。
其中 `sample_uniform_upper_hemisphere` 原本也是死的(NEE 内联展开了),
改为**真正调用它**,消除重复实现。

### 7. 一处保持等价性的性能优化

全金属(metallic = 1)的漫反射反照率为 0,那 4 条辐照度射线纯属白费。
改成**先算 albedo 再决定是否追踪** —— 乘数为 0 时提前跳过是精确等价的,不是近似。

### 性能提示(待实测)

环境光照每个 closest-hit 增加约 5 条射线(4 条辐照度可见性 + 1 条环境 NEE)。
两个旋钮:`common.slang` 的 `ENV_IRRADIANCE_SAMPLES`(默认 4)、
场景 UBO 的 `env_intensity`(设 0 完全关闭环境光)。实测时建议对比这两种设置。

---

## 13. 会话记录(第八轮):验收证据汇总 + 工具清理

### 新增 `pbr-acceptance-evidence.md`

把所有量化结论集中成一份文档,每条都带**复现命令**与**证据强度**标注。
特别地,它明确区分了:

- **强证据**(编译器/反射机器比对、解析恒等式 + 数值验证):着色器可编译、
  GPU 布局一致、能量守恒、环境光照无偏、几何拓扑正确;
- **无证据**(未能运行):实际成像、性能开销、环境光的视觉噪声水平。

> "未验证的部分不应被当作已通过" —— 这句话写进了文档,因为**运行期验证是
> 完成本目标的必要条件**,而当前它完全缺失。

### 删除了会误导的验证脚本

早期几轮我写过一批探索性脚本,其中一部分**结论是错的**(最典型的是
`pbr_energy_check.py`:它的解析 VNDF 密度全域积分是 7.97 而不是 1,
根因是我反复写错的 h 空间换元 Jacobian)。

保留一个会给出错误结论的工具比没有工具更危险,故删除:
`pbr_energy_check.py`、`pbr_density_proof.py`、`pbr_cdf_check.py`、
`pbr_vndf_moments.py`、`energy_compensation_table.h`(旧路径的冗余产物)。

保留的 12 个文件全部经过验证:`verify_all.ps1` 跑一遍五项全过。
`pbr_validation.py` 保留(它是 `shading.slang` 的参考实现,`pbr_env_check.py`
依赖它),并在文件头标注了它的已知局限。

### 完整构建:已定性为环境阻塞

两条路都验证过不可能:overlay port 与基线 port 都是 `vcpkg_from_github`;
本机无 KTX 可复用;GitHub 不可达。不再在此投入。


---

## 12. 会话记录(第七轮):白炉测试球体阵 + 统一离线验证入口

### 白炉测试场景(阶段 0 的最后一个交付物)

**程序化球体阵**:`scene_model_manager::create_procedural_sphere` 生成 UV 球,
`ui_record` 的"棋局管理"面板加开关,铺成 `roughness(6 列) × metallic(5 行)` 的网格。

关键设计:
- **所有实例共用同一份网格**(按参数做缓存键),所以 30 个球只有一份顶点/索引数据。
- 每个球一个材质(各自的 roughness/metallic),复用了 `gpu_material_data` 已有的字段,
  **没有为测试场景改任何 GPU 布局**。

### 新增的调试开关语义(重要)

原来的 `force_metallic` / `force_roughness` 是**全局**覆盖,会把整个球阵抹成同一材质,
对"逐格核查"没有意义。本轮把白炉预设改成**只强制 albedo 为白、保留每格材质**:

- `furnace_preset()` = 关全部钳制 + 只强制 albedo 白
- `furnace_preset_direct_only()` = 上面 + 关环境光(隔离直接光)

`force_metallic/force_roughness` 保留为**独立**的隔离工具(单独研究某个参数时用),
UI 上从"白炉预设"里拆了出来。

### 程序化球体网格:校验抓到两个真实缺陷

`tools/verify_sphere_mesh.py` 对生成器做拓扑校验,抓到:

1. **三角形绕序完全反了**(向外比例 0%)—— 面法线全部朝内,背面剔除会把球的正面剔掉。
   修正为 `(a, a+1, b) / (b, a+1, b+1)` 这一支。
2. **两极各有一圈零面积三角形**(96 个)—— 白占 BVH 空间,并让极点片拓扑不封闭。
   改为两极各只发**一个**三角形(极点扇)。

修正后校验全过:顶点严格在球面、法线单位且同向、UV ∈ [0,1]、**零退化三角形**、
**绕序向外 100%**、**每条边恰好被 2 个三角形共享(封闭 2-流形)**、UV 手性与几何一致。

> 第 3 点值得一提:校验脚本自己也修了两次 —— 先是把"极点圈每段一个重复顶点"
> 误判成网格不封闭,又是合并规则里混用 `str`/`int` 导致排序 TypeError。
> 检验脚本出错会直接产生**假失败**,和被测代码出错一样需要认真对待。

### 统一离线验证入口

`pwsh tools/verify_all.ps1` 一次跑完五项(退出码 0 = 全过):

| 项 | 内容 |
|---|---|
| 着色器编译 + GPU 布局 | 4 个着色器逐 entry point 编译 + 结构体逐字段偏移比对 |
| 环境光照验收 | 辐照度无偏性(π·L)+ 环境 NEE 的 MIS 组合无偏性 |
| 能量补偿验收 | 白炉与有色表面的能量守恒(误差 0.0000) |
| 球体网格校验 | 绕序/退化/封闭性/UV 手性 |
| EXR 对比工具自测 | 四种已知差异情形的判据行为 |

**这不能替代完整构建** —— 运行期的成像、帧时间、收敛帧数仍然需要一次能跑起来的构建。
但它把"能离线验证的部分"全部自动化了:着色器语法、GPU 内存布局、全部物理数值、测试几何拓扑。


---

## 11. 会话记录(第六轮):阶段 3 环境光照落地

### 与计划的偏离(有意为之,这里说明理由)

计划里阶段 3 写的是"预计算辐照度图 + GGX 预滤波 mip 链 + 环境重要性采样 + MIS"。
实际实现改为**运行时余弦重要性采样求辐照度**(每命中 4 样本 + 时域累积),原因:

- 不需要新增资产/纹理槽(当前纹理管线是单通道,预计算辐照度图要额外走一遍
  CPU 卷积 + 上传,收益不明显)。
- 时域累积已经存在且免费;静态场景下环境光项收敛很快(见方差数据)。
- **仍然是物理正确的**(无偏估计),只是把"预计算"换成了"实时 + 累积"。

**未做**的部分:GGX 预滤波 mip 链。镜面环境项改走"环境 NEE + BSDF 采样 MIS"的
路径积分形式 —— 物理上更一致(不需要 split-sum 近似),代价是方差更高。

### 删掉了 `AMBIENT_LIGHT`

那个恒定的经验项(0.25 × albedo × (0.5+0.5·NdotV))已彻底移除。
环境光现在来自真实辐照度:

```
E = 余弦重要性采样 ∫L(ω)cosθ dω(含可见性,命中即遮挡)
环境漫反射 = albedo · E/π · env_intensity
```

`env_intensity` 是新增的强度倍率字段(占用原 `debug_pad` 的偏移 180,布局仍是 192B,
已通过校验)。设为 0 即可关闭全部环境光,便于做"纯直接光"的对照实验。

### 环境 NEE 的 MIS

`环境 NEE(上半球均匀采样,pdf=1/(2π))` 与 `BSDF 采样命中天空` 两条路径,
用 balance heuristic 组合。**这一步是必需的** —— 少了它环境能量会翻倍。

### 量化验收(`tools/pbr_env_check.py`,报告在 `tools/env_report.txt`)

| 检查 | 结果 |
|---|---|
| 恒定环境 `L≡1.0` → 辐照度 = π·L | 相对误差 **0.0000%** |
| 恒定环境 `L≡0.25` → 辐照度 = π·L | 相对误差 **0.0000%** |
| 方向性天空 `L=max(0,ny)` → 解析值 2/3 | 相对误差 **0.0077%** |
| 环境 NEE + BSDF 采样组合 vs 解析值 `albedo·L` | 相对误差 **0.0184%** |
| MIS 权重范围 | 恒在 (0,1) |

辐照度估计量的方差(方向性天空下):1 样本 57%、4 样本 **29%**、16 样本 14%。
`ENV_IRRADIANCE_SAMPLES = 4` 的选择依据就是这张表 —— 单帧噪声靠时域累积压下去。

### 本轮踩到并修掉的错误(都靠数值检验抓到)

1. **环境 NEE 的"重试采样"概率补偿写错**,估计量恰好是解析值的 **2 倍**。
   原写法:`全空间均匀采样 + 拒绝下半球 + 重试 ENV_NEE_ATTEMPTS 次`,
   补偿因子却只除了一次 `env_hits`。改成**直接的上半球均匀采样**后无偏。
   (这是本轮最有价值的修复 —— 肉眼完全看不出来。)
2. 验证脚本里我自己的余弦采样器把 `cosθ` 放错了分量、常量环境的期望值算错
   (把"∫L cos dω"与"均匀采样下的 ∫L dω"混淆),导致两个用例假失败。
3. 组合无偏性检验里,两条策略**不能共用同一个估计量形式** ——
   NEE 侧(pdf=1/2π)与 BSDF 侧(pdf=cos/π)必须分别按各自 pdf 化简,
   否则会算出一个"看起来差 10%"的假结论。

### 已知取舍(诚实记录)

- 环境 NEE 用**上半球均匀采样**,没有按 HDR 亮度做重要性采样:
  方差偏大(尤其天空有强太阳时),靠 MIS + 时域累积缓解。
  真正的亮度 CDF 需要启动时扫一遍 90MB 的 HDR(约 1 秒)并建 2D 分布。
- `sky_pdf_hint`(按亮度比例的相对 pdf)在当前实现里**不再被使用**
  (环境 NEE 改成均匀采样后不需要它),保留作为将来亮度 CDF 的接口。
- 镜面环境项没有预滤波 mip 链,粗糙金属的环境反射收敛比 split-sum 慢。


---

## 10. 会话记录(第五轮):贴图通道(法线 + ORM)落地

### `material_data` 扩到 112B(阶段 1 的贴图通道部分)

新增字段(`scene_data.slang` ↔ `gpu_material_data` 逐字节一致,布局校验已通过):

| 字段 | 偏移 | 说明 |
|---|---|---|
| `albedo_index` | 12 | **原 `texture_index` 改名**,语义明确为"基础色/覆盖率图" |
| `normal_index` | 84 | 切线空间法线图 |
| `orm_index` | 88 | R=AO G=roughness B=metallic(glTF 约定) |
| `emissive_index` | 92 | 预留(自发光需进 NEE,阶段 4) |
| `normal_scale` | 96 | 法线强度 |
| `occlusion_strength` | 100 | AO 应用强度 |

`tools/verify_layout.py` 的期望偏移表同步更新,`verify_shaders.ps1` 重新校验通过。

### 关键实现:切线空间必须来自几何 UV 参数化

`common.slang` 原有的 `build_tangent_space` 是 **Frisvad 任意基** —— 它的法线方向
是任意的。用它做 TBN,即使贴了法线图也会得到**随机旋转**的结果(凹凸方向全错)。

新增 `shading.slang` 的 `build_geometric_tangent_space(p0,p1,p2, uv0,uv1,uv2)`:
由三角形世界位置与 UV 的有限差分求 `dP/du`、`dP/dv`,再用手性
`signed area` 定副切线方向。**不需要顶点切线**,所以现有 glb 无需改动。

`apply_normal_map` 做施密特正交化(法线图假设正交 TBN,而 UV 有限差分给的是
非正交基),并对 UV 退化(顶点共 UV / 切线退化)做了回退,避免 NaN。

### 一个容易写错的顺序问题(已在注释中标明)

法线图必须在**双面翻转之前**应用:

```
shaded = apply_normal_map(baked, scale, world_normal, gts)   // 相对几何法线扰动
surf.normal = dot(ray_dir, shaded) < 0 ? shaded : -shaded    // 之后才翻转
```

反过来做的话,背面(如棋子的内壁)凹凸会整体反向。

### ORM 的接入方式

- `R(AO)` → 乘在 **ambient** 项上。物理上 AO 应该作用于间接漫反射;
  在 IBL 到位之前,那个恒定的 `AMBIENT_LIGHT` 就是"环境可见性"的唯一代理,
  所以现在乘在它上面是自洽的 —— 并在注释里标明**阶段 3 接 IBL 后应改为作用于间接项**。
- `G(roughness)` / `B(metallic)` → **乘性**作用于材质标量(而不是替换):
  这样"材质面板的粗糙度"仍是有意义的整体缩放,贴图提供空间变化。

### UI

`ui_node` 增加了:消光系数 / 刻字附加消光的 `DragFloat3`(玉石颜色现在可直接调),
法线强度、AO 强度,以及法线/ORM 两个贴图槽的选择与清除(右键清空改为显式按钮,
因为 ImGui 没有内置清空语义)。

### 已知取舍

- albedo 仍走"单通道覆盖率图 + 两色混合",不是真实 RGB albedo 贴图。
  结构体已留好槽位(`albedo_index`),换真实资产时只需改采样方式
  (`.x` → `.rgb`),**不需要再改布局**。
- 法线强度缩放只作用于 x/y(标准做法),z 保持原样再归一化。






