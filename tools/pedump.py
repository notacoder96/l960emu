import pefile, sys, struct
p=pefile.PE(sys.argv[1]); base=p.OPTIONAL_HEADER.ImageBase
relocs=set()
for b in getattr(p,'DIRECTORY_ENTRY_BASERELOC',[]):
    for e in b.entries:
        if e.type==3: relocs.add(base+e.rva)
def rd(va,n): return p.get_data(va-base,n)
def s(va):
    try:
        d=rd(va,64); z=d.split(b'\0')[0]
        if len(z)>=3 and all(32<=c<127 for c in z): return '"'+z.decode()+'"'
    except Exception: pass
    return ''
va=int(sys.argv[2],16); n=int(sys.argv[3])
for i in range(n):
    a=va+4*i; v=struct.unpack('<I',rd(a,4))[0]
    tag='PTR '+s(v) if a in relocs else ''
    print(f"{a:08x}: {v:08x} {v if v<0x10000000 else '':>10} {tag}")
