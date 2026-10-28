// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/CoSave.h"
#include "Globals.h"
#include "State.h"

#include <numbers>

// Progressive snow accumulation: one global scalar, 0 = the authored per-class
// depth, 1 = AccumulationPeak times it. Co-save record 'SNAC'.
// Design in ACCUMULATION-PLAN.md, rationale in CODE-NOTES.md.

void SnowDeformation::TickAccumulation()
{
	// Loaded save: the co-save already supplied this timeline's value, so the
	// calendar jump must not be integrated.
	if (gameClock.reversed)
		return;

	const float elapsed = gameClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

	// Indoors and in a No Sky worldspace the last exterior intensity holds
	// rather than reading live: nothing snows there, so a live reading would
	// melt the exterior while the player sleeps. TES::interiorCell's few-frame
	// lag through a load transition is immaterial at these rates.
	auto* tes = globals::game::tes;
	if (!tes || (!tes->interiorCell && !NoSkyWorldspace()))
		accumWeatherIntensity.store(std::clamp(snowfallIntensity, 0.0f, 1.0f), std::memory_order_relaxed);

	// One signed rate. Melt is weighted by (1 - intensity) rather than gated on
	// a snowing threshold, so a weather cross-fade turns the curve instead of
	// stepping it; ComputeSnowfallIntensity already fades across transitions.
	const float intensity = accumWeatherIntensity.load(std::memory_order_relaxed);
	const float growth = settings.AccumulationHours > 0.01f ?
	                         intensity / settings.AccumulationHours :
	                         0.0f;
	const float melt = settings.AccumulationMeltHours > 0.01f ?
	                       (1.0f - intensity) / settings.AccumulationMeltHours :
	                       0.0f;
	// Applied in all weather including snowfall, so the layer is bounded at the
	// authored height. Same order as the melt, so it also shortens a
	// clear-weather settle.
	const float fade = settings.AccumulationFadeDays > 0.01f ?
	                       1.0f / (settings.AccumulationFadeDays * 24.0f) :
	                       0.0f;

	const float current = snowAccumulation.load(std::memory_order_relaxed);
	snowAccumulation.store(std::clamp(current + elapsed * (growth - melt - fade), 0.0f, 1.0f),
		std::memory_order_relaxed);
}

float SnowDeformation::GetAccumulationDepthScale() const
{
	if (!settings.EnableSnowAccumulation)
		return 1.0f;

	// Peak clamped at 1 from below: accumulation only adds, so a hand-edited
	// peak below 1 must not turn snowfall into a thaw.
	const float peak = std::max(settings.AccumulationPeak, 1.0f);
	return 1.0f + snowAccumulation.load(std::memory_order_relaxed) * (peak - 1.0f);
}

void SnowDeformation::SaveAccumulation(const SKSE::SerializationInterface* a_intfc)
{
	if (!a_intfc->OpenRecord(kAccumRecord, kAccumRecordVersion)) {
		logger::warn("[SNOW DEFORMATION] could not open the accumulation co-save record");
		return;
	}

	// Written zeroed rather than skipped when off, so the record layout does not
	// depend on a setting.
	const float value = settings.PersistAccumulation ? snowAccumulation.load(std::memory_order_relaxed) : 0.0f;
	const float weather = settings.PersistAccumulation ? accumWeatherIntensity.load(std::memory_order_relaxed) : 0.0f;
	if (!a_intfc->WriteRecordData(&value, sizeof(value)) ||
		!a_intfc->WriteRecordData(&weather, sizeof(weather))) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save write failed");
		return;
	}
	logger::debug("[SNOW DEFORMATION] accumulation saved at {:.3f}, held weather {:.2f}", value, weather);
}

void SnowDeformation::LoadAccumulation(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length)
{
	LoadTraceBegin("co-save accumulation");
	LoadTraceScope _loadTrace(this, "CoSave: LoadAccumulation");
	// Refuse an unrecognised version rather than parsing it as this one.
	if (a_version != kAccumRecordVersion) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save is version {}, this build reads {}; dropped",
			a_version, kAccumRecordVersion);
		return;
	}
	if (a_length != 2 * sizeof(float)) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save is {} bytes, expected {}; dropped",
			a_length, 2 * sizeof(float));
		return;
	}

	// Length-checked, not truthiness-checked: a short read would leave stack
	// contents in the scalar.
	float value = 0.0f;
	float weather = 0.0f;
	if (a_intfc->ReadRecordData(&value, sizeof(value)) != sizeof(value) ||
		a_intfc->ReadRecordData(&weather, sizeof(weather)) != sizeof(weather)) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save short read; dropped");
		return;
	}
	if (!std::isfinite(value) || !std::isfinite(weather)) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save holds {} / {}; dropped", value, weather);
		return;
	}

	// Gated on load as well as save: a record written while the toggle was on
	// must not restore after it is turned off.
	if (!settings.PersistAccumulation) {
		gameClockUnarm.store(true, std::memory_order_release);
		logger::info("[SNOW DEFORMATION] accumulation co-save ignored, Remember Snow Accumulation is off");
		return;
	}

	snowAccumulation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
	// Held weather rides the record; an indoor save must not resume as clear sky.
	accumWeatherIntensity.store(std::clamp(weather, 0.0f, 1.0f), std::memory_order_relaxed);
	// Unarm the clock, or the first tick reads the loaded calendar as a
	// backwards jump.
	gameClockUnarm.store(true, std::memory_order_release);
	logger::info("[SNOW DEFORMATION] accumulation restored at {:.3f}", value);
}

void SnowDeformation::RegisterAccumulationCoSave()
{
	CoSave::GetSingleton()->Register(
		kAccumRecord, kAccumRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) { SaveAccumulation(a_intfc); },
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) { LoadAccumulation(a_intfc, a_version, a_length); },
		[this]() {
			// Fires before a load and on a new game; state must not cross timelines.
			snowAccumulation.store(0.0f, std::memory_order_relaxed);
			accumWeatherIntensity.store(0.0f, std::memory_order_relaxed);
			gameClockUnarm.store(true, std::memory_order_release);
		});
}

// ---- Snow condition: freshness ----
// One global scalar beside the accumulated layer: 0 = settled firm snow (the
// authored trench look), 1 = loose new snow. Co-save record 'SNFR'.
// Research in TRENCH-MORPHOLOGY-RESEARCH.md (P4), rationale in CODE-NOTES.md.

void SnowDeformation::TickSnowCondition()
{
	if (gameClock.reversed)
		return;
	const float elapsed = gameClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

	// The accumulator's held weather: exterior intensity, frozen indoors.
	const float intensity = accumWeatherIntensity.load(std::memory_order_relaxed);
	const float freshen = intensity / kFreshenHours;
	const float settle = settings.FreshSnowSettleHours > 0.01f ?
	                         (1.0f - intensity) / settings.FreshSnowSettleHours :
	                         0.0f;
	const float current = snowFreshness.load(std::memory_order_relaxed);
	snowFreshness.store(std::clamp(current + elapsed * (freshen - settle), 0.0f, 1.0f), std::memory_order_relaxed);
}

float SnowDeformation::GetSnowSoftness() const
{
	const float freshness = debugFreshnessOverride >= 0.0f ? debugFreshnessOverride : snowFreshness.load(std::memory_order_relaxed);
	return std::clamp(freshness, 0.0f, 1.0f) * std::clamp(settings.FreshSnowSoftness, 0.0f, 1.0f);
}

// ---- Snow climate (TRACK-VARIATION-RESEARCH.md 3.2) ----
// A temperature proxy from the land's snow share, the weather and the sun;
// wetness, stickiness and firmness from it, freshness and wind packing; and
// the ageing clock. The condition formulas are mirrored in SnowShell.hlsl
// (SNOW_CONDITION_VIEW); the look they drive (3.3) is ComputeSnowClimateLook.

namespace
{
	float ClimateSmoothstep(float a_edge0, float a_edge1, float a_x)
	{
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	float ClimateWetRamp(float a_temperature)
	{
		return ClimateSmoothstep(-1.5f, 1.5f, a_temperature);
	}

	// How warm the air is for the snow, 0 at freezing to 1 at +3 C: ages tracks, melts a coat.
	float ClimateWarmth(float a_temperature)
	{
		return std::clamp(a_temperature / 3.0f, 0.0f, 1.0f);
	}

	// New snow is lighter the colder it falls (Hedstrom and Pomeroy 1998);
	// it settles toward 290 kg/m3, wetness and wind pack it further.
	float ClimateDensity(float a_temperature, float a_freshness, float a_wetness, float a_wind)
	{
		const float fresh = a_temperature <= 0.0f ? 67.9f + 51.3f * std::exp(a_temperature / 2.6f) : 125.0f + 40.0f * std::min(a_temperature, 3.0f);
		return std::clamp(std::lerp(fresh, 290.0f, 1.0f - a_freshness) + 110.0f * a_wetness + 150.0f * a_wind, 50.0f, 560.0f);
	}

	// Sintering is negligible below about -20 C and steepest near -2 C; moist
	// snow sticks, slush does not; wind-packed snow holds together.
	float ClimateStickiness(float a_temperature, float a_freshness, float a_wetness, float a_wind)
	{
		return std::clamp(0.1f + 0.62f * ClimateSmoothstep(-20.0f, -1.5f, a_temperature) * (0.3f + 0.7f * (1.0f - a_freshness)) +
							  0.45f * a_wetness * (1.0f - ClimateSmoothstep(0.75f, 1.0f, a_wetness)) + 0.25f * a_wind,
			0.0f, 1.0f);
	}

	float ClimateFirmness(float a_density)
	{
		return ClimateSmoothstep(150.0f, 460.0f, a_density);
	}

	// Under a tenth of its slider a term is off, so its branch skips cleanly.
	float ClimateHardZero(float a_factor)
	{
		return std::clamp((a_factor - 0.1f) / 0.9f, 0.0f, 1.0f);
	}

	// Loose snow falls back into the print; the rest of the wall stands.
	constexpr float kSloughMax = 0.5f;

	// Ageing-clock units a game hour of full snowfall adds.
	constexpr float kSnowfallAgeing = 20.0f;
	// Held water freezes at a rate of one per hour from this many degrees of frost.
	constexpr float kFreezeColdness = 3.0f;
	// Above freezing, water the snow no longer holds drains over this many hours.
	constexpr float kDrainHours = 6.0f;

	struct ClimateLookTerms
	{
		float berm, lip, teeth, slough;
	};

	// Every term is 1 in settled sticky dry snow (stickiness 0.7 and up),
	// where the look is the sliders' own. Wet snow is soft whatever its
	// density, so firmness counts dry.
	ClimateLookTerms ClimateLookOf(float a_stickiness, float a_firmness, float a_wetness)
	{
		ClimateLookTerms look;
		const float sticky = ClimateSmoothstep(0.30f, 0.70f, a_stickiness);
		const float dryFirm = a_firmness * (1.0f - a_wetness);
		look.berm = std::max(ClimateSmoothstep(0.25f, 0.65f, a_stickiness), 0.8f * ClimateSmoothstep(0.30f, 0.80f, a_wetness));
		look.lip = ClimateSmoothstep(0.35f, 0.65f, a_stickiness) * (1.0f - 0.6f * ClimateSmoothstep(0.70f, 1.0f, dryFirm));
		look.teeth = ClimateSmoothstep(0.45f, 0.70f, std::max(a_stickiness, dryFirm)) * (1.0f - 0.6f * a_wetness);
		look.slough = kSloughMax * (1.0f - std::max(sticky, ClimateSmoothstep(0.50f, 0.85f, dryFirm)));
		return look;
	}

	float WeatherCloudiness(RE::TESWeather* a_weather)
	{
		using Flag = RE::TESWeather::WeatherDataFlag;
		if (!a_weather)
			return 0.5f;
		const auto& flags = a_weather->data.flags;
		if (flags.any(Flag::kRainy) || flags.any(Flag::kSnow))
			return 0.9f;
		if (flags.any(Flag::kCloudy))
			return 0.6f;
		if (flags.any(Flag::kPleasant))
			return 0.1f;
		return 0.5f;
	}
}

void SnowDeformation::TickSnowClimate()
{
	auto& climate = snowClimate;
	if (gameClock.reversed) {
		climate.primed = false;
		return;
	}
	const float elapsed = gameClock.elapsedHours;
	auto* tes = globals::game::tes;
	const bool indoors = !tes || tes->interiorCell || NoSkyWorldspace();
	auto* sky = globals::game::sky;
	auto* player = RE::PlayerCharacter::GetSingleton();

	if (!indoors && player) {
		const uint32_t frame = globals::state->frameCount;
		if (climate.landSnowShare < 0.0f || frame - climate.shareFrame >= 60) {
			climate.shareFrame = frame;
			const auto position = player->GetPosition();
			if (const float share = LandSnowShareAround(position.x, position.y); share >= 0.0f)
				climate.landSnowShare = share;
		}
		climate.rainHeld = ComputePrecipitationIntensity(RE::TESWeather::WeatherDataFlag::kRainy);
	}

	const float snowfall = accumWeatherIntensity.load(std::memory_order_relaxed);
	const float snowing = std::clamp(snowfall, 0.0f, 1.0f);
	const float raining = std::clamp(climate.rainHeld, 0.0f, 1.0f);

	if (!indoors) {
		const float share = debugClimateShareOverride >= 0.0f ? std::min(debugClimateShareOverride, 1.0f) :
		                    climate.landSnowShare >= 0.0f     ? climate.landSnowShare :
		                                                        0.5f;
		float cloud = 0.5f;
		if (sky)
			cloud = std::lerp(WeatherCloudiness(sky->lastWeather), WeatherCloudiness(sky->currentWeather), std::clamp(sky->currentWeatherPct, 0.0f, 1.0f));
		float hour = 12.0f;
		if (auto* calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton())
			hour = calendar->GetHour();
		const float sun = hour > 6.0f && hour < 19.0f ? std::sin(std::numbers::pi_v<float> * (hour - 6.0f) / 13.0f) : 0.0f;
		const float blizzard = std::clamp(snowfall - 1.0f, 0.0f, 1.0f);

		climate.termRegion = std::lerp(4.0f, -12.0f, share);
		climate.termWeather = -snowing * (2.0f + 4.0f * blizzard) + 3.0f * raining;
		climate.termSun = (1.0f - cloud) * (5.0f * sun - 3.0f * (1.0f - sun));
		float target = climate.termRegion + climate.termWeather + climate.termSun;
		if (raining > 0.2f)
			target = std::max(target, 1.0f + raining);
		climate.targetTemperature = target;

		const float wetTarget = std::max(ClimateWetRamp(target), 0.9f * raining);
		const float windTarget = sky ? std::clamp(sky->windSpeed, 0.0f, 1.0f) : 0.0f;
		if (!climate.primed) {
			climate.temperature = target;
			climate.wetness = wetTarget;
			climate.windPacking = windTarget;
			climate.primed = true;
		} else if (elapsed > 0.0f) {
			const float hourEase = 1.0f - std::exp(-elapsed);
			const float windEase = 1.0f - std::exp(-elapsed / 6.0f);
			climate.temperature += (target - climate.temperature) * hourEase;
			climate.wetness += (wetTarget - climate.wetness) * hourEase;
			climate.windPacking += (windTarget - climate.windPacking) * windEase;
		}
	}

	// Snowfall buries in an hour or two, sun and rain round in hours, calm
	// cold keeps a track crisp for a day or two.
	const float warm = ClimateWarmth(climate.temperature);
	climate.ageingRate = debugAgeingRateOverride >= 0.0f ? debugAgeingRateOverride :
	                                                       1.0f + kSnowfallAgeing * snowing + 8.0f * raining + 4.0f * climate.windPacking * (1.0f - snowing) + 6.0f * warm;
	if (elapsed > 0.0f) {
		climate.ageingClock += climate.ageingRate * elapsed;
		climate.ageingTotal += (double)climate.ageingRate * elapsed;
		// Track dates follow the clock back (TrackRebaseCS).
		while (climate.ageingClock >= 2.0f * kAgeingClockStart) {
			climate.ageingClock -= kAgeingClockStart;
			climate.ageingRebase += kAgeingClockStart;
		}
		TickSnowIce(elapsed, snowing);
	}
}

void SnowDeformation::TickSnowIce(float a_elapsed, float a_snowing)
{
	auto& climate = snowClimate;
	const float playerT = debugClimateTemperatureOn ? debugClimateTemperature : climate.temperature;
	// Snowfall buries the glaze as it buries a track.
	const float bury = std::exp(-a_elapsed * kSnowfallAgeing * a_snowing / kTrackAgeSpan);
	// The override holds its ice still, and the history with it.
	const bool overridden = debugClimateIceOverride >= 0.0f;
	for (uint32_t i = 0; i < kClimateLUTRows; ++i) {
		const float t = playerT + (float(i) - 1.0f) * kClimateLUTStep;
		const float wet = SnowConditionAt(t).wetness;
		float& water = climate.rowWater[i];
		float& ice = climate.rowIce[i];
		water = std::max(water, wet);
		const float excess = water - wet;
		if (t < 0.0f) {
			const float frozen = excess * (1.0f - std::exp(-a_elapsed * std::min(-t / kFreezeColdness, 1.0f)));
			water -= frozen;
			ice += frozen;
		} else {
			water -= excess * (1.0f - std::exp(-a_elapsed / kDrainHours));
		}
		const float unthawed = std::exp(-a_elapsed * wet);
		const float thawed = ice * (1.0f - unthawed);
		ice -= thawed;
		water += thawed;
		ice = std::clamp(ice * bury, 0.0f, 1.0f);
		water = std::clamp(water * bury, 0.0f, 1.0f);
		// Ice frozen before a cut thaws and is buried like the rest.
		if (!overridden)
			for (auto& entry : climate.iceHistory)
				entry[i] *= unthawed * bury;
	}

	// The shown ice when each clock unit ended; the current unit's is now. Units
	// a jump skips take the ice it arrived at. A clock wrap is kAgeingClockStart
	// units, a whole number of histories.
	const int32_t unit = static_cast<int32_t>(std::floor(climate.ageingClock));
	int32_t passed = climate.iceHistoryUnit < 0 ? 0 : unit - climate.iceHistoryUnit;
	if (passed < 0)
		passed += static_cast<int32_t>(kAgeingClockStart);
	passed = std::clamp(passed, 1, static_cast<int32_t>(kIceHistoryBins));
	std::array<float, kClimateLUTRows> shown;
	for (uint32_t i = 0; i < kClimateLUTRows; ++i)
		shown[i] = SnowIceShown(i);
	for (int32_t u = 0; u < passed; ++u)
		climate.iceHistory[static_cast<uint32_t>(unit - u) & (kIceHistoryBins - 1)] = shown;
	climate.iceHistoryUnit = unit;
}

float SnowDeformation::SnowIceShown(uint32_t a_row) const
{
	if (debugClimateIceOverride >= 0.0f)
		return std::min(debugClimateIceOverride, 1.0f);
	return a_row < kClimateLUTRows ? snowClimate.rowIce[a_row] : 0.0f;
}

float SnowDeformation::SnowIceSinceCut(uint32_t a_row, uint32_t a_units) const
{
	const auto& climate = snowClimate;
	if (a_row >= kClimateLUTRows || climate.iceHistoryUnit < 0)
		return 0.0f;
	const int32_t cut = climate.iceHistoryUnit - static_cast<int32_t>(std::min(a_units, kIceHistoryBins - 1));
	return std::max(SnowIceShown(a_row) - climate.iceHistory[static_cast<uint32_t>(cut) & (kIceHistoryBins - 1)][a_row], 0.0f);
}

float SnowDeformation::SnowWetIceStrength() const
{
	return settings.WetSnowAndIce ? std::clamp(settings.SnowClimateStrength, 0.0f, 1.0f) : 0.0f;
}

float4 SnowDeformation::TrackAgeConstants() const
{
	return { snowClimate.ageingClock, std::clamp(settings.TrackAgeing, 0.0f, 2.0f) / kTrackAgeSpan, 0.0f, 0.0f };
}

SnowDeformation::SnowCondition SnowDeformation::GetSnowCondition() const
{
	return SnowConditionAt(debugClimateTemperatureOn ? debugClimateTemperature : snowClimate.temperature);
}

SnowDeformation::SnowCondition SnowDeformation::SnowConditionAt(float a_temperature) const
{
	SnowCondition condition{};
	const float liveTemperature = snowClimate.temperature;
	condition.temperature = a_temperature;
	condition.freshness = std::clamp(debugFreshnessOverride >= 0.0f ? debugFreshnessOverride : snowFreshness.load(std::memory_order_relaxed), 0.0f, 1.0f);
	condition.windPacking = snowClimate.windPacking;
	// An overridden temperature moves the wetness with it, as height does per pixel.
	condition.wetness = debugClimateWetnessOverride >= 0.0f ?
	                        std::min(debugClimateWetnessOverride, 1.0f) :
	                        std::clamp(snowClimate.wetness + ClimateWetRamp(condition.temperature) - ClimateWetRamp(liveTemperature), 0.0f, 1.0f);
	condition.density = ClimateDensity(condition.temperature, condition.freshness, condition.wetness, condition.windPacking);
	condition.stickiness = debugClimateStickinessOverride >= 0.0f ?
	                           std::min(debugClimateStickinessOverride, 1.0f) :
	                           ClimateStickiness(condition.temperature, condition.freshness, condition.wetness, condition.windPacking);
	condition.firmness = debugClimateFirmnessOverride >= 0.0f ? std::min(debugClimateFirmnessOverride, 1.0f) : ClimateFirmness(condition.density);
	return condition;
}

SnowDeformation::SnowClimateLook SnowDeformation::ComputeSnowClimateLook() const
{
	SnowClimateLook look;
	look.strength = std::clamp(settings.SnowClimateStrength, 0.0f, 1.0f);
	const SnowCondition player = GetSnowCondition();
	const ClimateLookTerms terms = ClimateLookOf(player.stickiness, player.firmness, player.wetness);
	look.berm = ClimateHardZero(terms.berm);
	look.lip = ClimateHardZero(terms.lip);
	look.teeth = ClimateHardZero(terms.teeth);
	look.slough = terms.slough;

	// Rows by the pixel's temperature offset; the shader lerps between them.
	const float wetIce = SnowWetIceStrength();
	float mostIce = 0.0f;
	for (uint32_t i = 0; i < kClimateLUTRows; ++i) {
		const SnowCondition row = SnowConditionAt(player.temperature + (float(i) - 1.0f) * kClimateLUTStep);
		look.lut[i] = { wetIce * SnowIceShown(i), wetIce * row.wetness, 0.0f, look.strength * row.wetness };
		mostIce = std::max(mostIce, look.lut[i].x);
	}
	// z: the shaders' one-read ice gate (SnowIceInView).
	for (auto& entry : look.lut)
		entry.z = mostIce;
	return look;
}

void SnowDeformation::RegisterSnowConditionCoSave()
{
	CoSave::GetSingleton()->Register(
		kFreshRecord, kFreshRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) {
			if (!a_intfc->OpenRecord(kFreshRecord, kFreshRecordVersion)) {
				logger::warn("[SNOW DEFORMATION] could not open the snow freshness co-save record");
				return;
			}
			const float value = snowFreshness.load(std::memory_order_relaxed);
			if (!a_intfc->WriteRecordData(&value, sizeof(value)))
				logger::warn("[SNOW DEFORMATION] snow freshness co-save write failed");
		},
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) {
			if (a_version != kFreshRecordVersion || a_length != sizeof(float)) {
				logger::warn("[SNOW DEFORMATION] snow freshness co-save is version {} / {} bytes; dropped", a_version, a_length);
				return;
			}
			float value = 0.0f;
			if (a_intfc->ReadRecordData(&value, sizeof(value)) != sizeof(value) || !std::isfinite(value)) {
				logger::warn("[SNOW DEFORMATION] snow freshness co-save unreadable; dropped");
				return;
			}
			snowFreshness.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
			gameClockUnarm.store(true, std::memory_order_release);
			logger::info("[SNOW DEFORMATION] snow freshness restored at {:.3f}", value);
		},
		[this]() {
			snowFreshness.store(0.0f, std::memory_order_relaxed);
			gameClockUnarm.store(true, std::memory_order_release);
		});
}

void SnowDeformation::RegisterSnowIceCoSave()
{
	CoSave::GetSingleton()->Register(
		kIceRecord, kIceRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) {
			if (!a_intfc->OpenRecord(kIceRecord, kIceRecordVersion)) {
				logger::warn("[SNOW DEFORMATION] could not open the snow ice co-save record");
				return;
			}
			float values[2 * kClimateLUTRows];
			for (uint32_t i = 0; i < kClimateLUTRows; ++i) {
				values[i] = snowClimate.rowWater[i];
				values[kClimateLUTRows + i] = snowClimate.rowIce[i];
			}
			const int32_t unit = snowClimate.iceHistoryUnit;
			if (!a_intfc->WriteRecordData(values, sizeof(values)) || !a_intfc->WriteRecordData(&unit, sizeof(unit)) ||
				!a_intfc->WriteRecordData(snowClimate.iceHistory.data(), sizeof(snowClimate.iceHistory)))
				logger::warn("[SNOW DEFORMATION] snow ice co-save write failed");
		},
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) {
			float values[2 * kClimateLUTRows];
			decltype(snowClimate.iceHistory) history{};
			const bool withHistory = a_version == kIceRecordVersion && a_length == sizeof(values) + sizeof(int32_t) + sizeof(history);
			if (!(withHistory || (a_version == 1 && a_length == sizeof(values))) || a_intfc->ReadRecordData(values, sizeof(values)) != sizeof(values)) {
				logger::warn("[SNOW DEFORMATION] snow ice co-save is version {} / {} bytes; dropped", a_version, a_length);
				return;
			}
			for (uint32_t i = 0; i < kClimateLUTRows; ++i) {
				snowClimate.rowWater[i] = std::isfinite(values[i]) ? std::clamp(values[i], 0.0f, 1.0f) : 0.0f;
				snowClimate.rowIce[i] = std::isfinite(values[kClimateLUTRows + i]) ? std::clamp(values[kClimateLUTRows + i], 0.0f, 1.0f) : 0.0f;
			}
			// Without a history every track already made reads as cut before the ice.
			int32_t unit = -1;
			if (withHistory && (a_intfc->ReadRecordData(&unit, sizeof(unit)) != sizeof(unit) || a_intfc->ReadRecordData(history.data(), sizeof(history)) != sizeof(history) ||
								   unit < static_cast<int32_t>(kAgeingClockStart) || unit >= static_cast<int32_t>(2.0f * kAgeingClockStart))) {
				unit = -1;
				history = {};
			}
			for (auto& entry : history)
				for (float& ice : entry)
					ice = std::isfinite(ice) ? std::clamp(ice, 0.0f, 1.0f) : 0.0f;
			snowClimate.iceHistory = history;
			snowClimate.iceHistoryUnit = unit;
			logger::info("[SNOW DEFORMATION] snow ice restored: water {:.2f}/{:.2f}/{:.2f}, ice {:.2f}/{:.2f}/{:.2f}, {}", values[0], values[1], values[2],
				values[3], values[4], values[5], unit < 0 ? "no history: tracks already made read as cut before the ice" : "history kept");
		},
		[this]() {
			snowClimate.rowWater = {};
			snowClimate.rowIce = {};
			snowClimate.iceHistory = {};
			snowClimate.iceHistoryUnit = -1;
		});
}

// ---- Snow coat aging inputs (SSPP-SNOW-PLAN.md section 7, stage A0) ----

SnowDeformation::CoatAging SnowDeformation::ComputeCoatAging(RE::Actor* a_actor) const
{
	CoatAging aging;
	if (!a_actor)
		return aging;
	const auto position = a_actor->GetPosition();
	// The melt bowl of SnowExclusions.hlsli without its edge noise: the fire
	// that clears the ground is the one that melts the coat.
	float governing = -1.0f;
	for (const auto& source : heatSources) {
		const float radius = std::max(source.w, 1.0f);
		if (std::abs(position.z - source.z) > radius)
			continue;
		const float dx = position.x - source.x;
		const float dy = position.y - source.y;
		const float distance = std::sqrt(dx * dx + dy * dy);
		const float heat = 1.0f - ClimateSmoothstep(0.35f * radius, radius, distance);
		const float rank = heat > 0.0f ? 1.0f + heat : -distance / radius;
		if (rank > governing || aging.heatDistance < 0.0f) {
			governing = rank;
			aging.heat = heat;
			aging.heatDistance = distance;
			aging.heatRadius = radius;
		}
	}

	auto* tes = globals::game::tes;
	aging.interior = tes && (tes->interiorCell || NoSkyWorldspace());
	aging.interiorSnow = aging.interior && capturedLoadedPrev > 0;
	const SnowCondition condition = GetSnowCondition();
	aging.temperature = condition.temperature;
	aging.warm = ClimateWarmth(condition.temperature);
	aging.wetness = condition.wetness;
	aging.stickiness = condition.stickiness;
	if (!aging.interior) {
		aging.rain = std::clamp(snowClimate.rainHeld, 0.0f, 1.0f);
		aging.snowfall = std::max(accumWeatherIntensity.load(std::memory_order_relaxed), 0.0f);
	}
	float jolt = 0.0f;
	if (auto it = impactTracks.find(a_actor->formID); it != impactTracks.end()) {
		aging.speed = it->second.speed;
		jolt = std::max({ it->second.speedEnergy, it->second.landingEnergy, it->second.knockEnergy });
	}

	const float snowing = std::min(aging.snowfall, 1.0f);
	const float warmIndoors = aging.interior && !aging.interiorSnow ? 1.0f : 0.0f;
	const float outdoors = aging.interior ? 0.0f : std::max(aging.rain, aging.warm) * (1.0f - snowing);
	aging.melt = std::max({ aging.heat, warmIndoors, outdoors });
	aging.shake = (1.0f - aging.stickiness) * jolt;
	aging.growth = aging.interior ? 0.0f : snowing;
	return aging;
}

std::vector<std::string> SnowDeformation::CoatAgingLines() const
{
	std::vector<std::string> lines;
	auto* player = globals::game::player;
	if (!player) {
		lines.push_back("No player.");
		return lines;
	}
	const CoatAging aging = ComputeCoatAging(player);
	lines.push_back("The player, as a coating update pass would read it. Rates are shares of a full rate; stage A1 sets their speed in the coat's own units.");
	if (aging.heatDistance < 0.0f)
		lines.push_back(std::format("Heat 0.00: no heat source within reach ({} gathered)", heatSources.size()));
	else
		lines.push_back(std::format("Heat {:.2f}: governing heat source {:.0f} units away, melt bowl radius {:.0f} ({} gathered)",
			aging.heat, aging.heatDistance, aging.heatRadius, heatSources.size()));
	if (!aging.interior)
		lines.push_back(std::format("Outdoors | rain {:.2f} | snowfall {:.2f}", aging.rain, aging.snowfall));
	else
		lines.push_back(aging.interiorSnow ? "Indoors, snow-projected objects drawn here: a cold place, no warm melt" :
		                                     "Indoors, no snow drawn here: a warm place");
	lines.push_back(std::format("Temperature {:+.1f} C, warm {:.2f}{} | wetness {:.2f} | stickiness {:.2f}", aging.temperature, aging.warm,
		aging.interior ? " (held from outdoors, unused)" : "", aging.wetness, aging.stickiness));
	lines.push_back(std::format("Ground speed {:.0f} units/s", aging.speed));
	lines.push_back(std::format("Melt {:.2f}: the largest of heat, a warm interior, and rain or warmth outdoors times (1 - snowfall)", aging.melt));
	lines.push_back(std::format("Shake-off {:.2f}: (1 - stickiness) x the impact energy", aging.shake));
	lines.push_back(std::format("Growth {:.2f}: snowfall outdoors", aging.growth));
	lines.push_back("Not measured yet: shelter from snowfall. The game's precipitation occlusion map is on the GPU; stage A1 reads it there.");
	return lines;
}

