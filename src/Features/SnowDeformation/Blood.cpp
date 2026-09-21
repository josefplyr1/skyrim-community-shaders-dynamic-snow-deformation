// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Deferred.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cctype>

// Blood on snow (BLOOD-DESIGN.md). The blood mods stay the simulation; the
// shells become the surface. Every decal-mode Lighting draw with a blood
// diffuse that the capture hook sees is re-rasterised from above into a
// toroidal blood map beside the deformation map, with its own texture, so
// the authored splatter shapes survive. Engine decals are baked geometry
// and deposit once; Dynamic Bloodpool quads animate and deposit every frame
// under MAX alpha. API discs ride the same pass. Nothing evolves the map:
// each texel is dated, and the shells fade by snowfall and age at read.

void SnowDeformation::CreateBloodTextures(const D3D11_TEXTURE2D_DESC& a_mapDesc)
{
	delete bloodMapTexture;
	bloodMapTexture = nullptr;
	delete bloodClockTexture;
	bloodClockTexture = nullptr;
	// The wrappers throw on a failed create; blood is optional, so a failure
	// (RenderDoc's wrapper rejecting a view, say) disables it and says why.
	try {

	D3D11_TEXTURE2D_DESC desc = a_mapDesc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	bloodMapTexture = new Texture2D(desc, "SnowDeformation::BloodMap");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtv{
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		// UAVs take no sRGB format; the ring clear writes zeros either way.
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		bloodMapTexture->CreateSRV(srv);
		bloodMapTexture->CreateRTV(rtv);
		bloodMapTexture->CreateUAV(uav);
	}
	desc.Format = DXGI_FORMAT_R32G32_FLOAT;
	bloodClockTexture = new Texture2D(desc, "SnowDeformation::BloodClock");
	{
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
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		bloodClockTexture->CreateSRV(srv);
		bloodClockTexture->CreateRTV(rtv);
		bloodClockTexture->CreateUAV(uav);
	}
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto context = globals::d3d::context;
	context->ClearRenderTargetView(bloodMapTexture->rtv.get(), zero);
	context->ClearRenderTargetView(bloodClockTexture->rtv.get(), zero);
	bloodSeen.clear();
	bloodTilesDrop = true;
	logger::info("[SNOW DEFORMATION] blood map {}x{} created", desc.Width, desc.Height);
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] blood map creation failed: {} - blood on snow is off", e.what());
		delete bloodMapTexture;
		bloodMapTexture = nullptr;
		delete bloodClockTexture;
		bloodClockTexture = nullptr;
	}
}

ID3D11ComputeShader* SnowDeformation::GetBloodRingCS()
{
	if (!bloodRingCS) {
		logger::debug("Compiling DeformationUpdateCS:BloodRingCS");
		bloodRingCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "BloodRingCS"));
	}
	return bloodRingCS;
}

void SnowDeformation::ReleaseBloodShaders()
{
	if (bloodRingCS)
		bloodRingCS->Release();
	bloodRingCS = nullptr;
	for (auto** shader : { &bloodVS, &bloodSkinVS, &bloodDiscVS }) {
		if (*shader)
			(*shader)->Release();
		*shader = nullptr;
	}
	for (auto** shader : { &bloodPS, &bloodDiscPS }) {
		if (*shader)
			(*shader)->Release();
		*shader = nullptr;
	}
	if (bloodOverlayVS)
		bloodOverlayVS->Release();
	bloodOverlayVS = nullptr;
	if (bloodOverlayPS)
		bloodOverlayPS->Release();
	bloodOverlayPS = nullptr;
	if (runePS)
		runePS->Release();
	runePS = nullptr;
	if (decalMaskVS)
		decalMaskVS->Release();
	decalMaskVS = nullptr;
	if (decalMaskPS)
		decalMaskPS->Release();
	decalMaskPS = nullptr;
	decalMaskFailed = false;
	runeResourcesFailed = false;
	bloodVSBlob = nullptr;
	bloodSkinVSBlob = nullptr;
	bloodILCache.clear();
	bloodSkinILCache.clear();
	bloodShadersFailed = false;
	ReleaseBloodTiles();
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
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER"));
		if (blob) {
			bloodVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodVS)))
				Util::SetResourceName(bloodVS, "SnowDeformation::BloodCaptureVS");
		}
	}
	if (!bloodSkinVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SKINNED"));
		if (blob) {
			bloodSkinVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodSkinVS)))
				Util::SetResourceName(bloodSkinVS, "SnowDeformation::BloodCaptureSkinVS");
		}
	}
	if (!bloodDiscVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "DISC"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodDiscVS)))
			Util::SetResourceName(bloodDiscVS, "SnowDeformation::BloodCaptureDiscVS");
	}
	if (!bloodPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodPS)))
			Util::SetResourceName(bloodPS, "SnowDeformation::BloodCapturePS");
	}
	if (!bloodDiscPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "DISC"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodDiscPS)))
			Util::SetResourceName(bloodDiscPS, "SnowDeformation::BloodCaptureDiscPS");
	}
	if (!bloodOverlayVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "OVERLAY"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlayVS)))
			Util::SetResourceName(bloodOverlayVS, "SnowDeformation::BloodOverlayVS");
	}
	if (!bloodOverlayPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "OVERLAY"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlayPS)))
			Util::SetResourceName(bloodOverlayPS, "SnowDeformation::BloodOverlayPS");
	}
	if (!bloodVS || !bloodSkinVS || !bloodDiscVS || !bloodPS || !bloodDiscPS || !bloodOverlayVS || !bloodOverlayPS) {
		bloodShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Blood on snow disabled (shader compilation failed)");
		return false;
	}
	if (!bloodOverlayBlendState) {
		// The decals' own contribution over the snow in the lit diffuse,
		// normal + gloss, albedo, specular and reflectance; motion vectors
		// and the masks are left to the snow.
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
		// No hardware test: the coat, the rise and the shell all sit nearer
		// than the decal by design. The PS tests against the pre-snow depth
		// copy instead, so bodies and props still hide it.
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
		// The pigment is the texture's colour, written as is: blending it by
		// alpha over the map's black darkened every faint mark to soot.
		// Concentration keeps its high-water mark, so a pool that grows over
		// frames only grows. The clock target overwrites: the newest deposit
		// dates the texel.
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
		// Top-down with the row flipped: winding is meaningless, cull nothing.
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
	auto* geometry = a_capture.geometry.get();
	const BloodCapture& capture = a_capture;
	// Every frame, once per geometry (the hook sees a decal once per pass
	// that draws it): the decal's screen rectangle joins the overlay's. The
	// hook runs before the draw, so at the frame's first decal the targets
	// still hold the ground the game is about to blend it onto.
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
			// Last frame's rectangle grown a little plus this decal; with
			// neither, the whole viewport. A missing copy loses every decal
			// of the frame, a large one costs a copy.
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

void SnowDeformation::CaptureBloodDraw(RE::BSRenderPass* a_pass, bool a_skinned)
{
	auto* geometry = a_pass->geometry;
	auto* property = a_pass->shaderProperty;
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
	if (!material)
		return;
	auto* texture = material->diffuseTexture.get();
	if (!texture || !texture->rendererTexture || !texture->rendererTexture->resourceView)
		return;
	auto& runtime = geometry->GetGeometryRuntimeData();
	const void* vb = runtime.rendererData ? runtime.rendererData->vertexBuffer : nullptr;
	if (!vb)
		return;
	const char* diffusePath = "";
	if (auto textureSet = material->textureSet.get())
		if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
			diffusePath = path;
	std::string pathLower(diffusePath);
	std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	// Not a stain to copy: drips and bleed trails (particle blood owns them),
	// and decals authored for additive or blend rendering, whose RGB is
	// black around the blood - the game blends them, we would paint the
	// black. The bound radius says nothing about a decal's size (Sanguine's
	// Large spray reports 11) and is not consulted.
	auto* alphaProperty = runtime.alphaProperty.get();
	const bool blendsOver = !alphaProperty || !alphaProperty->GetAlphaBlending() ||
	                        (alphaProperty->GetSrcBlendMode() == RE::NiAlphaProperty::AlphaFunction::kSrcAlpha &&
							 alphaProperty->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha);
	bool ignored = !blendsOver;
	for (const char* key : { "drop", "drip", "smallsplatter", "blend", "add" })
		ignored = ignored || pathLower.find(key) != std::string::npos;
	if (bloodPathsLogged.size() < 64 && bloodPathsLogged.insert(pathLower).second)
		logger::info("[SNOW DEFORMATION] blood mark '{}' tex='{}' skinned={} blendsOver={} ignored={}",
			geometry->name.c_str() ? geometry->name.c_str() : "", diffusePath, a_skinned ? 1 : 0, blendsOver ? 1 : 0, ignored ? 1 : 0);
	if (ignored)
		return;
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
	capture.skinned = a_skinned;
	RegisterDecalOverlay(capture);
	if (settings.BloodDirectDecals && BloodTilesWanted() && bloodDecalLive.size() < 512) {
		auto& live = bloodDecalLive[geometry];
		live.draw = capture;
		live.normal = nullptr;
		if (auto* normal = material->normalTexture.get(); normal && normal->rendererTexture && normal->rendererTexture->resourceView)
			live.normal.copy_from(normal->rendererTexture->resourceView);
		live.lastFrame = globals::state->frameCount;
	}
	if (!a_skinned) {
		// Baked geometry: deposited while it spreads, then left alone. A
		// pointer reused for a new decal fails the buffer-and-position check
		// and starts over.
		auto& seen = bloodSeen[geometry];
		const auto& position = geometry->world.translate;
		const bool same = seen.frame != 0 && seen.vb == vb && seen.position.GetSquaredDistance(position) < 1.0f;
		const uint32_t frame = globals::state->frameCount;
		seen.frame = frame;
		seen.vb = vb;
		seen.position = position;
		if (!same) {
			seen.firstSeconds = bloodRenderSeconds;
			seen.firstClock = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
			seen.fineEpoch = 0;
			seen.fineMissing = false;
			seen.fineRetryFrame = 0;
			seen.boundsKnown = false;
		}
		capture.clock = seen.firstClock;
		const float spread = std::max(settings.BloodSpreadSeconds, 0.0f);
		const double age = bloodRenderSeconds - seen.firstSeconds;
		const bool revealing = !(same && age > spread + 0.1);
		capture.reveal = spread > 0.0f ? std::clamp(float(age / spread), 0.05f, 1.0f) : 1.0f;
		// The detail tiles take the mark while it spreads, and again when a
		// tile under it was allocated after it finished. Not once snowfall
		// has buried it: it would only win back the tile that burial freed.
		const bool buried = bloodBurialClock - seen.firstClock.x >= std::max(settings.BloodBurial, 0.01f);
		if (BloodTilesWanted() && !settings.BloodDirectDecals && !buried && bloodFineQueue.size() < 512 &&
			(revealing || ((seen.fineEpoch != bloodTileEpoch || seen.fineMissing) && frame >= seen.fineRetryFrame)))
			bloodFineQueue.push_back({ capture, geometry, revealing });
		if (!revealing)
			return;
	} else if (BloodTilesWanted() && bloodFineQueue.size() < 512) {
		// (Direct decals need the entry too: it holds the mark's bounds.)
		// Pool quads animate: the tiles follow every eighth frame for the
		// first minute, by when a pool has stopped growing.
		auto& seen = bloodSeen[geometry];
		const uint32_t frame = globals::state->frameCount;
		if (seen.frame == 0 || frame - seen.frame > 600) {
			seen.firstSeconds = bloodRenderSeconds;
			seen.firstClock = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
			seen.fineEpoch = 0;
		}
		seen.frame = frame;
		seen.position = geometry->world.translate;
		capture.clock = seen.firstClock;
		const bool growing = bloodRenderSeconds - seen.firstSeconds < 60.0;
		if ((growing && ((frame + uint32_t(reinterpret_cast<uintptr_t>(geometry) >> 6)) & 7u) == 0) ||
			(!growing && (seen.fineEpoch != bloodTileEpoch || seen.fineMissing) && frame >= seen.fineRetryFrame)) {
			if (!settings.BloodDirectDecals)
				bloodFineQueue.push_back({ capture, geometry, growing });
		}
	}
	if (bloodCaptures.size() < 512)
		bloodCaptures.push_back(std::move(capture));
}

// Rune glyphs (BURIED-REF-LIFT-PLAN.md). A rune's visible glyph is an engine
// decal on the ground the landscape shell covers. Same capture as blood, its
// own target: the blood map's texel is several units and a glyph's lines are
// two or three, so each live rune gets a tile of a small atlas, cleared and
// redrawn every frame - the glyph leaves with its rune, and its pulse rides
// the decal's own per-frame alpha. A tile holds the decal's UV FIELD, not its
// colour: the shell samples the rune's own diffuse and normal map through it,
// at the texture's full resolution.
bool SnowDeformation::CaptureRuneDraw(RE::BSRenderPass* a_pass)
{
	auto* geometry = a_pass->geometry;
	auto* property = a_pass->shaderProperty;
	auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
	if (!geometry || !material)
		return false;
	const char* diffusePath = nullptr;
	if (auto textureSet = material->textureSet.get())
		diffusePath = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
	if (!diffusePath || !*diffusePath)
		return false;
	std::string pathLower(diffusePath);
	std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](unsigned char c) { return c == '/' ? '\\' : char(std::tolower(c)); });
	bool matched = false;
	{
		std::scoped_lock lock(runeLock);
		for (const auto& site : runeSites)
			matched = matched || pathLower.find(site.decalPath) != std::string::npos;
	}
	if (runeDecalPathsLogged.size() < 32 && runeDecalPathsLogged.insert(pathLower).second)
		logger::info("[SNOW DEFORMATION] rune watch: decal draw '{}' tex='{}' rune={}",
			geometry->name.c_str() ? geometry->name.c_str() : "", diffusePath, matched ? 1 : 0);
	if (!matched)
		return false;
	if (!runeCaptureSet.insert(geometry).second || runeCaptures.size() >= 64)
		return true;
	auto* texture = material->diffuseTexture.get();
	if (!texture || !texture->rendererTexture || !texture->rendererTexture->resourceView)
		return true;
	auto& runtime = geometry->GetGeometryRuntimeData();
	float threshold = -1.0f;
	if (auto* alpha = runtime.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		threshold = float(alpha->alphaThreshold) / 255.0f;
	RuneCapture capture{};
	capture.draw.geometry = RE::NiPointer<RE::BSGeometry>(geometry);
	capture.draw.world = geometry->world;
	capture.draw.diffuse.copy_from(texture->rendererTexture->resourceView);
	capture.draw.texcoord = { material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	capture.draw.alpha = property->alpha * material->materialAlpha;
	capture.draw.alphaThreshold = threshold;
	capture.draw.reveal = 1.0f;
	capture.draw.skinned = false;
	if (auto* normal = material->normalTexture.get(); normal && normal->rendererTexture && normal->rendererTexture->resourceView)
		capture.normal.copy_from(normal->rendererTexture->resourceView);
	if (auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property); lighting && lighting->emissiveColor) {
		const float mult = lighting->emissiveMult;
		capture.emissive = { lighting->emissiveColor->red * mult, lighting->emissiveColor->green * mult, lighting->emissiveColor->blue * mult };
	}
	capture.centre = geometry->worldBound.center;
	capture.radius = geometry->worldBound.radius;
	runeCaptures.push_back(std::move(capture));
	return true;
}

bool SnowDeformation::EnsureDecalMask(uint32_t a_width, uint32_t a_height)
{
	if (decalMaskFailed || !a_width || !a_height)
		return false;
	auto* device = globals::d3d::device;
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl";
	if (!decalMaskVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "MASK"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &decalMaskVS)))
			Util::SetResourceName(decalMaskVS, "SnowDeformation::DecalMaskVS");
	}
	if (!decalMaskPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "MASK"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &decalMaskPS)))
			Util::SetResourceName(decalMaskPS, "SnowDeformation::DecalMaskPS");
	}
	if (!decalMaskBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_ONE;
		rt.BlendOp = D3D11_BLEND_OP_MAX;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		device->CreateBlendState(&blendDesc, decalMaskBlendState.put());
	}
	if (!decalMaskVS || !decalMaskPS || !decalMaskBlendState) {
		logger::warn("[SNOW DEFORMATION] decal mask disabled (shader or blend state failed): spell marks stay under object snow");
		decalMaskFailed = true;
		return false;
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
			desc.Format = DXGI_FORMAT_R8_UNORM;
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

bool SnowDeformation::EnsureRuneResources()
{
	if (runeResourcesFailed)
		return false;
	if (runeAtlasTexture && runeCB && runePS && runeBlendState)
		return true;
	auto* device = globals::d3d::device;
	try {
		if (!runeAtlasTexture) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kRuneTileDim * 2;
			desc.Height = kRuneTileDim * 2;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			// uv at 16 bits: half floats step 1/1024 near 1, a texel of the glyph.
			desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			runeAtlasTexture = new Texture2D(desc, "SnowDeformation::RuneAtlas");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{
				.Format = DXGI_FORMAT_R16G16B16A16_UNORM,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_RENDER_TARGET_VIEW_DESC rtv{
				.Format = DXGI_FORMAT_R16G16B16A16_UNORM,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			runeAtlasTexture->CreateSRV(srv);
			runeAtlasTexture->CreateRTV(rtv);
		}
		if (!runeCB)
			runeCB = new ConstantBuffer(ConstantBufferDesc<RuneCB>(), "SnowDeformation::RuneCB");
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] rune atlas creation failed: {} - rune glyphs on snow are off", e.what());
		runeResourcesFailed = true;
		return false;
	}
	if (!runePS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl", "ps_5_0", "PSHADER", "RUNE"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &runePS)))
			Util::SetResourceName(runePS, "SnowDeformation::RuneCapturePS");
	}
	if (!runeBlendState) {
		// Pieces of one decal, clipped to different surfaces, overlap from
		// above: MAX keeps the deposit idempotent.
		D3D11_BLEND_DESC blendDesc{};
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_ONE;
		rt.BlendOp = D3D11_BLEND_OP_MAX;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		device->CreateBlendState(&blendDesc, runeBlendState.put());
	}
	if (!runePS || !runeBlendState) {
		logger::warn("[SNOW DEFORMATION] rune glyphs on snow disabled (shader or blend state failed)");
		runeResourcesFailed = true;
		return false;
	}
	return true;
}

void SnowDeformation::RenderRuneCapture()
{
	std::vector<RuneSite> sites;
	{
		std::scoped_lock lock(runeLock);
		sites = runeSites;
	}
	runeStatSites = uint32_t(sites.size());
	runeStatCaptures = uint32_t(runeCaptures.size());
	runeStatTiles = 0;

	// A rune in range whose decal never matched: say so once, with what the
	// record named, so the log can be read against the decal draws it lists.
	if (!sites.empty() && runeCaptures.empty()) {
		if (++runeMissFrames == 180 && !runeMissLogged) {
			runeMissLogged = true;
			logger::warn("[SNOW DEFORMATION] rune in range for 180 frames and no decal draw matched '{}'", sites.front().decalPath);
		}
	} else {
		runeMissFrames = 0;
	}

	auto context = globals::d3d::context;
	const bool paint = settings.RuneDecalsOnSnow && !sites.empty() && !runeCaptures.empty() &&
	                   EnsureBloodResources() && EnsureRuneResources();
	RuneCB cbRune{};
	if (!paint) {
		if (runeAtlasDirty && runeAtlasTexture) {
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearRenderTargetView(runeAtlasTexture->rtv.get(), zero);
			runeAtlasDirty = false;
		}
		if (runeCB)
			runeCB->Update(cbRune);
		for (uint32_t i = 0; i < kRuneMaxTiles; ++i) {
			runeTileDiffuse[i] = nullptr;
			runeTileNormal[i] = nullptr;
		}
		runeCaptures.clear();
		runeCaptureSet.clear();
		return;
	}

	globals::profiler->BeginPass("SnowDeformation::RuneCapture");
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearRenderTargetView(runeAtlasTexture->rtv.get(), zero);
	ID3D11RenderTargetView* rtvs[1] = { runeAtlasTexture->rtv.get() };
	context->OMSetRenderTargets(1, rtvs, nullptr);
	context->OMSetBlendState(runeBlendState.get(), nullptr, 0xFFFFFFFF);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	context->RSSetState(bloodRasterState.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11SamplerState* sampler = bloodSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetShader(runePS, nullptr, 0);

	uint32_t tiles = 0;
	std::vector<BloodCapture> list;
	for (const auto& site : sites) {
		if (tiles >= kRuneMaxTiles)
			break;
		list.clear();
		float3 emissive{ 0.0f, 0.0f, 0.0f };
		const RuneCapture* first = nullptr;
		for (const auto& capture : runeCaptures) {
			const float dx = capture.centre.x - site.position.x;
			const float dy = capture.centre.y - site.position.y;
			const float reach = capture.radius + kRuneTileHalf;
			if (dx * dx + dy * dy > reach * reach)
				continue;
			list.push_back(capture.draw);
			if (!first)
				first = &capture;
			emissive = { std::max(emissive.x, capture.emissive.x), std::max(emissive.y, capture.emissive.y), std::max(emissive.z, capture.emissive.z) };
		}
		if (list.empty())
			continue;
		const float tileX = float((tiles & 1u) * kRuneTileDim);
		const float tileY = float((tiles >> 1u) * kRuneTileDim);
		D3D11_VIEWPORT viewport{ tileX, tileY, float(kRuneTileDim), float(kRuneTileDim), 0.0f, 1.0f };
		context->RSSetViewports(1, &viewport);
		// The blood VS's map transform, aimed at one tile: no torus, so its
		// seam instances fall outside clip space.
		BloodCB cb{};
		cb.WindowOrigin = { site.position.x - kRuneTileHalf, site.position.y - kRuneTileHalf };
		cb.TexelSize = 2.0f * kRuneTileHalf / float(kRuneTileDim);
		cb.MapDim = float(kRuneTileDim);
		cb.MapOrigin = { 0, 0 };
		cb.Intensity = 1.0f;
		cb.NormalZMin = 0.3f;
		if (DrawBloodList(context, list, cb) == 0)
			continue;
		cbRune.RuneRects[tiles] = { cb.WindowOrigin.x, cb.WindowOrigin.y, 1.0f / (2.0f * kRuneTileHalf), 0.0f };
		cbRune.RuneTints[tiles] = { emissive.x, emissive.y, emissive.z, first->normal ? 1.0f : 0.0f };
		runeTileDiffuse[tiles] = first->draw.diffuse;
		runeTileNormal[tiles] = first->normal;
		tiles++;
	}
	for (uint32_t i = tiles; i < kRuneMaxTiles; ++i) {
		runeTileDiffuse[i] = nullptr;
		runeTileNormal[i] = nullptr;
	}
	cbRune.RuneParams = { float(tiles), std::clamp(settings.RuneGlow, 0.0f, 8.0f), 0.0f, 0.0f };
	runeCB->Update(cbRune);
	runeAtlasDirty = true;
	runeStatTiles = tiles;
	if (tiles && !runePaintLogged) {
		runePaintLogged = true;
		logger::info("[SNOW DEFORMATION] rune atlas: first frame painted {} tile(s) from {} decal draw(s), {} rune(s) in range",
			tiles, runeCaptures.size(), sites.size());
	}

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSRV);
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
	runeCaptures.clear();
	runeCaptureSet.clear();
}

// The decal's world bound projected with the game's current camera into the
// bound viewport, in render-resolution pixels. A bound crossing the near
// plane takes the whole viewport (the decal can be anywhere on screen);
// false only when it is off screen.
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

// Lit diffuse, normal + gloss, albedo, specular, reflectance over
// bloodCopyRect into the pre-decal or the pre-snow set. Mid-pass in the
// hook: the game's targets are unbound around the copy and put back.
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
		CopySRVResource(srv, kNames[a_preDecal ? 1 : 0][i], texs[i], srvs[i], &box);
		ok = ok && srvs[i];
	}
	if (a_preDecal && ok) {
		auto* masksSRV = rtData.renderTargets[MASKS].SRV;
		if (masksSRV)
			CopySRVResource(masksSRV, "SnowDeformation::BloodPreDecalMasks", bloodPreDecalMasksTex, bloodPreDecalMasksSRV, &box);
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


void SnowDeformation::APIDepositBlood(float a_x, float a_y, float a_z, float a_radius, float a_r, float a_g, float a_b, float a_amount)
{
	if (!APIIsActive() || !settings.BloodOnSnow || !(a_radius > 0.0f) || !(a_amount > 0.0f))
		return;
	std::scoped_lock lock(bloodDiscMutex);
	if (bloodDiscQueue.size() >= kBloodMaxDiscs)
		return;
	bloodDiscQueue.push_back({ a_x, a_y, a_z, a_radius, std::clamp(a_r, 0.0f, 1.0f), std::clamp(a_g, 0.0f, 1.0f), std::clamp(a_b, 0.0f, 1.0f), std::clamp(a_amount, 0.0f, 1.0f) });
}

uint32_t SnowDeformation::DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, BloodCB& a_cb, ID3D11VertexShader* a_rigidVS, UINT a_instances, bool a_ownClock)
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
		a_cb.Spread = { capture.reveal, 0.0f, capture.slotCode, 0.0f };
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
			context->VSSetShader(a_rigidVS ? a_rigidVS : bloodVS, nullptr, 0);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			context->DrawIndexedInstanced(indexCount, instances, 0, 0, 0);
			drawn++;
			continue;
		}

		// Pool quads: skinned, one palette per partition, absolute world rows
		// (bone world * skinToBone; the game's own palette where a slot has
		// no node). Partition-local bone indices, as SSE partitions carry.
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
		context->VSSetShader(bloodSkinVS, nullptr, 0);
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

void SnowDeformation::RenderBloodCapture()
{
	bloodDepositsLast = 0;
	bloodDiscsLast = 0;
	bloodRenderSeconds += double(globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f);
	const uint32_t frame = globals::state->frameCount;
	// A decal unseen for ten seconds is gone; forget it.
	if ((frame & 63) == 0 || bloodSeen.size() > 4096)
		std::erase_if(bloodSeen, [&](const auto& a_kv) { return frame - a_kv.second.frame > 600; });
	bloodSeenLive = uint32_t(bloodSeen.size());

	std::vector<BloodDisc> discs;
	{
		std::scoped_lock lock(bloodDiscMutex);
		discs.swap(bloodDiscQueue);
	}
	const bool tileWork = !bloodFineQueue.empty() || bloodTilesLive > 0 || !bloodDecalLive.empty();
	if (!settings.BloodOnSnow || !bloodMapTexture || !bloodClockTexture || (bloodCaptures.empty() && discs.empty() && !tileWork) || !EnsureBloodResources()) {
		bloodCaptures.clear();
		bloodFineQueue.clear();
		return;
	}
	RenderBloodTiles(discs);
	if (bloodCaptures.empty() && discs.empty())
		return;

	auto context = globals::d3d::context;
	globals::profiler->BeginPass("SnowDeformation::BloodCapture");
	ID3D11RenderTargetView* rtvs[2] = { bloodMapTexture->rtv.get(), bloodClockTexture->rtv.get() };
	context->OMSetRenderTargets(2, rtvs, nullptr);
	context->OMSetBlendState(bloodBlendState.get(), nullptr, 0xFFFFFFFF);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	context->RSSetState(bloodRasterState.get());
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(deformMapDim), float(deformMapDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11SamplerState* sampler = bloodSampler.get();
	context->PSSetSamplers(0, 1, &sampler);

	BloodCB cb{};
	cb.WindowOrigin = windowOrigin;
	cb.TexelSize = deformWorldSize / float(deformMapDim);
	cb.MapDim = float(deformMapDim);
	cb.MapOrigin = mapOrigin;
	cb.ClockNow = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
	cb.Intensity = std::clamp(settings.BloodIntensity, 0.0f, 2.0f);
	cb.NormalZMin = 0.3f;

	context->PSSetShader(bloodPS, nullptr, 0);
	bloodDepositsLast = DrawBloodList(context, bloodCaptures, cb);
	if (bloodDepositsLast && !bloodDepositLogged) {
		bloodDepositLogged = true;
		logger::info("[SNOW DEFORMATION] blood map: first frame deposited {} of {} decals", bloodDepositsLast, bloodCaptures.size());
	}
	bloodCaptures.clear();

	if (!discs.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(bloodDiscBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			auto* rows = static_cast<float4*>(mapped.pData);
			for (size_t i = 0; i < discs.size(); ++i) {
				const auto& d = discs[i];
				rows[i * 2] = { d.x, d.y, d.z, d.radius };
				rows[i * 2 + 1] = { d.r, d.g, d.b, d.amount };
			}
			context->Unmap(bloodDiscBuffer.get(), 0);
			cb.WorldRow0 = { 1, 0, 0, 0 };
			cb.WorldRow1 = { 0, 1, 0, 0 };
			cb.WorldRow2 = { 0, 0, 1, 0 };
			cb.MaterialAlpha = 1.0f;
			cb.AlphaThreshold = -1.0f;
			cb.Spread = { 1.0f, 0.0f, 0.0f, 0.0f };
			bloodCB->Update(cb);
			context->VSSetShader(bloodDiscVS, nullptr, 0);
			context->PSSetShader(bloodDiscPS, nullptr, 0);
			ID3D11ShaderResourceView* discSRV = bloodDiscSRV.get();
			context->VSSetShaderResources(1, 1, &discSRV);
			context->IASetInputLayout(nullptr);
			ID3D11Buffer* nullVB = nullptr;
			UINT zero = 0;
			context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
			context->DrawInstanced(6, uint32_t(discs.size()) * 4, 0, 0);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			context->VSSetShaderResources(1, 1, &nullSRV);
			bloodDiscsLast = uint32_t(discs.size());
		}
	}

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSRV);
	ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
	context->OMSetRenderTargets(2, nullRTVs, nullptr);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
}

// The game's blood decals drawn again over the object-snow coat, into the
// skin pass's own targets and depth: the coat is a lifted copy of the mesh
// the decal lies on, so the decal loses the depth test to it and vanishes
// under the recolored snow. Pushed past the coat's lift, tested against
// everything else, and confined by the PS to pixels the game painted solid.
void SnowDeformation::DrawBloodOverlay(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_postSkinDepth)
{
	bloodOverlaysLast = 0;
	bloodOverlaySet.clear();
	const bool copies = bloodPreSnowValid && bloodPreDecalValid;
	bloodPreSnowValid = false;
	bloodPreDecalValid = false;
	bloodRectPrev = bloodRectFrame;
	bloodRectPrevValid = bloodRectFrameValid;
	bloodRectFrameValid = false;
	decalMasksLast = 0;
	if (bloodOverlays.empty())
		return;
	const size_t decals = bloodOverlays.size();
	std::vector<BloodCapture> masked;
	for (auto& overlay : bloodOverlays)
		if (overlay.mask)
			masked.push_back(std::move(overlay));
	bloodOverlays.clear();
	// Without the prepass there is no post-skin depth to read the lift from,
	// and without both copies nothing to put back; the decals stay under the
	// snow that frame rather than double on rock.
	if (!DecalOverlayWanted() || !settings.ProjSnowMatch || !a_postSkinDepth || !copies || !EnsureBloodResources())
		return;
	auto* context = a_context;
	globals::profiler->BeginPass("SnowDeformation::BloodOverlay");

	winrt::com_ptr<ID3D11BlendState> savedBlend;
	FLOAT savedBlendFactor[4]{};
	UINT savedSampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepth;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepth.put(), &savedStencilRef);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	// The trench patch draws next and INHERITS most of its pixel-stage
	// textures from the skin pass: what this pass binds over is put back,
	// not nulled (the road patch shaded against empty rasters, 2026-09-18).
	constexpr UINT kOverlaySlots = 4 + 2 * kBloodCopyCount;  // t3 .. t16
	ID3D11ShaderResourceView* savedSRVs[kOverlaySlots]{};
	context->PSGetShaderResources(3, kOverlaySlots, savedSRVs);
	ID3D11ShaderResourceView* savedDiffuse = nullptr;
	context->PSGetShaderResources(0, 1, &savedDiffuse);
	ID3D11Buffer* savedVSCB1 = nullptr;
	ID3D11Buffer* savedPSCB1 = nullptr;
	context->VSGetConstantBuffers(1, 1, &savedVSCB1);
	context->PSGetConstantBuffers(1, 1, &savedPSCB1);

	context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(bloodOverlayDepthState.get(), 0);
	context->RSSetState(bloodRasterState.get());
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(bloodOverlayVS, nullptr, 0);
	context->PSSetShader(bloodOverlayPS, nullptr, 0);

	BloodCB cb{};
	cb.MapDim = 1.0f;
	cb.Intensity = 1.0f;
	const bool shellPaintsOwn = bloodPreSkinDepthThisFrame && BloodTilesLive();
	bloodPreSkinDepthThisFrame = false;
	cb.Spread = { 0.0f, shellPaintsOwn ? 1.0f : 0.0f, 0.0f, 0.0f };
	const float W = std::max(bloodViewport.Width, 1.0f), H = std::max(bloodViewport.Height, 1.0f);
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

	// The decals that are not blood, as alpha in screen space. The overlay
	// reads blood's alpha off its colour; a frost mark or a rune has no such
	// tell, so their own texture alpha is drawn here with the game's camera,
	// kept where the fragment lies on the pre-snow surface.
	ID3D11ShaderResourceView* maskSRV = nullptr;
	if (!masked.empty()) {
		uint32_t width = 0, height = 0;
		if (auto* mainTexture = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture) {
			D3D11_TEXTURE2D_DESC mainDesc{};
			mainTexture->GetDesc(&mainDesc);
			width = mainDesc.Width;
			height = mainDesc.Height;
		}
		if (EnsureDecalMask(width, height)) {
			ID3D11RenderTargetView* savedRTVs[8]{};
			ID3D11DepthStencilView* savedDSV = nullptr;
			context->OMGetRenderTargets(8, savedRTVs, &savedDSV);
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			context->ClearRenderTargetView(decalMaskTexture->rtv.get(), zero);
			ID3D11RenderTargetView* maskRTV = decalMaskTexture->rtv.get();
			context->OMSetRenderTargets(1, &maskRTV, nullptr);
			context->OMSetBlendState(decalMaskBlendState.get(), nullptr, 0xFFFFFFFF);
			context->PSSetShader(decalMaskPS, nullptr, 0);
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
			decalMasksLast = DrawBloodList(context, masked, maskCB, decalMaskVS, 1u);
			ID3D11SamplerState* restoreSampler = savedSampler.get();
			context->PSSetSamplers(0, 1, &restoreSampler);
			context->OMSetRenderTargets(8, savedRTVs, savedDSV);
			for (auto* rtv : savedRTVs)
				if (rtv)
					rtv->Release();
			if (savedDSV)
				savedDSV->Release();
			maskSRV = decalMaskTexture->srv.get();
			if (decalMasksLast && !decalMaskLogged) {
				decalMaskLogged = true;
				logger::info("[SNOW DEFORMATION] decal overlay: first frame masked {} of {} spell decals", decalMasksLast, masked.size());
			}
			// The list drew with its own state; back to the screen rectangle.
			context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(bloodOverlayVS, nullptr, 0);
			context->PSSetShader(bloodOverlayPS, nullptr, 0);
			bloodCB->Update(cb);
		}
	}
	context->PSSetShaderResources(15, 1, &maskSRV);
	ID3D11ShaderResourceView* preSkinSRV = shellPaintsOwn ? bloodPreSkinDepthSRV.get() : nullptr;
	context->PSSetShaderResources(16, 1, &preSkinSRV);

	context->Draw(6, 0);
	bloodOverlaysLast = uint32_t(decals);
	if (!bloodOverlayLogged) {
		bloodOverlayLogged = true;
		logger::info("[SNOW DEFORMATION] blood overlay: first frame composited {} decals over object snow, rect {}x{}", decals, bloodCopyRect.right - bloodCopyRect.left, bloodCopyRect.bottom - bloodCopyRect.top);
	}

	context->PSSetShaderResources(3, kOverlaySlots, savedSRVs);
	context->PSSetShaderResources(0, 1, &savedDiffuse);
	context->VSSetConstantBuffers(1, 1, &savedVSCB1);
	context->PSSetConstantBuffers(1, 1, &savedPSCB1);
	for (auto* srv : savedSRVs)
		if (srv)
			srv->Release();
	if (savedDiffuse)
		savedDiffuse->Release();
	if (savedVSCB1)
		savedVSCB1->Release();
	if (savedPSCB1)
		savedPSCB1->Release();
	context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
	context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
}
