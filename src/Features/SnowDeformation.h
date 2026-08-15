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
				T("feature.snow_deformation.key_feature_3", "Configurable snow refill over time"),
				T("feature.snow_deformation.key_feature_4", "Compute-shader based, low performance impact") } };
	};

	// Square world-space deformation window following the camera in whole-texel
	// steps. Texel value = normalized depression depth, 0 = untouched snow,
	// 1 = compressed to the ground. World size is runtime (deformWorldSize),
	// resolution is fixed, so trench detail coarsens with range.
	static constexpr uint kTextureDim = 2048;
	static constexpr uint kMaxStamps = 128;

	/** @brief Skyrim world units per meter (1 unit ≈ 1.43 cm). Range sliders are in meters. */
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
	static constexpr uint kSnowClassCount = 12;
	// Classes below this index count as snow for the terrain-shader mask bits.
	static constexpr uint kSnowOnlyClassCount = 5;
	struct SnowClassDef
	{
		const char* label;
		const char* match;
		float defaultDepth;
	};
	static constexpr SnowClassDef kSnowClasses[kSnowClassCount] = {
		{ "Grass Snow", "grasssnow", 14.0f },
		{ "Trodden Path", "snowpath", 18.0f },
		{ "Snowy Rocks", "snowrocks", 26.0f },
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

	/** @brief Baked per-cell terrain data: 33x33 vertex heights (absolute Z) plus per-class coverage weights (0-255). Depths are applied at window-rebuild time so class sliders retune without a game re-bake. */
	struct ShellCellData
	{
		std::array<float, 33 * 33> height;
		std::array<std::array<uint8_t, 33 * 33>, kSnowClassCount> classWeights;
	};

	struct Settings
	{
		bool EnableSnowDeformation = true;
		bool ShowDebugTexture = false;
		/** @brief Scale on Havok collision-shape radii (20 = 1.0x = the shapes' actual size). */
		float StampRadius = 20.0f;
		/** @brief Seconds for compressed snow to fully recover. 0 disables refilling. */
		float RefillTime = 700.0f;
		/** @brief Only refill while the current weather is snowing, so trails persist through clear spells and interiors. */
		bool RefillOnlyWhenSnowing = true;
		/** @brief Per-class shell depths, indexed like kSnowClasses (defaults duplicated from the table). */
		std::array<float, kSnowClassCount> SnowClassDepths = { 14.0f, 18.0f, 26.0f, 30.0f, 30.0f, -5.0f, -5.0f, -5.0f, -5.0f, -5.0f, -5.0f, -5.0f };
		/** @brief Statics skin, flat class: layer height on flat split-normal meshes (walkways, roofs, planks); classified per mesh on the GPU by smoothed-vs-raw normal divergence. These get completely flat snow (straight-up offset, raw shading normal). Default 0: painted directly onto the surface; even 1 unit reads as a tiny hover. */
		float ObjectsSnowDepth = 0.0f;
		/** @brief Statics skin, rounded class: layer height on organically smooth meshes (rocks, drifts, logs), where pillow inflation reads correctly. */
		float SnowMeshesDepth = 3.0f;
		/** @brief Model-class override: ROAD MESHES (matched by geometry name or road/bridge texture path). Default deliberately below the ~30-unit surrounding snow classes: the shallow band is what makes the road's course readable through the snowfield. */
		float RoadMeshesDepth = 10.0f;
		/** @brief Shell albedo texture, loaded through the VFS. User-editable so the shell can be matched to the modlist's snow by eye. The loader resolves PBR companion maps and falls back to the legacy path when the PBR set is absent. */
		std::string SnowTexturePath = "Textures\\PBR\\Landscape\\snow01.dds";
		/** @brief Set when the texture stores linear (PBR) color. Auto-detected for resolved PBR sets; only matters for legacy textures. */
		bool SnowTextureLinear = false;
		/** @brief World-unit jitter of where class-depth borders fall (domain warp), so snow edges never trace the texture seam. */
		float SnowBorderNoise = 32.0f;
		/** @brief World-unit radius widening the depth ramp between neighboring classes, so deep snow meets shallow ground in a slope instead of a ravine wall. */
		float SnowBorderSmoothness = 32.0f;
		/** @brief How far from a class border trampled snow keeps its visibility override; beyond ~20 the landscape under trenches becomes too visible. */
		float SnowBorderTrampledFade = 20.0f;
		/** @brief Depth band (units) over which untrampled snow's edge dissolves at class borders. */
		float SnowBorderUntrampledFade = 5.0f;
		/** @brief View-ray band (units) over which the object snow skin cross-fades into the landscape shell behind it, killing the hard seam where their surfaces run close in height (road meshes, low platforms). */
		float SnowSnowFade = 10.0f;
		/** @brief Render distances in meters (converted via kUnitsPerMeter). Shell scales the warped grid's spacing and applies live; Trenches resizes the deformation window and clears the map on apply (content is scale-relative). */
		float RangeShellM = 375.0f;
		float RangeTrenchesM = 100.0f;
		float RangeSkinsM = 750.0f;
		/** @brief Distance (m) where the object-snow skin STARTS dissolving back into the object's own material; fully gone at the Object Snow range end. Cures distant blank-white objects. */
		float RangeSkinsFadeM = 100.0f;
	};

	/** @brief GPU-side settings, appended to the shared FeatureData cbuffer (b6). Layout must match SnowDeformationSettings in SharedData.hlsli. */
	struct alignas(16) SettingsGPU
	{
		float2 WindowOrigin;
		float InvWorldSize;
		uint EnableSnowDeformation;

		uint DebugTerrainOverlay;
		float3 padSnow;
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

		float4 Stamps[kMaxStamps];
		/** @brief Capsule segment start per stamp (the stamped shape's previous position). */
		float4 StampEnds[kMaxStamps];
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	Settings settings;

	ConstantBuffer* perFrame = nullptr;
	Texture2D* deformationTextures[2] = { nullptr, nullptr };
	uint currentTexture = 0;

	/** @brief SRV of the most recently written deformation map, for shader sampling and debug UI. */
	ID3D11ShaderResourceView* GetDeformationSRV() const { return deformationTextures[currentTexture]->srv.get(); }
	/** @brief World XY of the corner of texel (0,0) of the current deformation window. */
	float2 GetWindowOrigin() const { return windowOrigin; }

	/** @brief Creates the ping-pong deformation textures and the per-frame constant buffer. */
	virtual void SetupResources() override;

	/**
	 * @brief Per-frame update: gathers actor stamp positions, scrolls the window
	 * to follow the camera, and dispatches the deformation update compute shader.
	 */
	virtual void Prepass() override;

	/** @brief Returns the deformation update compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetDeformationUpdateCS();
	ID3D11ComputeShader* deformationUpdateCS = nullptr;
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
		// destroys float32 finite differences → shimmering normals).
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
		float3 padShell;
	};
	STATIC_ASSERT_ALIGNAS_16(ShellCB);

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

	ConstantBuffer* shellCB = nullptr;
	winrt::com_ptr<ID3D11RasterizerState> shellRasterState;
	winrt::com_ptr<ID3D11DepthStencilState> shellDepthState;

	/** @brief Returns the depth sync compute shader (shell depth -> Terrain Blending's blended depth copies), compiling it on first use. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11ComputeShader* GetDepthSyncCS();
	ID3D11ComputeShader* depthSyncCS = nullptr;

	/** @brief Renders the shell as an always-visible plane colored by the sampled terrain data (red=height, green=coverage, blue=ramp depth). Runtime-only diagnostic. */
	bool shellDataDebug = false;

	/** @brief The landscape snow diffuse, loaded from the modlist via the VFS. Null when unavailable (constant-albedo fallback). */
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowDiffuseSRV;
	/** @brief TruePBR companion maps, auto-resolved by probing the Textures\PBR\ variant of the snow path: tangent normals (_n) and roughness/metal/AO/spec (_rmaos). Their presence also auto-selects linear color. */
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowRmaosSRV;
	bool shellSnowTextureIsPBR = false;
	bool shellSnowTextureAttempted = false;

	/** @brief Material parameters fetched from the modlist's own TruePBR config JSON (PBRTextureSets\, matched by texture basename) so the shell's sparkle and response follow whatever texture set the user runs. Defaults mirror common authored snow values. */
	float snowGlintLogDensity = 6.0f;
	float snowGlintMicroRoughness = 0.3f;
	float snowGlintDensityRandomization = 5.0f;
	float snowGlintScreenSpaceScale = 1.0f;
	float snowRoughnessScale = 0.7f;
	float snowSpecularLevel = 0.02f;
	winrt::com_ptr<ID3D11SamplerState> shellSnowSampler;

	/** @brief Lazy-loads the shell snow texture set (and its authored PBR parameters) from the user-configured path. Implemented in SnowDeformation/Shell.cpp. */
	void EnsureShellSnowTextures();

	/** @brief Bakes one cell's heights and per-vertex snow coverage from LoadedLandData. Called from the TESObjectLAND hook. Implemented in SnowDeformation/TerrainData.cpp. */
	void BakeShellCell(RE::TESObjectLAND* land);

	/** @brief Recenters and re-uploads the terrain data window when the camera crosses cells or new cells were baked. Called from Prepass. Implemented in SnowDeformation/TerrainData.cpp. */
	void UpdateShellTerrainWindow();

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
	};

	/** @brief Render-thread only: filled during opaque rendering by the SetupGeometry hook, consumed and cleared each frame. */
	std::vector<CapturedSnowStatic> capturedStatics;
	std::unordered_set<void*> capturedStaticsSet;
	std::atomic<uint32_t> statCapturedStatics{ 0 };

	/** @brief Records projected-snow lighting draws for the statics skin. Called from the BSLightingShader::SetupGeometry hook. Implemented in SnowDeformation/Statics.cpp. */
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
	/** @brief Installs the SetupGeometry capture hook. Called from PostPostLoad; implemented in SnowDeformation/Statics.cpp. */
	void InstallStaticsCaptureHook();

	/** @brief Depth copy taken after the terrain shell draw (shell surface included), so the statics skin can measure its view-ray gap to the landscape shell; Terrain Blending's technique adapted to the two snow kinds. */
	winrt::com_ptr<ID3D11Texture2D> shellDepthCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> shellDepthCopySRV;

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
		float padStat2;
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

	// 4096 units at 8-unit texels, following the camera; matches the
	// shell's inner grid density.
	static constexpr uint kHeightMapDim = 512;
	static constexpr float kHeightMapHalfExtent = 2048.0f;
	/** @brief Height sentinels for texels no object covers. */
	static constexpr float kHeightMapEmptyTop = -100000.0f;
	static constexpr float kHeightMapEmptyBottom = 100000.0f;

	/** @brief Ping-pong accumulated raw maps (scrolled each frame, captures rasterized on top): object TOP and BOTTOM surfaces. Persistence matters; the capture list is frustum-culled, and a map rebuilt from it alone loses every object behind the camera. */
	Texture2D* heightTopRaw[2] = { nullptr, nullptr };
	Texture2D* heightBottomRaw[2] = { nullptr, nullptr };
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

	/** @brief Per-dispatch constants for the height-window processing. Layout must match HeightProcessCB in HeightMapProcessCS.hlsl. */
	struct alignas(16) HeightProcessCB
	{
		DirectX::XMINT2 ScrollDelta;
		uint ClearAll;
		/** @brief Units/frame the accumulated tops/bottoms drift toward empty; stale object imprints (disabled/moved/harvested) melt instead of persisting until scrolled out. */
		float GhostDecay;
	};
	STATIC_ASSERT_ALIGNAS_16(HeightProcessCB);
	ConstantBuffer* heightProcessCB = nullptr;

	/** @brief Creates the height-window textures. Implemented in SnowDeformation/Statics.cpp. */
	void CreateHeightFieldResources();
	/** @brief Scrolls the accumulated height maps to the new window position and rasterizes this frame's captured statics into them (MAX/MIN). Called from DrawShell before the screen-space passes. Implemented in SnowDeformation/Statics.cpp. */
	void RenderObjectHeightMap();

	ID3D11VertexShader* staticsVS = nullptr;
	ID3D11PixelShader* staticsPS = nullptr;
	/** @brief Trench patch (PATCH define): the landscape shell's dense-grid carve applied to OBJECT tops; real geometry where parallax cannot notch silhouettes or hold floors angle-stably. */
	ID3D11VertexShader* patchVS = nullptr;
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

protected:
	/** @brief Fills perFrameData.Stamps from the player and nearby loaded actors. Implemented in SnowDeformation/Stamping.cpp. */
	void GatherStamps(PerFrame& perFrameData);

	std::unordered_map<uintptr_t, uint8_t> snowMasks;
	std::shared_mutex snowMaskMutex;

	/** @brief Baked cells keyed by (cellX << 32) | cellY. */
	std::unordered_map<uint64_t, ShellCellData> shellCells;
	std::shared_mutex shellCellMutex;
	std::atomic<bool> shellDataDirty{ true };
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
	bool rangeInitApplied = false;

public:
	/** @brief Applies pending range-setting changes (trench window resize + map clear). Called at Prepass start; the first call applies loaded settings. */
	void ApplyRangeSettings();

protected:
	/** @brief Trail history per collision shape: key = (formID << 16) | traversal index. */
	std::unordered_map<uint64_t, float2> stampPrevPositions;

	/** @brief Last 3D-root position per loose prop (formID), rebuilt every frame from the in-range scan. The position gate runs before any collision traversal, so resting clutter costs one hash lookup per frame. */
	std::unordered_map<uint32_t, RE::NiPoint3> propPrevPositions;

	/** @brief Stillness latch per corpse (formID). Once settled, only a large accumulated displacement (dragging, explosions) wakes it, so ragdoll micro-drift cannot re-trench under a buried corpse. Erased when the actor is seen alive again. */
	struct CorpseRest
	{
		uint16_t stillFrames = 0;
		bool settled = false;
	};
	std::unordered_map<uint32_t, CorpseRest> corpseRestStates;
};
