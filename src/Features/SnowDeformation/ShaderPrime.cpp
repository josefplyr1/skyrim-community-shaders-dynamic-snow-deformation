#include "Features/SnowDeformation.h"

#include <d3dcompiler.h>
#include <fstream>
#include <functional>

#include "Globals.h"
#include "Utils/D3D.h"

static constexpr uint32_t kSnowBlobMagic = 0x53445343u;
static constexpr uint32_t kSnowBlobVersion = 1;
static constexpr uint64_t kSnowBlobMaxBytes = 64ull << 20;

uint64_t SnowDeformation::ShaderKeyHash(std::string_view a_text)
{
	uint64_t hash = 14695981039346656037ull;
	for (unsigned char c : a_text) {
		hash ^= c;
		hash *= 1099511628211ull;
	}
	return hash;
}

static std::filesystem::path SnowShaderCacheDir()
{
	if (auto dir = SKSE::log::log_directory())
		return *dir / "SnowDeformation-ShaderCache";
	return std::filesystem::path("Data") / "SnowDeformation-ShaderCache";
}

std::string SnowDeformation::ShaderSourcesFingerprint()
{
	std::scoped_lock lock(snowShaderCacheMutex);
	if (!snowSourcesFingerprint.empty())
		return snowSourcesFingerprint;

	std::vector<std::filesystem::path> files;
	std::error_code ec;
	for (auto it = std::filesystem::recursive_directory_iterator("Data\\Shaders", ec);
		it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		if (ec)
			break;
		if (!it->is_regular_file(ec))
			continue;
		const auto ext = it->path().extension();
		if (ext != L".hlsl" && ext != L".hlsli")
			continue;
		files.push_back(it->path());
	}
	std::sort(files.begin(), files.end());

	uint64_t h1 = 14695981039346656037ull;
	uint64_t h2 = h1 ^ 0x9e3779b97f4a7c15ull;
	std::vector<char> buffer;
	for (const auto& file : files) {
		const auto name = file.string();
		h1 = ShaderKeyHash(name) ^ (h1 * 1099511628211ull);
		std::ifstream in(file, std::ios::binary);
		if (!in)
			continue;
		in.seekg(0, std::ios::end);
		const auto size = (size_t)in.tellg();
		in.seekg(0, std::ios::beg);
		buffer.resize(size);
		in.read(buffer.data(), size);
		const auto content = std::string_view(buffer.data(), size);
		h1 = ShaderKeyHash(content) ^ (h1 * 1099511628211ull);
		h2 = (ShaderKeyHash(content) + 0x9e3779b97f4a7c15ull) ^ (h2 * 1099511628211ull);
	}
	snowSourcesFingerprint = std::format("{:016x}{:016x}n{}", h1, h2, files.size());
	return snowSourcesFingerprint;
}

winrt::com_ptr<ID3DBlob> SnowDeformation::ShaderCacheLoad(const std::string& a_key, const std::string& a_file)
{
	winrt::com_ptr<ID3DBlob> blob;
	std::ifstream in(SnowShaderCacheDir() / a_file, std::ios::binary);
	if (!in) {
		snowShaderCacheMisses.fetch_add(1, std::memory_order_relaxed);
		return blob;
	}
	uint32_t magic = 0, fileVersion = 0, keyLen = 0;
	uint64_t size = 0;
	in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
	in.read(reinterpret_cast<char*>(&fileVersion), sizeof(fileVersion));
	in.read(reinterpret_cast<char*>(&keyLen), sizeof(keyLen));
	if (!in || magic != kSnowBlobMagic || fileVersion != kSnowBlobVersion || keyLen == 0 || keyLen > 16384) {
		snowShaderCacheMisses.fetch_add(1, std::memory_order_relaxed);
		return blob;
	}
	std::string storedKey(keyLen, '\0');
	in.read(storedKey.data(), keyLen);
	in.read(reinterpret_cast<char*>(&size), sizeof(size));
	if (!in || storedKey != a_key || size == 0 || size > kSnowBlobMaxBytes) {
		snowShaderCacheMisses.fetch_add(1, std::memory_order_relaxed);
		return blob;
	}
	if (FAILED(D3DCreateBlob((SIZE_T)size, blob.put()))) {
		snowShaderCacheMisses.fetch_add(1, std::memory_order_relaxed);
		return blob;
	}
	in.read(static_cast<char*>(blob->GetBufferPointer()), size);
	if (!in) {
		blob = nullptr;
		snowShaderCacheMisses.fetch_add(1, std::memory_order_relaxed);
		return blob;
	}
	snowShaderCacheHits.fetch_add(1, std::memory_order_relaxed);
	return blob;
}

void SnowDeformation::ShaderCacheStore(const std::string& a_key, const std::string& a_file, ID3DBlob* a_blob)
{
	if (!a_blob || a_key.size() > 16384)
		return;
	std::error_code ec;
	std::filesystem::create_directories(SnowShaderCacheDir(), ec);
	std::ofstream out(SnowShaderCacheDir() / a_file, std::ios::binary | std::ios::trunc);
	if (!out)
		return;
	const uint32_t keyLen = (uint32_t)a_key.size();
	const uint64_t size = a_blob->GetBufferSize();
	out.write(reinterpret_cast<const char*>(&kSnowBlobMagic), sizeof(kSnowBlobMagic));
	out.write(reinterpret_cast<const char*>(&kSnowBlobVersion), sizeof(kSnowBlobVersion));
	out.write(reinterpret_cast<const char*>(&keyLen), sizeof(keyLen));
	out.write(a_key.data(), keyLen);
	out.write(reinterpret_cast<const char*>(&size), sizeof(size));
	out.write(static_cast<const char*>(a_blob->GetBufferPointer()), size);
}

ID3D11DeviceChild* SnowDeformation::CompileSnowShader(const wchar_t* a_path, const std::vector<std::pair<const char*, const char*>>& a_defines, const char* a_target, const char* a_entry)
{
	return Util::CompileShader(a_path, a_defines, a_target, a_entry);
}

void SnowDeformation::DataLoaded()
{
	InstallDevBenchTools();
	int expected = 0;
	if (!snowPrimeState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
		return;
	snowPrimeThread = std::thread([this] {
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		RunShaderPrime();
		snowPrimeState.store(2, std::memory_order_release);
	});
}

SnowDeformation::~SnowDeformation()
{
	if (snowPrimeThread.joinable())
		snowPrimeThread.join();
}

void SnowDeformation::RunShaderPrime()
{
	const auto start = std::chrono::steady_clock::now();

	const std::function<void()> groundSteps[] = {
		[&] { ShaderSourcesFingerprint(); },
		[&] { GetShellPSPrepassEarly(); },
		[&] { GetShellPSPrepassAttr(false); },
		[&] { GetShellPSEarlyResolve(); },
		[&] { GetShellPSDeferred(); },
		[&] { GetNoiseHashCS(); },
		[&] { GetShellFillVS(); },
		[&] { GetShellFillRectVS(); },
		[&] { GetShellFillPS(); },
		[&] { GetShellFillPSClaim(); },
		[&] { GetShellFillPSPreSnow(); },
		[&] { GetShellTerrainCutCS(); },
		[&] { GetDepthSyncCS(); },
		[&] { GetPreSnowCopyCS(); },
		[&] { GetSssSyncCS(); },
		[&] { GetSssRestoreCS(); },
		[&] { GetSkinSssSyncCS(); },
		[&] { GetSkinSssRestoreCS(); },
		[&] { GetSkinSssSyncMarkCS(); },
		[&] { GetHiZBuildCS(); },
		[&] { GetSkinCullCS(); },
		[&] { GetClusterCullCS(); },
		[&] { GetTerrainFineCS(); },
		[&] { GetExclusionFieldCS(); },
		[&] { GetBermFieldTiledCS(); },
		[&] { GetUntouchedTilesCS(); },
		[&] { GetDeformationRingCS(); },
		[&] { GetTrackRebaseCS(); },
		[&] { GetDeformationEvolveCS(); },
		[&] { GetDeformationStampCS(); },
		[&] { GetDeformationStampAllCS(); },
		[&] { GetDeformationScanEvolveCS(); },
		[&] { GetDeformationScanSnapshotCS(); },
		[&] { GetDeformationSnapshotCS(); },
		[&] { GetDeformationSnapshotFullCS(); },
		[&] { GetDeformationScanBermCS(); },
		[&] { GetPrintStampCS(); },
		[&] { GetPrintRingCS(); },
		[&] { GetPrintRefillCS(); },
		[&] { GetUndulationFieldCS(); },
		[&] { GetWindowFillCS(); },
	};
	const std::function<void()> objectSteps[] = {
		[&] { EnsureStaticsShaders(); },
		[&] { PrimePatchDeferred(); },
		[&] { GetSssChangedRaymarchCS(); },
		[&] { GetPatchShadowVS(); },
		[&] { EnsureSmoothNormalsCS(); },
	};
	const std::function<void()> effectSteps[] = {
		[] {},
	};

	auto runGroup = [&](const std::function<void()>* a_steps, size_t a_count, int a_phase, const char* a_name) {
		for (size_t i = 0; i < a_count; i++) {
			if (globals::game::quitGame)
				return false;
			a_steps[i]();
		}
		snowPrimePhase.store(a_phase, std::memory_order_release);
		logger::info("[SNOW DEFORMATION] shader prime: {} ready at {:.1f} s", a_name,
			std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		return true;
	};

	if (!runGroup(groundSteps, std::size(groundSteps), 1, "ground (snow + computes)"))
		return;
	if (!runGroup(objectSteps, std::size(objectSteps), 2, "object snow"))
		return;
	if (!runGroup(effectSteps, std::size(effectSteps), 3, "effects"))
		return;

	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const auto hits = snowShaderCacheHits.load(std::memory_order_relaxed);
	const auto misses = snowShaderCacheMisses.load(std::memory_order_relaxed);
	logger::info("[SNOW DEFORMATION] shader prime done in {:.1f} s ({} blobs from cache, {} compiled fresh)", seconds, hits, misses);
	LoadTraceMark(std::format("shader prime done in {:.1f} s ({} cached / {} compiled)", seconds, hits, misses));
}

