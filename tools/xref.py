import pefile, capstone, sys
p=pefile.PE(sys.argv[1]); base=p.OPTIONAL_HEADER.ImageBase
data=open(sys.argv[1],'rb').read()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
text=[s for s in p.sections if s.Name.startswith(b'.text')][0]
code=text.get_data(); tva=base+text.VirtualAddress
for needle in sys.argv[2:]:
    nb=needle.encode()
    for sec in p.sections:
        d=sec.get_data(); i=d.find(nb)
        while i>=0:
            va=base+sec.VirtualAddress+i
            refs=[]
            le=va.to_bytes(4,'little'); j=code.find(le)
            while j>=0: refs.append(hex(tva+j)); j=code.find(le,j+1)
            print(repr(needle),hex(va),refs)
            i=d.find(nb,i+1)
