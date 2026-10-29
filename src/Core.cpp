#include "Core.h"
#include "DeathCheck.h"
#include "Rules.h"
#include "Settings.h"

#include <Windows.h>
#include <Psapi.h>
#include <MinHook.h>

// Reverse engineered from Skyrim Delirium 1.4.4 (Nexus 196333), SkyrimDeliriumCore.dll built
// 2026-10-27 02:10:03 UTC (PE time stamp 0x6AE0403B, image size 0x2F8000), with its own PDB.
// 1.4.5 (time stamp 0x6AE1C2B0, image size 0x2FD000): same functions, same signatures, same first bytes,
// same Engine layout; only the RVAs moved (table kKnownBuilds below). RVAs below are 1.4.4's.
//
//   delirium::Engine::Eligible(Actor*, const char* why, bool quiet) const -> bool    RVA 0x0A4360
//       the check before Delirium takes an NPC; React, OnHit, OnBump and OnKill all call it
//   delirium::Engine::Activate(Actor*, bool, bool) -> bool                           RVA 0x0C4860
//       where Delirium really takes an NPC; OnHit, React, TryTakeOver, DyingOnFeet, OnKey call it
//   delirium::Engine::TryTakeOver(Actor*, bool dead)                                 RVA 0x0C8540
//       called every frame for an NPC in Skyrim's ragdoll (from AfterAnimation)
//   delirium::Engine::OnKill(Actor* victim, Actor* killer, float, bool, bool) -> bool RVA 0x0BB060
//       true = Delirium keeps her alive at 1 health and plays the death itself ("dying on her feet")
//   delirium::Engine::NoKillMoves()                                                  RVA 0x00E060
//       sets the KillMove global to 0 (saved in the save!) and fKillCamBaseOdds / bVATSDisable
//   delirium::Engine::Release(size_t index, const char* why)                         RVA 0x0C8260
//       unshows, restores joints, erases m_active[index], updates g_count; called under the lock
//   delirium::Engine::Get() -> Engine&                                               RVA 0x00BAB0
//   std::atomic<int> delirium::g_count (NPCs Delirium drives)                        RVA 0x2D00F4
//   Engine + 0x78: std::mutex (the lock AfterAnimation takes before Release)
//   Engine + 0xC8 / 0xD0: std::vector<std::unique_ptr<Active>> begin / end
//   Active + 0x00: the NPC's handle (uint32)
namespace Core
{
	namespace
	{
		// Builds read from their own PDBs. Engine layout (lock, active list, Active::handle) is the same in both.
		struct KnownBuild
		{
			const char*   name;
			DWORD         stamp;
			DWORD         imageSize;
			std::uint32_t eligible, activate, tryTakeOver, onKill, noKillMoves;
			std::uint32_t get, release, count;
		};
		constexpr KnownBuild kKnownBuilds[] = {
			{ "Skyrim Delirium 1.4.4", 0x6AE0403B, 0x2F8000, 0x0A4360, 0x0C4860, 0x0C8540, 0x0BB060, 0x00E060, 0x00BAB0, 0x0C8260, 0x2D00F4 },
			{ "Skyrim Delirium 1.4.5", 0x6AE1C2B0, 0x2FD000, 0x0A4750, 0x0C4C50, 0x0C8930, 0x0BB450, 0x00E070, 0x00BAC0, 0x0C8650, 0x2D50F4 },
		};

		constexpr std::size_t   kOffLock = 0x78;
		constexpr std::size_t   kOffActiveBegin = 0xC8;
		constexpr std::size_t   kOffActiveEnd = 0xD0;

		// ---- originals ----
		using Eligible_t = bool (*)(void*, RE::Actor*, const char*, bool);
		using Activate_t = bool (*)(void*, RE::Actor*, bool, bool);
		using TryTakeOver_t = void (*)(void*, RE::Actor*, bool);
		using OnKill_t = bool (*)(void*, RE::Actor*, RE::Actor*, float, bool, bool);
		using NoKillMoves_t = void (*)(void*);
		using Release_t = void (*)(void*, std::uint64_t, const char*);
		using Get_t = void* (*)();
		using Mtx_t = int (*)(void*);

		Eligible_t    g_eligible = nullptr;
		Activate_t    g_activate = nullptr;
		TryTakeOver_t g_tryTakeOver = nullptr;
		OnKill_t      g_onKill = nullptr;
		NoKillMoves_t g_noKillMoves = nullptr;
		Release_t     g_release = nullptr;
		Get_t         g_get = nullptr;
		Mtx_t         g_mtxLock = nullptr;
		Mtx_t         g_mtxUnlock = nullptr;

		std::atomic<std::uintptr_t> g_base{ 0 };
		std::atomic<void*>          g_engine{ nullptr };
		std::atomic<int>*           g_count = nullptr;
		std::atomic<bool>           g_hooked{ false };
		std::atomic<bool>           g_canRelease{ false };
		std::mutex                  g_installLock;
		bool                        g_mhReady = false;

		void Capture(void* a_engine)
		{
			if (a_engine) {
				g_engine.store(a_engine, std::memory_order_relaxed);
			}
		}

		// ---- detours ----
		bool HookEligible(void* a_engine, RE::Actor* a_actor, const char* a_why, bool a_quiet)
		{
			Capture(a_engine);
			if (Settings::enabled) {
				if (const char* reason = Rules::StartReason(a_actor)) {
					Rules::LogYield(a_actor, a_why ? a_why : "start", reason);
					return false;
				}
			}
			return g_eligible(a_engine, a_actor, a_why, a_quiet);
		}

		bool HookActivate(void* a_engine, RE::Actor* a_actor, bool a_b1, bool a_b2)
		{
			Capture(a_engine);
			if (Settings::enabled) {
				if (const char* reason = Rules::StartReason(a_actor)) {
					Rules::LogYield(a_actor, "take", reason);
					return false;
				}
			}
			return g_activate(a_engine, a_actor, a_b1, a_b2);
		}

		void HookTryTakeOver(void* a_engine, RE::Actor* a_actor, bool a_dead)
		{
			Capture(a_engine);
			if (Settings::enabled) {
				if (const char* reason = Rules::RagdollReason(a_actor)) {
					Rules::LogYield(a_actor, "ragdoll takeover", reason);
					return;
				}
			}
			g_tryTakeOver(a_engine, a_actor, a_dead);
		}

		bool HookOnKill(void* a_engine, RE::Actor* a_victim, RE::Actor* a_killer, float a_damage, bool a_b1, bool a_b2)
		{
			Capture(a_engine);
			if (Settings::enabled) {
				if (const char* reason = Rules::DeathReason(a_victim, a_killer)) {
					Rules::LogYield(a_victim, "killing blow", reason);
					Rules::NoteYieldedDeath(a_victim);  // and Delirium may not pick up the corpse afterwards
					return false;  // the game's own death goes through: Carnage / the kill move get it
				}
			}
			const bool held = g_onKill(a_engine, a_victim, a_killer, a_damage, a_b1, a_b2);
			if (held) {
				DeathCheck::NoteHeldDeath(a_victim, a_killer);  // watched: finished if Delirium leaves it hanging
			}
			return held;
		}

		void HookNoKillMoves(void* a_engine)
		{
			Capture(a_engine);
			if (Settings::enabled && Settings::protectKillMoves) {
				static std::atomic<bool> logged{ false };
				if (!logged.exchange(true)) {
					logger::info("Kept Delirium from switching kill moves / kill cams off ([KillMoves] bProtectKillMoves=1)");
				}
				return;
			}
			g_noKillMoves(a_engine);
		}

		// ---- finding the core ----
		std::uintptr_t FindCoreModule()
		{
			HMODULE modules[1024];
			DWORD   needed = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
				return 0;
			}
			const auto count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(modules));
			for (std::size_t i = 0; i < count; ++i) {
				if (GetProcAddress(modules[i], "DeliriumCore_Get")) {
					return reinterpret_cast<std::uintptr_t>(modules[i]);
				}
			}
			return 0;
		}

		std::wstring ModulePath(std::uintptr_t a_base)
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(reinterpret_cast<HMODULE>(a_base), path, MAX_PATH);
			return path;
		}

		std::string Narrow(const std::wstring& a_w)
		{
			std::string out;
			for (auto c : a_w) {
				out.push_back(c < 128 ? static_cast<char>(c) : '?');
			}
			return out;
		}

		const IMAGE_NT_HEADERS64* NtHeaders(std::uintptr_t a_base)
		{
			auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
			return reinterpret_cast<const IMAGE_NT_HEADERS64*>(a_base + dos->e_lfanew);
		}

		// unique match of a byte pattern in the module's code; 0 if none or more than one
		std::uintptr_t ScanUnique(std::uintptr_t a_base, const std::vector<std::uint8_t>& a_pattern)
		{
			const auto* nt = NtHeaders(a_base);
			const auto* section = IMAGE_FIRST_SECTION(nt);
			std::uintptr_t found = 0;
			for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
				if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
					continue;
				}
				auto* begin = reinterpret_cast<const std::uint8_t*>(a_base + section->VirtualAddress);
				auto* end = begin + section->Misc.VirtualSize;
				auto* it = begin;
				while (true) {
					it = std::search(it, end, a_pattern.begin(), a_pattern.end());
					if (it == end) {
						break;
					}
					if (found) {
						return 0;  // not unique
					}
					found = reinterpret_cast<std::uintptr_t>(it);
					++it;
				}
			}
			return found;
		}

		struct HookDef
		{
			const char*               name;
			std::uint32_t             KnownBuild::*rva;
			std::vector<std::uint8_t> pattern;  // the function's first bytes in 1.4.4 (unique there)
			void*                     detour;
			void**                    original;
		};

		std::vector<HookDef> HookDefs()
		{
			return {
				{ "Engine::Eligible", &KnownBuild::eligible,
					{ 0x48, 0x89, 0x5C, 0x24, 0x10, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x6C },
					reinterpret_cast<void*>(&HookEligible), reinterpret_cast<void**>(&g_eligible) },
				{ "Engine::Activate", &KnownBuild::activate,
					{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0x68, 0xF9, 0xFF },
					reinterpret_cast<void*>(&HookActivate), reinterpret_cast<void**>(&g_activate) },
				{ "Engine::TryTakeOver", &KnownBuild::tryTakeOver,
					{ 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55 },
					reinterpret_cast<void*>(&HookTryTakeOver), reinterpret_cast<void**>(&g_tryTakeOver) },
				{ "Engine::OnKill", &KnownBuild::onKill,
					{ 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0xF3, 0x0F, 0x11, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41 },
					reinterpret_cast<void*>(&HookOnKill), reinterpret_cast<void**>(&g_onKill) },
				{ "Engine::NoKillMoves", &KnownBuild::noKillMoves,
					{ 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC },
					reinterpret_cast<void*>(&HookNoKillMoves), reinterpret_cast<void**>(&g_noKillMoves) },
			};
		}

		bool HookOne(std::uintptr_t a_base, const KnownBuild* a_known, const HookDef& a_def)
		{
			std::uintptr_t target = 0;
			if (a_known) {
				const auto rva = a_known->*(a_def.rva);
				target = a_base + rva;
				if (std::memcmp(reinterpret_cast<const void*>(target), a_def.pattern.data(), a_def.pattern.size()) != 0) {
					logger::error("  {}: bytes at RVA {:X} are not what {} has, not hooked", a_def.name, rva, a_known->name);
					return false;
				}
			} else {
				target = ScanUnique(a_base, a_def.pattern);
				if (!target) {
					logger::error("  {}: not found in this Delirium build, not hooked", a_def.name);
					return false;
				}
			}

			auto status = MH_CreateHook(reinterpret_cast<void*>(target), a_def.detour, a_def.original);
			if (status == MH_ERROR_ALREADY_CREATED) {
				// a reloaded core mapped at the same address as the old one: drop the stale entry, hook again
				MH_RemoveHook(reinterpret_cast<void*>(target));
				status = MH_CreateHook(reinterpret_cast<void*>(target), a_def.detour, a_def.original);
			}
			if (status != MH_OK) {
				logger::error("  {}: MH_CreateHook failed ({})", a_def.name, MH_StatusToString(status));
				return false;
			}
			status = MH_EnableHook(reinterpret_cast<void*>(target));
			if (status != MH_OK) {
				logger::error("  {}: MH_EnableHook failed ({})", a_def.name, MH_StatusToString(status));
				return false;
			}
			logger::info("  {} hooked at core+{:X}", a_def.name, target - a_base);
			return true;
		}

		void* Engine()
		{
			if (auto* e = g_engine.load(std::memory_order_relaxed)) {
				return e;
			}
			if (g_get && Rules::IsMainThread()) {
				auto* e = g_get();
				g_engine.store(e);
				return e;
			}
			return nullptr;
		}
	}

	void Install(const char* a_when)
	{
		std::scoped_lock lock(g_installLock);

		const auto base = FindCoreModule();
		if (!base) {
			if (g_base.load()) {
				logger::warn("[{}] Delirium's core is no longer loaded; the gate waits for it", a_when);
				g_base = 0;
				g_hooked = false;
				g_canRelease = false;
				g_engine = nullptr;
			}
			return;
		}
		if (base == g_base.load()) {
			return;  // already done for this core
		}

		const bool reload = g_base.load() != 0;
		g_base = base;
		g_hooked = false;
		g_canRelease = false;
		g_engine = nullptr;
		g_count = nullptr;
		g_release = nullptr;
		g_get = nullptr;

		const auto*       nt = NtHeaders(base);
		const KnownBuild* known = nullptr;
		for (const auto& build : kKnownBuilds) {
			if (nt->FileHeader.TimeDateStamp == build.stamp && nt->OptionalHeader.SizeOfImage == build.imageSize) {
				known = &build;
				break;
			}
		}
		logger::info("[{}] Delirium core {}found: {} at {:X} (time stamp {:08X}, size {:X}) - {}",
			a_when, reload ? "re" : "", Narrow(ModulePath(base)), base, nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
			known ? std::string(known->name) + ", known build" : std::string("unknown build, searching by pattern"));

		if (!g_mhReady) {
			auto status = MH_Initialize();
			if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
				logger::critical("MH_Initialize failed ({}), the gate is OFF", MH_StatusToString(status));
				return;
			}
			g_mhReady = true;
		}

		int  hooked = 0;
		auto defs = HookDefs();
		for (const auto& def : defs) {
			hooked += HookOne(base, known, def) ? 1 : 0;
		}
		g_hooked = hooked > 0;
		logger::info("[{}] {} of {} gate hooks in place", a_when, hooked, defs.size());
		if (hooked < static_cast<int>(defs.size())) {
			logger::warn("Not every hook is in: Delirium may still act where the missing hook should stop it");
		}

		if (known) {
			if (auto* msvcp = GetModuleHandleW(L"msvcp140.dll")) {
				g_mtxLock = reinterpret_cast<Mtx_t>(GetProcAddress(msvcp, "_Mtx_lock"));
				g_mtxUnlock = reinterpret_cast<Mtx_t>(GetProcAddress(msvcp, "_Mtx_unlock"));
			}
			g_release = reinterpret_cast<Release_t>(base + known->release);
			g_get = reinterpret_cast<Get_t>(base + known->get);
			g_count = reinterpret_cast<std::atomic<int>*>(base + known->count);
			g_canRelease = g_mtxLock && g_mtxUnlock;
			if (g_canRelease) {
				logger::info("[{}] Release is available: NPCs Delirium already has are handed back when another mod takes them", a_when);
			} else {
				logger::error("[{}] _Mtx_lock/_Mtx_unlock not found in msvcp140.dll: Release off", a_when);
			}
		} else {
			logger::warn("[{}] Unknown Delirium build: only the start/death/ragdoll gates are on; handing NPCs back (Release) is off until DeliriumGate is updated for it", a_when);
		}
	}

	bool Hooked()
	{
		return g_hooked.load();
	}

	bool CanRelease()
	{
		return g_canRelease.load();
	}

	std::vector<std::uint32_t> ActiveHandles()
	{
		std::vector<std::uint32_t> handles;
		if (!CanRelease() || !g_count || g_count->load(std::memory_order_relaxed) <= 0) {
			return handles;
		}
		auto* engine = static_cast<std::byte*>(Engine());
		if (!engine) {
			return handles;
		}
		g_mtxLock(engine + kOffLock);
		auto** begin = *reinterpret_cast<std::byte***>(engine + kOffActiveBegin);
		auto** end = *reinterpret_cast<std::byte***>(engine + kOffActiveEnd);
		for (auto** it = begin; it && it != end; ++it) {
			if (*it) {
				handles.push_back(*reinterpret_cast<std::uint32_t*>(*it));
			}
		}
		g_mtxUnlock(engine + kOffLock);
		return handles;
	}

	bool Release(std::uint32_t a_handle, const char* a_reason)
	{
		if (!CanRelease() || !g_release) {
			return false;
		}
		auto* engine = static_cast<std::byte*>(Engine());
		if (!engine) {
			return false;
		}
		bool found = false;
		g_mtxLock(engine + kOffLock);
		auto** begin = *reinterpret_cast<std::byte***>(engine + kOffActiveBegin);
		auto** end = *reinterpret_cast<std::byte***>(engine + kOffActiveEnd);
		for (std::size_t i = 0; begin && begin + i != end; ++i) {
			if (begin[i] && *reinterpret_cast<std::uint32_t*>(begin[i]) == a_handle) {
				g_release(engine, i, a_reason);  // erases the entry itself, same as Delirium's "actor gone" path
				found = true;
				break;
			}
		}
		g_mtxUnlock(engine + kOffLock);
		return found;
	}

	void CheckKillMoveSettings(const char* a_when)
	{
		if (auto* global = RE::TESForm::LookupByID<RE::TESGlobal>(0x100F19)) {  // Skyrim.esm KillMove
			if (global->value == 0.0f) {
				if (Settings::enabled && Settings::restoreKillMoveGlobal) {
					global->value = 1.0f;
					logger::info("[{}] KillMove global was 0 (Delirium's bNoKillMoves leaves that in the save): set back to 1", a_when);
				} else {
					logger::info("[{}] KillMove global is 0: kill moves are off ([KillMoves] bRestoreKillMoveGlobal=0)", a_when);
				}
			}
		}
		if (auto* settings = RE::GameSettingCollection::GetSingleton()) {
			if (auto* odds = settings->GetSetting("fKillCamBaseOdds"); odds && odds->GetFloat() == 0.0f) {
				logger::warn("[{}] fKillCamBaseOdds is 0: kill cams are off. If Delirium did that before the gate was in, set bNoKillCams=0 in SkyrimDelirium.ini", a_when);
			}
		}
	}
}
