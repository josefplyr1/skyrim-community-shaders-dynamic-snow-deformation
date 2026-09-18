// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Stage 0 weight sink watch (WEIGHT-SINK-PLAN.md). Dev builds only, throwaway:
// delete this file and its hooks when Stage 0 exits.

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/AlphaBuild.h"
#include "Globals.h"
#include "Utils/ActorUtils.h"
#include "Utils/Form.h"

#if !SNOW_ALPHA_BUILD
namespace
{
	struct SinkBodyRead
	{
		bool has3D = false;
		float localZ = 0.0f;
		RE::NiPoint3 world;
		uint32_t bodies = 0;
		float mass = 0.0f;
		int motionType = -1;
		int islandActive = -1;
		uint32_t inactive0 = 0;
		uint32_t inactive1 = 0;
		uint32_t integrateCounter = 0;
		float speed = 0.0f;
		float boundRadius = 0.0f;
		float halfX = 0.0f;
		float halfY = 0.0f;
		float halfZ = 0.0f;
	};

	// Game thread only. Masses summed over every body; the sleep state is the first dynamic body's.
	SinkBodyRead ReadSinkBody(RE::TESObjectREFR* a_ref)
	{
		SinkBodyRead out;
		auto* root = a_ref->Get3D(false);
		if (!root)
			return out;
		out.has3D = true;
		out.localZ = root->local.translate.z;
		out.world = root->world.translate;
		const float toGame = RE::bhkWorld::GetWorldScaleInverse();
		RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
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
				out.inactive0 = hkpRigid->motion.deactivationNumInactiveFrames[0];
				out.inactive1 = hkpRigid->motion.deactivationNumInactiveFrames[1];
				out.integrateCounter = hkpRigid->motion.deactivationIntegrateCounter;
				float v[4];
				_mm_storeu_ps(v, hkpRigid->motion.linearVelocity.quad);
				out.speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * toGame;
				if (const auto* shape = hkpRigid->collidable.GetShape()) {
					Util::ExtractShapeBound(shape, out.boundRadius);
					float hx = 0.0f, hy = 0.0f, hz = 0.0f;
					if (Util::ExtractShapeHalfExtents(shape, hx, hy, hz)) {
						out.halfX = hx * toGame;
						out.halfY = hy * toGame;
						out.halfZ = hz * toGame;
					}
				}
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return out;
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
		logger::info("[SNOW DEFORMATION] SW0 weight sink watch: {}", sinkWatch ? "armed" : "off");
		sinkWatchCandidates.clear();
		std::scoped_lock lock(sinkWatchLock);
		sinkWatchStates.clear();
		sinkWatchReadout = {};
	}
	if (!sinkWatch)
		return;
	sinkWatchFrame++;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* taskInterface = SKSE::GetTaskInterface();
	if (!player || !taskInterface)
		return;
	const RE::NiPoint3 origin = player->GetPosition();

	// The nearest few, by where the prop scan last saw them.
	struct Pick
	{
		uint32_t formID;
		RE::ObjectRefHandle handle;
		float distSq;
	};
	std::vector<Pick> picks;
	for (auto it = sinkWatchCandidates.begin(); it != sinkWatchCandidates.end();) {
		if (sinkWatchFrame - it->second.seenFrame > 1200) {
			it = sinkWatchCandidates.erase(it);
			continue;
		}
		const float distSq = origin.GetSquaredDistance(it->second.position);
		if (distSq < kSinkWatchRadius * kSinkWatchRadius)
			picks.push_back({ it->first, it->second.handle, distSq });
		++it;
	}
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.distSq < b.distSq; });
	if (picks.size() > kSinkWatchMax)
		picks.resize(kSinkWatchMax);
	sinkWatchCount.store(static_cast<uint32_t>(picks.size()), std::memory_order_relaxed);
	if (picks.empty())
		return;

	const bool liftRequested = sinkWatchLiftRequest.exchange(false, std::memory_order_acq_rel);
	const float liftAmount = sinkWatchLift;
	const uint32_t frame = sinkWatchFrame;
	taskInterface->AddTask([this, picks, liftRequested, liftAmount, frame]() {
		std::scoped_lock lock(sinkWatchLock);
		bool first = true;
		for (const auto& pick : picks) {
			auto ref = pick.handle.get();
			if (!ref)
				continue;
			const SinkBodyRead body = ReadSinkBody(ref.get());
			if (!body.has3D)
				continue;
			auto& state = sinkWatchStates[pick.formID];
			const RE::NiPoint3 refPos = ref->GetPosition();
			auto* base = ref->GetBaseObject();

			// Census, once per base form.
			if (base && sinkWatchFormsLogged.size() < 400 && sinkWatchFormsLogged.insert(base->formID).second) {
				const float footprint = 4.0f * body.halfX * body.halfY;
				logger::info("[SNOW DEFORMATION] SW0 FORM {:08X} '{}' '{}' type {} | havokMass {:.3f} bodies {} | refWeight {:.3f} formWeight {:.3f} | boundR {:.2f} half ({:.1f} {:.1f} {:.1f}) footprint {:.1f} | mass/footprint {:.5f}",
					base->formID, Util::GetFormEditorID(base), base->GetName(), static_cast<int>(base->GetFormType()),
					body.mass, body.bodies, ref->GetWeight(), base->GetWeight(),
					body.boundRadius * RE::bhkWorld::GetWorldScaleInverse(), body.halfX, body.halfY, body.halfZ, footprint,
					footprint > 0.01f ? body.mass / footprint : 0.0f);
			}

			// Our own stillness, engine-free.
			const bool moved = state.frames != 0 && body.world.GetSquaredDistance(state.lastWorld) > 0.0025f;
			state.stillFrames = moved ? 0 : state.stillFrames + 1;
			const bool still = state.stillFrames >= 30;

			// The manual lift, on the nearest still prop.
			bool liftedNow = false;
			if (first && liftRequested) {
				if (!still) {
					logger::info("[SNOW DEFORMATION] SW0 LIFT refused: nearest prop {:08X} is still moving", pick.formID);
				} else if (auto* root = ref->Get3D(false)) {
					state.liftBaseLocalZ = root->local.translate.z;
					state.liftApplied = liftAmount;
					state.lifted = true;
					state.burst = 900;
					root->local.translate.z += liftAmount;
					RE::NiUpdateData data{};
					root->Update(data);
					liftedNow = true;
					logger::info("[SNOW DEFORMATION] SW0 LIFT applied: {:08X} '{}' local z {:.2f} -> {:.2f} (+{:.1f})", pick.formID,
						base ? base->GetName() : "", state.liftBaseLocalZ, root->local.translate.z, liftAmount);
				}
			}

			// A line when any sleep signal flips, every frame in a burst, else a heartbeat.
			const bool flipped = state.frames == 0 || body.islandActive != state.islandActive || still != state.still ||
			                     (body.inactive0 == 0) != (state.inactive0 == 0);
			if (flipped)
				state.burst = std::max(state.burst, 20u);
			if (flipped || liftedNow || state.burst > 0 || state.frames % 120 == 0) {
				const float expected = state.lifted ? state.liftBaseLocalZ + state.liftApplied : 0.0f;
				logger::info("[SNOW DEFORMATION] SW0 {:08X} f{} {}{} | island {} inactive {}/{} counter {} speed {:.2f} motion {} | ref z {:.2f} node local z {:.2f} world z {:.2f}{} | mass {:.3f}",
					pick.formID, frame, still ? "STILL" : "MOVING", flipped && state.frames ? " FLIP" : "",
					body.islandActive < 0 ? "none" : (body.islandActive ? "ACTIVE" : "asleep"), body.inactive0, body.inactive1,
					body.integrateCounter, body.speed, body.motionType,
					refPos.z, body.localZ, body.world.z,
					state.lifted ? std::format(" | LIFTED expect local z {:.2f} delta {:+.2f}", expected, body.localZ - expected) : std::string(),
					body.mass);
			}
			if (state.burst > 0)
				state.burst--;
			state.frames++;
			state.lastWorld = body.world;
			state.islandActive = body.islandActive;
			state.inactive0 = body.inactive0;
			state.still = still;

			if (first) {
				sinkWatchReadout.formID = pick.formID;
				sinkWatchReadout.name = base ? base->GetName() : "";
				sinkWatchReadout.mass = body.mass;
				sinkWatchReadout.refWeight = ref->GetWeight();
				sinkWatchReadout.footprint = 4.0f * body.halfX * body.halfY;
				sinkWatchReadout.islandActive = body.islandActive;
				sinkWatchReadout.inactive0 = body.inactive0;
				sinkWatchReadout.speed = body.speed;
				sinkWatchReadout.still = still;
				sinkWatchReadout.refZ = refPos.z;
				sinkWatchReadout.localZ = body.localZ;
				sinkWatchReadout.worldZ = body.world.z;
				sinkWatchReadout.lifted = state.lifted;
				sinkWatchReadout.expectedLocalZ = state.lifted ? state.liftBaseLocalZ + state.liftApplied : 0.0f;
				first = false;
			}
		}
		if (sinkWatchStates.size() > 256)
			sinkWatchStates.clear();
	});
}
#endif
