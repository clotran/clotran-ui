// clotran_ui.c: clotran-ui's native half -- libui-ng's windows, a drawing
// area and a text field, behind clotran's package door (clotran's
// DynLoad.f90: a clotran_manifest and one NAME_shim(argc, argv) per
// routine). Plain C over libui-ng's own API, so it builds wherever
// libui-ng does: Cocoa, GTK 3, Win32.
//
// The three threads and what crosses between them (PLAN.md, the open
// questions):
//   - the process's first thread runs libui-ng (clotran_ui_main, handed
//     to it by clotran's fortran/main-thread*); every libui-ng call is
//     made there, posted by uiQueueMain and waited for (on_main);
//   - a clotran thread asks for windows and hands each area a DISPLAY
//     LIST -- drawing operations as doubles, plus their strings -- which
//     is copied here; the area's Draw callback replays the copy, so no C
//     callback ever calls into clotran and a collection never delays a
//     frame;
//   - mouse, key, resize, close and text events go into a queue that a
//     clotran thread drains (ui_next_event, blocking with a deadline).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ui.h"
#include "g3.h"

#define MAXWIN 64
#define NLAYER 2

typedef struct {
	double *ops; int nops;
	char **strs; int nstrs;
	char font[128];
} layer;

typedef struct {
	int used, open;
	uiWindow *w; uiArea *area; uiEntry *entry; uiLabel *label;
	layer layers[NLAYER];
	double aw, ah;            // the area's size at its last draw
	char text[4096];          // the text field's contents, as last changed
	void *g3;                 // the 3D view (g3.h), or NULL
	g3camera cam;             // its camera, turned by dragging, zoomed by scrolling
	double lx, ly;            // where a drag was last
	float *mesh[4];           // the 3D scene's arrays, as g3_mesh was last given them
	uint32_t *idx;
} win;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static win wins[MAXWIN + 1];
static int inited, running, nopen;
static const char *init_err;
static uiAreaHandler handler;

// ---- the event queue ------------------------------------------------------
enum { EV_MOVE = 1, EV_DOWN, EV_UP, EV_KEY, EV_RESIZE, EV_CLOSED, EV_TEXT, EV_LEFT, EV_SCROLL };
typedef struct { int kind, win; double x, y; int a, b; } event;
#define QN 1024
static event q[QN];
static int qh, qn;

static void push(event e)
{
	pthread_mutex_lock(&mu);
	// a move replaces a move still waiting for the same window: the reader
	// wants where the mouse is, not where it was
	int last = (qh + qn - 1) % QN;
	if (qn > 0 && e.kind == EV_MOVE && q[last].kind == EV_MOVE && q[last].win == e.win)
		q[last] = e;
	else if (qn < QN) { q[(qh + qn) % QN] = e; qn++; }
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

// ---- running on the main thread -------------------------------------------
static int pending;   // jobs queued and not yet run
static int made;      // windows made in this run of the loop
typedef struct { void (*f)(void *); void *arg; int done; } job;

static void run_job(void *p)
{
	job *j = p;
	j->f(j->arg);
	pthread_mutex_lock(&mu);
	j->done = 1;
	pending--;   // here, on the main thread, so the loop sees it before it next looks
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

// f(arg) on the main thread; returns 1 once it has run, or 0 at once when
// the loop is not running (it stops when its last window has closed and
// no job is pending -- decided under the same lock, so no job is lost)
static int on_main(void (*f)(void *), void *arg)
{
	job j = { f, arg, 0 };
	pthread_mutex_lock(&mu);
	if (!running) { pthread_mutex_unlock(&mu); return 0; }
	pending++;
	pthread_mutex_unlock(&mu);
	uiQueueMain(run_job, &j);
	pthread_mutex_lock(&mu);
	while (!j.done) pthread_cond_wait(&cv, &mu);
	pthread_mutex_unlock(&mu);
	return 1;
}

#if defined(__APPLE__) || defined(_WIN32)
extern void clotran_ui_front(uiWindow *);   // darwin.m, windows.c
#else
static void clotran_ui_front(uiWindow *w) {}
#endif

#ifdef __APPLE__
extern void clotran_ui_wake(void);
static void wake(void *p) { clotran_ui_wake(); }
#else
static void wake(void *p) {}   // GTK and Win32 return from a step for a queued function
#endif

// The function clotran hands the main thread: libui-ng initialised (once),
// then its event loop until the last window of this run has closed with
// nothing pending; the next window hands it to the main thread again.
void clotran_ui_main(void)
{
	if (!inited) {
		uiInitOptions o;
		memset(&o, 0, sizeof o);
		init_err = uiInit(&o);
		if (!init_err) uiMainSteps();
	}
	pthread_mutex_lock(&mu);
	inited = 1;
	running = init_err == NULL;
	made = 0;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	if (init_err) return;
	for (;;) {
		int more = uiMainStep(1);
		pthread_mutex_lock(&mu);
		if (!more || (made > 0 && nopen == 0 && pending == 0)) {
			running = 0;
			pthread_cond_broadcast(&cv);
			pthread_mutex_unlock(&mu);
			break;
		}
		pthread_mutex_unlock(&mu);
	}
}

// ---- drawing a display list -----------------------------------------------
// Each operation is an opcode and its operands, all doubles, colours as
// r g b a in 0..1:
//   1 rect      x y w h  r g b a                       filled
//   2 polyline  n width  r g b a  x1 y1 .. xn yn       stroked, round joins
//   3 polygon   n  fr fg fb fa  width sr sg sb sa  x1 y1 .. xn yn
//                                                      filled, then stroked if width > 0
//   4 text      x y anchor(0 start, 1 middle, 2 end) size italic r g b a index
//               (x, y the baseline's start, as SVG's; index into the strings)
static void brush(uiDrawBrush *b, const double *c)
{
	memset(b, 0, sizeof *b);
	b->Type = uiDrawBrushTypeSolid;
	b->R = c[0]; b->G = c[1]; b->B = c[2]; b->A = c[3];
}

static void path_of(uiDrawPath *p, int n, const double *xy)
{
	uiDrawPathNewFigure(p, xy[0], xy[1]);
	for (int i = 1; i < n; i++) uiDrawPathLineTo(p, xy[2 * i], xy[2 * i + 1]);
}

static void draw_text(uiDrawContext *c, const layer *L, const double *o)
{
	int idx = (int) o[9];
	if (idx < 0 || idx >= L->nstrs) return;
	uiAttributedString *s = uiNewAttributedString(L->strs[idx]);
	size_t len = strlen(L->strs[idx]);
	uiAttribute *col = uiNewColorAttribute(o[5], o[6], o[7], o[8]);
	uiAttributedStringSetAttribute(s, col, 0, len);
	uiFontDescriptor fd;
	fd.Family = (char *) (L->font[0] ? L->font : "Georgia");
	fd.Size = o[3];
	fd.Weight = uiTextWeightNormal;
	fd.Italic = o[4] != 0 ? uiTextItalicItalic : uiTextItalicNormal;
	fd.Stretch = uiTextStretchNormal;
	uiDrawTextLayoutParams lp = { s, &fd, -1, uiDrawTextAlignLeft };
	uiDrawTextLayout *tl = uiDrawNewTextLayout(&lp);
	double w, h;
	uiDrawTextLayoutExtents(tl, &w, &h);
	double x = o[0] - (o[2] == 1 ? w / 2 : o[2] == 2 ? w : 0);
	// a layout is placed by its top; its first baseline sits about 0.78 of
	// its height below that for one line of a Latin font
	uiDrawText(c, tl, x, o[1] - 0.78 * h);
	uiDrawFreeTextLayout(tl);
	uiFreeAttributedString(s);
}

static void replay(uiDrawContext *c, const layer *L)
{
	const double *o = L->ops, *end = L->ops + L->nops;
	uiDrawBrush b;
	while (o < end) {
		int op = (int) o[0];
		o++;
		if (op == 1) {
			uiDrawPath *p = uiDrawNewPath(uiDrawFillModeWinding);
			uiDrawPathAddRectangle(p, o[0], o[1], o[2], o[3]);
			uiDrawPathEnd(p);
			brush(&b, o + 4);
			uiDrawFill(c, p, &b);
			uiDrawFreePath(p);
			o += 8;
		} else if (op == 2 || op == 3) {
			int n = (int) o[0];
			const double *xy = o + (op == 2 ? 6 : 10);
			if (n >= 2) {
				uiDrawPath *p = uiDrawNewPath(uiDrawFillModeWinding);
				path_of(p, n, xy);
				if (op == 3) uiDrawPathCloseFigure(p);
				uiDrawPathEnd(p);
				uiDrawStrokeParams sp;
				memset(&sp, 0, sizeof sp);
				sp.Cap = uiDrawLineCapRound;
				sp.Join = uiDrawLineJoinRound;
				sp.MiterLimit = uiDrawDefaultMiterLimit;
				if (op == 2) {
					sp.Thickness = o[1];
					brush(&b, o + 2);
					uiDrawStroke(c, p, &b, &sp);
				} else {
					brush(&b, o + 1);
					uiDrawFill(c, p, &b);
					if (o[5] > 0) {
						sp.Thickness = o[5];
						brush(&b, o + 6);
						uiDrawStroke(c, p, &b, &sp);
					}
				}
				uiDrawFreePath(p);
			}
			o = xy + 2 * n;
		} else if (op == 4) {
			draw_text(c, L, o);
			o += 10;
		} else
			break;   // an unknown opcode: the rest cannot be parsed
	}
}

static win *of_area(uiArea *a)
{
	// open windows only: a closed window's area is freed, and a new area may
	// be given its address -- matching the closed one drew its old picture
	// in the new window
	for (int i = 1; i <= MAXWIN; i++) if (wins[i].used && wins[i].open && wins[i].area == a) return &wins[i];
	return NULL;
}

static void render3d(void *p);

static void h_draw(uiAreaHandler *h, uiArea *a, uiAreaDrawParams *p)
{
	win *w = of_area(a);
	if (!w) return;
	if (p->AreaWidth != w->aw || p->AreaHeight != w->ah) {
		w->aw = p->AreaWidth; w->ah = p->AreaHeight;
		push((event) { EV_RESIZE, (int) (w - wins), p->AreaWidth, p->AreaHeight, 0, 0 });
	}
	pthread_mutex_lock(&mu);
	for (int i = 0; i < NLAYER; i++) replay(p->Context, &w->layers[i]);
	pthread_mutex_unlock(&mu);
	if (w->g3) render3d(w);   // a resize: the 3D view follows
}

static void render3d(void *p)
{
	win *w = p;
	if (!w->open || !w->g3) return;
	pthread_mutex_lock(&mu);
	g3camera c = w->cam;
	pthread_mutex_unlock(&mu);
	g3_render(w->g3, &c);
}

// A 3D view turns as it is dragged (half a degree a pixel) and zooms as
// it is scrolled, here in C: no trip through clotran between the mouse
// and the picture. Its events still go to clotran, as every window's do.
static void orbit(win *w, event e)
{
	if (!w->g3) return;
	pthread_mutex_lock(&mu);
	int again = 0;
	if (e.kind == EV_DOWN) { w->lx = e.x; w->ly = e.y; }
	else if (e.kind == EV_MOVE && e.b) {
		w->cam.az -= 0.5f * (float) (e.x - w->lx);
		w->cam.el += 0.5f * (float) (e.y - w->ly);
		if (w->cam.el > 89) w->cam.el = 89;
		if (w->cam.el < -89) w->cam.el = -89;
		w->lx = e.x; w->ly = e.y;
		again = 1;
	} else if (e.kind == EV_SCROLL) {
		w->cam.zoom *= expf(-0.05f * (float) e.y);
		if (w->cam.zoom < 0.1f) w->cam.zoom = 0.1f;
		if (w->cam.zoom > 20) w->cam.zoom = 20;
		again = 1;
	}
	pthread_mutex_unlock(&mu);
	if (again) uiQueueMain(render3d, w);
}

static void h_mouse(uiAreaHandler *h, uiArea *a, uiAreaMouseEvent *e)
{
	win *w = of_area(a);
	if (!w) return;
	int kind = e->Down ? EV_DOWN : e->Up ? EV_UP : EV_MOVE;
	event ev = { kind, (int) (w - wins), e->X, e->Y, e->Down ? e->Down : e->Up, kind == EV_MOVE ? (int) e->Held1To64 : e->Count };
	orbit(w, ev);
	push(ev);
}

static void h_crossed(uiAreaHandler *h, uiArea *a, int left)
{
	win *w = of_area(a);
	if (w && left) push((event) { EV_LEFT, (int) (w - wins), 0, 0, 0, 0 });
}

static void h_broken(uiAreaHandler *h, uiArea *a) {}

static int h_key(uiAreaHandler *h, uiArea *a, uiAreaKeyEvent *e)
{
	win *w = of_area(a);
	if (!w || e->Up) return 0;
	push((event) { EV_KEY, (int) (w - wins), 0, 0, (unsigned char) e->Key, e->ExtKey });
	return 1;
}

static int on_closing(uiWindow *uw, void *data)
{
	win *w = data;
	pthread_mutex_lock(&mu);
	w->open = 0;
	w->area = NULL;   // freed with the window; never matched again
	nopen--;
	int last = nopen == 0;
	pthread_mutex_unlock(&mu);
	push((event) { EV_CLOSED, (int) (w - wins), 0, 0, 0, 0 });
	if (last) uiQueueMain(wake, NULL);   // the loop looks again, and stops
	return 1;             // libui-ng destroys the window
}

static void on_changed(uiEntry *e, void *data)
{
	win *w = data;
	char *t = uiEntryText(e);
	pthread_mutex_lock(&mu);
	snprintf(w->text, sizeof w->text, "%s", t);
	pthread_mutex_unlock(&mu);
	uiFreeText(t);
	push((event) { EV_TEXT, (int) (w - wins), 0, 0, 0, 0 });
}

static int should_quit(void *data)
{
	// Cmd-Q, or the application menu's Quit: every window closed; the
	// program itself goes on (the REPL), as closing each would
	for (int i = 1; i <= MAXWIN; i++)
		if (wins[i].used && wins[i].open) {
			uiWindow *uw = wins[i].w;
			on_closing(uw, &wins[i]);
			uiControlDestroy(uiControl(uw));
		}
	return 0;
}

// ---- the routines ---------------------------------------------------------
typedef struct { const char *title; int w, h, kind, id; } mkwin;

static void make_window(void *p)
{
	mkwin *m = p;
	static int handler_set;
	if (!handler_set) {
		handler.Draw = h_draw; handler.MouseEvent = h_mouse; handler.MouseCrossed = h_crossed;
		handler.DragBroken = h_broken; handler.KeyEvent = h_key;
		uiOnShouldQuit(should_quit, NULL);
		handler_set = 1;
	}
	// the next free slot after the last one given: a closed window's number is
	// not given again at once, so its late events (closed) find no new window
	static int last_id;
	int id = 0;
	for (int k = 1; k <= MAXWIN; k++) {
		int i = (last_id + k - 1) % MAXWIN + 1;
		if (!wins[i].used || !wins[i].open) { id = i; break; }
	}
	if (id) last_id = id;
	if (!id) { m->id = 0; return; }
	win *w = &wins[id];
	for (int i = 0; i < NLAYER; i++) {
		free(w->layers[i].ops);
		for (int k = 0; k < w->layers[i].nstrs; k++) free(w->layers[i].strs[k]);
		free(w->layers[i].strs);
	}
	memset(w, 0, sizeof *w);
	w->used = 1;
	w->w = uiNewWindow(m->title, m->w, m->h, 0);
	w->area = uiNewArea(&handler);
	uiWindowSetMargined(w->w, 0);
	if (m->kind == 1) {
		uiBox *b = uiNewVerticalBox();
		uiBoxSetPadded(b, 1);
		w->entry = uiNewEntry();
		w->label = uiNewLabel("");
		uiEntryOnChanged(w->entry, on_changed, w);
		uiBoxAppend(b, uiControl(w->entry), 0);
		uiBoxAppend(b, uiControl(w->label), 0);
		uiBoxAppend(b, uiControl(w->area), 1);
		uiWindowSetMargined(w->w, 1);
		uiWindowSetChild(w->w, uiControl(b));
	} else
		uiWindowSetChild(w->w, uiControl(w->area));
	uiWindowOnClosing(w->w, on_closing, w);
	pthread_mutex_lock(&mu);   // open before it is shown: its first draw finds it
	w->open = 1;
	nopen++;
	made++;
	pthread_mutex_unlock(&mu);
	uiControlShow(uiControl(w->w));
	clotran_ui_front(w->w);   // above the terminal that started clotran
	m->id = id;
}

// ui_start(out ok): the event loop running on the main thread, waited for.
// clotran hands it clotran_ui_main first; this waits until it is up.
// ok 1 running, 0 libui-ng would not start, -1 it is not running (yet).
void ui_start_shim(int argc, void **argv)
{
	int *ok = argv[0];
	int ms = *(int *) argv[1];
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ms / 10 && !running && !(inited && init_err); i++) {
		pthread_mutex_unlock(&mu);
		struct timespec ts = { 0, 10000000 };
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&mu);
	}
	*ok = running ? 1 : (inited && init_err) ? 0 : -1;
	pthread_mutex_unlock(&mu);
}

void ui_running_shim(int argc, void **argv)
{
	pthread_mutex_lock(&mu);
	*(int *) argv[0] = running;
	pthread_mutex_unlock(&mu);
}

void ui_window_shim(int argc, void **argv)
{
	mkwin m = { argv[0], *(int *) argv[1], *(int *) argv[2], *(int *) argv[3], 0 };
	if (!on_main(make_window, &m)) m.id = -1;   // the loop is not running: start it, ask again
	*(int *) argv[4] = m.id;
}

static void redraw(void *p)
{
	win *w = p;
	if (w->open) uiAreaQueueRedrawAll(w->area);
}

static win *live(int id)
{
	return id >= 1 && id <= MAXWIN && wins[id].used && wins[id].open ? &wins[id] : NULL;
}

// ui_scene(id, layer, ops, n, strings, font): the layer's display list
// replaced by a copy of ops[0..n) and the strings (separated by \x1f),
// and the area redrawn
void ui_scene_shim(int argc, void **argv)
{
	int id = *(int *) argv[0], li = *(int *) argv[1];
	const double *ops = argv[2];
	int n = *(int *) argv[4];
	const char *strs = argv[5], *font = argv[6];
	win *w = live(id);
	if (!w || li < 0 || li >= NLAYER) return;
	double *copy = malloc(sizeof(double) * (n > 0 ? n : 1));
	memcpy(copy, ops, sizeof(double) * n);
	int ns = 0;
	for (const char *s = strs; *s; s++) if (*s == '\x1f') ns++;
	ns += *strs ? 1 : 0;
	char **sv = calloc(ns ? ns : 1, sizeof(char *));
	const char *s = strs;
	for (int i = 0; i < ns; i++) {
		const char *e = strchr(s, '\x1f');
		size_t len = e ? (size_t) (e - s) : strlen(s);
		sv[i] = malloc(len + 1);
		memcpy(sv[i], s, len);
		sv[i][len] = 0;
		s = e ? e + 1 : s + len;
	}
	pthread_mutex_lock(&mu);
	layer *L = &w->layers[li];
	free(L->ops);
	for (int i = 0; i < L->nstrs; i++) free(L->strs[i]);
	free(L->strs);
	L->ops = copy; L->nops = n; L->strs = sv; L->nstrs = ns;
	snprintf(L->font, sizeof L->font, "%s", font);
	pthread_mutex_unlock(&mu);
	uiQueueMain(redraw, w);
}

typedef struct { win *w; const char *s; } settext;
static void set_label(void *p) { settext *t = p; if (t->w->open && t->w->label) uiLabelSetText(t->w->label, t->s); }
static void set_entry(void *p)
{
	settext *t = p;
	if (!t->w->open || !t->w->entry) return;
	uiEntrySetText(t->w->entry, t->s);
	pthread_mutex_lock(&mu);   // as typing would have
	snprintf(t->w->text, sizeof t->w->text, "%s", t->s);
	pthread_mutex_unlock(&mu);
}
static void set_title(void *p) { settext *t = p; if (t->w->open) uiWindowSetTitle(t->w->w, t->s); }

// ui_text(id, which, text): 0 the label, 1 the text field, 2 the title
void ui_text_shim(int argc, void **argv)
{
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	settext t = { w, argv[2] };
	int which = *(int *) argv[1];
	on_main(which == 0 ? set_label : which == 1 ? set_entry : set_title, &t);
}

// ui_entry_text(id, buf, out n): the text field's contents as bytes
// in buf (int4 per byte), n of them
void ui_entry_text_shim(int argc, void **argv)
{
	int id = *(int *) argv[0];
	int *buf = argv[1];
	int cap = *(int *) argv[2];   // the array's extent
	int *n = argv[3];
	*n = 0;
	if (id < 1 || id > MAXWIN || !wins[id].used) return;
	pthread_mutex_lock(&mu);
	const unsigned char *t = (const unsigned char *) wins[id].text;
	int i = 0;
	for (; t[i] && i < cap; i++) buf[i] = t[i];
	pthread_mutex_unlock(&mu);
	*n = i;
}

// ui_next_event(ms, out kind, out win, out x, out y, out a, out b): the
// oldest event, waiting at most ms; kind 0 when none came
void ui_next_event_shim(int argc, void **argv)
{
	int ms = *(int *) argv[0];
	struct timespec dl;
	clock_gettime(CLOCK_REALTIME, &dl);
	dl.tv_sec += ms / 1000;
	dl.tv_nsec += (long) (ms % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
	pthread_mutex_lock(&mu);
	while (qn == 0)
		if (pthread_cond_timedwait(&cv, &mu, &dl) != 0) break;
	event e = { 0, 0, 0, 0, 0, 0 };
	if (qn > 0) { e = q[qh]; qh = (qh + 1) % QN; qn--; }
	pthread_mutex_unlock(&mu);
	*(int *) argv[1] = e.kind; *(int *) argv[2] = e.win;
	*(double *) argv[3] = e.x; *(double *) argv[4] = e.y;
	*(int *) argv[5] = e.a; *(int *) argv[6] = e.b;
}

static void close_win(void *p)
{
	win *w = p;
	if (!w->open) return;
	on_closing(w->w, w);
	uiControlDestroy(uiControl(w->w));
}

void ui_close_shim(int argc, void **argv)
{
	win *w = live(*(int *) argv[0]);
	if (w) on_main(close_win, w);
}

// ui_front(id): an open window brought to the front, as a new one is
static void front_win(void *p)
{
	win *w = p;
	if (w->open) clotran_ui_front(w->w);
}

void ui_front_shim(int argc, void **argv)
{
	win *w = live(*(int *) argv[0]);
	if (w) on_main(front_win, w);
}

// ui_open(id, out open): 1 while the window is open; 0 from the moment it
// closes (close, or its close button), before its :closed event is handled
void ui_open_shim(int argc, void **argv)
{
	pthread_mutex_lock(&mu);
	*(int *) argv[1] = live(*(int *) argv[0]) != NULL;
	pthread_mutex_unlock(&mu);
}

// ui_open_count(out n): windows open now
void ui_open_count_shim(int argc, void **argv)
{
	pthread_mutex_lock(&mu);
	*(int *) argv[0] = nopen;
	pthread_mutex_unlock(&mu);
}

// ui_wait(ms, out n): wait until no window is open, at most ms (-1: no
// limit); n the windows still open
void ui_wait_shim(int argc, void **argv)
{
	int ms = *(int *) argv[0];
	pthread_mutex_lock(&mu);
	for (int t = 0; nopen > 0 && (ms < 0 || t < ms); t += 50) {
		struct timespec dl;
		clock_gettime(CLOCK_REALTIME, &dl);
		dl.tv_nsec += 50000000L;
		if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
		pthread_cond_timedwait(&cv, &mu, &dl);
	}
	*(int *) argv[1] = nopen;
	pthread_mutex_unlock(&mu);
}

#ifdef __APPLE__
// ui_snapshot(id, w, h, path, out ok): the window's display list drawn by
// the same replay into an offscreen bitmap of w by h, written as PNG --
// what a test looks at, with no screen needed. macOS only, for now.
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
extern uiDrawContext *uiprivDrawNewContext(CGContextRef, CGFloat);
extern void uiprivDrawFreeContext(uiDrawContext *);
static int snapshot(win *w, int W, int H, const char *path)
{
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGContextRef c = CGBitmapContextCreate(NULL, W, H, 8, 0, cs, (CGBitmapInfo) kCGImageAlphaPremultipliedLast);
	CGContextTranslateCTM(c, 0, H);
	CGContextScaleCTM(c, 1, -1);   // flipped, as libui-ng's area view is
	uiDrawContext *dc = uiprivDrawNewContext(c, H);
	pthread_mutex_lock(&mu);
	for (int i = 0; i < NLAYER; i++) replay(dc, &w->layers[i]);
	pthread_mutex_unlock(&mu);
	uiprivDrawFreeContext(dc);
	CGImageRef img = CGBitmapContextCreateImage(c);
	CFStringRef ps = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
	CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
	CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
	int ok = 0;
	if (d) { CGImageDestinationAddImage(d, img, NULL); ok = CGImageDestinationFinalize(d); CFRelease(d); }
	CFRelease(url); CFRelease(ps); CGImageRelease(img); CGContextRelease(c); CGColorSpaceRelease(cs);
	return ok;
}
typedef struct { win *w; int W, H; const char *path; int ok; } snap;
static void snap_main(void *p) { snap *s = p; s->ok = snapshot(s->w, s->W, s->H, s->path); }
#endif

#ifdef _WIN32
extern int clotran_ui_snapshot_win(int W, int H, const char *path, void (*draw)(uiDrawContext *, void *), void *arg);
static void wsnap_draw(uiDrawContext *c, void *p)
{
	win *w = p;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < NLAYER; i++) replay(c, &w->layers[i]);
	pthread_mutex_unlock(&mu);
}
typedef struct { win *w; int W, H; const char *path; int ok; } wsnap;
static void wsnap_main(void *p) { wsnap *s = p; s->ok = clotran_ui_snapshot_win(s->W, s->H, s->path, wsnap_draw, s->w); }
#endif

void ui_snapshot_shim(int argc, void **argv)
{
	int *ok = argv[4];
	*ok = 0;
#ifdef __APPLE__
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	snap s = { w, *(int *) argv[1], *(int *) argv[2], argv[3], 0 };
	on_main(snap_main, &s);   // text layout wants the main thread
	*ok = s.ok;
#elif defined(_WIN32)
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	wsnap s = { w, *(int *) argv[1], *(int *) argv[2], argv[3], 0 };
	on_main(wsnap_main, &s);
	*ok = s.ok;
#endif
}

#if defined(__APPLE__) || defined(_WIN32)
extern int clotran_ui_capture(uiWindow *, const char *);
typedef struct { win *w; const char *path; int ok; } cap;
static void cap_main(void *p)
{
	cap *c = p;
	if (c->w->g3) {   // a 3D view: the frame it shows, read back from the GPU
		pthread_mutex_lock(&mu);
		g3camera k = c->w->cam;
		pthread_mutex_unlock(&mu);
		c->ok = g3_capture(c->w->g3, &k, c->path);
	} else
		c->ok = clotran_ui_capture(c->w->w, c->path);
}
#endif

// ui_capture(id, path, out ok): the window as it is on screen -- its frame,
// widgets and drawing -- written as PNG, read from the window itself
void ui_capture_shim(int argc, void **argv)
{
	int *ok = argv[2];
	*ok = 0;
#if defined(__APPLE__) || defined(_WIN32)
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	cap c = { w, argv[1], 0 };
	on_main(cap_main, &c);
	*ok = c.ok;
#endif
}

// ui_inject(id, kind, x, y, a, b): an event as if the person made it --
// how a test drives trace and zoom with no mouse
void ui_inject_shim(int argc, void **argv)
{
	event e = { *(int *) argv[1], *(int *) argv[0], *(double *) argv[2], *(double *) argv[3], *(int *) argv[4], *(int *) argv[5] };
	win *w = live(e.win);
	if (w) orbit(w, e);
	push(e);
}

// ---- the 3D view ------------------------------------------------------------
#if !defined(__APPLE__) && !defined(CLOTRAN_VULKAN)
// no GPU backend built here: the 3D routines answer that there is none
const char *clotran_ui_g3_error = "this build of clotran-ui has no 3D view for this system";
void *g3_attach(uiArea *a) { return NULL; }
void g3_mesh(void *h, const g3scene *s) {}
void g3_render(void *h, const g3camera *c) {}
int g3_snapshot(void *h, const g3camera *c, int W, int H, const char *path) { return 0; }
int g3_capture(void *h, const g3camera *c, const char *path) { return 0; }
#endif

static void attach3d(void *p)
{
	win *w = p;
	if (!w->g3) w->g3 = g3_attach(w->area);
#ifdef __APPLE__
	extern void clotran_ui_watch_scroll(void);
	clotran_ui_watch_scroll();
#elif defined(_WIN32) && defined(CLOTRAN_VULKAN)
	extern void clotran_ui_watch_scroll_win(void *window_hwnd);
	if (w->g3) clotran_ui_watch_scroll_win((void *) uiControlHandle(uiControl(w->w)));
#endif
	w->cam = (g3camera) { -35, 28, 1 };   // clotran.plot.surface's view
}

// ui_view3d(id, out ok): the window's drawing area becomes a 3D view
void ui_view3d_shim(int argc, void **argv)
{
	int *ok = argv[1];
	*ok = 0;
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	on_main(attach3d, w);
	*ok = w->g3 != NULL;
}

typedef struct { win *w; g3scene s; } meshjob;
static void mesh_main(void *p)
{
	meshjob *m = p;
	g3_mesh(m->w->g3, &m->s);
	render3d(m->w);
}

// ui_scene3d(id, verts, nv, tris, nt, lines, nl, r, g, b): the 3D view's
// mesh -- nv vertices [x y z r g b], nt triangles of vertex indices, nl
// lines [x y z r g b x y z r g b] -- on a ground of colour r g b
void ui_scene3d_shim(int argc, void **argv)
{
	win *w = live(*(int *) argv[0]);
	if (!w || !w->g3) return;
	const double *v = argv[1], *l = argv[7];
	const int *t = argv[4];
	int nv = *(int *) argv[3], nt = *(int *) argv[6], nl = *(int *) argv[9];
	float *pos = malloc(sizeof(float) * 3 * (nv + 1)), *col = malloc(sizeof(float) * 3 * (nv + 1));
	float *lpos = malloc(sizeof(float) * 6 * (nl + 1)), *lcol = malloc(sizeof(float) * 6 * (nl + 1));
	uint32_t *idx = malloc(sizeof(uint32_t) * 3 * (nt + 1));
	for (int i = 0; i < nv; i++)
		for (int k = 0; k < 3; k++) { pos[3 * i + k] = (float) v[6 * i + k]; col[3 * i + k] = (float) v[6 * i + 3 + k]; }
	for (int i = 0; i < 3 * nt; i++) idx[i] = (uint32_t) (t[i] >= 0 && t[i] < nv ? t[i] : 0);
	for (int i = 0; i < 2 * nl; i++)
		for (int k = 0; k < 3; k++) { lpos[3 * i + k] = (float) l[6 * i + k]; lcol[3 * i + k] = (float) l[6 * i + 3 + k]; }
	meshjob m = { w, { pos, col, nv, idx, nt, lpos, lcol, 2 * nl,
	                   { (float) *(double *) argv[10], (float) *(double *) argv[11], (float) *(double *) argv[12] } } };
	on_main(mesh_main, &m);
	// the backend has copied them to the GPU
	free(pos); free(col); free(lpos); free(lcol); free(idx);
}

// ui_camera3d(id, set, az, el, zoom, out az, out el, out zoom): the
// camera, set first when set is 1
void ui_camera3d_shim(int argc, void **argv)
{
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	pthread_mutex_lock(&mu);
	if (*(int *) argv[1])
		w->cam = (g3camera) { (float) *(double *) argv[2], (float) *(double *) argv[3], (float) *(double *) argv[4] };
	*(double *) argv[5] = w->cam.az; *(double *) argv[6] = w->cam.el; *(double *) argv[7] = w->cam.zoom;
	pthread_mutex_unlock(&mu);
	if (*(int *) argv[1] && w->g3) uiQueueMain(render3d, w);
}

typedef struct { win *w; int W, H; const char *path; int ok; } snap3;
static void snap3_main(void *p)
{
	snap3 *s = p;
	pthread_mutex_lock(&mu);
	g3camera c = s->w->cam;
	pthread_mutex_unlock(&mu);
	s->ok = g3_snapshot(s->w->g3, &c, s->W, s->H, s->path);
}

// ui_snapshot3d(id, w, h, path, out ok): the 3D view drawn offscreen at w
// by h and written as PNG
void ui_snapshot3d_shim(int argc, void **argv)
{
	int *ok = argv[4];
	*ok = 0;
	win *w = live(*(int *) argv[0]);
	if (!w || !w->g3) return;
	snap3 s = { w, *(int *) argv[1], *(int *) argv[2], argv[3], 0 };
	on_main(snap3_main, &s);
	*ok = s.ok;
}

// A scroll over a window (darwin.m's monitor; vulkan.c's on Windows): its
// 3D view zooms, and clotran hears of it
void clotran_ui_on_scroll(void *native_window, double dy)
{
	for (int i = 1; i <= MAXWIN; i++)
		if (wins[i].used && wins[i].open && uiControlHandle(uiControl(wins[i].w)) == (uintptr_t) native_window) {
			event e = { EV_SCROLL, i, 0, dy, 0, 0 };
			orbit(&wins[i], e);
			push(e);
		}
}

// ui_area_size(id, out w, out h): the drawing area's size at its last
// draw -- what a picture for it must be drawn at
void ui_area_size_shim(int argc, void **argv)
{
	*(double *) argv[1] = 0;
	*(double *) argv[2] = 0;
	win *w = live(*(int *) argv[0]);
	if (!w) return;
	pthread_mutex_lock(&mu);
	*(double *) argv[1] = w->aw;
	*(double *) argv[2] = w->ah;
	pthread_mutex_unlock(&mu);
}

// ui_g3_error(buf, out n): why there is no 3D view, as bytes in buf
extern const char *clotran_ui_g3_error;
void ui_g3_error_shim(int argc, void **argv)
{
	int *buf = argv[0], cap = *(int *) argv[1], *n = argv[2];
	int i = 0;
	for (; clotran_ui_g3_error[i] && i < cap; i++) buf[i] = (unsigned char) clotran_ui_g3_error[i];
	*n = i;
}

const char *clotran_manifest(void)
{
	return "{:package \"clotran.ui.native\" :routines ["
		"{:name \"start\" :symbol \"ui_start_shim\" :args [{:name \"ok\" :type :int4 :intent :out :rank 0} {:name \"ms\" :type :int4 :intent :in :rank 0}]}"
		"{:name \"running\" :symbol \"ui_running_shim\" :args [{:name \"running\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"window\" :symbol \"ui_window_shim\" :args [{:name \"title\" :type :string :intent :in :rank 0} {:name \"w\" :type :int4 :intent :in :rank 0} {:name \"h\" :type :int4 :intent :in :rank 0} {:name \"kind\" :type :int4 :intent :in :rank 0} {:name \"id\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"scene\" :symbol \"ui_scene_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"layer\" :type :int4 :intent :in :rank 0} {:name \"ops\" :type :real8 :intent :in :rank 1} {:name \"n\" :type :int4 :intent :in :rank 0} {:name \"strings\" :type :string :intent :in :rank 0} {:name \"font\" :type :string :intent :in :rank 0}]}"
		"{:name \"text\" :symbol \"ui_text_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"which\" :type :int4 :intent :in :rank 0} {:name \"text\" :type :string :intent :in :rank 0}]}"
		"{:name \"entry-text\" :symbol \"ui_entry_text_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"buf\" :type :int4 :intent :inout :rank 1} {:name \"n\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"next-event\" :symbol \"ui_next_event_shim\" :args [{:name \"ms\" :type :int4 :intent :in :rank 0} {:name \"kind\" :type :int4 :intent :out :rank 0} {:name \"win\" :type :int4 :intent :out :rank 0} {:name \"x\" :type :real8 :intent :out :rank 0} {:name \"y\" :type :real8 :intent :out :rank 0} {:name \"a\" :type :int4 :intent :out :rank 0} {:name \"b\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"close\" :symbol \"ui_close_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0}]}"
		"{:name \"front\" :symbol \"ui_front_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0}]}"
		"{:name \"open\" :symbol \"ui_open_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"open\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"open-count\" :symbol \"ui_open_count_shim\" :args [{:name \"n\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"wait\" :symbol \"ui_wait_shim\" :args [{:name \"ms\" :type :int4 :intent :in :rank 0} {:name \"n\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"snapshot\" :symbol \"ui_snapshot_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"w\" :type :int4 :intent :in :rank 0} {:name \"h\" :type :int4 :intent :in :rank 0} {:name \"path\" :type :string :intent :in :rank 0} {:name \"ok\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"capture\" :symbol \"ui_capture_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"path\" :type :string :intent :in :rank 0} {:name \"ok\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"inject\" :symbol \"ui_inject_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"kind\" :type :int4 :intent :in :rank 0} {:name \"x\" :type :real8 :intent :in :rank 0} {:name \"y\" :type :real8 :intent :in :rank 0} {:name \"a\" :type :int4 :intent :in :rank 0} {:name \"b\" :type :int4 :intent :in :rank 0}]}"
		"{:name \"view3d\" :symbol \"ui_view3d_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"ok\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"scene3d\" :symbol \"ui_scene3d_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"verts\" :type :real8 :intent :in :rank 1} {:name \"nv\" :type :int4 :intent :in :rank 0} {:name \"tris\" :type :int4 :intent :in :rank 1} {:name \"nt\" :type :int4 :intent :in :rank 0} {:name \"lines\" :type :real8 :intent :in :rank 1} {:name \"nl\" :type :int4 :intent :in :rank 0} {:name \"r\" :type :real8 :intent :in :rank 0} {:name \"g\" :type :real8 :intent :in :rank 0} {:name \"b\" :type :real8 :intent :in :rank 0}]}"
		"{:name \"camera3d\" :symbol \"ui_camera3d_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"set\" :type :int4 :intent :in :rank 0} {:name \"az\" :type :real8 :intent :in :rank 0} {:name \"el\" :type :real8 :intent :in :rank 0} {:name \"zoom\" :type :real8 :intent :in :rank 0} {:name \"azimuth\" :type :real8 :intent :out :rank 0} {:name \"elevation\" :type :real8 :intent :out :rank 0} {:name \"scale\" :type :real8 :intent :out :rank 0}]}"
		"{:name \"snapshot3d\" :symbol \"ui_snapshot3d_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"w\" :type :int4 :intent :in :rank 0} {:name \"h\" :type :int4 :intent :in :rank 0} {:name \"path\" :type :string :intent :in :rank 0} {:name \"ok\" :type :int4 :intent :out :rank 0}]}"
		"{:name \"area-size\" :symbol \"ui_area_size_shim\" :args [{:name \"id\" :type :int4 :intent :in :rank 0} {:name \"width\" :type :real8 :intent :out :rank 0} {:name \"height\" :type :real8 :intent :out :rank 0}]}"
		"{:name \"g3-error\" :symbol \"ui_g3_error_shim\" :args [{:name \"buf\" :type :int4 :intent :inout :rank 1} {:name \"n\" :type :int4 :intent :out :rank 0}]}"
		"]}";
}
