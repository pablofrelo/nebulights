/*
 * nebulights — the scene: glowing ribbons orbiting the viewer.
 *
 * The viewer sits at the centre of mass and only turns their gaze — turning
 * does not move the attractor, so orbits stay clean ellipses.
 *
 * GL side: GLES2 + half-float buffer (when available), two-level glow,
 * everything blended additively.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <GLES2/gl2.h>

#include "scene.h"

#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif

/* ================= parameters ==================================== */

#define TAU 6.283185307179586f

#define MAX_FLYERS      190
#define MAX_SAMPLES     900
#define SAMPLE_STEP     0.07f   /* default value */
#define MAX_SPARK    110000
#define N_BG_MAX       9000
#define N_NEB_MAX      2400
#define MAX_GROUPS       32

#define HOLE_GM       420.0f
#define SOFT2          (1.6f * 1.6f)
#define R_PERI_MIN      9.5f
#define R_APO_MAX      46.0f

/* ---- settings read from the config file ------------------------
   Defaults; overridden by ~/.config/nebulights.conf */
static float TRAIL_LIFE = 2.2f;
static float MIST_MUL   = 1.0f;
static float HUE_MUL    = 1.0f;
static float NEB_MUL    = 1.0f;
static float BLOOM_MUL  = 1.0f;
static float FRINGE     = 0.010f;
static float SAMPLE_STEP_CFG = SAMPLE_STEP;

static int   N_BG       = N_BG_MAX;
static int   N_NEB      = 2400;   /* see the note on fill cost in README */
static int   FLYER_SCALE_PCT = 100;   /* cast size in percent */
static int   BLOOM_LEVELS = 2;        /* 0 = no glow, 1 = tight, 2 = both */
static int   ALLOW_HDR = 1;
static float NEB_SIZE = 1.8f;   /* multiplier for nebula puff radius */
static int   FPS_CAP = 0;       /* 0 = uncapped */
static int   PALETTE = 0;       /* 0 = rainbow, 1 = gruvbox, 2 = nostromo */
/* ---- config file -----------------------------------------------
   ~/.config/nebulights.conf, format "key value", # starts a comment.
   A missing file is not an error — defaults simply stay. */

static float DENSITY = 1.0f;
static float EXPOSURE_CFG = -1.0f;   /* <0 = pick automatically */

struct cfg_entry {
	const char *key;
	float      *fval;
	int        *ival;
	float       lo, hi;
};

static void config_apply(const char *key, const char *val)
{
	const struct cfg_entry tab[] = {
		{ "trail_life",   &TRAIL_LIFE,      NULL,              0.2f,  8.0f },
		{ "sample_step",  &SAMPLE_STEP_CFG, NULL,              0.02f, 0.6f },
		{ "mist",         &MIST_MUL,        NULL,              0.0f,  4.0f },
		{ "hue_rate",     &HUE_MUL,         NULL,              0.0f,  5.0f },
		{ "nebula",       &NEB_MUL,         NULL,              0.0f,  4.0f },
		{ "nebula_size",  &NEB_SIZE,        NULL,              0.2f,  3.0f },
		{ "fps_cap",      NULL, &FPS_CAP,                       0,   240 },
		{ "bloom",        &BLOOM_MUL,       NULL,              0.0f,  3.0f },
		{ "fringe",       &FRINGE,          NULL,              0.0f,  0.05f },
		{ "exposure",     &EXPOSURE_CFG,    NULL,              0.05f, 4.0f },
		{ "density",      &DENSITY,         NULL,              0.05f, 1.0f },
		{ "objects",      NULL, &FLYER_SCALE_PCT,               5,   100 },
		{ "stars",        NULL, &N_BG,                          0,   N_BG_MAX },
		{ "nebula_puffs", NULL, &N_NEB,                         0,   N_NEB_MAX },
		{ "bloom_levels", NULL, &BLOOM_LEVELS,                  0,     2 },
		{ "half_float",   NULL, &ALLOW_HDR,                     0,     1 },
		{ "palette",      NULL, &PALETTE,                       0,     2 },
	};

	for (size_t i = 0; i < sizeof(tab)/sizeof(tab[0]); i++) {
		if (strcmp(key, tab[i].key) != 0)
			continue;

		double v = atof(val);
		if (v < tab[i].lo) v = tab[i].lo;
		if (v > tab[i].hi) v = tab[i].hi;

		if (tab[i].fval) *tab[i].fval = (float)v;
		else             *tab[i].ival = (int)v;
		return;
	}
	fprintf(stderr, "nebulights: unknown config key: %s\n", key);
}

static void config_load(void)
{
	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");

	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/nebulights.conf", xdg);
	else if (home && *home)
		snprintf(path, sizeof(path), "%s/.config/nebulights.conf", home);
	else
		return;

	FILE *f = fopen(path, "r");
	if (!f)
		return;

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;

		char key[64], val[64];
		/* accept both "key value" and "key = value" */
		if (sscanf(p, "%63s = %63s", key, val) == 2 ||
		    sscanf(p, "%63s %63s", key, val) == 2)
			config_apply(key, val);
	}
	fclose(f);
	fprintf(stderr, "nebulights: loaded %s\n", path);
}

/* trail kinds */
enum ribbon { R_THREAD, R_SPARK, R_VAPOR, R_BEAD, R_TUBE, R_COUNT };

/* ================= small maths ================================== */

static uint32_t rng_state = 0x9E3779B9u;

static float frnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (float)(rng_state & 0xFFFFFF) / (float)0x1000000;
}
static float frnd2(void) { return frnd() * 2.0f - 1.0f; }
static float gauss(void) { return (frnd2() + frnd2() + frnd2()) * 0.577f; }
static int   irnd(int n)  { return (int)(frnd() * (float)n) % (n > 0 ? n : 1); }

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void vnorm(float *v)
{
	float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
	if (l > 1e-6f) { v[0]/=l; v[1]/=l; v[2]/=l; }
}

/* two vectors perpendicular to a given direction */
static void perp_basis(const float *d, float *u, float *v)
{
	float ax[3] = { 0.0f, 1.0f, 0.0f };
	if (fabsf(d[0]) < 0.8f) { ax[0] = 1.0f; ax[1] = 0.0f; }

	u[0] = d[1]*ax[2] - d[2]*ax[1];
	u[1] = d[2]*ax[0] - d[0]*ax[2];
	u[2] = d[0]*ax[1] - d[1]*ax[0];
	vnorm(u);

	v[0] = d[1]*u[2] - d[2]*u[1];
	v[1] = d[2]*u[0] - d[0]*u[2];
	v[2] = d[0]*u[1] - d[1]*u[0];
}

static const float PAL_GRUV[5][3] = {
        {0.98f, 0.74f, 0.18f},  /* yellow   #fabd2f */
        {1.00f, 0.50f, 0.10f},  /* orange   #fe8019 */
        {0.98f, 0.29f, 0.20f},  /* red      #fb4934 */
        {0.72f, 0.73f, 0.15f},  /* green    #b8bb26 */
        {0.56f, 0.75f, 0.49f},  /* aqua     #8ec07c */
};
static const float PAL_NOST[5][3] = {
        {0.20f, 1.00f, 0.45f},  /* phosphor green */
        {0.55f, 1.00f, 0.75f},  /* pale monitor green */
        {1.00f, 0.69f, 0.00f},  /* amber */
        {1.00f, 0.45f, 0.05f},  /* alarm orange */
        {0.10f, 0.60f, 0.25f},  /* dark green */
};

/* full saturation, drifting hue — hence the psychedelia */
static void hsv(float h, float s, float v, float *out)
{
	if (PALETTE > 0) {
		const float (*pal)[3] = PALETTE == 1 ? PAL_GRUV : PAL_NOST;
		h = h - floorf(h);
		float x = h * 5.0f;
		int pi = (int)x % 5;
		float fi = x - floorf(x);
		for (int k = 0; k < 3; k++) {
			float c = pal[pi][k] + (pal[(pi + 1) % 5][k] - pal[pi][k]) * fi;
			out[k] = v * (1.0f - s + s * c);
		}
		return;
	}
	h = h - floorf(h);
	int i = (int)(h * 6.0f);
	float f = h * 6.0f - (float)i;
	float p = v * (1.0f - s);
	float q = v * (1.0f - s * f);
	float t = v * (1.0f - s * (1.0f - f));

	switch (i % 6) {
	case 0: out[0]=v; out[1]=t; out[2]=p; break;
	case 1: out[0]=q; out[1]=v; out[2]=p; break;
	case 2: out[0]=p; out[1]=v; out[2]=t; break;
	case 3: out[0]=p; out[1]=q; out[2]=v; break;
	case 4: out[0]=t; out[1]=p; out[2]=v; break;
	default:out[0]=v; out[1]=p; out[2]=q; break;
	}
}
/* ---- 4x4 matrices, column-major as in OpenGL ------------------- */

/* Off-centre perspective; edges given as tangents (l, r, b, t). With
   several monitors each one gets its own slice of one shared frustum. */
static void m_frustum(float *m, const float *w, float zn, float zf)
{
	memset(m, 0, 16 * sizeof(float));
	m[0]  = 2.0f / (w[1] - w[0]);
	m[5]  = 2.0f / (w[3] - w[2]);
	m[8]  = (w[1] + w[0]) / (w[1] - w[0]);
	m[9]  = (w[3] + w[2]) / (w[3] - w[2]);
	m[10] = (zf + zn) / (zn - zf);
	m[11] = -1.0f;
	m[14] = 2.0f * zf * zn / (zn - zf);
}

static void m_lookat(float *m, const float *e, const float *c, const float *up)
{
	float f[3] = { c[0]-e[0], c[1]-e[1], c[2]-e[2] };
	vnorm(f);

	float s[3] = { f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2],
	               f[0]*up[1]-f[1]*up[0] };
	vnorm(s);

	float u[3] = { s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2],
	               s[0]*f[1]-s[1]*f[0] };

	memset(m, 0, 16 * sizeof(float));
	m[15] = 1.0f;
	m[0]=s[0]; m[4]=s[1]; m[8] =s[2];
	m[1]=u[0]; m[5]=u[1]; m[9] =u[2];
	m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];
	m[12] = -(s[0]*e[0] + s[1]*e[1] + s[2]*e[2]);
	m[13] = -(u[0]*e[0] + u[1]*e[1] + u[2]*e[2]);
	m[14] =  (f[0]*e[0] + f[1]*e[1] + f[2]*e[2]);
}

static void m_mul(const float *a, const float *b, float *o)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int i = 0; i < 4; i++)
			r[c*4+i] = a[i]*b[c*4] + a[4+i]*b[c*4+1]
			         + a[8+i]*b[c*4+2] + a[12+i]*b[c*4+3];
	memcpy(o, r, sizeof(r));
}

/* ================= sparks, steam, mist =========================== */
/*
 * One array for everything that is a point. The `grow` field splits two
 * behaviours: a spark shrinks and fades fast, a puff of steam swells
 * and fades slowly.
 */
struct sparks {
	float x[MAX_SPARK],  y[MAX_SPARK],  z[MAX_SPARK];
	float vx[MAX_SPARK], vy[MAX_SPARK], vz[MAX_SPARK];
	float r[MAX_SPARK],  g[MAX_SPARK],  b[MAX_SPARK];
	float size[MAX_SPARK];
	float life[MAX_SPARK], life0[MAX_SPARK];
	float grow[MAX_SPARK];
	int   n;
};
static struct sparks *SP;

static void spark_add(float x, float y, float z,
                      float vx, float vy, float vz,
                      const float *col, float life, float size, float grow)
{
	if (SP->n >= MAX_SPARK)
		return;
	int i = SP->n++;
	SP->x[i]=x; SP->y[i]=y; SP->z[i]=z;
	SP->vx[i]=vx; SP->vy[i]=vy; SP->vz[i]=vz;
	SP->r[i]=col[0]; SP->g[i]=col[1]; SP->b[i]=col[2];
	SP->size[i]=size; SP->life[i]=life; SP->life0[i]=life;
	SP->grow[i]=grow;
}

static void spark_step(float dt)
{
	int w = 0;
	for (int i = 0; i < SP->n; i++) {
		SP->life[i] -= dt;
		if (SP->life[i] <= 0.0f)
			continue;

		SP->x[i] += SP->vx[i]*dt;
		SP->y[i] += SP->vy[i]*dt;
		SP->z[i] += SP->vz[i]*dt;

		/* steam slows down noticeably faster than a spark */
		float drag = SP->grow[i] > 0.0f ? 1.0f - 1.9f*dt : 1.0f - 1.4f*dt;
		SP->vx[i]*=drag; SP->vy[i]*=drag; SP->vz[i]*=drag;

		if (w != i) {
			SP->x[w]=SP->x[i]; SP->y[w]=SP->y[i]; SP->z[w]=SP->z[i];
			SP->vx[w]=SP->vx[i]; SP->vy[w]=SP->vy[i]; SP->vz[w]=SP->vz[i];
			SP->r[w]=SP->r[i]; SP->g[w]=SP->g[i]; SP->b[w]=SP->b[i];
			SP->size[w]=SP->size[i];
			SP->life[w]=SP->life[i]; SP->life0[w]=SP->life0[i];
			SP->grow[w]=SP->grow[i];
		}
		w++;
	}
	SP->n = w;
}

static void mist_add(float x, float y, float z, const float *col, float scale)
{
	spark_add(x + frnd2()*0.3f, y + frnd2()*0.3f, z + frnd2()*0.3f,
	          frnd2()*0.3f, frnd2()*0.3f, frnd2()*0.3f,
	          col, (0.9f + frnd()*1.1f) * scale,
	          5.0f + frnd()*7.0f, 2.4f + frnd()*2.2f);
}

static void explode(float x, float y, float z, const float *col)
{
	static const float white[3] = { 1.0f, 1.0f, 1.0f };

	spark_add(x,y,z, 0,0,0, white, 0.55f, 42.0f, 0.0f);
	spark_add(x,y,z, 0,0,0, col,   0.90f, 26.0f, 0.0f);

	int n = 110 + irnd(90);
	for (int i = 0; i < n; i++) {
		float d[3] = { frnd2(), frnd2(), frnd2() };
		vnorm(d);
		float s = 1.6f + frnd()*5.5f;
		spark_add(x,y,z, d[0]*s, d[1]*s, d[2]*s,
		          frnd() < 0.35f ? white : col,
		          0.7f + frnd()*1.8f, 1.6f + frnd()*3.2f, 0.0f);
	}
	for (int i = 0; i < 26; i++)
		mist_add(x, y, z, col, 1.6f);
}

static void micro_burst(float x, float y, float z, const float *col)
{
	static const float white[3] = { 1.0f, 1.0f, 1.0f };
	spark_add(x,y,z, 0,0,0, white, 0.22f, 14.0f, 0.0f);
	for (int i = 0; i < 16; i++) {
		float d[3] = { frnd2(), frnd2(), frnd2() };
		vnorm(d);
		float s = 0.8f + frnd()*2.2f;
		spark_add(x,y,z, d[0]*s, d[1]*s, d[2]*s, col,
		          0.3f + frnd()*0.6f, 1.2f + frnd()*2.0f, 0.0f);
	}
}

/*
 * An explosion in the background. The farther, the bigger and slower —
 * otherwise it would read as something about to fall on us.
 */
static void far_burst(float x, float y, float z, float hue)
{
	float c[3];
	hsv(hue, 0.85f, 1.0f, c);

	float r = sqrtf(x*x + y*y + z*z);
	if (r < 1e-3f) r = 1e-3f;
	float scale = r / 70.0f;

	float hot[3]  = { 1.6f, 1.5f, 1.3f };
	float tint[3] = { c[0]*1.3f, c[1]*1.3f, c[2]*1.3f };
	spark_add(x,y,z, 0,0,0, hot,  0.9f, 60.0f*scale, 0.0f);
	spark_add(x,y,z, 0,0,0, tint, 1.7f, 40.0f*scale, 0.0f);

	static const float ash[3] = { 0.9f, 0.85f, 0.75f };
	int n = 180 + irnd(120);
	for (int i = 0; i < n; i++) {
		float d[3] = { frnd2(), frnd2(), frnd2() };
		vnorm(d);
		float s = (5.0f + frnd()*11.0f) * scale;
		spark_add(x,y,z, d[0]*s, d[1]*s, d[2]*s,
		          frnd() < 0.3f ? ash : c,
		          2.6f + frnd()*3.6f, (5.0f + frnd()*10.0f)*scale, 0.0f);
	}

	float dim[3] = { c[0]*0.5f, c[1]*0.5f, c[2]*0.5f };
	for (int i = 0; i < 34; i++)
		spark_add(x + gauss()*3.0f*scale,
		          y + gauss()*3.0f*scale,
		          z + gauss()*3.0f*scale,
		          gauss()*0.5f, gauss()*0.5f, gauss()*0.5f,
		          dim, 3.5f + frnd()*4.0f,
		          (26.0f + frnd()*34.0f)*scale, 2.2f + frnd()*2.0f);
}

/* ================= background stars and nebulae ================== */

static float *BG;          /* N_BG * 7: xyz rgb size */

static void seed_bg(void)
{
	for (int i = 0; i < N_BG; i++) {
		float d[3] = { frnd2(), frnd2(), frnd2() };
		vnorm(d);
		float r = 110.0f + frnd()*40.0f;
		float t = frnd();
		float b = 0.16f + t*t*t*0.85f;
		bool warm = frnd() < 0.25f;

		BG[i*7+0] = d[0]*r;
		BG[i*7+1] = d[1]*r;
		BG[i*7+2] = d[2]*r;
		BG[i*7+3] = b * (warm ? 1.00f : 0.82f);
		BG[i*7+4] = b * (warm ? 0.86f : 0.88f);
		BG[i*7+5] = b * (warm ? 0.70f : 1.00f);
		BG[i*7+6] = 0.9f + frnd()*1.9f;
	}
}

/*
 * Nebulae: huge, very dark clouds far behind the action, laid out in
 * a few clusters so they have a shape instead of an even mush.
 * Drawn as camera-facing quads — points were dropped because mobile
 * GPUs clamp gl_PointSize to 64-128 px and the clouds vanished.
 */
static float *NEB_POS;     /* N_NEB * 3 */
static float *NEB_COL;     /* N_NEB * 3 */
static float *NEB_R;       /* N_NEB     */
static float *NEB_PH;      /* N_NEB     */
static float *NEB_RATE;    /* N_NEB     */

static void seed_nebulae(void)
{
	struct { float p[3], hue, spread; } cl[9];
	int nclump = 5 + irnd(4);

	for (int c = 0; c < nclump; c++) {
		float d[3] = { frnd2(), frnd2(), frnd2() };
		vnorm(d);
		float r = 62.0f + frnd()*46.0f;
		cl[c].p[0] = d[0]*r;
		cl[c].p[1] = d[1]*r;
		cl[c].p[2] = d[2]*r;
		cl[c].hue = frnd();
		cl[c].spread = 16.0f + frnd()*26.0f;
	}

	for (int i = 0; i < N_NEB; i++) {
		int c = irnd(nclump);
		NEB_POS[i*3+0] = cl[c].p[0] + gauss()*cl[c].spread;
		NEB_POS[i*3+1] = cl[c].p[1] + gauss()*cl[c].spread;
		NEB_POS[i*3+2] = cl[c].p[2] + gauss()*cl[c].spread;

		/* the spread can push a puff into the flight zone (apoapsis 46);
		   such a giant would swallow the scene, so push it outwards */
		float rr = sqrtf(NEB_POS[i*3+0]*NEB_POS[i*3+0] +
		                 NEB_POS[i*3+1]*NEB_POS[i*3+1] +
		                 NEB_POS[i*3+2]*NEB_POS[i*3+2]);
		if (rr < 58.0f && rr > 1e-4f) {
			float k = 58.0f / rr;
			NEB_POS[i*3+0] *= k;
			NEB_POS[i*3+1] *= k;
			NEB_POS[i*3+2] *= k;
		}

		float col[3];
		hsv(cl[c].hue + frnd2()*0.07f, 0.75f + frnd()*0.25f, 1.0f, col);
		float t = frnd();
		float b = 0.006f + t*t*0.026f;
		NEB_COL[i*3+0] = col[0]*b;
		NEB_COL[i*3+1] = col[1]*b;
		NEB_COL[i*3+2] = col[2]*b;

		/* Radius in world units (a billboard, so no pixel limit).
		   Fill cost grows with the SQUARE of this number. */
		NEB_R[i]    = (8.0f + frnd()*20.0f) * NEB_SIZE;
		NEB_PH[i]   = frnd()*TAU;
		NEB_RATE[i] = 0.05f + frnd()*0.16f;
	}
}

/* ================= objects ======================================= */

struct flyer {
	float pos[3], vel[3], dir[3];
	float col[3];
	float hue, hue_rate;
	float width;
	int   ribbon;

	/* trail: colour stored at emission time, hence the rainbow gradient */
	float *sx, *sy, *sz, *sa, *scr, *scg, *scb;
	int   n;
	float acc;

	float bead_acc, mist_acc, spark_acc;
	float head_glow;
	float fuse;

	int   group;        /* -1 = solo */
	int   gindex;
	bool  waiting;      /* waiting for a spot outside the frame */
};

struct group {
	float pos[3], vel[3];        /* leader — invisible */
	float u[3], v[3];            /* perpendicular basis, carried over time */
	float radius, omega, phase;
	float fuse;
	int   members[3];
	int   count;
	bool  waiting;
};

static struct flyer  *FL;
static struct group  *GR;
static int  n_flyers = 0, n_groups = 0;
static float *TRAIL;                 /* shared trail storage */

/* view matrix and the edges of the whole view (all outputs) as
   tangents — needed for entries */
static float CUR_VIEW[16];
static float CUR_ALL[4];
static float CAM_EYE[3] = { 0.0f, 0.0f, 0.0f };
static bool  vp_ready = false;

/* ---- orbits ---------------------------------------------------- */

static void init_orbit(float *pos, float *vel, float r0, float k)
{
	float u[3] = { frnd2(), frnd2(), frnd2() };
	vnorm(u);

	float t1[3], t2[3];
	perp_basis(u, t1, t2);

	/* random orbital plane */
	float a = frnd() * TAU;
	float ca = cosf(a), sa = sinf(a);
	float w[3] = { t1[0]*ca + t2[0]*sa,
	               t1[1]*ca + t2[1]*sa,
	               t1[2]*ca + t2[2]*sa };

	pos[0] = u[0]*r0; pos[1] = u[1]*r0; pos[2] = u[2]*r0;

	float vc = sqrtf(HOLE_GM / sqrtf(r0*r0 + SOFT2)) * k;
	vel[0] = w[0]*vc; vel[1] = w[1]*vc; vel[2] = w[2]*vc;
}

/*
 * An orbit around the viewer. Periapsis must not be too small, or the
 * object would fly through the near plane and vanish with a bang.
 */
static void random_orbit(float *pos, float *vel)
{
	for (int i = 0; i < 40; i++) {
		float r0 = 7.0f + frnd()*34.0f;
		float k  = frnd() < 0.45f ? 0.45f + frnd()*0.34f   /* elongated */
		                          : 0.82f + frnd()*0.40f;  /* roundish */

		/* for k<1 the start point is apoapsis, for k>1 periapsis */
		float den = 2.0f - k*k;
		if (den < 0.12f) den = 0.12f;
		float other = r0 * (k*k) / den;
		float peri = r0 < other ? r0 : other;
		float apo  = r0 < other ? other : r0;

		if (peri < R_PERI_MIN || apo > R_APO_MAX)
			continue;

		init_orbit(pos, vel, r0, k);
		return;
	}
	init_orbit(pos, vel, 16.0f + frnd()*12.0f, 0.95f);
}

static void grav_step(float *pos, float *vel, float dt, float *moved)
{
	float s = pos[0]*pos[0] + pos[1]*pos[1] + pos[2]*pos[2] + SOFT2;
	float acc = -HOLE_GM / (s * sqrtf(s));

	vel[0] += pos[0]*acc*dt;
	vel[1] += pos[1]*acc*dt;
	vel[2] += pos[2]*acc*dt;

	float px = pos[0], py = pos[1], pz = pos[2];
	pos[0] += vel[0]*dt;
	pos[1] += vel[1]*dt;
	pos[2] += vel[2]*dt;

	if (moved) {
		float dx = pos[0]-px, dy = pos[1]-py, dz = pos[2]-pz;
		*moved = sqrtf(dx*dx + dy*dy + dz*dz);
	}
}

/* ---- entering from off-screen ---------------------------------- */

static bool on_screen(const float *p, float margin)
{
	if (!vp_ready)
		return false;

	const float *m = CUR_VIEW;
	float d = -(m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14]);
	if (d <= 0.05f)
		return false;                 /* behind the camera = off-screen */

	float x = (m[0]*p[0] + m[4]*p[1] + m[8]*p[2] + m[12]) / d;
	float y = (m[1]*p[0] + m[5]*p[1] + m[9]*p[2] + m[13]) / d;
	const float *a = CUR_ALL;
	return fabsf(x - (a[0]+a[1])*0.5f) <= (a[1]-a[0])*0.5f*margin &&
	       fabsf(y - (a[2]+a[3])*0.5f) <= (a[3]-a[2])*0.5f*margin;
}

/*
 * An object never materialises in view: keep drawing orbits until the
 * start point lands off-screen. If that fails, it waits hidden and
 * tries again — the camera keeps turning anyway.
 */
static bool respawn_offscreen(float *pos, float *vel)
{
	for (int i = 0; i < 60; i++) {
		random_orbit(pos, vel);

		float dx = pos[0]-CAM_EYE[0], dy = pos[1]-CAM_EYE[1], dz = pos[2]-CAM_EYE[2];
		if (sqrtf(dx*dx + dy*dy + dz*dz) < 12.0f)
			continue;

		/* margin with room for the circling radius in groups */
		if (!on_screen(pos, 1.22f))
			return true;
	}
	return false;
}

/* ---- creation -------------------------------------------------- */

static void trail_reset(struct flyer *f)
{
	f->n = 1; f->acc = 0.0f;
	f->sx[0]=f->pos[0]; f->sy[0]=f->pos[1]; f->sz[0]=f->pos[2];
	f->sa[0]=0.0f;
	f->scr[0]=f->col[0]; f->scg[0]=f->col[1]; f->scb[0]=f->col[2];
}

static int make_flyer(int ribbon, float hue, float hue_rate)
{
	if (n_flyers >= MAX_FLYERS)
		return -1;

	int idx = n_flyers++;
	struct flyer *f = &FL[idx];
	memset(f, 0, sizeof(*f));

	/* a slice of the shared storage: 7 channels of MAX_SAMPLES */
	float *base = TRAIL + (size_t)idx * MAX_SAMPLES * 7;
	f->sx  = base;
	f->sy  = base + MAX_SAMPLES;
	f->sz  = base + MAX_SAMPLES*2;
	f->sa  = base + MAX_SAMPLES*3;
	f->scr = base + MAX_SAMPLES*4;
	f->scg = base + MAX_SAMPLES*5;
	f->scb = base + MAX_SAMPLES*6;

	f->ribbon = ribbon >= 0 ? ribbon : irnd(R_COUNT);
	f->hue = hue >= 0.0f ? hue : frnd();
	f->hue_rate = hue_rate > 0.0f ? hue_rate
	            : (frnd() < 0.4f ? 0.010f + frnd()*0.030f
	                             : 0.055f + frnd()*0.14f);

	switch (f->ribbon) {
	case R_TUBE:   f->width = 0.22f + frnd()*0.20f;  break;
	case R_THREAD: f->width = 0.040f + frnd()*0.040f; break;
	case R_VAPOR:  f->width = 0.070f + frnd()*0.055f; break;
	case R_BEAD:   f->width = 0.045f + frnd()*0.035f; break;
	default:       f->width = 0.055f + frnd()*0.050f; break;
	}

	hsv(f->hue, 1.0f, 1.0f, f->col);
	random_orbit(f->pos, f->vel);
	f->dir[0] = 1.0f;
	f->head_glow = 1.0f;
	f->fuse = 11.0f + frnd()*28.0f;
	f->group = -1;
	trail_reset(f);
	return idx;
}

static void make_group(int count)
{
	if (n_groups >= MAX_GROUPS || count > 3)
		return;

	struct group *g = &GR[n_groups];
	memset(g, 0, sizeof(*g));
	random_orbit(g->pos, g->vel);

	g->radius = 0.6f + frnd()*1.5f;
	g->omega  = (0.7f + frnd()*4.2f) * (frnd() < 0.5f ? -1.0f : 1.0f);
	g->phase  = frnd()*TAU;
	g->fuse   = 15.0f + frnd()*30.0f;
	g->count  = count;

	/* member colours are related but distinguishable */
	float h0 = frnd(), spread = 0.16f + frnd()*0.22f;
	float rate = frnd() < 0.4f ? 0.012f + frnd()*0.030f
	                           : 0.05f + frnd()*0.13f;
	static const int kinds[] = { R_THREAD, R_SPARK, R_VAPOR, R_THREAD };

	for (int i = 0; i < count; i++) {
		int id = make_flyer(kinds[irnd(4)], h0 + (float)i*spread, rate);
		if (id < 0) return;
		FL[id].group = n_groups;
		FL[id].gindex = i;
		g->members[i] = id;
	}

	/* straight onto orbital positions — otherwise the first step
	   would teleport members from random places */
	float ld[3] = { g->vel[0], g->vel[1], g->vel[2] };
	vnorm(ld);
	perp_basis(ld, g->u, g->v);
	for (int i = 0; i < count; i++) {
		struct flyer *f = &FL[g->members[i]];
		float a = g->phase + (float)i * TAU / (float)count;
		float ca = cosf(a)*g->radius, sa = sinf(a)*g->radius;
		f->pos[0] = g->pos[0] + g->u[0]*ca + g->v[0]*sa;
		f->pos[1] = g->pos[1] + g->u[1]*ca + g->v[1]*sa;
		f->pos[2] = g->pos[2] + g->u[2]*ca + g->v[2]*sa;
		trail_reset(f);
	}
	n_groups++;
}

/* ================= simulation step =============================== */

static int active_count(void)
{
	int n = (int)lrintf((float)n_flyers * DENSITY);
	return n < 4 ? 4 : n;
}
static bool sleeping(int idx) { return idx >= active_count(); }

static void trail_push(struct flyer *f)
{
	if (f->n >= MAX_SAMPLES) {
		memmove(f->sx,  f->sx +1, (MAX_SAMPLES-1)*sizeof(float));
		memmove(f->sy,  f->sy +1, (MAX_SAMPLES-1)*sizeof(float));
		memmove(f->sz,  f->sz +1, (MAX_SAMPLES-1)*sizeof(float));
		memmove(f->sa,  f->sa +1, (MAX_SAMPLES-1)*sizeof(float));
		memmove(f->scr, f->scr+1, (MAX_SAMPLES-1)*sizeof(float));
		memmove(f->scg, f->scg+1, (MAX_SAMPLES-1)*sizeof(float));
		memmove(f->scb, f->scb+1, (MAX_SAMPLES-1)*sizeof(float));
		f->n--;
	}
	int i = f->n++;
	f->sx[i]=f->pos[0]; f->sy[i]=f->pos[1]; f->sz[i]=f->pos[2];
	f->sa[i]=0.0f;
	f->scr[i]=f->col[0]; f->scg[i]=f->col[1]; f->scb[i]=f->col[2];
}

static void flyer_tail(struct flyer *f, float t, float dt, float step)
{
	/* hue drift */
	f->hue += f->hue_rate * HUE_MUL * dt;
	hsv(f->hue, 1.0f, 1.0f, f->col);

	f->acc += step;
	while (f->acc >= SAMPLE_STEP_CFG) { f->acc -= SAMPLE_STEP_CFG; trail_push(f); }

	for (int i = 0; i < f->n; i++)
		f->sa[i] += dt;
	while (f->n > 1 && f->sa[0] > TRAIL_LIFE) {
		memmove(f->sx,  f->sx +1, (f->n-1)*sizeof(float));
		memmove(f->sy,  f->sy +1, (f->n-1)*sizeof(float));
		memmove(f->sz,  f->sz +1, (f->n-1)*sizeof(float));
		memmove(f->sa,  f->sa +1, (f->n-1)*sizeof(float));
		memmove(f->scr, f->scr+1, (f->n-1)*sizeof(float));
		memmove(f->scg, f->scg+1, (f->n-1)*sizeof(float));
		memmove(f->scb, f->scb+1, (f->n-1)*sizeof(float));
		f->n--;
	}

	float bx = f->pos[0], by = f->pos[1], bz = f->pos[2];
	static const float white[3] = { 1.0f, 1.0f, 1.0f };

	/* SPARKS — shed from the object itself, in all directions, and short-
	   lived, so they stay close to it like sparklers */
	if (f->ribbon == R_SPARK) {
		f->spark_acc += dt * 70.0f;
		int n = (int)f->spark_acc;
		f->spark_acc -= (float)n;
		if (n > 4) n = 4;
		for (int i = 0; i < n; i++) {
			float d[3] = { frnd2(), frnd2(), frnd2() };
			vnorm(d);
			float s = 1.8f + frnd()*3.4f;
			spark_add(bx, by, bz,
			          d[0]*s + f->vel[0]*0.12f,
			          d[1]*s + f->vel[1]*0.12f,
			          d[2]*s + f->vel[2]*0.12f,
			          frnd() < 0.3f ? white : f->col,
			          0.22f + frnd()*0.34f, 1.3f + frnd()*2.0f, 0.0f);
		}
	}

	/* beads along the path */
	if (f->ribbon == R_BEAD) {
		f->bead_acc += step;
		if (f->bead_acc > 0.5f) {
			f->bead_acc = 0.0f;
			spark_add(bx, by, bz, 0,0,0, f->col,
			          1.6f, 4.5f + frnd()*2.5f, 0.0f);
		}
	}

	/* STEAM AND SMOKE — only from the tail, never from the object */
	if (MIST_MUL > 0.0f && f->n > 4) {
		float rate = (f->ribbon == R_VAPOR ? 26.0f : 9.0f) * MIST_MUL;
		f->mist_acc += dt * rate;
		int n = (int)f->mist_acc;
		f->mist_acc -= (float)n;
		if (n > 6) n = 6;
		for (int i = 0; i < n; i++) {
			int j = irnd(f->n > 2 ? f->n - 2 : 1);
			float c[3] = { f->scr[j], f->scg[j], f->scb[j] };
			mist_add(f->sx[j], f->sy[j], f->sz[j], c,
			         f->ribbon == R_VAPOR ? 1.0f : 0.7f);
		}
	}

	f->head_glow = 0.85f + 0.35f * sinf(t*7.0f + f->hue*10.0f);
}

/*
 * A perpendicular basis computed from scratch can flip by 90 degrees
 * when the leader changes direction — members used to teleport to the
 * other side. Instead, the previous basis is carried over.
 */
static void carry_basis(struct group *g, const float *ld)
{
	float ux = g->u[0], uy = g->u[1], uz = g->u[2];
	float dot = ux*ld[0] + uy*ld[1] + uz*ld[2];
	ux -= ld[0]*dot; uy -= ld[1]*dot; uz -= ld[2]*dot;

	float l = sqrtf(ux*ux + uy*uy + uz*uz);
	if (l < 1e-4f) { perp_basis(ld, g->u, g->v); return; }

	g->u[0] = ux/l; g->u[1] = uy/l; g->u[2] = uz/l;
	g->v[0] = ld[1]*g->u[2] - ld[2]*g->u[1];
	g->v[1] = ld[2]*g->u[0] - ld[0]*g->u[2];
	g->v[2] = ld[0]*g->u[1] - ld[1]*g->u[0];
}

static void step_all(float t, float dt)
{
	/* solos */
	for (int i = 0; i < n_flyers; i++) {
		struct flyer *f = &FL[i];
		if (f->group >= 0 || sleeping(i))
			continue;

		if (f->waiting) {
			if (respawn_offscreen(f->pos, f->vel)) {
				f->waiting = false;
				trail_reset(f);
			}
			continue;
		}

		float step = 0.0f;
		grav_step(f->pos, f->vel, dt, &step);

		float vl = sqrtf(f->vel[0]*f->vel[0] + f->vel[1]*f->vel[1]
		               + f->vel[2]*f->vel[2]);
		if (vl < 1e-6f) vl = 1.0f;
		f->dir[0]=f->vel[0]/vl; f->dir[1]=f->vel[1]/vl; f->dir[2]=f->vel[2]/vl;

		flyer_tail(f, t, dt, step);

		f->fuse -= dt;
		if (f->fuse <= 0.0f) {
			explode(f->pos[0], f->pos[1], f->pos[2], f->col);
			f->fuse = 13.0f + frnd()*30.0f;
			f->hue = frnd();
			hsv(f->hue, 1.0f, 1.0f, f->col);
			f->waiting = !respawn_offscreen(f->pos, f->vel);
			trail_reset(f);
		}
	}

	/* groups: the leader steers, members circle it on a helix */
	for (int gi = 0; gi < n_groups; gi++) {
		struct group *g = &GR[gi];
		if (g->count == 0 || sleeping(g->members[0]))
			continue;

		if (g->waiting) {
			if (respawn_offscreen(g->pos, g->vel)) {
				g->waiting = false;
				for (int i = 0; i < g->count; i++) {
					struct flyer *f = &FL[g->members[i]];
					f->pos[0]=g->pos[0]; f->pos[1]=g->pos[1]; f->pos[2]=g->pos[2];
					trail_reset(f);
				}
			}
			continue;
		}

		grav_step(g->pos, g->vel, dt, NULL);

		float ld[3] = { g->vel[0], g->vel[1], g->vel[2] };
		vnorm(ld);
		carry_basis(g, ld);
		g->phase += g->omega * dt;

		for (int i = 0; i < g->count; i++) {
			struct flyer *f = &FL[g->members[i]];
			float a = g->phase + (float)i * TAU / (float)g->count;
			float ca = cosf(a)*g->radius, sa = sinf(a)*g->radius;

			float nx = g->pos[0] + g->u[0]*ca + g->v[0]*sa;
			float ny = g->pos[1] + g->u[1]*ca + g->v[1]*sa;
			float nz = g->pos[2] + g->u[2]*ca + g->v[2]*sa;

			float dx = nx-f->pos[0], dy = ny-f->pos[1], dz = nz-f->pos[2];
			float dl = sqrtf(dx*dx + dy*dy + dz*dz);
			if (dl > 1e-6f) {
				f->dir[0]=dx/dl; f->dir[1]=dy/dl; f->dir[2]=dz/dl;
				f->vel[0]=dx/dt; f->vel[1]=dy/dt; f->vel[2]=dz/dt;
			}
			f->pos[0]=nx; f->pos[1]=ny; f->pos[2]=nz;
			flyer_tail(f, t, dt, dl);
		}

		g->fuse -= dt;
		if (g->fuse <= 0.0f) {
			g->fuse = 17.0f + frnd()*32.0f;
			for (int i = 0; i < g->count; i++) {
				struct flyer *f = &FL[g->members[i]];
				explode(f->pos[0], f->pos[1], f->pos[2], f->col);
			}
			g->waiting = !respawn_offscreen(g->pos, g->vel);
			g->radius = 0.6f + frnd()*1.5f;
			g->omega  = (0.7f + frnd()*4.2f) * (frnd() < 0.5f ? -1.0f : 1.0f);

			float h0 = frnd(), spread = 0.16f + frnd()*0.22f;
			for (int i = 0; i < g->count; i++) {
				struct flyer *f = &FL[g->members[i]];
				f->hue = h0 + (float)i*spread;
				hsv(f->hue, 1.0f, 1.0f, f->col);
				f->pos[0]=g->pos[0]; f->pos[1]=g->pos[1]; f->pos[2]=g->pos[2];
				trail_reset(f);
			}
		}
	}
}

/* ================= shaders ======================================= */

static const char *VS_RIB =
"attribute vec3 aPos; attribute vec3 aCol; attribute vec3 aOff;\n"
"attribute float aSide;\n"
"uniform mat4 uMVP; uniform float uWidth, uGain;\n"
"varying vec3 vCol; varying float vSide;\n"
"void main(){\n"
"  gl_Position=uMVP*vec4(aPos+aOff*uWidth,1.0);\n"
"  vCol=aCol*uGain;\n"
"  vSide=aSide;\n"
"}\n";

static const char *FS_RIB =
"precision highp float;\n"
"varying vec3 vCol; varying float vSide;\n"
"uniform float uSoft, uHeat;\n"
"void main(){\n"
"  float s2=vSide*vSide;\n"
"  float a=exp(-s2*uSoft);\n"
/* the narrow axis of a ribbon is hotter than its edges — like glowing metal */
"  float core=exp(-s2*uSoft*7.0);\n"
"  float lum=max(max(vCol.r,vCol.g),vCol.b);\n"
"  gl_FragColor=vec4(vCol*a + vec3(lum)*core*uHeat, 1.0);\n"
"}\n";

static const char *VS_PTS =
"attribute vec3 aPos; attribute vec3 aCol; attribute float aSize;\n"
"uniform mat4 uMVP; uniform float uScale;\n"
"varying vec3 vCol;\n"
"void main(){\n"
"  gl_Position=uMVP*vec4(aPos,1.0);\n"
"  float w=max(gl_Position.w,0.15);\n"
"  gl_PointSize=clamp(aSize*uScale/w,1.0,260.0);\n"
"  vCol=aCol;\n"
"}\n";

static const char *FS_PTS =
"precision highp float;\n"
"varying vec3 vCol;\n"
"void main(){\n"
"  vec2 d=gl_PointCoord-vec2(0.5);\n"
"  float r2=dot(d,d);\n"
"  if(r2>0.25) discard;\n"
"  gl_FragColor=vec4(vCol*(exp(-r2*13.0)*0.72+smoothstep(0.020,0.0,r2)*1.45),1.0);\n"
"}\n";

static const char *FS_MIST =
"precision highp float;\n"
"varying vec3 vCol;\n"
"void main(){\n"
"  vec2 d=gl_PointCoord-vec2(0.5);\n"
"  float r2=dot(d,d);\n"
"  if(r2>0.25) discard;\n"
"  gl_FragColor=vec4(vCol*exp(-r2*7.0)*0.5,1.0);\n"
"}\n";

/* nebulae as camera-facing quads */
static const char *VS_NEB =
"attribute vec3 aPos; attribute vec3 aCol; attribute vec2 aUV;\n"
"uniform mat4 uMVP;\n"
"varying vec3 vCol; varying vec2 vUV;\n"
"void main(){ gl_Position=uMVP*vec4(aPos,1.0); vCol=aCol; vUV=aUV; }\n";

static const char *FS_NEB =
"precision highp float;\n"
"varying vec3 vCol; varying vec2 vUV;\n"
"void main(){\n"
"  float r2=dot(vUV,vUV);\n"
"  if(r2>1.0) discard;\n"
"  float a=exp(-r2*2.6)-0.074;\n"
"  if(a<=0.0) discard;\n"
"  gl_FragColor=vec4(vCol*a,1.0);\n"
"}\n";

static const char *VS_QUAD =
"attribute vec2 aPos; varying vec2 vUV;\n"
"void main(){ vUV=aPos*0.5+0.5; gl_Position=vec4(aPos,0.0,1.0); }\n";

static const char *FS_BRIGHT =
"precision highp float; varying vec2 vUV;\n"
"uniform sampler2D uSrc; uniform float uThresh, uScale;\n"
"void main(){\n"
"  vec3 c=texture2D(uSrc,vUV).rgb;\n"
"  gl_FragColor=vec4(max(c-uThresh,0.0)*uScale,1.0);\n"
"}\n";

static const char *FS_BLUR =
"precision highp float; varying vec2 vUV;\n"
"uniform sampler2D uSrc; uniform vec2 uDir;\n"
"void main(){\n"
"  vec3 c=texture2D(uSrc,vUV).rgb*0.2270270;\n"
"  c+=texture2D(uSrc,vUV+uDir*1.3846154).rgb*0.3162162;\n"
"  c+=texture2D(uSrc,vUV-uDir*1.3846154).rgb*0.3162162;\n"
"  c+=texture2D(uSrc,vUV+uDir*3.2307692).rgb*0.0702703;\n"
"  c+=texture2D(uSrc,vUV-uDir*3.2307692).rgb*0.0702703;\n"
"  gl_FragColor=vec4(c,1.0);\n"
"}\n";

static const char *FS_PRES =
"precision highp float; varying vec2 vUV;\n"
"uniform sampler2D uScene, uB4, uB8;\n"
"uniform float uBloom, uExposure, uFringe, uFade;\n"
"uniform float uHas4, uHas8;\n"
"uniform vec2 uRes;\n"
"void main(){\n"
"  vec3 c=texture2D(uScene,vUV).rgb;\n"
/* wide halo with channel split — the rainbow fringe */
"  vec2 d=(vUV-0.5)*uFringe;\n"
"  vec3 wide=vec3(texture2D(uB8,vUV+d).r,\n"
"                 texture2D(uB8,vUV).g,\n"
"                 texture2D(uB8,vUV-d).b);\n"
"  c+=(texture2D(uB4,vUV).rgb*uHas4 + wide*1.35*uHas8)*uBloom;\n"
"  c*=uExposure*uFade;\n"
"  c=c/(1.0+c*0.30);\n"
"  c=pow(c,vec3(0.4545));\n"
/* dithers quantisation error — without it nebulae band */
"  float n=fract(sin(dot(vUV*uRes,vec2(12.9898,78.233)))*43758.5453);\n"
"  c+=(n-0.5)*(1.0/255.0);\n"
"  gl_FragColor=vec4(c,1.0);\n"
"}\n";

/* ================= GL resources ================================== */

static GLuint compile_shader(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);

	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "nebulights: shader compile:\n%s\n", log);
		exit(1);
	}
	return sh;
}

static GLuint link_program(const char *vs, const char *fs,
                           const char *const *attrs, int n_attrs)
{
	GLuint p = glCreateProgram();
	GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
	GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);

	glAttachShader(p, v);
	glAttachShader(p, f);
	for (int i = 0; i < n_attrs; i++)
		glBindAttribLocation(p, (GLuint)i, attrs[i]);
	glLinkProgram(p);

	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "nebulights: program link:\n%s\n", log);
		exit(1);
	}
	glDeleteShader(v);
	glDeleteShader(f);
	return p;
}

static struct {
	GLuint rib, pts, mist, neb, bright, blur, pres;

	GLint r_mvp, r_soft, r_width, r_gain, r_heat;
	GLint p_mvp, p_scale;
	GLint m_mvp, m_scale;
	GLint n_mvp;
	GLint b_src, b_thresh, b_scale;
	GLint l_src, l_dir;
	GLint z_scene, z_b4, z_b8, z_bloom, z_exp, z_fringe, z_res, z_fade;
	GLint z_has4, z_has8;
} P;

static GLuint vbo_rib, vbo_pts, vbo_mist, vbo_neb, vbo_quad, vbo_bg;

/* CPU-side buffers */
#define RIB_STRIDE 10
static float *RIB;   int rib_v = 0;
static int    n_range = 0;   /* number of ribbons in the strip */
static float *PTS;   int pts_n = 0;
static float *MST;   int mst_n = 0;
static float *NEBV;  int neb_v = 0;

/* render target and glow chain */
static GLuint tex_scene = 0, fbo_scene = 0;
static GLuint t4a=0,t4b=0,t8a=0,t8b=0, f4a=0,f4b=0,f8a=0,f8b=0;
static GLuint tex_neb=0, fbo_neb=0;   /* nebulae at reduced resolution */
static int rt_w = 0, rt_h = 0, b4w=0,b4h=0,b8w=0,b8h=0;

static GLenum TEX_TYPE = GL_UNSIGNED_BYTE;
static GLint  TEX_FILTER = GL_LINEAR;
static bool   HDR = false;
static float  EXPOSURE = 1.0f;

/* ---- textures and the glow chain ------------------------------- */

static GLuint mk_tex(int w, int h)
{
	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, TEX_TYPE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TEX_FILTER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, TEX_FILTER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

static GLuint mk_fbo(GLuint t)
{
	GLuint f;
	glGenFramebuffers(1, &f);
	glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, t, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return f;
}

/*
 * An 8-bit buffer clips additive blending at 1.0 — a white-hot ribbon
 * core and ordinary glow end up as the same white. Half-float keeps
 * the real values, so the glow has something to tell apart.
 */
static void probe_hdr(void)
{
	if (!ALLOW_HDR)
		return;
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	if (!ext || !strstr(ext, "GL_OES_texture_half_float"))
		return;

	GLuint t;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA,
	             GL_HALF_FLOAT_OES, NULL);

	GLuint f;
	glGenFramebuffers(1, &f);
	glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, t, 0);
	bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &f);
	glDeleteTextures(1, &t);

	if (ok) {
		TEX_TYPE = GL_HALF_FLOAT_OES;
		HDR = true;
		/* linear filtering of half-floats is sometimes unsupported */
		TEX_FILTER = strstr(ext, "GL_OES_texture_half_float_linear")
		           ? GL_LINEAR : GL_NEAREST;
		/* without clipping the scene comes out brighter */
		EXPOSURE = 0.55f;
	}
}

/*
 * Every screen size has its own set of buffers. With monitors of
 * different resolutions everything used to be reallocated on every
 * frame of every output; now we just switch sets.
 */
struct targets {
	int w, h;
	GLuint tex_scene, fbo_scene, t4a, t4b, t8a, t8b, f4a, f4b, f8a, f8b;
	GLuint tex_neb, fbo_neb;
	int b4w, b4h, b8w, b8h;
};
#define MAX_TARGETS 4
static struct targets RT[MAX_TARGETS];
static int n_rt = 0;

static void targets_free(struct targets *r)
{
	GLuint ts[6] = { r->tex_scene, r->t4a, r->t4b, r->t8a, r->t8b, r->tex_neb };
	GLuint fs[6] = { r->fbo_scene, r->f4a, r->f4b, r->f8a, r->f8b, r->fbo_neb };
	glDeleteTextures(6, ts);
	glDeleteFramebuffers(6, fs);
}

static void make_targets(int w, int h)
{
	if (rt_w == w && rt_h == h)
		return;

	struct targets *r = NULL;
	for (int i = 0; i < n_rt; i++)
		if (RT[i].w == w && RT[i].h == h)
			r = &RT[i];

	if (!r) {
		if (n_rt == MAX_TARGETS) {   /* should not happen */
			targets_free(&RT[0]);
			memmove(RT, RT + 1, sizeof(RT[0]) * (MAX_TARGETS - 1));
			n_rt--;
		}
		r = &RT[n_rt++];
		r->w = w; r->h = h;

		r->tex_scene = mk_tex(w, h);
		r->fbo_scene = mk_fbo(r->tex_scene);

		r->b4w = w >> 2; r->b4h = h >> 2;
		r->b8w = w >> 3; r->b8h = h >> 3;
		if (r->b4w < 1) r->b4w = 1;
		if (r->b4h < 1) r->b4h = 1;
		if (r->b8w < 1) r->b8w = 1;
		if (r->b8h < 1) r->b8h = 1;

		/* Nebulae are the blurriest thing in the scene — a quarter of the side
		   spoils nothing and cuts fill sixteen times. */
		r->tex_neb = mk_tex(r->b4w, r->b4h);
		r->fbo_neb = mk_fbo(r->tex_neb);

		r->t4a = mk_tex(r->b4w, r->b4h); r->t4b = mk_tex(r->b4w, r->b4h);
		r->t8a = mk_tex(r->b8w, r->b8h); r->t8b = mk_tex(r->b8w, r->b8h);
		r->f4a = mk_fbo(r->t4a); r->f4b = mk_fbo(r->t4b);
		r->f8a = mk_fbo(r->t8a); r->f8b = mk_fbo(r->t8b);
	}

	tex_scene = r->tex_scene; fbo_scene = r->fbo_scene;
	t4a = r->t4a; t4b = r->t4b; t8a = r->t8a; t8b = r->t8b;
	f4a = r->f4a; f4b = r->f4b; f8a = r->f8a; f8b = r->f8b;
	tex_neb = r->tex_neb; fbo_neb = r->fbo_neb;
	b4w = r->b4w; b4h = r->b4h; b8w = r->b8w; b8h = r->b8h;
	rt_w = w; rt_h = h;
}

/* ---- building buffers ------------------------------------------ */

/*
 * All ribbons in ONE strip. Between objects we insert degenerate
 * triangles (the last and the first vertex repeated), which have zero
 * area and draw nothing. That way we have one draw call per pass
 * instead of 190.
 */
static void build_ribbons(const float *eye)
{
	rib_v = 0; n_range = 0;

	for (int fi = 0; fi < n_flyers; fi++) {
		struct flyer *f = &FL[fi];
		if (f->n < 2 || f->waiting || sleeping(fi))
			continue;
		if (f->group >= 0 && GR[f->group].waiting)
			continue;

		int start = rib_v;
		for (int i = 0; i < f->n; i++) {
			int i0 = i > 0 ? i-1 : 0;
			int i1 = i < f->n-1 ? i+1 : f->n-1;

			float tx=f->sx[i1]-f->sx[i0], ty=f->sy[i1]-f->sy[i0],
			      tz=f->sz[i1]-f->sz[i0];
			float tl = sqrtf(tx*tx+ty*ty+tz*tz);
			if (tl < 1e-6f) tl = 1.0f;
			tx/=tl; ty/=tl; tz/=tl;

			float vx=eye[0]-f->sx[i], vy=eye[1]-f->sy[i], vz=eye[2]-f->sz[i];
			float vl = sqrtf(vx*vx+vy*vy+vz*vz);
			if (vl < 1e-6f) vl = 1.0f;
			vx/=vl; vy/=vl; vz/=vl;

			float rx=ty*vz-tz*vy, ry=tz*vx-tx*vz, rz=tx*vy-ty*vx;
			float rl = sqrtf(rx*rx+ry*ry+rz*rz);
			if (rl < 1e-5f) { rx=1.0f; ry=0.0f; rz=0.0f; }
			else { rx/=rl; ry/=rl; rz/=rl; }

			float age = f->sa[i] / TRAIL_LIFE;
			float fade = 1.0f - age;
			if (fade < 0.0f) fade = 0.0f;
			fade = fade*fade*fade;                 /* steep falloff */
			float spread = 1.0f + age*age*3.2f;    /* swelling into mist */
			float headness = (float)i / (float)(f->n - 1);
			float hw = f->width * (0.22f + 0.78f*headness) * spread;

			float depth = clampf(20.0f / (vl > 3.0f ? vl : 3.0f), 0.30f, 2.4f);
			float g = fade * depth / spread
			        * (f->ribbon == R_TUBE ? 1.1f : 1.7f);

			float ox=rx*hw, oy=ry*hw, oz=rz*hw;
			float cr=f->scr[i]*g, cg=f->scg[i]*g, cb=f->scb[i]*g;

			for (int s = -1; s <= 1; s += 2) {
				float *o = RIB + (size_t)rib_v*RIB_STRIDE;
				o[0]=f->sx[i]; o[1]=f->sy[i]; o[2]=f->sz[i];
				o[3]=cr; o[4]=cg; o[5]=cb;
				o[6]=ox*(float)s; o[7]=oy*(float)s; o[8]=oz*(float)s;
				o[9]=(float)s;
				rib_v++;
			}
		}

		if (rib_v - start < 4) {          /* too short, skip */
			rib_v = start;
			continue;
		}

		/* Joint: repeat the last vertex of the previous ribbon AND the first
		   one of this ribbon. Both repeats are needed — with just one, a real
		   triangle appears that bridges the two ribbons. */
		if (n_range > 0) {
			memmove(RIB + (size_t)(start+2)*RIB_STRIDE,
			        RIB + (size_t)start*RIB_STRIDE,
			        (size_t)(rib_v-start)*RIB_STRIDE*sizeof(float));
			rib_v += 2;
			memcpy(RIB + (size_t)start*RIB_STRIDE,
			       RIB + (size_t)(start-1)*RIB_STRIDE,
			       RIB_STRIDE*sizeof(float));
			memcpy(RIB + (size_t)(start+1)*RIB_STRIDE,
			       RIB + (size_t)(start+2)*RIB_STRIDE,
			       RIB_STRIDE*sizeof(float));
		}
		n_range++;
	}
}

static void build_points(void)
{
	pts_n = 0;
	mst_n = 0;

	for (int i = 0; i < SP->n; i++) {
		float k = SP->life[i] / SP->life0[i];
		if (SP->grow[i] > 0.0f) {
			float *o = MST + (size_t)mst_n*7;
			float g = k*k*0.22f;
			o[0]=SP->x[i]; o[1]=SP->y[i]; o[2]=SP->z[i];
			o[3]=SP->r[i]*g; o[4]=SP->g[i]*g; o[5]=SP->b[i]*g;
			o[6]=SP->size[i]*(1.0f + SP->grow[i]*(1.0f-k));
			mst_n++;
		} else {
			float *o = PTS + (size_t)pts_n*7;
			float g = k*k*1.6f;
			o[0]=SP->x[i]; o[1]=SP->y[i]; o[2]=SP->z[i];
			o[3]=SP->r[i]*g; o[4]=SP->g[i]*g; o[5]=SP->b[i]*g;
			o[6]=SP->size[i]*(0.45f + k*0.75f);
			pts_n++;
		}
	}

	/* bright heads */
	for (int fi = 0; fi < n_flyers; fi++) {
		struct flyer *f = &FL[fi];
		if (f->waiting || sleeping(fi))
			continue;
		if (f->group >= 0 && GR[f->group].waiting)
			continue;

		float dx=CAM_EYE[0]-f->pos[0], dy=CAM_EYE[1]-f->pos[1],
		      dz=CAM_EYE[2]-f->pos[2];
		float d = sqrtf(dx*dx+dy*dy+dz*dz);
		float depth = clampf(20.0f / (d > 3.0f ? d : 3.0f), 0.4f, 3.0f);
		float g = 2.7f * f->head_glow * depth;

		float *o = PTS + (size_t)pts_n*7;
		o[0]=f->pos[0]; o[1]=f->pos[1]; o[2]=f->pos[2];
		o[3]=(f->col[0]*0.45f+0.55f)*g;
		o[4]=(f->col[1]*0.45f+0.55f)*g;
		o[5]=(f->col[2]*0.45f+0.55f)*g;
		o[6]=(f->ribbon == R_TUBE) ? 11.0f : 8.0f;
		pts_n++;
	}
}

static float VP[16];   /* view-projection of the current frame */

/* Fill estimate: how many full screens the puffs cover together.
   Counted, not guessed — it is the most expensive thing in the scene. */
static float neb_overdraw = 0.0f;

/* nebula quads spanned on the right/up vectors of the view matrix */
static void build_nebulae(const float *view, const float *eye,
                          float focal, float aspect, float t)
{
	static const float UV[12] = { -1,-1,  1,-1,  1,1,  -1,-1,  1,1,  -1,1 };
	float rx=view[0], ry=view[4], rz=view[8];
	float ux=view[1], uy=view[5], uz=view[9];

	neb_v = 0;
	neb_overdraw = 0.0f;

	for (int i = 0; i < N_NEB; i++) {
		float b = (0.72f + 0.28f*sinf(NEB_PH[i] + t*NEB_RATE[i])) * NEB_MUL;
		if (b <= 0.0f) continue;

		float s = NEB_R[i];

		{
			/* Project the centre: puffs behind us and off-screen cost
			   nothing, so they must not be counted. */
			const float *m = VP;
			float cx = NEB_POS[i*3], cy = NEB_POS[i*3+1], cz = NEB_POS[i*3+2];
			float w = m[3]*cx + m[7]*cy + m[11]*cz + m[15];
			if (w > 0.05f) {
				float nx = (m[0]*cx + m[4]*cy + m[8] *cz + m[12]) / w;
				float ny = (m[1]*cx + m[5]*cy + m[9] *cz + m[13]) / w;

				float dx = cx-eye[0], dy = cy-eye[1], dz = cz-eye[2];
				float dist = sqrtf(dx*dx+dy*dy+dz*dz);
				if (dist > 0.5f) {
					float ry = s*focal/dist;
					float rx = ry/aspect;
					/* off-screen? */
					if (fabsf(nx)-rx < 1.0f && fabsf(ny)-ry < 1.0f) {
						float a = 3.14159f*rx*ry/4.0f;
						neb_overdraw += a > 1.0f ? 1.0f : a;
					}
				}
			}
		}
		float cx=NEB_POS[i*3], cy=NEB_POS[i*3+1], cz=NEB_POS[i*3+2];
		float cr=NEB_COL[i*3]*b, cg=NEB_COL[i*3+1]*b, cb=NEB_COL[i*3+2]*b;

		for (int k = 0; k < 6; k++) {
			float u = UV[k*2], v = UV[k*2+1];
			float *o = NEBV + (size_t)neb_v*8;
			o[0]=cx+(rx*u+ux*v)*s;
			o[1]=cy+(ry*u+uy*v)*s;
			o[2]=cz+(rz*u+uz*v)*s;
			o[3]=cr; o[4]=cg; o[5]=cb;
			o[6]=u;  o[7]=v;
			neb_v++;
		}
	}
}

/* ---- drawing --------------------------------------------------- */

static void draw_quad(void)
{
	glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableVertexAttribArray(0);
}

static void draw_points(GLuint prog, GLint u_mvp, GLint u_scale,
                        GLuint vbo, int n, float scale)
{
	if (n <= 0) return;
	glUseProgram(prog);
	glUniformMatrix4fv(u_mvp, 1, GL_FALSE, VP);
	glUniform1f(u_scale, scale);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	for (int i = 0; i < 3; i++) glEnableVertexAttribArray((GLuint)i);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 28, (void *)12);
	glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 28, (void *)24);
	glDrawArrays(GL_POINTS, 0, n);
	for (int i = 0; i < 3; i++) glDisableVertexAttribArray((GLuint)i);
}

static void draw_ribbons(float soft, float width, float gain, float heat)
{
	if (rib_v == 0) return;

	glUseProgram(P.rib);
	glUniformMatrix4fv(P.r_mvp, 1, GL_FALSE, VP);
	glUniform1f(P.r_soft, soft);
	glUniform1f(P.r_width, width);
	glUniform1f(P.r_gain, gain);
	glUniform1f(P.r_heat, heat);

	glBindBuffer(GL_ARRAY_BUFFER, vbo_rib);
	const GLsizei st = RIB_STRIDE*sizeof(float);
	for (int i = 0; i < 4; i++) glEnableVertexAttribArray((GLuint)i);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, st, (void *)0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void *)12);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, st, (void *)24);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, st, (void *)36);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, rib_v);

	for (int i = 0; i < 4; i++) glDisableVertexAttribArray((GLuint)i);
}

static void blur_into(GLuint dst, GLuint src, int w, int h, float dx, float dy)
{
	glBindFramebuffer(GL_FRAMEBUFFER, dst);
	glViewport(0, 0, w, h);
	glUseProgram(P.blur);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src);
	glUniform1i(P.l_src, 0);
	glUniform2f(P.l_dir, dx, dy);
	draw_quad();
}

static void run_bloom(void)
{
	if (BLOOM_LEVELS <= 0)
		return;

	glDisable(GL_BLEND);

	glBindFramebuffer(GL_FRAMEBUFFER, f4a);
	glViewport(0, 0, b4w, b4h);
	glUseProgram(P.bright);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex_scene);
	glUniform1i(P.b_src, 0);
	/* in HDR the threshold is above white — only what is hot glows */
	glUniform1f(P.b_thresh, HDR ? 0.75f : 0.085f);
	glUniform1f(P.b_scale,  HDR ? 0.85f : 1.30f);
	draw_quad();

	blur_into(f4b, t4a, b4w, b4h, 1.0f/(float)b4w, 0.0f);
	blur_into(f4a, t4b, b4w, b4h, 0.0f, 1.0f/(float)b4h);

	if (BLOOM_LEVELS < 2)
		return;

	blur_into(f8a, t4a, b8w, b8h, 1.0f/(float)b8w, 0.0f);
	blur_into(f8b, t8a, b8w, b8h, 0.0f, 1.0f/(float)b8h);
	blur_into(f8a, t8b, b8w, b8h, 2.0f/(float)b8w, 0.0f);
	blur_into(f8b, t8a, b8w, b8h, 0.0f, 2.0f/(float)b8h);
}

/* ================= interface ===================================== */

static bool ready = false;    /* GL objects exist in the current context */
static double last_t = -1.0;  /* time of the last simulation step */

/*
 * Frame meter. NEBULIGHTS_STATS=1 prints the frame time and what it is
 * made of every 5 s. Without it, performance tuning is guesswork.
 */
static bool  stats_on = false;
static double stats_t0 = 0.0;
static int    stats_frames = 0;
static double stats_worst = 0.0, stats_sum = 0.0;

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1e6;
}

static void build_world(void)
{
	SP->n = 0;
	n_flyers = 0;
	n_groups = 0;

	seed_bg();
	seed_nebulae();

#define SCALED(n) ((n) * FLYER_SCALE_PCT / 100)
	for (int i = 0; i < SCALED(14); i++) make_group(2);
	for (int i = 0; i < SCALED(9);  i++) make_group(3);
	for (int i = 0; i < SCALED(48); i++) make_flyer(R_THREAD, -1.0f, 0.0f);
	for (int i = 0; i < SCALED(30); i++) make_flyer(R_SPARK,  -1.0f, 0.0f);
	for (int i = 0; i < SCALED(28); i++) make_flyer(R_VAPOR,  -1.0f, 0.0f);
	for (int i = 0; i < SCALED(12); i++) make_flyer(R_BEAD,   -1.0f, 0.0f);
	for (int i = 0; i < SCALED(12); i++) make_flyer(R_TUBE,   -1.0f, 0.0f);
#undef SCALED
}

/* CPU buffers live for the whole process, the world is rebuilt per activation */
static void alloc_buffers(void)
{
	if (SP)
		return;

	SP      = calloc(1, sizeof(*SP));
	FL      = calloc(MAX_FLYERS, sizeof(*FL));
	GR      = calloc(MAX_GROUPS, sizeof(*GR));
	TRAIL   = calloc((size_t)MAX_FLYERS*MAX_SAMPLES*7, sizeof(float));
	BG      = calloc((size_t)N_BG_MAX*7, sizeof(float));
	NEB_POS = calloc((size_t)N_NEB_MAX*3, sizeof(float));
	NEB_COL = calloc((size_t)N_NEB_MAX*3, sizeof(float));
	NEB_R   = calloc(N_NEB_MAX, sizeof(float));
	NEB_PH  = calloc(N_NEB_MAX, sizeof(float));
	NEB_RATE= calloc(N_NEB_MAX, sizeof(float));
	/* +2 vertices per ribbon: degenerate joints */
	RIB     = calloc((size_t)MAX_FLYERS*(MAX_SAMPLES*2+2)*RIB_STRIDE, sizeof(float));
	PTS     = calloc((size_t)(MAX_SPARK+MAX_FLYERS)*7, sizeof(float));
	MST     = calloc((size_t)MAX_SPARK*7, sizeof(float));
	NEBV    = calloc((size_t)N_NEB_MAX*6*8, sizeof(float));

	if (!SP || !FL || !GR || !TRAIL || !BG || !NEB_POS || !NEB_COL ||
	    !NEB_R || !NEB_PH || !NEB_RATE || !RIB || !PTS || !MST || !NEBV) {
		fprintf(stderr, "nebulights: out of memory\n");
		exit(1);
	}
}

void scene_init(void)
{
	if (ready)
		return;

	config_load();
	{
		const char *e = getenv("NEBULIGHTS_STATS");
		stats_on = e && *e && *e != '0';
	}
	probe_hdr();
	if (EXPOSURE_CFG > 0.0f)
		EXPOSURE = EXPOSURE_CFG;

	static const char *a_rib[]  = { "aPos", "aCol", "aOff", "aSide" };
	static const char *a_pts[]  = { "aPos", "aCol", "aSize" };
	static const char *a_neb[]  = { "aPos", "aCol", "aUV" };
	static const char *a_quad[] = { "aPos" };

	P.rib    = link_program(VS_RIB,  FS_RIB,    a_rib,  4);
	P.pts    = link_program(VS_PTS,  FS_PTS,    a_pts,  3);
	P.mist   = link_program(VS_PTS,  FS_MIST,   a_pts,  3);
	P.neb    = link_program(VS_NEB,  FS_NEB,    a_neb,  3);
	P.bright = link_program(VS_QUAD, FS_BRIGHT, a_quad, 1);
	P.blur   = link_program(VS_QUAD, FS_BLUR,   a_quad, 1);
	P.pres   = link_program(VS_QUAD, FS_PRES,   a_quad, 1);

	P.r_mvp=glGetUniformLocation(P.rib,"uMVP");
	P.r_soft=glGetUniformLocation(P.rib,"uSoft");
	P.r_width=glGetUniformLocation(P.rib,"uWidth");
	P.r_gain=glGetUniformLocation(P.rib,"uGain");
	P.r_heat=glGetUniformLocation(P.rib,"uHeat");
	P.p_mvp=glGetUniformLocation(P.pts,"uMVP");
	P.p_scale=glGetUniformLocation(P.pts,"uScale");
	P.m_mvp=glGetUniformLocation(P.mist,"uMVP");
	P.m_scale=glGetUniformLocation(P.mist,"uScale");
	P.n_mvp=glGetUniformLocation(P.neb,"uMVP");
	P.b_src=glGetUniformLocation(P.bright,"uSrc");
	P.b_thresh=glGetUniformLocation(P.bright,"uThresh");
	P.b_scale=glGetUniformLocation(P.bright,"uScale");
	P.l_src=glGetUniformLocation(P.blur,"uSrc");
	P.l_dir=glGetUniformLocation(P.blur,"uDir");
	P.z_scene=glGetUniformLocation(P.pres,"uScene");
	P.z_b4=glGetUniformLocation(P.pres,"uB4");
	P.z_b8=glGetUniformLocation(P.pres,"uB8");
	P.z_bloom=glGetUniformLocation(P.pres,"uBloom");
	P.z_exp=glGetUniformLocation(P.pres,"uExposure");
	P.z_fringe=glGetUniformLocation(P.pres,"uFringe");
	P.z_res=glGetUniformLocation(P.pres,"uRes");
	P.z_fade=glGetUniformLocation(P.pres,"uFade");
	P.z_has4=glGetUniformLocation(P.pres,"uHas4");
	P.z_has8=glGetUniformLocation(P.pres,"uHas8");

	glGenBuffers(1,&vbo_rib);  glGenBuffers(1,&vbo_pts);
	glGenBuffers(1,&vbo_mist); glGenBuffers(1,&vbo_neb);
	glGenBuffers(1,&vbo_quad); glGenBuffers(1,&vbo_bg);

	static const GLfloat quad[] = { -1,-1, 3,-1, -1,3 };
	glBindBuffer(GL_ARRAY_BUFFER, vbo_quad);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

	alloc_buffers();
	build_world();

	/* stars never move — upload once */
	glBindBuffer(GL_ARRAY_BUFFER, vbo_bg);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)N_BG*7*sizeof(float),
	             BG, GL_STATIC_DRAW);

	ready = true;
}

void scene_fini(void)
{
	/* GL objects die with the context; just forget their names */
	ready = false;
	n_rt = 0;
	rt_w = rt_h = 0;
}

void scene_reset_clock(void)
{
	last_t = -1.0;
}

int scene_fps_cap(void)
{
	return FPS_CAP;
}

#define HALF_FOV 0.61f   /* half of the vertical field of view, radians */

void scene_draw(int width, int height, const float *view, const float *all,
                double t, float fade)
{
	float asp = (float)width / (float)(height > 0 ? height : 1);
	float whole[4] = { -asp, asp, -1.0f, 1.0f };
	if (!view) view = whole;
	if (!all)  all = view;

	float th = tanf(HALF_FOV), win[4];
	for (int i = 0; i < 4; i++) {
		win[i] = view[i] * th;
		CUR_ALL[i] = all[i] * th;
	}
	/* points keep the same size in the world on every monitor */
	float pt_scale = (float)height / (win[3] - win[2]) * 0.00587f;

	scene_init();
	make_targets(width, height);

	/* All outputs share one context and one world. Outputs drawn within
	   a few ms of each other reuse the step and the uploaded buffers
	   instead of simulating and uploading once per monitor. */
	bool step = last_t < 0.0 || t < last_t || t - last_t >= 0.004;
	float dt = (last_t < 0.0 || !step) ? 0.0f : (float)(t - last_t);
	if (step)
		last_t = t;
	if (dt > 0.05f) dt = 0.05f;

	/* The camera stays at the centre and only turns its gaze — if it
	   moved, the attractor would move along with it. */
	float ft = (float)t;
	float camR = 3.2f + 1.4f*sinf(ft*0.013f);
	float camA = ft*0.041f;
	float eye[3] = { cosf(camA)*camR, sinf(camA)*camR, 1.4f*sinf(ft*0.019f) };

	float yaw = ft*0.055f, pitch = 0.28f*sinf(ft*0.017f);
	float fwd[3] = { cosf(yaw)*cosf(pitch), sinf(yaw)*cosf(pitch), sinf(pitch) };
	float tgt[3] = { eye[0]+fwd[0]*10.0f, eye[1]+fwd[1]*10.0f, eye[2]+fwd[2]*10.0f };
	static const float up[3] = { 0.0f, 0.0f, 1.0f };

	float proj[16], vmat[16];
	m_frustum(proj, win, 0.15f, 300.0f);
	m_lookat(vmat, eye, tgt, up);
	m_mul(proj, vmat, VP);

	memcpy(CUR_VIEW, vmat, sizeof(vmat));
	memcpy(CAM_EYE, eye, sizeof(eye));
	vp_ready = true;

	if (dt > 0.0f) {
		int sub = 1 + (int)(dt / 0.008f);
		if (sub > 4) sub = 4;
		for (int i = 0; i < sub; i++)
			step_all(ft, dt / (float)sub);
		spark_step(dt);

		static float micro_t = 1.2f, far_t = 3.0f;

		far_t -= dt;
		if (far_t <= 0.0f) {
			far_t = 5.0f + frnd()*11.0f;
			float d[3] = { frnd2(), frnd2(), frnd2() };
			vnorm(d);
			float r = 52.0f + frnd()*46.0f;
			far_burst(d[0]*r, d[1]*r, d[2]*r, frnd());
		}

		micro_t -= dt;
		if (micro_t <= 0.0f) {
			micro_t = 0.4f + frnd()*1.6f;
			float d[3] = { frnd2(), frnd2(), frnd2() };
			vnorm(d);
			float r = 8.0f + frnd()*26.0f;
			const float *col = n_flyers ? FL[irnd(n_flyers)].col : up;
			micro_burst(d[0]*r, d[1]*r, d[2]*r, col);
		}
	}

	double t_cpu0 = stats_on ? now_ms() : 0.0;
	static double prev_frame_end = 0.0;
	double frame_gap = (stats_on && prev_frame_end > 0.0)
	                 ? t_cpu0 - prev_frame_end : 0.0;

	if (step) {
		build_points();
		build_ribbons(eye);
		build_nebulae(vmat, eye, proj[5], asp, ft);

		glBindBuffer(GL_ARRAY_BUFFER, vbo_neb);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)neb_v*8*sizeof(float),
		             NEBV, GL_STREAM_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_rib);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)rib_v*RIB_STRIDE*sizeof(float),
		             RIB, GL_STREAM_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_pts);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)pts_n*7*sizeof(float),
		             PTS, GL_STREAM_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_mist);
		glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)mst_n*7*sizeof(float),
		             MST, GL_STREAM_DRAW);
	}

	double t_cpu1 = stats_on ? now_ms() : 0.0;

	/* --- nebulae into their own buffer, at reduced resolution --- */
	if (neb_v > 0) {
		glBindFramebuffer(GL_FRAMEBUFFER, fbo_neb);
		glViewport(0, 0, b4w, b4h);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);

		glUseProgram(P.neb);
		glUniformMatrix4fv(P.n_mvp, 1, GL_FALSE, VP);
		glBindBuffer(GL_ARRAY_BUFFER, vbo_neb);
		for (int i = 0; i < 3; i++) glEnableVertexAttribArray((GLuint)i);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
		glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
		glDrawArrays(GL_TRIANGLES, 0, neb_v);
		for (int i = 0; i < 3; i++) glDisableVertexAttribArray((GLuint)i);
	}

	/* --- scene --- */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_scene);
	glViewport(0, 0, width, height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);

	/* nebulae come in ready-made from a separate, smaller buffer */
	if (neb_v > 0) {
		glUseProgram(P.bright);          /* threshold 0, scale 1 = plain copy */
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, tex_neb);
		glUniform1i(P.b_src, 0);
		glUniform1f(P.b_thresh, 0.0f);
		glUniform1f(P.b_scale, 1.0f);
		draw_quad();
	}

	draw_points(P.mist, P.m_mvp, P.m_scale, vbo_mist, mst_n, pt_scale);

	/* ribbons: geometry built once, drawn twice */
	draw_ribbons(1.6f, 2.6f, 0.26f, 0.0f);   /* wide glow */
	draw_ribbons(7.0f, 1.0f, 1.00f, 0.9f);   /* white-hot core */

	draw_points(P.pts, P.p_mvp, P.p_scale, vbo_bg, N_BG, pt_scale);
	draw_points(P.pts, P.p_mvp, P.p_scale, vbo_pts, pts_n, pt_scale);

	/* --- glow and composite --- */
	run_bloom();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glUseProgram(P.pres);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_scene);
	glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, t4a);
	glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, t8b);
	glUniform1i(P.z_scene, 0);
	glUniform1i(P.z_b4, 1);
	glUniform1i(P.z_b8, 2);
	glUniform1f(P.z_bloom, BLOOM_MUL);
	glUniform1f(P.z_exp, EXPOSURE);
	glUniform1f(P.z_fringe, FRINGE);
	glUniform1f(P.z_fade, fade);
	glUniform1f(P.z_has4, BLOOM_LEVELS >= 1 ? 1.0f : 0.0f);
	glUniform1f(P.z_has8, BLOOM_LEVELS >= 2 ? 1.0f : 0.0f);
	glUniform2f(P.z_res, (float)width, (float)height);
	draw_quad();
	glActiveTexture(GL_TEXTURE0);

	if (stats_on) {
		double t_end = now_ms();
		prev_frame_end = t_end;
		double frame = t_end - t_cpu0;
		stats_sum += frame;
		if (frame > stats_worst) stats_worst = frame;
		stats_frames++;

		if (stats_t0 == 0.0) stats_t0 = t_end;

		/* Blanked monitors or a screen lock stop frame callbacks. The
		   measuring window then stretches over several seconds and would
		   print a bogus 0.3 fps — start counting again. */
		if (frame_gap > 500.0) {
			stats_t0 = t_end;
			stats_frames = 0;
			stats_sum = 0.0;
			stats_worst = 0.0;
		} else if (t_end - stats_t0 >= 5000.0) {
			fprintf(stderr,
			    "nebulights: %.1f fps | frame avg %.2f ms, "
			    "worst %.2f ms | buffers %.2f ms | "
			    "ribbons %d verts, points %d, mist %d | "
			    "nebulae %d = %.0f screens (1/16 after downscale) | %s\n",
			    (double)stats_frames * 1000.0 / (t_end - stats_t0),
			    stats_sum / (double)stats_frames,
			    stats_worst,
			    t_cpu1 - t_cpu0,
			    rib_v, pts_n, mst_n, neb_v / 6, (double)neb_overdraw,
			    HDR ? "HDR" : "8-bit");
			stats_t0 = t_end;
			stats_frames = 0;
			stats_sum = 0.0;
			stats_worst = 0.0;
		}
	}
}
