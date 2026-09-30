#ifndef PIXAQ_CARDFISH_H
#define PIXAQ_CARDFISH_H
// ---------------------------------------------------------------------------
// cardfish.h — the fish somebody else drew.
//
// Aquariums used to run this: you drew a fish at a table by the entrance, it
// was scanned, and a minute later it was swimming in a projection on the wall
// with everyone else's. This is that, on an SD card. Put a transparent PNG
// called fish.png on the card and one to three copies of it join the tank at
// the next boot; take the card out and the tank is just a tank again.
//
// Nothing here knows or cares what the picture is, which is the whole point.
// It is not baked, not colour-graded and not fitted to a skeleton - it is
// carried through as a single flat card, and it swims by rippling like a sheet
// of paper in water rather than by bending like a fish. A drawing that flexed
// through a five-bone spine would tear itself apart, and anything clever about
// where the head or the tail is would be wrong for half the pictures people
// put on the card.
//
// Wiring: the SD module is on the pins in cardfish.cpp, on the SPI bus the
// panel is not using.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include "fish_art.h"

// The sprite the card was reduced to, or nullptr if there is no usable card.
// Straight (not premultiplied) RGBA, the same format the baked sprites use.
extern const RGBA8* CARD_ART;
static const int CARD_W = 32;
static const int CARD_H = 32;

// Bring up the SD card, read /fish.png, and reduce it to CARD_W x CARD_H.
// Returns false for every ordinary reason a card is not there - no module, no
// card, no file, not a PNG, too big to decode - and says which on the serial
// console. Call it before buildRigs().
bool cardLoad();

// Read /bg.jpg off the card, if there is one, into dst (dw x dh RGB565),
// filling the screen and cropping what overhangs. False, and dst untouched,
// for no card, no file or a JPEG that will not decode.
bool cardLoadBackdrop(uint16_t* dst, int dw, int dh);
bool bgFromJpeg(const uint8_t* jpg, size_t len, uint16_t* dst, int dw, int dh);

#endif // PIXAQ_CARDFISH_H
