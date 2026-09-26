# 着色器与布局离线校验
#
# 用途:在**不需要 CMake/vcpkg install/完整构建**的前提下,用 vcpkg 自带的 slangc
# 验证所有着色器能编译,并从 SPIR-V 反射里核对 CPU/GPU 结构体布局。
#
# 这解决了本仓库的一个真实痛点:完整构建受 vcpkg 的 git 依赖与 Ninja PATH 阻塞,
# 但着色器语法与 GPU 结构体布局(设备丢失风险)其实可以离线验证。
#
# 用法:pwsh tools/verify_shaders.ps1
#
# 退出码 0 = 全部通过。

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$shaders = Join-Path $root "chinese-chess\resources\shaders"
$slangc = Join-Path $root "vcpkg_installed\x64-windows\tools\shader-slang\slangc.exe"
$outDir = Join-Path $root "out\shader_check"

if (-not (Test-Path $slangc)) {
    Write-Error "slangc 未找到: $slangc(先执行 vcpkg install / 或放宽网络限制完成依赖安装)"
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# 与 shader_compiler.cpp 的编译选项保持一致,否则校验没有意义:
#   format = SLANG_SPIRV, profile = spirv_1_6, matrix-layout-column-major,优化级别 maximal
$common = @("-target", "spirv", "-profile", "spirv_1_6", "-matrix-layout-column-major", "-O3")

$failed = @()

function Invoke-Slangc {
    # 参数名不能用 $Args —— 那是 PowerShell 的自动变量,会发生绑定冲突
    param([string]$Name, [string[]]$SlangArgs)
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    & $slangc @SlangArgs 2>&1 | ForEach-Object { Write-Host "    $_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "    [FAIL] exit=$LASTEXITCODE" -ForegroundColor Red
        $script:failed += $Name
    } else {
        Write-Host "    [OK]" -ForegroundColor Green
    }
}

# ---- 入口着色器(每个 entry point 都要显式声明 stage,与 create_from_shader 一致)----
Invoke-Slangc "ray_tracing.slang" ($common + @(
    "-entry", "rayGenMain", "-stage", "raygeneration",
    "-entry", "rayMissMain", "-stage", "miss",
    "-entry", "rayShadowMissMain", "-stage", "miss",
    "-entry", "rayClosestHitMain", "-stage", "closesthit",
    "-entry", "rayShadowAnyHitMain", "-stage", "anyhit",
    "-o", (Join-Path $outDir "ray_tracing.spv"),
    (Join-Path $shaders "ray_tracing.slang")))

Invoke-Slangc "rasterization.slang" ($common + @(
    "-entry", "vertMain", "-stage", "vertex",
    "-entry", "fragMain", "-stage", "fragment",
    "-o", (Join-Path $outDir "rasterization.spv"),
    (Join-Path $shaders "rasterization.slang")))

Invoke-Slangc "blend_image.slang" ($common + @(
    "-entry", "vertMain", "-stage", "vertex",
    "-entry", "fragMain", "-stage", "fragment",
    "-o", (Join-Path $outDir "blend_image.spv"),
    (Join-Path $shaders "blend_image.slang")))

# energy_compensation.slang 是**模块**(只有常量表、无入口点),单独编译会报
# "SPIR-V output contains no exported symbols" —— 那是正常的。
# 它的语法已由 ray_tracing.slang 的 import 链覆盖(上面第一步),这里只检查文件存在,
# 避免"表没生成就跑验证"这种假通过。
$ecModule = Join-Path $shaders "energy_compensation.slang"
if (Test-Path $ecModule) {
    $lines = (Get-Content $ecModule).Count
    Write-Host "=== energy_compensation.slang(模块,由 import 链覆盖)===" -ForegroundColor Cyan
    Write-Host "    [OK] 存在,$lines 行(由 tools/pbr_gen_energy_table.py 生成)" -ForegroundColor Green
} else {
    Write-Host "=== energy_compensation.slang 缺失 ===" -ForegroundColor Red
    Write-Host "    先运行:python tools/pbr_gen_energy_table.py"
    $failed += "energy_compensation(缺失)"
}

# ---- GPU 结构体布局校验(设备丢失风险的唯一可靠检查)----
# 注意:-reflection-json 必须出现在 -o **之前**,否则 slangc 不生成反射文件
$reflect = Join-Path $outDir "layout.json"
if (Test-Path $reflect) { Remove-Item $reflect -Force }
Invoke-Slangc "layout_probe(反射)" ($common + @(
    "-O0", "-entry", "probe", "-stage", "compute",
    "-I", $shaders,
    "-reflection-json", $reflect,
    "-o", (Join-Path $outDir "layout_probe.spv"),
    (Join-Path $root "tools\layout_probe.slang")))

Write-Host "=== 结构体布局比对 ===" -ForegroundColor Cyan
& python (Join-Path $root "tools\verify_layout.py") $reflect
if ($LASTEXITCODE -ne 0) { $failed += "layout" }

Write-Host ""
if ($failed.Count -gt 0) {
    Write-Host "失败项: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "全部通过:着色器编译 + GPU 结构体布局" -ForegroundColor Green
exit 0
