#!/usr/bin/env python3
"""从 super 里抽取一个逻辑分区（按 extent 顺序拼接）到文件。
用法: sudo python3 lp_extract.py <分区名子串> <输出文件>
"""
import struct, sys, os

GEOM_OFF = 4096
SLOT0 = 12288
HDR_MAGIC = 0x414C5030


def parse(dev):
    with open(dev, "rb") as f:
        f.seek(GEOM_OFF)
        geom = f.read(4096)
        metadata_max_size, metadata_slot_count = struct.unpack_from("<III", geom, 40)[0:3:2]
        metadata_max_size = struct.unpack_from("<I", geom, 40)[0]
        metadata_slot_count = struct.unpack_from("<I", geom, 44)[0]
        off = SLOT0
        f.seek(off)
        hdr = f.read(256)
        if struct.unpack_from("<I", hdr, 0)[0] != HDR_MAGIC:
            raise SystemExit("头部 magic 不对")
        header_size = struct.unpack_from("<I", hdr, 8)[0]
        p_off, p_num, p_size = struct.unpack_from("<III", hdr, 80)
        e_off, e_num, e_size = struct.unpack_from("<III", hdr, 92)
        tbase = off + header_size
        f.seek(tbase + e_off)
        raw = f.read(e_num * e_size)
        extents = []
        for i in range(e_num):
            b = raw[i * e_size:(i + 1) * e_size]
            if e_size >= 32:
                ns, = struct.unpack_from("<Q", b, 0)
                td, = struct.unpack_from("<Q", b, 16)
            else:
                ns, tt, td, ts = struct.unpack_from("<QIQI", b, 0)
            extents.append((ns, td))
        f.seek(tbase + p_off)
        praw = f.read(p_num * p_size)
        parts = []
        for i in range(p_num):
            b = praw[i * p_size:(i + 1) * p_size]
            name = b[0:36].split(b"\0")[0].decode(errors="replace")
            attrs, first_ext, num_ext, group = struct.unpack_from("<IIII", b, 36)
            parts.append((name, extents[first_ext:first_ext + num_ext]))
    return parts


def main():
    dev = sys.argv[3] if len(sys.argv) > 3 else "/dev/disk/by-partlabel/super"
    want = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "/root/part.img"
    parts = parse(dev)
    print("super 中的逻辑分区:")
    for name, exts in parts:
        total = sum(ns for ns, _ in exts) * 512
        print("   %-16s %8.1f MB  extents=%d" % (name, total / 1048576, len(exts)))
    hit = [p for p in parts if want in p[0]]
    if not hit:
        print("没找到包含 '%s' 的分区" % want)
        return 1
    name, exts = hit[0]
    with open(dev, "rb") as fi, open(out, "wb") as fo:
        written = 0
        for ns, td in exts:
            fi.seek(td * 512)
            left = ns * 512
            while left:
                b = fi.read(min(8 << 20, left))
                if not b:
                    break
                fo.write(b)
                left -= len(b)
                written += len(b)
    print("已抽取 %s -> %s : %.1f MB" % (name, out, written / 1048576))
    return 0


if __name__ == "__main__":
    sys.exit(main())
