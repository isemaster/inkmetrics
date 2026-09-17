INSTRUCTIONS: installing the inkmetrics agent on a PC

What this is
------------
Two command files that install and remove the agent on any Windows 10/11 PC:

    instagent.cmd   install the agent (metrics + internet sharing for the device)
    deinstall.cmd   remove the agent

The agent code travels INSIDE these files: nothing else is needed next to them,
no Python, no ESP-IDF, no drivers. They can be run from any folder - a USB stick,
a network share, the desktop.

What gets installed
-------------------
    install folder : C:\ProgramData\inkmetrics
    files          : agent.ps1   the metrics agent (CPU, memory, disk, ping -> device)
                     ics.ps1     internet sharing (ICS) for the device
                     setup.ps1   helper: registers the tasks and starts everything
                     agent.log   agent journal (written by the agent itself)
                     agent.pid   the agent process id
                     cpu.state   previous CPU sample (needed to compute percents)
    tasks          : "inkmetrics agent" - every minute, as SYSTEM
                     "inkmetrics ICS"   - at startup, at logon, every minute
    Nothing is added to the registry, to startup or to services; no third-party
    software is installed.

Installation (5 steps)
----------------------
1. Connect the device to the PC with a USB cable (directly to a port, no hub).
2. Start instagent.cmd by double click. Windows asks for administrator rights -
   accept ("Yes"). The rights are needed once, to register the tasks.
3. Wait for the lines "task ... registered" and "Agent installed". The window
   stays open, so you can read it calmly.
4. Check the device: press PWR until page HOST SYS (5/5). It shows CPU, memory,
   disk and the age of the data - the numbers must match this PC and the age must
   be tens of seconds.
5. Check on the PC (either):

       schtasks /query /tn "inkmetrics agent" /v /fo LIST
       type C:\ProgramData\inkmetrics\agent.log

   The journal must get "sent ok (cpu=..%)" lines once a minute.

What the agent does
-------------------
Every 60 seconds the agent collects this PC's metrics and sends them to the device
with POST http://192.168.7.1/ingest. The device shows them on the HOST SYS page and
tracks their age: data older than 3 minutes is marked STALE.

Manual channel test (from a PowerShell window):

    curl.exe -s -X POST -H "Content-Type: application/json" -d "{\"cpu_percent\":50.0}" http://192.168.7.1/ingest

An "ok" answer plus 50 on the device screen means the channel works and only the
agent is in question.

Internet sharing (ICS) - what you must know
-------------------------------------------
Together with the agent, internet sharing is enabled for the device adapter: the
device (and anything behind it) goes online through this PC. Windows drops sharing
often (network change, reboot), so a second task "inkmetrics ICS" brings it back
every minute.

IMPORTANT, known limitation: while sharing is on, the device adapter becomes
192.168.137.1 and the device moves into the 192.168.137.x subnet, while the metrics
agent still posts to 192.168.7.1 - in that mode metrics do NOT arrive (the device
does have internet). Until this is fixed, pick one:

    * you want metrics   - turn sharing off: ICS.PS1 -Off (from the pc-setup
                           folder), or clear "Allow other users..." in the
                           properties of the device adapter;
    * you want internet  - keep sharing on and do not expect metrics.

Removal
-------
Run deinstall.cmd by double click (it asks for administrator rights too). It stops
the agent, removes both tasks, turns sharing off and deletes
C:\ProgramData\inkmetrics. Nothing else on the PC is touched: shortcuts, the
registry, services and other network settings stay as they were.

Switches and variables
----------------------
    instagent.cmd --dry-run    change nothing: unpack into a temporary folder and
                               show what would be done
    deinstall.cmd --dry-run    only report the state: tasks, process, folder
    EINK_INSTALL_DIR=path      install into another folder instead of
                               C:\ProgramData\inkmetrics (used for testing)

A safe way to look around: instagent.cmd --dry-run, then deinstall.cmd --dry-run.

No metrics - what to check, in order
------------------------------------
1. Tasks: schtasks /query /tn "inkmetrics agent" - if the task is missing, the
   installation did not finish; run instagent.cmd again (as administrator).
2. Journal: type C:\ProgramData\inkmetrics\agent.log
       "agent started ... sent ok"      - all good;
       "send error: ..."                - the agent runs but cannot reach the
                                          device: check the cable and the device
                                          address (ADDR line on its screen);
       no file at all                   - the agent never started (see item 1).
3. Agent file: type C:\ProgramData\inkmetrics\agent.ps1 - the first line must
   read "agent.ps1 - host monitoring agent". If it is text about installing
   sharing, an older kit overwrote the agent: remove it (deinstall.cmd) and
   install again with this file.
4. Device: http://192.168.7.1/api/state, the ingest block (count and age). count
   must grow every minute, age must be seconds.
5. Device firmware: only 0.4.0 and newer accepts metrics (the HOST SYS page and
   the /ingest request). The version is shown on the device page and screen.

Requirements and limitations
----------------------------
* Windows 10 or 11, administrator rights during the installation.
* The device connected over USB; device firmware 0.4.0 or newer.
* The scripts are ASCII on purpose: cmd.exe and PowerShell 5.1 mangle Russian
  text in .cmd/.ps1 files under the system codepage, so messages are English and
  the details live in this file.
* macOS will not bring the device network up without extra drivers; on Linux the
  scripts are Windows-only. This is a Windows tool.
* The agent knows the device address 192.168.7.1. If the device got another
  address (for example 192.168.137.x with sharing on), metrics will not arrive -
  see the ICS section above.

Files nearby
------------
This folder holds only these two .cmd files and the two instructions (Russian and
English). A fuller PC setup set lives in the project folder pc-setup (it also has
CHECK.CMD - a detailed check of the agent, the device and the network - and
FIXDISK.PS1, which repairs Windows not showing the device disk).
