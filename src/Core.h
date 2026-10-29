#pragma once

// Skyrim Delirium's core DLL (SkyrimDeliriumCore.dll, loaded by SkyrimDelirium.dll as a copy so it can be
// reloaded): finding it, hooking it, and asking it to let an NPC go.
namespace Core
{
	// Look for the core and hook it if it is new (first load or a reload). Safe to call often.
	void Install(const char* a_when);

	bool Hooked();         // the gate hooks are in the current core
	bool CanRelease();     // the build is known, so the gate can hand NPCs back (Release)

	// Handles of the NPCs Delirium is driving right now (main thread).
	std::vector<std::uint32_t> ActiveHandles();
	// Make Delirium let this NPC go, through its own Release (main thread). True if she was in it.
	bool Release(std::uint32_t a_handle, const char* a_reason);

	// What Delirium's Engine::NoKillMoves would have touched: warn if it already ran before the hook.
	void CheckKillMoveSettings(const char* a_when);
}
