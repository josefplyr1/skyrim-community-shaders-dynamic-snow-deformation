// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/HitchSampler.h"

#include "Features/SnowDeformation/AlphaBuild.h"

#include "Globals.h"
#include "State.h"
#include "Features/SnowDeformation/ShapeBounds.h"
#include "Utils/ActorUtils.h"
#include "Utils/Game.h"

static constexpr float kStampSurfaceBand = 40.0f;
static constexpr float kMinStampShapeRadius = 4.0f;
static constexpr float kMaxStampShapeRadius = 128.0f;
static constexpr float kStampRadius = 10.0f;
static constexpr float kStampRadiusNeutral = 20.0f;
static constexpr float kTrailBreakDistance = 256.0f;
static constexpr float kStampMovementGate = 3.0f;

static bool PropBodyAwake(RE::NiAVObject* a_root)
{
	bool awake = false;
	RE::BSVisit::TraverseScenegraphCollision(a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
		auto* body = a_object ? a_object->body.get() : nullptr;
		auto* bhkRigid = body ? body->AsBhkRigidBody() : nullptr;
		auto* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
		if (!hkpRigid)
			return RE::BSVisit::BSVisitControl::kContinue;
		awake = hkpRigid->simulationIsland && hkpRigid->simulationIsland->isInActiveIslandsArray;
		return RE::BSVisit::BSVisitControl::kStop;
	});
	return awake;
}
static constexpr float kCorpseWakeDistance = 50.0f;
static constexpr uint16_t kCorpseSettleFrames = 90;
static constexpr float kCorpseStillSpeed = 0.5f;
static constexpr float kCorpseRasterWake = 12.0f;
static constexpr float kStampDepthReference = 30.0f;
static constexpr float kStampDepthScaleMin = 0.65f;
static constexpr float kStampDepthScaleMax = 1.2f;
static constexpr float kFootPlantBand = 15.0f;
static constexpr float kFootRelativeBand = 10.0f;
static constexpr float kFootRelativeLenFactor = 0.6f;
static constexpr float kFootToeExtend = 0.2f;
static constexpr float kFootWidthRatio = 0.22f;
static constexpr float kFootPrintScale = 1.0f;
static constexpr float kHoofRadius = 7.0f;
static constexpr float kMinFootStampRadius = 5.0f;
static constexpr float kFootDryRecollect = 150.0f;
static constexpr float kFootDryFallback = 300.0f;
static constexpr float kFootDryTeleport = 200.0f;
static constexpr uint16_t kFootAttachRecheckFrames = 60;
static constexpr uint16_t kBoneCollectSettleFrames = 15;
static constexpr int kBoneWalkMaxDepth = 32;
static constexpr uint64_t kFootKeyBit = 0x8000;
static constexpr uint64_t kLimbKeyBit = 0x4000;
static constexpr float kMinLimbCarve = 0.05f;
static constexpr float kMinPropStampRadius = 5.0f;
static constexpr float kFallSpeedGate = 300.0f;
static constexpr float kImpactSpeedSpan = 600.0f;
static constexpr float kImpactSpeedMax = 0.75f;
static constexpr float kImpactFallFloor = 250.0f;
static constexpr float kImpactFallSpan = 600.0f;
static constexpr float kImpactStampMin = 0.03f;
static constexpr float kImpactTrackGap = 0.25f;
static constexpr float kSpoilSpeedSpan = 300.0f;
static constexpr float kSpoilKickSeconds = 0.15f;
static constexpr float kSpoilSpreadCos = 0.906f;
static constexpr float kSpoilSpreadSin = 0.423f;
static constexpr float kSpoilArmWidth = 0.3f;
static constexpr float kMaxLimbEndpointDistance = 1024.0f;
static constexpr float kLimbStretchGate = 2.0f;
static constexpr float kLimbStretchFloor = 24.0f;
static constexpr float kLimbAspectGate = 10.0f;
static constexpr float kIncorporealAlpha = 0.95f;
static constexpr uint16_t kBodyAlphaRecheckFrames = 30;
static constexpr uint16_t kBodyAlphaSettleReads = 12;
static constexpr uint16_t kBodyAlphaSettleFrames = 5;
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
	const bool prefilter = needleLen && a_needle[0] != 'i' && a_needle[0] != 'I';
	const char firstFolded = (char)(a_needle[0] | 0x20);
	for (const char* p = hay; *p; ++p) {
		if (prefilter && (char)(*p | 0x20) != firstFolded)
			continue;
		if (_strnicmp(p, a_needle, needleLen) == 0)
			return true;
	}
	return false;
}

static bool NameStartsWith(const RE::BSFixedString& a_name, const char* a_prefix)
{
	const char* hay = a_name.c_str();
	return hay && _strnicmp(hay, a_prefix, strlen(a_prefix)) == 0;
}

static bool NodeAttachedTo(const RE::NiAVObject* a_node, const RE::NiAVObject* a_root)
{
	for (auto* p = a_node; p; p = p->parent)
		if (p == a_root)
			return true;
	return false;
}

struct FootPrint
{
	float2 heel;
	float2 tip;
	float radius;
	float length;
};

static FootPrint FootPrintOf(RE::NiAVObject* a_foot, RE::NiAVObject* a_toe, float a_depthScale, float a_radiusFloor)
{
	const auto& footWorld = a_foot->world;
	const float boneScale = footWorld.scale;
	FootPrint print{};
	print.heel = { footWorld.translate.x, footWorld.translate.y };
	print.tip = print.heel;
	print.length = 2.0f * kHoofRadius * boneScale;
	float radius = kHoofRadius * boneScale;
	if (auto* parentNode = a_foot->parent) {
		const float parentDist = footWorld.translate.GetDistance(parentNode->world.translate);
		print.length = std::max(print.length, parentDist);
		radius = std::max(radius, 0.2f * parentDist);
	}
	if (a_toe) {
		const auto& toePos = a_toe->world.translate;
		float2 dir = { toePos.x - print.heel.x, toePos.y - print.heel.y };
		const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
		if (len > 2.0f) {
			const float extend = 1.0f + kFootToeExtend;
			print.tip = { print.heel.x + dir.x * extend, print.heel.y + dir.y * extend };
			radius = len * extend * kFootWidthRatio;
			print.length = len * extend;
		}
	}
	print.radius = std::clamp(radius * kFootPrintScale * a_depthScale, a_radiusFloor, kMaxStampShapeRadius);
	return print;
}

struct FootPlantRef
{
	float z;
	bool relative;
};

static FootPlantRef FootPlantRefOf(const SnowDeformation::StampBones& a_bones, float a_groundZ)
{
	float minFootZ = FLT_MAX;
	float minFootScale = 1.0f;
	for (const auto& foot : a_bones.feet)
		if (auto* n = foot.node.get(); n && n->world.scale >= 0.01f && n->world.translate.z < minFootZ) {
			minFootZ = n->world.translate.z;
			minFootScale = n->world.scale;
		}
	const bool relative = minFootZ != FLT_MAX && minFootZ - a_groundZ > kFootPlantBand * minFootScale;
	return { relative ? minFootZ : a_groundZ, relative };
}

static float FootPlantBand(const FootPlantRef& a_ref, float a_boneScale, float a_footLength)
{
	return a_ref.relative ? std::max(kFootRelativeBand * a_boneScale, kFootRelativeLenFactor * a_footLength) :
	                        kFootPlantBand * a_boneScale;
}

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

static bool PlausibleNode(const RE::NiAVObject* a_obj)
{
	const auto p = reinterpret_cast<uintptr_t>(a_obj);
	return p >= 0x10000 && (p & 7) == 0;
}

static RE::NiAVObject* FindToeBone(RE::NiNode* a_node, int a_depth = 0)
{
	if (a_depth > kBoneWalkMaxDepth)
		return nullptr;
	auto& children = a_node->GetChildren();
	const auto count = children.free_idx();
	for (uint16_t i = 0; i < count; ++i) {
		auto* child = children[i].get();
		auto* node = PlausibleNode(child) ? child->AsNode() : nullptr;
		if (!node)
			continue;
		if (NameContains(node->name, "toe"))
			return node;
		if (auto* deeper = FindToeBone(node, a_depth + 1))
			return deeper;
	}
	return nullptr;
}

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

static __declspec(noinline) void CollectStampBones(RE::NiAVObject* a_obj, RE::NiAVObject* a_ancestor, float a_ancestorRadius,
	SnowDeformation::StampBones& a_out, int a_depth)
{
	auto* node = PlausibleNode(a_obj) ? a_obj->AsNode() : nullptr;
	if (!node || a_depth > kBoneWalkMaxDepth)
		return;
	const auto& name = node->name;
	const bool controlNode = NameStartsWith(name, "CME ") || NameStartsWith(name, "MOV ") ||
	                         GearNodeName(name);
	if (!controlNode &&
		(NameContains(name, "foot") || NameContains(name, "hoof") || NameContains(name, "paw"))) {
		if (a_out.feet.size() < kMaxCachedFeet)
			a_out.feet.push_back({ RE::NiPointer<RE::NiAVObject>(node), RE::NiPointer<RE::NiAVObject>(FindToeBone(node)) });
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
			auto& children = node->GetChildren();
			const auto count = children.free_idx();
			for (uint16_t i = 0; i < count; ++i)
				CollectStampBones(children[i].get(), node, spec->radius, a_out, a_depth + 1);
			return;
		}
	}
	auto& children = node->GetChildren();
	const auto count = children.free_idx();
	for (uint16_t i = 0; i < count; ++i)
		CollectStampBones(children[i].get(), a_ancestor, a_ancestorRadius, a_out, a_depth + 1);
}

static bool TryCollectStampBones(RE::NiAVObject* a_root, SnowDeformation::StampBones* a_out)
{
	__try {
		CollectStampBones(a_root, nullptr, 0.0f, *a_out, 0);
		return true;
	} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		return false;
	}
}

static uint32_t boneWalkFaultsLogged = 0;

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
		std::string boneTable;
		if (auto* si = runtime.skinInstance.get()) {
			uint32_t parts = si->skinPartition ? si->skinPartition->numPartitions : 0;
			uint32_t bones = si->skinData ? si->skinData->GetBoneCount() : 0;
			skin = std::format("skinned {} partitions / {} bones, {} matrices", parts, bones, si->numMatrices);
			uint32_t nullNodes = 0, nullPointers = 0;
			for (uint32_t b = 0; b < bones && b < 96; ++b) {
				auto* node = si->bones ? si->bones[b] : nullptr;
				const bool ptr = si->boneWorldTransforms && si->boneWorldTransforms[b];
				if (!node)
					nullNodes++;
				if (!ptr)
					nullPointers++;
				if (!node || !ptr)
					boneTable += std::format("[{}: node {} / ptr {}] ", b, node ? (node->name.c_str() ? node->name.c_str() : "?") : "NULL", ptr ? "ok" : "NULL");
			}
			boneTable = std::format(" | null nodes {} / null pointers {} of {}: ", nullNodes, nullPointers, bones) + boneTable;
			if (auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(si)) {
				const auto& rd = dismember->GetRuntimeData();
				boneTable += " | dismember: ";
				for (int32_t p = 0; rd.partitions && p < rd.numPartitions; ++p)
					boneTable += std::format("[p{} slot {} {}] ", p, rd.partitions[p].slot, rd.partitions[p].editorVisible ? "visible" : "HIDDEN");
			}
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
		if (!boneTable.empty())
			logger::info("[SNOW DEFORMATION] geom '{}' bones{}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", boneTable);
		return RE::BSVisit::BSVisitControl::kContinue;
	});
}

bool SnowDeformation::IsProxyOrOverlayGeometry(const RE::BSGeometry* a_geometry, const RE::BSShaderProperty* a_property)
{
	if (a_geometry && (NameContains(a_geometry->name, "Virtual") ||
	                      NameContains(a_geometry->name, "[Ovl") || NameContains(a_geometry->name, "[SOvl")))
		return true;
	return a_property && a_property->alpha < 0.01f;
}

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

	bool flagged = false;
	if (const auto* base = a_actor->GetActorBase())
		flagged = base->actorData.actorBaseFlags.all(RE::ACTOR_BASE_DATA::Flag::kIsGhost);
	if (a_flagOut)
		*a_flagOut = flagged;

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
			float highest = 0.0f, highestAny = 0.0f, lowestAny = 1.0f;
			RE::BSFixedString highestName, highestAnyName, lowestAnyName;
			bool bodySeen = false, anySeen = false;
			auto visitBody = [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
				auto& runtime = a_geometry->GetGeometryRuntimeData();
				if (!runtime.skinInstance)
					return RE::BSVisit::BSVisitControl::kContinue;
				auto* property = runtime.shaderProperty.get();
				if (!property || property->GetRTTI() != globals::rtti::BSLightingShaderPropertyRTTI.get())
					return RE::BSVisit::BSVisitControl::kContinue;
				const float a = static_cast<RE::BSLightingShaderProperty*>(property)->alpha;
				if (!anySeen || a < lowestAny) {
					lowestAny = a;
					lowestAnyName = a_geometry->name;
				}
				if (!anySeen || a > highestAny) {
					highestAny = a;
					highestAnyName = a_geometry->name;
				}
				anySeen = true;
				if (IsProxyOrOverlayGeometry(a_geometry, property))
					return RE::BSVisit::BSVisitControl::kContinue;
				if (!bodySeen || a > highest) {
					highest = a;
					highestName = a_geometry->name;
				}
				bodySeen = true;
				return RE::BSVisit::BSVisitControl::kContinue;
			};
			RE::BSVisit::TraverseScenegraphGeometries(a_root, std::ref(visitBody));
			if (!bodySeen) {
				highest = anySeen ? highestAny : 1.0f;
				highestName = highestAnyName;
			}
			alpha = highest;
			if (a_bones) {
				a_bones->bodyAlpha = highest;
				a_bones->bodyAlphaSource = highestName;
				a_bones->anyAlpha = anySeen ? lowestAny : 1.0f;
				a_bones->anyAlphaSource = lowestAnyName;
				const bool settling = a_bones->alphaSettle < kBodyAlphaSettleReads;
				a_bones->alphaRecheck = settling ? kBodyAlphaSettleFrames : kBodyAlphaRecheckFrames;
				if (settling && ++a_bones->alphaSettle == kBodyAlphaSettleReads)
					a_bones->alphaRecheck += static_cast<uint16_t>((reinterpret_cast<uintptr_t>(a_root) >> 4) % kBodyAlphaRecheckFrames);
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
	const bool measuredByFeet = lowest != FLT_MAX && a_bones && a_useFeet && !a_bones->feet.empty();
	if (a_byFeetOut)
		*a_byFeetOut = measuredByFeet;
	if (!measuredByFeet && a_root)
		SnowDeformation::TimedTraverse(a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			RE::NiPoint3 centerPos;
			float radius;
			if (SnowShapes::GetShapeBound(a_object, centerPos, radius))
				lowest = std::min(lowest, centerPos.z - radius);
			return RE::BSVisit::BSVisitControl::kContinue;
		});

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

struct ShapeStamp
{
	float2 a;
	float2 b;
	float radius;
	float halfHeight;
};
static ShapeStamp ShapeStampFor(RE::bhkNiCollisionObject* a_object, float2 a_current, float2 a_previous,
	float a_boundRadius)
{
	ShapeStamp out{ a_current, a_previous, a_boundRadius, a_boundRadius };
	float ax, ay, halfLen, halfWid, halfHgt;
	if (!SnowShapes::GetShapeFootprint(a_object, ax, ay, halfLen, halfWid, halfHgt))
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
	globals::profiler->BeginPass("SnowDeformation::Trails - Gather & Contact");
	uint stampCount = 0;
	const uint actorCeiling = kMaxStamps - kSpellStampReserve;
	float nearestDistSq = FLT_MAX;
	RE::NiPoint3 cameraPosition = Util::GetEyePosition();
	auto* const playerActor = globals::game::player;
	const float windowReach = 0.5f * deformWorldSize;
	const float actorReach = std::min(windowReach, std::max(settings.RangeActorTrenchesM, 0.0f) * kUnitsPerMeter);
	const float actorReachSq = actorReach * actorReach;
	std::unordered_map<uint64_t, float2> currentPositions;
	std::unordered_set<uint32_t> budgetCutRefs;
	stampStats = {};
	for (auto& cone : perFrameData.StampCones)
		cone = { 0.0f, 0.0f, 0.0f, 0.0f };

	const float texelFloor = deformWorldSize / (float)deformMapDim;
	const float footRadiusFloor = std::max(kMinFootStampRadius, texelFloor);
	const float limbRadiusFloor = std::max(kMinStampShapeRadius, texelFloor);
	const bool probeActive = debugSkeletonProbe && skeletonProbeTarget != 0;
	skeletonProbe.valid = false;
	contactProps.clear();
	contactActors.clear();
	contactCorpseCount = 0;
	contactCenter = { windowOrigin.x + deformWorldSize * 0.5f, windowOrigin.y + deformWorldSize * 0.5f };

	contactPriority.clear();
	if (!contactShadersFailed) {
		struct Nearby
		{
			float d2;
			uint32_t id;
		};
		static std::vector<Nearby> nearby;
		nearby.clear();
		auto consider = [&](RE::Actor* a_actor, float a_d2) {
			if (!a_actor || a_d2 > actorReachSq || a_actor->IsDead() || !a_actor->Is3DLoaded())
				return;
			auto* root = a_actor->Get3D(false);
			if (!root)
				return;
			const auto& b = root->worldBound;
			if (b.radius <= 0.0f ||
				std::abs(b.center.x - contactCenter.x) + b.radius >= kContactHalfExtent ||
				std::abs(b.center.y - contactCenter.y) + b.radius >= kContactHalfExtent)
				return;
			nearby.push_back({ a_d2, a_actor->formID });
		};
		if (auto player = globals::game::player)
			consider(player, -1.0f);
		if (const auto processLists = RE::ProcessLists::GetSingleton())
			for (auto& handle : processLists->highActorHandles)
				if (auto actor = handle.get(); actor)
					consider(actor.get(), cameraPosition.GetSquaredDistance(actor->GetPosition()));
		std::sort(nearby.begin(), nearby.end(), [](const Nearby& a_a, const Nearby& a_b) { return a_a.d2 < a_b.d2; });
		for (size_t i = 0; i < nearby.size() && contactPriority.size() < kContactMaxActors; i++)
			contactPriority.push_back(nearby[i].id);
	}
	auto prioritized = [&](uint32_t a_id) {
		return std::find(contactPriority.begin(), contactPriority.end(), a_id) != contactPriority.end();
	};

	auto addStamps = [&](RE::ActorHandle a_handle) {
		auto actor = a_handle.get();
		if (!actor || !actor->Is3DLoaded())
			return;
		auto position = actor->GetPosition();
		const float reach = actor.get() == playerActor ? windowReach : actorReach;
		if (cameraPosition.GetSquaredDistance(position) > reach * reach)
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
		if (stampCount >= actorCeiling) {
			budgetCutRefs.insert(formID);
			stampStats.budgetTurnedAway++;
			if (probing)
				skeletonProbe.verdict = "gated: stamp budget exhausted";
			return;
		}
		const bool isDead = actor->IsDead();

		CorpseRest* rest = nullptr;
		if (isDead) {
			if (corpseRestStates.size() > 512 && !corpseRestStates.contains(formID))
				corpseRestStates.clear();
			rest = &corpseRestStates[formID];
		} else {
			corpseRestStates.erase(formID);
		}
		bool anyShapeMoved = false;
		bool anyShapeWoken = false;

		bool contactStarved = false;
		if (!isDead)
			if (auto it = stampBoneCache.find(formID); it != stampBoneCache.end())
				contactStarved = it->second.contactStarved > 0;
		const bool rasterCandidate = !contactShadersFailed && !contactStarved &&
		                             (isDead ? contactCorpseCount < kContactMaxCorpses : prioritized(formID)) &&
		                             root->worldBound.radius > 0.0f &&
		                             std::abs(root->worldBound.center.x - contactCenter.x) + root->worldBound.radius < kContactHalfExtent &&
		                             std::abs(root->worldBound.center.y - contactCenter.y) + root->worldBound.radius < kContactHalfExtent;
		if (probing)
			skeletonProbe.rasterCandidate = rasterCandidate;

		auto* charController = actor->GetCharController();
		float footImpact = 0.0f;
		float knockImpact = 0.0f;
		bool landed = false;
		RE::NiPoint3 impactFrom = position;
		float spoilSpeed = 0.0f;
		if (!isDead) {
			if (impactTracks.size() > 512 && !impactTracks.contains(formID))
				impactTracks.clear();
			auto& track = impactTracks[formID];
			const float dt = static_cast<float>(passClockSeconds - track.time);
			if (!track.seen || dt > 1e-4f) {
				const bool inAir = charController && (charController->context.currentState == RE::hkpCharacterStateType::kInAir ||
														 charController->context.currentState == RE::hkpCharacterStateType::kJumping ||
														 charController->context.currentState == RE::hkpCharacterStateType::kFlying);
				if (track.seen && dt < kImpactTrackGap && position.GetDistance(track.prev) < kFootDryTeleport) {
					impactFrom = track.prev;
					const float ground = std::hypot(position.x - track.prev.x, position.y - track.prev.y) / dt;
					track.speed += (ground - track.speed) * (1.0f - std::exp(-dt / kImpactSpeedSmoothing));
					if (inAir)
						track.fall = std::max(track.fall, (track.prev.z - position.z) / dt);
				} else {
					track.speed = 0.0f;
					track.fall = 0.0f;
					track.wasInAir = inAir;
				}
				float landing = 0.0f;
				track.landingSpeed = 0.0f;
				if (track.wasInAir && !inAir) {
					landing = std::clamp((track.fall - kImpactFallFloor) / kImpactFallSpan, 0.0f, 1.0f);
					track.landingSpeed = track.fall;
					track.fall = 0.0f;
				}
				const auto knock = actor->AsActorState()->GetKnockState();
				const bool down = knock == RE::KNOCK_STATE_ENUM::kDown || knock == RE::KNOCK_STATE_ENUM::kGetUp;
				track.speedEnergy = inAir ? 0.0f : std::clamp((track.speed - kImpactSpeedFloor) / kImpactSpeedSpan, 0.0f, kImpactSpeedMax);
				track.landingEnergy = inAir ? 0.0f : landing;
				track.knockEnergy = !inAir && down ? kImpactKnockedDown : 0.0f;
				if (!inAir) {
					footImpact = std::max(track.speedEnergy, track.landingEnergy);
					knockImpact = track.knockEnergy;
					landed = track.landingEnergy > 0.0f;
					spoilSpeed = track.speed;
				}
				track.prev = position;
				track.time = passClockSeconds;
				track.seen = true;
				track.wasInAir = inAir;
			}
		}
		if (!isDead && !rasterCandidate && charController &&
			(charController->context.currentState == RE::hkpCharacterStateType::kInAir ||
				charController->context.currentState == RE::hkpCharacterStateType::kFlying)) {
			if (probing)
				skeletonProbe.verdict = "gated: airborne (controller in-air/flying)";
			return;
		}

		if (!isDead) {
			float landZ = position.z;
			if (const auto tesLand = globals::game::tes) {
				ScopedTicks _land(cpuCensus.landTicks, cpuCensus.landCalls);
				tesLand->GetLandHeight(position, landZ);
			}
			if (probing)
				skeletonProbe.gapToLand = position.z - landZ;
			if (position.z - landZ > kElevatedStampCutoff) {
				footImpact = 0.0f;
				knockImpact = 0.0f;
				spoilSpeed = 0.0f;
			}
			if (position.z - landZ > kElevatedStampCutoff && !rasterCandidate) {
				if (probing)
					skeletonProbe.verdict = "gated: elevated surface (above land cutoff)";
				return;
			}
		}

		float groundZ = position.z;
		if (isDead)
			if (const auto tesGround = globals::game::tes) {
				ScopedTicks _land(cpuCensus.landTicks, cpuCensus.landCalls);
				tesGround->GetLandHeight(position, groundZ);
			}

		if (rest) {
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
			const float probeDepth = GetNominalSnowDepthAt(position.x, position.y, -10000.0f);
			skeletonProbe.cellBaked = probeDepth > -9999.0f;
			skeletonProbe.shellDepth = skeletonProbe.cellBaked ? probeDepth : -1.0f;
		}


		StampBones* bones = nullptr;
		if (stampBoneCache.size() > 512 && !stampBoneCache.contains(formID))
			stampBoneCache.clear();
		auto& cache = stampBoneCache[formID];
		auto recollectBones = [&] {
			cache.feet.clear();
			cache.limbs.clear();
			cache.attachRecheck = kFootAttachRecheckFrames;
			if (TryCollectStampBones(root, &cache))
				return;
			cache.feet.clear();
			cache.limbs.clear();
			cache.collisionFallback = true;
			if (boneWalkFaultsLogged < 8) {
				boneWalkFaultsLogged++;
				logger::warn("[SNOW DEFORMATION] skeleton walk faulted on '{}' ({:08X}); collision stamping until its 3D reloads",
					actor->GetName() ? actor->GetName() : "", formID);
			}
		};
		if (cache.root.get() != root) {
			cache.root = RE::NiPointer<RE::NiAVObject>(root);
			cache.feet.clear();
			cache.limbs.clear();
			cache.collectDelay = kBoneCollectSettleFrames;
			cache.dryTravel = 0.0f;
			cache.hasPrevPos = false;
			cache.dryRecollected = false;
			cache.collisionFallback = false;
			cache.contactStarved = 0;
			cache.contactStarveLogged = false;
		} else if (cache.contactStarved > 0) {
			cache.contactStarved--;
		}
		if (cache.collectDelay > 0 && --cache.collectDelay == 0)
			recollectBones();

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
		uint usableFeet = 0;
		for (const auto& foot : cache.feet) {
			auto* n = foot.node.get();
			if (!n || n->world.scale < 0.01f)
				continue;
			usableFeet++;
		}
		if (!cache.feet.empty() || !cache.limbs.empty())
			bones = &cache;
		const bool footPath = !isDead && usableFeet > 0 && !cache.collisionFallback;

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
			skeletonProbe.bodyAlphaSource = cache.bodyAlphaSource.c_str() ? cache.bodyAlphaSource.c_str() : "";
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

		bool liftInRagdoll = true;
		float creatureWeight = 0.0f;
		const float bodyLift = isDead ? 0.0f : CreatureLiftOf(formID, &liftInRagdoll, &creatureWeight);
		float floatingGap = 0.0f;
		bool floatingByFeet = false;
		const bool floating = !isDead &&
		                      ActorIsFloating(actor.get(), root, bones, footPath, groundZ + bodyLift, &floatingGap, &floatingByFeet);
		if (probing)
			skeletonProbe.floatingGap = floatingGap;
		float bodyAlpha = 1.0f;
		bool ghostFlag = false;
		const bool elemental = ResolveInnateAura(actor.get()) != nullptr;
		const bool incorporeal = !isDead && !elemental &&
		                         ActorIsIncorporeal(actor.get(), root, bones, &bodyAlpha, &ghostFlag);

		if (!actor->IsPlayerRef()) {
			const float distSq = cameraPosition.GetSquaredDistance(position);
			if (!stampStats.nearestValid || distSq < nearestDistSq) {
				nearestDistSq = distSq;
				float landZ = position.z;
				if (const auto tesNear = globals::game::tes) {
					ScopedTicks _land(cpuCensus.landTicks, cpuCensus.landCalls);
					tesNear->GetLandHeight(position, landZ);
				}
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
				stampStats.nearestBodyAlphaSource = bones && bones->bodyAlphaSource.c_str() ? bones->bodyAlphaSource.c_str() : "";
				stampStats.nearestGhostFlag = ghostFlag;
				stampStats.nearestIncorporeal = incorporeal;
				auto* controller = actor->GetCharController();
				stampStats.nearestState = controller ?
				                              static_cast<uint>(controller->context.currentState) :
				                              0xFFu;
			}
		}

		if (floating && settings.NoCarveFloatingActors && !rasterCandidate) {
			stampStats.floating++;
			if (floatingByFeet)
				accumulateDry(dryStep);
			if (probing)
				skeletonProbe.verdict = floatingByFeet ? "gated: FLOATING (measured by feet)" :
				                                         "gated: FLOATING (measured by collision/limbs)";
			return;
		}
		if (incorporeal) {
			stampStats.incorporeal++;
			if (incorporealLogged.size() < 16 && incorporealLogged.insert(formID).second) {
				const bool byFlag = ghostFlag && settings.IncorporealMode != 1;
				logger::info("[SNOW DEFORMATION] '{}' ({:08X}) leaves no trench: {}", actor->GetDisplayFullName(), formID,
					byFlag ? std::string("marked Ghost on its record") :
					         std::format("most opaque body surface '{}' at alpha {:.2f}",
							 bones && bones->bodyAlphaSource.c_str() ? bones->bodyAlphaSource.c_str() : "", bodyAlpha));
			}
			if (probing)
				skeletonProbe.verdict = "gated: incorporeal (translucent/ghost)";
			return;
		}
		if (!isDead && !elemental && bones && bones->anyAlpha >= 0.0f && bones->anyAlpha < kIncorporealAlpha &&
		    seeThroughKeptLogged.size() < 16 && seeThroughKeptLogged.insert(formID).second) {
			logger::info("[SNOW DEFORMATION] '{}' ({:08X}) keeps its trench: see-through '{}' at alpha {:.2f}, most opaque body surface {:.2f} on '{}'",
				actor->GetDisplayFullName(), formID,
				bones->anyAlphaSource.c_str() ? bones->anyAlphaSource.c_str() : "", bones->anyAlpha,
				bodyAlpha, bones->bodyAlphaSource.c_str() ? bones->bodyAlphaSource.c_str() : "");
		}
		const bool alphaGoverns = !elemental &&
		                          (settings.IncorporealMode == 1 || settings.IncorporealMode == 3);
		if (!isDead && alphaGoverns && bones && bones->alphaSettle < kBodyAlphaSettleReads) {
			stampStats.incorporeal++;
			if (probing)
				skeletonProbe.verdict = "gated: alpha settling (newly seen actor)";
			return;
		}

		if (SnowEventsWanted())
			CollectActorSnowEvents(actor.get(), isDead, position, groundZ, bones, root);

		const float impactWeight = creatureWeight > 0.0f ? std::cbrt(creatureWeight) : 1.0f;
		footImpact = std::min(footImpact * impactWeight, 1.0f);
		knockImpact = std::min(knockImpact * impactWeight, 1.0f);
		const float impactReach = std::clamp(settings.ImpactReach, 0.5f, 4.0f);
		const bool footStrikes = footPath && bones;
		const float bodyImpact = footStrikes ? knockImpact : std::max(footImpact, knockImpact);
		if (bodyImpact > kImpactStampMin && stampCount < actorCeiling) {
			float4 stamp{};
			stamp.x = position.x;
			stamp.y = position.y;
			stamp.z = 0.0f;
			stamp.w = std::clamp(std::clamp(root->worldBound.radius * 0.45f, 16.0f, 120.0f) * impactReach, limbRadiusFloor, kMaxStampShapeRadius);
			perFrameData.Stamps[stampCount] = stamp;
			perFrameData.StampEnds[stampCount] = { impactFrom.x, impactFrom.y, kStampModeCarve + kStampImpactScale * bodyImpact, 0.0f };
			stampCount++;
		}
		if (footStrikes && footImpact > kImpactStampMin) {
			const auto plantRef = FootPlantRefOf(*bones, groundZ + bodyLift);
			for (const auto& foot : bones->feet) {
				if (stampCount >= actorCeiling)
					break;
				auto* footNode = foot.node.get();
				if (!footNode || footNode->world.scale < 0.01f)
					continue;
				const auto print = FootPrintOf(footNode, foot.toe.get(), depthScale, footRadiusFloor);
				if (!landed && footNode->world.translate.z - plantRef.z > FootPlantBand(plantRef, footNode->world.scale, print.length))
					continue;
				perFrameData.Stamps[stampCount] = { print.tip.x, print.tip.y, 0.0f,
					std::clamp(print.radius * impactReach, footRadiusFloor, kMaxStampShapeRadius) };
				perFrameData.StampEnds[stampCount] = { print.heel.x, print.heel.y, kStampModeCarve + kStampImpactScale * footImpact, 0.0f };
				stampCount++;
			}
		}
		if (settings.SpeedSpoil && footPath && bones) {
			if (spoilFeet.size() > 1024)
				spoilFeet.clear();
			const uint32_t spoilFrame = globals::state->frameCount;
			const float spoilEnergy = std::min(std::clamp((spoilSpeed - kImpactSpeedFloor) / kSpoilSpeedSpan, 0.0f, 1.0f) * impactWeight, 1.0f);
			const float spoilTravelX = position.x - impactFrom.x;
			const float spoilTravelY = position.y - impactFrom.y;
			const float spoilTravel = std::sqrt(spoilTravelX * spoilTravelX + spoilTravelY * spoilTravelY);
			const float wedgeLength = spoilSpeed * kSpoilKickSeconds * std::clamp(impactWeight, 0.5f, 2.0f) * std::clamp(settings.SpeedSpoilSize, 0.1f, 2.0f);
			uint32_t spoilIndex = 0;
			for (const auto& spoilFoot : bones->feet) {
				const uint32_t spoilKeyIndex = spoilIndex++;
				auto* spoilNode = spoilFoot.node.get();
				if (!spoilNode || spoilNode->world.scale < 0.01f)
					continue;
				auto& spoilState = spoilFeet[(uint64_t(formID) << 8) | (spoilKeyIndex & 0xFF)];
				const float spoilBand = kFootPlantBand * spoilNode->world.scale * (spoilState.planted ? 1.5f : 1.0f);
				const bool spoilPlanted = spoilNode->world.translate.z - (groundZ + bodyLift) <= spoilBand;
				const bool spoilSeen = spoilState.frame != 0 && spoilFrame - spoilState.frame <= 2;
				if (spoilSeen && spoilState.planted && !spoilPlanted && spoilEnergy > kImpactStampMin && spoilTravel > 1e-3f && stampCount + 2 <= actorCeiling) {
					const float backX = -spoilTravelX / spoilTravel;
					const float backY = -spoilTravelY / spoilTravel;
					for (const float spoilSide : { -1.0f, 1.0f }) {
						const float armX = backX * kSpoilSpreadCos - backY * kSpoilSpreadSin * spoilSide;
						const float armY = backY * kSpoilSpreadCos + backX * kSpoilSpreadSin * spoilSide;
						perFrameData.Stamps[stampCount] = { spoilState.last.x + armX * wedgeLength, spoilState.last.y + armY * wedgeLength, 0.0f,
							std::clamp(kSpoilArmWidth * wedgeLength, 4.0f, kMaxStampShapeRadius) };
						perFrameData.StampEnds[stampCount] = { spoilState.last.x, spoilState.last.y, kStampModeCarve + kStampImpactScale * spoilEnergy, 0.0f };
						stampCount++;
					}
					speedSpoilWedges++;
				}
				if (spoilPlanted)
					spoilState.last = { spoilNode->world.translate.x, spoilNode->world.translate.y };
				spoilState.planted = spoilPlanted;
				spoilState.frame = spoilFrame;
			}
		}

		if (!contactShadersFailed && !contactStarved) {
			const auto& bound = root->worldBound;
			const bool budget = isDead ? contactCorpseCount < kContactMaxCorpses : prioritized(formID);
			if (budget && bound.radius > 0.0f &&
				std::abs(bound.center.x - contactCenter.x) + bound.radius < kContactHalfExtent &&
				std::abs(bound.center.y - contactCenter.y) + bound.radius < kContactHalfExtent) {
				bool draw = true;
				if (isDead && rest) {
					const RE::NiPoint3 center = bound.center;
					if (rest->hasPrevCenter) {
						const float step = center.GetDistance(rest->prevCenter);
						if (step >= kFootDryTeleport) {
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
					if (!draw)
						stampStats.corpsesLatched++;
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
						stampStats.actorsRasterized++;
					}
				}
				return;
			}
		}

		if (footPath && bones) {
			if (probing)
				skeletonProbe.verdict = "carving: bone path";
			bool footStamped = false;
			{
				const float footGroundZ = groundZ + bodyLift;
				const float footReach = bodyLift > 0.0f ? std::clamp(1.0f - bodyLift / std::max(nominalDepth * GetAccumulationDepthScale(), 1.0f), 0.0f, 1.0f) : 1.0f;
				const auto plantRef = FootPlantRefOf(*bones, footGroundZ);

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

					const auto print = FootPrintOf(footNode, foot.toe.get(), depthScale, footRadiusFloor);
					const float2 heel = print.heel;
					const float2 tip = print.tip;
					const float radius = print.radius;

					const float plantBand = FootPlantBand(plantRef, boneScale, print.length);
					const uint64_t key = (uint64_t(formID) << 16) | (kFootKeyBit | uint64_t(thisIndex & 0x7FFF));
					const bool wasPlanted = stampPrevPositions.find(key) != stampPrevPositions.end();
					const float releaseBand = plantBand * (wasPlanted ? 1.5f : 1.0f);
					const bool planted = footWorld.translate.z - plantRef.z <= releaseBand;
					if (probing && thisIndex < skeletonProbe.feet.size()) {
						auto& row = skeletonProbe.feet[thisIndex];
						row.zAboveRef = footWorld.translate.z - plantRef.z;
						row.band = releaseBand;
						row.planted = planted;
						row.radius = radius;
					}
					if (!planted)
						continue;

					float2 segStart = heel;
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
					stamp.z = footReach;
					stamp.w = radius;
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { segStart.x, segStart.y, kStampModeBody,
						CrustBreakForce(radius) };
					stampCount++;
					stampStats.feet++;
					footStamped = true;
					if (probing && thisIndex < skeletonProbe.feet.size())
						skeletonProbe.feet[thisIndex].stamped = true;
				}
			}

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
				perFrameData.StampEnds[stampCount] = { aWorld.translate.x, aWorld.translate.y, kStampModeBody,
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

		const bool useCorpseBones = isDead && bones && !bones->limbs.empty();
		if (useCorpseBones) {
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
				if (stampCount >= actorCeiling) {
					budgetCutRefs.insert(formID);
					break;
				}
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
				perFrameData.StampEnds[stampCount] = { aWorld.translate.x, aWorld.translate.y, kStampModeBody,
					CrustBreakForce(radius) };
				stampCount++;
				stampStats.limbs++;
			}
		}

		const float bandRefZ = std::max(groundZ, position.z) + (liftInRagdoll ? 0.0f : bodyLift);
		uint32_t shapeIndex = 0;
		if (!useCorpseBones)
			SnowDeformation::TimedTraverse(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 centerPos;
				float radius;
				if (SnowShapes::GetShapeBound(a_object, centerPos, radius)) {
					const uint32_t thisIndex = shapeIndex++;
					if (stampCount >= actorCeiling) {
						budgetCutRefs.insert(formID);
						return RE::BSVisit::BSVisitControl::kStop;
					}
					if (centerPos.z - radius > bandRefZ + kStampSurfaceBand)
						return RE::BSVisit::BSVisitControl::kContinue;
					if (radius < kMinStampShapeRadius || radius > kMaxStampShapeRadius)
						return RE::BSVisit::BSVisitControl::kContinue;

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
					const bool woken = !firstSight && sqDelta > kCorpseWakeDistance * kCorpseWakeDistance;
					if (isDead) {
						anyShapeMoved |= !firstSight && sqDelta > kCorpseStillSpeed * kCorpseStillSpeed;
						anyShapeWoken |= woken;
					}
					if (isDead && (firstSight || (rest->settled && !woken))) {
						currentPositions[key] = firstSight ? current : it->second;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					currentPositions[key] = current;

					const ShapeStamp shapeStamp = ShapeStampFor(a_object, current, previous, radius);
					const float carve = std::clamp(
						1.0f - (centerPos.z - shapeStamp.halfHeight - bandRefZ) / nominalDepth, 0.0f, 1.0f);
					if (carve < kMinLimbCarve)
						return RE::BSVisit::BSVisitControl::kContinue;
					float4 stamp{};
					stamp.x = shapeStamp.a.x;
					stamp.y = shapeStamp.a.y;
					stamp.z = carve;
					stamp.w = std::max(shapeStamp.radius * kStampRadius / kStampRadiusNeutral * depthScale,
						texelFloor);
					perFrameData.Stamps[stampCount] = stamp;
					perFrameData.StampEnds[stampCount] = { shapeStamp.b.x, shapeStamp.b.y, kStampModeBody,
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

	cpuCensus.stampSourceCount = 0;
	auto addStampsCounted = [&](RE::ActorHandle a_handle) {
		const uint before = stampCount;
		addStamps(a_handle);
		auto& c = cpuCensus;
		if (stampCount > before && c.stampSourceCount < CpuCensus::kStampSourceMax) {
			auto actor = a_handle.get();
			c.stampSources[c.stampSourceCount++] = { actor ? actor->formID : 0u, stampCount - before };
		}
	};

	if (auto player = globals::game::player) {
		ScopedTicks _actor(cpuCensus.actorTicks, cpuCensus.actors);
		addStampsCounted(player->GetHandle());
	}

	if (const auto processLists = RE::ProcessLists::GetSingleton()) {
		for (auto& actorHandle : processLists->highActorHandles) {
			ScopedTicks _actor(cpuCensus.actorTicks, cpuCensus.actors);
			addStampsCounted(actorHandle);
		}
	}
	if (cpuCensus.stampSourceCount > 0) {
		std::copy_n(cpuCensus.stampSources, cpuCensus.stampSourceCount, cpuCensus.lastStampSources);
		cpuCensus.lastStampSourceCount = cpuCensus.stampSourceCount;
		cpuCensus.lastStampSourceFrame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
	}
	skeletonProbeTarget = stampStats.nearestValid ? stampStats.nearestFormID : 0;
	globals::profiler->EndPass();

	globals::profiler->BeginPass("SnowDeformation::Trails - Gather & Contact");
	const auto tes = globals::game::tes;
	auto* playerRef = globals::game::player;
	const uint32_t phase = propScanFrame;
	propScanFrame = (propScanFrame + 1) % kPropScanInterval;
	if (phase == 0)
		propScanCycle++;
	bool scanning = false;
	auto considerProp = [&](RE::TESObjectREFR* a_ref) {
		auto* base = a_ref->GetBaseObject();
		if (!base)
			return;
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

		stampStats.propRefs++;
		const auto position = root->world.translate;
		const uint32_t formID = a_ref->formID;
		ItemSinkNote(a_ref, position);
		auto prevIt = propPrevPositions.find(formID);
		if (prevIt == propPrevPositions.end()) {
			propPrevPositions[formID] = { position, propScanCycle };
			return;
		}
		const bool propTravelled = position.GetSquaredDistance(prevIt->second.pos) >= kStampMovementGate * kStampMovementGate;
		const bool propMoved = propTravelled || (prevIt->second.mover && PropBodyAwake(root));
		prevIt->second.mover = propMoved;
		prevIt->second.cycle = propScanCycle;
		if (propTravelled)
			prevIt->second.pos = position;
		if (propMoved) {
			stampStats.propMovers++;
			if (scanning)
				propScanMovers[phase].push_back(a_ref->CreateRefHandle());
		}
		if (!propMoved && !ItemSinkWantsPrint(a_ref))
			return;
		if (stampCount >= actorCeiling) {
			budgetCutRefs.insert(formID);
			return;
		}

		const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 1e-4f) : 1.0f / 60.0f;
		if ((position.z - prevIt->second.pos.z) / dt < -kFallSpeedGate)
			return;

		float groundZ = position.z;
		tes->GetLandHeight(position, groundZ);
		const bool elevated = position.z - groundZ > kElevatedStampCutoff;
		if (!contactShadersFailed) {
			const auto& bound = root->worldBound;
			if (bound.radius > 0.0f &&
				std::abs(bound.center.x - contactCenter.x) + bound.radius < kContactHalfExtent &&
				std::abs(bound.center.y - contactCenter.y) + bound.radius < kContactHalfExtent) {
				bool skinned = false;
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
					if (!a_geometry->GetGeometryRuntimeData().skinInstance)
						return RE::BSVisit::BSVisitControl::kContinue;
					skinned = true;
					return RE::BSVisit::BSVisitControl::kStop;
				});
				(skinned ? contactActors : contactProps).push_back({ a_ref->CreateRefHandle(), bound.center.x - bound.radius, bound.center.y - bound.radius, bound.center.x + bound.radius, bound.center.y + bound.radius, false, groundZ, std::max(GetNominalSnowDepthAt(position.x, position.y, kStampDepthReference), 1.0f) });
				stampStats.propsRasterized++;
				return;
			}
		}
		if (elevated)
			return;
		const float supportZ = std::max(groundZ, position.z);

		const float nominalDepth = std::max(
			GetNominalSnowDepthAt(position.x, position.y, kStampDepthReference), 1.0f);
		const float depthScale = std::clamp(nominalDepth / kStampDepthReference,
			kStampDepthScaleMin, kStampDepthScaleMax);
		uint32_t shapeIndex = 0;
		SnowDeformation::TimedTraverse(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			RE::NiPoint3 centerPos;
			float radius;
			if (SnowShapes::GetShapeBound(a_object, centerPos, radius)) {
				const uint32_t thisIndex = shapeIndex++;
				if (stampCount >= actorCeiling) {
					budgetCutRefs.insert(formID);
					return RE::BSVisit::BSVisitControl::kStop;
				}
				if (centerPos.z - radius > supportZ + kStampSurfaceBand)
					return RE::BSVisit::BSVisitControl::kContinue;
				if (radius > kMaxStampShapeRadius)
					return RE::BSVisit::BSVisitControl::kContinue;

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

				const ShapeStamp shapeStamp = ShapeStampFor(a_object, current, previous, radius);
				const float carve = std::clamp(
					1.0f - (centerPos.z - shapeStamp.halfHeight - supportZ) / nominalDepth, 0.0f, 1.0f);
				if (carve < kMinLimbCarve)
					return RE::BSVisit::BSVisitControl::kContinue;
				float4 stamp{};
				stamp.x = shapeStamp.a.x;
				stamp.y = shapeStamp.a.y;
				stamp.z = carve;
				stamp.w = std::max(shapeStamp.radius * kStampRadius / kStampRadiusNeutral * depthScale, kMinPropStampRadius);
				perFrameData.Stamps[stampCount] = stamp;
				perFrameData.StampEnds[stampCount] = { shapeStamp.b.x, shapeStamp.b.y, 0.0f,
					CrustBreakForce(radius) };
				stampCount++;
				stampStats.props++;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		if (shapeIndex == 0 && stampCount >= actorCeiling)
			budgetCutRefs.insert(formID);
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
					stamp.w = std::max(radius * kStampRadius / kStampRadiusNeutral * depthScale, kMinPropStampRadius);
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
	if (tes && playerRef) {
		const auto originPos = playerRef->GetPosition();
		const float radius = 0.5f * deformWorldSize;
		auto& cells = propScanCells;
		cells.clear();
		if (auto* parentCell = playerRef->GetParentCell(); parentCell && parentCell->IsInteriorCell()) {
			cells.push_back(parentCell);
		} else {
			if (auto* grid = tes->gridCells) {
				const uint32_t length = grid->length;
				for (uint32_t x = 0; x < length; x++) {
					for (uint32_t y = 0; y < length; y++) {
						auto* cell = grid->GetCell(x, y);
						if (!cell || !cell->IsAttached())
							continue;
						if (auto* coords = cell->GetCoordinates()) {
							constexpr float kCell = 4096.0f;
							const float minX = coords->cellX * kCell, minY = coords->cellY * kCell;
							if (originPos.x + radius < minX || originPos.x - radius > minX + kCell ||
								originPos.y + radius < minY || originPos.y - radius > minY + kCell)
								continue;
						}
						cells.push_back(cell);
					}
				}
			}
			if (auto* worldspace = playerRef->GetWorldspace())
				if (auto* sky = worldspace->GetSkyCell())
					cells.push_back(sky);
		}
		const bool scanAll = PropScanHazardCount() != 0 || propPrevPositions.empty();
		auto scanCell = [&](RE::TESObjectCELL* a_cell) {
			a_cell->ForEachReferenceInRange(originPos, radius, [&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->As<RE::Actor>())
					return RE::BSContainer::ForEachResult::kContinue;
				if (a_ref->GetFormType() == RE::FormType::PlacedHazard) {
					ConsiderHazard(a_ref);
					propScanHazards[phase].push_back(a_ref->CreateRefHandle());
					return RE::BSContainer::ForEachResult::kContinue;
				}
				considerProp(a_ref);
				return RE::BSContainer::ForEachResult::kContinue;
			});
		};
		scanning = true;
		if (scanAll) {
			for (auto& slice : propScanMovers)
				slice.clear();
			for (auto& slice : propScanHazards)
				slice.clear();
			for (auto* cell : cells)
				scanCell(cell);
		} else {
			propScanMovers[phase].clear();
			propScanHazards[phase].clear();
			for (size_t i = phase; i < cells.size(); i += kPropScanInterval)
				scanCell(cells[i]);
		}
		scanning = false;
		propScanRefs = stampStats.propRefs;
		for (uint32_t slice = 0; slice < kPropScanInterval; slice++) {
			if (scanAll || slice == phase)
				continue;
			for (const auto& handle : propScanHazards[slice])
				if (auto ref = handle.get(); ref)
					ConsiderHazard(ref.get());
			for (const auto& handle : propScanMovers[slice])
				if (auto ref = handle.get(); ref)
					considerProp(ref.get());
		}
		if (phase == 0 && propScanCycle >= 2)
			std::erase_if(propPrevPositions, [&](const auto& a_kv) { return a_kv.second.cycle + 1 < propScanCycle; });
	}

	stampStats.beforeSpells = stampCount;

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
		const bool shoves = emitter.mark == SpellMark::Carve;
		float4 stamp{};
		stamp.x = emitter.position.x;
		stamp.y = emitter.position.y;
		stamp.z = pits ? emitter.strength * std::clamp(settings.PitDepth, 0.0f, 1.0f) :
		                 (shoves ? emitter.strength * std::clamp(settings.ForceCarveDepth, 0.0f, 1.0f) :
								   emitter.strength);
		stamp.w = (pits && !emitter.cone) ? std::max(settings.PitRadius, 4.0f) * emitter.pitScale :
		                                    std::max(emitter.radius, 4.0f);
		perFrameData.Stamps[stampCount] = stamp;
		if (emitter.cone)
			perFrameData.StampCones[stampCount] = { emitter.coneLength, stamp.w, emitter.coneSeed, 0.0f };
		perFrameData.StampEnds[stampCount] = { emitter.previous.x, emitter.previous.y,
			(pits ? kStampModePit : (glazes ? kStampModeCrust : (shoves ? kStampModeCarve + kStampModeShove : kStampModeMelt))) +
				(emitter.cone ? kStampModeCone : 0.0f) + (emitter.bowl ? kStampModeBowl : 0.0f),
			shoves ? 1.0f :
					 (pits ? 0.0f :
							 (glazes ? std::max(settings.CrustRate, 0.0f) * emitter.rateScale : emitter.rate)) };
		stampCount++;
		if (pits)
			perFrameData.StampEnds[stampCount].z += kStampImpactScale * (0.6f + 0.4f * std::clamp(emitter.strength, 0.0f, 1.0f));
		stampStats.spells++;
	}

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

	if (!budgetCutRefs.empty()) {
		for (const auto& [key, anchor] : stampPrevPositions) {
			const bool footKey = (key & 0xFFFFull) != 0xFFFFull && (key & kFootKeyBit) != 0;
			if (!footKey && budgetCutRefs.contains(uint32_t(key >> 16)))
				currentPositions.try_emplace(key, anchor);
		}
	}
	stampPrevPositions = std::move(currentPositions);
	perFrameData.StampCount = stampCount;
	globals::profiler->EndPass();
}

void SnowDeformation::RollCpuCensus()
{
	SnowHitch::OnFrame();
	static double ticksToMs = 0.0;
	double periodMs = 0.0;
	{
		static LARGE_INTEGER qpcFreq = [] {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f;
		}();
		static LARGE_INTEGER qpcPrev{};
		static uint64_t tscPrev = 0;
		LARGE_INTEGER qpcNow;
		QueryPerformanceCounter(&qpcNow);
		const uint64_t tscNow = __rdtsc();
		if (tscPrev != 0 && tscNow > tscPrev) {
			const double ms = 1000.0 * double(qpcNow.QuadPart - qpcPrev.QuadPart) / double(qpcFreq.QuadPart);
			periodMs = ms;
			const double ratio = ms / double(tscNow - tscPrev);
			ticksToMs = ticksToMs == 0.0 ? ratio : ticksToMs + (ratio - ticksToMs) * 0.05;
		}
		qpcPrev = qpcNow;
		tscPrev = tscNow;
	}
	auto ema = [](float& a_shown, float a_value) { a_shown += (a_value - a_shown) * (1.0f / 30.0f); };
	auto& c = cpuCensus;
	auto& s = cpuShown;
	{
		std::scoped_lock ringLock(c.frameRingLock);
		auto& row = c.frameRing[c.frameRingHead];
		row.frame = globals::state->frameCount;
		row.periodMs = float(periodMs);
		row.hookMs = float(c.hookTicks * ticksToMs);
		row.actorMs = float(c.actorTicks * ticksToMs);
		row.landMs = float(c.landTicks * ticksToMs);
		for (uint32_t i = 0; i < CpuCensus::kSpanCount; i++)
			row.spanMs[i] = float(c.spanTicks[i] * ticksToMs);
		c.frameRingHead = (c.frameRingHead + 1) % CpuCensus::kFrameRing;
		c.frameRingCount = std::min(c.frameRingCount + 1, CpuCensus::kFrameRing);
	}
	ema(s.hookMs, float(c.hookTicks * ticksToMs));
	ema(s.hookCalls, float(c.hookCalls));
	ema(s.actorMs, float(c.actorTicks * ticksToMs));
	ema(s.actors, float(c.actors));
	ema(s.landMs, float(c.landTicks * ticksToMs));
	ema(s.landCalls, float(c.landCalls));
	ema(s.depthMs, float(c.depthTicks * ticksToMs));
	ema(s.depthCalls, float(c.depthCalls));
	ema(s.traverseMs, float(c.traverseTicks * ticksToMs));
	ema(s.traverseCalls, float(c.traverseCalls));
	ema(s.skinLoopDraws, float(c.skinLoopDraws));
	ema(s.skinLoopCBUpdates, float(c.skinLoopCBUpdates));
	for (uint32_t i = 0; i < CpuCensus::kSpanCount; i++) {
		ema(s.spanMs[i], float(c.spanTicks[i] * ticksToMs));
		ema(s.spanCalls[i], float(c.spanCalls[i]));
		c.spanTicks[i] = 0;
		c.spanCalls[i] = 0;
	}
	c.hookTicks = c.actorTicks = c.landTicks = c.depthTicks = c.traverseTicks = 0;
	c.hookCalls = c.hookBitCalls = c.actors = c.landCalls = c.depthCalls = c.traverseCalls = 0;
	c.skinLoopDraws = c.skinLoopCBUpdates = 0;
	if (c.stampHashDumpRequested) {
		c.stampHashDumpRequested = false;
		std::string line;
		for (uint32_t i = 0; i < CpuCensus::kHashRing; i++) {
			const uint64_t h = c.stampHashRing[(c.stampHashHead + i) % CpuCensus::kHashRing];
			line += std::format("{:016X}{}", h, (i % 8 == 7) ? "\n" : " ");
		}
		logger::info("[SNOW DEFORMATION] stamp hash ring ({} frames, oldest first):\n{}", CpuCensus::kHashRing, line);
	}
}
