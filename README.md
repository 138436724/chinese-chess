# ChineseChess

基于 C++23 与 Vulkan 1.4 的实时 3D 中国象棋（Xiangqi）渲染演示。

- 双渲染管线：前向光栅化（MSAA）+ Vulkan 光线追踪路径追踪（NEE + 俄罗斯轮盘赌 + 时域累积）
- Slang → SPIR-V 运行时编译（SHA-256 哈希增量缓存 .spv）
- ImGui（docking + 多视口）控制面板：相机、灯光、材质/物体、棋谱回放
- 棋谱回放：ICU4C 自动检测 GB2312/UTF-8 编码，正则解析中文记谱法，WASD 逐步回放
- OpenColorIO ACES 2.0 色彩管理（CPU 截图变换 + GPU 合成 Pass 注入）
- FreeType 字体栅格化（LXGW WenKai GB 生成棋盘与棋子文字）
- 按 C 保存当前帧（EXR/PNG），Debug 构建支持 F5 热重载与 RenderDoc 条件抓帧

## 构建

- 平台：Windows x64 + Visual Studio 2022（v145+），CMake + Ninja（见 CMakePresets.json 的 x64-debug / x64-release 预设）
- 依赖：vcpkg manifest 模式（本地 vcpkg 子模块）
- 详细说明见 [README_CN.md](README_CN.md)

## 快捷键

| 按键 | 功能 |
|------|------|
| **W/A** | 棋谱上一步 |
| **S/D** | 棋谱下一步 |
| **C** | 保存当前帧截图（resources/captures/） |
| **F5** | 热重载：重新编译着色器并重建当前渲染管线 |

渲染结果如下：

![场景图像](./chinese-chess/resources/captures/2026_06_09_00_03_06.png)