"""Generates skins/example: a drawn LARC2-style panel + skin.json, as a starting point for photo-based skins.
Usage: python3 tools/make_example_skin.py [out_dir]"""
import json, os, sys
from PIL import Image, ImageDraw, ImageFont

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'skins', 'example')
os.makedirs(out, exist_ok=True)
W, H = 1400, 760
img = Image.new('RGB', (W, H), (38, 40, 44))
d = ImageDraw.Draw(img)
def font(sz):
    for f in ('DejaVuSans-Bold.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', 'arialbd.ttf'):
        try: return ImageFont.truetype(f, sz)
        except OSError: pass
    return ImageFont.load_default()
f12, f10, f20 = font(13), font(10), font(22)

# chassis
d.rounded_rectangle((8, 8, W - 8, H - 8), 18, fill=(52, 54, 58), outline=(20, 20, 22), width=3)
d.text((40, 24), 'LEXICON', font=f20, fill=(230, 230, 230))
d.text((176, 30), 'LARC2', font=f12, fill=(180, 180, 180))

skin = {'background': 'background.png', 'width': W, 'height': H, 'faderCap': 'fader_cap.png', 'keys': [], 'faders': []}

# LCD
lcd = (60, 70, 640 * 1.25, 240 * 1.25)
x, y, w, h = lcd
d.rectangle((x - 14, y - 14, x + w + 14, y + h + 14), fill=(12, 12, 14))
skin['lcd'] = [x, y, w, h]

def key(name, rect, label=None):
    x, y, w, h = rect
    d.rounded_rectangle((x, y, x + w, y + h), 6, fill=(70, 73, 78), outline=(15, 15, 15), width=2)
    t = label or name
    fnt = f10 if len(t) > 7 else f12
    tw = d.textlength(t, font=fnt)
    d.text((x + (w - tw) / 2, y + h / 2 - 7), t, font=fnt, fill=(235, 235, 235))
    skin['keys'].append({'name': name, 'rect': [x, y, w, h]})

# soft keys F1-F8 under the LCD, faders under them (one strip per soft key)
sw = w / 8
for i in range(8):
    kx = x + i * sw + 8
    key('F%d' % (i + 1), (kx, y + h + 26, sw - 16, 30), '')
    tx, ty, tw_, th = kx + (sw - 16) / 2 - 7, y + h + 74, 14, 280
    d.rounded_rectangle((tx, ty, tx + tw_, ty + th), 7, fill=(10, 10, 10))
    for k in range(11):
        yy = ty + 10 + k * (th - 20) / 10
        d.line((tx - 12, yy, tx - 4, yy), fill=(140, 140, 140), width=1)
    skin['faders'].append({'index': i, 'track': [tx - 13, ty - 6, tw_ + 26, th + 12]})

# keypad on the right
kx0, ky0, kw, kh, gap = 920, 70, 100, 50, 12
rows = [["PROGRAM", "REGISTER", "BANK", "STORE"],
        ["EDIT", "CONTROL", "MACHINE", "JOYSTICK"],
        ["1", "2", "3", "MUTE MACH"],
        ["4", "5", "6", "MUTE ALL"],
        ["7", "8", "9", "FINE ADJUST"],
        ["-", "0", "+", "ENTER"]]
for r, row in enumerate(rows):
    for c, name in enumerate(row):
        key(name, (kx0 + c * (kw + gap), ky0 + r * (kh + gap), kw, kh))
# cursor keys
cx, cy = 1000, 470
key('Up', (cx + 60, cy, 60, 40), '▲'); key('Down', (cx + 60, cy + 90, 60, 40), '▼')
key('Left', (cx, cy + 45, 60, 40), '◀'); key('Right', (cx + 120, cy + 45, 60, 40), '▶')
key('LEXICON', (cx + 60, cy + 45, 60, 40), 'LEX')

# joystick well
jx, jy, js = 1240, 470, 120
d.ellipse((jx - 10, jy - 10, jx + js + 10, jy + js + 10), fill=(25, 25, 27), outline=(10, 10, 10), width=3)
skin['joystick'] = {'rect': [jx, jy, js, js], 'knob': 'joystick_knob.png', 'knobSize': 46}

img.save(os.path.join(out, 'background.png'))

cap = Image.new('RGBA', (40, 24), (0, 0, 0, 0)); cd = ImageDraw.Draw(cap)
cd.rounded_rectangle((0, 0, 39, 23), 4, fill=(200, 200, 205), outline=(40, 40, 40), width=2)
cd.line((4, 12, 35, 12), fill=(20, 20, 20), width=2)
cap.save(os.path.join(out, 'fader_cap.png'))
for f in skin['faders']: f['capWidth'], f['capHeight'] = 40, 24

knob = Image.new('RGBA', (64, 64), (0, 0, 0, 0)); kd = ImageDraw.Draw(knob)
kd.ellipse((2, 2, 61, 61), fill=(60, 60, 64), outline=(10, 10, 10), width=3)
kd.ellipse((16, 16, 47, 47), fill=(216, 176, 74))
knob.save(os.path.join(out, 'joystick_knob.png'))

import re
text = json.dumps(skin, indent=1)
text = re.sub(r'\[\s+([^\[\]{}]*?)\s+\]', lambda m: '[' + re.sub(r'\s+', ' ', m.group(1)) + ']', text)  # rects on one line
text = re.sub(r'\{\s+("name"[^{}]*?)\s+\}', lambda m: '{' + re.sub(r'\s+', ' ', m.group(1)) + '}', text)
text = re.sub(r'\{\s+("index"[^{}]*?)\s+\}', lambda m: '{' + re.sub(r'\s+', ' ', m.group(1)) + '}', text)
with open(os.path.join(out, 'skin.json'), 'w') as fp:
    fp.write(text + '\n')
print('wrote', out)
