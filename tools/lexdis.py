"""Lexichip III WCS disassembler (preliminary), reconstructed from Lexicon's LexDis.dll."""
import json, sys

def reg(n):
    if n < 0x20: return f'c{n}'
    if n < 0x40: return f'x{n-0x20}'
    if n < 0x50: return f"{'TMX' if n % 2 == 0 else 'LEX'}_out{n-0x40:02d}"
    if n < 0x60: return f"{'TMX' if n % 2 == 0 else 'LEX'}_in{n-0x40:02d}"
    if n < 0x64: return f'peak{n-0x60}'
    if n < 0x66: return f'dlmu{n-0x64}'
    return f'adf{n}'

def decode_line(w):
    b0, b1, b2, b3, b4 = w
    return dict(imm=b0, unity=b1 & 1, neg=(b1 >> 1) & 1, addscale=((b1 >> 2) & 3) or 4,
                add_en=(b1 >> 4) & 1, add_acc=(b1 >> 5) & 1, mul_acc=(b1 >> 6) & 1, dp=b1 >> 7,
                dst=b2 & 0x7f, src_acc=b2 >> 7, src=b3 & 0x7f, acc=b3 >> 7,
                inv=b4 & 1, dlmu=(b4 >> 1) & 1, nooff=(b4 >> 2) & 1, nocpc=(b4 >> 3) & 1,
                mem=(b4 >> 4) & 1, clk0=(b4 >> 5) & 1, clk1=(b4 >> 6) & 1, special=b4 >> 7)

def disasm(wcs_hex, lines, xmoq):
    raw = bytes.fromhex(wcs_hex)
    planes = [raw[k * lines:(k + 1) * lines] for k in range(5)]
    out, q = [], 0
    for i in range(lines):
        w = [planes[k][i] for k in range(5)]
        d = decode_line(w)
        parts = []
        if d['mem']:
            if q < len(xmoq):
                e = xmoq[q]; addr = e & 0xFFFFFF; r = (e >> 24) & 0x7f; write = (e >> 31) & 1
                a = (f"d{d['imm'] & 0x1f}+{addr}" if d['dlmu'] and not d['nooff'] else f'{addr}')
                parts.append(f"[{a}] = {reg(r)}" if write else f"{reg(r)} = [{a}]")
            else:
                parts.append('[?] (xmoq exhausted)')
            q += 1
        if d['unity']:
            coef = '-1.0' if d['neg'] else '1.0'
        elif d['dlmu']:
            coef = f"d{d['imm'] & 0x1f}"
        else:
            v = (~d['imm'] & 0xff) if d['inv'] else d['imm']
            coef = f"{'-' if d['neg'] else ''}{v}/256"
        x = f"acc{d['src_acc']}" if d['mul_acc'] else reg(d['src'])
        expr = f"acc{d['acc']} = {coef} * {x}"
        if d['add_en']:
            y = f"acc{d['src_acc']}" if d['add_acc'] else reg(d['src'])
            expr += f" + {y}" if d['addscale'] == 1 else f" + ({y} * {d['addscale']})"
        parts.append(expr)
        if d['dst'] < 0x65:
            parts.append(f"{reg(d['dst'])} = acc{d['src_acc']}")
        flags = [n for n in ('inv', 'clk0', 'clk1', 'special', 'dp', 'nocpc') if d[n]]
        out.append(f"{i:3d}: {' '.join(f'{b:02x}' for b in w)}   " + '; '.join(parts) + (f"   ; {' '.join(flags)}" if flags else ''))
    return out, q

if __name__ == '__main__':
    alg = json.load(open(sys.argv[1]))
    speed = sys.argv[2] if len(sys.argv) > 2 else 'single'
    for s in alg['speeds']:
        if s['speed'] != speed: continue
        for ci, c in enumerate(s['chips']):
            lst, used = disasm(c['wcs'], c['lines'], c['coefs'])
            print(f"; {alg['name']} {speed}-speed chip {ci}: {c['lines']} lines, {len(c['coefs'])} xmoq entries, {used} memory-op lines")
            print('\n'.join(lst))
