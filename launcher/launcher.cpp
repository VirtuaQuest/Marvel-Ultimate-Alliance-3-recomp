// MUA3 Recomp launcher: sets up the game files (base game, update, Expansion Pass, keys), runs the extraction,
// recompilation and build, edits the display / sound settings and the controls, and starts the game.
//
// Files (in the data folder: the folder with extracted/ or the Makefile - the launcher's own folder, or one up from it):
//   larecomp.ini   the game's settings; only the launcher's keys are edited, every other line is kept as it is (the game
//                  reads the same keys: runtime/settings.c; the controls: bind_key_* / bind_pad_*, runtime/hle/bindings.c)
//   launcher.ini   the launcher's own: the game file paths, the MSYS2 folder
// Steps run as child processes (their output in the log panel; a job object ends them all on Cancel):
//   Python environment  py -3.12 -m venv .venv; pip install the recompiler's modules (README)
//   Extract             .venv\Scripts\python -I tools\nsp_extract.py <base> <keys> extracted --update <update> [--dlc ..]
//   Recompile           make gen                (MSYS2 UCRT64: clang, make)
//   Build               make -jN build/mua3recomp.exe build/mua3game.dll build/.dlls
// In the release folder (make dist: mua3gen, zig, gamelib-src next to the launcher) the same steps need nothing
// installed: mua3gen extract / mua3gen gen, then the game code compiled by the bundled zig (build_gamelib), all in
// the launcher's own folder. Command lines (RecompLauncher, recomplauncher.json):
//   --prepare --game=<xci|nsp> --update=<nsp> [--dlc=<nsp>] --keys=<prod.keys>
//              no window: the INSTALL steps, "progress <0..1> <text>" lines on stdout, the log in prepare.log
//              (--base= is the same as --game=; each option also as two arguments: --keys <prod.keys>)
//   --play     the game, waited for; the window only opens when the game code must be built again first
// Without these the launcher opens its window as before.
// Tests: MUA3_LAUNCHER_SHOT=<file.bmp> saves the window after a few frames and quits; MUA3_LAUNCHER_TAB=<n> opens that
// page first; MUA3_LAUNCHER_PLAY=1 presses Play; MUA3_LAUNCHER_REBIND=k<slot>:<key name>|p<slot>:<button>,... rebinds
// (a click, then that press), then saves and quits.
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define APP "MUA3 Recomp"
#define SETTINGS_HEADER "MUA3 Recomp settings (the launcher, and the in-game menu: F1, Esc, or Minus+Plus on a controller)"

// What the release's setup accepts (tools/nsp_extract.py --title / --build): MARVEL ULTIMATE ALLIANCE 3 4.0.1, the base
// game with its last update (runtime/game_addresses.c)
#define GAME_TITLE_ID "010060700AC50000"
#define GAME_BUILD_ID "e853c44fdf18b88f"
#define GAME_WANT "MARVEL ULTIMATE ALLIANCE 3 4.0.1"

// ================================================================== palette
// The game's own colours, all drawn here (no game art, logos or fonts): a cosmic night - deep indigo to violet, nebula
// glows, stars - and six gem colours, one for each page (red, orange, blue, purple, green; gold for the title).
static const ImVec4 C_GOLD(1.0f, 0.80f, 0.22f, 1), C_RED(0.86f, 0.13f, 0.24f, 1), C_RED_HI(1.0f, 0.27f, 0.36f, 1);
static const ImVec4 C_GREY(0.72f, 0.70f, 0.86f, 1), C_OK(0.30f, 0.88f, 0.55f, 1), C_WARN(1.0f, 0.64f, 0.30f, 1);
static const ImVec4 C_CYAN(0.42f, 0.86f, 1.0f, 1);
static const ImVec4 k_gem[6] = {
    {0.92f, 0.16f, 0.27f, 1}, {1.0f, 0.55f, 0.13f, 1}, {0.22f, 0.52f, 1.0f, 1},   // red, orange, blue
    {0.64f, 0.32f, 1.0f, 1},  {0.20f, 0.82f, 0.46f, 1}, {1.0f, 0.80f, 0.20f, 1},  // purple, green, gold
};
static int g_page;  // (the page shown: its gem colour is the accent)
static ImVec4 accent(float a = 1) { ImVec4 c = k_gem[g_page % 6]; c.w = a; return c; }
static ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// ================================================================== paths
static std::string g_exe_dir, g_root;  // (the launcher's folder; the data folder, both ending in a separator)

static std::string native_path(std::string p) {
#ifndef _WIN32
    std::replace(p.begin(), p.end(), '\\', '/');
#endif
    return p;
}
static FILE *open_native(const char *p, const char *mode) { return fopen(native_path(p).c_str(), mode); }
#define fopen(p, mode) open_native(p, mode)

static bool is_dir(const std::string &p) {
    SDL_PathInfo info;
    std::string n = native_path(p);
    return SDL_GetPathInfo(n.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}
static bool is_file(const std::string &p) {
    SDL_PathInfo info;
    std::string n = native_path(p);
    return SDL_GetPathInfo(n.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}
static std::string full_path(const std::string &p) {
#ifdef _WIN32
    char full[MAX_PATH];
    if (GetFullPathNameA(p.c_str(), sizeof full, full, NULL)) return full;
#endif
    return p;
}
static void find_paths(void) {
    const char *base = SDL_GetBasePath();  // (ends with a separator)
    g_exe_dir = base ? base : "./";
    std::string up = g_exe_dir + "../";
    if (is_dir(g_exe_dir + "extracted") || is_file(g_exe_dir + "Makefile")) g_root = g_exe_dir;
    else if (is_dir(up + "extracted") || is_file(up + "Makefile")) g_root = up;
    else g_root = g_exe_dir;
    g_root = full_path(g_root);  // (build\..\ -> the folder itself)
    if (!g_root.empty() && g_root.back() != '\\' && g_root.back() != '/') g_root += '/';
}

// The release folder (make dist): the extractor / generator (mua3gen), the compiler (zig) and what the game code is
// compiled against (gamelib-src) next to the launcher, so INSTALL needs no MSYS2 or Python. gamelib-src/gamelib.id:
// the id the game (mua3recomp.exe) wants its game code library built with (GAMELIB_ID), then the C flags.
static bool g_release;
static std::string g_gamelib_id, g_gen_flags;
static void find_release(void) {
    g_release = is_file(g_exe_dir + "mua3gen\\mua3gen.exe") && is_file(g_exe_dir + "zig\\zig.exe") &&
                is_file(g_exe_dir + "gamelib-src\\gamelib.id");
    if (!g_release) return;
    if (FILE *f = fopen((g_exe_dir + "gamelib-src\\gamelib.id").c_str(), "r")) {
        char a[256] = {0}, b[1024] = {0};
        if (fgets(a, sizeof a, f)) g_gamelib_id = a;
        if (fgets(b, sizeof b, f)) g_gen_flags = b;
        fclose(f);
        for (std::string *s : {&g_gamelib_id, &g_gen_flags})
            while (!s->empty() && (s->back() == '\n' || s->back() == '\r' || s->back() == ' ')) s->pop_back();
    }
    if (g_gamelib_id.empty()) g_release = false;
}
// The id a game code library built from these game files has (runtime/gamelib.h gamelib_full_id): GAMELIB_ID-<the
// first 8 bytes of exefs/main's build ID, in hex>. "" when exefs/main is unreadable.
static std::string gamelib_full_id(void) {
    FILE *f = fopen((g_root + "extracted\\exefs\\main").c_str(), "rb");
    if (!f) return "";
    unsigned char b[8];
    bool ok = fseek(f, 64, SEEK_SET) == 0 && fread(b, 1, 8, f) == 8;
    fclose(f);
    if (!ok) return "";
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + i * 2, 3, "%02X", b[i]);
    return g_gamelib_id + "-" + hex;
}
static bool gamelib_current(void) {  // mua3game.dll is there and was built for these game files and this release
    std::string want = gamelib_full_id(), have;
#ifdef __linux__
    const char *name = "mua3game.so";
#else
    const char *name = "mua3game.dll";
#endif
    if (want.empty() || !is_file(g_root + name)) return false;
    if (FILE *f = fopen((g_root + name + std::string(".id")).c_str(), "r")) {
        char id[256] = {0};
        if (fgets(id, sizeof id, f)) have = id;
        fclose(f);
    }
    while (!have.empty() && (have.back() == '\n' || have.back() == '\r')) have.pop_back();
    return have == want;
}

// ================================================================== ini files (lines kept; get / set by key)
struct Ini {
    std::string path;
    std::vector<std::string> lines;
    static std::string trim(const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
    static bool split(const std::string &line, std::string *k, std::string *v) {
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) return false;
        *k = trim(line.substr(0, eq)), *v = trim(line.substr(eq + 1));
        return true;
    }
    void load(const std::string &p) {
        path = p;
        lines.clear();
        FILE *f = fopen(p.c_str(), "r");
        if (!f) return;
        char buf[4096];
        while (fgets(buf, sizeof buf, f)) {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    std::string get(const char *key, const char *def) const {
        std::string k, v;
        for (auto &l : lines) if (split(l, &k, &v) && k == key) return v;
        return def;
    }
    void set(const char *key, const std::string &val) {
        std::string k, v;
        for (auto &l : lines) if (split(l, &k, &v) && k == key) { l = std::string(key) + " = " + val; return; }
        lines.push_back(std::string(key) + " = " + val);
    }
    void remove_prefix(const char *prefix) {
        std::string k, v;
        lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string &l) {
            return split(l, &k, &v) && k.rfind(prefix, 0) == 0;
        }), lines.end());
    }
    bool save(const char *header) const {
        FILE *f = fopen(path.c_str(), "w");
        if (!f) return false;
        if (lines.empty() || lines[0].rfind("#", 0) != 0) fprintf(f, "# %s\n", header);
        for (auto &l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
        return true;
    }
};
static Ini g_settings_ini, g_launcher_ini;

// ================================================================== the display and sound settings (larecomp.ini)
struct Choice { const char *label, *value; };
static const Choice k_render[] = {{"Auto (this GPU's default)", "auto"}, {"Match display", "match"}, {"1x (1080p)", "1x"},
                                  {"2x (4K)", "2x"}, {"3x", "3x"}, {"4x", "4x"}, {"Dynamic", "dynamic"},
                                  {"75% of the display", "75%"}, {"50% of the display", "50%"}, {"25% of the display", "25%"}};
static const Choice k_upscaler[] = {{"Auto", "auto"}, {"AMD FSR 1 (sharp)", "fsr1"}, {"Bilinear", "bilinear"}};
static const Choice k_aa[] = {{"Off", "off"}, {"SMAA", "smaa"}};
static const Choice k_aspect[] = {{"Unlocked (fill the window)", "unlocked"}, {"Original 16:9", "original"}};
static int choice_index(const Choice *c, int n, const std::string &v) {
    for (int i = 0; i < n; i++) if (v == c[i].value) return i;
    return 0;
}
static bool choice_combo(const char *label, int *i, const Choice *c, int n) {
    bool changed = false;
    if (ImGui::BeginCombo(label, c[*i].label)) {
        for (int k = 0; k < n; k++)
            if (ImGui::Selectable(c[k].label, k == *i)) *i = k, changed = true;
        ImGui::EndCombo();
    }
    return changed;
}

struct Mode { int w, h; float hz; };
static std::vector<Mode> g_modes;  // (the primary display's sizes, largest first, each at its highest refresh rate)
static Mode g_desktop = {1920, 1080, 60};

static void list_modes(void) {
    SDL_DisplayID d = SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(d)) g_desktop = {m->w, m->h, m->refresh_rate};
    int n = 0;
    SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(d, &n);
    for (int i = 0; modes && i < n; i++) {
        auto it = std::find_if(g_modes.begin(), g_modes.end(), [&](const Mode &x) { return x.w == modes[i]->w && x.h == modes[i]->h; });
        if (it == g_modes.end()) g_modes.push_back({modes[i]->w, modes[i]->h, modes[i]->refresh_rate});
        else it->hz = std::max(it->hz, modes[i]->refresh_rate);
    }
    SDL_free(modes);
    std::sort(g_modes.begin(), g_modes.end(), [](const Mode &a, const Mode &b) { return a.w * a.h > b.w * b.h; });
    if (g_modes.empty()) g_modes.push_back(g_desktop);
}

static int s_mode;            // 0 windowed, 1 borderless, 2 exclusive
static int s_size;            // index into g_modes, -1: default (windowed 1280x720 / the desktop's)
static int s_render, s_upscaler, s_aa, s_aspect, s_fps, s_rumble_pct;
static bool s_show_fps, s_rumble, s_bg_input, s_movie_audio;
static float s_volume = 1.0f;

// ================================================================== the controls (larecomp.ini bind_key_* / bind_pad_*)
// The same tables as the game's (runtime/hle/bindings.c, runtime/input_bind.h): the Switch inputs in nn::hid button
// order, then the keyboard's stick directions; the defaults; the names larecomp.ini uses. Only bindings that differ
// from the defaults are written, as the game does, so the two always agree.
enum { N_PAD = 16, N_KEY = 24, PADB_LTRIGGER = 26 /* SDL_GAMEPAD_BUTTON_COUNT */, PADB_RTRIGGER, N_HOST = 28 };
static_assert(PADB_LTRIGGER == SDL_GAMEPAD_BUTTON_COUNT, "trigger pseudo-buttons follow SDL's buttons");
static const char *const k_bind_ini[N_KEY] = {
    "a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus", "left", "up", "right", "down",
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right",
};
static const char *const k_bind_name[N_KEY] = {
    "A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "Plus (+)", "Minus (-)",
    "D-pad Left", "D-pad Up", "D-pad Right", "D-pad Down", "Left stick Up", "Left stick Down", "Left stick Left",
    "Left stick Right", "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
};
static const int16_t k_def_pad[N_PAD] = {
    SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PADB_LTRIGGER, PADB_RTRIGGER, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN};
static const int16_t k_def_key[N_KEY] = {
    SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_I, SDL_SCANCODE_F, SDL_SCANCODE_KP_0,
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_1, SDL_SCANCODE_3, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6};
static const char *const k_pad_ini[N_HOST] = {  // (controller buttons by position)
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger",
};
static const char *pad_label(int host, int fam) {  // fam: 0 Nintendo, 1 Xbox (and pads SDL labels like one), 2 PlayStation
    static const char *const nin[N_HOST] = {
        "B", "A", "Y", "X", "Minus", "Home", "Plus", "Left stick press", "Right stick press", "L", "R",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Capture", "Right paddle 1", "Left paddle 1",
        "Right paddle 2", "Left paddle 2", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "ZL", "ZR"};
    static const char *const xb[N_HOST] = {
        "A", "B", "X", "Y", "View", "Xbox", "Menu", "LS (press)", "RS (press)", "LB", "RB",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Share", "Paddle P1", "Paddle P3",
        "Paddle P2", "Paddle P4", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT", "RT"};
    static const char *const ps[N_HOST] = {
        "Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1",
        "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Mute", "Right back button", "Left back button",
        "Right Fn", "Left Fn", "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "L2", "R2"};
    if (host < 0 || host >= N_HOST) return "(none)";
    return fam == 2 ? ps[host] : fam == 0 ? nin[host] : xb[host];
}
static const char *key_label(int sc) {
    if (sc <= 0 || sc >= SDL_SCANCODE_COUNT) return "(none)";
    const char *n = SDL_GetScancodeName((SDL_Scancode)sc);
    return n && *n ? n : "(unnamed key)";
}
static int16_t g_pad[N_PAD], g_key[N_KEY];
enum { DEV_PAD, DEV_KEY };
static int g_cap_dev = -1, g_cap_slot = -1;  // (a rebind waiting for its press)
static uint64_t g_cap_deadline;
static bool g_nav_hold;                     // (controller navigation off until the captured button is let go)
static std::string g_cap_result;

static void bindings_from_ini(void) {
    memcpy(g_pad, k_def_pad, sizeof g_pad);
    memcpy(g_key, k_def_key, sizeof g_key);
    std::string k, v;
    for (auto &l : g_settings_ini.lines) {  // (as runtime/hle/bindings.c bind_settings_load)
        if (!Ini::split(l, &k, &v)) continue;
        bool pad = !k.compare(0, 9, "bind_pad_"), kb = !k.compare(0, 9, "bind_key_");
        if (!pad && !kb) continue;
        for (int i = 0; i < (pad ? N_PAD : N_KEY); i++) {
            if (k.compare(9, std::string::npos, k_bind_ini[i])) continue;
            if (pad) {
                for (int h = 0; h < N_HOST; h++) if (v == k_pad_ini[h]) g_pad[i] = (int16_t)h;
            } else {
                SDL_Scancode sc = v != "none" ? SDL_GetScancodeFromName(v.c_str()) : SDL_SCANCODE_UNKNOWN;
                if (sc != SDL_SCANCODE_UNKNOWN || v == "none") g_key[i] = (int16_t)sc;
            }
        }
    }
}
static void bindings_to_ini(void) {  // (as bind_settings_save: only the ones that differ from the defaults)
    Ini &I = g_settings_ini;
    I.remove_prefix("bind_pad_");
    I.remove_prefix("bind_key_");
    for (int i = 0; i < N_PAD; i++)
        if (g_pad[i] != k_def_pad[i] && g_pad[i] >= 0 && g_pad[i] < N_HOST)
            I.lines.push_back(std::string("bind_pad_") + k_bind_ini[i] + " = " + k_pad_ini[g_pad[i]]);
    for (int i = 0; i < N_KEY; i++)
        if (g_key[i] != k_def_key[i])
            I.lines.push_back(std::string("bind_key_") + k_bind_ini[i] + " = " +
                              (g_key[i] ? SDL_GetScancodeName((SDL_Scancode)g_key[i]) : "none"));
}
// Bind `host` to `slot`; an input already on `host` takes the slot's old binding (a swap, as in the game's menu)
static void bind_assign(int dev, int slot, int host) {
    int16_t *t = dev == DEV_PAD ? g_pad : g_key;
    int n = dev == DEV_PAD ? N_PAD : N_KEY, other = -1;
    for (int i = 0; i < n; i++)
        if (i != slot && t[i] == host) { other = i; t[i] = t[slot]; }
    t[slot] = (int16_t)host;
    g_cap_result = std::string(k_bind_name[slot]) + " set" +
                   (other >= 0 ? std::string(" (swapped with ") + k_bind_name[other] + ")" : std::string("")) + ".";
}
static void capture_begin(int dev, int slot) {
    g_cap_dev = dev, g_cap_slot = slot;
    g_cap_deadline = SDL_GetTicks() + 5000;
    g_cap_result.clear();
}
// While a rebind waits: the next key / controller button is taken (Esc or a click cancels). true = the event is used.
static bool capture_event(const SDL_Event &e) {
    if (g_cap_dev < 0) return false;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) return true;
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) { g_cap_dev = -1; g_cap_result = "Cancelled."; return true; }
        // F1 (the game's menu), F9 (frame dump), F11 (fullscreen) stay the game's; a controller rebind ignores keys
        if (g_cap_dev != DEV_KEY || e.key.scancode == SDL_SCANCODE_F1 || e.key.scancode == SDL_SCANCODE_F9 ||
            e.key.scancode == SDL_SCANCODE_F11 || e.key.scancode >= SDL_SCANCODE_COUNT) return true;
        bind_assign(DEV_KEY, g_cap_slot, e.key.scancode);
        g_cap_dev = -1;
        return true;
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        // Home / Guide opens the game's menu; a keyboard rebind ignores the controller (Esc, or it times out)
        if (g_cap_dev != DEV_PAD || e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e.gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT)
            return true;
        bind_assign(DEV_PAD, g_cap_slot, e.gbutton.button);
        g_cap_dev = -1;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (g_cap_dev == DEV_PAD && e.gaxis.value > 16000 &&
            (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
            bind_assign(DEV_PAD, g_cap_slot, e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LTRIGGER : PADB_RTRIGGER);
            g_cap_dev = -1;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        g_cap_dev = -1;
        g_cap_result = "Cancelled.";
        return true;
    default:
        return false;
    }
}

// The connected controllers (opened so their presses arrive), and their family for the button names
static std::vector<SDL_Gamepad *> g_gamepads;
static int pad_family(void) {
    for (SDL_Gamepad *p : g_gamepads) {
        switch (SDL_GetGamepadType(p)) {
        case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 2;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 0;
        default: return 1;
        }
    }
    return 1;  // (none: Xbox names, the most common on PC)
}
static bool pad_input_held(void) {
    for (SDL_Gamepad *p : g_gamepads) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(p, (SDL_GamepadButton)b)) return true;
        if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000 || SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000)
            return true;
    }
    return false;
}

// ================================================================== reading / writing all the launcher's settings
static void settings_from_ini(void) {
    const Ini &I = g_settings_ini;
    bool fs = atoi(I.get("fullscreen", "0").c_str()) != 0, ex = atoi(I.get("exclusive_fullscreen", "0").c_str()) != 0;
    s_mode = !fs ? 0 : ex ? 2 : 1;
    int w = atoi(I.get("window_width", "0").c_str()), h = atoi(I.get("window_height", "0").c_str());
    s_size = -1;
    for (size_t i = 0; i < g_modes.size(); i++) if (g_modes[i].w == w && g_modes[i].h == h) s_size = (int)i;
    s_render = choice_index(k_render, IM_ARRAYSIZE(k_render), I.get("resolution", "auto"));
    s_upscaler = choice_index(k_upscaler, IM_ARRAYSIZE(k_upscaler), I.get("upscaler", "auto"));
    s_aa = choice_index(k_aa, IM_ARRAYSIZE(k_aa), I.get("antialias", "off"));
    s_aspect = choice_index(k_aspect, IM_ARRAYSIZE(k_aspect), I.get("aspect", "unlocked"));
    int fps = atoi(I.get("frame_rate", atoi(I.get("fps60", "0").c_str()) ? "60" : "30").c_str());
    s_fps = fps >= 120 ? 2 : fps >= 60 ? 1 : 0;
    s_show_fps = atoi(I.get("show_fps", "0").c_str()) != 0;
    s_volume = std::clamp((float)atof(I.get("volume", "1.00").c_str()), 0.0f, 2.0f);
    s_movie_audio = atoi(I.get("movie_audio", "1").c_str()) != 0;
    s_rumble = atoi(I.get("rumble", "1").c_str()) != 0;
    s_rumble_pct = atoi(I.get("rumble_strength", "100").c_str());
    if (s_rumble_pct < 25 || s_rumble_pct > 100) s_rumble_pct = 100;
    s_bg_input = atoi(I.get("background_input", "1").c_str()) != 0;  // (the game's default on Windows: on)
    bindings_from_ini();
}

static bool settings_to_ini(void) {
    Ini &I = g_settings_ini;
    I.set("fullscreen", s_mode ? "1" : "0");
    I.set("exclusive_fullscreen", s_mode == 2 ? "1" : "0");
    I.set("window_width", s_size >= 0 ? std::to_string(g_modes[s_size].w) : "0");
    I.set("window_height", s_size >= 0 ? std::to_string(g_modes[s_size].h) : "0");
    I.set("resolution", k_render[s_render].value);
    I.set("upscaler", k_upscaler[s_upscaler].value);
    I.set("antialias", k_aa[s_aa].value);
    I.set("aspect", k_aspect[s_aspect].value);
    I.set("frame_rate", s_fps == 2 ? "120" : s_fps == 1 ? "60" : "30");
    I.set("show_fps", s_show_fps ? "1" : "0");
    char v[32];
    snprintf(v, sizeof v, "%.2f", s_volume);
    I.set("volume", v);
    I.set("movie_audio", s_movie_audio ? "1" : "0");
    I.set("rumble", s_rumble ? "1" : "0");
    I.set("rumble_strength", std::to_string(s_rumble_pct));
    I.set("background_input", s_bg_input ? "1" : "0");
    bindings_to_ini();
    return I.save(SETTINGS_HEADER);
}

// ================================================================== game files and toolchain
static char p_base[1024], p_update[1024], p_dlc[1024], p_keys[1024], p_msys[512] = "C:\\msys64";
static std::mutex g_dialog_mu;
static std::string g_dialog_result;
static int g_dialog_target = -1;  // (which field the open file dialog fills)
static char *const g_path_fields[] = {p_base, p_update, p_dlc, p_keys};

static void paths_from_ini(void) {
    snprintf(p_base, sizeof p_base, "%s", g_launcher_ini.get("base_game", "").c_str());
    snprintf(p_update, sizeof p_update, "%s", g_launcher_ini.get("update", "").c_str());
    snprintf(p_dlc, sizeof p_dlc, "%s", g_launcher_ini.get("dlc", "").c_str());
    snprintf(p_keys, sizeof p_keys, "%s", g_launcher_ini.get("keys", "").c_str());
    snprintf(p_msys, sizeof p_msys, "%s", g_launcher_ini.get("msys2", "C:\\msys64").c_str());
}
static void paths_to_ini(void) {
    g_launcher_ini.set("base_game", p_base);
    g_launcher_ini.set("update", p_update);
    g_launcher_ini.set("dlc", p_dlc);
    g_launcher_ini.set("keys", p_keys);
    g_launcher_ini.set("msys2", p_msys);
    g_launcher_ini.save(APP " launcher: your game files (never shared) and tools");
}

static void SDLCALL dialog_done(void *userdata, const char *const *files, int filter) {
    (void)filter;
    std::lock_guard<std::mutex> lk(g_dialog_mu);
    if (files && files[0]) { g_dialog_result = files[0]; g_dialog_target = (int)(intptr_t)userdata; }
}
static void browse(int target, SDL_Window *win) {
    static const SDL_DialogFileFilter game[] = {{"Switch game (NSP, XCI)", "nsp;xci"}, {"All files", "*"}};
    static const SDL_DialogFileFilter nsp[] = {{"Switch update / add-on (NSP)", "nsp"}, {"All files", "*"}};
    static const SDL_DialogFileFilter keys[] = {{"Switch keys (prod.keys)", "keys;txt"}, {"All files", "*"}};
    const SDL_DialogFileFilter *f = target == 0 ? game : target == 3 ? keys : nsp;
    SDL_ShowOpenFileDialog(dialog_done, (void *)(intptr_t)target, win, f, 2, NULL, false);
}

struct Status { bool venv, msys, extracted, built, nsps_there; std::string version, missing_nsp; };
static Status g_status;
static std::string nacp_version(void) {  // (control.nacp: DisplayVersion at 0x3060, 16 bytes)
    FILE *f = fopen((g_root + "extracted\\control\\control.nacp").c_str(), "rb");
    if (!f) return "";
    char v[17] = {0};
    if (fseek(f, 0x3060, SEEK_SET) == 0) (void)!fread(v, 1, 16, f);
    fclose(f);
    return v;
}
// The game data is read in place from the NSPs (extracted\romfs.src names them): are they still where they were?
static void check_nsps(void) {
    g_status.nsps_there = true;
    g_status.missing_nsp.clear();
    FILE *f = fopen((g_root + "extracted\\romfs.src").c_str(), "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "section ", 8)) continue;
        const char *p = line;
        for (int field = 0; field < 7 && p; field++) p = strchr(p + 1, ' ');  // (the NSP's path: after 7 fields)
        if (!p) continue;
        std::string nsp = Ini::trim(p);
        if (!nsp.empty() && !is_file(nsp)) { g_status.nsps_there = false; g_status.missing_nsp = nsp; break; }
    }
    fclose(f);
}
static void refresh_status(void) {
    g_status.extracted = is_file(g_root + "extracted\\exefs\\main") && is_file(g_root + "extracted\\romfs.src");
    g_status.version = nacp_version();
    check_nsps();
    if (g_release) {  // (the tools are in the release; the generated C isn't kept once it's built)
        g_status.venv = g_status.msys = true;
        g_status.built = is_file(g_exe_dir + "mua3recomp.exe") && gamelib_current();
        return;
    }
#ifdef __linux__
    g_status.venv = is_file(g_root + ".venv-linux/bin/python");
    g_status.msys = true;  // native dependencies were present when the launcher was built
    g_status.built = is_file(g_root + "build/mua3recomp") && is_file(g_root + "build/mua3game.so");
#else
    g_status.venv = is_file(g_root + ".venv\\Scripts\\python.exe");
    g_status.msys = is_file(std::string(p_msys) + "\\usr\\bin\\make.exe") && is_file(std::string(p_msys) + "\\ucrt64\\bin\\clang.exe");
    g_status.built = is_file(g_root + "build\\mua3recomp.exe") && is_file(g_root + "build\\mua3game.dll");
#endif
}

// ================================================================== the step runner (child processes, a log)
enum { ST_WAIT, ST_RUN, ST_OK, ST_FAIL, ST_SKIP };
enum { K_PROCESS, K_BUILD_GAMELIB };  // (a command line; the release's own compile + link of the game code)
struct Step {
    std::string title, cmd, cwd;
    bool msys;
    int kind = K_PROCESS;
    double weight = 1;  // (its share of the whole run, for the progress the prepare step prints)
    int state = ST_WAIT;
    double secs = 0;
    std::string why;  // (when it failed: its last line of output, which says why)
};
static std::vector<Step> g_steps;
static std::atomic<double> g_step_frac{0};
static size_t g_step_index;
static std::mutex g_log_mu;
static std::vector<std::string> g_log;
static std::atomic<bool> g_running{false}, g_cancel{false};
static std::atomic<int> g_result{0};  // 0 none, 1 done, 2 failed, 3 cancelled
static std::thread g_worker;
static bool g_cli;            // mua3launcher --prepare: no window; progress lines on stdout (RecompLauncher's prepare)
static FILE *g_cli_log;       // (and the whole log in prepare.log)
#ifdef _WIN32
static HANDLE g_job;
#endif

static void cli_out(const std::string &s) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);  // (a GUI-subsystem program: the handle RecompLauncher gave it)
    DWORD n;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, s.data(), (DWORD)s.size(), &n, NULL);
#else
    fputs(s.c_str(), stdout), fflush(stdout);
#endif
}
// The whole run's progress (0..1) from the steps' weights and the running step's own fraction, printed as
// "progress <0..1> <text>" in prepare mode (at most every half second, or when the text changes)
static void report_progress(void) {
    if (!g_cli || g_steps.empty()) return;
    static double last_t;
    static std::string last_text;
    double total = 0, done = 0;
    for (size_t i = 0; i < g_steps.size(); i++) {
        if (g_steps[i].state == ST_SKIP) continue;
        total += g_steps[i].weight;
        if (i < g_step_index) done += g_steps[i].weight;
        else if (i == g_step_index) done += g_steps[i].weight * std::clamp(g_step_frac.load(), 0.0, 1.0);
    }
    std::string text = g_step_index < g_steps.size() ? g_steps[g_step_index].title : "Done";
    double now = SDL_GetTicks() / 1000.0;
    if (text == last_text && now - last_t < 0.5) return;
    last_t = now, last_text = text;
    char line[512];
    snprintf(line, sizeof line, "progress %.3f %s\n", total > 0 ? done / total : 0, text.c_str());
    cli_out(line);
}

static void log_line(const std::string &s) {
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log.push_back(s);
        if (g_log.size() > 4000) g_log.erase(g_log.begin(), g_log.begin() + 1000);
        if (g_cli_log) fprintf(g_cli_log, "%s\n", s.c_str()), fflush(g_cli_log);
    }
    // the generator's "<module>: progress <done>/<total>" lines: how far the running step is
    size_t p = s.find(": progress ");
    if (p != std::string::npos) {
        long a = 0, b = 0;
        if (sscanf(s.c_str() + p + 11, "%ld/%ld", &a, &b) == 2 && b > 0) g_step_frac = (double)a / b;
    }
    report_progress();
}

#ifdef _WIN32
static int run_process(const Step &st) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string cmd = st.cmd;
    std::string path_saved;
    if (st.msys) {  // (MSYS2 UCRT64's tools first, as in the README's Git Bash setup)
        char *old = getenv("PATH");
        path_saved = old ? old : "";
        std::string p = std::string(p_msys) + "\\ucrt64\\bin;" + p_msys + "\\usr\\bin;" + path_saved;
        SetEnvironmentVariableA("PATH", p.c_str());
    }
    BOOL ok = CreateProcessA(NULL, cmd.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL,
                             st.cwd.c_str(), &si, &pi);
    if (st.msys) SetEnvironmentVariableA("PATH", path_saved.c_str());
    CloseHandle(wr);
    if (!ok) {
        log_line("Could not start: " + cmd + " (Windows error " + std::to_string(GetLastError()) + ")");
        CloseHandle(rd);
        return -1;
    }
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    std::string part;
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        part.append(buf, n);
        size_t p;
        while ((p = part.find_first_of("\r\n")) != std::string::npos) {
            if (p) log_line(part.substr(0, p));
            part.erase(0, p + 1);
        }
    }
    if (!part.empty()) log_line(part);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
#else
static std::atomic<pid_t> g_active_pid{0};
static int run_process(const Step &st) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { log_line("Could not create a process output pipe."); return -1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        std::string cwd = native_path(st.cwd), command = native_path(st.cmd);
        setpgid(0, 0);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) { close(pipefd[0]); log_line("Could not start the requested step."); return -1; }
    g_active_pid = pid;
    std::string part;
    char buf[4096];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof buf);
        if (n > 0) {
            part.append(buf, (size_t)n);
            size_t p;
            while ((p = part.find_first_of("\r\n")) != std::string::npos) {
                if (p) log_line(part.substr(0, p));
                part.erase(0, p + 1);
            }
            if (g_cancel) kill(-pid, SIGTERM);
        } else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    close(pipefd[0]);
    if (!part.empty()) log_line(part);
    int status = 1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    g_active_pid = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}
#endif

static std::string q(const std::string &s) { return "\"" + native_path(s) + "\""; }

#ifdef _WIN32
// The release's build of the game code library: generated\{game,sdk,glslc}\*.c compiled by the bundled zig, as many
// at a time as the CPU and memory allow, then linked against the game's import library (mua3recomp.dll.a) with zig's
// runtime built ahead of time (zig\rt, tools/zig_slim.sh) - as the Makefile builds build/mua3game.dll with clang.
// Writes mua3game.dll and mua3game.dll.id (the id the game checks) next to the game, then deletes the generated C, the
// objects and zig's cache (about 1.5 GB).
static HANDLE spawn_logged(const std::string &cmd, const std::string &cwd, const std::string &log) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) return NULL;
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi = {};
    std::string c = cmd;
    BOOL ok = CreateProcessA(NULL, c.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd.c_str(),
                             &si, &pi);
    CloseHandle(out);
    if (!ok) return NULL;
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
static std::string file_text(const std::string &p, size_t max) {
    std::string s;
    if (FILE *f = fopen(p.c_str(), "rb")) {
        s.resize(max);
        s.resize(fread(&s[0], 1, max, f));
        fclose(f);
    }
    return s;
}
static void remove_tree(const std::string &dir) {  // (a folder this launcher made: generated\, gamebuild\)
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &d);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = d.cFileName, p = dir + "\\" + n;
            if (n == "." || n == "..") continue;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryA(p.c_str()) || DeleteFileA(p.c_str());
            else if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileA(p.c_str());
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}
static int build_gamelib(void) {
    const std::string work = g_root + "gamebuild\\", objdir = work + "obj\\";
    CreateDirectoryA(work.c_str(), NULL);
    CreateDirectoryA(objdir.c_str(), NULL);
    std::string full = gamelib_full_id();
    if (full.empty()) { log_line("extracted\\exefs\\main is missing: extract the game first."); return 1; }
    const std::string zig = q(g_exe_dir + "zig\\zig.exe");
    std::string flags;  // (gamelib.id's flags; zig names CPUs its own way: -march=x86-64-v3 is -mcpu=x86_64_v3)
    for (size_t a = 0; a < g_gen_flags.size();) {
        size_t b = g_gen_flags.find(' ', a);
        if (b == std::string::npos) b = g_gen_flags.size();
        std::string f = g_gen_flags.substr(a, b - a);
        a = b + 1;
        if (f.empty()) continue;
        if (f.rfind("-march=", 0) == 0) {
            f = "-mcpu=" + f.substr(7);
            std::replace(f.begin() + 6, f.end(), '-', '_');
        }
        flags += " " + f;
    }
    flags += " -g0 " + q("-I" + g_exe_dir + "gamelib-src");  // (-g0: zig adds debug info by default, 3x the objects)
    std::string zc = work + "zig-cache";
    SetEnvironmentVariableA("ZIG_GLOBAL_CACHE_DIR", zc.c_str());
    SetEnvironmentVariableA("ZIG_LOCAL_CACHE_DIR", zc.c_str());

    struct Unit { std::string src, obj, log; unsigned long long bytes; bool entry; };
    std::vector<Unit> units;
    for (const char *m : {"game", "sdk", "glslc"}) {
        WIN32_FIND_DATAA d;
        std::string dir = g_root + "generated\\" + m + "\\";
        HANDLE h = FindFirstFileA((dir + "*.c").c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string n = d.cFileName, stem = std::string(m) + "_" + n.substr(0, n.size() - 2);
            units.push_back({dir + n, objdir + stem + ".o", objdir + stem + ".log",
                             ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow, false});
        } while (FindNextFileA(h, &d));
        FindClose(h);
    }
    if (units.empty()) { log_line("No generated C in generated\\: recompile first."); return 1; }
    std::sort(units.begin(), units.end(), [](const Unit &a, const Unit &b) { return a.bytes > b.bytes; });  // (biggest first)
    units.push_back({g_exe_dir + "gamelib-src\\gamelib_entry.c", objdir + "entry.o", objdir + "entry.log", 0, true});

    int jobs = std::max(1, SDL_GetNumLogicalCPUCores());
    MEMORYSTATUSEX ms = {sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {  // (about 1.5 GB for each compiler on the biggest files)
        int by_mem = (int)((ms.ullTotalPhys / (1024.0 * 1024 * 1024) - 3) / 1.5);
        jobs = std::min(jobs, std::max(1, by_mem));
    }
    jobs = std::min(jobs, 32);
    log_line("Compiling " + std::to_string(units.size()) + " files with zig, " + std::to_string(jobs) + " at a time");
    std::vector<std::pair<HANDLE, size_t>> run;
    size_t next = 0, done = 0;
    bool failed = false;
    while (!g_cancel && (run.size() || (!failed && next < units.size()))) {
        while (!failed && (int)run.size() < jobs && next < units.size()) {
            const Unit &u = units[next];
            std::string cmd = zig + " cc" + flags;
            if (u.entry) cmd += " \"-DGAMELIB_ID=\\\"" + full + "\\\"\"";
            cmd += " -c " + q(u.src) + " -o " + q(u.obj);
            HANDLE h = spawn_logged(cmd, g_root, u.log);
            if (!h) {
                log_line("Couldn't start the compiler (Windows error " + std::to_string(GetLastError()) + "): " + cmd);
                failed = true;
                break;
            }
            run.push_back({h, next++});
        }
        if (run.empty()) break;
        std::vector<HANDLE> hs;
        for (auto &r : run) hs.push_back(r.first);
        DWORD w = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, 250);
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + hs.size()) {
            size_t k = w - WAIT_OBJECT_0, idx = run[k].second;
            DWORD code = 1;
            GetExitCodeProcess(run[k].first, &code);
            CloseHandle(run[k].first);
            run.erase(run.begin() + k);
            if (code != 0) {
                failed = true;
                log_line("Compiling " + units[idx].src + " failed (exit code " + std::to_string(code) + "):");
                std::string t = file_text(units[idx].log, 4000);
                for (size_t a = 0; a < t.size();) {
                    size_t b = t.find('\n', a);
                    if (b == std::string::npos) b = t.size();
                    if (b > a) log_line("  " + t.substr(a, b - a - (t[b - 1] == '\r')));
                    a = b + 1;
                }
            } else {
                DeleteFileA(units[idx].log.c_str());
                if (++done % 25 == 0) log_line("  compiled " + std::to_string(done) + " of " + std::to_string(units.size()));
            }
            g_step_frac = 0.92 * done / units.size();
            report_progress();
        }
    }
    for (auto &r : run) CloseHandle(r.first);  // (cancelled: the job object ends them)
    if (g_cancel || failed) return 1;

    log_line("Linking mua3game.dll");
    if (FILE *f = fopen((work + "objs.rsp").c_str(), "wb")) {  // (forward slashes: backslashes are escapes in it)
        for (auto &u : units) {
            std::string o = u.obj;
            std::replace(o.begin(), o.end(), '\\', '/');
            fprintf(f, "\"%s\"\n", o.c_str());
        }
        fclose(f);
    }
    const std::string dll = g_root + "mua3game.dll", tmp = dll + ".tmp";
    Step link = {"Link", zig + " lld-link " + q("@" + g_exe_dir + "zig\\rt\\link.rsp") + " " +
                         q("-LIBPATH:" + g_exe_dir + "zig\\rt") + " " + q("-OUT:" + tmp) + " " + q("@" + work + "objs.rsp") +
                         " " + q(g_exe_dir + "mua3recomp.dll.a"), g_root, false};
    int code = run_process(link);
    if (code != 0) { log_line("Linking failed (exit code " + std::to_string(code) + ")."); return code ? code : 1; }
    DeleteFileA((dll + ".id").c_str());
    if (!MoveFileExA(tmp.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        log_line("Couldn't replace mua3game.dll (Windows error " + std::to_string(GetLastError()) +
                 "): close the game if it's running, then try again.");
        return 1;
    }
    if (FILE *f = fopen((dll + ".id").c_str(), "w")) fprintf(f, "%s\n", full.c_str()), fclose(f);
    log_line("Built mua3game.dll (" + full + ")");
    remove_tree(g_root + "gamebuild");  // (the objects and zig's cache: of no use once it's linked)
    remove_tree(g_root + "generated");
    g_step_frac = 1;
    return 0;
}
#else
static int build_gamelib(void) { log_line("Building the game code is only implemented on Windows."); return 1; }
#endif

static void worker(void) {
    int result = 1;
    for (size_t i = 0; i < g_steps.size(); i++) {
        Step &st = g_steps[i];
        if (st.state == ST_SKIP) continue;
        if (g_cancel) { st.state = ST_SKIP; continue; }
        g_step_index = i;
        g_step_frac = 0;
        st.state = ST_RUN;
        log_line("");
        log_line("==== " + st.title);
        if (!st.cmd.empty()) log_line("> " + st.cmd);
        uint64_t t0 = SDL_GetTicks();
        int code = st.kind == K_BUILD_GAMELIB ? build_gamelib() : run_process(st);
        st.secs = (SDL_GetTicks() - t0) / 1000.0;
        if (g_cancel) { st.state = ST_FAIL; result = 3; log_line("Cancelled."); break; }
        if (code != 0) {
            st.state = ST_FAIL;
            result = 2;
            {
                std::lock_guard<std::mutex> lk(g_log_mu);
                for (size_t j = g_log.size(); j-- > 0 && g_log[j].rfind("> ", 0) != 0 && g_log[j].rfind("==== ", 0) != 0;)
                    if (!g_log[j].empty()) { st.why = g_log[j]; break; }
            }
            log_line("Failed (exit code " + std::to_string(code) + ").");
            break;
        }
        st.state = ST_OK;
        g_step_frac = 1;
        log_line("Done in " + std::to_string((int)st.secs) + " s.");
    }
    g_step_index = g_steps.size();
    g_result = result;
    g_running = false;
}

// The extraction's arguments after the program: the player's files, and the checks that refuse another game or
// version in one line (tools/nsp_extract.py --title / --build / --want)
static std::string extract_args(void) {
    std::string c = " " + q(p_base) + " " + q(p_keys) + " extracted --update " + q(p_update);
    if (p_dlc[0]) c += " --dlc " + q(p_dlc);
    return c + " --title " GAME_TITLE_ID " --build " GAME_BUILD_ID " --want \"" GAME_WANT "\"";
}

// The release's steps: everything from the folder the launcher is in (make dist)
static void release_steps(bool extract, bool recompile, bool build) {
    std::string gen = q(g_exe_dir + "mua3gen\\mua3gen.exe");
    if (extract) {
        Step s = {"Read your game files", gen + " extract" + extract_args(), g_root, false};
        g_steps.push_back(s);
    }
    if (recompile) {
        Step s = {"Recompile the game (ARM64 to C)", gen + " gen game extracted\\exefs\\main 0x7000000000 generated\\game --simd c",
                  g_root, false};
        s.weight = 2.5;
        g_steps.push_back(s);
        s = {"Recompile the system code", gen + " gen sdk extracted\\exefs\\sdk 0x7006000000 generated\\sdk --hle " +
                                              q(g_exe_dir + "gamelib-src\\hle_symbols.txt") + " --simd c", g_root, false};
        s.weight = 0.5;
        g_steps.push_back(s);
        s = {"Recompile the shader compiler", gen + " gen glslc extracted\\exefs\\subsdk1 0x7007000000 generated\\glslc --simd c "
                                                  "--optnone", g_root, false};  // (the Makefile's gen, the same)
        s.weight = 0.5;
        g_steps.push_back(s);
    }
    if (build) {
        Step s = {"Build the game code (C to x86-64)", "", g_root, false};
        s.kind = K_BUILD_GAMELIB;
        s.weight = 12;
        g_steps.push_back(s);
    }
}

static void start_steps(bool extract, bool recompile, bool build) {
    if (g_running) return;
    if (g_worker.joinable()) g_worker.join();
    if (!g_cli) {
        if (g_cli_log) fclose(g_cli_log);
        g_cli_log = fopen((g_root + "prepare.log").c_str(), "w");
    }
    g_steps.clear();
    g_cancel = false;
    g_result = 0;
    if (g_release) {
        release_steps(extract, recompile, build);
    } else {
#ifdef __linux__
        const std::string py = ".venv-linux/bin/python";
        if (build) g_steps.push_back({"Check native development packages", "sh tools/build_linux.sh --check", g_root, false});
        g_steps.push_back({"Python 3.12 environment", "sh tools/setup_python_linux.sh", g_root, false});
        // Retry modules even when a previous failed install left a venv behind.
        g_steps.push_back({"Update Python installer", q(g_root + py) +
                           " -m pip install --disable-pip-version-check --progress-bar off --upgrade pip setuptools wheel", g_root, false});
        g_steps.push_back({"Python modules", q(g_root + py) +
                           " -m pip install --disable-pip-version-check --progress-bar off --only-binary=:all: -r tools/requirements-linux.txt", g_root, false});
        if (extract) g_steps.push_back({"Read your game files", q(g_root + py) + " -I tools/nsp_extract.py" + extract_args(), g_root, false});
        if (recompile) {
            Step s = {"Recompile (ARM64 to C)", "make gen PY=.venv-linux/bin/python", g_root, false};
            s.weight = 3.5;
            g_steps.push_back(s);
        }
        if (build) {
            g_steps.push_back({"Build the native Linux version", "sh tools/build_linux.sh", g_root, false});
        }
#else
        std::string py = g_root + ".venv\\Scripts\\python.exe";
        if (!g_status.venv) {
            g_steps.push_back({"Python environment", "py -3.12 -m venv .venv", g_root, false});
            g_steps.push_back({"Python modules", q(py) + " -m pip install --disable-pip-version-check unicorn==2.1.4 "
                                                 "capstone==5.0.9 lz4==4.4.5 pycryptodome==3.23.0 pillow==13.0.0", g_root, false});
        }
        if (extract) g_steps.push_back({"Read your game files", q(py) + " -I tools\\nsp_extract.py" + extract_args(), g_root, false});
        std::string make = q(std::string(p_msys) + "\\usr\\bin\\make.exe");
        if (recompile) g_steps.push_back({"Recompile (ARM64 to C)", make + " gen PY=.venv/Scripts/python.exe", g_root, true});
        if (build) {
            int n = SDL_GetNumLogicalCPUCores();
            g_steps.push_back({"Build the PC version", make + " -j" + std::to_string(n > 0 ? n : 8) +
                                                        " PY=.venv/Scripts/python.exe build/mua3recomp.exe build/mua3game.dll build/.dlls",
                               g_root, true});
        }
#endif
    }
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        g_log.clear();
    }
    g_running = true;
    g_worker = std::thread(worker);
}
static void cancel_steps(void) {
    g_cancel = true;
#ifdef _WIN32
    if (g_job) TerminateJobObject(g_job, 1);
#else
    pid_t pid = g_active_pid.load();
    if (pid > 0) kill(-pid, SIGTERM);
#endif
}

// ================================================================== the game
// wait_code: wait for the game to close and give its exit code (mua3launcher --play, as RecompLauncher starts it)
static std::string launch_game(int *wait_code = nullptr) {
#ifdef __linux__
    std::string exe = g_root + "build/mua3recomp";
#else
    std::string exe = g_release ? g_exe_dir + "mua3recomp.exe" : g_root + "build\\mua3recomp.exe";
#endif
    if (!is_file(exe)) exe = g_exe_dir + "mua3recomp.exe";
    if (!is_file(exe)) return "The PC version isn't built yet: set up your game files first (GAME FILES).";
    if (!is_dir(g_root + "extracted")) return "No game files set up yet: choose them on GAME FILES, then INSTALL.";
    if (!g_status.nsps_there)
        return "Your game file moved: " + g_status.missing_nsp + ". The game reads its data from your NSPs where they "
               "were set up: put it back, or set up your game files again (GAME FILES).";
#ifdef _WIN32
    SetEnvironmentVariableA("LARECOMP_DATA_ROOT", g_root.c_str());  // (as tools/run.sh: the data folder)
    {   // the build copies the runtime DLLs next to the game (make dlls); MSYS2's own folder is the fallback
        static bool once;
        if (!once) {
            once = true;
            char *old = getenv("PATH");
            std::string p = g_root + "build;" + std::string(p_msys) + "\\ucrt64\\bin;" + (old ? old : "");
            SetEnvironmentVariableA("PATH", p.c_str());
        }
    }
    std::string cmd = q(exe) + " extracted";
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmd.data(), NULL, NULL, FALSE, 0, NULL, g_root.c_str(), &si, &pi))
        return "Couldn't start the game (Windows error " + std::to_string(GetLastError()) + ").";
    CloseHandle(pi.hThread);
    if (wait_code) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        *wait_code = (int)code;
    }
    CloseHandle(pi.hProcess);
    return "";
#else
    setenv("LARECOMP_DATA_ROOT", native_path(g_root).c_str(), 1);
    pid_t pid = fork();
    if (pid < 0) return "Couldn't start the game process.";
    if (pid == 0) {
        // Own the session so closing the launcher's terminal cannot hang up
        // the game. Keep the real child PID so startup failure is observable.
        if (setsid() < 0) _exit(126);
        std::string cwd = native_path(g_root);
        if (chdir(cwd.c_str()) != 0) _exit(126);
        int out = open("play.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out >= 0) {
            dup2(out, STDOUT_FILENO);
            dup2(out, STDERR_FILENO);
            if (out > STDERR_FILENO) close(out);
        }
        fprintf(stderr, "Starting native MUA3 Recomp (pid %ld)\n", (long)getpid());
        fflush(stderr);
        execl("/bin/sh", "sh", "tools/start_game_linux.sh", "extracted", (char *)NULL);
        perror("Game launch");
        _exit(127);
    }
    if (wait_code) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        *wait_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    } else {
        for (int attempt = 0; attempt < 20; ++attempt) {
            int status = 0;
            pid_t done = waitpid(pid, &status, WNOHANG);
            if (done == pid) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
                return "The game closed during startup (exit " + std::to_string(code) +
                       "). See play.log and lasrecomp.log in the Linux folder.";
            }
            if (done < 0 && errno != EINTR) return "Couldn't monitor game startup: see play.log.";
            SDL_Delay(50);
        }
    }
    return "";
#endif
}

// ================================================================== UI helpers
static float g_scale = 1;
static ImFont *f_body, *f_small, *f_big, *f_head, *f_title;  // (f_head, f_title: the display face, Baloo 2)
static std::string g_flash;
static double g_flash_until;
static void flash(const std::string &s) { g_flash = s; g_flash_until = ImGui::GetTime() + 6; }
static ImU32 U(ImVec4 c, float a = -1) { if (a >= 0) c.w = a; return ImGui::GetColorU32(c); }

// A soft glow: discs from the edge in, each adding a little of the colour
static void glow(ImDrawList *dl, ImVec2 c, float r, ImVec4 col, float alpha) {
    for (int i = 0; i < 16; i++) dl->AddCircleFilled(c, r * (1 - i / 16.0f), U(col, alpha / 16), 48);
}
static uint32_t hash32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }

// The background: a cosmic night - indigo to violet, slow-drifting nebula glows in the gem colours, twinkling stars
static void cosmos(ImDrawList *dl, ImVec2 s, float t) {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), s, IM_COL32(12, 10, 36, 255), IM_COL32(28, 12, 58, 255),
                                IM_COL32(40, 10, 52, 255), IM_COL32(8, 14, 44, 255));
    struct { float x, y, r; int gem; float a, speed; } neb[] = {
        {0.12f, 0.22f, 0.55f, 3, 0.55f, 0.050f}, {0.88f, 0.10f, 0.42f, 0, 0.30f, 0.037f},
        {0.78f, 0.88f, 0.50f, 2, 0.40f, 0.043f}, {0.30f, 0.95f, 0.30f, 4, 0.16f, 0.061f},
        {0.55f, 0.45f, 0.35f, 3, 0.18f, 0.029f},
    };
    float big = std::max(s.x, s.y);
    for (auto &n : neb) {
        ImVec2 c(s.x * (n.x + 0.04f * sinf(t * n.speed * 6.28f)), s.y * (n.y + 0.04f * cosf(t * n.speed * 5.1f)));
        glow(dl, c, big * n.r, k_gem[n.gem], n.a);
    }
    for (uint32_t i = 0; i < 170; i++) {  // stars: placed by a hash, each twinkling at its own pace
        uint32_t h = hash32(i * 2654435761u + 17);
        ImVec2 p(s.x * (h & 0xffff) / 65535.0f, s.y * (h >> 16) / 65535.0f);
        float tw = 0.55f + 0.45f * sinf(t * (0.6f + (h % 7) * 0.25f) + i);
        float r = (i % 23 == 0 ? 1.8f : i % 5 == 0 ? 1.2f : 0.8f) * g_scale;
        dl->AddCircleFilled(p, r, IM_COL32(235, 230, 255, (int)(200 * tw)), 8);
        if (i % 23 == 0) {  // (a few bright ones with a glint)
            float g = 6 * g_scale * tw;
            dl->AddLine(ImVec2(p.x - g, p.y), ImVec2(p.x + g, p.y), IM_COL32(255, 245, 220, (int)(120 * tw)), 1);
            dl->AddLine(ImVec2(p.x, p.y - g), ImVec2(p.x, p.y + g), IM_COL32(255, 245, 220, (int)(120 * tw)), 1);
        }
    }
}

// A faceted gem: a hexagon in the colour, a lighter facet up-left, a darker one down-right, a glint
static void gem(ImDrawList *dl, ImVec2 c, float r, ImVec4 col, float glow_a = 0.35f) {
    glow(dl, c, r * 2.6f, col, glow_a);
    ImVec2 v[6];
    for (int i = 0; i < 6; i++) v[i] = ImVec2(c.x + r * cosf(i * 1.0472f + 0.5236f), c.y + r * sinf(i * 1.0472f + 0.5236f));
    dl->AddConvexPolyFilled(v, 6, U(col));
    ImVec4 light = mix(col, ImVec4(1, 1, 1, 1), 0.45f), dark = mix(col, ImVec4(0, 0, 0, 1), 0.35f);
    dl->AddTriangleFilled(c, v[3], v[4], U(light));
    dl->AddTriangleFilled(c, v[4], v[5], U(mix(col, ImVec4(1, 1, 1, 1), 0.25f)));
    dl->AddTriangleFilled(c, v[0], v[1], U(dark));
    dl->AddTriangleFilled(c, v[1], v[2], U(mix(col, ImVec4(0, 0, 0, 1), 0.2f)));
    dl->AddPolyline(v, 6, U(light, 0.9f), ImDrawFlags_Closed, 1.2f * g_scale);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.35f), r * 0.14f, IM_COL32(255, 255, 255, 220), 8);
}

static void section(const char *title, float width = 0) {  // a heading in the display face, an underline in the accent
    ImGui::Spacing();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = f_head->FontSize;
    dl->AddText(f_head, fs, ImVec2(p.x + 1.5f * g_scale, p.y + 1.5f * g_scale), IM_COL32(0, 0, 0, 160), title);
    dl->AddText(f_head, fs, p, U(C_GOLD), title);
    float w = width > 0 ? width : ImGui::GetContentRegionAvail().x, y = p.y + fs + 3 * g_scale;
    dl->AddRectFilledMultiColor(ImVec2(p.x, y), ImVec2(p.x + w, y + 2 * g_scale), U(accent()), U(accent(0)), U(accent(0)),
                                U(accent()));
    ImGui::Dummy(ImVec2(w, fs + 8 * g_scale));
}
static void hint(const char *text) {
    ImGui::PushStyleColor(ImGuiCol_Text, C_GREY);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
static void row_label(const char *text, float w) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(w);
}
static void check_mark(bool ok, bool busy = false, bool pending = false) {  // a dot with a tick / a cross (no glyphs)
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 8 * g_scale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 c(p.x + r, p.y + ImGui::GetTextLineHeight() / 2 + 1);
    ImU32 ok_col = ImGui::GetColorU32(C_OK), bad_col = ImGui::GetColorU32(C_RED_HI), grey = ImGui::GetColorU32(C_GREY);
    if (pending) {
        dl->AddCircle(c, r - 1, grey, 20, 2 * g_scale);
    } else if (busy) {
        float t = (float)ImGui::GetTime() * 6;
        dl->PathArcTo(c, r - 1, t, t + 4.2f, 16);
        dl->PathStroke(ImGui::GetColorU32(C_GOLD), 0, 3 * g_scale);
    } else {
        dl->AddCircleFilled(c, r, ok ? ok_col : bad_col, 20);
        ImU32 ink = IM_COL32(18, 20, 28, 255);
        if (ok) {
            dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ink, 2.4f * g_scale);
            dl->AddLine(ImVec2(c.x - r * 0.1f, c.y + r * 0.4f), ImVec2(c.x + r * 0.5f, c.y - r * 0.4f), ink, 2.4f * g_scale);
        } else {
            dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.4f), ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), IM_COL32_WHITE, 2.4f * g_scale);
            dl->AddLine(ImVec2(c.x + r * 0.4f, c.y - r * 0.4f), ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), IM_COL32_WHITE, 2.4f * g_scale);
        }
    }
    ImGui::Dummy(ImVec2(r * 2 + 6 * g_scale, 0));
    ImGui::SameLine();
}
// A gem-coloured button: a gradient from the colour to a deeper shade, a light rim, a glow when it's hovered or has
// the controller's focus; greyed out when it can't be pressed
static bool gem_button(const char *label, ImVec2 size, ImVec4 col, bool enabled = true, ImFont *font = nullptr) {
    ImGui::BeginDisabled(!enabled);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused()), held = enabled && ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (!enabled) col = mix(col, ImVec4(0.35f, 0.33f, 0.45f, 1), 0.75f);
    float rr = 8 * g_scale;
    ImVec2 q(p.x + size.x, p.y + size.y);
    if (hot) for (int i = 4; i >= 1; i--)
        dl->AddRect(ImVec2(p.x - i * 2.0f, p.y - i * 2.0f), ImVec2(q.x + i * 2.0f, q.y + i * 2.0f), U(col, 0.10f), rr + i * 2.0f,
                    0, 2.5f * g_scale);
    ImVec4 top = mix(col, ImVec4(1, 1, 1, 1), hot ? 0.28f : 0.12f), bottom = mix(col, ImVec4(0, 0, 0, 1), held ? 0.5f : 0.35f);
    dl->AddRectFilled(p, q, U(bottom), rr);
    dl->AddRectFilledMultiColor(ImVec2(p.x + 1, p.y + rr * 0.5f), ImVec2(q.x - 1, q.y - rr * 0.5f), U(top), U(top), U(bottom),
                                U(bottom));
    dl->AddRectFilled(p, ImVec2(q.x, p.y + size.y * 0.5f), U(top, 0.35f), rr, ImDrawFlags_RoundCornersTop);
    dl->AddRect(p, q, U(mix(col, ImVec4(1, 1, 1, 1), 0.55f), enabled ? 0.9f : 0.4f), rr, 0, 1.4f * g_scale);
    ImFont *f = font ? font : ImGui::GetFont();
    float fs = font ? font->FontSize : ImGui::GetFontSize();
    const char *end = strstr(label, "##");  // (an id after ## isn't shown)
    if (!end) end = label + strlen(label);
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label, end);
    ImVec2 tp(p.x + (size.x - ts.x) / 2, p.y + (size.y - ts.y) / 2);
    dl->AddText(f, fs, ImVec2(tp.x + 1.5f * g_scale, tp.y + 1.5f * g_scale), IM_COL32(0, 0, 0, 140), label, end);
    dl->AddText(f, fs, tp, enabled ? IM_COL32_WHITE : IM_COL32(200, 196, 220, 160), label, end);
    return pressed && enabled;
}

// ================================================================== pages
static bool g_play_now;  // (PLAY pressed: settings saved, then the game started after this frame)

static void page_play(void) {
    section("READY TO PLAY");
    struct { const char *name; bool ok; const char *fix; } rows[] = {
        {"Your game files are set up", g_status.extracted, "GAME FILES: choose your game, update and keys, then INSTALL."},
        {"Your NSPs are where they were set up", g_status.nsps_there, "Put them back, or INSTALL again from GAME FILES."},
        {"The PC version is built", g_status.built, "GAME FILES: INSTALL builds it."},
    };
    for (auto &r : rows) {
        check_mark(r.ok);
        ImGui::TextUnformatted(r.name);
        if (!r.ok) { ImGui::SameLine(); ImGui::TextColored(C_GREY, "  %s", r.fix); }
    }
    if (!g_status.version.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(C_CYAN, "Game version %s", g_status.version.c_str());
    }
    ImGui::Spacing();
    ImGui::Spacing();
    bool ready = g_status.extracted && g_status.built && g_status.nsps_there && !g_running;
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w * 0.2f);
    static int autoplay = getenv("MUA3_LAUNCHER_PLAY") ? 5 : -1;  // (test: Play after a few frames)
    if (gem_button("PLAY", ImVec2(w * 0.6f, 80 * g_scale), k_gem[0], ready, f_title) ||
        (ready && autoplay > 0 && --autoplay == 0))
        g_play_now = true;
    ImGui::Spacing();
    hint("Keyboard: WASD move, K / J / L / I are the Switch's A / B / X / Y, Q / E are L / R, 1 / 3 are ZL / ZR, Enter is "
         "Plus (+), Backspace is Minus (-). Controllers work too (buttons by position, as on the Switch). Change them on "
         "CONTROLS. In game, F1 or Esc opens the recomp's menu; F11 or Alt+Enter switches fullscreen.");
}

static void path_row(const char *label, int target, SDL_Window *win, bool optional, float lw) {
    char *buf = g_path_fields[target];
    bool have = buf[0] && is_file(buf);
    check_mark(have || (optional && !buf[0]));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110 * g_scale);
    char id[32];
    snprintf(id, sizeof id, "##path%d", target);
    if (ImGui::InputText(id, buf, 1024)) paths_to_ini();
    ImGui::SameLine();
    snprintf(id, sizeof id, "Browse##%d", target);
    if (ImGui::Button(id, ImVec2(100 * g_scale, 0))) browse(target, win);
}

static void page_setup(SDL_Window *win) {
    section("YOUR GAME FILES");
    hint("Use your own copy of MARVEL ULTIMATE ALLIANCE 3: The Black Order, dumped from your Switch with your own keys. "
         "Nothing is downloaded or shared. The game's data stays in your NSP / XCI files and is read from them where they "
         "are, so keep them in place after setting up.");
    ImGui::Spacing();
    float lw = 250 * g_scale;
    path_row("Base game (NSP or XCI)", 0, win, false, lw);
    path_row("Update 4.0.1 (NSP)", 1, win, false, lw);
    path_row("Expansion Pass (optional)", 2, win, true, lw);
    path_row("Keys (prod.keys)", 3, win, false, lw);
    ImGui::Spacing();
    if (g_release) {
        check_mark(true);
        ImGui::TextUnformatted("Tools included: the extractor, the recompiler and the compiler (nothing else to install)");
    } else {
#ifdef __linux__
        check_mark(g_status.msys);
        ImGui::TextUnformatted("Native Linux build (game setup needs clang, Vulkan and FFmpeg)");
        check_mark(g_status.venv);
        ImGui::TextUnformatted(g_status.venv ? "Python environment (.venv)" : "Python environment: made by INSTALL");
#else
        check_mark(g_status.msys);
        row_label("MSYS2 (clang, make)", lw);
        ImGui::SetNextItemWidth(260 * g_scale);
        if (ImGui::InputText("##msys", p_msys, sizeof p_msys)) { paths_to_ini(); refresh_status(); }
        if (!g_status.msys) { ImGui::SameLine(); ImGui::TextColored(C_WARN, "not found: see the README"); }
        check_mark(g_status.venv);
        ImGui::TextUnformatted(g_status.venv ? "Python environment (.venv)" : "Python environment: made by INSTALL (needs Python 3.12)");
#endif
    }
    ImGui::Spacing();
    bool inputs = p_base[0] && is_file(p_base) && p_keys[0] && is_file(p_keys) && p_update[0] && is_file(p_update) &&
                  (!p_dlc[0] || is_file(p_dlc));
    bool can = !g_running && g_status.msys;
    float bw = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3, bh = 40 * g_scale;
    if (gem_button("INSTALL", ImVec2(bw, bh), k_gem[1], can && inputs, f_head)) {
        paths_to_ini();
        start_steps(true, true, true);
    }
    ImGui::SameLine();
    if (gem_button("REBUILD", ImVec2(bw, bh), k_gem[2], can && g_status.extracted, f_head)) start_steps(false, true, true);
    ImGui::SameLine();
    if (gem_button("CANCEL", ImVec2(bw, bh), k_gem[0], g_running, f_head)) cancel_steps();
    hint(g_release ? "INSTALL: read your game files, recompile the game and build it for this PC (about 20 to 40 minutes "
                     "the first time, depending on the CPU). REBUILD: recompile and build again from the files set up."
                   : "INSTALL: read your game files, recompile (make gen) and build (make). REBUILD: recompile and build "
                     "again from the files set up.");
    ImGui::Spacing();
    for (auto &st : g_steps) {
        check_mark(st.state == ST_OK, st.state == ST_RUN, st.state == ST_WAIT || st.state == ST_SKIP);
        if (st.state == ST_RUN) ImGui::TextColored(C_GOLD, "%s...", st.title.c_str());
        else if (st.state == ST_OK) ImGui::Text("%s  (%d s)", st.title.c_str(), (int)st.secs);
        else if (st.state == ST_FAIL) ImGui::TextColored(C_WARN, "%s: failed%s%s", st.title.c_str(), st.why.empty() ? "" : " - ",
                                                         st.why.c_str());
        else ImGui::TextColored(C_GREY, "%s", st.title.c_str());
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0.45f));
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(f_small);
    {
        std::lock_guard<std::mutex> lk(g_log_mu);
        ImGuiListClipper clip;
        clip.Begin((int)g_log.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const std::string &l = g_log[i];
                bool err = l.find("error") != std::string::npos || l.find("Failed") != std::string::npos;
                if (l.rfind("====", 0) == 0) ImGui::TextColored(C_GOLD, "%s", l.c_str());
                else if (err) ImGui::TextColored(C_WARN, "%s", l.c_str());
                else ImGui::TextUnformatted(l.c_str());
            }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

static void page_display(void) {
    float lw = 220 * g_scale;
    ImGui::PushItemWidth(-1);
    section("DISPLAY");
    const char *modes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};
    row_label("Window mode", lw);
    ImGui::Combo("##mode", &s_mode, modes, 3);
    row_label(s_mode == 2 ? "Display mode" : "Window size", lw);
    if (s_mode == 1) {
        ImGui::BeginDisabled();
        char d[64];
        snprintf(d, sizeof d, "Desktop (%dx%d)", g_desktop.w, g_desktop.h);
        if (ImGui::BeginCombo("##size_b", d)) ImGui::EndCombo();
        ImGui::EndDisabled();
    } else {
        auto label = [&](int i, char *out, size_t n) {
            if (i < 0) snprintf(out, n, s_mode == 2 ? "Desktop (%dx%d)" : "Default (1280x720)", g_desktop.w, g_desktop.h);
            else snprintf(out, n, s_mode == 2 ? "%dx%d (%.0f Hz)" : "%dx%d", g_modes[i].w, g_modes[i].h, g_modes[i].hz);
        };
        char cur[64];
        label(s_size, cur, sizeof cur);
        if (ImGui::BeginCombo("##size", cur)) {
            for (int i = -1; i < (int)g_modes.size(); i++) {
                char m[64];
                label(i, m, sizeof m);
                if (ImGui::Selectable(m, s_size == i)) s_size = i;
            }
            ImGui::EndCombo();
        }
    }
    hint(s_mode == 0 ? "The window's size (it can be resized while playing; F11 or Alt+Enter switches to fullscreen)."
         : s_mode == 1 ? "Fills the screen at the desktop's resolution and refresh rate."
                       : "Switches the display to this resolution at its highest refresh rate.");
    row_label("Render resolution", lw);
    choice_combo("##render", &s_render, k_render, IM_ARRAYSIZE(k_render));
    row_label("Upscaler", lw);
    choice_combo("##upscaler", &s_upscaler, k_upscaler, IM_ARRAYSIZE(k_upscaler));
    row_label("Anti-aliasing", lw);
    choice_combo("##aa", &s_aa, k_aa, IM_ARRAYSIZE(k_aa));
    row_label("Aspect ratio", lw);
    choice_combo("##aspect", &s_aspect, k_aspect, IM_ARRAYSIZE(k_aspect));

    section("FRAME RATE");
    const char *rates[] = {"30 fps", "60 fps", "120 fps"};
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        ImGui::RadioButton(rates[i], &s_fps, i);
    }
    hint(s_fps == 0 ? "The original frame rate."
         : s_fps == 1 ? "The game keeps time by the clock, so it plays at its normal speed at 60."
                      : "Normal game speed at 120 too. Needs a fast CPU and GPU and a 120 Hz (or faster) display; "
                        "where the PC can't keep up it runs between 60 and 120.");
    if (s_fps == 2 && g_desktop.hz > 0 && g_desktop.hz < 119 && s_mode != 2)
        ImGui::TextColored(C_WARN, "Your display runs at %.0f Hz: frames above that aren't shown.", g_desktop.hz);
    ImGui::Checkbox("Show the frame rate in the corner", &s_show_fps);

    section("SOUND");
    row_label("Volume", lw);
    int pct = (int)lroundf(s_volume * 100);
    if (ImGui::SliderInt("##vol", &pct, 0, 200, "%d%%")) s_volume = pct / 100.0f;
    ImGui::Checkbox("Sound in the movies", &s_movie_audio);
    ImGui::PopItemWidth();
}

// A row of the bindings table: the Switch input, what it's on now, and a button to change it
static void binding_row(int dev, int slot, int fam) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C_GOLD, "%s", k_bind_name[slot]);
    ImGui::TableNextColumn();
    bool waiting = g_cap_dev == dev && g_cap_slot == slot;
    bool dflt = dev == DEV_PAD ? g_pad[slot] == k_def_pad[slot] : g_key[slot] == k_def_key[slot];
    char label[96];
    if (waiting) snprintf(label, sizeof label, "Press a %s... (%d)", dev == DEV_PAD ? "button" : "key",
                          (int)((g_cap_deadline - SDL_GetTicks()) / 1000) + 1);
    else snprintf(label, sizeof label, "%s", dev == DEV_PAD ? pad_label(g_pad[slot], fam) : key_label(g_key[slot]));
    ImGui::PushID(dev * 100 + slot);
    int colours = 0;
    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, C_RED), colours++;
    else if (!dflt) ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN), colours++;  // (changed from the default)
    if (ImGui::Button(label, ImVec2(-1, 0)) && g_cap_dev < 0) capture_begin(dev, slot);
    ImGui::PopStyleColor(colours);
    ImGui::PopID();
}

static void page_controls(void) {
    int fam = pad_family();
    static const char *const fam_name[] = {"Nintendo", "Xbox", "PlayStation"};
    hint("Click a button on the right, then press the key or controller button you want for that Switch input (Esc "
         "cancels). A key or button already in use swaps places. Changes are saved with Save or PLAY, and the game's "
         "own menu (F1 > Controls) edits the same bindings. The recomp's menu always opens with F1, Esc or Home, so no "
         "binding can lock you out.");
    if (!g_cap_result.empty()) ImGui::TextColored(C_CYAN, "%s", g_cap_result.c_str());
    float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 2;
    float tall = std::max(160 * g_scale, ImGui::GetContentRegionAvail().y - 260 * g_scale);  // (room for the rest)
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;

    ImGui::BeginGroup();
    section("KEYBOARD", half);
    if (ImGui::BeginTable("##keys", 2, tf, ImVec2(half, tall))) {
        ImGui::TableSetupColumn("in", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        for (int i = 0; i < N_KEY; i++) binding_row(DEV_KEY, i, fam);
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset the keyboard")) {
        memcpy(g_key, k_def_key, sizeof g_key);
        g_cap_result = "Keyboard back to the defaults (saved with Save or PLAY).";
    }
    ImGui::EndGroup();
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3);
    ImGui::BeginGroup();
    char head[64];
    snprintf(head, sizeof head, "CONTROLLER (%s names)", fam_name[fam]);
    section(head, half);
    if (ImGui::BeginTable("##pad", 2, tf, ImVec2(half, tall))) {
        ImGui::TableSetupColumn("in", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        for (int i = 0; i < N_PAD; i++) binding_row(DEV_PAD, i, fam);
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset the controller")) {
        memcpy(g_pad, k_def_pad, sizeof g_pad);
        g_cap_result = "Controller back to the defaults (saved with Save or PLAY).";
    }
    ImGui::EndGroup();

    section("CONTROLLER");
    float lw = 220 * g_scale;
    ImGui::Checkbox("Rumble", &s_rumble);
    if (s_rumble) {
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(260 * g_scale);
        int i = s_rumble_pct / 25 - 1;
        const char *pct[] = {"25%", "50%", "75%", "100%"};
        if (ImGui::Combo("Intensity", &i, pct, 4)) s_rumble_pct = (i + 1) * 25;
    }
    ImGui::Checkbox("Controllers work while another window is in front", &s_bg_input);
    hint(g_gamepads.empty() ? "No controller connected: Xbox names are shown. The sticks always stay the sticks."
                            : "Buttons go by position, as on the Switch (its A is the right face button). The sticks always "
                              "stay the sticks.");
}

static void page_about(void) {
    section("ABOUT");
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(APP " runs your copy of MARVEL ULTIMATE ALLIANCE 3: The Black Order natively on Windows: the "
                           "game's ARM64 code is recompiled to C and built for x86-64, and a Switch runtime (graphics "
                           "through Vulkan, audio, input, files) takes the console's place. It's an early work in progress.");
    ImGui::Spacing();
    ImGui::TextColored(C_CYAN, "Credits");
    ImGui::BulletText("LAS Recomp by Peiikai and contributors (MIT): the recompiler and the Switch runtime");
    ImGui::BulletText("SDL 3, Dear ImGui, FFmpeg, AMD FidelityFX FSR, SMAA, shaderc");
    ImGui::BulletText("Recomp Achievements by Ripalda (with rcheevos and cJSON, MIT)");
    ImGui::BulletText("Baloo 2 font (Ek Type, SIL Open Font License); DejaVu Sans");
    ImGui::BulletText("Switchbrew's documentation");
    ImGui::Spacing();
    ImGui::TextColored(C_CYAN, "Legal");
    ImGui::TextColored(C_GREY, "MARVEL ULTIMATE ALLIANCE 3 is a trademark of Marvel and Nintendo; the game was developed by "
                       "Koei Tecmo. This project is not affiliated with them. It contains no game code, data or art: you "
                       "need your own legally obtained copy of the game, its update and your console's keys.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextColored(C_GREY, "Data folder: %s", g_root.c_str());
}

static void style(void) {  // translucent violet controls over the cosmos; the page's gem colour where it's chosen
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0, s.ChildRounding = 10, s.FrameRounding = 7, s.GrabRounding = 7, s.PopupRounding = 8;
    s.ScrollbarRounding = 8, s.FramePadding = ImVec2(10, 6), s.ItemSpacing = ImVec2(10, 8), s.WindowBorderSize = 0;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.95f, 1.0f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = ImVec4(0.70f, 0.60f, 1.0f, 0.22f);
    c[ImGuiCol_Separator] = ImVec4(0.70f, 0.60f, 1.0f, 0.22f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.12f, 0.34f, 0.85f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.18f, 0.50f, 0.90f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.34f, 0.22f, 0.60f, 0.95f);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.15f, 0.42f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.24f, 0.66f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.30f, 0.80f, 1);
    c[ImGuiCol_Header] = ImVec4(0.42f, 0.24f, 0.74f, 0.75f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.52f, 0.30f, 0.86f, 0.85f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.60f, 0.34f, 0.95f, 0.95f);
    c[ImGuiCol_CheckMark] = C_GOLD;
    c[ImGuiCol_SliderGrab] = C_GOLD;
    c[ImGuiCol_SliderGrabActive] = ImVec4(1, 0.9f, 0.5f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.06f, 0.20f, 0.98f);
    c[ImGuiCol_TableRowBg] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.15f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.50f, 0.36f, 0.86f, 0.70f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.64f, 0.32f, 1.0f, 0.45f);
    c[ImGuiCol_NavCursor] = C_GOLD;
}

static ImFont *load_font(float size, bool display = false) {  // display: Baloo 2 (SIL OFL, third_party/fonts)
    ImGuiIO &io = ImGui::GetIO();
    std::vector<std::string> paths;
    if (display) paths = {g_exe_dir + "Baloo2-SemiBold.ttf", g_root + "third_party\\fonts\\Baloo2\\Baloo2-SemiBold.ttf",
                          "C:\\Windows\\Fonts\\segoeuib.ttf"};
    paths.insert(paths.end(), {g_exe_dir + "DejaVuSans.ttf", g_root + "third_party\\fonts\\DejaVu\\DejaVuSans.ttf",
                               "C:\\Windows\\Fonts\\segoeui.ttf"});
    for (const std::string &p : paths)
        if (is_file(p))
            if (ImFont *f = io.Fonts->AddFontFromFileTTF(native_path(p).c_str(), size)) return f;
    return io.Fonts->AddFontDefault();
}

// ================================================================== the command lines
// mua3launcher --prepare --game=<nsp|xci> --update=<nsp> [--dlc=<nsp>] --keys=<prod.keys>: RecompLauncher's prepare
// step (recomplauncher.json). No window: the paths are saved (launcher.ini), then INSTALL's steps run, with
// "progress <0..1> <text>" lines on stdout and the whole log in prepare.log. Exit code 0 when the game is ready.
static int prepare_cli(int argc, char **argv) {
    p_dlc[0] = 0;  // (only what this command line gives: an Expansion Pass left out is left out)
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        auto val = [&](const char *key, char *dst, size_t n) {  // (--key=<path>, or --key <path>)
            size_t k = strlen(key);
            if (!strncmp(a, key, k) && a[k] == '=') { snprintf(dst, n, "%s", a + k + 1); return true; }
            if (!strcmp(a, key) && i + 1 < argc) { snprintf(dst, n, "%s", argv[++i]); return true; }
            return false;
        };
        if (!val("--base", p_base, sizeof p_base) && !val("--game", p_base, sizeof p_base) &&
            !val("--update", p_update, sizeof p_update) && !val("--dlc", p_dlc, sizeof p_dlc) &&
            !val("--keys", p_keys, sizeof p_keys) && strcmp(a, "--prepare"))
            cli_out(std::string("unknown argument: ") + a + "\n");
    }
    if (strstr(p_dlc, "{input:")) p_dlc[0] = 0;  // (an optional input the player skipped, passed on as it was written)
    g_cli_log = fopen((g_root + "prepare.log").c_str(), "w");
    auto fail = [](const std::string &why) {
        cli_out("error " + why + "\n");
        log_line(why);
        return 1;
    };
#ifndef __linux__
    if (!g_release) return fail("This isn't a release folder (mua3gen, zig and gamelib-src next to the launcher).");
#endif
    if (!p_base[0] || !is_file(p_base)) return fail("The game (an NSP or XCI of MARVEL ULTIMATE ALLIANCE 3) wasn't found: " + std::string(p_base));
    if (!p_keys[0] || !is_file(p_keys)) return fail("The keys file (prod.keys) wasn't found: " + std::string(p_keys));
    if (!p_update[0] || !is_file(p_update)) return fail("The 4.0.1 update (NSP) wasn't found: " + std::string(p_update));
    if (p_dlc[0] && !is_file(p_dlc)) return fail("The Expansion Pass (NSP) wasn't found: " + std::string(p_dlc));
    paths_to_ini();
    start_steps(true, true, true);
    g_worker.join();
    if (g_result != 1) {
        std::string why = "A step failed: see prepare.log in the game's folder.";
        for (auto &st : g_steps)
            if (st.state == ST_FAIL) {
                why = st.title + " failed: " + (st.why.empty() ? "" : st.why + " ") + "(prepare.log in the game's folder has more)";
                break;
            }
        cli_out("error " + why + "\n");
        return 2;
    }
    cli_out("progress 1 Ready to play\n");
    return 0;
}

int main(int argc, char **argv) {
    bool prepare = false, play = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prepare")) prepare = true;
        if (!strcmp(argv[i], "--play")) play = true;
    }
    find_paths();
    find_release();
    g_launcher_ini.load(g_root + "launcher.ini");
    paths_from_ini();
#ifdef _WIN32
    g_job = CreateJobObjectA(NULL, NULL);  // (Cancel / closing the launcher ends a step's whole process tree)
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
#endif
    if (prepare) {
        g_cli = true;
        int r = prepare_cli(argc, argv);
        if (g_cli_log) fclose(g_cli_log);
        return r;
    }
    refresh_status();
    // mua3launcher --play (RecompLauncher's Play): the game straight away, waited for (its exit code is ours). Only when
    // the game code needs building again - a release update changed the recompiler - the window opens and rebuilds it
    // first, from the game files set up before.
    if (play && g_status.extracted && g_status.built) {
        int code = 0;
        std::string e = launch_game(&code);
        if (!e.empty()) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP, e.c_str(), NULL); return 1; }
        return code;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Launcher: SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    list_modes();
    g_settings_ini.load(g_root + "larecomp.ini");
    settings_from_ini();
    bool play_after = play && g_status.extracted && g_status.nsps_there;  // (rebuild, then play)

    g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g_scale <= 0) g_scale = 1;
    int ww = (int)(980 * g_scale), wh = (int)(800 * g_scale);  // (within the desktop, high-DPI screens included)
    if (ww > g_desktop.w * 9 / 10) ww = g_desktop.w * 9 / 10;
    if (wh > g_desktop.h * 9 / 10) wh = g_desktop.h * 9 / 10;
    SDL_Window *win = SDL_CreateWindow("Marvel Ultimate Alliance 3 Recomp - Launcher", ww, wh,
                                       SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "Launcher: could not create the window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(win, std::min(ww, (int)(820 * g_scale)), std::min(wh, (int)(640 * g_scale)));
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
    if (!ren) {
        fprintf(stderr, "Launcher: could not create the SDL renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    SDL_Texture *art = NULL;
    for (const std::string &p : {g_exe_dir + "icon.png", g_root + "build\\icon.png"}) {
        if (art || !is_file(p)) continue;
        std::string np = native_path(p);
        if (SDL_Surface *s = SDL_LoadPNG(np.c_str())) {
            SDL_SetWindowIcon(win, s);
            art = SDL_CreateTextureFromSurface(ren, s);
            SDL_DestroySurface(s);
        }
    }

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    f_body = load_font(19 * g_scale);
    f_small = load_font(15 * g_scale);
    f_big = load_font(30 * g_scale);
    f_head = load_font(31 * g_scale, true);
    f_title = load_font(54 * g_scale, true);
    style();
    ImGui::GetStyle().ScaleAllSizes(g_scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    static const char *const tabs[] = {"PLAY", "GAME FILES", "DISPLAY", "CONTROLS", "ABOUT"};
    int want_tab = g_status.built && g_status.extracted ? 0 : 1;  // (a page to switch to, once: -1 none)
    if (getenv("MUA3_LAUNCHER_TAB")) want_tab = std::clamp(atoi(getenv("MUA3_LAUNCHER_TAB")), 0, 4);
    bool quit = false, was_running = false;
    uint64_t last_status = 0;
    int exit_code = 0;
    if (play_after) {
        want_tab = 1;
        start_steps(false, true, true);
        flash("The game code needs building again for this version: it plays as soon as it's done.");
    } else if (play) {
        want_tab = 1;
        flash("Set up your game files first: choose them here, then INSTALL.");
    }
    while (!quit) {
        {   // (test) MUA3_LAUNCHER_REBIND=k<slot>:<key name>|p<slot>:<SDL button>,...: each a rebind as the CONTROLS page
            // makes it (capture, then the press as an event), one a frame; then the settings saved, and quit
            static const char *rb = getenv("MUA3_LAUNCHER_REBIND");
            static int frames;
            if (rb && ++frames > 3 && g_cap_dev < 0) {
                if (!*rb) { if (!settings_to_ini()) exit_code = 1; quit = true; }
                else {
                    char dev = 0, name[64] = "";
                    int slot = -1, used = 0;
                    if (sscanf(rb, "%c%d:%63[^,]%n", &dev, &slot, name, &used) >= 3 && slot >= 0) {
                        SDL_Event ev = {};
                        if (dev == 'k' && slot < N_KEY) {
                            capture_begin(DEV_KEY, slot);
                            ev.type = SDL_EVENT_KEY_DOWN;
                            ev.key.scancode = SDL_GetScancodeFromName(name);
                        } else if (dev == 'p' && slot < N_PAD) {
                            capture_begin(DEV_PAD, slot);
                            ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                            ev.gbutton.button = (Uint8)atoi(name);
                        }
                        if (ev.type) SDL_PushEvent(&ev);
                    }
                    rb += used ? used : (int)strlen(rb);
                    if (*rb == ',') rb++;
                }
            }
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (SDL_Gamepad *p = SDL_OpenGamepad(e.gdevice.which)) g_gamepads.push_back(p);
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                for (size_t i = 0; i < g_gamepads.size(); i++)
                    if (SDL_GetGamepadID(g_gamepads[i]) == e.gdevice.which) {
                        SDL_CloseGamepad(g_gamepads[i]);
                        g_gamepads.erase(g_gamepads.begin() + i);
                        break;
                    }
            }
            if (capture_event(e)) continue;  // (a rebind took it: the launcher's own controls don't see it)
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        if (g_cap_dev >= 0 && SDL_GetTicks() >= g_cap_deadline) { g_cap_dev = -1; g_cap_result = "Nothing pressed: unchanged."; }
        // controller navigation is off while a rebind waits, and until the button it took is let go (else that press
        // would also press the launcher's focused button)
        if (g_cap_dev >= 0) g_nav_hold = true;
        else if (g_nav_hold && !pad_input_held()) g_nav_hold = false;
        if (g_nav_hold) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        {   // a file dialog's answer (SDL may call back from another thread)
            std::lock_guard<std::mutex> lk(g_dialog_mu);
            if (g_dialog_target >= 0) {
                snprintf(g_path_fields[g_dialog_target], 1024, "%s", g_dialog_result.c_str());
                g_dialog_target = -1;
                paths_to_ini();
            }
        }
        if (was_running && !g_running) {  // (a run just ended)
            refresh_status();
            flash(g_result == 1 ? "All done: the PC version is built. Press PLAY."
                  : g_result == 3 ? "Cancelled." : "A step failed: see the log for why.");
            if (g_result == 1) want_tab = 0;
            if (play_after && g_result == 1 && g_status.built) {  // (--play: rebuilt, so the game now, waited for)
                SDL_HideWindow(win);
                std::string err = launch_game(&exit_code);
                if (!err.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, APP, err.c_str(), NULL), exit_code = 1;
                quit = true;
            }
            play_after = false;
        }
        was_running = g_running;
        if (SDL_GetTicks() - last_status > 2000 && !g_running) { refresh_status(); last_status = SDL_GetTicks(); }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (want_tab >= 0) g_page = want_tab, want_tab = -1;
        float t = (float)ImGui::GetTime();
        cosmos(ImGui::GetBackgroundDrawList(), io.DisplaySize, t);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        // header: the icon, the title in gold, the six gems
        float icon = 76 * g_scale;
        ImVec2 h0 = ImGui::GetCursorScreenPos();
        if (art) {
            glow(dl, ImVec2(h0.x + icon / 2, h0.y + icon / 2), icon * 0.85f, accent(), 0.45f);
            ImGui::Image((ImTextureID)(intptr_t)art, ImVec2(icon, icon));
            ImGui::SameLine();
        }
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            const char *title = "MARVEL ULTIMATE ALLIANCE 3";
            float fs = f_title->FontSize;
            for (int k = 0; k < 4; k++)  // (a soft dark outline under the gold)
                dl->AddText(f_title, fs, ImVec2(p.x + (k & 1 ? 2 : -2) * g_scale, p.y + 2 * g_scale + (k & 2 ? 2 : -1) * g_scale),
                            IM_COL32(20, 6, 40, 200), title);
            dl->AddText(f_title, fs, p, U(C_GOLD), title);
            ImVec2 ts = f_title->CalcTextSizeA(fs, FLT_MAX, 0, title);
            dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + ts.y * 0.52f), ImVec2(p.x + ts.x, p.y + ts.y * 0.86f), 0, 0,
                                        IM_COL32(255, 140, 40, 70), IM_COL32(255, 140, 40, 70));  // (a warm lower half)
            dl->AddText(f_body, f_body->FontSize, ImVec2(p.x + 2 * g_scale, p.y + ts.y - 4 * g_scale), U(C_GREY),
                        "PC recompilation  -  game files, settings, controls and launch");
            float gx = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - 6 * 26 * g_scale;
            for (int k = 0; k < 6; k++) {  // (the six gems; the page's one bigger, and bobbing)
                bool on = k == g_page;
                float bob = on ? 2.5f * g_scale * sinf(t * 2.2f) : 0;
                gem(dl, ImVec2(gx + k * 26 * g_scale + 10 * g_scale, h0.y + 26 * g_scale + bob), (on ? 10.5f : 7.5f) * g_scale,
                    k_gem[k], on ? 0.55f : 0.25f);
            }
            ImGui::Dummy(ImVec2(ts.x, icon));
        }
        ImGui::Spacing();

        // tabs: one gem colour each; the shown one filled and glowing (buttons: mouse, keyboard and controller)
        for (int i = 0; i < 5; i++) {
            if (i) ImGui::SameLine(0, 8 * g_scale);
            ImVec2 ts = f_head->CalcTextSizeA(f_head->FontSize, FLT_MAX, 0, tabs[i]);
            ImVec2 sz(ts.x + 46 * g_scale, ts.y + 12 * g_scale), p = ImGui::GetCursorScreenPos(), q(p.x + sz.x, p.y + sz.y);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("tab", sz)) g_page = i;
            bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused(), on = g_page == i;
            ImGui::PopID();
            ImVec4 col = k_gem[i];
            if (on) {
                for (int k = 3; k >= 1; k--)
                    dl->AddRect(ImVec2(p.x - k * 2.0f, p.y - k * 2.0f), ImVec2(q.x + k * 2.0f, q.y + k * 2.0f), U(col, 0.12f),
                                10 * g_scale + k * 2, 0, 2.5f * g_scale);
                dl->AddRectFilledMultiColor(p, q, U(mix(col, ImVec4(1, 1, 1, 1), 0.15f)), U(mix(col, ImVec4(1, 1, 1, 1), 0.15f)),
                                            U(mix(col, ImVec4(0, 0, 0, 1), 0.35f)), U(mix(col, ImVec4(0, 0, 0, 1), 0.35f)));
            } else {
                dl->AddRectFilled(p, q, U(hot ? mix(col, ImVec4(0.1f, 0.06f, 0.2f, 1), 0.55f) : ImVec4(0.10f, 0.07f, 0.22f, 0.80f)),
                                  10 * g_scale);
            }
            dl->AddRect(p, q, U(col, on ? 0.95f : 0.55f), 10 * g_scale, 0, 1.4f * g_scale);
            gem(dl, ImVec2(p.x + 15 * g_scale, p.y + sz.y / 2), 5.5f * g_scale, col, on ? 0.0f : 0.15f);
            dl->AddText(f_head, f_head->FontSize, ImVec2(p.x + 28 * g_scale, p.y + 6 * g_scale),
                        on ? IM_COL32_WHITE : U(mix(col, ImVec4(1, 1, 1, 1), 0.55f)), tabs[i]);
        }
        ImGui::Spacing();

        // the page: a translucent card edged in its gem colour
        float bottom = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 10 * g_scale;
        {
            ImVec2 a = ImGui::GetCursorScreenPos();
            ImVec2 b(a.x + ImGui::GetContentRegionAvail().x, ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - bottom);
            float pad = 16 * g_scale, rr = 14 * g_scale;
            dl->AddRectFilled(a, b, IM_COL32(12, 8, 30, 205), rr);
            dl->AddRect(a, b, U(accent(0.55f)), rr, 0, 1.5f * g_scale);
            dl->AddRectFilledMultiColor(ImVec2(a.x + rr, a.y), ImVec2(b.x - rr, a.y + 3 * g_scale), U(accent(0)), U(accent()),
                                        U(accent()), U(accent(0)));
            ImGui::SetCursorScreenPos(ImVec2(a.x + pad, a.y + pad * 0.7f));
            ImGui::BeginChild("##page", ImVec2(b.x - a.x - 2 * pad, b.y - a.y - pad * 1.2f));
            switch (g_page) {
            case 0: page_play(); break;
            case 1: page_setup(win); break;
            case 2: page_display(); break;
            case 3: page_controls(); break;
            default: page_about(); break;
            }
            ImGui::EndChild();
        }
        // the bottom bar: status line (shortened to fit beside the buttons: the whole of it as a tooltip), Save, Quit
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - bottom + 4 * g_scale);
        float bw = 130 * g_scale;
        float room = ImGui::GetWindowWidth() - 2 * bw - 3 * ImGui::GetStyle().ItemSpacing.x - 2 * ImGui::GetStyle().WindowPadding.x;
        auto status_line = [&](const ImVec4 &col, const std::string &text, bool keep_end) {
            std::string shown = text;
            while (shown.size() > 4 && ImGui::CalcTextSize(shown.c_str()).x > room)  // (a path keeps its end, a message its start)
                shown = keep_end ? "..." + shown.substr(4) : shown.substr(0, shown.size() - 4) + "...";
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col, "%s", shown.c_str());
            if (shown != text && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
        };
        if (g_running) {
            int done = 0, n = 0;
            for (auto &st : g_steps) if (st.state != ST_SKIP) { n++; if (st.state == ST_OK) done++; }
            status_line(C_GOLD, "Working: step " + std::to_string(std::min(done + 1, n)) + " of " + std::to_string(n), false);
        } else if (!g_flash.empty() && ImGui::GetTime() < g_flash_until) {
            status_line(C_WARN, g_flash, false);
        } else {
            status_line(C_GREY, "Settings: " + g_root + "larecomp.ini", true);
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - 2 * bw - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().WindowPadding.x);
        if (gem_button("Save", ImVec2(bw, ImGui::GetFrameHeight()), accent()))
            flash(settings_to_ini() ? "Settings saved." : "Couldn't write larecomp.ini.");
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(bw, 0))) quit = true;
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 12, 10, 36, 255);
        SDL_RenderClear(ren);
        // ImGui and SDL mouse events use window coordinates. On Wayland/HiDPI,
        // the renderer targets more pixels than the window has logical units.
        // Scale geometry and clip rectangles together so visible buttons and
        // their input rectangles occupy the same place on the desktop.
        SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        {   // (test) MUA3_LAUNCHER_SHOT=<file.bmp>: the window's look saved after a few frames, then quit
            static int frames;
            const char *shot = getenv("MUA3_LAUNCHER_SHOT");
            if (shot && ++frames == 8) {
                if (SDL_Surface *s = SDL_RenderReadPixels(ren, NULL)) SDL_SaveBMP(s, shot), SDL_DestroySurface(s);
                quit = true;
            }
        }
        SDL_RenderPresent(ren);
        if (g_play_now) {  // (PLAY: the settings and controls saved, then the game; the launcher closes)
            g_play_now = false;
            if (!settings_to_ini()) flash("Couldn't write larecomp.ini.");
            else {
                std::string err = launch_game();
                if (err.empty()) quit = true;
                else flash(err);
            }
        }
    }
    if (g_running) cancel_steps();
    if (g_worker.joinable()) g_worker.join();
    if (g_cli_log) { fclose(g_cli_log); g_cli_log = nullptr; }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    for (SDL_Gamepad *p : g_gamepads) SDL_CloseGamepad(p);
    if (art) SDL_DestroyTexture(art);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return exit_code;
}
