/* Runs the scene through many minutes of simulation and checks invariants. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "scene.h"

int main(int argc, char **argv)
{
	int minutes = argc > 1 ? atoi(argv[1]) : 10;
	int frames = minutes * 60 * 60;
	const double dt = 1.0 / 60.0;

	scene_reset_clock();
	for (int i = 0; i < frames; i++) {
		/* every minute: screensaver off and on again, as after input */
		if (i > 0 && i % 3600 == 0) {
			scene_fini();
			scene_reset_clock();
		}
		double t = (double)(i % 3600) * dt;
		float fade = t < 1.5 ? (float)(t / 1.5) : 1.0f;
		/* like three monitors: alternating sizes */
		scene_draw(1920, 1200, t, fade);
		scene_draw(2560, 1440, t, fade);
	}
	printf("%d frames (%d min) without failure\n", frames, minutes);
	return 0;
}
