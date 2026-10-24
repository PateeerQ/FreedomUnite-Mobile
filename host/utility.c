/* MHFU-Android -- sceUtility dialogs: savedata, system messages and the
 * on-screen keyboard.
 *
 * On a PSP these are system screens drawn over the game. Here there is no
 * screen: each request is carried out the moment it starts and the dialog
 * then reports itself finished. A message is printed to the log (and yes/no
 * questions are answered yes); savedata goes to the memory stick folder,
 * <root>/ms/PSP/SAVEDATA/<gameName><saveName>/.
 *
 * Saves are stored as the game hands them over, unencrypted: the PSP
 * encrypts savedata in the firmware, and the firmware is us. That means a
 * save from a real PSP or PPSSPP (encrypted) cannot be read yet.
 *
 * Every dialog follows the same status sequence the games poll for:
 * InitStart -> INITIALIZE -> RUNNING -> FINISHED, ShutdownStart -> SHUTDOWN
 * -> NONE. Each GetStatus moves it one step, so a game that waits for any of
 * them sees it. Struct offsets follow PPSSPP's Core/Dialog. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "psprecomp/hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define mkdir_one(p) _mkdir(p)
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <dirent.h>
#include <unistd.h>
#define mkdir_one(p) mkdir((p), 0777)
#endif

#include "utility.h"

enum { ST_NONE, ST_INIT, ST_RUNNING, ST_FINISHED, ST_SHUTDOWN };

#define SCE_ERROR_UTILITY_INVALID_STATUS 0x80110001u

#define SD_ERR_LOAD_NO_DATA      0x80110307u
#define SD_ERR_LOAD_PARAM        0x80110308u
#define SD_ERR_RW_NO_DATA        0x80110327u
#define SD_ERR_RW_FILE_NOT_FOUND 0x80110329u
#define SD_ERR_SAVE_ACCESS       0x80110385u
#define SD_ERR_SAVE_PARAM        0x80110388u
#define SD_ERR_DELETE_NO_DATA    0x80110347u
#define SD_ERR_SIZES_NO_DATA     0x801103C7u
#define SD_ERR_TYPE              0x80110300u

#define MS_CLUSTER   32768u
#define MS_FREE      (1024ull * 1024 * 1024)

/* SceUtilitySavedataParam */
enum {
    P_RESULT = 28, P_MODE = 48, P_OVERWRITE = 56, P_GAME = 60, P_SAVE = 76, P_SAVELIST = 96,
    P_FILE = 100, P_DATABUF = 116, P_DATABUFSIZE = 120, P_DATASIZE = 124,
    P_SFO_TITLE = 128, P_SFO_SAVETITLE = 256, P_SFO_DETAIL = 384, P_SFO_PARENTAL = 1408,
    P_ICON0 = 1412, P_ICON1 = 1428, P_PIC1 = 1444, P_SND0 = 1460,
    P_FOCUS = 1480, P_MSFREE = 1488, P_MSDATA = 1492, P_UTILDATA = 1496,
    P_IDLIST = 1524, P_FILELIST = 1528, P_SIZEINFO = 1532
};

static const char *const k_mode_name[] = {
    "AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE", "LISTDELETE",
    "LISTALLDELETE", "SIZES", "AUTODELETE", "DELETE", "LIST", "FILES", "MAKEDATASECURE",
    "MAKEDATA", "READDATASECURE", "READDATA", "WRITEDATASECURE", "WRITEDATA",
    "ERASESECURE", "ERASE", "DELETEDATA", "GETSIZE",
};

static char g_ms[600];          /* <root>/ms */

typedef struct { int status; uint32_t param; } dialog;
static dialog g_save, g_msg, g_osk;
static char g_osk_text[64] = "Hunter";

/* ---- helpers --------------------------------------------------------------- */

static void guest_str(uint32_t at, char *out, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap; i++) {
        out[i] = (char)psp_read8(at + (uint32_t)i);
        if (!out[i]) break;
    }
    out[i] = 0;
}

static void put_str(uint32_t at, const char *s, size_t cap) {
    size_t i = 0;
    for (; i + 1 < cap && s[i]; i++) psp_write8(at + (uint32_t)i, (uint8_t)s[i]);
    for (; i < cap; i++) psp_write8(at + (uint32_t)i, 0);
}

static void mkdirs(const char *path) {
    char tmp[800];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir_one(tmp);
            *p = c;
        }
    }
    mkdir_one(tmp);
}

static int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode) ? (long)st.st_size : -1;
}

static int write_file(const char *path, uint32_t buf, uint32_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const void *p = size ? psp_mem_ptr(buf, size) : NULL;
    int ok = !size || (p && fwrite(p, 1, size, f) == size);
    fclose(f);
    return ok ? 0 : -1;
}

/* Reads up to `cap` bytes into guest memory; the file's size, or -1. */
static long read_file(const char *path, uint32_t buf, uint32_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t take = (uint32_t)n < cap ? (uint32_t)n : cap;
    void *p = take ? psp_mem_ptr(buf, take) : NULL;
    if (take && (!p || fread(p, 1, take, f) != take)) n = -1;
    fclose(f);
    return n;
}

/* Callback per file name in a directory; nonzero from fn stops. */
static void each_entry(const char *dir, int want_dirs, void (*fn)(const char *name, void *ctx), void *ctx) {
#ifdef _WIN32
    char pat[800];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (is_dir == want_dirs) fn(fd.cFileName, ctx);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (dir_exists(p) == want_dirs) fn(e->d_name, ctx);
    }
    closedir(d);
#endif
}

static void space_text(uint64_t bytes, char out[8]) {
    char t[32];
    if (bytes < 1024ull * 1024) snprintf(t, sizeof t, "%u KB", (unsigned)(bytes / 1024));
    else snprintf(t, sizeof t, "%u MB", (unsigned)(bytes / (1024 * 1024)));
    memcpy(out, t, 7);
    out[7] = 0;
}

static uint32_t clusters(uint64_t bytes) { return (uint32_t)((bytes + MS_CLUSTER - 1) / MS_CLUSTER); }

/* ScePspDateTime: u16 year, month, day, hour, minute, second; u32 us. */
static void put_time(uint32_t at, time_t t) {
    struct tm *tm = localtime(&t);
    if (!tm) return;
    psp_write16(at,      (uint16_t)(tm->tm_year + 1900));
    psp_write16(at + 2,  (uint16_t)(tm->tm_mon + 1));
    psp_write16(at + 4,  (uint16_t)tm->tm_mday);
    psp_write16(at + 6,  (uint16_t)tm->tm_hour);
    psp_write16(at + 8,  (uint16_t)tm->tm_min);
    psp_write16(at + 10, (uint16_t)tm->tm_sec);
    psp_write32(at + 12, 0);
}

/* ---- PARAM.SFO ------------------------------------------------------------- */

/* The save's description, in the PSP's own SFO format, so a save folder looks
 * like one: TITLE, SAVEDATA_TITLE, SAVEDATA_DETAIL, SAVEDATA_DIRECTORY,
 * PARENTAL_LEVEL and CATEGORY. */
static void write_sfo(const char *path, uint32_t p, const char *dirname) {
    char title[129], stitle[129], detail[1025];
    guest_str(p + P_SFO_TITLE, title, sizeof title);
    guest_str(p + P_SFO_SAVETITLE, stitle, sizeof stitle);
    guest_str(p + P_SFO_DETAIL, detail, sizeof detail);
    struct { const char *key; const char *s; uint32_t num; uint32_t max; } k[] = {
        { "CATEGORY", "MS", 0, 4 },
        { "PARENTAL_LEVEL", NULL, psp_read8(p + P_SFO_PARENTAL), 4 },
        { "SAVEDATA_DETAIL", detail, 0, 1024 },
        { "SAVEDATA_DIRECTORY", dirname, 0, 64 },
        { "SAVEDATA_TITLE", stitle, 0, 128 },
        { "TITLE", title, 0, 128 },
    };
    const int n = (int)(sizeof k / sizeof k[0]);
    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);
    uint32_t keys = 20 + 16 * (uint32_t)n, klen = 0;
    for (int i = 0; i < n; i++) klen += (uint32_t)strlen(k[i].key) + 1;
    uint32_t data = (keys + klen + 3) & ~3u, koff = 0, doff = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *e = buf + 20 + 16 * i;
        uint32_t len = k[i].s ? (uint32_t)strlen(k[i].s) + 1 : 4;
        e[0] = (uint8_t)koff; e[1] = (uint8_t)(koff >> 8);
        e[2] = 4; e[3] = k[i].s ? 2 : 4;                  /* utf-8 string / u32 */
        memcpy(e + 4, &len, 4);
        memcpy(e + 8, &k[i].max, 4);
        memcpy(e + 12, &doff, 4);
        strcpy((char *)buf + keys + koff, k[i].key);
        if (k[i].s) memcpy(buf + data + doff, k[i].s, len - 1);
        else memcpy(buf + data + doff, &k[i].num, 4);
        koff += (uint32_t)strlen(k[i].key) + 1;
        doff += k[i].max;
    }
    memcpy(buf, "\0PSF", 4);
    uint32_t ver = 0x0101, cnt = (uint32_t)n;
    memcpy(buf + 4, &ver, 4);
    memcpy(buf + 8, &keys, 4);
    memcpy(buf + 12, &data, 4);
    memcpy(buf + 16, &cnt, 4);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, data + doff, f); fclose(f); }
}

/* ---- savedata ---------------------------------------------------------------- */

/* A save name of exactly "<>" means no name: the folder is the game ID alone,
 * as in PPSSPP's SavedataParam::GetSaveName. MHFU saves as ULES01213 this way;
 * taken literally, "<>" is not a valid Windows file name and every save failed. */
static void save_dir(uint32_t p, const char *save_name, char *out, size_t cap) {
    char game[14];
    guest_str(p + P_GAME, game, sizeof game);
    if (strcmp(save_name, "<>") == 0) save_name = "";
    snprintf(out, cap, "%s/PSP/SAVEDATA/%s%s", g_ms, game, save_name);
}

/* LISTLOAD and friends name the saves to choose from; with no screen to
 * choose on, the focus decides, and the default is the first that exists
 * (or, to save, the first that does not). */
static int pick_save(uint32_t p, int want_existing, char *out, size_t cap) {
    uint32_t list = psp_read32(p + P_SAVELIST);
    char name[21], dir[800];
    guest_str(p + P_SAVE, name, sizeof name);
    if (!list) { snprintf(out, cap, "%s", name); return 0; }
    uint32_t focus = psp_read32(p + P_FOCUS);
    char first[21] = "";
    for (int i = 0; i < 256; i++) {
        char n[21];
        guest_str(list + 20 * (uint32_t)i, n, sizeof n);
        if (!n[0]) break;
        if (!first[0]) snprintf(first, sizeof first, "%s", n);
        if (focus == 0 && strcmp(n, name) == 0) { snprintf(out, cap, "%s", n); return 0; }
        save_dir(p, n, dir, sizeof dir);
        if (dir_exists(dir) == want_existing) { snprintf(out, cap, "%s", n); return 0; }
    }
    if (want_existing) return -1;
    snprintf(out, cap, "%s", first[0] ? first : name);
    return 0;
}

/* A save name with "<>" in it matches any save whose name starts with the
 * part before it; the first existing one is used. 0 if one was found. */
struct wild_ctx { char prefix[40]; size_t glen; char found[21]; };
static void wild_one(const char *name, void *ctx) {
    struct wild_ctx *c = (struct wild_ctx *)ctx;
    if (!c->found[0] && strncmp(name, c->prefix, strlen(c->prefix)) == 0)
        snprintf(c->found, sizeof c->found, "%s", name + c->glen);
}

static int resolve_wild(uint32_t p, char *name, size_t cap) {
    char *w = strstr(name, "<>");
    if (!w || strcmp(name, "<>") == 0) return 0;   /* "<>" alone is no name: see save_dir */
    struct wild_ctx c;
    memset(&c, 0, sizeof c);
    char game[14], base[700];
    guest_str(p + P_GAME, game, sizeof game);
    *w = 0;
    snprintf(c.prefix, sizeof c.prefix, "%s%s", game, name);
    c.glen = strlen(game);
    snprintf(base, sizeof base, "%s/PSP/SAVEDATA", g_ms);
    each_entry(base, 1, wild_one, &c);
    if (!c.found[0]) return -1;
    snprintf(name, cap, "%s", c.found);
    return 0;
}

static uint32_t sd_load(uint32_t p, int list_mode) {
    char name[21], dir[800], file[14], path[900];
    if (list_mode ? pick_save(p, 1, name, sizeof name) != 0 : (guest_str(p + P_SAVE, name, sizeof name), 0))
        return SD_ERR_LOAD_NO_DATA;
    if (resolve_wild(p, name, sizeof name) != 0) return SD_ERR_LOAD_NO_DATA;
    put_str(p + P_SAVE, name, 20);
    save_dir(p, name, dir, sizeof dir);
    if (!dir_exists(dir)) return SD_ERR_LOAD_NO_DATA;
    guest_str(p + P_FILE, file, sizeof file);
    if (file[0]) {
        snprintf(path, sizeof path, "%s/%s", dir, file);
        long n = read_file(path, psp_read32(p + P_DATABUF), psp_read32(p + P_DATABUFSIZE));
        if (n < 0) return SD_ERR_LOAD_NO_DATA;
        psp_write32(p + P_DATASIZE, (uint32_t)n);
    }
    printf("savedata: loaded %s\n", dir);
    return 0;
}

static uint32_t sd_save(uint32_t p, int list_mode) {
    char name[21], dir[800], file[14], path[900], game[14];
    if (list_mode) pick_save(p, 0, name, sizeof name);
    else guest_str(p + P_SAVE, name, sizeof name);
    put_str(p + P_SAVE, name, 20);
    save_dir(p, name, dir, sizeof dir);
    mkdirs(dir);
    if (!dir_exists(dir)) return SD_ERR_SAVE_ACCESS;
    guest_str(p + P_FILE, file, sizeof file);
    if (file[0]) {
        snprintf(path, sizeof path, "%s/%s", dir, file);
        if (write_file(path, psp_read32(p + P_DATABUF), psp_read32(p + P_DATASIZE)) != 0)
            return SD_ERR_SAVE_ACCESS;
    }
    static const struct { uint32_t off; const char *name; } k_extra[] = {
        { P_ICON0, "ICON0.PNG" }, { P_ICON1, "ICON1.PMF" }, { P_PIC1, "PIC1.PNG" }, { P_SND0, "SND0.AT3" },
    };
    for (int i = 0; i < 4; i++) {
        uint32_t buf = psp_read32(p + k_extra[i].off), size = psp_read32(p + k_extra[i].off + 8);
        if (!buf || !size) continue;
        snprintf(path, sizeof path, "%s/%s", dir, k_extra[i].name);
        write_file(path, buf, size);
    }
    guest_str(p + P_GAME, game, sizeof game);
    char dirname[40];
    snprintf(dirname, sizeof dirname, "%s%s", game, name);
    snprintf(path, sizeof path, "%s/PARAM.SFO", dir);
    write_sfo(path, p, dirname);
    printf("savedata: saved %s\n", dir);
    return 0;
}

struct del_ctx { char dir[800]; };
static void del_one(const char *name, void *ctx) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", ((struct del_ctx *)ctx)->dir, name);
    remove(p);
}

static uint32_t sd_delete(uint32_t p, int list_mode) {
    char name[21];
    if (list_mode) { if (pick_save(p, 1, name, sizeof name) != 0) return SD_ERR_DELETE_NO_DATA; }
    else guest_str(p + P_SAVE, name, sizeof name);
    struct del_ctx c;
    save_dir(p, name, c.dir, sizeof c.dir);
    if (!dir_exists(c.dir)) return SD_ERR_DELETE_NO_DATA;
    each_entry(c.dir, 0, del_one, &c);
#ifdef _WIN32
    _rmdir(c.dir);
#else
    rmdir(c.dir);
#endif
    printf("savedata: deleted %s\n", c.dir);
    return 0;
}

struct size_ctx { char dir[800]; uint64_t clusters; };
static void add_size(const char *name, void *ctx) {
    struct size_ctx *c = (struct size_ctx *)ctx;
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", c->dir, name);
    long n = file_size(p);
    if (n > 0) c->clusters += clusters((uint64_t)n);
}

static uint32_t sd_sizes(uint32_t p) {
    uint32_t ret = 0;
    uint32_t fr = psp_read32(p + P_MSFREE);
    if (fr) {
        psp_write32(fr, MS_CLUSTER);
        psp_write32(fr + 4, (uint32_t)(MS_FREE / MS_CLUSTER));
        psp_write32(fr + 8, (uint32_t)(MS_FREE / 1024));
        char t[8]; space_text(MS_FREE, t);
        put_str(fr + 12, t, 8);
    }
    uint32_t md = psp_read32(p + P_MSDATA);
    if (md) {
        char game[14], name[21];
        guest_str(md, game, sizeof game);
        guest_str(md + 16, name, sizeof name);
        struct size_ctx c = { "", 0 };
        snprintf(c.dir, sizeof c.dir, "%s/PSP/SAVEDATA/%s%s", g_ms, game, strcmp(name, "<>") ? name : "");
        uint32_t info = md + 36;
        char t[8] = "";
        if (dir_exists(c.dir)) {
            each_entry(c.dir, 0, add_size, &c);
            space_text(c.clusters * MS_CLUSTER, t);
        } else {
            ret = SD_ERR_SIZES_NO_DATA;
        }
        psp_write32(info, (uint32_t)c.clusters);
        psp_write32(info + 4, (uint32_t)(c.clusters * MS_CLUSTER / 1024));
        put_str(info + 8, t, 8);
        psp_write32(info + 16, (uint32_t)(c.clusters * MS_CLUSTER / 1024));
        put_str(info + 20, t, 8);
    }
    uint32_t ud = psp_read32(p + P_UTILDATA);
    if (ud) {
        uint64_t total = 2 * (uint64_t)MS_CLUSTER;               /* directory + SFO */
        if (psp_read8(p + P_FILE)) total += (uint64_t)clusters(psp_read32(p + P_DATASIZE)) * MS_CLUSTER;
        const uint32_t extra[4] = { P_ICON0, P_ICON1, P_PIC1, P_SND0 };
        for (int i = 0; i < 4; i++) total += (uint64_t)clusters(psp_read32(p + extra[i] + 8)) * MS_CLUSTER;
        char t[8]; space_text(total, t);
        psp_write32(ud, (uint32_t)(total / MS_CLUSTER));
        psp_write32(ud + 4, (uint32_t)(total / 1024));
        put_str(ud + 8, t, 8);
        psp_write32(ud + 16, (uint32_t)(total / 1024));
        put_str(ud + 20, t, 8);
    }
    return ret;
}

/* LIST: the game's saves whose names start with gameName (and saveName,
 * unless it is "<>"). SceUtilitySavedataIdListEntry is 72 bytes. */
struct list_ctx { char prefix[40]; size_t glen; uint32_t entries, max, count; char base[800]; };
static void list_one(const char *name, void *ctx) {
    struct list_ctx *c = (struct list_ctx *)ctx;
    size_t pl = strlen(c->prefix);
    if (strncmp(name, c->prefix, pl) != 0 || c->count >= c->max) return;
    uint32_t e = c->entries + 72 * c->count++;
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", c->base, name);
    struct stat st;
    time_t t = stat(p, &st) == 0 ? st.st_mtime : time(NULL);
    psp_write32(e, 0x11FF);
    put_time(e + 4, t); put_time(e + 20, t); put_time(e + 36, t);
    put_str(e + 52, name + c->glen, 20);                /* the save name, without the game's */
}

static uint32_t sd_list(uint32_t p) {
    uint32_t il = psp_read32(p + P_IDLIST);
    if (!il) return SD_ERR_LOAD_PARAM;
    char game[14], name[21];
    guest_str(p + P_GAME, game, sizeof game);
    guest_str(p + P_SAVE, name, sizeof name);
    struct list_ctx c;
    memset(&c, 0, sizeof c);
    snprintf(c.prefix, sizeof c.prefix, "%s", game);
    c.entries = psp_read32(il + 8);
    c.max = psp_read32(il);
    snprintf(c.base, sizeof c.base, "%s/PSP/SAVEDATA", g_ms);
    c.glen = strlen(game);
    /* "<>" in the save name matches anything from there on. */
    char *wild = strstr(name, "<>");
    if (wild) *wild = 0;
    strncat(c.prefix, name, sizeof c.prefix - strlen(c.prefix) - 1);
    if (c.entries) each_entry(c.base, 1, list_one, &c);
    psp_write32(il + 4, c.count);
    return 0;
}

/* FILES: the normal files of one save. SceUtilitySavedataFileListEntry is
 * 80 bytes. Secure files are listed as normal ones; nothing here encrypts. */
struct files_ctx { char dir[800]; uint32_t entries, max, count; };
static void files_one(const char *name, void *ctx) {
    struct files_ctx *c = (struct files_ctx *)ctx;
    if (!strcmp(name, "PARAM.SFO") || c->count >= c->max) return;
    uint32_t e = c->entries + 80 * c->count++;
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", c->dir, name);
    long n = file_size(p);
    time_t t = time(NULL);
    psp_write32(e, 0x21FF);
    psp_write32(e + 4, 0);
    psp_write32(e + 8, (uint32_t)(n > 0 ? n : 0));
    psp_write32(e + 12, 0);
    put_time(e + 16, t); put_time(e + 32, t); put_time(e + 48, t);
    put_str(e + 64, name, 16);
}

static uint32_t sd_files(uint32_t p) {
    uint32_t fl = psp_read32(p + P_FILELIST);
    if (!fl) return SD_ERR_RW_FILE_NOT_FOUND;
    char name[21];
    guest_str(p + P_SAVE, name, sizeof name);
    struct files_ctx c;
    memset(&c, 0, sizeof c);
    save_dir(p, name, c.dir, sizeof c.dir);
    if (!dir_exists(c.dir)) return SD_ERR_RW_NO_DATA;
    c.entries = psp_read32(fl + 28);
    c.max = psp_read32(fl + 4);
    if (c.entries) each_entry(c.dir, 0, files_one, &c);
    psp_write32(fl + 12, 0);
    psp_write32(fl + 16, c.count);
    psp_write32(fl + 20, 0);
    return 0;
}

/* READDATA/WRITEDATA and the rest: one file of a save, no screens. */
static uint32_t sd_readdata(uint32_t p) {
    char name[21], dir[800], file[14], path[900];
    guest_str(p + P_SAVE, name, sizeof name);
    save_dir(p, name, dir, sizeof dir);
    if (!dir_exists(dir)) return SD_ERR_RW_NO_DATA;
    guest_str(p + P_FILE, file, sizeof file);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    long n = read_file(path, psp_read32(p + P_DATABUF), psp_read32(p + P_DATABUFSIZE));
    if (n < 0) return SD_ERR_RW_FILE_NOT_FOUND;
    psp_write32(p + P_DATASIZE, (uint32_t)n);
    return 0;
}

static uint32_t sd_getsize(uint32_t p) {
    uint32_t si = psp_read32(p + P_SIZEINFO);
    char name[21], dir[800];
    guest_str(p + P_SAVE, name, sizeof name);
    save_dir(p, name, dir, sizeof dir);
    if (si) {
        psp_write32(si + 16, MS_CLUSTER);
        psp_write32(si + 20, (uint32_t)(MS_FREE / MS_CLUSTER));
        psp_write32(si + 24, (uint32_t)(MS_FREE / 1024));
        char t[8]; space_text(MS_FREE, t);
        put_str(si + 28, t, 8);
        uint64_t need = 0;
        uint32_t ns = psp_read32(si), nn = psp_read32(si + 4);
        uint32_t se = psp_read32(si + 8), ne = psp_read32(si + 12);
        for (uint32_t i = 0; i < ns && se; i++) need += (uint64_t)clusters(psp_read32(se + 24 * i)) * MS_CLUSTER;
        for (uint32_t i = 0; i < nn && ne; i++) need += (uint64_t)clusters(psp_read32(ne + 24 * i)) * MS_CLUSTER;
        need += MS_CLUSTER;
        psp_write32(si + 36, (uint32_t)(need / 1024));
        space_text(need, t); put_str(si + 40, t, 8);
        psp_write32(si + 48, (uint32_t)(need / 1024));
        put_str(si + 52, t, 8);
    }
    return dir_exists(dir) ? 0 : SD_ERR_RW_NO_DATA;
}

static uint32_t savedata_run(uint32_t p) {
    uint32_t mode = psp_read32(p + P_MODE);
    char game[14], name[21], file[14];
    guest_str(p + P_GAME, game, sizeof game);
    guest_str(p + P_SAVE, name, sizeof name);
    guest_str(p + P_FILE, file, sizeof file);
    uint32_t r;
    switch (mode) {
    case 0: case 2:   r = sd_load(p, 0); break;
    case 4:           r = sd_load(p, 1); break;
    case 1: case 3:   r = sd_save(p, 0); break;
    case 5:           r = sd_save(p, 1); break;
    case 6: case 7:   r = sd_delete(p, 1); break;
    case 9: case 10: case 21: r = sd_delete(p, 0); break;
    case 8:           r = sd_sizes(p); break;
    case 11:          r = sd_list(p); break;
    case 12:          r = sd_files(p); break;
    case 13: case 14: case 17: case 18: r = sd_save(p, 0); break;
    case 15: case 16: r = sd_readdata(p); break;
    case 19: case 20: r = sd_delete(p, 0); break;
    case 22:          r = sd_getsize(p); break;
    default:          r = SD_ERR_TYPE; break;
    }
    printf("savedata: %s game \"%s\" save \"%s\" file \"%s\" -> 0x%08X\n",
           mode < sizeof k_mode_name / sizeof k_mode_name[0] ? k_mode_name[mode] : "?",
           game, name, file, r);
    return r;
}

/* ---- the dialog state machine ---------------------------------------------------- */

static int start(dialog *d, uint32_t param) {
    if (d->status != ST_NONE) return -1;
    d->param = param;
    d->status = ST_INIT;
    return 0;
}

static uint32_t step(dialog *d) {
    int s = d->status;
    if (s == ST_INIT) d->status = ST_RUNNING;
    else if (s == ST_RUNNING) d->status = ST_FINISHED;
    else if (s == ST_SHUTDOWN) d->status = ST_NONE;
    return (uint32_t)s;
}

static void dlg_shutdown(dialog *d) {
    if (d->status == ST_FINISHED || d->status == ST_RUNNING) d->status = ST_SHUTDOWN;
}

static void hle_SavedataInitStart(void) {
    uint32_t p = psp_arg(0);
    if (start(&g_save, p) != 0) { psp_ret(SCE_ERROR_UTILITY_INVALID_STATUS); return; }
    psp_write32(p + P_RESULT, savedata_run(p));
    psp_ret(0);
}
static void hle_SavedataGetStatus(void)    { psp_ret(step(&g_save)); }
static void hle_SavedataUpdate(void)       { if (g_save.status == ST_RUNNING) g_save.status = ST_FINISHED; psp_ret(0); }
static void hle_SavedataShutdownStart(void) {
    if (g_save.status != ST_FINISHED && g_save.status != ST_RUNNING) { psp_ret(SCE_ERROR_UTILITY_INVALID_STATUS); return; }
    dlg_shutdown(&g_save);
    psp_ret(0);
}

/* pspMessageDialog: common (48), result, type (0 error code, 1 text),
 * errorNum, string[512], options, buttonPressed. */
#define MSG_OPTION_YESNO 0x10
static void hle_MsgDialogInitStart(void) {
    uint32_t p = psp_arg(0);
    if (start(&g_msg, p) != 0) { psp_ret(SCE_ERROR_UTILITY_INVALID_STATUS); return; }
    uint32_t size = psp_read32(p), type = psp_read32(p + 52);
    char text[513];
    guest_str(p + 60, text, sizeof text);
    uint32_t options = size >= 580 ? psp_read32(p + 572) : 0;
    if (type == 0) printf("message dialog: error 0x%08X\n", psp_read32(p + 56));
    else printf("message dialog: \"%s\"%s\n", text, (options & MSG_OPTION_YESNO) ? " [yes/no: answered yes]" : "");
    psp_write32(p + 28, 0);                    /* common.result */
    psp_write32(p + 48, 0);
    if (size >= 580) psp_write32(p + 576, 1);  /* buttonPressed: yes / OK */
    psp_ret(0);
}
static void hle_MsgDialogGetStatus(void)   { psp_ret(step(&g_msg)); }
static void hle_MsgDialogUpdate(void)      { if (g_msg.status == ST_RUNNING) g_msg.status = ST_FINISHED; psp_ret(0); }
static void hle_MsgDialogShutdownStart(void) {
    if (g_msg.status != ST_FINISHED && g_msg.status != ST_RUNNING) { psp_ret(SCE_ERROR_UTILITY_INVALID_STATUS); return; }
    dlg_shutdown(&g_msg);
    psp_ret(0);
}

/* The on-screen keyboard. SceUtilityOskParams: common (48), fieldCount,
 * fields -> SceUtilityOskData[]: language@8, inputtype@16, lines@20,
 * desc@28, intext@32, outtextlength@36, outtext@40, result@44,
 * outtextlimit@48 (52 bytes each). Strings are UTF-16. There is no keyboard
 * to show, so every field is answered with the text from --name. */
#define OSK_RESULT_CHANGED 2
static void utf16_to_ascii(uint32_t at, char *out, size_t cap) {
    size_t n = 0;
    if (at) for (uint32_t c; n + 1 < cap && (c = psp_read16(at + (uint32_t)n * 2)) != 0; n++)
        out[n] = c < 0x80 ? (char)c : '?';
    out[n] = 0;
}
static void hle_OskInitStart(void) {
    uint32_t p = psp_arg(0);
    if (start(&g_osk, p) != 0) { psp_ret(SCE_ERROR_UTILITY_INVALID_STATUS); return; }
    int32_t nfields = (int32_t)psp_read32(p + 48);
    uint32_t fields = psp_read32(p + 52);
    for (int32_t i = 0; i < nfields && i < 8 && fields; i++) {
        uint32_t f = fields + (uint32_t)i * 52;
        char desc[64], in[64];
        utf16_to_ascii(psp_read32(f + 28), desc, sizeof desc);
        utf16_to_ascii(psp_read32(f + 32), in, sizeof in);
        uint32_t out = psp_read32(f + 40), limit = psp_read32(f + 48);
        uint32_t room = psp_read32(f + 36);             /* outtextlength, in chars */
        if (limit && limit < room) room = limit + 1;
        uint32_t n = 0;
        if (out && room) {
            for (; g_osk_text[n] && n + 1 < room; n++) psp_write16(out + n * 2, (uint8_t)g_osk_text[n]);
            psp_write16(out + n * 2, 0);
        }
        psp_write32(f + 44, OSK_RESULT_CHANGED);
        printf("keyboard: \"%s\" (was \"%s\") -> \"%.*s\" (use run.ps1 -Name to change)\n",
               desc, in, (int)n, g_osk_text);
    }
    psp_write32(p + 28, 0);                     /* common.result */
    psp_ret(0);
}
static void hle_OskGetStatus(void)   { psp_ret(step(&g_osk)); }
static void hle_OskUpdate(void)      { if (g_osk.status == ST_RUNNING) g_osk.status = ST_FINISHED; psp_ret(0); }
static void hle_OskShutdownStart(void) {
    if (g_osk.status != ST_FINISHED && g_osk.status != ST_RUNNING) { psp_ret(SCE_ERROR_UTILITY_INVALID_STATUS); return; }
    dlg_shutdown(&g_osk);
    psp_ret(0);
}

void mhfu_utility_set_osk_text(const char *text) {
    if (text && *text) snprintf(g_osk_text, sizeof g_osk_text, "%s", text);
}

void mhfu_utility_register(const char *ms_dir) {
    snprintf(g_ms, sizeof g_ms, "%s", ms_dir ? ms_dir : "ms");
    memset(&g_save, 0, sizeof g_save);
    memset(&g_msg, 0, sizeof g_msg);
    memset(&g_osk, 0, sizeof g_osk);

    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",     hle_SavedataInitStart);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",     hle_SavedataGetStatus);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",        hle_SavedataUpdate);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart", hle_SavedataShutdownStart);
    psp_hle_register(0x2AD8E239, "sceUtility", "sceUtilityMsgDialogInitStart",     hle_MsgDialogInitStart);
    psp_hle_register(0x9A1C91D7, "sceUtility", "sceUtilityMsgDialogGetStatus",     hle_MsgDialogGetStatus);
    psp_hle_register(0x95FC253B, "sceUtility", "sceUtilityMsgDialogUpdate",        hle_MsgDialogUpdate);
    psp_hle_register(0x67AF3428, "sceUtility", "sceUtilityMsgDialogShutdownStart", hle_MsgDialogShutdownStart);
    psp_hle_register(0xF6269B82, "sceUtility", "sceUtilityOskInitStart",           hle_OskInitStart);
    psp_hle_register(0xF3F76017, "sceUtility", "sceUtilityOskGetStatus",           hle_OskGetStatus);
    psp_hle_register(0x4B85C861, "sceUtility", "sceUtilityOskUpdate",              hle_OskUpdate);
    psp_hle_register(0x3DFAEBA9, "sceUtility", "sceUtilityOskShutdownStart",       hle_OskShutdownStart);
}
