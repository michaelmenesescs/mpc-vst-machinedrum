import sys, capstone
import os
SEC0=os.environ.get('MD_SECTION0', 'section0.bin')   # tools/mdfw: mdfw OS.syx --dump DIR
d=open(SEC0,'rb').read(); BASE=0x200000
md=capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_040)
md.skipdata=True
def dis(a,b):
    for i in md.disasm(d[a-BASE:b-BASE], a):
        print('  %06x  %-10s %-8s %s'%(i.address, i.bytes.hex(), i.mnemonic, i.op_str))
if __name__=='__main__':
    for k in range(1,len(sys.argv),2): dis(int(sys.argv[k],16), int(sys.argv[k+1],16))
