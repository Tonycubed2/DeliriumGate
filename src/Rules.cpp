#include "Rules.h"
#include "Settings.h"

namespace Rules
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// Reason strings. Delirium prints the one it is handed in its "released (...)" log line.
		constexpr const char* kKillMove = "DeliriumGate: kill move";
		constexpr const char* kPlayerKill = "DeliriumGate: the player's kill (kill cam)";
		constexpr const char* kPaired = "DeliriumGate: paired animation";
		constexpr const char* kCarnage = "DeliriumGate: Maximum Carnage NPC";
		constexpr const char* kCarnageDeath = "DeliriumGate: Maximum Carnage gets this death";
		constexpr const char* kCarnageCorpse = "DeliriumGate: Maximum Carnage corpse";
		constexpr const char* kYieldedCorpse = "DeliriumGate: corpse of a death Delirium did not get";
		constexpr const char* kCarnageCorpseRagdoll = "DeliriumGate: Maximum Carnage corpse (ragdoll)";  // not logged: corpses ask every frame
		constexpr const char* kFaction = "DeliriumGate: another mod (faction)";
		constexpr const char* kKeyword = "DeliriumGate: another mod (keyword)";
		constexpr const char* kEffect = "DeliriumGate: another mod (magic effect)";
		constexpr const char* kSource = "DeliriumGate: hit from a blocked plugin";
		constexpr const char* kNotVanilla = "DeliriumGate: ragdoll not caused by a vanilla hit";

		std::thread::id g_mainThread{};

		double Now()
		{
			return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
		}

		// ---- resolved forms (filled on the main thread at data loaded, read-only after) ----
		std::vector<RE::TESFaction*> g_factions;
		std::vector<RE::BGSKeyword*> g_keywords;
		std::vector<RE::BGSKeyword*> g_effectKeywords;
		std::vector<const RE::TESFile*> g_effectPlugins;
		std::vector<const RE::TESFile*> g_sourcePlugins;
		const RE::TESFile*              g_carnageFile = nullptr;
		bool                            g_dataLoaded = false;

		// ---- state shared between threads ----
		std::mutex                                       g_lock;
		std::unordered_map<RE::FormID, double>           g_lastRealHit;     // actor -> time of last hit (not from a blocked plugin)
		std::unordered_map<RE::FormID, double>           g_blockedUntil;    // actor -> hit from a blocked plugin keeps Delirium away until
		std::unordered_map<RE::FormID, double>           g_yieldedDeaths;   // actor -> time the gate gave her death to the game
		std::unordered_map<RE::FormID, bool>             g_carnageBase;     // actor base -> has a Carnage spell
		struct Cached
		{
			const char* reason;
			double      time;
		};
		std::unordered_map<RE::FormID, Cached> g_mainChecks;  // actor -> result of the main-thread-only checks
		std::unordered_map<std::uint64_t, double> g_lastLog;  // (actor, reason) -> last log time

		constexpr double kCacheSeconds = 0.5;

		// Really dead (dying or dead). Actor::IsDead(true) is also true while she is knocked down
		// (unconscious life state): 1.0.0 used it and handed living NPCs back right after a stagger.
		bool ReallyDead(RE::Actor* a_actor)
		{
			auto* state = a_actor ? a_actor->AsActorState() : nullptr;
			if (!state) {
				return false;
			}
			const auto life = state->GetLifeState();
			return life == RE::ACTOR_LIFE_STATE::kDying || life == RE::ACTOR_LIFE_STATE::kDead;
		}

		constexpr double kYieldedSeconds = 30.0;

		// only a real corpse: an essential NPC (a follower) "killed" is only knocked down and stays alive
		bool YieldedDeath(RE::Actor* a_actor)
		{
			if (!a_actor || !ReallyDead(a_actor)) {
				return false;
			}
			const auto now = Now();
			std::scoped_lock lock(g_lock);
			auto it = g_yieldedDeaths.find(a_actor->GetFormID());
			return it != g_yieldedDeaths.end() && now - it->second < kYieldedSeconds;
		}

		// a dead NPC whose death is not Delirium's: one the gate gave away, or a Carnage corpse
		const char* CorpseReason(RE::Actor* a_actor, const char* a_carnageReason)
		{
			if (YieldedDeath(a_actor)) {
				return kYieldedCorpse;
			}
			if (Settings::carnageReleaseCorpses && ReallyDead(a_actor) && IsCarnageActor(a_actor)) {
				return a_carnageReason;
			}
			return nullptr;
		}

		bool FromFile(const RE::TESForm* a_form, const std::vector<const RE::TESFile*>& a_files)
		{
			if (!a_form || a_files.empty()) {
				return false;
			}
			const auto* file = a_form->GetFile(0);
			return file && std::find(a_files.begin(), a_files.end(), file) != a_files.end();
		}

		// "Plugin.esp|0x123" -> form
		RE::TESForm* ParsePluginForm(const std::string& a_text)
		{
			auto bar = a_text.find('|');
			if (bar == std::string::npos) {
				return nullptr;
			}
			std::string plugin = a_text.substr(0, bar);
			std::string id = a_text.substr(bar + 1);
			try {
				auto local = static_cast<RE::FormID>(std::stoul(id, nullptr, 16));
				auto* dh = RE::TESDataHandler::GetSingleton();
				return dh ? dh->LookupForm(local, plugin) : nullptr;
			} catch (...) {
				return nullptr;
			}
		}

		// Checks that read lists the game changes on the main thread (factions, active effects, graph variables).
		const char* MainThreadChecks(RE::Actor* a_actor)
		{
			if (Settings::pairedYield) {
				static const RE::BSFixedString kIsSynced{ "bIsSynced" };
				bool synced = false;
				if (a_actor->GetGraphVariableBool(kIsSynced, synced) && synced) {
					return kPaired;
				}
			}
			for (auto* faction : g_factions) {
				if (a_actor->IsInFaction(faction)) {
					return kFaction;
				}
			}
			if (!g_keywords.empty()) {
				auto* base = a_actor->GetActorBase();
				auto* race = a_actor->GetRace();
				for (auto* kw : g_keywords) {
					if ((base && base->HasKeyword(kw)) || (race && race->HasKeyword(kw))) {
						return kKeyword;
					}
				}
			}
			if (!g_effectKeywords.empty() || !g_effectPlugins.empty()) {
				auto* target = a_actor->AsMagicTarget();
				auto* list = target ? target->GetActiveEffectList() : nullptr;
				if (list) {
					for (auto* effect : *list) {
						if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kInactive, RE::ActiveEffect::Flag::kDispelled)) {
							continue;
						}
						if (FromFile(effect->spell, g_effectPlugins)) {
							return kEffect;
						}
						if (auto* mgef = effect->GetBaseObject()) {
							if (FromFile(mgef, g_effectPlugins)) {
								return kEffect;
							}
							for (auto* kw : g_effectKeywords) {
								if (mgef->HasKeyword(kw)) {
									return kEffect;
								}
							}
						}
					}
				}
			}
			return nullptr;
		}

		// The checks every caller gets, from any thread.
		const char* CommonReason(RE::Actor* a_actor, RE::Actor* a_other)
		{
			if (!a_actor || !g_dataLoaded) {
				return nullptr;
			}
			if (Settings::killMovesYield && (a_actor->IsInKillMove() || (a_other && a_other->IsInKillMove()))) {
				return kKillMove;
			}
			if (Settings::carnageYieldAll && IsCarnageActor(a_actor)) {
				return kCarnage;
			}

			const auto id = a_actor->GetFormID();
			const auto now = Now();
			{
				std::scoped_lock lock(g_lock);
				if (auto it = g_blockedUntil.find(id); it != g_blockedUntil.end() && it->second > now) {
					return kSource;
				}
			}

			if (IsMainThread()) {
				const char* reason = MainThreadChecks(a_actor);
				std::scoped_lock lock(g_lock);
				g_mainChecks[id] = { reason, now };
				return reason;
			}

			// Off the main thread: use what the main thread found last (hit sink, watch, earlier checks).
			std::scoped_lock lock(g_lock);
			if (auto it = g_mainChecks.find(id); it != g_mainChecks.end() && now - it->second.time < kCacheSeconds) {
				return it->second.reason;
			}
			return nullptr;
		}

		bool CarnageSpell(const RE::SpellItem* a_spell)
		{
			return a_spell && g_carnageFile && a_spell->GetFile(0) == g_carnageFile;
		}

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			static HitSink* GetSingleton()
			{
				static HitSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!a_event || !a_event->target) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* actor = a_event->target->As<RE::Actor>();
				if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
					return RE::BSEventNotifyControl::kContinue;
				}

				bool blocked = false;
				if (!g_sourcePlugins.empty()) {
					blocked = FromFile(RE::TESForm::LookupByID(a_event->source), g_sourcePlugins) ||
					          FromFile(RE::TESForm::LookupByID(a_event->projectile), g_sourcePlugins);
				}

				const auto id = actor->GetFormID();
				const auto now = Now();
				const char* reason = g_dataLoaded ? MainThreadChecks(actor) : nullptr;  // hit events arrive on the main thread
				{
					std::scoped_lock lock(g_lock);
					if (blocked) {
						g_blockedUntil[id] = now + Settings::sourceBlockSeconds;
					} else {
						g_lastRealHit[id] = now;
					}
					g_mainChecks[id] = { reason, now };

					// keep the maps small
					if (g_lastRealHit.size() > 2048) {
						std::erase_if(g_lastRealHit, [now](const auto& kv) { return now - kv.second > 30.0; });
						std::erase_if(g_blockedUntil, [now](const auto& kv) { return kv.second < now; });
						std::erase_if(g_mainChecks, [now](const auto& kv) { return now - kv.second.time > 30.0; });
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void SetMainThread()
	{
		g_mainThread = std::this_thread::get_id();
	}

	bool IsMainThread()
	{
		return std::this_thread::get_id() == g_mainThread;
	}

	void OnDataLoaded()
	{
		auto* dh = RE::TESDataHandler::GetSingleton();

		g_factions.clear();
		for (const auto& text : Settings::factions) {
			auto* form = ParsePluginForm(text);
			auto* faction = form ? form->As<RE::TESFaction>() : nullptr;
			if (faction) {
				g_factions.push_back(faction);
				logger::info("Other mods: faction {} found ({:08X})", text, faction->GetFormID());
			} else {
				logger::info("Other mods: faction {} not found (plugin not loaded?), skipped", text);
			}
		}

		auto keywordList = [](const std::vector<std::string>& a_ids, std::vector<RE::BGSKeyword*>& a_out, const char* a_what) {
			a_out.clear();
			for (const auto& edid : a_ids) {
				if (auto* kw = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(edid)) {
					a_out.push_back(kw);
					logger::info("Other mods: {} {} found ({:08X})", a_what, edid, kw->GetFormID());
				} else {
					logger::info("Other mods: {} {} not found, skipped", a_what, edid);
				}
			}
		};
		keywordList(Settings::keywords, g_keywords, "keyword");
		keywordList(Settings::effectKeywords, g_effectKeywords, "effect keyword");

		auto pluginList = [dh](const std::vector<std::string>& a_names, std::vector<const RE::TESFile*>& a_out, const char* a_what) {
			a_out.clear();
			for (const auto& name : a_names) {
				const auto* file = dh ? dh->LookupModByName(name) : nullptr;
				if (file) {
					a_out.push_back(file);
					logger::info("Other mods: {} plugin {} found", a_what, name);
				} else {
					logger::info("Other mods: {} plugin {} not loaded, skipped", a_what, name);
				}
			}
		};
		pluginList(Settings::effectPlugins, g_effectPlugins, "effect");
		pluginList(Settings::sourcePlugins, g_sourcePlugins, "hit source");

		g_carnageFile = dh ? dh->LookupModByName(Settings::carnagePlugin) : nullptr;
		if (g_carnageFile) {
			logger::info("Maximum Carnage: {} found; NPCs with its gore spell are Carnage's (deaths {}, corpses {}, everything {})",
				Settings::carnagePlugin,
				Settings::carnageYieldDeaths ? "yield" : "not",
				Settings::carnageReleaseCorpses ? "released" : "kept",
				Settings::carnageYieldAll ? "yield" : "not");
		} else {
			logger::info("Maximum Carnage: {} not loaded, Carnage rules off", Settings::carnagePlugin);
		}

		{
			std::scoped_lock lock(g_lock);
			g_carnageBase.clear();
			g_mainChecks.clear();
		}
		g_dataLoaded = true;
	}

	void RegisterHitSink()
	{
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESHitEvent>(HitSink::GetSingleton());
			logger::info("Hit event sink registered");
		} else {
			logger::error("No ScriptEventSourceHolder: hit tracking off (ragdoll rule will treat every ragdoll as not vanilla)");
		}
	}

	bool IsCarnageActor(RE::Actor* a_actor)
	{
		if (!g_carnageFile || !a_actor) {
			return false;
		}
		auto* base = a_actor->GetActorBase();
		if (base) {
			const auto baseId = base->GetFormID();
			{
				std::scoped_lock lock(g_lock);
				if (auto it = g_carnageBase.find(baseId); it != g_carnageBase.end() && it->second) {
					return true;
				}
			}
			bool found = false;
			if (auto* spells = base->actorEffects) {  // SPID puts the MC_Gore spells here
				for (std::uint32_t i = 0; spells->spells && i < spells->numSpells; ++i) {
					if (CarnageSpell(spells->spells[i])) {
						found = true;
						break;
					}
				}
			}
			if (found) {
				std::scoped_lock lock(g_lock);
				g_carnageBase[baseId] = true;
				return true;
			}
		}
		// spells added at run time (AddSpell): main thread only, the game changes that list there
		if (IsMainThread()) {
			for (auto* spell : a_actor->GetActorRuntimeData().addedSpells) {
				if (CarnageSpell(spell)) {
					return true;
				}
			}
		}
		return false;
	}

	const char* StartReason(RE::Actor* a_actor)
	{
		if (auto* reason = CommonReason(a_actor, nullptr)) {
			return reason;
		}
		return CorpseReason(a_actor, kCarnageCorpse);
	}

	void NoteYieldedDeath(RE::Actor* a_victim)
	{
		if (!a_victim) {
			return;
		}
		const auto now = Now();
		std::scoped_lock lock(g_lock);
		g_yieldedDeaths[a_victim->GetFormID()] = now;
		if (g_yieldedDeaths.size() > 256) {
			std::erase_if(g_yieldedDeaths, [now](const auto& kv) { return now - kv.second > kYieldedSeconds; });
		}
	}

	const char* RagdollReason(RE::Actor* a_actor)
	{
		if (auto* reason = CommonReason(a_actor, nullptr)) {
			return reason;
		}
		if (YieldedDeath(a_actor)) {
			return kCarnageCorpseRagdoll;  // not logged: a corpse asks every frame
		}
		if (ReallyDead(a_actor)) {
			// a corpse's ragdoll is the game's own: Delirium decides, except on a Carnage corpse
			return (Settings::carnageReleaseCorpses && IsCarnageActor(a_actor)) ? kCarnageCorpseRagdoll : nullptr;
		}
		if (Settings::ragdollVanillaOnly && a_actor) {
			const auto now = Now();
			std::scoped_lock lock(g_lock);
			auto it = g_lastRealHit.find(a_actor->GetFormID());
			if (it == g_lastRealHit.end() || now - it->second > Settings::ragdollHitWindow) {
				return kNotVanilla;
			}
		}
		return nullptr;
	}

	const char* DeathReason(RE::Actor* a_victim, RE::Actor* a_killer)
	{
		if (auto* reason = CommonReason(a_victim, a_killer)) {
			return reason;
		}
		if (Settings::carnageYieldDeaths && IsCarnageActor(a_victim)) {
			return kCarnageDeath;
		}
		if (Settings::yieldPlayerKills && a_killer && a_killer == RE::PlayerCharacter::GetSingleton()) {
			return kPlayerKill;
		}
		return nullptr;
	}

	const char* HeldReason(RE::Actor* a_actor)
	{
		if (auto* reason = CommonReason(a_actor, nullptr)) {
			return reason;
		}
		if (ReallyDead(a_actor)) {
			return CorpseReason(a_actor, kCarnageCorpse);
		}
		return nullptr;
	}

	void LogYield(RE::Actor* a_actor, const char* a_where, const char* a_reason)
	{
		if (!Settings::logYields || !a_actor || !a_reason || a_reason == kCarnageCorpseRagdoll) {
			return;
		}
		// a ragdoll asks every frame for as long as she lies there: log that one at most every 30 s
		const bool   ragdoll = std::string_view(a_where) == "ragdoll takeover";
		const double repeat = ragdoll ? std::max(30.0, static_cast<double>(Settings::logRepeatSeconds)) : Settings::logRepeatSeconds;
		const auto id = a_actor->GetFormID();
		const auto key = (static_cast<std::uint64_t>(id) << 32) ^ static_cast<std::uint64_t>(std::hash<std::string_view>{}(std::string_view(a_where)) ^ reinterpret_cast<std::uintptr_t>(a_reason));
		const auto now = Now();
		{
			std::scoped_lock lock(g_lock);
			auto& last = g_lastLog[key];
			if (last != 0.0 && now - last < repeat) {
				return;
			}
			last = now;
			if (g_lastLog.size() > 4096) {
				std::erase_if(g_lastLog, [now](const auto& kv) { return now - kv.second > 60.0; });
			}
		}
		const char* name = a_actor->GetName();
		logger::info("{:08X} {}: {} -> {}", id, name ? name : "", a_where, a_reason + 14);  // skip the "DeliriumGate: " prefix
	}
}
