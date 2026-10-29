#include "DeathCheck.h"
#include "Core.h"
#include "Settings.h"

namespace DeathCheck
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// ---- dead but not in ragdoll ----
		struct Pending
		{
			RE::FormID       id;
			Clock::time_point due;
			int               stage;  // 1 = first check, 2 = second check
			Clock::time_point died;
		};

		std::mutex           g_lock;
		std::vector<Pending> g_pending;

		// ---- deaths Delirium took and has not finished ----
		struct Held
		{
			RE::FormID        victim;
			RE::FormID        killer;  // 0 = none
			Clock::time_point took;
			Clock::time_point due;
			int               stage;  // 0 = ask Delirium to let her go, 1 = kill her ourselves
		};

		std::vector<Held>       g_held;            // under g_lock
		std::atomic<RE::FormID> g_finishing{ 0 };  // the NPC whose death we are finishing right now

		constexpr const char* kStuckDying = "DeliriumGate: dying on her feet ran too long";
		constexpr double      kKillMoveWaitSeconds = 60.0;  // a kill move / paired animation on her: wait at most this long

		double Seconds(Clock::duration a_d)
		{
			return std::chrono::duration<double>(a_d).count();
		}

		Clock::duration After(float a_seconds)
		{
			return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(a_seconds));
		}

		bool ReallyDead(RE::Actor* a_actor)
		{
			auto* state = a_actor->AsActorState();
			if (!state) {
				return false;
			}
			const auto life = state->GetLifeState();
			return life == RE::ACTOR_LIFE_STATE::kDying || life == RE::ACTOR_LIFE_STATE::kDead;
		}

		bool Synced(RE::Actor* a_actor)
		{
			static const RE::BSFixedString kIsSynced{ "bIsSynced" };
			bool synced = false;
			return a_actor->GetGraphVariableBool(kIsSynced, synced) && synced;
		}

		// returns true when a later check is still wanted
		bool Check(const Pending& a_entry)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_entry.id);
			if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
				return false;
			}
			if (!ReallyDead(actor) || actor->IsDisabled() || !actor->Get3D() || !actor->GetParentCell() || !actor->GetParentCell()->IsAttached()) {
				return false;  // revived, gone, unloaded: nothing to fix
			}
			if (actor->IsInRagdollState()) {
				return false;  // she dropped: all good
			}
			const char* name = actor->GetName();
			const auto  age = Seconds(Clock::now() - a_entry.died);
			if (actor->IsInKillMove() || Synced(actor)) {
				logger::info("Death check: {:08X} {} dead {:.1f} s, not in ragdoll, but still in a kill move / paired animation: waiting", a_entry.id, name ? name : "", age);
				return a_entry.stage == 1;
			}
			auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(actor);
			const bool sent = holder->NotifyAnimationGraph("RagdollInstant");
			logger::info("Death check: {:08X} {} dead {:.1f} s but not in ragdoll (stuck in a pose / death loop): RagdollInstant {}",
				a_entry.id, name ? name : "", age, sent ? "sent" : "REFUSED by her behavior graph");
			return a_entry.stage == 1;  // look once more later
		}

		// One death Delirium took: is it finished? If not, finish it. Returns true when it must be looked at again.
		bool CheckHeld(Held& a_entry, Clock::time_point a_now)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_entry.victim);
			if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
				return false;
			}
			if (ReallyDead(actor) || actor->IsDisabled() || !actor->Get3D()) {
				return false;  // died (the normal case), gone, or unloaded (Delirium kills her at the loading screen)
			}
			const char* name = actor->GetName();
			const auto  age = Seconds(a_now - a_entry.took);
			if (actor->IsEssential()) {
				logger::info("Stuck dying: {:08X} {} still up {:.1f} s after the killing blow, but she is essential (cannot die): left alone", a_entry.victim, name ? name : "", age);
				return false;
			}
			if (actor->IsInKillMove() || Synced(actor)) {
				// a kill move or paired animation on her: that one finishes her
				if (age < kKillMoveWaitSeconds) {
					a_entry.due = a_now + After(1.0f);
					return true;
				}
			}

			if (a_entry.stage == 0) {
				a_entry.stage = 1;
				a_entry.due = a_now + After(1.0f);
				// still driven by Delirium: its release finishes the death ("dies now (Delirium let her go)")
				if (Core::CanRelease()) {
					const auto handle = actor->GetHandle().native_handle();
					for (const auto native : Core::ActiveHandles()) {
						if (native == handle) {
							const bool released = Core::Release(native, kStuckDying);
							logger::info("Stuck dying: {:08X} {} still alive {:.1f} s after Delirium took her killing blow: {}",
								a_entry.victim, name ? name : "", age, released ? "Delirium made to let her go (it finishes the death)" : "Delirium's release failed");
							return true;  // look again in 1 s
						}
					}
				}
			}

			// Delirium is not finishing it: the game's own death, credited to the same killer
			auto* killer = a_entry.killer ? RE::TESForm::LookupByID<RE::Actor>(a_entry.killer) : nullptr;
			g_finishing.store(a_entry.victim);
			actor->KillImpl(killer, 0.0f, true, true);
			g_finishing.store(0);
			const bool dead = ReallyDead(actor);
			logger::info("Stuck dying: {:08X} {} still alive {:.1f} s after Delirium took her killing blow: killed by DeliriumGate (killer {:08X}){}",
				a_entry.victim, name ? name : "", age, a_entry.killer, dead ? "" : " - but she is STILL ALIVE");
			return false;
		}

		void TickHeld()
		{
			const auto        now = Clock::now();
			std::vector<Held> due;
			{
				std::scoped_lock lock(g_lock);
				if (g_held.empty()) {
					return;
				}
				for (auto it = g_held.begin(); it != g_held.end();) {
					if (it->due <= now) {
						due.push_back(*it);
						it = g_held.erase(it);
					} else {
						++it;
					}
				}
			}
			std::vector<Held> again;
			for (auto& entry : due) {
				if (CheckHeld(entry, now)) {
					again.push_back(entry);
				}
			}
			if (!again.empty()) {
				std::scoped_lock lock(g_lock);
				g_held.insert(g_held.end(), again.begin(), again.end());
			}
		}

		void TickPending()
		{
			std::vector<Pending> due;
			const auto           now = Clock::now();
			{
				std::scoped_lock lock(g_lock);
				if (g_pending.empty()) {
					return;
				}
				for (auto it = g_pending.begin(); it != g_pending.end();) {
					if (it->due <= now) {
						due.push_back(*it);
						it = g_pending.erase(it);
					} else {
						++it;
					}
				}
			}
			std::vector<Pending> again;
			for (const auto& entry : due) {
				if (Check(entry) && entry.stage == 1) {
					const auto wait = std::max(0.1f, Settings::deathCheckSecond - Settings::deathCheckFirst);
					again.push_back({ entry.id, now + After(wait), 2, entry.died });
				}
			}
			if (!again.empty()) {
				std::scoped_lock lock(g_lock);
				g_pending.insert(g_pending.end(), again.begin(), again.end());
			}
		}

		class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent>
		{
		public:
			static DeathSink* GetSingleton()
			{
				static DeathSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (!a_event || !a_event->dead || !a_event->actorDying || !Settings::enabled || !Settings::deathCheckEnabled) {
					return RE::BSEventNotifyControl::kContinue;
				}
				auto* actor = a_event->actorDying->As<RE::Actor>();
				if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
					return RE::BSEventNotifyControl::kContinue;
				}
				const auto now = Clock::now();
				std::scoped_lock lock(g_lock);
				if (g_pending.size() < 512) {
					g_pending.push_back({ actor->GetFormID(), now + After(Settings::deathCheckFirst), 1, now });
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void RegisterDeathSink()
	{
		if (Settings::enabled && Settings::finishStuckDying) {
			logger::info("Stuck dying on: an NPC Delirium keeps dying on her feet for more than {} s gets her death finished", Settings::stuckDyingSeconds);
		}
		if (!Settings::enabled || !Settings::deathCheckEnabled) {
			logger::info("Death check off ([DeathCheck] bEnabled=0)");
			return;
		}
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESDeathEvent>(DeathSink::GetSingleton());
			logger::info("Death check on: dead NPCs not in ragdoll after {} s and {} s get RagdollInstant", Settings::deathCheckFirst, Settings::deathCheckSecond);
		}
	}

	void Tick()
	{
		if (Settings::deathCheckEnabled) {
			TickPending();
		}
		if (Settings::finishStuckDying) {
			TickHeld();
		}
	}

	void NoteHeldDeath(RE::Actor* a_victim, RE::Actor* a_killer)
	{
		if (!a_victim || !Settings::enabled || !Settings::finishStuckDying) {
			return;
		}
		const auto id = a_victim->GetFormID();
		const auto killer = a_killer ? a_killer->GetFormID() : 0;
		const auto now = Clock::now();
		std::scoped_lock lock(g_lock);
		std::erase_if(g_held, [id](const Held& h) { return h.victim == id; });
		if (g_held.size() < 256) {
			g_held.push_back({ id, killer, now, now + After(Settings::stuckDyingSeconds), 0 });
		}
	}

	bool HeldDeath(RE::Actor* a_actor)
	{
		if (!a_actor) {
			return false;
		}
		const auto id = a_actor->GetFormID();
		std::scoped_lock lock(g_lock);
		return std::any_of(g_held.begin(), g_held.end(), [id](const Held& h) { return h.victim == id; });
	}

	bool Finishing(RE::Actor* a_actor)
	{
		return a_actor && g_finishing.load() == a_actor->GetFormID();
	}
}
