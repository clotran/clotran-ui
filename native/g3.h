// g3.h: the 3D view's interface, one per GPU (metal.m, vulkan.c), and the
// camera both use, so the two draw the same picture.
#ifndef CLOTRAN_G3_H
#define CLOTRAN_G3_H
#include <stdint.h>
#include <math.h>
#include "ui.h"
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// A mesh in the unit box (x, y in -1/2..1/2, z already scaled to the box's
// height): triangles with a colour per vertex, lit as clotran-plot's SVG
// surfaces are (0.35 + 0.65 cos to a light from the left, front, above),
// and lines in their own colours.
typedef struct {
	const float *pos, *col; int nv;
	const uint32_t *idx; int nt;
	const float *lpos, *lcol; int nlv;
	float bg[3];
} g3scene;

// Orthographic, as clotran.plot.surface/rotate: turned by the azimuth
// about the vertical, looked down on by the elevation (degrees); zoom 1
// fits the box to the view.
typedef struct { float az, el, zoom; } g3camera;

// u: azimuth and elevation in radians, the x and y scale to clip space,
// the light's direction, padding
static inline void g3_uniforms(const g3camera *c, int W, int H, float u[8])
{
	float a = c->az * (float) M_PI / 180, e = c->el * (float) M_PI / 180;
	// the box's diagonal, about 1.5, fits at any turn, so turning does not resize it
	float s = 0.6f * (W < H ? W : H) * c->zoom;
	float l = sqrtf(1.0f + 2.25f + 6.25f);
	u[0] = a; u[1] = e; u[2] = 2 * s / W; u[3] = 2 * s / H;
	u[4] = -1.0f / l; u[5] = -1.5f / l; u[6] = 2.5f / l; u[7] = 0;
}

void *g3_attach(uiArea *a);
void g3_mesh(void *h, const g3scene *s);
void g3_render(void *h, const g3camera *c);
int g3_snapshot(void *h, const g3camera *c, int W, int H, const char *path);
int g3_capture(void *h, const g3camera *c, const char *path);   // the shown frame, read back
#endif
