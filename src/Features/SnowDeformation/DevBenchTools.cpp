#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/HitchSampler.h"
#include "Features/SnowDeformation/DevSwitches.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <DevBenchAPI.h>
#endif

#ifdef DEVBENCH_BRIDGE_ENABLED
namespace
{
	void ShaderTypesTool(void*, const char*, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			auto* state = globals::state;
			const bool live = globals::menu && globals::menu->overlayVisible && globals::features::performanceOverlay.loaded && globals::features::performanceOverlay.IsOverlayVisible();
			json types = json::array();
			float total = 0.0f;
			{
				auto lock = state->Lock();
				State::ForEachShaderTypeWithMetrics([&](auto a_type, int, float a_drawCalls, float a_frameTime, float a_percent, float a_costPerCall) {
					types.push_back({ { "type", std::string(magic_enum::enum_name(a_type)) }, { "drawCalls", a_drawCalls }, { "cpuMs", a_frameTime }, { "percent", a_percent }, { "cpuMsPerCall", a_costPerCall } });
				});
				total = state->smoothFrameTimePerType[magic_enum::enum_integer(RE::BSShader::Type::Total)];
			}
			out = { { "live", live }, { "totalCpuMs", total }, { "frame_count", state->frameCountAtomic.load(std::memory_order_relaxed) }, { "types", types } };
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void CpuCensusTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			globals::features::snowDeformation.cpuCensusArmedUntil = globals::state->frameCount + std::clamp(args.value("arm", 120u), 1u, 100000u);
			const auto& s = globals::features::snowDeformation.cpuShown;
			json spans = json::array();
			for (uint32_t i = 0; i < SnowDeformation::CpuCensus::kSpanCount; i++)
				spans.push_back({ { "name", SnowDeformation::CpuCensus::kSpanNames[i] }, { "ms", s.spanMs[i] }, { "calls", s.spanCalls[i] } });
			const auto& cc = SnowDeformation::cpuCensus;
			json stampers = json::array();
			for (uint32_t i = 0; i < cc.lastStampSourceCount; i++) {
				const auto* form = RE::TESForm::LookupByID(cc.lastStampSources[i].formID);
				stampers.push_back({ { "formId", std::format("0x{:08X}", cc.lastStampSources[i].formID) }, { "name", form && form->GetName() ? form->GetName() : "" }, { "stamps", cc.lastStampSources[i].stamps } });
			}
			json storePaths = json::object();
			for (uint32_t i = 0; i < SnowDeformation::CpuCensus::kStorePathCount; i++)
				storePaths[SnowDeformation::CpuCensus::kStorePathNames[i]] = { { "calls", cc.storeCalls[i] }, { "raises", cc.storeRaises[i] } };
			out = { { "spans", spans }, { "storePaths", storePaths },
				{ "stampsTotal", cc.stampsTotal }, { "carveStampsTotal", cc.carveStampsTotal }, { "rowsMarkedTotal", cc.rowsMarkedTotal }, { "contactRectsTotal", cc.contactRectsTotal },
				{ "settledSkipsTotal", cc.settledSkipsTotal },
				{ "stampersThisFrame", cc.stampSourceCount }, { "lastStampers", stampers }, { "lastStampersFrame", cc.lastStampSourceFrame },
				{ "hookMs", s.hookMs }, { "hookCalls", s.hookCalls }, { "actorMs", s.actorMs }, { "actors", s.actors },
				{ "landMs", s.landMs }, { "landCalls", s.landCalls }, { "depthMs", s.depthMs }, { "depthCalls", s.depthCalls },
				{ "traverseMs", s.traverseMs }, { "traverseCalls", s.traverseCalls },
				{ "frame_count", globals::state->frameCountAtomic.load(std::memory_order_relaxed) } };
			if (const uint32_t want = args.value("history", 0u); want > 0) {
				auto& ring = SnowDeformation::cpuCensus;
				json names = json::array({ "frame", "periodMs", "hookMs", "actorMs", "landMs" });
				for (uint32_t i = 0; i < SnowDeformation::CpuCensus::kSpanCount; i++)
					names.push_back(SnowDeformation::CpuCensus::kSpanNames[i]);
				json rows = json::array();
				{
					std::scoped_lock ringLock(ring.frameRingLock);
					const uint32_t n = std::min(want, ring.frameRingCount);
					for (uint32_t k = 0; k < n; k++) {
						const auto& r = ring.frameRing[(ring.frameRingHead + SnowDeformation::CpuCensus::kFrameRing - n + k) % SnowDeformation::CpuCensus::kFrameRing];
						json row = json::array({ r.frame, r.periodMs, r.hookMs, r.actorMs, r.landMs });
						for (uint32_t i = 0; i < SnowDeformation::CpuCensus::kSpanCount; i++)
							row.push_back(r.spanMs[i]);
						rows.push_back(std::move(row));
					}
				}
				out["historyColumns"] = std::move(names);
				out["history"] = std::move(rows);
			}
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void HitchSamplerTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			const std::string action = args.value("action", "report");
			if (action == "start")
				out = { { "started", SnowHitch::Start(args.value("intervalUs", 1000u)) } };
			else
				out = SnowHitch::Report(args.value("thresholdMs", 60.0f), args.value("top", 25u), args.value("module", std::string{}));
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void ShellTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			out = globals::features::snowDeformation.DevBenchShellArgs(args);
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void VolumeTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			out = globals::features::snowDeformation.DevBenchVolumeArgs(args);
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void ReaderGatesTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			out = globals::features::snowDeformation.DevBenchReaderGatesArgs(args);
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void LandscapeTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			out = globals::features::snowDeformation.LandscapeSnowTool(args);
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	void TrenchProbeTool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			const json args = a_argsJson && *a_argsJson ? json::parse(a_argsJson) : json::object();
			json depths = json::array();
			for (const auto& p : args.value("points", json::array()))
				depths.push_back({ { "x", p.at(0) }, { "y", p.at(1) }, { "stored", globals::features::snowDeformation.ProbeTrenchStore(p.at(0).get<float>(), p.at(1).get<float>()) } });
			out = { { "points", depths } };
		} catch (const std::exception& e) {
			out = { { "error", e.what() } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}
}
#endif

json SnowDeformation::DevBenchShellArgs(const json& a_args)
{
	if (a_args.contains("shellKill") && a_args["shellKill"].is_number_integer())
		shellKillMask = a_args["shellKill"].get<uint32_t>();
	if (a_args.contains("shellStats") && a_args["shellStats"].is_boolean())
		shellPipelineStatsEnabled = a_args["shellStats"].get<bool>();
	if (a_args.contains("effectCostProbe") && a_args["effectCostProbe"].is_boolean() && a_args["effectCostProbe"].get<bool>())
		StartEffectCostProbe();
	if (a_args.contains("projSnowTint") && a_args["projSnowTint"].is_boolean())
		debugProjSnowView = a_args["projSnowTint"].get<bool>();
	if (a_args.contains("captureViewCull") && a_args["captureViewCull"].is_boolean())
		captureViewCullOff = !a_args["captureViewCull"].get<bool>();
	if (a_args.contains("viewGate") && a_args["viewGate"].is_boolean())
		shellViewGateOff = !a_args["viewGate"].get<bool>();
	if (a_args.contains("sssChangedMarch") && a_args["sssChangedMarch"].is_boolean())
		sssChangedMarchOff = !a_args["sssChangedMarch"].get<bool>();
	if (a_args.contains("decalOverlaySplit") && a_args["decalOverlaySplit"].is_boolean())
		SnowDevSwitches::decalOverlaySplit = a_args["decalOverlaySplit"].get<bool>();
	if (a_args.contains("heightMapSplit") && a_args["heightMapSplit"].is_boolean())
		SnowDevSwitches::heightMapSplit = a_args["heightMapSplit"].get<bool>();
	if (a_args.contains("evolveSnapshot") && a_args["evolveSnapshot"].is_boolean())
		evolveSnapshotOff = !a_args["evolveSnapshot"].get<bool>();
	if (a_args.contains("forceDeformationUpdate") && a_args["forceDeformationUpdate"].is_boolean())
		debugForceDeformationUpdate = a_args["forceDeformationUpdate"].get<bool>();
	if (a_args.contains("heightRasterCache") && a_args["heightRasterCache"].is_boolean())
		heightRasterCacheOff = !a_args["heightRasterCache"].get<bool>();
	if (a_args.contains("heightRasterVerify") && a_args["heightRasterVerify"].is_boolean())
		heightRasterVerify = a_args["heightRasterVerify"].get<bool>();
	if (a_args.contains("heightRasterVerifyMiss") && a_args["heightRasterVerifyMiss"].is_boolean())
		heightRasterVerifyMiss = a_args["heightRasterVerifyMiss"].get<bool>();
	if (a_args.contains("coneFused") && a_args["coneFused"].is_boolean())
		coneFusedOff = !a_args["coneFused"].get<bool>();
	if (a_args.contains("coneVerify") && a_args["coneVerify"].is_boolean())
		coneVerify = a_args["coneVerify"].get<bool>();
	if (a_args.contains("deadGhostTiles") && a_args["deadGhostTiles"].is_boolean())
		deadGhostTilesOff = !a_args["deadGhostTiles"].get<bool>();
	if (a_args.contains("patchBox") && a_args["patchBox"].is_boolean())
		patchBoxOff = !a_args["patchBox"].get<bool>();
	if (a_args.contains("patchVerify") && a_args["patchVerify"].is_boolean())
		patchVerify = a_args["patchVerify"].get<bool>();
	if (a_args.contains("combineVerify") && a_args["combineVerify"].is_boolean())
		combineVerify = a_args["combineVerify"].get<bool>();
	if (a_args.contains("heightRasterReset") && a_args["heightRasterReset"].is_boolean() && a_args["heightRasterReset"].get<bool>()) {
		heightRasterRuns = 0;
		for (int i = 0; i < 3; i++)
			heightRasterKept[i] = 0;
		std::fill_n(heightRasterVerified, 6, 0u);
		std::fill_n(heightRasterMismatch, 7, 0ull);
		coneRuns = coneFusedRuns = 0;
		patchVerifyFrames = 0;
		std::fill_n(patchVerifyMismatch, 5, 0ull);
		const std::scoped_lock debugLock(heightRasterDebugLock);
		heightRasterSamples.clear();
	}
	json rasterSamples = json::array();
	{
		const std::scoped_lock debugLock(heightRasterDebugLock);
		for (const auto& sample : heightRasterSamples)
			rasterSamples.push_back({ { "target", sample.target }, { "x", sample.x }, { "y", sample.y }, { "keptX", sample.keptX }, { "keptY", sample.keptY },
				{ "freshX", sample.freshX }, { "freshY", sample.freshY } });
	}
	static constexpr const char* kPresetNames[kQualityPresetCount] = { "low", "medium", "high" };
	static constexpr const char* kGateNames[3] = { "snowy", "unknown", "bare" };
	if (a_args.contains("qualityPreset") && a_args["qualityPreset"].is_string()) {
		const auto wanted = a_args["qualityPreset"].get<std::string>();
		for (int i = 0; i < kQualityPresetCount; i++)
			if (wanted == kPresetNames[i])
				ApplyQualityPreset(i);
	}
	json gateCells = json::array();
	if (a_args.contains("gateCells") && a_args["gateCells"].is_boolean() && a_args["gateCells"].get<bool>()) {
		int sqMinX, sqMinY, sqMaxX, sqMaxY;
		if (LoadedCellSquare(sqMinX, sqMinY, sqMaxX, sqMaxY)) {
			const std::vector<float> depths = LandTextureDepthSnapshot();
			const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
			const std::shared_lock lock(shellCellMutex);
			for (int cy = sqMinY; cy <= sqMaxY; ++cy) {
				for (int cx = sqMinX; cx <= sqMaxX; ++cx) {
					const uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(cy);
					json cell = { { "x", cx }, { "y", cy } };
					auto it = shellCells.find(key);
					if (it == shellCells.end() || it->second.worldspaceID != worldspace) {
						cell["state"] = shellFillerCells.contains(key) ? "filler" : "unbaked";
						gateCells.push_back(cell);
						continue;
					}
					std::map<uint16_t, std::pair<float, uint32_t>> hits;
					float maxRamp = 0.0f;
					uint32_t snowyVerts = 0;
					for (uint32_t idx = 0; idx < 33 * 33; ++idx) {
						float ramp = 0.0f;
						for (uint32_t slot = 0; slot < kShellVertexLayers; ++slot) {
							const uint16_t texture = it->second.layerTexture[idx][slot];
							if (texture >= depths.size() || depths[texture] <= 0.0f)
								continue;
							const float w = it->second.layerWeight[idx][slot] / 255.0f;
							if (w <= 0.0f)
								continue;
							ramp += w * depths[texture];
							auto& h = hits[texture];
							h.first = std::max(h.first, w);
							h.second++;
						}
						maxRamp = std::max(maxRamp, ramp);
						snowyVerts += ramp > 0.0f;
					}
					cell["state"] = snowyVerts ? "snowy" : "bare";
					cell["snowyVerts"] = snowyVerts;
					cell["maxRamp"] = maxRamp;
					json textures = json::array();
					for (const auto& [texture, h] : hits)
						textures.push_back({ { "index", texture }, { "path", texture < landTextures.size() ? landTextures[texture].path : std::string() },
							{ "depth", depths[texture] }, { "maxWeight", h.first }, { "verts", h.second } });
					cell["textures"] = textures;
					gateCells.push_back(cell);
				}
			}
		}
	}
	json capturedList = json::array();
	if (a_args.contains("capturedList") && a_args["capturedList"].is_boolean() && a_args["capturedList"].get<bool>()) {
		for (const auto& c : capturedStatics) {
			const auto& b = c.geometry->worldBound;
			capturedList.push_back({ { "name", c.geometry->name.c_str() }, { "x", c.world.translate.x }, { "y", c.world.translate.y }, { "z", c.world.translate.z },
				{ "radius", b.radius }, { "cell", { (int)std::floor(c.world.translate.x / 4096.0f), (int)std::floor(c.world.translate.y / 4096.0f) } },
				{ "lod", c.lodBatch }, { "road", c.road }, { "fadeExempt", c.fadeExempt }, { "fullCoat", c.fullCoat },
				{ "projected", c.projReal }, { "nearSnow", GroundNearHasSnow(c.world.translate.x, c.world.translate.y) } });
		}
	}
	const int preset = MatchingQualityPreset();
	json effectCosts = json::array();
	for (uint32_t i = 0; i < EffectCostProbe::kEffects && effectCost.done; i++)
		effectCosts.push_back({ { "effect", EffectCostProbe::kNames[i] }, { "ms", effectCost.withMs[i] - effectCost.withoutMs[i] }, { "withMs", effectCost.withMs[i] } });
	const auto& s = shellStatsLast;
	const auto& o = staticsStatsLast;
	const auto& p = patchStatsLast;
	return { { "shellKill", shellKillMask }, { "shellStats", shellPipelineStatsEnabled },
		{ "tessellation", settings.Tessellation },
		{ "conePasses", { { "coarse", conePassesCoarse } } },
		{ "exclusionCount", statExclusionCount }, { "exclusionWalk", { { "slot", exclusionGatherSlot }, { "slots", exclusionGatherSlots } } },
		{ "projSnowTint", debugProjSnowView }, { "tessViewportHalf", { lastShellCBData ? lastShellCBData->TessScreen.y : 0.0f, lastShellCBData ? lastShellCBData->TessScreen.z : 0.0f } },
		{ "qualityPreset", preset < 0 ? "custom" : kPresetNames[preset] },
		{ "roadMapsBuilt", RoadMapsWanted() }, { "capturedRoads", capturedRoads },
		{ "patchDeferredReady", !SnowShadersPending(2) && patchDeferredPS[0] && patchDeferredPS[1] },
		{ "deferredActive", shellDeferredThisFrame },
		{ "joinZPrepassState", static_cast<int>(shellEarlyState) }, { "joinZPrepassViewMismatches", shellEarlyViewMismatches },
		{ "sssSeesSnowActive", shellSssSyncFrame != UINT32_MAX && globals::state->frameCountAtomic.load(std::memory_order_relaxed) - shellSssSyncFrame <= 1u },
		{ "skinSssMarchActive", skinSssMarchFrame != UINT32_MAX && globals::state->frameCountAtomic.load(std::memory_order_relaxed) - skinSssMarchFrame <= 1u },
		{ "sssChangedMarch", !sssChangedMarchOff }, { "sssChangedMarchActive", sssChangedMarchFrame != UINT32_MAX && globals::state->frameCountAtomic.load(std::memory_order_relaxed) - sssChangedMarchFrame <= 1u },
		{ "evolveSnapshot", !evolveSnapshotOff }, { "forceDeformationUpdate", debugForceDeformationUpdate }, { "decalOverlaySplit", SnowDevSwitches::decalOverlaySplit.load() }, { "heightMapSplit", SnowDevSwitches::heightMapSplit.load() },
		{ "heightRaster", { { "cache", !heightRasterCacheOff }, { "verify", heightRasterVerify }, { "verifyMiss", heightRasterVerifyMiss },
							  { "keptLastRun", heightRasterKeptBits }, { "runs", heightRasterRuns },
							  { "kept", { heightRasterKept[0], heightRasterKept[1], heightRasterKept[2] } },
							  { "verified", { heightRasterVerified[0], heightRasterVerified[1], heightRasterVerified[2] } },
							  { "mismatchTexels", { heightRasterMismatch[0], heightRasterMismatch[1], heightRasterMismatch[2], heightRasterMismatch[3] } },
							  { "samples", rasterSamples } } },
		{ "cones", { { "fused", !coneFusedOff }, { "verify", coneVerify },
					   { "runs", coneRuns }, { "fusedRuns", coneFusedRuns },
					   { "verified", { heightRasterVerified[3], heightRasterVerified[4] } },
					   { "mismatchTexels", { heightRasterMismatch[4], heightRasterMismatch[5] } } } },
		{ "patchBox", { { "on", !patchBoxOff }, { "patches", patchBoxPatches }, { "lattice", kPatchGridDim * kPatchGridDim }, { "verify", patchVerify }, { "verifiedFrames", patchVerifyFrames },
						  { "mismatchTexels", { patchVerifyMismatch[0], patchVerifyMismatch[1], patchVerifyMismatch[2], patchVerifyMismatch[3], patchVerifyMismatch[4] } } } },
		{ "combine", { { "deadGhostTiles", !deadGhostTilesOff }, { "verify", combineVerify }, { "verified", heightRasterVerified[5] }, { "mismatchTexels", heightRasterMismatch[6] } } },
		{ "hiZSize", { skinCullHiZ ? skinCullHiZ->desc.Width : 0u, skinCullHiZ ? skinCullHiZ->desc.Height : 0u } },
		{ "gates", { { "shell", kGateNames[std::min(shellSnowVerdict, 2u)] }, { "shellBareFrames", shellGateBareFrames }, { "deform", kGateNames[std::min(deformSnowVerdict, 2u)] }, { "deformBareFrames", deformGateBareFrames }, { "groundSuspended", GroundShellsSuspended() }, { "captured", capturedStatics.size() }, { "capturesCulled", captureViewCulled }, { "captureCullBail", captureCullBail }, { "squareFloorZ", squareFloorValid ? json(squareFloorZ) : json(nullptr) }, { "heightMapValid", heightMapValid } } },
		{ "gateCells", gateCells }, { "capturedList", capturedList }, { "viewGate", !shellViewGateOff }, { "captureViewCull", !captureViewCullOff },
		{ "effectCost", { { "running", effectCost.running }, { "done", effectCost.done }, { "step", effectCost.step }, { "costs", effectCosts } } },
		{ "stats", { { "valid", s.valid }, { "vs", s.vsInvocations }, { "hs", s.hsInvocations }, { "ds", s.dsInvocations },
					   { "ps", s.psInvocations }, { "primitives", s.rasterizedPrimitives }, { "samplesPassed", s.samplesPassed } } },
		{ "objectStats", { { "valid", o.valid }, { "vs", o.vsInvocations }, { "hs", o.hsInvocations }, { "ds", o.dsInvocations },
							 { "ps", o.psInvocations }, { "primitives", o.rasterizedPrimitives }, { "samplesPassed", o.samplesPassed } } },
		{ "patchStats", { { "valid", p.valid }, { "vs", p.vsInvocations }, { "hs", p.hsInvocations }, { "ds", p.dsInvocations },
							{ "ps", p.psInvocations }, { "primitives", p.rasterizedPrimitives }, { "samplesPassed", p.samplesPassed } } } };
}

json SnowDeformation::DevBenchReaderGatesArgs(const json& a_args)
{
	auto flag = [&](const char* a_key, bool& a_off) {
		if (a_args.contains(a_key) && a_args[a_key].is_boolean())
			a_off = !a_args[a_key].get<bool>();
	};
	flag("interiorHeightGate", interiorHeightGateOff);
	flag("exteriorHeightGate", exteriorHeightGateOff);
	flag("cascadeHeightGate", cascadeHeightGateOff);
	flag("cascadeRaiseGate", cascadeRaiseGateOff);
	flag("lodRasterGate", lodRasterGateOff);
	flag("decalReachGate", decalReachGateOff);
	flag("masksCopyGate", masksCopyGateOff);
	flag("marchIdle", marchIdleOff);
	flag("bloodLatch", bloodLatchOff);
	flag("landMaskFeatureGate", landMaskFeatureGateOff);
	flag("recordGrowth", staticsRecordGrowOff);
	return { { "interiorHeightGate", !interiorHeightGateOff }, { "exteriorHeightGate", !exteriorHeightGateOff }, { "cascadeSnowSoon", cascadeSnowSoonLast },
		{ "cascadeHeightGate", !cascadeHeightGateOff }, { "cascadeRaiseGate", !cascadeRaiseGateOff }, { "cascadesRaised", cascadeRaisedLast }, { "cascadesUnraised", cascadeSkippedLast },
		{ "lodRasterGate", !lodRasterGateOff }, { "lodCapturesIgnored", lodCapturesIgnoredLast }, { "deformSuspended", deformSuspended }, { "groundSuspended", GroundShellsSuspended() },
		{ "decalReachGate", !decalReachGateOff }, { "decalReachMisses", decalReachMissesLast }, { "decalOverlays", bloodOverlaysLast }, { "decalMaskClearView", decalMaskClearView },
		{ "masksCopyGate", !masksCopyGateOff }, { "marchIdle", !marchIdleOff },
		{ "bloodLatch", !bloodLatchOff }, { "landMaskFeatureGate", !landMaskFeatureGateOff }, { "recordGrowth", !staticsRecordGrowOff },
		{ "heightIdleFrames", heightIdleFrames }, { "heightGhostsLive", heightGhostsLive },
		{ "heightGhostBox", { heightGhostBox.x, heightGhostBox.y, heightGhostBox.z, heightGhostBox.w } }, { "heightMapValid", heightMapValid },
		{ "marchIdleFrames", marchIdleFrames }, { "marchIdleSkipped", marchIdleSkipped },
		{ "bloodMapWritten", bloodMapWritten }, { "bloodWrittenBox", { bloodWrittenBox.x, bloodWrittenBox.y, bloodWrittenBox.z, bloodWrittenBox.w } },
		{ "landMaskHits", landMaskHits.load(std::memory_order_relaxed) }, { "landMaskMisses", landMaskMisses.load(std::memory_order_relaxed) },
		{ "landMaskForeignHits", landMaskForeignHits.load(std::memory_order_relaxed) },
		{ "recordCapacity", staticsRecordCapacity }, { "recordRefused", staticsRecordRefused }, { "recordFallbacks", staticsRecordFallbacks },
		{ "interior", InInteriorCell() }, { "frame_count", globals::state->frameCountAtomic.load(std::memory_order_relaxed) } };
}

json SnowDeformation::DevBenchVolumeArgs(const json& a_args)
{
	if (a_args.contains("memorySeconds") && a_args["memorySeconds"].is_number())
		voxelMemorySeconds = std::clamp(a_args["memorySeconds"].get<float>(), 0.5f, 30.0f);
	if (a_args.contains("showRings") && a_args["showRings"].is_boolean())
		showVolumeRings = a_args["showRings"].get<bool>();
	if (a_args.contains("forceRebuild") && a_args["forceRebuild"].is_boolean())
		debugForceVolumeRebuild = a_args["forceRebuild"].get<bool>();
	if (a_args.contains("stats") && a_args["stats"].is_boolean())
		voxelPipelineStatsEnabled = a_args["stats"].get<bool>();
	json levels = json::array();
	for (uint L = 0; L < VoxelLevelsLive(); L++) {
		const auto& lv = voxelLevels[L];
		levels.push_back({ { "level", L }, { "valid", lv.valid }, { "rebuilds", lv.rebuilds }, { "voxelSize", VoxelSizeForLevel(L) },
			{ "dims", { lv.dims.x, lv.dims.y, lv.dims.z } }, { "origin", { lv.origin.x, lv.origin.y, lv.origin.z } },
			{ "stableRebuilds", lv.stableRebuilds }, { "skipped", lv.skipped } });
	}
	return { { "volumeSnow", settings.VolumeSnow }, { "shadersFailed", voxelShadersFailed }, { "memorySeconds", voxelMemorySeconds },
		{ "showRings", showVolumeRings }, { "occupancyL0", voxelOccupancyValid ? json(voxelOccupancy) : json(nullptr) },
		{ "forceRebuild", debugForceVolumeRebuild }, { "idleSkipped", voxelIdleSkipped }, { "idleBlockers", voxelIdleBlockers }, { "skipRate", voxelSkipRate },
		{ "stats", { { "enabled", voxelPipelineStatsEnabled }, { "valid", voxelStatsLast.valid }, { "vs", voxelStatsLast.vsInvocations }, { "ps", voxelStatsLast.psInvocations },
					   { "primitives", voxelStatsLast.rasterizedPrimitives }, { "samplesPassed", voxelStatsLast.samplesPassed } } },
		{ "dirtyBricks", settings.VolumeDirtyBricks }, { "staggeredCapture", settings.VolumeStaggeredCapture }, { "ringShape", settings.VolumeRingShape },
		{ "capturedStatics", capturedStatics.size() }, { "levels", levels } };
}

void SnowDeformation::InstallDevBenchTools()
{
#ifdef DEVBENCH_BRIDGE_ENABLED
	auto* dvb = DevBenchAPI::GetDevBenchInterface001();
	if (!dvb)
		return;
	static constexpr const char* readerGatesDesc =
		R"({"description":"Reader-gate A/B switches (runtime-only, all default true = gate on). interiorHeightGate: indoors the cascades stop keeping the object height maps running; they idle once no ghost can be in the window (heightIdleFrames, heightGhostsLive, heightGhostBox). exteriorHeightGate: outdoors the cascades keep them running only while the next shadow pass can raise snow (cascadeSnowSoon: a snowy or unbaked cell in the loaded square, or a snowy cell one past it); with nothing else reading, they are skipped and rebuilt from a clear on return. cascadeHeightGate: that cell must also meet one of this frame's sun cascade footprints (false: the whole loaded square, as at first). cascadeRaiseGate: a sun cascade whose camera no snowy cell's raised land reaches draws its landscape unraised (cascadesRaised / cascadesUnraised: cascade cameras last frame). lodRasterGate: LOD batches with no skin in the object raster's window do not keep the height maps running (lodCapturesIgnored). In a walled city with its ground off, the deformation passes run only while a loaded capture meets the trench window (deformSuspended, groundSuspended). decalReachGate: the decal overlay takes only decals a loaded capture's skin can reach (its cull sphere plus the composite's 24-unit lift), or any while the ground snow draws, or with volume snow (decalReachMisses: decal draws turned away last frame; decalOverlays: decals composited; decalMaskClearView: the mask is cleared over the decals' rectangle only). masksCopyGate: the pre-shell Masks copy runs only for the ground snow or a capture. marchIdle: the sun-march caches stop after 30 frames with no ground snow and no capture and rebuild whole on return (marchIdleFrames, marchIdleSkipped). bloodLatch: the shells skip the blood map while no deposit lies in the window (bloodMapWritten, bloodWrittenBox). landMaskFeatureGate: the land snow-mask lookup runs only for landscape materials (landMaskForeignHits counts hits it would drop; counted only with the gate off). recordGrowth: the statics record buffers grow past 8192 records instead of falling back to per-draw updates (recordCapacity, recordFallbacks). No args = read state.","inputSchema":{"type":"object","properties":{"interiorHeightGate":{"type":"boolean"},"exteriorHeightGate":{"type":"boolean"},"cascadeHeightGate":{"type":"boolean"},"cascadeRaiseGate":{"type":"boolean"},"lodRasterGate":{"type":"boolean"},"decalReachGate":{"type":"boolean"},"masksCopyGate":{"type":"boolean"},"marchIdle":{"type":"boolean"},"bloodLatch":{"type":"boolean"},"landMaskFeatureGate":{"type":"boolean"},"recordGrowth":{"type":"boolean"}}}})";
	dvb->RegisterTool("snowdeformation.readergates", readerGatesDesc, &ReaderGatesTool, nullptr);
	static constexpr const char* shaderTypesDesc =
		R"({"description":"The F10 performance overlay's shader-type table: per BSShader type, smoothed draw calls and CPU wall time (ms) between draw submissions attributed to that type - NOT GPU time. live=false means the overlay is hidden and the numbers are stale (toggle it with F10).","readOnly":true,"inputSchema":{"type":"object","properties":{}}})";
	dvb->RegisterTool("snowdeformation.shadertypes", shaderTypesDesc, &ShaderTypesTool, nullptr);
	static constexpr const char* cpuCensusDesc =
		R"({"description":"Snow Deformation CPU census: 30-frame averages (ms, rdtsc calibrated to QPC) of named CPU spans - Prepass and DrawShell whole, DrawShell's consecutive segments - plus the capture hook, actor/land stamping, depth and traversal counters. Stamps: stampsTotal/carveStampsTotal/rowsMarkedTotal are cumulative (read deltas); lastStampers lists the actors that emitted stamps in the latest frame that had any (props and spells not attributed).","inputSchema":{"type":"object","properties":{"arm":{"type":"integer","description":"Frames the census stays armed (default 120)."},"history":{"type":"integer","description":"Return the newest N per-frame rows (up to 1200): frame, periodMs, hookMs, actorMs, landMs, then every span (historyColumns), oldest first."}}}})";
	dvb->RegisterTool("snowdeformation.cpucensus", cpuCensusDesc, &CpuCensusTool, nullptr);
	static constexpr const char* hitchSamplerDesc =
		R"({"description":"Stutter hunt: samples the render thread's stack (leaf address plus return addresses found in the top 4 KB) every intervalUs while started, and on report stops and compares frames longer than thresholdMs with the rest: leaf modules and addresses, and modules and return addresses on the stack, each with its share of hitch-frame and normal-frame samples. With module (e.g. CommunityShaders.dll), moduleAt charges every sample to that module: its own code at the leaf, or 'via' the nearest return address into it, so time in the driver or engine lands on the call that spent it. A threshold below or above every frame ranks the tables by count.","readOnly":true,"inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["start","report"]},"intervalUs":{"type":"integer"},"thresholdMs":{"type":"number"},"top":{"type":"integer"},"module":{"type":"string"}}}})";
	dvb->RegisterTool("snowdeformation.hitchsampler", hitchSamplerDesc, &HitchSamplerTool, nullptr);
	static constexpr const char* trenchProbeDesc =
		R"({"description":"Persistent trench store depth (0-1, bilinear) at world positions in the active worldspace. points: [[x,y],...].","readOnly":true,"inputSchema":{"type":"object","properties":{"points":{"type":"array","items":{"type":"array","items":{"type":"number"}}}},"required":["points"]}})";
	dvb->RegisterTool("snowdeformation.trenchprobe", trenchProbeDesc, &TrenchProbeTool, nullptr);
	static constexpr const char* shellDesc =
		R"({"description":"Ground snow measurement switches (runtime-only). shellKill: bitmask zeroing PS features before the ShellCB upload (1 parallax, 2 normal map, 4 RMAOS, 8 crisp shadows, 16 screen-space shadows, 64 glints, 128 point lights, 256 skylighting, 512 albedo texture; the statics shell shares the CB). conePasses (read-only): the cone passes run this frame. tessViewportHalf (read-only): the render viewport half size. qualityPreset: low|medium|high applies that preset (the menu buttons' code); the reply names the preset the settings match. effectCostProbe: true starts the effect-cost probe (each shading effect off in turn, paired with an all-on read, ~50 s); results in effectCost. shellStats: D3D11 pipeline statistics around the ground snow's DrawShell draws, returned in stats, and around the object-snow pass (depth prepass + shading), returned in objectStats (both read two frames late). roadMapsBuilt, capturedRoads (read-only): whether the road-only object maps were built this frame, and the roads captured. patchDeferredReady (read-only): both deferred trench patch shaders exist (primed with the object snow group); the patch draws deferred, its coverage cut into the ground's deferred targets then one full-screen shading pass, and forward until then and under the debug views. patchStats (with shellStats): the same queries around the patch draw alone, inside objectStats. hiZSize (read-only): the skin-cull pyramid's level 0. deferredActive (read-only): the ground snow's deferred pass drew this frame. joinZPrepassState (read-only): S1, the ground prepass before the game's opaque draws with its settled pixels in the main depth: 0 off, 1 drawing, 2 not drawing, 3 needs Terrain Blending, 4 unavailable. sssSeesSnowActive (read-only): the early prepass's depth went into Terrain Blending's 32-bit blended depth for Screen-Space Shadows' march in the last frame, so the ground snow took the mask as Lighting does (needs S1 drawing and Snow Contact Shadows). skinSssMarchActive (read-only): after the skins' depth prepass, Screen-Space Shadows marched again in the last frame with the object snow and the ground snow in the 32-bit blended depth, and the skins applied that mask as Lighting does (the second march adds to ScreenSpaceShadows::RayMarch). No args = read state. gates (read-only): the ground and deformation snow-presence verdicts (snowy / unknown / bare), their bare-frame counts, groundSuspended, the captured-statics count and heightMapValid. captureViewCull: false keeps captures that cannot be on screen while no ground snow is in view (A/B; gates.capturesCulled counts the drops; gates.captureCullBail says why no claim was made: 0 claimed, 1 off/empty/interior, 2 no square, 3 unbaked cell, 4 eye below floor, 5 corner at/above horizon, 6 view leaves the loaded square). viewGate: false makes the ground gate ask the whole loaded square again instead of the snowy cells in the view frustum (A/B). sssChangedMarch: false makes the skins' second march redo every wavefront instead of only those reading depth that changed since the first march (A/B; sssChangedMarchActive, read-only, says it skipped in the last frame). evolveSnapshot: false makes evolve's snapshot copy the whole map instead of the tiles within twice the evolve halo of occupancy (A/B). forceDeformationUpdate: the Debugging Options' Force Update (the map passes and evolve run every frame). decalOverlaySplit: true times the Decals - Overlay row's segments under their own rows (Decals - Overlay Depth Copy / Mask / Composite) until set false. heightMapSplit: the same for Objects - Height Maps (Layer 1, Ghost Merge, Fine, Far, Peel, Peel Merge, Combine, Object Cone, Sky). coneFused: false runs the cones as separate seed and settle passes where ConeFusedCS would take them in one (A/B). coneVerify: every fused or kept cone is recomputed by the separate passes and compared bit for bit; cones reports runs, fusedRuns, verified and mismatchTexels (object, road); heightRasterReset zeroes them. deadGhostTiles: false lets every ghost with a top and a bottom keep its tile elevated for the shelter mask (A/B). combineVerify: the shelter mask is built a second time with no tile skipping and compared bit for bit (combine.verified, combine.mismatchTexels). patchBox: false draws the whole trench-patch lattice instead of the row bands its footprint lines can reach (A/B); patchBox.patches = the last draw's patch count. patchVerify: the whole lattice also draws into copies of the patch's targets and depth taken before the bands, compared bit for bit (patchBox.verifiedFrames, patchBox.mismatchTexels: attributes 0-2, mark, depth); heightRasterReset zeroes them. capturedList: true lists the frame's captured statics (name, position, cell, bound radius, lod/road/fadeExempt/fullCoat, projected = the draw carries projection data, nearSnow = the ground gate's answer now). gateCells: true lists every cell of the loaded square with its gate state (snowy/bare/unbaked/filler), snowy vertex count, peak ramp depth and the land textures with positive depth (path, depth, peak weight, vertex count).","inputSchema":{"type":"object","properties":{"shellKill":{"type":"integer"},"shellStats":{"type":"boolean"},"effectCostProbe":{"type":"boolean"},"qualityPreset":{"type":"string"},"gateCells":{"type":"boolean"},"viewGate":{"type":"boolean"},"capturedList":{"type":"boolean"},"captureViewCull":{"type":"boolean"},"sssChangedMarch":{"type":"boolean"},"evolveSnapshot":{"type":"boolean"},"forceDeformationUpdate":{"type":"boolean"},"decalOverlaySplit":{"type":"boolean"},"heightMapSplit":{"type":"boolean"},"coneFused":{"type":"boolean"},"coneVerify":{"type":"boolean"},"deadGhostTiles":{"type":"boolean"},"combineVerify":{"type":"boolean"},"patchBox":{"type":"boolean"},"patchVerify":{"type":"boolean"}}}})";
	dvb->RegisterTool("snowdeformation.shell", shellDesc, &ShellTool, nullptr);
	static constexpr const char* volumeDesc =
		R"({"description":"Volume snow measurement switches (runtime-only, not serialised). memorySeconds: Volume Memory, 0.5-30 s. showRings: Colour Rings. forceRebuild: rebuild every ring on its cadence (the idle gate's A/B; false = gate on). stats: pipeline statistics + occlusion around the volume draw, returned in stats (PS invocations over samples passed = marches per written pixel; read two frames late). Returns the volume's state: shader failure, level-0 occupancy count, capture count, and per level valid/rebuilds/voxel size/dims/origin. No args = read state.","inputSchema":{"type":"object","properties":{"memorySeconds":{"type":"number"},"showRings":{"type":"boolean"},"forceRebuild":{"type":"boolean"},"stats":{"type":"boolean"}}}})";
	dvb->RegisterTool("snowdeformation.volume", volumeDesc, &VolumeTool, nullptr);
	static constexpr const char* landscapeDesc =
		R"({"description":"Landscape snow (runtime-only, not serialised): the ground snow drawn on the landscape's own meshes. The ground's prepass replays this frame's captured depth-prepass landscape; camera passes stay unraised and the sun cascades are raised onto the snow's surface (its visibility and depth push); 'shade' reports the replay's state and last-frame counts (captures, replayed draws, main-pass draws the capture missed, frames with no ground). blendBonus, blendPenalty: the land contest's depth bonus and carve penalty. landRecolor: the land's snow layers wear the shell's snow set texel by texel where the land shows. tessellate: near draws go through the hull (tessReach for the replay, shadowTessReach for the cascades, world units from the draw's bound); the rest take the raising VS. gate: tessellate only edges carrying sub-triangle surface detail; gateMask picks the checks (1 map tiles, 2 class borders, 4 water, 16 exclusions; default 23). cull: drop off-view patches (lift: units each corner is raised for the test, guard: clip-w fraction). maxFactor 1-64. measure: per-draw GPU timestamps of every landscape draw, averaged per frame since reset (categories and Utility depth targets); stats: pipeline statistics per draw. recompile: drop compiled shader sets. Returns config, totals, targets, shader sets (state 1 compiling, 2 ready, 3 failed) and the surface placement.","inputSchema":{"type":"object"}})";
	dvb->RegisterTool("snowdeformation.landscape", landscapeDesc, &LandscapeTool, nullptr);
	logger::info("[SNOW DEFORMATION] DevBench tools registered: snowdeformation.shadertypes, snowdeformation.cpucensus");
#endif
}
