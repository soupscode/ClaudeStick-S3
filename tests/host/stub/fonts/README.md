Glyph data copied unchanged from M5GFX 0.2.36 (`src/lgfx/Fonts`), so the host renderer draws the
fonts the firmware uses exactly as the device does: `Font16.h` (Font2), `Font32rle.h` (Font4) and
`Font64rle.h` (Font6) are the TFT_eSPI fonts M5GFX ships (FreeBSD licence, like M5GFX);
`FreeSansBold9pt7b.h` is an Adafruit GFX free font (`license.txt`). Add a file here when the
firmware starts using another built-in font, and declare it in `../lgfx_fonts.h` / `../fonts.cpp`.
