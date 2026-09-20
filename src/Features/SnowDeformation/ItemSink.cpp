// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Dropped items rest in the snow by weight and shape. Design, measurements and
// the rules every line here was paid for: MD plans/WEIGHT-SINK-PLAN.md.

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/CoSave.h"
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
	/** A saved rest is taken only by an item this close to where it was saved: a load jostles a pile by 10-30 units, and a recycled form ID is usually much further. */
	constexpr float kRecordMatch = 48.0f;
	constexpr size_t kMaxRecords = 4096;
	/** Melee weapons are authored anywhere from 9 to 35 for the same blade (steel vs iron greatsword); this much per unit of length at least. */
	constexpr float kWeaponMassPerLength = 0.3f;
	/** An item this far under where today's snow would put it is buried, and stops printing its trench. */
	constexpr float kBuriedMargin = 2.0f;
	/** Snow's grip on a body inside it, per second at full strength. Havok's damping is isotropic, so the linear part also slows the last of the fall: 686 u/s^2 over 6 = a 114 u/s sink. */
	constexpr float kGripLinear = 6.0f;
	constexpr float kGripAngular = 10.0f;
	/** Flat speed under which the snow holds fully, and over which a thrown item still slides. */
	constexpr float kGripSlow = 60.0f;
	constexpr float kGripFast = 240.0f;
	/** Snow over the body's underside at which the grip is full. */
	constexpr float kGripDeep = 24.0f;

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
		/** Up to four bodies, this frame only: the snow's grip is written to them. */
		RE::hkpRigidBody* rigid[4] = {};
		float speedFlat = 0.0f;
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
			if (out.bodies <= 4)
				out.rigid[out.bodies - 1] = hkpRigid;
			if (first) {
				first = false;
				out.asleep = hkpRigid->simulationIsland && !hkpRigid->simulationIsland->isInActiveIslandsArray;
				float v[4];
				_mm_storeu_ps(v, hkpRigid->motion.linearVelocity.quad);
				out.speedFlat = std::sqrt(v[0] * v[0] + v[1] * v[1]) * toGame;
			}
			// Havok's world box is the shape's own box turned with the body: for
			// anything round it hangs far under the shape (an apple: 10.7 under a
			// centre 5.5 above its skin), and the land clamp only helps an item
			// lying ON the land. Half-height as an ellipsoid's instead -
			// sqrt(sum((axis.z * extent)^2)) - exact for round shapes and for
			// anything lying flat, short only for a box balanced on an edge.
			if (const auto* shape = hkpRigid->collidable.GetShape()) {
				const auto& transform = hkpRigid->motion.motionState.transform;
				RE::hkAabb aabb;
				shape->GetAabbImpl(transform, 0.0f, aabb);
				RE::hkTransform identity;
				identity.rotation.col0 = { 1.0f, 0.0f, 0.0f, 0.0f };
				identity.rotation.col1 = { 0.0f, 1.0f, 0.0f, 0.0f };
				identity.rotation.col2 = { 0.0f, 0.0f, 1.0f, 0.0f };
				identity.translation = { 0.0f, 0.0f, 0.0f, 0.0f };
				RE::hkAabb box;
				shape->GetAabbImpl(identity, 0.0f, box);
				float low[4], high[4], boxLow[4], boxHigh[4], axis[3][4];
				_mm_storeu_ps(low, aabb.min.quad);
				_mm_storeu_ps(high, aabb.max.quad);
				_mm_storeu_ps(boxLow, box.min.quad);
				_mm_storeu_ps(boxHigh, box.max.quad);
				_mm_storeu_ps(axis[0], transform.rotation.col0.quad);
				_mm_storeu_ps(axis[1], transform.rotation.col1.quad);
				_mm_storeu_ps(axis[2], transform.rotation.col2.quad);
				float extent[3];
				float halfSquared = 0.0f;
				bool finite = std::isfinite(low[2]) && std::isfinite(high[2]);
				for (int i = 0; i < 3; ++i) {
					extent[i] = (boxHigh[i] - boxLow[i]) * toGame;
					finite = finite && std::isfinite(extent[i]) && extent[i] > 0.0f;
					halfSquared += (axis[i][2] * extent[i] * 0.5f) * (axis[i][2] * extent[i] * 0.5f);
				}
				if (finite) {
					const float centre = (low[2] + high[2]) * 0.5f * toGame;
					const float half = std::sqrt(halfSquared);
					out.undersideZ = out.hasBox ? std::min(out.undersideZ, centre - half) : centre - half;
					out.topZ = out.hasBox ? std::max(out.topZ, centre + half) : centre + half;
					out.hasBox = true;
					if (!out.hasLocal) {
						for (int i = 0; i < 3; ++i)
							out.local[i] = extent[i];
						out.hasLocal = true;
					}
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

	RE::NiAVObject* FirstFreeChild(RE::NiAVObject* a_root)
	{
		if (auto* node = a_root ? a_root->AsNode() : nullptr)
			for (auto& child : node->GetChildren())
				if (child && !child->collisionObject)
					return child.get();
		return nullptr;
	}

	// The offset is written on the mesh that carries it. States are dropped
	// by loads, roots are replaced and subtrees cloned with the offset still
	// in them; extra data goes wherever the transform goes.
	const RE::BSFixedString& LiftTag()
	{
		static const RE::BSFixedString tag("SnowDeformationItemLift");
		return tag;
	}

	bool ReadLiftTag(RE::NiAVObject* a_child, RE::NiPoint3& a_offset, float& a_lift)
	{
		auto* data = a_child ? a_child->GetExtraData<RE::NiFloatsExtraData>(LiftTag()) : nullptr;
		if (!data || data->size < 4 || !data->value)
			return false;
		a_offset = { data->value[0], data->value[1], data->value[2] };
		a_lift = data->value[3];
		return true;
	}

	void WriteLiftTag(RE::NiAVObject* a_child, const RE::NiPoint3& a_offset, float a_lift)
	{
		if (!a_child)
			return;
		if (auto* data = a_child->GetExtraData<RE::NiFloatsExtraData>(LiftTag()); data && data->size >= 4 && data->value) {
			data->value[0] = a_offset.x;
			data->value[1] = a_offset.y;
			data->value[2] = a_offset.z;
			data->value[3] = a_lift;
		} else if (auto* made = RE::NiFloatsExtraData::Create(LiftTag(), { a_offset.x, a_offset.y, a_offset.z, a_lift })) {
			a_child->AddExtraData(made);
		}
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

	/** The game's angles for a rotation (the inverse of EulerAnglesToAxesZXY); false near the pole or when the rebuilt matrix misses. */
	bool AnglesOf(const RE::NiMatrix3& a_m, RE::NiPoint3& a_out)
	{
		a_out = { std::asin(std::clamp(-a_m.entry[2][1], -1.0f, 1.0f)), std::atan2(a_m.entry[2][0], a_m.entry[2][2]), std::atan2(a_m.entry[0][1], a_m.entry[1][1]) };
		RE::NiMatrix3 back;
		back.EulerAnglesToAxesZXY(a_out);
		float worst = 0.0f;
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				worst = std::max(worst, std::abs(back.entry[i][j] - a_m.entry[i][j]));
		return worst < 0.01f;
	}

	/** Proven once a session on the cell's own statics: their 3D is their angles through EulerAnglesToAxesZXY. -1 until enough were seen. */
	int angleRule = -1;
	bool AngleRuleHolds(RE::TESObjectCELL* a_cell)
	{
		if (angleRule >= 0 || !a_cell)
			return angleRule == 1;
		int good = 0, bad = 0;
		float worstGood = 0.0f;
		a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			auto* root = base && base->Is(RE::FormType::Static) ? a_ref->Get3D() : nullptr;
			const auto& angle = a_ref->data.angle;
			if (!root || std::abs(angle.x) + std::abs(angle.y) + std::abs(angle.z) < 0.3f)
				return RE::BSContainer::ForEachResult::kContinue;
			RE::NiMatrix3 built;
			built.EulerAnglesToAxesZXY(angle);
			float worst = 0.0f;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					worst = std::max(worst, std::abs(built.entry[i][j] - root->world.rotate.entry[i][j]));
			if (worst < 0.01f) {
				good++;
				worstGood = std::max(worstGood, worst);
			} else {
				bad++;
			}
			return good + bad < 48 ? RE::BSContainer::ForEachResult::kContinue : RE::BSContainer::ForEachResult::kStop;
		});
		if (good + bad >= 6) {
			angleRule = good >= 9 * bad ? 1 : 0;
			logger::info("[SNOW DEFORMATION] item sink: angle rule checked on {} statics: {} match (worst {:.4f}), {} miss -> rotations {} written",
				good + bad, good, worstGood, bad, angleRule == 1 ? "are" : "are NOT");
		}
		return angleRule == 1;
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

		// What the meshes carry is read off them every frame, never remembered:
		// a load drops the state, replaces the root or clones the subtree with
		// the offset still in it, and lifting again draws the item at twice its
		// height (measured 2026-09-19 by capture, again 2026-09-20).
		{
			RE::NiPoint3 carried;
			float carriedLift = 0.0f;
			const bool tagged = ReadLiftTag(FirstFreeChild(body.root), carried, carriedLift);
			if (tagged && state.offsetRoot != body.root && itemSinkClaimsLogged < 96) {
				itemSinkClaimsLogged++;
				logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' {}: its meshes already carry +{:.1f}, kept",
					state.formID, base->GetName(), fresh ? "first seen" : "has a new root", carriedLift);
			}
			state.childOffset = tagged ? carried : RE::NiPoint3{};
			state.appliedLift = tagged ? carriedLift : 0.0f;
			state.offsetApplied = tagged && carriedLift > 0.0f;
			if (state.offsetRoot != body.root)
				state.offsetRoot = tagged ? body.root : nullptr;
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
		// Over the land under the item, not an absolute height: the rest then
		// holds for an item a load has shoved a few units along a slope.
		auto heightIn = [&](float a_rise, float a_depth, float a_carve) { return ground.z + a_rise - std::max(sink, a_carve) * a_depth - embed; };
		const float today = heightIn(rise, snowDepth, carveAround);

		// Snow buries: what is kept is the snow the item settled in. Rising
		// snow closes over it; falling or dug snow takes it down, and it stays
		// down. Kept as the snow rather than a height so the settings stay live.
		if (state.restValid && body.world.GetSquaredDistance(state.restPos) > kRestMove * kRestMove) {
			if (itemSinkClaimsLogged < 96) {
				itemSinkClaimsLogged++;
				logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' left its rest: {:.1f}, {:.1f} -> {:.1f}, {:.1f} ({})",
					state.formID, base->GetName(), state.restPos.x, state.restPos.y, body.world.x, body.world.y, body.asleep ? "asleep" : "awake");
			}
			state.restValid = false;
		}
		// Inside the grace a record survives its claim: a load that arrives in
		// two passes shows the items somewhere else first, and the rest taken
		// there is dropped the moment the second pass puts them back.
		if ((fresh || body.asleep) && !state.restValid) {
			if (auto found = itemSinkLoaded.find(state.formID); found != itemSinkLoaded.end()) {
				const bool grace = std::chrono::duration<float>(std::chrono::steady_clock::now() - itemSinkLoadedAt).count() < kItemSinkLoadGrace;
				const auto& record = found->second;
				const bool claimed = (grace || !state.claimed) && record.baseID == state.baseID &&
				                     std::abs(record.x - body.world.x) < kRecordMatch && std::abs(record.y - body.world.y) < kRecordMatch;
				if (claimed) {
					state.restValid = true;
					state.claimed = true;
					state.restRise = record.rise;
					state.restDepth = record.depth;
					state.restCarve = record.carve;
					state.restPos = body.world;
				}
				if ((claimed || fresh || !state.wasAsleep) && itemSinkClaimsLogged < 96) {
					itemSinkClaimsLogged++;
					logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' {} its saved rest (snow then {:.1f} deep rising {:.1f}, carved {:.2f}; today {:.1f} deep rising {:.1f}, carved {:.2f}; saved at {:.1f}, {:.1f}, now {:.1f}, {:.1f}{})",
						state.formID, base->GetName(), claimed ? "claims" : "REFUSES", record.depth, record.rise, record.carve, snowDepth, rise, carveAround,
						record.x, record.y, body.world.x, body.world.y, grace ? "; record kept" : "");
				}
				if (!grace)
					itemSinkLoaded.erase(found);
			}
		}
		// No snow data (a load's first frames, a cell edge) says nothing about
		// where the snow is; only real readings move the rest.
		const bool snowKnown = snowDepth >= 1.0f;
		float target = today;
		if (state.restValid) {
			if (snowKnown && today < heightIn(state.restRise, state.restDepth, state.restCarve)) {
				state.restRise = rise;
				state.restDepth = snowDepth;
				state.restCarve = carveAround;
			}
			target = heightIn(state.restRise, state.restDepth, state.restCarve);
		} else if (body.asleep && snowKnown) {
			state.restValid = true;
			state.restRise = rise;
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
			WriteLiftTag(FirstFreeChild(body.root), offset, want);
		}
		if (state.offsetApplied)
			held++;

		// Snow packs under what drops into it: the deeper the body is in, and
		// the less it is travelling, the harder it is held. Written only on a
		// change, under the world's write lock; the body's own damping goes back
		// when it sleeps or leaves the snow.
		{
			const float over = snowKnown ? surfaceZ - carveAround * snowDepth - std::max(body.hasBox ? body.undersideZ : body.world.z, ground.z) : 0.0f;
			float grip = enabled && !body.asleep ? std::clamp(settings.ItemSnowGripPercent, 0.0f, 100.0f) * 0.01f * Smooth(over / kGripDeep) *
														(1.0f - Smooth((body.speedFlat - kGripSlow) / (kGripFast - kGripSlow))) :
			                                       0.0f;
			if (grip < 0.02f)
				grip = 0.0f;
			if (std::abs(grip - state.grip) > 0.05f || (grip == 0.0f) != (state.grip == 0.0f)) {
				auto* cell = ref->GetParentCell();
				auto* world = cell ? cell->GetbhkWorld() : nullptr;
				if (world) {
					RE::BSWriteLockGuard guard(world->worldLock);
					for (auto* rigid : body.rigid) {
						if (!rigid)
							continue;
						auto& motion = rigid->motion.motionState;
						if (!state.gripBaseKnown) {
							state.gripBaseLinear = motion.linearDamping;
							state.gripBaseAngular = motion.angularDamping;
							state.gripBaseKnown = true;
						}
						motion.linearDamping = state.gripBaseLinear + grip * kGripLinear;
						motion.angularDamping = state.gripBaseAngular + grip * kGripAngular;
					}
					state.grip = grip;
				}
			}
		}

		// One line when an item comes to rest: every number the height is made of.
		if (body.asleep && !state.wasAsleep && itemSinkRestsLogged < 200) {
			itemSinkRestsLogged++;
			logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' RESTS at {:.1f}, {:.1f} (the game's own record of it {:.1f}, {:.1f}) | snow {:.1f} deep, surface z {:.1f}, land z {:.1f}, dug around {:.2f} | sink {:.2f}, embed {:.1f} -> today z {:.1f}, held to z {:.1f}{} | body z {:.1f}, collision underside z {:.1f}, used {:.1f} | lift +{:.1f}",
				state.formID, base->GetName(), body.world.x, body.world.y, ref->GetPositionX(), ref->GetPositionY(), snowDepth, surfaceZ, ground.z, carveAround, sink, embed, today, target, state.buried ? " (buried)" : "",
				body.world.z, body.hasBox ? body.undersideZ : body.world.z, bottomZ, state.appliedLift);
		}
		// The game keeps no running record of where a dropped item lies: carried
		// or rolled, it reloads where it was dropped. At rest its place goes into
		// the reference. (Marking it kHavokMoved instead, tried 2026-09-20, did
		// not move it and reloaded it floating.)
		if (body.asleep && !state.wasAsleep) {
			const RE::NiPoint3 place = body.root->world.translate;
			if (place.GetSquaredDistance(ref->data.location) > 1.0f) {
				auto* cell = ref->GetParentCell();
				const bool sameCell = cell && (cell->IsInteriorCell() || tes->GetCell(place) == cell);
				RE::NiPoint3 angle;
				const bool turned = sameCell && AnglesOf(body.root->world.rotate, angle) && AngleRuleHolds(cell);
				if (itemSinkClaimsLogged < 96) {
					itemSinkClaimsLogged++;
					logger::info("[SNOW DEFORMATION] item sink: {:08X} '{}' rests {:.1f} units from the game's record of it ({:.1f}, {:.1f}, {:.1f} -> {:.1f}, {:.1f}, {:.1f}): {}",
						state.formID, base->GetName(), std::sqrt(place.GetSquaredDistance(ref->data.location)), ref->data.location.x, ref->data.location.y, ref->data.location.z,
						place.x, place.y, place.z, !sameCell ? "in another cell, left alone" : turned ? "place and rotation written" : "place written, rotation left");
				}
				if (sameCell) {
					ref->data.location = place;
					if (turned)
						ref->data.angle = angle;
					ref->AddChange(RE::TESObjectREFR::ChangeFlags::kMoved);
				}
			}
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
		float x, y, rise, depth, carve;
	};
	std::vector<Row> rows;
	{
		std::scoped_lock lock(itemSinkLock);
		std::unordered_set<uint32_t> written;
		for (const auto& [key, state] : itemSinkStates)
			if (state.restValid && state.formID && rows.size() < kMaxRecords && written.insert(state.formID).second)
				rows.push_back({ state.formID, state.baseID, state.restPos.x, state.restPos.y, state.restRise, state.restDepth, state.restCarve });
		// Settled items this session never came near keep their records.
		for (const auto& [formID, record] : itemSinkLoaded)
			if (rows.size() < kMaxRecords && written.insert(formID).second)
				rows.push_back({ formID, record.baseID, record.x, record.y, record.rise, record.depth, record.carve });
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
	itemSinkLoadedAt = std::chrono::steady_clock::now();
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
