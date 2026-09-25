#!/usr/bin/env python3
"""grow_userdata.py — 把官方 GPT 里的 userdata 分区从 0 尺寸扩展为填满磁盘

官方 stock GPT 把 userdata 定义成 0 尺寸占位符（Android 由 fs_mgr 首次启动时扩展）。
Armbian 时期该分区是"已扩展"状态；恢复官方 GPT 后它变回 0 尺寸，
内核因此找不到 Armbian rootfs 分区 → initramfs 挂不上根。

本脚本：
  1. 把 userdata 的 last_lba 设为 alternate_lba-6（避开末尾 5 个扇区的备份 GPT）
  2. 主/备 GPT 都改
  3. 重算 主分区项 CRC、备份分区项 CRC、主 header CRC、备份 header CRC

用法: python3 grow_userdata.py <gpt_both0.bin> <out.bin>
"""
import struct, sys, zlib

SECTOR = 4096


def crc32(b):
    return zlib.crc32(b) & 0xffffffff


def find_entry(d, base, nent, esize, name):
    for i in range(nent):
        off = base + i * esize
        if d[off:off + 16] == b"\0" * 16:
            continue
        nm = d[off + 56:off + 128].decode("utf-16-le").rstrip("\0")
        if nm == name:
            return off
    return None


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, "rb").read())
    assert d[4096:4104] == b"EFI PART", "primary GPT header not found at LBA1"

    # ---- primary ----
    phdr = 4096
    alt, = struct.unpack_from("<Q", d, phdr + 32)
    pe_lba, = struct.unpack_from("<Q", d, phdr + 72)
    nent, esize = struct.unpack_from("<II", d, phdr + 80)
    pbase = pe_lba * SECTOR

    off = find_entry(d, pbase, nent, esize, "userdata")
    assert off is not None, "userdata entry not found"
    first, last = struct.unpack_from("<QQ", d, off + 32)
    new_last = alt - 6
    struct.pack_into("<Q", d, off + 40, new_last)
    print("primary  userdata: LBA %d..%d (%.2f GB)  ->  ..%d (%.2f GB)"
          % (first, last, (last - first + 1) * SECTOR / 1024**3,
             new_last, (new_last - first + 1) * SECTOR / 1024**3))

    # ---- backup (文件末尾: [4 个分区项扇区][1 个 header 扇区]) ----
    nsec = len(d) // SECTOR
    bhdr = (nsec - 1) * SECTOR
    assert d[bhdr:bhdr + 8] == b"EFI PART", "backup GPT header not found"
    bpe, = struct.unpack_from("<Q", d, bhdr + 72)
    bnent, besize = struct.unpack_from("<II", d, bhdr + 80)
    bbase = (nsec - 5) * SECTOR          # 备份分区项在文件里的位置

    off = find_entry(d, bbase, bnent, besize, "userdata")
    assert off is not None, "userdata entry not found in backup"
    struct.pack_into("<Q", d, off + 40, new_last)
    print("backup   userdata: last_lba -> %d" % new_last)

    # ---- recompute CRCs ----
    struct.pack_into("<I", d, phdr + 88, crc32(bytes(d[pbase:pbase + nent * esize])))
    struct.pack_into("<I", d, bhdr + 88, crc32(bytes(d[bbase:bbase + bnent * besize])))

    for h in (phdr, bhdr):
        hsize, = struct.unpack_from("<I", d, h + 12)
        struct.pack_into("<I", d, h + 16, 0)
        c = crc32(bytes(d[h:h + hsize]))
        struct.pack_into("<I", d, h + 16, c)
        print("  header @%#x crc -> %#010x" % (h, c))

    open(dst, "wb").write(d)
    print("written", dst, len(d), "bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
