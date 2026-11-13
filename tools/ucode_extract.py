"""Extract Lexichip UCODE images (WCS + coefficient tables) from 960L algorithm DLLs.

UCODE struct (16 bytes), as consumed by ChamAPI.dll cChameleonStandardInterface::LoadLexichip:
  +0 u16  unknown (0)
  +2 u16  WCS line count (256 single speed, 128 double speed)
  +4 ptr  WCS image: 5 byte-planes, each `lines` bytes, concatenated
          -> Z80 bulk-loaded to chip offset 0x600 + 0x100*plane
  +8 u8   coefficient slot count
  +C ptr  coefficient slots, 4 bytes each (24-bit value LE + flag byte)
          -> Z80 bulk-loaded to chip offset 0x200
ALG_INIT_STRUCT +0x4C -> ptr[2] (single, double speed) -> {u32 count, ptr -> UCODE*[count]}
"""
import pefile, struct, sys, json, os

def load(path):
    p = pefile.PE(path, fast_load=False)
    base = p.OPTIONAL_HEADER.ImageBase
    rd = lambda va, n: p.get_data(va - base, n)
    u32 = lambda va: struct.unpack('<I', rd(va, 4))[0]
    exp = {s.name.decode(): base + s.address for s in p.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
    return p, base, rd, u32, exp

def get_alg_struct(rd, u32, exp):
    fn = exp['GetAlgorithmStruct']
    code = rd(fn, 6)
    assert code[0] == 0xB8 and code[5] == 0xC3, 'GetAlgorithmStruct is not mov eax,imm; ret'
    return struct.unpack('<I', code[1:5])[0]

def cstr(rd, va):
    return rd(va, 64).split(b'\0')[0].decode('latin1')

def extract(path):
    p, base, rd, u32, exp = load(path)
    alg = get_alg_struct(rd, u32, exp)
    name = cstr(rd, u32(alg + 0x10))
    speeds = u32(alg + 0x4C)
    out = {'file': os.path.basename(path), 'name': name, 'alg_struct': hex(alg), 'speeds': []}
    for si, label in enumerate(('single', 'double')):
        desc = u32(speeds + 4 * si)
        if desc == 0:
            out['speeds'].append({'speed': label, 'chips': []}); continue
        count, lst = u32(desc), u32(desc + 4)
        chips = []
        for c in range(count):
            uc = u32(lst + 4 * c)
            lines = struct.unpack('<H', rd(uc + 2, 2))[0]
            wcs_ptr, ncoef, coef_ptr = u32(uc + 4), rd(uc + 8, 1)[0], u32(uc + 12)
            wcs = rd(wcs_ptr, 5 * lines)
            coefs = list(struct.unpack('<%dI' % ncoef, rd(coef_ptr, 4 * ncoef)))
            chips.append({'ucode': hex(uc), 'lines': lines, 'wcs': wcs.hex(), 'coefs': coefs})
        out['speeds'].append({'speed': label, 'chips': chips})
    return out

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    for f in sorted(os.listdir(src)):
        if not f.lower().endswith('.dll'): continue
        try:
            r = extract(os.path.join(src, f))
        except Exception as e:
            print(f'{f}: FAILED {e}'); continue
        json.dump(r, open(os.path.join(dst, f[:-4] + '.json'), 'w'), indent=1)
        summ = '; '.join(f"{s['speed']}: " + ', '.join(f"{c['lines']}L/{len(c['coefs'])}C" for c in s['chips']) for s in r['speeds'])
        print(f"{f:14} {r['name']:22} {summ}")
