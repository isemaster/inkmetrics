<#
 fix_usb_net.ps1 v2 - repair the inkmetrics device USB link and stop it from stealing
 the host internet. Run AS ADMINISTRATOR.

 What it does, in order:
   1. Finds the device USB nodes (VID_303A) and prints their state.
   2. Restarts the composite device so Windows re-binds its functions. If a function
      node is still in Error - removes it (/remove-device /subtree) and rescans the bus.
      This is what fixes: no device network, no device page, no device disk.
   3. Brings the device network adapter administratively UP.
   4. Gives it static 192.168.7.2/24 WITHOUT a gateway and metric 9000, so Windows
      never routes host internet into the device (the old DHCP lease carried gateway
      192.168.7.1, and that killed wired internet).
   5. Reports default routes, adapter state, ping to 192.168.7.1 and visible disks.

 Keys:
   -SkipNodes   skip step 2 (do not touch USB device nodes)
   -Restore     bring DHCP back on the device adapter (undo step 4)

 Log: %TEMP%\fix_usb_net.log
#>
param([switch]$SkipNodes, [switch]$Restore)

$log = Join-Path $env:TEMP 'fix_usb_net.log'
function Say($m) {
    $line = (Get-Date -Format 'HH:mm:ss') + '  ' + $m
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}
Set-Content -Path $log -Value ("fix_usb_net v2  " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding UTF8

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
         [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    Say 'NO ADMIN RIGHTS - run this script as administrator.'
    exit 1
}

function Get-DeviceNodes {
    @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like '*VID_303A*' })
}
function Show-DeviceNodes($title) {
    Say ("  nodes now (" + $title + "):")
    $n = Get-DeviceNodes
    if ($n.Count -eq 0) { Say '    none' } else {
        foreach ($d in $n) { Say ('    ' + $d.Status + '  ' + $d.Class + '  ' + $d.FriendlyName + '  ' + $d.InstanceId) }
    }
}

Say '--- 1. Device nodes before repair ---'
Show-DeviceNodes 'before'

if (-not $SkipNodes) {
    $nodes = Get-DeviceNodes
    $parent = $nodes | Where-Object { $_.InstanceId -notlike '*MI_*' } | Select-Object -First 1
    $bad    = @($nodes | Where-Object { $_.Status -ne 'OK' })
    Say ('--- 2. Repair (bad nodes: ' + $bad.Count + ') ---')

    if ($parent) {
        Say ('  restarting composite device: ' + $parent.InstanceId)
        & pnputil /restart-device "$($parent.InstanceId)" 2>&1 | ForEach-Object { Say ('  pnputil: ' + $_) }
        Start-Sleep -Seconds 10
        Show-DeviceNodes 'after restart'
    }

    $still = @(Get-DeviceNodes | Where-Object { $_.Status -ne 'OK' })
    foreach ($n in $still) {
        Say ('  removing stuck node: ' + $n.InstanceId)
        & pnputil /remove-device "$($n.InstanceId)" /subtree 2>&1 | ForEach-Object { Say ('  pnputil: ' + $_) }
    }
    if ($still.Count -gt 0) {
        & pnputil /scan-devices 2>&1 | ForEach-Object { Say ('  pnputil: ' + $_) }
        Start-Sleep -Seconds 10
        Show-DeviceNodes 'after remove+scan'
    }
} else {
    Say '--- 2. Repair skipped (-SkipNodes) ---'
}

Say '--- 3. Device network adapter ---'
$dev = @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object {
    $_.InterfaceDescription -like '*NDIS*' -or $_.InterfaceDescription -like '*RNDIS*' })
if ($dev.Count -eq 0) {
    Say '  device adapter not enumerated (RNDIS function did not come up)'
} else {
    foreach ($a in $dev) {
        Say ('  found: ' + $a.Name + '  |  ' + $a.InterfaceDescription + '  |  status ' + $a.Status + '  admin ' + $a.AdminStatus)
        if ($a.AdminStatus -ne 'Up') {
            try {
                Enable-NetAdapter -Name $a.Name -Confirm:$false -ErrorAction Stop
                Say '  adapter enabled'
                Start-Sleep -Seconds 5
            } catch { Say ('  enable adapter: ' + $_.Exception.Message) }
        }
        $a = Get-NetAdapter -Name $a.Name -ErrorAction SilentlyContinue
        $idx = $a.ifIndex

        Say '--- 4. Address on the device link ---'
        if ($Restore) {
            try { Set-NetIPInterface -InterfaceIndex $idx -Dhcp Enabled -ErrorAction Stop; Say '  DHCP enabled again' }
            catch { Say ('  dhcp on: ' + $_.Exception.Message) }
            continue
        }
        try { Set-NetIPInterface -InterfaceIndex $idx -Dhcp Disabled -ErrorAction Stop }
        catch { Say ('  dhcp off: ' + $_.Exception.Message) }
        try { Remove-NetRoute   -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue } catch { }
        try { Remove-NetIPAddress -InterfaceIndex $idx -Confirm:$false -ErrorAction SilentlyContinue } catch { }
        try {
            New-NetIPAddress -InterfaceIndex $idx -IPAddress 192.168.7.2 -PrefixLength 24 -ErrorAction Stop | Out-Null
            Say '  set 192.168.7.2/24 without gateway'
        } catch { Say ('  set 192.168.7.2: ' + $_.Exception.Message) }
        try {
            Set-NetIPInterface -InterfaceIndex $idx -InterfaceMetric 9000 -ErrorAction Stop
            Say '  metric 9000 (wired internet has priority)'
        } catch { }
        try { Set-DnsClientServerAddress -InterfaceIndex $idx -ResetServerAddresses -ErrorAction SilentlyContinue } catch { }
    }
}

Say '--- 5. Result ---'
Say '  default routes:'
Get-NetRoute -AddressFamily IPv4 -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue |
    ForEach-Object { Say ('    ' + $_.InterfaceAlias + '  via ' + $_.NextHop + '  metric ' + $_.InterfaceMetric) }
foreach ($a in @(Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like '*NDIS*' })) {
    $ip = (Get-NetIPAddress -InterfaceIndex $a.ifIndex -AddressFamily IPv4 -ErrorAction SilentlyContinue).IPAddress -join ', '
    if (-not $ip) { $ip = 'none' }
    Say ('  ' + $a.Name + ': ' + $a.Status + '  address ' + $ip)
}
$p = Test-Connection -ComputerName 192.168.7.1 -Count 1 -Quiet -ErrorAction SilentlyContinue
if ($p) { Say '  ping device 192.168.7.1: reply' } else { Say '  ping device 192.168.7.1: no reply' }
Say '  disks:'
Get-Disk -ErrorAction SilentlyContinue | ForEach-Object { Say ('    ' + $_.Number + '  ' + $_.FriendlyName + '  ' + $_.BusType + '  ' + $_.OperationalStatus) }
Say 'done.'
