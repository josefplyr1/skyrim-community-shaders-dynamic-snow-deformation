#include "Features/SnowDeformation.h"

#include <d3dcompiler.h>

#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

// True if the scenegraph carries a live attached light. A burning torch (held
// or dropped) has a NiPointLight in its 3D; torch-snuffing mods remove it
// while keeping the carryable light base form.
static bool HasActiveLight(RE::NiAVObject* a_obj)
{
	if (!a_obj || a_obj->GetAppCulled())
		return false;
	if (netimmerse_cast<RE::NiPointLight*>(a_obj))
		return true;
	if (auto* node = a_obj->AsNode())
		for (auto& child : node->GetChildren())
			if (HasActiveLight(child.get()))
				return true;
	return false;
}

// Model path of the reference a captured geometry belongs to. The ref hangs off
// the 3D root's user data, so walk up from the drawn trishape.
static std::string CapturedModelPath(RE::NiAVObject* a_object)
{
	for (RE::NiAVObject* node = a_object; node; node = node->parent) {
		if (auto* ref = node->GetUserData()) {
			if (auto* base = ref->GetBaseObject()) {
				if (auto* model = base->As<RE::TESModel>()) {
					if (const char* path = model->GetModel(); path && path[0])
						return path;
				}
				return std::format("{:08X} (no model)", base->GetFormID());
			}
		}
	}
	return "<no reference>";
}

SnowDeformation::ObjectSnowProbe SnowDeformation::ProbeObjectSnow(float a_x, float a_y)
{
	ObjectSnowProbe probe;
	probe.captured = capturedStatics.size();

	for (const auto& capture : capturedStatics) {
		if (!capture.geometry)
			continue;
		const auto& bound = capture.geometry->worldBound;
		const float dx = bound.center.x - a_x;
		const float dy = bound.center.y - a_y;
		const float distXY = std::sqrt(dx * dx + dy * dy);
		if (distXY > bound.radius)
			continue;

		probe.overlapping++;
		probe.entries.push_back({ capture.geometry->name.c_str(),
			CapturedModelPath(capture.geometry.get()),
			bound.center.z, bound.radius, bound.center.z + bound.radius, distXY, capture.road });
	}

	// Largest first: a sheet that covers a courtyard belongs to a big capture.
	std::sort(probe.entries.begin(), probe.entries.end(),
		[](const ObjectSnowProbe::Entry& a, const ObjectSnowProbe::Entry& b) { return a.radius > b.radius; });
	if (probe.entries.size() > 8)
		probe.entries.resize(8);
	return probe;
}

// One-shot samples of the object-LOD decision, so a single launch shows
// whether the containment rule separates Windhelm's sheets from distant
// scenery. Logs a handful of distinct outcomes, then goes quiet; this sits on
// the per-geometry render path and must not become per-frame spam.
// Samples the DIFFUSE path of LOD geometry the material gate drops, deduped by
// path. DynDOLOD emits separate snow-projected and plain LOD batches, so a
// PLAIN batch failing the gate is correct (it never wore projected snow in
// vanilla either) while a SNOW-textured one failing is a bug - and only the
// path tells the two apart. Same single-threaded assumption as
// driftMaterialCache below.
static void SampleMaterialReject(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase* a_material)
{
	static std::unordered_set<std::string> seen;
	if (seen.size() >= 12)
		return;
	auto* textureSet = a_material ? a_material->textureSet.get() : nullptr;
	const char* path = textureSet ? textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
	if (!seen.insert(path ? path : "<no diffuse>").second)
		return;
	logger::info("[SNOW DEFORMATION] LOD dropped by material gate: '{}' ({})",
		path ? path : "<no diffuse>",
		a_geometry->name.empty() ? "<unnamed>" : a_geometry->name.c_str());
}

static void SampleLODDecision(RE::BSGeometry* a_geometry, float a_radius, bool a_rejected, bool a_cameraInside)
{
	static std::atomic<uint32_t> logged{ 0 };
	if (logged.load(std::memory_order_relaxed) >= 8)
		return;
	if (logged.fetch_add(1, std::memory_order_relaxed) >= 8)
		return;
	logger::info("[SNOW DEFORMATION] object LOD {}: radius {:.0f}, cameraInside={}, '{}'",
		a_rejected ? "REJECTED (merged sheet, camera inside)" : "kept",
		a_radius, a_cameraInside ? "yes" : "no",
		a_geometry->name.empty() ? "<unnamed>" : a_geometry->name.c_str());
}
namespace
{
	enum class MatoClass
	{
		kNoReference,
		kNoMato,
		kSnow,
		kNotSnow
	};

	// The reference's STAT directional-material record settles snow vs sand:
	// a MATO's model path IS its projected texture. Cached per base form.
	MatoClass ClassifyProjectedMato(RE::BSGeometry* a_geometry)
	{
		RE::TESObjectREFR* refr = nullptr;
		for (RE::NiAVObject* node = a_geometry; node && !refr; node = node->parent)
			refr = static_cast<RE::TESObjectREFR*>(node->GetUserData());
		if (!refr)
			return MatoClass::kNoReference;
		auto* base = refr->GetBaseObject();
		if (!base)
			return MatoClass::kNoReference;
		static std::unordered_map<RE::FormID, MatoClass> matoClassCache;
		if (matoClassCache.size() > 4096)
			matoClassCache.clear();
		auto [it, inserted] = matoClassCache.try_emplace(base->GetFormID(), MatoClass::kNoMato);
		if (inserted) {
			if (auto* stat = base->As<RE::TESObjectSTAT>(); stat && stat->data.materialObj) {
				std::string matoPath(stat->data.materialObj->GetModel());
				std::transform(matoPath.begin(), matoPath.end(), matoPath.begin(),
					[](unsigned char c) { return (char)std::tolower(c); });
				it->second = matoPath.find("snow") != std::string::npos ? MatoClass::kSnow : MatoClass::kNotSnow;
			}
		}
		return it->second;
	}
}

void SnowDeformation::SetProjectedSnowBit(RE::BSRenderPass* a_pass)
{
	// Projected-snow bit for Lighting's material match (SNOW-MATCH Phase 2):
	// cleared every pass so it never leaks, set when this draw's projected
	// material is actually snow. Flags are trustworthy on full meshes (sand
	// projection sets kProjectedUV without kSnow — the Pale beach evidence);
	// the MATO check guards the flagged path against exceptions whenever a
	// reference is reachable. Runs BEFORE the game's SetupGeometry (the
	// ExtendedTranslucency pattern): the descriptor is consumed inside it.
	auto& extraDescriptor = globals::state->permutationData.ExtraFeatureDescriptor;
	extraDescriptor &= ~uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow);
	if (!a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	const auto& flags = a_pass->shaderProperty->flags;
	if (settings.EnableSnowDeformation && settings.ProjSnowMatch &&
		flags.all(Flag::kProjectedUV) && flags.all(Flag::kSnow) &&
		ClassifyProjectedMato(a_pass->geometry) != MatoClass::kNotSnow)
		extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow);
}

void SnowDeformation::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	// No depth-slider gate: the snow cover must exist at ANY slider values
	// (it replaces the mismatched projected diffuse beneath; sliders only
	// control thickness and trenching).
	if (!settings.EnableSnowDeformation)
		return;
	// Main world view only: probe/reflection passes must not fill the list.
	if (!globals::state->inWorld)
		return;

	// The clean gate: projected-UV + snow flags together; covers rocks,
	// roofs, logs, stumps and never flora, because foliage is not
	// snow-PROJECTED. Drifts (no flags at all) qualify via a NARROW texture
	// match; a catch-all "snow" match drags frosted bushes in, whose leaf
	// cards shard under the skin.
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	const auto& flags = a_pass->shaderProperty->flags;
	// Animated flora never qualifies: card meshes shard under the skin.
	if (flags.all(Flag::kTreeAnim))
		return;
	// Merged LOD spans a whole worldspace quad; nothing belonging to a single
	// reference comes close. Windhelm's merged quads measured 8700-12608.
	constexpr float kMergedLODRadius = 4096.0f;

	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();

	// Merged LOD sheets, discriminated by CONTAINMENT rather than by span.
	//
	// Distant objects are drawn as merged LargeRef LOD batches - the SAME
	// asset class as Windhelm's sheets (objSnow-LargeRef / objSnowHD-LargeRef)
	// at the same sizes - so neither the LOD flags nor worldBound.radius can
	// separate the two. Rejecting on either killed distant object snow
	// outright (RenderDoc pixel history, 2026-08-17: a distant rock was
	// written by objSnowHD-LargeRef DrawIndexed(60276) and NO event from our
	// statics pass ever touched that pixel).
	//
	// What actually separates them is whether the LOD is REDUNDANT. Inside the
	// loaded region the real meshes are drawn too, so a skin on the co-drawn
	// LOD is a second surface at the LOD height cutting through them - the
	// Windhelm sheet. Outside it, the LOD is the object's only representation
	// and must be skinned or distant scenery has no snow. "Am I standing
	// inside the thing" is the test; the unreferenced check stays as the
	// second half, since merged LOD hangs off no TESObjectREFR while a real
	// reference's geometry always has one.
	{
		const auto& wb = a_pass->geometry->worldBound;
		const float bx = wb.center.x - eye.x;
		const float by = wb.center.y - eye.y;
		// Horizontal containment: these sheets are broad and flat, so the
		// footprint is what matters, not the sphere's vertical reach.
		const bool cameraInside = (bx * bx + by * by) < (wb.radius * wb.radius);
		if (cameraInside && wb.radius > kMergedLODRadius) {
			bool referenced = false;
			for (RE::NiAVObject* node = a_pass->geometry; node && !referenced; node = node->parent)
				referenced = node->GetUserData() != nullptr;
			if (!referenced) {
				SampleLODDecision(a_pass->geometry, wb.radius, true, false);
				return;
			}
		}
		if (flags.any(Flag::kLODObjects, Flag::kHDLODObjects, Flag::kLODLandscape))
			SampleLODDecision(a_pass->geometry, wb.radius, false, cameraInside);
	}
	if (!(flags.all(Flag::kProjectedUV) && flags.all(Flag::kSnow))) {
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		if (!material)
			return;

		// Object LOD only. Terrain LOD (kLODLandscape) belongs to the shell and
		// the horizon recolor, never to object snow.
		const bool isObjectLOD = flags.any(Flag::kLODObjects, Flag::kHDLODObjects);

		// Two independent path facts, cached per material: the base match, and
		// the natural-feature match that only LOD is allowed to use. Caching
		// the ACCEPT decision instead would be wrong - the same material can
		// in principle reach here as both LOD and non-LOD, and the LOD path
		// accepts more.
		struct SnowPathMatch
		{
			bool base = false;
			bool naturalFeature = false;
			bool mergedAtlas = false;
		};
		static std::unordered_map<const void*, SnowPathMatch> driftMaterialCache;
		if (driftMaterialCache.size() > 4096)
			driftMaterialCache.clear();
		auto [it, inserted] = driftMaterialCache.try_emplace(material, SnowPathMatch{});
		if (inserted) {
			if (auto textureSet = material->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					std::string lowered(path);
					std::transform(lowered.begin(), lowered.end(), lowered.begin(),
						[](unsigned char c) { return (char)std::tolower(c); });
					// Drifts wear plain LANDSCAPE snow textures (no "drift" in
					// the path); requiring the landscape folder keeps frosted
					// plants (plant/tree folders) out.
					it->second.base = lowered.find("drift") != std::string::npos ||
					                  (lowered.find("landscape") != std::string::npos && lowered.find("snow") != std::string::npos);
					// LOD loses the kSnow/kProjectedUV flags its full mesh
					// carries, so a glacier keeps its snow up close and drops it
					// at range. The full mesh's own texture name is the only
					// classification left, and these two families are the ones
					// that wear projected snow as terrain-scale features.
					// "ice" is deliberately NOT matched: three letters that hit
					// lattice/office/service by accident, and "glacier" already
					// covers the ice family we actually saw dropped.
					it->second.naturalFeature = lowered.find("glacier") != std::string::npos ||
					                            lowered.find("mountain") != std::string::npos;
					// A merged DynDOLOD batch wears a generic atlas packing many
					// objects together, so the path says nothing about whether
					// any one of them is snowy. Gated behind the experiment
					// toggle: accepting these skins the WHOLE batch or none of
					// it, so if the batch is mixed it puts snow on ship hulls.
					it->second.mergedAtlas = lowered.find("dyndolod") != std::string::npos;
				}
			}
		}
		// The texture-name families over-accept: a shore RockShelf wears the
		// same mountain diffuse as a snowy crag, but its PROJECTED material
		// is the coastal sand MATO, not snow (Josef's Pale beach evidence,
		// 2026-08-22). When the LOD still hangs under its reference the MATO
		// settles it — positive snow evidence required. Unreferenced merged
		// batches keep the name heuristic.
		bool naturalFeature = it->second.naturalFeature;
		if (naturalFeature) {
			const MatoClass matoClass = ClassifyProjectedMato(a_pass->geometry);
			if (matoClass != MatoClass::kNoReference)
				naturalFeature = matoClass == MatoClass::kSnow;
		}
		const bool lodAccept = isObjectLOD &&
		                       (naturalFeature ||
								   (settings.SkinMergedLODAtlases && it->second.mergedAtlas));
		if (!(it->second.base || lodAccept)) {
			if (isObjectLOD)
				SampleMaterialReject(a_pass->geometry, material);
			return;
		}
	}

	// Twig-card shape class (branch piles, shore driftwood): vanilla flags
	// them snow-projected so they pass the flag gate, but the capture sees
	// sparse cards and the skin wraps them into broken shards (Josef's
	// TreeReachBranchPile01 evidence, 2026-08-22). Name-matched on the
	// diffuse path; extend the list as offenders surface.
	if (auto* shardMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material)) {
		static std::unordered_map<const void*, bool> shardMaterialCache;
		if (shardMaterialCache.size() > 4096)
			shardMaterialCache.clear();
		auto [shardIt, shardInserted] = shardMaterialCache.try_emplace(shardMaterial, false);
		if (shardInserted) {
			if (auto textureSet = shardMaterial->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					std::string lowered(path);
					std::transform(lowered.begin(), lowered.end(), lowered.begin(),
						[](unsigned char c) { return (char)std::tolower(c); });
					shardIt->second = lowered.find("branchpile") != std::string::npos ||
					                  lowered.find("driftwood") != std::string::npos;
				}
			}
		}
		if (shardIt->second)
			return;
	}

	// Range cap (Object Snow slider): distant mountains are snow-projected
	// everywhere in Skyrim; the skin only matters within the chosen range.
	const auto& translate = a_pass->geometry->world.translate;
	float dx = translate.x - eye.x;
	float dy = translate.y - eye.y;
	const float captureRange = settings.RangeSkinsM * kUnitsPerMeter;
	if (dx * dx + dy * dy > captureRange * captureRange)
		return;

	// The same geometry renders through multiple passes; capture once.
	if (!capturedStaticsSet.insert(a_pass->geometry).second)
		return;

	// Road-mesh model class: deterministic NAME + texture-path match. The
	// name check matters: road models are built from MULTIPLE trishapes
	// ('RoadChunk...:0', ':2'), and only some wear road textures; matching
	// textures alone splits one road across two depth settings, stacking a
	// second hovering shell.
	bool road = false;
	{
		std::string loweredName(a_pass->geometry->name.c_str());
		std::transform(loweredName.begin(), loweredName.end(), loweredName.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		road = loweredName.find("road") != std::string::npos || loweredName.find("bridge") != std::string::npos;
	}
	if (!road) {
		if (auto* roadMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material)) {
			static std::unordered_map<const void*, bool> roadMaterialCache;
			if (roadMaterialCache.size() > 4096)
				roadMaterialCache.clear();
			auto [roadIt, roadInserted] = roadMaterialCache.try_emplace(roadMaterial, false);
			if (roadInserted) {
				if (auto textureSet = roadMaterial->textureSet.get()) {
					if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
						std::string lowered(path);
						std::transform(lowered.begin(), lowered.end(), lowered.begin(),
							[](unsigned char c) { return (char)std::tolower(c); });
						roadIt->second = lowered.find("road") != std::string::npos || lowered.find("bridge") != std::string::npos;
					}
				}
			}
			road = roadIt->second;
		}
	}

	capturedStatics.push_back({ RE::NiPointer<RE::BSGeometry>(a_pass->geometry), a_pass->geometry->world, road });
}

struct SD_BSLightingShader_SetupGeometry
{
	static void thunk(RE::BSLightingShader* shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
	{
		auto& snowDeformation = globals::features::snowDeformation;
		if (snowDeformation.loaded)
			snowDeformation.SetProjectedSnowBit(a_pass);

		func(shader, a_pass, a_flags);

		if (snowDeformation.loaded)
			snowDeformation.BSLightingShader_SetupGeometry(a_pass);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void SnowDeformation::InstallStaticsCaptureHook()
{
	logger::info("[SNOW DEFORMATION] Hooking BSLightingShader::SetupGeometry");
	stl::write_vfunc<0x6, SD_BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
}

// Compiles one stage of the statics skin, RETURNING the bytecode blob;
// input layouts must be created against the VS bytecode, which
// Util::CompileShader discards. Include resolution matches CompileShader's
// convention (everything relative to Data\Shaders).
static ID3DBlob* SD_CompileShaderBlob(const wchar_t* a_path, const char* a_target, const char* a_stageDefine, const char* a_extraDefine = nullptr, const char* a_extraDefine2 = nullptr, const char* a_extraDefine3 = nullptr)
{
	struct ShaderInclude : public ID3DInclude
	{
		HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID, LPCVOID* ppData, UINT* pBytes) override
		{
			std::filesystem::path filePath = pFileName;
			filePath = L"Data\\Shaders" / filePath;
			std::ifstream file(filePath, std::ios::binary);
			if (!file.is_open()) {
				*ppData = nullptr;
				*pBytes = 0;
				return E_FAIL;
			}
			file.seekg(0, std::ios::end);
			UINT size = static_cast<UINT>(file.tellg());
			file.seekg(0, std::ios::beg);
			char* data = new char[size];
			file.read(data, size);
			*ppData = data;
			*pBytes = size;
			return S_OK;
		}
		HRESULT Close(LPCVOID pData) override
		{
			delete[] static_cast<const char*>(pData);
			return S_OK;
		}
	} includeHandler;

	D3D_SHADER_MACRO macros[] = {
		{ a_stageDefine, "" },
		{ a_extraDefine ? a_extraDefine : "DX11", "" },
		{ a_extraDefine2 ? a_extraDefine2 : "DX11", "" },
		{ a_extraDefine3 ? a_extraDefine3 : "DX11", "" },
		{ "WINPC", "" },
		{ "DX11", "" },
		{ nullptr, nullptr }
	};

	ID3DBlob* blob = nullptr;
	ID3DBlob* errors = nullptr;
	if (FAILED(D3DCompileFromFile(a_path, macros, &includeHandler, "main", a_target,
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
		logger::warn("[SNOW DEFORMATION] Statics skin {} compile failed:\n{}", a_target,
			errors ? static_cast<char*>(errors->GetBufferPointer()) : "unknown error");
		if (errors)
			errors->Release();
		return nullptr;
	}
	if (errors)
		errors->Release();
	return blob;
}

bool SnowDeformation::EnsureStaticsShaders()
{
	if (staticsVS && staticsPS)
		return true;
	if (staticsShadersFailed)
		return false;

	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl";

	if (!staticsVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsVS))) {
				staticsVSBlob = blob;
				Util::SetResourceName(staticsVS, "SnowDeformation::StaticsShellVS");
			}
		}
	}
	// Tessellated skin stages: optional (legacy path remains the fallback),
	// so failures here never set staticsShadersFailed.
	if (!staticsTessVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SNOW_TESS"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsTessVS)))
				Util::SetResourceName(staticsTessVS, "SnowDeformation::StaticsShellTessVS");
		}
	}
	if (!staticsHS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "hs_5_0", "HULLSHADER"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateHullShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsHS)))
				Util::SetResourceName(staticsHS, "SnowDeformation::StaticsShellHS");
		}
	}
	if (!staticsDS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ds_5_0", "DOMAINSHADER"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateDomainShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsDS)))
				Util::SetResourceName(staticsDS, "SnowDeformation::StaticsShellDS");
		}
	}
	// EHF sun attenuation only compiles when the addon is installed (its
	// hlsli is not CORE); the shells' PBR sun path gates on this define.
	const char* ehfDefine = globals::features::exponentialHeightFog.loaded ? "SNOW_EXP_HEIGHT_FOG" : nullptr;
	const char* iblDefine = globals::features::ibl.loaded ? "SNOW_IBL" : nullptr;

	if (!staticsPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPS)))
				Util::SetResourceName(staticsPS, "SnowDeformation::StaticsShellPS");
		}
	}

	// Trench patch (PATCH define): SV_VertexID grid, no input layout. The
	// draw guards on the pointers, so a compile failure just skips the pass.
	if (!patchVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "PATCH"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchVS)))
				Util::SetResourceName(patchVS, "SnowDeformation::TrenchPatchVS");
		}
	}
	if (!patchPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "PATCH", ehfDefine, iblDefine));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchPS)))
				Util::SetResourceName(patchPS, "SnowDeformation::TrenchPatchPS");
		}
	}
	if (!patchTessVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "PATCH", "SNOW_TESS"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchTessVS)))
				Util::SetResourceName(patchTessVS, "SnowDeformation::TrenchPatchTessVS");
		}
	}
	if (!patchHS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "hs_5_0", "HULLSHADER", "PATCH"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateHullShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchHS)))
				Util::SetResourceName(patchHS, "SnowDeformation::TrenchPatchHS");
		}
	}
	if (!patchDS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ds_5_0", "DOMAINSHADER", "PATCH"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateDomainShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchDS)))
				Util::SetResourceName(patchDS, "SnowDeformation::TrenchPatchDS");
		}
	}

	constexpr auto heightPath = L"Data\\Shaders\\SnowDeformation\\SnowHeightCapture.hlsl";
	if (!heightVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "vs_5_0", "VSHADER"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightVS)))
				Util::SetResourceName(heightVS, "SnowDeformation::HeightCaptureVS");
		}
	}
	if (!heightPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "ps_5_0", "PSHADER"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightPS)))
				Util::SetResourceName(heightPS, "SnowDeformation::HeightCapturePS");
		}
	}
	constexpr auto processPath = L"Data\\Shaders\\SnowDeformation\\HeightMapProcessCS.hlsl";
	if (!heightScrollCS)
		heightScrollCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(processPath, {}, "cs_5_0", "ScrollCS"));
	if (!heightCombineCS)
		heightCombineCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(processPath, {}, "cs_5_0", "CombineCS"));
	if (!heightConeCS)
		heightConeCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(processPath, {}, "cs_5_0", "ConeCS"));
	if (!objectConeSeedCS)
		objectConeSeedCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(processPath, {}, "cs_5_0", "ObjectConeSeedCS"));
	if (!objectConeCS)
		objectConeCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(processPath, {}, "cs_5_0", "ObjectConeCS"));

	if (!staticsVS || !staticsPS || !heightVS || !heightPS || !heightScrollCS || !heightCombineCS || !heightConeCS) {
		staticsShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Statics skin disabled (shader compilation failed)");
		return false;
	}
	return true;
}

void SnowDeformation::CreateHeightFieldResources()
{
	D3D11_TEXTURE2D_DESC heightDesc = {
		.Width = kHeightMapDim,
		.Height = kHeightMapDim,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R32_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC heightSrvDesc = {
		.Format = heightDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_RENDER_TARGET_VIEW_DESC heightRtvDesc = {
		.Format = heightDesc.Format,
		.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC heightUavDesc = {
		.Format = heightDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	auto makeHeightTexture = [&](const char* a_name) {
		auto* texture = new Texture2D(heightDesc, a_name);
		texture->CreateSRV(heightSrvDesc);
		texture->CreateRTV(heightRtvDesc);
		texture->CreateUAV(heightUavDesc);
		return texture;
	};
	heightTopRaw[0] = makeHeightTexture("SnowDeformation::HeightTopRaw0");
	heightTopRaw[1] = makeHeightTexture("SnowDeformation::HeightTopRaw1");
	heightBottomRaw[0] = makeHeightTexture("SnowDeformation::HeightBottomRaw0");
	heightBottomRaw[1] = makeHeightTexture("SnowDeformation::HeightBottomRaw1");
	heightTopFiltered = makeHeightTexture("SnowDeformation::HeightFieldFiltered");
	// The mask carries TWO independent channels (R = door suppression,
	// G = melt fraction). A single winner-takes-all channel discarded melt
	// wherever a door's faint influence tail reached - a ring of full-depth
	// plateau snow around every sheltered door.
	D3D11_TEXTURE2D_DESC maskDesc = heightDesc;
	maskDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
	D3D11_SHADER_RESOURCE_VIEW_DESC maskSrvDesc = heightSrvDesc;
	maskSrvDesc.Format = maskDesc.Format;
	D3D11_RENDER_TARGET_VIEW_DESC maskRtvDesc = heightRtvDesc;
	maskRtvDesc.Format = maskDesc.Format;
	D3D11_UNORDERED_ACCESS_VIEW_DESC maskUavDesc = heightUavDesc;
	maskUavDesc.Format = maskDesc.Format;
	heightBottomFiltered = new Texture2D(maskDesc, "SnowDeformation::HeightShelterMask");
	heightBottomFiltered->CreateSRV(maskSrvDesc);
	heightBottomFiltered->CreateRTV(maskRtvDesc);
	heightBottomFiltered->CreateUAV(maskUavDesc);
	heightScratch = makeHeightTexture("SnowDeformation::HeightConeScratch");
	objectSnowCone = makeHeightTexture("SnowDeformation::ObjectSnowCone");

	// Skin-depth raster: R16F, SRV+RTV only (cleared and re-rasterized fresh
	// every frame).
	D3D11_TEXTURE2D_DESC skinDepthDesc = heightDesc;
	skinDepthDesc.Format = DXGI_FORMAT_R16_FLOAT;
	skinDepthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	D3D11_SHADER_RESOURCE_VIEW_DESC skinDepthSrvDesc = heightSrvDesc;
	skinDepthSrvDesc.Format = skinDepthDesc.Format;
	D3D11_RENDER_TARGET_VIEW_DESC skinDepthRtvDesc = heightRtvDesc;
	skinDepthRtvDesc.Format = skinDepthDesc.Format;
	heightSkinDepth = new Texture2D(skinDepthDesc, "SnowDeformation::HeightSkinDepth");
	heightSkinDepth->CreateSRV(skinDepthSrvDesc);
	heightSkinDepth->CreateRTV(skinDepthRtvDesc);
}

void SnowDeformation::RenderObjectHeightMap()
{
	auto context = globals::d3d::context;

	// Camera-following window, snapped to texel size for stability.
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	constexpr float texel = kHeightMapHalfExtent * 2.0f / kHeightMapDim;
	float2 newCenter = {
		std::floor(eye.x / texel) * texel,
		std::floor(eye.y / texel) * texel
	};

	// Scroll the ACCUMULATED maps into the new window position.
	// +worldY maps to texture -v, so the v-axis delta is negated.
	HeightProcessCB processData{};
	processData.ScrollDelta = {
		(int)std::lround((newCenter.x - heightWindowCenter.x) / texel),
		-(int)std::lround((newCenter.y - heightWindowCenter.y) / texel)
	};
	processData.ClearAll = heightMapValid ? 0u : 1u;
	processData.HeightWindowCenter = newCenter;
	processData.HeightHalfExtent = kHeightMapHalfExtent;
	processData.SlopePerUnit = std::clamp(settings.SnowMoundSteepness, 0.5f, 3.0f);
	constexpr float shellCellSize = kShellVertexSpacing * kShellTexelsPerCell;
	processData.TerrainWindowOrigin = { shellWindowCellX * shellCellSize, shellWindowCellY * shellCellSize };
	processData.TerrainTexelSize = kShellVertexSpacing;
	processData.TerrainDim = kShellWindowDim;
	processData.GhostDecay = 0.5f;
	processData.DeformWindowOriginH = windowOrigin;
	processData.DeformInvWorldSizeH = 1.0f / deformWorldSize;
	processData.CorpseSphereCount = (uint32_t)corpseMoundSpheres.size();
	processData.CorpseMoundCap = kCorpseMoundCap;
	for (size_t sphereI = 0; sphereI < corpseMoundSpheres.size(); sphereI++)
		processData.CorpseSpheres[sphereI] = corpseMoundSpheres[sphereI];
	// Wind for the wall-drift bias, same source as the refill drift but
	// temporally smoothed (~8 s): drifted banks are slow accumulation and
	// must not pump up and down with per-frame gusts.
	float2 windNow = { 0.0f, 0.0f };
	if (auto* sky = RE::Sky::GetSingleton()) {
		float windStrength = std::clamp(sky->windSpeed, 0.0f, 1.0f);
		windNow = { std::sin(sky->windAngle) * windStrength, std::cos(sky->windAngle) * windStrength };
	}
	const float windDt = globals::game::deltaTime ? std::max(*globals::game::deltaTime, 0.0f) : 0.016f;
	const float windBlend = std::clamp(windDt / 8.0f, 0.0f, 1.0f);
	driftWind.x += (windNow.x - driftWind.x) * windBlend;
	driftWind.y += (windNow.y - driftWind.y) * windBlend;
	processData.WindBiasH = driftWind;
	processData.DriftHeight = std::max(settings.WallDriftHeight, 0.0f);
	processData.ObstructionCount = (uint32_t)obstructions.size();
	for (size_t obsI = 0; obsI < obstructions.size(); obsI++) {
		processData.ObstructionPosExt[obsI] = obstructions[obsI].first;
		processData.ObstructionRot[obsI] = obstructions[obsI].second;
	}
	heightProcessCB->Update(processData);
	heightWindowCenter = newCenter;
	heightMapValid = true;

	// Exclusion zones. Static sources (doors, campfires, heat sources,
	// dropped torches) refresh on window scroll and on a 60-frame cadence;
	// load doors (teleport data) are cave/building entrances; deeper
	// recesses, bigger clears. Heat sources come from Survival Mode's
	// warm-up-objects formlist when the plugin is present, plus model-path
	// matching that also covers non-Survival installs.
	const bool windowScrolled = processData.ScrollDelta.x != 0 || processData.ScrollDelta.y != 0;
	if (windowScrolled || (doorRefreshCounter++ % 60) == 0) {
		if (!survivalHeatSourcesResolved) {
			survivalHeatSourcesResolved = true;
			if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
				survivalHeatSources = dataHandler->LookupForm<RE::BGSListForm>(0x0008AA, "ccQDRSSE001-SurvivalMode.esl");
		}
		staticExclusions.clear();
		obstructions.clear();
		uint32_t gatherTrampleCount = 0;
		// Generic-flame entries by exclusion index, and the footprints of
		// stations that own their flames (smelters, forges): a flame inside
		// such a station must not add a melt spot on top of the
		// slider-controlled workspace clearing.
		std::vector<size_t> flameIndices;
		std::vector<float4> stationFlameGuards;
		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			if (auto tes = RE::TES::GetSingleton()) {
				// Reaches the WIDE field's window, not just the near mask's: a
				// campfire only shows at 200 m if it was gathered at 200 m.
				// References exist only inside loaded cells, so this is bounded by
				// uGridsToLoad rather than by the radius.
				tes->ForEachReferenceInRange(player, kExclusionFieldHalfExtent + 512.0f,
					[&](RE::TESObjectREFR* a_ref) {
						// Collect past the CB cap: the nearest-first sort below
						// picks the winners, so a far sconce can never evict a
						// near door. 4x cap bounds the pathological case.
						if (staticExclusions.size() >= kMaxExclusions * 4)
							return RE::BSContainer::ForEachResult::kStop;
						if (!a_ref || a_ref->IsDisabled() || !a_ref->Is3DLoaded())
							return RE::BSContainer::ForEachResult::kContinue;
						auto* base = a_ref->GetBaseObject();
						if (!base)
							return RE::BSContainer::ForEachResult::kContinue;

						// Doors: REMOVED per round-248 verdict. They were the
						// only coverage-kill (bare ground) system left, their
						// ellipse is large, and elevated doors (porch decks)
						// project it straight down onto the ground below
						// through the 300-unit z-gate. The suppress channel and
						// the shader ellipse stay for a better-scoped revisit.
						if (base->Is(RE::FormType::Door))
							return RE::BSContainer::ForEachResult::kContinue;

						// Dropped torches: carryable light references melt where
						// they lie - but only while actually BURNING. Torch-
						// snuffing mods keep the light base on the dropped ref
						// while removing its attached flame light; those still
						// trench as props, they just stop melting.
						if (auto* light = base->As<RE::TESObjectLIGH>(); light && light->CanBeCarried()) {
							if (HasActiveLight(a_ref->Get3D())) {
								auto pos = a_ref->GetPosition();
								staticExclusions.push_back({ { pos.x, pos.y, pos.z, kTorchClearRadius }, { 0.0f, 0.0f, 1.0f, 1.0f } });
							}
							return RE::BSContainer::ForEachResult::kContinue;
						}

						// Explicit form-type chain: skyrim_cast to TESModel
						// silently returns null for activator bases, which
						// makes campfires invisible to the gather.
						const char* modelPath = nullptr;
						if (auto* acti = base->As<RE::TESObjectACTI>())
							modelPath = acti->GetModel();
						else if (auto* stat = base->As<RE::TESObjectSTAT>())
							modelPath = stat->GetModel();
						else if (auto* movable = base->As<RE::BGSMovableStatic>())
							modelPath = movable->GetModel();
						// Classification order: named heat (model table / flames),
						// then the workspace table, then the Survival warm-up
						// formlist LAST - patched warm-up lists add workstations
						// (grindstones, anvils...), and those must stay
						// slider-controlled workspaces, not fixed heat spots.
						float heatRadius = 0.0f;
						bool genericFlame = false;
						std::string lowered;
						if (modelPath && modelPath[0]) {
							lowered.assign(modelPath);
							std::transform(lowered.begin(), lowered.end(), lowered.begin(),
								[](unsigned char c) { return (char)std::tolower(c); });
							if (lowered.find("torchbug") == std::string::npos) {
								if (lowered.find("fxfire") != std::string::npos) {
									genericFlame = true;
								} else {
									for (const auto& spec : kHeatSpecs) {
										if (lowered.find(spec.substring) != std::string::npos) {
											heatRadius = spec.radius;
											break;
										}
									}
								}
							}
						}
						if (heatRadius > 0.0f || genericFlame) {
							auto pos = a_ref->GetPosition();
							if (genericFlame) {
								// Grounded flame = open fire; raised flame
								// (brazier bowl, wall fire) melts a small spot
								// on the ground below.
								float landZ = pos.z;
								tes->GetLandHeight(pos, landZ);
								heatRadius = pos.z - landZ < kGroundFireBand ? kFireClearRadius : kRaisedFlameClearRadius;
							}
							heatRadius *= a_ref->GetScale();
							if (genericFlame)
								flameIndices.push_back(staticExclusions.size());
							staticExclusions.push_back({ { pos.x, pos.y, pos.z, heatRadius }, { 0.0f, 0.0f, 1.0f, 1.0f } });
							return RE::BSContainer::ForEachResult::kContinue;
						}

						// Workspace clearings: the shell only ever forms to
						// partial depth around worked spots (workstations,
						// stalls, wells, shrines) - a melt bowl centered toward
						// the working side; real trampling carves the rest. A
						// full pre-trampled look via the deformation map read
						// as mini mountains and was replaced by this.
						// TESFurniture derives from TESObjectACTI, so the model
						// chain above already reached every workstation.
						if (!lowered.empty()) {
							for (const auto& spec : kTrampleSpecs) {
								if (lowered.find(spec.substring) != std::string::npos) {
									auto pos = a_ref->GetPosition();
									float angleZ = a_ref->GetAngleZ();
									float scale = a_ref->GetScale();
									float zoneScale = std::max(settings.TrampleZoneScale, 0.0f);
									const float meltStrength = spec.meltStrength > 0.0f ?
								                                   spec.meltStrength :
								                                   1.0f - std::clamp(settings.TrampleZoneHeight, 0.0f, 100.0f) / 100.0f;
									// Elongation axis rides the facing (length =
									// aspect - 1; zero = circle); type 2 = smooth
									// bowl edge for bedding.
									const float elongation = std::max(spec.aspect, 1.0f) - 1.0f;
									staticExclusions.push_back({ { pos.x + std::sin(angleZ) * spec.forwardBias * scale,
																	 pos.y + std::cos(angleZ) * spec.forwardBias * scale,
																	 pos.z, spec.radius * scale * zoneScale },
										{ std::sin(angleZ) * elongation, std::cos(angleZ) * elongation, meltStrength,
											spec.smoothEdge ? 2.0f : 1.0f } });
									gatherTrampleCount++;
									// Physical footprint, NOT slider-scaled: the
									// flame sits inside the structure no matter
									// how small the clearing is tuned.
									if (spec.ownsFlames)
										stationFlameGuards.push_back({ pos.x, pos.y, pos.z, spec.radius * scale * 0.5f });
									return RE::BSContainer::ForEachResult::kContinue;
								}
							}
						}

						// Wall-drift obstructions: big grounded statics dam
						// drifting snow (buildings, towers, huge rocks). OBND
						// half-extents gate; trees excluded - their bounds are
						// mostly canopy air.
						if (obstructions.size() < kMaxObstructions) {
							if (auto* boundObj = base->As<RE::TESBoundObject>()) {
								const float scale = a_ref->GetScale();
								const float extX = (boundObj->boundData.boundMax.x - boundObj->boundData.boundMin.x) * 0.5f * scale;
								const float extY = (boundObj->boundData.boundMax.y - boundObj->boundData.boundMin.y) * 0.5f * scale;
								const float extZ = (boundObj->boundData.boundMax.z - boundObj->boundData.boundMin.z) * 0.5f * scale;
								if (extZ >= kObstructionMinHeight && std::min(extX, extY) >= kObstructionMinFootprint &&
									std::max(extX, extY) <= kObstructionMaxFootprint &&
									lowered.find("tree") == std::string::npos && lowered.find("pine") == std::string::npos) {
									const float centerX = (boundObj->boundData.boundMax.x + boundObj->boundData.boundMin.x) * 0.5f * scale;
									const float centerY = (boundObj->boundData.boundMax.y + boundObj->boundData.boundMin.y) * 0.5f * scale;
									auto pos = a_ref->GetPosition();
									const float angleZ = a_ref->GetAngleZ();
									const float sinZ = std::sin(angleZ), cosZ = std::cos(angleZ);
									obstructions.push_back({ { pos.x + cosZ * centerX + sinZ * centerY,
																 pos.y - sinZ * centerX + cosZ * centerY, extX, extY },
										{ sinZ, cosZ, pos.z, 0.0f } });
								}
							}
						}

						// Survival warm-up formlist: heat neither table named.
						// Modest circle when grounded (the list also holds
						// candelabras), footprint-sized spot when raised.
						if (survivalHeatSources && survivalHeatSources->HasForm(base)) {
							float halfExtent = 20.0f;
							if (auto* bound = base->As<RE::TESBoundObject>()) {
								float extentX = (bound->boundData.boundMax.x - bound->boundData.boundMin.x) * 0.5f;
								float extentY = (bound->boundData.boundMax.y - bound->boundData.boundMin.y) * 0.5f;
								halfExtent = std::max(extentX, extentY) * a_ref->GetScale();
							}
							auto pos = a_ref->GetPosition();
							float landZ = pos.z;
							tes->GetLandHeight(pos, landZ);
							float radius = pos.z - landZ < kGroundFireBand ? kUnknownHeatClearRadius :
						                                                     std::clamp(halfExtent * 1.5f, kHeatClearRadiusMin, kHeatClearRadiusMax);
							staticExclusions.push_back({ { pos.x, pos.y, pos.z, radius * a_ref->GetScale() }, { 0.0f, 0.0f, 1.0f, 1.0f } });
						}
						return RE::BSContainer::ForEachResult::kContinue;
					});

				// Drop flame melt spots that sit inside a flame-owning station
				// (descending index order keeps the remaining indices valid).
				if (!stationFlameGuards.empty() && !flameIndices.empty()) {
					for (auto it = flameIndices.rbegin(); it != flameIndices.rend(); ++it) {
						const auto& flame = staticExclusions[*it].first;
						for (const auto& guard : stationFlameGuards) {
							const float dx = flame.x - guard.x, dy = flame.y - guard.y;
							if (dx * dx + dy * dy < guard.w * guard.w && std::abs(flame.z - guard.z) < 300.0f) {
								staticExclusions.erase(staticExclusions.begin() + *it);
								break;
							}
						}
					}
				}

				statTrampleCount = gatherTrampleCount;
				statObstructionCount = (uint32_t)obstructions.size();

				// Overflow: keep the sources nearest the player.
				if (staticExclusions.size() > kMaxExclusions) {
					const auto playerPos = player->GetPosition();
					std::partial_sort(staticExclusions.begin(), staticExclusions.begin() + kMaxExclusions, staticExclusions.end(),
						[&](const auto& a_lhs, const auto& a_rhs) {
							const float lhsDx = a_lhs.first.x - playerPos.x, lhsDy = a_lhs.first.y - playerPos.y;
							const float rhsDx = a_rhs.first.x - playerPos.x, rhsDy = a_rhs.first.y - playerPos.y;
							return lhsDx * lhsDx + lhsDy * lhsDy < rhsDx * rhsDx + rhsDy * rhsDy;
						});
					staticExclusions.resize(kMaxExclusions);
				}
			}
		}
	}

	// Upload after each gather. Carried torches deliberately do NOT melt:
	// a moving basin warps the bearer's own trench and berms (tried at full
	// and 0.35 strength, both read as snow avoiding the player). Dropped
	// burning torches cover the on-ground case.
	{
		ExclusionsCB exclusionData{};
		uint32_t exclusionCount = 0;
		for (const auto& [posRadius, dirExtType] : staticExclusions) {
			if (exclusionCount >= kMaxExclusions)
				break;
			exclusionData.PosRadius[exclusionCount] = posRadius;
			exclusionData.DirExtType[exclusionCount] = dirExtType;
			exclusionCount++;
		}
		exclusionData.ExclusionCount = exclusionCount;
		statExclusionCount = exclusionCount;
		doorsCB->Update(exclusionData);
	}

	// Rebaked every frame: the window follows the camera, so a cadence-gated
	// bake would drag the clearings behind it.
	RenderExclusionField();

	uint previous = heightCurrent;
	heightCurrent ^= 1;

	ID3D11Buffer* processCB = heightProcessCB->CB();
	context->CSSetConstantBuffers(0, 1, &processCB);
	ID3D11ShaderResourceView* scrollSRVs[2] = { heightTopRaw[previous]->srv.get(), heightBottomRaw[previous]->srv.get() };
	ID3D11UnorderedAccessView* scrollUAVs[2] = { heightTopRaw[heightCurrent]->uav.get(), heightBottomRaw[heightCurrent]->uav.get() };
	context->CSSetShaderResources(0, 2, scrollSRVs);
	context->CSSetUnorderedAccessViews(0, 2, scrollUAVs, nullptr);
	context->CSSetShader(heightScrollCS, nullptr, 0);
	context->Dispatch((kHeightMapDim + 7) / 8, (kHeightMapDim + 7) / 8, 1);

	ID3D11ShaderResourceView* nullCsSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullCsUAVs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, 2, nullCsSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullCsUAVs, nullptr);
	ID3D11Buffer* nullProcessCB = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullProcessCB);
	context->CSSetShader(nullptr, nullptr, 0);

	// Rasterize this frame's captures on top of the scrolled maps.
	const float skinDepthClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	context->ClearRenderTargetView(heightSkinDepth->rtv.get(), skinDepthClear);
	ID3D11RenderTargetView* heightRTVs[3] = { heightTopRaw[heightCurrent]->rtv.get(), heightBottomRaw[heightCurrent]->rtv.get(), heightSkinDepth->rtv.get() };
	context->OMSetRenderTargets(3, heightRTVs, nullptr);
	context->OMSetBlendState(heightMaxBlendState.get(), nullptr, 0xFFFFFFFF);

	D3D11_VIEWPORT heightViewport{ 0.0f, 0.0f, float(kHeightMapDim), float(kHeightMapDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &heightViewport);

	context->VSSetShader(heightVS, nullptr, 0);
	context->PSSetShader(heightPS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	// The capture PS rejects grounded fragments from the bottoms raster
	// (elevated undersides only); it reads terrain via the process CB.
	ID3D11Buffer* captureCB0 = heightProcessCB->CB();
	context->PSSetConstantBuffers(0, 1, &captureCB0);
	ID3D11ShaderResourceView* captureTerrainSRV = shellTerrainTexture->srv.get();
	context->PSSetShaderResources(2, 1, &captureTerrainSRV);

	globals::profiler->BeginPass("SnowDeformation::ObjectHeightMap");
	for (const auto& cap : capturedStatics) {
		auto* geometry = cap.geometry.get();
		if (!geometry)
			continue;
		auto triShape = geometry->AsTriShape();
		if (!triShape)
			continue;
		auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
			continue;
		uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		if (indexCount == 0)
			continue;

		auto desc = rendererData->vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
			continue;

		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		auto layoutIt = staticsILCache.find(descKey);
		if (layoutIt == staticsILCache.end() || !layoutIt->second)
			continue;  // layouts are created by the skin pass; reuse only
		context->IASetInputLayout(layoutIt->second.get());

		UINT stride = uint32_t(descKey & 0xF) * 4;
		if (stride == 0)
			continue;
		UINT offset = 0;
		auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
		auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
		context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);

		StaticsCB scb{};
		const auto& rot = cap.world.rotate;
		const float scale = cap.world.scale;
		scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, cap.world.translate.x };
		scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, cap.world.translate.y };
		scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, cap.world.translate.z };
		scb.ObjectsDepth = cap.road ? settings.RoadMeshesDepth : settings.ObjectsSnowDepth;
		scb.RoundedDepth = cap.road ? settings.RoadMeshesDepth : settings.SnowMeshesDepth;
		scb.VertexCountF = float(triShape->GetTrishapeRuntimeData().vertexCount);
		scb.HeightWindowCenter = heightWindowCenter;
		scb.HeightHalfExtent = kHeightMapHalfExtent;
		// The raster VS zeroes skin depth for objects that may not carve, so
		// both gates have to reach this pass; without them every object reads
		// as non-carving and the trench patch dies everywhere, roads included.
		scb.LegacySkin = cap.road ? 1.0f : 0.0f;
		scb.ObjectTrenches = settings.ObjectTrenches ? 1.0f : 0.0f;
		// Flat/rounded stats for the skin-depth output (RT2): the raster VS
		// reads the same classification the skin uses.
		ID3D11ShaderResourceView* rasterSmoothSRV = EnsureSmoothedNormals(geometry);
		context->VSSetShaderResources(10, 1, &rasterSmoothSRV);
		scb.HasSmoothedNormals = rasterSmoothSRV ? 1.0f : 0.0f;
		staticsCB->Update(scb);

		context->DrawIndexed(indexCount, 0, 0);
	}
	globals::profiler->EndPass();

	ID3D11RenderTargetView* nullRTVs[3] = { nullptr, nullptr, nullptr };
	context->OMSetRenderTargets(3, nullRTVs, nullptr);
	ID3D11Buffer* nullVB = nullptr;
	UINT zero = 0;
	context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
	context->IASetInputLayout(nullptr);
	context->VSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	ID3D11Buffer* nullCB1 = nullptr;
	context->VSSetConstantBuffers(1, 1, &nullCB1);
	context->PSSetConstantBuffers(0, 1, &nullCB1);
	ID3D11ShaderResourceView* nullSmoothSRV = nullptr;
	context->VSSetShaderResources(10, 1, &nullSmoothSRV);
	context->PSSetShaderResources(2, 1, &nullSmoothSRV);

	// Combine: raw tops/bottoms -> base field (topFiltered) + shelter mask
	// (bottomFiltered); bare ground under floating walkways/roofs/bridges.
	const UINT dispatchDim = (kHeightMapDim + 7) / 8;
	ID3D11ShaderResourceView* terrainSRV = shellTerrainTexture->srv.get();
	context->CSSetConstantBuffers(0, 1, &processCB);
	context->CSSetShaderResources(2, 1, &terrainSRV);
	// Deformation map (t3): CombineCS gates corpse mounds on local refill.
	ID3D11ShaderResourceView* deformSRV = GetDeformationSRV();
	context->CSSetShaderResources(3, 1, &deformSRV);
	{
		ID3D11ShaderResourceView* combineSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightBottomRaw[heightCurrent]->srv.get() };
		// Field at u0, two-channel mask at u2 (u1 belongs to ScrollCS's raw
		// bottoms and stays clear here).
		ID3D11UnorderedAccessView* combineUAVs[3] = { heightTopFiltered->uav.get(), nullptr, heightBottomFiltered->uav.get() };
		ID3D11Buffer* exclusionCB = doorsCB->CB();
		context->CSSetConstantBuffers(1, 1, &exclusionCB);
		context->CSSetShaderResources(0, 2, combineSRVs);
		context->CSSetUnorderedAccessViews(0, 3, combineUAVs, nullptr);
		context->CSSetShader(heightCombineCS, nullptr, 0);
		context->Dispatch(dispatchDim, dispatchDim, 1);
		context->CSSetShaderResources(0, 2, nullCsSRVs);
		ID3D11UnorderedAccessView* nullCombineUAVs[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 3, nullCombineUAVs, nullptr);
		ID3D11Buffer* nullExclusionCB = nullptr;
		context->CSSetConstantBuffers(1, 1, &nullExclusionCB);
	}

	// Angle of repose: multi-scale min-plus cone passes (large steps first),
	// ping-ponging topFiltered <-> heightScratch and ENDING in topFiltered.
	// Steps are texels: the leading 64 preserves the cone's world reach at
	// the 4-unit texel (was 32..1 at 8-unit texels); the trailing repeat
	// keeps the pass count even.
	static constexpr uint kConeSteps[] = { 64, 32, 16, 8, 4, 2, 1, 1 };
	// An even pass count is what lands the final result back in topFiltered.
	static_assert(std::size(kConeSteps) % 2 == 0);
	context->CSSetShader(heightConeCS, nullptr, 0);
	Texture2D* coneIn = heightTopFiltered;
	Texture2D* coneOut = heightScratch;
	for (uint step : kConeSteps) {
		processData.ConeStep = step;
		heightProcessCB->Update(processData);
		ID3D11ShaderResourceView* coneSRV = coneIn->srv.get();
		ID3D11UnorderedAccessView* coneUAV = coneOut->uav.get();
		context->CSSetShaderResources(0, 1, &coneSRV);
		context->CSSetUnorderedAccessViews(0, 1, &coneUAV, nullptr);
		context->Dispatch(dispatchDim, dispatchDim, 1);
		context->CSSetShaderResources(0, 1, nullCsSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
		std::swap(coneIn, coneOut);
	}

	// Object snow cone: the same repose transform run over the OBJECT top
	// raster, giving the skin's edge taper a ready-made snow surface to read.
	// Seeded at full class depth over covered columns and at terrain height
	// off them, so each object's layer slopes down to the ground at its rim.
	if (objectConeSeedCS && objectConeCS && objectSnowCone && heightTopRaw[heightCurrent]) {
		// One shared field for both classes: seed it with the deeper of the
		// two and let each class normalize against it (see SkinLift.RimT).
		processData.ObjectSnowDepth = std::max({ settings.SnowMeshesDepth, settings.ObjectsSnowDepth, 0.1f });
		heightProcessCB->Update(processData);
		context->CSSetShader(objectConeSeedCS, nullptr, 0);
		ID3D11ShaderResourceView* seedSRV = heightTopRaw[heightCurrent]->srv.get();
		ID3D11UnorderedAccessView* seedUAV = objectSnowCone->uav.get();
		context->CSSetShaderResources(0, 1, &seedSRV);
		context->CSSetUnorderedAccessViews(0, 1, &seedUAV, nullptr);
		context->Dispatch(dispatchDim, dispatchDim, 1);
		context->CSSetShaderResources(0, 1, nullCsSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);

		context->CSSetShader(objectConeCS, nullptr, 0);
		Texture2D* objIn = objectSnowCone;
		Texture2D* objOut = heightScratch;
		for (uint step : kConeSteps) {
			processData.ConeStep = step;
			heightProcessCB->Update(processData);
			ID3D11ShaderResourceView* objSRV = objIn->srv.get();
			ID3D11UnorderedAccessView* objUAV = objOut->uav.get();
			context->CSSetShaderResources(0, 1, &objSRV);
			context->CSSetUnorderedAccessViews(0, 1, &objUAV, nullptr);
			context->Dispatch(dispatchDim, dispatchDim, 1);
			context->CSSetShaderResources(0, 1, nullCsSRVs);
			context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
			std::swap(objIn, objOut);
		}
	}

	ID3D11ShaderResourceView* nullTailSRVs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(2, 2, nullTailSRVs);
	context->CSSetConstantBuffers(0, 1, &nullProcessCB);
	context->CSSetShader(nullptr, nullptr, 0);
}

ID3D11ShaderResourceView* SnowDeformation::EnsureSmoothedNormals(RE::BSGeometry* a_geometry)
{
	auto triShape = a_geometry->AsTriShape();
	if (!triShape)
		return nullptr;
	auto rendererData = a_geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData || !rendererData->vertexBuffer)
		return nullptr;

	// Pointer reuse after cell unloads could serve stale normals to a new
	// mesh; the cap flushes the cache before that becomes likely.
	if (smoothedNormalsCache.size() > 1024)
		smoothedNormalsCache.clear();

	auto [it, inserted] = smoothedNormalsCache.try_emplace(rendererData->vertexBuffer);
	auto& entry = it->second;
	if (!inserted)
		return entry.ready ? entry.srv.get() : nullptr;

	// First sight of this geometry: build now (a buffer copy + two small
	// dispatches, once per unique mesh). Failures leave the permanent null
	// entry; no per-frame retries; the VS falls back to raw normals.
	if (!smoothAccumulateCS)
		smoothAccumulateCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "ACCUMULATE", "" } }, "cs_5_0"));
	if (!smoothResolveCS)
		smoothResolveCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "RESOLVE", "" } }, "cs_5_0"));
	if (!smoothFlatStatsCS)
		smoothFlatStatsCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "FLATSTATS", "" } }, "cs_5_0"));
	if (!smoothAccumulateCS || !smoothResolveCS || !smoothFlatStatsCS || !smoothCB)
		return nullptr;

	auto desc = rendererData->vertexDesc;
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
		return nullptr;
	uint64_t descKey;
	memcpy(&descKey, &desc, sizeof(descKey));
	const uint32_t stride = uint32_t(descKey & 0xF) * 4;
	const uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
	if (stride == 0 || vertexCount == 0)
		return nullptr;

	// Position size = distance to the first following attribute (same
	// offset-table logic the input layouts use; VF_FULLPREC is unreliable).
	uint32_t positionBytes = stride;
	static constexpr std::pair<RE::BSGraphics::Vertex::Flags, RE::BSGraphics::Vertex::Attribute> kSmoothAttrs[] = {
		{ RE::BSGraphics::Vertex::VF_UV, RE::BSGraphics::Vertex::VA_TEXCOORD0 },
		{ RE::BSGraphics::Vertex::VF_UV_2, RE::BSGraphics::Vertex::VA_TEXCOORD1 },
		{ RE::BSGraphics::Vertex::VF_NORMAL, RE::BSGraphics::Vertex::VA_NORMAL },
		{ RE::BSGraphics::Vertex::VF_TANGENT, RE::BSGraphics::Vertex::VA_BINORMAL },
		{ RE::BSGraphics::Vertex::VF_COLORS, RE::BSGraphics::Vertex::VA_COLOR },
		{ RE::BSGraphics::Vertex::VF_SKINNED, RE::BSGraphics::Vertex::VA_SKINNING },
		{ RE::BSGraphics::Vertex::VF_LANDDATA, RE::BSGraphics::Vertex::VA_LANDDATA },
		{ RE::BSGraphics::Vertex::VF_EYEDATA, RE::BSGraphics::Vertex::VA_EYEDATA },
	};
	for (auto [flag, attr] : kSmoothAttrs) {
		if (desc.HasFlag(flag)) {
			uint32_t attrOffset = desc.GetAttributeOffset(attr);
			if (attrOffset > 0 && attrOffset < positionBytes)
				positionBytes = attrOffset;
		}
	}
	const uint32_t normalOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	auto* gameVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);

	// SRV-capable copy of the game's vertex buffer (raw view); the game's
	// own buffers carry no shader-resource bind flag.
	D3D11_BUFFER_DESC srcDesc{};
	gameVB->GetDesc(&srcDesc);
	D3D11_BUFFER_DESC copyDesc{};
	copyDesc.ByteWidth = srcDesc.ByteWidth;
	copyDesc.Usage = D3D11_USAGE_DEFAULT;
	copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	copyDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	winrt::com_ptr<ID3D11Buffer> vbCopy;
	if (FAILED(device->CreateBuffer(&copyDesc, nullptr, vbCopy.put())))
		return nullptr;
	context->CopyResource(vbCopy.get(), gameVB);

	D3D11_SHADER_RESOURCE_VIEW_DESC rawSrvDesc{};
	rawSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	rawSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
	rawSrvDesc.BufferEx.NumElements = copyDesc.ByteWidth / 4;
	rawSrvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
	winrt::com_ptr<ID3D11ShaderResourceView> vbCopySRV;
	if (FAILED(device->CreateShaderResourceView(vbCopy.get(), &rawSrvDesc, vbCopySRV.put())))
		return nullptr;

	// Hash table (transient) and the persistent output buffer.
	uint32_t tableSlots = 64;
	while (tableSlots < vertexCount * 2)
		tableSlots <<= 1;
	D3D11_BUFFER_DESC tableDesc{};
	tableDesc.ByteWidth = tableSlots * 16;
	tableDesc.Usage = D3D11_USAGE_DEFAULT;
	tableDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	tableDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	winrt::com_ptr<ID3D11Buffer> tableBuffer;
	if (FAILED(device->CreateBuffer(&tableDesc, nullptr, tableBuffer.put())))
		return nullptr;
	D3D11_UNORDERED_ACCESS_VIEW_DESC rawUavDesc{};
	rawUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	rawUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	rawUavDesc.Buffer.NumElements = tableSlots * 4;
	rawUavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	winrt::com_ptr<ID3D11UnorderedAccessView> tableUAV;
	if (FAILED(device->CreateUnorderedAccessView(tableBuffer.get(), &rawUavDesc, tableUAV.put())))
		return nullptr;

	// One extra element past the vertices: the mesh's flatness stats, read by
	// the VS to pick the flat or rounded snow behavior without any CPU
	// readback.
	D3D11_BUFFER_DESC outDesc{};
	outDesc.ByteWidth = (vertexCount + 1) * 16;
	outDesc.Usage = D3D11_USAGE_DEFAULT;
	outDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	outDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	outDesc.StructureByteStride = 16;
	winrt::com_ptr<ID3D11Buffer> outBuffer;
	if (FAILED(device->CreateBuffer(&outDesc, nullptr, outBuffer.put())))
		return nullptr;
	Util::SetResourceName(outBuffer.get(), "SnowDeformation::SmoothedNormals");
	D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
	outSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	outSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	outSrvDesc.Buffer.NumElements = vertexCount + 1;
	winrt::com_ptr<ID3D11ShaderResourceView> outSRV;
	if (FAILED(device->CreateShaderResourceView(outBuffer.get(), &outSrvDesc, outSRV.put())))
		return nullptr;
	D3D11_UNORDERED_ACCESS_VIEW_DESC outUavDesc{};
	outUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	outUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	outUavDesc.Buffer.NumElements = vertexCount + 1;
	winrt::com_ptr<ID3D11UnorderedAccessView> outUAV;
	if (FAILED(device->CreateUnorderedAccessView(outBuffer.get(), &outUavDesc, outUAV.put())))
		return nullptr;

	const UINT clearZero[4] = { 0, 0, 0, 0 };
	context->ClearUnorderedAccessViewUint(tableUAV.get(), clearZero);

	SmoothCB cb{};
	cb.VertexCount = vertexCount;
	cb.StrideBytes = stride;
	cb.NormalOffsetBytes = normalOffset;
	cb.PosIsFloat32 = positionBytes >= 16 ? 1u : 0u;
	cb.TableMask = tableSlots - 1;
	smoothCB->Update(cb);

	// Compute-only state: does not disturb the surrounding draw pipeline.
	ID3D11Buffer* cscb = smoothCB->CB();
	context->CSSetConstantBuffers(0, 1, &cscb);
	ID3D11ShaderResourceView* csSRV = vbCopySRV.get();
	context->CSSetShaderResources(0, 1, &csSRV);
	ID3D11UnorderedAccessView* csUAVs[2] = { tableUAV.get(), outUAV.get() };
	context->CSSetUnorderedAccessViews(0, 2, csUAVs, nullptr);
	const uint32_t groups = (vertexCount + 63) / 64;
	context->CSSetShader(smoothAccumulateCS, nullptr, 0);
	context->Dispatch(groups, 1, 1);
	context->CSSetShader(smoothResolveCS, nullptr, 0);
	context->Dispatch(groups, 1, 1);
	// Flatness stats: one group strided over the resolved normals.
	context->CSSetShader(smoothFlatStatsCS, nullptr, 0);
	context->Dispatch(1, 1, 1);

	ID3D11ShaderResourceView* nullCsSRV = nullptr;
	context->CSSetShaderResources(0, 1, &nullCsSRV);
	ID3D11UnorderedAccessView* nullCsUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullCsUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	entry.buffer = outBuffer;
	entry.srv = outSRV;
	entry.ready = true;
	return entry.srv.get();
}

void SnowDeformation::DrawCapturedStatics()
{
	// The cover always draws (minimum coat); sliders never disable it.
	if (capturedStatics.empty())
		return;
	if (!EnsureStaticsShaders())
		return;

	// One-shot capture histogram by camera distance. Every draw-loop skip is
	// already logged and none fire, so if distant objects are missing skins the
	// gate is either up in the capture hook (nothing captured out there) or
	// down in the shader (captured and drawn, but shaded away). This tells the
	// two apart in one launch instead of bisecting for it. Fires once, on the
	// first frame with a populated exterior, so a loading frame cannot skew it.
	{
		static std::atomic<bool> loggedHistogram{ false };
		if (capturedStatics.size() > 50 && !loggedHistogram.exchange(true)) {
			constexpr float kBandM[] = { 25.0f, 50.0f, 100.0f, 200.0f, 400.0f, 750.0f };
			uint32_t bands[std::size(kBandM) + 1] = {};
			float furthest = 0.0f;
			auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
			for (const auto& cap : capturedStatics) {
				const float dx = cap.world.translate.x - eye.x;
				const float dy = cap.world.translate.y - eye.y;
				const float metres = std::sqrt(dx * dx + dy * dy) / kUnitsPerMeter;
				furthest = std::max(furthest, metres);
				size_t band = 0;
				while (band < std::size(kBandM) && metres > kBandM[band])
					++band;
				++bands[band];
			}
			logger::info("[SNOW DEFORMATION] capture histogram ({} total, furthest {:.0f} m): "
						 "<25m={} 25-50={} 50-100={} 100-200={} 200-400={} 400-750={} >750m={}",
				capturedStatics.size(), furthest,
				bands[0], bands[1], bands[2], bands[3], bands[4], bands[5], bands[6]);
		}
	}

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	context->PSSetShader(staticsPS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);

	// Tessellated skins: 3-control-point patches with the same index data;
	// the hull shader subdivides by edge length and distance, the domain
	// shader adds displacement-map relief along the inflate normal. The DS
	// binds its needs directly so the path is self-sufficient even if the
	// landscape shell's tessellation is unavailable.
	// Every class tessellates, relief or not: the vertical lift resolves its
	// rim over whatever vertices the source mesh happens to carry there, and
	// low-poly meshes have far too few. Relief stays gated inside the DS.
	const bool tessellateSkins = staticsTessVS && staticsHS && staticsDS;
	if (tessellateSkins) {
		context->VSSetShader(staticsTessVS, nullptr, 0);
		context->HSSetShader(staticsHS, nullptr, 0);
		context->DSSetShader(staticsDS, nullptr, 0);
		ID3D11Buffer* cb0 = shellCB->CB();
		context->HSSetConstantBuffers(0, 1, &cb0);
		context->DSSetConstantBuffers(0, 1, &cb0);
		// The DS evaluates the lift per generated vertex and the HS sizes
		// tessellation against the class collapse range, so both need the
		// per-object block.
		ID3D11Buffer* dsCB1 = staticsCB->CB();
		context->HSSetConstantBuffers(1, 1, &dsCB1);
		context->DSSetConstantBuffers(1, 1, &dsCB1);
		ID3D11ShaderResourceView* dsDeformSRV = GetDeformationSRV();
		context->DSSetShaderResources(1, 1, &dsDeformSRV);
		ID3D11ShaderResourceView* dsHeightSRV = shellSnowHeightSRV.get();
		context->DSSetShaderResources(8, 1, &dsHeightSRV);
		ID3D11SamplerState* dsSampler = shellSnowSampler.get();
		context->DSSetSamplers(0, 1, &dsSampler);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
	} else {
		context->VSSetShader(staticsVS, nullptr, 0);
	}

	globals::profiler->BeginPass("SnowDeformation::StaticsShell");
	// One-shot skip diagnostics: geometries that capture but cannot draw are
	// the "why is THIS rock bare" cases; name the reason in the log.
	static std::unordered_set<std::string> loggedSkips;
	auto logSkip = [](RE::BSGeometry* a_geometry, const char* a_reason) {
		if (loggedSkips.size() < 24 && loggedSkips.insert(std::string(a_geometry->name.c_str()) + a_reason).second)
			logger::info("[SNOW DEFORMATION] Statics skip '{}': {}", a_geometry->name.c_str(), a_reason);
	};

	// Object top raster (PS t11): the skin PS reads it to keep its rim wall
	// alive through the steepness gates. Same raster the patch binds to its
	// VS further down.
	ID3D11ShaderResourceView* objectTopSRV = (heightTopRaw[heightCurrent] && heightTopRaw[heightCurrent]->srv) ?
	                                             heightTopRaw[heightCurrent]->srv.get() :
	                                             nullptr;
	context->PSSetShaderResources(11, 1, &objectTopSRV);
	// IBL SH textures (t76/t77): the skins draw standalone from the landscape
	// shell, so slot state from its pass is not guaranteed here.
	if (globals::features::ibl.loaded && globals::features::ibl.envIBLTexture && globals::features::ibl.skyIBLTexture) {
		ID3D11ShaderResourceView* iblSRVs[2] = { globals::features::ibl.envIBLTexture->srv.get(), globals::features::ibl.skyIBLTexture->srv.get() };
		context->PSSetShaderResources(76, 2, iblSRVs);
	}
	// The VS reads the same raster for the edge taper; the patch rebinds VS
	// t11 for itself further down. The DS reads it too when tessellating.
	context->VSSetShaderResources(11, 1, &objectTopSRV);
	context->DSSetShaderResources(11, 1, &objectTopSRV);
	// Cone field (t13): the edge taper's single read.
	ID3D11ShaderResourceView* coneSRV = (objectSnowCone && objectSnowCone->srv) ? objectSnowCone->srv.get() : nullptr;
	context->VSSetShaderResources(13, 1, &coneSRV);
	context->DSSetShaderResources(13, 1, &coneSRV);
	context->PSSetShaderResources(13, 1, &coneSRV);
	// Wide exclusion field (t15) + frost crystal patterns (t16/t17): the
	// skin's self-shadow march and spell-mark shading read the landscape
	// shell's slots; the skins draw standalone, so bind explicitly here.
	ID3D11ShaderResourceView* skinExclusionSRV = GetExclusionFieldSRV();
	context->PSSetShaderResources(15, 1, &skinExclusionSRV);
	EnsureFrostPatternTextures();
	ID3D11ShaderResourceView* skinFrostSRVs[2] = { frostPatternNormalSRV.get(), frostPatternDiffuseSRV.get() };
	context->PSSetShaderResources(16, 2, skinFrostSRVs);

	for (const auto& cap : capturedStatics) {
		auto* geometry = cap.geometry.get();
		if (!geometry)
			continue;
		auto triShape = geometry->AsTriShape();
		if (!triShape) {
			logSkip(geometry, "not a BSTriShape");
			continue;
		}
		auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer) {
			logSkip(geometry, "no renderer buffers");
			continue;
		}
		uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		if (indexCount == 0) {
			logSkip(geometry, "zero triangles");
			continue;
		}

		auto desc = rendererData->vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL)) {
			logSkip(geometry, "vertex format lacks POSITION/NORMAL");
			continue;
		}

		// One input layout per distinct vertex descriptor; layouts may carry
		// more elements than the VS consumes, so POSITION+NORMAL suffices.
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		auto& layout = staticsILCache[descKey];
		if (!layout) {
			// Position size = distance to the first following attribute (the
			// descriptor's offset table is authoritative). The VF_FULLPREC
			// flag is NOT reliable: logged runtime buffers carry 16-byte
			// float4 positions with the flag clear, and reading them as
			// halfs shreds geometry into screen-wide streaks.
			uint32_t strideBytes = uint32_t(descKey & 0xF) * 4;
			uint32_t positionBytes = strideBytes;
			static constexpr std::pair<RE::BSGraphics::Vertex::Flags, RE::BSGraphics::Vertex::Attribute> kAttrs[] = {
				{ RE::BSGraphics::Vertex::VF_UV, RE::BSGraphics::Vertex::VA_TEXCOORD0 },
				{ RE::BSGraphics::Vertex::VF_UV_2, RE::BSGraphics::Vertex::VA_TEXCOORD1 },
				{ RE::BSGraphics::Vertex::VF_NORMAL, RE::BSGraphics::Vertex::VA_NORMAL },
				{ RE::BSGraphics::Vertex::VF_TANGENT, RE::BSGraphics::Vertex::VA_BINORMAL },
				{ RE::BSGraphics::Vertex::VF_COLORS, RE::BSGraphics::Vertex::VA_COLOR },
				{ RE::BSGraphics::Vertex::VF_SKINNED, RE::BSGraphics::Vertex::VA_SKINNING },
				{ RE::BSGraphics::Vertex::VF_LANDDATA, RE::BSGraphics::Vertex::VA_LANDDATA },
				{ RE::BSGraphics::Vertex::VF_EYEDATA, RE::BSGraphics::Vertex::VA_EYEDATA },
			};
			for (auto [flag, attr] : kAttrs) {
				if (desc.HasFlag(flag)) {
					uint32_t attrOffset = desc.GetAttributeOffset(attr);
					if (attrOffset > 0 && attrOffset < positionBytes)
						positionBytes = attrOffset;
				}
			}

			D3D11_INPUT_ELEMENT_DESC elements[2] = {
				{ "POSITION", 0, positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			if (FAILED(device->CreateInputLayout(elements, 2, staticsVSBlob->GetBufferPointer(), staticsVSBlob->GetBufferSize(), layout.put())))
				continue;  // null stays cached: this descriptor is skipped from now on
		}
		if (!layout)
			continue;
		context->IASetInputLayout(layout.get());

		// Stride comes from the descriptor's low nibble (in dwords); the
		// same field the game's renderer uses. VertexDesc::GetSize() is NOT
		// equivalent: it reconstructs from flags assuming 16-byte float
		// positions, but most SSE meshes store 8-byte half positions, and
		// the overshot stride shreds vertices into giant garbage triangles.
		UINT stride = uint32_t(descKey & 0xF) * 4;
		if (stride == 0 || desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL) >= stride) {
			logSkip(geometry, "implausible stride/offset");
			continue;
		}
		UINT offset = 0;
		auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
		auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
		context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);

		StaticsCB scb{};
		const auto& rot = cap.world.rotate;
		const float scale = cap.world.scale;
		scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, cap.world.translate.x };
		scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, cap.world.translate.y };
		scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, cap.world.translate.z };
		scb.ObjectsDepth = cap.road ? settings.RoadMeshesDepth : settings.ObjectsSnowDepth;
		scb.RoundedDepth = cap.road ? settings.RoadMeshesDepth : settings.SnowMeshesDepth;
		scb.VertexCountF = float(triShape->GetTrishapeRuntimeData().vertexCount);
		scb.HeightWindowCenter = heightWindowCenter;
		scb.HeightHalfExtent = kHeightMapHalfExtent;
		// Smoothed normals (built once per unique mesh): pillow inflation
		// for flat split-normal surfaces; planks, roofs, pole caps.
		ID3D11ShaderResourceView* smoothSRV = EnsureSmoothedNormals(geometry);
		context->VSSetShaderResources(10, 1, &smoothSRV);
		scb.HasSmoothedNormals = smoothSRV ? 1.0f : 0.0f;
		scb.HasObjectTop = objectTopSRV ? 1.0f : 0.0f;
		scb.SkinHeightFadeEnd = settings.RangeSkinsGeometryM * kUnitsPerMeter;
		scb.LegacySkin = cap.road ? 1.0f : 0.0f;
		scb.MoundSteepness = std::clamp(settings.SnowMoundSteepness, 0.5f, 3.0f);
		scb.ObjectTrenches = settings.ObjectTrenches ? 1.0f : 0.0f;
		scb.SkinDistantBareness = settings.SkinDistantBareness;
		staticsCB->Update(scb);

		context->DrawIndexed(indexCount, 0, 0);
	}
	globals::profiler->EndPass();

	// The trench patch and everything after run the normal pipeline.
	if (tessellateSkins) {
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	// Leave IA clean, mirroring DrawShell's convention (game state manager
	// rebinds via DIRTY_RENDERTARGET).
	ID3D11Buffer* nullVB = nullptr;
	UINT zero = 0;
	context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
	context->IASetInputLayout(nullptr);
	ID3D11ShaderResourceView* nullSmoothSRV = nullptr;
	context->VSSetShaderResources(10, 1, &nullSmoothSRV);
	context->VSSetShaderResources(11, 1, &nullSmoothSRV);
	context->DSSetShaderResources(11, 1, &nullSmoothSRV);
	context->PSSetShaderResources(11, 1, &nullSmoothSRV);
	context->VSSetShaderResources(13, 1, &nullSmoothSRV);
	context->DSSetShaderResources(13, 1, &nullSmoothSRV);
	context->PSSetShaderResources(13, 1, &nullSmoothSRV);

	// trench PATCH: the landscape shell's dense-grid carve applied to object
	// tops; real carved geometry drawn after the skins so it shows through
	// their dithered trench hand-off holes. SV_VertexID grid, no IA state.
	// Per-class trenching emerges from the raster: each captured object
	// writes its own class depth into the skin-depth raster, so a class at
	// 0 produces dead patch texels for its objects only. The pass gate just
	// needs ANY class active (the old > 1 threshold silently disabled the
	// whole patch at depth 1).
	if (patchVS && patchPS && heightSkinDepth && (settings.SnowMeshesDepth > 0.5f || settings.RoadMeshesDepth > 0.5f)) {
		globals::profiler->BeginPass("SnowDeformation::TrenchPatch");
		// Tessellated patch: quad patches with trench-aware factors, so the
		// object trenches pick up the same wall smoothness and rim relief as
		// the landscape shell. Self-sufficient bindings, same rationale as
		// the skins.
		const bool tessellatePatch = settings.Tessellation && patchTessVS && patchHS && patchDS;
		if (tessellatePatch) {
			context->VSSetShader(patchTessVS, nullptr, 0);
			context->HSSetShader(patchHS, nullptr, 0);
			context->DSSetShader(patchDS, nullptr, 0);
			ID3D11Buffer* cb0 = shellCB->CB();
			context->HSSetConstantBuffers(0, 1, &cb0);
			context->DSSetConstantBuffers(0, 1, &cb0);
			ID3D11Buffer* patchCB1 = staticsCB->CB();
			context->HSSetConstantBuffers(1, 1, &patchCB1);
			context->DSSetConstantBuffers(1, 1, &patchCB1);
			ID3D11ShaderResourceView* stageDeformSRV = GetDeformationSRV();
			context->HSSetShaderResources(1, 1, &stageDeformSRV);
			context->DSSetShaderResources(1, 1, &stageDeformSRV);
			ID3D11ShaderResourceView* stageHeightSRV = shellSnowHeightSRV.get();
			context->DSSetShaderResources(8, 1, &stageHeightSRV);
			ID3D11ShaderResourceView* patchStageSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
			context->HSSetShaderResources(11, 2, patchStageSRVs);
			context->DSSetShaderResources(11, 2, patchStageSRVs);
			ID3D11SamplerState* dsSampler = shellSnowSampler.get();
			context->DSSetSamplers(0, 1, &dsSampler);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
		} else {
			context->VSSetShader(patchVS, nullptr, 0);
		}
		context->PSSetShader(patchPS, nullptr, 0);
		// Object top raster (PS t11): the patch's self-shadow march needs the
		// same footprint test as the skins - inside a footprint the tap
		// surface is the object top, not the terrain window's class ramp.
		ID3D11ShaderResourceView* patchTopSRV = heightTopRaw[heightCurrent]->srv.get();
		context->PSSetShaderResources(11, 1, &patchTopSRV);

		StaticsCB scb{};
		// WorldRow0.xy = snapped patch origin (256 quads x 8 units = +-1024
		// around the height-window center, which tracks the camera).
		scb.WorldRow0 = {
			std::floor((heightWindowCenter.x - 1024.0f) / 8.0f) * 8.0f,
			std::floor((heightWindowCenter.y - 1024.0f) / 8.0f) * 8.0f, 0.0f, 0.0f
		};
		scb.ObjectsDepth = settings.ObjectsSnowDepth;
		scb.RoundedDepth = settings.SnowMeshesDepth;
		scb.HeightWindowCenter = heightWindowCenter;
		scb.HeightHalfExtent = kHeightMapHalfExtent;
		// The march's footprint test (see the t11 bind above).
		scb.HasObjectTop = 1.0f;
		// The patch only has texels where the raster already permitted carving,
		// so the per-pixel trench terms must not gate it a second time.
		scb.ObjectTrenches = 1.0f;
		staticsCB->Update(scb);

		ID3D11ShaderResourceView* patchSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
		context->VSSetShaderResources(11, 2, patchSRVs);
		if (tessellatePatch) {
			context->Draw(256 * 256 * 4, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		} else {
			context->Draw(256 * 256 * 6, 0);
		}

		ID3D11ShaderResourceView* nullHeightSRVs[2] = { nullptr, nullptr };
		context->VSSetShaderResources(11, 2, nullHeightSRVs);
		globals::profiler->EndPass();
	}

	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(1, 1, &nullCB);
	context->PSSetConstantBuffers(1, 1, &nullCB);
}

void SnowDeformation::RenderExclusionField()
{
	exclusionFieldValid = false;
	if (!exclusionFieldTexture || !exclusionFieldCB || !shellTerrainTexture)
		return;
	auto* cs = GetExclusionFieldCS();
	if (!cs)
		return;

	auto context = globals::d3d::context;
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();

	// Snap the window to whole texels so a texel keeps covering the same patch
	// of world as the camera moves; an unsnapped window resamples the bowls
	// every frame and their noisy rims crawl.
	constexpr float texelSize = kExclusionFieldHalfExtent * 2.0f / kExclusionFieldDim;
	exclusionFieldCenter = {
		std::floor(eye.x / texelSize) * texelSize,
		std::floor(eye.y / texelSize) * texelSize
	};

	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	ExclusionFieldCB cbData{};
	cbData.FieldCenter = exclusionFieldCenter;
	cbData.FieldHalfExtent = kExclusionFieldHalfExtent;
	cbData.FieldTexelSize = texelSize;
	cbData.TerrainWindowOrigin = { shellWindowCellX * cellSize, shellWindowCellY * cellSize };
	cbData.TerrainTexelSize = kShellVertexSpacing;
	cbData.TerrainDim = kShellWindowDim;
	exclusionFieldCB->Update(cbData);

	ID3D11Buffer* cbs[2] = { exclusionFieldCB->CB(), doorsCB->CB() };
	context->CSSetConstantBuffers(0, 2, cbs);
	ID3D11ShaderResourceView* srv = shellTerrainTexture->srv.get();
	context->CSSetShaderResources(0, 1, &srv);
	ID3D11UnorderedAccessView* uav = exclusionFieldTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(cs, nullptr, 0);

	globals::profiler->BeginPass("SnowDeformation::ExclusionField");
	context->Dispatch(kExclusionFieldDim / 16, kExclusionFieldDim / 16, 1);
	globals::profiler->EndPass();

	ID3D11Buffer* nullCBs[2] = { nullptr, nullptr };
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetConstantBuffers(0, 2, nullCBs);
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	exclusionFieldValid = true;
}
