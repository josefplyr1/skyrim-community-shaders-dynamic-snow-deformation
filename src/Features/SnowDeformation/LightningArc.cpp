#include "Features/SnowDeformation.h"

#include "Deferred.h"
#include "Features/LightLimitFix.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"

/**
 * The arc a shock cloak throws at the ground it just pocked.
 *
 * Purely visual, and it writes nothing into the game world. That constraint is
 * structural rather than cautious: this feature decides a projectile has
 * detonated by watching it LEAVE the projectile manager, so a real bolt
 * spawned for the look would be seen by our own detector, become a blast, pock
 * the snow, and arc again. A ribbon we draw ourselves cannot feed that loop,
 * and it costs no hit events, no sound, no aggro and no save bloat either.
 *
 * Drawn after the deferred composite, additively into the main colour target,
 * depth-tested against the scene but writing no depth - the ordinary shape of
 * an emissive overlay. Both endpoints already exist: the cloak's discharge
 * picks a spot and the wearer is standing right there, so nothing here has to
 * be guessed or searched for.
 */

void SnowDeformation::EmitLightningArc(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
{
	if (!settings.EnableLightningArcs)
		return;
	// Cosmetic, so a barrage drops the excess rather than growing a list on
	// the render thread.
	if (lightningArcs.size() >= kMaxLightningArcs)
		return;

	LightningArc arc{};
	arc.from = a_from;
	arc.to = a_to;
	arc.age = 0.0f;
	arc.life = std::max(settings.LightningArcLife, 0.02f);
	// Per-arc, so two bolts in the same burst do not fork identically.
	arc.seed = static_cast<float>((++lightningArcSeed * 2654435761u) >> 8 & 0xFFFF) / 65535.0f;
	lightningArcs.push_back(arc);
}

void SnowDeformation::UpdateLightningArcs(float a_deltaTime)
{
	if (lightningArcs.empty())
		return;
	if (!settings.EnableLightningArcs) {
		lightningArcs.clear();
		return;
	}
	for (auto it = lightningArcs.begin(); it != lightningArcs.end();) {
		it->age += a_deltaTime;
		it = it->age >= it->life ? lightningArcs.erase(it) : it + 1;
	}
}

ID3D11VertexShader* SnowDeformation::GetLightningArcVS()
{
	if (!arcVS) {
		logger::debug("Compiling LightningArc VS");
		arcVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\SnowDeformation\\LightningArc.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return arcVS;
}

ID3D11PixelShader* SnowDeformation::GetLightningArcPS()
{
	if (!arcPS) {
		logger::debug("Compiling LightningArc PS");
		arcPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\SnowDeformation\\LightningArc.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
	}
	return arcPS;
}

void SnowDeformation::EnsureLightningArcTexture()
{
	if (arcTextureAttempted && arcTextureLoaded == settings.LightningArcTexturePath)
		return;
	arcTextureAttempted = true;
	arcTextureLoaded = settings.LightningArcTexturePath;
	arcTextureSRV = nullptr;
	if (settings.LightningArcTexturePath.empty())
		return;
	// Through the game's own lookup, so a path inside a BSA resolves and a
	// modlist's retexture still wins - the same route the snow and frost
	// textures take.
	LoadGameDDS(settings.LightningArcTexturePath, arcTextureSRV);
}

bool SnowDeformation::EnsureLightningArcResources()
{
	auto* device = globals::d3d::device;
	if (!device)
		return false;

	if (!arcCB)
		arcCB = new ConstantBuffer(ConstantBufferDesc<ArcCB>(), "SnowDeformation::ArcCB");

	if (!arcBlendState) {
		// Additive, and the alpha channel is left alone: this is light added
		// to a lit scene, not a surface composited over it.
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = TRUE;
		desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&desc, arcBlendState.put())))
			return false;
	}

	if (!arcDepthState) {
		// Tested so the bolt is occluded by the world, but writing nothing -
		// several arcs overlap and each must add rather than mask the last.
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = TRUE;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		// LESS_EQUAL, matching shellDepthState in the same depth buffer. This
		// was GREATER_EQUAL on the assumption that the depth here is reversed;
		// it is not, and the assumption inverted the whole test - the bolt
		// showed where it was BEHIND the world and hid where it was in front,
		// which reads as flickering in and out rather than as a wrong compare.
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.StencilEnable = FALSE;
		if (FAILED(device->CreateDepthStencilState(&desc, arcDepthState.put())))
			return false;
	}

	if (!arcRasterState) {
		// Two-sided: a camera-facing ribbon has no meaningful winding, and
		// culling one side makes it vanish from half the angles it is seen at.
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&desc, arcRasterState.put())))
			return false;
	}

	if (!arcSampler) {
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&desc, arcSampler.put())))
			return false;
	}

	return GetLightningArcVS() && GetLightningArcPS();
}

void SnowDeformation::DrawLightningArcs()
{
	if (!settings.EnableSnowDeformation || !settings.EnableLightningArcs)
		return;
	if (lightningArcs.empty())
		return;
	if (!EnsureLightningArcResources())
		return;

	EnsureLightningArcTexture();

	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	globals::profiler->BeginPass("SnowDeformation::LightningArcs");

	// The composite has already run, so the lit scene is in kMAIN and this is
	// light added on top of it. Depth comes from the main buffer so the bolt
	// is cut off by anything standing in front of it.
	auto& rtData = renderer->GetRuntimeData();
	ID3D11RenderTargetView* rtv = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV;
	auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];

	// Save what the game had bound; this pass runs inside its frame.
	winrt::com_ptr<ID3D11BlendState> prevBlend;
	float prevBlendFactor[4]{};
	UINT prevSampleMask = 0;
	context->OMGetBlendState(prevBlend.put(), prevBlendFactor, &prevSampleMask);
	winrt::com_ptr<ID3D11DepthStencilState> prevDepth;
	UINT prevStencilRef = 0;
	context->OMGetDepthStencilState(prevDepth.put(), &prevStencilRef);
	winrt::com_ptr<ID3D11RasterizerState> prevRaster;
	context->RSGetState(prevRaster.put());

	context->OMSetRenderTargets(1, &rtv, dsv);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->RSSetState(arcRasterState.get());
	context->OMSetDepthStencilState(arcDepthState.get(), 0);
	const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	context->OMSetBlendState(arcBlendState.get(), blendFactor, 0xFFFFFFFF);

	context->VSSetShader(GetLightningArcVS(), nullptr, 0);
	context->PSSetShader(GetLightningArcPS(), nullptr, 0);

	ID3D11Buffer* cbs[1] = { arcCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(0, 1, cbs);

	const bool useTexture = settings.LightningArcUseTexture && arcTextureSRV;
	ID3D11ShaderResourceView* srv = useTexture ? arcTextureSRV.get() : nullptr;
	context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState* samplers[1] = { arcSampler.get() };
	context->PSSetSamplers(0, 1, samplers);

	// The same rebased space the shell draws in: the camera's own position is
	// subtracted before transform, so world coordinates never reach the matrix
	// at a magnitude that costs float precision.
	// CS's own cached frame buffer, exactly as the shell reads it - the matrix
	// and the rebase origin have to be the same pair or the bolt lands beside
	// the snow it is arcing into.
	auto& fb = globals::game::frameBufferCached;
	ArcCB data{};
	data.CameraViewProj = fb.GetCameraViewProj();
	data.ArcCameraPosAdjust = fb.GetCameraPosAdjust();
	data.ArcTint = { settings.LightningArcTint[0], settings.LightningArcTint[1],
		settings.LightningArcTint[2], useTexture ? 1.0f : 0.0f };

	// One draw per arc. There are never many, and the alternative is an array
	// in the constant buffer for no gain.
	for (const auto& arc : lightningArcs) {
		data.ArcFrom = { arc.from.x, arc.from.y, arc.from.z, 0.0f };
		data.ArcTo = { arc.to.x, arc.to.y, arc.to.z, 0.0f };
		data.ArcParams = { std::max(settings.LightningArcWidth, 0.5f),
			std::max(settings.LightningArcBrightness, 0.0f),
			std::clamp(arc.age / std::max(arc.life, 1e-3f), 0.0f, 1.0f),
			arc.seed };
		arcCB->Update(data);
		context->Draw(kLightningArcSegments * 6, 0);
	}

	// Leave the pipeline as it was found.
	ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
	context->PSSetShaderResources(0, 1, nullSRV);
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->RSSetState(prevRaster.get());
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);

	globals::profiler->EndPass();
}

/**
 * Airborne powder off the bow wave (ROADMAP #35 phase two).
 *
 * The arc's vehicle, alpha-blended and lit - see SnowMist.hlsl for the three
 * rules this obeys and why each one exists. It rides the SAME bowWaves list
 * the crest is written from, so the powder is anchored to real pushed snow
 * rather than to a guess: that is the whole difference between this and the
 * retired Stage 4 spray, which modelled emission with nothing pushing.
 */

ID3D11VertexShader* SnowDeformation::GetSnowMistVS()
{
	if (!mistVS) {
		logger::debug("Compiling SnowMist VS");
		mistVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(
			L"Data\\Shaders\\SnowDeformation\\SnowMist.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return mistVS;
}

ID3D11PixelShader* SnowDeformation::GetSnowMistPS()
{
	if (!mistPS) {
		logger::debug("Compiling SnowMist PS");
		mistPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(
			L"Data\\Shaders\\SnowDeformation\\SnowMist.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
	}
	return mistPS;
}

bool SnowDeformation::EnsureSnowMistResources()
{
	auto* device = globals::d3d::device;
	if (!device)
		return false;

	if (!mistCB)
		mistCB = new ConstantBuffer(ConstantBufferDesc<MistCB>(), "SnowDeformation::MistCB");

	if (!mistBlendState) {
		// Ordinary translucency, unlike the arc's additive: powder is a
		// surface in the air, not light added to the scene.
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].BlendEnable = TRUE;
		desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
		desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
		desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&desc, mistBlendState.put())))
			return false;
	}

	// Depth, raster and sampler states are the arc's: same pass, same needs.
	return EnsureLightningArcResources() && GetSnowMistVS() && GetSnowMistPS();
}

void SnowDeformation::DrawSnowMist()
{
	if (!settings.EnableSnowDeformation || !settings.EnableSnowMist)
		return;
	if (bowWaves.empty() || settings.BowWaveHeight <= 0.001f)
		return;
	if (!EnsureSnowMistResources())
		return;

	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	auto* state = globals::state;
	if (!context || !renderer || !state)
		return;

	globals::profiler->BeginPass("SnowDeformation::SnowMist");

	auto& rtData = renderer->GetRuntimeData();
	ID3D11RenderTargetView* rtv = rtData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV;
	auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];

	winrt::com_ptr<ID3D11BlendState> prevBlend;
	float prevBlendFactor[4]{};
	UINT prevSampleMask = 0;
	context->OMGetBlendState(prevBlend.put(), prevBlendFactor, &prevSampleMask);
	winrt::com_ptr<ID3D11DepthStencilState> prevDepth;
	UINT prevStencilRef = 0;
	context->OMGetDepthStencilState(prevDepth.put(), &prevStencilRef);
	winrt::com_ptr<ID3D11RasterizerState> prevRaster;
	context->RSGetState(prevRaster.put());

	context->OMSetRenderTargets(1, &rtv, dsv);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->RSSetState(arcRasterState.get());
	context->OMSetDepthStencilState(arcDepthState.get(), 0);
	const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	context->OMSetBlendState(mistBlendState.get(), blendFactor, 0xFFFFFFFF);

	context->VSSetShader(GetSnowMistVS(), nullptr, 0);
	context->PSSetShader(GetSnowMistPS(), nullptr, 0);

	// Shared CS buffers (b4-6) so the PS reads SharedData's sun and ambient,
	// same slots and same reasoning as the shell pass.
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);

	auto& lightLimitFix = globals::features::lightLimitFix;
	const bool crisp = shadowAtlasCopySRV != nullptr;
	if (crisp) {
		ID3D11ShaderResourceView* shadowAtlasSRV = shadowAtlasCopySRV.get();
		context->PSSetShaderResources(22, 1, &shadowAtlasSRV);
		ID3D11SamplerState* cmpSampler = shadowCmpSampler.get();
		context->PSSetSamplers(2, 1, &cmpSampler);
	}
	const bool pointLights = lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightIndexList && lightLimitFix.lightGrid;
	if (pointLights) {
		ID3D11ShaderResourceView* lightSRVs[3] = { lightLimitFix.lights->srv.get(), lightLimitFix.lightIndexList->srv.get(), lightLimitFix.lightGrid->srv.get() };
		context->PSSetShaderResources(35, 3, lightSRVs);
	}

	auto& fb = globals::game::frameBufferCached;
	MistCB data{};
	data.CameraViewProj = fb.GetCameraViewProj();
	data.MistCameraPosAdjust = fb.GetCameraPosAdjust();

	const uint count = std::min((uint)bowWaves.size(), (uint)kMaxBowWaves);
	data.MistParams = { (float)count,
		std::clamp(settings.MistAmount, 0.0f, 2.0f),
		std::clamp(settings.MistHeight, 0.0f, 3.0f),
		std::clamp(settings.MistBrightness, 0.0f, 4.0f) };
	data.MistShape = { std::clamp(settings.BowWaveReach, 0.25f, 3.0f),
		std::clamp(settings.BowWaveForward, 0.0f, 1.0f),
		1.0f, crisp ? 1.0f : 0.0f };
	data.MistSlices = { (float)sunCascadeSlice[0], (float)sunCascadeSlice[1],
		pointLights ? 1.0f : 0.0f, 0.0f };
	for (uint i = 0; i < count; i++) {
		const auto& wave = bowWaves[i];
		data.MistFootRad[i] = { wave.pos.x, wave.pos.y, wave.z, wave.radius };
		data.MistDirStr[i] = { wave.dir.x, wave.dir.y, wave.strength, 0.0f };
	}
	mistCB->Update(data);

	ID3D11Buffer* cbs[1] = { mistCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(0, 1, cbs);

	context->Draw(count * kMistSprites * 6, 0);

	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	context->PSSetShaderResources(22, 1, nullSRVs);
	// t35-37 stay bound by the same rule as the shell pass.
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->RSSetState(prevRaster.get());
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);

	globals::profiler->EndPass();
}
