#include "cardfish.h"
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>

extern "C" {
#include <lgfx/utility/lgfx_pngle.h>
}

// --- wiring -----------------------------------------------------------------
// The pins are the ones the SD module is already wired to. The *bus* is not:
// this board's panel is on SPI2 (lgfx_setup.h says SPI2_HOST, which Arduino
// calls FSPI), so the card has to go on SPI3 - Arduino's HSPI - or the two
// fight over the same peripheral and neither works. Both names are relative to
// the chip, not to the pins, and on the S3 any pin can reach any bus through
// the GPIO matrix, so only the host number matters here.
static const int SD_SCLK = 5;
static const int SD_MISO = 6;
static const int SD_MOSI = 7;
static const int SD_CS   = 4;
static const uint32_t SD_FREQ = 20000000;

static const char* CARD_FILE = "/fish.png";

// A drawing, a scan or a phone photo can be any size. This is the largest one
// we will decode: 4 bytes a pixel in PSRAM, and it is thrown away as soon as it
// has been reduced to 32x32.
static const uint32_t MAX_SRC = 1024;

const RGBA8* CARD_ART = nullptr;
static RGBA8 cardPixels[CARD_W * CARD_H];

// ---------------------------------------------------------------------------
// PNG decode.
//
// LovyanGFX can draw a PNG to a sprite, but drawing is exactly what we do not
// want: it composites the alpha away against whatever the sprite already held,
// and the alpha is the only reason the card works. Its decoder underneath -
// pngle - hands out straight RGBA a run at a time, so it gets used directly.
// ---------------------------------------------------------------------------
struct PngCtx {
  File*    f;
  uint8_t* rgba;        // w*h*4, R,G,B,A
  uint32_t w, h;
};

// Two things about this callback are not optional, and getting either wrong
// looks like "your PNG is broken" rather than like a bug here.
//
// A null buffer is not a read, it is a *skip*. pngle asks for one every time it
// meets a chunk it does not care about, and it checks that the full length came
// back - so a card written by anything except a bare-bones encoder (a phone, a
// paint program, an export with a colour profile: pHYs, sRGB, tEXt, iCCP, eXIf)
// hits this on its first ancillary chunk and dies there. Our own sample never
// did, because PIL writes nothing but IHDR, IDAT and IEND.
//
// And a short read is not the end of the file. SD returns what it has to hand;
// pngle compares the count against what it asked for and treats any shortfall
// as truncation, so the loop has to keep asking until it really is empty.
static uint32_t pngRead(void* user, uint8_t* buf, uint32_t len) {
  PngCtx* c = (PngCtx*)user;
  if (!buf) {
    c->f->seek(c->f->position() + len);
    return len;
  }
  uint32_t got = 0;
  while (got < len) {
    int n = c->f->read(buf + got, len - got);
    if (n <= 0) break;
    got += (uint32_t)n;
  }
  return got;
}

static void pngDraw(void* user, uint32_t x, uint32_t y, uint_fast8_t div_x,
                    size_t len, const uint8_t* argb) {
  PngCtx* c = (PngCtx*)user;
  if (y >= c->h) return;
  uint8_t* row = c->rgba + (size_t)y * c->w * 4;
  for (size_t i = 0; i < len; i++) {
    const uint32_t xx = x + i * div_x;        // div_x > 1 only when interlaced
    if (xx >= c->w) break;
    uint8_t* d = row + (size_t)xx * 4;
    // pngle packs a pixel as A in the low byte and then R, G, B
    d[0] = argb[i * 4 + 1];
    d[1] = argb[i * 4 + 2];
    d[2] = argb[i * 4 + 3];
    d[3] = argb[i * 4 + 0];
  }
}

// ---------------------------------------------------------------------------
// Reduce whatever was decoded to the sprite.
//
// Two things happen here that are not just a resize:
//
//   crop   to the opaque part. People leave margins, and a scan has margins
//          whether they meant them or not. Fitting the canvas instead of the
//          drawing would put a postage stamp in the tank.
//   fit    preserving aspect, centred. The sprite is square and the drawing
//          will not be; stretching somebody's fish to fill it is the one thing
//          they would notice.
//
// The box filter runs on premultiplied alpha and un-premultiplies at the end,
// which is what stops a soft edge picking up the colour of the empty pixels
// beside it - the halo you get from averaging RGB across a transparent border.
// ---------------------------------------------------------------------------
static bool reduceToSprite(const uint8_t* src, uint32_t sw, uint32_t sh) {
  uint32_t x0 = sw, y0 = sh, x1 = 0, y1 = 0;
  for (uint32_t y = 0; y < sh; y++) {
    const uint8_t* p = src + (size_t)y * sw * 4 + 3;
    for (uint32_t x = 0; x < sw; x++, p += 4) {
      if (*p < 8) continue;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
  }
  if (x1 < x0 || y1 < y0) {
    Serial.println("card: fish.png is entirely transparent");
    return false;
  }
  const uint32_t cw = x1 - x0 + 1, ch = y1 - y0 + 1;

  // the scale that makes the longer side exactly fill the sprite
  const float s = (cw * CARD_H >= ch * CARD_W) ? (float)CARD_W / cw
                                               : (float)CARD_H / ch;
  const int dw = (int)(cw * s + 0.5f) < 1 ? 1 : (int)(cw * s + 0.5f);
  const int dh = (int)(ch * s + 0.5f) < 1 ? 1 : (int)(ch * s + 0.5f);
  const int ox = (CARD_W - dw) / 2, oy = (CARD_H - dh) / 2;

  memset(cardPixels, 0, sizeof(cardPixels));
  for (int dy = 0; dy < dh; dy++) {
    // the source rows this destination row averages over
    const uint32_t sy0 = y0 + (uint32_t)((uint64_t)dy * ch / dh);
    uint32_t sy1 = y0 + (uint32_t)((uint64_t)(dy + 1) * ch / dh);
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int dx = 0; dx < dw; dx++) {
      const uint32_t sx0 = x0 + (uint32_t)((uint64_t)dx * cw / dw);
      uint32_t sx1 = x0 + (uint32_t)((uint64_t)(dx + 1) * cw / dw);
      if (sx1 <= sx0) sx1 = sx0 + 1;

      uint32_t ar = 0, ag = 0, ab = 0, aa = 0, n = 0;
      for (uint32_t y = sy0; y < sy1 && y < sh; y++) {
        const uint8_t* p = src + ((size_t)y * sw + sx0) * 4;
        for (uint32_t x = sx0; x < sx1 && x < sw; x++, p += 4) {
          const uint32_t a = p[3];
          ar += p[0] * a; ag += p[1] * a; ab += p[2] * a; aa += a;
          n++;
        }
      }
      if (!n) continue;
      RGBA8& o = cardPixels[(oy + dy) * CARD_W + (ox + dx)];
      o.a = (uint8_t)(aa / n);
      if (aa) {
        o.r = (uint8_t)(ar / aa);
        o.g = (uint8_t)(ag / aa);
        o.b = (uint8_t)(ab / aa);
      }
    }
  }
  Serial.printf("card: %ux%u -> crop %ux%u -> sprite %dx%d at (%d,%d)\n",
                sw, sh, cw, ch, dw, dh, ox, oy);
  return true;
}

static bool cardLoadAt(uint32_t freq) {
  static SPIClass sdSPI(HSPI);
  sdSPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, freq)) {
    Serial.println("card: no SD card (tank runs without one)");
    sdSPI.end();
    return false;
  }

  File f = SD.open(CARD_FILE, FILE_READ);
  if (!f) {
    Serial.printf("card: %s not found\n", CARD_FILE);
    SD.end();
    sdSPI.end();
    return false;
  }

  bool ok = false;
  uint8_t* rgba = nullptr;
  pngle_t* png = lgfx_pngle_new();
  PngCtx ctx = { &f, nullptr, 0, 0 };

  if (!png) {
    Serial.println("card: out of memory for the decoder");
  } else if (lgfx_pngle_prepare(png, pngRead, &ctx) < 0) {
    Serial.printf("card: %s is not a PNG\n", CARD_FILE);
  } else {
    ctx.w = lgfx_pngle_get_width(png);
    ctx.h = lgfx_pngle_get_height(png);
    if (!ctx.w || !ctx.h || ctx.w > MAX_SRC || ctx.h > MAX_SRC) {
      Serial.printf("card: %ux%u is out of range (max %ux%u)\n",
                    ctx.w, ctx.h, MAX_SRC, MAX_SRC);
    } else {
      const size_t bytes = (size_t)ctx.w * ctx.h * 4;
      rgba = (uint8_t*)heap_caps_calloc(bytes, 1, MALLOC_CAP_SPIRAM);
      if (!rgba) {
        Serial.printf("card: no PSRAM for %u bytes\n", (unsigned)bytes);
      } else {
        ctx.rgba = rgba;
        if (lgfx_pngle_decomp(png, pngDraw) < 0) {
          // Say what the file actually is. Every remaining way this can fail is
          // a property of the encoding, and without this the only thing to go
          // on is "it did not work".
          const pngle_ihdr_t* h = lgfx_pngle_get_ihdr(png);
          Serial.printf("card: PNG decode failed"
                        " (%ux%u, %u-bit, colour type %u, interlace %u)\n",
                        ctx.w, ctx.h, h ? h->depth : 0,
                        h ? h->color_type : 0, h ? h->interlace : 0);
        } else {
          ok = reduceToSprite(rgba, ctx.w, ctx.h);
        }
      }
    }
  }

  if (png) lgfx_pngle_destroy(png);
  if (rgba) heap_caps_free(rgba);
  f.close();
  SD.end();
  sdSPI.end();

  if (ok) {
    CARD_ART = cardPixels;
    Serial.printf("card: %s is in the tank\n", CARD_FILE);
  }
  return ok;
}

// A card that is there can still fail a read at power-up - the header comes
// back as garbage ("not a PNG") on some boots and the tank quietly runs without
// the drawing. So it is tried again, slower each time, before giving up.
bool cardLoad() {
  static const uint32_t FREQS[] = { SD_FREQ, 10000000, 4000000 };
  for (uint32_t f : FREQS) {
    if (cardLoadAt(f)) return true;
    delay(50);
  }
  return false;
}
