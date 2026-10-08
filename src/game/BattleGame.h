// BattleGame.h - the Sega Saturn release's Battle Game (port-added mod)
//
// See BattleGame.cpp for how the mode works and what it was taken from.
#pragma once

struct Entity;

// 1 from the moment BATTLE GAME is confirmed on the title screen until the
// player is back at the title. Everything below is a no-op while it is 0.
extern int g_battleActive;

// The battle room the player is in, 1..15 (the Saturn's STAGE8 room numbers:
// ROOM8010.RDT is room 1). 0 when no battle is running.
extern int g_battleRoom;

// --- title screen ------------------------------------------------------------

// The title menu shows BATTLE GAME: OG mode, the USA tree, the game finished
// at least once ([Player] ClearCount) or [BattleGame] AlwaysUnlocked=1, and the
// mode's data present (data/t_battle.tim and battle/ROOM8010.RDT).
int  battle_title_option_available(void);
// Confirmed on the title (before the character select) / backed out of it.
void battle_arm(void);
void battle_disarm(void);

// --- game start --------------------------------------------------------------

// InitializeGame, new-game branch: start room, loadout, health, timer.
void battle_new_game_setup(void);

// --- rooms -------------------------------------------------------------------

// LoadRoomRdt: the battle room's file (battle/ROOM80x0.RDT) instead of the
// base room's. Returns 1 and fills `path` when it applies.
int  battle_rdt_path(char* path, unsigned int size);

// room_transition_load: a door record's destination byte is a battle room
// number. Points g_stageId / g_roomId at that room's base room and returns
// 1 when the stage changed (the caller then runs init_room, else room_set).
int  battle_enter_room(unsigned char dest);

// cmd_enemy_set, after the entity is set up: records the enemy's death flag
// for the room-clear test and applies the Saturn's per-room enemy changes
// (Zombie Wesker in room 12).
void battle_on_enemy_set(Entity* enemy, unsigned char slot);

// LoadEntityEMD: a Battle Game model in place of the table's (Zombie Wesker).
// Returns NULL when the table entry stands.
const char* battle_emd_override(Entity* em, unsigned char tableIndex);

// --- every frame -------------------------------------------------------------

// game_loop, once per gameplay frame (not while paused): the timer, the clear
// test, the move to the next room, the results at the end, and the HUD.
void battle_frame(void);

// 1 while the end-of-run camera circles the player (background, room masks
// and enemies hidden, the pad ignored).
extern int g_battleEndingCam;

// SoundSystem.cpp: stop the Battle Game music (sound\battle.wav).
void battle_bgm_stop(void);
// 1 while the current Battle Game room is a safe room (rooms 1, 6, 11).
int  battle_in_safe_room(void);
// 1 in the boss rooms 5, 8 and 10, which play Yawn's boss music (Bgm_08).
int  battle_in_boss_room(void);
// 1 in room 15, which plays the Tyrant battle music (Bgm_24).
int  battle_in_tyrant_room(void);
// SoundSystem.cpp: the results screen's jingle (sound\battle_rank.wav), once.
void battle_jingle_play(void);
// SoundSystem.cpp: the ranking page's music, the staff roll's credits track
// (sound\battle_credits.wav), looped.
void battle_credits_play(void);

// --- shared helper (EntityModelLoader.cpp) -----------------------------------
bool mod_asset_exists(const char* relPath);
int  battle_man_spider_active(void);   // Mod: room 15 Man Spider in place of the Tyrant
