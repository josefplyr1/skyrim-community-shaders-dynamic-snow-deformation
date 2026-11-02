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
	// S1's cadence (estimates): the tier is picked every kPickFrames, a bare
	// actor out of it keeps its slots kKeepFrames past its last pick, its 3D is
	// walked again every kRewalkFrames (staggered by form ID), and at most
	// kWalksPerFrame walks run in one frame.
	constexpr uint32_t kPickFrames = 30;
	constexpr uint32_t kKeepFrames = 120;
	constexpr uint32_t kRewalkFrames = 300;
	constexpr uint32_t kWalksPerFrame = 4;
	// Round 8 (section 2.7, estimates): a coat whose actor's 3D is gone waits
	// kGoneSeconds for it (a load door, a follower coming through after the
	// player), a teammate's as long as it lives. An actor waiting for a slot
	// takes one from an actor out of the tier, a bare one, or one kEvictRatio as
	// far away.
	constexpr double kGoneSeconds = 10.0;
	constexpr float kEvictRatio = 1.5f;
	// Round 8 (estimates): one first seen indoors within kDoorReach of a door to
	// the outside, kResidentSeconds or more after the player came in, came in
	// from outside; a door's arrival marker stands a step inside it, and the
	// residents' 3D is in by then.
	constexpr float kDoorReach = 256.0f;
	constexpr double kResidentSeconds = 3.0;
	// A draw without a record walks its actor again, at most this often.
	constexpr uint32_t kMissFrames = 10;
	// S3's cadence (section 2.2, estimates): the player every kWritePlayerFrames,
	// at most kWritesPerFrame actors' coat passes a frame, the rest in turn.
	// Round 4 halved both for the update's cost: the growth integrates the time
	// between updates, so the coat comes out the same.
	constexpr uint32_t kWritePlayerFrames = 4;
	constexpr uint32_t kWritesPerFrame = 2;
	// Round 2: the time between an actor's updates builds its coat, counted up
	// to kWriteGapMax (estimate: past a turn of the rotation at a low frame
	// rate). The look draws a stored contact of kCoatContactFront as a full coat
	// (SnowLighting.hlsli kCoatSpeckFront).
	constexpr float kWriteGapMax = 1.0f;
	constexpr float kCoatContactFront = 0.5f;
	// Round 4: each due actor's snow-top tile, kCoatTileDim nodes a side
	// (SnowContactCapture.hlsl), stacked in rows for up to kCoatTiles actors.
	constexpr uint32_t kCoatTileDim = 64;
	constexpr uint32_t kCoatTiles = 4;
	static_assert(kWritesPerFrame <= kCoatTiles);
	// Round 3: Snow Climate's moist snow (ClimateStickiness). Contact snow holds
	// its full share of lumps from here up, drier snow fewer, as grains instead.
	constexpr float kCoatMoistStickiness = 0.7f;
	// S4 (section 2.5): a full coat in atlas levels; contact draws full at
	// kCoatContactFront, snowfall at 1.
	constexpr float kCoatFullLevels[2] = { kCoatContactFront * 255.0f, 255.0f };
	// Packed contact snow sheds half of what a jolt knocks off loose snow (estimate).
	constexpr float kCoatPackedShed = 0.5f;
	// Slots that only fade resolve about twice a second (section 2.2), at once
	// when one owes kFadeSoon levels (a fire's melt).
	constexpr float kFadeGap = 0.5f;
	constexpr float kFadeSoon = 8.0f;
	// A frame that brings this many game hours is a wait, a sleep or a fast travel.
	constexpr float kJumpHours = 0.25f;
	// After an equipment change, walks compare what is worn for this many frames.
	// A 3D still loading can show a piece missing, so walks outside it only add.
	constexpr uint32_t kWornWindowFrames = 90;
	// S5's cadence (section 2.4, estimates): a living actor's snowfall about
	// every kFallGap of real time, a settled corpse's every kFallGapSettled (its
	// exposure cannot change), at most kFallsPerFrame a frame. The fill
	// integrates the game time between, so the coat comes out the same.
	constexpr float kFallGap = 0.5f;
	constexpr float kFallGapSettled = 2.0f;
	constexpr uint32_t kFallsPerFrame = 1;
	static_assert(kWritesPerFrame + kFallsPerFrame <= kCoatTiles);
	// The fill time (section 2.4, D11): the ground refills Snow 01's 30-unit
	// layer in kBaseRefillTime at intensity 1, and a full coat is the object
	// coat's 3 units, so 70 s.
	constexpr float kCoatFillShare = 3.0f / 30.0f;
	// D20 (Josef, 2026-10-31): the snowfall's reach rises from the angle of
	// repose to 85 degrees, whatever the snow's stickiness.
	constexpr float kFallReposeTop = 85.0f;

	// SnowContactCapture.hlsl BodyCoatWriteCB (VS b3) and BodyCoatResolveCB (CS b0).
	struct alignas(16) BodyCoatWriteCB
	{
		float4 ModelToAtlas[3];
		float4 AtlasLo;
		float4 AtlasHi;
		float Growth;
		float Heading[2];
		float ImpactShare;
		float4 Tile;
		float Texel;
		float Contact;
		float Fall;
		float Sky;
		float4 ModelToCanonical[3];
		float4 Top;
		float Eye[3];
		float Voxel;
		float From[3];
		float TopZ;
		float Moving;
		float pad[3];
	};
	static_assert(sizeof(BodyCoatWriteCB) == 240);
	struct alignas(16) BodyCoatTileCB
	{
		float4 Origin[kCoatTiles];
		uint32_t Count;
		uint32_t pad[3];
	};
	static_assert(sizeof(BodyCoatTileCB) == 80);
	struct alignas(16) BodyCoatResolveCB
	{
		uint32_t Slots[32];
		uint32_t Count;
		uint32_t Cap;
		uint32_t Seed;
		uint32_t pad;
		// S4: each slot half's fade (slot x 2 + half): the levels melted from R and
		// G, then the shares of R and G shaken off, in 1/255, a byte each.
		uint32_t Fade[64];
		// S5: each slot half's snowfall, the fill's exponent at full exposure.
		float Fall[64];
	};
	static_assert(sizeof(BodyCoatResolveCB) == 656);
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
		/** @brief S4: the most its R (contact) and G (snowfall) can hold, in levels, from what writes and fades it; 0 is bare, known without a readback. */
		float bound[2] = {};
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
		/** @brief The form ID of the item equipped in its biped slot, 0 outside one. */
		uint32_t item = 0;
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
	/** @brief S4: a worn shape as an equipment change sees it (section 2.8): keyed by name, biped slot and item, never by pointer, with its bone spheres in its frame (xyz, radius) and the coat half that frame held. */
	struct Worn
	{
		std::string name;
		int slot = -1;
		uint32_t item = 0;
		int frame = -1;
		int coatSlot = -1;
		int coatHalf = -1;
		std::vector<float4> spheres;
		bool Same(const Worn& a_other) const { return slot == a_other.slot && item == a_other.item && name == a_other.name; }
	};
	/** @brief S4's readout: what faded an actor's coat at its last tick, and why. Rates per real second at the current timescale. */
	struct Fading
	{
		bool active = false;
		CoatAging aging;
		bool burning = false;
		/** @brief A corpse lying settled in the snow: the weather does not take its coat. */
		bool inSnow = false;
		/** @brief Full coats melted a second (the clear-weather clock, heat, fire spells), and the share shaken off a second. */
		float melt = 0.0f;
		float shake = 0.0f;
		float motion = 0.0f;
		/** @brief The last jolt's share shed at once, and when (the store's clock). */
		float jolt = 0.0f;
		double joltAt = -1.0;
		uint32_t resolves = 0;
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
		// S3: the time since its last coat update, the largest jolt in it, and
		// where it stood at that update.
		float sinceWrite = 0.0f;
		float joltHeld = 0.0f;
		RE::NiPoint3 lastPos;
		bool hasLastPos = false;
		// S4: the fade owed since the last fade resolve (levels of R and G), the
		// share of each a shake-off would leave, the impact track as E0 last read
		// it, the fire-hit memory as last read, what it wears, and the readout.
		float meltOwed[2] = {};
		float keep[2] = { 1.0f, 1.0f };
		double trackTime = -1.0;
		bool knocked = false;
		bool rushing = false;
		float fireHitSeen = -1.0f;
		uint64_t wornSignature = 0;
		std::vector<Worn> worn;
		bool wornKnown = false;
		bool wornChanging = false;
		uint32_t wornChangeFrame = 0;
		Fading fading;
		std::string changeLine;
		double changeAt = -1.0;
		// Round 2: its own ground speed, measured as the impact track measures it,
		// which sleeps off the snow with the deformation passes. Round 3: a
		// corpse's at its bound's centre, and whether it lay settled last frame.
		RE::NiPoint3 speedPos;
		bool hasSpeedPos = false;
		float speed = 0.0f;
		bool wasSettled = false;
		// S5: the real time since its last snowfall update, the fill owed since
		// (intensity x game seconds over the fill time), whether its next update
		// seeds it (first seen outdoors while it snows), and the last shout cone
		// that blew on it; for the readout, its last update's seed and the last
		// shout's share.
		float sinceFall = 0.0f;
		float fallDose = 0.0f;
		bool fallSeed = false;
		uint64_t lastShout = 0;
		float fallSeeded = 0.0f;
		double fallAt = -1.0;
		double shoutAt = -1.0;
		float shoutShare = 0.0f;
		// Round 8 (section 2.7): a teammate, among those the capture rasterizes
		// (the only ones contact reaches), its distance squared at the last pick,
		// when its 3D was last loaded, waiting for a slot; and a seed for one come
		// in from a storm, laid under an open sky, and whether the last was one.
		bool teammate = false;
		bool nearTier = false;
		float d2 = 0.0f;
		double loadedAt = 0.0;
		bool waiting = false;
		bool seedIndoors = false;
		bool seededAtDoor = false;
	};
	struct Pick
	{
		float d2;
		RE::NiPointer<RE::Actor> actor;
		RE::ActorHandle handle;
		bool player;
		bool teammate = false;
		bool nearTier = false;
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
	/** @brief S5: what the stored snowfall shows (D20, D21): x the share that fills the specks, y and z the angle of repose it rises from and to, in radians. */
	float fallLook[4] = {};
	bool hairSpecks = false;

	winrt::com_ptr<ID3D11Texture3D> atlas;
	winrt::com_ptr<ID3D11ShaderResourceView> atlasSRV;
	winrt::com_ptr<ID3D11DeviceContext1> context1;
	winrt::com_ptr<ID3D11Buffer> recordCB[2];
	winrt::com_ptr<ID3D11Buffer> fallbackCB;
	uint32_t parity = 0;
	uint32_t atlasBoundFrame = UINT32_MAX;
	/** @brief S5: the last Lighting draw was a corpse's model-space-normal skin, drawn with the coat's pixel shader in place of its technique's. */
	bool swappedPixelShader = false;
	bool gpuFailed = false;
	/** @brief The atlas as R8G8B8A8 words, for the writer's resolve. */
	winrt::com_ptr<ID3D11UnorderedAccessView> atlasUAV;

	// S3's contact writer (SnowContactCapture.hlsl, COAT): who it updates this
	// frame, the touches in two scratch volumes the atlas's size (contact; the
	// snowfall with the wash), its shaders and constants, and the readout.
	struct Due
	{
		uint32_t formID;
		// x the growth in atlas levels, yz the heading, w the impact share.
		float4 write;
		// Its snow-top tile: xy the first node in world, z the node spacing (0:
		// none), w the first row.
		float4 tile;
		// S5: x the fill's exponent at full exposure, y the seed's share of it, z 1
		// when this update measures snowfall.
		float4 fall;
		// Its tops: xy the centre of its window, z the window's half extent (0:
		// none), w the first row.
		float4 top;
		bool contact = false;
		bool seed = false;
		// A shape of it was drawn: the snowfall it owed is paid.
		bool drawn = false;
		// The tops' centre height.
		float topZ = 0.0f;
		// D30: how far it moved since, in deformation texels up to 1: its impacts'
		// weight, the lumps' only source.
		float moving = 0.0f;
		// Round 8: a seed for one come in from a storm, under an open sky.
		bool openSky = false;
	};
	size_t currentDue = 0;
	std::vector<Due> due;
	// S5: the due actors the capture does not draw (on a road, a rock or a
	// horse, or a settled corpse), walked for their coat only.
	std::vector<ContactProp> coatOnly;
	uint32_t fallRobin = 0;
	uint32_t roundRobin = 0;
	uint32_t touched = 0;
	uint32_t capLevel = 0;
	winrt::com_ptr<ID3D11Texture3D> contactScratch, fallScratch;
	winrt::com_ptr<ID3D11UnorderedAccessView> contactUAV, fallUAV;
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
	// VS b3, b5, b6; t0, t1, t2, t63; GS b3.
	winrt::com_ptr<ID3D11Buffer> savedCB[4];
	winrt::com_ptr<ID3D11ShaderResourceView> savedSRV[4];
	winrt::com_ptr<ID3D11GeometryShader> savedGS;
	// S5: each fall-due actor's tops, the highest its own body reaches toward
	// the snow over each texel across the snow's way (64 x 64 a row), drawn by
	// their own VS, the blend that keeps the highest, and the actor drawn now;
	// the eye Skylighting placed its probes about, and its probes when it runs.
	winrt::com_ptr<ID3D11Texture2D> topTexture;
	winrt::com_ptr<ID3D11RenderTargetView> topRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> topSRV;
	winrt::com_ptr<ID3D11BlendState> topBlend;
	winrt::com_ptr<ID3D11VertexShader> topVS;
	winrt::com_ptr<ID3D11Buffer> savedTopCB;
	float4 currentFall{};
	float4 currentTop{};
	float currentTopZ = 0.0f;
	float currentMoving = 0.0f;
	bool currentOpenSky = false;
	bool currentContact = false;
	float eye[3] = {};
	ID3D11ShaderResourceView* skySRV = nullptr;
	// S5: this frame's snowfall here (capped as the ground's refill reads it),
	// the way it comes from (round 2: the game's own falling snow, tilted by the
	// wind), and for the readout the fill time, that way's tilt from straight
	// up and the game's own, and fall passes in the last second.
	float fallIntensity = 0.0f;
	float fallFrom[3] = { 0.0f, 0.0f, 1.0f };
	float fillTime = 0.0f;
	float fallTilt = 0.0f, gameTilt = 0.0f;
	uint32_t fallPasses = 0, fallPassesLast = 0;
	// Round 8: the way the snow last came outdoors, for a seed laid indoors; the
	// interior the player is in, its doors to the outside and when the player
	// came in; and for the readout, the actors that gave up a slot.
	float fallFromHeld[3] = { 0.0f, 0.0f, 1.0f };
	const RE::TESObjectCELL* doorCell = nullptr;
	std::vector<RE::NiPoint3> doorsOut;
	double doorsAt = 0.0;
	uint32_t evictions = 0;
	const float* From() const { return currentOpenSky ? fallFromHeld : fallFrom; }
	bool NearDoorOut(const RE::NiPoint3& a_position) const
	{
		return std::ranges::any_of(doorsOut, [&](const RE::NiPoint3& a_door) { return a_door.GetSquaredDistance(a_position) <= kDoorReach * kDoorReach; });
	}
	// The snow-top tiles (round 4), the tile of the actor drawn now, and the
	// deformation texel, the taps' reach.
	winrt::com_ptr<ID3D11Texture2D> tileTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> tileSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> tileUAV;
	winrt::com_ptr<ID3D11ComputeShader> tileCS;
	winrt::com_ptr<ID3D11Buffer> tileCB;
	float4 currentTile{};
	float deformTexel = 0.0f;
	bool writerTried = false, writerReady = false;
	uint32_t writtenActorsLast = 0, writtenTrianglesLast = 0, raisedLast = 0;
	// S4: the coat's own reading of the calendar, the store's real-time clock,
	// the slot halves the writer drew into this frame, the fade's cadence, and
	// the readout's jumps, reloads and resolves a second.
	float clockHours = -1.0f;
	float timescale = 20.0f;
	double realClock = 0.0;
	uint64_t touchedHalves = 0;
	float sinceFade = 0.0f;
	float lastJumpHours = 0.0f;
	double lastJumpAt = -1.0;
	double reloadAt = -1.0;
	uint32_t fadeResolves = 0, fadeResolvesLast = 0;
	double fadeWindowStart = 0.0;

	uint32_t drawFrame = UINT32_MAX;
	uint32_t coatedBuilding = 0, coatedLast = 0, walksLast = 0, missesLast = 0;

	// The tier (round 8, section 2.7): every actor whose bound lies inside the
	// contact window, the player first, then teammates, then the nearest; the
	// slots cap it. The near tier is the part the contact capture rasterizes,
	// as it picks it (the player and the nearest living actors, then the
	// nearest corpses, each at the capture's own cap): contact reaches no one
	// else.
	static void PickTier(std::vector<Pick>& a_picks)
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
			a_picks.push_back({ a_d2, a_actor, a_handle, a_player, !a_player && a_actor->IsPlayerTeammate() });
		};
		consider(RE::NiPointer<RE::Actor>(player), {}, -1.0f, true);
		if (const auto processLists = RE::ProcessLists::GetSingleton())
			for (auto& handle : processLists->highActorHandles)
				if (auto actor = handle.get(); actor)
					consider(actor, handle, eye.GetSquaredDistance(actor->GetPosition()), false);
		std::sort(a_picks.begin(), a_picks.end(), [](const Pick& a_a, const Pick& a_b) { return a_a.d2 < a_b.d2; });
		uint32_t living = 0, dead = 0;
		for (auto& pick : a_picks)
			pick.nearTier = pick.actor->IsDead() ? dead++ < kContactMaxCorpses : living++ < kContactMaxActors;
		std::stable_sort(a_picks.begin(), a_picks.end(), [](const Pick& a_a, const Pick& a_b) { return (a_a.player || a_a.teammate) && !(a_b.player || a_b.teammate); });
	}

	static bool Bare(const Actor& a_actor)
	{
		auto empty = [](const Frame& a_frame) { return a_frame.slot < 0 || (a_frame.bound[0] <= 0.0f && a_frame.bound[1] <= 0.0f); };
		return empty(a_actor.body) && std::ranges::all_of(a_actor.parts, empty);
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
		struct Part
		{
			const RE::NiAVObject* clone;
			uint32_t slot;
			uint32_t item;
		};
		std::vector<Part> parts;
		auto addBiped = [&](const RE::BSTSmartPointer<RE::BipedAnim>& a_biped) {
			if (!a_biped)
				return;
			for (uint32_t i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i)
				if (const RE::NiAVObject* part = a_biped->objects[i].partClone.get())
					parts.push_back({ part, i, a_biped->objects[i].item ? a_biped->objects[i].item->formID : 0u });
		};
		addBiped(a_actor->GetActorRuntimeData().biped);
		if (a_actor == player)
			addBiped(player->GetInfoRuntimeData().largeBiped);
		auto partOf = [&](const RE::NiAVObject* a_object) -> const Part* {
			for (const RE::NiAVObject* n = a_object; n; n = n->parent)
				for (const auto& part : parts)
					if (part.clone == n)
						return &part;
			return nullptr;
		};
		auto carried = [](int a_slot) { return a_slot >= int(RE::BIPED_OBJECTS::kEditorTotal) || a_slot == int(RE::BIPED_OBJECTS::kShield); };

		// Eyes by their head part (section 5.5): an eye mod's eyes wear the
		// environment-map material, not the eye one. A facegen head names each
		// part's geometry after its editor ID: the NPC's own eyes, or its race's.
		// Brows and scars sit on the bare face with them (section 5.10), and on a
		// corpse take the coat with its face (S5). A mouth (teeth and tongue: a
		// misc part named or modelled as one) never takes it, nor its extra parts.
		const bool corpse = a_actor->IsDead();
		std::vector<const char*> eyeParts;
		auto mouth = [](const RE::BGSHeadPart* a_part) {
			for (const char* text : { a_part->formEditorID.c_str(), a_part->model.c_str() }) {
				std::string lower(text ? text : "");
				std::ranges::transform(lower, lower.begin(), [](unsigned char a_c) { return char(std::tolower(a_c)); });
				if (lower.find("mouth") != std::string::npos || lower.find("teeth") != std::string::npos || lower.find("tongue") != std::string::npos)
					return true;
			}
			return false;
		};
		auto bare = [&](const RE::BGSHeadPart* a_part) {
			using Type = RE::BGSHeadPart::HeadPartType;
			return a_part && a_part->formEditorID.c_str() &&
			       (a_part->type == Type::kEyes || (!corpse && (a_part->type == Type::kEyebrows || a_part->type == Type::kScar)) || (a_part->type == Type::kMisc && mouth(a_part)));
		};
		auto addEyes = [&](const RE::BGSHeadPart* a_part) {
			if (!a_part)
				return;
			if (bare(a_part))
				eyeParts.push_back(a_part->formEditorID.c_str());
			for (const auto* extra : a_part->extraParts)
				if (bare(extra))
					eyeParts.push_back(extra->formEditorID.c_str());
		};
		if (auto* npc = a_actor->GetActorBase()) {
			addEyes(npc->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kEyes));
			addEyes(npc->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kEyebrows));
			addEyes(npc->GetCurrentHeadPartByType(RE::BGSHeadPart::HeadPartType::kScar));
			for (int i = 0; npc->headParts && i < npc->numHeadParts; ++i)
				addEyes(npc->headParts[i]);
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
			const Part* part = a_first ? nullptr : partOf(a_geometry);
			const int slot = part ? int(part->slot) : -1;
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
			s.item = part ? part->item : 0u;
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
		// Twice a slot wide: the impact store (D30) lies beside the coat along x.
		D3D11_TEXTURE3D_DESC desc{ kBodyCoatSlotDim[0] * 2, kBodyCoatSlotDim[1], kBodyCoatSlotDim[2] * kBodyCoatSlots, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_USAGE_DEFAULT,
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
		auto tile = blob("cs_5_0", "CSHADER", "COAT", "COAT_TILE");
		winrt::com_ptr<ID3DBlob> top;
		top.attach(SD_CompileShaderBlob(kWritePath, "vs_5_0", "VSHADER", "SKINNED", "COAT", "COAT_TOP"));
		if (!vs || !gs || !ps || !cs || !tile || !top || FAILED(device->CreateVertexShader(top->GetBufferPointer(), top->GetBufferSize(), nullptr, topVS.put())) ||
			FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, writeVS.put())) ||
			FAILED(device->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, writeGS.put())) ||
			FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, writePS.put())) ||
			FAILED(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, resolveCS.put())) ||
			FAILED(device->CreateComputeShader(tile->GetBufferPointer(), tile->GetBufferSize(), nullptr, tileCS.put()))) {
			logger::warn("[SNOW DEFORMATION] body coat: the contact writer's shaders did not compile; it stays off");
			return false;
		}
		Util::SetResourceName(writeVS.get(), "SnowDeformation::BodyCoatWriteVS");
		Util::SetResourceName(writeGS.get(), "SnowDeformation::BodyCoatWriteGS");
		Util::SetResourceName(writePS.get(), "SnowDeformation::BodyCoatWritePS");
		Util::SetResourceName(resolveCS.get(), "SnowDeformation::BodyCoatResolveCS");
		Util::SetResourceName(tileCS.get(), "SnowDeformation::BodyCoatTileCS");
		Util::SetResourceName(topVS.get(), "SnowDeformation::BodyCoatTopVS");
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
		D3D11_BUFFER_DESC tileDesc{ sizeof(BodyCoatTileCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
		D3D11_TEXTURE2D_DESC tiles{ kCoatTileDim, kCoatTileDim * kCoatTiles, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
		bool ok = scratch(contactScratch, contactUAV, "BodyCoatContactScratch") && scratch(fallScratch, fallUAV, "BodyCoatFallScratch") &&
		          SUCCEEDED(device->CreateBuffer(&raw, nullptr, raisedBuffer.put())) && SUCCEEDED(device->CreateUnorderedAccessView(raisedBuffer.get(), &rawView, raisedUAV.put())) &&
		          SUCCEEDED(device->CreateBuffer(&writeDesc, nullptr, writeCB.put())) && SUCCEEDED(device->CreateBuffer(&resolveDesc, nullptr, resolveCB.put())) &&
		          SUCCEEDED(device->CreateBuffer(&tileDesc, nullptr, tileCB.put())) && SUCCEEDED(device->CreateTexture2D(&tiles, nullptr, tileTexture.put())) &&
		          SUCCEEDED(device->CreateShaderResourceView(tileTexture.get(), nullptr, tileSRV.put())) &&
		          SUCCEEDED(device->CreateUnorderedAccessView(tileTexture.get(), nullptr, tileUAV.put()));
		for (auto& s : raisedStaging)
			ok = ok && SUCCEEDED(device->CreateBuffer(&staging, nullptr, s.put()));
		// S5's tops, blended to the highest surface (R only, as the capture's field is).
		D3D11_TEXTURE2D_DESC tops = tiles;
		tops.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_BLEND_DESC highest{};
		highest.RenderTarget[0] = { TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_COLOR_WRITE_ENABLE_RED };
		ok = ok && SUCCEEDED(device->CreateTexture2D(&tops, nullptr, topTexture.put())) && SUCCEEDED(device->CreateRenderTargetView(topTexture.get(), nullptr, topRTV.put())) &&
		     SUCCEEDED(device->CreateShaderResourceView(topTexture.get(), nullptr, topSRV.put())) && SUCCEEDED(device->CreateBlendState(&highest, topBlend.put()));
		if (!ok) {
			logger::warn("[SNOW DEFORMATION] body coat: could not create the contact writer's buffers; it stays off");
			return false;
		}
		Util::SetResourceName(topTexture.get(), "SnowDeformation::BodyCoatTops");
		Util::SetResourceName(raisedBuffer.get(), "SnowDeformation::BodyCoatRaised");
		Util::SetResourceName(writeCB.get(), "SnowDeformation::BodyCoatWriteCB");
		Util::SetResourceName(resolveCB.get(), "SnowDeformation::BodyCoatResolveCB");
		Util::SetResourceName(tileCB.get(), "SnowDeformation::BodyCoatTileCB");
		Util::SetResourceName(tileTexture.get(), "SnowDeformation::BodyCoatTiles");
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
	// deposit direction as GBuffer::EncodeNormal writes the way down (S5), so
	// straight up is 0.5, 0.5. Each voxel's pattern is taken at its canonical
	// centre. Wading contact is whole to the ankle and thins up the shin, to
	// about a sixth at the knee and none at 1.3 times its height (section 5.11);
	// the left side ramps over four voxels. The impact store beside it (D30)
	// takes the forced contact whole, so Force Coat's lumps can still be judged.
	void FillSlot(Frame& a_frame)
	{
		const int slot = a_frame.slot, half = a_frame.half;
		a_frame.bound[0] = a_frame.bound[1] = !checks && force != 0 ? 255.0f : 0.0f;
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
		std::vector<uint32_t> impacts(texels.size(), 0u);
		for (uint32_t z = 0; z < d; ++z)
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x) {
					const size_t index = (size_t(z) * h + y) * w + x;
					uint32_t& texel = texels[index];
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
					texel = pack(contact, fall, 0.5f, 0.5f);
					impacts[index] = byte(contact);
				}
		const D3D11_BOX box{ x0, 0, z0, x0 + w, h, z0 + d };
		globals::d3d::context->UpdateSubresource(atlas.get(), 0, &box, texels.data(), w * 4, w * h * 4);
		const D3D11_BOX impactBox{ x0 + kBodyCoatSlotDim[0], 0, z0, x0 + kBodyCoatSlotDim[0] + w, h, z0 + d };
		globals::d3d::context->UpdateSubresource(atlas.get(), 0, &impactBox, impacts.data(), w * 4, w * h * 4);
	}

	void RefillAll()
	{
		for (auto& entry : actors) {
			if (entry.second.body.slot >= 0)
				FillSlot(entry.second.body);
			for (auto& part : entry.second.parts)
				if (part.slot >= 0)
					FillSlot(part);
		}
	}

	// S4: what an actor has equipped, by biped slot (both of the player's
	// bipeds); a change opens an equipment change's window.
	static uint64_t WornSignature(RE::Actor* a_actor)
	{
		uint64_t hash = 14695981039346656037ull;
		auto mix = [&](const RE::BSTSmartPointer<RE::BipedAnim>& a_biped) {
			if (!a_biped)
				return;
			for (uint32_t i = 0; i < RE::BIPED_OBJECTS::kTotal; ++i) {
				const auto& object = a_biped->objects[i];
				hash = (hash ^ ((uint64_t(object.item ? object.item->formID : 0u) << 32) | (object.addon ? object.addon->formID : 0u))) * 1099511628211ull;
			}
		};
		mix(a_actor->GetActorRuntimeData().biped);
		if (auto* player = globals::game::player; a_actor == player)
			mix(player->GetInfoRuntimeData().largeBiped);
		return hash;
	}

	// A skin's bone spheres in its frame, the region a piece taken off or put on
	// clears. Each bone's own box: one box over a pair of gloves would span the
	// body between the hands.
	static std::vector<float4> SpheresOf(const Skin& a_skin)
	{
		std::vector<float4> spheres;
		for (const auto& bone : a_skin.bones) {
			if (bone.bound.radius <= 0.0f)
				continue;
			const RE::NiPoint3 centre = a_skin.map.toAnchor * (bone.boneToSkin * bone.bound.center);
			spheres.emplace_back(centre.x, centre.y, centre.z, bone.bound.radius * bone.boneToSkin.scale * a_skin.map.toAnchor.scale);
		}
		return spheres;
	}

	// Empties a frame's coat over each sphere's box (R and G 0, the deposit
	// straight up as FillSlot leaves it) and its impacts beside it; the voxels
	// written, for the readout.
	uint32_t ClearSpheres(const Frame& a_frame, const std::vector<float4>& a_spheres)
	{
		if (a_frame.slot < 0)
			return 0;
		uint32_t written = 0;
		std::vector<uint32_t> bare;
		for (const auto& sphere : a_spheres) {
			const float centre[3] = { sphere.x, sphere.y, sphere.z };
			uint32_t lo[3], hi[3];
			bool empty = false;
			for (int a = 0; a < 3; ++a) {
				const int c = a_frame.axisOf[a];
				const float t0 = (centre[c] - sphere.w) * a_frame.scale[a] + a_frame.offset[a];
				const float t1 = (centre[c] + sphere.w) * a_frame.scale[a] + a_frame.offset[a];
				lo[a] = uint32_t(std::clamp(std::floor(std::min(t0, t1)), a_frame.lo[a], a_frame.hi[a]));
				hi[a] = uint32_t(std::clamp(std::ceil(std::max(t0, t1)), a_frame.lo[a], a_frame.hi[a]));
				empty = empty || hi[a] <= lo[a];
			}
			if (empty)
				continue;
			const uint32_t w = hi[0] - lo[0], h = hi[1] - lo[1], d = hi[2] - lo[2];
			bare.assign(size_t(w) * h * d, 0x80800000u);
			const D3D11_BOX box{ lo[0], lo[1], lo[2], hi[0], hi[1], hi[2] };
			globals::d3d::context->UpdateSubresource(atlas.get(), 0, &box, bare.data(), w * 4, w * h * 4);
			bare.assign(bare.size(), 0u);
			const D3D11_BOX impactBox{ lo[0] + kBodyCoatSlotDim[0], lo[1], lo[2], hi[0] + kBodyCoatSlotDim[0], hi[1], hi[2] };
			globals::d3d::context->UpdateSubresource(atlas.get(), 0, &impactBox, bare.data(), w * 4, w * h * 4);
			written += w * h * d;
		}
		return written;
	}

	// The frame holding a slot half (the body's slot holds both), or none.
	Frame* FrameAt(uint32_t a_slot, uint32_t a_half)
	{
		if (a_slot >= kBodyCoatSlots || !slotOwner[a_slot][a_half])
			return nullptr;
		const auto it = actors.find(slotOwner[a_slot][a_half]);
		if (it == actors.end())
			return nullptr;
		if (it->second.body.slot == int(a_slot))
			return &it->second.body;
		for (auto& part : it->second.parts)
			if (part.slot == int(a_slot) && part.half == int(a_half))
				return &part;
		return nullptr;
	}

	// S4: one resolve over the slots that owe a fade, the writer's pass with
	// nothing touched; its count is left out (the readout's is the writer's).
	void FadeResolve(ID3D11DeviceContext* a_context, const BodyCoatResolveCB& a_cb)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(resolveCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		memcpy(mapped.pData, &a_cb, sizeof(a_cb));
		a_context->Unmap(resolveCB.get(), 0);
		globals::profiler->BeginPass("SnowDeformation::Bodies - Coat Update");
		winrt::com_ptr<ID3D11ComputeShader> savedCS;
		a_context->CSGetShader(savedCS.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11Buffer> savedCB0;
		a_context->CSGetConstantBuffers(0, 1, savedCB0.put());
		a_context->CSSetShader(resolveCS.get(), nullptr, 0);
		ID3D11Buffer* resolve = resolveCB.get();
		a_context->CSSetConstantBuffers(0, 1, &resolve);
		ID3D11UnorderedAccessView* uavs[4] = { atlasUAV.get(), contactUAV.get(), fallUAV.get(), nullptr };
		a_context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		a_context->Dispatch(kBodyCoatSlotDim[0] / 8, kBodyCoatSlotDim[1] / 8, kBodyCoatSlotDim[2] / 4 * a_cb.Count);
		ID3D11UnorderedAccessView* noUAV[4] = {};
		a_context->CSSetUnorderedAccessViews(0, 4, noUAV, nullptr);
		a_context->CSSetShader(savedCS.get(), nullptr, 0);
		ID3D11Buffer* cb0 = savedCB0.get();
		a_context->CSSetConstantBuffers(0, 1, &cb0);
		// The UAV unbound the atlas from Lighting's t41: the hook binds it again.
		atlasBoundFrame = UINT32_MAX;
		globals::profiler->EndPass();
	}

	// Bounds fixed at the slot's birth (section 2.1), then the slot and its
	// pattern; true when it has bounds and no slot is free (round 8: it waits).
	bool Slot(Frame& a_frame, const Walked& a_walked, int a_index, uint32_t a_formID)
	{
		if (a_frame.slot >= 0)
			return false;
		if (!a_frame.bounded) {
			float lo[3], hi[3];
			BoundsOf(a_walked, a_index, lo, hi);
			if (!(hi[0] > lo[0]))
				return false;
		}
		int slot = -1, half = -1;
		if (a_index == 0)
			slot = AllocFull(a_formID);
		else
			std::tie(slot, half) = AllocHalf(a_formID);
		if (slot < 0)
			return true;
		if (!a_frame.bounded) {
			BoundsOf(a_walked, a_index, a_frame.boundsMin, a_frame.boundsMax);
			a_frame.bounded = true;
		}
		Place(a_frame, slot, half);
		FillSlot(a_frame);
		return false;
	}

	// Round 8 (section 2.7): a slot for the first of the tier waiting for one,
	// from the actor held least: out of the tier, then bare, then the farthest.
	// Never the player or a teammate, and one in the tier only kEvictRatio as
	// far as the one waiting. False when none gives way.
	bool GiveWay(uint32_t a_frame)
	{
		Actor* waiting = nullptr;
		for (auto& [formID, a] : actors)
			if (a.waiting && a.lastPicked == a_frame && (!waiting || (a.teammate && !waiting->teammate) || (a.teammate == waiting->teammate && a.d2 < waiting->d2)))
				waiting = &a;
		if (!waiting)
			return false;
		const float reach2 = waiting->teammate ? 0.0f : waiting->d2 * kEvictRatio * kEvictRatio;
		auto victim = actors.end();
		auto rank = [&](const Actor& a_actor) { return std::make_tuple(a_actor.lastPicked != a_frame, Bare(a_actor), a_actor.d2); };
		for (auto it = actors.begin(); it != actors.end(); ++it) {
			const Actor& a = it->second;
			if (a.player || a.teammate || a.body.slot < 0)
				continue;
			if (a.lastPicked == a_frame && !Bare(a) && a.d2 < reach2)
				continue;
			if (victim == actors.end() || rank(a) > rank(victim->second))
				victim = it;
		}
		if (victim == actors.end())
			return false;
		FreeActor(victim->second);
		actors.erase(victim);
		evictions++;
		waiting->waiting = false;
		waiting->walk = true;
		return true;
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
				r.Fall = float4(fallLook[0], fallLook[1], fallLook[2], fallLook[3]);
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
	// S4's clock (section 2.5): game time, so a wait, a sleep or a fast travel
	// lands at once. The calendar is read here: gameClock sleeps with the
	// deformation passes, and an inn holds no snow to wake them. Backwards is a
	// loaded save, whose coats belong to another timeline (S8 co-saves them).
	const float realDelta = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 0.0f) : 0.0f;
	store.realClock += realDelta;
	float coatHours = 0.0f;
	bool reloaded = false;
	{
		auto* calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
		const float hours = calendar ? calendar->GetHoursPassed() : -1.0f;
		if (hours >= 0.0f && store.clockHours >= 0.0f) {
			reloaded = hours - store.clockHours < -1.0e-4f;
			coatHours = std::max(hours - store.clockHours, 0.0f);
		}
		store.clockHours = hours;
		// The player's timescale, latched as TickGameClock latches it: a wait cranks the live one.
		if (const float scale = calendar ? calendar->GetTimescale() : 0.0f; scale >= 1.0f && scale <= 100.0f)
			store.timescale = scale;
	}
	const float coatSeconds = coatHours * 3600.0f / store.timescale;
	if (coatHours >= kJumpHours) {
		store.lastJumpHours = coatHours;
		store.lastJumpAt = store.realClock;
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
	const uint32_t lumpFlags = !bodyCoatLumpSRV          ? 0u :
	                           debugBodyCoatLumpLook == 1 ? kBodyCoatFlagLumpDepth :
	                           debugBodyCoatLumpLook == 2 ? kBodyCoatFlagLumpBlend :
	                                                        0u;
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
	// The forced coat's lumps, specks and hair's specks, apart (S2's cranks), and
	// the cohesion of the snow impacts bring, its share of lumps (round 3; D30),
	// in steps of 0.05 so the records rebuild only on a step. The forced coat
	// keeps its full share.
	const float stickiness = GetSnowCondition().stickiness;
	const float cohesion = force != 0 ? 1.0f : std::round(std::clamp(stickiness / kCoatMoistStickiness, 0.0f, 1.0f) * 20.0f) / 20.0f;
	const float amounts[4] = { std::clamp(debugBodyCoatLumps, 0.0f, 1.0f), std::clamp(debugBodyCoatGrains, 0.0f, 1.0f) * grainScale,
		std::clamp(debugBodyCoatHairAmount, 0.0f, 1.0f), cohesion };
	// S5 (D20-D22): the stored snowfall fills the specks up to the load order's
	// storm split, their reach rising from the angle of repose to 85 degrees,
	// then thickens them over the rest (D29, D30).
	const float fallLook[4] = { std::clamp(SnowWeathers().split, 0.05f, 0.95f), DirectX::XMConvertToRadians(repose),
		DirectX::XMConvertToRadians(std::max(kFallReposeTop, repose)), 0.0f };
	if (store.lookFlags != lookFlags || memcmp(store.glint, glint, sizeof(glint)) != 0 || memcmp(store.look, look, sizeof(look)) != 0 ||
		memcmp(store.amounts, amounts, sizeof(amounts)) != 0 || memcmp(store.fallLook, fallLook, sizeof(fallLook)) != 0 || store.hairSpecks != debugBodyCoatHairSpecks) {
		store.lookFlags = lookFlags;
		memcpy(store.glint, glint, sizeof(glint));
		memcpy(store.look, look, sizeof(look));
		memcpy(store.amounts, amounts, sizeof(amounts));
		memcpy(store.fallLook, fallLook, sizeof(fallLook));
		store.hairSpecks = debugBodyCoatHairSpecks;
		store.recordsDirty = true;
	}
	auto* player = globals::game::player;
	// S5: the snowfall here, read every frame (the module's own sleeps with the
	// deformation passes): none indoors, under No Sky or for ash. Round 2 (D10):
	// it comes the way the game's own falling snow comes, the precipitation's
	// fall speed against 500 units a second of wind at full (Precipitation
	// GetDirection), its slant scaled by Wind Tilt.
	{
		auto* tes = globals::game::tes;
		store.fallIntensity = tes && !tes->interiorCell ? ComputeSnowfallIntensity() : 0.0f;
		const RE::NiPoint3 fall = RE::Precipitation::GetDirection();
		const float drift = fall.z < -1e-3f ? std::hypot(fall.x, fall.y) / -fall.z : 0.0f;
		const float tilt = drift > 0.0f ? std::clamp(debugBodyCoatWindTilt, 0.0f, 1.0f) / -fall.z : 0.0f;
		const float from[3] = { -fall.x * tilt, -fall.y * tilt, 1.0f };
		const float length = std::sqrt(from[0] * from[0] + from[1] * from[1] + 1.0f);
		for (int i = 0; i < 3; ++i)
			store.fallFrom[i] = from[i] / length;
		store.gameTilt = DirectX::XMConvertToDegrees(std::atan(drift));
		store.fallTilt = DirectX::XMConvertToDegrees(std::acos(std::clamp(store.fallFrom[2], -1.0f, 1.0f)));
		if (store.fallIntensity > 0.0f)
			std::copy_n(store.fallFrom, 3, store.fallFromHeld);
	}
	const bool snowing = store.fallIntensity > 0.0f;
	// Round 8: the doors out of the interior the player is in, read once a cell.
	if (auto* tes = globals::game::tes; tes && tes->interiorCell != store.doorCell) {
		store.doorCell = tes->interiorCell;
		store.doorsAt = store.realClock;
		store.doorsOut.clear();
		if (tes->interiorCell)
			tes->interiorCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (a_ref)
					if (const auto door = a_ref->extraList.GetTeleportLinkedDoor().get(); door && door->GetWorldspace())
						store.doorsOut.push_back(a_ref->GetPosition());
				return RE::BSContainer::ForEachResult::kContinue;
			});
	}

	if (!store.picked || frame - store.pickFrame >= kPickFrames) {
		store.picked = true;
		store.pickFrame = frame;
		std::vector<Store::Pick> picks;
		Store::PickTier(picks);
		for (auto& [formID, a] : store.actors)
			a.nearTier = false;
		// Round 8: indoors, Accumulation holds the storm the player came in from.
		const bool stormOutside = store.doorCell && accumWeatherIntensity.load(std::memory_order_relaxed) > 0.0f;
		for (const auto& pick : picks) {
			auto [it, inserted] = store.actors.try_emplace(pick.actor->formID);
			auto& a = it->second;
			a.lastPicked = frame;
			a.player = pick.player;
			a.teammate = pick.teammate;
			a.nearTier = pick.nearTier;
			a.d2 = pick.d2;
			if (!pick.player)
				a.handle = pick.handle;
			if (!inserted)
				continue;
			a.loadedAt = store.realClock;
			// S5: first seen outdoors in a storm, so out in it a while. Round 8: or
			// first seen at a door to the outside, come in from the storm there.
			a.fallSeed = snowing;
			if (!snowing && stormOutside && store.realClock - store.doorsAt >= kResidentSeconds && store.NearDoorOut(pick.actor->GetPosition()))
				a.fallSeed = a.seedIndoors = true;
		}
		if (store.GiveWay(frame))
			store.recordsDirty = true;
	}
	// Round 8 (sections 2.7, 2.8): a coat stays while its actor is loaded, and
	// through a load door: kGoneSeconds for its 3D to come back, a teammate's as
	// long as it lives. A bare one out of the tier frees its slots.
	for (auto it = store.actors.begin(); it != store.actors.end();) {
		auto& a = it->second;
		bool keep = a.player;
		if (!keep) {
			if (const RE::NiPointer<RE::Actor> actor = a.handle.get()) {
				a.teammate = actor->IsPlayerTeammate();
				if (actor->Is3DLoaded())
					a.loadedAt = store.realClock;
				keep = (a.teammate || store.realClock - a.loadedAt <= kGoneSeconds) && (frame - a.lastPicked <= kKeepFrames || !Store::Bare(a));
			}
		}
		if (keep) {
			++it;
		} else {
			store.FreeActor(a);
			it = store.actors.erase(it);
			store.recordsDirty = true;
		}
	}
	// S4: what the writer and the fade hold for an actor, started over. S5: an
	// emptied coat in a storm is seeded again (a loaded save, a race change).
	auto restart = [snowing](Store::Actor& a_actor) {
		a_actor.sinceWrite = 0.0f;
		a_actor.joltHeld = 0.0f;
		a_actor.hasLastPos = false;
		a_actor.meltOwed[0] = a_actor.meltOwed[1] = 0.0f;
		a_actor.keep[0] = a_actor.keep[1] = 1.0f;
		a_actor.trackTime = -1.0;
		a_actor.knocked = a_actor.rushing = false;
		a_actor.fireHitSeen = -1.0f;
		a_actor.worn.clear();
		a_actor.wornKnown = a_actor.wornChanging = false;
		a_actor.fading = {};
		a_actor.hasSpeedPos = a_actor.wasSettled = false;
		a_actor.sinceFall = a_actor.fallDose = 0.0f;
		a_actor.fallSeed = snowing;
		a_actor.lastShout = 0;
		a_actor.fallAt = a_actor.shoutAt = -1.0;
	};
	if (reloaded) {
		store.RefillAll();
		for (auto& entry : store.actors) {
			restart(entry.second);
			entry.second.changeLine.clear();
		}
		store.reloadAt = store.realClock;
	}

	// Walks: a new actor, a new 3D or race, a draw that found no record, an
	// equipment change, and a slow refresh for what changes silently; a few a
	// frame.
	uint32_t walks = 0;
	for (auto& [formID, a] : store.actors) {
		RE::NiPointer<RE::Actor> actor = a.player ? RE::NiPointer<RE::Actor>(player) : a.handle.get();
		if (!actor || !actor->Is3DLoaded())
			continue;
		if (const bool dead = actor->IsDead(); dead != a.dead) {
			a.dead = dead;
			a.hasSpeedPos = false;
			store.recordsDirty = true;
			// S5: a corpse's skin, brows and scars join its coat; brought back, they leave it.
			a.walk = true;
		}
		// S4 round 2: its ground speed for the shake-off, off the snow too; a
		// corpse's at its bound's centre, which follows the ragdoll (round 3).
		const RE::NiAVObject* body = a.dead ? actor->Get3D(false) : nullptr;
		if (const RE::NiPoint3 at = body ? body->worldBound.center : actor->GetPosition(); a.hasSpeedPos && realDelta > 0.0f) {
			const float step = std::hypot(at.x - a.speedPos.x, at.y - a.speedPos.y);
			a.speed = step > kFootTeleportSpeed * realDelta ? 0.0f : a.speed + (step / realDelta - a.speed) * (1.0f - std::exp(-realDelta / kImpactSpeedSmoothing));
			a.speedPos = at;
		} else {
			a.speedPos = at;
			a.hasSpeedPos = true;
		}
		const RE::NiAVObject* root = actor->Get3D(false);
		const RE::NiAVObject* first = a.player ? actor->Get3D(true) : nullptr;
		const RE::TESRace* race = actor->GetRace();
		if (race != a.race) {
			// A new skeleton (werewolf, Vampire Lord, RaceMenu): the frames reset (section 2.8).
			if (a.race) {
				const char* name = race ? race->GetName() : nullptr;
				a.changeLine = std::format("reset by a race change to '{}'", name ? name : "");
				a.changeAt = store.realClock;
			}
			store.FreeActor(a);
			a.body = {};
			a.parts.clear();
			a.shapes.clear();
			a.race = race;
			a.walk = true;
			restart(a);
			store.recordsDirty = true;
		}
		// S4: an equipment change walks the actor at once and opens the window in
		// which its walks compare what it wears.
		if (const uint64_t worn = Store::WornSignature(actor.get()); worn != a.wornSignature) {
			if (a.wornSignature != 0) {
				a.walk = true;
				a.wornChanging = true;
				a.wornChangeFrame = frame;
			}
			a.wornSignature = worn;
		}
		if (a.wornChanging && frame - a.wornChangeFrame >= kWornWindowFrames)
			a.wornChanging = false;
		// Round 8: one waiting for a slot has no records, so its draws miss.
		if (root != a.root3D || first != a.first3D || (frame + formID) % kRewalkFrames == 0 ||
			(store.missed.contains(formID) && !a.refused && !a.waiting && frame - a.lastWalked >= kMissFrames))
			a.walk = true;
		if (!a.walk || walks >= kWalksPerFrame || !root)
			continue;
		walks++;
		a.walk = false;
		a.lastWalked = frame;
		// Round 8: a new 3D may stand past a load door: no heading across it.
		if (root != a.root3D)
			a.hasLastPos = false;
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
		a.waiting = store.Slot(a.body, walked, 0, formID);
		for (size_t p = 0; p < a.parts.size() && !a.waiting; ++p)
			store.Slot(a.parts[p], walked, int(p) + 1, formID);
		a.shapes.clear();
		a.skipped = std::move(walked.skipped);
		std::vector<Store::Worn> worn;
		for (const auto& s : walked.skins) {
			// Living skin takes no coat (Josef, S2 round 2; D4 melts it, S7): facegen
			// heads and tinted body skin, beast races' fur included. It still seeds and
			// chains the frames, so gloves and boots map through the body's bones. A
			// corpse's skin takes the coat with the rest of it (S5).
			const bool bareSkin = !a.dead && (s.feature == int(RE::BSShaderMaterial::Feature::kFaceGen) || s.feature == int(RE::BSShaderMaterial::Feature::kFaceGenRGBTint));
			if (s.frame >= 0 && !bareSkin) {
				a.shapes.push_back({ s.geometry, s.skin, uint32_t(s.frame), s.map.toAnchor, s.feature == int(RE::BSShaderMaterial::Feature::kHairTint) });
				if (!s.firstPerson) {
					const Store::Frame& f = s.frame == 0 ? a.body : a.parts[s.frame - 1];
					worn.push_back({ NameOf(s.geometry), s.slot, s.item, s.frame, f.slot, f.half, Store::SpheresOf(s) });
				}
			} else {
				a.skipped.push_back(s.geometry);
			}
		}
		// S4 (section 2.8, D3): a piece taken off or put on clears its region, the
		// new piece starting clean. Outside an equipment change's window a walk
		// only adds what it finds, as a 3D still loading can show a piece missing.
		auto among = [](const std::vector<Store::Worn>& a_list, const Store::Worn& a_shape) {
			return std::ranges::any_of(a_list, [&](const Store::Worn& a_other) { return a_other.Same(a_shape); });
		};
		if (!a.wornKnown || !a.wornChanging) {
			for (auto& w : worn)
				if (!among(a.worn, w))
					a.worn.push_back(std::move(w));
			a.wornKnown = true;
		} else {
			auto frameWith = [&](int a_slot, int a_half) -> const Store::Frame* {
				if (a.body.slot == a_slot && a_half < 0)
					return &a.body;
				for (const auto& part : a.parts)
					if (part.slot == a_slot && part.half == a_half)
						return &part;
				return nullptr;
			};
			std::string off, on;
			uint32_t voxels = 0;
			auto list = [](std::string& a_text, const std::string& a_name) { a_text += std::format("{}'{}'", a_text.empty() ? "" : ", ", a_name); };
			for (const auto& w : a.worn)
				if (!among(worn, w)) {
					if (const Store::Frame* f = frameWith(w.coatSlot, w.coatHalf))
						voxels += store.ClearSpheres(*f, w.spheres);
					list(off, w.name);
				}
			for (const auto& w : worn)
				if (!among(a.worn, w)) {
					if (const Store::Frame* f = frameWith(w.coatSlot, w.coatHalf))
						voxels += store.ClearSpheres(*f, w.spheres);
					list(on, w.name);
				}
			if (!off.empty() || !on.empty()) {
				a.changeLine = std::format("equipment changed: off {}, on {}; {} voxels cleared", off.empty() ? "nothing" : off, on.empty() ? "nothing" : on, voxels);
				a.changeAt = store.realClock;
			}
			a.worn = std::move(worn);
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
	// time since then, the cap reached in Build-up Time at full adhesion. And its
	// heading since then (round 3): Impact Share of the cap is kept for the parts
	// pushing into fresh snow, counted in full once it has moved a deformation
	// texel, the taps' reach. That move weighs its impacts too, the lumps' only
	// source (D30). Force Coat owns the atlas while it is on.
	store.due.clear();
	if (contact && force == 0 && store.live && store.atlasUAV) {
		const float dt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 0.0f) : 0.0f;
		const float cap = std::clamp(debugBodyCoatContactCap, 0.0f, 1.0f) * kCoatContactFront * 255.0f;
		store.capLevel = uint32_t(cap + 0.5f);
		const float perSecond = cap / std::max(debugBodyCoatBuildUp, 0.1f);
		const float texel = deformationTextures[0] && deformationTextures[0]->srv ? deformWorldSize / float(deformationTextures[0]->desc.Width) : 0.0f;
		const float impactShare = std::clamp(debugBodyCoatImpactShare, 0.0f, 1.0f);
		const auto make = [&](uint32_t a_formID, Store::Actor& a_actor, const RE::NiPoint3& a_pos) {
			// A0 per actor (S4): the snow's stickiness at its own height. A corpse
			// takes it at once (S4 round 2): its weight lays it in the snow, and it is
			// drawn only while it falls and settles.
			const float adhesion = std::min(1.0f, SnowConditionAtHeight(a_pos.z).stickiness + a_actor.joltHeld);
			float4 write{ a_actor.dead ? cap : adhesion * perSecond * a_actor.sinceWrite, 0.0f, 0.0f, 0.0f };
			const float dx = a_pos.x - a_actor.lastPos.x;
			const float dy = a_pos.y - a_actor.lastPos.y;
			const float moved = std::sqrt(dx * dx + dy * dy);
			float moving = 0.0f;
			if (a_actor.hasLastPos && texel > 0.0f && moved > 1e-3f) {
				moving = std::min(moved / texel, 1.0f);
				write = float4(write.x, dx / moved, dy / moved, impactShare * moving);
			}
			store.due.push_back({ a_formID, write, {}, {}, {}, true });
			store.due.back().moving = moving;
			a_actor.sinceWrite = 0.0f;
			a_actor.joltHeld = 0.0f;
			a_actor.lastPos = a_pos;
			a_actor.hasLastPos = true;
		};
		std::vector<uint32_t> others;
		for (auto& [formID, a] : store.actors) {
			if (a.refused || a.shapes.empty() || a.body.slot < 0)
				continue;
			a.sinceWrite = std::min(a.sinceWrite + dt, kWriteGapMax);
			if (const auto track = impactTracks.find(formID); track != impactTracks.end())
				a.joltHeld = std::max(a.joltHeld, track->second.Jolt());
			// Round 8: contact only for those the capture draws.
			if (!a.player && a.nearTier)
				others.push_back(formID);
			else if (frame % kWritePlayerFrames == 0 && player && player->Is3DLoaded())
				make(formID, a, player->GetPosition());
		}
		std::sort(others.begin(), others.end());
		const uint32_t count = uint32_t(others.size());
		uint32_t taken = 0;
		for (uint32_t i = 0; i < count && store.due.size() < kWritesPerFrame; ++i) {
			const uint32_t formID = others[(store.roundRobin + i) % count];
			taken = i + 1;
			auto& a = store.actors[formID];
			if (auto actor = a.handle.get(); actor && actor->Is3DLoaded())
				make(formID, a, actor->GetPosition());
		}
		store.roundRobin = count ? (store.roundRobin + taken) % count : 0;

		// S5 (section 2.4): snowfall while it snows here, each actor about every
		// kFallGap (a settled corpse every kFallGapSettled), the player first, with
		// the fill owed since: intensity x game time over the fill time, the ground
		// refill's own (70 s at Refill Rate x1). An actor first seen outdoors in a
		// storm is seeded with what the time it has snowed would have laid (the
		// ground's accumulation, Accumulation Time when full), so people met in a
		// storm are already snowy and at a storm's start as bare as the ground.
		// Round 2 (Josef): any snowfall fills a coat in time; the weather sets the
		// pace only.
		store.fillTime = settings.RefillRateMultiplier > 0.0f ? kBaseRefillTime * kCoatFillShare / settings.RefillRateMultiplier : 0.0f;
		if (debugBodyCoatFall) {
			const float dose = store.fillTime > 0.0f ? store.fallIntensity * coatSeconds / store.fillTime : 0.0f;
			const float accum = settings.EnableSnowAccumulation ? std::clamp(snowAccumulation.load(std::memory_order_relaxed), 0.0f, 1.0f) : 1.0f;
			auto seedOf = [&](float a_intensity) {
				return store.fillTime > 0.0f ? a_intensity * accum * std::max(settings.AccumulationHours, 0.0f) * 3600.0f / store.timescale / store.fillTime : 0.0f;
			};
			const float seedDose = seedOf(store.fallIntensity);
			// Round 8: one come in from a storm, seeded with the storm outside.
			const float seedIndoors = seedOf(std::clamp(accumWeatherIntensity.load(std::memory_order_relaxed), 0.0f, 1.0f));
			std::vector<uint32_t> waiting;
			uint32_t playerID = 0;
			for (auto& [formID, a] : store.actors) {
				if (a.refused || a.shapes.empty() || a.body.slot < 0)
					continue;
				a.sinceFall += realDelta;
				a.fallDose += dose;
				a.seedIndoors = a.seedIndoors && a.fallSeed && !snowing;
				if ((!snowing && !a.seedIndoors) || a.sinceFall < (a.dead && a.wasSettled ? kFallGapSettled : kFallGap))
					continue;
				if (a.player)
					playerID = formID;
				else
					waiting.push_back(formID);
			}
			std::sort(waiting.begin(), waiting.end());
			uint32_t falls = 0;
			auto fall = [&](uint32_t a_formID) {
				auto& a = store.actors[a_formID];
				RE::NiPointer<RE::Actor> actor = a.player ? RE::NiPointer<RE::Actor>(player) : a.handle.get();
				const RE::NiAVObject* root = actor && actor->Is3DLoaded() ? actor->Get3D(false) : nullptr;
				if (!root || root->worldBound.radius <= 0.0f)
					return false;
				auto it = std::ranges::find_if(store.due, [&](const Store::Due& a_due) { return a_due.formID == a_formID; });
				if (it == store.due.end()) {
					store.due.push_back({ a_formID });
					it = store.due.end() - 1;
				}
				// Paid once a pass draws it (BodyCoatWriteResolve).
				const float seed = a.fallSeed ? (a.seedIndoors ? seedIndoors : seedDose) : 0.0f;
				it->fall = float4(a.fallDose + seed, seed, 1.0f, 0.0f);
				it->seed = a.fallSeed;
				it->openSky = a.seedIndoors;
				const auto& b = root->worldBound;
				it->top = float4(b.center.x, b.center.y, b.radius, 0.0f);
				it->topZ = b.center.z;
				a.sinceFall = 0.0f;
				return true;
			};
			if (playerID && fall(playerID))
				falls++;
			const uint32_t total = uint32_t(waiting.size());
			uint32_t passed = 0;
			for (uint32_t i = 0; i < total && falls < kFallsPerFrame; ++i) {
				passed = i + 1;
				if (fall(waiting[(store.fallRobin + i) % total]))
					falls++;
			}
			store.fallRobin = total ? (store.fallRobin + passed) % total : 0;
		}
	}

	// S4 (section 2.5): every coat fades on game time. Per actor, A0 at its own
	// place: the clear-weather clock (a full coat goes in Melt Time, faster by
	// the ground's ageing weights, not at all while it snows), heat and fire
	// spells at their rate over the reference layer, and on the living, loose
	// snow shaken off by motion and jolts. Melt is owed in whole levels and the
	// shake-off as a share of what is there; the slots that owe a level resolve
	// with no point pass, about twice a second, at once after a fire.
	store.sinceFade += realDelta;
	if (store.realClock - store.fadeWindowStart >= 1.0) {
		store.fadeResolvesLast = std::exchange(store.fadeResolves, 0u);
		store.fallPassesLast = std::exchange(store.fallPasses, 0u);
		store.fadeWindowStart = store.realClock;
	}
	if (!contact || force != 0 || !debugBodyCoatFade || !store.writerReady || reloaded)
		return;
	const float meltHours = std::max(settings.AccumulationMeltHours, 0.01f);
	const float meltRate = std::max(settings.SpellMeltRate, 0.0f);
	const float shakeTime = std::max(debugBodyCoatShakeTime, 0.1f);
	const bool current = frame - stampGatherFrame <= 1u;
	// S5, D12: a shout cone's ground mark at a point, shaped as
	// DeformationUpdateCS.hlsl's ShoutMark shapes it, without its churn: the
	// share of its depth there, 0 outside what its front has reached.
	const float waist = std::clamp(settings.ShoutMarkWaist, 0.05f, 0.95f);
	const float roundness = std::max(settings.ShoutMarkRoundness, 1.0f);
	const float fadeStart = std::clamp(settings.ShoutMarkFadeStart, 0.0f, 0.98f);
	const float farDepth = std::clamp(settings.ShoutMarkFarDepth, 0.0f, 1.0f);
	auto markShare = [&](const SpellEmitter& a_cone, float a_x, float a_y) {
		const float sx = a_cone.position.x - a_cone.previous.x;
		const float sy = a_cone.position.y - a_cone.previous.y;
		const float axisLen = std::sqrt(sx * sx + sy * sy);
		if (axisLen < 1e-3f)
			return 0.0f;
		const float rx = a_x - a_cone.previous.x;
		const float ry = a_y - a_cone.previous.y;
		const float along = (rx * sx + ry * sy) / axisLen;
		const float perp = (rx * sy - ry * sx) / axisLen;
		const float u = along / std::max(a_cone.coneLength, axisLen);
		if (along < 0.0f || along > axisLen || u > 1.0f)
			return 0.0f;
		// Two half-superellipses meeting at the waist.
		const float t = std::clamp(u < waist ? (waist - u) / waist : (u - waist) / (1.0f - waist), 0.0f, 1.0f);
		const float halfWidth = std::max(a_cone.radius * std::pow(std::max(1.0f - std::pow(t, roundness), 0.0f), 1.0f / roundness), 1e-3f);
		if (std::abs(perp) >= halfWidth)
			return 0.0f;
		const float k = std::clamp((u - fadeStart) / (1.0f - fadeStart), 0.0f, 1.0f);
		return std::clamp(a_cone.strength, 0.0f, 1.0f) * std::lerp(1.0f, farDepth, k * k * (3.0f - 2.0f * k));
	};
	float most = 0.0f;
	for (auto& [formID, a] : store.actors) {
		// Read for every actor, so a coat that forms later sees only what is new.
		// A fire hit is new while the spell system's memory of it has not run
		// down since the last read.
		bool burning = false;
		if (current) {
			float hit = -1.0f;
			if (const auto it = lastElementalHit.find(formID); it != lastElementalHit.end() && it->second.element == SpellElement::Fire)
				hit = it->second.remaining;
			burning = hit >= 0.0f && (a.fireHitSeen < 0.0f || hit >= a.fireHitSeen - 1.0e-3f);
			a.fireHitSeen = hit;
		}
		// E0's jolts off the impact track: a landing, and the start of a knockdown or a rush.
		float jolt = 0.0f;
		if (current)
			if (const auto it = impactTracks.find(formID); it != impactTracks.end() && it->second.time != a.trackTime) {
				const auto& track = it->second;
				a.trackTime = track.time;
				if (track.landingEnergy > 0.0f)
					jolt = track.landingEnergy;
				if (track.knockEnergy > 0.0f && !a.knocked)
					jolt = std::max(jolt, track.knockEnergy);
				if (track.speedEnergy >= kRushEnergy && !a.rushing)
					jolt = std::max(jolt, track.speedEnergy);
				a.knocked = track.knockEnergy > 0.0f;
				a.rushing = track.speedEnergy >= kRushEnergy || (a.rushing && track.speedEnergy >= kRushRearm);
			}
		// Round 3: a corpse moved off its rest (the capture's settle latch woken)
		// is jolted as a body knocked into the snow is.
		if (current && a.dead)
			if (const auto rest = corpseRestStates.find(formID); rest != corpseRestStates.end()) {
				if (a.wasSettled && !rest->second.settled)
					jolt = std::max(jolt, kImpactKnockedDown);
				a.wasSettled = rest->second.settled;
			}
		float bound[2] = {};
		auto widen = [&](const Store::Frame& a_frame) {
			if (a_frame.slot >= 0)
				for (int c = 0; c < 2; ++c)
					bound[c] = std::max(bound[c], a_frame.bound[c]);
		};
		widen(a.body);
		for (const auto& part : a.parts)
			widen(part);
		if (bound[0] <= 0.0f && bound[1] <= 0.0f) {
			a.meltOwed[0] = a.meltOwed[1] = 0.0f;
			a.keep[0] = a.keep[1] = 1.0f;
			a.fading.active = false;
			continue;
		}
		RE::NiPointer<RE::Actor> actor = a.player ? RE::NiPointer<RE::Actor>(player) : a.handle.get();
		if (!actor)
			continue;
		auto& fading = a.fading;
		fading.active = true;
		fading.aging = ComputeCoatAging(actor.get());
		const auto& aging = fading.aging;
		fading.burning = burning;
		// Round 2 (Josef: gradual, by the fire's intensity). A heat source warms the
		// air on the melt clock, its core as warm as +3 C and in any weather. A
		// fire spell melts a coat at the pace it melts the snow under it, a layer
		// for a layer. A corpse lying settled in the snow is still in it: the
		// weather does not take its coat (E0's body-drop test against the snow).
		fading.inSnow = false;
		if (a.dead)
			if (const auto rest = corpseRestStates.find(formID); rest != corpseRestStates.end() && rest->second.settled)
				if (const auto* root = actor->Get3D(false)) {
					const auto& b = root->worldBound;
					float surface = 0.0f;
					fading.inSnow = SampleShellSurface(b.center.x, b.center.y, surface) && b.center.z - b.radius < surface;
				}
		const float clock = (fading.inSnow ? 0.0f : aging.clearClock) + aging.heatClock;
		const float fire = aging.fire + (burning ? meltRate : 0.0f);
		fading.melt = clock * store.timescale / (meltHours * 3600.0f) + fire;
		const float coats = clock * coatHours / meltHours + fire * coatSeconds;
		for (int c = 0; c < 2; ++c)
			a.meltOwed[c] += coats * kCoatFullLevels[c];
		// The shake-off, a moved corpse's too (round 3; a still one has no motion).
		// S5: snowfall is loose snow, shaken off alike, and a jolt sheds its full
		// loose share (contact snow is packed and holds half).
		const float loose = 1.0f - aging.stickiness;
		fading.motion = std::clamp(a.speed / kImpactSpeedFloor, 0.0f, 1.0f);
		fading.shake = loose * fading.motion / shakeTime;
		a.keep[0] *= std::exp(-fading.shake * coatSeconds);
		a.keep[1] *= std::exp(-fading.shake * coatSeconds);
		if (jolt > 0.0f) {
			fading.jolt = kCoatPackedShed * loose * std::min(jolt, 1.0f);
			fading.joltAt = store.realClock;
			a.keep[0] *= 1.0f - fading.jolt;
			a.keep[1] *= 1.0f - loose * std::min(jolt, 1.0f);
		}
		// D12: a shove's cone (Unrelenting Force) blows a body's snowfall off as
		// its front reaches it, by the share of its ground mark's depth there, once
		// a cone.
		if (current && settings.EnableShoutCones && bound[1] > 0.0f)
			for (const auto& cone : spellEmitters) {
				if (!cone.cone || cone.mark != SpellMark::Carve)
					continue;
				const uint64_t key = (uint64_t(uint32_t(int32_t(std::lround(cone.previous.x)))) << 32 | uint32_t(int32_t(std::lround(cone.previous.y)))) ^
				                     (uint64_t(std::lround(cone.coneSeed * 100.0f)) << 48);
				if (key == a.lastShout)
					continue;
				if (const float share = markShare(cone, a.speedPos.x, a.speedPos.y); share > 0.0f) {
					a.lastShout = key;
					a.keep[1] *= 1.0f - share;
					a.shoutShare = share;
					a.shoutAt = store.realClock;
				}
			}
		for (int c = 0; c < 2; ++c)
			if (bound[c] > 0.0f)
				most = std::max({ most, std::floor(a.meltOwed[c]), (1.0f - a.keep[c]) * bound[c] });
	}
	if (most < 1.0f || (store.sinceFade < kFadeGap && most < kFadeSoon))
		return;
	// Each half's word; what its voxels can hold after (the share rounds down at
	// most, then the whole levels); the owed fade paid.
	BodyCoatResolveCB cb{};
	uint32_t slots = 0;
	for (auto& [formID, a] : store.actors) {
		uint32_t melt[2], share[2];
		for (int c = 0; c < 2; ++c) {
			melt[c] = uint32_t(std::clamp(std::floor(a.meltOwed[c]), 0.0f, 255.0f));
			share[c] = uint32_t(std::clamp(std::floor((1.0f - a.keep[c]) * 255.0f), 0.0f, 255.0f));
		}
		const uint32_t word = melt[0] | (melt[1] << 8) | (share[0] << 16) | (share[1] << 24);
		if (!word)
			continue;
		bool any = false;
		auto fade = [&](Store::Frame& a_frame) {
			if (a_frame.slot < 0 || (a_frame.bound[0] <= 0.0f && a_frame.bound[1] <= 0.0f))
				return;
			any = true;
			const uint32_t first = uint32_t(a_frame.slot) * 2u;
			if (a_frame.half < 0)
				cb.Fade[first] = cb.Fade[first + 1] = word;
			else
				cb.Fade[first + uint32_t(a_frame.half)] = word;
			slots |= 1u << a_frame.slot;
			for (int c = 0; c < 2; ++c)
				a_frame.bound[c] = std::max(0.0f, a_frame.bound[c] - std::floor(a_frame.bound[c] * float(share[c]) / 255.0f) - float(melt[c]));
		};
		fade(a.body);
		for (auto& part : a.parts)
			fade(part);
		if (!any)
			continue;
		for (int c = 0; c < 2; ++c) {
			a.meltOwed[c] = melt[c] >= 255u ? 0.0f : a.meltOwed[c] - float(melt[c]);
			a.keep[c] = share[c] >= 255u ? 1.0f : std::min(a.keep[c] / (1.0f - float(share[c]) / 255.0f), 1.0f);
		}
		a.fading.resolves++;
	}
	for (uint32_t k = 0; k < kBodyCoatSlots; ++k)
		if (slots & (1u << k))
			cb.Slots[cb.Count++] = k;
	if (cb.Count) {
		cb.Seed = frame ^ 0x9E3779B9u;
		store.FadeResolve(context, cb);
		store.fadeResolves++;
	}
	store.sinceFade = 0.0f;
}

void SnowDeformation::PrimeBodyCoatWriter()
{
#if !SNOW_ALPHA_BUILD
	const char* variants[6][5] = { { "vs_5_0", "VSHADER", "SKINNED", "COAT", nullptr }, { "gs_5_0", "GSHADER", "COAT", nullptr, nullptr },
		{ "ps_5_0", "PSHADER", "COAT", nullptr, nullptr }, { "cs_5_0", "CSHADER", "COAT", nullptr, nullptr }, { "cs_5_0", "CSHADER", "COAT", "COAT_TILE", nullptr },
		{ "vs_5_0", "VSHADER", "SKINNED", "COAT", "COAT_TOP" } };
	for (const auto& v : variants)
		if (auto* blob = SD_CompileShaderBlob(kWritePath, v[0], v[1], v[2], v[3], v[4]))
			blob->Release();
#endif
}

bool SnowDeformation::BodyCoatWriteBegin(bool a_coatOnly)
{
	if (!bodyCoatStore || bodyCoatStore->due.empty() || !contactHeight || !contactSkinVS || !contactPS)
		return false;
	auto& store = *bodyCoatStore;
	if (!store.EnsureWriter())
		return false;
	// Contact reads the snow as the shell draws it, placed for this frame
	// (LandscapeSnowRefresh's recipe); snowfall needs none of it.
	const bool shell = lastShellCBData && shellTerrainTexture && shellTerrainTexture->srv && GetDeformationSRV() && heightBottomFiltered &&
	                   heightTopRaw[heightCurrent] && heightSkinDepth && ShellFootprintHasSnow();
	if (shell) {
		auto& fb = globals::game::frameBufferCached;
		ShellCB cb = *lastShellCBData;
		cb.CameraPosAdjust = fb.GetCameraPosAdjust();
		cb.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();
		PlaceShellFields(cb);
		store.shellCB->Update(cb);
		LandscapeSurfaceSRVs(store.shellSRVs);
		store.deformTexel = deformWorldSize / float(deformationTextures[0]->desc.Width);
	}
	store.waterSRV = waterFineTexture && waterFineTexture->srv ? waterFineTexture->srv.get() : nullptr;
	// S5: the eye Skylighting placed its probes about this frame, and its probes.
	const RE::NiPoint3 eyePosition = Util::GetEyePosition();
	store.eye[0] = eyePosition.x;
	store.eye[1] = eyePosition.y;
	store.eye[2] = eyePosition.z;
	const auto& sky = globals::features::skylighting;
	store.skySRV = sky.loaded && sky.texProbeArray && sky.texProbeArray->srv ? sky.texProbeArray->srv.get() : nullptr;
	// Each contact-due actor's snow-top tile (round 4): that surface sampled once
	// over the capture's bound of the actor, at the deformation texel (coarser
	// for a body wider than the tile), two texels beyond for the taps and their
	// filter. Each fall-due actor's row of tops (S5); one the capture does not
	// draw (on a road, a rock or a horse, or a settled corpse) is walked for its
	// coat alone, as is everyone while the capture sleeps (round 8).
	BodyCoatTileCB tiles{};
	uint32_t tops = 0;
	bool any = false;
	store.coatOnly.clear();
	for (auto& d : store.due) {
		d.tile = {};
		const ContactProp* drawn = nullptr;
		if (!a_coatOnly)
			for (const auto& c : contactActors)
				if (const auto ref = c.ref.get(); ref && ref->GetFormID() == d.formID) {
					drawn = &c;
					break;
				}
		if (d.contact && shell && drawn && tiles.Count < kCoatTiles) {
			const float margin = 2.0f * store.deformTexel;
			const float extent = std::max(drawn->maxX - drawn->minX, drawn->maxY - drawn->minY) + 2.0f * margin;
			d.tile = float4(drawn->minX - margin, drawn->minY - margin, std::max(store.deformTexel, extent / float(kCoatTileDim - 1)), float(tiles.Count * kCoatTileDim));
			tiles.Origin[tiles.Count++] = d.tile;
		}
		if (d.fall.z > 0.0f && tops < kCoatTiles) {
			d.top.w = float(tops++ * kCoatTileDim);
			if (!drawn)
				if (const auto it = store.actors.find(d.formID); it != store.actors.end()) {
					RE::NiPointer<RE::Actor> actor = it->second.player ? RE::NiPointer<RE::Actor>(globals::game::player) : it->second.handle.get();
					if (actor)
						store.coatOnly.push_back({ actor->CreateRefHandle(), d.top.x - d.top.z, d.top.y - d.top.z, d.top.x + d.top.z, d.top.y + d.top.z, it->second.dead, 0.0f, 0.0f });
				}
		} else {
			d.fall = {};
		}
		any = any || d.tile.z > 0.0f || d.fall.z > 0.0f;
	}
	if (!any)
		return false;
	auto* context = globals::d3d::context;
	if (tops > 0) {
		const float lowest[4] = { -1.0e30f, -1.0e30f, -1.0e30f, -1.0e30f };
		context->ClearRenderTargetView(store.topRTV.get(), lowest);
	}
	if (tiles.Count == 0)
		return true;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(store.tileCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	memcpy(mapped.pData, &tiles, sizeof(tiles));
	context->Unmap(store.tileCB.get(), 0);
	globals::profiler->EndPass();
	globals::profiler->BeginPass("SnowDeformation::Bodies - Coat Update");
	winrt::com_ptr<ID3D11ComputeShader> savedCS;
	context->CSGetShader(savedCS.put(), nullptr, nullptr);
	ID3D11Buffer* savedCBs[2] = {};
	context->CSGetConstantBuffers(0, 2, savedCBs);
	context->CSSetShader(store.tileCS.get(), nullptr, 0);
	ID3D11Buffer* cbs[2] = { store.shellCB->CB(), store.tileCB.get() };
	context->CSSetConstantBuffers(0, 2, cbs);
	context->CSSetShaderResources(0, kShellSurfaceSRVs, store.shellSRVs);
	ID3D11UnorderedAccessView* uav = store.tileUAV.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->Dispatch(kCoatTileDim / 8, kCoatTileDim / 8, tiles.Count);
	ID3D11UnorderedAccessView* noUAV = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &noUAV, nullptr);
	ID3D11ShaderResourceView* noSRV[kShellSurfaceSRVs] = {};
	context->CSSetShaderResources(0, kShellSurfaceSRVs, noSRV);
	context->CSSetShader(savedCS.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 2, savedCBs);
	for (auto* b : savedCBs)
		if (b)
			b->Release();
	globals::profiler->EndPass();
	globals::profiler->BeginPass("SnowDeformation::Trails - Gather & Contact");
	return true;
}

bool SnowDeformation::BodyCoatWriteDue(uint32_t a_formID, float4& a_write, float4& a_top) const
{
	if (bodyCoatStore)
		for (const auto& d : bodyCoatStore->due) {
			const bool contact = d.contact && d.tile.z > 0.0f;
			if (d.formID != a_formID || (!contact && d.fall.z <= 0.0f))
				continue;
			auto& store = *bodyCoatStore;
			a_write = contact ? d.write : float4{};
			a_top = d.fall.z > 0.0f ? d.top : float4{};
			store.currentDue = size_t(&d - store.due.data());
			store.currentTile = d.tile;
			store.currentContact = contact;
			store.currentFall = d.fall;
			store.currentTop = d.top;
			store.currentTopZ = d.topZ;
			store.currentMoving = contact ? d.moving : 0.0f;
			store.currentOpenSky = d.openSky;
			return true;
		}
	return false;
}

bool SnowDeformation::BodyCoatFallDue() const
{
	return bodyCoatStore && std::ranges::any_of(bodyCoatStore->due, [](const BodyCoatStore::Due& a_due) { return a_due.fall.z > 0.0f; });
}

const std::vector<SnowDeformation::ContactProp>& SnowDeformation::BodyCoatCoatOnly() const
{
	static const std::vector<ContactProp> none;
	return bodyCoatStore ? bodyCoatStore->coatOnly : none;
}

void SnowDeformation::BodyCoatTopBind(ID3D11DeviceContext* a_context, bool a_on)
{
	auto& store = *bodyCoatStore;
	// The actor's row of tops, its window over the whole row, the highest kept;
	// the tops' own VS reads its window and the snow's way from the write CB
	// (VS b3, put back after).
	ID3D11RenderTargetView* rtv = a_on ? store.topRTV.get() : contactHeight->rtv.get();
	a_context->OMSetRenderTargets(1, &rtv, nullptr);
	a_context->OMSetBlendState(a_on ? store.topBlend.get() : contactMinBlendBodies.get(), nullptr, 0xFFFFFFFF);
	const D3D11_VIEWPORT viewport = a_on ? D3D11_VIEWPORT{ 0.0f, store.currentTop.w, float(kCoatTileDim), float(kCoatTileDim), 0.0f, 1.0f } :
	                                       D3D11_VIEWPORT{ 0.0f, 0.0f, float(kContactDim), float(kContactDim), 0.0f, 1.0f };
	a_context->RSSetViewports(1, &viewport);
	if (a_on) {
		BodyCoatWriteCB cb{};
		cb.Top = store.currentTop;
		cb.TopZ = store.currentTopZ;
		for (int i = 0; i < 3; ++i)
			cb.From[i] = store.From()[i];
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_context->Map(store.writeCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, &cb, sizeof(cb));
			a_context->Unmap(store.writeCB.get(), 0);
		}
		a_context->VSGetConstantBuffers(3, 1, store.savedTopCB.put());
		ID3D11Buffer* write = store.writeCB.get();
		a_context->VSSetConstantBuffers(3, 1, &write);
		a_context->VSSetShader(store.topVS.get(), nullptr, 0);
	} else {
		ID3D11Buffer* saved = store.savedTopCB.get();
		a_context->VSSetConstantBuffers(3, 1, &saved);
		store.savedTopCB = nullptr;
		a_context->VSSetShader(contactSkinVS, nullptr, 0);
	}
}

bool SnowDeformation::BodyCoatWriteShape(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, const float4& a_write)
{
	auto& store = *bodyCoatStore;
	const auto it = store.recordOf.find(a_geometry);
	if (it == store.recordOf.end() || store.recordSkin[it->second] != a_geometry->GetGeometryRuntimeData().skinInstance.get())
		return false;
	const BodyCoatRecordCB& record = store.records[it->second];
	// A voxel's largest side in the frame's units: the self-cover's slack.
	float voxel = 0.0f;
	for (const auto& row : record.ModelToAtlas)
		if (const float length = std::sqrt(row.x * row.x + row.y * row.y + row.z * row.z); length > 0.0f)
			voxel = std::max(voxel, 1.0f / length);
	// Round 8: a seed laid indoors for one come in from a storm reads no roof.
	const float* from = store.From();
	BodyCoatWriteCB cb{ { record.ModelToAtlas[0], record.ModelToAtlas[1], record.ModelToAtlas[2] }, record.AtlasLo, record.AtlasHi, a_write.x, { a_write.y, a_write.z },
		a_write.w, store.currentTile, store.deformTexel, store.currentContact ? 1.0f : 0.0f, store.currentFall.z, store.skySRV && !store.currentOpenSky ? 1.0f : 0.0f,
		{ record.ModelToCanonical[0], record.ModelToCanonical[1], record.ModelToCanonical[2] }, store.currentTop, { store.eye[0], store.eye[1], store.eye[2] }, voxel,
		{ from[0], from[1], from[2] }, store.currentTopZ, store.currentMoving };
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(a_context->Map(store.writeCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	memcpy(mapped.pData, &cb, sizeof(cb));
	a_context->Unmap(store.writeCB.get(), 0);
	store.touched |= 1u << (record.Slot >> 1);
	store.touchedHalves |= 1ull << record.Slot;
	if (store.currentDue < store.due.size())
		store.due[store.currentDue].drawn = true;
	return true;
}

void SnowDeformation::BodyCoatWriteBind(ID3D11DeviceContext* a_context, bool a_on)
{
	auto& store = *bodyCoatStore;
	// The capture's target stays bound: the coat's points leave it as it is. The
	// coat VS reads its tile (t0), its tops (t1, S5), Skylighting's probes (t2),
	// the water (t63), its constants (b3), SharedData (b5: Grass Collision's slot
	// the rest of the frame) and FeatureData (b6); the GS its constants (b3,
	// round 2: the snow's way). What was bound there is put back after.
	ID3D11RenderTargetView* rtv = contactHeight->rtv.get();
	if (a_on) {
		a_context->VSGetConstantBuffers(3, 1, store.savedCB[0].put());
		a_context->VSGetConstantBuffers(5, 1, store.savedCB[1].put());
		a_context->VSGetConstantBuffers(6, 1, store.savedCB[2].put());
		a_context->GSGetConstantBuffers(3, 1, store.savedCB[3].put());
		for (UINT s = 0; s < 3; ++s)
			a_context->VSGetShaderResources(s, 1, store.savedSRV[s].put());
		a_context->VSGetShaderResources(kWaterWindowSlot, 1, store.savedSRV[3].put());
		a_context->GSGetShader(store.savedGS.put(), nullptr, nullptr);
		a_context->VSSetShader(store.writeVS.get(), nullptr, 0);
		a_context->GSSetShader(store.writeGS.get(), nullptr, 0);
		a_context->PSSetShader(store.writePS.get(), nullptr, 0);
		ID3D11Buffer* write = store.writeCB.get();
		a_context->VSSetConstantBuffers(3, 1, &write);
		a_context->GSSetConstantBuffers(3, 1, &write);
		ID3D11Buffer* shared[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		a_context->VSSetConstantBuffers(5, 2, shared);
		ID3D11ShaderResourceView* srvs[3] = { store.tileSRV.get(), store.topSRV.get(), store.skySRV };
		a_context->VSSetShaderResources(0, 3, srvs);
		a_context->VSSetShaderResources(kWaterWindowSlot, 1, &store.waterSRV);
		ID3D11UnorderedAccessView* uavs[2] = { store.contactUAV.get(), store.fallUAV.get() };
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 2, uavs, nullptr);
	} else {
		ID3D11UnorderedAccessView* noUAV[2] = {};
		a_context->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, nullptr, 1, 2, noUAV, nullptr);
		a_context->GSSetShader(store.savedGS.get(), nullptr, 0);
		a_context->PSSetShader(contactPS, nullptr, 0);
		a_context->VSSetShader(contactSkinVS, nullptr, 0);
		ID3D11ShaderResourceView* srvs[3] = { store.savedSRV[0].get(), store.savedSRV[1].get(), store.savedSRV[2].get() };
		a_context->VSSetShaderResources(0, 3, srvs);
		ID3D11ShaderResourceView* water = store.savedSRV[3].get();
		a_context->VSSetShaderResources(kWaterWindowSlot, 1, &water);
		ID3D11Buffer* write = store.savedCB[0].get();
		a_context->VSSetConstantBuffers(3, 1, &write);
		ID3D11Buffer* shared[2] = { store.savedCB[1].get(), store.savedCB[2].get() };
		a_context->VSSetConstantBuffers(5, 2, shared);
		ID3D11Buffer* geometry = store.savedCB[3].get();
		a_context->GSSetConstantBuffers(3, 1, &geometry);
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
	// S5: each fall-due actor's halves fill by its snowfall, and may then hold up
	// to what the fill brings at full exposure.
	std::vector<std::pair<BodyCoatStore::Frame*, float>> fallen;
	for (const auto& d : store.due) {
		if (d.fall.z <= 0.0f || !d.drawn)
			continue;
		const auto it = store.actors.find(d.formID);
		if (it == store.actors.end())
			continue;
		// What it owed is paid, the seed once.
		auto& a = it->second;
		a.fallDose = std::max(a.fallDose - (d.fall.x - d.fall.y), 0.0f);
		a.fallSeed = a.fallSeed && !d.seed;
		a.seedIndoors = a.seedIndoors && a.fallSeed;
		a.fallSeeded = d.fall.y;
		a.seededAtDoor = d.seed && d.openSky;
		a.fallAt = store.realClock;
		store.fallPasses++;
		auto fill = [&](BodyCoatStore::Frame& a_frame) {
			if (a_frame.slot < 0)
				return;
			for (int h = 0; h < 2; ++h)
				if (a_frame.half < 0 || a_frame.half == h)
					cb.Fall[a_frame.slot * 2 + h] = d.fall.x;
			fallen.emplace_back(&a_frame, d.fall.x);
		};
		fill(it->second.body);
		for (auto& part : it->second.parts)
			fill(part);
	}
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
		ID3D11UnorderedAccessView* uavs[4] = { store.atlasUAV.get(), store.contactUAV.get(), store.fallUAV.get(), store.raisedUAV.get() };
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
	// S4: a half the writer drew into may now hold up to the cap.
	const uint64_t halves = std::exchange(store.touchedHalves, 0ull);
	for (uint32_t half = 0; half < 2 * kBodyCoatSlots; ++half)
		if (halves & (1ull << half))
			if (auto* coat = store.FrameAt(half >> 1, half & 1u))
				coat->bound[0] = std::max(coat->bound[0], float(store.capLevel));
	for (auto& [coat, fall] : fallen)
		coat->bound[1] = std::min(255.0f - (255.0f - coat->bound[1]) * std::exp(-fall) + 1.0f, 255.0f);
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
	auto* state = globals::state;
	auto& permutation = state->permutationData;
	permutation.ExtraFeatureDescriptor &= ~uint32_t(State::ExtraFeatureDescriptors::SnowBodyCoat);
	BodyCoatStore* store = bodyCoatStore.get();
	// S5: the draw after a corpse's skin takes its technique's own pixel shader back.
	if (store && std::exchange(store->swappedPixelShader, false))
		if (auto* own = state->customPixelShader ? state->customPixelShader : *globals::game::currentPixelShader)
			globals::d3d::context->PSSetShader(reinterpret_cast<ID3D11PixelShader*>(own->shader), nullptr, 0);
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
	auto* context = globals::d3d::context;
	// S5 (skin on corpses): a model-space-normal skin's technique keys its plain pixel
	// shader, so living faces and bodies keep theirs; a corpse's skin draw takes the
	// coat's for itself alone, and the plain one draws while it compiles.
	using Flags = SIE::ShaderCache::LightingShaderFlags;
	if ((state->currentPixelDescriptor & uint32_t(Flags::ModelSpaceNormals)) != 0) {
		if ((store->records[it->second].Flags & kBodyCoatFlagLiving) != 0)
			return;
		auto* coat = globals::shaderCache->GetPixelShader(*state->currentShader, state->modifiedPixelDescriptor | uint32_t(Flags::SnowBodyCoat));
		if (!coat) {
			NoteBodyCoatShaders(state->currentVertexDescriptor, state->currentPixelDescriptor | uint32_t(Flags::SnowBodyCoat));
			return;
		}
		context->PSSetShader(reinterpret_cast<ID3D11PixelShader*>(coat->shader), nullptr, 0);
		store->swappedPixelShader = true;
	}
	permutation.ExtraFeatureDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowBodyCoat);
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
		if (store->lookFlags & (kBodyCoatFlagLumpDepth | kBodyCoatFlagLumpBlend)) {
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
	std::vector<Store::Pick> picks, living, dead;
	Store::PickTier(picks);
	for (auto& pick : picks)
		(pick.actor->IsDead() ? dead : living).push_back(std::move(pick));

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
	return std::format("Contact writer (S3): {}. Last update: {} actors, {} triangles tested, {} voxels raised (read a few frames late). Due this frame: {}. Lumps only from impacts, at {:.2f} of their share (stickiness {:.2f}; the rest sits as grains).",
		writer, store.writtenActorsLast, store.writtenTrianglesLast, store.raisedLast, store.due.size(), store.amounts[3], GetSnowCondition().stickiness);
}

std::vector<std::string> SnowDeformation::BodyCoatFadeLines() const
{
	std::vector<std::string> lines;
	if (!bodyCoatStore) {
		lines.push_back("Fading (S4): no store (Contact Coat is off).");
		return lines;
	}
	const auto& store = *bodyCoatStore;
	const float timescale = store.timescale;
	const float meltHours = std::max(settings.AccumulationMeltHours, 0.01f);
	const float fullMinutes = meltHours * 60.0f / timescale;
	const float capShare = std::clamp(debugBodyCoatContactCap, 0.0f, 1.0f);
	const char* state = !debugBodyCoatContact ? "off with Contact Coat" :
	                    store.force != 0      ? "off while Force Coat is on" :
	                    !debugBodyCoatFade    ? "off" :
	                    !store.writerReady    ? "waiting for the writer (an actor in snow)" :
	                                            "on";
	lines.push_back(std::format("Fading (S4): {}. Melt Time {:.0f} game hours at timescale {:.0f}: in clear, calm, cold weather a full coat goes in {:.1f} real minutes, one at Contact Cap in {:.1f}. Fade resolves in the last second: {}.",
		state, meltHours, timescale, fullMinutes, fullMinutes * capShare, store.fadeResolvesLast));
	auto ago = [&](double a_at) {
		const double s = store.realClock - a_at;
		return s >= 120.0 ? std::format("{:.1f} min", s / 60.0) : std::format("{:.0f} s", s);
	};
	if (store.lastJumpAt >= 0.0)
		lines.push_back(std::format("The calendar jumped {:.1f} game hours {} ago (a wait, a sleep or a fast travel): every coat faded by that time at once.", store.lastJumpHours, ago(store.lastJumpAt)));
	if (store.reloadAt >= 0.0)
		lines.push_back(std::format("A loaded save emptied every coat {} ago (S8 will keep them).", ago(store.reloadAt)));
	auto* player = globals::game::player;
	for (const auto& [formID, a] : store.actors) {
		if (!a.fading.active && a.changeLine.empty())
			continue;
		RE::NiPointer<RE::Actor> actor = a.player ? RE::NiPointer<RE::Actor>(player) : a.handle.get();
		const char* display = actor ? actor->GetDisplayFullName() : nullptr;
		std::string line = std::format("{}: ", a.player ? std::string("Player") : (display && *display ? std::string(display) : std::format("{:08X}", formID)));
		if (a.fading.active) {
			float bound = a.body.slot >= 0 ? a.body.bound[0] : 0.0f;
			for (const auto& part : a.parts)
				bound = part.slot >= 0 ? std::max(bound, part.bound[0]) : bound;
			const auto& g = a.fading.aging;
			std::string why = a.fading.inSnow ? std::string("lying settled in the snow, so the weather does not take it") :
			                  g.interior      ? (g.interiorSnow ? std::string("indoors, cold (snow drawn here): x1") :
			                                     g.interiorCold ? std::format("indoors, cold (by {}): x1", g.interiorCold) :
			                                                      std::string("indoors, warm: x7")) :
			                                    std::format("clear-weather clock x{:.2f} (snowing {:.2f}, rain {:.2f}, wind {:.2f}, warm {:.2f} at {:+.1f} C)", g.clearClock,
												 std::min(g.snowfall, 1.0f), g.rain, g.wind, g.warm, g.temperature);
			if (g.heat > 0.0f)
				why += std::format("; heat {:.2f}, {:.0f} units from a heat source (bowl {:.0f}), x{:.1f} more", g.heat, g.heatDistance, g.heatRadius, g.heatClock);
			if (g.fire > 0.0f)
				why += std::format("; fire spells {:.2f} coats a second", g.fire);
			if (a.fading.burning)
				why += "; struck by fire";
			const float coat = bound / kCoatFullLevels[0];
			const float bare = a.fading.melt > 0.0f ? coat / a.fading.melt : -1.0f;
			line += std::format("coat at most {:.2f} of a full one ({:.0f} % of Contact Cap). Melt: {}, so bare in {}", coat, 100.0f * coat / std::max(capShare, 1e-3f), why,
				bare < 0.0f ? std::string("never (no melt)") : (bare >= 120.0f ? std::format("{:.1f} min", bare / 60.0f) : std::format("{:.1f} s", bare)));
			line += std::format(". Shake-off {:.1f} % a second ({} {:.0f} units/s, {:.2f} of a run; stickiness {:.2f})", 100.0f * a.fading.shake,
				a.dead ? "a corpse moved at" : "moving", a.speed, a.fading.motion, g.stickiness);
			if (a.fading.joltAt >= 0.0)
				line += std::format("; the last jolt shed {:.0f} %, {} ago", 100.0f * a.fading.jolt, ago(a.fading.joltAt));
			line += std::format(". {} fade resolves.", a.fading.resolves);
		}
		if (!a.changeLine.empty())
			line += std::format(" Last: {}, {} ago.", a.changeLine, ago(a.changeAt));
		lines.push_back(std::move(line));
	}
	return lines;
}

std::vector<std::string> SnowDeformation::BodyCoatFallLines() const
{
	std::vector<std::string> lines;
	const auto& range = snowWeatherRange;
	if (!range.scanned)
		lines.push_back("The load order's snowfalls are not read yet.");
	else
		lines.push_back(std::format("The load order's {} snowfalls: the heaviest '{}' at intensity {:.2f}. Specks fill a coat up to {:.2f} of a full one ({}); past it they thicken to twice as many by a full one, and snowfall lays no lumps.",
			range.weathers, range.heaviestName, range.heaviest, range.split,
			range.splitName.empty() ? std::string("half: no step between heavy snowfalls") : std::format("'{}' over the heaviest", range.splitName)));
	if (!bodyCoatStore) {
		lines.push_back("Snowfall (S5): no store (Contact Coat is off).");
		return lines;
	}
	const auto& store = *bodyCoatStore;
	const char* state = !debugBodyCoatContact ? "off with Contact Coat" :
	                    store.force != 0      ? "off while Force Coat is on" :
	                    !debugBodyCoatFall    ? "off" :
	                    !store.writerReady    ? "waiting for the writer (it starts at the first coat update)" :
	                                            "on";
	const auto& sky = globals::features::skylighting;
	lines.push_back(std::format("Snowfall (S5): {}. Snowing here at {:.2f}. A coat fills {} at full exposure. {}. Snowfall updates in the last second: {}.",
		state, store.fallIntensity, store.fallIntensity > 0.0f && store.fillTime > 0.0f ? std::format("to 63 % in {:.0f} s", store.fillTime / store.fallIntensity) : std::string("not now"),
		sky.loaded ? "Shelter from Skylighting" : "Skylighting is off, so roofs give no shelter", store.fallPassesLast));
	lines.push_back(std::format("The game's falling snow slants {:.0f} degrees from straight down in this wind; the coat's comes {:.0f} degrees off vertical (Wind Tilt {:.2f}), from {:.0f} degrees of the compass.",
		store.gameTilt, store.fallTilt, std::clamp(debugBodyCoatWindTilt, 0.0f, 1.0f),
		DirectX::XMConvertToDegrees(std::atan2(store.fallFrom[0], store.fallFrom[1])) + (store.fallFrom[0] < 0.0f ? 360.0f : 0.0f)));
	auto ago = [&](double a_at) {
		const double s = store.realClock - a_at;
		return s >= 120.0 ? std::format("{:.1f} min", s / 60.0) : std::format("{:.1f} s", s);
	};
	auto* player = globals::game::player;
	for (const auto& [formID, a] : store.actors) {
		if (a.fallAt < 0.0 && a.shoutAt < 0.0)
			continue;
		RE::NiPointer<RE::Actor> actor = a.player ? RE::NiPointer<RE::Actor>(player) : a.handle.get();
		const char* display = actor ? actor->GetDisplayFullName() : nullptr;
		std::string line = std::format("{}: ", a.player ? std::string("Player") : (display && *display ? std::string(display) : std::format("{:08X}", formID)));
		float bound = a.body.slot >= 0 ? a.body.bound[1] : 0.0f;
		for (const auto& part : a.parts)
			bound = part.slot >= 0 ? std::max(bound, part.bound[1]) : bound;
		line += std::format("snowfall at most {:.2f} of a full coat", bound / 255.0f);
		if (a.fallAt >= 0.0)
			line += std::format("; last update {} ago", ago(a.fallAt));
		if (a.fallSeeded > 0.0f)
			line += std::format(", seeded with {:.1f} fill times of snowfall {}", a.fallSeeded, a.seededAtDoor ? "at the door, from the storm outside" : "at first sight");
		if (a.shoutAt >= 0.0)
			line += std::format("; a shout blew {:.0f} % off {} ago", 100.0f * a.shoutShare, ago(a.shoutAt));
		lines.push_back(std::move(line) + ".");
	}
	return lines;
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
		uint32_t teammates = 0, waiting = 0, nearTier = 0;
		for (const auto& entry : store.actors) {
			refused += entry.second.refused ? 1u : 0u;
			components += uint32_t(entry.second.parts.size());
			teammates += entry.second.teammate ? 1u : 0u;
			waiting += entry.second.waiting ? 1u : 0u;
			nearTier += entry.second.nearTier ? 1u : 0u;
		}
		const char* lumpLook = (store.lookFlags & kBodyCoatFlagLumpDepth) ? "in depth" : (store.lookFlags & kBodyCoatFlagLumpBlend) ? "blended" : "today's";
		lines.push_back(std::format("Body coat: {}; forced coat {}, lumps {:.2f} ({}), specks over {:.0f} % of a surface and hair's {:.2f} at {:.1f}-{:.1f} mm, {}, relief {}. {}", kViews[std::clamp(store.view, 0, 3)],
			kForces[std::clamp(store.force, 0, 4)], store.amounts[0], lumpLook, store.amounts[1] * kSpeckMax * 100.0f, store.amounts[2], store.look[1] * 2.0f * 14.2875f, store.look[2] * 2.0f * 14.2875f,
			(store.lookFlags & kBodyCoatFlagFlatPaint) ? "flat paint" : "crust",
			(store.lookFlags & kBodyCoatFlagReliefView) ? "shown" : ((store.lookFlags & kBodyCoatFlagRelief) ? "on" : "off"),
			store.gpuFailed ? "The atlas could not be made; see CommunityShaders.log." : ""));
		lines.push_back(std::format("Store: {} actors ({} refused as incorporeal), {} components; slots {} bodies + {} halves of {}; {} records ({}). Last frame: {} coated draws, {} draws found no record, {} walks.",
			store.actors.size(), refused, components, full, halves, kBodyCoatSlots, store.records.size(), store.context1 ? "bound by offset" : "written per draw",
			store.coatedLast, store.missesLast, store.walksLast));
		lines.push_back(std::format("Tier (round 8): {} teammates, {} the capture draws (contact reaches only them), {} waiting for a slot; {} gave a slot up to a nearer one this session.{}",
			teammates, nearTier, waiting, store.evictions,
			store.doorCell ? std::format(" Indoors, {} doors out; those first seen at one {:.0f} s or more after you came in, while it snowed outside, arrive snowy.", store.doorsOut.size(), kResidentSeconds) : std::string()));
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
