/* MHFU-Android -- see achievements.h. */
#include "achievements.h"

#if MHFU_RCHEEVOS

#include "psprecomp/mem.h"
#include "audio.h"
#include "window.h"

#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#define PLATFORM "Windows"
#else
#include <SDL3/SDL.h>
#include <sys/stat.h>
#define PLATFORM "Android"
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

#define USER_AGENT "FreedomUniteRecomp/0.1"

static rc_client_t *volatile g_rc;
static char g_cache[600];
static char g_agent[256];
static char g_user[64];          /* the display name once signed in */

/* A detached thread for `fn`. */
#ifdef _WIN32
typedef DWORD WINAPI thread_fn(LPVOID);
static int spawn(thread_fn *fn, void *arg) {
    HANDLE t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (!t) return -1;
    CloseHandle(t);
    return 0;
}
#define THREAD(name) static DWORD WINAPI name(LPVOID arg)
#define THREAD_END return 0
#else
typedef int SDLCALL thread_fn(void *);
static int spawn(thread_fn *fn, void *arg) {
    SDL_Thread *t = SDL_CreateThread(fn, "achievements", arg);
    if (!t) return -1;
    SDL_DetachThread(t);
    return 0;
}
#define THREAD(name) static int SDLCALL name(void *arg)
#define THREAD_END return 0
#endif

/* ---- memory: RetroAchievements' PSP address A is 0x08000000 + A ---- */

static uint32_t RC_CCONV read_memory(uint32_t address, uint8_t *buffer, uint32_t num_bytes, rc_client_t *client) {
    (void)client;
    if (address >= 0x02000000u) return 0;
    if (address + num_bytes > 0x02000000u) num_bytes = 0x02000000u - address;
    const uint8_t *p = (const uint8_t *)psp_mem_ptr(0x08000000u + address, num_bytes);
    if (!p) return 0;
    memcpy(buffer, p, num_bytes);
    return num_bytes;
}

/* ---- the disc, for the hash ---- */

#ifdef __ANDROID__
#include <unistd.h>

/* A disc chosen through Android's file picker comes as "/proc/self/fd/N";
 * opening that path again is refused (shared storage's permission check), so
 * the hash reads the descriptor itself, at a position of its own (as
 * psprecomp's iofilemgr.c does for the game). */
typedef struct { int fd; int64_t pos; } fd_reader;

static int fd_read(void *c, char *buf, int n) {
    fd_reader *r = (fd_reader *)c;
    ssize_t got = pread(r->fd, buf, (size_t)n, (off_t)r->pos);
    if (got > 0) r->pos += got;
    return (int)got;
}

static fpos_t fd_seek(void *c, fpos_t off, int whence) {
    fd_reader *r = (fd_reader *)c;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? r->pos : (int64_t)lseek(r->fd, 0, SEEK_END);
    if (base + (int64_t)off < 0) return -1;
    r->pos = base + (int64_t)off;
    return (fpos_t)r->pos;
}

static int fd_close(void *c) { free(c); return 0; }

static void *RC_CCONV disc_open(const char *path) {
    int fd;
    char tail;
    if (sscanf(path, "/proc/self/fd/%d%c", &fd, &tail) == 1) {
        fd_reader *r = (fd_reader *)calloc(1, sizeof *r);
        if (!r) return NULL;
        r->fd = fd;
        FILE *f = funopen(r, fd_read, NULL, fd_seek, fd_close);
        if (!f) free(r);
        return f;
    }
    return fopen(path, "rb");
}
static void RC_CCONV disc_seek(void *f, int64_t off, int origin) { fseeko((FILE *)f, (off_t)off, origin); }
static int64_t RC_CCONV disc_tell(void *f) { return (int64_t)ftello((FILE *)f); }
static size_t RC_CCONV disc_read(void *f, void *buf, size_t n) { return fread(buf, 1, n, (FILE *)f); }
static void RC_CCONV disc_close(void *f) { fclose((FILE *)f); }
#endif

/* ---- HTTP, on a thread per request ---- */

#ifdef _WIN32
static wchar_t *widen(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof *w);
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* GET, or POST when `post` is given. The body is malloc'd; the HTTP status,
 * or 0 when nothing came back. */
static int http(const char *url, const char *post, const char *ctype, char **body, size_t *len) {
    *body = NULL;
    *len = 0;
    wchar_t *wurl = widen(url);
    if (!wurl) return 0;
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    int status = 0;
    if (WinHttpCrackUrl(wurl, 0, 0, &uc)) {
        wchar_t object[4096];
        _snwprintf(object, 4096, L"%.*s%.*s", (int)uc.dwUrlPathLength, path, (int)uc.dwExtraInfoLength, extra);
        host[uc.dwHostNameLength] = 0;
        wchar_t *agent = widen(g_agent);
        HINTERNET s = WinHttpOpen(agent ? agent : L"" USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        free(agent);
        HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : NULL;
        HINTERNET r = c ? WinHttpOpenRequest(c, post ? L"POST" : L"GET", object, NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : NULL;
        if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
        wchar_t headers[256] = L"";
        if (post) _snwprintf(headers, 256, L"Content-Type: %hs\r\n", ctype ? ctype : "application/x-www-form-urlencoded");
        const DWORD plen = post ? (DWORD)strlen(post) : 0;
        if (r && WinHttpSendRequest(r, post ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
                                    post ? (LPVOID)post : WINHTTP_NO_REQUEST_DATA, plen, plen, 0) &&
            WinHttpReceiveResponse(r, NULL)) {
            DWORD st = 0, sl = sizeof st;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &st, &sl, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
                char *nb = (char *)realloc(*body, *len + avail + 1);
                if (!nb) break;
                *body = nb;
                if (!WinHttpReadData(r, *body + *len, avail, &got) || !got) break;
                *len += got;
            }
            if (*body) (*body)[*len] = 0;
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
    }
    free(wurl);
    return status;
}
#elif defined(__ANDROID__)
/* Through Java (MhfuActivity.http: HttpURLConnection, with Android's TLS);
 * it answers the status in 4 bytes, then the body. */
static int http(const char *url, const char *post, const char *ctype, char **body, size_t *len) {
    *body = NULL;
    *len = 0;
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject act = env ? (jobject)SDL_GetAndroidActivity() : NULL;
    if (!act) return 0;
    int status = 0;
    jclass cls = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, cls, "http", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[B");
    if (m) {
        jstring ju = (*env)->NewStringUTF(env, url);
        jstring jp = post ? (*env)->NewStringUTF(env, post) : NULL;
        jstring jc = ctype ? (*env)->NewStringUTF(env, ctype) : NULL;
        jbyteArray r = (jbyteArray)(*env)->CallStaticObjectMethod(env, cls, m, ju, jp, jc);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = NULL; }
        const jsize n = r ? (*env)->GetArrayLength(env, r) : 0;
        if (n >= 4) {
            jbyte *b = (*env)->GetByteArrayElements(env, r, NULL);
            status = (uint8_t)b[0] << 24 | (uint8_t)b[1] << 16 | (uint8_t)b[2] << 8 | (uint8_t)b[3];
            *body = (char *)malloc((size_t)n - 4 + 1);
            if (*body) { memcpy(*body, b + 4, (size_t)n - 4); (*body)[n - 4] = 0; *len = (size_t)n - 4; }
            (*env)->ReleaseByteArrayElements(env, r, b, JNI_ABORT);
        }
        if (r) (*env)->DeleteLocalRef(env, r);
        if (ju) (*env)->DeleteLocalRef(env, ju);
        if (jp) (*env)->DeleteLocalRef(env, jp);
        if (jc) (*env)->DeleteLocalRef(env, jc);
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteLocalRef(env, act);
    return status;
}
#endif

#if defined(_WIN32) || defined(__ANDROID__)
typedef struct {
    char *url, *post, *ctype;
    rc_client_server_callback_t callback;
    void *data;
} call;

static char *str_dup(const char *s) {
    if (!s) return NULL;
    const size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

THREAD(call_thread) {
    call *c = (call *)arg;
    char *body;
    size_t len;
    const int status = http(c->url, c->post, c->ctype, &body, &len);
    rc_api_server_response_t resp;
    memset(&resp, 0, sizeof resp);
    resp.body = body ? body : "";
    resp.body_length = len;
    resp.http_status_code = status ? status : RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    c->callback(&resp, c->data);
    free(body);
    free(c->url); free(c->post); free(c->ctype);
    free(c);
    THREAD_END;
}

/* The request is gone once this returns, so it is copied first. */
static void RC_CCONV server_call(const rc_api_request_t *request, rc_client_server_callback_t callback,
                                 void *callback_data, rc_client_t *client) {
    (void)client;
    call *c = (call *)calloc(1, sizeof *c);
    if (!c) return;
    c->url = str_dup(request->url);
    c->post = str_dup(request->post_data);
    c->ctype = str_dup(request->content_type);
    c->callback = callback;
    c->data = callback_data;
    if (spawn(call_thread, c) != 0) call_thread(c);
}

/* The badge, into the cache shared with the launcher (badge_<name>.png). */
static int fetch_badge(const char *url, const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/badge_%s.png", g_cache, name);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return 0; }
    char *body;
    size_t len;
    if (http(url, NULL, NULL, &body, &len) != 200 || !len) { free(body); out[0] = 0; return -1; }
#ifdef _WIN32
    CreateDirectoryA(g_cache, NULL);
#else
    mkdir(g_cache, 0755);
#endif
    f = fopen(out, "wb");
    if (f) { fwrite(body, 1, len, f); fclose(f); }
    free(body);
    return f ? 0 : -1;
}
#endif

/* ---- events ---- */

typedef struct {
    char head[64], title[256], text[512], url[512], name[16];
    int chime;
} toast;

#if defined(_WIN32) || defined(__ANDROID__)
/* The badge is fetched off the game's thread, then the toast is shown. */
THREAD(toast_thread) {
    toast *t = (toast *)arg;
    char path[700] = "";
    if (t->url[0]) fetch_badge(t->url, t->name, path, sizeof path);
    mhfu_window_notify(t->head, t->title, t->text, path[0] ? path : NULL);
    if (t->chime) mhfu_audio_chime();
    free(t);
    THREAD_END;
}
#endif

static void show(const char *head, const char *title, const char *text, const char *url, const char *name, int chime) {
    toast *t = (toast *)calloc(1, sizeof *t);
    if (!t) return;
    snprintf(t->head, sizeof t->head, "%s", head);
    snprintf(t->title, sizeof t->title, "%s", title ? title : "");
    snprintf(t->text, sizeof t->text, "%s", text ? text : "");
    snprintf(t->url, sizeof t->url, "%s", url ? url : "");
    snprintf(t->name, sizeof t->name, "%s", name ? name : "");
    t->chime = chime;
#if defined(_WIN32) || defined(__ANDROID__)
    if (spawn(toast_thread, t) != 0) free(t);
#else
    free(t);
#endif
}

static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *client) {
    char url[512] = "", text[600];
    switch (e->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
        const rc_client_achievement_t *a = e->achievement;
        rc_client_achievement_get_image_url(a, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED, url, sizeof url);
        snprintf(text, sizeof text, "%s", a->description);
        char head[64];
        snprintf(head, sizeof head, "ACHIEVEMENT UNLOCKED  \xC2\xB7  %u pts", a->points);
        printf("achievements: unlocked \"%s\" (%u, %u points)\n", a->title, a->id, a->points);
        show(head, a->title, text, url, a->badge_name, 1);
        break;
    }
    case RC_CLIENT_EVENT_GAME_COMPLETED: {
        const rc_client_game_t *g = rc_client_get_game_info(client);
        rc_client_user_game_summary_t s;
        rc_client_get_user_game_summary(client, &s);
        snprintf(text, sizeof text, "Every achievement, %u points.", s.points_unlocked);
        show("SET MASTERED", g ? g->title : "Monster Hunter Freedom Unite", text, NULL, NULL, 1);
        break;
    }
    case RC_CLIENT_EVENT_SERVER_ERROR:
        printf("achievements: server error in %s: %s\n", e->server_error->api, e->server_error->error_message);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        printf("achievements: offline; unlocks are kept and sent later\n");
        show("RETROACHIEVEMENTS", "Offline", "Unlocks are kept and sent when the connection is back.", NULL, NULL, 0);
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        printf("achievements: back online, pending unlocks sent\n");
        break;
    default:
        break;
    }
}

static void RC_CCONV on_log(const char *message, const rc_client_t *client) {
    (void)client;
    printf("rcheevos: %s\n", message);
}

static void RC_CCONV on_loaded(int result, const char *error, rc_client_t *client, void *userdata) {
    (void)userdata;
    if (result != RC_OK) {
        printf("achievements: could not load the game's set: %s\n", error ? error : "?");
        show("RETROACHIEVEMENTS", "No achievements", error ? error : "The game was not recognised.", NULL, NULL, 0);
        return;
    }
    const rc_client_game_t *g = rc_client_get_game_info(client);
    rc_client_user_game_summary_t s;
    rc_client_get_user_game_summary(client, &s);
    char text[200], url[512] = "", name[16] = "";
    snprintf(text, sizeof text, "%u of %u achievements unlocked  \xC2\xB7  %u of %u points  \xC2\xB7  softcore",
             s.num_unlocked_achievements, s.num_core_achievements, s.points_unlocked, s.points_core);
    printf("achievements: %s (game %u): %s\n", g ? g->title : "?", g ? g->id : 0, text);
    if (g) {
        rc_client_game_get_image_url(g, url, sizeof url);
        snprintf(name, sizeof name, "%s", g->badge_name);
    }
    show("RETROACHIEVEMENTS", g ? g->title : "Monster Hunter Freedom Unite", text, url, name, 0);
}

typedef struct { char iso[1024]; } load_args;
static load_args g_load;

static void RC_CCONV on_login(int result, const char *error, rc_client_t *client, void *userdata) {
    (void)userdata;
    if (result != RC_OK) {
        printf("achievements: sign-in failed: %s\n", error ? error : "?");
        show("RETROACHIEVEMENTS", "Sign-in failed", error ? error : "Sign in again.", NULL, NULL, 0);
        return;
    }
    const rc_client_user_t *u = rc_client_get_user_info(client);
    printf("achievements: signed in as %s\n", u ? u->display_name : "?");
    snprintf(g_user, sizeof g_user, "%s", u ? u->display_name : "?");
    rc_client_begin_identify_and_load_game(client, RC_CONSOLE_PSP, g_load.iso, NULL, 0, on_loaded, NULL);
}

void mhfu_ach_start(const char *iso_path, const char *cache_dir) {
    snprintf(g_load.iso, sizeof g_load.iso, "%s", iso_path ? iso_path : "");
    snprintf(g_cache, sizeof g_cache, "%s", cache_dir ? cache_dir : ".");
    const char *user = getenv("MHFU_RA_USER"), *token = getenv("MHFU_RA_TOKEN");
    if (user && *user && token && *token) mhfu_ach_login(user, token);
}

void mhfu_ach_login(const char *user, const char *token) {
    if (!g_load.iso[0]) { printf("achievements: no disc image, so the game cannot be identified\n"); return; }
    if (!g_rc) {
        rc_client_t *rc = rc_client_create(read_memory, server_call);
        if (!rc) return;
        char clause[128] = "";
        rc_client_get_user_agent_clause(rc, clause, sizeof clause);
        snprintf(g_agent, sizeof g_agent, "%s (" PLATFORM ") %s", USER_AGENT, clause);
        rc_client_enable_logging(rc, RC_CLIENT_LOG_LEVEL_WARN, on_log);
        rc_client_set_event_handler(rc, on_event);
        rc_client_set_hardcore_enabled(rc, 0);
#ifdef __ANDROID__
        static struct rc_hash_filereader reader = { disc_open, disc_seek, disc_tell, disc_read, disc_close };
        rc_hash_init_custom_filereader(&reader);
#endif
        rc_hash_init_default_cdreader();
        g_rc = rc;
    } else {
        rc_client_logout(g_rc);
        g_user[0] = 0;
    }
    printf("achievements: signing in as %s\n", user);
    rc_client_begin_login_with_token(g_rc, user, token, on_login, NULL);
}

void mhfu_ach_logout(void) {
    if (!g_rc) return;
    rc_client_logout(g_rc);
    g_user[0] = 0;
    printf("achievements: signed out\n");
}

const char *mhfu_ach_user(void) { return g_user; }

void mhfu_ach_frame(void) {
    /* MHFU_TEST_TOAST=1: a sample unlock at the 300th frame, for the look. */
    static int frame, test = -1;
    if (test < 0) test = getenv("MHFU_TEST_TOAST") != NULL;
    if (test && ++frame == 300)
        show("ACHIEVEMENT UNLOCKED  \xC2\xB7  10 pts", "Welcome to Pokke Village",
             "A test of the unlock toast: this is how an earned achievement is shown.", NULL, NULL, 1);
    if (!g_rc) return;
#ifdef _WIN32
    /* How long the checks take, every 300 frames (10 s): they run on the
     * game's own thread, so whatever they cost, the game loses. */
    static LARGE_INTEGER freq;
    static double sum_us, max_us;
    static int n;
    LARGE_INTEGER a, b;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    rc_client_do_frame(g_rc);
    QueryPerformanceCounter(&b);
    const double us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)freq.QuadPart;
    sum_us += us;
    if (us > max_us) max_us = us;
    if (++n == 300) {
        printf("achievements: checks take %.0f us a frame on average, %.0f us at most\n", sum_us / n, max_us);
        sum_us = max_us = 0;
        n = 0;
    }
#else
    rc_client_do_frame(g_rc);
#endif
}

#else

void mhfu_ach_start(const char *iso_path, const char *cache_dir) { (void)iso_path; (void)cache_dir; }
void mhfu_ach_frame(void) {}
void mhfu_ach_login(const char *user, const char *token) { (void)user; (void)token; }
void mhfu_ach_logout(void) {}
const char *mhfu_ach_user(void) { return ""; }

#endif
