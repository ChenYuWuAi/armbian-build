#!/usr/bin/env python3
"""解析 Android super 分区的 liblp 元数据，列出逻辑分区及其在物理设备上的区间。
用法: sudo python3 lp_list.py [super设备]
"""
import struct, sys, json

GEOM_MAGIC = 0x616C4467
HDR_MAGIC = 0x414C5030
GEOM_OFF = 4096


def read(path, off, size):
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(size)


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/disk/by-partlabel/super"
    geom = read(dev, GEOM_OFF, 4096)
    magic, struct_size = struct.unpack_from("<II", geom, 0)
    metadata_max_size, metadata_slot_count, logical_block_size = struct.unpack_from("<III", geom, 40)
    print("geometry: magic=0x%08X struct_size=%d max=%d slots=%d lbs=%d"
          % (magic, struct_size, metadata_max_size, metadata_slot_count, logical_block_size))
    if magic != GEOM_MAGIC:
        print("不是 liblp 几何结构，退出")
        return 1

    # 实测布局：几何结构在 4096，槽位 0 的头部在 12288（= 8192 + 4096），槽间距 = metadata_max_size
    slot0 = 12288
    for slot in range(metadata_slot_count):
        off = slot0 + slot * metadata_max_size
        hdr = read(dev, off, 256)
        hmagic, major, minor, header_size = struct.unpack_from("<IHHI", hdr, 0)
        tables_size = struct.unpack_from("<I", hdr, 44)[0]
        desc = struct.unpack_from("<III", hdr, 80)      # partitions
        desc2 = struct.unpack_from("<III", hdr, 92)     # extents
        desc3 = struct.unpack_from("<III", hdr, 104)    # groups
        print("slot %d @%d: magic=0x%08X v%d.%d header_size=%d tables_size=%d"
              % (slot, off, hmagic, major, minor, header_size, tables_size))
        if hmagic != HDR_MAGIC:
            print("   头部 magic 不对，跳过")
            continue
        tbase = off + header_size
        p_off, p_num, p_size = desc
        e_off, e_num, e_size = desc2
        print("   entries: partitions=%d(size %d) extents=%d(size %d) groups=%d"
              % (p_num, p_size, e_num, e_size, desc3[1]))

        extents = []
        raw = read(dev, tbase + e_off, e_num * e_size)
        for i in range(e_num):
            b = raw[i * e_size:(i + 1) * e_size]
            if e_size >= 32:
                num_sectors, = struct.unpack_from("<Q", b, 0)
                target_type, = struct.unpack_from("<I", b, 8)
                target_data, = struct.unpack_from("<Q", b, 16)
                target_source, = struct.unpack_from("<I", b, 24)
            else:
                num_sectors, target_type, target_data, target_source = struct.unpack_from("<QIQI", b, 0)
            extents.append((num_sectors, target_type, target_data, target_source))

        praw = read(dev, tbase + p_off, p_num * p_size)
        out = []
        for i in range(p_num):
            b = praw[i * p_size:(i + 1) * p_size]
            name = b[0:36].split(b"\0")[0].decode(errors="replace")
            attributes, first_ext, num_ext, group_index = struct.unpack_from("<IIII", b, 36)
            exts = []
            for j in range(first_ext, first_ext + num_ext):
                ns, tt, td, ts = extents[j]
                exts.append({"sectors": ns, "size_mb": round(ns * 512 / 1048576, 1),
                             "type": tt, "start_sector": td, "offset_mb": round(td * 512 / 1048576, 1)})
            out.append({"name": name, "attrs": attributes, "group": group_index, "extents": exts})
        print(json.dumps(out, ensure_ascii=False, indent=1))
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
