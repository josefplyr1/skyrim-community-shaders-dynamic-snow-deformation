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
// Authored radius up to which a placed hazard is one PIECE of something - a
// wall is laid down as a row of tiny hazards, and a frost barrier segment is
// 3.5 where a blizzard is 20 to 40. Only the small ones are objects standing
// on the ground that the snow can bury; an area effect has no mesh to swallow.
static constexpr float kHazardPieceRadius = 6.0f;
// Never raise by more than this, whatever the snow says. A lift is a lie told
// to hide a burial, and a big one starts reading as the effect floating.
static constexpr float kLiftFraction = 0.5f;
static constexpr float kMaxLiftHeight = 60.0f;
// And only HALF way up, per Josef after seeing it in game. A wall standing on
// top of the snow reads as balanced on it; half sunk it reads as standing IN
// it, which is what a wall of frost should look like - and the crust pattern
// laid around its feet closes the join.
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
// Held at least long enough to finish ramping in, whatever the ramp is set to.
static constexpr float kBlastMinHold = 0.05f;
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
// A dead body has stopped hovering, so its mark sits on the ground rather than
// at chest height - and it works a wider circle than the living one did,
// because the whole body is lying in the snow instead of passing over it.
static constexpr float kDeathAuraReach = 1.35f;
// A corpse works a tighter circle than a living source of the same school: it
// is one body lying still, not something walking about radiating.
static constexpr float kCorpseReachScale = 0.5f;
// Bound radius of a human-sized actor, which the corpse reach is measured
// against. A mammoth lying in the snow should mark more of it than a mudcrab,
// and neither should take a number meant for a man.
static constexpr float kCorpseReferenceBound = 80.0f;
static constexpr float kCorpseSizeMin = 0.4f;
static constexpr float kCorpseSizeMax = 2.5f;
// How fast the blast a dying atronach throws reaches its basin. Same shape as
// any other detonation: held open for a moment rather than applied in a frame.
static constexpr float kAtronachDeathRate = 4.0f;
// How recently an actor must have been seen alive for its DISAPPEARANCE to
// count as a death. An atronach is unsummoned when it dies, so it is simply
// gone on the next frame - but so is one whose cell the player walked out of,
// and that one must not leave a crater behind it.
static constexpr float kInnateDeathWindow = 0.5f;
// Resistance an innate ability must grant before an actor counts as being MADE
// of that element. Deliberately near-total: the playable races and the cold
// animals all sit at 50, and every elemental creature in the game sits at 100.
static constexpr float kElementalImmunity = 90.0f;
// How long a mark takes to reach full depth. A pit follows CARVE, which is
// instantaneous, so a discharge otherwise simply EXISTS on the frame it lands -
// snow that was never seen to move. ONE number for every discharge in the
// feature: bolts, cloak arcs, storm atronachs, electrified bodies, runes and
// walls all ramp on this.
static constexpr float kBlastRampSeconds = 0.09f;
// How long the memory of an elemental hit survives. What a body is burning
// with is whatever struck it moments before it fell - not something from a
// fight two rooms back.
static constexpr float kElementalMemory = 6.0f;
// Discs laid along a shout's axis. Enough that their overlap reads as one
// continuous wedge rather than a row of circles, few enough that a shout
// cannot swallow the stamp pool on its own.
static constexpr int kShoutConeDiscs = 10;
// How far in front of the shouter the wedge begins, in world units. A shout
// comes out of a MOUTH, so the ground the shouter is standing on is not in it -
// roughly a body's width plus half a metre at Skyrim's scale, where a metre is
// about 70 units. Without this the cone opened from between their feet.
static constexpr float kShoutMouthOffset = 60.0f;
// Authored impact force of Unrelenting Force's own projectile, which every
// other shout is measured against. Read off the records rather than invented:
// the player's push is 50, a dragon's 85, the breaths 10.
static constexpr float kShoutReferenceForce = 50.0f;
static constexpr float kShoutForceMin = 0.25f;
static constexpr float kShoutForceMax = 1.5f;
// Aim this far above horizontal and the shout passes over everything. Shouts
// are shockwaves along the ground, so pitch shortens the reach rather than
// tilting the mark - the map is 2D and has no way to hold a raised one.
static constexpr float kShoutMaxPitchDeg = 40.0f;
// A stagger's magnitude is a FRACTION in Skyrim - how much of a stagger, 0 to
// 1. Unrelenting Force authors 0.75 and Bend Will the same. Anything far above
// that is not a stagger amount at all; the archetype is being borrowed for
// something else, and it always turns out to be sound rather than shove -
// Dismay and Dragonrend author 25, and a werewolf's howl 5. Josef put it
// better: a howl is sound, not power.
static constexpr float kStaggerFraction = 1.0f;
// Impact force that the shared push projectile carries no matter what the
// shout does with it. VoicePushProjectile01 is 50 and is thrown by Unrelenting
// Force, Marked for Death and the werewolf howl alike, so 50 is evidence of
// nothing. Above it the author chose a bigger number on purpose: a dragon's
// push is 85 and Cyclone's is 1000.
static constexpr float kShoutCarrierForce = 50.0f;
// Speed below which a shove is a THING TRAVELLING rather than a shockwave, and
// so leaves the line it took instead of a wedge from the mouth. Every real
// shout blast crosses its own reach in about a second - Unrelenting Force at
// 1536 units a second, the breaths at 1200, Disarm at 1024. Cyclone crawls at
// 512, barely above a sprint, and takes four seconds to cross its 2048; you
// watch it go. The bar sits well clear of both, and well clear of a running
// man, which is the honest way to say it: anything a person could keep pace
// with is an object moving over the ground, not a blast leaving it.
static constexpr float kForceTrackSpeed = 800.0f;
// Impact force past which a shove is a vortex rather than a blast, whatever
// its speed says. A second and independent reading of the same fact: no
// shockwave in either master authors above 85, and a cyclone authors 1000. Two
// numbers have to agree that a thing is a shockwave before it gets a wedge.
static constexpr float kVortexForce = 200.0f;
// A shout's reach if its projectile names none.
static constexpr float kShoutDefaultRange = 1000.0f;
static constexpr float kShoutRangeMax = 12000.0f;
// Travel speed if a shout's projectile names none. Vanilla authors 1536 for
// Unrelenting Force and 1200 for the breaths.
static constexpr float kShoutDefaultSpeed = 1400.0f;
// The front's reach on the very first frame is one frame of travel and no more.
// A floor here was a mistake worth remembering: at eight percent of the wedge
// it meant the first stretch in front of the shouter simply EXISTED, and the
// travel only began beyond it.
// Rate a BLAST sets its crust at. The crust rate in settings is tuned for a
// sustained source - a frost cloak standing over one spot for seconds - and at
// 0.6 a second it reached barely a fifth of a glaze inside a blast's window,
// which is why Frost Breath was all but invisible while Fire Breath cut to the
// floor. A blast has one moment, so it has to arrive within it.
static constexpr float kBlastCrustRate = 4.0f;
// How long a self-delivered shout is watched for a dash. The dash itself is
// over well inside this; the window only has to outlast the wind-up.
static constexpr float kDashWindow = 1.5f;
// Speed past which an actor is being THROWN rather than running. Skyrim's run
// is about 350 units a second and its sprint about 500; Whirlwind Sprint
// crosses a thousand units in well under one, so there is a wide gap to sit in.
static constexpr float kDashSpeedGate = 800.0f;
// Beyond this in a frame it was not a dash but a teleport or a cell change,
// and a capsule across it would comb a line over the whole world.
static constexpr float kDashBreak = 700.0f;
// Half-width of the furrow, against the dasher's own bounding sphere. Half,
// because a body ploughs a channel about as wide as itself rather than twice.
static constexpr float kDashWidthOfBound = 0.22f;
static constexpr float kDashWidthMin = 10.0f;
static constexpr float kDashWidthMax = 220.0f;
// A dasher clear of the ground by more than this is passing OVER the snow, not
// through it. Whirlwind Sprint off a ledge carries you well clear, and the map
// is 2D - it cannot hold a furrow in the air, so it must not cut one below.
static constexpr float kDashGroundBand = 45.0f;
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
// Magic-effect area is authored in feet, not in world units - roughly 21 units
// to the foot at Skyrim's scale. Blizzard's 40 is therefore about 850 units
// across, which is why treating the number as a multiplier on a 90 unit radius
// left it a fraction of its own animation.
static constexpr float kSelfAreaToUnits = 21.0f;
static constexpr float kSelfAreaReachMin = 60.0f;
static constexpr float kSelfAreaReachMax = 1400.0f;
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
	case SpellElement::Force:
		// The only school that DISPLACES snow instead of changing it, which is
		// also the only one the berm field answers - a carve keeps its full
		// rim, and that rim is what reads as pushed rather than deleted.
		return SpellMark::Carve;
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

void SnowDeformation::LiftRefOntoSnow(RE::TESObjectREFR* a_ref, float a_lift)
{
	if (!a_ref || a_lift < 1.0f)
		return;
	auto* taskInterface = SKSE::GetTaskInterface();
	if (!taskInterface)
		return;
	// A handle, not the pointer: by the time the task runs the reference may
	// have gone, and a hazard's whole life is measured in seconds.
	const RE::ObjectRefHandle handle = a_ref->CreateRefHandle();
	taskInterface->AddTask([handle, a_lift]() {
		auto ref = handle.get();
		if (!ref)
			return;
		auto* root = ref->Get3D(false);
		if (!root)
			return;
		root->local.translate.z += a_lift;
		RE::NiUpdateData data{};
		root->UpdateWorldData(&data);
	});
}

void SnowDeformation::ConsiderHazard(RE::TESObjectREFR* a_ref)
{
	if (!settings.EnableSpellIntegration || spellEmitters.size() >= kSpellEmitterCeiling)
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

	// Raise a buried frost effect onto the snow standing over it. Frost only:
	// fire melts its own hole and lightning pits one, so those effects already
	// sit in snow they removed, while frost hardens what is there and leaves
	// the layer at full height on top of itself. And only the small PIECES -
	// a wall is a row of them - because an area hazard has no mesh to bury.
	if (settings.LiftFrostEffects && element == SpellElement::Frost &&
		base->data.radius <= kHazardPieceRadius && !liftedRefs.contains(a_ref->formID)) {
		if (liftedRefs.size() > 512)
			liftedRefs.clear();
		liftedRefs.insert(a_ref->formID);
		const float lift = std::min(GetNominalSnowDepthAt(position.x, position.y, 0.0f) * kLiftFraction,
			kMaxLiftHeight);
		if (lift >= 1.0f) {
			LiftRefOntoSnow(a_ref, lift);
			spellStats.lifted++;
		}
	}

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

	// A rune or a wall segment is a discharge too, and unlike a cloak arc it is
	// re-emitted every frame rather than opened once - so it cannot ride the
	// blast path, and would pop to full depth on the frame it appeared. It
	// carries its own age, so the same ramp costs no state at all. Melt and
	// crust need none of this: they integrate, so they already arrive over time.
	const float hazardRamp = MarkForElement(element) == SpellMark::Pit ?
	                             std::clamp(runtime.age / std::max(kBlastRampSeconds, 1e-3f), 0.0f, 1.0f) :
	                             1.0f;

	SpellEmitter emitter{};
	emitter.position = { position.x, position.y };
	// A wall segment and a rune both stay put, so there is no sweep to carry.
	emitter.previous = emitter.position;
	emitter.radius = radius;
	emitter.strength = strength * hazardRamp;
	emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest) * heightFade;
	emitter.element = element;
	emitter.mark = MarkForElement(element);
	emitter.pitScale = std::clamp(baseRadius / kPitReferenceRadius, kPitScaleMin, kPitScaleMax);
	emitter.rateScale = RateScaleOf(costliest) * heightFade;
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

	// A SHOUT ploughs the ground in front of the shouter, and nothing else in
	// the feature can express that: a cone is not a capsule, and the breaths
	// were leaving the two marks their projectile happened to make. Voice
	// power is the discriminator the design named, and AIMED is what separates
	// the ones that point somewhere from Storm Call and Whirlwind Sprint,
	// which are self-delivered and already work through other routes.
	if (feature.settings.EnableShoutCones)
		if (auto* spellItem = spell->As<RE::SpellItem>();
			spellItem && spellItem->data.spellType == RE::MagicSystem::SpellType::kVoicePower) {
			feature.ConsiderShout(spellItem, a_event->object.get());

			// And a watch, in case this is a shout that throws its caster.
			// Opened for every self-delivered one, because no reading of the
			// records can tell those apart - see DashWatch. What separates them
			// is whether the caster then MOVES, which costs nothing to watch.
			bool selfDelivered = false;
			bool elemental = false;
			for (const auto* item : spellItem->effects) {
				const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
				if (!base)
					continue;
				if (base->data.delivery == RE::MagicSystem::Delivery::kSelf)
					selfDelivered = true;
				if (ClassifyElement(base) != SpellElement::None)
					elemental = true;
			}
			if (selfDelivered && !elemental)
				if (auto* actor = a_event->object->As<RE::Actor>()) {
					DashWatch watch{};
					watch.actor = actor->GetHandle();
					watch.remaining = kDashWindow;
					std::scoped_lock lock(feature.queuedCastLock);
					if (feature.queuedDashes.size() < kMaxSpellEmitters)
						feature.queuedDashes.push_back(watch);
				}
		}

	// Self-delivered effects ONLY. Anything aimed or placed already has a
	// projectile or a hazard to follow, and marking it here as well would drop
	// a second crater under the caster's own feet every time they threw one.
	SpellElement element = SpellElement::None;
	const RE::BGSExplosion* blast = nullptr;
	SpellElement cloakElement = SpellElement::None;
	float cloakRate = 1.0f;
	float cloakDuration = 0.0f;
	float cloakReachOverride = 0.0f;
	for (auto* item : spell->effects) {
		const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
		if (!base || base->data.delivery != RE::MagicSystem::Delivery::kSelf)
			continue;

		// A cloak declares itself by ARCHETYPE. A self-centred AREA spell -
		// Blizzard and its like - does not, but behaves the same way for our
		// purposes: it sits on the caster and works the ground around them for
		// a duration.
		//
		// DURATION is what makes it cloak-like, and area alone is not enough.
		// Firestorm is self-delivered over an area of 100 and lasts no time at
		// all: it is a detonation, and treating it as a cloak both invented a
		// minute-long aura and skipped past the explosion collection below, so
		// its blast stopped being queued entirely. Blizzard authors 40 over ten
		// seconds and is the real thing.
		const bool selfArea = item->effectItem.area > 0 && item->effectItem.duration > 0;
		if (base->data.archetype == RE::EffectSetting::Archetype::kCloak || selfArea) {
			const SpellElement candidate = ClassifyElement(base);
			// First qualifying effect wins: a spell can carry several, and
			// Blizzard's paralysis rider should not displace its frost.
			if (candidate != SpellElement::None && cloakElement == SpellElement::None) {
				cloakElement = candidate;
				cloakRate = std::clamp(item->effectItem.magnitude / kSpellReferenceMagnitude,
					kSpellMagnitudeMin, kSpellMagnitudeMax);
				cloakDuration = item->effectItem.duration > 0 ?
				                    static_cast<float>(item->effectItem.duration) :
				                    kCloakDefaultDuration;
				cloakReachOverride = selfArea ?
				                         std::clamp(static_cast<float>(item->effectItem.area) * kSelfAreaToUnits,
											 kSelfAreaReachMin, kSelfAreaReachMax) :
				                         0.0f;
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
			cloak.reachOverride = cloakReachOverride;
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

RE::BSEventNotifyControl SnowDeformation::DeathSink::ProcessEvent(
	const RE::TESDeathEvent* a_event, RE::BSTEventSource<RE::TESDeathEvent>*)
{
	auto& feature = globals::features::snowDeformation;
	// The dying edge, not the dead one: a flame atronach is already coming
	// apart by the time the second fires, and its position is the better read
	// while the body is still where it fell.
	if (!a_event || a_event->dead || !a_event->actorDying || !feature.settings.EnableSpellIntegration)
		return RE::BSEventNotifyControl::kContinue;
	auto* actor = a_event->actorDying->As<RE::Actor>();
	if (!actor)
		return RE::BSEventNotifyControl::kContinue;

	// Uncached on purpose: this runs on the GAME thread, and the race cache is
	// the gather's. A death is rare enough that repeating the walk costs
	// nothing worth sharing state for.
	InnateAuraRecord record{};
	if (!AuraFromActorRecords(actor, record))
		return RE::BSEventNotifyControl::kContinue;
	// Counted here rather than at the drain, so the menu can tell an event
	// that never arrived from a mark that arrived and was rejected. That
	// distinction is the whole of what the parked atronach question needs.
	feature.spellStats.deathsSeen++;

	QueuedDeath queued{};
	queued.formID = actor->formID;
	queued.position = actor->GetPosition();
	queued.element = record.element;
	{
		std::scoped_lock lock(feature.queuedCastLock);
		if (feature.queuedDeaths.size() < kMaxSpellEmitters)
			feature.queuedDeaths.push_back(queued);
	}
	return RE::BSEventNotifyControl::kContinue;
}

RE::BSEventNotifyControl SnowDeformation::MagicApplySink::ProcessEvent(
	const RE::TESMagicEffectApplyEvent* a_event, RE::BSTEventSource<RE::TESMagicEffectApplyEvent>*)
{
	auto& feature = globals::features::snowDeformation;
	if (!a_event || !a_event->target || !feature.settings.EnableSpellIntegration ||
		!feature.settings.CorpseElementalMarks)
		return RE::BSEventNotifyControl::kContinue;
	auto* actor = a_event->target->As<RE::Actor>();
	if (!actor)
		return RE::BSEventNotifyControl::kContinue;

	// The effect RECORD, classified through the same resist variable as every
	// other detector. Nothing is read off the actor but its identity.
	auto* form = RE::TESForm::LookupByID(a_event->magicEffect);
	const SpellElement element = ClassifyElement(form ? form->As<RE::EffectSetting>() : nullptr);
	if (element == SpellElement::None)
		return RE::BSEventNotifyControl::kContinue;

	QueuedHit queued{};
	queued.formID = actor->formID;
	queued.element = element;
	{
		std::scoped_lock lock(feature.queuedCastLock);
		// Damage ticks arrive constantly, so this is a bound rather than a
		// budget: the newest hit is the one that matters and the rest are the
		// same answer repeated.
		if (feature.queuedHits.size() < kMaxSpellEmitters)
			feature.queuedHits.push_back(queued);
	}
	return RE::BSEventNotifyControl::kContinue;
}

bool SnowDeformation::ShoutLaysCone(const RE::MagicItem* a_spell) const
{
	if (!settings.EnableShoutCones)
		return false;
	auto* spellItem = a_spell ? a_spell->As<RE::SpellItem>() : nullptr;
	if (!spellItem || spellItem->data.spellType != RE::MagicSystem::SpellType::kVoicePower)
		return false;

	// The same reading ConsiderShout makes, so the two can never disagree about
	// which shouts wedge.
	SpellElement element = SpellElement::None;
	bool shoves = false;
	float shoveForce = 0.0f;
	const RE::BGSProjectile* projectile = nullptr;
	for (const auto* item : spellItem->effects) {
		const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
		if (!base || base->data.delivery != RE::MagicSystem::Delivery::kAimed)
			continue;
		if (element == SpellElement::None)
			element = ClassifyElement(base);
		if (base->data.archetype == RE::EffectSetting::Archetype::kStagger &&
			item->effectItem.magnitude <= kStaggerFraction)
			shoves = true;
		if (base->data.projectileBase) {
			shoveForce = std::max(shoveForce, base->data.projectileBase->data.force);
			if (!projectile || base->data.projectileBase->data.range > projectile->data.range)
				projectile = base->data.projectileBase;
		}
	}
	if (element == SpellElement::None && (shoves || shoveForce > kShoutCarrierForce))
		element = SpellElement::Force;
	if (element == SpellElement::None)
		return false;
	// A travelling shove lays no wedge - the projectile route IS its mark, so
	// this must not silence the very thing that draws it.
	return !(element == SpellElement::Force && IsTravellingShove(projectile, shoveForce));
}

bool SnowDeformation::IsTravellingShove(const RE::BGSProjectile* a_projectile, float a_force)
{
	if (a_force > kVortexForce)
		return true;
	return a_projectile && a_projectile->data.speed > 0.0f &&
	       a_projectile->data.speed <= kForceTrackSpeed;
}

bool SnowDeformation::SpellShoves(const RE::MagicItem* a_spell, float& a_force)
{
	a_force = 0.0f;
	if (!a_spell)
		return false;
	bool shoves = false;
	for (const auto* item : a_spell->effects) {
		const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
		if (!base || base->data.delivery != RE::MagicSystem::Delivery::kAimed)
			continue;
		if (ClassifyElement(base) != SpellElement::None)
			return false;  // an element outranks a shove, always
		if (base->data.archetype == RE::EffectSetting::Archetype::kStagger &&
			item->effectItem.magnitude <= kStaggerFraction)
			shoves = true;
		if (base->data.projectileBase)
			a_force = std::max(a_force, base->data.projectileBase->data.force);
	}
	return shoves || a_force > kShoutCarrierForce;
}

void SnowDeformation::ConsiderShout(const RE::SpellItem* a_spell, RE::TESObjectREFR* a_caster)
{
	if (!a_spell || !a_caster)
		return;

	// Element first, force second. Every breath carries a small stagger of its
	// own - Fire Breath's is 0.05 against Unrelenting Force's 0.75 - so a
	// stagger cannot be read as force wherever the spell already names an
	// element, or the breaths would carve as well as burn.
	SpellElement element = SpellElement::None;
	bool shoves = false;
	float shoveForce = 0.0f;
	const RE::BGSProjectile* projectile = nullptr;
	for (const auto* item : a_spell->effects) {
		const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
		// AIMED only. Storm Call and Whirlwind Sprint are self-delivered: the
		// first already pocks the snow through the strikes it calls down, and
		// the second is a dash rather than anything thrown forward.
		if (!base || base->data.delivery != RE::MagicSystem::Delivery::kAimed)
			continue;
		if (element == SpellElement::None)
			element = ClassifyElement(base);
		if (base->data.archetype == RE::EffectSetting::Archetype::kStagger &&
			item->effectItem.magnitude <= kStaggerFraction)
			shoves = true;
		// The longest reach any of its effects throws, and separately the
		// hardest it hits - a spell can carry several and they need not agree.
		if (base->data.projectileBase) {
			shoveForce = std::max(shoveForce, base->data.projectileBase->data.force);
			if (!projectile || base->data.projectileBase->data.range > projectile->data.range)
				projectile = base->data.projectileBase;
		}
	}
	// Two ways to be a shove, because one is not enough. A real stagger says so
	// outright; but a dragon's Unrelenting Force carries only a script effect
	// and Cyclone only damage, and both announce themselves instead through a
	// projectile that hits far harder than the shared carrier ever does.
	if (element == SpellElement::None && (shoves || shoveForce > kShoutCarrierForce))
		element = SpellElement::Force;

	// Whatever happens next, say what was read. A wedge that should have been a
	// track is otherwise only arguable, and this turns it into a number.
	spellStats.lastShoutElement = static_cast<uint>(element);
	spellStats.lastShoutSpeed = projectile ? projectile->data.speed : 0.0f;
	spellStats.lastShoutForce = shoveForce;
	spellStats.lastShoutVerdict = 3;

	if (element == SpellElement::None)
		return;

	// A shove that TRAVELS is not a shockwave and gets no wedge. It is an
	// object making its way across the ground, and what it leaves is the line
	// it took - which the projectile route follows for real, so it need not be
	// guessed at from the caster's facing here.
	//
	// Two independent readings, and either is enough, because they are two
	// ways of noticing the same thing and one of them failing quietly would
	// hand a cyclone a wedge. Speed: every shockwave crosses its reach in about
	// a second, a cyclone crawls for four. Impact force: no shockwave in either
	// master authors above 85, and a cyclone authors 1000.
	if (element == SpellElement::Force && IsTravellingShove(projectile, shoveForce)) {
		spellStats.lastShoutVerdict = 2;
		return;
	}
	spellStats.lastShoutVerdict = 1;

	// Reach off the projectile's own record. The effects author no AREA at all
	// for any shout in the game, so there is nothing else to read - and the
	// ranges differ by ten times across them, from a dragon's 10000 down to
	// Dismay's 768, which no single constant could have covered.
	float range = projectile && projectile->data.range > 1.0f ? projectile->data.range : kShoutDefaultRange;
	range = std::clamp(range * std::max(settings.ShoutConeLength, 0.0f), 0.0f, kShoutRangeMax);

	// Impact force says how hard a shout SHOVES, so it scales a shove and
	// nothing else. Reading it as a depth for the breaths was wrong and showed
	// it: Fire Breath authors 10 against Unrelenting Force's 50, so its cone
	// arrived at a fifth strength and barely marked. What a flame does to snow
	// is the melt rate's business, exactly as it is for every other fire source.
	const float force = projectile && projectile->data.force > 0.0f ? projectile->data.force :
	                                                                  kShoutReferenceForce;
	const float strength = element == SpellElement::Force ?
	                           std::clamp(force / kShoutReferenceForce, kShoutForceMin, kShoutForceMax) :
	                           1.0f;

	// Flattened heading. A shout is a shockwave along the ground, and the
	// deformation map is 2D - it has no way to hold a mark up in the air - so
	// aiming up shortens the reach rather than tilting the cone, and aiming at
	// the sky throws it away entirely.
	const float pitch = a_caster->data.angle.x;
	const float pitchDeg = -pitch * 57.2957795f;
	if (pitchDeg > kShoutMaxPitchDeg)
		return;
	if (pitchDeg > 0.0f)
		range *= std::max(1.0f - pitchDeg / kShoutMaxPitchDeg, 0.0f);

	const float yaw = a_caster->data.angle.z;
	QueuedCone cone{};
	cone.position = a_caster->GetPosition();
	cone.direction = { std::sin(yaw), std::cos(yaw), 0.0f };
	cone.length = range;
	cone.speed = projectile && projectile->data.speed > 1.0f ? projectile->data.speed : kShoutDefaultSpeed;
	cone.strength = strength;
	cone.element = element;
	{
		std::scoped_lock lock(queuedCastLock);
		if (queuedCones.size() < kMaxSpellEmitters)
			queuedCones.push_back(cone);
	}
}

void SnowDeformation::RegisterSpellCastSink()
{
	spellCastSinkRegistered = true;
	if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
		holder->AddEventSink<RE::TESSpellCastEvent>(&spellCastSink);
		holder->AddEventSink<RE::TESDeathEvent>(&deathSink);
		holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&magicApplySink);
		logger::debug("SnowDeformation: spell cast, death and magic-apply sinks registered");
	}
}

void SnowDeformation::ConsiderActorAuras(RE::Actor* a_actor, CloakState& a_cloak, float a_deltaTime)
{
	if (spellEmitters.size() >= kSpellEmitterCeiling || !a_actor)
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
	const float baseReach = arcs ? settings.ShockCloakRadius :
	                               (MarkForElement(a_cloak.element) == SpellMark::Crust ? settings.CrustRadius :
																						  settings.CloakRadius);
	// A body that has fallen is no longer carrying its aura around: it is
	// lying IN the snow rather than passing over it, so the mark drops to
	// ground level and widens, and it has nowhere left to sweep.
	const bool burning = a_cloak.burnRemaining > 0.0f;
	// An area spell states its own reach; a cloak names none and takes the
	// per-element setting. The scale is the innate auras' own knob - an
	// atronach's whole body is the source, where a cloak wraps one mage.
	const float cloakReach = (a_cloak.reachOverride > 0.0f ? a_cloak.reachOverride : std::max(baseReach, 1.0f)) *
	                         std::max(a_cloak.reachScale, 0.01f) * (burning ? kDeathAuraReach : 1.0f);
	if (!GroundMark(position.z + (burning ? 0.0f : kCloakCentreHeight) - groundZ, cloakReach, heightFade, radius))
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
	// A burning body does not sweep. Its ragdoll still drifts a little as it
	// settles, and a capsule from that would comb a line across the snow that
	// nothing actually travelled.
	if (!burning)
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

		// Opened as a BLAST rather than pushed straight out as an emitter. An
		// arc off a cloak is a discharge exactly as a bolt's strike is, so it
		// belongs on the same path - which is also what gives it the ramp, so
		// the snow is seen to give way instead of the pock simply existing.
		// One route now serves cloaks, storm atronachs and electrified bodies.
		if (activeBlasts.size() >= kMaxSpellEmitters)
			return;
		ActiveBlast arc{};
		arc.position = strike;
		arc.radius = radius;
		arc.strength = strength;
		// A pit is thrown, not melted into: nothing to integrate.
		arc.rate = 0.0f;
		arc.remaining = std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold);
		arc.pitScale = cloakReach * std::max(settings.ShockCloakStrikeScale, 0.05f) /
		               std::max(settings.PitRadius, 4.0f);
		arc.element = a_cloak.element;
		arc.mark = SpellMark::Pit;
		activeBlasts.push_back(arc);
		return;
	}

	emitter.position = current;
	emitter.previous = previous;
	emitter.radius = radius;
	emitter.strength = strength;
	emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * a_cloak.rateScale * heightFade;
	emitter.rateScale = a_cloak.rateScale * heightFade;
	spellEmitters.push_back(emitter);
}

// The aura an actor was BORN with, off its own records.
//
// Step 7's cast sink cannot see this one: nothing casts it. What it needs is
// not a race list - the aura is a real SpellItem on the race's spell list, and
// its cloak effect carries the same four axes every other detector classifies
// through. Verified against Skyrim.esm: AbFlameAtronach holds AbAtronachCloakFire
// with archetype Cloak, resist variable ResistFire and magnitude 10, and the
// frost and storm races hold the matching pair. So a modded atronach works for
// the same reason a modded Flames clone does.
//
// The read is of FORMS only - a race and a base object, both static for the
// run - never of a live actor's effect list, which is the thing that crashed
// three times in Step 7.
bool SnowDeformation::AuraFromSpellList(const RE::TESSpellList* a_list, InnateAuraRecord& a_out)
{
	const auto* effects = a_list ? a_list->actorEffects : nullptr;
	if (!effects || !effects->spells)
		return false;

	// An ELEMENTAL AFFINITY, gathered as we go: near-total immunity to one
	// element paired with a weakness to another is the engine's own way of
	// saying a creature is MADE of that element. It is the fallback for the
	// ones that radiate without carrying a cloak - an ice wraith is as much a
	// thing of frost as an atronach is, and says so in its records, but it has
	// no cloak effect for the walk below to find.
	//
	// Both halves are needed. Immunity alone catches the Dwarven automatons,
	// which are authored at 100 frost resistance and are machines rather than
	// ice; none of them carries a weakness, so the pair separates them cleanly.
	float immunity[4] = {};
	bool weakness[4] = {};
	auto elementOfAV = [](RE::ActorValue a_av) {
		switch (a_av) {
		case RE::ActorValue::kResistFire:
			return SpellElement::Fire;
		case RE::ActorValue::kResistFrost:
			return SpellElement::Frost;
		case RE::ActorValue::kResistShock:
			return SpellElement::Shock;
		default:
			return SpellElement::None;
		}
	};

	for (uint32_t i = 0; i < effects->numSpells; i++) {
		const RE::SpellItem* spell = effects->spells[i];
		if (!spell)
			continue;
		for (const auto* item : spell->effects) {
			const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
			if (!base)
				continue;

			// A CLOAK wins outright, and carries its own strength and reach.
			// The same gate the cast sink applies, minus its self-area case:
			// an innate ability declares no duration to make it cloak-like, so
			// only a true cloak archetype qualifies here.
			if (base->data.delivery == RE::MagicSystem::Delivery::kSelf &&
				base->data.archetype == RE::EffectSetting::Archetype::kCloak) {
				const SpellElement element = ClassifyElement(base);
				if (element != SpellElement::None) {
					a_out.element = element;
					a_out.rateScale = std::clamp(item->effectItem.magnitude / kSpellReferenceMagnitude,
						kSpellMagnitudeMin, kSpellMagnitudeMax);
					// Feet, as everywhere else. Vanilla atronachs name no area
					// at all and fall through to the per-school reach.
					a_out.reachOverride = item->effectItem.area > 0 ?
					                          std::clamp(static_cast<float>(item->effectItem.area) * kSelfAreaToUnits,
												  kSelfAreaReachMin, kSelfAreaReachMax) :
					                          0.0f;
					return true;
				}
			}

			const SpellElement affinity = elementOfAV(base->data.primaryAV);
			if (affinity == SpellElement::None)
				continue;
			const size_t slot = static_cast<size_t>(affinity);
			// Resistance and weakness are both positive ValueMods on a resist
			// actor value, so the sign is not what tells them apart - the
			// detrimental flag is.
			if (base->IsDetrimental())
				weakness[slot] = true;
			else
				immunity[slot] = std::max(immunity[slot], item->effectItem.magnitude);
		}
	}

	// Strongest immunity wins, and something else must be a weakness.
	SpellElement made = SpellElement::None;
	float best = kElementalImmunity;
	for (size_t slot = 1; slot < 4; slot++)
		if (immunity[slot] >= best) {
			bool weakElsewhere = false;
			for (size_t other = 1; other < 4; other++)
				weakElsewhere = weakElsewhere || (other != slot && weakness[other]);
			if (weakElsewhere) {
				best = immunity[slot];
				made = static_cast<SpellElement>(slot);
			}
		}
	if (made == SpellElement::None)
		return false;

	a_out.element = made;
	// A resistance PERCENTAGE says nothing about how hard the creature works
	// the snow, so it must not be read as one: the school's own rate stands.
	a_out.rateScale = 1.0f;
	a_out.reachOverride = 0.0f;
	return true;
}

bool SnowDeformation::AuraFromActorRecords(RE::Actor* a_actor, InnateAuraRecord& a_out)
{
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	if (!race)
		return false;
	// The RACE only, and deliberately.
	//
	// A race's spell list is abilities: things that are always on. An NPC's is
	// a LOADOUT - the spells that actor can cast - and the two are the same
	// field with opposite meanings. Nothing in the record separates them:
	// every atronach ability and every cloak an NPC merely knows is authored
	// kSpell, so spell type cannot be the discriminator, and 193 vanilla NPCs
	// carry a cloak they have not cast.
	//
	// Reading the NPC list also poisoned the cache below, which is keyed by
	// RACE: one Ice Warlock made every Nord in the game radiate frost, one
	// Storm Warlock every High Elf and Breton, and GuardWinterholdCollege -
	// authored on FoxRace - every fox.
	//
	// A mage that ACTUALLY casts a cloak still marks the ground. That is the
	// cast sink's job and has been since Step 7; this path is only for the
	// ones that radiate without ever casting.
	return AuraFromSpellList(race, a_out);
}

const SnowDeformation::InnateAuraRecord* SnowDeformation::ResolveInnateAura(RE::Actor* a_actor)
{
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	if (!race)
		return nullptr;

	// Cached per RACE form: races do not change while the game runs, and the
	// alternative is walking two spell lists for every actor every frame.
	// A miss is cached too, so a wolf costs one hash lookup.
	if (auto it = innateAuraByRace.find(race->formID); it != innateAuraByRace.end())
		return it->second.element != SpellElement::None ? &it->second : nullptr;

	InnateAuraRecord record{};
	AuraFromActorRecords(a_actor, record);

	if (innateAuraByRace.size() > 512)
		innateAuraByRace.clear();
	auto& stored = innateAuraByRace[race->formID] = record;
	return stored.element != SpellElement::None ? &stored : nullptr;
}

void SnowDeformation::OpenInnateDeathBlast(CloakState& a_state, const RE::NiPoint3& a_position)
{
	a_state.blasted = true;

	// A setting rather than a reading: an atronach's death explosion is spawned
	// by script and arrives as neither a projectile nor a reference, so there is
	// no live record to measure. The defaults are seeded from what the game
	// authors for each one.
	float authored = settings.AtronachFireDeathRadius;
	if (a_state.element == SpellElement::Frost)
		authored = settings.AtronachFrostDeathRadius;
	else if (a_state.element == SpellElement::Shock)
		authored = settings.AtronachShockDeathRadius;

	// Fire is the only one that keeps burning: a frost atronach shatters and a
	// storm one earths itself, and both are over the moment they land. A body
	// that vanished outright has nothing to burn either way - the caller drops
	// it immediately after.
	a_state.burnRemaining = a_state.element == SpellElement::Fire ?
	                            std::max(settings.AtronachFireBurnSeconds, 0.0f) :
	                            0.0f;

	auto* tes = RE::TES::GetSingleton();
	float groundZ = a_position.z;
	if (tes)
		tes->GetLandHeight(a_position, groundZ);
	const float scaled = authored * std::max(settings.BlastRadiusScale, 0.0f);
	float strength = 0.0f;
	float radius = 0.0f;
	if (!GroundMark(a_position.z - groundZ, scaled, strength, radius))
		return;
	if (strength < kMinSpellStrength || activeBlasts.size() >= kMaxSpellEmitters)
		return;

	ActiveBlast opened{};
	opened.position = { a_position.x, a_position.y };
	opened.radius = radius;
	opened.strength = strength;
	opened.rate = kAtronachDeathRate;
	opened.remaining = std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold);
	opened.element = a_state.element;
	opened.mark = MarkForElement(a_state.element);
	opened.pitScale = std::clamp(authored / kPitReferenceRadius, kPitScaleMin, kPitScaleMax);
	activeBlasts.push_back(opened);
}

void SnowDeformation::ConsiderCorpseEffect(RE::Actor* a_actor, float a_deltaTime)
{
	if (!settings.CorpseElementalMarks || !a_actor)
		return;
	const uint32_t formID = a_actor->formID;

	if (auto it = corpseEffects.find(formID); it != corpseEffects.end()) {
		// Raised again, or the effect has burnt out.
		if (!a_actor->IsDead() || it->second.burnRemaining <= 0.0f) {
			corpseEffects.erase(it);
			return;
		}
		spellStats.corpses++;
		ConsiderActorAuras(a_actor, it->second, a_deltaTime);
		it->second.burnRemaining -= a_deltaTime;
		return;
	}

	// Starts once, on the first frame the body is seen dead with a live memory
	// of what hit it. The corpse itself is never asked what it is doing.
	if (!a_actor->IsDead())
		return;
	auto hit = lastElementalHit.find(formID);
	if (hit == lastElementalHit.end() || hit->second.element == SpellElement::None)
		return;

	// Sized to the body. Nothing else in the feature did this - the reach was a
	// flat per-school number, so a mudcrab and a mammoth marked the same circle.
	// The bounding sphere of the actor's own 3D is what says how much snow it
	// is actually lying in, and a sprawled ragdoll reading larger than it did
	// standing up is correct rather than a fault.
	float sizeScale = 1.0f;
	if (auto* root = a_actor->Get3D(false))
		sizeScale = std::clamp(root->worldBound.radius / kCorpseReferenceBound,
			kCorpseSizeMin, kCorpseSizeMax);

	CloakState state{};
	state.actor = a_actor->GetHandle();
	state.element = hit->second.element;
	state.rateScale = 1.0f;
	state.reachScale = kCorpseReachScale * sizeScale;
	state.burnRemaining = std::max(settings.CorpseEffectSeconds, 0.0f);
	if (corpseEffects.size() > 256)
		corpseEffects.clear();
	corpseEffects[formID] = state;
	lastElementalHit.erase(hit);
}

void SnowDeformation::GatherActorMarks(float a_deltaTime, const RE::NiPoint3& a_cameraPosition, float a_cullRadius)
{
	// What struck whom, aged down. Drained before the sweep so a hit and the
	// death it caused can land on the same frame.
	{
		std::vector<QueuedHit> hits;
		{
			std::scoped_lock lock(queuedCastLock);
			hits.swap(queuedHits);
		}
		for (const auto& hit : hits) {
			if (lastElementalHit.size() > 512 && !lastElementalHit.contains(hit.formID))
				lastElementalHit.clear();
			lastElementalHit[hit.formID] = { hit.element, kElementalMemory };
		}
		for (auto it = lastElementalHit.begin(); it != lastElementalHit.end();) {
			it->second.remaining -= a_deltaTime;
			it = it->second.remaining > 0.0f ? std::next(it) : lastElementalHit.erase(it);
		}
	}

	// Deaths, from the sink. This is the ONLY route that reliably fires for an
	// atronach: it is unsummoned rather than left as a corpse, so by the time
	// any sweep looks it is already out of the high-process list, out of its
	// 3D, or both, and its handle stays resolvable long past the window that
	// tells a death from the player walking away.
	{
		std::vector<QueuedDeath> deaths;
		{
			std::scoped_lock lock(queuedCastLock);
			deaths.swap(queuedDeaths);
		}
		for (const auto& death : deaths) {
			auto& state = innateAuras[death.formID];
			if (state.element == SpellElement::None)
				state.element = death.element;
			if (!state.blasted) {
				const size_t before = activeBlasts.size();
				OpenInnateDeathBlast(state, death.position);
				if (activeBlasts.size() > before)
					spellStats.deathBlasts++;
			}
			state.lastPosition = death.position;
		}
	}

	// Aged first, zeroed by an observation below: after the sweep, anything
	// still carrying time has not been seen this frame.
	for (auto& entry : innateAuras)
		entry.second.unseenFor += a_deltaTime;

	auto consider = [&](RE::ActorHandle a_handle) {
		auto actor = a_handle.get();
		if (!actor || !actor->Is3DLoaded())
			return;
		if (a_cameraPosition.GetSquaredDistance(actor->GetPosition()) > a_cullRadius * a_cullRadius)
			return;

		// Runs for EVERY actor, not only the ones with an aura of their own:
		// an ordinary bandit killed by fire is exactly the case this is for.
		ConsiderCorpseEffect(actor.get(), a_deltaTime);

		const auto* record = ResolveInnateAura(actor.get());
		if (!record)
			return;

		const uint32_t formID = actor->formID;
		if (innateAuras.size() > 256 && !innateAuras.contains(formID))
			innateAuras.clear();
		auto& state = innateAuras[formID];
		if (state.element == SpellElement::None) {
			state.actor = actor->GetHandle();
			state.element = record->element;
			state.rateScale = record->rateScale;
			state.reachOverride = record->reachOverride;
			state.innate = true;
		}
		// Reach scale is read fresh so the menu sliders move live.
		switch (state.element) {
		case SpellElement::Frost:
			state.reachScale = settings.AtronachFrostReach;
			break;
		case SpellElement::Shock:
			state.reachScale = settings.AtronachShockReach;
			break;
		default:
			state.reachScale = settings.AtronachFireReach;
			break;
		}

		state.lastPosition = actor->GetPosition();
		state.unseenFor = 0.0f;

		if (!actor->IsDead()) {
			// Reanimated or resurrected: it can die - and burst - again.
			state.blasted = false;
			state.burnRemaining = 0.0f;
			spellStats.innate++;
			ConsiderActorAuras(actor.get(), state, a_deltaTime);
			return;
		}

		// Died and left a body: blast now, then let it burn out where it fell.
		// Its own carve stamps handle the dent - it landed, so it is touching -
		// and the burn is the heat on top.
		if (!state.blasted)
			OpenInnateDeathBlast(state, state.lastPosition);
		if (state.burnRemaining > 0.0f) {
			spellStats.burning++;
			ConsiderActorAuras(actor.get(), state, a_deltaTime);
			state.burnRemaining -= a_deltaTime;
		}
	};

	if (auto* player = RE::PlayerCharacter::GetSingleton())
		consider(player->GetHandle());
	if (auto* processLists = RE::ProcessLists::GetSingleton())
		for (auto& handle : processLists->highActorHandles)
			consider(handle);

	// Died and left NOTHING. An atronach is unsummoned on death rather than
	// left as a corpse, so the loop above never sees a dead one at all - it is
	// simply absent on the next frame, which is why the fire atronach's burst
	// marked nothing. This is the same shape as PendingBlast: remember what an
	// actor would leave while it is still there, and throw it when it goes.
	//
	// A handle that no longer resolves is the 'gone' test, exactly as a
	// projectile missing from its manager is. The recency window is what
	// separates a death from the player walking away: a real death is seen on
	// the frame before, while an unloading cell has left the actor unobserved
	// for a long time first.
	for (auto it = innateAuras.begin(); it != innateAuras.end();) {
		auto& state = it->second;
		if (state.unseenFor <= 0.0f || state.actor.get()) {
			// Seen this frame, or still loaded and merely out of range.
			++it;
			continue;
		}
		if (!state.blasted && state.unseenFor < kInnateDeathWindow)
			OpenInnateDeathBlast(state, state.lastPosition);
		// Nothing left to ride: a body that vanished leaves no corpse to burn.
		it = innateAuras.erase(it);
	}
}

void SnowDeformation::GatherDashGouges(float a_deltaTime, const RE::NiPoint3& a_cameraPosition, float a_cullRadius)
{
	{
		std::vector<DashWatch> arrived;
		{
			std::scoped_lock lock(queuedCastLock);
			arrived.swap(queuedDashes);
		}
		for (auto& watch : arrived) {
			// A re-cast restarts the window rather than stacking a watch.
			auto existing = std::find_if(dashWatches.begin(), dashWatches.end(),
				[&](const DashWatch& a_other) { return a_other.actor == watch.actor; });
			if (existing != dashWatches.end())
				existing->remaining = watch.remaining;
			else
				dashWatches.push_back(watch);
		}
	}

	for (auto it = dashWatches.begin(); it != dashWatches.end();) {
		it->remaining -= a_deltaTime;
		auto actor = it->remaining > 0.0f ? it->actor.get() : RE::NiPointer<RE::Actor>();
		if (!actor) {
			it = dashWatches.erase(it);
			continue;
		}
		spellStats.dashWatches++;

		const RE::NiPoint3 position = actor->GetPosition();
		const float2 current{ position.x, position.y };
		const float2 last = it->previous;
		const bool had = it->hasPrevious;
		it->previous = current;
		it->hasPrevious = true;
		++it;

		if (!had || spellEmitters.size() >= kSpellEmitterCeiling)
			continue;
		if (a_cameraPosition.GetSquaredDistance(position) > a_cullRadius * a_cullRadius)
			continue;

		// Airborne dashers cut nothing. The controller's own word first, since
		// it is the engine's, and the height above land behind it for the case
		// of dashing along a ledge with the controller still reporting ground.
		if (auto* controller = actor->GetCharController();
			controller && (controller->context.currentState == RE::hkpCharacterStateType::kInAir ||
							  controller->context.currentState == RE::hkpCharacterStateType::kFlying))
			continue;
		if (auto* tes = RE::TES::GetSingleton()) {
			float landZ = position.z;
			tes->GetLandHeight(position, landZ);
			if (position.z - landZ > kDashGroundBand)
				continue;
		}

		const float dx = current.x - last.x;
		const float dy = current.y - last.y;
		const float travelled = std::sqrt(dx * dx + dy * dy);
		if (travelled > kDashBreak)
			continue;
		// THE test. Running does not reach this, being hurled does.
		if (travelled / std::max(a_deltaTime, 1e-4f) < kDashSpeedGate)
			continue;

		// Sized to the dasher, so a dragon cuts a wider furrow than a man.
		float halfWidth = kDashWidthMin;
		if (auto* root = actor->Get3D(false))
			halfWidth = root->worldBound.radius * kDashWidthOfBound;
		halfWidth = std::clamp(halfWidth * std::max(settings.DashGougeScale, 0.05f),
			kDashWidthMin, kDashWidthMax);

		SpellEmitter emitter{};
		// A capsule, which is what the map has always drawn a swept body as -
		// the furrow is the dasher's own path, not a shape thrown ahead of it.
		emitter.position = current;
		emitter.previous = last;
		emitter.radius = halfWidth;
		emitter.strength = 1.0f;
		emitter.element = SpellElement::Force;
		emitter.mark = SpellMark::Carve;
		// Scooped, not cut. A body hurled through snow leaves a furrow with
		// sloped sides; the walled slot a trench profile gives is what a boot
		// pressing down makes, and this is nothing like that.
		emitter.bowl = true;
		spellEmitters.push_back(emitter);
		spellStats.dashGouges++;
	}
}

void SnowDeformation::OpenShoutCone(const QueuedCone& a_cone, RE::TES* a_tes)
{
	if (!a_tes || a_cone.element == SpellElement::None || a_cone.length <= 1.0f)
		return;
	if (activeBlasts.size() >= kMaxSpellEmitters)
		return;

	// ONE wedge. It was ten discs along the axis, and that could never work:
	// a stamp holds full depth across only the inner tenth of its radius and
	// tapers over all the rest, so overlapping discs read as a row of craters
	// with shallow gaps rather than as a widening mouth. The shape has to be
	// the shape.
	const float halfSpread = std::clamp(settings.ShoutConeSpread, 1.0f, 170.0f) * 0.5f;
	const float farHalfWidth = std::max(a_cone.length * std::tan(halfSpread * 0.0174532925f), 8.0f);

	// Not named 'far': that is a legacy Windows macro and expands to nothing,
	// which turns the declaration below into a syntax error a long way from here.
	const RE::NiPoint3 tip = a_cone.position + a_cone.direction * a_cone.length;
	// The apex stands off in front of the shouter, so the snow they are
	// standing in is left alone and the wedge opens from the mouth outward.
	const float standoff = std::min(kShoutMouthOffset, a_cone.length * 0.5f);
	const RE::NiPoint3 apex = a_cone.position + a_cone.direction * standoff;
	// Measured at the middle of the wedge rather than at either end: the apex
	// is beside the shouter, and the far end may be over a cliff.
	const RE::NiPoint3 mid = a_cone.position + a_cone.direction * (a_cone.length * 0.5f);
	float groundZ = mid.z;
	a_tes->GetLandHeight(mid, groundZ);
	float strength = 0.0f;
	float reachOut = 0.0f;
	if (!GroundMark(mid.z - groundZ, farHalfWidth, strength, reachOut))
		return;
	strength *= a_cone.strength;
	if (strength < kMinSpellStrength)
		return;

	const float travel = std::max(a_cone.length - standoff, 1.0f);
	const float speed = std::max(a_cone.speed, 1.0f);

	ActiveBlast opened{};
	opened.cone = true;
	opened.apex = { apex.x, apex.y };
	opened.position = { tip.x, tip.y };
	opened.coneDir = { a_cone.direction.x, a_cone.direction.y };
	opened.coneLength = travel;
	opened.coneSpeed = speed;
	opened.radius = farHalfWidth;
	opened.strength = strength;
	opened.rate = kExplosionRate;
	// Held for as long as the front takes to run out, and then a moment more.
	opened.remaining = travel / speed + std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold);
	opened.element = a_cone.element;
	opened.mark = MarkForElement(a_cone.element);
	opened.pitScale = 1.0f;
	activeBlasts.push_back(opened);
	spellStats.shouts++;
	spellStats.shoutDiscs++;
}

void SnowDeformation::OpenProjectileBlast(const PendingBlast& a_blast, RE::TES* a_tes)
{
	if (a_blast.element == SpellElement::None || !a_tes)
		return;

	// A bolt dies between frames, so the last sighting sits short of the
	// impact - the faster it flew, the shorter. Carrying the flight on to the
	// ground puts the crater where it struck instead of where it was last
	// drawn, which is the same reason a fire stream marks its contact rather
	// than its emitter.
	float2 markPosition = { a_blast.position.x, a_blast.position.y };
	float strength = 0.0f;
	float radius = 0.0f;

	RE::NiPoint3 landing{};
	const float reach = std::max(a_blast.landingReach, kBlastLandingReach);
	const bool landed = TraceGroundContact(a_tes, a_blast.position, a_blast.direction, reach, landing) &&
	                    a_blast.position.GetDistance(landing) <= reach;
	if (landed) {
		markPosition = { landing.x, landing.y };
		strength = 1.0f;
		radius = a_blast.radius;
	} else if (!GroundMark(a_blast.heightAboveLand, a_blast.radius, strength, radius)) {
		// Stopped by an actor or a wall well above the snow: the ground below
		// takes the weaker, broader mark rather than a full crater.
		return;
	}
	if (strength < kMinSpellStrength)
		return;
	spellStats.detonations++;
	if (activeBlasts.size() >= kMaxSpellEmitters)
		return;

	spellStats.lastStrength = strength;
	spellStats.lastRadius = radius;

	ActiveBlast opened{};
	opened.position = markPosition;
	// Carried from the AUTHORED radius rather than the fire-scaled one.
	opened.pitScale = a_blast.pitScale;
	opened.radius = radius;
	opened.strength = strength;
	opened.rate = kExplosionRate;
	opened.remaining = std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold);
	opened.element = a_blast.element;
	opened.mark = MarkForElement(a_blast.element);
	activeBlasts.push_back(opened);
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
		innateAuras.clear();
		corpseEffects.clear();
		lastElementalHit.clear();
		dashWatches.clear();
		{
			std::scoped_lock lock(queuedCastLock);
			queuedDeaths.clear();
			queuedHits.clear();
			queuedCones.clear();
			queuedDashes.clear();
		}
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

		// Innate auras run beside the cast ones, through the same emitter and
		// the same melt path - only their lifecycle differs, so they keep
		// their own map rather than a flag in that one.
		GatherActorMarks(cloakDelta, cameraPosition, cullRadius);
		GatherDashGouges(cloakDelta, cameraPosition, cullRadius);
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
	// Everything the manager still lists, destroyed or not. Separate from
	// stillAlive on purpose: a bolt that has struck is flagged destroyed at
	// once but stays listed while its beam is drawn, so the already-marked
	// latch has to be held against PRESENCE. Held against stillAlive it was
	// dropped on the same frame it was set, and the mark repeated every frame
	// until the beam expired.
	std::unordered_set<uint32_t> presentIDs;
	stillAlive.reserve(live.size());
	presentIDs.reserve(live.size());
	for (auto& projectile : live) {
		if (!projectile)
			continue;
		presentIDs.insert(projectile->formID);
		if (projectile->GetProjectileRuntimeData().flags.any(RE::Projectile::Flags::kDestroyed))
			continue;
		stillAlive.insert(projectile->formID);
	}

	for (auto& projectile : live) {
		if (spellEmitters.size() >= kSpellEmitterCeiling)
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

		// A SLOW shove leaves the ground it crosses. Deliberately narrow: only
		// the slow ones are given a force element here, so a shockwave keeps
		// being rejected and its cone stays the one mark it makes.
		bool tracks = false;
		if (element == SpellElement::None) {
			float shoveForce = 0.0f;
			if (auto* baseForm = projectile->GetBaseObject())
				if (auto* projectileBase = baseForm->As<RE::BGSProjectile>())
					if (SpellShoves(runtime.spell, shoveForce) &&
						IsTravellingShove(projectileBase, shoveForce)) {
						element = SpellElement::Force;
						tracks = true;
					}
		}

		if (element == SpellElement::None) {
			spellStats.rejectedElement++;
			continue;
		}

		const RE::NiPoint3 position = projectile->GetPosition();
		if (cameraPosition.GetSquaredDistance(position) > cullRadius * cullRadius)
			continue;

		if (tracks) {
			// The ground under it, swept from where it was - the same capsule
			// every moving source in this feature draws, and it follows the
			// real flight, so a cyclone rounding a slope or stopping at a wall
			// marks where it actually went rather than where it was aimed.
			float trackGroundZ = position.z;
			tes->GetLandHeight(position, trackGroundZ);
			float trackStrength = 0.0f;
			float trackRadius = 0.0f;
			if (!GroundMark(position.z - trackGroundZ, std::max(settings.ForceTrackWidth, 4.0f),
					trackStrength, trackRadius))
				continue;
			if (trackStrength < kMinSpellStrength || spellEmitters.size() >= kSpellEmitterCeiling)
				continue;

			const float2 currentTrack{ position.x, position.y };
			float2 previousTrack = currentTrack;
			if (auto it = spellPrevPositions.find(projectile->formID); it != spellPrevPositions.end()) {
				const float dx = currentTrack.x - it->second.x;
				const float dy = currentTrack.y - it->second.y;
				if (dx * dx + dy * dy < kSpellTrailBreak * kSpellTrailBreak)
					previousTrack = it->second;
			}
			currentPositions[projectile->formID] = currentTrack;

			SpellEmitter emitter{};
			emitter.position = currentTrack;
			emitter.previous = previousTrack;
			emitter.radius = trackRadius;
			// Scoured, not excavated. Depth is what the berm field reads, so
			// this is the same dial that decides whether the rim piles into
			// the two parallel ridges that make any wide cut read as a trench.
			emitter.strength = trackStrength * std::clamp(settings.ForceTrackDepth, 0.0f, 1.0f);
			emitter.element = SpellElement::Force;
			emitter.mark = SpellMark::Carve;
			// A vortex scours a rounded gouge; nothing about it presses a slot.
			emitter.bowl = true;
			spellEmitters.push_back(emitter);
			spellStats.forceTracks++;
			continue;
		}

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

		// A shout that already ploughs a wedge must not ALSO mark through the
		// projectile it throws. Aim a breath at your own feet and the wedge
		// covers the ground anyway, while the projectile's own crater punched a
		// little hole in the middle of it. Only the blast and the corridor are
		// silenced: a held stream still marks where it sweeps, which a single
		// wedge laid at the moment of casting cannot follow.
		const bool wedged = ShoutLaysCone(runtime.spell);

		{
			if (!blast && !strikes)
				spellStats.rejectedNoBlast++;
			if (!wedged && (strikes || blast)) {
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

				// A projectile that has already struck must mark NOW. Waiting
				// for it to leave the manager is right for something still in
				// flight, and wrong for a bolt that resolved the instant it
				// was cast: a hitscan projectile lingers for as long as its
				// beam is drawn, so the crater arrived most of a second after
				// the target was hit. Its own impact list says it has landed,
				// and the hitscan flag says it landed immediately.
				const bool struckAlready = strikes && (hitscan || !runtime.impacts.empty());
				if (struckAlready) {
					if (!hitscanBlasted.contains(projectile->formID)) {
						hitscanBlasted.insert(projectile->formID);
						OpenProjectileBlast(pending, tes);
					}
					// Never queued for the disappearance route, so it cannot
					// mark a second time when the beam finally expires.
				} else {
					projectileBlasts[projectile->formID] = pending;
				}
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
		if (strikes && !wedged) {
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
				if (cut >= kMinSpellStrength && spellEmitters.size() < kSpellEmitterCeiling) {
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
		emitter.rateScale = RateScaleOf(costliest) * strength;
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
		OpenProjectileBlast(blast, tes);
	}

	// Shouts, laid down as a wedge of discs. Drained before the self-centred
	// blasts because a shout is the bigger claim on the blast budget.
	{
		std::vector<QueuedCone> cones;
		{
			std::scoped_lock lock(queuedCastLock);
			cones.swap(queuedCones);
		}
		for (const auto& cone : cones)
			OpenShoutCone(cone, tes);
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
			opened.remaining = std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold);
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
			if (spellEmitters.size() < kSpellEmitterCeiling) {
				// A mark forms over a moment rather than at once. Melt already
				// ramps, because it integrates - a PIT does not, since it
				// follows carve, so a discharge would otherwise simply exist
				// on the frame it landed with nothing seen to move.
				//
				// A CONE needs none of that: it has somewhere to be going. Its
				// front runs out from the mouth at the shout's own speed, so
				// the ground it has already passed is at full depth while the
				// ground ahead has not been touched - which is what a shout
				// travelling looks like, and what a strength ramp cannot say.
				const float ramp = it->cone ?
				                       1.0f :
				                       std::clamp(it->age / std::max(kBlastRampSeconds, 1e-3f), 0.0f, 1.0f);
				SpellEmitter emitter{};
				emitter.position = it->position;
				// A wedge sweeps from its apex; everything else marks where it
				// stands and has no second point to give.
				emitter.previous = it->cone ? it->apex : it->position;
				emitter.cone = it->cone;
				emitter.radius = it->radius;
				if (it->cone) {
					// The angle is what stays fixed as the front advances, so
					// the half-width grows with the reach rather than standing
					// at its final size over a stub of a wedge.
					// Measured from the age this frame ENDS at, so the first
					// frame lays a sliver rather than nothing.
					const float extent = std::clamp(
						it->coneSpeed * (it->age + deltaTime) / std::max(it->coneLength, 1e-3f),
						0.0f, 1.0f);
					emitter.position = { it->apex.x + it->coneDir.x * it->coneLength * extent,
						it->apex.y + it->coneDir.y * it->coneLength * extent };
					emitter.radius = it->radius * extent;
				}
				emitter.strength = it->strength * ramp;
				emitter.rate = it->rate;
				emitter.element = it->element;
				emitter.mark = it->mark;
				emitter.pitScale = it->pitScale;
				// A blast has one moment to set its crust in, so it cannot use
				// the rate meant for something standing over a spot for
				// seconds. Expressed against the setting so the setting still
				// governs every sustained source, exactly as before.
				emitter.rateScale = it->mark == SpellMark::Crust ?
				                        kBlastCrustRate / std::max(settings.CrustRate, 0.01f) :
				                        1.0f;
				spellEmitters.push_back(emitter);
			}
			it->age += deltaTime;
			it->remaining -= deltaTime;
			it = it->remaining > 0.0f ? it + 1 : activeBlasts.erase(it);
		}
	}

	// Projectile form ids are recycled, so a fired id must not stay latched.
	std::erase_if(hitscanBlasted, [&](uint32_t a_id) { return !presentIDs.contains(a_id); });

	spellPrevPositions = std::move(currentPositions);
	spellTrailPrev = std::move(currentTrailPositions);
	spellAuraPrev = std::move(currentAuraPositions);
	spellStats.emitters = static_cast<uint>(spellEmitters.size());
	spellStats.armed = static_cast<uint>(projectileBlasts.size());
}
