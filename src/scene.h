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
 *   t    — absolute time in seconds since activation
 *   fade — brightness multiplier 0..1 (smooth fade-in)
 */
void scene_draw(int width, int height, double t, float fade);

/* Per-output frame cap from the config; 0 = uncapped. */
int scene_fps_cap(void);

#endif
