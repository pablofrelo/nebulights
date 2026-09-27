#ifndef NEBULIGHTS_SCENE_H
#define NEBULIGHTS_SCENE_H

/* Inicjalizacja zasobów GL. Wymaga aktywnego kontekstu. */
void scene_init(void);

/* Zwolnienie zasobów GL. */
void scene_fini(void);

/* Reset zegara symulacji (po ponownej aktywacji wygaszacza). */
void scene_reset_clock(void);

/*
 * Krok symulacji + narysowanie klatki.
 *   t    — czas bezwzględny w sekundach od aktywacji
 *   fade — mnożnik jasności 0..1 (płynne wejście)
 */
void scene_draw(int width, int height, double t, float fade);

/* Limit klatek na wyjście z konfiguracji; 0 = bez limitu. */
int scene_fps_cap(void);

#endif
