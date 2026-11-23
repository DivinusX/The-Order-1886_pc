# Button prompt artwork

This folder holds no game pixel. The keycaps are drawn at build time with Pillow fonts, in
the game's own palette; the Xbox set composites the letter marks from `marks/` onto the
player's own textures.

- `plan.json`: for each of the 308 glyph bitmaps, the button it shows and its role in the
  Xbox and the keyboard set (`face`, `label` or `keep`), reviewed against the source
  bitmaps. It holds no pixels. It also names bitmaps that no button class owns but that
  show a button (the weapon HUD's R1 and R2, the button-mash crosses); `xbox_glyph` and
  `keyboard_glyph` override the glyph for one set.
- `marks/`: the letter silhouettes the Xbox set uses (A, B, X, Y, LB, RB, LT, RT, LS, RS,
  MENU, VIEW). They were generated with an image model in the game's engraved serif style;
  `RT` was drawn by hand by the project owner. Only each mark's shape is used: ink,
  softness and transparency come from the player's own texture.
- `keyboard-bindings.json`: the default key layout the Keyboard and mouse prompts draw when
  the Controls page changes nothing. With custom bindings the prompts follow them instead.

`scripts/order1886_prompts.py` composes the selected set from the player's own game files.
PlayStation prompts (the default, and the original layout) paint nothing: the game's own
DualShock 4 artwork ships with every edition.
