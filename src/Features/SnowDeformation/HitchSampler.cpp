#include "HitchSampler.h"

#include "Globals.h"
#include "State.h"

#include <Psapi.h>

namespace
{
	constexpr uint32_t kStackCopyBytes = 4096;
	constexpr uint32_t kMaxReturns = 30;
	constexpr uint32_t kMaxSamples = 60000;
	constexpr uint32_t kMaxMarks = 8192;

	struct Sample
	{
		int64_t qpc;
		uint64_t rip;
		uint32_t returns;
		uint64_t ret[kMaxReturns];
	};

	struct Mark
	{
		int64_t qpc;
		uint32_t frame;
	};

	struct CodeRange
	{
		uint64_t start, end;
		uint32_t module;
	};

	struct Module
	{
		uint64_t base, size;
		std::string name;
	};

	std::atomic<uint32_t> g_mainTid{ 0 };
	std::atomic<uint64_t> g_stackHigh{ 0 };
	std::atomic<uint64_t> g_stackLow{ 0 };
	std::atomic<bool> g_running{ false };
	std::thread g_thread;
	std::vector<Sample> g_samples;
	std::atomic<uint32_t> g_sampleCount{ 0 };
	std::vector<Module> g_modules;
	std::vector<CodeRange> g_code;
	std::mutex g_markLock;
	Mark g_marks[kMaxMarks];
	uint32_t g_markHead = 0;
	uint32_t g_markCount = 0;
	uint32_t g_intervalUs = 1000;
	double g_qpcToMs = 0.0;

	void SnapshotModules()
	{
		g_modules.clear();
		g_code.clear();
		HMODULE mods[2048];
		DWORD needed = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
			return;
		const uint32_t count = std::min<uint32_t>(needed / sizeof(HMODULE), 2048);
		for (uint32_t i = 0; i < count; i++) {
			MODULEINFO info{};
			if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info)))
				continue;
			char name[MAX_PATH] = {};
			K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
			const uint32_t index = static_cast<uint32_t>(g_modules.size());
			const auto base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
			g_modules.push_back({ base, info.SizeOfImage, name });
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				continue;
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				continue;
			const auto* section = IMAGE_FIRST_SECTION(nt);
			for (uint32_t s = 0; s < nt->FileHeader.NumberOfSections; s++, section++)
				if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
					g_code.push_back({ base + section->VirtualAddress, base + section->VirtualAddress + std::max(section->Misc.VirtualSize, section->SizeOfRawData), index });
		}
		std::sort(g_code.begin(), g_code.end(), [](const CodeRange& a, const CodeRange& b) { return a.start < b.start; });
		std::vector<uint32_t> order(g_modules.size());
		for (uint32_t i = 0; i < order.size(); i++)
			order[i] = i;
		std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_modules[a].base < g_modules[b].base; });
		std::vector<Module> byBase;
		std::vector<uint32_t> remap(g_modules.size());
		for (uint32_t i = 0; i < order.size(); i++) {
			remap[order[i]] = i;
			byBase.push_back(std::move(g_modules[order[i]]));
		}
		g_modules = std::move(byBase);
		for (auto& r : g_code)
			r.module = remap[r.module];
	}

	const CodeRange* CodeAt(uint64_t a_address)
	{
		auto it = std::upper_bound(g_code.begin(), g_code.end(), a_address, [](uint64_t a, const CodeRange& r) { return a < r.start; });
		if (it == g_code.begin())
			return nullptr;
		--it;
		return a_address < it->end ? &*it : nullptr;
	}

	bool AfterCall(uint64_t a_address, const CodeRange& a_range)
	{
		if (a_address < a_range.start + 7)
			return false;
		const auto* p = reinterpret_cast<const uint8_t*>(a_address);
		if (p[-5] == 0xE8)
			return true;
		if (p[-6] == 0xFF && p[-5] == 0x15)
			return true;
		if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0)
			return true;
		if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
			return true;
		if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10 && (p[-5] & 0xC0) == 0x80)
			return true;
		return false;
	}

	void Loop()
	{
		const uint32_t tid = g_mainTid.load();
		HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
		if (!thread) {
			g_running = false;
			return;
		}
		HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		alignas(16) static uint8_t copy[kStackCopyBytes];
		while (g_running.load(std::memory_order_relaxed)) {
			const uint32_t slot = g_sampleCount.load(std::memory_order_relaxed);
			if (slot >= kMaxSamples)
				break;
			alignas(16) CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
			uint32_t copied = 0;
			int64_t qpc = 0;
			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				qpc = now.QuadPart;
				if (GetThreadContext(thread, &ctx)) {
					const uint64_t high = g_stackHigh.load(std::memory_order_relaxed);
					const uint64_t low = g_stackLow.load(std::memory_order_relaxed);
					if (ctx.Rsp >= low && ctx.Rsp < high) {
						copied = static_cast<uint32_t>(std::min<uint64_t>(kStackCopyBytes, high - ctx.Rsp)) & ~7u;
						memcpy(copy, reinterpret_cast<const void*>(ctx.Rsp), copied);
					}
				}
				ResumeThread(thread);
			}
			if (qpc) {
				Sample& s = g_samples[slot];
				s.qpc = qpc;
				s.rip = ctx.Rip;
				s.returns = 0;
				const auto* words = reinterpret_cast<const uint64_t*>(copy);
				for (uint32_t w = 0; w < copied / 8 && s.returns < kMaxReturns; w++) {
					if (const CodeRange* range = CodeAt(words[w]); range && AfterCall(words[w], *range))
						s.ret[s.returns++] = words[w];
				}
				g_sampleCount.store(slot + 1, std::memory_order_release);
			}
			if (timer) {
				LARGE_INTEGER due;
				due.QuadPart = -static_cast<int64_t>(g_intervalUs) * 10;
				SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
				WaitForSingleObject(timer, 100);
			} else {
				Sleep(1);
			}
		}
		if (timer)
			CloseHandle(timer);
		CloseHandle(thread);
	}

	const Module* ModuleAt(uint64_t a_address)
	{
		auto it = std::upper_bound(g_modules.begin(), g_modules.end(), a_address, [](uint64_t a, const Module& m) { return a < m.base; });
		if (it == g_modules.begin())
			return nullptr;
		--it;
		return a_address < it->base + it->size ? &*it : nullptr;
	}

	std::string Where(uint64_t a_address)
	{
		if (const Module* m = ModuleAt(a_address))
			return std::format("{}+0x{:X}", m->name, a_address - m->base);
		return std::format("0x{:X}", a_address);
	}

	std::string ModuleOf(uint64_t a_address)
	{
		const Module* m = ModuleAt(a_address);
		return m ? m->name : "?";
	}
}

namespace SnowHitch
{
	void OnFrame()
	{
		if (g_mainTid.load(std::memory_order_relaxed) == 0) {
			g_stackLow = 0;
			g_stackHigh = reinterpret_cast<uint64_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
			g_mainTid = GetCurrentThreadId();
		}
		if (!g_running.load(std::memory_order_relaxed))
			return;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		std::scoped_lock lock(g_markLock);
		g_marks[g_markHead] = { now.QuadPart, globals::state->frameCount };
		g_markHead = (g_markHead + 1) % kMaxMarks;
		g_markCount = std::min(g_markCount + 1, kMaxMarks);
	}

	bool Start(uint32_t a_intervalUs)
	{
		if (g_running.load() || g_mainTid.load() == 0)
			return false;
		if (g_thread.joinable())
			g_thread.join();
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		g_qpcToMs = 1000.0 / double(freq.QuadPart);
		SnapshotModules();
		g_samples.resize(kMaxSamples);
		g_sampleCount = 0;
		{
			std::scoped_lock lock(g_markLock);
			g_markHead = 0;
			g_markCount = 0;
		}
		g_intervalUs = std::clamp(a_intervalUs, 250u, 20000u);
		g_running = true;
		g_thread = std::thread(Loop);
		return true;
	}

	json Report(float a_thresholdMs, uint32_t a_top, const std::string& a_module)
	{
		g_running = false;
		if (g_thread.joinable())
			g_thread.join();
		std::vector<Mark> marks;
		{
			std::scoped_lock lock(g_markLock);
			for (uint32_t k = 0; k < g_markCount; k++)
				marks.push_back(g_marks[(g_markHead + kMaxMarks - g_markCount + k) % kMaxMarks]);
		}
		const uint32_t sampleCount = g_sampleCount.load();
		json out = { { "frames", marks.size() }, { "samples", sampleCount }, { "thresholdMs", a_thresholdMs }, { "intervalUs", g_intervalUs } };
		if (marks.size() < 3 || sampleCount == 0)
			return out;

		std::vector<float> periods;
		for (size_t i = 1; i < marks.size(); i++)
			periods.push_back(float((marks[i].qpc - marks[i - 1].qpc) * g_qpcToMs));
		std::vector<float> sorted = periods;
		std::sort(sorted.begin(), sorted.end());
		uint32_t hitchFrames = 0;
		double hitchTotal = 0.0;
		for (float p : periods)
			if (p > a_thresholdMs) {
				hitchFrames++;
				hitchTotal += p;
			}

		struct Tally
		{
			uint32_t hitch = 0, normal = 0;
		};
		std::unordered_map<std::string, Tally> leafModules, leafAt, stackModules, stackAt, moduleAt;
		uint32_t hitchSamples = 0, normalSamples = 0, moduleSamples = 0;
		for (uint32_t i = 0; i < sampleCount; i++) {
			const Sample& s = g_samples[i];
			auto it = std::upper_bound(marks.begin(), marks.end(), s.qpc, [](int64_t q, const Mark& m) { return q < m.qpc; });
			if (it == marks.begin() || it == marks.end())
				continue;
			const size_t frame = static_cast<size_t>(it - marks.begin()) - 1;
			const bool hitch = periods[frame] > a_thresholdMs;
			(hitch ? hitchSamples : normalSamples)++;
			auto bump = [&](std::unordered_map<std::string, Tally>& a_map, const std::string& a_key) {
				auto& t = a_map[a_key];
				(hitch ? t.hitch : t.normal)++;
			};
			bump(leafModules, ModuleOf(s.rip));
			bump(leafAt, Where(s.rip & ~0xFull));
			std::unordered_set<std::string> seen;
			bool charged = !a_module.empty() && ModuleOf(s.rip) == a_module;
			if (charged)
				bump(moduleAt, Where(s.rip & ~0xFull));
			for (uint32_t r = 0; r < s.returns; r++) {
				const std::string m = ModuleOf(s.ret[r]);
				if (seen.insert(m).second)
					bump(stackModules, m);
				bump(stackAt, Where(s.ret[r]));
				if (!charged && !a_module.empty() && m == a_module) {
					bump(moduleAt, "via " + Where(s.ret[r]));
					charged = true;
				}
			}
			moduleSamples += charged;
		}
		auto table = [&](const std::unordered_map<std::string, Tally>& a_map) {
			std::vector<std::pair<std::string, Tally>> rows(a_map.begin(), a_map.end());
			const double h = std::max(1u, hitchSamples), n = std::max(1u, normalSamples);
			const auto key = [&](const Tally& t) { return hitchSamples && normalSamples ? t.hitch / h - t.normal / n : double(t.hitch + t.normal); };
			std::sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return key(a.second) > key(b.second); });
			json t = json::array();
			for (size_t i = 0; i < rows.size() && i < a_top; i++)
				t.push_back({ { "at", rows[i].first }, { "hitchShare", rows[i].second.hitch / h }, { "normalShare", rows[i].second.normal / n }, { "hitch", rows[i].second.hitch }, { "normal", rows[i].second.normal } });
			return t;
		};
		out["hitchFrames"] = hitchFrames;
		out["hitchMeanMs"] = hitchFrames ? hitchTotal / hitchFrames : 0.0;
		out["medianFrameMs"] = sorted[sorted.size() / 2];
		out["hitchSamples"] = hitchSamples;
		out["normalSamples"] = normalSamples;
		out["leafModules"] = table(leafModules);
		out["leafAt"] = table(leafAt);
		out["stackModules"] = table(stackModules);
		out["stackAt"] = table(stackAt);
		if (!a_module.empty()) {
			out["module"] = a_module;
			out["moduleSamples"] = moduleSamples;
			out["moduleAt"] = table(moduleAt);
		}
		return out;
	}
}
