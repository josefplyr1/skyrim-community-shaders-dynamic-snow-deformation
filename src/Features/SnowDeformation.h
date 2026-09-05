// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#pragma once

#include "Buffer.h"

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
	/** @brief Prop contact field: texels across, and half the window (world units) it covers around the deformation centre - 3 units per texel, finer than the map it feeds. */
	static constexpr uint kContactDim = 1024;
	static constexpr float kContactHalfExtent = 1536.0f;
	/** @brief Clear value of the contact field: nothing drawn over this column. */
	static constexpr float kContactNone = 1.0e30f;
	/** @brief Bones a single skin partition may carry: 240 float4 rows at 3 per bone, the game's own ceiling. Partitions past it are skipped rather than truncated. */
	static constexpr uint kContactMaxBones = 80;
	/** @brief Actors rasterized per frame at most; a crowd must not turn the spike into a full character pass. */
	static constexpr uint kContactMaxActors = 8;
	/** @brief Corpses drawn by the contact pass, budgeted apart from the living: a corpse holds its slot only until it settles. */
	static constexpr uint kContactMaxCorpses = 8;
	/** @brief Must match MAX_BOW_WAVES in SnowShell.hlsl and MAX_DEPOSIT_WAVES in DeformationUpdateCS.hlsl. Declared HERE because PerFrame sizes arrays with it - an in-class static constexpr must precede the struct that uses it (S4 r20 lesson). */
	static constexpr size_t kMaxBowWaves = 16;
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

	/** @brief Baked per-cell terrain data: 33x33 vertex heights (absolute Z) plus per-class coverage weights (0-255). Depths are applied at window-rebuild time so class sliders retune without a game re-bake. */
	/** @brief Texture layers kept per vertex: the heaviest of the quad's base texture plus its 6 painted layers. */
	static constexpr uint kShellVertexLayers = 4;
	/** @brief Layer slot with no texture (unpainted weight, or an LTEX with no texture set). */
	static constexpr uint16_t kNoLandTexture = 0xFFFF;
	/** @brief Registry cap. Beyond it new textures fall back to their class depth. */
	static constexpr size_t kMaxLandTextures = 512;

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
			float centerZ = 0.0f;
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
			/** @brief Where the foot stood last frame, for the contact stillness gate. */
			RE::NiPoint3 prev;
			bool hasPrev = false;
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
		/** @brief Lowest material alpha found on a skinned body geometry, or -1 until measured. A ghost is drawn see-through, which is the only thing that separates it from the ordinary NPC it otherwise is. */
		float bodyAlpha = -1.0f;
		/** @brief Frames until the alpha is measured again. A ghost's shader can arrive after its 3D does, so one early look at an opaque body must not stand for the actor's whole life. */
		uint16_t alphaRecheck = 0;
		/** @brief How many times the body alpha has been read. The first reads of a freshly spawned actor cannot be trusted, so they are taken quickly and often before the cadence drops to kBodyAlphaRecheckFrames. */
		uint16_t alphaSettle = 0;
		/** @brief Frames until cached feet are re-verified as still attached to the root. Runtime skeleton editors (RaceMenu/NiOverride, IED, MuSkeletonEditor) edit the live tree without swapping the root, so the root key alone cannot vouch for a cached node: a detached foot's world transform freezes and it never plants again. */
		uint16_t attachRecheck = 0;
		/** @brief XY units walked on the ground with zero foot prints. A healthy walker plants each foot every ~60 units, so a growing figure here means the cached feet no longer speak for the skeleton, whatever edited them. */
		float dryTravel = 0.0f;
		float prevPosX = 0.0f;
		float prevPosY = 0.0f;
		bool hasPrevPos = false;
		/** @brief The one mid-dry re-collection has been spent; the next threshold latches the fallback. */
		bool dryRecollected = false;
		/** @brief Latched demotion to collision-shape stamping (and collision-measured floating), cleared when the 3D root changes. The failsafe for skeletons whose feet exist but never plant. */
		bool collisionFallback = false;
		/** @brief Where the body stood when the contact pass last drew it: stillness is measured against the last DRAWN pose, so slow motion accumulates into a redraw. */
		RE::NiPoint3 contactPrev;
		bool hasContactPrev = false;
	};

	struct Settings
	{
		bool EnableSnowDeformation = true;
		bool ShowDebugTexture = false;
		/** @brief Scale on Havok collision-shape radii: 20 = the shapes' actual size; the 10 default halves them, which is the intended in-game read (full-size prints read bloated). */
		float StampRadius = 10.0f;
		/** @brief Width multiplier on foot-bone stamps (length stays anatomical). */
		float FootPrintScale = 1.5f;
		/** @brief Lower smoothstep edge of the stamp falloff, in PERCENT of the stamp radius: 0 = the softest, widest banks; 100 = full depth held to the very edge (clamped just below degenerate in the CB fill). */
		float TrenchWallSharpness = 50.0f;
		/** @brief Unsupported-snow settle speed, 0-1. Strips left standing between separate trails sink toward whichever side is shallower once BOTH sides are dug away; walls and open snow never move. Default OFF until A/B'd - it changes the map every subsystem reads (TRENCH-REALISM-PLAN.md Stage 3b). */
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
		/** @brief Depth a shock discharge pocks the snow to, as a fraction of the layer. Lightning throws snow aside rather than boring into it, so this stays well under a footprint's carve. */
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
		/** @brief DDS for the bolt, relative to Data. The shader draws a real core-and-falloff channel on its own, so a path that fails to resolve costs detail rather than leaving a black band in the air - which is why a default can be shipped at all. Verified present in Skyrim's own effects set. */
		std::string LightningArcTexturePath = "Textures\\Effects\\fxlightningbolt01.dds";
		/** @brief Off by default: the procedural core-and-falloff channel is the primary look and the texture is the alternate. The path above stays filled so enabling this needs no typing. */
		bool LightningArcUseTexture = false;
		/** @brief How dark a discharge burns the snow it struck. 0 removes the scorch and leaves the pocking alone. */
		float ScorchStrength = 0.85f;
		/** @brief How fast frost sets a crust, for a spell of Frostbite's strength. */
		float CrustRate = 0.6f;
		/** @brief Reach of a frost mark on the ground, in world units. */
		float CrustRadius = 90.0f;
		/** @brief Depth a boot still prints on fully crusted snow, as a fraction of loose snow. NOT zero: actors stand on terrain while the shell floats above them, so a crust that takes no print at all buries feet inside apparent ice. */
		float CrustPrintDepth = 0.20f;
		/** @brief How icy crusted snow shades: 0 leaves it looking like powder, 1 gives the full polish. */
		float CrustGloss = 1.0f;
		/** @brief Roughness of fully crusted snow. Lower is glassier; snow sits near 0.6. */
		float CrustRoughness = 0.25f;
		/** @brief How far a crust flattens the snow's own normal map. The strongest of the ice cues by a distance: powder reads as grain and ice reads as a sheet, so smoothing the surface says "frozen over" louder than reflectance or colour can. */
		float CrustNormalFlatten = 0.75f;
		/** @brief Brightness of the grazing-angle sheen on crusted snow. Snow is already near-white, so a specular lobe has almost no headroom above it; a sheet catching the sky at a glancing angle is the one thing powder cannot do, and this is the strongest ice cue after smoothness. */
		float CrustSheen = 1.50f;
		/** @brief Reflectance of fully crusted snow. Loose snow sits near 0.028, which is so low that a physically honest ice value is invisible beside it; this is a look knob, not a measurement. */
		float CrustSpecular = 0.250f;
		/** @brief Colour cast multiplied onto crusted snow. Slightly dark and slightly blue reads as refrozen; leave at 1,1,1 for no cast at all. */
		std::array<float, 3> CrustTint = { 0.588f, 0.863f, 1.000f };
		/** @brief Minutes a crust takes to thaw on its own, with no snowfall at all. Ice answers to temperature rather than to weather, so this runs even under a clear sky where the refill has stopped. */
		float CrustThawMinutes = 4.0f;
		/** @brief How completely carving through a crust destroys it, against how deep the cut went. At 1 a shallow print barely dulls the glaze; higher values let any cut break the skin properly, which is what stops a trench through ice reading as a groove in ice cream. */
		float CrustBreakOnCarve = 6.0f;
		/** @brief Stamp radius past which a shape counts as heavy enough to break a crust rather than print on it. A human foot sits well under this; a mammoth or a landing dragon well over. */
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
		/** @brief Total spread of a shout's cone, in degrees. The records author no width - only a reach - so this is the one number about a shout's shape that has to be taste. */
		float ShoutConeSpread = 25.0f;
		/** @brief Multiplier on the reach the shout's own projectile authors. 1.0 is exactly what the game says: 1000 units for Unrelenting Force, 1200 for the breaths, 10000 for a dragon's. */
		float ShoutConeLength = 1.0f;
		/**
		 * @brief DDS the frost pattern is drawn from, relative to Data.
		 *
		 * Must be a TILEABLE surface, hence a landscape texture rather than a
		 * decal: a decal carries its content in the middle and nothing at the
		 * edges, so the stochastic sampler can only produce clumps with gaps.
		 * Editable because every modlist has different ice. The `_n` companion
		 * beside it carries the crystal structure.
		 */
		std::string FrostTexturePath = "Textures\\Landscape\\frozenmarshice01.dds";
		/** @brief How strongly the frost pattern shows on crusted snow. This is the CRYSTAL detail; the polish, colour and sheen that make it read as ice are the knobs below and are untouched by it. */
		float FrostPatternStrength = 1.0f;
		/** @brief World units across one tile of the frost pattern. Sampled stochastically, so this sets the size of the crystal detail rather than the size of a repeat - there is no repeat. */
		float FrostPatternScale = 96.0f;
		/**
		 * @brief Raise buried frost effects onto the snow surface instead of leaving them under it.
		 *
		 * Effects placed at terrain height are swallowed by the shell above
		 * them. Frost is the only school where that matters - fire and lightning
		 * remove the snow they sit in, frost only hardens it. The ONLY thing in
		 * the feature that moves a game object, so it is narrow and switchable.
		 */
		bool LiftFrostEffects = true;
		/** @brief How deep a travelling shove scours, against a full carve. Well under 1 on purpose: a vortex scours the surface rather than excavating to the ground, and the berm is derived from how deep the cut goes - so this is also the dial that decides whether the track reads as a scoured hollow or as a canyon with a ridge down each side. */
		float ForceTrackDepth = 0.45f;
		/** @brief Width of the track a slow shove leaves behind it, in world units. Nothing authors a width for any shout - only a reach - so this is taste, exactly as the cone's spread is. */
		float ForceTrackWidth = 80.0f;
		/** @brief Width of the furrow a dash shout ploughs, against the dasher's own size. A dragon hurling itself forward cuts a wider one than a man. */
		float DashGougeScale = 1.0f;
		/** @brief Depth a full-strength shove carves, as a fraction of the layer. Force is the only school that DISPLACES snow rather than changing it, so it is also the only one that raises a berm at the far lip - which is what makes Unrelenting Force read as pushed rather than deleted. */
		float ForceCarveDepth = 1.0f;
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
		/** @brief Statics skin, flat class: layer height on flat split-normal meshes (walkways, roofs, planks); classified per mesh on the GPU by smoothed-vs-raw normal divergence. These get completely flat snow (straight-up offset, raw shading normal). Default 0: painted directly onto the surface; even 1 unit reads as a tiny hover. */
		float ObjectsSnowDepth = 0.0f;
		/** @brief Steepest surface slope (degrees) that still grows the S4 shell; steeper faces keep the flat recolor only. 90 = every up-facing surface, small values = near-horizontal tops only (Josef's angle knob, 2026-08-29). Rocks/mountains/cliffs use RockMaxSlopeDeg instead. */
		float ShellMaxSlopeDeg = 65.0f;
		/** @brief The rock family's own max slope (Josef's call: rocks/mountains/cliffs were the only sufferers of a low global slope) - applies to draws the mountain/cliff name match flags (CapturedSnowStatic::forceRounded). */
		float RockMaxSlopeDeg = 65.0f;
		/** @brief S4 plane SPLIT knob (world units): a ledge whose slope discontinuity exceeds this - in either direction - becomes its own snow plane with its own rims and roll (stair treads separate). Lower = stricter splitting. Feeds HeightProcessCB::RimStep. */
		float PlaneSplitStep = 6.0f;
		/** @brief "Ignore Cover Above" (world units, Josef's crank): a surface more than this far ABOVE a plane is a separate world - it neither splits the plane (no taper ring under rails/walls) nor demotes its vertices to a peeled layer; the dome keeps full uniform height and clips through. Rises within [PlaneSplitStep, this] still separate (stair treads). Feeds HeightProcessCB::OverheadIgnore and StaticsCB::OverheadIgnore. */
		float OverheadClearance = 0.0f;
		/** @brief A/B (Josef): ON = co-planar surfaces a small horizontal gap apart meld into one dome (drop-bridge reach 3 texels). OFF = "cling" - no bridging at all, every object's shell rolls at its own raster edge and nearby shells simply clip into each other. */
		bool MeldCoPlanar = false;
		/** @brief The width failsafe (Josef's "peak rounded shape" spec): the dome's fillet radius freezes at this many times the feature's crest height - at 1 the frozen shape is the perfect half-dome exactly filling the feature's width; higher lets narrow features bulge taller before freezing. Wide interiors are unaffected. */
		float PileHeightRatio = 1.0f;
		/** @brief "Edge Lump Size", 0-3: the solid contour of the shell and the coat wanders through a blob field of this cell size (x kEdgeLumpBig), so the edge breaks into round lumps; 0 = the plain ragged edge. Also the cell size of the Edge Lump Reach islands. Feeds StaticsCB::EdgeBreakupScale. */
		float SkinEdgeLumpSize = 0.0f;
		/** @brief "Edge Lump Reach", 0-1: how far past the solid snow's contour the lumps hang on, in world units (1 = kEdgeReachUnits, 0 = no lumps), measured through the smooth projected weight's gradient so a wall's uniform faint frosting never counts as an edge. Feeds StaticsCB::EdgeFlankWidth. */
		float SkinEdgeFlankWidth = 0.01f;
		/** @brief P3 (edge-research study), 0-100%: how strongly sky exposure weights the object shell's depth. Open tops keep full depth; surfaces under cover in their own column and columns shaded by tall neighbours thin toward a dusting. 0 = off (pre-P3 behaviour). */
		float SkyExposurePct = 50.0f;
		/** @brief P4 (edge-research study), 0-100%: diffusion ("settling") on the cone depth fields after the repose chains. Rounds dome rims, arches shells across slit gaps instead of black cracks, denoises the raster. 0 = off (pre-P4 behaviour). */
		float SnowSettlingPct = 50.0f;
		/** @brief S4 plane MERGE knob (world units): surfaces within this height below a plane's top merge into it instead of claiming one of the three peeled layers. Raise so thin trims/beams under a roof stop starving the floor of a layer. Feeds StaticsCB::PeelTol. */
		float PlaneMergeHeight = 8.0f;
		/** @brief "Snow Fill", 0-100%: how much of the projected-snow footprint the Lighting recolor pushes to full shell-snow weight, most up-facing pixels first; 100 = every projected pixel solid (SKIN-PLACEMENT-PLAN round 13 - its own setting, decoupled from any depth). */
		float ProjSnowFillPct = 100.0f;
		/** @brief Model-class override: ROAD MESHES (matched by geometry name or road/bridge texture path). Default deliberately below the ~30-unit surrounding snow classes: the shallow band is what makes the road's course readable through the snowfield. */
		float RoadMeshesDepth = 10.0f;
		/** @brief Carve trenches into snow on non-road objects. Parked off until object trenching is reworked; roads carve regardless. */
		bool ObjectTrenches = false;
		/** @brief SKIN-PLACEMENT-PLAN S2: the skin's up-facing mask is multiplied by the NIF's authored projected-snow term (vertex alpha x normal-Z minus the material threshold), so surfaces Bethesda painted bare (walkway undersides, posts, railings) shed their skin. Suppressor only - it never adds snow; draws without projected-UV data are unchanged. Default ON per Josef's A/B verdict 2026-08-28 (red-only zones deleted, nothing lost snow it correctly wore). */
		bool ProjMaskPlacement = true;
		/** @brief SKIN-PLACEMENT-PLAN S2b: object snow depth SCALES with the authored density instead of ProjMaskPlacement's hard cutoff - thick where the paint is solid, thinning to a dusting where it fades. Supersedes the sharp gate while on (multiplying both would double-punish sparse paint). */
		bool ProjDepthDensity = true;
		/** @brief Master toggle for the raised 3D object snow layer = the S4 shell (the rolling-ball fillet over the fill's cyan slice, PD-carrying draws only). The old object shell is RETIRED (2026-08-29): draws without projection data get no skin at all - the Lighting recolor still covers the technique-classified ones flat, and roads keep their own machinery regardless. Off skips only the skin draws - capture, height rasters and the road/trench patch keep running. */
		bool ObjectSnow3D = true;
		/** @brief ROAD-HEIGHTFIELD-PLAN: roads drop their skin and the trench patch owns the whole road surface, so road snow is ONE deformable heightfield instead of skin + patch + floor + POM trench. Default ON per Josef's S0 verdict 2026-08-25 (no sheet, no verge seam). Bridges excluded pending #9e. */
		bool RoadHeightfield = true;
		/** @brief Shell albedo texture, loaded through the VFS. User-editable so the shell can be matched to the modlist's snow by eye. The loader resolves PBR companion maps and falls back to the legacy path when the PBR set is absent. */
		std::string SnowTexturePath = "Textures\\PBR\\Landscape\\snow01.dds";
		/** @brief Radius multiplier for the workspace clearings (workstations, stalls, wells, shrines). */
		float TrampleZoneScale = 0.75f;
		/** @brief Snow height remaining in a workspace clearing, in PERCENT of the class depth. 0 = melted to the floor, 100 = no clearing. */
		float TrampleZoneHeight = 50.0f;
		/** @brief On = a whisker of stochastic snow dust scatters just beyond the committed edge onto the ground; off = a clean binary cut. */
		bool SnowBorderDithering = true;
		/** @brief Minimum snow left on carved trench floors, in units above the terrain. Values near 0 let trampling wear through to the ground, which needs shell shadow casting and two-sided height blending to read correctly; until then a low floor exposes a bright, unblended pit. */
		float TrenchFloorHeight = 3.0f;
		/** @brief World-unit jitter of where class-depth borders fall (fine-grained domain warp), so snow edges never trace the texture seam. Capped 37-unit wander plus a fine 8-unit octave. */
		float SnowBorderNoise = 16.0f;
		/** @brief World-unit radius widening the depth ramp between neighboring classes, so deep snow meets shallow ground in a slope instead of a ravine wall. */
		float SnowBorderSmoothness = 32.0f;
		/** @brief Border Fade, as a percent. Remapped to the internal 2..64 contact-term band on upload; it mainly sets how visible the outward dust is. */
		float SnowBorderFade = 100.0f;
		/** @brief Angle-of-repose slope for the snow-height field (rise per world unit; 1.0 = 45 degrees). Steeper = raised snow clings tighter: narrow banks instead of broad aprons, juttier mounds. */
		float SnowMoundSteepness = 1.0f;
		/** @brief Dune-field amplitude in world units; 0 flattens deep snow into a mathematically smooth sheet. */
		float UndulationStrength = 8.0f;
		/** @brief Multiplier on the dune field's wavelengths; larger = broader, calmer waves instead of a spike carpet. */
		float UndulationSpacing = 1.0f;
		/** @brief Tessellate the shell and the trench patch. Its real job is trench smoothness: the hull shader's factors key off the deformation map, so carves get vertex density no coarse grid can express. Independent of ReliefDepth. */
		bool Tessellation = true;
		/** @brief Parallax self-shadow strength on the snow micro-relief (Extended Materials' term, the one PBR ground already receives). 0 skips the taps entirely. */
		float ParallaxShadowStrength = 0.5f;
		/** @brief Skin DynDOLOD's merged LOD atlas batches too. Those batches wear a generic atlas whose path says nothing about snowiness, so they are otherwise dropped and the objects inside them keep no distant snow. Measured +53 captures for +0.05 ms; a merged batch is one mesh, so this is all-or-nothing per batch. Turn off if any batch turns out to carry non-snow objects that gain snow. */
		/** @brief Parallax occlusion depth on the landscape shell, as a multiplier on the PBR config's displacementScale. 1 = exactly the slab depth PBR ground gets, since kSnowUVTile matches the landscape tiling. 0 skips the march. */
		float ParallaxDepth = 1.0f;
		/** @brief Edge berm crest height as a fraction of the local snow depth. */
		float BermHeight = 0.20f;
		/** @brief Stage 3 P5: rolled rim lip height as a fraction of local depth (cornice look). 0 = off. */
		float RimLip = 0.05f;
		/** @brief Stage 3 P5: rim teeth strength - the carve contour breaks into irregular teeth on the border work's two-octave noise. 0 = off. */
		float RimTeeth = 0.33f;
		/** @brief Stage 3 P6: berm clod amplitude in world units - the crest breaks into coarse thrown chunks at kClodSizeScale cells. 0 = off. */
		float BermClods = 2.0f;
		/** @brief Bow wave: crest height as a fraction of local snow depth. 0 = off. */
		float BowWaveHeight = 0.30f;
		/** @brief Bow wave: how far ahead of the feet the crest sits, and how far it stretches along travel. Not a size multiplier; width comes from BowWaveForward. */
		float BowWaveReach = 1.15f;
		/** @brief Bow wave: 0 = a ring all round the actor, 1 = only dead ahead. Mid values give the crescent. */
		float BowWaveForward = 0.80f;
		/** @brief Bow wave: how far the crest breaks into uneven lumps rather than a smooth swell (P6's clod octave, world-anchored). 0 = smooth. */
		float BowWaveChunk = 0.25f;
		/** @brief Bow wave: speed (units/sec) at which the crest reaches full strength. Lower = a walk already pushes. */
		float BowWaveFullSpeed = 200.0f;
		/** @brief Churn lump amplitude in world units on carved/piled snow (trench walls, floors, berms). */
		float ChurnHeight = 4.0f;
		/** @brief Multiplier on the churn lump wavelengths (larger = broader chunks). */
		float ChurnSize = 0.25f;
		/** @brief Re-march the SSS mask against the SHELL surface in the near field, instead of trusting the ground-marched mask. Restores grass shadows on the snow without the buried-caster prints; costs 8 depth taps per lit shell pixel. BOTH SHELLS run it (SkinRemarchSSS in SnowStaticsShell.hlsl is the same function on the same gate), so turning it on no longer makes the two shade differently across their seam. */
		bool ShellSSSRemarch = true;
		/** @brief Heightfield self-shadow (the 5-tap horizon march) on both shells. Off = only the cascades, the SSS mask and the re-march shade the snow. A/B for dark blotches on open snow at a low sun. */
		bool ShellHorizonMarch = true;
		/** @brief Object snow casts shadows: the S4 skins' depth-only caster pass. Off = the raised object snow throws no shadow of its own (its object still does). A/B for shadows that seem to come from snow nothing can see. */
		bool ObjectSnowShadows = true;
		/** @brief Streak fix for the re-march: occluders are thin shells (Bend SSS SurfaceThickness, 48 units), so a character in front of the ray no longer paints their silhouette as a streak across the snow behind them. */
		bool ShellSSSRemarchThickness = true;
		/** @brief Caster height cap (units above the snow line) for the re-march. Taller casters already shadow via the cascades, so their re-march copy is doubled bleed (actors, rails). 20 accepts short grass only; 200 accepts everything. */
		float ShellSSSRemarchCasterCap = 20.0f;
		/** @brief Skip shell patches whose depth has reached the -8 floor everywhere - ground with no snow class under it at all, which sits below the terrain and cannot produce a pixel. Tests the floor rather than a threshold part-way up, so the ramp that climbs to a snow layer is never cut. */
		bool ShellBareGroundCull = true;
		/** @brief How completely trampled snow loses its glints (packed snow has crushed the crystals that sparkle). Shared by both shells. */
		float CompactMatte = 0.6f;
		/** @brief Deformation map resolution (1024/2048/4096, snapped to pow2 - the toroidal mask requires it). The performance side of trench detail: cost scales quadratically (S0: 0.29 / ~1.1 / 4.71 ms full-map at the anchor), texel size scales with it and with the Trenches range. Applies like a range change: recreate + clear, the store re-injects. Promoted from the S0 debug combo once S3 made it a real perf lever. */
		uint32_t DeformMapResolution = 2048;
		/** @brief Render distances in meters (converted via kUnitsPerMeter). The shell itself auto-sizes to the loaded-cell grid (no slider); Trenches resizes the deformation window and clears the map on apply (content is scale-relative). */
		float RangeTrenchesM = 125.0f;
		float RangeSkinsM = 750.0f;
		/** @brief Distance (m) by which the skin's GEOMETRIC height has collapsed to zero, at the deepest class; shallower classes collapse proportionally sooner. Past the object height window (kHeightMapHalfExtent / kUnitsPerMeter, ~58 m) the rim-wall gate has no data, but the remaining rim is sub-pixel at that range â€” measured clean out to 200 m. */
		float RangeSkinsGeometryM = 100.0f;
		/** @brief Rasterizer depth bias for the object skins, in depth-buffer ULPs toward the camera (D3D11 DepthBias, negated). Replaces the decal viewport cap's accidental ~500-ULP push, which let a skin beat its own mesh at range but stood a peak 2000 units in front of its mist. */
		float SkinDepthBias = 64.0f;
		/** @brief Slope-scaled part of the same bias (D3D11 SlopeScaledDepthBias, negated): grows on grazing faces. Default 0: the constant term alone settled the contest with no visible cost. */
		float SkinSlopeDepthBias = 0.0f;
		/** @brief Object-skin tessellation cap: no generated edge segment shorter than this many screen pixels. 0 = off. The hull's base rule already targets ~20 px, so values below that trim only the rim-roll term (1-unit segments, sub-pixel past ~500 units); larger values coarsen the whole skin. Not bit-identical: it changes the surface. 16 measured -0.32 ms on StaticsShell with no visible change (PERF-RESEARCH 9.6); 32 is the tier value. */
		float SkinTessCapPx = 16.0f;
		/** @brief LOD-diffuse snow classification: 0 = only bright white counts, 1 = pale gray already counts. */
		float LODSnowSensitivity = 0.5f;
		/** @brief Horizon snow: recolor the game's LOD terrain with the shell's snow material wherever its bake classifies as snow. */
		bool HorizonSnow = true;
		/** @brief "Recolor Projected Snow" (SKIN-PLACEMENT-PLAN S3, round 11): projected snow wears the shell's snow set (albedo + PBR response) inside the object's own Lighting draw, on draws whose projected material is snow - every angle by construction. "Snow Fill" (ProjSnowFillPct -> SettingsGPU::ProjSnowFill) pushes the footprint to full shell-snow weight, most up-facing pixels first; max = every projected pixel solid. The flat-shell GEOMETRY experiments (rounds 4-10) are retired - the recolor has the real weight, nothing to reconstruct, no geometry to miss. */
		bool ProjSnowMatch = true;
	};

	/** @brief GPU-side settings, appended to the shared FeatureData cbuffer (b6). Layout must match SnowDeformationSettings in SharedData.hlsli. */
	struct alignas(16) SettingsGPU
	{
		float2 WindowOrigin;
		float InvWorldSize;
		uint EnableSnowDeformation;

		uint DebugTerrainOverlay;
		/** @brief Horizon snow (LOD terrain recolor): distance ramp start and 1/band, aligned to the shell edge. */
		float LODReplaceStart;
		float LODReplaceFadeInv;
		float LODSnowSensitivity;

		float SnowIsLinear;
		float SnowRoughnessScale;
		float LODReplaceEnable;
		/** @brief Snow normal map bound at t103 (0 = legacy set without one). */
		float SnowHasNormal;

		/** @brief was LODReplaceLegacy; the legacy recolor A/B is retired, slot kept for layout. */
		float padLegacy;
		/** @brief Projected-snow material match enabled and the snow set is bound. */
		float ProjSnowEnable;
		/** @brief Baked-snow (glacier) material match enabled and the snow set is bound. */
		float padBaked;
		/** @brief Snow Fill, 0..1: fraction of the projected-snow footprint the Lighting recolor pushes to full shell-snow weight, most up-facing pixels first; 1 = every angle solid. Mirror in SharedData.hlsli. */
		float ProjSnowFill;

		/** @brief Toroidal deformation-map addressing for Lighting's GetDeformation: physical position of logical texel (0,0). Mirror in SharedData.hlsli. */
		DirectX::XMINT2 DeformMapOrigin;
		DirectX::XMINT2 DeformTorusPad;
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
		/** @brief Retired TrailIrregularity slot; layout kept. */
		float padTrail;
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

		/** @brief 1 = InjectDepth holds the tile store's memory of texels arriving from outside the window. Its own row: Stamps must start 16-byte aligned. */
		uint InjectValid;
		/** @brief The frame's span in GAME time, expressed in the seconds DeltaTime is measured in. Equal to DeltaTime during ordinary play; a wait or a sleep passes hours without rendering them, and the world's own clocks (glaze thaw, slump) must not sit those hours out. Stamp application deliberately keeps DeltaTime - a fire must not carve its whole basin in the single frame after a wait. Claimed a pad slot, so the layout is byte-identical. */
		float GameDeltaTime;
		/** @brief 1 = the CS paints the per-texel activity view (u2): which texels changed at stored precision, and in which channel. Claimed a pad slot, layout unchanged. */
		uint DebugActivityView;
		uint InjectPad;

		/** @brief Arriving-band rects in LOGICAL texel space (x0, y0, w, h); RingCS covers their union by flat index. Two at most: one band per scrolled axis, or one full-map rect on a clear. */
		DirectX::XMINT4 RingRects[2];
		uint RingRectCount;
		uint RingTotalTexels;
		/** @brief 1 = the tile scans list every tile (the force-all-dirty cross-check). Claimed a pad slot, layout unchanged. */
		uint ForceAllDirty;
		uint RingPad;

		float4 Stamps[kMaxStamps];
		/** @brief Capsule segment start per stamp (the stamped shape's previous position). */
		float4 StampEnds[kMaxStamps];

		/** @brief Bow wave, deposited into the map's .w channel so the crest PERSISTS instead of following the feet. x = live count, y = reach, z = forward bias, w = settle seconds. Appended last; BermFieldCS mirrors only the leading rows and is unaffected. */
		float4 DepositParams;
		float4 DepositPosDir[kMaxBowWaves];
		float4 DepositShape[kMaxBowWaves];

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
		float ViewPad;
		/** @brief Object height window (RenderObjectHeightMap) the road raster at t9 was captured in; HasRoadRaster = 0 when it is not bound. */
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		float HasRoadRaster;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	Settings settings;

	/** @brief Seconds for compressed snow to fully recover at 1.0 snowfall intensity and a 1.0x multiplier. */
	static constexpr float kBaseRefillTime = 700.0f;
	/** @brief Precipitation particle density that maps to 1.0 snowfall intensity (vanilla densities run ~1-3). */
	static constexpr float kReferenceSnowDensity = 2.0f;
	/** @brief Intensity ceiling; keeps extreme weather-mod densities from erasing trails outright. */
	static constexpr float kMaxSnowfallIntensity = 4.0f;

	/** @brief Snowfall intensity from the weather records' precipitation density, faded across weather transitions. 1.0 = reference-density snowfall. */
	float ComputeSnowfallIntensity() const;
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
	ID3D11ShaderResourceView* GetDeformationSRV() const { return deformationTextures[0]->srv.get(); }
	/** @brief SRV of the baked berm field; null before SetupResources. */
	ID3D11ShaderResourceView* GetBermFieldSRV() const { return bermFieldTexture ? bermFieldTexture->srv.get() : nullptr; }

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
	/** @brief World XY of the corner of texel (0,0) of the current deformation window. */
	float2 GetWindowOrigin() const { return windowOrigin; }

	/** @brief Creates the ping-pong deformation textures and the per-frame constant buffer. */
	virtual void SetupResources() override;

	/** @brief (Re)creates the ping-pong deformation textures at deformMapDim, releasing any previous pair. */
	void CreateDeformationTextures();

	/**
	 * @brief Per-frame update: gathers actor stamp positions, scrolls the window
	 * to follow the camera, and dispatches the deformation update compute shader.
	 */
	virtual void Prepass() override;

	/** @brief Returns the ring compute shader (rewrites the texels a scroll reassigned: inject or pristine), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationRingCS();
	/** @brief Returns the map-evolution compute shader (refill/decay/slump - the neighbour-reading pass), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationEvolveCS();
	/** @brief Returns the stamp compute shader (stamps + bow waves, in-place RMW, one group per CPU-listed tile), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationStampCS();
	/** @brief Returns the full-map stamp fallback (tile list past its cap, or force-all-dirty). */
	ID3D11ComputeShader* GetDeformationStampAllCS();
	ID3D11ComputeShader* GetContactViewCS();
	ID3D11ComputeShader* deformationRingCS = nullptr;
	ID3D11ComputeShader* deformationEvolveCS = nullptr;
	ID3D11ComputeShader* deformationStampCS = nullptr;
	ID3D11ComputeShader* deformationStampAllCS = nullptr;
	ID3D11ComputeShader* contactViewCS = nullptr;

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
	/** @brief Evolve tile list: [0] count, then packed tiles. GPU-only (scan appends, TileArgsCS sizes the indirect dispatch); the CPU never reads it. */
	winrt::com_ptr<ID3D11Buffer> evolveTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> evolveTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> evolveTileSRV;
	/** @brief Indirect dispatch args for the evolve pass, written by TileArgsCS. */
	winrt::com_ptr<ID3D11Buffer> evolveArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> evolveArgsUAV;
	/** @brief Set when the map (and with it the occupancy grid) was (re)created; the next Prepass seeds occupancy all-1. */
	bool tileGridsNeedInit = true;
	/** @brief Last consumed evolve-tile census (from the activity readback; lags by the ring like the verdict). */
	uint32_t evolveTilesLast = 0;

	/** @brief Returns the evolve-scan compute shader (occupancy + halo -> tile list), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationScanEvolveCS();
	/** @brief Returns the list-count -> indirect-args shader, compiling it on first use. */
	ID3D11ComputeShader* GetDeformationTileArgsCS();
	/** @brief Returns the berm-scan compute shader (dirty grid + tap-reach halo -> tile list), compiling it on first use. */
	ID3D11ComputeShader* GetDeformationScanBermCS();
	/** @brief Returns the tiled berm bake (indirect over the berm tile list), compiling it on first use. */
	ID3D11ComputeShader* GetBermFieldTiledCS();
	ID3D11ComputeShader* deformationScanEvolveCS = nullptr;
	ID3D11ComputeShader* deformationTileArgsCS = nullptr;
	ID3D11ComputeShader* deformationScanBermCS = nullptr;
	ID3D11ComputeShader* bermFieldTiledCS = nullptr;

	/** @brief Per-tile "the map changed here" - the berm bake's dirty set, marked by every map writer and ACCUMULATED until a berm rebuild consumes it (cleared after), so the A/B toggle re-enabling rebuilds exactly what it missed. Recreated with the map; seeded all-1. */
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

	/** @brief Arriving-band rect in logical texel space; see ComputeArrivalRects. */
	struct ArrivalRect
	{
		int x0, y0, w, h;
	};
	/** @brief The rects whose world assignment changes under this frame's scroll (or the full map on a clear). ONE definition shared by RingCS's dispatch and BuildTrenchInject, so the ring pass and the inject upload can never disagree about which texels are arriving. Returns the rect count (0-2). */
	int ComputeArrivalRects(DirectX::XMINT2 a_scroll, bool a_clearing, ArrivalRect a_rects[2]) const;

	/** @brief Copies a LOGICAL-space box out of the toroidal map into a staging texture laid out logically: up to four physical segments split at the wrap seam, each landing at its logical offset in the destination. Every CPU read of the map routes through this so no site does its own torus arithmetic. a_physOrigin is the MapOrigin the source content was written under. */
	void CopyLogicalBox(ID3D11Texture2D* a_dst, ID3D11Texture2D* a_src, int a_x0, int a_y0, int a_w, int a_h, DirectX::XMINT2 a_physOrigin);
	/** @brief Returns the berm field bake compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetBermFieldCS();
	ID3D11ComputeShader* bermFieldCS = nullptr;
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
	/** @brief The ground as the engine renders it, at the land mesh's own 32-unit spacing: bicubic Catmull-Rom of the 128-texel window with cell edges extrapolated (TerrainFineCS), 9 cells centred on the camera's cell, rebuilt with the window. SampleTerrain's height reads it with the land's own checkerboard triangulation, so every shell vertex lies on the visible surface. */
	static constexpr int kShellFineCells = 9;
	static constexpr float kShellFineTexel = 32.0f;
	static constexpr int kShellFineDim = kShellFineCells * 128;
	Texture2D* shellTerrainFine = nullptr;
	/** @brief 2x2 and 4x4 maxima of shellTerrainFine (64- and 128-unit texels, TerrainFineMaxCS): the far bands' conservative ground, so a chord across several land quads clears the surface by construction. */
	Texture2D* shellTerrainFineMax1 = nullptr;
	Texture2D* shellTerrainFineMax2 = nullptr;
	ID3D11ComputeShader* terrainFineMaxCS = nullptr;
	ID3D11ComputeShader* GetTerrainFineMaxCS();
	/** @brief The rejected far-band fix, opt-in for comparison (ShellFlags.x bit 4): far vertices take the highest ground within their span. A box maximum is piecewise constant, so it terraces on slopes and buries rocks and NPCs on rough ground (Josef, 2026-09-05). Runtime-only. */
	bool shellFarMaxLift = false;
	/** @brief A/B: stops the hull's far relief tessellation (bit 5), so the 64/128-unit bands chord across the land again and the distant holes return. Runtime-only. */
	bool shellFarTessDisabled = false;
	float shellFineOriginX = 0.0f;
	float shellFineOriginY = 0.0f;
	bool shellFineValid = false;
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
	/** @brief A/B: height from the 128-texel window as before (bit 2 of ShellFlags.x). Runtime-only. */
	bool shellLandHeightDisabled = false;
	/** @brief A/B: flips which corner rotation the tessellated patches use for a '/' land quad (bit 3). The tessellator's factor-1 diagonal is assumed to be domain (0,0)-(1,1); if the height-delta view shows a checkerboard of sag on the 32-unit band, this is the other guess. Runtime-only. */
	bool shellTessDiagonalFlip = false;
	/** @brief A/B: the legacy grid draws with the old camera-phased union-jack index buffer instead of the land-matched one. Runtime-only. */
	bool shellOldUnionJack = false;

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
		uint GridDim;
		uint TerrainDim;
		uint ShellDebugData;

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
		float BorderNoise;
		float BorderSmooth;

		/** @brief Terrain height follows the landscape mesh's triangulation instead of bilinear, which averages the two and sits below both. Claims the retired BorderTrampledFade slot, so the buffer layout is untouched. */
		float ShellTriHeight;
		float BorderUntrampledFade;
		/** @brief Unused: the object/landscape seam cross-fade was removed. Layout keeper, uploaded as 0. */
		/** @brief Cull landscape-shell patches whose depth has reached the -8 bare floor everywhere. Claims the retired SeamFadeUnused slot, so the buffer layout is untouched. */
		float ShellCullBare;
		/** @brief Camera-distance band (world units) over which the statics skin dissolves back to the object's own material; start of the fade and the hard end (the capture range). */
		float SkinFadeStart;

		float SkinFadeEnd;
		/** @brief Also the enable gate for the object height field in the shader (>0 = field bound). */
		float ObjectLiftCap;
		float2 ObjectHeightCenter;

		float ObjectHeightHalfExtent;
		/** @brief Raw cascade-atlas copies are bound at t22/t23 this frame (else the shader falls back to the blurred VSM path). */
		float CrispShadows;
		/** @brief Screen-Space Shadows output bound at t45: the long-range depth-marched shadows carrying distant LOD tree shadows beyond the cascades. */
		float ScreenSpaceShadowsActive;
		/** @brief Dune-field amplitude in world units (0 flattens the undulation). */
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

		/** @brief Churn lump amplitude in world units on disturbed snow. */
		float ChurnHeightAmp;
		/** @brief Multiplier on the churn lump wavelengths. */
		float ChurnSizeScale;
		/** @brief C3 A/B: >0.5 disables the camera-keyed far-field height pad in ShellSurfaceZ. Reuses a retired keeper row, so the CB layout is unchanged. */
		float DebugNoFarPad;
		/** @brief C3 A/B: >0.5 disables the coarse-lattice data morph in ShellSurfaceZ. Same retired keeper row. */
		float DebugNoDataMorph;

		/** @brief Object-snow variants of the trench-detail knobs (independent of the landscape set). The two ObjCrisp rows are RETIRED layout keepers like the pair above. */
		float ObjBermHeightAmp;
		float ObjChurnHeightAmp;
		float ObjChurnSizeScale;
		float ObjCrispScaleV;

		float ObjCrispStrengthV;
		/** @brief Distant-snow diagnostics: 0 off, 1 depth-delta heatmap (histogram at u1), 2 warp-ring view, 3 data-provenance view. */
		uint ShellLODDebug;
		/** @brief 1/width of the seam depth ramp; 0 = no seam data this frame (span fade only). */
		float SeamRampInv;
		/** @brief >0.5: the shells read the baked berm field (t14) instead of recomputing its 17 taps per call. */
		float BermBakeActive;

		/** @brief Loaded-cell boundary square (minX, minY, maxX, maxY): the shell ends here and the horizon recolor takes over. */
		float4 SeamBounds;

		/** @brief Wide exclusion field window: xy = world centre, z = 1/half extent, w > 0.5 when the field was baked this frame. */
		float4 ExclusionFieldWindow;

		/** @brief Parallax on the shells: x = HeightScale (the PBR JSON displacementScale, 1:1 with landscape now that kSnowUVTile matches), y = self-shadow strength (0 disables the taps), z = occlusion depth multiplier (0 disables the march, landscape shell only), w = coarse march steps. */
		float4 SnowParallax;

		/** @brief x = how dark a shock discharge burns the snow it struck, y = crust shading strength, z = crust roughness, w = how far crust flattens the snow normal map. Mirrored in SnowShell.hlsl's ShellCB - see CLAUDE.md on constant buffers being the silent collision. */
		float4 SpellShading;
		/** @brief x = reflectance of fully crusted snow, yz = red and green of its colour cast, w = grazing-angle sheen strength. Mirror any change in SnowShell.hlsl. */
		float4 CrustLook;
		/** @brief x = blue of the crust colour cast. Mirror any change in SnowShell.hlsl. */
		float4 CrustLook2;
		/** @brief x > 0.5 = outward dust beyond the committed edge (0 = clean binary cut); y = minimum snow on carved trench floors in units above terrain; zw = atlas slices of sun cascades 0/1 (the shared atlas moves the sun's slices with the active-light set, and the PS crisp path needs the real indices). Mirror any change in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 BorderStyle;
		/** @brief x = compaction glint suppression (Stage 1); y = shell-surface SSS re-march, PACKED: integer part 0 off / 1 on / 2 on + thickness streak fix, fraction * 1000 = caster height cap in units; zw = dynamic-resolution scale for its screen-space taps (the shell pass does not bind FrameBuffer b12). One constant serves both shells. Mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 CompactLook;
		/** @brief Stage 3: x = P5 rim lip height (fraction of local depth), y = P5 rim teeth strength, z = P6 berm clod amplitude (world units), w spare. xy consumed inside CarveProfile; z at the berm sites. Appended LAST; mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 RimStyle;

		/** @brief Baked undulation window (UndulationFieldCS, t29): xy = world centre, z = 1/half-extent, w > 0.5 when the bake is live. Mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 UndulationFieldWindow;

		/** @brief Toroidal deformation-map addressing: physical position of logical texel (0,0). Every DeformationMap Load adds this and masks by dim-1. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		DirectX::XMINT2 DeformMapOrigin;
		/** @brief x: bit 0 = heightfield horizon self-shadow march on (Settings::ShellHorizonMarch). Took the torus pad; layout unchanged. Mirror in SnowShell.hlsl AND SnowStaticsShell.hlsl. */
		DirectX::XMINT2 ShellFlags;
		/** @brief Land-exact height layer: xy = GridOrigin - fine window origin, z = fine dim (0 = none), w = fine texel size. */
		float4 FineWindow;
	};
	STATIC_ASSERT_ALIGNAS_16(ShellCB);

	/**
	 * @brief Per-light shadow data for shadow-casting local lights (t38), indexed by the light's shadow-mask channel.
	 *
	 * Mirrors PointShadowLight in SnowShadow.hlsli. LightType 0 marks an empty slot; the table is left empty
	 * whenever the shadow-atlas copies are unavailable, so the shader falls back to unshadowed light.
	 */
	struct PointShadowLightData
	{
		DirectX::XMFLOAT4X4 LightTransform;
		uint32_t SliceIndex;
		uint32_t LightType;  ///< 0 empty, 1 spot, 2 paraboloid, 3 dual paraboloid
		float pad[2];
	};
	static constexpr uint32_t kPointShadowMaxLights = 4;

	/**
	 * @brief Draws the snow shell into the deferred G-buffer.
	 *
	 * Called from Deferred::DeferredPasses before the composite, while the
	 * frame's G-buffer contents and main depth are complete. Binds its own
	 * targets/state and restores the previous pipeline state afterwards.
	 * Implemented in SnowDeformation/Shell.cpp.
	 */
	void DrawShell();

	/** @brief Returns the shell vertex/pixel shaders, compiling them on first use. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11VertexShader* GetShellVS();
	ID3D11PixelShader* GetShellPS();
	ID3D11VertexShader* shellVS = nullptr;
	ID3D11PixelShader* shellPS = nullptr;

	/** @brief Index buffers for the non-tessellated grid draws (legacy main path, shadow caster): one per union-jack parity, absolute 32-bit lattice indices, one draw. DrawShellGrid routes to the indexed draw or, under the A/B toggle, the old six-vertices-per-quad Draw. Implemented in SnowDeformation/Shell.cpp. */
	bool EnsureShellGridIndexBuffers();
	void DrawShellGridIndexed(ID3D11DeviceContext* a_context, const ShellCB& a_cb);
	void DrawShellGrid(ID3D11DeviceContext* a_context, const ShellCB& a_cb);
	winrt::com_ptr<ID3D11Buffer> shellGridIB[2];
	/** @brief A/B measurement: the non-indexed grid draws (SNOW_GRID_NONINDEXED VS variants) for reading what the index buffers are worth. Runtime-only. */
	bool shellGridNonIndexed = false;
	ID3D11VertexShader* shellVSNonIndexed = nullptr;
	ID3D11VertexShader* shellShadowVSNonIndexed = nullptr;

	/** @brief Heatmap-mode PS permutation (SNOW_LOD_HISTOGRAM): single SV_Target + histogram UAV at u1. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11PixelShader* GetShellLODPS();
	ID3D11PixelShader* shellLODPS = nullptr;


	/** @brief Tessellated-path stages (SNOW_TESS): control-point VS (grid placement only), hull shader (distance-based crack-free factors) and domain shader (full surface evaluation + displacement-map relief). Active when Relief Depth > 0. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11VertexShader* GetShellTessVS();
	ID3D11HullShader* GetShellHS(bool a_bake);
	ID3D11DomainShader* GetShellDS();
	ID3D11VertexShader* shellTessVS = nullptr;
	ID3D11HullShader* shellHS = nullptr;
	/** @brief SNOW_HS_BAKE twin: the edge factors' two corner deformation taps come from shellVertexBake.w; the midpoint tap stays live. */
	ID3D11HullShader* shellHSBake = nullptr;
	ID3D11DomainShader* shellDS = nullptr;
	/** @brief Measurement: replaces the ShellShadowCast profiler row with per-cascade CasterGrid/CasterSkins/CasterPatch rows. Runtime-only. */
	bool shellCasterSplitDebug = false;

	/** @brief A/B measurement: draws every captured skin into every cascade, as before the per-cascade sphere cull. Runtime-only. */
	bool casterCullDisabled = false;
	/** @brief Last frame's caster cull census (summed over cascades), for the debug readout. */
	uint32_t casterSkinsCulled = 0;
	uint32_t casterSkinsDrawn = 0;
	uint32_t casterSkinsCulledLast = 0;
	uint32_t casterSkinsDrawnLast = 0;

	/** @brief A/B measurement: SNOW_DS_FLAT domain shader (terrain + class depth, no field work) bounds the geometry stages' share of the Shell row. Runtime-only. */
	bool shellFlatDSDebug = false;
	ID3D11DomainShader* shellDSFlat = nullptr;

	/** @brief Per-vertex surface bake: BakeCS runs ShellVertexZ once per base-grid vertex into shellVertexBake (RGBA32F, (GridDim+1)^2) each frame; the SNOW_DS_BAKE domain shader reads patch corners from it by grid index and keeps interior (tess factor > 1) vertices live. Same function, full float, index lookup: bit-identical to the live path. SNOW_DS_BAKE_CHECK evaluates both and spikes any corner whose bits differ. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11ComputeShader* GetShellBakeCS();
	ID3D11DomainShader* GetShellDSBake(bool a_check);
	bool EnsureShellVertexBake();
	ID3D11ComputeShader* shellBakeCS = nullptr;
	ID3D11DomainShader* shellDSBake = nullptr;
	ID3D11DomainShader* shellDSBakeCheck = nullptr;
	Texture2D* shellVertexBake = nullptr;
	/** @brief Companion to shellVertexBake: the vertex normal's two height differences, so a baked corner skips FinishShellVertex's four terrain taps as well. */
	Texture2D* shellVertexBakeSlope = nullptr;
	/** @brief A/B measurement: returns the domain shader to evaluating every vertex live. Runtime-only. */
	bool shellVertexBakeDisabled = false;
	/** @brief A/B measurement: stops the hull's view-frustum patch cull (ShellFlags.x bit 1), so the whole grid is tessellated and rasterised as before. The cull is bit-identical - the rasteriser discards those triangles anyway - and it does NOT reach the shadow caster, which draws from the light's matrix through its own non-tessellated VS. Runtime-only. */
	bool shellFrustumCullDisabled = false;
	/** @brief Measurement: the domain shader evaluates baked corners live as well and lifts any mismatching vertex by 50 units. Runtime-only. */
	bool shellVertexBakeCheck = false;

	/** @brief Measurement: D3D11 pipeline-statistics + occlusion queries around the shell grid draws and the statics pass. PS invocations over samples passed is overdraw x overshade; DS/HS/VS invocations check the geometry-stage arithmetic. Ring of three, read back without flushing. Runtime-only. */
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
	bool shellStatsIssued[kShellStatsRing] = {};
	int shellStatsRing = 0;
	ShellStatsResult shellStatsLast;
	ShellStatsResult staticsStatsLast;
	bool EnsureShellStatsQueries();
	void ReadShellStatsQueries(ID3D11DeviceContext* a_context);
	/** @brief Hull variants for the split draw: NEAR keeps patches inside the PS's far-clamp distance, FAR keeps the rest. A straddling patch lands in exactly one of them. */
	ID3D11HullShader* shellHSNear = nullptr;
	ID3D11HullShader* shellHSFar = nullptr;
	ID3D11HullShader* shellHSNearBake = nullptr;
	ID3D11HullShader* shellHSFarBake = nullptr;
	ID3D11HullShader* GetShellHSNear(bool a_bake);
	ID3D11HullShader* GetShellHSFar(bool a_bake);
	/** @brief Shell PS with the depth export compiled out, used for the split's NEAR pass while the clamp is on. Distinct from shellPS, which follows the clamp A/B flag. */
	ID3D11PixelShader* shellPSNoDepth = nullptr;
	ID3D11PixelShader* GetShellPSNoDepth();

	/** @brief Depth prepass: SNOW_SHELL_DEPTH_PREPASS writes the shell's clamped depth into the main buffer (alpha cut + export clamp) and its raster depth into shellRasterDepth; the fill pass copies that into shellTestDepth, a private depth buffer the shading pass tests EQUAL against with writes off, so occluded, self-hidden and alpha-cut fragments are rejected by hardware early-Z. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11PixelShader* GetShellPSPrepass();
	ID3D11PixelShader* shellPSPrepass = nullptr;
	ID3D11VertexShader* GetShellFillVS();
	ID3D11PixelShader* GetShellFillPS();
	ID3D11VertexShader* shellFillVS = nullptr;
	ID3D11PixelShader* shellFillPS = nullptr;
	bool EnsurePrepassResources(ID3D11ShaderResourceView* a_mainDepthSRV);
	Texture2D* shellRasterDepth = nullptr;
	winrt::com_ptr<ID3D11Texture2D> shellTestDepth;
	winrt::com_ptr<ID3D11DepthStencilView> shellTestDepthDSV;
	/** @brief Read view of shellTestDepth for the object-snow prepass's write-back into the main depth. */
	winrt::com_ptr<ID3D11ShaderResourceView> shellTestDepthSRV;
	winrt::com_ptr<ID3D11DepthStencilState> shellPrepassMainDepthState;
	winrt::com_ptr<ID3D11DepthStencilState> shellFillDepthState;
	/** @brief A/B measurement: returns the shell to the near/far split draws without the depth prepass. Runtime-only. */
	bool shellDepthPrepassDisabled = false;

	ConstantBuffer* shellCB = nullptr;

	/** @brief SSGI seam shield: analytic contact-fringe mask handed to the deferred composite (t16/b7) so it can lift SSGI's AO on the ground ring just beyond the shell edge, where the discarded shell cannot shield via Masks2. Bound from Deferred's composite dispatch; implemented in SnowDeformation/Shell.cpp. */
	struct SeamShieldCB
	{
		float2 WindowOffset;
		float TexelSize;
		float Dim;
		float4 Params;  ///< x = lift strength, w > 0.5 = active this frame
	};
	void BindSeamShield();
	ConstantBuffer* seamShieldCB = nullptr;

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

	/** @brief One actor's push crest for this frame. Rebuilt every frame from live position and velocity - nothing persists, so nothing can be left behind (the retired spray's failure). */
	struct BowWave
	{
		float2 pos{};
		/** @brief Previous foot position - the crest rides this capsule, exactly as the trench stamp does. */
		float2 prev{};
		float2 dir{};
		float radius = 24.0f;
		float strength = 0.0f;
		float distSq = 0.0f;
	};
	/** @brief Layout must match BowWaveCB in SnowShell.hlsl (b1 of the shell pass). Its own buffer: ShellCB is hand-mirrored across two shaders and this is landscape-only. */
	struct BowWaveCB
	{
		float4 BowWaveParams;
		float4 BowWaveLook;
	};
	ConstantBuffer* bowWaveCB = nullptr;
	std::vector<BowWave> bowWaves;
	/** @brief Smoothed speed + last travel direction per actor, so the crest eases in and OUT rather than snapping off the instant someone stops - the "settles after a brief moment" half of the design - and keeps pointing the right way while it eases. xy = unit direction, z = smoothed speed. Keyed by formID. */
	std::unordered_map<uint32_t, float4> bowWaveSpeed;
	/** @brief Uploads this frame's crests to b1 of the shell pass. */
	void UpdateBowWaveBuffer();

	/** @brief Layout must match ArcCB in LightningArc.hlsl. Its own buffer, NOT part of ShellCB, which is hand-mirrored across two shaders and must not grow for a cosmetic pass. */
	struct ArcCB
	{
		Matrix CameraViewProj;
		float4 ArcCameraPosAdjust;
		float4 ArcFrom;
		float4 ArcTo;
		float4 ArcParams;
		float4 ArcTint;
	};
	ConstantBuffer* arcCB = nullptr;
	ID3D11VertexShader* arcVS = nullptr;
	ID3D11PixelShader* arcPS = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> arcTextureSRV;
	winrt::com_ptr<ID3D11BlendState> arcBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> arcDepthState;
	winrt::com_ptr<ID3D11RasterizerState> arcRasterState;
	winrt::com_ptr<ID3D11SamplerState> arcSampler;
	bool arcTextureAttempted = false;
	std::string arcTextureLoaded;

	/** @brief Loads a DDS through the GAME's resource system, so archives resolve and a modlist override still wins. Shared by the snow, frost and arc textures - a second copy would drift. */
	static bool LoadGameDDS(const std::string& a_dataRelativePath, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv);
	ID3D11VertexShader* GetLightningArcVS();
	ID3D11PixelShader* GetLightningArcPS();
	bool EnsureLightningArcResources();
	void EnsureLightningArcTexture();

	/** @brief Records an arc for this frame's draw. Called from the shock cloak's own discharge, which already knows both ends. */
	void EmitLightningArc(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
	/** @brief Ages the live arcs and drops the spent ones. */
	void UpdateLightningArcs(float a_deltaTime);
	/** @brief Draws the live arcs. Called AFTER the deferred composite - an emissive overlay lit by nothing, which is what a bolt is. */
	void DrawLightningArcs();
	winrt::com_ptr<ID3D11RasterizerState> shellRasterState;
	/** @brief Skin draws' raster state: the shell's plus SkinDepthBias / SkinSlopeDepthBias, rebuilt when either slider moves. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11RasterizerState* GetSkinRasterState();
	winrt::com_ptr<ID3D11RasterizerState> skinRasterState;
	float skinRasterBiasBuilt = 0.0f;
	float skinRasterSlopeBuilt = 0.0f;
	winrt::com_ptr<ID3D11DepthStencilState> shellDepthState;

	/** @brief Returns the depth sync compute shader (shell depth -> Terrain Blending's blended depth copies), compiling it on first use. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11ComputeShader* GetDepthSyncCS();
	ID3D11ComputeShader* depthSyncCS = nullptr;

	/** @brief Renders the shell as an always-visible plane colored by the sampled terrain data (red=height, green=coverage, blue=ramp depth). Runtime-only diagnostic. */
	bool shellDataDebug = false;
	/** @brief Border-field debug plane (ShellDebugData 3): hue = class-depth band, brightness = snow grain, white = the cut contour, magenta grid = land grain data present. */
	bool shellBorderDebug = false;
	/** @brief SSS gate view (ShellDebugData 4): red = mask darkness from the ground march, green = vertical-hug trust, blue = buried-caster probe fired. Renders on the REAL shell surface with real discards. */
	bool shellSSSDebug = false;
	/** @brief Wall material view (ShellDebugData 5): raw two-plane albedo, unlit, red wash = side-projection weight. The strafe test for the wall-shift hunt. */
	bool shellWallDebug = false;
	/** @brief Debug plane mode 2: paints the exclusion channels (R = drift lift, G = melt, B = suppression). */
	bool shellExclusionDebug = false;

	/** @brief A/B measurement: gates the wide exclusion field off, returning clearings to the near mask's ~57 m reach. Runtime-only. */
	bool shellDistantExclusionsDisabled = false;

	/** @brief A/B measurement: skips the berm field bake and returns the shells to recomputing the 17-tap average per pixel. Runtime-only; the shell renders the same either way. */
	bool shellBermBakeDisabled = false;
	/** @brief Debug A/B: zeroes UndulationFieldWindow.w so both shells fall back to the live two-octave eval; the bake keeps updating underneath. */
	bool shellUndulationBakeDisabled = false;
	/** @brief Debug A/B: restores the horizon march's bicubic deformation sampler (16 loads/tap) in place of the shipped bilinear (4). Rekeys both shell PS variants via SNOW_MARCH_BICUBIC. */
	bool shellMarchBicubicRestored = false;
	bool shellMarchBicubicCompiledPS = false;
	bool shellMarchBicubicCompiledPSNoDepth = false;

	/** @brief Clamp state the cached shellPS was compiled against; a mismatch releases it. The clamp is a debug A/B now (shellDepthClampDisabled), not a setting. */
	bool shellDepthClampCompiled = true;


	/** @brief A/B measurement: forces the shell back to one draw with the depth export, so the split's win can be read against it. Runtime-only. */
	bool shellSplitDisabled = false;

	/** @brief A/B measurement: draws the shells through the viewport bound at DrawShell time (the deferred span's decal cap) instead of the main pass's depth range, so the range fix can be read against the ~3e-5 NDC bias it removed. Runtime-only. */
	bool shellMainViewportRangeDisabled = false;

	/** @brief A/B measurement: drops the shell's SV_DepthLessEqual export outright (single no-export draw, no far-field clamp). Demoted from a setting 2026-08-27: with the split draw on by default there is no configuration where turning the clamp off is a good trade, so it is an instrument, not a choice. Runtime-only; forces a PS recompile. */
	bool shellDepthClampDisabled = false;

	/** @brief A/B measurement: returns terrain height to plain bilinear, which averages the mesh's two triangulations and so sits below both. Turning it on should bring back the poke-through on steep ground. Runtime-only. */
	bool shellBilinearHeight = false;

	/** @brief A/B measurement: drops the statics PS's SV_Depth export. UPPER BOUND only - the carve reads that depth, so the surviving pixel set differs. Not shippable; stays a debug toggle. Runtime-only; forces a PS recompile. */
	bool staticsEarlyZSpike = false;

	/** @brief Object-snow debug view: skins and trench patch render decision variables as colors with dithering disabled. Runtime-only diagnostic. */
	/** @brief 0 off, 1 edge-taper masks, 2 coverage alpha (see the PS debug block). */
	int staticsDebugView = 0;

	// ---- Distant-snow / LOD diagnostics (runtime-only) ----

	/** @brief 0 off, 1 depth-delta heatmap, 2 warp-ring view, 3 data-provenance view, 4 land-exact delta. Mirrors ShellCB::ShellLODDebug. */
	int lodDebugView = 0;
	/** @brief Shimmer meter: a probe CS evaluates the shell mesh surface at world-anchored points each frame; the CPU tracks frame-to-frame height deltas per distance band. */
	bool lodShimmerMeter = false;
	/** @brief C3 discriminator D2: drop the camera-keyed far-field height pad. Expect holes back if it was carrying them. */
	bool lodDebugNoFarPad = false;
	/** @brief C3 discriminator D3: drop the coarse-lattice data morph, leaving the fine lattice everywhere. */
	bool lodDebugNoDataMorph = false;

	static constexpr uint32_t kLODHistBands = 4;
	static constexpr uint32_t kLODHistBuckets = 8;
	static constexpr uint32_t kLODProbeAzimuths = 24;
	static constexpr uint32_t kLODProbeRadii = 12;
	static constexpr uint32_t kLODProbeCount = kLODProbeAzimuths * kLODProbeRadii;
	/** @brief Probe ring radii in world units; must match kProbeRadius in SnowShell.hlsl. */
	static constexpr float kLODProbeRadius[kLODProbeRadii] = { 1500, 2500, 3500, 5000, 6500, 8000, 10000, 12500, 15000, 18000, 21000, 24000 };
	static constexpr uint32_t kLODShimmerHistory = 120;

	/** @brief Heatmap-mode depth state: test ALWAYS, write off. The PS re-creates occlusion, so poke-under pixels survive to be measured. */
	winrt::com_ptr<ID3D11DepthStencilState> shellLODDepthState;
	winrt::com_ptr<ID3D11Buffer> lodHistogram;
	winrt::com_ptr<ID3D11UnorderedAccessView> lodHistogramUAV;
	winrt::com_ptr<ID3D11Buffer> lodHistogramStaging[2];
	winrt::com_ptr<ID3D11Buffer> lodProbeBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> lodProbeUAV;
	winrt::com_ptr<ID3D11Buffer> lodProbeStaging[2];
	/** @brief Two-deep staging ring: frame N copies into [ring], maps [ring^1] (frame N-1's copy) so readback never stalls. */
	int lodReadbackRing = 0;
	bool lodHistStagingValid[2] = {};
	bool lodProbeStagingValid[2] = {};
	/** @brief Probe anchor (512-unit-quantized camera XY) captured at each dispatch; deltas are only valid between readbacks sharing an anchor. */
	float2 lodProbeAnchorAtCopy[2] = {};

	uint32_t lodHistData[kLODHistBands * kLODHistBuckets] = {};
	float lodProbePrev[kLODProbeCount] = {};
	bool lodProbePrevValid = false;
	float2 lodProbeAnchor = { 0.0f, 0.0f };
	/** @brief Per-band shimmer stats from the last readback: max |dZ|, mean |dZ|, probes moving > 1 unit, valid probe count. */
	float lodShimmerMax[kLODHistBands] = {};
	float lodShimmerAvg[kLODHistBands] = {};
	uint32_t lodShimmerHops[kLODHistBands] = {};
	uint32_t lodShimmerValid[kLODHistBands] = {};

	// Windowed shimmer accumulators: the per-frame row is a sample, not a
	// measurement. Reset, walk, then read - a screenshot of one frame is what
	// voided the first C3 A/B round.
	float lodShimmerRunMax[kLODHistBands] = {};
	double lodShimmerRunSum[kLODHistBands] = {};
	uint64_t lodShimmerRunCnt[kLODHistBands] = {};
	uint32_t lodShimmerRunHops[kLODHistBands] = {};
	uint32_t lodShimmerRunFrames = 0;
	/** @brief Seam square (SeamBounds) rewrites: it snaps to the PLAYER's cell, so this ticks on cell crossings. C3 mechanism 3's event counter. */
	uint32_t lodSeamChanges = 0;
	/** @brief Terrain data window rebuilds: a whole-window re-upload of baked heights. C3's other discrete-event suspect. */
	uint32_t lodWindowRebuilds = 0;
	float lodShimmerHistoryBuf[kLODHistBands][kLODShimmerHistory] = {};
	int lodShimmerHistoryIdx = 0;

	/** @brief Lazily creates the LOD-diagnostic GPU resources. Implemented in SnowDeformation/Shell.cpp. */
	bool EnsureLODDebugResources();
	/** @brief Probe CS (COMPUTESHADER block of SnowShell.hlsl), compiled on first use. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11ComputeShader* GetLODProbeCS();
	ID3D11ComputeShader* lodProbeCS = nullptr;
	/** @brief Dispatches the probe CS (when the shimmer meter is on), reads back last frame's histogram + probe copies and updates the stats. Implemented in SnowDeformation/Shell.cpp. */
	void RunLODProbePass();
	void ReadbackLODDiagnostics();

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

	/** @brief Registers a land texture and returns its registry index, or kNoLandTexture for an absent one. Implemented in SnowDeformation/TerrainData.cpp. */
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
	struct CapturedSnowStatic
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		/** @brief Road/bridge match: this capture uses RoadMeshesDepth, so the model class cannot be split across a road model's trishapes. */
		bool road;
		/** @brief Matched on "bridge" rather than "road". Held apart from `road` only to keep bridges out of the road heightfield (S0): their deck is elevated, and the stamp map has no z channel to tell a deck trail from the ground below it (#9e). */
		bool bridge;
		/** @brief Glacier/iceberg family: captured past the Object Snow range cap and exempt from the SkinFade distance dissolve — their own baked snow never matches the shell, so the skin must persist at every loaded distance. */
		bool fadeExempt;
		/** @brief projectedUVParams.w from the draw's property, -1 without kProjectedUV; see StaticsCB::ProjThreshold. */
		float projThreshold;
		/** @brief projectedUVParams.x - vanilla's noise-term strength; 0 without projection data. */
		float projNoiseScale;
		/** @brief projectedUVParams.z - the noise map's world-space tiling; 0 without projection data. */
		float projNoiseTiling;
		/** @brief Mountain/cliff family by geometry name: force the ROUNDED class. The divergence-only flat classifier reads a jagged low-poly cliff's split normals as "plate" and drapes it with a rigid vertical lift of the full flat depth - the hovering sheet Josef reported as a translucent film. Deterministic name match (road-class precedent), NOT a stats heuristic (those were tried and rejected, see SmoothNormalsCS FlatStatsCS). */
		bool forceRounded;
		/** @brief Plank family by geometry name (plank/walkway/catwalk). Currently DECIDES NOTHING - Josef's round-5 call classifies ALL PD draws rounded; the match is kept (and logged once per name) for the future cornice treatment when the 3D shell returns. */
		bool plankFamily;
		/** @brief The property really carries kProjectedUV (projThreshold read from it). False for the mesh-replacer default (threshold 0, no noise), whose reconstructed weight is a guess the coat and the edge lumps must not trust. */
		bool projReal;
	};

	/** @brief Render-thread only: filled during opaque rendering by the SetupGeometry hook, consumed and cleared each frame. */
	std::vector<CapturedSnowStatic> capturedStatics;
	std::unordered_set<void*> capturedStaticsSet;
	std::atomic<uint32_t> statCapturedStatics{ 0 };

	/** @brief Per-frame projected-snow classification counters (render thread writes, Prepass publishes, menu debug section reads). */
	std::atomic<uint32_t> statProjMatched{ 0 };
	std::atomic<uint32_t> statProjNoProjection{ 0 };
	std::atomic<uint32_t> statProjVetoed{ 0 };
	uint32_t statProjMatchedPrev = 0;
	uint32_t statProjNoProjectionPrev = 0;
	uint32_t statProjVetoedPrev = 0;

	/** @brief Records projected-snow lighting draws for the statics skin. Called from the BSLightingShader::SetupGeometry hook. Implemented in SnowDeformation/Statics.cpp. */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

	/** @brief Publishes the per-pass SnowProjectedIsSnow permutation bit, classified from the pass technique (currentRawTechnique), and re-binds t102/t103 for classified draws. Must run BEFORE the game's SetupGeometry, which consumes the descriptor. Implemented in SnowDeformation/Statics.cpp. */
	void SetProjectedSnowBit(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass);

	/** @brief Lists this frame's captured statics whose bounds cover a world position, largest first: what the object snow is skinning there. Implemented in SnowDeformation/Statics.cpp. */
	ObjectSnowProbe ProbeObjectSnow(float a_x, float a_y);
	/** @brief Installs the SetupGeometry capture hook. Called from PostPostLoad; implemented in SnowDeformation/Statics.cpp. */
	void InstallStaticsCaptureHook();

	/** @brief Pre-shell copy of the MASKS target: Masks.y carries the land's EM grain height (Lighting.hlsl LANDSCAPE; 0 = no data) for the shell's two-sided edge contest, readable only before the shell overwrites the G-buffer. Bound at t10 on the shell PS. */
	winrt::com_ptr<ID3D11Texture2D> landMasksCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> landMasksCopySRV;

	/** @brief Pre-shell copy of the NORMALROUGHNESS target: the scene's per-pixel shaded normals (normal maps included) before any shell overwrote them - the S4 shell's per-pixel footprint cut reads its nz here. Taken only while ObjectSnow3D is on with a nonzero depth; bound at skin PS t23. */
	winrt::com_ptr<ID3D11Texture2D> preSkinNormalsCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> preSkinNormalsCopySRV;

	/** @brief Copies the resource behind a_srcSRV into an owned SRV-only texture, recreating it when dimensions or format change. The SRV doubles as the validity signal (nulled by callers on invalid frames), so it is rebuilt even when the texture itself is still current. Implemented in SnowDeformation/Shell.cpp. */
	static void CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
		winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv);

	// ---- Sun shadows on the shells: crisp cascade receiver + caster ----

	/** @brief Full-resolution COPY of the game's raw sun-shadow cascade atlas, taken during the shadow-mask pass. The copy is mandatory: by deferred time the engine has reused the live target, and sampling it live produces garbage flicker. Taken after the shell is injected as a caster, so the snowfield receives its own banks' shadows; acne is held off by the caster's depth push. */
	winrt::com_ptr<ID3D11Texture2D> shadowAtlasCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> shadowAtlasCopySRV;
	/** @brief LESS_EQUAL comparison sampler for the atlas copies (s2). */
	winrt::com_ptr<ID3D11SamplerState> shadowCmpSampler;
	/** @brief Linear-clamp sampler standing in as ShadowSampling.hlsli's LinearSampler (s1). */
	winrt::com_ptr<ID3D11SamplerState> shellLinearSampler;

	/** @brief Called from State::Draw while the game renders the shadow MASK (Utility shader, RenderShadowmask); the only point where PS t4 genuinely holds the sun cascade atlas (at any other time it holds whatever texture the last draw bound). Copies it and the ESRAM partner for crisp shell shadows, then injects the shell as a caster. Same trigger VolumetricShadows and Skylighting use. Implemented in SnowDeformation/Shadows.cpp. */
	void CaptureShadowAtlas();

	/** @brief SNOW_SHADOW_CAST shell VS variant: flattens the base layer (sunk below terrain) so only excess height; mounds, drifts; casts. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11VertexShader* GetShellShadowVS();

	/** @brief Lazily compiles the trench patch's shadow-caster VS. Implemented in SnowDeformation/Statics.cpp. */
	ID3D11VertexShader* GetPatchShadowVS();
	ID3D11VertexShader* shellShadowVS = nullptr;

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

	/** @brief Depth-renders the terrain shell into both live cascade slices so the world receives snow-mound shadows. The statics skins deliberately do NOT cast: a skin hovers a few units above its object's own surface, so the object beneath always reads as shadowed by its own snow cap. Called from CaptureShadowAtlas after the receiver copies are taken. Implemented in SnowDeformation/Shadows.cpp. */
	void InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV);

	/** @brief Re-derives the camera-anchored grid placement fields of a ShellCB from THIS frame's camera. Shared by DrawShell and the shadow-caster injection so the caster geometry never lags the visible surface by a frame (a stale grid slides a crisp shadow during camera motion). Implemented in SnowDeformation/Shell.cpp. */
	void RefreshShellGridPlacement(ShellCB& a_cb);

	/** @brief Shadow-source diagnostics for the settings UI (kept permanently; they answer "where do this scene's shadows come from" without a debugger): cascade descriptor count, the three end-split distances, and the copied atlas's slice count. */
	uint32_t dbgLodDescriptorCount = 0;
	float dbgLodEndSplits[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t dbgLodAtlasSlices = 0;

	/** @brief Queries adapter VRAM usage/budget in MB via IDXGIAdapter3 (zeros when unavailable). Usage above budget = driver demotion to system RAM = the large persistent FPS-loss mode that survives disabling features. */
	void QueryAdapterVRAM(uint64_t& a_usageMB, uint64_t& a_budgetMB);
	/** @brief Sums this feature's tracked GPU textures (approximate, from descriptors) and fills a per-category breakdown line. */
	uint64_t SumFeatureTextureBytes(std::string& a_breakdown);
	/** @brief Periodic VRAM log from Prepass: fires on a ~90 s cadence, on large usage deltas, and immediately (as a warning) when usage exceeds budget. */
	void TickVRAMLog();
	uint64_t vramTickCounter = 0;
	uint64_t vramLastLoggedMB = 0;
	uint64_t vramLastLogTick = 0;

	/** @brief 4-entry structured buffer of PointShadowLightData (t38), uploaded each frame from the mask-time snapshots. */
	Buffer* pointShadowLights = nullptr;

	/** @brief Copy of the LOCAL lights' shadow atlas (t39), taken while a local light's mask renders; the local maps do NOT live in the sun cascade atlas, and by deferred time the engine has returned the live target. */
	winrt::com_ptr<ID3D11Texture2D> pointShadowAtlasCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> pointShadowAtlasCopySRV;

	/** @brief Per-channel descriptor snapshots, merged at each local mask pass; entries persist until their channel is reassigned (an extinguished light's entry is never sampled: its cluster light loses the Shadow flag). */
	PointShadowLightData pendingPointShadows[kPointShadowMaxLights] = {};
	/** @brief Frame index incremented in Prepass; per-slice latches so each light's slice is copied once per frame. */
	uint64_t pointShadowFrameIndex = 0;
	uint64_t pointShadowSliceFrame[kPointShadowMaxLights] = {};

	/** @brief Called from State::Draw while the game renders a LOCAL light's shadow mask (Utility RenderShadowmaskSpot/Pb/Dpb): the only moment the light's descriptor is live (renderTarget reads -1 once the engine returns the maps) and PS t4 genuinely holds the local atlas. Copies the atlas once per frame and snapshots every live local descriptor. Implemented in SnowDeformation/Shadows.cpp. */
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
		/** @brief World-unit distance by which the skin's geometric height has collapsed to zero at the deepest class; the material dissolve (SkinFadeStart/End) continues past it. */
		float SkinHeightFadeEnd;
		/** @brief >0.5: this draw keeps the tuned pre-rework skin behaviour (road and bridge meshes); set from the capture's road flag. */
		float LegacySkin;
		/** @brief Angle of repose (1.0 = 45 degrees) from SnowMoundSteepness; sets how far inside the silhouette the lift tapers out. */
		float MoundSteepness;
		/** @brief >0.5: this draw may be trenched. Roads always may; other objects are gated by Settings::ObjectTrenches. */
		float ObjectTrenches;
		float padDistantBareness;
		/** @brief >0.5: skip the SkinFadeStart/End distance dissolve (glacier/iceberg captures). Mirror in SnowStaticsShell.hlsl. */
		float FadeExempt;
		/** @brief >0.5: road heightfield active. On capture and skin draws it also means THIS draw is a road-heightfield object (road, not bridge), so the capture writes the per-texel road bit and the skin steps aside; on the patch draw it is the global gate. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float RoadField;
		/** @brief Vanilla projected-UV mask threshold (BSLightingShaderProperty::projectedUVParams.w) for this draw; -1 when the property carries no kProjectedUV (or kTreeAnim, whose vertex alpha is wind weight, not a snow mask). Feeds the S0 debug view and the S2 placement suppressor (SKIN-PLACEMENT-PLAN.md). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjThreshold;
		/** @brief >0.5: Settings::ProjMaskPlacement - ApplySkinLift multiplies its up-facing mask by the authored projected-snow term. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjMaskEnable;
		/** @brief >0.5: Settings::ProjDepthDensity - the graded density factor replaces the sharp gate. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjDensityEnable;
		/** @brief Class override code: 0 = flat classifier decides, 1 = force ROUNDED (CapturedSnowStatic::forceRounded, and every PD draw in authored-relief mode), 2 = force FLAT (plankFamily in authored-relief mode). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ClassOverride;
		/** @brief CapturedSnowStatic::projNoiseScale (projectedUVParams.x) - strength of vanilla's projected-noise term. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjNoiseScale;
		/** @brief Snow Fill, 0..1 (ProjSnowFillPct / 100) - the S4 shell grows only on the fill's angular slice; carried in StaticsCB because b6 is not bound to the skin VS/DS. Took the retired OpaqueCoverage slot. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjSnowFillSk;
		/** @brief CapturedSnowStatic::projNoiseTiling (projectedUVParams.z) - the noise map's world-space tiling. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjNoiseTiling;
		/** @brief 2 = the S4 shell owns this draw (SKIN-PLACEMENT-PLAN S4 phase 1): the rolling-ball fillet grown vertically over the fill-covered slice of the projected footprint, per-pixel coverage from the reconstructed vanilla weight. 0 = classic path (no projection data, or a road). Encoded as 2 so the shader's >1.5 tests survive any future middle state. Requires the noise map at t21. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ProjPixelEnable;
		/** @brief >0.5: preSkinNormalsCopySRV bound at skin PS t23 - the per-pixel nz for the authored-relief coverage cut comes from the scene's own shaded normal (normal maps included). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float HasSkinNormalCopy;
		/** @brief cos(Settings::ShellMaxSlopeDeg): minimum normal Z that grows the S4 shell (the shell's up-facing gate, user-tunable). Grew the CB a row. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float ShellMinNz;
		/** @brief Settings::PlaneMergeHeight - surfaces within this many units below a peeled layer's top belong to that layer's plane (the peel tolerance, user-tunable). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float PeelTol;
		/** @brief Settings::OverheadClearance - cover more than this far above a vertex neither splits its plane nor demotes it to a peeled layer. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float OverheadIgnore;
		/** @brief Settings::MeldCoPlanar for the skin: >0.5 lets side faces at MELDED boundaries lift (the vertical snow closing the slit between co-planar shells). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float MeldPlanesSk;

		/** @brief Settings::PileHeightRatio - a dome may stand at most this many times the repose height its footprint supports (the cone value); thin features saturate early instead of stretching fins. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float PileHeightRatio;
		/** @brief Settings::SkyExposurePct / 100 - strength of the P3 sky-exposure depth weighting (took a padPile slot; layout unchanged). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float SkyExposureSk;
		float padCorniceLip;
		float padBreakup;
		float padWeld;

		/** @brief >0.5: landMasksCopySRV is bound at the skin PS (the recolor's real projected weight, Masks.y = 2 + w on classified statics). Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float HasSkinMasksCopy;
		/** @brief Settings::SkinEdgeLumpSize - lump cell-size multiplier. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float EdgeBreakupScale;
		/** @brief Settings::SkinEdgeFlankWidth. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float EdgeFlankWidth;
		/** @brief Settings::ProjSnowMatch as 0/1: the skin coats the solidly painted projected snow with its own material near the camera. Mirror in SnowStaticsShell.hlsl and SnowHeightCapture.hlsl. */
		float EdgeCoat;
		float PadStatics0;
		float PadStatics1;
		float PadStatics2;
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
	};
	STATIC_ASSERT_ALIGNAS_16(SmoothCB);

	/** @brief Per unique geometry (keyed by vertex buffer pointer): position-averaged normals, built once by SmoothNormalsCS. Split-normal flat meshes (planks, roofs, pole caps) inflate along these so their snow drapes as a sealed pillow instead of a hovering parallel sheet. */
	struct SmoothedNormalsEntry
	{
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool ready = false;
		/** @brief Slot in meshBounds holding this mesh's local box (MESHBOUNDS pass), UINT32_MAX when none. */
		uint32_t boundsSlot = UINT32_MAX;
		/** @brief Cluster range in clusterBounds and the mesh's slice of clusterIndexPool (CLUSTERBOUNDS pass); count 0 when the mesh got none and its skins draw their own index buffer. */
		uint32_t clusterOffset = 0;
		uint32_t clusterCount = 0;
		uint32_t indexPoolOffset = 0;
		uint32_t indexCount = 0;
	};
	/** @brief Local boxes of the unique meshes, two float4 per slot (min, max), built beside the smoothed normals; the skin cull projects them. */
	static constexpr uint32_t kMeshBoundsSlots = 1024;
	Buffer* meshBounds = nullptr;
	uint32_t meshBoundsNext = 0;
	ID3D11ComputeShader* smoothBoundsCS = nullptr;
	uint32_t SmoothedBoundsSlot(void* a_vertexBuffer) const;

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
	uint32_t clusterNext = 0;
	uint32_t clusterIndexPoolNext = 0;
	/** @brief A/B measurement: stops the per-cluster pass, so every skin the whole-skin test kept draws its mesh entire. Runtime-only. */
	bool clusterCullDisabled = false;

	/** @brief One-shot debug probe: snapshots the next full-detail landscape draw's index and vertex buffers and reports how the land mesh splits its quads - which diagonal, and whether it alternates - on the world 128-unit lattice. The shell's own triangulation has to match it or the shell sags inside a quad (DISTANT-SHELL-STABILITY-PLAN, Stage 1's live caveat). Implemented in SnowDeformation/Statics.cpp. */
	bool landTriProbeArmed = false;
	struct LandTriProbe
	{
		winrt::com_ptr<ID3D11Buffer> vbStaging;
		winrt::com_ptr<ID3D11Buffer> ibStaging;
		uint32_t vertexCount = 0;
		uint32_t indexCount = 0;
		uint32_t stride = 0;
		uint32_t indexBytes = 2;
		bool posFloat32 = false;
		RE::NiTransform world{};
		std::string name;
		uint32_t frame = 0;
		bool pending = false;
	};
	LandTriProbe landTriProbe;
	std::string landTriProbeResult;
	void CaptureLandTriProbe(RE::BSRenderPass* a_pass);
	void ServiceLandTriProbe();
	/** @brief One-shot diagnostic for the land-exact height layer: reads the fine texture and the terrain window back and checks the window against the baked cells, the fine texture against a CPU replica of TerrainFineCS, and the shader's fine lookup along the camera's forward line against the same rule evaluated straight from the cells. Implemented in SnowDeformation/TerrainData.cpp. */
	bool fineProbeArmed = false;
	std::string fineProbeResult;
	void ProbeFineLayer(const ShellCB& a_cb);
	uint32_t clusterSkinsLast = 0;
	uint32_t clusterScratchUsedLast = 0;
	std::unordered_map<void*, SmoothedNormalsEntry> smoothedNormalsCache;
	ID3D11ComputeShader* smoothAccumulateCS = nullptr;
	ID3D11ComputeShader* smoothResolveCS = nullptr;
	/** @brief Third pass: one-group reduction writing the mesh's flat/rounded classification (fraction of smoothed-vs-raw divergent vertices) into the stats element appended at SmoothedNormals[vertexCount]. */
	ID3D11ComputeShader* smoothFlatStatsCS = nullptr;
	ConstantBuffer* smoothCB = nullptr;

	/** @brief Builds (or returns) the smoothed-normal buffer for a captured geometry. Dispatches the SmoothNormalsCS passes on first sight; cached thereafter. Returns null while unavailable (the VS falls back to raw normals). Implemented in SnowDeformation/Statics.cpp. */
	ID3D11ShaderResourceView* EnsureSmoothedNormals(RE::BSGeometry* a_geometry);
	/** @brief ONE StaticsCB recipe for both the visible skin draw and the shadow caster - the caster must be the exact surface the shell renders, and a drifted copy of this fill would be the CB-mirror class of bug. Presence flags come from the call site (the caster looks resources up without creating them). */
	void FillSkinDrawCB(const CapturedSnowStatic& a_cap, bool a_s4Shell, float a_vertexCount, bool a_hasSmoothedNormals, bool a_hasObjectTop, bool a_hasSkinNormalCopy, StaticsCB& a_scb) const;
	/** @brief ONE StaticsCB recipe for the trench patch, shared by the visible patch draw and its shadow caster for the same drift reason as FillSkinDrawCB. Implemented in SnowDeformation/Statics.cpp. */
	void FillPatchDrawCB(StaticsCB& a_scb) const;

	// ---- Top-down object height windows ----

	// 4096 units at 4-unit texels, following the camera. Halving the texel
	// (512 -> 1024) shrank the visible raster squares on the patch top
	// sheet; kHeightTexel in SnowStaticsShell.hlsl must match.
	// Dim and half-extent move together: the 4-unit texel is baked into
	// kHeightTexel in SnowStaticsShell.hlsl.
	static constexpr uint kHeightMapDim = 2048;
	static constexpr float kHeightMapHalfExtent = 4096.0f;
	/** @brief Height sentinels for texels no object covers. */
	static constexpr float kHeightMapEmptyTop = -100000.0f;
	static constexpr float kHeightMapEmptyBottom = 100000.0f;

	/** @brief Raised snow more than this far above the terrain does not lift the height field (buildings must not become snow tents). Also doubles as the shader-side field-enable gate. */
	static constexpr float kObjectLiftCap = 150.0f;

	/** @brief Ping-pong accumulated raw maps (scrolled each frame, captures rasterized on top): object TOP and BOTTOM surfaces. Persistence matters; the capture list is frustum-culled, and a map rebuilt from it alone loses every object behind the camera. */
	Texture2D* heightTopRaw[2] = { nullptr, nullptr };
	Texture2D* heightBottomRaw[2] = { nullptr, nullptr };
	/** @brief Processed maps the shell samples (t4/t5): the slope-limited snow-height field and the smooth shelter/suppression mask, plus a cone-iteration scratch. */
	Texture2D* heightTopFiltered = nullptr;
	Texture2D* heightBottomFiltered = nullptr;
	Texture2D* heightScratch = nullptr;
	/** @brief Cone-transformed snow SURFACE height over the object top raster; the skin's edge taper reads it with one tap. */
	Texture2D* objectSnowCone = nullptr;
	/** @brief S4 phase 2 - the PEELED second layer: ping-pong accumulated raw top of the highest surface more than kPeelTol below layer 1 per column, plus its own repose cone. A vertex whose height matches layer 2 takes its roll from here (SKIN-PLACEMENT-PLAN, layered top peeling). Skin VS/DS t24 (top) and t26 (cone). */
	Texture2D* heightTop2Raw[2] = { nullptr, nullptr };
	Texture2D* objectSnowCone2 = nullptr;
	/** @brief K=3: the third peeled layer (roof over beam over floor), same shape as layer 2. Skin VS/DS t27 (top) and t28 (cone). */
	Texture2D* heightTop3Raw[2] = { nullptr, nullptr };
	Texture2D* objectSnowCone3 = nullptr;
	/** @brief P3: per-column sky openness (1 = open sky) baked from the layer-1 tops at half the raster's resolution. Skin VS/DS + caster t25. */
	Texture2D* objectSkyOpen = nullptr;
	// ---- Height-field probe (Debugging Options): the six object maps read
	// back at the player's texel every frame, so a report carries numbers
	// instead of guesses. Ping-pong staging; the value shown is one frame old.
	winrt::com_ptr<ID3D11Texture2D> probeStaging[2];
	uint probeCursor = 0;
	/** @brief Sampled values: 0 = L1 top, 1 = L2 top, 2 = L3 top, 3 = cone1, 4 = cone2, 5 = cone3. Sentinels pass through raw. */
	float probeVals[6] = {};
	bool probeValid = false;
	float3 probeWorldPos = {};
	/** @brief Per-frame skin-depth raster (R16F, cleared each frame, MAX-blended): each captured mesh writes its class layer depth, so consumers know how thick the snow above any object top is. No scroll persistence; a missed frame is invisible for one frame. */
	Texture2D* heightSkinDepth = nullptr;
	uint heightCurrent = 0;
	bool heightMapValid = false;
	float2 heightWindowCenter = { 0, 0 };
	/** @brief Depth range of the game's main-pass viewport, sampled once per frame by the capture hook. The deferred span ends on the blended-decal viewport (max depth 0.999968 against the pass's 0.999998), and a shell drawn through that cap sits ~3e-5 NDC nearer than its own object. */
	float mainViewportMinDepth = 0.0f;
	float mainViewportMaxDepth = 1.0f;
	uint32_t mainViewportFrame = UINT32_MAX;

	/** @brief RT0 MAX (tops) + RT1 MIN (bottoms) + RT2 MAX (skin depth) in one raster pass: highest/lowest surfaces win per texel in any draw order; no depth buffer needed. */
	winrt::com_ptr<ID3D11BlendState> heightMaxBlendState;
	ID3D11VertexShader* heightVS = nullptr;
	ID3D11PixelShader* heightPS = nullptr;
	/** @brief S4 phase 2: the layer-2 peel PS (SnowHeightCapture.hlsl, PEEL define) - keeps only up-facing fragments below this frame's layer-1 top by the peel tolerance, MAX-blending the second-highest snow-bearing surface per column. */
	ID3D11PixelShader* heightPeelPS = nullptr;
	/** @brief K=3: the layer-3 peel PS (PEEL2 define) - additionally requires a known layer 2 and a height below it. */
	ID3D11PixelShader* heightPeel2PS = nullptr;
	/** @brief Depth-only skin caster VS (SHADOWCAST define): the full lift, clip position through the light matrix ShellCB carries during the cascade injection. Drawn by InjectShellShadowCasters so the object shells cast real sun shadows. */
	ID3D11VertexShader* skinShadowVS = nullptr;
	ID3D11ComputeShader* heightScrollCS = nullptr;
	ID3D11ComputeShader* heightCombineCS = nullptr;
	ID3D11ComputeShader* heightConeCS = nullptr;
	ID3D11ComputeShader* objectConeSeedCS = nullptr;
	ID3D11ComputeShader* objectConeCS = nullptr;
	/** @brief P3: bakes the half-res sky-openness field from the layer-1 tops (ObjectSkyOpenCS). */
	ID3D11ComputeShader* objectSkyOpenCS = nullptr;
	/** @brief P4: one Jacobi settling iteration over a cone depth field (ObjectConeDiffuseCS). */
	ID3D11ComputeShader* objectConeDiffuseCS = nullptr;

	/** @brief Per-dispatch constants for the height-window processing. Layout must match HeightProcessCB in HeightMapProcessCS.hlsl. */
	struct alignas(16) HeightProcessCB
	{
		DirectX::XMINT2 ScrollDelta;
		uint ClearAll;
		/** @brief Texel step for the current cone iteration. */
		uint ConeStep;

		float2 HeightWindowCenter;
		float HeightHalfExtent;
		/** @brief Max field rise per world unit (1.0 = 45 degrees), from SnowMoundSteepness. */
		float SlopePerUnit;

		/** @brief Terrain window addressing so the compute passes can sample ground heights. */
		float2 TerrainWindowOrigin;
		float TerrainTexelSize;
		uint TerrainDim;

		/** @brief Units/frame the accumulated tops/bottoms drift toward empty; stale object imprints (disabled/moved/harvested) melt instead of persisting until scrolled out. */
		float GhostDecay;
		/** @brief Rounded-class snow depth, seeding the object snow cone. */
		float ObjectSnowDepth;
		/** @brief Settings::PlaneSplitStep - the cone seed's slope-discontinuity rim threshold (user-tunable). */
		float RimStep;
		/** @brief Settings::OverheadClearance - the seed's rise-rim upper bound: surfaces further above do not split the plane WHEN this plane continues beneath them (the next layer's top says); a silhouette edge against tall cover still rims. */
		float OverheadIgnore;

		/** @brief Settings::MeldCoPlanar - >0.5: the seed's drop-bridge reaches 3 texels so co-planar surfaces a sliver apart meld; 0: no bridging, every shell clings to its own raster edge. */
		float MeldPlanes;
		/** @brief P4 "Snow Settling": per-iteration Jacobi blend toward the 4-neighbour average over the finished cone fields (Settings::SnowSettlingPct / 100 * 0.5; 0 = off). */
		float DiffuseLambda;
		float padHeight[2];
	};
	STATIC_ASSERT_ALIGNAS_16(HeightProcessCB);

	ConstantBuffer* heightProcessCB = nullptr;

	// ---- Exclusion zones: bare-by-design clearings in the snow field ----

	/** @brief Doors get elliptical clears stretched along their facing (load doors; cave and building entrances; larger) that fade coverage to bare ground. Fires get noisy-edged MELT BASINS instead: the shell's depth thins toward a small floor that never vanishes and never sinks below terrain (negative values in the shelter-mask channel; see CombineCS). */
	/** @brief Must match MAX_EXCLUSIONS in HeightMapProcessCS.hlsl. When the gather overflows, the nearest sources win (distance sort in the gather). */
	static constexpr uint kMaxExclusions = 256;
	static constexpr float kDoorClearRadius = 110.0f;
	static constexpr float kDoorForwardExtent = 70.0f;
	static constexpr float kLoadDoorClearRadius = 150.0f;
	static constexpr float kLoadDoorForwardExtent = 150.0f;
	/** @brief Melt circle around a dropped burning torch. Carried torches never melt: a moving basin warps the bearer's own trails. */
	static constexpr float kTorchClearRadius = 40.0f;
	/** @brief Living actors this far above the LAND height stand on an elevated structure and do not stamp (2D-map interim gate; drift tops below this keep trails). */
	static constexpr float kElevatedStampCutoff = 70.0f;

	/** @brief Sentinel in the skin-depth raster's G channel (road top): no road drew in this column. Mirrored as kNoRoadTop in SnowHeightCapture.hlsl and SnowStaticsShell.hlsl. */
	static constexpr float kNoRoadTop = -1000000.0f;

	/** @brief Trench-patch grid quads per axis. Mirrors kPatchGridDim in SnowStaticsShell.hlsl, whose band table it is derived from: 2 x (128 + 8 + 8 + 8 + 18). */
	static constexpr uint32_t kPatchGridDim = 340;
	/** @brief World-unit snap for the patch centre. MUST be the coarsest band step in use (kPatchBandMul's last entry x kPatchStep = 16 x 8), or vertices stop landing on their band's lattice and quad widths flip as the camera moves - the invariant SnowGrid.hlsli warns about. */
	static constexpr float kPatchSnap = 128.0f;
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
		float FieldHalfExtent;
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
	/** @brief Bakes the wide exclusion field from the gathered exclusion list. Implemented in SnowDeformation/Statics.cpp. */
	void RenderExclusionField();
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
	ConstantBuffer* doorsCB = nullptr;
	uint32_t doorRefreshCounter = 0;
	/** @brief Cadence-gathered exclusions (doors, campfires, heat sources, dropped burning torches). */
	std::vector<std::pair<float4, float4>> staticExclusions;
	/** @brief Survival Mode's Survival_WarmUpObjectsList when the plugin is present: base objects that count as heat sources. */
	RE::BGSListForm* survivalHeatSources = nullptr;
	bool survivalHeatSourcesResolved = false;
	/** @brief Exclusions uploaded this frame (static + carried torches), for the debug readout. */
	uint32_t statExclusionCount = 0;

	/** @brief Creates the height-window textures. Implemented in SnowDeformation/Statics.cpp. */
	void CreateHeightFieldResources();
	/** @brief Scrolls the accumulated height maps to the new window position and rasterizes this frame's captured statics into them (MAX/MIN). Called from DrawShell before the screen-space passes. Implemented in SnowDeformation/Statics.cpp. */
	void RenderObjectHeightMap();

	ID3D11VertexShader* staticsVS = nullptr;
	ID3D11PixelShader* staticsPS = nullptr;
	/** @brief Statics PS with the SV_Depth export compiled out, chosen per draw for captures that cannot carve. Only the parallax carve pushes depth, and it is gated on ObjectTrenches or the draw being a road. */
	ID3D11PixelShader* staticsPSNoDepth = nullptr;
	/** @brief Depth-prepass twin of the no-export shader (SNOW_STATICS_DEPTH_PREPASS): the alpha cut with no colour and no export. Non-carving skins draw once with it into a private copy of the scene depth, then once with the shipping shader under EQUAL against it, so hardware early-Z admits exactly the pixels the prepass wrote; carving draws keep their own export and LESS_EQUAL in the shading loop, since a shader-computed depth cannot be matched against a second compile of itself; the private depth is then written back. Bit-identical by construction: both passes run the same raster path. */
	ID3D11PixelShader* staticsPSPrepassNoDepth = nullptr;
	/** @brief A/B measurement: returns the object skins to their single draw loop. Runtime-only. */
	bool staticsDepthPrepassDisabled = false;
	/** @brief Whole-skin occlusion cull (DepthSyncCS.hlsl HiZBuildCS / SkinCullCS): a max-depth pyramid of the scene depth, one compute thread per skin testing its bounding sphere (worldBound + lift margin) against it, and per-skin DrawIndexedInstancedIndirect arguments with instance count 0 or 1. Skins outside the view or wholly behind the scene never enter the pipeline; both prepass and shading loops read the same arguments. Conservative, so bit-identical. Implemented in SnowDeformation/Statics.cpp. */
	bool skinCullDisabled = false;
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
	static constexpr int kSkinCullRing = 3;
	/** @brief World-unit growth of the bound beyond the class depth: cornice roll, relief, the toward-eye flank push. */
	static constexpr float kSkinCullMargin = 24.0f;
	/** @brief Depth-space margin: the skin rasterizer bias is clamped at 1e-5 (DepthBiasClamp), plus projection rounding. */
	static constexpr float kSkinCullDepthEps = 1.5e-5f;
	ConstantBuffer* skinCullCB = nullptr;
	Buffer* skinCullBounds = nullptr;
	Buffer* skinCullArgs = nullptr;
	uint32_t skinCullCapacity = 0;
	winrt::com_ptr<ID3D11Buffer> skinCullArgsStaging[kSkinCullRing];
	bool skinCullStagingIssued[kSkinCullRing] = {};
	uint32_t skinCullStagingCount[kSkinCullRing] = {};
	int skinCullRing = 0;
	Texture2D* skinCullHiZ = nullptr;
	/** @brief One single-mip scratch per level: the build reads level k-1 of the chain and writes scratch k, then copies it into the chain. A texture cannot be an input and an output of the same dispatch, mips included. */
	std::vector<Texture2D*> skinCullHiZScratch;
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> skinCullHiZSRVs;
	uint32_t skinCullLevels = 0;
	uint32_t skinCullDrawnLast = 0;
	uint32_t skinCullCulledLast = 0;
	uint32_t skinCullTrisDrawnLast = 0;
	uint32_t skinCullTrisTotalLast = 0;
	/** @brief Per-reason census (SkinCullCS reason codes 0-5) and the pyramid's 1x1 top level, read back through 1x1 staging textures: 0 there means the pyramid is dead. */
	uint32_t skinCullReasonLast[8] = {};
	float skinCullHiZTopLast = -1.0f;
	winrt::com_ptr<ID3D11Texture2D> skinCullHiZTopStaging[kSkinCullRing];
	ID3D11ComputeShader* GetHiZBuildCS();
	ID3D11ComputeShader* GetSkinCullCS();
	ID3D11ComputeShader* hiZBuildCS = nullptr;
	ID3D11ComputeShader* skinCullCS = nullptr;
	bool EnsureSkinCullResources(uint32_t a_count, ID3D11ShaderResourceView* a_mainDepthSRV);
	/** @brief Tessellated skin stages (optional; legacy path is the fallback): control-point VS, hull (edge-length/distance factors) and domain (displacement-map relief along the inflate normal). */
	ID3D11VertexShader* staticsTessVS = nullptr;
	ID3D11HullShader* staticsHS = nullptr;
	ID3D11DomainShader* staticsDS = nullptr;
	/** @brief Trench patch (PATCH define): the landscape shell's dense-grid carve applied to OBJECT tops; real geometry where parallax cannot notch silhouettes or hold floors angle-stably. */
	ID3D11VertexShader* patchVS = nullptr;
	/** @brief Depth-only caster variant of the patch VS (PATCH + SNOW_SHADOW_CAST); drawn into the sun cascade slices by InjectShellShadowCasters. */
	ID3D11VertexShader* patchShadowVS = nullptr;
	/** @brief Tessellated patch stages (optional; legacy path is the fallback). */
	ID3D11VertexShader* patchTessVS = nullptr;
	ID3D11HullShader* patchHS = nullptr;
	ID3D11DomainShader* patchDS = nullptr;
	ID3D11PixelShader* patchPS = nullptr;
	/** @brief Retained VS bytecode: input layouts are created against it, one per vertex descriptor. */
	winrt::com_ptr<ID3DBlob> staticsVSBlob;
	bool staticsShadersFailed = false;
	ConstantBuffer* staticsCB = nullptr;
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> staticsILCache;
	/** @brief Input layout for a vertex descriptor, created on first sight against the statics VS (POSITION+NORMAL; any VS reading a subset binds to it). Null is cached for descriptors that cannot be laid out. */
	ID3D11InputLayout* StaticsInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc);

	/** @brief Prop contact field (MESH-CONTACT-PLAN Route B, rigid half): moving props' render meshes rasterized from above, MIN-blended world Z per column, over a fine window around the deformation centre. Per frame, never accumulated; the stamp pass carves from it per texel. */
	Texture2D* contactHeight = nullptr;
	winrt::com_ptr<ID3D11BlendState> contactMinBlendState;
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
		/** @brief Living actor whose feet and body have not moved since last frame: its print is already in the map, so its skin is not drawn (carried gear still is, when it moves). */
		bool still = false;
	};
	/** @brief This frame's rasterized props, gathered by the prop scan; their collision shapes stay out of the stamp list. */
	std::vector<ContactProp> contactProps;
	/** @brief Skin instances whose partition layout has been logged once (capped), so the log states whether partitions share a buffer rather than the code assuming it. */
	std::unordered_set<const void*> contactSkinLogged;
	float2 contactCenter = { 0, 0 };
	uint contactDrawsLast = 0;
	/** @brief A/B, runtime-only, default ON: moving props inside the contact window carve by their render mesh. Off reverts them to collision-shape stamps. */
	bool debugContactCapture = true;
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
	ID3D11InputLayout* ContactSkinInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc);
	/** @brief This frame's actors drawn by their skinned meshes; their bone stamps are skipped so the A/B compares like for like. */
	std::vector<ContactProp> contactActors;
	/** @brief This frame's living and corpse entries in contactActors, against their separate caps. */
	uint contactLivingCount = 0;
	uint contactCorpseCount = 0;
	/** @brief Per-geometry scratch for the skinned contact palette: one composed transform per skin bone, built on first use and shared by every partition of the geometry. */
	std::vector<RE::NiTransform> contactPaletteScratch;
	std::vector<uint8_t> contactPaletteBuilt;
	/** @brief S1 spike, runtime-only, default OFF: actors carve by their skinned render mesh instead of foot and limb capsules. */
	bool debugActorContact = true;
	/** @brief Runtime-only: the contact field as the carve pass reads it (ContactViewCS into an RGBA8 the menu shows with the player's bound overlaid). S1's debug view: the silhouette's shape, extent and placement in one image. */
	bool debugContactView = false;
	/** @brief Runtime-only: half-extent of the field view's crop around the player, world units (192 = a body, 1536 = the whole field). */
	float debugContactViewHalf = 192.0f;
	/** @brief Runtime-only A/B: when off, no living body counts as still and every rasterized actor draws every frame. */
	bool debugContactStillGate = true;
	/** @brief Runtime-only: index of the one skinned geometry the actor contact pass draws (-1 = all), and its name. */
	int debugContactSolo = -1;
	std::string contactSoloName;
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
	/** @brief Rasterized living actors that stood still this frame and were not drawn. */
	uint contactStillLast = 0;
	/** @brief Skin partitions the actor contact pass declined this frame: dismember partitions the game hides, or partitions whose bones resolve to nothing. */
	uint contactHiddenPartsLast = 0;
	/** @brief Per skin instance, how its vertices index bones: 1 = each partition's own list, 2 = the skin's full list. Audited once from the raw vertex copy. Identity key only, never dereferenced. */
	std::unordered_map<const void*, uint8_t> contactSkinIndexing;
	/** @brief Skinned partitions drawn this frame from a whole-skin palette because their vertices index the skin's full bone list. */
	uint contactGlobalPartsLast = 0;
	/** @brief A foot or body that moved less than this since last frame counts as still. */
	static constexpr float kContactStillStep = 1.0f;
	/** @brief Extra sub-step draws issued this frame so fast gear sweeps instead of printing at intervals. */
	uint contactSweepLast = 0;
	/** @brief Previous frame's world transform per carried mesh, keyed by geometry, for the sweep. Identity only - never dereferenced. */
	struct ContactSweep
	{
		RE::NiTransform world;
		uint32_t frame = 0;
	};
	std::unordered_map<const void*, ContactSweep> contactSweepStates;
	uint32_t contactSweepFrame = 0;
	/** @brief World units a sweeping mesh may advance between sub-steps (two contact texels), and the ceiling on sub-steps for one mesh. */
	static constexpr float kContactSweepStep = 6.0f;
	static constexpr uint32_t kContactMaxSweep = 8;
	std::unordered_set<const void*> contactSkinMissingLogged;
	/** @brief Skinned geometries the contact pass refused because the game itself hides them, reported once each. */
	std::unordered_set<const void*> contactSkinHiddenLogged;
	/** @brief Margin over the layer depth before a part is refused as unreachable: the terrain window's bilinear ground can sit this far off the actor's feet on a slope. */
	static constexpr float kContactSkipMargin = 16.0f;
	/** @brief Debug view crop centre and the map texel size the last update used, for the menu's overlay. */
	float2 contactViewCenter{};
	float contactViewTexelSize = 0.0f;
	bool EnsureContactResources();
	void DrawContactCapture(ID3D11DeviceContext* a_context);

	/** @brief Compiles the statics skin VS (keeping bytecode) and PS on first use. Implemented in SnowDeformation/Statics.cpp. */
	bool EnsureStaticsShaders();
	/** @brief Re-draws this frame's captured projected-snow statics inflated, inside DrawShell's bound state. Implemented in SnowDeformation/Statics.cpp. */
	void DrawCapturedStatics();

	/** @brief Caches a "tile is snow material" bitmask per landscape quad material, for the terrain shader's per-tile snow detection. */
	void TESObjectLAND_SetupMaterial(RE::TESObjectLAND* land);
	/** @brief Publishes the cached snow mask for the material about to be drawn via ExtraFeatureDescriptor bits 10-15. */
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);

	/** @brief Diagnostics shown in the settings UI: landscape materials that hit/missed the snow-mask cache at draw time. */
	std::atomic<uint64_t> landMaskHits{ 0 };
	std::atomic<uint64_t> landMaskMisses{ 0 };

	/** @brief Thread-safe size read for the settings UI. */
	size_t snowMasksSizeForUI()
	{
		const std::shared_lock lock(snowMaskMutex);
		return snowMasks.size();
	}

	/** @brief Runtime-only diagnostic toggle; not persisted in settings JSON. */
	bool debugTerrainOverlay = false;

	/** @brief Debug melt emitter: a stationary heat source dropped from the menu at the player, exercising the additive stamp path before any spell detection exists. Runtime-only, never persisted. */
	bool debugMeltEmitterActive = false;
	/** @brief World position of the dropped emitter. */
	RE::NiPoint3 debugMeltEmitterPos{};
	/** @brief Emitter radius in world units (300 = kFireClearRadius, the campfire benchmark). */
	float debugMeltEmitterRadius = 100.0f;
	/** @brief Emitter accumulation rate in depth units per second at the core. */
	float debugMeltEmitterRate = 0.50f;
	/** @brief Runtime-only: land-UV / 256-unit / cell gridlines on terrain, for measuring the landscape texture's world-space repeat against kSnowUVTile. */
	bool debugTilingRuler = false;
	/** @brief Tints classified projected-snow pixels magenta (DebugTerrainOverlay bit 4) so the SnowProjectedIsSnow bit is verifiable in-game without a capture. */
	bool debugProjSnowView = false;
	/** @brief Fill instrument (DebugTerrainOverlay bit 16): the slice Snow Fill covers renders cyan inside the projected-snow recolor, so raising the slider visibly converts the debug purple. Not serialized, like every debug view. */
	bool debugProjFillView = false;
	/** @brief DebugTerrainOverlay bit 32: the Lighting recolor paints its real blend weight as a grey ramp and projected draws it does not classify in red. Hold against the object snow debug view's Projected mask mode (R = the skin's reconstruction). */
	bool debugProjWeightView = false;
	/** @brief Tints classified baked-snow (glacier) pixels cyan (DebugTerrainOverlay bit 8), same verification pattern as the projected view. */

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
	/** @brief WindowHasSnow over the deformation window. False means nothing can read the map here, so its passes are skipped. Never-baked ground counts as possibly snowy, and the verdict lands in deformSnowVerdict for the debug readout. */
	bool DeformationWindowHasSnow() const { return WindowHasSnow(deformWorldSize * 0.5f, true, &deformSnowVerdict); }
	/** @brief WindowHasSnow over the shell's own footprint, which reaches far past the deformation window. False means no shell geometry can stand above ground, so nothing casts. */
	bool ShellFootprintHasSnow() const { return WindowHasSnow(ShellWarpedHalfSpan()); }
	/** @brief Last verdict from DeformationWindowHasSnow, for the debug menu. */
	mutable uint32_t deformSnowVerdict = kSnowGateUnknown;
	/** @brief Set while the deformation passes are being skipped, so resuming can force a clear instead of trusting an accumulated scroll delta. */
	bool deformSuspended = false;
	mutable std::shared_mutex shellCellMutex;
	std::atomic<bool> shellDataDirty{ true };
	/** @brief Landscape textures discovered by the bake; indices are stable for the session and are what the baked cells store. */
	std::vector<LandTextureEntry> landTextures;
	/** @brief Substring filter for the texture list. Runtime UI state, not persisted. */
	std::string textureFilter;
	std::unordered_map<uint32_t, uint16_t> landTextureByForm;
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
	/** @brief Last frame's berm A/B state: re-enabling the bake must run one update even at rest, or the shells read a bake from before the toggle went off. */
	bool prevBermBakeDisabled = false;
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
	/** @brief Changed-texel counts from the newest consumed verdict - the magnitude behind "map-active": a handful is a precision tail, millions is a logic bug. Per channel, the counts name the term (depth / melt-scorch / crust-deposit). */
	uint32_t deformChangedTexels = 0;
	uint32_t deformChangedDepth = 0;
	uint32_t deformChangedMelt = 0;
	uint32_t deformChangedCrustDep = 0;
	/** @brief Sum of the per-texel max delta (fixed-point 1e6). Mean = sum/count is a term's fingerprint: the slump step is SlumpRate x 0.5 x dt and scales with the slider. */
	uint32_t deformChangedDeltaSum = 0;
	/** @brief Bounding box of the changed texels (map coords), from the same verdict. Valid while count > 0. A few hundred churning texels are sub-pixel in the 512 view; the box is what makes them findable. */
	uint32_t deformChangedMinX = 0, deformChangedMinY = 0;
	uint32_t deformChangedMaxX = 0, deformChangedMaxY = 0;
	/** @brief Runtime-only: the update CS paints result-vs-carried per texel (R depth, G melt/scorch, B crust|deposit) into a map-sized RGBA8 shown in the menu. Answers WHERE the map claims to be evolving. */
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
		/** @brief Decay clock reading when these bytes were last brought up to date. Per tile, so the sweep can lag without ever being wrong. */
		float clock = 0.0f;
		/** @brief Game hours when this tile was last written or last stood inside the window. The LRU key. */
		float lastTouch = 0.0f;
		/** @brief Exact encoded size under kTrenchRLE, measured by the sweep. What the budget is spent in - a raw byte count would bound the wrong number. */
		uint32_t encodedBytes = 0;
	};

	/** @brief Per-tile co-save header: key, clock and last touch. The encoder's own bytes are counted separately. */
	static constexpr uint32_t kTrenchTileHeaderBytes = 24;
	/** @brief Store encoding, fixed here because the budget accounting must agree with the writer byte for byte: byte-oriented RLE, pairs of (count 1-255, value), a run longer than 255 split across pairs, and a tile kept raw if that ever exceeds its raw size. */
	static constexpr uint32_t kTrenchRLEPairBytes = 2;

	/** @brief Quantised depth per trodden tile. Untrodden ground has no entry, and a tile that decays to nothing is erased. */
	std::unordered_map<TrenchTileKey, TrenchTile, TrenchTileKeyHash> trenchTiles;

	/** @brief Monotonic "depth removed since the store began", in 0-1 depth units. A tile's decay is the difference between this and its own clock, which is what lets a tile sit out of the window for a week and come back correct. */
	float trenchDecayClock = 0.0f;

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
	};

	/** @brief R8 depth the update CS reads for texels the scroll brings in from outside the window. Only the arriving band is uploaded; in-window texels overwrite it from the previous map, so the rest may be stale. */
	winrt::com_ptr<ID3D11Texture2D> trenchInjectTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchInjectSRV;
	std::vector<uint8_t> trenchInjectScratch;

	/** @brief Single-channel copy of the map's displaced depth, for the menu. The map itself cannot be shown honestly: ImGui blends by alpha and the map's .w is the bow wave's deposit, so drawing it directly renders depth through deposit and reads blank right after a load. R8_UNORM samples as (depth,0,0,1), so this one cannot lie. */
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
	/** @brief Reads one mapped band into the store, splatting each map texel across the store texels it covers. */
	void StoreTrenchBand(const TrenchBandCopy& a_meta, const D3D11_MAPPED_SUBRESOURCE& a_mapped);
	/** @brief Bilinear store depth at a world position, crossing tile edges through the one-entry cache. */
	float SampleTrenchStore(uint32_t a_worldspace, float a_worldX, float a_worldY);
	/** @brief Co-save record for the trench tiles. '{@link kTrenchRecordVersion}' is written into every chunk; #33's accumulation claims its own type on the same channel. */
	static constexpr uint32_t kTrenchRecord = 'SNTR';
	static constexpr uint32_t kTrenchRecordVersion = 1;

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
	/** @brief Writes the scalar. */
	void SaveAccumulation(const SKSE::SerializationInterface* a_intfc);
	/** @brief Reads it back, refusing a version this build does not know. */
	void LoadAccumulation(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);

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
			name(a_name), start(std::chrono::steady_clock::now())
		{
			owner = a_owner->loadTraceActive.load(std::memory_order_acquire) ? a_owner : nullptr;
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
		std::chrono::steady_clock::time_point start;
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
	std::map<std::string, LoadTracePhase> loadTracePhases;
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

	/** @brief Packs one tile under the pinned RLE rule. Its length MUST equal the tile's measured encodedBytes, or Stage D's budget stops describing the save. */
	void EncodeTrenchTile(const TrenchTile& a_tile, std::vector<uint8_t>& o_bytes) const;
	/** @brief Unpacks a payload written by EncodeTrenchTile; false if it is malformed or the wrong length. */
	bool DecodeTrenchTile(const uint8_t* a_bytes, uint32_t a_length, std::vector<uint8_t>& o_depth) const;

	/** @brief Drops every stored tile and the cache that points into it. Takes the lock. */
	void ClearTrenchStore(const char* a_reason);
	/** @brief As ClearTrenchStore, for callers already holding the lock. */
	void ClearTrenchStoreLocked(const char* a_reason);
	/** @brief Advances the decay clock off game time, and drops the store on a backwards jump. */
	void TickTrenchClock();
	/** @brief Brings one tile's bytes up to the current clock. Returns false when nothing nonzero is left, i.e. the tile should be erased. */
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

public:
	/** @brief Applies pending range-setting changes (trench window resize + map clear). Called at Prepass start; the first call applies loaded settings. */
	void ApplyRangeSettings();

protected:
	/** @brief Trail history per collision shape: key = (formID << 16) | traversal index. */
	std::unordered_map<uint64_t, float2> stampPrevPositions;

	std::unordered_map<uint32_t, StampBones> stampBoneCache;

	/** @brief Per-frame stamp diagnostics for the menu (rebuilt in GatherStamps). */
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
		/** @brief Pits only: 0 marks a pocked disc, >0 marks a pocked RING at that fraction of the radius - a cloak crackles around its wearer rather than under them. */
		float ringFraction = 0.0f;
		/** @brief Crust only: multiplier on the glazing rate, carrying the source's magnitude and how far above the snow it sits. */
		float rateScale = 1.0f;
		/** @brief Carve a scooped hollow rather than a walled trench. A body hurled through snow leaves a furrow with sloped sides, not a slot. */
		bool bowl = false;
		/** @brief Read the pair of points as a WEDGE instead of a capsule: previous is the apex, position the far centre, radius the half-width there. Only shouts set it - a row of discs cannot stand in for a cone, because every stamp holds full depth across only the inner tenth of its radius and so reads as a row of craters. */
		bool cone = false;
	};

	/** @brief This frame's emitters, rebuilt by GatherSpellEmitters and consumed by GatherStamps. */
	std::vector<SpellEmitter> spellEmitters;

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

	/**
	 * @brief Raises one buried effect onto the snow above it, on the GAME thread.
	 *
	 * Detection runs on the render thread with every other detector; the write
	 * must not. Mutating the game's scene graph from a render pass is the shape
	 * of thing that crashed this feature repeatedly when it was only reading,
	 * so the lift goes through the SKSE task interface.
	 */
	void LiftRefOntoSnow(RE::TESObjectREFR* a_ref, float a_lift);

	/** @brief Hazards already raised, by formID, so the lift happens once rather than every frame. */
	std::unordered_set<uint32_t> liftedRefs;

	/** @brief The game's own frost impact pattern, painted onto crusted snow. Its normal map carries the crystal detail; the albedo is a faint whitening on top of it. */
	winrt::com_ptr<ID3D11ShaderResourceView> frostPatternNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> frostPatternDiffuseSRV;
	bool frostPatternAttempted = false;
	/** @brief Loads the frost pattern once, from the game's own impact decal art. */
	void EnsureFrostPatternTextures();

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
	 * A cone is not a capsule, so a shout cannot be one stamp. Laid down as a
	 * row of discs of growing radius along its axis instead, which needs no new
	 * stamp mode, shader change or constant-buffer field.
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
	/** @brief A/B, runtime-only, default ON: collision shapes stamp their ground silhouette (a capsule along the longest horizontal extent) instead of a bounding sphere. Off reproduces the sphere for comparison. */
	bool debugShapeFootprint = true;
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
		uint16_t alphaSettle = 0;
		float gapToLand = 0.0f;
		float floatingGap = 0.0f;
		/** @brief Nominal shell depth at the actor's XY (-1 if the cell is unbaked). Separates "stamps land but nothing here can display a trench" from skeleton problems. */
		float shellDepth = -1.0f;
		bool cellBaked = false;
	};
	SkeletonProbe skeletonProbe;

	/** @brief Last 3D-root position per loose prop (formID), rebuilt every frame from the in-range scan. The position gate runs before any collision traversal, so resting clutter costs one hash lookup per frame. */
	std::unordered_map<uint32_t, RE::NiPoint3> propPrevPositions;
	/** @brief Frames since the last full reference scan (0 = scan this frame). The scan runs every kPropScanInterval frames; between scans only propScanMovers and propScanHazards are revisited and anchors update in place. */
	uint32_t propScanFrame = 0;
	/** @brief Props the last full scan saw moving, revisited every frame until the next scan. */
	std::vector<RE::ObjectRefHandle> propScanMovers;
	/** @brief Placed hazards the last full scan found, replayed through ConsiderHazard every frame because the emitter list is rebuilt per frame. */
	std::vector<RE::ObjectRefHandle> propScanHazards;
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
};
