// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

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
	for (auto** shader : { &bloodOverlayVS, &bloodOverlaySkinVS }) {
		if (*shader)
			(*shader)->Release();
		*shader = nullptr;
	}
	if (bloodOverlayPS)
		bloodOverlayPS->Release();
	bloodOverlayPS = nullptr;
	bloodVSBlob = nullptr;
	bloodSkinVSBlob = nullptr;
	bloodOverlayVSBlob = nullptr;
	bloodOverlaySkinVSBlob = nullptr;
	bloodILCache.clear();
	bloodSkinILCache.clear();
	bloodOverlayILCache.clear();
	bloodOverlaySkinILCache.clear();
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
		if (blob) {
			bloodOverlayVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlayVS)))
				Util::SetResourceName(bloodOverlayVS, "SnowDeformation::BloodOverlayVS");
		}
	}
	if (!bloodOverlaySkinVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "OVERLAY", "SKINNED"));
		if (blob) {
			bloodOverlaySkinVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlaySkinVS)))
				Util::SetResourceName(bloodOverlaySkinVS, "SnowDeformation::BloodOverlaySkinVS");
		}
	}
	if (!bloodOverlayPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "OVERLAY"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlayPS)))
			Util::SetResourceName(bloodOverlayPS, "SnowDeformation::BloodOverlayPS");
	}
	if (!bloodVS || !bloodSkinVS || !bloodDiscVS || !bloodPS || !bloodDiscPS || !bloodOverlayVS || !bloodOverlaySkinVS || !bloodOverlayPS) {
		bloodShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Blood on snow disabled (shader compilation failed)");
		return false;
	}
	if (!bloodOverlayBlendState) {
		// Lit diffuse (RT0) and albedo (RT3) take the pre-snow pixel back at
		// the decal's alpha; nothing else in the G-buffer is touched.
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = TRUE;
		for (int i = 0; i < 8; i++)
			blendDesc.RenderTarget[i].RenderTargetWriteMask = 0;
		for (int i : { 0, 3 }) {
			auto& rt = blendDesc.RenderTarget[i];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
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
	// Every frame, once per geometry (the hook sees a decal once per pass
	// that draws it): the overlay redraws it over the snow.
	if (bloodOverlays.size() < 512 && bloodOverlaySet.insert(geometry).second)
		bloodOverlays.push_back(capture);
	if (!a_skinned) {
		// Baked geometry: deposited while it spreads, then left alone. A
		// pointer reused for a new decal fails the buffer-and-position check
		// and starts over.
		auto& seen = bloodSeen[geometry];
		const auto& position = geometry->world.translate;
		const bool same = seen.frame != 0 && seen.vb == vb && seen.position.GetSquaredDistance(position) < 1.0f;
		seen.frame = globals::state->frameCount;
		seen.vb = vb;
		seen.position = position;
		if (!same)
			seen.firstSeconds = bloodRenderSeconds;
		const float spread = std::max(settings.BloodSpreadSeconds, 0.0f);
		const double age = bloodRenderSeconds - seen.firstSeconds;
		if (same && age > spread + 0.1)
			return;
		capture.reveal = spread > 0.0f ? std::clamp(float(age / spread), 0.05f, 1.0f) : 1.0f;
	}
	if (bloodCaptures.size() < 512)
		bloodCaptures.push_back(std::move(capture));
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

uint32_t SnowDeformation::DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, bool a_overlay, BloodCB& a_cb)
{
	auto* context = a_context;
	const UINT instances = a_overlay ? 1u : 4u;
	uint32_t drawn = 0;

	auto rowsFrom = [](const RE::NiTransform& a_x, BloodCB& a_out) {
		const auto& r = a_x.rotate;
		const float sc = a_x.scale;
		a_out.WorldRow0 = { r.entry[0][0] * sc, r.entry[0][1] * sc, r.entry[0][2] * sc, a_x.translate.x };
		a_out.WorldRow1 = { r.entry[1][0] * sc, r.entry[1][1] * sc, r.entry[1][2] * sc, a_x.translate.y };
		a_out.WorldRow2 = { r.entry[2][0] * sc, r.entry[2][1] * sc, r.entry[2][2] * sc, a_x.translate.z };
	};
	auto layoutFor = [&](uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc, bool a_skinned) -> ID3D11InputLayout* {
		auto& cache = a_overlay ? (a_skinned ? bloodOverlaySkinILCache : bloodOverlayILCache) : (a_skinned ? bloodSkinILCache : bloodILCache);
		auto& blob = a_overlay ? (a_skinned ? bloodOverlaySkinVSBlob : bloodOverlayVSBlob) : (a_skinned ? bloodSkinVSBlob : bloodVSBlob);
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
		a_cb.Spread = { a_overlay ? 1.0f : capture.reveal, 0.0f, 0.0f, 0.0f };
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
			context->VSSetShader(a_overlay ? bloodOverlayVS : bloodVS, nullptr, 0);
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
		context->VSSetShader(a_overlay ? bloodOverlaySkinVS : bloodSkinVS, nullptr, 0);
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
	if (!settings.BloodOnSnow || !bloodMapTexture || !bloodClockTexture || (bloodCaptures.empty() && discs.empty())) {
		bloodCaptures.clear();
		return;
	}
	if (!EnsureBloodResources()) {
		bloodCaptures.clear();
		return;
	}

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
	bloodDepositsLast = DrawBloodList(context, bloodCaptures, false, cb);
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
	const bool preSnow = bloodPreSnowValid;
	bloodPreSnowValid = false;
	if (bloodOverlays.empty())
		return;
	// Without the prepass there is no post-skin depth to read the lift from,
	// and without this frame's pre-snow copy nothing to put back; the decals
	// stay under the snow that frame rather than double on rock.
	if (!settings.BloodOnSnow || !settings.ProjSnowMatch || !a_postSkinDepth || !preSnow || !EnsureBloodResources()) {
		bloodOverlays.clear();
		return;
	}
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

	context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(bloodOverlayDepthState.get(), 0);
	context->RSSetState(bloodRasterState.get());
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11Buffer* cb0 = shellCB->CB();
	context->VSSetConstantBuffers(0, 1, &cb0);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11SamplerState* sampler = bloodSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	ID3D11ShaderResourceView* readSRVs[4] = { Util::GetCurrentSceneDepthSRV(false), a_postSkinDepth, bloodPreSnowColorSRV.get(), bloodPreSnowAlbedoSRV.get() };
	context->PSSetShaderResources(3, 4, readSRVs);
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);
	context->PSSetShader(bloodOverlayPS, nullptr, 0);

	BloodCB cb{};
	cb.MapDim = 1.0f;
	cb.Intensity = 1.0f;
	bloodOverlaysLast = DrawBloodList(context, bloodOverlays, true, cb);
	if (bloodOverlaysLast && !bloodOverlayLogged) {
		bloodOverlayLogged = true;
		logger::info("[SNOW DEFORMATION] blood overlay: first frame drew {} of {} decals over object snow", bloodOverlaysLast, bloodOverlays.size());
	}
	bloodOverlays.clear();

	ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, 1, nullSRVs);
	context->PSSetShaderResources(4, 3, nullSRVs);
	context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
	context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
}
