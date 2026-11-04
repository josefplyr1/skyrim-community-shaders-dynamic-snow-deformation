// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "SnowDeformation.h"

#include "Globals.h"
#include "Menu.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <DirectXPackedVector.h>
#include <dxgi1_4.h>

#define SNOWDEF_SETTINGS_FIELDS(X) \
	X(EnableSnowDeformation) \
	X(SlumpRate) \
	X(RefillRateMultiplier) \
	X(RefillOnlyWhenSnowing) \
	X(PersistTrenches) \
	X(StoredTrenchFadeDays) \
	X(TrenchMemoryMB) \
	X(EnableSnowAccumulation) \
	X(PersistAccumulation) \
	X(RecoloredSnowAccumulates) \
	X(SteepFaceThinning) \
	X(AccumulationPeak) \
	X(AccumulationHours) \
	X(AccumulationMeltHours) \
	X(AccumulationFadeDays) \
	X(MeltPersistence) \
	X(MeltBowlFloor) \
	X(MeltEdgeIrregularity) \
	X(EnableSpellIntegration) \
	X(SpellMeltRate) \
	X(BlastRadiusScale) \
	X(CloakRadius) \
	X(PitDepth) \
	X(PitRadius) \
	X(ShockCloakRadius) \
	X(ShockCloakInterval) \
	X(ShockCloakStrikeScale) \
	X(CrustRate) \
	X(CrustRadius) \
	X(CrustPrintDepth) \
	X(CrustBreakRadius) \
	X(CrustThawMinutes) \
	X(CrustBreakOnCarve) \
	X(ScorchStrength) \
	X(NoCarveFloatingActors) \
	X(FloatingActorBand) \
	X(IncorporealMode) \
	X(CorpseElementalMarks) \
	X(CorpseEffectSeconds) \
	X(EnableShoutCones) \
	X(ShoutMarkReach) \
	X(ShoutMarkWidth) \
	X(ShoutMarkWaist) \
	X(ShoutMarkRoundness) \
	X(ShoutMarkFadeStart) \
	X(ShoutMarkFarDepth) \
	X(ShoutChurn) \
	X(ShoutStreakSize) \
	X(ShoutStreakLength) \
	X(ForceCarveDepth) \
	X(DashGougeScale) \
	X(ForceTrackWidth) \
	X(ForceTrackDepth) \
	X(LiftFrostEffects) \
	X(LiftRunes) \
	X(DecalsOnObjectSnow) \
	X(RuneDecalsOnSnow) \
	X(FrostDecalsOnSnow) \
	X(ItemEmbedPercent) 	X(ItemSink) 	X(ItemMinRoundness) \
	X(ItemSnowGripPercent) \
	X(CreatureSink) \
	X(EnableLightningArcs) \
	X(LightningArcWidth) \
	X(LightningArcBrightness) \
	X(LightningArcLife) \
	X(LightningArcTint) \
	X(AtronachFireReach) \
	X(AtronachFireDeathRadius) \
	X(AtronachFireBurnSeconds) \
	X(AtronachFrostReach) \
	X(AtronachFrostDeathRadius) \
	X(AtronachShockReach) \
	X(AtronachShockDeathRadius) \
	X(SnowClassDepths) \
	X(TextureDepths) \
	X(ObjectRasterReachM) \
	X(SkinEdgeFlankWidth) \
	X(RoadMeshesDepth) \
	X(SnowTexturePath) \
	X(SnowPBRConfig) \
	X(TrampleZoneScale) \
	X(TrampleZoneHeight) \
	X(UndulationStrength) \
	X(UndulationSpacing) \
	X(Tessellation) \
	X(ParallaxShadowStrength) \
	X(ParallaxDepth) \
	X(BermHeight) \
	X(BermSteepness) \
	X(SnowResistance) \
	X(FreshSnowSoftness) \
	X(SnowClimateStrength) \
	X(TrackAgeing) \
	X(WetSnowAndIce) \
	X(FreshSnowSettleHours) \
	X(TrampledFlattening) \
	X(RimLip) \
	X(RimTeeth) \
	X(TrenchDetailRelief) \
	X(TrenchDetailRoughness) \
	X(TrenchDetailCoverage) \
	X(ImpactReach) \
	X(SpeedSpoil) \
	X(SpeedSpoilSize) \
	X(ShellHorizonMarch) \
	X(SnowGlints) \
	X(SnowScreenSpaceShadows) \
	X(SnowCastsShadows) \
	X(ObjectSnowNearDetail) \
	X(CameraAboveSnow) \
	X(BloodOnSnow) \
	X(BloodIntensity) \
	X(BloodBurial) \
	X(BloodAgeHours) \
	X(BloodSheen) \
	X(BloodSpreadSeconds) \
	X(DeformMapResolution) \
	X(VRAMAwareTrenchMap) \
	X(ObjectTrenchResolution) \
	X(ObjectPrintRimWidth) \
	X(RangeTrenchesM) \
	X(RangeActorTrenchesM) \
	X(RangeSkinsM) \
	X(SkinDepthBias) \
	X(SkinSlopeDepthBias) \
	X(SkinTessCapPx) \
	X(SlopeDrape) \
	X(CityShells) \
	X(LODSnowSensitivity) \
	X(HorizonSnow) \
	X(ProjSnowMatch) \
	X(SeasonsSnowMaxAngle) \
	X(MultipassSnowFollowsPaint) \
	X(ShelterMaxHeight) \
	X(ShelterDriftAngle) \
	X(ObjectMeetBand) \
	X(LODObjectSnow) \
	X(SnowTexturedRecolor) \
	X(ProjSnowNormals) \
	X(UnprojectedShapesBare) \
	X(SnowFollowsParallax) \
	X(FlattenParallaxUnderSnow) \
	X(VolumeSnow) \
	X(VolumeSnowDepth) \
	X(VolumeSnowCoverage) \
	X(VolumeVoxelSize) \
	X(VolumeSnowMaxSlopeDeg) \
	X(VolumeSkyExposurePct) \
	X(VolumeSnowOverhang) \
	X(VolumeSnowRounding) \
	X(VolumeLevels) \
	X(VolumeFineLevels) \
	X(VolumeDirtyBricks) \
	X(VolumeStaggeredCapture) \
	X(VolumeForwardBias) \
	X(VolumeVerticalBias) \
	X(VolumeMaxDistance) \
	X(VolumeRingShape) \
	X(VolumeMarchStep) \
	X(VolumeDetailDistance) \
	X(VolumeSkipEmptyCells) \

void to_json(nlohmann::json& j, const SnowDeformation::Settings& s)
{
#define X(field) j[#field] = s.field;
	SNOWDEF_SETTINGS_FIELDS(X)
#undef X
}

void from_json(const nlohmann::json& j, SnowDeformation::Settings& s)
{
	const SnowDeformation::Settings defaults{};
#define X(field) s.field = j.value(#field, defaults.field);
	SNOWDEF_SETTINGS_FIELDS(X)
#undef X
}

Texture2D* SD_MakeTexture2D(UINT a_width, UINT a_height, DXGI_FORMAT a_format, const char* a_name, UINT a_bind)
{
	D3D11_TEXTURE2D_DESC desc = {
		.Width = a_width,
		.Height = a_height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = a_format,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = a_bind
	};
	auto* texture = new Texture2D(desc, a_name);
	if (a_bind & D3D11_BIND_SHADER_RESOURCE)
		texture->CreateSRV({ .Format = a_format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
	if (a_bind & D3D11_BIND_UNORDERED_ACCESS)
		texture->CreateUAV({ .Format = a_format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
	if (a_bind & D3D11_BIND_RENDER_TARGET)
		texture->CreateRTV({ .Format = a_format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
	return texture;
}

void SnowDeformation::CreateDeformationTextures()
{
	LoadTraceScope _loadTrace(this, "CreateDeformationTextures");
	for (uint i = 0; i < 2; i++) {
		delete deformationTextures[i];
		deformationTextures[i] = nullptr;
	}

	deformationTextures[0] = SD_MakeTexture2D(deformMapDim, deformMapDim, DXGI_FORMAT_R16G16B16A16_FLOAT, "SnowDeformation::DeformationMap");
	deformationTextures[1] = SD_MakeTexture2D(deformMapDim, deformMapDim, DXGI_FORMAT_R16G16_FLOAT, "SnowDeformation::EvolveScratch");

	if (!CreateTrenchStoreResources()) {
		logger::warn("[SNOW DEFORMATION] Trench store resources failed; trenches will not survive leaving the window");
	}

	delete bermFieldTexture;
	bermFieldTexture = SD_MakeTexture2D(deformMapDim, deformMapDim, DXGI_FORMAT_R16G16B16A16_FLOAT, "SnowDeformation::BermField");

	{
		delete passStateTexture;
		passStateTexture = SD_MakeTexture2D(deformMapDim, deformMapDim, DXGI_FORMAT_R16_UINT, "SnowDeformation::PassState", D3D11_BIND_UNORDERED_ACCESS);
		const UINT zero[4] = { 0, 0, 0, 0 };
		globals::d3d::context->ClearUnorderedAccessViewUint(passStateTexture->uav.get(), zero);
	}

	const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	delete bodyReachTexture;
	bodyReachTexture = SD_MakeTexture2D(deformMapDim, deformMapDim, DXGI_FORMAT_R8_UNORM, "SnowDeformation::BodyReach");
	globals::d3d::context->ClearUnorderedAccessViewFloat(bodyReachTexture->uav.get(), zero);

	delete trackClockTexture;
	trackClockTexture = SD_MakeTexture2D(deformMapDim, deformMapDim, DXGI_FORMAT_R16_FLOAT, "SnowDeformation::TrackClock");
	globals::d3d::context->ClearUnorderedAccessViewFloat(trackClockTexture->uav.get(), zero);
	snowClimate.ageingRebase = 0.0f;

	{
		auto device = globals::d3d::device;
		const uint tiles = deformMapDim / 8;

		occupancyTexture = nullptr;
		occupancyUAV = nullptr;
		occupancySRV = nullptr;
		D3D11_TEXTURE2D_DESC occDesc{};
		occDesc.Width = tiles;
		occDesc.Height = tiles;
		occDesc.MipLevels = 1;
		occDesc.ArraySize = 1;
		occDesc.Format = DXGI_FORMAT_R8_UINT;
		occDesc.SampleDesc.Count = 1;
		occDesc.Usage = D3D11_USAGE_DEFAULT;
		occDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		DX::ThrowIfFailed(device->CreateTexture2D(&occDesc, nullptr, occupancyTexture.put()));
		Util::SetResourceName(occupancyTexture.get(), "SnowDeformation::TileOccupancy");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(occupancyTexture.get(), nullptr, occupancyUAV.put()));
		Util::SetResourceName(occupancyUAV.get(), "SnowDeformation::TileOccupancy UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(occupancyTexture.get(), nullptr, occupancySRV.put()));
		Util::SetResourceName(occupancySRV.get(), "SnowDeformation::TileOccupancy SRV");

		evolveTileBuffer = nullptr;
		evolveTileUAV = nullptr;
		evolveTileSRV = nullptr;
		D3D11_BUFFER_DESC listDesc{};
		listDesc.ByteWidth = (tiles * tiles + 1) * sizeof(uint32_t);
		listDesc.Usage = D3D11_USAGE_DEFAULT;
		listDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		listDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		listDesc.StructureByteStride = sizeof(uint32_t);
		DX::ThrowIfFailed(device->CreateBuffer(&listDesc, nullptr, evolveTileBuffer.put()));
		Util::SetResourceName(evolveTileBuffer.get(), "SnowDeformation::EvolveTiles");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(evolveTileBuffer.get(), nullptr, evolveTileUAV.put()));
		Util::SetResourceName(evolveTileUAV.get(), "SnowDeformation::EvolveTiles UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(evolveTileBuffer.get(), nullptr, evolveTileSRV.put()));
		Util::SetResourceName(evolveTileSRV.get(), "SnowDeformation::EvolveTiles SRV");

		snapshotTileBuffer = nullptr;
		snapshotTileUAV = nullptr;
		snapshotTileSRV = nullptr;
		DX::ThrowIfFailed(device->CreateBuffer(&listDesc, nullptr, snapshotTileBuffer.put()));
		Util::SetResourceName(snapshotTileBuffer.get(), "SnowDeformation::SnapshotTiles");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(snapshotTileBuffer.get(), nullptr, snapshotTileUAV.put()));
		Util::SetResourceName(snapshotTileUAV.get(), "SnowDeformation::SnapshotTiles UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(snapshotTileBuffer.get(), nullptr, snapshotTileSRV.put()));
		Util::SetResourceName(snapshotTileSRV.get(), "SnowDeformation::SnapshotTiles SRV");

		bermDirtyTexture = nullptr;
		bermDirtyUAV = nullptr;
		bermDirtySRV = nullptr;
		DX::ThrowIfFailed(device->CreateTexture2D(&occDesc, nullptr, bermDirtyTexture.put()));
		Util::SetResourceName(bermDirtyTexture.get(), "SnowDeformation::BermDirty");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(bermDirtyTexture.get(), nullptr, bermDirtyUAV.put()));
		Util::SetResourceName(bermDirtyUAV.get(), "SnowDeformation::BermDirty UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(bermDirtyTexture.get(), nullptr, bermDirtySRV.put()));
		Util::SetResourceName(bermDirtySRV.get(), "SnowDeformation::BermDirty SRV");

		bermTileBuffer = nullptr;
		bermTileUAV = nullptr;
		bermTileSRV = nullptr;
		DX::ThrowIfFailed(device->CreateBuffer(&listDesc, nullptr, bermTileBuffer.put()));
		Util::SetResourceName(bermTileBuffer.get(), "SnowDeformation::BermTiles");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(bermTileBuffer.get(), nullptr, bermTileUAV.put()));
		Util::SetResourceName(bermTileUAV.get(), "SnowDeformation::BermTiles UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(bermTileBuffer.get(), nullptr, bermTileSRV.put()));
		Util::SetResourceName(bermTileSRV.get(), "SnowDeformation::BermTiles SRV");

		bermOccupancyTexture = nullptr;
		bermOccupancyUAV = nullptr;
		bermOccupancySRV = nullptr;
		DX::ThrowIfFailed(device->CreateTexture2D(&occDesc, nullptr, bermOccupancyTexture.put()));
		Util::SetResourceName(bermOccupancyTexture.get(), "SnowDeformation::BermOccupancy");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(bermOccupancyTexture.get(), nullptr, bermOccupancyUAV.put()));
		Util::SetResourceName(bermOccupancyUAV.get(), "SnowDeformation::BermOccupancy UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(bermOccupancyTexture.get(), nullptr, bermOccupancySRV.put()));
		Util::SetResourceName(bermOccupancySRV.get(), "SnowDeformation::BermOccupancy SRV");

		untouchedTilesTexture = nullptr;
		untouchedTilesUAV = nullptr;
		untouchedTilesSRV = nullptr;
		DX::ThrowIfFailed(device->CreateTexture2D(&occDesc, nullptr, untouchedTilesTexture.put()));
		Util::SetResourceName(untouchedTilesTexture.get(), "SnowDeformation::UntouchedTiles");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(untouchedTilesTexture.get(), nullptr, untouchedTilesUAV.put()));
		Util::SetResourceName(untouchedTilesUAV.get(), "SnowDeformation::UntouchedTiles UAV");
		DX::ThrowIfFailed(device->CreateShaderResourceView(untouchedTilesTexture.get(), nullptr, untouchedTilesSRV.put()));
		Util::SetResourceName(untouchedTilesSRV.get(), "SnowDeformation::UntouchedTiles SRV");

		tileGridsNeedInit = true;
	}
}

void SnowDeformation::SetupResources()
{
	LoadTraceScope _loadTrace(this, "SetupResources");
	logger::info("[SNOW DEFORMATION] {}", kAttribution);

	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>(), "SnowDeformation::PerFrame");

	CreateDeformationTextures();

	{
		auto device = globals::d3d::device;
		D3D11_BUFFER_DESC flagDesc{};
		flagDesc.ByteWidth = 56;
		flagDesc.Usage = D3D11_USAGE_DEFAULT;
		flagDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		flagDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		DX::ThrowIfFailed(device->CreateBuffer(&flagDesc, nullptr, deformActivityBuffer.put()));
		Util::SetResourceName(deformActivityBuffer.get(), "SnowDeformation::DeformActivity");

		D3D11_UNORDERED_ACCESS_VIEW_DESC flagUavDesc{};
		flagUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		flagUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		flagUavDesc.Buffer.NumElements = 14;
		flagUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(deformActivityBuffer.get(), &flagUavDesc, deformActivityUAV.put()));
		Util::SetResourceName(deformActivityUAV.get(), "SnowDeformation::DeformActivity UAV");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = 56;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (uint i = 0; i < kDeformActivitySlots; i++) {
			DX::ThrowIfFailed(device->CreateBuffer(&stagingDesc, nullptr, deformActivityStaging[i].put()));
			Util::SetResourceName(deformActivityStaging[i].get(), "SnowDeformation::DeformActivityStaging");
		}

		D3D11_BUFFER_DESC tileDesc{};
		tileDesc.ByteWidth = kStampTileCap * sizeof(uint32_t);
		tileDesc.Usage = D3D11_USAGE_DYNAMIC;
		tileDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		tileDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		tileDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		tileDesc.StructureByteStride = sizeof(uint32_t);
		DX::ThrowIfFailed(device->CreateBuffer(&tileDesc, nullptr, stampTileBuffer.put()));
		Util::SetResourceName(stampTileBuffer.get(), "SnowDeformation::StampTiles");
		D3D11_SHADER_RESOURCE_VIEW_DESC tileSrvDesc{};
		tileSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
		tileSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		tileSrvDesc.Buffer.NumElements = kStampTileCap;
		DX::ThrowIfFailed(device->CreateShaderResourceView(stampTileBuffer.get(), &tileSrvDesc, stampTileSRV.put()));
		Util::SetResourceName(stampTileSRV.get(), "SnowDeformation::StampTiles SRV");

		D3D11_BUFFER_DESC argsDesc{};
		argsDesc.ByteWidth = 5 * sizeof(uint32_t);
		argsDesc.Usage = D3D11_USAGE_DEFAULT;
		argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		const uint32_t argsZero[5] = {};
		const D3D11_SUBRESOURCE_DATA argsInit{ argsZero, 0, 0 };
		DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, &argsInit, evolveArgsBuffer.put()));
		Util::SetResourceName(evolveArgsBuffer.get(), "SnowDeformation::EvolveArgs");
		D3D11_UNORDERED_ACCESS_VIEW_DESC argsUavDesc{};
		argsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		argsUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		argsUavDesc.Buffer.NumElements = 5;
		argsUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(evolveArgsBuffer.get(), &argsUavDesc, evolveArgsUAV.put()));
		Util::SetResourceName(evolveArgsUAV.get(), "SnowDeformation::EvolveArgs UAV");
		DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, &argsInit, bermArgsBuffer.put()));
		Util::SetResourceName(bermArgsBuffer.get(), "SnowDeformation::BermArgs");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(bermArgsBuffer.get(), &argsUavDesc, bermArgsUAV.put()));
		Util::SetResourceName(bermArgsUAV.get(), "SnowDeformation::BermArgs UAV");
		DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, &argsInit, snapshotArgsBuffer.put()));
		Util::SetResourceName(snapshotArgsBuffer.get(), "SnowDeformation::SnapshotArgs");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(snapshotArgsBuffer.get(), &argsUavDesc, snapshotArgsUAV.put()));
		Util::SetResourceName(snapshotArgsUAV.get(), "SnowDeformation::SnapshotArgs UAV");
	}

	exclusionFieldTexture = SD_MakeTexture2D(kExclusionFieldDim, kExclusionFieldDim, DXGI_FORMAT_R8G8_UNORM, "SnowDeformation::ExclusionField");
	exclusionFieldCB = new ConstantBuffer(ConstantBufferDesc<ExclusionFieldCB>(), "SnowDeformation::ExclusionFieldCB");

	undulationFieldTexture = SD_MakeTexture2D(kUndulationFieldDim, kUndulationFieldDim, DXGI_FORMAT_R16G16B16A16_FLOAT, "SnowDeformation::UndulationField");
	undulationFieldCB = new ConstantBuffer(ConstantBufferDesc<UndulationFieldCB>(), "SnowDeformation::UndulationFieldCB");
	undulationFieldValid = false;

	noiseHashTexture = SD_MakeTexture2D(kNoiseHashDim, kNoiseHashDim, DXGI_FORMAT_R16_UNORM, "SnowDeformation::NoiseHash");
	noiseHashBaked = false;

	shellTerrainTexture = SD_MakeTexture2D(kShellWindowDim, kShellWindowDim, DXGI_FORMAT_R32G32B32A32_FLOAT, "SnowDeformation::ShellTerrainWindow");
	shellTerrainWet = SD_MakeTexture2D(kShellWindowDim, kShellWindowDim, DXGI_FORMAT_R32G32B32A32_FLOAT, "SnowDeformation::ShellTerrainWet");
	shellTerrainBare = SD_MakeTexture2D(kShellWindowDim, kShellWindowDim, DXGI_FORMAT_R8_UNORM, "SnowDeformation::ShellTerrainBare", D3D11_BIND_SHADER_RESOURCE);

	windowFillCB = new ConstantBuffer(ConstantBufferDesc<WindowFillCB>(), "SnowDeformation::WindowFillCB");
	shellCB = new ConstantBuffer(ConstantBufferDesc<ShellCB>(), "SnowDeformation::ShellCB");
	staticsCB = new ConstantBuffer(ConstantBufferDesc<StaticsCB>(), "SnowDeformation::StaticsCB");
	smoothCB = new ConstantBuffer(ConstantBufferDesc<SmoothCB>(), "SnowDeformation::SmoothCB");
	heightProcessCB = new ConstantBuffer(ConstantBufferDesc<HeightProcessCB>(), "SnowDeformation::HeightProcessCB");
	heightGhostCB = new ConstantBuffer(ConstantBufferDesc<HeightGhostCB>(), "SnowDeformation::HeightGhostCB");
	doorsCB = new ConstantBuffer(ConstantBufferDesc<ExclusionsCB>(), "SnowDeformation::ExclusionsCB");
	exclusionListBuffer = new StructuredBuffer(StructuredBufferDesc<float4>(kMaxExclusions * 2), kMaxExclusions * 2, "SnowDeformation::ExclusionList");
	exclusionListBuffer->CreateSRV();

	CreateHeightFieldResources();

	{
		D3D11_BLEND_DESC minmaxBlendDesc{};
		minmaxBlendDesc.IndependentBlendEnable = TRUE;
		for (int i = 0; i < 3; i++) {
			minmaxBlendDesc.RenderTarget[i].BlendEnable = TRUE;
			minmaxBlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].BlendOp = i == 1 ? D3D11_BLEND_OP_MIN : D3D11_BLEND_OP_MAX;
			minmaxBlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_MAX;
			minmaxBlendDesc.RenderTarget[i].RenderTargetWriteMask = (i == 2) ?
			                                                            (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN) :
			                                                            D3D11_COLOR_WRITE_ENABLE_RED;
		}
		DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&minmaxBlendDesc, heightMaxBlendState.put()));
		Util::SetResourceName(heightMaxBlendState.get(), "SnowDeformation::HeightMinMaxBlend");

		D3D11_BLEND_DESC rasterBlendDesc{};
		rasterBlendDesc.RenderTarget[0] = minmaxBlendDesc.RenderTarget[0];
		rasterBlendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
		DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&rasterBlendDesc, heightRasterBlendState.put()));
		Util::SetResourceName(heightRasterBlendState.get(), "SnowDeformation::HeightRasterBlend");
	}

	auto device = globals::d3d::device;

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = TRUE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, shellRasterState.put()));
	Util::SetResourceName(shellRasterState.get(), "SnowDeformation::ShellRasterState");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = TRUE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, shellDepthState.put()));
	Util::SetResourceName(shellDepthState.get(), "SnowDeformation::ShellDepthState");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxAnisotropy = 8;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, shellSnowSampler.put()));
	Util::SetResourceName(shellSnowSampler.get(), "SnowDeformation::ShellSnowSampler");

	D3D11_SAMPLER_DESC linearDesc{};
	linearDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	linearDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	linearDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&linearDesc, shellLinearSampler.put()));
	Util::SetResourceName(shellLinearSampler.get(), "SnowDeformation::ShellLinearSampler");
}

SnowDeformation::SettingsGPU SnowDeformation::GetCommonBufferData(bool a_inWorld)
{
	static Util::FrameChecker frameChecker;
	if (a_inWorld && (settings.EnableSnowDeformation || debugTerrainOverlay) && frameChecker.IsNewFrame()) {
		auto eyePosFB = globals::game::frameBufferCached.GetCameraPosAdjust();
		const float deformTexel = deformWorldSize / deformMapDim;
		float2 desiredOrigin = {
			std::floor((eyePosFB.x - deformWorldSize * 0.5f) / deformTexel) * deformTexel,
			std::floor((eyePosFB.y - deformWorldSize * 0.5f) / deformTexel) * deformTexel
		};

		const int scrollX = (int)std::lround((desiredOrigin.x - windowOrigin.x) / deformTexel);
		const int scrollY = (int)std::lround((desiredOrigin.y - windowOrigin.y) / deformTexel);
		pendingScrollDelta.x += scrollX;
		pendingScrollDelta.y += scrollY;
		const int originMask = (int)deformMapDim - 1;
		mapOrigin.x = (mapOrigin.x + scrollX) & originMask;
		mapOrigin.y = (mapOrigin.y + scrollY) & originMask;
		windowOrigin = desiredOrigin;
	}

	SettingsGPU data{};
	data.WindowOrigin = windowOrigin;
	data.DeformMapOrigin = mapOrigin;
	data.InvWorldSize = 1.0f / deformWorldSize;
	data.DebugTerrainOverlay = (debugTerrainOverlay ? 1u : 0u) | (debugProjSnowView ? 4u : 0u) | (debugProjWeightView ? 32u : 0u) | (debugProjAlbedoView ? 64u : 0u) |
	                           (settings.EnableSnowDeformation ? 128u : 0u) | (LandRecolorActive() ? 256u : 0u) | (shellSnowRmaosSRV ? 512u : 0u);
	data.WaterWindowDim = 0.0f;
	if (settings.EnableSnowDeformation && waterFineTexture && waterFineTexture->srv && waterFineValid) {
		data.WaterWindowOrigin = { waterFineCenter.x - waterFineHalf, waterFineCenter.y - waterFineHalf };
		data.WaterWindowTexel = 2.0f * waterFineHalf / float(kWaterFineDim);
		data.WaterWindowDim = float(kWaterFineDim);
	}

	EnsureShellSnowTextures();
	data.LODReplaceStart = 0.0f;
	data.LODReplaceFadeInv = 1.0f / 2048.0f;
	data.LODSnowSensitivity = std::clamp(settings.LODSnowSensitivity, 0.0f, 1.0f);
	data.SnowIsLinear = shellSnowTextureIsPBR ? 1.0f : 0.0f;
	data.SnowRoughnessScale = snowRoughnessScale;
	data.LODReplaceEnable = (settings.EnableSnowDeformation && settings.HorizonSnow && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.SnowHasNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	data.ProjSnowEnable = (settings.EnableSnowDeformation && settings.ProjSnowMatch && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.ProjUnauthoredThreshold = std::cos(std::clamp(settings.SeasonsSnowMaxAngle, 0.0f, 90.0f) * (DirectX::XM_PI / 180.0f));
	data.LODObjectEnable = (settings.EnableSnowDeformation && settings.LODObjectSnow && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.SnowTexturedEnable = (settings.EnableSnowDeformation && settings.SnowTexturedRecolor && settings.ProjSnowMatch && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.ProjSnowNormalEnable = settings.ProjSnowNormals ? 1.0f : 0.0f;
	data.ParallaxFollow = settings.SnowFollowsParallax ? 1.0f : 0.0f;
	data.ParallaxFlatten = settings.FlattenParallaxUnderSnow ? 1.0f : 0.0f;
	data.SnowSpecularLevel = snowSpecularLevel;
	data.LandRecolorLumLo = kLandRecolorLumLo;
	data.LandWetPrint = std::clamp(settings.SnowClimateStrength, 0.0f, 1.0f) * GetSnowCondition().wetness;
	const float wetIce = SnowWetIceStrength();
	data.LandIce = wetIce * SnowIceShown(1);
	data.IceClock = TrackDateClock();
	for (uint32_t k = 0; k < kIceHistoryBins; ++k) {
		const auto& entry = snowClimate.iceHistory[k];
		data.IceHistory[k] = { wetIce * entry[0], wetIce * entry[1], wetIce * entry[2], 0.0f };
	}
	data.PrintFullDepth = BodyPrintScale();
	data.PrintRect = PrintRectForFrame();
	if (globals::state->isMapMenuOpen) {
		data.LODReplaceEnable = 0.0f;
		data.ProjSnowEnable = 0.0f;
		data.LODObjectEnable = 0.0f;
		data.SnowTexturedEnable = 0.0f;
		data.DebugTerrainOverlay &= ~(256u | 512u);
	}
	return data;
}

void SnowDeformation::ApplyRangeSettings()
{
	if (trenchRangeDirty || !rangeInitApplied) {
		float newWorldSize = std::clamp(settings.RangeTrenchesM, 29.0f, 200.0f) * 2.0f * kUnitsPerMeter;
		if (std::abs(newWorldSize - deformWorldSize) > 1.0f) {
			deformWorldSize = newWorldSize;
			clearRequested = true;
		}
		trenchRangeDirty = false;
	}

	if (deformMapDimDirty || !rangeInitApplied) {
		const uint desired = settings.DeformMapResolution >= 4096u ? 4096u : (settings.DeformMapResolution >= 2048u ? 2048u : 1024u);
		settings.DeformMapResolution = desired;
		const uint effective = settings.VRAMAwareTrenchMap && deformMapVramCap ? std::min(desired, deformMapVramCap) : desired;
		if (effective != deformMapDim) {
			deformMapDim = effective;
			CreateDeformationTextures();
			mapOrigin.x &= (int)deformMapDim - 1;
			mapOrigin.y &= (int)deformMapDim - 1;
			clearRequested = true;
		}
		deformMapDimDirty = false;
	}

	rangeInitApplied = true;
}

void SnowDeformation::PollDeformActivity(ID3D11DeviceContext* a_context)
{
	for (uint i = 0; i < kDeformActivitySlots; i++) {
		if (!deformActivityPending[i])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT hr = a_context->Map(deformActivityStaging[i].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
			continue;
		if (SUCCEEDED(hr)) {
			const uint* value = static_cast<const uint*>(mapped.pData);
			const uint flag = value[0];
			const uint changed = value[1];
			const uint cminX = value[2], cminY = value[3];
			const uint maxX = value[4], maxY = value[5];
			const uint countR = value[6], countG = value[7], countB = value[8];
			const uint deltaSum = value[9];
			const uint evolveFlag = value[10];
			const uint evolveTiles = value[11];
			const uint bermTiles = value[12];
			a_context->Unmap(deformActivityStaging[i].get(), 0);
			if (deformActivitySlotEvolveRan[i] && deformActivitySlotSeq[i] > evolveVerdictSeq) {
				evolveVerdictSeq = deformActivitySlotSeq[i];
				evolveFlagActive = evolveFlag != 0;
				evolveTilesLast = evolveTiles;
			}
			if (deformActivitySlotSeq[i] > deformFlagSeq) {
				deformFlagSeq = deformActivitySlotSeq[i];
				deformFlagActive = flag != 0;
				bermTilesLast = bermTiles;
				deformChangedTexels = changed;
				deformChangedDepth = countR;
				deformChangedMelt = countG;
				deformChangedCrustDep = countB;
				deformChangedDeltaSum = deltaSum;
				deformChangedMinX = 65535u - cminX;
				deformChangedMinY = 65535u - cminY;
				deformChangedMaxX = maxX;
				deformChangedMaxY = maxY;
			}
		}
		deformActivityPending[i] = false;
	}
}

void SnowDeformation::EnsureActivityViewTexture()
{
	if (activityViewTexture && activityViewDim == deformMapDim)
		return;
	auto device = globals::d3d::device;
	if (!device)
		return;

	activityViewTexture = nullptr;
	activityViewSRV = nullptr;
	activityViewUAV = nullptr;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = deformMapDim;
	desc.Height = deformMapDim;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, activityViewTexture.put())))
		return;
	Util::SetResourceName(activityViewTexture.get(), "SnowDeformation::ActivityView");
	device->CreateShaderResourceView(activityViewTexture.get(), nullptr, activityViewSRV.put());
	device->CreateUnorderedAccessView(activityViewTexture.get(), nullptr, activityViewUAV.put());
	activityViewDim = deformMapDim;
}

float SnowDeformation::GameClock::Read(std::atomic<bool>& a_unarm)
{
	elapsedHours = 0.0f;
	reversed = false;

	auto* calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	if (!calendar)
		return -1.0f;

	if (a_unarm.exchange(false, std::memory_order_acq_rel))
		lastHours = -1.0f;

	const float rawScale = calendar->GetTimescale();
	if (rawScale >= 1.0f && rawScale <= 100.0f)
		timescale = rawScale;

	const float hours = calendar->GetHoursPassed();
	if (lastHours < 0.0f) {
		lastHours = hours;
		return hours;
	}
	const float delta = hours - lastHours;
	lastHours = hours;

	if (delta < -1.0e-4f) {
		reversed = true;
		return hours;
	}
	elapsedHours = std::max(delta, 0.0f);
	return hours;
}

void SnowDeformation::TickGameClock()
{
	if (const float hours = gameClock.Read(gameClockUnarm); hours >= 0.0f)
		gameClockHours.store(hours, std::memory_order_relaxed);
}

RE::TESWorldSpace* SnowDeformation::NoSkyWorldspace()
{
	auto* tes = globals::game::tes;
	auto* worldSpace = tes && !tes->interiorCell ? tes->GetRuntimeData2().worldSpace : nullptr;
	return worldSpace && worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoSky) ? worldSpace : nullptr;
}

static bool FallsAsSnow(RE::TESWeather* a_weather)
{
	struct Verdict
	{
		const RE::BGSShaderParticleGeometryData* precipitation = nullptr;
		const char* texture = nullptr;
		bool snow = true;
	};
	static std::unordered_map<RE::FormID, Verdict> verdicts;

	auto* precipitation = a_weather->precipitationData;
	const char* texture = precipitation ? precipitation->GetRuntimeData().particleTexture.textureName.c_str() : nullptr;
	auto [it, inserted] = verdicts.try_emplace(a_weather->GetFormID());
	Verdict& verdict = it->second;
	if (!inserted && verdict.precipitation == precipitation && verdict.texture == texture)
		return verdict.snow;

	verdict.precipitation = precipitation;
	verdict.texture = texture;
	const char* veto = nullptr;
	if (precipitation) {
		std::string name;
		if (const char* edid = precipitation->GetFormEditorID())
			name = edid;
		name += '|';
		name += texture ? texture : "";
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (name.find("snow") == std::string::npos)
			veto = SnowDeformation::NotSnowKeywordIn(name.c_str());
	}
	verdict.snow = veto == nullptr;
	logger::info("[SNOW DEFORMATION] Snow-flagged weather {:08X} '{}': precipitation {:08X} '{}', texture '{}' -> {}",
		a_weather->GetFormID(), a_weather->GetFormEditorID() ? a_weather->GetFormEditorID() : "",
		precipitation ? precipitation->GetFormID() : 0, precipitation && precipitation->GetFormEditorID() ? precipitation->GetFormEditorID() : "",
		texture ? texture : "", veto ? std::format("NOT snow ('{}'), no snowfall", veto) : std::string("snow"));
	return verdict.snow;
}

float SnowDeformation::ComputeSnowfallIntensity() const
{
	if (auto* worldSpace = NoSkyWorldspace()) {
		static RE::FormID logged = 0;
		if (std::exchange(logged, worldSpace->GetFormID()) != worldSpace->GetFormID())
			logger::info("[SNOW DEFORMATION] Worldspace {:08X} '{}' has No Sky: no snowfall, outdoor weather held as indoors",
				worldSpace->GetFormID(), worldSpace->GetFormEditorID() ? worldSpace->GetFormEditorID() : "");
		return 0.0f;
	}
	return ComputePrecipitationIntensity(RE::TESWeather::WeatherDataFlag::kSnow);
}

const SnowDeformation::SnowWeatherRange& SnowDeformation::SnowWeathers()
{
	auto& range = snowWeatherRange;
	auto* data = range.scanned ? nullptr : RE::TESDataHandler::GetSingleton();
	if (!data)
		return range;
	range.scanned = true;
	std::unordered_set<const RE::TESWeather*> skyless;
	for (auto* world : data->GetFormArray<RE::TESWorldSpace>())
		if (world && world->flags.any(RE::TESWorldSpace::Flag::kNoSky) && world->climate)
			for (auto* entry : world->climate->weatherList)
				if (entry && entry->weather)
					skyless.insert(entry->weather);
	std::vector<std::pair<float, const RE::TESWeather*>> snows;
	for (auto* weather : data->GetFormArray<RE::TESWeather>()) {
		if (!weather || !weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow) || skyless.contains(weather) || !FallsAsSnow(weather))
			continue;
		const float density = weather->precipitationData ? weather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f : 0.0f;
		snows.emplace_back(density > 0.0f ? density / kReferenceSnowDensity : 1.0f, weather);
	}
	std::sort(snows.begin(), snows.end(), [](const auto& a_a, const auto& a_b) { return a_a.first < a_b.first; });
	auto name = [](const RE::TESWeather* a_weather) { return std::string(a_weather->GetFormEditorID() ? a_weather->GetFormEditorID() : std::format("{:08X}", a_weather->GetFormID())); };
	range.weathers = uint32_t(snows.size());
	if (snows.empty())
		return range;
	range.heaviest = snows.back().first;
	range.heaviestName = name(snows.back().second);
	float widest = 0.0f;
	for (size_t i = 1; i < snows.size(); ++i)
		if (snows[i - 1].first > 1.0f && snows[i].first > snows[i - 1].first && snows[i].first / snows[i - 1].first > widest) {
			widest = snows[i].first / snows[i - 1].first;
			range.split = snows[i - 1].first / range.heaviest;
			range.splitName = name(snows[i - 1].second);
		}
	std::string list;
	for (const auto& [intensity, weather] : snows)
		list += std::format("{}{} {:.2f}", list.empty() ? "" : ", ", name(weather), intensity);
	logger::info("[SNOW DEFORMATION] body coat: {} snowfalls ({}); the heaviest '{}' {:.2f}; lumps on the living past {:.2f} of it{}", snows.size(), list, range.heaviestName,
		range.heaviest, range.split, range.splitName.empty() ? " (no step among the heavy ones: half)" : std::format(" ('{}')", range.splitName));
	return range;
}

float SnowDeformation::ComputePrecipitationIntensity(RE::TESWeather::WeatherDataFlag a_flag) const
{
	auto* sky = globals::game::sky;
	if (!sky)
		return 0.0f;

	auto snowDensity = [a_flag](RE::TESWeather* weather) -> float {
		if (!weather || !weather->data.flags.any(a_flag))
			return 0.0f;
		if (a_flag == RE::TESWeather::WeatherDataFlag::kSnow && !FallsAsSnow(weather))
			return 0.0f;
		if (auto* precipitation = weather->precipitationData) {
			float density = precipitation->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
			if (density > 0.0f)
				return density / kReferenceSnowDensity;
		}
		return 1.0f;
	};

	auto linearstep = [](float edge0, float edge1, float x) {
		return edge0 >= edge1 ? (x >= edge1 ? 1.0f : 0.0f) : std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	};

	float pct = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
	float intensity = 0.0f;
	if (float current = snowDensity(sky->currentWeather); current > 0.0f) {
		float fadeIn = sky->currentWeather->data.precipitationBeginFadeIn / 255.0f;
		float ramp = fadeIn > 0.0f ? linearstep(1.0f - fadeIn, 1.0f, pct) : (pct > 0.1f ? 1.0f : 0.0f);
		intensity = current * ramp;
	}
	if (float last = snowDensity(sky->lastWeather); last > 0.0f) {
		float fadeOut = sky->lastWeather->data.precipitationEndFadeOut / 255.0f;
		intensity = std::max(intensity, last * (1.0f - linearstep(fadeOut, 1.0f, pct)));
	}
	return std::min(intensity, kMaxSnowfallIntensity);
}

void SnowDeformation::Prepass()
{
	lightingCloudShadowSRV = nullptr;
	globals::d3d::context->PSGetShaderResources(25, 1, lightingCloudShadowSRV.put());
	PrepassSimulation();
	DrawShellEarlyPrepass();
	SyncSssDepth();
}

void SnowDeformation::PostPrepass()
{
	RestoreSssDepth();
}

void SnowDeformation::PrepassSimulation()
{
	LoadTraceFramePulse();
	RollCpuCensus();
	cpuCensusArmed = globals::state->frameCount <= cpuCensusArmedUntil;
	ScopedTicks _prepassTicks(cpuCensus.spanTicks[CpuCensus::kSpanPrepass], cpuCensus.spanCalls[CpuCensus::kSpanPrepass]);
	SpanClock spanClock;
	ApplyRangeSettings();

	TickVRAMLog();
	TickRowSpikeLog();

	auto context = globals::d3d::context;

	ID3D11ShaderResourceView* deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);
	ID3D11ShaderResourceView* waterSRV = waterFineTexture ? waterFineTexture->srv.get() : nullptr;
	context->PSSetShaderResources(kWaterWindowSlot, 1, &waterSRV);
	BindPrintReaders(context);
	EnsureShellSnowTextures();
	if (shellSnowDiffuseSRV) {
		ID3D11ShaderResourceView* albedoSRV = shellSnowDiffuseSRV.get();
		ID3D11ShaderResourceView* normalSRV = shellSnowNormalSRV.get();
		context->PSSetShaderResources(kHorizonSnowAlbedoSlot, 1, &albedoSRV);
		context->PSSetShaderResources(kHorizonSnowNormalSlot, 1, &normalSRV);
	}

	cityWorldspace = false;
	if (auto* tes = globals::game::tes)
		if (auto* worldSpace = tes->GetRuntimeData2().worldSpace)
			cityWorldspace = worldSpace->parentWorld != nullptr;

	statCapturedStatics.store((uint32_t)capturedStatics.size(), std::memory_order_relaxed);
	const size_t capturedLast = capturedStatics.size();
	capturedStatics.clear();
	capturedStaticsSet.clear();
	capturedStaticsSet.reserve(capturedLast);
	capturedLoadedPrev = capturedLoaded;
	capturedLoaded = 0;
	capturedRoads = 0;
	coatReachBounds[1].swap(coatReachBounds[0]);
	coatReachBounds[0].clear();

	statProjMatchedPrev = std::exchange(statProjMatched, 0u);
	statProjNoProjectionPrev = std::exchange(statProjNoProjection, 0u);
	statProjVetoedPrev = std::exchange(statProjVetoed, 0u);

	UpdateActiveWorldspace();
	snowfallIntensity = ComputeSnowfallIntensity();
	weatherClock.Read(weatherClockUnarm);
	TickAccumulation();
	TickSnowCondition();
	TickSnowClimate();

	if (SnowShadersPending(1))
		return;

	if (settings.EnableSnowDeformation && globals::state->inWorld) {
		UpdateShellTerrainWindow();
		UpdateUndulationField();
	}

	if (!settings.EnableSnowDeformation && !debugTerrainOverlay)
		return;

	auto ui = globals::game::ui;
	if (ui && ui->GameIsPaused())
		return;

	spanClock.Lap(CpuCensus::kSpanPrepassHead);

	deformGateBareFrames = DeformationWindowHasReaders() ? 0 : std::min(deformGateBareFrames + 1, kShellGateBareFrames);
	if (deformGateBareFrames >= kShellGateBareFrames) {
		if (!deformSuspended) {
			deformSuspended = true;
			logger::debug("[SNOW DEFORMATION] Deformation passes suspended: no snow depth in window");
		}
		return;
	}
	if (deformSuspended) {
		deformSuspended = false;
		clearRequested = true;
		logger::debug("[SNOW DEFORMATION] Deformation passes resumed, clearing map");
	}
	spanClock.Lap(CpuCensus::kSpanPrepassArcsAndGate);

	if (deformationCSFailed)
		return;
	if (!GetSnowCS(kDeformationRingCS) || !GetSnowCS(kDeformationScanEvolveCS) || !GetSnowCS(kDeformationEvolveCS) ||
		!GetSnowCS(kDeformationStampCS) || !GetSnowCS(kDeformationStampAllCS) || !GetSnowCS(kDeformationScanBermCS)) {
		deformationCSFailed = true;
		logger::error("[SNOW DEFORMATION] DeformationUpdateCS failed to compile, deformation passes disabled");
		return;
	}

	PerFrame perFrameData{};

	const DirectX::XMINT2 scroll = pendingScrollDelta;
	pendingScrollDelta = { 0, 0 };

	perFrameData.MapOrigin = mapOrigin;
	perFrameData.WindowOrigin = windowOrigin;
	perFrameData.TexelSize = deformWorldSize / deformMapDim;
	perFrameData.BermReach = BermReachScale();
	perFrameData.BodyPrint = BodyPrintScale();
	{
		const SnowClimateLook look = ComputeSnowClimateLook();
		const float wallEase = std::lerp(std::lerp(1.0f, 0.3f, GetSnowSoftness()), std::lerp(1.0f, 0.3f, look.slough), look.strength);
		perFrameData.StampFalloffStart = std::clamp(kTrenchWallSharpness * 0.01f * wallEase, 0.0f, 0.98f);
	}
	perFrameData.SlumpRate = std::clamp(settings.SlumpRate, 0.0f, 1.0f);
	perFrameData.ShoutShape = { std::clamp(settings.ShoutMarkWaist, 0.05f, 0.95f), std::clamp(settings.ShoutMarkRoundness, 1.0f, 4.0f),
		std::clamp(settings.ShoutMarkFadeStart, 0.0f, 0.98f), std::clamp(settings.ShoutMarkFarDepth, 0.0f, 1.0f) };
	perFrameData.ShoutChurn = { std::clamp(settings.ShoutChurn, 0.0f, 1.0f), std::max(settings.ShoutStreakSize, 4.0f),
		std::max(settings.ShoutStreakLength, 1.0f), 0.0f };
	perFrameData.UndulationAmp = std::max(settings.UndulationStrength, 0.0f);
	perFrameData.UndulationScale = std::max(settings.UndulationSpacing, 0.05f);
	perFrameData.AccumulationScale = GetAccumulationDepthScale();

	float deltaTime = *globals::game::deltaTime;
	TickGameClock();
	float refillIntensity = settings.RefillOnlyWhenSnowing ? snowfallIntensity : 1.0f;
	const float refillSeconds = gameClock.lastHours >= 0.0f ?
	                                gameClock.elapsedHours * 3600.0f / std::max(gameClock.timescale, 1.0f) :
	                                deltaTime;
	const float frameRefill = refillSeconds / kBaseRefillTime * refillIntensity * std::max(settings.RefillRateMultiplier, 0.0f);
	refillBank += frameRefill;
	perFrameData.RefillAmount = 0.0f;
	if (refillBank >= kRefillStep) {
		perFrameData.RefillAmount = refillBank;
		refillBank = 0.0f;
	}
	bloodBurialClock += frameRefill;
	perFrameData.GameDeltaTime = refillSeconds;

	perFrameData.DeltaTime = deltaTime;
	passClockSeconds += deltaTime;
	perFrameData.PassClock = static_cast<uint32_t>(std::fmod(passClockSeconds * 2.0, 1024.0));
	perFrameData.PassGap = 4;
	perFrameData.MeltPersistence = std::clamp(settings.MeltPersistence, 0.0f, 1.0f);
	perFrameData.MeltFloorStart = std::clamp(settings.MeltBowlFloor, 0.0f, 0.98f);
	perFrameData.MeltEdgeNoise = std::max(settings.MeltEdgeIrregularity, 0.0f);
	perFrameData.CrustPrintDepth = std::clamp(settings.CrustPrintDepth, 0.0f, 1.0f);
	perFrameData.CrustThaw = settings.CrustThawMinutes > 0.01f ?
	                             1.0f / (settings.CrustThawMinutes * 60.0f) :
	                             0.0f;
	perFrameData.CrustBreakOnCarve = std::max(settings.CrustBreakOnCarve, 0.0f);

	perFrameData.WindBias = { 0.0f, 0.0f };
	if (auto* sky = globals::game::sky) {
		float windStrength = std::clamp(sky->windSpeed, 0.0f, 1.0f);
		perFrameData.WindBias = { std::sin(sky->windAngle) * windStrength, std::cos(sky->windAngle) * windStrength };
	}

	if (trenchReinjectRequested.exchange(false, std::memory_order_acq_rel))
		clearRequested = true;

	perFrameData.ClearMap = clearRequested;
	clearRequested = false;

	{
		ArrivalRect arriving[2];
		const int arrivingCount = ComputeArrivalRects(scroll, perFrameData.ClearMap != 0, arriving);
		perFrameData.RingRectCount = (uint)arrivingCount;
		perFrameData.RingTotalTexels = 0;
		for (int i = 0; i < arrivingCount; i++) {
			perFrameData.RingRects[i] = { arriving[i].x0, arriving[i].y0, arriving[i].w, arriving[i].h };
			perFrameData.RingTotalTexels += (uint)(arriving[i].w * arriving[i].h);
		}
	}
	perFrameData.ForceAllDirty = debugForceAllTilesDirty ? 1u : 0u;

	spanClock.Lap(CpuCensus::kSpanPrepassPerFrame);
	TickTrenchClock();
	TickSnowAgeing();
	TickTrenchAgeing();
	perFrameData.AgeingClock = snowClimate.ageingClock;
	perFrameData.TrackRebase = trackClockTexture && bermFieldTexture ? snowClimate.ageingRebase : 0.0f;
	spanClock.Lap(CpuCensus::kSpanTrenchTicks);
	SweepTrenchStore();
	spanClock.Lap(CpuCensus::kSpanTrenchSweep);
	DrainTrenchBands();
	spanClock.Lap(CpuCensus::kSpanTrenchDrain);
	FlushDepartingTrenches(scroll, perFrameData.ClearMap != 0);
	spanClock.Lap(CpuCensus::kSpanTrenchFlush);
	RollTrenchWindow();
	spanClock.Lap(CpuCensus::kSpanTrenchRoll);
	perFrameData.InjectValid = BuildTrenchInject(scroll, perFrameData.ClearMap != 0);
	spanClock.Lap(CpuCensus::kSpanTrenchInject);

	GatherStamps(perFrameData);
	stampGatherFrame = globals::state->frameCount;
	TickSnowEvents();
	spanClock.Lap(CpuCensus::kSpanGather);
	DrawContactCapture(context);
	perFrameData.ContactCenter = contactCenter;
	perFrameData.ContactHalfExtent = kContactHalfExtent;
	perFrameData.ContactDim = (contactDrawsLast > 0 || contactSkinDrawsLast > 0 || contactCarriedLast > 0) ? float(kContactDim) : 0.0f;
	const bool roadRaster = heightMapValid && heightSkinDepth && heightSkinDepth->srv;
	perFrameData.HeightWindowCenter = heightWindowCenter;
	perFrameData.HeightHalfExtent = ObjectRasterHalfExtent();
	perFrameData.HasRoadRaster = roadRaster ? 1.0f : 0.0f;
	{
		constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
		perFrameData.TerrainWindowOrigin = { shellWindowCellX * cellSize, shellWindowCellY * cellSize };
		perFrameData.TerrainTexelSize = kShellVertexSpacing;
		perFrameData.TerrainDim = float(kShellWindowDim);
	}
	perFrameData.ViewCenter = contactCenter;
	perFrameData.ViewHalf = std::clamp(debugContactViewHalf, 64.0f, kContactHalfExtent);
	contactViewBoundRadius = 0.0f;
	if (debugContactView) {
		if (auto* player = globals::game::player) {
			if (auto* root = player->Get3D(false)) {
				perFrameData.ViewCenter = { root->worldBound.center.x, root->worldBound.center.y };
				contactViewBoundCenter = perFrameData.ViewCenter;
				contactViewBoundRadius = root->worldBound.radius;
			}
		}
	}
	contactViewCenter = perFrameData.ViewCenter;
	contactViewHalfUsed = perFrameData.ViewHalf;
	contactViewTexelSize = perFrameData.TexelSize;

	spanClock.Lap(CpuCensus::kSpanContact);
	MarkTrenchDirtyRows(perFrameData);
	spanClock.Lap(CpuCensus::kSpanDirtyRows);

	if (debugActivityView)
		EnsureActivityViewTexture();
	perFrameData.DebugActivityView = (debugActivityView && activityViewUAV) ? 1u : 0u;

	PollDeformActivity(context);
	spanClock.Lap(CpuCensus::kSpanPolls);

	const float kStampTol = std::max(2.0f, perFrameData.TexelSize);
	const float kStampTolSq = kStampTol * kStampTol;
	bool stampsQuiet = perFrameData.StampCount == (uint)lastStampSet.size();
	if (!stampsQuiet)
		stampSetTallyCount++;
	if (stampsQuiet && perFrameData.StampCount > 0) {
		stampMatchUsed.assign(lastStampSet.size(), 0);
		stampMatchIndex.resize(perFrameData.StampCount);
		for (uint i = 0; i < perFrameData.StampCount && stampsQuiet; i++) {
			const auto& s = perFrameData.Stamps[i];
			const auto& e = perFrameData.StampEnds[i];
			bool found = false;
			for (size_t j = 0; j < lastStampSet.size(); j++) {
				if (stampMatchUsed[j])
					continue;
				const auto& ps = lastStampSet[j];
				const auto& pe = lastStampEnds[j];
				const float dx = s.x - ps.x, dy = s.y - ps.y;
				const float ex = e.x - pe.x, ey = e.y - pe.y;
				if (dx * dx + dy * dy <= kStampTolSq && ex * ex + ey * ey <= kStampTolSq &&
					std::abs(s.z - ps.z) <= 0.01f && std::abs(s.w - ps.w) <= 1.0f &&
					std::abs(e.z - pe.z) <= 0.01f &&
					std::abs(e.w - pe.w) <= 0.01f + 0.01f * std::abs(pe.w)) {
					stampMatchUsed[j] = 1;
					stampMatchIndex[i] = (uint32_t)j;
					found = true;
					break;
				}
			}
			stampsQuiet = found;
		}
		if (!stampsQuiet)
			stampSetTallyDrift++;
	}
	if (stampsQuiet) {
		for (uint i = 0; i < perFrameData.StampCount; i++) {
			perFrameData.Stamps[i] = lastStampSet[stampMatchIndex[i]];
			perFrameData.StampEnds[i] = lastStampEnds[stampMatchIndex[i]];
		}
	}

	const bool inputsIdle =
		scroll.x == 0 && scroll.y == 0 &&
		stampsQuiet &&
		perFrameData.InjectValid == 0 &&
		perFrameData.RefillAmount <= 0.0f &&
		perFrameData.ClearMap == 0 &&
		perFrameData.ContactDim < 0.5f
		;
	const bool mapQuiet = !deformFlagActive && deformFlagSeq > deformLastNonIdleSeq;
	deformIdleBlockers =
		((scroll.x != 0 || scroll.y != 0) ? 1u : 0u) |
		(!stampsQuiet ? 2u : 0u) |
		(perFrameData.InjectValid != 0 ? 4u : 0u) |
		(perFrameData.RefillAmount > 0.0f ? 8u : 0u) |
		(perFrameData.ClearMap != 0 ? 16u : 0u) |
		(deformFlagActive ? 32u : 0u) |
		(deformFlagSeq <= deformLastNonIdleSeq ? 64u : 0u) |
		(perFrameData.ContactDim > 0.5f ? 128u : 0u)
		;
	const bool bermReachStale = perFrameData.BermReach != bermReachBaked;
	deformIdleSkipped = inputsIdle && mapQuiet && !bermReachStale && perFrameData.TrackRebase <= 0.0f &&
	                    !(debugForceDeformationUpdate || debugContactView);

	deformSkipTallyFrames++;
	if (deformIdleSkipped)
		deformSkipTallySkipped++;
	if (deformSkipTallyFrames >= 300) {
		deformSkipRate = (float)deformSkipTallySkipped / (float)deformSkipTallyFrames;
		deformSkipTallyFrames = 0;
		deformSkipTallySkipped = 0;
		stampSetCountChanges = stampSetTallyCount;
		stampSetDriftChanges = stampSetTallyDrift;
		stampSetTallyCount = 0;
		stampSetTallyDrift = 0;
	}

	if (deformIdleSkipped) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Map Update");
	}

	const bool evolveQuiet = !evolveFlagActive && evolveVerdictSeq > evolveLastArmSeq;
	const bool evolveNeeded = perFrameData.RefillAmount > 0.0f || !evolveQuiet ||
	                          (debugForceDeformationUpdate || debugContactView);
	evolveIdleLastFrame = !deformIdleSkipped && !evolveNeeded;
	spanClock.Lap(CpuCensus::kSpanIdleLogic);

	if (!deformIdleSkipped) {
		perFrame->Update(perFrameData);

		auto* map = deformationTextures[0];
		auto* scratch = deformationTextures[1];
		const bool ringRan = perFrameData.RingTotalTexels > 0;
		{
			uint64_t h = 1469598103934665603ull;
			auto mix = [&](const void* a_p, size_t a_n) {
				const auto* b = static_cast<const uint8_t*>(a_p);
				for (size_t i = 0; i < a_n; i++) {
					h ^= b[i];
					h *= 1099511628211ull;
				}
			};
			mix(&perFrameData.StampCount, sizeof(perFrameData.StampCount));
			mix(perFrameData.Stamps, perFrameData.StampCount * sizeof(perFrameData.Stamps[0]));
			mix(perFrameData.StampEnds, perFrameData.StampCount * sizeof(perFrameData.StampEnds[0]));
			mix(perFrameData.StampCones, perFrameData.StampCount * sizeof(perFrameData.StampCones[0]));
			cpuCensus.stampHashStable = h == cpuCensus.stampHash ? cpuCensus.stampHashStable + 1 : 0;
			cpuCensus.stampHash = h;
			cpuCensus.stampHashRing[cpuCensus.stampHashHead] = h;
			cpuCensus.stampHashHead = (cpuCensus.stampHashHead + 1) % CpuCensus::kHashRing;
		}
		const bool stampRan = perFrameData.StampCount > 0 || perFrameData.ContactDim > 0.5f;
		bool stampDispatched = stampRan;

		{
			ID3D11Buffer* buffers[1] = { perFrame->CB() };
			context->CSSetConstantBuffers(0, 1, buffers);

			ID3D11ShaderResourceView* nullPS = nullptr;
			context->PSSetShaderResources(101, 1, &nullPS);
			context->PSSetShaderResources(kLandBermSlot, 1, &nullPS);

			const UINT zeroFlag[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(deformActivityUAV.get(), zeroFlag);
			if (perFrameData.DebugActivityView) {
				const FLOAT zeroView[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				context->ClearUnorderedAccessViewFloat(activityViewUAV.get(), zeroView);
			}

			if (tileGridsNeedInit) {
				const UINT allDirty[4] = { 1, 1, 1, 1 };
				context->ClearUnorderedAccessViewUint(occupancyUAV.get(), allDirty);
				context->ClearUnorderedAccessViewUint(bermDirtyUAV.get(), allDirty);
				context->ClearUnorderedAccessViewUint(bermOccupancyUAV.get(), allDirty);
				const UINT allTouched[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(untouchedTilesUAV.get(), allTouched);
				tileGridsNeedInit = false;
			}
			if (perFrameData.BermReach != bermReachBaked) {
				const UINT allDirty[4] = { 1, 1, 1, 1 };
				context->ClearUnorderedAccessViewUint(bermDirtyUAV.get(), allDirty);
				bermReachBaked = perFrameData.BermReach;
			}

			if (perFrameData.TrackRebase > 0.0f) {
				if (auto* rebaseCS = GetSnowCS(kTrackRebaseCS)) {
					ID3D11UnorderedAccessView* rebaseUavs[2] = { trackClockTexture->uav.get(), bermFieldTexture->uav.get() };
					context->CSSetUnorderedAccessViews(0, 2, rebaseUavs, nullptr);
					context->CSSetShader(rebaseCS, nullptr, 0);
					context->Dispatch((deformMapDim + 7) / 8, (deformMapDim + 7) / 8, 1);
					ID3D11UnorderedAccessView* nullRebase[2] = {};
					context->CSSetUnorderedAccessViews(0, 2, nullRebase, nullptr);
				}
				snowClimate.ageingRebase = 0.0f;
			}

			ID3D11UnorderedAccessView* uavs[] = { map->uav.get(), deformActivityUAV.get(),
				perFrameData.DebugActivityView ? activityViewUAV.get() : nullptr,
				occupancyUAV.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
			ID3D11UnorderedAccessView* dirtyUAV = bermDirtyUAV.get();
			context->CSSetUnorderedAccessViews(6, 1, &dirtyUAV, nullptr);
			ID3D11UnorderedAccessView* clockUAV = trackClockTexture ? trackClockTexture->uav.get() : nullptr;
			ID3D11UnorderedAccessView* nullClockUAV = nullptr;
			ID3D11UnorderedAccessView* reachUAV = bodyReachTexture ? bodyReachTexture->uav.get() : nullptr;

			if (perFrameData.RingTotalTexels > 0) {
				ID3D11ShaderResourceView* ringSrvs[2] = { nullptr, trenchInjectSRV.get() };
				context->CSSetShaderResources(0, ARRAYSIZE(ringSrvs), ringSrvs);
				context->CSSetUnorderedAccessViews(4, 1, &clockUAV, nullptr);
				context->CSSetUnorderedAccessViews(5, 1, &reachUAV, nullptr);
				context->CSSetShader(GetSnowCS(kDeformationRingCS), nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
				context->Dispatch((perFrameData.RingTotalTexels + 63) / 64, 1, 1);
				globals::profiler->EndPass();
				context->CSSetUnorderedAccessViews(4, 1, &nullClockUAV, nullptr);
				context->CSSetUnorderedAccessViews(5, 1, &nullClockUAV, nullptr);
			} else {
				globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Map Update");
			}

			if (evolveNeeded) {
				ID3D11UnorderedAccessView* nullUav = nullptr;
				context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
				context->CSSetUnorderedAccessViews(3, 1, &nullUav, nullptr);
				ID3D11ShaderResourceView* occSRV = occupancySRV.get();
				context->CSSetShaderResources(4, 1, &occSRV);
				const uint tilesPerAxis = deformMapDim / 8;
				globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
				auto* scanSnapshotCS = GetSnowCS(kDeformationScanSnapshotCS);
				auto* snapshotCS = GetSnowCS(kDeformationSnapshotCS);
				if (!evolveSnapshotOff && scanSnapshotCS && snapshotCS && snapshotTileUAV && snapshotTileSRV && snapshotArgsUAV) {
					ID3D11UnorderedAccessView* snapUAVs[2] = { snapshotTileUAV.get(), snapshotArgsUAV.get() };
					context->CSSetUnorderedAccessViews(4, 2, snapUAVs, nullptr);
					context->CSSetShader(scanSnapshotCS, nullptr, 0);
					context->Dispatch((tilesPerAxis + 7) / 8, (tilesPerAxis + 7) / 8, 1);
					ID3D11UnorderedAccessView* nullSnapUAVs[2] = { nullptr, nullptr };
					context->CSSetUnorderedAccessViews(4, 2, nullSnapUAVs, nullptr);
					ID3D11ShaderResourceView* mapSRV = map->srv.get();
					ID3D11ShaderResourceView* snapListSRV = snapshotTileSRV.get();
					context->CSSetShaderResources(0, 1, &mapSRV);
					context->CSSetShaderResources(3, 1, &snapListSRV);
					ID3D11UnorderedAccessView* scratchUAV = scratch->uav.get();
					context->CSSetUnorderedAccessViews(0, 1, &scratchUAV, nullptr);
					context->CSSetShader(snapshotCS, nullptr, 0);
					context->DispatchIndirect(snapshotArgsBuffer.get(), 0);
					context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
					ID3D11ShaderResourceView* nullSRV = nullptr;
					context->CSSetShaderResources(0, 1, &nullSRV);
				} else if (auto* fullSnapshotCS = GetSnowCS(kDeformationSnapshotFullCS)) {
					ID3D11ShaderResourceView* mapSRV = map->srv.get();
					ID3D11UnorderedAccessView* scratchUAV = scratch->uav.get();
					context->CSSetShaderResources(0, 1, &mapSRV);
					context->CSSetUnorderedAccessViews(0, 1, &scratchUAV, nullptr);
					context->CSSetShader(fullSnapshotCS, nullptr, 0);
					context->Dispatch(tilesPerAxis, tilesPerAxis, 1);
					context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
					ID3D11ShaderResourceView* nullSRV = nullptr;
					context->CSSetShaderResources(0, 1, &nullSRV);
				}
				globals::profiler->EndPass();

				ID3D11UnorderedAccessView* scanUAVs[2] = { evolveTileUAV.get(), evolveArgsUAV.get() };
				context->CSSetUnorderedAccessViews(4, 2, scanUAVs, nullptr);
				context->CSSetShader(GetSnowCS(kDeformationScanEvolveCS), nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
				context->Dispatch((tilesPerAxis + 7) / 8, (tilesPerAxis + 7) / 8, 1);
				globals::profiler->EndPass();
				ID3D11UnorderedAccessView* nullScanUAVs[2] = { nullptr, nullptr };
				context->CSSetUnorderedAccessViews(4, 2, nullScanUAVs, nullptr);
				ID3D11ShaderResourceView* nullOcc = nullptr;
				context->CSSetShaderResources(4, 1, &nullOcc);

				ID3D11ShaderResourceView* listSRV = evolveTileSRV.get();
				ID3D11ShaderResourceView* srvs[2] = { scratch->srv.get(), trenchInjectSRV.get() };
				context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
				context->CSSetShaderResources(3, 1, &listSRV);
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
				context->CSSetShader(GetSnowCS(kDeformationEvolveCS), nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
				context->DispatchIndirect(evolveArgsBuffer.get(), 0);
				globals::profiler->EndPass();
			} else {
				globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Map Update");
			}

			{
				ID3D11ShaderResourceView* contactSRVs[3] = {
					(contactHeight && perFrameData.ContactDim > 0.5f) ? contactHeight->srv.get() : nullptr,
					shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr,
					perFrameData.HasRoadRaster > 0.5f ? heightSkinDepth->srv.get() : nullptr
				};
				context->CSSetShaderResources(7, 3, contactSRVs);
				ID3D11UnorderedAccessView* passUAV = passStateTexture ? passStateTexture->uav.get() : nullptr;
				context->CSSetUnorderedAccessViews(7, 1, &passUAV, nullptr);
				context->CSSetUnorderedAccessViews(4, 1, &clockUAV, nullptr);
				context->CSSetUnorderedAccessViews(5, 1, &reachUAV, nullptr);
			}
			if (stampRan) {
				const uint32_t tileCount = debugForceAllTilesDirty ? UINT32_MAX : BuildStampTileList(perFrameData);
				stampTilesLast = tileCount == UINT32_MAX ? kStampTileCap + 1 : tileCount;
				bool tiled = tileCount > 0 && tileCount != UINT32_MAX;
				if (tiled) {
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (SUCCEEDED(context->Map(stampTileBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
						memcpy(mapped.pData, stampTileScratch.data(), tileCount * sizeof(uint32_t));
						context->Unmap(stampTileBuffer.get(), 0);
					} else {
						tiled = false;
					}
				}
				if (tileCount == 0) {
					stampDispatched = false;
					globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Map Update");
				} else if (tiled) {
					ID3D11ShaderResourceView* tileSRV = stampTileSRV.get();
					context->CSSetShaderResources(2, 1, &tileSRV);
					context->CSSetShader(GetSnowCS(kDeformationStampCS), nullptr, 0);
					globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
					context->Dispatch(tileCount, 1, 1);
					globals::profiler->EndPass();
				} else {
					context->CSSetShader(GetSnowCS(kDeformationStampAllCS), nullptr, 0);
					globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
					context->Dispatch(deformMapDim / 8, deformMapDim / 8, 1);
					globals::profiler->EndPass();
				}
			} else {
				stampTilesLast = 0;
				globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Map Update");
			}
			if (debugContactView && !debugObjectPrintView) {
				EnsureContactViewTexture();
				if (auto* viewCS = GetSnowCS(kContactViewCS); viewCS && contactViewUAV) {
					ID3D11UnorderedAccessView* viewUAV = contactViewUAV.get();
					context->CSSetUnorderedAccessViews(2, 1, &viewUAV, nullptr);
					context->CSSetShader(viewCS, nullptr, 0);
					context->Dispatch(kContactViewPx / 8, 2 * kContactViewPx / 8, 1);
					ID3D11UnorderedAccessView* nullView = nullptr;
					context->CSSetUnorderedAccessViews(2, 1, &nullView, nullptr);
				}
			}
			{
				ID3D11ShaderResourceView* nullContact[3] = {};
				context->CSSetShaderResources(7, 3, nullContact);
				ID3D11UnorderedAccessView* nullPass = nullptr;
				context->CSSetUnorderedAccessViews(7, 1, &nullPass, nullptr);
				context->CSSetUnorderedAccessViews(4, 1, &nullClockUAV, nullptr);
				context->CSSetUnorderedAccessViews(5, 1, &nullClockUAV, nullptr);
			}
		}

		if (!bermFieldTexture) {
			globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Map Update");
		} else {
			globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
			ID3D11UnorderedAccessView* nullUav = nullptr;
			context->CSSetUnorderedAccessViews(6, 1, &nullUav, nullptr);
			ID3D11ShaderResourceView* dirtySRV = bermDirtySRV.get();
			context->CSSetShaderResources(6, 1, &dirtySRV);
			ID3D11UnorderedAccessView* bermArgs = bermArgsUAV.get();
			context->CSSetUnorderedAccessViews(5, 1, &bermArgs, nullptr);
			ID3D11UnorderedAccessView* bermListUAV = bermTileUAV.get();
			context->CSSetUnorderedAccessViews(7, 1, &bermListUAV, nullptr);
			context->CSSetShader(GetSnowCS(kDeformationScanBermCS), nullptr, 0);
			const uint tilesPerAxis = deformMapDim / 8;
			context->Dispatch((tilesPerAxis + 7) / 8, (tilesPerAxis + 7) / 8, 1);
			context->CSSetUnorderedAccessViews(7, 1, &nullUav, nullptr);
			context->CSSetUnorderedAccessViews(5, 1, &nullUav, nullptr);
			ID3D11ShaderResourceView* nullDirty = nullptr;
			context->CSSetShaderResources(6, 1, &nullDirty);

			if (auto* bermCS = GetSnowCS(kBermFieldTiledCS)) {
				context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
				ID3D11ShaderResourceView* bermSrvs[3] = { map->srv.get(), bermTileSRV.get(), trackClockTexture ? trackClockTexture->srv.get() : nullptr };
				context->CSSetShaderResources(0, ARRAYSIZE(bermSrvs), bermSrvs);
				ID3D11SamplerState* clockSampler = shellSnowSampler.get();
				context->CSSetSamplers(0, 1, &clockSampler);
				ID3D11UnorderedAccessView* bermUavs[] = { bermFieldTexture->uav.get() };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(bermUavs), bermUavs, nullptr);
				ID3D11UnorderedAccessView* bermOccUAV = bermOccupancyUAV.get();
				context->CSSetUnorderedAccessViews(2, 1, &bermOccUAV, nullptr);
				ID3D11UnorderedAccessView* dirtyUAV = bermDirtyUAV.get();
				context->CSSetUnorderedAccessViews(6, 1, &dirtyUAV, nullptr);
				context->CSSetShader(bermCS, nullptr, 0);
				context->DispatchIndirect(bermArgsBuffer.get(), 0);
				bermBakeSeq++;
				context->CSSetUnorderedAccessViews(2, 1, &nullUav, nullptr);
				context->CSSetUnorderedAccessViews(6, 1, &nullUav, nullptr);
				ID3D11SamplerState* nullSampler = nullptr;
				context->CSSetSamplers(0, 1, &nullSampler);
			} else {
				const UINT zeroMarks[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(bermDirtyUAV.get(), zeroMarks);
			}
			globals::profiler->EndPass();

			if (auto* untouchedCS = GetSnowCS(kUntouchedTilesCS)) {
				globals::profiler->BeginPass("SnowDeformation::Trails - Map Update");
				ID3D11UnorderedAccessView* nullTileUavs[4] = {};
				context->CSSetUnorderedAccessViews(0, 4, nullTileUavs, nullptr);
				ID3D11ShaderResourceView* occSrvs[2] = { occupancySRV.get(), bermOccupancySRV.get() };
				context->CSSetShaderResources(3, 2, occSrvs);
				ID3D11UnorderedAccessView* untouchedUAV = untouchedTilesUAV.get();
				context->CSSetUnorderedAccessViews(3, 1, &untouchedUAV, nullptr);
				context->CSSetShader(untouchedCS, nullptr, 0);
				const uint tileGroups = (deformMapDim / 8 + 7) / 8;
				context->Dispatch(tileGroups, tileGroups, 1);
				context->CSSetUnorderedAccessViews(0, 4, nullTileUavs, nullptr);
				ID3D11ShaderResourceView* nullOccSrvs[2] = {};
				context->CSSetShaderResources(3, 2, nullOccSrvs);
				globals::profiler->EndPass();
			}
		}

		context->CSSetShader(nullptr, nullptr, 0);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ShaderResourceView* nullSrvs[7] = {};
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);

		ID3D11UnorderedAccessView* nullUavs[8] = {};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

		deformDispatchSeq++;
		if (!inputsIdle)
			deformLastNonIdleSeq = deformDispatchSeq;
		if (stampDispatched)
			evolveLastArmSeq = std::max(evolveLastArmSeq, deformDispatchSeq);
		if (ringRan && perFrameData.InjectValid)
			evolveLastArmSeq = std::max(evolveLastArmSeq, evolveNeeded ? deformDispatchSeq - 1 : deformDispatchSeq);
		lastStampSet.assign(perFrameData.Stamps, perFrameData.Stamps + perFrameData.StampCount);
		lastStampEnds.assign(perFrameData.StampEnds, perFrameData.StampEnds + perFrameData.StampCount);
		for (uint i = 0; i < kDeformActivitySlots; i++) {
			if (!deformActivityPending[i]) {
				context->CopyResource(deformActivityStaging[i].get(), deformActivityBuffer.get());
				deformActivitySlotSeq[i] = deformDispatchSeq;
				deformActivitySlotEvolveRan[i] = evolveNeeded;
				deformActivityPending[i] = true;
				break;
			}
		}
	}

	spanClock.Lap(CpuCensus::kSpanDispatch);
	UpdateObjectPrints(context, perFrameData, !deformIdleSkipped);
	UpdateTrenchDebugTexture();

	trenchMapOrigin = windowOrigin;
	trenchMapTexel = perFrameData.TexelSize;
	trenchMapWorldspace = activeWorldspace.load(std::memory_order_acquire);
	trenchMapPhysOrigin = mapOrigin;
	trenchMapPrimed = true;

	deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);
	waterSRV = waterFineTexture ? waterFineTexture->srv.get() : nullptr;
	context->PSSetShaderResources(kWaterWindowSlot, 1, &waterSRV);
	BindPrintReaders(context);
	spanClock.Lap(CpuCensus::kSpanPrepassTail);
}

ID3D11ComputeShader* SnowDeformation::GetSnowCS(SnowCS a_which)
{
	struct Shader
	{
		const wchar_t* path;
		const char* entry;
		const char* define;
	};
	static constexpr Shader kShaders[kSnowCSCount] = {
		{ L"Data\\Shaders\\SnowDeformation\\ExclusionFieldCS.hlsl", "main", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "RingCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "TrackRebaseCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "EvolveCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "StampCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "ContactViewCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "StampAllCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "ScanEvolveCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "ScanSnapshotCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "SnapshotCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "SnapshotFullCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", "ScanBermCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\BermFieldCS.hlsl", "UntouchedCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\BermFieldCS.hlsl", "BermTiledCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\UndulationFieldCS.hlsl", "main", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\UndulationFieldCS.hlsl", "NoiseHashCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", "TerrainCutCS", "SNOW_TERRAIN_CUT_CS" },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "main", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "SssSyncCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "SssRestoreCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "SkinSssSyncCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "SkinSssRestoreCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "SkinSssSyncMarkCS", "SKIN_SSS_MARK" },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "TerrainFineCS", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\TerrainWindowFillCS.hlsl", "main", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\TrenchDebugCS.hlsl", "main", nullptr },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "ClusterCullCS", "SNOW_CLUSTER_CULL" },
		{ L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", "SkinCullCS", nullptr },
	};
	auto& slot = snowCS[a_which];
	if (!slot) {
		const Shader& shader = kShaders[a_which];
		std::vector<std::pair<const char*, const char*>> defines;
		if (shader.define)
			defines.emplace_back(shader.define, "");
		slot.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(shader.path, defines, "cs_5_0", shader.entry)));
	}
	return slot.get();
}

void SnowDeformation::EnsureContactViewTexture()
{
	if (contactViewTexture)
		return;
	auto device = globals::d3d::device;
	if (!device)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = kContactViewPx;
	desc.Height = 2 * kContactViewPx;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, contactViewTexture.put())))
		return;
	Util::SetResourceName(contactViewTexture.get(), "SnowDeformation::ContactView");
	device->CreateShaderResourceView(contactViewTexture.get(), nullptr, contactViewSRV.put());
	device->CreateUnorderedAccessView(contactViewTexture.get(), nullptr, contactViewUAV.put());
}

namespace
{
	void BakeNoiseTexture(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_cs, Texture2D* a_tex, UINT a_slot)
	{
		ID3D11UnorderedAccessView* uav = a_tex->uav.get();
		a_context->CSSetUnorderedAccessViews(a_slot, 1, &uav, nullptr);
		a_context->CSSetShader(a_cs, nullptr, 0);
		a_context->Dispatch(a_tex->desc.Width / 8, a_tex->desc.Height / 8, 1);
		ID3D11UnorderedAccessView* nullUav = nullptr;
		a_context->CSSetUnorderedAccessViews(a_slot, 1, &nullUav, nullptr);
		a_context->CSSetShader(nullptr, nullptr, 0);
	}
}

ID3D11ShaderResourceView* SnowDeformation::NoiseHashSRV()
{
	auto context = globals::d3d::context;
	if (!noiseHashTexture || !noiseHashTexture->srv)
		return nullptr;
	if (!noiseHashBaked) {
		auto* cs = GetSnowCS(kNoiseHashCS);
		if (!cs || !context)
			return nullptr;
		BakeNoiseTexture(context, cs, noiseHashTexture, 1);
		noiseHashBaked = true;
	}
	return noiseHashTexture->srv.get();
}

void SnowDeformation::UpdateUndulationField()
{
	auto context = globals::d3d::context;
	if (!undulationFieldTexture || !undulationFieldCB || !context) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
		return;
	}

	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float2 snapped = {
		std::round(eye.x / kUndulationFieldSnap) * kUndulationFieldSnap,
		std::round(eye.y / kUndulationFieldSnap) * kUndulationFieldSnap
	};
	const float scale = std::max(settings.UndulationSpacing, 0.05f);

	const bool dirty = !undulationFieldValid ||
	                   snapped.x != undulationFieldCenter.x || snapped.y != undulationFieldCenter.y ||
	                   scale != undulationFieldBakedScale;
	if (!dirty) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
		return;
	}
	auto* cs = GetSnowCS(kUndulationFieldCS);
	if (!cs) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
		return;
	}

	globals::profiler->BeginPass("SnowDeformation::Ground - Field Bakes");
	UndulationFieldCB cbData{};
	cbData.FieldOriginWorld = { snapped.x - kUndulationFieldHalfExtent + 0.5f * kUndulationFieldTexel,
		snapped.y - kUndulationFieldHalfExtent + 0.5f * kUndulationFieldTexel };
	cbData.FieldTexel = kUndulationFieldTexel;
	cbData.FieldScale = scale;
	undulationFieldCB->Update(cbData);

	ID3D11Buffer* cb = undulationFieldCB->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11UnorderedAccessView* uav = undulationFieldTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(cs, nullptr, 0);
	context->Dispatch(kUndulationFieldDim / 8, kUndulationFieldDim / 8, 1);
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();

	undulationFieldCenter = snapped;
	undulationFieldBakedScale = scale;
	undulationFieldValid = true;
}

uint32_t SnowDeformation::BuildStampTileList(const PerFrame& a_data)
{
	const int dim = (int)deformMapDim;
	const int mask = dim - 1;
	const int tilesPerAxis = dim / 8;
	const float texel = a_data.TexelSize;

	stampTileScratch.clear();
	const size_t words = ((size_t)tilesPerAxis * tilesPerAxis + 31) / 32;
	if (stampTileBits.size() != words)
		stampTileBits.resize(words);
	std::fill(stampTileBits.begin(), stampTileBits.end(), 0u);

	bool overflow = false;

	auto addWorldBox = [&](float minX, float minY, float maxX, float maxY) {
		int l0[2] = { (int)std::floor((minX - a_data.WindowOrigin.x) / texel) - 1,
			(int)std::floor((minY - a_data.WindowOrigin.y) / texel) - 1 };
		int l1[2] = { (int)std::floor((maxX - a_data.WindowOrigin.x) / texel) + 1,
			(int)std::floor((maxY - a_data.WindowOrigin.y) / texel) + 1 };
		int seg[2][2][2];
		int segCount[2];
		const int origin[2] = { mapOrigin.x, mapOrigin.y };
		for (int axis = 0; axis < 2; axis++) {
			l0[axis] = std::max(l0[axis], 0);
			l1[axis] = std::min(l1[axis], dim - 1);
			if (l0[axis] > l1[axis])
				return;
			const int p = (l0[axis] + origin[axis]) & mask;
			const int len = l1[axis] - l0[axis] + 1;
			const int run = std::min(len, dim - p);
			seg[axis][0][0] = p;
			seg[axis][0][1] = p + run - 1;
			segCount[axis] = 1;
			if (run < len) {
				seg[axis][1][0] = 0;
				seg[axis][1][1] = len - run - 1;
				segCount[axis] = 2;
			}
		}
		for (int sy = 0; sy < segCount[1]; sy++)
			for (int sx = 0; sx < segCount[0]; sx++)
				for (int ty = seg[1][sy][0] >> 3; ty <= seg[1][sy][1] >> 3; ty++)
					for (int tx = seg[0][sx][0] >> 3; tx <= seg[0][sx][1] >> 3; tx++) {
						const uint32_t idx = (uint32_t)(ty * tilesPerAxis + tx);
						if (stampTileBits[idx >> 5] & (1u << (idx & 31)))
							continue;
						stampTileBits[idx >> 5] |= 1u << (idx & 31);
						if (stampTileScratch.size() >= kStampTileCap) {
							overflow = true;
							return;
						}
						stampTileScratch.push_back((uint32_t)tx | ((uint32_t)ty << 16));
					}
	};

	const float gateScale = std::max(1.31f, 1.0f + a_data.MeltEdgeNoise);
	for (uint i = 0; i < a_data.StampCount && !overflow; i++) {
		const float reach = a_data.Stamps[i].w * gateScale;
		addWorldBox(std::min(a_data.Stamps[i].x, a_data.StampEnds[i].x) - reach,
			std::min(a_data.Stamps[i].y, a_data.StampEnds[i].y) - reach,
			std::max(a_data.Stamps[i].x, a_data.StampEnds[i].x) + reach,
			std::max(a_data.Stamps[i].y, a_data.StampEnds[i].y) + reach);
	}

	for (const auto* list : { &contactProps, &contactActors }) {
		for (const auto& prop : *list) {
			if (overflow)
				break;
			const float pad = 2.0f * texel;
			addWorldBox(prop.minX - pad, prop.minY - pad, prop.maxX + pad, prop.maxY + pad);
		}
	}

	return overflow ? UINT32_MAX : (uint32_t)stampTileScratch.size();
}

void SnowDeformation::ClearShaderCache()
{
	if (snowPrimeState.load(std::memory_order_acquire) == 1)
		return;
	deformationCSFailed = false;
	ReleaseBloodShaders();
	ReleaseObjectPrintShaders();
	noiseHashBaked = false;
	undulationFieldValid = false;
	for (auto& cs : snowCS)
		cs = nullptr;
	if (conditionViewCompile.valid())
		conditionViewCompile.wait();
	conditionViewShaderState.store(0, std::memory_order_release);
	ReleaseMarchCache();
	for (auto& attrPS : shellPSPrepassAttr)
		attrPS = nullptr;
	shellTerrainWetValid = false;
	delete depthSyncRectCB;
	depthSyncRectCB = nullptr;
	delete terrainFineCB;
	terrainFineCB = nullptr;
	delete shellTerrainFine;
	shellTerrainFine = nullptr;
	shellFineValid = false;
	delete skinCullCB;
	skinCullCB = nullptr;
	delete skinCullBounds;
	skinCullBounds = nullptr;
	delete skinCullArgs;
	skinCullArgs = nullptr;
	delete skinCullHiZ;
	skinCullHiZ = nullptr;
	for (auto* scratch : skinCullHiZScratch)
		delete scratch;
	skinCullHiZScratch.clear();
	skinCullHiZSRVs.clear();
	skinCullHiZUAVs.clear();
	for (auto& st : skinCullArgsStaging)
		st = nullptr;
	for (auto& st : skinCullHiZTopStaging)
		st = nullptr;
	skinCullCapacity = 0;
	skinCullLevels = 0;
	staticsVSParallaxBlob = nullptr;
	coatParallaxTried = false;
	delete patchCullCB;
	patchCullCB = nullptr;
	ReleasePatchDeferred();
	staticsVSBlob = nullptr;
	staticsILCache.clear();
	staticsILParallaxCache.clear();
	staticsShadersFailed = false;
	delete clusterBounds;
	clusterBounds = nullptr;
	delete clusterIndexPool;
	clusterIndexPool = nullptr;
	delete clusterScratchIB;
	clusterScratchIB = nullptr;
	delete meshBounds;
	meshBounds = nullptr;
	ResetSmoothedNormals(true);
	for (auto& key : heightRasterKey)
		key.clear();
	auto release = [](auto&... a_shaders) { ((a_shaders = nullptr), ...); };
	release(shellPSDeferred, shellPSDeferredConditionView, shellPSPrepassEarly, shellPSEarlyResolve, shellFillVS, shellFillRectVS, shellFillPS, shellFillPSPreSnow, shellFillPSClaim, preSnowCopyCS, sssChangedRaymarchCS);
	release(hiZBuildCS, hiZBuildUAVCS, smoothBoundsCS, smoothClusterCS, smoothAccumulateCS, smoothResolveCS, smoothFlatStatsCS);
	release(staticsVS, staticsTessVS, staticsHS, staticsDS, staticsPS, staticsPSNoDepth, staticsPSPrepassNoDepth, staticsPSDrapeNoDepth, staticsPSDrapePrepass, staticsPSDrapePrintsNoDepth);
	release(staticsVSParallax, staticsPSDrapeParallax, staticsPSDrapePrintsParallax, patchVS, patchShadowVS, patchTessVS, patchHS, patchDS, patchPS);
	release(heightVS, heightPS, heightPSTopOnly, heightPSSkinDepth, heightScrollCS, heightScrollTopCS, heightScrollRasterCS, heightRasterCompareCS, heightCombineCS);
}

void SnowDeformation::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.RangeSkinsM = std::clamp(settings.RangeSkinsM, kMinRangeSkinsM, kMaxRangeSkinsM);
	trenchRangeDirty = true;
	deformMapDimDirty = true;
	RefreshLandTextureDepths();
}

void SnowDeformation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SnowDeformation::RestoreDefaultSettings()
{
	settings = {};
	trenchRangeDirty = true;
	deformMapDimDirty = true;
	RefreshLandTextureDepths();
	shellDataDirty.store(true, std::memory_order_release);
}

void SnowDeformation::QueryAdapterVRAM(uint64_t& a_usageMB, uint64_t& a_budgetMB, uint64_t* a_nonLocalMB)
{
	a_usageMB = 0;
	a_budgetMB = 0;
	if (a_nonLocalMB)
		*a_nonLocalMB = 0;
	const auto adapter3 = globals::menu ? globals::menu->GetDXGIAdapter3() : nullptr;
	if (!adapter3)
		return;
	DXGI_QUERY_VIDEO_MEMORY_INFO info{};
	if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
		a_usageMB = info.CurrentUsage >> 20;
		a_budgetMB = info.Budget >> 20;
	}
	if (a_nonLocalMB && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
		*a_nonLocalMB = info.CurrentUsage >> 20;
}

void SnowDeformation::TickVRAMLog()
{
	vramTickCounter++;

	if (settings.VRAMAwareTrenchMap && !deformMapVramCap && deformMapDim > 2048u) {
		uint64_t nowMB = 0, nowBudgetMB = 0;
		QueryAdapterVRAM(nowMB, nowBudgetMB);
		auto bytes = [](Texture2D* a_wrap) { return a_wrap ? Util::ResourceBytes(a_wrap->resource.get()) : uint64_t(0); };
		const uint64_t deformMB = (bytes(deformationTextures[0]) + bytes(deformationTextures[1])) >> 20;
		const uint64_t freesMB = deformMB - deformMB / 4;
		if (nowBudgetMB && nowMB + freesMB > nowBudgetMB) {
			deformMapVramCap = 2048u;
			deformMapDimDirty = true;
			logger::info("[SNOW DEFORMATION] VRAM-aware trench map: {} -> 2048 for this session (adapter {}/{} MB, the step frees ~{} MB)", deformMapDim, nowMB, nowBudgetMB, freesMB);
		}
	}
}

void SnowDeformation::TickRowSpikeLog()
{
	constexpr float kSpikeMs = 4.0f;
	if (vramTickCounter < rowSpikeQuietUntil)
		return;
	const auto& results = globals::profiler->GetResults();
	const Profiler::TimerResult* spike = nullptr;
	for (const auto& r : results)
		if (r.valid && r.gpuTimeMs > kSpikeMs && r.name.starts_with("SnowDeformation::") && (!spike || r.gpuTimeMs > spike->gpuTimeMs))
			spike = &r;
	if (!spike)
		return;
	rowSpikeQuietUntil = vramTickCounter + 120;

	uint64_t usageMB = 0, budgetMB = 0, nonLocalMB = 0;
	QueryAdapterVRAM(usageMB, budgetMB, &nonLocalMB);
	auto top = [&](bool a_cpu) {
		std::vector<const Profiler::TimerResult*> rows;
		for (const auto& r : results)
			if (r.valid)
				rows.push_back(&r);
		const size_t n = std::min<size_t>(rows.size(), 4);
		std::partial_sort(rows.begin(), rows.begin() + n, rows.end(), [a_cpu](auto* a, auto* b) {
			return a_cpu ? a->cpuTimeMs > b->cpuTimeMs : a->gpuTimeMs > b->gpuTimeMs;
		});
		std::string line;
		for (size_t i = 0; i < n; i++)
			line += std::format("{}{} {:.2f}", i ? ", " : "", rows[i]->name, a_cpu ? rows[i]->cpuTimeMs : rows[i]->gpuTimeMs);
		return line;
	};
	logger::warn("[SNOW DEFORMATION] Row spike: {} GPU {:.2f} ms (avg {:.2f}), CPU {:.2f} ms | all rows GPU {:.2f}, CPU {:.2f} ms | VRAM {}/{} MB, system {} MB | top GPU: {} | top CPU: {}",
		spike->name, spike->gpuTimeMs, spike->avgMs, spike->cpuTimeMs, globals::profiler->GetTotalTimeMs(), globals::profiler->GetCpuTotalTimeMs(),
		usageMB, budgetMB, nonLocalMB, top(false), top(true));
}
