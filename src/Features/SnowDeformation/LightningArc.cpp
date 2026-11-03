#include "Features/SnowDeformation.h"

#include "Deferred.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"

void SnowDeformation::EmitLightningArc(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to)
{
	if (!settings.EnableLightningArcs)
		return;
	if (lightningArcs.size() >= kMaxLightningArcs)
		return;

	LightningArc arc{};
	arc.from = a_from;
	arc.to = a_to;
	arc.age = 0.0f;
	arc.life = std::max(settings.LightningArcLife, 0.02f);
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
		arcVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(
			L"Data\\Shaders\\SnowDeformation\\LightningArc.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return arcVS;
}

ID3D11PixelShader* SnowDeformation::GetLightningArcPS()
{
	if (!arcPS) {
		logger::debug("Compiling LightningArc PS");
		arcPS = static_cast<ID3D11PixelShader*>(CompileSnowShader(
			L"Data\\Shaders\\SnowDeformation\\LightningArc.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
	}
	return arcPS;
}

bool SnowDeformation::EnsureLightningArcResources()
{
	auto* device = globals::d3d::device;
	if (!device)
		return false;

	if (!arcCB)
		arcCB = new ConstantBuffer(ConstantBufferDesc<ArcCB>(), "SnowDeformation::ArcCB");

	if (!arcBlendState) {
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
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = TRUE;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		desc.StencilEnable = FALSE;
		if (FAILED(device->CreateDepthStencilState(&desc, arcDepthState.put())))
			return false;
	}

	if (!arcRasterState) {
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_NONE;
		desc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&desc, arcRasterState.put())))
			return false;
	}

	return GetLightningArcVS() && GetLightningArcPS();
}

void SnowDeformation::DrawLightningArcs()
{
	if (!settings.EnableSnowDeformation || !settings.EnableLightningArcs)
		return;
	if (SnowShadersPending(3))
		return;
	if (lightningArcs.empty())
		return;
	if (!EnsureLightningArcResources())
		return;

	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!context || !renderer)
		return;

	globals::profiler->BeginPass("SnowDeformation::Spells - Lightning Arcs");

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
	context->OMSetBlendState(arcBlendState.get(), blendFactor, 0xFFFFFFFF);

	context->VSSetShader(GetLightningArcVS(), nullptr, 0);
	context->PSSetShader(GetLightningArcPS(), nullptr, 0);

	ID3D11Buffer* cbs[1] = { arcCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(0, 1, cbs);

	auto& fb = globals::game::frameBufferCached;
	ArcCB data{};
	data.CameraViewProj = fb.GetCameraViewProj();
	data.ArcCameraPosAdjust = fb.GetCameraPosAdjust();
	data.ArcTint = { settings.LightningArcTint[0], settings.LightningArcTint[1],
		settings.LightningArcTint[2], 0.0f };

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

	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->RSSetState(prevRaster.get());
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);

	globals::profiler->EndPass();
}
