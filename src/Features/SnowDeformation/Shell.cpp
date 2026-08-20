#include "Features/SnowDeformation.h"

#include <DDSTextureLoader.h>

#include "Deferred.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Features/LightLimitFix.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

void SnowDeformation::CopySRVResource(ID3D11ShaderResourceView* a_srcSRV, const char* a_name,
	winrt::com_ptr<ID3D11Texture2D>& a_tex, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv)
{
	winrt::com_ptr<ID3D11Resource> srcRes;
	a_srcSRV->GetResource(srcRes.put());
	auto srcTex = srcRes.try_as<ID3D11Texture2D>();
	if (!srcTex)
		return;

	D3D11_TEXTURE2D_DESC srcDesc;
	srcTex->GetDesc(&srcDesc);

	bool recreate = !a_tex || !a_srv;
	if (a_tex) {
		D3D11_TEXTURE2D_DESC haveDesc;
		a_tex->GetDesc(&haveDesc);
		recreate |= haveDesc.Width != srcDesc.Width || haveDesc.Height != srcDesc.Height ||
		            haveDesc.ArraySize != srcDesc.ArraySize || haveDesc.Format != srcDesc.Format;
	}
	if (recreate) {
		a_srv = nullptr;
		a_tex = nullptr;

		D3D11_TEXTURE2D_DESC copyDesc = srcDesc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		copyDesc.MiscFlags = 0;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		copyDesc.CPUAccessFlags = 0;
		if (FAILED(globals::d3d::device->CreateTexture2D(&copyDesc, nullptr, a_tex.put())))
			return;
		Util::SetResourceName(a_tex.get(), a_name);

		// Reuse the source SRV's view description so typeless depth formats
		// resolve to the same shader-readable format.
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		a_srcSRV->GetDesc(&srvDesc);
		if (FAILED(globals::d3d::device->CreateShaderResourceView(a_tex.get(), &srvDesc, a_srv.put()))) {
			a_tex = nullptr;
			return;
		}
	}

	globals::d3d::context->CopyResource(a_tex.get(), srcTex.get());
}

/**
 * @brief Lazy-loads the shell snow texture set from the user-configurable
 * path (through the MO2 VFS).
 *
 * The TruePBR variant of the chosen path (Textures\PBR\..., _n / _rmaos
 * companions, linear-color albedo) is probed first. When found, the
 * modlist's own TruePBR config JSON (PBRTextureSets\, matched by texture
 * basename) supplies the authored glint/roughness/specular values.
 */

/**
 * @brief Loads a DDS through the GAME's resource system rather than the filesystem.
 *
 * A plain file open sees only loose files. That is half the game: Bethesda
 * ships almost every texture inside a BSA, so a path that is perfectly valid -
 * and that the game itself renders every day - resolves to nothing unless some
 * mod happens to have unpacked it. The snow shell had that hole from the start
 * and never showed it, because anyone running this feature is also running a
 * snow retexture with loose files. The frost pattern found it immediately: its
 * default is a vanilla landscape texture that lives in Skyrim - Textures5.bsa.
 *
 * BSResourceNiBinaryStream is the game's own lookup, so it reads loose files
 * AND archives, and a modlist's override still wins exactly as it does in
 * game. The filesystem path stays as a fallback for anything outside Data.
 */
bool SnowDeformation::LoadGameDDS(const std::string& a_dataRelativePath, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv)
{
	a_srv = nullptr;
	if (a_dataRelativePath.empty())
		return false;

	// "Textures\Effects\shockbolttile01" is how anyone would write the path,
	// and the resource system wants the extension. Supplying it is kinder than
	// silently loading nothing and leaving the user to guess which half of the
	// path was wrong.
	std::string path = a_dataRelativePath;
	{
		const size_t dot = path.find_last_of('.');
		const size_t sep = path.find_last_of("\\/");
		// No dot at all, or the only dot is in a folder name: npos compares
		// LARGER than any real index, so the two cases cannot share one test.
		if (dot == std::string::npos || (sep != std::string::npos && dot < sep))
			path += ".dds";
	}

	RE::BSResourceNiBinaryStream stream(path);
	if (stream.good() && stream.stream) {
		// The stream reports the size the CONSUMER sees, so a compressed BSA
		// entry gives its uncompressed length here and decompresses as it is
		// read. binary_read is protected; the templated read is the way in.
		const uint32_t total = stream.stream->totalSize;
		if (total > 0) {
			std::vector<uint8_t> bytes(total);
			if (stream.read(reinterpret_cast<char*>(bytes.data()), total) &&
				SUCCEEDED(DirectX::CreateDDSTextureFromMemory(globals::d3d::device, bytes.data(), bytes.size(),
					nullptr, a_srv.put()))) {
				logger::debug("SnowDeformation: loaded {} through the game ({} bytes)", a_dataRelativePath, total);
				return true;
			}
		}
		a_srv = nullptr;
	}

	// Fallback: a real path on disk, for anything the resource system does not
	// own. Loose files already came back above, so this is only ever the
	// unusual case.
	const std::string diskPath = "Data\\" + path;
	const std::wstring wide(diskPath.begin(), diskPath.end());
	if (SUCCEEDED(DirectX::CreateDDSTextureFromFile(globals::d3d::device, wide.c_str(), nullptr, a_srv.put()))) {
		logger::debug("SnowDeformation: loaded {} off disk", path);
		return true;
	}
	a_srv = nullptr;
	logger::debug("SnowDeformation: could not load {} from archives or disk", a_dataRelativePath);
	return false;
}

void SnowDeformation::EnsureFrostPatternTextures()
{
	if (frostPatternAttempted)
		return;
	frostPatternAttempted = true;

	// A TILEABLE surface, not a decal. The first attempt reached for the game's
	// own frost impact art, which was the wrong kind of image: a decal carries
	// its content in the middle and nothing at the edges, because it is printed
	// once. Two offset copies of that blended together give clumps with gaps,
	// which is precisely what it looked like. A landscape texture meets itself
	// on every side, which is the thing the stochastic sampler assumes.
	std::string chosen = settings.FrostTexturePath;
	for (auto& pathChar : chosen)
		if (pathChar == '/')
			pathChar = '\\';
	const std::string base = chosen.size() > 4 ? chosen.substr(0, chosen.size() - 4) : chosen;

	// The normal is what actually matters - it carries the crystal - so the
	// pattern counts as absent without one and the crust falls back to the
	// smooth glaze it had before any of this.
	if (!LoadGameDDS(base + "_n.dds", frostPatternNormalSRV)) {
		logger::debug("SnowDeformation: no frost pattern normal at {}_n.dds; crust keeps its smooth glaze", base);
		return;
	}
	LoadGameDDS(chosen, frostPatternDiffuseSRV);
}

void SnowDeformation::EnsureShellSnowTextures()
{
	if (shellSnowTextureAttempted)
		return;
	shellSnowTextureAttempted = true;
	shellSnowDiffuseSRV = nullptr;
	shellSnowNormalSRV = nullptr;
	shellSnowRmaosSRV = nullptr;
	shellSnowHeightSRV = nullptr;
	shellSnowTextureIsPBR = false;

	// Through the game's own lookup, so a snow texture that lives only in a BSA
	// loads like any other. This path had the same hole the frost pattern
	// exposed; it simply never showed, because a modlist that wants deep snow
	// invariably ships a loose snow retexture on top of the vanilla one.
	auto tryLoadDDS = [](const std::string& a_path, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv) {
		return LoadGameDDS(a_path, a_srv);
	};

	std::string chosenPath = settings.SnowTexturePath.empty() ? "Textures\\Landscape\\snow01.dds" : settings.SnowTexturePath;
	for (auto& pathChar : chosenPath)
		if (pathChar == '/')
			pathChar = '\\';
	std::string loweredPath = chosenPath;
	std::transform(loweredPath.begin(), loweredPath.end(), loweredPath.begin(),
		[](unsigned char c) { return (char)std::tolower(c); });

	std::string pbrPath;
	if (loweredPath.find("\\pbr\\") != std::string::npos || loweredPath.rfind("pbr\\", 0) == 0)
		pbrPath = chosenPath;
	else if (size_t texPos = loweredPath.find("textures\\"); texPos != std::string::npos)
		pbrPath = chosenPath.substr(0, texPos + 9) + "PBR\\" + chosenPath.substr(texPos + 9);

	if (!pbrPath.empty() && pbrPath.size() > 4 && tryLoadDDS(pbrPath, shellSnowDiffuseSRV)) {
		shellSnowTextureIsPBR = true;
		std::string base = pbrPath.substr(0, pbrPath.size() - 4);
		bool hasNormal = tryLoadDDS(base + "_n.dds", shellSnowNormalSRV);
		bool hasRmaos = tryLoadDDS(base + "_rmaos.dds", shellSnowRmaosSRV);
		bool hasHeight = tryLoadDDS(base + "_p.dds", shellSnowHeightSRV);
		logger::info("[SNOW DEFORMATION] PBR snow set: {} (normal={} rmaos={} height={})", pbrPath, hasNormal, hasRmaos, hasHeight);

		snowGlintLogDensity = 6.0f;
		snowGlintMicroRoughness = 0.3f;
		snowGlintDensityRandomization = 5.0f;
		snowGlintScreenSpaceScale = 1.0f;
		snowRoughnessScale = 0.7f;
		snowSpecularLevel = 0.02f;
		snowDisplacementScale = 1.0f;
		snowPBRSetName.clear();

		// TruePBR already parsed every PBRTextureSets JSON into its table, keyed
		// by filename stem (the texture-set editor ID) - the same string the old
		// directory scan matched against. Match the keys instead: shared struct,
		// shared parse, and ReloadTextureSetData/menu edits reach us through
		// RefreshSnowPBRParams. Exact key first, then the lexicographically
		// smallest substring match so multi-match modlists resolve the same way
		// every run (the old filesystem-order scan did not).
		size_t slashPos = base.find_last_of('\\');
		std::string baseName = (slashPos == std::string::npos) ? base : base.substr(slashPos + 1);
		std::transform(baseName.begin(), baseName.end(), baseName.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		const auto& pbrSets = globals::features::truePBR.pbrTextureSets;
		size_t matchCount = 0;
		for (const auto& [setName, setData] : pbrSets) {
			std::string keyLower = setName;
			std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			if (keyLower.find(baseName) == std::string::npos)
				continue;
			++matchCount;
			if (keyLower == baseName) {
				snowPBRSetName = setName;
				break;
			}
			if (snowPBRSetName.empty() || setName < snowPBRSetName)
				snowPBRSetName = setName;
		}
		if (!snowPBRSetName.empty()) {
			RefreshSnowPBRParams();
			logger::info("[SNOW DEFORMATION] TruePBR config matched: {} ({} of {} sets matched '{}'; glintDensity={:.1f} roughScale={:.2f} spec={:.3f})",
				snowPBRSetName, matchCount, pbrSets.size(), baseName, snowGlintLogDensity, snowRoughnessScale, snowSpecularLevel);
		} else {
			logger::info("[SNOW DEFORMATION] no TruePBR config matches '{}' ({} sets loaded); using built-in snow defaults",
				baseName, pbrSets.size());
		}
	} else {
		snowPBRSetName.clear();
		bool ok = tryLoadDDS(chosenPath, shellSnowDiffuseSRV);
		if (!ok && chosenPath != "Textures\\Landscape\\snow01.dds") {
			logger::info("[SNOW DEFORMATION] Snow diffuse not loose-file loadable: {}", chosenPath);
			chosenPath = "Textures\\Landscape\\snow01.dds";
			ok = tryLoadDDS(chosenPath, shellSnowDiffuseSRV);
		}
		logger::info("[SNOW DEFORMATION] Snow diffuse load ({}): {}", ok ? "ok (legacy)" : "missing, using fallback color", chosenPath);
	}
}

void SnowDeformation::RefreshSnowPBRParams()
{
	if (snowPBRSetName.empty())
		return;
	const auto& pbrSets = globals::features::truePBR.pbrTextureSets;
	auto it = pbrSets.find(snowPBRSetName);
	if (it == pbrSets.end())
		return;
	const auto& cfg = it->second;
	snowRoughnessScale = cfg.roughnessScale;
	snowSpecularLevel = cfg.specularLevel;
	snowDisplacementScale = cfg.displacementScale;
	if (cfg.glintParameters.enabled) {
		snowGlintLogDensity = cfg.glintParameters.logMicrofacetDensity;
		// Same clamps Lighting.hlsl applies (PBR::Constants).
		snowGlintMicroRoughness = std::clamp(cfg.glintParameters.microfacetRoughness, 0.005f, 0.3f);
		snowGlintDensityRandomization = std::clamp(cfg.glintParameters.densityRandomization, 0.0f, 5.0f);
		snowGlintScreenSpaceScale = std::max(1.0f, cfg.glintParameters.screenSpaceScale);
	} else {
		snowGlintLogDensity = 0.0f;  // below the shader's >1.1 gate
	}
}

ID3D11VertexShader* SnowDeformation::GetShellVS()
{
	if (!shellVS) {
		logger::debug("Compiling SnowShell VS");
		shellVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}
	return shellVS;
}

ID3D11VertexShader* SnowDeformation::GetShellShadowVS()
{
	if (!shellShadowVS) {
		logger::debug("Compiling SnowShell shadow-cast VS");
		shellShadowVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_SHADOW_CAST", "" } }, "vs_5_0"));
	}
	return shellShadowVS;
}

ID3D11PixelShader* SnowDeformation::GetShellPS()
{
	if (!shellPS) {
		logger::debug("Compiling SnowShell PS");
		auto defines = ShellPSDefines();
		shellPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return shellPS;
}

// EHF sun attenuation only compiles when the addon is installed (its hlsli
// is not CORE); SnowShading.hlsli gates on the define.
std::vector<std::pair<const char*, const char*>> SnowDeformation::ShellPSDefines(const char* a_extra)
{
	std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
	if (a_extra)
		defines.emplace_back(a_extra, "");
	if (globals::features::exponentialHeightFog.loaded)
		defines.emplace_back("SNOW_EXP_HEIGHT_FOG", "");
	if (globals::features::ibl.loaded)
		defines.emplace_back("SNOW_IBL", "");
	return defines;
}

ID3D11PixelShader* SnowDeformation::GetShellLODPS()
{
	// Single-SV_Target permutation: SM5.0 shares PS UAV slots with the
	// render-target outputs, so the histogram UAV (u1) requires dropping the
	// G-buffer down to kMAIN.
	if (!shellLODPS) {
		logger::debug("Compiling SnowShell LOD heatmap PS");
		auto defines = ShellPSDefines("SNOW_LOD_HISTOGRAM");
		shellLODPS = static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", defines, "ps_5_0"));
	}
	return shellLODPS;
}

ID3D11VertexShader* SnowDeformation::GetShellTessVS()
{
	if (!shellTessVS) {
		logger::debug("Compiling SnowShell tess control-point VS");
		shellTessVS = static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", { { "VSHADER", "" }, { "SNOW_TESS", "" } }, "vs_5_0"));
	}
	return shellTessVS;
}

ID3D11HullShader* SnowDeformation::GetShellHS()
{
	if (!shellHS) {
		logger::debug("Compiling SnowShell HS");
		shellHS = static_cast<ID3D11HullShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", {}, "hs_5_0"));
	}
	return shellHS;
}

ID3D11DomainShader* SnowDeformation::GetShellDS()
{
	if (!shellDS) {
		logger::debug("Compiling SnowShell DS");
		shellDS = static_cast<ID3D11DomainShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", {}, "ds_5_0"));
	}
	return shellDS;
}

ID3D11ComputeShader* SnowDeformation::GetDepthSyncCS()
{
	if (!depthSyncCS) {
		logger::debug("Compiling DepthSyncCS");
		depthSyncCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0"));
	}
	return depthSyncCS;
}

void SnowDeformation::DrawShell()
{
	if (!settings.EnableSnowDeformation)
		return;

	if (!globals::state->inWorld)
		return;

	auto vs = GetShellVS();
	auto ps = GetShellPS();
	if (!vs || !ps)
		return;

	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& fb = globals::game::frameBufferCached;

	ShellCB cbData{};
	cbData.CameraViewProj = fb.GetCameraViewProj();
	cbData.CameraViewProjUnjittered = fb.GetCameraViewProjUnjittered();
	cbData.CameraPreviousViewProjUnjittered = fb.GetCameraPreviousViewProjUnjittered();
	cbData.CameraView = fb.GetCameraView();
	cbData.CameraPosAdjust = fb.GetCameraPosAdjust();
	cbData.CameraPreviousPosAdjust = fb.GetCameraPreviousPosAdjust();

	// Spacing is FIXED at 8 units: the shell ends at the loaded-cell seam
	// (ShellEdgeFade), so range no longer scales density. Tighter spacing
	// was measured to EXPLODE cost (sub-pixel triangles near the camera:
	// 94 m range = 2-unit triangles = 4.2 ms Shell pass vs 1.1 ms at 8).
	const float shellSpacing = kShellGridSpacing;
	cbData.GridSpacing = shellSpacing;
	cbData.GridDim = kShellGridDim;
	// The warped grid is camera-centered: snap the center to the grid step
	// so inner vertices stay texel-stable, then offset by the warped span.
	const float warpedHalfSpan = ShellWarpedHalfSpan(shellSpacing);
	cbData.WarpedHalfSpan = warpedHalfSpan;
	cbData.GridOrigin = {
		std::floor(cbData.CameraPosAdjust.x / kShellGridSpacing) * kShellGridSpacing - warpedHalfSpan,
		std::floor(cbData.CameraPosAdjust.y / kShellGridSpacing) * kShellGridSpacing - warpedHalfSpan
	};

	cbData.TerrainTexelSize = kShellVertexSpacing;
	cbData.TerrainDim = kShellWindowDim;
	cbData.ShellDebugData = shellDataDebug ? 1u : (shellExclusionDebug ? 2u : 0u);
	cbData.ShellLODDebug = (uint32_t)std::clamp(lodDebugView, 0, 3);
	cbData.StaticsDebugView = float(staticsDebugView);

	// Loaded-cell boundary square around the PLAYER's cell (cell attachment
	// follows the player, not the camera): full terrain inside, LOD outside.
	// The shell's edge fade anchors here so it hands off to the horizon
	// recolor exactly where the game swaps terrain for LOD meshes.
	cbData.SeamRampInv = 0.0f;
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		static const int uGrids = [] {
			if (auto* ini = RE::INISettingCollection::GetSingleton())
				if (auto* setting = ini->GetSetting("uGridsToLoad:General"))
					return std::max((int)setting->GetInteger(), 3);
			return 5;
		}();
		const auto playerPos = player->GetPosition();
		const int cellX = (int)std::floor(playerPos.x / 4096.0f);
		const int cellY = (int)std::floor(playerPos.y / 4096.0f);
		const int halfCells = (uGrids - 1) / 2;
		// The fade band sits ~29 m OUTSIDE the true boundary, over the
		// recolored LOD terrain: full-height shell up to the seam, melting
		// into same-material ground beyond it. Ending the fade AT the seam
		// left a visible unshelled strip of full terrain just inside it.
		constexpr float kSeamOverlap = 2048.0f;
		cbData.SeamBounds = { (cellX - halfCells) * 4096.0f - kSeamOverlap, (cellY - halfCells) * 4096.0f - kSeamOverlap,
			(cellX + halfCells + 1) * 4096.0f + kSeamOverlap, (cellY + halfCells + 1) * 4096.0f + kSeamOverlap };
		cbData.SeamRampInv = 1.0f / 2048.0f;
	}
	cbData.DeformInvWorldSize = 1.0f / deformWorldSize;

	// Keep shader-side sampling math in small grid-local coordinates.
	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	cbData.GridToTerrainOffset = {
		cbData.GridOrigin.x - shellWindowCellX * cellSize,
		cbData.GridOrigin.y - shellWindowCellY * cellSize
	};
	cbData.GridToDeformOffset = {
		cbData.GridOrigin.x - windowOrigin.x,
		cbData.GridOrigin.y - windowOrigin.y
	};

	// Snow uv offset folded to the tile period, so shader-side uv math stays
	// in small numbers. Must match kSnowUVTile in SnowShell.hlsl.
	constexpr float kSnowUVTile = 4096.0f / 24.0f;
	cbData.SnowUVOffset = {
		std::fmod(cbData.GridOrigin.x, kSnowUVTile),
		std::fmod(cbData.GridOrigin.y, kSnowUVTile)
	};

	EnsureShellSnowTextures();
	// Per frame so TruePBR's hot-reload and live menu edits of the matched
	// texture set reach the shell the same frame they reach the ground.
	RefreshSnowPBRParams();
	cbData.HasSnowTexture = shellSnowDiffuseSRV != nullptr;
	cbData.SnowTextureIsLinear = (shellSnowTextureIsPBR || settings.SnowTextureLinear) ? 1.0f : 0.0f;
	cbData.HasSnowHeight = shellSnowHeightSRV ? 1.0f : 0.0f;
	// Tessellated relief amplitude, straight from the slider (world units;
	// the PBR config's displacementScale is deliberately not multiplied in,
	// the slider is authoritative).
	cbData.SnowReliefDepth = std::max(settings.ReliefDepth, 0.0f);
	// Parallax. HeightScale is the PBR config's displacementScale verbatim:
	// kSnowUVTile now equals the game's landscape tiling, so the UV-space slab
	// depth Extended Materials derives from it lands on the same world depth
	// the ground beside us gets. No correction factor.
	cbData.SnowParallax = {
		snowDisplacementScale,
		std::clamp(settings.ParallaxShadowStrength, 0.0f, 2.0f),
		std::clamp(settings.ParallaxDepth, 0.0f, 2.0f),
		(float)std::clamp(settings.ParallaxSteps, 4, 16)
	};
	cbData.SpellShading = { std::max(settings.ScorchStrength, 0.0f),
		std::clamp(settings.CrustGloss, 0.0f, 1.0f),
		std::clamp(settings.CrustRoughness, 0.02f, 0.6f),
		std::clamp(settings.CrustNormalFlatten, 0.0f, 1.0f) };
	cbData.CrustLook = { std::clamp(settings.CrustSpecular, 0.0f, 0.5f),
		settings.CrustTint[0], settings.CrustTint[1],
		std::max(settings.CrustSheen, 0.0f) };
	// yzw ride spare room already in this row rather than growing the shared
	// constant buffer, which is hand-mirrored across two shaders and is the
	// silent collision CLAUDE.md warns about.
	cbData.CrustLook2 = { settings.CrustTint[2],
		std::max(settings.FrostPatternStrength, 0.0f),
		std::max(settings.FrostPatternScale, 4.0f),
		frostPatternNormalSRV ? 1.0f : 0.0f };
	cbData.BermHeightAmp = std::clamp(settings.BermHeight, 0.0f, 1.0f);
	cbData.ChurnHeightAmp = std::clamp(settings.ChurnHeight, 0.0f, 8.0f);
	cbData.ChurnSizeScale = std::clamp(settings.ChurnSize, 0.25f, 4.0f);
	cbData.CrispScaleV = std::clamp(settings.CrispScale, 1.0f, 8.0f);
	cbData.CrispStrengthV = std::clamp(settings.CrispStrength, 0.0f, 3.0f);
	cbData.ObjBermHeightAmp = std::clamp(settings.ObjBermHeight, 0.0f, 1.0f);
	cbData.ObjChurnHeightAmp = std::clamp(settings.ObjChurnHeight, 0.0f, 8.0f);
	cbData.ObjChurnSizeScale = std::clamp(settings.ObjChurnSize, 0.25f, 4.0f);
	cbData.ObjCrispScaleV = std::clamp(settings.ObjCrispScale, 1.0f, 8.0f);
	cbData.ObjCrispStrengthV = std::clamp(settings.ObjCrispStrength, 0.0f, 3.0f);
	cbData.HasSnowNormal = shellSnowNormalSRV ? 1.0f : 0.0f;
	cbData.HasSnowRmaos = shellSnowRmaosSRV ? 1.0f : 0.0f;
	cbData.SnowRoughnessScale = snowRoughnessScale;
	cbData.SnowGlintParams = { snowGlintLogDensity, snowGlintMicroRoughness, snowGlintDensityRandomization, snowGlintScreenSpaceScale };
	cbData.SnowSpecularLevel = snowSpecularLevel;
	// Sparkle: the shell's specular runs TruePBR's glint NDF when its shared
	// noise texture exists (bound to t20 below for the whole pass).
	cbData.EnableGlints = globals::features::truePBR.glintsNoiseTexture ? 1.0f : 0.0f;
	cbData.BorderNoise = settings.SnowBorderNoise;
	cbData.BorderSmooth = settings.SnowBorderSmoothness;
	cbData.BorderStyle = { settings.SnowBorderDithering ? 1.0f : 0.0f,
		std::clamp(settings.TrenchFloorHeight, 0.0f, 8.0f), 0.0f, 0.0f };
	cbData.BorderTrampledFade = settings.SnowBorderTrampledFade;
	cbData.BorderUntrampledFade = settings.SnowBorderUntrampledFade;
	cbData.SnowSnowFade = settings.SnowSnowFade;
	// Statics-skin distance dissolve: starts at the blend slider, fully gone
	// at the Object Snow capture range (floored one meter past the start so
	// the smoothstep never degenerates when the sliders cross).
	cbData.SkinFadeStart = settings.RangeSkinsFadeM * kUnitsPerMeter;
	cbData.SkinFadeEnd = std::max(settings.RangeSkinsM * kUnitsPerMeter, cbData.SkinFadeStart + kUnitsPerMeter);
	// Field enable gate + window addressing for the t4/t5 samplers; the
	// center is re-uploaded below once the height pass has recentered.
	cbData.ObjectLiftCap = kObjectLiftCap;
	cbData.ObjectHeightCenter = heightWindowCenter;
	cbData.ObjectHeightHalfExtent = kHeightMapHalfExtent;

	// Crisp shadows: full-resolution comparison PCF against the cascade-atlas
	// copies taken at the shadow-mask pass. When the copies are missing this
	// frame, the shader falls back to the blurred VSM path.
	cbData.CrispShadows = (shadowAtlasCopySRV && shadowEsramCopySRV) ? 1.0f : 0.0f;
	// Screen-Space Shadows availability: the feature clears its texture to
	// WHITE every Prepass even when disabled, so multiplying is always safe
	// once the texture exists.
	auto& screenSpaceShadowsFeature = globals::features::screenSpaceShadows;
	cbData.ScreenSpaceShadowsActive = (screenSpaceShadowsFeature.loaded && screenSpaceShadowsFeature.screenSpaceShadowsTexture) ? 1.0f : 0.0f;

	cbData.UndulationAmp = std::max(settings.UndulationStrength, 0.0f);
	cbData.UndulationScale = std::max(settings.UndulationSpacing, 0.05f);
	cbData.TrenchFloorFade = std::clamp(settings.TrenchFloorFade, 0.0f, 1.0f);
	// The bake is only usable once its texture exists AND Prepass has filled it
	// this frame; the A/B toggle suppresses both together.
	cbData.BermBakeActive = (!shellBermBakeDisabled && bermFieldTexture) ? 1.0f : 0.0f;
	// Wide exclusion field window: filled provisionally here and RE-UPLOADED
	// below, after the height pass has rebaked the field and moved its centre.
	// Sampling this frame's texture through last frame's centre offsets the
	// whole field by a texel whenever the camera advances one, which reads as
	// clearing depth (and the berm riding on it) twitching while the camera
	// moves and settling when it stops. w gates the sampler off until the bake
	// has actually run.
	cbData.ExclusionFieldWindow = { exclusionFieldCenter.x, exclusionFieldCenter.y,
		1.0f / kExclusionFieldHalfExtent,
		(exclusionFieldValid && !shellDistantExclusionsDisabled) ? 1.0f : 0.0f };

	// Point lights: LLF's clustered visible-light list. The cluster buffers
	// are only coherent when LLF ran this frame (CORE, but boot-disableable).
	auto& lightLimitFix = globals::features::lightLimitFix;
	cbData.PointLightsActive = (lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightIndexList && lightLimitFix.lightGrid) ? 1.0f : 0.0f;
	// Skylighting probe volume for ambient parity with terrain shading.
	auto& skylighting = globals::features::skylighting;
	cbData.SkylightingActive = (skylighting.loaded && skylighting.texProbeArray) ? 1.0f : 0.0f;

	// Shadow-source diagnostics for the settings UI.
	dbgLodDescriptorCount = 0;
	if (auto* shadowSceneNode = globals::game::smState->shadowSceneNode[0]) {
		if (auto* sunShadowLight = shadowSceneNode->GetRuntimeData().sunShadowDirLight) {
			auto& dirLightData = sunShadowLight->GetShadowDirectionalLightRuntimeData();
			dbgLodDescriptorCount = (uint32_t)sunShadowLight->GetRuntimeData().shadowmapDescriptors.size();
			dbgLodEndSplits[0] = dirLightData.endSplitDistances[0];
			dbgLodEndSplits[1] = dirLightData.endSplitDistances[1];
			dbgLodEndSplits[2] = dirLightData.endSplitDistances[2];
		}
	}

	shellCB->Update(cbData);

	// Back up the pipeline state we touch so the composite and later game
	// passes see exactly what they expect.
	winrt::com_ptr<ID3D11RasterizerState> prevRaster;
	winrt::com_ptr<ID3D11DepthStencilState> prevDepth;
	winrt::com_ptr<ID3D11BlendState> prevBlend;
	UINT prevStencilRef = 0;
	FLOAT prevBlendFactor[4]{};
	UINT prevSampleMask = 0xFFFFFFFF;
	D3D11_PRIMITIVE_TOPOLOGY prevTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	context->RSGetState(prevRaster.put());
	context->OMGetDepthStencilState(prevDepth.put(), &prevStencilRef);
	context->OMGetBlendState(prevBlend.put(), prevBlendFactor, &prevSampleMask);
	context->IAGetPrimitiveTopology(&prevTopology);
	D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&prevViewportCount, prevViewports);

	// Rasterize this frame's captured statics top-down into the object
	// height windows, then restore the viewport for the screen-space passes.
	if (EnsureStaticsShaders())
		RenderObjectHeightMap();
	if (prevViewportCount)
		context->RSSetViewports(prevViewportCount, prevViewports);

	// The height pass recentered its window after the shell CB was filled;
	// sampling the freshly scrolled maps with last frame's center makes the
	// whole field trail the camera by one frame of movement. Re-upload with
	// the current center. (Any CPU value consumed by both a constant buffer
	// and a same-frame-scrolled texture must be uploaded after the scroll.)
	cbData.ObjectHeightCenter = heightWindowCenter;
	cbData.ExclusionFieldWindow = { exclusionFieldCenter.x, exclusionFieldCenter.y,
		1.0f / kExclusionFieldHalfExtent,
		(exclusionFieldValid && !shellDistantExclusionsDisabled) ? 1.0f : 0.0f };
	shellCB->Update(cbData);

	// Snapshot for next frame's shadow-caster injection (it runs at the
	// shadow-mask pass, before DrawShell recomputes these values).
	if (!lastShellCBData)
		lastShellCBData = std::make_unique<ShellCB>();
	*lastShellCBData = cbData;

	// Pre-shell copy of MASKS: Masks.y carries the land's EM grain height
	// (Lighting.hlsl LANDSCAPE; 0 = no data), and the land under the shell
	// is only readable before the shell overwrites the G-buffer. Unbind
	// around the copy; our own targets are set immediately after.
	{
		auto& masksRT = renderer->GetRuntimeData().renderTargets[MASKS];
		if (masksRT.SRV) {
			context->OMSetRenderTargets(0, nullptr, nullptr);
			CopySRVResource(masksRT.SRV, "SnowDeformation::LandMasksCopy", landMasksCopyTex, landMasksCopySRV);
		}
	}

	// Bind the deferred G-buffer exactly as StartDeferred configures it,
	// plus the main depth buffer for correct intersection with the world.
	auto& rtData = renderer->GetRuntimeData();
	ID3D11RenderTargetView* rtvs[8] = {
		rtData.renderTargets[RE::RENDER_TARGETS::kMAIN].RTV,
		rtData.renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR].RTV,
		rtData.renderTargets[NORMALROUGHNESS].RTV,
		rtData.renderTargets[ALBEDO].RTV,
		rtData.renderTargets[SPECULAR].RTV,
		rtData.renderTargets[REFLECTANCE].RTV,
		rtData.renderTargets[MASKS].RTV,
		rtData.renderTargets[MASKS2].RTV
	};
	auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
	context->OMSetRenderTargets(8, rtvs, dsv);

	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->RSSetState(shellRasterState.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

	// Heatmap mode: depth test ALWAYS + no write (the PS re-creates
	// occlusion) so poke-under pixels survive to be measured, and the
	// G-buffer shrinks to kMAIN so the histogram UAV fits under the FL11.0
	// 8-slot RTV+UAV limit.
	const bool lodHeatmap = lodDebugView == 1 && EnsureLODDebugResources() && GetShellLODPS();
	if (lodHeatmap) {
		const UINT histClear[4] = {};
		context->ClearUnorderedAccessViewUint(lodHistogramUAV.get(), histClear);
		ID3D11UnorderedAccessView* histUAV = lodHistogramUAV.get();
		context->OMSetRenderTargetsAndUnorderedAccessViews(1, rtvs, dsv, 1, 1, &histUAV, nullptr);
		context->OMSetDepthStencilState(shellLODDepthState.get(), 0);
		ps = shellLODPS;
	}

	ID3D11Buffer* cbs[1] = { shellCB->CB() };
	context->VSSetConstantBuffers(0, 1, cbs);
	context->PSSetConstantBuffers(0, 1, cbs);
	// SharedData (b5) supplies SH ambient + sun for the PS lighting; rebind
	// the b4-b6 triple exactly as Deferred does for its own passes.
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);
	// The PS evaluates the terrain/deformation fields for per-pixel coverage
	// and normals, so the field textures must be bound to both stages; the
	// snow maps and scene depth are PS-only. The depth SRV is a copy (Terrain
	// Blending's blended depth when available), never the bound DSV, so
	// sampling it here is legal; the PS fades the shell where it hovers close
	// in front of any geometry so it dissolves into statics (walkways, mesh
	// roads, rocks).
	ID3D11ShaderResourceView* shellSRVs[9] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), shellSnowDiffuseSRV.get(), Util::GetCurrentSceneDepthSRV(false), heightTopFiltered->srv.get(), heightBottomFiltered->srv.get(), shellSnowNormalSRV.get(), shellSnowRmaosSRV.get(), shellSnowHeightSRV.get() };
	context->VSSetShaderResources(0, 6, shellSRVs);
	context->PSSetShaderResources(0, 9, shellSRVs);
	// Raw object tops + skin-depth raster (t11/t12, shared with the trench
	// patch): the object-depth cap on the shell's layer.
	ID3D11ShaderResourceView* objectCapSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
	context->VSSetShaderResources(11, 2, objectCapSRVs);
	context->PSSetShaderResources(11, 2, objectCapSRVs);
	// Land grain height copy (t10) for the two-sided edge contest.
	if (landMasksCopySRV) {
		ID3D11ShaderResourceView* landMasksSRV = landMasksCopySRV.get();
		context->PSSetShaderResources(10, 1, &landMasksSRV);
	}
	// Baked berm field (t14). The berm is read by the surface evaluation
	// (VS/DS) and by the normal block (PS), so every stage that can run
	// ShellSurfaceZ or shade needs it.
	ID3D11ShaderResourceView* bermSRV = GetBermFieldSRV();
	context->VSSetShaderResources(14, 1, &bermSRV);
	context->PSSetShaderResources(14, 1, &bermSRV);

	EnsureFrostPatternTextures();
	ID3D11ShaderResourceView* frostSRVs[] = { frostPatternNormalSRV.get(), frostPatternDiffuseSRV.get() };
	context->PSSetShaderResources(16, ARRAYSIZE(frostSRVs), frostSRVs);
	// Wide exclusion field (t15): read by the surface evaluation and by the
	// pixel-side melt overrides, so the same stages as the berm.
	ID3D11ShaderResourceView* exclusionSRV = GetExclusionFieldSRV();
	context->VSSetShaderResources(15, 1, &exclusionSRV);
	context->PSSetShaderResources(15, 1, &exclusionSRV);
	// Glint noise (t20): TruePBR binds this each prepass, but slot 20's state
	// at deferred time is not guaranteed; bind explicitly for this pass.
	if (globals::features::truePBR.glintsNoiseTexture) {
		ID3D11ShaderResourceView* glintSRV = globals::features::truePBR.glintsNoiseTexture->srv.get();
		context->PSSetShaderResources(20, 1, &glintSRV);
	}
	// IBL SH textures (t76/t77, the include's forward slots): same not-
	// guaranteed-at-deferred-time reasoning as the glint noise above.
	if (globals::features::ibl.loaded && globals::features::ibl.envIBLTexture && globals::features::ibl.skyIBLTexture) {
		ID3D11ShaderResourceView* iblSRVs[2] = { globals::features::ibl.envIBLTexture->srv.get(), globals::features::ibl.skyIBLTexture->srv.get() };
		context->PSSetShaderResources(76, 2, iblSRVs);
	}
	// Comparison sampler (s2), shared by the crisp cascade path and the
	// point-light shadow path.
	if (cbData.CrispShadows > 0.5f || pointShadowAtlasCopySRV) {
		if (!shadowCmpSampler) {
			D3D11_SAMPLER_DESC cmpDesc{};
			cmpDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			cmpDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			cmpDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
			cmpDesc.MaxLOD = D3D11_FLOAT32_MAX;
			globals::d3d::device->CreateSamplerState(&cmpDesc, shadowCmpSampler.put());
			Util::SetResourceName(shadowCmpSampler.get(), "SnowDeformation::ShadowCmpSampler");
		}
		ID3D11SamplerState* cmpSampler = shadowCmpSampler.get();
		context->PSSetSamplers(2, 1, &cmpSampler);
	}
	// Raw shadow-atlas copies (t22/t23) for crisp cascade shadows; the
	// statics skin inherits these too.
	if (cbData.CrispShadows > 0.5f) {
		ID3D11ShaderResourceView* shadowSRVs[2] = { shadowAtlasCopySRV.get(), shadowEsramCopySRV.get() };
		context->PSSetShaderResources(22, 2, shadowSRVs);
	}
	// Screen-Space Shadows output (t45) for the shell + statics passes.
	if (cbData.ScreenSpaceShadowsActive > 0.5f) {
		ID3D11ShaderResourceView* sssSRV = screenSpaceShadowsFeature.screenSpaceShadowsTexture->srv.get();
		context->PSSetShaderResources(45, 1, &sssSRV);
	}
	// LLF cluster buffers (t35-t37) + the point-shadow light table (t38) for
	// the shells' point lights. LLF binds t35-37 each Prepass, but slot
	// state at deferred time is not guaranteed; rebind explicitly like t20.
	// Do NOT null t35-37 in teardown: later forward passes (water, effects)
	// read LLF's own binding, which this rebind matches exactly. The shadow
	// maps themselves ride the t22/t23 atlas copies bound above.
	if (cbData.PointLightsActive > 0.5f) {
		ID3D11ShaderResourceView* lightSRVs[3] = { lightLimitFix.lights->srv.get(), lightLimitFix.lightIndexList->srv.get(), lightLimitFix.lightGrid->srv.get() };
		context->PSSetShaderResources(35, 3, lightSRVs);
		UpdatePointShadowLights();
		if (pointShadowLights) {
			ID3D11ShaderResourceView* pointShadowSRVs[2] = { pointShadowLights->srv.get(), pointShadowAtlasCopySRV.get() };
			context->PSSetShaderResources(38, 2, pointShadowSRVs);
		}
	}
	// Skylighting probe volume (t50). Skylighting's own SRV; no teardown
	// needed for the same reason as t35-37.
	if (cbData.SkylightingActive > 0.5f) {
		ID3D11ShaderResourceView* probeSRV = skylighting.texProbeArray->srv.get();
		context->PSSetShaderResources(50, 1, &probeSRV);
	}

	winrt::com_ptr<ID3D11SamplerState> prevSamplers[2];
	context->PSGetSamplers(0, 1, prevSamplers[0].put());
	context->PSGetSamplers(1, 1, prevSamplers[1].put());
	ID3D11SamplerState* shellSamplers[2] = { shellSnowSampler.get(), shellLinearSampler.get() };
	context->PSSetSamplers(0, 2, shellSamplers);

	context->PSSetShader(ps, nullptr, 0);

	// Tessellated path: 4-control-point patches through the hull/domain
	// stages, so near-camera snow renders the deformation map and the PBR
	// displacement relief as real geometry. The domain shader runs the full
	// surface evaluation, so it needs the same field textures and CB the
	// legacy VS reads, plus the height map and its sampler.
	// Gated on Tessellation, NOT on ReliefDepth: the tessellated path's real
	// client is trench smoothness (the hull factors key off the deformation
	// map), and that must survive relief being turned off. With relief at 0
	// the factors collapse to 1 on undeformed ground, so the path stays cheap.
	auto* tessVS = settings.Tessellation ? GetShellTessVS() : nullptr;
	auto* tessHS = settings.Tessellation ? GetShellHS() : nullptr;
	auto* tessDS = settings.Tessellation ? GetShellDS() : nullptr;
	const bool tessellate = tessVS && tessHS && tessDS;
	globals::profiler->BeginPass("SnowDeformation::Shell");
	if (tessellate) {
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
		context->VSSetShader(tessVS, nullptr, 0);
		context->HSSetShader(tessHS, nullptr, 0);
		context->DSSetShader(tessDS, nullptr, 0);
		context->HSSetConstantBuffers(0, 1, cbs);
		context->DSSetConstantBuffers(0, 1, cbs);
		// SharedData (b5): the skirt descent reads the EM height-blending
		// gate; bind the b4-b6 triple exactly as the PS gets it above.
		context->DSSetConstantBuffers(4, 3, sharedBuffers);
		// The hull shader reads the deformation map for trench-aware factors.
		ID3D11ShaderResourceView* hsDeformSRV = GetDeformationSRV();
		context->HSSetShaderResources(1, 1, &hsDeformSRV);
		context->DSSetShaderResources(0, 6, shellSRVs);
		ID3D11ShaderResourceView* dsHeightSRV = shellSnowHeightSRV.get();
		context->DSSetShaderResources(8, 1, &dsHeightSRV);
		context->DSSetShaderResources(11, 2, objectCapSRVs);
		context->DSSetShaderResources(14, 1, &bermSRV);
		context->DSSetShaderResources(15, 1, &exclusionSRV);
		ID3D11SamplerState* dsSampler = shellSnowSampler.get();
		context->DSSetSamplers(0, 1, &dsSampler);
		context->Draw(kShellGridDim * kShellGridDim * 4, 0);
		// The statics pass and everything after run the normal pipeline.
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	} else {
		context->VSSetShader(vs, nullptr, 0);
		context->Draw(kShellGridDim * kShellGridDim * 6, 0);
	}
	globals::profiler->EndPass();

	if (lodHeatmap) {
		// Statics + the depth copy below expect the full G-buffer and the
		// standard depth state; rebinding also unbinds the histogram UAV
		// before its CopyResource.
		context->OMSetRenderTargets(8, rtvs, dsv);
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
		context->CopyResource(lodHistogramStaging[lodReadbackRing].get(), lodHistogram.get());
		lodHistStagingValid[lodReadbackRing] = true;
	}

	// Post-shell depth copy (Terrain Blending's technique adapted): the main
	// depth now contains the landscape shell's surface. The statics skin
	// samples this at t9 to measure its view-ray gap to the shell and cross-
	// fade into it; fading toward what is actually behind the pixel, which
	// a height-based band cannot guarantee (it can expose the bare mesh
	// beneath the skin instead). Targets must be unbound around CopyResource
	// of a bound DSV.
	{
		auto& mainDepthDS = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (mainDepthDS.depthSRV) {
			ID3D11RenderTargetView* boundRTVs[8] = {};
			ID3D11DepthStencilView* boundDSV = nullptr;
			context->OMGetRenderTargets(8, boundRTVs, &boundDSV);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			CopySRVResource(mainDepthDS.depthSRV, "SnowDeformation::ShellDepthCopy", shellDepthCopyTex, shellDepthCopySRV);
			context->OMSetRenderTargets(8, boundRTVs, boundDSV);
			for (auto* rtv : boundRTVs)
				if (rtv)
					rtv->Release();
			if (boundDSV)
				boundDSV->Release();
		}
		if (shellDepthCopySRV) {
			ID3D11ShaderResourceView* copySRV = shellDepthCopySRV.get();
			context->PSSetShaderResources(9, 1, &copySRV);
		}
	}

	// Captured projected-snow statics, inflated with the same material.
	// Inherits this pass's bindings (b0, t0-t9, s0, b4-b6, RTs, depth).
	DrawCapturedStatics();

	// Restore everything we changed. DS/HS state is cleared unconditionally:
	// the statics pass binds its own DS resources even when the landscape
	// tessellation is unavailable, and the deformation map is UAV-written
	// next Prepass so it must not linger on a DS slot.
	{
		ID3D11ShaderResourceView* nullDSSRVs[16] = {};
		context->DSSetShaderResources(0, 16, nullDSSRVs);
		context->HSSetShaderResources(0, 16, nullDSSRVs);
		ID3D11Buffer* nullStageCB = nullptr;
		context->DSSetConstantBuffers(0, 1, &nullStageCB);
		context->HSSetConstantBuffers(0, 1, &nullStageCB);
		ID3D11Buffer* nullSharedCBs[3] = {};
		context->DSSetConstantBuffers(4, 3, nullSharedCBs);
		ID3D11SamplerState* nullDSSampler = nullptr;
		context->DSSetSamplers(0, 1, &nullDSSampler);
	}
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(0, 1, &nullCB);
	context->PSSetConstantBuffers(0, 1, &nullCB);
	ID3D11ShaderResourceView* nullSRVs[16] = {};
	context->VSSetShaderResources(0, 16, nullSRVs);
	context->PSSetShaderResources(0, 16, nullSRVs);
	// t22/t23 hold SRVs of the game's shadow depth targets; they must be
	// unbound before the next shadow render binds those targets as DSVs, or
	// D3D silently drops the binding with warning spam. t20 (glint noise) and
	// t21 are cleared alongside.
	ID3D11ShaderResourceView* nullShadowSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(20, 4, nullShadowSRVs);
	ID3D11SamplerState* restoreSamplers[2] = { prevSamplers[0].get(), prevSamplers[1].get() };
	context->PSSetSamplers(0, 2, restoreSamplers);
	ID3D11SamplerState* nullCmpSampler = nullptr;
	context->PSSetSamplers(2, 1, &nullCmpSampler);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->RSSetState(prevRaster.get());
	context->OMSetDepthStencilState(prevDepth.get(), prevStencilRef);
	context->OMSetBlendState(prevBlend.get(), prevBlendFactor, prevSampleMask);
	context->IASetPrimitiveTopology(prevTopology);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

	RunLODProbePass();

	// Screen-space passes running after us (SSGI) read Terrain Blending's
	// blended depth, finalized during opaque rendering; without a sync they
	// see buried geometry poking through the snow and paint occlusion halos
	// onto the shell. min() the shell's fresh depth into both blended copies
	// (DSV is unbound again at this point).
	auto& tb = globals::features::terrainBlending;
	if (tb.loaded && tb.settings.Enabled && tb.blendedDepthTexture && tb.blendedDepthTexture16) {
		if (auto cs = GetDepthSyncCS()) {
			auto mainDepthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
			ID3D11UnorderedAccessView* syncUAVs[2] = { tb.blendedDepthTexture->uav.get(), tb.blendedDepthTexture16->uav.get() };
			context->CSSetShaderResources(0, 1, &mainDepthSRV);
			context->CSSetUnorderedAccessViews(0, 2, syncUAVs, nullptr);
			context->CSSetShader(cs, nullptr, 0);
			const auto& depthDesc = tb.blendedDepthTexture->desc;
			globals::profiler->BeginPass("SnowDeformation::DepthSync");
			context->Dispatch((depthDesc.Width + 7) / 8, (depthDesc.Height + 7) / 8, 1);
			globals::profiler->EndPass();

			ID3D11ShaderResourceView* nullSyncSRV = nullptr;
			ID3D11UnorderedAccessView* nullSyncUAVs[2] = { nullptr, nullptr };
			context->CSSetShaderResources(0, 1, &nullSyncSRV);
			context->CSSetUnorderedAccessViews(0, 2, nullSyncUAVs, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
		}
	}
}

// ---- Distant-snow / LOD diagnostics ----

bool SnowDeformation::EnsureLODDebugResources()
{
	if (shellLODDepthState && lodHistogram && lodProbeBuffer)
		return true;

	auto device = globals::d3d::device;

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = TRUE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	if (FAILED(device->CreateDepthStencilState(&depthDesc, shellLODDepthState.put())))
		return false;
	Util::SetResourceName(shellLODDepthState.get(), "SnowDeformation::ShellLODDepthState");

	auto makeStructured = [&](uint32_t a_count, uint32_t a_stride, const char* a_name,
							  winrt::com_ptr<ID3D11Buffer>& a_buf, winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav,
							  winrt::com_ptr<ID3D11Buffer>(&a_staging)[2]) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = a_count * a_stride;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = a_stride;
		if (FAILED(device->CreateBuffer(&desc, nullptr, a_buf.put())))
			return false;
		Util::SetResourceName(a_buf.get(), a_name);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = a_count;
		if (FAILED(device->CreateUnorderedAccessView(a_buf.get(), &uavDesc, a_uav.put())))
			return false;

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = a_count * a_stride;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int i = 0; i < 2; ++i)
			if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, a_staging[i].put())))
				return false;
		return true;
	};

	if (!makeStructured(kLODHistBands * kLODHistBuckets, 4, "SnowDeformation::LODHistogram", lodHistogram, lodHistogramUAV, lodHistogramStaging))
		return false;
	if (!makeStructured(kLODProbeCount, 4, "SnowDeformation::LODProbeHeights", lodProbeBuffer, lodProbeUAV, lodProbeStaging))
		return false;
	return true;
}

ID3D11ComputeShader* SnowDeformation::GetLODProbeCS()
{
	if (!lodProbeCS) {
		logger::debug("Compiling SnowShell LOD probe CS");
		lodProbeCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SnowShell.hlsl", {}, "cs_5_0"));
	}
	return lodProbeCS;
}

/** @brief Distance band of a probe/pixel, matching the shader's histogram banding. */
static uint32_t LODBandOfRadius(float a_radius)
{
	return a_radius < 4000.0f ? 0u : (a_radius < 8000.0f ? 1u : (a_radius < 16000.0f ? 2u : 3u));
}

void SnowDeformation::RunLODProbePass()
{
	auto context = globals::d3d::context;

	if (lodShimmerMeter && EnsureLODDebugResources()) {
		if (auto cs = GetLODProbeCS()) {
			ID3D11Buffer* csCB = shellCB->CB();
			context->CSSetConstantBuffers(0, 1, &csCB);
			// ShellSurfaceZ reads the terrain window, deformation map and the
			// object height/shelter/cap fields; t2/t3 (snow diffuse, scene
			// depth) are unused by the surface math.
			ID3D11ShaderResourceView* csSRVs[6] = { shellTerrainTexture->srv.get(), GetDeformationSRV(), nullptr, nullptr, heightTopFiltered->srv.get(), heightBottomFiltered->srv.get() };
			context->CSSetShaderResources(0, 6, csSRVs);
			ID3D11ShaderResourceView* csCapSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
			context->CSSetShaderResources(11, 2, csCapSRVs);
			// ShellSurfaceZ's berm term reads the bake at t14.
			ID3D11ShaderResourceView* csBermSRV = GetBermFieldSRV();
			context->CSSetShaderResources(14, 1, &csBermSRV);
			ID3D11ShaderResourceView* csExclusionSRV = GetExclusionFieldSRV();
			context->CSSetShaderResources(15, 1, &csExclusionSRV);
			ID3D11UnorderedAccessView* probeUAV = lodProbeUAV.get();
			context->CSSetUnorderedAccessViews(0, 1, &probeUAV, nullptr);
			context->CSSetShader(cs, nullptr, 0);
			globals::profiler->BeginPass("SnowDeformation::LODProbes");
			context->Dispatch((kLODProbeCount + 63) / 64, 1, 1);
			globals::profiler->EndPass();

			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			ID3D11ShaderResourceView* nullSRVs[6] = {};
			context->CSSetShaderResources(0, 6, nullSRVs);
			context->CSSetShaderResources(11, 2, nullSRVs);
			context->CSSetShaderResources(14, 1, nullSRVs);
			context->CSSetShaderResources(15, 1, nullSRVs);
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetConstantBuffers(0, 1, &nullCB);
			context->CSSetShader(nullptr, nullptr, 0);

			context->CopyResource(lodProbeStaging[lodReadbackRing].get(), lodProbeBuffer.get());
			lodProbeStagingValid[lodReadbackRing] = true;
			auto eyeFB = globals::game::frameBufferCached.GetCameraPosAdjust();
			lodProbeAnchorAtCopy[lodReadbackRing] = { std::floor(eyeFB.x / 512.0f) * 512.0f, std::floor(eyeFB.y / 512.0f) * 512.0f };
		}
	}

	ReadbackLODDiagnostics();
	lodReadbackRing ^= 1;
}

void SnowDeformation::ReadbackLODDiagnostics()
{
	auto context = globals::d3d::context;
	const int mapRing = lodReadbackRing ^ 1;

	// Both maps target last frame's copies; DO_NOT_WAIT keeps a slow frame
	// from stalling the render thread (the table just lags one more frame).
	if (lodHistStagingValid[mapRing]) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(lodHistogramStaging[mapRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
			std::memcpy(lodHistData, mapped.pData, sizeof(lodHistData));
			context->Unmap(lodHistogramStaging[mapRing].get(), 0);
			lodHistStagingValid[mapRing] = false;
		}
	}

	if (!lodProbeStagingValid[mapRing])
		return;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(lodProbeStaging[mapRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
		return;
	lodProbeStagingValid[mapRing] = false;

	const float* heights = static_cast<const float*>(mapped.pData);
	const float2 anchor = lodProbeAnchorAtCopy[mapRing];
	const bool sameAnchor = lodProbePrevValid && anchor.x == lodProbeAnchor.x && anchor.y == lodProbeAnchor.y;

	float sum[kLODHistBands] = {};
	float mx[kLODHistBands] = {};
	uint32_t hops[kLODHistBands] = {};
	uint32_t cnt[kLODHistBands] = {};
	uint32_t validCnt[kLODHistBands] = {};

	for (uint32_t i = 0; i < kLODProbeCount; ++i) {
		const float h = heights[i];
		const uint32_t band = LODBandOfRadius(kLODProbeRadius[i / kLODProbeAzimuths]);
		const bool nowValid = h < 1.0e37f;
		if (nowValid)
			validCnt[band]++;
		if (sameAnchor && nowValid && lodProbePrev[i] < 1.0e37f) {
			const float d = std::abs(h - lodProbePrev[i]);
			sum[band] += d;
			mx[band] = std::max(mx[band], d);
			if (d > 1.0f)
				hops[band]++;
			cnt[band]++;
		}
		lodProbePrev[i] = h;
	}
	context->Unmap(lodProbeStaging[mapRing].get(), 0);

	for (uint32_t band = 0; band < kLODHistBands; ++band) {
		lodShimmerMax[band] = mx[band];
		lodShimmerAvg[band] = cnt[band] ? sum[band] / cnt[band] : 0.0f;
		lodShimmerHops[band] = hops[band];
		lodShimmerValid[band] = validCnt[band];
		lodShimmerHistoryBuf[band][lodShimmerHistoryIdx] = mx[band];
	}
	lodShimmerHistoryIdx = (lodShimmerHistoryIdx + 1) % kLODShimmerHistory;
	lodProbePrevValid = true;
	lodProbeAnchor = anchor;
}
