#!/usr/bin/env python3
"""Generate a small, deterministic FAT16 disk image for the Lateran driver.

No external filesystem tools required -- we lay out the boot sector (BPB), the
FAT, the root directory and the file data by hand. The result is a plain raw
image suitable for `-drive file=...,format=raw -device virtio-blk-device`.
"""
import struct
import sys

SECTOR = 512
SECTORS_PER_CLUSTER = 1
RESERVED_SECTORS = 1
NUM_FATS = 2
ROOT_ENTRIES = 512
TOTAL_SECTORS = 16384  # 8 MiB -> enough clusters to be FAT16
FAT_SIZE = 64          # sectors per FAT (holds > total cluster count)

FILES = [
    ("HELLO.TXT", b"Hello from the Lateran FAT16 disk!\n"),
    ("INDEX.TXT", b"Index reads real files off a FAT filesystem now.\n"),
    ("README.TXT", b"GrimoireFS=baked, Bookshelf=RAM, Lateran=disk (FAT16).\n"),
]


def main() -> None:
    # Usage: mkfatfs.py OUT.img [NAME=path ...]  -- extra files are read from
    # disk and added under their 8.3 NAME (e.g. INIT.ELF=build/init.elf).
    out = sys.argv[1] if len(sys.argv) > 1 else "fat.img"
    files = list(FILES)
    for arg in sys.argv[2:]:
        name, _, path = arg.partition("=")
        with open(path, "rb") as fh:
            files.append((name, fh.read()))

    img = bytearray(TOTAL_SECTORS * SECTOR)

    # --- Boot sector / BIOS Parameter Block ---
    img[0:3] = b"\xeb\x3c\x90"
    img[3:11] = b"INDEXdsk"
    struct.pack_into("<H", img, 11, SECTOR)
    img[13] = SECTORS_PER_CLUSTER
    struct.pack_into("<H", img, 14, RESERVED_SECTORS)
    img[16] = NUM_FATS
    struct.pack_into("<H", img, 17, ROOT_ENTRIES)
    struct.pack_into("<H", img, 19, TOTAL_SECTORS if TOTAL_SECTORS < 65536 else 0)
    img[21] = 0xF8  # media descriptor
    struct.pack_into("<H", img, 22, FAT_SIZE)
    struct.pack_into("<H", img, 24, 32)  # sectors per track
    struct.pack_into("<H", img, 26, 2)   # heads
    img[36] = 0x80
    img[38] = 0x29
    struct.pack_into("<I", img, 39, 0x12345678)
    img[43:54] = b"INDEX VOL  "
    img[54:62] = b"FAT16   "
    img[510:512] = b"\x55\xaa"

    fat_start = RESERVED_SECTORS
    root_start = RESERVED_SECTORS + NUM_FATS * FAT_SIZE
    root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
    data_start = root_start + root_sectors

    fat = {0: 0xFFF8, 1: 0xFFFF}
    dir_entries = []
    next_cluster = 2
    cluster_bytes = SECTOR * SECTORS_PER_CLUSTER

    for name, content in files:
        nclusters = max(1, (len(content) + cluster_bytes - 1) // cluster_bytes)
        clusters = list(range(next_cluster, next_cluster + nclusters))
        next_cluster += nclusters
        for i, cl in enumerate(clusters):
            off = (data_start + (cl - 2) * SECTORS_PER_CLUSTER) * SECTOR
            chunk = content[i * cluster_bytes:(i + 1) * cluster_bytes]
            img[off:off + len(chunk)] = chunk
            fat[cl] = 0xFFFF if i == len(clusters) - 1 else clusters[i + 1]

        base, _, ext = name.partition(".")
        short = (base[:8].ljust(8) + ext[:3].ljust(3)).upper().encode("ascii")
        ent = bytearray(32)
        ent[0:11] = short
        ent[11] = 0x20  # archive
        struct.pack_into("<H", ent, 26, clusters[0])
        struct.pack_into("<I", ent, 28, len(content))
        dir_entries.append(bytes(ent))

    fatbytes = bytearray(FAT_SIZE * SECTOR)
    for cl, val in fat.items():
        struct.pack_into("<H", fatbytes, cl * 2, val & 0xFFFF)
    for f in range(NUM_FATS):
        off = (fat_start + f * FAT_SIZE) * SECTOR
        img[off:off + len(fatbytes)] = fatbytes

    off = root_start * SECTOR
    blob = b"".join(dir_entries)
    img[off:off + len(blob)] = blob

    with open(out, "wb") as fh:
        fh.write(img)
    print(f"wrote {out}: {TOTAL_SECTORS} sectors FAT16, {len(files)} files, "
          f"root@{root_start} data@{data_start}")


if __name__ == "__main__":
    main()
