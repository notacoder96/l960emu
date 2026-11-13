import pefile, sys
p=pefile.PE(sys.argv[1]); base=p.OPTIONAL_HEADER.ImageBase
va=int(sys.argv[2],16); n=int(sys.argv[3]); w=int(sys.argv[4]) if len(sys.argv)>4 else 16
d=p.get_data(va-base,n)
for i in range(0,n,w): print(f"{va+i:08x}: "+' '.join(f"{b:02x}" for b in d[i:i+w]))
