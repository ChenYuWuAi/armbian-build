#!/usr/bin/env python3
"""Android sparse -> raw,流式写盘(不占内存)。用法: python3 sparse_to_raw.py <in.img> <out.img>"""
import struct, sys

RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4

def main():
    sp, out = sys.argv[1], sys.argv[2]
    fin = open(sp, 'rb')
    fout = open(out, 'wb')
    magic, major, minor, fhs, chs, blk, total_blks, total_chunks, crc = struct.unpack('<IHHHHIIII', fin.read(28))
    if magic != 0xED26FF3A:
        raise SystemExit("不是 sparse 镜像: 0x%08X" % magic)
    print("sparse v%d.%d  blk=%d total_blks=%d chunks=%d" % (major, minor, blk, total_blks, total_chunks))
    written = 0
    for i in range(total_chunks):
        ct, res, csz, tsz = struct.unpack('<HHII', fin.read(12))
        nbytes = csz * blk
        if ct == RAW:
            left = nbytes
            while left:
                b = fin.read(min(8 << 20, left))
                if not b: raise SystemExit("RAW chunk 数据不足")
                fout.write(b); left -= len(b)
            written += nbytes
        elif ct == FILL:
            v = fin.read(4)
            chunk = v * (blk // 4)
            left = nbytes
            while left:
                w = min(len(chunk), left)
                fout.write(chunk[:w]); left -= w
            written += nbytes
        elif ct == DONT_CARE:
            left = nbytes
            z = b'\0' * (1 << 20)
            while left:
                w = min(len(z), left)
                fout.write(z[:w]); left -= w
            written += nbytes
        elif ct == CRC32:
            fin.read(4)
        else:
            raise SystemExit("未知 chunk 类型 0x%04X @%d" % (ct, i))
    fin.close(); fout.close()
    print("完成: %s -> %s  %d 字节 (%.1f MB)" % (sp, out, written, written / 1048576))

if __name__ == '__main__':
    main()
