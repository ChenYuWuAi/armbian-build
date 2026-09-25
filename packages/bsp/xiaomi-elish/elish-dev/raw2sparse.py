#!/usr/bin/env python3
"""raw2sparse.py — 把 raw 镜像转成 Android sparse 格式

fastboot 刷超过 max-download-size 的镜像时必须用 sparse 格式。

用法:  python3 raw2sparse.py <raw.img> <out.simg>

格式: sparse_header(28B, magic 0xED26FF3A) + chunks(12B header each)
      CHUNK_TYPE_RAW=0xCAC1 / FILL=0xCAC2 / DONT_CARE=0xCAC3
连续同类扇区会合并成一个 chunk，避免 chunk 数量爆炸。
"""
import sys, struct, os

MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_DC = 0xCAC3
BLK = 4096


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    size = os.path.getsize(src)
    total_blks = (size + BLK - 1) // BLK
    zero = b"\0" * BLK
    chunks = 0

    with open(src, "rb") as f, open(dst, "wb") as o:
        o.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLK,
                            total_blks, 0, 0))
        blk_idx = 0
        while blk_idx < total_blks:
            data = f.read(BLK)
            if len(data) < BLK:
                data += b"\0" * (BLK - len(data))
            blk_idx += 1
            if data == zero:
                n = 1
                while blk_idx < total_blks:
                    d2 = f.read(BLK)
                    if len(d2) < BLK:
                        d2 += b"\0" * (BLK - len(d2))
                    if d2 != zero:
                        # 这不是零块，回退
                        f.seek(-BLK, os.SEEK_CUR)
                        break
                    blk_idx += 1
                    n += 1
                o.write(struct.pack("<HHII", CHUNK_DC, 0, n, 12))
                chunks += 1
            else:
                buf = [data]
                n = 1
                while blk_idx < total_blks:
                    d2 = f.read(BLK)
                    if len(d2) < BLK:
                        d2 += b"\0" * (BLK - len(d2))
                    if d2 == zero:
                        f.seek(-BLK, os.SEEK_CUR)
                        break
                    buf.append(d2)
                    blk_idx += 1
                    n += 1
                payload = b"".join(buf)
                o.write(struct.pack("<HHII", CHUNK_RAW, 0, n, 12 + len(payload)))
                o.write(payload)
                chunks += 1

        o.seek(0)
        o.write(struct.pack("<IHHHHIIII", MAGIC, 1, 0, 28, 12, BLK,
                            total_blks, chunks, 0))

    print("raw=%d bytes (%d blocks) -> sparse=%d bytes, chunks=%d"
          % (size, total_blks, os.path.getsize(dst), chunks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
