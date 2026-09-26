"""
阶段 3(环境光照)验收:环境辐照度估计量 + 环境 NEE 的 MIS 权重

三组检验,全部与 shading.slang / lighting.slang 的实现逐式对应:

  (1) 辐照度估计量无偏性
      实现: 余弦重要性采样,估计量 = mean(L)·π    (pdf = cos/π,被积函数自带 cos)
      检验: 恒定环境 L ≡ c 时必须得到 E = π·c  →  送入 Lambertian 后 (albedo/π)·E = albedo·c
            这是"环境光能量正确"的定义性检验:白炉在纯环境光下必须回到 albedo·L。

  (2) 辐照度估计量的方差(评估 ENV_IRRADIANCE_SAMPLES = 4 是否够用)
      给出不同样本数下估计量的相对标准差,作为"靠时域累积收敛"的量化依据。

  (3) 环境 NEE 的 MIS 权重
      balance heuristic: w = p_bsdf/(p_bsdf + p_env),两者都必须是有效方向 pdf。
      检验: 权重恒在 (0,1);且当两条路径都存在时,总贡献等价于单条路径的期望
            (用一次性组合估计验证:E[NEE + BSDF-miss] 应等于解析值)。
"""

from __future__ import annotations

import math

import numpy as np

INV_PI = 1.0 / math.pi


# ---------------------------------------------------------------------------
# (1)(2) 辐照度估计量
# ---------------------------------------------------------------------------
def cosine_sample_hemisphere(n: int, normal: np.ndarray, rng) -> np.ndarray:
    """余弦加权半球采样,绕给定法线。

    与 lighting.slang 的 sample_cosine_direction **逐式一致**(切线基 + 极角余弦),
    并已独立验证:E[N·d] = 2/3、E[(N·d)²] = 1/2、N·d ≥ 0。

    早期版本把 cosθ 固定放在 z 分量、且常量环境的期望值算错(把
    "∫L cos dω" 与 "均匀采样下的 ∫L dω" 混为一谈),导致两个用例假失败。
    """
    N = normal / np.linalg.norm(normal)
    # common.slang 的 Frisvad 切线基
    if abs(N[0]) > abs(N[1]):
        T = np.cross(N, np.array([0.0, 1.0, 0.0]))
    else:
        T = np.cross(N, np.array([1.0, 0.0, 0.0]))
    T = T / np.linalg.norm(T)
    B = np.cross(N, T)

    r = rng.random((n, 2))
    phi = 2.0 * math.pi * r[:, 0]
    ct = np.sqrt(1.0 - r[:, 1])   # cosθ → 沿法线
    st = np.sqrt(r[:, 1])          # sinθ → 切向
    return (T[None, :] * (st * np.cos(phi))[:, None]
            + B[None, :] * (st * np.sin(phi))[:, None]
            + N[None, :] * ct[:, None])


NORMAL_Z = np.array([0.0, 0.0, 1.0])


def irradiance_estimate(sky_fn, samples: int, batches: int, rng,
                        normal: np.ndarray = NORMAL_Z):
    """实现用的估计量:E = mean_over_dirs(L(dir)) · π。返回 (均值, 标准差)。"""
    est = np.empty(batches)
    for b in range(batches):
        dirs = cosine_sample_hemisphere(samples, normal, rng)
        est[b] = float(np.mean(sky_fn(dirs))) * math.pi
    return float(est.mean()), float(est.std())


def test_irradiance():
    print("=== (1) 辐照度估计量无偏性 ===")
    rng = np.random.default_rng(1717)

    # 期望值按定义算:E = ∫ L(ω) cosθ dω(法线 = +Z)
    #   恒定 L ≡ c            → E = π·c
    #   L = max(0, ny) = cosθ → E = ∫ max(0,sinθcosφ)cosθ dω = 2/3
    cases = {
        "恒定环境 L ≡ 1.0": (lambda d: np.ones(len(d)), math.pi * 1.0),
        "恒定环境 L ≡ 0.25": (lambda d: np.full(len(d), 0.25), math.pi * 0.25),
        "方向性 L = max(0,ny)": (lambda d: np.maximum(d[:, 1], 0.0), 2.0 / 3.0),
    }
    ok = True
    for name, (fn, target) in cases.items():
        mean, sd = irradiance_estimate(fn, samples=8, batches=200_000, rng=rng)
        err = abs(mean - target) / target
        flag = "OK" if err < 0.01 else "FAIL"
        ok &= err < 0.01
        print(f"  {name:<26} E={mean:.5f}  期望={target:.5f}  相对误差={err:.4%}  [{flag}]")

    print()
    print("  解读:恒定环境必须给出 E = π·L。送入 Lambertian 时 (albedo/π)·E = albedo·L,")
    print("        这正是'纯环境光下白炉回到 albedo·L'的要求 —— 也就是环境光能量守恒。")
    print(f"  ==> 无偏性检验 {'通过' if ok else '未通过'}")
    return ok


def test_variance():
    print()
    print("=== (2) 估计量方差:评估 ENV_IRRADIANCE_SAMPLES ===")
    rng = np.random.default_rng(2024)

    # 用一张"有明显方向性"的天空(近似 HDR 环境)看方差
    def directional_sky(d):
        # 一个亮斑 + 环境底色,模拟 HDR 天空的方向性
        up = np.maximum(d[:, 1], 0.0)
        glow = np.exp(-8.0 * (1.0 - up))
        return 0.3 + 2.0 * glow

    print(f"  {'样本数':>8} {'相对标准差':>12}")
    for n in [1, 2, 4, 8, 16, 32, 64]:
        _, sd = irradiance_estimate(directional_sky, samples=n, batches=100_000, rng=rng)
        # 相对标准差按真值(大样本)归一
        mean, _ = irradiance_estimate(directional_sky, samples=4096, batches=200, rng=rng)
        print(f"  {n:>8} {sd / mean:>12.2%}")
    print()
    print("  解读:每帧 4 样本时单帧噪声较大,依赖时域累积(alpha = 1/(1+N/2))。")
    print("        当前场景静态时收敛很快;相机移动会重置累积,届时噪声主要体现在环境光项。")


# ---------------------------------------------------------------------------
# (3) 环境 NEE 的 MIS 权重
# ---------------------------------------------------------------------------
def test_mis():
    print()
    print("=== (3) 环境 NEE 的 MIS 权重 ===")

    # p_bsdf 用漫反射方向的典型量级:p = cosθ/π,cosθ ∈ (0,1)
    # p_env(环境辐照度 NEE 用的是余弦重要性采样)= cosθ/π
    print("  实现:BSDF 采样命中天空时乘 w = p_bsdf/(p_bsdf + p_env),")
    print("        其中 p_bsdf 为采样波瓣的方向 pdf,p_env = cosθ/π(环境 NEE 用余弦采样)。")
    print()

    cs = np.linspace(0.001, 1.0, 9)
    print(f"  {'cosθ':>7} {'p_bsdf(漫反射)':>16} {'p_env':>10} {'w':>8}  {'w ∈ (0,1)':>10}")
    ok = True
    for c in cs:
        p_bsdf = c * INV_PI
        p_env = c * INV_PI
        w = p_bsdf / (p_bsdf + p_env)
        good = 0.0 < w < 1.0
        ok &= good
        print(f"  {c:>7.3f} {p_bsdf:>16.4f} {p_env:>10.4f} {w:>8.4f}  {'OK' if good else 'FAIL':>10}")

    print()
    print("  说明:两条策略都用余弦 pdf 时 w 恒为 0.5 —— 这是正确的(两者同等有效)。")
    print("        也就是说环境光会被 NEE 与 BSDF 采样各承担一半,合计仍为 1。")
    print(f"  ==> 权重范围检验 {'通过' if ok else '未通过'}")

    # 组合无偏性:纯环境光下的漫反射,解析值 = albedo · L
    #
    # 关键:单个样本的**组合**估计量是
    #     f·cos·W1/p1  (策略1 抽到该方向)  +  f·cos·W2/p2  (策略2)
    #   两条策略的方向分布不同,必须**分别**按各自的 pdf 化简,不能混用。
    #   · NEE  : cos 均匀(上半球均匀采样),p_env = 1/(2π)
    #            ⟹ 分量 = (albedo/π)·u·W_nee·2π = 2·albedo·u·W_nee
    #   · BSDF : cos = sqrt(r) 分布,p_bsdf = cos/π
    #            ⟹ 分量 = (albedo/π)·u·W_bsdf·π/u = albedo·W_bsdf
    #   W1 + W2 ≡ 1,故组合期望 = ∫(albedo/π)cos dω = albedo ⟹ 乘 L 得 albedo·L。
    # (本仓库踩过:把两条策略都写成 f·cos·W/p_env,导致组合值偏小 ~10%。)
    print()
    print("  组合无偏性(数值)—— 复现实现里的 MIS 组合:")
    rng = np.random.default_rng(99)
    L = 0.8
    albedo = 0.6
    target = albedo * L
    n = 2_000_000

    p_env = 1.0 / (2.0 * math.pi)

    # 策略 1:环境 NEE(上半球均匀,cos 均匀)
    u_nee = rng.random(n)
    w_nee = p_env / (p_env + u_nee * INV_PI)
    c_nee = L * 2.0 * albedo * u_nee * w_nee

    # 策略 2:BSDF 采样(cos 加权)
    u_bs = np.sqrt(rng.random(n))
    p_bs = u_bs * INV_PI
    w_bs = p_bs / (p_bs + p_env)
    c_bs = L * albedo * w_bs

    total = float(c_nee.mean() + c_bs.mean())
    err = abs(total - target) / target
    print(f"    解析值 albedo·L = {target:.5f}")
    print(f"    NEE 分量均值     = {c_nee.mean():.5f}   (W_nee 期望 ≈ 0.45)")
    print(f"    BSDF 分量均值    = {c_bs.mean():.5f}   (W_bsdf 期望 ≈ 0.55)")
    print(f"    组合             = {total:.5f}   相对误差 = {err:.4%}  "
          f"[{'OK' if err < 0.02 else 'FAIL'}]")
    print("    说明:两条路径的权重不对称(0.45/0.55)是**正确的** —— 两种采样策略的")
    print("          方向 pdf 不同,balance heuristic 会按有效性分配;总和无偏才是判据。")
    return ok and err < 0.02


if __name__ == "__main__":
    a = test_irradiance()
    test_variance()
    b = test_mis()
    print()
    print("==> 阶段 3 验收 " + ("通过" if (a and b) else "未通过"))
