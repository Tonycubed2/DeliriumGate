#pragma once

namespace Settings
{
	// [General]
	inline bool  enabled = true;
	inline bool  logYields = true;           // log every time Delirium is made to step aside
	inline float logRepeatSeconds = 3.0f;    // same NPC + same reason is logged at most this often

	// [KillMoves]
	inline bool killMovesYield = true;         // NPC (or its attacker) in a kill move: Delirium stays out
	inline bool protectKillMoves = true;       // Delirium may not switch kill moves / kill cams off
	inline bool restoreKillMoveGlobal = true;  // on load, KillMove global 0 (left in the save by Delirium) -> 1
	inline bool yieldPlayerKills = false;      // the player's killing blows are always the game's own death (kill cams); off by default

	inline bool  playerKillMoveWatch = true;          // player left stuck after a kill move: fix it
	inline float playerKillMoveStuckSeconds = 10.0f;  // kill move flag still on after this long: cleared
	inline float playerKillMoveControlsAfter = 2.0f;  // controls still off this long after it ended: switched on

	// [PairedAnimations]
	inline bool pairedYield = true;  // NPC in a synced/paired animation (bIsSynced): Delirium stays out

	// [Carnage]
	inline std::string carnagePlugin = "MaximumCarnage.esp";
	inline bool        carnageYieldDeaths = true;     // Carnage NPCs die the vanilla way (no "dying on her feet")
	inline bool        carnageReleaseCorpses = true;  // a Carnage NPC that dies while Delirium has her is handed back at once
	inline bool        carnageYieldAll = false;       // Carnage NPCs never get Delirium at all (not even hit reactions)

	// [Dismembering] Dismembering Framework (DismemberingFramework.dll + its .esm)
	inline std::string dfPlugin = "Dismembering Framework.esm";
	inline bool        dfYieldDeaths = true;  // a killing blow DF may dismember is the game's own death (DF needs her really dead)
	inline bool        dfCheckRaces = true;   // only races DF has limbs for (Data\SKSE\DismemberingFramework\*_DATA.json)

	// [FEC] Frozen Electrocuted Combustion (po3_FEC.dll + FEC.esp)
	inline std::string fecPlugin = "FEC.esp";
	inline bool        fecYieldDeaths = true;   // a fire / frost / shock / drain / sun death FEC may show is the game's own death
	inline bool        fecKeepCorpses = true;   // Delirium never picks those corpses up (frozen, charred, ash...)

	// [Ragdoll]
	inline bool  ragdollVanillaOnly = true;  // take over Skyrim's ragdoll only right after a real hit (shout, explosion, blow...)
	inline float ragdollHitWindow = 1.0f;    // seconds after that hit

	// [OtherMods]
	inline std::vector<std::string> factions{ "OStim.esp|0xD93" };  // NPC in one of these factions: another mod has her
	inline std::vector<std::string> keywords;                       // keyword editor IDs on the NPC or her race
	inline std::vector<std::string> effectKeywords;                 // active magic effects with one of these keywords
	inline std::vector<std::string> effectPlugins;                  // active magic effects from these plugins
	inline std::vector<std::string> sourcePlugins;                  // hits whose weapon/spell comes from these plugins
	inline float                    sourceBlockSeconds = 1.0f;      // how long such a hit keeps Delirium away

	// [DeathCheck]
	inline bool  deathCheckEnabled = true;  // a dead NPC still not in ragdoll after these times gets RagdollInstant
	inline float deathCheckFirst = 3.0f;    // seconds after death
	inline float deathCheckSecond = 8.0f;   // and once more

	inline bool  finishStuckDying = true;   // Delirium kept an NPC "dying on her feet" too long: finish her death
	inline float stuckDyingSeconds = 8.0f;  // seconds after the killing blow (Delirium's own limit is 3 s)

	// [Release]
	inline bool          releaseHeld = true;   // an NPC Delirium already has is handed back when another mod takes her
	inline std::uint32_t watchIntervalMs = 50; // how often that is checked (main thread)

	void Load();
}
