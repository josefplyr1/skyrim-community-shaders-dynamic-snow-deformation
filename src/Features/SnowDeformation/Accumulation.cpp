#include "Features/SnowDeformation.h"

#include "CoSave.h"
#include "Globals.h"

// Progressive snow accumulation, Stage B: the scalar and its co-save record.
// Design in ACCUMULATION-PLAN.md; rationale in CODE-NOTES.md.
//
// One global scalar, 0 = the authored per-class depth and 1 = AccumulationPeak
// times it. Stage B only tracks it; nothing reads it yet.

void SnowDeformation::TickAccumulation()
{
	// A loaded save. The co-save has already supplied this timeline's value, so
	// the jump is consumed and nothing integrates across it.
	if (gameClock.reversed)
		return;

	const float elapsed = gameClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

	// Indoors the weather is HELD, not frozen and not read live. There is no
	// snowing weather inside, so a live reading would melt the world outside
	// the door while the player slept through the blizzard that was burying it.
	// Holding the last exterior reading instead means the snow keeps rising
	// while they sleep, and whatever the sky is doing when they step back out
	// takes over from there.
	//
	// TES::interiorCell is the test the rest of CS uses (InteriorSun,
	// VolumetricLighting). UnifiedWater adds a parent-cell fallback because the
	// field lags a few frames through a load transition; at these rates a few
	// frames of held weather is worth nothing, so the bare check is enough here.
	auto* tes = RE::TES::GetSingleton();
	if (!tes || !tes->interiorCell)
		accumWeatherIntensity.store(std::clamp(snowfallIntensity, 0.0f, 1.0f), std::memory_order_relaxed);

	// ONE SIGNED RATE, no threshold on "is it snowing": melt is weighted by
	// (1 - intensity) so a weather cross-fade turns the curve instead of
	// putting a kink in it. ComputeSnowfallIntensity already fades across
	// transitions, and a threshold would throw that away.
	const float intensity = accumWeatherIntensity.load(std::memory_order_relaxed);
	const float growth = settings.AccumulationHours > 0.01f ?
	                         intensity / settings.AccumulationHours :
	                         0.0f;
	const float melt = settings.AccumulationMeltHours > 0.01f ?
	                       (1.0f - intensity) / settings.AccumulationMeltHours :
	                       0.0f;
	// Applied in ANY weather, snowfall included: this is the guarantee that the
	// layer returns to the authored height even through a winter that keeps
	// topping it up. Unlike the trench store's floor it is the same order as
	// the melt, so it shortens a clear-weather settle as well.
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

	// Clamped at 1 from below: the layer only ever ADDS to what the class
	// tables author, so a peak under 1 (a hand-edited JSON) must not turn
	// snowfall into a thaw.
	const float peak = std::max(settings.AccumulationPeak, 1.0f);
	return 1.0f + snowAccumulation.load(std::memory_order_relaxed) * (peak - 1.0f);
}

void SnowDeformation::SaveAccumulation(const SKSE::SerializationInterface* a_intfc)
{
	if (!a_intfc->OpenRecord(kAccumRecord, kAccumRecordVersion)) {
		logger::warn("[SNOW DEFORMATION] could not open the accumulation co-save record");
		return;
	}

	// Zeroed rather than skipped when the toggle is off, so the record's shape
	// never depends on a setting: a save written with it off still loads on a
	// build that reads the record, and lands on the authored depth.
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
	// A future layout read as version 1 is corrupt state, which is worse than
	// none: refuse rather than parse.
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

	// Length-checked, not truthiness-checked: a short read would otherwise
	// leave the scalar built out of whatever the stack held.
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

	// The toggle is read on the way in too: a save written while it was on must
	// not resurrect its layer after the player turns it off.
	if (!settings.PersistAccumulation) {
		gameClockUnarm.store(true, std::memory_order_release);
		logger::info("[SNOW DEFORMATION] accumulation co-save ignored, Remember Snow Accumulation is off");
		return;
	}

	snowAccumulation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
	// Saved inside during a storm: the sky has not cleared just because the
	// save was reloaded.
	accumWeatherIntensity.store(std::clamp(weather, 0.0f, 1.0f), std::memory_order_relaxed);
	// Same reverting-on-load trap the trench store pays for from both sides:
	// left armed, the first tick reads the loaded calendar as a backwards jump.
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
			// Fires before a save is loaded AND on a new game: a fresh game must
			// not inherit the last one's layer.
			snowAccumulation.store(0.0f, std::memory_order_relaxed);
			accumWeatherIntensity.store(0.0f, std::memory_order_relaxed);
			gameClockUnarm.store(true, std::memory_order_release);
		});
}
