// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void SnowDeformation::CaptureShadowAtlas()
{
	if (!settings.EnableSnowDeformation || !globals::state->HasDirectionalShadows()) {
		shadowAtlasLiveSRV = nullptr;
		return;
	}

	winrt::com_ptr<ID3D11ShaderResourceView> liveAtlasSRV;
	globals::d3d::context->PSGetShaderResources(4, 1, liveAtlasSRV.put());

	D3D11_TEXTURE2D_DESC atlasDesc{};
	if (liveAtlasSRV) {
		winrt::com_ptr<ID3D11Resource> liveRes;
		liveAtlasSRV->GetResource(liveRes.put());
		if (auto liveTex = liveRes.try_as<ID3D11Texture2D>()) {
			liveTex->GetDesc(&atlasDesc);
			dbgLodAtlasSlices = atlasDesc.ArraySize;
		}
		RecordSunCascadeSlices(atlasDesc.ArraySize);
		if (settings.SnowCastsShadows)
			InjectShellShadowCasters(liveAtlasSRV.get());

		shadowAtlasLiveSRV = liveAtlasSRV;
	}

	static uint32_t shadowLogCount = 0;
	static int lastValidState = -1;
	const int validState = ShadowAtlasSRV() ? 1 : 0;
	if ((shadowLogCount < 6 || validState != lastValidState) && shadowLogCount < 40) {
		shadowLogCount++;
		lastValidState = validState;
		D3D11_SHADER_RESOURCE_VIEW_DESC liveViewDesc{};
		if (liveAtlasSRV)
			liveAtlasSRV->GetDesc(&liveViewDesc);
		logger::info("[SNOW DEFORMATION] ShadowAtlas: atlasLive={} valid={} atlas={}x{} slices={} fmt={} samples={} liveViewDim={} liveViewFmt={}",
			liveAtlasSRV != nullptr, validState,
			atlasDesc.Width, atlasDesc.Height, atlasDesc.ArraySize, uint32_t(atlasDesc.Format), atlasDesc.SampleDesc.Count,
			uint32_t(liveViewDesc.ViewDimension), uint32_t(liveViewDesc.Format));
	}
}

void SnowDeformation::CapturePointShadowMask()
{
	if (!settings.EnableSnowDeformation || REL::Module::IsVR())
		return;

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return;

	auto context = globals::d3d::context;
	winrt::com_ptr<ID3D11ShaderResourceView> liveAtlasSRV;
	context->PSGetShaderResources(4, 1, liveAtlasSRV.put());
	if (!liveAtlasSRV)
		return;
	winrt::com_ptr<ID3D11Resource> liveRes;
	liveAtlasSRV->GetResource(liveRes.put());
	auto liveTex = liveRes.try_as<ID3D11Texture2D>();
	if (!liveTex)
		return;
	D3D11_TEXTURE2D_DESC liveDesc;
	liveTex->GetDesc(&liveDesc);
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	liveAtlasSRV->GetDesc(&srvDesc);
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;

	if (pointShadowLiveTex != liveTex) {
		pointShadowLiveSRV = nullptr;
		pointShadowLiveTex = nullptr;
		srvDesc.Texture2DArray.ArraySize = liveDesc.ArraySize;
		if (FAILED(globals::d3d::device->CreateShaderResourceView(liveTex.get(), &srvDesc, pointShadowLiveSRV.put())))
			return;
		pointShadowLiveTex = liveTex;
	}

	for (auto& lightPtr : shadowSceneNode->GetRuntimeData().activeShadowLights) {
		auto* bsLight = lightPtr.get();
		if (!bsLight || !bsLight->IsShadowLight())
			continue;
		auto* light = static_cast<RE::BSShadowLight*>(bsLight);
		const bool isFrustum = light->GetIsFrustumLight();
		const bool isParabolic = light->GetIsParabolicLight();
		if (!isFrustum && !isParabolic)
			continue;
		auto& lightRuntime = light->GetRuntimeData();
		const uint32_t maskIndex = lightRuntime.maskIndex;
		if (maskIndex >= kPointShadowMaxLights)
			continue;
		if (lightRuntime.shadowmapDescriptors.empty())
			continue;
		auto& desc = lightRuntime.shadowmapDescriptors[0];
		if (!desc.isEnabled || desc.renderTarget == static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(-1))
			continue;
		if (desc.shadowmapIndex >= liveDesc.ArraySize)
			continue;

		ID3D11Texture2D* lightTargetTex = nullptr;
		if (auto* gameRenderer = globals::game::renderer) {
			auto& dsData = gameRenderer->GetDepthStencilData().depthStencils[desc.renderTarget];
			lightTargetTex = reinterpret_cast<ID3D11Texture2D*>(dsData.texture);
		}
		if (lightTargetTex != liveTex.get())
			continue;

		PointShadowLightData& dd = pendingPointShadows[maskIndex];
		auto proj = DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(&desc.lightTransform));
		DirectX::XMStoreFloat4x4(&dd.LightTransform, proj);
		dd.SliceIndex = desc.shadowmapIndex;
		dd.LightType = isFrustum ? 1u : (light->GetIsOmniLight() ? 3u : 2u);
		dd.ShadowRadius = light->light ? light->light->GetLightRuntimeData().radius.x : 0.0f;

		static uint32_t layoutLogCount = 0;
		if (layoutLogCount < 12) {
			layoutLogCount++;
			logger::info("[SNOW DEFORMATION] PointShadow: mask={} type={} srcSlice={} rt={} arr={} biasScale={:.3f}",
				maskIndex, dd.LightType, desc.shadowmapIndex, uint32_t(desc.renderTarget), liveDesc.ArraySize, lightRuntime.shadowBiasScale);
		}
	}
}

void SnowDeformation::UpdatePointShadowLights()
{
	auto context = globals::d3d::context;

	if (!pointShadowLights) {
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PointShadowLightData);
		sbDesc.ByteWidth = sizeof(PointShadowLightData) * kPointShadowMaxLights;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = kPointShadowMaxLights;

		pointShadowLights = new Buffer(sbDesc, nullptr, "SnowDeformation::PointShadowLights");
		pointShadowLights->CreateSRV(srvDesc);
	}

	PointShadowLightData table[kPointShadowMaxLights]{};
	if (PointShadowAtlasSRV())
		memcpy(table, pendingPointShadows, sizeof(table));

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (SUCCEEDED(context->Map(pointShadowLights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		memcpy(mapped.pData, table, sizeof(table));
		context->Unmap(pointShadowLights->resource.get(), 0);
	}
}

void SnowDeformation::RecordSunCascadeSlices(uint32_t a_atlasSlices)
{
	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sunShadowLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunShadowLight || a_atlasSlices == 0)
		return;
	const auto& descriptors = sunShadowLight->GetRuntimeData().shadowmapDescriptors;
	const uint32_t count = std::min<uint32_t>((uint32_t)descriptors.size(), 2u);
	bool fallback = false;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t slice = descriptors[i].shadowmapIndex;
		if (slice >= a_atlasSlices) {
			slice = i;
			fallback = true;
		}
		if (slice < a_atlasSlices)
			sunCascadeSlice[i] = slice;
	}
	const uint32_t raw0 = count > 0 ? descriptors[0].shadowmapIndex : 0xFFu;
	const uint32_t raw1 = count > 1 ? descriptors[1].shadowmapIndex : 0xFFu;
	const uint32_t packed = (sunCascadeSlice[0] & 0xFF) | ((sunCascadeSlice[1] & 0xFF) << 8) | ((raw0 & 0xFF) << 16) | ((raw1 & 0xFF) << 24);
	static uint32_t lastPacked = 0xFFFFFFFFu;
	static uint32_t changeLogCount = 0;
	if (packed != lastPacked && changeLogCount < 200) {
		lastPacked = packed;
		changeLogCount++;
		logger::info("[SNOW DEFORMATION] Sun cascades: slices used = {}, {} (raw shadowmapIndex = {}, {}; descriptors = {}; atlas has {}; fallback = {})",
			sunCascadeSlice[0], sunCascadeSlice[1], raw0, raw1, (uint32_t)descriptors.size(), a_atlasSlices, fallback);
	}
}

void SnowDeformation::InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV)
{
	if (SnowShadersPending(2))
		return;
	static std::unordered_set<std::string> injectLogged;
	auto logOnce = [](const char* a_msg) {
		if (injectLogged.size() < 12 && injectLogged.insert(a_msg).second)
			logger::info("[SNOW DEFORMATION] ShadowInject: {}", a_msg);
	};
	auto skipPatch = [&](const char* a_msg) {
		logOnce(a_msg);
		globals::profiler->MarkPassSkipped("SnowDeformation::Shadows - Trench Patch");
	};

	if (!settings.EnableSnowDeformation || !lastShellCBData)
		return skipPatch("skip: disabled or no ShellCB snapshot yet");
	if (settings.RoadMeshesDepth <= 0.5f || capturedRoads == 0 || !heightSkinDepth || !heightSkinDepth->srv ||
		!heightTopRaw[heightCurrent] || !heightTopRaw[heightCurrent]->srv)
		return skipPatch("skip: no road meshes captured");
	if (!ShellFootprintHasSnow() || GroundShellsSuspended())
		return skipPatch("skip: no snow depth within the footprint");
	if (!shellDrawGroundGated && !shellViewGateOff)
		return skipPatch("skip: no snow in view");
	auto* patchCastVS = GetPatchShadowVS();
	if (!patchCastVS)
		return skipPatch("skip: patch shadow VS unavailable");
	if (!EnsurePatchCasterIB())
		return skipPatch("skip: patch caster index buffer unavailable");

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sunShadowLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunShadowLight)
		return skipPatch("skip: no sun shadow light");
	auto& lightRuntime = sunShadowLight->GetRuntimeData();
	const uint32_t cascadeCount = std::min<uint32_t>((uint32_t)lightRuntime.shadowmapDescriptors.size(), 4u);
	if (cascadeCount == 0)
		return skipPatch("skip: zero cascades");

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	winrt::com_ptr<ID3D11Resource> atlasRes;
	a_atlasSRV->GetResource(atlasRes.put());
	auto atlasTex = atlasRes.try_as<ID3D11Texture2D>();
	if (!atlasTex) {
		return skipPatch("skip: atlas SRV resource is not a Texture2D");
	}
	D3D11_TEXTURE2D_DESC atlasDesc;
	atlasTex->GetDesc(&atlasDesc);
	if (!(atlasDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
		return skipPatch("skip: atlas lacks DEPTH_STENCIL bind");
	}
	DXGI_FORMAT dsvFormat;
	switch (atlasDesc.Format) {
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		dsvFormat = DXGI_FORMAT_D16_UNORM;
		break;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		break;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
		dsvFormat = DXGI_FORMAT_D32_FLOAT;
		break;
	default:
		return skipPatch("skip: atlas format");
	}
	for (uint32_t i = 0; i < cascadeCount; i++) {
		uint32_t slice = lightRuntime.shadowmapDescriptors[i].shadowmapIndex;
		if (slice >= atlasDesc.ArraySize) {
			slice = i;
			if (slice >= atlasDesc.ArraySize)
				continue;
		}
		if (shadowAtlasDSVTexture == atlasTex.get() && shadowAtlasDSVSlice[i] == slice && shadowAtlasDSV[i])
			continue;
		shadowAtlasDSV[i] = nullptr;
		shadowAtlasDSVSlice[i] = slice;
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		dsvDesc.Format = dsvFormat;
		dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
		dsvDesc.Texture2DArray.MipSlice = 0;
		dsvDesc.Texture2DArray.FirstArraySlice = slice;
		dsvDesc.Texture2DArray.ArraySize = 1;
		device->CreateDepthStencilView(atlasTex.get(), &dsvDesc, shadowAtlasDSV[i].put());
		if (shadowAtlasDSV[i])
			Util::SetResourceName(shadowAtlasDSV[i].get(), "SnowDeformation::ShadowAtlas DSV");
	}
	shadowAtlasDSVTexture = atlasTex.get();
	if (!shadowAtlasDSV[0]) {
		return skipPatch("skip: DSV creation failed");
	}
	if (!shadowCastRS) {
		D3D11_RASTERIZER_DESC rsDesc{};
		rsDesc.FillMode = D3D11_FILL_SOLID;
		rsDesc.CullMode = D3D11_CULL_NONE;
		rsDesc.DepthClipEnable = TRUE;
		device->CreateRasterizerState(&rsDesc, shadowCastRS.put());
		Util::SetResourceName(shadowCastRS.get(), "SnowDeformation::ShadowCastRS");
	}
	if (!shadowCastDSS) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
		device->CreateDepthStencilState(&dsDesc, shadowCastDSS.put());
		Util::SetResourceName(shadowCastDSS.get(), "SnowDeformation::ShadowCastDSS");
	}
	if (!shadowCastRS || !shadowCastDSS) {
		return skipPatch("skip: RS/DSS creation failed");
	}
	logOnce("injecting: trench patch into the sun cascades");

	winrt::com_ptr<ID3D11RenderTargetView> prevRTVs[8];
	winrt::com_ptr<ID3D11DepthStencilView> prevDSV;
	{
		ID3D11RenderTargetView* rtvs[8] = {};
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(8, rtvs, &dsv);
		for (uint32_t i = 0; i < 8; i++)
			prevRTVs[i].attach(rtvs[i]);
		prevDSV.attach(dsv);
	}
	UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	context->RSGetViewports(&prevViewportCount, prevViewports);
	winrt::com_ptr<ID3D11RasterizerState> prevRS;
	context->RSGetState(prevRS.put());
	winrt::com_ptr<ID3D11DepthStencilState> prevDSS;
	UINT prevStencilRef = 0;
	context->OMGetDepthStencilState(prevDSS.put(), &prevStencilRef);
	winrt::com_ptr<ID3D11VertexShader> prevVS;
	context->VSGetShader(prevVS.put(), nullptr, nullptr);
	winrt::com_ptr<ID3D11PixelShader> prevPS;
	context->PSGetShader(prevPS.put(), nullptr, nullptr);
	D3D11_PRIMITIVE_TOPOLOGY prevTopology;
	context->IAGetPrimitiveTopology(&prevTopology);
	winrt::com_ptr<ID3D11InputLayout> prevLayout;
	context->IAGetInputLayout(prevLayout.put());
	winrt::com_ptr<ID3D11Buffer> prevVSCBs[2];
	{
		ID3D11Buffer* cbs[2] = {};
		context->VSGetConstantBuffers(0, 2, cbs);
		prevVSCBs[0].attach(cbs[0]);
		prevVSCBs[1].attach(cbs[1]);
	}
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVs[2];
	{
		ID3D11ShaderResourceView* srvs[2] = {};
		context->VSGetShaderResources(0, 2, srvs);
		for (uint32_t i = 0; i < 2; i++)
			prevVSSRVs[i].attach(srvs[i]);
	}
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVsSkin[4];
	{
		ID3D11ShaderResourceView* srvs[4] = {};
		context->VSGetShaderResources(10, 4, srvs);
		for (uint32_t i = 0; i < 4; i++)
			prevVSSRVsSkin[i].attach(srvs[i]);
	}
	winrt::com_ptr<ID3D11Buffer> prevVB;
	UINT prevVBStride = 0, prevVBOffset = 0;
	{
		ID3D11Buffer* vb = nullptr;
		context->IAGetVertexBuffers(0, 1, &vb, &prevVBStride, &prevVBOffset);
		prevVB.attach(vb);
	}
	winrt::com_ptr<ID3D11Buffer> prevIB;
	DXGI_FORMAT prevIBFormat = DXGI_FORMAT_UNKNOWN;
	UINT prevIBOffset = 0;
	{
		ID3D11Buffer* ib = nullptr;
		context->IAGetIndexBuffer(&ib, &prevIBFormat, &prevIBOffset);
		prevIB.attach(ib);
	}
	winrt::com_ptr<ID3D11ShaderResourceView> prevPSSRV4;
	{
		ID3D11ShaderResourceView* srv = nullptr;
		context->PSGetShaderResources(4, 1, &srv);
		prevPSSRV4.attach(srv);
	}

	context->PSSetShader(nullptr, nullptr, 0);
	context->RSSetState(shadowCastRS.get());
	context->OMSetDepthStencilState(shadowCastDSS.get(), 0);
	D3D11_VIEWPORT atlasViewport{ 0.0f, 0.0f, float(atlasDesc.Width), float(atlasDesc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &atlasViewport);
	ID3D11ShaderResourceView* nullPSSRV = nullptr;
	context->PSSetShaderResources(4, 1, &nullPSSRV);

	ID3D11ShaderResourceView* vsSRVs[2] = { shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr, GetDeformationSRV() };
	context->VSSetShaderResources(0, 2, vsSRVs);
	ID3D11ShaderResourceView* bermSRV = GetBermFieldSRV();
	context->VSSetShaderResources(14, 1, &bermSRV);
	ID3D11ShaderResourceView* undulationSRV = GetUndulationFieldSRV();
	context->VSSetShaderResources(29, 1, &undulationSRV);
	ID3D11Buffer* cb0 = shellCB->CB();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	auto bindCascade = [&](uint32_t a_cascade) {
		DirectX::XMMATRIX uvProj = DirectX::XMLoadFloat4x4(
			reinterpret_cast<const DirectX::XMFLOAT4X4*>(&lightRuntime.shadowmapDescriptors[a_cascade].lightTransform));
		constexpr float kCasterDepthPush = 0.0008f;
		DirectX::XMMATRIX uvToClip = DirectX::XMMatrixSet(
			2.0f, 0.0f, 0.0f, 0.0f,
			0.0f, -2.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			-1.0f, 1.0f, kCasterDepthPush, 1.0f);
		DirectX::XMMATRIX clip = DirectX::XMMatrixMultiply(uvProj, uvToClip);

		ShellCB shadowCB = *lastShellCBData;
		RefreshShellGridPlacement(shadowCB);
		shadowCB.CameraViewProj = DirectX::XMMatrixTranspose(clip);
		shadowCB.CameraPosAdjust = { 0.0f, 0.0f, 0.0f, 0.0f };
		shadowCB.CameraPreviousPosAdjust = { 0.0f, 0.0f, 0.0f, 0.0f };
		shellCB->Update(shadowCB);

		context->OMSetRenderTargets(0, nullptr, shadowAtlasDSV[a_cascade].get());
		return shadowCB;
	};

	{
		ID3D11Buffer* nullCB1 = nullptr;
		context->VSSetConstantBuffers(1, 1, &nullCB1);
		context->IASetInputLayout(nullptr);
		ID3D11Buffer* nullVB = nullptr;
		UINT zero = 0;
		context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	}
	{
		globals::profiler->BeginPass("SnowDeformation::Shadows - Trench Patch");
		context->VSSetShader(patchCastVS, nullptr, 0);
		context->IASetIndexBuffer(patchCasterIB.get(), DXGI_FORMAT_R32_UINT, 0);
		ID3D11Buffer* patchCB1 = staticsCB->CB();
		context->VSSetConstantBuffers(1, 1, &patchCB1);
		ID3D11ShaderResourceView* patchCastSRVs[3] = {
			heightTopRaw[heightCurrent]->srv.get(),
			heightSkinDepth->srv.get(),
			objectSnowCone ? objectSnowCone->srv.get() : nullptr
		};
		context->VSSetShaderResources(11, 3, patchCastSRVs);
		ID3D11ShaderResourceView* patchCastFarSRV = (farHalfExtentLive > 0.0f && heightTopRawFar && heightTopRawFar->srv) ? heightTopRawFar->srv.get() : nullptr;
		context->VSSetShaderResources(40, 1, &patchCastFarSRV);
		StaticsCB pscb{};
		FillPatchDrawCB(pscb);
		staticsCB->Update(pscb);
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++) {
			if (!shadowAtlasDSV[cascade])
				continue;
			bindCascade(cascade);
			context->DrawIndexed(patchCasterIndexCount, 0, 0);
		}
		globals::profiler->EndPass();
	}

	shellCB->Update(*lastShellCBData);
	{
		ID3D11RenderTargetView* rtvs[8];
		for (uint32_t i = 0; i < 8; i++)
			rtvs[i] = prevRTVs[i].get();
		context->OMSetRenderTargets(8, rtvs, prevDSV.get());
	}
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);
	context->RSSetState(prevRS.get());
	context->OMSetDepthStencilState(prevDSS.get(), prevStencilRef);
	context->VSSetShader(prevVS.get(), nullptr, 0);
	context->PSSetShader(prevPS.get(), nullptr, 0);
	context->IASetPrimitiveTopology(prevTopology);
	context->IASetInputLayout(prevLayout.get());
	{
		ID3D11Buffer* cbs[2] = { prevVSCBs[0].get(), prevVSCBs[1].get() };
		context->VSSetConstantBuffers(0, 2, cbs);
	}
	{
		ID3D11ShaderResourceView* srvs[2];
		for (uint32_t i = 0; i < 2; i++)
			srvs[i] = prevVSSRVs[i].get();
		context->VSSetShaderResources(0, 2, srvs);
		ID3D11ShaderResourceView* nullBermSRV = nullptr;
		context->VSSetShaderResources(14, 1, &nullBermSRV);
		context->VSSetShaderResources(29, 1, &nullBermSRV);
		ID3D11ShaderResourceView* skinSrvs[4];
		for (uint32_t i = 0; i < 4; i++)
			skinSrvs[i] = prevVSSRVsSkin[i].get();
		context->VSSetShaderResources(10, 4, skinSrvs);
	}
	{
		ID3D11Buffer* vb = prevVB.get();
		context->IASetVertexBuffers(0, 1, &vb, &prevVBStride, &prevVBOffset);
	}
	context->IASetIndexBuffer(prevIB.get(), prevIBFormat, prevIBOffset);
	{
		ID3D11ShaderResourceView* srv = prevPSSRV4.get();
		context->PSSetShaderResources(4, 1, &srv);
	}
}
