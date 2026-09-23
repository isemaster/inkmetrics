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
4. Check the device: the top line must read PING (this PC has internet), below it two large
   numbers (by default CPU load and GPU load; what to show is chosen on the device setup
   page, see "What the device shows"), then CPU / RAM / DISK percentages and uptime. A
   short PWR press switches to the SETUP screen and back. The numbers must match this PC;
   NO DATA in the top line means the agent is not sending (see step 5).
5. Check on the PC (either):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\ProgramData\inkmetrics\agent.log

   The journal must get "sent ok (cpu=..%)" lines once a minute.

What the device shows
---------------------
The top line: "PING - <ms> - <answers out of four>" when this PC has internet (for instance
"PING - 15MS - 4/4"); "OFFLINE - 0/4" when the pings went unanswered; NO DATA - no metrics
for 90 seconds, the numbers turn into dashes.

The node used for the internet check is chosen in the device settings (page
http://192.168.7.1/setup, the field "the node for the internet check"): the agent reads it
from the device once a minute, so a single setting serves every PC and no files have to be
edited on each machine. The agent sends four pings and shows the average of the answers; if
none of them came back it also checks a TCP connect to port 443 of the same node (providers
often ignore pings while the port stays open) and prints TCP instead of the
count. With the field empty or the device unreachable the fallback is 8.8.8.8.

Below the top line sit two large numbers. What they show is chosen on the device setup page:
open http://192.168.7.1/setup in a browser on this PC and pick from the lists "Крупное
число слева" and "Крупное число справа" (the large number, left and right). The choices
are CPU %, RAM %, DISK %, GPU0 %, GPU1 %, "GPU: second, else first", GPU0 °C, GPU1 °C and
"empty" (show nothing). By default the left one is CPU load and the right one is GPU load:
with two cards the second card is shown, with a single card the card itself. The choice is
kept in the device and survives a reboot.

Under them: CPU / RAM / DISK percentages, PC uptime, and the device's own temperature and
humidity. When a value has no source (for instance a GPU temperature without the NVIDIA
driver) the device shows a dash, not a zero.

A short PWR press switches between the summary screen and SETUP. SETUP shows the device
address, the SHOW line (what is in the large numbers), the PING NODE line (the node used for
the internet check) and the firmware version. Firmware 0.6.2 or newer.

What the agent does
-------------------
Every 60 seconds the agent collects this PC's metrics and sends them to the device with
POST http://192.168.7.1/ingest: CPU load, memory, disk, temperature and load of every
NVIDIA card, uptime. Separately the agent checks this PC's internet access: the node comes
from the device settings, four pings are sent and a TCP 443 check is made when ICMP stays
silent. That answer is what the device prints in the top line as "PING - <ms> - <answers>" or
"OFFLINE". The agent log shows the same: "sent ok (cpu=..%, ping YA.RU 15ms 4/4)".

When no metrics arrive for 90 seconds the device shows NO DATA and replaces the numbers
with dashes: it will not pass old readings off as current ones.

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
6. Device firmware: only 0.5.0 and newer accepts metrics (the summary screen and the
   /ingest request); the two large numbers on the summary screen are configurable from
   0.6.0 on. The version is shown on the device page and on its SETUP screen.

Requirements and limitations
----------------------------
* Windows 10 or 11, administrator rights during the installation.
* The device connected over USB; device firmware 0.5.0 or newer (the large numbers on the
  summary screen are configurable from 0.6.0).
* GPU load comes from nvidia-smi, or - when it is not installed - from the Windows
  "GPU Engine" counters (any vendor, Windows 10 1709+). GPU temperatures exist only where
  nvidia-smi is installed; CPU temperature is not reported at all: Windows has no stock
  source for it.
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
