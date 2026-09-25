#!/usr/bin/env python3
"""Build an elish boot_b image from a kernel Image, using the layout that was
empirically validated on the device (see ELISH_AMP_TDM_FIX.md sections 38-41).

Validated facts this relies on:
  * Android boot image v0 header, 4096-byte page.
  * second_size is at header offset 0x18 and is **0** on this device's image.
    (Do NOT read offset 0x14 - that is ramdisk_addr = 0x01000000 = 16777216 and
     mistaking it for second_size produces an image the bootloader rejects.)
  * Layout: header(4096) + round_up(kernel) + round_up(ramdisk) + zero padding.
  * The ~135 MB tail after the ramdisk in the original dump is inert partition
    garbage (entropy 7.99, no recognisable magic) - zeroing it still boots.
  * gzip with `gzip -9 -n` is used so the stream header matches the original
    (`1f8b080000000000`); Python's gzip writes OS=255 instead.

Usage:
    python3 make_boot_image.py <Image> <out.img> [--base boot_a_flash.img]
"""
import argparse
import gzip
import hashlib
import io
import struct
import sys

PAGE = 4096
TOTAL = 201326592  # elish boot partition size (192 MiB)


def ru(x, a=PAGE):
    return (x + a - 1) // a * a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--base", default="/home/axis/axis_rnd/work/boot_a_flash.img",
                    help="known-good boot image used as header+ramdisk source")
    ap.add_argument("--dtb", default=None,
                    help="device tree to append (default: reuse the base image's)")
    ap.add_argument("--ramdisk", default=None,
                    help="gzip cpio ramdisk to use instead of the base image's")
    ap.add_argument("--python-gzip", action="store_true",
                    help="use python gzip instead of system gzip (not recommended)")
    a = ap.parse_args()

    base = open(a.base, "rb").read()
    if len(base) != TOTAL:
        sys.exit("base image is %d bytes, expected %d" % (len(base), TOTAL))
    if base[:8] != b"ANDROID!":
        sys.exit("base image has no ANDROID! magic")

    ksize_orig, = struct.unpack_from("<I", base, 8)
    rsize, = struct.unpack_from("<I", base, 16)
    second_size, = struct.unpack_from("<I", base, 24)
    page, = struct.unpack_from("<I", base, 36)
    print("base: ksize=%d rsize=%d second_size=%d page=%d" %
          (ksize_orig, rsize, second_size, page))
    if second_size != 0:
        sys.exit("unexpected second_size %d" % second_size)
    if page != PAGE:
        sys.exit("unexpected page size %d" % page)

    r_off = PAGE + ru(ksize_orig)
    ramdisk = base[r_off:r_off + rsize]
    if len(ramdisk) != rsize:
        sys.exit("could not extract ramdisk")

    if a.ramdisk:
        rd = open(a.ramdisk, "rb").read()
        if rd[:2] != b"\x1f\x8b":
            sys.exit("--ramdisk is not gzip data")
        ramdisk = rd
        rsize = len(rd)

    img = open(a.image, "rb").read()
    if img[:2] != b"MZ":
        print("warning: kernel does not start with MZ (%r)" % img[:8])
    if img[56:60] != b"ARM\x64":
        sys.exit("kernel has no ARM\\x64 magic at offset 56")

    if a.python_gzip:
        b = io.BytesIO()
        with gzip.GzipFile(fileobj=b, mode="wb", compresslevel=9, mtime=0) as g:
            g.write(img)
        kz = b.getvalue()
    else:
        import subprocess
        kz = subprocess.run(["gzip", "-9", "-n", "-c"], input=img,
                            stdout=subprocess.PIPE, check=True).stdout

    # The base boot image uses the Image.gz-dtb layout: the device tree is
    # appended immediately after the gzip stream and IS included in
    # kernel_size.  Dropping it makes the bootloader fail to find a DTB and
    # fall back to fastboot (root-caused 2026-09-22).
    import zlib
    _o = zlib.decompressobj(31)
    _o.decompress(base[PAGE:PAGE + ksize_orig])
    gz_len = (ksize_orig - len(_o.unused_data))
    dtb = _o.unused_data
    if dtb[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("expected an appended DTB after the kernel gzip stream")
    if a.dtb:
        dtb = open(a.dtb, "rb").read()
        if dtb[:4] != b"\xd0\x0d\xfe\xed":
            sys.exit("--dtb is not an FDT")
    print("appended DTB: %d bytes (kernel gzip stream was %d)" % (len(dtb), gz_len))

    region = kz + dtb
    hdr = bytearray(base[:PAGE])
    struct.pack_into("<I", hdr, 8, len(region))
    struct.pack_into("<I", hdr, 16, rsize)

    out = bytearray()
    out += hdr
    out += region + b"\0" * (ru(len(region)) - len(region))
    out += ramdisk + b"\0" * (ru(len(ramdisk)) - len(ramdisk))
    if len(out) > TOTAL:
        sys.exit("image too large: %d > %d" % (len(out), TOTAL))
    out += b"\0" * (TOTAL - len(out))
    assert len(out) == TOTAL

    open(a.out, "wb").write(bytes(out))

    # self-verify by re-parsing what we just wrote
    d = bytes(out)
    k2, = struct.unpack_from("<I", d, 8)
    r2, = struct.unpack_from("<I", d, 16)
    assert d[:8] == b"ANDROID!" and k2 == len(region) and r2 == rsize
    print("raw kernel=%d  gzip=%d  ramdisk=%d" % (len(img), len(kz), rsize))
    print("wrote %s (%d bytes) md5=%s" % (a.out, len(d), hashlib.md5(d).hexdigest()))
    print("re-parse OK: magic=ANDROID! ksize=%d rsize=%d ramdisk_at=%d"
          % (k2, r2, PAGE + ru(k2)))


if __name__ == "__main__":
    main()
