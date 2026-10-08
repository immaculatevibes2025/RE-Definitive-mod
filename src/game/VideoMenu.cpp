// VideoMenu.cpp - F2 Video Options menu (port-only)
//
// Not in the original binary. A pause overlay, built on the same pieces as the
// F1 debug menu (DebugMenu.cpp) but available in every build: F2 freezes
// gameplay and opens
//
//          - VIDEO OPTIONS -
//    >DISPLAY         < WINDOWED >
//     RESOLUTION     < 1280 X 960 >
//     ASPECT RATIO       < 4:3 >
//     ANTI-ALIASING      < OFF >
//     CRT SHADER         < OFF >
//     APPLY
//     CLOSE
//
// DISPLAY       WINDOWED or FULLSCREEN. Fullscreen is borderless on the
//               window's monitor at the monitor's own resolution, as the port
//               has always done it (MarniDX::Create).
// RESOLUTION    The window's client size, from a preset list cut down to what
//               fits the monitor's work area. In fullscreen it shows the
//               native resolution and cannot be changed.
// ASPECT RATIO  How the 4:3 picture fills the window (MarniDX::SetAspectMode):
//               STRETCH fills it (the port's behaviour so far), 4:3 keeps the
//               shape with black bars, 4:3 INTEGER also keeps every game pixel
//               the same size (whole multiples of 320x240).
// ANTI-ALIASING MSAA: OFF, 2X, 4X or 8X. On a GPU that cannot do the count
//               asked for, the renderer quietly uses the highest one it can.
// CRT SHADER    A CRT-Royale style shader (MarniDX.cpp, g_CrtPS_Source):
//               scanlines, aperture-grille mask, halation and curvature.
//
// Changes are only made on APPLY, which also writes config.ini ([Display]
// FullScreen / Width / Height / Aspect - the keys the game owns and rewrites
// on exit anyway) and closes the menu so the next frame redraws the scene at
// the new size. F2, ESC or CLOSE leave without changing anything.
//
// Rendering goes through the pending-sprite queue (draw_rect + PrintText8x8,
// the 8x8 ASCII font region of fontus.tim), flushed with the rest of the frame,
// exactly like the debug menu. While it is open game_loop skips the room
// scripts, update_entities and the scene render, and PlayerPad_Update blanks
// the published pad state (g_videoMenuOpen, alongside g_debugMenuOpen).
// ============================================================================
#include "../Globals.h"
#include "../platform/platform.h"
#include "../marni/MarniDX.h"        // MARNI_ASPECT_*
#include "../marni/MarniSystem.h"    // MarniSetAspectMode, MarniSetMsaa, MarniSetCrtShader
#include "../system/ConfigFile.h"    // ConfigFile_Save
#include <cstdio>
#include <cstring>

int g_videoMenuOpen = 0;             // 1 while the overlay is up (game_loop pauses on it)

// ============================================================================
// Input - the debug menu's scheme with F2 as the toggle: arrow keys / ENTER /
// SPACE / ESC, and the pad through g_RawPadHeld, the function-level word that
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
    VID_ROW_MSAA,
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
static int s_vidPendMsaa   = 0;      // index into kVidMsaaSamples
static int s_vidPendCrt    = 0;
static int s_vidPendFps    = 0;
extern BOOL g_bInterpolate60;       // Rendering.cpp

static const int kVidMsaaSamples[] = { 1, 2, 4, 8 };
static const char* const kVidMsaaNames[] = { "OFF", "2X", "4X", "8X" };
#define VID_MSAA_COUNT 4

static int VideoMenu_MsaaIndex(DWORD samples)
{
    for (int i = 0; i < VID_MSAA_COUNT; i++) {
        if ((DWORD)kVidMsaaSamples[i] == samples) return i;
    }
    return 0;
}
static int s_vidPrevKeys   = 0;
static int s_vidPrevF2     = 0;

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
    s_vidPendMsaa   = VideoMenu_MsaaIndex(g_dwMsaa);
    s_vidPendCrt    = g_bCrtShader ? 1 : 0;
    s_vidPendFps    = g_bInterpolate60 ? 1 : 0;
    s_vidPrevKeys   = VideoMenu_SampleKeys();
    g_videoMenuOpen = 1;
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

    // MSAA and the CRT shader rebuild the
    // offscreen scene, so only touch them when they changed.
    const DWORD msaa = (DWORD)kVidMsaaSamples[s_vidPendMsaa];
    if (msaa != g_dwMsaa) {
        g_dwMsaa = msaa;
        crashlog_mark("video menu: apply MSAA");
        MarniSetMsaa((int)msaa);
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
// Rendering - the debug menu's translucent navy box and 8x8 text.
// ============================================================================
#define VIDCOL_TEXT  0x00    // full-brightness white
#define VIDCOL_HINT  0x03    // light grey
#define VIDCOL_DIM   0xF0    // 50% grey - a row that cannot change
#define VIDCOL_GREEN 0x01    // the row being edited

#define VID_BOX_X 24
#define VID_BOX_Y 52
#define VID_BOX_W 272
#define VID_BOX_H 180

static void VideoMenu_DrawBox(void)
{
    static RectDrawDesc box;
    box.x = VID_BOX_X - g_ScreenOffsetX;
    box.y = VID_BOX_Y - g_ScreenOffsetY;
    box.w = VID_BOX_W;
    box.h = VID_BOX_H;

    box.textureId = 0x70000000;         // variant 3: semi-transparent black
    box.r = 0;
    box.g = 96;                         // -> alpha 96
    box.b = 0;
    draw_rect(&box, 100, 1);

    box.textureId = 0x40000000;         // variant 1: semi-transparent tint
    box.r = 0;
    box.g = 0;
    box.b = 80;                         // -> alpha 80
    draw_rect(&box, 100, 1);
}

static void VideoMenu_Print(int x, int y, const char* text, unsigned char color)
{
    sprintf(PRINT_TEXT_BUFFER, "%s", text);
    PrintText8x8((short)x, (short)y, color, 0);
}

static void VideoMenu_PrintCentered(int y, const char* text, unsigned char color)
{
    VideoMenu_Print(VID_BOX_X + (VID_BOX_W - (int)strlen(text) * 8) / 2, y, text, color);
}

// Values sit centred in the column right of the labels (x 140..292).
static void VideoMenu_PrintValue(int y, const char* value, int changeable, unsigned char color)
{
    char buf[40];
    if (changeable) {
        snprintf(buf, sizeof(buf), "< %s >", value);
    } else {
        snprintf(buf, sizeof(buf), "%s", value);
    }
    int x = 140 + (152 - (int)strlen(buf) * 8) / 2;
    VideoMenu_Print(x, y, buf, color);
}

static void VideoMenu_Draw(void)
{
    static const int kRowY[VID_ROW_COUNT] = { 80, 92, 104, 116, 128, 140, 156, 168 };
    char value[32];

    VideoMenu_DrawBox();
    VideoMenu_PrintCentered(62, "- VIDEO OPTIONS -", VIDCOL_TEXT);

    for (int row = 0; row < VID_ROW_COUNT; row++) {
        const int y = kRowY[row];
        const int sel = (row == s_vidCursor);
        if (sel) {
            VideoMenu_Print(28, y, ">", VIDCOL_TEXT);
        }
        switch (row) {
        case VID_ROW_DISPLAY:
            VideoMenu_Print(40, y, "DISPLAY", VIDCOL_TEXT);
            VideoMenu_PrintValue(y, s_vidPendFull ? "FULLSCREEN" : "WINDOWED", 1,
                                 sel ? VIDCOL_GREEN : VIDCOL_TEXT);
            break;
        case VID_ROW_RESOLUTION:
            if (s_vidPendFull) {
                DWORD dw = 0, dh = 0;
                plat_display_size(&dw, &dh);
                snprintf(value, sizeof(value), "NATIVE %uX%u", (unsigned)dw, (unsigned)dh);
                VideoMenu_Print(40, y, "RESOLUTION", VIDCOL_DIM);
                VideoMenu_PrintValue(y, value, 0, VIDCOL_DIM);
            } else {
                snprintf(value, sizeof(value), "%u X %u",
                         (unsigned)s_vidResW[s_vidPendRes], (unsigned)s_vidResH[s_vidPendRes]);
                VideoMenu_Print(40, y, "RESOLUTION", VIDCOL_TEXT);
                VideoMenu_PrintValue(y, value, 1, sel ? VIDCOL_GREEN : VIDCOL_TEXT);
            }
            break;
        case VID_ROW_ASPECT:
            VideoMenu_Print(40, y, "ASPECT RATIO", VIDCOL_TEXT);
            VideoMenu_PrintValue(y, kVidAspectNames[s_vidPendAspect], 1,
                                 sel ? VIDCOL_GREEN : VIDCOL_TEXT);
            break;
        case VID_ROW_MSAA:
            VideoMenu_Print(40, y, "ANTI-ALIASING", VIDCOL_TEXT);
            VideoMenu_PrintValue(y, kVidMsaaNames[s_vidPendMsaa], 1,
                                 sel ? VIDCOL_GREEN : VIDCOL_TEXT);
            break;
        case VID_ROW_CRT:
            VideoMenu_Print(40, y, "CRT SHADER", VIDCOL_TEXT);
            VideoMenu_PrintValue(y, s_vidPendCrt ? "ON" : "OFF", 1,
                                 sel ? VIDCOL_GREEN : VIDCOL_TEXT);
            break;
        case VID_ROW_FPS:
            VideoMenu_Print(40, y, "FRAME RATE", VIDCOL_TEXT);
            VideoMenu_PrintValue(y, s_vidPendFps ? "60 FPS" : "30 FPS", 1,
                                 sel ? VIDCOL_GREEN : VIDCOL_TEXT);
            break;
        case VID_ROW_APPLY:
            VideoMenu_Print(40, y, "APPLY", VIDCOL_TEXT);
            break;
        case VID_ROW_CLOSE:
            VideoMenu_Print(40, y, "CLOSE", VIDCOL_TEXT);
            break;
        }
    }

    VideoMenu_PrintCentered(192, "LEFT/RIGHT: CHANGE", VIDCOL_HINT);
    VideoMenu_PrintCentered(204, "ENTER/ACTION: SELECT", VIDCOL_HINT);
    VideoMenu_PrintCentered(216, "F2/ESC: CLOSE", VIDCOL_HINT);
}

// ============================================================================
// video_menu_overlay
// Called by game_loop once per frame while the debug menu is closed. Owns the
// F2 edge detection; returns 1 while the menu is open.
// ============================================================================
int video_menu_overlay(void)
{
    const int f2 = (plat_key_state(VK_F2) & 0x8000) != 0 ? 1 : 0;
    const int f2Edge = f2 && !s_vidPrevF2;
    s_vidPrevF2 = f2;

    if (g_videoMenuOpen == 0) {
        if (f2Edge) {
            VideoMenu_Open();
            VideoMenu_Draw();
        }
        return g_videoMenuOpen;
    }

    const int keys = VideoMenu_SampleKeys();
    const int newKeys = keys & ~s_vidPrevKeys;
    s_vidPrevKeys = keys;

    if (f2Edge || (newKeys & VIDKEY_ESC)) {
        g_videoMenuOpen = 0;
        return 0;
    }

    if (newKeys & VIDKEY_UP) {
        s_vidCursor = (s_vidCursor + VID_ROW_COUNT - 1) % VID_ROW_COUNT;
    }
    if (newKeys & VIDKEY_DOWN) {
        s_vidCursor = (s_vidCursor + 1) % VID_ROW_COUNT;
    }

    const int step = (newKeys & VIDKEY_RIGHT) ? 1 : (newKeys & VIDKEY_LEFT) ? -1 : 0;
    if (step != 0) {
        switch (s_vidCursor) {
        case VID_ROW_DISPLAY:
            s_vidPendFull = !s_vidPendFull;
            break;
        case VID_ROW_RESOLUTION:
            if (!s_vidPendFull && s_vidResCount > 0) {
                s_vidPendRes = (s_vidPendRes + s_vidResCount + step) % s_vidResCount;
            }
            break;
        case VID_ROW_ASPECT:
            s_vidPendAspect = VidAspectToggle(s_vidPendAspect);
            break;
        case VID_ROW_MSAA:
            s_vidPendMsaa = (s_vidPendMsaa + VID_MSAA_COUNT + step) % VID_MSAA_COUNT;
            break;
        case VID_ROW_CRT:
            s_vidPendCrt = !s_vidPendCrt;
            break;
        case VID_ROW_FPS:
            s_vidPendFps = !s_vidPendFps;
            break;
        }
    }

    if (newKeys & VIDKEY_CONFIRM) {
        if (s_vidCursor == VID_ROW_APPLY) {
            VideoMenu_Apply();
            g_videoMenuOpen = 0;
            return 0;
        }
        if (s_vidCursor == VID_ROW_CLOSE) {
            g_videoMenuOpen = 0;
            return 0;
        }
        // ENTER on a setting row steps it forward, like RIGHT.
        if (s_vidCursor == VID_ROW_DISPLAY) {
            s_vidPendFull = !s_vidPendFull;
        } else if (s_vidCursor == VID_ROW_RESOLUTION && !s_vidPendFull && s_vidResCount > 0) {
            s_vidPendRes = (s_vidPendRes + 1) % s_vidResCount;
        } else if (s_vidCursor == VID_ROW_ASPECT) {
            s_vidPendAspect = VidAspectToggle(s_vidPendAspect);
        } else if (s_vidCursor == VID_ROW_MSAA) {
            s_vidPendMsaa = (s_vidPendMsaa + 1) % VID_MSAA_COUNT;
        } else if (s_vidCursor == VID_ROW_CRT) {
            s_vidPendCrt = !s_vidPendCrt;
        } else if (s_vidCursor == VID_ROW_FPS) {
            s_vidPendFps = !s_vidPendFps;
        }
    }

    VideoMenu_Draw();
    return 1;
}

// ============================================================================
// Port-added: the title screen's OPTIONS (TitleScreen.cpp, the scrolling
// menu's fourth line). A standalone screen in this menu's style:
//
//          - OPTIONS -
//    >VIDEO OPTIONS      the F2 Video Options menu above, run on its own
//     SOUND OPTIONS      MUSIC / EFFECTS volume, 0-10 (config.ini [Sound])
//     CONTROLS           the PC release's own key / joypad configuration
//     BACK               (OptionsMenu.cpp, g_optFromTitle: no character demo)
//
// UP/DOWN pick, ENTER/ACTION select, ESC/CANCEL back. Leaving chains back to
// title_state, which rebuilds the title from scratch.
// ============================================================================
#include "OptionsMenu.h"            // options_menu (CONTROLS)

extern DWORD g_dwMusicVolume;       // ConfigFile.cpp, [Sound] MusicVolume 0-10
extern DWORD g_dwEffectsVolume;     // ConfigFile.cpp, [Sound] EffectsVolume 0-10
extern void  MarniSound_SetVolumes(int musicStep, int sfxStep);   // MarniSound.cpp
extern int   g_optFromTitle;        // OptionsMenu.cpp
extern void  title_state(void);     // TitleScreen.cpp

static void TitleOpt_Black(void)
{
    static RectDrawDesc bg;
    bg.textureId = 0;
    bg.x = -160;
    bg.y = -120;
    bg.w = 320;
    bg.h = 240;
    bg.r = bg.g = bg.b = 0;
    draw_rect(&bg, 4000, 1);
}

static void TitleOpt_FadeIn(void)
{
    g_main_state_flags = (g_main_state_flags & ~MSF_SCREEN_MODE_MASK) | MSF_SCREEN_STANDALONE;
    g_fade_type_id = 2;
    g_fading_counter = (short)0xF000;
    fade_update();
}

// A 0-10 volume as a bar: "[#####-----]".
static void TitleOpt_Bar(char* out, int size, int v)
{
    char bar[16];
    for (int i = 0; i < 10; i++) bar[i] = (i < v) ? '#' : '-';
    bar[10] = 0;
    snprintf(out, size, "%s %2d", bar, v);
}

static void title_sound_options(void)
{
    enum { ROW_MUSIC, ROW_EFFECTS, ROW_BACK, ROW_COUNT };
    static const int kY[ROW_COUNT] = { 100, 116, 140 };
    int cursor = 0;
    int prev = VideoMenu_SampleKeys();
    for (;;) {
        TitleOpt_Black();
        VideoMenu_DrawBox();
        VideoMenu_PrintCentered(74, "- SOUND OPTIONS -", VIDCOL_TEXT);
        char value[32];
        for (int row = 0; row < ROW_COUNT; row++) {
            const int sel = (row == cursor);
            if (sel) VideoMenu_Print(28, kY[row], ">", VIDCOL_TEXT);
            if (row == ROW_BACK) {
                VideoMenu_Print(40, kY[row], "BACK", VIDCOL_TEXT);
                continue;
            }
            const int v = (int)(row == ROW_MUSIC ? g_dwMusicVolume : g_dwEffectsVolume);
            VideoMenu_Print(40, kY[row], row == ROW_MUSIC ? "MUSIC" : "EFFECTS", VIDCOL_TEXT);
            TitleOpt_Bar(value, sizeof(value), v);
            VideoMenu_PrintValue(kY[row], value, 1, sel ? VIDCOL_GREEN : VIDCOL_TEXT);
        }
        VideoMenu_PrintCentered(170, "LEFT/RIGHT: CHANGE", VIDCOL_HINT);
        VideoMenu_PrintCentered(182, "ENTER/ACTION: SELECT", VIDCOL_HINT);
        VideoMenu_PrintCentered(194, "ESC: BACK", VIDCOL_HINT);

        Task_sleep(1);

        const int keys = VideoMenu_SampleKeys();
        const int edge = keys & ~prev;
        prev = keys;
        if (edge & VIDKEY_UP)   cursor = (cursor + ROW_COUNT - 1) % ROW_COUNT;
        if (edge & VIDKEY_DOWN) cursor = (cursor + 1) % ROW_COUNT;
        const int step = (edge & VIDKEY_RIGHT) ? 1 : (edge & VIDKEY_LEFT) ? -1 : 0;
        if (step != 0 && cursor != ROW_BACK) {
            DWORD* v = (cursor == ROW_MUSIC) ? &g_dwMusicVolume : &g_dwEffectsVolume;
            int nv = (int)*v + step;
            if (nv < 0) nv = 0;
            if (nv > 10) nv = 10;
            *v = (DWORD)nv;
            MarniSound_SetVolumes((int)g_dwMusicVolume, (int)g_dwEffectsVolume);
        }
        if ((edge & VIDKEY_ESC) || ((edge & VIDKEY_CONFIRM) && cursor == ROW_BACK)) {
            ConfigFile_Save();
            return;
        }
    }
}

static void title_video_options(void)
{
    VideoMenu_Open();
    for (;;) {
        TitleOpt_Black();
        if (video_menu_overlay() == 0) break;     // APPLY / CLOSE / ESC
        Task_sleep(1);
    }
    g_videoMenuOpen = 0;
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

static void title_options_hub(void);

// Mod: the title's OPTIONS opens Option Mode directly (GAME PAD / KEYBOARD /
// VIDEO / SOUND tabs), then returns to the title.
void title_options_state(void)
{
    TitleOpt_FadeIn();
    title_controls();
    g_main_state_flags = (g_main_state_flags & ~MSF_SCREEN_MODE_MASK) | MSF_SCREEN_STANDALONE;
    Task_chain((void*)title_state);
}

// The old OPTIONS hub, no longer used.
static void title_options_hub(void)
{
    enum { ROW_VIDEO, ROW_SOUND, ROW_CONTROLS, ROW_BACK, ROW_COUNT };
    static const char* const kLabel[ROW_COUNT] = {
        "VIDEO OPTIONS", "SOUND OPTIONS", "CONTROLS", "BACK"
    };
    static const int kY[ROW_COUNT] = { 96, 110, 124, 146 };

    TitleOpt_FadeIn();
    int cursor = 0;
    int prev = VideoMenu_SampleKeys();
    for (;;) {
        TitleOpt_Black();
        VideoMenu_DrawBox();
        VideoMenu_PrintCentered(74, "- OPTIONS -", VIDCOL_TEXT);
        for (int row = 0; row < ROW_COUNT; row++) {
            if (row == cursor) VideoMenu_Print(28, kY[row], ">", VIDCOL_TEXT);
            VideoMenu_Print(40, kY[row], kLabel[row], row == cursor ? VIDCOL_GREEN : VIDCOL_TEXT);
        }
        VideoMenu_PrintCentered(182, "ENTER/ACTION: SELECT", VIDCOL_HINT);
        VideoMenu_PrintCentered(194, "ESC: BACK", VIDCOL_HINT);

        Task_sleep(1);

        const int keys = VideoMenu_SampleKeys();
        const int edge = keys & ~prev;
        prev = keys;
        if (edge & VIDKEY_UP)   cursor = (cursor + ROW_COUNT - 1) % ROW_COUNT;
        if (edge & VIDKEY_DOWN) cursor = (cursor + 1) % ROW_COUNT;
        if (edge & VIDKEY_ESC) break;
        if (edge & VIDKEY_CONFIRM) {
            if (cursor == ROW_BACK) break;
            if (cursor == ROW_VIDEO)    title_video_options();
            if (cursor == ROW_SOUND)    title_sound_options();
            if (cursor == ROW_CONTROLS) title_controls();
            prev = VideoMenu_SampleKeys();      // no re-fire on the way back
        }
    }

    g_main_state_flags = (g_main_state_flags & ~MSF_SCREEN_MODE_MASK) | MSF_SCREEN_STANDALONE;
    Task_chain((void*)title_state);
}

// ============================================================================
// Port-added: the VIDEO and SOUND tabs of the PC Option Mode screen
// (OptionsMenu.cpp). The rows are drawn there, in the screen's own font; this
// side owns the settings, using the same pending/APPLY logic as the F2 menu.
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
extern bool  g_bModTicks;           // Globals.cpp, [Mods] Ticks
extern bool  g_bQuickKnife;          // ConfigFile.cpp, [Mods] QuickKnife
extern bool  g_bQuickTurn;           // ConfigFile.cpp, [Mods] QuickTurn
extern bool  g_bReloadButton;        // ConfigFile.cpp, [Mods] Reload
int OptTab_RowCount(int tab) { return tab == 0 ? 7 : tab == 1 ? 3 : 4; }
int OptTab_Cursor(void)      { return s_optTabCursor; }

void OptTab_Open(int tab)
{
    if (tab == 0) {
        VideoMenu_Open();
        g_videoMenuOpen = 0;            // the tab, not the F2 overlay
    }
    s_optTabCursor = 0;
    s_optTabPrev = VideoMenu_SampleKeys();
}

const char* OptTab_Label(int tab, int row)
{
    static const char* const kVideo[7] = { "DISPLAY", "RES.", "ASPECT", "AA", "CRT", "60 FPS", "APPLY" };
    static const char* const kSound[3] = { "MUSIC", "EFFECTS", "VOICE" };
    if (tab == 2) return row == 0 ? "TICKS" : row == 1 ? "QUICK KNIFE" : row == 2 ? "QUICK TURN" : "RELOAD";
    return tab == 0 ? kVideo[row] : kSound[row];
}

// The row's current value ("" for APPLY).
void OptTab_Value(int tab, int row, char* out, int size)
{
    out[0] = 0;
    if (tab == 2) {
        snprintf(out, size, "%s", (row == 0 ? g_bModTicks : row == 1 ? g_bQuickKnife :
                                   row == 2 ? g_bQuickTurn : g_bReloadButton) ? "ON" : "OFF");
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
    case VID_ROW_MSAA:   snprintf(out, size, "%s", kVidMsaaNames[s_vidPendMsaa]); break;
    case VID_ROW_CRT:    snprintf(out, size, "%s", s_vidPendCrt ? "ON" : "OFF"); break;
    case VID_ROW_FPS:    snprintf(out, size, "%s", s_vidPendFps ? "ON" : "OFF"); break;
    }
}

static void OptTab_Step(int tab, int row, int step)
{
    if (tab == 2) {
        if (row == 0) g_bModTicks = !g_bModTicks;     // Ticks replace Hunters
        else if (row == 1) g_bQuickKnife = !g_bQuickKnife;
        else if (row == 2) g_bQuickTurn = !g_bQuickTurn;
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
    case VID_ROW_MSAA:
        s_vidPendMsaa = (s_vidPendMsaa + VID_MSAA_COUNT + step) % VID_MSAA_COUNT;
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
