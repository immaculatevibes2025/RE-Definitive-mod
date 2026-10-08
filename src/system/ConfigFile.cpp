// ConfigFile.cpp - config.ini as the single settings store (both builds).
// See ConfigFile.h for the contract.
#include "ConfigFile.h"

#include "../Globals.h"
#include "../DebugPrint.h"
#include "../platform/platform.h"
#include "AssetPath.h"   // SetAssetBase / SetSaveRoot / SetAssetVersion

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_NAME "config.ini"
#define MAX_LINE    1024
#define MAX_LINES   512

#if defined(_WIN32)
#define kPathSep '\\'
#else
#define kPathSep '/'
#endif

namespace {

char s_path[260];
BOOL s_pathResolved = FALSE;

// The keys the game owns and rewrites on exit; everything else in the file is
// the player's and is left alone.
struct OwnedKey {
    const char* section;
    const char* key;
};

const OwnedKey kOwnedKeys[] = {
    { "Display", "FullScreen" },
    { "Display", "Width" },
    { "Display", "Height" },
    { "Display", "BitDepth" },
    { "Display", "VSync" },
    { "Player",  "PlayCount" },
    { "Player",  "ClearCount" },
    { "Input",   "KeyDef" },
    { "Input",   "SideDef" },
    { "Display", "Aspect" },     // port-added (Video Options menu)
    { "Display", "MSAA" },       // port-added (Video Options menu)
    { "Display", "CRT" },        // port-added (Video Options menu)
    { "Sound",   "MusicVolume" },    // port-added (title OPTIONS > SOUND OPTIONS)
    { "Sound",   "EffectsVolume" },  // port-added (title OPTIONS > SOUND OPTIONS)
    { "Sound",   "VoiceVolume" },    // port-added (Option Mode > SOUND)
    { "Mods",    "Ticks" },          // port-added (Option Mode > GAMEPLAY)
    { "Mods",    "QuickKnife" },     // port-added (Option Mode > GAMEPLAY)
    { "Mods",    "QuickTurn" },      // port-added (Option Mode > GAMEPLAY)
    { "Mods",    "Reload" },         // port-added (Option Mode > GAMEPLAY)
    { "Input",   "PadDef" },         // port-added: the GAME PAD bindings (g_JoyRemapTbl[1])
    { "Display", "Interpolate60" },  // port-added (Option Mode > VIDEO, F2 menu)
};
const int kOwnedKeyCount = (int)(sizeof(kOwnedKeys) / sizeof(kOwnedKeys[0]));

BOOL FileExists(const char* path)
{
    FILE* f = fopen(path, "r");
    if (f == NULL) return FALSE;
    fclose(f);
    return TRUE;
}

void HexEncode(const BYTE* src, int count, char* out)
{
    static const char* kDigits = "0123456789ABCDEF";
    for (int i = 0; i < count; ++i) {
        out[i * 2]     = kDigits[src[i] >> 4];
        out[i * 2 + 1] = kDigits[src[i] & 0x0F];
    }
    out[count * 2] = '\0';
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void HexDecode(const char* src, BYTE* dst, int count)
{
    for (int i = 0; i < count; ++i) {
        const int hi = HexNibble(src[i * 2]);
        const int lo = HexNibble(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) return;   // leave the rest at their current value
        dst[i] = (BYTE)((hi << 4) | lo);
    }
}

// Trim leading/trailing whitespace in place; returns the trimmed start.
char* Trim(char* s)
{
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' '  || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    return s;
}

// Read `key` from `section`. Sections are tracked by the caller's scan.
BOOL ReadValue(const char* path, const char* section, const char* key,
               char* out, size_t outSize)
{
    FILE* f = fopen(path, "r");
    if (f == NULL) return FALSE;

    char line[MAX_LINE];
    char current[128] = "";
    BOOL found = FALSE;

    while (fgets(line, sizeof(line), f) != NULL) {
        char* p = Trim(line);
        if (*p == ';' || *p == '#' || *p == '\0') continue;

        if (*p == '[') {
            char* end = strchr(p, ']');
            if (end != NULL) {
                size_t n = (size_t)(end - p - 1);
                if (n >= sizeof(current)) n = sizeof(current) - 1;
                memcpy(current, p + 1, n);
                current[n] = '\0';
            }
            continue;
        }

        if (strcmp(current, section) != 0) continue;

        char* eq = strchr(p, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        if (strcmp(Trim(p), key) != 0) continue;

        char* v = Trim(eq + 1);
        strncpy(out, v, outSize - 1);
        out[outSize - 1] = '\0';
        found = TRUE;
        break;
    }
    fclose(f);
    return found;
}

int ReadInt(const char* path, const char* section, const char* key, int fallback)
{
    char buf[64];
    if (!ReadValue(path, section, key, buf, sizeof(buf))) return fallback;
    return atoi(buf);
}

// Resolve a configured folder against the executable's directory. Absolute
// paths - including Windows drive and UNC forms, so a config.ini written on one
// machine still reads sensibly on the other platform - are used as written;
// everything else is joined onto `exeDir`. With no anchor the value is left
// relative to the working directory.
const char* ResolveConfiguredPath(const char* exeDir, const char* value,
                                  char* out, size_t outSize)
{
    if (!plat_path_is_absolute(value) && exeDir != NULL && exeDir[0] != '\0') {
        snprintf(out, outSize, "%s%c%s", exeDir, kPathSep, value);
        return out;
    }
    strncpy(out, value, outSize - 1);
    out[outSize - 1] = '\0';
    return out;
}

// Static line pool for keys that have to be inserted rather than replaced.
char s_newLine[64][700];      // mod: room for every owned key
int  s_newLineUsed = 0;

void SetValue(const char* section, const char* key, const char* value,
              char** lines, int* count)
{
    char current[128] = "";
    int sectionHeader = -1;     // index of our section's [header]
    int sectionEnd = -1;        // index of the next section's header, if any

    for (int i = 0; i < *count; ++i) {
        char* p = Trim(lines[i]);
        if (*p == '[') {
            char* end = strchr(p, ']');
            if (end != NULL) {
                size_t n = (size_t)(end - p - 1);
                if (n >= sizeof(current)) n = sizeof(current) - 1;
                memcpy(current, p + 1, n);
                current[n] = '\0';
            }
            if (strcmp(current, section) == 0) {
                sectionHeader = i;
            } else if (sectionHeader >= 0 && sectionEnd < 0) {
                sectionEnd = i;
            }
            continue;
        }

        if (strcmp(current, section) != 0) continue;
        char* eq = strchr(p, '=');
        if (eq == NULL) continue;

        // Compare the key WITHOUT touching the line: an earlier version wrote
        // '\0' over the '=' while scanning, which silently ate the '=value'
        // part of every comment line and of the keys it passed over.
        char* keyEnd = eq;
        while (keyEnd > p && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) --keyEnd;
        const size_t keyLen = strlen(key);
        if ((size_t)(keyEnd - p) != keyLen || strncmp(p, key, keyLen) != 0) continue;

        // Replace in place.
        if (s_newLineUsed < 64) {
            snprintf(s_newLine[s_newLineUsed], sizeof(s_newLine[0]), "%s=%s", key, value);
            lines[i] = s_newLine[s_newLineUsed++];
        }
        return;
    }

    if (*count + 3 >= MAX_LINES || s_newLineUsed + 3 > 64) return;

    if (sectionHeader < 0) {
        // Unknown section: append it whole.
        snprintf(s_newLine[s_newLineUsed], sizeof(s_newLine[0]), "%s", "");
        lines[(*count)++] = s_newLine[s_newLineUsed++];
        snprintf(s_newLine[s_newLineUsed], sizeof(s_newLine[0]), "[%s]", section);
        lines[(*count)++] = s_newLine[s_newLineUsed++];
        snprintf(s_newLine[s_newLineUsed], sizeof(s_newLine[0]), "%s=%s", key, value);
        lines[(*count)++] = s_newLine[s_newLineUsed++];
        return;
    }

    // Insert the key at the end of its section.
    const int at = (sectionEnd >= 0) ? sectionEnd : *count;
    snprintf(s_newLine[s_newLineUsed], sizeof(s_newLine[0]), "%s=%s", key, value);
    char* newLine = s_newLine[s_newLineUsed++];
    for (int i = *count; i > at; --i) lines[i] = lines[i - 1];
    lines[at] = newLine;
    (*count)++;
}

}  // namespace

// Port-added: [Sound] MusicVolume / EffectsVolume, 0-10 (10 = full). Owned by
// VideoMenu.cpp's SOUND OPTIONS, applied through MarniSound_SetVolumes.
DWORD g_dwMusicVolume = 10;
DWORD g_dwEffectsVolume = 10;
DWORD g_dwVoiceVolume = 10;
DWORD g_dwTestArmorKey = 0;     // [Testing] ArmorKey (GameStart.cpp)
DWORD g_dwTestShieldKey = 0;    // [Testing] ShieldKey (GameStart.cpp)
bool  g_bQuickKnife = false;     // [Mods] QuickKnife (Option Mode > GAMEPLAY)
bool  g_bQuickTurn  = false;     // [Mods] QuickTurn  (Option Mode > GAMEPLAY)
bool  g_bReloadButton = false;   // [Mods] Reload     (Option Mode > GAMEPLAY)
extern BOOL g_bInterpolate60;    // Rendering.cpp: [Display] Interpolate60
bool  g_bTickAlwaysDecap = false; // [Mods] TickAlwaysDecapitate (testing)
bool  g_bBattleTicks = false;     // [BattleGame] Ticks
int   g_testStartCourtyard = 0;   // [Testing] StartCourtyard (GameStart.cpp)
int   g_testStartBeforeYawn = 0;  // [Testing] StartBeforeYawn (GameStart.cpp)
extern void MarniSound_SetVoiceVolume(int step);
extern void MarniSound_SetVolumes(int musicStep, int sfxStep);   // MarniSound.cpp

// ---------------------------------------------------------------------------
const char* ConfigFile_Find(void)
{
    if (s_pathResolved) return s_path;

    // Next to the executable first - that is the folder the asset and save
    // paths default to, so the settings file belongs there too. The
    // working-directory candidates stay as a fallback for launching the
    // development build from the repository root.
    char exeDir[240] = "";
    char exePath[300] = "";
    if (plat_exe_dir(exeDir, sizeof(exeDir))) {
        snprintf(exePath, sizeof(exePath), "%s%c%s", exeDir, kPathSep, CONFIG_NAME);
        if (FileExists(exePath)) {
            strncpy(s_path, exePath, sizeof(s_path) - 1);
            s_path[sizeof(s_path) - 1] = '\0';
            s_pathResolved = TRUE;
            return s_path;
        }
    }

    static const char* kCandidates[] = {
        "./" CONFIG_NAME, CONFIG_NAME, "../" CONFIG_NAME, "../../" CONFIG_NAME,
    };

    for (int i = 0; i < 4; ++i) {
        if (FileExists(kCandidates[i])) {
            strncpy(s_path, kCandidates[i], sizeof(s_path) - 1);
            s_path[sizeof(s_path) - 1] = '\0';
            s_pathResolved = TRUE;
            return s_path;
        }
    }

    // Nothing to read: a fresh file is created next to the executable when we
    // know where that is, otherwise in the working directory.
    const char* target = (exePath[0] != '\0') ? exePath : "./" CONFIG_NAME;
    strncpy(s_path, target, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = '\0';
    s_pathResolved = TRUE;
    return s_path;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// [Game] Mode <-> GAME_MODE_*.
//
// The name doubles as the asset overlay's folder name (SetAssetMode), so these
// two functions are the only place a mode name is spelled out. An unknown or
// missing name falls back to OG rather than refusing to start: a typo should
// give the stock game, not a black screen.
// ---------------------------------------------------------------------------
static const char* const kGameModeNames[] = { "OG", "DC", "SATURN", "DS" };
static const int kGameModeCount =
    (int)(sizeof(kGameModeNames) / sizeof(kGameModeNames[0]));

static const char* GameModeName(int mode)
{
    if (mode < 0 || mode >= kGameModeCount) {
        return kGameModeNames[GAME_MODE_OG];
    }
    return kGameModeNames[mode];
}

static int ParseGameMode(const char* name)
{
    if (name == NULL || name[0] == '\0') {
        return GAME_MODE_OG;
    }
    for (int i = 0; i < kGameModeCount; i++) {
        const char* a = name;
        const char* b = kGameModeNames[i];
        while (*a != '\0' && *b != '\0') {
            char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
            if (ca != *b) break;
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return GAME_MODE_OG;
}

void ConfigFile_EnsureExists(void)
{
    const char* path = ConfigFile_Find();
    if (FileExists(path)) return;

    FILE* f = fopen(path, "w");
    if (f == NULL) {
        dbg_printf("[CONFIG] could not create %s\n", path);
        return;
    }

    char keyHex[32 * 2 + 1];
    char sideHex[128 * 2 + 1];
    HexEncode(g_keyBindingData, (int)sizeof(g_keyBindingData), keyHex);
    HexEncode(g_joystickBindingData, (int)sizeof(g_joystickBindingData), sideHex);

    fprintf(f,
        "; config.ini - Resident Evil PC decomp configuration.\n"
        ";\n"
        "; The [Display], [Player] and [Input] values below are rewritten by the\n"
        "; game on exit. Everything else is yours to edit; comments are kept.\n"
        "\n"
        "[Display]\n"
        "; 0 = windowed, 1 = fullscreen\n"
        "FullScreen=1\n"
        "; Resolution width\n"
        "Width=%u\n"
        "; Resolution height\n"
        "Height=%u\n"
        "; Bits per pixel (16 or 32)\n"
        "BitDepth=%u\n"
        "; Wait for vblank on present. 0 = off (default, and what the original did\n"
        "; in a window): the engine paces itself to 33 ms per tick in software.\n"
        "VSync=%d\n"
        "; Aspect ratio: 1 = Normal (4:3 with black bars), 2 = Widescreen.\n"
        "Aspect=1\n"
        "; Multisample anti-aliasing: 1 = off, 2, 4 or 8 samples.\n"
        "MSAA=1\n"
        "; 1 = CRT shader (CRT style scanlines, phosphor mask, curvature).\n"
        "CRT=0\n"
        "; 1 = interpolated 60fps: game logic stays at 30 ticks/s, an in-between\n"
        "; frame blends character and object movement.\n"
        "Interpolate60=0\n"
        "\n"
        "[Sound]\n"
        "; Music and sound-effect volume, 0 (off) to 10 (full). Also in the title\n"
        "; screen's OPTIONS > SOUND OPTIONS.\n"
        "MusicVolume=%u\n"
        "EffectsVolume=%u\n"
        "VoiceVolume=%u\n"
        "\n"
        "[Assets]\n"
        "; Folder that holds the USA/ and JPN/ data trees. Relative paths are\n"
        "; resolved from the game binary's own directory. Leave it empty to use\n"
        "; that directory itself, i.e. put USA/ next to the executable.\n"
        "Path=\n"
        "; Which tree to run. USA = North American/GOG, JPN = Japanese PC\n"
        "; (Biohazard). Selects the subfolder of Path that every asset uses.\n"
        "Version=%s\n"
        "\n"
        "[Save]\n"
        "; Folder holding savedat*.dat, relative to the binary's directory unless\n"
        "; written absolute. Leave it empty for <Assets Path>/SAVE: with the\n"
        "; default [Assets] Path that is SAVE/ beside the executable, the layout\n"
        "; the original used, and an existing save/ or Save/ is found as well.\n"
        "Path=\n"
        "\n"
        "[Game]\n"
        "; Which release's content to run. This one key selects BOTH the code\n"
        "; branches and the asset overlay folder searched ahead of the [Assets]\n"
        "; tree, so the two can never disagree.\n"
        ";   OG     = the PC release (the original PS1 game's content). Default,\n"
        ";            and the only mode that uses no overlay folder at all.\n"
        ";   DC     = Director's Cut (Japanese MediaKite / SLUS_005.51). Needs a\n"
        ";            DC/ folder beside USA/ holding only the files it changes or\n"
        ";            adds; anything it does not carry falls back to the tree\n"
        ";            [Assets] Version selects.\n"
        "; 1 = use the PS1 staff-credit overlay in the ending FMVs. DC enables it\n"
        "; automatically; OG leaves it off unless this key is set.\n"
        "Ps1EndingCredits=%d\n"
        "; 1 = draw the JPN PS1 subtitle lines over the prologue FMV, as the\n"
        "; Biohazard Director's Cut disc does. Needs the JPN asset tree; the USA\n"
        "; disc has no such subtitles.\n"
        "Ps1FmvSubtitles=%d\n"
        "; 1 = let every FMV be skipped with the usual buttons. The original's\n"
        "; per-movie mask leaves the endings, the staff rolls and two of the\n"
        "; cutscenes unskippable; set this to 1 to make those skippable too.\n"
        "SkipUnskippableFmv=%d\n"
        "\n"
        "[Mods]\n"
        "; 1 = Sega Saturn Ticks in place of the Hunters from the courtyard.\n"
        "; Loads enemy/em1016.emd and em1116.emd (tools/saturn/tick2pc.py), the\n"
        "; Tick's close-range decapitation and sounds (sound/TK_*.wav from\n"
        "; tools/saturn/ticksnd.py). Anything missing falls back to the Hunter.\n"
        "Ticks=0\n"
        "; Option Mode > GAMEPLAY: 1 = on.\n"
        "QuickKnife=0\n"
        "QuickTurn=0\n"
        "Reload=0\n"
        "\n"
        "[BattleGame]\n"
        "AlwaysUnlocked=0\n"
        "\n"
        "[Debug]\n"
        "; Master switch for the port-added debug features: F1 debug menu, F6\n"
        "; texture viewer, F8 collision overlay. 0 = off, 1 = on.\n"
        "EnableDebug=0\n"
        "\n"
        "[Player]\n"
        "PlayCount=%u\n"
        "ClearCount=%u\n"
        "\n"
        "[Input]\n"
        "; Input debugging: InputLog=1 writes input_log.txt; Gamepad=0 ignores controllers.\n"
        "InputLog=0\n"
        "Gamepad=1\n"
        "; Keyboard, side and game-pad bindings as hex (the shipped default layout).\n"
        "KeyDef=5753414400100000000002010009000000000000000000005251451B110D201B\n"
        "SideDef=0010000000400000008000000020000000000000000000000000000000000000800000000000000040000000000800000000000000000000000000000800000000090000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000\n"
        "PadDef=0010000000400000008000000020000000100000004000000080000000200000800000000000040040000000000000000000010000000000000900000008000000000000000000000000020008000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000\n",
        (unsigned)g_dwScreenWidth, (unsigned)g_dwScreenHeight,
        (unsigned)g_dwBitDepth, g_bVSync ? 1 : 0,
        (unsigned)g_dwMusicVolume, (unsigned)g_dwEffectsVolume, (unsigned)g_dwVoiceVolume,
        (GetAssetVersion() == 1) ? "JPN" : "USA",
        g_bPs1EndingCredits ? 1 : 0,
        g_bPs1FmvSubtitles ? 1 : 0,
        g_bSkipUnskippableFmv ? 1 : 0,
        (unsigned)g_dwPlayCount, (unsigned)g_dwClearCount);
    (void)keyHex; (void)sideHex;

    fclose(f);
    dbg_printf("[CONFIG] created %s\n", path);
}

// ---------------------------------------------------------------------------
BOOL ConfigFile_Load(void)
{
    // Anchor everything to the executable's own folder before looking at the
    // file: with no [Assets]/[Save] path configured that directory IS the base,
    // so the game no longer depends on the working directory. See the note in
    // AssetPath.h for why the compile-time roots still exist.
    char exeDir[240] = "";
    plat_exe_dir(exeDir, sizeof(exeDir));
    SetAssetBase(exeDir);
    SetSaveRoot(NULL);

    const char* path = ConfigFile_Find();
    if (!FileExists(path)) return FALSE;

    g_bFullScreen    = ReadInt(path, "Display", "FullScreen", g_bFullScreen ? 1 : 0) ? TRUE : FALSE;
    g_dwScreenWidth  = (DWORD)ReadInt(path, "Display", "Width", (int)g_dwScreenWidth);
    g_dwScreenHeight = (DWORD)ReadInt(path, "Display", "Height", (int)g_dwScreenHeight);
    g_dwBitDepth     = (DWORD)ReadInt(path, "Display", "BitDepth", (int)g_dwBitDepth);
    g_bVSync         = ReadInt(path, "Display", "VSync", g_bVSync ? 1 : 0) ? TRUE : FALSE;
    // Port-added (Video Options menu): Aspect 1 = Normal (4:3), 2 = Widescreen.
    // 3 is the old Widescreen value and still selects it; anything else is
    // Normal. The file value is not the MARNI_ASPECT_* index.
    {
        const int aspect = ReadInt(path, "Display", "Aspect",
                                   g_dwAspectMode == MARNI_ASPECT_WIDE ? 2 : 1);
        g_dwAspectMode = (aspect == 2 || aspect == 3) ? MARNI_ASPECT_WIDE
                                                      : MARNI_ASPECT_4_3;
    }
    // Port-added (Video Options menu): MSAA 1/2/4/8 (anything else = off), CRT.
    g_dwMsaa         = (DWORD)ReadInt(path, "Display", "MSAA", (int)g_dwMsaa);
    if (g_dwMsaa != 2 && g_dwMsaa != 4 && g_dwMsaa != 8) g_dwMsaa = 1;
    g_bCrtShader     = ReadInt(path, "Display", "CRT", g_bCrtShader ? 1 : 0) ? TRUE : FALSE;
    // Port-added (title OPTIONS > SOUND OPTIONS): 0-10.
    g_dwMusicVolume   = (DWORD)ReadInt(path, "Sound", "MusicVolume", (int)g_dwMusicVolume);
    g_dwEffectsVolume = (DWORD)ReadInt(path, "Sound", "EffectsVolume", (int)g_dwEffectsVolume);
    if (g_dwMusicVolume > 10) g_dwMusicVolume = 10;
    if (g_dwEffectsVolume > 10) g_dwEffectsVolume = 10;
    MarniSound_SetVolumes((int)g_dwMusicVolume, (int)g_dwEffectsVolume);
    g_dwVoiceVolume = (DWORD)ReadInt(path, "Sound", "VoiceVolume", (int)g_dwVoiceVolume);
    if (g_dwVoiceVolume > 10) g_dwVoiceVolume = 10;
    MarniSound_SetVoiceVolume((int)g_dwVoiceVolume);

    // Same clamps the Windows build applied.
    if (g_dwScreenWidth < 320) g_dwScreenWidth = 640;
    if (g_dwScreenHeight < 240) g_dwScreenHeight = 480;
    if (g_dwScreenWidth > 3840) g_dwScreenWidth = 3840;
    if (g_dwScreenHeight > 2160) g_dwScreenHeight = 2160;
    if (g_dwBitDepth != 16 && g_dwBitDepth != 32) g_dwBitDepth = 32;

    g_dwPlayCount  = (DWORD)ReadInt(path, "Player", "PlayCount", (int)g_dwPlayCount);
    g_dwClearCount = (DWORD)ReadInt(path, "Player", "ClearCount", (int)g_dwClearCount);

    char buf[128 * 2 + 1];
    if (ReadValue(path, "Input", "KeyDef", buf, sizeof(buf))) {
        HexDecode(buf, g_keyBindingData, (int)sizeof(g_keyBindingData));
    }
    // Mod: undo the '_' an earlier options-menu bug wrote over the arrows.
    {
        static const BYTE kArrow[4] = { 0x26, 0x28, 0x25, 0x27 };   // up down left right
        for (int k = 0; k < 4; k++) {
            if (g_keyBindingData[k] == 0x5f || g_keyBindingData[k] == 0) g_keyBindingData[k] = kArrow[k];
        }
    }
    // Mod: QUICK TURN lives in keyboard slot 26, empty in older KeyDefs.
    if (g_keyBindingData[26] == 0) g_keyBindingData[26] = 'Q';
    if (g_keyBindingData[25] == 0) g_keyBindingData[25] = 'E';   // QUICK KNIFE
    g_bQuickKnife = ReadInt(path, "Mods", "QuickKnife", 0) != 0;
    g_bQuickTurn  = ReadInt(path, "Mods", "QuickTurn", 0) != 0;
    g_bReloadButton = ReadInt(path, "Mods", "Reload", 0) != 0;
    if (g_keyBindingData[24] == 0) g_keyBindingData[24] = 'R';   // RELOAD
    if (ReadValue(path, "Input", "SideDef", buf, sizeof(buf))) {
        HexDecode(buf, g_joystickBindingData, (int)sizeof(g_joystickBindingData));
    }
    {
        extern DWORD g_dwGamepadEnabled, g_dwInputLog;   // InputSystem.cpp
        g_dwGamepadEnabled = (DWORD)ReadInt(path, "Input", "Gamepad", 1);
        g_dwInputLog       = (DWORD)ReadInt(path, "Input", "InputLog", 0);
        g_dwTestArmorKey   = (DWORD)ReadInt(path, "Testing", "ArmorKey", 0);
        g_dwTestShieldKey  = (DWORD)ReadInt(path, "Testing", "ShieldKey", 0);
    }
    // Mod: the GAME PAD bindings, set in Option Mode and shared by every save.
    if (ReadValue(path, "Input", "PadDef", buf, sizeof(buf)) && buf[0] != '\0') {
        HexDecode(buf, (BYTE*)g_JoyRemapTbl[1], 128);
    }

    // Asset and save folders. [Assets] Path is the folder that holds the USA/
    // and JPN/ trees; [Save] Path is where savedat*.dat goes. Both are relative
    // to the executable's directory unless written absolute.
    char value[240];
    char resolved[320];
    if (ReadValue(path, "Assets", "Path", value, sizeof(value)) && value[0] != '\0') {
        SetAssetBase(ResolveConfiguredPath(exeDir, value, resolved, sizeof(resolved)));
    }
    if (ReadValue(path, "Save", "Path", value, sizeof(value)) && value[0] != '\0') {
        SetSaveRoot(ResolveConfiguredPath(exeDir, value, resolved, sizeof(resolved)));
    }

    char version[16];
    if (ReadValue(path, "Assets", "Version", version, sizeof(version))) {
        SetAssetVersion(version);
    }

    dbg_printf("[CONFIG] assets=%s save=%s\n", GetAssetRoot(), GetSaveRoot());

    g_debugFeaturesEnabled = ReadInt(path, "Debug", "EnableDebug",
#ifdef _DEBUG
                                     1
#else
                                     0
#endif
                                     );

    // Content mode. [Game] Mode is the single switch: it sets g_GameMode AND
    // the asset overlay folder.
    char modeName[16];
    if (ReadValue(path, "Game", "Mode", modeName, sizeof(modeName))) {
        g_GameMode = ParseGameMode(modeName);
    } else {
        // Pre-Mode config files said [Game] DcMode=1. Honour it rather than
        // silently booting OG against a config the user believes selects the
        // Director's Cut; writing the file back replaces it with Mode=.
        g_GameMode = ReadInt(path, "Game", "DcMode", 0) ? GAME_MODE_DC
                                                        : GAME_MODE_OG;
    }
    // OG uses no overlay; every other mode overlays a folder of its own name.
    SetAssetMode(g_GameMode == GAME_MODE_OG ? "" : GameModeName(g_GameMode));
    g_bPs1EndingCredits = ReadInt(path, "Game", "Ps1EndingCredits", 0) != 0;
    // JPN FMV subtitle overlay. Off by default: the assets only exist in the
    // JPN tree, and the USA build has no equivalent.
    g_bPs1FmvSubtitles = ReadInt(path, "Game", "Ps1FmvSubtitles", 0) != 0;
    // Port-added: overrides the per-FMV skip mask so the movies the original
    // marks unskippable can be skipped too. Off unless the key is set.
    g_bSkipUnskippableFmv = ReadInt(path, "Game", "SkipUnskippableFmv", 0) != 0;
    // Mod: Saturn Ticks replace Hunters. OG content only - the DC overlay
    // uses em1016 for its own zombie model.
    // DC too: the Tick model is read from TK1016/TK1116 there (EntityModelLoader).
    g_bModTicks = ReadInt(path, "Mods", "Ticks", 0) != 0;
    if (g_bModTicks) dbg_printf("[CONFIG] mod: Saturn Ticks enabled\n");
    g_bTickAlwaysDecap = ReadInt(path, "Mods", "TickAlwaysDecapitate", 0) != 0;
    g_bInterpolate60   = ReadInt(path, "Display", "Interpolate60", 0) ? TRUE : FALSE;
    // Mod: the Saturn Battle Game is offered on the title once the game has
    // been cleared; this offers it regardless (BattleGame.cpp).
    g_bBattleAlwaysUnlocked = ReadInt(path, "BattleGame", "AlwaysUnlocked", 0) != 0;
    {
        extern bool g_bBattleInvincible;    // BattleGame.cpp
        g_bBattleInvincible = ReadInt(path, "BattleGame", "Invincible", 0) != 0;
        extern int g_battleStartRoom;       // BattleGame.cpp
        g_battleStartRoom = ReadInt(path, "BattleGame", "StartRoom", 1);
        extern bool g_bBattleManSpider;     // BattleGame.cpp
        g_bBattleManSpider = ReadInt(path, "BattleGame", "ManSpider", 0) != 0;
        extern bool g_bManSpiderAlwaysDecap;  // BattleGame.cpp
        g_bManSpiderAlwaysDecap = ReadInt(path, "BattleGame", "ManSpiderAlwaysDecapitate", 0) != 0;
        extern bool g_bBattleEndingShot;    // BattleGame.cpp
        g_bBattleEndingShot = ReadInt(path, "BattleGame", "EndingShot", 1) != 0;
        g_bBattleTicks = ReadInt(path, "BattleGame", "Ticks", 0) != 0;
        g_testStartCourtyard  = ReadInt(path, "Testing", "StartCourtyard", 0);
        g_testStartBeforeYawn = ReadInt(path, "Testing", "StartBeforeYawn", 0);
    }
    dbg_printf("[CONFIG] mode=%s overlay=%s ps1_credits=%d\n", GameModeName(g_GameMode),
               GetAssetModeName()[0] ? GetAssetModeName() : "(none)",
               g_bPs1EndingCredits ? 1 : 0);

    // The port always renders in hardware; the original's adapter picker is gone.
    g_dwSelectedDisplayAdapterID = 1;
    g_dwSelectedDisplayModeID = 0;

    dbg_printf("[CONFIG] loaded %s (%ux%u, %s)\n", path,
               (unsigned)g_dwScreenWidth, (unsigned)g_dwScreenHeight,
               g_bFullScreen ? "fullscreen" : "windowed");
    return TRUE;
}

// ---------------------------------------------------------------------------
void ConfigFile_Save(void)
{
    const char* path = ConfigFile_Find();

    // Slurp the file into lines so unknown keys, comments and section order
    // survive; only the keys we own are replaced. Inserted lines come from the
    // static pool, so only the ones recorded here may be freed.
    char* lines[MAX_LINES];
    char* malloced[MAX_LINES];
    int mallocedCount = 0;
    int count = 0;
    s_newLineUsed = 0;

    FILE* f = fopen(path, "r");
    if (f != NULL) {
        char buf[MAX_LINE];
        while (count < MAX_LINES && fgets(buf, sizeof(buf), f) != NULL) {
            size_t n = strlen(buf);
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
            lines[count] = (char*)malloc(n + 1);
            if (lines[count] == NULL) break;
            memcpy(lines[count], buf, n + 1);
            malloced[mallocedCount++] = lines[count];
            count++;
        }
        fclose(f);
    }

    char values[kOwnedKeyCount][600];
    char keyHex[32 * 2 + 1];
    char sideHex[128 * 2 + 1];
    HexEncode(g_keyBindingData, (int)sizeof(g_keyBindingData), keyHex);
    HexEncode(g_joystickBindingData, (int)sizeof(g_joystickBindingData), sideHex);

    snprintf(values[0], sizeof(values[0]), "%d", g_bFullScreen ? 1 : 0);
    snprintf(values[1], sizeof(values[1]), "%u", (unsigned)g_dwScreenWidth);
    snprintf(values[2], sizeof(values[2]), "%u", (unsigned)g_dwScreenHeight);
    snprintf(values[3], sizeof(values[3]), "%u", (unsigned)g_dwBitDepth);
    snprintf(values[4], sizeof(values[4]), "%d", g_bVSync ? 1 : 0);
    snprintf(values[5], sizeof(values[5]), "%u", (unsigned)g_dwPlayCount);
    snprintf(values[6], sizeof(values[6]), "%u", (unsigned)g_dwClearCount);
    snprintf(values[7], sizeof(values[7]), "%s", keyHex);
    snprintf(values[8], sizeof(values[8]), "%s", sideHex);
    snprintf(values[9], sizeof(values[9]), "%d", g_dwAspectMode == MARNI_ASPECT_WIDE ? 2 : 1);
    snprintf(values[10], sizeof(values[10]), "%u", (unsigned)g_dwMsaa);
    snprintf(values[11], sizeof(values[11]), "%d", g_bCrtShader ? 1 : 0);
    snprintf(values[12], sizeof(values[12]), "%u", (unsigned)g_dwMusicVolume);
    snprintf(values[13], sizeof(values[13]), "%u", (unsigned)g_dwEffectsVolume);
    snprintf(values[14], sizeof(values[14]), "%u", (unsigned)g_dwVoiceVolume);
    snprintf(values[15], sizeof(values[15]), "%d", g_bModTicks ? 1 : 0);
    snprintf(values[16], sizeof(values[16]), "%d", g_bQuickKnife ? 1 : 0);
    snprintf(values[17], sizeof(values[17]), "%d", g_bQuickTurn ? 1 : 0);
    snprintf(values[18], sizeof(values[18]), "%d", g_bReloadButton ? 1 : 0);
    {
        char padHex[128 * 2 + 1];
        HexEncode((const BYTE*)g_JoyRemapTbl[1], 128, padHex);
        snprintf(values[19], sizeof(values[19]), "%s", padHex);
    }
    snprintf(values[20], sizeof(values[20]), "%d", g_bInterpolate60 ? 1 : 0);

    for (int i = 0; i < kOwnedKeyCount; ++i) {
        SetValue(kOwnedKeys[i].section, kOwnedKeys[i].key, values[i], lines, &count);
    }

    f = fopen(path, "w");
    if (f == NULL) {
        dbg_printf("[CONFIG] could not write %s\n", path);
    } else {
        for (int i = 0; i < count; ++i) {
            fprintf(f, "%s\n", lines[i]);
        }
        fclose(f);
    }

    for (int i = 0; i < mallocedCount; ++i) {
        free(malloced[i]);
    }
}
