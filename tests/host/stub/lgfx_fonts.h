// Host stand-in for LovyanGFX's font classes: the same class names, constructors and
// metrics as M5GFX (lgfx_fonts.hpp), with glyph data copied from M5GFX into stub/fonts/
// so the host renderer draws exactly what the device will.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct LGFXBase;

namespace lgfx {
  inline namespace v1 {
    struct FontMetrics { int32_t width = 0, x_advance = 0, x_offset = 0, height = 0, y_advance = 0, y_offset = 0, baseline = 0; };

    struct IFont {
      enum font_type_t { ft_unknown, ft_glcd, ft_bmp, ft_rle, ft_gfx };
      virtual font_type_t getType() const { return ft_unknown; }
      virtual void getDefaultMetric(FontMetrics* m) const = 0;
      virtual bool updateFontMetric(FontMetrics* m, uint16_t c) const = 0;
      // Draw one glyph with (x, y) the top-left of the line box, integer scale (sx, sy). Returns the x advance.
      virtual int32_t drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics* m, int sx, int sy, uint16_t fg, const uint16_t* bg) const = 0;
      virtual ~IFont() {}
    };

    struct BaseFont : IFont {
      const uint8_t* chartbl; const uint8_t* widthtbl; uint8_t width, height, baseline;
      constexpr BaseFont(const void* t, const uint8_t* w, uint8_t ww, uint8_t hh, uint8_t bl)
        : chartbl((const uint8_t*)t), widthtbl(w), width(ww), height(hh), baseline(bl) {}
      void getDefaultMetric(FontMetrics* m) const override {
        m->width = width; m->x_advance = width; m->x_offset = 0; m->height = height; m->y_advance = height; m->y_offset = 0; m->baseline = baseline;
      }
    };
    struct GLCDfont : BaseFont {   // 5x7 column glyphs in a 6x8 cell (Font0 and the firmware's termFont)
      constexpr GLCDfont(const void* t, const uint8_t* w, uint8_t ww, uint8_t hh, uint8_t bl) : BaseFont(t, w, ww, hh, bl) {}
      font_type_t getType() const override { return ft_glcd; }
      bool updateFontMetric(FontMetrics*, uint16_t c) const override { return widthtbl[0] <= c && c <= widthtbl[1]; }
      int32_t drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics* m, int sx, int sy, uint16_t fg, const uint16_t* bg) const override;
    };
    struct BMPfont : BaseFont {    // 1 bpp row bitmaps, proportional (Font2)
      constexpr BMPfont(const void* t, const uint8_t* w, uint8_t ww, uint8_t hh, uint8_t bl) : BaseFont(t, w, ww, hh, bl) {}
      font_type_t getType() const override { return ft_bmp; }
      bool updateFontMetric(FontMetrics* m, uint16_t c) const override {
        bool ok = (uint16_t)(c - 0x20) < 0x60; if (!ok) c = 0x20;
        m->x_advance = m->width = widthtbl[c - 0x20]; return ok;
      }
      int32_t drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics* m, int sx, int sy, uint16_t fg, const uint16_t* bg) const override;
    };
    struct RLEfont : BMPfont {     // run-length encoded (Font4, Font6, Font7, Font8)
      constexpr RLEfont(const void* t, const uint8_t* w, uint8_t ww, uint8_t hh, uint8_t bl) : BMPfont(t, w, ww, hh, bl) {}
      font_type_t getType() const override { return ft_rle; }
      int32_t drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics* m, int sx, int sy, uint16_t fg, const uint16_t* bg) const override;
    };

    struct GFXglyph { uint32_t bitmapOffset; uint8_t width, height, xAdvance; int8_t xOffset, yOffset; };
    struct GFXfont : IFont {       // Adafruit GFX free fonts (FreeSans*, DejaVu*, TomThumb, ...)
      uint8_t* bitmap; GFXglyph* glyph; uint16_t first, last; uint8_t yAdvance;
      constexpr GFXfont(uint8_t* b, GFXglyph* gl, uint16_t f, uint16_t l, uint8_t ya) : bitmap(b), glyph(gl), first(f), last(l), yAdvance(ya) {}
      font_type_t getType() const override { return ft_gfx; }
      const GFXglyph* getGlyph(uint16_t c) const { return (c < first || c > last) ? nullptr : &glyph[c - first]; }
      void getDefaultMetric(FontMetrics* m) const override;
      bool updateFontMetric(FontMetrics* m, uint16_t c) const override;
      int32_t drawChar(LGFXBase* g, int32_t x, int32_t y, uint16_t c, FontMetrics* m, int sx, int sy, uint16_t fg, const uint16_t* bg) const override;
    };
  }
}

namespace fonts {
  using lgfx::IFont;
  extern const lgfx::GLCDfont Font0;
  extern const lgfx::BMPfont  Font2;
  extern const lgfx::RLEfont  Font4, Font6;
  extern const lgfx::GFXfont  FreeSansBold9pt7b;
}
