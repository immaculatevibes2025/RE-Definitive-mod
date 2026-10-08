// BattleGame.cpp - the Sega Saturn release's Battle Game (port-added mod)
// ============================================================================
//
// The Saturn version adds a "Battle Game": once the game has been finished,
// the title menu gets a third option. The player picks Chris or Jill and
// fights through fifteen rooms, three of them safe rooms with an item box,
// against a timer, and is scored and ranked J..S at the end. Everything here
// was taken from the Saturn disc:
//
//   STAGE8/ROOM8010.RDT .. ROOM80F0.RDT   the fifteen rooms. Each is a copy of a
//       main-game room with new scripts; tools/saturn/battle2pc.py grafts those
//       scripts onto the PC rooms and writes USA/BATTLE/ROOM80x0.RDT.
//   GAME5.PRG 0x06088698   the mode's init: stage 8 room 1, the per-character
//       loadout written into the ITEM BOX (48 slots, 0x002f8724), the
//       starting ammo totals at 0x002fffa8..ae.
//   GAME5.PRG 0x0602ddd8   the move to the next room once a fight room is clear:
//       room number + 1, a 5-byte door record per room (0x06095950) and an
//       entry point per room (0x060953e0 + stage 7 * 174).
//   GAME5.PRG 0x06099334   which main-game room each battle room is a copy of
//       (BASE_ROOMS below); g_stageId / g_roomId stay on that room, exactly as
//       the Director's Cut arrange rooms do (dc/ArrangeStages.h), so every
//       stage/room-keyed table (backgrounds, sounds, music, special cases)
//       keeps working. Only the room file is re-pointed (battle_rdt_path).
//   GAME5.PRG 0x060959a0   which rooms are fights (1) and which are safe (0).
//   GAME5.PRG 0x0603c9e8   room 12's first enemy becomes Zombie Wesker
//       (ENEMY/EM1017.EMD, converted by tools/saturn/tick2pc.py).
//   GAME5.PRG 0x0603bd0a   room 15's Tyrant is recoloured (the Golden Tyrant).
//   RANKING.PRG 0x0602c3ac the score, 0x06037b12 the rank table.
//
// Rooms are left the way the Saturn leaves them: a fight room has no door, and
// once every enemy placed by its scripts is dead (their death flags are set)
// the game moves on by itself; a safe room's door leads to the next room - its
// door record's destination byte is the battle room number, which
// battle_enter_room resolves.
// ============================================================================
#include "../Globals.h"
#include "../DebugPrint.h"
#include "../system/AssetPath.h"
#include "../system/ConfigFile.h"
#include "BattleGame.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

int g_battleActive = 0;
int g_battleRoom = 0;
int g_battleStartRoom = 1;          // config.ini [BattleGame] StartRoom (testing)
bool g_bBattleEndingShot = true;     // config.ini [BattleGame] EndingShot
bool g_bBattleManSpider = false;
bool g_bManSpiderAlwaysDecap = false; // config.ini [BattleGame] ManSpiderAlwaysDecapitate (testing)    // config.ini [BattleGame] ManSpider (testing)
bool g_bBattleInvincible = false;   // config.ini [BattleGame] Invincible (testing)

extern void title_state(void);

// ============================================================================
// Saturn tables
// ============================================================================
#define BATTLE_ROOMS 15

struct BattleBase { unsigned char stage, room; };

// GAME5.PRG 0x06099334: battle room -> the main-game room it is a copy of.
static const BattleBase kBaseRooms[BATTLE_ROOMS + 1] = {
    { 0, 0x00 },
    { 0, 0x00 }, { 0, 0x03 }, { 2, 0x02 }, { 0, 0x17 }, { 1, 0x10 },
    { 0, 0x00 }, { 3, 0x04 }, { 3, 0x0C }, { 2, 0x09 }, { 2, 0x0C },
    { 2, 0x0E }, { 4, 0x09 }, { 4, 0x0F }, { 4, 0x10 }, { 4, 0x13 },
};

// GAME5.PRG 0x060959a0: 1 = a fight room, 0 = a safe room.
static const unsigned char kFightRoom[BATTLE_ROOMS + 1] = {
    0, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1,
};

// GAME5.PRG 0x06095950: the door shown leaving fight room N for room N+1,
// five bytes: door record +0x08 (door kind), +0x09 (room sound set),
// +0x0A (door animation), +0x0B (entry camera) and the +0x0B flag bits.
static const unsigned char kExitDoor[BATTLE_ROOMS + 1][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x01, 0x06, 0x15, 0x00, 0x40 }, { 0x02, 0x00, 0x1E, 0x05, 0x00 },
    { 0x02, 0x00, 0x07, 0x01, 0x00 }, { 0x00, 0x00, 0x00, 0x04, 0x00 },
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0x03, 0x00, 0x20, 0x05, 0x00 },
    { 0x00, 0x01, 0x08, 0x00, 0x00 }, { 0x0B, 0x01, 0x08, 0x00, 0x00 },
    { 0x00, 0x01, 0x08, 0x00, 0x00 }, { 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x0B, 0x01, 0x0B, 0x03, 0x00 }, { 0x00, 0x01, 0x0B, 0x03, 0x00 },
    { 0x0A, 0x0C, 0x0D, 0x00, 0x40 }, { 0x00, 0x00, 0x00, 0x00, 0x00 },
};

// GAME5.PRG 0x060953e0 + 7 * 174: where the player enters each room
// (direction, x, z).
static const short kEntry[BATTLE_ROOMS + 1][3] = {
    {    0,     0,     0 },
    {  992,  3800,  7400 }, {    0,  3000,  3000 }, { 1024, 23200, 29200 },
    { 1024,  2900, 10600 }, {    0,  6100,  3600 }, { 3072,  3800,  2700 },
    {    0, 12000, 13000 }, {    0,  2500, 11000 }, {    0,  4100, 26800 },
    { 1024, 10800, 13000 }, { 3072,  4600,  2500 }, { 1024,  3600,  2700 },
    { 1024, 12600, 20560 }, { 1024, 22000, 16500 }, { 1024,  4700, 25500 },
};

// GAME5.PRG 0x06088660 / 0x06088677: the item box at the start, (item, count)
// pairs. The inventory itself starts empty; the first room is a safe room.
static const unsigned char kLoadoutChris[] = {
    ITEM_KNIFE, 0, ITEM_BERETTA, 15, ITEM_SHOTGUN, 7, ITEM_COLT_PYTHON_MAG, 6,
    ITEM_CLIP, 75, ITEM_SHELLS, 49, ITEM_MAGNUM_ROUNDS, 48,
    ITEM_FIRST_AID_SPRAY, 1, ITEM_FIRST_AID_SPRAY, 1,
    ITEM_FIRST_AID_SPRAY, 1, ITEM_FIRST_AID_SPRAY, 1,
};
static const unsigned char kLoadoutJill[] = {
    ITEM_KNIFE, 0, ITEM_BERETTA, 15, ITEM_SHOTGUN, 7, ITEM_COLT_PYTHON_MAG, 6,
    ITEM_BAZOOKA_EXPLOSIVE, 0, ITEM_CLIP, 75, ITEM_SHELLS, 35, ITEM_MAGNUM_ROUNDS, 36,
    ITEM_EXPLOSIVE_ROUNDS, 6, ITEM_ACID_ROUNDS, 6, ITEM_FLAME_ROUNDS, 6,
    ITEM_FIRST_AID_SPRAY, 1, ITEM_FIRST_AID_SPRAY, 1, ITEM_FIRST_AID_SPRAY, 1,
    ITEM_FIRST_AID_SPRAY, 1, ITEM_FIRST_AID_SPRAY, 1,
};

// RANKING.PRG 0x06037ac0: score per remaining round / spray, by category.
enum { CAT_HANDGUN, CAT_SHOTGUN, CAT_MAGNUM, CAT_EXPLOSIVE, CAT_ACID, CAT_FLAME, CAT_SPRAY, CAT_COUNT };
static const int kCatWeight[CAT_COUNT] = { 2, 6, 10, 15, 15, 15, 150 };

#define BATTLE_ROOM_ID_HUNTERS 9      // three Hunters on the Saturn; two here
#define BATTLE_ROOM_ID_WESKER  12     // room 12's first enemy is Zombie Wesker
#define BATTLE_ROOM_ID_TYRANT  15     // the Golden Tyrant
#define BATTLE_WESKER_TYPE     0x00   // runs as a white-coat zombie (its skeleton)
#define BATTLE_CLEAR_HOLD      60     // frames a fight room must stay clear (2 s)
                                      // - "STAGE CLEAR!" shows during them

#define BATTLE_HUD_GREEN       0x01   // PrintText8x8 palette: green

// ============================================================================
// Session state
// ============================================================================
static unsigned int  s_ticks60;           // clear time, 1/60 s (the Saturn's 0x002fffa0)
static unsigned char s_roomFlags[32];     // death flags of this room's enemies
static int           s_roomFlagCount;
static int           s_roomEnemySets;     // enemy_set calls seen in this room
static int           s_clearHold;
static int           s_leaving;           // transition requested
static int           s_finishing;         // last room clear, fading to the results
static int           s_endingFrame;       // frames into the ending camera
static int           s_endingFade;        // the fade to the results has started
static float         s_endingAngle0;      // the orbit's starting angle

// The ending: after the last room, the camera leaves the room's fixed angle,
// closes in on the player and circles them against black (the room's
// background, masks and the dead enemies are hidden) - the Saturn plays a
// shot like this before its results. g_battleEndingCam is read by
// OT_InsertPrimitive (Rendering.cpp, background), game_loop (masks, enemies)
// and the pad code (InputSystem.cpp, the player stands still).
int g_battleEndingCam = 0;
#define BATTLE_ENDING_FRAMES  240     // 8 s at the engine's 30 Hz
#define BATTLE_ENDING_FADE     40     // the last frames fade to black
static int           s_goldApplied;
static int           s_weskerBoosted;     // his health has been tripled
static int           s_tyrantBoosted;     // the Golden Tyrant's health is +50%
static Entity*       s_wesker;            // room 12's Zombie Wesker, while it is loaded
static short         s_finalHealth;

static unsigned char s_doorRecord[0x18];

// ============================================================================
// Availability / arming
// ============================================================================
int battle_title_option_available(void)
{
    // OG and (mod) the Director's Cut; not the other content modes or JPN.
    if ((g_GameMode != GAME_MODE_OG && g_GameMode != GAME_MODE_DC) || GetAssetVersion() != 0) {
        return 0;
    }
    if (g_dwClearCount == 0 && !g_bBattleAlwaysUnlocked) {
        return 0;
    }
    static int s_dataChecked = -1;
    if (s_dataChecked < 0) {
        s_dataChecked = mod_asset_exists("data/t_battle.tim") &&
                        mod_asset_exists("battle/ROOM8010.RDT");
    }
    return s_dataChecked;
}

void battle_arm(void)
{
    g_battleActive = 1;
    g_battleRoom = 0;
}

void battle_disarm(void)
{
    g_battleActive = 0;
    g_battleRoom = 0;
    g_battleEndingCam = 0;
}

// One frame of the ending camera: a slow 3/4 turn around the player, closing
// from the room camera's distance to a head-and-shoulders shot over the first
// 40%, then holding that distance. Built as the from/to block MatrixToCamera
// takes (Room_SetupCamera, DoorSystem's door camera), with the room camera's
// own FOV.
static void battle_ending_camera(void)
{
    const float t = (float)s_endingFrame / (float)BATTLE_ENDING_FRAMES;
    float z = t < 0.4f ? t / 0.4f : 1.0f;
    z = z * z * (3.0f - 2.0f * z);                  // ease in/out

    const float radius = 6500.0f - 3700.0f * z;     // 6500 -> 2800 units
    const float lift   = 1400.0f - 1150.0f * z;     // well above -> just above eye level
    const float angle  = s_endingAngle0 + t * 3.14159265f * 1.5f;

    const int px = g_playerEntity.scaMatrixData.localMatrix.t[0];
    const int py = g_playerEntity.scaMatrixData.localMatrix.t[1];
    const int pz = g_playerEntity.scaMatrixData.localMatrix.t[2];
    // Aim point: chest at the start, the face at the end (Y is up-negative;
    // the player stands at y 0 and the face is around -1750), so the close
    // orbit frames the whole face rather than the chin and shoulders.
    const int ty = py - 1100 - (int)(650.0f * z);

    int cam[8];
    cam[0] = px + (int)(cosf(angle) * radius);
    cam[1] = ty - (int)lift;
    cam[2] = pz + (int)(sinf(angle) * radius);
    cam[3] = px;
    cam[4] = ty;
    cam[5] = pz;
    cam[6] = 0;
    cam[7] = 0;

    RDT_Camera* cameras = (RDT_Camera*)((char*)g_RdtPointer + sizeof(RDT));
    set_scene_render_param(cameras[g_roomCameraId].fov);
    MatrixToCamera((MATRIX*)cam);
}

static void battle_ending_start(void)
{
    // Start the orbit where the room camera already is, so the shot does not
    // jump to another side of the player.
    RDT_Camera* cameras = (RDT_Camera*)((char*)g_RdtPointer + sizeof(RDT));
    const RDT_Camera& c = cameras[g_roomCameraId];
    s_endingAngle0 = atan2f((float)(c.cam_from_z - g_playerEntity.scaMatrixData.localMatrix.t[2]),
                            (float)(c.cam_from_x - g_playerEntity.scaMatrixData.localMatrix.t[0]));
    s_endingFrame = 0;
    s_endingFade = 0;
    g_battleEndingCam = g_bBattleEndingShot ? 1 : 0;   // 0: stay on the room camera
    g_main_state_flags |= MSF_CAMERA_LOCK;      // no camera-zone switches
}

static void battle_reset_room_tracking(void)
{
    s_roomFlagCount = 0;
    s_roomEnemySets = 0;
    s_clearHold = 0;
    s_leaving = 0;
    s_goldApplied = 0;
    s_weskerBoosted = 0;
    s_tyrantBoosted = 0;
    s_wesker = NULL;
}

// ============================================================================
// Game start
// ============================================================================
void battle_new_game_setup(void)
{
    if (!g_battleActive) {
        return;
    }
    // Testing aid ([BattleGame] StartRoom=N): begin the run in room N instead
    // of room 1. The weapons are then put in the inventory as well (below),
    // since a fight room has no item box.
    const int start = (g_battleStartRoom >= 1 && g_battleStartRoom <= BATTLE_ROOMS)
                    ? g_battleStartRoom : 1;
    g_battleRoom = start;
    g_stageId = kBaseRooms[start].stage;
    g_roomId = kBaseRooms[start].room;

    // A clean slate of death flags: the battle rooms reuse flag numbers.
    memset(&g_EnemiesFlags[0], 0, 32);

    // Empty inventory (8 slots for Jill - hers run into the Rebecca block, as
    // in the original), the loadout in the item box.
    ItemSlot* inv = (ItemSlot*)&g_ItemsSlots[0];
    for (int i = 0; i < 8; i++) {
        inv[i].Id = 0;
        inv[i].qty = 0;
    }
    g_TotalHeldItems = 0;
    g_ItemSlotsBitmask = 0;

    ItemSlot* box = (ItemSlot*)&g_itemboxSlots[0];
    for (int i = 0; i < 48; i++) {
        box[i].Id = 0;
        box[i].qty = 0;
    }
    const int jill = (g_playerEntity.id & 1) != 0;
    const unsigned char* lo = jill ? kLoadoutJill : kLoadoutChris;
    const int n = jill ? (int)sizeof(kLoadoutJill) / 2 : (int)sizeof(kLoadoutChris) / 2;
    for (int i = 0; i < n; i++) {
        box[i].Id = lo[i * 2];
        box[i].qty = lo[i * 2 + 1];
    }

    // Starting past room 1: carry the loadout's weapons and ammo, everything
    // but the knife, in as many slots as the character has (6 Chris, 8 Jill).
    if (start != 1) {
        const int slots = jill ? 8 : 6;
        int held = 0;
        for (int i = 0; i < n && held < slots; i++) {
            if (lo[i * 2] == ITEM_KNIFE) continue;
            inv[held].Id = lo[i * 2];
            inv[held].qty = lo[i * 2 + 1];
            held++;
        }
        g_TotalHeldItems = (BYTE)held;
        g_ItemSlotsBitmask = (1u << (held & 0x1f)) - 1;   // as GameStart.cpp does
    }

    g_playerEntity.directionAngle = kEntry[start][0];
    g_playerEntity.position.x = kEntry[start][1];
    g_playerEntity.position.z = kEntry[start][2];

    s_ticks60 = 0;
    s_finishing = 0;
    s_finalHealth = 0;
    battle_reset_room_tracking();
    dbg_printf("[battle] start: %s, room %d (base stage %d room %02X)\n",
               jill ? "Jill" : "Chris", start, kBaseRooms[start].stage, kBaseRooms[start].room);
}

// ============================================================================
// Rooms
// ============================================================================
int battle_rdt_path(char* path, unsigned int size)
{
    if (!g_battleActive || g_battleRoom < 1 || g_battleRoom > BATTLE_ROOMS) {
        return 0;
    }
    snprintf(path, size, GAME_DATA_ROOT "battle\\room80%X0.rdt", (unsigned int)g_battleRoom);
    return 1;
}

// Safe rooms (item box, no enemies) keep their own room music; the battle
// track plays in the fight rooms (SoundSystem.cpp, battle_bgm_update).
int battle_in_safe_room(void)
{
    return g_battleActive && g_battleRoom >= 1 && g_battleRoom <= BATTLE_ROOMS &&
           !kFightRoom[g_battleRoom];
}

// Boss rooms (5 Yawn, 8 Plant 42, 10 Black Tiger) play Yawn's boss music
// instead of the battle track (SoundSystem.cpp, battle_bgm_update).
int battle_in_boss_room(void)
{
    return g_battleActive &&
           (g_battleRoom == 5 || g_battleRoom == 8 || g_battleRoom == 10);
}

// Room 15, the (Golden) Tyrant: the Tyrant battle music (Bgm_24).
int battle_in_tyrant_room(void)
{
    return g_battleActive && g_battleRoom == BATTLE_ROOM_ID_TYRANT;
}

int battle_enter_room(unsigned char dest)
{
    int room = dest & 0x1F;
    if (room < 1 || room > BATTLE_ROOMS) {
        dbg_printf("[battle] door to room %d - out of range, staying\n", room);
        room = g_battleRoom;
    }
    const unsigned char oldStage = g_stageId;
    g_battleRoom = room;
    g_stageId = kBaseRooms[room].stage;
    g_roomId = kBaseRooms[room].room;
    battle_reset_room_tracking();

    // A run never goes back to a room, and several battle rooms share a base
    // room id (1/6, 5/14, 8/10, 9/12). So nothing from the room just left may
    // carry over: no death flag (cmd_enemy_set skips an enemy whose flag is
    // already set) and no saved enemy state (BuildEnemySnap / FUN_0048f330
    // match on g_roomId alone and would put room 8's enemies into room 10).
    memset(&g_EnemiesFlags[0], 0, 32);
    memset(g_savedEnemyStates, 0, sizeof(g_savedEnemyStates));
    dbg_printf("[battle] room %d (base stage %d room %02X)\n", room, g_stageId, g_roomId);
    return g_stageId != oldStage;
}

void battle_on_enemy_set(Entity* enemy, unsigned char slot)
{
    if (!g_battleActive || enemy == NULL) {
        return;
    }
    // GAME5.PRG 0x0603c9e8: in room 12 the first enemy record's type becomes
    // Zombie Wesker's. The PC has no such type; he keeps the zombie AI on his
    // own model (battle_emd_override), which has the white-coat zombie's
    // skeleton, so he takes that type.
    // Room 9 places three Hunters (enemy slots 0-2); keep two. The third is
    // switched off before anything else sees it - not counted for the room
    // clear, and taken back out of g_enemy_count so RoomInit's model loop
    // (which counts active slots up to g_enemy_count) never waits for it.
    if (g_battleRoom == BATTLE_ROOM_ID_HUNTERS && slot == 2) {
        enemy->status_flags = 0;
        if (g_enemy_count > 0) {
            g_enemy_count--;
        }
        return;
    }

    if (g_battleRoom == BATTLE_ROOM_ID_WESKER && s_roomEnemySets == 0) {
        enemy->id = BATTLE_WESKER_TYPE;
        s_wesker = enemy;
    }
    s_roomEnemySets++;

    if (enemy->death_event_id != 0xFF && s_roomFlagCount < (int)sizeof(s_roomFlags)) {
        for (int i = 0; i < s_roomFlagCount; i++) {
            if (s_roomFlags[i] == enemy->death_event_id) {
                return;
            }
        }
        s_roomFlags[s_roomFlagCount++] = enemy->death_event_id;
    }
}

// Mod (testing): room 15's Tyrant is replaced by the RE 1.5 Man Spider
// (em1018/em1118, tools/saturn/manspider2pc.py). He runs the Tyrant's AI;
// his clips are mapped onto the Tyrant's in the converter.
static int battle_man_spider(void)
{
    return g_bBattleManSpider && g_battleActive && g_battleRoom == BATTLE_ROOM_ID_TYRANT &&
           mod_asset_exists((g_playerEntity.id & 1) ? "enemy/em1118.emd" : "enemy/em1018.emd");
}

int battle_man_spider_active(void) { return battle_man_spider(); }

const char* battle_emd_override(Entity* em, unsigned char tableIndex)
{
    if (em != NULL && (tableIndex == 0x0C + 4 || tableIndex == 0x10 + 4) && battle_man_spider()) {
        g_LastEnemyModelId = 0xFF;
        return (g_playerEntity.id & 1) ? "enemy/em1118.emd" : "enemy/em1018.emd";
    }
    if (!g_battleActive || em == NULL || em != s_wesker ||
        tableIndex != BATTLE_WESKER_TYPE + 4) {
        return NULL;
    }
    const char* path = (g_playerEntity.id & 1) ? "enemy/em1117.emd" : "enemy/em1017.emd";
    if (!mod_asset_exists(path)) {
        return NULL;
    }
    // RoomInit's model cache hands a following enemy of the same type the
    // previous one's model; make sure no ordinary zombie inherits his.
    g_LastEnemyModelId = 0xFF;
    return path;
}

// ============================================================================
// The Golden Tyrant: the colour multipliers TmdObjectTintSet writes
// (CmdFunctions.cpp), set to gold on every object of the Tyrant's model.
// ============================================================================
static void battle_tint_gold(void* modelObj)
{
    unsigned char* obj = (unsigned char*)modelObj;
    if (obj == NULL || *(int*)(obj + 0x10) != 0) {
        return;
    }
    unsigned char* tmd = *(unsigned char**)(obj + 0x20);
    if (tmd == NULL || (*(unsigned int*)(tmd + 0x4C0) & 0x7FFFFFFF) == 0) {
        return;
    }
    unsigned char* rec = tmd + 0x4D0;
    unsigned int count = (unsigned int)(*(int*)(tmd + 0x4C0) * 2);
    for (unsigned int i = 0; i < count; i++) {
        *(float*)(rec + 0x5C) = 1.00f;
        *(float*)(rec + 0x6C) = 1.00f;
        *(float*)(rec + 0x60) = 0.80f;
        *(float*)(rec + 0x70) = 0.80f;
        *(float*)(rec + 0x64) = 0.30f;
        *(float*)(rec + 0x74) = 0.30f;
        rec += 0x84;
    }
}

static void battle_apply_gold_tyrant(void)
{
    for (int n = 0; n < 30; n++) {
        Entity* e = &g_EnemiesList[n];
        if ((e->status_flags & 1) == 0 || (e->id != 0x0C && e->id != 0x10)) {
            continue;
        }
        if (e->jointsStructs == NULL || e->jointCount == 0) {
            continue;
        }
        for (int j = 0; j < (int)e->jointCount; j++) {
            battle_tint_gold(e->jointsStructs[j].anim_object);
        }
        s_goldApplied = 1;
    }
}

// ============================================================================
// Score (RANKING.PRG 0x0602c3ac)
// ============================================================================
// The scoring category an item counts towards, or -1.
static int battle_item_cat(int id)
{
    switch (id) {
    case ITEM_BERETTA: case ITEM_CLIP:                       return CAT_HANDGUN;
    case ITEM_SHOTGUN: case ITEM_SHELLS:                     return CAT_SHOTGUN;
    case ITEM_COLT_PYTHON_MAG: case ITEM_MAGNUM_ROUNDS:      return CAT_MAGNUM;
    case ITEM_BAZOOKA_EXPLOSIVE: case ITEM_EXPLOSIVE_ROUNDS: return CAT_EXPLOSIVE;
    case ITEM_BAZOOKA_ACID: case ITEM_ACID_ROUNDS:           return CAT_ACID;
    case ITEM_BAZOOKA_FLAME: case ITEM_FLAME_ROUNDS:         return CAT_FLAME;
    case ITEM_FIRST_AID_SPRAY:                               return CAT_SPRAY;
    default:                                                 return -1;
    }
}

// What the run started with, per category: the character's loadout, counted
// the same way battle_count_supplies counts what is left (rounds, loaded ones
// included; sprays one each).
static void battle_count_loadout(int jill, int out[CAT_COUNT])
{
    for (int i = 0; i < CAT_COUNT; i++) out[i] = 0;
    const unsigned char* lo = jill ? kLoadoutJill : kLoadoutChris;
    const int n = jill ? (int)sizeof(kLoadoutJill) / 2 : (int)sizeof(kLoadoutChris) / 2;
    for (int i = 0; i < n; i++) {
        const int c = battle_item_cat(lo[i * 2]);
        if (c < 0) continue;
        out[c] += (c == CAT_SPRAY) ? 1 : (lo[i * 2 + 1] & 0x7F);
    }
}

static void battle_count_supplies(int out[CAT_COUNT])
{
    for (int i = 0; i < CAT_COUNT; i++) out[i] = 0;
    const ItemSlot* inv = (const ItemSlot*)&g_ItemsSlots[0];
    const ItemSlot* box = (const ItemSlot*)&g_itemboxSlots[0];
    const int invSlots = (g_playerEntity.id & 1) ? 8 : 6;
    for (int pass = 0; pass < 2; pass++) {
        const ItemSlot* s = pass ? box : inv;
        const int n = pass ? 48 : invSlots;
        for (int i = 0; i < n; i++) {
            const int q = s[i].qty;
            switch (s[i].Id) {
            case ITEM_BERETTA: case ITEM_CLIP:                  out[CAT_HANDGUN] += q & 0x7F; break;
            case ITEM_SHOTGUN: case ITEM_SHELLS:                out[CAT_SHOTGUN] += q & 0x7F; break;
            case ITEM_COLT_PYTHON_MAG: case ITEM_MAGNUM_ROUNDS: out[CAT_MAGNUM] += q & 0x7F; break;
            case ITEM_BAZOOKA_EXPLOSIVE: case ITEM_EXPLOSIVE_ROUNDS: out[CAT_EXPLOSIVE] += q & 0x7F; break;
            case ITEM_BAZOOKA_ACID: case ITEM_ACID_ROUNDS:      out[CAT_ACID] += q & 0x7F; break;
            case ITEM_BAZOOKA_FLAME: case ITEM_FLAME_ROUNDS:    out[CAT_FLAME] += q & 0x7F; break;
            case ITEM_FIRST_AID_SPRAY:                          out[CAT_SPRAY] += 1; break;
            default: break;
            }
        }
    }
    for (int i = 0; i < CAT_COUNT; i++) {
        if (out[i] > 255) out[i] = 255;     // the Saturn keeps them in bytes
    }
}

struct BattleResult {
    int jill;
    int score;
    int lifePct;
    unsigned int ticks60;
    char rank;
    int used[CAT_COUNT];    // rounds / sprays used: the loadout minus what is left
};

static char battle_rank_letter(int score)
{
    // RANKING.PRG 0x06037b12, indexed by score / 100.
    static const char kRanks[10] = { 'J', 'I', 'H', 'G', 'F', 'E', 'D', 'C', 'B', 'A' };
    const int i = score / 100;
    return (i >= 10) ? 'S' : kRanks[i];
}

static BattleResult battle_score(void)
{
    BattleResult r;
    r.jill = (g_playerEntity.id & 1) != 0;
    r.ticks60 = s_ticks60;

    int sup[CAT_COUNT];
    battle_count_supplies(sup);
    {
        int start[CAT_COUNT];
        battle_count_loadout(r.jill, start);
        for (int i = 0; i < CAT_COUNT; i++) {
            r.used[i] = start[i] > sup[i] ? start[i] - sup[i] : 0;
        }
    }
    int ammo = sup[CAT_HANDGUN] * kCatWeight[CAT_HANDGUN] +
               sup[CAT_SHOTGUN] * kCatWeight[CAT_SHOTGUN] +
               sup[CAT_MAGNUM]  * kCatWeight[CAT_MAGNUM] +
               sup[CAT_SPRAY]   * kCatWeight[CAT_SPRAY];
    if (r.jill) {   // the bazooka rounds only count for Jill, who has one
        ammo += (sup[CAT_EXPLOSIVE] + sup[CAT_ACID] + sup[CAT_FLAME]) * kCatWeight[CAT_EXPLOSIVE];
    }

    const int maxHealth = r.jill ? 96 : 140;              // 140 - 44 * jill
    int health = s_finalHealth < 0 ? 0 : s_finalHealth;
    r.lifePct = (int)(100.0 / maxHealth * health);

    // Seconds under the par time (Chris 12:00, Jill 7:00), plus 50 per whole
    // minute of that.
    int left = (r.jill ? 420 : 720) - (int)(r.ticks60 / 60);
    if (left < 0) left = 0;
    const int timeScore = left + (left / 60) * 50;

    r.score = (ammo + r.lifePct + timeScore) & 0xFFFF;
    r.rank = battle_rank_letter(r.score);
    return r;
}

// ============================================================================
// Ranking table: top ten per character, kept beside the saves
// (<save folder>/battle.dat), like the Saturn's backup-RAM table.
// ============================================================================
#pragma pack(push, 1)
struct BattleRankEntry {
    unsigned short score;
    unsigned char  rank;
    unsigned char  lifePct;
    unsigned int   ticks60;
    char           name[4];         // initials, 3 letters + NUL ("BTL2" on)
};
struct BattleRankFile {
    char            magic[4];       // "BTL2"
    BattleRankEntry e[2][10];
};
// The first format, without initials: still read, its entries get "---".
struct BattleRankEntryV1 {
    unsigned short score;
    unsigned char  rank;
    unsigned char  lifePct;
    unsigned int   ticks60;
};
struct BattleRankFileV1 {
    char              magic[4];     // "BTL1"
    BattleRankEntryV1 e[2][10];
};
#pragma pack(pop)

static BattleRankFile s_rankFile;

static void battle_rank_path(char* path, unsigned int size)
{
    snprintf(path, size, "%sbattle.dat", GetSaveRoot());
}

static void battle_rank_load(void)
{
    memset(&s_rankFile, 0, sizeof(s_rankFile));
    memcpy(s_rankFile.magic, "BTL2", 4);
    char path[300];
    battle_rank_path(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (f == NULL) return;
    static unsigned char buf[sizeof(BattleRankFile)];
    const size_t got = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (got == sizeof(BattleRankFile) && memcmp(buf, "BTL2", 4) == 0) {
        memcpy(&s_rankFile, buf, sizeof(s_rankFile));
    } else if (got >= sizeof(BattleRankFileV1) && memcmp(buf, "BTL1", 4) == 0) {
        const BattleRankFileV1* old = (const BattleRankFileV1*)buf;
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < 10; i++) {
                BattleRankEntry& e = s_rankFile.e[c][i];
                e.score = old->e[c][i].score;
                e.rank = old->e[c][i].rank;
                e.lifePct = old->e[c][i].lifePct;
                e.ticks60 = old->e[c][i].ticks60;
                memcpy(e.name, "---", 4);
            }
        }
    }
}

static void battle_rank_save(void)
{
    char path[300];
    battle_rank_path(path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        dbg_printf("[battle] could not write %s\n", path);
        return;
    }
    fwrite(&s_rankFile, 1, sizeof(s_rankFile), f);
    fclose(f);
}

// Returns the place (0..9) the result took, or -1.
static int battle_rank_insert(const BattleResult& r)
{
    BattleRankEntry* list = s_rankFile.e[r.jill ? 1 : 0];
    int at = 0;
    while (at < 10 && list[at].rank != 0 && list[at].score >= r.score) at++;
    if (at >= 10) return -1;
    for (int i = 9; i > at; i--) list[i] = list[i - 1];
    list[at].score = (unsigned short)r.score;
    list[at].rank = (unsigned char)r.rank;
    list[at].lifePct = (unsigned char)r.lifePct;
    list[at].ticks60 = r.ticks60;
    memcpy(list[at].name, "AAA", 4);    // replaced on the initials screen
    return at;
}

// ============================================================================
// Text helpers (the 8x8 debug font, through the pending-sprite queue)
// ============================================================================
static void battle_print(int x, int y, unsigned char color, const char* text)
{
    snprintf(PRINT_TEXT_BUFFER, sizeof(PRINT_TEXT_BUFFER), "%s", text);
    PrintText8x8((short)x, (short)y, color, 0);
}

static void battle_format_time(char* out, unsigned int size, unsigned int ticks60)
{
    const unsigned int sec = ticks60 / 60;
    // Minutes and seconds only (no hundredths), on the HUD, the results and
    // the ranking alike.
    snprintf(out, size, "%02u:%02u", sec / 60 > 99 ? 99 : sec / 60, sec % 60);
}

// ============================================================================
// The results screen (RANKING.PRG): this run, then the character's top ten.
// ============================================================================
static BattleResult s_result;
static int          s_resultPlace;

static void battle_results_state(void)
{
    g_main_state_flags = (g_main_state_flags & ~MSF_SCREEN_MODE_MASK) | MSF_SCREEN_STANDALONE;

    // The Saturn's results jingle (song 1 of SND/BATTLE.CDP).
    battle_jingle_play();

    // Fade in from the black the last room faded to (game_loop's own fade-in
    // form: type 2 with a negative counter).
    g_fade_type_id = 2;
    g_fading_counter = (short)0xF000;
    fade_update();

    static RectDrawDesc bg;
    int frames = 0;
    int page = 0;           // 0 = this run, 1 = ranking
    int leaving = 0;

    // Initials: a run that made the top ten gets its three letters entered on
    // the ranking page before the page can be left. UP/DOWN change the
    // letter, ACTION (or RIGHT) confirms it and moves on, CANCEL (or LEFT)
    // goes back one. The table is saved again once the third is confirmed.
    static const char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.- ";
    const int nLetters = (int)sizeof(kLetters) - 1;
    int entering = 0;       // 1 while the initials are being entered
    int cursor = 0;         // 0..2
    int letter[3] = { 0, 0, 0 };
    BattleRankEntry* mine = (s_resultPlace >= 0)
        ? &s_rankFile.e[s_result.jill ? 1 : 0][s_resultPlace] : NULL;
    for (;;) {
        bg.textureId = 0;
        bg.x = -160;
        bg.y = -120;
        bg.w = 320;
        bg.h = 240;
        bg.r = bg.g = bg.b = 0;
        draw_rect(&bg, 4000, 1);

        char line[64];
        char t[16];
        if (page == 0) {
            battle_print(160 - 6 * 8 / 2, 20, 0x00, "RESULT");
            battle_print(64, 38, 0x03, s_result.jill ? "JILL" : "CHRIS");
            snprintf(line, sizeof(line), "SCORE              %5d", s_result.score);
            battle_print(64, 54, 0x00, line);
            snprintf(line, sizeof(line), "LIFE                %3d%%", s_result.lifePct);
            battle_print(64, 66, 0x00, line);
            battle_format_time(t, sizeof(t), s_result.ticks60);
            snprintf(line, sizeof(line), "CLEAR TIME         %s", t);
            battle_print(64, 78, 0x00, line);
            snprintf(line, sizeof(line), "RANK                   %c", s_result.rank);
            battle_print(64, 94, 0x01, line);

            // What the run used. Rounds for the guns (loaded ones included),
            // one per spray; Jill's three bazooka rounds as well.
            struct UsedRow { const char* label; int cat; };
            static const UsedRow kUsed[] = {
                { "HANDGUN BULLETS", CAT_HANDGUN }, { "SHELLS", CAT_SHOTGUN },
                { "MAGNUM ROUNDS", CAT_MAGNUM },    { "FIRST AID SPRAYS", CAT_SPRAY },
                { "EXPLOSIVE ROUNDS", CAT_EXPLOSIVE }, { "ACID ROUNDS", CAT_ACID },
                { "FLAME ROUNDS", CAT_FLAME },
            };
            const int rows = s_result.jill ? 7 : 4;
            battle_print(64, 114, 0x03, "USED");
            for (int i = 0; i < rows; i++) {
                snprintf(line, sizeof(line), "%-18s %5d", kUsed[i].label,
                         s_result.used[kUsed[i].cat]);
                battle_print(64, 128 + i * 12, 0x00, line);
            }
        } else {
            battle_print(160 - 7 * 8 / 2, 24, 0x00, "RANKING");
            battle_print(160 - (s_result.jill ? 4 : 5) * 8 / 2, 40, 0x03,
                         s_result.jill ? "JILL" : "CHRIS");
            static const char* kPlace[10] = { " 1ST", " 2ND", " 3RD", " 4TH", " 5TH",
                                              " 6TH", " 7TH", " 8TH", " 9TH", "10TH" };
            const BattleRankEntry* list = s_rankFile.e[s_result.jill ? 1 : 0];
            for (int i = 0; i < 10; i++) {
                if (list[i].rank == 0) {
                    snprintf(line, sizeof(line), "%s   -----", kPlace[i]);
                } else {
                    char name[4];
                    memcpy(name, list[i].name, 3);
                    name[3] = 0;
                    if (name[0] == 0) memcpy(name, "---", 4);
                    battle_format_time(t, sizeof(t), list[i].ticks60);
                    snprintf(line, sizeof(line), "%s %-3s %5d  %s  %c", kPlace[i], name,
                             (int)list[i].score, t, list[i].rank);
                }
                battle_print(48, 64 + i * 14, (i == s_resultPlace) ? 0x01 : 0x00, line);
            }
            if (entering) {
                // The letter being edited blinks; a caret sits under it.
                const int y = 64 + s_resultPlace * 14;
                const int x = 48 + 5 * 8 + cursor * 8;     // after "%s " (4 + 1 chars)
                if ((frames & 8) == 0) {
                    RectDrawDesc hide = {};
                    hide.textureId = 0;
                    hide.x = x - 160;
                    hide.y = y - 120;
                    hide.w = 8;
                    hide.h = 8;
                    hide.r = hide.g = hide.b = 0;
                    draw_rect(&hide, 450, 1);
                }
                battle_print(x, y + 9, 0x01, "^");
                battle_print(160 - 19 * 8 / 2, 214, 0x03, "ENTER YOUR INITIALS");
            }
        }

        Task_sleep(1);
        frames++;
        if (leaving) {
            if ((short)g_fading_state < 0) {
                break;
            }
        } else if (entering) {
            const unsigned int pad = g_PlayerPadPressed;
            if (pad & 0x1000) {                         // up
                letter[cursor] = (letter[cursor] + 1) % nLetters;
            } else if (pad & 0x4000) {                  // down
                letter[cursor] = (letter[cursor] + nLetters - 1) % nLetters;
            } else if ((pad & 0x0040) || (pad & 0x8000)) {   // cancel / left
                if (cursor > 0) cursor--;
            } else if ((pad & 0x2000) || (pad & 0x0EBF)) {    // right / action
                if (cursor < 2) {
                    cursor++;
                    letter[cursor] = letter[cursor - 1];   // start from the last letter
                } else {
                    entering = 0;
                    frames = 0;
                    battle_rank_save();
                }
            }
            if (mine != NULL) {
                for (int k = 0; k < 3; k++) mine->name[k] = kLetters[letter[k]];
                mine->name[3] = 0;
            }
        } else if (frames > 30 && (g_PlayerPadPressed & 0xEFF) != 0) {
            if (page == 0) {
                page = 1;
                frames = 0;
                entering = (mine != NULL);
                // The ranking page plays the credits music (the staff roll's).
                battle_credits_play();
            } else {
                leaving = 1;
                g_fade_type_id = 2;
                g_fading_counter = 0x0800;
                fade_update();
            }
        }
    }

    battle_disarm();
    g_main_state_flags2 &= MSF2_RESET_KEEP_MASK;
    g_main_state_flags = (g_main_state_flags & ~(MSF_SCREEN_MODE_MASK | MSF_CONTINUE_GAME)) | MSF_SCREEN_STANDALONE;
    Task_chain((void*)title_state);
}

// ============================================================================
// Per frame
// ============================================================================
static int battle_room_clear(void)
{
    if (s_roomFlagCount == 0) {
        // Nothing with a death flag was placed: clear unless something was.
        return s_roomEnemySets == 0;
    }
    for (int i = 0; i < s_roomFlagCount; i++) {
        if (Flg_ck((int)g_EnemiesFlags, s_roomFlags[i]) == 0) {
            return 0;
        }
    }
    return 1;
}

// The door handoff game_loop already knows (door_begin_transition /
// DebugRoomChange_Trigger): a record, MSF_GAMEPLAY_ACTIVE, g_openMenuFlag = 1;
// room_transition_load runs on the menu path and calls battle_enter_room.
static void battle_leave_room(void)
{
    const int from = g_battleRoom;
    const int to = from + 1;
    memset(s_doorRecord, 0, sizeof(s_doorRecord));
    s_doorRecord[0x08] = kExitDoor[from][0];
    s_doorRecord[0x09] = kExitDoor[from][1];
    s_doorRecord[0x0A] = kExitDoor[from][2];
    s_doorRecord[0x0B] = (unsigned char)((kExitDoor[from][3] & 0x3F) | (kExitDoor[from][4] & 0x40));
    s_doorRecord[0x0D] = (unsigned char)to;
    *(unsigned short*)(s_doorRecord + 0x0E) = (unsigned short)kEntry[to][1];
    *(short*)(s_doorRecord + 0x10) = 0;
    *(unsigned short*)(s_doorRecord + 0x12) = (unsigned short)kEntry[to][2];
    *(short*)(s_doorRecord + 0x14) = kEntry[to][0];
    s_doorRecord[0x16] = 0xFF;

    dbg_printf("[battle] room %d clear - on to room %d\n", from, to);
    s_leaving = 1;
    g_pendingDoorRecord = (int)(intptr_t)s_doorRecord;
    g_main_state_flags |= MSF_GAMEPLAY_ACTIVE;
    g_message_flags = 0;

    g_rect.textureId = 0;
    g_rect.x = -160;
    g_rect.y = -120;
    g_rect.w = 320;
    g_rect.h = 240;
    g_rect.r = 0;
    g_rect.g = 0;
    g_rect.b = 0;
    g_openMenuFlag = 1;
    draw_rect(&g_rect, 0, 0);
    Task_sleep(1);
    StMask(0, 0);
}

void battle_frame(void)
{
    if (!g_battleActive || g_battleRoom == 0) {
        return;
    }
    const int dead = (g_main_state_flags & MSF_PLAYER_DEAD) != 0;

    // Testing aid ([BattleGame] Invincible=1): top the player's health back
    // up every frame, so ordinary hits never add up to a death. Moves that
    // kill outright (a Hunter's decapitation, Yawn's swallow, the Tyrant's
    // impale) still play their own death animation.
    if (g_bBattleInvincible && !dead) {
        g_playerEntity.health = (g_playerEntity.id & 1) ? 96 : 140;   // full, as scored
    }

    if (s_finishing) {
        if (g_battleEndingCam) battle_ending_camera();
        s_endingFrame++;
        if (s_endingFrame < 75) {                // the first 2.5 s
            battle_print(160 - 12 * 8 / 2, 112, 0x00, "STAGE CLEAR!");   // white
        }
        if (!s_endingFade) {
            if (s_endingFrame >= BATTLE_ENDING_FRAMES - BATTLE_ENDING_FADE) {
                s_endingFade = 1;
                g_fade_type_id = 2;
                g_fading_counter = 0x1000;
                fade_update();
            }
            return;
        }
        if ((short)g_fading_state < 0) {
            g_battleEndingCam = 0;
            g_main_state_flags &= ~MSF_CAMERA_LOCK;
            BattleResult r = battle_score();
            s_result = r;
            battle_rank_load();
            s_resultPlace = battle_rank_insert(r);
            battle_rank_save();
            dbg_printf("[battle] finished: score %d rank %c time %u\n", r.score, r.rank, r.ticks60);
            battle_bgm_stop();
            // Leave game_loop the way its F9 reset does.
            StMask(0, 3);
            g_main_state_flags2 &= MSF2_RESET_KEEP_MASK;
            g_main_state_flags = (g_main_state_flags & ~(MSF_SCREEN_MODE_MASK | MSF_CONTINUE_GAME)) | MSF_SCREEN_STANDALONE;
            Task_chain((void*)battle_results_state);
        }
        return;
    }

    if (!dead && !s_leaving) {
        s_ticks60 += 2;     // the engine ticks at 30 Hz
    }

    // Tougher Battle Game bosses. Each entity sets its own health in its init
    // state (state 0), which runs after enemy_set, so the boost waits for the
    // init to have happened (state != 0) and is applied exactly once.
    //   Zombie Wesker: 3x the health his zombie init rolled.
    //   Golden Tyrant: 700 - with the PC's Tyrant damage (GL explosive / acid
    //   100, magnum 80) that is 9 magnum rounds (8 x 80 = 640 < 700) or 7
    //   grenade rounds of either kind.
    if (g_battleRoom == BATTLE_ROOM_ID_WESKER && !s_weskerBoosted && s_wesker != NULL &&
        (s_wesker->status_flags & 1) != 0 && s_wesker->state != 0 && s_wesker->health > 0) {
        int h = s_wesker->health * 3;
        s_wesker->health = (short)(h > 0x7FFF ? 0x7FFF : h);
        s_weskerBoosted = 1;
        dbg_printf("[battle] Zombie Wesker health %d\n", (int)s_wesker->health);
    }
    if (g_battleRoom == BATTLE_ROOM_ID_TYRANT && !s_tyrantBoosted && !battle_man_spider()) {
        for (int n = 0; n < 30; n++) {
            Entity* e = &g_EnemiesList[n];
            if ((e->status_flags & 1) == 0 || (e->id != 0x0C && e->id != 0x10) ||
                e->state == 0 || e->health <= 0) {
                continue;
            }
            e->health = 700;
            s_tyrantBoosted = 1;
            dbg_printf("[battle] Golden Tyrant health %d\n", (int)e->health);
            break;
        }
    }

    if (g_battleRoom == BATTLE_ROOM_ID_TYRANT && !s_goldApplied && !battle_man_spider()) {
        battle_apply_gold_tyrant();
    }

    // HUD: the running clear time, top left, in green.
    {
        char t[16];
        battle_format_time(t, sizeof(t), s_ticks60);
        battle_print(8, 8, BATTLE_HUD_GREEN, t);
    }

    // "STAGE CLEAR!" once a fight room's enemies are all down, until the
    // game moves on (or, after the last room, fades to the results).
    if (kFightRoom[g_battleRoom] && !dead && s_clearHold > 0) {
        static const char kClear[] = "STAGE CLEAR!";
        battle_print(160 - ((int)sizeof(kClear) - 1) * 8 / 2, 112, 0x00, kClear);   // white
    }

    if (dead || s_leaving || !kFightRoom[g_battleRoom]) {
        return;
    }
    if (!battle_room_clear()) {
        s_clearHold = 0;
        return;
    }
    if (++s_clearHold < BATTLE_CLEAR_HOLD) {
        return;
    }
    if (g_openMenuFlag != 0 || (g_main_state_flags & (MSF_MENU_ACTIVE | MSF_DOOR_TRANSITION)) != 0) {
        return;
    }

    if (g_battleRoom >= BATTLE_ROOMS) {
        s_finishing = 1;
        s_finalHealth = g_playerEntity.health;
        battle_ending_start();                  // the fade comes at its end
        return;
    }
    battle_leave_room();
}
