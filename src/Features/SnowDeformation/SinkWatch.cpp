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
	// Josef's measure, 2026-09-18: size-capped mass over the side of the largest face.
	constexpr float kSinkBoxDensityCap = 0.004f;
	constexpr float kSinkMeasureFloat = 0.1f;
	constexpr float kSinkMeasureFull = 0.8f;
	constexpr float kSinkMaxLift = 80.0f;

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
		float geometryPreviousZ = 0.0f;
		float radius = 0.0f;
		/** Lowest point of the collision shapes, world units. OBND cannot give it: armour boxes are in worn space. */
		bool hasUnderside = false;
		float undersideZ = 0.0f;
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
		out.radius = out.root->worldBound.radius;
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
			if (const auto* shape = hkpRigid->collidable.GetShape()) {
				RE::hkAabb aabb;
				shape->GetAabbImpl(hkpRigid->motion.motionState.transform, 0.0f, aabb);
				float low[4];
				_mm_storeu_ps(low, aabb.min.quad);
				const float z = low[2] * toGame;
				if (std::isfinite(z)) {
					out.undersideZ = out.hasUnderside ? std::min(out.undersideZ, z) : z;
					out.hasUnderside = true;
				}
			}
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
			out.geometryPreviousZ = geometry->previousWorld.translate.z;
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
		for (auto& child : node->GetChildren()) {
			if (!child || child->collisionObject)
				continue;
			child->local.translate += a_localOffset;
			shifted++;
		}
		return shifted;
	}

	void SyncPreviousWorld(RE::NiAVObject* a_object, int a_depth = 0)
	{
		if (!a_object || a_depth > 8)
			return;
		a_object->previousWorld = a_object->world;
		if (auto* node = a_object->AsNode())
			for (auto& child : node->GetChildren())
				SyncPreviousWorld(child.get(), a_depth + 1);
	}

	// An awake body's root is updated by Havok every frame and carries the
	// children with it. A sleeping one is not: push the locals down ourselves,
	// and level previousWorld, or the motion vectors keep the jump for ever.
	void PropagateChildren(RE::NiAVObject* a_root)
	{
		auto* node = a_root ? a_root->AsNode() : nullptr;
		if (!node)
			return;
		RE::NiUpdateData data{};
		for (auto& child : node->GetChildren()) {
			if (!child || child->collisionObject)
				continue;
			child->Update(data);
			SyncPreviousWorld(child.get());
		}
		a_root->UpdateWorldBound();
	}
}

void SnowDeformation::SinkWatchNote(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position)
{
	if (sinkWatchCandidates.size() > 512 && !sinkWatchCandidates.contains(a_ref->formID))
		sinkWatchCandidates.clear();
	// Every note: dropped items are temporary references and their IDs are recycled.
	auto& candidate = sinkWatchCandidates[a_ref->formID];
	candidate.handle = a_ref->CreateRefHandle();
	candidate.position = a_position;
	candidate.seenFrame = sinkWatchFrame;
}

void SnowDeformation::SinkWatchUpdate()
{
	if (sinkWatch != sinkWatchArmed) {
		sinkWatchArmed = sinkWatch;
		logger::info("[SNOW DEFORMATION] SW0 weight sink watch: {} (round 7)", sinkWatch ? "armed" : "off");
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
		float surfaceZ;  // absolute, render-thread reads
		float snowDepth;
		float landZ;
	};
	std::vector<Pick> picks;
	for (auto it = sinkWatchCandidates.begin(); it != sinkWatchCandidates.end();) {
		if (sinkWatchFrame - it->second.seenFrame > 1200) {
			it = sinkWatchCandidates.erase(it);
			continue;
		}
		const float distSq = origin.GetSquaredDistance(it->second.position);
		if (distSq < kSinkWatchRadius * kSinkWatchRadius)
			picks.push_back({ it->first, it->second.handle, distSq, 0.0f, 0.0f, 0.0f });
		++it;
	}
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.distSq < b.distSq; });
	if (picks.size() > kSinkWatchMax)
		picks.resize(kSinkWatchMax);
	sinkWatchCount.store(static_cast<uint32_t>(picks.size()), std::memory_order_relaxed);
	if (picks.empty())
		return;
	{
		// The prop scan is sliced, so its position can be many frames old; the task's is one.
		std::scoped_lock lock(sinkWatchLock);
		for (auto& pick : picks) {
			auto found = sinkWatchStates.find(pick.formID);
			if (found != sinkWatchStates.end() && found->second.liveValid && found->second.refHandle == pick.handle.native_handle()) {
				auto& position = sinkWatchCandidates[pick.formID].position;
				position.x = found->second.liveX;
				position.y = found->second.liveY;
			}
		}
	}
	for (auto& pick : picks) {
		// Measured from the ground under the item, so the answer does not
		// depend on where in its fall the item is.
		RE::NiPoint3 ground = sinkWatchCandidates[pick.formID].position;
		tes->GetLandHeight(ground, ground.z);
		const float rise = SnowLiftFor(ground, 1.0f, 0.0f, tes);
		pick.surfaceZ = ground.z + rise;
		pick.landZ = ground.z;
		pick.snowDepth = rise > 0.0f ? APISnowDepthAt(ground.x, ground.y) : 0.0f;
	}

	// Blur probe: the drawn mesh's height as THIS thread sees it, every frame.
	// The game-thread readings are steady, so look for a flicker they cannot see.
	for (const auto& pick : picks) {
		auto ref = pick.handle.get();
		auto* root = ref ? ref->Get3D(false) : nullptr;
		auto* geometry = FirstGeometry(root);
		if (!geometry)
			continue;
		auto& probe = sinkWatchProbes[pick.formID];
		const float z = geometry->world.translate.z;
		const float rootZ = root->world.translate.z;
		const bool rootStill = probe.valid && std::abs(rootZ - probe.rootZ) < 0.02f;
		if (rootStill && std::abs(z - probe.meshZ) > 0.25f && probe.logged < 300) {
			probe.logged++;
			logger::info("[SNOW DEFORMATION] SW0 RENDER-SIDE {:08X} f{}: body still, drawn mesh z {:.2f} -> {:.2f} ({:+.2f}), previous-frame z {:.2f}",
				pick.formID, sinkWatchFrame, probe.meshZ, z, z - probe.meshZ, geometry->previousWorld.translate.z);
		}
		probe.meshZ = z;
		probe.rootZ = rootZ;
		probe.valid = true;
	}
	if (sinkWatchProbes.size() > 256)
		sinkWatchProbes.clear();

	const bool liftRequested = sinkWatchLiftRequest.exchange(false, std::memory_order_acq_rel);
	const bool autoMode = sinkWatchAuto;
	const float manualLift = sinkWatchLift;
	const float trenchFloor = std::clamp(settings.TrenchFloorFraction, 0.0f, 1.0f);
	const uint32_t frame = sinkWatchFrame;
	taskInterface->AddTask([this, picks, liftRequested, autoMode, manualLift, trenchFloor, frame]() {
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
			if (state.refHandle != pick.handle.native_handle()) {
				state = {};
				state.refHandle = pick.handle.native_handle();
			}
			state.liveX = body.world.x;
			state.liveY = body.world.y;
			state.liveValid = true;
			auto* base = ref->GetBaseObject();
			const bool asleep = body.bodies > 0 && body.islandActive == 0;

			// Census, once per base form: mass against the game's weight, and the
			// authored object bounds (OBND) as the footprint it lies on right now.
			float footprint = 0.0f;
			float height = 0.0f;
			float measure = body.mass / 50.0f;
			// The item's lowest point, from its authored box through the body's
			// rotation: origins sit anywhere (clothes: on the underside).
			float bottomZ = body.world.z;
			if (auto* bound = base ? base->As<RE::TESBoundObject>() : nullptr) {
				const float s = ref->GetScale();
				float ex = float(bound->boundData.boundMax.x - bound->boundData.boundMin.x) * s;
				float ey = float(bound->boundData.boundMax.y - bound->boundData.boundMin.y) * s;
				float ez = float(bound->boundData.boundMax.z - bound->boundData.boundMin.z) * s;
				RE::NiPoint3 centre{ float(bound->boundData.boundMax.x + bound->boundData.boundMin.x) * 0.5f * s,
					float(bound->boundData.boundMax.y + bound->boundData.boundMin.y) * 0.5f * s,
					float(bound->boundData.boundMax.z + bound->boundData.boundMin.z) * 0.5f * s };
				// No authored box (mod-added forms): a cube inside the 3D's bound sphere.
				if (ex + ey + ez < 1.0f) {
					ex = ey = ez = body.radius * 1.1547f;
					centre = {};
				}
				const float scale = std::max(body.root->world.scale, 1e-3f);
				const float ux = std::abs(body.up.x * scale), uy = std::abs(body.up.y * scale), uz = std::abs(body.up.z * scale);
				footprint = ey * ez * ux + ex * ez * uy + ex * ey * uz;
				height = ex * ux + ey * uy + ez * uz;
				bottomZ = body.world.z + (body.up.x * centre.x + body.up.y * centre.y + body.up.z * centre.z) * scale - height * 0.5f;
				if (body.hasUnderside) {
					if (std::abs(bottomZ - body.undersideZ) > 8.0f && sinkWatchFormsLogged.insert(base->formID ^ 0x80000000u).second)
						logger::info("[SNOW DEFORMATION] SW0 BOX-vs-COLLISION {:08X} '{}': box underside {:+.2f} from the body, collision underside {:+.2f}",
							base->formID, base->GetName(), bottomZ - body.world.z, body.undersideZ - body.world.z);
				}
				const float cx = std::max(ex, 1.0f), cy = std::max(ey, 1.0f), cz = std::max(ez, 1.0f);
				const float cappedMass = std::min(body.mass, kSinkBoxDensityCap * cx * cy * cz);
				const float face = std::max({ cx * cy, cy * cz, cx * cz });
				measure = cappedMass / std::sqrt(face);
				if (sinkWatchFormsLogged.size() < 400 && sinkWatchFormsLogged.insert(base->formID).second)
					logger::info("[SNOW DEFORMATION] SW0 FORM {:08X} '{}' '{}' type {} | havokMass {:.2f} bodies {} | gameWeight {:.3f} | bounds ({:.1f} {:.1f} {:.1f}) lying footprint {:.1f} height {:.1f} | capped mass {:.3f} over side {:.1f} = MEASURE {:.4f}",
						base->formID, Util::GetFormEditorID(base), base->GetName(), static_cast<int>(base->GetFormType()),
						body.mass, body.bodies, ref->GetWeight(), ex, ey, ez, footprint, height,
						cappedMass, std::sqrt(face), measure);
			}

			// Havok's box is the shape's LOCAL box turned with the body: never
			// tighter than the shape, so on a wobbling shield its low corner
			// swings 6 units under a rim that never leaves the ground. Nothing
			// is under the land, and what is left of the swing is eased out.
			if (body.hasUnderside)
				bottomZ = body.undersideZ;
			bottomZ = std::max(bottomZ, pick.landZ);
			{
				const float offset = bottomZ - body.world.z;
				if (!state.undersideValid || std::abs(offset - state.undersideOffset) > 12.0f)
					state.undersideOffset = offset;
				else
					state.undersideOffset += (offset - state.undersideOffset) * 0.15f;
				state.undersideValid = true;
				bottomZ = body.world.z + state.undersideOffset;
			}

			// A rebuilt 3D has lost the offset with its nodes.
			if (state.offsetApplied && state.offsetRoot != body.root) {
				state.offsetApplied = false;
				state.childOffset = {};
				state.appliedLift = 0.0f;
				logger::info("[SNOW DEFORMATION] SW0 {:08X} 3D rebuilt: offset gone with it", pick.formID);
			}
			state.asleepFrames = asleep ? state.asleepFrames + 1 : 0;

			if (first && liftRequested) {
				state.manualLift = state.manualLift > 0.0f ? 0.0f : manualLift;
				logger::info("[SNOW DEFORMATION] SW0 BUTTON {:08X}: fixed lift {}", pick.formID, state.manualLift > 0.0f ? "on" : "off");
			}

			// The mesh is held at its rest depth every frame, awake or asleep:
			// the body falls to the ground, the mesh stops where the snow holds it.
			float sink = 0.0f;
			float want = 0.0f;
			if (body.bodies > 0 && body.mass > 0.0f && pick.snowDepth >= 1.0f && autoMode) {
				const float t = std::clamp((std::log(std::max(measure, 1e-4f)) - std::log(kSinkMeasureFloat)) /
											   (std::log(kSinkMeasureFull) - std::log(kSinkMeasureFloat)),
					0.0f, 1.0f);
				// Josef 2026-09-18: nothing rests below the trench floor; the heaviest sits ON it.
				sink = t * t * (3.0f - 2.0f * t) * (1.0f - trenchFloor);
				want = std::clamp(pick.surfaceZ - sink * pick.snowDepth - bottomZ, 0.0f, kSinkMaxLift);
				if (want < 0.5f)
					want = 0.0f;
			}
			if (state.manualLift > 0.0f)
				want = state.manualLift;

			const RE::NiPoint3 offset = body.up * want;
			const RE::NiPoint3 delta = offset - state.childOffset;
			if (delta.SqrLength() > 1e-4f) {
				const bool was = state.offsetApplied;
				const uint32_t shifted = ShiftChildren(body.root, delta);
				if (shifted > 0) {
					state.dirty = true;
					state.childOffset = offset;
					state.offsetApplied = want > 0.0f;
					state.offsetRoot = body.root;
					state.appliedLift = want;
				}
				if (was != state.offsetApplied) {
					state.burst = std::max(state.burst, 90u);
					logger::info("[SNOW DEFORMATION] SW0 HOLD {} {:08X} '{}' mass {:.2f} sink {:.2f} snow {:.1f} surface z {:.2f} body z {:.2f} -> +{:.2f} on {} child node(s)",
						state.offsetApplied ? "begins" : "ends", pick.formID, base ? base->GetName() : "", body.mass, sink,
						pick.snowDepth, pick.surfaceZ, body.world.z, want, shifted);
				}
			}
			if (state.dirty && (asleep || body.bodies == 0)) {
				PropagateChildren(body.root);
				state.dirty = false;
			}
			if (asleep && state.asleepFrames == 1)
				logger::info("[SNOW DEFORMATION] SW0 REST {:08X} '{}' measure {:.4f} sink {:.2f} snow {:.1f} | body z {:.2f} underside {:+.2f} from it | mesh held +{:.2f} = underside {:.1f} below the surface",
					pick.formID, base ? base->GetName() : "", measure, sink, pick.snowDepth, body.world.z, bottomZ - body.world.z, state.appliedLift,
					pick.surfaceZ - bottomZ - state.appliedLift);

			const bool flipped = state.frames == 0 || body.islandActive != state.islandActive;
			if (flipped)
				state.burst = std::max(state.burst, 20u);
			if (flipped || state.burst > 0 || state.frames % 120 == 0)
				logger::info("[SNOW DEFORMATION] SW0 {:08X} f{} {}{} speed {:.2f} | root z {:.2f} mesh z {:.2f} (mesh above root {:+.2f}, previous-frame z off by {:+.2f}){} | mass {:.2f}",
					pick.formID, frame, body.bodies == 0 ? "no body" : (body.islandActive < 0 ? "no island" : (body.islandActive ? "ACTIVE" : "asleep")),
					flipped && state.frames ? " EDGE" : "", body.speed, body.world.z, body.geometryZ, body.geometryZ - body.world.z, body.geometryPreviousZ - body.geometryZ,
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
				sinkWatchReadout.measure = measure;
				sinkWatchReadout.sink = sink;
				first = false;
			}
		}
		if (sinkWatchStates.size() > 256)
			sinkWatchStates.clear();
	});
}
#endif
