// Host text renderer: draws the real M5GFX glyph data (stub/fonts, copied from M5GFX 0.2.36) with the same
// placement rules as LovyanGFX's draw_string, so host-rendered screens match the device pixel for pixel.
#include <M5Unified.h>
#include <string.h>
#include "glcdfont.h"

namespace fonts {
  using lgfx::GFXfont; using lgfx::GFXglyph;
  #include "fonts/Font16.h"
  #include "fonts/Font32rle.h"
  #include "fonts/Font64rle.h"
  #include "fonts/FreeSansBold9pt7b.h"
  static const uint8_t font0_info[] = { 0, 255, 5 };
  const lgfx::GLCDfont Font0 = { font, font0_info, 6, 8, 7 };
  const lgfx::BMPfont  Font2 = { chrtbl_f16, widtbl_f16, 0, chr_hgt_f16, baseline_f16 };
  const lgfx::RLEfont  Font4 = { chrtbl_f32, widtbl_f32, 0, chr_hgt_f32, baseline_f32 };
  const lgfx::RLEfont  Font6 = { chrtbl_f64, widtbl_f64, 0, chr_hgt_f64, baseline_f64 };
}

namespace lgfx { inline namespace v1 {

static void box(LGFXBase* g, int x, int y, int w, int h, uint16_t c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) g->pix(x + i, y + j, c); }

int32_t GLCDfont::drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics*, int sx, int sy, uint16_t fg, const uint16_t* bg) const {
  int dw = widthtbl[2];
  if (c < widthtbl[0] || c > widthtbl[1]) { if (bg) box(g, x, y, width * sx, height * sy, *bg); return width * sx; }
  const uint8_t* col = chartbl + c * dw;
  for (int i = 0; i < width; i++) {
    uint8_t bits = i < dw ? col[i] : 0;
    for (int r = 0; r < height; r++) {
      bool on = (bits >> r) & 1;
      if (on) box(g, x + i * sx, y + r * sy, sx, sy, fg); else if (bg) box(g, x + i * sx, y + r * sy, sx, sy, *bg);
    }
  }
  return width * sx;
}

int32_t BMPfont::drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics*, int sx, int sy, uint16_t fg, const uint16_t* bg) const {
  if ((uint16_t)(c - 0x20) >= 0x60) c = 0x20;
  int fw = widthtbl[c - 0x20], bytes = (fw + 6) >> 3;   // one pixel of right margin is not in the data
  const uint8_t* rows = ((const uint8_t* const*)chartbl)[c - 0x20];
  for (int r = 0; r < height; r++) for (int i = 0; i < fw; i++) {
    bool on = i < fw - 1 && (rows[r * bytes + (i >> 3)] & (0x80 >> (i & 7)));
    if (on) box(g, x + i * sx, y + r * sy, sx, sy, fg); else if (bg) box(g, x + i * sx, y + r * sy, sx, sy, *bg);
  }
  return fw * sx;
}

int32_t RLEfont::drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics*, int sx, int sy, uint16_t fg, const uint16_t* bg) const {
  if ((uint16_t)(c - 0x20) >= 0x60) c = 0x20;
  int fw = widthtbl[c - 0x20];
  const uint8_t* p = ((const uint8_t* const*)chartbl)[c - 0x20];
  int n = fw * height, i = 0;
  while (i < n) {
    uint8_t run = *p++; bool on = run & 0x80; int len = (run & 0x7F) + 1;
    for (int k = 0; k < len && i < n; k++, i++) {
      int px = i % fw, py = i / fw;
      if (on) box(g, x + px * sx, y + py * sy, sx, sy, fg); else if (bg) box(g, x + px * sx, y + py * sy, sx, sy, *bg);
    }
  }
  return fw * sx;
}

void GFXfont::getDefaultMetric(FontMetrics* m) const {
  int ab = 0, bb = 0;
  for (int c = 0; c <= last - first; c++) {
    int a = -(int)glyph[c].yOffset; if (a > ab) ab = a;
    int b = (int)glyph[c].height - a; if (b > bb) bb = b;
  }
  m->baseline = ab; m->y_offset = -ab; m->height = ab + bb; m->y_advance = yAdvance;
  m->width = m->x_advance = yAdvance >> 1; m->x_offset = 0;
}
bool GFXfont::updateFontMetric(FontMetrics* m, uint16_t c) const {
  const GFXglyph* gl = getGlyph(c); bool ok = gl != nullptr;
  if (!gl) gl = getGlyph(0x20);
  if (!gl) { m->x_offset = 0; m->width = m->x_advance = yAdvance >> 1; return false; }
  m->x_offset = gl->xOffset; m->width = gl->width; m->x_advance = gl->xAdvance; return ok;
}
int32_t GFXfont::drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics* m, int sx, int sy, uint16_t fg, const uint16_t* bg) const {
  const GFXglyph* gl = getGlyph(c);
  if (!gl) { gl = getGlyph(0x20); if (!gl) return 0; if (bg) box(g, x, y, gl->xAdvance * sx, m->height * sy, *bg); return gl->xAdvance * sx; }
  int adv = gl->xAdvance * sx;
  if (bg) { int l = x + (gl->xOffset < 0 ? gl->xOffset * sx : 0), r = x + max(gl->width * sx + gl->xOffset * sx, adv); box(g, l, y, r - l, m->height * sy, *bg); }
  int gx = x + gl->xOffset * sx, gy = y + (m->baseline + gl->yOffset) * sy;
  const uint8_t* bits = bitmap + gl->bitmapOffset; int bit = 0;
  for (int r = 0; r < gl->height; r++) for (int i = 0; i < gl->width; i++, bit++)
    if (bits[bit >> 3] & (0x80 >> (bit & 7))) box(g, gx + i * sx, gy + r * sy, sx, sy, fg);
  return adv;
}
} }

int32_t LGFXBase::textWidth(const char* s, const lgfx::IFont* f) {
  if (!s || !*s) return 0;
  lgfx::FontMetrics m; f->getDefaultMetric(&m);
  int sx = (int)tsx, left = 0, right = 0;
  for (; *s; s++) {
    f->updateFontMetric(&m, (unsigned char)*s);
    int off = m.x_offset * sx;
    if (left == 0 && right == 0 && m.x_offset < 0) left = right = -off;
    int adv = m.x_advance * sx;
    right = left + max(adv, m.width * sx + off);
    left += adv;
  }
  return right;
}

size_t LGFXBase::drawStr(const char* s, int32_t x, int32_t y, textdatum_t d, const lgfx::IFont* f) {
  if (!s) return 0;
  lgfx::FontMetrics m; f->getDefaultMetric(&m);
  int sx = (int)tsx, sy = (int)tsy;
  int cw = textWidth(s, f), ch = m.height * sy;
  int sumX = 0;
  if (*s) { f->updateFontMetric(&m, (unsigned char)*s); if (m.x_offset < 0) sumX = -m.x_offset * sx; }
  if (d & middle_left) y -= ch >> 1; else if (d & bottom_left) y -= ch; else if (d & baseline_left) y -= m.baseline * sy;
  if (d & top_center) x -= cw >> 1; else if (d & top_right) x -= cw;
  const uint16_t* bgp = hasBg ? &bg : nullptr;
  for (; *s; s++) sumX += f->drawChar(this, x + sumX, y, (unsigned char)*s, &m, sx, sy, fg, bgp);
  return sumX;
}

void LGFXBase::print(char c) {
  font();
  if (c == '\n') { cx = 0; cy += fm.y_advance * (int)tsy; return; }
  lgfx::FontMetrics m = fm;
  curFont->updateFontMetric(&m, (unsigned char)c);
  cx += curFont->drawChar(this, cx, cy, (unsigned char)c, &m, (int)tsx, (int)tsy, fg, hasBg ? &bg : nullptr);
}
