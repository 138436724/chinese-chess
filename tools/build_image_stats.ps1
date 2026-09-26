# 构建离线图像统计工具(tools/image_stats.cpp)
#
# 为什么需要:项目在 C++ 侧链接了 OpenImageIO(vcpkg),但**系统 Python 没有
# OIIO 绑定**。要在脚本里读渲染截图(EXR, half, ZIP 压缩),直接复用 OIIO
# 比在 Python 里手写 EXR 解码可靠得多(手写版本未通过 "alpha 恒为 1" 的自校验)。
#
# 用法:pwsh tools/build_image_stats.ps1
# 产物:out/imgtools/image_stats.exe
#
# 运行前需要把 vcpkg 的 bin 加入 PATH(脚本末尾会打印现成命令)。

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"
$vcpkg = Join-Path $root "vcpkg_installed\x64-windows"
$outDir = Join-Path $root "out\imgtools"

if (-not (Test-Path $vcvars)) { Write-Error "找不到 vcvars64.bat: $vcvars" }
if (-not (Test-Path (Join-Path $vcpkg "include\OpenImageIO\imageio.h"))) { Write-Error "找不到 OIIO 头文件,检查 vcpkg_installed" }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# /utf-8 是必需的:源文件含中文字符串,而 OIIO 自带的 fmt 会静态断言
# "Unicode support requires compiling with /utf-8"(项目本身也用 /utf-8)
$cmd = "call `"$vcvars`" >nul 2>&1 && cl /nologo /std:c++20 /utf-8 /EHsc /O2 " +
       "/I `"$vcpkg\include`" `"$root\tools\image_stats.cpp`" " +
       "/link /LIBPATH:`"$vcpkg\lib`" OpenImageIO.lib OpenImageIO_Util.lib " +
       "/OUT:`"$outDir\image_stats.exe`""

cmd /c $cmd 2>&1 | Where-Object { $_ -notmatch "farmhash|warning C4" } | ForEach-Object { Write-Host "    $_" }

if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败 (exit=$LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "构建成功: $outDir\image_stats.exe" -ForegroundColor Green
Write-Host ""
Write-Host "用法(先把 vcpkg 的 bin 加入 PATH,否则找不到 OIIO 的 DLL):" -ForegroundColor Cyan
Write-Host "  `$env:PATH = `"$vcpkg\bin;`" + `$env:PATH"
Write-Host "  & `"$outDir\image_stats.exe`" chinese-chess\resources\captures\<file>.exr"
exit 0
