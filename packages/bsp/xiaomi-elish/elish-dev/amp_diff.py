#!/usr/bin/env python3
"""amp_diff.py — 比较 Android 与 mainline 的功放寄存器/DSP 系数快照

用法:  python3 amp_diff.py android_dump.txt armbian_dump.txt

快照由 amp_dump.sh 生成。输出仅列出「值不同」的行。
"""
import sys, re

def parse(path):
    regs, coeffs = {}, {}
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.strip()
        m = re.match(r'^==\s*REG\s+bus=(\d+)\s+addr=0x([0-9a-fA-F]+)\s*==$', line)
        if m:
            cur = "%s-00%s" % (m.group(1), m.group(2).lower())
            continue
        if line.startswith('== DSP'):
            cur = 'COEFF'
            continue
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        if len(parts) < 2:
            continue
        if cur == 'COEFF':
            key, val = parts[0], ''.join(parts[1:])
            coeffs[key] = val
        elif cur:
            name, val = parts[0], ''.join(parts[1:])
            regs["%s/%s" % (cur, name)] = val
    return regs, coeffs

def norm(v):
    # i2ctransfer 输出 "0x00 0x00 0x37 0x21" / debugfs 输出 "00003721"
    v = v.replace('0x', '').replace('0X', '')
    return v.zfill(8).lower()

def main():
    if len(sys.argv) != 3:
        print(__doc__); return 1
    a_regs, a_co = parse(sys.argv[1])
    b_regs, b_co = parse(sys.argv[2])
    keys = sorted(set(a_regs) | set(b_regs))
    print("%-34s %-12s %-12s" % ("REGISTER", "ANDROID", "MAINLINE"))
    print("-" * 62)
    n = 0
    for k in keys:
        va, vb = norm(a_regs.get(k, '')), norm(b_regs.get(k, ''))
        if va != vb:
            print("%-34s %-12s %-12s" % (k, va or '-', vb or '-')); n += 1
    print("\n寄存器差异: %d 项" % n)

    keys = sorted(set(a_co) | set(b_co))
    print("\n%-34s %-26s %-26s" % ("DSP COEFF", "ANDROID", "MAINLINE"))
    print("-" * 88)
    m = 0
    for k in keys:
        va, vb = a_co.get(k, ''), b_co.get(k, '')
        if va.replace('0x', '').lower() != vb.replace('0x', '').lower():
            print("%-34s %-26s %-26s" % (k, va or '-', vb or '-')); m += 1
    print("\n系数差异: %d 项" % m)
    return 0

if __name__ == '__main__':
    sys.exit(main())
