#pragma GCC optimize("O3")
#include "sim.h"
#include "fastmath.h"
#include "bubbles.h"
#include "cardfish.h"

static inline float rnd(float a, float b) {
  return a + (float)esp_random() / 4294967296.0f * (b - a);
}
static inline float rnd01() { return (float)esp_random() / 4294967296.0f; }
static inline float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}
static inline float wrapAng(float a) { return fwrap(a); }

static void makeFish(Fish& f, const SpeciesCfg* cfg, bool school, int sid,
                     float sx, float sy) {
  f.cfg = cfg;
  f.home = cfg;
  // spawn on the shoal it belongs to, or in the water this species uses, so
  // the tank never opens with every fish in one corner
  f.x = school ? sx + rnd(-30, 30) : rnd(22, 298);
  f.y = school ? sy + rnd(-16, 16) : rnd(cfg->yLo, cfg->yHi);
  f.x = clampf(f.x, VIEW::x0 + 4, VIEW::x1 - 4);
  f.y = clampf(f.y, cfg->yLo, cfg->yHi);
  f.heading = rnd(0, (float)M_PI * 2);
  f.trail.reset();
  // pre-fill a straight trail behind the head (0.5px spacing)
  for (int k = Trail::CAP - 1; k >= 0; k--) {
    f.trail.push(f.x - fcos(f.heading) * k * 0.5f,
                 f.y - fsin(f.heading) * k * 0.5f);
  }
  for (int k = 0; k < BONES; k++) {
    f.bones[k].x = f.x - fcos(f.heading) * k * cfg->spacing;
    f.bones[k].y = f.y - fsin(f.heading) * k * cfg->spacing;
    f.bones[k].a = f.heading;
  }
  // side-on, level: whichever way the random heading was nearer to
  f.yaw = (fcos(f.heading) >= 0) ? 0.0f : (float)M_PI;
  f.pitch = 0;
  f.turnSide = rnd01() < 0.5f ? -1.0f : 1.0f;
  f.yawV = 0;
  f.z = 0;
  f.lenScale = f.hScale = f.alphaMul = 1.0f;
  f.vflip = f.bodyOnly = f.dry = false;
  f.fore = 1.0f;
  f.heading = (f.yaw == 0) ? 0.0f : (float)M_PI;
  f.trail.reset();
  for (int k = Trail::CAP - 1; k >= 0; k--)
    f.trail.push(f.x - fcos(f.heading) * k * 0.5f, f.y);
  for (int k = 0; k < BONES; k++) {
    f.bones[k].x = f.x - fcos(f.heading) * k * cfg->spacing;
    f.bones[k].y = f.y;
    f.bones[k].a = f.heading;
  }
  f.speed = cfg->baseSpeed;
  f.prevHeading = f.heading;
  f.turnRate = 0;
  f.tx = rnd(30, 290);
  f.ty = rnd(VIEW::ymap(30), VIEW::ymap(110));
  f.retarget = rnd(2, 6);
  f.beat = rnd(0, 10);
  f.phase = rnd(0, (float)M_PI * 2);
  f.sf = rnd(0.88f, 1.12f);
  f.depth = { 0, 0, 1, 1, 1, 1, rnd(1, 5), 0 };
  f.biasF = 0; f.burstX = 0; f.burstY = 0; f.lifted = 0;
  f.sepX = 0; f.sepY = 0;
  f.speedNorm = 0.5f;
  f.school = school;
  f.sid = (uint8_t)(sid < 0 ? 0 : sid);
  f.orbitR = rnd(10, 34);
  f.facing = 1;
  f.mirror = (fcos(f.heading) < 0) ? -1.0f : 1.0f;
  f.turn = (f.mirror < 0) ? (float)M_PI : 0.0f;   // card only; see stepFish
  f.flare = 0;
  f.dorsal = 1.0f;
  f.flat = 0;
  f.tilt = 0;
  f.air = false;
  f.gone = false;
  f.vxAir = f.vyAir = 0;
  f.reps = 0;
  f.jumpIn = -1.0f;
  f.ownDir = (fcos(f.heading) >= 0) ? 1.0f : -1.0f;
  f.turnIn = 0;

  // guppies are the ones you actually catch loafing or picking at the surface;
  // tetras mostly stay with the shoal, but not always
  float r = rnd01();
  if (cfg->key == SP_BLACK)      f.pers = PERS_HOVERER;
  else if (cfg->key == SP_CORY)  f.pers = PERS_NONE;
  else if (cfg->key == SP_SHRIMP) f.pers = PERS_NONE;
  else if (cfg->key == SP_GUPPY || cfg->key == SP_MOLLY || cfg->key == SP_NOTHO)
                                 f.pers = r < 0.32f ? PERS_LOAFER
                                        : r < 0.62f ? PERS_GULPER : PERS_NONE;
  // a platy is forever picking at the glass and the plants
  else if (cfg->key == SP_PLATY) f.pers = r < 0.55f ? PERS_LOAFER
                                        : r < 0.75f ? PERS_GULPER : PERS_NONE;
  // the puffer "blimps" about, hanging in one place on its pectorals, and the
  // panchax hangs under the surface waiting for something to land on it
  else if (cfg->key == SP_PUFFER)  f.pers = r < 0.60f ? PERS_HOVERER : PERS_LOAFER;
  else if (cfg->key == SP_PANCHAX) f.pers = r < 0.70f ? PERS_HOVERER : PERS_NONE;
  // these three run on their own acts, not on a mood
  else if (cfg->key == SP_HATCHET || cfg->key == SP_TRANSLUCENT || cfg->key == SP_LOACH)
                                 f.pers = PERS_NONE;
  else                           f.pers = r < 0.08f ? PERS_LOAFER
                                        : r < 0.20f ? PERS_GULPER : PERS_NONE;
  f.act = (cfg->key == SP_CORY || cfg->key == SP_LOACH) ? ACT_GRAZE
        : (cfg->key == SP_SHRIMP) ? ACT_PICK
        : (cfg->key == SP_TRANSLUCENT)  ? ACT_STATION : ACT_SWIM;
  f.actT = rnd(1, 4);
  // Staggered, so they never all break at once. For the corydoras this is the
  // countdown to its next trip to the surface: measured air-breathing rates
  // run anywhere from 1 to 45 times an hour.
  // For the corydoras this is the countdown to its next trip to the surface;
  // for the shrimp, to its next proper swim rather than another scoot.
  f.nextAct = (cfg->key == SP_CORY)   ? rnd(10, 90)
            : (cfg->key == SP_SHRIMP) ? rnd(8, 45) : rnd(5, 30);
  f.holdX = f.x; f.holdY = f.y;
  if (cfg->key == SP_TRANSLUCENT) {
    // its own place in the shoal, as an offset from the shoal's centre
    f.holdX = rnd(-34, 34);
    f.holdY = rnd(-15, 15);
  }
  f.effort = 1.0f;
  f.thrash = 0;
}

// --- the codes the serial console uses --------------------------------------
uint8_t gForce[N_PLACES] = { FC_RND, FC_RND, FC_RND, FC_RND, FC_RND };

static const char* const CODE_NAME[FC_COUNT] = {
  "RND", "GPY", "EBI", "NEO", "SHR", "HAT", "PUF", "MOL", "PLA",
  "PAN", "NOT", "BLK", "TGC", "COR", "LOA",
};
static const char* const CODE_LABEL[FC_COUNT] = {
  "random", "guppy", "ebi-fry", "neon tetra", "amano shrimp", "silver hatchet",
  "green puffer", "sailfin molly", "platy", "striped panchax", "nothobranchius",
  "black tetra", "translucent glass catfish", "corydoras", "clown loach",
};
const char* fishCodeName(uint8_t c)  { return c < FC_COUNT ? CODE_NAME[c] : "?"; }
const char* fishCodeLabel(uint8_t c) { return c < FC_COUNT ? CODE_LABEL[c] : "?"; }
int fishCodeParse(const char* s) {
  for (int c = 0; c < FC_COUNT; c++)
    if (strcasecmp(s, CODE_NAME[c]) == 0) return c;
  return -1;
}

// the kind a code stands for; guppies and ebi-fry are drawn from the strains
static const SpeciesCfg* cfgForCode(uint8_t c) {
  switch (c) {
    case FC_NEO: return &NEON;        case FC_SHR: return &YAMATO;
    case FC_HAT: return &HATCHET;     case FC_PUF: return &PUFFER;
    case FC_MOL: return &SAILFIN;     case FC_PLA: return &PLATY;
    case FC_PAN: return &PANCHAX;     case FC_NOT: return &NOTHO;
    case FC_BLK: return &BLACKTETRA;  case FC_TGC: return &TRANSLUCENT;
    case FC_COR: return &CORYDORAS;   case FC_LOA: return &CLOWNLOACH;
    default:     return nullptr;
  }
}

void makeSim(Sim& sim) {
  // --- the stocking, dealt once (see sim.h) ---
  // A place set over the serial line keeps what it was set to; the rest are
  // dealt as always, and never the same kind as another place (fried guppies
  // excepted - those are not guppies any more). When places could share, the
  // same fish came up in two or three of them far too often, and with the big
  // ones - a sailfin molly is 32px - a boot could be a dozen of one kind.
  auto pick = [](int n) { int k = (int)rnd(0, n); return k >= n ? n - 1 : k; };
  uint8_t pc[N_PLACES];
  for (int p = 0; p < N_PLACES; p++) pc[p] = gForce[p];
  auto used = [&](uint8_t c) {
    for (int p = 0; p < N_PLACES; p++) if (pc[p] == c) return true;
    return false;
  };
  // the neons' five: more neons, the shrimp, or one of the six, all alike
  static const uint8_t MATE8[8] = { FC_NEO, FC_SHR, FC_HAT, FC_PUF, FC_MOL, FC_PLA, FC_PAN, FC_NOT };
  // the guppies' places: guppies at twice the odds, or one of the six
  static const uint8_t G8[8] = { FC_GPY, FC_GPY, FC_HAT, FC_PUF, FC_MOL, FC_PLA, FC_PAN, FC_NOT };
  if (pc[0] == FC_RND) { uint8_t c; do c = MATE8[pick(8)]; while (used(c)); pc[0] = c; }
  if (pc[1] == FC_RND) {
    if (rnd01() < GAG_CHANCE && !used(FC_EBI)) pc[1] = FC_EBI;
    else { uint8_t c; do c = G8[pick(8)]; while (used(c)); pc[1] = c; }
  }
  if (pc[2] == FC_RND) { uint8_t c; do c = G8[pick(8)]; while (used(c)); pc[2] = c; }
  if (pc[3] == FC_RND) pc[3] = (rnd01() < 0.5f) ? FC_BLK : FC_TGC;
  if (pc[4] == FC_RND) pc[4] = (rnd01() < 0.5f) ? FC_COR : FC_LOA;
#if TEST_HATCHET_JUMP
  pc[0] = FC_HAT; pc[2] = FC_HAT;
#endif
  sim.ebiDay = used(FC_EBI);

  sim.tgcX = sim.tgcTx = rnd(80, 240);
  sim.tgcY = sim.tgcTy = rnd(TRANSLUCENT.yLo + 14, TRANSLUCENT.yHi - 14);
  sim.tgcDir = rnd01() < 0.5f ? -1.0f : 1.0f;
  sim.tgcT = rnd(10, 30);
  sim.tgcHold = rnd(TGC_HOLD_MIN, TGC_HOLD_MAX);

  // Where the three shoals set up.
  //
  // These used to be constants - 70 / 160 / 250 across, and 20% / 50% / 80%
  // down - which meant every single boot opened with the same three shoals in
  // the same three places, in the same order, at the same depths. A tank that
  // looks identical every time you switch it on is the one thing this whole
  // build is trying not to be.
  //
  // So: the three stretches of the tank are handed out in a random order, each
  // shoal is jittered inside the stretch it drew, and its depth is drawn
  // separately from its position. Shuffling the stretches rather than just
  // jittering fixed ones matters - jitter alone still leaves the shallow shoal
  // on the left every time.
  int slot[N_SCHOOLS] = { 0, 1, 2 };
  for (int k = N_SCHOOLS - 1; k > 0; k--) {
    int j = (int)rnd(0, k + 1);
    if (j > k) j = k;                                // rnd() is inclusive at b
    int t = slot[k]; slot[k] = slot[j]; slot[j] = t;
  }
  for (int k = 0; k < N_SCHOOLS; k++) {
    float home = 62.0f + slot[k] * 98.0f + rnd(-24, 24);
    float y = NEON.yLo + (NEON.yHi - NEON.yLo) * rnd(0.12f, 0.86f);
    sim.school[k] = { home, y, home, y, rnd(2, 6), 0, home, y };
  }
  int i = 0;
  for (int k = 0; k < N_NEON; k++) {
    // Split into three shoals because one big shoal packs into a single blob.
    int sid = k % N_SCHOOLS;
    makeFish(sim.fish[i++], &NEON, true, sid,
             sim.school[sid].x, sim.school[sid].y);
  }
  // The guppy strains, shuffled, so which strains end up where - and which
  // ones the card pushes out - is not always the same.
  uint8_t gs[N_GUPPY];
  int ng = 0, gnext = 0;
  for (int g = 0; g < 5; g++)
    for (int k = 0; k < GUPPY_STRAINS[g].count && ng < N_GUPPY; k++)
      gs[ng++] = (uint8_t)g;
  for (int k = ng - 1; k > 0; k--) {
    int j = pick(k + 1);
    uint8_t t = gs[k]; gs[k] = gs[j]; gs[j] = t;
  }

  static const int PLACE_N[N_PLACES] = { N_MATE, N_GUPPY_A, N_GUPPY_B, 3, 2 };
  // the card's fish take the guppies' three first
  const int nCard = CARDFISH.count < N_GUPPY_B ? CARDFISH.count : N_GUPPY_B;
  for (int p = 0; p < N_PLACES; p++) {
    const uint8_t c = pc[p];
    int k = 0;
    if (p == 2)
      for (; k < nCard; k++) makeFish(sim.fish[i++], &CARDFISH, false, -1, 0, 0);
    for (; k < PLACE_N[p]; k++) {
      const SpeciesCfg* cfg = cfgForCode(c);
      if (!cfg) cfg = &GUPPY_STRAINS[gs[gnext++ % ng]];     // guppies / ebi-fry
      // neons join the shoals, wherever they were dealt
      if (c == FC_NEO) {
        const int sid = (N_NEON + k) % N_SCHOOLS;
        makeFish(sim.fish[i++], cfg, true, sid, sim.school[sid].x, sim.school[sid].y);
      } else {
        makeFish(sim.fish[i++], cfg, false, -1, 0, 0);
      }
      Fish& nf = sim.fish[i - 1];
      if (c == FC_EBI) nf.cfg = &EBIFRY;
      if (c == FC_TGC) {                     // start in the shoal, facing its way
        nf.x = clampf(sim.tgcX + nf.holdX, VIEW::x0 + 4, VIEW::x1 - 4);
        nf.y = clampf(sim.tgcY + nf.holdY, cfg->yLo, cfg->yHi);
        nf.heading = sim.tgcDir > 0 ? 0.0f : (float)M_PI;
        nf.yaw = nf.heading;
        nf.prevHeading = nf.heading;
        for (int q = 0; q < BONES; q++) {
          nf.bones[q].a = nf.heading;
          nf.bones[q].x = nf.x - fcos(nf.heading) * q * cfg->spacing;
          nf.bones[q].y = nf.y;
        }
        nf.mirror = sim.tgcDir;
        nf.ownDir = sim.tgcDir;
        nf.trail.reset();
        for (int q = Trail::CAP - 1; q >= 0; q--)
          nf.trail.push(nf.x - fcos(nf.heading) * q * 0.5f, nf.y);
      }
    }
  }
  sim.n = i;
  Serial.printf("%d fish: %d neons | 1: %d %s%s | 2: %d %s%s | 3: %d card + %d %s%s"
                " | 4: %d %s%s | 5: %d %s%s\n",
                sim.n, N_NEON,
                PLACE_N[0], CODE_NAME[pc[0]], gForce[0] ? "*" : "",
                PLACE_N[1], CODE_NAME[pc[1]], gForce[1] ? "*" : "",
                nCard, PLACE_N[2] - nCard, CODE_NAME[pc[2]], gForce[2] ? "*" : "",
                PLACE_N[3], CODE_NAME[pc[3]], gForce[3] ? "*" : "",
                PLACE_N[4], CODE_NAME[pc[4]], gForce[4] ? "*" : "");

  for (int m = 0; m < N_MOTES; m++) {
    sim.motes[m] = { rnd(10, 310), rnd(VIEW::ymap(22), VIEW::ymap(128)),
                     rnd(-1.2f, 1.2f), rnd(-0.4f, 0.9f),
                     rnd(0.05f, 0.22f), rnd(0, 6) };
  }
  sim.nSurge = 0;
  sim.nSplash = 0;
  sim.scareIn = rnd(0.4f, 1.6f) * SCARE_MEAN;
  sim.nBub = 0;
  sim.stress = 0;
  sim.nextBubble = 1.5f;
  sim.t = 0;
  sim.tw = 0;
  sim.sway = 0;
  sim.swayV = 0;
}

// --- depth event: fish swims toward / away from the viewer -----------------
void startDepthEvent(Fish& f, bool stress) {
  DepthEv& d = f.depth;
  if (d.mode == 1) return;
  // depth lunges happen only on a real startle; ambient swimming stays
  // strictly side-on
  if (!stress) { d.cool = 5; return; }
  const float maxD = 0.3f;
  float mag = (0.4f + rnd01() * 0.6f) * maxD;
  float sign = rnd01() < 0.5f ? -1.0f : 1.0f;
  // mid-turn the fish tends to nose toward the viewer
  if (fabsf(f.turnRate) > 1.2f && rnd01() < 0.65f) sign = -1;
  float to = clampf(f.sf + sign * mag, 0.74f, 1.28f);
  d.mode = 1; d.t = 0;
  d.dur = 0.45f + fabsf(to - f.sf) * 2.2f + rnd01() * 0.3f;
  d.from = f.sf; d.to = to;
  float s = to - f.sf;
  d.sign = (s > 0) ? 1.0f : (s < 0 ? -1.0f : 1.0f);
}

static void stepDepth(Fish& f, float dt) {
  DepthEv& d = f.depth;
  if (d.mode == 0) {
    d.cool -= dt;
    d.bell = fmaxf(0.0f, d.bell - dt * 3);
    if (d.cool <= 0) {
      startDepthEvent(f, false);
      d.cool = rnd(20, 40);
    }
    return;
  }
  d.t += dt;
  float p = clampf(d.t / d.dur, 0, 1);
  float e = p * p * (3 - 2 * p);              // smoothstep
  f.sf = d.from + (d.to - d.from) * e;
  d.bell = fsin((float)M_PI * p);             // transient 0->1->0
  if (p >= 1) { d.mode = 0; d.bell = 0; }
}

// --- spine chain -----------------------------------------------------------
void trailAt(const Trail& tr, float back, float& ox, float& oy, float& oz) {
  int n = 0;                                   // 0 = newest
  float acc = 0;
  int i0 = tr.idx(0);
  float px = tr.x[i0], py = tr.y[i0], pz = tr.z[i0];
  while (n < tr.n - 1 && acc < back) {
    int j = tr.idx(n + 1);
    float qx = tr.x[j], qy = tr.y[j], qz = tr.z[j];
    float dx = px - qx, dy = py - qy, dz = pz - qz;
    float seg = sqrtf(dx * dx + dy * dy + dz * dz);
    if (acc + seg >= back) {
      float t = (back - acc) / (seg > 1e-6f ? seg : 1e-6f);
      ox = px + (qx - px) * t;
      oy = py + (qy - py) * t;
      oz = pz + (qz - pz) * t;
      return;
    }
    acc += seg; px = qx; py = qy; pz = qz; n++;
  }
  ox = px; oy = py; oz = pz;
}

// Body length that takes the full ride, cm. The neon tetra is the smallest
// thing in the tank, so it is the one that goes wherever the water goes.
static const float LIFT_REF_CM = 3.0f;

static void stepChain(Fish& f) {
  Trail& tr = f.trail;
  int last = tr.idx(0);
  float dx = f.x - tr.x[last], dy = f.y - tr.y[last], dz = f.z - tr.z[last];
  if (dx * dx + dy * dy + dz * dz >= 0.45f * 0.45f) tr.push(f.x, f.y, f.z);

  // Each bone sits on the head's path, the body's own length behind it
  // (scaled by depth, as the renderer scales the segments), and is eased a
  // little towards the straight line behind the head so a body does not
  // wriggle through every kink the head made. Only x and y reach the screen:
  // a stretch of the path that runs into the depth of the tank shows short.
  // How much it keeps to the straight line: a deep, stiff-bodied fish - the
  // hatchetfish, the black tetra, a puffer in its box of a body - bends less
  // than the others, though it still bends.
  const SpKey key = f.cfg->key;
  const float kRelax = (key == SP_HATCHET || key == SP_BLACK || key == SP_PUFFER) ? 0.60f
                     : (key == SP_NEON) ? 0.5f : 0.35f;
  const float sp = f.cfg->spacing * f.sf;
  // The straight line is the chord of the path it has actually swum - head
  // to where the tail's point on the path is - not the way it is pointing
  // now. Pointing somewhere new does not turn the body; swimming there does,
  // so it comes round as it goes rather than before it sets off.
  float tx, ty, tz;
  trailAt(tr, (BONES - 1) * sp, tx, ty, tz);
  const float cx = (f.x - tx) * (1.0f / (BONES - 1));
  const float cy = (f.y - ty) * (1.0f / (BONES - 1));
  f.bones[0].x = f.x;
  f.bones[0].y = f.y;
  for (int b = 1; b < BONES; b++) {
    float px, py, pz;
    trailAt(tr, b * sp, px, py, pz);
    Bone& bone = f.bones[b];
    bone.x = px + (f.x - cx * b - px) * kRelax;
    bone.y = py + (f.y - cy * b - py) * kRelax;
  }
  // A hatchetfish's nose-up lean: the body tipped tail-down about the head.
  // Tail down on whichever side of the head each bone lies - mid-turn the
  // body is on both sides at once, and tipping it by the fish's overall
  // facing flicked the tail up and down as that facing flipped.
  if (f.tilt != 0.0f) {
    const float cp = fcos(f.tilt), s = fsin(f.tilt);
    for (int b = 1; b < BONES; b++) {
      float ox = f.bones[b].x - f.x, oy = f.bones[b].y - f.y;
      f.bones[b].x = f.x + ox * cp;
      f.bones[b].y = f.y + oy * cp + fabsf(ox) * s;
    }
  }
  // angles: the head along the body's axis, the rest along their segment
  f.bones[0].a = f.heading;
  for (int b = 1; b < BONES; b++) {
    float ddx = f.bones[b - 1].x - f.bones[b].x, ddy = f.bones[b - 1].y - f.bones[b].y;
    f.bones[b].a = (ddx * ddx + ddy * ddy > 0.04f) ? atan2f(ddy, ddx) : f.bones[b - 1].a;
  }
}

// How tight a turn is: a fish comes about on a curve of about a third of its
// own length, swimming round it; it does not spin on the spot.
static const float TURN_R = 0.22f;

// How far a fish lets its nose go up or down. Hardly at all: a fish swims
// level and climbs or drops at a shallow angle. Only a real dash to the
// surface or down to the bottom - or a loach running up the glass - goes
// anywhere near vertical.
static float pitchLimit(const Fish& f) {
  switch (f.act) {
    case ACT_DASH: case ACT_AIR:                 return 1.35f;
    case ACT_DANCE:                              return 1.25f;
    case ACT_SETTLE: case ACT_DIVE: case ACT_RETURN:
    case ACT_RISE:                               return 0.60f;
    default:                                     return 0.32f;
  }
}

// --- individual habits -----------------------------------------------------
// Returns the effort this fish is putting into swimming, and takes over its
// steering target while an act is running.
static float stepAct(Sim& sim, Fish& f, float dt) {
  const float surfaceY = VIEW::y0 + 3;

  // A real startle always wins - a parked fish bolts like any other. Not for
  // the corydoras or the shrimp though: both have their own reaction below,
  // and for both `nextAct` counts down to a fixed habit rather than to a mood.
  if (sim.stress > 0.45f && f.act != ACT_SWIM
      && f.cfg->key != SP_CORY && f.cfg->key != SP_SHRIMP
      && f.cfg->key != SP_HATCHET && f.cfg->key != SP_TRANSLUCENT && f.cfg->key != SP_LOACH) {
    f.act = ACT_SWIM;
    f.nextAct = rnd(8, 20);
  }

  float want = 1.0f;
  float wantThrash = 0.0f;

  if (f.cfg->key == SP_SHRIMP) {
    // A shrimp is not a fish and must not move like one, and it is not the
    // thing you are meant to be watching. Almost all of its day is spent
    // stopped, working one patch of sand over with its front claws; then it
    // creeps forward a body length and stops again. That is the whole animal:
    // something you notice has moved rather than something you watch moving.
    //
    // Now and then - a minute or two apart - it does pick up and swim to
    // another part of the bottom, level and without the tail sweep a fish has.
    // That is the only time it is obvious, and it is over in a few seconds.
    const float sandY = f.cfg->yHi - 2;
    switch (f.act) {
      case ACT_PICK:                 // stopped, picking the spot over
        // it does not hold perfectly still; it shuffles and turns on the spot
        f.tx = f.x + (fcos(f.heading) >= 0 ? 14.0f : -14.0f);
        f.ty = f.holdY + fsin(sim.tw * 1.7f + f.phase) * 0.8f;
        want = 0.02f;
        f.actT -= dt; f.nextAct -= dt;
        if (f.actT <= 0) {
          if (f.nextAct <= 0) {
            f.act = ACT_SWIMOFF; f.actT = rnd(2.5f, 5.0f);
            f.holdX = clampf(f.x + rnd(-80, 80), 24, 296);
            f.holdY = rnd(f.cfg->yLo + 2, sandY);
          } else {
            // a body length, no more - but walked rather than oozed, because
            // something that only moves between glances reads as a glitch
            f.act = ACT_CRAWL; f.actT = rnd(2.0f, 5.0f);
            f.holdX = clampf(f.x + rnd(-22, 22), 20, 300);
            f.holdY = clampf(f.holdY + rnd(-4, 4), f.cfg->yLo, sandY);
          }
        }
        break;

      case ACT_CRAWL:                // a walk to the next patch
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.32f;
        f.actT -= dt; f.nextAct -= dt;
        if ((fabsf(f.x - f.holdX) < 5 && fabsf(f.y - f.holdY) < 4) || f.actT <= 0) {
          f.act = ACT_PICK; f.actT = rnd(5.0f, 16.0f);
        }
        break;

      case ACT_SWIMOFF:              // pleopods: level, unhurried, purposeful
        f.tx = f.holdX; f.ty = f.holdY;
        want = 1.1f;
        f.actT -= dt;
        if ((fabsf(f.x - f.holdX) < 10 && fabsf(f.y - f.holdY) < 6) || f.actT <= 0) {
          f.act = ACT_PICK; f.actT = rnd(6.0f, 14.0f);
          f.nextAct = rnd(55, 150);
        }
        break;

      case ACT_FLICK:                // the caridoid escape, still travelling
        // The burst that carries it is applied once, when the flick starts;
        // all this does is keep the body writhing and stop it steering while
        // it is going backwards.
        f.tx = f.x + fcos(f.heading) * 20.0f;
        f.ty = f.y + fsin(f.heading) * 20.0f;
        want = 0.0f;
        wantThrash = 1.0f;
        f.actT -= dt;
        if (f.actT <= 0) {
          f.act = ACT_PICK; f.actT = rnd(1.0f, 3.0f);
          f.holdY = clampf(f.y, f.cfg->yLo, sandY);
        }
        break;

      default:
        f.act = ACT_PICK; f.actT = rnd(2.0f, 6.0f);
        f.holdY = clampf(f.y, f.cfg->yLo, sandY);
        break;
    }

    // Startle. A prawn does not turn and swim away, it snaps its abdomen under
    // itself and shoots backwards - the caridoid escape reaction - covering
    // several body lengths in a few tenths of a second before it has any idea
    // where it is going. The burst decays on its own, so it is set once here.
    //
    // Rare, though. A flat per-frame probability is not a probability at all:
    // at sixteen frames a second it fired within one frame of every startle
    // and then again the next, and the sand ended up popping like corn. The
    // chance is per second now, and each shrimp then ignores everything for a
    // minute or so afterwards - which is also what the animal does.
    //
    // `retarget` is the open-water wander timer and no act ever reads it, so
    // for a shrimp it is free to hold the cooldown.
    if (f.retarget > 0) f.retarget -= dt;
    if (sim.stress > 0.55f && f.act != ACT_FLICK && f.retarget <= 0
        && rnd01() < dt * 0.55f) {
      f.act = ACT_FLICK;
      f.actT = rnd(0.30f, 0.55f);
      f.retarget = rnd(40, 120);
      f.burstX = -fcos(f.heading) * 175.0f;
      f.burstY = -fsin(f.heading) * 175.0f - 38.0f;   // backwards and up
    }
    // however far the flick threw it, it belongs back down on the sand
    if (f.act == ACT_PICK && f.y < f.cfg->yLo - 10) {
      f.act = ACT_SWIMOFF; f.actT = rnd(2.5f, 5.0f);
      f.holdX = clampf(f.x + rnd(-30, 30), 24, 296);
      f.holdY = rnd(f.cfg->yLo + 4, sandY);
    }

    f.thrash += (wantThrash - f.thrash)
              * fminf(1.0f, dt * (wantThrash > f.thrash ? 14.0f : 4.0f));
    float ks = (want > f.effort) ? fminf(1.0f, dt * 5.0f) : fminf(1.0f, dt * 2.2f);
    f.effort += (want - f.effort) * ks;
    return f.effort;
  }

  if (f.cfg->key == SP_HATCHET) {
    // Under the surface film, cruising. The layer is only a few rows deep, so
    // the ordinary wander does the cruising; what is its own is going down a
    // little way and coming back up - "this species will descend from the
    // surface more often to interact with conspecifics, feed, or remain
    // motionless" - and the jump, which is handled in stepFish.
    switch (f.act) {
      case ACT_SWIM:
        f.nextAct -= dt;
        if (f.nextAct <= 0 && sim.stress < 0.2f) {
          f.act = ACT_DIVE; f.actT = 6.0f;
          f.holdX = clampf(f.x + (fcos(f.heading) >= 0 ? 1.0f : -1.0f) * rnd(20, 60), 30, 290);
          f.holdY = f.cfg->yHi + rnd(12, 42);
        }
        break;
      case ACT_DIVE:
        f.tx = f.holdX; f.ty = f.holdY; want = 0.55f;
        f.actT -= dt;
        if (fabsf(f.y - f.holdY) < 5 || f.actT <= 0) {
          f.act = ACT_RETURN; f.actT = 8.0f;
          f.holdX = clampf(f.x + (fcos(f.heading) >= 0 ? 1.0f : -1.0f) * rnd(30, 70), 30, 290);
        }
        break;
      case ACT_RETURN:
        f.tx = f.holdX; f.ty = f.cfg->yLo + 2; want = 0.7f;
        f.actT -= dt;
        if (f.y < f.cfg->yHi - 2 || f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(20, 60); }
        break;
      default:
        f.act = ACT_SWIM; f.nextAct = rnd(10, 40);
        break;
    }
    f.thrash += (0.0f - f.thrash) * fminf(1.0f, dt * 3.0f);
    float kh = (want > f.effort) ? fminf(1.0f, dt * 6.0f) : fminf(1.0f, dt * 1.6f);
    f.effort += (want - f.effort) * kh;
    return f.effort;
  }

  if (f.cfg->key == SP_TRANSLUCENT) {
    // Holding its place in the shoal, going nowhere under its own steam: the
    // aim is a point a little ahead of its place, and the effort is only what
    // it takes to get back to it. Which way is "ahead" is its own: when the
    // shoal's way changes it keeps facing as it was until its own moment
    // comes, then comes round by itself.
    if (f.ownDir != sim.tgcDir) {
      f.turnIn -= dt;
      if (f.turnIn <= 0) f.ownDir = sim.tgcDir;
    }
    const float dir = f.ownDir;
    const float px = sim.tgcX + f.holdX, py = sim.tgcY + f.holdY;
    const float ahead = (px - f.x) * dir;        // + when its place is ahead
    f.act = ACT_STATION;
    f.tx = px + dir * 30.0f;
    f.ty = py;
    want = clampf(0.05f + ahead * 0.05f + fabsf(py - f.y) * 0.02f, 0.03f, 0.9f);
    f.thrash += (0.0f - f.thrash) * fminf(1.0f, dt * 3.0f);
    float kg = (want > f.effort) ? fminf(1.0f, dt * 4.0f) : fminf(1.0f, dt * 2.0f);
    f.effort += (want - f.effort) * kg;
    return f.effort;
  }

  if (f.cfg->key == SP_LOACH) {
    // A shoal on the bottom, never far from one another, nosing through the
    // substrate in short runs. Two habits that are the reason people keep
    // them: lying on its side on the bottom as if dead, and running up and
    // down a corner of the glass.
    const float floorY = f.cfg->yHi - 1;
    float gx = 0; int ng = 0;                    // where the rest of them are
    for (int k = 0; k < sim.n; k++)
      if (sim.fish[k].cfg->key == SP_LOACH && &sim.fish[k] != &f) { gx += sim.fish[k].x; ng++; }
    gx = ng ? gx / ng : f.x;
    float wantFlat = 0.0f;
    switch (f.act) {
      case ACT_GRAZE:                      // a run along the bottom
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.34f;
        f.actT -= dt;
        if ((fabsf(f.x - f.holdX) < 9 && fabsf(f.y - f.holdY) < 6) || f.actT <= 0) {
          f.act = ACT_REST; f.actT = rnd(1.0f, 4.0f);
        }
        break;

      case ACT_REST:
        f.tx = f.x + (fcos(f.heading) >= 0 ? 20.0f : -20.0f);
        f.ty = f.holdY;
        want = 0.03f;
        f.actT -= dt;
        if (f.actT <= 0) {
          float r = rnd01();
          if (r < 0.10f) {
            f.act = ACT_PLAYDEAD; f.actT = rnd(6.0f, 18.0f);
          } else if (r < 0.18f) {
            f.act = ACT_DANCE; f.actT = 25.0f;
            f.reps = (int8_t)(3 + (int)rnd(0, 4));
            f.holdX = (f.x < 160) ? VIEW::x0 + 9 : VIEW::x1 - 9;
            f.holdY = floorY - rnd(45, 85);
          } else {
            f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.0f);
            // near the others, most of the time
            float cx = (rnd01() < 0.75f) ? gx : f.x;
            f.holdX = clampf(cx + rnd(-60, 60), 20, 300);
            f.holdY = floorY - rnd(0, (f.cfg->yHi - f.cfg->yLo) * 0.4f);
          }
        }
        break;

      case ACT_PLAYDEAD:                   // over on its side, not a fin moving
        f.tx = f.x + (fcos(f.heading) >= 0 ? 20.0f : -20.0f);
        f.ty = floorY;
        want = 0.0f;
        wantFlat = 1.0f;
        f.actT -= dt;
        if (f.actT <= 0) { f.act = ACT_REST; f.actT = rnd(0.5f, 1.5f); f.holdY = floorY; }
        break;

      case ACT_DANCE:                      // up the corner, down it, up again
        f.tx = f.holdX;
        f.ty = f.holdY;
        want = 1.1f;
        f.actT -= dt;
        if (fabsf(f.y - f.holdY) < 6 && fabsf(f.x - f.holdX) < 14) {
          if (--f.reps <= 0) {
            f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.0f);
            f.holdX = clampf(gx + rnd(-50, 50), 20, 300);
            f.holdY = floorY - rnd(0, 6);
          } else {
            f.holdY = (f.holdY > floorY - 20) ? floorY - rnd(45, 85) : floorY - rnd(0, 6);
          }
        }
        if (f.actT <= 0) {
          f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.0f);
          f.holdX = clampf(gx + rnd(-50, 50), 20, 300);
          f.holdY = floorY - rnd(0, 6);
        }
        break;

      default:
        f.act = ACT_GRAZE; f.actT = rnd(1.5f, 4.0f);
        f.holdX = clampf(f.x + rnd(-60, 60), 20, 300);
        f.holdY = floorY - rnd(0, 4);
        break;
    }
    // a startle gets it up off its side and moving
    if (sim.stress > 0.45f && (f.act == ACT_REST || f.act == ACT_PLAYDEAD)) {
      f.act = ACT_GRAZE; f.actT = rnd(1.0f, 2.5f);
      f.holdX = clampf(f.x + rnd(-80, 80), 20, 300);
      f.holdY = floorY - rnd(0, 6);
    }
    f.flat += (wantFlat - f.flat) * fminf(1.0f, dt * (wantFlat > f.flat ? 1.2f : 3.0f));
    f.thrash += (0.0f - f.thrash) * fminf(1.0f, dt * 3.0f);
    float kl = (want > f.effort) ? fminf(1.0f, dt * 6.0f) : fminf(1.0f, dt * 1.8f);
    f.effort += (want - f.effort) * kl;
    return f.effort;
  }

  if (f.cfg->key == SP_CORY) {
    // Bottom work in short scoots with long pauses, broken by a bolt to the
    // surface for a gulp of air and a quiet glide back down. baseSpeed is the
    // dash speed; the crawl is the same scale at a very low effort.
    const float floorY = f.cfg->yHi - 1;
    const float skyY   = VIEW::y0 + 3;
    switch (f.act) {
      case ACT_GRAZE:                      // a short scoot along the substrate
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.22f;
        f.actT -= dt; f.nextAct -= dt;
        if ((fabsf(f.x - f.holdX) < 9 && fabsf(f.y - f.holdY) < 6) || f.actT <= 0) {
          f.act = ACT_REST; f.actT = rnd(2.5f, 8.0f);
          f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
        }
        break;

      case ACT_REST:                       // sat on the sand, barely a fin
        f.tx = f.x + (fcos(f.heading) >= 0 ? 20.0f : -20.0f);
        f.ty = f.holdY;
        want = 0.03f;
        f.actT -= dt; f.nextAct -= dt;
        if (f.actT <= 0) {
          if (f.nextAct <= 0) {
            f.act = ACT_DASH; f.actT = 5.0f;
            f.holdX = clampf(f.x + rnd(-45, 45), 40, 280);
          } else {
            f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.5f);
            f.holdX = clampf(f.x + rnd(-80, 80), 20, 300);
            f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
          }
        }
        break;

      case ACT_DASH:                       // straight up, whole body writhing
        f.tx = f.holdX; f.ty = skyY;
        want = 5.5f;
        wantThrash = 1.0f;
        f.actT -= dt;
        if (f.y < skyY + 6 || f.actT <= 0) { f.act = ACT_AIR; f.actT = rnd(0.2f, 0.45f); }
        break;

      case ACT_AIR:                        // the gulp itself is very brief
        f.tx = f.x + (fcos(f.heading) >= 0 ? 6.0f : -6.0f);
        f.ty = skyY - 2;
        want = 0.05f;
        f.actT -= dt;
        if (f.actT <= 0) {
          f.act = ACT_SETTLE; f.actT = 14.0f;
          f.holdX = clampf(f.x + rnd(-95, 95), 20, 300);
          f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
        }
        break;

      case ACT_SETTLE:                     // and quietly back down, gliding
        f.tx = f.holdX; f.ty = f.holdY;
        want = 0.9f;
        f.actT -= dt;
        if (f.y > f.holdY - 9 || f.actT <= 0) {
          f.act = ACT_GRAZE; f.actT = rnd(2.0f, 5.5f);
          f.nextAct = rnd(35, 130);
        }
        break;

      default:                             // never mid-water; back to the sand
        f.act = ACT_GRAZE; f.actT = rnd(1.5f, 4.0f);
        f.holdX = clampf(f.x + rnd(-70, 70), 20, 300);
        f.holdY = floorY - rnd(0, (int)(f.cfg->yHi - f.cfg->yLo) / 3);
        break;
    }
    // A startle makes it scurry, but only if it is already down on the sand.
    // Interrupting a descent used to drop it into ACT_REST in mid-water, where
    // the effort is near zero - it just hung there as if snagged on something.
    if (sim.stress > 0.45f && f.act == ACT_REST) {
      f.act = ACT_GRAZE; f.actT = rnd(1.0f, 2.5f);
      f.holdX = clampf(f.x + rnd(-80, 80), 20, 300);
    }
    // safety net for any other way it could end up off the bottom while it
    // thinks it is working the substrate: glide back down instead of hovering
    if ((f.act == ACT_GRAZE || f.act == ACT_REST) && f.y < f.cfg->yLo - 12) {
      f.act = ACT_SETTLE; f.actT = 14.0f;
      f.holdX = clampf(f.x + rnd(-40, 40), 20, 300);
      f.holdY = floorY - rnd(0, 4);
    }
    f.thrash += (wantThrash - f.thrash)
              * fminf(1.0f, dt * (wantThrash > f.thrash ? 9.0f : 3.0f));
    float ke = (want > f.effort) ? fminf(1.0f, dt * 6.0f) : fminf(1.0f, dt * 1.8f);
    f.effort += (want - f.effort) * ke;
    return f.effort;
  }

  switch (f.act) {
    case ACT_HOLD:
      // hanging in open water, fins ticking over, going nowhere
      f.tx = f.holdX + (fcos(f.heading) >= 0 ? 22.0f : -22.0f);
      f.ty = f.holdY + fsin(sim.tw * 0.7f + f.phase) * 2.0f;
      want = 0.05f;
      f.actT -= dt;
      if (f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(1.5f, 5.0f); }
      break;

    case ACT_SWIM:
      f.nextAct -= dt;
      if (f.pers != PERS_NONE && f.nextAct <= 0 && sim.stress < 0.2f) {
        if (f.pers == PERS_HOVERER) {
          f.act = ACT_HOLD; f.actT = rnd(4, 15);
          f.holdX = f.x; f.holdY = f.y;
        } else if (f.pers == PERS_GULPER) {
          f.act = ACT_RISE; f.actT = 9.0f;
          f.holdX = clampf(f.x + rnd(-40, 40), 35, 285);
          f.holdY = surfaceY;
        } else {
          f.act = ACT_PARK; f.actT = 10.0f;
          const float lo = f.cfg->yLo, hi = f.cfg->yHi;
          if (rnd01() < 0.55f) {            // against a side pane
            f.holdX = (rnd01() < 0.5f) ? VIEW::x0 + 11 : VIEW::x1 - 11;
            f.holdY = rnd(lo + (hi - lo) * 0.3f, hi);
          } else {                          // hanging at the foot of its layer
            f.holdX = rnd(45, 275);
            f.holdY = hi - rnd(0, 8);
          }
        }
      }
      break;

    case ACT_RISE:
      f.tx = f.holdX; f.ty = f.holdY; want = 0.75f;
      f.actT -= dt;
      if (f.y < surfaceY + 6 || f.actT <= 0) { f.act = ACT_GULP; f.actT = rnd(0.7f, 1.7f); }
      break;

    case ACT_GULP:
      // hold station at the surface, nose up, barely moving
      f.tx = f.x + (fcos(f.heading) >= 0 ? 7.0f : -7.0f);
      f.ty = surfaceY - 2;
      want = 0.06f;
      f.actT -= dt;
      if (f.actT <= 0) {
        f.act = ACT_SINK; f.actT = 7.0f;
        f.holdY = rnd(VIEW::ymap(45), VIEW::ymap(92));
      }
      break;

    case ACT_SINK:
      f.tx = clampf(f.x + (fcos(f.heading) >= 0 ? 30.0f : -30.0f), 35, 285);
      f.ty = f.holdY; want = 0.5f;
      f.actT -= dt;
      if (fabsf(f.y - f.holdY) < 8 || f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(12, 32); }
      break;

    case ACT_PARK:
      f.tx = f.holdX; f.ty = f.holdY; want = 0.7f;
      f.actT -= dt;
      if ((fabsf(f.x - f.holdX) < 10 && fabsf(f.y - f.holdY) < 8) || f.actT <= 0) {
        f.act = ACT_HOVER; f.actT = rnd(4, 13);
      }
      break;

    case ACT_HOVER:
      // sitting still, pointing along the pane, fins just ticking over
      f.tx = f.holdX + (fcos(f.heading) >= 0 ? 22.0f : -22.0f);
      f.ty = f.holdY + fsin(sim.tw * 0.9f + f.phase) * 1.5f;
      want = 0.045f;
      f.actT -= dt;
      if (f.actT <= 0) { f.act = ACT_SWIM; f.nextAct = rnd(15, 38); }
      break;
  }

  f.thrash += (0.0f - f.thrash) * fminf(1.0f, dt * 3.0f);
  float k = (want > f.effort) ? fminf(1.0f, dt * 6.0f) : fminf(1.0f, dt * 1.6f);
  f.effort += (want - f.effort) * k;
  return f.effort;
}

// --- behaviour -------------------------------------------------------------
// A hatchetfish in the air. Once it has left the water nothing steers it: it
// flies out on its launch and falls back under gravity. If the arc carries it
// over the top of the picture it has jumped out of the tank, and it is gone
// for the rest of the session; if not, it drops back in and carries on.
static const float JUMP_G = 520.0f;             // px/s^2
static void addSplash(Sim& sim, SplashKind k, float x, float y, float vx, float vy,
                      float life, float size) {
  if (sim.nSplash >= MAX_SPLASH) return;
  sim.splash[sim.nSplash++] = { x, y, vx, vy, life, life, size, (uint8_t)k };
}

// Breaking out (exit) throws the most spray; dropping back in (entry) drives
// the most air down under the surface.
static void splashAt(Sim& sim, float x, float vx, bool exit) {
  const float S = SURFACE_Y;
  const int drops = exit ? 9 : 6, bubbles = exit ? 7 : 14;
  for (int k = 0; k < drops; k++)
    addSplash(sim, SPL_DROP, x + rnd(-3, 3), S - 1,
              vx * 0.35f + rnd(-45, 45), -rnd(50, exit ? 150 : 100),
              rnd(0.5f, 0.9f), rnd(0.7f, 1.2f));
  for (int k = 0; k < bubbles; k++)
    addSplash(sim, SPL_BUBBLE, x + rnd(-6, 6), S + rnd(1, exit ? 6 : 10),
              rnd(-12, 12), exit ? rnd(-10, 15) : rnd(10, 55),
              rnd(0.6f, 1.5f), rnd(0.8f, 1.8f));
}

static void stepSplash(Sim& sim, float dt) {
  for (int i = sim.nSplash - 1; i >= 0; i--) {
    Splash& p = sim.splash[i];
    p.life -= dt;
    bool dead = p.life <= 0;
    if (p.kind == SPL_DROP) {
      p.vy += JUMP_G * dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      if (p.vy > 0 && p.y > SURFACE_Y) dead = true;   // back into the water
    } else if (p.kind == SPL_BUBBLE) {
      p.vy += (-38.0f - p.vy) * fminf(1.0f, dt * 3.0f);   // buoyancy wins
      p.vx *= expf(-2.0f * dt);
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      if (p.y < SURFACE_Y) dead = true;                   // burst at the top
    }
    if (dead) sim.splash[i] = sim.splash[--sim.nSplash];
  }
}

static void stepAirborne(Sim& sim, Fish& f, float dt) {
  const float py = f.y;
  f.vyAir += JUMP_G * dt;
  f.x += f.vxAir * dt;
  f.y += f.vyAir * dt;
  // dropping back through the surface (the way out is splashed at launch)
  if (py <= SURFACE_Y && f.y > SURFACE_Y) splashAt(sim, f.x, f.vxAir, false);
  f.heading = atan2f(f.vyAir, f.vxAir);
  f.prevHeading = f.heading;
  f.mirror = (f.vxAir >= 0) ? 1.0f : -1.0f;
  f.yaw = (f.vxAir >= 0) ? 0.0f : (float)M_PI;
  f.pitch = atan2f(f.vyAir, fabsf(f.vxAir));
  f.fore = 1.0f;
  f.speedNorm = 1.0f;
  f.beat += dt * (float)M_PI * 2 * f.cfg->beatHz * 2.5f;
  if (f.beat > TRIG_WRAP) f.beat -= TRIG_WRAP;
  f.tilt = 0;
  if (f.y < -(float)f.cfg->H - 4) { f.gone = true; return; }
  if (f.vyAir > 0 && f.y > f.cfg->yLo) {        // fell back in
    f.air = false;
    f.speed = f.cfg->baseSpeed;
    f.burstY = f.vyAir * 0.3f;
  }
  if (f.x < VIEW::x0 - 6) f.x = VIEW::x0 - 6;
  if (f.x > VIEW::x1 + 6) f.x = VIEW::x1 + 6;
  stepChain(f);
}

static const float SCARE_REACH = 80.0f;         // px: how far the fright spreads

// A hatchetfish just leapt at x: the others near it shoot off sideways, away
// from the splash.
static void fleeFrom(Sim& sim, float x) {
  for (int i = 0; i < sim.n; i++) {
    Fish& h = sim.fish[i];
    if (h.cfg->key != SP_HATCHET || h.air || h.jumpIn >= 0) continue;
    if (fabsf(h.x - x) > SCARE_REACH || h.y > h.cfg->yHi + 6) continue;
    if (h.thrash > 0.5f) continue;                 // already bolting
    const float d = h.x - x;
    const float dir = (fabsf(d) > 1.0f) ? (d > 0 ? 1.0f : -1.0f)
                                        : (rnd01() < 0.5f ? -1.0f : 1.0f);
    // The startle turn is all but instant, so it simply faces the way it bolts
    // and the body is laid out behind it; then the burst carries it off.
    if ((fcos(h.yaw) >= 0 ? 1.0f : -1.0f) != dir) {
      h.yaw = h.heading = (dir > 0) ? 0.0f : (float)M_PI;
      h.pitch = 0; h.yawV = 0; h.mirror = dir; h.prevHeading = h.heading;
      h.trail.reset();
      for (int q = Trail::CAP - 1; q >= 0; q--)
        h.trail.push(h.x - dir * q * 0.5f, h.y, h.z);
    }
    // the nearer the splash, the harder it goes
    const float k = 1.0f - fabsf(d) / SCARE_REACH;
    h.burstX = dir * (120.0f + 110.0f * k) * rnd(0.85f, 1.15f);
    h.burstY = rnd(-8, 18);
    h.thrash = 1.0f;
  }
}

// Something gave the hatchets a fright at x. One or two of those near it go
// up, a moment apart; the rest scatter away from each splash as it happens.
static void scareHatchets(Sim& sim, float x) {
  int near[N_FISH], nn = 0;
  for (int i = 0; i < sim.n; i++) {
    const Fish& h = sim.fish[i];
    if (h.cfg->key != SP_HATCHET || h.air || h.jumpIn >= 0) continue;
    if (fabsf(h.x - x) > SCARE_REACH || h.y > h.cfg->yHi + 6) continue;
    near[nn++] = i;
  }
  if (!nn) return;
  // shuffle, then the first one or two jump
  for (int k = nn - 1; k > 0; k--) {
    int j = (int)rnd(0, k + 1); if (j > k) j = k;
    int t = near[k]; near[k] = near[j]; near[j] = t;
  }
  const int jumpers = (rnd01() < 0.6f) ? 1 : 2;
  for (int k = 0; k < nn; k++) {
    Fish& h = sim.fish[near[k]];
    if (k < jumpers) h.jumpIn = rnd(0.0f, 0.18f);
  }
}

static void stepScare(Sim& sim, float dt) {
  sim.scareIn -= dt;
  if (sim.scareIn > 0) return;
  sim.scareIn = rnd(0.4f, 1.6f) * SCARE_MEAN;
  // from wherever one of them happens to be
  int cand[N_FISH], nc = 0;
  for (int i = 0; i < sim.n; i++)
    if (sim.fish[i].cfg->key == SP_HATCHET && !sim.fish[i].air) cand[nc++] = i;
  if (nc) scareHatchets(sim, sim.fish[cand[(int)rnd(0, nc) % nc]].x);
}

static void stepFish(Sim& sim, Fish& f, float dt) {
  const SpeciesCfg* c = f.cfg;

  if (f.air) { stepAirborne(sim, f, dt); return; }
  if (c->key == SP_HATCHET && f.jumpIn >= 0) {
    f.jumpIn -= dt;
    if (f.jumpIn < 0) {
      f.jumpIn = -1.0f;
      // Off the surface, forward and up, already going as fast as it will
      // ever go: out of the water nothing adds to that, gravity only takes
      // it away, and it is over the rim before that shows.
      // each at its own angle, mostly the way it faces
      f.air = true;
      const float fwd = (fcos(f.heading) >= 0 ? 1.0f : -1.0f) * (rnd01() < 0.85f ? 1.0f : -1.0f);
      const float ang = rnd(0.12f, 1.05f);         // off vertical, rad
      const float spd = rnd(360, 450);
      f.vxAir = fwd * fsin(ang) * spd;
      f.vyAir = -fcos(ang) * spd;
      splashAt(sim, f.x, f.vxAir, true);
      fleeFrom(sim, f.x);
      stepAirborne(sim, f, dt);
      return;
    }
  }

  const float eff = stepAct(sim, f, dt);
  const bool busy = (f.act != ACT_SWIM);      // an act owns the steering target

  // steering target
  if (busy) {
    // stepAct() already set tx/ty
  } else if (f.school) {
    const School& s = sim.school[f.sid];
    float oa = sim.tw * 0.35f + f.phase;
    f.tx = s.x + fcos(oa) * f.orbitR;
    f.ty = s.y + fsin(oa * 0.8f + f.phase) * f.orbitR * 0.60f;
  } else {
    f.retarget -= dt;
    float dx = f.tx - f.x, dy = f.ty - f.y;
    if (f.retarget <= 0 || dx * dx + dy * dy < 100) {
      f.retarget = rnd(6, 16);
      if (c->roam > 0) {
        // holds a patch of the tank rather than crossing it
        f.tx = clampf(f.x + rnd(-c->roam, c->roam), 22, 298);
        f.ty = clampf(f.y + rnd(-c->roam * 0.45f, c->roam * 0.45f),
                      c->yLo, c->yHi);
      } else {
        // Aim at the far side of the tank rather than at a fresh uniform
        // point. A slow fish never reaches a uniform target before the timer
        // re-rolls it, and the mean of those targets is the middle of the
        // screen - which is exactly where every fish ends up. Picking a
        // destination it has to travel to keeps the tank evenly used.
        float lo, hi;
        if ((f.x < 160.0f) != (rnd01() < 0.25f)) {      // usually cross over
          lo = fminf(f.x + 70.0f, 250.0f); hi = 298.0f;
        } else {
          lo = 22.0f; hi = fmaxf(f.x - 70.0f, 70.0f);
        }
        f.tx = rnd(lo, hi);
        // upper part of its own layer most of the time, with occasional dives
        float mid = c->yLo + (c->yHi - c->yLo) * 0.55f;
        f.ty = rnd01() < 0.80f ? rnd(c->yLo, mid) : rnd(mid, c->yHi);
      }
    }
  }

  // soft wall avoidance - suspended while an act is deliberately holding the
  // fish against a pane, the substrate or the surface
  if (!busy) {
    if (f.x < VIEW::x0 + 8) f.tx = fmaxf(f.tx, 120.0f);
    if (f.x > VIEW::x1 - 8) f.tx = fminf(f.tx, 200.0f);
    if (f.y < c->yLo + 4) f.ty = fmaxf(f.ty, c->yLo + (c->yHi - c->yLo) * 0.35f);
    if (f.y > c->yHi - 4) f.ty = fminf(f.ty, c->yHi - (c->yHi - c->yLo) * 0.35f);
  }

  // --- steering: yaw to face the target's side, pitch to climb or drop ---
  const float dxT = f.tx - f.x, dyT = f.ty - f.y;
  // Face the side the target is on - but only once it is clearly on the other
  // side, or a fish climbing to a point straight above it would spin in place.
  float side = (fcos(f.yaw) >= 0) ? 1.0f : -1.0f;
  if (dxT * side < -8.0f) side = -side;
  float dYaw = wrapAng((side > 0 ? 0.0f : (float)M_PI) - f.yaw);
  if (fabsf(dYaw) > 3.0f) dYaw = f.turnSide * fabsf(dYaw);    // a reversal
  else if (fabsf(dYaw) < 0.05f) f.turnSide = rnd01() < 0.5f ? -1.0f : 1.0f;
  // Eased, not a constant rate switched on and off: it winds up into the turn
  // and slows as it comes round to face the new way. A turn that starts and
  // stops dead reads as a jerk at both ends.
  // how fast it can come round is how fast it is swimming round the curve
  const float vNow = f.speed * fmaxf(eff, 0.7f);
  const float maxRate = fminf(c->turnRate * (1.1f + 0.6f * f.speedNorm),
                              fmaxf(c->turnRate * 0.5f, vNow / (TURN_R * c->W * f.sf)));
  const float wantV = clampf(dYaw * 4.0f, -maxRate, maxRate);
  f.yawV += (wantV - f.yawV) * fminf(1.0f, dt * 9.0f);
  float yawStep = f.yawV * dt;
  if (fabsf(yawStep) > fabsf(dYaw) && fabsf(dYaw) < 0.3f) { yawStep = dYaw; f.yawV = 0; }
  f.yaw = fwrap(f.yaw + yawStep);

  const float pmax = pitchLimit(f);
  const float pWant = clampf(atan2f(dyT, fmaxf(fabsf(dxT), 6.0f)), -pmax, pmax);
  f.pitch += clampf(pWant - f.pitch, -1.8f * dt, 1.8f * dt);

  const float cyaw = fcos(f.yaw);
  const float newMirror = (cyaw >= 0) ? 1.0f : -1.0f;
  f.mirror = newMirror;
  f.fore = fmaxf(0.22f, fabsf(cyaw));
  f.heading = atan2f(fsin(f.pitch), f.mirror * fcos(f.pitch));

  // what the fins and the roll respond to is how fast it is coming round
  float inst = yawStep / fmaxf(dt, 1e-4f);
  f.prevHeading = f.heading;
  f.turnRate += (inst - f.turnRate) * fminf(1.0f, dt * 7);

  float spd = c->baseSpeed * (0.8f + 0.35f * fsin(sim.tw * 0.23f + f.phase));
  f.speed += (spd - f.speed) * fminf(1.0f, dt * 2);
  float boost = (1 + sim.stress * 1.3f
              + (f.school && !busy ? sim.school[f.sid].dash * 0.9f : 0.0f)) * eff;
  f.speedNorm = clampf((f.speed * boost) / (c->baseSpeed * 2.4f), 0, 1);

  // --- caught in the air stone ---------------------------------------------
  // Water moving is water moving; what differs between fish is how much of it
  // they go along with. A 3cm neon has almost no mass to anchor it and almost
  // no muscle to argue with, and both of those scale with its length, so the
  // ride it gets goes as roughly the square of how much smaller it is than the
  // reference. In the tank that reads clearly: neons are thrown up the column,
  // guppies drift up it, and a 5cm black tetra tips a little and swims on.
  float wx, wy;
  airFlowAt(f.x, f.y, &wx, &wy);
  if (wy != 0.0f) {
    const float r = LIFT_REF_CM / c->lenCm;
    const float k = clampf(r * r, 0.25f, 1.4f);
    wx *= k;
    wy *= k;
    // it does not take it lying down
    f.thrash = fmaxf(f.thrash, k * fminf(1.0f, -wy * (1.0f / 45.0f)) * 0.45f);
    f.lifted = 1.2f;
  }

  // across the glass it travels at cos(yaw) of its speed: mid-turn, nose to
  // the glass, it is swimming towards or away from you and barely moves
  // and it keeps swimming while it comes round - that is what carries it
  // round the curve - so the effort does not drop below a cruise mid-turn
  const float turnBoost = (fabsf(dYaw) > 0.4f && eff < 0.7f && eff > 0.0f)
                        ? 0.7f / eff : 1.0f;
  const float vdx = cyaw * fcos(f.pitch), vdy = fsin(f.pitch);
  const float vdz = fsin(f.yaw) * fcos(f.pitch);
  f.x += (vdx * f.speed * boost * turnBoost + f.burstX + f.sepX + wx) * dt;
  f.y += (vdy * f.speed * boost * turnBoost + f.burstY + f.sepY + wy) * dt;
  f.z += vdz * f.speed * boost * turnBoost * dt;
  f.x = clampf(f.x, VIEW::x0 - 6, VIEW::x1 + 6);

  // The layer clamp would undo the lift the moment it applied, so it is held
  // open while the fish is in the plume and until it has swum back down - a
  // fixed timeout would snap a corydoras from mid-water back onto the sand.
  if (f.lifted > 0) {
    f.lifted -= dt;
    if (f.lifted <= 0 && f.y < c->yLo - 8) f.lifted = 0.4f;
  }
  // Free to leave the layer while an act is running (a guppy going up for air,
  // a corydoras bolting to the surface), but not otherwise. The shrimp is the
  // exception: every one of its acts is an act, so going by `busy` would leave
  // it unclamped for its whole life - and it lives in a strip of sand a dozen
  // rows deep, below which is the near edge of the photograph.
  const bool loose = (c->key == SP_SHRIMP)
                   ? (f.lifted > 0 || f.act == ACT_FLICK)
                   : (busy || f.lifted > 0);
  if (loose) f.y = clampf(f.y, VIEW::y0 - 6, VIEW::y1 + 4);
  else       f.y = clampf(f.y, c->yLo - 8, c->yHi + 8);

  f.burstX *= expf(-3 * dt);
  f.burstY *= expf(-3 * dt);

  f.beat += dt * (float)M_PI * 2 * c->beatHz *
            (0.65f + 0.9f * f.speedNorm + sim.stress * 0.5f + f.thrash * 2.2f);
  if (f.beat > TRIG_WRAP) f.beat -= TRIG_WRAP;

  stepDepth(f, dt);

  // The art is drawn head-right; a fish facing left is mirrored, and the flip
  // happens as it passes nose-on (steering above).
  const float chd = cyaw;

  // The card does not mirror, it turns over - and it does not turn over as a
  // separate event either.
  //
  // Animating the flip on its own timer was the mistake. However carefully it
  // was paced, it was still a second thing happening beside the swimming, and
  // it always looked like the card stopped, turned, and then set off again.
  //
  // The card's face simply follows the direction it is travelling: broadside
  // while it is crossing the tank, edge-on when it is pointed straight up or
  // down, and over onto its back once it has come about. Now there is only one
  // motion. It cannot finish rotating before it starts moving, because the
  // rotation *is* the movement - the card is only ever part-way over because
  // the fish is part-way round its arc.
  //
  // The page-like pacing comes free with it. Width is cos(heading), so its
  // rate of change is sin(heading) x turn rate: barely moving while the card
  // is near flat, fastest as it passes through edge-on, settling again on the
  // other side. That is the profile the hand-written easing was trying to
  // imitate, and this one cannot drift out of step with the fish.
  if (c->key == SP_CARD) {
    // signed width, flattened so it only goes truly thin near the reversal
    const float w = (chd >= 0 ? 1.0f : -1.0f)
                  * powf(fabsf(chd), CARD_FACE_FLAT);
    const float target = acosf(clampf(w, -1.0f, 1.0f));
    f.turn += (target - f.turn) * fminf(1.0f, dt * CARD_FACE_EASE);
  }

  // Fin billow. A guppy's fan barely moves while it cruises; it opens like a
  // skirt when the fish banks into a turn and then settles back slowly, so the
  // envelope attacks fast and releases slow.
  DepthEv& d = f.depth;
  {
    float want = clampf(d.bell * 0.9f + fabsf(f.turnRate) * 0.45f, 0, 1.5f);
    float k = (want > f.flare) ? fminf(1.0f, dt * 10.0f) : fminf(1.0f, dt * 2.2f);
    f.flare += (want - f.flare) * k;
  }

  float roll = clampf(f.turnRate * 0.12f + d.bell * d.sign * 1.1f, -2, 2);
  f.biasF += (roll - f.biasF) * fminf(1.0f, dt * 9);
  // nose-on during a turn shows the far eye too, as the depth lunge does
  f.facing = fminf(1 - d.bell * 0.85f, 0.25f + 0.75f * fabsf(cyaw));

  // The molly's sail. Laid flat along the back while it is going fast, and
  // raised - slowly, as a sail is - when it slows and comes round. The turn
  // counts for more than the speed: a slow turn is when it is widest.
  if (c->key == SP_MOLLY) {
    float fast = clampf((f.speedNorm - 0.30f) / 0.35f, 0.0f, 1.0f);
    float turning = clampf(fabsf(f.turnRate) / 1.0f, 0.0f, 1.0f);
    float want = 1.0f - 0.78f * fast * (1.0f - turning);
    float k = (want < f.dorsal) ? fminf(1.0f, dt * 4.0f) : fminf(1.0f, dt * 1.3f);
    f.dorsal += (want - f.dorsal) * k;
  }

  // A hatchetfish's back is dead straight and it rides with it against the
  // surface, so at the top of the tank it swims with its nose lifted. The
  // chain hangs the body back from the head along the lean.
  if (c->key == SP_HATCHET) {
    float want = (f.act == ACT_SWIM && f.y < c->yHi) ? 0.22f : 0.05f;
    f.tilt += (want - f.tilt) * fminf(1.0f, dt * 2.0f);
  }

  stepChain(f);
}

static void stepSchool(School& s, float dt) {
  s.timer -= dt;
  float dx = s.tx - s.x, dy = s.ty - s.y;
  float dist = sqrtf(dx * dx + dy * dy);
  if (s.timer <= 0 || dist < 12) {
    s.timer = rnd(5, 11);
    bool far = rnd01() < 0.25f;
    s.dash = far ? 1.0f : 0.0f;
    // each shoal roams around its own stretch of the tank
    s.tx = clampf(s.home + rnd(-95, 95) + (far ? rnd(-60, 60) : 0), 25, 295);
    s.ty = clampf(s.homeY + rnd(-20, 20), NEON.yLo + 8, NEON.yHi - 8);
  }
  float v = s.dash > 0 ? 60.0f : 22.0f;
  if (dist > 0.5f) {
    s.x += (dx / dist) * fminf(v * dt, dist);
    s.y += (dy / dist) * fminf(v * dt, dist);
  }
  s.dash = fmaxf(0.0f, s.dash - dt * 0.5f);
}

// The translucent glass catfish shoal. It holds a place in mid-water for a while, then
// drifts to another - usually swimming there head first, so the turn to face
// the new way is one movement for all of them - and now and then simply turns
// round where it is.
static void stepTranslucentShoal(Sim& sim, float dt) {
  // the way the shoal faces: held for 30-180 s, then changed, with each fish
  // given its own moment to come round inside a 30-60 s grace period
  sim.tgcHold -= dt;
  if (sim.tgcHold <= 0) {
    const float grace = rnd(TGC_GRACE_MIN, TGC_GRACE_MAX);
    sim.tgcDir = -sim.tgcDir;
    for (int i = 0; i < sim.n; i++)
      if (sim.fish[i].cfg->key == SP_TRANSLUCENT) sim.fish[i].turnIn = rnd(0, grace);
    sim.tgcHold = grace + rnd(TGC_HOLD_MIN, TGC_HOLD_MAX);
    // and it stays put while they come round
    sim.tgcTx = sim.tgcX; sim.tgcTy = sim.tgcY;
    sim.tgcT = grace;
  }
  // Where it holds. It only ever drifts the way it is facing - a fish can
  // hold station, but it does not reverse into its place.
  sim.tgcT -= dt;
  if (sim.tgcT <= 0) {
    sim.tgcT = rnd(15, 45);
    sim.tgcTx = clampf(sim.tgcX + sim.tgcDir * rnd(0, 90), 70, 250);
    sim.tgcTy = rnd(TRANSLUCENT.yLo + 14, TRANSLUCENT.yHi - 14);
  }
  float dx = sim.tgcTx - sim.tgcX, dy = sim.tgcTy - sim.tgcY;
  float d = sqrtf(dx * dx + dy * dy);
  const float v = 5.0f;                          // px/s: barely drifting
  if (d > 0.5f) {
    sim.tgcX += dx / d * fminf(v * dt, d);
    sim.tgcY += dy / d * fminf(v * dt, d);
  }
}

// Crowd separation. The browser build has none - with 8 tetras on one orbit it
// never needed it, but 20 of them stack into a single blob. Neighbours inside
// roughly one body length push each other apart, vertically a little less so
// the school keeps its flat shape.
static void stepSeparation(Sim& sim) {
  const float STRENGTH = 26.0f;      // px/s at contact
  for (int i = 0; i < sim.n; i++) { sim.fish[i].sepX = 0; sim.fish[i].sepY = 0; }
  for (int i = 0; i < sim.n; i++) {
    Fish& a = sim.fish[i];
    for (int j = i + 1; j < sim.n; j++) {
      Fish& b = sim.fish[j];
      float dx = b.x - a.x, dy = b.y - a.y;
      float R = (a.cfg->W + b.cfg->W) * 0.45f;
      float d2 = dx * dx + dy * dy;
      if (d2 >= R * R || d2 < 1e-4f) continue;
      float d = sqrtf(d2);
      float push = (1 - d / R) * STRENGTH;
      float ux = dx / d, uy = dy / d;
      a.sepX -= ux * push;  a.sepY -= uy * push * 0.7f;
      b.sepX += ux * push;  b.sepY += uy * push * 0.7f;
    }
  }
}

// ---------------------------------------------------------------------------
// A corydoras blunders into a shrimp.
//
// The Amano are the hardest thing in the tank to notice: small, translucent,
// the colour of the sand, and deliberately almost motionless. They are also
// sharing the substrate with two catfish that spend all day working along it
// with their heads down. In a real tank that meeting happens constantly and it
// always ends the same way - the shrimp is gone before the fish has registered
// it was there, which is the single most shrimp-like thing an Amano does and,
// conveniently, the thing most likely to make you look at one.
//
// So it is not a startle, it is a contact reflex: no threshold on `stress`, no
// minute-long cooldown, no dice roll. Close enough, and it goes.
static const float CORY_TOUCH = 18.0f;    // px between centres

static void stepShrimpScatter(Sim& sim) {
  for (int i = 0; i < sim.n; i++) {
    Fish& sh = sim.fish[i];
    if (sh.cfg->key != SP_SHRIMP || sh.act == ACT_FLICK) continue;
    for (int j = 0; j < sim.n; j++) {
      const Fish& co = sim.fish[j];
      if (co.cfg->key != SP_CORY && co.cfg->key != SP_LOACH) continue;
      const float dx = sh.x - co.x, dy = sh.y - co.y;
      const float d2 = dx * dx + dy * dy;
      if (d2 > CORY_TOUCH * CORY_TOUCH) continue;

      // Away from the fish rather than simply backwards. A real prawn does not
      // aim - it snaps its abdomen and goes wherever that sends it - but at
      // 320x240 a shrimp that flicks *into* the catfish reads as a collision
      // rather than as an escape, and the whole point of this is to be read.
      const float d = sqrtf(d2);
      float ux, uy;
      if (d > 0.01f) { ux = dx / d; uy = dy / d; }
      else           { ux = (rnd01() < 0.5f ? -1.0f : 1.0f); uy = 0.0f; }
      // and off the bottom, because that is where the room is
      uy -= 0.85f;
      const float n = sqrtf(ux * ux + uy * uy);

      sh.act = ACT_FLICK;
      sh.actT = rnd(0.28f, 0.48f);
      sh.burstX = ux / n * 200.0f;
      sh.burstY = uy / n * 200.0f;
      sh.thrash = 1.0f;
      // Do not touch `retarget`: that is the cooldown on being startled by the
      // tank at large, and being trodden on is not the same thing. It is the
      // flick's own half second, plus the sixty-odd pixels the burst carries
      // it, that stops this re-triggering every frame.
      break;
    }
  }
}

void stepSim(Sim& sim, float dt) {
  sim.t += dt;
  sim.tw += dt;
  if (sim.tw > TRIG_WRAP) sim.tw -= TRIG_WRAP;
  sim.stress *= expf(-1.1f * dt);
  for (int k = 0; k < N_SCHOOLS; k++) stepSchool(sim.school[k], dt);
  stepTranslucentShoal(sim, dt);
  stepSplash(sim, dt);
  stepScare(sim, dt);
  stepSeparation(sim);
  stepShrimpScatter(sim);
  for (int i = 0; i < sim.n; i++) stepFish(sim, sim.fish[i], dt);
  // a hatchetfish that jumped clean out is not coming back
  for (int i = sim.n - 1; i >= 0; i--)
    if (sim.fish[i].gone) {
      Serial.println("a hatchetfish jumped out of the tank");
      sim.fish[i] = sim.fish[--sim.n];
    }

  // water sway: damped spring driven by tap surges
  for (int i = sim.nSurge - 1; i >= 0; i--) {
    Surge& s = sim.surges[i];
    s.t += dt;
    if (s.t < s.dur) {
      float p = s.t / s.dur;
      sim.swayV += s.dir * s.mag * fsin(p * (float)M_PI) * dt * 8;
    } else {
      sim.surges[i] = sim.surges[--sim.nSurge];
    }
  }
  sim.swayV += -sim.sway * 26 * dt - sim.swayV * 5.5f * dt;
  sim.sway += sim.swayV * dt;
  sim.sway = clampf(sim.sway, -3.5f, 3.5f);

  // bubbles
  sim.nextBubble -= dt;
  if (sim.nextBubble <= 0 && sim.nBub < MAX_BUB) {
    sim.nextBubble = rnd(1.6f, 4.5f);
    sim.bubbles[sim.nBub++] = { rnd(20, 300), VIEW::floorY - 3.0f,
                                rnd01() < 0.7f ? 0.8f : 1.3f,
                                rnd(9, 16), rnd(0, 6), rnd(0.25f, 0.5f) };
  }
  for (int i = sim.nBub - 1; i >= 0; i--) {
    Bubble& b = sim.bubbles[i];
    b.y -= b.vy * dt;
    b.x += fsin(sim.tw * 3 + b.ph) * 3 * dt;
    if (b.y < VIEW::horizonY + 4) sim.bubbles[i] = sim.bubbles[--sim.nBub];
  }

  // drifting motes
  for (int i = 0; i < N_MOTES; i++) {
    Mote& m = sim.motes[i];
    m.x += (m.vx + fsin(sim.tw * 0.4f + m.ph) * 0.5f) * dt;
    m.y += m.vy * dt;
    if (m.y > VIEW::floorY - 3) m.y = VIEW::horizonY + 3;
    if (m.y < VIEW::horizonY + 2) m.y = VIEW::floorY - 4;
    if (m.x < 6) m.x = 314;
    if (m.x > 314) m.x = 6;
  }
}

// The water column sloshes sideways, the school darts away, nearby fish burst
// and a couple lunge toward the viewer. No surface ripples (side view).
void tapWater(Sim& sim, float x, float y) {
  float dir = x < 160 ? 1.0f : -1.0f;
  if (sim.nSurge < MAX_SURGE) sim.surges[sim.nSurge++] = { 0, 0.9f, dir, 1 };
  sim.stress = fminf(1.0f, sim.stress + 0.7f);
  // every shoal darts away from the tap
  for (int k = 0; k < N_SCHOOLS; k++) {
    School& s = sim.school[k];
    // bolting away from the tap, but still around its own stretch of glass -
    // otherwise a tap near one end herds every shoal into the same corner
    s.tx = clampf(s.home + (s.home < x ? -1.0f : 1.0f) * rnd(35, 95), 25, 295);
    s.ty = clampf(s.homeY + rnd(-22, 22), NEON.yLo + 8, NEON.yHi - 8);
    s.timer = 4; s.dash = 1;
  }

  // nearby fish burst away; the two closest lunge toward the viewer
  int   order[N_FISH];
  float dist[N_FISH];
  for (int i = 0; i < sim.n; i++) {
    order[i] = i;
    float dx = sim.fish[i].x - x, dy = sim.fish[i].y - y;
    dist[i] = sqrtf(dx * dx + dy * dy);
  }
  for (int i = 1; i < sim.n; i++) {          // insertion sort by distance
    int   ki = order[i];
    float kd = dist[ki];
    int   j = i - 1;
    while (j >= 0 && dist[order[j]] > kd) { order[j + 1] = order[j]; j--; }
    order[j + 1] = ki;
  }
  for (int k = 0; k < sim.n; k++) {
    Fish& f = sim.fish[order[k]];
    float d = dist[order[k]];
    if (d > 110) break;
    float kk = 1 - d / 110;
    float ang = atan2f(f.y - y, f.x - x);
    f.burstX += fcos(ang) * 85 * kk;
    f.burstY += fsin(ang) * 60 * kk;
  }
  for (int i = 0; i < 2 && i < sim.n; i++) {
    if (dist[order[i]] < 130) startDepthEvent(sim.fish[order[i]], true);
  }
}
