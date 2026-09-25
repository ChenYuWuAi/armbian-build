"""expand an Android sparse image and hash it, for对比 raw 原图"""
import sys, os, struct, hashlib

RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4


def main():
    sp, raw = sys.argv[1], sys.argv[2]
    h_sp = hashlib.sha256()
    h_raw = hashlib.sha256()
    with open(sp, "rb") as f:
        hdr = f.read(28)
        magic, major, minor, fhsz, chsz, blk, total_blks, total_chunks, crc = struct.unpack("<IHHHHIIII", hdr)
        assert magic == 0xED26FF3A, "not a sparse image"
        print("sparse: v%d.%d blk=%d total_blks=%d chunks=%d crc=0x%08X" % (major, minor, blk, total_blks, total_chunks, crc))
        written = 0
        nchunks = 0
        counts = {RAW: 0, FILL: 0, DONT_CARE: 0, CRC32: 0}
        while nchunks < total_chunks:
            ch = f.read(12)
            if len(ch) < 12:
                break
            ctype, _r, csz, tsz = struct.unpack("<HHII", ch)
            counts[ctype] = counts.get(ctype, 0) + 1
            if ctype == RAW:
                left = csz * blk
                while left:
                    b = f.read(min(1 << 22, left))
                    h_sp.update(b)
                    left -= len(b)
                written += csz * blk
            elif ctype == FILL:
                fill = f.read(4)
                b = fill * (csz * blk // 4)
                h_sp.update(b)
                written += csz * blk
            elif ctype == DONT_CARE:
                zeros = bytes(1 << 22)
                left = csz * blk
                while left:
                    n = min(len(zeros), left)
                    h_sp.update(zeros[:n])
                    left -= n
                written += csz * blk
            elif ctype == CRC32:
                f.read(4)
            nchunks += 1
    with open(raw, "rb") as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            h_raw.update(b)
    print("chunk 统计: RAW=%d FILL=%d DONT_CARE=%d CRC32=%d" % (counts[RAW], counts[FILL], counts[DONT_CARE], counts[CRC32]))
    print("展开得到    : %d bytes (%.2f GiB)" % (written, written / 2**30))
    print("sparse 展开 sha256: %s" % h_sp.hexdigest())
    print("原始 raw    sha256: %s" % h_raw.hexdigest())
    print("一致: %s" % (h_sp.hexdigest() == h_raw.hexdigest()))


if __name__ == "__main__":
    main()
