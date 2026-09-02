// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <dxgi1_4.h>

// Settings serialisation, written out rather than generated.
//
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT tops out at 63 fields and
// that ceiling was reached exactly: adding the lightning cloak knobs pushed it
// over, and the macro failed with a hundred errors naming every field in the
// struct rather than the one thing that was wrong. The JSON shape here is
// identical - one flat object keyed by field name - so settings files written
// before this change load unchanged, and there is no ceiling left to hit when
// frost adds its own.
#define SNOWDEF_SETTINGS_FIELDS(X) \
	X(EnableSnowDeformation) \
	X(StampRadius) \
	X(FootPrintScale) \
	X(TrenchWallSharpness) \
	X(SlumpRate) \
	X(RefillRateMultiplier) \
	X(RefillOnlyWhenSnowing) \
	X(PersistTrenches) \
	X(StoredTrenchFadeDays) \
	X(TrenchMemoryMB) \
	X(EnableSnowAccumulation) \
	X(PersistAccumulation) \
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
	X(CrustGloss) \
	X(CrustRoughness) \
	X(CrustNormalFlatten) \
	X(CrustSpecular) \
	X(CrustSheen) \
	X(CrustTint) \
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
	X(ShoutConeSpread) \
	X(ShoutConeLength) \
	X(ForceCarveDepth) \
	X(DashGougeScale) \
	X(ForceTrackWidth) \
	X(ForceTrackDepth) \
	X(LiftFrostEffects) \
	X(FrostTexturePath) \
	X(EnableLightningArcs) \
	X(LightningArcWidth) \
	X(LightningArcBrightness) \
	X(LightningArcLife) \
	X(LightningArcTint) \
	X(LightningArcTexturePath) \
	X(LightningArcUseTexture) \
	X(FrostPatternStrength) \
	X(FrostPatternScale) \
	X(AtronachFireReach) \
	X(AtronachFireDeathRadius) \
	X(AtronachFireBurnSeconds) \
	X(AtronachFrostReach) \
	X(AtronachFrostDeathRadius) \
	X(AtronachShockReach) \
	X(AtronachShockDeathRadius) \
	X(SnowClassDepths) \
	X(TextureDepths) \
	X(ObjectsSnowDepth) \
	X(ProjSnowFillPct) \
	X(ShellMaxSlopeDeg) \
	X(RockMaxSlopeDeg) \
	X(PlaneSplitStep) \
	X(PlaneMergeHeight) \
	X(OverheadClearance) \
	X(MeldCoPlanar) \
	X(PileHeightRatio) \
	X(SkinBreakupAmt) \
	X(SkinWeldAmt) \
	X(EnableBlobShell) \
	X(BlobSpacing) \
	X(BlobSize) \
	X(BlobSizeNoise) \
	X(BlobMaskThreshold) \
	X(BlobMaxSlopeDeg) \
	X(BlobBorderNoise) \
	X(BlobLayers) \
	X(BlobRadius) \
	X(BlobMeldDepthRange) \
	X(BlobMeldSmoothRange) \
	X(BlobMeldIterations) \
	X(BlobMeldMaxRadiusPx) \
	X(BlobMeldSmoothing) \
	X(BlobMeldAnchor) \
	X(BlobMeldVerticalRange) \
	X(SkyExposurePct) \
	X(SnowSettlingPct) \
	X(RoadMeshesDepth) \
	X(SnowTexturePath) \
	X(TrampleZoneScale) \
	X(TrampleZoneHeight) \
	X(SnowBorderDithering) \
	X(TrenchFloorHeight) \
	X(SnowBorderNoise) \
	X(SnowBorderSmoothness) \
	X(SnowBorderFade) \
	X(SnowMoundSteepness) \
	X(UndulationStrength) \
	X(UndulationSpacing) \
	X(Tessellation) \
	X(ParallaxShadowStrength) \
	X(ParallaxDepth) \
	X(TrenchFloorFade) \
	X(BermHeight) \
	X(RimLip) \
	X(RimTeeth) \
	X(BermClods) \
	X(BowWaveHeight) \
	X(BowWaveReach) \
	X(BowWaveForward) \
	X(BowWaveChunk) \
	X(BowWaveFullSpeed) \
	X(ChurnHeight) \
	X(ChurnSize) \
	X(CompactMatte) \
	X(ShellSSSRemarch) \
	X(ShellSSSRemarchThickness) \
	X(ShellSSSRemarchCasterCap) \
	X(ShellBareGroundCull) \
	X(DeformMapResolution) \
	X(RangeTrenchesM) \
	X(RangeSkinsM) \
	X(RangeSkinsGeometryM) \
	X(SkinDistantBareness) \
	X(ObjectTrenches) \
	X(ProjMaskPlacement) \
	X(ProjDepthDensity) \
	X(ObjectSnow3D) \
	X(RoadHeightfield) \
	X(LODSnowSensitivity) \
	X(HorizonSnow) \
	X(ProjSnowMatch) \
	X(GlacierSnowMatch)

void to_json(nlohmann::json& j, const SnowDeformation::Settings& s)
{
#define X(field) j[#field] = s.field;
	SNOWDEF_SETTINGS_FIELDS(X)
#undef X
}

void from_json(const nlohmann::json& j, SnowDeformation::Settings& s)
{
	// A missing key keeps the default, matching what WITH_DEFAULT did, so a
	// settings file written before a field existed still loads.
	const SnowDeformation::Settings defaults{};
#define X(field) s.field = j.value(#field, defaults.field);
	SNOWDEF_SETTINGS_FIELDS(X)
#undef X
}

void SnowDeformation::CreateDeformationTextures()
{
	LoadTraceScope _loadTrace(this, "CreateDeformationTextures");
	for (uint i = 0; i < 2; i++) {
		delete deformationTextures[i];
		deformationTextures[i] = nullptr;
	}

	D3D11_TEXTURE2D_DESC texDesc = {
		.Width = deformMapDim,
		.Height = deformMapDim,
		.MipLevels = 1,
		.ArraySize = 1,
		// Four channels: depth, signed surface state (melt positive, scorch
		// negative), crust, and one spare. Widened rather than given a field of
		// its own because crust PERSISTS after the frost that made it - a wall
		// glazes ground and then expires - so a separate field would have had
		// to duplicate this map's ping-pong, scrolling and decay rather than
		// deriving itself each frame the way the exclusion field does.
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	};

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = 1 }
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	// [0] is THE map; [1] is EvolveCS's snapshot scratch (the retired
	// ping-pong partner, kept at identical spec for the pre-evolve copy).
	for (uint i = 0; i < 2; i++) {
		deformationTextures[i] = new Texture2D(texDesc, i == 0 ? "SnowDeformation::DeformationMap" : "SnowDeformation::EvolveScratch");
		deformationTextures[i]->CreateSRV(srvDesc);
		deformationTextures[i]->CreateUAV(uavDesc);
	}

	// The tile store's window-sized companions follow the map's dimension.
	if (!CreateTrenchStoreResources()) {
		logger::warn("[SNOW DEFORMATION] Trench store resources failed; trenches will not survive leaving the window");
	}

	// Berm field: a bake of the 17-tap disc average the shells used to run per
	// pixel. Same dimensions and format as the map it is derived from, and no
	// ping-pong - it is rebuilt from scratch from the current map each frame.
	delete bermFieldTexture;
	bermFieldTexture = new Texture2D(texDesc, "SnowDeformation::BermField");
	bermFieldTexture->CreateSRV(srvDesc);
	bermFieldTexture->CreateUAV(uavDesc);

	// Tile-dispatch state, sized to the map's tile grid (dim/8 per axis).
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

		// Occupancy and berm-dirty content is unknown until the next Prepass
		// seeds both all-1.
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
		// Idle-skip activity flag: one raw uint the update CS ORs when any texel
		// changed at stored precision, plus a staging ring to read it back
		// without a stall. Dimension-independent, so a map-resolution change
		// never touches it.
		auto device = globals::d3d::device;
		// 52 bytes, layout mirrored in DeformationUpdateCS.hlsl:
		// [0] changed-anywhere flag, [4] changed count,
		// [8] 65535-minX, [12] 65535-minY (min via complemented InterlockedMax,
		// so ClearUAV's all-zero init works for every field), [16] maxX,
		// [20] maxY, [24] depth count, [28] melt/scorch count, [32]
		// crust/deposit count, [36] per-texel max-delta sum, fixed-point 1e6,
		// [40] evolve-only flag (that pass's own idle gate; meaningful only on
		// frames evolve ran), [44] evolve tile count, [48] berm tile count
		// (the dispatch census).
		D3D11_BUFFER_DESC flagDesc{};
		flagDesc.ByteWidth = 52;
		flagDesc.Usage = D3D11_USAGE_DEFAULT;
		flagDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		flagDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		DX::ThrowIfFailed(device->CreateBuffer(&flagDesc, nullptr, deformActivityBuffer.put()));
		Util::SetResourceName(deformActivityBuffer.get(), "SnowDeformation::DeformActivity");

		D3D11_UNORDERED_ACCESS_VIEW_DESC flagUavDesc{};
		flagUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		flagUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		flagUavDesc.Buffer.NumElements = 13;
		flagUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(deformActivityBuffer.get(), &flagUavDesc, deformActivityUAV.put()));
		Util::SetResourceName(deformActivityUAV.get(), "SnowDeformation::DeformActivity UAV");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = 52;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (uint i = 0; i < kDeformActivitySlots; i++) {
			DX::ThrowIfFailed(device->CreateBuffer(&stagingDesc, nullptr, deformActivityStaging[i].put()));
			Util::SetResourceName(deformActivityStaging[i].get(), "SnowDeformation::DeformActivityStaging");
		}

		// Stamp-pass tile list: CPU-built per frame, capacity-fixed so it is
		// dimension-independent.
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

		// Indirect args for the evolve dispatch, written by TileArgsCS.
		// Dimension-independent (always three uints).
		D3D11_BUFFER_DESC argsDesc{};
		argsDesc.ByteWidth = 3 * sizeof(uint32_t);
		argsDesc.Usage = D3D11_USAGE_DEFAULT;
		argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, nullptr, evolveArgsBuffer.put()));
		Util::SetResourceName(evolveArgsBuffer.get(), "SnowDeformation::EvolveArgs");
		D3D11_UNORDERED_ACCESS_VIEW_DESC argsUavDesc{};
		argsUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		argsUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		argsUavDesc.Buffer.NumElements = 3;
		argsUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(evolveArgsBuffer.get(), &argsUavDesc, evolveArgsUAV.put()));
		Util::SetResourceName(evolveArgsUAV.get(), "SnowDeformation::EvolveArgs UAV");
		DX::ThrowIfFailed(device->CreateBuffer(&argsDesc, nullptr, bermArgsBuffer.put()));
		Util::SetResourceName(bermArgsBuffer.get(), "SnowDeformation::BermArgs");
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(bermArgsBuffer.get(), &argsUavDesc, bermArgsUAV.put()));
		Util::SetResourceName(bermArgsUAV.get(), "SnowDeformation::BermArgs UAV");
	}

	{
		// Wide exclusion field. Two 8-bit channels (suppression, melt) are
		// plenty for values the shells only ever smoothstep.
		D3D11_TEXTURE2D_DESC fieldDesc = {
			.Width = kExclusionFieldDim,
			.Height = kExclusionFieldDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8G8_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC fieldSrvDesc = {
			.Format = fieldDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC fieldUavDesc = {
			.Format = fieldDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		exclusionFieldTexture = new Texture2D(fieldDesc, "SnowDeformation::ExclusionField");
		exclusionFieldTexture->CreateSRV(fieldSrvDesc);
		exclusionFieldTexture->CreateUAV(fieldUavDesc);
		exclusionFieldCB = new ConstantBuffer(ConstantBufferDesc<ExclusionFieldCB>(), "SnowDeformation::ExclusionFieldCB");
	}

	{
		// Baked undulation field: amp-free height + shading gradient in half
		// floats. Camera-snapped and settings-keyed, so it rebakes on
		// recenter/Spacing change rather than per frame.
		D3D11_TEXTURE2D_DESC undDesc = {
			.Width = kUndulationFieldDim,
			.Height = kUndulationFieldDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC undSrvDesc = {
			.Format = undDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC undUavDesc = {
			.Format = undDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		undulationFieldTexture = new Texture2D(undDesc, "SnowDeformation::UndulationField");
		undulationFieldTexture->CreateSRV(undSrvDesc);
		undulationFieldTexture->CreateUAV(undUavDesc);
		undulationFieldCB = new ConstantBuffer(ConstantBufferDesc<UndulationFieldCB>(), "SnowDeformation::UndulationFieldCB");
		undulationFieldValid = false;
	}

	{
		D3D11_TEXTURE2D_DESC terrainDesc = {
			.Width = kShellWindowDim,
			.Height = kShellWindowDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			// UAV: the far-fill CS writes heightmap-sourced texels in place
			// after each CPU upload.
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC terrainSrvDesc = {
			.Format = terrainDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC terrainUavDesc = {
			.Format = terrainDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		shellTerrainTexture = new Texture2D(terrainDesc, "SnowDeformation::ShellTerrainWindow");
		shellTerrainTexture->CreateSRV(terrainSrvDesc);
		shellTerrainTexture->CreateUAV(terrainUavDesc);
	}

	windowFillCB = new ConstantBuffer(ConstantBufferDesc<WindowFillCB>(), "SnowDeformation::WindowFillCB");
	shellCB = new ConstantBuffer(ConstantBufferDesc<ShellCB>(), "SnowDeformation::ShellCB");
	staticsCB = new ConstantBuffer(ConstantBufferDesc<StaticsCB>(), "SnowDeformation::StaticsCB");
	smoothCB = new ConstantBuffer(ConstantBufferDesc<SmoothCB>(), "SnowDeformation::SmoothCB");
	heightProcessCB = new ConstantBuffer(ConstantBufferDesc<HeightProcessCB>(), "SnowDeformation::HeightProcessCB");
	blobCB = new ConstantBuffer(ConstantBufferDesc<BlobCB>(), "SnowDeformation::BlobCB");
	meldCB = new ConstantBuffer(ConstantBufferDesc<MeldCB>(), "SnowDeformation::MeldCB");
	meldSeedCB = new ConstantBuffer(ConstantBufferDesc<MeldSeedCB>(), "SnowDeformation::MeldSeedCB");
	doorsCB = new ConstantBuffer(ConstantBufferDesc<ExclusionsCB>(), "SnowDeformation::ExclusionsCB");

	CreateHeightFieldResources();

	{
		// RT0 MAX (tops) + RT1 MIN (bottoms) + RT2 MAX (skin depth): the
		// extreme surfaces win per texel in any draw order; no depth buffer.
		D3D11_BLEND_DESC minmaxBlendDesc{};
		minmaxBlendDesc.IndependentBlendEnable = TRUE;
		// RT3 (Blob Snow Shell): the per-layer placement mask, MAX like the tops.
		for (int i = 0; i < 4; i++) {
			minmaxBlendDesc.RenderTarget[i].BlendEnable = TRUE;
			minmaxBlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].BlendOp = i == 1 ? D3D11_BLEND_OP_MIN : D3D11_BLEND_OP_MAX;
			minmaxBlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ONE;
			minmaxBlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_MAX;
			// RT2 carries a second channel (G = road-heightfield bit); MAX on
			// it means "any road wrote this texel". RT3 too: G = the blob
			// mask's fresh-top channel.
			minmaxBlendDesc.RenderTarget[i].RenderTargetWriteMask = (i == 2 || i == 3) ?
			                                                            (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN) :
			                                                            D3D11_COLOR_WRITE_ENABLE_RED;
		}
		DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&minmaxBlendDesc, heightMaxBlendState.put()));
		Util::SetResourceName(heightMaxBlendState.get(), "SnowDeformation::HeightMinMaxBlend");
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
	// Advance the window once per frame and only from the in-world upload:
	// reflection/early uploads carry probe cameras that must not steer it.
	// Advance only while the map is actually updated (feature enabled, or
	// the overlay keeping the simulation alive for debugging); otherwise a
	// walking origin desyncs the constant buffer from the frozen texture,
	// visible as trails sliding with the camera.
	static Util::FrameChecker frameChecker;
	if (a_inWorld && (settings.EnableSnowDeformation || debugTerrainOverlay) && frameChecker.IsNewFrame()) {
		// Snap to whole texels so scrolling never resamples the map. The
		// cached FrameBuffer camera position is what the lighting pixel
		// shader sees as CameraPosAdjust, so map and terrain agree.
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
		// The toroidal origin advances HERE, with the window, so every
		// consumer CB filled after this point carries the pair consistently.
		// The map itself catches up in Prepass (RingCS), before anything
		// renders. The dim is a power of two, so the wrap is a mask.
		const int originMask = (int)deformMapDim - 1;
		mapOrigin.x = (mapOrigin.x + scrollX) & originMask;
		mapOrigin.y = (mapOrigin.y + scrollY) & originMask;
		windowOrigin = desiredOrigin;
	}

	SettingsGPU data{};
	data.WindowOrigin = windowOrigin;
	data.DeformMapOrigin = mapOrigin;
	data.InvWorldSize = 1.0f / deformWorldSize;
	data.EnableSnowDeformation = settings.EnableSnowDeformation;
	data.DebugTerrainOverlay = (debugTerrainOverlay ? 1u : 0u) | (debugTilingRuler ? 2u : 0u) | (debugProjSnowView ? 4u : 0u) | (debugGlacierView ? 8u : 0u) | (debugProjFillView ? 16u : 0u);

	// Horizon snow: LOD terrain only exists beyond the loaded-cell seam
	// (where the shell ends), so the recolor simply applies to all of it â€”
	// including any LOD peeking through under the shell, which then wears
	// the same material and helps hide holes.
	EnsureShellSnowTextures();
	data.LODReplaceStart = 0.0f;
	data.LODReplaceFadeInv = 1.0f / 2048.0f;
	data.LODSnowSensitivity = std::clamp(settings.LODSnowSensitivity, 0.0f, 1.0f);
	// PBR auto-detection only; the manual linear override for non-PBR sets was retired.
	data.SnowIsLinear = shellSnowTextureIsPBR ? 1.0f : 0.0f;
	data.SnowRoughnessScale = snowRoughnessScale;
	data.LODReplaceEnable = (settings.EnableSnowDeformation && settings.HorizonSnow && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.SnowHasNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	data.ProjSnowEnable = (settings.EnableSnowDeformation && settings.ProjSnowMatch && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	// Snow Fill, 0..1 across the slider's span: how much of the projected
	// footprint the recolor pushes to FULL shell-snow weight, most
	// up-facing pixels first (SKIN-PLACEMENT-PLAN round 11 - the fill
	// lives in Lighting's recolor, where the real weight is).
	data.ProjSnowFill = std::clamp(settings.ProjSnowFillPct / 100.0f, 0.0f, 1.0f);
	data.BakedSnowEnable = (settings.EnableSnowDeformation && settings.GlacierSnowMatch && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	// The world map renders the LOD world without the shell, so a shell-
	// matched recolor there mismatches everything else the map shows
	// skins are gated the same way in DrawCapturedStatics.
	if (globals::state->isMapMenuOpen) {
		data.LODReplaceEnable = 0.0f;
		data.ProjSnowEnable = 0.0f;
		data.BakedSnowEnable = 0.0f;
	}
	return data;
}

void SnowDeformation::ApplyRangeSettings()
{
	// Trench window: content is scale-relative, so a resize clears the map.
	if (trenchRangeDirty || !rangeInitApplied) {
		float newWorldSize = std::clamp(settings.RangeTrenchesM, 29.0f, 200.0f) * 2.0f * kUnitsPerMeter;
		if (std::abs(newWorldSize - deformWorldSize) > 1.0f) {
			deformWorldSize = newWorldSize;
			clearRequested = true;
		}
		trenchRangeDirty = false;
	}

	// Map resolution: same contract as the range change - texel content is
	// resolution-relative, so the pair recreates (with the store's window-sized
	// companions) and clears, and the store re-injects what it holds.
	if (deformMapDimDirty || !rangeInitApplied) {
		// Snapped to a power of two - the toroidal mask requires it. The
		// combo only offers these three; this guards a hand-edited JSON.
		const uint clamped = std::clamp(settings.DeformMapResolution, 1024u, 4096u);
		const uint desired = clamped >= 4096u ? 4096u : (clamped >= 2048u ? 2048u : 1024u);
		if (desired != deformMapDim) {
			deformMapDim = desired;
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
			// Evolve's own verdict, only from frames it actually ran (the
			// buffer is cleared per frame, so a skipped evolve reads 0 there).
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
		// A failed map drops the slot rather than wedging the ring; the verdict
		// simply stays whatever it was, which errs toward running.
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

void SnowDeformation::TickGameClock()
{
	gameClock.elapsedHours = 0.0f;
	gameClock.reversed = false;

	auto* calendar = globals::game::calendar ? globals::game::calendar : RE::Calendar::GetSingleton();
	if (!calendar)
		return;

	// The co-save callbacks land on the game thread. They ask rather than
	// write, so the reading below stays owned by one thread.
	if (gameClockUnarm.exchange(false, std::memory_order_acq_rel))
		gameClock.lastHours = -1.0f;

	// Waiting works by cranking the timescale enormously for its animation, so
	// a live reading taken during one is the wait and not the player's setting.
	// Latch the last plausible value; waited hours then pass at the rate the
	// same hours would have passed at if they had been played.
	const float rawScale = calendar->GetTimescale();
	if (rawScale >= 1.0f && rawScale <= 100.0f)
		gameClock.timescale = rawScale;

	// Elapsed hours telescope, so the calendar's float32 day quantisation
	// (~2.6-second steps late game) cancels instead of accumulating. A wait,
	// a sleep or a fast travel needs no special case: the next reading carries
	// the whole elapsed span.
	const float hours = calendar->GetHoursPassed();
	gameClockHours.store(hours, std::memory_order_relaxed);
	if (gameClock.lastHours < 0.0f) {
		gameClock.lastHours = hours;
		return;
	}
	const float delta = hours - gameClock.lastHours;
	gameClock.lastHours = hours;

	// Backwards is a loaded save: the consumers' state belongs to a timeline
	// that no longer exists. Each decides what that means for itself.
	if (delta < -1.0e-4f) {
		gameClock.reversed = true;
		return;
	}
	gameClock.elapsedHours = std::max(delta, 0.0f);
}

float SnowDeformation::ComputeSnowfallIntensity() const
{
	auto* sky = RE::Sky::GetSingleton();
	if (!sky)
		return 0.0f;

	// Normalized precipitation density of a snowing weather; snow-flagged
	// weathers without particle data count as baseline snowfall.
	auto snowDensity = [](RE::TESWeather* weather) -> float {
		if (!weather || !weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow))
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

	// Fade with the transition the way the engine fades the particles: the
	// incoming weather's precipitation ramps in over the last
	// precipitationBeginFadeIn/255 of the transition, the outgoing weather's
	// dies over the first precipitationEndFadeOut/255 (WetnessEffects' rain
	// interpretation of the same record bytes).
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
	LoadTraceFramePulse();
	ApplyRangeSettings();

	// Frame boundary for the once-per-frame local shadow atlas copy.
	pointShadowFrameIndex++;
	TickVRAMLog();

	auto context = globals::d3d::context;

	// Keep t101 bound even while paused or disabled: the lighting shader
	// samples it whenever the feature is compiled in (and gates on
	// EnableSnowDeformation from FeatureData).
	ID3D11ShaderResourceView* deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);
	// Horizon snow albedo (t102) + normals (t103) for the LOD terrain
	// recolor; the shader gates on LODReplaceEnable/SnowHasNormal, which
	// require these SRVs to exist.
	EnsureShellSnowTextures();
	if (shellSnowDiffuseSRV) {
		ID3D11ShaderResourceView* horizonSnowSRVs[2] = { shellSnowDiffuseSRV.get(), shellSnowNormalSRV.get() };
		context->PSSetShaderResources(102, 2, horizonSnowSRVs);
	}

	// New frame: publish last frame's statics-capture count and reset the
	// list before this frame's opaque rendering fills it again.
	statCapturedStatics.store((uint32_t)capturedStatics.size(), std::memory_order_relaxed);
	capturedStatics.clear();
	capturedStaticsSet.clear();

	// Publish last frame's projected-snow classification counters for the
	// menu debug readout, then reset for this frame's opaque pass.
	statProjMatchedPrev = statProjMatched.exchange(0, std::memory_order_relaxed);
	statProjNoProjectionPrev = statProjNoProjection.exchange(0, std::memory_order_relaxed);
	statProjVetoedPrev = statProjVetoed.exchange(0, std::memory_order_relaxed);

	UpdateActiveWorldspace();

	// While the prime owns a group's shader members, its render paths skip;
	// groups unlock as the worker publishes them (SnowShadersPending), so
	// the snowfield appears when ITS shaders exist rather than when all
	// do. Skipped frames resume exactly like a long hitch - the window-
	// jump and trench-inject paths already handle the catch-up.
	if (SnowShadersPending(1))
		return;

	if (settings.EnableSnowDeformation && globals::state->inWorld) {
		UpdateShellTerrainWindow();
		UpdateUndulationField();
	}

	// The overlay keeps the map simulation (stamps, scroll, refill) running
	// while the feature is disabled, so path tracking can be debugged with
	// every visual effect off.
	if (!settings.EnableSnowDeformation && !debugTerrainOverlay)
		return;

	auto ui = globals::game::ui;
	if (ui && ui->GameIsPaused())
		return;

	// No ground in the deformation window carries positive snow depth, so both
	// dispatches would produce a map no pixel can sample. Placed with the other
	// early-outs rather than at the dispatch: everything below it - the trench
	// store sweep, the flush, the inject build, the stamp gather - is work in
	// service of that map, and the scroll delta must keep accumulating rather
	// than be consumed, exactly as it does while the feature is switched off.
	//
	// Resuming forces a clear instead of trusting a delta that may now exceed
	// the map. Trenches come back through the tile store's re-inject, the same
	// path a worldspace change already uses.
	if (!DeformationWindowHasSnow()) {
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

	PerFrame perFrameData{};

	// The window origin and the toroidal map origin were advanced in
	// GetCommonBufferData (during UpdateSharedData); only consume the stored
	// state here. The scroll stays CPU-side: the GPU expresses it as the
	// arriving-ring rects below.
	const DirectX::XMINT2 scroll = pendingScrollDelta;
	pendingScrollDelta = { 0, 0 };

	perFrameData.MapOrigin = mapOrigin;
	perFrameData.WindowOrigin = windowOrigin;
	perFrameData.TexelSize = deformWorldSize / deformMapDim;
	// Sharpness is a percent slider; 100% clamps just below the degenerate
	// smoothstep(1, 1, x) edge.
	perFrameData.StampFalloffStart = std::clamp(settings.TrenchWallSharpness / 100.0f, 0.0f, 0.98f);
	perFrameData.SlumpRate = std::clamp(settings.SlumpRate, 0.0f, 1.0f);

	float deltaTime = *globals::game::deltaTime;
	// Taken here rather than beside the trench work below because the refill is
	// spent in game time; nothing between the two returns early, so it is still
	// read exactly once a frame.
	TickGameClock();
	// Refill rate follows the weather's snowfall density; interiors have no
	// snowing weather and do not refill. With RefillOnlyWhenSnowing off the
	// weather is ignored and the baseline rate applies everywhere.
	snowfallIntensity = ComputeSnowfallIntensity();
	float refillIntensity = settings.RefillOnlyWhenSnowing ? snowfallIntensity : 1.0f;
	// Spent in GAME time, not render time. A wait or a sleep advances the
	// calendar without rendering the hours it passes, so a render-second refill
	// left the trenches in front of the player untouched across a night in a
	// blizzard - while the stored ones behind them, which have always decayed on
	// the game clock, went away. ONE definition of refill for both. In ordinary
	// play this is the same number as before: the calendar advances at exactly
	// timescale x render time, so the conversion cancels.
	const float refillSeconds = gameClock.lastHours >= 0.0f ?
	                                gameClock.elapsedHours * 3600.0f / std::max(gameClock.timescale, 1.0f) :
	                                deltaTime;
	perFrameData.RefillAmount = refillSeconds / kBaseRefillTime * refillIntensity * std::max(settings.RefillRateMultiplier, 0.0f);
	// The same span, for the world's other self-driven clocks: the glaze thaw
	// (which runs under a clear sky, so waiting skipped it entirely) and the
	// unsupported-snow settle. NOT for stamp application - a fire must not
	// carve its whole basin in the one frame after a night's sleep.
	perFrameData.GameDeltaTime = refillSeconds;

	// Melt stamps accumulate per second. Persistence is a refill slowdown on
	// melted ground rather than banked depth, so the bowl profile survives the
	// readers' saturation intact.
	perFrameData.DeltaTime = deltaTime;
	perFrameData.MeltPersistence = std::clamp(settings.MeltPersistence, 0.0f, 1.0f);
	// Clamped just below the degenerate smoothstep(1, 1, x) edge, as the
	// trench sharpness slider is.
	perFrameData.MeltFloorStart = std::clamp(settings.MeltBowlFloor, 0.0f, 0.98f);
	perFrameData.MeltEdgeNoise = std::max(settings.MeltEdgeIrregularity, 0.0f);
	perFrameData.CrustPrintDepth = std::clamp(settings.CrustPrintDepth, 0.0f, 1.0f);
	perFrameData.CrustThaw = settings.CrustThawMinutes > 0.01f ?
	                             1.0f / (settings.CrustThawMinutes * 60.0f) :
	                             0.0f;
	perFrameData.CrustBreakOnCarve = std::max(settings.CrustBreakOnCarve, 0.0f);

	// Wind bias for the refill: the engine's live blended wind (derived from
	// the weather records), so drifting accumulation tracks transitions.
	perFrameData.WindBias = { 0.0f, 0.0f };
	if (auto* sky = RE::Sky::GetSingleton()) {
		float windStrength = std::clamp(sky->windSpeed, 0.0f, 1.0f);
		perFrameData.WindBias = { std::sin(sky->windAngle) * windStrength, std::cos(sky->windAngle) * windStrength };
	}

	// A co-save load has no other way in: nothing scrolls into a window already
	// sitting over the ground the store describes, and the map still holds the
	// timeline the player just left.
	if (trenchReinjectRequested.exchange(false, std::memory_order_acq_rel))
		clearRequested = true;

	perFrameData.ClearMap = clearRequested;
	clearRequested = false;

	// The texels whose world assignment this frame's scroll (or clear)
	// changed - RingCS's dispatch domain. Same rects BuildTrenchInject fills,
	// through the same function, so the two cannot disagree.
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

	// Persistent trenches. Ordered against the dispatch:
	// the flush stages what this frame's scroll is about to discard, so it must
	// read the map BEFORE the ping-pong swap below, and it uses the window
	// state the map's contents belong to rather than the live one - a clear
	// arrives here with the worldspace, texel size or both already changed.
	// The clock was read above; a tile folded in this frame must be stamped with
	// the decay the world has already accrued, and a backwards jump has to drop
	// the store before anything reads it.
	TickTrenchClock();
	TickAccumulation();
	SweepTrenchStore();
	DrainTrenchBands();
	FlushDepartingTrenches(scroll, perFrameData.ClearMap != 0);
	// Departure is not the only way ground becomes worth storing: dig and save
	// without moving and nothing ever leaves. This keeps the store true.
	RollTrenchWindow();
	perFrameData.InjectValid = BuildTrenchInject(scroll, perFrameData.ClearMap != 0);

	// The two CPU gathers, named beside the dispatches below. Every GPU pass in
	// this feature was already timed and neither of these was, which is the
	// half that grew: the spell branch walks the projectile manager, resolves
	// records and queries land height per source, all on the render thread.
	// Sequential rather than nested - the profiler tracks one current pass.
	UpdateLightningArcs(globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f);

	globals::profiler->BeginPass("SnowDeformation::GatherSpells");
	GatherSpellEmitters();
	globals::profiler->EndPass();

	globals::profiler->BeginPass("SnowDeformation::GatherStamps");
	GatherStamps(perFrameData);
	globals::profiler->EndPass();

	// Marked here, consumed by NEXT frame's roll: the map these stamps are
	// about to be written into is the one that frame will be copying.
	MarkTrenchDirtyRows(perFrameData);

	// Bow-wave crests, deposited into the map's .w so they PERSIST on the
	// ground rather than following the feet that made them.
	// GatherStamps has just sorted them nearest-first, so the slots go to the
	// crests the player can actually see.
	{
		const uint waveCount = std::min((uint)bowWaves.size(), (uint)kMaxBowWaves);
		perFrameData.DepositParams = { settings.BowWaveHeight > 0.001f ? (float)waveCount : 0.0f,
			std::clamp(settings.BowWaveReach, 0.25f, 3.0f),
			std::clamp(settings.BowWaveForward, 0.0f, 1.0f),
			// w unused: trench-spoil decay is a fixed clock in the CS.
			0.0f };
		for (uint i = 0; i < waveCount; i++) {
			const auto& wave = bowWaves[i];
			perFrameData.DepositPosDir[i] = { wave.pos.x, wave.pos.y, wave.dir.x, wave.dir.y };
			perFrameData.DepositShape[i] = { wave.radius, wave.strength, wave.prev.x, wave.prev.y };
		}
	}

	// Activity view: which texels the CS is about to claim changed, and in
	// which channel. The count in the readback rides along whether or not the
	// view is displayed.
	if (debugActivityView)
		EnsureActivityViewTexture();
	perFrameData.DebugActivityView = (debugActivityView && activityViewUAV) ? 1u : 0u;

	// Idle skip: with every input quiet and the last executed pass reporting
	// the map at its fixed point, both dispatches would rewrite the map
	// byte-identically - so neither runs. Any doubt (readback not in yet,
	// inputs active since the verdict) keeps them running.
	PollDeformActivity(context);

	// Stamps are "quiet" when the SET is unchanged since the last executed
	// dispatch, not when it is empty - planted feet stamp every frame, so a
	// count never reaches zero while anyone stands on snow. Re-applying an
	// unchanged set is exactly what the activity verdict proved absorbed
	// (carves max-blend; melt/crust saturate, and stay active until they do).
	// Matched within a tolerance rather than hashed on a grid: a swaying foot
	// straddling any quantization boundary reads as change every few frames,
	// and each flicker costs dispatch + verdict latency - measured as a
	// permanent 0% skip under `tai`. One TEXEL of tolerance: movement under a
	// texel cannot change which texels a print covers, and max-blend keeps the
	// high-water mark, so snapping it is visually free - while idle-animation
	// weight shifts (2+ units, measured at 116-161/300 frames) stay absorbed.
	// Safe for real walkers: drift is measured against the BASELINE, so a
	// walking foot accumulates past a texel within a frame or two and carves
	// normally.
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
	// Snap a quiet-matched set to its baseline. The match alone was not
	// enough: while anything else keeps the pass running, the swayed stamps
	// are still APPLIED, and each sub-tolerance sway re-carves footprint edge
	// texels by a hair - so the map keeps changing, which keeps the pass
	// running, which keeps applying sway. Substituting the baseline makes the
	// applied set bit-identical frame over frame; the edges converge and the
	// feedback breaks. The baseline rebase below then keeps it FIXED while
	// quiet, so it cannot creep.
	if (stampsQuiet) {
		for (uint i = 0; i < perFrameData.StampCount; i++) {
			perFrameData.Stamps[i] = lastStampSet[stampMatchIndex[i]];
			perFrameData.StampEnds[i] = lastStampEnds[stampMatchIndex[i]];
		}
	}

	const bool inputsIdle =
		scroll.x == 0 && scroll.y == 0 &&
		stampsQuiet &&
		perFrameData.DepositParams.x < 0.5f &&
		perFrameData.InjectValid == 0 &&
		perFrameData.RefillAmount <= 0.0f &&
		perFrameData.ClearMap == 0;
	const bool mapQuiet = !deformFlagActive && deformFlagSeq > deformLastNonIdleSeq;
	deformIdleBlockers =
		((scroll.x != 0 || scroll.y != 0) ? 1u : 0u) |
		(!stampsQuiet ? 2u : 0u) |
		(perFrameData.DepositParams.x >= 0.5f ? 4u : 0u) |
		(perFrameData.InjectValid != 0 ? 8u : 0u) |
		(perFrameData.RefillAmount > 0.0f ? 16u : 0u) |
		(perFrameData.ClearMap != 0 ? 32u : 0u) |
		(deformFlagActive ? 64u : 0u) |
		(deformFlagSeq <= deformLastNonIdleSeq ? 128u : 0u);
	// The berm field is only rebuilt by an executed pass; re-enabling its A/B
	// at rest needs one forced run or the stale bake stands until something moves.
	const bool bermHeal = prevBermBakeDisabled && !shellBermBakeDisabled;
	prevBermBakeDisabled = shellBermBakeDisabled;
	deformIdleSkipped = inputsIdle && mapQuiet && !bermHeal && !debugForceDeformationUpdate;

	// The skip-rate window: "idle" that is really a flicker (a stamp crossing
	// the hash quantum every few frames) reads identically in a single-frame
	// readout; the rate is what tells them apart.
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
		// Skipping IS the measurement. Without an explicit zero the profiler
		// keeps publishing the stale pre-skip window (or retires the row), so
		// an idle pass still reads as costing full price.
		globals::profiler->MarkPassSkipped("SnowDeformation::DeformationRing");
		globals::profiler->MarkPassSkipped("SnowDeformation::TileScan");
		globals::profiler->MarkPassSkipped("SnowDeformation::DeformationEvolve");
		globals::profiler->MarkPassSkipped("SnowDeformation::DeformationStamps");
		globals::profiler->MarkPassSkipped("SnowDeformation::BermField");
	}

	// Evolve's own gate, inside a running frame: when its last full run
	// changed nothing at stored precision and nothing external has written
	// the map since (ring inject, stamps), rerunning it is a proven no-op.
	// Refill arms it directly - a continuous input, not an event. This is
	// what keeps walking in clear weather off the 4M-thread pass: the ring
	// is a band, stamps are the stamp pass, and the settled world stays
	// settled.
	const bool evolveQuiet = !evolveFlagActive && evolveVerdictSeq > evolveLastArmSeq;
	const bool evolveNeeded = perFrameData.RefillAmount > 0.0f || !evolveQuiet ||
	                          debugForceDeformationUpdate;
	evolveIdleLastFrame = !deformIdleSkipped && !evolveNeeded;

	if (!deformIdleSkipped) {
		perFrame->Update(perFrameData);

		auto* map = deformationTextures[0];
		auto* scratch = deformationTextures[1];
		const bool ringRan = perFrameData.RingTotalTexels > 0;
		const bool stampRan = perFrameData.StampCount > 0 || perFrameData.DepositParams.x > 0.5f;
		// Whether the stamp pass actually wrote anything - a stamp set that
		// lies entirely outside the window dispatches nothing and must not
		// re-arm evolve.
		bool stampDispatched = stampRan;

		{
			ID3D11Buffer* buffers[1] = { perFrame->CB() };
			context->CSSetConstantBuffers(0, 1, buffers);

			// The single map is about to be written; the frame-start t101 bind
			// aliases it and the UAV bind would force-null it anyway - done
			// explicitly so the debug layer stays quiet. Rebound at the end of
			// Prepass as before.
			ID3D11ShaderResourceView* nullPS = nullptr;
			context->PSSetShaderResources(101, 1, &nullPS);

			const UINT zeroFlag[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(deformActivityUAV.get(), zeroFlag);
			// Max-composed by the passes, so it needs a fresh canvas each frame.
			if (perFrameData.DebugActivityView) {
				const FLOAT zeroView[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				context->ClearUnorderedAccessViewFloat(activityViewUAV.get(), zeroView);
			}

			// Grid content is unknown after a (re)create; seed all-dirty so
			// the first evolve and berm passes visit everything and write the
			// truth back.
			if (tileGridsNeedInit) {
				const UINT allDirty[4] = { 1, 1, 1, 1 };
				context->ClearUnorderedAccessViewUint(occupancyUAV.get(), allDirty);
				context->ClearUnorderedAccessViewUint(bermDirtyUAV.get(), allDirty);
				tileGridsNeedInit = false;
			}

			ID3D11UnorderedAccessView* uavs[] = { map->uav.get(), deformActivityUAV.get(),
				perFrameData.DebugActivityView ? activityViewUAV.get() : nullptr,
				occupancyUAV.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
			ID3D11UnorderedAccessView* dirtyUAV = bermDirtyUAV.get();
			context->CSSetUnorderedAccessViews(6, 1, &dirtyUAV, nullptr);

			// Ring: rewrite only the texels the scroll (or clear) reassigned.
			// This is the whole scroll cost now - a walking-speed frame is a
			// few thousand threads instead of a 4M-texel copy.
			if (perFrameData.RingTotalTexels > 0) {
				ID3D11ShaderResourceView* ringSrvs[2] = { nullptr, trenchInjectSRV.get() };
				context->CSSetShaderResources(0, ARRAYSIZE(ringSrvs), ringSrvs);
				context->CSSetShader(GetDeformationRingCS(), nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::DeformationRing");
				context->Dispatch((perFrameData.RingTotalTexels + 63) / 64, 1, 1);
				globals::profiler->EndPass();
			} else {
				globals::profiler->MarkPassSkipped("SnowDeformation::DeformationRing");
			}

			// Evolve reads a snapshot so its neighbour taps (slump support,
			// upwind supply) see one consistent frame. The copy must follow
			// the ring, or arriving texels would evolve from the departed
			// ground that used to stand at their physical position. The
			// dispatch is indirect over ScanEvolveCS's occupied-plus-halo
			// list; neither the list nor its count ever touches the CPU.
			if (evolveNeeded) {
				ID3D11UnorderedAccessView* nullUav = nullptr;
				context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
				context->CopyResource(scratch->resource.get(), map->resource.get());

				// Scan: occupancy (SRV, so drop its UAV bind) + halo -> list.
				context->CSSetUnorderedAccessViews(3, 1, &nullUav, nullptr);
				const UINT zeroList[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(evolveTileUAV.get(), zeroList);
				ID3D11ShaderResourceView* occSRV = occupancySRV.get();
				context->CSSetShaderResources(4, 1, &occSRV);
				ID3D11UnorderedAccessView* listUAV = evolveTileUAV.get();
				context->CSSetUnorderedAccessViews(4, 1, &listUAV, nullptr);
				context->CSSetShader(GetDeformationScanEvolveCS(), nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::TileScan");
				const uint tilesPerAxis = deformMapDim / 8;
				context->Dispatch((tilesPerAxis + 7) / 8, (tilesPerAxis + 7) / 8, 1);

				// List count -> indirect args.
				context->CSSetUnorderedAccessViews(4, 1, &nullUav, nullptr);
				ID3D11ShaderResourceView* listSRV = evolveTileSRV.get();
				context->CSSetShaderResources(5, 1, &listSRV);
				ID3D11UnorderedAccessView* argsUAV = evolveArgsUAV.get();
				context->CSSetUnorderedAccessViews(5, 1, &argsUAV, nullptr);
				context->CSSetShader(GetDeformationTileArgsCS(), nullptr, 0);
				context->Dispatch(1, 1, 1);
				globals::profiler->EndPass();
				context->CSSetUnorderedAccessViews(5, 1, &nullUav, nullptr);
				ID3D11ShaderResourceView* nullOcc = nullptr;
				context->CSSetShaderResources(4, 1, &nullOcc);

				ID3D11ShaderResourceView* srvs[2] = { scratch->srv.get(), trenchInjectSRV.get() };
				context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
				context->CSSetShaderResources(3, 1, &listSRV);
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
				context->CSSetShader(GetDeformationEvolveCS(), nullptr, 0);
				globals::profiler->BeginPass("SnowDeformation::DeformationEvolve");
				context->DispatchIndirect(evolveArgsBuffer.get(), 0);
				globals::profiler->EndPass();
			} else {
				globals::profiler->MarkPassSkipped("SnowDeformation::TileScan");
				globals::profiler->MarkPassSkipped("SnowDeformation::DeformationEvolve");
			}

			// Stamps + waves RMW the texels evolve just wrote. Sustained
			// stamps (melt, crust) must re-apply even when the set is quiet -
			// dwell time is their input - so the gate is presence, not change.
			// Dispatched over the inputs' bounding tiles (the CPU knows every
			// capsule and wave); full-map only past the list cap or under the
			// force-all-dirty cross-check - never a truncated list, which
			// would be a silently frozen stamp.
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
					// Every stamp lies outside the window; nothing to write.
					stampDispatched = false;
					globals::profiler->MarkPassSkipped("SnowDeformation::DeformationStamps");
				} else if (tiled) {
					ID3D11ShaderResourceView* tileSRV = stampTileSRV.get();
					context->CSSetShaderResources(2, 1, &tileSRV);
					context->CSSetShader(GetDeformationStampCS(), nullptr, 0);
					globals::profiler->BeginPass("SnowDeformation::DeformationStamps");
					context->Dispatch(tileCount, 1, 1);
					globals::profiler->EndPass();
				} else {
					context->CSSetShader(GetDeformationStampAllCS(), nullptr, 0);
					globals::profiler->BeginPass("SnowDeformation::DeformationStamps");
					context->Dispatch(deformMapDim / 8, deformMapDim / 8, 1);
					globals::profiler->EndPass();
				}
			} else {
				stampTilesLast = 0;
				globals::profiler->MarkPassSkipped("SnowDeformation::DeformationStamps");
			}
		}

		// Berm bake, rebuilt only where the map changed (plus the tap-reach
		// halo): scan the dirty grid AFTER the stamp pass so its marks are
		// in, size the indirect args, rebuild the listed tiles, consume the
		// marks. The toroidal layout is what makes the unrebuilt remainder
		// stay valid across scrolls. While the A/B toggle holds the shells on
		// their per-pixel path, marks ACCUMULATE instead - re-enabling (which
		// bermHeal forces to execute) rebuilds exactly what was missed.
		if (shellBermBakeDisabled || !bermFieldTexture) {
			globals::profiler->MarkPassSkipped("SnowDeformation::BermField");
		} else {
			globals::profiler->BeginPass("SnowDeformation::BermField");
			ID3D11UnorderedAccessView* nullUav = nullptr;
			// The dirty grid moves from UAV (writers) to SRV (scan).
			context->CSSetUnorderedAccessViews(6, 1, &nullUav, nullptr);
			const UINT zeroList[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(bermTileUAV.get(), zeroList);
			ID3D11ShaderResourceView* dirtySRV = bermDirtySRV.get();
			context->CSSetShaderResources(6, 1, &dirtySRV);
			ID3D11UnorderedAccessView* bermListUAV = bermTileUAV.get();
			context->CSSetUnorderedAccessViews(7, 1, &bermListUAV, nullptr);
			context->CSSetShader(GetDeformationScanBermCS(), nullptr, 0);
			const uint tilesPerAxis = deformMapDim / 8;
			context->Dispatch((tilesPerAxis + 7) / 8, (tilesPerAxis + 7) / 8, 1);

			context->CSSetUnorderedAccessViews(7, 1, &nullUav, nullptr);
			ID3D11ShaderResourceView* bermListSRV = bermTileSRV.get();
			context->CSSetShaderResources(5, 1, &bermListSRV);
			ID3D11UnorderedAccessView* bermArgs = bermArgsUAV.get();
			context->CSSetUnorderedAccessViews(5, 1, &bermArgs, nullptr);
			context->CSSetShader(GetDeformationTileArgsCS(), nullptr, 0);
			context->Dispatch(1, 1, 1);
			context->CSSetUnorderedAccessViews(5, 1, &nullUav, nullptr);

			if (auto* bermCS = GetBermFieldTiledCS()) {
				// The freshly written map is still bound as a UAV; a texture
				// cannot be read and written at once, so drop that binding
				// first. BermFieldCS's own binding space: t0 map, t1 list.
				context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
				ID3D11ShaderResourceView* bermSrvs[2] = { map->srv.get(), bermTileSRV.get() };
				context->CSSetShaderResources(0, ARRAYSIZE(bermSrvs), bermSrvs);
				ID3D11UnorderedAccessView* bermUavs[] = { bermFieldTexture->uav.get() };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(bermUavs), bermUavs, nullptr);
				context->CSSetShader(bermCS, nullptr, 0);
				context->DispatchIndirect(bermArgsBuffer.get(), 0);
			}
			globals::profiler->EndPass();

			// Consumed: the next frame's marks start clean. (Drop the scan's
			// SRV bind first so the clear sees no aliased view.)
			ID3D11ShaderResourceView* nullDirty = nullptr;
			context->CSSetShaderResources(6, 1, &nullDirty);
			context->ClearUnorderedAccessViewUint(bermDirtyUAV.get(), zeroList);
		}

		context->CSSetShader(nullptr, nullptr, 0);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullBuffer);

		ID3D11ShaderResourceView* nullSrvs[7] = {};
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);

		ID3D11UnorderedAccessView* nullUavs[8] = {};
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);

		// The verdict this dispatch just wrote, staged for a later frame's poll.
		// No free slot only happens if three are already in flight; the copy is
		// simply not taken and the skip waits for a newer one.
		deformDispatchSeq++;
		if (!inputsIdle)
			deformLastNonIdleSeq = deformDispatchSeq;
		// Evolve re-arm bookkeeping. Stamps run AFTER evolve, so a stamp
		// write is unassessed by this frame's evolve and arms at this seq; a
		// ring inject runs BEFORE it, so when evolve ran this frame its
		// verdict already covers the inject (arm at seq-1, which that verdict
		// beats). A ring without inject writes pristine zeros - nothing to
		// evolve - and arms nothing.
		if (stampDispatched)
			evolveLastArmSeq = std::max(evolveLastArmSeq, deformDispatchSeq);
		if (ringRan && perFrameData.InjectValid)
			evolveLastArmSeq = std::max(evolveLastArmSeq, evolveNeeded ? deformDispatchSeq - 1 : deformDispatchSeq);
		// Rebase the match set on every executed dispatch: the pass just
		// applied these exact stamps, so drift accumulates only while skipping
		// - bounded by the tolerance.
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

	// After the passes, so it copies the map this frame just wrote - including
	// anything the ring injected into it.
	UpdateTrenchDebugTexture();

	// What the map just written is anchored to. Recorded here because the live
	// values move ahead of it: the origins advance in GetCommonBufferData, and
	// a range or worldspace change rewrites the texel size and the key before
	// the next frame's flush ever sees this content.
	trenchMapOrigin = windowOrigin;
	trenchMapTexel = perFrameData.TexelSize;
	trenchMapWorldspace = activeWorldspace.load(std::memory_order_acquire);
	trenchMapPhysOrigin = mapOrigin;
	trenchMapPrimed = true;

	// Rebind: the dispatch block nulled t101 while the map was a UAV target.
	deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);
}

ID3D11ComputeShader* SnowDeformation::GetExclusionFieldCS()
{
	if (!exclusionFieldCS) {
		logger::debug("Compiling ExclusionFieldCS");
		exclusionFieldCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\ExclusionFieldCS.hlsl", {}, "cs_5_0"));
	}
	return exclusionFieldCS;
}

ID3D11ComputeShader* SnowDeformation::GetBermFieldCS()
{
	if (!bermFieldCS) {
		logger::debug("Compiling BermFieldCS");
		bermFieldCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\BermFieldCS.hlsl", {}, "cs_5_0"));
	}
	return bermFieldCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationRingCS()
{
	if (!deformationRingCS) {
		logger::debug("Compiling DeformationUpdateCS:RingCS");
		deformationRingCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "RingCS"));
	}
	return deformationRingCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationEvolveCS()
{
	if (!deformationEvolveCS) {
		logger::debug("Compiling DeformationUpdateCS:EvolveCS");
		deformationEvolveCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "EvolveCS"));
	}
	return deformationEvolveCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationStampCS()
{
	if (!deformationStampCS) {
		logger::debug("Compiling DeformationUpdateCS:StampCS");
		deformationStampCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "StampCS"));
	}
	return deformationStampCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationStampAllCS()
{
	if (!deformationStampAllCS) {
		logger::debug("Compiling DeformationUpdateCS:StampAllCS");
		deformationStampAllCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "StampAllCS"));
	}
	return deformationStampAllCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationScanEvolveCS()
{
	if (!deformationScanEvolveCS) {
		logger::debug("Compiling DeformationUpdateCS:ScanEvolveCS");
		deformationScanEvolveCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "ScanEvolveCS"));
	}
	return deformationScanEvolveCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationTileArgsCS()
{
	if (!deformationTileArgsCS) {
		logger::debug("Compiling DeformationUpdateCS:TileArgsCS");
		deformationTileArgsCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "TileArgsCS"));
	}
	return deformationTileArgsCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationScanBermCS()
{
	if (!deformationScanBermCS) {
		logger::debug("Compiling DeformationUpdateCS:ScanBermCS");
		deformationScanBermCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "ScanBermCS"));
	}
	return deformationScanBermCS;
}

ID3D11ComputeShader* SnowDeformation::GetBermFieldTiledCS()
{
	if (!bermFieldTiledCS) {
		logger::debug("Compiling BermFieldCS:BermTiledCS");
		bermFieldTiledCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\BermFieldCS.hlsl", {}, "cs_5_0", "BermTiledCS"));
	}
	return bermFieldTiledCS;
}

ID3D11ComputeShader* SnowDeformation::GetUndulationFieldCS()
{
	if (!undulationFieldCS) {
		logger::debug("Compiling UndulationFieldCS");
		undulationFieldCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\UndulationFieldCS.hlsl", {}, "cs_5_0"));
	}
	return undulationFieldCS;
}

void SnowDeformation::UpdateUndulationField()
{
	auto context = globals::d3d::context;
	if (!undulationFieldTexture || !undulationFieldCB || !context) {
		globals::profiler->MarkPassSkipped("SnowDeformation::UndulationField");
		return;
	}

	// Snap the centre so world-texel alignment is identical across rebakes
	// (no swimming); the +-16384 window then covers the shell grid's
	// +-15744 at any offset the snap allows.
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
		globals::profiler->MarkPassSkipped("SnowDeformation::UndulationField");
		return;
	}
	auto* cs = GetUndulationFieldCS();
	if (!cs) {
		globals::profiler->MarkPassSkipped("SnowDeformation::UndulationField");
		return;
	}

	globals::profiler->BeginPass("SnowDeformation::UndulationField");
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

	// A world box -> logical texel bounds (one-texel slop) -> physical
	// segments (wrap split per axis, the CPU's one torus crossing besides
	// CopyLogicalBox) -> deduped tiles.
	auto addWorldBox = [&](float minX, float minY, float maxX, float maxY) {
		int l0[2] = { (int)std::floor((minX - a_data.WindowOrigin.x) / texel) - 1,
			(int)std::floor((minY - a_data.WindowOrigin.y) / texel) - 1 };
		int l1[2] = { (int)std::floor((maxX - a_data.WindowOrigin.x) / texel) + 1,
			(int)std::floor((maxY - a_data.WindowOrigin.y) / texel) + 1 };
		int seg[2][2][2];  // [axis][segment][start,end], physical texels
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

	// Stamp capsules: the shader's own gate radius, mirrored (pit legs reach
	// furthest; melt edge noise widens the rest).
	const float gateScale = std::max(1.31f, 1.0f + a_data.MeltEdgeNoise);
	for (uint i = 0; i < a_data.StampCount && !overflow; i++) {
		const float reach = a_data.Stamps[i].w * gateScale;
		addWorldBox(std::min(a_data.Stamps[i].x, a_data.StampEnds[i].x) - reach,
			std::min(a_data.Stamps[i].y, a_data.StampEnds[i].y) - reach,
			std::max(a_data.Stamps[i].x, a_data.StampEnds[i].x) + reach,
			std::max(a_data.Stamps[i].y, a_data.StampEnds[i].y) + reach);
	}

	// Bow waves: crest forward extent is compressed by Reach in shaped space
	// (world extent 1.45 x r x reach); the wipe reaches 2.1 r laterally.
	if (a_data.DepositParams.x > 0.5f) {
		const uint waveCount = (uint)a_data.DepositParams.x;
		const float reachExtent = std::max(1.45f * std::max(a_data.DepositParams.y, 0.25f), 2.2f);
		for (uint w = 0; w < waveCount && !overflow; w++) {
			const float r = std::max(a_data.DepositShape[w].x, 1e-3f) * reachExtent;
			addWorldBox(std::min(a_data.DepositPosDir[w].x, a_data.DepositShape[w].z) - r,
				std::min(a_data.DepositPosDir[w].y, a_data.DepositShape[w].w) - r,
				std::max(a_data.DepositPosDir[w].x, a_data.DepositShape[w].z) + r,
				std::max(a_data.DepositPosDir[w].y, a_data.DepositShape[w].w) + r);
		}
	}

	return overflow ? UINT32_MAX : (uint32_t)stampTileScratch.size();
}

void SnowDeformation::ClearShaderCache()
{
	// The prime worker owns these members while it runs; a debug toggle in
	// that window no-ops rather than racing it.
	if (snowPrimeState.load(std::memory_order_acquire) == 1)
		return;
	// Rehash the sources on the next compile, so a recompile after a live
	// shader edit cannot serve stale cached bytecode.
	{
		std::scoped_lock lock(snowShaderCacheMutex);
		snowSourcesFingerprint.clear();
	}
	if (deformationRingCS)
		deformationRingCS->Release();
	deformationRingCS = nullptr;
	if (deformationEvolveCS)
		deformationEvolveCS->Release();
	deformationEvolveCS = nullptr;
	if (deformationStampCS)
		deformationStampCS->Release();
	deformationStampCS = nullptr;
	if (deformationStampAllCS)
		deformationStampAllCS->Release();
	deformationStampAllCS = nullptr;
	if (deformationScanEvolveCS)
		deformationScanEvolveCS->Release();
	deformationScanEvolveCS = nullptr;
	if (deformationTileArgsCS)
		deformationTileArgsCS->Release();
	deformationTileArgsCS = nullptr;
	if (deformationScanBermCS)
		deformationScanBermCS->Release();
	deformationScanBermCS = nullptr;
	if (bermFieldTiledCS)
		bermFieldTiledCS->Release();
	bermFieldTiledCS = nullptr;
	if (undulationFieldCS)
		undulationFieldCS->Release();
	undulationFieldCS = nullptr;
	// The next UpdateUndulationField rebakes with the fresh CS.
	undulationFieldValid = false;
	if (bermFieldCS)
		bermFieldCS->Release();
	bermFieldCS = nullptr;
	if (exclusionFieldCS)
		exclusionFieldCS->Release();
	exclusionFieldCS = nullptr;
	if (shellVS)
		shellVS->Release();
	shellVS = nullptr;
	if (shellTessVS)
		shellTessVS->Release();
	shellTessVS = nullptr;
	if (shellHS)
		shellHS->Release();
	shellHS = nullptr;
	if (shellHSNear)
		shellHSNear->Release();
	shellHSNear = nullptr;
	if (shellHSFar)
		shellHSFar->Release();
	shellHSFar = nullptr;
	if (shellPSNoDepth)
		shellPSNoDepth->Release();
	shellPSNoDepth = nullptr;
	if (shellDS)
		shellDS->Release();
	shellDS = nullptr;
	if (shellShadowVS)
		shellShadowVS->Release();
	shellShadowVS = nullptr;
	if (shellPS)
		shellPS->Release();
	shellPS = nullptr;
	if (shellLODPS)
		shellLODPS->Release();
	shellLODPS = nullptr;
	if (depthSyncCS)
		depthSyncCS->Release();
	depthSyncCS = nullptr;
	if (lodProbeCS)
		lodProbeCS->Release();
	lodProbeCS = nullptr;
	if (windowFillCS)
		windowFillCS->Release();
	windowFillCS = nullptr;
	if (staticsVS)
		staticsVS->Release();
	staticsVS = nullptr;
	if (staticsTessVS)
		staticsTessVS->Release();
	staticsTessVS = nullptr;
	if (staticsHS)
		staticsHS->Release();
	staticsHS = nullptr;
	if (staticsDS)
		staticsDS->Release();
	staticsDS = nullptr;
	if (staticsPS)
		staticsPS->Release();
	staticsPS = nullptr;
	if (staticsPSNoDepth)
		staticsPSNoDepth->Release();
	staticsPSNoDepth = nullptr;
	if (patchVS)
		patchVS->Release();
	patchVS = nullptr;
	if (patchShadowVS)
		patchShadowVS->Release();
	patchShadowVS = nullptr;
	if (patchTessVS)
		patchTessVS->Release();
	patchTessVS = nullptr;
	if (patchHS)
		patchHS->Release();
	patchHS = nullptr;
	if (patchDS)
		patchDS->Release();
	patchDS = nullptr;
	if (patchPS)
		patchPS->Release();
	patchPS = nullptr;
	staticsVSBlob = nullptr;
	staticsILCache.clear();
	staticsShadersFailed = false;
	if (smoothAccumulateCS)
		smoothAccumulateCS->Release();
	smoothAccumulateCS = nullptr;
	if (smoothResolveCS)
		smoothResolveCS->Release();
	smoothResolveCS = nullptr;
	if (smoothFlatStatsCS)
		smoothFlatStatsCS->Release();
	smoothFlatStatsCS = nullptr;
	if (heightVS)
		heightVS->Release();
	heightVS = nullptr;
	if (heightPS)
		heightPS->Release();
	heightPS = nullptr;
	if (heightScrollCS)
		heightScrollCS->Release();
	heightScrollCS = nullptr;
	if (heightCombineCS)
		heightCombineCS->Release();
	heightCombineCS = nullptr;
	if (heightConeCS)
		heightConeCS->Release();
	heightConeCS = nullptr;
}

void SnowDeformation::LoadSettings(json& o_json)
{
	settings = o_json;
	// Loaded values may change the window size or map resolution; the apply
	// path is a no-op when they match the current state.
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
	clearRequested = true;
	ClearTrenchStore("restore defaults");
	RefreshLandTextureDepths();
	shellDataDirty.store(true, std::memory_order_release);
}

namespace
{
	// Approximate GPU bytes of a texture from its descriptor. bpp16 = bytes
	// per pixel x16 so block-compressed half-byte formats stay integral.
	uint64_t TextureBytes(ID3D11Texture2D* a_tex)
	{
		if (!a_tex)
			return 0;
		D3D11_TEXTURE2D_DESC desc;
		a_tex->GetDesc(&desc);
		uint64_t bpp16 = 64;
		switch (desc.Format) {
		case DXGI_FORMAT_R8_TYPELESS:
		case DXGI_FORMAT_R8_UNORM:
			bpp16 = 16;
			break;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_UNORM:
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R8G8_UNORM:
			bpp16 = 32;
			break;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32G32_TYPELESS:
		case DXGI_FORMAT_R32G32_FLOAT:
			bpp16 = 128;
			break;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			bpp16 = 256;
			break;
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			bpp16 = 8;
			break;
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			bpp16 = 16;
			break;
		default:
			break;  // 4-byte default (R32/R24G8/R8G8B8A8/R16G16 families)
		}
		uint64_t bytes = (uint64_t)desc.Width * desc.Height * std::max(desc.ArraySize, 1u) * bpp16 / 16;
		if (desc.MipLevels != 1)
			bytes = bytes * 4 / 3;
		return bytes;
	}

	uint64_t SRVBytes(ID3D11ShaderResourceView* a_srv)
	{
		if (!a_srv)
			return 0;
		winrt::com_ptr<ID3D11Resource> res;
		a_srv->GetResource(res.put());
		auto tex = res.try_as<ID3D11Texture2D>();
		return tex ? TextureBytes(tex.get()) : 0;
	}
}

void SnowDeformation::QueryAdapterVRAM(uint64_t& a_usageMB, uint64_t& a_budgetMB)
{
	a_usageMB = 0;
	a_budgetMB = 0;
	static winrt::com_ptr<IDXGIAdapter3> adapter3;
	if (!adapter3 && globals::d3d::device) {
		winrt::com_ptr<IDXGIDevice> dxgiDevice;
		if (SUCCEEDED(globals::d3d::device->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()))) {
			winrt::com_ptr<IDXGIAdapter> adapter;
			if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.put())))
				adapter->QueryInterface(__uuidof(IDXGIAdapter3), adapter3.put_void());
		}
	}
	if (adapter3) {
		DXGI_QUERY_VIDEO_MEMORY_INFO info{};
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
			a_usageMB = info.CurrentUsage >> 20;
			a_budgetMB = info.Budget >> 20;
		}
	}
}

uint64_t SnowDeformation::SumFeatureTextureBytes(std::string& a_breakdown)
{
	auto texOf = [](Texture2D* a_wrap) -> ID3D11Texture2D* {
		return a_wrap ? a_wrap->resource.get() : nullptr;
	};

	const uint64_t deform = TextureBytes(texOf(deformationTextures[0])) + TextureBytes(texOf(deformationTextures[1]));
	const uint64_t terrain = TextureBytes(texOf(shellTerrainTexture));
	const uint64_t heights = TextureBytes(texOf(heightTopRaw[0])) + TextureBytes(texOf(heightTopRaw[1])) +
	                         TextureBytes(texOf(heightBottomRaw[0])) + TextureBytes(texOf(heightBottomRaw[1])) +
	                         TextureBytes(texOf(heightTopFiltered)) + TextureBytes(texOf(heightBottomFiltered)) +
	                         TextureBytes(texOf(heightScratch)) + TextureBytes(texOf(heightSkinDepth));
	const uint64_t shadowCopies = TextureBytes(shadowAtlasCopyTex.get());
	const uint64_t pointCopy = TextureBytes(pointShadowAtlasCopyTex.get());
	const uint64_t snowTex = SRVBytes(shellSnowDiffuseSRV.get()) + SRVBytes(shellSnowNormalSRV.get()) +
	                         SRVBytes(shellSnowRmaosSRV.get()) + SRVBytes(shellSnowHeightSRV.get());

	const uint64_t total = deform + terrain + heights + shadowCopies + pointCopy + snowTex;

	char line[256];
	snprintf(line, sizeof(line), "deform %llu, terrain %llu, heights %llu, sunShadowCopies %llu, pointShadowCopy %llu, snowTex %llu (MB)",
		(unsigned long long)(deform >> 20), (unsigned long long)(terrain >> 20), (unsigned long long)(heights >> 20),
		(unsigned long long)(shadowCopies >> 20), (unsigned long long)(pointCopy >> 20),
		(unsigned long long)(snowTex >> 20));
	a_breakdown = line;
	return total;
}

void SnowDeformation::TickVRAMLog()
{
	vramTickCounter++;
	if (vramTickCounter % 120 != 1)
		return;

	uint64_t usageMB = 0, budgetMB = 0;
	QueryAdapterVRAM(usageMB, budgetMB);
	if (budgetMB == 0)
		return;

	const bool overBudget = usageMB > budgetMB;
	const bool bigDelta = usageMB > vramLastLoggedMB + 256 || vramLastLoggedMB > usageMB + 512;
	const bool cadence = vramTickCounter - vramLastLogTick >= 5400;
	if (!overBudget && !bigDelta && !cadence)
		return;
	vramLastLoggedMB = usageMB;
	vramLastLogTick = vramTickCounter;

	std::string breakdown;
	const uint64_t featureMB = SumFeatureTextureBytes(breakdown) >> 20;
	if (overBudget)
		logger::warn("[SNOW DEFORMATION] VRAM OVER BUDGET: adapter {}/{} MB, feature ~{} MB | {} | driver demotion likely = large persistent FPS loss until game restart",
			usageMB, budgetMB, featureMB, breakdown);
	else
		logger::info("[SNOW DEFORMATION] VRAM: adapter {}/{} MB ({}%), feature ~{} MB | {}",
			usageMB, budgetMB, budgetMB ? usageMB * 100 / budgetMB : 0, featureMB, breakdown);
}
