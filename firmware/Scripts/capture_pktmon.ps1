<#
  =============================================================================
  File Name : capture_pktmon.ps1
  Function  : 使用 Windows 内置 pktmon 抓包，分析设备(192.168.10.101)与
              PC(192.168.10.100) 之间的 ICMP/ARP 流量
  用法      : 右键 PowerShell -> 以管理员身份运行：
              powershell -ExecutionPolicy Bypass -File Scripts\capture_pktmon.ps1 -Seconds 20
              抓包窗口内请在设备 CLI 执行:  ping 192.168.10.100
  作者      : lwip_test_author
  日期      : 2026-08-12
  =============================================================================
#>
param(
    [int]$Seconds = 20
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------
# 1) 管理员权限检查（pktmon 抓包必须提权）
# ---------------------------------------------------------------
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "[ERROR] 请以【管理员身份】运行本脚本！" -ForegroundColor Red
    Write-Host "        右键 PowerShell -> 以管理员身份运行，然后再次执行本脚本"
    exit 1
}

# ---------------------------------------------------------------
# 2) 检查 pktmon 是否可用（Win10 1809+ / Win11 内置）
# ---------------------------------------------------------------
if (-not (Get-Command pktmon -ErrorAction SilentlyContinue)) {
    Write-Host "[ERROR] 系统未包含 pktmon，需要 Windows 10 1809 及以上版本" -ForegroundColor Red
    exit 1
}

$outEtl = Join-Path $PSScriptRoot "pktmon_capture.etl"
$outTxt = Join-Path $PSScriptRoot "pktmon_capture.txt"
$outFlt = Join-Path $PSScriptRoot "pktmon_icmp_arp.txt"

Remove-Item $outEtl, $outTxt, $outFlt -ErrorAction SilentlyContinue

# 停止可能残留的旧抓包
pktmon stop 2>$null | Out-Null
Start-Sleep -Milliseconds 500

# ---------------------------------------------------------------
# 3) 开始抓包：--pkt-size 0 记录完整帧（含以太网头，可看 ARP）
# ---------------------------------------------------------------
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " 抓包即将开始 $Seconds 秒" -ForegroundColor Cyan
Write-Host " >>> 请立即在设备 CLI 执行:  ping 192.168.10.100 <<<" -ForegroundColor Yellow
Write-Host "============================================================" -ForegroundColor Cyan
pktmon start --capture --pkt-size 0 --file-name $outEtl | Out-Null

# 等待抓包窗口
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
}

Write-Host "[INFO] 抓包窗口结束，停止抓包..." -ForegroundColor Cyan
pktmon stop | Out-Null
Start-Sleep -Milliseconds 500

# ---------------------------------------------------------------
# 4) ETL 转可读文本
# ---------------------------------------------------------------
pktmon etl2txt $outEtl -o $outTxt | Out-Null
if (-not (Test-Path $outTxt)) {
    Write-Host "[ERROR] ETL 转文本失败，请检查 pktmon 输出" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------
# 5) 过滤与设备相关的流量（设备 IP / PC 有线 IP）
# ---------------------------------------------------------------
$lines = Get-Content $outTxt
$rel = $lines | Select-String -Pattern "192\.168\.10\.(100|101)"

Write-Host "`n================ 与设备相关的流量（过滤结果） ================" -ForegroundColor Green
if ($rel.Count -eq 0) {
    Write-Host "  [WARN] 未捕获到任何 192.168.10.100/.101 相关流量！" -ForegroundColor Yellow
    Write-Host "         可能原因：设备未实际发包 / 设备连到了其它接口 / 抓包期间未执行 ping"
} else {
    $rel | ForEach-Object { Write-Host ("  " + $_.Line) }
}

# 保存过滤结果
$rel | ForEach-Object { $_.Line } | Set-Content $outFlt

# 简单统计：ICMP 请求(类型8) / 应答(类型0) 数量
$icmp_req = ($rel | Where-Object { $_ -match "ICMP" -and $_ -match "type.?=?8|Type.*8|echo request|Echo Request" }).Count
$icmp_rsp = ($rel | Where-Object { $_ -match "ICMP" -and $_ -match "type.?=?0|Type.*0|echo reply|Echo Reply" }).Count
if (($icmp_req -gt 0) -or ($icmp_rsp -gt 0)) {
    Write-Host "`n  ICMP echo request(类型8) 条数: $icmp_req" -ForegroundColor Green
    Write-Host "  ICMP echo reply   (类型0) 条数: $icmp_rsp" -ForegroundColor Green
}

Write-Host "`n[INFO] 完整抓包文本: $outTxt" -ForegroundColor Cyan
Write-Host "[INFO] 过滤结果文件: $outFlt" -ForegroundColor Cyan

# 判读提示
Write-Host "`n================ 结果判读 ================" -ForegroundColor Magenta
Write-Host "  ① 无任何 192.168.10.10x 流量       -> 设备根本没把帧发到 PC 网卡，问题在设备发送侧"
Write-Host "  ② 有 ARP/ICMP 请求、无 echo reply   -> PC 未应答（继续查 PC 侧）"
Write-Host "  ③ 有请求且有 reply                  -> PC 已回包，问题在设备接收/解析侧"
