// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cmath>

// Blood decals painted directly (BLOOD-DESIGN.md "Direct decals"): the rune
// glyph's mechanism for blood, the A/B partner of the detail tiles. A tile
// holds no colour - it holds each decal's UV FIELD and which of a few bound
// textures it belongs to, and the landscape shell samples the game's own
// blood texture and normal map through it. Nothing is stored, dated, dried or
// buried: a mark shows while the game draws its decal and leaves with it.
//
// Tiles share the detail tiles' cell grid, index and slots (at the standard
// level). Where decals overlap, the one with more blood at that texel wins
// (depth = 1 - alpha), so the seam falls where the two are equal.

namespace
{
	uint64_t Mix(uint64_t a_hash, uint64_t a_value)
	{
		a_value *= 0x9E3779B97F4A7C15ull;
		a_value ^= a_value >> 32;
		return a_hash + a_value;
	}
}

void SnowDeformation::ReleaseBloodDecalResources()
{
	bloodDecalAtlas = nullptr;
	bloodDecalAtlasSRV = nullptr;
	bloodDecalScratch = nullptr;
	bloodDecalScratchRTV = nullptr;
	bloodDecalDepth = nullptr;
	bloodDecalDepthDSV = nullptr;
	for (auto& slot : bloodDecalSlots)
		slot = {};
	bloodDecalLive.clear();
}

bool SnowDeformation::EnsureBloodDecalResources()
{
	if (bloodDecalsFailed)
		return false;
	if (bloodDecalAtlasSRV && bloodDecalPS && bloodDecalDepthState && bloodDecalBlendState)
		return true;
	auto* device = globals::d3d::device;
	bool ok = true;
	const uint32_t atlasDim = kBloodTileDim * 4;
	if (!bloodDecalAtlasSRV) {
		// uv at 16 bits, as the rune atlas: rg = uv, b = texture slot + 1, a = the decal's alpha this frame.
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = atlasDim;
		desc.Height = atlasDim;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		winrt::com_ptr<ID3D11RenderTargetView> atlasRTV;
		ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, bloodDecalAtlas.put()));
		ok = ok && SUCCEEDED(device->CreateShaderResourceView(bloodDecalAtlas.get(), nullptr, bloodDecalAtlasSRV.put()));
		ok = ok && SUCCEEDED(device->CreateRenderTargetView(bloodDecalAtlas.get(), nullptr, atlasRTV.put()));
		desc.Width = kBloodTileDim;
		desc.Height = kBloodTileDim;
		ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, bloodDecalScratch.put()));
		ok = ok && SUCCEEDED(device->CreateRenderTargetView(bloodDecalScratch.get(), nullptr, bloodDecalScratchRTV.put()));
		desc.Format = DXGI_FORMAT_D16_UNORM;
		desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, bloodDecalDepth.put()));
		ok = ok && SUCCEEDED(device->CreateDepthStencilView(bloodDecalDepth.get(), nullptr, bloodDecalDepthDSV.put()));
		if (ok) {
			Util::SetResourceName(bloodDecalAtlas.get(), "SnowDeformation::BloodDecalAtlas");
			Util::SetResourceName(bloodDecalAtlasSRV.get(), "SnowDeformation::BloodDecalAtlas SRV");
			Util::SetResourceName(bloodDecalScratch.get(), "SnowDeformation::BloodDecalScratch");
			Util::SetResourceName(bloodDecalScratchRTV.get(), "SnowDeformation::BloodDecalScratch RTV");
			Util::SetResourceName(bloodDecalDepth.get(), "SnowDeformation::BloodDecalDepth");
			Util::SetResourceName(bloodDecalDepthDSV.get(), "SnowDeformation::BloodDecalDepth DSV");
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			globals::d3d::context->ClearRenderTargetView(atlasRTV.get(), zero);
		}
	}
	if (ok && !bloodDecalPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl", "ps_5_0", "PSHADER", "DECAL"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodDecalPS)))
			Util::SetResourceName(bloodDecalPS, "SnowDeformation::BloodDecalPS");
		ok = bloodDecalPS != nullptr;
	}
	if (ok && !bloodDecalDepthState) {
		D3D11_DEPTH_STENCIL_DESC desc{};
		desc.DepthEnable = TRUE;
		desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		desc.DepthFunc = D3D11_COMPARISON_LESS;
		ok = SUCCEEDED(device->CreateDepthStencilState(&desc, bloodDecalDepthState.put()));
	}
	if (ok && !bloodDecalBlendState) {
		D3D11_BLEND_DESC desc{};
		desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		ok = SUCCEEDED(device->CreateBlendState(&desc, bloodDecalBlendState.put()));
	}
	if (!ok) {
		logger::error("[SNOW DEFORMATION] direct blood decals are off (a resource or shader failed)");
		bloodDecalsFailed = true;
		return false;
	}
	logger::info("[SNOW DEFORMATION] direct blood decals ready: {} tiles of {}x{} uv field, {} texture slots", 16, kBloodTileDim, kBloodTileDim, kBloodDecalTextures);
	return true;
}

void SnowDeformation::RenderBloodDecalTiles()
{
	bloodFineQueue.clear();
	const uint32_t frame = globals::state->frameCount;
	// A decal the camera turned away from is not drawn; it keeps its place
	// for two seconds so a glance aside does not rebuild its tile twice.
	std::erase_if(bloodDecalLive, [&](const auto& a_kv) { return frame - a_kv.second.lastFrame > 120; });
	if (bloodDecalLive.empty() && bloodTilesLive == 0)
		return;
	if (SnowShadersPending(3) || !EnsureBloodTileResources() || !EnsureBloodDecalResources())
		return;

	auto* context = globals::d3d::context;
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		const auto position = player->GetPosition();
		eye.x = position.x;
		eye.y = position.y;
	}
	const int32_t playerCellX = BloodCellOf(eye.x);
	const int32_t playerCellY = BloodCellOf(eye.y);

	// Texture slots: one per distinct blood texture in use, the least
	// recently used giving way.
	for (auto& [key, live] : bloodDecalLive) {
		live.slot = -1;
		auto* diffuse = live.draw.diffuse.get();
		int32_t free = -1;
		for (uint32_t i = 0; i < kBloodDecalTextures; ++i) {
			auto& slot = bloodDecalSlots[i];
			if (slot.diffuse.get() == diffuse) {
				live.slot = int32_t(i);
				break;
			}
			if (slot.lastFrame != frame && (free < 0 || slot.lastFrame < bloodDecalSlots[free].lastFrame))
				free = int32_t(i);
		}
		if (live.slot < 0 && free >= 0) {
			bloodDecalSlots[free].diffuse = live.draw.diffuse;
			bloodDecalSlots[free].normal = live.normal;
			live.slot = free;
		}
		if (live.slot >= 0)
			bloodDecalSlots[live.slot].lastFrame = frame;
	}

	// Decals to tiles, by the cells their vertices touch.
	for (auto& tile : bloodTiles) {
		tile.draws.clear();
		tile.decalHashNow = 0;
	}
	for (auto& [key, live] : bloodDecalLive) {
		if (live.slot < 0 || !live.draw.geometry)
			continue;
		auto found = bloodSeen.find(key);
		if (found == bloodSeen.end())
			continue;
		auto& seen = found->second;
		BloodMarkBounds(live.draw, seen);
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
		live.draw.slotCode = float(live.slot + 1) / 255.0f;
		live.draw.reveal = 1.0f;
		uint64_t mark = Mix(0, reinterpret_cast<uintptr_t>(key));
		mark = Mix(mark, uint64_t(live.slot + 1));
		mark = Mix(mark, uint64_t(std::clamp(live.draw.alpha, 0.0f, 1.0f) * 64.0f));
		mark = Mix(mark, reinterpret_cast<uintptr_t>(seen.vb));
		// Pool quads grow by their bones: redrawn every eighth frame.
		if (live.draw.skinned)
			mark = Mix(mark, frame >> 3);
		for (int32_t cy = y0; cy <= y1; ++cy) {
			for (int32_t cx = x0; cx <= x1; ++cx) {
				int32_t slot = FindBloodTile(cx, cy);
				if (slot < 0) {
					slot = AllocateBloodTile(cx, cy, playerCellX, playerCellY);
					if (slot >= 0) {
						bloodTiles[slot].decalHash = 0;
						bloodTiles[slot].decalFrame = frame;
					}
				}
				if (slot < 0)
					continue;
				bloodTiles[slot].draws.push_back(live.draw);
				bloodTiles[slot].decalHashNow += mark;
			}
		}
	}

	// A tile is redrawn only when what lies in it changed.
	bool bound = false;
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	winrt::com_ptr<ID3D11DepthStencilState> savedDepth;
	UINT savedStencilRef = 0;
	bloodTileMergesLast = 0;
	for (uint32_t i = 0; i < 16; ++i) {
		auto& tile = bloodTiles[i];
		if (!tile.live)
			continue;
		if (tile.draws.empty()) {
			// Emptied: the slot is free for ground that has blood.
			if (tile.decalHash != 0 || frame - tile.decalFrame > 120) {
				tile.live = false;
				bloodTilesLive--;
				bloodTileIndexDirty = true;
			}
			continue;
		}
		tile.decalFrame = frame;
		if (tile.decalHashNow == tile.decalHash)
			continue;
		if (!bound) {
			bound = true;
			globals::profiler->BeginPass("SnowDeformation::BloodDecals");
			context->RSGetState(savedRaster.put());
			context->OMGetDepthStencilState(savedDepth.put(), &savedStencilRef);
			ID3D11ShaderResourceView* nullShell[4] = { nullptr, nullptr, nullptr, nullptr };
			context->PSSetShaderResources(66, 4, nullShell);
			context->OMSetBlendState(bloodDecalBlendState.get(), nullptr, 0xFFFFFFFF);
			context->OMSetDepthStencilState(bloodDecalDepthState.get(), 0);
			context->RSSetState(bloodRasterState.get());
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(kBloodTileDim), float(kBloodTileDim), 0.0f, 1.0f };
			context->RSSetViewports(1, &viewport);
			ID3D11Buffer* cb1 = bloodCB->CB();
			context->VSSetConstantBuffers(1, 1, &cb1);
			context->PSSetConstantBuffers(1, 1, &cb1);
			ID3D11SamplerState* sampler = bloodSampler.get();
			context->PSSetSamplers(0, 1, &sampler);
		}
		const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(bloodDecalScratchRTV.get(), zero);
		context->ClearDepthStencilView(bloodDecalDepthDSV.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
		ID3D11RenderTargetView* rtv = bloodDecalScratchRTV.get();
		context->OMSetRenderTargets(1, &rtv, bloodDecalDepthDSV.get());
		BloodCB cb{};
		cb.WindowOrigin = { float(tile.cellX) * BloodTileCell() - kBloodTileApron, float(tile.cellY) * BloodTileCell() - kBloodTileApron };
		cb.TexelSize = BloodTileTexel();
		cb.MapDim = float(kBloodTileDim);
		cb.MapOrigin = { 0, 0 };
		cb.Intensity = 1.0f;
		cb.NormalZMin = 0.3f;
		context->PSSetShader(bloodDecalPS, nullptr, 0);
		DrawBloodList(context, tile.draws, cb, nullptr, 1u);
		ID3D11RenderTargetView* nullRTV = nullptr;
		context->OMSetRenderTargets(1, &nullRTV, nullptr);
		context->CopySubresourceRegion(bloodDecalAtlas.get(), 0, (i % 4) * kBloodTileDim, (i / 4) * kBloodTileDim, 0, bloodDecalScratch.get(), 0, nullptr);
		tile.decalHash = tile.decalHashNow;
		bloodTileMergesLast++;
	}
	for (auto& tile : bloodTiles)
		tile.draws.clear();
	if (bound) {
		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->PSSetShaderResources(0, 1, &nullSRV);
		context->RSSetState(savedRaster.get());
		context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
		globals::profiler->EndPass();
		if (!bloodDecalsLogged) {
			bloodDecalsLogged = true;
			logger::info("[SNOW DEFORMATION] direct blood decals: first frame drew {} tile(s) from {} live decal(s)", bloodTileMergesLast, bloodDecalLive.size());
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
