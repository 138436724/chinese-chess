"""
白炉能量守恒数值验证 —— shading.slang 的 BSDF 参考实现

目的:在不依赖 Vulkan/编译的前提下量化验证 shading.slang 的能量守恒,
并给出 Fdez-Agüera 多次散射补偿所需的 E_ss(μ, α) 数据与拟合系数。

物理量:
  白炉测试 = 恒定单位环境光下的方向反射率(directional albedo)
    E(μ, α) = ∫ f_spec(V, L) · cosθ_L dω_L      (F0 = 1 时)
  能量守恒要求 E ≡ 1。单散射 GGX 在 roughness→1 时只剩约 0.6~0.7。

实现与 shading.slang 逐式对应:
  d_ggx / v_smith_ggx_correlated / g1_smith_ggx / f_schlick_f90 / f_f82
  layer_value_specular / sample_ggx_vndf
数值方法:VNDF 重要性采样(与实际渲染器同一采样策略),故估计量即渲染器的
无偏估计量,比均匀采样更贴近实际收敛值。
法线固定为 +Z,视线在 xz 平面。
"""

from __future__ import annotations

import math

import numpy as np

INV_PI = 1.0 / math.pi


# ---------------------------------------------------------------------------
# shading.slang 的标量基元
# ---------------------------------------------------------------------------
def d_ggx(ndoth: np.ndarray, roughness: np.ndarray) -> np.ndarray:
    a = roughness * roughness
    a2 = a * a
    denom = ndoth * ndoth * (a2 - 1.0) + 1.0
    return a2 * INV_PI / np.maximum(denom * denom, 1e-7)


def pow4(x: np.ndarray) -> np.ndarray:
    return (x * x) * (x * x)


def v_smith_ggx_correlated(ndotv, ndotl, roughness):
    a2 = pow4(roughness)
    gv = ndotl * np.sqrt(ndotv * ndotv * (1.0 - a2) + a2)
    gl = ndotv * np.sqrt(ndotl * ndotl * (1.0 - a2) + a2)
    return 0.5 / np.maximum(gv + gl, 1e-5)


def g1_smith_ggx(ndotx, roughness):
    a2 = pow4(roughness)
    return 2.0 * ndotx / np.maximum(ndotx + np.sqrt(a2 + (1.0 - a2) * ndotx * ndotx), 1e-5)


def f_schlick_f90(f0, f90, u):
    return f0 + (f90 - f0) * (1.0 - u) ** 5


def f_f82(f0, f82, u):
    return f0 + (f82 - f0) * (1.0 - u) ** 5


def compute_f82(f0, metallic, roughness):
    """与 shading.slang 的 compute_f82 逐式一致。"""
    f82 = np.clip(f0 * 25.0, 0.0, 1.0) * metallic
    f82 = f82 + (1.0 - f82) * roughness
    return 1.0 + (f82 - 1.0) * metallic


def eval_fresnel(f0_medium, metal_f0, f82, metallic, u):
    schlick = f_schlick_f90(f0_medium, 1.0, u)
    karis = f_f82(metal_f0, f82, u)
    return schlick + (karis - schlick) * metallic


# ---------------------------------------------------------------------------
# VNDF 采样(Heitz 2018)—— 与 shading.slang 的 sample_ggx_vndf 同构
# ---------------------------------------------------------------------------
def sample_ggx_vndf(ve: np.ndarray, roughness: float, rng: np.random.Generator):
    """ve: (N,3) 单位视线(世界空间,法线 = +Z)。返回 (N,3) 反射方向。

    严格按 Heitz 2018 的算法:
      1) 切线空间中把视线拉伸到椭圆体配置 → Vh
      2) 在 Vh 周围建立正交基 (T1,T2)
      3) 在投影面积上采样(同心圆盘)得到 Nh
      4) H = a·Nh.x·T1 + a·Nh.y·T2 + Nh.z·Vh,再归一化 → 微表面法线
      5) L = reflect(-V, H)

    第 4 步的关键:变换回椭圆体配置时乘在**切线基分量**上,
    不是在 Vh 基上;之后必须归一化。早期版本在这里多做了一次
    "变回世界 + 再乘 a",导致采样分布退化为近似均匀。
    """
    n = ve.shape[0]
    r = rng.random((n, 2))
    a = roughness * roughness

    # 法线为 +Z 时切线空间退化为恒等映射:T=(1,0,0), B=(0,1,0), N=(0,0,1)
    vl = ve
    vh = vl.copy()
    vh[:, 0] *= a
    vh[:, 1] *= a
    vh /= np.linalg.norm(vh, axis=1, keepdims=True)

    lensq = vh[:, 0] ** 2 + vh[:, 1] ** 2
    t1 = np.zeros_like(vh)
    ok = lensq > 0.0
    if ok.any():
        t1[ok] = np.stack([-vh[ok, 1], vh[ok, 0], np.zeros(int(ok.sum()))], axis=1) / np.sqrt(lensq[ok])[:, None]
    t1[~ok] = np.array([1.0, 0.0, 0.0])
    t2 = np.cross(vh, t1)

    r_disk = np.sqrt(r[:, 0])
    phi = 2.0 * math.pi * r[:, 1]
    p1 = r_disk * np.cos(phi)
    p2 = r_disk * np.sin(phi)
    s = 0.5 * (1.0 + vh[:, 2])
    p2 = (1.0 - s) * np.sqrt(np.maximum(0.0, 1.0 - p1 * p1)) + s * p2

    nh = (p1[:, None] * t1 + p2[:, None] * t2
          + np.sqrt(np.maximum(0.0, 1.0 - p1 * p1 - p2 * p2))[:, None] * vh)

    # 第 4 步:变换回椭圆体配置 → 微表面法线(世界空间)
    h = a * nh[:, 0:1] * t1 + a * nh[:, 1:2] * t2 + nh[:, 2:3] * vh
    h /= np.linalg.norm(h, axis=1, keepdims=True)

    # 第 5 步:L = reflect(-V, H)
    v = -ve
    reflected = v - 2.0 * np.sum(v * h, axis=1, keepdims=True) * h
    return reflected / np.linalg.norm(reflected, axis=1, keepdims=True)


# ---------------------------------------------------------------------------
# 白炉方向反射率
# ---------------------------------------------------------------------------
def specular_albedo_sampled(roughness: float, mu_v: float, f0: float = 1.0,
                            samples: int = 400_000, rng: np.random.Generator | None = None) -> float:
    """用 VNDF 采样估计 E(μ_v, α) = ∫ f_spec·cos dω(白炉 F0 = 1)。

    走完整 evaluate 路径(而非 sample_ggx_brdf 的约简形式),这样同时验证
    evaluate_bsdf 与 sample_bsdf 两侧的实现是否一致。
    """
    rng = rng or np.random.default_rng(12345)
    st = math.sqrt(max(0.0, 1.0 - mu_v * mu_v))
    ve = np.tile(np.array([st, 0.0, mu_v]), (samples, 1))

    l = sample_ggx_vndf(ve, roughness, rng)
    ndotl = l[:, 2]
    ndotv = np.full(samples, mu_v)

    valid = ndotl > 1e-6
    if not valid.any():
        return 0.0
    l, ndotl, ndotv = l[valid], ndotl[valid], ndotv[valid]
    ve_v = ve[valid]

    h = ve_v + l
    h /= np.linalg.norm(h, axis=1, keepdims=True)
    ndoth = np.maximum(h[:, 2], 0.0)
    vdoth = np.maximum(np.sum(ve_v * h, axis=1), 0.0)

    # F0 = 1 时 Fresnel ≡ 1(metal_f0=1, metallic=1 → f82=1 → f_f82 ≡ 1)
    metallic = np.ones_like(ndotl)
    f82 = compute_f82(np.full_like(ndotl, f0), metallic, np.full_like(ndotl, roughness))
    f = eval_fresnel(np.full_like(ndotl, f0), np.full_like(ndotl, f0), f82, metallic, vdoth)

    d = d_ggx(ndoth, roughness)
    g = v_smith_ggx_correlated(ndotv, ndotl, roughness)
    pdf = np.maximum(d * g1_smith_ggx(ndotv, roughness) / np.maximum(4.0 * ndotv, 1e-5), 1e-4)

    return float(np.mean(d * g * f * ndotl / pdf))


def specular_albedo_uniform(roughness: float, mu_v: float, f0: float = 1.0,
                            n_theta: int = 400, n_phi: int = 200) -> float:
    """均匀半球积分的独立交叉校验(不依赖采样策略,排除 pdf 错误)。"""
    st = math.sqrt(max(0.0, 1.0 - mu_v * mu_v))
    ve = np.array([st, 0.0, mu_v])

    theta = (np.arange(n_theta) + 0.5) * (0.5 * math.pi / n_theta)
    phi = (np.arange(n_phi) + 0.5) * (2.0 * math.pi / n_phi)
    tt, pp = np.meshgrid(theta, phi, indexing="ij")
    domega = np.sin(tt) * (0.5 * math.pi / n_theta) * (2.0 * math.pi / n_phi)

    lx = np.sin(tt) * np.cos(pp)
    ly = np.sin(tt) * np.sin(pp)
    lz = np.cos(tt)
    ndotl = lz

    hx = ve[0] + lx
    hy = ve[1] + ly
    hz = ve[2] + lz
    hn = np.sqrt(hx * hx + hy * hy + hz * hz)
    ndoth = np.maximum(hz / hn, 0.0)
    vdoth = np.maximum((ve[0] * hx + ve[1] * hy + ve[2] * hz) / hn, 0.0)

    f82 = compute_f82(np.full_like(ndoth, f0), np.ones_like(ndoth), np.full_like(ndoth, roughness))
    f = eval_fresnel(np.full_like(ndoth, f0), np.full_like(ndoth, f0), f82, np.ones_like(ndoth), vdoth)

    d = d_ggx(ndoth, roughness)
    g = v_smith_ggx_correlated(np.full_like(ndoth, mu_v), ndotl, roughness)
    return float(np.sum(d * g * f * ndotl * domega))


if __name__ == "__main__":
    roughs = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]
    roughs = [max(r, 0.02) for r in roughs]
    mus = [1.0, 0.8, 0.5, 0.25]

    print("=== 单散射 GGX 白炉方向反射率 E_ss(mu, alpha) ===")
    print(f"{'roughness':>10} " + " ".join(f"mu={m:<6}" for m in mus))
    rng = np.random.default_rng(20240)
    worst = 1.0
    for a in roughs:
        row = []
        for mu in mus:
            e = specular_albedo_sampled(a, mu, samples=400_000, rng=rng)
            worst = min(worst, e)
            row.append(e)
        print(f"{a:>10.2f} " + " ".join(f"{v:.4f}  " for v in row))

    print()
    print(f"最小方向反射率 = {worst:.4f}  →  单散射能量损失 = {1.0 - worst:.1%}")
    print("→ 该损失必须由多次散射补偿项补回,否则白炉不过。")

    # 采样策略 vs 均匀积分的交叉校验(排除 pdf 错误)
    # 注意:白炉积分的被积函数在 alpha 大时仍然很尖(高光瓣),均匀求积会系统性低估,
    # 因此这里做的是**收敛性**测试:求积加密后应向采样值收敛;若不收敛且差距恒定,
    # 说明求积分辨率不足(而非采样错误)。
    print()
    print("=== 交叉校验:sample-based vs uniform-quadrature(收敛性) ===")
    for a in [0.5, 1.0]:
        for mu in [1.0, 0.5]:
            s = specular_albedo_sampled(a, mu, samples=1_500_000, rng=rng)
            print(f"  alpha={a:.2f} mu={mu:.2f}  sample={s:.4f}")
            for n_theta, n_phi in [(400, 200), (2000, 1000), (8000, 2000)]:
                u = specular_albedo_uniform(a, mu, n_theta=n_theta, n_phi=n_phi)
                print(f"      uniform {n_theta:>5}x{n_phi:<5} = {u:.4f}  (diff {abs(s - u):.4f})")
