"""
从 slangc 的 reflection JSON 里提取关键结构体的成员偏移与大小,
与 C++ 侧的布局假设做机器比对。

C++ 侧权威值(scene_material_manager.cpp 的 gpu_material_data /
scene_raytracing_render.cpp 的 uniform_buffer)由本脚本硬编码为期望值 ——
它们必须与 Slang 逐字节一致,否则 materials[i>=1] 会错位 → 设备丢失。
"""

from __future__ import annotations

import json
import sys


def collect_structs(node, out):
    """递归收集所有 kind == 'struct' 的类型定义。

    slangc 的 reflection JSON 把成员偏移放在 field['binding']['offset']。
    """
    if isinstance(node, dict):
        if node.get("kind") == "struct" and node.get("name"):
            fields = []
            for f in node.get("fields", []):
                b = f.get("binding", {})
                fields.append((f.get("name"), b.get("offset"), f.get("type", {})))
            out[node["name"]] = fields
        for v in node.values():
            collect_structs(v, out)
    elif isinstance(node, list):
        for v in node:
            collect_structs(v, out)


def size_of(ty) -> str:
    if not isinstance(ty, dict):
        return "?"
    k = ty.get("kind")
    if k == "array":
        return f"array[{ty.get('elementCount')}] of {ty.get('elementType', {}).get('kind')}"
    if k == "vector":
        return f"vec{ty.get('elementCount')} of {ty.get('elementType', {}).get('kind')}"
    if k == "scalar":
        return str(ty.get("scalarType"))
    if k == "struct":
        return "struct"
    if k == "resource":
        return f"resource:{ty.get('baseShape')}"
    return str(k)


def main():
    with open(sys.argv[1] if len(sys.argv) > 1 else "out_layout.json", encoding="utf-8") as fh:
        data = json.load(fh)

    structs = {}
    collect_structs(data, structs)

    # C++ 侧期望布局(与 gpu_material_data / uniform_buffer 字段顺序一致)
    EXPECT = {
        "material_data": [
            ("background_color", 0),
            ("albedo_index", 12),
            ("foreground_color", 16),
            ("roughness", 28),
            ("metallic", 32),
            ("opacity", 36),
            ("ior", 40),
            ("transmission", 44),
            ("absorption_coefficient", 48),
            ("engrave_absorption", 64),
            ("dispersion_model", 76),
            ("dispersion_param", 80),
            # 阶段 1 贴图通道
            ("normal_index", 84),
            ("orm_index", 88),
            ("emissive_index", 92),
            ("normal_scale", 96),
            ("occlusion_strength", 100),
        ],
        "rt_scene_data": [
            ("proj_inv_matrix", 0),
            ("view_inv_matrix", 64),
            ("models", 128),
            ("materials", 136),
            ("lights", 144),
            ("model_count", 152),
            ("material_count", 156),
            ("light_count", 160),
            ("texture_count", 164),
            ("skybox_index", 168),
            ("frame_index", 172),
            ("debug_flags", 176),
            ("env_intensity", 180),
            ("force_metallic", 184),
            ("force_roughness", 188),
        ],
    }

    print("=== Slang 结构体成员布局(从 SPIR-V 反射读取)===")
    ok = True
    for sname, fields in sorted(structs.items()):
        print(f"\n{sname}:")
        for fname, off, ft in fields:
            print(f"    {fname:<26} offset={off:<5} {{{size_of(ft)}}}")

    print()
    print("=== 与 C++ 侧期望偏移比对 ===")
    for sname, expected in EXPECT.items():
        if sname not in structs:
            print(f"\n{sname}: 反射中未找到(检查它是否真的被使用)")
            ok = False
            continue
        actual = {f[0]: f[1] for f in structs[sname]}
        print(f"\n{sname}:")
        for fname, off in expected:
            got = actual.get(fname)
            status = "OK" if got == off else f"MISMATCH (got {got})"
            if got != off:
                ok = False
            print(f"    {fname:<26} expect={off:<5} {status}")

    print()
    print("==> 布局校验 " + ("通过" if ok else "未通过"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
