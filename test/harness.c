/* Przepuszcza scenę przez wiele minut symulacji i sprawdza inwarianty. */
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
		double t = (double)i * dt;
		float fade = t < 1.5 ? (float)(t / 1.5) : 1.0f;
		/* jak przy trzech monitorach: różne rozmiary na przemian */
		scene_draw(1920, 1200, t, fade);
		scene_draw(2560, 1440, t, fade);
	}
	printf("%d frames (%d min) without failure\n", frames, minutes);
	return 0;
}
