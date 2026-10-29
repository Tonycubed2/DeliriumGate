#pragma once

// Death check: a few seconds after an NPC dies (and once more later), if she is dead but still not in
// Skyrim's ragdoll (stuck on her feet or looping a death animation), send "RagdollInstant" so she drops.
// Death Idle Fix covers bodies after loading screens and cell changes; this covers the moment of death.
//
// Stuck dying: when Delirium takes a killing blow it keeps the NPC alive at 1 health and plays the death
// itself ("dying on her feet", up to 3 s). Sometimes that death never finishes: the NPC stays alive,
// not fighting, in a broken pose, until the next loading screen. fStuckDyingSeconds after the killing
// blow DeliriumGate first makes Delirium let her go (its release finishes the death), then, if she is
// still alive, kills her itself (the game's own death, credited to the same killer).
namespace DeathCheck
{
	void RegisterDeathSink();  // TESDeathEvent
	void Tick();               // main thread, from the watch

	// Delirium took this killing blow (Engine::OnKill returned true). Any thread.
	void NoteHeldDeath(RE::Actor* a_victim, RE::Actor* a_killer);
	// Delirium is still in the middle of this NPC's death (killing blow taken, not dead yet). Any thread.
	bool HeldDeath(RE::Actor* a_actor);
	// DeliriumGate itself is finishing this NPC's death right now: Delirium must not take it again.
	bool Finishing(RE::Actor* a_actor);
}
