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

// Blood detail tiles (BLOOD-DESIGN.md "Detail tiles"). The blood map's texel
// is several units; a splatter's contours are a fraction of one. Ground that
// holds blood within 100 m of the player gets a tile of an 8x8 atlas at a
// quarter unit per texel, world-anchored on a cell grid and kept while it is
// among the nearest.
// A tile is persistent like the map: decals deposit while they spread, and
// again when a tile under them is allocated later. The landscape shell reads
// a tile in place of the map wherever one lies.
//
// Beside the pigment a tile carries clock blocks: the map's two clocks, so a
// mark dries and is buried as the map's does.

ID3D11ComputeShader* SnowDeformation::GetBloodTileCS(BloodTileShader a_which)
{
	static constexpr const char* defines[kBloodTileShaderCount] = { "SEED_TILE", "MERGE_CLOCK", "MERGE_PIGMENT", "MIP_DOWN" };
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
	if (bloodCoverPS)
		bloodCoverPS->Release();
	bloodCoverPS = nullptr;
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
}

bool SnowDeformation::EnsureBloodTileResources()
{
	if (bloodTilesFailed)
		return false;
	if (bloodTileCB)
		return true;
	// fxc-reflected offsets (BloodTilesCS.hlsl; ShellCB in both shells).
	static_assert(sizeof(BloodTileCB) == 80);
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
	const uint32_t atlasDim = kBloodTileDim * BloodTilesAcross();
	const uint32_t blocks = kBloodTileDim / kBloodTileBlock;

	texture(bloodTileAtlas, atlasDim, DXGI_FORMAT_R8G8B8A8_TYPELESS, kCompute, kBloodTileMips, "SnowDeformation::BloodTileAtlas");
	srv(bloodTileAtlasSRV, bloodTileAtlas.get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, kBloodTileMips, "SnowDeformation::BloodTileAtlas SRV");
	srv(bloodTileAtlasRawSRV, bloodTileAtlas.get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, "SnowDeformation::BloodTileAtlas raw SRV");
	for (uint32_t mip = 0; mip < kBloodTileMips; ++mip)
		uav(bloodTileAtlasUAV[mip], bloodTileAtlas.get(), DXGI_FORMAT_R8G8B8A8_UNORM, mip, "SnowDeformation::BloodTileAtlas UAV");

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

	texture(bloodTileCover, kBloodTileDim, DXGI_FORMAT_R8_UNORM, kTarget, 1, "SnowDeformation::BloodTileCover");
	rtv(bloodTileCoverRTV, bloodTileCover.get(), DXGI_FORMAT_R8_UNORM, "SnowDeformation::BloodTileCover RTV");
	srv(bloodTileCoverSRV, bloodTileCover.get(), DXGI_FORMAT_R8_UNORM, 1, "SnowDeformation::BloodTileCover SRV");
	if (!bloodCoverPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl", "ps_5_0", "PSHADER", "COVER"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodCoverPS)))
			Util::SetResourceName(bloodCoverPS, "SnowDeformation::BloodCoverPS");
	}
	ok = ok && bloodCoverPS != nullptr;

	// Copies of what a pass is about to overwrite: a texture is never a
	// dispatch's input and output.
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
	for (uint32_t mip = 0; mip < kBloodTileMips; ++mip)
		context->ClearUnorderedAccessViewFloat(bloodTileAtlasUAV[mip].get(), zero);
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
	// The bound's radius says little about a decal's size (BLOOD-DESIGN.md),
	// so without vertices a mark is taken to be no smaller than most are.
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
				// A bound far from the geometry's own is a misread layout.
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
	// A tile gives way only to a request at least two cells nearer, so two
	// marks cannot trade a slot back and forth as the player walks between.
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
	tile.seeded = true;
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
	a_cb.padStep = 0;
	a_cb.WindowOrigin = windowOrigin;
	a_cb.CoarseTexel = deformWorldSize / float(deformMapDim);
	a_cb.CoarseDim = int32_t(deformMapDim);
	a_cb.MapOrigin[0] = mapOrigin.x;
	a_cb.MapOrigin[1] = mapOrigin.y;
	a_cb.BurialNow = bloodBurialClock;
	a_cb.BurialRefills = std::max(settings.BloodBurial, 0.01f);
	a_cb.MipTexel[0] = a_cb.TileTexel[0];
	a_cb.MipTexel[1] = a_cb.TileTexel[1];
	a_cb.MipDim = int32_t(kBloodTileDim);
	a_cb.padTile = 0;
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
		ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
		a_context->CSSetShaderResources(0, 4, nullSRVs);
		a_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	}
}

void SnowDeformation::SeedBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot)
{
	BloodTileCB cb{};
	FillBloodTileCB(cb, a_slot);
	UploadTileCB(a_context, bloodTileCB.get(), cb);
	ID3D11ShaderResourceView* srvs[2] = { GetBloodMapSRV(), GetBloodClockSRV() };
	ID3D11UnorderedAccessView* uavs[2] = { bloodTileAtlasUAV[0].get(), bloodTileClockUAV.get() };
	a_context->CSSetShaderResources(0, 2, srvs);
	a_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	a_context->CSSetShader(GetBloodTileCS(kBloodTileSeed), nullptr, 0);
	a_context->Dispatch(kBloodTileDim / 8, kBloodTileDim / 8, 1);
	UnbindCompute(a_context);
	// Or the shell reads the slot's last tenant from the mips.
	BuildBloodTileMips(a_context, a_slot);
}

void SnowDeformation::MergeBloodTile(ID3D11DeviceContext* a_context, uint32_t a_slot, const std::vector<BloodDisc>& a_discs)
{
	auto* context = a_context;
	auto& tile = bloodTiles[a_slot];
	BloodTileCB tileCB{};
	FillBloodTileCB(tileCB, a_slot);

	// The marks, as the blood map takes them, into the scratch.
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
			// The disc VS reads four seam instances per disc; seam 0 is the tile.
			context->DrawInstanced(6, uint32_t(a_discs.size()) * 4, 0, 0);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			context->VSSetShaderResources(1, 1, &nullSRV);
			drawn++;
		}
	}
	// Where the decals' geometry lies, for a tile that began as a copy.
	context->ClearRenderTargetView(bloodTileCoverRTV.get(), zero);
	if (tile.seeded && !tile.draws.empty()) {
		ID3D11RenderTargetView* coverRTV = bloodTileCoverRTV.get();
		context->OMSetRenderTargets(1, &coverRTV, nullptr);
		context->PSSetShader(bloodCoverPS, nullptr, 0);
		DrawBloodList(context, tile.draws, cb, nullptr, 1u, true);
	}
	ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
	context->OMSetRenderTargets(2, nullRTVs, nullptr);
	ID3D11ShaderResourceView* nullPS = nullptr;
	context->PSSetShaderResources(0, 1, &nullPS);
	tile.draws.clear();
	tile.discs = false;
	if (drawn == 0)
		return;

	// What the tile held, copied aside for the merge to read.
	const UINT x = UINT(tileCB.TileTexel[0]);
	const UINT y = UINT(tileCB.TileTexel[1]);
	const D3D11_BOX pigmentBox{ x, y, 0, x + kBloodTileDim, y + kBloodTileDim, 1 };
	context->CopySubresourceRegion(bloodTilePrev.get(), 0, 0, 0, 0, bloodTileAtlas.get(), 0, &pigmentBox);
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
	{
		ID3D11ShaderResourceView* srvs[4] = { bloodTileScratchRawSRV.get(), bloodTilePrevRawSRV.get(), bloodTileClockSRV.get(), bloodTileCoverSRV.get() };
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
		const UINT sy = UINT(cb.TileTexel[1]) >> (mip - 1);
		const D3D11_BOX box{ sx, sy, 0, sx + srcDim, sy + srcDim, 1 };
		context->CopySubresourceRegion(bloodTilePrev.get(), 0, 0, 0, 0, bloodTileAtlas.get(), mip - 1, &box);
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
	// The prime's last group compiles the tile computes; marks ask again.
	if (SnowShadersPending(3)) {
		bloodFineQueue.clear();
		return;
	}
	if (!EnsureBloodTileResources()) {
		bloodFineQueue.clear();
		return;
	}

	auto* context = globals::d3d::context;
	// Nearest to the PLAYER: an orbiting third-person camera swings hundreds
	// of units around the blood it is looking at, and tiles ranked by it
	// changed hands as it turned.
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		const auto position = player->GetPosition();
		eye.x = position.x;
		eye.y = position.y;
	}
	const int32_t cameraCellX = BloodCellOf(eye.x);
	const int32_t cameraCellY = BloodCellOf(eye.y);
	const int32_t keepCells = BloodTileKeepCells();

	// Tiles left far behind or snowed under whole are free.
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
			// The marks that lay in it ask again when they are next drawn.
			bloodTileEpoch++;
		}
	}

	// Requests to tiles. A mark asks for every cell its bounds touch; a tile
	// takes it while it spreads, or when the tile is newer than the mark's
	// last whole deposit.
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
		// One mark takes at most four cells a side - all sixteen tiles, at
		// the finest level, for a pool that size. A wider one keeps its middle.
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
			// A mark with no slot to be had asks again in half a second.
			seen.fineMissing = missing;
			if (missing)
				seen.fineRetryFrame = frame + 30;
		}
	}
	bloodFineQueue.clear();

	// API discs go to the tiles they touch; they allocate nothing.
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
		globals::profiler->BeginPass("SnowDeformation::BloodTiles");
		winrt::com_ptr<ID3D11ComputeShader> savedCS;
		context->CSGetShader(savedCS.put(), nullptr, nullptr);
		winrt::com_ptr<ID3D11Buffer> savedCB;
		context->CSGetConstantBuffers(0, 1, savedCB.put());
		ID3D11ShaderResourceView* savedSRVs[4]{};
		context->CSGetShaderResources(0, 4, savedSRVs);
		ID3D11UnorderedAccessView* savedUAVs[2]{};
		context->CSGetUnorderedAccessViews(0, 2, savedUAVs);
		winrt::com_ptr<ID3D11RasterizerState> savedRaster;
		context->RSGetState(savedRaster.put());
		ID3D11Buffer* tileBuffer = bloodTileCB.get();
		context->CSSetConstantBuffers(0, 1, &tileBuffer);

		// The shell's binds from last frame would be unbound under the writes
		// anyway; done here so the debug layer stays quiet.
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
		context->CSSetUnorderedAccessViews(0, 2, savedUAVs, nullptr);
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
		// Cell -> slot + 1 on a torus wider than twice the keep distance, so no
		// two live tiles share an entry.
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
