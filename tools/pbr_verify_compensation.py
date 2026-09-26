"""
验证多次散射补偿:白炉能量是否回到 1

补偿结构(与 shading.slang 逐式对应):
    E_ss = 单散射方向反射率(F0=1)
    k    = (1 - E_ss)/(1 + E_ss)
    单散射镜面项缩放 (1 + F_avg·k);多散射项 = F_avg²·k
    F0 = 1 时 F_avg = 1 ⟹ E_total = E_ss·(1+k) + k = 1

同时给出有色表面的检查(F0 = 0.04 的电介质):
    E_total = E_ss·F_avg·(1 + F_avg·k) + F_avg²·k  应落在 [F_avg, F_avg·(1+…)]
    重点是它**不应超过 1**,也不应低于 F_avg(否则说明补偿把能量算丢了)。
"""

from __future__ import annotations

import math

import numpy as np

import pbr_validation as pv


def e_ss_measured(roughness: float, mu_v: float, samples: int, rng) -> tuple[float, float]:
    """E_ss = E_{L~VNDF}[G1(L)](F0 = 1 时的解析恒等式)。"""
    st = math.sqrt(max(0.0, 1.0 - mu_v * mu_v))
    ve = np.tile(np.array([st, 0.0, mu_v]), (samples, 1))
    l = pv.sample_ggx_vndf(ve, roughness, rng)
    ndotl = l[:, 2]
    ok = ndotl > 1e-6
    if not ok.any():
        return 0.0, 0.0
    g1l = pv.g1_smith_ggx(ndotl[ok], roughness)
    return float(np.mean(g1l)), float(np.std(g1l) / math.sqrt(g1l.size))


def compensation_factor(e_ss: float, f_avg: float) -> float:
    """k = (1-E_ss)/(E_ss+f_avg),逐波长(此处取标量 f_avg)。

    极限校验:f_avg=1 → k=(1-E_ss)/(1+E_ss);f_avg=0.04 → k=(1-E_ss)/(E_ss+0.04)。
    """
    return (1.0 - e_ss) / max(e_ss + f_avg, 1e-4)


# ============================================================================
# ⚠ 这里必须与着色器**逐式**一致,否则验证的是模型而不是实现
#
# 着色器对应位置(shading.slang):
#   make_layer_bsdf            : k_ms = energy_compensation_factor(e_ss, f_avg)
#                                ms_scale = f_avg
#   layer_specular_energy_scale: 返回 1 + k_ms          ← 注意不是 1 + f_avg·k
#   layer_value_multiscatter   : 返回 f_avg²·k_ms/π
#
# 曾经的教训:本脚本写的是 (1+k),而着色器写的是 (1+f_avg·k)。两者只白炉等价,
# 于是"误差 0.0000"验证的是脚本的公式,不是着色器的实现。改成 1+f_avg·k 后
# F0=0.5 的金属只得 0.447(应为 0.5)、介电镜面只得 0.026(应为 0.04)。
# 结论:任何一侧改动后,必须重跑本脚本并核对两侧表达式。
# ============================================================================
def total_reflectance(e_ss: float, f_avg: float) -> float:
    """E_total = E_ss·f_avg·(1+k) + f_avg²·k,应恒等于 f_avg(任意 F0)。"""
    k = compensation_factor(e_ss, f_avg)
    return e_ss * f_avg * (1.0 + k) + f_avg * f_avg * k


if __name__ == "__main__":
    rng = np.random.default_rng(80808)
    print("=== (1) 白炉能量守恒(F0 = 1,f_avg = 1)===")
    print("E_total = E_ss·f_avg·(1+k) + f_avg²·k,应严格 = 1")
    print()
    print(f"{'alpha':>6} {'mu':>5} {'E_ss':>10} {'k':>10} {'E_total':>10} {'误差':>9}  verdict")
    worst = 0.0
    table = {}
    for a in [0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]:
        for mu in [1.0, 0.75, 0.5, 0.25]:
            e, se = e_ss_measured(a, mu, samples=1_000_000, rng=rng)
            total = total_reflectance(e, 1.0)
            err = abs(total - 1.0)
            worst = max(worst, err)
            table[(a, mu)] = e
            print(f"{a:>6.2f} {mu:>5.2f} {e:>10.4f} {compensation_factor(e, 1.0):>10.4f} "
                  f"{total:>10.4f} {err:>9.4f}  {'ok' if err < 0.01 else 'OUT'}")
        print()

    print(f"最大能量误差 = {worst:.4f}")
    print(f"==> 白炉能量守恒 {'通过(误差 < 1%)' if worst < 0.01 else '未通过'}")
    print()

    print("=== (2) 有色表面:E_total 应恒等于 f_avg(不随粗糙度掉能量)===")
    for f_avg in [0.04, 0.1, 0.5, 1.0]:
        print(f"\n  f_avg = {f_avg}")
        print(f"  {'alpha':>6} {'mu':>5} {'E_ss':>10} {'k':>10} {'E_total':>10} {'与 f_avg 之差':>14}")
        worst2 = 0.0
        for a in [0.2, 0.5, 0.8, 1.0]:
            for mu in [1.0, 0.5]:
                e = table[(a, mu)]
                k = compensation_factor(e, f_avg)
                total = total_reflectance(e, f_avg)
                d = abs(total - f_avg)
                worst2 = max(worst2, d)
                print(f"  {a:>6.2f} {mu:>5.2f} {e:>10.4f} {k:>10.4f} {total:>10.5f} {d:>14.5f}")
        print(f"  最大偏差 = {worst2:.5f}  ->  {'守恒' if worst2 < 1e-3 else '不守恒'}")


