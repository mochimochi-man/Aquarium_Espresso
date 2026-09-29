#ifndef PIXAQ_SIM_H
#define PIXAQ_SIM_H
// ---------------------------------------------------------------------------
// sim.h — fish behaviour, spine chain, depth (toward/away) events.
// Direct port of the browser build; the 320x200 coordinate space is unchanged.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "rig.h"

namespace VIEW {
  // The browser reserved the lower 40% of its 200px canvas for the floor and
  // the depth rail; the tank photo replaces both, so every y from the web build
  // is remapped onto the panel through ymap(). Move SWIM_TOP / SWIM_BOT to
  // re-frame how much of the photo the fish roam over - nothing else needs to
  // change.
  static const float SWIM_TOP = 30.0f;
  static const float SWIM_BOT = 205.0f;
  static constexpr float ymap(float v) {
    return SWIM_TOP + (v - 26.0f) * ((SWIM_BOT - SWIM_TOP) / 94.0f);
  }

  static const float x0 = 14, x1 = 306;          // swim bounds x
  static const float y0 = ymap(26), y1 = ymap(120);
  static const float horizonY = ymap(18);
  static const float floorY   = ymap(134);
  static const int   W = 320, H = 240;
}

struct DepthEv {
  uint8_t mode;          // 0 idle, 1 event
  float t, dur;
  float from, to;
  float sign;            // -1 toward viewer, +1 away
  float cool;
  float bell;            // transient envelope 0..1
};

// distance-based motion history (one point per ~0.45px swum)
// The head's path, in three dimensions: z is towards the glass. The body
// follows it, so when the fish comes about - swimming round through the depth
// of the tank - the body comes round the same curve behind the head, and on
// the screen it foreshortens from the head back and flips over segment by
// segment, the tail last.
struct Trail {
  static const int CAP = 128;
  float x[CAP], y[CAP], z[CAP];
  int   n, head;         // head = newest
  void reset() { n = 0; head = CAP - 1; }
  void push(float px, float py, float pz = 0.0f) {
    head = (head + 1) % CAP;
    x[head] = px; y[head] = py; z[head] = pz;
    if (n < CAP) n++;
  }
  // back = 0 is the newest sample
  int idx(int back) const { return (head - back + CAP * 2) % CAP; }
};

struct Bone { float x, y, a; };

// Individual habits. The browser build gives every fish the same wandering
// rule, which reads as a screensaver; a tank reads as alive because a few
// individuals are always doing something of their own.
enum Personality : uint8_t {
  PERS_NONE = 0,   // just swims
  PERS_GULPER,     // rises to the surface now and then and gulps at it
  PERS_LOAFER,     // parks against a wall or on the bottom and sits there
  PERS_HOVERER,    // holds station in open water for long stretches
};

enum FishAct : uint8_t {
  ACT_SWIM = 0,
  ACT_RISE, ACT_GULP, ACT_SINK,     // gulper
  ACT_PARK, ACT_HOVER,              // loafer
  ACT_HOLD,                         // hoverer: parked in mid-water
  // corydoras: works the bottom, bolts up for air, settles back down
  ACT_GRAZE, ACT_REST, ACT_DASH, ACT_AIR, ACT_SETTLE,
  // amano shrimp: picks at one spot, scoots to the next, swims when it wants
  // to be elsewhere, and shoots backwards when something startles it
  ACT_PICK, ACT_CRAWL, ACT_SWIMOFF, ACT_FLICK,
  // hatchetfish: a short dip below the surface and back up
  ACT_DIVE, ACT_RETURN,
  // translucent glass catfish: holding its place in the shoal
  ACT_STATION,
  // clown loach: lying on its side, and running up and down the glass
  ACT_PLAYDEAD, ACT_DANCE,
};

struct Fish {
  const SpeciesCfg* cfg;
  const SpeciesCfg* home;   // what it really is, while cfg says otherwise
  float x, y;
  float z;               // depth of the head along its own path (see Trail)
  // Which way it is pointing, as a fish points: `yaw` turns it about its own
  // vertical axis (0 = broadside heading right, PI = heading left, +-PI/2 =
  // nose to or from the glass) and `pitch` tips its nose up or down. A fish
  // coming about turns towards or away from you; it does not wheel round in
  // the plane of the glass, which is what reads as rolling onto its side.
  float yaw, pitch;
  float turnSide;        // which way round the next reversal goes, +-1
  float yawV;            // how fast it is coming round, rad/s (eased)
  float fore;            // how much of its length shows: |cos(yaw)|, floored
  // The body's axis on the screen - derived from the two above, near level
  // and pointing the way it faces. Kept because everything that asks "which
  // way is it facing" reads it.
  float heading;
  float speed;
  float prevHeading;
  float turnRate;        // smoothed rad/s
  float tx, ty;          // steering target
  float retarget;
  Trail trail;
  float beat;
  float phase;
  float sf;              // depth scale factor (persists between events)
  DepthEv depth;
  float biasF;           // smoothed roll bias (rows)
  float burstX, burstY;
  float sepX, sepY;      // crowd separation, recomputed every frame
  float speedNorm;
  bool    school;
  uint8_t sid;           // which shoal, when school is true
  float   orbitR;
  uint8_t pers;          // Personality
  uint8_t act;           // FishAct
  float   actT;          // time left in the current act
  float   nextAct;       // countdown to the next one
  float   holdX, holdY;  // where this act is taking it
  float   effort;        // swim effort multiplier, smoothed
  float   lifted;        // seconds of slack left on the layer clamp after the
                         // air stone has carried it out of its own water
  float   thrash;        // 0..1 whole-body writhing, for the corydoras dash
  Bone  bones[BONES];    // per-frame render cache
  float facing;          // 1 = pure side view
  float mirror;          // +1 art as drawn (heading right), -1 flipped
  float turn;            // card fish only: rotation about its own vertical
                         // axis. 0 = facing right, PI = facing left, and the
                         // values in between are the card edge-on.
  float flare;           // fin billow, 0 cruising .. 1.5 mid-turn
  float dorsal;          // sailfin molly: 1 sail raised .. ~0.25 laid flat
  float flat;            // clown loach: 0 upright .. 1 lying on its side
  float tilt;            // nose-up lean, rad (the hatchetfish at the surface)
  bool  air;             // hatchetfish: out of the water, mid-jump
  bool  gone;            // ... and over the rim: removed at the end of the step
  float vxAir, vyAir;    // its flight
  int8_t reps;           // clown loach: laps of the glass left in a dance
  float jumpIn;          // hatchetfish: seconds until it leaps, <0 when not
  float ownDir;          // translucent glass catfish: the way it is facing (+1/-1)
  float turnIn;          // ... and how long before it comes round to the shoal's
  // Render-only, for the copies drawn at the surface (renderer.cpp); the fish
  // the simulation steps always has these at their defaults.
  float lenScale;        // body length and bone spacing, x
  float hScale;          // body height, x
  float alphaMul;        // opacity, x
  bool  vflip;           // drawn upside down (the surface's reflection)
  bool  bodyOnly;        // no fins, eye or veil
  bool  dry;             // out of the water: no water haze
};

struct Surge  { float t, dur, dir, mag; };
// What a hatchetfish throws up when it breaks the surface, and what it takes
// down with it when it drops back in: droplets flung into the air, a puff of
// bubbles under the film, and a ripple running out along it.
enum SplashKind : uint8_t { SPL_DROP = 0, SPL_BUBBLE = 1, SPL_RIPPLE = 2 };
struct Splash { float x, y, vx, vy, life, max, size; uint8_t kind; };
static const int MAX_SPLASH = 96;
// the water surface, in panel rows: the water line in the backdrops, where
// the air stone's bubbles break (bubbles.cpp)
static const float SURFACE_Y = 16.0f;
struct Mote   { float x, y, vx, vy, a, ph; };
struct Bubble { float x, y, r, vy, ph, a; };
struct School { float x, y, tx, ty, timer, dash; float home, homeY; };

// Three shoals instead of one. Twenty tetras orbiting a single centre pack
// into one blob however wide the orbit gets; splitting them into separate
// shoals with their own home stretch of the tank is what actually spreads
// them out.
static const int N_SCHOOLS = 3;
// The stocking. Every group in the tank has a place, and at boot each place
// is dealt one of the kinds that can fill it - all of that group, never a mix,
// and never changed afterwards, so the tank you switch on is the tank you
// watch:
//
//   10 neon tetras       always
//    5 of one of         neon tetras (joining the shoals), Amano shrimp,
//                        silver hatchet, green puffer, sailfin molly, platy,
//                        striped panchax, nothobranchius (equal odds)
//    5 of one of         guppies (twice the odds of each other kind), or one
//                        of the six fish above; and on about one boot in
//                        twenty, guppies that are ebi-fry
//    3 of one of         the same choice, dealt separately - but the fish
//                        from the SD card, when there is one, take these
//                        places first
//    3 of one of         black tetras, translucent glass catfish (even odds)
//    2 of one of         corydoras, clown loaches (even odds)
//
// No kind is dealt to more than one of these places.
//
// So the stocking is a runtime number (`Sim::n`) and this is only the size of
// the array.
// Test build: every boot stocks hatchetfish in both places they can go, and
// they jump every few seconds. 0 for normal use.
#define TEST_HATCHET_JUMP 0

// The hatchetfish leap is an escape, not a habit: something startles them - a
// vibration in the water, a big fish coming up underneath - and the ones near
// it go up together. This is how often, on average, something does, s.
#if TEST_HATCHET_JUMP
static const float SCARE_MEAN = 8.0f;
#else
static const float SCARE_MEAN = 1500.0f;
#endif

static const int N_NEON = 10;
static const int N_MATE = 5;           // the five that share the neons' boot
static const int N_FISH = N_NEON + N_MATE + 8 + 3 + 2;

// How many copies of the picture on the SD card join the tank, when there is
// one. More than one reads as a shoal of the same drawing, which is exactly
// what the aquariums this imitates looked like.
//
// They are not extra fish. Each one takes a place in place 3 (the guppies'
// three), so the tank stays the density it was designed at.
static const int CARD_MIN = 1, CARD_MAX = 3;
static const int N_GUPPY = 8;                  // 2+2+2+1+1 across the strains
static const int N_GUPPY_A = 5, N_GUPPY_B = 3; // the two places they are split into

// How quickly the card's face catches up with the direction it is travelling.
// This is only there to take the jitter out of the heading - it is not what
// paces the turn, and it must not be slow enough to become that.
static const float CARD_FACE_EASE = 5.0f;

// How flat the card lies when it is not going straight sideways. The face
// follows the heading, so a card swimming at 45 degrees would be down to 71%
// of its width on the raw cosine; this pulls it back out to 84%, and keeps the
// truly edge-on moment for the reversal itself where it belongs.
static const float CARD_FACE_FLAT = 0.5f;
static const int N_MOTES   = 22;
static const int MAX_BUB   = 10;
static const int MAX_SURGE = 6;

struct Sim {
  Fish   fish[N_FISH];
  int    n;              // how many of them this boot actually stocked
  School school[N_SCHOOLS];
  Surge  surges[MAX_SURGE];
  int    nSurge;
  Bubble bubbles[MAX_BUB];
  int    nBub;
  Mote   motes[N_MOTES];
  float  stress;         // 0..1 global excitement
  float  nextBubble;
  float  t;
  float  tw;             // t wrapped to a common period, for the trig calls
  bool   ebiDay;         // this boot's guppies are ebi-fry
  float  sway;           // horizontal water displacement, px
  float  swayV;
  // The translucent glass catfish shoal: where it is holding, where it is drifting to,
  // and which way every one of them is facing.
  float  tgcX, tgcY, tgcTx, tgcTy, tgcDir, tgcT;
  float  tgcHold;        // time left before the shoal's way changes
  float  scareIn;        // seconds to the next thing that sends the hatchets up
  Splash splash[MAX_SPLASH];
  int    nSplash;
};

// The translucent glass catfish do not turn together. The way the shoal faces
// holds for a while; then it changes, and each fish comes round on its own
// somewhere inside a grace period, so that by the end of it they all happen to
// be facing the same way again.
static const float TGC_HOLD_MIN  = 30.0f,  TGC_HOLD_MAX  = 180.0f;   // s
static const float TGC_GRACE_MIN = 30.0f,  TGC_GRACE_MAX = 60.0f;    // s

// Every multiplier applied to `t`/`beat` before sinf() is a multiple of 0.01,
// so wrapping at 200*2pi keeps each wave continuous while keeping the argument
// small - large arguments make newlib's sinf argument reduction dominate the
// per-pixel cost.
static const float TRIG_WRAP = 200.0f * 2.0f * (float)M_PI;

// Once in a while - about one power-up in twenty - the guppies' place of
// five comes up as guppies already fried, and stays that way for the whole
// session.
static const float GAG_CHANCE = 0.05f;

// --- choosing what goes in each place, over the serial line -----------------
// The five places, in order: 1 the neons' five, 2 the guppies' five, 3 the
// guppies' three, 4 the black tetras' three, 5 the corydoras' two. Each can be
// left to chance (RND) or set to a kind; a set place takes any kind at all,
// whatever it would normally be dealt. The Aquarium_Espresso.ino console
// reads and writes these and keeps them in NVS.
enum FishCode : uint8_t {
  FC_RND = 0, FC_GPY, FC_EBI, FC_NEO, FC_SHR, FC_HAT, FC_PUF, FC_MOL, FC_PLA,
  FC_PAN, FC_NOT, FC_BLK, FC_TGC, FC_COR, FC_LOA, FC_COUNT
};
static const int N_PLACES = 5;
extern uint8_t gForce[N_PLACES];
const char* fishCodeName(uint8_t code);
const char* fishCodeLabel(uint8_t code);
int fishCodeParse(const char* s);          // -1 if not a code

void makeSim(Sim& sim);
void stepSim(Sim& sim, float dt);
void tapWater(Sim& sim, float x, float y);
void startDepthEvent(Fish& f, bool stress);

// walk the motion trail backwards by `back` px (measured in 3D)
void trailAt(const Trail& tr, float back, float& ox, float& oy, float& oz);

#endif // PIXAQ_SIM_H
