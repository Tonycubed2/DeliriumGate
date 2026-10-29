// DeliriumGate 1.0.9
// Built against CommonLibSSE-NG 6.7.0 for Skyrim AE 1.6.1170 and 1.7.99.
//
// Puts Skyrim Delirium (Nexus 196333) last in line. Delirium only plays its vanilla reactions
// (hits, arrows, spells, staggers, bumps, explosions, Skyrim's own ragdolls) and steps aside when:
//   - a kill move has the NPC (or her attacker); Delirium may no longer switch kill moves / kill cams off
//   - the NPC is in a paired / synced animation
//   - Maximum Carnage is going to take her death (she dies the vanilla way, Carnage does its gore)
//   - another mod has her (OStim scene faction by default, plus factions/keywords/effects in the INI)
//   - Skyrim's ragdoll was not caused by a real hit (a script push, a grab, another mod's ragdoll)
// Player kill move watchdog: a stuck kill move flag is cleared, and fighting/menu controls left off
// after one of the player's kill moves are switched back on.
// Death check: an NPC dead but still not in ragdoll 3 s / 8 s after death gets RagdollInstant.
// An NPC Delirium already drives is handed back through Delirium's own Release when one of these starts.
//
// Delirium itself is not changed: DeliriumGate hooks its core DLL in memory (MinHook).
// Log: Documents\My Games\Skyrim Special Edition\SKSE\DeliriumGate.log
// INI: Data\SKSE\Plugins\DeliriumGate.ini

#include "Core.h"
#include "DeathCheck.h"
#include "Rules.h"
#include "Settings.h"
#include "Watch.h"

SKSEPluginInfo(
	.Version = REL::Version{ 1, 0, 9, 0 },
	.Name = "DeliriumGate"sv,
	.Author = "tonycubed2"sv,
	.StructCompatibility = SKSE::StructCompatibility::Independent,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

namespace
{
	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			Core::Install("post load");
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			Core::Install("post post load");
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			Core::Install("input loaded");
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			Rules::SetMainThread();
			Core::Install("data loaded");
			Rules::OnDataLoaded();
			Rules::RegisterHitSink();
			DeathCheck::RegisterDeathSink();
			Watch::Start();
			Core::CheckKillMoveSettings("data loaded");
			if (!Core::Hooked()) {
				logger::warn("Skyrim Delirium's core is not loaded yet; the gate hooks it as soon as it shows up");
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Core::Install("save loaded");
			Core::CheckKillMoveSettings("save loaded");
			break;
		case SKSE::MessagingInterface::kNewGame:
			Core::Install("new game");
			Core::CheckKillMoveSettings("new game");
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	Rules::SetMainThread();

	logger::info("DeliriumGate 1.0.9 loading; game runtime {}", REL::Module::get().version().string());

	Settings::Load();
	if (!Settings::enabled) {
		logger::info("Disabled in DeliriumGate.ini (bEnabled=0): Delirium runs as it is");
		return true;
	}

	// If Delirium's core is already in, hook it now: before its Init can switch kill moves off.
	Core::Install("plugin load");

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
		logger::critical("Could not register SKSE messaging listener");
		return false;
	}

	logger::info("Loaded");
	return true;
}
