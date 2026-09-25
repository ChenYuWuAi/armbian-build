#!/usr/bin/env python3
"""解析 Qualcomm ACDB（Forte_*_cal.acdb）与小米出厂标定。

用法:
    python3 parse_acdb.py tags   Forte_Speaker_cal.acdb      # 列出所有 chunk 标签
    python3 parse_acdb.py avol   Forte_Speaker_cal.acdb      # 解码 AVOLLUT0 逐音量档曲线
    python3 parse_acdb.py dprop  Forte_Speaker_cal.acdb      # 解码 DPROPLUT 逐设备保护参数
    python3 parse_acdb.py persist /mnt/persist_probe/audio   # 读 cs35l41_cal_spk*.txt

ACDB chunk 格式: <4B tag><4B size LE><payload(size)>，根 QCMSNDDB -> AVDB/AMDB -> ...
AVOLLUT0 每条 5 个 u32: {1, param_id, index(0..15), value_a, value_b}
DPROPLUT 每条 3 个 u32: {device_n, param_id, value}
"""
import collections
import glob
import os
import re
import struct
import sys

# 本机权威映射: persist spk№ -> 功放前缀 (cal_r 由源码表 + dmesg 交叉确认)
SPK2AMP = {1: "TRH", 2: "TLH", 3: "TRL", 4: "TLL",
           5: "BRH", 6: "BLH", 7: "BRL", 8: "BLL"}
# cal_r ≈ Re(Ω) × 1398
CAL_R_PER_OHM = 1398


def chunk(d, tag):
    """返回 tag 首个 chunk 的 payload。"""
    i = d.find(tag if isinstance(tag, bytes) else tag.encode())
    if i < 0:
        return None
    sz = struct.unpack_from("<I", d, i + 8)[0]
    if sz == 0 or i + 12 + sz > len(d):
        return None
    return d[i + 12:i + 12 + sz]


def cmd_tags(fn):
    d = open(fn, "rb").read()
    print("%s  %d B" % (fn, len(d)))
    for m in re.finditer(rb"[A-Z][A-Z0-9 _]{4,11}", d):
        print("   @0x%06x  %s" % (m.start(), m.group().decode(errors="replace")))


def cmd_avol(fn):
    p = chunk(open(fn, "rb").read(), "AVOLLUT0")
    if p is None:
        print("没有 AVOLLUT0")
        return
    n, per = struct.unpack_from("<I", p, 0)[0], (len(p) - 4) // 4 // struct.unpack_from("<I", p, 0)[0]
    print("AVOLLUT0: %d 条 × %d u32 = %d B" % (n, per, len(p) + 12))
    ent = [struct.unpack_from("<%dI" % per, p, 4 + 4 * per * k) for k in range(n)]
    groups = collections.defaultdict(dict)
    seen = collections.Counter()
    for e in ent:
        pid, idx = e[1], e[2]
        rep = seen[pid] // 16
        seen[pid] += 1
        groups[(pid, rep)][idx] = e[-1]
    for (pid, rep), v in sorted(groups.items()):
        seq = [v.get(t, 0) for t in range(16)]
        if not any(seq):
            continue
        step = seq[1] - seq[0] if len(seq) > 1 else 0
        print("  param %-6d rep%-3d 顶=%-5d 步长=%-4d  %s"
              % (pid, rep, max(seq), step, " ".join(str(x) for x in seq)))


def cmd_dprop(fn):
    p = chunk(open(fn, "rb").read(), "DPROPLUT")
    if p is None:
        print("没有 DPROPLUT")
        return
    c = struct.unpack_from("<I", p, 0)[0]
    dev = collections.OrderedDict()
    for k in range(min(c, (len(p) - 4) // 12)):
        n, pid, val = struct.unpack_from("<3I", p, 4 + 12 * k)
        dev.setdefault(n, []).append((pid, val))
    print("DPROPLUT: count=%d, %d 个设备实例" % (c, len(dev)))
    for n, lst in dev.items():
        print("  dev n=%-6d %s" % (n, " ".join("%d=%d" % (a, b) for a, b in lst)))


def cmd_persist(path):
    if os.path.isfile(path):
        files = [path]
    else:
        files = sorted(glob.glob(os.path.join(path, "cs35l41_cal_spk*.txt")))
    print("persist 出厂标定 (逐只):")
    for f in files:
        m = re.search(r"spk(\d+)", f)
        n = int(m.group(1)) if m else 0
        t = open(f).read().strip()
        imp = re.search(r"Impedance\s*=\s*([\d.]+)", t)
        cal = re.search(r"cal_r\s*=\s*(\d+)", t)
        print("  spk%d -> %-4s %s  (Re=%sΩ cal_r=%s)"
              % (n, SPK2AMP.get(n, "?"), t,
                 imp.group(1) if imp else "?", cal.group(1) if cal else "?"))
    print("\n  cal_r ≈ Re x %d；cal_r 只在 DSP 首次启动时推，"
          "验证前需 dmesg -C 再触发一次播放。" % CAL_R_PER_OHM)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    {"tags": cmd_tags, "avol": cmd_avol,
     "dprop": cmd_dprop, "persist": cmd_persist}[sys.argv[1]](sys.argv[2])
