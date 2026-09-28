#include "rig.h"
#include "cardfish.h"

// ---------------------------------------------------------------------------
// Water layers. A community tank stratifies - that is most of what stops it
// reading as one soup of fish - so each species gets the band it actually uses,
// in panel rows. The swim area runs from SWIM_TOP (30) to SWIM_BOT (205); see
// sim.h. Neighbouring bands overlap a little on purpose.
//
//   guppy        30 .. 118   upper to middle
//   black tetra  84 .. 142   middle, and it barely leaves it
//   neon tetra  100 .. 185   middle to lower
//   amano shrimp 188 .. 207  the sand, a shade nearer than the corydoras
//   corydoras   188 .. 205   on the sand
//
// and the second eight:
//
//   hatchetfish      30 ..  48   right under the surface
//   striped panchax  30 ..  72   top water
//   green puffer     30 .. 130   upper to middle
//   sailfin molly    30 .. 140   upper to middle, like the guppies
//   platy            30 .. 185   anywhere
//   translucent glass catfish
//                   100 .. 175   middle to lower
//   nothobranchius  100 .. 185   middle to lower
//   clown loach     180 .. 205   the bottom
//
// ---------------------------------------------------------------------------
// Sizes come from each photo's own aspect ratio (tools/make_fish.py prints
// them), so the long-finned strains really are taller than the short-tailed
// one. At 24px for a 4.5cm guppy the scale is 5.3 px/cm, which puts a 3cm neon
// tetra at 16px.
const SpeciesCfg NEON = {
  SP_NEON, "NEON TETRA", 20, 3.0f,
  12, 5, 12.0f / 4,
  14.0f, 4.2f, 3.1f,
  { 0, 0.25f, 0.60f, 1.15f, 1.80f },
  8.7f, 0.15f, 0.0f, 100.0f, 185.0f, 0.30f, 0, NEON_ART,
};

const SpeciesCfg GUPPY_STRAINS[5] = {
  { SP_GUPPY, "GUPPY", 2, 4.5f, 24, 13, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.2f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 0, GUPPY1_ART },
  { SP_GUPPY, "GUPPY", 2, 4.5f, 24, 12, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.0f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 1, GUPPY2_ART },
  { SP_GUPPY, "GUPPY", 2, 4.5f, 24, 12, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 14.3f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 2, GUPPY3_ART },
  { SP_GUPPY, "GUPPY", 1, 4.5f, 24, 10, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.6f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 3, GUPPY4_ART },
  { SP_GUPPY, "GUPPY", 1, 4.5f, 24, 13, 24.0f / 4, 9.0f, 2.4f, 2.3f,
    { 0, 0.18f, 0.60f, 1.4f, 2.8f }, 15.3f, 0.55f, 0.0f, 30.0f, 118.0f, 0.38f, 4, GUPPY5_ART },
};

// Gymnocorymbus ternetzi. Deep, laterally flattened body with a big black
// anal-fin skirt. In a group of three - well under the shoal size the species
// wants - they do not shoal; they hang in mid-water, hold station for long
// stretches and only drift a short way between stops.
const SpeciesCfg BLACKTETRA = {
  SP_BLACK, "BLACK TETRA", 3, 5.0f,
  24, 14, 24.0f / 4,
  9.0f, 2.0f, 1.6f,
  { 0, 0.08f, 0.25f, 0.65f, 1.50f },       // stiffer than the others
  18.1f, 0.35f, 70.0f, 84.0f, 142.0f, 0.30f, 0, BLACK_ART,
};

// Armoured catfish: works the substrate in short scoots with long pauses, and
// every minute or two bolts to the surface for a gulp of air (it breathes
// through its gut) before settling quietly back down. baseSpeed is the dash
// speed - the crawl comes from a very low effort, so both extremes fit in one
// scale.
const SpeciesCfg CORYDORAS = {
  SP_CORY, "CORYDORAS", 2, 5.0f,
  24, 14, 24.0f / 4,
  26.0f, 3.0f, 2.6f,
  { 0, 0.12f, 0.35f, 0.80f, 1.50f },
  17.7f, 0.25f, 90.0f, 188.0f, 205.0f, 0.25f, 0, CORY_ART,
};

// Caridina multidentata. Not a fish at all, and it is the one thing in the tank
// that mostly is not swimming: an Amano spends its day walking over the
// substrate and the hardscape picking algae off it with its front claws, in
// short scoots with long stops, and only pushes off and swims - level, on its
// pleopods, not on a tail - when it wants to be somewhere else. Startle it and
// it does the one thing that makes a shrimp unmistakable: flicks its abdomen
// and shoots *backwards*.
//
// Females reach 5-6cm and males 3.5-4.5cm. This one is sized as a male rather
// than a female: a shrimp is not the thing you are meant to be looking at, and
// at female size it read as a pale object sitting on the photograph rather than
// as something living on the sand. It is also baked translucent, because that
// is what the animal is.
//
// The body is nearly rigid - almost all of a shrimp's flex is in the abdomen
// and only during that escape flick - so A[] is a fraction of what a tetra gets
// and the tail beat is really the swimmerets ticking over.
//
// The sprite is the body only. Its antennae are as long as it is and would
// have eaten a fifth of the sprite width for two hairs that disappear at this
// size, so they are cropped out of the bake and drawn in renderer.cpp instead.
const SpeciesCfg YAMATO = {
  SP_SHRIMP, "AMANO SHRIMP", 5, 4.0f,
  24, 8, 24.0f / 4,
  16.0f, 3.6f, 5.0f,
  { 0, 0.05f, 0.12f, 0.30f, 0.62f },
  19.7f, 0.10f, 60.0f, 188.0f, 207.0f, 0.18f, 0, SHRIMP_ART,
};

// Once per boot, every guppy in the tank becomes this and stays that way. It
// is authored at the guppy's own 24px width so the bone spacing is unchanged
// and the spine chain carries straight through the swap, but everything else
// says "fried": almost no body wave, no fin flare, and a slow stiff cruise.
const SpeciesCfg EBIFRY = {
  SP_EBI, "EBI FRY", 0, 4.5f,
  24, 9, 24.0f / 4,
  7.0f, 1.6f, 1.2f,
  { 0, 0.05f, 0.15f, 0.35f, 0.70f },
  19.0f, 0.05f, 0.0f, 30.0f, 118.0f, 0.20f, 0, EBIFRY_ART,
};

// The card fish. `count` and `art` are filled in at boot from whatever is on
// the SD card, and stay at 0/nullptr when there is no card - see cardfish.cpp.
//
// It is given the water the fish use rather than a layer of its own: a drawing
// belongs in the middle of the tank where you can see it, not tucked into a
// band. A[] is never read, because the card renderer does not use the spine.
//
// baseSpeed and turnRate are not free choices. Their ratio is the turning
// radius, and for a rigid card that radius is the whole difference between
// coming about and spinning on the spot: at the 11px/s and 1.7rad/s this
// started with, the radius was six pixels - a fifth of the card's own length,
// so it pivoted inside itself and read as a turntable. 17 and 0.70 put it at
// about 24px, three quarters of a body length, which is a sweeping arc you can
// watch it travel through. Slower and it cannot get out of a corner; faster
// and the paper is darting, which paper does not do.
SpeciesCfg CARDFISH = {
  SP_CARD, "CARD FISH", 0, 5.0f,
  CARD_W, CARD_H, (float)CARD_W / (BONES - 1),
  17.0f, 0.70f, 0.85f,
  { 0, 0, 0, 0, 0 },
  0.0f, 0.0f, 0.0f, 48.0f, 158.0f, 0.20f, 0, nullptr,
};

// ---------------------------------------------------------------------------
// The second eight. Sizes are the 5.3 px/cm of the rest, from aquarium-sized
// specimens, but nothing goes past 32px: the strip renderer's per-column
// tables are sized for that (renderer.cpp). The 640x480 bakes are twice these.
//
// How many of each go in, and when, is the stocking in sim.h/makeSim(); the
// `count` here is not read for these.

// Gasteropelecus sternicla. Lives right under the surface - its straight back
// against the film, keel hanging below - and cruises along it. Now and then it
// goes down a little way and comes back up, and now and then it jumps: they
// "clear several metres in a single jump", and in a tank without a lid that is
// the last you see of it. Deep keel, stiff body: small A[].
const SpeciesCfg HATCHET = {
  SP_HATCHET, "SILVER HATCHET", 4, 5.0f,
  24, 11, 24.0f / 4,
  11.0f, 2.2f, 2.6f,
  { 0, 0.06f, 0.18f, 0.50f, 1.20f },       // stiffer than the others
  16.9f, 0.20f, 0.0f, 30.0f, 48.0f, 0.20f, 0, HATCHET_ART,
};

// Translucent glass catfish, Kryptopterus bicirrhis as the trade has it (the
// fish actually sold under the name is now thought to be K. vitreolus). Not to
// be confused with the other fish sold as "glass catfish". It does not go to
// the bottom like other catfish: it lives "in mid-water, in a shoal, swimming
// with a wriggle of the body", and by day it hardly moves. Here the shoal
// hangs in the middle to lower water all facing one way, and turns together.
// baseSpeed is low because it is mostly holding station; the shoal itself
// moves (sim.cpp).
const SpeciesCfg TRANSLUCENT = {
  SP_TRANSLUCENT, "TRANSLUCENT GLASS CATFISH", 6, 7.0f,
  32, 12, 32.0f / 4,
  9.0f, 1.8f, 3.2f,
  { 0, 0.10f, 0.35f, 0.80f, 1.60f },
  26.3f, 0.25f, 0.0f, 100.0f, 175.0f, 0.20f, 0, TRANSLUCENT_ART,
};

// Chromobotia macracanthus, a juvenile. A shoaling bottom-dweller that forages
// along the substrate in a group, and is known for its oddities: it lies on
// its side on the bottom "appearing dead", and runs up and down a corner of
// the glass. baseSpeed is the dash, as for the corydoras.
const SpeciesCfg CLOWNLOACH = {
  SP_LOACH, "CLOWN LOACH", 3, 6.0f,
  28, 12, 28.0f / 4,
  24.0f, 3.0f, 2.8f,
  { 0, 0.15f, 0.40f, 0.90f, 1.60f },
  21.1f, 0.20f, 90.0f, 180.0f, 205.0f, 0.25f, 0, LOACH_ART,
};

// Dichotomyctere nigroviridis. Swims like the guppies (upper to middle water),
// but a puffer's body does not bend - it sculls with its tail and hovers on
// fluttering pectorals - so A[] stays near zero until the tail.
const SpeciesCfg PUFFER = {
  SP_PUFFER, "GREEN PUFFER", 1, 5.0f,
  24, 9, 24.0f / 4,
  8.0f, 3.2f, 2.0f,
  { 0, 0.04f, 0.12f, 0.50f, 1.40f },
  18.0f, 0.30f, 0.0f, 30.0f, 130.0f, 0.30f, 0, PUFFER_ART,
};

// Poecilia latipinna, a male. Guppy behaviour; the sail is what is special,
// and it is not a fixed part of the picture: laid flat along the back while it
// is going fast, raised when it slows and comes round (renderer.cpp).
const SpeciesCfg SAILFIN = {
  SP_MOLLY, "SAILFIN MOLLY", 1, 8.0f,
  32, 18, 32.0f / 4,
  9.5f, 2.0f, 2.0f,
  { 0, 0.16f, 0.50f, 1.20f, 2.40f },
  24.7f, 0.45f, 0.0f, 30.0f, 140.0f, 0.35f, 0, MOLLY_ART,
};

// Xiphophorus maculatus. Guppy behaviour, but it uses the whole height of the
// tank - "found in all aquarium levels" - and picks at surfaces.
const SpeciesCfg PLATY = {
  SP_PLATY, "PLATY", 2, 5.0f,
  24, 13, 24.0f / 4,
  9.0f, 2.4f, 2.3f,
  { 0, 0.18f, 0.55f, 1.30f, 2.50f },
  16.8f, 0.40f, 0.0f, 30.0f, 185.0f, 0.38f, 0, PLATY_ART,
};

// Aplocheilus lineatus. A top-dwelling killifish that hangs under the surface
// waiting for whatever lands on it: guppy behaviour, but kept to the top of
// the tank and given to holding still.
const SpeciesCfg PANCHAX = {
  SP_PANCHAX, "STRIPED PANCHAX", 1, 8.0f,
  32, 10, 32.0f / 4,
  11.0f, 2.2f, 2.2f,
  { 0, 0.20f, 0.60f, 1.40f, 2.60f },
  24.5f, 0.30f, 0.0f, 30.0f, 72.0f, 0.30f, 0, PANCHAX_ART,
};

// Nothobranchius rachovii, a male. Guppy behaviour, lower down - it "spends
// most of its time in the lower levels" - and quicker.
const SpeciesCfg NOTHO = {
  SP_NOTHO, "NOTHOBRANCHIUS", 1, 5.0f,
  24, 11, 24.0f / 4,
  12.5f, 2.8f, 2.8f,
  { 0, 0.18f, 0.60f, 1.40f, 2.80f },
  18.5f, 0.50f, 0.0f, 100.0f, 185.0f, 0.38f, 0, NOTHO_ART,
};

const SpeciesCfg* const NEW_SPECIES[N_NEWSP] = {
  &HATCHET, &TRANSLUCENT, &CLOWNLOACH, &PUFFER, &SAILFIN, &PLATY, &PANCHAX, &NOTHO,
};

Rig RIGS[N_RIGS];

const Rig* rigFor(const SpeciesCfg* cfg) {
  for (int i = 0; i < N_RIGS; i++) if (RIGS[i].cfg == cfg) return &RIGS[i];
  return &RIGS[0];
}

bool buildRigs() {
  RIGS[0].cfg = &NEON;
  RIGS[0].spr = NEON.art;
  for (int i = 0; i < 5; i++) {
    RIGS[i + 1].cfg = &GUPPY_STRAINS[i];
    RIGS[i + 1].spr = GUPPY_STRAINS[i].art;
  }
  RIGS[6].cfg = &BLACKTETRA;  RIGS[6].spr = BLACKTETRA.art;
  RIGS[7].cfg = &CORYDORAS;   RIGS[7].spr = CORYDORAS.art;
  RIGS[8].cfg = &YAMATO;      RIGS[8].spr = YAMATO.art;
  RIGS[9].cfg = &EBIFRY;      RIGS[9].spr = EBIFRY.art;
  CARDFISH.art = CARD_ART;    // still nullptr if there was no usable card
  RIGS[10].cfg = &CARDFISH;   RIGS[10].spr = CARDFISH.art;
  for (int i = 0; i < N_NEWSP; i++) {
    RIGS[11 + i].cfg = NEW_SPECIES[i];
    RIGS[11 + i].spr = NEW_SPECIES[i]->art;
  }
  return true;
}
