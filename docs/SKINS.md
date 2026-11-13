# LARC2 skins

You can replace the built-in remote with images (a photo of a real LARC2, a render, and so on) without rebuilding anything.
A skin is a folder:

```
my-skin/
  skin.json          layout: where the screen, keys, faders and joystick are
  background.png     the whole remote, as one picture
  fader_cap.png      optional: the fader knob
  joystick_knob.png  optional: the joystick top
  ...                optional: per-key "up"/"down" images
```

In the plugin, click **Skin... → Load skin folder...** and pick the folder. The choice is saved with the plugin
state. **Built-in look** switches back. `skins/example` (made by `tools/make_example_skin.py`) is a working
template.

## skin.json

All coordinates are in pixels of the background image. Rectangles are `[x, y, width, height]`; `{"x":..,"y":..,"w":..,"h":..}`
also works. The skin is scaled to the plugin window and keeps its aspect ratio.

```json
{
 "background": "background.png",
 "width": 1400, "height": 760,          // optional: defaults to the background image size
 "lcd": [60, 70, 800, 300],             // where the 640x240 screen is drawn (it is stretched to fit)
 "pressedTint": "60ffffff",             // optional: overlay (AARRGGBB) on a pressed key that has no "down" image
 "showHotspots": false,                 // true: outlines every control, to line them up with a photo
 "faderCap": "fader_cap.png",           // optional: default cap image for all faders
 "keys": [
  {"name": "PROGRAM", "rect": [920, 70, 100, 50]},
  {"name": "ENTER", "rect": [1256, 380, 100, 50], "up": "enter.png", "down": "enter_lit.png"}
 ],
 "faders": [
  {"index": 0, "track": [97, 438, 40, 292], "capWidth": 40, "capHeight": 24}
 ],
 "joystick": {"rect": [1240, 470, 120, 120], "knob": "joystick_knob.png", "knobSize": 46}
}
```

(Real JSON doesn't allow the `//` comments above; leave them out.)

### Keys

`name` must be one of the LARC2's keys:

| | | | | | | | |
|---|---|---|---|---|---|---|---|
| F1 | F2 | F3 | F4 | F5 | F6 | F7 | F8 |
| 1 | 2 | 3 | PROGRAM | REGISTER | Up | JOYSTICK | |
| 4 | 5 | 6 | BANK | STORE | Left | Right | |
| 7 | 8 | 9 | EDIT | CONTROL | Down | FINE ADJUST | |
| MUTE MACH | 0 | MUTE ALL | MACHINE | ENTER | - | + | LEXICON |

F1-F8 are the soft keys under the screen.

Keys are click areas over the background. Images are optional:

- **`up`** is drawn when the key isn't pressed. For keys with lamps, the background can show them off and `up` can stay unset.
- **`down`** is drawn while the key is pressed.
- With no `down` image, a pressed key gets the `pressedTint` overlay.

You can leave keys out; a skin doesn't need every key.

### Faders

`index` 0-7 is fader 1-8. `track` is the area the cap travels in, from top (fader up, 100%) to bottom.
Make it a little wider than the slot so it is easy to grab. The cap is drawn centred horizontally in the track,
at `capWidth` x `capHeight`, which default to the image size. Clicking a fader touches it, the same as the
touch-sensitive faders on the real remote.

### Joystick

`rect` is the area the knob centre can move in: left/right and bottom/top. The knob stays where it is released.

## Making a skin from a photo

1. Take the remote face-on and crop it. Correct the perspective if needed, then save it as `background.png`.
2. Copy `skins/example/skin.json` next to it and set `"showHotspots": true`.
3. Load the folder in the plugin. Move the rectangles in `skin.json` until the magenta key outlines and the cyan
   fader and joystick areas sit on the photo. Pick **Skin... → Load skin folder...** again to reload.
4. Make the fader caps and joystick knob separate transparent PNGs. Paint the real caps out of the background,
   otherwise the photo shows a second set of caps that don't move.
5. Set `showHotspots` back to false.
