import sys
src, dst = sys.argv[1], sys.argv[2]
extra = " panic=10 hardlockup_panic=1 softlockup_panic=1 hung_task_panic=1"
d = bytearray(open(src, 'rb').read())
cmd = bytes(d[64:64+512]).split(b'\x00')[0]
new = cmd + extra.encode()
assert len(new) < 500, "cmdline too long"
d[64:64+len(new)] = new
d[64+len(new)] = 0
open(dst, 'wb').write(bytes(d))
print("cmdline: %s" % new.decode())
