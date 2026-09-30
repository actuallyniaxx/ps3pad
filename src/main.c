/*
 * ps3pad - Forwards a PC gamepad (USB/Bluetooth) to a PS3 running webMAN MOD
 *
 * Uses webMAN MOD's virtual pad:  http://<ip>/pad.ps3?hold_<buttons>
 *   - "hold" keeps the sent state until the next request
 *   - every request replaces the whole virtual pad state
 *   - "off" unregisters the virtual pad
 *
 * webMAN limitations (not this program's):
 *   - Sticks can only be fully deflected in 8 directions (0x00 / 0xFF), no fine analog.
 *   - When a stick is sent, directions stop affecting the D-pad in that request.
 *   - Only one analog stick per request.
 *   - It's HTTP: tens of ms of latency. Great for XMB/menus, meh for gaming.
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
  #define BADSOCK INVALID_SOCKET
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int sock_t;
  #define CLOSESOCK close
  #define BADSOCK (-1)
#endif

#define VERSION "0.1.0"
#define CMD_MAX 256

/* ------------------------------------------------------------------ config */

typedef enum { STICK_DPAD, STICK_ANALOG, STICK_OFF } stick_mode_t;

static struct {
    const char  *host;
    char         port[8];
    int          pad_index;     /* -1 = first one found */
    stick_mode_t stick;
    int          deadzone;      /* 0..32767 */
    int          trigger;       /* L2/R2 threshold, 0..32767 */
    int          dry_run;
    int          verbose;
    int          send_off;      /* send "off" on exit */
} cfg = { NULL, "80", -1, STICK_DPAD, 16000, 12000, 0, 0, 1 };

/* ------------------------------------------------------------ network (HTTP) */

static int http_get(const char *path)
{
    struct addrinfo hints, *res = NULL, *ai;
    sock_t s = BADSOCK;
    char req[512];
    int len, ok = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(cfg.host, cfg.port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "[net] can't resolve %s\n", cfg.host);
        return 0;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BADSOCK) continue;
#ifdef _WIN32
        DWORD tmo = 1500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
#else
        struct timeval tv = { 1, 500000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        CLOSESOCK(s);
        s = BADSOCK;
    }
    freeaddrinfo(res);

    if (s == BADSOCK) {
        fprintf(stderr, "[net] can't connect to %s:%s (PS3 on? webMAN loaded?)\n",
                cfg.host, cfg.port);
        return 0;
    }

    len = snprintf(req, sizeof req,
                   "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                   path, cfg.host);

    if (send(s, req, len, 0) == len) {
        /* Drain the response: webMAN processes and closes. */
        char buf[1024];
        int n, first = 1;
        while ((n = recv(s, buf, sizeof buf - 1, 0)) > 0) {
            if (first) {
                buf[n] = '\0';
                ok = (strstr(buf, " 200") != NULL);
                if (!ok && cfg.verbose) {
                    char *eol = strpbrk(buf, "\r\n");
                    if (eol) *eol = '\0';
                    fprintf(stderr, "[net] unexpected response: %s\n", buf);
                }
                first = 0;
            }
        }
        if (first) ok = 1; /* closed with no body: count it as ok */
    }

    CLOSESOCK(s);
    return ok;
}

/* ------------------------------------------ sender thread (coalesces state) */

static SDL_mutex *mtx;
static SDL_cond  *cnd;
static char       pending[CMD_MAX];
static int        has_pending = 0;
static int        quitting    = 0;

static void queue_cmd(const char *cmd)
{
    SDL_LockMutex(mtx);
    SDL_strlcpy(pending, cmd, sizeof pending); /* only the latest state matters */
    has_pending = 1;
    SDL_CondSignal(cnd);
    SDL_UnlockMutex(mtx);
}

static void send_cmd(const char *cmd)
{
    char path[CMD_MAX + 16];
    Uint32 t0;

    snprintf(path, sizeof path, "/pad.ps3?%s", cmd);
    if (cfg.dry_run) {
        printf("%s\n", path);
        fflush(stdout);
        return;
    }
    t0 = SDL_GetTicks();
    int ok = http_get(path);
    if (cfg.verbose)
        printf("%-40s %s %ums\n", path, ok ? "ok " : "ERR", SDL_GetTicks() - t0);
}

static int sender_thread(void *unused)
{
    char local[CMD_MAX];
    (void)unused;

    for (;;) {
        SDL_LockMutex(mtx);
        while (!has_pending && !quitting) SDL_CondWait(cnd, mtx);
        if (!has_pending && quitting) { SDL_UnlockMutex(mtx); break; }
        SDL_strlcpy(local, pending, sizeof local);
        has_pending = 0;
        SDL_UnlockMutex(mtx);

        send_cmd(local);
    }
    return 0;
}

/* ------------------------------------------------ gamepad -> webMAN command */

static const struct { SDL_GameControllerButton b; const char *name; } BTN_MAP[] = {
    { SDL_CONTROLLER_BUTTON_A,             "cross"    },  /* bottom */
    { SDL_CONTROLLER_BUTTON_B,             "circle"   },  /* right  */
    { SDL_CONTROLLER_BUTTON_X,             "square"   },  /* left   */
    { SDL_CONTROLLER_BUTTON_Y,             "triangle" },  /* top    */
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  "l1"       },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "r1"       },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK,     "l3"       },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    "r3"       },
    { SDL_CONTROLLER_BUTTON_BACK,          "select"   },
    { SDL_CONTROLLER_BUTTON_START,         "start"    },
    { SDL_CONTROLLER_BUTTON_GUIDE,         "psbtn"    },
    { SDL_CONTROLLER_BUTTON_MISC1,         "psbtn"    },  /* Share/Capture: alternative PS */
    { SDL_CONTROLLER_BUTTON_TOUCHPAD,      "psbtn"    },  /* DS4/DS5 touchpad: same */
};

static void add(char *cmd, const char *tok)
{
    /* psbtn can come from several physical buttons: don't repeat it */
    if (!strcmp(tok, "psbtn") && strstr(cmd, "psbtn")) return;
    SDL_strlcat(cmd, "_", CMD_MAX);
    SDL_strlcat(cmd, tok, CMD_MAX);
}

static void stick_dirs(Sint16 x, Sint16 y, int *u, int *d, int *l, int *r)
{
    *u = y < -cfg.deadzone;
    *d = y >  cfg.deadzone;
    *l = x < -cfg.deadzone;
    *r = x >  cfg.deadzone;
}

static void build_cmd(SDL_GameController *gc, char *cmd)
{
    size_t i;
    int u, d, l, r;

    SDL_strlcpy(cmd, "hold", CMD_MAX);
    if (!gc) return; /* no pad = everything released */

    for (i = 0; i < SDL_arraysize(BTN_MAP); i++)
        if (SDL_GameControllerGetButton(gc, BTN_MAP[i].b)) add(cmd, BTN_MAP[i].name);

    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > cfg.trigger) add(cmd, "l2");
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > cfg.trigger) add(cmd, "r2");

    /* D-pad directions (real + left stick in dpad mode) */
    u = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
    d = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    l = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    r = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    if (cfg.stick == STICK_DPAD) {
        int su, sd, sl, sr;
        stick_dirs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                   SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                   &su, &sd, &sl, &sr);
        u |= su; d |= sd; l |= sl; r |= sr;
    }

    if (u || d || l || r) {
        /* D-pad wins: webMAN can't mix D-pad and stick in one request */
        if (u) add(cmd, "up");
        if (d) add(cmd, "down");
        if (l) add(cmd, "left");
        if (r) add(cmd, "right");
        return;
    }

    if (cfg.stick == STICK_OFF) return;

    /* Analog sticks (full deflection, 8 directions, one per request).
       In dpad mode the left stick was already used above, so only the right one is left. */
    {
        const char *stick = NULL;
        int su, sd, sl, sr;

        if (cfg.stick == STICK_ANALOG) {
            stick_dirs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                       SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                       &su, &sd, &sl, &sr);
            if (su || sd || sl || sr) stick = "analogL";
        }
        if (!stick) {
            stick_dirs(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                       SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY),
                       &su, &sd, &sl, &sr);
            if (su || sd || sl || sr) stick = "analogR";
        }
        if (stick) {
            add(cmd, stick);
            if (su) add(cmd, "up");
            if (sd) add(cmd, "down");
            if (sl) add(cmd, "left");
            if (sr) add(cmd, "right");
        }
    }
}

/* --------------------------------------------------------------- gamepads */

static SDL_GameController *open_pad(void)
{
    int i, n = SDL_NumJoysticks();

    if (cfg.pad_index >= 0) {
        if (cfg.pad_index < n && SDL_IsGameController(cfg.pad_index))
            return SDL_GameControllerOpen(cfg.pad_index);
        return NULL;
    }
    for (i = 0; i < n; i++)
        if (SDL_IsGameController(i)) return SDL_GameControllerOpen(i);
    return NULL;
}

static void list_pads(void)
{
    int i, n = SDL_NumJoysticks(), found = 0;
    printf("Detected gamepads:\n");
    for (i = 0; i < n; i++) {
        printf("  [%d] %s%s\n", i,
               SDL_IsGameController(i) ? SDL_GameControllerNameForIndex(i) : SDL_JoystickNameForIndex(i),
               SDL_IsGameController(i) ? "" : "  (no mapping, unusable)");
        found = 1;
    }
    if (!found) printf("  none. Plug it in / pair it and try again.\n");
}

/* ------------------------------------------------------------------ main */

static void usage(const char *argv0)
{
    printf(
        "ps3pad " VERSION " - PC gamepad -> PS3 via webMAN MOD\n\n"
        "Usage: %s <ps3-ip> [options]\n"
        "       %s --list\n\n"
        "Options:\n"
        "  --port N          webMAN web server port (default 80)\n"
        "  --pad N           use gamepad N from --list (default: first one)\n"
        "  --stick MODE      dpad   = left stick acts as D-pad, right = analogR (default)\n"
        "                    analog = both sticks as analog (full deflection)\n"
        "                    off    = ignore sticks\n"
        "  --deadzone N      stick deadzone, 0-32767 (default 16000)\n"
        "  --trigger N       L2/R2 threshold, 0-32767 (default 12000)\n"
        "  --keep            don't send 'off' on exit (keeps the virtual pad registered)\n"
        "  --dry-run         send nothing, print the commands (test without a PS3)\n"
        "  -v, --verbose     print every request and its latency\n"
        "  --list            list gamepads and exit\n"
        "  -h, --help        show this help\n",
        argv0, argv0);
}

static int want_help = 0;

static int parse_args(int argc, char **argv, int *list_only)
{
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "Missing value for %s\n", a), exit(2), ""))
        if      (!strcmp(a, "--list"))    *list_only = 1;
        else if (!strcmp(a, "--port"))    SDL_strlcpy(cfg.port, NEXT(), sizeof cfg.port);
        else if (!strcmp(a, "--pad"))     cfg.pad_index = atoi(NEXT());
        else if (!strcmp(a, "--deadzone"))cfg.deadzone = atoi(NEXT());
        else if (!strcmp(a, "--trigger")) cfg.trigger  = atoi(NEXT());
        else if (!strcmp(a, "--keep"))    cfg.send_off = 0;
        else if (!strcmp(a, "--dry-run")) cfg.dry_run  = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) cfg.verbose = 1;
        else if (!strcmp(a, "--stick")) {
            const char *m = NEXT();
            if      (!strcmp(m, "dpad"))   cfg.stick = STICK_DPAD;
            else if (!strcmp(m, "analog")) cfg.stick = STICK_ANALOG;
            else if (!strcmp(m, "off"))    cfg.stick = STICK_OFF;
            else { fprintf(stderr, "Unknown stick mode: %s\n", m); return 0; }
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { want_help = 1; return 0; }
        else if (a[0] == '-') { fprintf(stderr, "Unknown option: %s\n", a); return 0; }
        else cfg.host = a;
#undef NEXT
    }
    return *list_only || cfg.host || cfg.dry_run;
}

int main(int argc, char **argv)
{
    SDL_GameController *gc = NULL;
    SDL_Thread *th;
    char cmd[CMD_MAX], last[CMD_MAX] = "";
    int list_only = 0, running = 1;

    if (!parse_args(argc, argv, &list_only)) { usage(argv[0]); return want_help ? 0 : 2; }
    if (!cfg.host) cfg.host = "127.0.0.1";

#ifdef _WIN32
    { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; } }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    /* No window: we want events even without focus */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "0");
    SDL_SetMainReady();

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    if (list_only) { list_pads(); SDL_Quit(); return 0; }

    mtx = SDL_CreateMutex();
    cnd = SDL_CreateCond();
    th  = SDL_CreateThread(sender_thread, "sender", NULL);

    printf("ps3pad " VERSION " -> %s:%s%s  (Ctrl+C to quit)\n",
           cfg.host, cfg.port, cfg.dry_run ? " [dry-run]" : "");

    gc = open_pad();
    if (gc) printf("Gamepad: %s\n", SDL_GameControllerName(gc));
    else    printf("Waiting for a gamepad...\n");

    while (running) {
        SDL_Event ev;
        int dirty = 0;

        if (SDL_WaitEventTimeout(&ev, 250)) {
            do {
                switch (ev.type) {
                case SDL_QUIT:
                    running = 0;
                    break;
                case SDL_CONTROLLERDEVICEADDED:
                    if (!gc) {
                        gc = open_pad();
                        if (gc) { printf("Gamepad connected: %s\n", SDL_GameControllerName(gc)); dirty = 1; }
                    }
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (gc && ev.cdevice.which ==
                        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc))) {
                        printf("Gamepad disconnected, releasing everything.\n");
                        SDL_GameControllerClose(gc);
                        gc = open_pad(); /* in case there is another one */
                        if (gc) printf("Switched to: %s\n", SDL_GameControllerName(gc));
                        dirty = 1;
                    }
                    break;
                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP:
                case SDL_CONTROLLERAXISMOTION:
                    dirty = 1;
                    break;
                }
            } while (running && SDL_PollEvent(&ev));
        }

        if (dirty && running) {
            build_cmd(gc, cmd);
            /* Only send when the quantized state changes (axes change on
               every micro-movement, the command doesn't) */
            if (strcmp(cmd, last) != 0) {
                SDL_strlcpy(last, cmd, sizeof last);
                queue_cmd(cmd);
            }
        }
    }

    printf("\nExiting...\n");
    queue_cmd(cfg.send_off ? "off" : "hold");

    SDL_LockMutex(mtx);
    quitting = 1;
    SDL_CondSignal(cnd);
    SDL_UnlockMutex(mtx);
    SDL_WaitThread(th, NULL);

    if (gc) SDL_GameControllerClose(gc);
    SDL_DestroyCond(cnd);
    SDL_DestroyMutex(mtx);
    SDL_Quit();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
