// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <cmath>

ID3D11ComputeShader* SnowDeformation::GetBloodTileCS(BloodTileShader a_which)
{
	static constexpr const char* defines[kBloodTileShaderCount] = { "SEED_TILE", "MERGE_CLOCK", "MERGE_PIGMENT", "MIP_DOWN", "COPY_RESIDUE" };
	if (!bloodTileCS[a_which]) {
		logger::debug("Compiling BloodTilesCS:{}", defines[a_which]);
		bloodTileCS[a_which] = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\BloodTilesCS.hlsl", { { defines[a_which], "" } }, "cs_5_0"));
	}
	return bloodTileCS[a_which];
}

void SnowDeformation::ReleaseBloodTiles()
{
	for (auto*& shader : bloodTileCS) {
		if (shader)
			shader->Release();
		shader = nullptr;
	}
	bloodTilesFailed = false;
}

void SnowDeformation::DropBloodTiles()
{
	for (auto& tile : bloodTiles) {
		tile.live = false;
		tile.draws.clear();
	}
	bloodTilesLive = 0;
	bloodTileIndexDirty = true;
	bloodTileEpoch++;
	if (bloodTileAtlasRows > 1)
		SizeBloodTileAtlas(1);
}

bool SnowDeformation::SizeBloodTileAtlas(uint32_t a_rows)
{
	auto* device = globals::d3d::device;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = kBloodTileDim * BloodTilesAcross();
	desc.Height = kBloodTileDim;
	desc.MipLevels = kBloodTileMips;
	desc.ArraySize = a_rows;
	desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	winrt::com_ptr<ID3D11Texture2D> atlas;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, atlas.put()))) {
		logger::error("[SNOW DEFORMATION] blood tiles: a {}-row atlas failed", a_rows);
		return false;
	}
	Util::SetResourceName(atlas.get(), "SnowDeformation::BloodTileAtlas");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = kBloodTileMips, .FirstArraySlice = 0, .ArraySize = a_rows };
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	if (FAILED(device->CreateShaderResourceView(atlas.get(), &srvDesc, srv.put())))
		return false;
	Util::SetResourceName(srv.get(), "SnowDeformation::BloodTileAtlas SRV");
	winrt::com_ptr<ID3D11UnorderedAccessView> uavs[kBloodTileMips];
	for (uint32_t mip = 0; mip < kBloodTileMips; ++mip) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		uavDesc.Texture2DArray = { .MipSlice = mip, .FirstArraySlice = 0, .ArraySize = a_rows };
		if (FAILED(device->CreateUnorderedAccessView(atlas.get(), &uavDesc, uavs[mip].put())))
			return false;
		Util::SetResourceName(uavs[mip].get(), "SnowDeformation::BloodTileAtlas UAV");
	}
	auto* context = globals::d3d::context;
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (uint32_t mip = 0; mip < kBloodTileMips; ++mip)
		context->ClearUnorderedAccessViewFloat(uavs[mip].get(), zero);
	const uint32_t kept = bloodTileAtlas ? std::min(bloodTileAtlasRows, a_rows) : 0u;
	for (uint32_t row = 0; row < kept; ++row)
		for (uint32_t mip = 0; mip < kBloodTileMips; ++mip) {
			const UINT sub = D3D11CalcSubresource(mip, row, kBloodTileMips);
			context->CopySubresourceRegion(atlas.get(), sub, 0, 0, 0, bloodTileAtlas.get(), sub, nullptr);
		}
	bloodTileAtlas = atlas;
	bloodTileAtlasSRV = srv;
	for (uint32_t mip = 0; mip < kBloodTileMips; ++mip)
		bloodTileAtlasUAV[mip] = uavs[mip];
	bloodTileAtlasRows = a_rows;
	logger::info("[SNOW DEFORMATION] blood tile atlas: {} row{} of {} tiles", a_rows, a_rows == 1 ? "" : "s", BloodTilesAcross());
	return true;
}

bool SnowDeformation::EnsureBloodTileResources()
{
	if (bloodTilesFailed)
		return false;
	if (bloodTileCB)
		return true;
	static_assert(sizeof(BloodTileCB) == 96);
	static_assert(offsetof(ShellCB, BloodLook3) == 784);
	auto* device = globals::d3d::device;
	bool ok = true;
	auto texture = [&](winrt::com_ptr<ID3D11Texture2D>& a_out, uint32_t a_dim, DXGI_FORMAT a_format, UINT a_bind, uint32_t a_mips, const char* a_name) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_dim;
		desc.Height = a_dim;
		desc.MipLevels = a_mips;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = a_bind;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, a_out.put()))) {
			logger::error("[SNOW DEFORMATION] blood tiles: texture '{}' failed", a_name);
			ok = false;
			return;
		}
		Util::SetResourceName(a_out.get(), a_name);
	};
	auto srv = [&](winrt::com_ptr<ID3D11ShaderResourceView>& a_out, ID3D11Texture2D* a_texture, DXGI_FORMAT a_format, uint32_t a_mips, const char* a_name) {
		if (!a_texture)
			return;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = a_format;
		desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MostDetailedMip = 0;
		desc.Texture2D.MipLevels = a_mips;
		if (FAILED(device->CreateShaderResourceView(a_texture, &desc, a_out.put()))) {
			logger::error("[SNOW DEFORMATION] blood tiles: view '{}' failed", a_name);
			ok = false;
			return;
		}
		Util::SetResourceName(a_out.get(), a_name);
	};
	auto uav = [&](winrt::com_ptr<ID3D11UnorderedAccessView>& a_out, ID3D11Texture2D* a_texture, DXGI_FORMAT a_format, uint32_t a_mip, const char* a_name) {
		if (!a_texture)
			return;
		D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
		desc.Format = a_format;
		desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipSlice = a_mip;
		if (FAILED(device->CreateUnorderedAccessView(a_texture, &desc, a_out.put()))) {
			logger::error("[SNOW DEFORMATION] blood tiles: view '{}' failed", a_name);
			ok = false;
			return;
		}
		Util::SetResourceName(a_out.get(), a_name);
	};
	auto rtv = [&](winrt::com_ptr<ID3D11RenderTargetView>& a_out, ID3D11Texture2D* a_texture, DXGI_FORMAT a_format, const char* a_name) {
		if (!a_texture)
			return;
		D3D11_RENDER_TARGET_VIEW_DESC desc{};
		desc.Format = a_format;
		desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		if (FAILED(device->CreateRenderTargetView(a_texture, &desc, a_out.put()))) {
			logger::error("[SNOW DEFORMATION] blood tiles: view '{}' failed", a_name);
			ok = false;
			return;
		}
		Util::SetResourceName(a_out.get(), a_name);
	};

	constexpr UINT kRead = D3D11_BIND_SHADER_RESOURCE;
	constexpr UINT kCompute = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	constexpr UINT kTarget = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	constexpr UINT kComputeTarget = kCompute | D3D11_BIND_RENDER_TARGET;
	const uint32_t blocks = kBloodTileDim / kBloodTileBlock;

	ok = SizeBloodTileAtlas(1);

	texture(bloodTileClock, blocks * BloodTilesAcross(), DXGI_FORMAT_R32G32B32A32_FLOAT, kCompute, 1, "SnowDeformation::BloodTileClock");
	srv(bloodTileClockSRV, bloodTileClock.get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 1, "SnowDeformation::BloodTileClock SRV");
	uav(bloodTileClockUAV, bloodTileClock.get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, "SnowDeformation::BloodTileClock UAV");

	texture(bloodTileIndex, uint32_t(kBloodTileIndexDim), DXGI_FORMAT_R8_UINT, kRead, 1, "SnowDeformation::BloodTileIndex");
	srv(bloodTileIndexSRV, bloodTileIndex.get(), DXGI_FORMAT_R8_UINT, 1, "SnowDeformation::BloodTileIndex SRV");

	texture(bloodTileScratch, kBloodTileDim, DXGI_FORMAT_R8G8B8A8_TYPELESS, kTarget, 1, "SnowDeformation::BloodTileScratch");
	rtv(bloodTileScratchRTV, bloodTileScratch.get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "SnowDeformation::BloodTileScratch RTV");
	srv(bloodTileScratchRawSRV, bloodTileScratch.get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, "SnowDeformation::BloodTileScratch raw SRV");
	texture(bloodTileScratchClock, kBloodTileDim, DXGI_FORMAT_R32G32_FLOAT, kTarget, 1, "SnowDeformation::BloodTileScratchClock");
	rtv(bloodTileScratchClockRTV, bloodTileScratchClock.get(), DXGI_FORMAT_R32G32_FLOAT, "SnowDeformation::BloodTileScratchClock RTV");
	srv(bloodTileScratchClockSRV, bloodTileScratchClock.get(), DXGI_FORMAT_R32G32_FLOAT, 1, "SnowDeformation::BloodTileScratchClock SRV");

	const uint32_t snapAtlasDim = kBloodTileSnapDim * BloodTilesAcross();
	texture(bloodTileSnapshot, snapAtlasDim, DXGI_FORMAT_R8G8B8A8_TYPELESS, kCompute, 1, "SnowDeformation::BloodTileSnapshot");
	srv(bloodTileSnapshotSRV, bloodTileSnapshot.get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, "SnowDeformation::BloodTileSnapshot SRV");
	uav(bloodTileSnapshotUAV, bloodTileSnapshot.get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, "SnowDeformation::BloodTileSnapshot UAV");
	texture(bloodTileRedrawn, snapAtlasDim, DXGI_FORMAT_R8G8B8A8_UNORM, kComputeTarget, 1, "SnowDeformation::BloodTileRedrawn");
	rtv(bloodTileRedrawnRTV, bloodTileRedrawn.get(), DXGI_FORMAT_R8G8B8A8_UNORM, "SnowDeformation::BloodTileRedrawn RTV");
	srv(bloodTileRedrawnSRV, bloodTileRedrawn.get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, "SnowDeformation::BloodTileRedrawn SRV");
	uav(bloodTileRedrawnUAV, bloodTileRedrawn.get(), DXGI_FORMAT_R8G8B8A8_UNORM, 0, "SnowDeformation::BloodTileRedrawn UAV");
	texture(bloodTileResidue, kBloodTileSnapDim, DXGI_FORMAT_R16G16B16A16_FLOAT, kCompute, 1, "SnowDeformation::BloodTileResidue");
	srv(bloodTileResidueSRV, bloodTileResidue.get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SnowDeformation::BloodTileResidue SRV");
	uav(bloodTileResidueUAV, bloodTileResidue.get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 0, "SnowDeformation::BloodTileResidue UAV");

	texture(bloodTilePrev, kBloodTileDim, DXGI_FORMAT_R8G8B8A8_TYPELESS, kRead, 1, "SnowDeformation::BloodTilePrev");
	srv(bloodTilePrevRawSRV, bloodTilePrev.get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, "SnowDeformation::BloodTilePrev raw SRV");
	texture(bloodTilePrevClock, blocks, DXGI_FORMAT_R32G32B32A32_FLOAT, kRead, 1, "SnowDeformation::BloodTilePrevClock");
	srv(bloodTilePrevClockSRV, bloodTilePrevClock.get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 1, "SnowDeformation::BloodTilePrevClock SRV");

	for (uint32_t i = 0; i < kBloodTileShaderCount; ++i)
		ok = ok && GetBloodTileCS(BloodTileShader(i)) != nullptr;

	winrt::com_ptr<ID3D11Buffer> cb;
	if (ok) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = sizeof(BloodTileCB);
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ok = SUCCEEDED(device->CreateBuffer(&desc, nullptr, cb.put()));
	}
	if (!ok) {
		logger::error("[SNOW DEFORMATION] blood detail tiles are off (a resource or shader failed); the blood map carries on alone");
		bloodTilesFailed = true;
		return false;
	}
	Util::SetResourceName(cb.get(), "SnowDeformation::BloodTileCB");

	auto* context = globals::d3d::context;
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearUnorderedAccessViewFloat(bloodTileClockUAV.get(), zero);
	bloodTileIndexDirty = true;
	bloodTileCB = cb;
	logger::info("[SNOW DEFORMATION] blood detail tiles ready: {} tiles of {}x{} at {} u/texel", BloodTileCount(), kBloodTileDim, kBloodTileDim, BloodTileTexel());
	return true;
}

void SnowDeformation::BloodMarkBounds(const BloodCapture& a_capture, BloodSeen& a_seen)
{
	if (a_seen.boundsKnown)
		return;
	a_seen.boundsKnown = true;
	auto* geometry = a_capture.geometry.get();
	const auto& bound = geometry->worldBound;
	const float guess = std::max(bound.radius, 96.0f);
	a_seen.boundsMin = { bound.center.x - guess, bound.center.y - guess };
	a_seen.boundsMax = { bound.center.x + guess, bound.center.y + guess };
	bool read = false;
	if (!a_capture.skinned) {
		auto* triShape = geometry->AsTriShape();
		auto* rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (triShape && rendererData && rendererData->rawVertexData) {
			auto desc = rendererData->vertexDesc;
			uint64_t descKey;
			memcpy(&descKey, &desc, sizeof(descKey));
			const uint32_t stride = uint32_t(descKey & 0xF) * 4;
			const uint32_t count = triShape->GetTrishapeRuntimeData().vertexCount;
			if (stride != 0 && count != 0 && count <= 4096 && desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
				const bool full = SD_PositionBytes(descKey, desc) >= 16;
				float2 lo{ FLT_MAX, FLT_MAX };
				float2 hi{ -FLT_MAX, -FLT_MAX };
				for (uint32_t v = 0; v < count; ++v) {
					const uint8_t* base = rendererData->rawVertexData + size_t(v) * stride;
					RE::NiPoint3 p;
					if (full) {
						float f[3];
						memcpy(f, base, sizeof(f));
						p = { f[0], f[1], f[2] };
					} else {
						uint16_t h[3];
						memcpy(h, base, sizeof(h));
						p = { DirectX::PackedVector::XMConvertHalfToFloat(h[0]), DirectX::PackedVector::XMConvertHalfToFloat(h[1]), DirectX::PackedVector::XMConvertHalfToFloat(h[2]) };
					}
					const RE::NiPoint3 w = a_capture.world * p;
					lo = { std::min(lo.x, w.x), std::min(lo.y, w.y) };
					hi = { std::max(hi.x, w.x), std::max(hi.y, w.y) };
				}
				if (hi.x - lo.x < 1024.0f && hi.y - lo.y < 1024.0f && std::abs((lo.x + hi.x) * 0.5f - bound.center.x) < 1024.0f &&
					std::abs((lo.y + hi.y) * 0.5f - bound.center.y) < 1024.0f) {
					a_seen.boundsMin = lo;
					a_seen.boundsMax = hi;
					read = true;
				}
			}
		}
	}
	(read ? bloodTileBoundsRead : bloodTileBoundsGuessed)++;
}

int32_t SnowDeformation::FindBloodTile(int32_t a_cellX, int32_t a_cellY) const
{
	for (uint32_t i = 0; i < kBloodMaxTiles; ++i)
		if (bloodTiles[i].live && bloodTiles[i].cellX == a_cellX && bloodTiles[i].cellY == a_cellY)
			return int32_t(i);
	return -1;
}

int32_t SnowDeformation::AllocateBloodTile(int32_t a_cellX, int32_t a_cellY, int32_t a_cameraCellX, int32_t a_cameraCellY)
{
	auto distance = [&](int32_t a_x, int32_t a_y) {
		return std::max(std::abs(a_x - a_cameraCellX), std::abs(a_y - a_cameraCellY));
	};
	const int32_t wanted = distance(a_cellX, a_cellY);
	if (wanted > BloodTileKeepCells())
		return -1;
	int32_t slot = -1;
	int32_t farthest = wanted + 1;
	const uint32_t count = BloodTileCount();
	for (uint32_t i = 0; i < count; ++i) {
		if (!bloodTiles[i].live) {
			slot = int32_t(i);
			break;
		}
		const int32_t d = distance(bloodTiles[i].cellX, bloodTiles[i].cellY);
		if (d > farthest && bloodTiles[i].draws.empty()) {
			farthest = d;
			slot = int32_t(i);
		}
	}
	if (slot < 0)
		return -1;
	const uint32_t row = uint32_t(slot) / BloodTilesAcross();
	if (row >= bloodTileAtlasRows && !SizeBloodTileAtlas(row + 1))
		return -1;
	auto& tile = bloodTiles[slot];
	if (!tile.live)
		bloodTilesLive++;
	tile.live = true;
	tile.cellX = a_cellX;
	tile.cellY = a_cellY;
	tile.gen = ++bloodTileEpoch;
	tile.lastBurial = bloodBurialClock;
	tile.draws.clear();
	tile.discs = false;
	tile.seedOrigin = windowOrigin;
	const float coarse = deformWorldSize / float(deformMapDim);
	const float span = float(kBloodTileDim) * BloodTileTexel();
	tile.snapValid = true;
	const float worldMin[2] = { float(a_cellX) * BloodTileCell() - kBloodTileApron, float(a_cellY) * BloodTileCell() - kBloodTileApron };
	const float origin[2] = { windowOrigin.x, windowOrigin.y };
	for (int axis = 0; axis < 2; ++axis) {
		const int32_t first = int32_t(std::floor((worldMin[axis] - origin[axis]) / coarse - 0.5f)) - 1;
		const int32_t last = int32_t(std::floor((worldMin[axis] + span - origin[axis]) / coarse - 0.5f)) + 2;
		tile.snapLogical[axis] = first;
		tile.snapValid = tile.snapValid && last - first < int32_t(kBloodTileSnapDim);
	}
	bloodTileIndexDirty = true;
	return slot;
}

void SnowDeformation::FillBloodTileCB(BloodTileCB& a_cb, uint32_t a_slot) const
{
	const auto& tile = bloodTiles[a_slot];
	a_cb.TileTexel[0] = int32_t((a_slot % BloodTilesAcross()) * kBloodTileDim);
	a_cb.TileTexel[1] = int32_t((a_slot / BloodTilesAcross()) * kBloodTileDim);
	a_cb.TileBlock[0] = a_cb.TileTexel[0] / int32_t(kBloodTileBlock);
	a_cb.TileBlock[1] = a_cb.TileTexel[1] / int32_t(kBloodTileBlock);
	a_cb.TileWorldMin = { float(tile.cellX) * BloodTileCell() - kBloodTileApron, float(tile.cellY) * BloodTileCell() - kBloodTileApron };
	a_cb.FineTexel = BloodTileTexel();
	a_cb.Explained = 0;
	a_cb.WindowOrigin = tile.seedOrigin;
	a_cb.CoarseTexel = deformWorldSize / float(deformMapDim);
	a_cb.CoarseDim = int32_t(deformMapDim);
	a_cb.MapOrigin[0] = mapOrigin.x;
	a_cb.MapOrigin[1] = mapOrigin.y;
	a_cb.BurialNow = bloodBurialClock;
	a_cb.BurialRefills = std::max(settings.BloodBurial, 0.01f);
	a_cb.MipTexel[0] = a_cb.TileTexel[0];
	a_cb.MipTexel[1] = a_cb.TileTexel[1];
	a_cb.MipDim = int32_t(kBloodTileDim);
	a_cb.TileRow = int32_t(a_slot / BloodTilesAcross());
	a_cb.SnapTexel[0] = int32_t((a_slot % BloodTilesAcross()) * kBloodTileSnapDim);
	a_cb.SnapTexel[1] = int32_t((a_slot / BloodTilesAcross()) * kBloodTileSnapDim);
	a_cb.SnapLogical[0] = tile.snapLogical[0];
	a_cb.SnapLogical[1] = tile.snapLogical[1];
}

namespace
{
	void UploadTileCB(ID3D11DeviceContext* a_context, ID3D11Buffer* a_buffer, const SnowDeformation::BloodTileCB& a_cb)
	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(a_context->Map(a_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			memcpy(mapped.pData, &a_cb, sizeof(a_cb));
			a_context->Unmap(a_buffer, 0);
		}
	}

	void UnbindCompute(ID3D11DeviceContext* a_context)
	{
		ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
		ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
		a_context->CSSetShaderResources(0, 4, nullSRVs);
		a_context->CSSetUnorderedAccessViews(0, 4, nullUAVs, nullptr);
	}
}

void SnowDeformation::SeedBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot)
{
	BloodTileCB cb{};
	FillBloodTileCB(cb, a_slot);
	UploadTileCB(a_context, bloodTileCB.get(), cb);
	ID3D11ShaderResourceView* srvs[2] = { GetBloodMapSRV(), GetBloodClockSRV() };
	ID3D11UnorderedAccessView* uavs[4] = { bloodTileAtlasUAV[0].get(), bloodTileClockUAV.get(), bloodTileSnapshotUAV.get(), bloodTileRedrawnUAV.get() };
	a_context->CSSetShaderResources(0, 2, srvs);
	a_context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
	a_context->CSSetShader(GetBloodTileCS(kBloodTileSeed), nullptr, 0);
	a_context->Dispatch(kBloodTileDim / 8, kBloodTileDim / 8, 1);
	UnbindCompute(a_context);
	BuildBloodTileMips(a_context, a_slot);
}

void SnowDeformation::MergeBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot, const std::vector<BloodDisc>& a_discs)
{
	auto* context = a_context;
	auto& tile = bloodTiles[a_slot];
	BloodTileCB tileCB{};
	FillBloodTileCB(tileCB, a_slot);

	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearRenderTargetView(bloodTileScratchRTV.get(), zero);
	context->ClearRenderTargetView(bloodTileScratchClockRTV.get(), zero);
	ID3D11RenderTargetView* rtvs[2] = { bloodTileScratchRTV.get(), bloodTileScratchClockRTV.get() };
	context->OMSetRenderTargets(2, rtvs, nullptr);
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(kBloodTileDim), float(kBloodTileDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);

	BloodCB cb{};
	cb.WindowOrigin = tileCB.TileWorldMin;
	cb.TexelSize = BloodTileTexel();
	cb.MapDim = float(kBloodTileDim);
	cb.MapOrigin = { 0, 0 };
	cb.ClockNow = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
	cb.Intensity = std::clamp(settings.BloodIntensity, 0.0f, 2.0f);
	cb.NormalZMin = 0.3f;
	context->PSSetShader(bloodPS, nullptr, 0);
	uint32_t drawn = DrawBloodList(context, tile.draws, cb, nullptr, 1u, true);

	if (tile.discs && !a_discs.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(bloodDiscBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			auto* rows = static_cast<float4*>(mapped.pData);
			for (size_t i = 0; i < a_discs.size(); ++i) {
				const auto& d = a_discs[i];
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
			cb.ClockNow = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
			bloodCB->Update(cb);
			context->VSSetShader(bloodDiscVS, nullptr, 0);
			context->PSSetShader(bloodDiscPS, nullptr, 0);
			ID3D11ShaderResourceView* discSRV = bloodDiscSRV.get();
			context->VSSetShaderResources(1, 1, &discSRV);
			context->IASetInputLayout(nullptr);
			ID3D11Buffer* nullVB = nullptr;
			UINT none = 0;
			context->IASetVertexBuffers(0, 1, &nullVB, &none, &none);
			context->DrawInstanced(6, uint32_t(a_discs.size()) * 4, 0, 0);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			context->VSSetShaderResources(1, 1, &nullSRV);
			drawn++;
		}
	}
	const bool explained = tile.snapValid && !tile.draws.empty();
	if (explained) {
		BloodCB raster = cb;
		const float coarse = tileCB.CoarseTexel;
		raster.WindowOrigin = { tile.seedOrigin.x + float(tile.snapLogical[0]) * coarse, tile.seedOrigin.y + float(tile.snapLogical[1]) * coarse };
		raster.TexelSize = coarse;
		raster.MapDim = float(kBloodTileSnapDim);
		ID3D11RenderTargetView* redrawnRTV = bloodTileRedrawnRTV.get();
		context->OMSetRenderTargets(1, &redrawnRTV, nullptr);
		D3D11_VIEWPORT snapViewport{ float(tileCB.SnapTexel[0]), float(tileCB.SnapTexel[1]), float(kBloodTileSnapDim), float(kBloodTileSnapDim), 0.0f, 1.0f };
		context->RSSetViewports(1, &snapViewport);
		context->PSSetShader(bloodPS, nullptr, 0);
		DrawBloodList(context, tile.draws, raster, nullptr, 1u, true);
	}
	ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
	context->OMSetRenderTargets(2, nullRTVs, nullptr);
	ID3D11ShaderResourceView* nullPS = nullptr;
	context->PSSetShaderResources(0, 1, &nullPS);
	tile.draws.clear();
	tile.discs = false;
	if (drawn == 0)
		return;

	const UINT x = UINT(tileCB.TileTexel[0]);
	const D3D11_BOX pigmentBox{ x, 0, 0, x + kBloodTileDim, kBloodTileDim, 1 };
	context->CopySubresourceRegion(bloodTilePrev.get(), 0, 0, 0, 0, bloodTileAtlas.get(), D3D11CalcSubresource(0, UINT(tileCB.TileRow), kBloodTileMips), &pigmentBox);
	const UINT blocks = kBloodTileDim / kBloodTileBlock;
	const D3D11_BOX clockBox{ UINT(tileCB.TileBlock[0]), UINT(tileCB.TileBlock[1]), 0, UINT(tileCB.TileBlock[0]) + blocks, UINT(tileCB.TileBlock[1]) + blocks, 1 };
	context->CopySubresourceRegion(bloodTilePrevClock.get(), 0, 0, 0, 0, bloodTileClock.get(), 0, &clockBox);

	UploadTileCB(context, bloodTileCB.get(), tileCB);
	{
		ID3D11ShaderResourceView* srvs[3] = { bloodTileScratchRawSRV.get(), bloodTileScratchClockSRV.get(), bloodTilePrevClockSRV.get() };
		ID3D11UnorderedAccessView* uavs[1] = { bloodTileClockUAV.get() };
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetBloodTileCS(kBloodTileMergeClock), nullptr, 0);
		context->Dispatch(blocks / 8, blocks / 8, 1);
		UnbindCompute(context);
	}
	if (explained) {
		ID3D11ShaderResourceView* srvs[2] = { bloodTileSnapshotSRV.get(), bloodTileRedrawnSRV.get() };
		ID3D11UnorderedAccessView* uavs[1] = { bloodTileResidueUAV.get() };
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetBloodTileCS(kBloodTileCopyResidue), nullptr, 0);
		context->Dispatch((kBloodTileSnapDim + 7) / 8, (kBloodTileSnapDim + 7) / 8, 1);
		UnbindCompute(context);
		tileCB.Explained = 1;
		UploadTileCB(context, bloodTileCB.get(), tileCB);
	}
	{
		ID3D11ShaderResourceView* srvs[4] = { bloodTileScratchRawSRV.get(), bloodTilePrevRawSRV.get(), bloodTileClockSRV.get(), explained ? bloodTileResidueSRV.get() : nullptr };
		ID3D11UnorderedAccessView* uavs[1] = { bloodTileAtlasUAV[0].get() };
		context->CSSetShaderResources(0, 4, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(GetBloodTileCS(kBloodTileMergePigment), nullptr, 0);
		context->Dispatch(kBloodTileDim / 8, kBloodTileDim / 8, 1);
		UnbindCompute(context);
	}
	BuildBloodTileMips(context, a_slot);
	tile.lastBurial = bloodBurialClock;
	bloodTileMergesLast++;
}

void SnowDeformation::BuildBloodTileMips(ID3D11DeviceContext* a_context, uint32_t a_slot)
{
	auto* context = a_context;
	BloodTileCB cb{};
	FillBloodTileCB(cb, a_slot);

	context->CSSetShader(GetBloodTileCS(kBloodTileMipDown), nullptr, 0);
	for (uint32_t mip = 1; mip < kBloodTileMips; ++mip) {
		const UINT srcDim = kBloodTileDim >> (mip - 1);
		const UINT sx = UINT(cb.TileTexel[0]) >> (mip - 1);
		const D3D11_BOX box{ sx, 0, 0, sx + srcDim, srcDim, 1 };
		context->CopySubresourceRegion(bloodTilePrev.get(), 0, 0, 0, 0, bloodTileAtlas.get(), D3D11CalcSubresource(mip - 1, UINT(cb.TileRow), kBloodTileMips), &box);
		cb.MipTexel[0] = cb.TileTexel[0] >> mip;
		cb.MipTexel[1] = cb.TileTexel[1] >> mip;
		cb.MipDim = int32_t(kBloodTileDim >> mip);
		UploadTileCB(context, bloodTileCB.get(), cb);
		ID3D11ShaderResourceView* srvs[1] = { bloodTilePrevRawSRV.get() };
		ID3D11UnorderedAccessView* uavs[1] = { bloodTileAtlasUAV[mip].get() };
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->Dispatch((UINT(cb.MipDim) + 7) / 8, (UINT(cb.MipDim) + 7) / 8, 1);
		UnbindCompute(context);
	}
}

void SnowDeformation::RenderBloodTiles(const std::vector<BloodDisc>& a_discs)
{
	bloodTileMergesLast = 0;
	if (bloodTilesDrop) {
		bloodTilesDrop = false;
		if (bloodTilesLive)
			DropBloodTiles();
	}
	if (!BloodTilesWanted()) {
		if (bloodTilesLive)
			DropBloodTiles();
		bloodFineQueue.clear();
		return;
	}
	const uint32_t frame = globals::state->frameCount;
	if (bloodFineQueue.empty() && bloodTilesLive == 0)
		return;
	if (SnowShadersPending(3)) {
		bloodFineQueue.clear();
		return;
	}
	if (!EnsureBloodTileResources()) {
		bloodFineQueue.clear();
		return;
	}

	auto* context = globals::d3d::context;
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	if (auto* player = globals::game::player) {
		const auto position = player->GetPosition();
		eye.x = position.x;
		eye.y = position.y;
	}
	const int32_t cameraCellX = BloodCellOf(eye.x);
	const int32_t cameraCellY = BloodCellOf(eye.y);
	const int32_t keepCells = BloodTileKeepCells();

	const float burialRefills = std::max(settings.BloodBurial, 0.01f);
	for (auto& tile : bloodTiles) {
		if (!tile.live)
			continue;
		const int32_t d = std::max(std::abs(tile.cellX - cameraCellX), std::abs(tile.cellY - cameraCellY));
		if (d > keepCells || bloodBurialClock - tile.lastBurial >= burialRefills) {
			tile.live = false;
			tile.draws.clear();
			bloodTilesLive--;
			bloodTileIndexDirty = true;
			bloodTileEpoch++;
		}
	}

	uint32_t seeded[kBloodMaxTiles];
	uint32_t seededCount = 0;

	for (auto& request : bloodFineQueue) {
		auto found = bloodSeen.find(request.key);
		if (found == bloodSeen.end() || !request.draw.geometry)
			continue;
		auto& seen = found->second;
		BloodMarkBounds(request.draw, seen);
		int32_t x0 = BloodCellOf(seen.boundsMin.x), x1 = BloodCellOf(seen.boundsMax.x);
		int32_t y0 = BloodCellOf(seen.boundsMin.y), y1 = BloodCellOf(seen.boundsMax.y);
		if (x1 - x0 > 3) {
			x0 = BloodCellOf((seen.boundsMin.x + seen.boundsMax.x) * 0.5f) - 1;
			x1 = x0 + 3;
		}
		if (y1 - y0 > 3) {
			y0 = BloodCellOf((seen.boundsMin.y + seen.boundsMax.y) * 0.5f) - 1;
			y1 = y0 + 3;
		}
		bool missing = false;
		for (int32_t cy = y0; cy <= y1; ++cy) {
			for (int32_t cx = x0; cx <= x1; ++cx) {
				int32_t slot = FindBloodTile(cx, cy);
				if (slot < 0) {
					slot = AllocateBloodTile(cx, cy, cameraCellX, cameraCellY);
					if (slot >= 0 && seededCount < kBloodMaxTiles)
						seeded[seededCount++] = uint32_t(slot);
				}
				if (slot < 0) {
					missing = true;
					continue;
				}
				auto& tile = bloodTiles[slot];
				if (request.revealing || tile.gen > seen.fineEpoch)
					tile.draws.push_back(request.draw);
			}
		}
		if (!request.revealing) {
			seen.fineEpoch = bloodTileEpoch;
			seen.fineMissing = missing;
			if (missing)
				seen.fineRetryFrame = frame + 30;
		}
	}
	bloodFineQueue.clear();

	for (const auto& disc : a_discs) {
		for (int32_t cy = BloodCellOf(disc.y - disc.radius); cy <= BloodCellOf(disc.y + disc.radius); ++cy) {
			for (int32_t cx = BloodCellOf(disc.x - disc.radius); cx <= BloodCellOf(disc.x + disc.radius); ++cx) {
				const int32_t slot = FindBloodTile(cx, cy);
				if (slot >= 0)
					bloodTiles[slot].discs = true;
			}
		}
	}

	bool work = seededCount > 0;
	for (const auto& tile : bloodTiles)
		work = work || (tile.live && (!tile.draws.empty() || tile.discs));
	if (work) {
		globals::profiler->BeginPass("SnowDeformation::Decals - Capture");
		winrt::com_ptr<ID3D11ComputeShader> savedCS;
		context->CSGetShader(savedCS.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11Buffer> savedCB;
		context->CSGetConstantBuffers(0, 1, savedCB.put());
		ID3D11ShaderResourceView* savedSRVs[4]{};
		context->CSGetShaderResources(0, 4, savedSRVs);
		ID3D11UnorderedAccessView* savedUAVs[4]{};
		context->CSGetUnorderedAccessViews(0, 4, savedUAVs);
		winrt::com_ptr<ID3D11RasterizerState> savedRaster;
		context->RSGetState(savedRaster.put());
		ID3D11Buffer* tileBuffer = bloodTileCB.get();
		context->CSSetConstantBuffers(0, 1, &tileBuffer);

		ID3D11ShaderResourceView* nullShell[4] = { nullptr, nullptr, nullptr, nullptr };
		context->PSSetShaderResources(66, 4, nullShell);

		for (uint32_t i = 0; i < seededCount; ++i)
			SeedBloodTile(context, seeded[i]);

		context->OMSetBlendState(bloodBlendState.get(), nullptr, 0xFFFFFFFF);
		context->RSSetState(bloodRasterState.get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ID3D11Buffer* cb1 = bloodCB->CB();
		context->VSSetConstantBuffers(1, 1, &cb1);
		context->PSSetConstantBuffers(1, 1, &cb1);
		ID3D11SamplerState* sampler = bloodSampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		for (uint32_t i = 0; i < kBloodMaxTiles; ++i)
			if (bloodTiles[i].live && (!bloodTiles[i].draws.empty() || bloodTiles[i].discs))
				MergeBloodTile(context, i, a_discs);

		ID3D11Buffer* restoreCB = savedCB.get();
		context->CSSetConstantBuffers(0, 1, &restoreCB);
		context->CSSetShaderResources(0, 4, savedSRVs);
		context->CSSetUnorderedAccessViews(0, 4, savedUAVs, nullptr);
		for (auto* view : savedSRVs)
			if (view)
				view->Release();
		for (auto* view : savedUAVs)
			if (view)
				view->Release();
		context->CSSetShader(savedCS.get(), nullptr, 0);
		context->RSSetState(savedRaster.get());
		globals::profiler->EndPass();
		if (!bloodTilesLogged && bloodTileMergesLast) {
			bloodTilesLogged = true;
			logger::info("[SNOW DEFORMATION] blood detail tiles: first frame merged {} tile(s), {} live; mark bounds {} read from vertices, {} guessed",
				bloodTileMergesLast, bloodTilesLive, bloodTileBoundsRead, bloodTileBoundsGuessed);
		}
	}

	if (bloodTileIndexDirty) {
		uint8_t table[kBloodTileIndexDim * kBloodTileIndexDim]{};
		for (uint32_t i = 0; i < kBloodMaxTiles; ++i) {
			if (!bloodTiles[i].live)
				continue;
			const int32_t ix = bloodTiles[i].cellX & (kBloodTileIndexDim - 1);
			const int32_t iy = bloodTiles[i].cellY & (kBloodTileIndexDim - 1);
			table[iy * kBloodTileIndexDim + ix] = uint8_t(i + 1);
		}
		context->UpdateSubresource(bloodTileIndex.get(), 0, nullptr, table, kBloodTileIndexDim, 0);
		bloodTileIndexDirty = false;
	}
}
