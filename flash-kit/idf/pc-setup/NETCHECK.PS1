# net_check2.ps1 - what this PC sees: adapters, bridge, device, route.
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File net_check2.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File net_check2.ps1 -Device 192.168.1.26
#   ... -NoPause      (не ждать Enter в конце — если скрипт зовут из другого скрипта)
param(
    [string]$Device = "",
    [switch]$NoPause
)

$ErrorActionPreference = "Continue"

Write-Host "== Adapters =="
Get-NetAdapter | Select-Object Name, InterfaceDescription, Status, LinkSpeed |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host

Write-Host "== IPv4 addresses =="
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -ne "127.0.0.1" } |
    Select-Object InterfaceAlias, IPAddress, PrefixLength |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host

Write-Host "== Default route =="
Get-NetRoute -DestinationPrefix "0.0.0.0/0" |
    Select-Object NextHop, InterfaceAlias, RouteMetric |
    Format-Table -AutoSize | Out-String -Width 160 | Write-Host

Write-Host "== Bridge present? =="
$bridge = Get-NetAdapter | Where-Object { $_.InterfaceDescription -match "Bridge" -or $_.Name -match "Bridge|Most" }
if ($bridge) {
    $bridge | Select-Object Name, InterfaceDescription, Status |
        Format-Table -AutoSize | Out-String -Width 160 | Write-Host
    Write-Host "Bridged adapters (bridge members are listed in their properties):"
    Get-NetAdapterBinding | Where-Object { $_.ComponentID -like "*ms_bridge*" -and $_.Enabled } |
        Select-Object Name, DisplayName | Format-Table -AutoSize | Out-String -Width 160 | Write-Host
} else {
    Write-Host "no bridge found"
}

Write-Host "== Device on USB (VID_303A = Espressif) =="
$usb = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_303A*" }
if ($usb) {
    $usb | Select-Object Status, Class, FriendlyName, InstanceId |
        Format-Table -AutoSize | Out-String -Width 200 | Write-Host
} else {
    Write-Host "device not present on USB (check cable / power / BOOT mode)"
}

Write-Host "== ARP entries that look like the device (MAC 70:04:1D / 72:04:1D) =="
$arp = arp -a
$arp | Where-Object { $_ -match "70-04-1D" -or $_ -match "72-04-1D" } | Write-Host
if (-not ($arp | Where-Object { $_ -match "70-04-1D" -or $_ -match "72-04-1D" })) {
    Write-Host "not found yet (device address may be unknown or not seen)"
}

if ($Device -ne "") {
    Write-Host "== Device check: $Device =="
    ping -n 2 $Device | Out-String | Write-Host
    try {
        $r = Invoke-WebRequest -Uri ("http://" + $Device + "/") -TimeoutSec 5 -UseBasicParsing
        Write-Host ("HTTP " + $r.StatusCode + ", bytes " + $r.RawContentLength)
    } catch {
        Write-Host ("HTTP error: " + $_.Exception.Message)
    }
    try {
        $j = Invoke-WebRequest -Uri ("http://" + $Device + "/api/state") -TimeoutSec 5 -UseBasicParsing
        Write-Host "api/state:"
        Write-Host $j.Content
    } catch {
        Write-Host ("api/state error: " + $_.Exception.Message)
    }
}

Write-Host "== SharedAccess (ICS) service =="
Get-Service SharedAccess | Select-Object Name, Status, StartType |
    Format-Table -AutoSize | Out-String -Width 120 | Write-Host

# Пауза в конце: при запуске двойным кликом окно иначе закрывается вместе со скриптом,
# и вывод прочитать нельзя. Ключ -NoPause нужен тем, кто зовёт скрипт из другого скрипта,
# а если ввод перенаправлен (запуск из конвейера) — не ждём вообще.
if (-not $NoPause) {
    $interactive = $true
    try { if ([Console]::IsInputRedirected) { $interactive = $false } } catch { }
    if ($interactive) {
        Write-Host ""
        try { [void](Read-Host "Press Enter to close"); } catch { }
    }
}
