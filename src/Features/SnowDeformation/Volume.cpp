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
	if (voxelVolume[0] && voxelVolume[1] && voxelSliceTexture && voxelCB && voxelRasterState &&
		voxelVS && voxelGS && voxelPS && voxelScrollCS && voxelSliceCS)
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
	if (!voxelVS || !voxelGS || !voxelPS || !voxelScrollCS || !voxelSliceCS) {
		voxelShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Voxel volume disabled (shader compilation failed: VS {} GS {} PS {} ScrollCS {} SliceCS {})",
			voxelVS ? "ok" : "FAILED", voxelGS ? "ok" : "FAILED", voxelPS ? "ok" : "FAILED",
			voxelScrollCS ? "ok" : "FAILED", voxelSliceCS ? "ok" : "FAILED");
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

	if (showVoxelSlice)
		UpdateVoxelSliceTexture();
}

void SnowDeformation::UpdateVoxelSliceTexture()
{
	if (!voxelSliceCS || !voxelSliceTexture || !voxelCB || !voxelValid || !voxelVolume[voxelCurrent])
		return;
	auto* context = globals::d3d::context;
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* srv = voxelVolume[voxelCurrent]->srv.get();
	ID3D11UnorderedAccessView* uavs[2] = { nullptr, voxelSliceTexture->uav.get() };
	context->CSSetConstantBuffers(0, 1, &cbPtr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(voxelSliceCS, nullptr, 0);
	context->Dispatch(kVoxelDim / 8, kVoxelDim / 8, 1);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &nullCB);
}
