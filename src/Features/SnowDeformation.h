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

	static constexpr const char* kAttribution =
		"Snow Deformation for Community Shaders - (c) 2026 josefplyr1 - GPL-3.0-or-later - github.com/community-shaders/skyrim-community-shaders/pull/2659";

	static constexpr uint kTextureDim = 2048;
	static constexpr uint kMaxStamps = 256;
	static constexpr uint kContactDim = 2048;
	static constexpr float kContactHalfExtent = 3072.0f;
	static constexpr float kContactNone = 1.0e30f;
	static constexpr uint kContactMaxBones = 80;
	static constexpr uint kContactMaxActors = 16;
	static constexpr uint kContactMaxCorpses = 8;
	static constexpr float kStampModeCarve = 0.0f;
	static constexpr float kStampModeMelt = 1.0f;
	static constexpr float kStampModePit = 2.0f;
	static constexpr float kStampModeCrust = 3.0f;
	static constexpr float kStampModeCone = 10.0f;
	static constexpr float kStampModeBowl = 20.0f;
	static constexpr float kStampModeBody = 40.0f;
	static constexpr float kStampImpactScale = 0.45f;
	static constexpr float kStampModeShove = kStampImpactScale;
	static constexpr uint kSpellStampReserve = 48;

	static constexpr float kUnitsPerMeter = 70.0f;
	static constexpr float kMinRangeSkinsM = 50.0f;
	static constexpr float kMaxRangeSkinsM = 200.0f;

	static constexpr uint kShellGridDim = 640;
	static constexpr float kShellGridSpacing = 8.0f;

	static constexpr int kShellWarpBands = 5;
	static constexpr float kShellWarpBandVerts[kShellWarpBands] = { 192.0f, 8.0f, 8.0f, 8.0f, 104.0f };
	static constexpr float kShellWarpBandMul[kShellWarpBands] = { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f };

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
		span += std::max(kShellGridDim * 0.5f - verts, 0.0f) * kShellWarpBandMul[kShellWarpBands - 1];
		return span * a_spacing;
	}

	static constexpr int kShellWindowDim = 1024;
	static constexpr float kShellVertexSpacing = 128.0f;
	static constexpr int kShellTexelsPerCell = 32;
	static constexpr int kShellWindowCells = kShellWindowDim / kShellTexelsPerCell;
	static constexpr float kShellMissingHeight = -100000.0f;

	static constexpr uint kSnowClassCount = 12;
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
		{ "Roads", "road" },
		{ "Dirt", "dirt" },
		{ "Grass & Fields", "grass" },
		{ "Rocks & Cliffs", "rock" },
		{ "Coast & Beach", "coast" },
		{ "Mud & Rivers", "mud" },
		{ "Other", "" },
	};

	struct TextureDefaultDef
	{
		const char* match;
		float depth;
	};
	static constexpr TextureDefaultDef kTextureDefaults[] = {
		{ "frozenmarshice", 20.0f },
	};

	static constexpr uint kShellVertexLayers = 4;
	static constexpr uint16_t kNoLandTexture = 0xFFFF;
	static constexpr size_t kMaxLandTextures = 512;

	struct ShellCellData
	{
		std::array<float, 33 * 33> height;
		std::array<std::array<uint16_t, kShellVertexLayers>, 33 * 33> layerTexture;
		std::array<std::array<uint8_t, kShellVertexLayers>, 33 * 33> layerWeight;
		std::array<uint8_t, 33 * 33> vertexAO;
		std::vector<float> fine;
		std::array<uint8_t, 4> fineLevel{};
		uint32_t worldspaceID = 0;
	};

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

	struct LandTextureEntry
	{
		std::string path;
		std::string label;
		int classIndex = (int)kSnowClassCount - 1;
		float depth = -5.0f;
		bool overridden = false;
		bool shipped = false;
		float shippedDepth = 0.0f;
	};

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
			float restLength = -1.0f;
		};
		std::vector<Limb> limbs;
		float bodyAlpha = -1.0f;
		RE::BSFixedString bodyAlphaSource;
		float anyAlpha = -1.0f;
		RE::BSFixedString anyAlphaSource;
		uint16_t alphaRecheck = 0;
		uint16_t alphaSettle = 0;
		uint16_t attachRecheck = 0;
		uint16_t collectDelay = 0;
		float dryTravel = 0.0f;
		float prevPosX = 0.0f;
		float prevPosY = 0.0f;
		bool hasPrevPos = false;
		bool dryRecollected = false;
		bool collisionFallback = false;
		uint16_t contactStarved = 0;
		bool contactStarveLogged = false;
	};

	struct Settings
	{
		bool EnableSnowDeformation = true;
		float SlumpRate = 0.1f;
		float RefillRateMultiplier = 1.0f;
		bool RefillOnlyWhenSnowing = true;
		bool PersistTrenches = true;
		float StoredTrenchFadeDays = 3.0f;
		float TrenchMemoryMB = 1.0f;
		float AccumulationPeak = 1.5f;
		bool EnableSnowAccumulation = true;
		bool PersistAccumulation = true;
		bool RecoloredSnowAccumulates = true;
		float SteepFaceThinning = 1.0f;
		float AccumulationHours = 1.0f;
		float AccumulationMeltHours = 1.0f;
		float AccumulationFadeDays = 3.0f;
		float MeltPersistence = 0.50f;
		float MeltBowlFloor = 0.11f;
		float PitDepth = 1.00f;
		float PitRadius = 50.0f;
		float ShockCloakRadius = 75.0f;
		float ShockCloakInterval = 1.50f;
		float ShockCloakStrikeScale = 0.25f;
		bool EnableLightningArcs = true;
		float LightningArcWidth = 10.0f;
		float LightningArcBrightness = 10.0f;
		float LightningArcLife = 0.30f;
		std::array<float, 3> LightningArcTint = { 0.698f, 0.620f, 1.0f };
		float ScorchStrength = 0.85f;
		float CrustRate = 0.6f;
		float CrustRadius = 90.0f;
		float CrustPrintDepth = 0.20f;
		float CrustThawMinutes = 4.0f;
		float CrustBreakOnCarve = 6.0f;
		float CrustBreakRadius = 30.0f;
		bool EnableSpellIntegration = true;
		bool NoCarveFloatingActors = true;
		float FloatingActorBand = 20.0f;
		int IncorporealMode = 1;
		bool EnableShoutCones = true;
		float ShoutMarkReach = 0.1f;
		float ShoutMarkWidth = 0.35f;
		float ShoutMarkWaist = 0.5f;
		float ShoutMarkRoundness = 1.5f;
		float ShoutMarkFadeStart = 0.25f;
		float ShoutMarkFarDepth = 0.25f;
		float ShoutChurn = 1.0f;
		float ShoutStreakSize = 8.0f;
		float ShoutStreakLength = 1.0f;
		bool LiftFrostEffects = true;
		bool LiftRunes = true;
		bool DecalsOnObjectSnow = true;
		bool RuneDecalsOnSnow = true;
		bool FrostDecalsOnSnow = true;
		float ForceTrackDepth = 0.45f;
		float ForceTrackWidth = 80.0f;
		float DashGougeScale = 0.5f;
		float ForceCarveDepth = 0.5f;
		bool CorpseElementalMarks = true;
		float CorpseEffectSeconds = 8.0f;
		float AtronachFireReach = 0.50f;
		float AtronachFireDeathRadius = 400.0f;
		float AtronachFireBurnSeconds = 6.0f;
		float AtronachFrostReach = 2.00f;
		float AtronachFrostDeathRadius = 240.0f;
		float AtronachShockReach = 1.00f;
		float AtronachShockDeathRadius = 320.0f;
		float CloakRadius = 100.0f;
		float BlastRadiusScale = 0.33f;
		float SpellMeltRate = 0.8f;
		float MeltEdgeIrregularity = 0.15f;
		std::array<float, kSnowClassCount> SnowClassDepths = { 14.0f, 18.0f, 30.0f, 30.0f, 30.0f, -8.0f, -8.0f, -8.0f, -8.0f, -8.0f, -8.0f, -8.0f };
		std::map<std::string, float> TextureDepths;
		float SkinEdgeFlankWidth = 0.0f;
		float RoadMeshesDepth = 10.0f;
		bool CityShells = false;
		std::string SnowTexturePath = "Textures\\PBR\\Landscape\\snow01.dds";
		std::string SnowPBRConfig = "";
		float TrampleZoneScale = 0.75f;
		float TrampleZoneHeight = 50.0f;
		float ItemEmbedPercent = 50.0f;
		bool ItemSink = true;
		float ItemMinRoundness = 0.20f;
		float ItemSnowGripPercent = 100.0f;
		bool CreatureSink = true;
		float UndulationStrength = 8.0f;
		float UndulationSpacing = 0.5f;
		bool Tessellation = true;
		float ParallaxShadowStrength = 0.5f;
		float ParallaxDepth = 1.0f;
		float BermHeight = 0.50f;
		float BermSteepness = 0.75f;
		float RimLip = 0.05f;
		float RimTeeth = 0.50f;
		float SnowResistance = 33.0f;
		float SnowClimateStrength = 0.75f;
		float TrackAgeing = 1.0f;
		bool WetSnowAndIce = true;
		float FreshSnowSoftness = 0.66f;
		float FreshSnowSettleHours = 24.0f;
		float TrampledFlattening = 0.5f;
		float TrenchDetailRelief = 1.0f;
		float TrenchDetailRoughness = 1.0f;
		float TrenchDetailCoverage = 1.75f;
		float ImpactReach = 0.75f;
		bool SpeedSpoil = true;
		float SpeedSpoilSize = 0.25f;
		bool ShellHorizonMarch = true;
		bool SnowGlints = true;
		bool SnowScreenSpaceShadows = true;
		bool SnowCastsShadows = true;
		bool ObjectSnowNearDetail = true;
		bool CameraAboveSnow = true;
		bool BloodOnSnow = true;
		float BloodIntensity = 1.0f;
		float BloodBurial = 0.15f;
		float BloodAgeHours = 1.0f;
		float BloodSheen = 0.5f;
		float BloodSpreadSeconds = 1.5f;
		uint32_t DeformMapResolution = 4096;
		bool VRAMAwareTrenchMap = true;
		uint32_t ObjectTrenchResolution = 2048;
		float ObjectPrintRimWidth = 7.5f;
		float RangeTrenchesM = 200.0f;
		float RangeActorTrenchesM = 200.0f;
		float RangeSkinsM = kMaxRangeSkinsM;
		float ObjectRasterReachM = 100.0f;
		float SkinDepthBias = 64.0f;
		float SkinSlopeDepthBias = 0.0f;
		float SkinTessCapPx = 16.0f;
		float SlopeDrape = 1.0f;
		float LODSnowSensitivity = 0.5f;
		bool HorizonSnow = true;
		bool ProjSnowMatch = true;
		float SeasonsSnowMaxAngle = 87.0f;
		bool MultipassSnowFollowsPaint = true;
		bool SnowTexturedRecolor = true;
		bool ProjSnowNormals = true;
		bool UnprojectedShapesBare = true;
		bool SnowFollowsParallax = true;
		bool FlattenParallaxUnderSnow = true;
		float ShelterMaxHeight = 150.0f;
		float ShelterDriftAngle = 30.0f;
		float ObjectMeetBand = 12.0f;
		bool LODObjectSnow = true;
		bool VolumeSnow = false;
		float VolumeSnowDepth = 10.0f;
		float VolumeSnowCoverage = 0.15f;
		float VolumeSnowOverhang = 5.0f;
		float VolumeSnowRounding = 5.0f;
		int VolumeLevels = 4;
		int VolumeFineLevels = 6;
		float VolumeMarchStep = 0.25f;
		float VolumeDetailDistance = 1000.0f;
		bool VolumeSkipEmptyCells = true;
		float VolumeForwardBias = 0.45f;
		int VolumeRingShape = 1;
		float VolumeVerticalBias = 0.0f;
		float VolumeMaxDistance = 8000.0f;
		bool VolumeStaggeredCapture = true;
		bool VolumeDirtyBricks = true;
		float VolumeVoxelSize = 1.0f;
		float VolumeSnowMaxSlopeDeg = 69.0f;
		float VolumeSkyExposurePct = 0.0f;
	};

	static constexpr uint32_t kIceHistoryBins = 128;

	/** @brief GPU-side settings, appended to the shared FeatureData cbuffer (b6). Layout must match SnowDeformationSettings in SharedData.hlsli. */
	struct alignas(16) SettingsGPU
	{
		float2 WindowOrigin;
		float InvWorldSize;
		float ParallaxFollow;

		uint DebugTerrainOverlay;
		float LODReplaceStart;
		float LODReplaceFadeInv;
		float LODSnowSensitivity;

		float SnowIsLinear;
		float SnowRoughnessScale;
		float LODReplaceEnable;
		float SnowHasNormal;

		float SnowTexturedEnable;
		float ProjSnowEnable;
		float LODObjectEnable;
		float ProjUnauthoredThreshold;

		DirectX::XMINT2 DeformMapOrigin;
		float LandWetPrint;
		float PrintFullDepth;

		float2 WaterWindowOrigin;
		float WaterWindowTexel;
		float WaterWindowDim;

		float LandIce;
		float ProjSnowNormalEnable;
		float SnowSpecularLevel;
		float LandRecolorLumLo;

		DirectX::XMINT4 PrintRect;

		float ParallaxFlatten;
		float IceClock;
		float padIceClock[2];

		float4 IceHistory[kIceHistoryBins];
	};
	STATIC_ASSERT_ALIGNAS_16(SettingsGPU);
	static_assert(offsetof(SettingsGPU, IceClock) == 132 && offsetof(SettingsGPU, IceHistory) == 144 && sizeof(SettingsGPU) == 2192,
		"SettingsGPU must match the fxc-reflected SnowDeformationSettings in SharedData.hlsli");

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
		DirectX::XMINT2 MapOrigin;

		float TexelSize;
		uint StampCount;
		float RefillAmount;
		uint ClearMap;

		float StampFalloffStart;
		float BermReach;
		float2 WindBias;

		float DeltaTime;
		float MeltPersistence;
		float MeltFloorStart;
		float MeltEdgeNoise;
		float CrustPrintDepth;
		float CrustThaw;
		float CrustBreakOnCarve;
		float SlumpRate;

		uint InjectValid;
		float GameDeltaTime;
		uint DebugActivityView;
		float AgeingClock;

		DirectX::XMINT4 RingRects[2];
		uint RingRectCount;
		uint RingTotalTexels;
		uint ForceAllDirty;
		float TrackRebase;

		float4 Stamps[kMaxStamps];
		float4 StampEnds[kMaxStamps];
		float4 StampCones[kMaxStamps];

		float2 ContactCenter;
		float ContactHalfExtent;
		float ContactDim;
		float2 TerrainWindowOrigin;
		float TerrainTexelSize;
		float TerrainDim;
		float2 ViewCenter;
		float ViewHalf;
		float BodyPrint;
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		float HasRoadRaster;
		uint PassClock;
		uint PassGap;
		uint PassPad[2];
		float4 ShoutShape;
		float4 ShoutChurn;
		float UndulationAmp;
		float UndulationScale;
		float AccumulationScale;
		float DrawnPad;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(offsetof(PerFrame, ContactCenter) == 12432, "PerFrame tail must match the fxc-reflected layout of DeformationUpdateCS");
	static_assert(offsetof(PerFrame, UndulationAmp) == 12544, "PerFrame tail must match the fxc-reflected layout of DeformationUpdateCS");

	static constexpr float kTrenchWallSharpness = 66.0f;

	Settings settings;

	static constexpr float kBaseRefillTime = 700.0f;
	static constexpr float kReferenceSnowDensity = 2.0f;
	static constexpr float kMaxSnowfallIntensity = 4.0f;

	/** @brief Snowfall intensity from the weather records' precipitation density, faded across weather transitions. 1.0 = reference-density snowfall; 0 for ash and in a No Sky worldspace. */
	float ComputeSnowfallIntensity() const;
	/** @brief The same transition-faded precipitation density for any weather class (snow, rain). */
	float ComputePrecipitationIntensity(RE::TESWeather::WeatherDataFlag a_flag) const;
	struct SnowWeatherRange
	{
		bool scanned = false;
		uint32_t weathers = 0;
		float heaviest = 1.0f;
		float split = 0.5f;
		std::string heaviestName;
		std::string splitName;
	};
	/** @brief Scans the loaded weathers the first time it is asked, after the data has loaded. */
	const SnowWeatherRange& SnowWeathers();
	SnowWeatherRange snowWeatherRange;
	/** @brief The player's worldspace when it is flagged No Sky (Blackreach), else null. Outdoor weather state holds there as it does indoors. */
	static RE::TESWorldSpace* NoSkyWorldspace();
	/** @brief The first kNotSnowKeywords entry (the MATO and LOD batch vetoes' list) found in a name, case-insensitive, or null. */
	static const char* NotSnowKeywordIn(const char* a_name);
	float snowfallIntensity = 0.0f;

	struct GameClock
	{
		float lastHours = -1.0f;
		float timescale = 20.0f;
		float elapsedHours = 0.0f;
		bool reversed = false;
		/** @brief Takes this frame's reading; a set a_unarm re-arms instead of measuring. @return The calendar's hours passed, negative without a calendar. */
		float Read(std::atomic<bool>& a_unarm);
	};
	GameClock gameClock;
	std::atomic<bool> gameClockUnarm{ false };
	std::atomic<float> gameClockHours{ -1.0f };
	/** @brief Takes this frame's reading. Call once, before anything that consumes it. */
	void TickGameClock();
	GameClock weatherClock;
	std::atomic<bool> weatherClockUnarm{ false };

	ConstantBuffer* perFrame = nullptr;
	Texture2D* deformationTextures[2] = { nullptr, nullptr };

	Texture2D* bermFieldTexture = nullptr;

	/** @brief SRV of the deformation map (the single canonical texture), for shader sampling and debug UI. Physical (toroidal) layout - readers translate through DeformMapOrigin. */
	ID3D11ShaderResourceView* GetDeformationSRV() const { return deformationTextures[0] ? deformationTextures[0]->srv.get() : nullptr; }
	/** @brief SRV of the baked berm field; null before SetupResources. */
	ID3D11ShaderResourceView* GetBermFieldSRV() const { return bermFieldTexture ? bermFieldTexture->srv.get() : nullptr; }

	Texture2D* passStateTexture = nullptr;
	Texture2D* bodyReachTexture = nullptr;
	Texture2D* trackClockTexture = nullptr;
	ID3D11ComputeShader* trackRebaseCS = nullptr;
	ID3D11ComputeShader* GetTrackRebaseCS();
	static constexpr float kTrackAgeSpan = 24.0f;
	/** @brief ShellCB::TrackAge: the ageing clock now and the age per clock unit (Track Ageing / kTrackAgeSpan; 0 = tracks never age). */
	float4 TrackAgeConstants() const;
	/** @brief The ageing clock in the frame the track dates stand in now: ahead of ageingClock by a rebase the next update has yet to apply. */
	float TrackDateClock() const { return snowClimate.ageingClock + snowClimate.ageingRebase; }
	/** @brief Mirrors the unwrapped ageing clock into the store, under its lock; after TickSnowAgeing. */
	void TickTrenchAgeing();
	static constexpr float kTrackAgeHorizon = 128.0f;
	double passClockSeconds = 256.0;
	static constexpr UINT kUntouchedTilesSlot = 57;
	static constexpr UINT kBodyReachSlot = 56;
	static constexpr UINT kTerrainWetSlot = 58;

	Texture2D* bloodMapTexture = nullptr;
	Texture2D* bloodClockTexture = nullptr;
	bool bloodMapWritten = false;
	float4 bloodWrittenBox = {};
	bool bloodLatchOff = false;
	float bloodBurialClock = 0.0f;
	static constexpr float kRefillStep = 1.0f / 1024.0f;
	float refillBank = 0.0f;
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
		bool mask = false;
		float2 clock{ 0.0f, 0.0f };
		float decalId = 0.0f;
	};
	std::vector<BloodCapture> bloodCaptures;
	struct BloodSeen
	{
		uint32_t frame = 0;
		const void* vb = nullptr;
		RE::NiPoint3 position;
		double firstSeconds = 0.0;
		float2 firstClock{ 0.0f, 0.0f };
		uint32_t fineEpoch = 0;
		bool fineMissing = false;
		uint32_t fineRetryFrame = 0;
		bool boundsKnown = false;
		float2 boundsMin{ 0.0f, 0.0f };
		float2 boundsMax{ 0.0f, 0.0f };
	};
	double bloodRenderSeconds = 0.0;
	std::unordered_map<const void*, BloodSeen> bloodSeen;
	struct BloodDisc
	{
		float x, y, z, radius;
		float r, g, b, amount;
	};
	std::vector<BloodDisc> bloodDiscQueue;
	std::mutex bloodDiscMutex;
	std::unordered_set<std::string> bloodPathsLogged;
	std::vector<BloodCapture> bloodOverlays;
	std::unordered_set<const void*> bloodOverlaySet;
	static constexpr uint32_t kBloodCopyCount = 5;
	winrt::com_ptr<ID3D11Texture2D> bloodPreDecalTex[kBloodCopyCount];
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreDecalSRV[kBloodCopyCount];
	winrt::com_ptr<ID3D11Texture2D> bloodPreSnowTex[kBloodCopyCount];
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreSnowSRV[kBloodCopyCount];
	winrt::com_ptr<ID3D11Texture2D> bloodPreDecalMasksTex;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreDecalMasksSRV;
	bool bloodPreDecalValid = false;
	bool bloodPreSnowValid = false;
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
		float4 Spread;
		float4 OverlayRect;
		float4 ViewProjRow0;
		float4 ViewProjRow1;
		float4 ViewProjRow2;
		float4 ViewProjRow3;
		float4 CameraAdjust;
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
	bool bloodShadersFailed = false;
	bool bloodResourcesLogged = false;
	bool bloodDepositLogged = false;
	bool bloodOverlayLogged = false;
	bool EnsureBloodResourcesImpl();
	std::vector<RE::NiTransform> bloodPaletteScratch;
	uint32_t bloodDepositsLast = 0;
	uint32_t bloodDiscsLast = 0;
	uint32_t bloodSeenLive = 0;
	bool EnsureBloodResources();
	void ReleaseBloodShaders();
	/** @brief One list of captures through the rigid and skinned blood shaders; a_rigidVS / a_skinnedVS replace the map's vertex shaders, a_instances the map's four seam instances. */
	uint32_t DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, BloodCB& a_cb, ID3D11VertexShader* a_rigidVS = nullptr, UINT a_instances = 4u, bool a_ownClock = false, ID3D11VertexShader* a_skinnedVS = nullptr);
	/** @brief Joins the frame's overlay set: the decal's screen rectangle, and at the frame's first decal the pre-decal copy. Once per geometry. */
	void RegisterDecalOverlay(const BloodCapture& a_capture);
	/** @brief Whether the overlay can change a pixel of a decal with this world bound: a loaded capture's skin reaches it (the skin cull's sphere plus kDecalOverlayMaxLift), or the ground snow draws (its contest reads the pre-decal Masks under every decal), or volume snow is on. Implemented in Blood.cpp. */
	bool DecalReachesSnow(const RE::NiBound& a_bound);
	static constexpr float kDecalOverlayMaxLift = 24.0f;
	std::vector<float3> coatReachBounds[2];
	static constexpr float kCoatReachCell = 256.0f;
	std::vector<uint64_t> coatReachGrid;
	float coatReachOrigin[2] = {};
	int coatReachDim = 0;
	int coatReachWords = 0;
	uint32_t coatReachFrame = UINT32_MAX;
	size_t coatReachMarked = 0;
	void MarkCoatReach(float a_x, float a_y, float a_radius);
	bool decalReachGateOff = false;
	uint32_t decalReachMisses = 0;
	uint32_t decalReachMissesLast = 0;
	/** @brief From the capture hook: one of the engine's runtime decals that is not blood. Overlay only, never the blood map. */
	void CaptureDecalOverlay(RE::BSRenderPass* a_pass);
	bool DecalOverlayWanted() const { return settings.BloodOnSnow || settings.DecalsOnObjectSnow; }
	/** @brief Object snow paints the rune glyphs and frost marks itself where they reach it: when the overlay would otherwise take their decals (Statics.cpp CaptureDecalOverlay's gate). */
	bool CoatPaintsMarks() const { return settings.DecalsOnObjectSnow && settings.ProjSnowMatch; }
	Texture2D* decalMaskTexture = nullptr;
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
	std::vector<BloodFineRequest> bloodFineQueue;
	winrt::com_ptr<ID3D11Texture2D> bloodPreSkinDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreSkinDepthSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodPreSkinDepthUAV;
	bool bloodPreSkinDepthThisFrame = false;
	/** @brief bloodPreSkinDepth at a_width x a_height (the skins' viewport extent), recreated when that changes. */
	bool EnsureBloodPreSkinDepth(UINT a_width, UINT a_height);

	static constexpr uint32_t kUndulationFieldDim = 2048;
	static constexpr float kUndulationFieldTexel = 16.0f;
	static constexpr float kUndulationFieldHalfExtent = kUndulationFieldDim * kUndulationFieldTexel * 0.5f;
	static constexpr float kUndulationFieldSnap = 512.0f;

	Texture2D* undulationFieldTexture = nullptr;
	ID3D11ComputeShader* undulationFieldCS = nullptr;
	ID3D11ComputeShader* GetUndulationFieldCS();
	static constexpr uint32_t kNoiseHashDim = 256;
	static constexpr uint32_t kNoiseHashSlot = 109;
	Texture2D* noiseHashTexture = nullptr;
	bool noiseHashBaked = false;
	ID3D11ComputeShader* noiseHashCS = nullptr;
	ID3D11ComputeShader* GetNoiseHashCS();
	/** @brief Bakes the hash table on the first call after it was created; the bound SRV, or null until baked. */
	ID3D11ShaderResourceView* NoiseHashSRV();
	static constexpr uint32_t kTrenchDetailSlot = 112;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchDetailNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchDetailHeightSRV;
	bool trenchDetailAttempted = false;
	void EnsureTrenchDetailTextures();
	bool debugTrenchDetailView = false;
	struct alignas(16) UndulationFieldCB
	{
		float2 FieldOriginWorld;
		float FieldTexel;
		float FieldScale;
	};
	STATIC_ASSERT_ALIGNAS_16(UndulationFieldCB);
	ConstantBuffer* undulationFieldCB = nullptr;
	float2 undulationFieldCenter = { 0.0f, 0.0f };
	bool undulationFieldValid = false;
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

	static constexpr uint kStampTileCap = 16384;
	winrt::com_ptr<ID3D11Buffer> stampTileBuffer;
	winrt::com_ptr<ID3D11ShaderResourceView> stampTileSRV;
	std::vector<uint32_t> stampTileScratch;
	std::vector<uint32_t> stampTileBits;
	uint32_t stampTilesLast = 0;
	/** @brief Fills stampTileScratch with the deduped physical tiles the stamp inputs touch (wrap-aware). Returns the count, or UINT32_MAX on overflow past kStampTileCap. */
	uint32_t BuildStampTileList(const PerFrame& a_data);

	winrt::com_ptr<ID3D11Texture2D> occupancyTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> occupancyUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> occupancySRV;
	winrt::com_ptr<ID3D11Buffer> evolveTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> evolveTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> evolveTileSRV;
	winrt::com_ptr<ID3D11Buffer> evolveArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> evolveArgsUAV;
	winrt::com_ptr<ID3D11Buffer> snapshotTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> snapshotTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> snapshotTileSRV;
	winrt::com_ptr<ID3D11Buffer> snapshotArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> snapshotArgsUAV;
	bool evolveSnapshotOff = false;
	bool tileGridsNeedInit = true;
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

	winrt::com_ptr<ID3D11Texture2D> bermDirtyTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermDirtyUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> bermDirtySRV;
	winrt::com_ptr<ID3D11Buffer> bermTileBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermTileUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> bermTileSRV;
	winrt::com_ptr<ID3D11Buffer> bermArgsBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermArgsUAV;
	uint32_t bermTilesLast = 0;
	winrt::com_ptr<ID3D11Texture2D> bermOccupancyTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> bermOccupancyUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> bermOccupancySRV;
	winrt::com_ptr<ID3D11Texture2D> untouchedTilesTexture;
	winrt::com_ptr<ID3D11UnorderedAccessView> untouchedTilesUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> untouchedTilesSRV;
	/** @brief Returns BermFieldCS.hlsl:UntouchedCS, compiling it on first use. */
	ID3D11ComputeShader* GetUntouchedTilesCS();
	ID3D11ComputeShader* untouchedTilesCS = nullptr;

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

	Texture2D* shellTerrainTexture = nullptr;
	Texture2D* shellTerrainWet = nullptr;
	Texture2D* shellTerrainBare = nullptr;
	std::vector<uint8_t> shellBareScratch;
	static constexpr UINT kTerrainBareSlot = 26;
	static constexpr UINT kTBMaskSlot = 110;
	bool shellWindowLandBlend = false;
	float shellWindowLandBlendStart = 1.0f;
	ID3D11ComputeShader* shellTerrainCutCS = nullptr;
	ID3D11ComputeShader* GetShellTerrainCutCS();
	/** @brief Rebuilds shellTerrainWet when its inputs changed. Call after the water cover pass, before any landscape-shell pass. */
	void UpdateTerrainWet();
	/** @brief SampleTerrain's t21: the cut window while it is current, else the raw window. */
	ID3D11ShaderResourceView* TerrainWetSRV() const;
	uint64_t shellTerrainWetKey[3] = {};
	bool shellTerrainWetValid = false;
	/** @brief The SRV DrawShell and the caster bind at t27 (WaterWindow). */
	ID3D11ShaderResourceView* ShellWaterSRV() const;
	static constexpr int kShellFineCells = 9;
	static constexpr float kShellFineTexel = 32.0f;
	static constexpr int kShellFineDim = kShellFineCells * 128;
	static constexpr size_t kShellFineCacheCells = 128;
	std::deque<uint64_t> shellFineOrder;
	/** @brief Fills a_data.fine from the land's quad meshes; false (and fine empty) when any quad had no readable vertex copy or its lattice did not line up with heights[]. */
	bool ReadLandMeshHeights(RE::TESObjectLAND* a_land, ShellCellData& a_data);
	/** @brief Each frame: loaded cells whose bake ran before the engine subdivided their land mesh (289 vertices at hook time, 4225 later) get their fine data once the meshes have grown; a hard refusal is remembered so the cell is not re-tried. Sets shellFineDirty. */
	void RefreshLandMeshHeights();
	/** @brief The quad's drawn geometry: geom[q] when it holds the subdivided mesh, else the largest BSTriShape under mesh[q]. */
	static RE::BSTriShape* LandQuadGeometry(RE::TESObjectLAND::LoadedLandData* a_loaded, uint32_t a_quad);
	uint32_t fineRefreshFrame = 0;
	std::atomic<bool> shellFineDirty{ false };
	std::unordered_map<uint64_t, uint32_t> shellFineRefused;
	/** @brief 0 none, 1 = 17x17, 2 = 33x33, 4 = 65x65 vertices per quad. */
	static uint32_t LandQuadLevel(RE::BSTriShape* a_geometry);
	Texture2D* shellTerrainFine = nullptr;
	float shellFineOriginX = 0.0f;
	float shellFineOriginY = 0.0f;
	bool shellFineValid = false;
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
	static constexpr float kShellTessTargetPx = 16.0f;
	/** @brief kShellTessTargetPx in this frame's render pixels (TessScreen.x): an upscaler's render scale applied. */
	float ShellTessTargetRenderPx() const;

	static constexpr uint32_t kClimateLUTRows = 3;
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

		float2 GridToTerrainOffset;
		float2 GridToDeformOffset;

		float WarpedHalfSpan;
		uint LayoutKeeper6;
		uint TerrainDim;
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
		float TrenchDetailRelief;
		float TrenchDetailRoughness;

		float LayoutKeeper5;
		float TrenchDetailCoverage;
		float ShellCullBare;
		float SkinFadeStart;

		float SkinFadeEnd;
		float ObjectLiftCap;
		float2 ObjectHeightCenter;

		float ObjectHeightHalfExtent;
		float CrispShadows;
		float ScreenSpaceShadowsActive;
		float UndulationAmp;

		float UndulationScale;
		float SkinTessCapSlope;
		float PointLightsActive;
		float SkylightingActive;

		float HasSnowHeight;
		float SnowReliefDepth;
		float StaticsDebugView;
		float BermHeightAmp;

		float SpeedSpoil;
		float LayoutKeeper9;
		float DebugNoFarPad;
		float LayoutKeeper4;

		float LayoutKeeper7;
		float TrenchDetailShadow;
		float LayoutKeeper3;
		float ObjectMeetBand;

		float BermReach;
		uint LayoutKeeper10;
		float SeamRampInv;
		float BermBakeActive;

		float4 SeamBounds;

		float4 ExclusionFieldWindow;

		float4 SnowParallax;

		float4 SpellShading;
		float4 TrackAge;
		float4 padCrustLook2;
		float4 BorderStyle;
		float4 CompactLook;
		float4 RimStyle;

		float4 UndulationFieldWindow;

		DirectX::XMINT2 DeformMapOrigin;
		DirectX::XMINT2 ShellFlags;
		float4 FineWindow;
		float4 SlopeDrape;
		float4 DebugSkinDepth;
		float4 BloodLook;
		float4 BloodLook2;
		float4 BloodLook3;
		float4 TrampleStyle;
		float4 TessScreen;
		float4 FieldDims;
		float4 SnowClimate;
		float4 SnowClimate2;
		float4 SnowClimateOverride;
		float4 SnowClimateLUT[kClimateLUTRows];
	};
	STATIC_ASSERT_ALIGNAS_16(ShellCB);
	static_assert(offsetof(ShellCB, TrampleStyle) == 800, "ShellCB tail must match the fxc-reflected layout of both shells");
	static_assert(offsetof(ShellCB, SnowClimate) == 848, "ShellCB tail must match the fxc-reflected layout of SnowShell.hlsl");
	static_assert(offsetof(ShellCB, SnowClimateLUT) == 896 && sizeof(ShellCB) == 944, "ShellCB tail must match the fxc-reflected layout of SnowShell.hlsl");

	struct PointShadowLightData
	{
		DirectX::XMFLOAT4X4 LightTransform;
		uint32_t SliceIndex;
		uint32_t LightType;
		float ShadowRadius;
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
	winrt::com_ptr<ID3D11Query> patchStatsQuery[kShellStatsRing][2];
	bool patchStatsArmed = false;
	bool shellStatsIssued[kShellStatsRing] = {};
	int shellStatsRing = 0;
	ShellStatsResult shellStatsLast;
	ShellStatsResult staticsStatsLast;
	ShellStatsResult patchStatsLast;
	bool EnsureShellStatsQueries();
	void ReadShellStatsQueries(ID3D11DeviceContext* a_context);
	bool shellDeferredThisFrame = false;
	Texture2D* shellDeferredAttr[3] = {};
	winrt::com_ptr<ID3D11DepthStencilState> shellDeferredDepthState;
	bool EnsureDeferredResources();
	ID3D11PixelShader* shellPSDeferred = nullptr;
	ID3D11PixelShader* GetShellPSDeferred();
	ID3D11PixelShader* shellPSDeferredConditionView = nullptr;
	std::atomic<int> conditionViewShaderState = 0;
	std::future<bool> conditionViewCompile;
	/** @brief The condition-view deferred PS when the view is on and compiled; null otherwise (the normal shading draws). */
	ID3D11PixelShader* ConditionViewPS();

	Texture2D* marchHeight[3] = {};
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
		float4 Sun;
	};
	STATIC_ASSERT_ALIGNAS_16(MarchCacheCB);
	ConstantBuffer* marchCacheCB = nullptr;
	bool marchCacheValid = false;
	bool marchFarValid = false;
	uint64_t marchCacheKey = 0;
	uint32_t marchCacheTerrainVersion = 0;
	float marchCacheAgeingClock = 0.0f;
	float marchCacheFineOrigin[2] = {};
	uint32_t marchCacheRowCursor = 0;
	uint32_t marchFarRowCursor = 0;
	uint32_t marchCacheFastRows = 0;
	uint32_t marchFarFastRows = 0;
	uint64_t bermBakeSeq = 0;
	uint64_t marchCacheBermSeq = 0;
	float marchSunDir[3] = { 0.0f, 0.0f, 1.0f };
	bool marchSunValid = false;
	uint32_t marchSunRowCursor = 0;
	uint32_t marchSunFastRows = 0;
	static constexpr uint32_t kMarchCensusFrames = 300;
	uint32_t marchIdleFrames = 0;
	static constexpr uint32_t kMarchIdleFrames = 30;
	uint64_t marchIdleSkipped = 0;
	bool marchIdleOff = false;
	std::array<uint8_t, kMarchCensusFrames> marchCensus{};
	uint32_t marchCensusCursor = 0;
	uint32_t marchCensusFilled = 0;
	/** @brief Brings the caches up to date and binds them at PS t106-t108, the sun envelope at t100 (DrawShell, after the shell CB upload). False = no caches this frame: the marches must be switched off, as they read nothing else. */
	bool UpdateMarchCaches(const ShellCB& a_cb);
	void UnbindMarchCaches();
	void ReleaseMarchCache();
	ID3D11PixelShader* shellPSPrepassAttr[2] = {};
	ID3D11PixelShader* GetShellPSPrepassAttr(bool a_keepHidden);

	enum class EarlyPrepass : uint8_t
	{
		Off,
		Drawing,
		NotDrawing,
		NeedsTerrainBlending,
		Unavailable
	};
	EarlyPrepass shellEarlyState = EarlyPrepass::Off;
	uint32_t shellEarlyFrame = UINT32_MAX;
	std::unique_ptr<ShellCB> shellEarlyCB;
	D3D11_VIEWPORT shellEarlyViewport{};
	uint32_t shellEarlyViewMismatches = 0;
	D3D11_VIEWPORT shellDrawViewport{};
	uint32_t shellDrawViewportFrame = UINT32_MAX;
	bool shellDrawGroundGated = false;
	uint32_t shellDrawTerrainVersion = 0;
	float shellDrawDynRes[2] = {};
	Texture2D* shellEarlyPending = nullptr;
	Texture2D* shellEarlyMarks = nullptr;
	bool EnsureEarlyPrepassResources();
	void DrawShellEarlyPrepass();
	ID3D11PixelShader* shellPSPrepassEarly = nullptr;
	ID3D11PixelShader* GetShellPSPrepassEarly();

	ID3D11PixelShader* shellPSEarlyResolve = nullptr;
	ID3D11PixelShader* GetShellPSEarlyResolve();

	uint32_t shellSssSyncFrame = UINT32_MAX;
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

	uint32_t skinSssMarchFrame = UINT32_MAX;
	ID3D11ShaderResourceView* shellGroundMarksSRV = nullptr;
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
	Texture2D* sssFirstMarchDepth = nullptr;
	uint32_t sssFirstMarchFrame = UINT32_MAX;
	D3D11_BOX skinSssRectLast{};
	Texture2D* sssChangedTiles = nullptr;
	ID3D11ComputeShader* skinSssSyncMarkCS = nullptr;
	/** @brief DepthSyncCS's SkinSssSyncMarkCS entry (SKIN_SSS_MARK), compiled on first use. */
	ID3D11ComputeShader* GetSkinSssSyncMarkCS();
	/** @brief Screen-Space Shadows' raymarch with SSS_CHANGED_TILES, for its current scaled sample count (sssChangedRaymarchSamples); null without that feature. */
	ID3D11ComputeShader* GetSssChangedRaymarchCS();
	ID3D11ComputeShader* sssChangedRaymarchCS = nullptr;
	uint32_t sssChangedRaymarchSamples = 0;
	bool sssChangedMarchOff = false;
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
	winrt::com_ptr<ID3D11ShaderResourceView> shellTestDepthSRV;
	winrt::com_ptr<ID3D11Texture2D> preSnowDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> preSnowDepthSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> preSnowDepthUAV;
	winrt::com_ptr<ID3D11RenderTargetView> preSnowDepthRTV;
	bool preSnowDepthThisFrame = false;
	bool preSnowDepthDeferred = false;
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
	winrt::com_ptr<ID3D11DepthStencilState> shellPrepassMainDepthState;
	winrt::com_ptr<ID3D11DepthStencilState> shellFillDepthState;

	ConstantBuffer* shellCB = nullptr;

	/** @brief Loads a DDS through the GAME's resource system, so archives resolve and a modlist override still wins. Shared by the snow and trench detail textures - a second copy would drift. */
	static bool LoadGameDDS(const std::string& a_dataRelativePath, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv);
	winrt::com_ptr<ID3D11RasterizerState> shellRasterState;
	/** @brief Skin draws' raster state: the shell's plus SkinDepthBias / SkinSlopeDepthBias, rebuilt when either slider moves. The decal variant adds the game's decal depth bias (-1, slope -0.65, clamp -100) for skins of decal-mode draws (CapturedSnowStatic::decalDepth). Implemented in SnowDeformation/Shell.cpp. */
	ID3D11RasterizerState* GetSkinRasterState(bool a_decal = false);
	winrt::com_ptr<ID3D11RasterizerState> skinRasterState;
	winrt::com_ptr<ID3D11RasterizerState> skinDecalRasterState;
	float skinRasterBiasBuilt = 0.0f;
	float skinRasterSlopeBuilt = 0.0f;
	static constexpr float kDecalViewportMaxDepth = 0.99997199f;
	winrt::com_ptr<ID3D11DepthStencilState> shellDepthState;

	/** @brief Returns the depth sync compute shader (shell depth -> Terrain Blending's blended depth copies), compiling it on first use. Implemented in SnowDeformation/Shell.cpp. */
	ID3D11ComputeShader* GetDepthSyncCS();
	ID3D11ComputeShader* depthSyncCS = nullptr;

	int staticsDebugView = 0;

	winrt::com_ptr<ID3D11ShaderResourceView> lightingCloudShadowSRV;

	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowDiffuseSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowNormalSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowRmaosSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> shellSnowHeightSRV;
	bool shellSnowTextureIsPBR = false;
	bool shellSnowTextureAttempted = false;

	float snowGlintLogDensity = 6.0f;
	float snowGlintMicroRoughness = 0.3f;
	float snowGlintDensityRandomization = 5.0f;
	float snowGlintScreenSpaceScale = 1.0f;
	float snowRoughnessScale = 0.7f;
	float snowSpecularLevel = 0.02f;
	float snowDisplacementScale = 1.0f;
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

		float2 LODTileBase;
		float LODTileSpan;
		float LODSnowSensitivity;

		float4 LODTileValid;
	};
	STATIC_ASSERT_ALIGNAS_16(WindowFillCB);
	ConstantBuffer* windowFillCB = nullptr;

	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11ShaderResourceView>> lodTileCache;
	std::unordered_set<uint64_t> lodTileMisses;
	/** @brief Fetches (or loads) the level-32 LOD diffuse tile with the given SW cell coords for the current fill worldspace. Implemented in SnowDeformation/TerrainData.cpp. */
	ID3D11ShaderResourceView* GetLODTile(const std::string& a_worldspace, int a_cellX, int a_cellY);

	/** @brief Fills sentinel window texels from the Terrain Shadows xLODGen heightmap (height + snow-line coverage, provenance in .w). Runs after each window upload. Implemented in SnowDeformation/TerrainData.cpp. */
	void FillShellWindowFromHeightmap();
	ID3D11ComputeShader* GetWindowFillCS();
	ID3D11ComputeShader* windowFillCS = nullptr;
	std::string lastFillWorldspace;

	/** @brief Nominal (untrampled) snow depth in world units at a world XY, resolved from the baked cell data and the class depth sliders. Returns a_missing where no cell is baked (interiors, unvisited land). Implemented in SnowDeformation/TerrainData.cpp. */
	float GetNominalSnowDepthAt(float a_x, float a_y, float a_missing);

	/** @brief Thread-safe count of baked cells, for the settings UI. */
	size_t ShellCellCountForUI()
	{
		const std::shared_lock lock(shellCellMutex);
		return shellCells.size();
	}

	uint32_t shellStatCellsInWindow = 0;
	uint32_t shellStatSnowTexels = 0;
	float shellStatMinHeight = 0.0f;
	float shellStatMaxHeight = 0.0f;

	struct SmoothedNormalsEntry;
	struct CapturedSnowStatic
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		bool road;
		bool fadeExempt;
		float projThreshold;
		float projNoiseScale;
		float projNoiseTiling;
		float projNoiseWeight;
		bool forceRounded;
		bool plankFamily;
		bool projReal;
		bool fullCoat;
		bool lodBatch;
		bool decalDepth;
		bool alphaTested;
		bool snowTex;
		bool matoSnow;
		bool mayParallax;
		winrt::com_ptr<ID3D11ShaderResourceView> parallaxSRV;
		float4 parallaxUV = {};
		float4 parallax = {};
		SmoothedNormalsEntry* smoothEntry = nullptr;
		uint32_t smoothFrame = UINT32_MAX;
		uint32_t smoothResets = 0;
	};

	std::vector<CapturedSnowStatic> capturedStatics;
	uint32_t capturedRoads = 0;
	uint32_t capturedLoaded = 0;
	uint32_t capturedLoadedPrev = 0;
	struct CapturedWater
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		RE::NiTransform world;
		float planeConstant;
	};
	std::vector<CapturedWater> capturedWater;
	/** @brief Fills capturedWater from TESWaterSystem::waterObjects (every live body's shape, culled or not); not from the water shader's draws, which also carry the LOD quadtree, skirts, the displacement mesh and the waterline-crossing effect. */
	void GatherWaterObjects();
	ankerl::unordered_dense::set<void*> capturedStaticsSet;
	std::atomic<uint32_t> statCapturedStatics{ 0 };

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
	uint8_t cameraProbeState = 0;
	uint32_t cameraProbeFired = 0;
	uint32_t cameraProbeCamState = 0;
	bool cameraProbeDown = false;
	float cameraProbeZ = 0.0f;
	float cameraProbeSurface = 0.0f;

	winrt::com_ptr<ID3D11Texture2D> landMasksCopyTex;
	winrt::com_ptr<ID3D11ShaderResourceView> landMasksCopySRV;
	bool masksCopyGateOff = false;
	winrt::com_ptr<ID3D11Texture2D> preDecalMasksTex;
	winrt::com_ptr<ID3D11ShaderResourceView> preDecalMasksSRV;
	uint32_t preDecalMasksFrame = UINT32_MAX;
	void CapturePreDecalMasks();

	/** @brief Copies a_box (all of it when null) of the resource behind a_srcSRV into an owned SRV-only texture sized to a_extent's far corner (a_box's when null, the source's when both are), recreating it when that size or the format changes. The SRV doubles as the validity signal (nulled by callers on invalid frames), so it is rebuilt even when the texture itself is still current. Implemented in SnowDeformation/Shell.cpp. */
	static void CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
		winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, const D3D11_BOX* a_box = nullptr, const D3D11_BOX* a_extent = nullptr);

	winrt::com_ptr<ID3D11ShaderResourceView> shadowAtlasLiveSRV;
	/** @brief The atlas the crisp cascade shadows sample this frame (in place), or null (the VSM path). */
	ID3D11ShaderResourceView* ShadowAtlasSRV() const { return shadowAtlasLiveSRV.get(); }
	winrt::com_ptr<ID3D11SamplerState> shadowCmpSampler;
	winrt::com_ptr<ID3D11SamplerState> shellLinearSampler;

	/** @brief Called from State::Draw while the game renders the shadow MASK (Utility shader, RenderShadowmask); the only point where PS t4 genuinely holds the sun cascade atlas (at any other time it holds whatever texture the last draw bound). Injects the trench patch as a caster, then keeps the atlas for the crisp shell shadows to read in place. Same trigger VolumetricShadows and Skylighting use. Implemented in SnowDeformation/Shadows.cpp. */
	void CaptureShadowAtlas();

	/** @brief Lazily compiles the trench patch's shadow-caster VS (SNOW_PATCH_INDEXED, drawn through patchCasterIB). Implemented in SnowDeformation/Statics.cpp. */
	ID3D11VertexShader* GetPatchShadowVS();

	std::unique_ptr<ShellCB> lastShellCBData;

	winrt::com_ptr<ID3D11DepthStencilView> shadowAtlasDSV[4];
	ID3D11Texture2D* shadowAtlasDSVTexture = nullptr;
	uint32_t shadowAtlasDSVSlice[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	uint32_t sunCascadeSlice[2] = { 0, 1 };
	winrt::com_ptr<ID3D11RasterizerState> shadowCastRS;
	winrt::com_ptr<ID3D11DepthStencilState> shadowCastDSS;

	/** @brief Depth-renders the road trench patch into the live cascade slices; the ground snow casts through the raised landscape (LandscapeSnow.cpp). The statics skins deliberately do NOT cast: a skin hovers a few units above its object's own surface, so the object beneath always reads as shadowed by its own snow cap. Called from CaptureShadowAtlas before the snow reads the atlas. Implemented in SnowDeformation/Shadows.cpp. */
	void InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV);
	/** @brief The sun cascades' real atlas slices into sunCascadeSlice every frame (the snow's shadow reads), whether or not anything casts. */
	void RecordSunCascadeSlices(uint32_t a_atlasSlices);

	/** @brief Re-derives the camera-anchored placement fields of a ShellCB (the fields' frame) from THIS frame's camera. Shared by DrawShell, S1 and the shadow passes, so no pass reads the fields through a frame-old placement (a stale one slides a crisp shadow during camera motion). Implemented in SnowDeformation/Shell.cpp. */
	void RefreshShellGridPlacement(ShellCB& a_cb, const float4* a_camAdjust = nullptr);

	uint32_t dbgLodDescriptorCount = 0;
	float dbgLodEndSplits[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t dbgLodAtlasSlices = 0;

	/** @brief Queries adapter VRAM usage/budget in MB via IDXGIAdapter3 (zeros when unavailable). Usage above budget = driver demotion to system RAM = the large persistent FPS-loss mode that survives disabling features. */
	void QueryAdapterVRAM(uint64_t& a_usageMB, uint64_t& a_budgetMB, uint64_t* a_nonLocalMB = nullptr);
	/** @brief VRAM-Aware Trench Map, from Prepass: drops a trench map above 2048 to 2048 for the session when the adapter nears its VRAM budget. */
	void TickVRAMLog();
	uint64_t vramTickCounter = 0;
	/** @brief Logs a snow profiler row far over its usual cost, with the adapter's memory and the frame's heaviest rows. */
	void TickRowSpikeLog();
	uint64_t rowSpikeQuietUntil = 0;

	Buffer* pointShadowLights = nullptr;

	winrt::com_ptr<ID3D11Texture2D> pointShadowLiveTex;
	winrt::com_ptr<ID3D11ShaderResourceView> pointShadowLiveSRV;
	/** @brief The array the local-light shadows sample this frame (t39, in place), or null. */
	ID3D11ShaderResourceView* PointShadowAtlasSRV() const { return pointShadowLiveSRV.get(); }

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
		float ObjectsDepth;
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		float HasSmoothedNormals;
		float RoundedDepth;
		float VertexCountF;
		float HasObjectTop;
		float DebugSkinId;
		float LegacySkin;
		float padMoundSteepness;
		float AlphaTested;
		float FullCoat;
		float FadeExempt;
		float RoadField;
		float ProjThreshold;
		float ProjNoiseWeight;
		float ParallaxFollow;
		float ClassOverride;
		float ProjNoiseScale;
		float SteepThin;
		float ProjNoiseTiling;
		float ProjPixelEnable;
		float padSkinNormalCopy;
		float padShellMinNz;
		float PeelTol;
		float SkinSssSeen;
		float padMeldSk;

		float FarWindowCenterX;
		float FarWindowCenterY;
		float FarHalfExtent;
		float FarRoadDepth;
		float DecalMode;

		float HasSkinMasksCopy;
		float HasPreDecalMasks;
		float EdgeFlankWidth;
		float EdgeCoat;
		float FineHalfExtent;
		float LODBatch;
		float TexturedSnow;

		float4 ObjParallaxUV;
		float4 ObjParallax;
	};
	STATIC_ASSERT_ALIGNAS_16(StaticsCB);

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
		uint32_t OutBase;
		uint32_t SmoothPad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SmoothCB);

	struct RangeAllocator
	{
		std::map<uint32_t, uint32_t> free;
		void Reset(uint32_t a_capacity);
		/** @brief UINT32_MAX when no free range holds a_count at a_align. */
		uint32_t Allocate(uint32_t a_count, uint32_t a_align = 1);
		void Release(uint32_t a_offset, uint32_t a_count);
	};

	struct SmoothedNormalsEntry
	{
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool ready = false;
		uint32_t boundsSlot = UINT32_MAX;
		uint32_t clusterOffset = 0;
		uint32_t clusterCount = 0;
		uint32_t indexPoolOffset = 0;
		uint32_t indexCount = 0;
		uint32_t chunk = UINT32_MAX;
		uint32_t chunkOffset = 0;
		uint32_t chunkCount = 0;
		uint64_t descKey = 0;
		void* gameIB = nullptr;
		uint32_t vertexCount = 0;
		uint32_t triangleCount = 0;
		uint32_t lastUsedFrame = 0;
		std::list<void*>::iterator lru;
	};
	static constexpr uint32_t kSmoothedNormalsCap = 1024;
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
	static constexpr uint32_t kMeshBoundsSlots = 1024;
	Buffer* meshBounds = nullptr;
	RangeAllocator meshBoundsFree;
	ID3D11ComputeShader* smoothBoundsCS = nullptr;

	static constexpr uint32_t kClusterGroup = 256;
	static constexpr uint32_t kClusterTrisBase = 64;
	static constexpr uint32_t kClusterSlots = 65536;
	static constexpr uint32_t kClusterIndexPoolIndices = 4u * 1024u * 1024u;
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

	struct CpuCensus
	{
		int64_t hookTicks = 0;
		uint32_t hookCalls = 0;
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
		uint64_t stampHash = 0;
		uint32_t stampHashStable = 0;
		uint64_t captureHash = 0;
		uint32_t captureCount = 0;
		static constexpr uint32_t kHashRing = 300;
		uint64_t stampHashRing[kHashRing] = {};
		uint32_t stampHashHead = 0;
		bool stampHashDumpRequested = false;
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
		uint64_t stampsTotal = 0;
		uint64_t carveStampsTotal = 0;
		uint64_t rowsMarkedTotal = 0;
		uint64_t contactRectsTotal = 0;
		uint64_t settledSkipsTotal = 0;
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
		RE::BSVisit::TraverseScenegraphCollision(a_root, std::ref(a_visitor));
	}
	void RollCpuCensus();
	uint32_t clusterSkinsLast = 0;
	uint32_t clusterScratchUsedLast = 0;
	std::unordered_map<void*, SmoothedNormalsEntry> smoothedNormalsCache;
	ID3D11ComputeShader* smoothAccumulateCS = nullptr;
	ID3D11ComputeShader* smoothResolveCS = nullptr;
	ID3D11ComputeShader* smoothFlatStatsCS = nullptr;
	ConstantBuffer* smoothCB = nullptr;

	/** @brief The smoothed-normal entry for a captured geometry, built on first sight (SmoothNormalsCS passes) and cached thereafter; usable once ready. Null without one (the VS falls back to raw normals). Implemented in SnowDeformation/Statics.cpp. */
	SmoothedNormalsEntry* EnsureSmoothedNormals(RE::BSGeometry* a_geometry);
	/** @brief EnsureSmoothedNormals for a capture, looked up once per frame. */
	SmoothedNormalsEntry* SmoothedNormalsFor(CapturedSnowStatic& a_cap);
	uint32_t smoothedNormalsResets = 0;
	/** @brief ONE StaticsCB recipe for both the visible skin draw and the shadow caster - the caster must be the exact surface the shell renders, and a drifted copy of this fill would be the CB-mirror class of bug. Presence flags come from the call site (the caster looks resources up without creating them). */
	void FillSkinDrawCB(const CapturedSnowStatic& a_cap, bool a_s4Shell, float a_vertexCount, bool a_hasSmoothedNormals, bool a_hasObjectTop, StaticsCB& a_scb) const;
	/** @brief ONE StaticsCB recipe for the trench patch, shared by the visible patch draw and its shadow caster for the same drift reason as FillSkinDrawCB. Implemented in SnowDeformation/Statics.cpp. */
	void FillPatchDrawCB(StaticsCB& a_scb) const;

	static constexpr uint kHeightMapDim = 2048;
	static constexpr float kObjectConeSlope = 3.0f;
	/** @brief The live half-extent: Settings::ObjectRasterReachM in world units. Every window fill, texel derivation and shader mirror must take it from here - the texel size is derived from it, so a call site left on the constant addresses a different grid. */
	float ObjectRasterHalfExtent() const { return std::clamp(settings.ObjectRasterReachM, 29.0f, 234.0f) * kUnitsPerMeter; }
	/** @brief Horizontal camera distance (units) at which a non-exempt skin has dissolved whole (ShellCB::SkinFadeEnd). */
	float SkinFadeEndUnits() const { return settings.RangeSkinsM * kUnitsPerMeter; }
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
	static constexpr float kHeightMapEmptyTop = -100000.0f;
	static constexpr float kHeightMapEmptyBottom = 100000.0f;

	static constexpr float kPeelTol = 8.0f;
	static constexpr float kRimStep = 6.0f;
	static constexpr float kOverheadIgnore = 0.0f;
	static constexpr float kShelterBandHeight = 250.0f;
	static constexpr float kDiffuseLambda = 0.5f;

	Texture2D* heightTopRaw[2] = { nullptr, nullptr };
	Texture2D* heightBottomRaw[2] = { nullptr, nullptr };
	Texture2D* heightRaster = nullptr;
	Texture2D* heightBottomFiltered = nullptr;
	Texture2D* heightScratch = nullptr;
	Texture2D* objectSnowCone = nullptr;
	Texture2D* heightTop2Raw[2] = { nullptr, nullptr };
	Texture2D* objectSkyOpen = nullptr;
	Texture2D* heightTileEmpty = nullptr;
	Texture2D* heightTopRawFine = nullptr;
	Texture2D* heightTopRawFar = nullptr;
	float2 farWindowCenter = { 0, 0 };
	float farHalfExtentLive = 0.0f;
	/** @brief The far road level can be built (with the road maps): PatchTop and RoadOwnsColumn read it outside the coarse window, so road snow runs to the loaded grid's edge instead of handing back to the flat skin at ObjectRasterReachM. */
	bool FarLevelActive() const { return heightTopRawFar && settings.RoadMeshesDepth > 0.5f; }
	uint32_t conePassesCoarse = 0;
	bool coneFusedOff = false;
	bool coneVerify = false;
	uint32_t coneRuns = 0;
	uint32_t coneFusedRuns = 0;
	Texture2D* coneVerifyTex = nullptr;
	bool EnsureConeVerify();
	bool deadGhostTilesOff = false;
	bool combineVerify = false;
	Texture2D* combineVerifyTex = nullptr;
	bool EnsureCombineVerify();
	/** @brief Only road skins, the trench patch, the march's road taps and the debug views read the cones and the near level: true when one of them can this frame. */
	bool RoadMapsWanted() const;
	bool skyOpenCurrent = true;
	uint32_t heightProbeShownFrame = UINT32_MAX;
	bool FineLevelOn() const { return settings.ObjectSnowNearDetail; }
	static constexpr int kQualityPresetCount = 3;
	/** @brief Quality presets, Low (0) to High (2); High is the shipped defaults. Sets every preset lever; the rest of the settings keep their values. */
	void ApplyQualityPreset(int a_preset);
	/** @brief The preset every lever currently matches, or -1 (custom). */
	int MatchingQualityPreset() const;
	winrt::com_ptr<ID3D11Texture2D> probeStaging[2];
	uint probeCursor = 0;
	float probeVals[6] = {};
	bool probeValid = false;
	float3 probeWorldPos = {};
	Texture2D* heightSkinDepth = nullptr;
	uint heightCurrent = 0;
	bool heightMapValid = false;
	bool heightMapsEmpty = false;
	float2 heightWindowCenter = { 0, 0 };
	float mainViewportMinDepth = 0.0f;
	float mainViewportMaxDepth = 1.0f;
	uint32_t mainViewportFrame = UINT32_MAX;

	winrt::com_ptr<ID3D11BlendState> heightMaxBlendState;
	winrt::com_ptr<ID3D11BlendState> heightRasterBlendState;
	Texture2D* waterHeightTexture = nullptr;
	Texture2D* waterFineTexture = nullptr;
	Texture2D* waterVisibleTexture = nullptr;
	Texture2D* waterNearTexture = nullptr;
	bool waterCoverDirty = true;
	uint32_t waterCoverTerrainVersion = 0;
	uint32_t waterWindowVersion = 0;
	void CoverWaterWindow();
	float2 waterFineCenter = { 0, 0 };
	float waterFineHalf = 0.0f;
	bool waterFineValid = false;
	static constexpr int kWaterFineDim = 1024;
	int waterWindowCellX = INT_MIN;
	int waterWindowCellY = INT_MIN;
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
	uint32_t statWaterCaptured = 0;
	uint32_t statWaterDrawn = 0;
	bool waterFirstLogged = false;
	void RenderWaterCapture();
	ID3D11VertexShader* heightVS = nullptr;
	ID3D11PixelShader* heightPS = nullptr;
	ID3D11PixelShader* heightPSTopOnly = nullptr;
	ID3D11PixelShader* heightPSSkinDepth = nullptr;
	ID3D11PixelShader* heightPeelPS = nullptr;
	ID3D11ComputeShader* heightScrollCS = nullptr;
	ID3D11ComputeShader* heightScrollTopCS = nullptr;
	ID3D11ComputeShader* heightScrollRasterCS = nullptr;
	std::vector<uint8_t> heightRasterKey[3];
	bool heightRasterCacheOff = false;
	bool heightRasterVerify = false;
	bool heightRasterVerifyMiss = false;
	uint32_t heightRasterKeptBits = 0;
	uint32_t heightRasterRuns = 0;
	uint32_t heightRasterKept[3] = {};
	uint32_t heightRasterVerified[6] = {};
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
	std::vector<HeightRasterSample> heightRasterSamples;
	std::mutex heightRasterDebugLock;
	bool EnsureHeightRasterVerify();
	ID3D11ComputeShader* heightCombineCS = nullptr;
	ID3D11ComputeShader* heightWaterCoverCS = nullptr;
	ID3D11ComputeShader* objectConeSeedCS = nullptr;
	ID3D11ComputeShader* objectConeCS = nullptr;
	ID3D11ComputeShader* objectSkyOpenCS = nullptr;
	ID3D11ComputeShader* objectConeDiffuseCS = nullptr;
	ID3D11ComputeShader* objectConeFusedCS = nullptr;

	/** @brief Per-dispatch constants for the height-window processing. Layout must match HeightProcessCB in HeightMapProcessCS.hlsl. */
	struct alignas(16) HeightProcessCB
	{
		DirectX::XMINT2 ScrollDelta;
		uint ClearAll;
		uint ConeStep;

		float2 HeightWindowCenter;
		float HeightHalfExtent;
		float SlopePerUnit;

		float2 TerrainWindowOrigin;
		float TerrainTexelSize;
		uint TerrainDim;

		float GhostDecay;
		float ObjectSnowDepth;
		float RimStep;
		float OverheadIgnore;

		float ShelterBandHeight;
		float DiffuseLambda;
		float ShelterMaxHeight;
		float ShelterConeTan;

		uint keeper;
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

	/** @brief Layout must match VoxelCB in SnowVoxelCapture.hlsl. */
	struct alignas(16) VoxelVolumeCB
	{
		DirectX::XMINT4 OriginVox;
		DirectX::XMINT4 ScrollDelta;
		float VoxelSize;
		float Decay;
		int padDim;
		int SliceAxis;
		int SliceIndex;
		int SliceXray;
		int SliceSource;
		int BlurAxis;
		float2 HeightWindowCenter;
		float HeightHalfExtent;
		float ShelterDust;
		float DepthVox;
		float FieldThreshold;
		float RoundSigma;
		float SlopeMinNz;
		float CentreVox[4];
		float SkyStrength;
		float ForceDirty;
		float OverhangVox;
		float HeadroomVox;
		float EdgeParams[4];
		float LipParams[4];
		int Dims[4];
	};
	STATIC_ASSERT_ALIGNAS_16(VoxelVolumeCB);

	/** @brief V1b draw constants (b2 on the VOXEL variant of SnowStaticsShell). Layout must match VoxelDrawCB there. */
	struct alignas(16) VoxelDrawCB
	{
		DirectX::XMINT4 VoxOrigin;
		float4 VoxParams;
		float4 VoxFade;
		float4 VoxCentre;
		float4 VoxInnerCentre;
		float4 VoxDebug;
		float4 VoxDims;
	};
	STATIC_ASSERT_ALIGNAS_16(VoxelDrawCB);
	ConstantBuffer* voxelDebugCB = nullptr;
	bool showVolumeRings = false;
	bool debugForceVolumeRebuild = false;

	static constexpr uint kMaxExclusions = 256;
	static constexpr float kTorchClearRadius = 40.0f;
	static constexpr float kElevatedStampCutoff = 70.0f;

	static constexpr float kNoRoadTop = -1000000.0f;

	static constexpr uint32_t kPatchGridDim = 426;
	static constexpr float kPatchSnap = 256.0f;
	static constexpr float kFarPatchReach = 16384.0f;
	static constexpr float kHeatClearRadiusMin = 40.0f;
	static constexpr float kHeatClearRadiusMax = 90.0f;
	static constexpr float kFireClearRadius = 300.0f;
	static constexpr float kRaisedFlameClearRadius = 80.0f;
	static constexpr float kUnknownHeatClearRadius = 140.0f;

	struct HeatSourceSpec
	{
		const char* substring;
		float radius;
	};
	static constexpr HeatSourceSpec kHeatSpecs[] = {
		{ "giantcampfire", 450.0f },
		{ "bonfire", 450.0f },
		{ "campfire", 300.0f },
		{ "firepit", 300.0f },
		{ "hearth", 300.0f },
		{ "smolder", 160.0f },
		{ "brazier", 150.0f },
		{ "cookingspit", 140.0f },
		{ "cookingpot", 140.0f },
		{ "sconce", 80.0f },
		{ "torch", 80.0f },
	};

	struct TrampleSpec
	{
		const char* substring;
		float radius;
		float forwardBias;
		bool ownsFlames = false;
		float meltStrength = 0.0f;
		float aspect = 1.0f;
		bool smoothEdge = false;
	};
	static constexpr TrampleSpec kTrampleSpecs[] = {
		{ "smelter", 300.0f, 0.0f, true },
		{ "forge", 260.0f, 0.0f, true },
		{ "sawmill", 220.0f, 0.0f },
		{ "millsaw", 220.0f, 0.0f },
		{ "stables", 200.0f, 0.0f },
		{ "chopping", 150.0f, 50.0f },
		{ "enchanting", 110.0f, 60.0f },
		{ "alchemy", 110.0f, 60.0f },
		{ "workbench", 140.0f, 0.0f },
		{ "sharpeningwheel", 130.0f, 60.0f },
		{ "tanningrack", 120.0f, 60.0f },
		{ "anvil", 120.0f, 50.0f },
		{ "marketstall", 110.0f, 0.0f },
		{ "well01", 110.0f, 0.0f },
		{ "shrine", 90.0f, 0.0f },
		{ "bedroll", 160.0f, 0.0f, false, 1.0f, 1.75f, true },
		{ "haymound", 120.0f, 0.0f, false, 1.0f, 1.0f, true },
		{ "haybale", 100.0f, 0.0f, false, 1.0f, 1.0f, true },
	};

	uint32_t statTrampleCount = 0;

	static constexpr const char* kSealedContainerSubstrings[] = {
		"sarcophagus",
	};
	uint32_t statSealedCount = 0;

	static constexpr float kGroundFireBand = 40.0f;

	static constexpr uint kExclusionFieldDim = 1024;
	static constexpr float kExclusionFieldHalfExtent = 16384.0f;

	Texture2D* exclusionFieldTexture = nullptr;
	float2 exclusionFieldCenter = { 0.0f, 0.0f };
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
		float4 PosRadius[kMaxExclusions];
		float4 DirExtType[kMaxExclusions];
		uint ExclusionCount;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(ExclusionsCB);
	static_assert(offsetof(ExclusionsCB, ExclusionCount) == sizeof(float4) * kMaxExclusions * 2, "exclusionListBuffer mirrors the two leading arrays");
	ConstantBuffer* doorsCB = nullptr;
	StructuredBuffer* exclusionListBuffer = nullptr;
	uint64_t exclusionListHash = 0;
	bool doorsCBUploaded = false;
	int exclusionBakeCellX = INT_MIN;
	int exclusionBakeCellY = INT_MIN;
	uint32_t exclusionBakeTerrainVersion = 0;
	std::unique_ptr<ExclusionsCB> exclusionBakeList;
	/** @brief Bakes the wide exclusion field from the gathered exclusion list, or skips when nothing it reads has changed. Implemented in SnowDeformation/Statics.cpp. */
	void RenderExclusionField(const ExclusionsCB& a_list);
	uint32_t doorRefreshCounter = 0;
	std::vector<std::pair<float4, float4>> staticExclusions;
	std::vector<float4> heatSources;
	std::vector<float4> exclusionPendingHeat;
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
	RE::BGSListForm* survivalHeatSources = nullptr;
	bool survivalHeatSourcesResolved = false;
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
	float4 heightGhostBox = {};
	bool heightGhostsLive = true;
	uint64_t heightIdleFrames = 0;
	bool interiorHeightGateOff = false;
	bool exteriorHeightGateOff = false;
	bool cascadeSnowSoonLast = true;
	bool cascadeHeightGateOff = false;
	bool lodRasterGateOff = false;
	bool lodCapturesIgnoredLast = false;
	bool cascadeRaiseGateOff = false;
	Matrix cascadeGateVP{};
	float4 cascadeGateCam{};
	uint32_t cascadeGateFrame = UINT32_MAX;
	bool cascadeGateMeets = true;
	uint32_t cascadeRaised = 0;
	uint32_t cascadeSkipped = 0;
	uint32_t cascadeRaisedLast = 0;
	uint32_t cascadeSkippedLast = 0;
	uint32_t cascadeCountFrame = UINT32_MAX;
	static constexpr int kGatherJumpTexels = 32;

	ID3D11VertexShader* staticsVS = nullptr;
	ID3D11PixelShader* staticsPS = nullptr;
	ID3D11PixelShader* staticsPSNoDepth = nullptr;
	ID3D11PixelShader* staticsPSPrepassNoDepth = nullptr;
	ID3D11PixelShader* staticsPSDrapeNoDepth = nullptr;
	ID3D11PixelShader* staticsPSDrapePrepass = nullptr;
	ID3D11PixelShader* staticsPSDrapePrintsNoDepth = nullptr;
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
	static constexpr uint32_t kSkinCullReasons = 9;
	static constexpr int kSkinCullRing = 3;
	static constexpr float kSkinCullMargin = 24.0f;
	static constexpr float kSkinCullDepthEps = 1.5e-5f;
	ConstantBuffer* skinCullCB = nullptr;
	Buffer* skinCullBounds = nullptr;
	Buffer* skinCullArgs = nullptr;
	Buffer* skinFootprint = nullptr;
	uint32_t skinCullCapacity = 0;
	winrt::com_ptr<ID3D11Buffer> skinCullArgsStaging[kSkinCullRing];
	bool skinCullStagingIssued[kSkinCullRing] = {};
	uint32_t skinCullStagingCount[kSkinCullRing] = {};
	bool skinCullLogArmed = false;
	bool debugLogAllJourneys = false;
	std::vector<std::string> skinCullLogNames[kSkinCullRing];
	int skinCullRing = 0;
	Texture2D* skinCullHiZ = nullptr;
	std::vector<Texture2D*> skinCullHiZScratch;
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> skinCullHiZSRVs;
	std::vector<winrt::com_ptr<ID3D11UnorderedAccessView>> skinCullHiZUAVs;
	uint32_t skinCullLevels = 0;
	uint32_t skinCullDrawnLast = 0;
	uint32_t skinCullCulledLast = 0;
	uint32_t skinCullTrisDrawnLast = 0;
	uint32_t skinCullTrisTotalLast = 0;
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
	ID3D11VertexShader* staticsTessVS = nullptr;
	ID3D11HullShader* staticsHS = nullptr;
	ID3D11DomainShader* staticsDS = nullptr;
	ID3D11VertexShader* patchVS = nullptr;
	ID3D11VertexShader* patchShadowVS = nullptr;
	winrt::com_ptr<ID3D11Buffer> patchCasterIB;
	uint32_t patchCasterIndexCount = 0;
	bool EnsurePatchCasterIB();
	/** @brief The tessellated patch hull's frustum footprint lines (SnowStaticsShell.hlsl PatchCullCB, b11). */
	struct alignas(16) PatchCullCB
	{
		float4 Lines[6];
		uint32_t LineCount;
		uint32_t RowStart;
		uint32_t ColStart;
		uint32_t pad;
	};
	ConstantBuffer* patchCullCB = nullptr;
	/** @brief The view frustum's five planes (rows of CameraViewProj, camera-relative) with z eliminated: lines in world XY, inside where dot(xy, p) + z >= 0. */
	uint32_t FillPatchCullLines(const ShellCB& a_cb, PatchCullCB& a_out) const;
	/** @brief The trench patch lattice's centre, snapped to its coarsest band step (StaticsCB::WorldRow0.xy). */
	float2 PatchLatticeCentre() const;
	struct PatchBand
	{
		uint32_t x0, x1, y0, y1;
	};
	static constexpr uint32_t kPatchBands = 4;
	/** @brief The lattice's 8x8 blocks the hull's footprint lines cannot cull, as up to kPatchBands row bands in row order, each over its blocks' column span; returns the band count. */
	uint32_t PatchLatticeBands(const PatchCullCB& a_lines, float2 a_centre, PatchBand (&a_bands)[kPatchBands]) const;
	bool patchBoxOff = false;
	bool patchVerify = false;
	Texture2D* patchVerifyRT[4] = {};
	winrt::com_ptr<ID3D11Texture2D> patchVerifyDepth;
	winrt::com_ptr<ID3D11DepthStencilView> patchVerifyDSV;
	winrt::com_ptr<ID3D11ShaderResourceView> patchVerifyDepthSRV;
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
	uint32_t patchBoxPatches = 0;
	ID3D11VertexShader* patchTessVS = nullptr;
	ID3D11HullShader* patchHS = nullptr;
	ID3D11DomainShader* patchDS = nullptr;
	ID3D11PixelShader* patchPS = nullptr;
	ID3D11PixelShader* patchDeferredPS[2] = {};
	std::future<ID3D11PixelShader*> patchDeferredJob[2];
	bool patchDeferredTried[2] = {};
	winrt::com_ptr<ID3D11DepthStencilState> patchDeferredDepthState;
	Texture2D* patchDeferredMark = nullptr;
	/** @brief The mark target and the shading pass's depth-off state; false leaves the patch forward. */
	bool EnsurePatchDeferredResources();
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
	winrt::com_ptr<ID3DBlob> staticsVSBlob;
	bool staticsShadersFailed = false;
	ConstantBuffer* staticsCB = nullptr;
	winrt::com_ptr<ID3D11DeviceContext1> staticsContext1;
	winrt::com_ptr<ID3D11Buffer> staticsRecordCB[2];
	bool staticsRecordChecked = false;
	static constexpr uint32_t kStaticsRecordStride = 256;
	static constexpr uint32_t kStaticsRecordStart = 8192;
	static constexpr uint32_t kStaticsRecordMax = 65536;
	uint32_t staticsRecordCapacity = 0;
	uint32_t staticsRecordRefused = 0;
	uint64_t staticsRecordFallbacks = 0;
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

	Texture2D* contactHeight = nullptr;
	winrt::com_ptr<ID3D11BlendState> contactMinBlendBodies;
	winrt::com_ptr<ID3D11BlendState> contactMinBlendProps;
	winrt::com_ptr<ID3D11RasterizerState> contactRasterState;
	ID3D11VertexShader* contactVS = nullptr;
	ID3D11PixelShader* contactPS = nullptr;
	bool contactShadersFailed = false;
	struct ContactProp
	{
		RE::ObjectRefHandle ref;
		float minX, minY, maxX, maxY;
		bool corpse = false;
		float groundZ = 0.0f;
		float layer = 0.0f;
	};
	std::vector<ContactProp> contactProps;
	std::unordered_set<const void*> contactSkinLogged;
	float2 contactCenter = { 0, 0 };
	uint contactDrawsLast = 0;
	/** @brief Bone palette for one skin partition, plus the capture window. Absolute world rows, so the VS needs no object transform and no pivot. */
	struct alignas(16) ContactSkinCB
	{
		float4 BoneRows[240];
		float2 SkinWindowCenter;
		float SkinHalfExtent;
		float SkinBoneCount;
	};
	STATIC_ASSERT_ALIGNAS_16(ContactSkinCB);
	ConstantBuffer* contactSkinCB = nullptr;
	ID3D11VertexShader* contactSkinVS = nullptr;
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> contactSkinILCache;
	winrt::com_ptr<ID3DBlob> contactSkinVSBlob;
	/** @brief a_dynamicPositionBytes 0 = positions in the game's vertex buffer; 16 or 8 = a second stream of float4/half4 positions copied from a BSDynamicTriShape's dynamicData (RaceMenu BodyMorph, OBody, facegen). */
	ID3D11InputLayout* ContactSkinInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc, uint32_t a_dynamicPositionBytes);
	winrt::com_ptr<ID3D11Buffer> contactDynamicVB;
	uint32_t contactDynamicVBBytes = 0;
	std::unordered_set<const void*> contactDynamicLogged;
	std::vector<ContactProp> contactActors;
	uint contactCorpseCount = 0;
	std::vector<uint32_t> contactPriority;
	std::vector<RE::NiTransform> contactPaletteScratch;
	std::vector<uint8_t> contactPaletteBuilt;
	bool debugContactView = false;
	float debugContactViewHalf = 192.0f;
	int debugContactSolo = -1;
	std::string contactSoloName;
	static constexpr uint kContactViewPx = 512;
	winrt::com_ptr<ID3D11Texture2D> contactViewTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> contactViewSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> contactViewUAV;
	void EnsureContactViewTexture();
	uint contactSkinDrawsLast = 0;
	uint contactSkinMissingLast = 0;
	uint contactCarriedLast = 0;
	uint contactOverlaysLast = 0;
	uint contactShellsLast = 0;
	uint contactHiddenPartsLast = 0;
	struct ContactSkinAudit
	{
		const void* partition = nullptr;
		uint8_t indexing = 0;
	};
	std::unordered_map<const void*, ContactSkinAudit> contactSkinIndexing;
	uint contactGlobalPartsLast = 0;
	uint contactSweepLast = 0;
	struct ContactSweep
	{
		RE::NiTransform world;
		uint32_t frame = 0;
		const void* buffer = nullptr;
	};
	std::unordered_map<const void*, ContactSweep> contactSweepStates;
	uint32_t contactSweepFrame = 0;
	static constexpr float kContactSweepStep = 6.0f;
	static constexpr uint32_t kContactMaxSweep = 8;
	std::unordered_set<const void*> contactSkinMissingLogged;
	std::unordered_set<const void*> contactSkinHiddenLogged;
	std::unordered_set<uint32_t> incorporealLogged;
	std::unordered_set<uint32_t> seeThroughKeptLogged;
	static constexpr float kContactSkipMargin = 16.0f;
	static constexpr uint16_t kContactStarveFrames = 60;
	uint contactStarveLines = 0;
	float2 contactViewCenter{};
	float contactViewTexelSize = 0.0f;
	float contactViewHalfUsed = 0.0f;
	float2 contactViewBoundCenter{};
	float contactViewBoundRadius = 0.0f;
	bool EnsureContactResources();
	/** @brief The contact capture; a_coatOnly (S5 round 8, while the deformation passes sleep) walks only the actors the body coat's snowfall is due. */
	void DrawContactCapture(ID3D11DeviceContext* a_context, bool a_coatOnly = false);

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
	/** @brief The shell surface's grid and field windows for this frame, around a_cb's camera: DrawShellEarlyPrepass's recipe. */
	void PlaceShellFields(ShellCB& a_cb);
	static constexpr uint32_t kShellSurfaceSRVs = 59;
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
	static constexpr float kLandBlendDepthStart = 0.50f;
	bool landRecolor = true;
	static constexpr float kLandRecolorLumLo = 0.48f;
	/** @brief The recolor runs this frame: the ground snow draws here and the snow set is loaded. */
	bool LandRecolorActive() const { return landRecolor && LandBlendActive() && !GroundShellsSuspended() && shellSnowDiffuseSRV != nullptr; }
	static constexpr uint32_t kTrenchMarkSettleFrames = 60;
	static constexpr uint32_t kTrenchMarkRefreshFrames = 600;
	static constexpr float kTrenchMarkTolTexels = 2.0f;
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
	uint32_t shellKillMask = 0;
	struct EffectCostProbe
	{
		static constexpr uint32_t kEffects = 9;
		static constexpr const char* kNames[kEffects] = { "Parallax", "Normal map", "Roughness / AO maps", "Crisp shadows", "Screen-space shadows",
			"Glints", "Point lights", "Skylighting", "Snow albedo texture" };
		static constexpr uint32_t kKillBits[kEffects] = { 0, 1, 2, 3, 4, 6, 7, 8, 9 };
		bool running = false;
		bool done = false;
		uint32_t step = 0;
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
	/** @brief DevBench: the reader gates' A/B switches (interior height maps, masks copy, march idle, blood latch, land-mask feature test, record growth) and their counters. Implemented in DevBenchTools.cpp. */
	json DevBenchReaderGatesArgs(const json& a_args);
	/** @brief Stored trench depth (0-1) at a world position in the active worldspace, under the store lock. Any thread. Implemented in TrenchStore.cpp. */
	float ProbeTrenchStore(float a_worldX, float a_worldY);
	static constexpr uint32_t kLandscapeRunCap = 8;
	bool landscapeRunOpen = false;
	uint32_t landscapeRunFrame = 0;
	uint32_t landscapeRunsThisFrame = 0;
	uint32_t landscapeBatchesThisFrame = 0;
	uint64_t landscapeCensusRuns = 0;
	uint64_t landscapeCensusBatches = 0;
	uint64_t landscapeCensusCapped = 0;
	uint64_t landscapeCensusDropped = 0;
	uint32_t landscapeCensusFrames = 0;
	uint32_t landscapeCensusPeakRuns = 0;
	bool landscapeCensusLogged = false;

	std::atomic<uint64_t> landMaskHits{ 0 };
	std::atomic<uint64_t> landMaskMisses{ 0 };
	std::atomic<uint64_t> landMaskForeignHits{ 0 };
	bool landMaskFeatureGateOff = false;

	/** @brief Thread-safe size read for the settings UI. */
	size_t snowMasksSizeForUI()
	{
		const std::shared_lock lock(snowMaskMutex);
		return snowMasks.size();
	}

	bool showDebugTexture = false;
	bool debugTerrainOverlay = false;

	bool debugMeltEmitterActive = false;
	RE::NiPoint3 debugMeltEmitterPos{};
	float debugMeltEmitterRadius = 100.0f;
	float debugMeltEmitterRate = 0.50f;
	bool debugProjSnowView = false;
	bool debugProjWeightView = false;
	bool debugProjAlbedoView = false;

protected:
	void GatherStamps(PerFrame& perFrameData);

	std::unordered_map<uintptr_t, uint8_t> snowMasks;
	std::shared_mutex snowMaskMutex;

	std::unordered_map<uint64_t, ShellCellData> shellCells;
	std::unordered_map<uint64_t, uint32_t> shellFillerCells;
	std::unordered_set<uint64_t> shellSnowyCells;
	mutable std::shared_mutex shellSnowyCellMutex;
	enum SnowGateVerdict : uint32_t
	{
		kSnowGateSnowy = 0,
		kSnowGateUnknown = 1,
		kSnowGateBare = 2,
	};
	bool WindowHasSnow(float a_halfExtentUnits, bool a_unknownIsSnowy = false, uint32_t* a_verdictOut = nullptr) const;
	bool LoadedCellSquare(int& a_minX, int& a_minY, int& a_maxX, int& a_maxY) const;
	uint32_t CellRangeSnowVerdict(int a_minX, int a_minY, int a_maxX, int a_maxY) const;
	uint32_t ViewSnowVerdict(int a_minX, int a_minY, int a_maxX, int a_maxY, const ShellCB& a_cb);
	static constexpr float kViewGateMarginZ = 256.0f;
	std::unordered_map<uint64_t, std::pair<float, float>> viewGateCellZ;
	uint32_t viewGateCellZVersion = UINT32_MAX;
	bool shellViewGateOff = false;
	void CullUnseenCaptures(const ShellCB& a_cb);
	float squareFloorZ = 0.0f;
	bool squareFloorValid = false;
	uint32_t squareFloorVersion = UINT32_MAX;
	int squareFloorKey[4] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };
	uint32_t captureViewCulled = 0;
	uint32_t captureCullBail = 1;
	bool captureViewCullOff = false;
	bool GroundNearHasSnow(float a_x, float a_y);
	std::unordered_map<uint64_t, uint8_t> groundSnowCellCache;
	uint32_t groundSnowCellCacheVersion = 0;
	uint32_t groundSnowFrame = UINT32_MAX;
	bool groundSnowFrameInterior = false;
	bool groundSnowFrameHasSquare = false;
	int groundSnowFrameSquare[4] = {};
	int groundSnowCacheSquare[4] = { INT_MIN, INT_MIN, INT_MIN, INT_MIN };
	uint8_t groundSnowSquareVerdict = kSnowGateUnknown;
	bool groundSnowSquareValid = false;
	bool DeformationWindowHasSnow() const { return WindowHasSnow(deformWorldSize * 0.5f, true, &deformSnowVerdict); }
	bool DeformationWindowHasReaders() const;
	bool ShellFootprintHasSnow() const { return WindowHasSnow(ShellWarpedHalfSpan()); }
	bool CascadeSnowSoon();
	using FootprintPlanes = std::array<std::array<double, 4>, 4>;
	static constexpr size_t kMaxSunFootprints = 4;
	bool SunCascadeFootprints(FootprintPlanes* a_out, size_t& a_count) const;
	bool SnowCellsMeetFootprints(const FootprintPlanes* a_planes, size_t a_count, float a_rise, bool a_soon);
	bool LsCascadeMeetsSnow();
	float LsRiseReach() const;
	mutable uint32_t deformSnowVerdict = kSnowGateUnknown;
	mutable uint32_t shellSnowVerdict = kSnowGateUnknown;
	uint32_t shellGateBareFrames = 0;
	static constexpr uint32_t kShellGateBareFrames = 30;
	bool deformSuspended = false;
	uint32_t deformGateBareFrames = 0;
	mutable std::shared_mutex shellCellMutex;
	std::atomic<bool> shellDataDirty{ true };
	uint32_t shellTerrainVersion = 0;
	std::atomic<uint32_t> snowDepthVersion{ 1 };
	std::vector<LandTextureEntry> landTextures;
	std::string textureFilter;
	std::unordered_map<uint32_t, uint16_t> landTextureByTextureSet;
	std::shared_mutex landTextureMutex;

	std::atomic<uint32_t> activeWorldspace{ 0 };
	uint32_t shellWindowWorldspace = 0;
	int shellWindowCellX = INT_MIN;
	int shellWindowCellY = INT_MIN;
	std::vector<float> shellUploadScratch;

	float2 windowOrigin = { 0, 0 };
	DirectX::XMINT2 pendingScrollDelta = { 0, 0 };
	DirectX::XMINT2 mapOrigin = { 0, 0 };
	bool clearRequested = true;

	float deformWorldSize = 14000.0f;
	bool trenchRangeDirty = false;
	uint deformMapDim = kTextureDim;
	bool deformMapDimDirty = false;
	uint deformMapVramCap = 0;
	bool rangeInitApplied = false;

	bool debugForceDeformationUpdate = false;
	bool debugForceAllTilesDirty = false;
	bool deformIdleSkipped = false;
	winrt::com_ptr<ID3D11Buffer> deformActivityBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> deformActivityUAV;
	static constexpr uint kDeformActivitySlots = 3;
	winrt::com_ptr<ID3D11Buffer> deformActivityStaging[kDeformActivitySlots];
	uint64_t deformActivitySlotSeq[kDeformActivitySlots] = {};
	bool deformActivityPending[kDeformActivitySlots] = {};
	uint64_t deformDispatchSeq = 0;
	uint64_t deformLastNonIdleSeq = 0;
	uint64_t deformFlagSeq = 0;
	bool deformFlagActive = true;
	bool deformActivitySlotEvolveRan[kDeformActivitySlots] = {};
	bool evolveFlagActive = true;
	uint64_t evolveVerdictSeq = 0;
	uint64_t evolveLastArmSeq = 0;
	bool evolveIdleLastFrame = false;
	float bermReachBaked = 1.0f;
	std::vector<float4> lastStampSet;
	std::vector<float4> lastStampEnds;
	std::vector<uint8_t> stampMatchUsed;
	std::vector<uint32_t> stampMatchIndex;
	uint32_t deformIdleBlockers = 0;
	float deformSkipRate = -1.0f;
	uint32_t deformSkipTallyFrames = 0;
	uint32_t deformSkipTallySkipped = 0;
	uint32_t stampSetCountChanges = 0, stampSetDriftChanges = 0;
	uint32_t stampSetTallyCount = 0, stampSetTallyDrift = 0;
	uint32_t deformChangedTexels = 0;
	uint32_t deformChangedDepth = 0;
	uint32_t deformChangedMelt = 0;
	uint32_t deformChangedCrustDep = 0;
	uint32_t deformChangedDeltaSum = 0;
	uint32_t deformChangedMinX = 0, deformChangedMinY = 0;
	uint32_t deformChangedMaxX = 0, deformChangedMaxY = 0;
	bool debugActivityView = false;
	winrt::com_ptr<ID3D11Texture2D> activityViewTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> activityViewSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> activityViewUAV;
	uint32_t activityViewDim = 0;
	void EnsureActivityViewTexture();
	void PollDeformActivity(ID3D11DeviceContext* a_context);

	static constexpr float kTrenchTileWorld = 512.0f;
	static constexpr int kTrenchTileDim = 128;
	static constexpr float kTrenchStoreEpsilon = 0.02f;
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
		std::vector<uint8_t> fresh;
		float clock = 0.0f;
		double freshClock = 0.0;
		float lastTouch = 0.0f;
		uint32_t encodedBytes = 0;
	};

	static constexpr uint32_t kTrenchTileHeaderBytes = 32;
	static constexpr double kTrackFreshPerUnit = 255.0 / kTrackAgeHorizon;
	static constexpr uint32_t kTrenchRLEPairBytes = 2;

	std::unordered_map<TrenchTileKey, TrenchTile, TrenchTileKeyHash> trenchTiles;

	float trenchDecayClock = 0.0f;
	double trenchAgeingTotal = 0.0;

	std::vector<TrenchTileKey> trenchSweepQueue;
	size_t trenchStatNonZero = 0;
	size_t trenchStatThin = 0;
	size_t trenchStatSweptTiles = 0;
	size_t trenchAccumNonZero = 0;
	size_t trenchAccumThin = 0;
	size_t trenchAccumTiles = 0;
	size_t trenchAccumErased = 0;
	size_t trenchAccumEvicted = 0;
	size_t trenchAccumAdded = 0;
	size_t trenchStatTiles = 0;
	size_t trenchEncodedTotal = 0;
	std::vector<std::pair<float, TrenchTileKey>> trenchEvictScratch;

	float trenchKeepRadiusSq = std::numeric_limits<float>::max();

	struct TrenchBandCopy
	{
		float2 origin;
		float texel;
		uint32_t worldspace;
		int32_t x0, y0, w, h;
		float ageingClock = 0.0f;
		double ageingTotal = 0.0;
	};

	winrt::com_ptr<ID3D11Texture2D> trenchInjectTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchInjectSRV;
	std::vector<uint8_t> trenchInjectScratch;

	winrt::com_ptr<ID3D11Texture2D> trenchDebugTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trenchDebugSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> trenchDebugUAV;
	ID3D11ComputeShader* trenchDebugCS = nullptr;
	ID3D11ComputeShader* GetTrenchDebugCS();
	void UpdateTrenchDebugTexture();

	winrt::com_ptr<ID3D11Texture2D> trenchBandStaging[2][2];
	winrt::com_ptr<ID3D11Texture2D> trenchClockBandStaging[2][2];
	bool trenchBandValid[2][2] = {};
	TrenchBandCopy trenchBandMeta[2][2] = {};
	int trenchBandRing = 0;

	static constexpr int kTrenchRollRows = 128;
	winrt::com_ptr<ID3D11Texture2D> trenchRollStaging[2];
	winrt::com_ptr<ID3D11Texture2D> trenchClockRollStaging[2];
	bool trenchRollValid[2] = {};
	TrenchBandCopy trenchRollMeta[2] = {};
	int trenchRollRing = 0;
	int trenchRollRow = 0;

	std::vector<uint32_t> trenchDirtyRows;

	void MarkTrenchDirtyRows(const PerFrame& a_data);
	void RollTrenchWindow();

	std::atomic<bool> trenchReinjectRequested{ false };

	float2 trenchMapOrigin = { 0, 0 };
	float trenchMapTexel = 0.0f;
	uint32_t trenchMapWorldspace = 0;
	DirectX::XMINT2 trenchMapPhysOrigin = { 0, 0 };
	bool trenchMapPrimed = false;

	TrenchTileKey trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
	const TrenchTile* trenchSampleTile = nullptr;

	bool CreateTrenchStoreResources();
	void DrainTrenchBands();
	void FlushDepartingTrenches(DirectX::XMINT2 a_scroll, bool a_clearing);
	uint BuildTrenchInject(DirectX::XMINT2 a_scroll, bool a_clearing);
	void StoreTrenchBand(const TrenchBandCopy& a_meta, const D3D11_MAPPED_SUBRESOURCE& a_mapped, const D3D11_MAPPED_SUBRESOURCE* a_clocks);
	float SampleTrenchStore(uint32_t a_worldspace, float a_worldX, float a_worldY, float* o_fresh = nullptr);
	static constexpr uint32_t kTrenchRecord = 'SNTR';
	static constexpr uint32_t kTrenchRecordVersion = 2;

	static constexpr uint32_t kAccumRecord = 'SNAC';
	static constexpr uint32_t kAccumRecordVersion = 2;

	std::atomic<float> accumWeatherIntensity{ 0.0f };

	std::atomic<float> snowAccumulation{ 0.0f };
	void TickAccumulation();
	float GetAccumulationDepthScale() const;
	void RegisterAccumulationCoSave();

	static constexpr uint32_t kFreshRecord = 'SNFR';
	static constexpr uint32_t kFreshRecordVersion = 1;
	static constexpr float kFreshenHours = 2.0f;
	std::atomic<float> snowFreshness{ 0.0f };
	float debugFreshnessOverride = -1.0f;
	void TickSnowCondition();
	float GetSnowSoftness() const;
	void RegisterSnowConditionCoSave();

	static constexpr float kAgeingClockStart = 1024.0f;
	static constexpr float kClimateLapse = 0.0005f;
	struct SnowClimateState
	{
		float temperature = 0.0f;
		float wetness = 0.0f;
		float windPacking = 0.0f;
		float landSnowShare = -1.0f;
		float rainHeld = 0.0f;
		std::array<float, kClimateLUTRows> rowWater{};
		std::array<float, kClimateLUTRows> rowIce{};
		std::array<std::array<float, kClimateLUTRows>, kIceHistoryBins> iceHistory{};
		int32_t iceHistoryUnit = -1;
		float ageingClock = kAgeingClockStart;
		float ageingRate = 0.0f;
		double ageingTotal = 0.0;
		float ageingRebase = 0.0f;
		float targetTemperature = 0.0f;
		float termRegion = 0.0f;
		float termWeather = 0.0f;
		float termSun = 0.0f;
		bool primed = false;
		uint32_t shareFrame = 0;
	};
	SnowClimateState snowClimate;
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
	void TickSnowIce(float a_elapsed, float a_snowing);
	float SnowIceShown(uint32_t a_row) const;
	float SnowIceSinceCut(uint32_t a_row, uint32_t a_units) const;
	float SnowWetIceStrength() const;
	static constexpr uint32_t kIceRecord = 'SNIC';
	static constexpr uint32_t kIceRecordVersion = 2;
	void RegisterSnowIceCoSave();
	SnowCondition SnowConditionAt(float a_temperature) const;
	struct SnowClimateLook
	{
		float strength = 0.0f;
		float berm = 1.0f;
		float lip = 1.0f;
		float teeth = 1.0f;
		float slough = 0.0f;
		std::array<float4, kClimateLUTRows> lut{};
	};
	SnowClimateLook ComputeSnowClimateLook() const;
	void TickSnowClimate();
	void TickSnowAgeing();
	float LandSnowShareAround(float a_x, float a_y);
	bool debugClimateTemperatureOn = false;
	float debugClimateTemperature = -5.0f;
	float debugClimateShareOverride = -1.0f;
	float debugClimateWetnessOverride = -1.0f;
	float debugClimateStickinessOverride = -1.0f;
	float debugClimateFirmnessOverride = -1.0f;
	float debugAgeingRateOverride = -1.0f;
	float debugClimateIceOverride = -1.0f;
	int debugConditionView = 0;
	void SaveAccumulation(const SKSE::SerializationInterface* a_intfc);
	void LoadAccumulation(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);

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
		RE::NiPoint3 velocity;
		float strength = 0.0f;
		float amount = 0.0f;
		float surfaceZ = 0.0f;
		float snowDepth = 0.0f;
		float stickiness = 0.0f;
		uint32_t formID = 0;
		uint32_t seed = 0;
		SnowEventKind kind = SnowEventKind::KickUp;
	};
	static constexpr uint32_t kMaxSnowEventsPerFrame = 256;
	void RequestSnowEvents();
	bool SnowEventsWanted() const;
	const std::vector<SnowEvent>& PublishedSnowEvents() const { return snowEventsPublished; }
	void PushSnowEvent(const SnowEvent& a_event);
	void CollectActorSnowEvents(RE::Actor* a_actor, bool a_dead, const RE::NiPoint3& a_position, float a_groundZ,
		const StampBones* a_bones, const RE::NiAVObject* a_root);
	void TickSnowEvents();
	std::vector<std::string> SnowEventCensusLines() const;
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

	SnowCondition SnowConditionAtHeight(float a_z) const;
	uint32_t stampGatherFrame = UINT32_MAX;

public:

	/** @brief One mapped skin's block at Lighting PS b8, bound per draw by offset. Mirror: SnowLighting.hlsli BodyCoatRecord. */
	struct alignas(16) BodyCoatRecordCB
	{
		float4 ModelToCanonical[3];
		float4 ModelToAtlas[3];
		float4 AtlasLo;
		float4 AtlasHi;
		uint View;
		uint Slot;
		uint Component;
		uint Flags;
		float4 Glint;
		float4 Look;
		float4 Amounts;
		float4 Fall;
	};
	STATIC_ASSERT_ALIGNAS_16(BodyCoatRecordCB);
	/** @brief The engine's own decal geometries, by the names it gives them: blood and impact marks, skinned or not. Implemented in SnowDeformation/Statics.cpp. */
	static bool IsEngineDecalName(const char* a_name);

protected:

	void LoadTraceBegin(const char* a_reason);
	void LoadTraceMark(std::string_view a_what);
	void LoadTraceRecord(const char* a_name, double a_ms);
	void LoadTraceFramePulse();

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
	std::chrono::steady_clock::time_point loadTraceLastNotable;
	std::chrono::steady_clock::time_point loadTraceLastFrame;
	bool loadTraceFrameSeen = false;
	std::map<std::string, LoadTracePhase, std::less<>> loadTracePhases;
	std::vector<std::string> loadTraceTimeline;
	uint32_t loadTraceFrames = 0;
	uint32_t loadTraceStallFrames = 0;
	double loadTraceWorstFrameMs = 0.0;
	void LoadTraceReportLocked(const char* a_reason);

public:

	virtual void DataLoaded() override;
	~SnowDeformation();
	/** @brief Worker body: every non-debug shader getter once, in visibility order. */
	void RunShaderPrime();
	std::atomic<int> snowPrimeState = 0;
	std::atomic<int> snowPrimePhase = 0;
	/** @brief True while the prime still owns this group's shader members - the render path must skip. */
	bool SnowShadersPending(int a_group) const
	{
		return snowPrimeState.load(std::memory_order_acquire) == 1 &&
		       snowPrimePhase.load(std::memory_order_acquire) < a_group;
	}
	std::thread snowPrimeThread;

	/** @brief Util::CompileShader for the snow shaders; its disk cache keys on the preprocessed source, so every define reaches the key. */
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

	std::mutex trenchStoreMutex;

	void RegisterTrenchCoSave();
	void SaveTrenchStore(const SKSE::SerializationInterface* a_intfc);
	void LoadTrenchStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);

	void EncodeTrenchTile(const std::vector<uint8_t>& a_bytes, std::vector<uint8_t>& o_bytes) const;
	bool DecodeTrenchTile(const uint8_t* a_bytes, uint32_t a_length, std::vector<uint8_t>& o_depth) const;

	void ClearTrenchStore(const char* a_reason);
	void ClearTrenchStoreLocked(const char* a_reason);
	void TickTrenchClock();
	bool DecayTrenchTile(TrenchTile& a_tile);
	void SweepTrenchStore();
	void EnforceTrenchBudget();

	struct TrenchStoreStats
	{
		size_t tiles;
		size_t bytes;
		size_t encoded;
		float occupancy;
		size_t thin;
	};
	TrenchStoreStats GetTrenchStoreStats() const
	{
		const size_t bytes = trenchStatTiles * (size_t)kTrenchTileDim * kTrenchTileDim;
		const size_t swept = trenchStatSweptTiles * (size_t)kTrenchTileDim * kTrenchTileDim;
		return { trenchStatTiles, bytes, trenchEncodedTotal, swept ? (float)trenchStatNonZero / (float)swept : 0.0f, trenchStatThin };
	}

	static constexpr float kPrintTexel = 3.0f;
	static constexpr int kPrintTileDim = 32;
	static constexpr int kPrintTileTexels = kPrintTileDim * kPrintTileDim;
	static constexpr size_t kPrintStoreCap = 2048;
	static constexpr uint32_t kPrintBlockCap = 16384;
	static constexpr uint32_t kPrintStagingSlots = 16;
	static constexpr uint32_t kPrintMirrorPerFrame = 4;
	static constexpr uint32_t kPrintRecord = 'SNOP';
	static constexpr uint32_t kPrintRecordVersion = 1;
	static constexpr UINT kPrintReaderSlot = 114;
	static constexpr UINT kHorizonSnowAlbedoSlot = 33;
	static constexpr UINT kHorizonSnowNormalSlot = 23;
	static constexpr UINT kWaterWindowSlot = 63;
	static constexpr UINT kHorizonSnowRmaosSlot = 68;
	static constexpr UINT kLandBermSlot = kBodyReachSlot;
	static constexpr int kPrintRimApron = 5;

	struct alignas(16) PrintFrameCB
	{
		DirectX::XMINT2 WindowMin;
		uint RefillQuanta;
		uint RingRectCount;
		DirectX::XMINT4 RingRects[2];
		uint RingTotal;
		float RimWidth;
		uint SpellCount;
		uint Pad1;
	};
	STATIC_ASSERT_ALIGNAS_16(PrintFrameCB);
	static_assert(sizeof(PrintFrameCB) == 64, "PrintFrameCB must match the fxc-reflected PrintFrame");

	struct PrintSpellMark
	{
		RE::NiPoint3 a;
		RE::NiPoint3 b;
		float radius = 0.0f;
		float depth = 0.0f;
		float rate = 0.0f;
		uint32_t flags = 0;
		float reach = 0.0f;
		float seed = 0.0f;
	};
	static constexpr uint32_t kPrintSpellChurn = 8u;
	static constexpr uint32_t kPrintSpellCap = 32;

	struct PrintLiveTile
	{
		float refillSince = 0.0f;
		bool dirty = false;
	};
	struct PrintStoreTile
	{
		std::vector<uint32_t> texels;
		float clock = 0.0f;
	};
	struct PrintStagingSlot
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		TrenchTileKey key{};
		DirectX::XMINT2 windowMin{};
		uint32_t dim = 0;
		float clock = 0.0f;
		uint64_t frame = 0;
		uint32_t generation = 0;
		bool busy = false;
	};

	Texture2D* printMap = nullptr;
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
	bool printRimFailed = false;
	float printRimWidthLast = -1.0f;
	uint32_t printMapDim = 0;
	DirectX::XMINT2 printWindowMin = { 0, 0 };
	bool printWindowValid = false;
	uint32_t printWorldspace = 0;
	float printRefillBank = 0.0f;
	std::atomic<bool> printReinjectRequested{ false };
	std::unordered_map<uint64_t, PrintLiveTile> printLive;
	std::mutex printStoreMutex;
	std::unordered_map<TrenchTileKey, PrintStoreTile, TrenchTileKeyHash> printStore;
	float printClockSnapshot = 0.0f;
	std::atomic<uint32_t> printStoreGeneration{ 0 };
	uint32_t printQueueGeneration = 0;
	std::array<PrintStagingSlot, kPrintStagingSlots> printStaging;
	winrt::com_ptr<ID3D11Buffer> printWriteBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> printWriteUAV;
	struct PrintWriteReadback
	{
		winrt::com_ptr<ID3D11Buffer> staging;
		DirectX::XMINT2 tileOrigin{};
		uint32_t rowTiles = 0;
		uint32_t worldspace = 0;
		uint64_t frame = 0;
		bool busy = false;
	};
	static constexpr uint32_t kPrintWriteSlots = 4;
	std::array<PrintWriteReadback, kPrintWriteSlots> printWriteReadbacks;
	uint64_t printLastClearFrame = 0;
	std::vector<uint32_t> printBlockScratch;
	std::vector<uint8_t> printBlockSeen;
	uint64_t printFrame = 0;
	uint32_t printStampBlocksLast = 0;
	uint32_t printRefillBlocksLast = 0;
	uint32_t printMirrorsLast = 0;
	uint32_t printInjectsLast = 0;
	size_t printStoreTilesLast = 0;
	bool debugObjectPrintView = false;
	Texture2D* printTiles = nullptr;
	std::vector<uint8_t> printTilesScratch;
	std::vector<uint8_t> printTilesUploaded;
	std::vector<uint32_t> printTileSum;
	DirectX::XMINT2 printTileSumOrigin = { 0, 0 };
	int32_t printTileSumDim = 0;
	DirectX::XMINT2 printWindowTarget = { 0, 0 };
	uint32_t printDimTarget = 0;
	DirectX::XMINT4 printRectPublished = { 0, 0, 0, 0 };
	uint32_t printTwinDrawsLast = 0;
	uint32_t parallaxCapturesLast = 0;
	uint32_t parallaxCoatDrawsLast = 0;

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
	PrintSpellMark HeldPrintFront(const HeldPrintSpell& a_held, float a_deltaTime) const;
	bool QueuePrintMirror(ID3D11DeviceContext* a_context, int32_t a_tx, int32_t a_ty, float a_clock);
	void PollPrintMirrors(ID3D11DeviceContext* a_context);
	void PollPrintWrites(ID3D11DeviceContext* a_context);
	uint32_t InjectPrintRect(ID3D11DeviceContext* a_context, const DirectX::XMINT4& a_rect, float a_clock);
	static bool DecayPrintTile(PrintStoreTile& a_tile, float a_clock);
	void EnforcePrintStoreCap();
	void RegisterPrintCoSave();
	void SavePrintStore(const SKSE::SerializationInterface* a_intfc);
	void LoadPrintStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);
	void ClearPrintStore(const char* a_why);
	DirectX::XMINT4 PrintRectForFrame();
	void PublishPrintTiles(ID3D11DeviceContext* a_context);
	bool PrintsUnder(const RE::NiBound& a_bound) const;
	void BindPrintReaders(ID3D11DeviceContext* a_context) const;

public:
	/** @brief Applies pending range-setting changes (trench window resize + map clear). Called at Prepass start; the first call applies loaded settings. */
	void ApplyRangeSettings();

protected:
	std::unordered_map<uint64_t, float2> stampPrevPositions;

	std::unordered_map<uint32_t, StampBones> stampBoneCache;

	enum class SpellElement
	{
		None,
		Fire,
		Frost,
		Shock,
		Force
	};

	static SpellElement ClassifyElement(const RE::EffectSetting* a_effect);

	float CrustBreakForce(float a_radius) const;

	bool ActorIsFloating(RE::Actor* a_actor, RE::NiAVObject* a_root, const StampBones* a_bones, bool a_useFeet, float a_groundZ, float* a_gapOut = nullptr, bool* a_byFeetOut = nullptr) const;

	bool ActorIsIncorporeal(RE::Actor* a_actor, RE::NiAVObject* a_root, StampBones* a_bones,
		float* a_alphaOut = nullptr, bool* a_flagOut = nullptr) const;
	static bool IsProxyOrOverlayGeometry(const RE::BSGeometry* a_geometry, const RE::BSShaderProperty* a_property);

	struct ElementalHit
	{
		SpellElement element = SpellElement::None;
		float remaining = 0.0f;
	};
	std::unordered_map<uint32_t, ElementalHit> lastElementalHit;

	struct InnateAuraRecord
	{
		SpellElement element = SpellElement::None;
		float rateScale = 1.0f;
		float reachOverride = 0.0f;
	};
	std::unordered_map<uint32_t, InnateAuraRecord> innateAuraByRace;
	std::unordered_map<uint32_t, InnateAuraRecord> innateAuraByBase;

	const InnateAuraRecord* ResolveInnateAura(RE::Actor* a_actor);

	static bool AuraFromSpellList(const RE::TESSpellList* a_list, InnateAuraRecord& a_out);

	struct StampStats
	{
		uint feet = 0;
		uint limbs = 0;
		uint shapes = 0;
		uint props = 0;
		uint propRefs = 0;
		uint propMovers = 0;
		uint propsRasterized = 0;
		uint actorsRasterized = 0;
		uint corpsesRasterized = 0;
		uint corpsesLatched = 0;
		uint spells = 0;
		uint beforeSpells = 0;
		uint incorporeal = 0;
		uint nearestFeet = 0;
		uint nearestLimbs = 0;
		uint nearestUsableFeet = 0;
		bool nearestFallback = false;
		float nearestDryTravel = 0.0f;
		uint fallbackActors = 0;
		bool nearestElemental = false;
		uint floating = 0;
		float nearestGapToRoot = 0.0f;
		float nearestGapToLand = 0.0f;
		uint nearestState = 0;
		bool nearestFloating = false;
		bool nearestValid = false;
		float nearestBodyAlpha = 1.0f;
		std::string nearestBodyAlphaSource;
		bool nearestGhostFlag = false;
		bool nearestIncorporeal = false;
		uint32_t nearestFormID = 0;
		uint budgetTurnedAway = 0;
	};
	StampStats stampStats;

	bool debugSkeletonProbe = false;
	bool skeletonProbeDumpRequested = false;
	uint32_t skeletonProbeTarget = 0;
	struct SkeletonProbe
	{
		bool valid = false;
		uint32_t formID = 0;
		std::string actorName;
		const char* verdict = "";
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
		float shellDepth = -1.0f;
		bool cellBaked = false;
	};
	SkeletonProbe skeletonProbe;

	struct PropAnchor
	{
		RE::NiPoint3 pos;
		uint32_t cycle = 0;
		bool mover = false;
	};
	std::unordered_map<uint32_t, PropAnchor> propPrevPositions;
	static constexpr uint32_t kPropScanInterval = 6;
	uint32_t propScanCycle = 0;
	std::vector<RE::TESObjectCELL*> propScanCells;
	uint32_t propScanFrame = 0;
	std::vector<RE::ObjectRefHandle> propScanMovers[kPropScanInterval];
	std::vector<RE::ObjectRefHandle> propScanHazards[kPropScanInterval];
	size_t PropScanHazardCount() const
	{
		size_t n = 0;
		for (const auto& slice : propScanHazards)
			n += slice.size();
		return n;
	}
	uint propScanRefs = 0;

	struct CorpseRest
	{
		uint16_t stillFrames = 0;
		bool settled = false;
		RE::NiPoint3 prevCenter;
		bool hasPrevCenter = false;
		RE::NiPoint3 restCenter;
		float prevZ = 0.0f;
		bool hasPrevZ = false;
	};
	std::unordered_map<uint32_t, CorpseRest> corpseRestStates;

	static constexpr float kImpactSpeedFloor = 400.0f;
	static constexpr float kImpactSpeedSmoothing = 0.1f;
	static constexpr float kFootTeleportSpeed = 4000.0f;
	static constexpr float kImpactKnockedDown = 0.6f;
	static constexpr float kRushEnergy = 0.5f;
	static constexpr float kRushRearm = 0.4f;
	struct ImpactTrack
	{
		RE::NiPoint3 prev;
		double time = 0.0;
		float speed = 0.0f;
		float fall = 0.0f;
		float speedEnergy = 0.0f;
		float landingEnergy = 0.0f;
		float knockEnergy = 0.0f;
		float landingSpeed = 0.0f;
		bool seen = false;
		bool wasInAir = false;
		/** @brief The largest of the three: what packs snow onto a body's coat, and shakes it off. */
		float Jolt() const { return std::max({ speedEnergy, landingEnergy, knockEnergy }); }
	};
	std::unordered_map<uint32_t, ImpactTrack> impactTracks;
	struct SpoilFoot
	{
		float2 last{};
		uint32_t frame = 0;
		bool planted = false;
	};
	std::unordered_map<uint64_t, SpoilFoot> spoilFeet;
	uint64_t speedSpoilWedges = 0;
};
