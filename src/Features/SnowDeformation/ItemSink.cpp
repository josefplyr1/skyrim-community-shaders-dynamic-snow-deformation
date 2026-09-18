// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Dropped items rest in the snow by weight and shape. Design, measurements and
// the rules every line here was paid for: MD plans/WEIGHT-SINK-PLAN.md.

#include "Features/SnowDeformation.h"

#include "CoSave.h"
#include "Globals.h"
#include "State.h"

namespace
{
	constexpr float kBoxDensityCap = 0.004f;
	constexpr float kMeasureFloat = 0.1f;
	constexpr float kMeasureFull = 0.8f;
	constexpr float kMaxLift = 80.0f;
	/** A rest height is the item's where it lies; this far from there it is forgotten. */
	constexpr float kRestMove = 3.0f;
	/** A saved rest height is taken only by an item this close to where it was saved. */
	constexpr float kRecordMatch = 8.0f;
	constexpr size_t kMaxRecords = 4096;
	/** Melee weapons are authored anywhere from 9 to 35 for the same blade (steel vs iron greatsword); this much per unit of length at least. */
	constexpr float kWeaponMassPerLength = 0.3f;
	/** An item this far under where today's snow would put it is buried, and stops printing its trench. */
	constexpr float kBuriedMargin = 2.0f;

	struct BodyRead
	{
		RE::NiAVObject* root = nullptr;
		RE::NiPoint3 world;
		RE::NiPoint3 up;  // world up in the root's frame, over its scale
		float scale = 1.0f;
		float radius = 0.0f;
		uint32_t bodies = 0;
		float mass = 0.0f;
		bool asleep = false;
		bool hasBox = false;
		float undersideZ = 0.0f;
		float topZ = 0.0f;
		/** The first shape's box in its own frame, game units: pose-independent. */
		bool hasLocal = false;
		float local[3] = {};
	};

	// Masses summed over every body; sleep is the first body's island.
	BodyRead ReadBody(RE::TESObjectREFR* a_ref)
	{
		BodyRead out;
		out.root = a_ref->Get3D(false);
		if (!out.root)
			return out;
		out.world = out.root->world.translate;
		out.radius = out.root->worldBound.radius;
		const auto& rotate = out.root->world.rotate;
		out.scale = std::max(out.root->world.scale, 1e-3f);
		out.up = { rotate.entry[2][0] / out.scale, rotate.entry[2][1] / out.scale, rotate.entry[2][2] / out.scale };
		const float toGame = RE::bhkWorld::GetWorldScaleInverse();
		bool first = true;
		RE::BSVisit::TraverseScenegraphCollision(out.root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			auto* body = a_object ? a_object->body.get() : nullptr;
			auto* bhkRigid = body ? body->AsBhkRigidBody() : nullptr;
			auto* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
			if (!hkpRigid)
				return RE::BSVisit::BSVisitControl::kContinue;
			out.bodies++;
			out.mass += hkpRigid->motion.GetMass();
			if (first) {
				first = false;
				out.asleep = hkpRigid->simulationIsland && !hkpRigid->simulationIsland->isInActiveIslandsArray;
			}
			// The shape's LOCAL box turned with the body: never tighter than the
			// shape, so it only ever errs downward (clamped to the land below).
			if (const auto* shape = hkpRigid->collidable.GetShape()) {
				RE::hkAabb aabb;
				shape->GetAabbImpl(hkpRigid->motion.motionState.transform, 0.0f, aabb);
				float low[4], high[4];
				_mm_storeu_ps(low, aabb.min.quad);
				_mm_storeu_ps(high, aabb.max.quad);
				const float z = low[2] * toGame;
				const float top = high[2] * toGame;
				if (std::isfinite(z) && std::isfinite(top)) {
					out.undersideZ = out.hasBox ? std::min(out.undersideZ, z) : z;
					out.topZ = out.hasBox ? std::max(out.topZ, top) : top;
					out.hasBox = true;
				}
				if (!out.hasLocal) {
					RE::hkTransform identity;
					identity.rotation.col0 = { 1.0f, 0.0f, 0.0f, 0.0f };
					identity.rotation.col1 = { 0.0f, 1.0f, 0.0f, 0.0f };
					identity.rotation.col2 = { 0.0f, 0.0f, 1.0f, 0.0f };
					identity.translation = { 0.0f, 0.0f, 0.0f, 0.0f };
					RE::hkAabb box;
					shape->GetAabbImpl(identity, 0.0f, box);
					_mm_storeu_ps(low, box.min.quad);
					_mm_storeu_ps(high, box.max.quad);
					bool finite = true;
					for (int i = 0; i < 3; ++i) {
						out.local[i] = (high[i] - low[i]) * toGame;
						finite = finite && std::isfinite(out.local[i]) && out.local[i] > 0.0f;
					}
					out.hasLocal = finite;
				}
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return out;
	}

	// The root is Havok's: Update() on it re-reads the body and wipes any
	// offset, asleep or not. Its children are ours.
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

	using WorldList = std::vector<std::pair<RE::NiAVObject*, RE::NiTransform>>;

	void CollectWorld(RE::NiAVObject* a_object, WorldList& a_out, int a_depth = 0)
	{
		if (!a_object || a_depth > 8)
			return;
		a_out.emplace_back(a_object, a_object->world);
		if (auto* node = a_object->AsNode())
			for (auto& child : node->GetChildren())
				CollectWorld(child.get(), a_out, a_depth + 1);
	}

	// Nothing refreshes previousWorld under a Havok-driven root, and the motion
	// vectors are made from it. Last frame's final transforms where the subtree
	// is the same one, this frame's otherwise. a_last is never dereferenced.
	void PropagateChildren(RE::NiAVObject* a_root, bool a_consecutive, WorldList& a_last)
	{
		auto* node = a_root ? a_root->AsNode() : nullptr;
		if (!node)
			return;
		RE::NiUpdateData data{};
		WorldList now;
		for (auto& child : node->GetChildren()) {
			if (!child || child->collisionObject)
				continue;
			child->Update(data);
			CollectWorld(child.get(), now);
		}
		bool same = a_consecutive && now.size() == a_last.size();
		for (size_t i = 0; same && i < now.size(); ++i)
			same = now[i].first == a_last[i].first;
		for (size_t i = 0; i < now.size(); ++i)
			now[i].first->previousWorld = same ? a_last[i].second : now[i].second;
		a_last = std::move(now);
		a_root->UpdateWorldBound();
	}

	float Smooth(float a_t)
	{
		a_t = std::clamp(a_t, 0.0f, 1.0f);
		return a_t * a_t * (3.0f - 2.0f * a_t);
	}
}

void SnowDeformation::ItemSinkNote(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position)
{
	if (!settings.ItemSink && itemSinkHeld.load(std::memory_order_relaxed) == 0)
		return;
	// What a player can drop. The scan also walks effect planes, statics and
	// loose clutter that has lain where it is since the cell was built.
	auto* base = a_ref->GetBaseObject();
	if (!base || !base->IsInventoryObject())
		return;
	const auto handle = a_ref->CreateRefHandle();
	if (itemSinkCandidates.size() > 1024 && !itemSinkCandidates.contains(handle.native_handle()))
		itemSinkCandidates.clear();
	auto& candidate = itemSinkCandidates[handle.native_handle()];
	candidate.handle = handle;
	candidate.position = a_position;
	candidate.seenFrame = itemSinkFrame;
}

bool SnowDeformation::ItemSinkWantsPrint(RE::TESObjectREFR* a_ref)
{
	if (!settings.ItemSink || itemSinkHeld.load(std::memory_order_relaxed) == 0)
		return false;
	const uint32_t key = a_ref->CreateRefHandle().native_handle();
	std::scoped_lock lock(itemSinkLock);
	auto found = itemSinkStates.find(key);
	return found != itemSinkStates.end() && found->second.offsetApplied && !found->second.buried;
}

void SnowDeformation::ItemSinkUpdate()
{
	// Switched off: keep running until every offset this pass can reach is undone.
	if (!settings.EnableSnowDeformation || (!settings.ItemSink && itemSinkHeld.load(std::memory_order_relaxed) == 0))
		return;
	if (itemSinkCandidatesStale.exchange(false, std::memory_order_acq_rel))
		itemSinkCandidates.clear();
	// PlayerCamera::Update runs twice a frame.
	static uint lastRenderFrame = ~0u;
	if (lastRenderFrame == globals::state->frameCount)
		return;
	lastRenderFrame = globals::state->frameCount;
	itemSinkFrame++;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* tes = RE::TES::GetSingleton();
	if (!player || !tes)
		return;
	const RE::NiPoint3 origin = player->GetPosition();

	struct Pick
	{
		uint32_t key;
		RE::ObjectRefHandle handle;
		float distSq;
	};
	std::vector<Pick> picks;
	for (auto it = itemSinkCandidates.begin(); it != itemSinkCandidates.end();) {
		if (itemSinkFrame - it->second.seenFrame > 1200) {
			it = itemSinkCandidates.erase(it);
			continue;
		}
		const float distSq = origin.GetSquaredDistance(it->second.position);
		if (distSq < kItemSinkRadius * kItemSinkRadius)
			picks.push_back({ it->first, it->second.handle, distSq });
		++it;
	}
	std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.distSq < b.distSq; });
	if (picks.size() > kItemSinkMax)
		picks.resize(kItemSinkMax);

	const bool enabled = settings.ItemSink;
	const float trenchFloor = std::clamp(settings.TrenchFloorFraction, 0.0f, 1.0f);
	const float embedShare = std::clamp(settings.ItemEmbedPercent, 0.0f, 100.0f) * 0.01f;
	const float roundFloor = std::clamp(settings.ItemMinRoundness, 0.0f, 1.0f);
	const float roundFull = std::max(0.5f, roundFloor + 0.05f);
	const uint32_t frame = itemSinkFrame;
	const bool wantReadout = itemSinkReadoutWanted.exchange(false, std::memory_order_acq_rel);

	std::scoped_lock lock(itemSinkLock);
	uint32_t held = 0;
	bool first = true;
	for (const auto& pick : picks) {
		auto ref = pick.handle.get();
		if (!ref)
			continue;
		const BodyRead body = ReadBody(ref.get());
		if (!body.root || body.bodies == 0 || body.mass <= 0.0f)
			continue;
		auto* base = ref->GetBaseObject();
		if (!base)
			continue;
		auto& state = itemSinkStates[pick.key];
		const bool fresh = state.formID == 0;
		state.formID = ref->GetFormID();
		state.baseID = base->GetFormID();
		state.seenFrame = frame;
		itemSinkCandidates[pick.key].position = body.world;

		// Thickness, roundness and the weight measure, from the authored box's
		// EXTENTS (its centre is worn-space on armour and is never used).
		float thickness = body.radius * 1.1547f;
		float side = thickness;
		float longest = thickness;
		if (auto* bound = base->As<RE::TESBoundObject>()) {
			const float s = ref->GetScale();
			const float ex = std::max(float(bound->boundData.boundMax.x - bound->boundData.boundMin.x) * s, 1.0f);
			const float ey = std::max(float(bound->boundData.boundMax.y - bound->boundData.boundMin.y) * s, 1.0f);
			const float ez = std::max(float(bound->boundData.boundMax.z - bound->boundData.boundMin.z) * s, 1.0f);
			if (ex + ey + ez > 3.5f) {
				thickness = std::min({ ex, ey, ez });
				side = std::sqrt(std::max({ ex * ey, ey * ez, ex * ez }));
				longest = std::max({ ex, ey, ez });
			}
		}
		// No authored box (mod-added forms), or a cube (the iron mace is 54 on
		// every side): the collision's own box is the better shape.
		if (body.hasLocal && thickness > 0.95f * longest) {
			const float lx = std::max(body.local[0], 1.0f), ly = std::max(body.local[1], 1.0f), lz = std::max(body.local[2], 1.0f);
			thickness = std::min({ lx, ly, lz });
			side = std::sqrt(std::max({ lx * ly, ly * lz, lx * lz }));
			longest = std::max({ lx, ly, lz });
		}
		thickness = std::max(thickness, 1.0f);
		side = std::max(side, 1.0f);
		float mass = body.mass;
		if (auto* weapon = base->As<RE::TESObjectWEAP>(); weapon && !weapon->IsBow() && !weapon->IsCrossbow() && !weapon->IsStaff())
			mass = std::max(mass, kWeaponMassPerLength * longest);
		const float cappedMass = std::min(mass, kBoxDensityCap * side * side * thickness);
		const float measure = cappedMass / side;
		const float roundness = std::clamp(thickness / side, 0.0f, 1.0f);
		if (itemSinkFormsLogged.size() < 64 && itemSinkFormsLogged.insert(base->GetFormID()).second)
			logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' mass {:.1f} (used {:.3f}) side {:.1f} thickness {:.1f} -> measure {:.4f}, roundness {:.2f}",
				base->GetFormID(), base->GetName(), body.mass, cappedMass, side, thickness, measure, roundness);

		// Underside: the collision's, never under the land, eased - the loose
		// box swings with the pose while the shape's low point does not.
		RE::NiPoint3 ground = body.world;
		tes->GetLandHeight(ground, ground.z);
		const float rise = SnowLiftFor(ground, 1.0f, 0.0f, tes);
		const float surfaceZ = ground.z + rise;
		const float snowDepth = rise > 0.0f ? APISnowDepthAt(ground.x, ground.y) : 0.0f;
		float bottomZ = std::max(body.hasBox ? body.undersideZ : body.world.z, ground.z);
		{
			const float offset = bottomZ - body.world.z;
			if (!state.undersideValid || std::abs(offset - state.undersideOffset) > 12.0f)
				state.undersideOffset = offset;
			else
				state.undersideOffset += (offset - state.undersideOffset) * 0.15f;
			state.undersideValid = true;
			bottomZ = body.world.z + state.undersideOffset;
			if (body.hasBox) {
				const float tall = std::max(body.topZ - body.undersideZ, 0.0f);
				state.lyingHeight = state.lyingHeight > 0.0f ? state.lyingHeight + (tall - state.lyingHeight) * 0.15f : tall;
			}
		}

		// A rebuilt 3D has lost the offset with its nodes - and a 3D that
		// outlived its state (a load clears states) still carries one.
		if (state.offsetRoot != body.root) {
			state.offsetApplied = false;
			state.childOffset = {};
			state.appliedLift = 0.0f;
			state.offsetRoot = nullptr;
			if (auto found = itemSinkApplied.find(body.root); found != itemSinkApplied.end()) {
				auto* node = body.root->AsNode();
				RE::NiAVObject* firstChild = nullptr;
				if (node)
					for (auto& child : node->GetChildren())
						if (child && !child->collisionObject) {
							firstChild = child.get();
							break;
						}
				const auto& applied = found->second;
				const bool same = firstChild && firstChild == applied.child &&
				                  firstChild->local.translate.x == applied.childLocal.x &&
				                  firstChild->local.translate.y == applied.childLocal.y &&
				                  firstChild->local.translate.z == applied.childLocal.z;
				if (same) {
					state.offsetApplied = true;
					state.childOffset = applied.offset;
					state.offsetRoot = body.root;
					state.appliedLift = applied.lift;
					if (itemSinkClaimsLogged < 64) {
						itemSinkClaimsLogged++;
						logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' still carries +{:.1f} from before its state was dropped; adopted",
							state.formID, base->GetName(), applied.lift);
					}
				} else {
					itemSinkApplied.erase(found);
				}
			}
		}

		const float weight = Smooth((std::log(std::max(measure, 1e-4f)) - std::log(kMeasureFloat)) / (std::log(kMeasureFull) - std::log(kMeasureFloat)));
		const float sink = weight * (1.0f - trenchFloor);
		const float lying = state.lyingHeight > 0.0f ? std::min(thickness, state.lyingHeight) : thickness;
		const float embed = weight * weight * embedShare * lying * Smooth((roundness - roundFloor) / (roundFull - roundFloor));

		// Snow already dug away around the item (the player's trenches, a
		// trampled yard): it rests on what is left, not on the snow that was.
		// A ring outside its own print, or its own trench would pull it down.
		if (((frame + pick.key) & 7) == 0 || fresh) {
			const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
			// Just past its own print, and the three deepest of eight: an item
			// wider than a trail still drops into it, where the average of a ring
			// that mostly lands on untouched snow left it hanging over the trail.
			const float ring = side * 0.5f + 10.0f;
			float taps[8];
			{
				std::scoped_lock storeLock(trenchStoreMutex);
				for (int i = 0; i < 8; ++i) {
					const float angle = 0.785398f * i;
					taps[i] = SampleTrenchStore(worldspace, body.world.x + std::cos(angle) * ring, body.world.y + std::sin(angle) * ring);
				}
				// The sampler's one-tile cache must not outlive the lock.
				trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
				trenchSampleTile = nullptr;
			}
			std::sort(std::begin(taps), std::end(taps), std::greater<float>());
			state.carveAround = std::clamp((taps[0] + taps[1] + taps[2]) / 3.0f, 0.0f, 1.0f);
		}
		const float carveAround = std::min(state.carveAround, 1.0f - trenchFloor);
		// The height a snow state puts this item at, under the CURRENT settings.
		auto heightIn = [&](float a_surface, float a_depth, float a_carve) { return a_surface - std::max(sink, a_carve) * a_depth - embed; };
		const float today = heightIn(surfaceZ, snowDepth, carveAround);

		// Snow buries: what is kept is the snow the item settled in. Rising
		// snow closes over it; falling or dug snow takes it down, and it stays
		// down. Kept as the snow rather than a height so the settings stay live.
		if (state.restValid && body.world.GetSquaredDistance(state.restPos) > kRestMove * kRestMove)
			state.restValid = false;
		if (fresh && !state.restValid) {
			if (auto found = itemSinkLoaded.find(state.formID); found != itemSinkLoaded.end()) {
				const auto& record = found->second;
				const bool claimed = record.baseID == state.baseID && std::abs(record.x - body.world.x) < kRecordMatch && std::abs(record.y - body.world.y) < kRecordMatch;
				if (claimed) {
					state.restValid = true;
					state.restSurface = record.surface;
					state.restDepth = record.depth;
					state.restCarve = record.carve;
					state.restPos = body.world;
				}
				if (itemSinkClaimsLogged < 64) {
					itemSinkClaimsLogged++;
					logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' {} its saved rest (snow then {:.1f} deep at z {:.1f}, carved {:.2f}; today {:.1f} deep at z {:.1f}, carved {:.2f}; moved {:.1f}, {:.1f})",
						state.formID, base->GetName(), claimed ? "claims" : "REFUSES", record.depth, record.surface, record.carve, snowDepth, surfaceZ, carveAround,
						body.world.x - record.x, body.world.y - record.y);
				}
				itemSinkLoaded.erase(found);
			}
		}
		// No snow data (a load's first frames, a cell edge) says nothing about
		// where the snow is; only real readings move the rest.
		const bool snowKnown = snowDepth >= 1.0f;
		float target = today;
		if (state.restValid) {
			if (snowKnown && today < heightIn(state.restSurface, state.restDepth, state.restCarve)) {
				state.restSurface = surfaceZ;
				state.restDepth = snowDepth;
				state.restCarve = carveAround;
			}
			target = heightIn(state.restSurface, state.restDepth, state.restCarve);
		} else if (body.asleep && snowKnown) {
			state.restValid = true;
			state.restSurface = surfaceZ;
			state.restDepth = snowDepth;
			state.restCarve = carveAround;
			state.restPos = body.world;
		}
		state.buried = snowKnown && today - target > kBuriedMargin;

		float want = enabled && snowKnown ? std::clamp(target - bottomZ, 0.0f, kMaxLift) : 0.0f;
		if (want < 0.5f)
			want = 0.0f;

		const RE::NiPoint3 offset = body.up * want;
		const RE::NiPoint3 delta = offset - state.childOffset;
		const bool moved = delta.SqrLength() > 1e-4f;
		// A held item that is awake moves every frame whether or not the lift
		// changed, and needs its previousWorld kept; a sleeping one needs it
		// levelled once (two passes: the second sees last == now).
		if (!moved && state.offsetApplied && (!body.asleep || state.restPasses < 2)) {
			PropagateChildren(body.root, state.lastWorldFrame + 1 == frame, state.lastWorlds);
			state.lastWorldFrame = frame;
			state.restPasses = body.asleep ? state.restPasses + 1 : 0;
		}
		if (moved && ShiftChildren(body.root, delta) > 0) {
			PropagateChildren(body.root, state.lastWorldFrame + 1 == frame && state.offsetRoot == body.root, state.lastWorlds);
			state.lastWorldFrame = frame;
			state.restPasses = 0;
			state.childOffset = offset;
			state.offsetApplied = want > 0.0f;
			state.offsetRoot = body.root;
			state.appliedLift = want;
			if (itemSinkApplied.size() > 512)
				itemSinkApplied.clear();
			if (auto* node = body.root->AsNode())
				for (auto& child : node->GetChildren())
					if (child && !child->collisionObject) {
						itemSinkApplied[body.root] = { child.get(), child->local.translate, offset, want };
						break;
					}
		}
		if (state.offsetApplied)
			held++;

		// One line when an item comes to rest: every number the height is made of.
		if (body.asleep && !state.wasAsleep && itemSinkRestsLogged < 200) {
			itemSinkRestsLogged++;
			logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' RESTS | snow {:.1f} deep, surface z {:.1f}, land z {:.1f}, dug around {:.2f} | sink {:.2f}, embed {:.1f} -> today z {:.1f}, held to z {:.1f}{} | body z {:.1f}, collision underside z {:.1f}, used {:.1f} | lift +{:.1f}",
				state.formID, base->GetName(), snowDepth, surfaceZ, ground.z, carveAround, sink, embed, today, target, state.buried ? " (buried)" : "",
				body.world.z, body.hasBox ? body.undersideZ : body.world.z, bottomZ, state.appliedLift);
		}
		state.wasAsleep = body.asleep;

		if (first && wantReadout) {
			first = false;
			itemSinkReadout = std::format("nearest: {:08X} {} | {} | mass {:.1f} -> measure {:.3f} | roundness {:.2f}, thickness {:.1f} (lying {:.1f})\n"
										  "snow {:.1f} deep | sinks {:.0f} % of it + {:.1f} into the floor | held +{:.1f} | rest height {}",
				state.formID, base->GetName(), body.asleep ? "asleep" : "MOVING", body.mass, measure, roundness, thickness, lying,
				snowDepth, sink * 100.0f, embed, state.appliedLift,
				state.restValid ? std::format("z {:.1f}, {:.1f} under where today's snow would put it{} | snow dug away around it {:.0f} %",
									  target, today - target, state.buried ? " (BURIED, no trench)" : "", carveAround * 100.0f) :
								  std::string("not settled"));
		}
	}
	if (first && wantReadout)
		itemSinkReadout = "nearest: none - drop something on snow";
	itemSinkHeld.store(held, std::memory_order_relaxed);
	itemSinkWatched.store(static_cast<uint32_t>(picks.size()), std::memory_order_relaxed);

	// Unseen for a while: picked up, unloaded or out of range. Nothing to undo -
	// the offset lives on nodes that went with the 3D, or stays right for an
	// item that is merely far away.
	if ((frame & 0xFF) == 0)
		std::erase_if(itemSinkStates, [&](const auto& a_kv) { return frame - a_kv.second.seenFrame > 1200; });
}

void SnowDeformation::SaveItemSink(const SKSE::SerializationInterface* a_intfc)
{
	struct Row
	{
		uint32_t formID, baseID;
		float x, y, surface, depth, carve;
	};
	std::vector<Row> rows;
	{
		std::scoped_lock lock(itemSinkLock);
		for (const auto& [key, state] : itemSinkStates)
			if (state.restValid && state.formID && rows.size() < kMaxRecords)
				rows.push_back({ state.formID, state.baseID, state.restPos.x, state.restPos.y, state.restSurface, state.restDepth, state.restCarve });
		// Settled items this session never came near keep their records.
		for (const auto& [formID, record] : itemSinkLoaded)
			if (rows.size() < kMaxRecords)
				rows.push_back({ formID, record.baseID, record.x, record.y, record.surface, record.depth, record.carve });
	}
	if (rows.empty() || !a_intfc->OpenRecord(kItemSinkRecord, kItemSinkRecordVersion))
		return;
	const uint32_t count = static_cast<uint32_t>(rows.size());
	a_intfc->WriteRecordData(&count, sizeof(count));
	a_intfc->WriteRecordData(rows.data(), count * sizeof(Row));
	logger::info("[SNOW DEFORMATION] item sink: {} rest heights saved", count);
}

void SnowDeformation::LoadItemSink(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length)
{
	if (a_version != kItemSinkRecordVersion) {
		logger::warn("[SNOW DEFORMATION] item sink co-save is version {}, this build reads {}; dropped", a_version, kItemSinkRecordVersion);
		return;
	}
	uint32_t count = 0;
	if (a_length < sizeof(count) || a_intfc->ReadRecordData(&count, sizeof(count)) != sizeof(count) ||
		count > kMaxRecords || a_length != sizeof(count) + count * 28u) {
		logger::warn("[SNOW DEFORMATION] item sink co-save is {} bytes for {} rows; dropped", a_length, count);
		return;
	}
	std::unordered_map<uint32_t, ItemSinkRecord> restored;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t ids[2];
		float values[5];
		if (a_intfc->ReadRecordData(ids, sizeof(ids)) != sizeof(ids) || a_intfc->ReadRecordData(values, sizeof(values)) != sizeof(values)) {
			logger::warn("[SNOW DEFORMATION] item sink co-save short read at row {}; the rest dropped", i);
			break;
		}
		// A load order may have moved since the save; a form that no longer
		// resolves belongs to a plugin that is gone.
		RE::FormID formID = 0, baseID = 0;
		if (!a_intfc->ResolveFormID(ids[0], formID) || !a_intfc->ResolveFormID(ids[1], baseID))
			continue;
		if (!std::isfinite(values[0]) || !std::isfinite(values[1]) || !std::isfinite(values[2]) || !std::isfinite(values[3]) || !std::isfinite(values[4]))
			continue;
		restored[formID] = { baseID, values[0], values[1], values[2], values[3], std::clamp(values[4], 0.0f, 1.0f) };
	}
	std::scoped_lock lock(itemSinkLock);
	itemSinkLoaded = std::move(restored);
	logger::info("[SNOW DEFORMATION] item sink: {} of {} rest heights restored", itemSinkLoaded.size(), count);
}

void SnowDeformation::RegisterItemSinkCoSave()
{
	CoSave::GetSingleton()->Register(
		kItemSinkRecord, kItemSinkRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) { SaveItemSink(a_intfc); },
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) { LoadItemSink(a_intfc, a_version, a_length); },
		[this]() {
			// Before a load and on a new game. The offsets themselves go with the
			// 3D the load throws away.
			std::scoped_lock lock(itemSinkLock);
			itemSinkStates.clear();
			itemSinkLoaded.clear();
			itemSinkClaimsLogged = 0;
			itemSinkCandidatesStale.store(true, std::memory_order_release);
		});
}
