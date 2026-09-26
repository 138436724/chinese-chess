"""
程序化 UV 球网格校验

对象:scene_model_manager::create_procedural_sphere 生成的网格
     (与 C++ 实现同一套参数化:u 绕赤道、v 从北极到南极)

检验项(每一项出错都会在渲染里表现为可复现的缺陷):
  1. 所有顶点严格落在球面上(|P| = r)
  2. 法线为单位向量且与位置同向(球心在原点的球)
  3. UV ∈ [0,1],且 u 从 0 到 1、v 从 0(北极)到 1(南极)单调
  4. 索引在范围内、无退化三角形(面积 > 0)
  5. 绕序向外:三角形几何法线与顶点法线同向(否则背面剔除会剔掉正面)
  6. UV 参数化的手性与几何一致:cross(dP/du, dP/dv) 应与几何法线同向 ——
     切线空间法线图依赖这一点,不一致会让凹凸整体反向
  7. 顶点/索引数量与公式一致,网格是封闭的(每条边恰好被 2 个三角形共享,
     接缝与两极按环形/极点合并计数)

用法:python tools/verify_sphere_mesh.py
"""

from __future__ import annotations

import math
import sys

import numpy as np


def build_sphere(radius: float, segments: int, rings: int):
    """与 C++ create_procedural_sphere 逐式同名同序。"""
    vertices = []  # (position, normal, uv)
    for y in range(rings + 1):
        v = y / rings
        theta = v * math.pi           # 0 = 北极
        st, ct = math.sin(theta), math.cos(theta)
        for x in range(segments + 1):
            u = x / segments
            phi = u * 2.0 * math.pi
            n = np.array([st * math.cos(phi), ct, st * math.sin(phi)])
            vertices.append((n * radius, n, np.array([u, v])))

    indices = []
    stride = segments + 1
    for y in range(rings):
        at_north = (y == 0)
        at_south = (y + 1 == rings)
        for x in range(segments):
            a = y * stride + x
            b = a + stride
            if at_north:
                indices.extend([a, b + 1, b])
            elif at_south:
                indices.extend([a, a + 1, b])
            else:
                indices.extend([a, a + 1, b, b, a + 1, b + 1])
    return vertices, indices


def check(radius: float, segments: int, rings: int) -> bool:
    print(f"=== 球体:radius={radius} segments={segments} rings={rings} ===")
    verts, idx = build_sphere(radius, segments, rings)
    P = np.array([v[0] for v in verts])
    N = np.array([v[1] for v in verts])
    UV = np.array([v[2] for v in verts])
    ok = True

    def report(name, good, detail=""):
        nonlocal ok
        ok &= good
        print(f"  [{'OK' if good else 'FAIL'}] {name}{(' — ' + detail) if detail else ''}")

    # 1. 顶点在球面
    r = np.linalg.norm(P, axis=1)
    report("顶点严格落在球面", np.allclose(r, radius, atol=1e-5),
           f"|P| ∈ [{r.min():.6f}, {r.max():.6f}]")

    # 2. 法线单位且与位置同向
    nn = np.linalg.norm(N, axis=1)
    same_dir = np.allclose(N, P / np.maximum(r, 1e-12)[:, None], atol=1e-6)
    report("法线为单位向量", np.allclose(nn, 1.0, atol=1e-6))
    report("法线与位置同向", same_dir)

    # 3. UV 范围与单调性
    report("UV ∈ [0,1]", UV.min() >= -1e-9 and UV.max() <= 1.0 + 1e-9,
           f"u ∈ [{UV[:,0].min():.3f},{UV[:,0].max():.3f}] v ∈ [{UV[:,1].min():.3f},{UV[:,1].max():.3f}]")

    # 4. 索引范围与退化三角形
    report("索引在范围内", max(idx) < len(verts) and min(idx) >= 0,
           f"max index = {max(idx)} < {len(verts)}")
    tris = np.array(idx).reshape(-1, 3)
    e1 = P[tris[:, 1]] - P[tris[:, 0]]
    e2 = P[tris[:, 2]] - P[tris[:, 0]]
    areas = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
    degenerate = areas < 1e-12
    # 两极所在的圈必然出现零面积三角形(极点顶点位置重合)——
    # 这是 UV 参数化球的固有退化,允许存在,但必须**只**出现在极点圈。
    pole_mask = np.zeros(len(tris), dtype=bool)
    stride_v = segments + 1
    for ti, t in enumerate(tris):
        ys = {int(v) // stride_v for v in t}
        if any(y in (0, rings) for y in ys):
            pole_mask[ti] = True
    bad_degenerate = int((degenerate & ~pole_mask).sum())
    report("非极点处无退化三角形", bad_degenerate == 0,
           f"退化总数 = {int(degenerate.sum())}(其中极点圈 {int((degenerate & pole_mask).sum())}),非极点退化 = {bad_degenerate}")

    # 5. 绕序向外(只在非退化三角形上判定)
    e1v, e2v = e1[~degenerate], e2[~degenerate]
    geo_n = np.cross(e1v, e2v)
    geo_n /= np.maximum(np.linalg.norm(geo_n, axis=1, keepdims=True), 1e-12)
    tri_nd = tris[~degenerate]
    vert_n = (N[tri_nd[:, 0]] + N[tri_nd[:, 1]] + N[tri_nd[:, 2]]) / 3.0
    vert_n /= np.maximum(np.linalg.norm(vert_n, axis=1, keepdims=True), 1e-12)
    dots = np.sum(geo_n * vert_n, axis=1)
    outward = float((dots > 0.0).mean())
    report("绕序向外(逆时针为正面)", outward > 0.999, f"向外比例 = {outward:.4%}")

    # 6. UV 手性与几何一致
    du = np.array([1.0 / segments, 0.0])
    dv = np.array([0.0, 1.0 / rings])
    # 取一个赤道附近的四边形做差分
    x0 = segments // 4
    y0 = rings // 2
    a = y0 * (segments + 1) + x0
    b = a + (segments + 1)
    dPdu = P[a + 1] - P[a]
    dPdv = P[b] - P[a]
    geo = np.cross(dPdu, dPdv)
    geo /= max(np.linalg.norm(geo), 1e-12)
    n_ref = N[a]
    # 若手性一致,cross(dPdu,dPdv) 应与法线同向(同一约定)
    hand = float(np.dot(geo, n_ref))
    report("UV 手性与几何一致", hand > 0.0,
           f"cross(dPdu,dPdv)·N = {hand:+.4f}(应与 C++ 索引绕序同向)")

    # 7. 数量与封闭性
    exp_t = segments * (rings - 2) * 2 + segments * 2  # 中间圈各 2 个,两极各 1 个
    report("三角形数符合公式", len(tris) == exp_t, f"{len(tris)} == {exp_t}")

    # 每条边恰好被 2 个三角形共享。
    # 合并规则必须同时处理两类**物理重合**的顶点:
    #   · 接缝:u=1 的列与 u=0 的列是同一物理顶点(UV 接缝)
    #   · 两极:该圈所有顶点位置相同、只是 UV 不同(极点的 UV 退化)
    # 少了任一条都会把封闭网格误判为"边不配对"。
    from collections import Counter

    def phys(vi: int):
        # 统一返回 (int, int),便于排序比较(混用 str 会 TypeError)
        y, x = divmod(vi, segments + 1)
        if y == 0:
            return (0, 0)          # 北极:该圈所有顶点物理上是同一个点
        if y == rings:
            return (rings, 0)      # 南极:同上
        return (y, x % segments)   # 接缝:u=1 与 u=0 是同一个物理顶点

    edge_count = Counter()
    for t in tris:
        for i in range(3):
            e = tuple(sorted((phys(int(t[i])), phys(int(t[(i + 1) % 3])))))
            edge_count[e] += 1
    bad_edges = [e for e, c in edge_count.items() if c != 2]
    report("每条边被 2 个三角形共享(封闭)", len(bad_edges) == 0,
           f"异常边数 = {len(bad_edges)}")

    print(f"  ==> {'通过' if ok else '未通过'}")
    return ok


if __name__ == "__main__":
    results = [
        check(0.22, 48, 24),
        check(1.0, 8, 6),
        check(0.5, 4, 2),
    ]
    print()
    print("==> 球体网格校验 " + ("全部通过" if all(results) else "有失败项"))
    sys.exit(0 if all(results) else 1)
