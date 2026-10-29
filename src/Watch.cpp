#include "Watch.h"
#include "Core.h"
#include "DeathCheck.h"
#include "KillMoveWatch.h"
#include "Rules.h"
#include "Settings.h"

namespace Watch
{
	namespace
	{
		std::atomic<bool>          g_started{ false };
		std::atomic<bool>          g_queued{ false };
		std::atomic<std::uint32_t> g_ticks{ 0 };

		void Tick()
		{
			const auto tick = ++g_ticks;

			// about every 2 s: did Delirium reload its core?
			const auto every = std::max<std::uint32_t>(1, 2000 / Settings::watchIntervalMs);
			if (tick % every == 0) {
				Core::Install("reload check");
			}

			if (Settings::enabled && (Settings::deathCheckEnabled || Settings::finishStuckDying)) {
				DeathCheck::Tick();
			}
			if (Settings::enabled && Settings::playerKillMoveWatch) {
				KillMoveWatch::Tick();
			}

			if (!Settings::enabled || !Settings::releaseHeld || !Core::CanRelease()) {
				return;
			}
			for (const auto native : Core::ActiveHandles()) {
				RE::ActorHandle handle{};
				static_assert(sizeof(handle) == sizeof(native));
				std::memcpy(&handle, &native, sizeof(native));
				auto actor = handle.get();
				if (!actor) {
					continue;  // Delirium drops "actor gone" itself
				}
				if (const char* reason = Rules::HeldReason(actor.get())) {
					if (Core::Release(native, reason)) {
						Rules::LogYield(actor.get(), "handed back", reason);
					}
				}
			}
		}

		void Loop()
		{
			while (true) {
				std::this_thread::sleep_for(std::chrono::milliseconds(Settings::watchIntervalMs));
				if (g_queued.exchange(true)) {
					continue;  // the last one has not run yet (game paused in a menu, loading)
				}
				auto* tasks = SKSE::GetTaskInterface();
				if (!tasks) {
					g_queued = false;
					continue;
				}
				tasks->AddTask([]() {
					Tick();
					g_queued = false;
				});
			}
		}
	}

	void Start()
	{
		if (g_started.exchange(true)) {
			return;
		}
		std::thread(Loop).detach();
		logger::info("Watch started (every {} ms, main thread)", Settings::watchIntervalMs);
	}
}
