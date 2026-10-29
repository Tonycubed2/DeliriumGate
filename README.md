# DeliriumGate

SKSE plugin that puts [Skyrim Delirium](https://www.nexusmods.com/skyrimspecialedition/mods/196333) by Zorkanne33 last in line.
Delirium only plays its vanilla reactions and steps aside for kill moves, paired animations, OStim scenes and
Maximum Carnage deaths. Delirium itself is not modified, and none of its files are included here.

License: GPL-3.0-or-later (see `LICENSE`). Third-party notices: `THIRD_PARTY_NOTICES.txt`.

```
Targets: Skyrim AE 1.6.1170 and 1.7.99 (Address Library, version independent).
Requires: SKSE, Address Library for SKSE Plugins, Skyrim Delirium 1.4.4 or 1.4.5 (Nexus 196333).
Works with: Maximum Carnage (MaximumCarnage.esp), VioLens and vanilla kill moves, OStim NG.

Built against CommonLibSSE-NG 6.7.0 at C:\dllmaker\CommonLibSSE-NG
(taken from the CommonLibSSEPath_NG environment variable when it is set). Uses MinHook (vcpkg).

What it does
------------
Skyrim Delirium goes last. It only plays its vanilla reactions (blows, arrows, spells, staggers,
bumps, explosions, Skyrim's own ragdolls) and steps aside when something else has the NPC:

  Kill moves      An NPC in a kill move (victim, or her attacker is in one) is left alone, killing
                  blow included. Delirium can no longer switch kill moves / kill cams off, and the
                  KillMove global it left at 0 in your save is set back to 1 on load.
  Paired anims    An NPC in a synced animation (bIsSynced) is left alone.
  Maximum Carnage NPCs that carry a Carnage gore spell die the vanilla way: Delirium does not hold
                  them at 1 health ("dying on her feet"), so Carnage gets the death and does its
                  gore. One that dies while Delirium is driving her is handed back at once.
  Other mods      OStim scene actors (OStim.esp faction 0xD93) by default, plus any factions,
                  keywords, magic effects or hit-source plugins listed in DeliriumGate.ini.
  Ragdolls        Delirium takes over Skyrim's ragdoll only when a real hit put the NPC there.
                  A script push, a grab or another mod's ragdoll is left to that mod.

An NPC Delirium is already driving is handed back (through Delirium's own Release) within one
watch tick (50 ms) of any of these starting.

How (Delirium itself is not changed)
------------------------------------
SkyrimDelirium.dll loads a copy of SkyrimDeliriumCore.dll (so it can reload it). DeliriumGate
finds that copy by its DeliriumCore_Get export and hooks five functions in it (MinHook):
  Engine::Eligible     the check Delirium makes before taking an NPC
  Engine::Activate     where Delirium really takes an NPC
  Engine::TryTakeOver  Delirium taking over Skyrim's ragdoll
  Engine::OnKill       Delirium taking a killing blow (dying on her feet)
  Engine::NoKillMoves  Delirium switching kill moves / kill cams off
and calls Engine::Release to hand an NPC back. Addresses are from the 1.4.4 core and its PDB;
the hook bytes are checked before hooking. On another Delirium build the five hooks are found
by pattern (each one only if it is unique), and Release is off until DeliriumGate is updated.
After Delirium's console "reload" the new core is found and hooked again (checked every 2 s).

Delirium's own INI
------------------
"SkyrimDelirium.ini (kill moves on)" is Delirium 1.4.4's SkyrimDelirium.ini with
bNoKillMoves=0 and bNoKillCams=0 ([Deaths]). DeliriumGate already stops those two settings, but
with them at 0 Delirium never touches kill moves even before the gate hooks it. To use it, rename
it to SkyrimDelirium.ini and put it in place of Delirium's own (SKSE\Plugins\SkyrimDelirium.ini).

One build folder only: build
Output: build\Release\DeliriumGate.dll and .pdb

Build (Developer Command Prompt for VS 2022), one line at a time:
  cd /d C:\dllmaker\DeliriumGate
  cmake --preset vs2022-windows
  cmake --build build --config Release

Install (its own MO2 mod, any priority):
  SKSE\Plugins\DeliriumGate.dll
  SKSE\Plugins\DeliriumGate.pdb
  SKSE\Plugins\DeliriumGate.ini

1.0.7: Skyrim Delirium 1.4.5 is a known build (addresses from its PDB), so handing NPCs back stays on.
  1.4.5 has the same hooked functions and the same engine layout as 1.4.4; only the addresses moved.

1.0.6: [KillMoves] bPlayerKillMoveWatch (on): after one of the player's kill moves, if fighting /
  menu controls stay off (walks, but no weapon and no menus), EnablePlayerControls is run (up to 3
  tries, not while another mod has the player or a menu pauses the game). A kill move flag stuck on
  the player for 10 s with no paired animation is cleared. Logged as "Player kill move: ...".

1.0.5: the 30 s "corpse of a death Delirium did not get" rule only applies to a really dead NPC.
  1.0.4 also applied it to essential followers (Willow, Herika, Inigo) after a "killing blow" that
  only knocked them down, keeping Delirium off them while alive.

1.0.4: a death the gate gives to the game (kill move, Carnage, player kill) stays the game's: for
  30 s Delirium may not pick the corpse up ("died into Skyrim's ragdoll: taking over" and its dying
  writhe). Dead Carnage NPCs are also refused when Delirium tries to take them directly (Activate).
  Fixes a Carnage kill-move victim writhing for 2 s after the kill (her torso kept moving).

1.0.3: [DeathCheck] (on by default): 3 s and 8 s after an NPC dies, if she is dead but still not
  in ragdoll (stuck on her feet or looping a death animation), DeliriumGate sends RagdollInstant.
  Logged as "Death check: ...". Own code (not Death Idle Fix's), which still covers loads.

1.0.2: [KillMoves] bYieldPlayerKills (off by default): when 1, Delirium never takes the player's
  killing blows, so kill cams can play on every kill.

1.0.1: "Maximum Carnage corpse" now means really dead (dying/dead life state). 1.0.0 used
  Actor::IsDead(true), which is also true for a knocked-down NPC, so Delirium lost Carnage NPCs
  right after it staggered them. Corpse ragdolls are left to Delirium (Carnage corpses excepted)
  and "ragdoll takeover" lines are logged at most every 30 s per NPC.

Log: Documents\My Games\Skyrim Special Edition\SKSE\DeliriumGate.log
  Each time Delirium is made to step aside: NPC form ID, name, where (killing blow, ragdoll
  takeover, take, handed back...) and why. Delirium's own log shows "released (DeliriumGate: ...)".
```
