#include "DeathCheck.h"
#include "Settings.h"

namespace DeathCheck
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Pending
		{
			RE::FormID       id;
			Clock::time_point due;
			int               stage;  // 1 = first check, 2 = second check
			Clock::time_point died;
		};

		std::mutex           g_lock;
		std::vector<Pending> g_pending;

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
}
