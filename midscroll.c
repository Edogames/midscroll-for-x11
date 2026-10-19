/*
 * midscroll v2 - Windows-style middle-click autoscroll for X11 (Cinnamon etc.)
 *
 * Build:   gcc -O2 -Wall -o midscroll midscroll.c \
 *              $(pkg-config --cflags --libs atspi-2 glib-2.0 gobject-2.0) -lX11 -lXtst -lXext
 * Config:  ~/.config/midscroll.conf      (reload with: pkill -HUP midscroll)
 * Toggle:  pkill -USR1 midscroll         (enable/disable without quitting)
 *
 * A middle-click starts autoscroll ONLY when it lands inside something
 * scrollable (scroll pane, web page, document, terminal). Everything else
 * (tabs, title bars, toolbars, buttons, links, text entries, scrollbars,
 * desktop/panel) gets a normal middle-click. To know what is under the
 * cursor it asks the app through AT-SPI (the accessibility bus), so that
 * must be enabled - see README notes in the chat.
 *
 * Once autoscroll has started the cursor position no longer matters: the
 * pointer is pinned at the click point, mouse movement is accumulated into a
 * virtual offset, and scrolling keeps going to the window you started in -
 * even if you push the mouse past the screen edge.
 *
 * Idle cost: sleeps in ppoll(); wakes once every 30 s to drain the AT-SPI
 * connection, otherwise 0% CPU.
 */
#define _GNU_SOURCE
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/shape.h>
#include <atspi/atspi.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define R 14            /* indicator radius in px */
#define MAX_EXCL 128
#define MAX_TOP 32
#define TICK_NS 16000000L
#define QUERY_BUDGET_MS 300   /* max time we may freeze the pointer to ask AT-SPI */

static Display *dpy;
static Window root, ind;
static GC ind_gc;
static Atom net_wm_pid;

/* config */
static double speed = 0.25;     /* scroll clicks/sec per pixel beyond deadzone */
static double max_rate = 60.0;  /* clicks/sec cap */
static int deadzone = 12;       /* px around the origin with no scrolling */
static int unknown_scroll = 1;  /* what to do when AT-SPI can't tell: 1 = autoscroll, 0 = pass through */
static char *excl[MAX_EXCL];
static int nexcl;
static struct { char *name; int px; } tops[MAX_TOP];
static int ntops;

/* state */
static int enabled = 1, active, pending_start;
static int ox, oy;      /* where the middle-click happened; the pointer is pinned here while scrolling */
static int vx, vy;      /* virtual offset from that point, accumulated from mouse movement */
static double acc_x, acc_y;
static struct timespec last_tick, query_start;
static volatile sig_atomic_t want_reload, want_toggle;
static int x_error;

static const unsigned lockmods[4] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

/* ---------- config ---------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static void load_config(void) {
    for (int i = 0; i < nexcl; i++) free(excl[i]);
    for (int i = 0; i < ntops; i++) free(tops[i].name);
    nexcl = ntops = 0;
    speed = 0.25; max_rate = 60.0; deadzone = 12; unknown_scroll = 1;

    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(path, sizeof path, "%s/midscroll.conf", xdg);
    else snprintf(path, sizeof path, "%s/.config/midscroll.conf", home ? home : "");

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (*p == '#' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p), *val = trim(eq + 1);
        if (!strcmp(key, "speed")) speed = atof(val);
        else if (!strcmp(key, "max_rate")) max_rate = atof(val);
        else if (!strcmp(key, "deadzone")) deadzone = atoi(val);
        else if (!strcmp(key, "unknown")) unknown_scroll = strcasecmp(val, "pass") != 0;
        else if (!strcmp(key, "exclude")) {
            for (char *tok = strtok(val, ","); tok && nexcl < MAX_EXCL; tok = strtok(NULL, ",")) {
                tok = trim(tok);
                if (*tok) excl[nexcl++] = strdup(tok);
            }
        } else if (!strcmp(key, "ignore_top")) {      /* app:pixels, ... */
            for (char *tok = strtok(val, ","); tok && ntops < MAX_TOP; tok = strtok(NULL, ",")) {
                tok = trim(tok);
                char *colon = strrchr(tok, ':');
                if (!colon) continue;
                *colon = 0;
                char *name = trim(tok);
                int px = atoi(colon + 1);
                if (*name && px > 0) {
                    tops[ntops].name = strdup(name);
                    tops[ntops].px = px;
                    ntops++;
                }
            }
        }
    }
    fclose(f);
}

/* ---------- AT-SPI: "is the thing under the cursor scrollable?" ---------- */

static void pump_glib(void) { while (g_main_context_iteration(NULL, FALSE)) {} }

static int over_budget(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long ms = (t.tv_sec - query_start.tv_sec) * 1000L + (t.tv_nsec - query_start.tv_nsec) / 1000000L;
    return ms > QUERY_BUDGET_MS;
}

/* 1 = scrollable area, 0 = something else (pass the click through), -1 = keep walking up */
static int role_verdict(AtspiRole r) {
    switch (r) {
    case ATSPI_ROLE_SCROLL_PANE:
    case ATSPI_ROLE_DOCUMENT_WEB:
    case ATSPI_ROLE_DOCUMENT_FRAME:
    case ATSPI_ROLE_DOCUMENT_TEXT:
    case ATSPI_ROLE_DOCUMENT_EMAIL:
    case ATSPI_ROLE_DOCUMENT_SPREADSHEET:
    case ATSPI_ROLE_DOCUMENT_PRESENTATION:
    case ATSPI_ROLE_TERMINAL:
        return 1;
    /* things that have their own middle-click meaning, or aren't content */
    case ATSPI_ROLE_LINK:
    case ATSPI_ROLE_PUSH_BUTTON:
    case ATSPI_ROLE_TOGGLE_BUTTON:
    case ATSPI_ROLE_CHECK_BOX:
    case ATSPI_ROLE_RADIO_BUTTON:
    case ATSPI_ROLE_COMBO_BOX:
    case ATSPI_ROLE_SPIN_BUTTON:
    case ATSPI_ROLE_SLIDER:
    case ATSPI_ROLE_ENTRY:
    case ATSPI_ROLE_PASSWORD_TEXT:
    case ATSPI_ROLE_PAGE_TAB:
    case ATSPI_ROLE_PAGE_TAB_LIST:
    case ATSPI_ROLE_TOOL_BAR:
    case ATSPI_ROLE_MENU_BAR:
    case ATSPI_ROLE_MENU:
    case ATSPI_ROLE_MENU_ITEM:
    case ATSPI_ROLE_TITLE_BAR:
    case ATSPI_ROLE_STATUS_BAR:
    case ATSPI_ROLE_SCROLL_BAR:
    case ATSPI_ROLE_FRAME:
    case ATSPI_ROLE_WINDOW:
    case ATSPI_ROLE_DIALOG:
    case ATSPI_ROLE_APPLICATION:
        return 0;
    default:
        return -1;
    }
}

/* Top-level accessible of `app` whose screen rectangle contains (x,y). */
static AtspiAccessible *frame_at(AtspiAccessible *app, int x, int y) {
    int n = atspi_accessible_get_child_count(app, NULL);
    for (int i = 0; i < n && !over_budget(); i++) {
        AtspiAccessible *w = atspi_accessible_get_child_at_index(app, i, NULL);
        if (!w) continue;
        AtspiComponent *c = atspi_accessible_get_component_iface(w);
        if (c) {
            AtspiRect *r = atspi_component_get_extents(c, ATSPI_COORD_TYPE_SCREEN, NULL);
            g_object_unref(c);
            if (r) {
                int in = x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
                g_free(r);
                if (in) return w;
            }
        }
        g_object_unref(w);
    }
    return NULL;
}

/* Descend from `frame` to the deepest accessible at (x,y), then walk back up. */
static int classify(AtspiAccessible *frame, int x, int y) {
    AtspiAccessible *cur = frame;   /* consumes the reference */
    int descended = 0;
    for (int i = 0; i < 40 && !over_budget(); i++) {
        AtspiComponent *c = atspi_accessible_get_component_iface(cur);
        if (!c) break;
        AtspiAccessible *next = atspi_component_get_accessible_at_point(c, x, y, ATSPI_COORD_TYPE_SCREEN, NULL);
        g_object_unref(c);
        if (!next) break;
        if (next == cur) { g_object_unref(next); break; }
        g_object_unref(cur);
        cur = next;
        descended++;
    }
    if (!descended || over_budget()) { g_object_unref(cur); return -1; }  /* toolkit can't hit-test */

    int verdict = 0;   /* reached the top without finding a scrollable ancestor -> not scrollable */
    for (int i = 0; i < 48 && cur; i++) {
        int v = role_verdict(atspi_accessible_get_role(cur, NULL));
        if (v >= 0) { verdict = v; break; }
        if (over_budget()) { verdict = -1; break; }
        AtspiAccessible *p = atspi_accessible_get_parent(cur, NULL);
        g_object_unref(cur);
        cur = p;
    }
    if (cur) g_object_unref(cur);
    return verdict;
}

/* 1 / 0 as above, -1 = AT-SPI unavailable or couldn't answer */
static int a11y_scrollable(unsigned long pid, int x, int y) {
    if (!pid) return -1;
    clock_gettime(CLOCK_MONOTONIC, &query_start);
    pump_glib();

    AtspiAccessible *desk = atspi_get_desktop(0);
    if (!desk) return -1;
    GError *err = NULL;
    int n = atspi_accessible_get_child_count(desk, &err);
    if (err) { g_error_free(err); g_object_unref(desk); return -1; }

    AtspiAccessible *frame = NULL;
    for (int i = 0; i < n && !frame && !over_budget(); i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desk, i, NULL);
        if (!app) continue;
        if (atspi_accessible_get_process_id(app, NULL) == pid) frame = frame_at(app, x, y);
        g_object_unref(app);
    }
    g_object_unref(desk);
    if (!frame) return -1;
    return classify(frame, x, y);
}

/* ---------- X window lookup ---------- */

static unsigned long window_pid(Window w) {
    Atom type;
    int fmt;
    unsigned long n, after, pid = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, net_wm_pid, 0, 1, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success && data) {
        if (n == 1 && fmt == 32) pid = *(unsigned long *)data;
        XFree(data);
    }
    return pid;
}

static int client_allows(const XClassHint *ch, int client_y) {
    for (int i = 0; i < nexcl; i++)
        if ((ch->res_name && !strcasecmp(ch->res_name, excl[i])) ||
            (ch->res_class && !strcasecmp(ch->res_class, excl[i])))
            return 0;
    for (int i = 0; i < ntops; i++)
        if (((ch->res_name && !strcasecmp(ch->res_name, tops[i].name)) ||
             (ch->res_class && !strcasecmp(ch->res_class, tops[i].name))) &&
            client_y < tops[i].px)
            return 0;
    return 1;
}

/* 1 = start autoscroll, 0 = pass the click through to the app */
static int should_scroll(int rx, int ry) {
    Window w = root, child;
    int dx, dy;
    for (int depth = 0; depth < 32; depth++) {
        if (!XTranslateCoordinates(dpy, root, w, rx, ry, &dx, &dy, &child) || !child) break;
        w = child;
        XClassHint ch;
        if (!XGetClassHint(dpy, w, &ch)) continue;

        int cx = 0, cy = 0;
        Window dummy;
        XTranslateCoordinates(dpy, root, w, rx, ry, &cx, &cy, &dummy);
        int ok = client_allows(&ch, cy);
        if (ch.res_name) XFree(ch.res_name);
        if (ch.res_class) XFree(ch.res_class);
        if (!ok) return 0;

        int v = a11y_scrollable(window_pid(w), rx, ry);
        return v < 0 ? unknown_scroll : v;
    }
    return 0;   /* no client window under the cursor (title bar, desktop, panel...) */
}

/* ---------- grabs ---------- */

static void grab_button(unsigned btn, int ptr_mode) {
    for (int i = 0; i < 4; i++)
        XGrabButton(dpy, btn, lockmods[i], root, False,
                    ButtonPressMask | ButtonReleaseMask, ptr_mode, GrabModeAsync, None, None);
}

static void ungrab_button(unsigned btn) {
    for (int i = 0; i < 4; i++) XUngrabButton(dpy, btn, lockmods[i], root);
}

static void grab_escape(int on) {
    KeyCode kc = XKeysymToKeycode(dpy, XK_Escape);
    for (int i = 0; i < 4; i++) {
        if (on) XGrabKey(dpy, kc, lockmods[i], root, False, GrabModeAsync, GrabModeAsync);
        else XUngrabKey(dpy, kc, lockmods[i], root);
    }
}

/* ---------- indicator (click-through round marker) ---------- */

static void make_indicator(void) {
    int s = 2 * R + 1;
    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
    a.event_mask = ExposureMask;
    ind = XCreateWindow(dpy, root, 0, 0, s, s, 0, CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &a);

    Pixmap pm = XCreatePixmap(dpy, ind, s, s, 1);
    GC g = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, g, 0);
    XFillRectangle(dpy, pm, g, 0, 0, s, s);
    XSetForeground(dpy, g, 1);
    XFillArc(dpy, pm, g, 0, 0, s - 1, s - 1, 0, 360 * 64);
    XShapeCombineMask(dpy, ind, ShapeBounding, 0, 0, pm, ShapeSet);
    XShapeCombineRectangles(dpy, ind, ShapeInput, 0, 0, NULL, 0, ShapeSet, 0); /* click-through */
    XFreePixmap(dpy, pm);
    XFreeGC(dpy, g);

    ind_gc = XCreateGC(dpy, ind, 0, NULL);
    XSetForeground(dpy, ind_gc, WhitePixel(dpy, DefaultScreen(dpy)));
}

static void draw_indicator(void) {
    int s = 2 * R + 1, c = R;
    XPoint up[3] = {{c, 3}, {c - 4, 9}, {c + 4, 9}};
    XPoint dn[3] = {{c, s - 4}, {c - 4, s - 10}, {c + 4, s - 10}};
    XPoint lf[3] = {{3, c}, {9, c - 4}, {9, c + 4}};
    XPoint rt[3] = {{s - 4, c}, {s - 10, c - 4}, {s - 10, c + 4}};
    XFillPolygon(dpy, ind, ind_gc, up, 3, Convex, CoordModeOrigin);
    XFillPolygon(dpy, ind, ind_gc, dn, 3, Convex, CoordModeOrigin);
    XFillPolygon(dpy, ind, ind_gc, lf, 3, Convex, CoordModeOrigin);
    XFillPolygon(dpy, ind, ind_gc, rt, 3, Convex, CoordModeOrigin);
    XFillArc(dpy, ind, ind_gc, c - 1, c - 1, 3, 3, 0, 360 * 64);
}

/* ---------- autoscroll ---------- */

static void now(struct timespec *t) { clock_gettime(CLOCK_MONOTONIC, t); }

static void start_scroll(int x, int y) {
    ox = x; oy = y;
    acc_x = acc_y = 0;
    vx = vy = 0;
    active = 1;
    XMoveWindow(dpy, ind, x - R, y - R);
    XMapRaised(dpy, ind);
    /* Only 1 and 3 (+Esc): the fake wheel buttons 4-7 must NOT be grabbed,
     * or our own synthetic scrolling would be swallowed. Button 2 is always grabbed. */
    grab_button(Button1, GrabModeAsync);
    grab_button(Button3, GrabModeAsync);
    grab_escape(1);
    now(&last_tick);
}

static void stop_scroll(void) {
    if (!active) return;
    active = 0;
    XUnmapWindow(dpy, ind);
    ungrab_button(Button1);
    ungrab_button(Button3);
    grab_escape(0);
}

static void scroll_axis(int delta, double *acc, double dt, unsigned neg, unsigned pos) {
    int a = delta < 0 ? -delta : delta;
    if (a <= deadzone) { *acc = 0; return; }
    double rate = (a - deadzone) * speed;
    if (rate > max_rate) rate = max_rate;
    *acc += rate * dt;
    unsigned b = delta < 0 ? neg : pos;
    while (*acc >= 1.0) {
        XTestFakeButtonEvent(dpy, b, True, CurrentTime);
        XTestFakeButtonEvent(dpy, b, False, CurrentTime);
        *acc -= 1.0;
    }
}

static void tick(void) {
    struct timespec t;
    now(&t);
    double dt = (t.tv_sec - last_tick.tv_sec) + (t.tv_nsec - last_tick.tv_nsec) / 1e9;
    last_tick = t;
    if (dt > 0.1) dt = 0.1;

    Window rw, cw;
    int rx, ry, wx, wy;
    unsigned m;
    if (!XQueryPointer(dpy, root, &rw, &cw, &rx, &ry, &wx, &wy, &m)) return;

    /* Where the cursor is no longer matters: movement is accumulated into a
     * virtual offset and the real pointer is pulled back to the click point.
     * So scrolling always goes to the window you started in, and the offset
     * keeps growing even when the pointer would have hit the screen edge. */
    if (rx != ox || ry != oy) {
        vx += rx - ox;
        vy += ry - oy;
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, ox, oy);
    }
    /* cap the offset at the distance that already gives max speed, so moving
     * back reduces the speed immediately instead of "unwinding" */
    int lim = deadzone + (int)(max_rate / (speed > 0.001 ? speed : 0.001)) + 1;
    if (vx > lim) vx = lim; else if (vx < -lim) vx = -lim;
    if (vy > lim) vy = lim; else if (vy < -lim) vy = -lim;

    scroll_axis(vy, &acc_y, dt, 4, 5);  /* up / down    */
    scroll_axis(vx, &acc_x, dt, 6, 7);  /* left / right */
    XFlush(dpy);
}

/* ---------- events ---------- */

static void handle(XEvent *e) {
    switch (e->type) {
    case Expose:
        if (e->xexpose.window == ind && e->xexpose.count == 0) draw_indicator();
        break;
    case KeyPress:                       /* only Esc is grabbed, and only while active */
        stop_scroll();
        break;
    case ButtonPress:
        if (active) {                    /* any click stops; the click is swallowed */
            stop_scroll();
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
        } else if (e->xbutton.button != Button2) {
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
        } else if (!enabled || !should_scroll(e->xbutton.x_root, e->xbutton.y_root)) {
            XAllowEvents(dpy, ReplayPointer, CurrentTime);   /* app gets a normal middle-click */
        } else {
            pending_start = 1;           /* swallow press, start on release */
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
        }
        break;
    case ButtonRelease:
        if (pending_start && e->xbutton.button == Button2) {
            pending_start = 0;
            start_scroll(e->xbutton.x_root, e->xbutton.y_root);
        }
        break;
    }
}

static int xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; x_error = 1; return 0; }
static void on_signal(int s) { if (s == SIGHUP) want_reload = 1; else want_toggle = 1; }

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "midscroll: cannot open X display\n"); return 1; }

    int a, b, c, d;
    if (!XTestQueryExtension(dpy, &a, &b, &c, &d) || !XShapeQueryExtension(dpy, &a, &b)) {
        fprintf(stderr, "midscroll: XTEST/SHAPE extension missing\n");
        return 1;
    }
    XSetErrorHandler(xerr);
    root = DefaultRootWindow(dpy);
    net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);

    load_config();
    make_indicator();

    /* AT-SPI warm-up so the first click isn't slow; tight timeouts so a hung app can't stall us. */
    atspi_init();
    atspi_set_timeout(150, 1000);
    {
        GError *err = NULL;
        AtspiAccessible *desk = atspi_get_desktop(0);
        if (!desk || (atspi_accessible_get_child_count(desk, &err), err))
            fprintf(stderr, "midscroll: accessibility bus not reachable; falling back to unknown=%s\n",
                    unknown_scroll ? "scroll" : "pass");
        if (err) g_error_free(err);
        if (desk) g_object_unref(desk);
    }

    x_error = 0;
    grab_button(Button2, GrabModeSync);
    XSync(dpy, False);
    if (x_error) {
        fprintf(stderr, "midscroll: cannot grab the middle button (already grabbed by another program?)\n");
        return 1;
    }

    /* Signals are blocked except inside ppoll(), so reload/toggle never race. */
    sigset_t block, orig;
    sigemptyset(&block);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &orig);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    struct pollfd pfd = {ConnectionNumber(dpy), POLLIN, 0};
    struct timespec tick_ts = {0, TICK_NS}, idle_ts = {30, 0};

    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            handle(&e);
        }
        if (want_reload) { want_reload = 0; load_config(); }
        if (want_toggle) { want_toggle = 0; enabled = !enabled; if (!enabled) stop_scroll(); }
        XFlush(dpy);

        ppoll(&pfd, 1, active ? &tick_ts : &idle_ts, &orig);
        if (active) tick();
        else pump_glib();    /* keep the AT-SPI D-Bus connection drained */
    }
}
