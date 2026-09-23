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
import time
from datetime import datetime
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

def _lfn_checksum(short11: bytes) -> int:
    """Контрольная сумма короткого имени - по ней Windows связывает LFN с записью файла."""
    s = 0
    for b in short11:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def _short_name(name: str, taken: set[str]) -> str:
    """Короткое имя 8.3 для файла (верхний регистр); длинные имена дополняются записями LFN."""
    base, _, ext = name.upper().partition(".")
    stem = "".join(c for c in base if c.isalnum() or c in "_-$%@!(){}^#&'")[:8]
    ext3 = "".join(c for c in ext if c.isalnum())[:3]
    if stem and len(stem) <= 8 and len(ext3) <= 3 and stem == base and len(base) <= 8:
        cand = f"{stem}.{ext3}"
        if cand not in taken:
            return cand
    for n in range(1, 1000):
        stem2 = (stem[:6] + "~" + str(n))[:8]
        cand = f"{stem2}.{ext3}"
        if cand not in taken and len(stem2) >= 2:
            return cand
    raise SystemExit("не удалось собрать короткое имя 8.3 для " + name)


def _lfn_entries(short11: bytes, name: str) -> list[bytes]:
    """Записи длинного имени (по 13 символов UTF-16), в порядке, как их ждёт Windows:
    сначала последний кусок со старшим битом в номере, затем остальные, затем сам файл."""
    u = name.encode("utf-16-le")
    chars = [u[i:i + 2] for i in range(0, len(u), 2)]
    chunks = [chars[i:i + 13] for i in range(0, len(chars), 13)] or [[]]
    if len(chunks) > 20:
        raise SystemExit("слишком длинное имя для FAT: " + name)
    chk = _lfn_checksum(short11)
    slots = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]     # 13 позиций по 2 байта
    out: list[bytes] = []
    total = len(chunks)
    for idx, chunk in enumerate(reversed(chunks)):
        seq = total - idx
        if idx == 0:
            seq |= 0x40
        e = bytearray(32)
        e[0] = seq
        e[11] = 0x0F                      # атрибут: это запись длинного имени
        e[12] = 0x00
        e[13] = chk
        for i, off in enumerate(slots):
            if i < len(chunk):
                pair = chunk[i]
            elif i == len(chunk):
                pair = b"\x00\x00"        # конец имени
            else:
                pair = b"\xFF\xFF"        # добивка
            e[off:off + 2] = pair
        out.append(bytes(e))
    return out


def build(total_sectors: int, files: list[tuple]) -> bytes:
    """files: (name, content) или (name, content, время правки исходника).

    Время нужно для дат в записях каталога: без них Windows показывает файл с пустой
    датой (01.01.1601), и по диску не понять, свежий на нём набор или старый.
    """
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

    used = sum((len(f[1]) + SECTOR - 1) // SECTOR or 1 for f in files)
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
    entry_off = 0
    taken: set[str] = set()
    for item in files:
        name, content = item[0], item[1]
        mtime = item[2] if len(item) > 2 else None
        short = _short_name(name, taken)
        taken.add(short)
        base, _, ext = short.partition(".")
        short11 = base.ljust(8)[:8].encode("ascii") + ext.ljust(3)[:3].encode("ascii")
        n = max(1, (len(content) + SECTOR - 1) // SECTOR)
        for i in range(n):
            cl = next_cluster + i
            struct.pack_into("<H", fat, cl * 2, 0xFFFF if i == n - 1 else cl + 1)
            start = vol + (data_start + cl - 2) * SECTOR
            img[start:start + SECTOR] = content[i * SECTOR:(i + 1) * SECTOR].ljust(SECTOR, b"\x00")
        # длинное имя: записи LFN идут перед записью файла (только если имя не влезает в 8.3)
        if short != name.upper() or name != name.upper():
            for e in _lfn_entries(short11, name):
                root[entry_off:entry_off + 32] = e
                entry_off += 32
        off = entry_off
        root[off:off + 11] = short11
        root[off + 11] = 0x20
        struct.pack_into("<H", root, off + 26, next_cluster)
        struct.pack_into("<I", root, off + 28, len(content))
        # Даты создания и последней записи (формат FAT: год от 1980, секунды через 2).
        # Ноль здесь Windows показывает как «01.01.1601» — файл выглядит отставшим.
        dt = datetime.fromtimestamp(mtime if mtime else time.time())
        fdate = ((max(dt.year, 1980) - 1980) << 9) | (dt.month << 5) | dt.day
        ftime = (dt.hour << 11) | (dt.minute << 5) | (dt.second // 2)
        struct.pack_into("<H", root, off + 14, ftime)
        struct.pack_into("<H", root, off + 16, fdate)
        struct.pack_into("<H", root, off + 22, ftime)
        struct.pack_into("<H", root, off + 24, fdate)
        entry_off += 32
        next_cluster += n
    if entry_off > len(root):
        raise SystemExit("записи каталога не влезли в корневой каталог образа")

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

def _lfn_name(entries: list[bytes]) -> str:
    """Собрать длинное имя из записей LFN: они лежат в каталоге в обратном порядке."""
    parts: dict[int, str] = {}
    for e in entries:
        seq = e[0] & 0x1F
        parts[seq] = "".join(
            e[o:o + 2].decode("utf-16-le", "replace")
            for o in [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
        )
    name = "".join(parts[k] for k in sorted(parts))
    return name.split("\x00")[0].replace("\uffff", "")


def _root_dir(img: bytes) -> tuple[int, int, int]:
    """(смещение корневого каталога, число записей, смещение тома) в образе."""
    bs = img[:SECTOR]
    pstart = struct.unpack_from("<I", bs, 0x1BE + 8)[0] if bs[0x1BE + 4] else 0
    vol = pstart * SECTOR
    vbs = img[vol:vol + SECTOR]
    reserved = struct.unpack_from("<H", vbs, 14)[0]
    nfats = vbs[16]
    fat_sectors = struct.unpack_from("<H", vbs, 22)[0]
    entries = struct.unpack_from("<H", vbs, 17)[0]
    return vol + (reserved + nfats * fat_sectors) * SECTOR, entries, vol


def read_root_entries(img: bytes) -> list[tuple[str, int]]:
    """(имя, размер) файлов из корневого каталога — имя длинное, если оно записано.

    Размер берётся из записи каталога, поэтому цепочки FAT здесь не нужны.
    """
    root, entries, _vol = _root_dir(img)
    out: list[tuple[str, int]] = []
    pending: list[bytes] = []
    for i in range(entries):
        ent = img[root + i * 32: root + i * 32 + 32]
        if ent[0] in (0x00, 0xE5):
            break
        if ent[11] == 0x0F:                     # запись длинного имени
            pending.append(ent)
            continue
        if ent[11] & 0x08:                      # метка тома и прочее служебное
            pending = []
            continue
        short = ent[:8].decode("ascii", "replace").rstrip() + "." + \
                ent[8:11].decode("ascii", "replace").rstrip()
        name = _lfn_name(pending) if pending else short
        pending = []
        out.append((name, struct.unpack_from("<I", ent, 28)[0]))
    return out


def read_file(img: bytes, name: str) -> bytes | None:
    """Прочитать файл из тома в образе — целиком, побайтово (проверка сборки).

    Имя ищется и как длинное (записи LFN), и как короткое 8.3. Один размер из записи
    каталога ничего не доказывает: файл может оказаться обрезанным или собранным из
    чужих кластеров, поэтому идём по цепочке FAT16 и сверяем содержимое с исходником.
    Геометрию берём из загрузочного сектора тома, а не константами.
    """
    root, entries, vol = _root_dir(img)
    vbs = img[vol:vol + SECTOR]
    reserved = struct.unpack_from("<H", vbs, 14)[0]
    nfats = vbs[16]
    fat_sectors = struct.unpack_from("<H", vbs, 22)[0]
    entry_count = struct.unpack_from("<H", vbs, 17)[0]
    spc = vbs[13] or 1
    data = root + (entry_count * 32 + SECTOR - 1) // SECTOR * SECTOR
    fat = vol + reserved * SECTOR
    want = name.lower()
    pending: list[bytes] = []
    for i in range(entries):
        ent = img[root + i * 32: root + i * 32 + 32]
        if ent[0] in (0x00, 0xE5):
            break
        if ent[11] == 0x0F:
            pending.append(ent)
            continue
        if ent[11] & 0x08:
            pending = []
            continue
        short = ent[:8].decode("ascii", "replace").rstrip() + "." + \
                ent[8:11].decode("ascii", "replace").rstrip()
        long_name = _lfn_name(pending) if pending else short
        pending = []
        if long_name.lower() != want and short.lower() != want:
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
