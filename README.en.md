# inkmetrics

[Русский](README.md) · **English**

<img src="media/panel-in-action.jpg" alt="The panel on a server, showing the temperatures of two GPUs" width="100%">

A **200×200 e-paper desk panel on an ESP32-S3** for a PC or a home server: CPU, RAM and disk load, GPU
load and temperatures, uptime, SMART status and whether the machine still has internet.

The whole link is **one USB cable**. The device presents itself to the host as a composite USB device
(an RNDIS network adapter plus a flash drive), draws the screen itself with its own fonts, and the PC
sends nothing but numbers once a minute — flat JSON to `POST /ingest` at `192.168.7.1`. No Wi-Fi, no
drivers, no internet access needed on the device.

The full story of the project (in Russian) is on Habr:
**[«Монитор на e-ink для сервера за выходные»](https://habr.com/ru/articles/1085984/)**.

## Why it exists

The server with two Tesla P100s sits in the kitchen, gets hot and hums, and there are exactly three
things worth knowing about it: whether a GPU temperature is approaching the throttling point, how much
memory is in use, and whether the SSD is running out of space. That is where the layout comes from:
the two big numbers are almost always GPU temperatures, everything else is small print.

## What it shows

| On the screen | Where it comes from |
|---|---|
| CPU load, memory, disk, uptime | stock Windows performance counters, sent by the agent once a minute |
| GPU load and temperatures | `nvidia-smi`, and where there is none — WDDM counters, the same ones Task Manager uses |
| Disk health | SMART: `OK` / `WARNING` / `UNHEALTHY` |
| PC internet | ping: `PING - 16MS - 4/4`, `PING - <ms>MS - TCP` (ICMP silent, port 443 open), `OFFLINE - 0/4` |
| Data freshness | 90 seconds of silence from the agent wipes the numbers to dashes `--`, and the internet line becomes `NO DATA` |

<img src="media/page-state.png" alt="The device state page in a browser" width="470">

There are two screens; a short press of PWR switches between them. The summary screen is built like
this: the internet line on top, two big numbers below it, then CPU, RAM and DISK percentages, uptime
and the sensor. The SETUP screen shows the web cabinet address, what the big numbers show, rotation,
disk mode, the internet check host and the firmware version. Buttons: a short PWR press switches the
screen, holding it redraws the frame; on SETUP a short BOOT press rotates the panel by 90°, holding it
locks writes to the device's disk.

## How it works, and why it works this way

* **The device draws the screen, not the PC.** In the first revision the PC assembled the 200×200
  frame: Pillow, the Consolas font, 5000 bytes over a COM port, the board just accepted a finished
  buffer. It was rewritten because a picture drawn on the PC has no second screen, no browser settings
  and no "no data" state, and every layout tweak dragged the PC-side image generator along with it.
  Now the layout lives in the firmware, fonts are generated (`tools/make_font.py`), and all that is
  left on the PC is a dumb number collector.
* **ESP-IDF instead of Arduino.** USB networking is impossible on the Arduino core: TinyUSB there is
  built without the network class. The project moved to ESP-IDF precisely so that the device can serve
  its own web page.
* **No drivers.** The ESP32-S3 has native USB: Windows itself brings up both the network adapter and
  the flash drive with the agent files. One caveat — the device adapter must not be given a gateway,
  otherwise Windows sends the default route there and the PC loses its internet.
* **GPU load on other people's cards — through WDDM.** On machines without `nvidia-smi` the load is
  read from the `\GPU Engine(*)\Utilization Percentage` counters, the same ones Task Manager shows;
  adapters are told apart by LUID and ordered by video memory so that index 0 is the discrete card.
  This path yields percentages only: the temperature and memory of a foreign card stay dashes rather
  than a nice-looking zero.
* **The font-size rule comes from measurement, not from taste.** If both big numbers are at most two
  digits long they are printed in the large font; if either is three digits, both drop to the medium
  one. A large `100%` takes 124 pixels out of 200, so two of them never fit.
* **The flash drive is no longer a "floppy".** At first the device disk held 2880 sectors — exactly
  1.44 MB, the geometry of a floppy, and Windows picks between the Disk and SFloppy classes by size:
  Explorer showed a drive letter `A:` and "Floppy Disk". The disk grew to 3.7 MB and is now a normal
  `INKMETRICS` volume.

<img src="media/panel-on-desk.jpg" alt="The panel on a desk next to a computer" width="420">

## Flashing the device (3 steps)

All you need is Python 3 and `esptool` — no ESP-IDF, no Arduino IDE, and no internet for the device.

```bat
pip install esptool

rem 1) put the device into bootloader mode: hold BOOT, plug in USB, keep it ~2 s, release
rem 2) flash firmware and disk image in one run (use your own port instead of COM5)
flash-kit\idf\flash.bat COM5
```

You can list the ports with `python -m serial.tools.list_ports -v`. `flash.bat` writes the bootloader,
the partition table, the firmware and the disk image — and at the end clears the bootloader flag itself
so the device starts without unplugging the cable.

The device then comes up on its own: metrics on the screen, an `INKMETRICS` disk (3.69 MB) and a
network adapter at `192.168.7.1` in Explorer. `instagent.cmd` is started from that disk (see "PC agent"
below); the details are in `flash-kit/idf/START-HERE.txt`.

**If something goes wrong**

| Symptom | What to do |
|---|---|
| `esptool not found` | `pip install esptool` |
| `Could not open COM5`, port not listed | Unplug and plug USB back in; the port number may change — list the ports again |
| The device did not start after flashing (blank screen, no disk, no network) | Unplug and plug USB back in: the device boots normally |
| The disk is there but `instagent.cmd` will not install | Run it as administrator |
| The device disappears after ~5 minutes (no disk, no network) | That is the self-check working: after 5 minutes of silence from the host the device goes into bootloader mode. Flash the kit again, or unplug and plug USB back in |

## Flashing without installing anything — straight from the browser

Open the flasher in Chrome or Edge: **<https://isemaster.github.io/inkmetrics/flash-web/>** — nothing to
download or install; the page fetches the images and flashes them over USB.

<img src="media/webflasher.png" alt="Web flasher: verifying files and connecting the device" width="520">

1. Put the device into bootloader mode: hold **BOOT**, plug in USB, keep it ~2 seconds, release.
2. **"Check files"** — the page verifies the sha256 of the images itself, then **"Connect device"** and
   pick the port (usually `COM5` or higher).
3. **"Flash"** — the firmware and the device disk are written, and the page reboots the device back
   into normal mode.

The same flasher lives in the repository (`flash-web/index.html`, to be served by any local web server
from the repository root: `python -m http.server 8765`, then `http://localhost:8765/flash-web/` — opening
the file directly does not work because it pulls the images from a sibling folder) and as a single file
in the release archive (`inkmetrics-webflash-*.zip`: unpack and open `inkmetrics-webflash.html` — the
images are embedded, no internet needed).

> **Honest note on verification.** The page opens, verifies the checksums, finds the device and opens the
> port — but **we could not complete the actual write on our board**, and we do not claim "it works"
> until we have. The reason is not the page: the ESP32-S3 bootloader here is USB-Serial/JTAG, and
> toggling DTR/RTS lines puts its port out of action until the USB cable is physically replugged (it
> reproduces with plain `esptool` from the command line). If you see the same, flash with the kit from
> the section above: its commands never touch DTR/RTS.

## The device's own web pages

Nothing to install: the pages are served by the device itself at `192.168.7.1`.

* **`/`** — state: device address, whether the PC has internet and what shows on the screen, temperature
  and humidity from the SHTC3 sensor, time, who opened the page, host metrics, GPU temperatures and
  memory, uptime and TCP connections, SMART, device disk size, rotation, firmware version. It also has
  links to enter bootloader mode and to reflash the device address.
* **`/setup`** — settings: screen rotation, the two big numbers (what to show on the left and on the
  right), the internet check host, and the read-only lock for the device disk. Settings live in NVS and
  survive reflashing.

<img src="media/page-setup.png" alt="Device settings page" width="330">

## PC agent

<img src="media/disk-agent-kit.png" alt="Contents of the device disk: instagent.cmd, deinstall.cmd, readme files" width="430">

1. Copy `agent-kit/` (or just `instagent.cmd` from the device flash drive) to the computer.
2. Run `instagent.cmd` **as administrator**: the agent goes to `C:\ProgramData\inkmetrics` and a
   scheduled task named "inkmetrics agent" runs it every minute as SYSTEM.
3. Check `C:\ProgramData\inkmetrics\agent.log` — once a minute a line like
   `sent ok (cpu=8.4%, ping YA.RU 16ms 4/4)`.
4. To remove it, run `deinstall.cmd` (it deletes the task, the process and the folder).

No Python, no drivers, no services: the agent is a single `.cmd` file with the code inside. The host to
ping is set on the device's `/setup` page and handed out to agents by the device.

## What is in the repository

| Folder | What it is |
|---|---|
| `idf/` | firmware (ESP-IDF 5.5): screen, device web pages, settings, disk, USB descriptors |
| `agent-kit/` | PC agent: `instagent.cmd` and `deinstall.cmd` (agent code inside the files) plus RU/EN instructions |
| `flash-kit/` | ready-to-use flashing kit for another computer: `idf/flash.bat`, images, `START-HERE.txt`, copies of the agent scripts |
| `flash-web/` | web flasher: a page for Chrome/Edge that flashes the device over USB with nothing installed |
| `media/` | images for this README (the device, its web pages, the web flasher) |
| `tools/` | build and generators: fonts, disk image, transfer kit, screen layout preview stand, diagnostics |
| `docs/` | wire protocol, pinout, addressing, screen layout, release notes |

Not in the repository (see `.gitignore`): build directories, downloaded IDF components, the source disk
image `firmware/media/*.img` (a ready copy ships in the kit), `flash-kit-*.zip` archives and the
project's working notes (`MEMORY.md`, `ISSUES.md`, `NEXT-SESSION.md`).

## Building from source (for anyone changing the code)

You need **ESP-IDF 5.5** (`idf.py`). The order is:

```bash
idf.py -C idf build              # firmware → idf/build/inkmetrics_idf.bin
python tools/make_setup_disk.py  # device disk image: agent scripts and readmes (FAT)
python tools/make_flash_kit.py   # flash-kit/: images, flash.bat, START-HERE.txt
```

Screen layouts are not edited by eye: first a mock on the preview stand (`tools/preview`), then the
firmware. Changing the layout or a font size is a reason to bump the version in `idf/main/main.c`.

Flashing during development: `python tools/try_flash.py idf/build` — it waits for the port, writes the
firmware together with the disk image and clears the bootloader flag itself (the device can be put into
bootloader mode with a `GET /api/boot` request, no need to press BOOT).

## FAQ

* **Do I need the Arduino IDE?** No. The firmware is built with ESP-IDF and flashed with the kit
  (`flash.bat`) through `esptool`: no Arduino IDE, no `esp32` board package, no display libraries. An
  Arduino branch existed in early versions of the project and was removed — it worked over a COM port
  and could not do what the current one does (USB networking and receiving metrics from the agent).
* **Does the device need internet?** No. Power and data are one USB cable; internet is only needed to
  show whether the PC has it (ping).
* **Are drivers needed?** No: the ESP32-S3 has native USB, and Windows brings up both the network
  adapter and the flash drive with the agent files on its own.
* **Can I reflash without pressing BOOT?** Yes, if the device is already running: `GET /api/boot` (the
  device page) or `python tools/try_flash.py idf/build`.
* **What if I would rather not install Python?** The ready kit can be flashed without it using
  Espressif's **Flash Download Tool**: write `flash-kit/idf/inkmetrics-fw-0x0.bin` at address `0x0` and
  the disk image `setup-disk-big.img` at `0x430000`. That is exactly what `flash.bat` does, only with
  buttons.
* **How do I change the screen layout?** First a mock on the preview stand (`tools/preview`), then build
  and flash — see "Building from source" above.

## How it was made

The panel was written over a weekend and has reached version 0.7.1. An AI agent (Hermes) wrote the code
while a human set the tasks, complained about the results and verified everything on real hardware:
**18 hours** of work on the firmware, twelve sessions, 75 attempts, 4767 messages, 2407 tool calls. A
local Qwen3-27B read 2.1 million input tokens and wrote just 168 thousand — it read far more than it
wrote (panic backtraces, IDF sources, logs), so none of it cost anything but electricity.

The development loop ran almost hands-free: the device stayed plugged into the work computer over USB,
the agent flashed it itself (`GET /api/boot` puts the device into bootloader mode, `tools/try_flash.py`
writes the image and clears the flag), the boot counter in the device's "black box" log passed a
thousand within the first day, and the BOOT button was pressed maybe five times. The cable was only
unplugged when the USB port died after yet another experiment with bootloader modes.

## What is not there yet

* over-the-air firmware updates — although the partition already has room for a second slot: the
  application takes 642 KB out of 3.2 MB, with a spare megabyte next to it;
* a Wi-Fi access point, so readings could be checked from a phone;
* a case — the board just sits on a stand as it is.

## Hardware

Waveshare **ESP32-S3-ePaper-1.54** (200×200 panel), an SHTC3 temperature and humidity sensor, USB for
both power and data. Pinout and addressing: `docs/pins.md`, `docs/addressing.md`.

## Documentation

* `docs/protocol.md` — the `POST /ingest` protocol and the metric fields;
* `docs/screens-v7.md` + `docs/screens-v7.png` — the layout of both screens and the font sizes;
* `docs/slots-2026-09-23.md` — the configurable big numbers (two slots on the `/setup` page);
* `docs/release-0.7.0.md`, `docs/release-0.7.1.md` — releases: what is in them and what was verified;
* `docs/notify-plan.md` — draft: showing arbitrary text sent by smart-home devices;
* `idf/README.md` — build, diagnostics, panic decoding, working with the "black box" log.

## Licence

The project's code is released under the **PolyForm Noncommercial License 1.0.0** — full text in the
`LICENSE` file, original at <https://polyformproject.org/licenses/noncommercial/1.0.0>.

In short:

* **Personal use is free.** Personal use, hobby, study, experiments, modifications for your own needs,
  passing copies on to other people at no charge. The software comes as is, without warranty.
* **Selling requires permission.** Commercial use (selling devices with this firmware, selling the
  firmware or the kit, using it in a paid product or service) is not permitted by the licence. Terms
  are agreed separately — write to **isemaster+inkmetrics@gmail.com** (see `COMMERCIAL.md`).
* **What to pass on with a copy.** The licence text (or a link to it) and the notice line:
  `Required Notice: Copyright isemaster (https://github.com/isemaster/inkmetrics)`.
* **Third-party components** (ESP-IDF, TinyUSB, esptool-js, the panel driver, fonts) remain under their
  own terms — see `THIRD-PARTY-NOTICES.md`; this licence does not cover them.
