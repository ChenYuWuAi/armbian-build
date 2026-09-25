#!/usr/bin/env python3
"""Dump the ACDB *index tables* of a Qualcomm Forte_*_cal.acdb workspace file.

This is a read-only analysis helper for the elish speaker-calibration port
(see work/kernel/elish_acdb_cal/ACDB_PORT_PLAN.md).  It does NOT modify any
file and it does NOT need libacdbloader.so; it only walks the on-disk tables
that the userspace loader uses to resolve

    (cal_type, acdb_id, app_type, sample_rate|vol_index)  ->  cal-data offset

Verified against work/acdb/Forte_Speaker_cal.acdb (766696 B, md5 6d6c69bb*):

  chunk framing        <8-byte tag><u32 payload_len><payload>
  tail offset table    32-byte rows:
                       {u32 sample_rate; u32 index; u32 off1..off5; u32 0}
                       the 48000 rows are at 0xbabc2 (index 0) .. 0xbb242 (10)
  acdb_id key table    ~12-byte stride, 0x271b (10011) first at 0x1734
  app_type key table   ~20-byte stride, 0x11134 (69940) first at 0x1f30

Usage:
    python3 acdb_tables.py chunks   Forte_Speaker_cal.acdb
    python3 acdb_tables.py tail     Forte_Speaker_cal.acdb [sample_rate]
    python3 acdb_tables.py keytables Forte_Speaker_cal.acdb [value ...]
"""
import struct
import sys

TAIL_STRIDE = 32
TAIL_ROW = "<8I"


def read(fn):
    with open(fn, "rb") as f:
        return f.read()


def u32(d, off):
    return struct.unpack_from("<I", d, off)[0]


def chunks(d):
    """Yield (offset, tag, payload_off, payload_len) for the top-level chunks."""
    tag = d[0x10:0x14]
    if tag != b"AVDB":
        print("warning: expected AVDB at 0x10, got %r" % tag)
    off = 0x20
    while off + 12 <= len(d):
        t = d[off:off + 8]
        if not t.isascii() or not t.strip():
            break
        n = u32(d, off + 8)
        if n == 0 or off + 12 + n > len(d):
            break
        yield off, t.decode(errors="replace").rstrip(), off + 12, n
        off += 12 + n


def cmd_chunks(fn):
    d = read(fn)
    print("%s  %d B" % (fn, len(d)))
    for off, tag, poff, n in chunks(d):
        print("  @0x%06x  %-8s len=0x%06x payload=0x%06x" % (off, tag, n, poff))


def find_u32(d, val, start=0):
    out = []
    pat = struct.pack("<I", val)
    p = start
    while True:
        i = d.find(pat, p)
        if i < 0:
            return out
        out.append(i)
        p = i + 1


def tail_row(d, off):
    return struct.unpack_from(TAIL_ROW, d, off)


def cmd_tail(fn, want_rate=48000):
    d = read(fn)
    hits = [o for o in find_u32(d, want_rate) if o >= 0x1000]
    if not hits:
        print("no 0x%x (%d) found" % (want_rate, want_rate))
        return
    # The table is byte-packed: its rows are NOT 4-byte aligned in the file
    # (the 48000 rows start at 0xbabc2, which is 2 mod 4).  Do not filter on
    # alignment; group hits that are exactly one row apart instead.
    runs = []
    for o in hits:
        if runs and o - runs[-1][-1] == TAIL_STRIDE:
            runs[-1].append(o)
        else:
            runs.append([o])
    good = [r for r in runs if len(r) >= 4]
    print("%s: %d hit(s), %d run(s) of >=4 rows with sample_rate=%d"
          % (fn, len(hits), len(good), want_rate))
    for run in good:
        print("  run @0x%06x .. 0x%06x  (%d rows, %s)"
              % (run[0], run[-1], len(run),
                 "4-byte aligned" if run[0] % 4 == 0 else "2-byte aligned"))
        for o in run:
            f = tail_row(d, o)
            print("    @0x%06x rate=%-6d index=%-3d offs=%s"
                  % (o, f[0], f[1], " ".join("0x%06x" % x for x in f[2:7])))


def cmd_keytables(fn, values):
    d = read(fn)
    for v in values:
        hits = find_u32(d, v)
        print("%s: value %d (0x%x): %d hit(s)" % (fn, v, v, len(hits)))
        for i, o in enumerate(hits[:12]):
            # show the surrounding 3 u32 so the stride can be eyeballed
            lo = max(0, o - 12)
            ctx = struct.unpack_from("<6I", d, lo)
            stride = hits[i + 1] - o if i + 1 < len(hits) else 0
            print("   @0x%06x stride_to_next=%-3d ctx=%s"
                  % (o, stride, [hex(x) for x in ctx]))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, fn = sys.argv[1], sys.argv[2]
    if cmd == "chunks":
        cmd_chunks(fn)
    elif cmd == "tail":
        rate = int(sys.argv[3], 0) if len(sys.argv) > 3 else 48000
        cmd_tail(fn, rate)
    elif cmd == "keytables":
        vals = [int(x, 0) for x in sys.argv[3:]] or [10011, 0x11134, 0x11131,
                                                     0x1000A100]
        cmd_keytables(fn, vals)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
