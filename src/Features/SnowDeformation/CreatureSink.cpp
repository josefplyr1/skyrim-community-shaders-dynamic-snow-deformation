#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"

namespace
{
	constexpr float kPersonStepLoad = 51.0f;
	constexpr float kFistStrength = 5.0f;
	constexpr float kFistDensity = 100.0f;
	constexpr float kStrengthDecade = 125.0f;
	constexpr float kMaxLiftShare = 0.5f;
	constexpr float kLiftEaseSeconds = 0.2f;
	constexpr float kCorpseSettleSeconds = 1.0f;
	constexpr uint32_t kBuildQuickFrames = 15;
	constexpr uint32_t kBuildSlowFrames = 120;
	constexpr uint32_t kBuildQuickReads = 8;
	constexpr uint32_t kProbeFrames = 8;
	constexpr uint32_t kForgetFrames = 600;
	constexpr size_t kMaxCapsules = 64;
	constexpr size_t kMaxTracked = 96;
	constexpr RE::FormID kActorTypeNPC = 0x13794;

	struct BuildReading
	{
		float volume = 0.0f;
		float lowest = FLT_MAX;
		float top = -FLT_MAX;
		float belly = 0.0f;
		float largest = 0.0f;
		uint32_t capsules = 0;
		uint32_t simulated = 0;
	};

	RE::NiPoint3 HavokPoint(const RE::hkTransform& a_t, const float a_v[4])
	{
		float c0[4], c1[4], c2[4], t[4];
		_mm_storeu_ps(c0, a_t.rotation.col0.quad);
		_mm_storeu_ps(c1, a_t.rotation.col1.quad);
		_mm_storeu_ps(c2, a_t.rotation.col2.quad);
		_mm_storeu_ps(t, a_t.translation.quad);
		return { c0[0] * a_v[0] + c1[0] * a_v[1] + c2[0] * a_v[2] + t[0],
			c0[1] * a_v[0] + c1[1] * a_v[1] + c2[1] * a_v[2] + t[1],
			c0[2] * a_v[0] + c1[2] * a_v[1] + c2[2] * a_v[2] + t[2] };
	}

	BuildReading ReadBuild(RE::NiAVObject* a_root)
	{
		BuildReading out;
		const float toGame = RE::bhkWorld::GetWorldScaleInverse();
		float largestUnderside = 0.0f;
		RE::BSVisit::TraverseScenegraphCollision(a_root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
			if (out.capsules >= kMaxCapsules)
				return RE::BSVisit::BSVisitControl::kStop;
			auto* body = a_object ? a_object->body.get() : nullptr;
			auto* bhkRigid = body ? body->AsBhkRigidBody() : nullptr;
			auto* hkpRigid = bhkRigid ? skyrim_cast<RE::hkpRigidBody*>(bhkRigid->referencedObject.get()) : nullptr;
			const RE::hkpShape* shape = hkpRigid ? hkpRigid->collidable.GetShape() : nullptr;
			if (!shape || shape->type != RE::hkpShapeType::kCapsule)
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto* capsule = static_cast<const RE::hkpCapsuleShape*>(shape);
			float va[4], vb[4];
			_mm_storeu_ps(va, capsule->vertexA.quad);
			_mm_storeu_ps(vb, capsule->vertexB.quad);
			RE::NiPoint3 a, b;
			bool simulated = false;
			if (hkpRigid->world) {
				const auto& transform = hkpRigid->motion.motionState.transform;
				a = HavokPoint(transform, va) * toGame;
				b = HavokPoint(transform, vb) * toGame;
				simulated = true;
			} else if (auto* node = a_object->sceneObject) {
				a = node->world.translate + node->world.rotate * (RE::NiPoint3{ va[0], va[1], va[2] } * toGame);
				b = node->world.translate + node->world.rotate * (RE::NiPoint3{ vb[0], vb[1], vb[2] } * toGame);
			} else {
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			const float r = capsule->radius * toGame;
			if (!std::isfinite(a.z) || !std::isfinite(b.z) || !(r > 0.0f))
				return RE::BSVisit::BSVisitControl::kContinue;
			out.volume += std::numbers::pi_v<float> * r * r * (a.GetDistance(b) + 4.0f / 3.0f * r);
			const float under = std::min(a.z, b.z) - r;
			out.lowest = std::min(out.lowest, under);
			out.top = std::max(out.top, std::max(a.z, b.z) + r);
			if (r > out.largest) {
				out.largest = r;
				largestUnderside = under;
			}
			out.capsules++;
			out.simulated += simulated ? 1u : 0u;
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		if (out.capsules)
			out.belly = std::max(largestUnderside - out.lowest, 0.0f);
		return out;
	}

	RE::NiAVObject* FindLiftNode(RE::NiAVObject* a_root)
	{
		static const RE::BSFixedString animRoot("NPC Root [Root]");
		auto* skeleton = a_root ? a_root->GetObjectByName(animRoot) : nullptr;
		auto* parent = skeleton ? skeleton->parent : nullptr;
		return parent && parent != a_root ? parent : nullptr;
	}

	bool AttachedTo(const RE::NiAVObject* a_node, const RE::NiAVObject* a_root)
	{
		int depth = 0;
		for (auto* p = a_node; p && depth < 8; p = p->parent, ++depth)
			if (p == a_root)
				return true;
		return false;
	}

	const RE::BSFixedString& LiftTag()
	{
		static const RE::BSFixedString tag("SnowDeformationCreatureLift");
		return tag;
	}

	bool ReadLiftTag(RE::NiAVObject* a_node, float& a_lift, float& a_written)
	{
		auto* data = a_node->GetExtraData<RE::NiFloatsExtraData>(LiftTag());
		if (!data || data->size < 2 || !data->value)
			return false;
		a_lift = data->value[0];
		a_written = data->value[1];
		return true;
	}

	float ApplyLift(RE::NiAVObject* a_node, float a_lift)
	{
		float carried = 0.0f, written = 0.0f;
		const bool tagged = ReadLiftTag(a_node, carried, written);
		if (!tagged && a_lift <= 0.0f)
			return 0.0f;
		float base = a_node->local.translate.z;
		float inBones = 0.0f;
		if (tagged && written == base) {
			base -= carried;
			inBones = carried;
		}
		const float z = base + a_lift;
		if (z != a_node->local.translate.z) {
			a_node->local.translate.z = z;
			RE::NiUpdateData data{};
			a_node->UpdateWorldData(&data);
		}
		if (auto* data = a_node->GetExtraData<RE::NiFloatsExtraData>(LiftTag()); data && data->size >= 2 && data->value) {
			data->value[0] = a_lift;
			data->value[1] = z;
		} else if (auto* made = RE::NiFloatsExtraData::Create(LiftTag(), { a_lift, z })) {
			a_node->AddExtraData(made);
		}
		return inBones;
	}

	float Carried(float a_density, float a_load)
	{
		const float strength = kFistStrength * std::pow(10.0f, (a_density - kFistDensity) / kStrengthDecade);
		const float t = std::clamp(std::log2(strength / std::max(a_load, 1e-3f)) + 0.5f, 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	bool Upright(RE::Actor* a_actor)
	{
		if (a_actor->IsDead() || a_actor->IsInRagdollState() || a_actor->AsActorState()->GetKnockState() != RE::KNOCK_STATE_ENUM::kNormal)
			return false;
		auto* controller = a_actor->GetCharController();
		return controller && controller->context.currentState == RE::hkpCharacterStateType::kOnGround;
	}
}

bool SnowDeformation::MeasureCreatureBuild(RE::NiAVObject* a_root, CreatureBuild& a_build, bool a_upright, uint32_t a_frame, float a_baseZ)
{
	if (!a_root)
		return false;
	const float scale = a_root->world.scale;
	if (a_build.root != a_root || std::abs(scale - a_build.scale) > 1e-3f)
		a_build = { a_root, scale };
	if (a_build.reads > 0 && a_frame < a_build.nextFrame)
		return a_build.capsules > 0;
	const BuildReading reading = ReadBuild(a_root);
	a_build.reads++;
	a_build.nextFrame = a_frame + (a_build.reads < kBuildQuickReads ? kBuildQuickFrames : kBuildSlowFrames);
	if (!reading.capsules)
		return a_build.capsules > 0;
	a_build.volume = reading.volume;
	a_build.height = reading.top - reading.lowest;
	a_build.largest = reading.largest;
	a_build.lowestRel = reading.lowest - a_baseZ;
	a_build.capsules = reading.capsules;
	a_build.simulated = reading.simulated;
	if (a_upright || a_build.belly <= 0.0f)
		a_build.belly = std::max(a_build.belly, reading.belly);
	return true;
}

float SnowDeformation::CreatureLiftOf(uint32_t a_formID, bool* a_havokFollows, float* a_weight)
{
	std::scoped_lock lock(creatureLiftLock);
	auto found = creatureLiftsDrawn.find(a_formID);
	if (found == creatureLiftsDrawn.end()) {
		if (a_weight)
			*a_weight = 0.0f;
		return 0.0f;
	}
	if (a_havokFollows)
		*a_havokFollows = found->second.havokFollows;
	if (a_weight)
		*a_weight = found->second.weight;
	return found->second.inBones;
}

void SnowDeformation::CreatureSinkUpdate()
{
	static uint32_t lastFrame = ~0u;
	const uint32_t frame = globals::state->frameCount;
	if (lastFrame == frame)
		return;
	lastFrame = frame;
	const bool enabled = loaded && settings.EnableSnowDeformation && settings.CreatureSink;
	if (!enabled && creatureLifts.empty())
		return;
	auto* player = globals::game::player;
	auto* tes = globals::game::tes;
	auto* lists = RE::ProcessLists::GetSingleton();
	if (!player || !tes || !lists)
		return;
	const auto started = std::chrono::steady_clock::now();
	const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 0.0f) : 0.0f;
	const float ease = 1.0f - std::exp(-dt / kLiftEaseSeconds);

	const RE::NiPoint3 playerPos = player->GetPosition();
	const bool baseline = MeasureCreatureBuild(player->Get3D(false), playerBuild, Upright(player), frame, playerPos.z) && playerBuild.volume > 0.0f;
	const SnowCondition here = GetSnowCondition();
	static auto* npcType = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNPC);
	const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
	const bool wantReadout = creatureSinkReadoutWanted.exchange(false, std::memory_order_acq_rel);
	uint32_t nearestID = 0;
	std::string nearestName;
	float nearestDistSq = FLT_MAX;
	uint32_t watched = 0, lifted = 0;

	auto visit = [&](const RE::ActorHandle& a_handle) {
		auto ptr = a_handle.get();
		RE::Actor* actor = ptr.get();
		if (!actor || actor->IsPlayerRef())
			return;
		auto* root = actor->Get3D(false);
		if (!root)
			return;
		const uint32_t formID = actor->formID;
		auto* race = actor->GetRace();
		const bool person = race && npcType && race->HasKeyword(npcType);
		auto found = creatureLifts.find(formID);
		if (found == creatureLifts.end() && (!enabled || person || actor->IsDead() || creatureLifts.size() >= kMaxTracked))
			return;
		auto& state = found != creatureLifts.end() ? found->second : creatureLifts[formID];
		if (state.root != root) {
			const float settle = state.settleFrom;
			state = {};
			state.root = root;
			state.settleFrom = settle;
			state.node = FindLiftNode(root);
		} else if (state.node && !AttachedTo(state.node, root)) {
			state.node = FindLiftNode(root);
		}
		state.seenFrame = frame;
		watched++;

		const RE::NiPoint3 position = actor->GetPosition();
		state.dead = actor->IsDead();
		const uint32_t readsBefore = state.build.reads;
		const bool built = MeasureCreatureBuild(root, state.build, Upright(actor), frame, position.z);
		if (built && state.build.reads != readsBefore && state.lift > 2.0f) {
			state.havokFollows = state.build.lowestRel > 0.5f * state.lift;
			state.havokKnown = true;
		}
		state.weight = built && baseline ? state.build.volume / playerBuild.volume : 0.0f;

		float target = 0.0f;
		state.gate = "";
		state.depth = 0.0f;
		auto* controller = actor->GetCharController();
		const auto moveState = controller ? controller->context.currentState : RE::hkpCharacterStateType::kOnGround;
		float landZ = position.z;
		tes->GetLandHeight(position, landZ);
		const auto* support = controller ? controller->supportBody.get() : nullptr;
		const bool onLand = support ? support->collidable.GetCollisionLayer() == RE::COL_LAYER::kTerrain : std::abs(position.z - landZ) < 2.0f;
		if (!enabled)
			state.gate = "off";
		else if (person)
			state.gate = "a person";
		else if (state.dead)
			state.gate = "dead";
		else if (!state.node)
			state.gate = "no node to lift by";
		else if (actor->IsOnMount())
			state.gate = "riding";
		else if (actor->AsActorState()->IsSwimming() || actor->IsInWater())
			state.gate = "in water";
		else if (moveState == RE::hkpCharacterStateType::kFlying)
			state.gate = "flying";
		else if (moveState == RE::hkpCharacterStateType::kInAir || moveState == RE::hkpCharacterStateType::kJumping) {
			state.gate = "in the air";
			target = state.target;
		} else if (!onLand || position.z - landZ > kElevatedStampCutoff)
			state.gate = "not on the land";
		else if (!baseline)
			state.gate = "no build read off the player";
		else if (!built)
			state.gate = "no capsules";
		else if (state.build.lowestRel - (state.havokFollows ? state.inBones : 0.0f) > std::max(settings.FloatingActorBand, 0.0f))
			state.gate = "floating";
		else {
			const float depth = APISnowDepthAt(position.x, position.y);
			state.depth = depth;
			if (depth < 1.0f)
				state.gate = "no snow";
			else if (state.weight >= 1.0f)
				state.gate = "as heavy as you";
			else {
				state.density = SnowConditionAt(here.temperature - kClimateLapse * (position.z - playerPos.z)).density;
				state.support = Carried(state.density, kPersonStepLoad * std::cbrt(state.weight));
				state.byWeight = depth * (1.0f - std::lerp(1.0f, state.weight, state.support));
				if (((frame + formID) % kProbeFrames) == 0) {
					{
						std::scoped_lock storeLock(trenchStoreMutex);
						state.floorShare = 1.0f - std::clamp(SampleTrenchStore(worldspace, position.x, position.y), 0.0f, 1.0f);
						trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
						trenchSampleTile = nullptr;
					}
					const float margin = 0.25f * state.build.largest;
					const RE::NiPoint3 from{ position.x, position.y, position.z + state.build.height + 1.0f };
					const RE::NiPoint3 to{ from.x, from.y, from.z + depth + margin };
					const float toHavok = RE::bhkWorld::GetWorldScale();
					RE::bhkPickData pick{};
					pick.rayInput.from = RE::hkVector4(from.x * toHavok, from.y * toHavok, from.z * toHavok, 0.0f);
					pick.rayInput.to = RE::hkVector4(to.x * toHavok, to.y * toHavok, to.z * toHavok, 0.0f);
					actor->GetCollisionFilterInfo(pick.rayInput.filterInfo);
					tes->Pick(pick);
					state.clearance = FLT_MAX;
					if (const auto* hit = pick.rayOutput.rootCollidable) {
						auto* ref = RE::TESHavokUtilities::FindCollidableRef(*hit);
						if (!ref || !ref->As<RE::Actor>())
							state.clearance = std::max(pick.rayOutput.hitFraction * (depth + margin) - margin, 0.0f);
					}
				}
				target = std::min(state.byWeight, kMaxLiftShare * depth);
				if (state.build.belly > 0.0f)
					target = std::max(target, depth - state.build.belly);
				target = std::clamp(std::min({ target, depth * state.floorShare, state.clearance }), 0.0f, depth);
			}
		}
		state.target = target;

		if (state.dead && state.lift > 0.0f) {
			if (state.settleFrom < 0.0f)
				state.settleFrom = state.lift;
			state.lift = std::max(state.lift - state.settleFrom * dt / kCorpseSettleSeconds, 0.0f);
		} else {
			if (!state.dead)
				state.settleFrom = -1.0f;
			state.lift += (target - state.lift) * ease;
			if (target <= 0.0f && state.lift < 0.25f)
				state.lift = 0.0f;
		}
		state.inBones = state.node ? ApplyLift(state.node, state.lift) : 0.0f;
		if (state.lift > 0.0f)
			lifted++;

		if (race && built && state.weight > 0.0f && creatureRacesLogged.size() < 128 && creatureRacesLogged.insert(race->formID).second)
			logger::info("[SNOW DEFORMATION] creature weight: {} '{}' ({:08X}) at scale {:.2f}: {} capsules ({} simulated), volume {:.0f}, belly {:.1f}, height {:.1f} -> {:.2f} of the player's weight ({:.0f}); {}",
				race->GetFormEditorID() ? race->GetFormEditorID() : "", actor->GetDisplayFullName() ? actor->GetDisplayFullName() : "", formID, root->world.scale,
				state.build.capsules, state.build.simulated, state.build.volume,
				state.build.belly, state.build.height, state.weight, playerBuild.volume,
				state.node ? std::format("lifted by '{}'", state.node->name.c_str() ? state.node->name.c_str() : "") : std::string("NO node to lift by: never lifted"));
		if (wantReadout) {
			const float distSq = playerPos.GetSquaredDistance(position);
			if (distSq < nearestDistSq) {
				nearestDistSq = distSq;
				nearestID = formID;
				nearestName = actor->GetDisplayFullName() ? actor->GetDisplayFullName() : "";
			}
		}
	};
	for (const auto& handle : lists->highActorHandles)
		visit(handle);
	for (const auto& handle : lists->middleHighActorHandles)
		visit(handle);

	std::erase_if(creatureLifts, [&](const auto& a_kv) {
		const auto& state = a_kv.second;
		const bool settled = state.lift <= 0.0f && state.inBones <= 0.0f;
		return frame - state.seenFrame > kForgetFrames || (settled && (!enabled || state.dead));
	});
	std::string text;
	if (wantReadout) {
		text = baseline ? std::format("you: volume {:.0f}, belly {:.1f}, {} capsules | creatures watched {} | lifted {}",
							  playerBuild.volume, playerBuild.belly, playerBuild.capsules, watched, lifted) :
		                  std::string("no build read off the player yet: nothing is lifted");
		if (auto it = creatureLifts.find(nearestID); it != creatureLifts.end()) {
			const auto& s = it->second;
			text += std::format("\nnearest: {} | {}", nearestName, *s.gate ? s.gate : "lifted");
			text += std::format("\nweight {:.2f} of yours | belly {:.1f}, height {:.1f}, {} capsules ({} simulated){}",
				s.weight, s.build.belly, s.build.height, s.build.capsules, s.build.simulated,
				s.havokKnown ? (s.havokFollows ? " | its ragdoll rises with the lift" : " | its ragdoll stays on the ground") : "");
			text += std::format("\nsnow {:.1f} deep, {:.0f} kg/m3 | carried {:.0f} % | by weight {:.1f}, cap {:.1f}, belly floor {:.1f}, trench floor {:.1f}, overhead {}",
				s.depth, s.density, s.support * 100.0f, s.byWeight, kMaxLiftShare * s.depth, s.depth - s.build.belly, s.depth * s.floorShare,
				s.clearance < FLT_MAX ? std::format("{:.1f}", s.clearance) : std::string("clear"));
			text += std::format("\ntarget {:.1f} | drawn {:.1f} (the bones carry {:.1f})", s.target, s.lift, s.inBones);
		} else {
			text += "\nnearest: none tracked";
		}
	}
	{
		std::scoped_lock lock(creatureLiftLock);
		creatureLiftsDrawn.clear();
		for (const auto& [formID, state] : creatureLifts)
			if (state.inBones > 0.0f || state.weight > 0.0f)
				creatureLiftsDrawn[formID] = { state.inBones, state.havokFollows, state.weight };
		if (wantReadout)
			creatureSinkReadout = std::move(text);
	}
	creatureSinkWatched.store(watched, std::memory_order_relaxed);
	creatureSinkLifted.store(lifted, std::memory_order_relaxed);
	creatureSinkMicros.store(std::chrono::duration<float, std::micro>(std::chrono::steady_clock::now() - started).count(), std::memory_order_relaxed);
}
