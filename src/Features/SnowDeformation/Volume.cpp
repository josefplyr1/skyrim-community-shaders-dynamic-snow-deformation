// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/D3D.h"

// Voxel snow volume (VOLUME-SNOW-PLAN): the captured statics rasterised
// into camera-anchored 3D windows - a clipmap of them, each level twice the
// voxel of the one inside it, drawn only where no finer level reaches.

bool SnowDeformation::EnsureVoxelResources()
{
	const uint levels = VoxelLevelsLive();
	bool levelsReady = true;
	for (uint i = 0; i < levels; i++) {
		const auto& lv = voxelLevels[i];
		levelsReady = levelsReady && lv.volume[0] && lv.volume[1] && lv.field && lv.bricks && lv.drawArgs;
	}
	if (levelsReady && voxelSliceTexture && voxelCB && voxelDrawCB && voxelRasterState && voxelWrapSampler &&
		voxelVS && voxelGS && voxelPS && voxelScrollCS && voxelSliceCS && voxelSeedCS && voxelBlurCS && voxelBrickListCS)
		return true;
	if (voxelShadersFailed)
		return false;
	LoadTraceScope _loadTrace(this, "Volume: EnsureVoxelResources");
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;

	D3D11_TEXTURE3D_DESC volDesc{
		.Width = kVoxelDim,
		.Height = kVoxelDim,
		.Depth = kVoxelDim,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R8_UNORM,
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		.CPUAccessFlags = 0,
		.MiscFlags = 0
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC volSrvDesc = {
		.Format = volDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
		.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC volUavDesc = {
		.Format = volDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kVoxelDim }
	};
	// Fresh VRAM is not blank: cleared so a never-written volume reads
	// empty rather than as whatever lived there before.
	const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto makeVolume = [&](const std::string& a_name) {
		auto* tex = new Texture3D(volDesc, a_name.c_str());
		tex->CreateSRV(volSrvDesc);
		tex->CreateUAV(volUavDesc);
		context->ClearUnorderedAccessViewFloat(tex->uav.get(), clearZero);
		return tex;
	};
	for (uint i = 0; i < levels; i++) {
		auto& lv = voxelLevels[i];
		for (int p = 0; p < 2; p++)
			if (!lv.volume[p])
				lv.volume[p] = makeVolume(std::format("SnowDeformation::VoxelVolume{}{}", i, p));
		if (!lv.field)
			lv.field = makeVolume(std::format("SnowDeformation::VoxelField{}", i));
		if (!lv.bricks) {
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(uint32_t);
			desc.ByteWidth = sizeof(uint32_t) * kVoxelBrickCapacity;
			lv.bricks = new Buffer(desc, nullptr, std::format("SnowDeformation::VoxelBricks{}", i).c_str());
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = kVoxelBrickCapacity;
			lv.bricks->CreateSRV(srvDesc);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = 0;
			uavDesc.Buffer.NumElements = kVoxelBrickCapacity;
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
			lv.bricks->CreateUAV(uavDesc);
		}
		if (!lv.drawArgs) {
			// {VertexCountPerInstance, InstanceCount, StartVertex, StartInstance};
			// only the count changes, copied from the list's counter.
			const uint32_t args[4] = { 36, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA init{ args, 0, 0 };
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.ByteWidth = sizeof(args);
			desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			lv.drawArgs = new Buffer(desc, &init, std::format("SnowDeformation::VoxelDrawArgs{}", i).c_str());
		}
		lv.valid = false;
	}
	if (!voxelSliceTexture) {
		D3D11_TEXTURE2D_DESC desc{
			.Width = kVoxelDim,
			.Height = kVoxelDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		voxelSliceTexture = new Texture2D(desc, "SnowDeformation::VoxelSlice");
		voxelSliceTexture->CreateSRV(srvDesc);
		voxelSliceTexture->CreateUAV(uavDesc);
		// Shown by the menu before the first write; black, not stale VRAM.
		context->ClearUnorderedAccessViewFloat(voxelSliceTexture->uav.get(), clearZero);
	}
	if (!voxelCB)
		voxelCB = new ConstantBuffer(ConstantBufferDesc<VoxelVolumeCB>(), "SnowDeformation::VoxelVolumeCB");
	if (!voxelDrawCB)
		voxelDrawCB = new ConstantBuffer(ConstantBufferDesc<VoxelDrawCB>(), "SnowDeformation::VoxelDrawCB");
	if (!voxelWrapSampler) {
		// Trilinear + WRAP: the torus, after the shader clamps in logical space.
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (SUCCEEDED(device->CreateSamplerState(&sampDesc, voxelWrapSampler.put())))
			Util::SetResourceName(voxelWrapSampler.get(), "SnowDeformation::VoxelWrapSampler");
	}
	if (!voxelCountBuffer) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		HRESULT hr = device->CreateBuffer(&desc, nullptr, voxelCountBuffer.put());
		if (SUCCEEDED(hr)) {
			Util::SetResourceName(voxelCountBuffer.get(), "SnowDeformation::VoxelOccupancyCount");
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = 4;
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			hr = device->CreateUnorderedAccessView(voxelCountBuffer.get(), &uavDesc, voxelCountUAV.put());
			D3D11_BUFFER_DESC stagingDesc{};
			stagingDesc.ByteWidth = 16;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
				hr = device->CreateBuffer(&stagingDesc, nullptr, voxelCountStaging[i].put());
				if (SUCCEEDED(hr))
					Util::SetResourceName(voxelCountStaging[i].get(), "SnowDeformation::VoxelOccupancyStaging");
			}
		}
		if (FAILED(hr))
			logger::warn("[SNOW DEFORMATION] Voxel occupancy counter unavailable (HRESULT {:#x}); the volume still runs", (unsigned)hr);
	}
	if (!voxelRasterState) {
		// Both faces: undersides are what the roof test is about.
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&rasterDesc, voxelRasterState.put()))) {
			voxelShadersFailed = true;
			return false;
		}
		Util::SetResourceName(voxelRasterState.get(), "SnowDeformation::VoxelRaster");
	}

	// Util::CompileShader adds the stage define from the target itself.
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowVoxelCapture.hlsl";
	auto compileCS = [&](ID3D11ComputeShader*& a_cs, const char* a_entry, const char* a_name) {
		if (a_cs)
			return;
		a_cs = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", a_entry));
		if (a_cs)
			Util::SetResourceName(a_cs, a_name);
	};
	if (!voxelVS) {
		voxelVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(path, {}, "vs_5_0"));
		if (voxelVS)
			Util::SetResourceName(voxelVS, "SnowDeformation::VoxelCaptureVS");
	}
	if (!voxelGS) {
		voxelGS = static_cast<ID3D11GeometryShader*>(CompileSnowShader(path, {}, "gs_5_0"));
		if (voxelGS)
			Util::SetResourceName(voxelGS, "SnowDeformation::VoxelCaptureGS");
	}
	if (!voxelPS) {
		voxelPS = static_cast<ID3D11PixelShader*>(CompileSnowShader(path, {}, "ps_5_0"));
		if (voxelPS)
			Util::SetResourceName(voxelPS, "SnowDeformation::VoxelCapturePS");
	}
	compileCS(voxelScrollCS, "VoxelScrollCS", "SnowDeformation::VoxelScrollCS");
	compileCS(voxelSliceCS, "VoxelSliceCS", "SnowDeformation::VoxelSliceCS");
	compileCS(voxelSeedCS, "VoxelSeedCS", "SnowDeformation::VoxelSeedCS");
	compileCS(voxelBlurCS, "VoxelBlurCS", "SnowDeformation::VoxelBlurCS");
	compileCS(voxelBrickListCS, "VoxelBrickListCS", "SnowDeformation::VoxelBrickListCS");
	if (!voxelVS || !voxelGS || !voxelPS || !voxelScrollCS || !voxelSliceCS || !voxelSeedCS || !voxelBlurCS || !voxelBrickListCS) {
		voxelShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Voxel volume disabled (shader compilation failed: VS {} GS {} PS {} ScrollCS {} SliceCS {} SeedCS {} BlurCS {} BrickListCS {})",
			voxelVS ? "ok" : "FAILED", voxelGS ? "ok" : "FAILED", voxelPS ? "ok" : "FAILED",
			voxelScrollCS ? "ok" : "FAILED", voxelSliceCS ? "ok" : "FAILED",
			voxelSeedCS ? "ok" : "FAILED", voxelBlurCS ? "ok" : "FAILED", voxelBrickListCS ? "ok" : "FAILED");
		return false;
	}
	return true;
}

float SnowDeformation::VoxelSizeForLevel(uint a_level) const
{
	return VoxelSizeLive() * float(1u << a_level);
}

void SnowDeformation::VoxelReachBand(uint a_level, float& a_start, float& a_end) const
{
	a_end = kVoxelDim * VoxelSizeForLevel(a_level) * 0.5f * kVoxelReachFrac;
	a_start = a_end * (1.0f - kVoxelBandFrac);
}

void SnowDeformation::FillVoxelCB(uint a_level, VoxelVolumeCB& a_cb) const
{
	const auto& lv = voxelLevels[a_level];
	const float voxelSize = VoxelSizeForLevel(a_level);
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();

	a_cb.OriginVox = { lv.origin.x, lv.origin.y, lv.origin.z, 0 };
	a_cb.ScrollDelta = { 0, 0, 0, 0 };
	a_cb.VoxelSize = voxelSize;
	a_cb.Decay = 1.0f / std::max(voxelMemorySeconds * 60.0f, 1.0f);
	a_cb.Dim = (int)kVoxelDim;
	a_cb.SliceAxis = std::clamp(voxelSliceAxis, 0, 2);
	a_cb.SliceXray = voxelSliceXray ? 1 : 0;
	a_cb.SliceSource = std::clamp(voxelSliceSource, 0, 2);
	{
		const float along = a_cb.SliceAxis == 0 ? eye.z : (a_cb.SliceAxis == 1 ? eye.y : eye.x);
		const int originAlong = a_cb.SliceAxis == 0 ? lv.origin.z : (a_cb.SliceAxis == 1 ? lv.origin.y : lv.origin.x);
		a_cb.SliceIndex = std::clamp((int)std::floor((along + voxelSliceOffset) / voxelSize) - originAlong, 0, (int)kVoxelDim - 1);
	}
	// The seed gate's maps live in the height window; without them every
	// column reads as open sky (HalfExtent 0 fails the window test).
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	a_cb.HeightWindowCenter = heightWindowCenter;
	a_cb.HeightHalfExtent = seedMaps ? kHeightMapHalfExtent : 0.0f;
	a_cb.ShelterDust = kVoxelShelterDust;
	// sigma such that coverage 0.5 lands the isosurface at the slider:
	// exp(-h^2 / 2 s^2) = 0.5 -> h = 1.177 s. Floored per level, so a
	// coarse level's snow clears its own seed voxel.
	a_cb.SeedSigma = std::max(std::max(settings.VolumeSnowDepth, 2.0f) / (1.177f * voxelSize), kVoxelMinSigmaVox);
	a_cb.FieldThreshold = std::clamp(settings.VolumeSnowCoverage, 0.05f, 0.95f);
	a_cb.SideSigma = std::max(std::clamp(settings.VolumeSnowOverhang, 0.0f, 16.0f) / (1.177f * voxelSize), kVoxelMinSideSigmaVox);
	const float slopeDeg = std::clamp(settings.VolumeSnowMaxSlopeDeg, 0.0f, 90.0f);
	a_cb.SlopeTanMax = slopeDeg >= 89.5f ? 1.0e6f : std::tan(DirectX::XMConvertToRadians(slopeDeg));
	a_cb.SkyStrength = std::clamp(settings.VolumeSkyExposurePct / 100.0f, 0.0f, 1.0f);
	a_cb.EyeVox[0] = eye.x / voxelSize;
	a_cb.EyeVox[1] = eye.y / voxelSize;
	a_cb.EyeVox[2] = eye.z / voxelSize;
	// Bricks reach the end of this level's band; they also stop where the
	// level inside takes over - the start of ITS band, in this level's
	// voxels. Both Chebyshev, so the two surfaces coincide.
	float bandStart = 0.0f, bandEnd = 0.0f;
	VoxelReachBand(a_level, bandStart, bandEnd);
	a_cb.EyeVox[3] = bandEnd / voxelSize;
	a_cb.InnerHalfVox = 0.0f;
	if (a_level > 0) {
		float innerStart = 0.0f, innerEnd = 0.0f;
		VoxelReachBand(a_level - 1, innerStart, innerEnd);
		a_cb.InnerHalfVox = innerStart / voxelSize;
	}
	a_cb.pad0 = 0.0f;
	a_cb.pad1 = 0.0f;
}

void SnowDeformation::RenderVoxelVolume(const StaticsCB* a_records, uint32_t a_captureCount, bool a_recordsLive, uint32_t& a_parity)
{
	if (!settings.VolumeSnow) {
		for (auto& lv : voxelLevels)
			lv.valid = false;
		return;
	}
	if (!EnsureVoxelResources())
		return;
	auto* context = globals::d3d::context;

	// The window origins are in VOXEL units, so a size change makes every
	// stored voxel's world position wrong: start the windows over.
	const float voxelSize0 = VoxelSizeLive();
	if (voxelSize0 != voxelSizeBuilt) {
		voxelSizeBuilt = voxelSize0;
		for (auto& lv : voxelLevels)
			lv.valid = false;
	}
	const uint levels = VoxelLevelsLive();
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	constexpr UINT groups = kVoxelDim / 8;
	const bool counting = voxelCountUAV && voxelCountStaging[0] && voxelCountStaging[1];

	// ---- Occupancy: scroll + decay, then this frame's captures ----
	globals::profiler->BeginPass("SnowDeformation::VoxelVolume");
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		const float voxelSize = VoxelSizeForLevel(L);
		const float half = kVoxelDim * voxelSize * 0.5f;
		// The cube centred on the camera, its origin snapped to the lattice.
		const DirectX::XMINT3 origin{
			(int)std::floor((eye.x - half) / voxelSize),
			(int)std::floor((eye.y - half) / voxelSize),
			(int)std::floor((eye.z - half) / voxelSize)
		};
		const DirectX::XMINT3 delta{ origin.x - lv.origin.x, origin.y - lv.origin.y, origin.z - lv.origin.z };
		const bool clearAll = !lv.valid;
		lv.origin = origin;
		lv.valid = true;
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		cb.OriginVox.w = clearAll ? 1 : 0;
		cb.ScrollDelta = { delta.x, delta.y, delta.z, 0 };
		voxelCB->Update(cb);

		const uint previous = lv.current;
		lv.current ^= 1;
		{
			// Level 0 carries the menu's occupancy count.
			const bool countHere = counting && L == 0;
			if (countHere) {
				const UINT zeros[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(voxelCountUAV.get(), zeros);
			}
			ID3D11ShaderResourceView* srv = lv.volume[previous]->srv.get();
			ID3D11UnorderedAccessView* uavs[3] = { lv.volume[lv.current]->uav.get(), nullptr, countHere ? voxelCountUAV.get() : nullptr };
			context->CSSetConstantBuffers(0, 1, &cbPtr);
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
			context->CSSetShader(voxelScrollCS, nullptr, 0);
			context->Dispatch(groups, groups, groups);
			ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			// Copy this frame's count, map LAST frame's copy without waiting.
			if (countHere) {
				context->CopyResource(voxelCountStaging[voxelCountCursor].get(), voxelCountBuffer.get());
				voxelCountCursor ^= 1;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(context->Map(voxelCountStaging[voxelCountCursor].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
					voxelOccupancy = *static_cast<const uint32_t*>(mapped.pData);
					context->Unmap(voxelCountStaging[voxelCountCursor].get(), 0);
					voxelOccupancyValid = true;
				}
			}
		}

		// UAV-only raster: no target, the viewport sizes it. The game's VS b0
		// goes back afterwards; the capture pass never touched it.
		winrt::com_ptr<ID3D11RasterizerState> savedRaster;
		context->RSGetState(savedRaster.put());
		winrt::com_ptr<ID3D11Buffer> savedVSCB0;
		context->VSGetConstantBuffers(0, 1, savedVSCB0.put());
		context->RSSetState(voxelRasterState.get());
		D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(kVoxelDim), float(kVoxelDim), 0.0f, 1.0f };
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11UnorderedAccessView* volumeUAV = lv.volume[lv.current]->uav.get();
		context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &volumeUAV, nullptr);
		context->VSSetShader(voxelVS, nullptr, 0);
		context->GSSetShader(voxelGS, nullptr, 0);
		context->PSSetShader(voxelPS, nullptr, 0);
		context->VSSetConstantBuffers(0, 1, &cbPtr);
		context->GSSetConstantBuffers(0, 1, &cbPtr);
		context->PSSetConstantBuffers(0, 1, &cbPtr);
		ID3D11Buffer* cb1 = staticsCB->CB();
		context->VSSetConstantBuffers(1, 1, &cb1);

		for (uint32_t ci = 0; ci < a_captureCount; ci++) {
			const auto& cap = capturedStatics[ci];
			// Roads and bridges belong to the trench patch, which drapes the
			// road heightfield itself; the S4 skin excludes them for the same
			// reason. Voxelising them put volume snow over every RoadChunk
			// (Josef, 2026-09-06).
			if (cap.road || cap.bridge)
				continue;
			auto* geometry = cap.geometry.get();
			if (!geometry)
				continue;
			auto triShape = geometry->AsTriShape();
			if (!triShape)
				continue;
			auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
			if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				continue;
			uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
			if (indexCount == 0)
				continue;
			auto desc = rendererData->vertexDesc;
			if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
				continue;
			uint64_t descKey;
			memcpy(&descKey, &desc, sizeof(descKey));
			auto layoutIt = staticsILCache.find(descKey);
			if (layoutIt == staticsILCache.end() || !layoutIt->second)
				continue;
			context->IASetInputLayout(layoutIt->second.get());
			UINT stride = uint32_t(descKey & 0xF) * 4;
			if (stride == 0)
				continue;
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			if (a_recordsLive)
				BindStaticsRecord(ci, false, false, a_parity);
			else
				staticsCB->Update(a_records[ci]);
			context->DrawIndexed(indexCount, 0, 0);
		}

		context->GSSetShader(nullptr, nullptr, 0);
		context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &nullUAV, nullptr);
		ID3D11Buffer* restoreVSCB0 = savedVSCB0.get();
		context->VSSetConstantBuffers(0, 1, &restoreVSCB0);
		context->GSSetConstantBuffers(0, 1, &nullCB);
		context->PSSetConstantBuffers(0, 1, &nullCB);
		context->RSSetState(savedRaster.get());
	}
	globals::profiler->EndPass();

	// ---- The snow field, then the draw's brick list, per level ----
	// Seeds into the dead ping-pong volume (nothing reads it again before
	// next frame's scroll overwrites it), then three separable passes
	// bouncing between it and the field, ending in the field. UP FIRST:
	// the sideways passes then run in the air above surfaces, where the
	// solid blocker (occupancy at t4) can stop them at a riser or a wall
	// instead of at the surface's own neighbouring voxels.
	globals::profiler->BeginPass("SnowDeformation::VoxelField");
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		Texture3D* scratch = lv.volume[lv.current ^ 1];
		Texture3D* occupancy = lv.volume[lv.current];
		auto runPass = [&](ID3D11ComputeShader* a_cs, Texture3D* a_in, Texture3D* a_out) {
			ID3D11ShaderResourceView* srv = a_in->srv.get();
			ID3D11UnorderedAccessView* uav = a_out->uav.get();
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(a_cs, nullptr, 0);
			context->Dispatch(groups, groups, groups);
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		};
		cb.BlurAxis = 2;
		voxelCB->Update(cb);
		context->CSSetConstantBuffers(0, 1, &cbPtr);
		ID3D11ShaderResourceView* mapSRVs[2] = {
			seedMaps ? heightBottomFiltered->srv.get() : nullptr,
			seedMaps ? objectSkyOpen->srv.get() : nullptr
		};
		context->CSSetShaderResources(1, 2, mapSRVs);
		runPass(voxelSeedCS, occupancy, scratch);
		ID3D11ShaderResourceView* nullMapSRVs[2] = { nullptr, nullptr };
		context->CSSetShaderResources(1, 2, nullMapSRVs);
		// Every blur pass reads the occupancy at t4: Z stops at the first
		// solid beneath, X and Y at the first solid beside.
		ID3D11ShaderResourceView* blockerSRV = occupancy->srv.get();
		context->CSSetShaderResources(4, 1, &blockerSRV);
		// Z: scratch -> field.
		runPass(voxelBlurCS, scratch, lv.field);
		cb.BlurAxis = 0;
		voxelCB->Update(cb);
		runPass(voxelBlurCS, lv.field, scratch);
		cb.BlurAxis = 1;
		voxelCB->Update(cb);
		runPass(voxelBlurCS, scratch, lv.field);
		context->CSSetShaderResources(4, 1, &nullSRV);

		// The brick list. Binding the append UAV with a zero initial count
		// resets its counter; the count then becomes the instance count.
		{
			const UINT zeroCount = 0;
			ID3D11UnorderedAccessView* listUAV = lv.bricks->uav.get();
			ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
			context->CSSetUnorderedAccessViews(3, 1, &listUAV, &zeroCount);
			context->CSSetShaderResources(3, 1, &fieldSRV);
			context->CSSetShader(voxelBrickListCS, nullptr, 0);
			constexpr UINT brickGroups = kVoxelBricksPerAxis / 8;
			context->Dispatch(brickGroups, brickGroups, brickGroups);
			context->CSSetShaderResources(3, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(3, 1, &nullUAV, nullptr);
			context->CopyStructureCount(lv.drawArgs->resource.get(), 4, listUAV);
		}
		context->CSSetShader(nullptr, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &nullCB);
	}
	globals::profiler->EndPass();

	if (showVoxelSlice)
		UpdateVoxelSliceTexture();
}

void SnowDeformation::UpdateVoxelSliceTexture()
{
	const uint level = std::min((uint)std::max(voxelSliceLevel, 0), VoxelLevelsLive() - 1);
	const auto& lv = voxelLevels[level];
	if (!voxelSliceCS || !voxelSliceTexture || !voxelCB || !lv.valid || !lv.volume[lv.current] || !lv.field)
		return;
	auto* context = globals::d3d::context;
	VoxelVolumeCB cb{};
	FillVoxelCB(level, cb);
	voxelCB->Update(cb);
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* srv = lv.volume[lv.current]->srv.get();
	ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
	ID3D11UnorderedAccessView* uavs[2] = { nullptr, voxelSliceTexture->uav.get() };
	context->CSSetConstantBuffers(0, 1, &cbPtr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetShaderResources(3, 1, &fieldSRV);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(voxelSliceCS, nullptr, 0);
	context->Dispatch(kVoxelDim / 8, kVoxelDim / 8, 1);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetShaderResources(3, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &nullCB);
}

void SnowDeformation::DrawVoxelSnow()
{
	if (!settings.VolumeSnow || !voxelDrawCB || !voxelWrapSampler || !voxelShellVS || !voxelShellPS)
		return;
	const uint levels = VoxelLevelsLive();
	if (!voxelLevels[0].valid)
		return;
	auto* context = globals::d3d::context;

	// One block for the whole draw, as the patch does: rounded class at the
	// volume's depth, the window for the footprint reads, no coat, no relief.
	StaticsCB scb{};
	scb.ObjectsDepth = settings.VolumeSnowDepth;
	scb.RoundedDepth = settings.VolumeSnowDepth;
	scb.HeightWindowCenter = heightWindowCenter;
	scb.HeightHalfExtent = kHeightMapHalfExtent;
	scb.HasObjectTop = 1.0f;
	scb.ClassOverride = 1.0f;
	staticsCB->Update(scb);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);

	ID3D11SamplerState* wrap = voxelWrapSampler.get();
	context->PSSetSamplers(3, 1, &wrap);
	// What the skin pass unbinds and the patch rebinds: the march's depth
	// cap and SkinShadeSurface's footprint reads.
	ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->PSSetShaderResources(3, 1, &sceneDepthSRV);
	ID3D11ShaderResourceView* topSRV = (heightTopRaw[heightCurrent] && heightTopRaw[heightCurrent]->srv) ? heightTopRaw[heightCurrent]->srv.get() : nullptr;
	context->PSSetShaderResources(11, 1, &topSRV);
	ID3D11ShaderResourceView* skinDepthSRV = (heightSkinDepth && heightSkinDepth->srv) ? heightSkinDepth->srv.get() : nullptr;
	context->PSSetShaderResources(12, 1, &skinDepthSRV);

	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	// Both faces: the PS keeps the back one by distance (see the shader).
	context->RSSetState(voxelRasterState.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->IASetInputLayout(nullptr);
	ID3D11Buffer* nullVB = nullptr;
	UINT zero = 0;
	context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(voxelShellVS, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(voxelShellPS, nullptr, 0);

	globals::profiler->BeginPass("SnowDeformation::VolumeSnow");
	ID3D11Buffer* cb2 = voxelDrawCB->CB();
	for (uint L = 0; L < levels; L++) {
		const auto& lv = voxelLevels[L];
		if (!lv.valid || !lv.field || !lv.bricks || !lv.drawArgs)
			continue;
		const float voxelSize = VoxelSizeForLevel(L);
		VoxelDrawCB d{};
		d.VoxOrigin = { lv.origin.x, lv.origin.y, lv.origin.z, 0 };
		d.VoxParams = { voxelSize, float(kVoxelDim), std::clamp(settings.VolumeSnowCoverage, 0.05f, 0.95f), 0.5f };
		// Hand-over bands: in over the finer level's outer band (none on
		// level 0: a band below zero reads as fully in), out over this one's.
		float inStart = -2.0f, inEnd = -1.0f, outStart = 0.0f, outEnd = 0.0f;
		if (L > 0)
			VoxelReachBand(L - 1, inStart, inEnd);
		VoxelReachBand(L, outStart, outEnd);
		d.VoxFade = { inStart, inEnd, outStart, outEnd };
		voxelDrawCB->Update(d);
		context->VSSetConstantBuffers(2, 1, &cb2);
		context->PSSetConstantBuffers(2, 1, &cb2);
		ID3D11ShaderResourceView* bricksSRV = lv.bricks->srv.get();
		context->VSSetShaderResources(41, 1, &bricksSRV);
		ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
		context->PSSetShaderResources(42, 1, &fieldSRV);
		context->DrawInstancedIndirect(lv.drawArgs->resource.get(), 0);
	}
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetShaderResources(41, 1, &nullSRV);
	context->PSSetShaderResources(42, 1, &nullSRV);
	context->PSSetShaderResources(3, 1, &nullSRV);
	context->PSSetSamplers(3, 1, &nullSampler);
	context->VSSetConstantBuffers(2, 1, &nullCB);
	context->PSSetConstantBuffers(2, 1, &nullCB);
	context->RSSetState(savedRaster.get());
}
