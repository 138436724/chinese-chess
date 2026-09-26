"""
EXR 参考对比工具 —— 阶段 0 的回归框架(见 pbr-exploration-plan.md)

用途:把渲染器导出的 EXR(按 F 键截图,scene_manager 走 OCIO + OIIO 异步写盘)
与一份"已知可接受"的参考 EXR 做量化对比,让每个阶段的改动都有可复现的回归证据。

设计要点:
  · 只依赖 numpy + OpenImageIO(项目已在用);无 OIIO 时对 .npy 也能工作(便于自测)。
  · 报告**相对亮度尺度**(均值/中位数/分位数)与**逐像素差异直方图**,
    而不是只看"看起来对不对"。
  · 对 PBR 能量实验特别有用的量:平均亮度、亮度分位数、过曝/欠曝像素比例。
  · 支持忽略 alpha 通道,自动处理 (H,W,3) / (H,W,4) / 单通道。

典型用法:
    python tools/compare_exr.py render.exr reference.exr
    python tools/compare_exr.py render.exr reference.exr --mask 0.98   # 只比亮部
    python tools/compare_exr.py --selftest                              # 无资产自测

回归判据(建议):
    · 平均亮度相对差 < 2%(能量守恒改动应落在这个量级)
    · 亮度分位数曲线整体偏移 < 2%
    · 逐像素差异的分位分布无长尾(有长尾说明局部物理错误,而非整体能量偏移)
"""

from __future__ import annotations

import argparse
import sys

import numpy as np


def load_image(path: str) -> np.ndarray:
    """读取 EXR/PNG(经 OIIO)或 .npy。返回 (H,W,C) float32,线性空间。"""
    if path.endswith(".npy"):
        return np.load(path).astype(np.float32)

    try:
        import OpenImageIO as oiio
    except ImportError:
        raise SystemExit(
            "缺少 OpenImageIO。二选一:\n"
            "  · 用 vcpkg_installed 里的 python 绑定,或\n"
            "  · 先把图像转成 .npy 再对比(便于无 OIIO 环境自测)"
        ) from None

    inp = oiio.ImageInput.open(path)
    if not inp:
        raise SystemExit(f"无法打开图像: {path}")
    spec = inp.spec()
    pixels = inp.read_image(format=oiio.FLOAT)
    inp.close()

    arr = np.asarray(pixels, dtype=np.float32).reshape(spec.height, spec.width, spec.nchannels)
    return arr


def luminance(rgb: np.ndarray) -> np.ndarray:
    """BT.709 亮度。输入 (H,W,C>=3)。"""
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def prepare(arr: np.ndarray) -> np.ndarray:
    """统一成 (H,W,3) 线性 RGB;丢弃 alpha。"""
    if arr.ndim == 2:
        arr = arr[..., None]
    if arr.shape[-1] == 1:
        arr = np.repeat(arr, 3, axis=-1)
    return arr[..., :3]


def describe(name: str, lum: np.ndarray) -> dict:
    finite = lum[np.isfinite(lum)]
    qs = np.percentile(finite, [1, 5, 25, 50, 75, 95, 99])
    return {
        "name": name,
        "mean": float(finite.mean()),
        "median": float(qs[3]),
        "p01": float(qs[0]),
        "p05": float(qs[1]),
        "p25": float(qs[2]),
        "p75": float(qs[4]),
        "p95": float(qs[5]),
        "p99": float(qs[6]),
        "over": float((finite > 1.0).mean()),   # 过曝像素比例(线性空间 > 1)
        "under": float((finite < 1e-4).mean()),  # 近黑像素比例
    }


def print_stats(s: dict):
    print(f"  {s['name']}:")
    print(f"      mean={s['mean']:.5f}  median={s['median']:.5f}")
    print(f"      p01={s['p01']:.5f} p05={s['p05']:.5f} p25={s['p25']:.5f} "
          f"p75={s['p75']:.5f} p95={s['p95']:.5f} p99={s['p99']:.5f}")
    print(f"      过曝(>1)比例={s['over']:.4%}  近黑(<1e-4)比例={s['under']:.4%}")


def compare(a_path: str, b_path: str, mask_threshold: float | None = None) -> int:
    a = prepare(load_image(a_path))
    b = prepare(load_image(b_path))

    if a.shape != b.shape:
        print(f"[警告] 尺寸不一致: {a.shape} vs {b.shape} —— 取公共区域比较")
        h = min(a.shape[0], b.shape[0])
        w = min(a.shape[1], b.shape[1])
        a, b = a[:h, :w], b[:h, :w]

    la, lb = luminance(a), luminance(b)
    mask = np.ones_like(la, dtype=bool)
    if mask_threshold is not None:
        # 只在参考图的亮部比较(暗部相对噪声大,信噪比低)
        mask = lb >= mask_threshold
        if not mask.any():
            print(f"[错误] mask 阈值 {mask_threshold} 下没有任何像素,放宽阈值重试")
            return 1

    print("=== 亮度统计(线性空间)===")
    print_stats(describe("A(待测)", la[mask]))
    print_stats(describe("B(参考)", lb[mask]))

    mean_a = float(la[mask].mean())
    mean_b = float(lb[mask].mean())
    rel = (mean_a - mean_b) / max(abs(mean_b), 1e-9)

    diff = (la[mask] - lb[mask])
    absdiff = np.abs(diff)
    rel_pixel = absdiff / np.maximum(np.abs(lb[mask]), 1e-6)

    print()
    print("=== 差异 ===")
    print(f"  平均亮度相对差 = {rel:+.4%}   (判据: |rel| < 2%)")
    print(f"  逐像素绝对差: mean={absdiff.mean():.5f}  p50={np.percentile(absdiff, 50):.5f} "
          f"p95={np.percentile(absdiff, 95):.5f}  p99={np.percentile(absdiff, 99):.5f}  "
          f"max={absdiff.max():.5f}")
    print(f"  逐像素相对差: p50={np.percentile(rel_pixel, 50):.2%} "
          f"p95={np.percentile(rel_pixel, 95):.2%} p99={np.percentile(rel_pixel, 99):.2%}")

    # 长尾判据:差异是否集中在少数像素上(局部物理错误)还是整体平移(能量偏移)。
    # 用绝对差的分位数比 —— 相对差在参考值接近 0 时会爆炸、在完全相同时 p50=0 退化。
    a50 = float(np.percentile(absdiff, 50))
    a99 = float(np.percentile(absdiff, 99))
    a_mean = float(absdiff.mean())
    tail = a99 / max(a_mean, 1e-9)
    print(f"  绝对差长尾(p99/mean) = {tail:.2f}   "
          f"(≈1 表示整体平移;远大于 ~5 表示差异集中在少数像素 → 局部物理问题)")

    print()
    ok = abs(rel) < 0.02
    print(f"==> 整体能量判据 {'通过(|相对差| < 2%)' if ok else '未通过'}")
    if ok and tail > 5.0:
        print("    注意:整体能量通过但长尾明显 —— 差异集中在少数像素,"
              "建议用 --mask 或差异图定位局部错误(几何/材质/阴影),而非调全局能量")
    return 0 if ok else 1


def selftest() -> int:
    """无资产自测:构造已知差异的两张图,验证统计与判据行为正确。"""
    print("=== 自测:构造已知差异的图像 ===")
    rng = np.random.default_rng(7)
    h, w = 64, 96
    ref = rng.random((h, w, 3), dtype=np.float32) * 0.8 + 0.1

    cases = {
        "完全相同": ref.copy(),
        "整体亮度 +1%(应通过)": ref * 1.01,
        "整体亮度 +10%(应不通过)": ref * 1.10,
        "局部亮斑 +0.5(长尾,整体应通过)": None,
    }
    spike = ref.copy()
    spike[10:14, 10:14] += 0.5
    cases["局部亮斑 +0.5(长尾,整体应通过)"] = spike

    failures = 0
    for name, img in cases.items():
        np.save("_cmp_ref.npy", ref)
        np.save("_cmp_test.npy", img)
        print(f"\n---- 用例: {name} ----")
        rc = compare("_cmp_test.npy", "_cmp_ref.npy")
        expected_fail = "不通过" in name or "+10%" in name
        if expected_fail and rc == 0:
            print("  [自测失败] 预期不通过却通过了")
            failures += 1
        elif not expected_fail and rc != 0:
            print("  [自测失败] 预期通过却未通过")
            failures += 1
        else:
            print("  [自测 OK]")

    import os
    for f in ("_cmp_ref.npy", "_cmp_test.npy"):
        if os.path.exists(f):
            os.remove(f)

    print()
    print("==> 自测 " + ("全部通过" if failures == 0 else f"有 {failures} 项失败"))
    return 0 if failures == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="EXR 参考对比(阶段 0 回归框架)")
    ap.add_argument("test", nargs="?", help="待测图像(.exr/.png/.npy)")
    ap.add_argument("reference", nargs="?", help="参考图像(.exr/.png/.npy)")
    ap.add_argument("--mask", type=float, default=None,
                    help="只在参考亮度 >= 该值的像素上比较(例如 0.98 只看亮部)")
    ap.add_argument("--selftest", action="store_true", help="无资产自测")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.test or not args.reference:
        ap.print_help()
        return 2
    return compare(args.test, args.reference, args.mask)


if __name__ == "__main__":
    sys.exit(main())
