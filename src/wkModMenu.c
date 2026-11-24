/* wkModMenu: one place for the settings of WormKit modules, in the game (a WormKit module for Worms Armageddon 3.8.1).
 * WA's Esc menu gets one more row, "Mods". Clicked, WA's own menu steps aside and a panel of its own opens: the loaded
 * wk modules on the left, the chosen module's settings on the right. A click changes a setting; it is written to that
 * module's ini file at once, and the module is told (if it wants to be), so the change shows without restarting WA.
 *
 * What a module's settings are. A module can say so itself by exporting two functions (cdecl):
 *     const char *wkModSettings(void);        a text that describes its settings (the format is below)
 *     void wkModSettingsChanged(void);        called after a setting was written: read the ini again (optional)
 * Without them the module's ini file (its own name with .ini, beside WA.exe) is read as it is: numbers can be stepped,
 * on / off words switched, other text is shown only, and a change takes effect when that module reads its file again
 * (for most: at WA's next start).
 *
 * The description's text, one line each:
 *     wkModSettings 1                         the first line
 *     title=Map zoom                          the name shown (else the file's)
 *     file=wkMapZoom.ini                      the ini file, beside WA.exe
 *     section=wkMapZoom                       the ini section of the settings that follow
 *     live=1                                  changes show at once (else: "at WA's next start" is said)
 *     key|label|type|args|default|help        a setting. type: bool (0 / 1), choice (args a,b,c), int and num
 *                                             (args min:max:step), pct (a num shown in percent), key (a key of
 *                                             the keyboard: clicked, the next key pressed is taken; kept as text:
 *                                             "A", "Ctrl+A", "Alt+F6", "Shift+PageUp", "Numpad +", "Key 220"
 *                                             (a virtual-key number), "none")
 *
 * How it gets into WA's menu (found by wkVista, MIT, Copyright (c) 2026 ropahektic, github.com/ropahektic/wkVista, and
 * used by wkMapZoom up to 4.3). GameRuntime::OpenEscMenu 00535200 (stdcall, the runtime) lays its items out with
 * MenuPanel::AppendItem 005408F0 (ESI the panel, runtime +0x30; EAX the x; stack: id, label (kept), y, centred, a
 * slider's value and maximum; ret 0x18). Right after its first item (the call returns to 005356D3: EBP that row's y,
 * [esp+0x20] a row's height, [esp+0x2C] the buttons' x) this module appends "Mods" and moves EBP on. A click reaches
 * MenuPanel::ActivateAtCursor 00540810 (stdcall: the panel, where the id goes; ret 8).
 *
 * The panel. While WA's Esc menu is open (runtime +0x434 == 1) WA itself keeps the game from getting keys and the mouse,
 * moves its menu pointer (the panel's +0x10 / +0x14, kept inside +0x1C .. +0x28 by 005407D0) and calls ActivateAtCursor
 * at a click. This module leaves all of that running and only: widens the pointer's limits to the whole screen, has
 * WA's own menu not drawn (its drawing function 00535000 is skipped), draws its own panel and pointer after WA's frame
 * (through wkDirect3D in true colour when that is loaded and drawing, else into WA's 8-bit frame in WA's colours), and
 * takes the clicks. Esc closes it as it closes WA's menu; "Back" gives WA's menu back. Nothing of the game is changed.
 *
 * Hooks (WA 3.8.1 only: WA.exe checked, and the bytes at each hook point): 00533DC0 GameRender, 005356D3 (see above),
 * 00540810 ActivateAtCursor, 00535000 the Esc menu's drawing, 00528CD0 the runtime's end (the game is over), and a
 * WH_GETMESSAGE hook on WA's thread for the mouse wheel while the panel is open.
 * Log: %LOCALAPPDATA%\wkModMenu\wkModMenu.log (or the file named by WKMODMENU_LOG).
 * Test (isolated copies only): WKMODMENU_SCRIPT="1500:esc;2500:mods;3500:select=wkMapZoom;4500:set=AutoZoom,+;6000:close"
 * runs commands so many milliseconds after the first game frame: esc (WA's Esc menu opened), mods (the panel opened as
 * by a click on "Mods"), select=<module>, set=<key>,<+|-|next>, scroll=<rows>, point=<x>,<y> (WA's menu pointer put
 * there), click (a click where the pointer is), capture=<key setting> (it waits for a key), back, close (WA's menu
 * closed), say (the state logged);
 * WKMODMENU_NO_GPU=1 never draws through wkDirect3D.
 * MinHook (third_party/minhook) is BSD licensed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "MinHook.h"
#include "image_guard.h"

#define VERSION "0.1 beta"
#define WA_BASE 0x400000u
#define VA(a) ((uintptr_t)(base + ((uintptr_t)(a) - WA_BASE)))
#define U32(a) (*(uint32_t *)(uintptr_t)(a))
#define I32(a) (*(int32_t *)(uintptr_t)(a))
#define GAME_RENDER_VA 0x533dc0u
#define OPEN_MENU_VA 0x535200u
#define AFTER_FIRST_VA 0x5356d3u
#define APPEND_VA 0x5408f0u
#define ACTIVATE_VA 0x540810u
#define MENU_DRAW_VA 0x535000u
#define RUNTIME_GONE_VA 0x528cd0u
#define FRAME_PTR_VA 0x7a0eecu          /* WA's 8-bit frame: pixels, width (= row stride), height */
#define FRAME_W_VA 0x7a0ef0u
#define FRAME_H_VA 0x7a0ef4u
#define PAL_SHOWN_VA 0x8ac8c8u          /* the palette as shown: 256 x R, G, B, flags */
#define RT_PANEL 0x30                   /* the runtime: its Esc menu's panel, whether the menu is open, the world */
#define RT_MENU_OPEN 0x434
#define P_CURSOR 0x10                   /* the panel: its pointer (x, y) and the pointer's limits (x1, y1, x2, y2) */
#define P_LIMITS 0x1c
#define MODS_ID 0x5bu                   /* this module's row in WA's menu (WA's own ids are 0 to 4) */
#define MSG_KEY 0x00ff00ffu             /* the panel picture's "nothing here" colour */

static uintptr_t base;
static HINSTANCE self;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static HHOOK msg_hook;
static int installed, no_gpu;
static char wa_dir[MAX_PATH];           /* WA's folder, with the last backslash */

/* ---- the log ---- */
static void say(const char *fmt, ...) {
    char b[1024];
    SYSTEMTIME t;
    va_list v;
    DWORD n;
    int k, m;
    if (log_file == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&t);
    k = sprintf(b, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_start(v, fmt);
    m = _vsnprintf(b + k, sizeof(b) - k - 3, fmt, v);
    va_end(v);
    k = m < 0 ? (int)sizeof(b) - 3 : k + m;
    b[k++] = '\r'; b[k++] = '\n';
    WriteFile(log_file, b, (DWORD)k, &n, NULL);
}

static void open_log(void) {
    char p[MAX_PATH + 32];
    DWORD high = 0, size, n = GetEnvironmentVariableA("WKMODMENU_LOG", p, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        n = GetEnvironmentVariableA("LOCALAPPDATA", p, MAX_PATH);
        if (!n || n >= MAX_PATH) return;
        strcat(p, "\\wkModMenu");
        CreateDirectoryA(p, NULL);
        strcat(p, "\\wkModMenu.log");
    }
    log_file = CreateFileA(p, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(log_file, &high);
    if (high || size > 2u * 1024u * 1024u) SetEndOfFile(log_file);
    else SetFilePointer(log_file, 0, NULL, FILE_END);
}

/* ---- the modules and their settings ---- */
enum { T_TEXT, T_BOOL, T_CHOICE, T_INT, T_NUM, T_PCT, T_WORD, T_HEADER, T_KEY };
#define MAX_SETTINGS 96
#define MAX_MODS 32
#define MAX_CHOICES 10
typedef struct {
    char section[48], key[48], label[64], help[420], value[96], dflt[48];
    char choice[MAX_CHOICES][24];
    int type, nchoice, decimals;
    double lo, hi, step;
} setting;
typedef struct {
    char name[64], title[64], file[MAX_PATH];   /* wkMapZoom; what is shown; its ini's full path ("": none) */
    HMODULE h;
    const char *(__cdecl *describe)(void);
    void (__cdecl *changed)(void);
    int live, described;
} modinfo;
static modinfo mods[MAX_MODS];
static int n_mods, sel = -1;
static setting sets[MAX_SETTINGS];
static int n_sets;
static char status[200];

/* ---- a key as text: "A", "Ctrl+A", "Alt+F6", "Shift+PageUp", "Numpad +", "Key 220" (a virtual-key number), "none" ---- */
typedef struct { int vk, mods; } keybind;   /* mods: 1 Ctrl, 2 Alt, 4 Shift; vk 0: none */
static const struct { int vk; const char *name; } key_names[] = {
    {VK_SPACE, "Space"}, {VK_TAB, "Tab"}, {VK_RETURN, "Enter"}, {VK_BACK, "Backspace"}, {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"},
    {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_ADD, "Numpad +"}, {VK_SUBTRACT, "Numpad -"}, {VK_MULTIPLY, "Numpad *"},
    {VK_DIVIDE, "Numpad /"}, {VK_DECIMAL, "Numpad ."}, {VK_PAUSE, "Pause"}, {VK_SCROLL, "ScrollLock"}, {VK_CAPITAL, "CapsLock"}};
static void key_text(keybind k, char *out, size_t n) {
    char name[24] = "";
    unsigned i;
    if (!k.vk) { _snprintf(out, n, "none"); out[n - 1] = 0; return; }
    if ((k.vk >= 'A' && k.vk <= 'Z') || (k.vk >= '0' && k.vk <= '9')) sprintf(name, "%c", k.vk);
    else if (k.vk >= VK_F1 && k.vk <= VK_F24) sprintf(name, "F%d", k.vk - VK_F1 + 1);
    else if (k.vk >= VK_NUMPAD0 && k.vk <= VK_NUMPAD9) sprintf(name, "Numpad %d", k.vk - VK_NUMPAD0);
    else {
        for (i = 0; i < sizeof(key_names) / sizeof(key_names[0]); i++) if (key_names[i].vk == k.vk) sprintf(name, "%s", key_names[i].name);
        if (!name[0]) sprintf(name, "Key %d", k.vk);
    }
    _snprintf(out, n, "%s%s%s%s", k.mods & 1 ? "Ctrl+" : "", k.mods & 2 ? "Alt+" : "", k.mods & 4 ? "Shift+" : "", name);
    out[n - 1] = 0;
}
static int key_mods_now(void) { return (GetKeyState(VK_CONTROL) < 0 ? 1 : 0) | (GetKeyState(VK_MENU) < 0 ? 2 : 0) | (GetKeyState(VK_SHIFT) < 0 ? 4 : 0); }

static void trim(char *s) {
    char *a = s, *e;
    while (*a == ' ' || *a == '\t') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}
static void copy(char *to, size_t n, const char *from) { _snprintf(to, n, "%s", from); to[n - 1] = 0; }
static int exists(const char *path) { const DWORD a = GetFileAttributesA(path); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static const char *file_name(const char *path) { const char *s = strrchr(path, '\\'); return s ? s + 1 : path; }

static int is_number(const char *v, int *decimals) {
    const char *p = v;
    int digits = 0, dec = 0, dot = 0;
    if (*p == '-' || *p == '+') p++;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') { digits++; if (dot) dec++; }
        else if (*p == '.' && !dot) dot = 1;
        else return 0;
    }
    if (decimals) *decimals = dec;
    return digits > 0;
}

static const char *const word_pairs[][2] = {{"on", "off"}, {"true", "false"}, {"yes", "no"}};
static int word_pair(const char *v, const char **other) {
    unsigned i, k;
    for (i = 0; i < sizeof(word_pairs) / sizeof(word_pairs[0]); i++)
        for (k = 0; k < 2; k++)
            if (!lstrcmpiA(v, word_pairs[i][k])) {
                static char buf[8];
                const char *o = word_pairs[i][!k];
                copy(buf, sizeof(buf), o);
                if (v[0] >= 'A' && v[0] <= 'Z') buf[0] = (char)(buf[0] - 32);   /* (On <-> Off: its capital kept) */
                if (other) *other = buf;
                return 1;
            }
    return 0;
}

/* a module's ini file when it does not say: its own name, or (wkZRealTime -> wkRealTime, wkTrackMeBetter381 ->
 * wkTrackMeBetter) its name without a Z after "wk" or without digits at its end, or the first file that starts with it */
static void find_ini(modinfo *m) {
    char base_name[64], p[MAX_PATH], *e;
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int pass;
    m->file[0] = 0;
    for (pass = 0; pass < 3; pass++) {
        copy(base_name, sizeof(base_name), m->name);
        if (pass == 1) { if ((base_name[2] | 0x20) != 'z' || !base_name[3]) continue; memmove(base_name + 2, base_name + 3, strlen(base_name + 3) + 1); }
        if (pass == 2) { e = base_name + strlen(base_name); while (e > base_name + 3 && e[-1] >= '0' && e[-1] <= '9') *--e = 0; if (!strcmp(base_name, m->name)) continue; }
        _snprintf(p, sizeof(p), "%s%s.ini", wa_dir, base_name);
        p[sizeof(p) - 1] = 0;
        if (exists(p)) { copy(m->file, sizeof(m->file), p); return; }
    }
    _snprintf(p, sizeof(p), "%s%s*.ini", wa_dir, m->name);
    p[sizeof(p) - 1] = 0;
    f = FindFirstFileA(p, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { _snprintf(m->file, sizeof(m->file), "%s%s", wa_dir, fd.cFileName); m->file[sizeof(m->file) - 1] = 0; }
        FindClose(f);
    }
}

static int mod_cmp(const void *a, const void *b) { return lstrcmpiA(((const modinfo *)a)->name, ((const modinfo *)b)->name); }

/* the wk modules WA has loaded now */
static void list_modules(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    MODULEENTRY32 me;
    char keep[64] = "";
    if (sel >= 0 && sel < n_mods) copy(keep, sizeof(keep), mods[sel].name);
    n_mods = 0; sel = -1;
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) do {
        modinfo *m;
        const size_t len = strlen(me.szModule);
        const char *d;
        if (_strnicmp(me.szModule, "wk", 2) || len < 7 || lstrcmpiA(me.szModule + len - 4, ".dll") || me.hModule == (HMODULE)self || n_mods == MAX_MODS) continue;
        m = &mods[n_mods++];
        memset(m, 0, sizeof(*m));
        copy(m->name, sizeof(m->name), me.szModule);
        m->name[len - 4 < sizeof(m->name) - 1 ? len - 4 : sizeof(m->name) - 1] = 0;
        copy(m->title, sizeof(m->title), m->name);
        m->h = me.hModule;
        m->describe = (const char *(__cdecl *)(void))(void *)GetProcAddress(me.hModule, "wkModSettings");
        m->changed = (void (__cdecl *)(void))(void *)GetProcAddress(me.hModule, "wkModSettingsChanged");
        d = m->describe ? m->describe() : NULL;
        if (d && !strncmp(d, "wkModSettings 1", 15)) {
            const char *p = d;
            m->described = 1;
            while (*p) {                    /* (its title, file and whether changes show at once; the settings when chosen) */
                char line[200];
                const char *nl = strchr(p, '\n');
                const size_t n = nl ? (size_t)(nl - p) : strlen(p);
                copy(line, sizeof(line) < n + 1 ? sizeof(line) : n + 1, p);
                trim(line);
                if (!strncmp(line, "title=", 6)) copy(m->title, sizeof(m->title), line + 6);
                else if (!strncmp(line, "file=", 5) && !strchr(line + 5, '\\') && !strchr(line + 5, '/') && !strchr(line + 5, ':')) {
                    _snprintf(m->file, sizeof(m->file), "%s%s", wa_dir, line + 5);
                    m->file[sizeof(m->file) - 1] = 0;
                } else if (!strncmp(line, "live=", 5)) m->live = atoi(line + 5) != 0;
                p += n + (nl ? 1 : 0);
            }
        } else find_ini(m);
    } while (Module32Next(snap, &me));
    CloseHandle(snap);
    qsort(mods, (size_t)n_mods, sizeof(modinfo), mod_cmp);
    {
        int i;
        for (i = 0; i < n_mods; i++) if (keep[0] && !lstrcmpiA(mods[i].name, keep)) sel = i;
        for (i = 0; i < n_mods && sel < 0; i++) if (mods[i].file[0]) sel = i;
        if (sel < 0 && n_mods) sel = 0;
    }
}

static void split(char *line, char sep, char **out, int max, int *n) {
    *n = 0;
    while (*n < max) {
        char *s = strchr(line, sep);
        out[(*n)++] = line;
        if (!s) break;
        *s = 0;
        line = s + 1;
    }
}

/* the chosen module's settings: as it describes them, with their values from its file; or its file as it is */
static void load_settings(void) {
    modinfo *m;
    n_sets = 0;
    if (sel < 0 || sel >= n_mods) return;
    m = &mods[sel];
    if (m->described) {
        const char *p = m->describe();
        char section[48];
        copy(section, sizeof(section), m->name);
        while (p && *p && n_sets < MAX_SETTINGS) {
            char line[700], *f[6];
            const char *nl = strchr(p, '\n');
            const size_t n = nl ? (size_t)(nl - p) : strlen(p);
            int nf;
            copy(line, sizeof(line) < n + 1 ? sizeof(line) : n + 1, p);
            p += n + (nl ? 1 : 0);
            trim(line);
            if (!strncmp(line, "section=", 8)) { copy(section, sizeof(section), line + 8); continue; }
            if (!strchr(line, '|')) continue;
            split(line, '|', f, 6, &nf);
            if (nf < 3) continue;
            {
                setting *s = &sets[n_sets];
                char *a[3];
                int na = 0;
                memset(s, 0, sizeof(*s));
                copy(s->section, sizeof(s->section), section);
                copy(s->key, sizeof(s->key), f[0]);
                copy(s->label, sizeof(s->label), f[1][0] ? f[1] : f[0]);
                if (nf > 4) copy(s->dflt, sizeof(s->dflt), f[4]);
                if (nf > 5) copy(s->help, sizeof(s->help), f[5]);
                if (!strcmp(f[2], "bool")) s->type = T_BOOL;
                else if (!strcmp(f[2], "key")) s->type = T_KEY;
                else if (!strcmp(f[2], "choice") && nf > 3) {
                    char *c[MAX_CHOICES];
                    int k, nc;
                    s->type = T_CHOICE;
                    split(f[3], ',', c, MAX_CHOICES, &nc);
                    for (k = 0; k < nc; k++) { copy(s->choice[k], sizeof(s->choice[k]), c[k]); trim(s->choice[k]); }
                    s->nchoice = nc;
                } else if ((!strcmp(f[2], "int") || !strcmp(f[2], "num") || !strcmp(f[2], "pct")) && nf > 3) {
                    s->type = !strcmp(f[2], "int") ? T_INT : !strcmp(f[2], "num") ? T_NUM : T_PCT;
                    split(f[3], ':', a, 3, &na);
                    s->lo = na > 0 ? atof(a[0]) : 0; s->hi = na > 1 ? atof(a[1]) : 100; s->step = na > 2 ? atof(a[2]) : 1;
                    if (!(s->step > 0)) s->step = 1;
                    s->decimals = s->type == T_INT ? 0 : s->step < 0.1 ? 2 : s->step < 1 ? 1 : 0;
                    if (s->type != T_INT && s->decimals < 2 && (s->lo != floor(s->lo * 10 + 0.5) / 10)) s->decimals = 2;
                } else s->type = T_TEXT;
                if (m->file[0]) GetPrivateProfileStringA(s->section, s->key, s->dflt, s->value, sizeof(s->value), m->file);
                else copy(s->value, sizeof(s->value), s->dflt);
                trim(s->value);
                n_sets++;
            }
        }
        return;
    }
    if (!m->file[0]) return;
    {   /* the file as it is: its sections, its keys, the comment lines above a key as that key's help */
        HANDLE f = CreateFileA(m->file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        static char text[128 * 1024];
        char section[48] = "", help[420] = "", *p, *next;
        DWORD got = 0;
        int sections = 0, first_header = -1;
        if (f == INVALID_HANDLE_VALUE) return;
        if (!ReadFile(f, text, sizeof(text) - 1, &got, NULL)) got = 0;
        CloseHandle(f);
        text[got] = 0;
        for (p = text; *p && n_sets < MAX_SETTINGS; p = next) {
            char line[600], *eq;
            char *nl = strchr(p, '\n');
            const size_t n = nl ? (size_t)(nl - p) : strlen(p);
            next = p + n + (nl ? 1 : 0);
            copy(line, sizeof(line) < n + 1 ? sizeof(line) : n + 1, p);
            trim(line);
            if (!line[0]) { help[0] = 0; continue; }
            if (line[0] == ';' || line[0] == '#') {
                char *c = line + 1;
                while (*c == ' ' || *c == ';' || *c == '#') c++;
                if (strlen(help) + strlen(c) + 2 < sizeof(help)) { if (help[0]) strcat(help, " "); strcat(help, c); }
                continue;
            }
            if (line[0] == '[') {
                char *e = strchr(line, ']');
                setting *s = &sets[n_sets];
                if (e) *e = 0;
                copy(section, sizeof(section), line + 1);
                memset(s, 0, sizeof(*s));
                s->type = T_HEADER;
                copy(s->label, sizeof(s->label), section);
                if (first_header < 0) first_header = n_sets;
                sections++;
                n_sets++;
                help[0] = 0;
                continue;
            }
            eq = strchr(line, '=');
            if (!eq || !section[0]) { help[0] = 0; continue; }
            {
                setting *s = &sets[n_sets];
                const char *other;
                int dec = 0;
                memset(s, 0, sizeof(*s));
                *eq = 0;
                copy(s->section, sizeof(s->section), section);
                copy(s->key, sizeof(s->key), line); trim(s->key);
                copy(s->label, sizeof(s->label), s->key);
                copy(s->value, sizeof(s->value), eq + 1); trim(s->value);
                copy(s->help, sizeof(s->help), help);
                help[0] = 0;
                if (!s->key[0]) continue;
                if (is_number(s->value, &dec)) {
                    s->type = dec ? T_NUM : T_INT;
                    s->decimals = dec > 3 ? 3 : dec;
                    s->step = dec == 0 ? 1 : dec == 1 ? 0.1 : dec == 2 ? 0.05 : 0.001;
                    s->lo = -1e12; s->hi = 1e12;
                } else if (word_pair(s->value, &other)) s->type = T_WORD;
                else s->type = T_TEXT;
                n_sets++;
            }
        }
        if (sections == 1 && first_header >= 0) {   /* (one section: its name is no help) */
            memmove(&sets[first_header], &sets[first_header + 1], sizeof(setting) * (size_t)(n_sets - first_header - 1));
            n_sets--;
        }
    }
}

/* a setting's value as shown */
static void shown(const setting *s, char *out, size_t n) {
    if (s->type == T_BOOL) copy(out, n, atoi(s->value) || !lstrcmpiA(s->value, "on") || !lstrcmpiA(s->value, "yes") || !lstrcmpiA(s->value, "true") ? "On" : "Off");
    else if (s->type == T_PCT) _snprintf(out, n, "%.0f%%", atof(s->value) * 100.0);
    else copy(out, n, s->value[0] ? s->value : "(empty)");
    out[n - 1] = 0;
}

static void store(int i, const char *v);
/* a setting changed (dir: +1 / -1 a step or the next / last choice) */
static void change(int i, int dir) {
    modinfo *m = sel >= 0 && sel < n_mods ? &mods[sel] : NULL;
    setting *s;
    char v[96], was[96];
    if (!m || i < 0 || i >= n_sets) return;
    s = &sets[i];
    copy(was, sizeof(was), s->value);
    copy(v, sizeof(v), s->value);
    switch (s->type) {
    case T_BOOL: copy(v, sizeof(v), atoi(s->value) || !lstrcmpiA(s->value, "on") || !lstrcmpiA(s->value, "yes") || !lstrcmpiA(s->value, "true") ? "0" : "1"); break;
    case T_WORD: { const char *o; if (word_pair(s->value, &o)) copy(v, sizeof(v), o); break; }
    case T_CHOICE: {
        int k, at = -1;
        for (k = 0; k < s->nchoice; k++) if (!lstrcmpiA(s->value, s->choice[k])) at = k;
        if (s->nchoice > 0) copy(v, sizeof(v), s->choice[((at < 0 ? 0 : at + dir) % s->nchoice + s->nchoice) % s->nchoice]);
        break;
    }
    case T_INT: case T_NUM: case T_PCT: {
        double x = atof(s->value) + dir * s->step;
        x = floor(x / s->step + 0.5) * s->step;   /* (on the steps' grid) */
        if (x < s->lo) x = s->lo;
        if (x > s->hi) x = s->hi;
        _snprintf(v, sizeof(v), "%.*f", s->decimals, x);
        v[sizeof(v) - 1] = 0;
        break;
    }
    default: return;
    }
    store(i, v);
}

/* a setting's new value: written to the file, the module told */
static void store(int i, const char *v) {
    modinfo *m = sel >= 0 && sel < n_mods ? &mods[sel] : NULL;
    setting *s;
    char was[96];
    if (!m || i < 0 || i >= n_sets) return;
    s = &sets[i];
    copy(was, sizeof(was), s->value);
    if (!strcmp(v, was)) return;
    if (!m->file[0]) { _snprintf(m->file, sizeof(m->file), "%s%s.ini", wa_dir, m->name); m->file[sizeof(m->file) - 1] = 0; }
    if (!WritePrivateProfileStringA(s->section, s->key, v, m->file)) {
        _snprintf(status, sizeof(status), "Could not write %s (error %lu).", file_name(m->file), GetLastError());
        say("%s: %s=%s could not be written to %s (error %lu)", m->name, s->key, v, m->file, GetLastError());
        return;
    }
    copy(s->value, sizeof(s->value), v);
    if (m->changed) m->changed();
    _snprintf(status, sizeof(status), "%s = %s saved to %s. %s", s->label, v, file_name(m->file),
              m->changed && m->live ? "It applies now." : m->changed ? "The module was told." : "It applies when WA starts next.");
    status[sizeof(status) - 1] = 0;
    say("%s: [%s] %s: %s -> %s (%s)", m->name, s->section, s->key, was, v, m->changed ? "the module told" : "for WA's next start");
}

/* ---- the panel's picture ---- */
enum { A_NONE, A_BACK, A_MODULE, A_NEXT, A_MINUS, A_PLUS, A_UP, A_DOWN, A_ROW };
typedef struct { RECT r; int act, i; } hit;
static hit hits[256];
static int n_hits;
static int panel_open, hide_menu;       /* hide_menu: read by the thunk that skips WA's own menu drawing */
static uintptr_t runtime_now;
static int scroll, hover = -1, hover_row = -1, dirty = 1, rows_fit = 1;
static int capture = -1;                /* the key setting waiting for a key to be pressed (-1: none) */
static int sw, sh, pw, ph, px0, py0;    /* the screen; the panel's size and place */
static double scale = 1.0;
static HDC pdc;
static HBITMAP pbm;
static uint32_t *pbits, *ppre;          /* the panel as painted (0x00RRGGBB, MSG_KEY: nothing), and premultiplied ARGB */
static int pcap_w, pcap_h;
static HFONT f_title, f_text, f_small, f_bold;
static double font_scale;
static int ptr_x, ptr_y;                /* the pointer, on the screen */
static uint32_t arrow[24 * 24];
static int arrow_ok;

static int S(double v) { return (int)floor(v * scale + 0.5); }

static void make_fonts(void) {
    if (f_title && font_scale == scale) return;
    if (f_title) { DeleteObject(f_title); DeleteObject(f_text); DeleteObject(f_small); DeleteObject(f_bold); }
    f_title = CreateFontA(-S(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    f_text = CreateFontA(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    f_bold = CreateFontA(-S(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    f_small = CreateFontA(-S(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    font_scale = scale;
}

static void box(int x1, int y1, int x2, int y2, COLORREF fill, COLORREF edge, int round) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(pdc, b), op = SelectObject(pdc, p);
    if (round) RoundRect(pdc, x1, y1, x2, y2, round, round); else Rectangle(pdc, x1, y1, x2, y2);
    SelectObject(pdc, ob); SelectObject(pdc, op);
    DeleteObject(b); DeleteObject(p);
}
static void text(const char *s, int x1, int y1, int x2, int y2, HFONT f, COLORREF c, UINT how) {
    RECT r;
    HGDIOBJ of = SelectObject(pdc, f);
    r.left = x1; r.top = y1; r.right = x2; r.bottom = y2;
    SetBkMode(pdc, TRANSPARENT);
    SetTextColor(pdc, c);
    DrawTextA(pdc, s, -1, &r, how | DT_NOPREFIX);
    SelectObject(pdc, of);
}
static void add_hit(int x1, int y1, int x2, int y2, int act, int i) {
    if (n_hits == 256) return;
    hits[n_hits].r.left = x1; hits[n_hits].r.top = y1; hits[n_hits].r.right = x2; hits[n_hits].r.bottom = y2;
    hits[n_hits].act = act; hits[n_hits].i = i;
    n_hits++;
}
static int hit_at(int x, int y) {       /* (the last one painted there: the buttons lie over their rows) */
    int k;
    for (k = n_hits - 1; k >= 0; k--) if (x >= hits[k].r.left && x < hits[k].r.right && y >= hits[k].r.top && y < hits[k].r.bottom) return k;
    return -1;
}

#define C_BACK RGB(24, 30, 42)
#define C_EDGE RGB(70, 88, 120)
#define C_BAR RGB(34, 43, 60)
#define C_ROW RGB(40, 52, 74)
#define C_BTN RGB(52, 68, 98)
#define C_BTN_HOT RGB(78, 110, 168)
#define C_TEXT RGB(232, 236, 244)
#define C_DIM RGB(150, 160, 180)
#define C_ACCENT RGB(110, 180, 255)

/* the panel painted for the screen's size: the modules, the chosen one's settings, what the pointer is over */
static void paint(void) {
    BITMAPINFO bi;
    const modinfo *m = sel >= 0 && sel < n_mods ? &mods[sel] : NULL;
    int i, y, left_w, row_h, top, bottom, x_set, shown_rows, max_scroll;
    char b[200];
    scale = sh / 1080.0;
    if (scale < 0.9) scale = 0.9;
    if (scale > 2.2) scale = 2.2;
    if (820 * scale > sw - 16) scale = (sw - 16) / 820.0;
    if (560 * scale > sh - 16) scale = (sh - 16) / 560.0;
    pw = S(820); ph = S(560);
    px0 = (sw - pw) / 2; py0 = (sh - ph) / 2;
    if (!pdc) pdc = CreateCompatibleDC(NULL);
    if (!pbm || pw > pcap_w || ph > pcap_h) {
        if (pbm) DeleteObject(pbm);
        free(ppre);
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = pw; bi.bmiHeader.biHeight = -ph;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        pbm = CreateDIBSection(pdc, &bi, DIB_RGB_COLORS, (void **)&pbits, NULL, 0);
        ppre = (uint32_t *)malloc((size_t)pw * ph * 4);
        pcap_w = pw; pcap_h = ph;
        if (!pbm || !pbits || !ppre) { if (pbm) DeleteObject(pbm); pbm = NULL; pbits = NULL; free(ppre); ppre = NULL; pcap_w = pcap_h = 0; return; }
        SelectObject(pdc, pbm);
    }
    make_fonts();
    n_hits = 0;
    for (i = 0; i < pcap_w * ph; i++) pbits[i] = MSG_KEY;
    box(0, 0, pw, ph, C_BACK, C_EDGE, S(14));
    /* the top: the title, and the way back */
    text("Mods", S(22), S(12), S(300), S(50), f_title, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text("settings of the WormKit modules that are loaded", S(92), S(18), pw - S(130), S(50), f_small, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    {
        const int x1 = pw - S(112), y1 = S(14), x2 = pw - S(18), y2 = S(46), hot = hover >= 0 && hits[hover].act == A_BACK;
        (void)hot;
        box(x1, y1, x2, y2, C_BTN, C_EDGE, S(8));
        text("Back", x1, y1, x2, y2, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hit(x1, y1, x2, y2, A_BACK, 0);
    }
    /* the left: the modules */
    left_w = S(210); top = S(60); bottom = ph - S(96);
    box(S(14), top, left_w, bottom, C_BAR, C_BAR, S(8));
    row_h = S(30);                          /* (many modules: closer together, so that all are there) */
    if (n_mods * row_h > bottom - top - S(10)) row_h = (bottom - top - S(10)) / n_mods;
    if (row_h < S(17)) row_h = S(17);
    for (i = 0, y = top + S(6); i < n_mods && y + row_h <= bottom - S(4); i++, y += row_h) {
        const int on = i == sel;
        if (on) box(S(18), y, left_w - S(4), y + row_h - S(2), C_BTN_HOT, C_BTN_HOT, S(6));
        text(mods[i].title, S(28), y, left_w - S(8), y + row_h - S(2), on ? f_bold : f_text, mods[i].file[0] || mods[i].described ? C_TEXT : C_DIM,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_hit(S(18), y, left_w - S(4), y + row_h - S(2), A_MODULE, i);
    }
    if (!n_mods) text("No wk modules are loaded.", S(24), top + S(8), left_w - S(8), top + S(60), f_small, C_DIM, DT_LEFT | DT_WORDBREAK);
    /* the right: the chosen module's settings */
    x_set = left_w + S(14);
    if (m) {
        _snprintf(b, sizeof(b), "%s", m->title);
        text(b, x_set, top, pw - S(20), top + S(28), f_bold, C_ACCENT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (m->file[0]) _snprintf(b, sizeof(b), "%s%s", file_name(m->file), m->changed && m->live ? "  (changes apply at once)" : "  (changes apply when WA starts next)");
        else _snprintf(b, sizeof(b), "no settings file");
        b[sizeof(b) - 1] = 0;
        text(b, x_set + S(200), top, pw - S(20), top + S(28), f_small, C_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    row_h = S(28);
    y = top + S(34);
    shown_rows = (bottom - y) / row_h;
    if (shown_rows < 1) shown_rows = 1;
    rows_fit = shown_rows;
    max_scroll = n_sets > shown_rows ? n_sets - shown_rows : 0;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
    if (m && !n_sets)
        text(m->file[0] ? "This file has no settings in it." : "This module has no settings file beside WA.exe, and does not say what its settings are.",
             x_set, y + S(8), pw - S(24), y + S(80), f_text, C_DIM, DT_LEFT | DT_WORDBREAK);
    for (i = scroll; i < n_sets && i < scroll + shown_rows; i++, y += row_h) {
        const setting *s = &sets[i];
        const int x2 = pw - (max_scroll ? S(52) : S(20)), vy1 = y + S(3), vy2 = y + row_h - S(3);
        char v[100];
        if (s->type == T_HEADER) {
            _snprintf(b, sizeof(b), "[%s]", s->label); b[sizeof(b) - 1] = 0;
            text(b, x_set, y, x2, y + row_h, f_bold, C_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (i == hover_row) box(x_set - S(6), y, x2 + S(4), y + row_h - S(1), C_ROW, C_ROW, S(6));
        add_hit(x_set - S(6), y, x2 + S(4), y + row_h, A_ROW, i);
        text(s->label, x_set, y, x2 - S(190), y + row_h, f_text, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        shown(s, v, sizeof(v));
        if (s->type == T_INT || s->type == T_NUM || s->type == T_PCT) {
            const int bw = S(28), mx1 = x2 - S(170), px1 = x2 - bw;
            box(mx1, vy1, mx1 + bw, vy2, C_BTN, C_EDGE, S(6));
            text("-", mx1, vy1 - S(1), mx1 + bw, vy2, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            add_hit(mx1, vy1, mx1 + bw, vy2, A_MINUS, i);
            text(v, mx1 + bw, vy1, px1, vy2, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            box(px1, vy1, px1 + bw, vy2, C_BTN, C_EDGE, S(6));
            text("+", px1, vy1 - S(1), px1 + bw, vy2, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            add_hit(px1, vy1, px1 + bw, vy2, A_PLUS, i);
        } else if (s->type == T_TEXT) {
            text(v, x2 - S(190), vy1, x2, vy2, f_text, C_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            const int on = (s->type == T_BOOL && !strcmp(v, "On")) || i == capture;
            box(x2 - S(170), vy1, x2, vy2, on ? C_BTN_HOT : C_BTN, C_EDGE, S(6));
            text(i == capture ? "press a key..." : v, x2 - S(170), vy1, x2, vy2, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            add_hit(x2 - S(170), vy1, x2, vy2, A_NEXT, i);
        }
    }
    if (max_scroll) {                       /* more rows than fit: up and down */
        const int x1 = pw - S(44), x2 = pw - S(16), y1 = top + S(34), y2 = bottom - S(2), mid = (y1 + y2) / 2;
        box(x1, y1, x2, mid - S(2), scroll > 0 ? C_BTN : C_BAR, C_EDGE, S(6));
        text("^", x1, y1, x2, mid - S(2), f_bold, scroll > 0 ? C_TEXT : C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hit(x1, y1, x2, mid - S(2), A_UP, 0);
        box(x1, mid + S(2), x2, y2, scroll < max_scroll ? C_BTN : C_BAR, C_EDGE, S(6));
        text("v", x1, mid + S(2), x2, y2, f_bold, scroll < max_scroll ? C_TEXT : C_DIM, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_hit(x1, mid + S(2), x2, y2, A_DOWN, 0);
    }
    /* the bottom: what the setting under the pointer is for, and what was done last */
    box(S(14), bottom + S(8), pw - S(14), ph - S(14), C_BAR, C_BAR, S(8));
    if (hover_row >= 0 && hover_row < n_sets && sets[hover_row].help[0])
        text(sets[hover_row].help, S(26), bottom + S(14), pw - S(26), ph - S(38), f_small, C_TEXT, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    else if (hover_row < 0)
        text("Pick a module on the left. Click a value to change it; it is saved at once. Esc closes.", S(26), bottom + S(14), pw - S(26), ph - S(38),
             f_small, C_DIM, DT_LEFT | DT_WORDBREAK);
    if (status[0]) text(status, S(26), ph - S(38), pw - S(26), ph - S(16), f_small, C_ACCENT, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    GdiFlush();
    for (i = 0; i < pw * ph; i++) {         /* (the rows of the picture are pcap_w apart) */
        const int x = i % pw, yy = i / pw;
        const uint32_t c = pbits[(size_t)yy * pcap_w + x] & 0xffffffu;
        ppre[i] = c == MSG_KEY ? 0u : 0xfa000000u | (((c >> 16) & 0xff) * 250 / 255) << 16 | (((c >> 8) & 0xff) * 250 / 255) << 8 | ((c & 0xff) * 250 / 255);
    }
    dirty = 0;
}

static void make_arrow(void) {              /* the pointer: a white arrow with a dark edge (premultiplied ARGB, 24 x 24) */
    static const char *const shape[] = {
        "#", "##", "#.#", "#..#", "#...#", "#....#", "#.....#", "#......#", "#.......#", "#........#", "#.........#",
        "#......#####", "#...#..#", "#..##..#", "#.#  #..#", "##   #..#", "#     #..#", "      #..#", "       ##"};
    int x, y;
    memset(arrow, 0, sizeof(arrow));
    for (y = 0; y < (int)(sizeof(shape) / sizeof(shape[0])); y++)
        for (x = 0; shape[y][x]; x++)
            arrow[y * 24 + x] = shape[y][x] == '#' ? 0xff101820u : shape[y][x] == '.' ? 0xffffffffu : 0u;
    arrow_ok = 1;
}

/* ---- onto the screen: through wkDirect3D, or into WA's frame ---- */
static int (__cdecl *d3d_overlay)(const uint32_t *, int, int, int, int, int);
static int d3d_looked;
static void d3d_find(void) {
    const HMODULE m = GetModuleHandleA("wkDirect3D.dll");
    int (__cdecl *api)(void);
    d3d_looked = 1;
    if (!m || no_gpu) return;
    api = (int (__cdecl *)(void))(void *)GetProcAddress(m, "wkDirect3D_Api");
    if (!api || api() < 1) return;
    d3d_overlay = (int (__cdecl *)(const uint32_t *, int, int, int, int, int))(void *)GetProcAddress(m, "wkDirect3D_Overlay");
    if (d3d_overlay) say("wkDirect3D is loaded: the panel is drawn through it when it draws the frame");
}

/* the shown palette's nearest colour, by 5 bits of red, green and blue (worked out once a colour; started afresh when
 * WA's palette changes: looked at once a picture, not once a pixel) */
static uint16_t *near_lut;
static int near_ready(void) {
    static uint8_t pal_was[1024];
    const uint8_t *pal = (const uint8_t *)VA(PAL_SHOWN_VA);
    if (near_lut && !memcmp(pal_was, pal, sizeof(pal_was))) return 1;
    if (!near_lut) near_lut = (uint16_t *)malloc(32768 * sizeof(uint16_t));
    if (!near_lut) return 0;
    memset(near_lut, 0xff, 32768 * sizeof(uint16_t));
    memcpy(pal_was, pal, sizeof(pal_was));
    return 1;
}
static inline uint8_t nearest(uint32_t rgb) {
    const int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255, k = (r >> 3) << 10 | (g >> 3) << 5 | b >> 3;
    if (near_lut[k] == 0xffff) {
        const uint8_t *pal = (const uint8_t *)VA(PAL_SHOWN_VA);
        long best = 1L << 30;
        int i;
        for (i = 0; i < 256; i++) {
            const long dr = pal[4 * i] - r, dg = pal[4 * i + 1] - g, db = pal[4 * i + 2] - b, d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < best) { best = d; near_lut[k] = (uint16_t)i; }
        }
    }
    return (uint8_t)near_lut[k];
}

static void to_frame(const uint32_t *argb, int w, int h, int x0, int y0) {
    uint8_t *frame = (uint8_t *)(uintptr_t)U32(VA(FRAME_PTR_VA));
    const int fw = I32(VA(FRAME_W_VA)), fh = I32(VA(FRAME_H_VA));
    int x, y;
    if (!frame || fw < 64 || fh < 64 || !near_ready()) return;
    for (y = 0; y < h; y++) {
        uint8_t *row;
        if (y0 + y < 0 || y0 + y >= fh) continue;
        row = frame + (size_t)(y0 + y) * fw;
        for (x = 0; x < w; x++) {
            const uint32_t c = argb[(size_t)y * w + x];
            if ((c >> 24) < 128 || x0 + x < 0 || x0 + x >= fw) continue;
            row[x0 + x] = nearest(c);
        }
    }
}

static void show(void) {
    static int said = -1;
    int gpu = 0;
    if (!ppre) return;
    if (!d3d_looked) d3d_find();
    if (d3d_overlay && d3d_overlay(ppre, pw, ph, pw * 4, px0, py0)) gpu = 1;
    else to_frame(ppre, pw, ph, px0, py0);
    if (!arrow_ok) make_arrow();
    if (!gpu || !d3d_overlay(arrow, 24, 24, 24 * 4, ptr_x, ptr_y)) to_frame(arrow, 24, 24, ptr_x, ptr_y);
    if (gpu != said) { said = gpu; say("the panel is drawn %s", gpu ? "through wkDirect3D (true colour)" : "into WA's frame (WA's colours)"); }
}

/* ---- opening, closing, clicking ---- */
typedef void (__stdcall *open_fn)(uintptr_t);
static void limits(int whole) {             /* WA's menu pointer: free over the whole screen, and put at the panel's middle */
    const uintptr_t p = runtime_now ? U32(runtime_now + RT_PANEL) : 0;
    if (!p) return;
    I32(p + P_LIMITS) = 0; I32(p + P_LIMITS + 4) = 0; I32(p + P_LIMITS + 8) = sw - 1; I32(p + P_LIMITS + 12) = sh - 1;
    if (whole) { I32(p + P_CURSOR) = sw / 2; I32(p + P_CURSOR + 4) = sh / 2; }
}

static void panel_show(void) {
    if (!runtime_now || U32(runtime_now + RT_MENU_OPEN) != 1) return;
    sw = I32(VA(FRAME_W_VA)); sh = I32(VA(FRAME_H_VA));
    if (sw < 320 || sh < 200) return;
    list_modules();
    load_settings();
    scroll = 0; hover = hover_row = -1; status[0] = 0; capture = -1;
    panel_open = 1; hide_menu = 1; dirty = 1;
    limits(1);
    say("the panel opened: %d modules, %s chosen (%d settings)", n_mods, sel >= 0 ? mods[sel].name : "none", n_sets);
}

static void panel_hide(int back) {          /* back: WA's own menu again (laid out anew: its pointer's limits too) */
    if (!panel_open) return;
    panel_open = 0; hide_menu = 0;
    if (back && runtime_now && U32(runtime_now + RT_MENU_OPEN) == 1) ((open_fn)VA(OPEN_MENU_VA))(runtime_now);
    say("the panel closed (%s)", back ? "back to WA's menu" : "WA's menu closed");
}

static void act(int k) {
    if (capture >= 0) { capture = -1; status[0] = 0; dirty = 1; }   /* (a click instead of a key: the key stays as it is) */
    if (k < 0 || k >= n_hits) return;
    switch (hits[k].act) {
    case A_BACK: panel_hide(1); return;
    case A_MODULE: if (hits[k].i != sel) { sel = hits[k].i; load_settings(); scroll = 0; status[0] = 0; hover_row = -1; } break;
    case A_NEXT:
        if (hits[k].i >= 0 && hits[k].i < n_sets && sets[hits[k].i].type == T_KEY) {
            capture = hits[k].i;
            _snprintf(status, sizeof(status), "Press the key for \"%s\" (with Ctrl, Alt or Shift if you like). Delete: none. A click: leave it.", sets[capture].label);
            status[sizeof(status) - 1] = 0;
            break;
        }
        change(hits[k].i, 1);
        break;
    case A_PLUS: change(hits[k].i, 1); break;
    case A_MINUS: change(hits[k].i, -1); break;
    case A_UP: scroll -= rows_fit > 2 ? rows_fit - 1 : 1; break;
    case A_DOWN: scroll += rows_fit > 2 ? rows_fit - 1 : 1; break;
    default: return;
    }
    dirty = 1;
}

/* each frame while the panel is open (before WA draws): where the pointer is, what it is over */
static void panel_frame(void) {
    const uintptr_t p = runtime_now ? U32(runtime_now + RT_PANEL) : 0;
    int k, row;
    if (!p || U32(runtime_now + RT_MENU_OPEN) != 1) { panel_hide(0); return; }   /* (Esc: WA closed its menu) */
    if (I32(VA(FRAME_W_VA)) != sw || I32(VA(FRAME_H_VA)) != sh) { sw = I32(VA(FRAME_W_VA)); sh = I32(VA(FRAME_H_VA)); dirty = 1; }
    limits(0);
    ptr_x = I32(p + P_CURSOR); ptr_y = I32(p + P_CURSOR + 4);
    if (dirty) paint();
    k = hit_at(ptr_x - px0, ptr_y - py0);
    row = -1;
    if (k >= 0 && (hits[k].act == A_ROW || hits[k].act == A_NEXT || hits[k].act == A_MINUS || hits[k].act == A_PLUS)) row = hits[k].i;
    if (k != hover || row != hover_row) { hover = k; hover_row = row; paint(); }
}

/* ---- the hooks ---- */
uint32_t __cdecl mods_call_append(uint32_t panel, uint32_t x, uint32_t id, const char *label, uint32_t y);
void __cdecl mods_after_first(uint32_t *r);
void __cdecl mods_runtime_gone(uintptr_t rt);
void mods_thunk_after_first(void);
void mods_thunk_gone(void);
void mods_thunk_draw(void);
void *mods_orig_after_first, *mods_append_fn, *mods_orig_gone, *mods_orig_draw;
int mods_hide;                              /* (the thunk's copy of hide_menu) */
__asm__(
    ".text\n"
    /* MenuPanel::AppendItem(ESI panel, EAX x; id, label, y, centred 1, 0, 0) from C */
    ".globl _mods_call_append\n"
    "_mods_call_append:\n"
    "    pushl %ebx\n    pushl %esi\n    pushl %edi\n    pushl %ebp\n"
    "    movl 20(%esp), %esi\n"
    "    movl 24(%esp), %eax\n"
    "    pushl $0\n    pushl $0\n    pushl $1\n"
    "    pushl 48(%esp)\n"                  /* y */
    "    pushl 48(%esp)\n"                  /* label */
    "    pushl 48(%esp)\n"                  /* id */
    "    call *_mods_append_fn\n"
    "    popl %ebp\n    popl %edi\n    popl %esi\n    popl %ebx\n"
    "    ret\n"
    /* 005356D3, WA's first item appended: the registers to C (EBP may change), then WA's own code on */
    ".globl _mods_thunk_after_first\n"
    "_mods_thunk_after_first:\n"
    "    pushal\n    pushl %esp\n    call _mods_after_first\n    addl $4, %esp\n    popal\n"
    "    jmp *_mods_orig_after_first\n"
    /* the runtime's destructor (EAX the runtime): told first */
    ".globl _mods_thunk_gone\n"
    "_mods_thunk_gone:\n"
    "    pushal\n    pushl %eax\n    call _mods_runtime_gone\n    addl $4, %esp\n    popal\n"
    "    jmp *_mods_orig_gone\n"
    /* WA's Esc menu drawing (EAX the runtime; plain ret): not while the panel is open */
    ".globl _mods_thunk_draw\n"
    "_mods_thunk_draw:\n"
    "    cmpl $0, _mods_hide\n    jne 1f\n    jmp *_mods_orig_draw\n1:  ret\n");

static char mods_label[] = "Mods";
void __cdecl mods_after_first(uint32_t *r) {
    /* pushal: edi esi ebp esp ebx edx ecx eax */
    const uintptr_t runtime = r[4], esp = r[3];
    const uint32_t panel = U32(runtime + RT_PANEL), row = U32(esp + 0x20), x = U32(esp + 0x2c), y = row + r[2] + 1;
    runtime_now = runtime;
    if (panel && mods_call_append(panel, x, MODS_ID, mods_label, y)) r[2] = y;   /* (EBP: WA's own items follow this row) */
}

void __cdecl mods_runtime_gone(uintptr_t rt) {
    if (!rt || rt != runtime_now) return;
    panel_open = 0; hide_menu = 0; mods_hide = 0;
    runtime_now = 0;
}

static int (__stdcall *orig_activate)(void *, uint32_t *);
static int __stdcall hook_activate(void *panel, uint32_t *out) {
    int r;
    if (panel_open && runtime_now && (uintptr_t)panel == U32(runtime_now + RT_PANEL)) {   /* a click while the panel is open: its own */
        ptr_x = I32((uintptr_t)panel + P_CURSOR); ptr_y = I32((uintptr_t)panel + P_CURSOR + 4);
        act(hit_at(ptr_x - px0, ptr_y - py0));
        mods_hide = hide_menu;
        return 0;
    }
    r = orig_activate(panel, out);
    if (r == 1 && out && *out == MODS_ID) { panel_show(); mods_hide = hide_menu; return 0; }
    return r;
}

/* the test script: commands at times after the first game frame */
static char script[600];
static DWORD script_t0;
static const char *script_at;
static void script_run(void) {
    if (!script[0] || !runtime_now) return;
    if (!script_t0) { script_t0 = GetTickCount(); script_at = script; }
    while (script_at && *script_at) {
        char cmd[96], *eq;
        const char *end = strchr(script_at, ';'), *colon = strchr(script_at, ':');
        const size_t n = end ? (size_t)(end - script_at) : strlen(script_at);
        if (!colon || (end && colon > end)) { script_at = NULL; break; }
        if (GetTickCount() - script_t0 < (DWORD)atoi(script_at)) return;
        copy(cmd, sizeof(cmd) < n - (size_t)(colon - script_at) ? sizeof(cmd) : n - (size_t)(colon - script_at), colon + 1);
        script_at = end ? end + 1 : NULL;
        eq = strchr(cmd, '=');
        if (eq) *eq++ = 0;
        say("test: %s%s%s", cmd, eq ? " " : "", eq ? eq : "");
        if (!strcmp(cmd, "esc")) ((open_fn)VA(OPEN_MENU_VA))(runtime_now);
        else if (!strcmp(cmd, "mods")) panel_show();
        else if (!strcmp(cmd, "back")) panel_hide(1);
        else if (!strcmp(cmd, "close")) { if (runtime_now) U32(runtime_now + RT_MENU_OPEN) = 0; }
        else if (!strcmp(cmd, "select") && eq) {
            int i;
            for (i = 0; i < n_mods; i++) if (!lstrcmpiA(mods[i].name, eq)) { sel = i; load_settings(); scroll = 0; dirty = 1; }
        } else if (!strcmp(cmd, "set") && eq) {
            char *how = strchr(eq, ',');
            int i;
            if (how) *how++ = 0;
            for (i = 0; i < n_sets; i++) if (!lstrcmpiA(sets[i].key, eq)) { change(i, how && *how == '-' ? -1 : 1); dirty = 1; }
        } else if (!strcmp(cmd, "capture") && eq) {
            int i;
            for (i = 0; i < n_sets; i++) if (!lstrcmpiA(sets[i].key, eq) && sets[i].type == T_KEY) { capture = i; dirty = 1; }
        } else if (!strcmp(cmd, "scroll") && eq) { scroll += atoi(eq); dirty = 1; }
        else if (!strcmp(cmd, "point") && eq) {
            const uintptr_t p = U32(runtime_now + RT_PANEL);
            int x = 0, y = 0;
            if (p && sscanf(eq, "%d,%d", &x, &y) == 2) { I32(p + P_CURSOR) = x; I32(p + P_CURSOR + 4) = y; }
        } else if (!strcmp(cmd, "click")) {
            const uintptr_t p = U32(runtime_now + RT_PANEL);
            uint32_t id = 0;
            if (p) say("test: the click gave %d (id %u)", hook_activate((void *)p, &id), id);
        } else if (!strcmp(cmd, "say")) {
            const uintptr_t p = U32(runtime_now + RT_PANEL);
            say("test: WA's menu %s, the panel %s, pointer %d,%d within %d,%d..%d,%d, over %d (row %d), %d modules, chosen %s, %d settings, scroll %d",
                U32(runtime_now + RT_MENU_OPEN) == 1 ? "open" : "closed", panel_open ? "open" : "closed", p ? I32(p + P_CURSOR) : -1, p ? I32(p + P_CURSOR + 4) : -1,
                p ? I32(p + P_LIMITS) : -1, p ? I32(p + P_LIMITS + 4) : -1, p ? I32(p + P_LIMITS + 8) : -1, p ? I32(p + P_LIMITS + 12) : -1,
                hover >= 0 ? hits[hover].act : -1, hover_row, n_mods, sel >= 0 ? mods[sel].name : "none", n_sets, scroll);
        }
        mods_hide = hide_menu;
    }
}

static void (__fastcall *orig_render)(void *);
static void __fastcall hook_render(void *runtime) {
    if (runtime) runtime_now = (uintptr_t)runtime;
    script_run();
    if (panel_open) panel_frame();
    mods_hide = hide_menu;
    orig_render(runtime);
    if (panel_open) show();
}

static LRESULT CALLBACK on_message(int code, WPARAM wp, LPARAM lp) {
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m && panel_open && m->message == 0x020a /* WM_MOUSEWHEEL */) {
        scroll -= (short)HIWORD(m->wParam) / 120 * 3;
        dirty = 1;
        m->message = WM_NULL;
    } else if (code == HC_ACTION && wp == PM_REMOVE && m && panel_open && capture >= 0 && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)) {
        const int vk = (int)m->wParam;      /* the key for the setting that waits for one (Esc closes the menu, as ever) */
        if (vk != VK_CONTROL && vk != VK_SHIFT && vk != VK_MENU && vk != VK_LCONTROL && vk != VK_RCONTROL && vk != VK_LSHIFT && vk != VK_RSHIFT &&
            vk != VK_LMENU && vk != VK_RMENU && vk != VK_LWIN && vk != VK_RWIN && vk != VK_ESCAPE) {
            keybind k;
            char v[48];
            k.vk = vk; k.mods = key_mods_now();
            if (vk == VK_DELETE && !k.mods) k.vk = 0;
            key_text(k, v, sizeof(v));
            store(capture, v);
            capture = -1;
            dirty = 1;
            m->message = WM_NULL;
        }
    }
    return CallNextHookEx(msg_hook, code, wp, lp);
}

/* ---- start-up ---- */
typedef BOOL (WINAPI *handle_ex_fn)(DWORD, LPCSTR, HMODULE *);
static handle_ex_fn handle_ex(void) { return (handle_ex_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetModuleHandleExA"); }

/* a hook point: WA's own bytes, or another module's jump there (chained onto: that module kept loaded) */
static int point_ok(uint32_t va, const unsigned char *bytes, unsigned n, const char *what) {
    const uint8_t *at = (const uint8_t *)VA(va);
    if (!memcmp(at, bytes, n)) return 1;
    if (*at == 0xe9) {
        const handle_ex_fn ex = handle_ex();
        HMODULE m;
        char name[MAX_PATH] = "?";
        if (ex && ex(0x1 | 0x4, (LPCSTR)(at + 5 + *(const int32_t *)(at + 1)), &m)) GetModuleFileNameA(m, name, sizeof(name));
        say("%s is hooked by %s already: chained onto it (it stays loaded)", what, file_name(name));
        return 1;
    }
    say("unexpected code at %08X (%s)", va, what);
    return 0;
}

static void install(void) {
    static const unsigned char render_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x83, 0xec, 0x20};
    static const unsigned char before_first[] = {0x6a, 0x03, 0x89, 0x44, 0x24, 0x44, 0xe8, 0x1d, 0xb2, 0x00, 0x00};   /* 005356C8: ..., call AppendItem */
    static const unsigned char after_first[] = {0x8b, 0x83, 0x88, 0x04, 0x00, 0x00};
    static const unsigned char activate_bytes[] = {0x53, 0x8b, 0x5c, 0x24, 0x08, 0x56, 0x8b, 0x73, 0x14};
    static const unsigned char open_bytes[] = {0x81, 0xec, 0x08, 0x04, 0x00, 0x00, 0x53};
    static const unsigned char append_bytes[] = {0x83, 0xbe, 0xb0, 0x03, 0x00, 0x00, 0x10, 0x53};
    static const unsigned char draw_bytes[] = {0x83, 0xec, 0x0c, 0x53, 0x55, 0x56, 0x8b, 0xf0, 0x8b, 0x86, 0x88, 0x04, 0x00, 0x00};
    static const unsigned char gone_bytes[] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x51, 0x53, 0x56, 0x8b, 0xf0, 0x83, 0xbe, 0x38, 0x04, 0x00, 0x00, 0x00};
    static const unsigned char limit_bytes[] = {0x8b, 0x48, 0x1c, 0x3b, 0xd1, 0x89, 0x50, 0x10, 0x89, 0x70, 0x14};   /* 005407D0: the pointer's x and
                                                                                   y stored, and kept inside the limits at +0x1C .. */
    char exe[MAX_PATH], why[256] = "WA.exe's path unknown", ini[MAX_PATH], *slash;
    const DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    const handle_ex_fn ex = handle_ex();
    const char *line = GetCommandLineA();
    HMODULE me;
    MH_STATUS s;
    open_log();
    if (!n || n >= sizeof(exe) || !wa_supported_image(exe, why, sizeof(why))) { say("wkModMenu " VERSION ": not the WA 3.8.1 this module knows (%s): not installed", why); return; }
    if (line && (strstr(line, "/getvideo") || strstr(line, "/getlog") || strstr(line, "/getmap") || strstr(line, "/getscheme") || strstr(line, "/repair"))) return;
    base = (uintptr_t)GetModuleHandleA(NULL);
    copy(wa_dir, sizeof(wa_dir), exe);
    slash = strrchr(wa_dir, '\\');
    if (!slash) return;
    slash[1] = 0;
    {
        char name[64];
        sprintf(name, "wkModMenu-loaded-%lu", (unsigned long)GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, name) && GetLastError() == ERROR_ALREADY_EXISTS) { say("wkModMenu " VERSION ": another copy is loaded already: this copy does nothing"); return; }
    }
    _snprintf(ini, sizeof(ini), "%swkModMenu.ini", wa_dir);
    ini[sizeof(ini) - 1] = 0;
    if (!GetPrivateProfileIntA("wkModMenu", "Enabled", 1, ini)) { say("wkModMenu " VERSION ": switched off in wkModMenu.ini (Enabled=0): not installed"); return; }
    no_gpu = GetEnvironmentVariableA("WKMODMENU_NO_GPU", why, sizeof(why)) > 0;
    if (GetEnvironmentVariableA("WKMODMENU_SCRIPT", script, sizeof(script)) >= sizeof(script)) script[0] = 0;
    if (!point_ok(GAME_RENDER_VA, render_bytes, sizeof(render_bytes), "GameRender") ||
        memcmp((void *)VA(0x5356c8), before_first, sizeof(before_first)) || !point_ok(AFTER_FIRST_VA, after_first, sizeof(after_first), "WA's Esc menu (its first item)") ||
        !point_ok(ACTIVATE_VA, activate_bytes, sizeof(activate_bytes), "WA's Esc menu (a click)") ||
        memcmp((void *)VA(OPEN_MENU_VA), open_bytes, sizeof(open_bytes)) || memcmp((void *)VA(APPEND_VA), append_bytes, sizeof(append_bytes)) ||
        memcmp((void *)VA(0x5407d0), limit_bytes, sizeof(limit_bytes)) ||
        !point_ok(MENU_DRAW_VA, draw_bytes, sizeof(draw_bytes), "WA's Esc menu (its drawing)") ||
        !point_ok(RUNTIME_GONE_VA, gone_bytes, sizeof(gone_bytes), "the game's end")) {
        say("wkModMenu " VERSION ": WA's Esc menu is not as in WA 3.8.1: not installed");
        return;
    }
    if (!ex || !ex(0x1 | 0x4, (LPCSTR)(void *)&install, &me)) { say("wkModMenu " VERSION ": could not pin the module in memory (error %lu): not installed", GetLastError()); return; }
    mods_append_fn = (void *)VA(APPEND_VA);
    s = MH_Initialize();
    if (s == MH_OK) s = MH_CreateHook((void *)VA(GAME_RENDER_VA), (void *)hook_render, (void **)&orig_render);
    if (s == MH_OK) s = MH_CreateHook((void *)VA(AFTER_FIRST_VA), (void *)mods_thunk_after_first, &mods_orig_after_first);
    if (s == MH_OK) s = MH_CreateHook((void *)VA(ACTIVATE_VA), (void *)hook_activate, (void **)&orig_activate);
    if (s == MH_OK) s = MH_CreateHook((void *)VA(MENU_DRAW_VA), (void *)mods_thunk_draw, &mods_orig_draw);
    if (s == MH_OK) s = MH_CreateHook((void *)VA(RUNTIME_GONE_VA), (void *)mods_thunk_gone, &mods_orig_gone);
    if (s == MH_OK) s = MH_EnableHook(MH_ALL_HOOKS);
    installed = s == MH_OK;
    if (installed) msg_hook = SetWindowsHookExA(WH_GETMESSAGE, on_message, self, GetCurrentThreadId());
    say("wkModMenu " VERSION " %s (%s): WA's Esc menu has a \"Mods\" row; it opens the modules' settings%s", installed ? "ready" : "failed", why,
        script[0] ? "; a test script runs" : "");
}

BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { self = dll; DisableThreadLibraryCalls(dll); install(); }
    return TRUE;
}
