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

	// Interiors freeze it. Weather is an exterior property and the shell is not
	// drawn in here, so days spent indoors must not strip the world outside the
	// door - the same reasoning UpdateActiveWorldspace keeps its last exterior
	// state for. A wait is covered by the same test: the whole waited span
	// arrives on the first unpaused frame, which is indoors if the wait was.
	auto* tes = RE::TES::GetSingleton();
	if (!tes || !tes->GetRuntimeData2().worldSpace)
		return;

	// ONE SIGNED RATE, no threshold on "is it snowing": melt is weighted by
	// (1 - intensity) so a weather cross-fade turns the curve instead of
	// putting a kink in it. ComputeSnowfallIntensity already fades across
	// transitions, and a threshold would throw that away.
	const float intensity = std::clamp(snowfallIntensity, 0.0f, 1.0f);
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

void SnowDeformation::SaveAccumulation(const SKSE::SerializationInterface* a_intfc)
{
	if (!a_intfc->OpenRecord(kAccumRecord, kAccumRecordVersion)) {
		logger::warn("[SNOW DEFORMATION] could not open the accumulation co-save record");
		return;
	}

	const float value = snowAccumulation.load(std::memory_order_relaxed);
	if (!a_intfc->WriteRecordData(&value, sizeof(value))) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save write failed");
		return;
	}
	logger::debug("[SNOW DEFORMATION] accumulation saved at {:.3f}", value);
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
	if (a_length != sizeof(float)) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save is {} bytes, expected {}; dropped",
			a_length, sizeof(float));
		return;
	}

	// Length-checked, not truthiness-checked: a short read would otherwise
	// leave the scalar built out of whatever the stack held.
	float value = 0.0f;
	if (a_intfc->ReadRecordData(&value, sizeof(value)) != sizeof(value)) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save short read; dropped");
		return;
	}
	if (!std::isfinite(value)) {
		logger::warn("[SNOW DEFORMATION] accumulation co-save holds {}; dropped", value);
		return;
	}

	snowAccumulation.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
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
			gameClockUnarm.store(true, std::memory_order_release);
		});
}
