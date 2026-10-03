#ifndef NEBULIGHTS_SCENE_H
#define NEBULIGHTS_SCENE_H

/* Initialise GL resources. Needs a current context. */
void scene_init(void);

/* Release GL resources. */
void scene_fini(void);

/* Reset the simulation clock (when the screensaver activates again). */
void scene_reset_clock(void);

/*
 * Simulation step + draw a frame.
 *   view — the part of the shared view this output shows: left, right,
 *          bottom, top edge, in units of half the main screen's height,
 *          relative to the view centre (y up). NULL = the whole view.
 *   all  — the same for the bounding box of all outputs, so objects
 *          never appear on any of them. NULL = same as view.
 *   t    — absolute time in seconds since activation
 *   fade — brightness multiplier 0..1 (smooth fade-in)
 */
void scene_draw(int width, int height, const float *view, const float *all,
                double t, float fade);

/* Per-output frame cap from the config; 0 = uncapped. */
int scene_fps_cap(void);

#endif
