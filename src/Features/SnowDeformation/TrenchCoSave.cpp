// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "CoSave.h"
#include "Globals.h"

#include <unordered_set>

// Persistent trenches, Stage C: the tile store on the SKSE co-save channel.
// Design in PERSISTENT-TRENCHES-PLAN.md; the channel itself is CoSave.h.
//
// Layout, version 1. The per-tile header is exactly kTrenchTileHeaderBytes so
// Stage D's budget counts what actually lands in the file:
//
//   chunk  { u32 tileCount, u16 tileDim, u16 pad, f32 tileWorldSize }
//   tile   { u32 worldspace, i32 x, i32 y, f32 clockRel, f32 touchRel,
//            u32 payloadBytes }  followed by payloadBytes of payload
//
// Clock and touch are RELATIVE to this session's own (both <= 0, meaning "this
// far behind now"), so nothing depends on two playthroughs sharing a calendar.
// On load the decay clock restarts at zero and a tile's negative clock is the
// decay it still owes. tileDim and tileWorldSize are written so a resolution
// change is detected rather than misread at the wrong scale.

void SnowDeformation::EncodeTrenchTile(const TrenchTile& a_tile, std::vector<uint8_t>& o_bytes) const
{
	const auto& depth = a_tile.depth;
	o_bytes.clear();
	o_bytes.reserve(depth.size() / 4);

	// The rule pinned in the header, and the one SweepTrenchStore measures
	// against: (count 1-255, value) pairs, a longer run split across pairs.
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

	// A tile the encoder would grow is written raw, which is what the measured
	// size assumed as well.
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

	// Written even when empty or switched off, so a record always exists and a
	// load can tell "no trenches" from "no data".
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
	size_t written = 0;
	for (const auto& [key, tile] : trenchTiles) {
		EncodeTrenchTile(tile, payload);
		const uint32_t payloadBytes = (uint32_t)payload.size();
		const float clockRel = tile.clock - trenchDecayClock;
		const float touchRel = tile.lastTouch - gameClockHours.load(std::memory_order_relaxed);

		a_intfc->WriteRecordData(&key.worldspace, sizeof(key.worldspace));
		a_intfc->WriteRecordData(&key.x, sizeof(key.x));
		a_intfc->WriteRecordData(&key.y, sizeof(key.y));
		a_intfc->WriteRecordData(&clockRel, sizeof(clockRel));
		a_intfc->WriteRecordData(&touchRel, sizeof(touchRel));
		a_intfc->WriteRecordData(&payloadBytes, sizeof(payloadBytes));
		a_intfc->WriteRecordData(payload.data(), payloadBytes);
		written += payloadBytes + kTrenchTileHeaderBytes;
	}

	// The budget slider is denominated in this number. A drift between what the
	// sweep measured and what was actually written means the cap has quietly
	// stopped bounding the save - cheap to notice here, expensive to diagnose
	// from a bug report about save size.
	if (trenchEncodedTotal && written > trenchEncodedTotal + trenchEncodedTotal / 8)
		logger::warn("[SNOW DEFORMATION] trench co-save wrote {} bytes against a measured {}; budget accounting has drifted",
			written, trenchEncodedTotal);
	logger::info("[SNOW DEFORMATION] trench co-save: {} tiles, {} KB", tileCount, written / 1024);
}

void SnowDeformation::LoadTrenchStore(const SKSE::SerializationInterface* a_intfc, uint32_t a_version, uint32_t)
{
	LoadTraceBegin("co-save trench store");
	LoadTraceScope _loadTrace(this, "CoSave: LoadTrenchStore");
	if (a_version != kTrenchRecordVersion) {
		// Refused, not guessed at: a future layout read as this one is a
		// corrupt store rather than a missing one.
		logger::warn("[SNOW DEFORMATION] trench co-save is version {}, this build reads {}; stored trenches dropped",
			a_version, kTrenchRecordVersion);
		return;
	}

	uint32_t tileCount = 0;
	uint16_t tileDim = 0;
	uint16_t pad = 0;
	float tileWorld = 0.0f;
	// Length-checked, not truthiness-checked: a truncated chunk returns a SHORT
	// read, and treating that as success would build tiles out of whatever the
	// stack happened to hold.
	if (a_intfc->ReadRecordData(&tileCount, sizeof(tileCount)) != sizeof(tileCount) ||
		a_intfc->ReadRecordData(&tileDim, sizeof(tileDim)) != sizeof(tileDim) ||
		a_intfc->ReadRecordData(&pad, sizeof(pad)) != sizeof(pad) ||
		a_intfc->ReadRecordData(&tileWorld, sizeof(tileWorld)) != sizeof(tileWorld)) {
		// Never silent: a return with no line here is indistinguishable from
		// the callback not firing at all.
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

	// Restart the clock with the store: every tile's reading was written
	// relative to it, so a negative clock is exactly the decay it still owes.
	trenchDecayClock = 0.0f;
	// UNARM the game-hours watch. Left armed, the first tick would read the
	// loaded save's calendar as a backwards jump and clear the store just
	// filled - the reverting-on-load trap arriving from the other side.
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
		// Both stay negative: relative readings order correctly against the
		// positive ones this session will stamp, so the LRU needs no fixup.
		tile.clock = std::min(clockRel, 0.0f);
		tile.lastTouch = std::min(touchRel, 0.0f);
		tile.encodedBytes = payloadBytes;
		trenchTiles.emplace(key, std::move(tile));
		restored++;
	}

	trenchStatTiles = trenchTiles.size();

	// The next update rebuilds the whole window from this. Without it the
	// tiles sit in the store unreachable: injection only touches texels
	// arriving from outside the window, and the player loads standing on the
	// ground they describe.
	trenchReinjectRequested.store(true, std::memory_order_release);

	// The worldspaces restored, against the one the inject will ask for. A
	// silent mismatch here looks exactly like a store that failed to load.
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
			// Fires before a save is loaded AND on a new game. Without it a
			// fresh game inherits the last one's trenches.
			std::scoped_lock lock(trenchStoreMutex);
			ClearTrenchStoreLocked("the co-save revert");
			// Unarmed so the first tick of the new timeline re-arms instead of
			// reading its calendar as a jump.
			gameClockUnarm.store(true, std::memory_order_release);
			trenchDecayClock = 0.0f;
		});
}
