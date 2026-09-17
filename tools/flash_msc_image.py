"""Write the host-disk image into the device `msc` partition (the chip must be in the ROM
bootloader: hold BOOT, plug USB while holding, ~2 s, release).

Why this tool exists: the "flash drive" the device shows the host is not a real card - it is
a flash partition served over USB MSC. Whatever files must appear on that drive have to be
written into the partition, and the partition table is in idf/partitions.csv.

Why the MBR image matters: a volume without a partition table makes Windows treat the whole
device as a "superfloppy" and bind the floppy driver (USBSTOR\\SFloppy..., service sfloppy,
letter A:, shown as "Diskette (A:)"). The INQUIRY removable bit was already forced to 0 in
idf/main/msc.c and it did not change that. firmware/media/setup-disk.img carries a real MBR
plus a FAT12 partition (type 0x01) with SETUP.CMD, AGENT.PS1, ICS.PS1, NETCHECK.PS1 and the
readme files - that is the layout a normal flash drive has.

  python tools/flash_msc_image.py --list                  # ports only
  python tools/flash_msc_image.py                          # backup, write, verify
  python tools/flash_msc_image.py --image <path>           # use another image
  python tools/flash_msc_image.py --restore <backup.bin>   # put a backup back

Run it with an interpreter that has esptool, e.g.
  C:/Users/user/.espressif/python_env/idf5.5_py3.14_env/Scripts/python.exe
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

from serial.tools import list_ports

ROOT = Path(__file__).resolve().parent.parent
CSV = ROOT / "idf" / "partitions.csv"
DEFAULT_IMAGE = ROOT / "firmware" / "media" / "setup-disk-big.img"
BACKUP_DIR = ROOT / "firmware" / "media"

VID_ESPRESSIF = 0x303A
PID_BOOTLOADER = 0x1001


def find_msc_partition() -> tuple[int, int]:
    """(offset, size) of the msc partition, read from partitions.csv."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import partitions                                    # noqa: E402

    return partitions.msc_partition(CSV)


def find_port() -> str | None:
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid == VID_ESPRESSIF and p.pid == PID_BOOTLOADER:
            return p.device
    for p in ports:
        desc = (p.description or "") + " " + (p.product or "")
        if p.vid == VID_ESPRESSIF or "JTAG" in desc or "USB Serial" in desc:
            return p.device
    return None


def show_ports() -> None:
    for p in list_ports.comports():
        vid = f"{p.vid:04X}:{p.pid:04X}" if p.vid is not None else "-"
        print(f"  {p.device:8} {vid:10} {p.description}")
    print("ROM bootloader port:", find_port() or "NOT FOUND")


def esptool(port: str, args: list[str]) -> int:
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--baud", "921600", "--before", "default_reset", "--after", "no_reset"] + args
    print("[esptool] " + " ".join(cmd[2:]))
    return subprocess.run(cmd).returncode


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="COM port (otherwise the ROM port is detected)")
    ap.add_argument("--image", default=str(DEFAULT_IMAGE), help="image to write")
    ap.add_argument("--restore", help="write this backup back into the partition")
    ap.add_argument("--list", action="store_true", help="show ports and exit")
    args = ap.parse_args()

    if args.list:
        show_ports()
        return 0

    offset, size = find_msc_partition()
    print(f"msc partition: offset 0x{offset:x}, size 0x{size:x} ({size // 1024} KB)")

    port = args.port or find_port()
    if not port:
        print("ROM bootloader port not found - hold BOOT, plug USB while holding, release.")
        show_ports()
        return 2
    print("port:", port)

    if args.restore:
        src = Path(args.restore)
        if not src.exists():
            raise SystemExit(f"backup not found: {src}")
        data = src.read_bytes()
        print(f"restoring {src.name}: {len(data)} bytes")
        rc = esptool(port, ["write_flash", hex(offset), str(src)])
        return rc

    img = Path(args.image)
    if not img.exists():
        raise SystemExit(f"image not found: {img}")
    data = img.read_bytes()
    if len(data) > size:
        raise SystemExit(f"image {len(data)} bytes does not fit into {size} bytes")

    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    backup = BACKUP_DIR / ("msc-backup-" + time.strftime("%Y%m%d-%H%M%S") + ".bin")
    print(f"1/3 backup 0x{offset:x}..0x{offset + size:x} -> {backup.name}")
    rc = esptool(port, ["read_flash", hex(offset), hex(size), str(backup)])
    if rc != 0:
        print("backup failed - nothing written")
        return rc

    print(f"2/3 writing {img.name} ({len(data)} bytes, {len(data) // 512} sectors)")
    rc = esptool(port, ["write_flash", hex(offset), str(img)])
    if rc != 0:
        print("write failed")
        return rc

    print("3/3 verifying")
    rc = esptool(port, ["verify_flash", hex(offset), str(img)])
    print("verify:", "OK" if rc == 0 else f"FAILED ({rc})")
    print("now replug USB so the device leaves the bootloader and starts the firmware")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
