"""Parse a Windows CE .bin image (B000FF) into a flat RAM image + ROMHDR module/file tables."""
import struct, sys, json
def load(path):
    d=open(path,'rb').read(); assert d[:7]==b'B000FF\n'
    start,length=struct.unpack_from('<II',d,7); off=15; recs=[]; entry=None
    while off+12<=len(d):
        a,l,c=struct.unpack_from('<III',d,off); off+=12
        if a==0: entry=l; break
        recs.append((a,d[off:off+l])); off+=l
    lo=min(a for a,_ in recs); hi=max(a+len(b) for a,b in recs)
    img=bytearray(hi-lo)
    for a,b in recs: img[a-lo:a-lo+len(b)]=b
    return lo,img,entry,recs
def u32(img,lo,va): return struct.unpack_from('<I',img,va-lo)[0]
def cstr(img,lo,va):
    o=va-lo; e=img.index(b'\0',o); return img[o:e].decode('latin1')
if __name__=='__main__':
    lo,img,entry,recs=load(sys.argv[1])
    print('image %08x-%08x entry %08x, %d records'%(lo,lo+len(img),entry or 0,len(recs)))
    sig=img[0x40:0x44]; print('sig@+0x40',sig, 'ROMHDR ptr %08x'%u32(img,lo,lo+0x44))
    rh=u32(img,lo,lo+0x44)
    f=struct.unpack_from('<IIIIIIIIIIIIIIIHHII',img,rh-lo)
    names=['dllfirst','dlllast','physfirst','physlast','nummods','ulRAMStart','ulRAMFree','ulRAMEnd','ulCopyEntries','ulCopyOffset','ulProfileLen','ulProfileOffset','numfiles','ulKernelFlags','ulFSRamPercent','ulDrivglobStart','ulDrivglobLen','usCPUType','usMiscFlags','pExtensions','ulTrackingStart','ulTrackingLen']
    hdr=dict(zip(names,f)); print({k:(hex(v)) for k,v in hdr.items()})
    # TOCentry: dwFileAttributes, ftTime(8), nFileSize, lpszFileName, ulE32Offset, ulO32Offset, ulLoadOffset = 32 bytes
    toc=rh+84
    mods=[]
    for i in range(hdr['nummods']):
        e=struct.unpack_from('<IIIIIIII',img,toc+32*i-lo)
        mods.append((cstr(img,lo,e[4]),e[3]))
    print('MODULES:',' '.join(f'{n}({s})' for n,s in mods))
    ftoc=toc+32*hdr['nummods']
    files=[]
    for i in range(hdr['numfiles']):
        e=struct.unpack_from('<IIIIIII',img,ftoc+28*i-lo)
        files.append((cstr(img,lo,e[5]),e[3]))
    print('FILES:',' '.join(f'{n}({s})' for n,s in files))
