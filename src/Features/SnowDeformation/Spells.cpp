#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/Game.h"

// How far along a stream to look for the ground it lands on. A held flame is
// a cone from the hand, and what melts is where that cone LANDS - the
// projectile's own altitude says nothing about whether the fire reaches snow.
static constexpr float kSpellStreamReach = 700.0f;
static constexpr int kSpellTraceSteps = 12;
// Footprint where a stream meets the ground.
static constexpr float kSpellContactRadius = 70.0f;
// Radiant heat: a flame passing ABOVE snow without landing on it still warms
// what goes under. Fades out over this height, and spreads as it rises.
static constexpr float kSpellRadiantBand = 300.0f;
static constexpr float kSpellRadiantRadiusBase = 55.0f;
static constexpr float kSpellRadiantRadiusPerUnit = 0.45f;
// Radiant heat never melts as hard as direct contact.
static constexpr float kSpellRadiantScale = 0.55f;
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
static constexpr float kMinSpellStrength = 0.04f;
// A velocity shorter than this carries no usable direction, so the aim comes
// off the caster instead.
static constexpr float kMinSpellSpeed = 1.0f;

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

// Where a ray from a_origin along a_direction first passes below the land.
// Marched rather than raycast: the land height is the surface the shell is
// built on, so agreeing with it matters more than agreeing with collision.
static bool TraceGroundContact(RE::TES* a_tes, const RE::NiPoint3& a_origin,
	const RE::NiPoint3& a_direction, RE::NiPoint3& a_contact)
{
	auto gapAt = [&](const RE::NiPoint3& a_point) {
		float landZ = a_point.z;
		a_tes->GetLandHeight(a_point, landZ);
		return a_point.z - landZ;
	};

	RE::NiPoint3 previous = a_origin;
	float previousGap = gapAt(a_origin);
	if (previousGap <= 0.0f) {
		a_contact = a_origin;
		return true;
	}

	for (int step = 1; step <= kSpellTraceSteps; step++) {
		const RE::NiPoint3 sample =
			a_origin + a_direction * (kSpellStreamReach * static_cast<float>(step) / kSpellTraceSteps);
		const float gap = gapAt(sample);
		if (gap <= 0.0f) {
			// Linear refine across the straddling pair; a marched crossing is
			// already within a step of the truth and the bowl is 70 wide.
			const float t = std::clamp(previousGap / std::max(previousGap - gap, 1e-4f), 0.0f, 1.0f);
			a_contact = previous + (sample - previous) * t;
			return true;
		}
		previous = sample;
		previousGap = gap;
	}
	return false;
}

// Aim direction of whoever is casting, for streams whose projectile carries no
// usable velocity.
static bool ShooterAim(const RE::ObjectRefHandle& a_shooter, RE::NiPoint3& a_direction)
{
	auto shooter = a_shooter.get();
	if (!shooter)
		return false;
	const float pitch = shooter->data.angle.x;
	const float yaw = shooter->data.angle.z;
	const float cosPitch = std::cos(pitch);
	a_direction = { std::sin(yaw) * cosPitch, std::cos(yaw) * cosPitch, -std::sin(pitch) };
	return true;
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
	// walks the land height a dozen times per stream, and none of that belongs
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

		float magnitudeScale = 1.0f;
		if (costliest)
			magnitudeScale = std::clamp(costliest->effectItem.magnitude / kSpellReferenceMagnitude,
				kSpellMagnitudeMin, kSpellMagnitudeMax);
		const float rate = std::max(settings.SpellMeltRate, 0.0f) * magnitudeScale;

		// Where the stream lands. A flame held at hand height still melts what
		// it is pointed at, so the mark belongs at the ground contact, not
		// under the projectile.
		RE::NiPoint3 direction = runtime.velocity;
		const float speed = direction.Length();
		if (speed > kMinSpellSpeed)
			direction /= speed;
		else if (!ShooterAim(runtime.shooter, direction))
			direction = { 0.0f, 0.0f, -1.0f };

		RE::NiPoint3 markPosition{};
		float strength = 0.0f;
		float radius = 0.0f;

		RE::NiPoint3 contact{};
		if (TraceGroundContact(tes, position, direction, contact)) {
			spellStats.groundContacts++;
			markPosition = contact;
			strength = 1.0f;
			radius = kSpellContactRadius;
		} else {
			// The stream never meets the ground - played horizontally, or at
			// an actor. Heat still radiates onto whatever passes beneath it,
			// weaker and broader the higher the flame runs.
			float groundZ = position.z;
			tes->GetLandHeight(position, groundZ);
			const float heightAbove = position.z - groundZ;
			if (heightAbove < 0.0f || heightAbove > kSpellRadiantBand)
				continue;
			const float reach = 1.0f - heightAbove / kSpellRadiantBand;
			markPosition = { position.x, position.y, groundZ };
			strength = reach * kSpellRadiantScale;
			radius = kSpellRadiantRadiusBase + heightAbove * kSpellRadiantRadiusPerUnit;
		}

		spellStats.lastStrength = strength;
		spellStats.lastRadius = radius;
		if (strength < kMinSpellStrength)
			continue;

		// Capsule from the mark's previous position: a stream swept across the
		// ground melts a continuous band, not a row of dots.
		const float2 current{ markPosition.x, markPosition.y };
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
		emitter.radius = radius;
		emitter.strength = strength;
		// Strength is NOT folded in here: the shader already scales both the
		// melt target and its approach rate by the stamp's strength.
		emitter.rate = rate;
		emitter.element = SpellElement::Fire;
		emitter.mark = SpellMark::Melt;
		spellEmitters.push_back(emitter);
	}

	spellPrevPositions = std::move(currentPositions);
	spellStats.emitters = static_cast<uint>(spellEmitters.size());
}
