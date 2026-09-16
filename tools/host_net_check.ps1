# RNDIS adapter check (device on USB). ASCII only on purpose: PowerShell 5.1 reads
# .ps1 in the system codepage, and Russian text here breaks parsing.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools/host_net_check.ps1
$ad = Get-NetAdapter | Where-Object { $_.InterfaceDescription -like '*Remote NDIS*' }
if (-not $ad) { Write-Host "no RNDIS adapter found (device not attached or USB stack down)"; exit 1 }

$ix = $ad.ifIndex
Write-Host ("Adapter: {0} (ifIndex {1}), status {2}, MAC {3}" -f $ad.Name, $ix, $ad.Status, $ad.MacAddress)

Write-Host "`n--- IPv4 on adapter ---"
Get-NetIPAddress -InterfaceIndex $ix -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Select-Object IPAddress, PrefixLength, PrefixOrigin, SuffixOrigin | Format-Table -AutoSize

Write-Host "--- DHCP / state / MTU ---"
Get-NetIPInterface -InterfaceIndex $ix -AddressFamily IPv4 |
    Select-Object Dhcp, ConnectionState, NlMtu, InterfaceMetric | Format-List

Write-Host "--- routes on adapter ---"
Get-NetRoute -InterfaceIndex $ix -ErrorAction SilentlyContinue |
    Select-Object DestinationPrefix, NextHop, RouteMetric | Format-Table -AutoSize

Write-Host "--- device web page ---"
try {
    $r = Invoke-WebRequest -Uri 'http://192.168.7.1/api/state' -TimeoutSec 6 -UseBasicParsing
    Write-Host ("HTTP {0}: {1}" -f $r.StatusCode, $r.Content)
} catch {
    Write-Host ("HTTP no answer: {0}" -f $_.Exception.Message)
}
