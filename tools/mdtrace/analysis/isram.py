# Disassemble ColdFire internal-SRAM code captured from a running machine.
# Capture: mdProbe ROM FLASH peek32:1000000:1000a00 2>&1 | grep "^   0100" > isram.txt ; then: python3 isram.py isram.txt FROM TO
import sys, capstone
b = bytearray()
for line in open(sys.argv[1]):
    for w in line.split()[1:]: b += bytes.fromhex(w)
md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_040); md.skipdata = True
a, e = int(sys.argv[2], 16), int(sys.argv[3], 16)
for i in md.disasm(bytes(b[a - 0x1000000:e - 0x1000000]), a):
    print('  %07x  %-12s %-8s %s' % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
