// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/D3D.h"

// Voxel occupancy volume (VOLUME-SNOW-PLAN V0): the captured statics
// rasterised into a camera-anchored 3D window. Nothing consumes it yet;
// the slice view is the deliverable.

bool SnowDeformation::EnsureVoxelResources()
{
	if (voxelVolume[0] && voxelVolume[1] && voxelField && voxelSliceTexture && voxelCB && voxelRasterState &&
		voxelVS && voxelGS && voxelPS && voxelScrollCS && voxelSliceCS && voxelSeedCS && voxelBlurCS &&
		voxelBrickBuffer && voxelDrawArgs && voxelDrawCB && voxelWrapSampler && voxelBrickListCS)
		return true;
	if (voxelShadersFailed)
		return false;
	LoadTraceScope _loadTrace(this, "Volume: EnsureVoxelResources");
	auto* device = globals::d3d::device;

	if (!voxelVolume[0] || !voxelVolume[1]) {
		D3D11_TEXTURE3D_DESC desc{
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
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kVoxelDim }
		};
		// Fresh VRAM is not blank: cleared so a never-written volume reads
		// empty rather than as whatever lived there before.
		const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (int i = 0; i < 2; i++) {
			if (voxelVolume[i])
				continue;
			voxelVolume[i] = new Texture3D(desc, i ? "SnowDeformation::VoxelVolume1" : "SnowDeformation::VoxelVolume0");
			voxelVolume[i]->CreateSRV(srvDesc);
			voxelVolume[i]->CreateUAV(uavDesc);
			globals::d3d::context->ClearUnorderedAccessViewFloat(voxelVolume[i]->uav.get(), clearZero);
		}
		if (!voxelField) {
			voxelField = new Texture3D(desc, "SnowDeformation::VoxelField");
			voxelField->CreateSRV(srvDesc);
			voxelField->CreateUAV(uavDesc);
			globals::d3d::context->ClearUnorderedAccessViewFloat(voxelField->uav.get(), clearZero);
		}
		voxelValid = false;
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
		const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		globals::d3d::context->ClearUnorderedAccessViewFloat(voxelSliceTexture->uav.get(), clearZero);
	}
	if (!voxelCB)
		voxelCB = new ConstantBuffer(ConstantBufferDesc<VoxelVolumeCB>(), "SnowDeformation::VoxelVolumeCB");
	if (!voxelDrawCB)
		voxelDrawCB = new ConstantBuffer(ConstantBufferDesc<VoxelDrawCB>(), "SnowDeformation::VoxelDrawCB");
	if (!voxelBrickBuffer) {
		D3D11_BUFFER_DESC desc{};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(uint32_t);
		desc.ByteWidth = sizeof(uint32_t) * kVoxelBrickCapacity;
		voxelBrickBuffer = new Buffer(desc, nullptr, "SnowDeformation::VoxelBricks");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = kVoxelBrickCapacity;
		voxelBrickBuffer->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.NumElements = kVoxelBrickCapacity;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
		voxelBrickBuffer->CreateUAV(uavDesc);
	}
	if (!voxelDrawArgs) {
		// {VertexCountPerInstance, InstanceCount, StartVertex, StartInstance};
		// only the count changes, copied from the list's counter.
		const uint32_t args[4] = { 36, 0, 0, 0 };
		D3D11_SUBRESOURCE_DATA init{ args, 0, 0 };
		D3D11_BUFFER_DESC desc{};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.ByteWidth = sizeof(args);
		desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
		voxelDrawArgs = new Buffer(desc, &init, "SnowDeformation::VoxelDrawArgs");
	}
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
	if (!voxelScrollCS) {
		voxelScrollCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", "VoxelScrollCS"));
		if (voxelScrollCS)
			Util::SetResourceName(voxelScrollCS, "SnowDeformation::VoxelScrollCS");
	}
	if (!voxelSliceCS) {
		voxelSliceCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", "VoxelSliceCS"));
		if (voxelSliceCS)
			Util::SetResourceName(voxelSliceCS, "SnowDeformation::VoxelSliceCS");
	}
	if (!voxelSeedCS) {
		voxelSeedCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", "VoxelSeedCS"));
		if (voxelSeedCS)
			Util::SetResourceName(voxelSeedCS, "SnowDeformation::VoxelSeedCS");
	}
	if (!voxelBlurCS) {
		voxelBlurCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", "VoxelBlurCS"));
		if (voxelBlurCS)
			Util::SetResourceName(voxelBlurCS, "SnowDeformation::VoxelBlurCS");
	}
	if (!voxelBrickListCS) {
		voxelBrickListCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", "VoxelBrickListCS"));
		if (voxelBrickListCS)
			Util::SetResourceName(voxelBrickListCS, "SnowDeformation::VoxelBrickListCS");
	}
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

void SnowDeformation::RenderVoxelVolume(const StaticsCB* a_records, uint32_t a_captureCount, bool a_recordsLive, uint32_t& a_parity)
{
	if (!settings.VolumeSnow) {
		voxelValid = false;
		return;
	}
	if (!EnsureVoxelResources())
		return;
	auto* context = globals::d3d::context;

	// The cube centred on the camera, its origin snapped to the lattice.
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	constexpr float half = kVoxelDim * kVoxelSize * 0.5f;
	const DirectX::XMINT3 origin{
		(int)std::floor((eye.x - half) / kVoxelSize),
		(int)std::floor((eye.y - half) / kVoxelSize),
		(int)std::floor((eye.z - half) / kVoxelSize)
	};

	VoxelVolumeCB cb{};
	cb.OriginVox = { origin.x, origin.y, origin.z, voxelValid ? 0 : 1 };
	cb.ScrollDelta = { origin.x - voxelOriginVox.x, origin.y - voxelOriginVox.y, origin.z - voxelOriginVox.z, 0 };
	cb.VoxelSize = kVoxelSize;
	cb.Decay = 1.0f / std::max(voxelMemorySeconds * 60.0f, 1.0f);
	cb.Dim = (int)kVoxelDim;
	cb.SliceAxis = std::clamp(voxelSliceAxis, 0, 2);
	cb.SliceXray = voxelSliceXray ? 1 : 0;
	cb.SliceSource = std::clamp(voxelSliceSource, 0, 2);
	// The seed gate's maps live in the height window; without them every
	// column reads as open sky (HalfExtent 0 fails the window test).
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	cb.HeightWindowCenter = heightWindowCenter;
	cb.HeightHalfExtent = seedMaps ? kHeightMapHalfExtent : 0.0f;
	cb.ShelterDust = kVoxelShelterDust;
	// sigma such that coverage 0.5 lands the isosurface at the slider depth:
	// exp(-h^2 / 2 s^2) = 0.5 -> h = 1.177 s.
	cb.SeedSigma = std::max(settings.VolumeSnowDepth, 4.0f) / (1.177f * kVoxelSize);
	cb.FieldThreshold = std::clamp(settings.VolumeSnowCoverage, 0.05f, 0.95f);
	cb.EyeVox[0] = eye.x / kVoxelSize;
	cb.EyeVox[1] = eye.y / kVoxelSize;
	cb.EyeVox[2] = eye.z / kVoxelSize;
	cb.EyeVox[3] = kVoxelFadeEndFrac * half / kVoxelSize;
	{
		const float along = cb.SliceAxis == 0 ? eye.z : (cb.SliceAxis == 1 ? eye.y : eye.x);
		const int originAlong = cb.SliceAxis == 0 ? origin.z : (cb.SliceAxis == 1 ? origin.y : origin.x);
		cb.SliceIndex = std::clamp((int)std::floor((along + voxelSliceOffset) / kVoxelSize) - originAlong, 0, (int)kVoxelDim - 1);
	}
	voxelCB->Update(cb);
	voxelOriginVox = origin;
	voxelValid = true;

	const uint previous = voxelCurrent;
	voxelCurrent ^= 1;
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;

	globals::profiler->BeginPass("SnowDeformation::VoxelVolume");
	{
		const bool counting = voxelCountUAV && voxelCountStaging[0] && voxelCountStaging[1];
		if (counting) {
			const UINT zeros[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(voxelCountUAV.get(), zeros);
		}
		ID3D11ShaderResourceView* srv = voxelVolume[previous]->srv.get();
		ID3D11UnorderedAccessView* uavs[3] = { voxelVolume[voxelCurrent]->uav.get(), nullptr, counting ? voxelCountUAV.get() : nullptr };
		context->CSSetConstantBuffers(0, 1, &cbPtr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->CSSetShader(voxelScrollCS, nullptr, 0);
		constexpr UINT groups = kVoxelDim / 8;
		context->Dispatch(groups, groups, groups);
		ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &nullCB);

		// Copy this frame's count, map LAST frame's copy without waiting.
		if (counting) {
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
	ID3D11UnorderedAccessView* volumeUAV = voxelVolume[voxelCurrent]->uav.get();
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
	globals::profiler->EndPass();

	context->GSSetShader(nullptr, nullptr, 0);
	context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, &nullUAV, nullptr);
	ID3D11Buffer* restoreVSCB0 = savedVSCB0.get();
	context->VSSetConstantBuffers(0, 1, &restoreVSCB0);
	context->GSSetConstantBuffers(0, 1, &nullCB);
	context->PSSetConstantBuffers(0, 1, &nullCB);
	context->RSSetState(savedRaster.get());

	// V1a: the snow field. Seeds into the dead ping-pong volume (nothing
	// reads it again before next frame's scroll overwrites it), then three
	// separable passes bouncing between it and the field, ending in the
	// field. Every pass fully overwrites its output.
	{
		globals::profiler->BeginPass("SnowDeformation::VoxelField");
		Texture3D* scratch = voxelVolume[previous];
		constexpr UINT groups = kVoxelDim / 8;
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
		context->CSSetConstantBuffers(0, 1, &cbPtr);
		ID3D11ShaderResourceView* mapSRVs[2] = {
			seedMaps ? heightBottomFiltered->srv.get() : nullptr,
			seedMaps ? objectSkyOpen->srv.get() : nullptr
		};
		context->CSSetShaderResources(1, 2, mapSRVs);
		runPass(voxelSeedCS, voxelVolume[voxelCurrent], scratch);
		ID3D11ShaderResourceView* nullMapSRVs[2] = { nullptr, nullptr };
		context->CSSetShaderResources(1, 2, nullMapSRVs);
		Texture3D* blurIn = scratch;
		Texture3D* blurOut = voxelField;
		for (int axis = 0; axis < 3; axis++) {
			cb.BlurAxis = axis;
			voxelCB->Update(cb);
			runPass(voxelBlurCS, blurIn, blurOut);
			std::swap(blurIn, blurOut);
		}
		// Three swaps: the result is in the field, the scratch is dead again.

		// V1b: the brick list for the draw. Binding the append UAV with a
		// zero initial count resets its counter; the count then becomes the
		// instance count.
		if (settings.VolumeSnowDraw) {
			const UINT zeroCount = 0;
			ID3D11UnorderedAccessView* listUAV = voxelBrickBuffer->uav.get();
			ID3D11ShaderResourceView* fieldSRV = voxelField->srv.get();
			context->CSSetUnorderedAccessViews(3, 1, &listUAV, &zeroCount);
			context->CSSetShaderResources(3, 1, &fieldSRV);
			context->CSSetShader(voxelBrickListCS, nullptr, 0);
			constexpr UINT brickGroups = kVoxelBricksPerAxis / 8;
			context->Dispatch(brickGroups, brickGroups, brickGroups);
			context->CSSetShaderResources(3, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(3, 1, &nullUAV, nullptr);
			context->CopyStructureCount(voxelDrawArgs->resource.get(), 4, listUAV);
		}
		context->CSSetShader(nullptr, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &nullCB);
		globals::profiler->EndPass();
	}

	if (showVoxelSlice)
		UpdateVoxelSliceTexture();
}

void SnowDeformation::DrawVoxelSnow()
{
	if (!settings.VolumeSnow || !settings.VolumeSnowDraw || !voxelValid || !voxelField ||
		!voxelBrickBuffer || !voxelDrawArgs || !voxelDrawCB || !voxelWrapSampler || !voxelShellVS || !voxelShellPS)
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

	constexpr float half = kVoxelDim * kVoxelSize * 0.5f;
	VoxelDrawCB d{};
	d.VoxOrigin = { voxelOriginVox.x, voxelOriginVox.y, voxelOriginVox.z, 0 };
	d.VoxParams = { kVoxelSize, float(kVoxelDim), std::clamp(settings.VolumeSnowCoverage, 0.05f, 0.95f), 0.5f };
	d.VoxFade = { kVoxelFadeStartFrac * half, kVoxelFadeEndFrac * half, 0.0f, 0.0f };
	voxelDrawCB->Update(d);
	ID3D11Buffer* cb2 = voxelDrawCB->CB();
	context->VSSetConstantBuffers(2, 1, &cb2);
	context->PSSetConstantBuffers(2, 1, &cb2);

	ID3D11ShaderResourceView* bricksSRV = voxelBrickBuffer->srv.get();
	context->VSSetShaderResources(41, 1, &bricksSRV);
	ID3D11ShaderResourceView* fieldSRV = voxelField->srv.get();
	context->PSSetShaderResources(42, 1, &fieldSRV);
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
	context->DrawInstancedIndirect(voxelDrawArgs->resource.get(), 0);
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

void SnowDeformation::UpdateVoxelSliceTexture()
{
	if (!voxelSliceCS || !voxelSliceTexture || !voxelCB || !voxelValid || !voxelVolume[voxelCurrent] || !voxelField)
		return;
	auto* context = globals::d3d::context;
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* srv = voxelVolume[voxelCurrent]->srv.get();
	ID3D11ShaderResourceView* fieldSRV = voxelField->srv.get();
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
