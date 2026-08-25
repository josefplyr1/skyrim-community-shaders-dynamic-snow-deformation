#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void SnowDeformation::CaptureShadowAtlas()
{
	// Called mid-frame while the game renders the shadow mask: PS t4 holds
	// the sun cascade atlas right now (at any other time it holds whatever
	// material texture the last draw bound; an earlier EarlyPrepass grab
	// captured a 128px BC7 diffuse). Copy the atlas and its ESRAM partner
	// immediately: by deferred time the engine has reused the live targets
	// (ESRAM is aliased scratch).
	//
	// When a frame skips this pass, the previous copies are kept; one-frame-
	// stale cascades are invisible, but flapping between the crisp and VSM
	// paths reads as full-surface flicker.
	if (!globals::state->HasDirectionalShadows()) {
		shadowAtlasCopySRV = nullptr;
		shadowEsramCopySRV = nullptr;
		return;
	}

	winrt::com_ptr<ID3D11ShaderResourceView> liveAtlasSRV;
	globals::d3d::context->PSGetShaderResources(4, 1, liveAtlasSRV.put());

	ID3D11ShaderResourceView* liveEsramSRV = nullptr;
	if (auto* gameRenderer = globals::game::renderer)
		liveEsramSRV = gameRenderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM].depthSRV;

	if (liveAtlasSRV && liveEsramSRV) {
		CopySRVResource(liveAtlasSRV.get(), "SnowDeformation::ShadowAtlasCopy", shadowAtlasCopyTex, shadowAtlasCopySRV);
		CopySRVResource(liveEsramSRV, "SnowDeformation::ShadowEsramCopy", shadowEsramCopyTex, shadowEsramCopySRV);
		// Diagnostics: how many slices the copy carries (settings UI line).
		if (shadowAtlasCopyTex) {
			D3D11_TEXTURE2D_DESC atlasDesc;
			shadowAtlasCopyTex->GetDesc(&atlasDesc);
			dbgLodAtlasSlices = atlasDesc.ArraySize;
		}

		// With the receiver copies safely taken (pre-shell), render the shell
		// INTO the live atlas so the world receives snow shadows: the game's
		// shadow mask is drawn right after this and picks the shell up like
		// any other caster.
		InjectShellShadowCasters(liveAtlasSRV.get());
	}

	// One-shot + transition diagnostics: what is the shadow source? Logged on
	// the first few frames and whenever validity flips, so user reports of
	// missing/flickering shell shadows come with the answer attached.
	static uint32_t shadowLogCount = 0;
	static int lastValidState = -1;
	int validState = (shadowAtlasCopySRV && shadowEsramCopySRV) ? 1 : 0;
	if ((shadowLogCount < 6 || validState != lastValidState) && shadowLogCount < 40) {
		shadowLogCount++;
		lastValidState = validState;
		D3D11_TEXTURE2D_DESC atlasDesc{};
		if (shadowAtlasCopyTex)
			shadowAtlasCopyTex->GetDesc(&atlasDesc);
		D3D11_SHADER_RESOURCE_VIEW_DESC liveViewDesc{};
		if (liveAtlasSRV)
			liveAtlasSRV->GetDesc(&liveViewDesc);
		logger::info("[SNOW DEFORMATION] ShadowCopy: atlasLive={} esramLive={} valid={} atlas={}x{} slices={} fmt={} samples={} liveViewDim={} liveViewFmt={}",
			liveAtlasSRV != nullptr, liveEsramSRV != nullptr, validState,
			atlasDesc.Width, atlasDesc.Height, atlasDesc.ArraySize, uint32_t(atlasDesc.Format), atlasDesc.SampleDesc.Count,
			uint32_t(liveViewDesc.ViewDimension), uint32_t(liveViewDesc.Format));
	}
}

void SnowDeformation::InjectShellShadowCasters(ID3D11ShaderResourceView* a_atlasSRV)
{
	// One-shot diagnostics: name the exit taken, so a missing shadow points
	// straight at its gate.
	static std::unordered_set<std::string> injectLogged;
	auto logOnce = [](const char* a_msg) {
		if (injectLogged.size() < 12 && injectLogged.insert(a_msg).second)
			logger::info("[SNOW DEFORMATION] ShadowInject: {}", a_msg);
	};

	if (!settings.EnableSnowDeformation || !lastShellCBData) {
		logOnce("skip: disabled or no ShellCB snapshot yet");
		return;
	}

	auto* vs = GetShellShadowVS();
	if (!vs) {
		logOnce("skip: shell shadow VS unavailable");
		return;
	}

	auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	auto* sunShadowLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunShadowLight) {
		logOnce("skip: no sun shadow light");
		return;
	}
	auto& lightRuntime = sunShadowLight->GetRuntimeData();
	const uint32_t cascadeCount = std::min<uint32_t>((uint32_t)lightRuntime.shadowmapDescriptors.size(), 2u);
	if (cascadeCount == 0) {
		logOnce("skip: zero cascades");
		return;
	}

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	// Per-slice DSVs on the LIVE atlas texture, cached by texture pointer.
	winrt::com_ptr<ID3D11Resource> atlasRes;
	a_atlasSRV->GetResource(atlasRes.put());
	auto atlasTex = atlasRes.try_as<ID3D11Texture2D>();
	if (!atlasTex) {
		logOnce("skip: atlas SRV resource is not a Texture2D");
		return;
	}
	D3D11_TEXTURE2D_DESC atlasDesc;
	atlasTex->GetDesc(&atlasDesc);
	if (!(atlasDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL) || atlasDesc.ArraySize < cascadeCount) {
		logOnce("skip: atlas lacks DEPTH_STENCIL bind or slices");
		return;
	}
	if (shadowAtlasDSVTexture != atlasTex.get()) {
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
			return;
		}
		for (uint32_t i = 0; i < 2; i++) {
			shadowAtlasDSV[i] = nullptr;
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = dsvFormat;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			dsvDesc.Texture2DArray.MipSlice = 0;
			dsvDesc.Texture2DArray.FirstArraySlice = i;
			dsvDesc.Texture2DArray.ArraySize = 1;
			if (i < atlasDesc.ArraySize)
				device->CreateDepthStencilView(atlasTex.get(), &dsvDesc, shadowAtlasDSV[i].put());
			if (shadowAtlasDSV[i])
				Util::SetResourceName(shadowAtlasDSV[i].get(), "SnowDeformation::ShadowAtlas DSV");
		}
		shadowAtlasDSVTexture = atlasTex.get();
	}
	if (!shadowAtlasDSV[0]) {
		logOnce("skip: DSV creation failed");
		return;
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
		logOnce("skip: RS/DSS creation failed");
		return;
	}
	logOnce("injecting: shell grid into both cascades");

	// ---- Save every piece of state we touch: we are inside the game's setup
	// for the shadow-mask draw and must hand it back byte-identical.
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
	winrt::com_ptr<ID3D11ShaderResourceView> prevVSSRVs[6];
	{
		ID3D11ShaderResourceView* srvs[6] = {};
		context->VSGetShaderResources(0, 6, srvs);
		for (uint32_t i = 0; i < 6; i++)
			prevVSSRVs[i].attach(srvs[i]);
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

	// ---- Common caster state.
	context->PSSetShader(nullptr, nullptr, 0);  // depth-only
	context->RSSetState(shadowCastRS.get());
	context->OMSetDepthStencilState(shadowCastDSS.get(), 0);
	D3D11_VIEWPORT atlasViewport{ 0.0f, 0.0f, float(atlasDesc.Width), float(atlasDesc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &atlasViewport);
	// Binding the atlas as DSV force-nulls its SRV binding at PS t4; we
	// restore it at the end.
	ID3D11ShaderResourceView* nullPSSRV = nullptr;
	context->PSSetShaderResources(4, 1, &nullPSSRV);

	// Shell field textures for the VS (ShellSurfaceZ): terrain window,
	// deformation, height field. Snow diffuse / scene depth are PS-only.
	ID3D11ShaderResourceView* vsSRVs[6] = { shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr,
		GetDeformationSRV(), nullptr, nullptr,
		heightTopFiltered ? heightTopFiltered->srv.get() : nullptr,
		heightBottomFiltered ? heightBottomFiltered->srv.get() : nullptr };
	context->VSSetShaderResources(0, 6, vsSRVs);
	ID3D11Buffer* cb0 = shellCB->CB();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	globals::profiler->BeginPass("SnowDeformation::ShellShadowCast");
	for (uint32_t cascade = 0; cascade < cascadeCount; cascade++) {
		if (!shadowAtlasDSV[cascade])
			continue;

		// lightTransform maps absolute world -> per-slice [0,1] UV + depth
		// (row-vector XM convention, proven by GetVSMShadow2D). Append the
		// UV->clip mapping and a small caster push AWAY from the light so
		// receivers exactly at the shell surface (fence tops under their own
		// snow) do not acne.
		DirectX::XMMATRIX uvProj = DirectX::XMLoadFloat4x4(
			reinterpret_cast<const DirectX::XMFLOAT4X4*>(&lightRuntime.shadowmapDescriptors[cascade].lightTransform));
		constexpr float kCasterDepthPush = 0.0008f;
		DirectX::XMMATRIX uvToClip = DirectX::XMMatrixSet(
			2.0f, 0.0f, 0.0f, 0.0f,
			0.0f, -2.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			-1.0f, 1.0f, kCasterDepthPush, 1.0f);
		DirectX::XMMATRIX clip = DirectX::XMMatrixMultiply(uvProj, uvToClip);

		// ShellCB matrices are row_major with mul(M, v): store the transpose.
		ShellCB shadowCB = *lastShellCBData;
		shadowCB.CameraViewProj = DirectX::XMMatrixTranspose(clip);
		// Absolute-world rendering: zero the camera-relative adjust.
		shadowCB.CameraPosAdjust = { 0.0f, 0.0f, 0.0f, 0.0f };
		shadowCB.CameraPreviousPosAdjust = { 0.0f, 0.0f, 0.0f, 0.0f };
		shadowCB.ShellDebugData = 0;
		shellCB->Update(shadowCB);

		context->OMSetRenderTargets(0, nullptr, shadowAtlasDSV[cascade].get());

		// Terrain shell only: vertex-buffer-less grid with the excess-height
		// caster VS.
		context->IASetInputLayout(nullptr);
		ID3D11Buffer* nullVB = nullptr;
		UINT zero = 0;
		context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
		context->VSSetShader(vs, nullptr, 0);
		context->Draw(kShellGridDim * kShellGridDim * 6, 0);
	}
	globals::profiler->EndPass();

	// ---- Restore everything.
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
		ID3D11ShaderResourceView* srvs[6];
		for (uint32_t i = 0; i < 6; i++)
			srvs[i] = prevVSSRVs[i].get();
		context->VSSetShaderResources(0, 6, srvs);
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
