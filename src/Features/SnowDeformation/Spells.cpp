// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/Game.h"

static constexpr float kSpellStreamReachDefault = 900.0f;
static constexpr float kSpellReachMin = 200.0f;
static constexpr float kSpellReachMax = 4000.0f;
static constexpr int kSpellTraceSteps = 20;
static constexpr float kSpellContactRadius = 70.0f;
static constexpr float kSpellContactBand = 48.0f;
static constexpr float kSpellRadiantBand = 300.0f;
static constexpr float kSpellRadiantRadiusPerUnit = 0.45f;
static constexpr float kSpellRadiantScale = 0.55f;
static constexpr float kHazardPieceRadius = 6.0f;
static constexpr float kLiftFraction = 0.5f;
static constexpr auto kProjectileTriggered = RE::Projectile::Flags::kUnk30;
static constexpr float kMaxLiftHeight = 60.0f;
static constexpr float kRuneClearance = 1.0f;
static constexpr float kRuneMinUpZ = 0.7f;
static constexpr float kHazardRadiusMin = 40.0f;
static constexpr float kHazardRadiusMax = 260.0f;
static constexpr float kExplosionRadiusMin = 50.0f;
static constexpr float kExplosionRadiusMax = 2400.0f;
static constexpr float kImpactRadiusDefault = 90.0f;
static constexpr float kExplosionRate = 4.0f;
static constexpr float kBlastDuration = 0.35f;
static constexpr float kBlastMinHold = 0.05f;
static constexpr float kSpellReferenceMagnitude = 8.0f;
static constexpr float kSpellMagnitudeMin = 0.35f;
static constexpr float kSpellMagnitudeMax = 3.0f;
static constexpr float kSpellTrailBreak = 512.0f;
static constexpr float kMinSpellStrength = 0.04f;
static constexpr float kMinSpellSpeed = 1.0f;
static constexpr float kTrailRadius = 35.0f;
static constexpr float kTrailRate = 100.0f;
static constexpr float kMinTrailDepth = 2.0f;
static constexpr float kCloakCentreHeight = 70.0f;
static constexpr float kCloakDefaultDuration = 60.0f;
static constexpr float kDeathAuraReach = 1.35f;
static constexpr float kCorpseReachScale = 0.5f;
static constexpr float kCorpseReferenceBound = 80.0f;
static constexpr float kCorpseSizeMin = 0.4f;
static constexpr float kCorpseSizeMax = 2.5f;
static constexpr float kAtronachDeathRate = 4.0f;
static constexpr float kAtronachDeathHold = 1.2f;
static constexpr float kFireDeathFuseSeconds = 3.0f;
static constexpr float kInnateDeathWindow = 0.5f;
static constexpr float kElementalImmunity = 90.0f;
static constexpr float kBlastRampSeconds = 0.09f;
static constexpr float kElementalMemory = 6.0f;
static constexpr float kShoutMouthOffset = 60.0f;
static constexpr float kShoutReferenceForce = 50.0f;
static constexpr float kShoutForceMin = 0.25f;
static constexpr float kShoutForceMax = 1.5f;
static constexpr float kShoutMaxPitchDeg = 40.0f;
static constexpr float kStaggerFraction = 1.0f;
static constexpr float kShoutCarrierForce = 50.0f;
static constexpr float kForceTrackSpeed = 800.0f;
static constexpr float kVortexForce = 200.0f;
static constexpr float kShoutDefaultRange = 1000.0f;
static constexpr float kShoutRangeMax = 12000.0f;
static constexpr float kShoutDefaultSpeed = 1400.0f;
static constexpr float kBlastCrustRate = 4.0f;
static constexpr float kDashWindow = 1.5f;
static constexpr float kDashSpeedGate = 800.0f;
static constexpr float kDashBreak = 700.0f;
static constexpr float kDashWidthOfBound = 0.22f;
static constexpr float kDashWidthMin = 10.0f;
static constexpr float kDashWidthMax = 220.0f;
static constexpr float kDashGroundBand = 45.0f;
static constexpr float kPitReferenceRadius = 90.0f;
static constexpr float kPitScaleMin = 0.8f;
static constexpr float kPitScaleMax = 2.2f;
static constexpr float kStreamPitScale = 0.65f;
static constexpr float kTrailPitScale = 0.5f;
static constexpr float kSelfAreaToUnits = 21.0f;
static constexpr float kSelfAreaReachMin = 60.0f;
static constexpr float kSelfAreaReachMax = 1400.0f;
static constexpr float kCloakStrikeInner = 0.45f;
static constexpr float kBlastTravelWindow = 0.05f;
static constexpr float kBlastLandingReach = 220.0f;
SnowDeformation::SpellElement SnowDeformation::ClassifyElement(const RE::EffectSetting* a_effect)
{
	if (!a_effect)
		return SpellElement::None;
	switch (a_effect->data.resistVariable) {
	case RE::ActorValue::kResistFire:
		return SpellElement::Fire;
	case RE::ActorValue::kResistFrost:
		return SpellElement::Frost;
	case RE::ActorValue::kResistShock:
		return SpellElement::Shock;
	default:
		break;
	}
	if (a_effect->HasKeywordString("MagicDamageFire"))
		return SpellElement::Fire;
	if (a_effect->HasKeywordString("MagicDamageFrost"))
		return SpellElement::Frost;
	if (a_effect->HasKeywordString("MagicDamageShock"))
		return SpellElement::Shock;
	return SpellElement::None;
}

SnowDeformation::SpellMark SnowDeformation::MarkForElement(SpellElement a_element)
{
	switch (a_element) {
	case SpellElement::Frost:
		return SpellMark::Crust;
	case SpellElement::Shock:
		return SpellMark::Pit;
	case SpellElement::Force:
		return SpellMark::Carve;
	default:
		return SpellMark::Melt;
	}
}

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
			const float t = std::clamp(previousGap / std::max(previousGap - gap, 1e-4f), 0.0f, 1.0f);
			a_contact = previous + (sample - previous) * t;
			return true;
		}
		previous = sample;
		previousGap = gap;
	}
	return false;
}

static bool TraceStreamObjectHit(RE::TES* a_tes, const RE::NiPoint3& a_origin,
	const RE::NiPoint3& a_direction, float a_reach, RE::NiPoint3& a_hit)
{
	const float toHavok = RE::bhkWorld::GetWorldScale();
	const RE::NiPoint3 end = a_origin + a_direction * a_reach;
	RE::bhkPickData pick{};
	pick.rayInput.from = RE::hkVector4(a_origin.x * toHavok, a_origin.y * toHavok, a_origin.z * toHavok, 0.0f);
	pick.rayInput.to = RE::hkVector4(end.x * toHavok, end.y * toHavok, end.z * toHavok, 0.0f);
	pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kProjectile);
	a_tes->Pick(pick);
	const RE::hkpCollidable* collidable = pick.rayOutput.rootCollidable;
	if (!collidable || collidable->GetCollisionLayer() == RE::COL_LAYER::kTerrain)
		return false;
	if (auto* ref = RE::TESHavokUtilities::FindCollidableRef(*collidable); ref && ref->As<RE::Actor>())
		return false;
	a_hit = a_origin + (end - a_origin) * pick.rayOutput.hitFraction;
	return true;
}

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

static float RateScaleOf(const RE::Effect* a_effect)
{
	if (!a_effect)
		return 1.0f;
	return std::clamp(a_effect->effectItem.magnitude / kSpellReferenceMagnitude,
		kSpellMagnitudeMin, kSpellMagnitudeMax);
}

float SnowDeformation::SnowLiftFor(const RE::NiPoint3& a_position, float a_fraction, float a_clearance, RE::TES* a_tes)
{
	float landZ = a_position.z;
	if (a_tes)
		a_tes->GetLandHeight(a_position, landZ);
	if (a_position.z - landZ > kElevatedStampCutoff)
		return 0.0f;
	const float depth = std::max(GetNominalSnowDepthAt(a_position.x, a_position.y, 0.0f), 0.0f) * GetAccumulationDepthScale();
	if (depth < 1.0f)
		return 0.0f;
	float surfaceZ = 0.0f;
	if (!SampleShellSurface(a_position.x, a_position.y, surfaceZ))
		surfaceZ = landZ + depth;
	const float lift = surfaceZ - (1.0f - std::clamp(a_fraction, 0.0f, 1.0f)) * depth + a_clearance - a_position.z;
	return lift >= 1.0f ? std::min(lift, kMaxLiftHeight) : 0.0f;
}

void SnowDeformation::LiftRefOntoSnow(RE::TESObjectREFR* a_ref, float a_lift, float a_minUpZ)
{
	if (!a_ref || a_lift < 1.0f)
		return;
	std::scoped_lock lock(spellLiftLock);
	if (spellLiftQueue.size() < 256)
		spellLiftQueue.push_back({ a_ref->CreateRefHandle(), a_lift, a_minUpZ });
}

void SnowDeformation::ApplySpellLifts()
{
	std::vector<PendingLift> pending;
	{
		std::scoped_lock lock(spellLiftLock);
		if (spellLiftQueue.empty())
			return;
		pending.swap(spellLiftQueue);
	}
	for (auto& entry : pending) {
		auto ref = entry.handle.get();
		if (!ref)
			continue;
		auto* root = ref->Get3D(false);
		if (!root) {
			if (++entry.waited < 120) {
				std::scoped_lock lock(spellLiftLock);
				spellLiftQueue.push_back(entry);
			}
			continue;
		}
		if (root->world.rotate.entry[2][2] < entry.minUpZ)
			continue;
		root->local.translate.z += entry.lift;
		RE::NiUpdateData data{};
		root->Update(data);
	}
}

void SnowDeformation::ConsiderRune(RE::Projectile* a_projectile, const RE::NiPoint3& a_camera, float a_cullRadius, RE::TES* a_tes)
{
	auto* base = a_projectile->GetBaseObject();
	auto* record = base ? base->As<RE::BGSProjectile>() : nullptr;
	if (!record || !record->data.flags.any(RE::BGSProjectileData::BGSProjectileFlags::kExplosionAltTrigger))
		return;
	const RE::NiPoint3 position = a_projectile->GetPosition();
	const float distSq = a_camera.GetSquaredDistance(position);
	if (distSq > a_cullRadius * a_cullRadius)
		return;
	const uint32_t formID = a_projectile->formID;

	if (settings.RuneDecalsOnSnow) {
		auto [it, fresh] = runeDecalPathCache.try_emplace(record);
		if (fresh && record->data.decalData) {
			if (const char* path = record->data.decalData->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
				std::string lower(path);
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return ch == '/' ? '\\' : char(std::tolower(ch)); });
				it->second = std::move(lower);
				logger::info("[SNOW DEFORMATION] rune record {:08X}: decal texture '{}'", record->formID, it->second);
			}
		}
		if (!it->second.empty())
			runeSitesScratch.push_back({ formID, position, it->second, distSq });
	}

	if (settings.LiftRunes && runeLifted.insert(formID).second) {
		const float lift = SnowLiftFor(position, 1.0f, kRuneClearance, a_tes);
		if (lift > 0.0f) {
			LiftRefOntoSnow(a_projectile, lift, kRuneMinUpZ);
			spellStats.lifted++;
		}
	}
}

void SnowDeformation::PublishRuneSites(const std::unordered_set<uint32_t>& a_present)
{
	std::erase_if(runeLifted, [&](uint32_t a_id) { return !a_present.contains(a_id); });
	std::sort(runeSitesScratch.begin(), runeSitesScratch.end(), [](const RuneSite& a, const RuneSite& b) { return a.distSq < b.distSq; });
	if (runeSitesScratch.size() > kRuneMaxTiles)
		runeSitesScratch.resize(kRuneMaxTiles);
	std::scoped_lock lock(runeLock);
	runeSites.swap(runeSitesScratch);
	runeSitesScratch.clear();
	runeSitesLive.store(static_cast<uint32_t>(runeSites.size()), std::memory_order_release);
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

	const RE::Effect* costliest = base->data.spell ? base->data.spell->GetCostliestEffectItem() : nullptr;
	const RE::EffectSetting* effect = costliest ? costliest->baseEffect : nullptr;
	const SpellElement element = ClassifyElement(effect);
	if (element == SpellElement::None)
		return;

	auto* tes = globals::game::tes;
	if (!tes)
		return;

	const RE::NiPoint3 position = a_ref->GetPosition();
	float groundZ = position.z;
	tes->GetLandHeight(position, groundZ);

	if (settings.LiftFrostEffects && element == SpellElement::Frost &&
		base->data.radius <= kHazardPieceRadius && !liftedRefs.contains(a_ref->formID)) {
		if (liftedRefs.size() > 512)
			liftedRefs.clear();
		liftedRefs.insert(a_ref->formID);
		const float lift = SnowLiftFor(position, kLiftFraction, 0.0f, tes);
		if (lift > 0.0f) {
			LiftRefOntoSnow(a_ref, lift);
			spellStats.lifted++;
		}
	}

	const float baseRadius = std::clamp(runtime.radius > 1.0f ? runtime.radius : base->data.radius,
		kHazardRadiusMin, kHazardRadiusMax);
	QueueObjectSpellMarkFor(position, position, baseRadius, MarkForElement(element), 1.0f, std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest),
		std::clamp(baseRadius / kPitReferenceRadius, kPitScaleMin, kPitScaleMax), 0u);

	float heightFade = 0.0f;
	float radius = 0.0f;
	if (!GroundMark(position.z - groundZ, baseRadius, heightFade, radius))
		return;
	if (heightFade < kMinSpellStrength)
		return;
	const float strength = 1.0f;
	spellStats.lastStrength = strength;
	spellStats.lastRadius = radius;

	const float hazardRamp = MarkForElement(element) == SpellMark::Pit ?
	                             std::clamp(runtime.age / std::max(kBlastRampSeconds, 1e-3f), 0.0f, 1.0f) :
	                             1.0f;

	SpellEmitter emitter{};
	emitter.position = { position.x, position.y };
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
	feature.propScanFrame = 0;

	if (feature.settings.EnableShoutCones)
		if (auto* spellItem = spell->As<RE::SpellItem>();
			spellItem && spellItem->data.spellType == RE::MagicSystem::SpellType::kVoicePower) {
			feature.ConsiderShout(spellItem, a_event->object.get());

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

		const bool selfArea = item->effectItem.area > 0 && item->effectItem.duration > 0;
		if (base->data.archetype == RE::EffectSetting::Archetype::kCloak || selfArea) {
			const SpellElement candidate = ClassifyElement(base);
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
	if (!a_event || a_event->dead || !a_event->actorDying || !feature.settings.EnableSpellIntegration)
		return RE::BSEventNotifyControl::kContinue;
	auto* actor = a_event->actorDying->As<RE::Actor>();
	if (!actor)
		return RE::BSEventNotifyControl::kContinue;

	InnateAuraRecord record{};
	if (!AuraFromActorRecords(actor, record))
		return RE::BSEventNotifyControl::kContinue;
	feature.spellStats.deathsSeen++;

	QueuedDeath queued{};
	queued.formID = actor->formID;
	queued.position = actor->GetPosition();
	queued.element = record.element;
	queued.actor = actor->GetHandle();
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

	auto* form = RE::TESForm::LookupByID(a_event->magicEffect);
	const SpellElement element = ClassifyElement(form ? form->As<RE::EffectSetting>() : nullptr);
	if (element == SpellElement::None)
		return RE::BSEventNotifyControl::kContinue;

	QueuedHit queued{};
	queued.formID = actor->formID;
	queued.element = element;
	{
		std::scoped_lock lock(feature.queuedCastLock);
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
			return false;
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

	SpellElement element = SpellElement::None;
	bool shoves = false;
	float shoveForce = 0.0f;
	const RE::BGSProjectile* projectile = nullptr;
	for (const auto* item : a_spell->effects) {
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

	spellStats.lastShoutElement = static_cast<uint>(element);
	spellStats.lastShoutSpeed = projectile ? projectile->data.speed : 0.0f;
	spellStats.lastShoutForce = shoveForce;
	spellStats.lastShoutVerdict = 3;

	if (element == SpellElement::None) {
		logger::info("[SNOW DEFORMATION] shout rejected, no element: {} ({} effects)",
			a_spell->GetName() ? a_spell->GetName() : "?", a_spell->effects.size());
		for (const auto* item : a_spell->effects)
			if (const auto* base = item ? item->baseEffect : nullptr) {
				std::string keywords;
				for (uint32_t k = 0; k < base->numKeywords; k++)
					if (base->keywords && base->keywords[k]) {
						keywords += base->keywords[k]->GetFormEditorID();
						keywords += ' ';
					}
				const auto* impacts = base->data.impactDataSet;
				logger::info("    effect {:08X} delivery={} resist={} archetype={} impacts={:08X} keywords: {}",
					base->formID, static_cast<int>(base->data.delivery),
					static_cast<int>(base->data.resistVariable), static_cast<int>(base->data.archetype),
					impacts ? impacts->formID : 0u,
					keywords.empty() ? "(none)" : keywords.c_str());
			}
		return;
	}

	if (element == SpellElement::Force && IsTravellingShove(projectile, shoveForce)) {
		spellStats.lastShoutVerdict = 2;
		return;
	}
	spellStats.lastShoutVerdict = 1;

	float range = projectile && projectile->data.range > 1.0f ? projectile->data.range : kShoutDefaultRange;
	range = std::clamp(range * std::max(settings.ShoutMarkReach, 0.0f), 0.0f, kShoutRangeMax);

	const float force = projectile && projectile->data.force > 0.0f ? projectile->data.force :
	                                                                  kShoutReferenceForce;
	const float strength = element == SpellElement::Force ?
	                           std::clamp(force / kShoutReferenceForce, kShoutForceMin, kShoutForceMax) :
	                           1.0f;

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
	auto* tes = globals::game::tes;
	if (!tes)
		return;

	const RE::NiPoint3 position = a_actor->GetPosition();
	float groundZ = position.z;
	tes->GetLandHeight(position, groundZ);

	float heightFade = 0.0f;
	float radius = 0.0f;
	const bool arcs = MarkForElement(a_cloak.element) == SpellMark::Pit;
	const float baseReach = arcs ? settings.ShockCloakRadius :
	                               (MarkForElement(a_cloak.element) == SpellMark::Crust ? settings.CrustRadius :
																						  settings.CloakRadius);
	const bool burning = a_cloak.burnRemaining > 0.0f;
	const float cloakReach = (a_cloak.reachOverride > 0.0f ? a_cloak.reachOverride : std::max(baseReach, 1.0f)) *
	                         std::max(a_cloak.reachScale, 0.01f) * (burning ? kDeathAuraReach : 1.0f);
	if (MarkForElement(a_cloak.element) == SpellMark::Melt)
		QueueObjectSpellMarkFor(position, position, cloakReach, SpellMark::Melt, 1.0f, std::max(settings.SpellMeltRate, 0.0f) * a_cloak.rateScale, 1.0f, 0u);
	if (!GroundMark(position.z + (burning ? 0.0f : kCloakCentreHeight) - groundZ, cloakReach, heightFade, radius))
		return;
	if (heightFade < kMinSpellStrength)
		return;
	spellStats.auras++;

	const float strength = 1.0f;

	const float2 current{ position.x, position.y };
	float2 previous = current;
	const uint32_t formID = a_actor->formID;
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
		a_cloak.strikeTimer -= a_deltaTime;
		if (a_cloak.strikeTimer > 0.0f)
			return;
		a_cloak.strikeTimer = std::max(settings.ShockCloakInterval, 0.02f);

		const uint32_t seed = ++a_cloak.strikeSeed * 2654435761u;
		const float angle = static_cast<float>(seed >> 8 & 0xFFFF) / 65535.0f * 6.2831853f;
		const float spread = kCloakStrikeInner +
		                     (1.0f - kCloakStrikeInner) * static_cast<float>(seed >> 3 & 0xFF) / 255.0f;
		const float reach = cloakReach * spread;

		const float2 strike{ current.x + std::cos(angle) * reach, current.y + std::sin(angle) * reach };

		if (settings.EnableLightningArcs) {
			float strikeZ = position.z;
			tes->GetLandHeight(RE::NiPoint3{ strike.x, strike.y, position.z }, strikeZ);
			EmitLightningArc(RE::NiPoint3{ position.x, position.y, position.z + kCloakCentreHeight },
				RE::NiPoint3{ strike.x, strike.y, strikeZ });
		}

		if (activeBlasts.size() >= kMaxSpellEmitters)
			return;
		ActiveBlast arc{};
		arc.position = strike;
		arc.radius = radius;
		arc.strength = strength;
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

bool SnowDeformation::AuraFromSpellList(const RE::TESSpellList* a_list, InnateAuraRecord& a_out)
{
	const auto* effects = a_list ? a_list->actorEffects : nullptr;
	if (!effects || !effects->spells)
		return false;

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
		if (spell->data.spellType != RE::MagicSystem::SpellType::kAbility)
			continue;
		for (const auto* item : spell->effects) {
			const RE::EffectSetting* base = item ? item->baseEffect : nullptr;
			if (!base)
				continue;

			if (base->data.delivery == RE::MagicSystem::Delivery::kSelf &&
				base->data.archetype == RE::EffectSetting::Archetype::kCloak) {
				const SpellElement element = ClassifyElement(base);
				if (element != SpellElement::None) {
					a_out.element = element;
					a_out.rateScale = std::clamp(item->effectItem.magnitude / kSpellReferenceMagnitude,
						kSpellMagnitudeMin, kSpellMagnitudeMax);
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
			if (base->IsDetrimental())
				weakness[slot] = true;
			else
				immunity[slot] = std::max(immunity[slot], item->effectItem.magnitude);
		}
	}

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
	a_out.rateScale = 1.0f;
	a_out.reachOverride = 0.0f;
	return true;
}

bool SnowDeformation::AuraFromActorRecords(RE::Actor* a_actor, InnateAuraRecord& a_out)
{
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	if (!race)
		return false;
	return AuraFromSpellList(race, a_out) || AuraFromSpellList(a_actor->GetActorBase(), a_out);
}

const SnowDeformation::InnateAuraRecord* SnowDeformation::ResolveInnateAura(RE::Actor* a_actor)
{
	auto* race = a_actor ? a_actor->GetRace() : nullptr;
	if (!race)
		return nullptr;

	auto raceIt = innateAuraByRace.find(race->formID);
	if (raceIt == innateAuraByRace.end()) {
		InnateAuraRecord record{};
		AuraFromSpellList(race, record);
		if (innateAuraByRace.size() > 512)
			innateAuraByRace.clear();
		raceIt = innateAuraByRace.insert_or_assign(race->formID, record).first;
	}
	if (raceIt->second.element != SpellElement::None)
		return &raceIt->second;

	auto* base = a_actor->GetActorBase();
	if (!base)
		return nullptr;
	auto baseIt = innateAuraByBase.find(base->formID);
	if (baseIt == innateAuraByBase.end()) {
		InnateAuraRecord record{};
		AuraFromSpellList(base, record);
		if (innateAuraByBase.size() > 2048)
			innateAuraByBase.clear();
		baseIt = innateAuraByBase.insert_or_assign(base->formID, record).first;
	}
	return baseIt->second.element != SpellElement::None ? &baseIt->second : nullptr;
}

void SnowDeformation::OpenInnateDeathBlast(CloakState& a_state, const RE::NiPoint3& a_position)
{
	a_state.blasted = true;

	float authored = settings.AtronachFireDeathRadius;
	if (a_state.element == SpellElement::Frost)
		authored = settings.AtronachFrostDeathRadius;
	else if (a_state.element == SpellElement::Shock)
		authored = settings.AtronachShockDeathRadius;

	a_state.burnRemaining = a_state.element == SpellElement::Fire ?
	                            std::max(settings.AtronachFireBurnSeconds, 0.0f) :
	                            0.0f;

	auto* tes = globals::game::tes;
	float groundZ = a_position.z;
	if (tes)
		tes->GetLandHeight(a_position, groundZ);
	const float scaled = authored * std::max(settings.BlastRadiusScale, 0.0f);
	const float height = std::max(a_position.z - groundZ, 0.0f);
	if (height >= scaled) {
		logger::info("[SNOW DEFORMATION] death blast skipped: {:.0f} above ground, reach {:.0f}", height, scaled);
		return;
	}
	const float strength = std::clamp(1.5f * (1.0f - height / std::max(scaled, 1.0f)), 0.0f, 1.0f);
	const float radius = std::max(std::sqrt(std::max(scaled * scaled - height * height, 0.0f)), scaled * 0.35f);
	logger::info("[SNOW DEFORMATION] death blast: height {:.0f} reach {:.0f} -> strength {:.2f} radius {:.0f} rate {:.1f}",
		height, scaled, strength, radius, kAtronachDeathRate);
	if (strength < kMinSpellStrength || activeBlasts.size() >= kMaxSpellEmitters)
		return;

	ActiveBlast opened{};
	opened.position = { a_position.x, a_position.y };
	opened.radius = radius;
	opened.strength = strength;
	opened.rate = kAtronachDeathRate;
	opened.remaining = kAtronachDeathHold;
	opened.diag = true;
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
		if (!a_actor->IsDead() || it->second.burnRemaining <= 0.0f) {
			corpseEffects.erase(it);
			return;
		}
		spellStats.corpses++;
		ConsiderActorAuras(a_actor, it->second, a_deltaTime);
		it->second.burnRemaining -= a_deltaTime;
		return;
	}

	if (!a_actor->IsDead())
		return;
	auto hit = lastElementalHit.find(formID);
	if (hit == lastElementalHit.end() || hit->second.element == SpellElement::None)
		return;

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
				if (death.element == SpellElement::Fire) {
					state.blasted = true;
					pendingDeathBlasts.push_back({ death.formID, death.element, death.position,
						kFireDeathFuseSeconds, death.actor });
				} else {
					const size_t before = activeBlasts.size();
					OpenInnateDeathBlast(state, death.position);
					if (activeBlasts.size() > before)
						spellStats.deathBlasts++;
				}
			}
			state.lastPosition = death.position;
		}
	}

	for (auto& entry : innateAuras)
		entry.second.unseenFor += a_deltaTime;

	auto consider = [&](RE::ActorHandle a_handle) {
		auto actor = a_handle.get();
		if (!actor || !actor->Is3DLoaded())
			return;
		if (a_cameraPosition.GetSquaredDistance(actor->GetPosition()) > a_cullRadius * a_cullRadius)
			return;

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
			state.blasted = false;
			state.burnRemaining = 0.0f;
			spellStats.innate++;
			ConsiderActorAuras(actor.get(), state, a_deltaTime);
			return;
		}

		if (!state.blasted)
			OpenInnateDeathBlast(state, state.lastPosition);
		if (state.burnRemaining > 0.0f) {
			spellStats.burning++;
			ConsiderActorAuras(actor.get(), state, a_deltaTime);
			state.burnRemaining -= a_deltaTime;
		}
	};

	if (auto* player = globals::game::player)
		consider(player->GetHandle());
	if (auto* processLists = RE::ProcessLists::GetSingleton())
		for (auto& handle : processLists->highActorHandles)
			consider(handle);

	for (auto it = innateAuras.begin(); it != innateAuras.end();) {
		auto& state = it->second;
		if (state.unseenFor <= 0.0f || state.actor.get()) {
			++it;
			continue;
		}
		if (!state.blasted && state.unseenFor < kInnateDeathWindow)
			OpenInnateDeathBlast(state, state.lastPosition);
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

		if (auto* controller = actor->GetCharController();
			controller && (controller->context.currentState == RE::hkpCharacterStateType::kInAir ||
							  controller->context.currentState == RE::hkpCharacterStateType::kFlying))
			continue;
		bool overGround = true;
		if (auto* tes = globals::game::tes) {
			float landZ = position.z;
			tes->GetLandHeight(position, landZ);
			overGround = position.z - landZ <= kDashGroundBand;
		}

		const float dx = current.x - last.x;
		const float dy = current.y - last.y;
		const float travelled = std::sqrt(dx * dx + dy * dy);
		if (travelled > kDashBreak)
			continue;
		if (travelled / std::max(a_deltaTime, 1e-4f) < kDashSpeedGate)
			continue;

		float halfWidth = kDashWidthMin;
		if (auto* root = actor->Get3D(false))
			halfWidth = root->worldBound.radius * kDashWidthOfBound;
		halfWidth = std::clamp(halfWidth * std::max(settings.DashGougeScale, 0.05f),
			kDashWidthMin, kDashWidthMax);
		QueueObjectSpellMarkFor(position, RE::NiPoint3{ last.x, last.y, position.z }, halfWidth, SpellMark::Carve, 1.0f, 0.0f, 1.0f, kPrintSpellBowl);
		if (!overGround)
			continue;

		SpellEmitter emitter{};
		emitter.position = current;
		emitter.previous = last;
		emitter.radius = halfWidth;
		emitter.strength = 1.0f;
		emitter.element = SpellElement::Force;
		emitter.mark = SpellMark::Carve;
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

	const RE::NiPoint3 tip = a_cone.position + a_cone.direction * a_cone.length;
	const float standoff = std::min(kShoutMouthOffset, a_cone.length * 0.5f);
	const RE::NiPoint3 apex = a_cone.position + a_cone.direction * standoff;
	const float travel = std::max(a_cone.length - standoff, 1.0f);
	const float halfWidth = std::max(travel * std::clamp(settings.ShoutMarkWidth, 0.05f, 2.0f) * 0.5f, 8.0f);
	const float speed = std::max(a_cone.speed, 1.0f);
	const float hold = travel / speed + std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold);
	const float seed = static_cast<float>(spellStats.shouts % 97u) * 7.31f;
	QueueObjectSpellMarkFor(RE::NiPoint3{ tip.x, tip.y, a_cone.position.z }, RE::NiPoint3{ apex.x, apex.y, a_cone.position.z }, halfWidth,
		MarkForElement(a_cone.element), a_cone.strength, 0.0f, 1.0f, kPrintSpellCone, hold, speed, seed);
	const RE::NiPoint3 mid = a_cone.position + a_cone.direction * (a_cone.length * 0.5f);
	float groundZ = mid.z;
	a_tes->GetLandHeight(mid, groundZ);
	float strength = 0.0f;
	float reachOut = 0.0f;
	if (!GroundMark(mid.z - groundZ, halfWidth, strength, reachOut))
		return;
	strength *= a_cone.strength;
	if (strength < kMinSpellStrength)
		return;

	ActiveBlast opened{};
	opened.cone = true;
	opened.apex = { apex.x, apex.y };
	opened.position = { tip.x, tip.y };
	opened.coneDir = { a_cone.direction.x, a_cone.direction.y };
	opened.coneLength = travel;
	opened.coneSpeed = speed;
	opened.coneSeed = seed;
	opened.radius = halfWidth;
	opened.strength = strength;
	opened.rate = kExplosionRate;
	opened.remaining = hold;
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

	QueueObjectSpellMarkFor(a_blast.position, a_blast.position + a_blast.direction * std::max(a_blast.landingReach, kBlastLandingReach), a_blast.radius,
		MarkForElement(a_blast.element), 1.0f, 0.0f, a_blast.pitScale, kPrintSpellRay, std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold));

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
	printSpellMarks.clear();
	ReplayHeldObjectSpellMarks(globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f);
	{
		const SpellStats carried = spellStats;
		spellStats = {};
		spellStats.casts = carried.casts;
		spellStats.detonations = carried.detonations;
		spellStats.shouts = carried.shouts;
		spellStats.shoutDiscs = carried.shoutDiscs;
		spellStats.groundContacts = carried.groundContacts;
		spellStats.deathsSeen = carried.deathsSeen;
		spellStats.deathBlasts = carried.deathBlasts;
		spellStats.rejectedElement = carried.rejectedElement;
		spellStats.rejectedNoBlast = carried.rejectedNoBlast;
		spellStats.lifted = carried.lifted;
		spellStats.lastShoutElement = carried.lastShoutElement;
		spellStats.lastShoutSpeed = carried.lastShoutSpeed;
		spellStats.lastShoutForce = carried.lastShoutForce;
		spellStats.lastShoutVerdict = carried.lastShoutVerdict;
		spellStats.lastStrength = carried.lastStrength;
		spellStats.lastRadius = carried.lastRadius;
	}

	if (!settings.EnableSpellIntegration) {
		runeSitesScratch.clear();
		PublishRuneSites({});
		spellStats = {};
		spellGameHours = -1.0f;
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

	auto* calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	static bool watchAnnounced = false;
	if (!watchAnnounced) {
		watchAnnounced = true;
		logger::info("[SNOW DEFORMATION] time-jump watch: calendar {}, timescale {:.0f}",
			calendar ? "armed" : "MISSING", calendar ? calendar->GetTimescale() : 0.0f);
	}
	if (calendar) {
		const float hours = calendar->GetHoursPassed();
		float jumpSeconds = 0.0f;
		bool wentBack = false;
		if (spellGameHours >= 0.0f) {
			const float dtReal = globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f;
			const float gameSeconds = (hours - spellGameHours) * 3600.0f;
			const float expected = dtReal * std::clamp(calendar->GetTimescale(), 1.0f, 100.0f);
			if (gameSeconds < -1.0f) {
				wentBack = true;
				spellGameDrift = 0.0f;
			} else {
				spellGameDrift = std::max(spellGameDrift + gameSeconds - expected, -5.0f);
				if (spellGameDrift > 15.0f) {
					jumpSeconds = spellGameDrift;
					spellGameDrift = 0.0f;
					logger::info("[SNOW DEFORMATION] time jump: {:.0f} game seconds applied to {} cloaks, {} blasts",
						jumpSeconds, activeCloaks.size(), activeBlasts.size());
				}
			}
		}
		spellGameHours = hours;
		if (wentBack) {
			activeCloaks.clear();
			corpseEffects.clear();
			dashWatches.clear();
			activeBlasts.clear();
			lightningArcs.clear();
		} else if (jumpSeconds > 0.0f) {
			for (auto& cloak : activeCloaks) {
				cloak.second.remaining -= jumpSeconds;
				cloak.second.burnRemaining -= jumpSeconds;
			}
			for (auto& corpse : corpseEffects) {
				corpse.second.remaining -= jumpSeconds;
				corpse.second.burnRemaining -= jumpSeconds;
			}
			for (auto& aura : innateAuras)
				aura.second.burnRemaining -= jumpSeconds;
			for (auto& watch : dashWatches)
				watch.remaining -= jumpSeconds;
			for (auto& blast : activeBlasts)
				blast.remaining -= jumpSeconds;
			lightningArcs.clear();
		}
	}

	auto* manager = RE::Projectile::Manager::GetSingleton();
	auto* tes = globals::game::tes;
	if (!manager || !tes) {
		spellPrevPositions.clear();
		return;
	}

	std::vector<RE::NiPointer<RE::Projectile>> live;
	{
		RE::BSSpinLockGuard lock(manager->projectileLock);
		live.reserve(manager->limited.size() + manager->unlimited.size() + manager->pending.size());
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

	{
		std::vector<CloakState> arrived;
		{
			std::scoped_lock lock(queuedCastLock);
			arrived.swap(queuedCloaks);
		}
		for (auto& cloak : arrived)
			if (auto actor = cloak.actor.get())
				activeCloaks[actor->formID] = cloak;

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

		GatherActorMarks(cloakDelta, cameraPosition, cullRadius);
		GatherDashGouges(cloakDelta, cameraPosition, cullRadius);
	}

	std::unordered_set<uint32_t> stillAlive;
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

		if (settings.RuneDecalsOnSnow || settings.LiftRunes)
			ConsiderRune(projectile.get(), cameraPosition, cullRadius, tes);

		auto& runtime = projectile->GetProjectileRuntimeData();

		const RE::EffectSetting* effect = runtime.avEffect;
		const RE::Effect* costliest = runtime.spell ? runtime.spell->GetCostliestEffectItem() : nullptr;
		if (!effect && costliest)
			effect = costliest->baseEffect;
		SpellElement element = ClassifyElement(effect);

		const RE::BGSExplosion* blast = runtime.explosion;
		if (effect && effect->data.explosion && !blast)
			blast = effect->data.explosion;
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
			QueueObjectSpellMark(position, position, std::max(settings.ForceTrackWidth, 4.0f), std::clamp(settings.ForceTrackDepth, 0.0f, 1.0f), 0.0f, kPrintSpellBowl);
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
			emitter.strength = trackStrength * std::clamp(settings.ForceTrackDepth, 0.0f, 1.0f);
			emitter.element = SpellElement::Force;
			emitter.mark = SpellMark::Carve;
			emitter.bowl = true;
			spellEmitters.push_back(emitter);
			spellStats.forceTracks++;
			continue;
		}

		RE::NiPoint3 direction = runtime.velocity;
		const float speed = direction.Length();
		if (speed > kMinSpellSpeed)
			direction /= speed;
		else if (!ShooterAim(runtime.shooter, direction))
			direction = { 0.0f, 0.0f, -1.0f };

		float spellReach = kSpellStreamReachDefault;
		bool hitscan = false;
		if (auto* baseForm = projectile->GetBaseObject())
			if (auto* projectileBase = baseForm->As<RE::BGSProjectile>()) {
				if (projectileBase->data.range > 1.0f)
					spellReach = std::clamp(projectileBase->data.range, kSpellReachMin, kSpellReachMax);
			}
		hitscan = runtime.flags.any(RE::Projectile::Flags::kHitScan);

		const bool strikes = effect->data.castingType != RE::MagicSystem::CastingType::kConcentration;

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
				pending.landingReach = hitscan ? spellReach :
				                                 std::max(kSpellReachMin, speed * kBlastTravelWindow);
				pending.position = position;
				pending.direction = direction;
				pending.heightAboveLand = position.z - blastGroundZ;
				pending.radius = authored * std::max(settings.BlastRadiusScale, 0.0f);
				pending.element = element;
				if (auto* baseForm = projectile->GetBaseObject())
					if (auto* projectileBase = baseForm->As<RE::BGSProjectile>())
						pending.proximity = projectileBase->data.flags.any(
							RE::BGSProjectileData::BGSProjectileFlags::kExplosionAltTrigger);
				pending.triggered = runtime.flags.any(kProjectileTriggered);

				const bool struckAlready = strikes && (hitscan || !runtime.impacts.empty());
				if (struckAlready) {
					if (!hitscanBlasted.contains(projectile->formID)) {
						hitscanBlasted.insert(projectile->formID);
						OpenProjectileBlast(pending, tes);
					}
				} else {
					projectileBlasts[projectile->formID] = pending;
				}
			}
		}

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

		if (effect->data.castingType != RE::MagicSystem::CastingType::kConcentration)
			continue;
		spellStats.streams++;

		RE::NiPoint3 markPosition{};
		float strength = 0.0f;
		float radius = 0.0f;

		QueueObjectSpellMarkFor(position, position + direction * spellReach, kSpellContactRadius, MarkForElement(element), 1.0f,
			std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest), kStreamPitScale, kPrintSpellRay);

		RE::NiPoint3 objectHit{};
		float objectSnowTop = 0.0f;
		if (TraceStreamObjectHit(tes, position, direction, spellReach, objectHit) &&
			!(SampleShellSurface(objectHit.x, objectHit.y, objectSnowTop) && objectHit.z <= objectSnowTop + kFrostDecalAboveSnow))
			continue;

		RE::NiPoint3 contact{};
		if (TraceGroundContact(tes, position, direction, spellReach, contact)) {
			spellStats.groundContacts++;
			markPosition = contact;
			strength = 1.0f;
			radius = kSpellContactRadius;
		} else {
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
		emitter.rate = std::max(settings.SpellMeltRate, 0.0f) * RateScaleOf(costliest);
		emitter.element = element;
		emitter.mark = MarkForElement(element);
		emitter.pitScale = kStreamPitScale;
		emitter.rateScale = RateScaleOf(costliest) * strength;
		spellEmitters.push_back(emitter);
	}

	for (auto it = projectileBlasts.begin(); it != projectileBlasts.end();) {
		if (stillAlive.contains(it->first)) {
			++it;
			continue;
		}
		const PendingBlast blast = it->second;
		it = projectileBlasts.erase(it);
		if (blast.proximity && !blast.triggered)
			continue;
		OpenProjectileBlast(blast, tes);
	}

	{
		std::vector<QueuedCone> cones;
		{
			std::scoped_lock lock(queuedCastLock);
			cones.swap(queuedCones);
		}
		for (const auto& cone : cones)
			OpenShoutCone(cone, tes);
	}

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
			QueueObjectSpellMarkFor(cast.position, cast.position, scaled, MarkForElement(cast.element), 1.0f, 0.0f, 1.0f, 0u, std::max(kBlastDuration, kBlastRampSeconds + kBlastMinHold));
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

	{
		const float deltaTime = globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f;
		for (auto pend = pendingDeathBlasts.begin(); pend != pendingDeathBlasts.end();) {
			pend->fuse -= deltaTime;
			if (auto actor = pend->actor.get(); actor && actor->Is3DLoaded())
				pend->position = actor->GetPosition();
			if (pend->fuse > 0.0f) {
				++pend;
				continue;
			}
			auto& state = innateAuras[pend->formID];
			if (state.element == SpellElement::None)
				state.element = pend->element;
			const size_t before = activeBlasts.size();
			OpenInnateDeathBlast(state, pend->position);
			if (activeBlasts.size() > before)
				spellStats.deathBlasts++;
			pend = pendingDeathBlasts.erase(pend);
		}

		for (auto it = activeBlasts.begin(); it != activeBlasts.end();) {
			if (spellEmitters.size() < kSpellEmitterCeiling) {
				const float ramp = it->cone ?
				                       1.0f :
				                       std::clamp(it->age / std::max(kBlastRampSeconds, 1e-3f), 0.0f, 1.0f);
				SpellEmitter emitter{};
				emitter.position = it->position;
				emitter.previous = it->cone ? it->apex : it->position;
				emitter.cone = it->cone;
				emitter.radius = it->radius;
				if (it->cone) {
					const float extent = std::clamp(
						it->coneSpeed * (it->age + deltaTime) / std::max(it->coneLength, 1e-3f),
						0.0f, 1.0f);
					emitter.position = { it->apex.x + it->coneDir.x * it->coneLength * extent,
						it->apex.y + it->coneDir.y * it->coneLength * extent };
					emitter.coneLength = it->coneLength;
					emitter.coneSeed = it->coneSeed;
				}
				emitter.strength = it->strength * ramp;
				emitter.rate = it->rate;
				emitter.element = it->element;
				emitter.mark = it->mark;
				emitter.pitScale = it->pitScale;
				emitter.rateScale = it->mark == SpellMark::Crust ?
				                        kBlastCrustRate / std::max(settings.CrustRate, 0.01f) :
				                        1.0f;
				spellEmitters.push_back(emitter);
			}
			it->age += deltaTime;
			it->remaining -= deltaTime;
			if (it->remaining <= 0.0f) {
				if (it->diag)
					logger::info("[SNOW DEFORMATION] death blast closed: lived {:.2f}s, final strength {:.2f}, radius {:.0f}, {} emitters queued this frame",
						it->age, it->strength, it->radius, spellEmitters.size());
				it = activeBlasts.erase(it);
			} else {
				++it;
			}
		}
	}

	std::erase_if(hitscanBlasted, [&](uint32_t a_id) { return !presentIDs.contains(a_id); });

	PublishRuneSites(presentIDs);

	spellPrevPositions = std::move(currentPositions);
	spellTrailPrev = std::move(currentTrailPositions);
	spellAuraPrev = std::move(currentAuraPositions);
	spellStats.emitters = static_cast<uint>(spellEmitters.size());
	spellStats.armed = static_cast<uint>(projectileBlasts.size());
}
