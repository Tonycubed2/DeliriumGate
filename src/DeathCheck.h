#pragma once

// Death check: a few seconds after an NPC dies (and once more later), if she is dead but still not in
// Skyrim's ragdoll (stuck on her feet or looping a death animation), send "RagdollInstant" so she drops.
// Death Idle Fix covers bodies after loading screens and cell changes; this covers the moment of death.
namespace DeathCheck
{
	void RegisterDeathSink();  // TESDeathEvent
	void Tick();               // main thread, from the watch
}
