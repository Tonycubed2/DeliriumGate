#pragma once

// Player kill move watchdog (main thread, from the watch):
//  - the player still flagged "in a kill move" long after any kill move lasts, and no paired animation
//    running: the flag is cleared
//  - a moment after one of the player's kill moves ends, fighting (draw weapon) and menu controls are
//    still off and no other mod has the player: controls are switched back on (EnablePlayerControls)
// Kill moves can leave the player like that when they are interrupted (the victim's corpse, gore,
// another hit), and they are on again now that Delirium no longer switches them off.
namespace KillMoveWatch
{
	void Tick();
}
