INSTRUCTIONS: the inkmetrics metrics agent (the device works as a monitor)

What this is
------------
Two command files that install and remove the agent on any Windows 10/11 PC:

    instagent.cmd   install the agent
    deinstall.cmd   remove the agent

The agent code travels INSIDE these files: nothing else is needed next to them, no
Python, no ESP-IDF, no drivers. They can be run from any folder - a USB stick, a
network share, the desktop.

The device works as a MONITOR: it has no internet, and internet sharing (ICS) is not
used in this set at all. That is deliberate - it keeps the whole scheme simple.

Addresses: always the same
--------------------------
    device (the ADDR line on its screen)  : 192.168.7.1
    this PC, device adapter               : 192.168.7.2/24

Nothing has to be looked up or negotiated: the addresses are fixed, the agent always
posts to 192.168.7.1, and the installer gives this PC 192.168.7.2.

Important: the address is set WITHOUT a gateway and with metric 9000. If Windows takes
a lease from the device together with the gateway 192.168.7.1, the host internet goes
into the device and this PC loses its internet. The installer does it right; if you set
the address by hand, do not fill in a gateway.

What gets installed
-------------------
    install folder : C:\ProgramData\inkmetrics
    files          : agent.ps1   the metrics agent (CPU, memory, disk, ping -> device)
                     net.ps1     the address on the device link (192.168.7.2, no gateway)
                     setup.ps1   helper: registers the task and starts the agent
                     agent.log   agent journal (written by the agent itself)
                     agent.pid   the agent process id
                     cpu.state   previous CPU sample (needed to compute percents)
    task           : "inkmetrics agent" - at startup, at logon and every minute, as SYSTEM
    Nothing is added to the registry, to startup or to services; no third-party software
    is installed. Internet sharing is not enabled.

Installation (5 steps)
----------------------
1. Connect the device to the PC with a USB cable (directly to a port, no hub).
2. Start instagent.cmd by double click. Windows asks for administrator rights - accept
   ("Yes"). The rights are needed once: for the adapter address and the task.
3. Wait for "set : 192.168.7.2/24, no gateway", "task inkmetrics agent : registered"
   and "Agent installed". The window stays open.
4. Check the device: press PWR until page HOST SYS (5/5). It shows CPU, memory, disk
   and the age of the data - the numbers must match this PC and the age must be tens
   of seconds.
5. Check on the PC (either):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\ProgramData\inkmetrics\agent.log

   The journal must get "sent ok (cpu=..%)" lines once a minute.

What the agent does
-------------------
Every 60 seconds the agent collects this PC's metrics and sends them to the device with
POST http://192.168.7.1/ingest. The device shows them on the HOST SYS page and tracks
their age: data older than 3 minutes is marked STALE.

Manual channel test (from a PowerShell window):

    curl.exe -s -X POST -H 'Content-Type: application/json' -d '{"cpu_percent":50.0}' http://192.168.7.1/ingest

An "ok" answer plus 50 on the device screen means the channel works and only the agent is
in question. Those numbers are a probe: the agent overwrites them with a real sample
within a minute.

Removal
-------
Run deinstall.cmd by double click (it asks for administrator rights too). It stops the
agent, removes the task "inkmetrics agent", gives the device adapter address back to
DHCP and deletes C:\ProgramData\inkmetrics. Nothing else is touched: shortcuts, the
registry, services and other network settings stay as they were.

Switches and variables
----------------------
    instagent.cmd --dry-run    change nothing: unpack into a temporary folder and show
                               what would be done
    instagent.cmd --keep-net   do not touch the adapter address (already configured)
    deinstall.cmd --dry-run    only report the state: task, process, folder
    deinstall.cmd --keep-net   do not give the address back to DHCP
    EINK_INSTALL_DIR=path      install into another folder instead of
                               C:\ProgramData\inkmetrics (used for testing)

A safe way to look around: instagent.cmd --dry-run, then deinstall.cmd --dry-run.

No metrics - what to check, in order
------------------------------------
1. Task: schtasks /query /tn "inkmetrics agent". If it is missing, the installation did
   not finish: run instagent.cmd again as administrator.
2. Journal: type C:\ProgramData\inkmetrics\agent.log
       "agent started ... sent ok"      - all good;
       "send error: ..."                - the agent runs but cannot reach the device:
                                          check the cable and the adapter address;
       no file at all                   - the agent never started (see item 1).
3. Agent file: type C:\ProgramData\inkmetrics\agent.ps1 - the first line must read
   "agent.ps1 - host monitoring agent". Any other text means an older kit overwrote it:
   remove it (deinstall.cmd) and install again.
4. Address: in a PowerShell window

       Get-NetIPAddress -AddressFamily IPv4 | Where-Object InterfaceAlias -like '*NDIS*'

   the device adapter must be 192.168.7.2 and must have no default route
   (Get-NetRoute -DestinationPrefix 0.0.0.0/0 - zero routes on it). A gateway there means
   the host internet goes into the device: remove the gateway, keep only the address.
5. Device: http://192.168.7.1/api/state, the ingest block (count and age). count must
   grow every minute, age must be seconds.
6. Device firmware: only 0.4.0 and newer accepts metrics (the HOST SYS page and the
   /ingest request). The version is shown on the device page and screen.

Requirements and limitations
----------------------------
* Windows 10 or 11, administrator rights during the installation.
* The device connected over USB; device firmware 0.4.0 or newer.
* The device has no internet by design: it is a monitor. This PC keeps its own internet
  because the address on the device link is set without a gateway.
* The scripts are ASCII on purpose: cmd.exe and PowerShell 5.1 mangle Russian text in
  .cmd/.ps1 files under the system codepage, so messages are English and the details
  live in this file.
* macOS will not bring the device network up without extra drivers; on Linux the scripts
  are Windows-only. This is a Windows tool.

What this set does NOT do
-------------------------
* It does not set up internet sharing for the device - there is none in this scheme.
* It does not repair the case when Windows shows no device disk: FIXDISK.PS1 in the
  project folder pc-setup does that.
* It does not run the detailed check of the agent, the device and the network with
  verdicts: that is CHECK.CMD from the same pc-setup folder.
* It does not touch the device firmware.

Files nearby
------------
This folder holds four files: the two .cmd files and the two instructions (Russian and
English). Exactly the same four files sit on the device disk, which Windows shows as an
ordinary removable drive - the agent can be installed right from it, nothing to copy.
