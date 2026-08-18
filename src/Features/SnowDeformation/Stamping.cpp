#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/ActorUtils.h"
#include "Utils/Game.h"

// Shapes whose bottom is further than this above ground level do not carve.
static constexpr float kStampSurfaceBand = 40.0f;
// Sanity clamp on extracted shape radii.
static constexpr float kMinStampShapeRadius = 4.0f;
static constexpr float kMaxStampShapeRadius = 128.0f;
// StampRadius setting value at which shape radii are unscaled.
static constexpr float kStampRadiusNeutral = 20.0f;
// Per-frame movement beyond this (teleport, cell load) breaks the capsule trail.
static constexpr float kTrailBreakDistance = 256.0f;
// Movement below this counts as standing still.
static constexpr float kStampMovementGate = 3.0f;
// Corpse settled-latch: wake displacement and frames-still until settled.
static constexpr float kCorpseWakeDistance = 50.0f;
static constexpr uint16_t kCorpseSettleFrames = 90;
// Per-frame speed below which an unsettled corpse shape counts as still for
// the settle counter (ragdoll jitter sits below, real motion above).
static constexpr float kCorpseStillSpeed = 0.5f;
// Depth-scaled stamps: the nominal snow depth at the mover's position
// scales its stamp radii (shallow snow takes narrower trenches). The clamp
// keeps bare and unbaked ground recording readable trails.
static constexpr float kStampDepthReference = 30.0f;
static constexpr float kStampDepthScaleMin = 0.65f;
static constexpr float kStampDepthScaleMax = 1.2f;
// Per-foot stamping: a foot bone higher than this above the actor's ground
// reference is in swing phase and does not stamp. Scaled by the bone's world
// scale (giants, scaled races). The ankle joint sits ~8 units above the sole
// on humanoids, so the band leaves ~7 units of stride tolerance.
static constexpr float kFootPlantBand = 15.0f;
// Tall-ankle skeletons (mammoths) hold every foot bone above the absolute
// band; when even the lowest foot misses it, the plant test switches to
// relative-to-lowest-foot (the lowest foot of a grounded skeleton is
// planted). Tighter than the absolute band: swing feet pass close by.
static constexpr float kFootRelativeBand = 10.0f;
// Big feet articulate proportionally higher (and spread across slopes), so
// the relative band also grows with the foot's own length.
static constexpr float kFootRelativeLenFactor = 0.6f;
// The toe bone marks the ball of the foot; the print capsule extends past it
// by this fraction of the heel-toe length (the capsule end caps add the
// rounded heel and toe tips on top).
static constexpr float kFootToeExtend = 0.2f;
// Print half-width as a fraction of the extended heel-toe length.
static constexpr float kFootWidthRatio = 0.22f;
// Circular print radius (at bone scale 1) for feet without a toe bone (hooves).
static constexpr float kHoofRadius = 7.0f;
// Below ~1.5 deformation texels a print aliases away; snow prints collapse
// wider than the foot anyway.
static constexpr float kMinFootStampRadius = 5.0f;
// Trail keys for foot/limb stamps set these bits so they never collide with
// Havok shape traversal indices when an actor switches paths (death, fallback).
static constexpr uint64_t kFootKeyBit = 0x8000;
static constexpr uint64_t kLimbKeyBit = 0x4000;
// Limb stamps below this carve fraction are invisible; skip them.
static constexpr float kMinLimbCarve = 0.05f;
// Prop stamps floor here so small dropped items (daggers, gems) stay visible
// at deformation-texel resolution.
static constexpr float kMinPropStampRadius = 5.0f;
// Bodies descending faster than this are in flight and do not stamp (no
// carving under a throw or a ragdoll arc). Support is judged by fall speed,
// not land height: props and corpses resting on statics (roads, bridges,
// snow drifts) sit far above the land, and a land-height band starves
// their stamps there.
static constexpr float kFallSpeedGate = 300.0f;
// Skeletons carry bones that are hidden or never composed for this race
// (tail bones on tailless races, XPMSSE style nodes): scale 0 and/or a
// world transform at the origin. A capsule anchored on one combs a trench
// across the world, so both segment endpoints must be live and near the
// actor (dragons are the far-endpoint ceiling).
static constexpr float kMaxLimbEndpointDistance = 1024.0f;
// Runaway-skeleton caps on the bone cache.
static constexpr size_t kMaxCachedFeet = 8;
static constexpr size_t kMaxCachedLimbs = 32;

static bool LimbEndpointValid(const RE::NiTransform& a_world, const RE::NiPoint3& a_actorPos)
{
	if (a_world.scale < 0.01f)
		return false;
	const float dx = a_world.translate.x - a_actorPos.x;
	const float dy = a_world.translate.y - a_actorPos.y;
	return dx * dx + dy * dy < kMaxLimbEndpointDistance * kMaxLimbEndpointDistance;
}

// Case-insensitive substring/prefix tests for skeleton bone names.
static bool NameContains(const RE::BSFixedString& a_name, const char* a_needle)
{
	const char* hay = a_name.c_str();
	if (!hay)
		return false;
	const size_t needleLen = strlen(a_needle);
	for (const char* p = hay; *p; ++p)
		if (_strnicmp(p, a_needle, needleLen) == 0)
			return true;
	return false;
}

static bool NameStartsWith(const RE::BSFixedString& a_name, const char* a_prefix)
{
	const char* hay = a_name.c_str();
	return hay && _strnicmp(hay, a_prefix, strlen(a_prefix)) == 0;
}

static RE::NiAVObject* FindToeBone(RE::NiNode* a_node)
{
	for (auto& child : a_node->GetChildren()) {
		auto* node = child.get() ? child.get()->AsNode() : nullptr;
		if (!node)
			continue;
		if (NameContains(node->name, "toe"))
			return node;
		if (auto* deeper = FindToeBone(node))
			return deeper;
	}
	return nullptr;
}

// Body bones stamped as joint-to-joint segments. A bone node sits at its
// PROXIMAL joint, so the segment (nearest matched ancestor -> bone) spans the
// ancestor's flesh; min() of the two class radii keeps shoulder/hip joins from
// inheriting torso thickness. Terminal classes get an extra sphere for the
// mass beyond the last joint (skull, fingers).
struct LimbSpec
{
	const char* substr;
	float radius;
	bool terminal;
};
static constexpr LimbSpec kLimbSpecs[] = {
	{ "pelvis", 11.0f, true },
	{ "spine", 10.0f, false },
	{ "thigh", 7.0f, false },
	{ "calf", 5.5f, false },
	{ "upperarm", 5.5f, false },
	{ "forearm", 4.5f, false },
	{ "hand", 4.5f, true },
	{ "neck", 5.0f, false },
	{ "head", 8.5f, true },
	{ "tail", 4.0f, false },
};

static const LimbSpec* MatchLimb(const RE::BSFixedString& a_name)
{
	for (const auto& spec : kLimbSpecs)
		if (NameContains(a_name, spec.substr))
			return &spec;
	return nullptr;
}

// Bones only (NiNode): skinned geometry like "FemaleFeet" must not match.
// CME/MOV prefixes are XPMSSE control nodes mirroring bone names; they fail
// the match but stay on the recursion path (XPMSSE inserts them as parents
// of the real bones).
static void CollectStampBones(RE::NiAVObject* a_obj, RE::NiAVObject* a_ancestor, float a_ancestorRadius,
	SnowDeformation::StampBones& a_out)
{
	auto* node = a_obj ? a_obj->AsNode() : nullptr;
	if (!node)
		return;
	const auto& name = node->name;
	const bool controlNode = NameStartsWith(name, "CME ") || NameStartsWith(name, "MOV ");
	if (!controlNode &&
		(NameContains(name, "foot") || NameContains(name, "hoof") || NameContains(name, "paw"))) {
		if (a_out.feet.size() < kMaxCachedFeet)
			a_out.feet.push_back({ RE::NiPointer<RE::NiAVObject>(node), RE::NiPointer<RE::NiAVObject>(FindToeBone(node)) });
		// The shin: ancestor (calf) joint down to the ankle.
		if (a_ancestor && a_out.limbs.size() < kMaxCachedLimbs)
			a_out.limbs.push_back({ RE::NiPointer<RE::NiAVObject>(a_ancestor), RE::NiPointer<RE::NiAVObject>(node),
				a_ancestorRadius });
		return;
	}
	if (!controlNode) {
		if (const auto* spec = MatchLimb(name)) {
			if (a_ancestor && a_out.limbs.size() < kMaxCachedLimbs)
				a_out.limbs.push_back({ RE::NiPointer<RE::NiAVObject>(a_ancestor), RE::NiPointer<RE::NiAVObject>(node),
					std::min(a_ancestorRadius, spec->radius) });
			if ((spec->terminal || !a_ancestor) && a_out.limbs.size() < kMaxCachedLimbs)
				a_out.limbs.push_back({ RE::NiPointer<RE::NiAVObject>(node), RE::NiPointer<RE::NiAVObject>(node),
					spec->radius });
			for (auto& child : node->GetChildren())
				CollectStampBones(child.get(), node, spec->radius, a_out);
			return;
		}
	}
	for (auto& child : node->GetChildren())
		CollectStampBones(child.get(), a_ancestor, a_ancestorRadius, a_out);
}

void SnowDeformation::GatherStamps(PerFrame& perFrameData)
{
	GatherSpellEmitters();

	uint stampCount = 0;
	RE::NiPoint3 cameraPosition = Util::GetEyePosition();
	std::unordered_map<uint64_t, float2> currentPositions;
	corpseMoundSpheres.clear();
	stampStats = {};

	// Living actors stamp heel-to-toe capsules from skeleton foot bones
	// (discrete alternating prints); skeletons without foot bones, corpses
	// and props stamp their Havok collision shapes (Util::GetShapeBound over
	// TraverseScenegraphCollision), so ragdoll limbs still carve individually.
	auto addStamps = [&](RE::ActorHandle a_handle) {
		if (stampCount >= kMaxStamps)
			return;
		auto actor = a_handle.get();
		if (!actor || !actor->Is3DLoaded())
			return;
		auto position = actor->GetPosition();
		// Cull to the deformation window.
		if (cameraPosition.GetSquaredDistance(position) > 0.25f * deformWorldSize * deformWorldSize)
			return;
		auto root = actor->Get3D(false);
		if (!root)
			return;

		const uint32_t formID = actor->formID;
		// The dead carve every frame until they settle; once settled only a
		// large displacement (dragging, explosions) wakes them and the
		// refill buries their imprint. No first-sight waiver: decapitation
		// swaps the 3D, and a waiver would re-trench under buried corpses.
		const bool isDead = actor->IsDead();

		CorpseRest* rest = nullptr;
		if (isDead) {
			if (corpseRestStates.size() > 512 && !corpseRestStates.contains(formID))
				corpseRestStates.clear();
			rest = &corpseRestStates[formID];
		} else {
			// Reanimated: back to living rules.
			corpseRestStates.erase(formID);
		}
		bool anyShapeMoved = false;
		bool anyShapeWoken = false;

		// Airborne living actors do not carve. Dead ragdolls are exempt:
		// their controllers freeze in stale states (often kInAir).
		if (!isDead)
			if (auto* charController = actor->GetCharController(); charController && charController->context.currentState == RE::hkpCharacterStateType::kInAir)
				return;

		// Living actors on ELEVATED structures (walkways, roofs, bridges) do
		// not stamp either: the deformation map is 2D, so their trails would
		// carve the ground shell below AND every snow surface above at the
		// same XY (tracks on roofs from walking the walkway beneath).
		// Interim gate until the stamp-height channel lands; drift tops
		// (under the cutoff) keep their trails. Corpses keep the round-218
		// elevated-surface behavior.
		if (!isDead) {
			float landZ = position.z;
			if (const auto tesLand = RE::TES::GetSingleton())
				tesLand->GetLandHeight(position, landZ);
			if (position.z - landZ > kElevatedStampCutoff)
				return;
		}

		// Living actors use their own position as the ground reference. Dead
		// ragdolls are exempt from the airborne gate above, so their ground
		// is the land height: a corpse flung off a ledge must not carve the
		// snow beneath its flight arc.
		float groundZ = position.z;
		if (isDead)
			if (const auto tesGround = RE::TES::GetSingleton())
				tesGround->GetLandHeight(position, groundZ);

		if (rest) {
			// Flight gate: a flung ragdoll must not carve under its arc.
			const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 1e-4f) : 1.0f / 60.0f;
			const bool fallingCorpse = rest->hasPrevZ && (position.z - rest->prevZ) / dt < -kFallSpeedGate;
			rest->prevZ = position.z;
			rest->hasPrevZ = true;
			if (fallingCorpse)
				return;
		}

		const float nominalDepth = std::max(
			GetNominalSnowDepthAt(position.x, position.y, kStampDepthReference), 1.0f);
		const float depthScale = std::clamp(nominalDepth / kStampDepthReference,
			kStampDepthScaleMin, kStampDepthScaleMax);

		StampBones* bones = nullptr;
		if (stampBoneCache.size() > 512 && !stampBoneCache.contains(formID))
			stampBoneCache.clear();
		auto& cache = stampBoneCache[formID];
		if (cache.root.get() != root) {
			cache.root = RE::NiPointer<RE::NiAVObject>(root);
			cache.feet.clear();
			cache.limbs.clear();
			CollectStampBones(root, nullptr, 0.0f, cache);
		}
		if (!cache.feet.empty() || !cache.limbs.empty())
			bones = &cache;

		// Living actors need matched feet to take the bone path: a limbs-only
		// match (creature spines/necks) would steal the collision-shape
		// fallback while its high segments carve nothing.
		if (!isDead && bones && !bones->feet.empty()) {
			{
				float minFootZ = FLT_MAX;
				float minFootScale = 1.0f;
				for (const auto& foot : bones->feet)
					if (auto* n = foot.node.get(); n && n->world.scale >= 0.01f && n->world.translate.z < minFootZ) {
						minFootZ = n->world.translate.z;
						minFootScale = n->world.scale;
					}
				const bool groundRefStarved =
					minFootZ != FLT_MAX && minFootZ - groundZ > kFootPlantBand * minFootScale;

				uint32_t footIndex = 0;
				for (const auto& foot : bones->feet) {
					const uint32_t thisIndex = footIndex++;
					if (stampCount >= kMaxStamps)
						break;
					auto* footNode = foot.node.get();
					if (!footNode)
						continue;
					const auto& footWorld = footNode->world;
					if (footWorld.scale < 0.01f)
						continue;
					const float boneScale = footWorld.scale;

					float2 heel = { footWorld.translate.x, footWorld.translate.y };
					float2 tip = heel;
					// Toeless feet size off the parent-bone distance: a fixed
					// hoof radius turns mammoth feet into dots.
					float footLen = 2.0f * kHoofRadius * boneScale;
					float radius = kHoofRadius * boneScale;
					if (auto* parentNode = footNode->parent) {
						const float parentDist = footWorld.translate.GetDistance(parentNode->world.translate);
						footLen = std::max(footLen, parentDist);
						radius = std::max(radius, 0.2f * parentDist);
					}
					if (auto* toeNode = foot.toe.get()) {
						const auto& toePos = toeNode->world.translate;
						float2 dir = { toePos.x - heel.x, toePos.y - heel.y };
						const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
						if (len > 2.0f) {
							const float extend = 1.0f + kFootToeExtend;
							tip = { heel.x + dir.x * extend, heel.y + dir.y * extend };
							radius = len * extend * kFootWidthRatio;
							footLen = len * extend;
						}
					}
					radius = std::clamp(radius * settings.FootPrintScale * depthScale,
						kMinFootStampRadius, kMaxStampShapeRadius);

					// Absence from the trail map is the lifted latch: a foot in
					// swing phase drops out, so its next plant starts a fresh
					// discrete print instead of dragging from the previous one.
					const float plantRef = groundRefStarved ? minFootZ : groundZ;
					const float plantBand = groundRefStarved ?
					                            std::max(kFootRelativeBand * boneScale, kFootRelativeLenFactor * footLen) :
					                            kFootPlantBand * boneScale;
					if (footWorld.translate.z - plantRef > plantBand)
						continue;

					// A continuously planted heel can still slide (shuffles,
					// slopes); the capsule then covers drag plus foot length.
					float2 segStart = heel;
					const uint64_t key = (uint64_t(formID) << 16) | (kFootKeyBit | uint64_t(thisIndex & 0x7FFF));
					auto it = stampPrevPositions.find(key);
					if (it != stampPrevPositions.end()) {
						float2 delta = { heel.x - it->second.x, heel.y - it->second.y };
						if (delta.x * delta.x + delta.y * delta.y < kTrailBreakDistance * kTrailBreakDistance)
							segStart = it->second;
					}
					currentPositions[key] = heel;

					float4 stamp{};
					stamp.x = tip.x;
					stamp.y = tip.y;
					stamp.z = 1.0f;
					stamp.w = radius;
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { segStart.x, segStart.y, 0.0f, 0.0f };
					stampCount++;
					stampStats.feet++;
				}
			}

			// Limb segments carve to the fraction their underside reaches
			// into the nominal snow layer: wading legs connect the prints in
			// deep snow, shallow snow keeps prints discrete. No trail latch:
			// per-frame segment stamps stay continuous at any speed.
			for (const auto& limb : bones->limbs) {
				if (stampCount >= kMaxStamps)
					break;
				auto* nodeA = limb.a.get();
				auto* nodeB = limb.b.get();
				if (!nodeA || !nodeB)
					continue;
				const auto& aWorld = nodeA->world;
				const auto& bWorld = nodeB->world;
				if (!LimbEndpointValid(aWorld, position) || !LimbEndpointValid(bWorld, position))
					continue;
				const float boneScale = aWorld.scale;
				const float radius = std::clamp(limb.radius * boneScale * depthScale,
					kMinStampShapeRadius, kMaxStampShapeRadius);
				const float heightAbove = std::min(aWorld.translate.z, bWorld.translate.z) - radius - groundZ;
				const float carve = std::min(1.0f - heightAbove / nominalDepth, 1.0f);
				if (carve < kMinLimbCarve)
					continue;

				float4 stamp{};
				stamp.x = bWorld.translate.x;
				stamp.y = bWorld.translate.y;
				stamp.z = carve;
				stamp.w = radius;
				perFrameData.Stamps[stampCount] = stamp;
				perFrameData.StampEnds[stampCount] = { aWorld.translate.x, aWorld.translate.y, 0.0f, 0.0f };
				stampCount++;
				stampStats.limbs++;
			}
			return;
		}

		// Corpses with cached bones imprint body-shaped: the same limb
		// segments, run through the shape path's settle latch per limb.
		const bool useCorpseBones = isDead && bones && !bones->limbs.empty();
		if (useCorpseBones) {
			// A corpse rests on whatever its lowest limb touches (statics
			// included): that limb carves fully, higher ones taper.
			float lowestLimbBottom = FLT_MAX;
			for (const auto& limb : bones->limbs) {
				auto* nA = limb.a.get();
				auto* nB = limb.b.get();
				if (!nA || !nB || !LimbEndpointValid(nA->world, position) || !LimbEndpointValid(nB->world, position))
					continue;
				lowestLimbBottom = std::min(lowestLimbBottom,
					std::min(nA->world.translate.z, nB->world.translate.z) - limb.radius * nA->world.scale);
			}
			const float corpseGroundZ = lowestLimbBottom != FLT_MAX ? std::max(groundZ, lowestLimbBottom) : groundZ;

			uint32_t limbIndex = 0;
			for (const auto& limb : bones->limbs) {
				const uint32_t thisIndex = limbIndex++;
				if (stampCount >= kMaxStamps)
					break;
				auto* nodeA = limb.a.get();
				auto* nodeB = limb.b.get();
				if (!nodeA || !nodeB)
					continue;
				const auto& aWorld = nodeA->world;
				const auto& bWorld = nodeB->world;
				if (!LimbEndpointValid(aWorld, position) || !LimbEndpointValid(bWorld, position))
					continue;
				const float boneScale = aWorld.scale;
				const float radius = std::clamp(limb.radius * boneScale * depthScale,
					kMinStampShapeRadius, kMaxStampShapeRadius);
				const RE::NiPoint3 center = (aWorld.translate + bWorld.translate) * 0.5f;
				const float halfLen = aWorld.translate.GetDistance(bWorld.translate) * 0.5f;

				float2 current = { center.x, center.y };
				const uint64_t key = (uint64_t(formID) << 16) | (kLimbKeyBit | uint64_t(thisIndex & 0x3FFF));
				auto it = stampPrevPositions.find(key);
				float sqDelta = 0.0f;
				if (it != stampPrevPositions.end()) {
					float2 delta = { current.x - it->second.x, current.y - it->second.y };
					sqDelta = delta.x * delta.x + delta.y * delta.y;
				}
				const bool firstSight = (it == stampPrevPositions.end());
				const bool woken = !firstSight && sqDelta > kCorpseWakeDistance * kCorpseWakeDistance;
				anyShapeMoved |= !firstSight && sqDelta > kCorpseStillSpeed * kCorpseStillSpeed;
				anyShapeWoken |= woken;
				if (firstSight || (rest->settled && !woken)) {
					// First sight baselines only; settled corpses keep the
					// frozen anchor and feed the burial mounds instead.
					currentPositions[key] = firstSight ? current : it->second;
					if (corpseMoundSpheres.size() < kMaxCorpseSpheres)
						corpseMoundSpheres.push_back({ center.x, center.y, center.z, halfLen + radius });
					continue;
				}
				currentPositions[key] = current;

				const float heightAbove = std::min(aWorld.translate.z, bWorld.translate.z) - radius - corpseGroundZ;
				const float carve = std::clamp(1.0f - heightAbove / nominalDepth, 0.0f, 1.0f);
				if (carve < kMinLimbCarve)
					continue;

				float4 stamp{};
				stamp.x = bWorld.translate.x;
				stamp.y = bWorld.translate.y;
				stamp.z = carve;
				stamp.w = radius;
				perFrameData.Stamps[stampCount] = stamp;
				perFrameData.StampEnds[stampCount] = { aWorld.translate.x, aWorld.translate.y, 0.0f, 0.0f };
				stampCount++;
				stampStats.limbs++;
			}
		}

		// Band reference: identical to groundZ for the living (their ground
		// IS their position); for dead ragdolls it lifts to the body, so
		// corpses on statics keep their shape stamps.
		const float bandRefZ = std::max(groundZ, position.z);
		uint32_t shapeIndex = 0;
		if (!useCorpseBones)
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 centerPos;
				float radius;
				if (Util::GetShapeBound(a_object, centerPos, radius)) {
					// Stable per-skeleton traversal order keys the trail history.
					const uint32_t thisIndex = shapeIndex++;
					if (stampCount >= kMaxStamps)
						return RE::BSVisit::BSVisitControl::kStop;
					if (centerPos.z - radius > bandRefZ + kStampSurfaceBand)
						return RE::BSVisit::BSVisitControl::kContinue;
					if (radius < kMinStampShapeRadius || radius > kMaxStampShapeRadius)
						return RE::BSVisit::BSVisitControl::kContinue;

					// Capsule stamp from the shape's previous position keeps
					// fast movers' trails continuous.
					float2 current = { centerPos.x, centerPos.y };
					float2 previous = current;
					const uint64_t key = (uint64_t(formID) << 16) | uint64_t(thisIndex & 0xFFFF);
					auto it = stampPrevPositions.find(key);
					float sqDelta = 0.0f;
					if (it != stampPrevPositions.end()) {
						float2 delta = { current.x - it->second.x, current.y - it->second.y };
						sqDelta = delta.x * delta.x + delta.y * delta.y;
						if (sqDelta < kTrailBreakDistance * kTrailBreakDistance)
							previous = it->second;
					}
					const bool firstSight = (it == stampPrevPositions.end());
					// Against the frozen resting anchor: dragging accumulates
					// past the wake distance, ragdoll jitter does not.
					const bool woken = !firstSight && sqDelta > kCorpseWakeDistance * kCorpseWakeDistance;
					if (isDead) {
						// While unsettled the anchor follows every frame, so
						// sqDelta is per-frame speed there; the epsilon separates
						// real motion from jitter for the settle counter.
						anyShapeMoved |= !firstSight && sqDelta > kCorpseStillSpeed * kCorpseStillSpeed;
						anyShapeWoken |= woken;
					}
					if (isDead && (firstSight || (rest->settled && !woken))) {
						// First sight baselines only; settled corpses keep the
						// frozen anchor and feed the burial mounds instead.
						currentPositions[key] = firstSight ? current : it->second;
						if (corpseMoundSpheres.size() < kMaxCorpseSpheres)
							corpseMoundSpheres.push_back({ centerPos.x, centerPos.y, centerPos.z, radius });
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Unsettled dead stamp every frame, exactly like the living:
					// the snow deforms as the body moves through it.
					currentPositions[key] = current;

					float4 stamp{};
					stamp.x = current.x;
					stamp.y = current.y;
					stamp.z = 1.0f;
					// StampRadius scales the shape's own radius.
					stamp.w = radius * settings.StampRadius / kStampRadiusNeutral * depthScale;
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { previous.x, previous.y, 0.0f, 0.0f };
					stampCount++;
					stampStats.shapes++;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});

		if (rest) {
			if (anyShapeWoken) {
				rest->settled = false;
				rest->stillFrames = 0;
			} else if (anyShapeMoved) {
				rest->stillFrames = 0;
			} else if (!rest->settled && ++rest->stillFrames >= kCorpseSettleFrames) {
				rest->settled = true;
			}
		}
	};

	if (auto player = RE::PlayerCharacter::GetSingleton())
		addStamps(player->GetHandle());

	if (const auto processLists = RE::ProcessLists::GetSingleton()) {
		for (auto& actorHandle : processLists->highActorHandles)
			addStamps(actorHandle);
	}

	// Loose props carve while moving. The cheap root-position gate runs
	// before any collision traversal.
	std::unordered_map<uint32_t, RE::NiPoint3> currentPropPositions;
	const auto tes = RE::TES::GetSingleton();
	auto* playerRef = RE::PlayerCharacter::GetSingleton();
	if (tes && playerRef) {
		tes->ForEachReferenceInRange(playerRef, 0.5f * deformWorldSize, [&](RE::TESObjectREFR* a_ref) {
			if (!a_ref || a_ref->As<RE::Actor>())
				return RE::BSContainer::ForEachResult::kContinue;
			// Spell walls and runes ride this scan instead of adding one of
			// their own: they are ordinary references and this pass already
			// runs every frame at the right radius. Note the form type is
			// PlacedHazard - Hazard is the base form, and filtering on that
			// silently matches nothing.
			if (a_ref->GetFormType() == RE::FormType::PlacedHazard) {
				ConsiderHazard(a_ref);
				return RE::BSContainer::ForEachResult::kContinue;
			}
			auto* base = a_ref->GetBaseObject();
			if (!base)
				return RE::BSContainer::ForEachResult::kContinue;
			// Havok-movable base types only; projectiles must not carve
			// under their flight path.
			switch (base->GetFormType()) {
			case RE::FormType::Misc:
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Ammo:
			case RE::FormType::Book:
			case RE::FormType::Ingredient:
			case RE::FormType::AlchemyItem:
			case RE::FormType::SoulGem:
			case RE::FormType::KeyMaster:
			case RE::FormType::Light:
			case RE::FormType::MovableStatic:
				break;
			default:
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (!a_ref->Is3DLoaded())
				return RE::BSContainer::ForEachResult::kContinue;
			auto root = a_ref->Get3D(false);
			if (!root)
				return RE::BSContainer::ForEachResult::kContinue;

			// Gate on the 3D root's world transform, not the reference
			// position: Havok moves the scene graph every frame while the
			// reference position lags until the body settles.
			stampStats.propRefs++;
			const auto position = root->world.translate;
			const uint32_t formID = a_ref->formID;
			auto prevIt = propPrevPositions.find(formID);
			if (prevIt == propPrevPositions.end()) {
				currentPropPositions[formID] = position;
				return RE::BSContainer::ForEachResult::kContinue;  // first sight: baseline only
			}
			// Frozen anchor: slow motion accumulates toward the gate instead
			// of resetting every frame.
			const bool propMoved = position.GetSquaredDistance(prevIt->second) >= kStampMovementGate * kStampMovementGate;
			currentPropPositions[formID] = propMoved ? position : prevIt->second;
			if (propMoved)
				stampStats.propMovers++;
			if (!propMoved)
				return RE::BSContainer::ForEachResult::kContinue;  // at rest: the refill buries it
			if (stampCount >= kMaxStamps)
				return RE::BSContainer::ForEachResult::kContinue;  // keep collecting anchors

			// Fast-falling props must not carve under their arc; supported
			// ones stamp wherever they lie, including on top of statics.
			const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 1e-4f) : 1.0f / 60.0f;
			if ((position.z - prevIt->second.z) / dt < -kFallSpeedGate)
				return RE::BSContainer::ForEachResult::kContinue;

			float groundZ = position.z;
			tes->GetLandHeight(position, groundZ);
			// Band reference: whichever is higher, the land or the prop's own
			// root — elevated resting surfaces keep their stamps.
			const float supportZ = std::max(groundZ, position.z);

			const float depthScale = std::clamp(
				GetNominalSnowDepthAt(position.x, position.y, kStampDepthReference) / kStampDepthReference,
				kStampDepthScaleMin, kStampDepthScaleMax);
			uint32_t shapeIndex = 0;
			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 centerPos;
				float radius;
				if (Util::GetShapeBound(a_object, centerPos, radius)) {
					const uint32_t thisIndex = shapeIndex++;
					if (stampCount >= kMaxStamps)
						return RE::BSVisit::BSVisitControl::kStop;
					if (centerPos.z - radius > supportZ + kStampSurfaceBand)
						return RE::BSVisit::BSVisitControl::kContinue;
					// Small item shapes (daggers, gems) are real: floored at
					// stamp time instead of skipped like actor shapes.
					if (radius > kMaxStampShapeRadius)
						return RE::BSVisit::BSVisitControl::kContinue;

					// Props share the (formID << 16 | shape) keyspace with
					// actors; formIDs are unique.
					float2 current = { centerPos.x, centerPos.y };
					float2 previous = current;
					const uint64_t key = (uint64_t(formID) << 16) | uint64_t(thisIndex & 0xFFFF);
					auto it = stampPrevPositions.find(key);
					if (it != stampPrevPositions.end()) {
						float2 delta = { current.x - it->second.x, current.y - it->second.y };
						if (delta.x * delta.x + delta.y * delta.y < kTrailBreakDistance * kTrailBreakDistance)
							previous = it->second;
					}
					currentPositions[key] = current;

					float4 stamp{};
					stamp.x = current.x;
					stamp.y = current.y;
					stamp.z = 1.0f;
					stamp.w = std::max(radius * settings.StampRadius / kStampRadiusNeutral * depthScale, kMinPropStampRadius);
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { previous.x, previous.y, 0.0f, 0.0f };
					stampCount++;
					stampStats.props++;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});

			// Shape types with no bound extractor (MOPP/list): one stamp from
			// the root's bound sphere.
			if (shapeIndex == 0 && stampCount < kMaxStamps) {
				const auto& bound = root->worldBound;
				float radius = std::clamp(bound.radius, kMinStampShapeRadius, kMaxStampShapeRadius);
				if (bound.center.z - radius <= supportZ + kStampSurfaceBand) {
					float2 current = { bound.center.x, bound.center.y };
					float2 previous = current;
					const uint64_t key = (uint64_t(formID) << 16) | 0xFFFFull;
					auto it = stampPrevPositions.find(key);
					if (it != stampPrevPositions.end()) {
						float2 delta = { current.x - it->second.x, current.y - it->second.y };
						if (delta.x * delta.x + delta.y * delta.y < kTrailBreakDistance * kTrailBreakDistance)
							previous = it->second;
					}
					currentPositions[key] = current;

					float4 stamp{};
					stamp.x = current.x;
					stamp.y = current.y;
					stamp.z = 1.0f;
					stamp.w = std::max(radius * settings.StampRadius / kStampRadiusNeutral * depthScale, kMinPropStampRadius);
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { previous.x, previous.y, 0.0f, 0.0f };
					stampCount++;
					stampStats.props++;
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}
	propPrevPositions = std::move(currentPropPositions);

	// Spell emitters melt rather than displace. Appended AFTER actors and
	// props on purpose: a busy fight must not starve foot prints out of the
	// stamp budget, and prints are the marks players read first.
	for (const auto& emitter : spellEmitters) {
		if (stampCount >= kMaxStamps)
			break;
		// Frost and shock sources are detected and carry their element, but
		// crust and pitting do not exist yet, so they mark nothing until
		// their own steps land. Splitting detection from effect is exactly
		// what the emitter list is for.
		if (emitter.mark != SpellMark::Melt) {
			spellStats.pending++;
			continue;
		}
		float4 stamp{};
		stamp.x = emitter.position.x;
		stamp.y = emitter.position.y;
		stamp.z = emitter.strength;
		stamp.w = emitter.radius;
		perFrameData.Stamps[stampCount] = stamp;
		perFrameData.StampEnds[stampCount] = { emitter.previous.x, emitter.previous.y,
			kStampModeMelt, emitter.rate };
		stampCount++;
		stampStats.spells++;
	}

	// Debug melt emitter: a stationary heat source, so the additive stamp path
	// can be watched against a known shape with no spell involved.
	if (debugMeltEmitterActive && stampCount < kMaxStamps) {
		float4 stamp{};
		stamp.x = debugMeltEmitterPos.x;
		stamp.y = debugMeltEmitterPos.y;
		stamp.z = 1.0f;
		stamp.w = debugMeltEmitterRadius;
		perFrameData.Stamps[stampCount] = stamp;
		perFrameData.StampEnds[stampCount] = { debugMeltEmitterPos.x, debugMeltEmitterPos.y,
			kStampModeMelt, debugMeltEmitterRate };
		stampCount++;
	}

	stampPrevPositions = std::move(currentPositions);
	perFrameData.StampCount = stampCount;
}
