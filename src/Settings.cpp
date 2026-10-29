#include "Settings.h"

namespace
{
	constexpr auto kIniPath = "Data/SKSE/Plugins/DeliriumGate.ini";

	std::string Trim(std::string a_s)
	{
		auto notSpace = [](unsigned char c) { return !std::isspace(c); };
		a_s.erase(a_s.begin(), std::find_if(a_s.begin(), a_s.end(), notSpace));
		a_s.erase(std::find_if(a_s.rbegin(), a_s.rend(), notSpace).base(), a_s.end());
		return a_s;
	}

	std::string Lower(std::string a_s)
	{
		for (auto& c : a_s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return a_s;
	}

	using IniMap = std::unordered_map<std::string, std::string>;

	IniMap ReadIni()
	{
		IniMap        map;
		std::ifstream file(kIniPath);
		if (!file) {
			logger::warn("{} not found, using defaults", kIniPath);
			return map;
		}

		std::string section;
		std::string line;
		while (std::getline(file, line)) {
			for (const char marker : { ';', '#' }) {
				if (auto pos = line.find(marker); pos != std::string::npos) {
					line.erase(pos);
				}
			}
			line = Trim(line);
			if (line.empty()) {
				continue;
			}
			if (line.front() == '[' && line.back() == ']') {
				section = Lower(Trim(line.substr(1, line.size() - 2)));
				continue;
			}
			auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			map[section + "." + Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
		}
		return map;
	}

	const std::string* Find(const IniMap& a_map, const char* a_key)
	{
		auto it = a_map.find(a_key);
		return it != a_map.end() ? &it->second : nullptr;
	}

	void Get(const IniMap& a_map, const char* a_key, bool& a_out)
	{
		if (auto* v = Find(a_map, a_key)) {
			auto s = Lower(*v);
			a_out = (s == "1" || s == "true" || s == "yes" || s == "on");
		}
	}

	void Get(const IniMap& a_map, const char* a_key, float& a_out, float a_min, float a_max)
	{
		if (auto* v = Find(a_map, a_key)) {
			try {
				a_out = std::clamp(std::stof(*v), a_min, a_max);
			} catch (...) {
				logger::warn("Bad value for {}: '{}', keeping {}", a_key, *v, a_out);
			}
		}
	}

	void Get(const IniMap& a_map, const char* a_key, std::uint32_t& a_out, std::uint32_t a_min, std::uint32_t a_max)
	{
		if (auto* v = Find(a_map, a_key)) {
			try {
				auto n = std::clamp<long long>(std::stoll(*v), a_min, a_max);
				a_out = static_cast<std::uint32_t>(n);
			} catch (...) {
				logger::warn("Bad value for {}: '{}', keeping {}", a_key, *v, a_out);
			}
		}
	}

	void Get(const IniMap& a_map, const char* a_key, std::string& a_out)
	{
		if (auto* v = Find(a_map, a_key)) {
			a_out = *v;
		}
	}

	// comma separated list; an empty value clears the default
	void Get(const IniMap& a_map, const char* a_key, std::vector<std::string>& a_out)
	{
		auto* v = Find(a_map, a_key);
		if (!v) {
			return;
		}
		a_out.clear();
		std::size_t start = 0;
		while (start <= v->size()) {
			auto comma = v->find(',', start);
			if (comma == std::string::npos) {
				comma = v->size();
			}
			auto item = Trim(v->substr(start, comma - start));
			if (!item.empty()) {
				a_out.push_back(item);
			}
			start = comma + 1;
		}
	}

	std::string Join(const std::vector<std::string>& a_list)
	{
		std::string out;
		for (const auto& s : a_list) {
			if (!out.empty()) {
				out += ", ";
			}
			out += s;
		}
		return out.empty() ? "(none)" : out;
	}
}

namespace Settings
{
	void Load()
	{
		const auto ini = ReadIni();

		Get(ini, "general.benabled", enabled);
		Get(ini, "general.blogyields", logYields);
		Get(ini, "general.flogrepeatseconds", logRepeatSeconds, 0.0f, 600.0f);

		Get(ini, "killmoves.byield", killMovesYield);
		Get(ini, "killmoves.bprotectkillmoves", protectKillMoves);
		Get(ini, "killmoves.brestorekillmoveglobal", restoreKillMoveGlobal);
		Get(ini, "killmoves.byieldplayerkills", yieldPlayerKills);
		Get(ini, "killmoves.bplayerkillmovewatch", playerKillMoveWatch);
		Get(ini, "killmoves.fplayerstuckseconds", playerKillMoveStuckSeconds, 3.0f, 120.0f);
		Get(ini, "killmoves.fplayercontrolsafter", playerKillMoveControlsAfter, 0.5f, 30.0f);

		Get(ini, "pairedanimations.byield", pairedYield);

		Get(ini, "carnage.splugin", carnagePlugin);
		Get(ini, "carnage.byielddeaths", carnageYieldDeaths);
		Get(ini, "carnage.breleasecorpses", carnageReleaseCorpses);
		Get(ini, "carnage.byieldall", carnageYieldAll);

		Get(ini, "dismembering.splugin", dfPlugin);
		Get(ini, "dismembering.byielddeaths", dfYieldDeaths);
		Get(ini, "dismembering.bcheckraces", dfCheckRaces);

		Get(ini, "fec.splugin", fecPlugin);
		Get(ini, "fec.byielddeaths", fecYieldDeaths);
		Get(ini, "fec.bkeepcorpses", fecKeepCorpses);

		Get(ini, "ragdoll.bvanillaonly", ragdollVanillaOnly);
		Get(ini, "ragdoll.fhitwindow", ragdollHitWindow, 0.05f, 10.0f);

		Get(ini, "othermods.sfactions", factions);
		Get(ini, "othermods.skeywords", keywords);
		Get(ini, "othermods.seffectkeywords", effectKeywords);
		Get(ini, "othermods.seffectplugins", effectPlugins);
		Get(ini, "othermods.ssourceplugins", sourcePlugins);
		Get(ini, "othermods.fsourceblockseconds", sourceBlockSeconds, 0.0f, 30.0f);

		Get(ini, "deathcheck.benabled", deathCheckEnabled);
		Get(ini, "deathcheck.ffirstcheck", deathCheckFirst, 0.5f, 60.0f);
		Get(ini, "deathcheck.fsecondcheck", deathCheckSecond, 0.5f, 120.0f);
		if (deathCheckSecond < deathCheckFirst) {
			deathCheckSecond = deathCheckFirst;
		}
		Get(ini, "deathcheck.bfinishstuckdying", finishStuckDying);
		Get(ini, "deathcheck.fstuckdyingseconds", stuckDyingSeconds, 4.0f, 60.0f);

		Get(ini, "release.breleaseheld", releaseHeld);
		Get(ini, "release.iwatchintervalms", watchIntervalMs, 10, 1000);

		logger::info("Settings: bEnabled={} bLogYields={} fLogRepeatSeconds={}", enabled, logYields, logRepeatSeconds);
		logger::info("Settings: [KillMoves] bYield={} bProtectKillMoves={} bRestoreKillMoveGlobal={} bYieldPlayerKills={}", killMovesYield, protectKillMoves, restoreKillMoveGlobal, yieldPlayerKills);
		logger::info("Settings: [KillMoves] bPlayerKillMoveWatch={} fPlayerStuckSeconds={} fPlayerControlsAfter={}", playerKillMoveWatch, playerKillMoveStuckSeconds, playerKillMoveControlsAfter);
		logger::info("Settings: [PairedAnimations] bYield={}", pairedYield);
		logger::info("Settings: [Carnage] sPlugin={} bYieldDeaths={} bReleaseCorpses={} bYieldAll={}", carnagePlugin, carnageYieldDeaths, carnageReleaseCorpses, carnageYieldAll);
		logger::info("Settings: [Dismembering] sPlugin={} bYieldDeaths={} bCheckRaces={}", dfPlugin, dfYieldDeaths, dfCheckRaces);
		logger::info("Settings: [FEC] sPlugin={} bYieldDeaths={} bKeepCorpses={}", fecPlugin, fecYieldDeaths, fecKeepCorpses);
		logger::info("Settings: [Ragdoll] bVanillaOnly={} fHitWindow={}", ragdollVanillaOnly, ragdollHitWindow);
		logger::info("Settings: [OtherMods] sFactions={}", Join(factions));
		logger::info("Settings: [OtherMods] sKeywords={} sEffectKeywords={}", Join(keywords), Join(effectKeywords));
		logger::info("Settings: [OtherMods] sEffectPlugins={} sSourcePlugins={} fSourceBlockSeconds={}", Join(effectPlugins), Join(sourcePlugins), sourceBlockSeconds);
		logger::info("Settings: [DeathCheck] bEnabled={} fFirstCheck={} fSecondCheck={} bFinishStuckDying={} fStuckDyingSeconds={}", deathCheckEnabled, deathCheckFirst, deathCheckSecond, finishStuckDying, stuckDyingSeconds);
		logger::info("Settings: [Release] bReleaseHeld={} iWatchIntervalMs={}", releaseHeld, watchIntervalMs);
	}
}
