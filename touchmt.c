/*
 * TouchMT - user-mode touch driver for MicroTouch serial controllers
 *           (EX / Excalibur family, "Format Tablet" protocol)
 *
 * Reads the controller on a COM port and turns touches into mouse input
 * with SendInput(). No kernel driver, so it cannot bluescreen the system.
 * Targets Windows XP / XP x64 / Server 2003 and later.
 *
 * Usage:
 *   touchmt.exe                 run (calibrates first if no calibration saved)
 *   touchmt.exe --calibrate     run calibration, then keep running
 *   touchmt.exe --debug         run with a console showing raw packets
 *   touchmt.exe --stop          stop a running copy
 *   touchmt.exe --list          list monitors (for "monitor=" in touchmt.ini)
 *
 * Settings live in touchmt.ini next to the exe.
 */
#define _WIN32_WINNT 0x0502
#define WINVER       0x0502
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#endif

#define APP_NAME   "TouchMT"
#define MSG_CLASS  "TouchMTMsgWnd"
#define CAL_CLASS  "TouchMTCalWnd"
#define MUTEX_NAME "TouchMT_SingleInstance"
#define WM_TOUCHPKT (WM_APP + 1)

#define SOH 0x01
#define CR  0x0D

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */
static struct {
    char   port[32];
    int    baud;
    int    monitor;          /* 0 = primary, else 1-based enumeration index */
    int    calibrated;
    double a, b, c;          /* screenX = a*rx + b*ry + c   (virtual desktop px) */
    double d, e, f;          /* screenY = d*rx + e*ry + f */
    int    deadzone;         /* px a touch must move before it becomes a drag */
    int    rightclick_ms;    /* hold still this long = right click (0 = off) */
    int    smoothing;        /* 0 = none, higher = smoother/laggier */
    int    liftoff_timeout;  /* ms without packets = release (0 = off) */
    int    swap_touch_bit;   /* 1 = invert meaning of touch bit (diagnostic) */
    int    edge_snap;        /* px: touches this close to a screen edge snap onto it */
    double edge_l, edge_r, edge_t, edge_b;  /* how far in (fraction) the glass stops */
    int    edge_band;        /* px over which the edge stretch is spread */
} cfg;

static char   g_ini[MAX_PATH];
static int    g_debug = 0;
static HANDLE g_com = INVALID_HANDLE_VALUE;
static volatile LONG g_quit = 0;
static volatile HWND g_cal_hwnd = NULL;   /* when set, packets go to calibration */
static HWND   g_msg_hwnd = NULL;

static HANDLE g_con = NULL;
static FILE  *g_logfile = NULL;
static CRITICAL_SECTION g_logcs;

static void dbg(const char *fmt, ...)
{
    char buf[512];
    DWORD n;
    va_list ap;
    if (!g_debug) return;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 3, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 3] = 0;
    strcat(buf, "\r\n");
    EnterCriticalSection(&g_logcs);
    if (g_con && g_con != INVALID_HANDLE_VALUE)
        WriteConsoleA(g_con, buf, (DWORD)strlen(buf), &n, NULL);
    if (g_logfile) { fputs(buf, g_logfile); fflush(g_logfile); }
    LeaveCriticalSection(&g_logcs);
}

static double ini_get_d(const char *key, double def)
{
    char buf[64], defs[64];
    sprintf(defs, "%.12g", def);
    GetPrivateProfileStringA("touch", key, defs, buf, sizeof buf, g_ini);
    return atof(buf);
}
static int ini_get_i(const char *key, int def)
{
    return (int)GetPrivateProfileIntA("touch", key, def, g_ini);
}
static void ini_put_d(const char *key, double v)
{
    char buf[64];
    sprintf(buf, "%.12g", v);
    WritePrivateProfileStringA("touch", key, buf, g_ini);
}
static void ini_put_i(const char *key, int v)
{
    char buf[32];
    sprintf(buf, "%d", v);
    WritePrivateProfileStringA("touch", key, buf, g_ini);
}

static void load_config(void)
{
    char *p;
    GetModuleFileNameA(NULL, g_ini, MAX_PATH);
    p = strrchr(g_ini, '\\');
    if (p) p[1] = 0; else g_ini[0] = 0;
    strcat(g_ini, "touchmt.ini");

    GetPrivateProfileStringA("touch", "port", "COM1", cfg.port, sizeof cfg.port, g_ini);
    cfg.baud            = ini_get_i("baud", 9600);
    cfg.monitor         = ini_get_i("monitor", 0);
    cfg.calibrated      = ini_get_i("calibrated", 0);
    cfg.a = ini_get_d("a", 0); cfg.b = ini_get_d("b", 0); cfg.c = ini_get_d("c", 0);
    cfg.d = ini_get_d("d", 0); cfg.e = ini_get_d("e", 0); cfg.f = ini_get_d("f", 0);
    cfg.deadzone        = ini_get_i("deadzone", 8);
    cfg.rightclick_ms   = ini_get_i("rightclick_ms", 0);
    cfg.smoothing       = ini_get_i("smoothing", 1);
    cfg.liftoff_timeout = ini_get_i("liftoff_timeout_ms", 0);
    cfg.swap_touch_bit  = ini_get_i("swap_touch_bit", 0);
    cfg.edge_snap       = ini_get_i("edge_snap", 6);
    cfg.edge_l = ini_get_d("edge_l", 0); cfg.edge_r = ini_get_d("edge_r", 0);
    cfg.edge_t = ini_get_d("edge_t", 0); cfg.edge_b = ini_get_d("edge_b", 0);
    cfg.edge_band       = ini_get_i("edge_band", 60);

    /* write defaults back so the user can see/edit every option */
    WritePrivateProfileStringA("touch", "port", cfg.port, g_ini);
    ini_put_i("baud", cfg.baud);
    ini_put_i("monitor", cfg.monitor);
    ini_put_i("deadzone", cfg.deadzone);
    ini_put_i("rightclick_ms", cfg.rightclick_ms);
    ini_put_i("smoothing", cfg.smoothing);
    ini_put_i("liftoff_timeout_ms", cfg.liftoff_timeout);
    ini_put_i("swap_touch_bit", cfg.swap_touch_bit);
    ini_put_i("edge_snap", cfg.edge_snap);
    ini_put_i("edge_band", cfg.edge_band);
}

static void save_calibration(void)
{
    ini_put_d("a", cfg.a); ini_put_d("b", cfg.b); ini_put_d("c", cfg.c);
    ini_put_d("d", cfg.d); ini_put_d("e", cfg.e); ini_put_d("f", cfg.f);
    ini_put_i("calibrated", 1);
    cfg.calibrated = 1;
}

/* ------------------------------------------------------------------ */
/* monitors                                                            */
/* ------------------------------------------------------------------ */
#define MAX_MON 16
static RECT g_mon[MAX_MON];
static int  g_mon_primary[MAX_MON];
static int  g_nmon = 0;

static BOOL CALLBACK mon_cb(HMONITOR hm, HDC hdc, LPRECT r, LPARAM lp)
{
    MONITORINFO mi;
    (void)hdc; (void)r; (void)lp;
    if (g_nmon >= MAX_MON) return FALSE;
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(hm, &mi);
    g_mon[g_nmon] = mi.rcMonitor;
    g_mon_primary[g_nmon] = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
    g_nmon++;
    return TRUE;
}

static void enum_monitors(void)
{
    g_nmon = 0;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
}

static RECT touch_monitor_rect(void)
{
    int i;
    RECT r;
    enum_monitors();
    if (cfg.monitor >= 1 && cfg.monitor <= g_nmon) return g_mon[cfg.monitor - 1];
    for (i = 0; i < g_nmon; i++) if (g_mon_primary[i]) return g_mon[i];
    r.left = 0; r.top = 0;
    r.right = GetSystemMetrics(SM_CXSCREEN);
    r.bottom = GetSystemMetrics(SM_CYSCREEN);
    return r;
}

/* ------------------------------------------------------------------ */
/* serial                                                              */
/* ------------------------------------------------------------------ */
static int com_open(void)
{
    char name[64];
    DCB dcb;
    COMMTIMEOUTS to;

    sprintf(name, "\\\\.\\%s", cfg.port);
    g_com = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (g_com == INVALID_HANDLE_VALUE) return 0;

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(g_com, &dcb);
    dcb.BaudRate        = cfg.baud;
    dcb.ByteSize        = 8;
    dcb.Parity          = NOPARITY;
    dcb.StopBits        = ONESTOPBIT;
    dcb.fBinary         = TRUE;
    dcb.fParity         = FALSE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_ENABLE;
    dcb.fRtsControl     = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX           = FALSE;
    dcb.fInX            = FALSE;
    dcb.fNull           = FALSE;
    dcb.fAbortOnError   = FALSE;
    if (!SetCommState(g_com, &dcb)) {
        CloseHandle(g_com);
        g_com = INVALID_HANDLE_VALUE;
        return 0;
    }

    /* return as soon as >=1 byte arrives, or after 100 ms */
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = MAXDWORD;
    to.ReadTotalTimeoutConstant    = 20;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 500;
    SetCommTimeouts(g_com, &to);
    PurgeComm(g_com, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 1;
}

static void com_send(const char *cmd)
{
    unsigned char buf[32];
    DWORD n, len = (DWORD)strlen(cmd);
    if (len > 28) return;
    buf[0] = SOH;
    memcpy(buf + 1, cmd, len);
    buf[len + 1] = CR;
    WriteFile(g_com, buf, len + 2, &n, NULL);
    dbg("> %s", cmd);
}

/* ------------------------------------------------------------------ */
/* mouse injection                                                     */
/* ------------------------------------------------------------------ */
static void send_mouse(int px, int py, DWORD flags)
{
    INPUT in;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw < 2) vw = 2;
    if (vh < 2) vh = 2;
    if (px < vx) px = vx;
    if (py < vy) py = vy;
    if (px > vx + vw - 1) px = vx + vw - 1;
    if (py > vy + vh - 1) py = vy + vh - 1;

    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((LONGLONG)(px - vx) * 65535 + (vw - 1) / 2) / (vw - 1));
    in.mi.dy = (LONG)(((LONGLONG)(py - vy) * 65535 + (vh - 1) / 2) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                    MOUSEEVENTF_VIRTUALDESK | flags;
    SendInput(1, &in, sizeof in);
}

/* ------------------------------------------------------------------ */
/* grid calibration (corrects edge non-linearity of the sensor)        */
/* ------------------------------------------------------------------ */
#define GRID 4
#define NCAL (GRID * GRID)
static const double g_frac[GRID] = {0.035, 0.35, 0.65, 0.965};
static double g_rawx[NCAL], g_rawy[NCAL];   /* row-major: index = row*GRID + col */
static int    g_grid_ok = 0;

/* Lagrange basis over the GRID nodes: a smooth bicubic surface through all
 * 16 measured points (exact for any cubic distortion of the sensor). */
static void lagrange(double f, double *L)
{
    int i, k;
    for (i = 0; i < GRID; i++) {
        L[i] = 1.0;
        for (k = 0; k < GRID; k++)
            if (k != i) L[i] *= (f - g_frac[k]) / (g_frac[i] - g_frac[k]);
    }
}

/* raw coordinates predicted at screen fraction (fx,fy) */
static void grid_fwd(double fx, double fy, double *rx, double *ry)
{
    double Lx[GRID], Ly[GRID], x = 0, y = 0;
    int i, j;
    lagrange(fx, Lx);
    lagrange(fy, Ly);
    for (j = 0; j < GRID; j++)
        for (i = 0; i < GRID; i++) {
            double w = Lx[i] * Ly[j];
            x += w * g_rawx[j * GRID + i];
            y += w * g_rawy[j * GRID + i];
        }
    *rx = x; *ry = y;
}

/* invert grid_fwd with Newton's method; (*pf,*qf) hold the initial guess */
static void grid_inv(double rx, double ry, double *pf, double *qf)
{
    double p = *pf, q = *qf, h = 1e-4;
    int it;
    for (it = 0; it < 20; it++) {
        double fx, fy, ax, ay, bx, by, ex, ey, J11, J12, J21, J22, det, dp, dq;
        grid_fwd(p, q, &fx, &fy);
        ex = fx - rx; ey = fy - ry;
        if (ex * ex + ey * ey < 1e-6) break;
        grid_fwd(p + h, q, &ax, &ay);
        grid_fwd(p, q + h, &bx, &by);
        J11 = (ax - fx) / h; J21 = (ay - fy) / h;
        J12 = (bx - fx) / h; J22 = (by - fy) / h;
        det = J11 * J22 - J12 * J21;
        if (det > -1e-12 && det < 1e-12) break;
        dp = ( J22 * ex - J12 * ey) / det;
        dq = (-J21 * ex + J11 * ey) / det;
        if (dp > 0.2) dp = 0.2;
        if (dp < -0.2) dp = -0.2;
        if (dq > 0.2) dq = 0.2;
        if (dq < -0.2) dq = -0.2;
        p -= dp; q -= dq;
        if (p < -0.1) p = -0.1;
        if (p > 1.1) p = 1.1;
        if (q < -0.1) q = -0.1;
        if (q > 1.1) q = 1.1;
    }
    *pf = p; *qf = q;
}

static void load_grid(void)
{
    char key[16];
    int i;
    g_grid_ok = ini_get_i("grid_ok", 0) && ini_get_i("grid_n", 0) == GRID;
    for (i = 0; i < NCAL && g_grid_ok; i++) {
        sprintf(key, "g%dx", i); g_rawx[i] = ini_get_d(key, 0);
        sprintf(key, "g%dy", i); g_rawy[i] = ini_get_d(key, 0);
    }
}

static void save_grid(void)
{
    char key[16];
    int i;
    for (i = 0; i < NCAL; i++) {
        sprintf(key, "g%dx", i); ini_put_d(key, g_rawx[i]);
        sprintf(key, "g%dy", i); ini_put_d(key, g_rawy[i]);
    }
    ini_put_i("grid_n", GRID);
    ini_put_i("grid_ok", 1);
    g_grid_ok = 1;
}

static RECT mon_cached(void)
{
    static RECT m;
    static DWORD t = 0;
    if (t == 0 || GetTickCount() - t > 5000) { m = touch_monitor_rect(); t = GetTickCount(); }
    return m;
}

static void raw_to_screen_base(double rx, double ry, double *ox, double *oy)
{
    double x = cfg.a * rx + cfg.b * ry + cfg.c;   /* affine estimate */
    double y = cfg.d * rx + cfg.e * ry + cfg.f;
    if (g_grid_ok) {
        RECT m = mon_cached();
        double w = m.right - m.left, hh = m.bottom - m.top;
        double p = (x - m.left) / w;
        double q = (y - m.top) / hh;
        if (p < 0) p = 0;
        if (p > 1) p = 1;
        if (q < 0) q = 0;
        if (q > 1) q = 1;
        grid_inv(rx, ry, &p, &q);
        x = m.left + p * w;
        y = m.top + q * hh;
    }
    *ox = x; *oy = y;
}

/* stretch the strip between where the glass physically stops and
 * edge_band px further in, so the reachable limit lands on the edge */
static double stretch(double v, double lo, double hi, double inset_lo, double inset_hi, int band)
{
    double L = lo + inset_lo, H = hi - inset_hi;
    if (inset_lo > 0.5 && v < L + band)
        v = lo + (v - L) * ((L + band - lo) / band);
    if (inset_hi > 0.5 && v > H - band)
        v = hi - (H - v) * ((hi - (H - band)) / band);
    return v;
}

static void raw_to_screen(double rx, double ry, int *sx, int *sy)
{
    double x, y;
    RECT m = mon_cached();
    double w = m.right - m.left, hh = m.bottom - m.top;
    raw_to_screen_base(rx, ry, &x, &y);
    x = stretch(x, m.left, m.right - 1, cfg.edge_l * w, cfg.edge_r * w, cfg.edge_band);
    y = stretch(y, m.top, m.bottom - 1, cfg.edge_t * hh, cfg.edge_b * hh, cfg.edge_band);
    *sx = (int)(x + (x >= 0 ? 0.5 : -0.5));
    *sy = (int)(y + (y >= 0 ? 0.5 : -0.5));
    if (cfg.edge_snap > 0) {
        if (*sx < m.left + cfg.edge_snap)          *sx = m.left;
        if (*sx > m.right - 1 - cfg.edge_snap)     *sx = m.right - 1;
        if (*sy < m.top + cfg.edge_snap)           *sy = m.top;
        if (*sy > m.bottom - 1 - cfg.edge_snap)    *sy = m.bottom - 1;
    }
}

/* touch state machine */
static struct {
    int    down;          /* finger currently on the glass */
    int    left_down;     /* we have sent LEFTDOWN */
    int    pending;       /* right-click mode: waiting to decide click/drag/rclick */
    int    moved;         /* left the dead zone */
    int    rclicked;
    int    sx0, sy0;      /* touchdown screen position */
    int    lastx, lasty;
    double fx, fy;        /* smoothed raw coords */
    DWORD  t_down, t_last;
} ts;

static void touch_release(void)
{
    if (!ts.down) return;
    if (ts.pending && !ts.rclicked) {
        send_mouse(ts.sx0, ts.sy0, MOUSEEVENTF_LEFTDOWN);
        send_mouse(ts.sx0, ts.sy0, MOUSEEVENTF_LEFTUP);
    } else if (ts.left_down) {
        send_mouse(ts.lastx, ts.lasty, MOUSEEVENTF_LEFTUP);
    }
    memset(&ts, 0, sizeof ts);
    dbg("  release");
}

static void touch_event(int touching, int rx, int ry)
{
    DWORD now = GetTickCount();
    int sx, sy, dx, dy;
    double alpha;

    if (!touching) { touch_release(); return; }

    alpha = 1.0 / (1.0 + (cfg.smoothing < 0 ? 0 : cfg.smoothing));
    if (!ts.down) {
        ts.fx = rx; ts.fy = ry;
    } else {
        ts.fx += alpha * (rx - ts.fx);
        ts.fy += alpha * (ry - ts.fy);
    }
    raw_to_screen(ts.fx, ts.fy, &sx, &sy);
    ts.t_last = now;

    if (!ts.down) {
        ts.down = 1;
        ts.sx0 = ts.lastx = sx;
        ts.sy0 = ts.lasty = sy;
        ts.t_down = now;
        if (cfg.rightclick_ms > 0) {
            ts.pending = 1;
            send_mouse(sx, sy, 0);
        } else {
            send_mouse(sx, sy, MOUSEEVENTF_LEFTDOWN);
            ts.left_down = 1;
        }
        dbg("  down at %d,%d", sx, sy);
        return;
    }

    if (ts.rclicked) return;     /* ignore rest of this touch */

    dx = sx - ts.sx0; dy = sy - ts.sy0;
    if (!ts.moved && dx * dx + dy * dy > cfg.deadzone * cfg.deadzone) {
        ts.moved = 1;
        if (ts.pending) {        /* start a drag from the touchdown point */
            send_mouse(ts.sx0, ts.sy0, MOUSEEVENTF_LEFTDOWN);
            ts.left_down = 1;
            ts.pending = 0;
        }
    }

    if (ts.pending && !ts.moved && cfg.rightclick_ms > 0 &&
        now - ts.t_down >= (DWORD)cfg.rightclick_ms) {
        send_mouse(ts.sx0, ts.sy0, MOUSEEVENTF_RIGHTDOWN);
        send_mouse(ts.sx0, ts.sy0, MOUSEEVENTF_RIGHTUP);
        ts.rclicked = 1;
        ts.pending = 0;
        dbg("  right click");
        return;
    }

    if (ts.moved && (sx != ts.lastx || sy != ts.lasty)) {
        send_mouse(sx, sy, 0);
        ts.lastx = sx; ts.lasty = sy;
    }
}

/* ------------------------------------------------------------------ */
/* protocol parser                                                     */
/* ------------------------------------------------------------------ */
static volatile int g_mode = 1;
static volatile LONG g_npkts = 0;
static volatile int  g_last_bit = -1, g_last_x = 0, g_last_y = 0;
static volatile LONG g_nbytes = 0;

/* In stream mode the controller sends many packets while the finger is down
 * and a single packet on liftoff. If we see a long run of packets whose touch
 * bit is 0, the bit is inverted on this controller: flip it and remember. */
static void polarity_check(int bit)
{
    static int last = -1, run = 0;
    if (bit == last) run++; else { last = bit; run = 1; }
    if (bit == 0 && run == 6 && !cfg.swap_touch_bit) {
        cfg.swap_touch_bit = 1;
        WritePrivateProfileStringA("touch", "swap_touch_bit", "1", g_ini);
        dbg("  touch bit looks inverted on this controller -> swap_touch_bit=1");
    }
}

static void handle_packet(int touching, int rx, int ry)
{
    HWND cal = g_cal_hwnd;
    InterlockedIncrement(&g_npkts);
    g_last_bit = touching; g_last_x = rx; g_last_y = ry;
    if (g_mode == 1) {
        polarity_check(touching);
        if (cfg.swap_touch_bit) touching = !touching;
    }
    if (cal) {
        PostMessageA(cal, WM_TOUCHPKT, (WPARAM)touching, MAKELPARAM(rx, ry));
        return;
    }
    if (cfg.calibrated) touch_event(touching, rx, ry);
}

/* g_mode: 1 = Format Tablet (binary, 8-bit), 0 = Format Decimal text lines
 * "<SOH>xxx,yyy<CR>" (works even when the controller is in 7-bit mode; the
 * top bit of every byte is masked off). Decimal format has no lift-off
 * report, so lift-off is detected by a short gap in the stream.            */
static volatile int g_dec_down = 0;
static volatile DWORD g_dec_last = 0;
static int g_dec_lx = 0, g_dec_ly = 0;

static void parse_byte(unsigned char ch)
{
    static unsigned char pkt[5];
    static int  n = 0;
    static int  in_resp = 0;
    static char resp[64];
    static int  rn = 0;

    if (g_mode == 0) ch &= 0x7F;

    if (in_resp) {
        if (ch == CR) {
            int x, y;
            resp[rn] = 0;
            in_resp = 0; rn = 0;
            if (g_mode == 0 && sscanf(resp, "%d,%d", &x, &y) == 2) {
                g_dec_down = 1;
                g_dec_last = GetTickCount();
                g_dec_lx = x; g_dec_ly = y;
                dbg("pkt dec x=%5d y=%5d", x, y);
                handle_packet(1, x, y);
            } else {
                dbg("< %s", resp);
            }
        } else if (rn < 63) {
            resp[rn++] = (char)ch;
        }
        return;
    }
    if (g_mode == 1 && (ch & 0x80)) {   /* status byte starts a tablet packet */
        pkt[0] = ch; n = 1;
        return;
    }
    if (n == 0) {
        if (ch == SOH) { in_resp = 1; rn = 0; }
        return;                         /* stray byte */
    }
    pkt[n++] = ch;
    if (n == 5) {
        int x = pkt[1] | (pkt[2] << 7);
        int y = pkt[3] | (pkt[4] << 7);
        int touching = (pkt[0] & 0x40) ? 1 : 0;
        n = 0;
        dbg("pkt %02X %02X %02X %02X %02X  touch=%d x=%5d y=%5d",
            pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], touching, x, y);
        handle_packet(touching, x, y);
    }
}

static DWORD WINAPI reader_thread(LPVOID p)
{
    unsigned char buf[256];
    DWORD n, i;
    (void)p;
    while (!g_quit) {
        if (!ReadFile(g_com, buf, sizeof buf, &n, NULL)) {
            Sleep(100);
            continue;
        }
        if (n) InterlockedExchangeAdd(&g_nbytes, (LONG)n);
        if (n && g_debug) {
            char hex[256 * 3 + 1];
            DWORD k, m = n > 32 ? 32 : n;
            hex[0] = 0;
            for (k = 0; k < m; k++) sprintf(hex + k * 3, "%02X ", buf[k]);
            dbg("rx %lu bytes: %s%s", n, hex, n > m ? "..." : "");
        }
        for (i = 0; i < n; i++) parse_byte(buf[i]);
        if (g_mode == 0 && g_dec_down) {
            DWORD lim = cfg.liftoff_timeout > 0 ? (DWORD)cfg.liftoff_timeout : 80;
            if (GetTickCount() - g_dec_last > lim) {
                g_dec_down = 0;
                dbg("  (lift-off)");
                handle_packet(0, g_dec_lx, g_dec_ly);
            }
        }
        else if (g_mode == 1 && cfg.liftoff_timeout > 0 && ts.down && !g_cal_hwnd &&
            GetTickCount() - ts.t_last > (DWORD)cfg.liftoff_timeout) {
            dbg("  (liftoff timeout)");
            touch_release();
        }
    }
    return 0;
}

/* send a command and wait for its <SOH>...<CR> reply (top bit masked).
 * Touch-coordinate lines that arrive meanwhile are skipped.
 * *raw8 is set to 1 if the reply's bytes had the top bit clear (8-bit mode). */
static int com_cmd(const char *cmd, char *out, int outsz, int *raw8)
{
    unsigned char ch;
    DWORD n, t0;
    int in = 0, k = 0, hi = 0;
    PurgeComm(g_com, PURGE_RXCLEAR);
    com_send(cmd);
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 600) {
        if (!ReadFile(g_com, &ch, 1, &n, NULL) || n == 0) continue;
        if (ch & 0x80) hi = 1;
        ch &= 0x7F;
        if (ch == SOH) { in = 1; k = 0; hi = 0; continue; }
        if (!in) continue;
        if (ch == CR) {
            out[k] = 0;
            if (strchr(out, ',')) { in = 0; continue; }   /* a touch report */
            if (raw8) *raw8 = !hi;
            dbg("< %s  (%s)", out, hi ? "7-bit" : "8-bit");
            return 1;
        }
        if (k < outsz - 1) out[k++] = (char)ch;
    }
    out[0] = 0;
    dbg("< (no reply)");
    return 0;
}

static int g_reset_ctl = 0;

static void controller_init(void)
{
    char r[64];
    int eight = 0;

    if (g_reset_ctl) {
        /* Restore Defaults: 9600 N81, Format Tablet, controller-side
         * calibration cleared so it reports the full sensor range. */
        com_cmd("RD", r, sizeof r, NULL);
        Sleep(800);
        PurgeComm(g_com, PURGE_RXCLEAR);
    }

    com_cmd("Z", r, sizeof r, &eight);
    if (!eight) {
        /* controller is in 7-bit mode: ask for no parity, 8 data, 1 stop */
        com_cmd("PN81", r, sizeof r, NULL);
        Sleep(200);
        com_cmd("Z", r, sizeof r, &eight);
    }
    if (eight && com_cmd("FT", r, sizeof r, NULL) && r[0] == '0') {
        g_mode = 1;
        dbg("controller in Format Tablet (binary) mode");
    } else {
        g_mode = 0;
        com_cmd("FD", r, sizeof r, NULL);     /* Format Decimal text */
        dbg("using Format Decimal (text) mode with gap-based lift-off");
    }
    com_cmd("MS", r, sizeof r, NULL);         /* Mode Stream */
    PurgeComm(g_com, PURGE_RXCLEAR);
}

/* ------------------------------------------------------------------ */
/* calibration                                                         */
/* ------------------------------------------------------------------ */
static struct {
    RECT   mon;
    int    idx;
    int    tx[NCAL], ty[NCAL];      /* targets, virtual-desktop px */
    double rx[NCAL], ry[NCAL];      /* measured raw */
    double sumx, sumy;
    int    nsamp;
    int    done;                    /* 1 ok, -1 cancelled */
    int    touching;
    int    phase;                   /* 0 = grid targets, 1 = trace the border */
    double ext[4][5];               /* most extreme mapped values: l, r, t, b */
    int    next[4];
    double curx, cury;
} cal;

/* keep the 5 most extreme values; e=0/2 keep smallest, 1/3 keep largest */
static void ext_add(int e, double v)
{
    int i, j, n = cal.next[e];
    int better_small = (e == 0 || e == 2);
    for (i = 0; i < n; i++)
        if (better_small ? v < cal.ext[e][i] : v > cal.ext[e][i]) break;
    if (i >= 5) return;
    for (j = (n < 5 ? n : 4); j > i; j--) cal.ext[e][j] = cal.ext[e][j - 1];
    cal.ext[e][i] = v;
    if (n < 5) cal.next[e]++;
}

/* robust extreme: the 3rd most extreme sample, ignoring single noise spikes */
static double ext_get(int e)
{
    int n = cal.next[e];
    if (n == 0) return -1e9;
    return cal.ext[e][n >= 3 ? 2 : n - 1];
}

static void edges_finish(void)
{
    double w = cal.mon.right - cal.mon.left, hh = cal.mon.bottom - cal.mon.top;
    double in[4];
    const char *keys[4] = {"edge_l", "edge_r", "edge_t", "edge_b"};
    int e;
    in[0] = (ext_get(0) - cal.mon.left) / w;
    in[1] = (cal.mon.right - 1 - ext_get(1)) / w;
    in[2] = (ext_get(2) - cal.mon.top) / hh;
    in[3] = (cal.mon.bottom - 1 - ext_get(3)) / hh;
    for (e = 0; e < 4; e++) {
        if (cal.next[e] == 0 || in[e] < 0 || in[e] > 0.15) in[e] = 0;  /* not traced */
        ini_put_d(keys[e], in[e]);
        dbg("  edge %s: glass stops %.1f%% in", keys[e], in[e] * 100);
    }
    cfg.edge_l = in[0]; cfg.edge_r = in[1]; cfg.edge_t = in[2]; cfg.edge_b = in[3];
}

static double det3(double m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
         - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
         + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* least-squares affine fit: out = p0*rx + p1*ry + p2 */
static int fit_axis(const double *rx, const double *ry, const int *out, int n,
                    double *p0, double *p1, double *p2)
{
    double M[3][3] = {{0}}, v[3] = {0}, D, T[3][3];
    int i, j, k;
    for (i = 0; i < n; i++) {
        double r[3];
        r[0] = rx[i]; r[1] = ry[i]; r[2] = 1.0;
        for (j = 0; j < 3; j++) {
            for (k = 0; k < 3; k++) M[j][k] += r[j] * r[k];
            v[j] += r[j] * out[i];
        }
    }
    D = det3(M);
    if (D > -1e-9 && D < 1e-9) return 0;
    for (k = 0; k < 3; k++) {
        double *res = (k == 0) ? p0 : (k == 1) ? p1 : p2;
        memcpy(T, M, sizeof T);
        for (j = 0; j < 3; j++) T[j][k] = v[j];
        *res = det3(T) / D;
    }
    return 1;
}

static void draw_target(HDC dc, int x, int y)
{
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HPEN old = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, x - 25, y, NULL); LineTo(dc, x + 26, y);
    MoveToEx(dc, x, y - 25, NULL); LineTo(dc, x, y + 26);
    Ellipse(dc, x - 12, y - 12, x + 13, y + 13);
    SelectObject(dc, ob);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static LRESULT CALLBACK cal_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        char text[256];
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        if (cal.phase == 1) {
            double w = cal.mon.right - cal.mon.left, hh = cal.mon.bottom - cal.mon.top;
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 160, 255));
            HPEN op = (HPEN)SelectObject(dc, pen);
            HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, rc.right, rc.bottom);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
            if (cal.touching) draw_target(dc, (int)(cal.curx - cal.mon.left),
                                              (int)(cal.cury - cal.mon.top));
            sprintf(text, "Last step: trace the border\n\n"
                          "Slide your finger slowly all the way around the edge of the glass,\n"
                          "pushing right into the bezel on all four sides and into each corner.\n\n"
                          "Furthest reached (px short of the edge):  left %.0f   right %.0f   top %.0f   bottom %.0f\n\n"
                          "Press Enter when done, or Esc to skip this step.",
                    cal.next[0] ? ext_get(0) - cal.mon.left : -1,
                    cal.next[1] ? cal.mon.right - 1 - ext_get(1) : -1,
                    cal.next[2] ? ext_get(2) - cal.mon.top : -1,
                    cal.next[3] ? cal.mon.bottom - 1 - ext_get(3) : -1);
            (void)w; (void)hh;
        } else if (cal.idx < NCAL) {
            draw_target(dc, cal.tx[cal.idx] - cal.mon.left,
                            cal.ty[cal.idx] - cal.mon.top);
            sprintf(text, "TouchMT calibration - point %d of %d\n\n"
                          "Touch the centre of the target and hold briefly, then lift.\n"
                          "Press Esc to cancel.%s",
                    cal.idx + 1, NCAL,
                    cal.touching ? "\n\n(touch detected...)" : "");
        } else {
            sprintf(text, "Calibration saved.");
        }
        {
            char st[200];
            RECT r2;
            GetClientRect(h, &r2);
            r2.top = r2.bottom - 60;
            sprintf(st, "Port %s   bytes received: %ld   packets: %ld   "
                        "last packet: touch-bit=%d x=%d y=%d   swap_touch_bit=%d",
                    cfg.port, g_nbytes, g_npkts, g_last_bit, g_last_x, g_last_y,
                    cfg.swap_touch_bit);
            SetTextColor(dc, RGB(160, 160, 160));
            DrawTextA(dc, st, -1, &r2, DT_CENTER | DT_TOP);
        }
        rc.top = (rc.bottom - rc.top) / 2 - 40;
        DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_TOP);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TOUCHPKT: {
        int touching = (int)w;
        int rx = LOWORD(l), ry = HIWORD(l);
        if (cal.phase == 1) {
            if (touching) {
                double x, y;
                raw_to_screen_base(rx, ry, &x, &y);
                cal.curx = x; cal.cury = y;
                ext_add(0, x); ext_add(1, x); ext_add(2, y); ext_add(3, y);
            }
            cal.touching = touching;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (cal.idx >= NCAL) return 0;
        if (touching) {
            if (!cal.touching) { cal.touching = 1; cal.nsamp = 0; cal.sumx = cal.sumy = 0;
                                 InvalidateRect(h, NULL, FALSE); }
            cal.nsamp++;
            if (cal.nsamp > 3) { cal.sumx += rx; cal.sumy += ry; }  /* skip settling */
        } else if (cal.touching) {
            int used = cal.nsamp - 3;
            cal.touching = 0;
            if (used >= 1) {
                cal.rx[cal.idx] = cal.sumx / used;
                cal.ry[cal.idx] = cal.sumy / used;
                dbg("  cal point %d: raw %.0f,%.0f -> screen %d,%d", cal.idx + 1,
                     cal.rx[cal.idx], cal.ry[cal.idx], cal.tx[cal.idx], cal.ty[cal.idx]);
                cal.idx++;
                MessageBeep(MB_OK);             /* Windows "Default Beep" */
            }
            if (cal.idx >= NCAL) {
                double a, b, c, d, e, f;
                if (fit_axis(cal.rx, cal.ry, cal.tx, NCAL, &a, &b, &c) &&
                    fit_axis(cal.rx, cal.ry, cal.ty, NCAL, &d, &e, &f)) {
                    int g;
                    cfg.a = a; cfg.b = b; cfg.c = c;
                    cfg.d = d; cfg.e = e; cfg.f = f;
                    for (g = 0; g < NCAL; g++) { g_rawx[g] = cal.rx[g]; g_rawy[g] = cal.ry[g]; }
                    save_calibration();
                    save_grid();
                    cal.phase = 1;
                    cal.touching = 0;
                    memset(cal.next, 0, sizeof cal.next);
                    dbg("  calibration: x=%.6f*rx%+.6f*ry%+.2f  y=%.6f*rx%+.6f*ry%+.2f",
                         a, b, c, d, e, f);
                    InvalidateRect(h, NULL, FALSE);
                } else {
                    MessageBoxA(h, "Calibration points were degenerate. Please try again.",
                                APP_NAME, MB_OK | MB_ICONWARNING);
                    cal.idx = 0;
                }
            }
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 2) { InvalidateRect(h, NULL, FALSE); return 0; }
        KillTimer(h, 1);
        DestroyWindow(h);
        return 0;
    case WM_KEYDOWN:
        if (cal.phase == 1 && (w == VK_RETURN || w == VK_ESCAPE)) {
            if (w == VK_RETURN) { edges_finish(); MessageBeep(MB_OK); }
            else { memset(cal.next, 0, sizeof cal.next); edges_finish(); }
            cal.done = 1;
            DestroyWindow(h);
        } else if (w == VK_ESCAPE) { cal.done = -1; DestroyWindow(h); }
        return 0;
    case WM_SETCURSOR:
        SetCursor(NULL);
        return TRUE;
    case WM_DESTROY:
        g_cal_hwnd = NULL;
        if (cal.done == 0) cal.done = -1;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static int run_calibration(HINSTANCE inst, int edges_only)
{
    WNDCLASSA wc;
    HWND h;
    MSG msg;
    int w, hgt, i;

    memset(&cal, 0, sizeof cal);
    cal.mon = touch_monitor_rect();
    w = cal.mon.right - cal.mon.left;
    hgt = cal.mon.bottom - cal.mon.top;
    for (i = 0; i < NCAL; i++) {
        cal.tx[i] = cal.mon.left + (int)(g_frac[i % GRID] * w + 0.5);
        cal.ty[i] = cal.mon.top + (int)(g_frac[i / GRID] * hgt + 0.5);
    }

    if (edges_only) { cal.idx = NCAL; cal.phase = 1; }

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = cal_proc;
    wc.hInstance = inst;
    wc.lpszClassName = CAL_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    h = CreateWindowExA(WS_EX_TOPMOST, CAL_CLASS, "TouchMT Calibration", WS_POPUP,
                        cal.mon.left, cal.mon.top, w, hgt, NULL, NULL, inst, NULL);
    if (!h) return 0;
    g_cal_hwnd = h;
    SetTimer(h, 2, 250, NULL);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    UpdateWindow(h);

    while (cal.done == 0 && GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (IsWindow(h)) DestroyWindow(h);
    g_cal_hwnd = NULL;
    return cal.done == 1;
}

/* ------------------------------------------------------------------ */
/* --widen-controller: use the controller's own 2-point calibration    */
/* (CX) to shrink its output so the screen edges no longer clip at 0 / */
/* 16383. The controller assumes its CX targets sit 1/8 in from the    */
/* edges; we show them only 5% in, so the whole screen maps inside its */
/* range with margin to spare. The grid calibration then does the rest.*/
/* ------------------------------------------------------------------ */
static int  cx_step = 0;        /* 0 = lower-left, 1 = upper-right, 2 = done */
static char cx_msg[160] = "";

static LRESULT CALLBACK cx_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT rc;
        char text[320];
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 0));
        if (cx_step < 2) {
            int W = rc.right, H = rc.bottom;
            int x = cx_step == 0 ? (int)(0.05 * W) : (int)(0.95 * W);
            int y = cx_step == 0 ? (int)(0.95 * H) : (int)(0.05 * H);
            draw_target(dc, x, y);
        }
        sprintf(text, "TouchMT - controller range setup (step %d of 2)\n\n"
                      "Touch the centre of the target, hold briefly, then lift.\n"
                      "Press Esc to cancel.\n\n%s",
                cx_step < 2 ? cx_step + 1 : 2, cx_msg);
        rc.top = rc.bottom / 2 - 50;
        DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_TOP);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { cx_step = -1; return 0; }
    if (m == WM_SETCURSOR) { SetCursor(NULL); return TRUE; }
    return DefWindowProcA(h, m, w, l);
}

/* wait for one <SOH>reply<CR> while keeping the window responsive */
static int cx_wait_reply(char *out, int outsz, DWORD timeout)
{
    unsigned char ch;
    DWORD n, t0 = GetTickCount();
    int in = 0, k = 0;
    MSG msg;
    while (GetTickCount() - t0 < timeout) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (cx_step < 0) return 0;
        if (!ReadFile(g_com, &ch, 1, &n, NULL) || n == 0) continue;
        if (!in) {                      /* skip binary touch packets */
            if (ch == SOH) { in = 1; k = 0; }
            continue;
        }
        if (ch == CR) { out[k] = 0; dbg("< %s", out); return 1; }
        if (ch & 0x80 || k >= outsz - 1) { in = 0; continue; }
        out[k++] = (char)ch;
    }
    return 0;
}

static int run_widen(HINSTANCE inst)
{
    WNDCLASSA wc;
    HWND h;
    RECT mon = touch_monitor_rect();
    char r[64];
    int ok = 0;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = cx_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "TouchMTCxWnd";
    RegisterClassA(&wc);
    h = CreateWindowExA(WS_EX_TOPMOST, wc.lpszClassName, "TouchMT", WS_POPUP,
                        mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                        NULL, NULL, inst, NULL);
    if (!h) return 0;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    UpdateWindow(h);

    cx_step = 0; cx_msg[0] = 0;
    PurgeComm(g_com, PURGE_RXCLEAR);
    com_send("CX");
    if (!cx_wait_reply(r, sizeof r, 3000) || r[0] != '0') {
        sprintf(cx_msg, "Controller did not accept the calibrate command (reply '%s').", r);
        goto out;
    }
    InvalidateRect(h, NULL, TRUE);
    if (!cx_wait_reply(r, sizeof r, 60000) || r[0] != '1') {
        sprintf(cx_msg, "First point not accepted (reply '%s').", cx_step < 0 ? "cancelled" : r);
        goto out;
    }
    MessageBeep(MB_OK);
    cx_step = 1;
    InvalidateRect(h, NULL, TRUE);
    if (!cx_wait_reply(r, sizeof r, 60000) || r[0] != '1') {
        sprintf(cx_msg, "Second point not accepted (reply '%s').", cx_step < 0 ? "cancelled" : r);
        goto out;
    }
    MessageBeep(MB_OK);
    cx_step = 2;
    ok = 1;
    strcpy(cx_msg, "Done. Now the 16-point calibration...");
out:
    dbg("widen: %s", ok ? "ok" : cx_msg);
    if (cx_step >= 0) {
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);
        Sleep(ok ? 1000 : 3000);
    }
    DestroyWindow(h);
    if (!ok) com_send("R");             /* make sure it leaves calibrate mode */
    Sleep(200);
    com_send("MS");
    Sleep(200);
    PurgeComm(g_com, PURGE_RXCLEAR);
    return ok;
}

/* ------------------------------------------------------------------ */
/* control window (lets "--stop" shut us down)                         */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK msg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_CLOSE) { InterlockedExchange(&g_quit, 1); PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static int stop_running_copy(void)
{
    HWND h = FindWindowExA(HWND_MESSAGE, NULL, MSG_CLASS, NULL);
    int i;
    if (!h) return 0;
    PostMessageA(h, WM_CLOSE, 0, 0);
    for (i = 0; i < 30; i++) {            /* wait up to 3 s for it to exit */
        Sleep(100);
        if (!FindWindowExA(HWND_MESSAGE, NULL, MSG_CLASS, NULL)) break;
    }
    Sleep(300);
    return 1;
}

static void list_monitors(void)
{
    char text[2048], line[160];
    int i;
    enum_monitors();
    strcpy(text, "Monitors (use the number as monitor= in touchmt.ini, 0 = primary):\n\n");
    for (i = 0; i < g_nmon; i++) {
        sprintf(line, "%d: %ldx%ld at (%ld,%ld)%s\n", i + 1,
                g_mon[i].right - g_mon[i].left, g_mon[i].bottom - g_mon[i].top,
                g_mon[i].left, g_mon[i].top, g_mon_primary[i] ? "  [primary]" : "");
        strcat(text, line);
    }
    MessageBoxA(NULL, text, APP_NAME, MB_OK | MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    int want_cal = 0, want_widen = 0, want_edges = 0;
    HANDLE mtx, thr;
    WNDCLASSA wc;
    MSG msg;
    char err[256];
    (void)prev; (void)show;

    InitializeCriticalSection(&g_logcs);
    if (strstr(cmdline, "--stop")) {
        if (!stop_running_copy())
            MessageBoxA(NULL, "TouchMT is not running.", APP_NAME, MB_OK);
        return 0;
    }
    if (strstr(cmdline, "--list"))      { list_monitors(); return 0; }
    if (strstr(cmdline, "--calibrate")) want_cal = 1;
    if (strstr(cmdline, "--reset-controller")) { g_reset_ctl = 1; want_cal = 1; }
    if (strstr(cmdline, "--widen-controller")) want_widen = 1;
    if (strstr(cmdline, "--edges")) want_edges = 1;
    if (strstr(cmdline, "--debug")) {
        g_debug = 1;
        AllocConsole();
        SetConsoleTitleA("TouchMT debug - close this window to quit");
        g_con = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    }

    /* a copy already running would hold the COM port: replace it */
    stop_running_copy();
    mtx = CreateMutexA(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        WaitForSingleObject(mtx, 3000);
    }

    load_config();
    load_grid();
    if (g_debug) {
        char lp[MAX_PATH], *q;
        strcpy(lp, g_ini);
        q = strrchr(lp, '\\');
        if (q) strcpy(q + 1, "touchmt.log"); else strcpy(lp, "touchmt.log");
        g_logfile = fopen(lp, "w");
    }
    dbg("TouchMT starting. ini: %s", g_ini);
    dbg("port %s @ %d, monitor %d, calibrated %d", cfg.port, cfg.baud,
         cfg.monitor, cfg.calibrated);

    if (!com_open()) {
        sprintf(err, "Could not open %s (error %lu).\n\n"
                     "Make sure nothing else (a printer, terminal program, or old "
                     "touch driver) is using the port, and that port= in touchmt.ini "
                     "is correct.", cfg.port, GetLastError());
        MessageBoxA(NULL, err, APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    controller_init();
    if (want_widen) {
        if (g_mode != 1) {
            MessageBoxA(NULL, "The controller is not in binary mode. Run "
                        "--reset-controller first, then --widen-controller.",
                        APP_NAME, MB_OK | MB_ICONWARNING);
        } else if (run_widen(inst)) {
            want_cal = 1;               /* controller output changed: redo grid */
        } else {
            MessageBoxA(NULL, cx_msg[0] ? cx_msg : "Range setup cancelled.",
                        APP_NAME, MB_OK | MB_ICONWARNING);
        }
    }
    thr = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
    SetThreadPriority(thr, THREAD_PRIORITY_ABOVE_NORMAL);

    if (!g_grid_ok) cfg.calibrated = 0;      /* old 4-point calibration: redo */
    if (want_cal || !cfg.calibrated) {
        if (!run_calibration(inst, 0) && !cfg.calibrated) {
            InterlockedExchange(&g_quit, 1);
            WaitForSingleObject(thr, 1000);
            CloseHandle(g_com);
            MessageBoxA(NULL, "Calibration was cancelled; TouchMT has no calibration "
                              "and will exit.", APP_NAME, MB_OK);
            return 1;
        }
    }

    if (want_edges && !want_cal && cfg.calibrated) run_calibration(inst, 1);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = msg_proc;
    wc.hInstance = inst;
    wc.lpszClassName = MSG_CLASS;
    RegisterClassA(&wc);
    g_msg_hwnd = CreateWindowExA(0, MSG_CLASS, APP_NAME, 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, NULL, inst, NULL);
    dbg("running. touch the screen; use 'touchmt.exe --stop' to quit.");

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    InterlockedExchange(&g_quit, 1);
    WaitForSingleObject(thr, 1000);
    touch_release();
    CloseHandle(g_com);
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
