# 性能探针 —— 记录帧率,并量化环境光的开销
#
# 做什么:启动应用 → 采样窗口标题里的 FPS(每 0.5 秒刷新一次)
#         → PostMessage 投 `J` 关掉环境光 → 再采样 → 恢复 → 关闭
#
# 为什么用标题栏:FPS 已经在窗口标题里(`glfw_window` 每 0.5 s 更新),
# 不需要改代码加仪表盘。
#
# 用法:pwsh tools/perf_probe.ps1 [-Exe <path>] [-WarmupSeconds N] [-Samples N]

param(
    [string]$Exe = "out\Release\chinese-chess.exe",
    [int]$WarmupSeconds = 10,
    [int]$Samples = 5,
    [int]$SampleIntervalSeconds = 2
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PerfW32 {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@

function Send-Key([IntPtr]$hwnd, [int]$vk, [int]$scancode) {
    $down = [IntPtr](1 -bor ($scancode -shl 16))
    $up   = [IntPtr](0xC0000001 -bor ($scancode -shl 16))
    [void][PerfW32]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, $down)
    [void][PerfW32]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, $up)
}

function Read-Fps([int]$procId) {
    $q = Get-Process -Id $procId -ErrorAction SilentlyContinue
    if (-not $q) { return $null }
    if ($q.MainWindowTitle -match '([0-9]+(?:\.[0-9]+)?)\s*fps') { return [double]$Matches[1] }
    return $null
}

function Sample-Fps([int]$procId, [int]$n, [int]$interval) {
    $vals = @()
    for ($i = 0; $i -lt $n; $i++) {
        Start-Sleep -Seconds $interval
        $f = Read-Fps $procId
        if ($null -ne $f) { $vals += $f }
        Write-Host ("    sample {0}: {1}" -f ($i + 1), ($(if ($null -ne $f) { "{0:N1} fps" -f $f } else { "n/a" })))
    }
    return $vals
}

$exePath = Join-Path $root $Exe
if (-not (Test-Path $exePath)) { Write-Error "找不到可执行文件: $exePath" }
Write-Host "可执行文件: $exePath" -ForegroundColor Cyan

$p = Start-Process -FilePath $exePath -WorkingDirectory (Join-Path $root "chinese-chess") -PassThru
Write-Host "已启动 PID=$($p.Id),等待 $WarmupSeconds 秒初始化..." -ForegroundColor Cyan
Start-Sleep -Seconds $WarmupSeconds

$q = Get-Process -Id $p.Id -ErrorAction SilentlyContinue
if (-not $q) { Write-Error "进程提前退出" }
$hwnd = $q.MainWindowHandle

Write-Host ""
Write-Host "[A] 默认场景(环境光开,env_intensity = 1)" -ForegroundColor Yellow
$a = Sample-Fps $p.Id $Samples $SampleIntervalSeconds

Write-Host ""
Write-Host "投递 J(关闭环境光:辐照度 + 环境 NEE)..." -ForegroundColor Cyan
Send-Key $hwnd 0x4A 0x24   # 'J'
Start-Sleep -Seconds 3

Write-Host ""
Write-Host "[B] 关闭环境光" -ForegroundColor Yellow
$b = Sample-Fps $p.Id $Samples $SampleIntervalSeconds

Write-Host ""
Write-Host "恢复环境光并关闭..." -ForegroundColor Cyan
Send-Key $hwnd 0x4A 0x24
Start-Sleep -Seconds 1
if (-not $p.HasExited) { [void]$p.CloseMainWindow(); if (-not $p.WaitForExit(15000)) { $p.Kill() } }

Write-Host ""
Write-Host "=== 结果 ===" -ForegroundColor Green
if ($a.Count -gt 0) {
    $ma = ($a | Measure-Object -Average).Average
    Write-Host ("  [A] 环境光开:平均 {0:N1} fps  (min {1:N1}, max {2:N1})" -f $ma, ($a | Measure-Object -Minimum).Minimum, ($a | Measure-Object -Maximum).Maximum)
}
if ($b.Count -gt 0) {
    $mb = ($b | Measure-Object -Average).Average
    Write-Host ("  [B] 环境光关:平均 {0:N1} fps" -f $mb)
}
if ($a.Count -gt 0 -and $b.Count -gt 0) {
    $ma = ($a | Measure-Object -Average).Average
    $mb = ($b | Measure-Object -Average).Average
    Write-Host ("  环境光开销:每帧多 {0:N2} ms(占比 {1:N1}%)" -f (1000.0 / $ma - 1000.0 / $mb), (100.0 * (1.0 / $ma - 1.0 / $mb) / (1.0 / $ma)))
}
