inkmetrics - a desk device. What to do with this disk
======================================================

This disk IS the device. The device also presents itself as a network card: after
setup its page opens in a browser at the address shown on the device screen (line ADDR).

STEPS (once per computer)

1. The device is already plugged into USB - that is why you see this disk.
2. Double-click SETUP.CMD and confirm the User Account Control prompt.
   Windows never runs scripts from removable media by itself, so one double-click
   is always required.
3. The script installs a small agent (folder C:\ProgramData\inkmetrics) and enables
   Internet Connection Sharing on the device adapter. Later it keeps it enabled by
   itself - after a reboot and after network changes.
4. Look at the device screen: line ADDR is the page address. Open it in a browser:
   http://<address>/ - status; http://<address>/setup - settings.
   The address is usually 192.168.137.x.

FILES ON THIS DISK

    SETUP.CMD      double-click this
    AGENT.PS1      installs the agent and the scheduled task
    ICS.PS1        enable/disable sharing: -Off, -DryRun
    NETCHECK.PS1   check adapters, bridge, device, page, sharing
    READRU.TXT     same in Russian
    READEN.TXT     this file

SETTINGS ON THE DEVICE ITSELF (http://<address>/setup)

    * screen rotation (0/90/180/270) - how the device stands on your desk;
    * write-lock for this disk (read-only) - switched from the settings page;
    * ping target in the internet (ya.ru by default).

Troubleshooting

* Disk appears, but the device has no internet: run NETCHECK.PS1 and see whether
  sharing was enabled. The full PC guide (bridge or sharing) is
  docs/pc-setup-bridge.md in the project folder.
* The device screen shows NET EMERGENCY and address 192.168.7.1: sharing is not
  enabled yet. The device retries getting an address within a minute after SETUP.CMD,
  there is no need to replug it.
* Remove the agent: powershell -File C:\ProgramData\inkmetrics\AGENT.PS1 -Remove

The disk is readable and writable; the write lock is a device setting (when enabled,
Windows sees the disk as write protected).

> Device: Waveshare ESP32-S3-ePaper-1.54, firmware 0.3.2-idf.
