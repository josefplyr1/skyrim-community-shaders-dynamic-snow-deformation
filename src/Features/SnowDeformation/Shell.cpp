#include "Features/SnowDeformation.h"

#include <DDSTextureLoader.h>

#include "Deferred.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

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
	// and normals, so the field textures must be bound to BOTH stages; the
	// snow maps and scene depth are PS-only. The depth SRV is a copy (Terrain
	// Blending's blended depth when available), never the bound DSV, so
	// sampling it here is legal; the PS fades the shell where it hovers close
	// in front of any geometry so it dissolves into statics (walkways, mesh
	// roads, rocks).
	ID3D11ShaderResourceView* shellSRVs[8] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), shellSnowDiffuseSRV.get(), Util::GetCurrentSceneDepthSRV(false), nullptr, nullptr, shellSnowNormalSRV.get(), shellSnowRmaosSRV.get() };
	context->VSSetShaderResources(0, 4, shellSRVs);
	context->PSSetShaderResources(0, 8, shellSRVs);
	// Glint noise (t20): TruePBR binds this each prepass, but slot 20's state
	// at deferred time is not guaranteed; bind explicitly for this pass.
	if (globals::features::truePBR.glintsNoiseTexture) {
		ID3D11ShaderResourceView* glintSRV = globals::features::truePBR.glintsNoiseTexture->srv.get();
		context->PSSetShaderResources(20, 1, &glintSRV);
	}

	winrt::com_ptr<ID3D11SamplerState> prevSampler;
	context->PSGetSamplers(0, 1, prevSampler.put());
	ID3D11SamplerState* snowSampler = shellSnowSampler.get();
	context->PSSetSamplers(0, 1, &snowSampler);

	context->VSSetShader(vs, nullptr, 0);
	context->PSSetShader(ps, nullptr, 0);

	globals::profiler->BeginPass("SnowDeformation::Shell");
	context->Draw(kShellGridDim * kShellGridDim * 6, 0);
	globals::profiler->EndPass();

	// Restore everything we changed.
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(0, 1, &nullCB);
	context->PSSetConstantBuffers(0, 1, &nullCB);
	ID3D11ShaderResourceView* nullSRVs[8] = {};
	context->VSSetShaderResources(0, 4, nullSRVs);
	context->PSSetShaderResources(0, 8, nullSRVs);
	ID3D11ShaderResourceView* nullGlintSRV = nullptr;
	context->PSSetShaderResources(20, 1, &nullGlintSRV);
	ID3D11SamplerState* restoreSampler = prevSampler.get();
	context->PSSetSamplers(0, 1, &restoreSampler);
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
