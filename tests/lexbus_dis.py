# Dev tool: disassembles a Lexichip bus dump (3072 bytes: tests/lexi_capture or l960_bridge L960_DUMP_LEX), using the
# programmed XMOQ addresses and the current DLMU values. Semantics: docs/LEXICHIP.md.
# Usage: python3 tests/lexbus_dis.py lex0.bin
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from lexdis import reg, decode_line
b=open(sys.argv[1],'rb').read()
b=b+bytes(max(0,0xb00-len(b)))
q=0
for i in range(256):
    w=[b[0x600+0x100*p+i] for p in range(5)]
    d=decode_line(w); parts=[]
    if d['mem']:
        e=int.from_bytes(b[0x200+4*q:0x204+4*q],'little'); q+=1
        a=e&0xffffff; r=(e>>24)&0x7f; wr=e>>31
        parts.append(f"[{a:05x}] = {reg(r)}" if wr else f"{reg(r)} = [{a:05x}]")
    u=d['imm']&0x1f
    if d['unity']: coef='-1.0' if d['neg'] else '1.0'
    elif d['dlmu']:
        p=[b[0x400+0x20*j+u] for j in range(9)]
        coef=f"d{u}({'-' if d['neg']^(p[6]&1) else ''}{(p[5]<<8|p[4])/65536:.3f}{'>'+format(p[3]/256,'.3f') if p[0] else ''}{' STEP' if d['imm']&0x20 else ''})"
    else:
        v=(~d['imm']&0xff) if d['inv'] else d['imm']; coef=f"{'-' if d['neg'] else ''}{v/256:.3f}"
    x=f"acc{d['src_acc']}" if d['mul_acc'] else reg(d['src'])
    e=f"acc{d['acc']} = {coef}*{x}"
    if d['add_en']:
        y=f"acc{d['src_acc']}" if d['add_acc'] else reg(d['src'])
        e+=f" + {y}" + (f"*{d['addscale']}" if d['addscale']!=1 else '')
    parts.append(e)
    if d['dst']<0x65: parts.append(f"{reg(d['dst'])} = acc{d['src_acc']}")
    fl=[n for n in ('inv','clk0','clk1','special','dp','nocpc') if d[n]]
    print(f"{i:3d} " + ' ; '.join(parts) + ('   !'+' '.join(fl) if fl else ''))
