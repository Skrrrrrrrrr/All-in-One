# =============================================================================
# check_icmp_rule.ps1
# 自动检查 Windows 的 ICMPv4 入站规则状态（只读查询，无需管理员权限）
#
# 背景：设备 ping PC 收不到 Echo Reply。Wireshark 已证明设备发出的 Echo
# Request 到达 PC 网卡，但 PC 未回包。本脚本从 PC 侧检查三类嫌疑：
#   1. 防火墙 ICMPv4 入站规则（是否允许"回显请求"入站）
#   2. 防火墙配置文件启用状态
#   3. 到目标网段（192.168.11.0/24）的出口路由（排除回复走错网卡）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File check_icmp_rule.ps1
# =============================================================================

$ErrorActionPreference = 'Continue'

Write-Host "==== 0. 防火墙服务状态 ====" -ForegroundColor Cyan
Get-Service mpssvc -ErrorAction SilentlyContinue | Select-Object Name, Status, StartType | Format-Table -AutoSize

Write-Host "`n==== 1. 防火墙 ICMPv4 入站规则 ====" -ForegroundColor Cyan
$icmpRules = @()
Get-NetFirewallRule -Direction Inbound -ErrorAction SilentlyContinue | ForEach-Object {
    $rule = $_
    $port = $rule | Get-NetFirewallPortFilter -ErrorAction SilentlyContinue
    if ($port -and $port.Protocol -eq 'ICMPv4') {
        $icmpRules += [PSCustomObject]@{
            Name     = $rule.DisplayName
            Enabled  = $rule.Enabled
            Action   = $rule.Action
            Profile  = ($rule.Profile -join ',')
            IcmpType = $port.IcmpType
        }
    }
}
if ($icmpRules.Count -gt 0) {
    $icmpRules | Format-Table -AutoSize
} else {
    Write-Host "未找到任何 ICMPv4 规则" -ForegroundColor Yellow
}

Write-Host ""
$allowEcho = $icmpRules | Where-Object {
    $_.Action -eq 'Allow' -and $_.Enabled -eq 'True' -and
    ($_.IcmpType -eq 8 -or $_.IcmpType -eq 'Any')
}
if ($allowEcho) {
    Write-Host "[OK] 存在已启用的'允许 ICMPv4 回显请求(Echo Request)'规则" -ForegroundColor Green
} else {
    Write-Host "[警告] 未找到已启用的'允许 ICMPv4 回显请求'入站规则" -ForegroundColor Red
    Write-Host "        PC 拒绝回应 ping 的常见原因。可用管理员运行："
    Write-Host "        netsh advfirewall firewall add rule name=\"允许ICMP回显\" protocol=icmpv4:8,any dir=in action=allow" -ForegroundColor Yellow
}

Write-Host "`n==== 2. 防火墙当前活动配置文件 ====" -ForegroundColor Cyan
Get-NetFirewallProfile -ErrorAction SilentlyContinue |
    Select-Object Name, Enabled, DefaultInboundAction |
    Format-Table -AutoSize

Write-Host "`n==== 3. 本机 192.168.* 网段 IP 配置 ====" -ForegroundColor Cyan
$ipcfg = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -like '192.168.*' } |
    Select-Object InterfaceAlias, InterfaceIndex, IPAddress, PrefixLength
if ($ipcfg) {
    $ipcfg | Format-Table -AutoSize
} else {
    Write-Host "未找到 192.168.* 网段的 IPv4 地址" -ForegroundColor Yellow
}

Write-Host "`n==== 4. 到设备 192.168.11.101 的出口路由（关键）====" -ForegroundColor Cyan
# 网线直连时，PC 到设备必须命中"on-link"（NextHop 为 on-link / 或接口自身 IP）。
# 若 NextHop 显示 192.168.11.1 或其它网关，说明 PC 认为设备需经网关转发——
# 直连无网关导致 Echo Reply 无法发出，这正是"PC 收包不回包"的根因。
$routes = Find-NetRoute -RemoteIPAddress '192.168.11.101' -ErrorAction SilentlyContinue
if ($routes) {
    $routes |
        Select-Object InterfaceAlias, InterfaceIndex, Destination, NextHop, RouteMetric |
        Format-Table -AutoSize
} else {
    Write-Host "无法解析到 192.168.11.101 的路由（本机以太网口可能未配置 192.168.11.x 网段）" -ForegroundColor Yellow
}

Write-Host "`n==== 5. 手动 route 表检查（192.168.11 相关条目）====" -ForegroundColor Cyan
route print -4 | Select-String -Pattern '192\.168\.11' | ForEach-Object { $_.Line }
Write-Host "  （直连网段应为：192.168.11.0  255.255.255.0  on-link  <接口IP>  276）" -ForegroundColor DarkGray

Write-Host "`n==== 6. 以太网口子网掩码 ====" -ForegroundColor Cyan
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -like '192.168.*' } |
    Select-Object InterfaceAlias, IPAddress, PrefixLength, InterfaceMetric |
    Format-Table -AutoSize
Write-Host "  （掩码必须是 /24=255.255.255.0；网关栏应留空，直连无需网关）" -ForegroundColor DarkGray

Write-Host "`n==== 7. 结果解读 ====" -ForegroundColor Cyan
Write-Host "  - 若第 1 步显示 [警告]：ICMP 入站被防火墙拦截，按提示命令放行后重测。"
Write-Host "  - 若第 4 步 NextHop 显示 192.168.11.1（而非 on-link）：PC 认为回复要经不存在的"
Write-Host "    网关转发，直连时该帧永远发不出去（Wireshark 帧10 即此症状）。修复：将以太网"
Write-Host "    口 IP 设为 192.168.11.100/24，网关留空，再 'route delete 192.168.11.0' 后重测。"
Write-Host "  - 若第 4 步出口不是以太网口（而是 WiFi 等其它接口）：Windows 会把 Echo Reply"
Write-Host "    从错误网卡发出，设备收不到。禁用 WiFi 或调整接口跃点即可。"
Write-Host "  - 若第 1/4 步都正常仍无回复：用 Wireshark 看 PC 是否真的生成了 Echo Reply 帧"
Write-Host "    及其源 MAC（直连场景必须能看到发往 2C:F0:5D:96:C8:77 的 192.168.11.100 单播帧）。"
Write-Host ""
Write-Host "检查完成。"
