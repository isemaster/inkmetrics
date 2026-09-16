# ics_enable.ps1 - turn Internet Connection Sharing (ICS) on/off for the device adapter.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
#
# What it does:
#   * device adapter  = the one whose description matches "Remote NDIS" (the inkmetrics board)
#   * internet adapter = the adapter that holds the default route, skipping virtual/VPN ones
#     (on the wired PC this is the Ethernet cable, as the user confirmed ICS works on wired)
#   * enables sharing: public = internet adapter, private = device adapter
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File ics_enable.ps1 -DryRun
#   powershell -NoProfile -ExecutionPolicy Bypass -File ics_enable.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File ics_enable.ps1 -Off
#   (enabling/disabling needs administrator rights)
param(
    [switch]$Off,
    [switch]$DryRun,
    [switch]$Quiet,
    [switch]$NoPause,
    [string]$Public = "",
    [string]$Private = ""
)

$ErrorActionPreference = "Continue"

function Say($msg) { if (-not $Quiet) { Write-Host $msg } }
function Fail($msg) { Write-Host "ERROR: $msg"; exit 1 }

# ---------------------------------------------------------------- who is who

# device adapter: Remote NDIS (the board). Fall back to a name match.
$device = Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object {
    $_.InterfaceDescription -match "Remote NDIS" -or $_.Name -match "Remote NDIS" -or
    $_.InterfaceDescription -match "RNDIS"
} | Select-Object -First 1
if (-not $device) {
    Say "device adapter (Remote NDIS) not found - device is not plugged in; nothing to do"
    exit 0
}
if (-not $Off -and $device.Status -ne "Up") {
    Fail "device adapter '$($device.Name)' is not Up (status: $($device.Status))"
}

# internet adapter: default route, skipping virtual adapters (VPN, WSL, Hyper-V, ...)
$skip = "Virtual|VPN|WSL|Hyper-V|Loopback|Bluetooth|TAP|Tunnel|Radmin|Npcap|vEthernet"
$pub = $null
if ($Public -ne "") {
    $pub = Get-NetAdapter -Name $Public -ErrorAction SilentlyContinue
    if (-not $pub) { Fail "adapter '$Public' not found (parameter -Public)" }
} else {
    $routes = Get-NetRoute -DestinationPrefix "0.0.0.0/0" -ErrorAction SilentlyContinue |
        Sort-Object RouteMetric
    foreach ($r in $routes) {
        $cand = Get-NetAdapter -Name $r.InterfaceAlias -ErrorAction SilentlyContinue
        if (-not $cand) { continue }
        if ($cand.InterfaceDescription -match $skip) { continue }
        if ($cand.Status -ne "Up") { continue }
        $pub = $cand
        break
    }
}
if (-not $pub) { Fail "no internet adapter found (no default route on a physical adapter)" }

Say ("device adapter  : " + $device.Name + " | " + $device.InterfaceDescription + " | " + $device.Status)
Say ("internet adapter: " + $pub.Name + " | " + $pub.InterfaceDescription + " | " + $pub.Status)

# ---------------------------------------------------------------- ICS via COM

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $elevated) {
    if ($DryRun) {
        Say "NOTE: not elevated - only showing what would be done (-DryRun)"
    } else {
        Fail "administrator rights are required to enable/disable ICS"
    }
}

try {
    $mgr = New-Object -ComObject HNetCfg.HNetShare
} catch {
    Fail ("COM HNetCfg.HNetShare is not available: " + $_.Exception.Message)
}

$conns = @{}
foreach ($c in $mgr.EnumEveryConnection) {
    $p = $mgr.NetConnectionProps($c)
    $conns[$p.Name] = @{ conn = $c; name = $p.Name; device = $p.DeviceName }
}

$pubConn = $null
$privConn = $null
foreach ($k in $conns.Keys) {
    $e = $conns[$k]
    if ($e.name -eq $pub.Name -or ($e.device -ne "" -and $e.device -eq $pub.InterfaceDescription)) { $pubConn = $e }
    if ($Private -ne "" -and $e.name -eq $Private) { $privConn = $e }
    if ($e.name -eq $device.Name -or ($e.device -ne "" -and $e.device -eq $device.InterfaceDescription)) { $privConn = $e }
}
if (-not $pubConn)  { Fail ("internet adapter '" + $pub.Name + "' is not visible to ICS (COM)") }
if (-not $privConn) { Fail ("device adapter '" + $device.Name + "' is not visible to ICS (COM)") }

$cfgPub = $mgr.INetSharingConfigurationForINetConnection($pubConn.conn)
$cfgPriv = $mgr.INetSharingConfigurationForINetConnection($privConn.conn)

Say ("sharing now     : public=" + $pubConn.name + " enabled=" + $cfgPub.SharingEnabled +
     " type=" + $cfgPub.SharingConnectionType + " ; private=" + $privConn.name +
     " enabled=" + $cfgPriv.SharingEnabled + " type=" + $cfgPriv.SharingConnectionType)

if ($Off) {
    if ($DryRun) { Say "DRY RUN: would disable sharing on both adapters"; exit 0 }
    if ($cfgPriv.SharingEnabled) { $cfgPriv.DisableSharing() }
    if ($cfgPub.SharingEnabled)  { $cfgPub.DisableSharing() }
    Say "sharing disabled"
    exit 0
}

# already configured the right way? (public = internet, private = device)
if ($cfgPub.SharingEnabled -and $cfgPub.SharingConnectionType -eq 0 -and
    $cfgPriv.SharingEnabled -and $cfgPriv.SharingConnectionType -eq 1) {
    Say "already enabled correctly - nothing to do"
} else {
    if ($DryRun) {
        Say "DRY RUN: would enable sharing: public=$($pubConn.name) (type 0), private=$($privConn.name) (type 1)"
        exit 0
    }
    # order matters: clear the old configuration first, then set public, then private
    if ($cfgPub.SharingEnabled)  { $cfgPub.DisableSharing(); Start-Sleep -Seconds 2 }
    if ($cfgPriv.SharingEnabled) { $cfgPriv.DisableSharing(); Start-Sleep -Seconds 2 }
    $cfgPub.EnableSharing(0)
    Start-Sleep -Seconds 3
    $cfgPriv.EnableSharing(1)
    Say "sharing enabled: public=$($pubConn.name), private=$($privConn.name)"
}

# ---------------------------------------------------------------- wait for the lease

$addr = $null
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Seconds 1
    $ip = Get-NetIPAddress -InterfaceIndex $device.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike "169.254.*" } | Select-Object -First 1
    if ($ip) { $addr = $ip.IPAddress; break }
}
if ($addr) {
    Say ("device address  : " + $addr)
    try {
        $r = Invoke-WebRequest -Uri ("http://" + $addr + "/api/state") -TimeoutSec 5 -UseBasicParsing
        Say ("device answers  : HTTP " + $r.StatusCode)
        Say $r.Content
    } catch {
        Say ("device answers  : no HTTP yet (" + $_.Exception.Message + ")")
    }
} else {
    Say "device address  : not received yet (check the ADDR line on the device screen)"
}

Say "shared adapter on this PC is now 192.168.137.1; device address is shown on the device screen"
exit 0

# Пауза в конце, чтобы окно не исчезало при запуске двойным кликом. Задание
# планировщика и установщик агента зовут скрипт с -NoPause (ждать Enter там некому).
if (-not $NoPause) {
    $interactive = $true
    try { if ([Console]::IsInputRedirected) { $interactive = $false } } catch { }
    if ($interactive) {
        Write-Host ""
        try { [void](Read-Host "Press Enter to close"); } catch { }
    }
}
