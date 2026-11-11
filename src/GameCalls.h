#pragma once

// Game functions DeliriumGate calls by Address Library ID instead of through CommonLib's virtual
// declarations.
//
// CommonLibSSE-NG 6.7.0 declares one virtual function too many in TESObjectREFR (around slot 0x88),
// so every TESObjectREFR / Actor virtual from there on is called one slot too far on SE and AE:
//   actor->KillImpl(...)       called slot 0x10F = Actor::DrinkPotion (the killer was "drunk" as a potion)
//   actor->IsInFaction(f)      called slot 0xFA  = Actor::ForEachPerk (ran the faction's destructor)
//   actor->IsDead(b)           called slot 0x9A  = TESObjectREFR::CreateAnimNoteReceiver
// Checked against the Character vtable of SkyrimSE.exe 1.5.97 (VTABLE_Character 261397) and
// 1.6.1170 (207886): IsDead is slot 0x99, IsInFaction 0xF9, KillImpl 0x10E, and those slots hold
// exactly the functions below.
namespace GameCalls
{
	// TESObjectREFR::IsDead / Actor::IsDead (slot 0x99): SE 36484, AE 37483
	inline bool IsDead(RE::Actor* a_actor, bool a_notEssential = true)
	{
		using func_t = bool (*)(RE::Actor*, bool);
		static REL::Relocation<func_t> func{ RELOCATION_ID(36484, 37483) };
		return func(a_actor, a_notEssential);
	}

	// Actor::IsInFaction (slot 0xF9): SE 36679, AE 37687
	inline bool IsInFaction(RE::Actor* a_actor, const RE::TESFaction* a_faction)
	{
		using func_t = bool (*)(RE::Actor*, const RE::TESFaction*);
		static REL::Relocation<func_t> func{ RELOCATION_ID(36679, 37687) };
		return func(a_actor, a_faction);
	}

	// Actor::KillImpl (slot 0x10E): SE 36872, AE 37896
	inline void KillImpl(RE::Actor* a_actor, RE::Actor* a_attacker, float a_damage, bool a_sendEvent, bool a_ragdollInstant)
	{
		using func_t = void (*)(RE::Actor*, RE::Actor*, float, bool, bool);
		static REL::Relocation<func_t> func{ RELOCATION_ID(36872, 37896) };
		func(a_actor, a_attacker, a_damage, a_sendEvent, a_ragdollInstant);
	}
}
