inkmetrics - a desk device. What to do with this disk
======================================================

This disk IS the device. The device also presents itself as a network card: its page
opens in a browser at the address shown on the device screen (line ADDR).

STEPS (once per computer)

1. The device is already plugged into USB - that is why you see this disk.
2. Double-click SETUP.CMD and confirm the User Account Control prompt
   (Windows never runs scripts from removable media by itself).
3. The script does two things:
   * installs the metrics agent - once a minute it sends the device this PC's CPU load,
     memory, disk, GPU, temperatures, ping, TCP connection count and SMART state; the
     scheduled task "inkmetrics agent" runs it every minute as SYSTEM;
   * enables Internet Connection Sharing on the device adapter.
4. Where to look:
   * on the device screen - page 5/5 "HOST SYS" (PWR cycles pages): CPU, RAM, disk,
     GPU, temperatures, TCP, host uptime and the age of the data (AGE);
   * in a browser at the address from the screen (line ADDR): rows "Метрики хоста
     (агент)" and "Хост: пинг, TCP, SMART" on the device page.

FILES ON THIS DISK

    SETUP.CMD      double-click this
    MINSTALL.PS1   installs the metrics agent and its scheduled task
    METRICS.PS1    the agent itself (the installer copies it to C:\ProgramData\inkmetrics)
    AGENT.PS1      internet sharing setup for the device (ICS)
    ICS.PS1        enable/disable sharing: -Off, -DryRun
    NETCHECK.PS1   check adapters, bridge, device, page, sharing
    READRU.TXT     same in Russian
    READMEEN.TXT   this file

WHAT APPEARS IN THE SYSTEM AFTER SETUP.CMD

    C:\ProgramData\inkmetrics\
        agent.ps1        the metrics agent (working copy the scheduler runs)
        METRICS.PS1      the agent's source from this disk
        AGENT.PS1, ICS.PS1, NETCHECK.PS1   setup and diagnostics scripts
        agent.log        the agent's log - look here if metrics are missing
        agent.pid, cpu.state               the agent's own state files

    Scheduled tasks:
        "inkmetrics agent"  - metrics, every minute, as SYSTEM
        "inkmetrics ICS"    - internet sharing, at startup, logon and every minute

AGENT SETTINGS (top of C:\ProgramData\inkmetrics\agent.ps1)

    $DeviceIP    device address (192.168.7.1 by default - the ADDR line on its screen)
    $Interval    send period in seconds (60)
    $PingTarget  internet ping target (8.8.8.8)
    $HostName    the name the device shows next to AGE (this PC's name by default)

REMOVE

    MINSTALL.PS1 -Remove   remove the metrics agent (administrator rights required)
    AGENT.PS1 -Remove      remove internet sharing and disable ICS

Both scripts delete the shared folder C:\ProgramData\inkmetrics entirely (log included), so
the order does not matter - or just run SETUP.CMD again when you need the setup back.

CHECKING THAT METRICS FLOW

    schtasks /query /tn "inkmetrics agent" /v /fo LIST
    Get-Content C:\ProgramData\inkmetrics\agent.log -Tail 10

The log should show "sent ok (cpu=...)" about once a minute, and on the device the data
age (AGE) stays under a minute. If AGE grows and shows STALE, the agent is not running:
check the task and the log.

SETTINGS ON THE DEVICE ITSELF (http://<address>/setup)

    * screen rotation (0/90/180/270) - how the device stands on your desk;
    * write-lock for this disk - then Windows sees it as write protected;
    * ping target in the internet (ya.ru by default).

Troubleshooting

* While internet sharing (ICS) is not enabled, the device hands this PC a lease whose
  gateway is the device itself (192.168.7.1): Windows may route the whole internet there and
  the connection drops. Enabling sharing (SETUP.CMD) or unplugging the device fixes it.
  Details: docs/ingest-2026-09-17.md.
* Disk appears, but the device page does not open: run NETCHECK.PS1 - it shows whether
  internet sharing was enabled. Full PC guide: docs/pc-setup-bridge.md.
* The device screen shows NET EMERGENCY and address 192.168.7.1: sharing is not enabled
  yet; the device asks for an address again within a minute, no replug needed.
* Metrics are missing (N/A on screen, AGE growing): check the agent log and the task, and
  that the device answers: ping 192.168.7.1 and http://192.168.7.1/api/state
  (the reply must contain the "ingest" object).
* Remove the metrics agent: MINSTALL.PS1 -Remove (deletes the task and the folder).

* After sharing is enabled the device address changes (usually 192.168.137.x; see the ADDR
  line on its screen). If the agent then cannot find the device, set `$DeviceIP` in
  `C:\ProgramData\inkmetrics\agent.ps1` to that address (known gap З-40 in the project's
  ISSUES.md).

The disk is readable and writable; the write lock is a device setting.

> Device: Waveshare ESP32-S3-ePaper-1.54, firmware 0.4.2-idf.
