import pefile, capstone, sys
p=pefile.PE(sys.argv[1]); base=p.OPTIONAL_HEADER.ImageBase
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
text=[s for s in p.sections if s.Name.startswith(b'.text')][0]
code=text.get_data(); va=base+text.VirtualAddress
start=int(sys.argv[2],16) if len(sys.argv)>2 else va
n=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
imps={}
for e in getattr(p,'DIRECTORY_ENTRY_IMPORT',[]):
    for i in e.imports: imps[i.address]=(e.dll.decode()+'!'+(i.name.decode() if i.name else str(i.ordinal)))
c=0
for ins in md.disasm(code[start-va:],start):
    extra=''
    for a,nm in imps.items():
        if hex(a) in ins.op_str: extra='   ; '+nm
    print(f"{ins.address:08x}: {ins.mnemonic:6} {ins.op_str}{extra}")
    c+=1
    if c>=n: break
