#include "Features/SnowDeformation.h"

#include <DDSTextureLoader.h>

#include "Deferred.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

void SnowDeformation::CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
	winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv)
{
	winrt::com_ptr<ID3D11Resource> srcRes;
	a_srcSRV->GetResource(srcRes.put());
	auto srcTex = srcRes.try_as<ID3D11Texture2D>();
	if (!srcTex)
		return;

	D3D11_TEXTURE2D_DESC srcDesc;
	srcTex->GetDesc(&srcDesc);

	bool recreate = !a_tex || !a_srv;
	if (a_tex) {
		D3D11_TEXTURE2D_DESC haveDesc;
		a_tex->GetDesc(&haveDesc);
		recreate |= haveDesc.Width != srcDesc.Width || haveDesc.Height != srcDesc.Height ||
		            haveDesc.ArraySize != srcDesc.ArraySize || haveDesc.Format != srcDesc.Format;
	}
	if (recreate) {
		a_srv = nullptr;
		a_tex = nullptr;

		D3D11_TEXTURE2D_DESC copyDesc = srcDesc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		copyDesc.MiscFlags = 0;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		copyDesc.CPUAccessFlags = 0;
		if (FAILED(globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, a_tex.put())))
			return;
		Util::SetResourceName(a_tex.get(), a_name);

		// Reuse the source SRV's view description so typeless depth formats
		// resolve to the same shader-readable format.
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		a_srcSRV->GetDesc(&srvDesc);
		if (FAILED(globals::d3d::device->CreateShaderResourceView(a_tex.get(), &srvDesc, a_srv.put()))) {
			a_tex = nullptr;
			return;
		}
	}

	globals::d3d::context->CopyResource(a_tex.get(), srcTex.get());
}

/**
 * @brief Lazy-loads the shell snow texture set from the user-configurable
 * path (through the MO2 VFS).
 *
 * The TruePBR variant of the chosen path (Textures\PBR\..., _n / _rmaos
 * companions, linear-color albedo) is probed first. When found, the
 * modlist's own TruePBR config JSON (PBRTextureSets\, matched by texture
 * basename) supplies the authored glint/roughness/specular values.
 */
void SnowDeformation::EnsureShellSnowTextures()
{
	if (shellSnowTextureAttempted)
		return;
	shellSnowTextureAttempted = true;
	shellSnowDiffuseSRV = nullptr;
	shellSnowNormalSRV = nullptr;
	shellSnowRmaosSRV = nullptr;
	shellSnowTextureIsPBR = false;

	auto tryLoadDDS = [](const std::string& a_path, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv) {
		a_srv = nullptr;
		std::wstring wide = L"Data\\" + std::wstring(a_path.begin(), a_path.end());
		return SUCCEEDED(DirectX::CreateDDSTextureFromFile(globals::d3d::device, wide.c_str(), nullptr, a_srv.put()));
	};

	std::string chosenPath = settings.SnowTexturePath.empty() ? "Textures\\Landscape\\snow01.dds" : settings.SnowTexturePath;
	for (auto& pathChar : chosenPath)
		if (pathChar == '/')
			pathChar = '\\';
	std::string loweredPath = chosenPath;
	std::transform(loweredPath.begin(), loweredPath.end(), loweredPath.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });

	std::string pbrPath;
	if (loweredPath.find("\\pbr\\") != std::string::npos || loweredPath.rfind("pbr\\", 0) == 0)
		pbrPath = chosenPath;
	else if (size_t texPos = loweredPath.find("textures\\"); texPos != std::string::npos)
		pbrPath = chosenPath.substr(0, texPos + 9) + "PBR\\" + chosenPath.substr(texPos + 9);

	if (!pbrPath.empty() && pbrPath.size() > 4 && tryLoadDDS(pbrPath, shellSnowDiffuseSRV)) {
		shellSnowTextureIsPBR = true;
		std::string base = pbrPath.substr(0, pbrPath.size() - 4);
		bool hasNormal = tryLoadDDS(base + "_n.dds", shellSnowNormalSRV);
		bool hasRmaos = tryLoadDDS(base + "_rmaos.dds", shellSnowRmaosSRV);
		logger::info("[SNOW DEFORMATION] PBR snow set: {} (normal={} rmaos={})", pbrPath, hasNormal, hasRmaos);

		snowGlintLogDensity = 6.0f;
		snowGlintMicroRoughness = 0.3f;
		snowGlintDensityRandomization = 5.0f;
		snowGlintScreenSpaceScale = 1.0f;
		snowRoughnessScale = 0.7f;
		snowSpecularLevel = 0.02f;
		try {
			size_t slashPos = base.find_last_of('\\');
			std::string baseName = (slashPos == std::string::npos) ? base : base.substr(slashPos + 1);
			std::transform(baseName.begin(), baseName.end(), baseName.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			for (const auto& entry : std::filesystem::directory_iterator("Data\\PBRTextureSets")) {
				if (!entry.is_regular_file())
					continue;
				std::string fname = entry.path().filename().string();
				std::string fnameLower = fname;
				std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(),
					[](unsigned char c) { return (char)std::tolower(c); });
				if (!fnameLower.ends_with(".json") || fnameLower.find(baseName) == std::string::npos)
					continue;
				std::ifstream file(entry.path());
				nlohmann::json cfg = nlohmann::json::parse(file, nullptr, false);
				if (cfg.is_discarded())
					continue;
				if (auto glintIt = cfg.find("glintParameters"); glintIt != cfg.end() && glintIt->is_object()) {
					snowGlintLogDensity = glintIt->value("logMicrofacetDensity", 6.0f);
					// Same clamps Lighting.hlsl applies (PBR::Constants).
					snowGlintMicroRoughness = std::clamp(glintIt->value("microfacetRoughness", 1.0f), 0.005f, 0.3f);
					snowGlintDensityRandomization = std::clamp(glintIt->value("densityRandomization", 5.0f), 0.0f, 5.0f);
					snowGlintScreenSpaceScale = std::max(1.0f, glintIt->value("screenSpaceScale", 1.0f));
					if (!glintIt->value("enabled", true))
						snowGlintLogDensity = 0.0f;  // below the shader's >1.1 gate
				}
				snowRoughnessScale = cfg.value("roughnessScale", 0.7f);
				snowSpecularLevel = cfg.value("specularLevel", 0.02f);
				logger::info("[SNOW DEFORMATION] PBR config matched: {} (glintDensity={:.1f} roughScale={:.2f} spec={:.3f})",
					fname, snowGlintLogDensity, snowRoughnessScale, snowSpecularLevel);
				break;
			}
		} catch (const std::exception& e) {
			logger::info("[SNOW DEFORMATION] PBR config scan failed: {}", e.what());
		}
	} else {
		bool ok = tryLoadDDS(chosenPath, shellSnowDiffuseSRV);
		if (!ok && chosenPath != "Textures\\Landscape\\snow01.dds") {
			logger::info("[SNOW DEFORMATION] Snow diffuse not loose-file loadable: {}", chosenPath);
			chosenPath = "Textures\\Landscape\\snow01.dds";
			ok = tryLoadDDS(chosenPath, shellSnowDiffuseSRV);
		}
		logger::info("[SNOW DEFORMATION] Snow diffuse load ({}): {}", ok ? "ok (legacy)" : "missing, using fallback color", chosenPath);
	}
}

ID3D11VertexShader* SnowDeformation::GetShellVS()
{
	if (!shellVS) {
		logger::debug("Compiling SnowShell VS");
		shellVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return shellVS;
}

ID3D11VertexShader* SnowDeformation::GetShellShadowVS()
{
	if (!shellShadowVS) {
		logger::debug("Compiling SnowShell shadow-cast VS");
		shellShadowVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_SHADOW_CAST", "" } }, "vs_5_0"));
	}
	return shellShadowVS;
}

ID3D11PixelShader* SnowDeformation::GetShellPS()
{
	if (!shellPS) {
		logger::debug("Compiling SnowShell PS");
		shellPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
	}
	return shellPS;
}

ID3D11ComputeShader* SnowDeformation::GetDepthSyncCS()
{
	if (!depthSyncCS) {
		logger::debug("Compiling DepthSyncCS");
		depthSyncCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0"));
	}
	return depthSyncCS;
}

void SnowDeformation::DrawShell()
{
	if (!settings.EnableSnowDeformation)
		return;

	if (!globals::state->inWorld)
		return;

	auto vs = GetShellVS();
	auto ps = GetShellPS();
	if (!vs || !ps)
		return;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& fb = globals::game::frameBufferCached;

	ShellCB cbData{};
	cbData.CameraViewProj = fb.GetCameraViewProj();
	cbData.CameraViewProjUnjittered = fb.GetCameraViewProjUnjittered();
	cbData.CameraPreviousViewProjUnjittered = fb.GetCameraPreviousViewProjUnjittered();
	cbData.CameraView = fb.GetCameraView();
	cbData.CameraPosAdjust = fb.GetCameraPosAdjust();
	cbData.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();

	// The range slider scales the warped grid's inner spacing; the warp
	// shape is unchanged, so range costs only near-field density (8 units
	// at the 375 m default).
	const float shellSpacing = kShellGridSpacing * std::clamp(settings.RangeShellM, 94.0f, 750.0f) * kUnitsPerMeter / ShellWarpedHalfSpan(kShellGridSpacing);
	cbData.GridSpacing = shellSpacing;
	cbData.GridDim = kShellGridDim;
	// The warped grid is camera-centered: snap the center to the grid step
	// so inner vertices stay texel-stable, then offset by the warped span.
	const float warpedHalfSpan = ShellWarpedHalfSpan(shellSpacing);
	cbData.WarpedHalfSpan = warpedHalfSpan;
	cbData.GridOrigin = {
		std::floor(cbData.CameraPosAdjust.x / kShellGridSpacing) * kShellGridSpacing - warpedHalfSpan,
		std::floor(cbData.CameraPosAdjust.y / kShellGridSpacing) * kShellGridSpacing - warpedHalfSpan
	};

	cbData.TerrainTexelSize = kShellVertexSpacing;
	cbData.TerrainDim = kShellWindowDim;
	cbData.ShellDebugData = shellDataDebug;
	cbData.DeformInvWorldSize = 1.0f / deformWorldSize;

	// Keep shader-side sampling math in small grid-local coordinates.
	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	cbData.GridToTerrainOffset = {
		cbData.GridOrigin.x - shellWindowCellX * cellSize,
		cbData.GridOrigin.y - shellWindowCellY * cellSize
	};
	cbData.GridToDeformOffset = {
		cbData.GridOrigin.x - windowOrigin.x,
		cbData.GridOrigin.y - windowOrigin.y
	};

	// Snow uv offset folded to the tile period, so shader-side uv math stays
	// in small numbers (256-unit texture tiling).
	constexpr float kSnowUVTile = 256.0f;
	cbData.SnowUVOffset = {
		std::fmod(cbData.GridOrigin.x, kSnowUVTile),
		std::fmod(cbData.GridOrigin.y, kSnowUVTile)
	};

	EnsureShellSnowTextures();
	cbData.HasSnowTexture = shellSnowDiffuseSRV != nullptr;
	cbData.SnowTextureIsLinear = (shellSnowTextureIsPBR || settings.SnowTextureLinear) ? 1.0f : 0.0f;
	cbData.HasSnowNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	cbData.HasSnowRmaos = shellSnowRmaosSRV ? 1.0f : 0.0f;
	cbData.SnowRoughnessScale = snowRoughnessScale;
	cbData.SnowGlintParams = { snowGlintLogDensity, snowGlintMicroRoughness, snowGlintDensityRandomization, snowGlintScreenSpaceScale };
	cbData.SnowSpecularLevel = snowSpecularLevel;
	// Sparkle: the shell's specular runs TruePBR's glint NDF when its shared
	// noise texture exists (bound to t20 below for the whole pass).
	cbData.EnableGlints = globals::features::truePBR.glintsNoiseTexture ? 1.0f : 0.0f;
	cbData.BorderNoise = settings.SnowBorderNoise;
	cbData.BorderSmooth = settings.SnowBorderSmoothness;
	cbData.BorderTrampledFade = settings.SnowBorderTrampledFade;
	cbData.BorderUntrampledFade = settings.SnowBorderUntrampledFade;
	cbData.SnowSnowFade = settings.SnowSnowFade;
	// Statics-skin distance dissolve: starts at the blend slider, fully gone
	// at the Object Snow capture range (floored one meter past the start so
	// the smoothstep never degenerates when the sliders cross).
	cbData.SkinFadeStart = settings.RangeSkinsFadeM * kUnitsPerMeter;
	cbData.SkinFadeEnd = std::max(settings.RangeSkinsM * kUnitsPerMeter, cbData.SkinFadeStart + kUnitsPerMeter);
	// Field enable gate + window addressing for the t4/t5 samplers; the
	// center is re-uploaded below once the height pass has recentered.
	cbData.ObjectLiftCap = kObjectLiftCap;
	cbData.ObjectHeightCenter = heightWindowCenter;
	cbData.ObjectHeightHalfExtent = kHeightMapHalfExtent;

	// Crisp shadows: full-resolution comparison PCF against the cascade-atlas
	// copies taken at the shadow-mask pass. When the copies are missing this
	// frame, the shader falls back to the blurred VSM path.
	cbData.CrispShadows = (shadowAtlasCopySRV && shadowEsramCopySRV) ? 1.0f : 0.0f;
	// Screen-Space Shadows availability: the feature clears its texture to
	// WHITE every Prepass even when disabled, so multiplying is always safe
	// once the texture exists.
	auto& screenSpaceShadowsFeature = globals::features::screenSpaceShadows;
	cbData.ScreenSpaceShadowsActive = (screenSpaceShadowsFeature.loaded && screenSpaceShadowsFeature.screenSpaceShadowsTexture) ? 1.0f : 0.0f;

	// Shadow-source diagnostics for the settings UI.
	dbgLodDescriptorCount = 0;
	if (auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0]) {
		if (auto* sunShadowLight = shadowSceneNode->GetRuntimeData().sunShadowDirLight) {
			auto& dirLightData = sunShadowLight->GetShadowDirectionalLightRuntimeData();
			dbgLodDescriptorCount = (uint32_t)sunShadowLight->GetRuntimeData().shadowmapDescriptors.size();
			dbgLodEndSplits[0] = dirLightData.endSplitDistances[0];
			dbgLodEndSplits[1] = dirLightData.endSplitDistances[1];
			dbgLodEndSplits[2] = dirLightData.endSplitDistances[2];
		}
	}

	shellCB->Update(cbData);

	// Back up the pipeline state we touch so the composite and later game
	// passes see exactly what they expect.
	winrt::com_ptr<ID3D11RasterizerState> prevRaster;
	winrt::com_ptr<ID3D11DepthStencilState> prevDepth;
	winrt::com_ptr<ID3D11BlendState> prevBlend;
	UINT prevStencilRef = 0;
	FLOAT prevBlendFactor[4]{};
	UINT prevSampleMask = 0xFFFFFFFF;
	D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	context->RSGetState(prevRaster.put());
	context->OMGetDepthStencilState(prevDepth.put(), &prevStencilRef);
	context->OMGetBlendState(prevBlend.put(), prevBlendFactor, &prevSampleMask);
	context->IAGetPrimitiveTopology(&prevTopology);
	D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&prevViewportCount, prevViewports);

	// Rasterize this frame's captured statics top-down into the object
	// height windows, then restore the viewport for the screen-space passes.
	if (EnsureStaticsShaders())
		RenderObjectHeightMap();
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);

	// The height pass recentered its window after the shell CB was filled;
	// sampling the freshly scrolled maps with last frame's center makes the
	// whole field trail the camera by one frame of movement. Re-upload with
	// the current center. (Any CPU value consumed by both a constant buffer
	// and a same-frame-scrolled texture must be uploaded after the scroll.)
	cbData.ObjectHeightCenter = heightWindowCenter;
	shellCB->Update(cbData);

	// Snapshot for next frame's shadow-caster injection (it runs at the
	// shadow-mask pass, before DrawShell recomputes these values).
	if (!lastShellCBData)
		lastShellCBData = std::make_unique<ShellCB>();
	*lastShellCBData = cbData;

	// Bind the deferred G-buffer exactly as StartDeferred configures it,
	// plus the main depth buffer for correct intersection with the world.
	auto& rtData = renderer->GetRuntimeData();
	ID3D11RenderTargetView* rtvs[8] = {
		rtData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV,
		rtData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV,
		rtData.renderTargets[NORMALROUGHNESS].RTV,
		rtData.renderTargets[ALBEDO].RTV,
		rtData.renderTargets[SPECULAR].RTV,
		rtData.renderTargets[REFLECTANCE].RTV,
		rtData.renderTargets[MASKS].RTV,
		rtData.renderTargets[MASKS2].RTV
	};
	auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
	context->OMSetRenderTargets(8, rtvs, dsv);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->RSSetState(shellRasterState.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	ID3D11Buffer* cbs[1] = { shellCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(0, 1, cbs);
	// SharedData (b5) supplies SH ambient + sun for the PS lighting; rebind
	// the b4-b6 triple exactly as Deferred does for its own passes.
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);
	// The PS evaluates the terrain/deformation fields for per-pixel coverage
	// and normals, so the field textures must be bound to both stages; the
	// snow maps and scene depth are PS-only. The depth SRV is a copy (Terrain
	// Blending's blended depth when available), never the bound DSV, so
	// sampling it here is legal; the PS fades the shell where it hovers close
	// in front of any geometry so it dissolves into statics (walkways, mesh
	// roads, rocks).
	ID3D11ShaderResourceView* shellSRVs[8] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), shellSnowDiffuseSRV.get(), Util::GetCurrentSceneDepthSRV(false), heightTopFiltered->srv.get(), heightBottomFiltered->srv.get(), shellSnowNormalSRV.get(), shellSnowRmaosSRV.get() };
	context->VSSetShaderResources(0, 6, shellSRVs);
	context->PSSetShaderResources(0, 8, shellSRVs);
	// Glint noise (t20): TruePBR binds this each prepass, but slot 20's state
	// at deferred time is not guaranteed; bind explicitly for this pass.
	if (globals::features::truePBR.glintsNoiseTexture) {
		ID3D11ShaderResourceView* glintSRV = globals::features::truePBR.glintsNoiseTexture->srv.get();
		context->PSSetShaderResources(20, 1, &glintSRV);
	}
	// Raw shadow-atlas copies (t22/t23) + comparison sampler (s2) for crisp
	// cascade shadows; the statics skin inherits these too.
	if (cbData.CrispShadows > 0.5f) {
		if (!shadowCmpSampler) {
			D3D11_SAMPLER_DESC cmpDesc{};
			cmpDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			cmpDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
			cmpDesc.MaxLOD = D3D11_FLOAT32_MAX;
			globals::d3d::device->CreateSamplerState(&cmpDesc, shadowCmpSampler.put());
			Util::SetResourceName(shadowCmpSampler.get(), "SnowDeformation::ShadowCmpSampler");
		}
		ID3D11ShaderResourceView* shadowSRVs[2] = { shadowAtlasCopySRV.get(), shadowEsramCopySRV.get() };
		context->PSSetShaderResources(22, 2, shadowSRVs);
		ID3D11SamplerState* cmpSampler = shadowCmpSampler.get();
		context->PSSetSamplers(2, 1, &cmpSampler);
	}
	// Screen-Space Shadows output (t45) for the shell + statics passes.
	if (cbData.ScreenSpaceShadowsActive > 0.5f) {
		ID3D11ShaderResourceView* sssSRV = screenSpaceShadowsFeature.screenSpaceShadowsTexture->srv.get();
		context->PSSetShaderResources(45, 1, &sssSRV);
	}

	winrt::com_ptr<ID3D11SamplerState> prevSamplers[2];
	context->PSGetSamplers(0, 1, prevSamplers[0].put());
	context->PSGetSamplers(1, 1, prevSamplers[1].put());
	ID3D11SamplerState* shellSamplers[2] = { shellSnowSampler.get(), shellLinearSampler.get() };
	context->PSSetSamplers(0, 2, shellSamplers);

	context->VSSetShader(vs, nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);

	globals::profiler->BeginPass("SnowDeformation::Shell");
	context->Draw(kShellGridDim * kShellGridDim * 6, 0);
	globals::profiler->EndPass();

	// Post-shell depth copy (Terrain Blending's technique adapted): the main
	// depth now contains the landscape shell's surface. The statics skin
	// samples this at t9 to measure its view-ray gap to the shell and cross-
	// fade into it; fading toward what is actually behind the pixel, which
	// a height-based band cannot guarantee (it can expose the bare mesh
	// beneath the skin instead). Targets must be unbound around CopyResource
	// of a bound DSV.
	{
		auto& mainDepthDS = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (mainDepthDS.depthSRV) {
			ID3D11RenderTargetView* boundRTVs[8] = {};
			ID3D11DepthStencilView* boundDSV = nullptr;
			context->OMGetRenderTargets(8, boundRTVs, &boundDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			CopySRVResource(mainDepthDS.depthSRV, "SnowDeformation::ShellDepthCopy", shellDepthCopyTex, shellDepthCopySRV);
			context->OMSetRenderTargets(8, boundRTVs, boundDSV);
			for (auto* rtv : boundRTVs)
				if (rtv)
					rtv->Release();
			if (boundDSV)
				boundDSV->Release();
		}
		if (shellDepthCopySRV) {
			ID3D11ShaderResourceView* copySRV = shellDepthCopySRV.get();
			context->PSSetShaderResources(9, 1, &copySRV);
		}
	}

	// Captured projected-snow statics, inflated with the same material.
	// Inherits this pass's bindings (b0, t0-t9, s0, b4-b6, RTs, depth).
	DrawCapturedStatics();

	// Restore everything we changed.
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(0, 1, &nullCB);
	context->PSSetConstantBuffers(0, 1, &nullCB);
	ID3D11ShaderResourceView* nullSRVs[10] = {};
	context->VSSetShaderResources(0, 6, nullSRVs);
	context->PSSetShaderResources(0, 10, nullSRVs);
	// t22/t23 hold SRVs of the game's shadow depth targets; they must be
	// unbound before the next shadow render binds those targets as DSVs, or
	// D3D silently drops the binding with warning spam. t20 (glint noise) and
	// t21 are cleared alongside.
	ID3D11ShaderResourceView* nullShadowSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(20, 4, nullShadowSRVs);
	ID3D11SamplerState* restoreSamplers[2] = { prevSamplers[0].get(), prevSamplers[1].get() };
	context->PSSetSamplers(0, 2, restoreSamplers);
	ID3D11SamplerState* nullCmpSampler = nullptr;
	context->PSSetSamplers(2, 1, &nullCmpSampler);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->RSSetState(prevRaster.get());
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);
	context->IASetPrimitiveTopology(prevTopology);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	// Screen-space passes running after us (SSGI) read Terrain Blending's
	// blended depth, finalized during opaque rendering; without a sync they
	// see buried geometry poking through the snow and paint occlusion halos
	// onto the shell. min() the shell's fresh depth into both blended copies
	// (DSV is unbound again at this point).
	auto& tb = globals::features::terrainBlending;
	if (tb.loaded && tb.settings.Enabled && tb.blendedDepthTexture && tb.blendedDepthTexture16) {
		if (auto cs = GetDepthSyncCS()) {
			auto mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
			ID3D11UnorderedAccessView* syncUAVs[2] = { tb.blendedDepthTexture->uav.get(), tb.blendedDepthTexture16->uav.get() };
			context->CSSetShaderResources(0, 1, &mainDepthSRV);
			context->CSSetUnorderedAccessViews(0, 2, syncUAVs, nullptr);
			context->CSSetShader(cs, nullptr, 0);
			const auto& depthDesc = tb.blendedDepthTexture->desc;
			globals::profiler->BeginPass("SnowDeformation::DepthSync");
			context->Dispatch((depthDesc.Width + 7) / 8, (depthDesc.Height + 7) / 8, 1);
			globals::profiler->EndPass();

			ID3D11ShaderResourceView* nullSyncSRV = nullptr;
			ID3D11UnorderedAccessView* nullSyncUAVs[2] = { nullptr, nullptr };
			context->CSSetShaderResources(0, 1, &nullSyncSRV);
			context->CSSetUnorderedAccessViews(0, 2, nullSyncUAVs, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	}
}
