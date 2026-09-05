#include "Features/SnowDeformation.h"

#include <d3dcompiler.h>
#include <fstream>
#include <functional>

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

// Shader prime + blob disk cache. See the header block for the contract.
//
// The fingerprint deliberately hashes ALL of Data\Shaders, not the snow
// includes' exact closure: over-wide only costs a background recompile after
// an unrelated edit, while under-wide serves stale bytecode after a deploy -
// the invisible-change trap this project's workflow cannot afford.

static constexpr uint32_t kSnowBlobMagic = 0x53445343u;  // "SDSC"
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

	// Contents, not timestamps: MO2's VFS makes mtimes untrustworthy across
	// deploys. Deterministic order so the hash is stable per launch.
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
	// The full key is compared, not just the filename hash, so a collision or
	// a stale fingerprint reads as a miss rather than serving wrong bytecode.
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
	std::string defs;
	for (const auto& define : a_defines) {
		defs += define.first ? define.first : "";
		if (define.second && *define.second) {
			defs += '=';
			defs += define.second;
		}
		defs += ';';
	}
	// Everything Util::CompileShader folds into the compile beyond its
	// arguments: the global define set, developer mode (which also swaps the
	// flag set), and the flag-affecting toggles. Any of them changing must
	// miss the cache.
	auto state = globals::state;
	std::string global;
	if (auto shaderDefines = state->GetDefines())
		for (const auto& define : *shaderDefines)
			global += define.first + "=" + define.second + ";";
	const auto pathUtf8 = Util::WStringToString(a_path);
	const auto key = std::format("v{}|fp{}|{}|{}|{}|defs:{}|glob:{}|env:d{}p{}f{}v{}",
		kSnowBlobVersion, ShaderSourcesFingerprint(), pathUtf8, a_entry, a_target, defs, global,
		state->IsDeveloperMode() ? 1 : 0,
		state->enablePartialPrecision.load(std::memory_order_relaxed) ? 1 : 0,
		state->enableAvoidFlowControl.load(std::memory_order_relaxed) ? 1 : 0,
		globals::shaderCache->IsDiskCache() ? 1 : 0);

	const auto file = std::format("{}_{}_{}_{:016x}.bin",
		std::filesystem::path(a_path).stem().string(), a_target, a_entry,
		ShaderKeyHash(std::format("{}|{}|{}|{}", pathUtf8, a_entry, a_target, defs)));

	if (auto blob = ShaderCacheLoad(key, file))
		return Util::CreateShaderFromBlob(blob->GetBufferPointer(), blob->GetBufferSize(), a_target);

	ID3DBlob* raw = nullptr;
	auto shader = Util::CompileShader(a_path, a_defines, a_target, a_entry, &raw);
	winrt::com_ptr<ID3DBlob> blob;
	blob.attach(raw);
	if (shader && blob)
		ShaderCacheStore(key, file, blob.get());
	return shader;
}

void SnowDeformation::DataLoaded()
{
	int expected = 0;
	if (!snowPrimeState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
		return;
	snowPrimeThread = std::thread([this] {
		// Grace nap: a render-thread call already past a gate when the state
		// flipped drains out before the worker touches any shader member.
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

	// Three groups, in the order the player sees them, each PUBLISHED through
	// snowPrimePhase the moment it completes so the gated render paths
	// unlock per group: on a cold cache the snowfield appears when ITS
	// shaders exist, not when all of them do. Ownership contract: the worker
	// never touches a published group's members again, and the render thread
	// never touches an unpublished group's (SnowShadersPending). A quit
	// mid-prime bails between items so shutdown never waits on more than one
	// compile.
	const std::function<void()> groundSteps[] = {
		[&] { ShaderSourcesFingerprint(); },
		[&] { GetShellVS(); },
		[&] { GetShellPS(); },
		[&] { GetShellHS(false); },
		[&] { GetShellHS(true); },
		[&] { GetShellHSNear(false); },
		[&] { GetShellHSNear(true); },
		[&] { GetShellHSFar(false); },
		[&] { GetShellHSFar(true); },
		[&] { GetShellDS(); },
		[&] { GetShellDSBake(false); },
		[&] { GetShellBakeCS(); },
		[&] { GetShellTessVS(); },
		[&] { GetShellShadowVS(); },
		[&] { GetShellPSNoDepth(); },
		[&] { GetDepthSyncCS(); },
		[&] { GetHiZBuildCS(); },
		[&] { GetSkinCullCS(); },
		[&] { GetClusterCullCS(); },
		[&] { GetExclusionFieldCS(); },
		[&] { GetBermFieldCS(); },
		[&] { GetBermFieldTiledCS(); },
		[&] { GetDeformationRingCS(); },
		[&] { GetDeformationEvolveCS(); },
		[&] { GetDeformationStampCS(); },
		[&] { GetDeformationStampAllCS(); },
		[&] { GetDeformationScanEvolveCS(); },
		[&] { GetDeformationTileArgsCS(); },
		[&] { GetDeformationScanBermCS(); },
		[&] { GetUndulationFieldCS(); },
		[&] { GetWindowFillCS(); },
	};
	const std::function<void()> objectSteps[] = {
		[&] { EnsureStaticsShaders(); },
		[&] { GetPatchShadowVS(); },
		[&] { EnsureSmoothNormalsCS(); },
	};
	const std::function<void()> effectSteps[] = {
		[&] { GetLightningArcVS(); },
		[&] { GetLightningArcPS(); },
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

	if (!runGroup(groundSteps, std::size(groundSteps), 1, "ground (shell + computes)"))
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

