// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/CoSave.h"
#include "Globals.h"
#include "State.h"

#include <numbers>

void SnowDeformation::TickAccumulation()
{
	if (weatherClock.reversed)
		return;

	const float elapsed = weatherClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

	auto* tes = globals::game::tes;
	if (!tes || (!tes->interiorCell && !NoSkyWorldspace()))
		accumWeatherIntensity.store(std::clamp(snowfallIntensity, 0.0f, 1.0f), std::memory_order_relaxed);

	const float intensity = accumWeatherIntensity.load(std::memory_order_relaxed);
	const float growth = settings.AccumulationHours > 0.01f ?
	                         intensity / settings.AccumulationHours :
	                         0.0f;
	const float melt = settings.AccumulationMeltHours > 0.01f ?
	                       (1.0f - intensity) / settings.AccumulationMeltHours :
	                       0.0f;
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

	const float peak = std::max(settings.AccumulationPeak, 1.0f);
	return 1.0f + snowAccumulation.load(std::memory_order_relaxed) * (peak - 1.0f);
}

void SnowDeformation::SaveAccumulation(const SKSE::SerializationInterface* a_intfc)
{
	if (!a_intfc->OpenRecord(kAccumRecord, kAccumRecordVersion)) {
		logger::warn("[SNOW DEFORMATION] could not open the accumulation co-save record");
		return;
	}

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

	if (!settings.PersistAccumulation) {
		weatherClockUnarm.store(true, std::memory_order_release);
		gameClockUnarm.store(true, std::memory_order_release);
		logger::info("[SNOW DEFORMATION] accumulation co-save ignored, Remember Snow Accumulation is off");
		return;
	}

	snowAccumulation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
	accumWeatherIntensity.store(std::clamp(weather, 0.0f, 1.0f), std::memory_order_relaxed);
	weatherClockUnarm.store(true, std::memory_order_release);
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
			snowAccumulation.store(0.0f, std::memory_order_relaxed);
			accumWeatherIntensity.store(0.0f, std::memory_order_relaxed);
			weatherClockUnarm.store(true, std::memory_order_release);
			gameClockUnarm.store(true, std::memory_order_release);
		});
}

void SnowDeformation::TickSnowCondition()
{
	if (weatherClock.reversed)
		return;
	const float elapsed = weatherClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

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

	float ClimateWarmth(float a_temperature)
	{
		return std::clamp(a_temperature / 3.0f, 0.0f, 1.0f);
	}

	float ClimateDensity(float a_temperature, float a_freshness, float a_wetness, float a_wind)
	{
		const float fresh = a_temperature <= 0.0f ? 67.9f + 51.3f * std::exp(a_temperature / 2.6f) : 125.0f + 40.0f * std::min(a_temperature, 3.0f);
		return std::clamp(std::lerp(fresh, 290.0f, 1.0f - a_freshness) + 110.0f * a_wetness + 150.0f * a_wind, 50.0f, 560.0f);
	}

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

	float ClimateHardZero(float a_factor)
	{
		return std::clamp((a_factor - 0.1f) / 0.9f, 0.0f, 1.0f);
	}

	constexpr float kSloughMax = 0.5f;

	constexpr float kSnowfallAgeing = 20.0f;
	constexpr float kRainAgeing = 8.0f;
	constexpr float kWindAgeing = 4.0f;
	constexpr float kWarmAgeing = 6.0f;
	constexpr float kFreezeColdness = 3.0f;
	constexpr float kDrainHours = 6.0f;

	struct ClimateLookTerms
	{
		float berm, lip, teeth, slough;
	};

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
	if (weatherClock.reversed) {
		climate.primed = false;
		return;
	}
	const float elapsed = weatherClock.elapsedHours;
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

	const float warm = ClimateWarmth(climate.temperature);
	climate.ageingRate = debugAgeingRateOverride >= 0.0f ? debugAgeingRateOverride :
	                                                       1.0f + kSnowfallAgeing * snowing + kRainAgeing * raining + kWindAgeing * climate.windPacking * (1.0f - snowing) + kWarmAgeing * warm;
}

void SnowDeformation::TickSnowAgeing()
{
	auto& climate = snowClimate;
	if (gameClock.reversed)
		return;
	const float elapsed = gameClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;
	climate.ageingClock += climate.ageingRate * elapsed;
	climate.ageingTotal += (double)climate.ageingRate * elapsed;
	while (climate.ageingClock >= 2.0f * kAgeingClockStart) {
		climate.ageingClock -= kAgeingClockStart;
		climate.ageingRebase += kAgeingClockStart;
	}
	TickSnowIce(elapsed, std::clamp(accumWeatherIntensity.load(std::memory_order_relaxed), 0.0f, 1.0f));
}

void SnowDeformation::TickSnowIce(float a_elapsed, float a_snowing)
{
	auto& climate = snowClimate;
	const float playerT = debugClimateTemperatureOn ? debugClimateTemperature : climate.temperature;
	const float bury = std::exp(-a_elapsed * kSnowfallAgeing * a_snowing / kTrackAgeSpan);
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
		if (!overridden)
			for (auto& entry : climate.iceHistory)
				entry[i] *= unthawed * bury;
	}

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

	const float wetIce = SnowWetIceStrength();
	float mostIce = 0.0f;
	for (uint32_t i = 0; i < kClimateLUTRows; ++i) {
		const SnowCondition row = SnowConditionAt(player.temperature + (float(i) - 1.0f) * kClimateLUTStep);
		look.lut[i] = { wetIce * SnowIceShown(i), wetIce * row.wetness, 0.0f, look.strength * row.wetness };
		mostIce = std::max(mostIce, look.lut[i].x);
	}
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
			weatherClockUnarm.store(true, std::memory_order_release);
			gameClockUnarm.store(true, std::memory_order_release);
			logger::info("[SNOW DEFORMATION] snow freshness restored at {:.3f}", value);
		},
		[this]() {
			snowFreshness.store(0.0f, std::memory_order_relaxed);
			weatherClockUnarm.store(true, std::memory_order_release);
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

namespace
{
	const char* InteriorColdBy(const RE::TESObjectCELL* a_cell, const RE::BGSLocation* a_location)
	{
		static constexpr const char* kColdTypes[] = { "LocTypeDungeon", "LocTypeClearable", "LocTypeAnimalDen", "LocTypeBanditCamp", "LocTypeDragonLair",
			"LocTypeDragonPriestLair", "LocTypeDraugrCrypt", "LocTypeDwarvenAutomatons", "LocTypeFalmerHive", "LocTypeForswornCamp", "LocTypeGiantCamp",
			"LocTypeHagravenNest", "LocTypeSprigganGrove", "LocTypeVampireLair", "LocTypeWarlockLair", "LocTypeWerewolfLair", "LocTypeMine", "LocTypeJail",
			"LocTypeMilitaryFort", "LocTypeCemetery", "LocTypeShip", "LocTypeShipwreck" };
		static const RE::TESObjectCELL* cell = nullptr;
		static const RE::BGSLocation* location = nullptr;
		static const char* cold = nullptr;
		static bool known = false;
		if (known && a_cell == cell && a_location == location)
			return cold;
		static const auto survival = [] {
			std::array<const RE::BGSListForm*, 2> lists{};
			if (auto* data = RE::TESDataHandler::GetSingleton()) {
				lists[0] = data->LookupForm<RE::BGSListForm>(0x92A, "ccQDRSSE001-SurvivalMode.esl");
				lists[1] = data->LookupForm<RE::BGSListForm>(0x92C, "ccQDRSSE001-SurvivalMode.esl");
			}
			return lists;
		}();
		known = true;
		cell = a_cell;
		location = a_location;
		cold = nullptr;
		if (a_location)
			for (const char* type : kColdTypes)
				if (a_location->HasKeywordString(type)) {
					cold = type;
					return cold;
				}
		if ((survival[0] && a_cell && survival[0]->HasForm(a_cell)) || (survival[1] && a_location && survival[1]->HasForm(a_location)))
			cold = "Survival Mode's cold interiors";
		return cold;
	}
}

SnowDeformation::SnowCondition SnowDeformation::SnowConditionAtHeight(float a_z) const
{
	auto* player = globals::game::player;
	const float playerZ = player ? player->GetPosition().z : a_z;
	return SnowConditionAt((debugClimateTemperatureOn ? debugClimateTemperature : snowClimate.temperature) - kClimateLapse * (a_z - playerZ));
}

