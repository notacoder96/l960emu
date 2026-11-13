import capstone,sys,struct
d=open(sys.argv[1],'rb').read()
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM)
def lit(a): return struct.unpack_from('<I',d,a)[0] if a+4<=len(d) else 0
def dis(a,b):
    for ins in md.disasm(d[a:b],a):
        extra=''
        if ins.mnemonic.startswith('ldr') and '[pc, #' in ins.op_str:
            off=int(ins.op_str.split('#')[1].rstrip(']'),16); la=ins.address+8+off
            v=lit(la); extra=f'   ; ={v:#x}'
            if v<len(d) and 32<=d[v]<127: extra+=' "'+d[v:v+48].split(b'\0')[0].decode('latin1').replace('\n','\\n')+'"'
        print(f"{ins.address:06x}: {ins.mnemonic} {ins.op_str}{extra}")
for r in sys.argv[2:]:
    a,b=r.split('-'); dis(int(a,16),int(b,16)); print('---')
