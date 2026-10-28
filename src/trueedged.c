/* trueedged — mouse crossing between monitors where they physically meet (X11).
 *
 * X moves the cursor between monitors where their *pixel* edges meet. With
 * monitors of different size, density or height that is not where they meet
 * physically, and parts of an edge lead nowhere at all. trueedged fixes that:
 *
 *  - every pixel edge shared by two monitors gets an XFixes pointer barrier,
 *    so X never crosses on its own;
 *  - XInput2 raw motion tells us when the cursor sits on a monitor edge and
 *    the mouse keeps pushing outwards;
 *  - the exit point is converted to millimetres using the physical layout
 *    (layout.conf), carried straight across to the monitor that is really
 *    there, and the cursor is warped to the matching pixel.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>

#define MAX_MON 16
#define MAX_BARRIERS 64
/* A monitor still counts as "beyond" an edge if it overlaps it by this much. */
#define EDGE_TOL_MM 5.0

typedef struct {
    char name[64];
    int primary;
    int px[2], ps[2];    /* pixel origin and size, [0] = x, [1] = y */
    double mx[2], ms[2]; /* physical origin and size in mm */
    int placed;          /* physical position known */
} Monitor;

static Monitor mons[MAX_MON];
static int nmon;
static int jump = 1; /* exit point with no monitor beyond it: go to the nearest one */
static PointerBarrier barriers[MAX_BARRIERS];
static int nbarriers;
static char config_path[512];
static volatile sig_atomic_t reload_requested, quit_requested;

static int x_error(Display *dpy, XErrorEvent *e)
{
    char msg[128];
    XGetErrorText(dpy, e->error_code, msg, sizeof msg);
    fprintf(stderr, "trueedged: X error: %s (request %d.%d)\n", msg, e->request_code, e->minor_code);
    return 0;
}

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        reload_requested = 1;
    else
        quit_requested = 1;
}

static void detect_monitors(Display *dpy, Window root)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    RROutput primary = XRRGetOutputPrimary(dpy, root);

    nmon = 0;
    for (int i = 0; i < res->noutput && nmon < MAX_MON; i++) {
        XRROutputInfo *out = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (out->connection == RR_Connected && out->crtc) {
            XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, out->crtc);
            Monitor *m = &mons[nmon++];
            memset(m, 0, sizeof *m);
            snprintf(m->name, sizeof m->name, "%s", out->name);
            m->primary = res->outputs[i] == primary;
            m->px[0] = crtc->x;
            m->px[1] = crtc->y;
            m->ps[0] = crtc->width;
            m->ps[1] = crtc->height;
            /* EDID size is for the unrotated panel. */
            double w = out->mm_width, h = out->mm_height;
            if (crtc->rotation & (RR_Rotate_90 | RR_Rotate_270)) {
                double t = w;
                w = h;
                h = t;
            }
            if (w <= 0 || h <= 0) { /* no EDID size: assume 96 DPI */
                w = m->ps[0] * 25.4 / 96;
                h = m->ps[1] * 25.4 / 96;
            }
            m->ms[0] = w;
            m->ms[1] = h;
            XRRFreeCrtcInfo(crtc);
        }
        XRRFreeOutputInfo(out);
    }
    XRRFreeScreenResources(res);
}

static void read_config(void)
{
    FILE *f = fopen(config_path, "r");
    char line[256], name[64];
    double x, y, w, h;
    int v;

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "jump %d", &v) == 1) {
            jump = v != 0;
        } else if (sscanf(line, "monitor %63s %lf %lf %lf %lf", name, &x, &y, &w, &h) == 5) {
            for (int i = 0; i < nmon; i++) {
                if (strcmp(mons[i].name, name) || w <= 0 || h <= 0)
                    continue;
                mons[i].mx[0] = x;
                mons[i].mx[1] = y;
                mons[i].ms[0] = w;
                mons[i].ms[1] = h;
                mons[i].placed = 1;
            }
        }
    }
    fclose(f);
}

static void place_at_pixel_position(Monitor *m)
{
    for (int a = 0; a < 2; a++)
        m->mx[a] = m->px[a] * m->ms[a] / m->ps[a];
    m->placed = 1;
}

/* Gives every monitor without a configured position a default one: touching
 * an already placed pixel neighbour, keeping the pixel offset along the
 * shared edge (measured in the neighbour's millimetres). */
static void place_defaults(void)
{
    int any = 0;
    for (int i = 0; i < nmon; i++)
        any |= mons[i].placed;
    if (!any && nmon) {
        int seed = 0;
        for (int i = 0; i < nmon; i++)
            if (mons[i].primary)
                seed = i;
        place_at_pixel_position(&mons[seed]);
    }

    for (;;) {
        int progress = 0, unplaced = -1;
        for (int b = 0; b < nmon; b++) {
            Monitor *B = &mons[b];
            if (B->placed)
                continue;
            unplaced = b;
            for (int i = 0; i < nmon && !B->placed; i++) {
                Monitor *A = &mons[i];
                if (!A->placed)
                    continue;
                for (int a = 0; a < 2 && !B->placed; a++) {
                    int p = 1 - a;
                    if (A->px[p] >= B->px[p] + B->ps[p] || B->px[p] >= A->px[p] + A->ps[p])
                        continue;
                    if (A->px[a] + A->ps[a] == B->px[a])
                        B->mx[a] = A->mx[a] + A->ms[a];
                    else if (B->px[a] + B->ps[a] == A->px[a])
                        B->mx[a] = A->mx[a] - B->ms[a];
                    else
                        continue;
                    B->mx[p] = A->mx[p] + (B->px[p] - A->px[p]) * A->ms[p] / A->ps[p];
                    B->placed = progress = 1;
                }
            }
        }
        if (unplaced < 0)
            break;
        if (!progress) /* no neighbour placed yet: fall back to its own pixel position */
            place_at_pixel_position(&mons[unplaced]);
    }

    if (!any) /* fully generated layout: start it at 0, 0 */
        for (int a = 0; a < 2; a++) {
            double lo = INFINITY;
            for (int i = 0; i < nmon; i++)
                lo = fmin(lo, mons[i].mx[a]);
            for (int i = 0; i < nmon; i++)
                mons[i].mx[a] -= lo;
        }
}

static void load_layout(Display *dpy, Window root, int use_config)
{
    detect_monitors(dpy, root);
    if (use_config)
        read_config();
    place_defaults();
}

static void print_layout(FILE *f)
{
    fprintf(f, "# trueedged layout: physical position and size of each monitor in mm\n");
    fprintf(f, "jump %d\n", jump);
    for (int i = 0; i < nmon; i++) {
        Monitor *m = &mons[i];
        fprintf(f, "monitor %s %.1f %.1f %.1f %.1f  # %dx%d+%d+%d%s\n", m->name, m->mx[0], m->mx[1],
                m->ms[0], m->ms[1], m->ps[0], m->ps[1], m->px[0], m->px[1],
                m->primary ? " primary" : "");
    }
}

static void clear_barriers(Display *dpy)
{
    for (int i = 0; i < nbarriers; i++)
        XFixesDestroyPointerBarrier(dpy, barriers[i]);
    nbarriers = 0;
}

/* Blocks X's own crossing on every shared pixel edge; trueedged does it instead. */
static void set_barriers(Display *dpy, Window root)
{
    clear_barriers(dpy);
    for (int i = 0; i < nmon; i++) {
        for (int j = 0; j < nmon; j++) {
            Monitor *A = &mons[i], *B = &mons[j];
            for (int a = 0; a < 2; a++) {
                int p = 1 - a;
                if (i == j || A->px[a] + A->ps[a] != B->px[a] || nbarriers == MAX_BARRIERS)
                    continue;
                int lo = A->px[p] > B->px[p] ? A->px[p] : B->px[p];
                int hi = A->px[p] + A->ps[p] < B->px[p] + B->ps[p] ? A->px[p] + A->ps[p]
                                                                  : B->px[p] + B->ps[p];
                if (lo >= hi)
                    continue;
                int e = B->px[a];
                barriers[nbarriers++] =
                    a == 0 ? XFixesCreatePointerBarrier(dpy, root, e, lo, e, hi, 0, 0, NULL)
                           : XFixesCreatePointerBarrier(dpy, root, lo, e, hi, e, 0, 0, NULL);
            }
        }
    }
    XFlush(dpy);
}

static int find_monitor(int x, int y)
{
    for (int i = 0; i < nmon; i++)
        if (x >= mons[i].px[0] && x < mons[i].px[0] + mons[i].ps[0] && y >= mons[i].px[1] &&
            y < mons[i].px[1] + mons[i].ps[1])
            return i;
    return -1;
}

/* The cursor at pixel (x, y) on monitor src leaves across axis a (0 = x,
 * 1 = y) in direction s (+1 / -1). Finds where that point physically lands
 * and returns its pixel in (*ox, *oy); 0 if there is nothing there. */
static int map_exit(int src, int x, int y, int a, int s, int *ox, int *oy)
{
    Monitor *A = &mons[src];
    int p = 1 - a, pix[2] = {x, y}, out[2];
    double edge = s > 0 ? A->mx[a] + A->ms[a] : A->mx[a];
    double t = A->mx[p] + (pix[p] + 0.5 - A->px[p]) * A->ms[p] / A->ps[p];
    int best = -1, best_hit = 0;
    double best_d = INFINITY;

    for (int i = 0; i < nmon; i++) {
        Monitor *B = &mons[i];
        if (i == src)
            continue;
        double gap = s > 0 ? B->mx[a] - edge : edge - (B->mx[a] + B->ms[a]);
        if (gap < -EDGE_TOL_MM)
            continue;
        if (gap < 0)
            gap = 0;
        double lo = B->mx[p], hi = lo + B->ms[p];
        double off = t < lo ? lo - t : t > hi ? t - hi : 0;
        int hit = off == 0;
        if (!hit && !jump)
            continue;
        double d = hypot(gap, off);
        if (hit > best_hit || (hit == best_hit && d < best_d)) {
            best = i;
            best_d = d;
            best_hit = hit;
        }
    }
    if (best < 0)
        return 0;

    Monitor *B = &mons[best];
    double tt = fmin(fmax(t, B->mx[p]), B->mx[p] + B->ms[p]);
    out[a] = s > 0 ? B->px[a] : B->px[a] + B->ps[a] - 1;
    out[p] = B->px[p] + (int)floor((tt - B->mx[p]) * B->ps[p] / B->ms[p]);
    if (out[p] < B->px[p])
        out[p] = B->px[p];
    if (out[p] > B->px[p] + B->ps[p] - 1)
        out[p] = B->px[p] + B->ps[p] - 1;
    *ox = out[0];
    *oy = out[1];
    return 1;
}

/* Edge crossing for a cursor at (x, y) moved by (dx, dy); 0 if it stays put. */
static int resolve(int x, int y, double dx, double dy, int *ox, int *oy)
{
    int src = find_monitor(x, y);
    if (src < 0)
        return 0;

    Monitor *A = &mons[src];
    int pos[2] = {x, y};
    double d[2] = {dx, dy};
    int first = fabs(dy) > fabs(dx);
    for (int k = 0; k < 2; k++) {
        int a = k ? !first : first, s;
        if (d[a] > 0 && pos[a] == A->px[a] + A->ps[a] - 1)
            s = 1;
        else if (d[a] < 0 && pos[a] == A->px[a])
            s = -1;
        else
            continue;
        if (map_exit(src, x, y, a, s, ox, oy))
            return 1;
    }
    return 0;
}

static void on_raw_motion(Display *dpy, Window root, XIRawEvent *re)
{
    double d[2] = {0, 0};
    const double *v = re->raw_values;
    for (int axis = 0, n = 0; axis < re->valuators.mask_len * 8; axis++) {
        if (!XIMaskIsSet(re->valuators.mask, axis))
            continue;
        if (axis < 2)
            d[axis] = v[n];
        n++;
    }
    if (d[0] == 0 && d[1] == 0)
        return;

    Window r, c;
    int x, y, wx, wy, tx, ty;
    unsigned int mask;
    if (!XQueryPointer(dpy, root, &r, &c, &x, &y, &wx, &wy, &mask))
        return;
    if (resolve(x, y, d[0], d[1], &tx, &ty)) {
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, tx, ty);
        XFlush(dpy);
    }
}

static int single_instance(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/trueedged.lock", dir ? dir : "/tmp");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: trueedged [--config FILE]            run the daemon (SIGHUP reloads the layout)\n"
            "       trueedged --print [--defaults]       print the layout in use (or the detected default)\n"
            "       trueedged --test X Y DX DY           show where a move of (DX, DY) from (X, Y) lands\n");
}

int main(int argc, char **argv)
{
    int print = 0, use_config = 1, test = 0;
    double targ[4];
    const char *cfg_home = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (cfg_home && *cfg_home)
        snprintf(config_path, sizeof config_path, "%s/trueedge/layout.conf", cfg_home);
    else
        snprintf(config_path, sizeof config_path, "%s/.config/trueedge/layout.conf",
                 home ? home : ".");

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--print"))
            print = 1;
        else if (!strcmp(argv[i], "--defaults"))
            use_config = 0;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc)
            snprintf(config_path, sizeof config_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--test") && i + 4 < argc) {
            test = 1;
            for (int k = 0; k < 4; k++)
                targ[k] = atof(argv[++i]);
        } else {
            usage();
            return 2;
        }
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "trueedged: cannot open display (trueedged needs an X11 session)\n");
        return 1;
    }
    XSetErrorHandler(x_error);
    Window root = DefaultRootWindow(dpy);

    int xi_opcode, ev, err, rr_event, rr_error, fx_event, fx_error;
    int major = 2, minor = 2;
    if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err) ||
        XIQueryVersion(dpy, &major, &minor) != Success || !XRRQueryExtension(dpy, &rr_event, &rr_error) ||
        !XFixesQueryExtension(dpy, &fx_event, &fx_error)) {
        fprintf(stderr, "trueedged: X server lacks XInput2, RandR or XFixes\n");
        return 1;
    }
    major = 5;
    minor = 0;
    XFixesQueryVersion(dpy, &major, &minor);
    if (major < 5) {
        fprintf(stderr, "trueedged: XFixes 5 (pointer barriers) required\n");
        return 1;
    }

    load_layout(dpy, root, use_config);
    if (print) {
        print_layout(stdout);
        return 0;
    }
    if (test) {
        int tx, ty;
        if (resolve((int)targ[0], (int)targ[1], targ[2], targ[3], &tx, &ty))
            printf("%d %d\n", tx, ty);
        else
            printf("none\n");
        return 0;
    }

    if (!single_instance()) {
        fprintf(stderr, "trueedged: already running (send it SIGHUP to reload the layout)\n");
        return 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal; /* no SA_RESTART: poll() must wake up */
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XIEventMask mask = {XIAllMasterDevices, sizeof mask_bits, mask_bits};
    XISetMask(mask_bits, XI_RawMotion);
    XISelectEvents(dpy, root, &mask, 1);
    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);

    set_barriers(dpy, root);
    print_layout(stderr);

    struct pollfd pfd = {ConnectionNumber(dpy), POLLIN, 0};
    while (!quit_requested) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == GenericEvent && e.xcookie.extension == xi_opcode &&
                XGetEventData(dpy, &e.xcookie)) {
                if (e.xcookie.evtype == XI_RawMotion)
                    on_raw_motion(dpy, root, e.xcookie.data);
                XFreeEventData(dpy, &e.xcookie);
            } else if (e.type >= rr_event && e.type <= rr_event + RRNotify) {
                XRRUpdateConfiguration(&e);
                reload_requested = 1;
            }
        }
        if (reload_requested) {
            reload_requested = 0;
            load_layout(dpy, root, 1);
            set_barriers(dpy, root);
            fprintf(stderr, "trueedged: layout reloaded\n");
            print_layout(stderr);
        }
        if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
            break;
    }

    clear_barriers(dpy);
    XCloseDisplay(dpy);
    return 0;
}
