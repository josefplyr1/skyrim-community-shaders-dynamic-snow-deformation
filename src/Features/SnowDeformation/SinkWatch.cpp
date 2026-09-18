// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Stage 0 weight sink watch (WEIGHT-SINK-PLAN.md). Dev builds only, throwaway:
// delete this file and its hooks when Stage 0 exits.

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/AlphaBuild.h"
#include "Globals.h"
#include "Utils/Form.h"

#if !SNOW_ALPHA_BUILD
namespace
{
	// Provisional, mass alone: the real anchors come from this round's census.
	constexpr float kSinkMassFloat = 3.0f;
	constexpr float kSinkMassFull = 35.0f;

	struct SinkBodyRead
	{
		RE::NiAVObject* root = nullptr;
		RE::NiPoint3 world;
		RE::NiPoint3 up;  // world up in the root's local frame, unit length over scale
		uint32_t bodies = 0;
		float mass = 0.0f;
		int motionType = -1;
		int islandActive = -1;
		float speed = 0.0f;
		bool hasGeometry = false;
		float geometryZ = 0.0f;
	};

	RE::BSGeometry* FirstGeometry(RE::NiAVObject* a_object, int a_depth = 0)
	{
		if (!a_object || a_depth > 8)
			return nullptr;
		if (auto* geometry = a_object->AsGeometry())
			return geometry;
		if (auto* node = a_object->AsNode())
			for (auto& child : node->GetChildren())
				if (auto* found = FirstGeometry(child.get(), a_depth + 1))
					return found;
		return nullptr;
	}

	// Game thread only. Masses summed over every body; the sleep state is the first body's.
	SinkBodyRead ReadSinkBody(RE::TESObjectREFR* a_ref)
	{
		SinkBodyRead out;
		out.root = a_ref->Get3D(false);
		if (!out.root)
			return out;
		out.world = out.root->world.translate;
		const auto& rotate = out.root->world.rotate;
		const float scale = std::max(out.root->world.scale, 1e-3f);
		out.up = { rotate.entry[2][0] / scale, rotate.entry[2][1] / scale, rotate.entry[2][2] / scale };
		const float toGame = RE::bhkWorld::GetWorldScaleInverse();
		RE::BSVisit::TraverseScenegraphCollision(out.root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			auto* body = a_object ? a_object->body.get() : nullptr;
			auto* bhkRigid = body ? body->AsBhkRigidBody() : nullptr;
			auto* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
			if (!hkpRigid)
				return RE::BSVisit::BSVisitControl::kContinue;
			out.bodies++;
			out.mass += hkpRigid->motion.GetMass();
			if (out.motionType < 0) {
				out.motionType = static_cast<int>(hkpRigid->motion.type.underlying());
				out.islandActive = hkpRigid->simulationIsland ? (hkpRigid->simulationIsland->isInActiveIslandsArray ? 1 : 0) : -1;
				float v[4];
				_mm_storeu_ps(v, hkpRigid->motion.linearVelocity.quad);
				out.speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * toGame;
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		if (auto* geometry = FirstGeometry(out.root)) {
			out.hasGeometry = true;
			out.geometryZ = geometry->world.translate.z;
		}
		return out;
	}

	// The root is Havok's: Update() on it re-reads the body and wipes any
	// offset, asleep or not (round 1). Its children are ours.
	uint32_t ShiftChildren(RE::NiAVObject* a_root, const RE::NiPoint3& a_localOffset)
	{
		auto* node = a_root ? a_root->AsNode() : nullptr;
		if (!node)
			return 0;
		uint32_t shifted = 0;
		RE::NiUpdateData data{};
		for (auto& child : node->GetChildren()) {
			if (!child || child->collisionObject)
				continue;
			child->local.translate += a_localOffset;
			child->Update(data);
			shifted++;
		}
		if (shifted)
			a_root->UpdateWorldBound();
		return shifted;
	}
}

void SnowDeformation::SinkWatchNote(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position)
{
	if (sinkWatchCandidates.size() > 512 && !sinkWatchCandidates.contains(a_ref->formID))
		sinkWatchCandidates.clear();
	auto& candidate = sinkWatchCandidates[a_ref->formID];
	if (!candidate.handle)
		candidate.handle = a_ref->CreateRefHandle();
	candidate.position = a_position;
	candidate.seenFrame = sinkWatchFrame;
}

void SnowDeformation::SinkWatchUpdate()
{
	if (sinkWatch != sinkWatchArmed) {
		sinkWatchArmed = sinkWatch;
		logger::info("[SNOW DEFORMATION] SW0 weight sink watch: {} (round 2: child offsets)", sinkWatch ? "armed" : "off");
		sinkWatchCandidates.clear();
	}
	if (!sinkWatch)
		return;
	sinkWatchFrame++;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* taskInterface = SKSE::GetTaskInterface();
	auto* tes = RE::TES::GetSingleton();
	if (!player || !taskInterface || !tes)
		return;
	const RE::NiPoint3 origin = player->GetPosition();

	struct Pick
	{
		uint32_t formID;
		RE::ObjectRefHandle handle;
		float distSq;
		float liftToSurface;  // from the root to the snow surface, render-thread reads
		float snowDepth;
	};
	std::vector<Pick> picks;
	for (auto it = sinkWatchCandidates.begin(); it != sinkWatchCandidates.end();) {
		if (sinkWatchFrame - it->second.seenFrame > 1200) {
			it = sinkWatchCandidates.erase(it);
			continue;
		}
		const float distSq = origin.GetSquaredDistance(it->second.position);
		if (distSq < kSinkWatchRadius * kSinkWatchRadius)
			picks.push_back({ it->first, it->second.handle, distSq, 0.0f, 0.0f });
		++it;
	}
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.distSq < b.distSq; });
	if (picks.size() > kSinkWatchMax)
		picks.resize(kSinkWatchMax);
	sinkWatchCount.store(static_cast<uint32_t>(picks.size()), std::memory_order_relaxed);
	if (picks.empty())
		return;
	for (auto& pick : picks) {
		const auto& position = sinkWatchCandidates[pick.formID].position;
		pick.liftToSurface = SnowLiftFor(position, 1.0f, 0.0f, tes);
		pick.snowDepth = APISnowDepthAt(position.x, position.y);
	}

	const bool liftRequested = sinkWatchLiftRequest.exchange(false, std::memory_order_acq_rel);
	const bool autoMode = sinkWatchAuto;
	const float manualLift = sinkWatchLift;
	const uint32_t frame = sinkWatchFrame;
	taskInterface->AddTask([this, picks, liftRequested, autoMode, manualLift, frame]() {
		std::scoped_lock lock(sinkWatchLock);
		bool first = true;
		for (const auto& pick : picks) {
			auto ref = pick.handle.get();
			if (!ref)
				continue;
			const SinkBodyRead body = ReadSinkBody(ref.get());
			if (!body.root)
				continue;
			auto& state = sinkWatchStates[pick.formID];
			auto* base = ref->GetBaseObject();
			const bool asleep = body.bodies > 0 && body.islandActive == 0;

			// Census, once per base form: mass against the game's weight, and the
			// authored object bounds (OBND) as the footprint it lies on right now.
			float footprint = 0.0f;
			float height = 0.0f;
			if (auto* bound = base ? base->As<RE::TESBoundObject>() : nullptr) {
				const float s = ref->GetScale();
				const float ex = float(bound->boundData.boundMax.x - bound->boundData.boundMin.x) * s;
				const float ey = float(bound->boundData.boundMax.y - bound->boundData.boundMin.y) * s;
				const float ez = float(bound->boundData.boundMax.z - bound->boundData.boundMin.z) * s;
				const float scale = std::max(body.root->world.scale, 1e-3f);
				const float ux = std::abs(body.up.x * scale), uy = std::abs(body.up.y * scale), uz = std::abs(body.up.z * scale);
				footprint = ey * ez * ux + ex * ez * uy + ex * ey * uz;
				height = ex * ux + ey * uy + ez * uz;
				if (sinkWatchFormsLogged.size() < 400 && sinkWatchFormsLogged.insert(base->formID).second)
					logger::info("[SNOW DEFORMATION] SW0 FORM {:08X} '{}' '{}' type {} | havokMass {:.2f} bodies {} | gameWeight {:.3f} | bounds ({:.1f} {:.1f} {:.1f}) lying footprint {:.1f} height {:.1f} | mass/footprint {:.5f}",
						base->formID, Util::GetFormEditorID(base), base->GetName(), static_cast<int>(base->GetFormType()),
						body.mass, body.bodies, ref->GetWeight(), ex, ey, ez, footprint, height,
						footprint > 0.5f ? body.mass / footprint : 0.0f);
			}

			// A rebuilt 3D has lost the offset with its nodes.
			if (state.offsetApplied && state.offsetRoot != body.root) {
				state.offsetApplied = false;
				logger::info("[SNOW DEFORMATION] SW0 {:08X} 3D rebuilt: offset gone with it", pick.formID);
			}
			// Awake: the item goes where physics takes it, unlifted.
			if (state.offsetApplied && !asleep) {
				ShiftChildren(body.root, state.childOffset * -1.0f);
				state.offsetApplied = false;
				state.burst = std::max(state.burst, 60u);
				logger::info("[SNOW DEFORMATION] SW0 DROP {:08X} woke: offset removed", pick.formID);
			}
			state.asleepFrames = asleep ? state.asleepFrames + 1 : 0;

			const bool manual = first && liftRequested;
			if (manual && !asleep)
				logger::info("[SNOW DEFORMATION] SW0 LIFT refused: nearest item {:08X} is not asleep yet", pick.formID);
			if (!state.offsetApplied && asleep && state.asleepFrames >= 10 && (manual || autoMode)) {
				float sink = 0.0f;
				float lift = manualLift;
				if (!manual) {
					const float t = std::clamp((std::log(std::max(body.mass, 0.01f)) - std::log(kSinkMassFloat)) /
												   (std::log(kSinkMassFull) - std::log(kSinkMassFloat)),
						0.0f, 1.0f);
					sink = t * t * (3.0f - 2.0f * t);
					lift = std::max(pick.liftToSurface - sink * pick.snowDepth, 0.0f);
				}
				if (lift >= 1.0f) {
					state.childOffset = body.up * lift;
					const uint32_t shifted = ShiftChildren(body.root, state.childOffset);
					state.offsetApplied = shifted > 0;
					state.offsetRoot = body.root;
					state.appliedLift = lift;
					state.burst = 600;
					logger::info("[SNOW DEFORMATION] SW0 LIFT applied ({}): {:08X} '{}' mass {:.2f} sink {:.2f} snow {:.1f} to-surface {:.1f} -> +{:.2f} on {} child node(s)",
						manual ? "button" : "auto", pick.formID, base ? base->GetName() : "", body.mass, sink, pick.snowDepth,
						pick.liftToSurface, lift, shifted);
				} else if (manual) {
					logger::info("[SNOW DEFORMATION] SW0 LIFT refused: {:08X} computed lift {:.2f} is under 1 unit", pick.formID, lift);
				}
			}

			const bool flipped = state.frames == 0 || body.islandActive != state.islandActive;
			if (flipped)
				state.burst = std::max(state.burst, 20u);
			if (flipped || state.burst > 0 || state.frames % 120 == 0)
				logger::info("[SNOW DEFORMATION] SW0 {:08X} f{} {}{} speed {:.2f} | root z {:.2f} mesh z {:.2f} (mesh above root {:+.2f}){} | mass {:.2f}",
					pick.formID, frame, body.bodies == 0 ? "no body" : (body.islandActive < 0 ? "no island" : (body.islandActive ? "ACTIVE" : "asleep")),
					flipped && state.frames ? " EDGE" : "", body.speed, body.world.z, body.geometryZ, body.geometryZ - body.world.z,
					state.offsetApplied ? std::format(" | LIFTED +{:.2f}", state.appliedLift) : std::string(), body.mass);
			if (state.burst > 0)
				state.burst--;
			state.frames++;
			state.islandActive = body.islandActive;

			if (first) {
				sinkWatchReadout.formID = pick.formID;
				sinkWatchReadout.name = base ? base->GetName() : "";
				sinkWatchReadout.mass = body.mass;
				sinkWatchReadout.refWeight = ref->GetWeight();
				sinkWatchReadout.footprint = footprint;
				sinkWatchReadout.islandActive = body.bodies == 0 ? -1 : body.islandActive;
				sinkWatchReadout.speed = body.speed;
				sinkWatchReadout.rootZ = body.world.z;
				sinkWatchReadout.meshZ = body.geometryZ;
				sinkWatchReadout.lifted = state.offsetApplied;
				sinkWatchReadout.appliedLift = state.appliedLift;
				sinkWatchReadout.snowDepth = pick.snowDepth;
				first = false;
			}
		}
		if (sinkWatchStates.size() > 256)
			sinkWatchStates.clear();
	});
}
#endif
