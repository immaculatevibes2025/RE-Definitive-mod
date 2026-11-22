// VideoMenu.cpp - the video and sound settings behind Option Mode (port-only)
//
// Not in the original binary. Owns the settings the VIDEO / SOUND / GAMEPLAY
// tabs of the PC Option Mode screen edit (OptionsMenu.cpp draws them), and the
// title screen's OPTIONS, which opens Option Mode directly.
//
// VIDEO: DISPLAY (windowed / borderless fullscreen), RES., ASPECT (4:3 or
// widescreen), AA (SMAA), CRT shader, 60 FPS (interpolated). Video changes are
// held as pending values and only made on APPLY, which also writes config.ini.
//
// (The old F2 pause overlay that used to live here has been removed.)
// ============================================================================
#include "../Globals.h"
#include "../platform/platform.h"
#include "../marni/MarniDX.h"        // MARNI_ASPECT_*
#include "../marni/MarniSystem.h"    // MarniSetAspectMode, MarniSetSmaa, MarniSetCrtShader
#include "../system/ConfigFile.h"    // ConfigFile_Save
#include <cstdio>
#include <cstring>


// ============================================================================
// Input - the debug menu's scheme: arrow keys / ENTER / SPACE / ESC, and the pad through g_RawPadHeld, the function-level word that
// has already been through the player's bindings (see DebugMenu.cpp).
// ============================================================================
#define VIDKEY_LEFT    0x001
#define VIDKEY_RIGHT   0x002
#define VIDKEY_UP      0x004
#define VIDKEY_DOWN    0x008
#define VIDKEY_CONFIRM 0x010
#define VIDKEY_ESC     0x020

#define VIDPAD_UP      0x1000
#define VIDPAD_DOWN    0x4000
#define VIDPAD_LEFT    0x8000
#define VIDPAD_RIGHT   0x2000
#define VIDPAD_ACTION  0x0080
#define VIDPAD_CANCEL  0x0040

static int VideoMenu_SampleKeys(void)
{
    int keys = 0;
    if (plat_key_state(VK_LEFT)   & 0x8000) keys |= VIDKEY_LEFT;
    if (plat_key_state(VK_RIGHT)  & 0x8000) keys |= VIDKEY_RIGHT;
    if (plat_key_state(VK_UP)     & 0x8000) keys |= VIDKEY_UP;
    if (plat_key_state(VK_DOWN)   & 0x8000) keys |= VIDKEY_DOWN;
    if (plat_key_state(VK_RETURN) & 0x8000) keys |= VIDKEY_CONFIRM;
    if (plat_key_state(VK_SPACE)  & 0x8000) keys |= VIDKEY_CONFIRM;
    if (plat_key_state(VK_ESCAPE) & 0x8000) keys |= VIDKEY_ESC;

    DWORD pad = g_RawPadHeld;
    if (pad & VIDPAD_LEFT)   keys |= VIDKEY_LEFT;
    if (pad & VIDPAD_RIGHT)  keys |= VIDKEY_RIGHT;
    if (pad & VIDPAD_UP)     keys |= VIDKEY_UP;
    if (pad & VIDPAD_DOWN)   keys |= VIDKEY_DOWN;
    if (pad & VIDPAD_ACTION) keys |= VIDKEY_CONFIRM;
    if (pad & VIDPAD_CANCEL) keys |= VIDKEY_ESC;
    return keys;
}

// ============================================================================
// Resolution list - window client sizes. Built when the menu opens: every
// preset that fits the monitor's work area, plus the current window size if it
// is not one of them, in ascending order.
// ============================================================================
static const unsigned short kVidPresets[][2] = {
    {  640,  480 }, {  800,  600 }, {  960,  720 }, { 1024,  768 },
    { 1280,  720 }, { 1280,  960 }, { 1366,  768 }, { 1440, 1080 },
    { 1600,  900 }, { 1600, 1200 }, { 1920, 1080 }, { 1920, 1440 },
    { 2560, 1440 }, { 2880, 2160 }, { 3200, 1800 }, { 3840, 2160 },
};
#define VID_PRESET_COUNT ((int)(sizeof(kVidPresets) / sizeof(kVidPresets[0])))
#define VID_MAX_RES      (VID_PRESET_COUNT + 1)

static DWORD s_vidResW[VID_MAX_RES];
static DWORD s_vidResH[VID_MAX_RES];
static int   s_vidResCount = 0;

static void VideoMenu_AddRes(DWORD w, DWORD h)
{
    for (int i = 0; i < s_vidResCount; i++) {
        if (s_vidResW[i] == w && s_vidResH[i] == h) return;
    }
    if (s_vidResCount >= VID_MAX_RES) return;
    // Insertion sort by area, then width: the list stays ascending.
    int at = s_vidResCount;
    while (at > 0 &&
           (s_vidResW[at - 1] * s_vidResH[at - 1] > w * h ||
            (s_vidResW[at - 1] * s_vidResH[at - 1] == w * h && s_vidResW[at - 1] > w))) {
        s_vidResW[at] = s_vidResW[at - 1];
        s_vidResH[at] = s_vidResH[at - 1];
        at--;
    }
    s_vidResW[at] = w;
    s_vidResH[at] = h;
    s_vidResCount++;
}

static void VideoMenu_BuildResList(void)
{
    DWORD workW = 0, workH = 0;
    plat_display_work_size(&workW, &workH);
    s_vidResCount = 0;
    for (int i = 0; i < VID_PRESET_COUNT; i++) {
        if (kVidPresets[i][0] <= workW && kVidPresets[i][1] <= workH) {
            VideoMenu_AddRes(kVidPresets[i][0], kVidPresets[i][1]);
        }
    }
    if (g_dwScreenWidth >= 320 && g_dwScreenHeight >= 240 &&
        g_dwScreenWidth <= workW && g_dwScreenHeight <= workH) {
        VideoMenu_AddRes(g_dwScreenWidth, g_dwScreenHeight);
    }
    if (s_vidResCount == 0) {
        VideoMenu_AddRes(640, 480);
    }
}

// The entry matching the window size, else the largest one not bigger than it.
static int VideoMenu_CurrentResIndex(void)
{
    int best = 0;
    for (int i = 0; i < s_vidResCount; i++) {
        if (s_vidResW[i] == g_dwScreenWidth && s_vidResH[i] == g_dwScreenHeight) return i;
        if (s_vidResW[i] <= g_dwScreenWidth && s_vidResH[i] <= g_dwScreenHeight) best = i;
    }
    return best;
}

// ============================================================================
// State. The pending values are what the rows show; nothing changes until
// APPLY copies them into the real settings.
// ============================================================================
enum {
    VID_ROW_DISPLAY = 0,
    VID_ROW_RESOLUTION,
    VID_ROW_ASPECT,
    VID_ROW_SMAA,
    VID_ROW_CRT,
    VID_ROW_FPS,        // mod: interpolated 60fps ([Display] Interpolate60)
    VID_ROW_APPLY,
    VID_ROW_CLOSE,
    VID_ROW_COUNT
};

static int s_vidCursor     = 0;
static int s_vidPendFull   = 0;
static int s_vidPendRes    = 0;
static int s_vidPendAspect = 0;
static int s_vidPendSmaa   = 0;      // 0 = off, 1 = SMAA
static int s_vidPendCrt    = 0;
static int s_vidPendFps    = 0;
extern BOOL g_bInterpolate60;       // Rendering.cpp

static const char* const kVidSmaaNames[] = { "OFF", "ON" };
#define VID_SMAA_COUNT 2
static int s_vidPrevKeys   = 0;

static const char* const kVidAspectNames[MARNI_ASPECT_COUNT] = {
    "NORMAL",        // MARNI_ASPECT_STRETCH (no longer offered)
    "NORMAL",        // MARNI_ASPECT_4_3
    "NORMAL",        // MARNI_ASPECT_INTEGER (no longer offered)
    "WIDESCREEN",    // MARNI_ASPECT_WIDE (16:9 pan-and-scan)
};
// Only Normal (4:3) and Widescreen are offered: every step toggles.
static int VidAspectToggle(int a)
{
    return a == MARNI_ASPECT_WIDE ? MARNI_ASPECT_4_3 : MARNI_ASPECT_WIDE;
}

static void VideoMenu_Open(void)
{
    VideoMenu_BuildResList();
    s_vidCursor     = 0;
    s_vidPendFull   = g_bFullScreen ? 1 : 0;
    s_vidPendRes    = VideoMenu_CurrentResIndex();
    s_vidPendAspect = (g_dwAspectMode == MARNI_ASPECT_WIDE) ? MARNI_ASPECT_WIDE : MARNI_ASPECT_4_3;
    s_vidPendSmaa   = g_bSmaa ? 1 : 0;
    s_vidPendCrt    = g_bCrtShader ? 1 : 0;
    s_vidPendFps    = g_bInterpolate60 ? 1 : 0;
    s_vidPrevKeys   = VideoMenu_SampleKeys();
}

static void VideoMenu_Apply(void)
{
    const BOOL  full = s_vidPendFull ? TRUE : FALSE;
    const DWORD w    = s_vidResW[s_vidPendRes];
    const DWORD h    = s_vidResH[s_vidPendRes];
    const BOOL modeChanged = (full != (g_bFullScreen ? TRUE : FALSE)) ||
                             (!full && (w != g_dwScreenWidth || h != g_dwScreenHeight));

    // Aspect first: the resize below recomputes the content rectangle with it.
    g_dwAspectMode = (DWORD)s_vidPendAspect;
    MarniSetAspectMode(s_vidPendAspect);

    // SMAA and the CRT shader rebuild the
    // offscreen scene, so only touch them when they changed.
    const BOOL smaa = s_vidPendSmaa ? TRUE : FALSE;
    if (smaa != g_bSmaa) {
        g_bSmaa = smaa;
        crashlog_mark("video menu: apply SMAA");
        MarniSetSmaa(smaa);
    }
    const BOOL crt = s_vidPendCrt ? TRUE : FALSE;
    if (crt != g_bCrtShader) {
        g_bCrtShader = crt;
        crashlog_mark("video menu: apply CRT");
        MarniSetCrtShader(crt);
    }
    g_bInterpolate60 = s_vidPendFps ? TRUE : FALSE;
    crashlog_mark("video menu: apply done");

    // Width/Height stay the WINDOWED size while in fullscreen, so switching
    // back later restores the window the player had.
    if (!full) {
        g_dwScreenWidth  = w;
        g_dwScreenHeight = h;
    }
    const BOOL wasFull = g_bFullScreen ? TRUE : FALSE;
    g_bFullScreen = full;
    if (modeChanged) {
        plat_apply_video_mode(g_dwScreenWidth, g_dwScreenHeight, g_bFullScreen);
    }

    // Keep what start-up derives from the mode in step: CMarniDirect3D's
    // flag (MarniSystem.cpp, from g_bFullScreen) and the cursor, which
    // InitializeMarniSystem hides once in fullscreen. ShowCursor is a counter,
    // so only toggle it when the mode really flipped.
    CMarniDirect3D* pD3D = (CMarniDirect3D*)g_pMarniDirect3D;
    if (pD3D != NULL) {
        pD3D->m_isFullScreen = full;
    }
    if (full != wasFull) {
        plat_cursor_show(full ? FALSE : TRUE);
    }

    ConfigFile_Save();
}



// ============================================================================
// Port-added: the title screen's OPTIONS (TitleScreen.cpp, the scrolling
// menu's fourth line) opens Option Mode (GAME PAD / KEYBOARD / VIDEO / SOUND /
// GAMEPLAY) and chains back to title_state when it is left.
// ============================================================================
#include "OptionsMenu.h"            // options_menu (CONTROLS)

extern DWORD g_dwMusicVolume;       // ConfigFile.cpp, [Sound] MusicVolume 0-10
extern DWORD g_dwEffectsVolume;     // ConfigFile.cpp, [Sound] EffectsVolume 0-10
extern void  MarniSound_SetVolumes(int musicStep, int sfxStep);   // MarniSound.cpp
extern int   g_optFromTitle;        // OptionsMenu.cpp
extern void  title_state(void);     // TitleScreen.cpp

static void TitleOpt_FadeIn(void)
{
    g_main_state_flags = (g_main_state_flags & ~MSF_SCREEN_MODE_MASK) | MSF_SCREEN_STANDALONE;
    g_fade_type_id = 2;
    g_fading_counter = (short)0xF000;
    fade_update();
}

static void title_controls(void)
{
    // The PC release's own configuration screen, as the in-game Option Mode
    // opens it (check_menus_state): a task of its own that suspends this one
    // (task 0) until it exits, resuming us.
    g_optFromTitle = 1;
    Task_execute(1, (void*)options_menu);
    Task_sleep(1);
    g_optFromTitle = 0;
    ConfigFile_Save();          // the bindings it changed
    TitleOpt_FadeIn();
}


// Mod: the title's OPTIONS opens Option Mode directly (GAME PAD / KEYBOARD /
// VIDEO / SOUND tabs), then returns to the title.
void title_options_state(void)
{
    TitleOpt_FadeIn();
    title_controls();
    g_main_state_flags = (g_main_state_flags & ~MSF_SCREEN_MODE_MASK) | MSF_SCREEN_STANDALONE;
    Task_chain((void*)title_state);
}


// ============================================================================
// Port-added: the VIDEO and SOUND tabs of the PC Option Mode screen
// (OptionsMenu.cpp). The rows are drawn there, in the screen's own font; this
// side owns the settings, with pending values that APPLY makes real.
//
//   VIDEO: DISPLAY / RES. / ASPECT / AA / CRT / 60 FPS / APPLY  (APPLY makes them real)
//   SOUND: MUSIC / EFFECTS, 0-10, live, saved when the tab is left
//
// LEFT/RIGHT (or ACTION) change a value, UP/DOWN pick a row, CANCEL/ESC
// leaves the tab (unapplied video changes are dropped).
// ============================================================================
static int s_optTabCursor = 0;
static int s_optTabPrev   = 0;

extern DWORD g_dwVoiceVolume;       // ConfigFile.cpp, [Sound] VoiceVolume 0-10
extern void  MarniSound_SetVoiceVolume(int step);
extern bool  g_bQuickKnife;          // ConfigFile.cpp, [Mods] QuickKnife
extern bool  g_bQuickTurn;           // ConfigFile.cpp, [Mods] QuickTurn
extern bool  g_bReloadButton;        // ConfigFile.cpp, [Mods] Reload
int OptTab_RowCount(int tab) { return tab == 0 ? 7 : tab == 1 ? 3 : 3; }
int OptTab_Cursor(void)      { return s_optTabCursor; }

void OptTab_Open(int tab)
{
    if (tab == 0) {
        VideoMenu_Open();
    }
    s_optTabCursor = 0;
    s_optTabPrev = VideoMenu_SampleKeys();
}

const char* OptTab_Label(int tab, int row)
{
    static const char* const kVideo[7] = { "DISPLAY", "RES.", "ASPECT", "AA", "CRT", "60 FPS", "APPLY" };
    static const char* const kSound[3] = { "MUSIC", "EFFECTS", "VOICE" };
    if (tab == 2) return row == 0 ? "QUICK KNIFE" : row == 1 ? "QUICK TURN" : "RELOAD";
    return tab == 0 ? kVideo[row] : kSound[row];
}

// The row's current value ("" for APPLY).
void OptTab_Value(int tab, int row, char* out, int size)
{
    out[0] = 0;
    if (tab == 2) {
        snprintf(out, size, "%s", (row == 0 ? g_bQuickKnife :
                                   row == 1 ? g_bQuickTurn : g_bReloadButton) ? "ON" : "OFF");
        return;
    }
    if (tab != 0) {
        snprintf(out, size, "%u/10", (unsigned)(row == 0 ? g_dwMusicVolume :
                                                row == 1 ? g_dwEffectsVolume : g_dwVoiceVolume));
        return;
    }
    switch (row) {
    case VID_ROW_DISPLAY:
        snprintf(out, size, "%s", s_vidPendFull ? "FULLSCREEN" : "WINDOWED");
        break;
    case VID_ROW_RESOLUTION:
        if (s_vidPendFull) {
            DWORD dw = 0, dh = 0;
            plat_display_size(&dw, &dh);
            snprintf(out, size, "%uX%u", (unsigned)dw, (unsigned)dh);
        } else if (s_vidResCount > 0) {
            snprintf(out, size, "%uX%u", (unsigned)s_vidResW[s_vidPendRes],
                     (unsigned)s_vidResH[s_vidPendRes]);
        }
        break;
    case VID_ROW_ASPECT: snprintf(out, size, "%s", kVidAspectNames[s_vidPendAspect]); break;
    case VID_ROW_SMAA:   snprintf(out, size, "%s", kVidSmaaNames[s_vidPendSmaa]); break;
    case VID_ROW_CRT:    snprintf(out, size, "%s", s_vidPendCrt ? "ON" : "OFF"); break;
    case VID_ROW_FPS:    snprintf(out, size, "%s", s_vidPendFps ? "ON" : "OFF"); break;
    }
}

static void OptTab_Step(int tab, int row, int step)
{
    if (tab == 2) {
        if (row == 0) g_bQuickKnife = !g_bQuickKnife;
        else if (row == 1) g_bQuickTurn = !g_bQuickTurn;
        else          g_bReloadButton = !g_bReloadButton;
        return;
    }
    if (tab != 0) {
        DWORD* v = (row == 0) ? &g_dwMusicVolume : (row == 1) ? &g_dwEffectsVolume : &g_dwVoiceVolume;
        int nv = (int)*v + step;
        if (nv < 0) nv = 0;
        if (nv > 10) nv = 10;
        *v = (DWORD)nv;
        MarniSound_SetVolumes((int)g_dwMusicVolume, (int)g_dwEffectsVolume);
        MarniSound_SetVoiceVolume((int)g_dwVoiceVolume);
        return;
    }
    switch (row) {
    case VID_ROW_DISPLAY: s_vidPendFull = !s_vidPendFull; break;
    case VID_ROW_RESOLUTION:
        if (!s_vidPendFull && s_vidResCount > 0)
            s_vidPendRes = (s_vidPendRes + s_vidResCount + step) % s_vidResCount;
        break;
    case VID_ROW_ASPECT:
        s_vidPendAspect = VidAspectToggle(s_vidPendAspect);
        break;
    case VID_ROW_SMAA:
        s_vidPendSmaa = (s_vidPendSmaa + VID_SMAA_COUNT + step) % VID_SMAA_COUNT;
        break;
    case VID_ROW_CRT: s_vidPendCrt = !s_vidPendCrt; break;
    case VID_ROW_FPS: s_vidPendFps = !s_vidPendFps; break;
    }
}

// One frame of input. Returns 1 when the tab is left.
int OptTab_Update(int tab)
{
    const int rows = OptTab_RowCount(tab);
    const int keys = VideoMenu_SampleKeys();
    const int edge = keys & ~s_optTabPrev;
    s_optTabPrev = keys;

    if (edge & VIDKEY_ESC) {
        if (tab != 0) ConfigFile_Save();
        play_sfx(3, 5, 0);
        return 1;
    }
    if (edge & VIDKEY_UP)   { s_optTabCursor = (s_optTabCursor + rows - 1) % rows; play_sfx(3, 4, 0); }
    if (edge & VIDKEY_DOWN) { s_optTabCursor = (s_optTabCursor + 1) % rows;        play_sfx(3, 4, 0); }

    const int step = (edge & VIDKEY_RIGHT) ? 1 : (edge & VIDKEY_LEFT) ? -1 : 0;
    const int isApply = (tab == 0 && s_optTabCursor == VID_ROW_APPLY);
    if (step != 0 && !isApply) {
        OptTab_Step(tab, s_optTabCursor, step);
        play_sfx(3, 4, 0);
    }
    if (edge & VIDKEY_CONFIRM) {
        play_sfx(3, 6, 0);
        if (isApply) {
            VideoMenu_Apply();
            VideoMenu_BuildResList();       // the new size may change the list
            s_vidPendRes = VideoMenu_CurrentResIndex();
        } else {
            OptTab_Step(tab, s_optTabCursor, 1);
        }
    }
    return 0;
}

// Mod: Option Mode's QUIT GAME confirm. LEFT/RIGHT move between YES (0) and
// NO (1). Returns 1 for YES, 2 for NO / cancel, 0 while undecided.
void OptConfirm_Open(void)
{
    s_optTabPrev = VideoMenu_SampleKeys();
}

int OptConfirm_Update(int* sel)
{
    const int keys = VideoMenu_SampleKeys();
    const int edge = keys & ~s_optTabPrev;
    s_optTabPrev = keys;
    if (edge & VIDKEY_ESC) { play_sfx(3, 5, 0); return 2; }
    if (edge & (VIDKEY_LEFT | VIDKEY_RIGHT)) { *sel = *sel ? 0 : 1; play_sfx(3, 4, 0); }
    if (edge & VIDKEY_CONFIRM) {
        play_sfx(3, *sel == 0 ? 6 : 5, 0);
        return *sel == 0 ? 1 : 2;
    }
    return 0;
}
