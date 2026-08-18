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
// A source within this of the ground is resting on it and marks at full
// strength. Slack enough to cover the snow layer a hazard settles on top of.
static constexpr float kSpellContactBand = 48.0f;
// Radiant heat: a source passing ABOVE snow without landing on it still warms
// what goes under. Fades out over this height, and spreads as it rises.
static constexpr float kSpellRadiantBand = 300.0f;
static constexpr float kSpellRadiantRadiusPerUnit = 0.45f;
// Radiant heat never marks as hard as direct contact.
static constexpr float kSpellRadiantScale = 0.55f;
// Clamp on a hazard's authored radius: a rune's footprint and a wall segment's
// differ by a lot, and a modded one can be anything at all.
static constexpr float kHazardRadiusMin = 40.0f;
static constexpr float kHazardRadiusMax = 260.0f;
// Same for a blast. The ceiling matters more here: a modded explosion with an
// absurd radius would otherwise melt half the deformation window at once.
static constexpr float kExplosionRadiusMin = 50.0f;
static constexpr float kExplosionRadiusMax = 320.0f;
// A detonation is over before the next frame, so its mark arrives whole rather
// than growing into place. Any rate past a few hundred reaches the target
// within one frame; the value is deliberately far past that.
static constexpr float kExplosionRate = 1000.0f;
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

SnowDeformation::SpellMark SnowDeformation::MarkForElement(SpellElement a_element)
{
	switch (a_element) {
	case SpellElement::Frost:
		return SpellMark::Crust;
	case SpellElement::Shock:
		return SpellMark::Pit;
	default:
		return SpellMark::Melt;
	}
}

// Strength and footprint for a source at a_heightAbove over the land it marks.
// Resting on the snow marks it fully; hovering above only warms what passes
// beneath, weaker and broader the higher it runs. Shared deliberately: every
// detector that marks the ground from an airborne source needs this, and the
// first one to skip it produced marks so weak they were invisible.
static bool GroundMark(float a_heightAbove, float a_contactRadius, float& a_strength, float& a_radius)
{
	a_heightAbove = std::max(a_heightAbove, 0.0f);
	if (a_heightAbove <= kSpellContactBand) {
		a_strength = 1.0f;
		a_radius = a_contactRadius;
		return true;
	}
	if (a_heightAbove > kSpellRadiantBand)
		return false;
	a_strength = (1.0f - a_heightAbove / kSpellRadiantBand) * kSpellRadiantScale;
	a_radius = a_contactRadius + a_heightAbove * kSpellRadiantRadiusPerUnit;
	return true;
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

// Magnitude drives how fast a mark reaches its basin, never how deep it ends
// up: a hotter source arrives sooner at the same shape.
static float RateScaleOf(const RE::Effect* a_effect)
{
	if (!a_effect)
		return 1.0f;
	return std::clamp(a_effect->effectItem.magnitude / kSpellReferenceMagnitude,
		kSpellMagnitudeMin, kSpellMagnitudeMax);
}

void SnowDeformation::ConsiderHazard(RE::TESObjectREFR* a_ref)
{
	if (!settings.EnableSpellIntegration || spellEmitters.size() >= kMaxSpellEmitters)
		return;
	auto* hazard = a_ref ? a_ref->As<RE::Hazard>() : nullptr;
	if (!hazard)
		return;

	auto& runtime = hazard->GetHazardRuntimeData();
	auto* base = runtime.hazard;
	if (!base)
		return;
	spellStats.hazards++;

	// A placed hazard names no element of its own; the spell it applies does.
	const RE::Effect* costliest = base->data.spell ? base->data.spell->GetCostliestEffectItem() : nullptr;
	const RE::EffectSetting* effect = costliest ? costliest->baseEffect : nullptr;
	const SpellElement element = ClassifyElement(effect);
	if (element == SpellElement::None)
		return;

	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;

	const RE::NiPoint3 position = a_ref->GetPosition();
	float groundZ = position.z;
	tes->GetLandHeight(position, groundZ);

	// The live radius while it burns, falling back to the authored one before
	// the hazard has grown into it.
	const float baseRadius = std::clamp(runtime.radius > 1.0f ? runtime.radius : base->data.radius,
		kHazardRadiusMin, kHazardRadiusMax);

	float strength = 0.0f;
	float radius = 0.0f;
	if (!GroundMark(position.z - groundZ, baseRadius, strength, radius))
		return;
	spellStats.lastStrength = strength;
	spellStats.lastRadius = radius;
	if (strength < kMinSpellStrength)
		return;

	SpellEmitter emitter{};
	emitter.position = { position.x, position.y };
	// A wall segment and a rune both stay put, so there is no sweep to carry.
	emitter.previous = emitter.position;
	emitter.radius = radius;
	emitter.strength = strength;
	emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest);
	emitter.element = element;
	emitter.mark = MarkForElement(element);
	spellEmitters.push_back(emitter);
}

void SnowDeformation::BuildExplosionElements()
{
	explosionElementsBuilt = true;
	auto* handler = RE::TESDataHandler::GetSingleton();
	if (!handler)
		return;
	// Inverted out of the effect records: an explosion record has no element,
	// but every effect that spawns one names both. Walked once, and it covers
	// modded content for free because a mod's effect declares its explosion
	// exactly the same way.
	for (auto* effect : handler->GetFormArray<RE::EffectSetting>()) {
		if (!effect || !effect->data.explosion)
			continue;
		const SpellElement element = ClassifyElement(effect);
		if (element == SpellElement::None)
			continue;
		explosionElements.emplace(effect->data.explosion, element);
	}
	logger::debug("SnowDeformation: mapped {} explosions to elements", explosionElements.size());
}

void SnowDeformation::ConsiderExplosion(RE::TESObjectREFR* a_ref)
{
	if (!settings.EnableSpellIntegration || !a_ref)
		return;

	const uint32_t formID = a_ref->formID;
	explosionsLive.insert(formID);
	spellStats.explosions++;
	// Marked on first sight only. A blast persists for several frames and its
	// runtime radius grows across them, so marking every frame would sink a
	// crater in proportion to how long the animation ran.
	if (explosionsStamped.contains(formID))
		return;

	auto* base = a_ref->GetBaseObject();
	auto* explosion = base ? base->As<RE::BGSExplosion>() : nullptr;
	if (!explosion)
		return;

	if (!explosionElementsBuilt)
		BuildExplosionElements();
	const auto found = explosionElements.find(explosion);
	if (found == explosionElements.end())
		return;
	const SpellElement element = found->second;

	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;

	const RE::NiPoint3 position = a_ref->GetPosition();
	float groundZ = position.z;
	tes->GetLandHeight(position, groundZ);

	// The AUTHORED radius, not the live one: the live value is mid-expansion
	// on the frame we catch it, and the mark wants the blast's final size.
	const float baseRadius = std::clamp(explosion->data.radius, kExplosionRadiusMin, kExplosionRadiusMax);

	float strength = 0.0f;
	float radius = 0.0f;
	// A bolt that detonates against a chest marks weakly and broadly below it,
	// not sharply at the height it went off.
	if (!GroundMark(position.z - groundZ, baseRadius, strength, radius))
		return;
	spellStats.lastStrength = strength;
	spellStats.lastRadius = radius;
	if (strength < kMinSpellStrength)
		return;

	explosionsStamped.insert(formID);
	if (spellEmitters.size() >= kMaxSpellEmitters)
		return;

	SpellEmitter emitter{};
	emitter.position = { position.x, position.y };
	emitter.previous = emitter.position;
	emitter.radius = radius;
	emitter.strength = strength;
	emitter.rate = kExplosionRate;
	emitter.element = element;
	emitter.mark = MarkForElement(element);
	spellEmitters.push_back(emitter);
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

		const SpellElement element = ClassifyElement(effect);
		if (element == SpellElement::None)
			continue;
		// Concentration only: a held stream is the case the additive melt path
		// exists for. Aimed one-shots arrive with the explosion detector,
		// which is where their impact actually lives.
		if (effect->data.castingType != RE::MagicSystem::CastingType::kConcentration)
			continue;
		spellStats.streams++;

		const RE::NiPoint3 position = projectile->GetPosition();
		if (cameraPosition.GetSquaredDistance(position) > cullRadius * cullRadius)
			continue;

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
			// The stream never meets the ground - played flat, or held on an
			// actor. Heat still radiates onto whatever passes beneath it.
			float groundZ = position.z;
			tes->GetLandHeight(position, groundZ);
			if (!GroundMark(position.z - groundZ, kSpellContactRadius, strength, radius))
				continue;
			markPosition = { position.x, position.y, groundZ };
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
		emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest);
		emitter.element = element;
		emitter.mark = MarkForElement(element);
		spellEmitters.push_back(emitter);
	}

	spellPrevPositions = std::move(currentPositions);
	spellStats.emitters = static_cast<uint>(spellEmitters.size());
}
