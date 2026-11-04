// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/DevSwitches.h"

#include "Deferred.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cctype>

void SnowDeformation::ReleaseBloodShaders()
{
	bloodVS = nullptr;
	bloodSkinVS = nullptr;
	bloodDiscVS = nullptr;
	bloodPS = nullptr;
	bloodDiscPS = nullptr;
	bloodOverlayVS = nullptr;
	bloodOverlayPS = nullptr;
	decalMaskVS = nullptr;
	decalMaskSkinVS = nullptr;
	decalMaskPS = nullptr;
	decalMaskFailed = false;
	bloodVSBlob = nullptr;
	bloodSkinVSBlob = nullptr;
	bloodILCache.clear();
	bloodSkinILCache.clear();
	bloodShadersFailed = false;
}

bool SnowDeformation::EnsureBloodResources()
{
	if (bloodShadersFailed)
		return false;
	try {
		return EnsureBloodResourcesImpl();
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] blood resources failed: {} - blood on snow is off", e.what());
		bloodShadersFailed = true;
		return false;
	}
}

bool SnowDeformation::EnsureBloodResourcesImpl()
{
	auto* device = globals::d3d::device;
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl";
	if (!bloodVS) {
		bloodVS.attach(static_cast<ID3D11VertexShader*>(SD_CompileShader(path, "vs_5_0", {}, &bloodVSBlob)));
		if (bloodVS)
			Util::SetResourceName(bloodVS.get(), "SnowDeformation::BloodCaptureVS");
	}
	if (!bloodSkinVS) {
		bloodSkinVS.attach(static_cast<ID3D11VertexShader*>(SD_CompileShader(path, "vs_5_0", { "SKINNED" }, &bloodSkinVSBlob)));
		if (bloodSkinVS)
			Util::SetResourceName(bloodSkinVS.get(), "SnowDeformation::BloodCaptureSkinVS");
	}
	if (!bloodDiscVS) {
		bloodDiscVS.attach(static_cast<ID3D11VertexShader*>(SD_CompileShader(path, "vs_5_0", { "DISC" })));
		if (bloodDiscVS)
			Util::SetResourceName(bloodDiscVS.get(), "SnowDeformation::BloodCaptureDiscVS");
	}
	if (!bloodPS) {
		bloodPS.attach(static_cast<ID3D11PixelShader*>(SD_CompileShader(path, "ps_5_0")));
		if (bloodPS)
			Util::SetResourceName(bloodPS.get(), "SnowDeformation::BloodCapturePS");
	}
	if (!bloodDiscPS) {
		bloodDiscPS.attach(static_cast<ID3D11PixelShader*>(SD_CompileShader(path, "ps_5_0", { "DISC" })));
		if (bloodDiscPS)
			Util::SetResourceName(bloodDiscPS.get(), "SnowDeformation::BloodCaptureDiscPS");
	}
	if (!bloodOverlayVS) {
		bloodOverlayVS.attach(static_cast<ID3D11VertexShader*>(SD_CompileShader(path, "vs_5_0", { "OVERLAY" })));
		if (bloodOverlayVS)
			Util::SetResourceName(bloodOverlayVS.get(), "SnowDeformation::BloodOverlayVS");
	}
	if (!bloodOverlayPS) {
		bloodOverlayPS.attach(static_cast<ID3D11PixelShader*>(SD_CompileShader(path, "ps_5_0", { "OVERLAY" })));
		if (bloodOverlayPS)
			Util::SetResourceName(bloodOverlayPS.get(), "SnowDeformation::BloodOverlayPS");
	}
	if (!bloodVS || !bloodSkinVS || !bloodDiscVS || !bloodPS || !bloodDiscPS || !bloodOverlayVS || !bloodOverlayPS) {
		bloodShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Blood on snow disabled (shader compilation failed)");
		return false;
	}
	if (!bloodOverlayBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = TRUE;
		for (int i = 0; i < 8; i++)
			blendDesc.RenderTarget[i].RenderTargetWriteMask = 0;
		for (int i : { 0, 2, 3, 4, 5 }) {
			auto& rt = blendDesc.RenderTarget[i];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		}
		if (FAILED(device->CreateBlendState(&blendDesc, bloodOverlayBlendState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: overlay blend state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodOverlayDepthState) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = FALSE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		if (FAILED(device->CreateDepthStencilState(&dsDesc, bloodOverlayDepthState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: overlay depth state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(BloodCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bloodCB = new ConstantBuffer(cbDesc, "SnowDeformation::BloodCB");
	}
	if (!bloodSkinCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(BloodSkinCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bloodSkinCB = new ConstantBuffer(cbDesc, "SnowDeformation::BloodSkinCB");
	}
	if (!bloodBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = TRUE;
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_ZERO;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		blendDesc.RenderTarget[1].BlendEnable = FALSE;
		blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blendDesc, bloodBlendState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: deposit blend state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodRasterState) {
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&rasterDesc, bloodRasterState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: raster state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodSampler) {
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampDesc, bloodSampler.put()))) {
			logger::error("[SNOW DEFORMATION] blood: sampler failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodDiscBuffer) {
		D3D11_BUFFER_DESC bufDesc{};
		bufDesc.ByteWidth = sizeof(float4) * 2 * kBloodMaxDiscs;
		bufDesc.Usage = D3D11_USAGE_DYNAMIC;
		bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufDesc.StructureByteStride = sizeof(float4);
		if (FAILED(device->CreateBuffer(&bufDesc, nullptr, bloodDiscBuffer.put()))) {
			logger::error("[SNOW DEFORMATION] blood: disc buffer failed");
			bloodShadersFailed = true;
			return false;
		}
		Util::SetResourceName(bloodDiscBuffer.get(), "SnowDeformation::BloodDiscs");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 2 * kBloodMaxDiscs;
		if (FAILED(device->CreateShaderResourceView(bloodDiscBuffer.get(), &srvDesc, bloodDiscSRV.put()))) {
			logger::error("[SNOW DEFORMATION] blood: disc view failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodResourcesLogged) {
		bloodResourcesLogged = true;
		logger::info("[SNOW DEFORMATION] blood shaders and states ready");
	}
	return true;
}

void SnowDeformation::RegisterDecalOverlay(const BloodCapture& a_capture)
{
	if (capturedLoaded == 0 && capturedLoadedPrev == 0)
		return;
	auto* geometry = a_capture.geometry.get();
	if (bloodOverlaySet.contains(geometry))
		return;
	if (!decalReachGateOff && !DecalReachesSnow(geometry->worldBound)) {
		decalReachMisses++;
		return;
	}
	const BloodCapture& capture = a_capture;
	if (bloodOverlays.size() < 512 && bloodOverlaySet.insert(geometry).second) {
		RECT rect{};
		UINT viewports = 1;
		globals::d3d::context->RSGetViewports(&viewports, &bloodViewport);
		const bool haveRect = viewports >= 1 && BloodScreenRect(geometry, rect);
		auto unite = [](RECT& a, const RECT& b) {
			a.left = std::min(a.left, b.left);
			a.top = std::min(a.top, b.top);
			a.right = std::max(a.right, b.right);
			a.bottom = std::max(a.bottom, b.bottom);
		};
		if (haveRect) {
			if (bloodRectFrameValid)
				unite(bloodRectFrame, rect);
			else
				bloodRectFrame = rect;
			bloodRectFrameValid = true;
		}
		if (bloodOverlays.empty() && settings.ProjSnowMatch) {
			bool any = haveRect;
			bloodCopyRect = rect;
			if (bloodRectPrevValid) {
				RECT prev = bloodRectPrev;
				const LONG gx = (prev.right - prev.left) / 12 + 8;
				const LONG gy = (prev.bottom - prev.top) / 12 + 8;
				prev.left = std::max(prev.left - gx, 0L);
				prev.top = std::max(prev.top - gy, 0L);
				prev.right = std::min(prev.right + gx, LONG(bloodViewport.Width));
				prev.bottom = std::min(prev.bottom + gy, LONG(bloodViewport.Height));
				if (any)
					unite(bloodCopyRect, prev);
				else
					bloodCopyRect = prev;
				any = true;
			}
			if (!any)
				bloodCopyRect = { 0, 0, LONG(bloodViewport.Width), LONG(bloodViewport.Height) };
			CopyBloodTargets(true);
		}
		bloodOverlays.push_back(capture);
	}
}

void SnowDeformation::MarkCoatReach(float a_x, float a_y, float a_radius)
{
	const float reach = a_radius + settings.RoadMeshesDepth + kSkinCullMargin + kDecalOverlayMaxLift;
	const int x0 = std::max(int(std::floor((a_x - reach - coatReachOrigin[0]) / kCoatReachCell)), 0);
	const int x1 = std::min(int(std::floor((a_x + reach - coatReachOrigin[0]) / kCoatReachCell)), coatReachDim - 1);
	const int y0 = std::max(int(std::floor((a_y - reach - coatReachOrigin[1]) / kCoatReachCell)), 0);
	const int y1 = std::min(int(std::floor((a_y + reach - coatReachOrigin[1]) / kCoatReachCell)), coatReachDim - 1);
	for (int y = y0; y <= y1; ++y) {
		uint64_t* row = coatReachGrid.data() + size_t(y) * coatReachWords;
		for (int w = x0 >> 6; w <= x1 >> 6; ++w)
			row[w] |= (~0ull >> (63 - std::min(x1 - (w << 6), 63))) & (~0ull << std::max(x0 - (w << 6), 0));
	}
}

bool SnowDeformation::DecalReachesSnow(const RE::NiBound& a_bound)
{
	const uint32_t frame = globals::state->frameCount;
	if (settings.VolumeSnow || shellDrawGroundGated || shellEarlyFrame == frame)
		return true;
	if (coatReachFrame != frame) {
		coatReachFrame = frame;
		coatReachMarked = 0;
		coatReachDim = 0;
		int sqMinX, sqMinY, sqMaxX, sqMaxY;
		if (LoadedCellSquare(sqMinX, sqMinY, sqMaxX, sqMaxY)) {
			constexpr float kCellSize = kShellVertexSpacing * 32.0f;
			coatReachOrigin[0] = float(sqMinX - 1) * kCellSize;
			coatReachOrigin[1] = float(sqMinY - 1) * kCellSize;
			coatReachDim = int(float(std::max(sqMaxX - sqMinX, sqMaxY - sqMinY) + 3) * kCellSize / kCoatReachCell);
			coatReachWords = (coatReachDim + 63) >> 6;
			coatReachGrid.assign(size_t(coatReachDim) * coatReachWords, 0ull);
			for (const auto& b : coatReachBounds[1])
				MarkCoatReach(b.x, b.y, b.z);
		}
	}
	if (coatReachDim == 0)
		return true;
	for (; coatReachMarked < coatReachBounds[0].size(); ++coatReachMarked) {
		const auto& b = coatReachBounds[0][coatReachMarked];
		MarkCoatReach(b.x, b.y, b.z);
	}
	const float r = a_bound.radius;
	const int x0 = int(std::floor((a_bound.center.x - r - coatReachOrigin[0]) / kCoatReachCell));
	const int x1 = int(std::floor((a_bound.center.x + r - coatReachOrigin[0]) / kCoatReachCell));
	const int y0 = int(std::floor((a_bound.center.y - r - coatReachOrigin[1]) / kCoatReachCell));
	const int y1 = int(std::floor((a_bound.center.y + r - coatReachOrigin[1]) / kCoatReachCell));
	if (!(r >= 0.0f) || x0 < 0 || y0 < 0 || x1 >= coatReachDim || y1 >= coatReachDim)
		return true;
	for (int y = y0; y <= y1; ++y) {
		const uint64_t* row = coatReachGrid.data() + size_t(y) * coatReachWords;
		for (int w = x0 >> 6; w <= x1 >> 6; ++w)
			if (row[w] & (~0ull >> (63 - std::min(x1 - (w << 6), 63))) & (~0ull << std::max(x0 - (w << 6), 0)))
				return true;
	}
	return false;
}

void SnowDeformation::CaptureDecalOverlay(RE::BSRenderPass* a_pass)
{
	auto* geometry = a_pass->geometry;
	auto* property = a_pass->shaderProperty;
	auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
	if (!geometry || !material || bloodOverlaySet.contains(geometry))
		return;
	auto* texture = material->diffuseTexture.get();
	if (!texture || !texture->rendererTexture || !texture->rendererTexture->resourceView)
		return;
	auto& runtime = geometry->GetGeometryRuntimeData();
	float threshold = -1.0f;
	if (auto* alpha = runtime.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		threshold = float(alpha->alphaThreshold) / 255.0f;
	BloodCapture capture{};
	capture.geometry = RE::NiPointer<RE::BSGeometry>(geometry);
	capture.world = geometry->world;
	capture.diffuse.copy_from(texture->rendererTexture->resourceView);
	capture.texcoord = { material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	capture.alpha = property->alpha * material->materialAlpha;
	capture.alphaThreshold = threshold;
	capture.reveal = 1.0f;
	capture.skinned = false;
	capture.mask = true;
	if (decalOverlayNamesLogged.size() < 16) {
		const char* path = "";
		if (auto textureSet = material->textureSet.get())
			if (auto diffuse = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
				path = diffuse;
		if (decalOverlayNamesLogged.insert(path).second)
			logger::info("[SNOW DEFORMATION] decal overlay takes '{}' tex='{}'", geometry->name.c_str() ? geometry->name.c_str() : "", path);
	}
	RegisterDecalOverlay(capture);
}

bool SnowDeformation::EnsureDecalMask(uint32_t a_width, uint32_t a_height)
{
	if (decalMaskFailed || !a_width || !a_height)
		return false;
	auto* device = globals::d3d::device;
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl";
	if (!decalMaskVS) {
		decalMaskVS.attach(static_cast<ID3D11VertexShader*>(SD_CompileShader(path, "vs_5_0", { "MASK" })));
		if (decalMaskVS)
			Util::SetResourceName(decalMaskVS.get(), "SnowDeformation::DecalMaskVS");
	}
	if (!decalMaskSkinVS) {
		decalMaskSkinVS.attach(static_cast<ID3D11VertexShader*>(SD_CompileShader(path, "vs_5_0", { "SKINNED", "MASK" })));
		if (decalMaskSkinVS)
			Util::SetResourceName(decalMaskSkinVS.get(), "SnowDeformation::DecalMaskSkinVS");
	}
	if (!decalMaskPS) {
		decalMaskPS.attach(static_cast<ID3D11PixelShader*>(SD_CompileShader(path, "ps_5_0", { "MASK" })));
		if (decalMaskPS)
			Util::SetResourceName(decalMaskPS.get(), "SnowDeformation::DecalMaskPS");
	}
	if (!decalMaskBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
		device->CreateBlendState(&blendDesc, decalMaskBlendState.put());
	}
	if (!decalMaskVS || !decalMaskSkinVS || !decalMaskPS || !decalMaskBlendState) {
		logger::warn("[SNOW DEFORMATION] decal mask disabled (shader or blend state failed): spell marks stay under object snow, the overlay is not gated by its decals");
		decalMaskFailed = true;
		return false;
	}
	if (!decalMaskClearViewChecked) {
		decalMaskClearViewChecked = true;
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		if (SUCCEEDED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), decalMaskContext1.put_void())) && decalMaskContext1 &&
			SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
			decalMaskClearView = options.ClearView != FALSE;
		logger::info("[SNOW DEFORMATION] decal mask: {}", decalMaskClearView ? "cleared over the decals' rectangle (ClearView)" : "cleared whole (no native ClearView)");
	}
	if (decalMaskTexture && (decalMaskTexture->desc.Width != a_width || decalMaskTexture->desc.Height != a_height)) {
		delete decalMaskTexture;
		decalMaskTexture = nullptr;
	}
	if (!decalMaskTexture) {
		try {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			decalMaskTexture = new Texture2D(desc, "SnowDeformation::DecalMask");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{
				.Format = desc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_RENDER_TARGET_VIEW_DESC rtv{
				.Format = desc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			decalMaskTexture->CreateSRV(srv);
			decalMaskTexture->CreateRTV(rtv);
		} catch (const std::exception& e) {
			logger::error("[SNOW DEFORMATION] decal mask creation failed: {}", e.what());
			decalMaskFailed = true;
			return false;
		}
	}
	return true;
}

bool SnowDeformation::BloodScreenRect(const RE::BSGeometry* a_geometry, RECT& a_rect) const
{
	const auto& fb = globals::game::frameBufferCached;
	const auto& m = fb.GetCameraViewProj();
	const auto adjust = fb.GetCameraPosAdjust();
	const auto& bound = a_geometry->worldBound;
	const float r = std::max(bound.radius, 1.0f);
	const float cx = bound.center.x - adjust.x, cy = bound.center.y - adjust.y, cz = bound.center.z - adjust.z;
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	for (int c = 0; c < 8; ++c) {
		const float x = cx + ((c & 1) ? r : -r), y = cy + ((c & 2) ? r : -r), z = cz + ((c & 4) ? r : -r);
		const float w = m.m[3][0] * x + m.m[3][1] * y + m.m[3][2] * z + m.m[3][3];
		if (w < 0.1f) {
			a_rect = { 0, 0, LONG(bloodViewport.Width), LONG(bloodViewport.Height) };
			return a_rect.right > 0 && a_rect.bottom > 0;
		}
		const float nx = (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3]) / w;
		const float ny = (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3]) / w;
		minX = std::min(minX, nx);
		maxX = std::max(maxX, nx);
		minY = std::min(minY, ny);
		maxY = std::max(maxY, ny);
	}
	const float W = bloodViewport.Width, H = bloodViewport.Height;
	if (W < 1.0f || H < 1.0f)
		return false;
	a_rect.left = LONG(std::floor((minX * 0.5f + 0.5f) * W)) - 2;
	a_rect.right = LONG(std::ceil((maxX * 0.5f + 0.5f) * W)) + 2;
	a_rect.top = LONG(std::floor((0.5f - maxY * 0.5f) * H)) - 2;
	a_rect.bottom = LONG(std::ceil((0.5f - minY * 0.5f) * H)) + 2;
	a_rect.left = std::clamp(a_rect.left, 0L, LONG(W));
	a_rect.right = std::clamp(a_rect.right, 0L, LONG(W));
	a_rect.top = std::clamp(a_rect.top, 0L, LONG(H));
	a_rect.bottom = std::clamp(a_rect.bottom, 0L, LONG(H));
	return a_rect.right > a_rect.left && a_rect.bottom > a_rect.top;
}

void SnowDeformation::CopyBloodTargets(bool a_preDecal)
{
	auto* context = globals::d3d::context;
	auto& rtData = globals::game::renderer->GetRuntimeData();
	static constexpr uint32_t kSources[kBloodCopyCount] = { uint32_t(RE::RENDER_TARGETS::kMAIN), uint32_t(NORMALROUGHNESS), uint32_t(ALBEDO), uint32_t(SPECULAR), uint32_t(REFLECTANCE) };
	static constexpr const char* kNames[2][kBloodCopyCount] = {
		{ "SnowDeformation::BloodPreSnow0", "SnowDeformation::BloodPreSnow2", "SnowDeformation::BloodPreSnow3", "SnowDeformation::BloodPreSnow4", "SnowDeformation::BloodPreSnow5" },
		{ "SnowDeformation::BloodPreDecal0", "SnowDeformation::BloodPreDecal2", "SnowDeformation::BloodPreDecal3", "SnowDeformation::BloodPreDecal4", "SnowDeformation::BloodPreDecal5" }
	};
	bool& valid = a_preDecal ? bloodPreDecalValid : bloodPreSnowValid;
	valid = false;
	auto* texs = a_preDecal ? bloodPreDecalTex : bloodPreSnowTex;
	auto* srvs = a_preDecal ? bloodPreDecalSRV : bloodPreSnowSRV;
	const RECT& rc = bloodCopyRect;
	if (rc.right <= rc.left || rc.bottom <= rc.top)
		return;
	const D3D11_BOX box{ UINT(rc.left), UINT(rc.top), 0, UINT(rc.right), UINT(rc.bottom), 1 };
	const D3D11_BOX extent = ScreenRectBox(&bloodViewport, UINT32_MAX, UINT32_MAX, 0);
	ID3D11RenderTargetView* rtvs[8]{};
	ID3D11DepthStencilView* dsv = nullptr;
	context->OMGetRenderTargets(8, rtvs, &dsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	bool ok = true;
	for (uint32_t i = 0; i < kBloodCopyCount; ++i) {
		auto* srv = rtData.renderTargets[kSources[i]].SRV;
		if (!srv) {
			ok = false;
			break;
		}
		CopySRVResource(srv, kNames[a_preDecal ? 1 : 0][i], texs[i], srvs[i], &box, &extent);
		ok = ok && srvs[i];
	}
	if (a_preDecal && ok) {
		auto* masksSRV = rtData.renderTargets[MASKS].SRV;
		if (masksSRV)
			CopySRVResource(masksSRV, "SnowDeformation::BloodPreDecalMasks", bloodPreDecalMasksTex, bloodPreDecalMasksSRV, &box, &extent);
		ok = masksSRV && bloodPreDecalMasksSRV;
	}
	context->OMSetRenderTargets(8, rtvs, dsv);
	for (auto* rtv : rtvs)
		if (rtv)
			rtv->Release();
	if (dsv)
		dsv->Release();
	valid = ok;
}

void SnowDeformation::APIDepositBlood(float, float, float, float, float, float, float, float)
{
}

uint32_t SnowDeformation::DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, BloodCB& a_cb, ID3D11VertexShader* a_rigidVS, UINT a_instances, bool a_ownClock, ID3D11VertexShader* a_skinnedVS)
{
	auto* context = a_context;
	const UINT instances = a_instances;
	uint32_t drawn = 0;

	auto rowsFrom = [](const RE::NiTransform& a_x, BloodCB& a_out) {
		const auto& r = a_x.rotate;
		const float sc = a_x.scale;
		a_out.WorldRow0 = { r.entry[0][0] * sc, r.entry[0][1] * sc, r.entry[0][2] * sc, a_x.translate.x };
		a_out.WorldRow1 = { r.entry[1][0] * sc, r.entry[1][1] * sc, r.entry[1][2] * sc, a_x.translate.y };
		a_out.WorldRow2 = { r.entry[2][0] * sc, r.entry[2][1] * sc, r.entry[2][2] * sc, a_x.translate.z };
	};
	auto layoutFor = [&](uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc, bool a_skinned) -> ID3D11InputLayout* {
		auto& cache = a_skinned ? bloodSkinILCache : bloodILCache;
		auto& blob = a_skinned ? bloodSkinVSBlob : bloodVSBlob;
		auto& layout = cache[a_descKey];
		if (!layout && blob) {
			const uint32_t positionBytes = SD_PositionBytes(a_descKey, a_desc);
			const DXGI_FORMAT positionFormat = positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
			const uint32_t uvOffset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			if (a_skinned) {
				const uint32_t skinOffset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
				D3D11_INPUT_ELEMENT_DESC elements[4] = {
					{ "POSITION", 0, positionFormat, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, uvOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, skinOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, skinOffset + 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				};
				globals::d3d::device->CreateInputLayout(elements, 4, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put());
			} else {
				D3D11_INPUT_ELEMENT_DESC elements[3] = {
					{ "POSITION", 0, positionFormat, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, uvOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 },
				};
				globals::d3d::device->CreateInputLayout(elements, 3, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put());
			}
		}
		return layout.get();
	};

	for (const auto& capture : a_list) {
		auto* geometry = capture.geometry.get();
		if (!geometry)
			continue;
		auto& runtime = geometry->GetGeometryRuntimeData();
		a_cb.TexcoordOffset = capture.texcoord;
		a_cb.MaterialAlpha = capture.alpha;
		a_cb.AlphaThreshold = capture.alphaThreshold;
		a_cb.Spread = { capture.reveal, capture.mask ? 1.0f : 0.0f, 0.0f, capture.decalId };
		if (a_ownClock && capture.clock.y > 0.0f)
			a_cb.ClockNow = capture.clock;
		ID3D11ShaderResourceView* diffuse = capture.diffuse.get();
		context->PSSetShaderResources(0, 1, &diffuse);

		if (!capture.skinned) {
			auto* triShape = geometry->AsTriShape();
			auto* rendererData = runtime.rendererData;
			if (!triShape || !rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				continue;
			const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
			auto desc = rendererData->vertexDesc;
			if (indexCount == 0 || !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
				continue;
			uint64_t descKey;
			memcpy(&descKey, &desc, sizeof(descKey));
			auto* layout = layoutFor(descKey, desc, false);
			const UINT stride = uint32_t(descKey & 0xF) * 4;
			if (!layout || stride == 0)
				continue;
			rowsFrom(capture.world, a_cb);
			bloodCB->Update(a_cb);
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->VSSetShader(a_rigidVS ? a_rigidVS : bloodVS.get(), nullptr, 0);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			context->DrawIndexedInstanced(indexCount, instances, 0, 0, 0);
			drawn++;
			continue;
		}

		auto* skin = runtime.skinInstance.get();
		auto* skinData = skin ? skin->skinData.get() : nullptr;
		auto* skinPartition = skin ? skin->skinPartition.get() : nullptr;
		if (!skinData || !skinPartition || !skin->bones || !skinPartition->partitions.data())
			continue;
		const uint32_t boneCount = skinData->GetBoneCount();
		bloodPaletteScratch.assign(boneCount, RE::NiTransform{});
		std::vector<uint8_t> built(boneCount, 0);
		auto composedFor = [&](uint16_t a_bone) -> const RE::NiTransform* {
			if (a_bone >= boneCount)
				return nullptr;
			if (!built[a_bone]) {
				auto* boneNode = skin->bones[a_bone];
				if (boneNode) {
					bloodPaletteScratch[a_bone] = boneNode->world * skinData->GetBoneDataSkinToBone(a_bone);
				} else if (skin->boneMatrices && a_bone < skin->numMatrices) {
					const float* m = reinterpret_cast<const float*>(skin->boneMatrices) + size_t(a_bone) * 12;
					RE::NiTransform& t = bloodPaletteScratch[a_bone];
					for (int r = 0; r < 3; ++r) {
						for (int c = 0; c < 3; ++c)
							t.rotate.entry[r][c] = m[r * 4 + c];
						t.translate[r] = m[r * 4 + 3];
					}
					t.scale = 1.0f;
				} else {
					return nullptr;
				}
				built[a_bone] = 1;
			}
			return &bloodPaletteScratch[a_bone];
		};
		a_cb.WorldRow0 = { 1, 0, 0, 0 };
		a_cb.WorldRow1 = { 0, 1, 0, 0 };
		a_cb.WorldRow2 = { 0, 0, 1, 0 };
		bloodCB->Update(a_cb);
		context->VSSetShader(a_skinnedVS ? a_skinnedVS : bloodSkinVS.get(), nullptr, 0);
		ID3D11Buffer* cb2 = bloodSkinCB->CB();
		context->VSSetConstantBuffers(2, 1, &cb2);
		RE::BSGraphics::TriShape* rangeBuff = nullptr;
		uint32_t indexStart = 0;
		for (uint32_t p = 0; p < skinPartition->numPartitions; ++p) {
			const auto& part = skinPartition->partitions[p];
			auto* buff = part.buffData;
			if (!buff || !buff->vertexBuffer || !buff->indexBuffer)
				continue;
			if (buff != rangeBuff) {
				rangeBuff = buff;
				indexStart = 0;
			}
			const uint32_t indexCount = uint32_t(part.triangles) * 3;
			const uint32_t thisStart = indexStart;
			indexStart += indexCount;
			if (!part.bones || part.numBones == 0 || part.numBones > kContactMaxBones || indexCount == 0)
				continue;
			auto partDesc = buff->vertexDesc;
			if (!partDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !partDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !partDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
				continue;
			uint64_t descKey;
			memcpy(&descKey, &partDesc, sizeof(descKey));
			auto* layout = layoutFor(descKey, partDesc, true);
			const UINT stride = uint32_t(descKey & 0xF) * 4;
			if (!layout || stride == 0)
				continue;
			BloodSkinCB skinCB{};
			bool complete = true;
			for (uint16_t j = 0; j < part.numBones && complete; ++j) {
				const RE::NiTransform* m = composedFor(part.bones[j]);
				if (!m) {
					complete = false;
					break;
				}
				const auto& rot = m->rotate;
				const float sc = m->scale;
				skinCB.BoneRows[j * 3 + 0] = { rot.entry[0][0] * sc, rot.entry[0][1] * sc, rot.entry[0][2] * sc, m->translate.x };
				skinCB.BoneRows[j * 3 + 1] = { rot.entry[1][0] * sc, rot.entry[1][1] * sc, rot.entry[1][2] * sc, m->translate.y };
				skinCB.BoneRows[j * 3 + 2] = { rot.entry[2][0] * sc, rot.entry[2][1] * sc, rot.entry[2][2] * sc, m->translate.z };
			}
			if (!complete)
				continue;
			skinCB.SkinBoneCount = float(part.numBones);
			bloodSkinCB->Update(skinCB);
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(buff->indexBuffer);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			context->DrawIndexedInstanced(indexCount, instances, thisStart, 0, 0);
			drawn++;
		}
	}
	return drawn;
}

void SnowDeformation::DrawBloodOverlay(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_postSkinDepth)
{
	bloodOverlaysLast = 0;
	bloodOverlaySet.clear();
	decalReachMissesLast = std::exchange(decalReachMisses, 0u);
	const bool copies = bloodPreSnowValid && bloodPreDecalValid;
	bloodPreSnowValid = false;
	bloodPreDecalValid = false;
	bloodRectPrev = bloodRectFrame;
	bloodRectPrevValid = bloodRectFrameValid;
	bloodRectFrameValid = false;
	decalMasksLast = 0;
	std::unordered_set<const RE::BSGeometry*> marked;
	if (bloodOverlays.empty()) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Decals - Overlay");
		return;
	}
	const size_t decals = bloodOverlays.size();
	std::vector<BloodCapture> overlays;
	overlays.swap(bloodOverlays);
	if (!DecalOverlayWanted() || !settings.ProjSnowMatch || !a_postSkinDepth || !copies || !EnsureBloodResources())
		return;
	auto* context = a_context;
	globals::profiler->BeginPass(SnowDevSwitches::DecalOverlayRow("SnowDeformation::Decals - Overlay Mask"));

	winrt::com_ptr<ID3D11BlendState> savedBlend;
	FLOAT savedBlendFactor[4]{};
	UINT savedSampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepth;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepth.put(), &savedStencilRef);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	constexpr UINT kOverlaySlots = 4 + 2 * kBloodCopyCount;
	ID3D11ShaderResourceView* savedSRVs[kOverlaySlots]{};
	context->PSGetShaderResources(3, kOverlaySlots, savedSRVs);
	ID3D11ShaderResourceView* savedDiffuse = nullptr;
	context->PSGetShaderResources(0, 1, &savedDiffuse);
	ID3D11Buffer* savedVSCB1 = nullptr;
	ID3D11Buffer* savedVSCB2 = nullptr;
	ID3D11Buffer* savedPSCB1 = nullptr;
	context->VSGetConstantBuffers(1, 1, &savedVSCB1);
	context->VSGetConstantBuffers(2, 1, &savedVSCB2);
	context->PSGetConstantBuffers(1, 1, &savedPSCB1);

	context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(bloodOverlayDepthState.get(), 0);
	context->RSSetState(bloodRasterState.get());
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(bloodOverlayVS.get(), nullptr, 0);
	context->PSSetShader(bloodOverlayPS.get(), nullptr, 0);

	BloodCB cb{};
	cb.MapDim = 1.0f;
	cb.Intensity = 1.0f;
	const bool shellPaintsOwn = false;
	bloodPreSkinDepthThisFrame = false;
	cb.Spread = { 0.0f, shellPaintsOwn ? 1.0f : 0.0f, 0.0f, 0.0f };
	const float W = std::max(bloodViewport.Width, 1.0f), H = std::max(bloodViewport.Height, 1.0f);
	const auto& eye = globals::game::shadowState->GetRuntimeData().cameraData.getEye();
	cb.OverlayView = { 0.5f * H * eye.projMat(1, 1), bloodViewport.TopLeftX + 0.5f * W, bloodViewport.TopLeftY + 0.5f * H, 0.0f };
	cb.OverlayRect = { float(bloodCopyRect.left) / W * 2.0f - 1.0f, 1.0f - float(bloodCopyRect.top) / H * 2.0f,
		float(bloodCopyRect.right) / W * 2.0f - 1.0f, 1.0f - float(bloodCopyRect.bottom) / H * 2.0f };
	bloodCB->Update(cb);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11ShaderResourceView* readSRVs[2 + 2 * kBloodCopyCount] = { Util::GetCurrentSceneDepthSRV(false), a_postSkinDepth };
	for (uint32_t i = 0; i < kBloodCopyCount; ++i) {
		readSRVs[2 + i] = bloodPreSnowSRV[i].get();
		readSRVs[2 + kBloodCopyCount + i] = bloodPreDecalSRV[i].get();
	}
	context->PSSetShaderResources(3, 2 + 2 * kBloodCopyCount, readSRVs);
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);

	ID3D11ShaderResourceView* maskSRV = nullptr;
	uint32_t width = 0, height = 0;
	if (auto* mainTexture = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture) {
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTexture->GetDesc(&mainDesc);
		const D3D11_BOX extent = ScreenRectBox(&bloodViewport, mainDesc.Width, mainDesc.Height, 0);
		width = std::max(extent.right, 1u);
		height = std::max(extent.bottom, 1u);
	}
	if (EnsureDecalMask(width, height)) {
		ID3D11RenderTargetView* savedRTVs[8]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(8, savedRTVs, &savedDSV);
		const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		if (decalMaskClearView) {
			const D3D11_RECT clearRect = bloodCopyRect;
			decalMaskContext1->ClearView(decalMaskTexture->rtv.get(), zero, &clearRect, 1);
		} else {
			context->ClearRenderTargetView(decalMaskTexture->rtv.get(), zero);
		}
		ID3D11RenderTargetView* maskRTV = decalMaskTexture->rtv.get();
		context->OMSetRenderTargets(1, &maskRTV, nullptr);
		context->OMSetBlendState(decalMaskBlendState.get(), nullptr, 0xFFFFFFFF);
		context->PSSetShader(decalMaskPS.get(), nullptr, 0);
		winrt::com_ptr<ID3D11SamplerState> savedSampler;
		context->PSGetSamplers(0, 1, savedSampler.put());
		ID3D11SamplerState* sampler = bloodSampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		BloodCB maskCB{};
		maskCB.MapDim = 1.0f;
		maskCB.Intensity = 1.0f;
		maskCB.NormalZMin = -2.0f;
		const auto& fb = globals::game::frameBufferCached;
		const auto& m = fb.GetCameraViewProj();
		const auto adjust = fb.GetCameraPosAdjust();
		maskCB.ViewProjRow0 = { m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3] };
		maskCB.ViewProjRow1 = { m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3] };
		maskCB.ViewProjRow2 = { m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3] };
		maskCB.ViewProjRow3 = { m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3] };
		maskCB.CameraAdjust = { adjust.x, adjust.y, adjust.z, 0.0f };
		const auto kept = std::remove_if(overlays.begin(), overlays.end(), [&](const BloodCapture& a_capture) { return marked.contains(a_capture.geometry.get()); });
		const bool excluded = kept != overlays.end();
		overlays.erase(kept, overlays.end());
		const uint32_t drawn = DrawBloodList(context, overlays, maskCB, decalMaskVS.get(), 1u, false, decalMaskSkinVS.get());
		decalMasksLast = uint32_t(std::count_if(overlays.begin(), overlays.end(), [](const BloodCapture& a_capture) { return a_capture.mask; }));
		ID3D11SamplerState* restoreSampler = savedSampler.get();
		context->PSSetSamplers(0, 1, &restoreSampler);
		context->OMSetRenderTargets(8, savedRTVs, savedDSV);
		for (auto* rtv : savedRTVs)
			if (rtv)
				rtv->Release();
		if (savedDSV)
			savedDSV->Release();
		maskSRV = decalMaskTexture->srv.get();
		if (drawn && !decalMaskLogged) {
			decalMaskLogged = true;
			logger::info("[SNOW DEFORMATION] decal overlay: first frame drew {} mask draws for {} decals, {} not blood", drawn, overlays.size(), decalMasksLast);
		}
		cb.Spread.z = (drawn || excluded) ? 1.0f : 0.0f;
		context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(bloodOverlayVS.get(), nullptr, 0);
		context->PSSetShader(bloodOverlayPS.get(), nullptr, 0);
		bloodCB->Update(cb);
	}
	context->PSSetShaderResources(15, 1, &maskSRV);
	ID3D11ShaderResourceView* preSkinSRV = shellPaintsOwn ? bloodPreSkinDepthSRV.get() : nullptr;
	context->PSSetShaderResources(16, 1, &preSkinSRV);

	globals::profiler->EndPass();
	globals::profiler->BeginPass(SnowDevSwitches::DecalOverlayRow("SnowDeformation::Decals - Overlay Composite"));
	context->Draw(6, 0);
	bloodOverlaysLast = uint32_t(decals);
	if (!bloodOverlayLogged) {
		bloodOverlayLogged = true;
		logger::info("[SNOW DEFORMATION] blood overlay: first frame composited {} decals over object snow, rect {}x{}", decals, bloodCopyRect.right - bloodCopyRect.left, bloodCopyRect.bottom - bloodCopyRect.top);
	}

	context->PSSetShaderResources(3, kOverlaySlots, savedSRVs);
	context->PSSetShaderResources(0, 1, &savedDiffuse);
	context->VSSetConstantBuffers(1, 1, &savedVSCB1);
	context->VSSetConstantBuffers(2, 1, &savedVSCB2);
	context->PSSetConstantBuffers(1, 1, &savedPSCB1);
	for (auto* srv : savedSRVs)
		if (srv)
			srv->Release();
	if (savedDiffuse)
		savedDiffuse->Release();
	if (savedVSCB1)
		savedVSCB1->Release();
	if (savedVSCB2)
		savedVSCB2->Release();
	if (savedPSCB1)
		savedPSCB1->Release();
	context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
	context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
}
