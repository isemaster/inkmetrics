"""Build the disk image the device serves to the host: MBR + one FAT16 partition.

Why a bigger disk: the device used to report exactly 2880 sectors (1.44 MB) - a floppy
geometry. Windows answers "Disk or SFloppy" for a direct-access SCSI device
(learn.microsoft.com/windows-hardware/drivers/install/identifiers-generated-by-usbstor-sys)
and picked SFloppy, binding the floppy driver: letter A:, shown as "Diskette", and with the
removable bit cleared in INQUIRY it reports no media at all ("device not ready"). A disk of
a normal size, with an MBR and a real FAT16 partition, has none of that to match.

Files are taken from an existing image (setup-disk.img, FAT12 with MBR) so the drive keeps
the same content: SETUP.CMD, AGENT.PS1, ICS.PS1, NETCHECK.PS1 and the readme files.

  python tools/make_disk_image.py --sectors 7552 --out firmware/media/setup-disk-big.img
  python tools/make_disk_image.py --show firmware/media/setup-disk-big.img

Sector count must match MSC_SECTORS in idf/main/msc.c and the msc partition size
(idf/partitions.csv) - the device serves exactly that many sectors.
"""
from __future__ import annotations

import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_IMAGE = ROOT / "firmware" / "media" / "setup-disk.img"

SECTOR = 512
ROOT_ENTRIES = 512
FAT_TYPE = b"FAT16   "
VOLUME_LABEL = b"INKMETRICS    "
VOLUME_ID = 0x1A2B3C4D


# --------------------------------------------------------------- reading the source image

def read_fat12_volume(img: bytes, part_start: int) -> list[tuple[str, bytes]]:
    """Files from a FAT12 volume (the source image is FAT12 inside an MBR partition)."""
    off = part_start * SECTOR
    bs = img[off:off + SECTOR]
    reserved = struct.unpack_from("<H", bs, 14)[0]
    nfats = bs[16]
    fat_sectors = struct.unpack_from("<H", bs, 22)[0]
    root_sec = (struct.unpack_from("<H", bs, 17)[0] * 32 + SECTOR - 1) // SECTOR
    data_start = reserved + nfats * fat_sectors + root_sec

    def fat12(cl: int) -> int:
        o = off + reserved * SECTOR + cl + cl // 2
        if cl & 1:
            return (img[o] >> 4) | (img[o + 1] << 4)
        return img[o] | ((img[o + 1] & 0x0F) << 8)

    root = off + (reserved + nfats * fat_sectors) * SECTOR
    out: list[tuple[str, bytes]] = []
    for i in range(struct.unpack_from("<H", bs, 17)[0]):
        ent = img[root + i * 32: root + i * 32 + 32]
        if ent[0] in (0x00, 0xE5):
            break
        if ent[11] & 0x08:                       # volume label
            continue
        name = ent[:8].decode("ascii", "replace").rstrip() + "." + \
               ent[8:11].decode("ascii", "replace").rstrip()
        first = struct.unpack_from("<H", ent, 26)[0]
        size = struct.unpack_from("<I", ent, 28)[0]
        data = bytearray()
        cl, guard = first, 0
        while 2 <= cl < 0xFF8 and guard < 100000:
            start = off + (data_start + cl - 2) * SECTOR
            data += img[start:start + SECTOR]
            cl = fat12(cl)
            guard += 1
        out.append((name, bytes(data[:size])))
    return out


# --------------------------------------------------------------- building the new image

def build(total_sectors: int, files: list[tuple[str, bytes]]) -> bytes:
    part_start = 1
    part_sectors = total_sectors - part_start

    # FAT16 geometry: keep solving until the FAT is large enough for its own cluster count.
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR      # 32
    fat_sectors = 1
    while True:
        clusters = part_sectors - 1 - 2 * fat_sectors - root_sectors
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need <= fat_sectors:
            break
        fat_sectors = need
    data_start = 1 + 2 * fat_sectors + root_sectors
    max_clusters = part_sectors - data_start

    if max_clusters < 4085:
        raise SystemExit(f"{total_sectors} sectors is too small for FAT16 "
                         f"({max_clusters} clusters, need 4085+)")

    used = sum((len(c) + SECTOR - 1) // SECTOR or 1 for _, c in files)
    if used > max_clusters:
        raise SystemExit(f"files need {used} clusters, volume has {max_clusters}")

    img = bytearray(total_sectors * SECTOR)
    vol = part_start * SECTOR

    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = 1                                   # sectors per cluster
    struct.pack_into("<H", bs, 14, 1)            # reserved sectors
    bs[16] = 2                                   # number of FATs
    struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bs, 19, 0)            # 16-bit sector count: 0 -> use 32-bit
    bs[21] = 0xF8                                # fixed media, as on a real flash drive
    struct.pack_into("<H", bs, 22, fat_sectors)
    struct.pack_into("<H", bs, 24, 63)           # sectors per track (nominal)
    struct.pack_into("<H", bs, 26, 255)          # heads (nominal)
    struct.pack_into("<I", bs, 28, part_start)   # hidden sectors
    struct.pack_into("<I", bs, 32, part_sectors)  # 32-bit total sectors
    bs[36] = 0x80                                # drive number
    bs[38] = 0x29                                # extended boot signature
    struct.pack_into("<I", bs, 39, VOLUME_ID)
    bs[43:54] = VOLUME_LABEL
    bs[54:62] = FAT_TYPE
    bs[510:512] = b"\x55\xAA"
    img[vol:vol + SECTOR] = bs

    # FAT16 table: two copies, entries 0 and 1 reserved, then the file chains
    fat = bytearray(fat_sectors * SECTOR)
    struct.pack_into("<H", fat, 0, 0xFFF8)
    struct.pack_into("<H", fat, 2, 0xFFFF)
    root = bytearray(root_sectors * SECTOR)
    next_cluster = 2
    for idx, (name, content) in enumerate(files):
        base, _, ext = name.upper().partition(".")
        short = base.ljust(8)[:8].encode("ascii") + ext.ljust(3)[:3].encode("ascii")
        n = max(1, (len(content) + SECTOR - 1) // SECTOR)
        for i in range(n):
            cl = next_cluster + i
            struct.pack_into("<H", fat, cl * 2, 0xFFFF if i == n - 1 else cl + 1)
            start = vol + (data_start + cl - 2) * SECTOR
            img[start:start + SECTOR] = content[i * SECTOR:(i + 1) * SECTOR].ljust(SECTOR, b"\x00")
        off = idx * 32
        root[off:off + 11] = short
        root[off + 11] = 0x20
        struct.pack_into("<H", root, off + 26, next_cluster)
        struct.pack_into("<I", root, off + 28, len(content))
        next_cluster += n

    for i in range(2):
        start = vol + (1 + i * fat_sectors) * SECTOR
        img[start:start + len(fat)] = fat
    root_start = vol + (1 + 2 * fat_sectors) * SECTOR
    img[root_start:root_start + len(root)] = root

    # MBR
    e = 0x1BE
    img[e + 0] = 0x00
    img[e + 1:e + 4] = bytes([0x00, 0x01, 0x01])
    img[e + 4] = 0x06                            # FAT16, 32 MB or less
    img[e + 5:e + 8] = bytes([0xFE, 0x3F, 0x20])
    struct.pack_into("<I", img, e + 8, part_start)
    struct.pack_into("<I", img, e + 12, part_sectors)
    img[510:512] = b"\x55\xAA"
    return bytes(img)


# --------------------------------------------------------------- checking the result

def read_root_entries(img: bytes) -> list[tuple[str, int]]:
    """(имя, размер) файлов из корневого каталога тома в образе — для проверки сборки.

    Размер берётся из записи каталога, поэтому цепочки FAT здесь не нужны.
    """
    bs = img[:SECTOR]
    pstart = struct.unpack_from("<I", bs, 0x1BE + 8)[0] if bs[0x1BE + 4] else 0
    vol = pstart * SECTOR
    vbs = img[vol:vol + SECTOR]
    reserved = struct.unpack_from("<H", vbs, 14)[0]
    nfats = vbs[16]
    fat_sectors = struct.unpack_from("<H", vbs, 22)[0]
    entries = struct.unpack_from("<H", vbs, 17)[0]
    root = vol + (reserved + nfats * fat_sectors) * SECTOR
    out: list[tuple[str, int]] = []
    for i in range(entries):
        ent = img[root + i * 32: root + i * 32 + 32]
        if ent[0] in (0x00, 0xE5):
            break
        if ent[11] & 0x08:
            continue
        name = ent[:8].decode("ascii", "replace").rstrip() + "." + \
               ent[8:11].decode("ascii", "replace").rstrip()
        out.append((name, struct.unpack_from("<I", ent, 28)[0]))
    return out


def read_file(img: bytes, name: str) -> bytes | None:
    """Прочитать файл из тома в образе — целиком, побайтово (проверка сборки).

    Один размер из записи каталога ничего не доказывает: файл может оказаться в образе
    обрезанным или собранным из чужих кластеров. Поэтому идём по цепочке FAT16 и сверяем
    содержимое с исходником. Геометрию берём из загрузочного сектора тома, а не константами.
    """
    bs = img[:SECTOR]
    pstart = struct.unpack_from("<I", bs, 0x1BE + 8)[0] if bs[0x1BE + 4] else 0
    vol = pstart * SECTOR
    vbs = img[vol:vol + SECTOR]
    reserved = struct.unpack_from("<H", vbs, 14)[0]
    nfats = vbs[16]
    fat_sectors = struct.unpack_from("<H", vbs, 22)[0]
    entries = struct.unpack_from("<H", vbs, 17)[0]
    spc = vbs[13] or 1
    root = vol + (reserved + nfats * fat_sectors) * SECTOR
    data = root + (entries * 32 + SECTOR - 1) // SECTOR * SECTOR
    fat = vol + reserved * SECTOR
    base, _, ext = name.upper().partition(".")
    short = base.ljust(8)[:8] + ext.ljust(3)[:3]
    for i in range(entries):
        ent = img[root + i * 32: root + i * 32 + 32]
        if ent[0] in (0x00, 0xE5):
            break
        if ent[11] & 0x08 or ent[:11].decode("ascii", "replace") != short:
            continue
        size = struct.unpack_from("<I", ent, 28)[0]
        cl = struct.unpack_from("<H", ent, 26)[0]
        out = bytearray()
        while 2 <= cl < 0xFFF8:
            start = data + (cl - 2) * spc * SECTOR
            out += img[start:start + spc * SECTOR]
            cl = struct.unpack_from("<H", img, fat + cl * 2)[0]
        return bytes(out[:size])
    return None


def show(path: Path) -> int:
    img = path.read_bytes()
    bs = img[:SECTOR]
    ptype = bs[0x1BE + 4]
    pstart, plen = struct.unpack_from("<II", bs, 0x1BE + 8)
    vol = pstart * SECTOR
    vbs = img[vol:vol + SECTOR]
    total = struct.unpack_from("<I", vbs, 32)[0] or struct.unpack_from("<H", vbs, 19)[0]
    print(f"{path.name}: {len(img)} bytes = {len(img) // SECTOR} sectors")
    print(f"  MBR partition: type 0x{ptype:02x}, start {pstart}, sectors {plen}")
    print(f"  volume: {vbs[3:11]!r} type {vbs[54:62]!r} sectors {total} "
          f"cluster {vbs[13] * SECTOR} B, FAT {struct.unpack_from('<H', vbs, 22)[0]} sectors")
    clusters = (plen - 1 - 2 * struct.unpack_from("<H", vbs, 22)[0] -
                (struct.unpack_from("<H", vbs, 17)[0] * 32 + SECTOR - 1) // SECTOR)
    print(f"  clusters {clusters}")
    root = vol + (1 + 2 * struct.unpack_from("<H", vbs, 22)[0]) * SECTOR
    for i in range(ROOT_ENTRIES):
        ent = img[root + i * 32: root + i * 32 + 32]
        if ent[0] in (0x00, 0xE5):
            break
        nm = ent[:8].decode("ascii", "replace").rstrip() + "." + \
             ent[8:11].decode("ascii", "replace").rstrip()
        print(f"    {nm:16} {struct.unpack_from('<I', ent, 28)[0]} bytes")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--sectors", type=int, default=7552,
                    help="disk size in 512-byte sectors (must match MSC_SECTORS)")
    ap.add_argument("--out", default=str(ROOT / "firmware" / "media" / "setup-disk-big.img"))
    ap.add_argument("--source", default=str(SOURCE_IMAGE), help="image to take files from")
    ap.add_argument("--show", help="just print what an image contains")
    args = ap.parse_args()

    if args.show:
        return show(Path(args.show))

    src = Path(args.source)
    if not src.exists():
        raise SystemExit(f"source image not found: {src}")
    data = src.read_bytes()
    pstart = struct.unpack_from("<I", data, 0x1BE + 8)[0]
    files = read_fat12_volume(data, pstart)
    print(f"source {src.name}: {len(files)} files")
    for n, c in files:
        print(f"    {n:16} {len(c)} bytes")

    img = build(args.sectors, files)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(img)
    print(f"written {out} ({len(img)} bytes)")
    return show(out)


if __name__ == "__main__":
    raise SystemExit(main())
