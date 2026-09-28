#ifndef PIXAQ_RIG_H
#define PIXAQ_RIG_H
// ---------------------------------------------------------------------------
// rig.h — species definitions and the sprite each one is drawn from.
//
// The browser build stacks five vertically-offset copies of the same art into
// a sheet and samples a shifted block to fake body roll. Working through that
// indexing, block `bias` sampled at row r always resolves to art row r - bias,
// so one sprite is enough here: the renderer offsets the source row and treats
// out-of-range rows as transparent.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "fish_art.h"
#include "gfx.h"

static const int BONES = 5;

enum SpKey : uint8_t { SP_NEON = 0, SP_GUPPY = 1, SP_BLACK = 2, SP_CORY = 3,
                       SP_EBI = 4, SP_SHRIMP = 5, SP_CARD = 6,
                       SP_HATCHET = 7, SP_TRANSLUCENT = 8, SP_LOACH = 9, SP_PUFFER = 10,
                       SP_MOLLY = 11, SP_PLATY = 12, SP_PANCHAX = 13, SP_NOTHO = 14 };

struct SpeciesCfg {
  SpKey       key;
  const char* label;
  int         count;
  float       lenCm;
  int         W, H;        // sprite size; W must be a multiple of BONES-1
  float       spacing;     // spine bone spacing (px) = W / (BONES-1)
  float       baseSpeed;   // px/s
  float       turnRate;    // rad/s
  float       beatHz;      // tail beats per second at cruise
  float       A[BONES];    // lateral amplitude per bone (px)
  float       finFrom;     // arc distance behind the head where the fin starts
  float       fanRipple;   // how much the fin surface ripples across its height
  float       roam;        // wander radius (px); 0 = the whole tank
  float       yLo, yHi;    // the layer of water this species lives in, in rows
  float       pitchMax;    // max pseudo-pitch during depth events
  uint8_t     strain;      // colour variant, for picking the art
  const RGBA8* art;
};

struct Rig {
  const SpeciesCfg* cfg;
  const RGBA8*      spr;   // cfg->W * cfg->H, straight-on art, head at +x
};

extern const SpeciesCfg NEON;
extern const SpeciesCfg GUPPY_STRAINS[5];
extern const SpeciesCfg BLACKTETRA;
extern const SpeciesCfg CORYDORAS;
extern const SpeciesCfg YAMATO;
extern const SpeciesCfg EBIFRY;
extern const SpeciesCfg HATCHET;
extern const SpeciesCfg TRANSLUCENT;
extern const SpeciesCfg CLOWNLOACH;
extern const SpeciesCfg PUFFER;
extern const SpeciesCfg SAILFIN;
extern const SpeciesCfg PLATY;
extern const SpeciesCfg PANCHAX;
extern const SpeciesCfg NOTHO;
// the eight that came second, in one list so stocking can walk it
static const int N_NEWSP = 8;
extern const SpeciesCfg* const NEW_SPECIES[N_NEWSP];
// Not const: how many there are and what they look like are both decided at
// boot, by what is on the card. See cardfish.h.
extern SpeciesCfg CARDFISH;
static const int N_RIGS = 19;         // five species, five guppy strains, gag,
                                      // whatever is on the SD card, and the
                                      // second eight

extern Rig RIGS[N_RIGS];

bool buildRigs();                     // wire the sprites up; call from setup()
const Rig* rigFor(const SpeciesCfg* cfg);

#endif // PIXAQ_RIG_H
