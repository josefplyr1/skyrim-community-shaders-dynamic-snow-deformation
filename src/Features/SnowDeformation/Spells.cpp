#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/Game.h"

// A source higher than this above the ground beneath it no longer reaches the
// snow. Also the fade distance: heat falls off with the square of it, so a
// stream held against the ground digs and one played at head height barely
// warms what passes below.
static constexpr float kSpellGroundBand = 160.0f;
// Footprint of a stream at ground contact, and how much wider it spreads per
// unit of height. A cone opens as it travels, so a stream crossing the snow
// from waist height marks a broader band than one pressed into it.
static constexpr float kSpellRadiusBase = 55.0f;
static constexpr float kSpellRadiusPerUnit = 0.45f;
// Effect magnitude mapping to the unscaled rate (vanilla Flames is 8/sec), and
// the clamp either side of it. Modded spells run to absurd magnitudes; the
// ceiling stops one of them melting a crater in a frame.
static constexpr float kSpellReferenceMagnitude = 8.0f;
static constexpr float kSpellMagnitudeMin = 0.35f;
static constexpr float kSpellMagnitudeMax = 3.0f;
// Per-frame movement beyond this breaks the capsule. Projectile form IDs are
// recycled, so a reused id must not comb a melt line across the world.
static constexpr float kSpellTrailBreak = 512.0f;
// Below this the emitter is invisible and not worth a stamp slot.
static constexpr float kMinSpellStrength = 0.02f;

SnowDeformation::SpellElement SnowDeformation::ClassifyElement(const RE::EffectSetting* a_effect)
{
	if (!a_effect)
		return SpellElement::None;
	// The resist variable is the one axis every damaging effect carries,
	// vanilla or modded, which is why no spell is ever named here.
	switch (a_effect->data.resistVariable) {
	case RE::ActorValue::kResistFire:
		return SpellElement::Fire;
	case RE::ActorValue::kResistFrost:
		return SpellElement::Frost;
	case RE::ActorValue::kResistShock:
		return SpellElement::Shock;
	default:
		return SpellElement::None;
	}
}

void SnowDeformation::GatherSpellEmitters()
{
	spellEmitters.clear();
	spellStats = {};

	if (!settings.EnableSpellIntegration) {
		spellPrevPositions.clear();
		return;
	}

	auto* manager = RE::Projectile::Manager::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!manager || !tes) {
		spellPrevPositions.clear();
		return;
	}

	// Snapshot under the lock, resolve and classify outside it: the work below
	// touches the land height and the form database, and none of that belongs
	// inside a spin lock the game's own projectile update contends for.
	std::vector<RE::NiPointer<RE::Projectile>> live;
	{
		RE::BSSpinLockGuard lock(manager->projectileLock);
		live.reserve(manager->limited.size() + manager->unlimited.size());
		for (auto& handle : manager->limited)
			if (auto projectile = handle.get())
				live.push_back(projectile);
		for (auto& handle : manager->unlimited)
			if (auto projectile = handle.get())
				live.push_back(projectile);
	}

	const RE::NiPoint3 cameraPosition = Util::GetEyePosition();
	const float cullRadius = 0.5f * deformWorldSize;
	std::unordered_map<uint32_t, float2> currentPositions;

	for (auto& projectile : live) {
		if (spellEmitters.size() >= kMaxSpellEmitters)
			break;
		if (!projectile || !projectile->Is3DLoaded())
			continue;

		auto& runtime = projectile->GetProjectileRuntimeData();

		// The projectile carries its own effect, so element, delivery and
		// casting type all come without walking the spell's effect list.
		const RE::EffectSetting* effect = runtime.avEffect;
		const RE::Effect* costliest = runtime.spell ? runtime.spell->GetCostliestEffectItem() : nullptr;
		if (!effect && costliest)
			effect = costliest->baseEffect;
		if (!effect)
			continue;
		spellStats.projectiles++;

		if (ClassifyElement(effect) != SpellElement::Fire)
			continue;
		// Concentration only for now: a held stream is the case the additive
		// melt path exists for. Aimed one-shots arrive with the explosion
		// detector, which is where their impact actually lives.
		if (effect->data.castingType != RE::MagicSystem::CastingType::kConcentration)
			continue;
		spellStats.fireStreams++;

		const RE::NiPoint3 position = projectile->GetPosition();
		if (cameraPosition.GetSquaredDistance(position) > cullRadius * cullRadius)
			continue;

		// Ground projection. The deformation map is 2D, and a stream aimed at
		// an actor sits at chest height: without this it would melt a hole
		// under the target as though the flame were on the floor. Height above
		// the land drives both a fade and the spread, so the mark below a
		// mid-air hit is weak and broad rather than sharp and deep.
		float groundZ = position.z;
		tes->GetLandHeight(position, groundZ);
		const float heightAbove = position.z - groundZ;
		if (heightAbove > kSpellGroundBand || heightAbove < -kSpellGroundBand)
			continue;

		const float reach = 1.0f - std::clamp(heightAbove, 0.0f, kSpellGroundBand) / kSpellGroundBand;
		const float strength = reach * reach;
		if (strength < kMinSpellStrength)
			continue;

		float magnitudeScale = 1.0f;
		if (costliest)
			magnitudeScale = std::clamp(costliest->effectItem.magnitude / kSpellReferenceMagnitude,
				kSpellMagnitudeMin, kSpellMagnitudeMax);

		// Capsule from the projectile's previous position: a stream swept
		// across the ground marks a continuous band, not a row of dots.
		const float2 current{ position.x, position.y };
		float2 previous = current;
		const uint32_t key = projectile->formID;
		if (auto it = spellPrevPositions.find(key); it != spellPrevPositions.end()) {
			const float dx = current.x - it->second.x;
			const float dy = current.y - it->second.y;
			if (dx * dx + dy * dy < kSpellTrailBreak * kSpellTrailBreak)
				previous = it->second;
		}
		currentPositions[key] = current;

		SpellEmitter emitter{};
		emitter.position = current;
		emitter.previous = previous;
		emitter.radius = kSpellRadiusBase + std::max(heightAbove, 0.0f) * kSpellRadiusPerUnit;
		emitter.strength = strength;
		// Strength is NOT folded in here: the shader already scales both the
		// melt target and its approach rate by the stamp's strength.
		emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * magnitudeScale;
		emitter.element = SpellElement::Fire;
		emitter.mark = SpellMark::Melt;
		spellEmitters.push_back(emitter);
	}

	spellPrevPositions = std::move(currentPositions);
	spellStats.emitters = static_cast<uint>(spellEmitters.size());
}
