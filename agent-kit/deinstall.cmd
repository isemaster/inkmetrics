@echo off
rem ============================================================================
rem  deinstall.cmd - remove the inkmetrics agent from this PC.
rem
rem  Self-contained as well: the removal script travels inside this file, so it
rem  works from any folder. It stops the agent, removes the task "inkmetrics
rem  agent", gives the device adapter address back to DHCP and deletes the install
rem  folder (C:\ProgramData\inkmetrics, or EINK_INSTALL_DIR if that is set).
rem  Nothing else on this PC is touched.
rem
rem  Usage:   deinstall.cmd              remove everything (asks for admin rights)
rem           deinstall.cmd --keep-net   keep the adapter address as it is
rem           deinstall.cmd --dry-run    only report what is installed
rem
rem  ASCII only on purpose: cmd.exe renders Russian text from a UTF-8 .cmd as garbage.
rem  Below the batch part there are payload sections, each starting with a rem marker
rem  line; from the first marker on the file is DATA, not batch code.
rem ============================================================================
setlocal EnableExtensions
set "SELF=%~f0"
set "DRY="
set "KEEPNET="
set "DIR="
for %%A in (%*) do (
    if /i "%%A"=="--dry-run"  set "DRY=1"
    if /i "%%A"=="--keep-net" set "KEEPNET=1"
)
if defined EINK_INSTALL_DIR set "DIR=%EINK_INSTALL_DIR%"
if not defined DIR set "DIR=%ProgramData%\inkmetrics"

set "RARGS="
if defined KEEPNET set "RARGS=-KeepNet"

echo.
echo   inkmetrics: agent removal
echo   install folder : %DIR%
echo.

set "EINK_DIR=%TEMP%\inkmetrics-remove"
if not defined DRY (
    net session >nul 2>&1
    if errorlevel 1 (
        echo   Administrator rights are required - asking for them now.
        powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:SELF -Verb RunAs"
        exit /b 0
    )
)

if not exist "%EINK_DIR%" mkdir "%EINK_DIR%" 2>nul
echo   unpacking the removal script...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$t=[IO.File]::ReadAllText($env:SELF); $p=[regex]::Split($t,'rem =====PAYLOAD:([A-Za-z0-9._-]+)====='); for($i=1;$i -lt $p.Count;$i+=2){ $n=$p[$i]; $b=$p[$i+1].Replace([string][char]13,''); [IO.File]::WriteAllText((Join-Path $env:EINK_DIR $n),$b); Write-Host ('    '+$n+': '+$b.Length+' bytes') }"
if errorlevel 1 (
    echo   FAILED: cannot unpack the payload from this file.
    pause
    exit /b 1
)

if defined DRY (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -DryRun -InstallDir "%DIR%" %RARGS%
    echo.
    echo   DRY RUN: nothing was changed.
    exit /b 0
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%EINK_DIR%\remove.ps1" -InstallDir "%DIR%" %RARGS%

echo.
if exist "%DIR%" (
    echo   deleting "%DIR%" ...
    rd /s /q "%DIR%" 2>nul
)
if exist "%DIR%" (
    echo   WARN: could not delete "%DIR%" - look at it by hand
) else (
    echo   install folder removed.
)
rd /s /q "%EINK_DIR%" 2>nul

echo.
echo   Agent removed: tasks gone, sharing off, files deleted.
echo.
pause
exit /b 0
rem =====PAYLOAD:net.ps1=====# net.ps1 - part of instagent.cmd: the address on the device link.
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
rem =====PAYLOAD:remove.ps1=====# remove.ps1 - part of deinstall.cmd: remove the agent from this PC.
#
# The monitor-only design has no internet sharing, so removal is short:
#   1. stops the running agent;
#   2. unregisters the task "inkmetrics agent" (and the old "inkmetrics ICS", if any);
#   3. gives the device adapter address back to DHCP (unless -KeepNet);
#   4. reports what is left; the caller (deinstall.cmd) then deletes the install folder.
#
# ASCII ONLY: PowerShell 5.1 reads .ps1 in the system codepage, Russian text breaks parsing.
param([switch]$DryRun, [switch]$KeepNet, [string]$InstallDir = '')
$ErrorActionPreference = 'Continue'

if (-not $InstallDir) { $InstallDir = Join-Path $env:ProgramData 'inkmetrics' }
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Net  = Join-Path $Here 'net.ps1'
if (-not (Test-Path $Net)) { $Net = Join-Path $InstallDir 'net.ps1' }

function Say($m) { Write-Host ('  ' + $m) }

Say ('install folder : ' + $InstallDir + '  (present: ' + (Test-Path $InstallDir) + ')')
# our tasks, plus every task that starts an agent.ps1 from another folder: those belong to an
# older installation and are removed in one go (the name of that folder does not matter)
$taskNames = @('inkmetrics agent', 'inkmetrics ICS')
Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object {
    $a = (($_.Actions | ForEach-Object { $_.Execute + ' ' + $_.Arguments }) -join ' ')
    $a -like '*agent.ps1*' -and $a -notlike ('*' + $InstallDir + '*')
} | ForEach-Object { $taskNames += $_.TaskName }
foreach ($tn in ($taskNames | Select-Object -Unique)) {
    $t = Get-ScheduledTask -TaskName $tn -ErrorAction SilentlyContinue
    if ($t) { Say ('task "' + $tn + '" : present (' + $t.State + ')') }
    else    { Say ('task "' + $tn + '" : not registered') }
}
$pidFile = Join-Path $InstallDir 'agent.pid'
$agentPid = 0
if (Test-Path $pidFile) {
    $txt = ''
    try { $txt = (Get-Content $pidFile -Encoding UTF8 | Select-Object -First 1).Trim() } catch { }
    [void][int]::TryParse($txt, [ref]$agentPid)
    if ($agentPid -and (Get-Process -Id $agentPid -ErrorAction SilentlyContinue)) {
        Say ('agent process  : running, pid ' + $agentPid)
    } else {
        Say ('agent process  : not running (agent.pid says ' + $txt + ')')
    }
} else {
    Say 'agent process  : no agent.pid file'
}
if (-not $KeepNet) { Say 'address        : would go back to DHCP' }

if ($DryRun) {
    Say 'DRY RUN: would stop the agent, remove the task and give the address back to DHCP'
    exit 0
}

# ---------------------------------------------------------------- 1. stop the agent
$stopped = 0
$running = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" -ErrorAction SilentlyContinue |
             Where-Object { $_.ProcessId -ne $PID -and $_.CommandLine -like ('*' + $InstallDir + '\agent.ps1*') })
foreach ($r in $running) {
    try { Stop-Process -Id $r.ProcessId -Force -ErrorAction Stop; $stopped++ } catch { }
}
Say ('stopped processes : ' + $stopped)

# ---------------------------------------------------------------- 2. tasks
foreach ($tn in ($taskNames | Select-Object -Unique)) {
    try {
        Unregister-ScheduledTask -TaskName $tn -Confirm:$false -ErrorAction Stop
        Say ('task removed      : ' + $tn)
    } catch {
        Say ('task not present  : ' + $tn)
    }
}

# ---------------------------------------------------------------- 3. address back to DHCP
if ($KeepNet) {
    Say 'address           : left as is (-KeepNet)'
} elseif (Test-Path $Net) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $Net -Restore -Quiet
} else {
    Say 'net.ps1 not found - set the device adapter back to DHCP by hand if you need to'
}
# ------------------------------------- 4. folders left over by an older installation
$here = $InstallDir.TrimEnd('\')
foreach ($d in @(Get-ChildItem $env:ProgramData -Directory -ErrorAction SilentlyContinue | Where-Object {
        $_.FullName.TrimEnd('\') -ne $here -and (Test-Path (Join-Path $_.FullName 'agent.ps1')) })) {
    Remove-Item -Recurse -Force $d.FullName -ErrorAction SilentlyContinue
    Say ('stale folder      : removed ' + $d.FullName)
}
Say 'done.'
exit 0
