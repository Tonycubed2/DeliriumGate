#pragma once

// Twitch watch (diagnostics only, main thread, from the watch). Changes nothing in the game.
//  - a dead NPC near the player whose bones keep moving long after death ("endless twitching") is
//    logged once with everything that can hold or drive a ragdoll: kill move / paired animation
//    state, knock state, Delirium, and every Havok body of the corpse (motion type, collision
//    filter, speed, awake or asleep, constraints, and which DLL owns the actions and contact
//    listeners attached to it); then a short line every 20 s, and a line when it stops
//  - the game's own "can the player switch to first person" checks (reverse engineered from
//    ThirdPersonState's button handler, SkyrimSE.exe 1.6.1170 +8E7FA0) are logged whenever one of
//    them starts or stops blocking: POV switch control off, script / beast POV lock, the player's
//    knock state, riding a mount that blocks it
//  - the player's weapon being drawn / sheathed is logged while a corpse is twitching
namespace TwitchWatch
{
	void Tick();
}
