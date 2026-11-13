# Extract a 960L software ISO (Joliet) to a folder. Requires: pip install pycdlib
# Usage: python3 extract_iso.py 960L_V4.02.ISO out/
import sys, os, pycdlib
iso = pycdlib.PyCdlib(); iso.open(sys.argv[1]); out = sys.argv[2]
facade = None
for kind in ("rr","joliet","iso9660"):
    try:
        if kind=="rr" and iso.has_rock_ridge(): facade=("rr",iso.get_rock_ridge_facade()); break
        if kind=="joliet" and iso.has_joliet(): facade=("joliet",iso.get_joliet_facade()); break
    except Exception: pass
if facade is None: facade=("iso9660",iso.get_iso9660_facade())
print("using", facade[0])
f=facade[1]
for dirname, dirs, files in f.walk('/'):
    for fn in files:
        p = (dirname.rstrip('/')+'/'+fn)
        local = os.path.join(out, p.lstrip('/').split(';')[0])
        os.makedirs(os.path.dirname(local), exist_ok=True)
        f.get_file_from_iso(local, **({'rr_path':p} if facade[0]=='rr' else {'joliet_path':p} if facade[0]=='joliet' else {'iso_path':p}))
