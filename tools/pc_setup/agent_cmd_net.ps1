# net.ps1 - part of instagent.cmd: the address on the device link.
#
# The device works as a monitor only and always keeps 192.168.7.1 (emergency mode: its own
# DHCP server hands out addresses). If Windows takes such a lease, the lease comes WITH the
# gateway 192.168.7.1 and Windows routes the host internet into the device - the host loses
# its internet. The fix is a fixed address without a gateway, exactly what is needed here:
#
#     192.168.7.2/24, no gateway, metric 9000, DHCP off, DNS cleared
#
# The host address is then always the same (192.168.7.2) and the device always at
# 192.168.7.1, so nothing has to be looked up or negotiated.
#
# Keys:
#   (none)      set the fixed address
#   -Restore    give the address back to DHCP (undo)
#   -DryRun     only show what is there now
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$Restore, [switch]$DryRun, [switch]$Quiet)
$ErrorActionPreference = 'Continue'

$Wanted = '192.168.7.2'

function Say($m) { if (-not $Quiet) { Write-Host ('  ' + $m) } }

function Get-DeviceAdapters {
    @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object {
        $_.InterfaceDescription -like '*NDIS*' -or $_.InterfaceDescription -like '*RNDIS*' })
}

$dev = Get-DeviceAdapters
if ($dev.Count -eq 0) {
    Say 'device adapter (RNDIS) is not enumerated - plug the device in first'
    exit 1
}

$changed = 0
foreach ($a in $dev) {
    Say ('adapter: ' + $a.Name + '  |  ' + $a.InterfaceDescription + '  |  ' + $a.Status)
    $idx = $a.ifIndex
    $ips = @((Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress)
    $got = if ($ips.Count) { $ips -join ', ' } else { 'none' }
    $dhcp = (Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).Dhcp
    $met = (Get-NetIPInterface -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue).InterfaceMetric
    Say ('  now    : address ' + $got + ', dhcp ' + $dhcp + ', metric ' + $met)

    if ($DryRun) { continue }

    if ($Restore) {
        try {
            Remove-NetRoute   -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue
            Remove-NetIPAddress -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue
            Set-NetIPInterface -InterfaceIndex $idx -Dhcp Enabled -ErrorAction Stop
            Say '  result : DHCP enabled again'
            $changed++
        } catch { Say ('  dhcp on: ' + $_.Exception.Message) }
        continue
    }

    try { Set-NetIPInterface -InterfaceIndex $idx -Dhcp Disabled -ErrorAction Stop }
    catch { Say ('  dhcp off: ' + $_.Exception.Message) }
    try { Remove-NetRoute   -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue } catch { }
    try { Remove-NetIPAddress -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue } catch { }
    try {
        New-NetIPAddress -InterfaceIndex $idx -IPAddress $Wanted -PrefixLength 24 -ErrorAction Stop | Out-Null
        Say ('  set    : ' + $Wanted + '/24, no gateway')
    } catch { Say ('  set ' + $Wanted + ': ' + $_.Exception.Message) }
    try {
        Set-NetIPInterface -InterfaceIndex $idx -InterfaceMetric 9000 -ErrorAction Stop
        Say '  set    : metric 9000 (the host internet keeps priority)'
    } catch { }
    try { Set-DnsClientServerAddress -InterfaceIndex $idx -ResetServerAddresses -ErrorAction SilentlyContinue } catch { }
    $changed++
}

if ($DryRun) {
    Say 'DRY RUN: the address was not touched'
    exit 0
}

# ---- verify
foreach ($a in Get-DeviceAdapters) {
    $ips = @((Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress)
    $gw  = @((Get-NetRoute -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue))
    $got = if ($ips.Count) { $ips -join ', ' } else { 'none' }
    Say ('check  : ' + $a.Name + ' -> ' + $got + ', default routes on it: ' + $gw.Count +
         ' (must be 0 - otherwise the host internet can go into the device)')
    if (-not $Restore -and $ips -notcontains $Wanted) {
        Say ('WARN   : the adapter does not have ' + $Wanted + ' - set it by hand if metrics do not arrive')
    }
}
Say ('adapters changed: ' + $changed)
exit 0
