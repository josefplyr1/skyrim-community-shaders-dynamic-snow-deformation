#include "Features/SnowDeformation.h"

#include "CoSave.h"
#include "Globals.h"

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

	// Indoors holds the last exterior intensity rather than reading live: no
	// interior weather snows, so a live reading would melt the exterior while
	// the player sleeps. TES::interiorCell is the test the rest of CS uses; its
	// few-frame lag through a load transition is immaterial at these rates.
	auto* tes = RE::TES::GetSingleton();
	if (!tes || !tes->interiorCell)
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
