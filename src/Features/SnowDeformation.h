// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#pragma once

#include "Buffer.h"

#include <d3d11_1.h>
#include <deque>
#include <future>
#include <list>
#include <intrin.h>

// Statics.cpp: runtime shader compile returning the bytecode (input layouts
// need it), and the vertex position size a vertex descriptor implies.
ID3DBlob* SD_CompileShaderBlob(const wchar_t* a_path, const char* a_target, const char* a_stageDefine, const char* a_extraDefine = nullptr, const char* a_extraDefine2 = nullptr, const char* a_extraDefine3 = nullptr, const char* a_extraDefine4 = nullptr, const char* a_extraDefine5 = nullptr, const char* a_extraDefine6 = nullptr);
uint32_t SD_PositionBytes(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc);

struct SnowDeformation : Feature
{
public:
	virtual inline std::string GetName() override { return "Snow Deformation"; }
	virtual std::string GetDisplayName() override { return T("feature.snow_deformation.name", "Snow Deformation"); }
	virtual inline std::string GetShortName() override { return "SnowDeformation"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SNOW_DEFORMATION"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }

	/** @brief The lighting shader samples the deformation map on landscape draws. */
	virtual bool HasShaderDefine(RE::BSShader::Type shaderType) override
	{
		return shaderType == RE::BSShader::Type::Lighting;
	}

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.snow_deformation.description", "Maintains a persistent deformation map around the player so snow can be visibly compressed by actors moving through it, leaving lasting trails."),
			{ T("feature.snow_deformation.key_feature_1", "Persistent world-space deformation map following the player"),
				T("feature.snow_deformation.key_feature_2", "Trails carved by the player, NPCs and creatures"),
				T("feature.snow_deformation.key_feature_3", "Snowfall-driven snow refill"),
				T("feature.snow_deformation.key_feature_4", "Compute-shader based, low performance impact") } };
	};

	/** @brief Attribution, compiled into any DLL built from this source and emitted once at startup. Kept as a referenced constant rather than a comment because comments do not survive a build - this is what makes a redistributed copy identifiable with `strings`. */
	static constexpr const char* kAttribution =
		"Snow Deformation for Community Shaders - (c) 2026 josefplyr1 - GPL-3.0-or-later - github.com/community-shaders/skyrim-community-shaders/pull/2659";

	// Square world-space deformation window following the camera in whole-texel
	// steps. Texel value = normalized depression depth, 0 = untouched snow,
	// 1 = compressed to the ground. World size is runtime (deformWorldSize),
	// so trench detail coarsens with range.
	static constexpr uint kTextureDim = 2048;
	static constexpr uint kMaxStamps = 256;
	/** @brief Prop contact field: texels across, and half the window (world units) it covers around the deformation centre - 3 units per texel, finer than the map it feeds. Doubled 2026-09-06 (Josef: NPCs in a fight printed by bones until he closed in); the texel size is kept, the field is 4x the texels. */
	static constexpr uint kContactDim = 2048;
	static constexpr float kContactHalfExtent = 3072.0f;
	/** @brief Clear value of the contact field: nothing drawn over this column. */
	static constexpr float kContactNone = 1.0e30f;
	/** @brief Bones a single skin partition may carry: 240 float4 rows at 3 per bone, the game's own ceiling. Partitions past it are skipped rather than truncated. */
	static constexpr uint kContactMaxBones = 80;
	/** @brief Living actors rasterized per frame at most, the NEAREST first (contactPriority); a crowd must not turn the spike into a full character pass. 8 → 16 at Josef's call, 2026-09-06. */
	static constexpr uint kContactMaxActors = 16;
	/** @brief Corpses drawn by the contact pass, budgeted apart from the living: a corpse holds its slot only until it settles. */
	static constexpr uint kContactMaxCorpses = 8;
	/** @brief StampEnds[i].z selector. Carve displaces snow (instantaneous depth, max-blended); melt removes it while a heat source stands there (additive, dt-scaled, so dwell time deepens the bowl). Must match DeformationUpdateCS.hlsl. */
	static constexpr float kStampModeCarve = 0.0f;
	static constexpr float kStampModeMelt = 1.0f;
	/** @brief A discharge throwing snow aside: max-blended like a carve, because the throw is instantaneous, and scorching as it goes. */
	static constexpr float kStampModePit = 2.0f;
	/** @brief Frost refreezing the surface: sustained like a melt, but moving no snow at all - it only hardens what is already there. */
	static constexpr float kStampModeCrust = 3.0f;
	/** @brief Added to a stamp's mode to make it a CONE rather than a capsule. Needs no extra fields: a capsule is already two points and a radius, and a cone is the same three read differently - apex at the segment start, axis to its end, radius = the half-width it has opened to by then. Mirrored by STAMP_MODE_CONE in DeformationUpdateCS.hlsl. */
	static constexpr float kStampModeCone = 10.0f;
	/** @brief Added to a stamp's mode to give a carve a BOWL cross-section rather than a trench's flat floor and standing walls. Mirrored by STAMP_MODE_BOWL in DeformationUpdateCS.hlsl. */
	static constexpr float kStampModeBowl = 20.0f;
	/** @brief Added to a carve made by a body (feet, limbs, a body's own shapes): Snow Resistance and its trampled fade apply per texel. Decoded before BOWL and CONE; TrenchStore's carve test strips it. Mirrored by STAMP_MODE_BODY in DeformationUpdateCS.hlsl. */
	static constexpr float kStampModeBody = 40.0f;
	/** @brief A stamp's impact energy (0-1) rides its mode as a fraction, energy x this: under every decode's 0.5 threshold, so each mode still reads as itself. StampCS records it in the map's .w, where the trench detail turns to its wet (clod) set and its relief rises. Mirrored by STAMP_IMPACT_SCALE in DeformationUpdateCS.hlsl. */
	static constexpr float kStampImpactScale = 0.45f;
	/** @brief A shove (Force) carries the full impact energy. */
	static constexpr float kStampModeShove = kStampImpactScale;
	/** @brief Cap on spell emitters STAMPED per frame. Well under kMaxStamps: emitters are appended after actors and props, so a barrage cannot starve foot prints out of the budget. */
	static constexpr size_t kMaxSpellEmitters = 64;
	/**
	 * @brief How many emitters may be GATHERED before the distance sort trims to kMaxSpellEmitters.
	 *
	 * Producers run in a fixed order, so a cap enforced while gathering drops
	 * whatever runs last regardless of distance. Gather wide, then keep the
	 * nearest - the same policy the exclusion field uses when it overflows.
	 */
	static constexpr size_t kSpellEmitterCeiling = 192;
	/**
	 * @brief Slots of kMaxStamps that only spells may take.
	 *
	 * Actors and props are gathered first, so a mass ragdoll can fill every
	 * slot before one emitter is read. Small enough that the reservation costs
	 * only a few limb prints inside a pile; large enough that spells still
	 * mark during the fight that caused it.
	 */
	static constexpr uint kSpellStampReserve = 48;

	/** @brief Skyrim world units per meter (1 unit â‰ˆ 1.43 cm). Range sliders are in meters. */
	static constexpr float kUnitsPerMeter = 70.0f;
	/** @brief Object Snow range floor (m); below it the 2048-unit skin dissolve band starts at the camera. */
	static constexpr float kMinRangeSkinsM = 50.0f;
	/** @brief Object Snow range ceiling (m): past it the coat reads no different from the LOD recolor under it. */
	static constexpr float kMaxRangeSkinsM = 200.0f;

	// ---- Snow shell: a camera-following grid of real snow geometry ----

	static constexpr uint kShellGridDim = 640;
	// 8-unit inner spacing: 16-unit vertices undersample the ~10-unit trench
	// walls into blocky silhouettes.
	static constexpr float kShellGridSpacing = 8.0f;

	// Distance warp, power-of-two bands. Each band holds kShellWarpBandVerts
	// vertices spaced kShellWarpBandMul x kShellGridSpacing apart; steps are
	// exact powers of two of the base step and every band START is a multiple
	// of both its own step and kShellOriginSnap.
	//
	// INVARIANT: break the alignment and adjacent vertices snap one or two
	// fineSteps apart depending on the grid centre, so quad widths flip as the
	// camera moves and the surface inside them jumps (distant up/down jumping).
	// Must match the kWarpBand tables in SnowShell.hlsl.
	//
	// The coarsest band is 128 units - exactly the land-vertex spacing - from
	// 2432 out to the seam, so the shell samples every terrain texel it covers
	// and needs no clearance pad. A 256-unit outer band would skip every other
	// texel, which is what made a pad necessary.
	static constexpr int kShellWarpBands = 5;
	static constexpr float kShellWarpBandVerts[kShellWarpBands] = { 192.0f, 8.0f, 8.0f, 8.0f, 104.0f };
	static constexpr float kShellWarpBandMul[kShellWarpBands] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };

	/** @brief Grid origin snap. The coarsest band step, so every vertex lands exactly on its own band's world lattice with no per-vertex rounding left to churn. */
	static constexpr float kShellOriginSnap = kShellGridSpacing * 32.0f;

	/** @brief World half-span of the warped shell grid (center to edge) at a given inner spacing. Linear in spacing: the band shape is unchanged. */
	static float ShellWarpedHalfSpan(float a_spacing = kShellGridSpacing)
	{
		float span = 0.0f;
		float verts = 0.0f;
		for (int band = 0; band < kShellWarpBands; ++band) {
			span += kShellWarpBandVerts[band] * kShellWarpBandMul[band];
			verts += kShellWarpBandVerts[band];
		}
		// Any vertices past the table extend at the coarsest step.
		span += std::max(kShellGridDim * 0.5f - verts, 0.0f) * kShellWarpBandMul[kShellWarpBands - 1];
		return span * a_spacing;
	}

	// Terrain data window: 16x16 cells at land-vertex resolution (128 units),
	// cell-anchored so texels never resample as the camera moves. Sized with
	// margin so the warped grid never samples past the window even with the
	// camera at a cell edge.
	static constexpr int kShellWindowDim = 1024;
	static constexpr float kShellVertexSpacing = 128.0f;
	static constexpr int kShellTexelsPerCell = 32;
	static constexpr int kShellWindowCells = kShellWindowDim / kShellTexelsPerCell;
	/** @brief Height sentinel for window texels with no baked cell data. */
	static constexpr float kShellMissingHeight = -100000.0f;

	// Landscape mods retexture the same vanilla LTEX files, so classes match
	// on diffuse filename substrings. First match wins: more specific names
	// must precede their substrings ("grasssnow" before "snow01"). Unmatched
	// snow-material textures fall to "Snow 01", anything else to "Other".
	// A class is only the DEFAULT depth for the textures that match it; each
	// texture is tunable on its own and the terrain-shader snow mask follows
	// the resolved depth, not the class.
	static constexpr uint kSnowClassCount = 12;
	// Default depths live ONLY in Settings::SnowClassDepths - a defaultDepth
	// column here was never read at runtime and drifted from the live values.
	struct SnowClassDef
	{
		const char* label;
		const char* match;
	};
	static constexpr SnowClassDef kSnowClasses[kSnowClassCount] = {
		{ "Grass Snow", "grasssnow" },
		{ "Trodden Path", "snowpath" },
		{ "Snowy Rocks", "snowrocks" },
		{ "Snow 01", "snow01" },
		{ "Snow 02", "snow02" },
		// Non-snow classes sit at -8, matching the fully-bare submerge in
		// ShellSurfaceZ, so every bare texel reaches one depth instead of
		// stopping short of it.
		{ "Roads", "road" },
		{ "Dirt", "dirt" },
		{ "Grass & Fields", "grass" },
		{ "Rocks & Cliffs", "rock" },
		{ "Coast & Beach", "coast" },
		{ "Mud & Rivers", "mud" },
		{ "Other", "" },
	};

	// Textures whose family default is wrong for them: frozen marsh ice is
	// snow but shares no name with any snow family. Substring match on the
	// diffuse path, first match wins. These do not follow their family
	// slider; the user's own value still wins over both.
	struct TextureDefaultDef
	{
		const char* match;
		float depth;
	};
	static constexpr TextureDefaultDef kTextureDefaults[] = {
		{ "frozenmarshice", 20.0f },
	};

	/** @brief Texture layers kept per vertex: the heaviest of the quad's base texture plus its 6 painted layers. */
	static constexpr uint kShellVertexLayers = 4;
	/** @brief Layer slot with no texture (unpainted weight, or an LTEX with no texture set). */
	static constexpr uint16_t kNoLandTexture = 0xFFFF;
	/** @brief Registry cap. Beyond it new textures fall back to their class depth. */
	static constexpr size_t kMaxLandTextures = 512;

	/** @brief Baked per-cell terrain data: 33x33 vertex heights (absolute Z) plus each vertex's land-texture layers and their weights (0-255). Depths are applied at window-rebuild time so the depth sliders retune without a game re-bake. */
	struct ShellCellData
	{
		std::array<float, 33 * 33> height;
		// Land-texture registry indices and their 0-255 weights. Depth is
		// resolved at window-rebuild time, not here, so the sliders retune the
		// shell from cached data without a re-bake.
		std::array<std::array<uint16_t, kShellVertexLayers>, 33 * 33> layerTexture;
		std::array<std::array<uint8_t, kShellVertexLayers>, 33 * 33> layerWeight;
		/** @brief Max component of the land vertex color per vertex (0-255): the baked AO ground's skylighting is applied relative to. Packed into the terrain window's w channel as [0, 0.499). */
		std::array<uint8_t, 33 * 33> vertexAO;
		/** @brief The engine's own land mesh heights at 32-unit spacing (129 x 129, x-fastest), read from the four quad BSTriShapes at bake time; empty when a quad's vertex data was not readable. The fine layer copies these in verbatim, so the shell stands on the mesh the game draws rather than on a cubic guess at it. Kept for at most kShellFineCacheCells cells. */
		std::vector<float> fine;
		/** @brief Mesh level per quad the fine data was read at: 1 = 17x17, 2 = 33x33, 4 = 65x65 (the engine picks by distance); re-read when it changes. */
		std::array<uint8_t, 4> fineLevel{};
		// City worldspaces reuse their Tamriel cell coordinates (WindhelmWorld
		// spans the same 28-36 / 6-12 block as the terrain outside its gate),
		// so the coordinate key alone matches cells from a worldspace we left.
		uint32_t worldspaceID = 0;
	};

	/** @brief One-point readout of the terrain data behind the shell, for the menu diagnostics. Reads the CPU cache the window is built from. */
	struct ShellProbe
	{
		float worldX = 0.0f;
		float worldY = 0.0f;
		int cellX = 0;
		int cellY = 0;
		int vertexX = 0;
		int vertexY = 0;
		bool cellFound = false;
		bool worldspaceMatch = false;
		uint32_t cellWorldspace = 0;
		uint32_t activeWorldspaceID = 0;
		uint32_t windowWorldspaceID = 0;
		float height = 0.0f;
		float rampDepth = 0.0f;
		float coverage = 0.0f;
		struct Layer
		{
			std::string label;
			float weight = 0.0f;
			float depth = 0.0f;
		};
		std::vector<Layer> layers;
	};

	/** @brief The captured statics around a world position, for naming whatever the object snow is actually skinning. */
	struct ObjectSnowProbe
	{
		struct Entry
		{
			std::string name;
			std::string model;
			float radius = 0.0f;
			float topZ = 0.0f;
			float distXY = 0.0f;
			bool road = false;
		};
		size_t captured = 0;
		size_t overlapping = 0;
		std::vector<Entry> entries;
	};

	/** @brief One landscape texture discovered by the bake. The diffuse path is the settings key; the class supplies the depth until the user overrides it. */
	struct LandTextureEntry
	{
		std::string path;
		std::string label;
		int classIndex = (int)kSnowClassCount - 1;
		float depth = -5.0f;
		bool overridden = false;
		/** @brief Set when kTextureDefaults ships a depth for this texture, which then replaces the family default. */
		bool shipped = false;
		float shippedDepth = 0.0f;
	};

	/** @brief Cached stamp bones per actor, keyed by formID; rebuilt when the 3D root changes (cell reload, decapitation swap). Feet stamp heel-to-toe prints; limbs are joint-to-joint segments (nearest matched ancestor to bone) that carve when inside the snow layer. */
	struct StampBones
	{
		RE::NiPointer<RE::NiAVObject> root;
		struct Foot
		{
			RE::NiPointer<RE::NiAVObject> node;
			RE::NiPointer<RE::NiAVObject> toe;
		};
		std::vector<Foot> feet;
		struct Limb
		{
			RE::NiPointer<RE::NiAVObject> a;
			RE::NiPointer<RE::NiAVObject> b;
			float radius;
			/** @brief Shortest this segment has ever measured, so a torn body cannot comb a line. Bones are rigid, so a segment's length is a property of the skeleton; the shortest sighting is the honest one, and taking the minimum lets a first sight that happened to be mid-shatter correct itself. Negative until first measured. */
			float restLength = -1.0f;
		};
		std::vector<Limb> limbs;
		/** @brief Material alpha of the most opaque skinned body geometry, or -1 until measured. A ghost is drawn see-through all over. */
		float bodyAlpha = -1.0f;
		/** @brief Geometry whose alpha bodyAlpha is. */
		RE::BSFixedString bodyAlphaSource;
		/** @brief Lowest alpha over every lit skinned geometry, proxies and overlays included, and where it was read. */
		float anyAlpha = -1.0f;
		RE::BSFixedString anyAlphaSource;
		/** @brief Frames until the alpha is measured again. A ghost's shader can arrive after its 3D does, so one early look at an opaque body must not stand for the actor's whole life. */
		uint16_t alphaRecheck = 0;
		/** @brief How many times the body alpha has been read. The first reads of a freshly spawned actor cannot be trusted, so they are taken quickly and often before the cadence drops to kBodyAlphaRecheckFrames. */
		uint16_t alphaSettle = 0;
		/** @brief Frames until cached feet are re-verified as still attached to the root. Runtime skeleton editors (RaceMenu/NiOverride, IED, MuSkeletonEditor) edit the live tree without swapping the root, so the root key alone cannot vouch for a cached node: a detached foot's world transform freezes and it never plants again. */
		uint16_t attachRecheck = 0;
		/** @brief Frames left before a freshly seen 3D has its bones walked; gear is still attaching in the frames after a skeleton loads. */
		uint16_t collectDelay = 0;
		/** @brief XY units walked on the ground with zero foot prints. A healthy walker plants each foot every ~60 units, so a growing figure here means the cached feet no longer speak for the skeleton, whatever edited them. */
		float dryTravel = 0.0f;
		float prevPosX = 0.0f;
		float prevPosY = 0.0f;
		bool hasPrevPos = false;
		/** @brief The one mid-dry re-collection has been spent; the next threshold latches the fallback. */
		bool dryRecollected = false;
		/** @brief Latched demotion to collision-shape stamping (and collision-measured floating), cleared when the 3D root changes. The failsafe for skeletons whose feet exist but never plant. */
		bool collisionFallback = false;
		/** @brief Frames this actor prints by bones because its last contact draw put nothing into the field. Counts down; at zero the mesh route is tried again. */
		uint16_t contactStarved = 0;
		/** @brief The starvation line for this 3D has been logged. */
		bool contactStarveLogged = false;
	};

	struct Settings
	{
		bool EnableSnowDeformation = true;
		/** @brief Unsupported-snow settle speed, 0-1. Strips left standing between separate trails sink about halfway toward the shallower side's floor once BOTH sides are dug away; walls and open snow never move. 0 disables the pass. */
		float SlumpRate = 0.1f;
		/** @brief Multiplier on the snowfall-driven refill rate. 0 disables refilling. */
		float RefillRateMultiplier = 1.0f;
		/** @brief Refill rate follows the current weather's snowfall density; clear spells and interiors do not refill. Off: constant baseline rate in any weather. */
		bool RefillOnlyWhenSnowing = true;
		/** @brief Trenches survive leaving the deformation window: departing texels go to a sparse world-grid tile store and are re-injected when the window returns. Off discards them on departure. */
		bool PersistTrenches = true;
		/** @brief In-game days for a stored trench to fade with no snowfall at all. Snowfall does the real erasing, at the live refill's own rate so ground behaves the same whether or not it is being looked at; this is the floor underneath it, so a clear-weather modlist still prunes its store instead of growing one for ever. The floor rarely decides anything, since three snowless days running is unusual. 0 disables it and leaves snowfall as the only reaper. */
		float StoredTrenchFadeDays = 3.0f;
		/** @brief Budget for the trench store, in MB of encoded data - which is what a save will cost once #34 Stage C writes it, not the raw in-memory figure. Beyond it, least-recently-visited ground is forgotten first. The cap is the ONLY thing bounding the store: decay alone leaves it unbounded on ground that never sees snowfall, and a fully trodden worldspace would be gigabytes. 1 MB is roughly two to four deformation windows of remembered ground. */
		float TrenchMemoryMB = 1.0f;
		/** @brief Ceiling the accumulated layer grows to, as a multiple of each class's authored depth. Scaling is proportional, so the gap between a road and the field beside it widens as it snows. 1.0 = accumulation reaches nothing. */
		float AccumulationPeak = 1.5f;
		/** @brief The accumulated layer scales the shell's depth. Off pins it at the authored depth. */
		bool EnableSnowAccumulation = true;
		/** @brief Carry the accumulated layer through save/load on the 'SNAC' co-save record. Off, the record is written zeroed and every load starts at the authored depth. */
		bool PersistAccumulation = true;
		/** @brief Edge Lump Reach follows the accumulation scalar itself (0 at rest, 1 at the peak, over Accumulation Time / Melt Time / Fade regardless of the peak's height) instead of the slider. */
		bool RecoloredSnowAccumulates = true;
		/** @brief "Steep Face Thinning", 0-1: share of the accumulation reach a vertical face gives up (none below ~60 degrees of slope, all of this by ~80). The game's own paint is never touched: at Edge Lump Reach 0 this does nothing. Feeds StaticsCB::SteepThin. */
		float SteepFaceThinning = 1.0f;
		/** @brief Game hours of full-intensity snowfall to grow from the authored depth to the peak. Growth is scaled by the held snowfall intensity, so light snow takes proportionally longer. Tuned low so the change is visible within a session at typical timescales. */
		float AccumulationHours = 1.0f;
		/** @brief Game hours to settle from the peak back to the authored depth in clear weather. Equal to the growth time rather than asymmetric, so the change stays watchable. */
		float AccumulationMeltHours = 1.0f;
		/** @brief In-game days for the layer to settle on its own, applied in ANY weather including snowfall. This is the guarantee that the world returns to its authored height even through a winter that keeps topping it up; unlike the trench floor it is the same order as the melt, so it also shortens a clear-weather settle. 0 disables it and leaves the melt as the only reaper. */
		float AccumulationFadeDays = 3.0f;
		/** @brief How much slower melted ground refills than trampled ground, 0-1. The ground under a fire is warm and wet after the flame is gone, so a melt basin outlasts a footprint of the same depth. Applied as a refill slowdown rather than as banked extra depth: depth must stay within 0-1 or the saturating readers flatten the bowl profile into a walled pit. 0 = melted ground recovers exactly as fast as a footprint. */
		float MeltPersistence = 0.50f;
		/** @brief Fraction of a melt bowl's radius held at full depth before the flank begins. 0 = a pure bowl curving from the centre; high = a flat floor with walls. Heat spreads, so low values read as melted and high ones read as blasted. */
		float MeltBowlFloor = 0.11f;
		/** @brief Depth a shock discharge pocks the snow to, as a fraction of the layer, times the strike's strength; the pock mask keeps 45-100% of it per texel. Max-blended like a carve, so a cloak standing over one spot never bores a shaft. */
		float PitDepth = 1.00f;
		/** @brief Reach of a single discharge mark, in world units, before its arc legs. */
		float PitRadius = 50.0f;
		/** @brief Reach of a SHOCK cloak, kept apart from the fire one: arcs jump clear of the body where heat wraps it, so the two want different numbers. */
		float ShockCloakRadius = 75.0f;
		/** @brief Seconds between a shock cloak's discharges. Higher is sparser - lightning cracks now and then rather than pouring. */
		float ShockCloakInterval = 1.50f;
		/** @brief Size of one cloak arc against the cloak's own reach. */
		float ShockCloakStrikeScale = 0.25f;
		/** @brief Draw the arc that justifies the pock. The snow already marks where a cloak discharges; without something reaching the spot, the hole reads as a glitch rather than as lightning. Visual only - nothing is spawned into the world. */
		bool EnableLightningArcs = true;
		float LightningArcWidth = 10.0f;
		float LightningArcBrightness = 10.0f;
		float LightningArcLife = 0.30f;
		std::array<float, 3> LightningArcTint = { 0.698f, 0.620f, 1.0f };
		/** @brief How dark a discharge burns the snow it struck. 0 removes the scorch and leaves the pocking alone. */
		float ScorchStrength = 0.85f;
		/** @brief How fast frost sets a crust, for a spell of Frostbite's strength. */
		float CrustRate = 0.6f;
		/** @brief Base reach, in world units, of a frost source that names none: frost cloaks, frost atronach auras (x AtronachFrostReach) and frozen corpses (x body size). Area spells take their own area; streams kSpellContactRadius. */
		float CrustRadius = 90.0f;
		/** @brief Depth a boot still prints on fully crusted snow, as a fraction of loose snow. NOT zero: actors stand on terrain while the shell floats above them, so a crust that takes no print at all buries feet inside apparent ice. */
		float CrustPrintDepth = 0.20f;
		/** @brief Minutes a crust takes to thaw on its own, with no snowfall at all. Ice answers to temperature rather than to weather, so this runs even under a clear sky where the refill has stopped. */
		float CrustThawMinutes = 4.0f;
		/** @brief How completely carving through a crust destroys it, against how deep the cut went. At 1 a shallow print barely dulls the glaze; higher values let any cut break the skin properly, which is what stops a trench through ice reading as a groove in ice cream. */
		float CrustBreakOnCarve = 6.0f;
		/** @brief Stamp radius past which a shape counts as heavy enough to break a crust rather than print on it (CrustBreakForce). Capsule stamps only: bodies outside the contact window (kContactHalfExtent) or past its budget (kContactMaxActors living, kContactMaxCorpses dead) and props outside it, such as a distant mammoth or a landing dragon. The contact carve puts no weight through a crust. */
		float CrustBreakRadius = 30.0f;
		/** @brief Master switch for spell-driven marks. Off, the melt path still exists for the test emitter and for campfire clearings. */
		bool EnableSpellIntegration = true;
		/** @brief Stop actors that never reach the ground from carving it. Measured rather than listed: an actor whose lowest contact stays clear of its own footing is not standing on anything, which covers atronachs, wisps, ghosts and any modded levitator without naming one. */
		bool NoCarveFloatingActors = true;
		/** @brief How far an actor's lowest contact may sit above its footing and still count as standing on it, in world units. Above this it carves nothing. Generous enough to cover a walker's stride and the slack in a creature skeleton's lowest bone. */
		float FloatingActorBand = 20.0f;
		/**
		 * @brief How an actor with no substance is recognised, so it stops carving. 0 off, 1 by translucency, 2 by the record flag, 3 either.
		 *
		 * A separate question from the floating gate: a ghost stands with its
		 * feet on the ground, so no clearance measurement can catch one.
		 * Translucency needs no list and is the honest test. The record flag is
		 * the fallback, and broader than it sounds - "Is Ghost" means
		 * invulnerable.
		 */
		int IncorporealMode = 1;
		/** @brief Let shouts plough the snow in front of the shouter. Off, a shout still marks through whatever projectile it throws, which is why the breaths already made a mark or two before this existed. */
		bool EnableShoutCones = true;
		/** @brief Multiplier on the reach the shout's own projectile authors: 1000 units for Unrelenting Force, 1200 for the breaths, 10000 for a dragon's. The mark fades and breaks up before it ends, so this is where the last streaks stop. */
		float ShoutMarkReach = 0.1f;
		/** @brief Widest full width of a shout's mark, as a fraction of its reach. The records author no width - only a reach - so this is taste. */
		float ShoutMarkWidth = 0.35f;
		/** @brief Where along the reach the mark is widest, as a fraction of it. */
		float ShoutMarkWaist = 0.5f;
		/** @brief Outline exponent: 1 straight-sided, 2 an oval, higher blunter. */
		float ShoutMarkRoundness = 1.5f;
		/** @brief Fraction of the reach held at full depth before the mark fades. */
		float ShoutMarkFadeStart = 0.25f;
		/** @brief Depth at the far end, as a fraction of the depth at the mouth. */
		float ShoutMarkFarDepth = 0.25f;
		/** @brief Shout churn, 0-1. On a shove, uneven streaks near the mouth thinning to dashes with distance (coneCarve, the carve branch only); on every cone, a wobble of the outline (edgeCoord), so a breath's melt or glaze stays solid with a ragged edge. 0 = a smooth scoop, clean edge. */
		float ShoutChurn = 1.0f;
		/** @brief Width of a churn streak, world units. */
		float ShoutStreakSize = 8.0f;
		/** @brief Length of a churn streak as a multiple of its width, along the shout. */
		float ShoutStreakLength = 1.0f;
		/**
		 * @brief Raise buried frost effects onto the snow surface instead of leaving them under it.
		 *
		 * Effects placed at terrain height are swallowed by the shell above
		 * them. Frost is the only school where that matters - fire and lightning
		 * remove the snow they sit in, frost only hardens it. The ONLY thing in
		 * the feature that moves a game object, so it is narrow and switchable.
		 */
		bool LiftFrostEffects = true;
		/** @brief Raises a rune's effect meshes (shimmer, glow, cast flash) onto the snow over them. Ground runes only. */
		bool LiftRunes = true;
		/** @brief Scorch, frost and shock marks and rune glyphs - the engine's runtime decals - are put back over the object-snow coat drawn on top of them. */
		bool DecalsOnObjectSnow = true;
		/** @brief Paints a rune's glyph - an engine decal on the ground the shell covers - onto the landscape shell. */
		bool RuneDecalsOnSnow = true;
		/** @brief Paints the game's frost impact decals onto the landscape snow and road snow as object snow wears them: the ones on the ground under the snow, or on something level with its top (a walkway). */
		bool FrostDecalsOnSnow = true;
		/** @brief How deep a travelling shove scours, against a full carve. Well under 1 on purpose: a vortex scours the surface rather than excavating to the ground, and the berm is derived from how deep the cut goes - so this is also the dial that decides whether the track reads as a scoured hollow or as a canyon with a ridge down each side. */
		float ForceTrackDepth = 0.45f;
		/** @brief Width of the track a slow shove leaves behind it, in world units. Nothing authors a width for any shout - only a reach - so this is taste, exactly as the cone's spread is. */
		float ForceTrackWidth = 80.0f;
		/** @brief Width of the furrow a dash shout ploughs, against the dasher's own size. A dragon hurling itself forward cuts a wider one than a man. */
		float DashGougeScale = 0.5f;
		/** @brief Depth a full-strength shove carves, as a fraction of the layer. Force is the only school that DISPLACES snow rather than changing it, so it is also the only one that raises a berm at the far lip - which is what makes Unrelenting Force read as pushed rather than deleted. */
		float ForceCarveDepth = 0.5f;
		/** @brief Let a body that is still burning, crackling or frozen over go on marking the snow beneath it. The element comes from the last one that struck the actor before it died, so it needs no reading of the corpse itself. */
		bool CorpseElementalMarks = true;
		/** @brief Seconds a body goes on marking after it dies. Mods that keep a corpse visibly alight or frozen (Frozen Electrocuted Combusted and its like) run far longer than vanilla, so this is taste rather than physics. */
		float CorpseEffectSeconds = 8.0f;
		/** @brief Reach of a FIRE atronach's innate aura, against the fire cloak reach. An atronach's whole body burns, so it works a wider circle than a cloak wrapped round a mage. */
		float AtronachFireReach = 0.50f;
		/** @brief Radius of the blast a fire atronach leaves when it dies, in world units. Seeded from the record the game authors for that explosion (400), then scaled like any other blast. */
		float AtronachFireDeathRadius = 400.0f;
		/** @brief Seconds a dead fire atronach keeps burning the ground it fell on. The body burns out after it lands, so the mark deepens for a while and then stops - unlike the blast, which is one moment. */
		float AtronachFireBurnSeconds = 6.0f;
		/** @brief Reach of a FROST atronach's innate aura, against the frost crust reach. This is the glaze it leaves in the trench it walks. */
		float AtronachFrostReach = 2.00f;
		/** @brief Radius a frost atronach glazes when it shatters, in world units. The record authors 100, but that is the radius it HURTS over, and after the blast scale it lands near 30 units - under half a footprint, so the mark could not be judged at all. Raised to something visible; still far tighter than the fire one, which is the relationship the records describe. */
		float AtronachFrostDeathRadius = 240.0f;
		/** @brief Reach of a STORM atronach's innate aura, against the shock cloak reach. */
		float AtronachShockReach = 1.00f;
		/** @brief Radius a storm atronach discharges over when it dies, in world units. Authored at 320. */
		float AtronachShockDeathRadius = 320.0f;
		/** @brief Reach of a cloak's mark on the ground, in world units. Unlike a blast there is no authored number to scale against - a cloak record says nothing about how far its heat spreads - so this is the reach itself. It also widens with the wearer's height above the snow, as every airborne source does. */
		float CloakRadius = 100.0f;
		/** @brief Scale on the crater a detonation leaves, against the radius the explosion record authors. Bethesda's blast radii are tuned for damage, not for how far the ground should be scarred, and read far too wide on snow at 1.0. */
		float BlastRadiusScale = 0.33f;
		/** @brief Depth per second a reference-magnitude fire stream melts at its core. Effect magnitude scales it, so a stronger spell melts faster without reaching any deeper. */
		float SpellMeltRate = 0.8f;
		/** @brief How far a melt bowl's rim wanders, as a fraction of its radius. Coarse-celled on purpose: it moves the OUTLINE without chipping the surface, which is what separates a melt basin from a crater. */
		float MeltEdgeIrregularity = 0.15f;
		/** @brief Per-class shell depths, indexed like kSnowClasses (defaults duplicated from the table). The default for any texture without its own entry in TextureDepths. */
		std::array<float, kSnowClassCount> SnowClassDepths = { 14.0f, 18.0f, 30.0f, 30.0f, 30.0f, -8.0f, -8.0f, -8.0f, -8.0f, -8.0f, -8.0f, -8.0f };
		/** @brief Per-texture depth overrides keyed by lowercased diffuse path. Keyed by path, not form ID, so load-order changes cannot rebind them. */
		std::map<std::string, float> TextureDepths;
		/** @brief "Edge Lump Reach", 0-1: how far the coat reaches past the game's solid paint, as a drop of the cut on the projection's own weight read back from Masks.y (1 = kEdgeReachWeightShift, 0 = the paint itself). Same from every view. Feeds StaticsCB::EdgeFlankWidth. */
		float SkinEdgeFlankWidth = 0.0f;
		/** @brief Model-class override: ROAD MESHES (matched by geometry name or road texture path; "bridge" in either excludes). Default deliberately below the ~30-unit surrounding snow classes: the shallow band is what makes the road's course readable through the snowfield. */
		float RoadMeshesDepth = 10.0f;
		/** @brief Landscape shell, road class and trench patch inside child worldspaces (the walled cities). Off by default (Josef, 2026-09-17): where those shells meet city meshes they never sit right. Off leaves the city ground as the game draws it; object snow stays. */
		bool CityShells = false;
		/** @brief Shell albedo texture, loaded through the VFS. User-editable so the shell can be matched to the modlist's snow by eye. The loader resolves PBR companion maps and falls back to the legacy path when the PBR set is absent. */
		std::string SnowTexturePath = "Textures\\PBR\\Landscape\\snow01.dds";
		/** @brief "Snow PBR Config" (2026-09-22): pins the TruePBR texture set the shell takes its roughness, specular and glint from. Empty = the ranked automatic match. Exists because a modlist can hold several sets whose key contains the snow texture's basename and no rule fits every one - an AutoBlend stub with no glintParameters block once won on the letter B and took the sparkle off both shells. */
		std::string SnowPBRConfig = "";
		/** @brief Radius multiplier for the workspace clearings (workstations, stalls, wells, shrines). */
		float TrampleZoneScale = 0.75f;
		/** @brief Snow height remaining in a workspace clearing, in PERCENT of the class depth. 0 = melted to the floor, 100 = no clearing. */
		float TrampleZoneHeight = 50.0f;
		/** @brief Percent of its own thickness a heavy, rounded dropped item presses into its trench floor (WEIGHT-SINK-PLAN.md). Flat items take a fraction of it by shape. */
		float ItemEmbedPercent = 50.0f;
		/** @brief Dropped items rest in the snow by weight and shape; off, they lie on the ground under it as the game leaves them. */
		bool ItemSink = true;
		/** @brief Thickness over the side of the largest face below which an item counts as flat and does not press into its trench floor; the share ramps to full at 0.5. */
		float ItemMinRoundness = 0.20f;
		/** @brief Percent of full grip: snow damps a dropped item's roll and slide by how deep it is in and how slowly it is travelling. 0 = the game's physics untouched. */
		float ItemSnowGripPercent = 100.0f;
		/** @brief Creatures lighter than the player are drawn standing part way up the snow and carve only as deep as they sink (CreatureSink.cpp, TRACK-VARIATION-RESEARCH.md Part 4 Stage 3). Off, every actor stands on the ground under it. */
		bool CreatureSink = true;
		/** @brief Dune-field amplitude in world units; 0 flattens deep snow into a mathematically smooth sheet. */
		float UndulationStrength = 8.0f;
		/** @brief Multiplier on the dune field's wavelengths; larger = broader, calmer waves instead of a spike carpet. */
		float UndulationSpacing = 0.5f;
		/** @brief Tessellate the ground snow and the trench patch near the camera. Its real job is trench smoothness: the hull shaders' factors key off the deformation map, so carves get vertex density the land's own triangles cannot express. */
		bool Tessellation = true;
		/** @brief Parallax self-shadow strength on the snow micro-relief (Extended Materials' term, the one PBR ground already receives). 0 skips the taps entirely. */
		float ParallaxShadowStrength = 0.5f;
		/** @brief Parallax occlusion depth on the landscape shell, as a multiplier on the PBR config's displacementScale. 1 = exactly the slab depth PBR ground gets, since kSnowUVTile matches the landscape tiling. 0 skips the march. */
		float ParallaxDepth = 1.0f;
		/** @brief Edge berm crest height as a fraction of the local snow depth. */
		float BermHeight = 0.50f;
		/** @brief Berm Steepness, 0-2: the berm's reach (its tap rings) is 2^(1 - s). 1 = the authored ~40-unit apron; 0 spreads the same crest height twice as wide, 2 packs it into half the width. */
		float BermSteepness = 0.75f;
		/** @brief Stage 3 P5: rolled rim lip height as a fraction of local depth (cornice look). 0 = off. */
		float RimLip = 0.05f;
		/** @brief Rim teeth strength: the carve contour breaks into irregular teeth on two octaves of SnowDetailNoise (37 and 8 units). 0 = off. */
		float RimTeeth = 0.50f;
		/** @brief Snow Resistance, percent: how much of a body's push the snow holds back. A body carves (100 - this)% of its reach into the layer, so a boot on the ground leaves half the layer at 50. Loose props, spells, melt and resting items keep their own depths. 0 = every reach carves in full. */
		float SnowResistance = 33.0f;
		/** @brief Snow Climate, 0-1: how far the snow climate (temperature, wetness, stickiness, firmness) shapes trenches instead of the fixed look. 0 = the look the sliders and Fresh Snow Softness give; 1 = berm, lip, teeth and wall softness follow the snow, each with its slider (the walls: kTrenchWallSharpness) as the maximum. TRACK-VARIATION-RESEARCH.md 3.3. */
		float SnowClimateStrength = 0.75f;
		/** @brief Track Ageing, 0-2: how fast tracks age on the ageing clock (TRACK-VARIATION-RESEARCH.md 3.4). An old track rounds toward the carve's short blur, loses its rim teeth and lip first, lowers its berm, and wears new snow over its detail and wet print. 1 = fully aged after kTrackAgeSpan clock units (about a day of calm cold, an hour of snowfall); 0 = tracks never age. */
		float TrackAgeing = 1.0f;
		/** @brief Wet Snow and Ice (TRACK-VARIATION-RESEARCH.md 3.6, Part 4 Stage 4): wet snow darkens by Community Shaders' wetness rule and stops sparkling; meltwater held in packed snow refreezes into a glaze that bears new prints. Rides Snow Climate. Off = Stage 1's wet print alone. */
		bool WetSnowAndIce = true;
		/** @brief Snow condition: how much fresh snow softens trenches - wall sharpness, berm height, rim lip and teeth all ease toward a loose-snow look while the freshness scalar is high. 0 = off, the snow is always firm. */
		float FreshSnowSoftness = 0.66f;
		/** @brief Snow condition: game hours of clear weather for fresh snow to settle fully firm. */
		float FreshSnowSettleHours = 24.0f;
		/** @brief Trampled ground: multiplier on the trench detail's relief on floors walked deeper than one pass; the berm keeps its own. 1 = off. */
		float TrampledFlattening = 0.5f;
		/** @brief Trench detail tiles (tools/churn-tiles): height and slopes of the per-condition broken snow in and around trenches, x the authored relief. 0 = off. */
		float TrenchDetailRelief = 1.0f;
		/** @brief Trench detail: snow roughness multiplier over the detail mask (lower = glossier). 1 = the snow set's own. */
		float TrenchDetailRoughness = 1.0f;
		/** @brief Trench detail: how far across the trench it reaches, 0-2.5: 0 the floor only, 1 just past the rim, 2 over the berm. Soft at every value. */
		float TrenchDetailCoverage = 1.75f;
		/** @brief How far a hard hit's impact energy (the trench detail's impact set) spreads, as a share of what struck (0.5-4). At 1 a foot strikes over its own print, and a body knocked down (or one with no feet to read) over 0.45 of its bound. */
		float ImpactReach = 0.75f;
		/** @brief A fast foot leaving its print throws a wedge of impact energy back from it (no carve), and the trench detail shows that thrown snow past the trench too. */
		bool SpeedSpoil = true;
		/** @brief Speed Spoil: the wedge's length and width, as a share of a kick at the actor's speed (0.1-2). */
		float SpeedSpoilSize = 0.25f;
		/** @brief "Snow Self-Shadowing": heightfield self-shadow (the 5-tap horizon march over the cached heights) on the ground snow and object snow, faded out toward a grazing sun. Off = only the cascades and the SSS mask shade the snow. A quality-preset lever (Low turns it off). */
		bool ShellHorizonMarch = true;
		/** @brief "Snow Sparkle": both shells' specular runs TruePBR's glint NDF (needs its glint noise). Off = plain specular. A quality-preset lever. */
		bool SnowGlints = true;
		/** @brief "Snow Contact Shadows": both shells multiply in Community Shaders' screen-space shadow mask (needs Screen-Space Shadows). A quality-preset lever. */
		bool SnowScreenSpaceShadows = true;
		/** @brief "Snow Casts Shadows": the landscape shell and the trench patch are drawn into the sun's shadow cascades. Off = the snow still receives shadows but casts none. A quality-preset lever. */
		bool SnowCastsShadows = true;
		/** @brief "Object Snow Near Detail": the object height raster's near level, a quarter of the coarse window (FineRasterHalfExtent) at a quarter of its texel. Built only with the road maps and read by PatchTop, so it serves the road patch and road skins. Off = they read the coarse level everywhere. A quality-preset lever. */
		bool ObjectSnowNearDetail = true;
		/** @brief The third-person camera is pulled in along its own line when the game places it under the landscape shell (Havok has no snow to collide with). */
		bool CameraAboveSnow = true;
		/** @brief Blood on snow (BLOOD-DESIGN.md): the game's blood decals and pool quads are re-rasterised into a blood map the shells tint by. Off = the map is neither written nor read. */
		bool BloodOnSnow = true;
		/** @brief Concentration multiplier on every deposit and on the shells' extinction, 0..2. */
		float BloodIntensity = 1.0f;
		/** @brief Snowfall that buries a mark completely, as a fraction of one full trench refill (the same clock that refills prints). */
		float BloodBurial = 0.15f;
		/** @brief Game hours over which a mark dries: the wet sheen goes and the hue turns maroon. */
		float BloodAgeHours = 1.0f;
		/** @brief Gloss of fresh blood on the shells, 0..1 (0 = matte like the snow). */
		float BloodSheen = 0.5f;
		/** @brief Seconds a fresh mark takes to spread to its full shape: the dense core first, the thin fringe last. 0 = at once. */
		float BloodSpreadSeconds = 1.5f;
		/** @brief Deformation map resolution (1024, 2048 or 4096, snapped to pow2 - the toroidal mask requires it). 4096 was retired 2026-09-21 (trenches looked worse at it, 4.71 ms) and is back since 2026-10-15 at Josef's call: the default and the High preset. The performance side of trench detail: cost scales quadratically (S0: 0.29 / ~1.1 ms full-map at the anchor), texel size scales with it and with the Trenches range. Applies like a range change: recreate + clear, the store re-injects. Promoted from the S0 debug combo once S3 made it a real perf lever. */
		uint32_t DeformMapResolution = 4096;
		/** @brief Runs the map at 2048 for the session once the adapter's free video memory budget is smaller than what 4096 costs over 2048: past the budget the driver demotes textures to system memory and frames stall on paging. DeformMapResolution keeps its value. */
		bool VRAMAwareTrenchMap = true;
		/** @brief Snow Trenches on Objects: the print map's texels a side (512, 1024 or 2048) at kPrintTexel units each, so the reach around the player is 11, 22 or 44 m. A quality-preset lever. */
		uint32_t ObjectTrenchResolution = 2048;
		/** @brief "Object Print Rim": how far past a print's edge, in units (0-15), the snow on an object is broken up: the trench detail reaches this far at twice Detail Relief, fading out. 0 = the print's own edge only. */
		float ObjectPrintRimWidth = 7.5f;
		/** @brief Render distances in meters (converted via kUnitsPerMeter). The shell itself auto-sizes to the loaded-cell grid (no slider); Trenches resizes the deformation window and clears the map on apply (content is scale-relative). */
		float RangeTrenchesM = 200.0f;
		/** @brief Camera distance (m) past which NPCs, creatures and corpses do not carve; the player always does. Bounded by RangeTrenchesM. */
		float RangeActorTrenchesM = 200.0f;
		float RangeSkinsM = kMaxRangeSkinsM;
		/** @brief "Object Height Map Range" (m): half-width of the top-down object height window (ObjectRasterHalfExtent). Read by the ground snow's shelter under cover (the near mask), the road patch and road skins (PatchTop, ObjectConeDepth; past it only the far road level), road stamps (ContactAbove), the object-print top gate (PrintTopGap: below the print map's reach it clips the prints) and the volume seed's shelter and sky maps. Fixed 2048 texels, so this trades texel size against reach: 58 m = 4 units a texel, 117 m = 8. The near level stays a quarter of it. */
		float ObjectRasterReachM = 100.0f;
		/** @brief Rasterizer depth bias for the object skins, in depth-buffer ULPs toward the camera (D3D11 DepthBias, negated). Replaces the decal viewport cap's accidental ~500-ULP push, which let a skin beat its own mesh at range but stood a peak 2000 units in front of its mist. */
		float SkinDepthBias = 64.0f;
		/** @brief Slope-scaled part of the same bias (D3D11 SlopeScaledDepthBias, negated): grows on grazing faces. Default 0: the constant term alone settled the contest with no visible cost. */
		float SkinSlopeDepthBias = 0.0f;
		/** @brief Road-snow tessellation cap: no generated edge segment shorter than this many screen pixels. Only road skins tessellate (S4 skins draw untessellated), so this caps how finely road snow is subdivided: higher = cheaper, 0 = off. The hull's base rule already targets ~20 px, so values below that trim only the rim-roll term (1-unit segments, sub-pixel past ~500 units); larger values coarsen the whole surface. A quality-preset lever. */
		float SkinTessCapPx = 16.0f;
		/** @brief Ground snow slope drape, 0-1: how far the layer's stand-off turns from straight up toward the ground normal as the slope steepens (30 to 60 degrees). A vertical-only offset thins to depth*cos(slope) and steep faces show through. Read through SHELL_DRAPE by the ground snow's vertices and its sun caster only. */
		float SlopeDrape = 1.0f;
		/** @brief LOD-diffuse snow classification: 0 = only bright white counts, 1 = pale gray already counts. */
		float LODSnowSensitivity = 0.5f;
		/** @brief Horizon snow: recolor the game's LOD terrain with the shell's snow material wherever its bake classifies as snow. */
		bool HorizonSnow = true;
		/** @brief "Recolor Projected Snow" (SKIN-PLACEMENT-PLAN S3, round 11): projected snow wears the shell's snow set (albedo + PBR response) inside the object's own Lighting draw, on draws whose projected material is snow - every angle by construction. All or nothing: every pixel the game paints at all takes full shell-snow weight (Snow Fill retired 2026-09-10).The flat-shell GEOMETRY experiments (rounds 4-10) are retired - the recolor has the real weight, nothing to reconstruct, no geometry to miss. */
		bool ProjSnowMatch = true;
		/** @brief "Seasons Snow Max Angle" (degrees): runtime-applied projections (Seasons of Skyrim) carry the record default of 90 and no vertex-alpha mask; this is the max slope such snow rests on, applied in Lighting to those draws only. 90 = Seasons' own coverage; the default 87 cuts only near-vertical faces. */
		float SeasonsSnowMaxAngle = 87.0f;
		/** @brief "Multipass Snow Follows Paint": objects whose snow material is multipass (vanilla glaciers and ice, Simplicity of Snow, Stretched Snow Begone) get their base pass marked "known, unpainted" for the skin's read-back, so the drape follows only the snow the game's own snow pass paints. Off = the skin reconstructs a coat over every face of them (Alpha Test 2 behaviour). */
		bool MultipassSnowFollowsPaint = true;
		/** @brief "Recolor Snow-Textured Meshes": shapes whose diffuse is a snow texture but which carry no projection (dirt cliffs' snow01 tops, a season swap's alternate sets, drifts) take the baked-snow recolor in Lighting by texel, their skins take the coat, and the written weight reaches the volume. Unprojected shapes only; the projected-pass texel floor that shipped with the 2026-09-14 A/B is not part of it. */
		bool SnowTexturedRecolor = true;
		/** @brief "Recolor Projected Snow Normals" (2026-09-22): recolored projected snow also wears the shell snow's NORMAL, perturbed the way the drape's coat does. Vanilla's own projected normal never reaches the G-buffer (Lighting builds worldNormal before the projected block; the block's normal write is dead, upstream included), so without this every projected-snow pixel the coat does not cover wears the object's bump and reads as snow painted on. Rides the snow verdict. */
		bool ProjSnowNormals = true;
		/** @brief "Unpainted Shapes Stay Bare" (2026-09-22): a shape of a snow-MATO object that the game draws without projection writes the drape's read-back mask as "known, unpainted", instead of leaving 0 there and having the drape read "unknown" as licence to coat. Found with XPMF, which strips the projected flag from one shape of a building; the shape then overdraws its projected siblings' verdict. Positive snow MATOs only, so mesh replacers and drifts keep the reconstruct path. */
		bool UnprojectedShapesBare = true;
		/** @brief "Snow Follows Parallax" (A/B, 2026-10-22): on an object drawn with Extended Materials parallax, Lighting reads the projection's noise where the parallax drew the texel instead of on the mesh, so the projected snow's edge, and the drape's coat that copies it, stays on the drawn surface as the camera moves. Shapes on the vanilla parallax technique paint nothing and keep the coat's reconstruction on the mesh. */
		bool SnowFollowsParallax = true;
		/** @brief "Flatten Parallax Under Snow" (A/B, 2026-10-22): on an object with projected snow, Extended Materials' parallax fades as the projection covers the surface (its noise-free weight, judged in Lighting before the parallax runs), flat where the snow lies solid, so the snow, its coat and its prints lie on a surface that does not move. Snow fills the cracks. Stacks with SnowFollowsParallax, which then follows the relief that stays; the coat's march takes the same share. */
		bool FlattenParallaxUnderSnow = true;
		/** @brief "Shelter Max Height" (units): a roof, bridge or archway whose underside is higher than this above the ground no longer thins the snow beneath it; a wide arch with plenty of air is as open as the sky. */
		float ShelterMaxHeight = 150.0f;
		/** @brief "Snow Drift Angle" (degrees from vertical). Cover whose underside sits between ShelterMaxHeight and kShelterBandHeight also shelters, as far as the spot is enclosed (covered share of a disc of radius kShelterBandHeight * tan(ShelterDriftAngle)): tents, not overhangs. Higher asks for deeper cover before the band shelters. */
		float ShelterDriftAngle = 30.0f;
		/** @brief "Object Meeting Blend" (units): thickness of snow over a captured object across which the landscape sheet and an object's coat meet, half on each side of the junction: the sheet's macro slope goes halfway to the object's over its half, the coat's normal halfway to the sheet's over the other. 0 = off. */
		float ObjectMeetBand = 12.0f;
		/** @brief "Recolor Baked LOD Snow": plain object-LOD batches (DynDOLOD's unflagged 'obj' shapes: drifts, roads, piles beyond the loaded grid) take the horizon recolor wherever their atlas texel reads as snow. RenderDoc 2026-09-06: no road capture past 7,538 units, snow-flagged LOD skinned to 70,000 - the far roads and drifts were these batches. */
		bool LODObjectSnow = true;
		/** @brief "Volume Snow" (VOLUME-SNOW-PLAN V0-V2): rasterise the captured statics into the clipmap's voxel occupancy volumes, grow the snow field on them and draw it. One switch for build and draw (Josef, 2026-09-07); the slice view stays available under it. */
		bool VolumeSnow = false;
		/** @brief "Volume Snow Depth", world units: the snow top over a surface, the same on every ring - the field crosses Coverage at it, so a coarse ring's voxel size no longer sets a floor. Josef's tuned default. */
		float VolumeSnowDepth = 10.0f;
		/** @brief "Volume Snow Coverage": the field's value at the snow top and its crossing, 0.02..0.5. Lower lets a lip survive more dilution (longer overhangs, thin things kept); it does not change the depth. Josef's tuned default. */
		float VolumeSnowCoverage = 0.15f;
		/** @brief "Volume Snow Overhang", world units: how far past a snow column the snow may reach sideways, a hard cap with a one-voxel ramp - past it the snow only grows up. Josef's tuned default. */
		float VolumeSnowOverhang = 5.0f;
		/** @brief "Volume Edge Rounding", world units: the sideways averaging width - the shoulder over which the snow falls off toward an edge, independent of how far it may reach past it. Josef's tuned default. */
		float VolumeSnowRounding = 5.0f;
		/** @brief "Volume Levels": clipmap levels, each twice the voxel of the one inside it (a full Wide level allocates ~226 MB, see the menu's readout). Reach doubles per level; detail stays the base voxel near the camera. Josef's tuned default. */
		int VolumeLevels = 4;
		/** @brief "Fine Levels": how many levels, from the camera out, keep the Ring Shape's full grid; the rest are halved per axis at twice the voxel - the same reach at an eighth of the voxels, one octave less detail where it is too far to see. */
		int VolumeFineLevels = 6;
		/** @brief "March Step", voxels: the draw's sample spacing along each ray. The hit is refined on the cubic afterwards, so the march only has to find the crossing interval; features thinner than a step can be stepped over. */
		float VolumeMarchStep = 0.25f;
		/** @brief "Volume Detail Distance", world units: inside it the volume snow shades with the full skin material; over the next half of it the parts invisible at range - parallax marching, berm relief, the horizon shadow march - fade out. */
		float VolumeDetailDistance = 1000.0f;
		/** @brief "Skip Empty Cells": the march jumps over the 4^3 sub-cells of a brick that the brick list marked as holding no crossing. */
		bool VolumeSkipEmptyCells = true;
		/** @brief "Volume Forward Bias": how far ahead of the eye each window sits, as a fraction of its extent (0 = centred). Half a cube behind you is air the capture never lists; ahead is where the reach is wanted. */
		float VolumeForwardBias = 0.45f;
		/** @brief "Ring Shape": 0 = cubic (256 voxels a side), 1 = wide (512 x 512 x 256: twice the reach sideways and ahead for four times the voxels), 2 = wide and flat (512 x 512 x 128: the reach of wide at half its voxels and height). Every ring shares the shape, so each still doubles the one inside it in every axis. */
		int VolumeRingShape = 1;
		/** @brief "Volume Vertical Bias": how far BELOW the eye each window's centre sits, as a fraction of its extent. The third-person camera rises when it looks down, and a cube centred on it then puts the ground beside the player outside the nearest ring - which is why the ring colours changed with pitch (Josef, 2026-09-07). */
		float VolumeVerticalBias = 0.0f;
		/** @brief "Volume Max Distance", world units: past it the volume snow dithers out over a quarter of that distance and the object snow carries the object alone. The outermost rings sample at their coarsest voxel; there is a range at which the object snow simply looks better. */
		float VolumeMaxDistance = 8000.0f;
		/** @brief "Staggered Capture": a record already in a level's volume is re-rasterised only every 8th rebuild on the finest ring (4th, 2nd on the next two), the memory nibble keeping it alive between; a level starting from nothing captures everything. New records wait at most that long. Off captures every record every rebuild, for comparing. */
		bool VolumeStaggeredCapture = true;
		/** @brief "Dirty Bricks": a rebuild recomputes the field only in the brick columns whose occupancy changed (plus the blur's reach around them); the rest keeps last time's. Off rebuilds every column every time, for comparing. */
		bool VolumeDirtyBricks = true;
		/** @brief "Volume Voxel Size", world units, of the finest level; each further level doubles it. Josef's tuned default. */
		float VolumeVoxelSize = 1.0f;
		/** @brief "Volume Snow Max Slope": surfaces steeper than this grow no volume snow - the object's own normal, packed into the voxel at capture. Josef's tuned default. */
		float VolumeSnowMaxSlopeDeg = 69.0f;
		/** @brief "Volume Sky Exposure": strength of the sky-openness weighting on the seed, percent. Josef's tuned default: off. */
		float VolumeSkyExposurePct = 0.0f;
	};

	/** @brief GPU-side settings, appended to the shared FeatureData cbuffer (b6). Layout must match SnowDeformationSettings in SharedData.hlsli. */
	struct alignas(16) SettingsGPU
	{
		float2 WindowOrigin;
		float InvWorldSize;
		/** @brief Settings::SnowFollowsParallax as 0/1 (was a pad). Mirror in SharedData.hlsli. */
		float ParallaxFollow;

		uint DebugTerrainOverlay;
		/** @brief Horizon snow (LOD terrain recolor): distance ramp start and 1/band, aligned to the shell edge. */
		float LODReplaceStart;
		float LODReplaceFadeInv;
		float LODSnowSensitivity;

		float SnowIsLinear;
		float SnowRoughnessScale;
		float LODReplaceEnable;
		/** @brief Snow normal map bound at t23 (0 = legacy set without one). */
		float SnowHasNormal;

		/** @brief Snow-textured mesh recolor (Settings::SnowTexturedRecolor) enabled and the snow set is bound; the old LODReplaceLegacy slot. Mirror in SharedData.hlsli. */
		float SnowTexturedEnable;
		/** @brief Projected-snow material match enabled and the snow set is bound. */
		float ProjSnowEnable;
		/** @brief Baked-snow (glacier) material match enabled and the snow set is bound. */
		float LODObjectEnable;
		/** @brief cos(SeasonsSnowMaxAngle): the slope cut Lighting applies to the vertex alpha of SnowProjectedUnauthored draws, in place of the mask the record lacks. Mirror in SharedData.hlsli. */
		float ProjUnauthoredThreshold;

		/** @brief Toroidal deformation-map addressing for Lighting's GetDeformation: physical position of logical texel (0,0). Mirror in SharedData.hlsli. */
		DirectX::XMINT2 DeformMapOrigin;
		/** @brief Snow Climate x wetness at the player, for the wet print on the land's recoloured snow (SnowLighting.hlsli LandRecolorBegin). Took the torus pad. Mirror in SharedData.hlsli. */
		float LandWetPrint;
		/** @brief Snow Trenches on Objects: the stored depth of a full print, BodyPrintScale(); 0 = no print goes bare. Was a pad. Mirror in SharedData.hlsli. */
		float PrintFullDepth;

		/** @brief Water raster (t63) frame for Lighting's underwater veto: world xy of texel (0,0) and the texel size; Dim 0 = no raster this frame. Mirror in SharedData.hlsli. */
		float2 WaterWindowOrigin;
		float WaterWindowTexel;
		float WaterWindowDim;

		/** @brief Ice at the player (SnowIceShown, row 1), for the glaze on the land's recoloured snow. Was a pad. Mirror in SharedData.hlsli. */
		float LandIce;
		/** @brief Settings::ProjSnowNormals as 0/1. Mirror in SharedData.hlsli. */
		float ProjSnowNormalEnable;
		/** @brief The snow set's specular level (F0 scale), for the land recolor. Mirror in SharedData.hlsli. */
		float SnowSpecularLevel;
		/** @brief Land Snow Recolor: luminance where a snow layer's texel starts to read as snow. Mirror in SharedData.hlsli. */
		float LandRecolorLumLo;

		/** @brief Snow Trenches on Objects: the world print texels [xy, zw) the print map (t114) holds this frame; empty = nothing to read. Mirror in SharedData.hlsli. */
		DirectX::XMINT4 PrintRect;

		/** @brief Settings::FlattenParallaxUnderSnow as 0/1. Mirror in SharedData.hlsli. */
		float ParallaxFlatten;
		float padParallaxFlatten[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SettingsGPU);

	/**
	 * @brief Returns this frame's GPU settings for the shared FeatureData buffer.
	 *
	 * Also advances the deformation window when a_inWorld is true. The origin
	 * must be computed here (during State::UpdateSharedData, before Prepass) so
	 * the constant buffer and the scrolled texture agree within a frame.
	 */
	SettingsGPU GetCommonBufferData(bool a_inWorld);

	/** @brief Per-dispatch constants for the deformation update. Layout must match PerFrame in DeformationUpdateCS.hlsl. */
	struct alignas(16) PerFrame
	{
		float2 WindowOrigin;
		/** @brief Toroidal store: physical position of logical texel (0,0). A scroll advances this and RingCS rewrites the reassigned band; nothing else moves. */
		DirectX::XMINT2 MapOrigin;

		float TexelSize;
		uint StampCount;
		float RefillAmount;
		uint ClearMap;

		/** @brief Lower smoothstep edge of the stamp falloff (fraction of radius): higher = steeper trench walls. */
		float StampFalloffStart;
		/** @brief BermReachScale() (Berm Steepness): the berm bake's tap reach and the dirty scan's halo. Took the retired TrailIrregularity slot. Mirror in DeformationUpdateCS.hlsl AND BermFieldCS.hlsl. */
		float BermReach;
		/** @brief Unit wind direction (world XY, blowing toward) times wind strength 0-1; zero = uniform refill. */
		float2 WindBias;

		/** @brief Seconds this frame. Melt accumulates per second, not per frame, so the bowl a heat source digs does not depend on framerate. */
		float DeltaTime;
		/** @brief Settings::MeltPersistence, the refill slowdown on melted ground. */
		float MeltPersistence;
		/** @brief Settings::MeltBowlFloor, the smoothstep start of the melt flank. */
		float MeltFloorStart;
		/** @brief Settings::MeltEdgeIrregularity, the fraction the melt radius wobbles by. */
		float MeltEdgeNoise;
		/** @brief Settings::CrustPrintDepth, how deep a boot still prints on fully crusted snow. */
		float CrustPrintDepth;
		/** @brief Crust lost per second regardless of weather - ice answers to temperature, not to snowfall. */
		float CrustThaw;
		/** @brief How completely a carve destroys the crust it cuts through, against the depth of the cut. */
		float CrustBreakOnCarve;
		/** @brief Settings::SlumpRate, the unsupported-snow settle speed; 0 disables the pass. Claimed the old pad, so the layout is byte-identical. */
		float SlumpRate;

		/** @brief 1 = InjectStore holds the tile store's memory of texels arriving from outside the window. Its own row: Stamps must start 16-byte aligned. */
		uint InjectValid;
		/** @brief The frame's span in GAME time, expressed in the seconds DeltaTime is measured in. Equal to DeltaTime during ordinary play; a wait or a sleep passes hours without rendering them, and the world's own clocks (glaze thaw, slump) must not sit those hours out. Stamp application deliberately keeps DeltaTime - a fire must not carve its whole basin in the single frame after a wait. Claimed a pad slot, so the layout is byte-identical. */
		float GameDeltaTime;
		/** @brief 1 = the CS paints the per-texel activity view (u2): which texels changed at stored precision, and in which channel. Claimed a pad slot, layout unchanged. */
		uint DebugActivityView;
		/** @brief The ageing clock (SnowClimateState::ageingClock): StampCS dates a cut texel with it, RingCS dates arriving ones from the store's freshness, and the berm bake reads ages against it. Claimed InjectPad, layout unchanged. Mirror in DeformationUpdateCS.hlsl AND BermFieldCS.hlsl. */
		float AgeingClock;

		/** @brief Arriving-band rects in LOGICAL texel space (x0, y0, w, h); RingCS covers their union by flat index. Two at most: one band per scrolled axis, or one full-map rect on a clear. */
		DirectX::XMINT4 RingRects[2];
		uint RingRectCount;
		uint RingTotalTexels;
		/** @brief 1 = the tile scans list every tile (the force-all-dirty cross-check). Claimed a pad slot, layout unchanged. */
		uint ForceAllDirty;
		/** @brief Clock units the ageing clock fell back by since the last executed update: TrackRebaseCS moves every date with it. Claimed RingPad, layout unchanged. */
		float TrackRebase;

		float4 Stamps[kMaxStamps];
		/** @brief Capsule segment start per stamp (the stamped shape's previous position). */
		float4 StampEnds[kMaxStamps];
		/** @brief Cone stamps only: x final reach from the apex, y widest half-width, z churn seed. Zero for every other stamp. */
		float4 StampCones[kMaxStamps];


		float2 ContactCenter;
		float ContactHalfExtent;
		float ContactDim;
		float2 TerrainWindowOrigin;
		float TerrainTexelSize;
		float TerrainDim;
		/** @brief Debug view crop centre (the player's bound centre), world XY. */
		float2 ViewCenter;
		/** @brief Debug view crop half-extent, world units. */
		float ViewHalf;
		/** @brief BodyPrintScale() (Snow Resistance): the share of its reach a body's contact carves; loose props carve all of theirs. Claimed ViewPad, layout unchanged. */
		float BodyPrint;
		/** @brief Object height window (RenderObjectHeightMap) the road raster at t9 was captured in; HasRoadRaster = 0 when it is not bound. */
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		float HasRoadRaster;
		/** @brief Body passes: the pass clock in half seconds (mod 1024), and the free time in the same units after which contact starts a new pass. Mirror in DeformationUpdateCS.hlsl. */
		uint PassClock;
		uint PassGap;
		uint PassPad[2];
		/** @brief Shout mark outline and depth along the reach: x ShoutMarkWaist, y ShoutMarkRoundness, z ShoutMarkFadeStart, w ShoutMarkFarDepth. */
		float4 ShoutShape;
		/** @brief Shout mark churn: x ShoutChurn, y ShoutStreakSize, z ShoutStreakLength, w spare. */
		float4 ShoutChurn;
		/** @brief The ground snow as drawn, which the body carve measures against: the shell's UndulationAmp, UndulationScale and accumulation depth scale (RimStyle.w). Mirror in DeformationUpdateCS.hlsl. */
		float UndulationAmp;
		float UndulationScale;
		float AccumulationScale;
		float DrawnPad;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(offsetof(PerFrame, ContactCenter) == 12432, "PerFrame tail must match the fxc-reflected layout of DeformationUpdateCS");
	static_assert(offsetof(PerFrame, UndulationAmp) == 12544, "PerFrame tail must match the fxc-reflected layout of DeformationUpdateCS");

	/** @brief Lower smoothstep edge of a capsule stamp's falloff, in percent of its radius, before fresh snow and Snow Climate ease it (PerFrame::StampFalloffStart): 0 = the softest, widest banks; 100 = full depth to the edge. */
	static constexpr float kTrenchWallSharpness = 66.0f;

	Settings settings;

	/** @brief Seconds for compressed snow to fully recover at 1.0 snowfall intensity and a 1.0x multiplier. */
	static constexpr float kBaseRefillTime = 700.0f;
	/** @brief Precipitation particle density that maps to 1.0 snowfall intensity (vanilla densities run ~1-3). */
	static constexpr float kReferenceSnowDensity = 2.0f;
	/** @brief Intensity ceiling; keeps extreme weather-mod densities from erasing trails outright. */
	static constexpr float kMaxSnowfallIntensity = 4.0f;

	/** @brief Snowfall intensity from the weather records' precipitation density, faded across weather transitions. 1.0 = reference-density snowfall; 0 for ash and in a No Sky worldspace. */
	float ComputeSnowfallIntensity() const;
	/** @brief The same transition-faded precipitation density for any weather class (snow, rain). */
	float ComputePrecipitationIntensity(RE::TESWeather::WeatherDataFlag a_flag) const;
	/** @brief The player's worldspace when it is flagged No Sky (Blackreach), else null. Outdoor weather state holds there as it does indoors. */
	static RE::TESWorldSpace* NoSkyWorldspace();
	/** @brief The first kNotSnowKeywords entry (the MATO and LOD batch vetoes' list) found in a name, case-insensitive, or null. */
	static const char* NotSnowKeywordIn(const char* a_name);
	/** @brief Last computed snowfall intensity, for the debug readout. */
	float snowfallIntensity = 0.0f;

	/**
	 * @brief One game-time reading per frame, shared by every subsystem that
	 * integrates it (trench decay, accumulation). Render-thread owned.
	 *
	 * Shared, not duplicated: a second copy would tie the accumulator's clock to
	 * whether trenches are enabled, and two clocks on one calendar drift. Not
	 * the spell system's drift accumulator, which corrects render-second timers.
	 */
	struct GameClock
	{
		/** @brief Last calendar reading in hours; negative until armed. Elapsed hours telescope, so the calendar's float32 day quantisation cancels rather than accumulating. */
		float lastHours = -1.0f;
		/** @brief Last plausible timescale. Waiting cranks the live one enormously for its animation, so a reading taken then is the wait and not the player's setting. */
		float timescale = 20.0f;
		/** @brief Game hours since the previous reading. Zero on the arming frame and on a backwards jump. */
		float elapsedHours = 0.0f;
		/** @brief The calendar went backwards, which means a save was loaded: this timeline is not the one the consumers' state belongs to. */
		bool reversed = false;
	};
	GameClock gameClock;
	/** @brief Set by the co-save callbacks on the GAME thread; consumed by the next tick. An unarm request rather than a direct write, so the clock itself stays render-thread owned. */
	std::atomic<bool> gameClockUnarm{ false };
	/** @brief The clock's reading published for game-thread readers (the co-save writers), which must not reach into the struct. */
	std::atomic<float> gameClockHours{ -1.0f };
	/** @brief Takes this frame's reading. Call once, before anything that consumes it. */
	void TickGameClock();

	ConstantBuffer* perFrame = nullptr;
	/** @brief [0] = THE map (single canonical texture, toroidal layout); [1] = EvolveCS's snapshot scratch, copied from the map just before that pass. The ping-pong is retired: consumers always read [0]. */
	Texture2D* deformationTextures[2] = { nullptr, nullptr };

	/** @brief Baked berm field: the 17-tap disc average of the deformation map, rebuilt from the current map every frame so the shells read it with one bilinear tap instead of 68 loads per call. */
	Texture2D* bermFieldTexture = nullptr;

	/** @brief SRV of the deformation map (the single canonical texture), for shader sampling and debug UI. Physical (toroidal) layout - readers translate through DeformMapOrigin. */
	ID3D11ShaderResourceView* GetDeformationSRV() const { return deformationTextures[0] ? deformationTextures[0]->srv.get() : nullptr; }
	/** @brief SRV of the baked berm field; null before SetupResources. */
	ID3D11ShaderResourceView* GetBermFieldSRV() const { return bermFieldTexture ? bermFieldTexture->srv.get() : nullptr; }

	/** @brief The body pass each map texel last took (R16_UINT, physical layout): the carve it started from and when bodies last touched it. StampCS u7 only. */
	Texture2D* passStateTexture = nullptr;
	/** @brief The deepest a body has reached into the layer at each map texel (R8_UNORM, physical layout; 0 = none since the ground arrived). StampCS raises it and RingCS clears what it reassigns (u5); the shells measure trampled ground against it (kBodyReachSlot). */
	Texture2D* bodyReachTexture = nullptr;
	/** @brief When each map texel was last cut, on the ageing clock (R16_FLOAT, physical layout; 0 = never). StampCS dates cuts that reach the floor and RingCS arriving ground from the store (u4); the berm bake turns it into the field's dates (t2); TrackRebaseCS moves it with the clock. */
	Texture2D* trackClockTexture = nullptr;
	ID3D11ComputeShader* trackRebaseCS = nullptr;
	ID3D11ComputeShader* GetTrackRebaseCS();
	/** @brief Ageing-clock units an old track takes to age fully at Track Ageing 1. */
	static constexpr float kTrackAgeSpan = 24.0f;
	/** @brief ShellCB::TrackAge: the ageing clock now and the age per clock unit (Track Ageing / kTrackAgeSpan; 0 = tracks never age). */
	float4 TrackAgeConstants() const;
	/** @brief The ageing clock in the frame the track dates stand in now: ahead of ageingClock by a rebase the next update has yet to apply. */
	float TrackDateClock() const { return snowClimate.ageingClock + snowClimate.ageingRebase; }
	/** @brief Mirrors the unwrapped ageing clock into the store, under its lock; after TickSnowClimate. */
	void TickTrenchAgeing();
	/** @brief Ageing-clock units a date stays its own before it reads as ancient: the store's freshness span and the berm bake's clamp. Mirrored by TRACK_AGE_HORIZON (DeformationUpdateCS.hlsl) and kTrackAgeHorizon (BermFieldCS.hlsl). */
	static constexpr float kTrackAgeHorizon = 128.0f;
	/** @brief Real seconds the body-pass clock has run; PerFrame::PassClock is it in half seconds, mod 1024. */
	double passClockSeconds = 256.0;
	/** @brief PS slot of the untouched-tile mask (BermFieldCS UntouchedCS), read by the shell PS's skip. */
	static constexpr UINT kUntouchedTilesSlot = 57;
	/** @brief PS slot of the body reach (TrampleDetailScale), bound by DrawShell for the shell and the trench patch. */
	static constexpr UINT kBodyReachSlot = 56;
	/** @brief SampleTerrain's terrain window (TerrainWetSRV), bound wherever the shell's t0 is. */
	static constexpr UINT kTerrainWetSlot = 58;


	// ---- Blood on snow (Blood.cpp; BLOOD-DESIGN.md) ----
	/** @brief Blood map beside the deformation map: same dim, same toroidal origin. R8G8B8A8 sRGB: rgb = pigment, a = concentration. */
	Texture2D* bloodMapTexture = nullptr;
	/** @brief Per-texel deposit clocks, R32G32: x = bloodBurialClock, y = game hours at deposit. The shells fade by both; nothing evolves the map. */
	Texture2D* bloodClockTexture = nullptr;
	ID3D11ShaderResourceView* GetBloodMapSRV() const { return bloodMapTexture ? bloodMapTexture->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetBloodClockSRV() const { return bloodClockTexture ? bloodClockTexture->srv.get() : nullptr; }
	/** @brief The map may hold a deposit: set by a capture or disc draw; cleared with the map, or once the window misses every window a deposit landed in (the ring zeroes what leaves). Clear = BloodLook2.z 0. */
	bool bloodMapWritten = false;
	/** @brief Union (min x, min y, max x, max y) of the windows deposits landed in since bloodMapWritten was set. */
	float4 bloodWrittenBox = {};
	/** @brief DevBench A/B: true keeps the map live whenever blood is on, as before the latch. */
	bool bloodLatchOff = false;
	/** @brief Sum of RefillAmount over the session: the snowfall clock a deposit is dated by. */
	float bloodBurialClock = 0.0f;
	/** @brief Smallest refill applied in one dispatch: two R16F steps just below 1.0, so it always moves a stored value. */
	static constexpr float kRefillStep = 1.0f / 1024.0f;
	/** @brief Refill accumulated since the last dispatch that applied any. */
	float refillBank = 0.0f;
	/** @brief A blood draw seen by the capture hook this frame, drawn into the map after the object height raster. */
	struct BloodCapture
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		winrt::com_ptr<ID3D11ShaderResourceView> diffuse;
		float4 texcoord;
		float alpha;
		float alphaThreshold;
		float reveal;
		bool skinned;
		/** @brief Not blood: the overlay cannot read its alpha off the colour, so it is drawn into the decal mask. */
		bool mask = false;
		/** @brief Burial clock and game hours when the mark was first seen; the detail tiles date it by these, so a redraw does not make it fresh again. */
		float2 clock{ 0.0f, 0.0f };
		/** @brief The rune atlas's id for this decal in its tile (Spread.w): which texels share its uv plane. */
		float decalId = 0.0f;
	};
	std::vector<BloodCapture> bloodCaptures;
	/** @brief Engine decals are baked geometry: one deposit per decal. Keyed by geometry, validated by its buffer and position. */
	struct BloodSeen
	{
		uint32_t frame = 0;
		const void* vb = nullptr;
		RE::NiPoint3 position;
		double firstSeconds = 0.0;
		float2 firstClock{ 0.0f, 0.0f };
		/** @brief Detail tiles: bloodTileEpoch when every tile under the mark last took it whole; a tile allocated since is owed a redraw. */
		uint32_t fineEpoch = 0;
		/** @brief A cell under the mark had no slot to be had; asked again at fineRetryFrame. */
		bool fineMissing = false;
		uint32_t fineRetryFrame = 0;
		/** @brief World XY bounds of the decal's vertices, read once. */
		bool boundsKnown = false;
		float2 boundsMin{ 0.0f, 0.0f };
		float2 boundsMax{ 0.0f, 0.0f };
	};
	double bloodRenderSeconds = 0.0;
	std::unordered_map<const void*, BloodSeen> bloodSeen;
	/** @brief API disc deposits queued from any thread, drawn once. */
	struct BloodDisc
	{
		float x, y, z, radius;
		float r, g, b, amount;
	};
	std::vector<BloodDisc> bloodDiscQueue;
	std::mutex bloodDiscMutex;
	std::unordered_set<std::string> bloodPathsLogged;
	/** @brief Every blood decal drawn this frame, once each, for the overlay that redraws it on the snow that covered it. */
	std::vector<BloodCapture> bloodOverlays;
	std::unordered_set<const void*> bloodOverlaySet;
	/** @brief G-buffer targets the overlay reads: lit diffuse, normal + gloss, albedo, specular, reflectance (RT 0, 2, 3, 4, 5), copied over the decals' screen rectangle before the frame's first blood decal drew (the capture hook) and before any snow drew (Shell.cpp). Valid for one frame. */
	static constexpr uint32_t kBloodCopyCount = 5;
	winrt::com_ptr<ID3D11Texture2D> bloodPreDecalTex[kBloodCopyCount];
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreDecalSRV[kBloodCopyCount];
	winrt::com_ptr<ID3D11Texture2D> bloodPreSnowTex[kBloodCopyCount];
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreSnowSRV[kBloodCopyCount];
	/** @brief Masks before the frame's first blood decal drew: the game's decal writes Masks.y = 0 under itself, which the coat reads as no paint and leaves a hole; LandMasksCopy takes this over the rectangle instead. */
	winrt::com_ptr<ID3D11Texture2D> bloodPreDecalMasksTex;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreDecalMasksSRV;
	bool bloodPreDecalValid = false;
	bool bloodPreSnowValid = false;
	/** @brief Screen rectangle (render-resolution pixels) of this frame's blood decals so far, last frame's whole, and the one the copies cover: the pre-decal copy is taken at the first decal, so it covers last frame's rectangle grown a little plus that decal; a decal outside it waits a frame. */
	RECT bloodRectFrame{};
	bool bloodRectFrameValid = false;
	RECT bloodRectPrev{};
	bool bloodRectPrevValid = false;
	RECT bloodCopyRect{};
	D3D11_VIEWPORT bloodViewport{};
	bool BloodScreenRect(const RE::BSGeometry* a_geometry, RECT& a_rect) const;
	void CopyBloodTargets(bool a_preDecal);
	ID3D11VertexShader* bloodOverlayVS = nullptr;
	ID3D11PixelShader* bloodOverlayPS = nullptr;
	winrt::com_ptr<ID3D11BlendState> bloodOverlayBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> bloodOverlayDepthState;
	uint32_t bloodOverlaysLast = 0;
	/** @brief Draws this frame's blood decals over the object-snow coat, into the skin pass's targets. Called at the end of the skin pass. */
	void DrawBloodOverlay(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_postSkinDepth);
	struct alignas(16) BloodCB
	{
		float4 WorldRow0;
		float4 WorldRow1;
		float4 WorldRow2;
		float2 WindowOrigin;
		float TexelSize;
		float MapDim;
		DirectX::XMINT2 MapOrigin;
		float2 ClockNow;
		float4 TexcoordOffset;
		float Intensity;
		float AlphaThreshold;
		float MaterialAlpha;
		float NormalZMin;
		/** @brief x = reveal 0..1 (how much of the mark's alpha range has spread in). y: overlay = leave the shell's pixels, mask = not blood. z: overlay = the mask's cover is live. w spare. */
		float4 Spread;
		/** @brief Overlay: the copied screen rectangle in NDC (x0, y0, x1, y1). */
		float4 OverlayRect;
		/** @brief Mask: the game's view-projection rows (camera-relative) and CameraPosAdjust. */
		float4 ViewProjRow0;
		float4 ViewProjRow1;
		float4 ViewProjRow2;
		float4 ViewProjRow3;
		float4 CameraAdjust;
		/** @brief Overlay: x = the focal length in pixels, yz = the principal point in pixels, w spare. */
		float4 OverlayView;
	};
	STATIC_ASSERT_ALIGNAS_16(BloodCB);
	struct alignas(16) BloodSkinCB
	{
		float4 BoneRows[240];
		float SkinBoneCount;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(BloodSkinCB);
	ConstantBuffer* bloodCB = nullptr;
	ConstantBuffer* bloodSkinCB = nullptr;
	ID3D11VertexShader* bloodVS = nullptr;
	ID3D11VertexShader* bloodSkinVS = nullptr;
	ID3D11VertexShader* bloodDiscVS = nullptr;
	ID3D11PixelShader* bloodPS = nullptr;
	ID3D11PixelShader* bloodDiscPS = nullptr;
	winrt::com_ptr<ID3DBlob> bloodVSBlob;
	winrt::com_ptr<ID3DBlob> bloodSkinVSBlob;
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> bloodILCache;
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> bloodSkinILCache;
	winrt::com_ptr<ID3D11BlendState> bloodBlendState;
	winrt::com_ptr<ID3D11RasterizerState> bloodRasterState;
	winrt::com_ptr<ID3D11SamplerState> bloodSampler;
	winrt::com_ptr<ID3D11Buffer> bloodDiscBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodDiscSRV;
	static constexpr uint32_t kBloodMaxDiscs = 256;
	ID3D11ComputeShader* bloodRingCS = nullptr;
	ID3D11ComputeShader* GetBloodRingCS();
	bool bloodShadersFailed = false;
	bool bloodResourcesLogged = false;
	bool bloodDepositLogged = false;
	bool bloodOverlayLogged = false;
	bool EnsureBloodResourcesImpl();
	std::vector<RE::NiTransform> bloodPaletteScratch;
	uint32_t bloodDepositsLast = 0;
	uint32_t bloodDiscsLast = 0;
	uint32_t bloodSeenLive = 0;
	void CreateBloodTextures(const D3D11_TEXTURE2D_DESC& a_mapDesc);
	bool EnsureBloodResources();
	void ReleaseBloodShaders();
	/** @brief From the capture hook: a decal-mode Lighting draw whose diffuse is a blood texture. */
	void CaptureBloodDraw(RE::BSRenderPass* a_pass, bool a_skinned);
	/** @brief Draws this frame's captures and discs into the blood map. After the object height raster, before the shells. */
	void RenderBloodCapture();
	/** @brief One list of captures through the rigid and skinned blood shaders; a_rigidVS / a_skinnedVS replace the map's vertex shaders, a_instances the map's four seam instances. */
	uint32_t DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, BloodCB& a_cb, ID3D11VertexShader* a_rigidVS = nullptr, UINT a_instances = 4u, bool a_ownClock = false, ID3D11VertexShader* a_skinnedVS = nullptr);
	/** @brief Joins the frame's overlay set: the decal's screen rectangle, and at the frame's first decal the pre-decal copy. Once per geometry. */
	void RegisterDecalOverlay(const BloodCapture& a_capture);
	/** @brief Whether the overlay can change a pixel of a decal with this world bound: a loaded capture's skin reaches it (the skin cull's sphere plus kDecalOverlayMaxLift), or the ground snow draws (its contest reads the pre-decal Masks under every decal), or volume snow is on. Implemented in Blood.cpp. */
	bool DecalReachesSnow(const RE::NiBound& a_bound);
	/** @brief SnowBloodCapture.hlsl kOverlayMaxLift: the composite keeps pixels whose snow lies at most this far in front of the decal along the view ray. */
	static constexpr float kDecalOverlayMaxLift = 24.0f;
	/** @brief XY bound spheres (x, y, radius) of the loaded captures: [0] this frame so far, [1] last frame. */
	std::vector<float3> coatReachBounds[2];
	/** @brief Both lists on a kCoatReachCell grid over the loaded square plus one cell, each sphere grown by the skin cull's margin and kDecalOverlayMaxLift: one bit row per grid row. Built at the frame's first decal test, topped up from [0] as captures arrive. */
	static constexpr float kCoatReachCell = 256.0f;
	std::vector<uint64_t> coatReachGrid;
	float coatReachOrigin[2] = {};
	int coatReachDim = 0;
	int coatReachWords = 0;
	uint32_t coatReachFrame = UINT32_MAX;
	size_t coatReachMarked = 0;
	void MarkCoatReach(float a_x, float a_y, float a_radius);
	/** @brief DevBench A/B: true registers every decal again while a loaded capture exists anywhere, as before the reach test. */
	bool decalReachGateOff = false;
	/** @brief Decal draws the reach test turned away, this frame and the last finished one. */
	uint32_t decalReachMisses = 0;
	uint32_t decalReachMissesLast = 0;
	/** @brief From the capture hook: one of the engine's runtime decals that is not blood. Overlay only, never the blood map. */
	void CaptureDecalOverlay(RE::BSRenderPass* a_pass);
	bool DecalOverlayWanted() const { return settings.BloodOnSnow || settings.DecalsOnObjectSnow; }
	/** @brief Object snow paints the rune glyphs and frost marks itself where they reach it: when the overlay would otherwise take their decals (Statics.cpp CaptureDecalOverlay's gate). */
	bool CoatPaintsMarks() const { return settings.DecalsOnObjectSnow && settings.ProjSnowMatch; }
	/** @brief The overlay decals' alpha in screen space: x the ones that are not blood, y all of them (the overlay's cover). */
	Texture2D* decalMaskTexture = nullptr;
	/** @brief ClearView over the decals' rectangle when the driver supports it natively; otherwise the whole target is cleared. */
	winrt::com_ptr<ID3D11DeviceContext1> decalMaskContext1;
	bool decalMaskClearView = false;
	bool decalMaskClearViewChecked = false;
	ID3D11VertexShader* decalMaskVS = nullptr;
	ID3D11VertexShader* decalMaskSkinVS = nullptr;
	ID3D11PixelShader* decalMaskPS = nullptr;
	winrt::com_ptr<ID3D11BlendState> decalMaskBlendState;
	bool decalMaskFailed = false;
	bool decalMaskLogged = false;
	std::unordered_set<std::string> decalOverlayNamesLogged;
	uint32_t decalMasksLast = 0;
	bool EnsureDecalMask(uint32_t a_width, uint32_t a_height);
	void APIDepositBlood(float a_x, float a_y, float a_z, float a_radius, float a_r, float a_g, float a_b, float a_amount);

	// ---- Blood detail tiles (BloodTiles.cpp; BLOOD-DESIGN.md "Detail tiles") ----
	static constexpr uint32_t kBloodTileDim = 512;
	/** @brief 64 tiles of 512 x 512 at a quarter unit a texel: an atlas of 8 rows of 8 tiles, one array slice a row (4096 x 512, 10.7 MB with its mips), sized to the rows the live tiles reach. */
	static constexpr uint32_t kBloodTilesAcross = 8;
	static constexpr uint32_t kBloodMaxTiles = kBloodTilesAcross * kBloodTilesAcross;
	static constexpr float kBloodTileTexel = 0.25f;
	static constexpr uint32_t kBloodTileMips = 4;
	/** @brief A tile owns a cell and carries an apron around it, so a filtered tap, mips included, never leaves the tile. */
	static constexpr float kBloodTileApron = 4.0f;
	/** @brief World units of the cell a tile owns: the tile's span less its two aprons, 120. The shell derives the apron back from cell and texel (ShellCB BloodLook3.zw). */
	static constexpr float kBloodTileCell = float(kBloodTileDim) * kBloodTileTexel - 2.0f * kBloodTileApron;
	/** @brief How far from the player ground may hold a tile: 100 m. Past it, and wherever the 64 tiles have run out nearer in, the shell reads the blood map. */
	static constexpr float kBloodTileKeepUnits = 7000.0f;
	static constexpr int32_t kBloodTileKeepCells = 59;
	/** @brief Cell -> slot on a torus. Two live cells must never share an entry, so its side is more than twice the keep distance in cells. */
	static constexpr int32_t kBloodTileIndexDim = 128;
	static_assert(float(kBloodTileKeepCells) * kBloodTileCell >= kBloodTileKeepUnits && 2 * kBloodTileKeepCells + 1 <= kBloodTileIndexDim);
	uint32_t BloodTilesAcross() const { return kBloodTilesAcross; }
	uint32_t BloodTileCount() const { return kBloodMaxTiles; }
	float BloodTileTexel() const { return kBloodTileTexel; }
	float BloodTileCell() const { return kBloodTileCell; }
	int32_t BloodCellOf(float a_world) const { return int32_t(std::floor(a_world / kBloodTileCell)); }
	int32_t BloodTileKeepCells() const { return kBloodTileKeepCells; }
	static constexpr uint32_t kBloodTileBlock = 8;
	/** @brief Blood map texels a side of a tile's snapshot and redrawn raster: the tile's 128 units at the finest map texel (29 m range, 2048 map: 1.98 units) is 65, plus the bilinear pair and a ring. */
	static constexpr uint32_t kBloodTileSnapDim = 80;
	struct BloodTile
	{
		bool live = false;
		int32_t cellX = 0;
		int32_t cellY = 0;
		/** @brief bloodTileEpoch at allocation. */
		uint32_t gen = 0;
		/** @brief Burial clock at the newest deposit: a tile snowed under whole is free. */
		float lastBurial = 0.0f;
		std::vector<BloodCapture> draws;
		bool discs = false;
		/** @brief The deformation window the tile was seeded under, and the map's logical texel its snapshot starts at. */
		float2 seedOrigin{ 0.0f, 0.0f };
		int32_t snapLogical[2]{};
		/** @brief The snapshot spans every map texel the seed read; false only for a map texel finer than kBloodTileSnapDim allows, and then copies are left whole. */
		bool snapValid = false;
	};
	struct BloodFineRequest
	{
		BloodCapture draw;
		const void* key = nullptr;
		bool revealing = false;
	};
	struct alignas(16) BloodTileCB
	{
		int32_t TileTexel[2];
		int32_t TileBlock[2];
		float2 TileWorldMin;
		float FineTexel;
		int32_t Explained;
		float2 WindowOrigin;
		float CoarseTexel;
		int32_t CoarseDim;
		int32_t MapOrigin[2];
		float BurialNow;
		float BurialRefills;
		int32_t MipTexel[2];
		int32_t MipDim;
		int32_t TileRow;
		int32_t SnapTexel[2];
		int32_t SnapLogical[2];
	};
	STATIC_ASSERT_ALIGNAS_16(BloodTileCB);
	enum BloodTileShader : uint32_t
	{
		kBloodTileSeed,
		kBloodTileMergeClock,
		kBloodTileMergePigment,
		kBloodTileMipDown,
		kBloodTileCopyResidue,
		kBloodTileShaderCount
	};
	BloodTile bloodTiles[kBloodMaxTiles];
	std::vector<BloodFineRequest> bloodFineQueue;
	uint32_t bloodTileEpoch = 1;
	uint32_t bloodTilesLive = 0;
	uint32_t bloodTileMergesLast = 0;
	uint32_t bloodTileBoundsRead = 0;
	uint32_t bloodTileBoundsGuessed = 0;
	bool bloodTileIndexDirty = true;
	bool bloodTilesFailed = false;
	bool bloodTilesLogged = false;
	/** @brief Set where the deformation map is cleared whole (worldspace or range change): the tiles' ground is gone. */
	bool bloodTilesDrop = false;
	/** @brief The main depth after the landscape shell and before the object-snow skins, copied over the decals' rectangle on frames the decal overlay will run with tiles live: a pixel the skins left alone is the shell's, and the shell paints its own blood. R32F like preSnowDepth (PreSnowCopyCS). */
	winrt::com_ptr<ID3D11Texture2D> bloodPreSkinDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreSkinDepthSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodPreSkinDepthUAV;
	bool bloodPreSkinDepthThisFrame = false;
	/** @brief bloodPreSkinDepth at a_width x a_height (the skins' viewport extent), recreated when that changes. */
	bool EnsureBloodPreSkinDepth(UINT a_width, UINT a_height);
	/** @brief Per tile, kBloodTileSnapDim a side on the blood map's own texel grid: the map's texels as the tile was seeded (sRGB view), and the highest concentration every mark redrawn into the tile since has deposited there (the map's own shader and blend). */
	winrt::com_ptr<ID3D11Texture2D> bloodTileSnapshot;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileSnapshotSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileSnapshotUAV;
	winrt::com_ptr<ID3D11Texture2D> bloodTileRedrawn;
	winrt::com_ptr<ID3D11RenderTargetView> bloodTileRedrawnRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileRedrawnSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileRedrawnUAV;
	/** @brief One tile's snapshot less what its redrawn marks account for, premultiplied; rebuilt by each merge that redraws a mark. */
	winrt::com_ptr<ID3D11Texture2D> bloodTileResidue;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileResidueSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileResidueUAV;
	/** @brief The tile's own mip chain from its mip 0, through a scratch copy per level. Run with every merge: past a few metres the shell reads the mips, not mip 0. */
	void BuildBloodTileMips(ID3D11DeviceContext* a_context, uint32_t a_slot);
	winrt::com_ptr<ID3D11Texture2D> bloodTileAtlas;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileAtlasSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileAtlasUAV[kBloodTileMips];
	uint32_t bloodTileAtlasRows = 0;
	/** @brief Remakes the pigment atlas with a_rows rows (array slices), cleared, keeping the rows both sizes hold. False when the device refuses; the old atlas then stays. */
	bool SizeBloodTileAtlas(uint32_t a_rows);
	winrt::com_ptr<ID3D11Texture2D> bloodTileClock;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileClockSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileClockUAV;
	winrt::com_ptr<ID3D11Texture2D> bloodTileIndex;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileIndexSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodTileScratch;
	winrt::com_ptr<ID3D11RenderTargetView> bloodTileScratchRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileScratchRawSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodTileScratchClock;
	winrt::com_ptr<ID3D11RenderTargetView> bloodTileScratchClockRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileScratchClockSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodTilePrev;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTilePrevRawSRV;
	winrt::com_ptr<ID3D11Texture2D> bloodTilePrevClock;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTilePrevClockSRV;
	winrt::com_ptr<ID3D11Buffer> bloodTileCB;
	ID3D11ComputeShader* bloodTileCS[kBloodTileShaderCount]{};
	ID3D11ComputeShader* GetBloodTileCS(BloodTileShader a_which);
	bool BloodTilesWanted() const { return settings.BloodOnSnow && !bloodTilesFailed; }
	bool BloodTilesLive() const { return BloodTilesWanted() && bloodTilesLive > 0 && bloodTileAtlasSRV; }
	bool EnsureBloodTileResources();
	void ReleaseBloodTiles();
	void DropBloodTiles();
	/** @brief The mark's world XY bounds: its vertices where the CPU holds them, a guess around its bound otherwise. */
	void BloodMarkBounds(const BloodCapture& a_capture, BloodSeen& a_seen);
	int32_t FindBloodTile(int32_t a_cellX, int32_t a_cellY) const;
	int32_t AllocateBloodTile(int32_t a_cellX, int32_t a_cellY, int32_t a_cameraCellX, int32_t a_cameraCellY);
	void FillBloodTileCB(BloodTileCB& a_cb, uint32_t a_slot) const;
	void SeedBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot);
	void MergeBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot, const std::vector<BloodDisc>& a_discs);
	/** @brief Draws this frame's detail requests into their tiles. Before the blood map takes the same frame's marks, so a new tile is not seeded with them. */
	void RenderBloodTiles(const std::vector<BloodDisc>& a_discs);

	// ---- Rune decals on snow (BURIED-REF-LIFT-PLAN.md) ----
	static constexpr uint32_t kRuneMaxTiles = 4;
	static constexpr uint32_t kRuneTileDim = 512;
	static constexpr float kRuneTileHalf = 110.0f;
	/** @brief Frost marks: every live one on the landscape snow, as a uv plane over the ground fitted from its decal's vertices, in a window round the player of kFrostCells cells kFrostCellSize across, each listing the marks whose round reaches it. */
	static constexpr uint32_t kFrostCells = 64;
	static constexpr float kFrostCellSize = 128.0f;
	/** @brief Distinct frost decal textures the field can show at once. */
	static constexpr uint32_t kFrostTextures = 4;
	/** @brief A frost mark whose centre stands more than this over the landscape snow's top is on an object; the object coat wears it. */
	static constexpr float kFrostDecalAboveSnow = 24.0f;
	struct RuneSite
	{
		uint32_t formID = 0;
		RE::NiPoint3 position;
		std::string decalPath;
		float distSq = 0.0f;
	};
	/** @brief A captured rune or frost decal: its draw, and its material as the game's Lighting shader takes it, which the shells light the mark with. */
	struct RuneCapture
	{
		BloodCapture draw;
		winrt::com_ptr<ID3D11ShaderResourceView> normal;
		/** @brief The glow map of a Glowmap material; null without one. */
		winrt::com_ptr<ID3D11ShaderResourceView> glow;
		/** @brief EmitColor as Lighting receives it: emissive colour x emissive multiplier, in working space under Linear Lighting. */
		float3 emissive{ 0.0f, 0.0f, 0.0f };
		float emissiveMult = 1.0f;
		/** @brief A Glowmap material: its EmitColor lights through the glow map. */
		bool glowMap = false;
		/** @brief SpecularColor x its scale (0 without the Specular flag), in working space under Linear Lighting; w = specular power. */
		float4 specular{ 0.0f, 0.0f, 0.0f, 0.0f };
		RE::NiPoint3 centre;
		float radius = 0.0f;
	};
	struct alignas(16) RuneCB
	{
		/** @brief x = live tiles. */
		float4 RuneParams;
		/** @brief xy = tile's world min, z = 1 / world size, w = 1 where its material has a glow map. Tile i sits at (i & 1, i >> 1) of the 2x2 atlas. */
		float4 RuneRects[kRuneMaxTiles];
		/** @brief rgb = the decal material's EmitColor, w = 1 when the tile has a normal map. */
		float4 RuneTints[kRuneMaxTiles];
		/** @brief The frost window: xy = its world min, z = 1 / cell size, w = 1 while it holds marks. */
		float4 FrostRect;
		/** @brief 1 where frost texture slot i has a normal map. */
		float4 FrostNormals;
		/** @brief The tile's SpecularColor, w = specular power (RuneCapture::specular). */
		float4 RuneSpecular[kRuneMaxTiles];
		/** @brief x = the lowest, y = the highest point of the surfaces the rune's decal covers, z = its emissive multiplier. */
		float4 RuneReach[kRuneMaxTiles];
		/** @brief Per frost texture slot: SpecularColor and specular power, */
		float4 FrostSpecular[kFrostTextures];
		/** @brief EmitColor and w = 1 with a glow map, */
		float4 FrostTints[kFrostTextures];
		/** @brief and the emissive multiplier. */
		float4 FrostEmissiveMults;
	};
	STATIC_ASSERT_ALIGNAS_16(RuneCB);
	static_assert(kFrostTextures == 4, "RuneCB's frost rows and the shells' FrostDiffuse / FrostNormal / FrostGlow slots hold four");
	/** @brief Guards runeSites: written by the spell gather, read by the capture hook. */
	std::mutex runeLock;
	std::vector<RuneSite> runeSites;
	std::atomic<uint32_t> runeSitesLive{ 0 };
	std::vector<RuneSite> runeSitesScratch;
	std::unordered_map<const RE::BGSProjectile*, std::string> runeDecalPathCache;
	/** @brief Runes already judged for a lift, by formID; pruned as they leave, since projectile ids are recycled. */
	std::unordered_set<uint32_t> runeLifted;
	std::vector<RuneCapture> runeCaptures;
	std::unordered_set<const RE::BSGeometry*> runeCaptureSet;
	/** @brief This frame's frost impact decals (FrostDecalsOnSnow), every one, for the frost planes. */
	std::vector<RuneCapture> frostCaptures;
	std::unordered_set<std::string> runeDecalPathsLogged;
	Texture2D* runeAtlasTexture = nullptr;
	/** @brief Where decals overlap in a tile, the most opaque owns the texel: depth 1 - its alpha. */
	winrt::com_ptr<ID3D11Texture2D> runeDepthTexture;
	winrt::com_ptr<ID3D11DepthStencilView> runeDepthDSV;
	winrt::com_ptr<ID3D11DepthStencilState> runeDepthState;
	/** @brief Frost marks: three float4s each (uv plane and atlas quarter, from Blood.cpp FitFrostMark, and the height span of the surfaces the decal covers), the window's cells (first entry, count) and their lists, and the textures by slot. */
	winrt::com_ptr<ID3D11Buffer> frostPlaneBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> frostPlaneSRV;
	uint32_t frostPlaneCapacity = 0;
	winrt::com_ptr<ID3D11Buffer> frostCellBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> frostCellSRV;
	winrt::com_ptr<ID3D11Buffer> frostListBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> frostListSRV;
	uint32_t frostListCapacity = 0;
	std::vector<uint32_t> frostCellCounts;
	std::vector<uint32_t> frostListScratch;
	std::vector<float4> frostMarkScratch;
	winrt::com_ptr<ID3D11ShaderResourceView> frostDiffuse[kFrostTextures];
	winrt::com_ptr<ID3D11ShaderResourceView> frostNormal[kFrostTextures];
	winrt::com_ptr<ID3D11ShaderResourceView> frostGlow[kFrostTextures];
	bool frostPlanesFailed = false;
	/** @brief Each live tile's own decal textures, sampled by the shell through the tile's uv field. */
	winrt::com_ptr<ID3D11ShaderResourceView> runeTileDiffuse[kRuneMaxTiles];
	winrt::com_ptr<ID3D11ShaderResourceView> runeTileNormal[kRuneMaxTiles];
	winrt::com_ptr<ID3D11ShaderResourceView> runeTileGlow[kRuneMaxTiles];
	/** @brief This frame's decal draws the marks paint themselves, on every snow lying on a surface they cover: the decal overlay leaves them out. */
	std::unordered_set<const RE::BSGeometry*> markDecals;
	ConstantBuffer* runeCB = nullptr;
	ID3D11PixelShader* runePS = nullptr;
	bool runeResourcesFailed = false;
	bool runeAtlasDirty = false;
	bool runePaintLogged = false;
	bool frostPaintLogged = false;
	bool frostFitMissLogged = false;
	bool runeMissLogged = false;
	uint32_t runeMissFrames = 0;
	uint32_t runeStatSites = 0;
	uint32_t runeStatCaptures = 0;
	uint32_t runeStatTiles = 0;
	ID3D11ShaderResourceView* GetRuneAtlasSRV() const { return runeAtlasTexture ? runeAtlasTexture->srv.get() : nullptr; }
	/** @brief From the spell gather, per live projectile: a proximity record is a rune - lifted once, and listed as a site for its decal. */
	void ConsiderRune(RE::Projectile* a_projectile, const RE::NiPoint3& a_camera, float a_cullRadius, RE::TES* a_tes);
	void PublishRuneSites(const std::unordered_set<uint32_t>& a_present);
	/** @brief From the capture hook: a decal-mode Lighting draw. True when its diffuse is a live rune's decal texture, or a frost impact decal (a_engineDecal, FrostDecalsOnSnow), and it was taken. */
	bool CaptureRuneDraw(RE::BSRenderPass* a_pass, bool a_engineDecal);
	bool EnsureRuneResources();
	/** @brief Room for a_marks frost marks and a_entries cell entries. */
	bool EnsureFrostPlanes(uint32_t a_marks, uint32_t a_entries);
	/** @brief The frost planes, textures and cell lists at t117-t127 and the glow maps, or null with a_bind false. */
	void BindFrostField(ID3D11DeviceContext* a_context, bool a_bind);
	/** @brief Everything the shells read the rune glyphs and frost marks from (RuneCB at b7, the atlas, each tile's and slot's textures, the frost field) for a pass that paints them - the landscape shell, the object snow, the road patch - or null with a_bind false. */
	void BindMarkReaders(ID3D11DeviceContext* a_context, bool a_bind);
	/** @brief Clears the atlas and draws this frame's rune decals into their tiles, and finds each frost mark's uv plane and cells. After the blood capture, before the shells. */
	void RenderRuneCapture();

	// ---- Baked undulation field ----
	// The dune field is a pure function of world XY and the Spacing slider,
	// so it bakes: height + the +-12-unit shading gradient (amp-free; the
	// strength slider stays a live multiplier) into a camera-snapped window,
	// rebaked only on recenter or a Spacing change. Kills one two-octave
	// eval per vertex/march tap and four per shaded pixel on both shells.

	/** @brief 2048 texels of 16 world units: +-16384 around the snapped centre covers the shell grid's +-15744 at every snap offset; taps beyond (distant statics) fall back to the live eval. */
	static constexpr uint32_t kUndulationFieldDim = 2048;
	static constexpr float kUndulationFieldTexel = 16.0f;
	static constexpr float kUndulationFieldHalfExtent = kUndulationFieldDim * kUndulationFieldTexel * 0.5f;
	/** @brief Recenter grid; a multiple of the texel so world-texel alignment never swims across rebakes. */
	static constexpr float kUndulationFieldSnap = 512.0f;

	/** @brief x = amp-free height, yz = shading gradient. */
	Texture2D* undulationFieldTexture = nullptr;
	ID3D11ComputeShader* undulationFieldCS = nullptr;
	ID3D11ComputeShader* GetUndulationFieldCS();
	/** @brief The rim teeth's value-noise hash, one R16 texel per cell of the 256-cell torus (UndulationFieldCS NoiseHashCS), baked once and bound at PS t109 for the ground and the trench patch: one fetch per octave where the live noise takes four hashes. */
	static constexpr uint32_t kNoiseHashDim = 256;
	static constexpr uint32_t kNoiseHashSlot = 109;
	Texture2D* noiseHashTexture = nullptr;
	bool noiseHashBaked = false;
	ID3D11ComputeShader* noiseHashCS = nullptr;
	ID3D11ComputeShader* GetNoiseHashCS();
	/** @brief Bakes the hash table on the first call after it was created; the bound SRV, or null until baked. */
	ID3D11ShaderResourceView* NoiseHashSRV();
	/** @brief Trench detail tiles (tools/churn-tiles/pack_game.py): BC5 arrays, slices powder / wet; normal at PS t112, (height, cavity) at t113, read by the ground's and the road patch's shading. Absent files leave the detail off. */
	static constexpr uint32_t kTrenchDetailSlot = 112;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchDetailNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchDetailHeightSRV;
	bool trenchDetailAttempted = false;
	void EnsureTrenchDetailTextures();
	/** @brief Runtime-only: the trench detail coverage view (ShellCB::TrenchDetailView). */
	bool debugTrenchDetailView = false;
	struct alignas(16) UndulationFieldCB
	{
		float2 FieldOriginWorld;
		float FieldTexel;
		float FieldScale;
	};
	STATIC_ASSERT_ALIGNAS_16(UndulationFieldCB);
	ConstantBuffer* undulationFieldCB = nullptr;
	/** @brief World centre of the current bake; meaningless while !undulationFieldValid. */
	float2 undulationFieldCenter = { 0.0f, 0.0f };
	bool undulationFieldValid = false;
	/** @brief UndulationScale the field was baked at; a Spacing change rebakes. */
	float undulationFieldBakedScale = -1.0f;
	/** @brief Rebakes on recenter/Spacing change; no-ops when current. Called once per frame from Prepass. */
	void UpdateUndulationField();
	ID3D11ShaderResourceView* GetUndulationFieldSRV() const { return undulationFieldTexture ? undulationFieldTexture->srv.get() : nullptr; }

	/** @brief Creates the ping-pong deformation textures and the per-frame constant buffer. */
	virtual void SetupResources() override;

	/** @brief (Re)creates the ping-pong deformation textures at deformMapDim, releasing any previous pair. */
	void CreateDeformationTextures();

	/**
	 * @brief Per-frame update: gathers actor stamp positions, scrolls the window
	 * to follow the camera, and dispatches the deformation update compute shader.
	 */
	virtual void Prepass() override;
	/** @brief Prepass's map, simulation and per-frame work; Prepass then runs the early ground prepass (S1). */
	void PrepassSimulation();
	/** @brief Puts back the blended depth SyncSssDepth changed; runs after Screen-Space Shadows' PostPrepass march (feature order). */
	virtual void PostPrepass() override;

	/** @brief Returns the ring compute shader (rewrites the texels a scroll reassigned: inject or pristine), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationRingCS();
	/** @brief Returns the map-evolution compute shader (refill/decay/slump - the neighbour-reading pass), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationEvolveCS();
	/** @brief Returns the stamp compute shader (in-place RMW, one group per CPU-listed tile), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationStampCS();
	/** @brief Returns the full-map stamp fallback (tile list past its cap, or force-all-dirty). */
	ID3D11ComputeShader* GetDeformationStampAllCS();
	ID3D11ComputeShader* GetContactViewCS();
	ID3D11ComputeShader* deformationRingCS = nullptr;
	ID3D11ComputeShader* deformationEvolveCS = nullptr;
	ID3D11ComputeShader* deformationStampCS = nullptr;
	ID3D11ComputeShader* deformationStampAllCS = nullptr;
	ID3D11ComputeShader* contactViewCS = nullptr;
	bool deformationCSFailed = false;

	// ---- Tile dispatch (DEFORMATION-UPDATE-PLAN S3) ----
	/** @brief Stamp-pass tile list capacity. 256 stamps at walking bboxes are a few thousand tiles; past the cap the pass falls back to full-map rather than truncate (a truncated list is a silently frozen stamp). */
	static constexpr uint kStampTileCap = 16384;
	/** @brief Dynamic structured buffer of PHYSICAL tiles (x | y<<16) for StampCS, rebuilt per frame from the stamp capsules' and waves' bounding boxes. */
	winrt::com_ptr<ID3D11Buffer> stampTileBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> stampTileSRV;
	std::vector<uint32_t> stampTileScratch;
	std::vector<uint32_t> stampTileBits;
	/** @brief Last frame's stamp tile count, for the census readout (kStampTileCap+1 = overflowed to full-map). */
	uint32_t stampTilesLast = 0;
	/** @brief Fills stampTileScratch with the deduped physical tiles the stamp inputs touch (wrap-aware). Returns the count, or UINT32_MAX on overflow past kStampTileCap. */
	uint32_t BuildStampTileList(const PerFrame& a_data);

	/** @brief Per-tile (8x8) occupancy of the physical map, GPU-maintained (writers set it, evolve groups write their tile's truth). Recreated with the map; seeded all-1 (safe default: dirty). */
	winrt::com_ptr<ID3D11Texture2D> occupancyTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> occupancyUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> occupancySRV;
	/** @brief Evolve tile list: [0] count, then packed tiles. GPU-only (scan appends; its last group writes the count and sizes the indirect dispatch); the CPU never reads it. */
	winrt::com_ptr<ID3D11Buffer> evolveTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> evolveTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> evolveTileSRV;
	/** @brief Indirect dispatch args for the evolve pass (bytes 0-11), then the scan's append and finished-group counters (zero between scans). */
	winrt::com_ptr<ID3D11Buffer> evolveArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> evolveArgsUAV;
	/** @brief The tiles evolve's snapshot needs (occupancy dilated by twice the evolve halo) + their indirect args; GPU-only like the evolve pair. */
	winrt::com_ptr<ID3D11Buffer> snapshotTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> snapshotTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> snapshotTileSRV;
	winrt::com_ptr<ID3D11Buffer> snapshotArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> snapshotArgsUAV;
	/** @brief A/B, runtime-only: evolve's snapshot copies the whole map. */
	bool evolveSnapshotOff = false;
	/** @brief Set when the map (and with it the occupancy grid) was (re)created; the next Prepass seeds occupancy all-1. */
	bool tileGridsNeedInit = true;
	/** @brief Last consumed evolve-tile census (from the activity readback; lags by the ring like the verdict). */
	uint32_t evolveTilesLast = 0;

	/** @brief Returns the evolve-scan compute shader (occupancy + halo -> tile list), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationScanEvolveCS();
	/** @brief Returns the berm-scan compute shader (dirty grid + tap-reach halo -> tile list), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationScanBermCS();
	/** @brief Returns the tiled berm bake (indirect over the berm tile list), compiling it on first use. */
	ID3D11ComputeShader* GetBermFieldTiledCS();
	ID3D11ComputeShader* deformationScanEvolveCS = nullptr;
	/** @brief Returns the snapshot scan (occupancy + twice the evolve halo -> tile list), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationScanSnapshotCS();
	/** @brief Returns the snapshot copy (indirect over the snapshot list, map -> evolve's scratch), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationSnapshotCS();
	/** @brief Returns the whole-map snapshot copy (evolveSnapshotOff, or the listed copy unavailable), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationSnapshotFullCS();
	ID3D11ComputeShader* deformationScanSnapshotCS = nullptr;
	ID3D11ComputeShader* deformationSnapshotCS = nullptr;
	ID3D11ComputeShader* deformationSnapshotFullCS = nullptr;
	ID3D11ComputeShader* deformationScanBermCS = nullptr;
	ID3D11ComputeShader* bermFieldTiledCS = nullptr;

	/** @brief Per-tile "the map changed here" - the berm bake's dirty set, marked by every map writer and ACCUMULATED until a berm rebuild consumes it (each rebuilt tile clears its own mark), so a rebuild covers every change since the last one. Recreated with the map; seeded all-1. */
	winrt::com_ptr<ID3D11Texture2D> bermDirtyTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermDirtyUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> bermDirtySRV;
	/** @brief Berm tile list ([0] count) + its indirect args; GPU-only like the evolve pair. */
	winrt::com_ptr<ID3D11Buffer> bermTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> bermTileSRV;
	winrt::com_ptr<ID3D11Buffer> bermArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermArgsUAV;
	/** @brief Last consumed berm-tile census (activity readback). */
	uint32_t bermTilesLast = 0;
	/** @brief Per-tile "a berm texel is non-zero", written by each bake group for its own tile. Recreated with the map; seeded all-1. */
	winrt::com_ptr<ID3D11Texture2D> bermOccupancyTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermOccupancyUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> bermOccupancySRV;
	/** @brief 1 per tile where the map and the berm field are zero within one tile (ExtentCS, or UntouchedCS while the extent is frozen; after the berm bake). The shell PS skips its carve/berm reads there. Seeded all-0 (touched). */
	winrt::com_ptr<ID3D11Texture2D> untouchedTilesTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> untouchedTilesUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> untouchedTilesSRV;
	/** @brief Returns BermFieldCS.hlsl:UntouchedCS, compiling it on first use. */
	ID3D11ComputeShader* GetUntouchedTilesCS();
	ID3D11ComputeShader* untouchedTilesCS = nullptr;

	/** @brief Arriving-band rect in logical texel space; see ComputeArrivalRects. */
	struct ArrivalRect
	{
		int x0, y0, w, h;
	};
	/** @brief The rects whose world assignment changes under this frame's scroll (or the full map on a clear). ONE definition shared by RingCS's dispatch and BuildTrenchInject, so the ring pass and the inject upload can never disagree about which texels are arriving. Returns the rect count (0-2). */
	int ComputeArrivalRects(DirectX::XMINT2 a_scroll, bool a_clearing, ArrivalRect a_rects[2]) const;

	/** @brief Copies a LOGICAL-space box out of the toroidal map into a staging texture laid out logically: up to four physical segments split at the wrap seam, each landing at its logical offset in the destination. Every CPU read of the map routes through this so no site does its own torus arithmetic. a_physOrigin is the MapOrigin the source content was written under. */
	void CopyLogicalBox(ID3D11Texture2D* a_dst, ID3D11Texture2D* a_src, int a_x0, int a_y0, int a_w, int a_h, DirectX::XMINT2 a_physOrigin);
	virtual void ClearShaderCache() override;

	/** @brief Draws the ImGui settings UI, including the debug view of the deformation map. Implemented in SnowDeformation/Menu.cpp. */
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Installs both landscape hooks; the TESObjectLAND detour attaches after TruePBR's so it sees the final quad materials. Implemented in SnowDeformation/TerrainData.cpp. */
	virtual void PostPostLoad() override;

	/** @brief The terrain data window texture (absolute height, ramp depth in world units, coverage, spare). Ramp depth is resolved from the class weights and the class depth sliders at rebuild time. */
	Texture2D* shellTerrainTexture = nullptr;
	/** @brief shellTerrainTexture with WaterBareTexel applied per texel (TerrainCutCS), bound at t21 for SampleTerrain (TerrainWetSRV: the raw window until the cut has run, or if TerrainCutCS is unavailable). Rebuilt when the terrain window, the bound water window or its version changes. */
	Texture2D* shellTerrainWet = nullptr;
	/** @brief R8_UNORM beside the terrain window: 1 - the land's snow-texture weight (texture depth > 0) per texel, 0 where unbaked. Read by the land-blended edges. */
	Texture2D* shellTerrainBare = nullptr;
	std::vector<uint8_t> shellBareScratch;
	/** @brief Shell SRV slot of shellTerrainBare (SnowShell.hlsl TerrainBare). */
	static constexpr UINT kTerrainBareSlot = 26;
	/** @brief Replay prepass PS slot of Terrain Blending's mask depth (SnowShell.hlsl TBMaskDepth); null without Terrain Blending. */
	static constexpr UINT kTBMaskSlot = 110;
	/** @brief The land-blend state the window was last built with, so a switch rebuilds it. */
	bool shellWindowLandBlend = false;
	float shellWindowLandBlendStart = 1.0f;
	ID3D11ComputeShader* shellTerrainCutCS = nullptr;
	ID3D11ComputeShader* GetShellTerrainCutCS();
	/** @brief Rebuilds shellTerrainWet when its inputs changed. Call after the water cover pass, before any landscape-shell pass. */
	void UpdateTerrainWet();
	/** @brief SampleTerrain's t21: the cut window while it is current, else the raw window. */
	ID3D11ShaderResourceView* TerrainWetSRV() const;
	/** @brief The inputs shellTerrainWet was cut from: terrain version, water window version, water SRV. */
	uint64_t shellTerrainWetKey[3] = {};
	bool shellTerrainWetValid = false;
	/** @brief The SRV DrawShell and the caster bind at t27 (WaterWindow). */
	ID3D11ShaderResourceView* ShellWaterSRV() const;
	/** @brief The ground as the engine renders it, at the land mesh's own 32-unit spacing: bicubic Catmull-Rom of the 128-texel window with cell edges extrapolated (TerrainFineCS), 9 cells centred on the camera's cell, rebuilt with the window. SampleTerrain's height reads it with the land's own checkerboard triangulation, so every shell vertex lies on the visible surface. */
	static constexpr int kShellFineCells = 9;
	static constexpr float kShellFineTexel = 32.0f;
	static constexpr int kShellFineDim = kShellFineCells * 128;
	static constexpr size_t kShellFineCacheCells = 128;
	/** @brief Bake order of cells holding fine mesh data; the oldest lose theirs past kShellFineCacheCells (66 KB each). */
	std::deque<uint64_t> shellFineOrder;
	/** @brief Fills a_data.fine from the land's quad meshes; false (and fine empty) when any quad had no readable vertex copy or its lattice did not line up with heights[]. */
	bool ReadLandMeshHeights(RE::TESObjectLAND* a_land, ShellCellData& a_data);
	/** @brief Each frame: loaded cells whose bake ran before the engine subdivided their land mesh (289 vertices at hook time, 4225 later) get their fine data once the meshes have grown; a hard refusal is remembered so the cell is not re-tried. Sets shellFineDirty. */
	void RefreshLandMeshHeights();
	/** @brief The quad's drawn geometry: geom[q] when it holds the subdivided mesh, else the largest BSTriShape under mesh[q]. */
	static RE::BSTriShape* LandQuadGeometry(RE::TESObjectLAND::LoadedLandData* a_loaded, uint32_t a_quad);
	uint32_t fineRefreshFrame = 0;
	std::atomic<bool> shellFineDirty{ false };
	/** @brief Cells whose mesh read was refused, with the quad-level signature it was refused at; retried when the levels change. */
	std::unordered_map<uint64_t, uint32_t> shellFineRefused;
	/** @brief 0 none, 1 = 17x17, 2 = 33x33, 4 = 65x65 vertices per quad. */
	static uint32_t LandQuadLevel(RE::BSTriShape* a_geometry);
	Texture2D* shellTerrainFine = nullptr;
	float shellFineOriginX = 0.0f;
	float shellFineOriginY = 0.0f;
	bool shellFineValid = false;
	/** @brief Cells in the current fine window whose texels came from the land mesh rather than the cubic (menu readout). */
	uint32_t shellFineMeshCells = 0;
	std::atomic<bool> fineMeshWarned{ false };
	struct alignas(16) TerrainFineCB
	{
		float2 FineOriginWorld;
		float2 WindowOriginWorld;
		uint32_t FineDim;
		uint32_t WindowDim;
		float TexelSize;
		float FineTexel;
	};
	ConstantBuffer* terrainFineCB = nullptr;
	ID3D11ComputeShader* terrainFineCS = nullptr;
	ID3D11ComputeShader* GetTerrainFineCS();
	void BuildTerrainFineWindow();
	/** @brief The ground snow's and the patch's deformation tessellation keep no more segments than an edge's projected length holds at this many display pixels each. */
	static constexpr float kShellTessTargetPx = 16.0f;
	/** @brief kShellTessTargetPx in this frame's render pixels (TessScreen.x): an upscaler's render scale applied. */
	float ShellTessTargetRenderPx() const;

	/** @brief Rows of ShellCB::SnowClimateLUT: 2 C below the player's temperature, at it, 2 C above. */
	static constexpr uint32_t kClimateLUTRows = 3;
	/** @brief Temperature step between the rows (C). */
	static constexpr float kClimateLUTStep = 2.0f;

	/** @brief Per-draw constants for the shell pass. Layout must match ShellCB in SnowShell.hlsl. */
	struct alignas(16) ShellCB
	{
		Matrix CameraViewProj;
		Matrix CameraViewProjUnjittered;
		Matrix CameraPreviousViewProjUnjittered;
		Matrix CameraView;

		float4 CameraPosAdjust;
		float4 CameraPreviousPosAdjust;

		float2 GridOrigin;
		float GridSpacing;
		float TerrainTexelSize;

		// Precomputed on CPU so all shader-side field sampling happens in
		// small grid-local coordinates (absolute world XY at ~1e5 magnitude
		// destroys float32 finite differences â†’ shimmering normals).
		float2 GridToTerrainOffset;
		float2 GridToDeformOffset;

		float WarpedHalfSpan;
		/** @brief Layout keeper, uploaded as 0. */
		uint LayoutKeeper6;
		uint TerrainDim;
		/** @brief 1 = the trench detail coverage view (debugTrenchDetailView). Claims the retired debug-planes keeper. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		uint TrenchDetailView;

		float DeformInvWorldSize;
		uint HasSnowTexture;
		float SnowTextureIsLinear;
		float HasSnowNormal;

		float HasSnowRmaos;
		float SnowRoughnessScale;
		float2 SnowUVOffset;

		float4 SnowGlintParams;

		float SnowSpecularLevel;
		float EnableGlints;
		/** @brief Trench detail: relief (0 = off or no textures) and roughness multiplier (Settings::TrenchDetailRelief / TrenchDetailRoughness). Claim the retired Border Noise / Smoothness keepers. */
		float TrenchDetailRelief;
		float TrenchDetailRoughness;

		/** @brief Layout keeper, uploaded as 0. */
		float LayoutKeeper5;
		/** @brief Trench detail: the coverage cut across the trench profile (Settings::TrenchDetailCoverage). Claims the retired Border Fade keeper. */
		float TrenchDetailCoverage;
		/** @brief Cull replayed landscape triangles whose depth has reached the -8 bare floor everywhere. Claims the retired SeamFadeUnused slot, so the buffer layout is untouched. */
		float ShellCullBare;
		/** @brief Camera-distance band (world units) over which the statics skin dissolves back to the object's own material; start of the fade and the hard end (the capture range). */
		float SkinFadeStart;

		float SkinFadeEnd;
		/** @brief The object field maps: 0 = unbound, 0.5 = bound (shelter and shadow reads). Never read as a magnitude. */
		float ObjectLiftCap;
		float2 ObjectHeightCenter;

		float ObjectHeightHalfExtent;
		/** @brief The cascade atlas is bound at t22 this frame (else the shader falls back to the blurred VSM path). */
		float CrispShadows;
		/** @brief Screen-Space Shadows output bound at t45: the long-range depth-marched shadows carrying distant LOD tree shadows beyond the cascades. */
		float ScreenSpaceShadowsActive;
		/** @brief Dune-field amplitude in world units (0 flattens the undulation). Baked into the undulation field; the shells read it only on the live fallback. */
		float UndulationAmp;

		/** @brief Multiplier on the dune field's wavelengths (>1 = broader, calmer waves). */
		float UndulationScale;
		/** @brief Object-skin tessellation cap as world units per unit of camera distance: SkinTessCapPx / focal length in pixels. 0 = off. */
		float SkinTessCapSlope;
		/** @brief LLF cluster buffers bound at t35-t37, point-shadow table at t38 (point lights on the shells). */
		float PointLightsActive;
		/** @brief Skylighting probe volume bound at t50 (ambient parity with terrain). */
		float SkylightingActive;

		/** @brief PBR displacement companion bound at t8. */
		float HasSnowHeight;
		/** @brief Tessellated relief amplitude in world units (0 disables the tessellated path). */
		float SnowReliefDepth;
		/** @brief Statics debug view: object snow renders decision variables as colors (patch: R=deform G=skinDepth/8; skin: teal tint by coverage), dithering disabled. */
		float StaticsDebugView;
		/** @brief Edge berm crest height as a fraction of local snow depth. */
		float BermHeightAmp;

		/** @brief Settings::SpeedSpoil, 0 or 1: the trench detail shows the impact energy lying past the trench. Was a layout keeper. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		float SpeedSpoil;
		/** @brief Layout keeper, uploaded as 0 (the retired Detail Set's second slice's weight). */
		float LayoutKeeper9;
		/** @brief Layout keepers, uploaded as 0 (LayoutKeeper4 was the retired parallax trench view). */
		float DebugNoFarPad;
		float LayoutKeeper4;

		/** @brief Layout keeper, uploaded as 0. */
		float LayoutKeeper7;
		/** @brief Trench detail: sun self-shadow fade, Settings::ParallaxDepth up to 1 (the presets set it), 0 = off; its occlusion scales by SnowParallax.y (Parallax Shadow) as the snow texture's does. Claims a retired churn keeper. */
		float TrenchDetailShadow;
		/** @brief Layout keeper, uploaded as 0. */
		float LayoutKeeper3;
		/** @brief settings.ObjectMeetBand, read by the landscape shell and the drape. Was Spare0. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		float ObjectMeetBand;

		/** @brief BermReachScale() (Berm Steepness), for the per-pixel berm taps and BermGradStep (the bake reads PerFrame's copy). Was Spare1. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		float BermReach;
		/** @brief Layout keeper, uploaded as 0 (the retired Detail Set's slice). */
		uint LayoutKeeper10;
		/** @brief 1/width of the seam depth ramp; 0 = no seam data this frame (span fade only). */
		float SeamRampInv;
		/** @brief >0.5: the shells read the baked berm field (t14) instead of recomputing its 17 taps per call. */
		float BermBakeActive;

		/** @brief Loaded-cell boundary square (minX, minY, maxX, maxY): the shell ends here and the horizon recolor takes over. */
		float4 SeamBounds;

		/** @brief Wide exclusion field window: xy = world centre, z = 1/half extent, w > 0.5 when the field was baked this frame. */
		float4 ExclusionFieldWindow;

		/** @brief Parallax on the shells: x = HeightScale (the PBR JSON displacementScale, 1:1 with landscape now that kSnowUVTile matches), y = self-shadow strength (0 disables the taps), z = occlusion depth multiplier (0 disables the march, landscape shell only), w spare (the retired Screen-Space Parallax Trenches' reach). */
		float4 SnowParallax;

		/** @brief x = how dark a shock discharge burns the snow it struck; yzw spare (the retired crust look), uploaded as 0. Mirrored in SnowShell.hlsl's ShellCB - see CLAUDE.md on constant buffers being the silent collision. */
		float4 SpellShading;
		/** @brief Track age (TRACK-VARIATION-RESEARCH.md 3.4, SnowFields.hlsli TrackAgeOf): x = the ageing clock now, y = age per clock unit, Track Ageing / kTrackAgeSpan (0 = tracks never age); zw spare, 0. Was the retired crust look's first spare row. Mirror any change in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 TrackAge;
		/** @brief Spare row (the retired crust look), uploaded as 0 so BorderStyle keeps its offset. Mirror any change in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 padCrustLook2;
		/** @brief xy layout keepers (the retired dust switch and trench floor), uploaded as 0; zw = atlas slices of sun cascades 0/1 (the shared atlas moves the sun's slices with the active-light set, and the PS crisp path needs the real indices). Mirror any change in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 BorderStyle;
		/** @brief x spare, uploaded as 0; yz = the land-blend contest's depth bonus and carve penalty; w = trench wall slough, 0-1: how far CarveProfile takes the berm field's short blur over the carve (Snow Climate). Mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 CompactLook;
		/** @brief Stage 3: x = P5 rim lip height (fraction of local depth), y = P5 rim teeth strength, z spare (the retired berm clods), uploaded as 0, w = GetAccumulationDepthScale(). xy consumed inside CarveProfile. Appended LAST; mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 RimStyle;

		/** @brief Baked undulation window (UndulationFieldCS, t29): xy = world centre, z = 1/half-extent, w > 0.5 when the bake is live. Mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 UndulationFieldWindow;

		/** @brief Toroidal deformation-map addressing: physical position of logical texel (0,0). Every DeformationMap Load adds this and masks by dim-1. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		DirectX::XMINT2 DeformMapOrigin;
		/** @brief x: bit 0 = heightfield horizon self-shadow march on (Settings::ShellHorizonMarch); y: bit 0 = road tops in the skin-depth raster this frame. Took the torus pad; layout unchanged. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		DirectX::XMINT2 ShellFlags;
		/** @brief Land-exact height layer: xy = GridOrigin - fine window origin, z = fine dim (0 = none), w = fine texel size. */
		float4 FineWindow;
		/** @brief x = Settings::SlopeDrape, y = normal.z at full tilt (cos 60), z = normal.z where the tilt starts (cos 30), w = spare. */
		float4 SlopeDrape;
		/** @brief Debug view 9 (depth fight): x = Settings::SkinDepthBias, y = Settings::SkinSlopeDepthBias. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		float4 DebugSkinDepth;
		/** @brief Blood: x = Settings::BloodIntensity, y = the burial clock now (bloodBurialClock), z = game hours now, w = Settings::BloodBurial. Appended last; mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		float4 BloodLook;
		/** @brief Blood: x = Settings::BloodAgeHours, y = Settings::BloodSheen, z > 0.5 = the map is live, w > 0.5 = detail tiles are live. Mirror in both shells. */
		float4 BloodLook2;
		/** @brief Blood detail tiles: xy spare (the retired soak), z = the tiles' cell in units, w = their texel in units. Appended last; mirror in both shells. */
		float4 BloodLook3;
		/** @brief Trampled ground: x spare (the retired Floor Lowering), y = Settings::TrampledFlattening, z = BodyPrintScale() (one pass's share of the layer), w spare. Read through TrampleDetailScale. Appended last; mirror in both shells. */
		float4 TrampleStyle;
		/** @brief Shell tessellation screen-size cap: x = target pixels per segment (0 = the distance factor alone), yz = viewport half size in pixels, w = viewport depth range (MaxDepth - MinDepth). Appended last; mirror in both shells. */
		float4 TessScreen;
		/** @brief GetDimensions of the field maps every landscape-shell pass binds, 0 where unbound: x deformation (t1), y berm field (t14), z object maps (t5), w exclusion field (t15). All square. Landscape shell only (SnowShell.hlsl SNOW_FIELD_DIMS); appended last. */
		float4 FieldDims;
		/** @brief Snow climate at the player (TRACK-VARIATION-RESEARCH.md 3.2): x = air temperature (C), y = cooling per world unit of height, z = the player's world z, w = freshness. Overrides applied. SnowShell.hlsl only; appended last. */
		float4 SnowClimate;
		/** @brief x = wind packing, y = wetness at the player, z = condition view (0 off, 1 temperature, 2 wetness, 3 stickiness, 4 firmness, 5 all), w = 1 - Snow Climate: the weight of the fixed thin print. */
		float4 SnowClimate2;
		/** @brief Overrides painted everywhere, below 0 = derived per pixel: x wetness, y stickiness, z firmness; w spare. */
		float4 SnowClimateOverride;
		/** @brief The climate look per pixel, row i at the player's temperature + (i - 1) x kClimateLUTStep: x = ice (SnowIceShown, Wet Snow and Ice), y = wetness for Wet Snow and Ice, z = the most ice of the rows, in every row (the shaders' ice gate), w = Snow Climate x wetness. All x Snow Climate. SnowShell.hlsl SnowClimateRow interpolates by the pixel's height. */
		float4 SnowClimateLUT[kClimateLUTRows];
	};
	STATIC_ASSERT_ALIGNAS_16(ShellCB);
	static_assert(offsetof(ShellCB, TrampleStyle) == 800, "ShellCB tail must match the fxc-reflected layout of both shells");
	static_assert(offsetof(ShellCB, SnowClimate) == 848, "ShellCB tail must match the fxc-reflected layout of SnowShell.hlsl");
	static_assert(offsetof(ShellCB, SnowClimateLUT) == 896 && sizeof(ShellCB) == 944, "ShellCB tail must match the fxc-reflected layout of SnowShell.hlsl");

	/**
	 * @brief Per-light shadow data for shadow-casting local lights (t38), indexed by the light's shadow-mask channel.
	 *
	 * Mirrors PointShadowLight in SnowShadow.hlsli. LightType 0 marks an empty slot; the table is left empty
	 * whenever the local shadow map array is unavailable, so the shader falls back to unshadowed light.
	 * SliceIndex is the light's slice in that array.
	 */
	struct PointShadowLightData
	{
		DirectX::XMFLOAT4X4 LightTransform;
		uint32_t SliceIndex;
		uint32_t LightType;  ///< 0 empty, 1 spot, 2 paraboloid, 3 dual paraboloid
		float ShadowRadius;  ///< radius the engine normalised this map's depth by
		float pad;
	};
	static constexpr uint32_t kPointShadowMaxLights = 4;

	/**
	 * @brief Draws the ground snow and the captured statics into the deferred G-buffer.
	 *
	 * Called from Deferred::DeferredPasses before the composite, while the
	 * frame's G-buffer contents and main depth are complete. The ground is the
	 * landscape replay's prepass (here when S1 did not draw it), the resolve
	 * and one full-screen deferred pass. Binds its own targets/state and
	 * restores the previous pipeline state afterwards.
	 * Implemented in SnowDeformation/Shell.cpp.
	 */
	void DrawShell();

	/** @brief Measurement: D3D11 pipeline-statistics + occlusion queries around the ground snow's draws and the statics pass. PS invocations over samples passed is overdraw x overshade; DS/HS/VS invocations check the geometry-stage arithmetic. Ring of three, read back without flushing. Runtime-only. */
	bool shellPipelineStatsEnabled = false;
	struct ShellStatsResult
	{
		uint64_t vsInvocations = 0;
		uint64_t hsInvocations = 0;
		uint64_t dsInvocations = 0;
		uint64_t psInvocations = 0;
		uint64_t rasterizedPrimitives = 0;
		uint64_t samplesPassed = 0;
		bool valid = false;
	};
	static constexpr int kShellStatsRing = 3;
	winrt::com_ptr<ID3D11Query> shellStatsQuery[kShellStatsRing][2];
	winrt::com_ptr<ID3D11Query> staticsStatsQuery[kShellStatsRing][2];
	/** @brief The same two around the trench patch draw, inside the statics pass's. */
	winrt::com_ptr<ID3D11Query> patchStatsQuery[kShellStatsRing][2];
	/** @brief The statics pass's queries are open this frame, so the patch issues its own. */
	bool patchStatsArmed = false;
	bool shellStatsIssued[kShellStatsRing] = {};
	int shellStatsRing = 0;
	ShellStatsResult shellStatsLast;
	/** @brief Measurement, runtime-only: the same two queries around the volume draw. PS invocations over samples passed is the marches a written pixel costs. */
	bool voxelPipelineStatsEnabled = false;
	winrt::com_ptr<ID3D11Query> voxelStatsQuery[kShellStatsRing][2];
	bool voxelStatsIssued[kShellStatsRing] = {};
	int voxelStatsRing = 0;
	ShellStatsResult voxelStatsLast;
	bool EnsureVoxelStatsQueries();
	void ReadVoxelStatsQueries(ID3D11DeviceContext* a_context);
	ShellStatsResult staticsStatsLast;
	ShellStatsResult patchStatsLast;
	bool EnsureShellStatsQueries();
	void ReadShellStatsQueries(ID3D11DeviceContext* a_context);
	/** @brief The ground snow drew this frame: the prepass writes GridLocal, the terrain normal, the view depth and its own coarse derivatives, and one full-screen triangle (LESS against the fill's private depth at z 0) shades exactly the prepass's winners. The position is rebuilt from the view depth, the derivatives are the prepass's, the detail-parallax shift's own derivative is dropped, and the shading trusts the prepass's alpha cut. */
	bool shellDeferredThisFrame = false;
	/** @brief The deferred pass's inputs, display-size like shellRasterDepth: [0] GridLocal.xy + normal.xy (RGBA32F), [1] GridLocal's coarse derivatives (RGBA16F), [2] view depth + WorldPos.z's coarse derivatives (RGBA32F). */
	Texture2D* shellDeferredAttr[3] = {};
	winrt::com_ptr<ID3D11DepthStencilState> shellDeferredDepthState;
	bool EnsureDeferredResources();
	/** @brief The deferred PS. */
	ID3D11PixelShader* shellPSDeferred = nullptr;
	ID3D11PixelShader* GetShellPSDeferred();
	/** @brief The deferred PS with SNOW_CONDITION_VIEW (tessellated trenches only), compiled off the render thread the first time the view is switched on. */
	ID3D11PixelShader* shellPSDeferredConditionView = nullptr;
	std::atomic<int> conditionViewShaderState = 0;
	std::future<bool> conditionViewCompile;
	/** @brief The condition-view deferred PS when the view is on and compiled; null otherwise (the normal shading draws). */
	ID3D11PixelShader* ConditionViewPS();

	/** @brief The sun heightfield marches' occluder heights, baked (MarchCache.cpp), R32F: [0] the ground march's tap height per deformation texel in the map's torus (SnowShell.hlsl MarchHeightCS), [1] the statics march's road tap there (SnowStaticsShell.hlsl RoadMarchHeightCS; the sentinel where a tap is no road tap), [2] the ground's per land-layer vertex over the FineWindow, for marches that leave the trail map (MarchHeightFarRowsCS). Bound at PS t106-t108 for the ground and object snow. */
	Texture2D* marchHeight[3] = {};
	/** @brief The sun envelope over marchHeight[0], R32F in the same torus (SnowShell.hlsl MarchSunCS): per texel, the highest ground height toward the sun less its run times the sun's tangent. The near march reads it at PS t100 in place of its near taps. */
	Texture2D* marchSunEnvelope = nullptr;
	enum MarchCS : uint32_t
	{
		kGroundTilesCS,
		kGroundRowsCS,
		kRoadTilesCS,
		kRoadRowsCS,
		kFarRowsCS,
		kSunTilesCS,
		kSunRowsCS,
		kMarchCSCount
	};
	ID3D11ComputeShader* marchHeightCS[kMarchCSCount] = {};
	ID3D11ComputeShader* GetMarchHeightCS(MarchCS a_which);
	struct alignas(16) MarchCacheCB
	{
		uint32_t RowStart;
		uint32_t RowCount;
		uint32_t pad[2];
		/** @brief The envelope's sun: xy its horizontal direction (unit), z its tangent. */
		float4 Sun;
	};
	STATIC_ASSERT_ALIGNAS_16(MarchCacheCB);
	ConstantBuffer* marchCacheCB = nullptr;
	/** @brief The near caches hold every texel for the current inputs. Cleared by a resize, a settings change, missing inputs, or a berm bake the caches missed. */
	bool marchCacheValid = false;
	/** @brief The far cache holds every texel for the current land-layer window. */
	bool marchFarValid = false;
	uint64_t marchCacheKey = 0;
	uint32_t marchCacheTerrainVersion = 0;
	/** @brief The ageing clock at the last cache update: a jump (a wait in snowfall) ages every track at once and takes a fast sweep. */
	float marchCacheAgeingClock = 0.0f;
	float marchCacheFineOrigin[2] = {};
	uint32_t marchCacheRowCursor = 0;
	uint32_t marchFarRowCursor = 0;
	/** @brief Rows still to refresh at the fast rate after a terrain or land-layer rebuild. */
	uint32_t marchCacheFastRows = 0;
	uint32_t marchFarFastRows = 0;
	/** @brief Berm bakes run, and the last one the caches applied: the tile list holds only the latest bake's tiles. */
	uint64_t bermBakeSeq = 0;
	uint64_t marchCacheBermSeq = 0;
	/** @brief The sun the envelope is baked for (unit, toward the sun), taken when a sweep starts; whether it holds every texel; its row cursor; rows still to sweep at the fast rate. */
	float marchSunDir[3] = { 0.0f, 0.0f, 1.0f };
	bool marchSunValid = false;
	uint32_t marchSunRowCursor = 0;
	uint32_t marchSunFastRows = 0;
	/** @brief Census of the last 300 cache updates, per update: bit 0 key changed, bit 1 near rebuilt whole, bit 2 far rebuilt whole. */
	static constexpr uint32_t kMarchCensusFrames = 300;
	/** @brief Frames in a row with no march reader (no ground snow, no capture); from kMarchIdleFrames the caches stop and the next update rebuilds them whole. */
	uint32_t marchIdleFrames = 0;
	static constexpr uint32_t kMarchIdleFrames = 30;
	uint64_t marchIdleSkipped = 0;
	/** @brief DevBench A/B: true keeps the caches updating with no reader, as before the idle gate. */
	bool marchIdleOff = false;
	std::array<uint8_t, kMarchCensusFrames> marchCensus{};
	uint32_t marchCensusCursor = 0;
	uint32_t marchCensusFilled = 0;
	/** @brief Brings the caches up to date and binds them at PS t106-t108, the sun envelope at t100 (DrawShell, after the shell CB upload). False = no caches this frame: the marches must be switched off, as they read nothing else. */
	bool UpdateMarchCaches(const ShellCB& a_cb);
	void UnbindMarchCaches();
	void ReleaseMarchCache();	/** @brief The prepass PS writing the deferred attributes [plain, hidden kept]. */
	ID3D11PixelShader* shellPSPrepassAttr[2] = {};
	ID3D11PixelShader* GetShellPSPrepassAttr(bool a_keepHidden);

	/** @brief S1: DrawShellEarlyPrepass runs the ground prepass (the landscape replay) at the end of Prepass, before the game's opaque draws, against the blended scene depth seeded into shellTestDepth, and writes its settled pixels into the main depth, so the terrain replay and buried objects fail the depth test under the snow. Pixels whose cut needs this frame's land masks (the land's verdict) or a static's mask (the export clamp) stay pending (negative raster depth, inputs in shellEarlyPending); DrawShell's resolve finishes them and re-tests every winner against the main depth. Needs Terrain Blending and this frame's landscape capture. */
	enum class EarlyPrepass : uint8_t
	{
		Off,
		Drawing,
		NotDrawing,
		NeedsTerrainBlending,
		Unavailable
	};
	EarlyPrepass shellEarlyState = EarlyPrepass::Off;
	/** @brief Frame the early prepass drew in; DrawShell finishes it only in that frame. */
	uint32_t shellEarlyFrame = UINT32_MAX;
	/** @brief The CB the early prepass drew with; DrawShell counts the frames whose view moved since. */
	std::unique_ptr<ShellCB> shellEarlyCB;
	D3D11_VIEWPORT shellEarlyViewport{};
	/** @brief Frames whose shell viewport or camera differed from the ones the early prepass drew with. */
	uint32_t shellEarlyViewMismatches = 0;
	/** @brief The last DrawShell's shell viewport and frame, and its ground gate: the next early prepass draws with them. */
	D3D11_VIEWPORT shellDrawViewport{};
	uint32_t shellDrawViewportFrame = UINT32_MAX;
	bool shellDrawGroundGated = false;
	/** @brief shellTerrainVersion when the last DrawShell rebuilt the water windows and the wet cut, laid on the terrain window's grid: a window Prepass moved since leaves them a cell off. */
	uint32_t shellDrawTerrainVersion = 0;
	/** @brief The dynamic-resolution scale the last DrawShell drew with: the early prepass needs this frame's to match. */
	float shellDrawDynRes[2] = {};
	/** @brief Pending pixels' resolve inputs (RGBA16F) and the resolve's winner marks (R32F), display-size like shellRasterDepth. */
	Texture2D* shellEarlyPending = nullptr;
	Texture2D* shellEarlyMarks = nullptr;
	bool EnsureEarlyPrepassResources();
	void DrawShellEarlyPrepass();
	/** @brief SNOW_PREPASS_EARLY prepass PS. */
	ID3D11PixelShader* shellPSPrepassEarly = nullptr;
	ID3D11PixelShader* GetShellPSPrepassEarly();

	ID3D11PixelShader* shellPSEarlyResolve = nullptr;
	ID3D11PixelShader* GetShellPSEarlyResolve();

	/** @brief Frame SyncSssDepth ran in: RestoreSssDepth and DrawShell's ScreenSpaceShadowsActive = 2 key on it. */
	uint32_t shellSssSyncFrame = UINT32_MAX;
	/** @brief The early prepass's rect, shared by the sync and the restore. */
	D3D11_BOX shellSssRect{};
	/** @brief Early-prepass frames: S1's raster depth into Terrain Blending's 32-bit blended depth, the replaced values into shellEarlyMarks. */
	void SyncSssDepth();
	/** @brief Writes shellEarlyMarks back over the pixels SyncSssDepth changed. */
	void RestoreSssDepth();
	/** @brief a_cs over shellSssRect with t1 the raster depth, u0 the blended depth, u1 the marks; CS t1, u0-u1, b2 and the shader go back as found. */
	void DispatchSssDepth(ID3D11ComputeShader* a_cs, const char* a_pass);
	ID3D11ComputeShader* sssSyncCS = nullptr;
	ID3D11ComputeShader* sssRestoreCS = nullptr;
	/** @brief DepthSyncCS's SssSyncCS entry, compiled on first use. */
	ID3D11ComputeShader* GetSssSyncCS();
	/** @brief DepthSyncCS's SssRestoreCS entry, compiled on first use. */
	ID3D11ComputeShader* GetSssRestoreCS();

	/** @brief Frame MarchSkinSss ran in (menu and DevBench status): after the skins' depth prepass, Screen-Space Shadows march again with the object snow and the ground snow in the depth, and the skins apply that mask as Lighting does (StaticsCB::SkinSssSeen). */
	uint32_t skinSssMarchFrame = UINT32_MAX;
	/** @brief DrawShell's ground marks as DepthSync reads them (the early marks or the raster depth; null when the ground drew no prepass), set before DrawCapturedStatics. */
	ID3D11ShaderResourceView* shellGroundMarksSRV = nullptr;
	/** @brief The blended depth MarchSkinSss replaced, until its restore (R32F, over the skins' viewport extent). */
	Texture2D* skinSssSaved = nullptr;
	/** @brief The conditions under which ScreenSpaceShadows::PostPrepass marches, plus this pass's resources (skinSssSaved over a_viewport's extent) and shaders. */
	bool SkinSssMarchReady(const D3D11_VIEWPORT* a_viewport);
	/** @brief Skin and ground-snow pixels (a_skinDepthSRV, the skins' private depth, against the pre-snow copy and shellGroundMarksSRV) into the 32-bit blended depth over a_viewport's rect, ScreenSpaceShadows::DrawShadows, the blended depth back; rebinds PS t3 (a_sceneDepthSRV) and t45 for the skin shading. */
	void MarchSkinSss(ID3D11ShaderResourceView* a_skinDepthSRV, ID3D11ShaderResourceView* a_sceneDepthSRV, const D3D11_VIEWPORT* a_viewport);
	ID3D11ComputeShader* skinSssSyncCS = nullptr;
	ID3D11ComputeShader* skinSssRestoreCS = nullptr;
	/** @brief DepthSyncCS's SkinSssSyncCS entry, compiled on first use. */
	ID3D11ComputeShader* GetSkinSssSyncCS();
	/** @brief DepthSyncCS's SkinSssRestoreCS entry, compiled on first use. */
	ID3D11ComputeShader* GetSkinSssRestoreCS();
	/** @brief The blended depth ScreenSpaceShadows::PostPrepass marched, copied before RestoreSssDepth (sssFirstMarchFrame the frame) up to last frame's march rect's far corner. */
	Texture2D* sssFirstMarchDepth = nullptr;
	uint32_t sssFirstMarchFrame = UINT32_MAX;
	/** @brief MarchSkinSss's rect, kept for the next frame's first-march copy. */
	D3D11_BOX skinSssRectLast{};
	/** @brief One texel per 8x8 depth texels, set where MarchSkinSss's depth differs from sssFirstMarchDepth (each with its neighbours). */
	Texture2D* sssChangedTiles = nullptr;
	ID3D11ComputeShader* skinSssSyncMarkCS = nullptr;
	/** @brief DepthSyncCS's SkinSssSyncMarkCS entry (SKIN_SSS_MARK), compiled on first use. */
	ID3D11ComputeShader* GetSkinSssSyncMarkCS();
	/** @brief Screen-Space Shadows' raymarch with SSS_CHANGED_TILES, for its current scaled sample count (sssChangedRaymarchSamples); null without that feature. */
	ID3D11ComputeShader* GetSssChangedRaymarchCS();
	ID3D11ComputeShader* sssChangedRaymarchCS = nullptr;
	uint32_t sssChangedRaymarchSamples = 0;
	/** @brief A/B, runtime-only: the skins' march redoes every wavefront. */
	bool sssChangedMarchOff = false;
	/** @brief Last frame the skins' march skipped the wavefronts reading only unchanged depth. */
	uint32_t sssChangedMarchFrame = UINT32_MAX;
	/** @brief Copies the blended depth the first march read, on frames after one the skins marched in. */
	void CopyFirstMarchDepth();
	/** @brief (Re)creates sssChangedTiles for a depth of this size; true when it is usable. */
	bool EnsureSssChangedTiles(UINT a_width, UINT a_height);
	ID3D11PixelShader* shellFillPSClaim = nullptr;
	ID3D11PixelShader* GetShellFillPSClaim();
	static void SetShellSeamBounds(ShellCB& a_cb, int a_minX, int a_minY, int a_maxX, int a_maxY);

	ID3D11VertexShader* GetShellFillVS();
	/** @brief FillVS over the skins' footprint (skinFootprint at VS t62) instead of the whole viewport. */
	ID3D11VertexShader* GetShellFillRectVS();
	ID3D11PixelShader* GetShellFillPS();
	ID3D11VertexShader* shellFillVS = nullptr;
	ID3D11VertexShader* shellFillRectVS = nullptr;
	ID3D11PixelShader* shellFillPS = nullptr;
	bool EnsurePrepassResources(ID3D11ShaderResourceView* a_mainDepthSRV);
	Texture2D* shellRasterDepth = nullptr;
	winrt::com_ptr<ID3D11Texture2D> shellTestDepth;
	winrt::com_ptr<ID3D11DepthStencilView> shellTestDepthDSV;
	/** @brief Read view of shellTestDepth for the object-snow prepass's write-back into the main depth. */
	winrt::com_ptr<ID3D11ShaderResourceView> shellTestDepthSRV;
	/** @brief Main depth before any snow draw this frame: DepthSync syncs the pixels the snow passes (shell, skins, patch) moved nearer. R32F, the float the main depth's SRV returns, written over the rendered rectangle by PreSnowCopyCS. */
	winrt::com_ptr<ID3D11Texture2D> preSnowDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> preSnowDepthSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> preSnowDepthUAV;
	/** @brief Render-target view of preSnowDepth for the skins' first fill (FILL_PRE_SNOW). */
	winrt::com_ptr<ID3D11RenderTargetView> preSnowDepthRTV;
	bool preSnowDepthThisFrame = false;
	/** @brief The copy waits for DrawCapturedStatics: the shell prepass leaves raster marks, so DepthSync reads it only where the main depth still holds the pre-snow value until a statics pass writes. */
	bool preSnowDepthDeferred = false;
	/** @brief The rendered rectangle DrawShell's copy and DepthSync cover this frame. */
	D3D11_BOX preSnowRect{};
	/** @brief Takes the pre-snow copy now (PreSnowCopyCS over preSnowRect), with the output-merger bindings saved and restored around it. */
	void TakePreSnowCopy(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_mainDepthSRV);
	/** @brief PreSnowCopyCS: the main depth's floats over a_box into an R32F UAV, with the output-merger bindings saved and restored around it. A depth-stencil source cannot be copied in part. */
	void CopyDepthRect(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_mainDepthSRV, const D3D11_BOX& a_box, ID3D11UnorderedAccessView* a_uav);
	ID3D11PixelShader* GetShellFillPSPreSnow();
	ID3D11PixelShader* shellFillPSPreSnow = nullptr;
	/** @brief Origin and extent (pixels) of the rendered rectangle for DepthSyncCS: the viewport under dynamic resolution. */
	struct alignas(16) DepthSyncRectCB
	{
		uint32_t Origin[2];
		uint32_t Extent[2];
	};
	ConstantBuffer* depthSyncRectCB = nullptr;
	/** @brief depthSyncRectCB, created on first use. */
	ConstantBuffer* DepthSyncRectBuffer();
	ID3D11ComputeShader* GetPreSnowCopyCS();
	ID3D11ComputeShader* preSnowCopyCS = nullptr;
	/** @brief The rectangle a screen pass covers: the viewport plus a one-pixel guard (the skins read neighbours), clamped to the target; the whole target with no viewport. */
	D3D11_BOX ScreenRectBox(const D3D11_VIEWPORT* a_viewport, uint32_t a_width, uint32_t a_height, uint32_t a_guard) const;
	/** @brief EQUAL against the main depth, writes off: the statics' shading admits only their prepass's winners. */
	winrt::com_ptr<ID3D11DepthStencilState> shellPrepassMainDepthState;
	winrt::com_ptr<ID3D11DepthStencilState> shellFillDepthState;

	ConstantBuffer* shellCB = nullptr;

	/** @brief One live arc: where it came from, where it struck, and how far through its life it is. */
	struct LightningArc
	{
		RE::NiPoint3 from;
		RE::NiPoint3 to;
		float age = 0.0f;
		float life = 0.16f;
		float seed = 0.0f;
	};
	/** @brief Bounded hard. Arcs are cosmetic, so a barrage drops the excess rather than growing a list on the render thread. */
	static constexpr size_t kMaxLightningArcs = 24;
	/** @brief Quads along one bolt. Must match ARC_SEGMENTS in LightningArc.hlsl. */
	static constexpr uint kLightningArcSegments = 32;
	std::vector<LightningArc> lightningArcs;
	uint32_t lightningArcSeed = 0;


	/** @brief Layout must match ArcCB in LightningArc.hlsl. Its own buffer, NOT part of ShellCB, which is hand-mirrored across two shaders and must not grow for a cosmetic pass. */
	struct ArcCB
	{
		Matrix CameraViewProj;
		float4 ArcCameraPosAdjust;
		float4 ArcFrom;
		float4 ArcTo;
		float4 ArcParams;
		/** @brief rgb = colour, w = spare. */
		float4 ArcTint;
	};
	ConstantBuffer* arcCB = nullptr;
	ID3D11VertexShader* arcVS = nullptr;
	ID3D11PixelShader* arcPS = nullptr;
	winrt::com_ptr<ID3D11BlendState> arcBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> arcDepthState;
	winrt::com_ptr<ID3D11RasterizerState> arcRasterState;

	/** @brief Loads a DDS through the GAME's resource system, so archives resolve and a modlist override still wins. Shared by the snow and trench detail textures - a second copy would drift. */
	static bool LoadGameDDS(const std::string& a_dataRelativePath, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv);
	ID3D11VertexShader* GetLightningArcVS();
	ID3D11PixelShader* GetLightningArcPS();
	bool EnsureLightningArcResources();

	/** @brief Records an arc for this frame's draw. Called from the shock cloak's own discharge, which already knows both ends. */
	void EmitLightningArc(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
	/** @brief Ages the live arcs and drops the spent ones. */
	void UpdateLightningArcs(float a_deltaTime);
	/** @brief Draws the live arcs. Called AFTER the deferred composite - an emissive overlay lit by nothing, which is what a bolt is. */
	void DrawLightningArcs();
	winrt::com_ptr<ID3D11RasterizerState> shellRasterState;
	/** @brief Skin draws' raster state: the shell's plus SkinDepthBias / SkinSlopeDepthBias, rebuilt when either slider moves. The decal variant adds the game's decal depth bias (-1, slope -0.65, clamp -100) for skins of decal-mode draws (CapturedSnowStatic::decalDepth). Implemented in SnowDeformation/Shell.cpp. */
	ID3D11RasterizerState* GetSkinRasterState(bool a_decal = false);
	winrt::com_ptr<ID3D11RasterizerState> skinRasterState;
	winrt::com_ptr<ID3D11RasterizerState> skinDecalRasterState;
	float skinRasterBiasBuilt = 0.0f;
	float skinRasterSlopeBuilt = 0.0f;
	/** @brief Viewport max depth of the game's decal depth-bias mode, measured (RenderDoc 2026-09-10, Windhelm: 0.99997199 against the main pass's 0.99999797). */
	static constexpr float kDecalViewportMaxDepth = 0.99997199f;
	winrt::com_ptr<ID3D11DepthStencilState> shellDepthState;

	/** @brief Returns the depth sync compute shader (shell depth -> Terrain Blending's blended depth copies), compiling it on first use. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11ComputeShader* GetDepthSyncCS();
	ID3D11ComputeShader* depthSyncCS = nullptr;

	/** @brief Object-snow debug view: skins and trench patch render decision variables as colors with dithering disabled. Runtime-only diagnostic. */
	/** @brief 0 off, 1 edge-taper masks, 2 coverage alpha (see the PS debug block). */
	int staticsDebugView = 0;

	/** @brief The landscape snow diffuse, loaded from the modlist via the VFS. Null when unavailable (constant-albedo fallback). */
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowDiffuseSRV;
	/** @brief TruePBR companion maps, auto-resolved by probing the Textures\PBR\ variant of the snow path: tangent normals (_n) and roughness/metal/AO/spec (_rmaos). Their presence also auto-selects linear color. */
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowRmaosSRV;
	/** @brief Displacement companion (_p): drives the shells' parallax occlusion, the same depth mechanic PBR ground uses. */
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowHeightSRV;
	bool shellSnowTextureIsPBR = false;
	bool shellSnowTextureAttempted = false;

	/** @brief Material parameters resolved from TruePBR's shared texture-set table (keys = JSON filename stems, matched by texture basename) so the shell's sparkle and response follow whatever texture set the user runs. Defaults mirror common authored snow values. */
	float snowGlintLogDensity = 6.0f;
	float snowGlintMicroRoughness = 0.3f;
	float snowGlintDensityRandomization = 5.0f;
	float snowGlintScreenSpaceScale = 1.0f;
	float snowRoughnessScale = 0.7f;
	float snowSpecularLevel = 0.02f;
	float snowDisplacementScale = 1.0f;
	/** @brief Matched key into TruePBR::pbrTextureSets; empty = no match, built-in defaults. The full PBRTextureSetData (subsurface, coat, fuzz) is reachable through it. */
	std::string snowPBRSetName;
	winrt::com_ptr<ID3D11SamplerState> shellSnowSampler;

	/** @brief Lazy-loads the shell snow texture set (and its authored PBR parameters) from the user-configured path. Implemented in SnowDeformation/Shell.cpp. */
	void EnsureShellSnowTextures();
	/** @brief PS define list for the shell shaders: PSHADER, optional extra, plus SNOW_EXP_HEIGHT_FOG when the EHF addon is loaded. Implemented in SnowDeformation/Shell.cpp. */
	static std::vector<std::pair<const char*, const char*>> ShellPSDefines(const char* a_extra = nullptr);
	/** @brief Re-reads the matched TruePBR texture set's values (per frame: follows ReloadTextureSetData and live menu edits). Implemented in SnowDeformation/Shell.cpp. */
	void RefreshSnowPBRParams();

	/** @brief Bakes one cell's heights and per-vertex snow coverage from LoadedLandData. Called from the TESObjectLAND hook. Implemented in SnowDeformation/TerrainData.cpp. */
	void BakeShellCell(RE::TESObjectLAND* land);

	/** @brief Recenters and re-uploads the terrain data window when the camera crosses cells or new cells were baked. Called from Prepass. Implemented in SnowDeformation/TerrainData.cpp. */
	void UpdateShellTerrainWindow();

	/** @brief Tracks the camera's worldspace and invalidates the world-anchored caches on a change. Called from Prepass. Implemented in SnowDeformation/TerrainData.cpp. */
	void UpdateActiveWorldspace();

	/** @brief Registers the texture set a land texture is drawn with (the season's swap when one is active) and returns its registry index, or kNoLandTexture for an absent one. Implemented in SnowDeformation/TerrainData.cpp. */
	uint16_t RegisterLandTexture(RE::TESLandTexture* a_landTexture);

	/** @brief Re-resolves every non-overridden texture against the current class depths. Call after the class sliders or the whole settings block move. Implemented in SnowDeformation/TerrainData.cpp. */
	void RefreshLandTextureDepths();

	/** @brief Pins one texture to its own depth, or clears the pin when the depth is empty. Holds the registry lock: the bake thread reads the same map. Implemented in SnowDeformation/TerrainData.cpp. */
	void SetLandTextureOverride(const std::string& a_path, std::optional<float> a_depth);

	/** @brief Snapshot of the per-index depths, taken once per window rebuild so the texel loop needs no lock. Implemented in SnowDeformation/TerrainData.cpp. */
	std::vector<float> LandTextureDepthSnapshot();

	/** @brief Reads the baked cell under a world position: which worldspace it came from, its height, and the textures resolving its depth. Implemented in SnowDeformation/TerrainData.cpp. */
	ShellProbe ProbeShellData(float a_x, float a_y);

	/** @brief Re-resolves one entry: user value, then shipped per-texture default, then the family depth. Caller holds landTextureMutex exclusively. */
	void ResolveLandTextureDepthLocked(LandTextureEntry& a_entry);

	/** @brief Re-resolves every entry against the class defaults and the override map. Caller holds landTextureMutex exclusively. */
	void ResolveLandTextureDepthsLocked();

	// ---- Far fill: heightmap-sourced distant terrain ----

	/** @brief Per-dispatch constants for the window far fill. Layout must match WindowFillCB in TerrainWindowFillCS.hlsl. */
	struct alignas(16) WindowFillCB
	{
		float2 WindowOriginWorld;
		float TexelSize;
		uint WindowDim;

		float2 HeightMapScale;
		float2 HeightMapOffset;

		float2 HeightRange;
		float SnowDepthUnits;
		float padFill;

		/** @brief 2x2 level-32 LOD diffuse tile block: SW corner world XY, per-tile world span (32 cells), snow-classification sensitivity. */
		float2 LODTileBase;
		float LODTileSpan;
		float LODSnowSensitivity;

		float4 LODTileValid;
	};
	STATIC_ASSERT_ALIGNAS_16(WindowFillCB);
	ConstantBuffer* windowFillCB = nullptr;

	/** @brief Level-32 LOD terrain diffuse tiles (loose xLODGen output, sRGB ignored), keyed by SW cell coords; misses are remembered so absent files are probed once. Cleared on worldspace change. */
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11ShaderResourceView>> lodTileCache;
	std::unordered_set<uint64_t> lodTileMisses;
	/** @brief Fetches (or loads) the level-32 LOD diffuse tile with the given SW cell coords for the current fill worldspace. Implemented in SnowDeformation/TerrainData.cpp. */
	ID3D11ShaderResourceView* GetLODTile(const std::string& a_worldspace, int a_cellX, int a_cellY);

	/** @brief Fills sentinel window texels from the Terrain Shadows xLODGen heightmap (height + snow-line coverage, provenance in .w). Runs after each window upload. Implemented in SnowDeformation/TerrainData.cpp. */
	void FillShellWindowFromHeightmap();
	ID3D11ComputeShader* GetWindowFillCS();
	ID3D11ComputeShader* windowFillCS = nullptr;
	/** @brief Worldspace whose heightmap filled the current window; a change forces a rebuild even when the origin is unchanged. */
	std::string lastFillWorldspace;

	/** @brief Nominal (untrampled) snow depth in world units at a world XY, resolved from the baked cell data and the class depth sliders. Returns a_missing where no cell is baked (interiors, unvisited land). Implemented in SnowDeformation/TerrainData.cpp. */
	float GetNominalSnowDepthAt(float a_x, float a_y, float a_missing);

	/** @brief Thread-safe count of baked cells, for the settings UI. */
	size_t ShellCellCountForUI()
	{
		const std::shared_lock lock(shellCellMutex);
		return shellCells.size();
	}

	// Diagnostics for the settings UI (written on window rebuild).
	uint32_t shellStatCellsInWindow = 0;
	uint32_t shellStatSnowTexels = 0;
	float shellStatMinHeight = 0.0f;
	float shellStatMaxHeight = 0.0f;

	// ---- Statics snow skin: capture & redraw ----

	/** @brief One snow-flagged draw captured this frame, for re-rendering inflated in DrawShell. NiPointer keeps the geometry alive across the frame even if its cell detaches mid-frame. */
	struct SmoothedNormalsEntry;
	struct CapturedSnowStatic
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		/** @brief Road match (name or texture, never a bridge): this capture uses RoadMeshesDepth, so the model class cannot be split across a road model's trishapes. */
		bool road;
		/** @brief Glacier/iceberg family: captured past the Object Snow range cap and exempt from the SkinFade distance dissolve — their own baked snow never matches the shell, so the skin must persist at every loaded distance. */
		bool fadeExempt;
		/** @brief projectedUVParams.w from the draw's property, -1 without kProjectedUV; see StaticsCB::ProjThreshold. */
		float projThreshold;
		/** @brief projectedUVParams.x - vanilla's noise-term strength; 0 without projection data. */
		float projNoiseScale;
		/** @brief projectedUVParams.z - the noise map's world-space tiling; 0 without projection data. */
		float projNoiseTiling;
		/** @brief The noise term's weight in Lighting's projWeight (its ProjectedUVParams.x): red * (1 - alpha) of the unmodified property; 0 without projection data. */
		float projNoiseWeight;
		/** @brief Mountain/cliff family by geometry name: force the ROUNDED class. The divergence-only flat classifier reads a jagged low-poly cliff's split normals as "plate" and drapes it with a rigid vertical lift of the full flat depth - the hovering sheet Josef reported as a translucent film. Deterministic name match (road-class precedent), NOT a stats heuristic (those were tried and rejected, see SmoothNormalsCS FlatStatsCS). */
		bool forceRounded;
		/** @brief Plank family by geometry name (plank/walkway/catwalk). Currently DECIDES NOTHING - Josef's round-5 call classifies ALL PD draws rounded; the match is kept (and logged once per name) for the future cornice treatment when the 3D shell returns. */
		bool plankFamily;
		/** @brief The property really carries kProjectedUV (projThreshold read from it). False for the mesh-replacer default (threshold 0, no noise), whose reconstructed weight is a guess the coat and the edge lumps must not trust. */
		bool projReal;
		/** @brief Drift family by geometry name: coated at every angle and every loaded distance (StaticsCB::FullCoat); implies the range-cap and dissolve exemption. */
		bool fullCoat;
		/** @brief Object LOD batch (kLODObjects / kHDLODObjects): the game draws a DynDOLOD BTO one segment per reference or cell, and which segments it draws is not a function of distance (the large-reference bug keeps segments on inside the loaded grid). Kept out of the height raster; the skin keeps a pixel only where its hull coincides with the scene depth, and coats only off the recolor's written weight (Lighting writes 2 + w for the LOD brightness recolor as for projected snow). */
		bool lodBatch;
		/** @brief The game rasterised this draw in its decal depth-bias mode (RendererShadowState::rasterStateDepthBiasMode != 0, or the Decal/DynamicDecal property flags): depth written through DepthBias -1, SlopeScaledDepthBias -0.65, viewport max depth 0.999972. Its skin draws through the same state plus the skin bias, or it loses the depth test at every grazing view (Windhelm's snow-overlay shapes, RenderDoc 2026-09-10). */
		bool decalDepth;
		/** @brief The mesh alpha-tests: StaticsCB::AlphaTested. */
		bool alphaTested;
		/** @brief Snow-textured shape without projection data (Settings::SnowTexturedRecolor): Lighting recolors it by texel and writes the weight, so its skin coats off that read-back like an LOD batch. */
		bool snowTex;
		/** @brief The projected MATO classifies as snow (the record's cached class). */
		bool matoSnow;
		/** @brief The capturing pass's technique is one whose paint Snow Follows Parallax can move (envmap's complex material, TruePBR displacement): Lighting may read the projection noise off the mesh there. StaticsCB::ParallaxFollow. */
		bool mayParallax;
		/** @brief Snow Follows Parallax: the object's height map as Extended Materials marches it (TruePBR displacement or the complex-material env mask), null without one or with the setting off; parallaxUV/parallax fill StaticsCB::ObjParallaxUV/ObjParallax. */
		winrt::com_ptr<ID3D11ShaderResourceView> parallaxSRV;
		float4 parallaxUV = {};
		float4 parallax = {};
		/** @brief SmoothedNormalsFor's entry, valid in frame smoothFrame while smoothResets matches smoothedNormalsResets. */
		SmoothedNormalsEntry* smoothEntry = nullptr;
		uint32_t smoothFrame = UINT32_MAX;
		uint32_t smoothResets = 0;
	};

	/** @brief Render-thread only: filled during opaque rendering by the SetupGeometry hook, consumed and cleared each frame. */
	std::vector<CapturedSnowStatic> capturedStatics;
	/** @brief This frame's road captures: none leaves the trench patch nothing to stand on. */
	uint32_t capturedRoads = 0;
	/** @brief This frame's loaded (non-LOD) captures, and last frame's: the decal hook runs before the list is whole. */
	uint32_t capturedLoaded = 0;
	uint32_t capturedLoadedPrev = 0;
	/** @brief The engine's live water bodies (TESWaterSystem::waterObjects, gathered each Prepass by GatherWaterObjects): cell planes and placed bodies, culled or not. Rasterised top-down into the water window, then cleared. */
	struct CapturedWater
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		float planeConstant;
	};
	std::vector<CapturedWater> capturedWater;
	/** @brief Fills capturedWater from TESWaterSystem::waterObjects (every live body's shape, culled or not); not from the water shader's draws, which also carry the LOD quadtree, skirts, the displacement mesh and the waterline-crossing effect. */
	void GatherWaterObjects();
	/** @brief Open addressing, reserved once: clear() keeps the slots, so a frame's captures allocate nothing. */
	ankerl::unordered_dense::set<void*> capturedStaticsSet;
	std::atomic<uint32_t> statCapturedStatics{ 0 };

	/** @brief Per-frame projected-snow classification counters (render thread writes, Prepass publishes, menu debug section reads). Plain: every access is on the render thread. */
	uint32_t statProjMatched = 0;
	uint32_t statProjNoProjection = 0;
	uint32_t statProjVetoed = 0;
	uint32_t statProjMatchedPrev = 0;
	uint32_t statProjNoProjectionPrev = 0;
	uint32_t statProjVetoedPrev = 0;

	/** @brief Records projected-snow lighting draws for the statics skin. Called from the BSLightingShader::SetupGeometry hook. Implemented in SnowDeformation/Statics.cpp. */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass, uint32_t a_rawTechnique);

	/** @brief Publishes the per-pass SnowProjectedIsSnow permutation bit, classified from the pass technique (currentRawTechnique), and re-binds the snow set for classified draws. Must run BEFORE the game's SetupGeometry, which consumes the descriptor. Implemented in SnowDeformation/Statics.cpp. */
	void SetProjectedSnowBit(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass);

	/** @brief Lists this frame's captured statics whose bounds cover a world position, largest first: what the object snow is skinning there. Implemented in SnowDeformation/Statics.cpp. */
	ObjectSnowProbe ProbeObjectSnow(float a_x, float a_y);
	/** @brief Installs the SetupGeometry capture hook. Called from PostPostLoad; implemented in SnowDeformation/Statics.cpp. */
	void InstallStaticsCaptureHook();
	/** @brief PlayerCamera::Update vfunc: after the game has placed the camera, ClampCameraAboveSnow. Implemented in TerrainData.cpp. */
	void InstallCameraHook();
	// ---- Public API (SnowDeformation/API.cpp, SNOW-API.md): Papyrus globals on the SnowDeformation script + the exported SnowDeformation_GetAPI C struct. Read-only, any thread. ----
	/** @brief Queues the Papyrus native registration. */
	void InstallAPI();
	/** @brief Loaded and switched on. */
	bool APIIsActive() const;
	/** @brief Untrampled terrain snow depth at a world XY, accumulation-scaled, clamped at 0; 0 when off or unbaked. Trenches are not subtracted. */
	float APISnowDepthAt(float a_x, float a_y);
	/** @brief APISnowDepthAt under a reference, 0 when it stands more than kElevatedStampCutoff above the land. */
	float APISnowDepthAtRef(RE::TESObjectREFR* a_ref);
	/** @brief The accumulation scalar 0..1, 0 when accumulation or the feature is off. */
	float APISnowAccumulation() const;
	/** @brief Snow surface (ground + class depth, accumulation-scaled) at a world XY from the CPU terrain window; false where the window has no ground or no snow. */
	bool SampleShellSurface(float a_x, float a_y, float& a_surface) const;
	/** @brief Third person, mount, bleedout (ragdoll, death), dragon and PCTransition, plus any non-free camera while the player is down: if the camera sits under the shell, pull it toward the player along its own line until it clears the snow, the way a terrain hit would; while knocked down, ragdolled or dead, lift it straight up instead. */
	void ClampCameraAboveSnow();
	/** @brief Holds dropped items at their depth in the snow. From the camera-update hook: before the frame's first pass, main thread. */
	void ItemSinkUpdate();
	/** @brief Raises the rune and frost pieces the spell scan queued. From the camera-update hook: main thread, before the frame's first pass. */
	void ApplySpellLifts();
	/** @brief Draws creatures lighter than the player standing in the snow by weight. From the camera-update hook: main thread, before the frame's first pass. */
	void CreatureSinkUpdate();
	/** @brief Probe readout of the last clamp: 0 hook never ran, 1 no shell data under the camera, 2 clear, 3 pulled in, 4 camera state not clamped, 5 off, 6 lifted (player down). */
	uint8_t cameraProbeState = 0;
	uint32_t cameraProbeFired = 0;
	uint32_t cameraProbeCamState = 0;
	bool cameraProbeDown = false;
	float cameraProbeZ = 0.0f;
	float cameraProbeSurface = 0.0f;

	/** @brief Pre-shell copy of the MASKS target: Masks.y carries the land's EM grain height (Lighting.hlsl LANDSCAPE; 0 = no data) for the shell's two-sided edge contest, readable only before the shell overwrites the G-buffer. Bound at t10 on the shell PS. */
	winrt::com_ptr<ID3D11Texture2D> landMasksCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> landMasksCopySRV;
	/** @brief DevBench A/B: true takes the copy whenever any object-field reader runs, as before it was gated on its own readers. */
	bool masksCopyGateOff = false;
	/** @brief The Masks note as it stood before the frame's first DECAL-MODE lighting draw. Decal mode blends every render target, the Masks channel included, so a decal over painted snow drags the drape's verdict off "painted" toward its own value (measured 2026-09-22: 48 -> 20 under Inn01:0, and the drape cut itself out in the decal's alpha pattern). The coat takes whichever of this and the live copy reads as more painted. */
	winrt::com_ptr<ID3D11Texture2D> preDecalMasksTex;
	winrt::com_ptr<ID3D11ShaderResourceView> preDecalMasksSRV;
	uint32_t preDecalMasksFrame = UINT32_MAX;
	void CapturePreDecalMasks();

	/** @brief Copies a_box (all of it when null) of the resource behind a_srcSRV into an owned SRV-only texture sized to a_extent's far corner (a_box's when null, the source's when both are), recreating it when that size or the format changes. The SRV doubles as the validity signal (nulled by callers on invalid frames), so it is rebuilt even when the texture itself is still current. Implemented in SnowDeformation/Shell.cpp. */
	static void CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
		winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, const D3D11_BOX* a_box = nullptr, const D3D11_BOX* a_extent = nullptr);

	// ---- Sun shadows on the shells: crisp cascade receiver + caster ----

	/** @brief The engine's sun atlas as bound at the shadow-mask pass. */
	winrt::com_ptr<ID3D11ShaderResourceView> shadowAtlasLiveSRV;
	/** @brief The atlas the crisp cascade shadows sample this frame (in place), or null (the VSM path). */
	ID3D11ShaderResourceView* ShadowAtlasSRV() const { return shadowAtlasLiveSRV.get(); }
	/** @brief LESS_EQUAL comparison sampler for the atlas (s2). */
	winrt::com_ptr<ID3D11SamplerState> shadowCmpSampler;
	/** @brief Linear-clamp sampler standing in as ShadowSampling.hlsli's LinearSampler (s1). */
	winrt::com_ptr<ID3D11SamplerState> shellLinearSampler;

	/** @brief Called from State::Draw while the game renders the shadow MASK (Utility shader, RenderShadowmask); the only point where PS t4 genuinely holds the sun cascade atlas (at any other time it holds whatever texture the last draw bound). Injects the trench patch as a caster, then keeps the atlas for the crisp shell shadows to read in place. Same trigger VolumetricShadows and Skylighting use. Implemented in SnowDeformation/Shadows.cpp. */
	void CaptureShadowAtlas();

	/** @brief Lazily compiles the trench patch's shadow-caster VS (SNOW_PATCH_INDEXED, drawn through patchCasterIB). Implemented in SnowDeformation/Statics.cpp. */
	ID3D11VertexShader* GetPatchShadowVS();

	/** @brief Last frame's fully-computed ShellCB (heap-held: ShellCB is over-aligned and embedding it pads the class). The caster injection runs at the shadow-mask pass, before this frame's DrawShell recomputes the windows; one-frame-stale grid placement is invisible in a shadow. Null until the first DrawShell. */
	std::unique_ptr<ShellCB> lastShellCBData;

	/** @brief Per-descriptor DSVs created on the LIVE atlas texture, cached by texture pointer (not owned; key only) and by the REAL slice each descriptor renders to (shadowmapIndex — the atlas is shared with local shadow lights and the sun's slices move with the active-light set). Four entries: the sun owns more shadowmaps than the two cascades (the focus map is in the family, and descriptor order is not guaranteed), so the shell is injected into every one — each descriptor carries its own transform and slice, making the ordering irrelevant. Truncated shadows (near lobe present, far lobe missing, boundary sweeping with the view) were the two-descriptor assumption missing the far cascade. */
	winrt::com_ptr<ID3D11DepthStencilView> shadowAtlasDSV[4];
	ID3D11Texture2D* shadowAtlasDSVTexture = nullptr;
	uint32_t shadowAtlasDSVSlice[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	/** @brief Atlas slices of sun cascades 0/1, captured at mask time (descriptors are only live then) and uploaded via BorderStyle.zw for the PS crisp path, which samples the same shared atlas. */
	uint32_t sunCascadeSlice[2] = { 0, 1 };
	winrt::com_ptr<ID3D11RasterizerState> shadowCastRS;
	winrt::com_ptr<ID3D11DepthStencilState> shadowCastDSS;

	/** @brief Depth-renders the road trench patch into the live cascade slices; the ground snow casts through the raised landscape (LandscapeSnow.cpp). The statics skins deliberately do NOT cast: a skin hovers a few units above its object's own surface, so the object beneath always reads as shadowed by its own snow cap. Called from CaptureShadowAtlas before the snow reads the atlas. Implemented in SnowDeformation/Shadows.cpp. */
	void InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV);
	/** @brief The sun cascades' real atlas slices into sunCascadeSlice every frame (the snow's shadow reads), whether or not anything casts. */
	void RecordSunCascadeSlices(uint32_t a_atlasSlices);

	/** @brief Re-derives the camera-anchored placement fields of a ShellCB (the fields' frame) from THIS frame's camera. Shared by DrawShell, S1 and the shadow passes, so no pass reads the fields through a frame-old placement (a stale one slides a crisp shadow during camera motion). Implemented in SnowDeformation/Shell.cpp. */
	void RefreshShellGridPlacement(ShellCB& a_cb, const float4* a_camAdjust = nullptr);

	/** @brief Shadow-source diagnostics for the settings UI (kept permanently; they answer "where do this scene's shadows come from" without a debugger): cascade descriptor count, the three end-split distances, and the live atlas's slice count. */
	uint32_t dbgLodDescriptorCount = 0;
	float dbgLodEndSplits[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t dbgLodAtlasSlices = 0;

	/** @brief Queries adapter VRAM usage/budget in MB via IDXGIAdapter3 (zeros when unavailable). Usage above budget = driver demotion to system RAM = the large persistent FPS-loss mode that survives disabling features. */
	void QueryAdapterVRAM(uint64_t& a_usageMB, uint64_t& a_budgetMB, uint64_t* a_nonLocalMB = nullptr);
	/** @brief Sums every live resource named "SnowDeformation::" (Util::SumNamedResources, from descriptors) and fills a line with the largest by name. */
	uint64_t SumFeatureTextureBytes(std::string& a_breakdown);
	/** @brief Periodic VRAM log from Prepass: fires on a ~90 s cadence, on large usage deltas, and immediately (as a warning) when usage exceeds budget; lists every snow resource when the set changed, at most once a cadence. */
	void TickVRAMLog();
	uint64_t vramTickCounter = 0;
	uint64_t vramLastLoggedMB = 0;
	uint64_t vramLastLogTick = 0;
	uint64_t vramListedBytes = 0;
	uint32_t vramListedResources = 0;
	uint64_t vramListedTick = 0;
	/** @brief Logs a snow profiler row far over its usual cost, with the adapter's memory and the frame's heaviest rows. */
	void TickRowSpikeLog();
	uint64_t rowSpikeQuietUntil = 0;

	/** @brief 4-entry structured buffer of PointShadowLightData (t38), uploaded each frame from the mask-time snapshots. */
	Buffer* pointShadowLights = nullptr;

	/** @brief Full-array view over the engine's local shadow map array (kSHADOWMAPS), cached by texture. */
	winrt::com_ptr<ID3D11Texture2D> pointShadowLiveTex;
	winrt::com_ptr<ID3D11ShaderResourceView> pointShadowLiveSRV;
	/** @brief The array the local-light shadows sample this frame (t39, in place), or null. */
	ID3D11ShaderResourceView* PointShadowAtlasSRV() const { return pointShadowLiveSRV.get(); }

	/** @brief Per-channel descriptor snapshots, merged at each local mask pass; entries persist until their channel is reassigned (an extinguished light's entry is never sampled: its cluster light loses the Shadow flag). */
	PointShadowLightData pendingPointShadows[kPointShadowMaxLights] = {};

	/** @brief Called from State::Draw at a LOCAL light's shadow-mask pass (Utility RenderShadowmaskSpot/Pb/Dpb): the only moment the light's descriptor is live and PS t4 holds the local map array. Snapshots every live local descriptor. Implemented in SnowDeformation/Shadows.cpp. */
	void CapturePointShadowMask();

	/** @brief Uploads the pending point-shadow table (t38). The table stays empty until mask-time snapshots exist. Implemented in SnowDeformation/Shadows.cpp. */
	void UpdatePointShadowLights();

	/** @brief Per-object constants for the statics skin. Layout must match StaticCB in SnowStaticsShell.hlsl. */
	struct alignas(16) StaticsCB
	{
		float4 WorldRow0;
		float4 WorldRow1;
		float4 WorldRow2;
		/** @brief flat-class depth (walkways, roofs, planks); road captures use RoadMeshesDepth for both classes so the GPU pick cannot override it. */
		float ObjectsDepth;
		/** @brief The top-down height window (center-anchored, camera-following). */
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		/** @brief >0.5: a smoothed-normal buffer is bound at VS t10 for this object (pillow inflation for flat meshes). */
		float HasSmoothedNormals;
		/** @brief rounded-class depth (rocks, drifts, logs); the VS picks per mesh from the GPU flatness stats. */
		float RoundedDepth;
		/** @brief Vertex count = index of the flatness-stats element appended to the SmoothedNormals buffer. */
		float VertexCountF;
		/** @brief >0.5: the object top raster is bound at PS t11 for this draw (skin rim-wall gate). */
		float HasObjectTop;
		/** @brief Debug views 8/9: skin draw ordinal + 1, negated for a skin drawn in the game's decal depth mode. */
		float DebugSkinId;
		/** @brief >0.5: this draw keeps the tuned pre-rework skin behaviour (road and bridge meshes); set from the capture's road flag. */
		float LegacySkin;
		/** @brief was MoundSteepness (Mound Steepness, retired 2026-09-25); slot kept for layout. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float padMoundSteepness;
		/** @brief >0.5: the mesh alpha-tests (a thatch fringe, a plank end); the skin keeps only pixels coincident with the scene surface, so it never spans the texels the game discarded. Took the retired ObjectTrenches row. */
		float AlphaTested;
		/** @brief Drift meshes (geometry name): the whole mesh is the snow, so the skin coats every facing and never collapses, dissolves or drops out of range. */
		float FullCoat;
		/** @brief >0.5: skip the SkinFadeStart/End distance dissolve (glacier/iceberg captures). Mirror in SnowStaticsShell.hlsl. */
		float FadeExempt;
		/** @brief >0.5: this capture or skin draw is a road (not a bridge): the capture writes the per-texel road bit and the skin steps aside where the trench patch owns the column. 1 on the patch draw, which does not read it. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float RoadField;
		/** @brief Vanilla projected-UV mask threshold (BSLightingShaderProperty::projectedUVParams.w) for this draw; -1 when the property carries no kProjectedUV (or kTreeAnim, whose vertex alpha is wind weight, not a snow mask). Feeds the S0 debug view and the S2 placement suppressor (SKIN-PLACEMENT-PLAN.md). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjThreshold;
		/** @brief CapturedSnowStatic::projNoiseWeight: the coat's reach swaps that noise term for a hex-tiled read of the same map. Took the padProjMask slot. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjNoiseWeight;
		/** @brief >0.5: Snow Follows Parallax is on and this draw's technique can carry Extended Materials parallax, so Lighting may have read the projection noise off the mesh; the coat's reach then keeps the game's noise instead of swapping it. Took the padProjDensity slot. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ParallaxFollow;
		/** @brief Class override code: 0 = flat classifier decides, 1 = force ROUNDED (CapturedSnowStatic::forceRounded, and every PD draw in authored-relief mode), 2 = force FLAT (plankFamily in authored-relief mode). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ClassOverride;
		/** @brief CapturedSnowStatic::projNoiseScale (projectedUVParams.x) - strength of vanilla's projected-noise term. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjNoiseScale;
		/** @brief Settings::SteepFaceThinning. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float SteepThin;
		/** @brief CapturedSnowStatic::projNoiseTiling (projectedUVParams.z) - the noise map's world-space tiling. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjNoiseTiling;
		/** @brief 2 = the S4 shell owns this draw (SKIN-PLACEMENT-PLAN S4 phase 1): the rolling-ball fillet grown vertically over the fill-covered slice of the projected footprint, per-pixel coverage from the reconstructed vanilla weight. 0 = classic path (no projection data, or a road). Encoded as 2 so the shader's >1.5 tests survive any future middle state. Requires the noise map at t21. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjPixelEnable;
		float padSkinNormalCopy;
		float padShellMinNz;
		/** @brief kPeelTol (the retired Plane Merge Height, fixed at its default) - surfaces within this many units below a peeled layer's top belong to that layer's plane (the peel tolerance, user-tunable). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float PeelTol;
		/** @brief > 0.5: this frame's Screen-Space Shadows mask was marched again with the object snow in the depth (MarchSkinSss); the skin applies it as Lighting does. Took the padOverhead slot. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float SkinSssSeen;
		float padMeldSk;

		/** @brief Far road level: window centre (snapped to its texel), half-extent in world units (0 = off), and the one class depth roads carry there (Settings::RoadMeshesDepth). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float FarWindowCenterX;
		float FarWindowCenterY;
		float FarHalfExtent;
		float FarRoadDepth;
		/** @brief >0.5: the mesh drew in the game's decal mode, after the opaque section; the blended depth never holds it. */
		float DecalMode;

		/** @brief >0.5: landMasksCopySRV is bound at the skin PS (the recolor's real projected weight, Masks.y = 2 + w on classified statics). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float HasSkinMasksCopy;
		/** @brief >0.5: preDecalMasksSRV is bound at the skin PS t53 - the same note taken before the frame's first decal-mode draw blended it. Took the retired padLumpSize slot, so the buffer layout is untouched. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float HasPreDecalMasks;
		/** @brief Settings::SkinEdgeFlankWidth. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float EdgeFlankWidth;
		/** @brief Settings::ProjSnowMatch as 0/1: the skin coats the solidly painted projected snow with its own material near the camera. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float EdgeCoat;
		/** @brief Near clipmap: half-extent of the fine object window in world units, 0 when the level is off. Its centre is the coarse window's (the fine texel divides the coarse one, so one snap serves both). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float FineHalfExtent;
		/** @brief CapturedSnowStatic::lodBatch as 0/1. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float LODBatch;
		/** @brief CapturedSnowStatic::snowTex as 0/1: the coat follows Lighting's written verdict alone, never a reconstruction. Took the padLODCells slot, so the buffer layout is untouched. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float TexturedSnow;

		/** @brief Snow Follows Parallax, read by the SNOW_COAT_PARALLAX variants: the object's uv transform, Lighting's TexcoordOffset (offset xy, scale zw). Mirror in SnowStaticsShell.hlsl. */
		float4 ObjParallaxUV;
		/** @brief x HeightScale (0 = no parallax), y the height channel (0 TruePBR displacement, 3 complex-material env mask), zw the dims scale and mip shift that make ExtendedMaterials::GetMipLevelFromDims the source's own mip rule. Mirror in SnowStaticsShell.hlsl. */
		float4 ObjParallax;
	};
	STATIC_ASSERT_ALIGNAS_16(StaticsCB);

	// ---- Smoothed normals for the statics skin (pillow inflation) ----

	/** @brief GPU-side per-mesh CB for SmoothNormalsCS. Layout must match SmoothCB in SmoothNormalsCS.hlsl. */
	struct alignas(16) SmoothCB
	{
		uint32_t VertexCount;
		uint32_t StrideBytes;
		uint32_t NormalOffsetBytes;
		uint32_t PosIsFloat32;
		uint32_t TableMask;
		uint32_t ColorOffsetBytes;
		uint32_t HasColor;
		uint32_t BoundsSlot;
		uint32_t IndexPoolOffset;
		uint32_t IndexCount;
		uint32_t ClusterOffset;
		uint32_t ClusterStrideIndices;
		/** @brief First element of the mesh's range in its smoothedNormalsChunks buffer. */
		uint32_t OutBase;
		uint32_t SmoothPad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SmoothCB);

	/** @brief First-fit allocator over [0, capacity): free ranges keyed by offset, merged on release. Implemented in SnowDeformation/Statics.cpp. */
	struct RangeAllocator
	{
		std::map<uint32_t, uint32_t> free;
		void Reset(uint32_t a_capacity);
		/** @brief UINT32_MAX when no free range holds a_count at a_align. */
		uint32_t Allocate(uint32_t a_count, uint32_t a_align = 1);
		void Release(uint32_t a_offset, uint32_t a_count);
	};

	/** @brief Per unique geometry (keyed by vertex buffer pointer): position-averaged normals, built once by SmoothNormalsCS. Split-normal flat meshes (planks, roofs, pole caps) inflate along these so their snow drapes as a sealed pillow instead of a hovering parallel sheet. */
	struct SmoothedNormalsEntry
	{
		/** @brief The mesh's range in its smoothedNormalsChunks buffer (FirstElement = chunkOffset, vertexCount + 1 elements). */
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool ready = false;
		/** @brief Slot in meshBounds holding this mesh's local box (MESHBOUNDS pass), UINT32_MAX when none. */
		uint32_t boundsSlot = UINT32_MAX;
		/** @brief Cluster range in clusterBounds and the mesh's slice of clusterIndexPool (CLUSTERBOUNDS pass); count 0 when the mesh got none and its skins draw their own index buffer. */
		uint32_t clusterOffset = 0;
		uint32_t clusterCount = 0;
		uint32_t indexPoolOffset = 0;
		uint32_t indexCount = 0;
		uint32_t chunk = UINT32_MAX;
		uint32_t chunkOffset = 0;
		uint32_t chunkCount = 0;
		/** @brief The geometry the key's vertex buffer belonged to at the build; a recycled pointer that disagrees rebuilds. */
		uint64_t descKey = 0;
		void* gameIB = nullptr;
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		uint32_t lastUsedFrame = 0;
		std::list<void*>::iterator lru;
	};
	/** @brief Least recently used entries past this many are evicted at the next build; entries used this frame never are. */
	static constexpr uint32_t kSmoothedNormalsCap = 1024;
	/** @brief Elements (float4) per smoothed-normal pool buffer; a larger mesh gets a buffer of its own size. */
	static constexpr uint32_t kSmoothedNormalsChunkElements = 1u << 20;
	struct SmoothedNormalsChunk
	{
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		RangeAllocator ranges;
		uint32_t capacity = 0;
	};
	std::vector<SmoothedNormalsChunk> smoothedNormalsChunks;
	std::list<void*> smoothedNormalsLru;
	/** @brief Build scratch reused by every build: the copied vertex stream (one raw SRV) and the hash table (one raw UAV per power-of-two table size). */
	winrt::com_ptr<ID3D11Buffer> smoothSource;
	winrt::com_ptr<ID3D11ShaderResourceView> smoothSourceSRV;
	uint32_t smoothSourceBytes = 0;
	winrt::com_ptr<ID3D11Buffer> smoothTable;
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> smoothTableUAVs;
	uint32_t smoothTableSlots = 0;
	bool smoothAllocatorsLive = false;
	bool EnsureSmoothBuildScratch(uint32_t a_sourceBytes, uint32_t a_tableSlots);
	bool AllocateSmoothedNormals(uint32_t a_count, uint32_t& a_chunk, uint32_t& a_offset);
	void EvictSmoothedNormals(std::unordered_map<void*, SmoothedNormalsEntry>::iterator a_it);
	/** @brief Drops every entry and frees every pool range; the buffers stay unless a_release. */
	void ResetSmoothedNormals(bool a_release);
	/** @brief Local boxes of the unique meshes, two float4 per slot (min, max), built beside the smoothed normals; the skin cull projects them. */
	static constexpr uint32_t kMeshBoundsSlots = 1024;
	Buffer* meshBounds = nullptr;
	RangeAllocator meshBoundsFree;
	ID3D11ComputeShader* smoothBoundsCS = nullptr;

	/** @brief Cluster cull: a precombined cell chunk's box contains the camera, so the whole-skin test can never reject it. Each unique mesh is cut into at most one thread group's worth of clusters (64 triangles each until that would exceed the cap, proportionally larger after), each with its own local box; ClusterCullCS tests them against the same depth pyramid and compacts the survivors' indices, in order, into clusterScratchIB, which both skin loops draw through the same indirect arguments. Implemented in SnowDeformation/Statics.cpp. */
	static constexpr uint32_t kClusterGroup = 256;
	static constexpr uint32_t kClusterTrisBase = 64;
	static constexpr uint32_t kClusterSlots = 65536;
	static constexpr uint32_t kClusterIndexPoolIndices = 4u * 1024u * 1024u;
	/** @brief The compacted index stream is sized to what the scene actually asks for, grown a frame late and capped here; a skin that does not fit draws its own index buffer entire. */
	static constexpr uint32_t kClusterScratchMaxIndices = 8u * 1024u * 1024u;
	uint32_t clusterScratchCapacity = 0;
	uint32_t clusterScratchNeeded = 0;
	ID3D11ComputeShader* smoothClusterCS = nullptr;
	ID3D11ComputeShader* clusterCullCS = nullptr;
	ID3D11ComputeShader* GetClusterCullCS();
	bool EnsureClusterResources(uint32_t a_scratchIndices);
	Buffer* clusterBounds = nullptr;
	Buffer* clusterIndexPool = nullptr;
	Buffer* clusterScratchIB = nullptr;
	RangeAllocator clusterFree;
	RangeAllocator clusterIndexPoolFree;

	/** @brief CPU census (PERF-RESEARCH §11.1): QPC ticks and counts accumulated across one frame, rolled into cpuShown by RollCpuCensus at the next Prepass. Static so the free helpers in Stamping.cpp can reach it. The capture hook has no profiler row of its own: it runs inside the game's lighting draws. */
	struct CpuCensus
	{
		int64_t hookTicks = 0;
		uint32_t hookCalls = 0;
		/** @brief SetProjectedSnowBit's calls; its ticks go into hookTicks (it is the same hook, before the game's own setup). */
		uint32_t hookBitCalls = 0;
		int64_t actorTicks = 0;
		uint32_t actors = 0;
		int64_t landTicks = 0;
		uint32_t landCalls = 0;
		int64_t depthTicks = 0;
		uint32_t depthCalls = 0;
		int64_t traverseTicks = 0;
		uint32_t traverseCalls = 0;
		uint32_t skinLoopDraws = 0;
		uint32_t skinLoopCBUpdates = 0;
		/** @brief FNV-1a of (StampCount, Stamps, StampEnds) as uploaded: the bit-identity check for every CPU lever. */
		uint64_t stampHash = 0;
		uint32_t stampHashStable = 0;
		/** @brief FNV-1a of the capture list as DrawCapturedStatics sees it (pointer, transform, every classification field): the identity check for hook levers. */
		uint64_t captureHash = 0;
		uint32_t captureCount = 0;
		static constexpr uint32_t kHashRing = 300;
		uint64_t stampHashRing[kHashRing] = {};
		uint32_t stampHashHead = 0;
		bool stampHashDumpRequested = false;
		/** @brief Named CPU spans: Prepass and DrawShell whole, then DrawShell's consecutive segments (SpanClock laps). */
		enum Span : uint32_t
		{
			kSpanPrepass,
			kSpanDrawShell,
			kSpanShellSetup,
			kSpanObjectHeightMap,
			kSpanWaterCapture,
			kSpanWaterCover,
			kSpanBloodCapture,
			kSpanRuneCapture,
			kSpanGround,
			kSpanStatics,
			kSpanShellRestore,
			kSpanShellTail,
			kSpanPrepassHead,
			kSpanPrepassArcsAndGate,
			kSpanPrepassPerFrame,
			kSpanTrenchTicks,
			kSpanTrenchSweep,
			kSpanTrenchDrain,
			kSpanTrenchFlush,
			kSpanTrenchRoll,
			kSpanTrenchInject,
			kSpanGather,
			kSpanContact,
			kSpanDirtyRows,
			kSpanPolls,
			kSpanIdleLogic,
			kSpanDispatch,
			kSpanPrepassTail,
			kSpanContactActor,
			kSpanContactUpload,
			kSpanContactDynamic,
			kSpanContactDraw,
			kSpanContactGeometry,
			kSpanContactSkinned,
			kSpanExclusionGather,
			kSpanCount
		};
		static constexpr const char* kSpanNames[kSpanCount] = { "Prepass", "DrawShell", "DrawShell.Setup", "DrawShell.ObjectHeightMap", "DrawShell.WaterCapture", "DrawShell.WaterCover", "DrawShell.BloodCapture", "DrawShell.RuneCapture", "DrawShell.Ground", "DrawShell.Statics", "DrawShell.Restore", "DrawShell.Tail",
			"Prepass.Head", "Prepass.ArcsAndGate", "Prepass.PerFrame", "Prepass.TrenchTicks", "Prepass.TrenchSweep", "Prepass.TrenchDrain", "Prepass.TrenchFlush", "Prepass.TrenchRoll", "Prepass.TrenchInject", "Prepass.Gather", "Prepass.Contact", "Prepass.DirtyRows", "Prepass.Polls", "Prepass.IdleLogic", "Prepass.Dispatch", "Prepass.Tail",
			"Contact.Actor", "Contact.Upload", "Contact.Dynamic", "Contact.Draw", "Contact.Geometry", "Contact.Skinned", "DrawShell.ExclusionGather" };
		int64_t spanTicks[kSpanCount] = {};
		uint32_t spanCalls[kSpanCount] = {};
		/** @brief Cumulative: every stamp uploaded, the carve (non-melt) ones MarkTrenchDirtyRows saw, and rows it newly marked dirty. */
		uint64_t stampsTotal = 0;
		uint64_t carveStampsTotal = 0;
		uint64_t rowsMarkedTotal = 0;
		/** @brief Cumulative raster-candidate (contact capture) rectangles that marked rows. */
		uint64_t contactRectsTotal = 0;
		/** @brief Cumulative marks skipped because their source had settled (unchanged key past the settle tail). */
		uint64_t settledSkipsTotal = 0;
		/** @brief Actors that emitted stamps: this frame, and the latest frame that had any. Props and spells are not attributed. */
		struct StampSource
		{
			uint32_t formID;
			uint32_t stamps;
		};
		static constexpr uint32_t kStampSourceMax = 16;
		StampSource stampSources[kStampSourceMax] = {};
		uint32_t stampSourceCount = 0;
		StampSource lastStampSources[kStampSourceMax] = {};
		uint32_t lastStampSourceCount = 0;
		uint32_t lastStampSourceFrame = 0;
		/** @brief Cumulative StoreTrenchBand calls and texel raises per caller path (set in trenchStorePath before each call). */
		enum StorePath : uint32_t
		{
			kStoreRoll,
			kStoreRollStale,
			kStoreBand,
			kStoreBandStale,
			kStoreJump,
			kStorePathCount
		};
		static constexpr const char* kStorePathNames[kStorePathCount] = { "roll", "rollStale", "band", "bandStale", "jump" };
		uint64_t storeCalls[kStorePathCount] = {};
		uint64_t storeRaises[kStorePathCount] = {};
		uint32_t trenchStorePath = kStoreRoll;
		/** @brief Per-frame history for stutter hunts (DevBench snowdeformation.cpucensus history): each frame's period, roll to roll, and every span's raw ms while armed. Written by the render thread, read by DevBench's, under frameRingLock. */
		static constexpr uint32_t kFrameRing = 1200;
		struct FrameRow
		{
			uint32_t frame;
			float periodMs;
			float hookMs, actorMs, landMs;
			float spanMs[kSpanCount];
		};
		FrameRow frameRing[kFrameRing] = {};
		uint32_t frameRingHead = 0;
		uint32_t frameRingCount = 0;
		std::mutex frameRingLock;
	};
	static inline CpuCensus cpuCensus;
	static inline bool cpuCensusArmed = false;
	uint32_t cpuCensusArmedUntil = 0;
	struct CpuCensusShown
	{
		float hookMs = 0.0f, hookCalls = 0.0f, actorMs = 0.0f, actors = 0.0f, landMs = 0.0f, landCalls = 0.0f;
		float depthMs = 0.0f, depthCalls = 0.0f, traverseMs = 0.0f, traverseCalls = 0.0f;
		float skinLoopDraws = 0.0f, skinLoopCBUpdates = 0.0f;
		float spanMs[CpuCensus::kSpanCount] = {};
		float spanCalls[CpuCensus::kSpanCount] = {};
	};
	CpuCensusShown cpuShown;
	// rdtsc, not QPC: two QPC reads are ~50 ns and the hook runs thousands of
	// times a frame, so the instrument was a fifth of what it measured.
	// Calibrated against QPC once per frame in RollCpuCensus.
	/** @brief Counts only while the census is armed (read by the menu or DevBench within the last 120 frames). */
	struct ScopedTicks
	{
		int64_t& acc;
		uint32_t& calls;
		uint64_t t0;
		ScopedTicks(int64_t& a_acc, uint32_t& a_calls) :
			acc(a_acc), calls(a_calls), t0(cpuCensusArmed ? __rdtsc() : 0) {}
		~ScopedTicks()
		{
			if (t0) {
				acc += int64_t(__rdtsc() - t0);
				calls++;
			}
		}
	};
	/** @brief Consecutive segments: each Lap charges the time since the previous lap (or construction) to one span. */
	struct SpanClock
	{
		uint64_t t = __rdtsc();
		void Lap(CpuCensus::Span a_span)
		{
			const uint64_t now = __rdtsc();
			cpuCensus.spanTicks[a_span] += int64_t(now - t);
			cpuCensus.spanCalls[a_span]++;
			t = now;
		}
	};
	template <class F>
	static void TimedTraverse(RE::NiAVObject* a_root, F&& a_visitor)
	{
		ScopedTicks _t(cpuCensus.traverseTicks, cpuCensus.traverseCalls);
		// By reference: the walk copies its std::function at every node.
		RE::BSVisit::TraverseScenegraphCollision(a_root, std::ref(a_visitor));
	}
	void RollCpuCensus();
	uint32_t clusterSkinsLast = 0;
	uint32_t clusterScratchUsedLast = 0;
	std::unordered_map<void*, SmoothedNormalsEntry> smoothedNormalsCache;
	ID3D11ComputeShader* smoothAccumulateCS = nullptr;
	ID3D11ComputeShader* smoothResolveCS = nullptr;
	/** @brief Third pass: one-group reduction writing the mesh's flat/rounded classification (fraction of smoothed-vs-raw divergent vertices) into the stats element appended at SmoothedNormals[vertexCount]. */
	ID3D11ComputeShader* smoothFlatStatsCS = nullptr;
	ConstantBuffer* smoothCB = nullptr;

	/** @brief The smoothed-normal entry for a captured geometry, built on first sight (SmoothNormalsCS passes) and cached thereafter; usable once ready. Null without one (the VS falls back to raw normals). Implemented in SnowDeformation/Statics.cpp. */
	SmoothedNormalsEntry* EnsureSmoothedNormals(RE::BSGeometry* a_geometry);
	/** @brief EnsureSmoothedNormals for a capture, looked up once per frame. */
	SmoothedNormalsEntry* SmoothedNormalsFor(CapturedSnowStatic& a_cap);
	/** @brief Bumped by ResetSmoothedNormals: entries a capture kept from before it are gone. */
	uint32_t smoothedNormalsResets = 0;
	/** @brief ONE StaticsCB recipe for both the visible skin draw and the shadow caster - the caster must be the exact surface the shell renders, and a drifted copy of this fill would be the CB-mirror class of bug. Presence flags come from the call site (the caster looks resources up without creating them). */
	void FillSkinDrawCB(const CapturedSnowStatic& a_cap, bool a_s4Shell, float a_vertexCount, bool a_hasSmoothedNormals, bool a_hasObjectTop, StaticsCB& a_scb) const;
	/** @brief ONE StaticsCB recipe for the trench patch, shared by the visible patch draw and its shadow caster for the same drift reason as FillSkinDrawCB. Implemented in SnowDeformation/Statics.cpp. */
	void FillPatchDrawCB(StaticsCB& a_scb) const;

	// ---- Top-down object height windows ----

	// Texels a side of the top-down object height windows. A window spans
	// 2 x ObjectRasterHalfExtent() around the camera, so its texel follows
	// the Object Height Map Range: 6.8 world units at the 100 m default.
	static constexpr uint kHeightMapDim = 2048;
	/** @brief Repose slope of the object snow cone (rise per world unit); sets the road verge bank down to a shallower landscape class. The retired Mound Steepness at its last default. */
	static constexpr float kObjectConeSlope = 3.0f;
	/** @brief The live half-extent: Settings::ObjectRasterReachM in world units. Every window fill, texel derivation and shader mirror must take it from here - the texel size is derived from it, so a call site left on the constant addresses a different grid. */
	float ObjectRasterHalfExtent() const { return std::clamp(settings.ObjectRasterReachM, 29.0f, 234.0f) * kUnitsPerMeter; }
	/** @brief Horizontal camera distance (units) at which a non-exempt skin has dissolved whole (ShellCB::SkinFadeEnd). */
	float SkinFadeEndUnits() const { return settings.RangeSkinsM * kUnitsPerMeter; }
	/** @brief The player is in a child worldspace (a walled city); sampled once per frame in Prepass. */
	bool cityWorldspace = false;
	/** @brief Landscape shell, road class and patch are off this frame (city, CityShells off). */
	bool GroundShellsSuspended() const { return cityWorldspace && !settings.CityShells; }
	/** @brief 2^(1 - Berm Steepness): the berm tap reach both the bake and the shells use. Exactly 1 at the default. */
	float BermReachScale() const { return std::exp2(1.0f - std::clamp(settings.BermSteepness, 0.0f, 2.0f)); }
	/** @brief 1 - Snow Resistance: the share of a body's reach into the layer that carves. */
	float BodyPrintScale() const { return 1.0f - std::clamp(settings.SnowResistance, 0.0f, 100.0f) * 0.01f; }
	/** @brief Interior cell: no landscape exists, so no ground shell and no trench can; object snow is unaffected. */
	bool InInteriorCell() const
	{
		auto* tes = globals::game::tes;
		return tes && tes->interiorCell;
	}
	/** @brief The reach the accumulated maps were built at; a change re-texels the grid, so the scroll cannot carry them and the window clears instead. */
	float objectRasterHalfExtentBuilt = 0.0f;

	/**
	 * @brief Near clipmap level: the same 2048 grid over a QUARTER of the
	 * reach, so its texel is a quarter of the coarse level's.
	 *
	 * Single-buffered on purpose - it is rebuilt from this frame's captures
	 * every frame and carries no ghost. The coarse level keeps the history,
	 * and every reader falls back to it outside the fine window, which is
	 * also what makes the fine level free of the scroll/decay machinery.
	 * Its centre IS the coarse window's: a coarse texel is four fine ones, so
	 * one snap serves both grids and only the half-extent has to reach the shaders.
	 */
	/** @brief A QUARTER of the coarse reach, always: the two levels then sit a fixed factor of four apart however far the coarse one is asked to reach, so raising the reach coarsens the near field by the same factor it coarsens the far one instead of stranding it. At the 100 m default that is 25 m at 1.7 world units a texel. */
	float FineRasterHalfExtent() const { return ObjectRasterHalfExtent() * 0.25f; }
	/** @brief Height sentinels for texels no object covers. */
	static constexpr float kHeightMapEmptyTop = -100000.0f;
	static constexpr float kHeightMapEmptyBottom = 100000.0f;

	/** @brief Peel tolerance: surfaces within this many units below a peeled layer's top belong to that layer's plane. Was the "Plane Merge Height" slider, retired 2026-09-06 at its default. */
	static constexpr float kPeelTol = 8.0f;
	/** @brief Cone seed rim threshold (world units). Was "Plane Split Step", retired 2026-09-10 at its default. */
	static constexpr float kRimStep = 6.0f;
	/** @brief Cover above a plane that neither splits it nor demotes it (world units). Was "Ignore Cover Above", retired 2026-09-10 at its default. */
	static constexpr float kOverheadIgnore = 0.0f;
	/** @brief Top of the enclosed-shelter band (world units, fades over the next 100): a tent ridge is inside it. */
	static constexpr float kShelterBandHeight = 250.0f;
	/** @brief Cone settling, the 4-neighbour Jacobi lambda at its stability bound. Was "Snow Settling" at 100%, retired 2026-09-10. */
	static constexpr float kDiffuseLambda = 0.5f;

	/** @brief Ping-pong accumulated raw maps (scrolled each frame, captures rasterized on top): object TOP and BOTTOM surfaces. Persistence matters; the capture list is frustum-culled, and a map rebuilt from it alone loses every object behind the camera. */
	Texture2D* heightTopRaw[2] = { nullptr, nullptr };
	Texture2D* heightBottomRaw[2] = { nullptr, nullptr };
	/** @brief Layer 1's raster this frame (R32G32, RT0 MAX on both channels): x = top, y = the negated bottom. The merge (heightScrollRasterCS, t6) reads it into heightTopRaw/heightBottomRaw. */
	Texture2D* heightRaster = nullptr;
	/** @brief Processed maps: the smooth shelter/suppression mask the shell samples (t5; its size sets CombineCS's dispatch and FieldDims.z), and the object cone chains' ping-pong scratch. */
	Texture2D* heightBottomFiltered = nullptr;
	Texture2D* heightScratch = nullptr;
	/** @brief Cone-transformed snow SURFACE height over the object top raster; the skin's edge taper reads it with one tap. */
	Texture2D* objectSnowCone = nullptr;
	/** @brief The peeled second layer: ping-pong accumulated raw top of the highest up-facing surface more than kPeelTol below layer 1 per column. The layer-1 cone seed reads it as the plane-continuation test behind every upward rim. */
	Texture2D* heightTop2Raw[2] = { nullptr, nullptr };
	/** @brief P3: per-column sky openness (1 = open sky) baked from the layer-1 tops at half the raster's resolution, for the volume's seed. */
	Texture2D* objectSkyOpen = nullptr;
	/** @brief Per 8x8 tile of the layer-1 raw maps, 1 where no texel holds both a top and a bottom: the layer-1 merge writes it (u3), the shelter mask skips tiles whose 5x5 block is all 1 (t4). */
	Texture2D* heightTileEmpty = nullptr;
	/** @brief Near clipmap level 0: the layer-1 top at one world unit per texel (t34). Single, not ping-pong - rebuilt from this frame's captures with no ghost, because the coarse level owns the history and every reader falls back to it outside FineRasterHalfExtent(). */
	Texture2D* heightTopRawFine = nullptr;
	/** @brief Far road level (t40): the road captures alone, MAX-rasterized over the loaded-cell square every frame. Single, no ghost - roads never move, and an off-screen road needs no patch. Window in farWindowCenter / farHalfExtentLive (0 = off this frame). */
	Texture2D* heightTopRawFar = nullptr;
	float2 farWindowCenter = { 0, 0 };
	float farHalfExtentLive = 0.0f;
	/** @brief The far road level can be built (with the road maps): PatchTop and RoadOwnsColumn read it outside the coarse window, so road snow runs to the loaded grid's edge instead of handing back to the flat skin at ObjectRasterReachM. */
	bool FarLevelActive() const { return heightTopRawFar && settings.RoadMeshesDepth > 0.5f; }
	/** @brief Cone passes the chain ran this frame (of kConeSteps). */
	uint32_t conePassesCoarse = 0;
	/** @brief A/B, runtime-only: the cones run as separate seed and settle passes even where ConeFusedCS could take them. */
	bool coneFusedOff = false;
	/** @brief Debug, runtime-only: every fused or kept cone is recomputed by the separate passes into coneVerifyTex and compared bit for bit (heightRasterVerifyCount slots 4 and 5). */
	bool coneVerify = false;
	uint32_t coneRuns = 0;
	uint32_t coneFusedRuns = 0;
	/** @brief The cone verify's reference target (R32F, created on first use). */
	Texture2D* coneVerifyTex = nullptr;
	bool EnsureConeVerify();
	/** @brief A/B, runtime-only: every ghost with a top and a bottom keeps its tile elevated (HeightProcessCB::DeadGhostTiles 0). */
	bool deadGhostTilesOff = false;
	/** @brief Debug, runtime-only: CombineCS runs a second time with no tile flags (t4 unbound never skips) into combineVerifyTex, compared bit for bit (slot 6). */
	bool combineVerify = false;
	Texture2D* combineVerifyTex = nullptr;
	bool EnsureCombineVerify();
	/** @brief Only road skins, the trench patch, the march's road taps and the debug views read the cones and the near level: true when one of them can this frame. */
	bool RoadMapsWanted() const;
	/** @brief The sky-openness field holds the last height-map run's bake (Volume Snow reads that one), so a run that turns the bake back on need not bake it again before the volume draws. */
	bool skyOpenCurrent = true;
	/** @brief Frame the Debugging Options height probe readout last drew (its cone line needs the cone built). */
	uint32_t heightProbeShownFrame = UINT32_MAX;
	bool FineLevelOn() const { return settings.ObjectSnowNearDetail; }
	static constexpr int kQualityPresetCount = 3;
	/** @brief Quality presets, Low (0) to High (2); High is the shipped defaults. Sets every preset lever; the rest of the settings keep their values. */
	void ApplyQualityPreset(int a_preset);
	/** @brief The preset every lever currently matches, or -1 (custom). */
	int MatchingQualityPreset() const;
	// ---- Height-field probe (Debugging Options): the six object maps read
	// back at the player's texel every frame, so a report carries numbers
	// instead of guesses. Ping-pong staging; the value shown is one frame old.
	winrt::com_ptr<ID3D11Texture2D> probeStaging[2];
	uint probeCursor = 0;
	/** @brief Sampled values: 0 = L1 top, 1 = L2 top, 2 = cone1. Sentinels pass through raw. */
	float probeVals[6] = {};
	bool probeValid = false;
	float3 probeWorldPos = {};
	/** @brief Per-frame skin-depth raster (R16F, cleared each frame, MAX-blended): each captured mesh writes its class layer depth, so consumers know how thick the snow above any object top is. No scroll persistence; a missed frame is invisible for one frame. */
	Texture2D* heightSkinDepth = nullptr;
	uint heightCurrent = 0;
	bool heightMapValid = false;
	/** @brief The current object maps are an object-free window's: sentinel tops and bottoms, empty tiles, a zero cone. While the window stays free a run keeps them. */
	bool heightMapsEmpty = false;
	float2 heightWindowCenter = { 0, 0 };
	/** @brief Depth range of the game's main-pass viewport, sampled once per frame after the first deferred-pass lighting draw (NoteMainViewport). The deferred span ends on the blended-decal viewport (max depth 0.999968 against the pass's 0.999998), and a shell drawn through that cap sits ~3e-5 NDC nearer than its own object. */
	float mainViewportMinDepth = 0.0f;
	float mainViewportMaxDepth = 1.0f;
	uint32_t mainViewportFrame = UINT32_MAX;

	/** @brief RT0 MAX (tops): the highest surface wins per texel in any draw order; no depth buffer needed. The fine, far, peeled and water rasters. */
	winrt::com_ptr<ID3D11BlendState> heightMaxBlendState;
	/** @brief RT0 MAX on R and G: layer 1's heightRaster and the roads' skin depth, each bound alone. */
	winrt::com_ptr<ID3D11BlendState> heightRasterBlendState;
	/** @brief Drawn water's height per terrain-window texel (kShellWindowDim^2, R32F, the terrain window's own frame): MAX of the bodies touching the texel, kShellMissingHeight where none. Rebuilt every frame from last frame's planes (a kept plane poisoned the area until the next cell crossing). Bound at t27 for the landscape shell. */
	Texture2D* waterHeightTexture = nullptr;
	/** @brief The same planes at object-raster resolution (kWaterFineDim over ObjectRasterHalfExtent) for Lighting's underwater veto (t63); rebuilt whenever the list or the height window moves. */
	Texture2D* waterFineTexture = nullptr;
	/** @brief The water window the shell's data route reads (t27): waterHeightTexture minus its lone wet texels (WaterCoverCS), so a single vertex dipping under a cell plane inside a cliff mesh cannot bare a whole texel. The shore cap reads the raw window at t28. */
	Texture2D* waterVisibleTexture = nullptr;
	/** @brief Texels to the nearest land vertex under its water (WaterCoverCS, t19): the shore cap's horizontal term. */
	Texture2D* waterNearTexture = nullptr;
	/** @brief WaterCoverCS reruns when the water raster (waterCoverDirty) or the terrain window (shellTerrainVersion) changed. */
	bool waterCoverDirty = true;
	uint32_t waterCoverTerrainVersion = 0;
	/** @brief Bumped on every write of the water windows bound at t27 (the raster and its covered copy). */
	uint32_t waterWindowVersion = 0;
	void CoverWaterWindow();
	float2 waterFineCenter = { 0, 0 };
	float waterFineHalf = 0.0f;
	bool waterFineValid = false;
	static constexpr int kWaterFineDim = 1024;
	int waterWindowCellX = INT_MIN;
	int waterWindowCellY = INT_MIN;
	/** @brief One water body as the capture loop draws it. The bake skips while the fresh gather matches the last one at the same window cell - the texture already holds the same bits. */
	struct WaterBakeKey
	{
		const void* vb;
		const void* ib;
		uint64_t descKey;
		uint32_t indexCount;
		RE::NiTransform world;
		float planeConstant;
	};
	std::vector<WaterBakeKey> waterBakeKeys;
	std::vector<WaterBakeKey> waterBakeScratch;
	bool waterBakeValid = false;
	ID3D11VertexShader* waterCaptureVS = nullptr;
	ID3D11PixelShader* waterCapturePS = nullptr;
	winrt::com_ptr<ID3DBlob> waterCaptureVSBlob;
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> waterILCache;
	bool waterCaptureShadersFailed = false;
	/** @brief Last consume: planes in the list, planes that drew, and whether the first plane's facts were logged (once). */
	uint32_t statWaterCaptured = 0;
	uint32_t statWaterDrawn = 0;
	bool waterFirstLogged = false;
	void RenderWaterCapture();
	ID3D11VertexShader* heightVS = nullptr;
	/** @brief Layer 1's capture PS: top and negated bottom into heightRaster through RT0. */
	ID3D11PixelShader* heightPS = nullptr;
	/** @brief Capture PS writing the top alone (TOP_ONLY): the fine and far rasters bind RT0 only. */
	ID3D11PixelShader* heightPSTopOnly = nullptr;
	/** @brief Capture PS writing the roads' skin depth alone (SKIN_DEPTH) into heightSkinDepth through RT0. */
	ID3D11PixelShader* heightPSSkinDepth = nullptr;
	/** @brief S4 phase 2: the layer-2 peel PS (SnowHeightCapture.hlsl, PEEL define) - keeps only up-facing fragments below this frame's layer-1 top by the peel tolerance, MAX-blending the second-highest snow-bearing surface per column. */
	ID3D11PixelShader* heightPeelPS = nullptr;
	ID3D11ComputeShader* heightScrollCS = nullptr;
	/** @brief ScrollCS with SCROLL_TOP_ONLY, for the peeled layers. */
	ID3D11ComputeShader* heightScrollTopCS = nullptr;
	/** @brief ScrollCS with SCROLL_RASTER_RG: layer 1's merge, its raster read from heightRaster (t6). */
	ID3D11ComputeShader* heightScrollRasterCS = nullptr;
	/** @brief Per raster level (0 = layer 1 with the skin depth, 1 = fine, 2 = far): what its targets were last drawn from (every draw's binds and record bytes, the inherited state, shaders and terrain window). A run whose key matches keeps the targets; the ghost merge still runs. */
	std::vector<uint8_t> heightRasterKey[3];
	/** @brief A/B, runtime-only: every level redraws every run, and an object-free window reruns its merges and cone (heightMapsEmpty). */
	bool heightRasterCacheOff = false;
	/** @brief Debug, runtime-only: a kept level (and with heightRasterVerifyMiss a redrawn one) is drawn again into scratch targets and compared bit for bit; totals read back without stalling. */
	bool heightRasterVerify = false;
	bool heightRasterVerifyMiss = false;
	uint32_t heightRasterKeptBits = 0;
	uint32_t heightRasterRuns = 0;
	uint32_t heightRasterKept[3] = {};
	/** @brief Verified runs per level: layer 1, fine, far, object cone, road cone, shelter mask. */
	uint32_t heightRasterVerified[6] = {};
	/** @brief Mismatching texels per compared target: layer-1 raster, skin depth, fine, far, object cone, road cone, shelter mask. */
	uint64_t heightRasterMismatch[7] = {};
	Texture2D* heightRasterVerifyTex[4] = {};
	winrt::com_ptr<ID3D11Buffer> heightRasterVerifyCount;
	winrt::com_ptr<ID3D11UnorderedAccessView> heightRasterVerifyAllUAV;
	winrt::com_ptr<ID3D11UnorderedAccessView> heightRasterVerifySlotUAV[7];
	winrt::com_ptr<ID3D11Buffer> heightRasterVerifyStaging[3];
	uint32_t heightRasterVerifyStagingLevels[3] = {};
	uint32_t heightRasterVerifyRing = 0;
	ID3D11ComputeShader* heightRasterCompareCS = nullptr;
	struct HeightRasterSample
	{
		uint32_t target, x, y;
		float keptX, keptY, freshX, freshY;
	};
	/** @brief The last readback's first differing texels; guarded by heightRasterDebugLock (DevBench reads it on its own thread). */
	std::vector<HeightRasterSample> heightRasterSamples;
	std::mutex heightRasterDebugLock;
	bool EnsureHeightRasterVerify();
	ID3D11ComputeShader* heightCombineCS = nullptr;
	/** @brief WaterCoverCS: the raw water window minus its lone wet texels into waterVisibleTexture, and the near field into waterNearTexture. */
	ID3D11ComputeShader* heightWaterCoverCS = nullptr;
	ID3D11ComputeShader* objectConeSeedCS = nullptr;
	ID3D11ComputeShader* objectConeCS = nullptr;
	/** @brief P3: bakes the half-res sky-openness field from the layer-1 tops (ObjectSkyOpenCS). */
	ID3D11ComputeShader* objectSkyOpenCS = nullptr;
	/** @brief P4: one Jacobi settling iteration over a cone depth field (ObjectConeDiffuseCS). */
	ID3D11ComputeShader* objectConeDiffuseCS = nullptr;
	/** @brief ConeFusedCS: a cone with no repose step, seed and both settles in one dispatch. */
	ID3D11ComputeShader* objectConeFusedCS = nullptr;

	/** @brief Per-dispatch constants for the height-window processing. Layout must match HeightProcessCB in HeightMapProcessCS.hlsl. */
	struct alignas(16) HeightProcessCB
	{
		DirectX::XMINT2 ScrollDelta;
		uint ClearAll;
		/** @brief Texel step for the current cone iteration. */
		uint ConeStep;

		float2 HeightWindowCenter;
		float HeightHalfExtent;
		/** @brief Max cone rise per world unit (1.0 = 45 degrees): kObjectConeSlope. */
		float SlopePerUnit;

		/** @brief Terrain window addressing so the compute passes can sample ground heights. */
		float2 TerrainWindowOrigin;
		float TerrainTexelSize;
		uint TerrainDim;

		/** @brief Units/frame the accumulated tops/bottoms drift toward empty; stale object imprints (disabled/moved/harvested) melt instead of persisting until scrolled out. */
		float GhostDecay;
		/** @brief Rounded-class snow depth, seeding the object snow cone. */
		float ObjectSnowDepth;
		/** @brief kRimStep - the cone seed's slope-discontinuity rim threshold. */
		float RimStep;
		/** @brief kOverheadIgnore - the seed's rise-rim upper bound: surfaces further above do not split the plane when this plane continues beneath them (the next layer's top says). */
		float OverheadIgnore;

		/** @brief Top of the enclosure band (units), >= ShelterMaxHeight. */
		float ShelterBandHeight;
		/** @brief kDiffuseLambda - per-iteration Jacobi blend toward the 4-neighbour average over the finished cone fields. */
		float DiffuseLambda;
		/** @brief A floating structure whose underside is this far above the ground stops sheltering it (fade over the next 100 units). Mirror in HeightMapProcessCS.hlsl. */
		float ShelterMaxHeight;
		/** @brief tan(ShelterDriftAngle), always > 0: sizes the enclosure disc. */
		float ShelterConeTan;

		/** @brief Layout keeper, uploaded as 0. */
		uint keeper;
		/** @brief ScrollCS: a ghost whose bottom has risen ShelterBandHeight + 61 over its top no longer marks its tile elevated for CombineCS (0 = every floating texel does). */
		uint DeadGhostTiles;
		uint pad1;
		uint pad2;
	};
	STATIC_ASSERT_ALIGNAS_16(HeightProcessCB);

	ConstantBuffer* heightProcessCB = nullptr;

	/** @brief ScrollCS only: the camera clip transform, so the ghost merge can ask whether the surface that wrote a stale texel would still have been submitted this frame. Its own buffer, not a HeightProcessCB growth - that block is prefix-mirrored by the capture PS. Layout must match GhostCB in HeightMapProcessCS.hlsl. */
	struct alignas(16) HeightGhostCB
	{
		Matrix GhostViewProj;
		float4 GhostCameraPosAdjust;
	};
	STATIC_ASSERT_ALIGNAS_16(HeightGhostCB);
	ConstantBuffer* heightGhostCB = nullptr;

	// ---- Voxel occupancy volume (VOLUME-SNOW-PLAN V0) ----

	/** @brief 256 voxels a side; pow2 for the torus. The voxel SIZE is Settings::VolumeVoxelSize, so the cube's reach and its detail trade against each other at fixed memory - which is what a clipmap would break. Mirrors Dim in SnowVoxelCapture.hlsl. */
	static constexpr uint kVoxelDim = 256;
	/** @brief A wide ring's voxels a side in x and y (Settings::VolumeRingShape), and the flat shape's in z. */
	static constexpr uint kVoxelDimWide = 512;
	static constexpr uint kVoxelDimFlatZ = 128;
	/** @brief Voxels a side per axis for a level: the shape's, halved per axis on the far rings. */
	DirectX::XMUINT3 VoxelDimsForLevel(uint a_level) const;
	/** @brief The largest dims any level uses (the shape's fine dims): the shared support volume's size. */
	DirectX::XMUINT3 VoxelDimsMax() const;
	/** @brief The window's extent in world units per axis: shape dims * base voxel * 2^L whatever the grid size. */
	DirectX::XMFLOAT3 VoxelExtentForLevel(uint a_level) const;
	/** @brief Live voxel size in world units; changing it invalidates the accumulated volume (the torus origin is in voxel units). */
	float VoxelSizeLive() const { return std::clamp(settings.VolumeVoxelSize, 1.0f, 24.0f); }
	float voxelSizeBuilt = 0.0f;

	/** @brief Layout must match VoxelCB in SnowVoxelCapture.hlsl. */
	struct alignas(16) VoxelVolumeCB
	{
		/** @brief xyz = window origin in voxels, w = clear all. */
		DirectX::XMINT4 OriginVox;
		/** @brief xyz = this frame's origin minus last frame's. */
		DirectX::XMINT4 ScrollDelta;
		float VoxelSize;
		/** @brief Occupancy lost per frame; a voxel not re-rasterised fades out. */
		float Decay;
		int padDim;
		int SliceAxis;
		int SliceIndex;
		/** @brief >0: the slice is a max over the whole fixed axis (silhouettes), not one plane. */
		int SliceXray;
		/** @brief 0 occupancy, 1 the snow field, 2 the field at the threshold. */
		int SliceSource;
		int BlurAxis;
		/** @brief The height window, for the seed gate's shelter/sky maps; HalfExtent 0 = no maps bound, treat every column as open sky. */
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		/** @brief Seed weight under shelter: a dusting, not bare. */
		float ShelterDust;
		/** @brief Settings::VolumeSnowDepth in this level's voxels, unfloored: the field is a ramp about the true snow top (surface height + depth), so a depth under a voxel still reads at height on a coarse ring. */
		float DepthVox;
		/** @brief 0.5: the ramp's crossing. */
		float FieldThreshold;
		/** @brief Sideways averaging sigma in voxels, from Settings::VolumeSnowRounding: the shoulder at an edge. */
		float RoundSigma;
		/** @brief cos(Settings::VolumeSnowMaxSlopeDeg): the least up-ness a seed's surface may have, read from the normal the raster packed into the voxel. */
		float SlopeMinNz;
		/** @brief xyz = this window's centre in voxel units (absolute; ahead of the camera by Settings::VolumeForwardBias), w = the bricks' reach from it in voxels, Chebyshev. */
		float CentreVox[4];
		/** @brief Settings::VolumeSkyExposurePct / 100 - strength of the sky-openness weighting on the seed. */
		float SkyStrength;
		/** @brief 1: every brick column is rebuilt this time (Dirty Bricks off, the periodic refresh, or a level with nothing built). */
		float ForceDirty;
		/** @brief Settings::VolumeSnowOverhang in this level's voxels, rounded: the cap's reach past a snow column. */
		float OverhangVox;
		/** @brief kVoxelHeadroomUnits in this level's voxels, at least 1: air a seed needs above it. */
		float HeadroomVox;
		/** @brief x = 0 (Edge Noise retired; the lip's end no longer wanders), y = kVoxelEdgeNoiseCell, z = the field's exponential rate per voxel (ln 2 over the Rounding in voxels, the Rounding floored at two voxels so the crossing interpolates true), w = Settings::VolumeSnowOverhang (world units). */
		float EdgeParams[4];
		/** @brief x = an air neighbour's weight in the sideways average: 1 where the voxel is small against the Rounding (dilution shapes the dome and the lip), 0 on the far rings (a rim keeps its slab). y, z, w spare. */
		float LipParams[4];
		/** @brief xyz = voxels a side per axis (pow2 each); w unused. */
		int Dims[4];
	};
	STATIC_ASSERT_ALIGNAS_16(VoxelVolumeCB);
	DirectX::XMFLOAT3 VoxelLevelCentre(uint a_level) const;
	/** @brief Frames between a level's rebuilds: 2^L. */
	uint32_t VoxelLevelPeriod(uint a_level) const;
	/** @brief The field's value at the snow top on this level: Coverage times a gain (up to 8) where air neighbours do not vote, so a far ring's third-of-a-voxel layer is not three R8 steps; capped so the threshold stays under 0.5. */
	float VoxelThresholdForLevel(uint a_level) const;
	/** @brief Frames rendered with the volume on; the memory nibble ticks down every memorySeconds*60/15 of them. */
	uint32_t voxelFrame = 0;
	/** @brief Bricks and the draw reach this fraction of a level's half-extent; the rest is the blur-truncated edge plus the origin's brick snap (up to 4 voxels). Chebyshev, so the rings are cube shells and tile without gaps. The far grids have fewer voxels of slack, so a smaller fraction. */
	static constexpr float kVoxelReachFrac = 0.9f;
	static constexpr float kVoxelReachFracFar = 0.85f;
	/** @brief Byte offsets of the partial passes' DispatchIndirect args in VoxelLevel::lists, then the three column lists (1024 entries each); mirrors LIST_* in SnowVoxelCapture.hlsl. */
	static constexpr uint kVoxelListArgsSeed = 0;
	static constexpr uint kVoxelListArgsZ = 12;
	static constexpr uint kVoxelListArgsX = 24;
	static constexpr uint kVoxelListArgsY = 36;
	static constexpr uint kVoxelListArgsFlags = 48;
	static constexpr uint kVoxelListHeaderBytes = 64;
	static constexpr uint kVoxelListBytes = 64 + 3 * 16384;
	/** @brief Every this-many rebuilds of a level are whole: the seed weights read the shelter/sky maps, which change without the occupancy changing. Rare on purpose: a whole rebuild also re-reads every voxel's captured height and facing, and overlapping triangles write those in GPU-arbitrary order, so each re-capture can differ by a hair - the incremental path never looks, the whole one does, and the snow shifts (Josef, 2026-09-07). The real fix is a deterministic capture (ordered writes or atomics); until then, 900 = 15 s on the finest ring. */
	static constexpr uint32_t kVoxelFullRefreshRebuilds = 900;
	ID3D11ComputeShader* voxelOccFlagsCS = nullptr;
	ID3D11ComputeShader* voxelDecayCS = nullptr;
	ID3D11ComputeShader* voxelDirtyColsCS = nullptr;
	ID3D11ComputeShader* voxelBrickFlagsCS = nullptr;
	/** @brief The last fraction of that reach is the hand-over band: the level dithers out over it while the level outside dithers in, complementary per pixel. The outermost dithers to nothing. */
	static constexpr float kVoxelBandFrac = 0.2f;
	/** @brief World units per cell of the edge noise (the cap's reach wanders per column so the snow's edge is not the object's edge traced straight). No sigma floors any more: the ramp field reads at height on any ring, and equal-height ramps interpolate flat between columns, so neither reason for them remains. */
	static constexpr float kVoxelEdgeNoiseCell = 12.0f;
	/** @brief Air a seed needs above it, world units: a roof's or a beam's underside shell has its member's own inside above it, one voxel of air, and seeded snow that grew out through the eave. */
	static constexpr float kVoxelHeadroomUnits = 8.0f;
	/** @brief Layers under the top the heightmap keeps: a porch, a roof, a sill, a roof again, and one spare (Josef, 2026-09-24). Mirrors VOXEL_UNDER_LAYERS in SnowVoxelCapture.hlsl and SnowStaticsShell.hlsl. */
	static constexpr uint kVoxelUnderLayers = 4;
	static constexpr uint kVoxelMaxLevels = 6;
	/** @brief One clipmap level: occupancy ping-pong, its blob field, and the draw's brick list. Level L's voxel is VoxelSizeLive() * 2^L. */
	struct VoxelLevel
	{
		Texture3D* volume[2] = { nullptr, nullptr };
		Texture3D* field = nullptr;
		/** @brief R32_UINT, (4 dim)^2, world-anchored like the volume: the topmost captured surface over each sub-pixel as the bits of (world z + 32768) with the surface's up-ness in the low four bits, kept by an atomic max, so a column's height is deterministic and prefiltered over sixteen samples. */
		Texture2D* heightMap = nullptr;
		/** @brief R32_UINT, (2 dim)^2, kVoxelUnderLayers slices: layer k the highest up-facing surface a headroom or more below layer k - 1 (the top map being layer 0), the same key and max; the seed's facing under cover. */
		Texture2D* heightMapUnder = nullptr;
		Buffer* bricks = nullptr;
		Buffer* drawArgs = nullptr;
		/** @brief Dirty bricks: a uint per physical brick, set by the scroll (reused slots) and the occupancy compare, cleared each rebuild. */
		Buffer* dirty = nullptr;
		/** @brief A uint per physical brick: its sub-cell crossing mask, kept between rebuilds, recomputed on dirty columns only. */
		Buffer* flags = nullptr;
		/** @brief The partial passes' dispatch args and column lists (kVoxelListBytes); the header is rewritten each rebuild. */
		Buffer* lists = nullptr;
		/** @brief Rebuilds so far; every kVoxelFullRefreshRebuilds-th is forced whole. */
		uint32_t rebuilds = 0;
		/** @brief This rebuild started the level from nothing, so every record must capture, staggering or not. */
		bool cleared = false;
		uint current = 0;
		/** @brief Voxels a side per axis the textures were made with; a mismatch with VoxelDimsForLevel remakes them. */
		DirectX::XMUINT3 dims = { 0, 0, 0 };
		bool valid = false;
		/** @brief This frame rebuilt the level's occupancy, so its field and bricks are due too. */
		bool updated = false;
		/** @brief This frame was a rebuild frame the idle gate skipped: the draw list still refreshes. */
		bool skipped = false;
		/** @brief The idle gate's memory of the last rebuild's inputs, and how many rebuild frames in a row they have held. */
		uint64_t captureHashLast = 0;
		uint32_t captureCountLast = 0;
		uint64_t exclusionHashLast = 0;
		uint64_t cbHashLast = 0;
		uint32_t stableRebuilds = 0;
		DirectX::XMINT3 origin = { 0, 0, 0 };
	};
	VoxelLevel voxelLevels[kVoxelMaxLevels];
	uint VoxelLevelsLive() const { return (uint)std::clamp(settings.VolumeLevels, 1, (int)kVoxelMaxLevels); }
	float VoxelSizeForLevel(uint a_level) const;
	/** @brief Where a level's bricks and draw end (kVoxelReachFrac) and where its hand-over band starts (kVoxelBandFrac before that), Chebyshev units from the camera. */
	void VoxelReachBand(uint a_level, float& a_start, float& a_end) const;
	/** @brief Fills every per-level field of the volume CB from live state; ScrollDelta and the clear flag are the caller's. Implemented in SnowDeformation/Volume.cpp. */
	void FillVoxelCB(uint a_level, VoxelVolumeCB& a_cb) const;
	/** @brief Runtime-only: which level the slice shows. */
	int voxelSliceLevel = 0;

	/** @brief V1b draw constants (b2 on the VOXEL variant of SnowStaticsShell). Layout must match VoxelDrawCB there. */
	struct alignas(16) VoxelDrawCB
	{
		/** @brief xyz = window origin in voxels. */
		DirectX::XMINT4 VoxOrigin;
		/** @brief x voxel size, y dim, z field threshold, w march step in voxels. */
		float4 VoxParams;
		/** @brief xy = the inner hand-over band (the finer level's outer band; below zero on level 0), zw = this level's outer band - Chebyshev units from the respective window centre. Per pixel one level owns the band: this one where the noise is under its weight, the outer where it is not. */
		float4 VoxFade;
		/** @brief xyz = this window's centre relative to the camera. */
		float4 VoxCentre;
		/** @brief xyz = the next-finer window's centre relative to the camera. */
		float4 VoxInnerCentre;
		/** @brief x = this level's index, y = 1 to tint the snow by level (showVolumeRings), z = Settings::VolumeMaxDistance, w = its fade width. */
		float4 VoxDebug;
		/** @brief xyz = voxels a side per axis, w = x / z: the factor a wide ring's z distances carry in every Chebyshev test. */
		float4 VoxDims;
	};
	STATIC_ASSERT_ALIGNAS_16(VoxelDrawCB);
	/** @brief Seed weight under a roof/fire: the 2D pipeline's dusting, as a fraction of full. */
	static constexpr float kVoxelShelterDust = 0.1f;

	ID3D11ComputeShader* voxelSeedCS = nullptr;
	ID3D11ComputeShader* voxelBlurZCS = nullptr;
	ID3D11ComputeShader* voxelBlurCS = nullptr;
	// ---- V1b: the draw ----
	ConstantBuffer* voxelDrawCB = nullptr;
	/** @brief Debug view 10 (volume seed): level 0's VoxelVolumeCB for the skin PS at b3. */
	ConstantBuffer* voxelDebugCB = nullptr;
	winrt::com_ptr<ID3D11SamplerState> voxelWrapSampler;
	ID3D11ComputeShader* voxelBrickListCS = nullptr;
	/** @brief The VOXEL variant of SnowStaticsShell: brick AABB VS + marching PS through SkinShadeSurface. Compiled with the other statics variants; optional. */
	ID3D11VertexShader* voxelShellVS = nullptr;
	ID3D11PixelShader* voxelShellPS = nullptr;
	/** @brief Draws the volume snow at the end of the statics pass (targets, depth and every skin binding still live). Implemented in SnowDeformation/Volume.cpp. */
	void DrawVoxelSnow();
	/** @brief Runtime-only: what the slice draws - 0 occupancy, 1 the field, 2 the field at the threshold (the snow). */
	int voxelSliceSource = 2;
	/** @brief The menu's plane through the current volume. */
	Texture2D* voxelSliceTexture = nullptr;
	/** @brief The overhang cap's 1D distance between the X and Y passes - ONE volume for every level, at the finest grid: it lives only inside a level's field pass and the levels run one after another. Four rings share what was five volumes (Josef, 2026-09-07). */
	Texture3D* voxelSupport = nullptr;
	/** @brief The dims voxelSupport was made with; a shape change remakes it. */
	DirectX::XMUINT3 voxelSupportDims = { 0, 0, 0 };
	ConstantBuffer* voxelCB = nullptr;
	winrt::com_ptr<ID3D11RasterizerState> voxelRasterState;
	ID3D11VertexShader* voxelVS = nullptr;
	ID3D11GeometryShader* voxelGS = nullptr;
	ID3D11PixelShader* voxelPS = nullptr;
	ID3D11ComputeShader* voxelScrollCS = nullptr;
	ID3D11ComputeShader* voxelSliceCS = nullptr;
	bool voxelShadersFailed = false;
	/** @brief Runtime-only: write and show the slice. */
	bool showVoxelSlice = false;
	/** @brief Runtime-only: the volume snow tinted by the ring that drew it. */
	bool showVolumeRings = false;
	/** @brief Debug, not serialised: rebuild every ring on its cadence whatever its inputs did - the idle gate's A/B. */
	bool debugForceVolumeRebuild = false;
	/** @brief Every level due this frame skipped its rebuild (the idle gate). */
	bool voxelIdleSkipped = false;
	/** @brief Why a level rebuilt, OR of the levels: 1 origin moved, 2 captures changed, 4 exclusions changed, 8 constants changed, 16 settling, 32 forced, 64 first build. */
	uint32_t voxelIdleBlockers = 0;
	float voxelSkipRate = -1.0f;
	uint32_t voxelSkipTallyFrames = 0;
	uint32_t voxelSkipTallySkipped = 0;
	/** @brief 0 top-down (XY), 1 side XZ, 2 side YZ. */
	int voxelSliceAxis = 1;
	/** @brief Slice plane offset from the camera, world units. */
	float voxelSliceOffset = 0.0f;
	/** @brief Seconds an unseen voxel takes to fade, at 60 fps. */
	float voxelMemorySeconds = 0.5f;
	/** @brief Runtime-only: the slice flattens the whole cube along the view axis (silhouettes) instead of showing one plane. The readable first look. */
	bool voxelSliceXray = true;
	/** @brief Occupied-voxel count: a raw UAV summed by VoxelScrollCS (post-decay, pre-raster, so it describes LAST frame's volume), read back one frame late through a staging pair so it never stalls. */
	winrt::com_ptr<ID3D11Buffer> voxelCountBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> voxelCountUAV;
	winrt::com_ptr<ID3D11Buffer> voxelCountStaging[2];
	uint voxelCountCursor = 0;
	uint32_t voxelOccupancy = 0;
	bool voxelOccupancyValid = false;
	/** @brief Creates the volumes, slice, CB, rasterizer and shaders on first use. Implemented in SnowDeformation/Volume.cpp. */
	bool EnsureVoxelResources();
	/** @brief Scrolls and decays the volume, then rasterises this frame's captured statics into it. Called inside RenderObjectHeightMap after the peel passes; a_records are the capture blocks it filled. Implemented in SnowDeformation/Volume.cpp. */
	void RenderVoxelVolume(const StaticsCB* a_records, uint32_t a_captureCount, bool a_recordsLive, uint32_t& a_parity);
	/** @brief Writes the selected plane of the current volume to voxelSliceTexture. Implemented in SnowDeformation/Volume.cpp. */
	void UpdateVoxelSliceTexture();

	// ---- Exclusion zones: bare-by-design clearings in the snow field ----

	/** @brief Doors get elliptical clears stretched along their facing (load doors; cave and building entrances; larger) that fade coverage to bare ground. Fires get noisy-edged MELT BASINS instead: the shell's depth thins toward a small floor that never vanishes and never sinks below terrain (negative values in the shelter-mask channel; see CombineCS). */
	/** @brief Must match MAX_EXCLUSIONS in HeightMapProcessCS.hlsl. When the gather overflows, the nearest sources win (distance sort in the gather). */
	static constexpr uint kMaxExclusions = 256;
	/** @brief Melt circle around a dropped burning torch. Carried torches never melt: a moving basin warps the bearer's own trails. */
	static constexpr float kTorchClearRadius = 40.0f;
	/** @brief Living actors this far above the LAND height stand on an elevated structure and do not stamp (2D-map interim gate; drift tops below this keep trails). */
	static constexpr float kElevatedStampCutoff = 70.0f;

	/** @brief Sentinel in the skin-depth raster's G channel (road top): no road drew in this column. Mirrored as kNoRoadTop in SnowHeightCapture.hlsl and SnowStaticsShell.hlsl. */
	static constexpr float kNoRoadTop = -1000000.0f;

	/** @brief Trench-patch grid quads per axis. Mirrors kPatchGridDim in SnowStaticsShell.hlsl, whose band table it is derived from: 2 x (128 + 16 + 8 + 4 + 57). */
	static constexpr uint32_t kPatchGridDim = 426;
	/** @brief World-unit snap for the patch centre. MUST be the coarsest band step in use (kPatchBandMul's last entry x kPatchStep = 32 x 8), or vertices stop landing on their band's lattice and quad widths flip as the camera moves - the invariant SnowGrid.hlsli warns about. */
	static constexpr float kPatchSnap = 256.0f;
	/** @brief The patch grid's outer reach in world units (the band table's last edge). The far road window is capped here: a wider window would hold roads the grid can never drape. */
	static constexpr float kFarPatchReach = 16384.0f;
	/** @brief Clamp band for heat-source melt radii derived from object bounds (braziers, sconces, forges). */
	static constexpr float kHeatClearRadiusMin = 40.0f;
	static constexpr float kHeatClearRadiusMax = 90.0f;
	/** @brief Melt bowl radius for ground-level fires â€” the heat-tier benchmark. Generous: full melt only in the inner ~35% ("the camper cleared the snow"), then a long noisy rise. */
	static constexpr float kFireClearRadius = 300.0f;
	/** @brief Raised generic flames (fxfire in brazier bowls, wall fires): small melt spot on the ground below. */
	static constexpr float kRaisedFlameClearRadius = 80.0f;
	/** @brief Grounded Survival-formlist heat the spec table does not name. Deliberately modest: the list also holds candelabras and the like, which must not crater like campfires. */
	static constexpr float kUnknownHeatClearRadius = 140.0f;

	/** @brief Heat-source classification: lowercase model-path substring -> melt bowl radius (kFireClearRadius 300 = campfire benchmark). First match wins, so specific names precede generic ones. Generic flame FX (fxfire) is handled separately BEFORE this table: fxfirewithembers would misfile as embers, and flames size by height, not name. */
	struct HeatSourceSpec
	{
		const char* substring;
		float radius;
	};
	static constexpr HeatSourceSpec kHeatSpecs[] = {
		{ "giantcampfire", 450.0f },  // giant fires dwarf man-made ones; must precede "campfire"
		{ "bonfire", 450.0f },
		{ "campfire", 300.0f },
		{ "firepit", 300.0f },
		{ "hearth", 300.0f },
		{ "smolder", 160.0f },  // dying fires still radiate
		{ "brazier", 150.0f },
		{ "cookingspit", 140.0f },
		{ "cookingpot", 140.0f },
		{ "sconce", 80.0f },
		{ "torch", 80.0f },  // wall torches; torchbug critters guarded at the call site
	};

	// ---- Workspace clearings: the shell never fully forms around worked spots ----

	/** @brief Workspace classification: lowercase model-path substring -> clearing radius + forward bias (units the bowl center rides along the object's facing, toward the working side; 0 = symmetric area). These become partial melt bowls (Settings::TrampleZoneHeight percent of depth remains), so actual actor trampling carves the visible tracks. Heat wins when both would match a model. First match wins: enchanting/alchemy precede the generic "workbench" their models also contain. */
	struct TrampleSpec
	{
		const char* substring;
		float radius;
		float forwardBias;
		/** @brief The station contains its own flame FX ref; generic-flame melt spots inside its footprint are suppressed so the workspace clearing alone governs it. */
		bool ownsFlames = false;
		/** @brief Fixed melt strength instead of the Workspace Clearing Height slider (0 = slider-controlled). Bedrolls clear fully: freshly laid down and slept in, whatever the workspace tuning. */
		float meltStrength = 0.0f;
		/** @brief Bowl elongation along the object's facing (1 = circle; 1.75 stretches a bedroll's clearing to its shape), centered on the mesh. */
		float aspect = 1.0f;
		/** @brief Smooth bowl edge (no noise): bedding melts flush like shelters, so a bedroll inside a tent joins the tent's sink cleanly instead of scribbling a noisy rim across it. */
		bool smoothEdge = false;
	};
	static constexpr TrampleSpec kTrampleSpecs[] = {
		{ "smelter", 300.0f, 0.0f, true },  // smelters and forges count as workspaces: both sliders apply, and their own flames must not add melt spots
		{ "forge", 260.0f, 0.0f, true },
		{ "sawmill", 220.0f, 0.0f },
		{ "millsaw", 220.0f, 0.0f },
		{ "stables", 200.0f, 0.0f },    // NOT "stable": clutter\ruins\ruinstable01 is a table
		{ "chopping", 150.0f, 50.0f },  // wood chopping blocks
		{ "enchanting", 110.0f, 60.0f },
		{ "alchemy", 110.0f, 60.0f },
		{ "workbench", 140.0f, 0.0f },         // symmetric: workbench facing disagrees with itself across refs, and a centred bowl cannot be wrong-sided
		{ "sharpeningwheel", 130.0f, 60.0f },  // grindstones: furniture\clutter\blacksmithsharpeningwheelanimating.nif - no "grind" anywhere in the path
		{ "tanningrack", 120.0f, 60.0f },
		{ "anvil", 120.0f, 50.0f },
		{ "marketstall", 110.0f, 0.0f },
		{ "well01", 110.0f, 0.0f },  // bare "well" would substring-match too much
		{ "shrine", 90.0f, 0.0f },
		{ "bedroll", 160.0f, 0.0f, false, 1.0f, 1.75f, true },  // furniture\bedroll\*.nif; full clear - nobody sleeps in a buried bedroll
		{ "haymound", 120.0f, 0.0f, false, 1.0f, 1.0f, true },  // clutter\hay\haymound*.nif; bedding stays clear like bedrolls
		{ "haybale", 100.0f, 0.0f, false, 1.0f, 1.0f, true },   // hayscatter* deliberately absent: flat ground decals, a clearing reads wrong
	};

	/** @brief Workspace clearings in the last gather, for the debug readout. */
	uint32_t statTrampleCount = 0;

	// ---- Sealed containers: boxes that have been shut for centuries ----

	/** @brief Lowercase model-path substrings whose references get a SEALED-CONTAINER exclusion (shader type 3): an oriented rectangle, taken from the reference's own bounds, that kills shell coverage inside its footprint. Draugr sarcophagi are the case that motivated it - one opens, the draugr steps out, and the landscape shell is standing inside a coffin that was shut for centuries. This is the ONLY exclusion that suppresses rather than melts: a melt bowl thins depth toward a floor, and a floor is still a sheet of snow lying in an open box. Matched against the same lowered model path the heat and workspace tables use, so it costs nothing extra per reference. */
	static constexpr const char* kSealedContainerSubstrings[] = {
		"sarcophagus",  // dungeons\nordic\ruins\ruinssarcophagus__bottom01.nif and the lid/whole variants
	};
	/** @brief Sealed containers in the last gather, for the debug readout. */
	uint32_t statSealedCount = 0;

	/** @brief Heat within this height of the land counts as a ground fire (full basin); higher sources melt only their footprint spot. */
	static constexpr float kGroundFireBand = 40.0f;

	// ---- Wide exclusion field: clearings at any range ----

	/** @brief The near mask (ObjectBottoms) stops at the object height window, ~57 m, because it also carries the SHELTER term, which needs a top-down render of real geometry. Clearings are analytic - position, radius, type - so they are baked separately over a window that reaches the shell's own extent. */
	static constexpr uint kExclusionFieldDim = 1024;
	/** @brief Half extent in world units. Sized to cover uGridsToLoad=7 plus the shell's seam overlap; references only exist inside loaded cells, so a wider window would find nothing to bake. 32 units/texel at kExclusionFieldDim, against a 300-unit campfire bowl. */
	static constexpr float kExclusionFieldHalfExtent = 16384.0f;

	Texture2D* exclusionFieldTexture = nullptr;
	/** @brief Texel-snapped window centre, so the baked edges do not crawl as the camera moves. */
	float2 exclusionFieldCenter = { 0.0f, 0.0f };
	/** @brief Set once the field has been baked at least once this session; until then the shells fall back to the near mask alone. */
	bool exclusionFieldValid = false;

	/** @brief Layout must match ExclusionFieldCB in ExclusionFieldCS.hlsl. */
	struct alignas(16) ExclusionFieldCB
	{
		float2 FieldCenter;
		float padHalfExtent;
		float FieldTexelSize;

		float2 TerrainWindowOrigin;
		float TerrainTexelSize;
		uint TerrainDim;
	};
	STATIC_ASSERT_ALIGNAS_16(ExclusionFieldCB);
	ConstantBuffer* exclusionFieldCB = nullptr;

	/** @brief Returns the exclusion field bake compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetExclusionFieldCS();
	ID3D11ComputeShader* exclusionFieldCS = nullptr;
	/** @brief SRV of the wide exclusion field; null before SetupResources. */
	ID3D11ShaderResourceView* GetExclusionFieldSRV() const { return exclusionFieldTexture ? exclusionFieldTexture->srv.get() : nullptr; }

	/** @brief Layout must match ExclusionCB in SnowExclusions.hlsli. */
	struct alignas(16) ExclusionsCB
	{
		float4 PosRadius[kMaxExclusions];   ///< xyz = position, w = radius
		float4 DirExtType[kMaxExclusions];  ///< doors (w=0): xy = facing, z = forward extent. Fires (w=1 noisy, w=2 smooth-edged): xy = elongation axis x (aspect-1) (zero = circle), z = melt strength
		uint ExclusionCount;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(ExclusionsCB);
	static_assert(offsetof(ExclusionsCB, ExclusionCount) == sizeof(float4) * kMaxExclusions * 2, "exclusionListBuffer mirrors the two leading arrays");
	ConstantBuffer* doorsCB = nullptr;
	/** @brief ExclusionsCB's PosRadius and DirExtType arrays as a structured buffer (t5): CombineCS's per-tile cull reads one entry per thread, which a cbuffer serializes. */
	StructuredBuffer* exclusionListBuffer = nullptr;
	/** @brief The list as last uploaded, hashed: the volume's idle gate reads it. */
	uint64_t exclusionListHash = 0;
	bool doorsCBUploaded = false;
	/** @brief Inputs of the last exclusion-field bake: terrain window cell, terrain texture version, the list as uploaded. The bake skips while all match and the snapped centre is unchanged - the texture already holds the same bits. */
	int exclusionBakeCellX = INT_MIN;
	int exclusionBakeCellY = INT_MIN;
	uint32_t exclusionBakeTerrainVersion = 0;
	// Heap-held: an alignas(16) member would pad the class (C4324 is an error here).
	std::unique_ptr<ExclusionsCB> exclusionBakeList;
	/** @brief Bakes the wide exclusion field from the gathered exclusion list, or skips when nothing it reads has changed. Implemented in SnowDeformation/Statics.cpp. */
	void RenderExclusionField(const ExclusionsCB& a_list);
	uint32_t doorRefreshCounter = 0;
	/** @brief Cadence-gathered exclusions (doors, campfires, heat sources, dropped burning torches). */
	std::vector<std::pair<float4, float4>> staticExclusions;
	/** @brief The heat among them as position + melt-bowl radius, kept apart from the GPU list: flames a station owns stay here though its clearing replaces their bowl, and the overflow cut does not apply. Read by the coat aging inputs. */
	std::vector<float4> heatSources;
	std::vector<float4> exclusionPendingHeat;
	/** @brief The walk in progress: its accumulators, the loaded-cell slot it resumes at (0 = idle), and the origin it was started from. Swapped into staticExclusions whole when the last slot is done. */
	std::vector<std::pair<float4, float4>> exclusionPending;
	std::vector<size_t> exclusionPendingFlames;
	std::vector<float4> exclusionPendingGuards;
	uint32_t exclusionPendingTrample = 0;
	uint32_t exclusionPendingSealed = 0;
	uint32_t exclusionGatherSlot = 0;
	uint32_t exclusionGatherSlots = 0;
	bool exclusionGatherInterior = false;
	RE::NiPoint3 exclusionGatherOrigin;
	void StartExclusionGather();
	void StepExclusionGather();
	RE::BSContainer::ForEachResult ExclusionVisit(RE::TESObjectREFR* a_ref);
	void FinishExclusionGather();
	/** @brief Survival Mode's Survival_WarmUpObjectsList when the plugin is present: base objects that count as heat sources. */
	RE::BGSListForm* survivalHeatSources = nullptr;
	bool survivalHeatSourcesResolved = false;
	/** @brief Exclusions uploaded this frame (static + carried torches), for the debug readout. */
	uint32_t statExclusionCount = 0;

	/** @brief Creates the height-window textures. Implemented in SnowDeformation/Statics.cpp. */
	void CreateHeightFieldResources();
	/** @brief Scrolls the accumulated height maps to the new window position and rasterizes this frame's captured statics into them (MAX/MIN). Called from DrawShell before the screen-space passes. Implemented in SnowDeformation/Statics.cpp. */
	void RenderObjectHeightMap();
	/** @brief Bare square, nothing captured, no volume snow: no reader of the object fields runs, so the block is skipped and its rows read 0; the next run rebuilds from a clear. */
	void SkipObjectHeightMap();
	/** @brief Interior with nothing reading the object fields and no ghost in the maps: the window and the exclusion walk advance as a run's would; no GPU work. */
	void IdleObjectHeightMap();
	/** @brief The maps hold sentinels only, are valid, and were built at the current reach. */
	bool HeightMapIdleReady() const;
	/** @brief World XY box (min x, min y, max x, max y) of every layer-1 draw since the maps last held sentinels only. Ghosts die only by scrolling out, so none survive once the box misses the window. */
	float4 heightGhostBox = {};
	bool heightGhostsLive = true;
	uint64_t heightIdleFrames = 0;
	/** @brief DevBench A/B: true keeps the interior's height maps running for the cascades, as before the interior gate. */
	bool interiorHeightGateOff = false;
	/** @brief DevBench A/B: true keeps a bare exterior's height maps running for the cascades, as before the exterior gate. */
	bool exteriorHeightGateOff = false;
	/** @brief Last CascadeSnowSoon answer DrawShell took (DevBench readout). */
	bool cascadeSnowSoonLast = true;
	/** @brief DevBench A/B: true gives the height maps the whole loaded square's verdict again, with no cascade footprint test. */
	bool cascadeHeightGateOff = false;
	/** @brief DevBench A/B: true lets any capture, LOD batches with no skin in the raster's window included, keep the height maps running. */
	bool lodRasterGateOff = false;
	/** @brief The last DrawShell's captures were LOD batches with no skin in the raster's window, so they did not keep the height maps (DevBench readout). */
	bool lodCapturesIgnoredLast = false;
	/** @brief DevBench A/B: true raises every sun cascade while the shell footprint holds snow, as before the per-cascade test. */
	bool cascadeRaiseGateOff = false;
	/** @brief The cascade camera the last raise verdict was taken for (per-frame buffer), and the verdict. */
	Matrix cascadeGateVP{};
	float4 cascadeGateCam{};
	uint32_t cascadeGateFrame = UINT32_MAX;
	bool cascadeGateMeets = true;
	/** @brief Sun cascade cameras raised and left unraised, this frame and the last. */
	uint32_t cascadeRaised = 0;
	uint32_t cascadeSkipped = 0;
	uint32_t cascadeRaisedLast = 0;
	uint32_t cascadeSkippedLast = 0;
	uint32_t cascadeCountFrame = UINT32_MAX;
	/** @brief A window move past this many texels restarts the exclusion walk. */
	static constexpr int kGatherJumpTexels = 32;

	ID3D11VertexShader* staticsVS = nullptr;
	ID3D11PixelShader* staticsPS = nullptr;
	/** @brief Statics PS with the SV_Depth export compiled out, chosen per draw for captures that cannot carve. Only the parallax carve pushes depth, and only road draws carve. */
	ID3D11PixelShader* staticsPSNoDepth = nullptr;
	/** @brief Depth-prepass twin of the no-export shader (SNOW_STATICS_DEPTH_PREPASS): the alpha cut with no colour and no export. Non-carving skins draw once with it into a private copy of the scene depth, then once with the shipping shader under EQUAL against it, so hardware early-Z admits exactly the pixels the prepass wrote; carving draws keep their own export and LESS_EQUAL in the shading loop, since a shader-computed depth cannot be matched against a second compile of itself; the private depth is then written back. Bit-identical by construction: both passes run the same raster path. */
	ID3D11PixelShader* staticsPSPrepassNoDepth = nullptr;
	/** @brief SNOW_STATICS_DRAPE twins of the no-export shading and prepass shaders: the values FillSkinDrawCB gives every drape (S4) draw, and the debug views off, folded in. Chosen per draw for drapes; a compile failure keeps the general pair. */
	ID3D11PixelShader* staticsPSDrapeNoDepth = nullptr;
	ID3D11PixelShader* staticsPSDrapePrepass = nullptr;
	/** @brief The drapes' print twin (SNOW_OBJECT_PRINTS): the no-export drape PS plus the rim around a bare print. Chosen per draw where the print map holds prints under it; a compile failure keeps those drapes on staticsPSDrapeNoDepth. */
	ID3D11PixelShader* staticsPSDrapePrintsNoDepth = nullptr;
	/** @brief Snow Follows Parallax (SNOW_COAT_PARALLAX): the skin VS that also carries the object's uv and tangent frame, and the drape and print-twin shading PS that march the object's parallax and read the coat's material where it lands. Both loops of a drape with a height map take this VS; a compile failure keeps it on the plain pair. */
	ID3D11VertexShader* staticsVSParallax = nullptr;
	winrt::com_ptr<ID3DBlob> staticsVSParallaxBlob;
	ID3D11PixelShader* staticsPSDrapeParallax = nullptr;
	ID3D11PixelShader* staticsPSDrapePrintsParallax = nullptr;
	bool coatParallaxTried = false;
	/** @brief Compiles the three on first use (the setting on); true when the VS and the drape PS exist. */
	bool EnsureCoatParallaxShaders();
	/** @brief Whole-skin occlusion cull (DepthSyncCS.hlsl HiZBuildCS / SkinCullCS): a max-depth pyramid of the scene depth, one compute thread per skin testing its bounding sphere (worldBound + lift margin) against it, and per-skin DrawIndexedInstancedIndirect arguments with instance count 0 or 1. Skins outside the view or wholly behind the scene never enter the pipeline; both prepass and shading loops read the same arguments. Conservative, so bit-identical. Implemented in SnowDeformation/Statics.cpp. */
	struct alignas(16) SkinCullBound
	{
		float Center[3];
		float Radius;
		uint32_t IndexCount;
		uint32_t BoundsSlot;
		uint32_t HasBounds;
		float LiftMargin;
		uint32_t ClusterOffset;
		uint32_t ClusterCount;
		uint32_t IndexPoolOffset;
		uint32_t ScratchBase;
		float4 WorldRow0;
		float4 WorldRow1;
		float4 WorldRow2;
		/** @brief Horizontal camera distance past which the skin dissolves whole (SkinFadeEnd); 0 = never (fade-exempt, debug views). */
		float FadeEnd;
		float pad[3];
	};
	struct alignas(16) SkinCullCB
	{
		Matrix ViewProj;
		float4 CameraPosAdjust;
		float4 Viewport;
		float4 Depth;
		uint32_t SkinCount;
		float Level;
		float pad[2];
	};
	static constexpr uint32_t kSkinCullArgStride = 20;
	/** @brief SkinCullCS reason codes 0-8 (DepthSyncCS.hlsl). */
	static constexpr uint32_t kSkinCullReasons = 9;
	static constexpr int kSkinCullRing = 3;
	/** @brief World-unit growth of the bound beyond the class depth: cornice roll, relief, the toward-eye flank push. */
	static constexpr float kSkinCullMargin = 24.0f;
	/** @brief Depth-space margin: the skin rasterizer bias is clamped at 1e-5 (DepthBiasClamp), plus projection rounding. */
	static constexpr float kSkinCullDepthEps = 1.5e-5f;
	ConstantBuffer* skinCullCB = nullptr;
	Buffer* skinCullBounds = nullptr;
	Buffer* skinCullArgs = nullptr;
	/** @brief The drawn skins' NDC footprint (DepthSyncCS SkinFootprint, u7 in the culls, t62 in FillRectVS): four uints, cleared each frame. */
	Buffer* skinFootprint = nullptr;
	uint32_t skinCullCapacity = 0;
	winrt::com_ptr<ID3D11Buffer> skinCullArgsStaging[kSkinCullRing];
	bool skinCullStagingIssued[kSkinCullRing] = {};
	uint32_t skinCullStagingCount[kSkinCullRing] = {};
	/** @brief One-shot: name every skin submitted this frame, and log the culled and decal-state ones with their verdict when the census reads back. */
	bool skinCullLogArmed = false;
	/** @brief Debug: LogCaptureVerdict logs every geometry's capture verdict (once per geometry and outcome). */
	bool debugLogAllJourneys = false;
	std::vector<std::string> skinCullLogNames[kSkinCullRing];
	int skinCullRing = 0;
	Texture2D* skinCullHiZ = nullptr;
	/** @brief One single-mip scratch per level, for when the UAV build fails to compile: the build reads level k-1 of the chain and writes scratch k, then copies it into the chain. A texture cannot be an input and an output of the same dispatch, mips included. */
	std::vector<Texture2D*> skinCullHiZScratch;
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> skinCullHiZSRVs;
	/** @brief One UAV per mip of the chain: each level is written in place and read by the next through it. */
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> skinCullHiZUAVs;
	uint32_t skinCullLevels = 0;
	uint32_t skinCullDrawnLast = 0;
	uint32_t skinCullCulledLast = 0;
	uint32_t skinCullTrisDrawnLast = 0;
	uint32_t skinCullTrisTotalLast = 0;
	/** @brief Per-reason census (SkinCullCS reason codes) and the pyramid's 1x1 top level, read back through 1x1 staging textures: 0 there means the pyramid is dead. */
	uint32_t skinCullReasonLast[kSkinCullReasons] = {};
	float skinCullHiZTopLast = -1.0f;
	winrt::com_ptr<ID3D11Texture2D> skinCullHiZTopStaging[kSkinCullRing];
	ID3D11ComputeShader* GetHiZBuildCS(bool a_fromUAV = false);
	ID3D11ComputeShader* hiZBuildUAVCS = nullptr;
	ID3D11ComputeShader* GetSkinCullCS();
	ID3D11ComputeShader* hiZBuildCS = nullptr;
	ID3D11ComputeShader* skinCullCS = nullptr;
	/** @brief a_extentW/H: the pixels the pyramid covers from the depth's origin (the rendered rectangle's far corner). */
	bool EnsureSkinCullResources(uint32_t a_count, ID3D11ShaderResourceView* a_mainDepthSRV, uint32_t a_extentW, uint32_t a_extentH);
	/** @brief Tessellated skin stages (optional; legacy path is the fallback): control-point VS, hull (edge-length/distance factors) and domain (displacement-map relief along the inflate normal). */
	ID3D11VertexShader* staticsTessVS = nullptr;
	ID3D11HullShader* staticsHS = nullptr;
	ID3D11DomainShader* staticsDS = nullptr;
	/** @brief Trench patch (PATCH define): the landscape shell's dense-grid carve applied to OBJECT tops; real geometry where parallax cannot notch silhouettes or hold floors angle-stably. */
	ID3D11VertexShader* patchVS = nullptr;
	/** @brief Depth-only caster variant of the patch VS (PATCH + SNOW_SHADOW_CAST + SNOW_PATCH_INDEXED): the patch's quads from an index buffer over the (kPatchGridDim + 1)^2 lattice, one VS invocation per shared vertex; drawn into the sun cascade slices by InjectShellShadowCasters. */
	ID3D11VertexShader* patchShadowVS = nullptr;
	winrt::com_ptr<ID3D11Buffer> patchCasterIB;
	uint32_t patchCasterIndexCount = 0;
	bool EnsurePatchCasterIB();
	/** @brief The tessellated patch hull's frustum footprint lines (SnowStaticsShell.hlsl PatchCullCB, b11). */
	struct alignas(16) PatchCullCB
	{
		float4 Lines[6];
		uint32_t LineCount;
		/** @brief The tessellated draw's band: first lattice row (its VS adds SV_InstanceID) and first column (SV_VertexID / 4 from 0: the start vertex does not reach it). */
		uint32_t RowStart;
		uint32_t ColStart;
		uint32_t pad;
	};
	ConstantBuffer* patchCullCB = nullptr;
	/** @brief The view frustum's five planes (rows of CameraViewProj, camera-relative) with z eliminated: lines in world XY, inside where dot(xy, p) + z >= 0. */
	uint32_t FillPatchCullLines(const ShellCB& a_cb, PatchCullCB& a_out) const;
	/** @brief The trench patch lattice's centre, snapped to its coarsest band step (StaticsCB::WorldRow0.xy). */
	float2 PatchLatticeCentre() const;
	/** @brief Lattice rows [y0, y1) drawn over columns [x0, x1). */
	struct PatchBand
	{
		uint32_t x0, x1, y0, y1;
	};
	static constexpr uint32_t kPatchBands = 4;
	/** @brief The lattice's 8x8 blocks the hull's footprint lines cannot cull, as up to kPatchBands row bands in row order, each over its blocks' column span; returns the band count. */
	uint32_t PatchLatticeBands(const PatchCullCB& a_lines, float2 a_centre, PatchBand (&a_bands)[kPatchBands]) const;
	/** @brief A/B, runtime-only: the tessellated patch draws the whole lattice. */
	bool patchBoxOff = false;
	/** @brief Debug, runtime-only: the whole lattice (the one Draw the bands replace) also draws into copies of the patch's deferred targets and depth taken before the bands, compared bit for bit with the bands' result (attributes 0-2, mark, depth). */
	bool patchVerify = false;
	Texture2D* patchVerifyRT[4] = {};
	winrt::com_ptr<ID3D11Texture2D> patchVerifyDepth;
	winrt::com_ptr<ID3D11DepthStencilView> patchVerifyDSV;
	winrt::com_ptr<ID3D11ShaderResourceView> patchVerifyDepthSRV;
	/** @brief The SRV over the real depth the patch draws against, and the texture it views. */
	winrt::com_ptr<ID3D11ShaderResourceView> patchVerifyRealDepthSRV;
	ID3D11Texture2D* patchVerifyRealDepth = nullptr;
	winrt::com_ptr<ID3D11Buffer> patchVerifyCount;
	winrt::com_ptr<ID3D11UnorderedAccessView> patchVerifyAllUAV;
	winrt::com_ptr<ID3D11UnorderedAccessView> patchVerifySlotUAV[5];
	winrt::com_ptr<ID3D11Buffer> patchVerifyStaging[3];
	bool patchVerifyStagingBusy[3] = {};
	uint32_t patchVerifyRing = 0;
	uint32_t patchVerifyFrames = 0;
	uint64_t patchVerifyMismatch[5] = {};
	bool EnsurePatchVerify(ID3D11DepthStencilView* a_dsv);
	/** @brief Patches the last tessellated patch draw issued (of kPatchGridDim^2). */
	uint32_t patchBoxPatches = 0;
	/** @brief Tessellated patch stages (optional; legacy path is the fallback). */
	ID3D11VertexShader* patchTessVS = nullptr;
	ID3D11HullShader* patchHS = nullptr;
	ID3D11DomainShader* patchDS = nullptr;
	/** @brief The forward patch PS: drawn under the statics debug views and until both deferred shaders exist. Otherwise the patch draws deferred: its geometry once with the coverage cut alone (SNOW_PATCH_ATTR, into the ground's deferred targets), then one full-screen pass shades the pixels it kept (SNOW_PATCH_DEFERRED). */
	ID3D11PixelShader* patchPS = nullptr;
	/** @brief [attribute pass, shading pass]: primed with the object snow group, else compiled off the render thread on first use. */
	ID3D11PixelShader* patchDeferredPS[2] = {};
	std::future<ID3D11PixelShader*> patchDeferredJob[2];
	bool patchDeferredTried[2] = {};
	winrt::com_ptr<ID3D11DepthStencilState> patchDeferredDepthState;
	/** @brief R8, screen-sized: 1 where the attribute pass kept a pixel, cleared per frame; the shading pass tests it before reading the attributes. */
	Texture2D* patchDeferredMark = nullptr;
	/** @brief The mark target and the shading pass's depth-off state; false leaves the patch forward. */
	bool EnsurePatchDeferredResources();
	/** @brief The deferred patch shading PS with SNOW_CONDITION_VIEW, compiled off the render thread the first time the view is switched on. */
	ID3D11PixelShader* patchConditionViewPS = nullptr;
	std::future<ID3D11PixelShader*> patchConditionViewJob;
	bool patchConditionViewTried = false;
	/** @brief The condition view's patch shading PS while Debugging Options > Snow Condition is on and it has compiled; null otherwise. */
	ID3D11PixelShader* PatchConditionViewPS();
	/** @brief One deferred patch PS (0 attribute pass, 1 shading pass, 2 shading pass with the condition view), compiled now; null on failure. */
	static ID3D11PixelShader* CompilePatchDeferredPS(int a_which);
	/** @brief Shader-prime step (object snow group): compiles both deferred patch shaders on the prime worker. */
	void PrimePatchDeferred();
	/** @brief Both deferred patch shaders, once compiled (the first call starts any compile the prime did not run). */
	bool PatchDeferredReady();
	/** @brief A deferred patch shader is compiling. */
	bool PatchDeferredCompiling() const;
	void ReleasePatchDeferred();
	/** @brief Retained VS bytecode: input layouts are created against it, one per vertex descriptor. */
	winrt::com_ptr<ID3DBlob> staticsVSBlob;
	bool staticsShadersFailed = false;
	ConstantBuffer* staticsCB = nullptr;
	/** @brief Per-frame records of every static's StaticsCB at a 256-byte stride, bound per draw by offset (D3D11.1 *SetConstantBuffers1) instead of a Map/Unmap per draw (PERF-RESEARCH §11.1b). Two copies alternate per draw: the runtime's redundancy filter ignores the offsets on a rebound buffer. Null when the runtime or driver lacks constant-buffer offsetting; staticsCB->Update per draw is the fallback. */
	winrt::com_ptr<ID3D11DeviceContext1> staticsContext1;
	winrt::com_ptr<ID3D11Buffer> staticsRecordCB[2];
	bool staticsRecordChecked = false;
	static constexpr uint32_t kStaticsRecordStride = 256;  // 16 constants, the offset granularity
	static constexpr uint32_t kStaticsRecordStart = 8192;
	static constexpr uint32_t kStaticsRecordMax = 65536;
	/** @brief Records per copy: doubles from kStaticsRecordStart to the largest upload seen, up to kStaticsRecordMax. */
	uint32_t staticsRecordCapacity = 0;
	/** @brief The smallest capacity the device refused; never asked again. */
	uint32_t staticsRecordRefused = 0;
	/** @brief Uploads that fell back to per-draw updates for want of room. */
	uint64_t staticsRecordFallbacks = 0;
	/** @brief DevBench A/B: true holds the buffers at kStaticsRecordStart, as before they grew. */
	bool staticsRecordGrowOff = false;
	bool EnsureStaticsRecordCB(uint32_t a_count);
	/** @brief a_needed (optional, one byte per record): records left out are not copied; it must cover every record a draw binds. */
	bool UploadStaticsRecords(const StaticsCB* a_records, uint32_t a_count, const uint8_t* a_needed = nullptr);
	void BindStaticsRecord(uint32_t a_index, bool a_pixelStage, bool a_tessStages, uint32_t& a_parity);
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> staticsILCache;
	/** @brief Input layout for a vertex descriptor, created on first sight against the statics VS (POSITION+NORMAL; any VS reading a subset binds to it). Null is cached for descriptors that cannot be laid out. */
	ID3D11InputLayout* StaticsInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc);
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> staticsILParallaxCache;
	/** @brief The same against staticsVSParallax, adding TEXCOORD0 and BINORMAL; null (cached) for descriptors without uv or tangents. */
	ID3D11InputLayout* StaticsParallaxInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc);

	/** @brief Prop contact field (MESH-CONTACT-PLAN Route B, rigid half): moving props' render meshes rasterized from above, MIN-blended world Z per column, over a fine window around the deformation centre. R32G32F: R = bodies (Snow Resistance applies), G = loose props. Per frame, never accumulated; the stamp pass carves from it per texel. */
	Texture2D* contactHeight = nullptr;
	/** @brief MIN blends masked to one channel of the contact field: bodies write R, loose props G. */
	winrt::com_ptr<ID3D11BlendState> contactMinBlendBodies;
	winrt::com_ptr<ID3D11BlendState> contactMinBlendProps;
	winrt::com_ptr<ID3D11RasterizerState> contactRasterState;
	ID3D11VertexShader* contactVS = nullptr;
	ID3D11PixelShader* contactPS = nullptr;
	bool contactShadersFailed = false;
	/**
	 * @brief One body queued for contact rasterization: a HANDLE, never a NiPointer.
	 *
	 * Holding NiPointer to a live 3D root across a frame makes this module a
	 * co-owner of the scene graph, and the last release runs the destructor -
	 * so an actor whose 3D the game swapped or unloaded that frame would be
	 * destroyed HERE, on the render thread, while animation jobs may still be
	 * walking it. The handle re-resolves at draw time instead: current 3D or
	 * nothing, and this module never owns a node.
	 */
	struct ContactProp
	{
		RE::ObjectRefHandle ref;
		float minX, minY, maxX, maxY;
		bool corpse = false;
		/** @brief Actors only: the ground under the feet and the layer depth there, so the draw can refuse parts that cannot reach the snow. */
		float groundZ = 0.0f;
		float layer = 0.0f;
	};
	/** @brief This frame's rasterized props, gathered by the prop scan; their collision shapes stay out of the stamp list. */
	std::vector<ContactProp> contactProps;
	/** @brief Skin instances whose partition layout has been logged once (capped), so the log states whether partitions share a buffer rather than the code assuming it. */
	std::unordered_set<const void*> contactSkinLogged;
	float2 contactCenter = { 0, 0 };
	uint contactDrawsLast = 0;
	/** @brief Bone palette for one skin partition, plus the capture window. Absolute world rows, so the VS needs no object transform and no pivot. */
	struct alignas(16) ContactSkinCB
	{
		float4 BoneRows[240];
		float2 SkinWindowCenter;
		float SkinHalfExtent;
		/** @brief Bones in this partition's palette, so the shader can refuse an index past it. */
		float SkinBoneCount;
	};
	STATIC_ASSERT_ALIGNAS_16(ContactSkinCB);
	ConstantBuffer* contactSkinCB = nullptr;
	ID3D11VertexShader* contactSkinVS = nullptr;
	/** @brief Input layouts for SKINNED vertex descriptors (POSITION + BLENDWEIGHT + BLENDINDICES); separate cache because the formats differ from the rigid layout. */
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> contactSkinILCache;
	winrt::com_ptr<ID3DBlob> contactSkinVSBlob;
	/** @brief a_dynamicPositionBytes 0 = positions in the game's vertex buffer; 16 or 8 = a second stream of float4/half4 positions copied from a BSDynamicTriShape's dynamicData (RaceMenu BodyMorph, OBody, facegen). */
	ID3D11InputLayout* ContactSkinInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc, uint32_t a_dynamicPositionBytes);
	/** @brief Positions of the dynamic tri shape being drawn, re-uploaded per geometry (WRITE_DISCARD). */
	winrt::com_ptr<ID3D11Buffer> contactDynamicVB;
	uint32_t contactDynamicVBBytes = 0;
	/** @brief Dynamic tri shapes whose measured layout has been logged once (capped). */
	std::unordered_set<const void*> contactDynamicLogged;
	/** @brief This frame's actors drawn by their skinned meshes; their bone stamps are skipped. */
	std::vector<ContactProp> contactActors;
	/** @brief This frame's corpse entries in contactActors, against their cap. */
	uint contactCorpseCount = 0;
	/** @brief FormIDs of the living actors that hold this frame's raster slots: the player and the nearest inside the contact window, up to kContactMaxActors. Anyone else inside the window prints by bones. Before this the slots went in process-list order, so a near NPC could print by bones while a far one rasterized. */
	std::vector<uint32_t> contactPriority;
	/** @brief Per-geometry scratch for the skinned contact palette: one composed transform per skin bone, built on first use and shared by every partition of the geometry. */
	std::vector<RE::NiTransform> contactPaletteScratch;
	std::vector<uint8_t> contactPaletteBuilt;
	/** @brief Runtime-only: the contact field as the carve pass reads it (ContactViewCS into an RGBA8 the menu shows with the player's bound overlaid). S1's debug view: the silhouette's shape, extent and placement in one image. */
	bool debugContactView = false;
	/** @brief Runtime-only: half-extent of the field view's crop around the player, world units (192 = a body, 1536 = the whole field). */
	float debugContactViewHalf = 192.0f;
	/** @brief Runtime-only: index of the one skinned geometry the actor contact pass draws (-1 = all), and its name. */
	int debugContactSolo = -1;
	std::string contactSoloName;
	/** @brief Field view texels per side. The map view sits under the field, so the texture is kContactViewPx x 2*kContactViewPx. Mirrors VIEW_PX in DeformationUpdateCS.hlsl. */
	static constexpr uint kContactViewPx = 512;
	winrt::com_ptr<ID3D11Texture2D> contactViewTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> contactViewSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> contactViewUAV;
	void EnsureContactViewTexture();
	uint contactSkinDrawsLast = 0;
	/** @brief Bone slots the actor contact pass had to stand in for this frame (skeleton lacks the bone), and the skins already reported. */
	uint contactSkinMissingLast = 0;
	/** @brief Rigid meshes carried by rasterized actors (weapons, shields) drawn into the field this frame. */
	uint contactCarriedLast = 0;
	/** @brief RaceMenu overlay clones the actor contact pass declined this frame. */
	uint contactOverlaysLast = 0;
	/** @brief Creature fur shells the actor contact pass declined this frame. */
	uint contactShellsLast = 0;
	/** @brief Skin partitions the actor contact pass declined this frame: dismember partitions the game hides, or partitions whose bones resolve to nothing. */
	uint contactHiddenPartsLast = 0;
	/** @brief Per skin instance, how its vertices index bones: 1 = each partition's own list, 2 = the skin's full list. Audited once from the raw vertex copy. Identity key only, never dereferenced. */
	struct ContactSkinAudit
	{
		/** @brief Identity witness: the skin's partition object. The map is keyed by a raw skin address, which a reload recycles. */
		const void* partition = nullptr;
		uint8_t indexing = 0;
	};
	std::unordered_map<const void*, ContactSkinAudit> contactSkinIndexing;
	/** @brief Skinned partitions drawn this frame from a whole-skin palette because their vertices index the skin's full bone list. */
	uint contactGlobalPartsLast = 0;
	/** @brief Extra sub-step draws issued this frame so fast gear sweeps instead of printing at intervals. */
	uint contactSweepLast = 0;
	/** @brief Previous frame's world transform per carried mesh, keyed by geometry, for the sweep. Identity only - never dereferenced. */
	struct ContactSweep
	{
		RE::NiTransform world;
		uint32_t frame = 0;
		/** @brief Identity witness: the geometry's vertex buffer. The map is keyed by a raw BSGeometry address, which the game recycles - a freed mesh's slot handed to a new one carries a previous pose belonging to something else, and the sweep then draws the new mesh at every step between the two. Mismatch means a new tenant, not a move. */
		const void* buffer = nullptr;
	};
	std::unordered_map<const void*, ContactSweep> contactSweepStates;
	uint32_t contactSweepFrame = 0;
	/** @brief World units a sweeping mesh may advance between sub-steps (two contact texels), and the ceiling on sub-steps for one mesh. */
	static constexpr float kContactSweepStep = 6.0f;
	static constexpr uint32_t kContactMaxSweep = 8;
	std::unordered_set<const void*> contactSkinMissingLogged;
	/** @brief Skinned geometries the contact pass refused because the game itself hides them, reported once each. */
	std::unordered_set<const void*> contactSkinHiddenLogged;
	/** @brief Actors refused as incorporeal, logged once each with the reading that decided it. */
	std::unordered_set<uint32_t> incorporealLogged;
	/** @brief Actors that keep their trench despite a see-through piece, logged once each. */
	std::unordered_set<uint32_t> seeThroughKeptLogged;
	/** @brief Margin over the layer depth before a part is refused as unreachable: the terrain window's bilinear ground can sit this far off the actor's feet on a slope. */
	static constexpr float kContactSkipMargin = 16.0f;
	/** @brief Frames a living actor whose contact draw printed nothing stamps by bones before the mesh route is retried. */
	static constexpr uint16_t kContactStarveFrames = 60;
	/** @brief Starvation lines written to the log this session, capped. */
	uint contactStarveLines = 0;
	/** @brief Debug view crop centre and the map texel size the last update used, for the menu's overlay. */
	float2 contactViewCenter{};
	float contactViewTexelSize = 0.0f;
	/**
	 * @brief The crop half-extent and the player bound the view CS was actually dispatched with.
	 *
	 * The overlay must be drawn from the SAME numbers the pixels under it were
	 * built from. Reading the live slider or a fresh worldBound in the menu
	 * instead puts the box somewhere the image never claimed - the menu runs
	 * after the pass, so a slider dragged this frame, or a bound that moved
	 * between the two reads, shifts the box off a silhouette that is exactly
	 * where it should be. A wrong box then reads as a wrong rasterizer.
	 */
	float contactViewHalfUsed = 0.0f;
	float2 contactViewBoundCenter{};
	float contactViewBoundRadius = 0.0f;
	bool EnsureContactResources();
	void DrawContactCapture(ID3D11DeviceContext* a_context);

	/** @brief Compiles the statics skin VS (keeping bytecode) and PS on first use. Implemented in SnowDeformation/Statics.cpp. */
	bool EnsureStaticsShaders();
	/** @brief Re-draws this frame's captured projected-snow statics inflated, inside DrawShell's bound state. Implemented in SnowDeformation/Statics.cpp. */
	void DrawCapturedStatics();

	/** @brief Caches a "tile is snow material" bitmask per landscape quad material, for the terrain shader's per-tile snow detection. */
	void TESObjectLAND_SetupMaterial(RE::TESObjectLAND* land);
	/** @brief Publishes the cached snow mask for the material about to be drawn via ExtraFeatureDescriptor bits 11-16. */
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);

	/** @brief Before each Lighting draw (SetupGeometry): a deferred-pass draw with the landscape property flag opens or extends the landscape run (one profiler pass over consecutive terrain draws); any other Lighting draw closes it. Implemented in TerrainData.cpp. */
	void OnLightingDraw(RE::BSRenderPass* a_pass);
	/** @brief Ends the open landscape run. Called on non-terrain Lighting draws, any other shader's SetupTechnique, and DrawShell entry. */
	void CloseLandscapeRun();
	/** @brief Installs SetupTechnique hooks on the non-Lighting batch-rendered BSShader vtables. Called from PostPostLoad. */
	void InstallLandscapeTimingHook();
	/** @brief Registers read-only DevBench tools (snowdeformation.shadertypes). No-op without devbench. Called from DataLoaded; implemented in DevBenchTools.cpp. */
	void InstallDevBenchTools();
	/** @brief Landscape snow (LANDSCAPE-SNOW-PLAN): hooks Lighting RestoreGeometry and Utility Setup/RestoreGeometry; Lighting's SetupGeometry side rides the statics capture hook. Implemented in LandscapeSnow.cpp. */
	void InstallLandscapeSnow();
	/** @brief After the game's SetupGeometry: captures a depth-prepass landscape draw for the ground snow's replay, and moves a sun-cascade one onto LandscapeSnow.hlsl, tessellated near the camera and through the raising VS beyond. */
	void LandscapeSnowSetup(RE::BSRenderPass* a_pass, bool a_lighting);
	/** @brief Before the game's RestoreGeometry: puts back everything LandscapeSnowSetup changed. */
	void LandscapeSnowRestore();
	/** @brief After a Lighting draw went out (its RestoreGeometry): the first deferred-pass draw of the frame records the main pass's viewport depth range (mainViewportMaxDepth). */
	void NoteMainViewport();
	/** @brief Rebuilds the landscape's ShellCB copy and SRV set from the live fields; false while one is missing or the footprint holds no snow. */
	bool LandscapeSnowRefresh();
	/** @brief The surface's SRV set t0-t58 (the slots ShellVertexZ and the landscape hull read), from the fields as they stand now. */
	void LandscapeSurfaceSRVs(ID3D11ShaderResourceView** a_srvs);
	/** @brief The ground snow can draw this frame: the replay's shaders are ready and this frame's depth prepass has been captured. Counts the frames that wanted it and drew no ground. */
	bool LandscapeShadeReady();
	/** @brief The land-blended edges (LANDSCAPE-SNOW-PLAN Phase 5), on whenever the feature is: wherever the land is mixed, its own height blend decides snow or ground (Lighting writes its snow odds into Masks.y); trenches reach the ground; the window bake keeps snow depth only where the land is nearly all snow. */
	bool LandBlendActive() const;
	/** @brief The land's snow-texture weight from which the bake keeps snow depth (smoothstep to 1). */
	float LandBlendDepthStart() const;
	/** @brief A blended class depth under the land-blend rule: positive depth scaled by smoothstep(LandBlendDepthStart, 1, snow weight); unchanged when the mode is off. Shared by the window bake and GetNominalSnowDepthAt. */
	float LandBlendDepth(float a_depth, float a_snowWeight) const;
	/** @brief The resolve's depth bonus (log2 odds per unit of remaining snow) and carve penalty (log2 odds at a full carve). */
	float2 LandBlendContest() const;
	/** @brief The ground snow's prepass geometry: draws this frame's captured depth-prepass landscape through LandscapeSnowShell.hlsl. Binds VS/HS/DS, the HS/DS fields on a_cb (a_shellCB at b0) and the IA; the caller's PS, targets and depth state stay. IA buffers and the b11 slots go back as found. Returns the draws issued. */
	uint32_t DrawLandscapeSnowPrepass(ID3D11DeviceContext* a_context, const ShellCB& a_cb, ID3D11Buffer* a_shellCB);
	/** @brief One-line status for the Debugging menu. */
	std::string LandscapeSnowStatus() const;
	/** @brief DevBench snowdeformation.landscape: switches, per-pass GPU timing, shader state. */
	json LandscapeSnowTool(const json& a_args);
	/** @brief The land's snow-texture weight from which the snow deepens; below it the snow is a film the land's blend cuts. */
	static constexpr float kLandBlendDepthStart = 0.50f;
	/** @brief Land Snow Recolor (LANDSCAPE-SNOW-PLAN 7.4): the land's snow layers wear the shell's snow set texel by texel wherever the land shows (DebugTerrainOverlay bit 256; 512 = the set's RMAOS is bound). Debug A/B, default on, not serialised. */
	bool landRecolor = true;
	/** @brief Land Snow Recolor: gamma luminance where a snow layer's texel starts to read as snow (full 0.18 above). */
	static constexpr float kLandRecolorLumLo = 0.48f;
	/** @brief The recolor runs this frame: the ground snow draws here and the snow set is loaded. */
	bool LandRecolorActive() const { return landRecolor && LandBlendActive() && !GroundShellsSuspended() && shellSnowDiffuseSRV != nullptr; }
	/** @brief Settled mark sources (an unchanged max-carve rewrites nothing): frames a source keeps marking after its last change against its anchor, and the period at which a settled source re-marks once (bounds creep inside the hysteresis tolerance). */
	static constexpr uint32_t kTrenchMarkSettleFrames = 60;
	static constexpr uint32_t kTrenchMarkRefreshFrames = 600;
	/** @brief Hysteresis tolerance in map texels (positions) and world units (stamp z, radius). */
	static constexpr float kTrenchMarkTolTexels = 2.0f;
	/** @brief Mark anchors: stamps matched to the nearest unclaimed anchor, contact draws by ref handle. */
	struct TrenchStampAnchor
	{
		float4 s;
		float2 e;
		uint32_t age;
	};
	std::vector<TrenchStampAnchor> trenchStampAnchors;
	std::vector<TrenchStampAnchor> trenchStampAnchorsNext;
	std::vector<uint8_t> trenchStampAnchorClaimed;
	struct TrenchRectAnchor
	{
		float minX, minY, maxX, maxY;
		uint32_t age;
	};
	std::unordered_map<uint32_t, TrenchRectAnchor> trenchRectAnchors;
	std::unordered_map<uint32_t, TrenchRectAnchor> trenchRectAnchorsNext;
	/** @brief Measurement, runtime-only: bitmask zeroing shell PS feature flags before the ShellCB upload (bit 0 parallax, 1 normal map, 2 RMAOS, 3 crisp shadows, 4 screen-space shadows, 5 unused, 6 glints, 7 point lights, 8 skylighting, 9 albedo texture). The statics shell shares the CB, so it sees the kills too. */
	uint32_t shellKillMask = 0;
	/** @brief The effect-cost probe: turns each shellKillMask effect off in turn and reads how far the snow GPU rows drop. Started from Debugging Options > Landscape Shell or DevBench; ticks once per DrawShell. */
	struct EffectCostProbe
	{
		static constexpr uint32_t kEffects = 9;
		static constexpr const char* kNames[kEffects] = { "Parallax", "Normal map", "Roughness / AO maps", "Crisp shadows", "Screen-space shadows",
			"Glints", "Point lights", "Skylighting", "Snow albedo texture" };
		/** @brief shellKillMask bit per effect. */
		static constexpr uint32_t kKillBits[kEffects] = { 0, 1, 2, 3, 4, 6, 7, 8, 9 };
		bool running = false;
		bool done = false;
		uint32_t step = 0;  // even = everything on, odd = effect step / 2 off
		uint32_t frame = 0;
		std::vector<float> samples;
		uint32_t savedMask = 0;
		float withMs[kEffects] = {};
		float withoutMs[kEffects] = {};
	};
	EffectCostProbe effectCost;
	void StartEffectCostProbe();
	void TickEffectCostProbe();
	/** @brief DevBench: applies ground measurement args (shellKill, shellStats, ...) and returns the current switches plus the last pipeline statistics. Implemented in DevBenchTools.cpp. */
	json DevBenchShellArgs(const json& a_args);
	/** @brief DevBench: applies volume measurement args (memorySeconds, showRings) and returns the volume's live state per level. Implemented in DevBenchTools.cpp. */
	json DevBenchVolumeArgs(const json& a_args);
	/** @brief DevBench: the reader gates' A/B switches (interior height maps, masks copy, march idle, blood latch, land-mask feature test, record growth) and their counters. Implemented in DevBenchTools.cpp. */
	json DevBenchReaderGatesArgs(const json& a_args);
	/** @brief Stored trench depth (0-1) at a world position in the active worldspace, under the store lock. Any thread. Implemented in TrenchStore.cpp. */
	float ProbeTrenchStore(float a_worldX, float a_worldY);
	/** @brief Profiler timers are a shared 128-slot frame budget; a frame may open this many landscape runs. */
	static constexpr uint32_t kLandscapeRunCap = 8;
	bool landscapeRunOpen = false;
	uint32_t landscapeRunFrame = 0;
	uint32_t landscapeRunsThisFrame = 0;
	uint32_t landscapeBatchesThisFrame = 0;
	/** @brief Log-once census over in-world terrain frames: runs, batches, runs past the cap, runs dropped at the frame boundary. */
	uint64_t landscapeCensusRuns = 0;
	uint64_t landscapeCensusBatches = 0;
	uint64_t landscapeCensusCapped = 0;
	uint64_t landscapeCensusDropped = 0;
	uint32_t landscapeCensusFrames = 0;
	uint32_t landscapeCensusPeakRuns = 0;
	bool landscapeCensusLogged = false;

	/** @brief Diagnostics shown in the settings UI: landscape materials that hit/missed the snow-mask cache at draw time. */
	std::atomic<uint64_t> landMaskHits{ 0 };
	std::atomic<uint64_t> landMaskMisses{ 0 };
	/** @brief Hits on a material whose feature is not a landscape one (counted only with the feature gate off). */
	std::atomic<uint64_t> landMaskForeignHits{ 0 };
	/** @brief DevBench A/B: true takes the snow-mask lock and lookup for every material, as before the feature gate. */
	bool landMaskFeatureGateOff = false;

	/** @brief Thread-safe size read for the settings UI. */
	size_t snowMasksSizeForUI()
	{
		const std::shared_lock lock(snowMaskMutex);
		return snowMasks.size();
	}

	/** @brief Runtime-only diagnostic toggles; not persisted in settings JSON. */
	bool showDebugTexture = false;
	bool debugTerrainOverlay = false;

	/** @brief Debug melt emitter: a stationary heat source dropped from the menu at the player, exercising the additive stamp path before any spell detection exists. Runtime-only, never persisted. */
	bool debugMeltEmitterActive = false;
	/** @brief World position of the dropped emitter. */
	RE::NiPoint3 debugMeltEmitterPos{};
	/** @brief Emitter radius in world units (300 = kFireClearRadius, the campfire benchmark). */
	float debugMeltEmitterRadius = 100.0f;
	/** @brief Emitter accumulation rate in depth units per second at the core. */
	float debugMeltEmitterRate = 0.50f;
	/** @brief Tints classified projected-snow pixels magenta (DebugTerrainOverlay bit 4) so the SnowProjectedIsSnow bit is verifiable in-game without a capture. */
	bool debugProjSnowView = false;
	/** @brief DebugTerrainOverlay bit 32: the Lighting recolor paints its real blend weight as a grey ramp and projected draws it does not classify in red. Hold against the object snow debug view's Projected mask mode (R = the skin's reconstruction). */
	bool debugProjWeightView = false;
	/** @brief DebugTerrainOverlay bit 64: the recolor paints the SNOW TEXTURE IT JUST SAMPLED, raw and unshaded, instead of blending it. Answers the one question the magenta view cannot - whether HorizonSnowAlbedo is reaching the draw at all - since the magenta path bypasses the sample. Not serialized, like every debug view. */
	bool debugProjAlbedoView = false;

protected:
	/** @brief Fills perFrameData.Stamps from the player and nearby loaded actors. Implemented in SnowDeformation/Stamping.cpp. */
	void GatherStamps(PerFrame& perFrameData);

	std::unordered_map<uintptr_t, uint8_t> snowMasks;
	std::shared_mutex snowMaskMutex;

	/** @brief Baked cells keyed by (cellX << 32) | cellY; entries carry their worldspace, which the window rebuild must match. */
	std::unordered_map<uint64_t, ShellCellData> shellCells;
	/** @brief Cells whose land was rejected as dead-flat filler (city worldspaces), keyed like shellCells, value = the worldspace that looked. A tombstone: "looked, no real land here", so the snow-presence gate can tell known-bare from never-baked without keeping the filler data. */
	std::unordered_map<uint64_t, uint32_t> shellFillerCells;
	/** @brief Cells in the current window whose blended depth goes positive somewhere, keyed like shellCells. Rebuilt with the window; read per frame by DeformationWindowHasSnow. */
	std::unordered_set<uint64_t> shellSnowyCells;
	mutable std::shared_mutex shellSnowyCellMutex;
	/** @brief Snow-presence verdicts, for the gate readout and the fail-open rule. */
	enum SnowGateVerdict : uint32_t
	{
		kSnowGateSnowy = 0,
		kSnowGateUnknown = 1,
		kSnowGateBare = 2,
	};
	/**
	 * @brief Whether any cell within a_halfExtentUnits of the deformation window's centre carries positive snow depth, plus two cells of lead so the answer flips before the player arrives.
	 *
	 * Three-way underneath: a cell can be known snowy, known bare (baked for the
	 * active worldspace, or tombstoned as city filler), or never looked at. With
	 * a_unknownIsSnowy the never-looked-at case answers true - after a city gate,
	 * a door, or fast travel the surrounding cells take seconds to bake, and a
	 * gate that fails closed on that ignorance suspends stamping while the player
	 * already stands on snow. Steady state is unaffected: once baked, the verdict
	 * is honest and the skip engages as before. The shell-footprint caller keeps
	 * a_unknownIsSnowy false - at shell-span reach, unbaked cells are the norm
	 * (land past uGrids never loads), so failing open there would never suspend.
	 * Implemented in SnowDeformation/TerrainData.cpp.
	 */
	bool WindowHasSnow(float a_halfExtentUnits, bool a_unknownIsSnowy = false, uint32_t* a_verdictOut = nullptr) const;
	/** @brief The loaded-cell square around the player (uGridsToLoad), inclusive cell coordinates; the landscape shell ends at its seam and land past it never loads. False before the player exists. Implemented in TerrainData.cpp. */
	bool LoadedCellSquare(int& a_minX, int& a_minY, int& a_maxX, int& a_maxY) const;
	/** @brief Three-way verdict over an inclusive cell range, by WindowHasSnow's rules: snowy if any cell carries positive depth, unknown if any cell has never been looked at, else bare (an empty range is bare). Implemented in TerrainData.cpp. */
	uint32_t CellRangeSnowVerdict(int a_minX, int a_minY, int a_maxX, int a_maxY) const;
	/** @brief CellRangeSnowVerdict narrowed to the view: a snowy range answers bare when no snowy cell's box (land height range plus kViewGateMarginZ) meets the camera frustum. Unknown stays unknown. Implemented in TerrainData.cpp. */
	uint32_t ViewSnowVerdict(int a_minX, int a_minY, int a_maxX, int a_maxY, const ShellCB& a_cb);
	static constexpr float kViewGateMarginZ = 256.0f;
	/** @brief Land height range per snowy cell for ViewSnowVerdict, dropped when snowDepthVersion moves. */
	std::unordered_map<uint64_t, std::pair<float, float>> viewGateCellZ;
	uint32_t viewGateCellZVersion = UINT32_MAX;
	/** @brief A/B, runtime-only: the shell gate asks the whole loaded square again instead of the view. */
	bool shellViewGateOff = false;
	/** @brief With the ground gate closed and every frustum corner ray below the horizon, nothing visible lies below the loaded square's lowest land: the view cannot leave the loaded square, so this frame's LOD-batch captures are dropped, and the loaded ones too when none of their bounds meets the frustum's box down to that floor. No claim when the box leaves the loaded square or a cell is unbaked. Implemented in TerrainData.cpp. */
	void CullUnseenCaptures(const ShellCB& a_cb);
	/** @brief Lowest land height over the loaded square, keyed on the square and snowDepthVersion; invalid while any cell of it is unbaked. */
	float squareFloorZ = 0.0f;
	bool squareFloorValid = false;
	uint32_t squareFloorVersion = UINT32_MAX;
	int squareFloorKey[4] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };
	/** @brief Captures CullUnseenCaptures dropped this frame (DevBench readout). */
	uint32_t captureViewCulled = 0;
	/** @brief Why CullUnseenCaptures made no claim this frame: 0 claimed, 1 off/empty/interior, 2 no square, 3 a square cell unbaked, 4 eye below the floor, 5 a corner ray at or above the horizon, 6 the view leaves the loaded square. */
	uint32_t captureCullBail = 1;
	/** @brief A/B, runtime-only: keep every capture whatever the view. */
	bool captureViewCullOff = false;
	/** @brief Whether ground within two cells of a world XY, clipped to the loaded square, can carry snow (snowy or still baking). An object wholly outside the square (LOD batches) takes the square's own verdict. The capture hook's per-object gate; cached per cell on the render thread, dropped when snowDepthVersion moves or the square does. */
	bool GroundNearHasSnow(float a_x, float a_y);
	std::unordered_map<uint64_t, uint8_t> groundSnowCellCache;
	uint32_t groundSnowCellCacheVersion = 0;
	/** @brief Per-frame inputs of GroundNearHasSnow (interior flag, loaded square), taken once per frame. */
	uint32_t groundSnowFrame = UINT32_MAX;
	bool groundSnowFrameInterior = false;
	bool groundSnowFrameHasSquare = false;
	int groundSnowFrameSquare[4] = {};
	int groundSnowCacheSquare[4] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };
	uint8_t groundSnowSquareVerdict = kSnowGateUnknown;
	bool groundSnowSquareValid = false;
	/** @brief WindowHasSnow over the deformation window. False means nothing can read the map here, so its passes are skipped. Never-baked ground counts as possibly snowy, and the verdict lands in deformSnowVerdict for the debug readout. */
	bool DeformationWindowHasSnow() const { return WindowHasSnow(deformWorldSize * 0.5f, true, &deformSnowVerdict); }
	/** @brief Whether anything can read the deformation map: the ground's snow in the window, or, in a walled city with its ground off, a loaded capture (object snow takes prints) whose bound last frame met the window. Implemented in TerrainData.cpp. */
	bool DeformationWindowHasReaders() const;
	/** @brief WindowHasSnow over the ground snow's own footprint, which reaches far past the deformation window. False means no snow can stand above ground, so nothing casts. */
	bool ShellFootprintHasSnow() const { return WindowHasSnow(ShellWarpedHalfSpan()); }
	/** @brief Whether the next frame's sun cascades may raise snow: a snowy or never-looked-at cell of the loaded square, or a snowy cell one past it (the square moves when the player crosses a cell), that meets one of this frame's cascade footprints. Land past the square never bakes, so unknown there does not count. Without sun descriptors, any such cell. Implemented in TerrainData.cpp. */
	bool CascadeSnowSoon();
	/** @brief Four side planes of a clip footprint, world space, inside where >= 0. */
	using FootprintPlanes = std::array<std::array<double, 4>, 4>;
	static constexpr size_t kMaxSunFootprints = 4;
	/** @brief The sun's shadow-map footprints this frame from its descriptors (lightTransform: absolute world to slice UV, inside 0..w); false with no sun shadow light or descriptor. Implemented in TerrainData.cpp. */
	bool SunCascadeFootprints(FootprintPlanes* a_out, size_t& a_count) const;
	/** @brief Whether a cell the sun cascades can raise meets one of a_count footprints: the snowy cells of the loaded square, and with a_soon its never-looked-at cells and the snowy cells one past it. A cell's box spans the land of it and its neighbours up to a_rise above, grown sideways by a_rise and a vertex spacing; an unknown cell takes the known land's range. True with no square or no known land. Implemented in TerrainData.cpp. */
	bool SnowCellsMeetFootprints(const FootprintPlanes* a_planes, size_t a_count, float a_rise, bool a_soon);
	/** @brief The sun cascade being drawn (the per-frame buffer's camera) meets a snowy cell's raised land; cached per cascade camera. Implemented in LandscapeSnow.cpp. */
	bool LsCascadeMeetsSnow();
	/** @brief The raise's highest lift above the land: LsConfig::lift, the hull's cull height. Implemented in LandscapeSnow.cpp. */
	float LsRiseReach() const;
	/** @brief Last verdict from DeformationWindowHasSnow, for the debug menu. */
	mutable uint32_t deformSnowVerdict = kSnowGateUnknown;
	/** @brief Last verdict of the landscape shell's gate (DrawShell, over the loaded square narrowed to the view), for the debug menu. */
	mutable uint32_t shellSnowVerdict = kSnowGateUnknown;
	/** @brief Consecutive frames the loaded square has read bare. The ground draw and the depth sync are skipped once this reaches kShellGateBareFrames; a snowy cell in view or a still-baking cell resets it, so the surface returns the frame snow enters the view. */
	uint32_t shellGateBareFrames = 0;
	static constexpr uint32_t kShellGateBareFrames = 30;
	/** @brief Set while the deformation passes are being skipped, so resuming can force a clear instead of trusting an accumulated scroll delta. */
	bool deformSuspended = false;
	/** @brief Consecutive frames the deformation window (clipped to the loaded square) has read bare; the passes suspend once this reaches kShellGateBareFrames. */
	uint32_t deformGateBareFrames = 0;
	mutable std::shared_mutex shellCellMutex;
	std::atomic<bool> shellDataDirty{ true };
	/** @brief Bumped on every write to shellTerrainTexture (window upload, heightmap fill); render thread only. Bakes that read the window key on it. */
	uint32_t shellTerrainVersion = 0;
	/** @brief Bumped on every write that can change GetNominalSnowDepthAt's answer: shellCells, landTextures, their depths, activeWorldspace. The capture hook's per-record depth memo is valid while its stamp matches. */
	std::atomic<uint32_t> snowDepthVersion{ 1 };
	/** @brief Landscape textures discovered by the bake; indices are stable for the session and are what the baked cells store. */
	std::vector<LandTextureEntry> landTextures;
	/** @brief Substring filter for the texture list. Runtime UI state, not persisted. */
	std::string textureFilter;
	/** @brief Registry index by the form ID of the texture set the land is drawn with, not the LTEX: a seasonal swap changes the set under the same record. */
	std::unordered_map<uint32_t, uint16_t> landTextureByTextureSet;
	std::shared_mutex landTextureMutex;

	/** @brief Form ID of the worldspace the camera is in; 0 until the first exterior. Interiors keep the last exterior's value. */
	std::atomic<uint32_t> activeWorldspace{ 0 };
	/** @brief Worldspace the current terrain window was built for. */
	uint32_t shellWindowWorldspace = 0;
	int shellWindowCellX = INT_MIN;
	int shellWindowCellY = INT_MIN;
	std::vector<float> shellUploadScratch;

	float2 windowOrigin = { 0, 0 };
	DirectX::XMINT2 pendingScrollDelta = { 0, 0 };
	/** @brief Physical position of logical texel (0,0) in the toroidal map. Advanced with windowOrigin (same site, same gate) so every consumer CB filled afterwards carries the pair consistently; kept masked to [0, dim). */
	DirectX::XMINT2 mapOrigin = { 0, 0 };
	bool clearRequested = true;

	// ---- Runtime render-distance state (driven by the Range* settings) ----
	/** @brief Deformation window world size (2x the Trenches range). Changing it clears the map. */
	float deformWorldSize = 14000.0f;
	bool trenchRangeDirty = false;
	/** @brief Deformation map resolution. */
	uint deformMapDim = kTextureDim;
	/** @brief Settings::DeformMapResolution changed; ApplyRangeSettings recreates the map and the store's window-sized companions and clears; the world-anchored store re-injects what it holds. */
	bool deformMapDimDirty = false;
	/** @brief Settings::VRAMAwareTrenchMap's cap on the map (0 = none): set at most once a session by TickVRAMLog, applied by ApplyRangeSettings. */
	uint deformMapVramCap = 0;
	bool rangeInitApplied = false;

	// ---- Idle skip (DEFORMATION-UPDATE-PLAN S1) ----
	// Both map dispatches are skipped while every input is idle (no scroll,
	// stamps, waves, inject, refill or clear) AND the last executed pass
	// reported the map at its fixed point. The flag is written by the CS at
	// stored (half) precision and read back through a small staging ring, so
	// the default on any doubt - readback late, resolution changed, feature
	// re-enabled - is to keep running.
	/** @brief Diagnostic override: keeps the update dispatching every frame so the pass can be measured at rest. Runtime-only. */
	bool debugForceDeformationUpdate = false;
	/** @brief Diagnostic override: the tile-dispatch passes cover the whole map instead of their lists (S3's one-click "my trenches froze" cross-check - if a symptom vanishes with this on, a dirty-tracking path missed a writer). Runtime-only. */
	bool debugForceAllTilesDirty = false;
	/** @brief Last frame's verdict, for the menu readout. */
	bool deformIdleSkipped = false;
	winrt::com_ptr<ID3D11Buffer> deformActivityBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> deformActivityUAV;
	static constexpr uint kDeformActivitySlots = 3;
	winrt::com_ptr<ID3D11Buffer> deformActivityStaging[kDeformActivitySlots];
	uint64_t deformActivitySlotSeq[kDeformActivitySlots] = {};
	bool deformActivityPending[kDeformActivitySlots] = {};
	/** @brief Executed update dispatches. */
	uint64_t deformDispatchSeq = 0;
	/** @brief Seq of the last dispatch whose inputs were non-idle; a quiet verdict older than this proves nothing. */
	uint64_t deformLastNonIdleSeq = 0;
	/** @brief Newest readback consumed and its verdict. */
	uint64_t deformFlagSeq = 0;
	bool deformFlagActive = true;
	/** @brief Whether EvolveCS executed in the frame each staging slot describes; its verdict word means nothing on frames it did not run (the buffer is cleared per frame). */
	bool deformActivitySlotEvolveRan[kDeformActivitySlots] = {};
	/** @brief Evolve pass's own verdict: the last executed evolve changed nothing at stored precision. Starts active (errs toward running). */
	bool evolveFlagActive = true;
	/** @brief Seq of the newest consumed evolve verdict. */
	uint64_t evolveVerdictSeq = 0;
	/** @brief Seq of the last frame that re-armed evolve: an external write landed (ring or stamps) so the settled verdict is stale. RefillAmount > 0 arms directly, per frame. */
	uint64_t evolveLastArmSeq = 0;
	/** @brief Last frame's evolve decision, for the menu readout. */
	bool evolveIdleLastFrame = false;
	/** @brief BermReach the bake was last marked all-dirty for: a new Berm Steepness moves every texel's taps, so the whole field rebuilds. */
	float bermReachBaked = 1.0f;
	/** @brief Stamp set of the last EXECUTED dispatch, for the tolerance match. Planted feet stamp every frame, so a count can never reach zero while anyone stands on snow; "unchanged since the verdict" is the idle test instead. A quantized hash was tried first and flickered: a swaying foot straddling a grid-cell boundary reads as change every few frames, and each flicker buys dispatch + verdict latency - permanent 0% skip. Matching within a tolerance has no boundaries to straddle. */
	std::vector<float4> lastStampSet;
	std::vector<float4> lastStampEnds;
	std::vector<uint8_t> stampMatchUsed;
	std::vector<uint32_t> stampMatchIndex;
	/** @brief Why the last frame ran, for the menu readout (bit 0 scroll, 1 stamps, 2 waves, 3 inject, 4 refill, 5 clear, 6 map-active, 7 verdict-stale). */
	uint32_t deformIdleBlockers = 0;
	/** @brief Skip rate over the previous 300 decisions (fraction; negative until the first window closes). Separates "fully idle" from a flickering skip in one readout. */
	float deformSkipRate = -1.0f;
	uint32_t deformSkipTallyFrames = 0;
	uint32_t deformSkipTallySkipped = 0;
	/** @brief Stamp-set changes over the same window, split by mechanism: count changed (a stamp appeared or vanished - the plant-band flicker signature) vs drift (a matched stamp moved past tolerance). Published with the rate. */
	uint32_t stampSetCountChanges = 0, stampSetDriftChanges = 0;
	uint32_t stampSetTallyCount = 0, stampSetTallyDrift = 0;
	/** @brief Changed-texel counts from the newest consumed verdict - the magnitude behind "map-active": a handful is a precision tail, millions is a logic bug. Per channel, the counts name the term (depth / melt-scorch / crust-impact). */
	uint32_t deformChangedTexels = 0;
	uint32_t deformChangedDepth = 0;
	uint32_t deformChangedMelt = 0;
	uint32_t deformChangedCrustDep = 0;
	/** @brief Sum of the per-texel max delta (fixed-point 1e6). Mean = sum/count is a term's fingerprint: the slump step is SlumpRate x 0.5 x dt and scales with the slider. */
	uint32_t deformChangedDeltaSum = 0;
	/** @brief Bounding box of the changed texels (map coords), from the same verdict. Valid while count > 0. A few hundred churning texels are sub-pixel in the 512 view; the box is what makes them findable. */
	uint32_t deformChangedMinX = 0, deformChangedMinY = 0;
	uint32_t deformChangedMaxX = 0, deformChangedMaxY = 0;
	/** @brief Runtime-only: the update CS paints result-vs-carried per texel (R depth, G melt/scorch, B crust|impact energy) into a map-sized RGBA8 shown in the menu. Answers WHERE the map claims to be evolving. */
	bool debugActivityView = false;
	winrt::com_ptr<ID3D11Texture2D> activityViewTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> activityViewSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> activityViewUAV;
	uint32_t activityViewDim = 0;
	/** @brief Lazily (re)creates the activity-view texture at the current map dimension. */
	void EnsureActivityViewTexture();
	/** @brief Consumes completed activity readbacks (non-blocking). */
	void PollDeformActivity(ID3D11DeviceContext* a_context);

	// ---- Persistent trenches: world-anchored sparse tile store ----
	// The map is a camera-following window and DeformationUpdateCS discards
	// whatever the scroll pushes past its edge, so trenches die when the player
	// walks away. Departing texels are read back into tiles on a FIXED world
	// grid and put back when the window returns. Fixed grid, not texel indices:
	// RangeTrenchesM changes the map's texel size at runtime (2.0-13.7 units),
	// so stored indices would be the wrong scale on the way back in.

	/** @brief World units per store tile edge. */
	static constexpr float kTrenchTileWorld = 512.0f;
	/** @brief Texels per store tile edge; 512/128 = 4 world units per texel, whatever the range slider is set to. */
	static constexpr int kTrenchTileDim = 128;
	/** @brief Depth below which a texel stores as nothing, so refill remnants do not keep tiles alive that hold no trench. */
	static constexpr float kTrenchStoreEpsilon = 0.02f;
	/** @brief A scroll wider than this on either axis is a jump: the whole map is flushed instead of an edge band. */
	static constexpr int kTrenchBandMax = 32;

	struct TrenchTileKey
	{
		uint32_t worldspace;
		int32_t x;
		int32_t y;
		bool operator==(const TrenchTileKey&) const = default;
	};

	struct TrenchTileKeyHash
	{
		size_t operator()(const TrenchTileKey& a_key) const noexcept
		{
			size_t h = a_key.worldspace * 0x9E3779B9u;
			h ^= (size_t)(uint32_t)a_key.x + 0x9E3779B9u + (h << 6) + (h >> 2);
			h ^= (size_t)(uint32_t)a_key.y + 0x9E3779B9u + (h << 6) + (h >> 2);
			return h;
		}
	};

	struct TrenchTile
	{
		std::vector<uint8_t> depth;
		/** @brief How lately each texel was cut, 255 just now down to 0 at kTrackAgeHorizon ageing-clock units old or never (kTrackFreshPerUnit). Empty = all 0: the tile's tracks have aged out. Decays on the ageing clock as depth does on the refill's. */
		std::vector<uint8_t> fresh;
		/** @brief Decay clock reading when these bytes were last brought up to date. Per tile, so the sweep can lag without ever being wrong. */
		float clock = 0.0f;
		/** @brief trenchAgeingTotal reading the fresh bytes were last brought up to. */
		double freshClock = 0.0;
		/** @brief Game hours when this tile was last written or last stood inside the window. The LRU key. */
		float lastTouch = 0.0f;
		/** @brief Exact encoded size under kTrenchRLE, measured by the sweep. What the budget is spent in - a raw byte count would bound the wrong number. */
		uint32_t encodedBytes = 0;
	};

	/** @brief Per-tile co-save header: key, clock, last touch, the depth payload's length, the freshness clock and its payload's length. The encoder's own bytes are counted separately. */
	static constexpr uint32_t kTrenchTileHeaderBytes = 32;
	/** @brief Freshness bytes per ageing-clock unit of age: 255 over kTrackAgeHorizon. */
	static constexpr double kTrackFreshPerUnit = 255.0 / kTrackAgeHorizon;
	/** @brief Store encoding, fixed here because the budget accounting must agree with the writer byte for byte: byte-oriented RLE, pairs of (count 1-255, value), a run longer than 255 split across pairs, and a tile kept raw if that ever exceeds its raw size. */
	static constexpr uint32_t kTrenchRLEPairBytes = 2;

	/** @brief Quantised depth per trodden tile. Untrodden ground has no entry, and a tile that decays to nothing is erased. */
	std::unordered_map<TrenchTileKey, TrenchTile, TrenchTileKeyHash> trenchTiles;

	/** @brief Monotonic "depth removed since the store began", in 0-1 depth units. A tile's decay is the difference between this and its own clock, which is what lets a tile sit out of the window for a week and come back correct. */
	float trenchDecayClock = 0.0f;
	/** @brief SnowClimateState::ageingTotal as of this frame's tick, under the store's lock: the clock the fresh bytes age by. */
	double trenchAgeingTotal = 0.0;

	/** @brief Keys still to visit this sweep cycle; refilled from the store when it empties. Amortised so no frame pays for the whole store. */
	std::vector<TrenchTileKey> trenchSweepQueue;
	size_t trenchStatNonZero = 0;
	size_t trenchStatThin = 0;
	size_t trenchStatSweptTiles = 0;
	size_t trenchAccumNonZero = 0;
	size_t trenchAccumThin = 0;
	size_t trenchAccumTiles = 0;
	/** @brief Tiles decay erased this sweep cycle. Reported, because the old silent prune is why a store that was deleting itself took a code review rather than a glance at the log to find. */
	size_t trenchAccumErased = 0;
	/** @brief Tiles the budget evicted this cycle. */
	size_t trenchAccumEvicted = 0;
	/** @brief Tiles created this cycle. Counted so the census can attribute every change rather than only the losses. */
	size_t trenchAccumAdded = 0;
	/** @brief Live tile count, published rather than read off the container so the menu never races the game thread. */
	size_t trenchStatTiles = 0;
	/** @brief Encoded size of every live tile, refreshed each sweep cycle. What the budget is measured against. */
	size_t trenchEncodedTotal = 0;
	/** @brief Scratch for the eviction's sort; kept so a cap breach does not allocate. */
	std::vector<std::pair<float, TrenchTileKey>> trenchEvictScratch;

	/**
	 * @brief Squared distance from the window centre beyond which new tiles are
	 * refused, set by the last eviction to the nearest tile it had to drop.
	 *
	 * Without it the writer and evictor thrash: the mirror creates a tile for
	 * any trodden ground it finds, knowing nothing about the budget, and
	 * eviction culls the furthest a few frames later. A save then catches an
	 * arbitrary mid-thrash spread. Effectively infinite whenever the store
	 * fits, so the default budget never feels it.
	 */
	float trenchKeepRadiusSq = std::numeric_limits<float>::max();

	/** @brief What one staged band covers, captured at copy time: a clear, a worldspace change or a range change all move the live values out from under the map before the readback lands. */
	struct TrenchBandCopy
	{
		float2 origin;
		float texel;
		uint32_t worldspace;
		int32_t x0, y0, w, h;
		/** @brief The ageing clock the copied dates were written on, and the store's unwrapped one at the copy. */
		float ageingClock = 0.0f;
		double ageingTotal = 0.0;
	};

	/** @brief R8G8 depth and freshness the update CS reads for texels the scroll brings in from outside the window. Only the arriving band is uploaded; in-window texels overwrite it from the previous map, so the rest may be stale. */
	winrt::com_ptr<ID3D11Texture2D> trenchInjectTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchInjectSRV;
	std::vector<uint8_t> trenchInjectScratch;

	/** @brief Single-channel copy of the map's displaced depth, for the menu. The map itself cannot be shown honestly: ImGui blends by alpha and the map's .w is the impact energy, so drawing it directly renders depth through that energy and reads blank right after a load (the energy is never stored). R8_UNORM samples as (depth,0,0,1), so this one cannot lie. */
	winrt::com_ptr<ID3D11Texture2D> trenchDebugTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchDebugSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> trenchDebugUAV;
	ID3D11ComputeShader* trenchDebugCS = nullptr;
	/** @brief Compiles the debug copy shader on first use. */
	ID3D11ComputeShader* GetTrenchDebugCS();
	/** @brief Copies the freshly written map's depth into the debug texture. Only while the menu is showing it. */
	void UpdateTrenchDebugTexture();

	/** @brief One departing band per axis, double buffered; mapped a frame later so the readback never stalls the render thread. */
	winrt::com_ptr<ID3D11Texture2D> trenchBandStaging[2][2];
	/** @brief The same bands of the track dates (R16_FLOAT), copied and mapped with the map's. */
	winrt::com_ptr<ID3D11Texture2D> trenchClockBandStaging[2][2];
	bool trenchBandValid[2][2] = {};
	TrenchBandCopy trenchBandMeta[2][2] = {};
	int trenchBandRing = 0;

	/**
	 * @brief Rolling mirror of the LIVE window into the store.
	 *
	 * Scroll-out and the jump path only teach the store about ground that has
	 * LEFT the window, so a trench dug and saved without moving is never stored.
	 * Folding a slice in every frame keeps the store continuously correct
	 * instead, so no save has to catch a moment.
	 *
	 * The slice is wide because the cursor skips rows nothing has dug and the
	 * copy is trimmed to the last dug row, so cost tracks carved ground rather
	 * than slice size.
	 */
	static constexpr int kTrenchRollRows = 128;
	winrt::com_ptr<ID3D11Texture2D> trenchRollStaging[2];
	winrt::com_ptr<ID3D11Texture2D> trenchClockRollStaging[2];
	bool trenchRollValid[2] = {};
	TrenchBandCopy trenchRollMeta[2] = {};
	int trenchRollRing = 0;
	int trenchRollRow = 0;

	/**
	 * @brief Map rows dug since they were last mirrored, one bit per row.
	 *
	 * A SKIP hint, never a seek target: the cursor only moves forward and these
	 * bits let it step over rows with nothing to add. Seeking to the lowest
	 * marked row instead starves the sweep - stamps mark rows faster than a
	 * slice can clear them, so the cursor never reaches the player, and the
	 * lowest index is the window's south edge rather than the freshest row.
	 *
	 * Safe only because writes are raise-only. Marked after stamps are gathered
	 * and consumed by the next frame's roll.
	 */
	std::vector<uint32_t> trenchDirtyRows;

	/** @brief Flags the rows this frame's carve stamps will write, for the next roll to prioritise. */
	void MarkTrenchDirtyRows(const PerFrame& a_data);
	/** @brief Copies the next slice of the live window toward the store, changed rows first. */
	void RollTrenchWindow();

	/**
	 * @brief Set by the co-save load to force the next update to rebuild the
	 * whole window from the store.
	 *
	 * Injection only reaches texels arriving from outside the window or a full
	 * clear, so a store restored while the player stands on the ground it
	 * describes has no route into an already-populated map. A worldspace change
	 * would force the clear, but loading into the worldspace the main-menu
	 * backdrop already uses is not a change. The clear is correct regardless -
	 * the map still holds another timeline's data.
	 *
	 * Atomic: co-save callbacks run on the game thread, the update on the
	 * render thread.
	 */
	std::atomic<bool> trenchReinjectRequested{ false };

	/** @brief Window state the CURRENT map's contents belong to. The flush uses these, never the live values. */
	float2 trenchMapOrigin = { 0, 0 };
	float trenchMapTexel = 0.0f;
	uint32_t trenchMapWorldspace = 0;
	/** @brief MapOrigin the current map CONTENT was written under. The live mapOrigin advances ahead of it at frame start; the flush and the mirror read content that still belongs to this one. */
	DirectX::XMINT2 trenchMapPhysOrigin = { 0, 0 };
	bool trenchMapPrimed = false;

	/** @brief One-entry tile cache for the inject sampler, which walks a tile's pixel footprint in scan order. */
	TrenchTileKey trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
	const TrenchTile* trenchSampleTile = nullptr;

	/** @brief Creates the inject texture and the band staging ring. */
	bool CreateTrenchStoreResources();
	/** @brief Folds any band copied on an earlier frame into the tile store. */
	void DrainTrenchBands();
	/** @brief Stages the texels this frame's scroll or clear is about to discard. */
	void FlushDepartingTrenches(DirectX::XMINT2 a_scroll, bool a_clearing);
	/** @brief Fills the inject texture for the texels arriving this frame. Returns 1 when the store had anything to put there. */
	uint BuildTrenchInject(DirectX::XMINT2 a_scroll, bool a_clearing);
	/** @brief Reads one mapped band and its dates (null: none, the band stores aged) into the store, splatting each map texel across the store texels it covers. */
	void StoreTrenchBand(const TrenchBandCopy& a_meta, const D3D11_MAPPED_SUBRESOURCE& a_mapped, const D3D11_MAPPED_SUBRESOURCE* a_clocks);
	/** @brief Bilinear store depth at a world position, crossing tile edges through the one-entry cache; o_fresh, when given, the freshness there weighted by the same depth (0-1). */
	float SampleTrenchStore(uint32_t a_worldspace, float a_worldX, float a_worldY, float* o_fresh = nullptr);
	/** @brief Co-save record for the trench tiles. '{@link kTrenchRecordVersion}' is written into every chunk; #33's accumulation claims its own type on the same channel. */
	static constexpr uint32_t kTrenchRecord = 'SNTR';
	/** @brief 2 adds each tile's freshness (track age); 1 still loads, its tracks aged. */
	static constexpr uint32_t kTrenchRecordVersion = 2;

	/** @brief Co-save record for the accumulated layer, on the same channel as the trench tiles. Version 2 carries the held weather beside the scalar. */
	static constexpr uint32_t kAccumRecord = 'SNAC';
	static constexpr uint32_t kAccumRecordVersion = 2;

	/**
	 * @brief Snowfall intensity the accumulator integrates, held at its last
	 * EXTERIOR reading while the player is indoors.
	 *
	 * No interior weather snows, so integrating the live value would melt the
	 * exterior while the player sleeps. Persisted with the scalar, or an indoor
	 * save resumes as clear sky. Atomic: the co-save writer reads it on the
	 * game thread while the tick advances it on the render thread.
	 */
	std::atomic<float> accumWeatherIntensity{ 0.0f };

	/**
	 * @brief Accumulated layer, 0-1: 0 is the authored depth and 1 is
	 * AccumulationPeak times it. See ACCUMULATION-PLAN.md.
	 *
	 * Atomic because the co-save writer reads it on the game thread while the
	 * tick advances it on the render thread. A scalar needs no mutex; the tile
	 * store does, which is why that one has one.
	 */
	std::atomic<float> snowAccumulation{ 0.0f };
	/** @brief Advances the layer off the shared clock. One signed rate, no thresholds: a weather cross-fade turns the curve instead of putting a kink in it. */
	void TickAccumulation();
	/** @brief What the shells multiply the authored depth by: 1.0 when accumulation is off or the layer is at rest, up to AccumulationPeak at full. Never below 1 - a peak under 1 would make snowfall THIN the snow. */
	float GetAccumulationDepthScale() const;
	/** @brief Claims 'SNAC' on the co-save channel. */
	void RegisterAccumulationCoSave();

	/** @brief Co-save record for the snow freshness scalar; its own type so the accumulation record's version is untouched. */
	static constexpr uint32_t kFreshRecord = 'SNFR';
	static constexpr uint32_t kFreshRecordVersion = 1;
	/** @brief Game hours of reference snowfall that take firm snow to fully fresh. */
	static constexpr float kFreshenHours = 2.0f;
	/** @brief Snow freshness, 0-1: 0 = settled and firm (the authored trench look), 1 = loose new snow. Rises with snowfall, settles in clear weather. Atomic for the co-save writer, like snowAccumulation. */
	std::atomic<float> snowFreshness{ 0.0f };
	/** @brief Debug: >= 0 replaces the live freshness (not serialised). */
	float debugFreshnessOverride = -1.0f;
	/** @brief Advances freshness off the shared clock and the accumulator's held weather; call after TickAccumulation. */
	void TickSnowCondition();
	/** @brief Freshness x Settings::FreshSnowSoftness, 0-1: what the three levers ease by. */
	float GetSnowSoftness() const;
	/** @brief Claims 'SNFR' on the co-save channel. */
	void RegisterSnowConditionCoSave();

	/** @brief Starting value of the ageing clock: half floats hold integers exactly in [1024, 2048). */
	static constexpr float kAgeingClockStart = 1024.0f;
	/** @brief Air temperature falls this much per world unit of height (C). */
	static constexpr float kClimateLapse = 0.0005f;
	/**
	 * @brief Snow climate (TRACK-VARIATION-RESEARCH.md 3.2): what kind of snow
	 * lies around the player. The trench look follows it by Snow Climate
	 * (ComputeSnowClimateLook). Render thread only.
	 */
	struct SnowClimateState
	{
		float temperature = 0.0f;
		float wetness = 0.0f;
		float windPacking = 0.0f;
		/** @brief Share of the loaded land painted with snow textures; below 0 until measured. */
		float landSnowShare = -1.0f;
		float rainHeld = 0.0f;
		/** @brief Per climate row: meltwater the snow holds, and how much of it has frozen (TickSnowIce). Co-saved as 'SNIC'. */
		std::array<float, kClimateLUTRows> rowWater{};
		std::array<float, kClimateLUTRows> rowIce{};
		float ageingClock = kAgeingClockStart;
		/** @brief Clock units per game hour, last tick. */
		float ageingRate = 0.0f;
		/** @brief The same clock unwrapped, never reset: the trench store's freshness decays by it. */
		double ageingTotal = 0.0;
		/** @brief Units ageingClock fell back by that the track dates have not followed yet (TrackRebaseCS). */
		float ageingRebase = 0.0f;
		float targetTemperature = 0.0f;
		float termRegion = 0.0f;
		float termWeather = 0.0f;
		float termSun = 0.0f;
		bool primed = false;
		uint32_t shareFrame = 0;
	};
	SnowClimateState snowClimate;
	/** @brief The snow at the player with every override applied: what the readout shows and the shells receive. */
	struct SnowCondition
	{
		float temperature;
		float wetness;
		float stickiness;
		float firmness;
		float density;
		float freshness;
		float windPacking;
	};
	SnowCondition GetSnowCondition() const;
	/** @brief Advances the meltwater and the ice per climate row: water the snow held freezes below 0 C, drains above it, thaws when wet again and is buried by snowfall as tracks are. Inside TickSnowClimate. */
	void TickSnowIce(float a_elapsed, float a_snowing);
	/** @brief Ice in climate row a_row as the shells show it: the override, or the live value. */
	float SnowIceShown(uint32_t a_row) const;
	/** @brief Snow Climate x Wet Snow and Ice: what every Stage 4 term is scaled by. */
	float SnowWetIceStrength() const;
	static constexpr uint32_t kIceRecord = 'SNIC';
	static constexpr uint32_t kIceRecordVersion = 1;
	/** @brief Claims 'SNIC': the rows' meltwater and ice. */
	void RegisterSnowIceCoSave();
	/** @brief The snow at the player had the air there this temperature: wetness moves with it as height moves it per pixel, overrides apply. */
	SnowCondition SnowConditionAt(float a_temperature) const;
	/**
	 * @brief The trench look the climate asks for (TRACK-VARIATION-RESEARCH.md 3.3).
	 * Each factor scales its slider, 1 in settled sticky snow just below
	 * freezing, where the look is the sliders' own; Snow Climate is applied.
	 */
	struct SnowClimateLook
	{
		float strength = 0.0f;
		/** @brief At the player, 0-1, before Snow Climate. */
		float berm = 1.0f;
		float lip = 1.0f;
		float teeth = 1.0f;
		/** @brief Wall softness: how far loose snow falls back into a print, toward the berm field's short blur. */
		float slough = 0.0f;
		std::array<float4, kClimateLUTRows> lut{};
	};
	SnowClimateLook ComputeSnowClimateLook() const;
	/** @brief Advances the climate off the shared clock; call after TickSnowCondition. */
	void TickSnowClimate();
	/** @brief Mean snow-texture weight of the loaded land around a point, from the baked cells; below 0 when none are baked. */
	float LandSnowShareAround(float a_x, float a_y);
	/** @brief Debug overrides, not saved. Temperature has its own switch; the others replace the live value at 0 or above. */
	bool debugClimateTemperatureOn = false;
	float debugClimateTemperature = -5.0f;
	float debugClimateShareOverride = -1.0f;
	float debugClimateWetnessOverride = -1.0f;
	float debugClimateStickinessOverride = -1.0f;
	float debugClimateFirmnessOverride = -1.0f;
	float debugAgeingRateOverride = -1.0f;
	float debugClimateIceOverride = -1.0f;
	/** @brief Condition view: 0 off, 1 temperature, 2 wetness, 3 stickiness, 4 firmness, 5 all three. */
	int debugConditionView = 0;
	/** @brief Writes the scalar. */
	void SaveAccumulation(const SKSE::SerializationInterface* a_intfc);
	/** @brief Reads it back, refusing a version this build does not know. */
	void LoadAccumulation(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);

	// ---- Snow events: moments the snow is thrown, for a particle feature ----
	// MD plans/SSPP-SNOW-PLAN.md stage E0. Collected only while a consumer asks
	// (RequestSnowEvents) or the debug census counts; render thread throughout.
	// Implemented in SnowDeformation/SnowEvents.cpp.

	enum class SnowEventKind : uint8_t
	{
		KickUp,
		Crumble,
		Landing,
		Knockdown,
		Rush,
		BodyDrop,
		Shout,
		Blast,
		Lightning,
		Count
	};
	struct SnowEvent
	{
		RE::NiPoint3 position;
		/** @brief Units per second: the foot or the body; a shout's heading times its front speed; zero for blasts. */
		RE::NiPoint3 velocity;
		/** @brief 0-1. Kick-up: share of the layer the foot moved through. Crumble: wall height over the layer, times looseness. Landing, knockdown, rush: that impact energy term. Body drop: peak fall over free fall from the body's own bound radius. Shout, blast, lightning: the mark's strength. */
		float strength = 0.0f;
		/** @brief Kick-up: snow face pushed, units squared. Crumble: wall height. Landing: fall speed. Rush: ground speed. Body drop: peak fall speed. Shout: reach. Blast, lightning: radius. */
		float amount = 0.0f;
		/** @brief Untrampled snow surface and layer depth at the position. */
		float surfaceZ = 0.0f;
		float snowDepth = 0.0f;
		/** @brief Snow Climate stickiness at the player. */
		float stickiness = 0.0f;
		/** @brief The actor, 0 for spell marks. */
		uint32_t formID = 0;
		uint32_t seed = 0;
		SnowEventKind kind = SnowEventKind::KickUp;
	};
	static constexpr uint32_t kMaxSnowEventsPerFrame = 256;
	/** @brief A consumer calls this every frame it wants events; collection stops two frames after the last call. */
	void RequestSnowEvents();
	bool SnowEventsWanted() const;
	/** @brief Last frame's events, valid until the next TickSnowEvents. */
	const std::vector<SnowEvent>& PublishedSnowEvents() const { return snowEventsPublished; }
	void PushSnowEvent(const SnowEvent& a_event);
	/** @brief Per actor from GatherStamps, after the floating and incorporeal gates. */
	void CollectActorSnowEvents(RE::Actor* a_actor, bool a_dead, const RE::NiPoint3& a_position, float a_groundZ,
		const StampBones* a_bones, const RE::NiAVObject* a_root);
	/** @brief After GatherStamps: spell marks opened this frame, then publish and count. */
	void TickSnowEvents();
	std::vector<std::string> SnowEventCensusLines() const;
	/** @brief Debug only, not saved: the census is the consumer while no particle feature reads the events. */
	bool debugSnowEventCensus = false;
	std::vector<SnowEvent> snowEventsBuilding;
	std::vector<SnowEvent> snowEventsPublished;
	uint32_t snowEventRequestFrame = UINT32_MAX;
	uint32_t snowEventsDroppedFrame = 0;
	struct SnowEventFoot
	{
		RE::NiPoint3 prev;
		double time = 0.0;
		float travel = 0.0f;
		float pushed = 0.0f;
		float wallHeight = 0.0f;
		uint32_t frame = 0;
		bool inSnow = false;
	};
	std::unordered_map<uint64_t, SnowEventFoot> snowEventFeet;
	struct SnowEventBody
	{
		double trackTime = -1.0;
		double time = 0.0;
		RE::NiPoint3 prev;
		float centreZ = 0.0f;
		float peakFall = 0.0f;
		float descent = 0.0f;
		uint32_t frame = 0;
		bool valid = false;
		bool knocked = false;
		bool rushing = false;
	};
	std::unordered_map<uint32_t, SnowEventBody> snowEventBodies;
	/** @brief Spell marks already turned into events, by position, element and kind; value = last frame seen. */
	std::unordered_map<uint64_t, uint32_t> snowEventMarksSeen;
	struct SnowEventCensus
	{
		std::array<uint32_t, size_t(SnowEventKind::Count)> window{};
		std::array<uint32_t, size_t(SnowEventKind::Count)> perSecond{};
		std::array<uint64_t, size_t(SnowEventKind::Count)> total{};
		std::array<SnowEvent, size_t(SnowEventKind::Count)> last{};
		std::array<float, size_t(SnowEventKind::Count)> lastDistance{};
		uint32_t droppedWindow = 0;
		uint32_t droppedPerSecond = 0;
		uint32_t logLines = 0;
		double windowStart = -1.0;
	};
	SnowEventCensus snowEventCensus;

	// ---- Snow coat aging inputs (MD plans/SSPP-SNOW-PLAN.md stage A0) ----
	// The snow terms a coating update pass would read for one character, as a
	// readout. Rates are 0-1 shares of a full rate; stage A1 sets their speed
	// against the coat's own units. Implemented in SnowDeformation/Accumulation.cpp.

	struct CoatAging
	{
		/** @brief The melt-bowl profile of the governing heat source at the character, 0-1. */
		float heat = 0.0f;
		float heatDistance = -1.0f;
		float heatRadius = 0.0f;
		bool interior = false;
		/** @brief Interiors only: snow-projected objects were drawn here last frame, so the place is cold. */
		bool interiorSnow = false;
		float rain = 0.0f;
		float snowfall = 0.0f;
		float temperature = 0.0f;
		/** @brief The ageing clock's warm term, from the temperature. */
		float warm = 0.0f;
		float wetness = 0.0f;
		float stickiness = 0.0f;
		/** @brief Smoothed ground speed from the impact track, units per second. */
		float speed = 0.0f;
		float melt = 0.0f;
		float shake = 0.0f;
		float growth = 0.0f;
	};
	CoatAging ComputeCoatAging(RE::Actor* a_actor) const;
	std::vector<std::string> CoatAgingLines() const;

public:
	// ---- Snow on bodies (MD plans/BODY-SNOW-PLAN.md) ----
	// R0's probes and S1's store and mapping. S1 draws no coat yet: Body Coat
	// View (Debugging Options) paints each mapped skin's canonical frame, or
	// its slot's pattern read back from the atlas, so the frames can be judged
	// before anything writes snow. Runtime-only switches. Implemented in
	// SnowDeformation/BodyCoat.cpp.

	/** @brief One actor as the coat sees it. Lengths in game units. Frames through the bones (BODY-SNOW-PLAN.md sections 5.2 and 5.4). */
	struct BodyCoatActorCensus
	{
		std::string name;
		uint32_t formID = 0;
		float distance = 0.0f;
		bool corpse = false;
		bool weaponDrawn = false;
		uint32_t skinned = 0;
		uint32_t dynamic = 0;
		uint32_t noNormals = 0;
		uint32_t skinShapes = 0;
		/** @brief Left out: eye shapes, refraction FX and engine decals (Decal, Permanent Decal, SkinnedDecal). */
		uint32_t eyeShapes = 0;
		uint32_t fx = 0;
		uint32_t decals = 0;
		/** @brief The body frame: the skin whose own space it is, that skin's root-most bone, and the bones it holds. Sticky: kept from the coat store rather than made by this census. */
		std::string seed;
		std::string anchor;
		uint32_t bindBones = 0;
		bool sticky = false;
		/** @brief Skins in the body frame, and frames of their own (skins that share no bone with the body: bows, tails). */
		uint32_t chained = 0;
		uint32_t components = 0;
		uint32_t componentSkins = 0;
		/** @brief Over mapped skins: the worst distance between a bone and where the skin's chosen map puts it, among the bones that agree. */
		float worstResidual = 0.0f;
		std::string worstShape;
		/** @brief Bones a skin lists that disagree with its chosen map by more than a voxel (listed but unweighted, or a different bind pose). */
		uint32_t outlierBones = 0;
		/** @brief The largest rootParentToSkin translation: how far the raw vertex frames sit apart (the engine never reads it). */
		float rawOffsetMax = 0.0f;
		uint32_t triangles = 0;
		/** @brief Rigid meshes: under an equipped weapon, shield or quiver slot; under another worn slot; sheaths (Scb); under no equipped slot. */
		uint32_t gear = 0;
		uint32_t gearSkinned = 0;
		uint32_t rigidWorn = 0;
		uint32_t sheaths = 0;
		uint32_t attached = 0;
		uint32_t gearTriangles = 0;
		float gearRadiusMax = 0.0f;
		/** @brief Lit geometries whose GetUserData() is this actor, and those whose is not. */
		uint32_t owned = 0;
		uint32_t unowned = 0;
		/** @brief The body frame's bounds: its skins' bone spheres, fixed when its slot was made. */
		RE::NiPoint3 boundsMin{ 1e30f, 1e30f, 1e30f };
		RE::NiPoint3 boundsMax{ -1e30f, -1e30f, -1e30f };
		/** @brief Skins whose bone spheres reach more than a voxel past their frame's fixed bounds. */
		uint32_t outside = 0;
		/** @brief The body's atlas slot (-1 none), component half slots held, and draw records. */
		int slot = -1;
		uint32_t halves = 0;
		uint32_t records = 0;
		/** @brief The player's first-person meshes: skins, those mapped through the third-person bind, their worst residual, owners. */
		uint32_t firstSkinned = 0;
		uint32_t firstChained = 0;
		float firstResidual = 0.0f;
		uint32_t firstOwned = 0;
		uint32_t firstUnowned = 0;
	};
	/** @brief One mapped skin's block at Lighting PS b8, bound per draw by offset. Mirror: SnowLighting.hlsli BodyCoatRecord. */
	struct alignas(16) BodyCoatRecordCB
	{
		/** @brief Rows of the skin's own space into its frame: bind(b) x skinToBone(b). */
		float4 ModelToCanonical[3];
		/** @brief Rows of the skin's own space into atlas texels: the frame's axes, scaled over its slot. */
		float4 ModelToAtlas[3];
		/** @brief The slot's texel box: lo inclusive, hi exclusive. */
		float4 AtlasLo;
		float4 AtlasHi;
		/** @brief The debug view (debugBodyCoatView, 0 the look), the slot, the frame (0 the body, 1 and up its components), and kBodyCoatFlag bits. */
		uint View;
		uint Slot;
		uint Component;
		uint Flags;
		/** @brief The shell's glint, as Lighting's glintParameters: screen-space scale, 40 - log density, microfacet roughness, density randomization. */
		float4 Glint;
		/** @brief The look's cranks: the cosines of the slope where snowfall starts sliding off, where it is gone, and where its grains end; the graininess, 0 to 1. */
		float4 Look;
	};
	STATIC_ASSERT_ALIGNAS_16(BodyCoatRecordCB);
	static constexpr uint32_t kBodyCoatFlagFlatPaint = 1;
	static constexpr uint32_t kBodyCoatFlagLiving = 2;
	/** @brief S1's store: per-actor frames, slots, records and the atlas. Complete in BodyCoat.cpp. */
	struct BodyCoatStore;
	/** @brief Lighting draws over one probe window: their owners, and the owned draws the probe bind did not reach. */
	struct BodyCoatDrawTally
	{
		uint64_t frames = 0;
		uint64_t lightingDraws = 0;
		uint64_t skinnedDraws = 0;
		uint64_t skinnedOwned = 0;
		uint64_t skinnedNoOwner = 0;
		uint64_t skinnedOtherOwner = 0;
		uint64_t rigidOwned = 0;
		uint64_t ownedDeferred = 0;
		uint64_t ownedForward = 0;
		uint64_t ownedReflection = 0;
		uint64_t lostDeferred = 0;
		uint64_t lostForward = 0;
		uint64_t lostReflection = 0;
		uint64_t framesWithLoss = 0;
	};
	/** @brief Per frame, from PrepassSimulation: the census on its cadence, the probe unbound once it is switched off, and the coat store (near tier, frames, slots, records). */
	void TickBodyCoat();
	/** @brief Per Lighting pass, before the game's SetupGeometry: bit 25 and the record of a mapped skin's draw, the atlas once a frame. */
	void SetBodyCoatBit(RE::BSRenderPass* a_pass);
	/** @brief Per Lighting technique, from TruePBR's GetPixelTechnique: whether its pixel key takes SnowBodyCoat (skinned with tangent-space normals, not eyes, the coat live). The pixel key keeps Skinned only with model-space normals, so armour, clothing and hair otherwise share a static object's pixel shader. */
	[[nodiscard]] bool BodyCoatPixelKey(uint32_t a_rawTechnique) const;
	/** @brief Per Lighting pass, after the game's SetupGeometry: owners, and whether the t41 bind still holds. */
	void ProbeBodyCoatDraw(RE::BSRenderPass* a_pass);
	std::vector<std::string> BodyCoatProbeLines() const;
	/** @brief The engine's own decal geometries, by the names it gives them: blood and impact marks, skinned or not. Implemented in SnowDeformation/Statics.cpp. */
	static bool IsEngineDecalName(const char* a_name);
	/** @brief Runtime-only: run the census every kBodyCoatCensusFrames. */
	bool debugBodyCoatCensus = false;
	/** @brief Runtime-only: the next census writes every shape to CommunityShaders.log. */
	bool debugBodyCoatCensusDump = false;
	/** @brief Runtime-only: bind a probe texture once a frame and count the actor draws it reaches. */
	bool debugBodyCoatDrawProbe = false;
	/** @brief Runtime-only: 0 off (the store is empty and nothing binds), 1 bands in each skin's canonical frame, 2 each slot's pattern read from the atlas, 3 each skin's slot and frame. */
	int debugBodyCoatView = 0;
	/** @brief Runtime-only, S2: a synthetic coat in every slot until a writer exists: 0 off, 1 legs to the knee, 2 snowfall from above, 3 the left side, 4 everything. */
	int debugBodyCoatForce = 0;
	/** @brief Runtime-only, S2: the forced coat's strength, 0 to 1. */
	float debugBodyCoatAmount = 1.0f;
	/** @brief Runtime-only, S2's A/B: 0 the crust (opacity from thickness and clumps), 1 flat paint (opacity = coverage). */
	int debugBodyCoatLook = 0;
	/** @brief Runtime-only, S2: the slope in degrees where snowfall starts sliding off; it is gone 20 degrees steeper. */
	float debugBodyCoatRepose = 50.0f;
	/** @brief Runtime-only, S2: how far snow grains spread past the clumps and the slope, 0 to 1. */
	float debugBodyCoatGrain = 0.5f;
	static constexpr uint32_t kBodyCoatCensusFrames = 120;
	static constexpr uint32_t kBodyCoatDrawWindow = 300;
	/** @brief The atlas's slot, and the probe's: in no Lighting permutation, read by no pixel shader (the volume's VoxelBricks is a vertex-stage input), bound at the pixel stage by nothing else. */
	static constexpr UINT kBodyCoatAtlasSlot = 41;
	/** @brief The record's slot: free in every Lighting PS (b0-b7 and b9-b12 are taken). */
	static constexpr UINT kBodyCoatRecordSlot = 8;
	/** @brief 16 constants, the D3D11.1 offset granularity. */
	static constexpr uint32_t kBodyCoatRecordStride = 256;
	static constexpr uint32_t kBodyCoatRecordsMax = 1024;
	/** @brief The atlas: kBodyCoatSlots slots of kBodyCoatSlotDim voxels (across, front-back, up) stacked in z. A body takes a slot; a component takes half of one, 16 across. */
	static constexpr uint32_t kBodyCoatSlots = 32;
	static constexpr uint32_t kBodyCoatSlotDim[3] = { 32, 16, 48 };
	/** @brief A coat voxel's planned size: 32 x 16 x 48 over a human's ~80 x 50 x 150 units. */
	static constexpr float kBodyCoatVoxel = 2.5f;

protected:
	void RunBodyCoatCensus(bool a_dump);
	void TickBodyCoatStore();
	std::shared_ptr<BodyCoatStore> bodyCoatStore;
	/** @brief The store holds records this frame: skinned Lighting techniques take the coat's pixel key. */
	bool bodyCoatLive = false;
	std::vector<BodyCoatActorCensus> bodyCoatCensus;
	uint32_t bodyCoatCensusFrame = 0;
	bool bodyCoatCensusRan = false;
	double bodyCoatCensusMs = 0.0;
	BodyCoatDrawTally bodyCoatDrawBuilding;
	BodyCoatDrawTally bodyCoatDrawLast;
	uint32_t bodyCoatDrawFrame = UINT32_MAX;
	bool bodyCoatDrawLostThisFrame = false;
	winrt::com_ptr<ID3D11Texture3D> bodyCoatProbeTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> bodyCoatProbeSRV;
	bool bodyCoatProbeBound = false;

	// ---- Load trace: what a save load spends its time on ----
	// Begins at the first co-save read, aggregates instrumented phases and
	// render-frame gaps, logs every event >= 100 ms live, and writes
	// SnowDeformation-LoadTrace.txt to the SKSE log folder once the load
	// path has been quiet for 20 s. Implemented in SnowDeformation/LoadTrace.cpp.

	/** @brief Starts a trace if none is running; a second call while active only marks the timeline. */
	void LoadTraceBegin(const char* a_reason);
	/** @brief Timeline event with a timestamp relative to the trace start. No-op while no trace runs. */
	void LoadTraceMark(std::string_view a_what);
	/** @brief Aggregates one timed run of a named phase; runs >= 100 ms also land on the timeline. */
	void LoadTraceRecord(const char* a_name, double a_ms);
	/** @brief Once per rendered frame (top of Prepass): detects frame gaps and decides when the trace ends. */
	void LoadTraceFramePulse();

	/** @brief RAII phase timer. Costs one relaxed atomic load when no trace is active. */
	struct LoadTraceScope
	{
		LoadTraceScope(SnowDeformation* a_owner, const char* a_name) :
			name(a_name)
		{
			owner = a_owner->loadTraceActive.load(std::memory_order_acquire) ? a_owner : nullptr;
			if (owner)
				start = std::chrono::steady_clock::now();
		}
		~LoadTraceScope()
		{
			if (owner)
				owner->LoadTraceRecord(name, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		}
		LoadTraceScope(const LoadTraceScope&) = delete;
		LoadTraceScope& operator=(const LoadTraceScope&) = delete;

		SnowDeformation* owner;
		const char* name;
		std::chrono::steady_clock::time_point start{};
	};

	struct LoadTracePhase
	{
		uint32_t count = 0;
		double totalMs = 0.0;
		double maxMs = 0.0;
	};
	std::atomic<bool> loadTraceActive = false;
	std::mutex loadTraceMutex;
	std::chrono::steady_clock::time_point loadTraceStart;
	/** @brief Last event worth reporting; 20 quiet seconds after it, the report writes. */
	std::chrono::steady_clock::time_point loadTraceLastNotable;
	std::chrono::steady_clock::time_point loadTraceLastFrame;
	bool loadTraceFrameSeen = false;
	std::map<std::string, LoadTracePhase, std::less<>> loadTracePhases;
	std::vector<std::string> loadTraceTimeline;
	uint32_t loadTraceFrames = 0;
	uint32_t loadTraceStallFrames = 0;
	double loadTraceWorstFrameMs = 0.0;
	/** @brief Writes the report file and ends the trace. Caller holds loadTraceMutex. */
	void LoadTraceReportLocked(const char* a_reason);

public:
	// ---- Shader prime + blob disk cache ----
	// Runtime D3DCompile of the snow shaders measured ~150 s on the render
	// thread at first draw (LoadTrace 2026-08-29). All variants now compile on
	// a worker thread started at the main menu (DataLoaded); the draw entry
	// points skip while it runs, so a load that beats the prime shows snow a
	// few seconds late instead of freezing. Compiled bytecode lands in a blob
	// disk cache keyed on a content fingerprint of every .hlsl/.hlsli under
	// Data\Shaders plus the exact defines/flags, so an unchanged launch loads
	// blobs in milliseconds and any source or define change recompiles.
	// Implemented in SnowDeformation/ShaderPrime.cpp.

	virtual void DataLoaded() override;
	~SnowDeformation();
	/** @brief Worker body: every non-debug shader getter once, in visibility order. */
	void RunShaderPrime();
	/** @brief 0 = never started (getters compile lazily, pre-prime behaviour); 1 = running; 2 = done. While running, member ownership is split by snowPrimePhase. */
	std::atomic<int> snowPrimeState = 0;
	/** @brief Groups the worker has PUBLISHED (release; gates read acquire): 1 = landscape shell + every compute, 2 = object snow, 3 = effects. The worker never touches a published group's members again; the render thread never touches an unpublished group's. */
	std::atomic<int> snowPrimePhase = 0;
	/** @brief True while the prime still owns this group's shader members - the render path must skip. */
	bool SnowShadersPending(int a_group) const
	{
		return snowPrimeState.load(std::memory_order_acquire) == 1 &&
		       snowPrimePhase.load(std::memory_order_acquire) < a_group;
	}
	std::thread snowPrimeThread;

	/** @brief Util::CompileShader with the blob disk cache in front. Same contract; every snow shader compiles through this. */
	ID3D11DeviceChild* CompileSnowShader(const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines, const char* a_target, const char* a_entry = "main");
	/** @brief Content hash over every shader source under Data\Shaders; cached until ClearShaderCache drops it. */
	std::string ShaderSourcesFingerprint();
	/** @brief Loads a cached blob whose stored key matches a_key exactly; null on any mismatch. */
	winrt::com_ptr<ID3DBlob> ShaderCacheLoad(const std::string& a_key, const std::string& a_file);
	void ShaderCacheStore(const std::string& a_key, const std::string& a_file, ID3DBlob* a_blob);
	static uint64_t ShaderKeyHash(std::string_view a_text);
	/** @brief Compiles the three SmoothNormalsCS variants; shared by the primer and EnsureSmoothedNormals. Implemented in SnowDeformation/Statics.cpp. */
	bool EnsureSmoothNormalsCS();
	std::mutex snowShaderCacheMutex;
	std::string snowSourcesFingerprint;
	std::atomic<uint32_t> snowShaderCacheHits = 0;
	std::atomic<uint32_t> snowShaderCacheMisses = 0;

protected:

	/**
	 * @brief Guards the tile store. SKSE's save, load and revert callbacks arrive
	 * on the GAME thread while the flush, sweep and inject run on the render
	 * thread, so every entry point from either side takes this. The *Locked
	 * helpers assume it is already held.
	 */
	std::mutex trenchStoreMutex;

	/** @brief Claims the co-save record. Called from PostPostLoad, before the main menu. */
	void RegisterTrenchCoSave();
	void SaveTrenchStore(const SKSE::SerializationInterface* a_intfc);
	void LoadTrenchStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);

	/** @brief Packs one tile's bytes (depth or fresh) under the pinned RLE rule. Their lengths together MUST equal the tile's measured encodedBytes, or Stage D's budget stops describing the save. */
	void EncodeTrenchTile(const std::vector<uint8_t>& a_bytes, std::vector<uint8_t>& o_bytes) const;
	/** @brief Unpacks a payload written by EncodeTrenchTile; false if it is malformed or the wrong length. */
	bool DecodeTrenchTile(const uint8_t* a_bytes, uint32_t a_length, std::vector<uint8_t>& o_depth) const;

	/** @brief Drops every stored tile and the cache that points into it. Takes the lock. */
	void ClearTrenchStore(const char* a_reason);
	/** @brief As ClearTrenchStore, for callers already holding the lock. */
	void ClearTrenchStoreLocked(const char* a_reason);
	/** @brief Advances the decay clock off game time, and drops the store on a backwards jump. */
	void TickTrenchClock();
	/** @brief Brings one tile's bytes up to the current clocks, its freshness too. Returns false when no depth is left, i.e. the tile should be erased. */
	bool DecayTrenchTile(TrenchTile& a_tile);
	/** @brief Decays and prunes a slice of the store, and gathers the occupancy and encoded-size figures. Amortised: correctness never depends on it, only reclaimed memory does. */
	void SweepTrenchStore();
	/** @brief Evicts least-recently-touched tiles until the store fits the memory budget. Runs at sweep-cycle boundaries, when every tile's encoded size has just been measured. */
	void EnforceTrenchBudget();

	/** @brief Debug readout: live tiles, raw bytes, encoded (what a save would cost), mean occupancy 0-1, and how many tiles are under a twentieth full. */
	struct TrenchStoreStats
	{
		size_t tiles;
		size_t bytes;
		size_t encoded;
		float occupancy;
		size_t thin;
	};
	/** @brief Reads only figures the sweep publishes, never the container, so the menu needs no lock against the game thread's save and load callbacks. */
	TrenchStoreStats GetTrenchStoreStats() const
	{
		const size_t bytes = trenchStatTiles * (size_t)kTrenchTileDim * kTrenchTileDim;
		const size_t swept = trenchStatSweptTiles * (size_t)kTrenchTileDim * kTrenchTileDim;
		return { trenchStatTiles, bytes, trenchEncodedTotal, swept ? (float)trenchStatNonZero / (float)swept : 0.0f, trenchStatThin };
	}

	// ---- Snow Trenches on Objects: the print map (ObjectPrints.cpp, MD plans\OBJECT-TOP-TRACKS-PLAN.md) ----

	/** @brief World units per print texel: the contact field's own. Mirrors PRINT_TEXEL in DeformationUpdateCS.hlsl. */
	static constexpr float kPrintTexel = 3.0f;
	/** @brief Print texels per store tile edge, on a world-anchored grid; a tile is one contiguous block of the map at any of its dims. */
	static constexpr int kPrintTileDim = 32;
	static constexpr int kPrintTileTexels = kPrintTileDim * kPrintTileDim;
	/** @brief Tiles the store keeps at most (8 KB each in memory); past it the furthest go first. */
	static constexpr size_t kPrintStoreCap = 2048;
	/** @brief Stamp and refill dispatch capacity, in 8x8 blocks. */
	static constexpr uint32_t kPrintBlockCap = 16384;
	/** @brief Tile mirror (map to store): staging slots, and dirty tiles queued per frame beyond the ones about to leave the window. */
	static constexpr uint32_t kPrintStagingSlots = 16;
	static constexpr uint32_t kPrintMirrorPerFrame = 4;
	static constexpr uint32_t kPrintRecord = 'SNOP';
	static constexpr uint32_t kPrintRecordVersion = 1;
	/** @brief PS slots of the print readers (SnowPrints.hlsli): t114 the print map, t115 its tile mask, t116 the rim (the drapes' twin). */
	static constexpr UINT kPrintReaderSlot = 114;
	/**
	 * @brief PS slots of Lighting's snow set and water raster (SnowDeformation.hlsli).
	 * Albedo and water stay bound across draws (LOD recolor, every object's underwater
	 * test), so no other code binds their slots; normal and RMAOS are bound per draw.
	 */
	static constexpr UINT kHorizonSnowAlbedoSlot = 33;
	static constexpr UINT kHorizonSnowNormalSlot = 23;
	static constexpr UINT kWaterWindowSlot = 63;
	static constexpr UINT kHorizonSnowRmaosSlot = 68;
	/** @brief Print texels a rim reaches past its print at most. Mirrors PRINT_RIM_APRON and kRimApron. */
	static constexpr int kPrintRimApron = 5;

	/** @brief Per-frame constants of the print passes (b1). Layout must match PrintFrame in DeformationUpdateCS.hlsl. */
	struct alignas(16) PrintFrameCB
	{
		/** @brief World print texel at the window's corner. */
		DirectX::XMINT2 WindowMin;
		uint RefillQuanta;
		uint RingRectCount;
		/** @brief Arriving texels in world print-texel space (x0, y0, w, h). */
		DirectX::XMINT4 RingRects[2];
		uint RingTotal;
		/** @brief Object Print Rim, units. */
		float RimWidth;
		/** @brief Marks in printSpellBuffer this frame. */
		uint SpellCount;
		uint Pad1;
	};
	STATIC_ASSERT_ALIGNAS_16(PrintFrameCB);
	static_assert(sizeof(PrintFrameCB) == 64, "PrintFrameCB must match the fxc-reflected PrintFrame");

	/** @brief One spell's mark on object snow this frame. A surface mark lies at a.z: a disc at a, a capsule from b to a, or with kPrintSpellCone a wedge from its apex b to a, radius its half-width there. With kPrintSpellRay it is a ray from a to b that PrintRayCS follows to the first object top it enters. Mirrors PrintSpells in DeformationUpdateCS.hlsl. */
	struct PrintSpellMark
	{
		RE::NiPoint3 a;
		RE::NiPoint3 b;
		float radius = 0.0f;
		/** @brief The depth it wants, 0-1 of the snow, as the ground stamp's. */
		float depth = 0.0f;
		/** @brief Depth per second it approaches that at; 0 = at once. */
		float rate = 0.0f;
		uint32_t flags = 0;
		/** @brief A cone's final reach from its apex, and the ground mark's churn seed. */
		float reach = 0.0f;
		float seed = 0.0f;
	};
	static constexpr uint32_t kPrintSpellCone = 1u;
	static constexpr uint32_t kPrintSpellBowl = 2u;
	static constexpr uint32_t kPrintSpellRay = 4u;
	/** @brief A cone that shoves: the churn thins it as on the ground. */
	static constexpr uint32_t kPrintSpellChurn = 8u;
	/** @brief Spell marks the print stamp takes a frame; the rest wait for none, as a mark is re-sent while its source lasts. */
	static constexpr uint32_t kPrintSpellCap = 32;

	/** @brief One tile of the map known to hold prints or to have held them since its last write: the refill dispatch domain. */
	struct PrintLiveTile
	{
		/** @brief Refill applied since the tile's last write; a full depth of it leaves nothing. */
		float refillSince = 0.0f;
		/** @brief Written since its last mirror to the store. */
		bool dirty = false;
	};
	/** @brief A stored tile: both slices' texels (slice-major, row-major), and the decay clock reading they are current to. */
	struct PrintStoreTile
	{
		std::vector<uint32_t> texels;
		float clock = 0.0f;
	};
	struct PrintStagingSlot
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		TrenchTileKey key{};
		/** @brief The window the copied texels belong to; texels outside it hold other ground. */
		DirectX::XMINT2 windowMin{};
		uint32_t dim = 0;
		float clock = 0.0f;
		uint64_t frame = 0;
		/** @brief printStoreGeneration when queued; a copy from another timeline is never folded. */
		uint32_t generation = 0;
		bool busy = false;
	};

	Texture2D* printMap = nullptr;
	/** @brief The rim around each print (PrintRimCS): one R32_UINT texel per print texel, packed and addressed as the map, rebuilt from it. */
	Texture2D* printRim = nullptr;
	ConstantBuffer* printFrameCB = nullptr;
	winrt::com_ptr<ID3D11Buffer> printBlockBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> printBlockSRV;
	ID3D11ComputeShader* printStampCS = nullptr;
	ID3D11ComputeShader* printRingCS = nullptr;
	ID3D11ComputeShader* printRefillCS = nullptr;
	ID3D11ComputeShader* printViewCS = nullptr;
	ID3D11ComputeShader* printRimCS = nullptr;
	ID3D11ComputeShader* printRayCS = nullptr;
	/** @brief This frame's spell marks on object snow, from the producers in Spells.cpp; cleared where spellEmitters is. */
	std::vector<PrintSpellMark> printSpellMarks;
	/** @brief A one-shot mark sent again every frame for as long as its ground mark lasts, so a frame whose stamp is skipped cannot lose it; a cone's front runs out at speed. */
	struct HeldPrintSpell
	{
		PrintSpellMark mark;
		float age = 0.0f;
		float life = 0.0f;
		float speed = 0.0f;
	};
	std::vector<HeldPrintSpell> heldPrintSpells;
	winrt::com_ptr<ID3D11Buffer> printSpellBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> printSpellSRV;
	winrt::com_ptr<ID3D11Buffer> printRayHitBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> printRayHitSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> printRayHitUAV;
	uint32_t printSpellsLast = 0;
	bool printRayFailed = false;
	bool printCSFailed = false;
	/** @brief The rim kernel failed to compile: prints go on without a rim. */
	bool printRimFailed = false;
	/** @brief The width the rim was last built at; a change rebuilds it over every live tile. */
	float printRimWidthLast = -1.0f;
	uint32_t printMapDim = 0;
	DirectX::XMINT2 printWindowMin = { 0, 0 };
	bool printWindowValid = false;
	uint32_t printWorldspace = 0;
	/** @brief Refill not yet applied, in depth units: the map takes it in 1023rds. */
	float printRefillBank = 0.0f;
	/** @brief Set by the co-save load and revert (game thread): the next update clears the map and refills it from the store. */
	std::atomic<bool> printReinjectRequested{ false };
	std::unordered_map<uint64_t, PrintLiveTile> printLive;
	/** @brief Guards printStore and printClockSnapshot: the co-save callbacks run on the game thread. */
	std::mutex printStoreMutex;
	std::unordered_map<TrenchTileKey, PrintStoreTile, TrenchTileKeyHash> printStore;
	/** @brief trenchDecayClock as of this frame's update, for the co-save writer. */
	float printClockSnapshot = 0.0f;
	/** @brief Bumped under printStoreMutex by every co-save load and revert. */
	std::atomic<uint32_t> printStoreGeneration{ 0 };
	/** @brief The generation an update started under; its copies carry it. Render thread. */
	uint32_t printQueueGeneration = 0;
	std::array<PrintStagingSlot, kPrintStagingSlots> printStaging;
	/** @brief One uint per window tile, set where the stamp changed a texel (PrintWrites, u1); read back to mark tiles live and dirty from what was written rather than from where bodies stood. */
	winrt::com_ptr<ID3D11Buffer> printWriteBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> printWriteUAV;
	struct PrintWriteReadback
	{
		winrt::com_ptr<ID3D11Buffer> staging;
		/** @brief World tile of the flags' first entry, and entries per row. */
		DirectX::XMINT2 tileOrigin{};
		uint32_t rowTiles = 0;
		uint32_t worldspace = 0;
		uint64_t frame = 0;
		bool busy = false;
	};
	static constexpr uint32_t kPrintWriteSlots = 4;
	std::array<PrintWriteReadback, kPrintWriteSlots> printWriteReadbacks;
	/** @brief Frame of the last full clear: write flags read back from before it describe a map that no longer exists. */
	uint64_t printLastClearFrame = 0;
	std::vector<uint32_t> printBlockScratch;
	std::vector<uint8_t> printBlockSeen;
	uint64_t printFrame = 0;
	/** @brief Debug readout: last update's stamp and refill blocks, tiles mirrored and injected, store size. */
	uint32_t printStampBlocksLast = 0;
	uint32_t printRefillBlocksLast = 0;
	uint32_t printMirrorsLast = 0;
	uint32_t printInjectsLast = 0;
	size_t printStoreTilesLast = 0;
	/** @brief Runtime-only: the field view shows the print verdicts and the print map (PrintViewCS) instead of the trench pair. */
	bool debugObjectPrintView = false;
	/** @brief The readers' tile mask (PS t115): one R8 texel per print tile on the map's world-anchored grid, nonzero where the tile or the next one along x, y or both holds prints, so a reader's texel block takes one load. */
	Texture2D* printTiles = nullptr;
	/** @brief The mask as last built and as last uploaded; it uploads only when it changes. */
	std::vector<uint8_t> printTilesScratch;
	std::vector<uint8_t> printTilesUploaded;
	/** @brief Summed-area table of the live tiles over the window's tile grid, for the drapes' twin choice: printTileSumDim tiles a side from printTileSumOrigin. */
	std::vector<uint32_t> printTileSum;
	DirectX::XMINT2 printTileSumOrigin = { 0, 0 };
	int32_t printTileSumDim = 0;
	/** @brief The window and dim this frame's update moves the map to, decided with the trench window in GetCommonBufferData so the readers' rect agrees with the map. */
	DirectX::XMINT2 printWindowTarget = { 0, 0 };
	uint32_t printDimTarget = 0;
	/** @brief This frame's SettingsGPU::PrintRect. */
	DirectX::XMINT4 printRectPublished = { 0, 0, 0, 0 };
	/** @brief Debug readout: drape draws on the print twin last frame. */
	uint32_t printTwinDrawsLast = 0;
	/** @brief Debug readout, Snow Follows Parallax last frame: captures that carried a height map, and drapes drawn through the parallax coat. */
	uint32_t parallaxCapturesLast = 0;
	uint32_t parallaxCoatDrawsLast = 0;

	/** @brief The print map's frame: window, mirror, ring, inject, refill, stamp, view. Runs after the trench dispatch; a_trenchPassRan says whether PerFrame (b0) holds this frame's values. */
	void UpdateObjectPrints(ID3D11DeviceContext* a_context, const PerFrame& a_frame, bool a_trenchPassRan);
	void CreatePrintResources(uint32_t a_dim);
	bool EnsurePrintShaders();
	ID3D11ComputeShader* GetPrintStampCS();
	ID3D11ComputeShader* GetPrintRingCS();
	ID3D11ComputeShader* GetPrintRefillCS();
	ID3D11ComputeShader* GetPrintViewCS();
	ID3D11ComputeShader* GetPrintRimCS();
	ID3D11ComputeShader* GetPrintRayCS();
	void ReleaseObjectPrintShaders();
	/** @brief One spell mark on object snow this frame (PrintSpellMark), taken while there is room. a_hold > 0 sends it again every frame for that many seconds, a cone's front running out at a_speed; a_seed is a cone's churn seed. */
	void QueueObjectSpellMark(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_radius, float a_depth, float a_rate, uint32_t a_flags, float a_hold = 0.0f, float a_speed = 0.0f, float a_seed = 0.0f);
	/** @brief The held marks into this frame's printSpellMarks, aged by a_deltaTime; called where printSpellMarks is cleared. */
	void ReplayHeldObjectSpellMarks(float a_deltaTime);
	/** @brief A held mark as it stands at the end of this frame: a cone's front where it has run to. */
	PrintSpellMark HeldPrintFront(const HeldPrintSpell& a_held, float a_deltaTime) const;
	/** @brief Queues one tile's copy into a free staging slot; false when none is free. */
	bool QueuePrintMirror(ID3D11DeviceContext* a_context, int32_t a_tx, int32_t a_ty, float a_clock);
	/** @brief Folds finished copies into the store. */
	void PollPrintMirrors(ID3D11DeviceContext* a_context);
	/** @brief Marks the tiles finished write-flag readbacks name live and dirty. */
	void PollPrintWrites(ID3D11DeviceContext* a_context);
	/** @brief Uploads the stored texels of every tile inside a_rect (world print texels) that the store holds; returns tiles injected. */
	uint32_t InjectPrintRect(ID3D11DeviceContext* a_context, const DirectX::XMINT4& a_rect, float a_clock);
	/** @brief Brings a stored tile to a_clock; false when nothing is left in it. Caller holds printStoreMutex. */
	static bool DecayPrintTile(PrintStoreTile& a_tile, float a_clock);
	/** @brief Drops the furthest tiles past kPrintStoreCap. Caller holds printStoreMutex. */
	void EnforcePrintStoreCap();
	void RegisterPrintCoSave();
	void SavePrintStore(const SKSE::SerializationInterface* a_intfc);
	void LoadPrintStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);
	/** @brief Empties the store; the next update clears the map and re-injects nothing. Any thread. */
	void ClearPrintStore(const char* a_why);
	/** @brief Decides this frame's print window (printWindowTarget, printDimTarget) and returns the rect the readers may use: the texels both the map as it stands and the map after this frame's update hold. Called from GetCommonBufferData. */
	DirectX::XMINT4 PrintRectForFrame();
	/** @brief Rebuilds the tile mask and the live-tile sums from printLive; uploads the mask when it changed. */
	void PublishPrintTiles(ID3D11DeviceContext* a_context);
	/** @brief Whether a world bound reaches a tile holding prints: the drapes' twin choice. */
	bool PrintsUnder(const RE::NiBound& a_bound) const;
	/** @brief Binds the print map, its tile mask and the rim for the readers (PS t114-t116) while the published rect is live, else nulls them. */
	void BindPrintReaders(ID3D11DeviceContext* a_context) const;

public:
	/** @brief Applies pending range-setting changes (trench window resize + map clear). Called at Prepass start; the first call applies loaded settings. */
	void ApplyRangeSettings();

protected:
	/** @brief Trail history per collision shape: key = (formID << 16) | traversal index. */
	std::unordered_map<uint64_t, float2> stampPrevPositions;

	std::unordered_map<uint32_t, StampBones> stampBoneCache;

	/** @brief Which behaviour table a spell effect falls under, read off the effect's resist variable. No spell is ever named. */
	enum class SpellElement
	{
		None,
		Fire,
		Frost,
		Shock,
		/** @brief Not an element the resist variable knows: nothing resists being SHOVED. Force is recognised by a stagger archetype on a spell that names no element, and it is the only one that displaces snow without changing it. Appended rather than inserted - AuraFromSpellList maps slots 1-3 onto the three resist values by cast. */
		Force
	};

	/** @brief What an emitter does to the snow. Only Melt is wired up so far; the rest name the per-element behaviours in SPELL-INTEGRATION.md section 4. */
	enum class SpellMark
	{
		Melt,
		Carve,
		Pit,
		Crust
	};

	/**
	 * @brief One spell-driven mark, gathered fresh each frame.
	 *
	 * Several detectors fill this list and one consumer turns it into stamps.
	 * Positions are already ground-projected: the map is 2D, so an emitter
	 * carries where it marks the GROUND, not where the source floats.
	 */
	struct SpellEmitter
	{
		float2 position{};
		/** @brief Previous position, so a swept source marks a continuous band instead of a row of dots. */
		float2 previous{};
		float radius = 0.0f;
		/** @brief Target-depth multiplier, 0-1. Fades with the source's height above the ground it marks. */
		float strength = 0.0f;
		/** @brief Depth units per second the mark approaches its target at. */
		float rate = 0.0f;
		SpellElement element = SpellElement::None;
		SpellMark mark = SpellMark::Melt;
		/** @brief Pits only: multiplier on the discharge radius, so a big blast forks wider than a bolt. */
		float pitScale = 1.0f;
		/** @brief Crust only: multiplier on the glazing rate, carrying the source's magnitude and how far above the snow it sits. */
		float rateScale = 1.0f;
		/** @brief Carve a scooped hollow rather than a walled trench. A body hurled through snow leaves a furrow with sloped sides, not a slot. */
		bool bowl = false;
		/** @brief Read the pair of points as a WEDGE instead of a capsule: previous is the apex, position the far centre, radius the half-width there. Only shouts set it - a row of discs cannot stand in for a cone, because every stamp holds full depth across only the inner tenth of its radius and so reads as a row of craters. */
		bool cone = false;
		/** @brief Cone only: final reach from the apex, and the churn seed. */
		float coneLength = 0.0f;
		float coneSeed = 0.0f;
	};

	/** @brief This frame's emitters, rebuilt by GatherSpellEmitters and consumed by GatherStamps. */
	std::vector<SpellEmitter> spellEmitters;
	/** @brief The object-snow twin of a spell emitter (Snow Trenches on Objects), depth and reach as GatherStamps gives the ground's stamp: a melt at its strength and rate, a pit at Pit Depth over Pit Radius x a_pitScale unless a cone, a carve at Force Carve Depth. Crust and scorch leave no print. a_hold, a_speed and a_seed as QueueObjectSpellMark's. */
	void QueueObjectSpellMarkFor(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_radius, SpellMark a_mark, float a_strength, float a_rate, float a_pitScale, uint32_t a_flags, float a_hold = 0.0f, float a_speed = 0.0f, float a_seed = 0.0f);

	/** @brief Last XY per projectile (formID), for the capsule sweep. */
	std::unordered_map<uint32_t, float2> spellPrevPositions;

	/**
	 * @brief Last XY per projectile, for the corridor it cuts through the snow.
	 *
	 * Separate from spellPrevPositions, which tracks where a stream's cone
	 * LANDS rather than where its projectile is. A trail is about the
	 * projectile's own path.
	 */
	std::unordered_map<uint32_t, float2> spellTrailPrev;

	/** @brief Behaviour table for an effect, from its resist variable. Implemented in SnowDeformation/Spells.cpp. */
	static SpellElement ClassifyElement(const RE::EffectSetting* a_effect);

	/** @brief What an element does to snow, per SPELL-INTEGRATION.md section 4. Only Melt is wired up; the rest ride the emitter so the detectors need no revisiting when their marks land. */
	static SpellMark MarkForElement(SpellElement a_element);

	/**
	 * @brief Rebuilds spellEmitters from the live projectile list.
	 *
	 * Projectiles come from Projectile::Manager rather than a reference scan:
	 * it is global rather than cell-limited and costs one locked snapshot.
	 * Hazards and explosions are references and will piggyback the per-frame
	 * scan already running in GatherStamps instead of adding another.
	 */
	void GatherSpellEmitters();

	/** @brief Weight a stamped shape puts through a crust, from its radius. Implemented in SnowDeformation/Stamping.cpp. */
	float CrustBreakForce(float a_radius) const;

	/**
	 * @brief True when nothing this actor carries comes down to its footing.
	 *
	 * Measured, not listed: the atronach races are authored `Walks` and none
	 * sets `kFlies`, so a flag lookup finds nothing and a race table misses
	 * every modded levitator. The lowest thing an actor could carve with,
	 * against the ground it stands on, answers it directly.
	 *
	 * Reads cached bones where there are any, else the collision shapes.
	 * a_useFeet false skips the foot bones (limbs and collision still measure):
	 * feet an editor detached or zero-scaled must not decide the verdict.
	 * a_byFeetOut reports whether feet alone decided it.
	 */
	bool ActorIsFloating(RE::Actor* a_actor, RE::NiAVObject* a_root, const StampBones* a_bones, bool a_useFeet, float a_groundZ, float* a_gapOut = nullptr, bool* a_byFeetOut = nullptr) const;

	/**
	 * @brief True when this actor has no substance, so nothing it does should cut snow.
	 *
	 * A separate axis from floating: a ghost walks with its feet on the ground.
	 *
	 * Translucency routes through ExtendedTranslucency's own see-through tests
	 * rather than inventing a second definition, narrowed to SKINNED geometry
	 * with material alpha below full - plain alpha blending catches every NPC's
	 * hair and eyes. The record flag is a static read of the base, not the
	 * runtime call.
	 */
	bool ActorIsIncorporeal(RE::Actor* a_actor, RE::NiAVObject* a_root, StampBones* a_bones,
		float* a_alphaOut = nullptr, bool* a_flagOut = nullptr) const;
	/** @brief Skinned geometry that is not a body surface: an HDT-SMP collision proxy ('Virtual*', or a material faded to nothing) or a RaceMenu overlay layer. */
	static bool IsProxyOrOverlayGeometry(const RE::BSGeometry* a_geometry, const RE::BSShaderProperty* a_property);

	/**
	 * @brief Raises one buried effect onto the snow above it, on the GAME thread.
	 *
	 * Detection runs on the render thread with every other detector; the write
	 * must not. Mutating the game's scene graph from a render pass is the shape
	 * of thing that crashed this feature repeatedly when it was only reading,
	 * so the lift goes through the SKSE task interface.
	 */
	void LiftRefOntoSnow(RE::TESObjectREFR* a_ref, float a_lift, float a_minUpZ = -2.0f);
	struct PendingLift
	{
		RE::ObjectRefHandle handle;
		float lift = 0.0f;
		float minUpZ = -2.0f;
		/** @brief Frames waited for a 3D that was not there yet. */
		uint32_t waited = 0;
	};
	/** @brief Lifts asked for by the spell scan, applied by ApplySpellLifts. */
	std::mutex spellLiftLock;
	std::vector<PendingLift> spellLiftQueue;

	/**
	 * @brief How far to raise something resting at a_position so it sits a_fraction of the way up the snow over it, plus a_clearance.
	 *
	 * The one place a lift is measured (BURIED-REF-LIFT-PLAN.md section 4):
	 * frost pieces pass 0.5, rune effects 1 and a clearance, a dropped item
	 * its weight's share. Live snow - the shell's own surface where its
	 * window reaches, the class depth times accumulation elsewhere. 0 when
	 * there is nothing to lift out of: no snow, already above it, or standing
	 * off the terrain on a bridge or a roof.
	 */
	float SnowLiftFor(const RE::NiPoint3& a_position, float a_fraction, float a_clearance, RE::TES* a_tes);

	// Dropped items rest in the snow by weight and shape (ItemSink.cpp, WEIGHT-SINK-PLAN.md).
	static constexpr float kItemSinkRadius = 3000.0f;
	static constexpr size_t kItemSinkMax = 32;
	static constexpr uint32_t kItemSinkRecord = 'SNIS';
	static constexpr uint32_t kItemSinkRecordVersion = 3;
	struct ItemSinkCandidate
	{
		RE::ObjectRefHandle handle;
		RE::NiPoint3 position;
		uint32_t seenFrame = 0;
	};
	struct ItemSinkState
	{
		uint32_t formID = 0;
		uint32_t baseID = 0;
		uint32_t seenFrame = 0;
		/** @brief Identity only, never dereferenced: a rebuilt 3D took the offset with it. */
		RE::NiAVObject* offsetRoot = nullptr;
		bool offsetApplied = false;
		RE::NiPoint3 childOffset;
		float appliedLift = 0.0f;
		bool undersideValid = false;
		float undersideOffset = 0.0f;
		float lyingHeight = 0.0f;
		/** @brief Last frame's final transforms under the root; pointers are compared, never dereferenced. */
		std::vector<std::pair<RE::NiAVObject*, RE::NiTransform>> lastWorlds;
		uint32_t lastWorldFrame = 0;
		uint32_t restPasses = 0;
		/** @brief The snow the item settled in (surface height, depth, share dug away around it) and where that was. Snow buries: replaced only by a snow state that puts the item lower. */
		bool restValid = false;
		float restRise = 0.0f;
		float restDepth = 0.0f;
		float restCarve = 0.0f;
		RE::NiPoint3 restPos;
		/** @brief Trench store read on a ring around the item, refreshed every 8th frame. */
		float carveAround = 0.0f;
		/** @brief Under where today's snow would put it: it stops printing, or its own trench would keep the snow off it. */
		bool buried = false;
		bool wasAsleep = false;
		/** @brief Took its co-save record this session; past the load's grace it does not take it twice. */
		bool claimed = false;
		/** @brief The snow's grip as last written to the body's damping, and the damping it had before. */
		float grip = 0.0f;
		bool gripBaseKnown = false;
		float gripBaseLinear = 0.0f;
		float gripBaseAngular = 0.0f;
	};
	uint32_t itemSinkRestsLogged = 0;
	struct ItemSinkRecord
	{
		uint32_t baseID = 0;
		float x = 0.0f;
		float y = 0.0f;
		float rise = 0.0f;
		float depth = 0.0f;
		float carve = 0.0f;
	};
	uint32_t itemSinkFrame = 0;
	/** @brief Main thread only, by ref handle: dropped items are temporary references and their form IDs are recycled. */
	std::unordered_map<uint32_t, ItemSinkCandidate> itemSinkCandidates;
	std::atomic<bool> itemSinkCandidatesStale{ false };
	/** @brief Guards the states, the loaded records and the readout: the co-save callbacks arrive on the game thread. */
	std::mutex itemSinkLock;
	std::unordered_map<uint32_t, ItemSinkState> itemSinkStates;
	/** @brief Rest heights from the co-save, by form ID, until the item is seen and takes (or fails) its own. */
	std::unordered_map<uint32_t, ItemSinkRecord> itemSinkLoaded;
	/** @brief Records outlive their first claim this long after a load: a load can arrive in two passes, the second putting the items back. */
	static constexpr float kItemSinkLoadGrace = 60.0f;
	std::chrono::steady_clock::time_point itemSinkLoadedAt{};
	std::unordered_set<uint32_t> itemSinkFormsLogged;
	std::string itemSinkReadout;
	std::atomic<bool> itemSinkReadoutWanted{ false };
	std::atomic<uint32_t> itemSinkHeld{ 0 };
	std::atomic<uint32_t> itemSinkWatched{ 0 };
	uint32_t itemSinkClaimsLogged = 0;
	void ItemSinkNote(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position);
	/** @brief A held item at rest keeps printing its trench (the refill would close it), unless the snow has buried it. */
	bool ItemSinkWantsPrint(RE::TESObjectREFR* a_ref);
	void SaveItemSink(const SKSE::SerializationInterface* a_intfc);
	void LoadItemSink(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);
	void RegisterItemSinkCoSave();

	/** @brief Hazards already raised, by formID, so the lift happens once rather than every frame. */
	std::unordered_set<uint32_t> liftedRefs;

	// Creatures stand in the snow by weight (CreatureSink.cpp, TRACK-VARIATION-RESEARCH.md 3.5 and Part 4 Stage 3).
	/** @brief An actor's ragdoll capsules, measured on its live 3D: the weight and shape the sink is judged by. */
	struct CreatureBuild
	{
		/** @brief Identity only, never dereferenced. */
		const RE::NiAVObject* root = nullptr;
		float scale = 0.0f;
		/** @brief Capsule volume, units^3. */
		float volume = 0.0f;
		/** @brief Underside of the largest capsule over the lowest point, the highest seen standing. */
		float belly = 0.0f;
		/** @brief Top of the capsules over the lowest point. */
		float height = 0.0f;
		float largest = 0.0f;
		/** @brief Lowest capsule under the actor's position. */
		float lowestRel = 0.0f;
		uint32_t capsules = 0;
		/** @brief Capsules read from bodies Havok simulates; the rest from their bones. */
		uint32_t simulated = 0;
		uint32_t reads = 0;
		uint32_t nextFrame = 0;
	};
	struct CreatureLift
	{
		/** @brief Identity only. */
		const RE::NiAVObject* root = nullptr;
		/** @brief The parent of NPC Root [Root] under the root: no animation writes it. Re-validated against root every frame. */
		RE::NiAVObject* node = nullptr;
		CreatureBuild build;
		/** @brief Eased lift written this frame, and the one this frame's bones carry (last frame's). */
		float lift = 0.0f;
		float inBones = 0.0f;
		/** @brief The lift at death, settling to 0; below 0 while alive. */
		float settleFrom = -1.0f;
		float clearance = FLT_MAX;
		/** @brief Share of the layer a trench under it leaves standing. */
		float floorShare = 1.0f;
		/** @brief The ragdoll rose with the last lift the build read: Havok drives it to the drawn bones. */
		bool havokFollows = true;
		bool havokKnown = false;
		bool dead = false;
		uint32_t seenFrame = 0;
		float weight = 0.0f;
		/** @brief What its weight alone lifts it by, before the caps. */
		float byWeight = 0.0f;
		float depth = 0.0f;
		float density = 0.0f;
		float support = 0.0f;
		float target = 0.0f;
		const char* gate = "";
	};
	/** @brief Main thread only. */
	std::unordered_map<uint32_t, CreatureLift> creatureLifts;
	CreatureBuild playerBuild;
	struct CreatureLiftDrawn
	{
		float inBones;
		bool havokFollows;
		/** @brief Capsule volume over the player's, 0 = unmeasured. */
		float weight;
	};
	/** @brief This frame's lifts in the bones and the creatures' weights, by formID, for the stamp gather. Under creatureLiftLock. */
	std::unordered_map<uint32_t, CreatureLiftDrawn> creatureLiftsDrawn;
	std::mutex creatureLiftLock;
	std::unordered_set<uint32_t> creatureRacesLogged;
	std::string creatureSinkReadout;
	std::atomic<bool> creatureSinkReadoutWanted{ false };
	std::atomic<uint32_t> creatureSinkLifted{ 0 };
	std::atomic<uint32_t> creatureSinkWatched{ 0 };
	std::atomic<float> creatureSinkMicros{ 0.0f };
	/** @brief Measures a_build off a_root's capsules on its cadence; false when it has none. a_baseZ is the ground the actor stands on. */
	bool MeasureCreatureBuild(RE::NiAVObject* a_root, CreatureBuild& a_build, bool a_upright, uint32_t a_frame, float a_baseZ);
	/** @brief The lift an actor's bones carry this frame, 0 when none; a_havokFollows says whether its ragdoll carries it too, a_weight its weight against the player's (0 = unmeasured: people, or Creatures Sink by Weight off). */
	float CreatureLiftOf(uint32_t a_formID, bool* a_havokFollows = nullptr, float* a_weight = nullptr);

	/**
	 * @brief Adds a placed hazard (spell wall, rune) to this frame's emitters.
	 *
	 * Called from the reference scan GatherStamps already runs for props
	 * rather than from a scan of its own: a hazard is an ordinary reference,
	 * and that pass covers the right radius every frame. Explosions will join
	 * it the same way.
	 */
	void ConsiderHazard(RE::TESObjectREFR* a_ref);

	/**
	 * @brief What a projectile will leave behind when it dies, recorded while
	 * it is still alive.
	 *
	 * A projectile leaving Projectile::Manager has detonated, and its own effect
	 * already gave the element, so this needs neither the explosion reference
	 * nor an explosion-to-element table. Also the only route that works when
	 * spawned explosions never reach a cell's reference list.
	 */
	struct PendingBlast
	{
		/** @brief Last seen position, in full. A fast bolt dies between frames, so this sits short of where it actually struck. */
		RE::NiPoint3 position{};
		/** @brief Unit heading on that frame, used to find where the flight would have ended. */
		RE::NiPoint3 direction{};
		float heightAboveLand = 0.0f;
		float radius = 0.0f;
		/** @brief Pit sizing, taken from the AUTHORED blast radius before the fire-tuned blast scale touches it. */
		float pitScale = 1.0f;
		/** @brief How far past the last sighting a traced landing still counts as where this projectile struck. A hitscan bolt resolves at its muzzle, so its strike can be its whole range away; a travelling one moves only a frame's worth. */
		float landingReach = 0.0f;
		SpellElement element = SpellElement::None;
		/** @brief Proximity-triggered record (a rune): leaving the manager is only a blast if it was seen triggered. */
		bool proximity = false;
		bool triggered = false;
	};
	/** @brief Per live projectile (formID), rebuilt every frame. Holds only projectiles still IN FLIGHT - one that has already struck marks at once instead. */
	std::unordered_map<uint32_t, PendingBlast> projectileBlasts;

	/** @brief Projectiles that have already struck and already marked. A hitscan bolt lingers for as long as its beam is drawn, so it stays in the manager long after it hit; this stops it marking again every frame of that. Pruned as the projectiles die, since form ids are recycled. */
	std::unordered_set<uint32_t> hitscanBlasted;

	/** @brief Turns a recorded blast into a held-open mark. Shared by the projectile that struck on sight and the one whose death we only notice when it leaves the manager. */
	void OpenProjectileBlast(const PendingBlast& a_blast, RE::TES* a_tes);

	/**
	 * @brief A detonation's mark, held open for a fraction of a second.
	 *
	 * A blast is one instant, but a stamp only deepens while its emitter exists,
	 * so one frame at any sane rate marks nothing. Held for a moment instead, so
	 * the snow is seen to give way rather than the crater simply appearing.
	 */
	struct ActiveBlast
	{
		float2 position{};
		float radius = 0.0f;
		float strength = 0.0f;
		float rate = 0.0f;
		/** @brief Seconds left before the mark stops deepening. */
		float remaining = 0.0f;
		/** @brief Seconds since it opened. A pit follows CARVE, which is instantaneous, so without this a discharge simply EXISTS on the frame it lands; the age ramps its depth in over a fraction of a second instead. */
		float age = 0.0f;
		/** @brief Pits only: how much wider than a bolt this discharge forks. */
		float pitScale = 1.0f;
		/** @brief Log this blast's lifecycle. Set only by the atronach death path while its mark is under investigation: the counters prove the blast opens and the snow says nothing lands, and this is the instrument that says which frame is lying. */
		bool diag = false;
		SpellElement element = SpellElement::None;
		SpellMark mark = SpellMark::Melt;
		/** @brief Laid as a wedge from apex to position. Shouts only. */
		bool cone = false;
		/** @brief Cone only: the apex, which the emitter carries as its previous position. */
		float2 apex{};
		/** @brief Cone only: unit heading, so the front can be advanced along it each frame. */
		float2 coneDir{};
		/** @brief Cone only: how far the wedge reaches once fully out, from the apex. */
		float coneLength = 0.0f;
		/** @brief Cone only: units per second the front travels, off the shout's own projectile. A shout is a thing that GOES somewhere - it does not appear along its whole length at once. */
		float coneSpeed = 0.0f;
		/** @brief Cone only: offsets the churn pattern, so two shouts from one spot do not share streaks. */
		float coneSeed = 0.0f;
	};
	std::vector<ActiveBlast> activeBlasts;

	/**
	 * @brief Cast-event sink, for spells that leave nothing to follow.
	 *
	 * A self-centred area spell has no projectile, no hazard and no explosion
	 * reference: the blast simply happens on the caster. Every other detector
	 * watches an object; this one watches the act of casting.
	 */
	class SpellCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
			RE::BSTEventSource<RE::TESSpellCastEvent>* a_source) override;
	};
	SpellCastSink spellCastSink;
	bool spellCastSinkRegistered = false;

	/**
	 * @brief Death sink, because an atronach's death cannot be polled for.
	 *
	 * An atronach is unsummoned on death rather than left as a corpse, so by the
	 * time a per-frame sweep looks it is out of the high-process list, its 3D,
	 * or both. The engine reports the moment exactly.
	 *
	 * Reads the actor's POSITION and its RACE's spell list - static form data.
	 * Nothing here walks a live actor's effects.
	 */
	class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
			RE::BSTEventSource<RE::TESDeathEvent>* a_source) override;
	};
	DeathSink deathSink;

	/**
	 * @brief A shout, queued by the cast sink for the gather to lay down.
	 *
	 * Opened as one cone stamp whose front runs out from the mouth at the
	 * shout's own speed.
	 */
	struct QueuedCone
	{
		RE::NiPoint3 position{};
		/** @brief Unit heading, already flattened: a shockwave runs along the ground rather than following the crosshair into the sky. */
		RE::NiPoint3 direction{};
		float length = 0.0f;
		/** @brief Units per second the front travels, from the shout's own projectile record. */
		float speed = 0.0f;
		/** @brief 0-1, from the projectile's authored impact force against Unrelenting Force's own. */
		float strength = 1.0f;
		SpellElement element = SpellElement::None;
	};
	std::vector<QueuedCone> queuedCones;

	/** @brief Lays one shout down as a wedge along its axis. */
	void OpenShoutCone(const QueuedCone& a_cone, RE::TES* a_tes);

	/**
	 * @brief A shout that moves its own caster, watched rather than classified.
	 *
	 * The records cannot separate Whirlwind Sprint from Storm Call: both are
	 * self-delivered voice powers with a script effect and a projectile, and
	 * impact force is no better (Marked for Death throws the same 50-force push
	 * projectile as Unrelenting Force).
	 *
	 * So the records are not asked. A self-delivered shout opens a short watch
	 * on its caster, and what marks the snow is the caster moving faster than
	 * anything on foot can. Nothing is named or excluded.
	 */
	struct DashWatch
	{
		RE::ActorHandle actor;
		float remaining = 0.0f;
		float2 previous{};
		bool hasPrevious = false;
	};
	/** @brief Queued by the cast sink on the game thread, drained by the gather. */
	std::vector<DashWatch> queuedDashes;
	/** @brief Live dash watches. Short-lived, so a vector rather than a map. */
	std::vector<DashWatch> dashWatches;

	/** @brief Runs every live dash watch, cutting a furrow behind anything moving fast enough to be dashing. */
	void GatherDashGouges(float a_deltaTime, const RE::NiPoint3& a_cameraPosition, float a_cullRadius);

	/** @brief True when this spell shoves rather than merely staggering or damaging, and how hard its projectile hits. Shared by the shout cone and the travelling-track route so the two can never disagree about what force is. */
	static bool SpellShoves(const RE::MagicItem* a_spell, float& a_force);

	/**
	 * @brief True when this spell already lays a shout WEDGE, so its projectile must not mark as well.
	 *
	 * Asked of the spell record rather than remembered from the cast, because
	 * it is a property of the record: a shout either cones or it does not. That
	 * also means the two routes cannot fall out of step - the same classifier
	 * answers both.
	 */
	bool ShoutLaysCone(const RE::MagicItem* a_spell) const;

	/** @brief True when a shove TRAVELS rather than blasting - slow enough to watch, or hitting far harder than any shockwave is authored to. Either reading is enough; they are two ways of noticing the same thing. */
	static bool IsTravellingShove(const RE::BGSProjectile* a_projectile, float a_force);

	/** @brief Classifies a shout off its records and queues its cone. Called from the cast sink on the game thread; reads only static forms and the caster's own position and facing. */
	void ConsiderShout(const RE::SpellItem* a_spell, RE::TESObjectREFR* a_caster);

	/** @brief Queued by the death sink on the game thread, drained by the gather. */
	struct QueuedDeath
	{
		uint32_t formID = 0;
		RE::NiPoint3 position{};
		SpellElement element = SpellElement::None;
		/** @brief The dying actor, so a FUSED death can follow the body through its death animation. The position at event time is where it STOOD when the engine decided it was dead; a flame atronach then collapses and slides for seconds before bursting, and the mark belongs under the corpse, not under the decision. */
		RE::ActorHandle actor;
	};
	std::vector<QueuedDeath> queuedDeaths;

	/**
	 * @brief What last struck an actor, so a body knows what it is still burning with.
	 *
	 * A corpse still alight or frozen should go on marking, but what a corpse is
	 * doing is live-actor state this feature refuses to poll. The effect that
	 * hit it announced itself on the way in, and that record is static data.
	 */
	class MagicApplySink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event,
			RE::BSTEventSource<RE::TESMagicEffectApplyEvent>* a_source) override;
	};
	MagicApplySink magicApplySink;

	/** @brief Queued by the apply sink on the game thread, drained by the gather. */
	struct QueuedHit
	{
		uint32_t formID = 0;
		SpellElement element = SpellElement::None;
	};
	std::vector<QueuedHit> queuedHits;

	/** @brief The last element to strike each actor, and how long that memory has left. Short: what killed a body is whatever hit it moments before, not something from a fight two rooms back. */
	struct ElementalHit
	{
		SpellElement element = SpellElement::None;
		float remaining = 0.0f;
	};
	std::unordered_map<uint32_t, ElementalHit> lastElementalHit;

	/** @brief Registered lazily on the first gather, so the event holder is certainly up. */
	void RegisterSpellCastSink();
	/** @brief Calendar reading last frame, in game hours. -1 = not yet read. Watches for wait/sleep/fast-travel jumps, which move the game's clock without moving ours. */
	float spellGameHours = -1.0f;
	/** @brief Accumulated calendar-beyond-timescale movement, in game seconds. Zero-mean in normal play; a wait, sleep or fast travel piles up here and is then applied to every timer at once. */
	float spellGameDrift = 0.0f;
	/** @brief Fire atronach deaths waiting out the death animation. The death EVENT fires the moment the creature dies, but a flame atronach staggers and only then bursts - a mark at event time appears seconds before the explosion it is supposed to be from. Frost shatters and storm earths itself instantly, so only fire waits. */
	struct PendingDeathBlast
	{
		uint32_t formID = 0;
		SpellElement element = SpellElement::None;
		RE::NiPoint3 position{};
		float fuse = 0.0f;
		/** @brief Followed while it still resolves; the last position it was seen at is where the blast lands. */
		RE::ActorHandle actor;
	};
	std::vector<PendingDeathBlast> pendingDeathBlasts;

	/** @brief Queued by the sink on the GAME thread and drained by the gather on the render thread, hence the lock. */
	struct QueuedCast
	{
		RE::NiPoint3 position{};
		float radius = 0.0f;
		SpellElement element = SpellElement::None;
	};
	std::vector<QueuedCast> queuedCasts;

	/**
	 * @brief A cloak, known from the cast that started it.
	 *
	 * NOT read off the actor: walking an actor's active effects crashes, most
	 * reliably inside GetActiveEffectList on console-spawned actors. The spell
	 * RECORD carries element, magnitude and duration and is static data, so the
	 * cast says what began and its duration says when it ends.
	 *
	 * The cost: a cloak dispelled early keeps marking until its timer expires,
	 * and an innate aura that is never cast is not seen here at all.
	 */
	struct CloakState
	{
		RE::ActorHandle actor;
		SpellElement element = SpellElement::None;
		float rateScale = 1.0f;
		float remaining = 0.0f;
		/** @brief Seconds until this cloak's next discharge. A shock cloak arcs in bursts rather than glowing a steady ring. */
		float strikeTimer = 0.0f;
		/** @brief Advances per discharge so successive arcs land in different places. */
		uint32_t strikeSeed = 0;
		/** @brief Reach in world units taken straight from the effect's authored area, or 0 when it named none and the per-element cloak radius should be used instead. */
		float reachOverride = 0.0f;
		/** @brief Multiplier on whatever reach this aura settles on. Only innate auras use it: an atronach's body is a far larger source than a cloak wrapped round a mage, and each school states its own. */
		float reachScale = 1.0f;
		/** @brief Set on an INNATE aura - one the actor was born with rather than one anybody cast. It has no timer to run down, so it lives as long as the actor does. */
		bool innate = false;
		/** @brief Innate only: the one-frame death blast has been queued, so it never fires twice. */
		bool blasted = false;
		/** @brief Innate only: seconds of ground burn left after the body landed. Zero for the schools that leave nothing burning. */
		float burnRemaining = 0.0f;
		/** @brief Innate only: where this actor was last seen alive. An atronach is UNSUMMONED when it dies rather than left as a corpse, so the blast has to be thrown at the last sighting - the same shape as PendingBlast, which exists because a projectile is gone by the time it has detonated. */
		RE::NiPoint3 lastPosition{};
		/** @brief Innate only: seconds since this actor was last observed alive. Separates a death from a walk out of range, which would otherwise blast the snow wherever the actor happened to be standing. */
		float unseenFor = 0.0f;
	};
	/** @brief Queued by the sink on the game thread, drained by the gather. */
	std::vector<CloakState> queuedCloaks;
	/** @brief Live cloaks by wearer formID; a re-cast refreshes rather than stacks. */
	std::unordered_map<uint32_t, CloakState> activeCloaks;

	/**
	 * @brief What an actor's own records say it radiates, with nothing cast.
	 *
	 * An innate aura is never cast, so the cast sink never sees it. Not a race
	 * list or keyword table: the aura is a real SpellItem on the race's own
	 * spell list, and its cloak effect carries the same four axes every other
	 * detector classifies through (AbFlameAtronach carries AbAtronachCloakFire,
	 * archetype Cloak, resist ResistFire, magnitude 10; frost and storm match).
	 *
	 * Derived exactly like a cast spell, so modded atronachs work too. Every
	 * read is of a static FORM, never a live actor's effect list.
	 */
	struct InnateAuraRecord
	{
		SpellElement element = SpellElement::None;
		float rateScale = 1.0f;
		/** @brief Authored area in world units, or 0 when the effect named none. */
		float reachOverride = 0.0f;
	};
	/** @brief Resolved once per race form and kept: race records do not change while the game runs. Absence of an aura is cached too, so a wolf costs one lookup. */
	std::unordered_map<uint32_t, InnateAuraRecord> innateAuraByRace;
	/** @brief The NPC-record answers, keyed by the BASE they came from. Separate from the race cache on purpose: one map keyed by race holding a value read off an NPC is what gave every Nord in the game a frost aura from a single Ice Warlock. */
	std::unordered_map<uint32_t, InnateAuraRecord> innateAuraByBase;
	/** @brief Live innate auras by actor formID. Separate from activeCloaks because their lifecycles differ: a cast cloak runs down its own timer, an innate one lasts as long as its owner and then goes through the death phases. */
	std::unordered_map<uint32_t, CloakState> innateAuras;

	/** @brief Bodies still marking after death, by formID. Rides the same CloakState and the same emitter as everything else; only its start condition is different. */
	std::unordered_map<uint32_t, CloakState> corpseEffects;

	/** @brief Runs the after-death mark for one actor, whether or not it ever had an aura of its own. */
	void ConsiderCorpseEffect(RE::Actor* a_actor, float a_deltaTime);

	/** @brief Reads an actor's race (and its base) for an innate aura, through the cache. Implemented in SnowDeformation/Spells.cpp. */
	const InnateAuraRecord* ResolveInnateAura(RE::Actor* a_actor);

	/**
	 * @brief The uncached walk behind ResolveInnateAura, for callers off the render thread.
	 *
	 * The death sink runs on the GAME thread and must not touch the race cache
	 * the gather is reading, so it repeats the walk instead. A death is rare
	 * enough that the saving would have bought nothing anyway.
	 */
	static bool AuraFromActorRecords(RE::Actor* a_actor, InnateAuraRecord& a_out);
	static bool AuraFromSpellList(const RE::TESSpellList* a_list, InnateAuraRecord& a_out);

	/** @brief Runs the per-actor marks for everything in the window: innate auras (hover, trail, death blast, burnout) and bodies still burning after death. */
	void GatherActorMarks(float a_deltaTime, const RE::NiPoint3& a_cameraPosition, float a_cullRadius);

	/** @brief Throws the one-frame blast a dying innate-aura actor leaves. Shared by both death routes - the one that leaves a body and the one that simply vanishes. */
	void OpenInnateDeathBlast(CloakState& a_state, const RE::NiPoint3& a_position);
	/** @brief Last XY per cloaked actor, so a moving aura sweeps a band rather than dotting it. */
	std::unordered_map<uint32_t, float2> spellAuraPrev;
	std::unordered_map<uint32_t, float2> currentAuraPositions;

	/**
	 * @brief Emits the mark for one cloaked actor.
	 *
	 * Reads the actor's POSITION and nothing else - the same read every stamp
	 * in this feature already performs each frame.
	 */
	void ConsiderActorAuras(RE::Actor* a_actor, CloakState& a_cloak, float a_deltaTime);

	std::mutex queuedCastLock;

	struct SpellStats
	{
		uint projectiles = 0;
		/** @brief Concentration streams of any element. */
		uint streams = 0;
		/** @brief Placed hazards seen: spell walls, runes. */
		uint hazards = 0;
		/** @brief Blasts marked from a projectile leaving the manager - the route that does not depend on explosions being references at all. */
		uint detonations = 0;
		/** @brief Projectiles currently carrying a blast, waiting to die. A spell that never appears here was never recorded; one that sits here and never fires is being rejected at detonation. */
		uint armed = 0;
		/** @brief Sub-surface corridors marked this frame: projectiles cutting through the snow layer. */
		uint trails = 0;
		/** @brief Self-centred area spells caught by the cast sink. */
		uint casts = 0;
		/** @brief Shout cones laid down, and the discs they cost. */
		uint shouts = 0;
		uint shoutDiscs = 0;
		/** @brief Dash watches running, and how many are actually cutting a furrow this frame. */
		uint dashWatches = 0;
		uint dashGouges = 0;
		/** @brief Slow shoves tracking across the ground, leaving the line they took. */
		uint forceTracks = 0;
		/** @brief Buried frost effects raised onto the snow. */
		uint lifted = 0;
		/** @brief What the last shout classified as, so a wedge that should have been a track can be read rather than argued about. */
		uint lastShoutElement = 0;
		float lastShoutSpeed = 0.0f;
		float lastShoutForce = 0.0f;
		/** @brief 0 nothing, 1 wedge, 2 track, 3 rejected. */
		uint lastShoutVerdict = 0;
		/** @brief Cloaks currently running, marking the ground their wearer crosses. */
		uint auras = 0;
		/** @brief Innate auras seen this frame: atronachs and anything else whose records give it one without a cast. */
		uint innate = 0;
		/** @brief Dead innate-aura bodies still burning the ground they fell on. */
		uint burning = 0;
		/** @brief Emitters gathered past kMaxSpellEmitters and dropped by the distance sort. Non-zero means the frame carried more spell sources than the budget and the ones kept are the nearest. */
		uint emittersCulled = 0;
		/** @brief Death events received for an actor carrying an innate aura. If this stays 0 while atronachs die in front of you, the event is not the route. */
		uint deathsSeen = 0;
		/** @brief Death blasts actually opened. Against deathsSeen this says whether the event fired and the mark was rejected, or the event never came. */
		uint deathBlasts = 0;
		/** @brief Bodies marking the snow after death from what last struck them. */
		uint corpses = 0;
		/** @brief Projectiles whose effects name no element this feature knows. */
		uint rejectedElement = 0;
		/** @brief Projectiles that name an element but no explosion form, so there is no blast to arm. */
		uint rejectedNoBlast = 0;
		/** @brief Streams whose aim actually met the ground; the rest fall back to radiant heat. */
		uint groundContacts = 0;
		uint emitters = 0;
		/** @brief Emitters detected and classified whose mark is not implemented yet (pitting, crust). Not a fault: those arrive with their own steps. */
		uint pending = 0;
		/** @brief Last emitter's target-depth multiplier and footprint, so a source that marks nothing can be told from one marking invisibly. */
		float lastStrength = 0.0f;
		float lastRadius = 0.0f;
	};
	SpellStats spellStats;

	/** @brief Per-frame stamp diagnostics for the menu (rebuilt in GatherStamps). */
	struct StampStats
	{
		uint feet = 0;
		uint limbs = 0;
		uint shapes = 0;
		uint props = 0;
		uint propRefs = 0;
		uint propMovers = 0;
		/** @brief Moving props that carved by their render mesh this frame instead of by collision shapes. */
		uint propsRasterized = 0;
		/** @brief Actors that carved by their skinned mesh this frame instead of by bones. */
		uint actorsRasterized = 0;
		/** @brief Corpses drawn by the contact pass this frame (still settling, or woken). */
		uint corpsesRasterized = 0;
		/** @brief Settled corpses the still-corpse gate latched out of the contact draw this frame. */
		uint corpsesLatched = 0;
		uint spells = 0;
		/** @brief Stamps taken by actors and props, read before any emitter is. Against kMaxStamps - kSpellStampReserve this says whether the fight is running into the budget or nowhere near it. */
		uint beforeSpells = 0;
		/** @brief Actors stopped by the TRANSLUCENCY gate, which is a different measurement from floating and answers to a different setting. */
		uint incorporeal = 0;
		/** @brief What the nearest actor's skeleton offered the stamper. Feet decide the path outright: with them an actor prints heel-to-toe, without them it falls to its collision shapes, and an actor that passes every gate and still marks nothing is one of those two coming up empty. */
		uint nearestFeet = 0;
		uint nearestLimbs = 0;
		/** @brief Of the nearest actor's matched feet, how many are attached and scaled this frame. Matched-but-unusable is the edited-skeleton signature. */
		uint nearestUsableFeet = 0;
		/** @brief Nearest actor is on the collision-stamping failsafe. */
		bool nearestFallback = false;
		/** @brief Nearest actor's ground travel with zero foot prints; resets on every plant. */
		float nearestDryTravel = 0.0f;
		/** @brief Living actors stamping through the collision failsafe this frame. */
		uint fallbackActors = 0;
		/** @brief Nearest actor is MADE of an element, so the translucency test was skipped rather than passed. Without this the panel reports alpha 1.00 for an actor whose alpha was never read, which reads as "measured opaque". */
		bool nearestElemental = false;
		/** @brief Actors whose lowest contact never reached their footing this frame, so they carved nothing. */
		uint floating = 0;
		/** @brief Nearest non-player actor's measurements, so the floating gate can be read against a real creature instead of guessed at. */
		float nearestGapToRoot = 0.0f;
		float nearestGapToLand = 0.0f;
		uint nearestState = 0;
		bool nearestFloating = false;
		bool nearestValid = false;
		/** @brief Lowest skinned-body alpha on the nearest actor, so the translucency test can be read against a real ghost rather than guessed at. */
		float nearestBodyAlpha = 1.0f;
		std::string nearestBodyAlphaSource;
		bool nearestGhostFlag = false;
		bool nearestIncorporeal = false;
		/** @brief FormID of the nearest non-player actor, latched into skeletonProbeTarget at gather end. */
		uint32_t nearestFormID = 0;
		/** @brief Actors in range that were turned away because the stamp budget was already full. Whole-actor dropout: the range slider widens the gather radius against a fixed budget. */
		uint budgetTurnedAway = 0;
	};
	StampStats stampStats;

	/** @brief Runtime-only skeleton probe: per-foot trenching status of one NPC (the nearest, latched by formID with one frame of lag), plus which gate ate the actor when nothing carved. Filled by GatherStamps only while debugSkeletonProbe is on; drawn in Debugging Options. */
	bool debugSkeletonProbe = false;
	/** @brief One-shot: log the probed actor's whole node tree (names, scales, match classification) to CommunityShaders.log, for tester reports. */
	bool skeletonProbeDumpRequested = false;
	/** @brief FormID the probe follows: last frame's nearest non-player actor. */
	uint32_t skeletonProbeTarget = 0;
	struct SkeletonProbe
	{
		bool valid = false;
		uint32_t formID = 0;
		std::string actorName;
		/** @brief What happened to the actor this frame: which path carved, or which gate returned first. */
		const char* verdict = "";
		/** @brief The gather would hand this actor to the contact pass, so the airborne, elevated and floating gates were bypassed. */
		bool rasterCandidate = false;
		struct FootRow
		{
			std::string name;
			std::string toe;
			float scale = 1.0f;
			bool attached = false;
			float zAboveRef = 0.0f;
			float band = 0.0f;
			bool planted = false;
			bool stamped = false;
			float radius = 0.0f;
		};
		std::vector<FootRow> feet;
		uint usableFeet = 0;
		uint limbs = 0;
		uint limbsStamped = 0;
		uint shapes = 0;
		float dryTravel = 0.0f;
		bool collisionFallback = false;
		float bodyAlpha = 1.0f;
		std::string bodyAlphaSource;
		uint16_t alphaSettle = 0;
		float gapToLand = 0.0f;
		float floatingGap = 0.0f;
		/** @brief Nominal shell depth at the actor's XY (-1 if the cell is unbaked). Separates "stamps land but nothing here can display a trench" from skeleton problems. */
		float shellDepth = -1.0f;
		bool cellBaked = false;
	};
	SkeletonProbe skeletonProbe;

	/** @brief Last 3D-root position per loose prop (formID), rebuilt every frame from the in-range scan. The position gate runs before any collision traversal, so resting clutter costs one hash lookup per frame. */
	struct PropAnchor
	{
		RE::NiPoint3 pos;
		uint32_t cycle = 0;  // the scan cycle that last visited it; a full cycle unseen = the reference left
		bool mover = false;  // travelled, and its body has not slept since
	};
	std::unordered_map<uint32_t, PropAnchor> propPrevPositions;
	static constexpr uint32_t kPropScanInterval = 6;
	/** @brief Scan cycles completed; anchors carry the cycle they were last visited in. */
	uint32_t propScanCycle = 0;
	/** @brief The cells TES::ForEachReferenceInRange would walk, gathered the same way each frame and scanned a sixth at a time (PERF-RESEARCH §11.2-C): the engine's walk over every loaded reference was the 0.3 ms spike every sixth frame, not the callback. */
	std::vector<RE::TESObjectCELL*> propScanCells;
	/** @brief Frames since the last full reference scan (0 = scan this frame). The scan runs every kPropScanInterval frames; between scans only propScanMovers and propScanHazards are revisited and anchors update in place. */
	uint32_t propScanFrame = 0;
	/** @brief Props the last full scan saw moving, revisited every frame until the next scan. */
	std::vector<RE::ObjectRefHandle> propScanMovers[kPropScanInterval];
	/** @brief Placed hazards the last full scan found, replayed through ConsiderHazard every frame because the emitter list is rebuilt per frame. */
	std::vector<RE::ObjectRefHandle> propScanHazards[kPropScanInterval];
	size_t PropScanHazardCount() const
	{
		size_t n = 0;
		for (const auto& slice : propScanHazards)
			n += slice.size();
		return n;
	}
	/** @brief References visited by the last full scan, for the menu; stampStats.propRefs counts only the movers on the frames between. */
	uint propScanRefs = 0;

	/** @brief Stillness latch per corpse (formID). Once settled, only a large accumulated displacement (dragging, explosions) wakes it, so ragdoll micro-drift cannot re-trench under a buried corpse. Erased when the actor is seen alive again. */
	struct CorpseRest
	{
		uint16_t stillFrames = 0;
		bool settled = false;
		/** @brief Contact-drawn corpses: the bound centre last frame (motion source) and where it came to rest (wake reference). */
		RE::NiPoint3 prevCenter;
		bool hasPrevCenter = false;
		RE::NiPoint3 restCenter;
		/** @brief Root z from the previous frame, for the flight gate. */
		float prevZ = 0.0f;
		bool hasPrevZ = false;
	};
	std::unordered_map<uint32_t, CorpseRest> corpseRestStates;

	/** @brief Impact energy per living actor (formID): where its root was and when, its smoothed ground speed, and the fastest fall since it left the ground. */
	struct ImpactTrack
	{
		RE::NiPoint3 prev;
		double time = 0.0;
		float speed = 0.0f;
		float fall = 0.0f;
		/** @brief The last update's energy terms, and the fall speed a landing came in at: read by the snow events and the coat aging inputs. */
		float speedEnergy = 0.0f;
		float landingEnergy = 0.0f;
		float knockEnergy = 0.0f;
		float landingSpeed = 0.0f;
		bool seen = false;
		bool wasInAir = false;
	};
	std::unordered_map<uint32_t, ImpactTrack> impactTracks;
	/** @brief Speed Spoil: per foot ((formID << 8) | index), planted last visit, where, and when. */
	struct SpoilFoot
	{
		float2 last{};
		uint32_t frame = 0;
		bool planted = false;
	};
	std::unordered_map<uint64_t, SpoilFoot> spoilFeet;
	/** @brief Speed Spoil wedges thrown since load, for the menu. */
	uint64_t speedSpoilWedges = 0;
};
