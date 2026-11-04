// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/CoSave.h"
#include "Globals.h"

namespace
{
	uint64_t PrintTileId(int32_t a_x, int32_t a_y)
	{
		return ((uint64_t)(uint32_t)a_x << 32) | (uint32_t)a_y;
	}

	int32_t PrintTileX(uint64_t a_id)
	{
		return (int32_t)(uint32_t)(a_id >> 32);
	}

	int32_t PrintTileY(uint64_t a_id)
	{
		return (int32_t)(uint32_t)(a_id & 0xFFFFFFFFu);
	}

	int32_t FloorDiv(int32_t a_v, int32_t a_d)
	{
		return a_v >= 0 ? a_v / a_d : -((-a_v + a_d - 1) / a_d);
	}

	bool Overlap(const DirectX::XMINT4& a_a, const DirectX::XMINT4& a_b, DirectX::XMINT4& o_r)
	{
		const int32_t x0 = std::max(a_a.x, a_b.x);
		const int32_t y0 = std::max(a_a.y, a_b.y);
		const int32_t x1 = std::min(a_a.x + a_a.z, a_b.x + a_b.z);
		const int32_t y1 = std::min(a_a.y + a_a.w, a_b.y + a_b.w);
		o_r = { x0, y0, x1 - x0, y1 - y0 };
		return x1 > x0 && y1 > y0;
	}

	DirectX::XMINT4 TileRect(int32_t a_tx, int32_t a_ty, int32_t a_tileDim)
	{
		return { a_tx * a_tileDim, a_ty * a_tileDim, a_tileDim, a_tileDim };
	}

	bool AnyPrint(const std::vector<uint32_t>& a_texels)
	{
		return std::any_of(a_texels.begin(), a_texels.end(), [](uint32_t v) { return (v & 1023u) != 0u; });
	}

	uint32_t SnapPrintDim(uint32_t a_dim)
	{
		return a_dim >= 2048u ? 2048u : (a_dim >= 1024u ? 1024u : 512u);
	}
}

ID3D11ComputeShader* SnowDeformation::GetPrintStampCS()
{
	if (!printStampCS)
		printStampCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintStampCS")));
	return printStampCS.get();
}

ID3D11ComputeShader* SnowDeformation::GetPrintRingCS()
{
	if (!printRingCS)
		printRingCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintRingCS")));
	return printRingCS.get();
}

ID3D11ComputeShader* SnowDeformation::GetPrintRefillCS()
{
	if (!printRefillCS)
		printRefillCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintRefillCS")));
	return printRefillCS.get();
}

ID3D11ComputeShader* SnowDeformation::GetPrintRimCS()
{
	if (!printRimCS && !printRimFailed) {
		printRimCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "PrintRimCS")));
		if (!printRimCS) {
			printRimFailed = true;
			logger::warn("[SNOW DEFORMATION] print rim kernel failed to compile; object prints go on without their rim");
		}
	}
	return printRimCS.get();
}

bool SnowDeformation::EnsurePrintShaders()
{
	if (printCSFailed)
		return false;
	if (!GetPrintStampCS() || !GetPrintRingCS() || !GetPrintRefillCS()) {
		printCSFailed = true;
		logger::error("[SNOW DEFORMATION] print map kernels failed to compile; Snow Trenches on Objects disabled");
		return false;
	}
	return true;
}

void SnowDeformation::ReleaseObjectPrintShaders()
{
	for (auto* cs : { &printStampCS, &printRingCS, &printRefillCS, &printRimCS, &printRayCS })
		*cs = nullptr;
	printCSFailed = false;
	printRimFailed = false;
	printRayFailed = false;
}

void SnowDeformation::CreatePrintResources(uint32_t a_dim)
{
	auto device = globals::d3d::device;

	delete printMap;
	printMap = nullptr;
	D3D11_TEXTURE2D_DESC mapDesc{};
	mapDesc.Width = a_dim;
	mapDesc.Height = a_dim;
	mapDesc.MipLevels = 1;
	mapDesc.ArraySize = 2;
	mapDesc.Format = DXGI_FORMAT_R32_UINT;
	mapDesc.SampleDesc.Count = 1;
	mapDesc.Usage = D3D11_USAGE_DEFAULT;
	mapDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	printMap = new Texture2D(mapDesc, "SnowDeformation::PrintMap");
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = mapDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray = { 0, 1, 0, 2 };
	printMap->CreateSRV(srvDesc);
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = mapDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	uavDesc.Texture2DArray = { 0, 0, 2 };
	printMap->CreateUAV(uavDesc);
	printMapDim = a_dim;

	delete printRim;
	printRim = nullptr;
	D3D11_TEXTURE2D_DESC rimDesc = mapDesc;
	rimDesc.ArraySize = 1;
	printRim = new Texture2D(rimDesc, "SnowDeformation::PrintRim");
	D3D11_SHADER_RESOURCE_VIEW_DESC rimSrvDesc{};
	rimSrvDesc.Format = rimDesc.Format;
	rimSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	rimSrvDesc.Texture2D.MipLevels = 1;
	printRim->CreateSRV(rimSrvDesc);
	D3D11_UNORDERED_ACCESS_VIEW_DESC rimUavDesc{};
	rimUavDesc.Format = rimDesc.Format;
	rimUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	printRim->CreateUAV(rimUavDesc);
	printRimWidthLast = -1.0f;

	delete printTiles;
	printTiles = nullptr;
	D3D11_TEXTURE2D_DESC tilesDesc{};
	tilesDesc.Width = a_dim / kPrintTileDim;
	tilesDesc.Height = a_dim / kPrintTileDim;
	tilesDesc.MipLevels = 1;
	tilesDesc.ArraySize = 1;
	tilesDesc.Format = DXGI_FORMAT_R8_UINT;
	tilesDesc.SampleDesc.Count = 1;
	tilesDesc.Usage = D3D11_USAGE_DEFAULT;
	tilesDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	printTiles = new Texture2D(tilesDesc, "SnowDeformation::PrintTiles");
	D3D11_SHADER_RESOURCE_VIEW_DESC tilesSrvDesc{};
	tilesSrvDesc.Format = tilesDesc.Format;
	tilesSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	tilesSrvDesc.Texture2D.MipLevels = 1;
	printTiles->CreateSRV(tilesSrvDesc);
	printTilesUploaded.clear();

	if (!printFrameCB)
		printFrameCB = new ConstantBuffer(ConstantBufferDesc<PrintFrameCB>(), "SnowDeformation::PrintFrameCB");

	if (!printBlockBuffer) {
		D3D11_BUFFER_DESC blockDesc = StructuredBufferDesc<uint32_t>(uint64_t(kPrintBlockCap), false, true);
		DX::ThrowIfFailed(device->CreateBuffer(&blockDesc, nullptr, printBlockBuffer.put()));
		Util::SetResourceName(printBlockBuffer.get(), "SnowDeformation::PrintBlocks");
		D3D11_SHADER_RESOURCE_VIEW_DESC blockSrvDesc{};
		blockSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
		blockSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		blockSrvDesc.Buffer.NumElements = kPrintBlockCap;
		DX::ThrowIfFailed(device->CreateShaderResourceView(printBlockBuffer.get(), &blockSrvDesc, printBlockSRV.put()));
		Util::SetResourceName(printBlockSRV.get(), "SnowDeformation::PrintBlocks SRV");
	}

	if (!printWriteBuffer) {
		constexpr uint32_t maxRow = 2048u / kPrintTileDim + 1u;
		D3D11_BUFFER_DESC writeDesc{};
		writeDesc.ByteWidth = maxRow * maxRow * sizeof(uint32_t);
		writeDesc.Usage = D3D11_USAGE_DEFAULT;
		writeDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		writeDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		DX::ThrowIfFailed(device->CreateBuffer(&writeDesc, nullptr, printWriteBuffer.put()));
		Util::SetResourceName(printWriteBuffer.get(), "SnowDeformation::PrintWrites");
		D3D11_UNORDERED_ACCESS_VIEW_DESC writeUavDesc{};
		writeUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		writeUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		writeUavDesc.Buffer.NumElements = maxRow * maxRow;
		writeUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(printWriteBuffer.get(), &writeUavDesc, printWriteUAV.put()));
		Util::SetResourceName(printWriteUAV.get(), "SnowDeformation::PrintWrites UAV");

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = writeDesc.ByteWidth;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& slot : printWriteReadbacks) {
			DX::ThrowIfFailed(device->CreateBuffer(&stagingDesc, nullptr, slot.staging.put()));
			Util::SetResourceName(slot.staging.get(), "SnowDeformation::PrintWritesStaging");
		}
	}

	if (!printStaging[0].texture) {
		D3D11_TEXTURE2D_DESC stagingDesc{};
		stagingDesc.Width = kPrintTileDim;
		stagingDesc.Height = kPrintTileDim;
		stagingDesc.MipLevels = 1;
		stagingDesc.ArraySize = 2;
		stagingDesc.Format = DXGI_FORMAT_R32_UINT;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& slot : printStaging) {
			DX::ThrowIfFailed(device->CreateTexture2D(&stagingDesc, nullptr, slot.texture.put()));
			Util::SetResourceName(slot.texture.get(), "SnowDeformation::PrintMirrorStaging");
		}
	}

	printBlockSeen.assign((size_t)(a_dim / 8) * (a_dim / 8), 0);
	printWindowValid = false;
	printLive.clear();
}

bool SnowDeformation::DecayPrintTile(PrintStoreTile& a_tile, float a_clock)
{
	const int drop = (int)std::floor((a_clock - a_tile.clock) * 1023.0f);
	if (drop <= 0)
		return AnyPrint(a_tile.texels);
	a_tile.clock += (float)drop / 1023.0f;
	bool any = false;
	for (auto& v : a_tile.texels) {
		const uint32_t depth = v & 1023u;
		if (depth <= (uint32_t)drop) {
			v = 0u;
		} else {
			v -= (uint32_t)drop;
			any = true;
		}
	}
	return any;
}

void SnowDeformation::EnforcePrintStoreCap()
{
	if (printStore.size() <= kPrintStoreCap)
		return;
	const float centreX = ((float)printWindowMin.x + 0.5f * (float)printMapDim) * kPrintTexel;
	const float centreY = ((float)printWindowMin.y + 0.5f * (float)printMapDim) * kPrintTexel;
	std::vector<std::pair<float, TrenchTileKey>> ranked;
	ranked.reserve(printStore.size());
	for (const auto& [key, tile] : printStore) {
		float rank = std::numeric_limits<float>::max();
		if (key.worldspace == printWorldspace) {
			const float dx = ((float)key.x + 0.5f) * kPrintTileDim * kPrintTexel - centreX;
			const float dy = ((float)key.y + 0.5f) * kPrintTileDim * kPrintTexel - centreY;
			rank = dx * dx + dy * dy;
		}
		ranked.emplace_back(rank, key);
	}
	std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	size_t evicted = 0;
	for (const auto& [rank, key] : ranked) {
		if (printStore.size() <= kPrintStoreCap)
			break;
		printStore.erase(key);
		evicted++;
	}
	logger::info("[SNOW DEFORMATION] print store over its cap: evicted {} tiles, {} remain", evicted, printStore.size());
}

bool SnowDeformation::QueuePrintMirror(ID3D11DeviceContext* a_context, int32_t a_tx, int32_t a_ty, float a_clock)
{
	auto slot = std::find_if(printStaging.begin(), printStaging.end(), [](const PrintStagingSlot& s) { return !s.busy; });
	if (slot == printStaging.end())
		return false;
	slot->key = { printWorldspace, a_tx, a_ty };
	slot->windowMin = printWindowMin;
	slot->dim = printMapDim;
	slot->clock = a_clock;
	slot->frame = printFrame;
	slot->generation = printQueueGeneration;
	slot->busy = true;
	const int32_t mask = (int32_t)printMapDim - 1;
	const UINT px = (UINT)((a_tx * kPrintTileDim) & mask);
	const UINT py = (UINT)((a_ty * kPrintTileDim) & mask);
	const D3D11_BOX box{ px, py, 0, px + kPrintTileDim, py + kPrintTileDim, 1 };
	for (UINT s = 0; s < 2; s++)
		a_context->CopySubresourceRegion(slot->texture.get(), D3D11CalcSubresource(0, s, 1), 0, 0, 0,
			printMap->resource.get(), D3D11CalcSubresource(0, s, 1), &box);
	return true;
}

void SnowDeformation::PollPrintMirrors(ID3D11DeviceContext* a_context)
{
	std::vector<uint32_t> texels(2 * (size_t)kPrintTileTexels);
	for (auto& slot : printStaging) {
		if (!slot.busy || printFrame < slot.frame + 2)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped[2]{};
		if (FAILED(a_context->Map(slot.texture.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[0])))
			continue;
		if (FAILED(a_context->Map(slot.texture.get(), 1, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped[1]))) {
			a_context->Unmap(slot.texture.get(), 0);
			continue;
		}
		for (int s = 0; s < 2; s++)
			for (int j = 0; j < kPrintTileDim; j++)
				std::memcpy(texels.data() + (size_t)s * kPrintTileTexels + (size_t)j * kPrintTileDim,
					static_cast<const uint8_t*>(mapped[s].pData) + (size_t)j * mapped[s].RowPitch, kPrintTileDim * sizeof(uint32_t));
		a_context->Unmap(slot.texture.get(), 1);
		a_context->Unmap(slot.texture.get(), 0);
		slot.busy = false;

		const DirectX::XMINT4 window = { slot.windowMin.x, slot.windowMin.y, (int32_t)slot.dim, (int32_t)slot.dim };
		DirectX::XMINT4 valid{};
		if (!Overlap(TileRect(slot.key.x, slot.key.y, kPrintTileDim), window, valid))
			continue;
		const int32_t lx0 = valid.x - slot.key.x * kPrintTileDim;
		const int32_t ly0 = valid.y - slot.key.y * kPrintTileDim;

		std::scoped_lock lock(printStoreMutex);
		if (slot.generation != printStoreGeneration.load(std::memory_order_relaxed))
			continue;
		auto it = printStore.find(slot.key);
		if (it == printStore.end()) {
			bool any = false;
			for (int s = 0; s < 2 && !any; s++)
				for (int j = ly0; j < ly0 + valid.w && !any; j++)
					for (int i = lx0; i < lx0 + valid.z && !any; i++)
						any = (texels[(size_t)s * kPrintTileTexels + (size_t)j * kPrintTileDim + i] & 1023u) != 0u;
			if (!any)
				continue;
			it = printStore.emplace(slot.key, PrintStoreTile{ std::vector<uint32_t>(2 * (size_t)kPrintTileTexels, 0u), slot.clock }).first;
		} else {
			DecayPrintTile(it->second, slot.clock);
		}
		for (int s = 0; s < 2; s++)
			for (int j = ly0; j < ly0 + valid.w; j++)
				for (int i = lx0; i < lx0 + valid.z; i++) {
					const size_t index = (size_t)s * kPrintTileTexels + (size_t)j * kPrintTileDim + i;
					it->second.texels[index] = texels[index];
				}
		it->second.clock = slot.clock;
		if (!AnyPrint(it->second.texels))
			printStore.erase(it);
		else
			EnforcePrintStoreCap();
	}
}

void SnowDeformation::PollPrintWrites(ID3D11DeviceContext* a_context)
{
	for (auto& slot : printWriteReadbacks) {
		if (!slot.busy || printFrame < slot.frame + 1)
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			continue;
		slot.busy = false;
		if (slot.frame >= printLastClearFrame && slot.worldspace == printWorldspace) {
			const auto* flags = static_cast<const uint32_t*>(mapped.pData);
			const DirectX::XMINT4 window = { printWindowMin.x, printWindowMin.y, (int32_t)printMapDim, (int32_t)printMapDim };
			for (uint32_t row = 0; row < slot.rowTiles; row++)
				for (uint32_t col = 0; col < slot.rowTiles; col++) {
					if (flags[row * slot.rowTiles + col] == 0u)
						continue;
					const int32_t tx = slot.tileOrigin.x + (int32_t)col;
					const int32_t ty = slot.tileOrigin.y + (int32_t)row;
					DirectX::XMINT4 inside{};
					if (!Overlap(TileRect(tx, ty, kPrintTileDim), window, inside))
						continue;
					auto& live = printLive[PrintTileId(tx, ty)];
					live.refillSince = 0.0f;
					live.dirty = true;
				}
		}
		a_context->Unmap(slot.staging.get(), 0);
	}
}

uint32_t SnowDeformation::InjectPrintRect(ID3D11DeviceContext* a_context, const DirectX::XMINT4& a_rect, float a_clock)
{
	if (a_rect.z <= 0 || a_rect.w <= 0)
		return 0;
	const int32_t mask = (int32_t)printMapDim - 1;
	const int32_t tx0 = FloorDiv(a_rect.x, kPrintTileDim);
	const int32_t ty0 = FloorDiv(a_rect.y, kPrintTileDim);
	const int32_t tx1 = FloorDiv(a_rect.x + a_rect.z - 1, kPrintTileDim);
	const int32_t ty1 = FloorDiv(a_rect.y + a_rect.w - 1, kPrintTileDim);
	uint32_t injected = 0;
	std::scoped_lock lock(printStoreMutex);
	if (printStore.empty())
		return 0;
	for (int32_t ty = ty0; ty <= ty1; ty++) {
		for (int32_t tx = tx0; tx <= tx1; tx++) {
			auto it = printStore.find({ printWorldspace, tx, ty });
			if (it == printStore.end())
				continue;
			if (!DecayPrintTile(it->second, a_clock)) {
				printStore.erase(it);
				continue;
			}
			DirectX::XMINT4 box{};
			if (!Overlap(TileRect(tx, ty, kPrintTileDim), a_rect, box))
				continue;
			const int32_t lx = box.x - tx * kPrintTileDim;
			const int32_t ly = box.y - ty * kPrintTileDim;
			const UINT px = (UINT)(box.x & mask);
			const UINT py = (UINT)(box.y & mask);
			const D3D11_BOX dst{ px, py, 0, px + (UINT)box.z, py + (UINT)box.w, 1 };
			for (UINT s = 0; s < 2; s++) {
				const uint32_t* src = it->second.texels.data() + (size_t)s * kPrintTileTexels + (size_t)ly * kPrintTileDim + lx;
				a_context->UpdateSubresource(printMap->resource.get(), D3D11CalcSubresource(0, s, 1), &dst, src, kPrintTileDim * sizeof(uint32_t), 0);
			}
			printLive[PrintTileId(tx, ty)].refillSince = 0.0f;
			injected++;
		}
	}
	return injected;
}

void SnowDeformation::UpdateObjectPrints(ID3D11DeviceContext* a_context, const PerFrame& a_frame, bool a_trenchPassRan)
{
	if (!EnsurePrintShaders())
		return;
	if (!printDimTarget)
		PrintRectForFrame();
	const uint32_t wantDim = printDimTarget;
	settings.ObjectTrenchResolution = wantDim;
	if (wantDim != printMapDim)
		CreatePrintResources(wantDim);
	if (!printMap)
		return;
	ID3D11ShaderResourceView* nullReaders[3] = {};
	a_context->PSSetShaderResources(kPrintReaderSlot, 3, nullReaders);
	printFrame++;
	printStampBlocksLast = printInjectsLast = 0;

	float clock;
	{
		std::scoped_lock lock(trenchStoreMutex);
		clock = trenchDecayClock;
	}
	const bool persist = settings.PersistTrenches;
	{
		std::scoped_lock lock(printStoreMutex);
		printClockSnapshot = clock;
		if (!persist && !printStore.empty()) {
			logger::info("[SNOW DEFORMATION] print store CLEARED by the Remember Trenches toggle, {} tiles dropped", printStore.size());
			printStore.clear();
		}
	}

	const bool reinject = printReinjectRequested.exchange(false, std::memory_order_acq_rel);
	printQueueGeneration = printStoreGeneration.load(std::memory_order_acquire);
	if (reinject) {
		for (auto& slot : printStaging)
			slot.busy = false;
		for (auto& slot : printWriteReadbacks)
			slot.busy = false;
	}

	ID3D11UnorderedAccessView* nullUAV[4] = {};
	a_context->CSSetUnorderedAccessViews(0, 4, nullUAV, nullptr);
	PollPrintWrites(a_context);
	PollPrintMirrors(a_context);

	const int32_t dim = (int32_t)printMapDim;
	const DirectX::XMINT2 newMin = printWindowTarget;
	const DirectX::XMINT4 newWindow = { newMin.x, newMin.y, dim, dim };
	const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
	const int32_t dx = newMin.x - printWindowMin.x;
	const int32_t dy = newMin.y - printWindowMin.y;
	const bool fullClear = reinject || !printWindowValid || worldspace != printWorldspace || std::abs(dx) + std::abs(dy) >= dim / 2;
	const bool moved = dx != 0 || dy != 0;

	if (printWindowValid && (fullClear || moved)) {
		for (auto it = printLive.begin(); it != printLive.end();) {
			const int32_t tx = PrintTileX(it->first);
			const int32_t ty = PrintTileY(it->first);
			const auto rect = TileRect(tx, ty, kPrintTileDim);
			DirectX::XMINT4 inside{};
			const bool overlaps = !fullClear && Overlap(rect, newWindow, inside);
			const bool keepsAll = overlaps && inside.z == kPrintTileDim && inside.w == kPrintTileDim;
			if (!keepsAll && it->second.dirty && persist && !reinject && QueuePrintMirror(a_context, tx, ty, clock)) {
				it->second.dirty = false;
			}
			if (overlaps)
				++it;
			else
				it = printLive.erase(it);
		}
	}

	PrintFrameCB cb{};
	cb.WindowMin = newMin;
	uint32_t ringTotal = 0;
	if (fullClear) {
		const UINT zero[4] = {};
		a_context->ClearUnorderedAccessViewUint(printMap->uav.get(), zero);
		if (printRim)
			a_context->ClearUnorderedAccessViewUint(printRim->uav.get(), zero);
		printLive.clear();
		printLastClearFrame = printFrame;
	} else if (moved) {
		uint32_t n = 0;
		if (dx != 0)
			cb.RingRects[n++] = { dx > 0 ? newMin.x + dim - dx : newMin.x, newMin.y, std::abs(dx), dim };
		if (dy != 0)
			cb.RingRects[n++] = { dx > 0 ? newMin.x : newMin.x - dx, dy > 0 ? newMin.y + dim - dy : newMin.y, dim - std::abs(dx), std::abs(dy) };
		cb.RingRectCount = n;
		for (uint32_t i = 0; i < n; i++)
			ringTotal += (uint32_t)(cb.RingRects[i].z * cb.RingRects[i].w);
		cb.RingTotal = ringTotal;
	}

	uint32_t quanta = 0;
	if (a_trenchPassRan && a_frame.RefillAmount > 0.0f) {
		printRefillBank = std::min(printRefillBank + a_frame.RefillAmount, 1.0f);
		quanta = (uint32_t)std::floor(printRefillBank * 1023.0f);
		printRefillBank -= (float)quanta / 1023.0f;
	}
	cb.RefillQuanta = quanta;
	cb.RimWidth = std::clamp(settings.ObjectPrintRimWidth, 0.0f, 15.0f);
	const uint32_t spellCount = 0;
	cb.SpellCount = spellCount;
	printFrameCB->Update(cb);

	ID3D11Buffer* cbs[2] = { perFrame->CB(), printFrameCB->CB() };
	a_context->CSSetConstantBuffers(0, 2, cbs);
	ID3D11UnorderedAccessView* mapUAV = printMap->uav.get();

	if (ringTotal > 0) {
		ID3D11UnorderedAccessView* ringUAVs[4] = { mapUAV, nullptr, nullptr, printRim ? printRim->uav.get() : nullptr };
		a_context->CSSetUnorderedAccessViews(0, 4, ringUAVs, nullptr);
		a_context->CSSetShader(printRingCS.get(), nullptr, 0);
		a_context->Dispatch((ringTotal + 63) / 64, 1, 1);
		a_context->CSSetUnorderedAccessViews(0, 4, nullUAV, nullptr);
	}
	printWindowMin = newMin;
	printWindowValid = true;
	printWorldspace = worldspace;

	if (persist) {
		if (fullClear) {
			printInjectsLast += InjectPrintRect(a_context, newWindow, clock);
		} else {
			for (uint32_t i = 0; i < cb.RingRectCount; i++)
				printInjectsLast += InjectPrintRect(a_context, cb.RingRects[i], clock);
		}
	}

	const int32_t blockMask = dim / 8 - 1;
	std::vector<uint32_t> touched;
	auto addBlocks = [&](DirectX::XMINT4 a_rect) {
		DirectX::XMINT4 clipped{};
		if (!Overlap(a_rect, newWindow, clipped))
			return;
		const int32_t bx0 = FloorDiv(clipped.x, 8);
		const int32_t by0 = FloorDiv(clipped.y, 8);
		const int32_t bx1 = FloorDiv(clipped.x + clipped.z - 1, 8);
		const int32_t by1 = FloorDiv(clipped.y + clipped.w - 1, 8);
		for (int32_t by = by0; by <= by1; by++)
			for (int32_t bx = bx0; bx <= bx1; bx++) {
				if (printBlockScratch.size() >= kPrintBlockCap)
					return;
				const uint32_t px = (uint32_t)(bx & blockMask);
				const uint32_t py = (uint32_t)(by & blockMask);
				const uint32_t seen = py * (uint32_t)(dim / 8) + px;
				if (printBlockSeen[seen])
					continue;
				printBlockSeen[seen] = 1;
				touched.push_back(seen);
				printBlockScratch.push_back(px | (py << 16));
			}
	};
	auto uploadBlocks = [&]() {
		for (uint32_t seen : touched)
			printBlockSeen[seen] = 0;
		touched.clear();
		if (printBlockScratch.empty())
			return false;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(printBlockBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		std::memcpy(mapped.pData, printBlockScratch.data(), printBlockScratch.size() * sizeof(uint32_t));
		a_context->Unmap(printBlockBuffer.get(), 0);
		return true;
	};
	ID3D11ShaderResourceView* blockSRV = printBlockSRV.get();

	if (quanta > 0 && !printLive.empty()) {
		printBlockScratch.clear();
		for (const auto& [id, live] : printLive)
			addBlocks(TileRect(PrintTileX(id), PrintTileY(id), kPrintTileDim));
		const uint32_t blocks = (uint32_t)printBlockScratch.size();
		if (uploadBlocks()) {
			a_context->CSSetShaderResources(2, 1, &blockSRV);
			a_context->CSSetUnorderedAccessViews(0, 1, &mapUAV, nullptr);
			a_context->CSSetShader(printRefillCS.get(), nullptr, 0);
			a_context->Dispatch(blocks, 1, 1);
			a_context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		}
		const float refill = (float)quanta / 1023.0f;
		for (auto it = printLive.begin(); it != printLive.end();) {
			it->second.refillSince += refill;
			if (it->second.refillSince >= 1.0f && !it->second.dirty)
				it = printLive.erase(it);
			else
				++it;
		}
	}

	ID3D11ShaderResourceView* topSRVs[2] = {
		heightTopRaw[heightCurrent] ? heightTopRaw[heightCurrent]->srv.get() : nullptr,
		heightTop2Raw[heightCurrent] ? heightTop2Raw[heightCurrent]->srv.get() : nullptr
	};
	ID3D11ShaderResourceView* contactSRVs[3] = {
		(contactHeight && a_frame.ContactDim > 0.5f) ? contactHeight->srv.get() : nullptr,
		shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr,
		(a_frame.HasRoadRaster > 0.5f && heightSkinDepth) ? heightSkinDepth->srv.get() : nullptr
	};
	const bool fieldReady = a_trenchPassRan && contactSRVs[1] && heightMapValid && topSRVs[0] && topSRVs[1] && (contactSRVs[0] || spellCount > 0);

	std::vector<DirectX::XMINT4> stampRects;
	if (fieldReady) {
		printBlockScratch.clear();
		auto texelRect = [](float a_minX, float a_minY, float a_maxX, float a_maxY) {
			const int32_t x0 = (int32_t)std::floor(a_minX / kPrintTexel);
			const int32_t y0 = (int32_t)std::floor(a_minY / kPrintTexel);
			const int32_t x1 = (int32_t)std::floor(a_maxX / kPrintTexel);
			const int32_t y1 = (int32_t)std::floor(a_maxY / kPrintTexel);
			return DirectX::XMINT4{ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
		};
		const float pad = 2.0f * kPrintTexel;
		if (contactSRVs[0]) {
			for (const auto& actor : contactActors)
				stampRects.push_back(texelRect(actor.minX - pad, actor.minY - pad, actor.maxX + pad, actor.maxY + pad));
			for (const auto& prop : contactProps)
				stampRects.push_back(texelRect(prop.minX - pad, prop.minY - pad, prop.maxX + pad, prop.maxY + pad));
		}
		for (const auto& rect : stampRects)
			addBlocks(rect);
		const uint32_t blocks = (uint32_t)printBlockScratch.size();
		auto readback = std::find_if(printWriteReadbacks.begin(), printWriteReadbacks.end(), [](const PrintWriteReadback& r) { return !r.busy; });
		if (blocks > 0 && readback != printWriteReadbacks.end() && uploadBlocks()) {
			const UINT zero[4] = {};
			a_context->ClearUnorderedAccessViewUint(printWriteUAV.get(), zero);
			a_context->CSSetShaderResources(2, 1, &blockSRV);
			a_context->CSSetShaderResources(7, 3, contactSRVs);
			a_context->CSSetShaderResources(10, 2, topSRVs);
			ID3D11ShaderResourceView* spellSRVs[2] = {};
			a_context->CSSetShaderResources(13, 2, spellSRVs);
			ID3D11UnorderedAccessView* stampUAVs[2] = { mapUAV, printWriteUAV.get() };
			a_context->CSSetUnorderedAccessViews(0, 2, stampUAVs, nullptr);
			a_context->CSSetShader(printStampCS.get(), nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::Objects - Trench Prints");
			a_context->Dispatch(blocks, 1, 1);
			globals::profiler->EndPass();
			a_context->CSSetUnorderedAccessViews(0, 2, nullUAV, nullptr);
			a_context->CopyResource(readback->staging.get(), printWriteBuffer.get());
			readback->tileOrigin = { FloorDiv(newMin.x, kPrintTileDim), FloorDiv(newMin.y, kPrintTileDim) };
			readback->rowTiles = printMapDim / kPrintTileDim + 1;
			readback->worldspace = worldspace;
			readback->frame = printFrame;
			readback->busy = true;
			printStampBlocksLast = blocks;
		} else {
			if (blocks > 0 && readback == printWriteReadbacks.end())
				logger::debug("[SNOW DEFORMATION] print stamp skipped: every write-flag readback is in flight");
			for (uint32_t seen : touched)
				printBlockSeen[seen] = 0;
			touched.clear();
		}
	}
	uint32_t rimBlocks = 0;
	const bool rimAll = fullClear || moved || quanta > 0 || printInjectsLast > 0 || cb.RimWidth != printRimWidthLast;
	if (printRim && GetPrintRimCS() && ((rimAll && !printLive.empty()) || printStampBlocksLast > 0)) {
		printBlockScratch.clear();
		auto addApron = [&](const DirectX::XMINT4& a_rect) {
			addBlocks({ a_rect.x - kPrintRimApron, a_rect.y - kPrintRimApron, a_rect.z + 2 * kPrintRimApron, a_rect.w + 2 * kPrintRimApron });
		};
		if (rimAll)
			for (const auto& [id, live] : printLive)
				addApron(TileRect(PrintTileX(id), PrintTileY(id), kPrintTileDim));
		if (printStampBlocksLast > 0)
			for (const auto& rect : stampRects)
				addApron(rect);
		const uint32_t blocks = (uint32_t)printBlockScratch.size();
		if (blocks > 0 && uploadBlocks()) {
			ID3D11ShaderResourceView* mapSRV = printMap->srv.get();
			a_context->CSSetShaderResources(2, 1, &blockSRV);
			a_context->CSSetShaderResources(12, 1, &mapSRV);
			ID3D11UnorderedAccessView* rimUAVs[4] = { nullptr, nullptr, nullptr, printRim->uav.get() };
			a_context->CSSetUnorderedAccessViews(0, 4, rimUAVs, nullptr);
			a_context->CSSetShader(printRimCS.get(), nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::Objects - Trench Prints");
			a_context->Dispatch(blocks, 1, 1);
			globals::profiler->EndPass();
			a_context->CSSetUnorderedAccessViews(0, 4, nullUAV, nullptr);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			a_context->CSSetShaderResources(12, 1, &nullSRV);
			rimBlocks = blocks;
		}
	}
	if (rimAll && (rimBlocks > 0 || printLive.empty()))
		printRimWidthLast = cb.RimWidth;
	if (!printStampBlocksLast && !rimBlocks)
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Trench Prints");

	if (persist) {
		uint32_t queued = 0;
		for (auto& [id, live] : printLive) {
			if (queued >= kPrintMirrorPerFrame)
				break;
			if (!live.dirty)
				continue;
			if (!QueuePrintMirror(a_context, PrintTileX(id), PrintTileY(id), clock))
				break;
			live.dirty = false;
			queued++;
		}
	}

	PublishPrintTiles(a_context);

	printSpellsLast = spellCount;
	a_context->CSSetShader(nullptr, nullptr, 0);
	ID3D11ShaderResourceView* nullSRVs[13] = {};
	a_context->CSSetShaderResources(2, 13, nullSRVs);
	a_context->CSSetUnorderedAccessViews(0, 4, nullUAV, nullptr);
	ID3D11Buffer* nullCBs[2] = {};
	a_context->CSSetConstantBuffers(0, 2, nullCBs);
}

DirectX::XMINT4 SnowDeformation::PrintRectForFrame()
{
	printDimTarget = SnapPrintDim(settings.ObjectTrenchResolution);
	const int32_t dim = (int32_t)printDimTarget;
	printWindowTarget = { (int32_t)std::floor((windowOrigin.x + deformWorldSize * 0.5f) / kPrintTexel) - dim / 2,
		(int32_t)std::floor((windowOrigin.y + deformWorldSize * 0.5f) / kPrintTexel) - dim / 2 };
	printRectPublished = { 0, 0, 0, 0 };
	DirectX::XMINT4 both{};
	if (settings.EnableSnowDeformation && printMap && printWindowValid && !printLive.empty() &&
		!printReinjectRequested.load(std::memory_order_acquire) && printWorldspace == activeWorldspace.load(std::memory_order_acquire) &&
		Overlap({ printWindowMin.x, printWindowMin.y, (int32_t)printMapDim, (int32_t)printMapDim }, { printWindowTarget.x, printWindowTarget.y, dim, dim }, both))
		printRectPublished = { both.x, both.y, both.x + both.z, both.y + both.w };
	return printRectPublished;
}

void SnowDeformation::PublishPrintTiles(ID3D11DeviceContext* a_context)
{
	if (!printTiles)
		return;
	const int32_t n = (int32_t)printMapDim / kPrintTileDim;
	const int32_t mask = n - 1;
	printTilesScratch.assign((size_t)n * n, 0);
	const int32_t w = n + 1;
	printTileSumOrigin = { FloorDiv(printWindowMin.x, kPrintTileDim), FloorDiv(printWindowMin.y, kPrintTileDim) };
	printTileSumDim = w;
	printTileSum.assign((size_t)(w + 1) * (w + 1), 0u);
	for (const auto& [id, live] : printLive) {
		const int32_t tx = PrintTileX(id);
		const int32_t ty = PrintTileY(id);
		for (int32_t dy = 0; dy <= 1; dy++)
			for (int32_t dx = 0; dx <= 1; dx++)
				printTilesScratch[(size_t)((ty - dy) & mask) * n + ((tx - dx) & mask)] = 1;
		const int32_t lx = tx - printTileSumOrigin.x;
		const int32_t ly = ty - printTileSumOrigin.y;
		if (lx >= 0 && ly >= 0 && lx < w && ly < w)
			printTileSum[(size_t)(ly + 1) * (w + 1) + lx + 1] = 1u;
	}
	for (int32_t y = 1; y <= w; y++)
		for (int32_t x = 1; x <= w; x++)
			printTileSum[(size_t)y * (w + 1) + x] += printTileSum[(size_t)(y - 1) * (w + 1) + x] + printTileSum[(size_t)y * (w + 1) + x - 1] -
			                                         printTileSum[(size_t)(y - 1) * (w + 1) + x - 1];
	if (printTilesScratch != printTilesUploaded) {
		a_context->UpdateSubresource(printTiles->resource.get(), 0, nullptr, printTilesScratch.data(), (UINT)n, 0);
		printTilesUploaded = printTilesScratch;
	}
}

bool SnowDeformation::PrintsUnder(const RE::NiBound& a_bound) const
{
	if (printTileSumDim <= 0 || printRectPublished.z <= printRectPublished.x || printRectPublished.w <= printRectPublished.y ||
		!std::isfinite(a_bound.center.x) || !std::isfinite(a_bound.center.y) || !(a_bound.radius >= 0.0f && a_bound.radius < 1e6f))
		return false;
	const float reach = a_bound.radius + (2.0f + (float)kPrintRimApron) * kPrintTexel;
	const float tileSize = kPrintTexel * (float)kPrintTileDim;
	const int32_t w = printTileSumDim;
	const int32_t x0 = std::max((int32_t)std::floor((a_bound.center.x - reach) / tileSize) - printTileSumOrigin.x, 0);
	const int32_t y0 = std::max((int32_t)std::floor((a_bound.center.y - reach) / tileSize) - printTileSumOrigin.y, 0);
	const int32_t x1 = std::min((int32_t)std::floor((a_bound.center.x + reach) / tileSize) - printTileSumOrigin.x, w - 1);
	const int32_t y1 = std::min((int32_t)std::floor((a_bound.center.y + reach) / tileSize) - printTileSumOrigin.y, w - 1);
	if (x0 > x1 || y0 > y1)
		return false;
	const auto sum = [&](int32_t a_x, int32_t a_y) { return printTileSum[(size_t)a_y * (w + 1) + a_x]; };
	return sum(x1 + 1, y1 + 1) - sum(x0, y1 + 1) - sum(x1 + 1, y0) + sum(x0, y0) > 0u;
}

void SnowDeformation::BindPrintReaders(ID3D11DeviceContext* a_context) const
{
	const bool live = printMap && printTiles && printRectPublished.z > printRectPublished.x && printRectPublished.w > printRectPublished.y;
	ID3D11ShaderResourceView* srvs[3] = { live ? printMap->srv.get() : nullptr, live ? printTiles->srv.get() : nullptr,
		(live && printRim) ? printRim->srv.get() : nullptr };
	a_context->PSSetShaderResources(kPrintReaderSlot, 3, srvs);
}

void SnowDeformation::SavePrintStore(const SKSE::SerializationInterface* a_intfc)
{
	std::scoped_lock lock(printStoreMutex);
	if (!a_intfc->OpenRecord(kPrintRecord, kPrintRecordVersion)) {
		logger::error("[SNOW DEFORMATION] Could not open the print co-save record");
		return;
	}
	uint32_t tileCount = 0;
	if (settings.PersistTrenches)
		for (const auto& [key, tile] : printStore)
			tileCount += AnyPrint(tile.texels) ? 1u : 0u;
	const uint16_t tileDim = (uint16_t)kPrintTileDim;
	const uint16_t pad = 0;
	const float texel = kPrintTexel;
	a_intfc->WriteRecordData(&tileCount, sizeof(tileCount));
	a_intfc->WriteRecordData(&tileDim, sizeof(tileDim));
	a_intfc->WriteRecordData(&pad, sizeof(pad));
	a_intfc->WriteRecordData(&texel, sizeof(texel));
	if (tileCount == 0) {
		logger::info("[SNOW DEFORMATION] print co-save: no tiles");
		return;
	}

	size_t entriesTotal = 0;
	for (const auto& [key, tile] : printStore) {
		if (!AnyPrint(tile.texels))
			continue;
		uint32_t entries = 0;
		for (uint32_t v : tile.texels)
			entries += (v & 1023u) ? 1u : 0u;
		const float clockRel = std::min(tile.clock - printClockSnapshot, 0.0f);
		a_intfc->WriteRecordData(&key.worldspace, sizeof(key.worldspace));
		a_intfc->WriteRecordData(&key.x, sizeof(key.x));
		a_intfc->WriteRecordData(&key.y, sizeof(key.y));
		a_intfc->WriteRecordData(&clockRel, sizeof(clockRel));
		a_intfc->WriteRecordData(&entries, sizeof(entries));
		for (size_t i = 0; i < tile.texels.size(); i++) {
			const uint32_t v = tile.texels[i];
			if ((v & 1023u) == 0u)
				continue;
			const uint16_t index = (uint16_t)i;
			a_intfc->WriteRecordData(&index, sizeof(index));
			a_intfc->WriteRecordData(&v, sizeof(v));
		}
		entriesTotal += entries;
	}
	logger::info("[SNOW DEFORMATION] print co-save: {} tiles, {} prints, {} KB", tileCount, entriesTotal, (tileCount * 20 + entriesTotal * 6) / 1024);
}

void SnowDeformation::LoadPrintStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t)
{
	if (a_version != kPrintRecordVersion) {
		logger::warn("[SNOW DEFORMATION] print co-save is version {}, this build reads {}; stored prints dropped", a_version, kPrintRecordVersion);
		return;
	}
	uint32_t tileCount = 0;
	uint16_t tileDim = 0;
	uint16_t pad = 0;
	float texel = 0.0f;
	if (a_intfc->ReadRecordData(&tileCount, sizeof(tileCount)) != sizeof(tileCount) ||
		a_intfc->ReadRecordData(&tileDim, sizeof(tileDim)) != sizeof(tileDim) ||
		a_intfc->ReadRecordData(&pad, sizeof(pad)) != sizeof(pad) ||
		a_intfc->ReadRecordData(&texel, sizeof(texel)) != sizeof(texel)) {
		logger::warn("[SNOW DEFORMATION] print co-save header would not read; nothing restored");
		return;
	}
	if (tileDim != (uint16_t)kPrintTileDim || std::abs(texel - kPrintTexel) > 0.01f) {
		logger::warn("[SNOW DEFORMATION] print co-save was written at {} texels of {} units, this build uses {} of {}; dropped",
			tileDim, texel, kPrintTileDim, kPrintTexel);
		return;
	}

	std::scoped_lock lock(printStoreMutex);
	printStore.clear();
	printStoreGeneration.fetch_add(1, std::memory_order_acq_rel);
	uint32_t restored = 0;
	const uint32_t tileValues = 2u * (uint32_t)kPrintTileTexels;
	for (uint32_t i = 0; i < tileCount; i++) {
		TrenchTileKey key{};
		float clockRel = 0.0f;
		uint32_t entries = 0;
		if (a_intfc->ReadRecordData(&key.worldspace, sizeof(key.worldspace)) != sizeof(key.worldspace) ||
			a_intfc->ReadRecordData(&key.x, sizeof(key.x)) != sizeof(key.x) ||
			a_intfc->ReadRecordData(&key.y, sizeof(key.y)) != sizeof(key.y) ||
			a_intfc->ReadRecordData(&clockRel, sizeof(clockRel)) != sizeof(clockRel) ||
			a_intfc->ReadRecordData(&entries, sizeof(entries)) != sizeof(entries))
			break;
		if (entries > tileValues) {
			logger::warn("[SNOW DEFORMATION] print co-save tile {} claims {} prints; the rest is dropped", i, entries);
			break;
		}
		PrintStoreTile tile{ std::vector<uint32_t>(tileValues, 0u), std::min(clockRel, 0.0f) };
		bool intact = true;
		for (uint32_t e = 0; e < entries && intact; e++) {
			uint16_t index = 0;
			uint32_t value = 0;
			intact = a_intfc->ReadRecordData(&index, sizeof(index)) == sizeof(index) &&
			         a_intfc->ReadRecordData(&value, sizeof(value)) == sizeof(value) && index < tileValues;
			if (intact)
				tile.texels[index] = value;
		}
		if (!intact) {
			logger::warn("[SNOW DEFORMATION] print co-save tile {} would not read; the rest is dropped", i);
			break;
		}
		printStore.insert_or_assign(key, std::move(tile));
		restored++;
	}
	printReinjectRequested.store(true, std::memory_order_release);
	logger::info("[SNOW DEFORMATION] print co-save restored {} of {} tiles (active worldspace {:08X})",
		restored, tileCount, activeWorldspace.load(std::memory_order_acquire));
}

void SnowDeformation::RegisterPrintCoSave()
{
	CoSave::GetSingleton()->Register(
		kPrintRecord, kPrintRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) { SavePrintStore(a_intfc); },
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) { LoadPrintStore(a_intfc, a_version, a_length); },
		[this]() { ClearPrintStore("the co-save revert"); });
}

void SnowDeformation::ClearPrintStore(const char* a_why)
{
	std::scoped_lock lock(printStoreMutex);
	if (!printStore.empty())
		logger::info("[SNOW DEFORMATION] print store CLEARED by {}, {} tiles dropped", a_why, printStore.size());
	printStore.clear();
	printStoreGeneration.fetch_add(1, std::memory_order_acq_rel);
	printReinjectRequested.store(true, std::memory_order_release);
}
