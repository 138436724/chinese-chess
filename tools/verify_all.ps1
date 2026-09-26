# 全量离线验证入口
#
# 一条命令跑完所有**不需要完整构建**的验证:
#   1. 着色器编译(逐 entry point)+ GPU 结构体布局比对
#   2. 环境光照数值验收(辐照度无偏性 + 环境 NEE 的 MIS 组合无偏性)
#   3. 多次散射能量补偿验收(白炉与有色表面能量守恒)
#   4. 程序化球体网格拓扑校验(绕序/退化/封闭性/UV 手性)
#   5. EXR 参考对比工具自测
#
# 用法:pwsh tools/verify_all.ps1
# 退出码 0 = 全部通过。
#
# 说明:这**不替代**完整构建 —— 运行期的成像/帧时间仍需一次能跑起来的构建
# (需先在联网环境完成 vcpkg install)。但着色器语法、GPU 布局、全部物理数值
# 都能在这里离线验证。

$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
$failed = @()

function Step {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ""
    Write-Host "################ $Name ################" -ForegroundColor Yellow
    & $Body
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] $Name (exit=$LASTEXITCODE)" -ForegroundColor Red
        $script:failed += $Name
    } else {
        Write-Host "[PASS] $Name" -ForegroundColor Green
    }
}

$env:PYTHONIOENCODING = "utf-8"

Step "着色器编译 + GPU 布局" { pwsh -NoProfile -File (Join-Path $PSScriptRoot "verify_shaders.ps1") }
Step "环境光照验收"         { python (Join-Path $PSScriptRoot "pbr_env_check.py") }
Step "能量补偿验收"         { python (Join-Path $PSScriptRoot "pbr_verify_compensation.py") }
Step "球体网格校验"         { python (Join-Path $PSScriptRoot "verify_sphere_mesh.py") }
Step "EXR 对比工具自测"     { python (Join-Path $PSScriptRoot "compare_exr.py") --selftest }

Write-Host ""
if ($failed.Count -gt 0) {
    Write-Host "失败项: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "全部离线验证通过" -ForegroundColor Green
Write-Host "注意:运行期验证(成像 / 帧时间 / 收敛帧数)仍需一次完整构建。" -ForegroundColor DarkGray
exit 0
