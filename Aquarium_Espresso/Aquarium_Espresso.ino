// ===========================================================================
// Aquarium_Espresso — ESP32-S3 + ST7789 (320x240) port of the `pixel-aquarium`
// web app (g:/マイドライブ/Arduino/app).
//
// 33 fish in four species, each with its own habits and its own layer of the
// water column: guppies up top, black tetras hanging in the middle, three
// shoals of neon tetras below them, corydoras working the sand. Bodies are
// spine chains drawn as bent sprite strips.
//
// Everything the web build drew around the fish is replaced by a photo of a
// real tank: one of the five shots in app/ is picked at random each boot,
// decoded into PSRAM and lit every frame by light.cpp - LED inverter flicker,
// surface caustics and the refraction shimmer of water in motion.
//
// And if there is an SD card with a fish.png on it, one to three copies of
// whatever is drawn on it swim here too - see cardfish.h.
//
// Wiring / panel options: lgfx_setup.h
// Backdrop photos:        bg_images.cpp (regenerate with tools/make_bg.py)
// SD card / the drawing:  cardfish.cpp
// ===========================================================================
#include <esp_heap_caps.h>
#include "lgfx_setup.h"
#include "gfx.h"
#include "rig.h"
#include "sim.h"
#include "renderer.h"
#include "light.h"
#include "bubbles.h"
#include "cardfish.h"
#include "fastmath.h"
#include "bg_images.h"
#include <Preferences.h>

extern uint32_t tRestore[2], tVeil[2], tSeg[2], tExtra[2];

static LGFX tft;
static Sim  sim;

#define PERF_LOG 0     // 1: print frame timings every 5 s

// ---------------------------------------------------------------------------
// Screen recording, for tools/record.py: "REC <seconds>" switches the line to
// REC_BAUD and sends every frame as a JPEG with the time it took to make. The
// encode and the sending take far longer than a frame, so while recording the
// scene is stepped by those measured times rather than the wall clock - the
// video plays at the speed, and the frame rate, the screen really runs at.
// ---------------------------------------------------------------------------
#include "img_converters.h"
static const uint32_t REC_BAUD = 2000000;
static bool     recOn = false;
static int64_t  recLeftUs = 0;
static uint32_t recPeriod = 0;

static void recSendFrame(uint32_t periodUs) {
  // By now every band has been byte-swapped for the panel, which is the
  // camera's own big-endian RGB565 - what the encoder expects.
  uint8_t* jpg = nullptr;
  size_t len = 0;
  if (!fmt2jpg((uint8_t*)FB, (size_t)FB_W * FB_H * 2, FB_W, FB_H,
               PIXFORMAT_RGB565, 85, &jpg, &len)) len = 0;
  // with a checksum: at this rate the odd byte goes missing on the way, and a
  // JPEG with a hole in it still decodes - into garbage
  uint32_t sum = 0;
  for (size_t k = 0; k < len; k++) sum = sum * 31u + jpg[k];
  const uint32_t L = (uint32_t)len, P = periodUs;
  Serial.write((const uint8_t*)"FRM1", 4);
  Serial.write((const uint8_t*)&L, 4);
  Serial.write((const uint8_t*)&P, 4);
  Serial.write((const uint8_t*)&sum, 4);
  if (len) Serial.write(jpg, len);
#if DEBUG_ONE_MOLLY
  // debug: the first fish's state, so the host can follow it exactly
  {
    const Fish& d = sim.fish[0];
    float st[4 + 2 * BONES + 2];
    st[0] = d.x; st[1] = d.y; st[2] = d.yaw; st[3] = d.dorsal;
    for (int b = 0; b < BONES; b++) { st[4 + 2 * b] = d.bones[b].x; st[5 + 2 * b] = d.bones[b].y; }
    st[4 + 2 * BONES] = d.mirror; st[5 + 2 * BONES] = d.pitch;
    Serial.write((const uint8_t*)"DBG0", 4);
    Serial.write((const uint8_t*)st, sizeof(st));
  }
#endif
  if (jpg) free(jpg);
  recLeftUs -= periodUs;
  if (recLeftUs <= 0) {
    Serial.write((const uint8_t*)"END0", 4);
    Serial.flush();
    Serial.updateBaudRate(115200);
    recOn = false;
  }
}

// ---------------------------------------------------------------------------
// Serial console: choosing what goes in each of the five places (sim.h).
//
//   SET <1-5> <CODE>              one place, e.g. SET 1 GPY
//   SET ALL <C1> <C2> <C3> <C4> <C5>   all five, RND for "as usual"
//   SHOW                          what is set
//   RESET | CLEAR                 everything back to RND
//   HELP                          the codes
//   REBOOT                        restart, to see it
//
// A set place takes any kind at all. Settings are kept in NVS and take effect
// at the next boot - the tank is never restocked under the viewer.
// ---------------------------------------------------------------------------
static Preferences prefs;
static const char* PREF_NS = "aqesp";
static const char* PREF_KEY = "places";

static void loadPlaces() {
  if (!prefs.begin(PREF_NS, true)) return;       // nothing saved yet
  uint8_t b[N_PLACES];
  if (prefs.getBytes(PREF_KEY, b, N_PLACES) == N_PLACES)
    for (int p = 0; p < N_PLACES; p++) gForce[p] = (b[p] < FC_COUNT) ? b[p] : FC_RND;
  prefs.end();
}

static void savePlaces() {
  prefs.begin(PREF_NS, false);
  prefs.putBytes(PREF_KEY, gForce, N_PLACES);
  prefs.end();
}

static void showPlaces() {
  static const char* const PLACE[N_PLACES] = {
    "neon tetra 5", "guppy 5", "guppy 3", "black tetra 3", "corydoras 2" };
  for (int p = 0; p < N_PLACES; p++)
    Serial.printf("  %d (%s): %s  %s\n", p + 1, PLACE[p], fishCodeName(gForce[p]),
                  fishCodeLabel(gForce[p]));
}

// EBI works but is not listed: it is the secret
static void showHelp() {
  Serial.print(R"(Aquarium Espresso

Command:
 SET [SlotNum 1-5] [Species]

 e.g.
  SET 1 GPY

SET ALL [Slot1-Species] [Slot2-Species] [Slot3-Species] [Slot3-Species] [Slot4-Species] [Slot5-Species]

 e.g.
  SET ALL NEO GPY GPY BLK COR

Species:
 GPY:Guppy
 HAT:Silver Hatchet
 PUF:Green Puffer
 MOL:Sailfin Molly
 PLA:Platy
 PAN:Striped Panchax
 NOT:Nothobranchius
 SHR:Yamato Shrimp
 BLK:Black Tetra
 TGC:Translucent Glass Catfish
 COR:Corydoras
 LOA:Clown Loach

Default Species:
 Slot1:5fish NEO/HAT/PUF/MOL/PLA/PAN/NOT/SHR
 Slot2:5fish GPY/HAT/PUF/MOL/PLA/PAN/NOT
 Slot3:3fish GPY/HAT/PUF/MOL/PLA/PAN/NOT/(SD Card:fish.png)
 Slot4:3fish BLK/TGC
 Slot5:2fish COR/LOA
 FixedSlot:10fish NEO
)");
}

static void runCommand(char* line) {
  char* tok[8];
  int n = 0;
  for (char* t = strtok(line, " \t\r"); t && n < 8; t = strtok(nullptr, " \t\r")) tok[n++] = t;
  if (n == 0) return;
  if (!strcasecmp(tok[0], "SET")) {
    if (n >= 2 && !strcasecmp(tok[1], "ALL")) {
      if (n != 2 + N_PLACES) {
        Serial.printf("ERROR: SET ALL needs %d codes, got %d\n", N_PLACES, n - 2);
        return;
      }
      uint8_t v[N_PLACES];
      for (int p = 0; p < N_PLACES; p++) {
        int c = fishCodeParse(tok[2 + p]);
        if (c < 0) { Serial.printf("ERROR: unknown code '%s' (HELP)\n", tok[2 + p]); return; }
        v[p] = (uint8_t)c;
      }
      memcpy(gForce, v, N_PLACES);
    } else {
      if (n != 3) { Serial.println("ERROR: SET <1-5> <CODE>"); return; }
      int p = atoi(tok[1]);
      if (p < 1 || p > N_PLACES) { Serial.println("ERROR: place must be 1-5"); return; }
      int c = fishCodeParse(tok[2]);
      if (c < 0) { Serial.printf("ERROR: unknown code '%s' (HELP)\n", tok[2]); return; }
      gForce[p - 1] = (uint8_t)c;
    }
    savePlaces();
    Serial.println("SET OK, REBOOT NOW...");
    Serial.flush();
    delay(200);
    ESP.restart();
  } else if (!strcasecmp(tok[0], "RESET") || !strcasecmp(tok[0], "CLEAR")) {
    for (int p = 0; p < N_PLACES; p++) gForce[p] = FC_RND;
    prefs.begin(PREF_NS, false);
    prefs.clear();
    prefs.end();
    Serial.println("RESET OK, REBOOT NOW...");
    Serial.flush();
    delay(200);
    ESP.restart();
  } else if (!strcasecmp(tok[0], "SHOW")) {
    showPlaces();
  } else if (!strcasecmp(tok[0], "HELP")) {
    showHelp();
  } else if (!strcasecmp(tok[0], "REC")) {           // not in HELP: a dev tool
    int sec = (n >= 2) ? atoi(tok[1]) : 30;
    if (sec < 1) sec = 30;
    Serial.println("REC OK");
    Serial.flush();
    delay(50);
    Serial.updateBaudRate(REC_BAUD);
    recLeftUs = (int64_t)sec * 1000000;
    recPeriod = 0;
    recOn = true;
  } else if (!strcasecmp(tok[0], "REBOOT")) {
    Serial.println("rebooting");
    Serial.flush();
    ESP.restart();
  } else {
    Serial.printf("ERROR: unknown command '%s' (HELP)\n", tok[0]);
  }
}

static void pollConsole() {
  static char buf[96];
  static int len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (len) { buf[len] = 0; runCommand(buf); len = 0; }
    } else if (len < (int)sizeof(buf) - 1) {
      buf[len++] = ch;
    }
  }
}

// On an ebi-fry boot the tank says so, once, as it comes up: the screen goes
// black with a title card for a few seconds, then the tank fades in. The text
// is drawn straight into the frame buffer through a sprite that wraps it, after
// each band has been byte-swapped for the panel - the sprite keeps its pixels
// in that same order.
static LGFX_Sprite secretText;
static float secretT = -1.0f;                  // seconds shown so far; <0: none
static const float SECRET_HOLD = 2.0f;         // the card
static const float SECRET_FADE = 0.5f;         // then the tank comes up out of black

// before the band is swapped: the fade back in from black
static void fadeSecret(int y0, int y1) {
  if (secretT < SECRET_HOLD) return;
  const int keep = (int)(32.0f * (secretT - SECRET_HOLD) / SECRET_FADE);
  if (keep >= 32) return;
  uint16_t* p = FB + (size_t)y0 * FB_W;
  for (size_t i = 0, n = (size_t)(y1 - y0) * FB_W; i < n; i++) p[i] = blend565(0, p[i], keep);
}

// after it is swapped: the black card and its two lines
static void drawSecret(int y0, int y1) {
  if (secretT >= SECRET_HOLD) return;
  secretText.setClipRect(0, y0, FB_W, y1 - y0);
  secretText.fillRect(0, y0, FB_W, y1 - y0, TFT_BLACK);
  secretText.setTextDatum(middle_center);
  // anti-aliased fonts, at their own sizes - a scaled-up bitmap font is all
  // stair-steps
  secretText.setTextColor(TFT_WHITE, TFT_BLACK);
  secretText.setTextSize(1);
  secretText.setFont(&fonts::DejaVu24);
  secretText.drawString("Secrets Appeared", FB_W / 2, FB_H / 2 - 36);
  secretText.setFont(&fonts::DejaVu56);
  const int w = secretText.textWidth("EBI-FRY");
  if (w > FB_W - 16) secretText.setTextSize((float)(FB_W - 16) / w);
  secretText.drawString("EBI-FRY", FB_W / 2, FB_H / 2 + 20);
  secretText.setTextSize(1);
  secretText.clearClipRect();
}

// There is no tap input on this build, so the tank startles itself now and
// then to keep the dart / turn / flare behaviour visible.
static float autoTap = 4.0f;

// Bands the frame is composed and DMA'd in. The SPI peripheral only starts a
// transfer asynchronously when it fits one hardware descriptor run (32KB); a
// larger one falls back to a blocking chunk loop. 320x48x2 = 30720 B stays
// under that, so five bands is the coarsest split that actually overlaps.
static const int BANDS = 5;

// ---------------------------------------------------------------------------
// Both cores draw.
//
// The frame is panel-bound: 320x240 at 16 bits and 20MHz is 61.4ms on the wire,
// and nothing can make a frame shorter than that. What the CPU can do is stay
// out of the way - every microsecond it spends drawing while the bus is idle is
// a microsecond added on top of those 61.4.
//
// Two things were costing exactly that. The first band of a frame had no
// transfer to hide behind, and all the drawing sat on core 1 while core 0 did
// nothing whatsoever. So: band 0 is now drawn during the *previous* frame's
// last transfer, and every band is split between the two cores - core 0 takes
// the top slice, core 1 the bottom, and core 1 joins before handing the band to
// the DMA. Between them that is enough to fit all the drawing inside the wire
// time, which puts the frame rate on the panel's own ceiling.
//
// Everything a core touches while drawing has to be its own: the band clip
// (gfx.h), the polygon coverage row (gfx.cpp), the broad-wave row (light.cpp)
// and the timing counters (renderer.cpp) are all per core for this reason.
static const float SPLIT = 0.50f;        // share of a band core 0 takes

static TaskHandle_t hWorker = nullptr;
static TaskHandle_t hMain   = nullptr;
static const Sim*   wSim    = nullptr;
static volatile int wY0 = 0, wY1 = 0;

static void renderWorker(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    renderBand(*wSim, wY0, wY1);
    xTaskNotifyGive(hMain);
  }
}

// Draw one band on both cores and return when it is whole.
static void renderBandMT(const Sim& s, int y0, int y1) {
  const int ym = y0 + (int)((y1 - y0) * SPLIT + 0.5f);
  wSim = &s;
  wY0 = y0;
  wY1 = ym;
  xTaskNotifyGive(hWorker);
  renderBand(s, ym, y1);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static bool loadBackdrop() {
  BGBUF = (uint16_t*)heap_caps_malloc((size_t)FB_W * FB_H * sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM);
  if (!BGBUF) {
    Serial.println("FATAL: no PSRAM for the backdrop");
    return false;
  }
  LGFX_Sprite spr(&tft);
  spr.setPsram(true);
  spr.setColorDepth(16);
  if (!spr.createSprite(FB_W, FB_H)) {
    Serial.println("FATAL: backdrop sprite alloc failed");
    return false;
  }
  const int pick = (int)(esp_random() % N_BG_IMAGES);
  spr.fillScreen(0);
  bool ok = spr.drawJpg(BG_IMAGES[pick].data, BG_IMAGES[pick].len, 0, 0);
  // read back through an explicit rgb565 type so the byte order matches FB
  spr.readRect(0, 0, FB_W, FB_H, (lgfx::rgb565_t*)BGBUF);
  spr.deleteSprite();
  Serial.printf("backdrop %d/%d (%u B) decode %s\n",
                pick + 1, N_BG_IMAGES, (unsigned)BG_IMAGES[pick].len,
                ok ? "ok" : "FAILED");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\npixel-aquarium / ESP32-S3 + ST7789");

  fastMathInit();

  tft.init();
  tft.setRotation(1);              // 240x320 panel -> 320x240 landscape
  tft.setColorDepth(16);
  tft.fillScreen(0);
#if PIN_TFT_BLK >= 0
  pinMode(PIN_TFT_BLK, OUTPUT);
  digitalWrite(PIN_TFT_BLK, HIGH);
#endif

  const size_t fbBytes = (size_t)FB_W * FB_H * sizeof(uint16_t);
  FB = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!FB) {
    FB = (uint16_t*)heap_caps_malloc(fbBytes, MALLOC_CAP_SPIRAM);
    Serial.println("framebuffer: PSRAM fallback");
  }
  if (!FB) {
    Serial.println("FATAL: no framebuffer");
    while (true) delay(1000);
  }
  Serial.printf("framebuffer %ux%u (%u B), free internal %u\n",
                FB_W, FB_H, (unsigned)fbBytes,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (!loadBackdrop()) while (true) delay(1000);
  lightInit();
  lightPrepBackdrop();
  bubblesInit();

  // Before buildRigs(), which is where the sprite pointers are wired up. No
  // card, no card file, or an unreadable one, and the tank simply comes up
  // without them - this is a thing the tank can have, not a thing it needs.
  if (cardLoad()) {
    CARDFISH.count = CARD_MIN + (int)(esp_random() % (CARD_MAX - CARD_MIN + 1));
  }

  uint32_t t0 = millis();
  if (!buildRigs()) {
    Serial.println("FATAL: sprite bake failed");
    while (true) delay(1000);
  }
  Serial.printf("sprites baked in %lu ms\n", (unsigned long)(millis() - t0));

  loadPlaces();
  makeSim(sim);
  if (sim.ebiDay) {
    secretText.setColorDepth(16);
    secretText.setBuffer(FB, FB_W, FB_H, 16);
    secretT = 0.0f;
  }

  hMain = xTaskGetCurrentTaskHandle();
  // core 0 is otherwise idle on this build - no radio, no file system - so the
  // worker owns it outright
  xTaskCreatePinnedToCore(renderWorker, "render0", 8192, nullptr, 2,
                          &hWorker, 0);
  Serial.printf("drawing on core %d + core 0\n", xPortGetCoreID());
}

void loop() {
  static uint32_t last = micros();
  static uint32_t fpsT = millis();
  static int frames = 0;

  uint32_t now = micros();
  float dt = (now - last) / 1000000.0f;
  last = now;
  // while recording, the scene runs on the time frames really take on screen
  if (recOn && recPeriod) dt = recPeriod / 1000000.0f;
  if (dt > 1.0f / 30) dt = 1.0f / 30;

  autoTap -= dt;
  if (autoTap <= 0) {
    autoTap = 12.0f + (float)esp_random() / 4294967296.0f * 20.0f;
    float tx = 20 + (float)esp_random() / 4294967296.0f * 280.0f;
    float ty = VIEW::y0 + (float)esp_random() / 4294967296.0f * (VIEW::y1 - VIEW::y0);
    tapWater(sim, tx, ty);
  }

  static uint32_t accSim = 0, accDraw = 0, accPush = 0, accEnd = 0;

  static bool writeOpen = false;

  uint32_t m0 = micros();
  pollConsole();
  if (secretT >= 0.0f) {
    secretT += dt;
    if (secretT > SECRET_HOLD + SECRET_FADE) secretT = -1.0f;
  }
  bubblesStep(dt);
  lightStep(dt);
  stepSim(sim, dt);

  // The last band of the previous frame is on the wire through all of this, and
  // there is enough of it left over to draw the first band of the new frame as
  // well - the one band that would otherwise have no transfer to hide behind.
  renderBandMT(sim, 0, SCR_H / BANDS);
  uint32_t m1 = micros();

  // Only now wait for that last band, which by this point has little left to
  // run.
  if (writeOpen) { tft.endWrite(); writeOpen = false; }
  uint32_t m1b = micros();

  // Send band by band: each pushImageDMA() returns while the panel is still
  // being fed, so the next band is drawn during the transfer.
  tft.startWrite();
  writeOpen = true;
  for (int b = 0; b < BANDS; b++) {
    const int y0 = b * SCR_H / BANDS;
    const int y1 = (b + 1) * SCR_H / BANDS;
    uint32_t p0 = micros();
    if (secretT >= 0.0f) fadeSecret(y0, y1);
    fbSwapBand(y0, y1);
    if (secretT >= 0.0f) drawSecret(y0, y1);
    tft.pushImageDMA(0, y0, FB_W, y1 - y0,
                     (lgfx::swap565_t*)(FB + (size_t)y0 * FB_W));
    accPush += micros() - p0;
    if (b + 1 < BANDS)
      renderBandMT(sim, y1, (b + 2) * SCR_H / BANDS);
  }
  uint32_t m2 = micros();
  if (recOn) {
    recPeriod = m2 - now;           // what the frame took, not the sending
    recSendFrame(recPeriod);
  }
  accSim += m1 - m0;
  accDraw += m2 - m1b;
  accEnd += m1b - m1;

  frames++;
  // The 5-second timing line. Off in normal use: it only gets in the way of
  // the console.
  if (PERF_LOG && millis() - fpsT >= 5000) {
    uint32_t el = millis() - fpsT;
    Serial.printf("%.1f fps sim+b0 %.2f draw %.2f (cpu bg %.2f veil %.2f seg %.2f fin %.2f) dma %.2f end %.2f\n",
                  frames * 1000.0f / el,
                  accSim / 1000.0f / frames,
                  accDraw / 1000.0f / frames,
                  (tRestore[0] + tRestore[1]) / 1000.0f / frames,
                  (tVeil[0] + tVeil[1]) / 1000.0f / frames,
                  (tSeg[0] + tSeg[1]) / 1000.0f / frames,
                  (tExtra[0] + tExtra[1]) / 1000.0f / frames,
                  accPush / 1000.0f / frames,
                  accEnd / 1000.0f / frames);
    tRestore[0] = tRestore[1] = tVeil[0] = tVeil[1] = 0;
    tSeg[0] = tSeg[1] = tExtra[0] = tExtra[1] = 0;
    fpsT = millis();
    frames = 0;
    accSim = accDraw = accPush = accEnd = 0;
  }
}
