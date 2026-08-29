#include "Features/SnowDeformation.h"

#include <fstream>
#include <sstream>

// Load trace: attributes the time between "Continue" and playable. The
// instrumented phases live where the work is (TerrainData, TrenchStore,
// Statics, Shell); this file only aggregates and reports. Everything is
// gated on loadTraceActive so ordinary play pays one atomic load per site.

using Clock = std::chrono::steady_clock;

static double MsBetween(Clock::time_point a_from, Clock::time_point a_to)
{
	return std::chrono::duration<double, std::milli>(a_to - a_from).count();
}

void SnowDeformation::LoadTraceBegin(const char* a_reason)
{
	std::scoped_lock lock(loadTraceMutex);
	if (loadTraceActive.load(std::memory_order_relaxed)) {
		double t = MsBetween(loadTraceStart, Clock::now()) / 1000.0;
		loadTraceTimeline.push_back(std::format("{:8.2f}s  another load began ({})", t, a_reason));
		loadTraceLastNotable = Clock::now();
		return;
	}
	loadTracePhases.clear();
	loadTraceTimeline.clear();
	loadTraceStart = loadTraceLastNotable = Clock::now();
	loadTraceFrames = 0;
	loadTraceStallFrames = 0;
	loadTraceWorstFrameMs = 0.0;
	loadTraceFrameSeen = false;
	loadTraceActive.store(true, std::memory_order_release);
	logger::info("[LoadTrace] begin ({})", a_reason);
}

void SnowDeformation::LoadTraceMark(std::string_view a_what)
{
	if (!loadTraceActive.load(std::memory_order_acquire))
		return;
	std::scoped_lock lock(loadTraceMutex);
	double t = MsBetween(loadTraceStart, Clock::now()) / 1000.0;
	if (loadTraceTimeline.size() < 400)
		loadTraceTimeline.push_back(std::format("{:8.2f}s  {}", t, a_what));
	loadTraceLastNotable = Clock::now();
	logger::info("[LoadTrace] {:.2f}s {}", t, a_what);
}

void SnowDeformation::LoadTraceRecord(const char* a_name, double a_ms)
{
	if (!loadTraceActive.load(std::memory_order_acquire))
		return;
	std::scoped_lock lock(loadTraceMutex);
	auto& phase = loadTracePhases[a_name];
	phase.count++;
	phase.totalMs += a_ms;
	phase.maxMs = std::max(phase.maxMs, a_ms);
	if (a_ms >= 100.0) {
		double t = MsBetween(loadTraceStart, Clock::now()) / 1000.0;
		if (loadTraceTimeline.size() < 400)
			loadTraceTimeline.push_back(std::format("{:8.2f}s  {} took {:.0f} ms", t, a_name, a_ms));
		loadTraceLastNotable = Clock::now();
		logger::info("[LoadTrace] {:.2f}s {} took {:.0f} ms", t, a_name, a_ms);
	}
}

void SnowDeformation::LoadTraceFramePulse()
{
	if (!loadTraceActive.load(std::memory_order_acquire))
		return;
	auto now = Clock::now();
	std::scoped_lock lock(loadTraceMutex);
	if (!loadTraceActive.load(std::memory_order_relaxed))
		return;
	if (loadTraceFrameSeen) {
		double gapMs = MsBetween(loadTraceLastFrame, now);
		loadTraceWorstFrameMs = std::max(loadTraceWorstFrameMs, gapMs);
		// 250 ms with no rendered frame is a stall a player sees. The gap is
		// measured between OUR pulses, so it includes engine and other-mod
		// time - the phase table below says how much of it was ours.
		if (gapMs >= 250.0) {
			loadTraceStallFrames++;
			double t = MsBetween(loadTraceStart, now) / 1000.0;
			if (loadTraceTimeline.size() < 400)
				loadTraceTimeline.push_back(std::format("{:8.2f}s  no frame for {:.0f} ms", t, gapMs));
			loadTraceLastNotable = now;
			logger::info("[LoadTrace] {:.2f}s no frame for {:.0f} ms", t, gapMs);
		}
	} else {
		double t = MsBetween(loadTraceStart, now) / 1000.0;
		loadTraceTimeline.push_back(std::format("{:8.2f}s  first rendered frame after the co-save read", t));
		logger::info("[LoadTrace] {:.2f}s first rendered frame after the co-save read", t);
	}
	loadTraceFrameSeen = true;
	loadTraceLastFrame = now;
	loadTraceFrames++;

	double sinceStart = MsBetween(loadTraceStart, now);
	double sinceNotable = MsBetween(loadTraceLastNotable, now);
	if (sinceStart > 300000.0)
		LoadTraceReportLocked("300 s cap");
	else if (sinceStart > 30000.0 && sinceNotable > 20000.0)
		LoadTraceReportLocked("load path quiet for 20 s");
}

void SnowDeformation::LoadTraceReportLocked(const char* a_reason)
{
	loadTraceActive.store(false, std::memory_order_release);
	double totalS = MsBetween(loadTraceStart, Clock::now()) / 1000.0;

	std::string report;
	report += std::format("Snow Deformation load trace - ended after {:.1f} s ({})\n", totalS, a_reason);
	report += std::format("Frames: {} rendered, {} gaps >= 250 ms, worst gap {:.0f} ms\n\n", loadTraceFrames, loadTraceStallFrames, loadTraceWorstFrameMs);

	report += "Timeline (marks, phase runs >= 100 ms, frame gaps >= 250 ms):\n";
	for (const auto& line : loadTraceTimeline)
		report += line + "\n";
	if (loadTraceTimeline.size() >= 400)
		report += "  (timeline capped at 400 entries; the phase table below is complete)\n";

	report += "\nPhases (every instrumented run, sorted by total):\n";
	report += std::format("  {:<44} {:>7} {:>10} {:>9}\n", "phase", "count", "total ms", "max ms");
	std::vector<std::pair<std::string, LoadTracePhase>> sorted(loadTracePhases.begin(), loadTracePhases.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.totalMs > b.second.totalMs; });
	double attributedMs = 0.0;
	for (const auto& [name, phase] : sorted) {
		report += std::format("  {:<44} {:>7} {:>10.1f} {:>9.1f}\n", name, phase.count, phase.totalMs, phase.maxMs);
		attributedMs += phase.totalMs;
	}
	report += std::format("\nAttributed to instrumented Snow Deformation phases: {:.1f} s of the {:.1f} s window.\n", attributedMs / 1000.0, totalS);
	report += "A long load whose phases attribute little time is NOT this feature's stall.\n";

	if (auto dir = SKSE::log::log_directory()) {
		auto path = *dir / "SnowDeformation-LoadTrace.txt";
		std::ofstream out(path, std::ios::trunc);
		if (out) {
			out << report;
			logger::info("[LoadTrace] report written to {}", path.string());
		} else {
			logger::warn("[LoadTrace] could not write {}; report follows in this log", path.string());
		}
	}
	// The log carries the report too: it survives a crash before the menu,
	// and the file location is discoverable from it.
	std::istringstream lines(report);
	for (std::string line; std::getline(lines, line);)
		logger::info("[LoadTrace] {}", line);
}
