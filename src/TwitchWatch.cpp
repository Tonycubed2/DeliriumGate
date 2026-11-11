#include "TwitchWatch.h"
#include "Core.h"
#include "GameCalls.h"
#include "Settings.h"

#include <Windows.h>

#include <optional>

namespace TwitchWatch
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr float kSampleSeconds = 0.5f;   // bones sampled this often
		constexpr float kRange = 3000.0f;        // corpses this close to the player (about 43 m)
		constexpr float kDeadSeconds = 6.0f;     // a normal ragdoll has settled by then
		constexpr float kMoveUnits = 3.0f;       // summed bone movement per sample that counts as moving
		constexpr int   kMovingSamples = 6;      // 3 s in a row moving: twitching
		constexpr int   kQuietSamples = 6;       // 3 s in a row still: stopped
		constexpr float kRepeatSeconds = 20.0f;  // "still twitching" line

		constexpr std::array<std::string_view, 6> kBones{
			"NPC Pelvis [Pelv]", "NPC Head [Head]", "NPC L Hand [LHnd]",
			"NPC R Hand [RHnd]", "NPC L Foot [Lft ]", "NPC R Foot [Rft ]"
		};

		struct Corpse
		{
			Clock::time_point            deadSince{};
			Clock::time_point            twitchSince{};
			Clock::time_point            lastLine{};
			std::array<RE::NiPoint3, 6>  last{};
			std::array<bool, 6>          have{};
			int                          moving = 0;
			int                          quiet = 0;
			bool                         twitching = false;
			bool                         reported = false;
			std::uint32_t                seenTick = 0;
		};

		std::unordered_map<RE::FormID, Corpse> g_corpses;
		Clock::time_point                      g_lastSample{};
		std::uint32_t                          g_tick = 0;
		std::string                            g_povBlock;
		bool                                   g_povKnown = false;
		int                                    g_weaponState = -1;

		double Seconds(Clock::duration a_d)
		{
			return std::chrono::duration<double>(a_d).count();
		}

		const char* Name(RE::TESObjectREFR* a_ref)
		{
			const char* n = a_ref ? a_ref->GetName() : nullptr;
			return n && *n ? n : "(no name)";
		}

		// "SomeMod.dll+1234" for a code / vtable address, so we can tell whose object it is
		std::string ModuleOf(const void* a_addr)
		{
			if (!a_addr) {
				return "null";
			}
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCWSTR>(a_addr), &module) ||
				!module) {
				return std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_addr));
			}
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(module, path, MAX_PATH);
			const auto file = std::filesystem::path(path).filename().string();
			return std::format("{}+{:X}", file, reinterpret_cast<std::uintptr_t>(a_addr) - reinterpret_cast<std::uintptr_t>(module));
		}

		std::string VtableModule(const void* a_obj)
		{
			if (!a_obj) {
				return "null";
			}
			return ModuleOf(*reinterpret_cast<void* const*>(a_obj));
		}

		bool GraphBool(RE::Actor* a_actor, std::string_view a_name, bool& a_out)
		{
			const RE::BSFixedString name{ a_name };
			return a_actor->GetGraphVariableBool(name, a_out);
		}

		std::string GraphBools(RE::Actor* a_actor)
		{
			std::string out;
			for (const auto name : { "bIsSynced"sv, "bAnimationDriven"sv, "IsAttacking"sv, "bInJumpState"sv, "IsStaggering"sv }) {
				bool v = false;
				if (GraphBool(a_actor, name, v)) {
					out += std::format(" {}={}", name, v ? 1 : 0);
				}
			}
			return out.empty() ? " (no graph)" : out;
		}

		const char* CameraName(std::uint32_t a_id)
		{
			static constexpr std::array<const char*, 13> kNames{
				"first person", "auto vanity", "VATS / kill cam", "free", "iron sights", "furniture",
				"PC transition", "tween menu", "animated", "third person", "mount", "bleedout", "dragon"
			};
			return a_id < kNames.size() ? kNames[a_id] : "?";
		}

		int CameraId()
		{
			auto* camera = RE::PlayerCamera::GetSingleton();
			if (!camera || !camera->currentState) {
				return -1;
			}
			return static_cast<int>(camera->currentState->id);
		}

		// The checks ThirdPersonState's button handler (SkyrimSE.exe 1.6.1170 +8E7FA0) makes before it
		// switches to first person: POV switch control on (ControlMap), PlayerControls data
		// povScriptMode / povBeastMode off, the player's knock state (actorState1 bits 25-27) normal,
		// and not on a mount that blocks it.
		std::string PovBlockers(RE::PlayerCharacter* a_player)
		{
			std::string out;
			auto add = [&](std::string_view a_text) {
				if (!out.empty()) {
					out += ", ";
				}
				out += a_text;
			};
			if (auto* controls = RE::ControlMap::GetSingleton(); controls && !controls->IsPOVSwitchControlsEnabled()) {
				add("POV switch control is off (ControlMap)");
			}
			if (auto* pc = RE::PlayerControls::GetSingleton()) {
				if (pc->data.povScriptMode) {
					add("POV locked by a script (povScriptMode)");
				}
				if (pc->data.povBeastMode) {
					add("POV locked in beast form (povBeastMode)");
				}
			}
			if (const auto knock = a_player->AsActorState()->GetKnockState(); knock != RE::KNOCK_STATE_ENUM::kNormal) {
				add(std::format("player knock state {} (not normal)", static_cast<int>(knock)));
			}
			if (a_player->IsOnMount()) {
				add("player on a mount");
			}
			return out;
		}

		std::string PlayerState(RE::PlayerCharacter* a_player)
		{
			auto*      state = a_player->AsActorState();
			auto*      controls = RE::ControlMap::GetSingleton();
			const int  cam = CameraId();
			const auto blockers = PovBlockers(a_player);
			return std::format(
				"player: camera {} ({}), weapon state {}, knock state {}, kill move {}, ragdoll {}, controls fighting {} POV {} movement {}{}; first person {}",
				cam, cam >= 0 ? CameraName(static_cast<std::uint32_t>(cam)) : "-",
				static_cast<int>(state->GetWeaponState()), static_cast<int>(state->GetKnockState()),
				a_player->IsInKillMove() ? 1 : 0, a_player->IsInRagdollState() ? 1 : 0,
				controls ? controls->IsFightingControlsEnabled() : true, controls ? controls->IsPOVSwitchControlsEnabled() : true,
				controls ? controls->IsMovementControlsEnabled() : true,
				GraphBools(a_player),
				blockers.empty() ? "allowed" : "blocked: " + blockers);
		}

		bool HeldByDelirium(RE::Actor* a_actor)
		{
			const auto handle = a_actor->GetHandle().native_handle();
			for (const auto h : Core::ActiveHandles()) {
				if (h == handle) {
					return true;
				}
			}
			return false;
		}

		void Walk(RE::NiAVObject* a_obj, std::vector<RE::NiAVObject*>& a_out, int a_depth)
		{
			if (!a_obj || a_depth > 64 || a_out.size() > 512) {
				return;
			}
			if (a_obj->collisionObject) {
				a_out.push_back(a_obj);
			}
			if (auto* node = a_obj->AsNode()) {
				for (auto& child : node->GetChildren()) {
					Walk(child.get(), a_out, a_depth + 1);
				}
			}
		}

		float Len3(const RE::hkVector4& a_v)
		{
			alignas(16) float f[4];
			_mm_store_ps(f, a_v.quad);
			return std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		}

		// hkSmallArray's members are private in CommonLibSSE-NG: read its layout directly
		// (T* data at 00, std::uint16_t size at 08; static_assert(sizeof == 0x10) in the header)
		template <class T>
		std::uint16_t SmallSize(const RE::hkSmallArray<T>& a_arr)
		{
			return *reinterpret_cast<const std::uint16_t*>(reinterpret_cast<const std::byte*>(&a_arr) + 0x8);
		}

		template <class T>
		T SmallAt(const RE::hkSmallArray<T>& a_arr, std::uint16_t a_i)
		{
			return (*reinterpret_cast<T* const*>(&a_arr))[a_i];
		}

		// every Havok body in the corpse's 3D, one line each
		void LogBodies(RE::Actor* a_actor)
		{
			std::vector<RE::NiAVObject*> nodes;
			Walk(a_actor->Get3D(), nodes, 0);
			if (nodes.empty()) {
				logger::info("    no Havok bodies in her 3D");
				return;
			}

			RE::bhkWorld* world = nullptr;
			if (auto* cell = a_actor->GetParentCell()) {
				world = cell->GetbhkWorld();
			}
			std::optional<RE::BSReadLockGuard> lock;
			if (world) {
				lock.emplace(world->worldLock);
			}

			int count = 0;
			for (auto* node : nodes) {
				auto* col = node->collisionObject.get();
				auto* nicol = col ? col->AsBhkNiCollisionObject() : nullptr;
				auto* bhk = nicol ? nicol->body.get() : nullptr;
				if (!bhk) {
					continue;
				}
				auto* rigid = skyrim_cast<RE::bhkRigidBody*>(bhk);
				if (!rigid) {
					logger::info("    {}: {} (not a rigid body)", node->name.c_str(), VtableModule(bhk));
					continue;
				}
				auto* body = static_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
				if (!body) {
					logger::info("    {}: bhkRigidBody without a Havok body", node->name.c_str());
					continue;
				}
				++count;
				const auto  filter = body->collidable.broadPhaseHandle.collisionFilterInfo.filter;
				const auto* island = body->simulationIsland;
				const bool  awake = island && island->activeMark;

				std::string actions;
				for (std::uint16_t i = 0; i < SmallSize(body->actions) && i < 8; ++i) {
					actions += " " + VtableModule(SmallAt(body->actions, i));
				}
				std::string listeners;
				for (std::uint16_t i = 0; i < SmallSize(body->contactListeners) && i < 8; ++i) {
					listeners += " " + VtableModule(SmallAt(body->contactListeners, i));
				}
				logger::info(
					"    {}: motion {} filter {:08X} (layer {} group {:04X} bone {} ragdoll bit {}{}) speed {:.2f} m/s spin {:.2f} rad/s {} in world {} constraints {}+{} actions {}{} listeners {}{} bhk {}",
					node->name.c_str(), static_cast<int>(body->motion.type.get()), filter,
					filter & 0x7F, filter >> 16, (filter >> 8) & 0x1F, (filter >> 15) & 1, (filter & (1u << 14)) ? " NO COLLISION" : "",
					Len3(body->motion.linearVelocity), Len3(body->motion.angularVelocity),
					awake ? "awake" : "asleep", body->world ? 1 : 0,
					SmallSize(body->constraintsMaster), body->constraintsSlave.size(),
					SmallSize(body->actions), actions, SmallSize(body->contactListeners), listeners,
					VtableModule(bhk));
			}
			logger::info("    {} rigid bodies", count);
		}

		void Report(RE::Actor* a_actor, Corpse& a_c, RE::PlayerCharacter* a_player, float a_dist, const Clock::time_point& a_now)
		{
			auto* state = a_actor->AsActorState();
			logger::info("Twitch watch: {:08X} {} dead {:.1f} s and still moving for {:.1f} s, {:.0f} units from the player",
				a_actor->GetFormID(), Name(a_actor), Seconds(a_now - a_c.deadSince), Seconds(a_now - a_c.twitchSince), a_dist);
			logger::info("    her: knock state {}, life state {}, kill move {}, ragdoll {}, Delirium has her {}, 3D {}{}",
				static_cast<int>(state->GetKnockState()), static_cast<int>(state->GetLifeState()),
				a_actor->IsInKillMove() ? 1 : 0, a_actor->IsInRagdollState() ? 1 : 0,
				HeldByDelirium(a_actor) ? 1 : 0, a_actor->Is3DLoaded() ? 1 : 0, GraphBools(a_actor));
			logger::info("    {}", PlayerState(a_player));
			LogBodies(a_actor);
		}

		void SampleCorpses(RE::PlayerCharacter* a_player, const Clock::time_point& a_now)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			const auto playerPos = a_player->GetPosition();

			for (auto& handle : lists->highActorHandles) {
				auto ptr = handle.get();
				auto* actor = ptr.get();
				if (!actor || actor == a_player || !GameCalls::IsDead(actor) || !actor->Is3DLoaded()) {
					continue;
				}
				const float dist = actor->GetPosition().GetDistance(playerPos);
				if (dist > kRange) {
					continue;
				}
				auto* root = actor->Get3D();
				if (!root) {
					continue;
				}

				auto [it, isNew] = g_corpses.try_emplace(actor->GetFormID());
				auto& c = it->second;
				if (isNew) {
					c.deadSince = a_now;
				}
				c.seenTick = g_tick;

				float moved = 0.0f;
				int   bones = 0;
				for (std::size_t i = 0; i < kBones.size(); ++i) {
					auto* bone = root->GetObjectByName(RE::BSFixedString{ kBones[i] });
					if (!bone) {
						c.have[i] = false;
						continue;
					}
					const auto p = bone->world.translate;
					if (c.have[i]) {
						moved += p.GetDistance(c.last[i]);
						++bones;
					}
					c.last[i] = p;
					c.have[i] = true;
				}
				if (bones == 0 || Seconds(a_now - c.deadSince) < kDeadSeconds) {
					continue;
				}

				if (moved >= kMoveUnits) {
					c.quiet = 0;
					if (++c.moving == 1 && !c.twitching) {
						c.twitchSince = a_now;
					}
					if (!c.twitching && c.moving >= kMovingSamples) {
						c.twitching = true;
						c.lastLine = a_now;
						if (!c.reported) {
							c.reported = true;
							Report(actor, c, a_player, dist, a_now);
						} else {
							logger::info("Twitch watch: {:08X} {} moving again ({:.1f} units in {} s); {}",
								actor->GetFormID(), Name(actor), moved, kSampleSeconds, PlayerState(a_player));
						}
					} else if (c.twitching && Seconds(a_now - c.lastLine) >= kRepeatSeconds) {
						c.lastLine = a_now;
						logger::info("Twitch watch: {:08X} {} still moving after {:.0f} s ({:.1f} units in {} s, {:.0f} units away); {}",
							actor->GetFormID(), Name(actor), Seconds(a_now - c.twitchSince), moved, kSampleSeconds, dist, PlayerState(a_player));
					}
				} else {
					c.moving = 0;
					if (c.twitching && ++c.quiet >= kQuietSamples) {
						c.twitching = false;
						logger::info("Twitch watch: {:08X} {} stopped after {:.0f} s; {}",
							actor->GetFormID(), Name(actor), Seconds(a_now - c.twitchSince), PlayerState(a_player));
					}
				}
			}

			// forget corpses no longer near / loaded
			for (auto it = g_corpses.begin(); it != g_corpses.end();) {
				if (it->second.seenTick != g_tick) {
					if (it->second.twitching) {
						logger::info("Twitch watch: {:08X} out of range / unloaded while still moving", it->first);
					}
					it = g_corpses.erase(it);
				} else {
					++it;
				}
			}
		}

		bool AnyTwitching()
		{
			for (const auto& [id, c] : g_corpses) {
				if (c.twitching) {
					return true;
				}
			}
			return false;
		}
	}

	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Get3D()) {
			return;
		}
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) {
			return;
		}
		const auto now = Clock::now();

		// first person checks: logged when they change (only while the camera is in third person,
		// the only place the POV key switches to first person)
		if (CameraId() == static_cast<int>(RE::CameraStates::kThirdPerson)) {
			const auto block = PovBlockers(player);
			if (!g_povKnown || block != g_povBlock) {
				if (g_povKnown || !block.empty()) {
					if (block.empty()) {
						logger::info("POV watch: switching to first person allowed again; {}", PlayerState(player));
					} else {
						logger::info("POV watch: switching to first person blocked: {}; {}", block, PlayerState(player));
					}
				}
				g_povBlock = block;
				g_povKnown = true;
			}
		}

		const int weapon = static_cast<int>(player->AsActorState()->GetWeaponState());
		if (weapon != g_weaponState) {
			if (g_weaponState >= 0 && AnyTwitching()) {
				logger::info("Twitch watch: player weapon state {} -> {} while a corpse is twitching; {}", g_weaponState, weapon, PlayerState(player));
			}
			g_weaponState = weapon;
		}

		if (Seconds(now - g_lastSample) < kSampleSeconds) {
			return;
		}
		g_lastSample = now;
		++g_tick;
		SampleCorpses(player, now);
	}
}
