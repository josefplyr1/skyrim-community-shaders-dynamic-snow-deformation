#include "Features/SnowDeformation.h"

#include <functional>

#include "Globals.h"
#include "Utils/D3D.h"

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
	logger::info("[SNOW DEFORMATION] shader prime done in {:.1f} s", seconds);
	LoadTraceMark(std::format("shader prime done in {:.1f} s", seconds));
}

