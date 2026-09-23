<#
 reset_disk_node.ps1 - make Windows classify the device disk from scratch. Run AS ADMIN.

 Symptom: the device mass-storage interface is bound (USB\Class_08&SubClass_06&Prot_50,
 service USBSTOR) but no disk appears - Windows kept a "floppy" device from an earlier
 classification (USBSTOR\SFloppy&...*) and an empty A: with no media. That key is
 cached from an older firmware build; the current firmware answers INQUIRY with
 removable=0, so a fresh enumeration should produce a normal disk.

 What it does: removes the cached USBSTOR child node of the device, rescans the bus, then
 reports what Windows enumerated instead (DiskDrive / FloppyDisk) and which volumes exist.

 Log: %TEMP%\reset_disk_node.log
#>
$log = Join-Path $env:TEMP 'reset_disk_node.log'
function Say($m) {
    $line = (Get-Date -Format 'HH:mm:ss') + '  ' + $m
    Write-Host $line
    Add-Content -Path $log -Value $line -Encoding UTF8
}
Set-Content -Path $log -Value ("reset_disk_node  " + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding UTF8

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
         [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Say 'NO ADMIN RIGHTS - run as administrator.'; exit 1 }

Say '--- 1. storage nodes of the device (before) ---'
$nodes = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -like '*INKMETRICS*' })
if ($nodes.Count -eq 0) { Say '  none found' }
foreach ($n in $nodes) { Say ('  ' + $n.Status + '  ' + $n.Class + '  ' + $n.FriendlyName + '  ' + $n.InstanceId) }

Say '--- 2. removing the cached node(s) ---'
foreach ($n in $nodes) {
    Say ('  remove: ' + $n.InstanceId)
    & pnputil /remove-device "$($n.InstanceId)" /subtree 2>&1 | ForEach-Object { Say ('  pnputil: ' + $_) }
}
Say '  rescanning the bus'
& pnputil /scan-devices 2>&1 | ForEach-Object { Say ('  pnputil: ' + $_) }
Start-Sleep -Seconds 10

Say '--- 3. what Windows enumerated now ---'
$after = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -like '*INKMETRICS*' })
if ($after.Count -eq 0) { Say '  nothing with INKMETRICS in the ID' }
foreach ($n in $after) { Say ('  ' + $n.Status + '  ' + $n.Class + '  ' + $n.FriendlyName + '  ' + $n.InstanceId) }

Say '  disk drives:'
Get-PnpDevice -PresentOnly -Class DiskDrive -ErrorAction SilentlyContinue |
    ForEach-Object { Say ('    ' + $_.Status + '  ' + $_.FriendlyName) }
Say '  floppy drives:'
Get-PnpDevice -PresentOnly -Class FloppyDisk -ErrorAction SilentlyContinue |
    ForEach-Object { Say ('    ' + $_.Status + '  ' + $_.FriendlyName) }
Say '  volumes:'
Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveLetter } |
    ForEach-Object { Say ('    ' + $_.DriveLetter + ':  ' + $_.FileSystem + '  ' + $_.Size + '  ' + $_.DriveType) }
Say 'done.'
