// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

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
// Full reference scan cadence, in frames. Between scans only the movers and
// hazards the last scan found are revisited; a prop's first motion waits at
// most one interval (~100 ms), which no one has seen.
static constexpr uint32_t kPropScanInterval = 6;
// Corpse settled-latch: wake displacement and frames-still until settled.
static constexpr float kCorpseWakeDistance = 50.0f;
static constexpr uint16_t kCorpseSettleFrames = 90;
// Per-frame speed below which an unsettled corpse shape counts as still for
// the settle counter (ragdoll jitter sits below, real motion above).
static constexpr float kCorpseStillSpeed = 0.5f;
// Contact-drawn corpses wake when the body has left its resting place by this much: a loot jiggle stays under it, a drag or a kick does not.
static constexpr float kCorpseRasterWake = 12.0f;
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
// Edited-skeleton failsafe. A healthy walker plants each foot every ~60 units
// of travel; ground travel with zero prints past the first threshold buys one
// bone re-collection (the editor may have replaced the nodes), past the second
// it latches the actor onto collision stamping for this 3D.
static constexpr float kFootDryRecollect = 150.0f;
static constexpr float kFootDryFallback = 300.0f;
// Per-frame travel above this is a teleport, not a stride.
static constexpr float kFootDryTeleport = 200.0f;
// Cadence for re-verifying cached feet are still attached to the root.
static constexpr uint16_t kFootAttachRecheckFrames = 60;
// Trail keys for foot/limb stamps set these bits so they never collide with
// Havok shape traversal indices when an actor switches paths (death, fallback).
static constexpr uint64_t kFootKeyBit = 0x8000;
static constexpr uint64_t kLimbKeyBit = 0x4000;
/** Body key for the bow wave's own previous-position entry. */
static constexpr uint64_t kBodyKeyBit = 0x2000;
/** Push radius of a human-sized actor before the Reach crank, in world units. */
static constexpr float kBowWaveBaseRadius = 30.0f;
/** A jump this big in one frame is a teleport, a fast travel or a cell load, not a stride. */
static constexpr float kBowWaveTeleport = 400.0f;
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
// How far past its own rest length a limb segment may stretch before it stops
// being a limb. Bones are rigid, so in principle this is 1.0; the slack is for
// animated scale and for the pseudo-joints the ancestor walk produces. A frost
// atronach SHATTERS on death and its pieces fly apart, and the capsule between
// two of them combs a flat band across the snow - the endpoint test cannot see
// it, because both ends stay within reach of the actor while being nowhere
// near each other.
static constexpr float kLimbStretchGate = 2.0f;
// Floor under the gate, so terminal segments (a == b, rest length zero) and
// very short bones are not held to a hair's breadth.
static constexpr float kLimbStretchFloor = 24.0f;
// Backstop on how long a segment may be against its own THICKNESS, for the
// case the learned length cannot cover: a death that swaps the actor's 3D
// rebuilds the bone cache, so the first length it ever measures is already the
// shattered one and there is no intact frame left to correct it. Self-scaling
// in the right direction - a mammoth's limbs are thick, so it stays permissive
// where bones really are long, and tight on the small skeletons where a comb
// across the snow is most visible.
static constexpr float kLimbAspectGate = 10.0f;
// Material alpha below which a skinned body counts as drawn see-through. Kept
// well clear of 1 rather than near it: an ordinary body is authored at exactly
// 1.0, and Skyrim's ghost shader sits far below this, so the gap between the
// two cases is wide and nothing needs tuning in between.
static constexpr float kIncorporealAlpha = 0.95f;
// Frames between re-measurements of a body's alpha. The look can arrive after
// the 3D does, so one early opaque reading must not stand for ever - but it
// changes rarely enough that measuring every frame would be waste.
static constexpr uint16_t kBodyAlphaRecheckFrames = 30;
/**
 * @brief Early re-reads of an actor's body alpha, and how far apart.
 *
 * A ghost's see-through shader is applied AFTER its model loads, so the first
 * measurement of a freshly spawned one reads a solid 1.0 and it carves trenches
 * until the next check comes round - half a second of tracks no ghost should
 * leave. Cell loading races the same way, so this is not only a console-spawn
 * artefact. Twelve reads five frames apart covers the first second; after that
 * the answer is settled and the slow cadence is enough.
 */
static constexpr uint16_t kBodyAlphaSettleReads = 12;
static constexpr uint16_t kBodyAlphaSettleFrames = 5;
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
// True when this segment has stretched past anything a bone could be. The rest
// length is learned rather than configured: the shortest sighting is the honest
// one, so a first sight that happened to catch a body mid-shatter corrects
// itself as soon as an intact frame arrives.
static bool LimbStretched(SnowDeformation::StampBones::Limb& a_limb, float a_length, float a_radius)
{
	if (a_limb.restLength < 0.0f || a_length < a_limb.restLength)
		a_limb.restLength = a_length;
	if (a_length > std::max(a_limb.restLength * kLimbStretchGate, kLimbStretchFloor))
		return true;
	return a_length > std::max(a_radius, 1.0f) * kLimbAspectGate;
}

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

// Parent-walk: a cached node an editor detached no longer reaches the root,
// and its world transform is frozen wherever it was left.
static bool NodeAttachedTo(const RE::NiAVObject* a_node, const RE::NiAVObject* a_root)
{
	for (auto* p = a_node; p; p = p->parent)
		if (p == a_root)
			return true;
	return false;
}

// Non-flesh nodes that carry body-part substrings and stamped as limbs:
// holsters ("Weapon L Calf" matches calf, "Ankle Dagger Offhand" matches
// hand), IED/XPMSSE "Extra*" gear slots ("ExtraLThighFlute"), and HDT-SMP
// physics bones (the auto-rename prefix itself contains "Head", so every
// hair braid matched - and dangling cloth bones wobble every frame, which
// also kept the stamp-quiet idle skip from ever engaging nearby). Gear
// never touches snow while worn; physics bones are covered by the flesh
// they hang from.
static bool GearNodeName(const RE::BSFixedString& a_name)
{
	return NameStartsWith(a_name, "Extra") || NameStartsWith(a_name, "hdt") ||
	       NameContains(a_name, "weapon") || NameContains(a_name, "dagger") ||
	       NameContains(a_name, "sword") || NameContains(a_name, "axe") ||
	       NameContains(a_name, "mace") || NameContains(a_name, "staff") ||
	       NameContains(a_name, "shield") || NameContains(a_name, "quiver") ||
	       NameContains(a_name, "bolt") || NameContains(a_name, "torch") ||
	       NameContains(a_name, "magicnode") || NameContains(a_name, "animobject");
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
// CME/MOV prefixes are XPMSSE control nodes mirroring bone names, and gear
// holster nodes carry body-part substrings; both fail the match but stay on
// the recursion path (XPMSSE inserts control nodes as parents of real bones).
static void CollectStampBones(RE::NiAVObject* a_obj, RE::NiAVObject* a_ancestor, float a_ancestorRadius,
	SnowDeformation::StampBones& a_out)
{
	auto* node = a_obj ? a_obj->AsNode() : nullptr;
	if (!node)
		return;
	const auto& name = node->name;
	const bool controlNode = NameStartsWith(name, "CME ") || NameStartsWith(name, "MOV ") ||
	                         GearNodeName(name);
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

// Full-tree dump for the skeleton probe: every node with its match
// classification, so a tester's log shows exactly what the stamper saw.
static void DumpSkeletonToLog(RE::NiAVObject* a_obj, int a_depth)
{
	if (!a_obj || a_depth > 24)
		return;
	auto* node = a_obj->AsNode();
	const char* name = a_obj->name.c_str() ? a_obj->name.c_str() : "";
	const char* kind = "";
	if (!node)
		kind = " [geometry: never matches]";
	else if (NameStartsWith(a_obj->name, "CME ") || NameStartsWith(a_obj->name, "MOV "))
		kind = " [control: skipped]";
	else if (GearNodeName(a_obj->name))
		kind = " [gear: skipped]";
	else if (NameContains(a_obj->name, "foot") || NameContains(a_obj->name, "hoof") || NameContains(a_obj->name, "paw"))
		kind = " [FOOT]";
	else if (NameContains(a_obj->name, "toe"))
		kind = " [toe]";
	else if (MatchLimb(a_obj->name))
		kind = " [limb]";
	logger::info("[SNOW DEFORMATION] skel {:{}}{} scale={:.3f} worldZ={:.1f}{}",
		"", a_depth * 2, name, a_obj->world.scale, a_obj->world.translate.z, kind);
	if (node)
		for (auto& child : node->GetChildren())
			DumpSkeletonToLog(child.get(), a_depth + 1);
}

// Geometry census for the same dump: every mesh under the actor with what the
// contact pass will make of it. A part the game draws but nobody sees, or a
// bound nowhere near the body, names itself here.
static void DumpGeometryToLog(RE::NiAVObject* a_root)
{
	if (!a_root)
		return;
	const RE::NiPoint3 rootPos = a_root->world.translate;
	RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
		auto& runtime = a_geometry->GetGeometryRuntimeData();
		bool hidden = false;
		for (const RE::NiAVObject* n = a_geometry; n && !hidden; n = n->parent)
			hidden = n->GetAppCulled();
		std::string skin = "rigid";
		if (auto* si = runtime.skinInstance.get()) {
			uint32_t parts = si->skinPartition ? si->skinPartition->numPartitions : 0;
			uint32_t bones = si->skinData ? si->skinData->GetBoneCount() : 0;
			skin = std::format("skinned {} partitions / {} bones", parts, bones);
		}
		const char* shader = "NO SHADER";
		if (auto* sp = runtime.shaderProperty.get())
			shader = sp->GetRTTI() && sp->GetRTTI()->GetName() ? sp->GetRTTI()->GetName() : "shader";
		const auto& b = a_geometry->worldBound;
		uint32_t tris = 0;
		if (auto* si = runtime.skinInstance.get(); si && si->skinPartition && si->skinPartition->partitions.data()) {
			for (uint32_t p = 0; p < si->skinPartition->numPartitions; ++p)
				tris += si->skinPartition->partitions[p].triangles;
		} else if (auto* ts = a_geometry->AsTriShape()) {
			tris = ts->GetTrishapeRuntimeData().triangleCount;
		}
		logger::info("[SNOW DEFORMATION] geom '{}': {} | {} tris | {} | {} | bound rel root ({:.0f}, {:.0f}, {:.0f}) r {:.0f} | parent '{}'",
			a_geometry->name.c_str() ? a_geometry->name.c_str() : "", skin, tris, shader, hidden ? "HIDDEN" : "visible",
			b.center.x - rootPos.x, b.center.y - rootPos.y, b.center.z - rootPos.z, b.radius,
			(a_geometry->parent && a_geometry->parent->name.c_str()) ? a_geometry->parent->name.c_str() : "");
		return RE::BSVisit::BSVisitControl::kContinue;
	});
}

// Weight a shape puts through a crust, from its size. Crust bears a boot and
// gives under a mammoth, and the shapes a heavy skeleton carries are simply
// bigger - there is no mass to read off a collision shape, but this tracks it
// closely enough that the exceptions do not matter.
bool SnowDeformation::ActorIsIncorporeal(RE::Actor* a_actor, RE::NiAVObject* a_root,
	StampBones* a_bones, float* a_alphaOut, bool* a_flagOut) const
{
	if (a_alphaOut)
		*a_alphaOut = 1.0f;
	if (a_flagOut)
		*a_flagOut = false;
	if (!a_actor)
		return false;
	const int mode = settings.IncorporealMode;

	// The record flag. A static read of the base, so no relocation and no
	// live-actor state - Bethesda's "Is Ghost" means invulnerable rather than
	// incorporeal, so it catches more than ghosts, but it never misses one.
	bool flagged = false;
	if (const auto* base = a_actor->GetActorBase())
		flagged = base->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kIsGhost);
	if (a_flagOut)
		*a_flagOut = flagged;

	// Translucency, through Community Shaders' own test for a see-through
	// surface. Cached with the bones, which are already keyed by the 3D root,
	// and re-measured on a slow timer because the shader can be applied after
	// the model loads.
	float alpha = a_bones ? a_bones->bodyAlpha : -1.0f;
	const bool wantAlpha = mode == 1 || mode == 3;
	if (wantAlpha && a_root) {
		bool stale = alpha < 0.0f;
		if (a_bones) {
			if (a_bones->alphaRecheck > 0)
				a_bones->alphaRecheck--;
			else
				stale = true;
		} else {
			stale = true;
		}
		if (stale) {
			float lowest = 1.0f;
			RE::BSVisit::TraverseScenegraphGeometries(a_root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
				auto& runtime = a_geometry->GetGeometryRuntimeData();
				// SKINNED only. Plain alpha blending is no use on its own -
				// every NPC's hair and eyes blend - so what is read is the
				// body's own material alpha, which an ordinary actor authors
				// at exactly 1.
				if (!runtime.skinInstance)
					return RE::BSVisit::BSVisitControl::kContinue;
				auto& property = runtime.shaderProperty;
				if (property && property->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get())
					lowest = std::min(lowest, static_cast<RE::BSLightingShaderProperty*>(property.get())->alpha);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			alpha = lowest;
			if (a_bones) {
				a_bones->bodyAlpha = lowest;
				const bool settling = a_bones->alphaSettle < kBodyAlphaSettleReads;
				a_bones->alphaRecheck = settling ? kBodyAlphaSettleFrames : kBodyAlphaRecheckFrames;
				if (settling)
					a_bones->alphaSettle++;
			}
		}
	}
	if (alpha < 0.0f)
		alpha = 1.0f;
	if (a_alphaOut)
		*a_alphaOut = alpha;

	const bool translucent = alpha < kIncorporealAlpha;
	switch (mode) {
	case 1:
		return translucent;
	case 2:
		return flagged;
	case 3:
		return translucent || flagged;
	default:
		return false;
	}
}

bool SnowDeformation::ActorIsFloating(RE::Actor* a_actor, RE::NiAVObject* a_root,
	const StampBones* a_bones, bool a_useFeet, float a_groundZ, float* a_gapOut, bool* a_byFeetOut) const
{
	if (a_gapOut)
		*a_gapOut = 0.0f;
	if (a_byFeetOut)
		*a_byFeetOut = false;
	if (!a_actor)
		return false;
	const float band = std::max(settings.FloatingActorBand, 0.0f);
	float lowest = FLT_MAX;

	// Whatever the stamping path below would use. Feet first, because an
	// actor that has them stamps from them; then limb undersides; and only
	// when a skeleton offers neither is the collision tree walked.
	if (a_bones) {
		if (a_useFeet)
			for (const auto& foot : a_bones->feet)
				if (auto* node = foot.node.get(); node && node->world.scale >= 0.01f)
					lowest = std::min(lowest, node->world.translate.z);
		if (lowest == FLT_MAX)
			for (const auto& limb : a_bones->limbs) {
				auto* nodeA = limb.a.get();
				auto* nodeB = limb.b.get();
				if (!nodeA || !nodeB)
					continue;
				lowest = std::min(lowest,
					std::min(nodeA->world.translate.z, nodeB->world.translate.z) - limb.radius * nodeA->world.scale);
			}
	}
	// Feet decide alone; LIMBS do not. A skeleton that matched no foot bone
	// offers only spines and necks, and a reindeer measured by its spine reads
	// 71 units off the ground - past every wisp threshold - while standing
	// flat-footed in the snow. So a footless skeleton is also measured by its
	// COLLISION, which reaches its legs, and the lower of the two answers.
	// Wisps stay caught: their collision is their floating body.
	const bool measuredByFeet = lowest != FLT_MAX && a_bones && a_useFeet && !a_bones->feet.empty();
	if (a_byFeetOut)
		*a_byFeetOut = measuredByFeet;
	if (!measuredByFeet && a_root)
		RE::BSVisit::TraverseScenegraphCollision(a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			RE::NiPoint3 centerPos;
			float radius;
			if (Util::GetShapeBound(a_object, centerPos, radius))
				lowest = std::min(lowest, centerPos.z - radius);
			return RE::BSVisit::BSVisitControl::kContinue;
		});

	// Nothing to measure is not the same as hovering.
	if (lowest == FLT_MAX)
		return false;
	const float gap = lowest - a_groundZ;
	if (a_gapOut)
		*a_gapOut = gap;
	return gap > band;
}

float SnowDeformation::CrustBreakForce(float a_radius) const
{
	const float threshold = std::max(settings.CrustBreakRadius, 1.0f);
	return std::clamp((a_radius - threshold) / threshold, 0.0f, 1.0f);
}

// A collision shape's stamp: its own silhouette on the ground (a capsule
// along its longest horizontal extent, as wide as the next) when that covers
// this frame's motion, else the sweep from where it was. Subtracting the
// width from the length keeps the capsule's end caps inside the shape - a
// sphere collapses to the point it always was.
struct ShapeStamp
{
	float2 a;
	float2 b;
	float radius;
	// Vertical half-extent in this orientation; the bound radius when the
	// footprint is unavailable, which is what the old underside used.
	float halfHeight;
};
static ShapeStamp ShapeStampFor(RE::bhkNiCollisionObject* a_object, float2 a_current, float2 a_previous,
	float a_boundRadius, bool a_footprints)
{
	ShapeStamp out{ a_current, a_previous, a_boundRadius, a_boundRadius };
	float ax, ay, halfLen, halfWid, halfHgt;
	if (!a_footprints || !Util::GetShapeFootprint(a_object, ax, ay, halfLen, halfWid, halfHgt))
		return out;
	out.halfHeight = halfHgt;
	const float seg = std::max(halfLen - halfWid, 0.0f);
	const float dx = a_current.x - a_previous.x;
	const float dy = a_current.y - a_previous.y;
	out.radius = halfWid;
	if (dx * dx + dy * dy <= seg * seg) {
		out.a = { a_current.x + ax * seg, a_current.y + ay * seg };
		out.b = { a_current.x - ax * seg, a_current.y - ay * seg };
	}
	return out;
}

void SnowDeformation::GatherStamps(PerFrame& perFrameData)
{
	globals::profiler->BeginPass("SnowDeformation::GatherStamps");
	uint stampCount = 0;
	// Actors and props stop short of the pool so spells always have somewhere
	// to land. They are read first and in engine order, so without this a
	// ragdoll pile fills all 256 and the fight that caused the pile marks
	// nothing. The slots go unused when no spell is active, which costs a few
	// limb prints inside a heap nobody is reading.
	const uint actorCeiling = kMaxStamps - kSpellStampReserve;
	float nearestDistSq = FLT_MAX;
	RE::NiPoint3 cameraPosition = Util::GetEyePosition();
	std::unordered_map<uint64_t, float2> currentPositions;
	bowWaves.clear();
	if (bowWaveSpeed.size() > 512)
		bowWaveSpeed.clear();
	stampStats = {};

	// The map is 2048 whatever the Trenches range, so past ~100 m a texel
	// outgrows the minimum print and a min-size stamp can fall entirely
	// between texel centres - NPC feet at the clamp floor sporadically write
	// nothing. Floor every stamp at one texel; fatter prints at long range
	// are the honest cost of the coarser map (raise Deformation Map
	// Resolution to keep them sharp instead).
	const float texelFloor = deformWorldSize / (float)deformMapDim;
	const float footRadiusFloor = std::max(kMinFootStampRadius, texelFloor);
	const float limbRadiusFloor = std::max(kMinStampShapeRadius, texelFloor);
	const bool probeActive = debugSkeletonProbe && skeletonProbeTarget != 0;
	skeletonProbe.valid = false;
	contactProps.clear();
	contactActors.clear();
	contactLivingCount = 0;
	contactCorpseCount = 0;
	contactCenter = { windowOrigin.x + deformWorldSize * 0.5f, windowOrigin.y + deformWorldSize * 0.5f };

	// Living actors stamp heel-to-toe capsules from skeleton foot bones
	// (discrete alternating prints); skeletons without foot bones, corpses
	// and props stamp their Havok collision shapes (Util::GetShapeBound over
	// TraverseScenegraphCollision), so ragdoll limbs still carve individually.
	auto addStamps = [&](RE::ActorHandle a_handle) {
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
		const bool probing = probeActive && formID == skeletonProbeTarget;
		if (probing) {
			skeletonProbe = {};
			skeletonProbe.valid = true;
			skeletonProbe.formID = formID;
			skeletonProbe.actorName = actor->GetName() ? actor->GetName() : "";
			skeletonProbe.verdict = "processing";
		}
		// Counted per actor turned away whole, which is what the Trenches
		// range slider risks: a wider gather radius against a fixed budget.
		if (stampCount >= actorCeiling) {
			stampStats.budgetTurnedAway++;
			if (probing)
				skeletonProbe.verdict = "gated: stamp budget exhausted";
			return;
		}
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
		//
		// kFlying is the engine's own word for a creature that does not walk,
		// which is the cleanest floating signal there is where a creature uses
		// it - no measurement, no threshold. It does not cover everything: the
		// atronach races are all authored Walks and hover by animation instead,
		// which is what ActorIsFloating below is for.
		auto* charController = actor->GetCharController();
		if (!isDead && charController &&
			(charController->context.currentState == RE::hkpCharacterStateType::kInAir ||
				charController->context.currentState == RE::hkpCharacterStateType::kFlying)) {
			if (probing)
				skeletonProbe.verdict = "gated: airborne (controller in-air/flying)";
			return;
		}

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
			if (probing)
				skeletonProbe.gapToLand = position.z - landZ;
			if (position.z - landZ > kElevatedStampCutoff) {
				if (probing)
					skeletonProbe.verdict = "gated: elevated surface (above land cutoff)";
				return;
			}
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
		if (probing) {
			// Sentinel far below any real blended depth: a baked bare-class
			// cell legitimately reads small negative.
			const float probeDepth = GetNominalSnowDepthAt(position.x, position.y, -10000.0f);
			skeletonProbe.cellBaked = probeDepth > -9999.0f;
			skeletonProbe.shellDepth = skeletonProbe.cellBaked ? probeDepth : -1.0f;
		}

		// Bow wave. The body measures the motion - one smoothed speed and
		// heading per actor, so the crest never pulses with the gait - but the
		// feet carry it (emitted in the foot loop below). Anchoring the crest
		// to the body places it a fixed radius from the actor's centre, which
		// a sprinting lead foot can reach and clip through. A crest measured
		// from the foot is zero at the foot and peaks ahead of it.
		float bowWaveStrength = 0.0f;
		float2 bowWaveDir = { 0.0f, 0.0f };
		if (settings.BowWaveHeight > 0.001f && !isDead) {
			const uint64_t bodyKey = (uint64_t(formID) << 16) | kBodyKeyBit;
			const float2 here = { position.x, position.y };
			auto prevBody = stampPrevPositions.find(bodyKey);
			currentPositions[bodyKey] = here;
			auto& track = bowWaveSpeed[formID];
			const float dtBody = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 1e-4f) : 1.0f / 60.0f;
			if (prevBody != stampPrevPositions.end()) {
				const float2 step = { here.x - prevBody->second.x, here.y - prevBody->second.y };
				const float dist = std::sqrt(step.x * step.x + step.y * step.y);
				// A teleport must not read as a supersonic stride.
				if (dist < kBowWaveTeleport) {
					if (dist > 0.5f) {
						track.x = step.x / dist;
						track.y = step.y / dist;
					}
					// Asymmetric smoothing: the crest builds fast when you set
					// off and eases out over ~0.3 s when you stop, which IS the
					// "settles after a brief moment" half of the design.
					const float instant = dist / dtBody;
					// Builds fast, HOLDS slow. Snow that has been shoved
					// aside does not shove itself back the moment you stop;
					// too quick a decay reads as the crest morphing back into
					// flat ground. The settle time is a crank.
					// The deposit field owns persistence now, so this only
					// has to stop the live crest flickering between strides:
					// a short release, not the settle time (which decays the
					// DEPOSIT, in DeformationUpdateCS).
					const float rate = instant > track.z ? 12.0f : 2.5f;
					track.z += (instant - track.z) * std::clamp(rate * dtBody, 0.0f, 1.0f);
				}
			}
			bowWaveStrength = std::clamp(track.z / std::max(settings.BowWaveFullSpeed, 1.0f), 0.0f, 1.0f);
			bowWaveDir = { track.x, track.y };
		}

		StampBones* bones = nullptr;
		if (stampBoneCache.size() > 512 && !stampBoneCache.contains(formID))
			stampBoneCache.clear();
		auto& cache = stampBoneCache[formID];
		auto recollectBones = [&] {
			cache.root = RE::NiPointer<RE::NiAVObject>(root);
			cache.feet.clear();
			cache.limbs.clear();
			CollectStampBones(root, nullptr, 0.0f, cache);
			cache.attachRecheck = kFootAttachRecheckFrames;
		};
		if (cache.root.get() != root) {
			recollectBones();
			cache.dryTravel = 0.0f;
			cache.hasPrevPos = false;
			cache.dryRecollected = false;
			cache.collisionFallback = false;
		}

		// Runtime skeleton editors (RaceMenu/NiOverride, IED, MuSkeletonEditor)
		// edit the live tree without swapping the root, so the root key alone
		// cannot vouch for a cached foot: re-verify attachment on a cadence.
		if (!cache.feet.empty()) {
			if (cache.attachRecheck > 0) {
				cache.attachRecheck--;
			} else {
				cache.attachRecheck = kFootAttachRecheckFrames;
				for (const auto& foot : cache.feet)
					if (auto* n = foot.node.get(); n && !NodeAttachedTo(n, root)) {
						recollectBones();
						break;
					}
			}
		}
		// A matched foot detached or zero-scaled is not a foot this frame; an
		// actor with none usable must not take the bone path, or the collision
		// fallback its skeleton needs is unreachable.
		uint usableFeet = 0;
		for (const auto& foot : cache.feet)
			if (auto* n = foot.node.get(); n && n->world.scale >= 0.01f)
				usableFeet++;
		if (!cache.feet.empty() || !cache.limbs.empty())
			bones = &cache;
		const bool footPath = !isDead && usableFeet > 0 && !cache.collisionFallback;

		// Watchdog: ground travel with zero foot prints. Only strides count -
		// not teleports, and not frames off the ground or in a saddle.
		float dryStep = 0.0f;
		if (cache.hasPrevPos) {
			const float dx = position.x - cache.prevPosX;
			const float dy = position.y - cache.prevPosY;
			const float d = std::sqrt(dx * dx + dy * dy);
			if (d < kFootDryTeleport)
				dryStep = d;
		}
		cache.prevPosX = position.x;
		cache.prevPosY = position.y;
		cache.hasPrevPos = true;
		const bool walkingOnGround = !isDead && charController &&
		                             charController->context.currentState == RE::hkpCharacterStateType::kOnGround &&
		                             !actor->IsOnMount();
		auto accumulateDry = [&](float a_step) {
			if (!walkingOnGround || !footPath)
				return;
			cache.dryTravel += a_step;
			if (!cache.dryRecollected && cache.dryTravel > kFootDryRecollect) {
				recollectBones();
				cache.dryRecollected = true;
			}
			if (cache.dryTravel > kFootDryFallback)
				cache.collisionFallback = true;
		};

		if (probing) {
			skeletonProbe.usableFeet = usableFeet;
			skeletonProbe.dryTravel = cache.dryTravel;
			skeletonProbe.collisionFallback = cache.collisionFallback;
			skeletonProbe.bodyAlpha = cache.bodyAlpha;
			skeletonProbe.alphaSettle = cache.alphaSettle;
			skeletonProbe.limbs = (uint)cache.limbs.size();
			for (const auto& foot : cache.feet) {
				SkeletonProbe::FootRow row;
				auto* n = foot.node.get();
				row.name = n && n->name.c_str() ? n->name.c_str() : "<null>";
				auto* t = foot.toe.get();
				row.toe = t && t->name.c_str() ? t->name.c_str() : "-";
				row.scale = n ? n->world.scale : 0.0f;
				row.attached = n && NodeAttachedTo(n, root);
				skeletonProbe.feet.push_back(std::move(row));
			}
			if (skeletonProbeDumpRequested) {
				skeletonProbeDumpRequested = false;
				logger::info("[SNOW DEFORMATION] Skeleton dump: {} ({:08X}), {} feet / {} limbs matched",
					skeletonProbe.actorName, formID, cache.feet.size(), cache.limbs.size());
				DumpSkeletonToLog(root, 0);
				DumpGeometryToLog(root);
			}
		}

		// Floating actors carve nothing. Atronachs, wisps and ghosts never
		// touch the ground, so every mark they leave today is one the snow
		// should not have taken - and the foot path is what puts it there:
		// when no foot reaches the ground it falls back to plumbing the
		// LOWEST foot instead, which plants a hovering actor's sole no matter
		// how high it is. Corpses are exempt: a body that has fallen is lying
		// in the snow, and its dent is correct.
		float floatingGap = 0.0f;
		bool floatingByFeet = false;
		const bool floating = !isDead &&
		                      ActorIsFloating(actor.get(), root, bones, footPath, groundZ, &floatingGap, &floatingByFeet);
		if (probing)
			skeletonProbe.floatingGap = floatingGap;
		float bodyAlpha = 1.0f;
		bool ghostFlag = false;
		// A thing made of an element has a body however transparent it is, and
		// the translucency test cannot tell ice from a ghost - a Frost Atronach
		// reads 0.75 and carves nothing. The discriminator is an elemental
		// affinity on the RACE (an aura cloak, or near-immunity to one element
		// paired with a weakness to another): atronachs and ice wraiths carry
		// one, wisps and ghosts do not. Derived rather than named, reusing the
		// per-race cache the innate auras fill.
		//
		// NOT the kIsGhost flag: 72 of the 90 ghost-named actors in Skyrim.esm
		// do not set it. The flag means invulnerable.
		const bool elemental = ResolveInnateAura(actor.get()) != nullptr;
		// Corpses are exempt from BOTH gates: a body that has fallen is lying
		// in the snow whatever it was in life.
		const bool incorporeal = !isDead && !elemental &&
		                         ActorIsIncorporeal(actor.get(), root, bones, &bodyAlpha, &ghostFlag);

		// Diagnostics for the nearest creature, so the clearance band can be
		// read off a real wisp or ghost rather than guessed at. The gap to the
		// LAND is reported beside it because the two references answer
		// different questions: an actor lifted by its animation shows up in the
		// first, one whose controller itself floats only in the second.
		if (!actor->IsPlayerRef()) {
			const float distSq = cameraPosition.GetSquaredDistance(position);
			if (!stampStats.nearestValid || distSq < nearestDistSq) {
				nearestDistSq = distSq;
				float landZ = position.z;
				if (const auto tesNear = RE::TES::GetSingleton())
					tesNear->GetLandHeight(position, landZ);
				stampStats.nearestValid = true;
				stampStats.nearestGapToRoot = floatingGap;
				stampStats.nearestGapToLand = position.z - landZ;
				stampStats.nearestFloating = floating;
				stampStats.nearestElemental = elemental;
				stampStats.nearestFeet = bones ? static_cast<uint>(bones->feet.size()) : 0;
				stampStats.nearestLimbs = bones ? static_cast<uint>(bones->limbs.size()) : 0;
				stampStats.nearestUsableFeet = usableFeet;
				stampStats.nearestFallback = cache.collisionFallback;
				stampStats.nearestDryTravel = cache.dryTravel;
				stampStats.nearestFormID = formID;
				stampStats.nearestBodyAlpha = bodyAlpha;
				stampStats.nearestGhostFlag = ghostFlag;
				stampStats.nearestIncorporeal = incorporeal;
				auto* controller = actor->GetCharController();
				stampStats.nearestState = controller ?
				                              static_cast<uint>(controller->context.currentState) :
				                              0xFFu;
			}
		}

		// Counted apart. One number for both said "floating actors not carving"
		// while incorporeal ones were in it too, which is a diagnostic that
		// answers the wrong question: the two gates measure different things
		// and are tuned by different settings, so a reading that cannot say
		// which one fired sends you to the wrong slider.
		if (floating && settings.NoCarveFloatingActors) {
			stampStats.floating++;
			// A hover verdict cast by feet alone is the frozen-foot signature
			// (foot z stuck, actor gone): dry travel accrues so the watchdog
			// can break out of it. Collision-measured hovering is trusted.
			if (floatingByFeet)
				accumulateDry(dryStep);
			if (probing)
				skeletonProbe.verdict = floatingByFeet ? "gated: FLOATING (measured by feet)" :
				                                         "gated: FLOATING (measured by collision/limbs)";
			return;
		}
		if (incorporeal) {
			stampStats.incorporeal++;
			if (probing)
				skeletonProbe.verdict = "gated: incorporeal (translucent/ghost)";
			return;
		}
		// A ghost's see-through look arrives by SCRIPT, frames after its model
		// loads, and a carve is permanent until the refill buries it - so the
		// stamps laid before the first honest alpha reading leave a trench no
		// ghost should own. Re-reading faster (the previous fix) cannot close
		// that window; not stamping until the readings settle does. Costs any
		// newly seen actor its first second of prints, which nobody standing
		// a hundred metres away has ever been close enough to miss.
		const bool alphaGoverns = !elemental &&
		                          (settings.IncorporealMode == 1 || settings.IncorporealMode == 3);
		if (!isDead && alphaGoverns && bones && bones->alphaSettle < kBodyAlphaSettleReads) {
			stampStats.incorporeal++;
			if (probing)
				skeletonProbe.verdict = "gated: alpha settling (newly seen actor)";
			return;
		}

		// S1: an actor inside the contact window carves by its skinned mesh
		// instead of by bones. Placed AFTER the floating, incorporeal and
		// settle gates, so a ghost is still refused; and it skips the bone
		// stamps outright, so the A/B compares like for like. The living and
		// the dead draw from separate budgets: a corpse is drawn while its
		// body translates and for the settle window after, then latched out
		// on the same rest state the bone corpse path uses; a latched corpse
		// takes NO path - its print is already in the map - until the body
		// moves off its resting place again.
		if (debugActorContact && !contactShadersFailed) {
			const auto& bound = root->worldBound;
			const bool budget = isDead ? contactCorpseCount < kContactMaxCorpses : contactLivingCount < kContactMaxActors;
			if (budget && bound.radius > 0.0f &&
				std::abs(bound.center.x - contactCenter.x) + bound.radius < kContactHalfExtent &&
				std::abs(bound.center.y - contactCenter.y) + bound.radius < kContactHalfExtent) {
				bool draw = true;
				if (isDead && rest) {
					const RE::NiPoint3 center = bound.center;
					if (rest->hasPrevCenter) {
						const float step = center.GetDistance(rest->prevCenter);
						if (step >= kFootDryTeleport) {
							// A cell load or a physics wake moved the body wholesale:
							// a new placement, printed afresh.
							rest->settled = false;
							rest->stillFrames = 0;
						} else if (rest->settled) {
							if (center.GetDistance(rest->restCenter) > kCorpseRasterWake) {
								rest->settled = false;
								rest->stillFrames = 0;
							}
						} else if (step > kCorpseStillSpeed) {
							rest->stillFrames = 0;
						} else if (++rest->stillFrames >= kCorpseSettleFrames) {
							rest->settled = true;
							rest->restCenter = center;
						}
					}
					rest->prevCenter = center;
					rest->hasPrevCenter = true;
					draw = !rest->settled;
				}
				if (draw) {
					contactActors.push_back({ actor->CreateRefHandle(),
						bound.center.x - bound.radius, bound.center.y - bound.radius,
						bound.center.x + bound.radius, bound.center.y + bound.radius, isDead,
						groundZ, nominalDepth });
					if (isDead) {
						contactCorpseCount++;
						stampStats.corpsesRasterized++;
					} else {
						contactLivingCount++;
						stampStats.actorsRasterized++;
					}
				}
				return;
			}
		}

		// Living actors need matched USABLE feet to take the bone path: a
		// limbs-only match (creature spines/necks) would steal the
		// collision-shape fallback while its high segments carve nothing, and
		// so would feet an editor broke or a watchdog already gave up on.
		if (footPath && bones) {
			if (probing)
				skeletonProbe.verdict = "carving: bone path";
			bool footStamped = false;
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
					if (stampCount >= actorCeiling)
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
						footRadiusFloor, kMaxStampShapeRadius);

					// Absence from the trail map is the lifted latch: a foot in
					// swing phase drops out, so its next plant starts a fresh
					// discrete print instead of dragging from the previous one.
					// The band releases at 1.5x for a foot that stamped last
					// frame: an idle foot hovering AT the band flickers its
					// stamp in and out every frame or two, and each flicker is
					// a real map input. A swing lift clears 1.5x instantly, so
					// the trail-break latch is unaffected.
					const float plantRef = groundRefStarved ? minFootZ : groundZ;
					const float plantBand = groundRefStarved ?
					                            std::max(kFootRelativeBand * boneScale, kFootRelativeLenFactor * footLen) :
					                            kFootPlantBand * boneScale;
					const uint64_t key = (uint64_t(formID) << 16) | (kFootKeyBit | uint64_t(thisIndex & 0x7FFF));
					const bool wasPlanted = stampPrevPositions.find(key) != stampPrevPositions.end();
					const float releaseBand = plantBand * (wasPlanted ? 1.5f : 1.0f);
					const bool planted = footWorld.translate.z - plantRef <= releaseBand;
					if (probing && thisIndex < skeletonProbe.feet.size()) {
						auto& row = skeletonProbe.feet[thisIndex];
						row.zAboveRef = footWorld.translate.z - plantRef;
						row.band = releaseBand;
						row.planted = planted;
						row.radius = radius;
					}
					if (!planted)
						continue;

					// A continuously planted heel can still slide (shuffles,
					// slopes); the capsule then covers drag plus foot length.
					float2 segStart = heel;
					auto it = stampPrevPositions.find(key);
					if (it != stampPrevPositions.end()) {
						float2 delta = { heel.x - it->second.x, heel.y - it->second.y };
						if (delta.x * delta.x + delta.y * delta.y < kTrailBreakDistance * kTrailBreakDistance)
							segStart = it->second;
					}
					currentPositions[key] = heel;

					// Only the LEADING foot pushes. The trailing foot is
					// standing in the trench its owner already dug, so there
					// is nothing left there to shoulder - that was the "wave
					// behind the character". Emitted HERE, after segStart, so
					// the crest rides the same swept CAPSULE the trench stamp
					// uses rather than radiating from a point: a point source
					// threw its whole radius ahead of wherever the foot was,
					// which bulged the far wall of a trench before the foot
					// had crossed it.
					const float footAhead = (tip.x - position.x) * bowWaveDir.x +
					                        (tip.y - position.y) * bowWaveDir.y;
					if (bowWaveStrength > 0.02f && footAhead > -2.0f &&
						(bowWaveDir.x != 0.0f || bowWaveDir.y != 0.0f) &&
						bowWaves.size() < kMaxBowWaves) {
						BowWave wave{};
						wave.pos = tip;
						wave.prev = segStart;
						wave.dir = bowWaveDir;
						wave.radius = kBowWaveBaseRadius * depthScale;
						wave.strength = bowWaveStrength;
						const float cdx = footWorld.translate.x - cameraPosition.x;
						const float cdy = footWorld.translate.y - cameraPosition.y;
						wave.distSq = cdx * cdx + cdy * cdy;
						bowWaves.push_back(wave);
					}

					float4 stamp{};
					stamp.x = tip.x;
					stamp.y = tip.y;
					stamp.z = 1.0f;
					stamp.w = radius;
					perFrameData.Stamps[stampCount] = stamp;
					// StampEnds.w on a carve is the weight it puts through a
					// crust: a boot prints shallow on ice, a mammoth goes
					// through it. Radius stands in for mass - the shapes a
					// heavy skeleton carries are simply bigger.
					perFrameData.StampEnds[stampCount] = { segStart.x, segStart.y, 0.0f,
						CrustBreakForce(radius) };
					stampCount++;
					stampStats.feet++;
					footStamped = true;
					if (probing && thisIndex < skeletonProbe.feet.size())
						skeletonProbe.feet[thisIndex].stamped = true;
				}
			}

			// Limb segments carve to the fraction their underside reaches
			// into the nominal snow layer: wading legs connect the prints in
			// deep snow, shallow snow keeps prints discrete. No trail latch:
			// per-frame segment stamps stay continuous at any speed.
			for (auto& limb : bones->limbs) {
				if (stampCount >= actorCeiling)
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
				if (LimbStretched(limb, aWorld.translate.GetDistance(bWorld.translate), limb.radius * boneScale))
					continue;
				const float radius = std::clamp(limb.radius * boneScale * depthScale,
					limbRadiusFloor, kMaxStampShapeRadius);
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
				perFrameData.StampEnds[stampCount] = { aWorld.translate.x, aWorld.translate.y, 0.0f,
					CrustBreakForce(radius) };
				stampCount++;
				stampStats.limbs++;
				if (probing)
					skeletonProbe.limbsStamped++;
			}
			if (footStamped) {
				cache.dryTravel = 0.0f;
				cache.dryRecollected = false;
			} else {
				accumulateDry(dryStep);
			}
			return;
		}

		if (!isDead && cache.collisionFallback)
			stampStats.fallbackActors++;
		if (probing) {
			if (isDead)
				skeletonProbe.verdict = "corpse path";
			else if (cache.collisionFallback)
				skeletonProbe.verdict = "carving: collision shapes (failsafe latched)";
			else if (cache.feet.empty())
				skeletonProbe.verdict = "carving: collision shapes (no matched feet)";
			else
				skeletonProbe.verdict = "carving: collision shapes (feet unusable)";
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
			for (auto& limb : bones->limbs) {
				const uint32_t thisIndex = limbIndex++;
				if (stampCount >= actorCeiling)
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
				if (LimbStretched(limb, aWorld.translate.GetDistance(bWorld.translate), limb.radius * boneScale))
					continue;
				const float radius = std::clamp(limb.radius * boneScale * depthScale,
					limbRadiusFloor, kMaxStampShapeRadius);
				const RE::NiPoint3 center = (aWorld.translate + bWorld.translate) * 0.5f;

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
					// First sight baselines only; a settled corpse keeps its
					// frozen anchor and stops carving.
					currentPositions[key] = firstSight ? current : it->second;
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
				perFrameData.StampEnds[stampCount] = { aWorld.translate.x, aWorld.translate.y, 0.0f,
					CrustBreakForce(radius) };
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
					if (stampCount >= actorCeiling)
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
						// First sight baselines only; a settled corpse keeps
						// its frozen anchor and stops carving.
						currentPositions[key] = firstSight ? current : it->second;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Unsettled dead stamp every frame, exactly like the living:
					// the snow deforms as the body moves through it.
					currentPositions[key] = current;

					const ShapeStamp shapeStamp = ShapeStampFor(a_object, current, previous, radius, debugShapeFootprint);
					// Carve to the fraction the underside reaches into the layer,
					// as limbs do: a shape hovering over the shell must not print
					// to the ground. After the trail and settle bookkeeping above,
					// so a hovering shape keeps its anchor.
					const float carve = std::clamp(
						1.0f - (centerPos.z - shapeStamp.halfHeight - bandRefZ) / nominalDepth, 0.0f, 1.0f);
					if (carve < kMinLimbCarve)
						return RE::BSVisit::BSVisitControl::kContinue;
					float4 stamp{};
					stamp.x = shapeStamp.a.x;
					stamp.y = shapeStamp.a.y;
					stamp.z = carve;
					// StampRadius scales the shape's own radius; the crust reads the
					// bound sphere, the mass proxy, unchanged.
					stamp.w = std::max(shapeStamp.radius * settings.StampRadius / kStampRadiusNeutral * depthScale,
						texelFloor);
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { shapeStamp.b.x, shapeStamp.b.y, 0.0f,
						CrustBreakForce(radius) };
					stampCount++;
					stampStats.shapes++;
					if (probing)
						skeletonProbe.shapes++;
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
	// One frame of lag: the probe follows whoever ended THIS gather nearest,
	// and fills during the next, so the pick is settled before any row is
	// written.
	skeletonProbeTarget = stampStats.nearestValid ? stampStats.nearestFormID : 0;
	globals::profiler->EndPass();

	// The reference scan, timed apart from the actor walk above: it visits
	// every reference in range each frame whether or not anything moves.
	globals::profiler->BeginPass("SnowDeformation::GatherProps");
	// Loose props carve while moving. The cheap root-position gate runs
	// before any collision traversal. The reference scan itself is the cost
	// (0.31 ms to visit 144 refs and find 0 movers, measured), so it runs
	// every kPropScanInterval frames; between scans only the movers and
	// hazards it found are revisited, and anchors update in place rather
	// than being rebuilt. Hazards replay every frame from the cache because
	// the emitter list is cleared per frame.
	std::unordered_map<uint32_t, RE::NiPoint3> currentPropPositions;
	const auto tes = RE::TES::GetSingleton();
	auto* playerRef = RE::PlayerCharacter::GetSingleton();
	// Hazards present means a wall or rune spell is live: those spawn a
	// SERIES of hazard references as they spread, and a segment placed
	// between scans is invisible until the next one - the wall's growing
	// edge flickered. Scan every frame while any hazard stands; spell
	// combat is brief, and the throttle holds for the rest of play.
	const bool fullScan = propScanFrame == 0 || propPrevPositions.empty() || !propScanHazards.empty();
	propScanFrame = (propScanFrame + 1) % kPropScanInterval;
	auto considerProp = [&](RE::TESObjectREFR* a_ref) {
		auto* base = a_ref->GetBaseObject();
		if (!base)
			return;
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
			return;
		}
		if (!a_ref->Is3DLoaded())
			return;
		auto root = a_ref->Get3D(false);
		if (!root)
			return;

		// Gate on the 3D root's world transform, not the reference
		// position: Havok moves the scene graph every frame while the
		// reference position lags until the body settles.
		stampStats.propRefs++;
		const auto position = root->world.translate;
		const uint32_t formID = a_ref->formID;
		// Full scan rebuilds the anchors; between scans they update in place.
		auto& anchors = fullScan ? currentPropPositions : propPrevPositions;
		auto prevIt = propPrevPositions.find(formID);
		if (prevIt == propPrevPositions.end()) {
			anchors[formID] = position;
			return;  // first sight: baseline only
		}
		// Frozen anchor: slow motion accumulates toward the gate instead
		// of resetting every frame.
		const bool propMoved = position.GetSquaredDistance(prevIt->second) >= kStampMovementGate * kStampMovementGate;
		anchors[formID] = propMoved ? position : prevIt->second;
		if (propMoved) {
			stampStats.propMovers++;
			if (fullScan)
				propScanMovers.push_back(a_ref->CreateRefHandle());
		}
		if (!propMoved)
			return;  // at rest: the refill buries it
		if (stampCount >= actorCeiling)
			return;  // keep collecting anchors

		// Fast-falling props must not carve under their arc; supported
		// ones stamp wherever they lie, including on top of statics.
		const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 1e-4f) : 1.0f / 60.0f;
		if ((position.z - prevIt->second.z) / dt < -kFallSpeedGate)
			return;

		float groundZ = position.z;
		tes->GetLandHeight(position, groundZ);
		// Props on ELEVATED surfaces, or simply hanging in the air, do not
		// carve - the same gate the living have obeyed since the walkway
		// rounds, and for the same reason. The band below measures against
		// the prop's own root, so it can never find the prop too high;
		// something dropped on a table or still falling therefore cut the
		// ground a storey beneath it at full depth.
		if (position.z - groundZ > kElevatedStampCutoff)
			return;
		// Inside the contact window a moving prop carves by its own render
		// mesh (DrawContactCapture, per texel in the stamp pass); its
		// collision shapes stay out of the stamp list.
		if (debugContactCapture && !contactShadersFailed) {
			const auto& bound = root->worldBound;
			if (bound.radius > 0.0f &&
				std::abs(bound.center.x - contactCenter.x) + bound.radius < kContactHalfExtent &&
				std::abs(bound.center.y - contactCenter.y) + bound.radius < kContactHalfExtent) {
				contactProps.push_back({ a_ref->CreateRefHandle(),
					bound.center.x - bound.radius, bound.center.y - bound.radius,
					bound.center.x + bound.radius, bound.center.y + bound.radius });
				stampStats.propsRasterized++;
				return;
			}
		}
		// Band reference: whichever is higher, the land or the prop's own
		// root — elevated resting surfaces keep their stamps.
		const float supportZ = std::max(groundZ, position.z);

		const float nominalDepth = std::max(
			GetNominalSnowDepthAt(position.x, position.y, kStampDepthReference), 1.0f);
		const float depthScale = std::clamp(nominalDepth / kStampDepthReference,
			kStampDepthScaleMin, kStampDepthScaleMax);
		uint32_t shapeIndex = 0;
		RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			RE::NiPoint3 centerPos;
			float radius;
			if (Util::GetShapeBound(a_object, centerPos, radius)) {
				const uint32_t thisIndex = shapeIndex++;
				if (stampCount >= actorCeiling)
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

				const ShapeStamp shapeStamp = ShapeStampFor(a_object, current, previous, radius, debugShapeFootprint);
				// Depth from the underside, as limbs do - see the actor site.
				const float carve = std::clamp(
					1.0f - (centerPos.z - shapeStamp.halfHeight - supportZ) / nominalDepth, 0.0f, 1.0f);
				if (carve < kMinLimbCarve)
					return RE::BSVisit::BSVisitControl::kContinue;
				float4 stamp{};
				stamp.x = shapeStamp.a.x;
				stamp.y = shapeStamp.a.y;
				stamp.z = carve;
				stamp.w = std::max(shapeStamp.radius * settings.StampRadius / kStampRadiusNeutral * depthScale, kMinPropStampRadius);
				perFrameData.Stamps[stampCount] = stamp;
				perFrameData.StampEnds[stampCount] = { shapeStamp.b.x, shapeStamp.b.y, 0.0f,
					CrustBreakForce(radius) };
				stampCount++;
				stampStats.props++;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		// Shape types with no bound extractor (MOPP/list): one stamp from
		// the root's bound sphere.
		if (shapeIndex == 0 && stampCount < actorCeiling) {
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

				const float carve = std::clamp(
					1.0f - (bound.center.z - radius - supportZ) / nominalDepth, 0.0f, 1.0f);
				if (carve >= kMinLimbCarve) {
					float4 stamp{};
					stamp.x = current.x;
					stamp.y = current.y;
					stamp.z = carve;
					stamp.w = std::max(radius * settings.StampRadius / kStampRadiusNeutral * depthScale, kMinPropStampRadius);
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { previous.x, previous.y, 0.0f,
						CrustBreakForce(radius) };
					stampCount++;
					stampStats.props++;
				}
			}
		}
		return;
	};
	if (tes && playerRef && fullScan) {
		propScanMovers.clear();
		propScanHazards.clear();
		tes->ForEachReferenceInRange(playerRef, 0.5f * deformWorldSize, [&](RE::TESObjectREFR* a_ref) {
			if (!a_ref || a_ref->As<RE::Actor>())
				return RE::BSContainer::ForEachResult::kContinue;
			// Spell walls and runes ride this scan instead of adding one of
			// their own: they are ordinary references and this pass already
			// runs at the right radius. Note the form type is PlacedHazard -
			// Hazard is the base form, and filtering on that silently
			// matches nothing.
			if (a_ref->GetFormType() == RE::FormType::PlacedHazard) {
				ConsiderHazard(a_ref);
				propScanHazards.push_back(a_ref->CreateRefHandle());
				return RE::BSContainer::ForEachResult::kContinue;
			}
			considerProp(a_ref);
			return RE::BSContainer::ForEachResult::kContinue;
		});
		propScanRefs = stampStats.propRefs;
		propPrevPositions = std::move(currentPropPositions);
	} else if (tes && playerRef) {
		for (const auto& handle : propScanHazards)
			if (auto ref = handle.get(); ref)
				ConsiderHazard(ref.get());
		for (const auto& handle : propScanMovers)
			if (auto ref = handle.get(); ref)
				considerProp(ref.get());
	}

	// Spell emitters melt rather than displace. Appended AFTER actors and
	// props on purpose: a busy fight must not starve foot prints out of the
	// stamp budget, and prints are the marks players read first.
	stampStats.beforeSpells = stampCount;

	// Overflow: keep the sources nearest the camera. The producers above run in
	// a fixed order, so a cap enforced while gathering drops whichever ran last
	// rather than whichever is furthest away. Sorted here rather than at the end
	// of GatherSpellEmitters because hazards join during the prop scan above.
	if (spellEmitters.size() > kMaxSpellEmitters) {
		spellStats.emittersCulled = static_cast<uint>(spellEmitters.size() - kMaxSpellEmitters);
		std::partial_sort(spellEmitters.begin(), spellEmitters.begin() + kMaxSpellEmitters, spellEmitters.end(),
			[&](const SpellEmitter& a_lhs, const SpellEmitter& a_rhs) {
				const float lhsDx = a_lhs.position.x - cameraPosition.x, lhsDy = a_lhs.position.y - cameraPosition.y;
				const float rhsDx = a_rhs.position.x - cameraPosition.x, rhsDy = a_rhs.position.y - cameraPosition.y;
				return lhsDx * lhsDx + lhsDy * lhsDy < rhsDx * rhsDx + rhsDy * rhsDy;
			});
		spellEmitters.resize(kMaxSpellEmitters);
	}

	for (const auto& emitter : spellEmitters) {
		if (stampCount >= kMaxStamps)
			break;
		const bool pits = emitter.mark == SpellMark::Pit;
		const bool glazes = emitter.mark == SpellMark::Crust;
		// Force. The only school that DISPLACES snow, so it is the only one
		// that takes the trench path a boot takes - max-blended to a depth
		// rather than approached at a rate - and the only one the berm field
		// answers, since a carve never touches the melted channel it subtracts.
		const bool shoves = emitter.mark == SpellMark::Carve;
		float4 stamp{};
		stamp.x = emitter.position.x;
		stamp.y = emitter.position.y;
		// A pit is thrown to a fixed depth rather than melted toward one, so
		// its strength is the depth itself scaled by how much of the discharge
		// reached the ground.
		stamp.z = pits ? emitter.strength * std::clamp(settings.PitDepth, 0.0f, 1.0f) :
		                 (shoves ? emitter.strength * std::clamp(settings.ForceCarveDepth, 0.0f, 1.0f) :
								   emitter.strength);
		// Crust uses the emitter's OWN reach, like every other mark. Forcing
		// the crust radius here instead threw away whatever the source had
		// worked out for itself - which is why Blizzard glazed 90 units after
		// its record had said 840. The setting still governs the sources that
		// have no reach of their own to state; a frost cloak takes it as its
		// base reach before it ever becomes an emitter.
		// A cone states its own half-width; the pit radius is for discharges
		// that land at a point.
		stamp.w = (pits && !emitter.cone) ? std::max(settings.PitRadius, 4.0f) * emitter.pitScale :
		                                    std::max(emitter.radius, 4.0f);
		perFrameData.Stamps[stampCount] = stamp;
		// Pits carry their ring fraction where a melt carries its rate: an
		// instantaneous mark has no rate to give, and a cloak needs to say it
		// pocks a ring at its reach rather than a bowl at its feet.
		perFrameData.StampEnds[stampCount] = { emitter.previous.x, emitter.previous.y,
			(pits ? kStampModePit : (glazes ? kStampModeCrust : (shoves ? kStampModeCarve : kStampModeMelt))) +
				(emitter.cone ? kStampModeCone : 0.0f) + (emitter.bowl ? kStampModeBowl : 0.0f),
			// A carve reads this as the weight it puts through a crust. A
			// shout goes through: nothing survives being shoved that hard.
			shoves ? 1.0f :
					 (pits ? emitter.ringFraction :
							 (glazes ? std::max(settings.CrustRate, 0.0f) * emitter.rateScale : emitter.rate)) };
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
	// Nearest crests win the slots: a crowd cannot each carry one, and the
	// ones the player can see are the ones worth keeping.
	if (bowWaves.size() > 1)
		std::sort(bowWaves.begin(), bowWaves.end(),
			[](const BowWave& a, const BowWave& b) { return a.distSq < b.distSq; });
	perFrameData.StampCount = stampCount;
	globals::profiler->EndPass();
}
