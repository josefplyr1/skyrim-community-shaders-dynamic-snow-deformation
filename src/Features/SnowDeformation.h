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

	// Square world-space deformation window following the camera in whole-texel
	// steps. Texel value = normalized depression depth, 0 = untouched snow,
	// 1 = compressed to the ground. World size is runtime (deformWorldSize),
	// so trench detail coarsens with range.
	static constexpr uint kTextureDim = 2048;
	static constexpr uint kMaxStamps = 256;
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
	 * Producers run in a fixed order - cloaks, then projectiles, then hazards,
	 * then blasts - so a cap enforced while gathering drops whatever happens to
	 * run last, wherever it is. A firewall burning across the valley outranks
	 * the bolt landing at your feet purely because cloaks are read first.
	 * Gather wide, then keep the nearest, which is what the exclusion field
	 * already does when it overflows.
	 */
	static constexpr size_t kSpellEmitterCeiling = 192;
	/**
	 * @brief Slots of kMaxStamps that only spells may take.
	 *
	 * Actors and props are gathered first and in engine order, so a mass
	 * ragdoll - a dozen bodies each stamping every limb - can fill all 256
	 * before one emitter is read, and a fireball at the player's feet then
	 * leaves nothing at all. Small enough that giving it up costs a few limb
	 * prints inside a pile nobody is looking at; large enough that every
	 * school still marks during the fight that caused the pile.
	 */
	static constexpr uint kSpellStampReserve = 48;

	/** @brief Skyrim world units per meter (1 unit â‰ˆ 1.43 cm). Range sliders are in meters. */
	static constexpr float kUnitsPerMeter = 70.0f;

	// ---- Snow shell: a camera-following grid of real snow geometry ----

	static constexpr uint kShellGridDim = 640;
	// 8-unit inner spacing: 16-unit vertices undersample the ~10-unit trench
	// walls into blocky silhouettes.
	static constexpr float kShellGridSpacing = 8.0f;

	// Distance warp: the inner kShellWarpInnerVerts vertices per side keep
	// linear kShellGridSpacing; beyond them each ring's spacing grows by
	// kShellWarpGrowth (~26k units half-span). Must match WarpAxis() in
	// SnowShell.hlsl.
	static constexpr float kShellWarpInnerVerts = 256.0f;
	static constexpr float kShellWarpGrowth = 1.0902f;

	/** @brief World half-span of the warped shell grid (center to edge) at a given inner spacing. Linear in spacing: the warp shape is unchanged. */
	static float ShellWarpedHalfSpan(float a_spacing = kShellGridSpacing)
	{
		const float outerVerts = kShellGridDim * 0.5f - kShellWarpInnerVerts;
		const float outer = kShellWarpGrowth * (std::pow(kShellWarpGrowth, outerVerts) - 1.0f) / (kShellWarpGrowth - 1.0f);
		return (kShellWarpInnerVerts + outer) * a_spacing;
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
	struct SnowClassDef
	{
		const char* label;
		const char* match;
		float defaultDepth;
	};
	static constexpr SnowClassDef kSnowClasses[kSnowClassCount] = {
		{ "Grass Snow", "grasssnow", 14.0f },
		{ "Trodden Path", "snowpath", 18.0f },
		{ "Snowy Rocks", "snowrocks", 30.0f },
		{ "Snow 01", "snow01", 30.0f },
		{ "Snow 02", "snow02", 30.0f },
		{ "Roads", "road", -5.0f },
		{ "Dirt", "dirt", -5.0f },
		{ "Grass & Fields", "grass", -5.0f },
		{ "Rocks & Cliffs", "rock", -5.0f },
		{ "Coast & Beach", "coast", -5.0f },
		{ "Mud & Rivers", "mud", -5.0f },
		{ "Other", "", -5.0f },
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
	};

	struct Settings
	{
		bool EnableSnowDeformation = true;
		bool ShowDebugTexture = false;
		/** @brief Scale on Havok collision-shape radii (20 = 1.0x = the shapes' actual size). */
		float StampRadius = 10.0f;
		/** @brief Width multiplier on foot-bone stamps (length stays anatomical). */
		float FootPrintScale = 1.5f;
		/** @brief Lower smoothstep edge of the stamp falloff, in PERCENT of the stamp radius: 0 = the softest, widest banks; 100 = full depth held to the very edge (clamped just below degenerate in the CB fill). */
		float TrenchWallSharpness = 50.0f;
		/** @brief World-anchored noise on stamp edges (fraction of stamp radius), breaking the swept-capsule look of trails into churned snow. */
		float TrailIrregularity = 0.60f;
		/** @brief Unsupported-snow settle speed, 0-1. Strips left standing between separate trails sink toward whichever side is shallower once BOTH sides are dug away; walls and open snow never move. Default OFF until A/B'd - it changes the map every subsystem reads (TRENCH-REALISM-PLAN.md Stage 3b). */
		float SlumpRate = 0.1f;
		/** @brief Multiplier on the snowfall-driven refill rate. 0 disables refilling. */
		float RefillRateMultiplier = 1.0f;
		/** @brief Refill rate follows the current weather's snowfall density; clear spells and interiors do not refill. Off: constant baseline rate in any weather. */
		bool RefillOnlyWhenSnowing = true;
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
		/** @brief OFF by default, per Josef: the procedural core-and-falloff channel is the look; the texture is the alternate. The path above stays filled so switching this on needs no typing. */
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
		 * Separate from the floating gate because it is a different question.
		 * A ghost is not hovering - measured in game it stands with a 7 unit
		 * gap to its own footing, feet on the ground like any Nord, because
		 * that is exactly what it is. No measurement will ever catch one.
		 *
		 * Translucency is the honest test: a ghost is drawn see-through, and
		 * that is the whole of what makes it a ghost. It also needs no list.
		 * The record flag is the fallback - broader than it sounds, since
		 * Bethesda's "Is Ghost" means invulnerable rather than incorporeal.
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
		 * Must be a TILEABLE surface, which is why it defaults to a LANDSCAPE
		 * texture. The first attempt used the game's own frost impact decal and
		 * that was the wrong kind of image entirely: a decal has its content in
		 * the middle and nothing at the edges because it is printed once, so
		 * blending two offset copies of it can only ever give clumps with gaps
		 * between them. Landscape textures are authored to meet themselves on
		 * every side, which is exactly what the stochastic sampler assumes.
		 *
		 * Editable for the same reason the shell's snow texture is: every
		 * modlist has different ice, and this should match the one in front of
		 * you. The `_n` companion beside it carries the crystal structure.
		 */
		std::string FrostTexturePath = "Textures\\Landscape\\frozenmarshice01.dds";
		/** @brief How strongly the frost pattern shows on crusted snow. This is the CRYSTAL detail; the polish, colour and sheen that make it read as ice are the knobs below and are untouched by it. */
		float FrostPatternStrength = 1.0f;
		/** @brief World units across one tile of the frost pattern. Sampled stochastically, so this sets the size of the crystal detail rather than the size of a repeat - there is no repeat. */
		float FrostPatternScale = 96.0f;
		/**
		 * @brief Raise buried frost effects onto the snow surface instead of leaving them under it.
		 *
		 * Anything the game places at terrain height is swallowed by the shell
		 * floating above it. FROST is the only school where that matters: fire
		 * melts its own hole and lightning pits one, so those effects sit in
		 * snow they have already removed, while frost only hardens what is
		 * there and leaves the layer at full height on top of itself.
		 *
		 * The ONLY thing in the feature that moves a game object. Everything
		 * else reads game state and writes nothing but its own textures, so
		 * this is deliberately narrow and deliberately switchable.
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
		std::array<float, kSnowClassCount> SnowClassDepths = { 14.0f, 18.0f, 30.0f, 30.0f, 30.0f, -5.0f, -5.0f, -5.0f, -5.0f, -5.0f, -5.0f, -5.0f };
		/** @brief Per-texture depth overrides keyed by lowercased diffuse path. Keyed by path, not form ID, so load-order changes cannot rebind them. */
		std::map<std::string, float> TextureDepths;
		/** @brief Statics skin, flat class: layer height on flat split-normal meshes (walkways, roofs, planks); classified per mesh on the GPU by smoothed-vs-raw normal divergence. These get completely flat snow (straight-up offset, raw shading normal). Default 0: painted directly onto the surface; even 1 unit reads as a tiny hover. */
		float ObjectsSnowDepth = 3.0f;
		/** @brief Statics skin, rounded class: layer height on organically smooth meshes (rocks, drifts, logs), where pillow inflation reads correctly. Default 0 like the flat class, per in-game tuning. */
		float SnowMeshesDepth = 3.0f;
		/** @brief Model-class override: ROAD MESHES (matched by geometry name or road/bridge texture path). Default deliberately below the ~30-unit surrounding snow classes: the shallow band is what makes the road's course readable through the snowfield. */
		float RoadMeshesDepth = 10.0f;
		/** @brief Carve trenches into snow on non-road objects. Parked off until object trenching is reworked; roads carve regardless. */
		bool ObjectTrenches = false;
		/** @brief Shell albedo texture, loaded through the VFS. User-editable so the shell can be matched to the modlist's snow by eye. The loader resolves PBR companion maps and falls back to the legacy path when the PBR set is absent. */
		std::string SnowTexturePath = "Textures\\PBR\\Landscape\\snow01.dds";
		/** @brief Set when the texture stores linear (PBR) color. Auto-detected for resolved PBR sets; only matters for legacy textures. */
		bool SnowTextureLinear = false;
		/** @brief Radius multiplier for the workspace clearings (workstations, stalls, wells, shrines). */
		float TrampleZoneScale = 0.75f;
		/** @brief Snow height remaining in a workspace clearing, in PERCENT of the class depth. 0 = melted to the floor, 100 = no clearing. */
		float TrampleZoneHeight = 50.0f;
		/** @brief ON (default) = a whisker of stochastic snow dust scatters just beyond the committed edge onto the ground; OFF = clean binary cut. Round 18 fixed the inverted polarity (the checkbox used to gate a retired cross-fade path, so OFF showed the dust). */
		bool SnowBorderDithering = true;
		/** @brief Minimum snow left on carved trench floors, in units above the terrain. The old hard-coded 5 guaranteed solid snow floors against the terrain window's bilinear error. Default 3 (Josef): wear-through to real ground is gated on shell shadow casting + two-sided height blending landing first — until then low floors expose a bright, unblended pit. */
		float TrenchFloorHeight = 3.0f;
		/** @brief World-unit jitter of where class-depth borders fall (fine-grained domain warp), so snow edges never trace the texture seam. Round 18: capped 37-unit wander + fine 8-unit octave; default 16 (Josef, round 19). */
		float SnowBorderNoise = 16.0f;
		/** @brief World-unit radius widening the depth ramp between neighboring classes, so deep snow meets shallow ground in a slope instead of a ravine wall. */
		float SnowBorderSmoothness = 32.0f;
		/** @brief Border Fade, in PERCENT (round 18: the old 2..64-unit band read as a big move when it mainly sets how visible the outward dust is). Remapped to the internal 2..64 contact-term band on upload; 100% = the old 64. */
		float SnowBorderFade = 100.0f;
		/** @brief View-ray band (units) over which the object snow skin cross-fades into the landscape shell behind it, killing the hard seam where their surfaces run close in height (road meshes, low platforms). */
		float SnowSnowFade = 10.0f;
		/** @brief Angle-of-repose slope for the snow-height field (rise per world unit; 1.0 = 45 degrees). Steeper = raised snow clings tighter: narrow banks instead of broad aprons, juttier mounds. */
		float SnowMoundSteepness = 1.0f;
		/** @brief Dune-field amplitude in world units; 0 flattens deep snow into a mathematically smooth sheet. */
		float UndulationStrength = 8.0f;
		/** @brief Multiplier on the dune field's wavelengths; larger = broader, calmer waves instead of a spike carpet. */
		float UndulationSpacing = 1.0f;
		/** @brief Tessellate the shell and the trench patch. Its real job is trench smoothness: the hull shader's factors key off the deformation map, so carves get vertex density no coarse grid can express. Independent of ReliefDepth since 2026-08-17. */
		bool Tessellation = true;
		/** @brief Displacement-map relief amplitude in world units on UNTRAMPLED landscape snow (carved ground is excluded by the domain shader's (1 - carve) term). Also sets whether undeformed ground is subdivided at all: at 0 its tessellation factor collapses to 1 and only trenches keep theirs. */
		float ReliefDepth = 0.0f;
		/** @brief Parallax self-shadow strength on the snow micro-relief (Extended Materials' term, the one PBR ground already receives). 0 skips the taps entirely. */
		float ParallaxShadowStrength = 0.5f;
		/** @brief Skin DynDOLOD's merged LOD atlas batches too. Those batches wear a generic atlas whose path says nothing about snowiness, so they are otherwise dropped and the objects inside them keep no distant snow. Measured +53 captures for +0.05 ms; a merged batch is one mesh, so this is all-or-nothing per batch. Turn off if any batch turns out to carry non-snow objects that gain snow. */
		/** @brief Parallax occlusion depth on the landscape shell, as a multiplier on the PBR config's displacementScale. 1 = exactly the slab depth PBR ground gets, since kSnowUVTile matches the landscape tiling. 0 skips the march. */
		float ParallaxDepth = 1.0f;
		/** @brief Coarse steps in the parallax march before contact refinement (which re-marches the hit interval at the same budget, so N resolves like N*N). Scaled down with distance. The main quality/cost dial. */
		int ParallaxSteps = 8;
		/** @brief How much a heavily trampled object-trench floor dissolves to the object's own surface (rock, log, planks) instead of holding solid snow. Default 0 until the projected snow diffuse beneath can be hidden. */
		float TrenchFloorFade = 0.0f;
		/** @brief Edge berm crest height as a fraction of the local snow depth. */
		float BermHeight = 0.20f;
		/** @brief Stage 3 P5: rolled rim lip height as a fraction of local depth (cornice look). 0 = off. */
		float RimLip = 0.05f;
		/** @brief Stage 3 P5: rim teeth strength - the carve contour breaks into irregular teeth on the border work's two-octave noise. 0 = off. */
		float RimTeeth = 0.33f;
		/** @brief Stage 3 P6: berm clod amplitude in world units - the crest breaks into coarse thrown chunks at kClodSizeScale cells. 0 = off. */
		float BermClods = 2.0f;
		/** @brief Bow wave (ROADMAP #35): crest height as a fraction of local snow depth. 0 = off. */
		float BowWaveHeight = 0.70f;
		/** @brief Bow wave: multiplier on the push radius, i.e. how far ahead and aside the crest reaches. */
		float BowWaveReach = 0.90f;
		/** @brief Bow wave: 0 = a ring all round the actor, 1 = only dead ahead. Mid values give the crescent. */
		float BowWaveForward = 0.30f;
		/** @brief Bow wave: how far the crest breaks into uneven lumps rather than a smooth swell (P6's clod octave, world-anchored). 0 = smooth. */
		float BowWaveChunk = 1.0f;
		/** @brief Bow wave: seconds the pushed crest holds its height after the actor stops, before sinking back. Displaced snow does not un-displace; this is the closest a live-computed crest gets to that (see ROADMAP #35's deposit-field note). */
		float BowWaveSettle = 2.5f;
		/** @brief Bow wave: speed (units/sec) at which the crest reaches full strength. Lower = a walk already pushes. */
		float BowWaveFullSpeed = 220.0f;
		/** @brief Churn lump amplitude in world units on carved/piled snow (trench walls, floors, berms). */
		float ChurnHeight = 4.0f;
		/** @brief Multiplier on the churn lump wavelengths (larger = broader chunks). */
		float ChurnSize = 0.25f;
		/** @brief Re-march the SSS mask against the SHELL surface in the near field, instead of trusting the ground-marched mask. Restores grass shadows on the snow without the buried-caster prints; costs 8 depth taps per lit shell pixel. Default ON since 2026-08-22 (Josef's A/B). */
		bool ShellSSSRemarch = true;
		/** @brief Streak fix for the re-march: occluders are thin shells (Bend SSS SurfaceThickness, 48 units), so a character in front of the ray no longer paints their silhouette as a streak across the snow behind them. Default ON since 2026-08-22 (Josef's A/B). */
		bool ShellSSSRemarchThickness = true;
		/** @brief Caster height cap (units above the snow line) for the re-march. Taller casters already shadow via the cascades, so their re-march copy is doubled bleed (actors, rails). Default 20 (Josef's pick after the round-13 A/B): short grass only. 200 = accept everything. */
		float ShellSSSRemarchCasterCap = 20.0f;
		/** @brief How completely trampled snow loses its glints (packed snow has crushed the crystals that sparkle). Shared by both shells. Compaction Shading and the Grain (crisp) sliders were RETIRED 2026-08-22, Josef's verdict: IBL + DALC already darken trenches, and real geometry carries the detail the crisp layer faked. */
		float CompactMatte = 0.6f;
		/** @brief Object-snow trench detail: same knobs as the landscape set, independent so tuning one never disturbs the other. Berm is shading-only on objects (geometry berm waits for the skin rework). */
		float ObjBermHeight = 0.35f;
		float ObjChurnHeight = 5.0f;
		float ObjChurnSize = 0.25f;
		/** @brief Render distances in meters (converted via kUnitsPerMeter). The shell itself auto-sizes to the loaded-cell grid (no slider); Trenches resizes the deformation window and clears the map on apply (content is scale-relative). */
		float RangeTrenchesM = 100.0f;
		float RangeSkinsM = 750.0f;
		/** @brief Distance (m) where the object-snow skin STARTS dissolving back into the object's own material; fully gone at the Object Snow range end. Cures distant blank-white objects. */
		float RangeSkinsFadeM = 100.0f;
		/** @brief Distance (m) by which the skin's GEOMETRIC height has collapsed to zero, at the deepest class; shallower classes collapse proportionally sooner. Past the object height window (kHeightMapHalfExtent / kUnitsPerMeter, ~58 m) the rim-wall gate has no data, but the remaining rim is sub-pixel at that range â€” measured clean out to 200 m. */
		float RangeSkinsGeometryM = 100.0f;
		/** @brief Strength of the far-field facing handover: as a pixel grows past the edge taper's own width the coverage test hands over to the true face normal, so distant objects keep bare rock on steep faces instead of collapsing to white. Scales against kFacingLODMax; 0 disables it. The near-field rim-contour push is deliberately NOT on this dial (see the PS). */
		float SkinDistantBareness = 0.6f;
		/** @brief Distant snow line (world Z units): heightmap-sourced far terrain above this height gets snow coverage. */
		float DistantSnowLineZ = 5000.0f;
		/** @brief How far the snow line sinks (world units) toward the worldspace's north edge, so the northern coast is snowy at sea level. */
		float DistantSnowNorthDrop = 15000.0f;
		/** @brief Half-width (world units) of the bare-to-snow blend band around the snow line. */
		float DistantSnowLineFade = 1500.0f;
		/** @brief LOD-diffuse snow classification: 0 = only bright white counts, 1 = pale gray already counts. */
		float LODSnowSensitivity = 0.5f;
		/** @brief Horizon snow: recolor the game's LOD terrain with the shell's snow material wherever its bake classifies as snow. */
		bool HorizonSnow = true;
		/** @brief A/B toggle: shade horizon snow with the old vanilla-math recolor instead of the shell's recipe. */
		bool LODReplaceLegacy = false;
		/** @brief Projected snow wears the shell's snow set (albedo + PBR response) on draws whose projected material is snow. */
		bool ProjSnowMatch = true;
		/** @brief Glacier/iceberg baked snow is recolored to the shell's snow set in Lighting (up-facing bright texels), and the ice family is EXCLUDED from the geometry skin — the skin conforms through the object raster, whose 4096-unit window can never cover a glacier (Josef's 70 m ceiling), and its mesh-facet lift produced square patches, dual class layers and rim gaps on them. */
		bool GlacierSnowMatch = true;
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

		/** @brief A/B: 1 = old input-patch recolor, 0 = shell-recipe output override. */
		float LODReplaceLegacy;
		/** @brief Projected-snow material match enabled and the snow set is bound. */
		float ProjSnowEnable;
		/** @brief Baked-snow (glacier) material match enabled and the snow set is bound. */
		float BakedSnowEnable;
		float padLod;
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
		DirectX::XMINT2 ScrollDelta;

		float TexelSize;
		uint StampCount;
		float RefillAmount;
		uint ClearMap;

		/** @brief Lower smoothstep edge of the stamp falloff (fraction of radius): higher = steeper trench walls. */
		float StampFalloffStart;
		/** @brief Fraction-of-radius noise wobbling each stamp's edge. */
		float StampNoiseAmp;
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

		float4 Stamps[kMaxStamps];
		/** @brief Capsule segment start per stamp (the stamped shape's previous position). */
		float4 StampEnds[kMaxStamps];
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

	ConstantBuffer* perFrame = nullptr;
	Texture2D* deformationTextures[2] = { nullptr, nullptr };
	uint currentTexture = 0;

	/** @brief Baked berm field: the 17-tap disc average of the deformation map, rebuilt from the current map every frame so the shells read it with one bilinear tap instead of 68 loads per call. */
	Texture2D* bermFieldTexture = nullptr;

	/** @brief SRV of the most recently written deformation map, for shader sampling and debug UI. */
	ID3D11ShaderResourceView* GetDeformationSRV() const { return deformationTextures[currentTexture]->srv.get(); }
	/** @brief SRV of the baked berm field; null before SetupResources. */
	ID3D11ShaderResourceView* GetBermFieldSRV() const { return bermFieldTexture ? bermFieldTexture->srv.get() : nullptr; }
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

	/** @brief Returns the deformation update compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetDeformationUpdateCS();
	ID3D11ComputeShader* deformationUpdateCS = nullptr;
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

		float BorderTrampledFade;
		float BorderUntrampledFade;
		/** @brief View-ray band over which the statics skin cross-fades into the landscape shell behind it. */
		float SnowSnowFade;
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
		/** @brief How much heavily trampled object-trench floors dissolve to the object's own texture (0 = solid snow floors). */
		float TrenchFloorFade;
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
		/** @brief RETIRED 2026-08-22 (crisp grain removed; layout keepers, uploaded 1/0). */
		float CrispScaleV;
		float CrispStrengthV;

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
		/** @brief x > 0.5 = outward dust beyond the committed edge (0 = clean binary cut); y = minimum snow on carved trench floors in units above terrain; zw = atlas slices of sun cascades 0/1 (round 22: the shared atlas moves the sun's slices with the active-light set; the PS crisp path needs the real indices). Mirror any change in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 BorderStyle;
		/** @brief x = compaction glint suppression (Stage 1); y = shell-surface SSS re-march, PACKED: integer part 0 off / 1 on / 2 on + thickness streak fix, fraction * 1000 = caster height cap in units; zw = dynamic-resolution scale for its screen-space taps (the shell pass does not bind FrameBuffer b12 - round 164). ONE constant for BOTH shells. Mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 CompactLook;
		/** @brief Stage 3: x = P5 rim lip height (fraction of local depth), y = P5 rim teeth strength, z = P6 berm clod amplitude (world units), w spare. xy consumed inside CarveProfile; z at the berm sites. Appended LAST; mirror in SnowShell.hlsl AND the SnowStaticsShell.hlsl ShellCB prefix. */
		float4 RimStyle;
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

	/** @brief Heatmap-mode PS permutation (SNOW_LOD_HISTOGRAM): single SV_Target + histogram UAV at u1. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11PixelShader* GetShellLODPS();
	ID3D11PixelShader* shellLODPS = nullptr;


	/** @brief Tessellated-path stages (SNOW_TESS): control-point VS (grid placement only), hull shader (distance-based crack-free factors) and domain shader (full surface evaluation + displacement-map relief). Active when Relief Depth > 0. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11VertexShader* GetShellTessVS();
	ID3D11HullShader* GetShellHS();
	ID3D11DomainShader* GetShellDS();
	ID3D11VertexShader* shellTessVS = nullptr;
	ID3D11HullShader* shellHS = nullptr;
	ID3D11DomainShader* shellDS = nullptr;

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
		float2 dir{};
		float radius = 24.0f;
		float strength = 0.0f;
		float distSq = 0.0f;
	};
	/** @brief Must match MAX_BOW_WAVES in SnowShell.hlsl. The 256-stamp cap already means player + near actors; this is tighter still. */
	static constexpr size_t kMaxBowWaves = 16;
	/** @brief Layout must match BowWaveCB in SnowShell.hlsl (b1 of the shell pass). Its own buffer: ShellCB is hand-mirrored across two shaders and this is landscape-only. */
	struct BowWaveCB
	{
		float4 BowWaveParams;
		float4 BowWaveLook;
		float4 BowWavePosDir[kMaxBowWaves];
		float4 BowWaveShape[kMaxBowWaves];
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

	/** @brief Object-snow debug view: skins and trench patch render decision variables as colors with dithering disabled. Runtime-only diagnostic. */
	/** @brief 0 off, 1 edge-taper masks, 2 coverage alpha (see the PS debug block). */
	int staticsDebugView = 0;

	// ---- Distant-snow / LOD diagnostics (runtime-only) ----

	/** @brief 0 off, 1 depth-delta heatmap, 2 warp-ring view, 3 data-provenance view. Mirrors ShellCB::ShellLODDebug. */
	int lodDebugView = 0;
	/** @brief Shimmer meter: a probe CS evaluates the shell mesh surface at world-anchored points each frame; the CPU tracks frame-to-frame height deltas per distance band. */
	bool lodShimmerMeter = false;

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
		float2 WorldYRange;

		float SnowLineZ;
		float SnowNorthDrop;
		float SnowLineFade;
		float SnowDepthUnits;

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
		/** @brief Glacier/iceberg family: captured past the Object Snow range cap and exempt from the SkinFade distance dissolve — their own baked snow never matches the shell, so the skin must persist at every loaded distance. */
		bool fadeExempt;
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

	/** @brief Depth copy taken after the terrain shell draw (shell surface included), so the statics skin can measure its view-ray gap to the landscape shell; Terrain Blending's technique adapted to the two snow kinds. */
	winrt::com_ptr<ID3D11Texture2D> shellDepthCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> shellDepthCopySRV;
	/** @brief Pre-shell copy of the MASKS target: Masks.y carries the land's EM grain height (Lighting.hlsl LANDSCAPE; 0 = no data) for the shell's two-sided edge contest, readable only before the shell overwrites the G-buffer. Bound at t10 on the shell PS. */
	winrt::com_ptr<ID3D11Texture2D> landMasksCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> landMasksCopySRV;

	/** @brief Copies the resource behind a_srcSRV into an owned SRV-only texture, recreating it when dimensions or format change. The SRV doubles as the validity signal (nulled by callers on invalid frames), so it is rebuilt even when the texture itself is still current. Implemented in SnowDeformation/Shell.cpp. */
	static void CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
		winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv);

	// ---- Sun shadows on the shells: crisp cascade receiver + caster ----

	/** @brief Full-resolution COPY of the game's raw sun-shadow cascade atlas, taken during the shadow-mask pass. The copy is mandatory: by deferred time the engine has reused the live target, and sampling it live produces garbage flicker. Taken AFTER the shell is injected as a caster (round 21), so the snowfield receives its own banks' shadows; acne is held off by the caster's depth push. */
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
	ID3D11VertexShader* shellShadowVS = nullptr;

	/** @brief Last frame's fully-computed ShellCB (heap-held: ShellCB is over-aligned and embedding it pads the class). The caster injection runs at the shadow-mask pass, before this frame's DrawShell recomputes the windows; one-frame-stale grid placement is invisible in a shadow. Null until the first DrawShell. */
	std::unique_ptr<ShellCB> lastShellCBData;

	/** @brief Per-descriptor DSVs created on the LIVE atlas texture, cached by texture pointer (not owned; key only) and by the REAL slice each descriptor renders to (shadowmapIndex — the atlas is shared with local shadow lights and the sun's slices move with the active-light set). Four entries (round 26): the sun owns MORE shadowmaps than the two cascades (the focus map is in the family, and descriptor order is not guaranteed), so the shell is injected into every one — each descriptor carries its own transform and slice, making the ordering irrelevant. Truncated shadows (near lobe present, far lobe missing, boundary sweeping with the view) were the two-descriptor assumption missing the far cascade. */
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
		/** @brief Strength of the skin's coverage LOD terms (facing handover to the geometric face normal, rim-contour push); 0 reproduces the pre-LOD gates exactly. */
		float SkinDistantBareness;
		/** @brief >0.5: skip the SkinFadeStart/End distance dissolve (glacier/iceberg captures). Mirror in SnowStaticsShell.hlsl. */
		float FadeExempt;
		float padStatics[2];
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
		uint32_t padSm[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SmoothCB);

	/** @brief Per unique geometry (keyed by vertex buffer pointer): position-averaged normals, built once by SmoothNormalsCS. Split-normal flat meshes (planks, roofs, pole caps) inflate along these so their snow drapes as a sealed pillow instead of a hovering parallel sheet. */
	struct SmoothedNormalsEntry
	{
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool ready = false;
	};
	std::unordered_map<void*, SmoothedNormalsEntry> smoothedNormalsCache;
	ID3D11ComputeShader* smoothAccumulateCS = nullptr;
	ID3D11ComputeShader* smoothResolveCS = nullptr;
	/** @brief Third pass: one-group reduction writing the mesh's flat/rounded classification (fraction of smoothed-vs-raw divergent vertices) into the stats element appended at SmoothedNormals[vertexCount]. */
	ID3D11ComputeShader* smoothFlatStatsCS = nullptr;
	ConstantBuffer* smoothCB = nullptr;

	/** @brief Builds (or returns) the smoothed-normal buffer for a captured geometry. Dispatches the SmoothNormalsCS passes on first sight; cached thereafter. Returns null while unavailable (the VS falls back to raw normals). Implemented in SnowDeformation/Statics.cpp. */
	ID3D11ShaderResourceView* EnsureSmoothedNormals(RE::BSGeometry* a_geometry);

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
	/** @brief Corpse burial: mounds cap this far above the terrain (a mammoth makes a bump, not a hill), from at most this many resting collision spheres per frame. */
	static constexpr float kCorpseMoundCap = 20.0f;
	static constexpr uint kMaxCorpseSpheres = 64;

	/** @brief Ping-pong accumulated raw maps (scrolled each frame, captures rasterized on top): object TOP and BOTTOM surfaces. Persistence matters; the capture list is frustum-culled, and a map rebuilt from it alone loses every object behind the camera. */
	Texture2D* heightTopRaw[2] = { nullptr, nullptr };
	Texture2D* heightBottomRaw[2] = { nullptr, nullptr };
	/** @brief Processed maps the shell samples (t4/t5): the slope-limited snow-height field and the smooth shelter/suppression mask, plus a cone-iteration scratch. */
	Texture2D* heightTopFiltered = nullptr;
	Texture2D* heightBottomFiltered = nullptr;
	Texture2D* heightScratch = nullptr;
	/** @brief Cone-transformed snow SURFACE height over the object top raster; the skin's edge taper reads it with one tap. */
	Texture2D* objectSnowCone = nullptr;
	/** @brief Per-frame skin-depth raster (R16F, cleared each frame, MAX-blended): each captured mesh writes its class layer depth, so consumers know how thick the snow above any object top is. No scroll persistence; a missed frame is invisible for one frame. */
	Texture2D* heightSkinDepth = nullptr;
	uint heightCurrent = 0;
	bool heightMapValid = false;
	float2 heightWindowCenter = { 0, 0 };

	/** @brief RT0 MAX (tops) + RT1 MIN (bottoms) + RT2 MAX (skin depth) in one raster pass: highest/lowest surfaces win per texel in any draw order; no depth buffer needed. */
	winrt::com_ptr<ID3D11BlendState> heightMaxBlendState;
	ID3D11VertexShader* heightVS = nullptr;
	ID3D11PixelShader* heightPS = nullptr;
	ID3D11ComputeShader* heightScrollCS = nullptr;
	ID3D11ComputeShader* heightCombineCS = nullptr;
	ID3D11ComputeShader* heightConeCS = nullptr;
	ID3D11ComputeShader* objectConeSeedCS = nullptr;
	ID3D11ComputeShader* objectConeCS = nullptr;

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
		/** @brief Deformation-map addressing for the corpse-mound refill gate (same mapping the shell's deformation samplers use). */
		float2 DeformWindowOriginH;
		float DeformInvWorldSizeH;

		/** @brief Dead actors at rest, as collision spheres (xyz world center, w radius): CombineCS raises capped snow mounds over them, gated by local refill. */
		uint32_t CorpseSphereCount;
		float CorpseMoundCap;
		float2 padWind;
		float4 CorpseSpheres[kMaxCorpseSpheres];

		/** @brief Rounded-class snow depth, seeding the object snow cone. */
		float ObjectSnowDepth;
		float padObs[3];
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
		{ "smelter", 300.0f, 0.0f, true },  // smelters/forges are workspaces per Josef's call: both sliders apply, and their own flames must not add melt spots
		{ "forge", 260.0f, 0.0f, true },
		{ "sawmill", 220.0f, 0.0f },
		{ "millsaw", 220.0f, 0.0f },
		{ "stables", 200.0f, 0.0f },    // NOT "stable": clutter\ruins\ruinstable01 is a table
		{ "chopping", 150.0f, 50.0f },  // wood chopping blocks
		{ "enchanting", 110.0f, 60.0f },
		{ "alchemy", 110.0f, 60.0f },
		{ "workbench", 140.0f, 0.0f },         // symmetric: workbench facing disagreed with itself across refs (round 230 behind, round 232 right) - a centered bowl cannot be wrong-sided
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
	/** @brief Tessellated skin stages (optional; legacy path is the fallback): control-point VS, hull (edge-length/distance factors) and domain (displacement-map relief along the inflate normal). */
	ID3D11VertexShader* staticsTessVS = nullptr;
	ID3D11HullShader* staticsHS = nullptr;
	ID3D11DomainShader* staticsDS = nullptr;
	/** @brief Trench patch (PATCH define): the landscape shell's dense-grid carve applied to OBJECT tops; real geometry where parallax cannot notch silhouettes or hold floors angle-stably. */
	ID3D11VertexShader* patchVS = nullptr;
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
	/** @brief Tints classified baked-snow (glacier) pixels cyan (DebugTerrainOverlay bit 8), same verification pattern as the projected view. */
	bool debugGlacierView = false;

protected:
	/** @brief Fills perFrameData.Stamps from the player and nearby loaded actors. Implemented in SnowDeformation/Stamping.cpp. */
	void GatherStamps(PerFrame& perFrameData);

	std::unordered_map<uintptr_t, uint8_t> snowMasks;
	std::shared_mutex snowMaskMutex;

	/** @brief Baked cells keyed by (cellX << 32) | cellY; entries carry their worldspace, which the window rebuild must match. */
	std::unordered_map<uint64_t, ShellCellData> shellCells;
	std::shared_mutex shellCellMutex;
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
	bool clearRequested = true;

	// ---- Runtime render-distance state (driven by the Range* settings) ----
	/** @brief Deformation window world size (2x the Trenches range). Changing it clears the map. */
	float deformWorldSize = 14000.0f;
	bool trenchRangeDirty = false;
	/** @brief Deformation map resolution. */
	uint deformMapDim = kTextureDim;
	bool rangeInitApplied = false;

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
	 * Decoupling detection from effect is the point: several detectors
	 * (projectiles, hazards, explosions, actor auras) fill this list, and one
	 * consumer turns it into stamps. Positions are already ground-projected -
	 * the deformation map is 2D, so an emitter must carry where it marks the
	 * GROUND, not where the source happens to float.
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
	 * Measured, not listed. Bethesda does not flag a hovering creature - the
	 * atronach races are all authored `Walks` and none sets `kFlies` - so a
	 * flag lookup would find nothing and a race table would miss every modded
	 * levitator. The lowest thing an actor could carve with, against the
	 * ground it stands on, answers the question directly and covers wisps and
	 * ghosts for free.
	 *
	 * Reads the cached bones where there are any and falls back to the
	 * collision shapes, which is the pair the stamping paths below already
	 * choose between.
	 */
	bool ActorIsFloating(RE::Actor* a_actor, RE::NiAVObject* a_root, const StampBones* a_bones, float a_groundZ, float* a_gapOut = nullptr) const;

	/**
	 * @brief True when this actor has no substance, so nothing it does should cut snow.
	 *
	 * A separate axis from floating, and it has to be: a ghost walks with its
	 * feet on the ground, so no gap measurement can see one.
	 *
	 * Translucency routes through Community Shaders' own definition of a
	 * see-through surface - the pair of tests ExtendedTranslucency runs on
	 * every geometry it shades - rather than inventing a second one. Narrowed
	 * to SKINNED geometry with a material alpha below full, because plain
	 * alpha BLENDING catches every NPC's hair and eyes.
	 *
	 * The record flag is a static read of the actor's base, not the runtime
	 * call: the same answer without a relocation.
	 */
	bool ActorIsIncorporeal(RE::Actor* a_actor, RE::NiAVObject* a_root, StampBones* a_bones,
		float* a_alphaOut = nullptr, bool* a_flagOut = nullptr) const;

	/**
	 * @brief Raises one buried effect onto the snow above it, on the GAME thread.
	 *
	 * The detection runs where every other detector runs - the per-frame
	 * reference scan, on the render thread - but the write does not. Moving a
	 * node in the game's scene graph from a render pass is precisely the shape
	 * of thing that crashed this feature three times in Step 7, and that was
	 * only READING. So the lift is handed to the SKSE task interface, which is
	 * how the rest of this codebase mutates a reference.
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
	 * A projectile that drops out of Projectile::Manager has detonated, and
	 * its own effect already told us the element - so this path needs neither
	 * the explosion reference nor the explosion-to-element table. It is also
	 * the only route that works if spawned explosions never reach a cell's
	 * reference list at all.
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
	 * A blast is one instant, but a stamp only deepens while its emitter
	 * exists, so a single frame of any sane rate is a single frame of melt and
	 * therefore nothing. Either the rate is made effectively infinite and the
	 * crater simply appears, or the emitter is held for a moment and the snow
	 * is seen to give way. This is the second.
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
	 * It is UNSUMMONED when it dies rather than left as a corpse, so by the
	 * time any per-frame sweep looks it is already out of the high-process
	 * list, out of its 3D, or both - and waiting for its handle to go stale
	 * takes far longer than the window that separates a death from the player
	 * simply walking away. The engine says exactly when it happened, so ask it.
	 *
	 * The sink reads the actor's POSITION and its RACE's spell list, which is
	 * static form data. Nothing here walks a live actor's effects.
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
	 * Shouts are the one source whose shape the deformation map cannot express
	 * as a single stamp: a cone is not a capsule. It is laid down as a row of
	 * discs of growing radius along the shout's axis instead, which needs no
	 * new stamp mode, no shader change and no constant-buffer field - the
	 * thing this project would otherwise have had to grow for one spell.
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
	 * Whirlwind Sprint and Storm Call cannot be told apart by their records:
	 * both are voice powers, both self-delivered, both carry a script effect
	 * and a projectile, and Storm Call carries the stagger besides. Impact
	 * force is no better - Marked for Death throws the very same 50-force push
	 * projectile Unrelenting Force does.
	 *
	 * So the question is not asked of the records at all. A self-delivered
	 * shout opens a short watch on its caster, and what marks the snow is the
	 * caster MOVING faster than anything on foot can. Storm Call leaves them
	 * standing and so marks nothing, without ever being named or excluded; a
	 * modded dash shout works for the same reason.
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
	 * A corpse that is alight, crackling or frozen stiff should go on marking
	 * the snow under it - but what a corpse is DOING is exactly the sort of
	 * live-actor state this feature refuses to poll for. The element does not
	 * have to be read off the body at all: the effect that hit it announced
	 * itself on the way in, and the effect record is static data.
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
	 * Deliberately NOT read off the actor. Three attempts to walk an actor's
	 * active effects crashed - the last inside GetActiveEffectList itself, on
	 * a console-spawned actor, with a null vtable entry. The spell RECORD
	 * carries everything needed (element, magnitude, duration) and is static
	 * data, so the cast tells us what began and its own duration tells us when
	 * it ends. Nothing here ever reaches into a live actor for state.
	 *
	 * The cost is honest: a cloak dispelled early keeps marking until its
	 * timer runs out, and an innate aura that is never cast - an atronach's -
	 * is not seen at all. Step 10 owns atronachs regardless.
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
	 * An atronach's aura is INNATE, so the cast sink of Step 7 never sees it -
	 * nothing ever casts it. The answer is not a race list and not a keyword
	 * table: the aura is a real SpellItem sitting on the race's own spell list,
	 * and its cloak effect carries the same four axes every other detector
	 * classifies through. Verified against Skyrim.esm - AbFlameAtronach carries
	 * AbAtronachCloakFire (archetype Cloak, resist ResistFire, magnitude 10),
	 * and the frost and storm races carry the matching pair.
	 *
	 * So it is derived exactly like a cast spell, and modded atronachs work for
	 * the same reason modded Flames clones do. Every read here is of a static
	 * FORM, never of a live actor's effect list - the thing that crashed three
	 * times in Step 7 and must not come back.
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
		uint spells = 0;
		/** @brief Stamps taken by actors and props, read before any emitter is. Against kMaxStamps - kSpellStampReserve this says whether the fight is running into the budget or nowhere near it. */
		uint beforeSpells = 0;
		/** @brief Actors stopped by the TRANSLUCENCY gate, which is a different measurement from floating and answers to a different setting. */
		uint incorporeal = 0;
		/** @brief What the nearest actor's skeleton offered the stamper. Feet decide the path outright: with them an actor prints heel-to-toe, without them it falls to its collision shapes, and an actor that passes every gate and still marks nothing is one of those two coming up empty. */
		uint nearestFeet = 0;
		uint nearestLimbs = 0;
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
	};
	StampStats stampStats;

	/** @brief Rebuilt each frame in GatherStamps: resting dead actors' collision spheres, consumed by CombineCS as capped snow mounds (buried-corpse bumps). */
	std::vector<float4> corpseMoundSpheres;

	/** @brief Last 3D-root position per loose prop (formID), rebuilt every frame from the in-range scan. The position gate runs before any collision traversal, so resting clutter costs one hash lookup per frame. */
	std::unordered_map<uint32_t, RE::NiPoint3> propPrevPositions;

	/** @brief Stillness latch per corpse (formID). Once settled, only a large accumulated displacement (dragging, explosions) wakes it, so ragdoll micro-drift cannot re-trench under a buried corpse. Erased when the actor is seen alive again. */
	struct CorpseRest
	{
		uint16_t stillFrames = 0;
		bool settled = false;
		/** @brief Root z from the previous frame, for the flight gate. */
		float prevZ = 0.0f;
		bool hasPrevZ = false;
	};
	std::unordered_map<uint32_t, CorpseRest> corpseRestStates;
};
