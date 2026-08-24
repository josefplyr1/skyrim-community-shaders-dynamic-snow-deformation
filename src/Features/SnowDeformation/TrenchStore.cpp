#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/D3D.h"

#include <DirectXPackedVector.h>
#include <limits>

// Persistent trenches, Stage A. Design and staging in PERSISTENT-TRENCHES-PLAN.md;
// rationale in CODE-NOTES.md.
//
// DeformationUpdateCS reads the previous map at a scrolled offset and drops
// whatever falls outside it. This file is the memory that gives it back:
// departing texels are staged, read back a frame later, and folded into tiles
// on a fixed world grid; arriving texels are resampled out of those tiles into
// the inject texture the CS seeds from.

namespace
{
	// Floor division on the store's world grid; plain / truncates toward zero
	// and would fold the tiles either side of the origin onto each other.
	inline int FloorDiv(int a_value, int a_divisor)
	{
		return a_value >= 0 ? a_value / a_divisor : -(((-a_value) + a_divisor - 1) / a_divisor);
	}
}

bool SnowDeformation::CreateTrenchStoreResources()
{
	auto device = globals::d3d::device;
	if (!device)
		return false;

	trenchInjectTexture = nullptr;
	trenchInjectSRV = nullptr;
	for (int ring = 0; ring < 2; ring++)
		for (int axis = 0; axis < 2; axis++) {
			trenchBandStaging[ring][axis] = nullptr;
			trenchBandValid[ring][axis] = false;
		}

	D3D11_TEXTURE2D_DESC injectDesc{};
	injectDesc.Width = deformMapDim;
	injectDesc.Height = deformMapDim;
	injectDesc.MipLevels = 1;
	injectDesc.ArraySize = 1;
	injectDesc.Format = DXGI_FORMAT_R8_UNORM;
	injectDesc.SampleDesc.Count = 1;
	injectDesc.Usage = D3D11_USAGE_DEFAULT;
	injectDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&injectDesc, nullptr, trenchInjectTexture.put())))
		return false;
	Util::SetResourceName(trenchInjectTexture.get(), "SnowDeformation::TrenchInject");
	if (FAILED(device->CreateShaderResourceView(trenchInjectTexture.get(), nullptr, trenchInjectSRV.put())))
		return false;
	Util::SetResourceName(trenchInjectSRV.get(), "SnowDeformation::TrenchInject SRV");

	// Staged bands are the map's own format: the copy is a straight region
	// blit and the half-float unpack happens on the CPU, off the render path.
	D3D11_TEXTURE2D_DESC bandDesc{};
	bandDesc.MipLevels = 1;
	bandDesc.ArraySize = 1;
	bandDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	bandDesc.SampleDesc.Count = 1;
	bandDesc.Usage = D3D11_USAGE_STAGING;
	bandDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (int ring = 0; ring < 2; ring++) {
		for (int axis = 0; axis < 2; axis++) {
			bandDesc.Width = axis == 0 ? (UINT)kTrenchBandMax : deformMapDim;
			bandDesc.Height = axis == 0 ? deformMapDim : (UINT)kTrenchBandMax;
			if (FAILED(device->CreateTexture2D(&bandDesc, nullptr, trenchBandStaging[ring][axis].put())))
				return false;
			Util::SetResourceName(trenchBandStaging[ring][axis].get(), "SnowDeformation::TrenchBandStaging");
		}
	}

	// Rolling window mirror: same format, a fixed slice of full-width rows.
	bandDesc.Width = deformMapDim;
	bandDesc.Height = (UINT)kTrenchRollRows;
	for (int ring = 0; ring < 2; ring++) {
		trenchRollStaging[ring] = nullptr;
		trenchRollValid[ring] = false;
		if (FAILED(device->CreateTexture2D(&bandDesc, nullptr, trenchRollStaging[ring].put())))
			return false;
		Util::SetResourceName(trenchRollStaging[ring].get(), "SnowDeformation::TrenchRollStaging");
	}
	trenchRollRow = 0;

	trenchInjectScratch.assign((size_t)deformMapDim * deformMapDim, 0);
	trenchMapPrimed = false;
	return true;
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

	for (uint i = 0; i < a_data.StampCount && i < kMaxStamps; i++) {
		// Carves only. A melt stamp's depth is subtracted out of what the
		// store keeps anyway, so mirroring its rows early buys nothing.
		if (a_data.StampEnds[i].z > 0.5f)
			continue;

		// The capsule spans segStart to tip, so take both ends plus the radius.
		const float radius = a_data.Stamps[i].w;
		const float minY = std::min(a_data.Stamps[i].y, a_data.StampEnds[i].y) - radius;
		const float maxY = std::max(a_data.Stamps[i].y, a_data.StampEnds[i].y) + radius;

		// Against the CURRENT origin: these rows are consumed next frame, by
		// which time the map being copied is the one this frame wrote.
		int row0 = (int)std::floor((minY - a_data.WindowOrigin.y) / texel);
		int row1 = (int)std::floor((maxY - a_data.WindowOrigin.y) / texel);
		row0 = std::max(row0, 0);
		row1 = std::min(row1, dim - 1);
		for (int row = row0; row <= row1; row++)
			trenchDirtyRows[(size_t)row >> 5] |= 1u << (row & 31);
	}
}

void SnowDeformation::RollTrenchWindow()
{
	if (!settings.PersistTrenches || !trenchMapPrimed)
		return;

	auto context = globals::d3d::context;
	auto* live = deformationTextures[currentTexture];
	if (!context || !live || !live->resource)
		return;

	std::scoped_lock lock(trenchStoreMutex);

	const int ring = trenchRollRing;
	trenchRollRing ^= 1;

	// Same rule as the departing bands: never overwrite an undrained slot,
	// because losing a slice loses ground the store would then never relearn.
	if (trenchRollValid[ring]) {
		D3D11_MAPPED_SUBRESOURCE stale{};
		if (SUCCEEDED(context->Map(trenchRollStaging[ring].get(), 0, D3D11_MAP_READ, 0, &stale))) {
			StoreTrenchBand(trenchRollMeta[ring], stale);
			context->Unmap(trenchRollStaging[ring].get(), 0);
		}
		trenchRollValid[ring] = false;
	}

	const int dim = (int)deformMapDim;

	// Freshly dug rows first, then the sequential sweep. Without the priority
	// the newest metres of a trail are the likeliest to be missing from a save
	// - the reindeer that ran past just before Josef saved, and whose last few
	// metres came back gone.
	int start = -1;
	if (trenchDirtyRows.size() == (size_t)(dim + 31) / 32) {
		for (size_t word = 0; word < trenchDirtyRows.size() && start < 0; word++) {
			if (!trenchDirtyRows[word])
				continue;
			unsigned long bit = 0;
			_BitScanForward(&bit, trenchDirtyRows[word]);
			start = (int)(word * 32 + bit);
		}
	}
	if (start < 0)
		start = trenchRollRow;
	start = std::min(start, dim - 1);

	const int rows = std::min(kTrenchRollRows, dim - start);
	// Cleared whether they were dirty or swept: this slice is now mirrored.
	for (int row = start; row < start + rows; row++)
		if (trenchDirtyRows.size() == (size_t)(dim + 31) / 32)
			trenchDirtyRows[(size_t)row >> 5] &= ~(1u << (row & 31));

	const D3D11_BOX box{ 0, (UINT)start, 0, (UINT)dim, (UINT)(start + rows), 1 };
	context->CopySubresourceRegion(trenchRollStaging[ring].get(), 0, 0, 0, 0, live->resource.get(), 0, &box);

	// The window state the CONTENTS belong to, not the live values - the same
	// rule the departing flush follows.
	trenchRollMeta[ring] = { trenchMapOrigin, trenchMapTexel, trenchMapWorldspace, 0, start, dim, rows };
	trenchRollValid[ring] = true;

	// The sequential sweep advances only when it was the one that ran; a
	// priority slice must not let untouched ground go unmirrored for ever.
	if (start == trenchRollRow) {
		trenchRollRow += rows;
		if (trenchRollRow >= dim)
			trenchRollRow = 0;
	}
}

void SnowDeformation::TickTrenchClock()
{
	// The co-save callbacks land on the game thread and can rewrite the store
	// mid-frame.
	std::scoped_lock lock(trenchStoreMutex);

	// The reading itself is TickGameClock's, taken once for every consumer.
	// Backwards means a loaded save: the store belongs to a timeline that no
	// longer exists, and keeping it would hand a fresh game the last one's
	// trenches. Same rule the spell system's clocks follow.
	if (gameClock.reversed) {
		ClearTrenchStoreLocked();
		return;
	}
	const float elapsed = gameClock.elapsedHours;
	if (elapsed <= 0.0f)
		return;

	// Snowfall erases stored trenches at the LIVE REFILL'S OWN RATE rather than
	// a second number of its own: ground should behave the same whether or not
	// it is being looked at, and that also makes the away-decay follow the
	// refill sliders for free. The refill is per RENDER second, so it converts
	// through the timescale - RefillAmount is deltaTime/kBaseRefillTime scaled
	// by intensity and the multiplier, and one game hour is 3600/timescale
	// render seconds.
	// Elapsed hours telescope, so the calendar's float32 day quantisation
	// (~2.6-second steps late game) cancels instead of accumulating.
	const float refillIntensity = settings.RefillOnlyWhenSnowing ? snowfallIntensity : 1.0f;
	const float realSecondsPerGameHour = 3600.0f / std::max(gameClock.timescale, 1.0f);
	const float weather = realSecondsPerGameHour * refillIntensity *
	                      std::max(settings.RefillRateMultiplier, 0.0f) / kBaseRefillTime;
	// The floor underneath it: without one, a clear-weather modlist never
	// prunes and the store only grows.
	const float floorRate = settings.StoredTrenchFadeDays > 0.01f ?
	                            1.0f / (settings.StoredTrenchFadeDays * 24.0f) :
	                            0.0f;

	trenchDecayClock += elapsed * (weather + floorRate);
}

bool SnowDeformation::DecayTrenchTile(TrenchTile& a_tile)
{
	const float pending = trenchDecayClock - a_tile.clock;
	if (pending <= 0.0f)
		return true;

	// FLOORED, and the clock advances only by what was actually applied. The
	// sweep visits a tile every few frames, so rounding each visit's fraction
	// away and stamping the clock to now would discard the remainder every
	// time and the store would never decay at all.
	const int drop = (int)std::floor(pending * 255.0f);
	if (drop <= 0)
		return true;
	a_tile.clock += (float)drop / 255.0f;

	bool alive = false;
	for (auto& texel : a_tile.depth) {
		texel = (uint8_t)std::max(0, (int)texel - drop);
		alive = alive || texel != 0;
	}
	return alive;
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
		// Cycle boundary: publish what the last pass measured, then spend the
		// budget while every tile's encoded size is freshly known.
		trenchStatNonZero = trenchAccumNonZero;
		trenchStatThin = trenchAccumThin;
		trenchStatSweptTiles = trenchAccumTiles;
		// Ground going away is worth a line. Silence here is exactly what hid a
		// store that was deleting itself.
		if (trenchAccumErased)
			logger::info("[SNOW DEFORMATION] trench store: refill erased {} tiles this cycle, {} remain",
				trenchAccumErased, trenchTiles.size());
		trenchAccumNonZero = trenchAccumThin = trenchAccumTiles = trenchAccumErased = 0;
		trenchEncodedTotal = 0;
		for (const auto& [key, tile] : trenchTiles)
			trenchEncodedTotal += tile.encodedBytes + kTrenchTileHeaderBytes;
		EnforceTrenchBudget();
		trenchStatTiles = trenchTiles.size();

		trenchSweepQueue.reserve(trenchTiles.size());
		for (const auto& [key, tile] : trenchTiles)
			trenchSweepQueue.push_back(key);
	}

	// A slice per frame. The store cycles in well under a second at any size
	// worth sweeping, and no frame pays for the whole of it.
	constexpr int kPerFrame = 8;
	constexpr size_t texels = (size_t)kTrenchTileDim * kTrenchTileDim;
	for (int i = 0; i < kPerFrame && !trenchSweepQueue.empty(); i++) {
		const TrenchTileKey key = trenchSweepQueue.back();
		trenchSweepQueue.pop_back();
		auto it = trenchTiles.find(key);
		if (it == trenchTiles.end())
			continue;

		// Refill is the reaper: a tile it has taken back to bare ground is
		// deleted, so the store self-prunes anywhere weather happens. This is
		// the ONLY place a tile dies of emptiness now - writes raise only, so
		// a blank map can no longer delete ground the store knows is dug.
		if (!DecayTrenchTile(it->second)) {
			trenchTiles.erase(it);
			trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
			trenchSampleTile = nullptr;
			trenchAccumErased++;
			continue;
		}

		// One pass over the tile counts BOTH figures: how full it is, and
		// exactly what it will cost to write. The encoded size has to be exact
		// rather than estimated, or the budget bounds a number that is not the
		// one landing in the save.
		const auto& bytes = it->second.depth;
		size_t nonZero = 0;
		size_t pairs = 0;
		size_t run = 0;
		uint8_t previous = bytes[0];
		for (size_t b = 0; b < bytes.size(); b++) {
			if (bytes[b] != 0)
				nonZero++;
			if (bytes[b] == previous) {
				run++;
			} else {
				// A run longer than 255 splits across pairs; the count field
				// is one byte.
				pairs += (run + 254) / 255;
				previous = bytes[b];
				run = 1;
			}
		}
		pairs += (run + 254) / 255;

		const uint32_t encoded = (uint32_t)(pairs * kTrenchRLEPairBytes);
		// A tile the encoder would make bigger is kept raw, so a pathological
		// pattern costs its raw size and never more.
		it->second.encodedBytes = std::min(encoded, (uint32_t)bytes.size());

		trenchAccumNonZero += nonZero;
		trenchAccumTiles++;
		if (nonZero * 20 < texels)
			trenchAccumThin++;
	}
}

void SnowDeformation::EnforceTrenchBudget()
{
	// Floored low enough that the cap can actually be REACHED on a normal
	// store. At ~354 bytes a tile the 1 MB default holds nearly 3000 of them,
	// perhaps ten times a busy session, so without a settable floor this whole
	// path would never run in play and would never be tested either.
	const size_t budget = (size_t)(std::max(settings.TrenchMemoryMB, 0.01f) * 1024.0f * 1024.0f);
	if (trenchEncodedTotal <= budget) {
		// Fits, so nothing is out of reach and the writer is unconstrained.
		trenchKeepRadiusSq = std::numeric_limits<float>::max();
		return;
	}

	// FURTHEST ground goes first, not least-recently-touched.
	//
	// Recency was the original rule and it degenerated: the rolling mirror
	// rewrites every tile in the window every few seconds, so it touches
	// nearly everything and the timestamps stop discriminating. Eviction then
	// picked arbitrarily, and the ground the player was standing on was as
	// likely to go as an NPC trail on the far side of the hold - which is
	// exactly what Josef saw when a store larger than the budget was culled on
	// load and his trenches were among the casualties.
	//
	// Distance from the window centre cannot degenerate that way, and it
	// matches what the feature is for: snow is only ever SEEN nearby, so when
	// the store cannot hold everything the far half is what to lose. Another
	// worldspace outranks any distance - Solstheim's trenches are not competing
	// with the Rift's.
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
	// Furthest first.
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
		// Furthest-first, so the last one dropped is the nearest that had to
		// go. That distance becomes the frontier the writer must respect, or
		// the mirror simply recreates everything just culled.
		nearestEvicted = rank;
	}
	trenchKeepRadiusSq = evicted ? nearestEvicted : std::numeric_limits<float>::max();

	if (evicted) {
		trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
		trenchSampleTile = nullptr;
		// INFO, not debug: this should be a rare event, and if it is happening
		// in ordinary play that is the finding, not noise.
		// The keep radius is the number that says whether the store is
		// SETTLING on the near field or thrashing: it should stabilise, and
		// the survivors should sit inside it.
		logger::info("[SNOW DEFORMATION] trench store over budget: evicted {} of {} tiles, {} remain, {} KB encoded, keep radius {:.0f} units",
			evicted, trenchEvictScratch.size(), trenchTiles.size(), trenchEncodedTotal / 1024,
			std::sqrt(trenchKeepRadiusSq));
	}
}

void SnowDeformation::ClearTrenchStore()
{
	std::scoped_lock lock(trenchStoreMutex);
	ClearTrenchStoreLocked();
}

void SnowDeformation::ClearTrenchStoreLocked()
{
	trenchTiles.clear();
	trenchStatTiles = 0;
	trenchEncodedTotal = 0;
	trenchSweepQueue.clear();
	trenchStatNonZero = trenchStatThin = trenchStatSweptTiles = 0;
	trenchAccumNonZero = trenchAccumThin = trenchAccumTiles = 0;
	trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
	trenchSampleTile = nullptr;
	// The staged bands and slices describe a map that is about to be wiped;
	// folding them in afterwards would put the trenches straight back.
	for (int ring = 0; ring < 2; ring++) {
		for (int axis = 0; axis < 2; axis++)
			trenchBandValid[ring][axis] = false;
		trenchRollValid[ring] = false;
	}
	trenchMapPrimed = false;
}

float SnowDeformation::SampleTrenchStore(uint32_t a_worldspace, float a_worldX, float a_worldY)
{
	constexpr float storeTexel = kTrenchTileWorld / (float)kTrenchTileDim;

	// Continuous store-texel coordinates, offset by half a texel so the lerp
	// runs between texel CENTRES.
	const float fx = a_worldX / storeTexel - 0.5f;
	const float fy = a_worldY / storeTexel - 0.5f;
	const int gx = (int)std::floor(fx);
	const int gy = (int)std::floor(fy);
	const float tx = fx - (float)gx;
	const float ty = fy - (float)gy;

	float corner[4] = {};
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
		// The tile's own pending decay applied at READ time, so a sweep that
		// has not reached this tile yet cannot hand back a stale depth. The
		// sweep exists to reclaim memory, never to keep the store correct.
		corner[i] = std::max(0.0f,
			trenchSampleTile->depth[(size_t)ly * kTrenchTileDim + lx] * (1.0f / 255.0f) -
				(trenchDecayClock - trenchSampleTile->clock));
	}

	return std::lerp(std::lerp(corner[0], corner[1], tx), std::lerp(corner[2], corner[3], tx), ty);
}

void SnowDeformation::StoreTrenchBand(const TrenchBandCopy& a_meta, const D3D11_MAPPED_SUBRESOURCE& a_mapped)
{
	constexpr float storeTexel = kTrenchTileWorld / (float)kTrenchTileDim;
	const float half = a_meta.texel * 0.5f;

	// Centre of the window these contents belong to, for the keep-radius gate.
	const float centreX = a_meta.origin.x + a_meta.texel * (float)deformMapDim * 0.5f;
	const float centreY = a_meta.origin.y + a_meta.texel * (float)deformMapDim * 0.5f;

	// No emptiness check here any more. Writes raise only, so a batch cannot
	// empty a tile, and erasure belongs to the sweep, which decides it from the
	// tile's actual content rather than from what one slice of map happened to
	// say. That also gives it a log line; this prune had none, which is why it
	// took a code review rather than a glance at the log to find.

	// One-entry tile cache. The jump path folds in the whole 2048 square, and a
	// row crosses a tile only every 128 store texels, so without this the inner
	// loop is millions of hash lookups at a loading screen.
	TrenchTileKey cachedKey{ 0, INT32_MIN, INT32_MIN };
	TrenchTile* cachedTile = nullptr;

	for (int row = 0; row < a_meta.h; row++) {
		const auto* src = reinterpret_cast<const DirectX::PackedVector::HALF*>(
			static_cast<const uint8_t*>(a_mapped.pData) + (size_t)row * a_mapped.RowPitch);
		const float worldY = a_meta.origin.y + ((float)(a_meta.y0 + row) + 0.5f) * a_meta.texel;
		const int gy0 = (int)std::floor((worldY - half) / storeTexel);
		const int gy1 = (int)std::floor((worldY + half - 0.001f) / storeTexel);

		for (int col = 0; col < a_meta.w; col++) {
			// DISPLACED depth, not total - the shell's own definition
			// (SampleDisplacedFast in SnowShell.hlsl), reused verbatim.
			//
			// The store keeps depth alone, so a texel's melt classification
			// cannot come back with it. Storing a melt basin's total depth
			// would re-inject it as a DUG one: it would grow the berm melted
			// snow never earns, start casting the shadow melt pits are exempt
			// from, and refill at trench speed instead of MeltPersistence.
			// Subtracting the melted part instead means a spell basin is
			// simply not remembered - which is honest, since what made it a
			// basin is not remembered either - while a boot print through one
			// still stores its own displacement.
			//
			// Scorch (negative y) is NOT subtracted: it was displaced and
			// keeps its berm, so its depth is a real dent worth remembering.
			// Campfire melt never reaches here at all - that comes from the
			// exclusion field, rederived each frame from live fire positions.
			const float total = DirectX::PackedVector::XMConvertHalfToFloat(src[(size_t)col * 4]);
			const float melted = DirectX::PackedVector::XMConvertHalfToFloat(src[(size_t)col * 4 + 1]);
			const float depth = std::clamp(total - std::max(melted, 0.0f), 0.0f, 1.0f);
			// Written, not maxed: the store is the map's record, so snow the
			// refill has put back must be able to shallow a stored trench.
			const uint8_t quantised = depth >= kTrenchStoreEpsilon ?
			                              (uint8_t)std::lround(std::clamp(depth, 0.0f, 1.0f) * 255.0f) :
			                              (uint8_t)0;

			const float worldX = a_meta.origin.x + ((float)(a_meta.x0 + col) + 0.5f) * a_meta.texel;
			const int gx0 = (int)std::floor((worldX - half) / storeTexel);
			const int gx1 = (int)std::floor((worldX + half - 0.001f) / storeTexel);

			// A map texel is 2.0-13.7 units against the store's 4, so it can
			// cover several store texels or share one. Splat across every
			// texel it covers; where several map texels share one, the last
			// wins, which costs sub-texel detail and nothing else.
			for (int gy = gy0; gy <= gy1; gy++) {
				for (int gx = gx0; gx <= gx1; gx++) {
					const TrenchTileKey key{ a_meta.worldspace, FloorDiv(gx, kTrenchTileDim), FloorDiv(gy, kTrenchTileDim) };
					// The MISS is cached too, not just the hit. Most of the map
					// is untrodden, and a cache that only remembers tiles that
					// exist does a fresh hash lookup for every texel of empty
					// ground - which the rolling mirror now walks tens of
					// thousands of times a frame.
					if (key != cachedKey) {
						auto it = trenchTiles.find(key);
						cachedKey = key;
						cachedTile = it == trenchTiles.end() ? nullptr : &it->second;
						if (cachedTile) {
							// Brought up to date before fresh texels mix in, or
							// the batch's new depths would sit beside stale ones
							// under a single clock and decay twice.
							DecayTrenchTile(*cachedTile);
							// Written to, so it is recent ground whatever the
							// LRU thought a moment ago.
							cachedTile->lastTouch = gameClock.lastHours;
						}
					}
					if (!cachedTile) {
						// Untrodden ground never allocates: this is what keeps
						// the store sparse, and most of the world is.
						if (quantised == 0)
							continue;
						// Nor does ground the cap has already pushed out of
						// reach. Creating it would only hand the next eviction
						// the same tile back, and the store would thrash
						// instead of settling on the near field.
						if (trenchKeepRadiusSq < std::numeric_limits<float>::max()) {
							const float dx = ((float)key.x + 0.5f) * kTrenchTileWorld - centreX;
							const float dy = ((float)key.y + 0.5f) * kTrenchTileWorld - centreY;
							if (dx * dx + dy * dy >= trenchKeepRadiusSq)
								continue;
						}
						TrenchTile fresh;
						fresh.depth.assign((size_t)kTrenchTileDim * kTrenchTileDim, 0);
						fresh.clock = trenchDecayClock;
						fresh.lastTouch = gameClock.lastHours;
						// Re-seated after the insert, which may have rehashed.
						cachedTile = &trenchTiles.emplace(key, std::move(fresh)).first->second;
					}
					const int lx = gx - key.x * kTrenchTileDim;
					const int ly = gy - key.y * kTrenchTileDim;
					uint8_t& stored = cachedTile->depth[(size_t)ly * kTrenchTileDim + lx];
					// RAISE ONLY. The map may teach the store what has been dug;
					// it may not teach it that snow is gone.
					//
					// The map is routinely emptier than the store for reasons
					// that have nothing to do with the weather: a ClearMap frame
					// wipes it and only the inject repopulates it, an inject
					// that reaches nothing leaves it entirely blank, and every
					// clear has frames where the map is bare ground the store
					// knows is dug. Letting a write lower the store made all of
					// those destructive, and the tile then vanished through the
					// empty-tile prune without a single line in the log.
					//
					// Removal is decay's job and always was: Stage B derives the
					// store's rate from RefillAmount itself, precisely so ground
					// behaves the same whether or not it is being looked at. In
					// window and out of window now follow one rule instead of
					// two that disagree whenever the map is mid-rebuild.
					stored = std::max(stored, quantised);
				}
			}
		}
	}

	// Inserting rehashes; the sampler's cached pointer would dangle.
	trenchSampleKey = { 0, INT32_MIN, INT32_MIN };
	trenchSampleTile = nullptr;
}

void SnowDeformation::DrainTrenchBands()
{
	auto context = globals::d3d::context;
	if (!context)
		return;

	// StoreTrenchBand writes the store, which the game thread's save and load
	// callbacks also touch.
	std::scoped_lock lock(trenchStoreMutex);

	for (int ring = 0; ring < 2; ring++) {
		for (int axis = 0; axis < 2; axis++) {
			if (!trenchBandValid[ring][axis])
				continue;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			// DO_NOT_WAIT: a copy issued this frame is still in flight and
			// simply waits another frame rather than stalling the render thread.
			if (FAILED(context->Map(trenchBandStaging[ring][axis].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
				continue;
			StoreTrenchBand(trenchBandMeta[ring][axis], mapped);
			context->Unmap(trenchBandStaging[ring][axis].get(), 0);
			trenchBandValid[ring][axis] = false;
		}
	}

	for (int ring = 0; ring < 2; ring++) {
		if (!trenchRollValid[ring])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(trenchRollStaging[ring].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
			continue;
		StoreTrenchBand(trenchRollMeta[ring], mapped);
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
	auto* previous = deformationTextures[currentTexture];
	if (!context || !device || !previous || !previous->resource)
		return;

	// The jump path folds straight into the store, and the blocking drain below
	// does too.
	std::scoped_lock lock(trenchStoreMutex);

	const int dim = (int)deformMapDim;

	// A clear, a cell load, a coc or a fast travel all abandon the whole
	// window at once; the band ring cannot hold that, so take the map entire.
	// Every one of these lands on a load boundary or a menu press, which is
	// the one place a blocking map costs nothing anyone will see.
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

		// Created and dropped around the copy: 32 MB is too much to hold for a
		// path that runs at a loading screen.
		winrt::com_ptr<ID3D11Texture2D> fullStaging;
		if (FAILED(device->CreateTexture2D(&fullDesc, nullptr, fullStaging.put())))
			return;
		Util::SetResourceName(fullStaging.get(), "SnowDeformation::TrenchFullStaging");
		context->CopyResource(fullStaging.get(), previous->resource.get());

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(fullStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
			StoreTrenchBand({ trenchMapOrigin, trenchMapTexel, trenchMapWorldspace, 0, 0, dim, dim }, mapped);
			context->Unmap(fullStaging.get(), 0);
		}
		return;
	}

	if (a_scroll.x == 0 && a_scroll.y == 0)
		return;

	// Destination texel p reads previous texel p + scroll, so the previous
	// texels nothing reaches any more are the leading band on each axis.
	const int ring = trenchBandRing;
	trenchBandRing ^= 1;

	for (int axis = 0; axis < 2; axis++) {
		const int delta = axis == 0 ? a_scroll.x : a_scroll.y;
		if (delta == 0)
			continue;

		TrenchBandCopy meta{ trenchMapOrigin, trenchMapTexel, trenchMapWorldspace, 0, 0, dim, dim };
		const int start = delta > 0 ? 0 : dim + delta;
		if (axis == 0) {
			meta.x0 = start;
			meta.w = std::abs(delta);
		} else {
			meta.y0 = start;
			meta.h = std::abs(delta);
		}

		// The slot still holds an undrained copy: block rather than overwrite
		// it. Losing a band loses trenches; a rare stall does not.
		if (trenchBandValid[ring][axis]) {
			D3D11_MAPPED_SUBRESOURCE stale{};
			if (SUCCEEDED(context->Map(trenchBandStaging[ring][axis].get(), 0, D3D11_MAP_READ, 0, &stale))) {
				StoreTrenchBand(trenchBandMeta[ring][axis], stale);
				context->Unmap(trenchBandStaging[ring][axis].get(), 0);
			}
			trenchBandValid[ring][axis] = false;
		}

		const D3D11_BOX box{ (UINT)meta.x0, (UINT)meta.y0, 0, (UINT)(meta.x0 + meta.w), (UINT)(meta.y0 + meta.h), 1 };
		context->CopySubresourceRegion(trenchBandStaging[ring][axis].get(), 0, 0, 0, 0, previous->resource.get(), 0, &box);
		trenchBandMeta[ring][axis] = meta;
		trenchBandValid[ring][axis] = true;
	}
}

uint SnowDeformation::BuildTrenchInject(DirectX::XMINT2 a_scroll, bool a_clearing)
{
	auto context = globals::d3d::context;
	if (!settings.PersistTrenches || !trenchInjectTexture || !context)
		return 0;

	// Taken before the emptiness test, not after: a load on the game thread can
	// fill the store between the two. Held across the whole tile walk rather
	// than per sample, because SampleTrenchStore caches a pointer INTO the
	// store that the same load would invalidate.
	std::scoped_lock lock(trenchStoreMutex);
	if (trenchTiles.empty())
		return 0;

	const int dim = (int)deformMapDim;
	const float texel = deformWorldSize / (float)deformMapDim;
	const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);

	// Destination texel p reads previous p + scroll; the ones that miss are
	// the trailing band on each axis. A clear misses everywhere.
	struct Rect
	{
		int x0, y0, w, h;
	};
	Rect rects[2];
	int rectCount = 0;
	const bool full = a_clearing || std::abs(a_scroll.x) >= dim || std::abs(a_scroll.y) >= dim;
	if (full) {
		rects[rectCount++] = { 0, 0, dim, dim };
	} else {
		if (a_scroll.x != 0)
			rects[rectCount++] = { a_scroll.x > 0 ? dim - a_scroll.x : 0, 0, std::abs(a_scroll.x), dim };
		if (a_scroll.y != 0)
			rects[rectCount++] = { 0, a_scroll.y > 0 ? dim - a_scroll.y : 0, dim, std::abs(a_scroll.y) };
	}
	if (rectCount == 0)
		return 0;

	bool any = false;
	for (int i = 0; i < rectCount; i++) {
		const Rect& rect = rects[i];
		const size_t bytes = (size_t)rect.w * rect.h;
		std::fill_n(trenchInjectScratch.begin(), bytes, (uint8_t)0);

		const float minX = windowOrigin.x + (float)rect.x0 * texel;
		const float maxX = windowOrigin.x + (float)(rect.x0 + rect.w) * texel;
		const float minY = windowOrigin.y + (float)rect.y0 * texel;
		const float maxY = windowOrigin.y + (float)(rect.y0 + rect.h) * texel;

		// Driven by the tiles that exist rather than by the texels that need
		// filling: the store is sparse, so a full rebuild costs the ground you
		// have walked and not the 4 M texels of the window.
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

			// Standing inside the window counts as use: the LRU should forget
			// where you have not been, not where you happen not to be digging.
			tile.lastTouch = gameClock.lastHours;

			// Texel centres inside this tile's world span, widened by one so a
			// texel just outside still picks up the tile's edge through the
			// bilinear tap instead of notching the trench at the seam.
			const int px0 = std::max(rect.x0, (int)std::ceil((tileX - windowOrigin.x) / texel - 0.5f) - 1);
			const int px1 = std::min(rect.x0 + rect.w - 1, (int)std::floor((tileX + kTrenchTileWorld - windowOrigin.x) / texel - 0.5f) + 1);
			const int py0 = std::max(rect.y0, (int)std::ceil((tileY - windowOrigin.y) / texel - 0.5f) - 1);
			const int py1 = std::min(rect.y0 + rect.h - 1, (int)std::floor((tileY + kTrenchTileWorld - windowOrigin.y) / texel - 0.5f) + 1);

			for (int py = py0; py <= py1; py++) {
				const float worldY = windowOrigin.y + ((float)py + 0.5f) * texel;
				uint8_t* row = trenchInjectScratch.data() + (size_t)(py - rect.y0) * rect.w;
				for (int px = px0; px <= px1; px++) {
					const float worldX = windowOrigin.x + ((float)px + 0.5f) * texel;
					// Bilinear, not nearest: the store is finer than the map at
					// every range setting above 4 units per texel, and dropping
					// texels thins a trail on the way back in.
					const float depth = SampleTrenchStore(worldspace, worldX, worldY);
					const uint8_t quantised = (uint8_t)std::lround(std::clamp(depth, 0.0f, 1.0f) * 255.0f);
					row[px - rect.x0] = quantised;
					painted = painted || quantised != 0;
				}
			}
		}

		// Only on a full rebuild, so this costs nothing per frame. The three
		// counts separate the failures that otherwise look identical: a store
		// keyed to another worldspace, tiles keyed outside the window, and a
		// window that simply had nothing to put back.
		if (full)
			logger::info("[SNOW DEFORMATION] trench inject rebuild: {} tiles in store, {} in this worldspace, {} inside the window, painted {}",
				trenchTiles.size(), matched, reached, painted ? "yes" : "no");

		any = any || painted;
		// Uploaded even when nothing was painted: with the flag on, an
		// unpainted rect would otherwise hand the CS whatever an older band
		// left in the texture. Zeroes are the correct answer for bare ground.
		const D3D11_BOX box{ (UINT)rect.x0, (UINT)rect.y0, 0, (UINT)(rect.x0 + rect.w), (UINT)(rect.y0 + rect.h), 1 };
		context->UpdateSubresource(trenchInjectTexture.get(), 0, &box, trenchInjectScratch.data(), (UINT)rect.w, 0);
	}

	return any ? 1u : 0u;
}
