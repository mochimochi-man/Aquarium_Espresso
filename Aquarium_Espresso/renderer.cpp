// ---------------------------------------------------------------------------
// renderer.cpp — bent-strip fish renderer + tank environment.
//
// Fish are drawn as horizontal 1px slices of the sprite: each slice is mapped
// through the local spine transform of its bone segment, offset by the
// undulation wave. A vertical pitch scale fakes the body pointing toward/away
// from the viewer; a source-row shift fakes roll (bank); the guppy gets a
// procedural trailing caudal veil so the fan keeps breathing.
//
// The browser draws each slice with drawImage(); here the whole segment is one
// oriented quad rasterised with an inverse transform and 2x2 samples per
// pixel, which gives the same picture with the anti-aliasing canvas provided
// for free.
//
// Everything the browser drew *around* the fish - water gradient, god-ray
// shafts, horizon and floor lines, wall shadows, bubbles, drifting motes, the
// slosh shimmer and the depth rail - is replaced by the tank photo backdrop.
// ---------------------------------------------------------------------------
#pragma GCC optimize("O3")
#include "renderer.h"
#include "light.h"
#include "bubbles.h"
#include "cardfish.h"
#include "fastmath.h"
#include <esp_timer.h>

// Temporary breakdown counters (us), read + cleared by the sketch. Per core,
// because both of them are inside these routines at the same time; the sketch
// adds the two together, so what it prints is CPU time, not wall time.
uint32_t tRestore[2] = { 0, 0 }, tVeil[2] = { 0, 0 };
uint32_t tSeg[2] = { 0, 0 },     tExtra[2] = { 0, 0 };

// The lighting pass rewrites every pixel from the backdrop, so the dirty-rect
// path that the unlit build used cannot apply. Set to 0 to get it back.
#define TANK_LIGHTING 1

// Samples per pixel in the strip renderer. The sprites already carry soft
// alpha edges from the photo downsample, so most of what supersampling buys
// here is smoothing the rotation; two diagonal samples keep that and halve the
// cost of the hottest loop in the frame. 4 = full 2x2, 1 = nearest.
#define SEG_SAMPLES 2

// Water haze. There is a column of lit water between every fish and the front
// glass, and it scatters its own light in. Without it the fish read as cut-outs
// laid on the photo however well their brightness is matched. The amount rides
// on the depth factor `sf` (1.28 nearest, 0.74 furthest), and the haze colour
// is the water's own, lit by the same lamp as everything else.
//
// Both this and FISH_LIGHT_MIX are deliberately gentler than what the backdrop
// gets. At full strength the tank looked right but the fish did not: a guppy
// sitting in a caustic shadow lost a third of its brightness and a chunk of
// its saturation, and a neon tetra's stripes - two pixels of pure colour - are
// the first thing to go. The fish only need enough of the effect to belong in
// the water, not all of it.
static const float HAZE_MAX = 0.10f;
static const int   HAZE_R8 = 46, HAZE_G8 = 86, HAZE_B8 = 92;

// fraction of the backdrop's light variation the fish take
static const float FISH_LIGHT_MIX = 0.30f;

static inline float hazeFor(float sf) {
  float h = (1.28f - sf) * (HAZE_MAX / 0.54f);
  return h < 0 ? 0 : (h > HAZE_MAX ? HAZE_MAX : h);
}

// the scene's light gain, softened so it shades the fish without draining them
static inline float fishLight(int x, int y) {
  float g = lightGainAt(x, y) * (1.0f / 256.0f);
  return 1.0f - (1.0f - g) * FISH_LIGHT_MIX;
}

// a fin/veil colour, lit and veiled the same way the body is
static inline uint16_t hazed(int r, int g, int b, float lg, float h) {
  float k = 1.0f - h;
  return rgb565((int)(r * lg * k + HAZE_R8 * lg * h),
                (int)(g * lg * k + HAZE_G8 * lg * h),
                (int)(b * lg * k + HAZE_B8 * lg * h));
}

#if SEG_SAMPLES == 4
static const float SS_OX[4] = { 0.25f, 0.75f, 0.25f, 0.75f };
static const float SS_OY[4] = { 0.25f, 0.25f, 0.75f, 0.75f };
#elif SEG_SAMPLES == 2
static const float SS_OX[2] = { 0.30f, 0.70f };
static const float SS_OY[2] = { 0.30f, 0.70f };
#else
static const float SS_OX[1] = { 0.5f };
static const float SS_OY[1] = { 0.5f };
#endif
#include <string.h>

static inline float clampf(float v, float a, float b) {
  return v < a ? a : (v > b ? b : v);
}


// ---------------------------------------------------------------------------
// one bone segment of a fish, as an oriented textured quad
static void IRAM_ATTR drawSegment(const Rig& rig, const Fish& f, int b,
                                  float sY, float sf, int biasVal, float drift,
                                  float offX, float mirror) {
  const SpeciesCfg* c = f.cfg;
  const int W = c->W, H = c->H;
  const int segs = BONES - 1;
  const float segW = (float)W / segs;
  const int   segWi = (int)segW;
  const float kappa = (float)(M_PI * 2) / (2.6f * W);

  const Bone& b0 = f.bones[b];
  const Bone& b1 = f.bones[b + 1];
  float mx = (b0.x + b1.x) * 0.5f + offX;
  float my = (b0.y + b1.y) * 0.5f;
  // Each segment is drawn as it lies: as long on screen as its bones are
  // apart (it foreshortens where the body runs into the depth of the tank),
  // and facing whichever way it points, so in a turn the head comes round
  // first and the tail last.
  const float sdx = b0.x - b1.x, sdy = b0.y - b1.y;
  const float slen = sqrtf(sdx * sdx + sdy * sdy);
  const float gsf = sf * f.lenScale;               // drawn size (depth + surface copies)
  const float full = segW * gsf;
  const float fore = clampf(slen / (full > 0.1f ? full : 0.1f), 0.22f, 1.0f);
  // A segment foreshortened almost to nothing mid-turn still points the way it
  // points: its direction comes from a wider span of the bones around it, not
  // from the fish's overall heading - which is the *new* way while this part
  // of the body, the tail especially, is still on its way round.
  float ddx = sdx, ddy = sdy;
  if (slen <= 0.25f * full) {
    const Bone& wa = f.bones[b > 0 ? b - 1 : 0];
    const Bone& wb = f.bones[b + 2 < BONES ? b + 2 : BONES - 1];
    ddx = wa.x - wb.x; ddy = wa.y - wb.y;
  }
  const float dl = sqrtf(ddx * ddx + ddy * ddy);
  if (fabsf(ddx) > 0.15f * dl && dl > 0.05f * full) mirror = (ddx >= 0) ? 1.0f : -1.0f;
  if (f.vflip) mirror = -mirror;                   // seen in the surface, belly up
  float ang = (dl > 0.05f * full) ? atan2f(ddy, ddx)
            : ((mirror == f.mirror) ? f.heading : (float)M_PI - f.heading);
  // A fish nose-on to the glass mid-turn, climbing or dropping a little, has
  // a segment whose screen direction is nearly straight up or down - and the
  // sprite laid along that stood the part on end like a hook. Seen nose-on a
  // fish is short, not upright, so the tilt a segment is drawn at is held to
  // the fish's own climb plus a little.
  {
    const float lim = fabsf(f.pitch) + 0.15f;
    if (dl > 0.05f * full) {
      float tilt = atan2f(ddy, fabsf(ddx) > 1e-4f ? fabsf(ddx) : 1e-4f);
      if (tilt > lim || tilt < -lim) {
        tilt = clampf(tilt, -lim, lim);
        // which way along x it points: its own, or the facing when it has none
        const float sx = (fabsf(ddx) > 0.15f * dl) ? ddx : (f.mirror);
        ang = (sx >= 0) ? tilt : (float)M_PI - tilt;
      }
    }
  }
  int srcX = (int)(W - (b + 1) * segW);

  // The browser evaluates the undulation once per bone segment, so the wave is
  // a staircase - fine on a 12px tetra, but it makes a guppy's fan swing like a
  // rigid plate. Evaluating it per sprite column instead costs a handful of
  // table lookups and gives a wave that runs smoothly out through the fin.
  //
  // The fin also opens vertically with `flare`: a fancy guppy's tail hangs
  // fairly still while it cruises and billows out like a skirt when it turns.
  // The opening is applied as a per-column stretch of the sprite about the
  // spine, tapering to nothing at the peduncle.
  float latCol[8], shrinkCol[8];
  float latMin = 1e9f, latMax = -1e9f, expMax = 1.0f;
  {
    const float sweep = fsin(f.beat * 0.37f + f.phase) * 0.25f;
    // A tail beats side to side, which from the side is mostly into and out of
    // the glass; drawn as an up-and-down wave it has to stay small, and all but
    // vanish when the fish is only drifting.
    const float spd = (0.10f + 0.60f * f.speedNorm + f.thrash * 1.4f) * sY;
    const float open = f.flare * 0.55f;            // extra fin height
    const float fanSweep = 0.35f + 1.1f * f.flare; // fin-only sweep scale
    const float finSpan = (float)(W - 1) - c->finFrom;
    for (int j = 0; j < segWi; j++) {
      float sArc = (float)(W - 1 - (srcX + j));      // distance behind the head
      float fanW = (sArc - c->finFrom) / (finSpan > 0.1f ? finSpan : 1.0f);
      fanW = clampf(fanW, 0.0f, 1.0f);

      float t = sArc / c->spacing;                   // in bone units
      int   ti = (int)t;
      if (ti > BONES - 2) ti = BONES - 2;
      float amp = c->A[ti] + (c->A[ti + 1] - c->A[ti]) * (t - ti);
      amp *= (1.0f - fanW) + fanW * fanSweep;

      float v = amp * fsin(f.beat - kappa * sArc + sweep) * spd;
      latCol[j] = v;
      if (v < latMin) latMin = v;
      if (v > latMax) latMax = v;

      float expand = 1.0f + open * fanW;
      shrinkCol[j] = 1.0f / expand;
      if (expand > expMax) expMax = expand;
    }
  }

  float ca = fcos(ang), sa = fsin(ang);
  float tx = mx + drift * ca, ty = my + drift * sa;

  // turned from broadside, the body is foreshortened along its own length
  const float dw = segW * gsf * fore + 1.2f;
  const float lx0 = -segW * gsf * fore * 0.5f - 0.3f;
  // the membrane ripple is part of the billow too, not a constant flutter
  const float ripA = (c->A[b] + c->A[b + 1]) * 0.5f * c->fanRipple * sY
                   * (0.25f + 0.9f * f.flare);
  const float mid = (H - 1) * 0.5f;

  // Row offsets without the lateral wave, which is now per column; the fin
  // surface also ripples across its own height so it reads as a membrane
  // rather than a solid paddle.
  float dyv[20];
  float dyLo = 1e9f, dyHi = -1e9f;
  for (int r = 0; r < H; r++) {
    dyv[r] = (r - mid) * sY
           + ripA * fsin(f.beat * 0.53f + r * 0.55f + f.phase) - 0.5f;
    if (dyv[r] < dyLo) dyLo = dyv[r];
    if (dyv[r] > dyHi) dyHi = dyv[r];
  }
  dyHi += 1.18f;

  // local y extent, after the wave, the fin opening and mirroring
  // the -0.5 row offset inside dyv gets stretched too, so the slack only needs
  // to cover that - a flat +-1 here cost 20% of the segment loop
  const float slack = (expMax - 1.0f) * 0.6f + 0.15f;
  float lyTop = dyLo * expMax + latMin - slack;
  float lyBot = dyHi * expMax + latMax + slack;
  if (mirror < 0) { float t = lyTop; lyTop = -lyBot; lyBot = -t; }

  // Where the body bends, neighbouring segments are turned against each other
  // and on the outside of the bend a wedge opens between them - a deep body
  // like the molly's showed it as a bite out of the back and the root of the
  // tail. Each segment reaches past its ends by as much as that wedge is wide
  // at the body's edge, repeating its end column, so the joints stay closed.
  float extF = 0.0f, extR = 0.0f;
  {
    // Only as much as closes the wedge, and never more than a fraction of the
    // segment as it shows: a foreshortened segment next to a turned one has a
    // big angle between them, and stretching its end column that far stuck it
    // out of the fish.
    const float edge = (H * 0.5f) * sY * 0.6f;
    const float cap = 0.3f * segW * gsf * fore;
    if (b > 0) {
      const float an = atan2f(f.bones[b - 1].y - b0.y, f.bones[b - 1].x - b0.x);
      extF = fminf(cap, edge * fabsf(fsin(an - ang)));
    }
    if (b + 2 < BONES) {
      const float an = atan2f(b1.y - f.bones[b + 2].y, b1.x - f.bones[b + 2].x);
      extR = fminf(cap, edge * fabsf(fsin(an - ang)));
    }
  }
  const float qx0 = lx0 - extR, qx1 = lx0 + dw + extF;

  // world bounding box of the quad
  float xmin = 1e9f, xmax = -1e9f, ymin = 1e9f, ymax = -1e9f;
  const float cx[4] = { qx0, qx1, qx1, qx0 };
  const float cy[4] = { lyTop, lyTop, lyBot, lyBot };
  for (int i = 0; i < 4; i++) {
    float wx = tx + cx[i] * ca - cy[i] * sa;
    float wy = ty + cx[i] * sa + cy[i] * ca;
    if (wx < xmin) xmin = wx;
    if (wx > xmax) xmax = wx;
    if (wy < ymin) ymin = wy;
    if (wy > ymax) ymax = wy;
  }
  const int cid = xPortGetCoreID();
  int ix0 = (int)floorf(xmin), ix1 = (int)ceilf(xmax) + 1;
  int iy0 = (int)floorf(ymin), iy1 = (int)ceilf(ymax) + 1;
  if (ix0 < 0) ix0 = 0;
  if (iy0 < gClipY0[cid]) iy0 = gClipY0[cid];
  if (ix1 > FB_W) ix1 = FB_W;
  if (iy1 > gClipY1[cid]) iy1 = gClipY1[cid];
  if (ix0 >= ix1 || iy0 >= iy1) return;

  const RGBA8* spr = rig.spr;
  // The sailfin molly's sail is not drawn with the body: cut into the body's
  // segments it came apart at every joint when the fish bent. drawSail() lays
  // it along the back in one piece instead; here it is left out.
  const int8_t* sail = (c->key == SP_MOLLY) ? MOLLY_DORSAL : nullptr;
  const float invDw = 1.0f / dw;
  const float invSY = 1.0f / sY;
  const float uScale = invDw * segW;
  const float invCa = (fabsf(ca) > 1e-4f) ? 1.0f / ca : 0.0f;
  const float invSa = (fabsf(sa) > 1e-4f) ? 1.0f / sa : 0.0f;

  // Accumulating premultiplied colour lets the write go through px_blend_pm(),
  // and these constants fold the 0..255 sample sums straight into 5/6/5.
  // The caustics play over the fish too, sampled once per bone segment so a
  // band can sweep along a body instead of switching the whole fish at once.
  // Without this the fish sit at full brightness on a shaded tank and read as
  // stickers rather than as something the light is moving across.
  // The ebi-fry is a joke, and a joke that has to read instantly. It skips the
  // light and the haze entirely so it stays flat, bright and obvious - looking
  // pasted on is the point.
  const bool gag = (c->key == SP_EBI);
  const float lg = gag ? 1.0f : fishLight((int)tx, (int)ty);
  // The haze folds into these constants for free: the accumulator already has
  // the coverage sum, so scaling the sprite colour by (1-h) and adding the
  // water tint times the coverage costs nothing in the pixel loop.
  const float h = (gag || f.dry) ? 0.0f : hazeFor(sf), k = 1.0f - h;
  // premultiplied, so a fainter copy is every term scaled together
  const float am = f.alphaMul;
  const float PR = 31.0f / (255.0f * SEG_SAMPLES) * lg * k * am;
  const float PG = 63.0f / (255.0f * SEG_SAMPLES) * lg * k * am;
  const float PB = 31.0f / (255.0f * SEG_SAMPLES) * lg * k * am;
  const float HR = (HAZE_R8 * 31.0f / 255.0f) * lg * h / SEG_SAMPLES * am;
  const float HG = (HAZE_G8 * 63.0f / 255.0f) * lg * h / SEG_SAMPLES * am;
  const float HB = (HAZE_B8 * 31.0f / 255.0f) * lg * h / SEG_SAMPLES * am;
  const float AL = 32.0f / SEG_SAMPLES * am;

  float lxRow[SEG_SAMPLES], lyRow[SEG_SAMPLES];
  for (int y = iy0; y < iy1; y++) {
    // Clip the row to the exact span where the rotated quad lives instead of
    // walking its bounding box - most of that box is outside the fish.
    float dxc = (ix0 + 0.5f) - tx, dyc = (y + 0.5f) - ty;
    float lxc = dxc * ca + dyc * sa;
    float lyc = -dxc * sa + dyc * ca;
    float u0 = 0.0f, u1 = (float)(ix1 - ix0);
    if (invCa != 0.0f) {
      float a0 = (qx0 - lxc) * invCa, a1 = (qx1 - lxc) * invCa;
      if (a0 > a1) { float t2 = a0; a0 = a1; a1 = t2; }
      if (a0 > u0) u0 = a0;
      if (a1 < u1) u1 = a1;
    } else if (lxc < qx0 || lxc >= qx1) {
      continue;
    }
    if (invSa != 0.0f) {
      float a0 = (lyc - lyBot) * invSa, a1 = (lyc - lyTop) * invSa;
      if (a0 > a1) { float t2 = a0; a0 = a1; a1 = t2; }
      if (a0 > u0) u0 = a0;
      if (a1 < u1) u1 = a1;
    } else if (lyc < lyTop || lyc >= lyBot) {
      continue;
    }
    if (u1 < u0) continue;
    int xs = ix0 + (int)(u0 + 4096.0f) - 4097;    // floor(u0) - 1
    int xe = ix0 + (int)(u1 + 4096.0f) - 4094;    // floor(u1) + 2
    if (xs < ix0) xs = ix0;
    if (xe > ix1) xe = ix1;
    if (xs >= xe) continue;

    for (int ss = 0; ss < SEG_SAMPLES; ss++) {
      float dxw = xs + SS_OX[ss] - tx;
      float dyw = y + SS_OY[ss] - ty;
      lxRow[ss] = dxw * ca + dyw * sa;
      lyRow[ss] = -dxw * sa + dyw * ca;
    }
    for (int x = xs; x < xe; x++) {
      float ar = 0, ag = 0, ab = 0, aa = 0;
      for (int ss = 0; ss < SEG_SAMPLES; ss++) {
        float lx = lxRow[ss], ly = lyRow[ss];
        lxRow[ss] += ca;
        lyRow[ss] -= sa;
        if (lx < qx0 || lx >= qx1) continue;

        int col = (int)((lx - lx0) * uScale);
        if (col < 0) col = 0;
        else if (col >= segWi) col = segWi - 1;

        // mirror the local frame for a leftward fish, take the wave off, then
        // undo the fin's opening to land back on a sprite row
        float lyr = (ly * mirror - latCol[col]) * shrinkCol[col];

        // floor() without the libm call: the bias keeps the argument positive
        int r = (int)(lyr * invSY + mid + 4096.5f) - 4096;
        int hit = -1;
        for (int k = 1; k >= -1; k--) {          // later rows win, as on canvas
          int rr = r + k;
          if (rr < 0 || rr >= H) continue;
          if (lyr >= dyv[rr] && lyr < dyv[rr] + 1.18f) { hit = rr; break; }
        }
        if (hit < 0) continue;
        int art = hit - biasVal;
        if (art < 0 || art >= H) continue;
        int sc = srcX + col;
        if (sail && sail[sc] >= 0 && art * 4 + 2 < sail[sc]) continue;

        const RGBA8& sp = spr[art * W + sc];
        if (!sp.a) continue;
        float a = sp.a * (1.0f / 255.0f);
        ar += sp.r * a; ag += sp.g * a; ab += sp.b * a; aa += a;
      }
      if (aa <= 0.004f) continue;
      int al = (int)(aa * AL + 0.5f);
      if (al <= 0) continue;
      if (al > 32) al = 32;
      px_blend_pm(x, y, (int)(ar * PR + aa * HR + 0.5f),
                        (int)(ag * PG + aa * HG + 0.5f),
                        (int)(ab * PB + aa * HB + 0.5f), al);
    }
  }
}

// ---------------------------------------------------------------------------
// The sailfin molly's sail, in one piece.
//
// Its foot is laid along the back as the back actually lies - interpolated
// along the bones, with the body's own wave - and it rises from there along
// the local "up", which also turns smoothly from column to column. So however
// the body bends the sail bends with it and never splits.
//
// It folds with `dorsal`: its height shrinks towards the back, and it lies
// back towards the tail as it goes down - a lowered fin lies along the body,
// it does not just get shorter.
//
// Drawn as one thin textured strip per sprite column between that column's
// foot and top, each strip a small parallelogram mapped back to the sprite.
// ---------------------------------------------------------------------------
static void IRAM_ATTR drawSail(const Rig& rig, const Fish& f, float sY, float sf,
                               int biasVal, float drift, float offX) {
  const SpeciesCfg* c = f.cfg;
  const int W = c->W, H = c->H;
  const RGBA8* spr = rig.spr;
  const int8_t* sail = MOLLY_DORSAL;
  const float mid = (H - 1) * 0.5f;
  const float fold = f.dorsal > 0.2f ? f.dorsal : 0.2f;
  const float lay = (1.0f - f.dorsal) * 1.3f;
  const int cid = xPortGetCoreID();

  // the body's wave, as drawSegment() has it
  const float kappa = (float)(M_PI * 2) / (2.6f * W);
  const float sweep = fsin(f.beat * 0.37f + f.phase) * 0.25f;
  const float spd = (0.10f + 0.60f * f.speedNorm + f.thrash * 1.4f) * sY;

  // a point on the back at arc s (sprite px behind the head), and its heading
  auto spineAt = [&](float s, float& x, float& y) {
    float t = s / c->spacing;
    if (t < 0) t = 0;
    int b = (int)t;
    if (b > BONES - 2) b = BONES - 2;
    const float u = t - b;
    x = f.bones[b].x + (f.bones[b + 1].x - f.bones[b].x) * u + offX;
    y = f.bones[b].y + (f.bones[b + 1].y - f.bones[b].y) * u;
  };

  const float lg = fishLight((int)(f.x + offX), (int)f.y);
  const float h = f.dry ? 0.0f : hazeFor(sf), k = 1.0f - h;
  const float am = f.alphaMul;
  const float PR = 31.0f / 255.0f * lg * k * am, PG = 63.0f / 255.0f * lg * k * am;
  const float PB = 31.0f / 255.0f * lg * k * am;
  const float HR = HAZE_R8 * 31.0f / 255.0f * lg * h * am;
  const float HG = HAZE_G8 * 63.0f / 255.0f * lg * h * am;
  const float HB = HAZE_B8 * 31.0f / 255.0f * lg * h * am;

  // One "up" for the whole sail, from the chord of the back under it. Taking
  // it column by column from the bones made the rays fan out every which way
  // as the body bent - the bones kink at the joints. The foot still follows
  // the back exactly; only the direction the rays stand in is shared.
  float tx, ty;
  {
    int cf = W - 1, cr = 0;                        // the sail's front and rear columns
    while (cf > 0 && sail[cf] < 0) cf--;
    while (cr < W - 1 && sail[cr] < 0) cr++;
    float px, py, qx, qy;
    spineAt((float)(W - 1 - cf), px, py);
    spineAt((float)(W - 1 - cr), qx, qy);
    tx = px - qx; ty = py - qy;
    float tl = sqrtf(tx * tx + ty * ty);
    if (tl < 1e-3f) { tx = f.mirror; ty = 0; tl = 1; }
    tx /= tl; ty /= tl;
  }
  float mir = (fabsf(tx) > 0.15f) ? (tx >= 0 ? 1.0f : -1.0f) : f.mirror;
  if (f.vflip) mir = -mir;
  // local "down" in the sprite is (-ty, tx) * mir on screen
  const float dnx = -ty * mir, dny = tx * mir;
  // While the body is coming round, the back under the sail is foreshortened
  // and its two ends do not yet agree which way the fish faces - and a sail
  // stood up on that sticks out of the side of the fish. A sail seen that
  // nearly end-on is a sliver anyway, so it is lowered until the back under
  // it is laid out broadside again.
  float showSail;
  {
    int cf = W - 1, cr = 0;
    while (cf > 0 && sail[cf] < 0) cf--;
    while (cr < W - 1 && sail[cr] < 0) cr++;
    float px, py, qx, qy;
    spineAt((float)(W - 1 - cf), px, py);
    spineAt((float)(W - 1 - cr), qx, qy);
    const float got = sqrtf((px - qx) * (px - qx) + (py - qy) * (py - qy));
    const float full = (float)(cf - cr) * sf * f.lenScale;
    showSail = clampf((got / (full > 0.1f ? full : 0.1f) - 0.45f) / 0.35f, 0.0f, 1.0f);
  }
  if (showSail <= 0.0f) return;

  // foot and top of the sail at sprite column cx
  struct Edge { float bx, by, tx, ty; float base; };
  auto edgeAt = [&](int cx, Edge& e) -> bool {
    const int ci = cx < 0 ? 0 : (cx > W - 1 ? W - 1 : cx);
    if (sail[ci] < 0) return false;
    e.base = sail[ci] * 0.25f;
    const float s = (float)(W - 1 - cx);
    float ox, oy;
    spineAt(s, ox, oy);
    ox += tx * drift; oy += ty * drift;
    // the body's wave at this column (the sail sits well forward of the fin
    // proper, so the fan terms do not apply)
    float t = s / c->spacing;
    int ti = (int)t; if (ti > BONES - 2) ti = BONES - 2;
    const float amp = c->A[ti] + (c->A[ti + 1] - c->A[ti]) * (t - ti);
    const float lat = amp * fsin(f.beat - kappa * s + sweep) * spd;
    const float footY = (e.base + biasVal - mid) * sY + lat;
    e.bx = ox + dnx * footY;
    e.by = oy + dny * footY;
    // up to the tip: the full height times the fold, swept back as it folds
    const float hgt = e.base * sY * showSail;
    e.tx = e.bx - dnx * hgt * fold - tx * hgt * lay;
    e.ty = e.by - dny * hgt * fold - ty * hgt * lay;
    return true;
  };

  for (int cx = 0; cx < W - 1; cx++) {
    Edge e0, e1;
    if (!edgeAt(cx, e0) || !edgeAt(cx + 1, e1)) continue;
    // the strip: origin at this column's foot, U to the next column's foot,
    // V up this column
    float ux = e1.bx - e0.bx, uy = e1.by - e0.by;
    const float vx = e0.tx - e0.bx, vy = e0.ty - e0.by;
    // Mid-turn the back runs into the depth of the tank and the columns crowd
    // together, down to nothing - and a strip of no width is not drawn, so the
    // sail blinked out. Keep each strip at least the width the body keeps
    // (drawSegment floors its foreshortening at 0.22), along the back.
    const float minU = 0.22f * sf * f.lenScale;
    if (ux * ux + uy * uy < minU * minU) { ux = tx * minU; uy = ty * minU; }
    const float det = ux * vy - uy * vx;
    if (fabsf(det) < 1e-4f) continue;
    const float id = 1.0f / det;
    float xmin = fminf(fminf(e0.bx, e1.bx), fminf(e0.tx, e1.tx));
    float xmax = fmaxf(fmaxf(e0.bx, e1.bx), fmaxf(e0.tx, e1.tx));
    float ymin = fminf(fminf(e0.by, e1.by), fminf(e0.ty, e1.ty));
    float ymax = fmaxf(fmaxf(e0.by, e1.by), fmaxf(e0.ty, e1.ty));
    int ix0 = (int)floorf(xmin) - 1, ix1 = (int)ceilf(xmax) + 1;
    int iy0 = (int)floorf(ymin) - 1, iy1 = (int)ceilf(ymax) + 1;
    if (ix0 < 0) ix0 = 0;
    if (ix1 > FB_W) ix1 = FB_W;
    if (iy0 < gClipY0[cid]) iy0 = gClipY0[cid];
    if (iy1 > gClipY1[cid]) iy1 = gClipY1[cid];
    for (int y = iy0; y < iy1; y++) {
      for (int x = ix0; x < ix1; x++) {
        float ar = 0, ag = 0, ab = 0, aa = 0;
        for (int ss = 0; ss < SEG_SAMPLES; ss++) {
          const float dx = x + SS_OX[ss] - e0.bx, dy = y + SS_OY[ss] - e0.by;
          const float a = (dx * vy - dy * vx) * id;   // across, 0..1
          const float b = (ux * dy - uy * dx) * id;   // up, 0..1
          // a hair of overlap between strips so no seam shows through
          if (a < -0.08f || a > 1.08f || b < 0.0f || b >= 1.0f) continue;
          const float base = e0.base + (e1.base - e0.base) * clampf(a, 0.0f, 1.0f);
          int col = cx + (a >= 0.5f ? 1 : 0);
          int row = (int)(base * (1.0f - b));       // b=0 foot, b=1 row 0
          if (row < 0 || row >= H || row * 4 + 2 >= sail[col]) continue;
          const RGBA8& sp = spr[row * W + col];
          if (!sp.a) continue;
          const float al = sp.a * (1.0f / 255.0f);
          ar += sp.r * al; ag += sp.g * al; ab += sp.b * al; aa += al;
        }
        if (aa <= 0.004f) continue;
        int al = (int)(aa * (32.0f / SEG_SAMPLES) * am + 0.5f);
        if (al <= 0) continue;
        if (al > 32) al = 32;
        px_blend_pm(x, y, (int)((ar * PR + aa * HR) / SEG_SAMPLES + 0.5f),
                          (int)((ag * PG + aa * HG) / SEG_SAMPLES + 0.5f),
                          (int)((ab * PB + aa * HB) / SEG_SAMPLES + 0.5f), al);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The card: somebody's drawing, swimming.
//
// Everything else in this file bends a sprite through a five-bone spine, which
// works because we know where the head, the peduncle and the fin are on every
// one of those sprites. We know nothing about this one. It could be a fish, a
// whale, a name, a face; whatever it is, articulating it would tear it apart
// and put the joints in the wrong places.
//
// So it is treated as what it physically is: a card in water. A card in water
// ripples - a wave runs down it from the leading edge, growing as it goes,
// and where the sheet turns edge-on to you it foreshortens. Both of those fall
// out of one number per column, which is cheap and, more to the point, is
// correct for any picture at all.
//
//   dy   a travelling wave, zero at the leading edge and growing as t^2, so
//        the picture is only ever distorted where a real sheet would be
//   sy    vertical foreshortening, strongest a quarter-wave off the crests,
//        which is where the surface is tilting away fastest
//   shear a whole-card lean, standing in for a rotation; over a card this size
//         the difference is not visible and a shear costs nothing
//
// Turning round is the same idea taken further. `Fish::turn` is a real angle
// about the card's own vertical axis - set in sim.cpp from the direction the
// fish is travelling, so it is never doing anything the fish is not - and
// cos(turn) is exactly how wide the card should be drawn, its sign being which
// face you are looking at. The card narrows, passes through edge-on, and opens
// out the other way showing its back. The near half is drawn slightly taller
// than the far half while that is happening, because without that gradient a
// rotation and a horizontal squash look identical.
//
// The axis is at the leading edge, not the middle. A page is hinged at its
// spine and sweeps its far edge through the air; a card spinning about its own
// centre is a revolving door, which is a different object entirely. Hinging it
// at the front also puts it where every other fish in the tank keeps its head,
// because `Fish::x` is the head position everywhere else in this build - so
// the card now hangs back from the point it is steering, the way they all do,
// and the nose stays put while the body comes round behind it.
//
// Sampling is bilinear on premultiplied alpha. Premultiplied matters: average
// the raw colour across the transparent border of a drawing and every soft
// edge picks up a halo of whatever was next to it.
// ---------------------------------------------------------------------------
static const float CARD_WAVE_AMP = 0.16f;   // of the card's height, at the tail
static const float CARD_WAVE_K   = 3.6f;    // wave crests across the card
static const float CARD_SQUASH   = 0.26f;   // how far it foreshortens edge-on
static const float CARD_PERSP    = 0.20f;   // near/far size split while turning

static void IRAM_ATTR drawCard(const Rig& rig, const Fish& f, float offX) {
  const int cid = xPortGetCoreID();
  const RGBA8* spr = rig.spr;
  if (!spr) return;

  // The card is laid along the body's bones, like every other fish: they sit
  // on the head's path, so when it comes about the card follows round the
  // same curve - foreshortened where that runs into the depth of the tank,
  // and seen from behind (mirrored) where it has already come round, the tail
  // last. Sampled head to tail in short slices; each slice fills the screen
  // columns between its two ends, and the tail end is drawn first so the
  // near half of a fold is on top.
  const float sf = f.sf;
  const float halfH = CARD_H * 0.5f * sf;
  const float lg = fishLight((int)(f.x + offX), (int)f.y);
  const float h = hazeFor(sf), k = 1.0f - h;
  const float PR = lg * k * (31.0f / 255.0f);
  const float PG = lg * k * (63.0f / 255.0f);
  const float PB = lg * k * (31.0f / 255.0f);
  const float HR = HAZE_R8 * lg * h * (31.0f / 255.0f);
  const float HG = HAZE_G8 * lg * h * (63.0f / 255.0f);
  const float HB = HAZE_B8 * lg * h * (31.0f / 255.0f);
  const float beat = f.beat;

  auto along = [&](float q, float& x, float& y) {   // q: 0 head .. 1 tail
    float t = q * (BONES - 1);
    int b = (int)t;
    if (b > BONES - 2) b = BONES - 2;
    const float u = t - b;
    x = f.bones[b].x + (f.bones[b + 1].x - f.bones[b].x) * u + offX;
    y = f.bones[b].y + (f.bones[b + 1].y - f.bones[b].y) * u;
  };

  const int SLICES = CARD_W * 2;
  for (int si = SLICES - 1; si >= 0; si--) {
    const float qa = (float)si / SLICES, qb = (float)(si + 1) / SLICES;
    float xa, ya, xb, yb;
    along(qa, xa, ya);
    along(qb, xb, yb);
    int ix0 = (int)floorf(fminf(xa, xb)), ix1 = (int)ceilf(fmaxf(xa, xb));
    if (ix1 <= ix0) ix1 = ix0 + 1;                // edge-on: still one column
    if (ix0 < 0) ix0 = 0;
    if (ix1 > FB_W) ix1 = FB_W;
    const float span = xb - xa;
    for (int x = ix0; x < ix1; x++) {
      // where along the card this column is, and the card column there
      float w = (fabsf(span) > 1e-3f) ? ((float)x + 0.5f - xa) / span : 0.5f;
      w = clampf(w, 0.0f, 1.0f);
      const float q = qa + (qb - qa) * w;
      const float cy = ya + (yb - ya) * w;
      const float uu = (1.0f - q) * (CARD_W - 1);   // art: head at +x
      int u0 = (int)uu;
      if (u0 > CARD_W - 1) u0 = CARD_W - 1;
      const float fu = uu - u0;
      const int u1 = (u0 + 1 < CARD_W) ? u0 + 1 : u0;

      // the ripple running down it, growing towards the tail
      const float ph = beat - q * CARD_WAVE_K;
      const float dy = CARD_WAVE_AMP * q * q * fsin(ph) * CARD_H * sf;
      const float sy = 1.0f - CARD_SQUASH * q * fabsf(fcos(ph));
      const float top = cy + dy - halfH * sy;
      const float colH = halfH * 2.0f * sy;
      if (colH < 0.5f) continue;
      const float invH = (float)CARD_H / colH;

      int iy0 = (int)floorf(top), iy1 = (int)ceilf(top + colH) + 1;
      if (iy0 < gClipY0[cid]) iy0 = gClipY0[cid];
      if (iy1 > gClipY1[cid]) iy1 = gClipY1[cid];

      for (int y = iy0; y < iy1; y++) {
        const float v = ((float)y + 0.5f - top) * invH - 0.5f;
        if (v <= -1.0f || v >= (float)CARD_H) continue;
        int v0 = (int)floorf(v);
        const float fv = v - v0;
        int v1 = v0 + 1;
        if (v0 < 0) v0 = 0;
        if (v1 > CARD_H - 1) v1 = CARD_H - 1;
        if (v0 > CARD_H - 1) v0 = CARD_H - 1;

        const RGBA8& p00 = spr[v0 * CARD_W + u0];
        const RGBA8& p10 = spr[v0 * CARD_W + u1];
        const RGBA8& p01 = spr[v1 * CARD_W + u0];
        const RGBA8& p11 = spr[v1 * CARD_W + u1];
        const float w00 = (1 - fu) * (1 - fv), w10 = fu * (1 - fv);
        const float w01 = (1 - fu) * fv,       w11 = fu * fv;
        // premultiplied, so the transparent border cannot bleed colour inwards
        const float a = p00.a * w00 + p10.a * w10 + p01.a * w01 + p11.a * w11;
        if (a < 4.0f) continue;
        const float r = p00.r * p00.a * w00 + p10.r * p10.a * w10
                      + p01.r * p01.a * w01 + p11.r * p11.a * w11;
        const float g = p00.g * p00.a * w00 + p10.g * p10.a * w10
                      + p01.g * p01.a * w01 + p11.g * p11.a * w11;
        const float b = p00.b * p00.a * w00 + p10.b * p10.a * w10
                      + p01.b * p01.a * w01 + p11.b * p11.a * w11;
        const float ia = 1.0f / 255.0f;
        const int al = (int)(a * (32.0f / 255.0f) + 0.5f);
        px_blend_pm(x, y,
                    (int)(r * ia * PR + a * ia * HR),
                    (int)(g * ia * PG + a * ia * HG),
                    (int)(b * ia * PB + a * ia * HB),
                    al > 32 ? 32 : al);
      }
    }
  }
}

// ---------------------------------------------------------------------------
static void drawFish(const Rig& rig, const Fish& f, float offX) {
  const int cid = xPortGetCoreID();
  const SpeciesCfg* c = f.cfg;
  const DepthEv& d = f.depth;

  float ch = fcos(f.heading);
  float pitchBody = clampf(fsin(f.heading), -0.9f, 0.9f) * 0.2f * fabsf(ch);
  float sgn = (ch >= 0) ? 1.0f : -1.0f;
  float pitch = d.bell * d.sign * c->pitchMax + pitchBody * -sgn;
  float sY = clampf(1 - 0.5f * fabsf(fsin(pitch)), 0.78f, 1.0f);
  // a clown loach lying on its side shows its back to the glass: a thin shape
  if (c->key == SP_LOACH) sY *= 1.0f - 0.55f * f.flat;
  sY *= f.hScale;
  float sf = f.sf;
  float facing = f.facing;                  // 1 side view, dips mid depth-event
  float drift = (1 - sf) * 8 * facing;      // strips lurch forward near the glass
  int biasVal = (int)lroundf(f.biasF);
  if (biasVal < -2) biasVal = -2;
  if (biasVal > 2) biasVal = 2;
  const float mir = f.mirror;      // -1 when the fish is heading left
  // fins, veil, glow and eye follow the same light and haze as the body
  const bool gagF = (c->key == SP_EBI);
  const float lgF = gagF ? 1.0f : fishLight((int)(f.x + offX), (int)f.y);
  const float hF = gagF ? 0.0f : hazeFor(f.sf);
  const float kF = 1.0f - hF;

  // --- guppy trailing caudal veil (behind the body) ------------------------
  int64_t _t0 = esp_timer_get_time();
  if (c->key == SP_GUPPY && !f.bodyOnly) {
    const float flare = f.flare;
    // the veil streams back from the last bone along the body's axis
    float px[5], py[5];
    const Bone& tb = f.bones[BONES - 1];
    const Bone& tb3 = f.bones[BONES - 2];
    const float tl = sqrtf((tb3.x - tb.x) * (tb3.x - tb.x) + (tb3.y - tb.y) * (tb3.y - tb.y));
    // The way the tail points - from a wider span of bones when the last
    // segment is foreshortened to nothing mid-turn, the same way the body
    // segments take theirs, so the veil does not flip ahead of the tail.
    float tdx = tb3.x - tb.x, tdy = tb3.y - tb.y;
    if (tl <= 0.25f * c->spacing * f.sf) {
      tdx = f.bones[BONES - 3].x - tb.x;
      tdy = f.bones[BONES - 3].y - tb.y;
    }
    const float tdl = sqrtf(tdx * tdx + tdy * tdy);
    const float tcx = (tdl > 1e-3f) ? -tdx / tdl : -fcos(tb.a);
    const float tcy = (tdl > 1e-3f) ? -tdy / tdl : -fsin(tb.a);
    const float tfore = clampf(tl / (c->spacing * f.sf), 0.22f, 1.0f);
    for (int k = 0; k <= 4; k++) {
      float back = (-1.5f + k * 1.5f) * tfore * f.sf;
      px[k] = tb.x + tcx * back + offX;
      py[k] = tb.y + tcy * back;
    }
    float xs[10], ys[10];
    float Lx[5], Ly[5], Rx[5], Ry[5];
    for (int k = 0; k <= 4; k++) {
      float qx = px[k < 4 ? k + 1 : 4], qy = py[k < 4 ? k + 1 : 4];
      float ang = atan2f(py[k] - qy, px[k] - qx);
      // the trailing edge lags the peduncle, so the veil ripples rather than
      // swinging as one piece
      float wave = fsin(f.beat * 1.15f - k * 1.25f) * (0.25f + k * 0.3f)
                 * (0.35f + flare);
      float spread = (k * 0.55f + 0.2f) * (1 + flare * 1.7f) * sY;
      float nx = fcos(ang + (float)M_PI_2), ny = fsin(ang + (float)M_PI_2);
      Lx[k] = px[k] + nx * (spread + wave * 0.85f);
      Ly[k] = py[k] + ny * (spread + wave * 0.85f);
      Rx[k] = px[k] - nx * (spread - wave * 0.85f);
      Ry[k] = py[k] - ny * (spread - wave * 0.85f);
    }
    for (int k = 0; k <= 4; k++) { xs[k] = Lx[k]; ys[k] = Ly[k]; }
    for (int k = 0; k <= 4; k++) { xs[5 + k] = Rx[4 - k]; ys[5 + k] = Ry[4 - k]; }

    PolyPaint vp;
    vp.grad = true;
    vp.gx0 = Lx[0]; vp.gy0 = Ly[0];
    vp.gx1 = Lx[4]; vp.gy1 = Ly[4];
    if (c->strain == 1) {
      vp.r0 = 235; vp.g0 = 210; vp.b0 = 90;  vp.a0 = 0.06f + flare * 0.30f;
      vp.r1 = 200; vp.g1 = 170; vp.b1 = 50;  vp.a1 = 0.03f;
    } else if (c->strain == 2) {
      vp.r0 = 250; vp.g0 = 95;  vp.b0 = 25;  vp.a0 = 0.06f + flare * 0.32f;
      vp.r1 = 210; vp.g1 = 50;  vp.b1 = 15;  vp.a1 = 0.03f;
    } else {
      vp.r0 = 250; vp.g0 = 105; vp.b0 = 60;  vp.a0 = 0.06f + flare * 0.32f;
      vp.r1 = 210; vp.g1 = 40;  vp.b1 = 40;  vp.a1 = 0.03f;
    }
    vp.r0 = (uint8_t)(vp.r0 * lgF * kF + HAZE_R8 * lgF * hF);
    vp.g0 = (uint8_t)(vp.g0 * lgF * kF + HAZE_G8 * lgF * hF);
    vp.b0 = (uint8_t)(vp.b0 * lgF * kF + HAZE_B8 * lgF * hF);
    vp.r1 = (uint8_t)(vp.r1 * lgF * kF + HAZE_R8 * lgF * hF);
    vp.g1 = (uint8_t)(vp.g1 * lgF * kF + HAZE_G8 * lgF * hF);
    vp.b1 = (uint8_t)(vp.b1 * lgF * kF + HAZE_B8 * lgF * hF);
    fillPolyAA(xs, ys, 10, vp);
  }

  // --- body strips ---------------------------------------------------------
  int64_t _t1 = esp_timer_get_time();
  tVeil[cid] += (uint32_t)(_t1 - _t0);
  // Far segments first. Coming about through the depth of the tank the body
  // lies round a U, and seen from the side its two ends overlap: drawn head to
  // tail regardless, the tail - on the far side of the U - landed on top of
  // the head and stuck out of it. (+z is towards the glass.)
  int segOrder[BONES - 1];
  float segZ[BONES - 1];
  for (int b = 0; b < BONES - 1; b++) {
    segOrder[b] = b;
    segZ[b] = f.bones[b].z + f.bones[b + 1].z;
  }
  for (int i = 1; i < BONES - 1; i++) {
    const int k = segOrder[i];
    int j = i - 1;
    while (j >= 0 && segZ[segOrder[j]] > segZ[k] + 0.01f) { segOrder[j + 1] = segOrder[j]; j--; }
    segOrder[j + 1] = k;
  }
  for (int i = 0; i < BONES - 1; i++)
    drawSegment(rig, f, segOrder[i], sY, sf, biasVal, drift, offX, mir);
  if (c->key == SP_MOLLY) drawSail(rig, f, sY, sf, biasVal, drift, offX);
  int64_t _t2 = esp_timer_get_time();
  tSeg[cid] += (uint32_t)(_t2 - _t1);

  // --- neon iridescent stripe glow -----------------------------------------
  if (c->key == SP_NEON && !f.bodyOnly) {
    const Bone& m = f.bones[1];
    float glow = 0.6f + 0.4f * fsin(f.beat * 0.5f + f.phase) + (1 - sf) * 0.8f;
    float ca = fcos(m.a), sa = fsin(m.a);
    float ox = m.x + offX, oy = m.y;
    const float W = c->W;
    const Bone& m2 = f.bones[2];
    const float ml = sqrtf((m.x - m2.x) * (m.x - m2.x) + (m.y - m2.y) * (m.y - m2.y));
    const float mfore = clampf(ml / (c->spacing * f.sf), 0.22f, 1.0f);
    const float lx0 = -W * 0.4f * mfore, lx1 = W * 0.45f * mfore;
    float lxs[4] = { lx0, lx1, lx1, lx0 };
    float lys[4] = { -0.7f, -0.7f, 0.7f, 0.7f };
    float xs[4], ys[4];
    for (int i = 0; i < 4; i++) {
      xs[i] = ox + lxs[i] * ca - lys[i] * sa;
      ys[i] = oy + lxs[i] * sa + lys[i] * ca;
    }
    PolyPaint gp;
    gp.grad = true;
    gp.additive = true;
    gp.gx0 = ox + lx0 * ca; gp.gy0 = oy + lx0 * sa;
    gp.gx1 = ox + lx1 * ca; gp.gy1 = oy + lx1 * sa;
    gp.r0 = (uint8_t)(90 * lgF * kF);  gp.g0 = (uint8_t)(240 * lgF * kF);
    gp.b0 = (uint8_t)(255 * lgF * kF); gp.a0 = 0.55f * glow;
    gp.r1 = (uint8_t)(60 * lgF * kF);  gp.g1 = (uint8_t)(200 * lgF * kF);
    gp.b1 = (uint8_t)(235 * lgF * kF); gp.a1 = 0.25f * glow;
    if (gp.a0 > 1) gp.a0 = 1;
    if (gp.a1 > 1) gp.a1 = 1;
    fillPolyAA(xs, ys, 4, gp);
  }

  if (f.bodyOnly) { tExtra[cid] += (uint32_t)(esp_timer_get_time() - _t2); return; }
  const Bone& head = f.bones[0];
  float hx = fcos(head.a), hy = fsin(head.a);

  if (c->key == SP_SHRIMP) {
    // --- antennae ----------------------------------------------------------
    // An Amano's antennae are as long as the animal and never still, and at
    // 28px they are most of what says "shrimp" rather than "small pale fish".
    // They are why the bake crops them off the sprite: as photographed they
    // sweep a long way forward of the rostrum, so leaving them in would have
    // spent a fifth of the sprite width on two hairs too thin to survive the
    // downsample. Drawn here they cost four line segments and read perfectly.
    //
    // They lie forward and a little apart while it is stopped and picking, and
    // stream out to the sides as it gets moving - and the sweep is a slow lash
    // that turns frantic when the abdomen flicks.
    const float bx = head.x + offX + hx * c->spacing * 0.55f * sf;
    const float by = head.y + hy * c->spacing * 0.55f * sf;
    const float L = c->W * 0.40f * sf;
    const uint16_t acol = hazed(176, 192, 180, lgF, hF);
    for (int side = 0; side < 2; side++) {
      const float sgn = side ? 1.0f : -1.0f;
      const float w = fsin(f.beat * 0.55f + f.phase + side * 2.1f)
                    * (0.15f + 0.55f * f.thrash);
      float a = head.a + sgn * (0.30f + 1.05f * f.speedNorm + w);
      float px = bx, py = by, al = 0.22f;
      for (int k = 0; k < 3; k++) {
        const float len = L * (0.42f - 0.09f * k);
        const float nx = px + fcos(a) * len, ny = py + fsin(a) * len;
        lineAA(px, py, nx, ny, 0.6f, acol, al);
        px = nx; py = ny;
        a += sgn * (0.24f + w * 0.45f);
        al *= 0.60f;
      }
    }
    // --- swimmerets --------------------------------------------------------
    // The pleopods under the abdomen are what a swimming shrimp actually moves
    // with, and they tick over even when it is standing still. One short
    // stroke under the middle of the body is enough at this size.
    const Bone& ab = f.bones[2];
    const float beat = fsin(f.beat * 3.1f + f.phase) * 0.5f;
    // under the belly as this part of the body lies, not by the overall facing
    float smir = mir;
    {
      const float dx = f.bones[1].x - f.bones[3].x;
      if (fabsf(dx) > 0.6f) smir = (dx >= 0) ? 1.0f : -1.0f;
    }
    const float pax = ab.a + smir * (1.9f + beat * 0.5f);
    const float pl = 2.1f * (0.6f + 0.9f * clampf(f.effort, 0.0f, 1.0f));
    lineAA(ab.x + offX, ab.y, ab.x + offX + fcos(pax) * pl, ab.y + fsin(pax) * pl,
           0.6f, hazed(196, 206, 194, lgF, hF), 0.18f);
  } else {
    // --- eye: a single dark pixel on the head side, plus a far-eye hint -----
    // (not for the hatchetfish, whose eye is well back from the snout and is
    // baked into its sprite - a dot here would sit on its lip)
    if (c->key != SP_HATCHET) {
    float ex = head.x + offX + hx * 1.0f * sf + mir * hy * 0.9f * sY;
    float ey = head.y + hy * 1.0f * sf - mir * hx * 0.9f * sY;
    fillRectAA(ex - 0.45f, ey - 0.45f, 0.9f, 0.9f,
             hazed(6, 14, 16, lgF, hF), 0.85f);
    if (facing < 0.75f) {
      float a2 = (0.75f - facing) * 1.2f;
      float ex2 = head.x + offX + hx * 1.1f * sf - mir * hy * 1.0f * sY;
      float ey2 = head.y + hy * 1.1f * sf + mir * hx * 1.0f * sY;
      fillRectAA(ex2 - 0.4f, ey2 - 0.4f, 0.8f, 0.8f, rgb565(6, 14, 16), a2 * 0.7f);
    }
    }

    // --- pectoral fins -----------------------------------------------------
    // a puffer hovers on its pectorals, and they never stop fluttering
    const bool puff = (c->key == SP_PUFFER);
    float flap = fsin(f.beat * (puff ? 6.0f : 2.1f) + f.phase)
               * (0.5f + d.bell + f.flare * 0.4f + (puff ? 0.4f : 0.0f));
    const Bone& pb = f.bones[1];
    // Which side is "down" here goes by which way this part of the body
    // points, not by the fish's overall facing: mid-turn the two disagree, and
    // going by the overall facing flipped the fin over to the back and back.
    float pmir = mir;
    {
      const float dx = f.bones[0].x - f.bones[2].x;
      if (fabsf(dx) > 0.6f) pmir = (dx >= 0) ? 1.0f : -1.0f;
    }
    float pa = pb.a + pmir * ((float)M_PI_2 + flap * 0.55f);
    float pl = (c->key == SP_NEON ? 2.2f : 3.2f) * (1 + d.bell * 0.4f);
    // Mid-turn the body is foreshortened, and a fin drawn at its full length
    // off a body that short stuck out of it like a spike; it shortens with the
    // body round it.
    {
      const float hl = sqrtf((f.bones[0].x - f.bones[2].x) * (f.bones[0].x - f.bones[2].x)
                           + (f.bones[0].y - f.bones[2].y) * (f.bones[0].y - f.bones[2].y));
      const float fore1 = clampf(hl / (2.0f * c->spacing * f.sf), 0.0f, 1.0f);
      pl *= 0.25f + 0.75f * fore1;
    }
    float bx = pb.x + offX + fcos(pb.a) * 1.5f;
    float by = pb.y + fsin(pb.a) * 1.5f;
    if (c->key == SP_NEON)
      lineAA(bx, by, bx + fcos(pa) * pl, by + fsin(pa) * pl, 0.7f,
             hazed(170, 235, 228, lgF, hF), 0.55f);
    else if (puff)
      lineAA(bx, by, bx + fcos(pa) * pl, by + fsin(pa) * pl, 0.7f,
             hazed(200, 222, 196, lgF, hF), 0.55f);
    else
      lineAA(bx, by, bx + fcos(pa) * pl, by + fsin(pa) * pl, 0.7f,
             hazed(235, 150, 110, lgF, hF), 0.60f);
    if (facing < 0.9f) {
      float aa = 0.3f * (1 - facing) + 0.15f;
      float fa = pb.a - pmir * ((float)M_PI_2 + flap * 0.55f);
      lineAA(bx, by, bx + fcos(fa) * pl * 0.9f, by + fsin(fa) * pl * 0.9f, 0.7f,
             hazed(200, 240, 235, lgF, hF), aa);
    }
  }
  tExtra[cid] += (uint32_t)(esp_timer_get_time() - _t2);
}

// ---------------------------------------------------------------------------
// The frame is composed and sent one horizontal band at a time. While a band is
// on the wire the next one is being drawn, so the 15ms it takes to shift a full
// 320x240 frame out at 80MHz costs almost nothing.
//
// (The unlit build could restore and push only the rectangles the fish touched.
// Moving light rewrites every pixel, so that path is gone; the pipelining below
// buys more than it did anyway.)

// how far outside its spine a fish can paint: veil overhang, fins, sprite depth
static inline int fishMargin(const SpeciesCfg* c) { return c->H + 14; }

// ---------------------------------------------------------------------------
// The surface, seen through the front glass from a little below it.
//
// Below the surface everything is seen through water, which makes it look
// about 1.33 times nearer and larger than it is; above it, through air, a
// thing is its own size. So the part of a fish that is out of the water -
// a hatchetfish breaking the surface, or in the air - is drawn a quarter
// smaller, pulled down towards the surface line, and with no water haze on
// it; the body steps in size where it crosses the line.
//
// And the underside of the surface is a mirror - total internal reflection -
// so a fish just under it has an upside-down twin just above it, fading as
// the fish goes deeper.
static const float AIR_SCALE = 0.75f;          // 1 / 1.33
static const float REFLECT_DEPTH = 16.0f;      // rows under the surface it still shows
static const float REFLECT_SQUASH = 0.6f;      // the mirror is seen at a grazing angle

// a fish is 2KB with its trail - too big to copy onto a render task's stack
static Fish gSurfaceCopy[2];

static void toAir(Fish& g, const Fish& f) {
  const float S = SURFACE_Y;
  // about the fish's own position, with a little of the pull towards the
  // middle of the view that a real change of magnification has
  const float ax = f.x, pull = (160.0f - f.x) * 0.05f;
  for (int b = 0; b < BONES; b++) {
    g.bones[b].x = ax + (f.bones[b].x - ax) * AIR_SCALE + pull;
    g.bones[b].y = S - (S - f.bones[b].y) * AIR_SCALE;
  }
  g.x = g.bones[0].x; g.y = g.bones[0].y;
  g.lenScale = AIR_SCALE;
  g.hScale = AIR_SCALE;
  g.dry = true;
}

static void toMirror(Fish& g, const Fish& f, float strength) {
  const float S = SURFACE_Y;
  for (int b = 0; b < BONES; b++) {
    g.bones[b].y = S - (f.bones[b].y - S) * REFLECT_SQUASH;
    const float a = f.bones[b].a;
    g.bones[b].a = atan2f(-fsin(a) * REFLECT_SQUASH, fcos(a));
  }
  g.heading = atan2f(-fsin(f.heading) * REFLECT_SQUASH, fcos(f.heading));
  g.y = g.bones[0].y;
  g.hScale = REFLECT_SQUASH;
  g.vflip = true;
  g.bodyOnly = true;
  g.alphaMul = strength;
}

void renderBand(const Sim& sim, int y0, int y1) {
  const int cid = xPortGetCoreID();
  clipBand(y0, y1);

  int64_t _r0 = esp_timer_get_time();
  lightApply(y0, y1);          // backdrop + flicker + caustics + shimmer
  bubblesDraw(y0, y1);         // the air line, behind everything that swims
  tRestore[cid] += (uint32_t)(esp_timer_get_time() - _r0);

  // far-to-near order
  int order[N_FISH];
  for (int i = 0; i < sim.n; i++) order[i] = i;
  for (int i = 1; i < sim.n; i++) {
    int k = order[i]; int j = i - 1;
    while (j >= 0 && sim.fish[order[j]].sf > sim.fish[k].sf) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }

  for (int i = 0; i < sim.n; i++) {
    const Fish& f = sim.fish[order[i]];
    // cheap band cull from the spine, so a fish is only drawn twice when it
    // genuinely straddles the seam
    float ymin = f.bones[0].y, ymax = ymin;
    for (int b = 1; b < BONES; b++) {
      if (f.bones[b].y < ymin) ymin = f.bones[b].y;
      if (f.bones[b].y > ymax) ymax = f.bones[b].y;
    }
    int m = fishMargin(f.cfg);
    if (ymax + m < y0 || ymin - m >= y1) continue;

    float offX = sim.sway * (0.55f + 0.45f * (f.sf - 0.74f) / 0.54f);
    if (f.cfg->key == SP_CARD) { drawCard(*rigFor(f.cfg), f, offX); continue; }
    const Rig& rig = *rigFor(f.cfg);
    const int S = (int)SURFACE_Y;
    if (ymin - m >= SURFACE_Y + REFLECT_DEPTH || y0 >= S + (int)REFLECT_DEPTH + m) {
      drawFish(rig, f, offX);                  // nowhere near the surface
      continue;
    }
    // its reflection in the underside of the surface, just above it
    const float depth = ymin - SURFACE_Y;      // of its highest point
    if (!f.air && depth > -2.0f && depth < REFLECT_DEPTH && y0 < S) {
      clipBand(y0, y1 < S ? y1 : S);
      Fish& g = gSurfaceCopy[cid];
      g = f;
      toMirror(g, f, 0.38f * (1.0f - fmaxf(depth, 0.0f) / REFLECT_DEPTH));
      drawFish(rig, g, offX);
    }
    // under the surface, as it is
    if (y1 > S) {
      clipBand(y0 > S ? y0 : S, y1);
      drawFish(rig, f, offX);
    }
    // above it: the part that is out, seen through air
    if (y0 < S) {
      clipBand(y0, y1 < S ? y1 : S);
      if (ymin < SURFACE_Y) {
        Fish& g = gSurfaceCopy[cid];
        g = f;
        toAir(g, f);
        drawFish(rig, g, offX);
      }
    }
    clipBand(y0, y1);
  }

  // --- splashes: spray, bubbles and a ripple where a hatchetfish broke the
  // surface. Only ever a few rows either side of the surface, so most bands
  // skip the lot.
  if (sim.nSplash && y0 < (int)SURFACE_Y + 40 && y1 > 0) {
    for (int i = 0; i < sim.nSplash; i++) {
      const Splash& p = sim.splash[i];
      const float fade = p.life / p.max;
      if (p.kind == SPL_DROP) {
        fillRectAA(p.x - p.size * 0.5f, p.y - p.size * 0.5f, p.size, p.size,
                   rgb565(225, 240, 245), 0.85f * fade + 0.1f);
      } else if (p.kind == SPL_BUBBLE) {
        const float s = p.size;
        // a pale rim and a brighter glint, which is all a bubble is at this size
        fillRectAA(p.x - s * 0.5f, p.y - s * 0.5f, s, s, rgb565(190, 225, 232), 0.40f * fade + 0.1f);
        fillRectAA(p.x - s * 0.3f, p.y - s * 0.3f, 0.6f, 0.6f, rgb565(245, 252, 255), 0.6f * fade);
      } else {
        // the ring seen edge-on: a bright line along the surface, spreading and fading
        const float age = 1.0f - fade;
        const float r = (3.0f + age * 26.0f) * p.size;
        lineAA(p.x - r, p.y, p.x - r * 0.35f, p.y, 0.8f, rgb565(215, 238, 242), 0.55f * fade);
        lineAA(p.x + r * 0.35f, p.y, p.x + r, p.y, 0.8f, rgb565(215, 238, 242), 0.55f * fade);
      }
    }
  }

  clipBand(0, FB_H);
}
