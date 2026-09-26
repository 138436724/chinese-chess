# 反证测试:白炉预设 + 关闭环境光 ⟹ 应当没有任何光源,球必须为纯黑
#
# 用途:验证"白炉预设的调试标志真的被应用了"。
# 如果球不黑,说明 uniform_environment / disable_direct_lights 等标志没生效
# (例如 C++ 侧未重建、或热键没送达),此时白炉读数不能作为证据。

param(
    [string]$Exe = "out\Release\chinese-chess.exe",
    [int]$WarmupSeconds = 10,
    [int]$AccumulateSeconds = 8,
    [int]$ImageWidth = 800,
    [int]$ImageHeight = 600
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$cols = 6; $rows = 5; $spacing = 0.85; $gridZ = -2.0
$aspect = $ImageWidth / $ImageHeight
$halfH = [Math]::Abs($gridZ) * [Math]::Tan([Math]::PI / 4)
$halfW = $halfH * $aspect
$dotParts = @()
for ($r = 0; $r -lt $rows; $r++) {
    for ($c = 0; $c -lt $cols; $c++) {
        $x = ($c - ($cols - 1) / 2.0) * $spacing
        $y = ($r - ($rows - 1) / 2.0) * $spacing
        $px = [int][Math]::Round(($x / $halfW + 1) * 0.5 * $ImageWidth)
        $py = [int][Math]::Round((1 - ($y / $halfH)) * 0.5 * $ImageHeight)
        $dotParts += "$px,$py"
    }
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class DarkW32 {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@
function Send-Key([IntPtr]$hwnd, [int]$vk, [int]$scancode) {
    $down = [IntPtr](1 -bor ($scancode -shl 16))
    $up   = [IntPtr](0xC0000001 -bor ($scancode -shl 16))
    [void][DarkW32]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, $down)
    [void][DarkW32]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, $up)
}

$exePath = Join-Path $root $Exe
$capsDir = Join-Path $root "chinese-chess\resources\captures"
$before = @(Get-ChildItem $capsDir -Filter "*.exr" | Select-Object -ExpandProperty Name)

$p = Start-Process -FilePath $exePath -WorkingDirectory (Join-Path $root "chinese-chess") -PassThru
Write-Host "已启动 PID=$($p.Id)" -ForegroundColor Cyan
Start-Sleep -Seconds $WarmupSeconds
$q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if (-not $q) { Write-Error "进程提前退出" }
$hwnd = $q.MainWindowHandle

Write-Host "G(球体阵) / H(白炉预设) / J(关环境光)..." -ForegroundColor Cyan
Send-Key $hwnd 0x47 0x22
Start-Sleep -Seconds 2
Send-Key $hwnd 0x48 0x23
Start-Sleep -Seconds 2
Send-Key $hwnd 0x4A 0x24
Write-Host "累积 $AccumulateSeconds 秒(此时应当无任何光源)..." -ForegroundColor Cyan
Start-Sleep -Seconds $AccumulateSeconds

Write-Host "C(截图)..." -ForegroundColor Cyan
Send-Key $hwnd 0x43 0x2E
Start-Sleep -Seconds 8
if (-not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(15000)) { $p.Kill() } }

$new = Get-ChildItem $capsDir -Filter "*.exr" | Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime
if (-not $new) { Write-Error "没有产生新截图" }
$img = $new[-1].FullName
Write-Host "截图: $(Split-Path $img -Leaf)" -ForegroundColor Green

$env:PATH = (Join-Path $root "vcpkg_installed\x64-windows\bin") + ";" + $env:PATH
& (Join-Path $root "out\imgtools\image_stats.exe") $img --dots ($dotParts -join ",") --dot-radius 10 |
    Select-String -Pattern 'FILE|SIZE|LUMINANCE|FRACTION|^\s+\d+\s+\d+\s+[0-9.]+'

Write-Host ""
Write-Host "判据:若白炉预设 + 关环境光真的生效,球的亮度应 **接近 0**;" -ForegroundColor Yellow
Write-Host "      若仍在 0.3~0.6,说明调试标志没有被应用,之前的白炉读数不成立。" -ForegroundColor Yellow
