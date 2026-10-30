// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/AlphaBuild.h"

#include "Deferred.h"
#include "Features/Skylighting.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileSystem.h"
#include "Utils/Game.h"

// Snow on bodies (MD plans/BODY-SNOW-PLAN.md): R0's probes and S1's store and
// mapping. Rationale in CODE-NOTES.md, "Snow on bodies".

namespace
{
	// Coat shader keys (vertex << 32 | pixel) this and earlier sessions had to
	// compile on demand, kept beside SettingsUser.json so a ShaderCache clear
	// leaves them; LoadShaders queues them with the game's own.
	std::mutex coatKeyLock;
	std::set<uint64_t> coatKeys;
	bool coatKeysDirty = false;

	std::filesystem::path CoatKeyPath()
	{
		return Util::PathHelpers::GetCommunityShaderPath() / "SnowBodyCoatShaders.txt";
	}

	// The shader's speck fit (SnowLighting.hlsli kCoatDotRank, kCoatSpeckMax):
	// a share q of the balls on covers 1 - e^-L, where q = L (x + y L).
	constexpr float kSpeckRank[2] = { 0.9212f, -0.0764f };
	constexpr float kSpeckMax = 0.5f;

	constexpr uint32_t kDumpLinesMax = 900;
	// S1's cadence (estimates): the near tier is picked every kPickFrames, an
	// actor keeps its slots kKeepFrames past its last pick, its 3D is walked
	// again every kRewalkFrames (staggered by form ID), and at most
	// kWalksPerFrame walks run in one frame.
	constexpr uint32_t kPickFrames = 30;
	constexpr uint32_t kKeepFrames = 120;
	constexpr uint32_t kRewalkFrames = 300;
	constexpr uint32_t kWalksPerFrame = 4;
	// A draw without a record walks its actor again, at most this often.
	constexpr uint32_t kMissFrames = 10;
	// S3's cadence (section 2.2, estimates): the player every kWritePlayerFrames,
	// at most kWritesPerFrame actors' coat passes a frame, the rest in turn.
	constexpr uint32_t kWritePlayerFrames = 2;
	constexpr uint32_t kWritesPerFrame = 4;
	// Round 2: the time between an actor's updates builds its coat, counted up
	// to kWriteGapMax (estimate). The look draws a stored contact of
	// kCoatContactFront as a full coat (SnowLighting.hlsli kCoatSpeckFront).
	constexpr float kWriteGapMax = 0.5f;
	constexpr float kCoatContactFront = 0.5f;

	// SnowContactCapture.hlsl BodyCoatWriteCB (VS b3) and BodyCoatResolveCB (CS b0).
	struct alignas(16) BodyCoatWriteCB
	{
		float4 ModelToAtlas[3];
		float4 AtlasLo;
		float4 AtlasHi;
		float Growth;
		float pad[3];
	};
	static_assert(sizeof(BodyCoatWriteCB) == 96);
	struct alignas(16) BodyCoatResolveCB
	{
		uint32_t Slots[32];
		uint32_t Count;
		uint32_t Cap;
		uint32_t Seed;
		uint32_t pad;
	};
	static_assert(sizeof(BodyCoatResolveCB) == 144);
	constexpr auto kWritePath = L"Data\\Shaders\\SnowDeformation\\SnowContactCapture.hlsl";
	bool IsLit(const RE::BSShaderProperty* a_property)
	{
		return a_property && a_property->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get();
	}

	bool IsRefraction(const RE::BSShaderProperty* a_property)
	{
		return a_property && a_property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction);
	}

	RE::BSShaderMaterial::Feature FeatureOf(const RE::BSShaderProperty* a_property)
	{
		auto* material = a_property ? static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material) : nullptr;
		return material ? material->GetFeature() : RE::BSShaderMaterial::Feature::kDefault;
	}

	bool HiddenByGame(const RE::NiAVObject* a_geometry, const RE::NiAVObject* a_root, bool a_firstPersonPlayer)
	{
		for (const RE::NiAVObject* n = a_geometry; n; n = n->parent)
			if (n->GetAppCulled() && !(a_firstPersonPlayer && n == a_root))
				return true;
		return false;
	}

	const char* NameOf(const RE::NiAVObject* a_object)
	{
		return a_object && a_object->name.c_str() ? a_object->name.c_str() : "";
	}

	uint32_t TrianglesOf(RE::BSGeometry* a_geometry)
	{
		auto* triShape = a_geometry->AsTriShape();
		return triShape ? uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) : 0u;
	}

	uint32_t DepthBelow(const RE::NiAVObject* a_node, const RE::NiAVObject* a_root)
	{
		uint32_t depth = 0;
		for (const RE::NiAVObject* n = a_node; n && n != a_root; n = n->parent)
			depth++;
		return depth;
	}

	struct SkinBone
	{
		std::string name;
		const RE::NiAVObject* node;
		RE::NiTransform skinToBone;
		/** @brief The bone's bind pose in the skin's own space; its translate is the bone's origin there. */
		RE::NiTransform boneToSkin;
		RE::NiBound bound;
	};

	std::vector<SkinBone> BonesOf(const RE::NiSkinInstance* a_skin, const RE::NiSkinData* a_data)
	{
		std::vector<SkinBone> bones;
		const uint32_t count = a_data->GetBoneCount();
		bones.reserve(count);
		for (uint32_t b = 0; b < count; ++b) {
			const RE::NiAVObject* node = a_skin->bones[b];
			if (!node)
				continue;
			const RE::NiTransform& skinToBone = a_data->GetBoneDataSkinToBone(b);
			bones.push_back({ NameOf(node), node, skinToBone, skinToBone.Invert(), a_data->GetBoneDataBound(b) });
		}
		return bones;
	}

	/** @brief Each bone's bind pose in a frame's space, by name. */
	using BindMap = std::unordered_map<std::string, RE::NiTransform>;

	struct SkinMap
	{
		bool chained = false;
		/** @brief The skin's space into the frame's: bind(via) x skinToBone(via). */
		RE::NiTransform toAnchor;
		std::string via;
		uint32_t shared = 0;
		uint32_t agree = 0;
		float residual = 0.0f;
		/** @brief Shared bones the chosen map puts more than a voxel off: name, distance, bone-sphere radius. */
		std::vector<std::tuple<std::string, float, float>> outliers;
	};

	// The transform skinning itself applies: a vertex bound to bone b lands at
	// bone world x skinToBone(b) x v, so bind(b) x skinToBone(b) maps the skin
	// into the frame whatever its rootParentToSkin claims. Every shared bone
	// proposes one; the proposal most of them agree with wins, so a bone a
	// skin lists but never weights cannot pull it off.
	SkinMap MapSkin(const std::vector<SkinBone>& a_bones, const BindMap& a_bind, float a_voxel)
	{
		SkinMap best;
		std::vector<std::pair<const SkinBone*, const RE::NiTransform*>> known;
		for (const auto& bone : a_bones)
			if (auto it = a_bind.find(bone.name); it != a_bind.end())
				known.emplace_back(&bone, &it->second);
		best.shared = uint32_t(known.size());
		if (known.empty())
			return best;
		int bestAgree = -1;
		float bestResidual = 0.0f;
		for (const auto& [candidate, candidateBind] : known) {
			const RE::NiTransform map = *candidateBind * candidate->skinToBone;
			int agree = 0;
			float residual = 0.0f;
			for (const auto& [bone, bind] : known) {
				const float r = (map * bone->boneToSkin.translate).GetDistance(bind->translate);
				if (r <= a_voxel) {
					agree++;
					residual = std::max(residual, r);
				}
			}
			const bool better = agree > bestAgree || (agree == bestAgree && (residual < bestResidual || (residual == bestResidual && candidate->name < best.via)));
			if (better) {
				bestAgree = agree;
				bestResidual = residual;
				best.toAnchor = map;
				best.via = candidate->name;
			}
		}
		best.chained = true;
		best.agree = uint32_t(bestAgree);
		best.residual = bestResidual;
		for (const auto& [bone, bind] : known) {
			const float r = (best.toAnchor * bone->boneToSkin.translate).GetDistance(bind->translate);
			if (r > a_voxel)
				best.outliers.emplace_back(bone->name, r, bone->bound.radius);
		}
		return best;
	}

	std::string OutlierText(const SkinMap& a_map)
	{
		std::string text;
		for (const auto& [name, distance, radius] : a_map.outliers)
			text += std::format("{}'{}' {:.1f} off (bone sphere {:.1f})", text.empty() ? "" : ", ", name, distance, radius);
		return text;
	}

	/** @brief A slot's colour in the debug views: golden-angle hues, so neighbours differ. */
	RE::NiPoint3 SlotColour(int a_slot, int a_half)
	{
		const float h = std::fmod(0.618034f * float(a_slot) + (a_half > 0 ? 0.5f : 0.0f), 1.0f) * 6.0f;
		const float x = 1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f);
		const int sector = int(h) % 6;
		const float rgb[6][3] = { { 1, x, 0 }, { x, 1, 0 }, { 0, 1, x }, { 0, x, 1 }, { x, 0, 1 }, { 1, 0, x } };
		return { 0.25f + 0.75f * rgb[sector][0], 0.25f + 0.75f * rgb[sector][1], 0.25f + 0.75f * rgb[sector][2] };
	}
}

// The coat's frames, slots and records (S1). Render thread only: Prepass
// fills it, the Lighting hook reads it.
struct SnowDeformation::BodyCoatStore
{
	/** @brief A frame: every bone's bind in one skin's own space, the seed's. The body's is kept for the actor's life. */
	struct Frame
	{
		std::string seed;
		std::string anchor;
		BindMap bind;
		bool bounded = false;
		float boundsMin[3] = {};
		float boundsMax[3] = {};
		int slot = -1;
		int half = -1;
		/** @brief Atlas axis a reads canonical axis axisOf[a]: texel = canonical x scale + offset. */
		int axisOf[3] = { 0, 1, 2 };
		float scale[3] = { 1.0f, 1.0f, 1.0f };
		float offset[3] = {};
		float lo[3] = {};
		float hi[3] = {};
		bool seen = false;
	};
	/** @brief One lit skin of a walk, and where it maps. */
	struct Skin
	{
		RE::BSGeometry* geometry = nullptr;
		const RE::NiSkinInstance* skin = nullptr;
		std::vector<SkinBone> bones;
		RE::NiTransform rootParentToSkin;
		uint32_t triangles = 0;
		int feature = 0;
		int slot = -1;
		bool dynamic = false;
		bool hidden = false;
		bool firstPerson = false;
		/** @brief 0 the body frame, 1 and up a component, -1 unmapped. */
		int frame = -1;
		SkinMap map;
		bool outside = false;
	};
	struct Walked
	{
		std::vector<Skin> skins;
		std::vector<std::string> gearLines;
		/** @brief Component halves a walk dropped: slot, half. */
		std::vector<std::pair<int, int>> freed;
		/** @brief Skinned geometry left out (overlays, proxies, decals, FX, eyes, brows, scars), so its draws are not taken for new gear. */
		std::vector<const RE::BSGeometry*> skipped;
		uint32_t owned = 0, unowned = 0, firstOwned = 0, firstUnowned = 0;
		uint32_t decals = 0, fx = 0, eyes = 0, noNormals = 0;
		uint32_t gear = 0, gearSkinned = 0, rigidWorn = 0, sheaths = 0, attached = 0, gearTriangles = 0;
		float gearRadiusMax = 0.0f;
	};
	struct Shape
	{
		const RE::BSGeometry* geometry;
		const RE::NiSkinInstance* skin;
		uint32_t frame;
		RE::NiTransform toCanonical;
		bool hair;
	};
	struct Actor
	{
		bool player = false;
		RE::ActorHandle handle;
		const RE::TESRace* race = nullptr;
		const RE::NiAVObject* root3D = nullptr;
		const RE::NiAVObject* first3D = nullptr;
		Frame body;
		std::vector<Frame> parts;
		std::vector<Shape> shapes;
		std::vector<const RE::BSGeometry*> skipped;
		uint32_t lastPicked = 0;
		uint32_t lastWalked = 0;
		bool walk = true;
		bool refused = false;
		bool dead = false;
		// S3: the time since its last coat update, and the largest jolt in it.
		float sinceWrite = 0.0f;
		float joltHeld = 0.0f;
	};
	struct Pick
	{
		float d2;
		RE::NiPointer<RE::Actor> actor;
		RE::ActorHandle handle;
		bool player;
	};

	std::unordered_map<uint32_t, Actor> actors;
	std::unordered_set<uint32_t> missed;
	std::unordered_map<const RE::BSGeometry*, uint32_t> recordOf;
	std::unordered_set<const RE::BSGeometry*> skippedOf;
	std::vector<BodyCoatRecordCB> records;
	std::vector<const RE::NiSkinInstance*> recordSkin;
	bool recordsDirty = true;
	uint32_t slotOwner[kBodyCoatSlots][2] = {};
	bool slotFull[kBodyCoatSlots] = {};
	bool picked = false;
	uint32_t pickFrame = 0;
	int view = 0;
	bool live = false;
	/** @brief What the slots hold: S1's checks for the slot pattern view, otherwise the forced coat's pattern (its amounts ride the records). */
	bool checks = false;
	int force = -1;
	uint32_t lookFlags = 0;
	float glint[4] = {};
	float look[4] = {};
	float amounts[4] = {};
	bool hairSpecks = false;

	winrt::com_ptr<ID3D11Texture3D> atlas;
	winrt::com_ptr<ID3D11ShaderResourceView> atlasSRV;
	winrt::com_ptr<ID3D11DeviceContext1> context1;
	winrt::com_ptr<ID3D11Buffer> recordCB[2];
	winrt::com_ptr<ID3D11Buffer> fallbackCB;
	uint32_t parity = 0;
	uint32_t atlasBoundFrame = UINT32_MAX;
	bool gpuFailed = false;
	/** @brief The atlas as R8G8B8A8 words, for the writer's resolve. */
	winrt::com_ptr<ID3D11UnorderedAccessView> atlasUAV;

	// S3's contact writer (SnowContactCapture.hlsl, COAT): who it updates this
	// frame, the touches in two scratch volumes the atlas's size, its shaders
	// and constants, and the readout.
	struct Due
	{
		uint32_t formID;
		float growth;
	};
	std::vector<Due> due;
	uint32_t roundRobin = 0;
	uint32_t touched = 0;
	uint32_t capLevel = 0;
	winrt::com_ptr<ID3D11Texture3D> contactScratch, washScratch;
	winrt::com_ptr<ID3D11UnorderedAccessView> contactUAV, washUAV;
	winrt::com_ptr<ID3D11Buffer> raisedBuffer, raisedStaging[3];
	winrt::com_ptr<ID3D11UnorderedAccessView> raisedUAV;
	bool raisedPending[3] = {};
	uint32_t raisedRing = 0;
	winrt::com_ptr<ID3D11VertexShader> writeVS;
	winrt::com_ptr<ID3D11GeometryShader> writeGS;
	winrt::com_ptr<ID3D11PixelShader> writePS;
	winrt::com_ptr<ID3D11ComputeShader> resolveCS;
	winrt::com_ptr<ID3D11Buffer> writeCB, resolveCB;
	std::unique_ptr<ConstantBuffer> shellCB;
	ID3D11ShaderResourceView* shellSRVs[SnowDeformation::kShellSurfaceSRVs] = {};
	ID3D11ShaderResourceView* waterSRV = nullptr;
	// What a coat pass changes on the VS and GS, put back after it: features
	// bind some of these once a frame (Grass Collision's constants at VS b5).
	winrt::com_ptr<ID3D11Buffer> savedCB[5];
	winrt::com_ptr<ID3D11ShaderResourceView> savedSRV[SnowDeformation::kShellSurfaceSRVs + 1];
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	bool writerTried = false, writerReady = false;
	uint32_t writtenActorsLast = 0, writtenTrianglesLast = 0, raisedLast = 0;

	uint32_t drawFrame = UINT32_MAX;
	uint32_t coatedBuilding = 0, coatedLast = 0, walksLast = 0, missesLast = 0;

	// The near tier, as the contact capture picks it: the player and the
	// nearest living actors whose bound lies inside the contact window, then
	// the nearest corpses, each list at the capture's own cap.
	static void PickNearTier(std::vector<Pick>& a_living, std::vector<Pick>& a_dead)
	{
		auto* player = globals::game::player;
		if (!player)
			return;
		const RE::NiPoint3 eye = Util::GetEyePosition();
		auto consider = [&](RE::NiPointer<RE::Actor> a_actor, RE::ActorHandle a_handle, float a_d2, bool a_player) {
			if (!a_actor || !a_actor->Is3DLoaded())
				return;
			auto* root = a_actor->Get3D(false);
			if (!root)
				return;
			const auto& b = root->worldBound;
			if (b.radius <= 0.0f || std::abs(b.center.x - eye.x) + b.radius >= kContactHalfExtent ||
				std::abs(b.center.y - eye.y) + b.radius >= kContactHalfExtent)
				return;
			(a_actor->IsDead() ? a_dead : a_living).push_back({ a_d2, a_actor, a_handle, a_player });
		};
		consider(RE::NiPointer<RE::Actor>(player), {}, -1.0f, true);
		if (const auto processLists = RE::ProcessLists::GetSingleton())
			for (auto& handle : processLists->highActorHandles)
				if (auto actor = handle.get(); actor)
					consider(actor, handle, eye.GetSquaredDistance(actor->GetPosition()), false);
		auto byDistance = [](const Pick& a_a, const Pick& a_b) { return a_a.d2 < a_b.d2; };
		std::sort(a_living.begin(), a_living.end(), byDistance);
		std::sort(a_dead.begin(), a_dead.end(), byDistance);
		if (a_living.size() > kContactMaxActors)
			a_living.resize(kContactMaxActors);
		if (a_dead.size() > kContactMaxCorpses)
			a_dead.resize(kContactMaxCorpses);
	}

	// One actor's lit geometry, mapped into its frames. a_body is kept from
	// walk to walk: only an empty one is seeded, by the biggest worn skin, so
	// the frame never moves with the outfit (BODY-SNOW-PLAN.md section 5.4).
	// A skin that shares no bone with it seeds, or joins, a component.
	static void Walk(RE::Actor* a_actor, Frame& a_body, std::vector<Frame>& a_parts, Walked& a_out, bool a_dump)
	{
		auto* player = globals::game::player;
		auto* root = a_actor->Get3D(false);
		if (!root)
			return;
		const bool firstPersonPlayer = a_actor == player && RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson();

		// What the actor has equipped, by biped slot: each item's attached 3D.
		std::vector<std::pair<const RE::NiAVObject*, uint32_t>> parts;
		auto addBiped = [&](const RE::BSTSmartPointer<RE::BipedAnim>& a_biped) {
			if (!a_biped)
				return;
			for (uint32_t i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i)
				if (const RE::NiAVObject* part = a_biped->objects[i].partClone.get())
					parts.emplace_back(part, i);
		};
		addBiped(a_actor->GetActorRuntimeData().biped);
		if (a_actor == player)
			addBiped(player->GetInfoRuntimeData().largeBiped);
		auto slotOf = [&](const RE::NiAVObject* a_object) -> int {
			for (const RE::NiAVObject* n = a_object; n; n = n->parent)
				for (const auto& [part, slot] : parts)
					if (part == n)
						return int(slot);
			return -1;
		};
		auto carried = [](int a_slot) { return a_slot >= int(RE::BIPED_OBJECTS::kEditorTotal) || a_slot == int(RE::BIPED_OBJECTS::kShield); };

		// Eyes by their head part (section 5.5): an eye mod's eyes wear the
		// environment-map material, not the eye one. A facegen head names each
		// part's geometry after its editor ID: the NPC's own eyes, or its race's.
		// Brows and scars sit on the bare face with them (section 5.10).
		std::vector<const char*> eyeParts;
		auto addEyes = [&](const RE::BGSHeadPart* a_part) {
			using Type = RE::BGSHeadPart::HeadPartType;
			if (a_part && (a_part->type == Type::kEyes || a_part->type == Type::kEyebrows || a_part->type == Type::kScar) && a_part->formEditorID.c_str())
				eyeParts.push_back(a_part->formEditorID.c_str());
		};
		if (auto* npc = a_actor->GetActorBase()) {
			addEyes(npc->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kEyes));
			addEyes(npc->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kEyebrows));
			addEyes(npc->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kScar));
			if (const auto* race = a_actor->GetRace())
				if (const auto* face = race->faceRelatedData[npc->GetSex() == RE::SEX::kFemale ? 1 : 0]; face && face->headParts)
					for (const auto* part : *face->headParts)
						addEyes(part);
		}
		auto isEye = [&](const RE::BSGeometry* a_geometry, const RE::BSShaderProperty* a_property) {
			if (FeatureOf(a_property) == RE::BSShaderMaterial::Feature::kEye || a_property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kEyeReflect))
				return true;
			const char* name = NameOf(a_geometry);
			return std::ranges::any_of(eyeParts, [&](const char* a_eye) { return _stricmp(name, a_eye) == 0; });
		};

		auto visit = [&](RE::BSGeometry* a_geometry, bool a_first) {
			auto& runtime = a_geometry->GetGeometryRuntimeData();
			auto* property = runtime.shaderProperty.get();
			auto* skin = runtime.skinInstance.get();
			auto skip = [&]() {
				if (skin)
					a_out.skipped.push_back(a_geometry);
			};
			if (!IsLit(property) || IsProxyOrOverlayGeometry(a_geometry, property)) {
				skip();
				return;
			}
			const bool owned = a_geometry->GetUserData() == a_actor;
			if (a_first)
				(owned ? a_out.firstOwned : a_out.firstUnowned)++;
			else
				(owned ? a_out.owned : a_out.unowned)++;
			if (IsEngineDecalName(NameOf(a_geometry))) {
				a_out.decals++;
				skip();
				return;
			}
			auto* data = skin ? skin->skinData.get() : nullptr;
			if (skin && (!data || !skin->bones)) {
				skip();
				return;
			}
			const uint32_t triangles = TrianglesOf(a_geometry);
			const int slot = a_first ? -1 : slotOf(a_geometry);
			// Heat haze and spell planes: refraction, never a coat (section 2.6).
			if (IsRefraction(property)) {
				a_out.fx++;
				if (a_dump)
					a_out.gearLines.push_back(std::format("  FX left out '{}' under '{}'{}: shader flags {:016X}", NameOf(a_geometry), NameOf(a_geometry->parent),
						skin ? ", skinned" : "", uint64_t(property->flags.underlying())));
				skip();
				return;
			}
			if (!skin) {
				// Rigid: carried gear (section 2.11). S1 maps none of it; the census
				// sorts it by where it hangs, for the gear stage.
				if (a_first)
					return;
				const char* kind = "attached, not equipped";
				if (carried(slot)) {
					a_out.gear++;
					kind = "gear";
				} else if (slot >= 0) {
					a_out.rigidWorn++;
					kind = "rigid worn";
				} else if (_stricmp(NameOf(a_geometry), "Scb") == 0) {
					a_out.sheaths++;
					kind = "sheath";
				} else {
					a_out.attached++;
				}
				a_out.gearTriangles += triangles;
				a_out.gearRadiusMax = std::max(a_out.gearRadiusMax, a_geometry->worldBound.radius);
				if (a_dump)
					a_out.gearLines.push_back(std::format("  {} '{}' under '{}', slot {}: radius {:.0f}, {} triangles, shader flags {:016X}, owner {}", kind, NameOf(a_geometry),
						NameOf(a_geometry->parent), slot, a_geometry->worldBound.radius, triangles, uint64_t(property->flags.underlying()), owned ? "this actor" : "NOT this actor"));
				return;
			}
			const auto feature = FeatureOf(property);
			if (isEye(a_geometry, property)) {
				a_out.eyes++;
				skip();
				return;
			}
			if (!a_first && carried(slot)) {
				a_out.gearSkinned++;
				a_out.gearTriangles += triangles;
			}
			if (!a_first) {
				auto* partition = skin->skinPartition.get();
				const RE::BSGraphics::TriShape* buffer = partition && partition->numPartitions > 0 && partition->partitions.data() ? partition->partitions[0].buffData : nullptr;
				if (buffer && !buffer->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
					a_out.noNormals++;
			}
			Skin s;
			s.geometry = a_geometry;
			s.skin = skin;
			s.bones = BonesOf(skin, data);
			s.rootParentToSkin = data->rootParentToSkin;
			s.triangles = triangles;
			s.feature = int(feature);
			s.slot = slot;
			s.dynamic = a_geometry->AsDynamicTriShape() != nullptr;
			s.hidden = !a_first && HiddenByGame(a_geometry, root, firstPersonPlayer);
			s.firstPerson = a_first;
			a_out.skins.push_back(std::move(s));
		};
		auto third = [&](RE::BSGeometry* a_geometry) {
			visit(a_geometry, false);
			return RE::BSVisit::BSVisitControl::kContinue;
		};
		RE::BSVisit::TraverseScenegraphGeometries(root, std::ref(third));
		if (a_actor == player)
			if (auto* first = player->Get3D(true); first && first != root) {
				auto firstVisit = [&](RE::BSGeometry* a_geometry) {
					visit(a_geometry, true);
					return RE::BSVisit::BSVisitControl::kContinue;
				};
				RE::BSVisit::TraverseScenegraphGeometries(first, std::ref(firstVisit));
			}

		// A frame starts as one skin's own space: its bones' bind poses are
		// their boneToSkin. The body's seed is the biggest worn skin, so a
		// displayed bow or a long tail never becomes the body.
		auto seed = [&](Frame& a_frame, bool a_preferWorn) {
			Skin* best = nullptr;
			auto rank = [&](const Skin& a_s) { return std::make_tuple(a_preferWorn && a_s.slot >= 0 && !carried(a_s.slot), a_s.triangles); };
			for (auto& s : a_out.skins)
				if (s.frame < 0 && !s.firstPerson && !s.bones.empty() &&
					(!best || rank(s) > rank(*best) || (rank(s) == rank(*best) && std::string_view(NameOf(s.geometry)) < NameOf(best->geometry))))
					best = &s;
			if (!best)
				return false;
			a_frame.seed = NameOf(best->geometry);
			uint32_t depth = UINT32_MAX;
			for (const auto& bone : best->bones) {
				a_frame.bind.emplace(bone.name, bone.boneToSkin);
				const uint32_t d = DepthBelow(bone.node, root);
				if (d < depth || (d == depth && bone.name < a_frame.anchor)) {
					depth = d;
					a_frame.anchor = bone.name;
				}
			}
			return true;
		};
		// Skins join a frame through the bones they share with it, the
		// biggest first; each adds the bones only it names.
		auto chain = [&](Frame& a_frame, int a_index) {
			uint32_t joined = 0;
			std::vector<Skin*> order;
			for (auto& s : a_out.skins)
				if (s.frame < 0 && !s.firstPerson)
					order.push_back(&s);
			std::sort(order.begin(), order.end(), [](const Skin* a_a, const Skin* a_b) { return a_a->bones.size() > a_b->bones.size(); });
			for (bool progress = true; progress;) {
				progress = false;
				for (Skin* s : order) {
					if (s->frame >= 0)
						continue;
					SkinMap map = MapSkin(s->bones, a_frame.bind, kBodyCoatVoxel);
					if (!map.chained)
						continue;
					for (const auto& bone : s->bones)
						if (!a_frame.bind.contains(bone.name))
							a_frame.bind.emplace(bone.name, map.toAnchor * bone.boneToSkin);
					s->frame = a_index;
					s->map = std::move(map);
					joined++;
					progress = true;
				}
			}
			return joined;
		};

		if (a_body.bind.empty())
			seed(a_body, true);
		chain(a_body, 0);
		for (size_t p = 0; p < a_parts.size(); ++p)
			a_parts[p].seen = chain(a_parts[p], int(p) + 1) > 0;
		for (;;) {
			Frame part;
			if (!seed(part, false))
				break;
			a_parts.push_back(std::move(part));
			a_parts.back().seen = chain(a_parts.back(), int(a_parts.size())) > 0;
		}
		// Components no skin joined this walk go, and their halves with them.
		std::vector<int> renumber(a_parts.size() + 1, -1);
		renumber[0] = 0;
		size_t kept = 0;
		for (size_t p = 0; p < a_parts.size(); ++p) {
			if (!a_parts[p].seen) {
				if (a_parts[p].slot >= 0)
					a_out.freed.emplace_back(a_parts[p].slot, a_parts[p].half);
				continue;
			}
			renumber[p + 1] = int(kept) + 1;
			if (kept != p)
				a_parts[kept] = std::move(a_parts[p]);
			kept++;
		}
		a_parts.resize(kept);
		for (auto& s : a_out.skins)
			if (s.frame > 0)
				s.frame = renumber[s.frame];

		// First person: the same bones on the first-person skeleton, mapped by
		// name through the third-person bind, which they never extend.
		for (auto& s : a_out.skins)
			if (s.firstPerson) {
				s.map = MapSkin(s.bones, a_body.bind, kBodyCoatVoxel);
				s.frame = s.map.chained ? 0 : -1;
			}
	}

	// A frame's skins' bone spheres, in its space.
	static void BoundsOf(const Walked& a_walked, int a_frame, float a_min[3], float a_max[3])
	{
		for (int a = 0; a < 3; ++a) {
			a_min[a] = 1e30f;
			a_max[a] = -1e30f;
		}
		for (const auto& s : a_walked.skins) {
			if (s.frame != a_frame || s.firstPerson)
				continue;
			for (const auto& bone : s.bones) {
				const RE::NiPoint3 centre = s.map.toAnchor * (bone.boneToSkin * bone.bound.center);
				const float radius = bone.bound.radius * bone.boneToSkin.scale * s.map.toAnchor.scale;
				for (int a = 0; a < 3; ++a) {
					a_min[a] = std::min(a_min[a], centre[a] - radius);
					a_max[a] = std::max(a_max[a], centre[a] + radius);
				}
			}
		}
	}

	static void MarkOutside(Walked& a_walked, const Frame& a_frame, int a_index)
	{
		if (!a_frame.bounded)
			return;
		for (auto& s : a_walked.skins) {
			if (s.frame != a_index || s.firstPerson)
				continue;
			for (const auto& bone : s.bones) {
				const RE::NiPoint3 centre = s.map.toAnchor * (bone.boneToSkin * bone.bound.center);
				const float radius = bone.bound.radius * bone.boneToSkin.scale * s.map.toAnchor.scale;
				for (int a = 0; a < 3; ++a)
					if (centre[a] - radius < a_frame.boundsMin[a] - kBodyCoatVoxel || centre[a] + radius > a_frame.boundsMax[a] + kBodyCoatVoxel)
						s.outside = true;
			}
		}
	}

	int AllocFull(uint32_t a_formID)
	{
		for (int k = 0; k < int(kBodyCoatSlots); ++k)
			if (!slotOwner[k][0] && !slotOwner[k][1]) {
				slotOwner[k][0] = slotOwner[k][1] = a_formID;
				slotFull[k] = true;
				return k;
			}
		return -1;
	}

	std::pair<int, int> AllocHalf(uint32_t a_formID)
	{
		for (int pass = 0; pass < 2; ++pass)
			for (int k = 0; k < int(kBodyCoatSlots); ++k) {
				if (slotFull[k])
					continue;
				const bool split = slotOwner[k][0] || slotOwner[k][1];
				if (pass == 0 && !split)
					continue;
				for (int h = 0; h < 2; ++h)
					if (!slotOwner[k][h]) {
						slotOwner[k][h] = a_formID;
						return { k, h };
					}
			}
		return { -1, -1 };
	}

	void Free(int a_slot, int a_half)
	{
		if (a_slot < 0 || a_slot >= int(kBodyCoatSlots))
			return;
		if (a_half < 0) {
			slotOwner[a_slot][0] = slotOwner[a_slot][1] = 0;
			slotFull[a_slot] = false;
		} else {
			slotOwner[a_slot][a_half] = 0;
		}
	}

	void FreeActor(Actor& a_actor)
	{
		Free(a_actor.body.slot, -1);
		a_actor.body.slot = -1;
		for (auto& part : a_actor.parts) {
			Free(part.slot, part.half);
			part.slot = -1;
		}
	}

	// The frame's bounds over its slot's texels: its longest axis along the
	// slot's 48, the next along its 32 (16 for a half), the shortest along 16.
	// A voxel of margin each side, so nothing at the surface clamps.
	static void Place(Frame& a_frame, int a_slot, int a_half)
	{
		a_frame.slot = a_slot;
		a_frame.half = a_half;
		const float dims[3] = { float(a_half < 0 ? kBodyCoatSlotDim[0] : kBodyCoatSlotDim[0] / 2), float(kBodyCoatSlotDim[1]), float(kBodyCoatSlotDim[2]) };
		const float lo[3] = { a_half < 0 ? 0.0f : dims[0] * float(a_half), 0.0f, float(kBodyCoatSlotDim[2]) * float(a_slot) };
		float extent[3];
		int order[3] = { 0, 1, 2 };
		for (int c = 0; c < 3; ++c)
			extent[c] = std::max(a_frame.boundsMax[c] - a_frame.boundsMin[c], 1.0f);
		std::sort(order, order + 3, [&](int a_a, int a_b) { return extent[a_a] > extent[a_b] || (extent[a_a] == extent[a_b] && a_a < a_b); });
		a_frame.axisOf[2] = order[0];
		a_frame.axisOf[0] = order[1];
		a_frame.axisOf[1] = order[2];
		for (int a = 0; a < 3; ++a) {
			const int c = a_frame.axisOf[a];
			const float margin = extent[c] / (dims[a] - 2.0f);
			const float min = a_frame.boundsMin[c] - margin;
			a_frame.scale[a] = dims[a] / (extent[c] + 2.0f * margin);
			a_frame.offset[a] = lo[a] - min * a_frame.scale[a];
			a_frame.lo[a] = lo[a];
			a_frame.hi[a] = lo[a] + dims[a];
		}
	}

	bool EnsureGPU()
	{
		if (atlasSRV)
			return true;
		if (gpuFailed)
			return false;
		auto* device = globals::d3d::device;
		auto* context = globals::d3d::context;
		gpuFailed = true;
		if (!device || !context)
			return false;
		// Typeless: Lighting reads it as UNORM, the writer's resolve as R32 words.
		D3D11_TEXTURE3D_DESC desc{ kBodyCoatSlotDim[0], kBodyCoatSlotDim[1], kBodyCoatSlotDim[2] * kBodyCoatSlots, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
		std::vector<uint32_t> zero(size_t(desc.Width) * desc.Height * desc.Depth, 0u);
		D3D11_SUBRESOURCE_DATA init{ zero.data(), desc.Width * 4, desc.Width * desc.Height * 4 };
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MipLevels = 1;
		if (FAILED(device->CreateTexture3D(&desc, &init, atlas.put())) || FAILED(device->CreateShaderResourceView(atlas.get(), &srvDesc, atlasSRV.put()))) {
			logger::warn("[SNOW DEFORMATION] body coat: could not create the atlas");
			atlas = nullptr;
			atlasSRV = nullptr;
			return false;
		}
		Util::SetResourceName(atlas.get(), "SnowDeformation::BodyCoatAtlas");
		Util::SetResourceName(atlasSRV.get(), "SnowDeformation::BodyCoatAtlas SRV");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_UINT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.WSize = desc.Depth;
		if (SUCCEEDED(device->CreateUnorderedAccessView(atlas.get(), &uavDesc, atlasUAV.put())))
			Util::SetResourceName(atlasUAV.get(), "SnowDeformation::BodyCoatAtlas UAV");
		else
			logger::warn("[SNOW DEFORMATION] body coat: no R32 view of the atlas; the contact writer stays off");
		// Records by 11.1 offset where the runtime has them, two copies
		// alternating per draw (the redundancy filter ignores offsets on a
		// rebound buffer); one block written per draw otherwise.
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) && options.ConstantBufferOffsetting &&
			SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(context1.put())))) {
			for (int i = 0; i < 2; ++i) {
				D3D11_BUFFER_DESC cb{ kBodyCoatRecordsMax * kBodyCoatRecordStride, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
				if (FAILED(device->CreateBuffer(&cb, nullptr, recordCB[i].put()))) {
					context1 = nullptr;
					break;
				}
				Util::SetResourceName(recordCB[i].get(), i ? "SnowDeformation::BodyCoatRecords1" : "SnowDeformation::BodyCoatRecords0");
			}
		}
		D3D11_BUFFER_DESC one{ kBodyCoatRecordStride, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		if (FAILED(device->CreateBuffer(&one, nullptr, fallbackCB.put()))) {
			logger::warn("[SNOW DEFORMATION] body coat: could not create the record buffer");
			return false;
		}
		Util::SetResourceName(fallbackCB.get(), "SnowDeformation::BodyCoatRecord");
		gpuFailed = false;
		logger::info("[SNOW DEFORMATION] body coat: atlas {}x{}x{} RGBA8, {} slots; records {}", desc.Width, desc.Height, desc.Depth, kBodyCoatSlots,
			context1 ? "by D3D11.1 offset" : "written per draw");
		return true;
	}

	// The contact writer's shaders and buffers, made the first time an actor is
	// due. A failure is logged once and leaves the writer off.
	bool EnsureWriter()
	{
		if (writerTried)
			return writerReady;
		writerTried = true;
		auto* device = globals::d3d::device;
		if (!atlasUAV || !device)
			return false;
		auto blob = [](const char* a_target, const char* a_stage, const char* a_extra, const char* a_extra2 = nullptr) {
			winrt::com_ptr<ID3DBlob> b;
			b.attach(SD_CompileShaderBlob(kWritePath, a_target, a_stage, a_extra, a_extra2));
			return b;
		};
		auto vs = blob("vs_5_0", "VSHADER", "SKINNED", "COAT");
		auto gs = blob("gs_5_0", "GSHADER", "COAT");
		auto ps = blob("ps_5_0", "PSHADER", "COAT");
		auto cs = blob("cs_5_0", "CSHADER", "COAT");
		if (!vs || !gs || !ps || !cs || FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, writeVS.put())) ||
			FAILED(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, writeGS.put())) ||
			FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, writePS.put())) ||
			FAILED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, resolveCS.put()))) {
			logger::warn("[SNOW DEFORMATION] body coat: the contact writer's shaders did not compile; it stays off");
			return false;
		}
		Util::SetResourceName(writeVS.get(), "SnowDeformation::BodyCoatWriteVS");
		Util::SetResourceName(writeGS.get(), "SnowDeformation::BodyCoatWriteGS");
		Util::SetResourceName(writePS.get(), "SnowDeformation::BodyCoatWritePS");
		Util::SetResourceName(resolveCS.get(), "SnowDeformation::BodyCoatResolveCS");
		D3D11_TEXTURE3D_DESC desc{ kBodyCoatSlotDim[0], kBodyCoatSlotDim[1], kBodyCoatSlotDim[2] * kBodyCoatSlots, 1, DXGI_FORMAT_R32_UINT, D3D11_USAGE_DEFAULT,
			D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
		std::vector<uint32_t> zero(size_t(desc.Width) * desc.Height * desc.Depth, 0u);
		D3D11_SUBRESOURCE_DATA init{ zero.data(), desc.Width * 4, desc.Width * desc.Height * 4 };
		auto scratch = [&](winrt::com_ptr<ID3D11Texture3D>& a_texture, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav, const char* a_name) {
			if (FAILED(device->CreateTexture3D(&desc, &init, a_texture.put())) || FAILED(device->CreateUnorderedAccessView(a_texture.get(), nullptr, a_uav.put())))
				return false;
			Util::SetResourceName(a_texture.get(), "SnowDeformation::%s", a_name);
			return true;
		};
		D3D11_BUFFER_DESC raw{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
		D3D11_UNORDERED_ACCESS_VIEW_DESC rawView{};
		rawView.Format = DXGI_FORMAT_R32_TYPELESS;
		rawView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		rawView.Buffer.NumElements = 4;
		rawView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		D3D11_BUFFER_DESC staging{ 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
		D3D11_BUFFER_DESC writeDesc{ sizeof(BodyCoatWriteCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_BUFFER_DESC resolveDesc{ sizeof(BodyCoatResolveCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		bool ok = scratch(contactScratch, contactUAV, "BodyCoatContactScratch") && scratch(washScratch, washUAV, "BodyCoatWashScratch") &&
		          SUCCEEDED(device->CreateBuffer(&raw, nullptr, raisedBuffer.put())) && SUCCEEDED(device->CreateUnorderedAccessView(raisedBuffer.get(), &rawView, raisedUAV.put())) &&
		          SUCCEEDED(device->CreateBuffer(&writeDesc, nullptr, writeCB.put())) && SUCCEEDED(device->CreateBuffer(&resolveDesc, nullptr, resolveCB.put()));
		for (auto& s : raisedStaging)
			ok = ok && SUCCEEDED(device->CreateBuffer(&staging, nullptr, s.put()));
		if (!ok) {
			logger::warn("[SNOW DEFORMATION] body coat: could not create the contact writer's buffers; it stays off");
			return false;
		}
		Util::SetResourceName(raisedBuffer.get(), "SnowDeformation::BodyCoatRaised");
		Util::SetResourceName(writeCB.get(), "SnowDeformation::BodyCoatWriteCB");
		Util::SetResourceName(resolveCB.get(), "SnowDeformation::BodyCoatResolveCB");
		shellCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ShellCB>(), "SnowDeformation::BodyCoatShellCB");
		writerReady = true;
		logger::info("[SNOW DEFORMATION] body coat: contact writer ready");
		return true;
	}

	// The knee joint's height in a frame: its calf bones' bind origin. A
	// skeleton without them (a creature, a bow) takes its bound's lower 30
	// percent, an estimate.
	static float KneeHeight(const Frame& a_frame)
	{
		float sum = 0.0f;
		int count = 0;
		for (const char* name : { "NPC L Calf [LClf]", "NPC R Calf [RClf]" })
			if (auto it = a_frame.bind.find(name); it != a_frame.bind.end()) {
				sum += it->second.translate.z;
				count++;
			}
		return count ? sum / float(count) : a_frame.boundsMin[2] + 0.3f * (a_frame.boundsMax[2] - a_frame.boundsMin[2]);
	}

	// No writer before S3: a slot holds a pattern. The slot pattern view gets
	// S1's checks (two voxels, the slot's colour, a white border); otherwise the
	// forced coat in the atlas's own channels: R contact, G snowfall, B/A the
	// deposit direction as GBuffer::EncodeNormal writes it (it encodes -n, so
	// straight up is 1, 1). Each voxel's pattern is taken at its canonical
	// centre. Wading contact is whole to the ankle and thins up the shin, to
	// about a sixth at the knee and none at 1.3 times its height (section 5.11);
	// the left side ramps over four voxels.
	void FillSlot(const Frame& a_frame)
	{
		const int slot = a_frame.slot, half = a_frame.half;
		const uint32_t w = half < 0 ? kBodyCoatSlotDim[0] : kBodyCoatSlotDim[0] / 2;
		const uint32_t h = kBodyCoatSlotDim[1];
		const uint32_t d = kBodyCoatSlotDim[2];
		const uint32_t x0 = half < 0 ? 0u : w * uint32_t(half);
		const uint32_t z0 = d * uint32_t(slot);
		auto byte = [](float a_v) { return uint32_t(std::clamp(a_v, 0.0f, 1.0f) * 255.0f + 0.5f); };
		auto pack = [&](float a_r, float a_g, float a_b, float a_a) { return byte(a_r) | (byte(a_g) << 8) | (byte(a_b) << 16) | (byte(a_a) << 24); };
		auto ramp = [](float a_inside) { return std::clamp(0.5f + a_inside / (4.0f * kBodyCoatVoxel), 0.0f, 1.0f); };
		const RE::NiPoint3 colour = SlotColour(slot, half);
		const float knee = KneeHeight(a_frame);
		const float sole = a_frame.boundsMin[2];
		auto wade = [&](float a_z) {
			const float t = std::clamp(((a_z - sole) / std::max(knee - sole, 1.0f) - 0.15f) / 1.15f, 0.0f, 1.0f);
			return 1.0f - t * t * (3.0f - 2.0f * t);
		};
		const float midX = 0.5f * (a_frame.boundsMin[0] + a_frame.boundsMax[0]);
		std::vector<uint32_t> texels(size_t(w) * h * d);
		for (uint32_t z = 0; z < d; ++z)
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x) {
					uint32_t& texel = texels[(size_t(z) * h + y) * w + x];
					if (checks) {
						const bool edge = x == 0 || y == 0 || z == 0 || x == w - 1 || y == h - 1 || z == d - 1;
						const float k = (((x >> 1) + (y >> 1) + (z >> 1)) & 1) ? 1.0f : 0.35f;
						texel = edge ? pack(1.0f, 1.0f, 1.0f, 1.0f) : pack(colour.x * k, colour.y * k, colour.z * k, 1.0f);
						continue;
					}
					const float at[3] = { float(x0 + x), float(y), float(z0 + z) };
					float canonical[3];
					for (int a = 0; a < 3; ++a)
						canonical[a_frame.axisOf[a]] = (at[a] + 0.5f - a_frame.offset[a]) / a_frame.scale[a];
					float contact = 0.0f, fall = 0.0f;
					switch (force) {
					case 1:
						contact = wade(canonical[2]);
						break;
					case 2:
						fall = 1.0f;
						break;
					case 3:
						contact = ramp(midX - canonical[0]);
						break;
					case 4:
						contact = 1.0f;
						break;
					default:
						break;
					}
					texel = pack(contact, fall, 1.0f, 1.0f);
				}
		const D3D11_BOX box{ x0, 0, z0, x0 + w, h, z0 + d };
		globals::d3d::context->UpdateSubresource(atlas.get(), 0, &box, texels.data(), w * 4, w * h * 4);
	}

	void RefillAll()
	{
		for (auto& entry : actors) {
			if (entry.second.body.slot >= 0)
				FillSlot(entry.second.body);
			for (const auto& part : entry.second.parts)
				if (part.slot >= 0)
					FillSlot(part);
		}
	}

	// Bounds fixed at the slot's birth (section 2.1), then the slot and its pattern.
	void Slot(Frame& a_frame, const Walked& a_walked, int a_index, uint32_t a_formID)
	{
		if (a_frame.slot >= 0)
			return;
		int slot = -1, half = -1;
		if (a_index == 0)
			slot = AllocFull(a_formID);
		else
			std::tie(slot, half) = AllocHalf(a_formID);
		if (slot < 0)
			return;
		if (!a_frame.bounded) {
			BoundsOf(a_walked, a_index, a_frame.boundsMin, a_frame.boundsMax);
			a_frame.bounded = a_frame.boundsMax[0] > a_frame.boundsMin[0];
		}
		if (!a_frame.bounded) {
			Free(slot, half);
			return;
		}
		Place(a_frame, slot, half);
		FillSlot(a_frame);
	}

	void BuildRecords()
	{
		records.clear();
		recordSkin.clear();
		recordOf.clear();
		skippedOf.clear();
		for (const auto& entry : actors)
			skippedOf.insert(entry.second.skipped.begin(), entry.second.skipped.end());
		for (const auto& entry : actors)
			for (const auto& shape : entry.second.shapes) {
				const Actor& actor = entry.second;
				const Frame& f = shape.frame == 0 ? actor.body : actor.parts[shape.frame - 1];
				if (f.slot < 0 || records.size() >= kBodyCoatRecordsMax)
					continue;
				BodyCoatRecordCB r{};
				const RE::NiTransform& t = shape.toCanonical;
				for (int i = 0; i < 3; ++i)
					r.ModelToCanonical[i] = float4(t.rotate.entry[i][0] * t.scale, t.rotate.entry[i][1] * t.scale, t.rotate.entry[i][2] * t.scale, t.translate[i]);
				for (int a = 0; a < 3; ++a) {
					const float4& c = r.ModelToCanonical[f.axisOf[a]];
					r.ModelToAtlas[a] = float4(c.x * f.scale[a], c.y * f.scale[a], c.z * f.scale[a], c.w * f.scale[a] + f.offset[a]);
				}
				r.AtlasLo = float4(f.lo[0], f.lo[1], f.lo[2], 0.0f);
				r.AtlasHi = float4(f.hi[0], f.hi[1], f.hi[2], 0.0f);
				r.View = uint(view);
				r.Slot = uint(f.slot * 2 + std::max(f.half, 0));
				r.Component = shape.frame;
				r.Flags = lookFlags | (actor.dead ? 0u : kBodyCoatFlagLiving) | (shape.hair && hairSpecks ? kBodyCoatFlagHairSpecks : 0u);
				r.Glint = float4(glint[0], glint[1], glint[2], glint[3]);
				r.Look = float4(look[0], look[1], look[2], look[3]);
				r.Amounts = float4(amounts[0], amounts[1], amounts[2], amounts[3]);
				recordOf[shape.geometry] = uint32_t(records.size());
				records.push_back(r);
				recordSkin.push_back(shape.skin);
			}
		if (context1 && !records.empty())
			for (int i = 0; i < 2; ++i) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(globals::d3d::context->Map(recordCB[i].get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					records.clear();
					recordOf.clear();
					return;
				}
				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (size_t r = 0; r < records.size(); ++r)
					memcpy(dst + r * kBodyCoatRecordStride, &records[r], sizeof(BodyCoatRecordCB));
				globals::d3d::context->Unmap(recordCB[i].get(), 0);
			}
	}
};

void SnowDeformation::NoteBodyCoatShaders(uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
{
	const std::lock_guard lock(coatKeyLock);
	coatKeysDirty |= coatKeys.insert((uint64_t(a_vertexDescriptor) << 32) | a_pixelDescriptor).second;
}

void SnowDeformation::EnsureBodyCoatLumps()
{
	if (std::exchange(bodyCoatLumpAttempted, true))
		return;
	if (!LoadGameDDS("Shaders\\SnowDeformation\\BodyCoatLumps.dds", bodyCoatLumpSRV)) {
		bodyCoatLumpSRV = nullptr;
		logger::info("[SNOW DEFORMATION] body coat lump field not found; the lumps stay flat");
		return;
	}
	logger::info("[SNOW DEFORMATION] body coat lump field loaded");
}

void SnowDeformation::GenerateShaderPermutations(RE::BSShader* a_shader)
{
	if (a_shader->shaderType.get() != RE::BSShader::Type::Lighting)
		return;
	std::vector<uint64_t> keys;
	{
		const std::lock_guard lock(coatKeyLock);
		std::ifstream file(CoatKeyPath());
		uint32_t vertex = 0;
		uint32_t pixel = 0;
		while (file >> std::hex >> vertex >> pixel)
			coatKeys.insert((uint64_t(vertex) << 32) | pixel);
		keys.assign(coatKeys.begin(), coatKeys.end());
	}
	// As LoadShaders queues the game's own: the vertex shader, and the pixel
	// shader forward and deferred.
	auto state = globals::state;
	auto shaderCache = globals::shaderCache;
	for (const uint64_t key : keys) {
		for (const bool deferred : { false, true }) {
			auto vertex = uint32_t(key >> 32);
			auto pixel = uint32_t(key);
			state->ModifyShaderLookup(*a_shader, vertex, pixel, deferred);
			if (!deferred)
				std::ignore = shaderCache->GetVertexShader(*a_shader, vertex);
			std::ignore = shaderCache->GetPixelShader(*a_shader, pixel);
		}
	}
	logger::info("Snow on bodies: queued {} coat shader pairs drawn in earlier sessions", keys.size());
}

void SnowDeformation::TickBodyCoat()
{
	{
		const std::lock_guard lock(coatKeyLock);
		if (coatKeysDirty) {
			coatKeysDirty = false;
			std::ofstream file(CoatKeyPath(), std::ios::trunc);
			for (const uint64_t key : coatKeys)
				file << std::format("{:08X} {:08X}\n", uint32_t(key >> 32), uint32_t(key));
		}
	}
	if (!debugBodyCoatDrawProbe && bodyCoatProbeBound) {
		ID3D11ShaderResourceView* none = nullptr;
		globals::d3d::context->PSSetShaderResources(kBodyCoatAtlasSlot, 1, &none);
		bodyCoatProbeBound = false;
		bodyCoatDrawFrame = UINT32_MAX;
		if (bodyCoatStore)
			bodyCoatStore->atlasBoundFrame = UINT32_MAX;
	}
	TickBodyCoatStore();
	const bool dump = debugBodyCoatCensusDump;
	if (!debugBodyCoatCensus && !dump)
		return;
	const uint32_t frame = globals::state->frameCount;
	if (!dump && bodyCoatCensusRan && frame - bodyCoatCensusFrame < kBodyCoatCensusFrames)
		return;
	debugBodyCoatCensusDump = false;
	bodyCoatCensusFrame = frame;
	bodyCoatCensusRan = true;
	const auto start = std::chrono::steady_clock::now();
	RunBodyCoatCensus(dump);
	bodyCoatCensusMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void SnowDeformation::TickBodyCoatStore()
{
	using Store = BodyCoatStore;
	auto* context = globals::d3d::context;
	bodyCoatLive = false;
	const int view = std::clamp(debugBodyCoatView, 0, 3);
	const int force = std::clamp(debugBodyCoatForce, 0, 4);
	// Testers' packages leave the writer off until body snow ships.
	const bool contact = debugBodyCoatContact && !SNOW_ALPHA_BUILD;
	if ((view == 0 && force == 0 && !contact) || !settings.EnableSnowDeformation) {
		if (bodyCoatStore) {
			if (bodyCoatStore->live && !bodyCoatProbeBound) {
				ID3D11ShaderResourceView* none = nullptr;
				context->PSSetShaderResources(kBodyCoatAtlasSlot, 1, &none);
			}
			bodyCoatStore.reset();
		}
		return;
	}
	if (!bodyCoatStore)
		bodyCoatStore = std::make_shared<Store>();
	auto& store = *bodyCoatStore;
	if (!store.EnsureGPU()) {
		store.live = false;
		return;
	}
	const uint32_t frame = globals::state->frameCount;
	store.coatedLast = std::exchange(store.coatedBuilding, 0u);
	if (store.view != view) {
		store.view = view;
		store.recordsDirty = true;
	}
	const bool checks = view == 2;
	if (store.checks != checks || store.force != force) {
		store.checks = checks;
		store.force = force;
		store.RefillAll();
	}
	// The record's look: the A/B, and the shell's sparkle as Lighting's
	// glintParameters (40 - log density, as LandscapeSeamsFix writes them).
	if (debugBodyCoatLumpLook != 0)
		EnsureBodyCoatLumps();
	const uint32_t lumpFlags = debugBodyCoatLumpLook != 0 && bodyCoatLumpSRV ? kBodyCoatFlagLumpDepth : 0u;
	const uint32_t lookFlags = (debugBodyCoatLook == 1 ? kBodyCoatFlagFlatPaint : 0u) | (debugBodyCoatRelief ? kBodyCoatFlagRelief : 0u) |
	                           (debugBodyCoatReliefView ? kBodyCoatFlagReliefView : 0u) | (debugBodyCoatHairShape == 1 ? kBodyCoatFlagHairBeads : 0u) | lumpFlags;
	const bool sparkle = settings.SnowGlints && snowGlintLogDensity > 1.1f;
	const float glint[4] = { snowGlintScreenSpaceScale, 40.0f - (sparkle ? snowGlintLogDensity : 1.0f), snowGlintMicroRoughness, snowGlintDensityRandomization };
	// The cranks as the shader reads them: one over the angle of repose in
	// radians; the smallest and largest speck radius in units (the sizes are
	// diameters in mm, 14.2875 mm a unit); and how many more balls a share needs
	// as the sizes' mean volume falls below the largest's, sizes spread evenly
	// between: E[(r / largest)^3] = (1 + p)(1 + p^2) / 4 at p = smallest / largest.
	const float repose = std::clamp(debugBodyCoatRepose, 1.0f, 89.0f);
	const float sizeLo = std::clamp(std::min(debugBodyCoatSpeckMin, debugBodyCoatSpeckMax), 0.5f, 10.0f);
	const float sizeHi = std::clamp(std::max(debugBodyCoatSpeckMin, debugBodyCoatSpeckMax), 0.5f, 10.0f);
	const float sizeRatio = sizeLo / sizeHi;
	const float look[4] = { 1.0f / DirectX::XMConvertToRadians(repose), sizeLo * 0.5f / 14.2875f, sizeHi * 0.5f / 14.2875f,
		4.0f / ((1.0f + sizeRatio) * (1.0f + sizeRatio * sizeRatio)) };
	// Grain 1 is half a surface, or as many specks as the lattice holds when
	// that is less (sizes spread wider than 1.5 : 1): every ball on covers
	// 1 - e^-L at L (x + y L) = the sizes' mean volume.
	const float roomL = (kSpeckRank[0] - std::sqrt(kSpeckRank[0] * kSpeckRank[0] + 4.0f * kSpeckRank[1] / look[3])) / (-2.0f * kSpeckRank[1]);
	const float grainScale = std::min(1.0f, (1.0f - std::exp(-roomL)) / kSpeckMax);
	// The forced coat's lumps, specks and hair's specks, apart (S2's cranks).
	const float amounts[4] = { std::clamp(debugBodyCoatLumps, 0.0f, 1.0f), std::clamp(debugBodyCoatGrains, 0.0f, 1.0f) * grainScale,
		std::clamp(debugBodyCoatHairAmount, 0.0f, 1.0f), 0.0f };
	if (store.lookFlags != lookFlags || memcmp(store.glint, glint, sizeof(glint)) != 0 || memcmp(store.look, look, sizeof(look)) != 0 ||
		memcmp(store.amounts, amounts, sizeof(amounts)) != 0 || store.hairSpecks != debugBodyCoatHairSpecks) {
		store.lookFlags = lookFlags;
		memcpy(store.glint, glint, sizeof(glint));
		memcpy(store.look, look, sizeof(look));
		memcpy(store.amounts, amounts, sizeof(amounts));
		store.hairSpecks = debugBodyCoatHairSpecks;
		store.recordsDirty = true;
	}
	auto* player = globals::game::player;

	if (!store.picked || frame - store.pickFrame >= kPickFrames) {
		store.picked = true;
		store.pickFrame = frame;
		std::vector<Store::Pick> living, dead;
		Store::PickNearTier(living, dead);
		for (auto* list : { &living, &dead })
			for (const auto& pick : *list) {
				auto [it, inserted] = store.actors.try_emplace(pick.actor->formID);
				it->second.lastPicked = frame;
				it->second.player = pick.player;
				if (!pick.player)
					it->second.handle = pick.handle;
			}
	}
	for (auto it = store.actors.begin(); it != store.actors.end();) {
		if (!it->second.player && frame - it->second.lastPicked > kKeepFrames) {
			store.FreeActor(it->second);
			it = store.actors.erase(it);
			store.recordsDirty = true;
		} else {
			++it;
		}
	}

	// Walks: a new actor, a new 3D or race, a draw that found no record, and a
	// slow refresh for what changes silently; a few a frame.
	uint32_t walks = 0;
	for (auto& [formID, a] : store.actors) {
		RE::NiPointer<RE::Actor> actor = a.player ? RE::NiPointer<RE::Actor>(player) : a.handle.get();
		if (!actor || !actor->Is3DLoaded())
			continue;
		if (const bool dead = actor->IsDead(); dead != a.dead) {
			a.dead = dead;
			store.recordsDirty = true;
		}
		const RE::NiAVObject* root = actor->Get3D(false);
		const RE::NiAVObject* first = a.player ? actor->Get3D(true) : nullptr;
		const RE::TESRace* race = actor->GetRace();
		if (race != a.race) {
			// A new skeleton (werewolf, Vampire Lord, RaceMenu): the frames reset (section 2.8).
			store.FreeActor(a);
			a.body = {};
			a.parts.clear();
			a.shapes.clear();
			a.race = race;
			a.walk = true;
			store.recordsDirty = true;
		}
		if (root != a.root3D || first != a.first3D || (frame + formID) % kRewalkFrames == 0 ||
			(store.missed.contains(formID) && !a.refused && frame - a.lastWalked >= kMissFrames))
			a.walk = true;
		if (!a.walk || walks >= kWalksPerFrame || !root)
			continue;
		walks++;
		a.walk = false;
		a.lastWalked = frame;
		a.root3D = root;
		a.first3D = first;
		store.recordsDirty = true;
		a.refused = ActorIsIncorporeal(actor.get(), actor->Get3D(false), nullptr);
		if (a.refused) {
			store.FreeActor(a);
			a.shapes.clear();
			continue;
		}
		Store::Walked walked;
		Store::Walk(actor.get(), a.body, a.parts, walked, false);
		for (const auto& [slot, half] : walked.freed)
			store.Free(slot, half);
		store.Slot(a.body, walked, 0, formID);
		for (size_t p = 0; p < a.parts.size(); ++p)
			store.Slot(a.parts[p], walked, int(p) + 1, formID);
		a.shapes.clear();
		a.skipped = std::move(walked.skipped);
		for (const auto& s : walked.skins) {
			// Skin takes no coat until skin snow looks right (Josef, S2 round 2): facegen
			// heads and tinted body skin, beast races' fur included. It still seeds and
			// chains the frames, so gloves and boots map through the body's bones.
			const bool bareSkin = s.feature == int(RE::BSShaderMaterial::Feature::kFaceGen) || s.feature == int(RE::BSShaderMaterial::Feature::kFaceGenRGBTint);
			if (s.frame >= 0 && !bareSkin)
				a.shapes.push_back({ s.geometry, s.skin, uint32_t(s.frame), s.map.toAnchor, s.feature == int(RE::BSShaderMaterial::Feature::kHairTint) });
			else
				a.skipped.push_back(s.geometry);
		}
	}
	store.walksLast = walks;
	store.missesLast = uint32_t(store.missed.size());
	store.missed.clear();
	if (store.recordsDirty) {
		store.recordsDirty = false;
		store.BuildRecords();
	}
	store.live = !store.records.empty();
	bodyCoatLive = store.live;
	if (std::exchange(debugBodyCoatClear, false))
		store.RefillAll();

	// S3: who the contact writer updates this frame, each with how far its
	// touches grow the coat (section 5.31): adhesion (Snow Climate's stickiness,
	// raised by the largest jolt since its last update, section 2.3) over the
	// time since then, the cap reached in Build-up Time at full adhesion. Force
	// Coat owns the atlas while it is on.
	store.due.clear();
	if (contact && force == 0 && store.live && store.atlasUAV) {
		const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 0.0f) : 0.0f;
		const float stickiness = GetSnowCondition().stickiness;
		const float cap = std::clamp(debugBodyCoatContactCap, 0.0f, 1.0f) * kCoatContactFront * 255.0f;
		store.capLevel = uint32_t(cap + 0.5f);
		const float perSecond = cap / std::max(debugBodyCoatBuildUp, 0.1f);
		const auto make = [&](uint32_t a_formID, Store::Actor& a_actor) {
			store.due.push_back({ a_formID, std::min(1.0f, stickiness + a_actor.joltHeld) * perSecond * a_actor.sinceWrite });
			a_actor.sinceWrite = 0.0f;
			a_actor.joltHeld = 0.0f;
		};
		std::vector<uint32_t> others;
		for (auto& [formID, a] : store.actors) {
			if (a.refused || a.shapes.empty())
				continue;
			a.sinceWrite = std::min(a.sinceWrite + dt, kWriteGapMax);
			if (const auto track = impactTracks.find(formID); track != impactTracks.end())
				a.joltHeld = std::max(a.joltHeld, track->second.Jolt());
			if (!a.player)
				others.push_back(formID);
			else if (frame % kWritePlayerFrames == 0 && player && player->Is3DLoaded())
				make(formID, a);
		}
		std::sort(others.begin(), others.end());
		const uint32_t count = uint32_t(others.size());
		uint32_t taken = 0;
		for (uint32_t i = 0; i < count && store.due.size() < kWritesPerFrame; ++i) {
			const uint32_t formID = others[(store.roundRobin + i) % count];
			taken = i + 1;
			auto& a = store.actors[formID];
			if (auto actor = a.handle.get(); actor && actor->Is3DLoaded())
				make(formID, a);
		}
		store.roundRobin = count ? (store.roundRobin + taken) % count : 0;
	}
}

void SnowDeformation::PrimeBodyCoatWriter()
{
#if !SNOW_ALPHA_BUILD
	const char* variants[4][4] = { { "vs_5_0", "VSHADER", "SKINNED", "COAT" }, { "gs_5_0", "GSHADER", "COAT", nullptr }, { "ps_5_0", "PSHADER", "COAT", nullptr },
		{ "cs_5_0", "CSHADER", "COAT", nullptr } };
	for (const auto& v : variants)
		if (auto* blob = SD_CompileShaderBlob(kWritePath, v[0], v[1], v[2], v[3]))
			blob->Release();
#endif
}

bool SnowDeformation::BodyCoatWriteBegin()
{
	if (!bodyCoatStore || bodyCoatStore->due.empty() || !contactHeight || !contactSkinVS || !contactPS)
		return false;
	auto& store = *bodyCoatStore;
	if (!lastShellCBData || !shellTerrainTexture || !shellTerrainTexture->srv || !GetDeformationSRV() || !heightBottomFiltered ||
		!heightTopRaw[heightCurrent] || !heightSkinDepth || !ShellFootprintHasSnow() || !store.EnsureWriter())
		return false;
	// The snow as the shell draws it, placed for this frame (LandscapeSnowRefresh's recipe).
	auto& fb = globals::game::frameBufferCached;
	ShellCB cb = *lastShellCBData;
	cb.CameraPosAdjust = fb.GetCameraPosAdjust();
	cb.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();
	PlaceShellFields(cb);
	store.shellCB->Update(cb);
	LandscapeSurfaceSRVs(store.shellSRVs);
	store.waterSRV = waterFineTexture && waterFineTexture->srv ? waterFineTexture->srv.get() : nullptr;
	return true;
}

bool SnowDeformation::BodyCoatWriteDue(uint32_t a_formID, float& a_growth) const
{
	if (bodyCoatStore)
		for (const auto& d : bodyCoatStore->due)
			if (d.formID == a_formID) {
				a_growth = d.growth;
				return true;
			}
	return false;
}

bool SnowDeformation::BodyCoatWriteShape(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, float a_growth)
{
	auto& store = *bodyCoatStore;
	const auto it = store.recordOf.find(a_geometry);
	if (it == store.recordOf.end() || store.recordSkin[it->second] != a_geometry->GetGeometryRuntimeData().skinInstance.get())
		return false;
	const BodyCoatRecordCB& record = store.records[it->second];
	BodyCoatWriteCB cb{ { record.ModelToAtlas[0], record.ModelToAtlas[1], record.ModelToAtlas[2] }, record.AtlasLo, record.AtlasHi, a_growth, {} };
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(a_context->Map(store.writeCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	memcpy(mapped.pData, &cb, sizeof(cb));
	a_context->Unmap(store.writeCB.get(), 0);
	store.touched |= 1u << (record.Slot >> 1);
	return true;
}

void SnowDeformation::BodyCoatWriteBind(ID3D11DeviceContext* a_context, bool a_on)
{
	auto& store = *bodyCoatStore;
	// The capture's target stays bound: the coat's points leave it as it is.
	ID3D11RenderTargetView* rtv = contactHeight->rtv.get();
	if (a_on) {
		ID3D11Buffer* cbs[5] = {};
		a_context->VSGetConstantBuffers(0, 1, cbs);
		a_context->VSGetConstantBuffers(3, 4, cbs + 1);
		ID3D11ShaderResourceView* srvs[kShellSurfaceSRVs + 1] = {};
		a_context->VSGetShaderResources(0, kShellSurfaceSRVs, srvs);
		a_context->VSGetShaderResources(kWaterWindowSlot, 1, srvs + kShellSurfaceSRVs);
		for (uint32_t i = 0; i < 5; ++i)
			store.savedCB[i].attach(cbs[i]);
		for (uint32_t i = 0; i <= kShellSurfaceSRVs; ++i)
			store.savedSRV[i].attach(srvs[i]);
		store.savedGS = nullptr;
		a_context->GSGetShader(store.savedGS.put(), nullptr, nullptr);
		a_context->VSSetShader(store.writeVS.get(), nullptr, 0);
		a_context->GSSetShader(store.writeGS.get(), nullptr, 0);
		a_context->PSSetShader(store.writePS.get(), nullptr, 0);
		ID3D11Buffer* shell = store.shellCB->CB();
		a_context->VSSetConstantBuffers(0, 1, &shell);
		ID3D11Buffer* write = store.writeCB.get();
		a_context->VSSetConstantBuffers(3, 1, &write);
		auto* state = globals::state;
		ID3D11Buffer* shared[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
		a_context->VSSetConstantBuffers(4, 3, shared);
		a_context->VSSetShaderResources(0, kShellSurfaceSRVs, store.shellSRVs);
		a_context->VSSetShaderResources(kWaterWindowSlot, 1, &store.waterSRV);
		ID3D11UnorderedAccessView* uavs[2] = { store.contactUAV.get(), store.washUAV.get() };
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 2, uavs, nullptr);
	} else {
		ID3D11UnorderedAccessView* noUAV[2] = {};
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 2, noUAV, nullptr);
		a_context->GSSetShader(store.savedGS.get(), nullptr, 0);
		a_context->PSSetShader(contactPS, nullptr, 0);
		a_context->VSSetShader(contactSkinVS, nullptr, 0);
		ID3D11ShaderResourceView* srvs[kShellSurfaceSRVs + 1];
		for (uint32_t i = 0; i <= kShellSurfaceSRVs; ++i)
			srvs[i] = store.savedSRV[i].get();
		a_context->VSSetShaderResources(0, kShellSurfaceSRVs, srvs);
		a_context->VSSetShaderResources(kWaterWindowSlot, 1, srvs + kShellSurfaceSRVs);
		ID3D11Buffer* cbs[5];
		for (uint32_t i = 0; i < 5; ++i)
			cbs[i] = store.savedCB[i].get();
		a_context->VSSetConstantBuffers(0, 1, cbs);
		a_context->VSSetConstantBuffers(3, 4, cbs + 1);
		for (auto& cb : store.savedCB)
			cb = nullptr;
		for (auto& srv : store.savedSRV)
			srv = nullptr;
		store.savedGS = nullptr;
	}
}

void SnowDeformation::BodyCoatWriteResolve(ID3D11DeviceContext* a_context, uint32_t a_triangles, uint32_t a_actors)
{
	auto& store = *bodyCoatStore;
	store.writtenTrianglesLast = a_triangles;
	store.writtenActorsLast = a_actors;
	// The readout: the copy two resolves back, if the GPU has it.
	const uint32_t oldest = (store.raisedRing + 1) % 3;
	if (store.raisedPending[oldest]) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_context->Map(store.raisedStaging[oldest].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			store.raisedLast = *static_cast<const uint32_t*>(mapped.pData);
			a_context->Unmap(store.raisedStaging[oldest].get(), 0);
			store.raisedPending[oldest] = false;
		}
	}
	if (!store.touched)
		return;
	globals::profiler->BeginPass("SnowDeformation::Bodies - Coat Update");
	BodyCoatResolveCB cb{};
	for (uint32_t k = 0; k < kBodyCoatSlots; ++k)
		if (store.touched & (1u << k))
			cb.Slots[cb.Count++] = k;
	cb.Cap = store.capLevel;
	cb.Seed = globals::state->frameCount;
	store.touched = 0;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(a_context->Map(store.resolveCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		memcpy(mapped.pData, &cb, sizeof(cb));
		a_context->Unmap(store.resolveCB.get(), 0);
		const UINT zero[4] = {};
		a_context->ClearUnorderedAccessViewUint(store.raisedUAV.get(), zero);
		winrt::com_ptr<ID3D11ComputeShader> savedCS;
		a_context->CSGetShader(savedCS.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11Buffer> savedCB0;
		a_context->CSGetConstantBuffers(0, 1, savedCB0.put());
		a_context->CSSetShader(store.resolveCS.get(), nullptr, 0);
		ID3D11Buffer* resolve = store.resolveCB.get();
		a_context->CSSetConstantBuffers(0, 1, &resolve);
		ID3D11UnorderedAccessView* uavs[4] = { store.atlasUAV.get(), store.contactUAV.get(), store.washUAV.get(), store.raisedUAV.get() };
		a_context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		a_context->Dispatch(kBodyCoatSlotDim[0] / 8, kBodyCoatSlotDim[1] / 8, kBodyCoatSlotDim[2] / 4 * cb.Count);
		ID3D11UnorderedAccessView* noUAV[4] = {};
		a_context->CSSetUnorderedAccessViews(0, 4, noUAV, nullptr);
		a_context->CSSetShader(savedCS.get(), nullptr, 0);
		ID3D11Buffer* cb0 = savedCB0.get();
		a_context->CSSetConstantBuffers(0, 1, &cb0);
		store.raisedRing = (store.raisedRing + 1) % 3;
		a_context->CopyResource(store.raisedStaging[store.raisedRing].get(), store.raisedBuffer.get());
		store.raisedPending[store.raisedRing] = true;
		// The UAV unbound the atlas from Lighting's t41: the hook binds it again.
		store.atlasBoundFrame = UINT32_MAX;
	}
	globals::profiler->EndPass();
}

bool SnowDeformation::BodyCoatPixelKey(uint32_t a_rawTechnique) const
{
	using Flags = SIE::ShaderCache::LightingShaderFlags;
	using Technique = SIE::ShaderCache::LightingShaderTechniques;
	// Model-space-normal skins carry no bind frame for the coat to read (section 5.10): skin keeps its plain shader.
	return bodyCoatLive && (a_rawTechnique & uint32_t(Flags::Skinned)) != 0 && (a_rawTechnique & uint32_t(Flags::ModelSpaceNormals)) == 0 &&
	       static_cast<Technique>(0x3F & (a_rawTechnique >> 24)) != Technique::Eye;
}

void SnowDeformation::SetBodyCoatBit(RE::BSRenderPass* a_pass)
{
	auto& permutation = globals::state->permutationData;
	permutation.ExtraFeatureDescriptor &= ~uint32_t(State::ExtraFeatureDescriptors::SnowBodyCoat);
	BodyCoatStore* store = bodyCoatStore.get();
	if (!store || !store->live || !a_pass || !a_pass->geometry)
		return;
	// Reflections and the inventory's 3D take no coat (section 2.6).
	if ((permutation.ExtraShaderDescriptor & uint32_t(State::ExtraShaderDescriptors::IsReflections)) != 0 || permutation.RenderToUI)
		return;
	RE::BSGeometry* geometry = a_pass->geometry;
	const RE::NiSkinInstance* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
	if (!skin)
		return;
	const auto it = store->recordOf.find(geometry);
	if (it == store->recordOf.end() || store->recordSkin[it->second] != skin) {
		// A skin the last walk did not see (an equip change): walk its actor soon.
		if (it == store->recordOf.end() && store->skippedOf.contains(geometry))
			return;
		if (auto* owner = geometry->GetUserData(); owner && owner->formType == RE::FormType::ActorCharacter && store->actors.contains(owner->formID))
			store->missed.insert(owner->formID);
		return;
	}
	permutation.ExtraFeatureDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowBodyCoat);
	auto* context = globals::d3d::context;
	// Once a frame: R0 measured the t41 bind reaching every actor draw (BODY-SNOW-PLAN.md section 5.4).
	const uint32_t frame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
	if (store->atlasBoundFrame != frame) {
		ID3D11ShaderResourceView* srv = store->atlasSRV.get();
		context->PSSetShaderResources(kBodyCoatAtlasSlot, 1, &srv);
		store->atlasBoundFrame = frame;
	}
	// The look wears the shell's set, bound per draw as projected snow binds it
	// (a Prepass bind does not survive to the Lighting draws).
	if (store->view == 0) {
		ID3D11ShaderResourceView* albedoSRV = shellSnowDiffuseSRV.get();
		ID3D11ShaderResourceView* normalSRV = shellSnowNormalSRV.get();
		ID3D11ShaderResourceView* rmaosSRV = shellSnowRmaosSRV.get();
		context->PSSetShaderResources(kHorizonSnowAlbedoSlot, 1, &albedoSRV);
		context->PSSetShaderResources(kHorizonSnowNormalSlot, 1, &normalSRV);
		context->PSSetShaderResources(kHorizonSnowRmaosSlot, 1, &rmaosSRV);
		if (store->lookFlags & kBodyCoatFlagLumpDepth) {
			ID3D11ShaderResourceView* lumpSRV = bodyCoatLumpSRV.get();
			context->PSSetShaderResources(kBodyCoatLumpSlot, 1, &lumpSRV);
		}
	}
	if (store->context1) {
		ID3D11Buffer* buffer = store->recordCB[store->parity & 1].get();
		store->parity++;
		const UINT firstConstant = it->second * (kBodyCoatRecordStride / 16);
		const UINT constants = kBodyCoatRecordStride / 16;
		store->context1->PSSetConstantBuffers1(kBodyCoatRecordSlot, 1, &buffer, &firstConstant, &constants);
	} else {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(store->fallbackCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, &store->records[it->second], sizeof(BodyCoatRecordCB));
			context->Unmap(store->fallbackCB.get(), 0);
		}
		ID3D11Buffer* buffer = store->fallbackCB.get();
		context->PSSetConstantBuffers(kBodyCoatRecordSlot, 1, &buffer);
	}
	store->coatedBuilding++;
}

void SnowDeformation::RunBodyCoatCensus(bool a_dump)
{
	using Store = BodyCoatStore;
	bodyCoatCensus.clear();
	std::vector<Store::Pick> living, dead;
	Store::PickNearTier(living, dead);

	uint32_t dumpLines = 0;
	auto dumpLine = [&](const std::string& a_line) {
		if (a_dump && dumpLines++ < kDumpLinesMax)
			logger::info("[SNOW DEFORMATION] body coat census: {}", a_line);
	};

	auto censusOf = [&](RE::Actor* a_actor, float a_d2, bool a_corpse) {
		BodyCoatActorCensus c;
		const char* display = a_actor->GetDisplayFullName();
		c.name = display ? display : "";
		c.formID = a_actor->formID;
		c.distance = a_d2 > 0.0f ? std::sqrt(a_d2) : 0.0f;
		c.corpse = a_corpse;
		if (auto* state = a_actor->AsActorState())
			c.weaponDrawn = state->IsWeaponDrawn();

		// The store's frames where it holds the actor (what the coat draws
		// with), copied so the census changes nothing; fresh ones otherwise.
		Store::Frame body;
		std::vector<Store::Frame> parts;
		const Store::Actor* held = nullptr;
		if (bodyCoatStore)
			if (auto it = bodyCoatStore->actors.find(a_actor->formID); it != bodyCoatStore->actors.end()) {
				held = &it->second;
				body = held->body;
				parts = held->parts;
			}
		c.sticky = !body.bind.empty();
		Store::Walked walked;
		Store::Walk(a_actor, body, parts, walked, a_dump);
		if (body.bounded) {
			Store::MarkOutside(walked, body, 0);
		} else {
			float lo[3], hi[3];
			Store::BoundsOf(walked, 0, lo, hi);
			std::copy(lo, lo + 3, body.boundsMin);
			std::copy(hi, hi + 3, body.boundsMax);
		}
		for (size_t p = 0; p < parts.size(); ++p)
			Store::MarkOutside(walked, parts[p], int(p) + 1);

		c.seed = body.seed;
		c.anchor = body.anchor;
		c.bindBones = uint32_t(body.bind.size());
		c.components = uint32_t(parts.size());
		c.boundsMin = { body.boundsMin[0], body.boundsMin[1], body.boundsMin[2] };
		c.boundsMax = { body.boundsMax[0], body.boundsMax[1], body.boundsMax[2] };
		c.decals = walked.decals;
		c.fx = walked.fx;
		c.eyeShapes = walked.eyes;
		c.noNormals = walked.noNormals;
		c.gear = walked.gear;
		c.gearSkinned = walked.gearSkinned;
		c.rigidWorn = walked.rigidWorn;
		c.sheaths = walked.sheaths;
		c.attached = walked.attached;
		c.gearTriangles = walked.gearTriangles;
		c.gearRadiusMax = walked.gearRadiusMax;
		c.owned = walked.owned;
		c.unowned = walked.unowned;
		c.firstOwned = walked.firstOwned;
		c.firstUnowned = walked.firstUnowned;
		if (held) {
			c.slot = held->body.slot;
			for (const auto& part : held->parts)
				c.halves += part.slot >= 0 ? 1u : 0u;
			for (const auto& shape : held->shapes)
				c.records += bodyCoatStore->recordOf.contains(shape.geometry) ? 1u : 0u;
		}

		std::vector<std::string> skinLines;
		std::vector<std::string> firstLines;
		for (const auto& s : walked.skins) {
			const auto& map = s.map;
			if (s.firstPerson) {
				c.firstSkinned++;
				if (map.chained) {
					c.firstChained++;
					c.firstResidual = std::max(c.firstResidual, map.residual);
				}
				if (a_dump)
					firstLines.push_back(std::format("  first-person skin '{}': {}", NameOf(s.geometry),
						map.chained ? std::format("via '{}' ({} of {} shared agree), residual {:.2f}{}", map.via, map.agree, map.shared, map.residual,
										  map.outliers.empty() ? "" : std::format(", outliers {}", OutlierText(map))) :
									  std::string("NOT MAPPED (no bone reaches the body frame)")));
				continue;
			}
			c.skinned++;
			c.triangles += s.triangles;
			c.dynamic += s.dynamic ? 1u : 0u;
			c.outside += s.outside ? 1u : 0u;
			if (s.feature == int(RE::BSShaderMaterial::Feature::kFaceGen) || s.feature == int(RE::BSShaderMaterial::Feature::kFaceGenRGBTint))
				c.skinShapes++;
			c.rawOffsetMax = std::max(c.rawOffsetMax, s.rootParentToSkin.translate.Length());
			if (s.frame == 0)
				c.chained++;
			else if (s.frame > 0)
				c.componentSkins++;
			if (s.frame >= 0) {
				c.outlierBones += uint32_t(map.outliers.size());
				if (map.residual > c.worstResidual) {
					c.worstResidual = map.residual;
					c.worstShape = NameOf(s.geometry);
				}
			}
			if (a_dump) {
				const auto& rps = s.rootParentToSkin.translate;
				const std::string frameText = s.frame == 0 ? std::string("body frame") :
				                              s.frame > 0  ? std::format("frame {} (its own, seed '{}', slot {}{})", s.frame, parts[s.frame - 1].seed, parts[s.frame - 1].slot,
																 parts[s.frame - 1].half >= 0 ? std::format(" half {}", parts[s.frame - 1].half) : std::string()) :
				                                           std::string("UNMAPPED");
				skinLines.push_back(std::format("  skin '{}': {} bones, {} via '{}' ({} of {} shared agree), residual {:.2f}{}; rootParentToSkin ({:.1f}, {:.1f}, {:.1f}); feature {}{}{}{}",
					NameOf(s.geometry), s.bones.size(), frameText, map.via, map.agree, map.shared, map.residual,
					map.outliers.empty() ? "" : std::format(", outliers {}", OutlierText(map)), rps.x, rps.y, rps.z, s.feature, s.dynamic ? ", dynamic" : "",
					s.hidden ? ", hidden" : "", s.outside ? ", PAST ITS BOUNDS" : ""));
			}
		}

		if (a_dump) {
			const RE::NiPoint3 size = c.boundsMax - c.boundsMin;
			dumpLine(std::format("{:08X} '{}' {:.1f} m{}{}: frame of '{}' ({}, root bone '{}', {} bind bones, {:.0f} x {:.0f} x {:.0f}); {} skins: {} in the body frame, {} in {} frames of their own; worst residual {:.2f}{}, {} outlier bones; left out {} face parts, {} FX, {} decals; slot {}, {} halves, {} records, {} skins past their bounds; gear {} (+{} skinned), rigid worn {}, sheaths {}, attached {}; {} lit geometries owned / {} not",
				c.formID, c.name, c.distance / kUnitsPerMeter, c.corpse ? " corpse" : "", c.weaponDrawn ? " weapon drawn" : "", c.seed,
				c.sticky ? "kept by the store" : "made by this census", c.anchor, c.bindBones, size.x, size.y, size.z,
				c.skinned, c.chained, c.componentSkins, c.components, c.worstResidual, c.worstShape.empty() ? "" : std::format(" ('{}')", c.worstShape), c.outlierBones,
				c.eyeShapes, c.fx, c.decals, c.slot, c.halves, c.records, c.outside, c.gear, c.gearSkinned, c.rigidWorn, c.sheaths, c.attached, c.owned, c.unowned));
			for (const auto& line : skinLines)
				dumpLine(line);
			for (const auto& line : firstLines)
				dumpLine(line);
			for (const auto& line : walked.gearLines)
				dumpLine(line);
		}
		return c;
	};

	if (a_dump)
		logger::info("[SNOW DEFORMATION] body coat census (S1 frames): {} living, {} corpses inside the contact window; each actor's line is followed by its shapes", living.size(), dead.size());
	for (const auto& pick : living)
		bodyCoatCensus.push_back(censusOf(pick.actor.get(), pick.d2, false));
	for (const auto& pick : dead)
		bodyCoatCensus.push_back(censusOf(pick.actor.get(), pick.d2, true));
}

void SnowDeformation::ProbeBodyCoatDraw(RE::BSRenderPass* a_pass)
{
	if (!debugBodyCoatDrawProbe || !a_pass || !a_pass->geometry)
		return;
	auto* context = globals::d3d::context;
	if (!bodyCoatProbeSRV) {
		D3D11_TEXTURE3D_DESC desc{ 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
		const uint32_t zero = 0;
		D3D11_SUBRESOURCE_DATA init{ &zero, 4, 4 };
		if (FAILED(globals::d3d::device->CreateTexture3D(&desc, &init, bodyCoatProbeTexture.put())) ||
			FAILED(globals::d3d::device->CreateShaderResourceView(bodyCoatProbeTexture.get(), nullptr, bodyCoatProbeSRV.put()))) {
			debugBodyCoatDrawProbe = false;
			logger::warn("[SNOW DEFORMATION] body coat probe: could not create the probe texture");
			return;
		}
		Util::SetResourceName(bodyCoatProbeTexture.get(), "SnowDeformation::BodyCoatProbe");
		Util::SetResourceName(bodyCoatProbeSRV.get(), "SnowDeformation::BodyCoatProbe SRV");
	}
	// With the coat live, the probe checks the atlas itself.
	ID3D11ShaderResourceView* probe = bodyCoatStore && bodyCoatStore->live ? bodyCoatStore->atlasSRV.get() : bodyCoatProbeSRV.get();

	// Once a frame, at its first Lighting pass: the bind the coat would make.
	const uint32_t frame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
	auto& t = bodyCoatDrawBuilding;
	if (frame != bodyCoatDrawFrame) {
		if (bodyCoatDrawFrame != UINT32_MAX) {
			t.frames++;
			t.framesWithLoss += bodyCoatDrawLostThisFrame ? 1u : 0u;
			if (t.frames >= kBodyCoatDrawWindow) {
				bodyCoatDrawLast = t;
				logger::info("[SNOW DEFORMATION] body coat draw probe over {} frames: {} skinned draws ({} name an actor, {} no owner, {} another form), {} rigid draws name an actor; owned draws deferred {} / forward {} / reflections {}, probe bind lost before {} / {} / {}, in {} frames",
					t.frames, t.skinnedDraws, t.skinnedOwned, t.skinnedNoOwner, t.skinnedOtherOwner, t.rigidOwned, t.ownedDeferred, t.ownedForward, t.ownedReflection,
					t.lostDeferred, t.lostForward, t.lostReflection, t.framesWithLoss);
				t = {};
			}
		}
		bodyCoatDrawFrame = frame;
		bodyCoatDrawLostThisFrame = false;
		context->PSSetShaderResources(kBodyCoatAtlasSlot, 1, &probe);
		bodyCoatProbeBound = true;
	}

	t.lightingDraws++;
	auto* geometry = a_pass->geometry;
	const bool skinned = geometry->GetGeometryRuntimeData().skinInstance != nullptr;
	auto* owner = geometry->GetUserData();
	const bool actorOwner = owner && owner->formType == RE::FormType::ActorCharacter;
	if (skinned) {
		t.skinnedDraws++;
		if (actorOwner)
			t.skinnedOwned++;
		else if (!owner)
			t.skinnedNoOwner++;
		else
			t.skinnedOtherOwner++;
	} else if (actorOwner) {
		t.rigidOwned++;
	}
	if (!actorOwner)
		return;

	// A reflection draw is one inside the cubemap camera's render, which marks
	// it in the descriptor; State::activeReflections stays set from that
	// render to Present, so it cannot tell (round 2's split read it).
	const bool reflection = (globals::state->permutationData.ExtraShaderDescriptor & uint32_t(State::ExtraShaderDescriptors::IsReflections)) != 0;
	const bool deferred = !reflection && globals::deferred && globals::deferred->deferredPass;
	(reflection ? t.ownedReflection : deferred ? t.ownedDeferred : t.ownedForward)++;
	ID3D11ShaderResourceView* bound = nullptr;
	context->PSGetShaderResources(kBodyCoatAtlasSlot, 1, &bound);
	const bool held = bound == probe;
	if (bound)
		bound->Release();
	if (!held) {
		(reflection ? t.lostReflection : deferred ? t.lostDeferred : t.lostForward)++;
		bodyCoatDrawLostThisFrame = true;
		context->PSSetShaderResources(kBodyCoatAtlasSlot, 1, &probe);
	}
}

std::string SnowDeformation::BodyCoatWriterLine() const
{
	if (!bodyCoatStore)
		return debugBodyCoatContact ? "Contact writer (S3): no store yet." : "Contact writer (S3): off.";
	const auto& store = *bodyCoatStore;
	const char* writer = !debugBodyCoatContact ? "off" :
	                     store.force != 0      ? "off while Force Coat is on" :
	                     !store.writerTried    ? "waiting for an actor in snow" :
	                     store.writerReady     ? "on" :
	                                             "failed (see CommunityShaders.log)";
	return std::format("Contact writer (S3): {}. Last update: {} actors, {} triangles tested, {} voxels raised (read a few frames late). Due this frame: {}.",
		writer, store.writtenActorsLast, store.writtenTrianglesLast, store.raisedLast, store.due.size());
}

std::vector<std::string> SnowDeformation::BodyCoatProbeLines() const
{
	std::vector<std::string> lines;
	static constexpr const char* kViews[] = { "the look", "canonical bands", "slot pattern from the atlas", "slots and frames" };
	static constexpr const char* kForces[] = { "off", "legs to the knee", "snowfall from above", "left half", "everything" };
	if (!bodyCoatStore) {
		lines.push_back("Body coat: off (Force Coat, Body Coat View and Contact Coat are all off). The store is empty and nothing is bound.");
	} else {
		const auto& store = *bodyCoatStore;
		uint32_t full = 0, halves = 0, refused = 0, components = 0;
		for (uint32_t k = 0; k < kBodyCoatSlots; ++k) {
			if (store.slotFull[k])
				full++;
			else
				halves += (store.slotOwner[k][0] ? 1u : 0u) + (store.slotOwner[k][1] ? 1u : 0u);
		}
		for (const auto& entry : store.actors) {
			refused += entry.second.refused ? 1u : 0u;
			components += uint32_t(entry.second.parts.size());
		}
		const char* lumpLook = (store.lookFlags & kBodyCoatFlagLumpDepth) ? "in depth" : "today's";
		lines.push_back(std::format("Body coat: {}; forced coat {}, lumps {:.2f} ({}), specks over {:.0f} % of a surface and hair's {:.2f} at {:.1f}-{:.1f} mm, {}, relief {}. {}", kViews[std::clamp(store.view, 0, 3)],
			kForces[std::clamp(store.force, 0, 4)], store.amounts[0], lumpLook, store.amounts[1] * kSpeckMax * 100.0f, store.amounts[2], store.look[1] * 2.0f * 14.2875f, store.look[2] * 2.0f * 14.2875f,
			(store.lookFlags & kBodyCoatFlagFlatPaint) ? "flat paint" : "crust",
			(store.lookFlags & kBodyCoatFlagReliefView) ? "shown" : ((store.lookFlags & kBodyCoatFlagRelief) ? "on" : "off"),
			store.gpuFailed ? "The atlas could not be made; see CommunityShaders.log." : ""));
		lines.push_back(std::format("Store: {} actors ({} refused as incorporeal), {} components; slots {} bodies + {} halves of {}; {} records ({}). Last frame: {} coated draws, {} draws found no record, {} walks.",
			store.actors.size(), refused, components, full, halves, kBodyCoatSlots, store.records.size(), store.context1 ? "bound by offset" : "written per draw",
			store.coatedLast, store.missesLast, store.walksLast));
	}

	if (!bodyCoatCensusRan) {
		lines.push_back("Census: not run yet.");
	} else {
		uint32_t skins = 0, chained = 0, componentSkins = 0, components = 0, outliers = 0, decals = 0, fx = 0, eyes = 0, outside = 0, noNormals = 0, dynamic = 0;
		uint32_t gear = 0, gearSkinned = 0, rigidWorn = 0, sheaths = 0, attached = 0, owned = 0, unowned = 0;
		uint64_t triangles = 0;
		float worst = 0.0f, rawOffset = 0.0f;
		for (const auto& c : bodyCoatCensus) {
			skins += c.skinned;
			chained += c.chained;
			componentSkins += c.componentSkins;
			components += c.components;
			outliers += c.outlierBones;
			decals += c.decals;
			fx += c.fx;
			eyes += c.eyeShapes;
			outside += c.outside;
			noNormals += c.noNormals;
			dynamic += c.dynamic;
			gear += c.gear;
			gearSkinned += c.gearSkinned;
			rigidWorn += c.rigidWorn;
			sheaths += c.sheaths;
			attached += c.attached;
			owned += c.owned;
			unowned += c.unowned;
			triangles += c.triangles + c.gearTriangles;
			worst = std::max(worst, c.worstResidual);
			rawOffset = std::max(rawOffset, c.rawOffsetMax);
		}
		lines.push_back(std::format("Census {:.0f} frames ago, {:.2f} ms: {} actors, {} skins, {} triangles in all; left out {} face parts, {} FX, {} engine decals.",
			double(globals::state->frameCount - bodyCoatCensusFrame), bodyCoatCensusMs, bodyCoatCensus.size(), skins, triangles, eyes, fx, decals));
		lines.push_back(std::format("Frames through the bones: {} skins in their actor's body frame, {} in {} frames of their own; worst residual {:.2f} (a voxel is {:.1f}); {} outlier bones dropped by the vote; {} skins past their frame's bounds. Raw skin frames sit up to {:.1f} units apart.",
			chained, componentSkins, components, worst, kBodyCoatVoxel, outliers, outside, rawOffset));
		lines.push_back(std::format("Gear: {} rigid and {} skinned under weapon, shield or quiver slots; {} rigid worn pieces; {} sheaths; {} attached to no equipped slot. Skins without vertex normals {}, dynamic {}. Owners: {} lit geometries name their actor, {} do not.",
			gear, gearSkinned, rigidWorn, sheaths, attached, noNormals, dynamic, owned, unowned));
		for (const auto& c : bodyCoatCensus) {
			const RE::NiPoint3 size = c.boundsMax - c.boundsMin;
			const bool sized = c.chained > 0 && size.x > 0.0f;
			lines.push_back(std::format("{} ({:.0f} m{}{}): frame of '{}' ({}, root '{}'), {} skins, {} in it / {} in {} of their own, worst residual {:.2f}{}, {} outliers; bounds {}; slot {} (+{} halves), {} records, {} past bounds; gear {} (+{} skinned, radius up to {:.0f}), worn {}, sheaths {}, attached {}; owners {}/{}",
				c.name.empty() ? std::format("{:08X}", c.formID) : c.name, c.distance / kUnitsPerMeter, c.corpse ? ", corpse" : "", c.weaponDrawn ? ", weapon drawn" : "",
				c.seed, c.sticky ? "kept" : "fresh", c.anchor, c.skinned, c.chained, c.componentSkins, c.components, c.worstResidual,
				c.worstShape.empty() ? "" : std::format(" ('{}')", c.worstShape), c.outlierBones,
				sized ? std::format("{:.0f} x {:.0f} x {:.0f}", size.x, size.y, size.z) : std::string("(none)"), c.slot, c.halves, c.records, c.outside,
				c.gear, c.gearSkinned, c.gearRadiusMax, c.rigidWorn, c.sheaths, c.attached, c.owned, c.owned + c.unowned));
			if (c.firstSkinned > 0 || c.firstOwned + c.firstUnowned > 0)
				lines.push_back(std::format("  first person: {} skins, {} mapped through the third-person bind, worst residual {:.2f}; owners {}/{}", c.firstSkinned, c.firstChained,
					c.firstResidual, c.firstOwned, c.firstOwned + c.firstUnowned));
		}
	}

	if (!debugBodyCoatDrawProbe) {
		lines.push_back("Draw probe: off.");
	} else if (bodyCoatDrawLast.frames == 0) {
		lines.push_back(std::format("Draw probe: measuring, {} of {} frames.", bodyCoatDrawBuilding.frames, kBodyCoatDrawWindow));
	} else {
		const auto& t = bodyCoatDrawLast;
		const double frames = double(t.frames);
		lines.push_back(std::format("Draw probe, last {} frames, per frame: {:.0f} Lighting draws, {:.1f} skinned ({:.1f} name an actor, {:.1f} no owner, {:.1f} another form), {:.1f} rigid draws name an actor.",
			t.frames, t.lightingDraws / frames, t.skinnedDraws / frames, t.skinnedOwned / frames, t.skinnedNoOwner / frames, t.skinnedOtherOwner / frames, t.rigidOwned / frames));
		lines.push_back(std::format("Probe bind (t{}, once a frame{}) at owned draws: deferred {} lost of {}, forward {} of {}, reflections {} of {}; frames with a loss {} of {}.",
			kBodyCoatAtlasSlot, bodyCoatStore && bodyCoatStore->live ? ", the coat atlas" : "", t.lostDeferred, t.ownedDeferred, t.lostForward, t.ownedForward,
			t.lostReflection, t.ownedReflection, t.framesWithLoss, t.frames));
	}

	const auto& sky = globals::features::skylighting;
	if (!sky.loaded) {
		lines.push_back("Skylighting: not loaded. Snowfall shelter would fall back to the precipitation occlusion map.");
	} else {
		const float halfAcross = sky.occlusionDistance * 0.5f;
		const float tall = sky.occlusionDistance * 0.5f;
		lines.push_back(std::format("Skylighting: loaded; probe grid {}x{}x{}, +-{:.0f} units across and {:.0f} tall around the camera; the near tier reaches +-{:.0f}, so it is {}.",
			sky.probeArrayDims[0], sky.probeArrayDims[1], sky.probeArrayDims[2], halfAcross, tall, kContactHalfExtent,
			halfAcross >= kContactHalfExtent ? "inside the grid" : "partly outside the grid"));
	}
	return lines;
}
