// agentbar — always-on-top dock for terminal AI agent sessions (omp / claude / codex).
// Pure Win32 + GDI, no dependencies beyond system DLLs. Build: build.cmd
//
//   click session       focus its terminal window (and tab, if the window has several)
//   ctrl+click          toggle selection (on a group header: its whole group)
//   click group header  focus the group's session, or start one in its first folder
//   +                   new session: quick idea (next free ideaN), ephemeral (%TEMP%), chosen folder;
//                       quick idea and ephemeral ask for an optional first prompt (also names the folder)
//   Open All / Open N   tile all (or the selected) session windows in a grid
//   right-click         session: close / open folder / put in group · group: new session, tile group
//                       bar: dock left / right / top, start with Windows, quit
//   drop a folder       start a session there
//   hotkeys             see agentbar.ini
//   agentbar.exe group <name> <folder>    put a folder in a group (moves it out of any other)
//   agentbar.exe ungroup <folder>         remove a folder from all groups
//
// Files next to the exe: agentbar.ini (yours; reloaded within 2 s of a change, created with
// defaults if missing) and agentbar.state.ini (the bar's: last edge and agent).
//
// Sessions are discovered, not registered: every running agent process whose console is
// hosted by a visible terminal window is a session. Its console title and cwd are read
// directly, so nothing is persisted and nothing can drift.
#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <psapi.h>
#include <winternl.h>
#include <uiautomationclient.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── fixed look & limits ─────────────────────────────────────────────────────────────── */
#define SCAN_MS   2000                   // process scan period (OS may coalesce it)
#define HOVER_MS  150                    // pointer rest time before the bar expands
#define THICK_H   40                     // top bar: pill height (sizes in px at 100% scaling)
#define EXPAND_H  112                    //          height while hovered
#define THICK_V   76                     // left/right bar: pill width
#define EXPAND_V  320                    //                 width while hovered
#define MARGIN    4                      // gap between the screen edge and the pill
#define PAD       6                      // padding at both ends of the pill
#define C_BG      RGB(28, 28, 30)        // Apple-style dark palette
#define C_HOT     RGB(44, 44, 46)
#define C_SEP     RGB(58, 58, 60)
#define C_SEL     RGB(18, 54, 94)
#define C_ATTN    RGB(255, 214, 10)              // glow strip: finished while you were away
#define C_RUN     RGB(48, 209, 88)               // glow strip: working
#define C_TEXT    RGB(245, 245, 247)
#define C_MUTED   RGB(152, 152, 157)
#define C_ACCENT  RGB(10, 132, 255)
#define C_WORK    RGB(48, 209, 88)
#define C_WAIT    RGB(255, 214, 10)
#define C_ERR     RGB(255, 69, 58)
#define C_CLAUDE  RGB(217, 119, 87)               // usage labels: which provider at a glance
#define C_CODEX   RGB(116, 170, 255)
#define MAX_LIMITS 4
#define MAX_AGENTS 8
#define MAX_GROUPS 16
#define MAX_GDIRS  8
#define MAX_S      32
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#endif

static const COLORREF PALETTE[MAX_AGENTS] = {
    RGB(167, 139, 250), RGB(217, 119, 87), RGB(16, 163, 127), RGB(96, 165, 250),
    RGB(244, 114, 182), RGB(250, 204, 21), RGB(45, 212, 191), RGB(248, 113, 113) };

typedef struct { wchar_t name[32], exe[40], cmd[160]; COLORREF color; } Agent;
typedef struct { UINT mods, vk; } Hotkey;
typedef struct { wchar_t name[48]; int ndirs; wchar_t dirs[MAX_GDIRS][MAX_PATH]; Hotkey hk; } Group;

typedef struct {
    DWORD pid;
    int agent, group;      // group: index or -1
    HWND con, term;        // console window (per tab) and the terminal window hosting it
    ULONGLONG born;        // process creation time: stable ordering
    BOOL sel, working, attn;
    wchar_t cwd[MAX_PATH];
    wchar_t title[160];    // console title: per tab, includes the agent's own status
    wchar_t model[40];     // omp: current model, from its session file
    wchar_t file[MAX_PATH];// omp: that session file, and the title it carries
    wchar_t ftitle[128];
    LONGLONG foff;         // forward cursor: bytes of `file` scanned for model changes
    LONGLONG back;         // backward cursor for the first visit (-1 = not started, 0 = done)
    UINT resolved;         // scan number of the last file lookup
    ULONGLONG since;       // tick when the current working stretch began (0 = idle)
    ULONGLONG mins;        // whole minutes of that stretch, as displayed
    wchar_t label[64];     // your name for it (agentbar.state.ini [names]); empty = folder name
} Session;

enum { IT_SESSION, IT_GROUP, IT_PLUS, IT_OPEN, IT_USAGE };
typedef struct { int kind, idx; RECT r; } Item;

enum { WM_APPBAR_CB = WM_APP + 1, T_SCAN = 1, T_BURST,
       HK_IDEA = 1, HK_EPHEMERAL, HK_TILE, HK_FOCUS = 10, HK_GROUP = 30,
       CMD_IDEA = 100, CMD_EPHEMERAL, CMD_FOLDER, CMD_LEFT, CMD_RIGHT, CMD_TOP, CMD_QUIT,
       CMD_CLOSE, CMD_EXPLORE, CMD_UNGROUP, CMD_AGENT = 200, CMD_GROUP = 300, CMD_GDIR = 400,
       CMD_GTILE = 500, CMD_STARTUP, CMD_RENAME };

static void load_label(Session *s);

/* ── state ───────────────────────────────────────────────────────────────────────────── */
static Agent   g_agents[MAX_AGENTS];
static int     g_nagents, g_agent;
static Group   g_groups[MAX_GROUPS];
static int     g_ngroups;
static UINT    g_edge = ABE_TOP;
static wchar_t g_ideas_dir[MAX_PATH] = L"%USERPROFILE%\\new_ideas";
static wchar_t g_prefix[32] = L"idea";
static wchar_t g_terminal[256] = L"wt.exe -w new new-tab -d \"%d\" %c";
static Hotkey  g_hk_idea, g_hk_eph, g_hk_tile, g_hk_focus;
static wchar_t g_ini[MAX_PATH], g_state[MAX_PATH];
static FILETIME g_ini_time;

static Session g_s[MAX_S];
static int     g_n;
static Item    g_items[MAX_S + MAX_GROUPS + 3];
static int     g_nitems;
static HWND    g_bar;
static UINT    g_dpi = 96;
static RECT    g_rc;                     // reserved (collapsed) appbar rect
static BOOL    g_expanded, g_tracking, g_menu;
static int     g_hot = -1, g_burst;
static HFONT   g_font, g_small, g_big;

typedef struct { wchar_t label[24], id[48]; double used; LONGLONG resets, at; BOOL have; } Limit;
static Limit   g_lim[MAX_LIMITS];
static int     g_nlim;
static BOOL    g_lim_stale;
static FILETIME g_db_time;
static wchar_t g_omp_dir[MAX_PATH];             // %USERPROFILE%\.omp\agent
static UINT    g_scans = 1;
static UINT    g_taskbar_created;               // broadcast when Explorer (re)starts
static int     g_sess_len;                      // session tile length when squeezed to fit; 0 = natural

static int S(int px) { return MulDiv(px, (int)g_dpi, 96); }

static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *b = p;
    for (; *p; p++)
        if (*p == L'\\' || *p == L'/') b = p + 1;
    return b;
}

static BOOL path_eq(const wchar_t *a, const wchar_t *b) { return _wcsicmp(a, b) == 0; }

// TRUE if `path` is `dir` or inside it.
static BOOL path_under(const wchar_t *path, const wchar_t *dir) {
    size_t n = wcslen(dir);
    while (n && dir[n - 1] == L'\\') n--;
    return _wcsnicmp(path, dir, n) == 0 && (path[n] == 0 || path[n] == L'\\');
}

/* ── configuration ───────────────────────────────────────────────────────────────────── */

static const wchar_t DEFAULT_INI[] =
L"; agentbar configuration. Edit by hand or by script; the bar reloads it within 2 seconds.\r\n"
L"; Keep comments on their own lines. Save as UTF-16 (as created) or plain ASCII.\r\n"
L"\r\n"
L"[bar]\r\n"
L"; left | right | top  (the last choice made from the bar's menu wins; see agentbar.state.ini)\r\n"
L"edge=top\r\n"
L"ideas_dir=%USERPROFILE%\\new_ideas\r\n"
L"idea_prefix=idea\r\n"
L"; %d = folder, %c = agent command\r\n"
L"terminal=wt.exe -w new new-tab -d \"%d\" %c\r\n"
L"\r\n"
L"[agents]\r\n"
L"; name=command to run. The process detected on the bar is <name>.exe\r\n"
L"omp=omp\r\n"
L"claude=claude\r\n"
L"codex=cmd /c codex\r\n"
L"\r\n"
L"[hotkeys]\r\n"
L"; Win, Alt, Ctrl, Shift + a letter, digit, F1-F12 or Space. Empty = off.\r\n"
L"new_idea=Win+Alt+N\r\n"
L"ephemeral=Win+Alt+E\r\n"
L"tile=Win+Alt+T\r\n"
L"; focus takes the modifiers only; digits 1-9 pick the Nth session on the bar\r\n"
L"focus=Win+Alt\r\n"
L"\r\n"
L"[usage]\r\n"
L"; label=limit id, read from omp's own usage records (~/.omp/agent/agent.db, kept fresh by omp).\r\n"
L"; Other ids omp records: anthropic:7d:fable. anthropic:* labels are drawn orange, openai-* blue.\r\n"
L"Claude 7d=anthropic:7d\r\n"
L"Codex 7d=openai-codex:primary\r\n"
L"Claude 5h=anthropic:5h\r\n"
L"\r\n"
L"; Groups: one [group:<name>] section per group, every key is a folder (the key is a label),\r\n"
L"; except an optional  hotkey=  which focuses the group's session or starts one.\r\n"
L"; Sessions whose folder is inside one of them are listed under the group; clicking the\r\n"
L"; group header does the same as its hotkey. A folder belongs to one group. Add here,\r\n"
L"; from the bar (right-click a session > Group), or:  agentbar.exe group <name> <folder>\r\n"
L"; and  agentbar.exe ungroup <folder>\r\n"
L";[group:omp]\r\n"
L";hotkey=Win+Alt+O\r\n"
L";main=C:\\Users\\me\\omp\r\n";

static void write_default_ini(void) {
    if (GetFileAttributesW(g_ini) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, "\xFF\xFE", 2, &w, NULL);           // UTF-16LE BOM: the profile API reads Unicode then
    WriteFile(h, DEFAULT_INI, (DWORD)(wcslen(DEFAULT_INI) * 2), &w, NULL);
    CloseHandle(h);
}

static UINT parse_edge(const wchar_t *s) {
    return !_wcsicmp(s, L"left") ? ABE_LEFT : !_wcsicmp(s, L"top") ? ABE_TOP : ABE_RIGHT;
}

static const wchar_t *edge_name(UINT e) { return e == ABE_LEFT ? L"left" : e == ABE_TOP ? L"top" : L"right"; }

static Hotkey parse_hotkey(const wchar_t *s) {
    Hotkey h = { 0, 0 };
    wchar_t buf[64], *tok, *ctx;
    wcsncpy(buf, s, 63);
    buf[63] = 0;
    for (tok = wcstok(buf, L"+ ", &ctx); tok; tok = wcstok(NULL, L"+ ", &ctx)) {
        if (!_wcsicmp(tok, L"win")) h.mods |= MOD_WIN;
        else if (!_wcsicmp(tok, L"alt")) h.mods |= MOD_ALT;
        else if (!_wcsicmp(tok, L"ctrl")) h.mods |= MOD_CONTROL;
        else if (!_wcsicmp(tok, L"shift")) h.mods |= MOD_SHIFT;
        else if (!_wcsicmp(tok, L"space")) h.vk = VK_SPACE;
        else if ((tok[0] == L'F' || tok[0] == L'f') && iswdigit(tok[1])) h.vk = VK_F1 + _wtoi(tok + 1) - 1;
        else if (!tok[1]) h.vk = towupper(tok[0]);
    }
    return h;
}

static void register_hotkeys(void) {
    for (int i = 1; i < HK_GROUP + MAX_GROUPS; i++) UnregisterHotKey(g_bar, i);
    if (g_hk_idea.vk) RegisterHotKey(g_bar, HK_IDEA, g_hk_idea.mods | MOD_NOREPEAT, g_hk_idea.vk);
    if (g_hk_eph.vk) RegisterHotKey(g_bar, HK_EPHEMERAL, g_hk_eph.mods | MOD_NOREPEAT, g_hk_eph.vk);
    if (g_hk_tile.vk) RegisterHotKey(g_bar, HK_TILE, g_hk_tile.mods | MOD_NOREPEAT, g_hk_tile.vk);
    if (g_hk_focus.mods)
        for (int n = 1; n <= 9; n++) RegisterHotKey(g_bar, HK_FOCUS + n, g_hk_focus.mods | MOD_NOREPEAT, L'0' + n);
    for (int g = 0; g < g_ngroups; g++)
        if (g_groups[g].hk.vk) RegisterHotKey(g_bar, HK_GROUP + g, g_groups[g].hk.mods | MOD_NOREPEAT, g_groups[g].hk.vk);
}

static int group_of(const wchar_t *cwd) {
    int best = -1;
    size_t bestlen = 0;
    for (int g = 0; g < g_ngroups; g++)
        for (int d = 0; d < g_groups[g].ndirs; d++) {
            size_t l = wcslen(g_groups[g].dirs[d]);
            if (l > bestlen && path_under(cwd, g_groups[g].dirs[d])) { best = g; bestlen = l; }
        }
    return best;
}

static void build_items(void);

static void load_config(void) {
    static wchar_t buf[8192];
    wchar_t v[256];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa)) g_ini_time = fa.ftLastWriteTime;

    GetPrivateProfileStringW(L"bar", L"edge", L"top", v, 256, g_ini);
    g_edge = parse_edge(v);
    GetPrivateProfileStringW(L"state", L"edge", L"", v, 256, g_state);
    if (*v) g_edge = parse_edge(v);
    GetPrivateProfileStringW(L"bar", L"ideas_dir", g_ideas_dir, g_ideas_dir, MAX_PATH, g_ini);
    GetPrivateProfileStringW(L"bar", L"idea_prefix", g_prefix, g_prefix, 32, g_ini);
    GetPrivateProfileStringW(L"bar", L"terminal", g_terminal, g_terminal, 256, g_ini);

    g_nagents = 0;
    GetPrivateProfileSectionW(L"agents", buf, 8192, g_ini);
    for (wchar_t *p = buf; *p && g_nagents < MAX_AGENTS; p += wcslen(p) + 1) {
        wchar_t *eq = wcschr(p, L'=');
        if (!eq) continue;
        *eq = 0;
        Agent *a = &g_agents[g_nagents];
        wcsncpy(a->name, p, 31);
        a->name[31] = 0;
        swprintf(a->exe, 40, L"%ls.exe", a->name);
        wcsncpy(a->cmd, eq + 1, 159);
        a->cmd[159] = 0;
        a->color = PALETTE[g_nagents++];
    }
    if (!g_nagents) {
        g_agents[0] = (Agent){ L"omp", L"omp.exe", L"omp", PALETTE[0] };
        g_nagents = 1;
    }
    GetPrivateProfileStringW(L"state", L"agent", L"", v, 256, g_state);
    g_agent = 0;
    for (int a = 0; a < g_nagents; a++)
        if (!_wcsicmp(v, g_agents[a].name)) g_agent = a;

    g_ngroups = 0;
    static wchar_t names[4096];
    GetPrivateProfileSectionNamesW(names, 4096, g_ini);
    for (wchar_t *n = names; *n && g_ngroups < MAX_GROUPS; n += wcslen(n) + 1) {
        if (_wcsnicmp(n, L"group:", 6)) continue;
        Group *g = &g_groups[g_ngroups];
        wcsncpy(g->name, n + 6, 47);
        g->name[47] = 0;
        g->ndirs = 0;
        g->hk = (Hotkey){ 0, 0 };
        GetPrivateProfileSectionW(n, buf, 8192, g_ini);
        for (wchar_t *p = buf; *p && g->ndirs < MAX_GDIRS; p += wcslen(p) + 1) {
            wchar_t *eq = wcschr(p, L'=');
            if (!eq || !eq[1]) continue;
            if (eq - p == 6 && !_wcsnicmp(p, L"hotkey", 6)) { g->hk = parse_hotkey(eq + 1); continue; }
            ExpandEnvironmentStringsW(eq + 1, g->dirs[g->ndirs], MAX_PATH);
            size_t l = wcslen(g->dirs[g->ndirs]);
            if (l > 3 && g->dirs[g->ndirs][l - 1] == L'\\') g->dirs[g->ndirs][l - 1] = 0;
            g->ndirs++;
        }
        if (g->ndirs) g_ngroups++;                   // an empty section is not a group
    }

    GetPrivateProfileStringW(L"hotkeys", L"new_idea", L"", v, 256, g_ini);
    g_hk_idea = parse_hotkey(v);
    GetPrivateProfileStringW(L"hotkeys", L"ephemeral", L"", v, 256, g_ini);
    g_hk_eph = parse_hotkey(v);
    GetPrivateProfileStringW(L"hotkeys", L"tile", L"", v, 256, g_ini);
    g_hk_tile = parse_hotkey(v);
    GetPrivateProfileStringW(L"hotkeys", L"focus", L"", v, 256, g_ini);
    g_hk_focus = parse_hotkey(v);

    g_nlim = 0;
    GetPrivateProfileSectionW(L"usage", buf, 8192, g_ini);
    for (wchar_t *p = buf; *p && g_nlim < MAX_LIMITS; p += wcslen(p) + 1) {
        wchar_t *eq = wcschr(p, L'=');
        if (!eq || !eq[1]) continue;
        *eq = 0;
        Limit *l = &g_lim[g_nlim++];
        memset(l, 0, sizeof *l);
        wcsncpy(l->label, p, 23);
        wcsncpy(l->id, eq + 1, 47);
    }
    if (!g_nlim) {
        g_lim[0] = (Limit){ .label = L"Claude 7d", .id = L"anthropic:7d" };
        g_lim[1] = (Limit){ .label = L"Codex 7d", .id = L"openai-codex:primary" };
        g_lim[2] = (Limit){ .label = L"Claude 5h", .id = L"anthropic:5h" };
        g_nlim = 3;
    }
    memset(&g_db_time, 0, sizeof g_db_time);         // re-read on the next scan

    for (int i = 0; i < g_n; i++) g_s[i].group = group_of(g_s[i].cwd);
    if (g_bar) { register_hotkeys(); build_items(); }
}

static void save_state(void) {
    WritePrivateProfileStringW(L"state", L"edge", edge_name(g_edge), g_state);
    WritePrivateProfileStringW(L"state", L"agent", g_agents[g_agent].name, g_state);
}

// Delete every [group:*] entry whose value is exactly `dir`.
static void ungroup(const wchar_t *dir) {
    static wchar_t names[4096], buf[8192];
    GetPrivateProfileSectionNamesW(names, 4096, g_ini);
    for (wchar_t *n = names; *n; n += wcslen(n) + 1) {
        if (_wcsnicmp(n, L"group:", 6)) continue;
        GetPrivateProfileSectionW(n, buf, 8192, g_ini);
        for (wchar_t *p = buf; *p; p += wcslen(p) + 1) {
            wchar_t *eq = wcschr(p, L'=');
            if (!eq) continue;
            *eq = 0;
            if (path_eq(eq + 1, dir)) WritePrivateProfileStringW(n, p, NULL, g_ini);
            *eq = L'=';
        }
    }
}

// Move a folder into [group:<name>]: it leaves any other group first. The key is a label
// derived from the folder name.
static BOOL add_to_group(const wchar_t *group, const wchar_t *dir) {
    wchar_t sec[64], key[MAX_PATH], cur[MAX_PATH];
    ungroup(dir);
    swprintf(sec, 64, L"group:%ls", group);
    // The key is a label from the folder name; characters the ini format treats specially are replaced.
    wchar_t stem[MAX_PATH];
    const wchar_t *label = base_name(dir);
    wcscpy(stem, *label && _wcsicmp(label, L"hotkey") ? label : L"dir");   // `hotkey` is reserved
    for (wchar_t *p = stem; *p; p++)
        if (*p == L'=' || *p == L'[' || *p == L']' || (p == stem && *p == L';')) *p = L'_';
    for (int n = 1;; n++) {
        if (n == 1) wcscpy(key, stem);
        else swprintf(key, MAX_PATH, L"%ls%d", stem, n);
        GetPrivateProfileStringW(sec, key, L"", cur, MAX_PATH, g_ini);
        if (!*cur) break;
    }
    return WritePrivateProfileStringW(sec, key, dir, g_ini);
}

static void check_config(void) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa)) {
        write_default_ini();
        if (!GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa)) return;
    }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_ini_time)) load_config();
}

/* ── session discovery ───────────────────────────────────────────────────────────────── */

// <name>.exe, or <name>.<anything>.exe: self-updating agents (omp) rename the running binary,
// e.g. omp.before-vocabulary-1792236567675.exe, while its sessions keep running.
static int agent_of(const wchar_t *exe) {
    size_t l = wcslen(exe);
    for (int a = 0; a < g_nagents; a++) {
        size_t n = wcslen(g_agents[a].name);
        if (!_wcsicmp(exe, g_agents[a].exe) ||
            (l > n + 4 && !_wcsnicmp(exe, g_agents[a].name, n) && exe[n] == L'.' && !_wcsicmp(exe + l - 4, L".exe")))
            return a;
    }
    return -1;
}

static BOOL read_title(Session *s) {
    if (!AttachConsole(s->pid)) return FALSE;
    s->con = GetConsoleWindow();
    if (!GetConsoleTitleW(s->title, 160)) s->title[0] = 0;
    FreeConsole();
    return s->con != NULL;
}

static void norm(const wchar_t *s, wchar_t *out, int cap) {   // drop spinner frames (Braille block)
    int n = 0;
    for (; *s && n < cap - 1; s++)
        if (*s < 0x2800 || *s > 0x28FF) out[n++] = *s;
    out[n] = 0;
}

// Agents animate a Braille spinner in the title while working.
static BOOL has_spinner(const wchar_t *s) {
    for (; *s; s++)
        if (*s >= 0x2800 && *s <= 0x28FF) return TRUE;
    return FALSE;
}

// Attach to the process's console: its window is owned by the hosting terminal window
// (Windows Terminal's PseudoConsoleWindow -> CASCADIA_HOSTING_WINDOW_CLASS). Consoles
// without a visible host (helpers, agents running inside other tools' ptys) are skipped.
static BOOL probe_console(Session *s) {
    if (!read_title(s)) return FALSE;
    s->term = GetAncestor(s->con, GA_ROOTOWNER);
    if (s->term == s->con) {
        wchar_t cls[32];
        GetClassNameW(s->con, cls, 32);
        if (!wcscmp(cls, L"PseudoConsoleWindow")) return FALSE;
    }
    return IsWindowVisible(s->term);
}

// Creation time (stable ordering) and cwd from the PEB (x64 layout:
// PEB+0x20 ProcessParameters, +0x38 CurrentDirectory.DosPath).
static void read_proc(HANDLE p, Session *s) {
    FILETIME c, x, k, u;
    if (GetProcessTimes(p, &c, &x, &k, &u)) s->born = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    PROCESS_BASIC_INFORMATION pbi;
    ULONG rl;
    PVOID params;
    UNICODE_STRING us;
    SIZE_T r;
    if (NT_SUCCESS(NtQueryInformationProcess(p, ProcessBasicInformation, &pbi, sizeof pbi, &rl)) &&
        ReadProcessMemory(p, (char *)pbi.PebBaseAddress + 0x20, &params, sizeof params, &r) &&
        ReadProcessMemory(p, (char *)params + 0x38, &us, sizeof us, &r)) {
        USHORT n = us.Length / 2;
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        if (ReadProcessMemory(p, us.Buffer, s->cwd, n * 2, &r)) {
            if (n > 3 && s->cwd[n - 1] == L'\\') n--;
            s->cwd[n] = 0;
        }
    }
}

// Examine a pid seen for the first time. Returns TRUE if it is a session (filled into *s).
static BOOL classify(DWORD pid, Session *s) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!p) return FALSE;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(p, 0, path, &len);
    int a = ok ? agent_of(base_name(path)) : -1;
    if (a >= 0) {
        memset(s, 0, sizeof *s);
        s->pid = pid;
        s->agent = a;
        ok = probe_console(s);
        if (ok) {
            read_proc(p, s);
            load_label(s);
            s->group = group_of(s->cwd);
            s->working = has_spinner(s->title);
        }
    } else {
        ok = FALSE;
    }
    CloseHandle(p);
    return ok;
}

static int by_born(const void *a, const void *b) {
    ULONGLONG x = ((const Session *)a)->born, y = ((const Session *)b)->born;
    return x < y ? -1 : x > y;
}

static BOOL is_ephemeral(const wchar_t *dir) {
    wchar_t t[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, t);
    if (!n) return FALSE;
    wcscpy(t + n, L"agentbar");
    return path_under(dir, t) && !path_eq(dir, t);
}

static BOOL same_view(const Session *a, const Session *b) {
    return a->pid == b->pid && a->term == b->term && a->sel == b->sel && a->working == b->working &&
           a->attn == b->attn && a->group == b->group && a->mins == b->mins && !wcscmp(a->cwd, b->cwd) &&
           !wcscmp(a->model, b->model) &&
           (!g_expanded || !wcscmp(a->title, b->title));
}

/* ── omp: model per tab ──────────────────────────────────────────────────────────────── */
// omp writes ~/.omp/agent/terminal-sessions/<terminal id> = "<cwd>\n<session file>\n…". Each session
// file starts with a {"type":"title",…} line kept current in place, and appends
// {"type":"model_change",…,"model":"provider/id"} lines. The tab's console title contains the
// session title, which tells apart several sessions in the same folder.

static int read_head(const wchar_t *path, char *buf, int cap) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, cap - 1, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    return (int)got;
}

// Value of the first "key":"…" in s, as UTF-16 (only \" and \\ unescaped).
static BOOL json_str(const char *s, const char *key, wchar_t *out, int cap) {
    char pat[40], val[512];
    int n = 0;
    strcpy(pat, "\"");
    strcat(pat, key);
    strcat(pat, "\":\"");
    const char *p = strstr(s, pat);
    if (!p) return FALSE;
    for (p += strlen(pat); *p && *p != '"' && n < (int)sizeof val - 1; p++) {
        if (*p == '\\' && p[1]) p++;
        val[n++] = *p;
    }
    val[n] = 0;
    return MultiByteToWideChar(CP_UTF8, 0, val, -1, out, cap) > 0;
}

// Find the session file of this tab: breadcrumbs for its folder, preferring the one whose title
// is in the console title, else the most recently written.
static void resolve_omp(Session *s) {
    wchar_t pat[MAX_PATH], path[MAX_PATH], cwd[MAX_PATH], file[MAX_PATH], t[128], want[160];
    wchar_t best[MAX_PATH] = L"", bestt[128] = L"";
    char buf[6 * MAX_PATH], head[2048];
    FILETIME bt = { 0, 0 };
    BOOL bestm = FALSE;
    norm(s->title, want, 160);
    swprintf(pat, MAX_PATH, L"%ls\\terminal-sessions\\*", g_omp_dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        swprintf(path, MAX_PATH, L"%ls\\terminal-sessions\\%ls", g_omp_dir, fd.cFileName);
        if (!read_head(path, buf, sizeof buf)) continue;
        char *l2 = strchr(buf, '\n');
        if (!l2) continue;
        *l2++ = 0;
        buf[strcspn(buf, "\r")] = 0;
        l2[strcspn(l2, "\r\n")] = 0;
        if (!MultiByteToWideChar(CP_UTF8, 0, buf, -1, cwd, MAX_PATH) || !path_eq(cwd, s->cwd) ||
            !MultiByteToWideChar(CP_UTF8, 0, l2, -1, file, MAX_PATH)) continue;
        t[0] = 0;
        if (read_head(file, head, sizeof head) && !strncmp(head, "{\"type\":\"title\"", 15)) json_str(head, "title", t, 128);
        BOOL m = *t && wcsstr(want, t);
        if (m > bestm || (m == bestm && CompareFileTime(&fd.ftLastWriteTime, &bt) > 0)) {
            bestm = m;
            bt = fd.ftLastWriteTime;
            wcscpy(best, file);
            wcscpy(bestt, t);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (wcscmp(best, s->file)) {
        wcscpy(s->file, best); s->foff = 0; s->back = -1; s->model[0] = 0;
        load_label(s);
    }
    wcscpy(s->ftitle, bestt);
}

static void short_model(const wchar_t *m, wchar_t *out) {   // "anthropic/claude-opus-5-5" -> "opus-5-5"
    const wchar_t *p = wcsrchr(m, L'/');
    p = p ? p + 1 : m;
    if (!wcsncmp(p, L"claude-", 7)) p += 7;
    wcsncpy(out, p, 39);
    out[39] = 0;
}

static DWORD read_at(HANDLE h, LONGLONG pos, char *buf, DWORD len) {
    LARGE_INTEGER p = { .QuadPart = pos };
    DWORD got = 0;
    if (!SetFilePointerEx(h, p, NULL, FILE_BEGIN) || !ReadFile(h, buf, len, &got, NULL)) got = 0;
    buf[got] = 0;
    return got;
}

static BOOL last_model(const char *buf, wchar_t *out) {   // latest model change in buf
    const char *last = NULL;
    for (const char *p = buf; (p = strstr(p, "{\"type\":\"model_change\"")); p++) last = p;
    wchar_t m[64];
    if (!last || !json_str(last, "model", m, 64)) return FALSE;
    short_model(m, out);
    return TRUE;
}

// Current model of the session. Runs on the UI thread, so work per call is bounded: newly appended
// lines are read forward from `foff`; on the first visit the file is searched backward from its end
// until the latest model change, a few MiB per scan, so a huge transcript never stalls the bar.
static void update_model(Session *s) {
    enum { CHUNK = 1 << 20, OVERLAP = 1 << 12, BUDGET = 4 };   // OVERLAP > any model_change line
    if (!*s->file) return;
    HANDLE h = CreateFileW(s->file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    char *buf = NULL;
    int budget = BUDGET;
    if (!GetFileSizeEx(h, &size)) goto done;
    if (s->back < 0 || size.QuadPart < s->foff) s->foff = s->back = size.QuadPart;   // first visit / rewritten
    if (s->foff == size.QuadPart && (s->model[0] || !s->back)) goto done;
    if (!(buf = HeapAlloc(GetProcessHeap(), 0, CHUNK + 1))) goto done;
    while (budget && s->foff < size.QuadPart) {                 // forward: appended lines
        DWORD got = read_at(h, s->foff, buf, CHUNK);
        if (!got) break;
        budget--;
        char *end = buf + got;                                  // consume whole lines only
        while (end > buf && end[-1] != '\n') end--;
        if (end == buf) {
            if (got < CHUNK) break;                             // last line still being written
            end = buf + got;                                    // a line longer than a chunk: step through it
        }
        *end = 0;
        if (last_model(buf, s->model)) s->back = 0;             // newer than anything before it
        s->foff += end - buf;
    }
    while (budget-- > 0 && !s->model[0] && s->back > 0) {     // backward: first visit only
        LONGLONG pos = s->back > CHUNK ? s->back - CHUNK : 0;
        DWORD got = read_at(h, pos, buf, (DWORD)(s->back - pos));
        if (s->back == s->foff) {                               // the tail: stop at its last full line,
            char *end = buf + got;                              // the forward pass takes it from there
            while (end > buf && end[-1] != '\n') end--;
            if (end > buf) { *end = 0; s->foff = pos + (end - buf); }
        }
        last_model(buf, s->model);
        s->back = pos ? pos + OVERLAP : 0;
    }
done:
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
}

static void omp_model(Session *s) {
    wchar_t want[160];
    norm(s->title, want, 160);
    BOOL valid = *s->file && *s->ftitle && wcsstr(want, s->ftitle);
    if (!valid && (!s->resolved || g_scans - s->resolved >= 10)) {   // unverified: re-check every ~20 s
        resolve_omp(s);
        s->resolved = g_scans;
    }
    update_model(s);
}

/* ── usage limits, as omp records them (agent.db usage_history) ──────────────────────── */
// Read through Windows' own winsqlite3.dll, only when the database changes. No network, no credentials.

typedef int (WINAPI *sq_open_t)(const char *, void **, int, const char *);
typedef int (WINAPI *sq_prepare_t)(void *, const char *, int, void **, const char **);
typedef int (WINAPI *sq_one_t)(void *);
typedef const void *(WINAPI *sq_text16_t)(void *, int);
typedef double (WINAPI *sq_double_t)(void *, int);
typedef LONGLONG (WINAPI *sq_int64_t)(void *, int);

static void read_usage(void) {
    static HMODULE dll;
    if (!dll && !(dll = LoadLibraryExW(L"winsqlite3.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32))) return;
    sq_open_t open = (sq_open_t)(void *)GetProcAddress(dll, "sqlite3_open_v2");
    sq_prepare_t prepare = (sq_prepare_t)(void *)GetProcAddress(dll, "sqlite3_prepare_v2");
    sq_one_t step = (sq_one_t)(void *)GetProcAddress(dll, "sqlite3_step");
    sq_one_t finalize = (sq_one_t)(void *)GetProcAddress(dll, "sqlite3_finalize");
    sq_one_t close = (sq_one_t)(void *)GetProcAddress(dll, "sqlite3_close");
    sq_text16_t text16 = (sq_text16_t)(void *)GetProcAddress(dll, "sqlite3_column_text16");
    sq_double_t dbl = (sq_double_t)(void *)GetProcAddress(dll, "sqlite3_column_double");
    sq_int64_t i64 = (sq_int64_t)(void *)GetProcAddress(dll, "sqlite3_column_int64");
    if (!open || !prepare || !step || !finalize || !close || !text16 || !dbl || !i64) return;
    wchar_t w[MAX_PATH];
    char path[3 * MAX_PATH];
    swprintf(w, MAX_PATH, L"%ls\\agent.db", g_omp_dir);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof path, NULL, NULL);
    void *db = NULL, *st = NULL;
    if (open(path, &db, 1 /* SQLITE_OPEN_READONLY */, NULL) == 0 &&
        prepare(db, "SELECT limit_id, used_fraction, resets_at, recorded_at FROM usage_history "
                    "WHERE id IN (SELECT MAX(id) FROM usage_history GROUP BY limit_id)", -1, &st, NULL) == 0) {
        while (step(st) == 100 /* SQLITE_ROW */) {
            const wchar_t *id = text16(st, 0);
            for (int i = 0; id && i < g_nlim; i++)
                if (!wcscmp(id, g_lim[i].id)) {
                    g_lim[i].used = dbl(st, 1);
                    g_lim[i].resets = i64(st, 2);
                    g_lim[i].at = i64(st, 3);
                    g_lim[i].have = TRUE;
                }
        }
    }
    if (st) finalize(st);
    if (db) close(db);
}

static LONGLONG now_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (LONGLONG)(((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ULL) / 10000);
}

static void check_usage(void) {
    if (!g_nlim) return;
    wchar_t p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME t = { 0, 0 };
    for (int k = 0; k < 2; k++) {                    // writes land in the -wal file first
        swprintf(p, MAX_PATH, k ? L"%ls\\agent.db-wal" : L"%ls\\agent.db", g_omp_dir);
        if (GetFileAttributesExW(p, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &t) > 0)
            t = fa.ftLastWriteTime;
    }
    if (CompareFileTime(&t, &g_db_time)) {
        g_db_time = t;
        read_usage();
        InvalidateRect(g_bar, NULL, FALSE);
    }
    LONGLONG at = 0;
    for (int i = 0; i < g_nlim; i++)
        if (g_lim[i].have && g_lim[i].at > at) at = g_lim[i].at;
    BOOL stale = now_ms() - at > 30 * 60000;         // omp has not refreshed them in a while
    if (stale != g_lim_stale) { g_lim_stale = stale; InvalidateRect(g_bar, NULL, FALSE); }
}

// Non-session pids are remembered so a scan is normally just a pid-set diff. Entries expire
// (SEEN_TTL scans) to recover from a pid being reused between two scans.
#define SEEN_TTL 60
typedef struct { DWORD pid; int ttl; } Seen;
static Seen g_seen[2048];
static int  g_nseen;

static void scan(void) {
    static Session ns[MAX_S];
    static DWORD pids[2048];
    static Seen keep[2048];
    DWORD cb;
    check_config();
    if (!EnumProcesses(pids, sizeof pids, &cb)) return;
    int np = (int)(cb / sizeof *pids), nn = 0, nk = 0;
    for (int i = 0; i < np; i++) {
        DWORD pid = pids[i];
        int k = 0;
        while (k < g_n && g_s[k].pid != pid) k++;
        if (k < g_n) {
            if (IsWindow(g_s[k].con) && nn < MAX_S) {
                ns[nn] = g_s[k];
                ns[nn].term = GetAncestor(ns[nn].con, GA_ROOTOWNER);   // a tab dragged out moves to a new window
                nn++;
            }
            continue;
        }
        k = 0;
        while (k < g_nseen && g_seen[k].pid != pid) k++;
        if (k < g_nseen && g_seen[k].ttl > 0) {
            keep[nk++] = (Seen){ pid, g_seen[k].ttl - 1 };
            continue;
        }
        if (nn < MAX_S && classify(pid, &ns[nn])) nn++;
        else keep[nk++] = (Seen){ pid, SEEN_TTL };
    }
    // oldest first; an agent sharing a console with an older one (child process) is not a new session
    qsort(ns, nn, sizeof *ns, by_born);
    int m = 0;
    for (int i = 0; i < nn; i++) {
        int j = 0;
        while (j < m && ns[j].con != ns[i].con) j++;
        if (j == m) ns[m++] = ns[i];
        else keep[nk++] = (Seen){ ns[i].pid, SEEN_TTL };
    }
    memcpy(g_seen, keep, nk * sizeof *keep);
    g_nseen = nk;

    // live title -> working state; a session that just finished while you were elsewhere asks for
    // attention. "Looking at it" = its terminal is foreground and, when the window has several
    // tabs, the window title (= active tab title) is this session's title.
    HWND fg = GetForegroundWindow();
    ULONGLONG now = GetTickCount64();
    wchar_t fgtitle[160] = L"", a[160], b[160];
    GetWindowTextW(fg, fgtitle, 160);
    norm(fgtitle, a, 160);
    for (int i = 0; i < m; i++) {
        Session *s = &ns[i];
        BOOL was = s->working;
        int mates = 0;
        for (int j = 0; j < m; j++) mates += ns[j].term == s->term;
        read_title(s);
        s->working = has_spinner(s->title);
        if (!s->working) s->since = 0;
        else if (!was || !s->since) s->since = now;   // found mid-run: counts from when the bar saw it
        s->mins = s->since ? (now - s->since) / 60000 : 0;
        norm(s->title, b, 160);
        BOOL looking = fg == s->term && (mates == 1 || !wcscmp(a, b));
        if (looking) s->attn = FALSE;
        else if (was && !s->working) s->attn = TRUE;
    }
    for (int i = 0; i < m; i++)
        if (!_wcsicmp(g_agents[ns[i].agent].exe, L"omp.exe")) omp_model(&ns[i]);
    g_scans++;
    check_usage();

    // sessions that went away: drop their ephemeral folder if the agent left it empty
    for (int i = 0; i < g_n; i++) {
        int j = 0;
        while (j < m && ns[j].pid != g_s[i].pid) j++;
        if (j == m && is_ephemeral(g_s[i].cwd)) RemoveDirectoryW(g_s[i].cwd);
    }

    BOOL changed = m != g_n;
    for (int i = 0; i < m && !changed; i++) changed = !same_view(&ns[i], &g_s[i]);
    memcpy(g_s, ns, m * sizeof *ns);
    g_n = m;
    if (changed) build_items();
}

/* ── actions ─────────────────────────────────────────────────────────────────────────── */

// Escape `s` for use inside "..." (CommandLineToArgvW rules): `"` -> `\"`, backslashes before a
// quote or the end doubled. `semi`: also `;` -> `\;` (Windows Terminal's command separator).
static wchar_t *escape_arg(wchar_t *o, const wchar_t *s, BOOL semi) {
    int bs = 0;
    for (; *s; s++) {
        if (*s == L'\\') { bs++; continue; }
        if (*s == L'"') {
            for (int i = 0; i < bs * 2 + 1; i++) *o++ = L'\\';
            *o++ = L'"';
            bs = 0;
            continue;
        }
        for (; bs; bs--) *o++ = L'\\';
        if (semi && *s == L';') *o++ = L'\\';
        *o++ = *s;
    }
    for (int i = 0; i < bs * 2; i++) *o++ = L'\\';
    *o = 0;
    return o;
}

// Append the prompt as one argument. It crosses two parses: wt.exe's, then the agent's (WT
// re-wraps it in quotes without escaping), so it is escaped for the agent, then for wt.
// For cmd.exe-wrapped agents `"` becomes `'`: cmd does not understand backslash escapes.
static void quote_prompt(wchar_t *o, const wchar_t *s, BOOL for_cmd) {
    static wchar_t plain[512], inner[1100];
    wcsncpy(plain, s, 511);
    plain[511] = 0;
    if (for_cmd)
        for (wchar_t *p = plain; *p; p++) if (*p == L'"') *p = L'\'';
    escape_arg(inner, plain, FALSE);
    *o++ = L'"';
    o = escape_arg(o, inner, TRUE);
    *o++ = L'"';
    *o = 0;
}

// Open the configured terminal in `dir` running the current agent; `prompt` (optional) is passed
// to it as the first message.
static void launch(const wchar_t *dir, const wchar_t *prompt) {
    static wchar_t cmd[4096], agent[2400];
    wchar_t d[MAX_PATH + 2];
    wcsncpy(d, dir, MAX_PATH);
    d[MAX_PATH] = 0;
    size_t n = wcslen(d);
    if (n && d[n - 1] == L'\\') wcscat(d, L".");     // `"C:\"` would escape the closing quote
    wcscpy(agent, g_agents[g_agent].cmd);
    if (prompt && *prompt) {
        size_t l = wcslen(agent);
        agent[l++] = L' ';
        quote_prompt(agent + l, prompt, !_wcsnicmp(agent, L"cmd ", 4));
    }
    size_t o = 0;
    for (const wchar_t *t = g_terminal; *t && o < sizeof cmd / sizeof *cmd - 1; t++) {
        const wchar_t *ins = t[0] == L'%' && t[1] == L'd' ? d : t[0] == L'%' && t[1] == L'c' ? agent : NULL;
        if (!ins) { cmd[o++] = *t; continue; }
        t++;
        size_t l = wcslen(ins);
        if (o + l >= sizeof cmd / sizeof *cmd) break;
        wcscpy(cmd + o, ins);
        o += l;
    }
    cmd[o] = 0;
    STARTUPINFOW si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, d, &si, &pi)) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_burst = 10;                                    // pick the new session up quickly
    SetTimer(g_bar, T_BURST, 400, NULL);
}

/* ── prompt box: one line, Enter = go, Esc / click away = cancel ─────────────────────── */

static void style_window(HWND w);

static HWND    g_ask_edit;
static int     g_ask_state;                      // 0 open, 1 accepted, 2 cancelled
static BOOL    g_asking;
static WNDPROC g_edit_proc;
static HBRUSH  g_br_bg, g_br_edit;

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_KEYDOWN || m == WM_CHAR) && (w == VK_RETURN || w == VK_ESCAPE)) {
        if (m == WM_KEYDOWN) g_ask_state = w == VK_RETURN ? 1 : 2;
        return 0;                                    // WM_CHAR swallowed: no beep
    }
    return CallWindowProcW(g_edit_proc, h, m, w, l);
}

static LRESULT CALLBACK ask_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_HOT);
        return (LRESULT)g_br_edit;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)w, C_MUTED);
        SetBkColor((HDC)w, C_BG);
        return (LRESULT)g_br_bg;
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_br_bg);
        return 1;
    }
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !g_ask_state) g_ask_state = 2;
        return 0;
    case WM_CLOSE:
        g_ask_state = 2;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL ask(const wchar_t *caption, wchar_t *out, int cap) {
    if (g_asking) return FALSE;
    static BOOL registered;
    HINSTANCE hi = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc = { .lpfnWndProc = ask_proc, .hInstance = hi, .hCursor = LoadCursorW(NULL, IDC_IBEAM),
                         .lpszClassName = L"agentbar.ask" };
        RegisterClassW(&wc);
        registered = TRUE;
    }
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(g_bar, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = S(600), h = S(76);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 4;
    g_br_bg = CreateSolidBrush(C_BG);
    g_br_edit = CreateSolidBrush(C_HOT);
    g_asking = TRUE;
    g_ask_state = 0;
    HWND d = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"agentbar.ask", L"", WS_POPUP,
                             x, y, w, h, NULL, NULL, hi, NULL);
    style_window(d);
    HWND lbl = CreateWindowExW(0, L"STATIC", caption, WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS,
                               S(12), S(8), w - S(24), S(18), d, NULL, hi, NULL);
    g_ask_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 S(12), S(34), w - S(24), S(26), d, NULL, hi, NULL);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)g_small, FALSE);
    SendMessageW(g_ask_edit, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_ask_edit, EM_SETLIMITTEXT, cap - 1, 0);
    if (*out) { SetWindowTextW(g_ask_edit, out); SendMessageW(g_ask_edit, EM_SETSEL, 0, -1); }   // prefill
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_ask_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    ShowWindow(d, SW_SHOW);
    SetForegroundWindow(d);
    SetFocus(g_ask_edit);
    MSG m;
    while (!g_ask_state) {
        if (GetMessageW(&m, NULL, 0, 0) <= 0) { PostQuitMessage(0); g_ask_state = 2; break; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    BOOL ok = g_ask_state == 1;
    if (ok) {
        GetWindowTextW(g_ask_edit, out, cap);
        wchar_t *p = out;
        while (*p == L' ') p++;
        memmove(out, p, (wcslen(p) + 1) * sizeof *p);
        size_t l = wcslen(out);
        while (l && out[l - 1] == L' ') out[--l] = 0;
    }
    DestroyWindow(d);
    DeleteObject(g_br_bg);
    DeleteObject(g_br_edit);
    g_asking = FALSE;
    return ok;
}

static void ideas_base(wchar_t *base) { ExpandEnvironmentStringsW(g_ideas_dir, base, MAX_PATH); }

// Next free <prefix>N: derived from the folder every time, so manual or forgotten folders count.
// A folder counts as <prefix>N or <prefix>N-<anything> (the slug a prompt may add).
static int next_idea(const wchar_t *base) {
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\%ls*", base, g_prefix);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchLimitToDirectories, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return 1;
    size_t pl = wcslen(g_prefix);
    long max = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        wchar_t *end;
        long v = wcstol(fd.cFileName + pl, &end, 10);
        if (end != fd.cFileName + pl && (!*end || *end == L'-') && v > max) max = v;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return (int)max + 1;
}

// Lowercase ASCII alphanumerics of `s`, runs of anything else collapsed to one '-'.
static void slugify(const wchar_t *s, wchar_t *out, int cap) {
    int n = 0;
    BOOL dash = FALSE;
    for (; *s && n < cap - 2; s++) {
        wchar_t c = *s;
        if (c < 128 && iswalnum(c)) {
            if (dash && n) out[n++] = L'-';
            dash = FALSE;
            out[n++] = towlower(c);
        } else {
            dash = TRUE;
        }
    }
    out[n] = 0;
}

static void new_idea(const wchar_t *prompt) {
    wchar_t base[MAX_PATH], dir[MAX_PATH], slug[32];
    ideas_base(base);
    SHCreateDirectoryExW(NULL, base, NULL);
    slugify(prompt, slug, 32);
    for (int n = next_idea(base);; n++) {
        if (*slug) swprintf(dir, MAX_PATH, L"%ls\\%ls%d-%ls", base, g_prefix, n, slug);
        else swprintf(dir, MAX_PATH, L"%ls\\%ls%d", base, g_prefix, n);
        if (CreateDirectoryW(dir, NULL)) { launch(dir, prompt); return; }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return;
    }
}

// Throwaway session: a fresh folder under %TEMP%\agentbar, removed again if left empty.
static void new_ephemeral(const wchar_t *prompt) {
    wchar_t dir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (!n || n > MAX_PATH - 32) return;
    swprintf(dir + n, MAX_PATH - n, L"agentbar\\%llu", GetTickCount64());
    if (SHCreateDirectoryExW(NULL, dir, NULL) == ERROR_SUCCESS) launch(dir, prompt);
}

static void ask_idea(void) {
    wchar_t base[MAX_PATH], cap[MAX_PATH], t[512] = L"";
    ideas_base(base);
    swprintf(cap, MAX_PATH, L"Quick idea \u2192 %ls%d   \u00b7   first prompt (optional; also names the folder)", g_prefix, next_idea(base));
    if (ask(cap, t, 512)) new_idea(t);
}

static void ask_ephemeral(void) {
    wchar_t t[512] = L"";
    if (ask(L"Ephemeral \u2192 %TEMP%   \u00b7   first prompt (optional)", t, 512)) new_ephemeral(t);
}

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static BOOL startup_enabled(void) {
    HKEY k;
    BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, L"agentbar", NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

static void set_startup(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t p[MAX_PATH + 2];
        DWORD n = GetModuleFileNameW(NULL, p + 1, MAX_PATH);
        p[0] = L'"';
        p[n + 1] = L'"';
        p[n + 2] = 0;
        RegSetValueExW(k, L"agentbar", 0, REG_SZ, (const BYTE *)p, (n + 3) * sizeof *p);
    } else {
        RegDeleteValueW(k, L"agentbar");
    }
    RegCloseKey(k);
}

static BOOL pick_folder(wchar_t *out) {
    IFileOpenDialog *d;
    BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return FALSE;
    FILEOPENDIALOGOPTIONS o;
    IFileOpenDialog_GetOptions(d, &o);
    IFileOpenDialog_SetOptions(d, o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IShellItem *it;
    PWSTR p;
    if (SUCCEEDED(IFileOpenDialog_Show(d, g_bar)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
        if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcsncpy(out, p, MAX_PATH - 1);
            out[MAX_PATH - 1] = 0;
            CoTaskMemFree(p);
            ok = TRUE;
        }
        IShellItem_Release(it);
    }
    IFileOpenDialog_Release(d);
    return ok;
}

static BOOL select_named(IUIAutomationElementArray *tabs, const wchar_t *want) {
    int n = 0;
    BOOL done = FALSE;
    IUIAutomationElementArray_get_Length(tabs, &n);
    for (int i = 0; i < n && !done; i++) {
        IUIAutomationElement *t;
        BSTR name = NULL;
        wchar_t got[160];
        if (FAILED(IUIAutomationElementArray_GetElement(tabs, i, &t))) continue;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentName(t, &name)) && name) {
            norm(name, got, 160);
            SysFreeString(name);
            IUIAutomationSelectionItemPattern *p = NULL;
            if (!wcscmp(got, want) &&
                SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(t, UIA_SelectionItemPatternId,
                          &IID_IUIAutomationSelectionItemPattern, (void **)&p)) && p) {
                done = SUCCEEDED(IUIAutomationSelectionItemPattern_Select(p));
                IUIAutomationSelectionItemPattern_Release(p);
            }
        }
        IUIAutomationElement_Release(t);
    }
    return done;
}

// The session is one of several tabs in its terminal window: select the tab whose title is the
// session's console title. UI Automation is only loaded when this case is actually clicked.
static void select_tab(Session *s) {
    IUIAutomation *ua;
    IUIAutomationElement *win;
    IUIAutomationCondition *cond;
    IUIAutomationElementArray *tabs;
    VARIANT v = { 0 };
    v.vt = VT_I4;
    v.lVal = UIA_TabItemControlTypeId;
    if (FAILED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER, &IID_IUIAutomation, (void **)&ua)))
        return;
    if (SUCCEEDED(IUIAutomation_ElementFromHandle(ua, s->term, &win))) {
        if (SUCCEEDED(IUIAutomation_CreatePropertyCondition(ua, UIA_ControlTypePropertyId, v, &cond))) {
            if (SUCCEEDED(IUIAutomationElement_FindAll(win, TreeScope_Descendants, cond, &tabs)) && tabs) {
                wchar_t want[160];
                read_title(s);
                norm(s->title, want, 160);
                select_named(tabs, want);
                IUIAutomationElementArray_Release(tabs);
            }
            IUIAutomationCondition_Release(cond);
        }
        IUIAutomationElement_Release(win);
    }
    IUIAutomation_Release(ua);
}

static int sharing(const Session *s) {               // sessions hosted by the same terminal window
    int n = 0;
    for (int j = 0; j < g_n; j++) n += g_s[j].term == s->term;
    return n;
}

static void focus(Session *s) {
    s->term = GetAncestor(s->con, GA_ROOTOWNER);    // may have moved since the last scan
    if (sharing(s) > 1) select_tab(s);
    if (IsIconic(s->term)) ShowWindow(s->term, SW_RESTORE);
    SetForegroundWindow(s->term);                    // allowed: we just received the click / hotkey
    s->attn = FALSE;
    build_items();
}

// Group header / group hotkey: focus the group's session (the one in its first folder, else one
// that wants attention, else any), or start one in the first folder if it has none.
static void open_group(int g) {
    if (!g_groups[g].ndirs) return;
    Session *pick = NULL;
    for (int i = 0; i < g_n; i++) {
        if (g_s[i].group != g) continue;
        if (path_eq(g_s[i].cwd, g_groups[g].dirs[0])) { pick = &g_s[i]; break; }
        if (!pick || (g_s[i].attn && !pick->attn)) pick = &g_s[i];
    }
    if (pick) focus(pick);
    else launch(g_groups[g].dirs[0], NULL);
}

// Grid like tmux "tiled": ceil(sqrt n) columns, last row shares the full width.
static void tile_windows(HWND *ws, int n) {
    if (!n) return;
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(g_bar, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wa = mi.rcWork;                             // already excludes the bar and taskbar
    int W = wa.right - wa.left, H = wa.bottom - wa.top;
    int cols = 1;
    while (cols * cols < n) cols++;
    int rows = (n + cols - 1) / cols;
    for (int k = 0; k < n; k++) {
        int row = k / cols, col = k % cols, inrow = row == rows - 1 ? n - row * cols : cols;
        RECT t = { wa.left + W * col / inrow, wa.top + H * row / rows,
                   wa.left + W * (col + 1) / inrow, wa.top + H * (row + 1) / rows };
        HWND w = ws[k];
        WINDOWPLACEMENT wp = { .length = sizeof wp };
        if (GetWindowPlacement(w, &wp) && (IsIconic(w) || IsZoomed(w))) {
            // Restore straight into the target: Windows Terminal re-applies its restore rect after
            // a plain SW_RESTORE, which put every window back on its old (often identical) spot.
            // rcNormalPosition is in work-area coordinates.
            wp.showCmd = SW_SHOWNORMAL;
            wp.flags = 0;
            wp.rcNormalPosition = t;
            OffsetRect(&wp.rcNormalPosition, mi.rcMonitor.left - wa.left, mi.rcMonitor.top - wa.top);
            SetWindowPlacement(w, &wp);
        }
        RECT wr, fr;                                 // compensate invisible DWM resize borders
        GetWindowRect(w, &wr);
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr))) {
            t.left -= fr.left - wr.left;
            t.top -= fr.top - wr.top;
            t.right += wr.right - fr.right;
            t.bottom += wr.bottom - fr.bottom;
        }
        SetWindowPos(w, HWND_TOP, t.left, t.top, t.right - t.left, t.bottom - t.top, SWP_NOACTIVATE);
    }
    SetForegroundWindow(ws[0]);                      // only one focus change is allowed; z-order raised the rest
    build_items();
}

// Tile the selected sessions' windows, or all of them if nothing is selected (group < 0), or
// one group's (group >= 0). Sessions sharing a window tile it once.
static void tile(int group) {
    HWND ws[MAX_S];
    int n = 0;
    BOOL any = FALSE;
    for (int i = 0; i < g_n; i++) any |= g_s[i].sel;
    for (int i = 0; i < g_n; i++) {
        if (group >= 0 ? g_s[i].group != group : any && !g_s[i].sel) continue;
        int j = 0;
        g_s[i].term = GetAncestor(g_s[i].con, GA_ROOTOWNER);
        while (j < n && ws[j] != g_s[i].term) j++;
        if (j == n) ws[n++] = g_s[i].term;
    }
    for (int i = 0; i < g_n; i++) g_s[i].sel = FALSE;
    tile_windows(ws, n);
}

static void focus_nth(int n) {                       // Nth session as listed on the bar
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i].kind == IT_SESSION && --n == 0) { focus(&g_s[g_items[i].idx]); return; }
}

/* ── layout & drawing ────────────────────────────────────────────────────────────────── */
// The bar is a pill centred on its edge and exactly as long as its contents: sessions, then + and
// Open, then the usage meters. Plain GDI; the window's rounded corners and hairline border are DWM's.

static BOOL vertical(void) { return g_edge != ABE_TOP; }

static int item_len(const Item *it) {               // length along the edge
    BOOL v = vertical();
    switch (it->kind) {
    case IT_SESSION: return g_sess_len ? g_sess_len : S(v ? 58 : 168);
    case IT_GROUP:   return S(v ? 26 : 84);
    case IT_PLUS:    return S(40);
    case IT_OPEN:    return S(v ? 40 : 80);
    default:         return S(10 + (v ? 26 : 136) * g_nlim);          // usage meters
    }
}

static int content_len(void) {
    int n = 2 * S(PAD);
    for (int i = 0; i < g_nitems; i++) n += item_len(&g_items[i]);
    return n;
}

static void layout(void) {
    RECT c;
    GetClientRect(g_bar, &c);
    int pos = S(PAD);
    for (int i = 0; i < g_nitems; i++) {
        Item *it = &g_items[i];
        int len = item_len(it);
        if (vertical()) SetRect(&it->r, 0, pos, c.right, pos + len);
        else SetRect(&it->r, pos, 0, pos + len, c.bottom);
        pos += len;
    }
}

static void apply_size(void);

static void build_items(void) {
    int n = 0;
    for (int g = 0; g < g_ngroups; g++) {
        g_items[n++] = (Item){ IT_GROUP, g, { 0 } };
        for (int i = 0; i < g_n; i++)
            if (g_s[i].group == g) g_items[n++] = (Item){ IT_SESSION, i, { 0 } };
    }
    for (int i = 0; i < g_n; i++)
        if (g_s[i].group < 0) g_items[n++] = (Item){ IT_SESSION, i, { 0 } };
    g_items[n++] = (Item){ IT_PLUS, 0, { 0 } };
    g_items[n++] = (Item){ IT_OPEN, 0, { 0 } };
    if (g_nlim) g_items[n++] = (Item){ IT_USAGE, 0, { 0 } };
    g_nitems = n;
    if (g_hot >= n) g_hot = -1;
    apply_size();                                    // the pill grows and shrinks with its contents
}

static int hit(POINT p) {
    for (int i = 0; i < g_nitems; i++)
        if (PtInRect(&g_items[i].r, p)) return i;
    return -1;
}

static void fill(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

// Rounded box; `line` draws a 1px outline in that colour (0 = none).
static void round_fill(HDC dc, RECT r, int radius, COLORREF c, COLORREF line) {
    SelectObject(dc, GetStockObject(line ? DC_PEN : NULL_PEN));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCPenColor(dc, line);
    SetDCBrushColor(dc, c);
    RoundRect(dc, r.left, r.top, r.right + !line, r.bottom + !line, radius, radius);
}

// A thin light strip along the bottom of a tile: full colour in the middle, fading out to both ends.
static void glow(HDC dc, RECT b, COLORREF bg, COLORREF c) {
    int x0 = b.left + S(18), x1 = b.right - S(18), h = S(2) > 2 ? S(2) : 2, y = b.bottom - h - S(1);
    double mid = (x0 + x1) / 2.0, half = (x1 - x0) / 2.0;
    for (int x = x0; x < x1; x++) {
        double dx = x - mid;
        double t = 1 - (dx < 0 ? -dx : dx) / half;   // 0 at the ends, 1 in the middle
        t = t * t * (3 - 2 * t);                     // smoothstep
        RECT px = { x, y, x + 1, y + h };
        fill(dc, px, RGB(GetRValue(bg) + (GetRValue(c) - GetRValue(bg)) * t,
                         GetGValue(bg) + (GetGValue(c) - GetGValue(bg)) * t,
                         GetBValue(bg) + (GetBValue(c) - GetBValue(bg)) * t));
    }
}

static void text(HDC dc, HFONT f, COLORREF c, const wchar_t *s, RECT r, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX);
}

static void text_wrap(HDC dc, HFONT f, COLORREF c, const wchar_t *s, RECT r) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// Status dot + folder, then model (and minutes working). Hovered: the agent's title and the path.
static void draw_session(HDC dc, RECT b, const Session *s) {
    const Agent *a = &g_agents[s->agent];
    const wchar_t *name = s->label[0] ? s->label : s->cwd[0] ? base_name(s->cwd) : a->name;
    const wchar_t *model = s->model[0] ? s->model : a->name;
    wchar_t took[16] = L"";
    if (s->working) {
        if (!s->mins) wcscpy(took, L"<1m");
        else if (s->mins < 60) swprintf(took, 16, L"%llum", s->mins);
        else swprintf(took, 16, L"%lluh%02llu", s->mins / 60, s->mins % 60);
    }
    int x = b.left + S(12), right = b.right - S(10), h1 = S(17), h2 = S(14);
    int y = g_expanded ? b.top + S(5) : b.top + (b.bottom - b.top - h1 - h2) / 2;
    RECT r1 = { x, y, right, y + h1 };
    RECT r2 = { x, y + h1, right, y + h1 + h2 };
    if (g_expanded && vertical()) {                  // side panel: model and time share the name's line
        if (*took) { text(dc, g_small, C_WORK, took, r1, DT_RIGHT | DT_VCENTER); r1.right -= S(36); }
        text(dc, g_small, C_MUTED, model, r1, DT_RIGHT | DT_VCENTER);
        r1.right -= S(84);
        text(dc, g_font, C_TEXT, name, r1, DT_VCENTER | DT_END_ELLIPSIS);
        text(dc, g_small, C_TEXT, s->title, r2, DT_VCENTER | DT_END_ELLIPSIS);
        RECT r3 = { x, r2.bottom, right, r2.bottom + h2 };
        text(dc, g_small, C_MUTED, s->cwd, r3, DT_VCENTER | DT_PATH_ELLIPSIS);
        return;
    }
    text(dc, g_font, C_TEXT, name, r1, DT_VCENTER | DT_END_ELLIPSIS);
    if (*took) { text(dc, g_small, C_WORK, took, r2, DT_RIGHT | DT_VCENTER); r2.right -= S(34); }
    text(dc, g_small, C_MUTED, model, r2, DT_VCENTER | DT_END_ELLIPSIS);
    if (!g_expanded) return;
    RECT r3 = { x, r2.bottom + S(6), right, r2.bottom + S(6) + 2 * S(16) };   // two full lines
    text_wrap(dc, g_small, C_TEXT, s->title, r3);
    RECT r4 = { x, r3.bottom + S(2), right, r3.bottom + S(2) + h2 };
    text(dc, g_small, C_MUTED, s->cwd, r4, DT_VCENTER | DT_PATH_ELLIPSIS);
}

static void fmt_when(LONGLONG ms, wchar_t *out, int cap) {   // "Thu 06:00", local time
    static const wchar_t *days[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)ms * 10000 + 116444736000000000ULL;
    FILETIME ft = { u.LowPart, u.HighPart }, lt;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lt);
    FileTimeToSystemTime(&lt, &st);
    swprintf(out, cap, L"%ls %02d:%02d", days[st.wDayOfWeek], st.wHour, st.wMinute);
}

static void fmt_age(LONGLONG ms, wchar_t *out, int cap) {
    long m = (long)(ms / 60000);
    if (m < 1) wcscpy(out, L"just now");
    else if (m < 120) swprintf(out, cap, L"%ldm ago", m);
    else swprintf(out, cap, L"%ldh ago", m / 60);
}

// What is still available per limit: a percentage over a thin meter (green, amber <= 30%, red <= 10%).
static void draw_usage(HDC dc, RECT b) {
    LONGLONG now = now_ms();
    for (int i = 0; i < g_nlim; i++) {
        const Limit *l = &g_lim[i];
        double left = l->used < 0 ? 1 : l->used > 1 ? 0 : 1 - l->used;
        COLORREF col = !l->have || g_lim_stale ? C_MUTED : left <= .1 ? C_ERR : left <= .3 ? C_WAIT : C_WORK;
        wchar_t pct[16], when[24], age[24];
        if (l->have) swprintf(pct, 16, L"%d%%", (int)(left * 100 + .5));
        else wcscpy(pct, L"\u2013");
        if (l->have) { fmt_when(l->resets, when, 24); fmt_age(now - l->at, age, 24); }
        RECT bar, lab, val;
        if (vertical()) {                            // one row per limit: label ... value, meter below
            int rh = (b.bottom - b.top) / g_nlim;
            RECT row = { b.left + S(8), b.top + i * rh, b.right - S(8), b.top + (i + 1) * rh };
            SetRect(&bar, row.left, row.bottom - S(6), row.right, row.bottom - S(4));
            lab = val = row;
            lab.bottom = val.bottom = bar.top;
            text(dc, g_small, l->have && !g_lim_stale ? C_TEXT : C_MUTED, pct, val, DT_RIGHT | DT_VCENTER);
            lab.right -= S(30);
            text(dc, g_small, C_MUTED, l->label, lab, DT_VCENTER | DT_END_ELLIPSIS);
            if (g_expanded && l->have) {
                wchar_t info[64];
                swprintf(info, 64, L"resets %ls \u00b7 %ls", when, age);
                lab.left += S(54);
                text(dc, g_small, C_MUTED, info, lab, DT_VCENTER | DT_END_ELLIPSIS);
            }
        } else {                                     // one column per limit: "Claude 7d   62%", meter below
            int cw = (b.right - b.left - S(8)) / g_nlim;
            int cx = b.left + S(8) + i * cw, top = g_expanded ? b.top + S(5) : b.top + (b.bottom - b.top - S(25)) / 2;
            COLORREF brand = !wcsncmp(l->id, L"anthropic", 9) ? C_CLAUDE : !wcsncmp(l->id, L"openai", 6) ? C_CODEX : C_TEXT;
            SetRect(&lab, cx, top, cx + cw - S(12), top + S(17));
            SetRect(&bar, cx, lab.bottom + S(4), lab.right, lab.bottom + S(8));
            val = lab;
            text(dc, g_font, l->have && !g_lim_stale ? C_TEXT : C_MUTED, pct, val, DT_RIGHT | DT_VCENTER);
            lab.right -= S(36);
            text(dc, g_font, brand, l->label, lab, DT_VCENTER | DT_END_ELLIPSIS);
            if (g_expanded && l->have) {
                RECT w = { cx, bar.bottom + S(8), val.right, bar.bottom + S(8) + S(14) };
                text(dc, g_small, C_MUTED, when, w, DT_VCENTER | DT_END_ELLIPSIS);
                OffsetRect(&w, 0, S(14));
                text(dc, g_small, C_MUTED, age, w, DT_VCENTER | DT_END_ELLIPSIS);
            }
        }
        fill(dc, bar, C_SEP);
        if (l->have) {
            RECT f = bar;
            f.right = f.left + (LONG)((bar.right - bar.left) * left);
            fill(dc, f, col);
        }
    }
}

static void top_band(RECT *b) {                     // +, Open and group labels stay on the bar's line
    if (g_expanded && !vertical()) b->bottom = b->top + S(THICK_H) - S(8);
}

static void divider(HDC dc, RECT r) {
    int w = S(1) > 1 ? S(1) : 1;
    RECT l = vertical() ? (RECT){ r.left + S(14), r.top, r.right - S(14), r.top + w }
                        : (RECT){ r.left, r.top + S(11), r.left + w, r.top + S(THICK_H) - S(11) };
    fill(dc, l, C_SEP);
}

static void paint(HDC dc, RECT c) {
    fill(dc, c, C_BG);
    SetBkMode(dc, TRANSPARENT);
    int nsel = 0;
    for (int i = 0; i < g_n; i++) nsel += g_s[i].sel;
    for (int i = 0; i < g_nitems; i++) {
        const Item *it = &g_items[i];
        RECT b = it->r;
        if (vertical()) InflateRect(&b, -S(4), -S(2));
        else InflateRect(&b, -S(2), -S(4));
        if (i && (it->kind == IT_PLUS || it->kind == IT_USAGE || it->kind == IT_GROUP)) divider(dc, it->r);
        const Session *s = it->kind == IT_SESSION ? &g_s[it->idx] : NULL;
        BOOL hot = i == g_hot && it->kind != IT_USAGE;
        COLORREF bg = s && s->sel ? C_SEL : hot ? C_HOT : C_BG;
        if (!s) top_band(&b);
        if (bg != C_BG) round_fill(dc, b, S(12), bg, 0);
        if (s && (s->attn || s->working)) {          // along the pill's bottom edge, below the text
            RECT g = { b.left, b.top, b.right, it->r.bottom };
            glow(dc, g, C_BG, s->attn ? C_ATTN : C_RUN);
        }
        if (s) {
            draw_session(dc, b, s);
        } else if (it->kind == IT_GROUP) {
            text(dc, g_small, C_MUTED, g_groups[it->idx].name, b,
                 (vertical() ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_END_ELLIPSIS);
        } else if (it->kind == IT_USAGE) {
            draw_usage(dc, it->r);
        } else if (it->kind == IT_PLUS) {
            text(dc, g_big, C_TEXT, L"+", b, DT_CENTER | DT_VCENTER);
        } else {
            wchar_t lbl[32];
            if (nsel) swprintf(lbl, 32, L"Open %d", nsel);
            else wcscpy(lbl, L"Open all");
            text(dc, g_font, nsel ? C_ACCENT : C_TEXT, lbl, b, DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }
}

static HFONT font(int tenths, int weight, const wchar_t *face) {   // size in tenths of a point
    return CreateFontW(-MulDiv(tenths, (int)g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       0, 0, CLEARTYPE_QUALITY, 0, face);
}

static void make_fonts(void) {
    if (g_font) { DeleteObject(g_font); DeleteObject(g_small); DeleteObject(g_big); }
    g_font = font(95, FW_SEMIBOLD, L"Segoe UI Variable Text");
    g_small = font(80, FW_NORMAL, L"Segoe UI Variable Text");
    g_big = font(150, FW_LIGHT, L"Segoe UI Variable Display");
}

// Dark mode, rounded corners and a hairline border, all drawn by DWM (Windows 11).
static void style_window(HWND w) {
    BOOL dark = TRUE;
    DWORD round = 2 /* DWMWCP_ROUND */;
    COLORREF border = C_SEP;
    DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DwmSetWindowAttribute(w, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    DwmSetWindowAttribute(w, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
}

// Popup menus follow the dark look. uxtheme's SetPreferredAppMode (ordinal 135) is undocumented
// but what Explorer and Notepad use; if it is missing, menus simply stay light.
static void dark_menus(void) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    typedef int (WINAPI *set_mode_t)(int);
    typedef void (WINAPI *flush_t)(void);
    set_mode_t set_mode = (set_mode_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    flush_t flush = (flush_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(2 /* ForceDark */);
    if (flush) flush();
}

/* ── appbar docking & expansion ──────────────────────────────────────────────────────── */
// The appbar reserves a strip along the edge (so maximized windows stay clear); the pill sits
// centred in it. Hovering grows the pill over the windows; the reservation stays the same.

static void apply_size(void) {
    if (!g_bar) return;
    BOOL v = vertical();
    int thick = S(v ? (g_expanded ? EXPAND_V : THICK_V) : (g_expanded ? EXPAND_H : THICK_H));
    int m = S(MARGIN);
    int avail = v ? g_rc.bottom - g_rc.top - 2 * m : g_rc.right - g_rc.left - 2 * m;
    g_sess_len = 0;
    int len = content_len();
    if (len > avail) {                               // too many sessions: narrow their tiles so that
        int ns = 0;                                  // +, Open and the meters stay on screen
        for (int i = 0; i < g_nitems; i++) ns += g_items[i].kind == IT_SESSION;
        if (ns) {
            int per = (avail - (len - ns * S(v ? 58 : 168))) / ns, min = S(v ? 36 : 72);
            g_sess_len = per > min ? per : min;
            len = content_len();
        }
        if (len > avail) len = avail;
    }
    RECT r;
    if (v) {
        r.top = g_rc.top + m + (avail - len) / 2;
        r.left = g_edge == ABE_LEFT ? g_rc.left + m : g_rc.right - m - thick;
        r.bottom = r.top + len;
        r.right = r.left + thick;
    } else {
        r.left = g_rc.left + m + (avail - len) / 2;
        r.top = g_rc.top + m;
        r.right = r.left + len;
        r.bottom = r.top + thick;
    }
    SetWindowPos(g_bar, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    InvalidateRect(g_bar, NULL, FALSE);
}

static void set_pos(void) {
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    APPBARDATA ab = { .cbSize = sizeof ab, .hWnd = g_bar, .uEdge = g_edge, .rc = mi.rcMonitor };
    int t = S((vertical() ? THICK_V : THICK_H) + 2 * MARGIN);
    SHAppBarMessage(ABM_QUERYPOS, &ab);
    if (g_edge == ABE_LEFT) ab.rc.right = ab.rc.left + t;
    else if (g_edge == ABE_RIGHT) ab.rc.left = ab.rc.right - t;
    else ab.rc.bottom = ab.rc.top + t;
    SHAppBarMessage(ABM_SETPOS, &ab);
    g_rc = ab.rc;
    apply_size();
}

static void set_expanded(BOOL e) {
    if (g_expanded == e) return;
    g_expanded = e;
    g_tracking = FALSE;                              // re-arm hover/leave tracking
    if (e)
        for (int i = 0; i < g_n; i++) read_title(&g_s[i]);
    apply_size();
}

/* ── menus ───────────────────────────────────────────────────────────────────────────── */

static int popup(HMENU m, int x, int y, UINT align) {
    g_menu = TRUE;
    SetForegroundWindow(g_bar);
    int cmd = TrackPopupMenuEx(m, align | TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, g_bar, NULL);
    PostMessageW(g_bar, WM_NULL, 0, 0);
    g_menu = FALSE;
    return cmd;
}

static void anchor(int item, int *x, int *y, UINT *align) {
    RECT r = g_items[item].r;
    MapWindowPoints(g_bar, NULL, (POINT *)&r, 2);
    *x = g_edge == ABE_LEFT ? r.right : r.left;
    *y = g_edge == ABE_TOP ? r.bottom : r.top;
    *align = g_edge == ABE_RIGHT ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
}

static void collapse_if_outside(void) {
    POINT p;
    GetCursorPos(&p);
    if (WindowFromPoint(p) != g_bar) set_expanded(FALSE);
}

static void plus_menu(int item) {
    int x, y;
    UINT align;
    anchor(item, &x, &y, &align);
    wchar_t base[MAX_PATH], dir[MAX_PATH], label[MAX_PATH + 32];
    ideas_base(base);
    for (;;) {
        HMENU m = CreatePopupMenu();
        swprintf(label, sizeof label / sizeof *label, L"Quick idea  \u2192  %ls%d", g_prefix, next_idea(base));
        AppendMenuW(m, MF_STRING, CMD_IDEA, label);
        AppendMenuW(m, MF_STRING, CMD_EPHEMERAL, L"Ephemeral  \u2192  %TEMP%");
        AppendMenuW(m, MF_STRING, CMD_FOLDER, L"Open in folder\u2026");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (int a = 0; a < g_nagents; a++) AppendMenuW(m, MF_STRING, CMD_AGENT + a, g_agents[a].name);
        CheckMenuRadioItem(m, CMD_AGENT, CMD_AGENT + g_nagents - 1, CMD_AGENT + g_agent, MF_BYCOMMAND);
        int cmd = popup(m, x, y, align);
        DestroyMenu(m);
        if (cmd >= CMD_AGENT && cmd < CMD_AGENT + g_nagents) { g_agent = cmd - CMD_AGENT; save_state(); continue; }
        if (cmd == CMD_IDEA) ask_idea();
        else if (cmd == CMD_EPHEMERAL) ask_ephemeral();
        else if (cmd == CMD_FOLDER && pick_folder(dir)) launch(dir, NULL);
        break;
    }
    collapse_if_outside();
}

// Names live in the state file, keyed by the session's transcript (omp) or else its folder.
static const wchar_t *label_key(const Session *s) { return s->file[0] ? s->file : s->cwd; }

static void load_label(Session *s) {
    GetPrivateProfileStringW(L"names", label_key(s), L"", s->label, 64, g_state);
}

static void rename_session(Session *s) {
    wchar_t t[64];
    wcscpy(t, s->label[0] ? s->label : base_name(s->cwd));
    if (!ask(L"Rename   \u00b7   empty = folder name", t, 64)) return;
    WritePrivateProfileStringW(L"names", label_key(s), *t ? t : NULL, g_state);
    wcscpy(s->label, t);
    InvalidateRect(g_bar, NULL, FALSE);
}

static void session_menu(int item) {
    Session *s = &g_s[g_items[item].idx];
    int x, y;
    UINT align;
    anchor(item, &x, &y, &align);
    HMENU m = CreatePopupMenu(), gm = CreatePopupMenu();
    for (int g = 0; g < g_ngroups; g++)
        AppendMenuW(gm, MF_STRING | (s->group == g ? MF_CHECKED : 0), CMD_GROUP + g, g_groups[g].name);
    if (!g_ngroups) AppendMenuW(gm, MF_STRING | MF_GRAYED, 0, L"(no groups in agentbar.ini)");
    AppendMenuW(gm, MF_SEPARATOR, 0, NULL);
    AppendMenuW(gm, MF_STRING | (s->group < 0 ? MF_GRAYED : 0), CMD_UNGROUP, L"Ungroup");
    AppendMenuW(m, MF_STRING, CMD_RENAME, L"Rename\u2026");
    AppendMenuW(m, MF_STRING, CMD_EXPLORE, L"Open folder in Explorer");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)gm, L"Group");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (sharing(s) > 1 ? MF_GRAYED : 0), CMD_CLOSE, L"Close session");
    int cmd = popup(m, x, y, align);
    DestroyMenu(m);
    if (cmd == CMD_RENAME) rename_session(s);
    else if (cmd == CMD_EXPLORE) ShellExecuteW(NULL, L"explore", s->cwd, NULL, NULL, SW_SHOWNORMAL);
    else if (cmd == CMD_CLOSE) PostMessageW(s->term, WM_CLOSE, 0, 0);
    else if (cmd == CMD_UNGROUP) { ungroup(s->cwd); load_config(); }
    else if (cmd >= CMD_GROUP && cmd < CMD_GROUP + g_ngroups) {
        add_to_group(g_groups[cmd - CMD_GROUP].name, s->cwd);
        load_config();
    }
    collapse_if_outside();
}

static void group_menu(int item) {
    Group *g = &g_groups[g_items[item].idx];
    int x, y;
    UINT align;
    anchor(item, &x, &y, &align);
    HMENU m = CreatePopupMenu();
    wchar_t label[MAX_PATH + 16];
    for (int d = 0; d < g->ndirs; d++) {
        swprintf(label, MAX_PATH + 16, L"New session in %ls", base_name(g->dirs[d]));
        AppendMenuW(m, MF_STRING, CMD_GDIR + d, label);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_GTILE, L"Tile this group");
    int cmd = popup(m, x, y, align);
    DestroyMenu(m);
    if (cmd == CMD_GTILE) tile(g_items[item].idx);
    else if (cmd >= CMD_GDIR && cmd < CMD_GDIR + g->ndirs) launch(g->dirs[cmd - CMD_GDIR], NULL);
    collapse_if_outside();
}

static void bar_menu(void) {
    POINT p;
    GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_LEFT, L"Dock left");
    AppendMenuW(m, MF_STRING, CMD_RIGHT, L"Dock right");
    AppendMenuW(m, MF_STRING, CMD_TOP, L"Dock top");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), CMD_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_STRING, CMD_QUIT, L"Quit");
    UINT cur = g_edge == ABE_LEFT ? CMD_LEFT : g_edge == ABE_RIGHT ? CMD_RIGHT : CMD_TOP;
    CheckMenuRadioItem(m, CMD_LEFT, CMD_TOP, cur, MF_BYCOMMAND);
    int cmd = popup(m, p.x, p.y, g_edge == ABE_RIGHT ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    DestroyMenu(m);
    if (cmd == CMD_QUIT) { DestroyWindow(g_bar); return; }
    if (cmd == CMD_STARTUP) set_startup(!startup_enabled());
    if (cmd >= CMD_LEFT && cmd <= CMD_TOP) {
        g_edge = cmd == CMD_LEFT ? ABE_LEFT : cmd == CMD_RIGHT ? ABE_RIGHT : ABE_TOP;
        g_expanded = FALSE;
        save_state();
        set_pos();
    }
    collapse_if_outside();
}

/* ── window ──────────────────────────────────────────────────────────────────────────── */

static void click(int i, BOOL ctrl) {
    if (i < 0) return;
    Item *it = &g_items[i];
    switch (it->kind) {
    case IT_SESSION:
        if (ctrl) { g_s[it->idx].sel = !g_s[it->idx].sel; InvalidateRect(g_bar, NULL, FALSE); }
        else focus(&g_s[it->idx]);
        break;
    case IT_GROUP:
        if (ctrl) {
            BOOL any = FALSE;
            for (int k = 0; k < g_n; k++) any |= g_s[k].group == it->idx && g_s[k].sel;
            for (int k = 0; k < g_n; k++) if (g_s[k].group == it->idx) g_s[k].sel = !any;
            InvalidateRect(g_bar, NULL, FALSE);
        } else {
            open_group(it->idx);
        }
        break;
    case IT_PLUS: plus_menu(i); break;
    case IT_USAGE: break;
    default: tile(-1); break;
    }
}

static void rclick(int i) {
    if (i >= 0 && g_items[i].kind == IT_SESSION) session_menu(i);
    else if (i >= 0 && g_items[i].kind == IT_GROUP) group_menu(i);
    else bar_menu();
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == g_taskbar_created && msg) {           // Explorer restarted: the appbar must register again
        APPBARDATA ab = { .cbSize = sizeof ab, .hWnd = h, .uCallbackMessage = WM_APPBAR_CB };
        SHAppBarMessage(ABM_NEW, &ab);
        set_pos();
        return 0;
    }
    switch (msg) {
    case WM_APPBAR_CB:
        if (w == ABN_POSCHANGED) set_pos();
        else if (w == ABN_FULLSCREENAPP)             // step aside for fullscreen apps
            SetWindowPos(h, l ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    case WM_ACTIVATE:
    case WM_WINDOWPOSCHANGED: {
        APPBARDATA ab = { .cbSize = sizeof ab, .hWnd = h };
        SHAppBarMessage(msg == WM_ACTIVATE ? ABM_ACTIVATE : ABM_WINDOWPOSCHANGED, &ab);
        break;
    }
    case WM_DPICHANGED:
        g_dpi = HIWORD(w);
        make_fonts();
        set_pos();
        return 0;
    case WM_DISPLAYCHANGE:
        set_pos();
        return 0;
    case WM_TIMER:
        if (w == T_BURST && --g_burst <= 0) KillTimer(h, T_BURST);
        scan();
        return 0;
    case WM_HOTKEY:
        if (w == HK_IDEA) ask_idea();
        else if (w == HK_EPHEMERAL) ask_ephemeral();
        else if (w == HK_TILE) tile(-1);
        else if (w > HK_FOCUS && w <= HK_FOCUS + 9) focus_nth((int)(w - HK_FOCUS));
        else if (w >= HK_GROUP && w < HK_GROUP + MAX_GROUPS && (int)(w - HK_GROUP) < g_ngroups) open_group((int)(w - HK_GROUP));
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, path, MAX_PATH) && (GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY))
            launch(path, NULL);
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int i = hit(p);
        if (i != g_hot) { g_hot = i; InvalidateRect(h, NULL, FALSE); }
        if (!g_tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE | (g_expanded ? 0 : TME_HOVER), h, HOVER_MS };
            TrackMouseEvent(&t);
            g_tracking = TRUE;
        }
        return 0;
    }
    case WM_MOUSEHOVER:
        set_expanded(TRUE);
        return 0;
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        g_hot = -1;
        if (!g_menu) set_expanded(FALSE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        click(hit(p), (w & MK_CONTROL) != 0);
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        rclick(hit(p));
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c;
        GetClientRect(h, &c);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: {
        APPBARDATA ab = { .cbSize = sizeof ab, .hWnd = h };
        SHAppBarMessage(ABM_REMOVE, &ab);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, w, l);
}

// We briefly attach to agents' consoles; never let their Ctrl+C/close events reach us.
static BOOL WINAPI ignore_ctrl(DWORD type) { (void)type; return TRUE; }

static void say(const wchar_t *s) {                  // CLI output to the parent console, if any
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    DWORD w;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), L"\n", 1, &w, NULL);
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)wcslen(s), &w, NULL);
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), L"\n", 1, &w, NULL);
    FreeConsole();
}

static int cli(int argc, wchar_t **argv) {
    BOOL grp = argc >= 4 && !_wcsicmp(argv[1], L"group"), ungrp = argc >= 3 && !_wcsicmp(argv[1], L"ungroup");
    if (grp || ungrp) {
        wchar_t full[MAX_PATH], msg[MAX_PATH + 96];
        const wchar_t *arg = argv[grp ? 3 : 2];
        DWORD attr;
        if (!GetFullPathNameW(arg, MAX_PATH, full, NULL) ||
            (attr = GetFileAttributesW(full)) == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            say(L"agentbar: not a folder");
            return 1;
        }
        size_t l = wcslen(full);
        if (l > 3 && full[l - 1] == L'\\') full[l - 1] = 0;
        write_default_ini();
        BOOL ok = TRUE;
        if (grp) ok = add_to_group(argv[2], full);
        else ungroup(full);
        if (!ok) swprintf(msg, MAX_PATH + 96, L"agentbar: could not write %ls", g_ini);
        else if (grp) swprintf(msg, MAX_PATH + 96, L"agentbar: [group:%ls] <- %ls", argv[2], full);
        else swprintf(msg, MAX_PATH + 96, L"agentbar: ungrouped %ls", full);
        say(msg);
        return ok ? 0 : 1;
    }
    say(L"usage: agentbar.exe                        run the bar\n"
        L"       agentbar.exe group <name> <folder>   put a folder in a group (moving it out of any other)\n"
        L"       agentbar.exe ungroup <folder>        remove a folder from all groups");
    return 1;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline; (void)show;
    GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    *(wchar_t *)base_name(g_ini) = 0;
    wcscpy(g_state, g_ini);
    wcscat(g_ini, L"agentbar.ini");
    wcscat(g_state, L"agentbar.state.ini");

    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) return cli(argc, argv);

    CreateMutexW(NULL, TRUE, L"agentbar.single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(ignore_ctrl, TRUE);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ExpandEnvironmentStringsW(L"%USERPROFILE%\\.omp\\agent", g_omp_dir, MAX_PATH);
    write_default_ini();
    load_config();

    WNDCLASSW wc = { .lpfnWndProc = wndproc, .hInstance = hi,
                     .hCursor = LoadCursorW(NULL, IDC_ARROW), .lpszClassName = L"agentbar" };
    RegisterClassW(&wc);
    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_ACCEPTFILES, L"agentbar", L"agentbar", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, hi, NULL);
    g_dpi = GetDpiForWindow(g_bar);
    make_fonts();
    style_window(g_bar);
    dark_menus();
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    APPBARDATA ab = { .cbSize = sizeof ab, .hWnd = g_bar, .uCallbackMessage = WM_APPBAR_CB };
    SHAppBarMessage(ABM_NEW, &ab);
    register_hotkeys();
    set_pos();
    scan();
    build_items();
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    SetCoalescableTimer(g_bar, T_SCAN, SCAN_MS, NULL, SCAN_MS / 2);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
