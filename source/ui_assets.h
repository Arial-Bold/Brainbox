/* ui_assets.h - made by tools/make_assets.py: do not edit by hand. */
#ifndef UI_ASSETS_H
#define UI_ASSETS_H
#include <gctypes.h>

/* one character of a font atlas: where it is, its size, where it sits
 * against the pen (left, top from the baseline) and how far it moves the
 * pen (in 1/16 pixels) */
typedef struct { u16 x, y; u8 w, h; s8 left, top; u16 adv16; } UiGlyph;
typedef struct { const u8 *tex; u16 w, h; u8 px, ascent, descent; const UiGlyph *glyphs; } UiFont;

extern const UiFont font_small;   /* 14 px */
extern const UiFont font_large;   /* 24 px */


#endif
