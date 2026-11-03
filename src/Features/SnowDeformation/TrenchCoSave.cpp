#include "Features/SnowDeformation.h"

#include "Features/SnowDeformation/CoSave.h"
#include "Globals.h"

#include <unordered_set>

void SnowDeformation::EncodeTrenchTile(const std::vector<uint8_t>& a_bytes, std::vector<uint8_t>& o_bytes) const
{
	const auto& depth = a_bytes;
	o_bytes.clear();
	o_bytes.reserve(depth.size() / 4);

	size_t run = 0;
	uint8_t previous = depth[0];
	auto emit = [&]() {
		while (run > 0) {
			const uint8_t take = (uint8_t)std::min<size_t>(run, 255);
			o_bytes.push_back(take);
			o_bytes.push_back(previous);
			run -= take;
		}
	};
	for (size_t b = 0; b < depth.size(); b++) {
		if (depth[b] == previous) {
			run++;
		} else {
			emit();
			previous = depth[b];
			run = 1;
		}
	}
	emit();

	if (o_bytes.size() >= depth.size())
		o_bytes.assign(depth.begin(), depth.end());
}

bool SnowDeformation::DecodeTrenchTile(const uint8_t* a_bytes, uint32_t a_length, std::vector<uint8_t>& o_depth) const
{
	constexpr size_t texels = (size_t)kTrenchTileDim * kTrenchTileDim;
	o_depth.assign(texels, 0);

	if (a_length == texels) {
		std::memcpy(o_depth.data(), a_bytes, texels);
		return true;
	}
	if ((a_length & 1u) != 0)
		return false;

	size_t out = 0;
	for (uint32_t i = 0; i + 1 < a_length; i += 2) {
		const uint8_t count = a_bytes[i];
		const uint8_t value = a_bytes[i + 1];
		if (count == 0 || out + count > texels)
			return false;
		std::fill_n(o_depth.begin() + out, count, value);
		out += count;
	}
	return out == texels;
}

void SnowDeformation::SaveTrenchStore(const SKSE::SerializationInterface* a_intfc)
{
	std::scoped_lock lock(trenchStoreMutex);

	if (!a_intfc->OpenRecord(kTrenchRecord, kTrenchRecordVersion)) {
		logger::error("[SNOW DEFORMATION] Could not open the trench co-save record");
		return;
	}

	const uint32_t tileCount = settings.PersistTrenches ? (uint32_t)trenchTiles.size() : 0u;
	const uint16_t tileDim = (uint16_t)kTrenchTileDim;
	const uint16_t pad = 0;
	const float tileWorld = kTrenchTileWorld;
	a_intfc->WriteRecordData(&tileCount, sizeof(tileCount));
	a_intfc->WriteRecordData(&tileDim, sizeof(tileDim));
	a_intfc->WriteRecordData(&pad, sizeof(pad));
	a_intfc->WriteRecordData(&tileWorld, sizeof(tileWorld));
	if (tileCount == 0)
		return;

	std::vector<uint8_t> payload;
	std::vector<uint8_t> freshPayload;
	size_t written = 0;
	for (const auto& [key, tile] : trenchTiles) {
		EncodeTrenchTile(tile.depth, payload);
		const uint32_t payloadBytes = (uint32_t)payload.size();
		const float clockRel = tile.clock - trenchDecayClock;
		const float touchRel = tile.lastTouch - gameClockHours.load(std::memory_order_relaxed);
		freshPayload.clear();
		if (!tile.fresh.empty())
			EncodeTrenchTile(tile.fresh, freshPayload);
		const uint32_t freshBytes = (uint32_t)freshPayload.size();
		const float freshRel = (float)std::min(tile.freshClock - trenchAgeingTotal, 0.0);

		a_intfc->WriteRecordData(&key.worldspace, sizeof(key.worldspace));
		a_intfc->WriteRecordData(&key.x, sizeof(key.x));
		a_intfc->WriteRecordData(&key.y, sizeof(key.y));
		a_intfc->WriteRecordData(&clockRel, sizeof(clockRel));
		a_intfc->WriteRecordData(&touchRel, sizeof(touchRel));
		a_intfc->WriteRecordData(&payloadBytes, sizeof(payloadBytes));
		a_intfc->WriteRecordData(payload.data(), payloadBytes);
		a_intfc->WriteRecordData(&freshRel, sizeof(freshRel));
		a_intfc->WriteRecordData(&freshBytes, sizeof(freshBytes));
		if (freshBytes)
			a_intfc->WriteRecordData(freshPayload.data(), freshBytes);
		written += payloadBytes + freshBytes + kTrenchTileHeaderBytes;
	}

	if (trenchEncodedTotal && written > trenchEncodedTotal + trenchEncodedTotal / 8)
		logger::warn("[SNOW DEFORMATION] trench co-save wrote {} bytes against a measured {}; budget accounting has drifted",
			written, trenchEncodedTotal);
	logger::info("[SNOW DEFORMATION] trench co-save: {} tiles, {} KB", tileCount, written / 1024);
}

void SnowDeformation::LoadTrenchStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t)
{
	LoadTraceBegin("co-save trench store");
	LoadTraceScope _loadTrace(this, "CoSave: LoadTrenchStore");
	if (a_version != kTrenchRecordVersion && a_version != 1) {
		logger::warn("[SNOW DEFORMATION] trench co-save is version {}, this build reads 1-{}; stored trenches dropped",
			a_version, kTrenchRecordVersion);
		return;
	}
	const bool dated = a_version >= 2;

	uint32_t tileCount = 0;
	uint16_t tileDim = 0;
	uint16_t pad = 0;
	float tileWorld = 0.0f;
	if (a_intfc->ReadRecordData(&tileCount, sizeof(tileCount)) != sizeof(tileCount) ||
		a_intfc->ReadRecordData(&tileDim, sizeof(tileDim)) != sizeof(tileDim) ||
		a_intfc->ReadRecordData(&pad, sizeof(pad)) != sizeof(pad) ||
		a_intfc->ReadRecordData(&tileWorld, sizeof(tileWorld)) != sizeof(tileWorld)) {
		logger::warn("[SNOW DEFORMATION] trench co-save header would not read; nothing restored");
		return;
	}
	logger::info("[SNOW DEFORMATION] trench co-save header: {} tiles, {} texels over {} units",
		tileCount, tileDim, tileWorld);

	if (tileDim != (uint16_t)kTrenchTileDim || std::abs(tileWorld - kTrenchTileWorld) > 0.5f) {
		logger::warn("[SNOW DEFORMATION] trench co-save was written at {} texels over {} units, this build uses {} over {}; dropped",
			tileDim, tileWorld, kTrenchTileDim, kTrenchTileWorld);
		return;
	}

	std::scoped_lock lock(trenchStoreMutex);
	ClearTrenchStoreLocked("the co-save load");

	trenchDecayClock = 0.0f;
	gameClockUnarm.store(true, std::memory_order_release);

	std::vector<uint8_t> payload;
	uint32_t restored = 0;
	for (uint32_t i = 0; i < tileCount; i++) {
		TrenchTileKey key{};
		float clockRel = 0.0f;
		float touchRel = 0.0f;
		uint32_t payloadBytes = 0;
		if (a_intfc->ReadRecordData(&key.worldspace, sizeof(key.worldspace)) != sizeof(key.worldspace) ||
			a_intfc->ReadRecordData(&key.x, sizeof(key.x)) != sizeof(key.x) ||
			a_intfc->ReadRecordData(&key.y, sizeof(key.y)) != sizeof(key.y) ||
			a_intfc->ReadRecordData(&clockRel, sizeof(clockRel)) != sizeof(clockRel) ||
			a_intfc->ReadRecordData(&touchRel, sizeof(touchRel)) != sizeof(touchRel) ||
			a_intfc->ReadRecordData(&payloadBytes, sizeof(payloadBytes)) != sizeof(payloadBytes))
			break;

		if (payloadBytes == 0 || payloadBytes > (uint32_t)kTrenchTileDim * kTrenchTileDim) {
			logger::warn("[SNOW DEFORMATION] trench co-save tile {} claims {} payload bytes; the rest is dropped", i, payloadBytes);
			break;
		}
		payload.resize(payloadBytes);
		if (a_intfc->ReadRecordData(payload.data(), payloadBytes) != payloadBytes)
			break;

		TrenchTile tile;
		if (!DecodeTrenchTile(payload.data(), payloadBytes, tile.depth)) {
			logger::warn("[SNOW DEFORMATION] trench co-save tile {} would not decode; the rest is dropped", i);
			break;
		}
		tile.freshClock = trenchAgeingTotal;
		if (dated) {
			float freshRel = 0.0f;
			uint32_t freshBytes = 0;
			if (a_intfc->ReadRecordData(&freshRel, sizeof(freshRel)) != sizeof(freshRel) ||
				a_intfc->ReadRecordData(&freshBytes, sizeof(freshBytes)) != sizeof(freshBytes))
				break;
			if (freshBytes > (uint32_t)kTrenchTileDim * kTrenchTileDim) {
				logger::warn("[SNOW DEFORMATION] trench co-save tile {} claims {} freshness bytes; the rest is dropped", i, freshBytes);
				break;
			}
			if (freshBytes) {
				payload.resize(freshBytes);
				if (a_intfc->ReadRecordData(payload.data(), freshBytes) != freshBytes)
					break;
				if (!DecodeTrenchTile(payload.data(), freshBytes, tile.fresh)) {
					logger::warn("[SNOW DEFORMATION] trench co-save tile {} freshness would not decode; the rest is dropped", i);
					break;
				}
				tile.freshClock = trenchAgeingTotal + std::min((double)freshRel, 0.0);
			}
		}
		tile.clock = std::min(clockRel, 0.0f);
		tile.lastTouch = std::min(touchRel, 0.0f);
		tile.encodedBytes = payloadBytes + (uint32_t)(tile.fresh.empty() ? 0 : payload.size());
		trenchTiles.emplace(key, std::move(tile));
		restored++;
	}

	trenchStatTiles = trenchTiles.size();

	trenchReinjectRequested.store(true, std::memory_order_release);

	std::unordered_set<uint32_t> worldspaces;
	for (const auto& [key, tile] : trenchTiles)
		worldspaces.insert(key.worldspace);
	std::string spaces;
	for (uint32_t id : worldspaces)
		spaces += std::format("{:08X} ", id);
	logger::info("[SNOW DEFORMATION] trench co-save restored {} of {} tiles across worldspace(s) {}(active {:08X})",
		restored, tileCount, spaces, activeWorldspace.load(std::memory_order_acquire));
}

void SnowDeformation::RegisterTrenchCoSave()
{
	CoSave::GetSingleton()->Register(
		kTrenchRecord, kTrenchRecordVersion,
		[this](const SKSE::SerializationInterface* a_intfc) { SaveTrenchStore(a_intfc); },
		[this](const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t a_length) { LoadTrenchStore(a_intfc, a_version, a_length); },
		[this]() {
			std::scoped_lock lock(trenchStoreMutex);
			ClearTrenchStoreLocked("the co-save revert");
			gameClockUnarm.store(true, std::memory_order_release);
			trenchDecayClock = 0.0f;
		});
}
