// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"

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
	if (!bloodVS || !bloodSkinVS || !bloodDiscVS || !bloodPS || !bloodDiscPS) {
		bloodShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Blood on snow disabled (shader compilation failed)");
		return false;
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
			bloodShadersFailed = true;
			return false;
		}
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
	if (!a_skinned) {
		// Baked geometry: one deposit. A pointer reused for a new decal fails
		// the buffer-and-position check and deposits again.
		auto& seen = bloodSeen[geometry];
		const auto& position = geometry->world.translate;
		const bool same = seen.frame != 0 && seen.vb == vb && seen.position.GetSquaredDistance(position) < 1.0f;
		seen.frame = globals::state->frameCount;
		seen.vb = vb;
		seen.position = position;
		if (same)
			return;
	}
	const char* diffusePath = "";
	if (auto textureSet = material->textureSet.get())
		if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
			diffusePath = path;
	const float radius = geometry->worldBound.radius;
	std::string pathLower(diffusePath);
	std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	// Weapon drips: tiny decals a blood mod scatters under a bloodied blade
	// for as long as it likes. At map resolution they are a texel each, so
	// they go in as small round drops, and only for the first seconds of a
	// trail - a blade runs dry.
	const bool drip = !a_skinned && (radius < 16.0f || pathLower.find("drop") != std::string::npos);
	if (bloodPathsLogged.size() < 64 && bloodPathsLogged.insert(pathLower).second)
		logger::info("[SNOW DEFORMATION] blood mark '{}' tex='{}' radius {:.1f} skinned={} drip={}",
			geometry->name.c_str() ? geometry->name.c_str() : "", diffusePath, radius, a_skinned ? 1 : 0, drip ? 1 : 0);
	if (drip) {
		const double now = bloodRenderSeconds;
		const auto& centre = geometry->worldBound.center;
		BloodDripCluster* cluster = nullptr;
		for (auto& k : bloodDripClusters) {
			const float dx = k.x - centre.x, dy = k.y - centre.y;
			if (now - k.lastSeen < 2.0 && dx * dx + dy * dy < 300.0f * 300.0f) {
				cluster = &k;
				break;
			}
		}
		if (!cluster) {
			if (bloodDripClusters.size() >= 8)
				bloodDripClusters.erase(bloodDripClusters.begin());
			bloodDripClusters.push_back({ centre.x, centre.y, now, now, -1.0 });
			cluster = &bloodDripClusters.back();
		}
		cluster->x = centre.x;
		cluster->y = centre.y;
		cluster->lastSeen = now;
		const bool live = settings.BloodDripSeconds > 0.0f && now - cluster->firstSeen <= settings.BloodDripSeconds && now - cluster->lastDeposit >= 0.25;
		if (!live)
			return;
		cluster->lastDeposit = now;
		std::scoped_lock lock(bloodDiscMutex);
		if (bloodDiscQueue.size() < kBloodMaxDiscs)
			bloodDiscQueue.push_back({ centre.x, centre.y, centre.z, std::clamp(radius * 0.45f, 2.5f, 8.0f), 0.28f, 0.015f, 0.01f, 0.5f });
		return;
	}
	if (bloodCaptures.size() >= 512)
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
	capture.skinned = a_skinned;
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

	auto rowsFrom = [](const RE::NiTransform& a_x, BloodCB& a_cb) {
		const auto& r = a_x.rotate;
		const float sc = a_x.scale;
		a_cb.WorldRow0 = { r.entry[0][0] * sc, r.entry[0][1] * sc, r.entry[0][2] * sc, a_x.translate.x };
		a_cb.WorldRow1 = { r.entry[1][0] * sc, r.entry[1][1] * sc, r.entry[1][2] * sc, a_x.translate.y };
		a_cb.WorldRow2 = { r.entry[2][0] * sc, r.entry[2][1] * sc, r.entry[2][2] * sc, a_x.translate.z };
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

	context->PSSetShader(bloodPS, nullptr, 0);
	for (const auto& capture : bloodCaptures) {
		auto* geometry = capture.geometry.get();
		if (!geometry)
			continue;
		auto& runtime = geometry->GetGeometryRuntimeData();
		cb.TexcoordOffset = capture.texcoord;
		cb.MaterialAlpha = capture.alpha;
		cb.AlphaThreshold = capture.alphaThreshold;
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
			rowsFrom(capture.world, cb);
			bloodCB->Update(cb);
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->VSSetShader(bloodVS, nullptr, 0);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			context->DrawIndexedInstanced(indexCount, 4, 0, 0, 0);
			bloodDepositsLast++;
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
		cb.WorldRow0 = { 1, 0, 0, 0 };
		cb.WorldRow1 = { 0, 1, 0, 0 };
		cb.WorldRow2 = { 0, 0, 1, 0 };
		bloodCB->Update(cb);
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
			context->DrawIndexedInstanced(indexCount, 4, thisStart, 0, 0);
			bloodDepositsLast++;
		}
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
