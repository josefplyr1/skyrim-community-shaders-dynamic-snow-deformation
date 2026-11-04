// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/TrenchSimd.h"

#include "Globals.h"
#include "Utils/D3D.h"

#include <DirectXPackedVector.h>
#include <limits>

namespace
{
	inline int FloorDiv(int a_value, int a_divisor)
	{
		return a_value >= 0 ? a_value / a_divisor : -(((-a_value) + a_divisor - 1) / a_divisor);
	}
}

int SnowDeformation::ComputeArrivalRects(DirectX::XMINT2 a_scroll, bool a_clearing, ArrivalRect a_rects[2]) const
{
	const int dim = (int)deformMapDim;
	if (a_clearing || std::abs(a_scroll.x) >= dim || std::abs(a_scroll.y) >= dim) {
		a_rects[0] = { 0, 0, dim, dim };
		return 1;
	}
	int count = 0;
	if (a_scroll.x != 0)
		a_rects[count++] = { a_scroll.x > 0 ? dim - a_scroll.x : 0, 0, std::abs(a_scroll.x), dim };
	if (a_scroll.y != 0)
		a_rects[count++] = { 0, a_scroll.y > 0 ? dim - a_scroll.y : 0, dim, std::abs(a_scroll.y) };
	return count;
}

void SnowDeformation::CopyLogicalBox(ID3D11Texture2D* a_dst, ID3D11Texture2D* a_src,
	int a_x0, int a_y0, int a_w, int a_h, DirectX::XMINT2 a_physOrigin)
{
	auto context = globals::d3d::context;
	if (!context)
		return;
	const int dim = (int)deformMapDim;
	const int mask = dim - 1;

	int yDone = 0;
	while (yDone < a_h) {
		const int py = (a_y0 + yDone + a_physOrigin.y) & mask;
		const int yRun = std::min(a_h - yDone, dim - py);
		int xDone = 0;
		while (xDone < a_w) {
			const int px = (a_x0 + xDone + a_physOrigin.x) & mask;
			const int xRun = std::min(a_w - xDone, dim - px);
			const D3D11_BOX box{ (UINT)px, (UINT)py, 0, (UINT)(px + xRun), (UINT)(py + yRun), 1 };
			context->CopySubresourceRegion(a_dst, 0, (UINT)xDone, (UINT)yDone, 0, a_src, 0, &box);
			xDone += xRun;
		}
		yDone += yRun;
	}
}

bool SnowDeformation::CreateTrenchStoreResources()
{
	LoadTraceScope _loadTrace(this, "TrenchStore: CreateTrenchStoreResources");
	auto device = globals::d3d::device;
	if (!device)
		return false;

	trenchInjectTexture = nullptr;
	trenchInjectSRV = nullptr;
	for (int ring = 0; ring < 2; ring++)
		for (int axis = 0; axis < 2; axis++) {
			trenchBandStaging[ring][axis] = nullptr;
			trenchClockBandStaging[ring][axis] = nullptr;
			trenchBandValid[ring][axis] = false;
		}

	D3D11_TEXTURE2D_DESC injectDesc{};
	injectDesc.Width = deformMapDim;
	injectDesc.Height = deformMapDim;
	injectDesc.MipLevels = 1;
	injectDesc.ArraySize = 1;
	injectDesc.Format = DXGI_FORMAT_R8G8_UNORM;
	injectDesc.SampleDesc.Count = 1;
	injectDesc.Usage = D3D11_USAGE_DEFAULT;
	injectDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&injectDesc, nullptr, trenchInjectTexture.put())))
		return false;
	Util::SetResourceName(trenchInjectTexture.get(), "SnowDeformation::TrenchInject");
	if (FAILED(device->CreateShaderResourceView(trenchInjectTexture.get(), nullptr, trenchInjectSRV.put())))
		return false;
	Util::SetResourceName(trenchInjectSRV.get(), "SnowDeformation::TrenchInject SRV");

	D3D11_TEXTURE2D_DESC bandDesc{};
	bandDesc.MipLevels = 1;
	bandDesc.ArraySize = 1;
	bandDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	bandDesc.SampleDesc.Count = 1;
	bandDesc.Usage = D3D11_USAGE_STAGING;
	bandDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	D3D11_TEXTURE2D_DESC clockBandDesc = bandDesc;
	clockBandDesc.Format = DXGI_FORMAT_R16_FLOAT;
	for (int ring = 0; ring < 2; ring++) {
		for (int axis = 0; axis < 2; axis++) {
			bandDesc.Width = clockBandDesc.Width = axis == 0 ? (UINT)kTrenchBandMax : deformMapDim;
			bandDesc.Height = clockBandDesc.Height = axis == 0 ? deformMapDim : (UINT)kTrenchBandMax;
			if (FAILED(device->CreateTexture2D(&bandDesc, nullptr, trenchBandStaging[ring][axis].put())) ||
				FAILED(device->CreateTexture2D(&clockBandDesc, nullptr, trenchClockBandStaging[ring][axis].put())))
				return false;
			Util::SetResourceName(trenchBandStaging[ring][axis].get(), "SnowDeformation::TrenchBandStaging");
			Util::SetResourceName(trenchClockBandStaging[ring][axis].get(), "SnowDeformation::TrenchClockBandStaging");
		}
	}

	bandDesc.Width = clockBandDesc.Width = deformMapDim;
	bandDesc.Height = clockBandDesc.Height = (UINT)kTrenchRollRows;
	for (int ring = 0; ring < 2; ring++) {
		trenchRollStaging[ring] = nullptr;
		trenchClockRollStaging[ring] = nullptr;
		trenchRollValid[ring] = false;
		if (FAILED(device->CreateTexture2D(&bandDesc, nullptr, trenchRollStaging[ring].put())) ||
			FAILED(device->CreateTexture2D(&clockBandDesc, nullptr, trenchClockRollStaging[ring].put())))
			return false;
		Util::SetResourceName(trenchRollStaging[ring].get(), "SnowDeformation::TrenchRollStaging");
		Util::SetResourceName(trenchClockRollStaging[ring].get(), "SnowDeformation::TrenchClockRollStaging");
	}
	trenchRollRow = 0;

	trenchDebugTexture = nullptr;
	trenchDebugSRV = nullptr;
	trenchDebugUAV = nullptr;

	trenchInjectScratch.assign((size_t)deformMapDim * deformMapDim * 2, 0);
	trenchMapPrimed = false;
	return true;
}

void SnowDeformation::UpdateTrenchDebugTexture()
{
	if (!showDebugTexture)
		return;
	if (!trenchDebugUAV) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = deformMapDim;
		desc.Height = deformMapDim;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		auto device = globals::d3d::device;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, trenchDebugTexture.put())))
			return;
		Util::SetResourceName(trenchDebugTexture.get(), "SnowDeformation::TrenchDebugDepth");
		device->CreateShaderResourceView(trenchDebugTexture.get(), nullptr, trenchDebugSRV.put());
		device->CreateUnorderedAccessView(trenchDebugTexture.get(), nullptr, trenchDebugUAV.put());
		if (!trenchDebugUAV)
			return;
	}
	if (snowPrimeState.load(std::memory_order_acquire) == 1)
		return;

	auto context = globals::d3d::context;
	auto* live = deformationTextures[0];
	auto* shader = GetSnowCS(kTrenchDebugCS);
	if (!context || !live || !shader)
		return;

	ID3D11Buffer* cb = perFrame->CB();
	context->CSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView* srvs[] = { live->srv.get() };
	ID3D11UnorderedAccessView* uavs[] = { trenchDebugUAV.get() };
	context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch(deformMapDim / 8, deformMapDim / 8, 1);

	ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
	ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetShaderResources(0, 1, nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
}

void SnowDeformation::MarkTrenchDirtyRows(const PerFrame& a_data)
{
	if (!settings.PersistTrenches)
		return;

	const int dim = (int)deformMapDim;
	if (trenchDirtyRows.size() != (size_t)(dim + 31) / 32)
		trenchDirtyRows.assign((size_t)(dim + 31) / 32, 0u);

	const float texel = a_data.TexelSize;
	if (texel <= 0.0f)
		return;

	auto markSpan = [&](float a_minY, float a_maxY) {
		const int row0 = std::max((int)std::floor((a_minY - a_data.WindowOrigin.y) / texel), 0);
		const int row1 = std::min((int)std::floor((a_maxY - a_data.WindowOrigin.y) / texel), dim - 1);
		for (int row = row0; row <= row1; row++) {
			uint32_t& word = trenchDirtyRows[(size_t)row >> 5];
			const uint32_t bit = 1u << (row & 31);
			cpuCensus.rowsMarkedTotal += (word & bit) ? 0 : 1;
			word |= bit;
		}
	};

	auto marks = [&](uint32_t a_age) {
		if (a_age < kTrenchMarkSettleFrames || a_age % kTrenchMarkRefreshFrames == 0)
			return true;
		cpuCensus.settledSkipsTotal++;
		return false;
	};
	const float tol = texel * kTrenchMarkTolTexels;
	auto within = [tol](float a_a, float a_b) { return std::abs(a_a - a_b) <= tol; };
	trenchStampAnchorClaimed.assign(trenchStampAnchors.size(), 0);

	cpuCensus.stampsTotal += a_data.StampCount;
	for (uint i = 0; i < a_data.StampCount && i < kMaxStamps; i++) {
		const float stampMode = a_data.StampEnds[i].z;
		if ((stampMode > kStampModeBody - 0.5f ? stampMode - kStampModeBody : stampMode) > 0.5f)
			continue;
		if (a_data.Stamps[i].z <= 0.0f)
			continue;
		cpuCensus.carveStampsTotal++;

		const auto& s = a_data.Stamps[i];
		const auto& e = a_data.StampEnds[i];
		TrenchStampAnchor anchor{ s, { e.x, e.y }, 0 };
		for (size_t j = 0; j < trenchStampAnchors.size(); j++) {
			const auto& a = trenchStampAnchors[j];
			if (!trenchStampAnchorClaimed[j] && within(a.s.x, s.x) && within(a.s.y, s.y) && within(a.e.x, e.x) && within(a.e.y, e.y) &&
				std::abs(a.s.z - s.z) <= 2.0f && std::abs(a.s.w - s.w) <= 1.0f) {
				trenchStampAnchorClaimed[j] = 1;
				anchor = a;
				anchor.age = std::min(a.age + 1, 0xFFFFFFu);
				break;
			}
		}
		trenchStampAnchorsNext.push_back(anchor);
		if (!marks(anchor.age))
			continue;

		markSpan(std::min(s.y, e.y) - s.w, std::max(s.y, e.y) + s.w);
	}

	for (const auto* draws : { &contactActors, &contactProps })
		for (const auto& draw : *draws) {
			cpuCensus.contactRectsTotal++;
			const uint32_t handle = draw.ref.native_handle();
			TrenchRectAnchor anchor{ draw.minX, draw.minY, draw.maxX, draw.maxY, 0 };
			if (const auto it = trenchRectAnchors.find(handle); it != trenchRectAnchors.end()) {
				const auto& a = it->second;
				if (within(a.minX, draw.minX) && within(a.minY, draw.minY) && within(a.maxX, draw.maxX) && within(a.maxY, draw.maxY)) {
					anchor = a;
					anchor.age = std::min(a.age + 1, 0xFFFFFFu);
				}
			}
			trenchRectAnchorsNext[handle] = anchor;
			if (marks(anchor.age))
				markSpan(draw.minY, draw.maxY);
		}

	std::swap(trenchStampAnchors, trenchStampAnchorsNext);
	trenchStampAnchorsNext.clear();
	std::swap(trenchRectAnchors, trenchRectAnchorsNext);
	trenchRectAnchorsNext.clear();
}

void SnowDeformation::RollTrenchWindow()
{
	if (!settings.PersistTrenches || !trenchMapPrimed)
		return;

	auto context = globals::d3d::context;
	auto* live = deformationTextures[0];
	if (!context || !live || !live->resource)
		return;

	std::scoped_lock lock(trenchStoreMutex);

	const int dim = (int)deformMapDim;
	const bool haveMarks = trenchDirtyRows.size() == (size_t)(dim + 31) / 32;

	if (haveMarks) {
		int skipped = 0;
		while (skipped < dim && !(trenchDirtyRows[(size_t)trenchRollRow >> 5] & (1u << (trenchRollRow & 31)))) {
			if (++trenchRollRow >= dim)
				trenchRollRow = 0;
			skipped++;
		}
		if (skipped >= dim)
			return;
	}

	const int ring = trenchRollRing;
	trenchRollRing ^= 1;

	if (trenchRollValid[ring]) {
		D3D11_MAPPED_SUBRESOURCE stale{};
		D3D11_MAPPED_SUBRESOURCE staleClocks{};
		if (SUCCEEDED(context->Map(trenchRollStaging[ring].get(), 0, D3D11_MAP_READ, 0, &stale))) {
			const bool dated = SUCCEEDED(context->Map(trenchClockRollStaging[ring].get(), 0, D3D11_MAP_READ, 0, &staleClocks));
			cpuCensus.trenchStorePath = CpuCensus::kStoreRollStale;
			StoreTrenchBand(trenchRollMeta[ring], stale, dated ? &staleClocks : nullptr);
			if (dated)
				context->Unmap(trenchClockRollStaging[ring].get(), 0);
			context->Unmap(trenchRollStaging[ring].get(), 0);
		}
		trenchRollValid[ring] = false;
	}

	const int start = trenchRollRow;
	int rows = std::min(kTrenchRollRows, dim - start);

	if (haveMarks) {
		int last = start;
		for (int row = start; row < start + rows; row++)
			if (trenchDirtyRows[(size_t)row >> 5] & (1u << (row & 31)))
				last = row;
		rows = last - start + 1;

		for (int row = start; row < start + rows; row++)
			trenchDirtyRows[(size_t)row >> 5] &= ~(1u << (row & 31));
	}

	CopyLogicalBox(trenchRollStaging[ring].get(), live->resource.get(), 0, start, dim, rows, trenchMapPhysOrigin);
	if (trackClockTexture)
		CopyLogicalBox(trenchClockRollStaging[ring].get(), trackClockTexture->resource.get(), 0, start, dim, rows, trenchMapPhysOrigin);

	trenchRollMeta[ring] = { trenchMapOrigin, trenchMapTexel, trenchMapWorldspace, 0, start, dim, rows, TrackDateClock(), trenchAgeingTotal };
	trenchRollValid[ring] = true;

	trenchRollRow = start + rows;
	if (trenchRollRow >= dim)
		trenchRollRow = 0;
}

void SnowDeformation::TickTrenchClock()
{
	std::scoped_lock lock(trenchStoreMutex);

	if (gameClock.reversed) {
		logger::info("[SNOW DEFORMATION] trench clock went backwards; skipping the tick ({} tiles kept)",
			trenchTiles.size());
		return;
	}
	const float elapsed = gameClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

	const float refillIntensity = settings.RefillOnlyWhenSnowing ? snowfallIntensity : 1.0f;
	const float realSecondsPerGameHour = 3600.0f / std::max(gameClock.timescale, 1.0f);
	const float weather = realSecondsPerGameHour * refillIntensity *
	                      std::max(settings.RefillRateMultiplier, 0.0f) / kBaseRefillTime;
	const float floorRate = settings.StoredTrenchFadeDays > 0.01f ?
	                            1.0f / (settings.StoredTrenchFadeDays * 24.0f) :
	                            0.0f;

	trenchDecayClock += elapsed * (weather + floorRate);
}

void SnowDeformation::TickTrenchAgeing()
{
	std::scoped_lock lock(trenchStoreMutex);
	trenchAgeingTotal = snowClimate.ageingTotal;
}

bool SnowDeformation::DecayTrenchTile(TrenchTile& a_tile)
{
	if (const int age = (int)std::floor((trenchAgeingTotal - a_tile.freshClock) * kTrackFreshPerUnit); age > 0) {
		a_tile.freshClock += (double)age / kTrackFreshPerUnit;
		if (!a_tile.fresh.empty() && !TrenchSimd::DecayBytes(a_tile.fresh.data(), a_tile.fresh.size(), std::min(age, 255)))
			std::vector<uint8_t>().swap(a_tile.fresh);
	}

	const float pending = trenchDecayClock - a_tile.clock;
	if (pending <= 0.0f)
		return true;

	const int drop = (int)std::floor(pending * 255.0f);
	if (drop <= 0)
		return true;
	a_tile.clock += (float)drop / 255.0f;

	return TrenchSimd::DecayBytes(a_tile.depth.data(), a_tile.depth.size(), drop);
}

void SnowDeformation::SweepTrenchStore()
{
	std::scoped_lock lock(trenchStoreMutex);

	if (trenchTiles.empty()) {
		trenchSweepQueue.clear();
		trenchStatNonZero = trenchStatThin = trenchStatSweptTiles = 0;
		trenchAccumNonZero = trenchAccumThin = trenchAccumTiles = 0;
		trenchStatTiles = 0;
		trenchEncodedTotal = 0;
		return;
	}

	if (trenchSweepQueue.empty()) {
		trenchStatNonZero = trenchAccumNonZero;
		trenchStatThin = trenchAccumThin;
		trenchStatSweptTiles = trenchAccumTiles;
		const size_t live = trenchTiles.size();
		if (live != trenchStatTiles) {
			const long long delta = (long long)live - (long long)trenchStatTiles;
			const long long accounted = -(long long)(trenchAccumErased + trenchAccumEvicted);
			const long long unexplained = delta - accounted - (long long)trenchAccumAdded;
			logger::info("[SNOW DEFORMATION] trench store census: {} -> {} ({:+}), decay -{}, evicted -{}, added +{}{}",
				trenchStatTiles, live, delta, trenchAccumErased, trenchAccumEvicted, trenchAccumAdded,
				unexplained ? std::format(", UNEXPLAINED {:+}", unexplained) : "");
		}
		trenchAccumNonZero = trenchAccumThin = trenchAccumTiles = 0;
		trenchAccumErased = trenchAccumEvicted = trenchAccumAdded = 0;
		trenchEncodedTotal = 0;
		for (const auto& [key, tile] : trenchTiles)
			trenchEncodedTotal += tile.encodedBytes + kTrenchTileHeaderBytes;
		EnforceTrenchBudget();
		trenchStatTiles = trenchTiles.size();

		trenchSweepQueue.reserve(trenchTiles.size());
		for (const auto& [key, tile] : trenchTiles)
			trenchSweepQueue.push_back(key);
	}

	constexpr int kPerFrame = 8;
	constexpr size_t texels = (size_t)kTrenchTileDim * kTrenchTileDim;
	for (int i = 0; i < kPerFrame && !trenchSweepQueue.empty(); i++) {
		const TrenchTileKey key = trenchSweepQueue.back();
		trenchSweepQueue.pop_back();
		auto it = trenchTiles.find(key);
		if (it == trenchTiles.end())
			continue;

		if (!DecayTrenchTile(it->second)) {
			trenchTiles.erase(it);
			trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
			trenchSampleTile = nullptr;
			trenchAccumErased++;
			continue;
		}

		const auto& bytes = it->second.depth;
		size_t nonZero = 0;
		size_t pairs = 0;
		TrenchSimd::CountRuns(bytes.data(), bytes.size(), nonZero, pairs);

		const uint32_t encoded = (uint32_t)(pairs * kTrenchRLEPairBytes);
		it->second.encodedBytes = std::min(encoded, (uint32_t)bytes.size());
		if (const auto& fresh = it->second.fresh; !fresh.empty()) {
			size_t freshNonZero = 0;
			size_t freshPairs = 0;
			TrenchSimd::CountRuns(fresh.data(), fresh.size(), freshNonZero, freshPairs);
			it->second.encodedBytes += std::min((uint32_t)(freshPairs * kTrenchRLEPairBytes), (uint32_t)fresh.size());
		}

		trenchAccumNonZero += nonZero;
		trenchAccumTiles++;
		if (nonZero * 20 < texels)
			trenchAccumThin++;
	}
}

void SnowDeformation::EnforceTrenchBudget()
{
	const size_t budget = (size_t)(std::max(settings.TrenchMemoryMB, 0.01f) * 1024.0f * 1024.0f);
	if (trenchEncodedTotal <= budget) {
		trenchKeepRadiusSq = std::numeric_limits<float>::max();
		return;
	}

	const float2 centre = { windowOrigin.x + deformWorldSize * 0.5f,
		windowOrigin.y + deformWorldSize * 0.5f };
	const uint32_t here = activeWorldspace.load(std::memory_order_acquire);

	trenchEvictScratch.clear();
	trenchEvictScratch.reserve(trenchTiles.size());
	for (const auto& [key, tile] : trenchTiles) {
		float rank;
		if (key.worldspace != here) {
			rank = std::numeric_limits<float>::max();
		} else {
			const float dx = ((float)key.x + 0.5f) * kTrenchTileWorld - centre.x;
			const float dy = ((float)key.y + 0.5f) * kTrenchTileWorld - centre.y;
			rank = dx * dx + dy * dy;
		}
		trenchEvictScratch.emplace_back(rank, key);
	}
	std::sort(trenchEvictScratch.begin(), trenchEvictScratch.end(),
		[](const auto& a, const auto& b) { return a.first > b.first; });

	size_t evicted = 0;
	float nearestEvicted = std::numeric_limits<float>::max();
	for (const auto& [rank, key] : trenchEvictScratch) {
		if (trenchEncodedTotal <= budget)
			break;
		auto it = trenchTiles.find(key);
		if (it == trenchTiles.end())
			continue;
		trenchEncodedTotal -= std::min(trenchEncodedTotal, (size_t)it->second.encodedBytes + kTrenchTileHeaderBytes);
		trenchTiles.erase(it);
		evicted++;
		nearestEvicted = rank;
	}
	trenchKeepRadiusSq = evicted ? nearestEvicted : std::numeric_limits<float>::max();
	trenchAccumEvicted += evicted;

	if (evicted) {
		trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
		trenchSampleTile = nullptr;
		logger::info("[SNOW DEFORMATION] trench store over budget: evicted {} of {} tiles, {} remain, {} KB encoded, keep radius {:.0f} units",
			evicted, trenchEvictScratch.size(), trenchTiles.size(), trenchEncodedTotal / 1024,
			std::sqrt(trenchKeepRadiusSq));
	}
}

void SnowDeformation::ClearTrenchStore(const char* a_reason)
{
	std::scoped_lock lock(trenchStoreMutex);
	ClearTrenchStoreLocked(a_reason);
}

void SnowDeformation::ClearTrenchStoreLocked(const char* a_reason)
{
	if (!trenchTiles.empty())
		logger::info("[SNOW DEFORMATION] trench store CLEARED by {}, {} tiles dropped", a_reason, trenchTiles.size());

	trenchTiles.clear();
	trenchStatTiles = 0;
	trenchEncodedTotal = 0;
	trenchSweepQueue.clear();
	trenchStatNonZero = trenchStatThin = trenchStatSweptTiles = 0;
	trenchAccumNonZero = trenchAccumThin = trenchAccumTiles = 0;
	trenchAccumErased = trenchAccumEvicted = trenchAccumAdded = 0;
	trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
	trenchSampleTile = nullptr;
	for (int ring = 0; ring < 2; ring++) {
		for (int axis = 0; axis < 2; axis++)
			trenchBandValid[ring][axis] = false;
		trenchRollValid[ring] = false;
	}
	trenchMapPrimed = false;
}

float SnowDeformation::ProbeTrenchStore(float a_worldX, float a_worldY)
{
	std::scoped_lock lock(trenchStoreMutex);
	return SampleTrenchStore(activeWorldspace.load(std::memory_order_acquire), a_worldX, a_worldY);
}

float SnowDeformation::SampleTrenchStore(uint32_t a_worldspace, float a_worldX, float a_worldY, float* o_fresh)
{
	constexpr float storeTexel = kTrenchTileWorld / (float)kTrenchTileDim;

	const float fx = a_worldX / storeTexel - 0.5f;
	const float fy = a_worldY / storeTexel - 0.5f;
	const int gx = (int)std::floor(fx);
	const int gy = (int)std::floor(fy);
	const float tx = fx - (float)gx;
	const float ty = fy - (float)gy;

	float corner[4] = {};
	float cornerFresh[4] = {};
	for (int i = 0; i < 4; i++) {
		const int sx = gx + (i & 1);
		const int sy = gy + (i >> 1);
		const TrenchTileKey key{ a_worldspace, FloorDiv(sx, kTrenchTileDim), FloorDiv(sy, kTrenchTileDim) };
		if (key != trenchSampleKey) {
			auto it = trenchTiles.find(key);
			trenchSampleTile = it == trenchTiles.end() ? nullptr : &it->second;
			trenchSampleKey = key;
		}
		if (!trenchSampleTile)
			continue;
		const int lx = sx - key.x * kTrenchTileDim;
		const int ly = sy - key.y * kTrenchTileDim;
		corner[i] = std::max(0.0f,
			trenchSampleTile->depth[(size_t)ly * kTrenchTileDim + lx] * (1.0f / 255.0f) -
				(trenchDecayClock - trenchSampleTile->clock));
		if (o_fresh && !trenchSampleTile->fresh.empty())
			cornerFresh[i] = std::max(0.0f, (float)(trenchSampleTile->fresh[(size_t)ly * kTrenchTileDim + lx] -
													  (trenchAgeingTotal - trenchSampleTile->freshClock) * kTrackFreshPerUnit) *
												 (1.0f / 255.0f));
	}

	const float w[4] = { (1.0f - tx) * (1.0f - ty), tx * (1.0f - ty), (1.0f - tx) * ty, tx * ty };
	if (o_fresh) {
		float weight = 0.0f;
		float fresh = 0.0f;
		for (int i = 0; i < 4; i++) {
			weight += w[i] * corner[i];
			fresh += w[i] * corner[i] * cornerFresh[i];
		}
		*o_fresh = weight > 1e-6f ? fresh / weight : 0.0f;
	}
	return std::lerp(std::lerp(corner[0], corner[1], tx), std::lerp(corner[2], corner[3], tx), ty);
}

void SnowDeformation::StoreTrenchBand(const TrenchBandCopy& a_meta, const D3D11_MAPPED_SUBRESOURCE& a_mapped, const D3D11_MAPPED_SUBRESOURCE* a_clocks)
{
	cpuCensus.storeCalls[cpuCensus.trenchStorePath]++;
	constexpr float storeTexel = kTrenchTileWorld / (float)kTrenchTileDim;
	const float half = a_meta.texel * 0.5f;
	const double sinceCopy = trenchAgeingTotal - a_meta.ageingTotal;

	const float centreX = a_meta.origin.x + a_meta.texel * (float)deformMapDim * 0.5f;
	const float centreY = a_meta.origin.y + a_meta.texel * (float)deformMapDim * 0.5f;

	TrenchTileKey cachedKey{ 0, INT32_MIN, INT32_MIN };
	TrenchTile* cachedTile = nullptr;

	static const DirectX::PackedVector::HALF kEpsilonHalf = DirectX::PackedVector::XMConvertFloatToHalf(kTrenchStoreEpsilon);

	auto tileFor = [&](const TrenchTileKey& a_key) -> TrenchTile* {
		if (a_key != cachedKey) {
			auto it = trenchTiles.find(a_key);
			cachedKey = a_key;
			cachedTile = it == trenchTiles.end() ? nullptr : &it->second;
			if (cachedTile) {
				DecayTrenchTile(*cachedTile);
				cachedTile->lastTouch = gameClock.lastHours;
			}
		}
		if (!cachedTile) {
			if (trenchKeepRadiusSq < std::numeric_limits<float>::max()) {
				const float dx = ((float)a_key.x + 0.5f) * kTrenchTileWorld - centreX;
				const float dy = ((float)a_key.y + 0.5f) * kTrenchTileWorld - centreY;
				if (dx * dx + dy * dy >= trenchKeepRadiusSq)
					return nullptr;
			}
			TrenchTile fresh;
			fresh.depth.assign((size_t)kTrenchTileDim * kTrenchTileDim, 0);
			fresh.clock = trenchDecayClock;
			fresh.freshClock = trenchAgeingTotal;
			fresh.lastTouch = gameClock.lastHours;
			cachedTile = &trenchTiles.emplace(a_key, std::move(fresh)).first->second;
			trenchAccumAdded++;
		}
		return cachedTile;
	};

	auto freshOf = [&](TrenchTile& a_tile) -> uint8_t* {
		if (a_tile.fresh.empty()) {
			a_tile.fresh.assign((size_t)kTrenchTileDim * kTrenchTileDim, 0);
			a_tile.freshClock = trenchAgeingTotal;
		}
		return a_tile.fresh.data();
	};

	for (int row = 0; row < a_meta.h; row++) {
		const auto* src = reinterpret_cast<const DirectX::PackedVector::HALF*>(
			static_cast<const uint8_t*>(a_mapped.pData) + (size_t)row * a_mapped.RowPitch);
		const auto* dates = a_clocks ? reinterpret_cast<const DirectX::PackedVector::HALF*>(
										   static_cast<const uint8_t*>(a_clocks->pData) + (size_t)row * a_clocks->RowPitch) :
		                               nullptr;
		const float worldY = a_meta.origin.y + ((float)(a_meta.y0 + row) + 0.5f) * a_meta.texel;
		const int gy0 = (int)std::floor((worldY - half) / storeTexel);
		const int gy1 = (int)std::floor((worldY + half - 0.001f) / storeTexel);
		const int ky0 = FloorDiv(gy0, kTrenchTileDim);
		const int ky1 = FloorDiv(gy1, kTrenchTileDim);

		for (int col = 0; col < a_meta.w; col++) {
			if (!(col & 7) && col + 8 <= a_meta.w && !TrenchSimd::AnyAtLeast8(src + (size_t)col * 4, kEpsilonHalf)) {
				col += 7;
				continue;
			}
			const DirectX::PackedVector::HALF totalBits = src[(size_t)col * 4];
			if ((totalBits & 0x8000) || totalBits < kEpsilonHalf)
				continue;
			const float total = DirectX::PackedVector::XMConvertHalfToFloat(totalBits);
			const float melted = DirectX::PackedVector::XMConvertHalfToFloat(src[(size_t)col * 4 + 1]);
			const float depth = std::clamp(total - std::max(melted, 0.0f), 0.0f, 1.0f);
			const uint8_t quantised = depth >= kTrenchStoreEpsilon ?
			                              (uint8_t)std::lround(std::clamp(depth, 0.0f, 1.0f) * 255.0f) :
			                              (uint8_t)0;
			if (quantised == 0)
				continue;
			const float date = dates ? DirectX::PackedVector::XMConvertHalfToFloat(dates[col]) : 0.0f;
			const uint8_t freshByte = date > 0.0f ?
			                              (uint8_t)std::clamp((int)std::lround(255.0 - ((double)a_meta.ageingClock - date + sinceCopy) * kTrackFreshPerUnit), 0, 255) :
			                              (uint8_t)0;

			const float worldX = a_meta.origin.x + ((float)(a_meta.x0 + col) + 0.5f) * a_meta.texel;
			const int gx0 = (int)std::floor((worldX - half) / storeTexel);
			const int gx1 = (int)std::floor((worldX + half - 0.001f) / storeTexel);

			const int kx0 = FloorDiv(gx0, kTrenchTileDim);
			if (gx1 >= gx0 && gy1 >= gy0 && ky0 == ky1 && kx0 == FloorDiv(gx1, kTrenchTileDim)) {
				const TrenchTileKey key{ a_meta.worldspace, kx0, ky0 };
				TrenchTile* tile = tileFor(key);
				if (!tile)
					continue;
				const int lx0 = gx0 - key.x * kTrenchTileDim;
				const int lx1 = gx1 - key.x * kTrenchTileDim;
				uint64_t raised = 0;
				uint8_t* freshBytes = freshByte ? freshOf(*tile) : nullptr;
				for (int gy = gy0; gy <= gy1; gy++) {
					const size_t lineStart = (size_t)(gy - key.y * kTrenchTileDim) * kTrenchTileDim;
					uint8_t* line = tile->depth.data() + lineStart;
					for (int lx = lx0; lx <= lx1; lx++) {
						raised += quantised > line[lx];
						line[lx] = std::max(line[lx], quantised);
					}
					if (freshBytes)
						for (int lx = lx0; lx <= lx1; lx++)
							freshBytes[lineStart + lx] = std::max(freshBytes[lineStart + lx], freshByte);
				}
				cpuCensus.storeRaises[cpuCensus.trenchStorePath] += raised;
				continue;
			}
			for (int gy = gy0; gy <= gy1; gy++) {
				for (int gx = gx0; gx <= gx1; gx++) {
					const TrenchTileKey key{ a_meta.worldspace, FloorDiv(gx, kTrenchTileDim), FloorDiv(gy, kTrenchTileDim) };
					TrenchTile* tile = tileFor(key);
					if (!tile)
						continue;
					const int lx = gx - key.x * kTrenchTileDim;
					const int ly = gy - key.y * kTrenchTileDim;
					uint8_t& stored = tile->depth[(size_t)ly * kTrenchTileDim + lx];
					if (quantised > stored)
						cpuCensus.storeRaises[cpuCensus.trenchStorePath]++;
					stored = std::max(stored, quantised);
					if (freshByte) {
						uint8_t& storedFresh = freshOf(*tile)[(size_t)ly * kTrenchTileDim + lx];
						storedFresh = std::max(storedFresh, freshByte);
					}
				}
			}
		}
	}

	trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
	trenchSampleTile = nullptr;
}

void SnowDeformation::DrainTrenchBands()
{
	auto context = globals::d3d::context;
	if (!context)
		return;

	std::scoped_lock lock(trenchStoreMutex);

	for (int ring = 0; ring < 2; ring++) {
		for (int axis = 0; axis < 2; axis++) {
			if (!trenchBandValid[ring][axis])
				continue;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			D3D11_MAPPED_SUBRESOURCE clocks{};
			if (FAILED(context->Map(trenchBandStaging[ring][axis].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
				continue;
			if (FAILED(context->Map(trenchClockBandStaging[ring][axis].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &clocks))) {
				context->Unmap(trenchBandStaging[ring][axis].get(), 0);
				continue;
			}
			cpuCensus.trenchStorePath = CpuCensus::kStoreBand;
			StoreTrenchBand(trenchBandMeta[ring][axis], mapped, &clocks);
			context->Unmap(trenchClockBandStaging[ring][axis].get(), 0);
			context->Unmap(trenchBandStaging[ring][axis].get(), 0);
			trenchBandValid[ring][axis] = false;
		}
	}

	for (int ring = 0; ring < 2; ring++) {
		if (!trenchRollValid[ring])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		D3D11_MAPPED_SUBRESOURCE clocks{};
		if (FAILED(context->Map(trenchRollStaging[ring].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			continue;
		if (FAILED(context->Map(trenchClockRollStaging[ring].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &clocks))) {
			context->Unmap(trenchRollStaging[ring].get(), 0);
			continue;
		}
		cpuCensus.trenchStorePath = CpuCensus::kStoreRoll;
		StoreTrenchBand(trenchRollMeta[ring], mapped, &clocks);
		context->Unmap(trenchClockRollStaging[ring].get(), 0);
		context->Unmap(trenchRollStaging[ring].get(), 0);
		trenchRollValid[ring] = false;
	}
}

void SnowDeformation::FlushDepartingTrenches(DirectX::XMINT2 a_scroll, bool a_clearing)
{
	if (!settings.PersistTrenches || !trenchMapPrimed)
		return;

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;
	auto* previous = deformationTextures[0];
	if (!context || !device || !previous || !previous->resource)
		return;

	std::scoped_lock lock(trenchStoreMutex);

	const int dim = (int)deformMapDim;

	if (a_clearing || std::abs(a_scroll.x) >= kTrenchBandMax || std::abs(a_scroll.y) >= kTrenchBandMax) {
		D3D11_TEXTURE2D_DESC fullDesc{};
		fullDesc.Width = deformMapDim;
		fullDesc.Height = deformMapDim;
		fullDesc.MipLevels = 1;
		fullDesc.ArraySize = 1;
		fullDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		fullDesc.SampleDesc.Count = 1;
		fullDesc.Usage = D3D11_USAGE_STAGING;
		fullDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		winrt::com_ptr<ID3D11Texture2D> fullStaging;
		winrt::com_ptr<ID3D11Texture2D> fullClockStaging;
		D3D11_TEXTURE2D_DESC fullClockDesc = fullDesc;
		fullClockDesc.Format = DXGI_FORMAT_R16_FLOAT;
		if (FAILED(device->CreateTexture2D(&fullDesc, nullptr, fullStaging.put())))
			return;
		Util::SetResourceName(fullStaging.get(), "SnowDeformation::TrenchFullStaging");
		if (!trackClockTexture || FAILED(device->CreateTexture2D(&fullClockDesc, nullptr, fullClockStaging.put())))
			fullClockStaging = nullptr;
		else
			Util::SetResourceName(fullClockStaging.get(), "SnowDeformation::TrenchClockFullStaging");
		CopyLogicalBox(fullStaging.get(), previous->resource.get(), 0, 0, dim, dim, trenchMapPhysOrigin);
		if (fullClockStaging)
			CopyLogicalBox(fullClockStaging.get(), trackClockTexture->resource.get(), 0, 0, dim, dim, trenchMapPhysOrigin);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		D3D11_MAPPED_SUBRESOURCE clocks{};
		if (SUCCEEDED(context->Map(fullStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			const bool dated = fullClockStaging && SUCCEEDED(context->Map(fullClockStaging.get(), 0, D3D11_MAP_READ, 0, &clocks));
			cpuCensus.trenchStorePath = CpuCensus::kStoreJump;
			StoreTrenchBand({ trenchMapOrigin, trenchMapTexel, trenchMapWorldspace, 0, 0, dim, dim, TrackDateClock(), trenchAgeingTotal }, mapped,
				dated ? &clocks : nullptr);
			if (dated)
				context->Unmap(fullClockStaging.get(), 0);
			context->Unmap(fullStaging.get(), 0);
		}
		return;
	}

	if (a_scroll.x == 0 && a_scroll.y == 0)
		return;

	const int ring = trenchBandRing;
	trenchBandRing ^= 1;

	for (int axis = 0; axis < 2; axis++) {
		const int delta = axis == 0 ? a_scroll.x : a_scroll.y;
		if (delta == 0)
			continue;

		TrenchBandCopy meta{ trenchMapOrigin, trenchMapTexel, trenchMapWorldspace, 0, 0, dim, dim, TrackDateClock(), trenchAgeingTotal };
		const int start = delta > 0 ? 0 : dim + delta;
		if (axis == 0) {
			meta.x0 = start;
			meta.w = std::abs(delta);
		} else {
			meta.y0 = start;
			meta.h = std::abs(delta);
		}

		if (trenchBandValid[ring][axis]) {
			D3D11_MAPPED_SUBRESOURCE stale{};
			D3D11_MAPPED_SUBRESOURCE staleClocks{};
			if (SUCCEEDED(context->Map(trenchBandStaging[ring][axis].get(), 0, D3D11_MAP_READ, 0, &stale))) {
				const bool dated = SUCCEEDED(context->Map(trenchClockBandStaging[ring][axis].get(), 0, D3D11_MAP_READ, 0, &staleClocks));
				cpuCensus.trenchStorePath = CpuCensus::kStoreBandStale;
				StoreTrenchBand(trenchBandMeta[ring][axis], stale, dated ? &staleClocks : nullptr);
				if (dated)
					context->Unmap(trenchClockBandStaging[ring][axis].get(), 0);
				context->Unmap(trenchBandStaging[ring][axis].get(), 0);
			}
			trenchBandValid[ring][axis] = false;
		}

		CopyLogicalBox(trenchBandStaging[ring][axis].get(), previous->resource.get(),
			meta.x0, meta.y0, meta.w, meta.h, trenchMapPhysOrigin);
		if (trackClockTexture)
			CopyLogicalBox(trenchClockBandStaging[ring][axis].get(), trackClockTexture->resource.get(),
				meta.x0, meta.y0, meta.w, meta.h, trenchMapPhysOrigin);
		trenchBandMeta[ring][axis] = meta;
		trenchBandValid[ring][axis] = true;
	}
}

uint SnowDeformation::BuildTrenchInject(DirectX::XMINT2 a_scroll, bool a_clearing)
{
	LoadTraceScope _loadTrace(this, "TrenchStore: BuildTrenchInject");
	auto context = globals::d3d::context;
	if (!settings.PersistTrenches || !trenchInjectTexture || !context)
		return 0;

	std::scoped_lock lock(trenchStoreMutex);
	if (trenchTiles.empty())
		return 0;

	const int dim = (int)deformMapDim;
	const float texel = deformWorldSize / (float)deformMapDim;
	const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);

	ArrivalRect rects[2];
	const int rectCount = ComputeArrivalRects(a_scroll, a_clearing, rects);
	if (rectCount == 0)
		return 0;
	const bool full = rectCount == 1 && rects[0].w == dim && rects[0].h == dim;

	bool any = false;
	for (int i = 0; i < rectCount; i++) {
		const ArrivalRect& rect = rects[i];
		const size_t bytes = (size_t)rect.w * rect.h * 2;
		std::fill_n(trenchInjectScratch.begin(), bytes, (uint8_t)0);

		const float minX = windowOrigin.x + (float)rect.x0 * texel;
		const float maxX = windowOrigin.x + (float)(rect.x0 + rect.w) * texel;
		const float minY = windowOrigin.y + (float)rect.y0 * texel;
		const float maxY = windowOrigin.y + (float)(rect.y0 + rect.h) * texel;

		bool painted = false;
		int matched = 0;
		int reached = 0;
		for (auto& [key, tile] : trenchTiles) {
			if (key.worldspace != worldspace)
				continue;
			matched++;
			const float tileX = (float)key.x * kTrenchTileWorld;
			const float tileY = (float)key.y * kTrenchTileWorld;
			if (tileX + kTrenchTileWorld <= minX || tileX >= maxX || tileY + kTrenchTileWorld <= minY || tileY >= maxY)
				continue;
			reached++;

			tile.lastTouch = gameClock.lastHours;

			const int px0 = std::max(rect.x0, (int)std::ceil((tileX - windowOrigin.x) / texel - 0.5f) - 1);
			const int px1 = std::min(rect.x0 + rect.w - 1, (int)std::floor((tileX + kTrenchTileWorld - windowOrigin.x) / texel - 0.5f) + 1);
			const int py0 = std::max(rect.y0, (int)std::ceil((tileY - windowOrigin.y) / texel - 0.5f) - 1);
			const int py1 = std::min(rect.y0 + rect.h - 1, (int)std::floor((tileY + kTrenchTileWorld - windowOrigin.y) / texel - 0.5f) + 1);

			for (int py = py0; py <= py1; py++) {
				const float worldY = windowOrigin.y + ((float)py + 0.5f) * texel;
				uint8_t* row = trenchInjectScratch.data() + (size_t)(py - rect.y0) * rect.w * 2;
				for (int px = px0; px <= px1; px++) {
					const float worldX = windowOrigin.x + ((float)px + 0.5f) * texel;
					float fresh = 0.0f;
					const float depth = SampleTrenchStore(worldspace, worldX, worldY, &fresh);
					const uint8_t quantised = (uint8_t)std::lround(std::clamp(depth, 0.0f, 1.0f) * 255.0f);
					row[(px - rect.x0) * 2] = quantised;
					row[(px - rect.x0) * 2 + 1] = (uint8_t)std::lround(std::clamp(fresh, 0.0f, 1.0f) * 255.0f);
					painted = painted || quantised != 0;
				}
			}
		}

		if (full)
			logger::info("[SNOW DEFORMATION] trench inject rebuild: {} tiles in store, {} in this worldspace, {} inside the window, painted {}",
				trenchTiles.size(), matched, reached, painted ? "yes" : "no");

		any = any || painted;
		const D3D11_BOX box{ (UINT)rect.x0, (UINT)rect.y0, 0, (UINT)(rect.x0 + rect.w), (UINT)(rect.y0 + rect.h), 1 };
		context->UpdateSubresource(trenchInjectTexture.get(), 0, &box, trenchInjectScratch.data(), (UINT)rect.w * 2, 0);
	}

	return any ? 1u : 0u;
}
