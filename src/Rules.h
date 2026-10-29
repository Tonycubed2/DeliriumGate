#pragma once

// The yield rules: when Skyrim Delirium must leave an NPC alone.
// Every function returns nullptr when Delirium may act, or a short reason when it must step aside.
// The reasons are static strings (Delirium keeps the pointer for its own log line).
namespace Rules
{
	void SetMainThread();
	bool IsMainThread();

	void OnDataLoaded();     // resolve factions, keywords, plugins
	void RegisterHitSink();  // TESHitEvent: real hits (for ragdoll takeovers) and blocked-plugin hits

	// Delirium wants to start on this NPC (a hit, a stagger, a bump, its test key)
	const char* StartReason(RE::Actor* a_actor);
	// Delirium wants to take over Skyrim's ragdoll on this NPC
	const char* RagdollReason(RE::Actor* a_actor);
	// Delirium wants to take this killing blow ("dying on her feet")
	const char* DeathReason(RE::Actor* a_victim, RE::Actor* a_killer);
	// Delirium already has this NPC: must it hand her back? (main thread only)
	const char* HeldReason(RE::Actor* a_actor);

	bool IsCarnageActor(RE::Actor* a_actor);

	// the gate gave this killing blow to the game (kill move, Carnage, player kill): Delirium may not
	// pick the corpse up afterwards either ("died into Skyrim's ragdoll: taking over", dying writhe).
	// a_reason: the reason DeathReason gave (an FEC death keeps its corpse away from Delirium for good).
	void NoteYieldedDeath(RE::Actor* a_victim, const char* a_reason = nullptr);

	void LogYield(RE::Actor* a_actor, const char* a_where, const char* a_reason);
}
