#ifndef PIXAQ_BG_IMAGES_H
#define PIXAQ_BG_IMAGES_H
// ---------------------------------------------------------------------------
// bg_images.h — the tank photos from app/ and preview/, baked by
// tools/make_bg.py.
// One is picked at random each boot and decoded into PSRAM as the backdrop.
// ---------------------------------------------------------------------------
#include <Arduino.h>

struct BgImage { const uint8_t* data; uint32_t len; };

static const int N_BG_IMAGES = 7;
extern const BgImage BG_IMAGES[N_BG_IMAGES];

#endif // PIXAQ_BG_IMAGES_H
