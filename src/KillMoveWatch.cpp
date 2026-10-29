#include "KillMoveWatch.h"
#include "Rules.h"
#include "Settings.h"

namespace KillMoveWatch
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		bool              g_inKillMove = false;
		Clock::time_point g_started{};
		bool              g_checkPending = false;
		Clock::time_point g_ended{};
		int               g_attempts = 0;

		double Seconds(Clock::duration a_d)
		{
			return std::chrono::duration<double>(a_d).count();
		}

		bool Synced(RE::Actor* a_actor)
		{
			static const RE::BSFixedString kIsSynced{ "bIsSynced" };
			bool synced = false;
			return a_actor->GetGraphVariableBool(kIsSynced, synced) && synced;
		}

		bool ControlsOk()
		{
			auto* controls = RE::ControlMap::GetSingleton();
			return !controls || (controls->IsFightingControlsEnabled() && controls->IsMenuControlsEnabled());
		}

		void EnablePlayerControls()
		{
			auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto* script = factory ? factory->Create() : nullptr;
			if (!script) {
				logger::error("Player kill move: could not create a script to run EnablePlayerControls");
				return;
			}
			script->SetCommand("EnablePlayerControls");
			script->CompileAndRun(nullptr);
			delete script;
		}
	}

	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Get3D()) {
			return;
		}
		const auto now = Clock::now();
		const bool inKillMove = player->IsInKillMove();

		if (inKillMove && !g_inKillMove) {
			g_started = now;
			g_checkPending = false;
		}
		if (inKillMove) {
			const auto age = Seconds(now - g_started);
			if (age > Settings::playerKillMoveStuckSeconds && !Synced(player)) {
				player->GetActorRuntimeData().boolFlags.reset(RE::Actor::BOOL_FLAGS::kIsInKillMove);
				logger::info("Player kill move: still flagged after {:.1f} s with no paired animation running: flag cleared", age);
				g_inKillMove = false;
				g_checkPending = true;
				g_ended = now;
				g_attempts = 0;
				return;
			}
		} else if (g_inKillMove) {
			logger::info("Player kill move ended after {:.1f} s", Seconds(now - g_started));
			g_checkPending = true;
			g_ended = now;
			g_attempts = 0;
		}
		g_inKillMove = inKillMove;

		if (!g_checkPending || inKillMove) {
			return;
		}
		const auto since = Seconds(now - g_ended);
		if (since < Settings::playerKillMoveControlsAfter) {
			return;
		}
		if (ControlsOk()) {
			g_checkPending = false;
			return;
		}
		if (const char* reason = Rules::StartReason(player)) {
			logger::info("Player kill move: controls off {:.1f} s after it ended, but {}: left alone", since, reason + 14);
			g_checkPending = false;
			return;
		}
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			return;  // a menu has the game: look again when it closes
		}
		EnablePlayerControls();
		++g_attempts;
		logger::info("Player kill move: fighting / menu controls still off {:.1f} s after it ended: EnablePlayerControls (attempt {}) -> {}",
			since, g_attempts, ControlsOk() ? "controls back" : "still off");
		if (ControlsOk() || g_attempts >= 3) {
			g_checkPending = false;
		} else {
			g_ended = now;  // try again after the same wait
		}
	}
}
