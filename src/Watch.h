#pragma once

// A light poll (every iWatchIntervalMs) that runs a check on the main thread:
//  - an NPC Delirium already drives is handed back when a kill move, a paired animation, another mod
//    or Maximum Carnage (her death) takes her
//  - every few seconds: is Delirium's core still the one we hooked (its console "reload" swaps it)
namespace Watch
{
	void Start();
}
