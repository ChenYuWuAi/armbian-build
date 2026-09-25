#!/usr/bin/env python3
"""build_gpt_elish.py — 还原 Armbian(elish) 时期 LUN0 的 GPT

官方 stock GPT 把 userdata 定义成 0 尺寸占位符，Armbian 时期的分区表是：
    userdata  99,994,148,864 B (24,412,634 扇区)   <- 原地缩小
    esp          500,170,752 B (   122,112 扇区)   <- 新建
    linux    141,960,413,184 B (34,658,304 扇区)   <- 新建（rootfs 所在）
尺寸来自本机 fastboot getvar all 快照（getvar_all.txt），GPT 内条目顺序
（userdata -> esp -> linux，linux 在最后）由该快照的倒序列出顺序推定，
并且与 SESSION_SUMMARY 的 "userdata 100G / esp 0.5G / linux 142G" 完全吻合。

用法: python3 build_gpt_elish.py <stock gpt_both0.bin> <out.bin>
"""
import struct, sys, uuid, zlib

SECTOR = 4096
GPT_SIG = b"EFI PART"

# 原始条目（未改动的 34 个）之外新增的两个
esp_type  = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"   # EFI System
linux_type = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"  # Linux filesystem
esp_guid  = "8a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"
linux_guid = "1f2e3d4c-5b6a-4798-a1b2-c3d4e5f60718"

USERDATA_FIRST, USERDATA_LAST = 2686976, 27099609
ESP_FIRST, ESP_LAST = 27099610, 27221721
LINUX_FIRST, LINUX_LAST = 27221722, 61880025
LAST_USABLE = LINUX_LAST
ALT_LBA = LAST_USABLE + 5


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def crc32(b):
    return zlib.crc32(b) & 0xffffffff


def entry(name, type_guid, uniq, first, last):
    e = bytearray(128)
    e[0:16] = guid_bytes(type_guid)
    e[16:32] = guid_bytes(uniq)
    struct.pack_into("<QQ", e, 32, first, last)
    struct.pack_into("<Q", e, 48, 0)          # attributes
    nm = name.encode("utf-16-le")
    assert len(nm) <= 72
    e[56:56 + len(nm)] = nm
    return bytes(e)


def find_entry(d, base, nent, esize, name):
    for i in range(nent):
        o = base + i * esize
        if d[o:o + 16] == b"\0" * 16:
            continue
        if d[o + 56:o + 128].decode("utf-16-le").rstrip("\0") == name:
            return i
    return None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, "rb").read())
    assert len(d) == 45056, len(d)
    assert d[0x1000:0x1008] == GPT_SIG and d[0xa000:0xa008] == GPT_SIG

    # ---------- primary ----------
    nent, esize = struct.unpack_from("<II", d, 0x1000 + 80)
    pbase = struct.unpack_from("<Q", d, 0x1000 + 72)[0] * SECTOR   # 0x2000
    assert (nent, esize, pbase) == (64, 128, 0x2000), (nent, esize, hex(pbase))

    i = find_entry(d, pbase, nent, esize, "userdata")
    assert i == 33, i
    struct.pack_into("<Q", d, pbase + i * esize + 40, USERDATA_LAST)

    for idx, (nm, tg, ug, f, l) in enumerate((
            ("esp", esp_type, esp_guid, ESP_FIRST, ESP_LAST),
            ("linux", linux_type, linux_guid, LINUX_FIRST, LINUX_LAST)), start=34):
        old = d[pbase + idx * esize:pbase + (idx + 1) * esize]
        assert old == b"\0" * 128, "slot %d not free" % idx
        d[pbase + idx * esize:pbase + (idx + 1) * esize] = entry(nm, tg, ug, f, l)

    arr_crc = crc32(bytes(d[pbase:pbase + nent * esize]))
    struct.pack_into("<I", d, 0x1000 + 88, arr_crc)
    struct.pack_into("<Q", d, 0x1000 + 32, ALT_LBA)          # alternate_lba
    struct.pack_into("<Q", d, 0x1000 + 48, LAST_USABLE)      # last_usable_lba

    # ---------- backup ----------
    bbase = 0x6000
    assert find_entry(d, bbase, nent, esize, "userdata") == i
    d[bbase:bbase + nent * esize] = d[pbase:pbase + nent * esize]

    struct.pack_into("<Q", d, 0xa000 + 24, ALT_LBA)          # my_lba
    struct.pack_into("<Q", d, 0xa000 + 32, 1)                # alternate_lba
    struct.pack_into("<Q", d, 0xa000 + 48, LAST_USABLE)      # last_usable_lba
    struct.pack_into("<Q", d, 0xa000 + 72, ALT_LBA - 4)      # partition_entry_lba
    struct.pack_into("<I", d, 0xa000 + 88, arr_crc)

    # ---------- header crcs ----------
    for h in (0x1000, 0xa000):
        hsize = struct.unpack_from("<I", d, h + 12)[0]
        struct.pack_into("<I", d, h + 16, 0)
        c = crc32(bytes(d[h:h + hsize]))
        struct.pack_into("<I", d, h + 16, c)

    open(dst, "wb").write(d)
    print("written %s (%d bytes) alt_lba=%d last_usable=%d arr_crc=%#010x"
          % (dst, len(d), ALT_LBA, LAST_USABLE, arr_crc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
