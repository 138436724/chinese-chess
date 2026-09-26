"""
生成多次散射能量补偿表(Kulla-Conty 路线)

数学依据(逐步可验,不含任何记忆中的拟合系数):

  单散射 GGX(VNDF 采样,pdf = D·G1(V)/(4·NdotV))的估计量存在解析恒等式:
      f_spec·cosθ_L / p(L) = G1(V)·G1(L)
  证明:f_spec = D·G·F/(4·NdotV·NdotL),取 F = 1、G = G1(V)G1(L),
       ⟹ f·cos/p = [D·G1(V)G1(L)/(4·NdotV)] · [4·NdotV/(D·G1(V))] = G1(L)
  因此
      E_ss(mu_v, alpha) = E_{L~p}[ G1(L) ]        (F0 = 1 时)
  这个式子的自洽性检验非常强:
      alpha -> 0 时 G1 ≡ 1  ⟹ E_ss ≡ 1(完美镜面,无损失)
      mu_v = 1  时 E_ss = E[G1(cosθ_L)] 必须单调递减于 alpha

  平均菲涅尔(同样用同一批样本):
      F_avg = E[F(V·h)] / E[1] 在 h 的 VNDF 分布下按 (V·h) 权重平均

输出:
  · energy_compensation_table.h  —— 可直接嵌进 shading.slang 的 E_ss / F_avg 表
  · 控制台上的自洽性检验结果

用法:python tools/pbr_gen_energy_table.py
"""

from __future__ import annotations

import math

import numpy as np

import pbr_validation as pv

# 表分辨率:粗糙度 32 档(按感知粗糙度线性),视角 16 档(按 mu_v 线性)
N_ROUGH = 32
N_ANGLE = 16


def e_ss_and_favg(roughness: float, mu_v: float, samples: int, rng, f0: float = 0.04):
    """用解析恒等式 E_ss = E[G1(L)] 估计(与 shader 的 VNDF 采样同源)。

    注意两个 f0 的区别:
      · E_ss 是在 **F0 = 1**(白炉)下测的方向反射率,与 f0 无关;
      · F_avg 是给定 f0 下的平均菲涅尔,默认取电介质的 0.04。
    早期把 f0 默认写成 1.0,导致 F_avg 整表恒为 1.0。
    """
    st = math.sqrt(max(0.0, 1.0 - mu_v * mu_v))
    ve = np.tile(np.array([st, 0.0, mu_v]), (samples, 1))
    l = pv.sample_ggx_vndf(ve, roughness, rng)

    ndotl = l[:, 2]
    ok = ndotl > 1e-6
    if not ok.any():
        return 0.0, f0
    l, ndotl = l[ok], ndotl[ok]
    ve_ok = ve[ok]

    g1l = pv.g1_smith_ggx(ndotl, roughness)
    e_ss = float(np.mean(g1l))

    # F_avg:在 VNDF 的 h 分布上按 (V·h) 权重平均 F(V·h)
    # 注意:局部变量不能叫 f —— 会遮蔽入参 f0(踩过一次:第二次调用起 f0 变成数组,
    # 结果恒为 1.0)
    h = ve_ok + l
    h /= np.linalg.norm(h, axis=1, keepdims=True)
    vdoth = np.maximum(h @ np.array([st, 0.0, mu_v]), 0.0)
    fres = pv.f_schlick_f90(np.full_like(vdoth, f0), 1.0, vdoth)
    w = np.maximum(vdoth, 1e-9)
    f_avg = float(np.sum(fres * w) / np.sum(w))

    return e_ss, f_avg


def build_table(samples: int = 800_000, seed: int = 606):
    rng = np.random.default_rng(seed)
    roughs = np.linspace(0.0, 1.0, N_ROUGH)
    angles = np.linspace(0.0, 1.0, N_ANGLE)

    e_ss = np.zeros((N_ROUGH, N_ANGLE))
    f_avg = np.zeros((N_ROUGH, N_ANGLE))

    for i, r in enumerate(roughs):
        rr = max(r, 0.02)
        for j, mu in enumerate(angles):
            mm = max(mu, 0.02)
            e, fa = e_ss_and_favg(rr, mm, samples, rng)
            e_ss[i, j] = e
            f_avg[i, j] = fa

    return roughs, angles, e_ss, f_avg


def self_check(roughs, angles, e_ss):
    print("=== 自洽性检验 ===")
    ok = True

    # 检验 1:alpha -> 0 时 E_ss -> 1
    v = e_ss[0, :].mean()
    print(f"  1) roughness=0 时 E_ss 均值 = {v:.4f}(应≈1)")
    ok &= abs(v - 1.0) < 0.02

    # 检验 2:mu_v = 1 时随 alpha 单调递减
    col = e_ss[:, -1]
    mono = np.all(np.diff(col) <= 1e-6)
    print(f"  2) mu_v=1 时 E_ss 随 roughness 单调递减:{mono}(最小值 {col.min():.4f})")
    ok &= bool(mono)

    # 检验 3:能量损失量级应落在文献区间(roughness=1 时约 30%~45%)
    loss = 1.0 - e_ss[-1, -1]
    print(f"  3) roughness=1, mu=1 时能量损失 = {loss:.1%}(文献量级 30%~45%)")
    ok &= 0.25 < loss < 0.50

    print(f"  ==> 自洽性 {'通过' if ok else '未通过'}")
    return ok


def emit_header(roughs, angles, e_ss, f_avg) -> str:
    lines = []
    lines.append("// 本文件由 tools/pbr_gen_energy_table.py 自动生成,请勿手改。")
    lines.append("//")
    lines.append("// 多次散射能量补偿表(Kulla-Conty 路线):")
    lines.append("//   EC_E_SS[ri][ai] = 单散射 GGX 在 (roughness, mu_v) 下的方向反射率(F0=1)")
    lines.append("//   EC_F_AVG[ri][ai] = 对应视角下的平均菲涅尔(余弦加权)")
    lines.append("// 生成依据的解析恒等式:E_ss = E_{L~VNDF}[G1(L)],自洽性检验见生成脚本输出。")
    lines.append("//")
    lines.append("// 注意:该模块必须放在 resources/shaders/ 下 —— Slang 的搜索路径是")
    lines.append("// 入口 .slang 的所在目录(shader_compiler.cpp 设置 searchPaths = 父目录)。")
    lines.append("module energy_compensation;")
    lines.append("")
    lines.append(f"public static const uint EC_ROUGH_COUNT = {N_ROUGH};")
    lines.append(f"public static const uint EC_ANGLE_COUNT = {N_ANGLE};")
    lines.append("")
    lines.append("public static const float EC_E_SS[%d][%d] = {" % (N_ROUGH, N_ANGLE))
    for i in range(N_ROUGH):
        row = ", ".join(f"{e_ss[i, j]:.5f}" for j in range(N_ANGLE))
        lines.append("  { " + row + " },")
    lines.append("};")
    lines.append("")
    lines.append("public static const float EC_F_AVG[%d][%d] = {" % (N_ROUGH, N_ANGLE))
    for i in range(N_ROUGH):
        row = ", ".join(f"{f_avg[i, j]:.5f}" for j in range(N_ANGLE))
        lines.append("  { " + row + " },")
    lines.append("};")
    return "\n".join(lines)


if __name__ == "__main__":
    print(f"生成能量补偿表:{N_ROUGH} 粗糙度 x {N_ANGLE} 视角 ...")
    roughs, angles, e_ss, f_avg = build_table()
    ok = self_check(roughs, angles, e_ss)

    header = emit_header(roughs, angles, e_ss, f_avg)
    out = "chinese-chess/resources/shaders/energy_compensation.slang"
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(header + "\n")
    print(f"已写出 {out}({len(header)} 字节)")
    print(f"总体结论:{'表可用于着色器' if ok else '表不可用,需先修验证工具'}")
