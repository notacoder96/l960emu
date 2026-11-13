import pefile, sys, struct
p=pefile.PE(sys.argv[1]); base=p.OPTIONAL_HEADER.ImageBase
t=[s for s in p.sections if s.Name.startswith(b'.text')][0]
code=t.get_data(); va=base+t.VirtualAddress
for tgt in [int(x,16) for x in sys.argv[2:]]:
    hits=[]
    for i in range(len(code)-5):
        if code[i]==0xE8:
            rel=struct.unpack('<i',code[i+1:i+5])[0]
            if va+i+5+rel==tgt: hits.append(hex(va+i))
    print(hex(tgt),hits)
