#pragma once
#include <stdint.h>
#include "../config.h"

// THE CANVAS -> PANEL PIXEL MAPPING, IN A HEADER SO IT CAN BE PROVEN.
//
// Same reasoning as shift_tour.h, and the same lesson learned twice. The blit
// lives in display.cpp, which cannot be compiled on the host, so for months
// nothing in the build could see that it applied the burn-in shift in CANVAS
// space and rotated afterwards -- which negates the shift on the two flipped
// panels, sliding the top row down while the bottom row slid up, by up to
// 2*amp px. The prover proves the canvas; the panel is where it stopped being
// checked, and that is exactly where both shift bugs lived.
//
// So the mapping is here, display.cpp calls it, and layoutcheck asserts the
// property that matters: THE SAME SHIFT MOVES EVERY PANEL THE SAME PHYSICAL
// DIRECTION, whichever way it is mounted.

// Map a canvas pixel to a panel pixel. Returns false if the shift pushes it
// off the glass (clipped, per the safe-area contract).
// bias_y is the slot's fixed, measured vertical correction (slot_bias.h). It
// rides with the shift for exactly the same reason: both are displacements in
// the VIEWER's frame, and applying either one after the rotation would let the
// mounting negate it on the two flipped panels.
static inline bool blit_map(int16_t x, int16_t y, bool flip,
                            int16_t sh_x, int16_t sh_y, int16_t bias_y,
                            int16_t *px, int16_t *py) {
  // SHIFT FIRST, THEN ROTATE. This is the original order and it is the correct
  // one. I changed it to rotate-then-shift on 2026-08-11 on the reasoning that
  // "the shift should move every panel the same way", and that was wrong,
  // because it named the wrong space.
  //
  //   CANVAS space is the VIEWER's space -- upright, the way the clock is read.
  //   PANEL space is the driver's GDDRAM.
  //   The `flip` rotation exists to CANCEL the mounting, mapping viewer-upright
  //   onto a module that is physically installed upside down.
  //
  // So a shift added in canvas space is already uniform in the only frame that
  // matters: for an upright panel the viewer sees +s, and for a flipped one the
  // mounting's own 180 turn undoes the blit's, so the viewer sees +s there too.
  // Adding it in panel space instead makes the mounting negate it, and the top
  // row then slides down while the bottom row slides up -- which is what the
  // user saw the moment it shipped: "hour and minute way too low now".
  //
  // The selftest below checks this in VIEWER space, not panel space. Checking
  // it in panel space is exactly the mistake this comment exists to prevent:
  // it "passes" for the broken version and fails for the correct one.
  int16_t ox = (int16_t)(x + sh_x), oy = (int16_t)(y + sh_y + bias_y);
  if (ox < 0 || ox >= SCR_W || oy < 0 || oy >= SCR_H) return false;
  if (flip) { ox = (int16_t)(SCR_W - 1 - ox); oy = (int16_t)(SCR_H - 1 - oy); }
  *px = ox; *py = oy;
  return true;
}
