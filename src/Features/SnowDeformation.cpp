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
	X(TrailIrregularity) \
	X(SlumpRate) \
	X(RefillRateMultiplier) \
	X(RefillOnlyWhenSnowing) \
	X(PersistTrenches) \
	X(StoredTrenchFadeDays) \
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
	X(SnowMeshesDepth) \
	X(RoadMeshesDepth) \
	X(SnowTexturePath) \
	X(SnowTextureLinear) \
	X(TrampleZoneScale) \
	X(TrampleZoneHeight) \
	X(SnowBorderDithering) \
	X(TrenchFloorHeight) \
	X(SnowBorderNoise) \
	X(SnowBorderSmoothness) \
	X(SnowBorderFade) \
	X(SnowSnowFade) \
	X(SnowMoundSteepness) \
	X(UndulationStrength) \
	X(UndulationSpacing) \
	X(Tessellation) \
	X(ReliefDepth) \
	X(ParallaxShadowStrength) \
	X(ParallaxDepth) \
	X(ParallaxSteps) \
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
	X(ObjBermHeight) \
	X(ObjChurnHeight) \
	X(ObjChurnSize) \
	X(RangeTrenchesM) \
	X(RangeSkinsM) \
	X(RangeSkinsFadeM) \
	X(RangeSkinsGeometryM) \
	X(SkinDistantBareness) \
	X(ObjectTrenches) \
	X(DistantSnowLineZ) \
	X(DistantSnowNorthDrop) \
	X(DistantSnowLineFade) \
	X(LODSnowSensitivity) \
	X(HorizonSnow) \
	X(LODReplaceLegacy) \
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

	for (uint i = 0; i < 2; i++) {
		deformationTextures[i] = new Texture2D(texDesc, i == 0 ? "SnowDeformation::DeformationMap0" : "SnowDeformation::DeformationMap1");
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
}

void SnowDeformation::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>(), "SnowDeformation::PerFrame");

	CreateDeformationTextures();

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
	doorsCB = new ConstantBuffer(ConstantBufferDesc<ExclusionsCB>(), "SnowDeformation::ExclusionsCB");

	CreateHeightFieldResources();

	{
		// RT0 MAX (tops) + RT1 MIN (bottoms) + RT2 MAX (skin depth): the
		// extreme surfaces win per texel in any draw order; no depth buffer.
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
			minmaxBlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
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

		pendingScrollDelta.x += (int)std::lround((desiredOrigin.x - windowOrigin.x) / deformTexel);
		pendingScrollDelta.y += (int)std::lround((desiredOrigin.y - windowOrigin.y) / deformTexel);
		windowOrigin = desiredOrigin;
	}

	SettingsGPU data{};
	data.WindowOrigin = windowOrigin;
	data.InvWorldSize = 1.0f / deformWorldSize;
	data.EnableSnowDeformation = settings.EnableSnowDeformation;
	data.DebugTerrainOverlay = (debugTerrainOverlay ? 1u : 0u) | (debugTilingRuler ? 2u : 0u) | (debugProjSnowView ? 4u : 0u) | (debugGlacierView ? 8u : 0u);

	// Horizon snow: LOD terrain only exists beyond the loaded-cell seam
	// (where the shell ends), so the recolor simply applies to all of it â€”
	// including any LOD peeking through under the shell, which then wears
	// the same material and helps hide holes.
	EnsureShellSnowTextures();
	data.LODReplaceStart = 0.0f;
	data.LODReplaceFadeInv = 1.0f / 2048.0f;
	data.LODSnowSensitivity = std::clamp(settings.LODSnowSensitivity, 0.0f, 1.0f);
	data.SnowIsLinear = (shellSnowTextureIsPBR || settings.SnowTextureLinear) ? 1.0f : 0.0f;
	data.SnowRoughnessScale = snowRoughnessScale;
	data.LODReplaceEnable = (settings.EnableSnowDeformation && settings.HorizonSnow && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.SnowHasNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	data.LODReplaceLegacy = settings.LODReplaceLegacy ? 1.0f : 0.0f;
	data.ProjSnowEnable = (settings.EnableSnowDeformation && settings.ProjSnowMatch && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	data.BakedSnowEnable = (settings.EnableSnowDeformation && settings.GlacierSnowMatch && shellSnowDiffuseSRV) ? 1.0f : 0.0f;
	// The world map renders the LOD world without the shell, so a shell-
	// matched recolor there mismatches everything else the map shows
	// (Josef's ON/OFF map pair); skins are gated the same way in
	// DrawCapturedStatics. The map's consistent look is the untouched one.
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

	rangeInitApplied = true;
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

	if (settings.EnableSnowDeformation && globals::state->inWorld)
		UpdateShellTerrainWindow();

	// The overlay keeps the map simulation (stamps, scroll, refill) running
	// while the feature is disabled, so path tracking can be debugged with
	// every visual effect off.
	if (!settings.EnableSnowDeformation && !debugTerrainOverlay)
		return;

	auto ui = globals::game::ui;
	if (ui && ui->GameIsPaused())
		return;

	PerFrame perFrameData{};

	// The window origin was advanced in GetCommonBufferData (during
	// UpdateSharedData); only consume the stored state here.
	perFrameData.ScrollDelta = pendingScrollDelta;
	pendingScrollDelta = { 0, 0 };

	perFrameData.WindowOrigin = windowOrigin;
	perFrameData.TexelSize = deformWorldSize / deformMapDim;
	// Sharpness is a percent slider; 100% clamps just below the degenerate
	// smoothstep(1, 1, x) edge.
	perFrameData.StampFalloffStart = std::clamp(settings.TrenchWallSharpness / 100.0f, 0.0f, 0.98f);
	perFrameData.StampNoiseAmp = std::max(settings.TrailIrregularity, 0.0f);
	perFrameData.SlumpRate = std::clamp(settings.SlumpRate, 0.0f, 1.0f);

	float deltaTime = *globals::game::deltaTime;
	// Refill rate follows the weather's snowfall density; interiors have no
	// snowing weather and do not refill. With RefillOnlyWhenSnowing off the
	// weather is ignored and the baseline rate applies everywhere.
	snowfallIntensity = ComputeSnowfallIntensity();
	float refillIntensity = settings.RefillOnlyWhenSnowing ? snowfallIntensity : 1.0f;
	perFrameData.RefillAmount = deltaTime / kBaseRefillTime * refillIntensity * std::max(settings.RefillRateMultiplier, 0.0f);

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

	perFrameData.ClearMap = clearRequested;
	clearRequested = false;

	// Persistent trenches (ROADMAP #34 Stage A). Ordered against the dispatch:
	// the flush stages what this frame's scroll is about to discard, so it must
	// read the map BEFORE the ping-pong swap below, and it uses the window
	// state the map's contents belong to rather than the live one - a clear
	// arrives here with the worldspace, texel size or both already changed.
	// Clock first: a tile folded in this frame must be stamped with the decay
	// the world has already accrued, and a backwards jump has to drop the store
	// before anything reads it.
	TickTrenchClock();
	SweepTrenchStore();
	DrainTrenchBands();
	FlushDepartingTrenches(perFrameData.ScrollDelta, perFrameData.ClearMap != 0);
	perFrameData.InjectValid = BuildTrenchInject(perFrameData.ScrollDelta, perFrameData.ClearMap != 0);

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

	// Bow-wave crests, deposited into the map's .w so they PERSIST on the
	// ground rather than following the feet that made them (ROADMAP #35).
	// GatherStamps has just sorted them nearest-first, so the slots go to the
	// crests the player can actually see.
	{
		const uint waveCount = std::min((uint)bowWaves.size(), (uint)kMaxBowWaves);
		perFrameData.DepositParams = { settings.BowWaveHeight > 0.001f ? (float)waveCount : 0.0f,
			std::clamp(settings.BowWaveReach, 0.25f, 3.0f),
			std::clamp(settings.BowWaveForward, 0.0f, 1.0f),
			// w retired with the Settle slider (round 8); trench-spoil decay
			// is a fixed clock in the CS now.
			0.0f };
		for (uint i = 0; i < waveCount; i++) {
			const auto& wave = bowWaves[i];
			perFrameData.DepositPosDir[i] = { wave.pos.x, wave.pos.y, wave.dir.x, wave.dir.y };
			perFrameData.DepositShape[i] = { wave.radius, wave.strength, wave.prev.x, wave.prev.y };
		}
	}

	perFrame->Update(perFrameData);

	uint previousTexture = currentTexture;
	currentTexture = 1 - currentTexture;

	{
		ID3D11Buffer* buffers[1] = { perFrame->CB() };
		context->CSSetConstantBuffers(0, 1, buffers);

		ID3D11ShaderResourceView* srvs[] = { deformationTextures[previousTexture]->srv.get(), trenchInjectSRV.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { deformationTextures[currentTexture]->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetDeformationUpdateCS(), nullptr, 0);
		globals::profiler->BeginPass("SnowDeformation::DeformationUpdate");
		context->Dispatch(deformMapDim / 8, deformMapDim / 8, 1);
		globals::profiler->EndPass();
	}

	// Berm bake, reading the map the pass above just wrote. Skipped while the
	// A/B toggle holds the shells on their per-pixel path, so the comparison
	// measures the whole trade and not just the sampling half of it.
	if (!shellBermBakeDisabled && bermFieldTexture) {
		if (auto* bermCS = GetBermFieldCS()) {
			ID3D11ShaderResourceView* bermSrvs[] = { deformationTextures[currentTexture]->srv.get() };
			ID3D11UnorderedAccessView* bermUavs[] = { bermFieldTexture->uav.get() };
			// The freshly written map is still bound as a UAV; a texture cannot
			// be read and written at once, so drop that binding first.
			ID3D11UnorderedAccessView* nullUav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
			context->CSSetShaderResources(0, ARRAYSIZE(bermSrvs), bermSrvs);
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(bermUavs), bermUavs, nullptr);

			context->CSSetShader(bermCS, nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::BermField");
			context->Dispatch(deformMapDim / 8, deformMapDim / 8, 1);
			globals::profiler->EndPass();
		}
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullBuffer);

	ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);

	ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

	// What the map just written is anchored to. Recorded here because the live
	// values move ahead of it: the origin advances in GetCommonBufferData, and
	// a range or worldspace change rewrites the texel size and the key before
	// the next frame's flush ever sees this content.
	trenchMapOrigin = windowOrigin;
	trenchMapTexel = perFrameData.TexelSize;
	trenchMapWorldspace = activeWorldspace.load(std::memory_order_acquire);
	trenchMapPrimed = true;

	// Rebind: after the ping-pong flip this points at the freshly written map.
	deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);
}

ID3D11ComputeShader* SnowDeformation::GetExclusionFieldCS()
{
	if (!exclusionFieldCS) {
		logger::debug("Compiling ExclusionFieldCS");
		exclusionFieldCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\ExclusionFieldCS.hlsl", {}, "cs_5_0"));
	}
	return exclusionFieldCS;
}

ID3D11ComputeShader* SnowDeformation::GetBermFieldCS()
{
	if (!bermFieldCS) {
		logger::debug("Compiling BermFieldCS");
		bermFieldCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\BermFieldCS.hlsl", {}, "cs_5_0"));
	}
	return bermFieldCS;
}

ID3D11ComputeShader* SnowDeformation::GetDeformationUpdateCS()
{
	if (!deformationUpdateCS) {
		logger::debug("Compiling DeformationUpdateCS");
		deformationUpdateCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0"));
	}
	return deformationUpdateCS;
}

void SnowDeformation::ClearShaderCache()
{
	if (deformationUpdateCS)
		deformationUpdateCS->Release();
	deformationUpdateCS = nullptr;
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
	if (patchVS)
		patchVS->Release();
	patchVS = nullptr;
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
	// Loaded values may change the window size; the apply path is a no-op
	// when they match the current state.
	trenchRangeDirty = true;
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
	clearRequested = true;
	ClearTrenchStore();
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
	const uint64_t sceneCopies = TextureBytes(shellDepthCopyTex.get());
	const uint64_t snowTex = SRVBytes(shellSnowDiffuseSRV.get()) + SRVBytes(shellSnowNormalSRV.get()) +
	                         SRVBytes(shellSnowRmaosSRV.get()) + SRVBytes(shellSnowHeightSRV.get());

	const uint64_t total = deform + terrain + heights + shadowCopies + pointCopy + sceneCopies + snowTex;

	char line[256];
	snprintf(line, sizeof(line), "deform %llu, terrain %llu, heights %llu, sunShadowCopies %llu, pointShadowCopy %llu, depthCopy %llu, snowTex %llu (MB)",
		(unsigned long long)(deform >> 20), (unsigned long long)(terrain >> 20), (unsigned long long)(heights >> 20),
		(unsigned long long)(shadowCopies >> 20), (unsigned long long)(pointCopy >> 20),
		(unsigned long long)(sceneCopies >> 20), (unsigned long long)(snowTex >> 20));
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
