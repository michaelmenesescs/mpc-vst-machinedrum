# List the ColdFire OS machine descriptor table (MD OS 1.63): id, name, param names, defaults, coefficient fn.
# Input: decompressed section 0 of the OS .syx (tools/mdfw: mdfw OS.syx --dump DIR -> DIR/section0.bin).
# The table is located by its 86-byte record shape around the TRX-B2 entry; addresses assume load base $200000.
import os, struct, sys
d = open(sys.argv[1] if len(sys.argv) > 1 else os.environ.get('MD_SECTION0', 'section0.bin'), 'rb').read()
BASE, R = 0x200000, 86
def rec(o):
    fn, = struct.unpack_from('>I', d, o)
    params = [d[o+10+4*k:o+14+4*k].split(b'\0')[0].decode('latin1') for k in range(8)]
    return fn, d[o+4], d[o+5:o+10].decode('latin1').strip('\0'), params, list(d[o+42:o+50])
def valid(o):
    if o < 0 or o + R > len(d): return False
    fn = struct.unpack_from('>I', d, o)[0]
    return BASE <= fn < BASE + len(d) and all(32 <= c < 127 or c == 0 for c in d[o+5:o+10])
anchor = d.find(b'TRXB2') - 5
start = anchor
while valid(start - R): start -= R
o = start
print('table at $%06x' % (BASE + start))
while valid(o):
    fn, mid, name, params, dfl = rec(o)
    print('%3d %-6s fn=$%06x  %-40s defaults=%s' % (mid, name, fn, ' '.join(p or '-' for p in params), dfl))
    o += R
