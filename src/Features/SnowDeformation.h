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
	static constexpr size_t kMaxSpellEmitters = 64;
	static constexpr size_t kSpellEmitterCeiling = 192;
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

	SettingsGPU GetCommonBufferData(bool a_inWorld);

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

	float ComputeSnowfallIntensity() const;
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
	const SnowWeatherRange& SnowWeathers();
	SnowWeatherRange snowWeatherRange;
	static RE::TESWorldSpace* NoSkyWorldspace();
	static const char* NotSnowKeywordIn(const char* a_name);
	float snowfallIntensity = 0.0f;

	struct GameClock
	{
		float lastHours = -1.0f;
		float timescale = 20.0f;
		float elapsedHours = 0.0f;
		bool reversed = false;
		float Read(std::atomic<bool>& a_unarm);
	};
	GameClock gameClock;
	std::atomic<bool> gameClockUnarm{ false };
	std::atomic<float> gameClockHours{ -1.0f };
	void TickGameClock();
	GameClock weatherClock;
	std::atomic<bool> weatherClockUnarm{ false };

	ConstantBuffer* perFrame = nullptr;
	Texture2D* deformationTextures[2] = { nullptr, nullptr };

	Texture2D* bermFieldTexture = nullptr;

	ID3D11ShaderResourceView* GetDeformationSRV() const { return deformationTextures[0] ? deformationTextures[0]->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetBermFieldSRV() const { return bermFieldTexture ? bermFieldTexture->srv.get() : nullptr; }

	Texture2D* passStateTexture = nullptr;
	Texture2D* bodyReachTexture = nullptr;
	Texture2D* trackClockTexture = nullptr;
	ID3D11ComputeShader* trackRebaseCS = nullptr;
	ID3D11ComputeShader* GetTrackRebaseCS();
	static constexpr float kTrackAgeSpan = 24.0f;
	float4 TrackAgeConstants() const;
	float TrackDateClock() const { return snowClimate.ageingClock + snowClimate.ageingRebase; }
	void TickTrenchAgeing();
	static constexpr float kTrackAgeHorizon = 128.0f;
	double passClockSeconds = 256.0;
	static constexpr UINT kUntouchedTilesSlot = 57;
	static constexpr UINT kBodyReachSlot = 56;
	static constexpr UINT kTerrainWetSlot = 58;

	Texture2D* bloodMapTexture = nullptr;
	Texture2D* bloodClockTexture = nullptr;
	ID3D11ShaderResourceView* GetBloodMapSRV() const { return bloodMapTexture ? bloodMapTexture->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetBloodClockSRV() const { return bloodClockTexture ? bloodClockTexture->srv.get() : nullptr; }
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
	void CaptureBloodDraw(RE::BSRenderPass* a_pass, bool a_skinned);
	void RenderBloodCapture();
	uint32_t DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, BloodCB& a_cb, ID3D11VertexShader* a_rigidVS = nullptr, UINT a_instances = 4u, bool a_ownClock = false, ID3D11VertexShader* a_skinnedVS = nullptr);
	void RegisterDecalOverlay(const BloodCapture& a_capture);
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
	void CaptureDecalOverlay(RE::BSRenderPass* a_pass);
	bool DecalOverlayWanted() const { return settings.BloodOnSnow || settings.DecalsOnObjectSnow; }
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

	static constexpr uint32_t kBloodTileDim = 512;
	static constexpr uint32_t kBloodTilesAcross = 8;
	static constexpr uint32_t kBloodMaxTiles = kBloodTilesAcross * kBloodTilesAcross;
	static constexpr float kBloodTileTexel = 0.25f;
	static constexpr uint32_t kBloodTileMips = 4;
	static constexpr float kBloodTileApron = 4.0f;
	static constexpr float kBloodTileCell = float(kBloodTileDim) * kBloodTileTexel - 2.0f * kBloodTileApron;
	static constexpr float kBloodTileKeepUnits = 7000.0f;
	static constexpr int32_t kBloodTileKeepCells = 59;
	static constexpr int32_t kBloodTileIndexDim = 128;
	static_assert(float(kBloodTileKeepCells) * kBloodTileCell >= kBloodTileKeepUnits && 2 * kBloodTileKeepCells + 1 <= kBloodTileIndexDim);
	uint32_t BloodTilesAcross() const { return kBloodTilesAcross; }
	uint32_t BloodTileCount() const { return kBloodMaxTiles; }
	float BloodTileTexel() const { return kBloodTileTexel; }
	float BloodTileCell() const { return kBloodTileCell; }
	int32_t BloodCellOf(float a_world) const { return int32_t(std::floor(a_world / kBloodTileCell)); }
	int32_t BloodTileKeepCells() const { return kBloodTileKeepCells; }
	static constexpr uint32_t kBloodTileBlock = 8;
	static constexpr uint32_t kBloodTileSnapDim = 80;
	struct BloodTile
	{
		bool live = false;
		int32_t cellX = 0;
		int32_t cellY = 0;
		uint32_t gen = 0;
		float lastBurial = 0.0f;
		std::vector<BloodCapture> draws;
		bool discs = false;
		float2 seedOrigin{ 0.0f, 0.0f };
		int32_t snapLogical[2]{};
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
	bool bloodTilesDrop = false;
	winrt::com_ptr<ID3D11Texture2D> bloodPreSkinDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodPreSkinDepthSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodPreSkinDepthUAV;
	bool bloodPreSkinDepthThisFrame = false;
	bool EnsureBloodPreSkinDepth(UINT a_width, UINT a_height);
	winrt::com_ptr<ID3D11Texture2D> bloodTileSnapshot;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileSnapshotSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileSnapshotUAV;
	winrt::com_ptr<ID3D11Texture2D> bloodTileRedrawn;
	winrt::com_ptr<ID3D11RenderTargetView> bloodTileRedrawnRTV;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileRedrawnSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileRedrawnUAV;
	winrt::com_ptr<ID3D11Texture2D> bloodTileResidue;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileResidueSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileResidueUAV;
	void BuildBloodTileMips(ID3D11DeviceContext* a_context, uint32_t a_slot);
	winrt::com_ptr<ID3D11Texture2D> bloodTileAtlas;
	winrt::com_ptr<ID3D11ShaderResourceView> bloodTileAtlasSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> bloodTileAtlasUAV[kBloodTileMips];
	uint32_t bloodTileAtlasRows = 0;
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
	void BloodMarkBounds(const BloodCapture& a_capture, BloodSeen& a_seen);
	int32_t FindBloodTile(int32_t a_cellX, int32_t a_cellY) const;
	int32_t AllocateBloodTile(int32_t a_cellX, int32_t a_cellY, int32_t a_cameraCellX, int32_t a_cameraCellY);
	void FillBloodTileCB(BloodTileCB& a_cb, uint32_t a_slot) const;
	void SeedBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot);
	void MergeBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot, const std::vector<BloodDisc>& a_discs);
	void RenderBloodTiles(const std::vector<BloodDisc>& a_discs);

	static constexpr uint32_t kRuneMaxTiles = 4;
	static constexpr uint32_t kRuneTileDim = 512;
	static constexpr float kRuneTileHalf = 110.0f;
	static constexpr uint32_t kFrostCells = 64;
	static constexpr float kFrostCellSize = 128.0f;
	static constexpr uint32_t kFrostTextures = 4;
	static constexpr float kFrostDecalAboveSnow = 24.0f;
	struct RuneSite
	{
		uint32_t formID = 0;
		RE::NiPoint3 position;
		std::string decalPath;
		float distSq = 0.0f;
	};
	struct RuneCapture
	{
		BloodCapture draw;
		winrt::com_ptr<ID3D11ShaderResourceView> normal;
		winrt::com_ptr<ID3D11ShaderResourceView> glow;
		float3 emissive{ 0.0f, 0.0f, 0.0f };
		float emissiveMult = 1.0f;
		bool glowMap = false;
		float4 specular{ 0.0f, 0.0f, 0.0f, 0.0f };
		RE::NiPoint3 centre;
		float radius = 0.0f;
	};
	struct alignas(16) RuneCB
	{
		float4 RuneParams;
		float4 RuneRects[kRuneMaxTiles];
		float4 RuneTints[kRuneMaxTiles];
		float4 FrostRect;
		float4 FrostNormals;
		float4 RuneSpecular[kRuneMaxTiles];
		float4 RuneReach[kRuneMaxTiles];
		float4 FrostSpecular[kFrostTextures];
		float4 FrostTints[kFrostTextures];
		float4 FrostEmissiveMults;
	};
	STATIC_ASSERT_ALIGNAS_16(RuneCB);
	static_assert(kFrostTextures == 4, "RuneCB's frost rows and the shells' FrostDiffuse / FrostNormal / FrostGlow slots hold four");
	std::mutex runeLock;
	std::vector<RuneSite> runeSites;
	std::atomic<uint32_t> runeSitesLive{ 0 };
	std::vector<RuneSite> runeSitesScratch;
	std::unordered_map<const RE::BGSProjectile*, std::string> runeDecalPathCache;
	std::unordered_set<uint32_t> runeLifted;
	std::vector<RuneCapture> runeCaptures;
	std::unordered_set<const RE::BSGeometry*> runeCaptureSet;
	std::vector<RuneCapture> frostCaptures;
	std::unordered_set<std::string> runeDecalPathsLogged;
	Texture2D* runeAtlasTexture = nullptr;
	winrt::com_ptr<ID3D11Texture2D> runeDepthTexture;
	winrt::com_ptr<ID3D11DepthStencilView> runeDepthDSV;
	winrt::com_ptr<ID3D11DepthStencilState> runeDepthState;
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
	winrt::com_ptr<ID3D11ShaderResourceView> runeTileDiffuse[kRuneMaxTiles];
	winrt::com_ptr<ID3D11ShaderResourceView> runeTileNormal[kRuneMaxTiles];
	winrt::com_ptr<ID3D11ShaderResourceView> runeTileGlow[kRuneMaxTiles];
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
	void ConsiderRune(RE::Projectile* a_projectile, const RE::NiPoint3& a_camera, float a_cullRadius, RE::TES* a_tes);
	void PublishRuneSites(const std::unordered_set<uint32_t>& a_present);
	bool CaptureRuneDraw(RE::BSRenderPass* a_pass, bool a_engineDecal);
	bool EnsureRuneResources();
	bool EnsureFrostPlanes(uint32_t a_marks, uint32_t a_entries);
	void BindFrostField(ID3D11DeviceContext* a_context, bool a_bind);
	void BindMarkReaders(ID3D11DeviceContext* a_context, bool a_bind);
	void RenderRuneCapture();

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
	void UpdateUndulationField();
	ID3D11ShaderResourceView* GetUndulationFieldSRV() const { return undulationFieldTexture ? undulationFieldTexture->srv.get() : nullptr; }

	virtual void SetupResources() override;

	void CreateDeformationTextures();

	virtual void Prepass() override;
	void PrepassSimulation();
	virtual void PostPrepass() override;

	ID3D11ComputeShader* GetDeformationRingCS();
	ID3D11ComputeShader* GetDeformationEvolveCS();
	ID3D11ComputeShader* GetDeformationStampCS();
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

	ID3D11ComputeShader* GetDeformationScanEvolveCS();
	ID3D11ComputeShader* GetDeformationScanBermCS();
	ID3D11ComputeShader* GetBermFieldTiledCS();
	ID3D11ComputeShader* deformationScanEvolveCS = nullptr;
	ID3D11ComputeShader* GetDeformationScanSnapshotCS();
	ID3D11ComputeShader* GetDeformationSnapshotCS();
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
	ID3D11ComputeShader* GetUntouchedTilesCS();
	ID3D11ComputeShader* untouchedTilesCS = nullptr;

	struct ArrivalRect
	{
		int x0, y0, w, h;
	};
	int ComputeArrivalRects(DirectX::XMINT2 a_scroll, bool a_clearing, ArrivalRect a_rects[2]) const;

	void CopyLogicalBox(ID3D11Texture2D* a_dst, ID3D11Texture2D* a_src, int a_x0, int a_y0, int a_w, int a_h, DirectX::XMINT2 a_physOrigin);
	virtual void ClearShaderCache() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

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
	void UpdateTerrainWet();
	ID3D11ShaderResourceView* TerrainWetSRV() const;
	uint64_t shellTerrainWetKey[3] = {};
	bool shellTerrainWetValid = false;
	ID3D11ShaderResourceView* ShellWaterSRV() const;
	static constexpr int kShellFineCells = 9;
	static constexpr float kShellFineTexel = 32.0f;
	static constexpr int kShellFineDim = kShellFineCells * 128;
	static constexpr size_t kShellFineCacheCells = 128;
	std::deque<uint64_t> shellFineOrder;
	bool ReadLandMeshHeights(RE::TESObjectLAND* a_land, ShellCellData& a_data);
	void RefreshLandMeshHeights();
	static RE::BSTriShape* LandQuadGeometry(RE::TESObjectLAND::LoadedLandData* a_loaded, uint32_t a_quad);
	uint32_t fineRefreshFrame = 0;
	std::atomic<bool> shellFineDirty{ false };
	std::unordered_map<uint64_t, uint32_t> shellFineRefused;
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
	float ShellTessTargetRenderPx() const;

	static constexpr uint32_t kClimateLUTRows = 3;
	static constexpr float kClimateLUTStep = 2.0f;

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
	bool shellDeferredThisFrame = false;
	Texture2D* shellDeferredAttr[3] = {};
	winrt::com_ptr<ID3D11DepthStencilState> shellDeferredDepthState;
	bool EnsureDeferredResources();
	ID3D11PixelShader* shellPSDeferred = nullptr;
	ID3D11PixelShader* GetShellPSDeferred();
	ID3D11PixelShader* shellPSDeferredConditionView = nullptr;
	std::atomic<int> conditionViewShaderState = 0;
	std::future<bool> conditionViewCompile;
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
	void SyncSssDepth();
	void RestoreSssDepth();
	void DispatchSssDepth(ID3D11ComputeShader* a_cs, const char* a_pass);
	ID3D11ComputeShader* sssSyncCS = nullptr;
	ID3D11ComputeShader* sssRestoreCS = nullptr;
	ID3D11ComputeShader* GetSssSyncCS();
	ID3D11ComputeShader* GetSssRestoreCS();

	uint32_t skinSssMarchFrame = UINT32_MAX;
	ID3D11ShaderResourceView* shellGroundMarksSRV = nullptr;
	Texture2D* skinSssSaved = nullptr;
	bool SkinSssMarchReady(const D3D11_VIEWPORT* a_viewport);
	void MarchSkinSss(ID3D11ShaderResourceView* a_skinDepthSRV, ID3D11ShaderResourceView* a_sceneDepthSRV, const D3D11_VIEWPORT* a_viewport);
	ID3D11ComputeShader* skinSssSyncCS = nullptr;
	ID3D11ComputeShader* skinSssRestoreCS = nullptr;
	ID3D11ComputeShader* GetSkinSssSyncCS();
	ID3D11ComputeShader* GetSkinSssRestoreCS();
	Texture2D* sssFirstMarchDepth = nullptr;
	uint32_t sssFirstMarchFrame = UINT32_MAX;
	D3D11_BOX skinSssRectLast{};
	Texture2D* sssChangedTiles = nullptr;
	ID3D11ComputeShader* skinSssSyncMarkCS = nullptr;
	ID3D11ComputeShader* GetSkinSssSyncMarkCS();
	ID3D11ComputeShader* GetSssChangedRaymarchCS();
	ID3D11ComputeShader* sssChangedRaymarchCS = nullptr;
	uint32_t sssChangedRaymarchSamples = 0;
	bool sssChangedMarchOff = false;
	uint32_t sssChangedMarchFrame = UINT32_MAX;
	void CopyFirstMarchDepth();
	bool EnsureSssChangedTiles(UINT a_width, UINT a_height);
	ID3D11PixelShader* shellFillPSClaim = nullptr;
	ID3D11PixelShader* GetShellFillPSClaim();
	static void SetShellSeamBounds(ShellCB& a_cb, int a_minX, int a_minY, int a_maxX, int a_maxY);

	ID3D11VertexShader* GetShellFillVS();
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
	void TakePreSnowCopy(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_mainDepthSRV);
	void CopyDepthRect(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_mainDepthSRV, const D3D11_BOX& a_box, ID3D11UnorderedAccessView* a_uav);
	ID3D11PixelShader* GetShellFillPSPreSnow();
	ID3D11PixelShader* shellFillPSPreSnow = nullptr;
	struct alignas(16) DepthSyncRectCB
	{
		uint32_t Origin[2];
		uint32_t Extent[2];
	};
	ConstantBuffer* depthSyncRectCB = nullptr;
	ConstantBuffer* DepthSyncRectBuffer();
	ID3D11ComputeShader* GetPreSnowCopyCS();
	ID3D11ComputeShader* preSnowCopyCS = nullptr;
	D3D11_BOX ScreenRectBox(const D3D11_VIEWPORT* a_viewport, uint32_t a_width, uint32_t a_height, uint32_t a_guard) const;
	winrt::com_ptr<ID3D11DepthStencilState> shellPrepassMainDepthState;
	winrt::com_ptr<ID3D11DepthStencilState> shellFillDepthState;

	ConstantBuffer* shellCB = nullptr;

	struct LightningArc
	{
		RE::NiPoint3 from;
		RE::NiPoint3 to;
		float age = 0.0f;
		float life = 0.16f;
		float seed = 0.0f;
	};
	static constexpr size_t kMaxLightningArcs = 24;
	static constexpr uint kLightningArcSegments = 32;
	std::vector<LightningArc> lightningArcs;
	uint32_t lightningArcSeed = 0;

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
	winrt::com_ptr<ID3D11BlendState> arcBlendState;
	winrt::com_ptr<ID3D11DepthStencilState> arcDepthState;
	winrt::com_ptr<ID3D11RasterizerState> arcRasterState;

	static bool LoadGameDDS(const std::string& a_dataRelativePath, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv);
	ID3D11VertexShader* GetLightningArcVS();
	ID3D11PixelShader* GetLightningArcPS();
	bool EnsureLightningArcResources();

	void EmitLightningArc(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to);
	void UpdateLightningArcs(float a_deltaTime);
	void DrawLightningArcs();
	winrt::com_ptr<ID3D11RasterizerState> shellRasterState;
	ID3D11RasterizerState* GetSkinRasterState(bool a_decal = false);
	winrt::com_ptr<ID3D11RasterizerState> skinRasterState;
	winrt::com_ptr<ID3D11RasterizerState> skinDecalRasterState;
	float skinRasterBiasBuilt = 0.0f;
	float skinRasterSlopeBuilt = 0.0f;
	static constexpr float kDecalViewportMaxDepth = 0.99997199f;
	winrt::com_ptr<ID3D11DepthStencilState> shellDepthState;

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

	void EnsureShellSnowTextures();
	static std::vector<std::pair<const char*, const char*>> ShellPSDefines(const char* a_extra = nullptr);
	void RefreshSnowPBRParams();

	void BakeShellCell(RE::TESObjectLAND* land);

	void UpdateShellTerrainWindow();

	void UpdateActiveWorldspace();

	uint16_t RegisterLandTexture(RE::TESLandTexture* a_landTexture);

	void RefreshLandTextureDepths();

	void SetLandTextureOverride(const std::string& a_path, std::optional<float> a_depth);

	std::vector<float> LandTextureDepthSnapshot();

	ShellProbe ProbeShellData(float a_x, float a_y);

	void ResolveLandTextureDepthLocked(LandTextureEntry& a_entry);

	void ResolveLandTextureDepthsLocked();

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
	ID3D11ShaderResourceView* GetLODTile(const std::string& a_worldspace, int a_cellX, int a_cellY);

	void FillShellWindowFromHeightmap();
	ID3D11ComputeShader* GetWindowFillCS();
	ID3D11ComputeShader* windowFillCS = nullptr;
	std::string lastFillWorldspace;

	float GetNominalSnowDepthAt(float a_x, float a_y, float a_missing);

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
	void GatherWaterObjects();
	ankerl::unordered_dense::set<void*> capturedStaticsSet;
	std::atomic<uint32_t> statCapturedStatics{ 0 };

	uint32_t statProjMatched = 0;
	uint32_t statProjNoProjection = 0;
	uint32_t statProjVetoed = 0;
	uint32_t statProjMatchedPrev = 0;
	uint32_t statProjNoProjectionPrev = 0;
	uint32_t statProjVetoedPrev = 0;

	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass, uint32_t a_rawTechnique);

	void SetProjectedSnowBit(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass);

	ObjectSnowProbe ProbeObjectSnow(float a_x, float a_y);
	void InstallStaticsCaptureHook();
	void InstallCameraHook();
	void InstallAPI();
	bool APIIsActive() const;
	float APISnowDepthAt(float a_x, float a_y);
	float APISnowDepthAtRef(RE::TESObjectREFR* a_ref);
	float APISnowAccumulation() const;
	bool SampleShellSurface(float a_x, float a_y, float& a_surface) const;
	void ClampCameraAboveSnow();
	void ItemSinkUpdate();
	void ApplySpellLifts();
	void CreatureSinkUpdate();
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

	static void CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
		winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv, const D3D11_BOX* a_box = nullptr, const D3D11_BOX* a_extent = nullptr);

	winrt::com_ptr<ID3D11ShaderResourceView> shadowAtlasLiveSRV;
	ID3D11ShaderResourceView* ShadowAtlasSRV() const { return shadowAtlasLiveSRV.get(); }
	winrt::com_ptr<ID3D11SamplerState> shadowCmpSampler;
	winrt::com_ptr<ID3D11SamplerState> shellLinearSampler;

	void CaptureShadowAtlas();

	ID3D11VertexShader* GetPatchShadowVS();

	std::unique_ptr<ShellCB> lastShellCBData;

	winrt::com_ptr<ID3D11DepthStencilView> shadowAtlasDSV[4];
	ID3D11Texture2D* shadowAtlasDSVTexture = nullptr;
	uint32_t shadowAtlasDSVSlice[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	uint32_t sunCascadeSlice[2] = { 0, 1 };
	winrt::com_ptr<ID3D11RasterizerState> shadowCastRS;
	winrt::com_ptr<ID3D11DepthStencilState> shadowCastDSS;

	void InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV);
	void RecordSunCascadeSlices(uint32_t a_atlasSlices);

	void RefreshShellGridPlacement(ShellCB& a_cb, const float4* a_camAdjust = nullptr);

	uint32_t dbgLodDescriptorCount = 0;
	float dbgLodEndSplits[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t dbgLodAtlasSlices = 0;

	void QueryAdapterVRAM(uint64_t& a_usageMB, uint64_t& a_budgetMB, uint64_t* a_nonLocalMB = nullptr);
	uint64_t SumFeatureTextureBytes(std::string& a_breakdown);
	void TickVRAMLog();
	uint64_t vramTickCounter = 0;
	uint64_t vramLastLoggedMB = 0;
	uint64_t vramLastLogTick = 0;
	uint64_t vramListedBytes = 0;
	uint32_t vramListedResources = 0;
	uint64_t vramListedTick = 0;
	void TickRowSpikeLog();
	uint64_t rowSpikeQuietUntil = 0;

	Buffer* pointShadowLights = nullptr;

	winrt::com_ptr<ID3D11Texture2D> pointShadowLiveTex;
	winrt::com_ptr<ID3D11ShaderResourceView> pointShadowLiveSRV;
	ID3D11ShaderResourceView* PointShadowAtlasSRV() const { return pointShadowLiveSRV.get(); }

	PointShadowLightData pendingPointShadows[kPointShadowMaxLights] = {};

	void CapturePointShadowMask();

	void UpdatePointShadowLights();

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

	SmoothedNormalsEntry* EnsureSmoothedNormals(RE::BSGeometry* a_geometry);
	SmoothedNormalsEntry* SmoothedNormalsFor(CapturedSnowStatic& a_cap);
	uint32_t smoothedNormalsResets = 0;
	void FillSkinDrawCB(const CapturedSnowStatic& a_cap, bool a_s4Shell, float a_vertexCount, bool a_hasSmoothedNormals, bool a_hasObjectTop, StaticsCB& a_scb) const;
	void FillPatchDrawCB(StaticsCB& a_scb) const;

	static constexpr uint kHeightMapDim = 2048;
	static constexpr float kObjectConeSlope = 3.0f;
	float ObjectRasterHalfExtent() const { return std::clamp(settings.ObjectRasterReachM, 29.0f, 234.0f) * kUnitsPerMeter; }
	float SkinFadeEndUnits() const { return settings.RangeSkinsM * kUnitsPerMeter; }
	bool cityWorldspace = false;
	bool GroundShellsSuspended() const { return cityWorldspace && !settings.CityShells; }
	float BermReachScale() const { return std::exp2(1.0f - std::clamp(settings.BermSteepness, 0.0f, 2.0f)); }
	float BodyPrintScale() const { return 1.0f - std::clamp(settings.SnowResistance, 0.0f, 100.0f) * 0.01f; }
	bool InInteriorCell() const
	{
		auto* tes = globals::game::tes;
		return tes && tes->interiorCell;
	}
	float objectRasterHalfExtentBuilt = 0.0f;

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
	bool RoadMapsWanted() const;
	bool skyOpenCurrent = true;
	uint32_t heightProbeShownFrame = UINT32_MAX;
	bool FineLevelOn() const { return settings.ObjectSnowNearDetail; }
	static constexpr int kQualityPresetCount = 3;
	void ApplyQualityPreset(int a_preset);
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

	struct alignas(16) HeightGhostCB
	{
		Matrix GhostViewProj;
		float4 GhostCameraPosAdjust;
	};
	STATIC_ASSERT_ALIGNAS_16(HeightGhostCB);
	ConstantBuffer* heightGhostCB = nullptr;

	static constexpr uint kVoxelDim = 256;
	static constexpr uint kVoxelDimWide = 512;
	static constexpr uint kVoxelDimFlatZ = 128;
	DirectX::XMUINT3 VoxelDimsForLevel(uint a_level) const;
	DirectX::XMUINT3 VoxelDimsMax() const;
	DirectX::XMFLOAT3 VoxelExtentForLevel(uint a_level) const;
	float VoxelSizeLive() const { return std::clamp(settings.VolumeVoxelSize, 1.0f, 24.0f); }
	float voxelSizeBuilt = 0.0f;

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
	DirectX::XMFLOAT3 VoxelLevelCentre(uint a_level) const;
	uint32_t VoxelLevelPeriod(uint a_level) const;
	float VoxelThresholdForLevel(uint a_level) const;
	uint32_t voxelFrame = 0;
	static constexpr float kVoxelReachFrac = 0.9f;
	static constexpr float kVoxelReachFracFar = 0.85f;
	static constexpr uint kVoxelListArgsSeed = 0;
	static constexpr uint kVoxelListArgsZ = 12;
	static constexpr uint kVoxelListArgsX = 24;
	static constexpr uint kVoxelListArgsY = 36;
	static constexpr uint kVoxelListArgsFlags = 48;
	static constexpr uint kVoxelListHeaderBytes = 64;
	static constexpr uint kVoxelListBytes = 64 + 3 * 16384;
	static constexpr uint32_t kVoxelFullRefreshRebuilds = 900;
	ID3D11ComputeShader* voxelOccFlagsCS = nullptr;
	ID3D11ComputeShader* voxelDecayCS = nullptr;
	ID3D11ComputeShader* voxelDirtyColsCS = nullptr;
	ID3D11ComputeShader* voxelBrickFlagsCS = nullptr;
	static constexpr float kVoxelBandFrac = 0.2f;
	static constexpr float kVoxelEdgeNoiseCell = 12.0f;
	static constexpr float kVoxelHeadroomUnits = 8.0f;
	static constexpr uint kVoxelUnderLayers = 4;
	static constexpr uint kVoxelMaxLevels = 6;
	struct VoxelLevel
	{
		Texture3D* volume[2] = { nullptr, nullptr };
		Texture3D* field = nullptr;
		Texture2D* heightMap = nullptr;
		Texture2D* heightMapUnder = nullptr;
		Buffer* bricks = nullptr;
		Buffer* drawArgs = nullptr;
		Buffer* dirty = nullptr;
		Buffer* flags = nullptr;
		Buffer* lists = nullptr;
		uint32_t rebuilds = 0;
		bool cleared = false;
		uint current = 0;
		DirectX::XMUINT3 dims = { 0, 0, 0 };
		bool valid = false;
		bool updated = false;
		bool skipped = false;
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
	void VoxelReachBand(uint a_level, float& a_start, float& a_end) const;
	void FillVoxelCB(uint a_level, VoxelVolumeCB& a_cb) const;
	int voxelSliceLevel = 0;

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
	static constexpr float kVoxelShelterDust = 0.1f;

	ID3D11ComputeShader* voxelSeedCS = nullptr;
	ID3D11ComputeShader* voxelBlurZCS = nullptr;
	ID3D11ComputeShader* voxelBlurCS = nullptr;
	ConstantBuffer* voxelDrawCB = nullptr;
	ConstantBuffer* voxelDebugCB = nullptr;
	winrt::com_ptr<ID3D11SamplerState> voxelWrapSampler;
	ID3D11ComputeShader* voxelBrickListCS = nullptr;
	ID3D11VertexShader* voxelShellVS = nullptr;
	ID3D11PixelShader* voxelShellPS = nullptr;
	void DrawVoxelSnow();
	int voxelSliceSource = 2;
	Texture2D* voxelSliceTexture = nullptr;
	Texture3D* voxelSupport = nullptr;
	DirectX::XMUINT3 voxelSupportDims = { 0, 0, 0 };
	ConstantBuffer* voxelCB = nullptr;
	winrt::com_ptr<ID3D11RasterizerState> voxelRasterState;
	ID3D11VertexShader* voxelVS = nullptr;
	ID3D11GeometryShader* voxelGS = nullptr;
	ID3D11PixelShader* voxelPS = nullptr;
	ID3D11ComputeShader* voxelScrollCS = nullptr;
	ID3D11ComputeShader* voxelSliceCS = nullptr;
	bool voxelShadersFailed = false;
	bool showVoxelSlice = false;
	bool showVolumeRings = false;
	bool debugForceVolumeRebuild = false;
	bool voxelIdleSkipped = false;
	uint32_t voxelIdleBlockers = 0;
	float voxelSkipRate = -1.0f;
	uint32_t voxelSkipTallyFrames = 0;
	uint32_t voxelSkipTallySkipped = 0;
	int voxelSliceAxis = 1;
	float voxelSliceOffset = 0.0f;
	float voxelMemorySeconds = 0.5f;
	bool voxelSliceXray = true;
	winrt::com_ptr<ID3D11Buffer> voxelCountBuffer;
	winrt::com_ptr<ID3D11UnorderedAccessView> voxelCountUAV;
	winrt::com_ptr<ID3D11Buffer> voxelCountStaging[2];
	uint voxelCountCursor = 0;
	uint32_t voxelOccupancy = 0;
	bool voxelOccupancyValid = false;
	bool EnsureVoxelResources();
	void RenderVoxelVolume(const StaticsCB* a_records, uint32_t a_captureCount, bool a_recordsLive, uint32_t& a_parity);
	void UpdateVoxelSliceTexture();

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

	ID3D11ComputeShader* GetExclusionFieldCS();
	ID3D11ComputeShader* exclusionFieldCS = nullptr;
	ID3D11ShaderResourceView* GetExclusionFieldSRV() const { return exclusionFieldTexture ? exclusionFieldTexture->srv.get() : nullptr; }

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

	void CreateHeightFieldResources();
	void RenderObjectHeightMap();
	void SkipObjectHeightMap();
	void IdleObjectHeightMap();
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
	bool EnsureCoatParallaxShaders();
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
	bool EnsureSkinCullResources(uint32_t a_count, ID3D11ShaderResourceView* a_mainDepthSRV, uint32_t a_extentW, uint32_t a_extentH);
	ID3D11VertexShader* staticsTessVS = nullptr;
	ID3D11HullShader* staticsHS = nullptr;
	ID3D11DomainShader* staticsDS = nullptr;
	ID3D11VertexShader* patchVS = nullptr;
	ID3D11VertexShader* patchShadowVS = nullptr;
	winrt::com_ptr<ID3D11Buffer> patchCasterIB;
	uint32_t patchCasterIndexCount = 0;
	bool EnsurePatchCasterIB();
	struct alignas(16) PatchCullCB
	{
		float4 Lines[6];
		uint32_t LineCount;
		uint32_t RowStart;
		uint32_t ColStart;
		uint32_t pad;
	};
	ConstantBuffer* patchCullCB = nullptr;
	uint32_t FillPatchCullLines(const ShellCB& a_cb, PatchCullCB& a_out) const;
	float2 PatchLatticeCentre() const;
	struct PatchBand
	{
		uint32_t x0, x1, y0, y1;
	};
	static constexpr uint32_t kPatchBands = 4;
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
	bool EnsurePatchDeferredResources();
	ID3D11PixelShader* patchConditionViewPS = nullptr;
	std::future<ID3D11PixelShader*> patchConditionViewJob;
	bool patchConditionViewTried = false;
	ID3D11PixelShader* PatchConditionViewPS();
	static ID3D11PixelShader* CompilePatchDeferredPS(int a_which);
	void PrimePatchDeferred();
	bool PatchDeferredReady();
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
	bool UploadStaticsRecords(const StaticsCB* a_records, uint32_t a_count, const uint8_t* a_needed = nullptr);
	void BindStaticsRecord(uint32_t a_index, bool a_pixelStage, bool a_tessStages, uint32_t& a_parity);
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> staticsILCache;
	ID3D11InputLayout* StaticsInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc);
	std::unordered_map<uint64_t, winrt::com_ptr<ID3D11InputLayout>> staticsILParallaxCache;
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
	void DrawContactCapture(ID3D11DeviceContext* a_context, bool a_coatOnly = false);

	bool EnsureStaticsShaders();
	void DrawCapturedStatics();

	void TESObjectLAND_SetupMaterial(RE::TESObjectLAND* land);
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);

	void OnLightingDraw(RE::BSRenderPass* a_pass);
	void CloseLandscapeRun();
	void InstallLandscapeTimingHook();
	void InstallDevBenchTools();
	void InstallLandscapeSnow();
	void LandscapeSnowSetup(RE::BSRenderPass* a_pass, bool a_lighting);
	void LandscapeSnowRestore();
	void NoteMainViewport();
	bool LandscapeSnowRefresh();
	void PlaceShellFields(ShellCB& a_cb);
	static constexpr uint32_t kShellSurfaceSRVs = 59;
	void LandscapeSurfaceSRVs(ID3D11ShaderResourceView** a_srvs);
	bool LandscapeShadeReady();
	bool LandBlendActive() const;
	float LandBlendDepthStart() const;
	float LandBlendDepth(float a_depth, float a_snowWeight) const;
	float2 LandBlendContest() const;
	uint32_t DrawLandscapeSnowPrepass(ID3D11DeviceContext* a_context, const ShellCB& a_cb, ID3D11Buffer* a_shellCB);
	std::string LandscapeSnowStatus() const;
	json LandscapeSnowTool(const json& a_args);
	static constexpr float kLandBlendDepthStart = 0.50f;
	bool landRecolor = true;
	static constexpr float kLandRecolorLumLo = 0.48f;
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
	json DevBenchShellArgs(const json& a_args);
	json DevBenchVolumeArgs(const json& a_args);
	json DevBenchReaderGatesArgs(const json& a_args);
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

	struct CoatAging
	{
		float heat = 0.0f;
		float heatDistance = -1.0f;
		float heatRadius = 0.0f;
		bool interior = false;
		bool interiorSnow = false;
		const char* interiorCold = nullptr;
		float rain = 0.0f;
		float snowfall = 0.0f;
		float temperature = 0.0f;
		float warm = 0.0f;
		float wetness = 0.0f;
		float stickiness = 0.0f;
		float speed = 0.0f;
		float jolt = 0.0f;
		float melt = 0.0f;
		float shake = 0.0f;
		float growth = 0.0f;
		float clearClock = 0.0f;
		float heatClock = 0.0f;
		float wind = 0.0f;
		float fire = 0.0f;
		float fireHit = -1.0f;
		float speedEnergy = 0.0f;
		float landingEnergy = 0.0f;
		float knockEnergy = 0.0f;
		double trackTime = -1.0;
	};
	CoatAging ComputeCoatAging(RE::Actor* a_actor) const;
	std::vector<std::string> CoatAgingLines() const;
	SnowCondition SnowConditionAtHeight(float a_z) const;
	uint32_t stampGatherFrame = UINT32_MAX;

public:

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
		uint32_t eyeShapes = 0;
		uint32_t fx = 0;
		uint32_t decals = 0;
		std::string seed;
		std::string anchor;
		uint32_t bindBones = 0;
		bool sticky = false;
		uint32_t chained = 0;
		uint32_t components = 0;
		uint32_t componentSkins = 0;
		float worstResidual = 0.0f;
		std::string worstShape;
		uint32_t outlierBones = 0;
		float rawOffsetMax = 0.0f;
		uint32_t triangles = 0;
		uint32_t gear = 0;
		uint32_t gearSkinned = 0;
		uint32_t rigidWorn = 0;
		uint32_t sheaths = 0;
		uint32_t attached = 0;
		uint32_t gearTriangles = 0;
		float gearRadiusMax = 0.0f;
		uint32_t owned = 0;
		uint32_t unowned = 0;
		RE::NiPoint3 boundsMin{ 1e30f, 1e30f, 1e30f };
		RE::NiPoint3 boundsMax{ -1e30f, -1e30f, -1e30f };
		uint32_t outside = 0;
		int slot = -1;
		uint32_t halves = 0;
		uint32_t records = 0;
		uint32_t firstSkinned = 0;
		uint32_t firstChained = 0;
		float firstResidual = 0.0f;
		uint32_t firstOwned = 0;
		uint32_t firstUnowned = 0;
	};
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
	static constexpr uint32_t kBodyCoatFlagFlatPaint = 1;
	static constexpr uint32_t kBodyCoatFlagLiving = 2;
	static constexpr uint32_t kBodyCoatFlagHairSpecks = 4;
	static constexpr uint32_t kBodyCoatFlagRelief = 8;
	static constexpr uint32_t kBodyCoatFlagReliefView = 16;
	static constexpr uint32_t kBodyCoatFlagHairBeads = 64;
	static constexpr uint32_t kBodyCoatFlagLumpDepth = 32;
	static constexpr uint32_t kBodyCoatFlagLumpBlend = 128;
	struct BodyCoatStore;
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
	void TickBodyCoat();
	void SetBodyCoatBit(RE::BSRenderPass* a_pass);
	[[nodiscard]] bool BodyCoatPixelKey(uint32_t a_rawTechnique) const;
	void NoteBodyCoatShaders(uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor);
	virtual void GenerateShaderPermutations(RE::BSShader* a_shader) override;
	void ProbeBodyCoatDraw(RE::BSRenderPass* a_pass);
	std::vector<std::string> BodyCoatProbeLines() const;
	std::string BodyCoatWriterLine() const;
	std::vector<std::string> BodyCoatFadeLines() const;
	std::vector<std::string> BodyCoatFallLines() const;
	static bool IsEngineDecalName(const char* a_name);
	bool debugBodyCoatCensus = false;
	bool debugBodyCoatCensusDump = false;
	bool debugBodyCoatDrawProbe = false;
	int debugBodyCoatView = 0;
	int debugBodyCoatForce = 0;
	bool debugBodyCoatContact = true;
	bool debugBodyCoatClear = false;
	bool debugBodyCoatFade = true;
	float debugBodyCoatShakeTime = 10.0f;
	bool debugBodyCoatFall = true;
	float debugBodyCoatWindTilt = 1.0f;
	float debugBodyCoatContactCap = 0.5f;
	float debugBodyCoatBuildUp = 10.0f;
	float debugBodyCoatImpactShare = 0.5f;
	float debugBodyCoatLumps = 1.0f;
	float debugBodyCoatGrains = 1.0f;
	float debugBodyCoatHairAmount = 0.10f;
	float debugBodyCoatSpeckMin = 1.0f;
	float debugBodyCoatSpeckMax = 1.5f;
	int debugBodyCoatLook = 0;
	float debugBodyCoatRepose = 85.0f;
	bool debugBodyCoatHairSpecks = true;
	bool debugBodyCoatRelief = true;
	bool debugBodyCoatReliefView = false;
	int debugBodyCoatHairShape = 0;
	int debugBodyCoatLumpLook = 2;
	static constexpr uint32_t kBodyCoatCensusFrames = 120;
	static constexpr uint32_t kBodyCoatDrawWindow = 300;
	static constexpr UINT kBodyCoatAtlasSlot = 41;
	static constexpr UINT kBodyCoatLumpSlot = 44;
	static constexpr UINT kBodyCoatRecordSlot = 8;
	static constexpr uint32_t kBodyCoatRecordStride = 256;
	static constexpr uint32_t kBodyCoatRecordsMax = 1024;
	static constexpr uint32_t kBodyCoatSlots = 64;
	static constexpr uint32_t kBodyCoatSlotsPerRow = 32;
	static constexpr uint32_t kBodyCoatSlotDim[3] = { 32, 16, 48 };
	static constexpr float kBodyCoatVoxel = 2.5f;

protected:
	void RunBodyCoatCensus(bool a_dump);
	void TickBodyCoatStore();
	void EnsureBodyCoatLumps();
	winrt::com_ptr<ID3D11ShaderResourceView> bodyCoatLumpSRV;
	bool bodyCoatLumpAttempted = false;
	bool BodyCoatWriteBegin(bool a_coatOnly = false);
	bool BodyCoatWriteDue(uint32_t a_formID, float4& a_write, float4& a_top) const;
	bool BodyCoatWriteShape(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, const float4& a_write);
	void BodyCoatWriteBind(ID3D11DeviceContext* a_context, bool a_on);
	void BodyCoatTopBind(ID3D11DeviceContext* a_context, bool a_on);
	[[nodiscard]] bool BodyCoatFallDue() const;
	[[nodiscard]] const std::vector<ContactProp>& BodyCoatCoatOnly() const;
	void BodyCoatWriteResolve(ID3D11DeviceContext* a_context, uint32_t a_triangles, uint32_t a_actors);
	void PrimeBodyCoatWriter();
	std::shared_ptr<BodyCoatStore> bodyCoatStore;
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
	void RunShaderPrime();
	std::atomic<int> snowPrimeState = 0;
	std::atomic<int> snowPrimePhase = 0;
	bool SnowShadersPending(int a_group) const
	{
		return snowPrimeState.load(std::memory_order_acquire) == 1 &&
		       snowPrimePhase.load(std::memory_order_acquire) < a_group;
	}
	std::thread snowPrimeThread;

	ID3D11DeviceChild* CompileSnowShader(const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines, const char* a_target, const char* a_entry = "main");
	std::string ShaderSourcesFingerprint();
	winrt::com_ptr<ID3DBlob> ShaderCacheLoad(const std::string& a_key, const std::string& a_file);
	void ShaderCacheStore(const std::string& a_key, const std::string& a_file, ID3DBlob* a_blob);
	static uint64_t ShaderKeyHash(std::string_view a_text);
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
	static constexpr uint32_t kPrintSpellCone = 1u;
	static constexpr uint32_t kPrintSpellBowl = 2u;
	static constexpr uint32_t kPrintSpellRay = 4u;
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
	std::vector<PrintSpellMark> printSpellMarks;
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
	void QueueObjectSpellMark(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_radius, float a_depth, float a_rate, uint32_t a_flags, float a_hold = 0.0f, float a_speed = 0.0f, float a_seed = 0.0f);
	void ReplayHeldObjectSpellMarks(float a_deltaTime);
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

	enum class SpellMark
	{
		Melt,
		Carve,
		Pit,
		Crust
	};

	struct SpellEmitter
	{
		float2 position{};
		float2 previous{};
		float radius = 0.0f;
		float strength = 0.0f;
		float rate = 0.0f;
		SpellElement element = SpellElement::None;
		SpellMark mark = SpellMark::Melt;
		float pitScale = 1.0f;
		float rateScale = 1.0f;
		bool bowl = false;
		bool cone = false;
		float coneLength = 0.0f;
		float coneSeed = 0.0f;
	};

	std::vector<SpellEmitter> spellEmitters;
	void QueueObjectSpellMarkFor(const RE::NiPoint3& a_a, const RE::NiPoint3& a_b, float a_radius, SpellMark a_mark, float a_strength, float a_rate, float a_pitScale, uint32_t a_flags, float a_hold = 0.0f, float a_speed = 0.0f, float a_seed = 0.0f);

	std::unordered_map<uint32_t, float2> spellPrevPositions;

	std::unordered_map<uint32_t, float2> spellTrailPrev;

	static SpellElement ClassifyElement(const RE::EffectSetting* a_effect);

	static SpellMark MarkForElement(SpellElement a_element);

	void GatherSpellEmitters();

	float CrustBreakForce(float a_radius) const;

	bool ActorIsFloating(RE::Actor* a_actor, RE::NiAVObject* a_root, const StampBones* a_bones, bool a_useFeet, float a_groundZ, float* a_gapOut = nullptr, bool* a_byFeetOut = nullptr) const;

	bool ActorIsIncorporeal(RE::Actor* a_actor, RE::NiAVObject* a_root, StampBones* a_bones,
		float* a_alphaOut = nullptr, bool* a_flagOut = nullptr) const;
	static bool IsProxyOrOverlayGeometry(const RE::BSGeometry* a_geometry, const RE::BSShaderProperty* a_property);

	void LiftRefOntoSnow(RE::TESObjectREFR* a_ref, float a_lift, float a_minUpZ = -2.0f);
	struct PendingLift
	{
		RE::ObjectRefHandle handle;
		float lift = 0.0f;
		float minUpZ = -2.0f;
		uint32_t waited = 0;
	};
	std::mutex spellLiftLock;
	std::vector<PendingLift> spellLiftQueue;

	float SnowLiftFor(const RE::NiPoint3& a_position, float a_fraction, float a_clearance, RE::TES* a_tes);

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
		RE::NiAVObject* offsetRoot = nullptr;
		bool offsetApplied = false;
		RE::NiPoint3 childOffset;
		float appliedLift = 0.0f;
		bool undersideValid = false;
		float undersideOffset = 0.0f;
		float lyingHeight = 0.0f;
		std::vector<std::pair<RE::NiAVObject*, RE::NiTransform>> lastWorlds;
		uint32_t lastWorldFrame = 0;
		uint32_t restPasses = 0;
		bool restValid = false;
		float restRise = 0.0f;
		float restDepth = 0.0f;
		float restCarve = 0.0f;
		RE::NiPoint3 restPos;
		float carveAround = 0.0f;
		bool buried = false;
		bool wasAsleep = false;
		bool claimed = false;
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
	std::unordered_map<uint32_t, ItemSinkCandidate> itemSinkCandidates;
	std::atomic<bool> itemSinkCandidatesStale{ false };
	std::mutex itemSinkLock;
	std::unordered_map<uint32_t, ItemSinkState> itemSinkStates;
	std::unordered_map<uint32_t, ItemSinkRecord> itemSinkLoaded;
	static constexpr float kItemSinkLoadGrace = 60.0f;
	std::chrono::steady_clock::time_point itemSinkLoadedAt{};
	std::unordered_set<uint32_t> itemSinkFormsLogged;
	std::string itemSinkReadout;
	std::atomic<bool> itemSinkReadoutWanted{ false };
	std::atomic<uint32_t> itemSinkHeld{ 0 };
	std::atomic<uint32_t> itemSinkWatched{ 0 };
	uint32_t itemSinkClaimsLogged = 0;
	void ItemSinkNote(RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_position);
	bool ItemSinkWantsPrint(RE::TESObjectREFR* a_ref);
	void SaveItemSink(const SKSE::SerializationInterface* a_intfc);
	void LoadItemSink(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length);
	void RegisterItemSinkCoSave();

	std::unordered_set<uint32_t> liftedRefs;

	struct CreatureBuild
	{
		const RE::NiAVObject* root = nullptr;
		float scale = 0.0f;
		float volume = 0.0f;
		float belly = 0.0f;
		float height = 0.0f;
		float largest = 0.0f;
		float lowestRel = 0.0f;
		uint32_t capsules = 0;
		uint32_t simulated = 0;
		uint32_t reads = 0;
		uint32_t nextFrame = 0;
	};
	struct CreatureLift
	{
		const RE::NiAVObject* root = nullptr;
		RE::NiAVObject* node = nullptr;
		CreatureBuild build;
		float lift = 0.0f;
		float inBones = 0.0f;
		float settleFrom = -1.0f;
		float clearance = FLT_MAX;
		float floorShare = 1.0f;
		bool havokFollows = true;
		bool havokKnown = false;
		bool dead = false;
		uint32_t seenFrame = 0;
		float weight = 0.0f;
		float byWeight = 0.0f;
		float depth = 0.0f;
		float density = 0.0f;
		float support = 0.0f;
		float target = 0.0f;
		const char* gate = "";
	};
	std::unordered_map<uint32_t, CreatureLift> creatureLifts;
	CreatureBuild playerBuild;
	struct CreatureLiftDrawn
	{
		float inBones;
		bool havokFollows;
		float weight;
	};
	std::unordered_map<uint32_t, CreatureLiftDrawn> creatureLiftsDrawn;
	std::mutex creatureLiftLock;
	std::unordered_set<uint32_t> creatureRacesLogged;
	std::string creatureSinkReadout;
	std::atomic<bool> creatureSinkReadoutWanted{ false };
	std::atomic<uint32_t> creatureSinkLifted{ 0 };
	std::atomic<uint32_t> creatureSinkWatched{ 0 };
	std::atomic<float> creatureSinkMicros{ 0.0f };
	bool MeasureCreatureBuild(RE::NiAVObject* a_root, CreatureBuild& a_build, bool a_upright, uint32_t a_frame, float a_baseZ);
	float CreatureLiftOf(uint32_t a_formID, bool* a_havokFollows = nullptr, float* a_weight = nullptr);

	void ConsiderHazard(RE::TESObjectREFR* a_ref);

	struct PendingBlast
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 direction{};
		float heightAboveLand = 0.0f;
		float radius = 0.0f;
		float pitScale = 1.0f;
		float landingReach = 0.0f;
		SpellElement element = SpellElement::None;
		bool proximity = false;
		bool triggered = false;
	};
	std::unordered_map<uint32_t, PendingBlast> projectileBlasts;

	std::unordered_set<uint32_t> hitscanBlasted;

	void OpenProjectileBlast(const PendingBlast& a_blast, RE::TES* a_tes);

	struct ActiveBlast
	{
		float2 position{};
		float radius = 0.0f;
		float strength = 0.0f;
		float rate = 0.0f;
		float remaining = 0.0f;
		float age = 0.0f;
		float pitScale = 1.0f;
		bool diag = false;
		SpellElement element = SpellElement::None;
		SpellMark mark = SpellMark::Melt;
		bool cone = false;
		float2 apex{};
		float2 coneDir{};
		float coneLength = 0.0f;
		float coneSpeed = 0.0f;
		float coneSeed = 0.0f;
	};
	std::vector<ActiveBlast> activeBlasts;

	class SpellCastSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event,
			RE::BSTEventSource<RE::TESSpellCastEvent>* a_source) override;
	};
	SpellCastSink spellCastSink;
	bool spellCastSinkRegistered = false;

	class DeathSink : public RE::BSTEventSink<RE::TESDeathEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* a_event,
			RE::BSTEventSource<RE::TESDeathEvent>* a_source) override;
	};
	DeathSink deathSink;

	struct QueuedCone
	{
		RE::NiPoint3 position{};
		RE::NiPoint3 direction{};
		float length = 0.0f;
		float speed = 0.0f;
		float strength = 1.0f;
		SpellElement element = SpellElement::None;
	};
	std::vector<QueuedCone> queuedCones;

	void OpenShoutCone(const QueuedCone& a_cone, RE::TES* a_tes);

	struct DashWatch
	{
		RE::ActorHandle actor;
		float remaining = 0.0f;
		float2 previous{};
		bool hasPrevious = false;
	};
	std::vector<DashWatch> queuedDashes;
	std::vector<DashWatch> dashWatches;

	void GatherDashGouges(float a_deltaTime, const RE::NiPoint3& a_cameraPosition, float a_cullRadius);

	static bool SpellShoves(const RE::MagicItem* a_spell, float& a_force);

	bool ShoutLaysCone(const RE::MagicItem* a_spell) const;

	static bool IsTravellingShove(const RE::BGSProjectile* a_projectile, float a_force);

	void ConsiderShout(const RE::SpellItem* a_spell, RE::TESObjectREFR* a_caster);

	struct QueuedDeath
	{
		uint32_t formID = 0;
		RE::NiPoint3 position{};
		SpellElement element = SpellElement::None;
		RE::ActorHandle actor;
	};
	std::vector<QueuedDeath> queuedDeaths;

	class MagicApplySink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent* a_event,
			RE::BSTEventSource<RE::TESMagicEffectApplyEvent>* a_source) override;
	};
	MagicApplySink magicApplySink;

	struct QueuedHit
	{
		uint32_t formID = 0;
		SpellElement element = SpellElement::None;
	};
	std::vector<QueuedHit> queuedHits;

	struct ElementalHit
	{
		SpellElement element = SpellElement::None;
		float remaining = 0.0f;
	};
	std::unordered_map<uint32_t, ElementalHit> lastElementalHit;

	void RegisterSpellCastSink();
	float spellGameHours = -1.0f;
	float spellGameDrift = 0.0f;
	struct PendingDeathBlast
	{
		uint32_t formID = 0;
		SpellElement element = SpellElement::None;
		RE::NiPoint3 position{};
		float fuse = 0.0f;
		RE::ActorHandle actor;
	};
	std::vector<PendingDeathBlast> pendingDeathBlasts;

	struct QueuedCast
	{
		RE::NiPoint3 position{};
		float radius = 0.0f;
		SpellElement element = SpellElement::None;
	};
	std::vector<QueuedCast> queuedCasts;

	struct CloakState
	{
		RE::ActorHandle actor;
		SpellElement element = SpellElement::None;
		float rateScale = 1.0f;
		float remaining = 0.0f;
		float strikeTimer = 0.0f;
		uint32_t strikeSeed = 0;
		float reachOverride = 0.0f;
		float reachScale = 1.0f;
		bool innate = false;
		bool blasted = false;
		float burnRemaining = 0.0f;
		RE::NiPoint3 lastPosition{};
		float unseenFor = 0.0f;
	};
	std::vector<CloakState> queuedCloaks;
	std::unordered_map<uint32_t, CloakState> activeCloaks;

	struct InnateAuraRecord
	{
		SpellElement element = SpellElement::None;
		float rateScale = 1.0f;
		float reachOverride = 0.0f;
	};
	std::unordered_map<uint32_t, InnateAuraRecord> innateAuraByRace;
	std::unordered_map<uint32_t, InnateAuraRecord> innateAuraByBase;
	std::unordered_map<uint32_t, CloakState> innateAuras;

	std::unordered_map<uint32_t, CloakState> corpseEffects;

	void ConsiderCorpseEffect(RE::Actor* a_actor, float a_deltaTime);

	const InnateAuraRecord* ResolveInnateAura(RE::Actor* a_actor);

	static bool AuraFromActorRecords(RE::Actor* a_actor, InnateAuraRecord& a_out);
	static bool AuraFromSpellList(const RE::TESSpellList* a_list, InnateAuraRecord& a_out);

	void GatherActorMarks(float a_deltaTime, const RE::NiPoint3& a_cameraPosition, float a_cullRadius);

	void OpenInnateDeathBlast(CloakState& a_state, const RE::NiPoint3& a_position);
	std::unordered_map<uint32_t, float2> spellAuraPrev;
	std::unordered_map<uint32_t, float2> currentAuraPositions;

	void ConsiderActorAuras(RE::Actor* a_actor, CloakState& a_cloak, float a_deltaTime);

	std::mutex queuedCastLock;

	struct SpellStats
	{
		uint projectiles = 0;
		uint streams = 0;
		uint hazards = 0;
		uint detonations = 0;
		uint armed = 0;
		uint trails = 0;
		uint casts = 0;
		uint shouts = 0;
		uint shoutDiscs = 0;
		uint dashWatches = 0;
		uint dashGouges = 0;
		uint forceTracks = 0;
		uint lifted = 0;
		uint lastShoutElement = 0;
		float lastShoutSpeed = 0.0f;
		float lastShoutForce = 0.0f;
		uint lastShoutVerdict = 0;
		uint auras = 0;
		uint innate = 0;
		uint burning = 0;
		uint emittersCulled = 0;
		uint deathsSeen = 0;
		uint deathBlasts = 0;
		uint corpses = 0;
		uint rejectedElement = 0;
		uint rejectedNoBlast = 0;
		uint groundContacts = 0;
		uint emitters = 0;
		uint pending = 0;
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
