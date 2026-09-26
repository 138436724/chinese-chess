# 白炉球体阵探针 —— 全自动能量验收
#
# 做什么:
#   1. 启动应用(需要它已按当前代码构建好)
#   2. PostMessage 投递按键(不抢焦点):
#        G → 显示白炉球体阵(并隐藏棋盘棋子)
#        H → 白炉预设(关全部启发式钳制 + 反照率强制为白,保留每格 roughness/metallic)
#        C → 截图(后台 jthread 异步写盘到 resources/captures/)
#   3. 用 tools/image_stats.exe 在**每个球心**取圆盘均值
#
# 判据(能量守恒的像素级证据):
#   metallic=1 那一行:反照率强制为白 ⟹ F0=1 ⟹ 多次散射补偿下
#   **亮度应随 roughness 基本不变**(未补偿时高粗糙度会暗 30%~40%)。
#   metallic=0 那一行:介电(白反照率)以漫反射为主,应整体偏亮。
#
# 用法:pwsh tools/furnace_probe.ps1 [-Exe <path>] [-WarmupSeconds N] [-AccumulateSeconds N]

param(
    [string]$Exe = "out\Debug\chinese-chess.exe",
    [int]$WarmupSeconds = 12,
    [int]$AccumulateSeconds = 12,
    [int]$ImageWidth = 800,
    [int]$ImageHeight = 600
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# ---- 球心像素坐标 ----
# 与 ui_record::rebuild_furnace_grid 的布局一致:
#   6 列(roughness 0.05→1.0)沿 x 铺开,5 行(metallic 0→1)沿 y 铺开,
#   半径 0.22、间距 0.62、z = -2.0;相机在原点朝 -Z,FOV_y = 90°。
# 投影:z=2.0 处可视半高 = 2.0·tan(45°) = 2.0,半宽 = 2.0·aspect
$cols = 6; $rows = 5; $spacing = 0.85; $gridZ = -2.0
$aspect = $ImageWidth / $ImageHeight
$halfH = [Math]::Abs($gridZ) * [Math]::Tan([Math]::PI / 4)
$halfW = $halfH * $aspect

$dotParts = @()
$centers = @()
for ($r = 0; $r -lt $rows; $r++) {
    $y = ($r - ($rows - 1) / 2.0) * $spacing
    for ($c = 0; $c -lt $cols; $c++) {
        $x = ($c - ($cols - 1) / 2.0) * $spacing
        $ndcX = $x / $halfW
        $ndcY = $y / $halfH
        $px = [int][Math]::Round(($ndcX + 1) * 0.5 * $ImageWidth)
        $py = [int][Math]::Round((1 - $ndcY) * 0.5 * $ImageHeight)
        $dotParts += "$px,$py"
        $centers += [pscustomobject]@{ Row = $r; Col = $c; Metallic = $r / ($rows - 1); Roughness = 0.05 + 0.95 * $c / ($cols - 1); Px = $px; Py = $py }
    }
}
$dotsArg = ($dotParts -join ",")

Write-Host "球心像素坐标(共 $($centers.Count) 个),示例前 3 个:" -ForegroundColor Cyan
$centers | Select-Object -First 3 | Format-Table -AutoSize

# ---- PostMessage(不抢焦点)----
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FurnaceW32 {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@

function Send-Key([IntPtr]$hwnd, [int]$vk, [int]$scancode) {
    # WM_KEYDOWN / WM_KEYUP;lParam 低 16 位为重复次数,16-23 位为扫描码
    $down = [IntPtr](1 -bor ($scancode -shl 16))
    $up   = [IntPtr](0xC0000001 -bor ($scancode -shl 16))
    [void][FurnaceW32]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, $down)
    [void][FurnaceW32]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, $up)
}

$exePath = Join-Path $root $Exe
if (-not (Test-Path $exePath)) { Write-Error "找不到可执行文件: $exePath" }

$before = @(Get-ChildItem (Join-Path $root "chinese-chess\resources\captures") -Filter "*.exr" -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Name)

$p = Start-Process -FilePath $exePath -WorkingDirectory (Join-Path $root "chinese-chess") -PassThru
Write-Host "已启动 PID=$($p.Id),等待 $WarmupSeconds 秒初始化..." -ForegroundColor Cyan
Start-Sleep -Seconds $WarmupSeconds

$q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if (-not $q) { Write-Error "进程提前退出" }
$hwnd = $q.MainWindowHandle
Write-Host "hwnd=$hwnd  title=$($q.MainWindowTitle)"

Write-Host "投递 G(球体阵)..." -ForegroundColor Cyan
Send-Key $hwnd 0x47 0x22   # 'G'
Start-Sleep -Seconds 3

Write-Host "投递 H(白炉预设)..." -ForegroundColor Cyan
Send-Key $hwnd 0x48 0x23   # 'H'
Write-Host "累积 $AccumulateSeconds 秒..." -ForegroundColor Cyan
Start-Sleep -Seconds $AccumulateSeconds

$q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if ($q) { Write-Host "累积后 FPS: $($q.MainWindowTitle)" }

Write-Host "投递 C(截图)..." -ForegroundColor Cyan
Send-Key $hwnd 0x43 0x2E   # 'C'
Start-Sleep -Seconds 8

if (-not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(15000)) { $p.Kill() } }
Write-Host "已关闭" -ForegroundColor Cyan

# ---- 找出新截图并读数 ----
$capsDir = Join-Path $root "chinese-chess\resources\captures"
$new = Get-ChildItem $capsDir -Filter "*.exr" | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime
if (-not $new) { Write-Error "没有产生新截图(热键是否生效?)" }
$img = $new[-1].FullName
Write-Host "新截图: $(Split-Path $img -Leaf)" -ForegroundColor Green

$env:PATH = (Join-Path $root "vcpkg_installed\x64-windows\bin") + ";" + $env:PATH
$stats = Join-Path $root "out\imgtools\image_stats.exe"
Write-Host ""
Write-Host "=== 逐球读数(按行输出:每 6 个为一行,行 = metallic 0→1,列 = roughness 0.05→1.0)===" -ForegroundColor Cyan
& $stats $img --dots $dotsArg --dot-radius 10
