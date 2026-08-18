#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/Game.h"

// How far along a stream to look for the ground it lands on. A held flame is
// a cone from the hand, and what melts is where that cone LANDS - the
// projectile's own altitude says nothing about whether the fire reaches snow.
// Fallback trace length for a projectile whose record names no range. The real
// reach comes from the projectile itself: a fixed number either strands the
// master beams short or hands Sparks a reach it never had.
static constexpr float kSpellStreamReachDefault = 900.0f;
static constexpr float kSpellReachMin = 200.0f;
static constexpr float kSpellReachMax = 4000.0f;
static constexpr int kSpellTraceSteps = 20;
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
// High enough to pass the largest thing Skyrim actually authors. Read off the
// records rather than guessed: FireStormExplosion is 2100 units, where Fireball
// and every rune are 320 and an expert firebolt is 100. A tighter ceiling
// silently flattened the master-tier spells onto the same crater as a bolt,
// which is the opposite of what those 2100 units are saying. Still a ceiling,
// because a modded explosion can name any number at all.
static constexpr float kExplosionRadiusMax = 2400.0f;
// A bolt that carries no explosion at all still strikes the snow. Without this
// the smaller single-target spells mark nothing, while their master-tier
// versions - which do author an explosion - leave craters.
static constexpr float kImpactRadiusDefault = 90.0f;
// A detonation reaches its basin in about a quarter second: fast enough to
// read as a blast rather than a melt, slow enough that the snow visibly gives
// way instead of the crater simply existing on the next frame. The mark has to
// be HELD for that long to get there - see ActiveBlast.
static constexpr float kExplosionRate = 4.0f;
static constexpr float kBlastDuration = 0.35f;
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
// Width of the corridor a projectile cuts through the snow layer. A bolt is
// slim, but the deformation map's texels are about 7 units, so a groove much
// narrower than this aliases away to nothing.
static constexpr float kTrailRadius = 35.0f;
// A bolt crosses the whole snow layer inside a frame or two, so its corridor
// has to arrive at once. Unlike a blast there is nothing to hold open: the
// projectile is already gone.
static constexpr float kTrailRate = 100.0f;
// Snow shallower than this has no column worth cutting through.
static constexpr float kMinTrailDepth = 2.0f;
// A cloak wraps the body, not the boots, so the aura is treated as riding at
// roughly mid-chest. GroundMark then fades it exactly as it fades any other
// source held above the snow, rather than scouring at full strength simply
// because an actor's position sits at their feet.
static constexpr float kCloakCentreHeight = 70.0f;
// Fallback lifetime for a cloak whose effect declares no duration.
static constexpr float kCloakDefaultDuration = 60.0f;
// Authored blast radius that maps to an unscaled pit. Pit sizing deliberately
// ignores the blast radius SCALE, which is tuned for how wide FIRE should
// scar; a discharge answers to its own setting, and the authored size only
// says how much bigger than a bolt it forks. With the blast scale at a third,
// a rune was forking narrower than Sparks, which is backwards.
static constexpr float kPitReferenceRadius = 90.0f;
static constexpr float kPitScaleMin = 0.8f;
static constexpr float kPitScaleMax = 2.2f;
// A held stream pits tighter than a strike: a continuous arc onto one spot
// rather than a discharge dumping into the ground.
static constexpr float kStreamPitScale = 0.65f;
// The corridor a bolt cuts on its way in is thinner still.
static constexpr float kTrailPitScale = 0.5f;
// Where a cloak's arcs land, as a fraction of its reach. How OFTEN they land
// and how big each one is are settings, since taste decides both.
static constexpr float kCloakStrikeInner = 0.45f;
// Travel a projectile could plausibly have made since it was last seen, as a
// slice of a second. A landing further out than this was something else
// stopping the flight, not the ground.
static constexpr float kBlastTravelWindow = 0.05f;
// How near the last sighting a traced landing has to be to count as where the
// bolt actually struck. Further than this and the flight was stopped by
// something else - an actor, a wall - so the ground below only gets the weaker
// radiant mark rather than a full crater directly beneath the burst.
static constexpr float kBlastLandingReach = 220.0f;
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
//
// What the returned strength MEANS depends on how long the source lasts, and
// getting that backwards leaves a mark that can never deepen:
//   INSTANTANEOUS (a blast) - fade the TARGET. It has no time to dig, so a
//     detonation high above the snow leaves a shallow scorch and that is that.
//   SUSTAINED (a cloak, a wall) - fade the RATE and keep the target full. Heat
//     held near snow melts through eventually however far above it sits; being
//     further away makes it slower, not permanently shallower.
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
	const RE::NiPoint3& a_direction, float a_reach, RE::NiPoint3& a_contact)
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
			a_origin + a_direction * (a_reach * static_cast<float>(step) / kSpellTraceSteps);
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

	float heightFade = 0.0f;
	float radius = 0.0f;
	if (!GroundMark(position.z - groundZ, baseRadius, heightFade, radius))
		return;
	if (heightFade < kMinSpellStrength)
		return;
	// A wall burns for seconds, so it is sustained too: height slows it rather
	// than capping how deep it can ever get. Most hazards drop to the ground
	// and fade by nothing at all, but one left on a ledge behaves sensibly.
	const float strength = 1.0f;
	spellStats.lastStrength = strength;
	spellStats.lastRadius = radius;

	SpellEmitter emitter{};
	emitter.position = { position.x, position.y };
	// A wall segment and a rune both stay put, so there is no sweep to carry.
	emitter.previous = emitter.position;
	emitter.radius = radius;
	emitter.strength = strength;
	emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest) * heightFade;
	emitter.element = element;
	emitter.mark = MarkForElement(element);
	emitter.pitScale = std::clamp(baseRadius / kPitReferenceRadius, kPitScaleMin, kPitScaleMax);
	spellEmitters.push_back(emitter);
}

RE::BSEventNotifyControl SnowDeformation::SpellCastSink::ProcessEvent(
	const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*)
{
	auto& feature = globals::features::snowDeformation;
	if (!a_event || !a_event->object || !feature.settings.EnableSpellIntegration)
		return RE::BSEventNotifyControl::kContinue;

	auto* form = RE::TESForm::LookupByID(a_event->spell);
	auto* spell = form ? form->As<RE::MagicItem>() : nullptr;
	if (!spell)
		return RE::BSEventNotifyControl::kContinue;

	// Self-delivered effects ONLY. Anything aimed or placed already has a
	// projectile or a hazard to follow, and marking it here as well would drop
	// a second crater under the caster's own feet every time they threw one.
	SpellElement element = SpellElement::None;
	const RE::BGSExplosion* blast = nullptr;
	SpellElement cloakElement = SpellElement::None;
	float cloakRate = 1.0f;
	float cloakDuration = 0.0f;
	for (auto* item : spell->effects) {
		const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
		if (!base || base->data.delivery != RE::MagicSystem::Delivery::kSelf)
			continue;

		// A cloak declares itself by ARCHETYPE, so it needs no guessing from
		// delivery alone - which would sweep up every standing ability an
		// actor carries. Everything the mark needs is on this record.
		if (base->data.archetype == RE::EffectSetting::Archetype::kCloak) {
			const SpellElement candidate = ClassifyElement(base);
			if (candidate != SpellElement::None) {
				cloakElement = candidate;
				cloakRate = std::clamp(item->effectItem.magnitude / kSpellReferenceMagnitude,
					kSpellMagnitudeMin, kSpellMagnitudeMax);
				cloakDuration = item->effectItem.duration > 0 ?
				                    static_cast<float>(item->effectItem.duration) :
				                    kCloakDefaultDuration;
			}
			continue;
		}

		if (element == SpellElement::None)
			element = ClassifyElement(base);
		// The LARGEST explosion on the spell, not the first. A spell can carry
		// several, and the first need not be the one that defines its reach -
		// Fire Storm authors its 2100 unit blast alongside a companion of
		// radius zero, which would otherwise win and clamp down to nothing.
		if (base->data.explosion &&
			(!blast || base->data.explosion->data.radius > blast->data.radius))
			blast = base->data.explosion;
	}
	if (cloakElement != SpellElement::None) {
		if (auto* actor = a_event->object->As<RE::Actor>()) {
			CloakState cloak{};
			cloak.actor = actor->GetHandle();
			cloak.element = cloakElement;
			cloak.rateScale = cloakRate;
			cloak.remaining = cloakDuration;
			std::scoped_lock lock(feature.queuedCastLock);
			if (feature.queuedCloaks.size() < kMaxSpellEmitters)
				feature.queuedCloaks.push_back(cloak);
		}
	}

	// An explosion is what separates a self-centred BLAST from a self buff.
	if (element == SpellElement::None || !blast)
		return RE::BSEventNotifyControl::kContinue;

	QueuedCast queued{};
	queued.position = a_event->object->GetPosition();
	queued.radius = std::clamp(blast->data.radius, kExplosionRadiusMin, kExplosionRadiusMax);
	queued.element = element;
	{
		std::scoped_lock lock(feature.queuedCastLock);
		if (feature.queuedCasts.size() < kMaxSpellEmitters)
			feature.queuedCasts.push_back(queued);
	}
	return RE::BSEventNotifyControl::kContinue;
}

void SnowDeformation::RegisterSpellCastSink()
{
	spellCastSinkRegistered = true;
	if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
		holder->AddEventSink<RE::TESSpellCastEvent>(&spellCastSink);
		logger::debug("SnowDeformation: spell cast sink registered");
	}
}

void SnowDeformation::ConsiderActorAuras(RE::Actor* a_actor, CloakState& a_cloak, float a_deltaTime)
{
	if (spellEmitters.size() >= kMaxSpellEmitters || !a_actor)
		return;
	auto* tes = RE::TES::GetSingleton();
	if (!tes)
		return;

	const RE::NiPoint3 position = a_actor->GetPosition();
	float groundZ = position.z;
	tes->GetLandHeight(position, groundZ);

	float heightFade = 0.0f;
	float radius = 0.0f;
	// Shock keeps its own reach: arcs jump clear of the body where heat wraps
	// it, so one number for both had them fighting.
	const bool arcs = MarkForElement(a_cloak.element) == SpellMark::Pit;
	const float cloakReach = std::max(arcs ? settings.ShockCloakRadius : settings.CloakRadius, 1.0f);
	if (!GroundMark(position.z + kCloakCentreHeight - groundZ, cloakReach, heightFade, radius))
		return;
	if (heightFade < kMinSpellStrength)
		return;
	spellStats.auras++;

	// SUSTAINED, so the height fade slows the melt rather than capping it. The
	// bowl therefore reaches full depth at its core and takes its shape from
	// the falloff, which is what makes a wider cloak dig deeper as well as
	// further instead of spreading one shallow dish.
	const float strength = 1.0f;

	// Sweeps with the wearer, so a cloaked actor crossing snow leaves a band
	// rather than a row of rings.
	const float2 current{ position.x, position.y };
	float2 previous = current;
	const uint32_t formID = a_actor->formID;
	if (auto it = spellAuraPrev.find(formID); it != spellAuraPrev.end()) {
		const float dx = current.x - it->second.x;
		const float dy = current.y - it->second.y;
		if (dx * dx + dy * dy < kSpellTrailBreak * kSpellTrailBreak)
			previous = it->second;
	}
	currentAuraPositions[formID] = current;

	spellStats.lastStrength = strength;
	spellStats.lastRadius = radius;

	SpellEmitter emitter{};
	emitter.element = a_cloak.element;
	emitter.mark = MarkForElement(a_cloak.element);

	if (emitter.mark == SpellMark::Pit) {
		// A shock cloak does not glow a steady ring; it ARCS, in bursts, to
		// somewhere different each time. Marking a full ring every frame
		// raised one continuous ridge that grew as the wearer walked, which
		// read as ground morphing into hills rather than as lightning
		// striking. So each cloak fires its own small discharges on a timer,
		// scattered around its reach.
		a_cloak.strikeTimer -= a_deltaTime;
		if (a_cloak.strikeTimer > 0.0f)
			return;
		a_cloak.strikeTimer = std::max(settings.ShockCloakInterval, 0.02f);

		// Advanced per discharge, so successive arcs never stack on one spot.
		// Hashed rather than sequential: an incrementing angle would walk
		// steadily around the wearer like a clock hand.
		const uint32_t seed = ++a_cloak.strikeSeed * 2654435761u;
		const float angle = static_cast<float>(seed >> 8 & 0xFFFF) / 65535.0f * 6.2831853f;
		const float spread = kCloakStrikeInner +
		                     (1.0f - kCloakStrikeInner) * static_cast<float>(seed >> 3 & 0xFF) / 255.0f;
		const float reach = cloakReach * spread;

		const float2 strike{ current.x + std::cos(angle) * reach, current.y + std::sin(angle) * reach };
		emitter.position = strike;
		// A discharge lands, it does not sweep - no capsule from the last one.
		emitter.previous = strike;
		emitter.radius = radius;
		emitter.strength = strength;
		emitter.rate = 0.0f;
		emitter.pitScale = cloakReach * std::max(settings.ShockCloakStrikeScale, 0.05f) /
		                   std::max(settings.PitRadius, 4.0f);
		spellEmitters.push_back(emitter);
		return;
	}

	emitter.position = current;
	emitter.previous = previous;
	emitter.radius = radius;
	emitter.strength = strength;
	emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * a_cloak.rateScale * heightFade;
	spellEmitters.push_back(emitter);
}

void SnowDeformation::GatherSpellEmitters()
{
	spellEmitters.clear();
	spellStats = {};

	if (!settings.EnableSpellIntegration) {
		spellPrevPositions.clear();
		spellTrailPrev.clear();
		spellAuraPrev.clear();
		activeCloaks.clear();
		return;
	}

	if (!spellCastSinkRegistered)
		RegisterSpellCastSink();

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
		live.reserve(manager->limited.size() + manager->unlimited.size() + manager->pending.size());
		// All three lists. A projectile can sit in pending before the manager
		// promotes it, and one missed on the frame it is recorded looks exactly
		// like one that detonated.
		for (auto* list : { &manager->limited, &manager->unlimited, &manager->pending })
			for (auto& handle : *list)
				if (auto projectile = handle.get())
					live.push_back(projectile);
	}

	const RE::NiPoint3 cameraPosition = Util::GetEyePosition();
	const float cullRadius = 0.5f * deformWorldSize;
	std::unordered_map<uint32_t, float2> currentPositions;
	std::unordered_map<uint32_t, float2> currentTrailPositions;
	currentAuraPositions.clear();

	// Cloaks. Started by a cast, ended by their own declared duration, and
	// never once read off a live actor - only the wearer's POSITION is touched
	// here, which is the same read every stamp in this feature already makes.
	{
		std::vector<CloakState> arrived;
		{
			std::scoped_lock lock(queuedCastLock);
			arrived.swap(queuedCloaks);
		}
		for (auto& cloak : arrived)
			if (auto actor = cloak.actor.get())
				activeCloaks[actor->formID] = cloak;  // a re-cast refreshes

		const float cloakDelta = globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f;
		for (auto it = activeCloaks.begin(); it != activeCloaks.end();) {
			it->second.remaining -= cloakDelta;
			auto actor = it->second.remaining > 0.0f ? it->second.actor.get() : RE::NiPointer<RE::Actor>();
			if (!actor) {
				it = activeCloaks.erase(it);
				continue;
			}
			if (cameraPosition.GetSquaredDistance(actor->GetPosition()) <= cullRadius * cullRadius)
				ConsiderActorAuras(actor.get(), it->second, cloakDelta);
			++it;
		}
	}

	// Every projectile still flying, recorded before any culling or
	// classification. Anything missing from this next frame has DIED; a
	// projectile that merely flew out of the window is still in here, so
	// leaving is never mistaken for detonating.
	//
	// A projectile already flagged destroyed is treated as gone even while the
	// manager still lists it: waiting for it to leave would strand its blast
	// forever if the game keeps spent projectiles around.
	std::unordered_set<uint32_t> stillAlive;
	stillAlive.reserve(live.size());
	for (auto& projectile : live) {
		if (!projectile)
			continue;
		if (projectile->GetProjectileRuntimeData().flags.any(RE::Projectile::Flags::kDestroyed))
			continue;
		stillAlive.insert(projectile->formID);
	}

	for (auto& projectile : live) {
		if (spellEmitters.size() >= kMaxSpellEmitters)
			break;
		if (!projectile || !projectile->Is3DLoaded())
			continue;

		auto& runtime = projectile->GetProjectileRuntimeData();

		// The projectile usually carries its own effect, which answers element,
		// delivery and casting type on its own.
		const RE::EffectSetting* effect = runtime.avEffect;
		const RE::Effect* costliest = runtime.spell ? runtime.spell->GetCostliestEffectItem() : nullptr;
		if (!effect && costliest)
			effect = costliest->baseEffect;
		SpellElement element = ClassifyElement(effect);

		// When it does not, walk the spell's whole effect list rather than
		// trusting the costliest one. A spell that carries several effects
		// need not have the elemental one rated costliest, and the effect that
		// names the element is not always the one that names the explosion, so
		// both are searched independently.
		const RE::BGSExplosion* blast = runtime.explosion;
		if (effect && effect->data.explosion && !blast)
			blast = effect->data.explosion;
		// An aimed bolt keeps its explosion on the PROJECTILE record, not on
		// any effect: the effect says what the magic does, the projectile says
		// what the thing in flight does when it stops.
		if (!blast)
			if (auto* base = projectile->GetBaseObject())
				if (auto* projectileBase = base->As<RE::BGSProjectile>())
					blast = projectileBase->data.explosionType;
		if ((element == SpellElement::None || !blast) && runtime.spell) {
			for (auto* item : runtime.spell->effects) {
				const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
				if (!base)
					continue;
				if (element == SpellElement::None) {
					const SpellElement candidate = ClassifyElement(base);
					if (candidate != SpellElement::None) {
						element = candidate;
						if (!effect)
							effect = base;
					}
				}
				if (!blast && base->data.explosion)
					blast = base->data.explosion;
			}
		}
		if (!effect)
			continue;
		spellStats.projectiles++;

		if (element == SpellElement::None) {
			spellStats.rejectedElement++;
			continue;
		}

		const RE::NiPoint3 position = projectile->GetPosition();
		if (cameraPosition.GetSquaredDistance(position) > cullRadius * cullRadius)
			continue;

		// Heading, shared by the stream trace and the blast below.
		RE::NiPoint3 direction = runtime.velocity;
		const float speed = direction.Length();
		if (speed > kMinSpellSpeed)
			direction /= speed;
		else if (!ShooterAim(runtime.shooter, direction))
			direction = { 0.0f, 0.0f, -1.0f };

		// How far this projectile can actually reach, off its own record. A
		// single global number gave Sparks the range of a master beam.
		float spellReach = kSpellStreamReachDefault;
		bool hitscan = false;
		if (auto* baseForm = projectile->GetBaseObject())
			if (auto* projectileBase = baseForm->As<RE::BGSProjectile>()) {
				if (projectileBase->data.range > 1.0f)
					spellReach = std::clamp(projectileBase->data.range, kSpellReachMin, kSpellReachMax);
			}
		hitscan = runtime.flags.any(RE::Projectile::Flags::kHitScan);

		// Remember what this projectile would leave if it went off here, for
		// EVERY projectile - this must sit above the concentration gate below,
		// because the things that detonate are precisely the ones that gate
		// rejects. The element comes off its own effect, so a blast needs
		// neither the explosion reference nor the explosion-to-element table.
		// A held stream marks continuously through the contact trace below, so it
		// must neither leave a crater every time one of its short lived
		// projectiles expires nor cut a corridor on the way. Everything else
		// marks by striking.
		const bool strikes = effect->data.castingType != RE::MagicSystem::CastingType::kConcentration;

		{
			if (!blast && !strikes)
				spellStats.rejectedNoBlast++;
			if (strikes || blast) {
				float blastGroundZ = position.z;
				tes->GetLandHeight(position, blastGroundZ);
				const float authored = blast ?
				                           std::clamp(blast->data.radius, kExplosionRadiusMin, kExplosionRadiusMax) :
				                           kImpactRadiusDefault;
				PendingBlast pending{};
				pending.pitScale = std::clamp(authored / kPitReferenceRadius, kPitScaleMin, kPitScaleMax);
				// A hitscan bolt resolves where it was fired, so the only
				// sighting we ever get is the muzzle and its strike may be its
				// whole range away. Bounding that by a frame of travel put the
				// mark at the caster's feet instead of on the target.
				pending.landingReach = hitscan ? spellReach :
				                                 std::max(kSpellReachMin, speed * kBlastTravelWindow);
				pending.position = position;
				pending.direction = direction;
				pending.heightAboveLand = position.z - blastGroundZ;
				pending.radius = authored * std::max(settings.BlastRadiusScale, 0.0f);
				pending.element = element;
				projectileBlasts[projectile->formID] = pending;
			}
		}

		// The corridor a projectile cuts on its way in. The shell has no
		// collision, so a bolt flies THROUGH the snow and detonates on the
		// terrain underneath: without this it silently vanishes for the frames
		// it spends inside the layer, then a crater appears from nowhere.
		//
		// Depth cut is what the column loses above the projectile, so a bolt
		// skimming the surface scores a shallow groove and one deep in the
		// snow melts nearly to the ground - which is also why this reads
		// correctly on a slope, where entry and impact are at different depths.
		if (strikes) {
			const float columnDepth = GetNominalSnowDepthAt(position.x, position.y, 0.0f);
			float trailGroundZ = position.z;
			tes->GetLandHeight(position, trailGroundZ);
			const float heightAboveLand = position.z - trailGroundZ;

			const float2 currentTrail{ position.x, position.y };
			float2 previousTrail = currentTrail;
			if (auto it = spellTrailPrev.find(projectile->formID); it != spellTrailPrev.end()) {
				const float dx = currentTrail.x - it->second.x;
				const float dy = currentTrail.y - it->second.y;
				if (dx * dx + dy * dy < kSpellTrailBreak * kSpellTrailBreak)
					previousTrail = it->second;
			}
			currentTrailPositions[projectile->formID] = currentTrail;

			if (columnDepth > kMinTrailDepth && heightAboveLand >= 0.0f && heightAboveLand < columnDepth) {
				const float cut = 1.0f - heightAboveLand / columnDepth;
				if (cut >= kMinSpellStrength && spellEmitters.size() < kMaxSpellEmitters) {
					SpellEmitter emitter{};
					emitter.position = currentTrail;
					emitter.previous = previousTrail;
					emitter.radius = kTrailRadius;
					emitter.strength = cut;
					emitter.rate = kTrailRate;
					emitter.element = element;
					emitter.mark = MarkForElement(element);
					emitter.pitScale = kTrailPitScale;
					spellEmitters.push_back(emitter);
					spellStats.trails++;
				}
			}
		}

		// Concentration only from here: a held stream is what the sweeping
		// contact trace below is for. Everything else marks by detonating.
		if (effect->data.castingType != RE::MagicSystem::CastingType::kConcentration)
			continue;
		spellStats.streams++;

		// Where the stream lands. A flame held at hand height still melts what
		// it is pointed at, so the mark belongs at the ground contact, not
		// under the projectile.
		RE::NiPoint3 markPosition{};
		float strength = 0.0f;
		float radius = 0.0f;

		RE::NiPoint3 contact{};
		if (TraceGroundContact(tes, position, direction, spellReach, contact)) {
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
		emitter.pitScale = kStreamPitScale;
		spellEmitters.push_back(emitter);
	}

	// A projectile gone from the manager has detonated. This is where a rune
	// finally marks the snow: it is a projectile the whole time it waits, and
	// what carves the ground is the blast it becomes on the frame it vanishes.
	for (auto it = projectileBlasts.begin(); it != projectileBlasts.end();) {
		if (stillAlive.contains(it->first)) {
			++it;
			continue;
		}
		const PendingBlast blast = it->second;
		it = projectileBlasts.erase(it);

		if (blast.element == SpellElement::None)
			continue;

		// A bolt dies between frames, so the last sighting sits short of the
		// impact - the faster it flew, the shorter. Carrying the flight on to
		// the ground puts the crater where it struck instead of where it was
		// last drawn, which is the same reason a fire stream marks its contact
		// rather than its emitter.
		float2 markPosition = { blast.position.x, blast.position.y };
		float strength = 0.0f;
		float radius = 0.0f;

		RE::NiPoint3 landing{};
		const float reach = std::max(blast.landingReach, kBlastLandingReach);
		const bool landed = TraceGroundContact(tes, blast.position, blast.direction, reach, landing) &&
		                    blast.position.GetDistance(landing) <= reach;
		if (landed) {
			markPosition = { landing.x, landing.y };
			strength = 1.0f;
			radius = blast.radius;
		} else if (!GroundMark(blast.heightAboveLand, blast.radius, strength, radius)) {
			// Stopped by an actor or a wall well above the snow: the ground
			// below takes the weaker, broader mark rather than a full crater.
			continue;
		}
		if (strength < kMinSpellStrength)
			continue;
		spellStats.detonations++;
		if (activeBlasts.size() >= kMaxSpellEmitters)
			continue;

		spellStats.lastStrength = strength;
		spellStats.lastRadius = radius;

		ActiveBlast opened{};
		opened.position = markPosition;
		// Carried from the AUTHORED radius rather than the fire-scaled one.
		opened.pitScale = blast.pitScale;
		opened.radius = radius;
		opened.strength = strength;
		opened.rate = kExplosionRate;
		opened.remaining = kBlastDuration;
		opened.element = blast.element;
		opened.mark = MarkForElement(blast.element);
		activeBlasts.push_back(opened);
	}

	// Self-centred area spells enter here: the sink queued them on the game
	// thread because there is no object anywhere to watch.
	{
		std::vector<QueuedCast> drained;
		{
			std::scoped_lock lock(queuedCastLock);
			drained.swap(queuedCasts);
		}
		for (const auto& cast : drained) {
			float castGroundZ = cast.position.z;
			tes->GetLandHeight(cast.position, castGroundZ);
			const float scaled = cast.radius * std::max(settings.BlastRadiusScale, 0.0f);
			float strength = 0.0f;
			float radius = 0.0f;
			if (!GroundMark(cast.position.z - castGroundZ, scaled, strength, radius))
				continue;
			if (strength < kMinSpellStrength)
				continue;
			spellStats.casts++;
			if (activeBlasts.size() >= kMaxSpellEmitters)
				continue;

			spellStats.lastStrength = strength;
			spellStats.lastRadius = radius;

			ActiveBlast opened{};
			opened.position = { cast.position.x, cast.position.y };
			opened.radius = radius;
			opened.strength = strength;
			opened.rate = kExplosionRate;
			opened.remaining = kBlastDuration;
			opened.element = cast.element;
			opened.mark = MarkForElement(cast.element);
			activeBlasts.push_back(opened);
		}
	}

	// Every blast still opening marks again this frame. Without this a
	// detonation would be one frame of melt and therefore invisible unless its
	// rate were made absurd.
	{
		const float deltaTime = globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f;
		for (auto it = activeBlasts.begin(); it != activeBlasts.end();) {
			if (spellEmitters.size() < kMaxSpellEmitters) {
				SpellEmitter emitter{};
				emitter.position = it->position;
				emitter.previous = it->position;
				emitter.radius = it->radius;
				emitter.strength = it->strength;
				emitter.rate = it->rate;
				emitter.element = it->element;
				emitter.mark = it->mark;
				emitter.pitScale = it->pitScale;
				spellEmitters.push_back(emitter);
			}
			it->remaining -= deltaTime;
			it = it->remaining > 0.0f ? it + 1 : activeBlasts.erase(it);
		}
	}

	spellPrevPositions = std::move(currentPositions);
	spellTrailPrev = std::move(currentTrailPositions);
	spellAuraPrev = std::move(currentAuraPositions);
	spellStats.emitters = static_cast<uint>(spellEmitters.size());
	spellStats.armed = static_cast<uint>(projectileBlasts.size());
}
