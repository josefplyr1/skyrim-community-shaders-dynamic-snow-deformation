// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include <DirectXPackedVector.h>
#include "Features/SnowDeformation.h"

#include <d3dcompiler.h>

#include "Features/ExponentialHeightFog.h"
#include "Features/IBL.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

// The engine's projected-noise map, when reachable - the S4 shell's hard
// dependency (its per-pixel footprint cut reconstructs vanilla's weight).
// Checked identically at capture-raster and skin time so both passes pick
// the same class for a draw.
static ID3D11ShaderResourceView* SD_ProjNoiseMapSRV()
{
	auto* graphicsState = globals::game::graphicsState;
	auto* noiseTex = graphicsState ? graphicsState->defaultTextureProjNoiseMap.get() : nullptr;
	return (noiseTex && noiseTex->rendererTexture) ? noiseTex->rendererTexture->resourceView : nullptr;
}

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
// Allocation-free case-insensitive substring; the needle is lowercase ASCII.
static bool ContainsNoCase(const char* a_text, const char* a_needle)
{
	if (!a_text || !a_needle || !*a_needle)
		return false;
	const size_t n = strlen(a_needle);
	for (const char* p = a_text; *p; ++p) {
		size_t i = 0;
		while (i < n && p[i] && (char)std::tolower((unsigned char)p[i]) == a_needle[i])
			++i;
		if (i == n)
			return true;
	}
	return false;
}

// Every name-derived fact the capture hook needs, computed once per interned
// geometry name (BSFixedString: equal content, equal pointer). Validated by
// length and the first eight bytes against pointer reuse. The logged flags
// carry the "one line per unique name" logs that used to hash a std::string
// per capture per frame.
struct GeometryNameFacts
{
	uint32_t length = 0;
	uint64_t head = 0;
	bool bridge = false;
	bool road = false;
	bool mountainCliff = false;
	bool plank = false;
	bool iceFamily = false;
	bool drift = false;
	bool capturedLogged = false;
	bool roundedLogged = false;
	bool plankLogged = false;
	bool decalLogged = false;
};

static GeometryNameFacts& NameFactsOf(RE::BSGeometry* a_geometry)
{
	static std::unordered_map<const char*, GeometryNameFacts> cache;
	static GeometryNameFacts empty;
	const char* name = a_geometry ? a_geometry->name.c_str() : nullptr;
	if (!name || !*name)
		return empty;
	if (cache.size() > 16384)
		cache.clear();
	const uint32_t length = (uint32_t)strlen(name);
	uint64_t head = 0;
	memcpy(&head, name, std::min<size_t>(length, sizeof(head)));
	auto [it, inserted] = cache.try_emplace(name);
	auto& f = it->second;
	if (inserted || f.length != length || f.head != head) {
		f = {};
		f.length = length;
		f.head = head;
		f.bridge = ContainsNoCase(name, "bridge");
		f.road = f.bridge || ContainsNoCase(name, "road");
		f.mountainCliff = ContainsNoCase(name, "mountain") || ContainsNoCase(name, "cliff");
		f.plank = ContainsNoCase(name, "plank") || ContainsNoCase(name, "walkway") || ContainsNoCase(name, "catwalk");
		f.iceFamily = (std::tolower((unsigned char)name[0]) == 'i' && std::tolower((unsigned char)name[1]) == 'c' && std::tolower((unsigned char)name[2]) == 'e') ||
		              ContainsNoCase(name, "glacier") || ContainsNoCase(name, "iceberg");
		// Snow drifts, not shore driftwood (a twig-card class on its diffuse).
		f.drift = ContainsNoCase(name, "drift") && !ContainsNoCase(name, "driftwood");
	}
	return f;
}

static void SampleLODDecision(RE::BSGeometry* a_geometry, float a_radius, bool a_rejected, bool a_cameraInside)
{
	// Own budget per outcome: eight "kept" lines used to spend the whole
	// budget before the first rejection could be read.
	static std::atomic<uint32_t> loggedKept{ 0 };
	static std::atomic<uint32_t> loggedRejected{ 0 };
	auto& logged = a_rejected ? loggedRejected : loggedKept;
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

	// The reference's STAT directional-material record vetoes non-snow
	// projections. Negative keywords only: requiring "snow" in the path
	// vetoed every MATO whose replacer names it differently.
	// Sand/moss/ash keep their veto; an
	// unrecognized path passes. Cached per base form; each new entry is
	// logged so the modlist's actual MATO names are in CommunityShaders.log.
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
				// The MODL path is junk on vanilla MATO records (Bethesda left
				// 'shadertests\shaderbox.nif' in most of them — the whole
				// modlist logged that one path), so the editor ID carries the
				// real identity (SnowMaterialObjectNoise1P, SandMaterialObject
				// ...). Runtime editor IDs need po3 Tweaks; when absent the
				// signal degrades to the path alone.
				std::string matoPath;
				if (const char* edid = stat->data.materialObj->GetFormEditorID())
					matoPath = edid;
				matoPath += '|';
				matoPath += stat->data.materialObj->GetModel();
				std::transform(matoPath.begin(), matoPath.end(), matoPath.begin(),
					[](unsigned char c) { return (char)std::tolower(c); });
				if (matoPath.find("snow") != std::string::npos) {
					it->second = MatoClass::kSnow;
				} else {
					static constexpr std::array kNotSnowKeywords{ "sand", "moss", "dirt", "mud", "gravel", "ash", "coast" };
					it->second = MatoClass::kNoMato;
					for (const auto* keyword : kNotSnowKeywords) {
						if (matoPath.find(keyword) != std::string::npos) {
							it->second = MatoClass::kNotSnow;
							break;
						}
					}
				}
				logger::info("[SNOW DEFORMATION] Projected MATO on base {:08X}: '{}' -> {}", base->GetFormID(), matoPath,
					it->second == MatoClass::kSnow ? "snow" : (it->second == MatoClass::kNotSnow ? "NOT snow (vetoed)" : "neutral"));
			} else {
				logger::info("[SNOW DEFORMATION] Projected base {:08X}: no STAT MATO -> neutral", base->GetFormID());
			}
		}
		return it->second;
	}

	// Diffuse-path facts, cached per material and shared by the flags-fail
	// acceptance, the range-cap exemption and the capture's fade flag.
	// Caching the ACCEPT decision instead would be wrong: the same material
	// can reach the hook as both LOD and non-LOD, and the LOD path accepts
	// more.
	struct SnowPathMatch
	{
		// Drifts wear plain LANDSCAPE snow textures (no "drift" in the
		// path); requiring the landscape folder keeps frosted plants
		// (plant/tree folders) out.
		bool base = false;
		// The glacier/iceberg family wears baked snow the projected match
		// can never recolor. "ice" is deliberately NOT matched (hits
		// lattice/office/service); "mountain" is not matched either, since
		// shore rocks share the diffuse, their LOD hangs off no reference, and
		// distant snowy mountainsides are Horizon Snow's job. The merged-
		// DynDOLOD-atlas experiment was retired the same day.
		bool naturalFeature = false;
		// Mountain/cliff diffuse. Not a capture family on its own (see above);
		// read only for large-reference LOD, which has no other snow signal.
		bool mountain = false;
	};

	// Pointer-identity ownership: does any loaded reference's 3D subtree
	// contain this geometry? The userData walk fails on some real meshes
	// (a glacier's base never reaches the MATO log), and the unreferenced
	// check then mistook them for merged Windhelm sheets whenever the camera
	// stood inside their footprint - for a glacier underfoot, always. Exact and
	// heuristic-free; merged LOD genuinely belongs to no reference. Only
	// candidates already big + camera-inside + walk-unreferenced get here;
	// cached per geometry, render thread only like the other caches.
	bool GeometryBelongsToLoadedReference(RE::BSGeometry* a_geometry)
	{
		static std::unordered_map<const void*, bool> ownershipCache;
		if (ownershipCache.size() > 4096)
			ownershipCache.clear();
		auto [it, inserted] = ownershipCache.try_emplace(a_geometry, false);
		if (!inserted)
			return it->second;
		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return false;
		const RE::NiPoint3 center = a_geometry->worldBound.center;
		const float reach = a_geometry->worldBound.radius + 1024.0f;
		bool found = false;
		tes->ForEachReference([&](RE::TESObjectREFR* a_ref) {
			if (!a_ref || a_ref->GetPosition().GetDistance(center) > reach)
				return RE::BSContainer::ForEachResult::kContinue;
			auto* root = a_ref->Get3D();
			if (!root)
				return RE::BSContainer::ForEachResult::kContinue;
			std::vector<RE::NiAVObject*> stack{ root };
			while (!stack.empty()) {
				RE::NiAVObject* node = stack.back();
				stack.pop_back();
				if (node == a_geometry) {
					found = true;
					return RE::BSContainer::ForEachResult::kStop;
				}
				if (auto* niNode = node->AsNode()) {
					for (auto& child : niNode->GetChildren())
						if (child)
							stack.push_back(child.get());
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		it->second = found;
		return found;
	}

	// Mesh-name ice family (Iceberg*, Glacier*, IcePile*): texture paths alone
	// miss several. Bare "ice" is only safe as a name prefix - as a substring
	// it hits Cornice/Device.
	bool IsIceFamilyGeometry(RE::BSGeometry* a_geometry)
	{
		return NameFactsOf(a_geometry).iceFamily;
	}

	// The combined family signal (node name OR diffuse path), cached per
	// geometry and per material: SetProjectedSnowBit runs on every Lighting
	// draw, so the lowercase transforms cannot run per call.
	bool IceFamilySignal(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase* a_material)
	{
		static std::unordered_map<const void*, bool> nameCache;
		if (nameCache.size() > 4096)
			nameCache.clear();
		auto [nameIt, nameInserted] = nameCache.try_emplace(a_geometry, false);
		if (nameInserted)
			nameIt->second = IsIceFamilyGeometry(a_geometry);
		if (nameIt->second)
			return true;
		if (!a_material)
			return false;
		static std::unordered_map<const void*, bool> texCache;
		if (texCache.size() > 4096)
			texCache.clear();
		auto [texIt, texInserted] = texCache.try_emplace(a_material, false);
		if (texInserted) {
			if (auto textureSet = a_material->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse); path) {
					std::string lowered(path);
					std::transform(lowered.begin(), lowered.end(), lowered.begin(),
						[](unsigned char c) { return (char)std::tolower(c); });
					texIt->second = lowered.find("glacier") != std::string::npos ||
					                lowered.find("iceberg") != std::string::npos;
				}
			}
		}
		return texIt->second;
	}

	// One-shot journey log for the ice family (glacier/iceberg): the skins
	// keep failing at SOME gate and the gates key on different identity
	// signals (geometry node name vs texture path vs shader flags). Any
	// signal matching logs the geometry's facts and which gate ended this
	// pass's journey, once per geometry+outcome; the outcome must be a
	// string literal (the dedup keys on its address). Render thread only,
	// like the other caches.
	void LogIceJourney(RE::BSRenderPass* a_pass, bool a_ice, const char* a_outcome)
	{
		if (!a_ice)
			return;
		auto* geometry = a_pass->geometry;
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		static std::unordered_set<uint64_t> logged;
		if (logged.size() > 512)
			return;
		if (!logged.insert((uint64_t)(uintptr_t)geometry ^ ((uint64_t)(uintptr_t)a_outcome << 1)).second)
			return;
		const char* texPath = "";
		if (material) {
			if (auto textureSet = material->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse); path)
					texPath = path;
			}
		}
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto& flags = a_pass->shaderProperty->flags;
		RE::TESObjectREFR* refr = nullptr;
		for (RE::NiAVObject* node = geometry; node && !refr; node = node->parent)
			refr = static_cast<RE::TESObjectREFR*>(node->GetUserData());
		logger::info("[SNOW DEFORMATION] ice journey '{}' r={:.0f} proj={} snow={} lodObj={} hdLod={} lodLand={} ref={:08X} tex='{}' -> {}",
			geometry->name.empty() ? "<unnamed>" : geometry->name.c_str(),
			geometry->worldBound.radius,
			flags.all(Flag::kProjectedUV) ? 1 : 0, flags.all(Flag::kSnow) ? 1 : 0,
			flags.all(Flag::kLODObjects) ? 1 : 0, flags.all(Flag::kHDLODObjects) ? 1 : 0,
			flags.all(Flag::kLODLandscape) ? 1 : 0,
			refr && refr->GetBaseObject() ? refr->GetBaseObject()->GetFormID() : 0u,
			texPath, a_outcome);
	}

	const SnowPathMatch& ClassifySnowPath(RE::BSLightingShaderMaterialBase* a_material)
	{
		static std::unordered_map<const void*, SnowPathMatch> pathCache;
		if (pathCache.size() > 4096)
			pathCache.clear();
		auto [it, inserted] = pathCache.try_emplace(a_material, SnowPathMatch{});
		if (inserted) {
			if (auto textureSet = a_material->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					std::string lowered(path);
					std::transform(lowered.begin(), lowered.end(), lowered.begin(),
						[](unsigned char c) { return (char)std::tolower(c); });
					it->second.base = lowered.find("drift") != std::string::npos ||
					                  (lowered.find("landscape") != std::string::npos && lowered.find("snow") != std::string::npos);
					it->second.naturalFeature = lowered.find("glacier") != std::string::npos ||
					                            lowered.find("iceberg") != std::string::npos;
					it->second.mountain = lowered.find("mountain") != std::string::npos ||
					                      lowered.find("cliff") != std::string::npos;
				}
			}
		}
		return it->second;
	}
}

// One record per geometry: every material-, name- and reference-derived fact
// the two capture hooks read, so a draw pays one lookup (and the second hook
// on the same draw none). Validated by the material and interned-name
// pointers; a mismatch rebuilds from the per-material caches above. The MATO
// class rides the record too: a geometry re-parented under another reference
// keeps its old class until material or name changes.
struct GeometryRecord
{
	const void* material = nullptr;
	const char* name = nullptr;
	bool pathBase = false;
	bool pathNatural = false;
	bool pathMountain = false;
	bool iceName = false;
	bool ice = false;
	bool shard = false;
	uint8_t roadTex = 0;  // 0 none, 1 road, 2 bridge
	MatoClass mato = MatoClass::kNoReference;
};

static GeometryRecord& RecordOf(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase* a_material)
{
	static std::unordered_map<const void*, GeometryRecord> cache;
	static const void* lastGeometry = nullptr;
	static GeometryRecord* lastRecord = nullptr;
	const char* name = a_geometry->name.c_str();
	if (lastRecord && lastGeometry == a_geometry && lastRecord->material == a_material && lastRecord->name == name)
		return *lastRecord;
	if (cache.size() > 16384) {
		cache.clear();
		lastRecord = nullptr;
	}
	auto [it, inserted] = cache.try_emplace(a_geometry);
	auto& r = it->second;
	if (inserted || r.material != a_material || r.name != name) {
		r = {};
		r.material = a_material;
		r.name = name;
		r.iceName = NameFactsOf(a_geometry).iceFamily;
		if (a_material) {
			const SnowPathMatch& path = ClassifySnowPath(a_material);
			r.pathBase = path.base;
			r.pathNatural = path.naturalFeature;
			r.pathMountain = path.mountain;
			if (auto textureSet = a_material->textureSet.get()) {
				if (auto diffuse = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					r.shard = ContainsNoCase(diffuse, "branchpile") || ContainsNoCase(diffuse, "driftwood");
					r.roadTex = ContainsNoCase(diffuse, "bridge") ? 2 : (ContainsNoCase(diffuse, "road") ? 1 : 0);
				}
			}
		}
		// The texture half of IceFamilySignal is the natural-feature test.
		r.ice = r.iceName || r.pathNatural;
		r.mato = ClassifyProjectedMato(a_geometry);
	}
	lastGeometry = a_geometry;
	lastRecord = &r;
	return r;
}

void SnowDeformation::SetProjectedSnowBit(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass)
{
	ScopedTicks _bitTicks(cpuCensus.hookTicks, cpuCensus.hookBitCalls);
	// Projected-snow bit for Lighting's material match (SNOW-MATCH Phase 2):
	// cleared every pass so it never leaks, set when this draw's projected
	// material is actually snow. Classified from the PASS TECHNIQUE
	// (currentRawTechnique — the TruePBR precedent), NOT the property flags:
	// the game renders projected snow as its own pass whose technique
	// carries ProjectedUV (frame7075's fence: descriptor 100E201) while the
	// property need not carry kProjectedUV or kSnow — gating on property
	// flags is what kept this bit off the fence through two rounds. Sand
	// and moss stay excluded by their MATO (kNotSnow). Runs BEFORE the
	// game's SetupGeometry (the ExtendedTranslucency pattern): the
	// descriptor is consumed inside it.
	auto& extraDescriptor = globals::state->permutationData.ExtraFeatureDescriptor;
	extraDescriptor &= ~(uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow) | uint32_t(State::ExtraFeatureDescriptors::SnowLODBakedIsSnow));
	if (!a_shader || !a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	if (!settings.EnableSnowDeformation || !shellSnowDiffuseSRV)
		return;
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	bool bindSnowSet = false;
	if (settings.ProjSnowMatch) {
		const bool passProjected = (a_shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) != 0;
		if (!passProjected || a_pass->shaderProperty->flags.all(Flag::kTreeAnim)) {
			statProjNoProjection.fetch_add(1, std::memory_order_relaxed);
		} else if (RecordOf(a_pass->geometry, static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material)).mato == MatoClass::kNotSnow) {
			statProjVetoed.fetch_add(1, std::memory_order_relaxed);
		} else {
			statProjMatched.fetch_add(1, std::memory_order_relaxed);
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow);
			bindSnowSet = true;
		}
	}
	// Plain object LOD: DynDOLOD's unflagged batches (drifts, roads, piles
	// past the loaded grid) carry baked snow in the atlas and no projection,
	// so the hook never sees them; Lighting classifies their texels like the
	// horizon terrain. Snow-flagged LOD stays on the projected path above.
	if (settings.LODObjectSnow && !bindSnowSet &&
		a_pass->shaderProperty->flags.any(Flag::kLODObjects, Flag::kHDLODObjects) &&
		!a_pass->shaderProperty->flags.all(Flag::kTreeAnim) &&
		(a_shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) == 0) {
		extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowLODBakedIsSnow);
		bindSnowSet = true;
	}
	if (bindSnowSet) {
		// The Prepass-time t102/t103 bind does NOT survive to the Lighting
		// draws (frame7075: null at every player-view draw — stomped around
		// the cubemap pass; t101 only survives via its later re-bind).
		// Re-bind per classified draw, where it is actually sampled.
		ID3D11ShaderResourceView* horizonSnowSRVs[2] = { shellSnowDiffuseSRV.get(), shellSnowNormalSRV.get() };
		globals::d3d::context->PSSetShaderResources(102, 2, horizonSnowSRVs);
	}
}

void SnowDeformation::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	ScopedTicks _hookTicks(cpuCensus.hookTicks, cpuCensus.hookCalls);
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

	// The main pass's depth range, from the frame's first non-reflection
	// lighting draw: DrawShell runs after the blended decals and would
	// otherwise inherit their viewport (see mainViewportMaxDepth).
	if (mainViewportFrame != globals::state->frameCount &&
		!(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) {
		UINT count = 1;
		D3D11_VIEWPORT vp{};
		globals::d3d::context->RSGetViewports(&count, &vp);
		if (count) {
			mainViewportMinDepth = vp.MinDepth;
			mainViewportMaxDepth = vp.MaxDepth;
			mainViewportFrame = globals::state->frameCount;
		}
	}

	if (landTriProbeArmed && !landTriProbe.pending)
		CaptureLandTriProbe(a_pass);

	// One verdict per geometry per frame: the same geometry arrives once per
	// pass that draws it, and nothing below depends on the pass. A capture
	// already returned at the set insert; this returns the rejections too.
	// The clean gate: projected-UV + snow flags together; covers rocks,
	// roofs, logs, stumps and never flora, because foliage is not
	// snow-PROJECTED. Drifts (no flags at all) qualify via a NARROW texture
	// match; a catch-all "snow" match drags frosted bushes in, whose leaf
	// cards shard under the skin.
	auto& rec = RecordOf(a_pass->geometry, static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material));
	// Drifts ride the ice journey log: same once-per-outcome budget.
	const bool driftJourney = NameFactsOf(a_pass->geometry).drift;
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	const auto& flags = a_pass->shaderProperty->flags;
	// Animated flora never qualifies: card meshes shard under the skin.
	if (flags.all(Flag::kTreeAnim)) {
		LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: tree-anim flag");
		return;
	}
	// Skinned geometry never qualifies: with the family acceptance no
	// longer LOD-only, an "Ice"-prefixed actor mesh (ice wraith) would
	// otherwise capture and drag a static skin behind a moving creature.
	if (a_pass->geometry->GetGeometryRuntimeData().skinInstance != nullptr) {
		LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: skinned geometry");
		return;
	}
	// Ice-family meshes keep their skins at every range: the always-covered
	// LOD family look is the acceptance criterion, and a skinless loaded
	// glacier reads as having no snow even with the baked-snow recolor
	// active. The recolor rides
	// underneath as a second layer: it tints the baked snow that shows
	// through skin gaps and beyond the raster window. The skin's
	// structural limits here (58 m conforming window, facet squares,
	// stacked trishape layers, rim gaps) stay known; the recolor's job is
	// to soften what they expose.
	// Merged LOD spans a whole worldspace quad; nothing belonging to a single
	// reference comes close. Windhelm's merged quads measured 8700-12608.
	constexpr float kMergedLODRadius = 4096.0f;

	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();

	// Object LOD batches: the game draws a DynDOLOD BTO one segment per
	// reference or cell, segments off where the real model is loaded, and a
	// capture draws the whole buffer (Josef's ghost shell, RenderDoc
	// 2026-09-10: objSnowHD-LargeRef, the game's 900 indices of our 6372).
	// Flagged, not rejected: the skin PS keeps a pixel only where the hull
	// is the scene surface.
	auto& nameFacts = NameFactsOf(a_pass->geometry);
	const bool lodBatch = flags.any(Flag::kLODObjects, Flag::kHDLODObjects);
	// Read after SetupGeometry: the raster depth-bias mode this draw uses.
	// Decal mode writes depth with a slope bias (-0.65/px) and a shorter
	// viewport range, so at grazing views the mesh's own depth sits far
	// nearer than the geometry; the skin has to draw through the same state
	// (Windhelm paving, RenderDoc 2026-09-10).
	const uint32_t depthBiasMode = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().rasterStateDepthBiasMode;
	const bool decalDepth = depthBiasMode != 0 || flags.any(Flag::kDecal, Flag::kDynamicDecal);

	// Merged LOD sheets, discriminated by CONTAINMENT rather than by span.
	//
	// Distant objects and Windhelm's sheets are the same asset class at the
	// same sizes, so neither the LOD flags nor worldBound.radius separates
	// them. What does is whether the LOD is REDUNDANT: inside the loaded
	// region the real meshes draw too, so a skin on the co-drawn LOD is a
	// second surface cutting through them; outside it the LOD is the object's
	// only representation and must be skinned. "Am I standing inside it" is
	// the test, with the unreferenced check as the second half - merged LOD
	// hangs off no TESObjectREFR.
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
			// The userData walk is a fallible proxy: real glacier meshes walk
			// as unreferenced too, and this rejection then ate them whenever
			// the camera stood inside their footprint. Settle it exactly
			// before rejecting.
			if (!referenced)
				referenced = GeometryBelongsToLoadedReference(a_pass->geometry);
			// Ice-family sheets are exempt: the containment rule protects
			// Windhelm's stone sheets, but a glacier LOD batch is rejected
			// exactly while the camera stands inside its span - precisely
			// where the glacier field below must stay skinned - and on the
			// world map the panning camera strobed the whole field on and
			// off across sheet boundaries.
			const bool iceSheet = rec.pathNatural;
			if (!referenced && !iceSheet && !lodBatch) {
				LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: containment (big, camera inside, no owning reference found)");
				SampleLODDecision(a_pass->geometry, wb.radius, true, false);
				return;
			}
		}
		if (flags.any(Flag::kLODObjects, Flag::kHDLODObjects, Flag::kLODLandscape))
			SampleLODDecision(a_pass->geometry, wb.radius, false, cameraInside);
	}
	// Projected snow paint is the drape's whole input and Lighting recolors
	// on PROJECTED_UV alone; the Snow flag adds nothing, and a PBR retexture
	// drops it (Dawnstar's cliff, Alftand's dome, loaded rocks - bare with
	// the recolor on them). Only draws WITHOUT projection data face the
	// family gate below.
	if (!flags.all(Flag::kProjectedUV)) {
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		if (!material)
			return;

		// Object LOD only. Terrain LOD (kLODLandscape) belongs to the shell and
		// the horizon recolor, never to object snow.
		const bool isObjectLOD = flags.any(Flag::kLODObjects, Flag::kHDLODObjects);

		// Texture family OR mesh-name family: several glacier/ice meshes
		// carry non-family texture paths. The MATO only vetoes (kNotSnow,
		// the sand-shore rocks); requiring positive
		// snow MATOs wrongly rejected glaciers, whose snow is baked and
		// needs no projection record.
		// The mountain/cliff family too, on snowy ground: LOD batches carry
		// the family's texture paths and no projection data.
		const auto& wbCenter = a_pass->geometry->worldBound.center;
		const bool mountainFeature = (rec.pathMountain || nameFacts.mountainCliff) &&
		                             GetNominalSnowDepthAt(wbCenter.x, wbCenter.y, 0.0f) > 0.5f;
		bool naturalFeature = rec.pathNatural || rec.iceName || mountainFeature;
		bool matoVetoed = false;
		if (naturalFeature && rec.mato == MatoClass::kNotSnow) {
			naturalFeature = false;
			matoVetoed = true;
		}
		// Family accepts at any range, not just LOD: loaded glacier/ice meshes
		// carry no proj/snow flags when PBR glacier textures replace the
		// vanilla projected-snow setup, while their LOD counterparts capture
		// normally. LOD-only acceptance was the whole
		// bare-glacier bug). The MATO veto stands.
		// Plain object LOD passes whole: Lighting recolors every such batch by
		// texel brightness (SetProjectedSnowBit) and writes the weight back,
		// and the coat reads that weight and nothing else (the atlas holds
		// ships and walls beside the mountains).
		if (!(rec.pathBase || naturalFeature || isObjectLOD)) {
			if (matoVetoed)
				LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: family matched but MATO vetoed (kNotSnow)");
			else
				LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: no family signal at material gate (name/texture both missed)");
			return;
		}
	}


	// Twig-card shape class (branch piles, shore driftwood): vanilla flags
	// them snow-projected so they pass the flag gate, but the capture sees
	// sparse cards and the skin wraps them into broken shards. Name-matched
	// on the diffuse path; extend the list as offenders surface.
	if (rec.shard) {
		LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: twig-card shape class");
		return;
	}

	// Range cap (Object Snow slider): distant mountains are snow-projected
	// everywhere in Skyrim; the skin only matters within the chosen range.
	// Glacier/iceberg captures are exempt: their baked snow is far whiter
	// than the shell and no
	// projection exists for the match to recolor, so between the skin range
	// and cell unload they stood out bright; the skin now covers them at
	// every loaded distance and skips the fade to match.
	const bool fadeExempt = rec.pathNatural || rec.iceName;
	// Drifts: the mesh IS the snow, and at range the vanilla material reads
	// far brighter than the shell (Josef, 2026-09-06). Coated whole at every
	// loaded distance; the projection default below still applies.
	const bool fullCoat = nameFacts.drift;
	const auto& translate = a_pass->geometry->world.translate;
	float dx = translate.x - eye.x;
	float dy = translate.y - eye.y;
	const float captureRange = settings.RangeSkinsM * kUnitsPerMeter;
	if (!fadeExempt && !fullCoat && dx * dx + dy * dy > captureRange * captureRange) {
		LogIceJourney(a_pass, rec.ice || driftJourney, "rejected: range cap despite family signal (fadeExempt did not fire)");
		return;
	}

	// The same geometry renders through multiple passes; capture once.
	if (!capturedStaticsSet.insert(a_pass->geometry).second)
		return;
	LogIceJourney(a_pass, rec.ice || driftJourney, fadeExempt ? "CAPTURED (fadeExempt)" : "CAPTURED (range-faded)");

	// Road-mesh model class: deterministic NAME + texture-path match. The
	// name check matters: road models are built from MULTIPLE trishapes
	// ('RoadChunk...:0', ':2'), and only some wear road textures; matching
	// textures alone splits one road across two depth settings, stacking a
	// second hovering shell.
	// `bridge` is tracked apart from `road` for the road heightfield only:
	// both classes share RoadMeshesDepth exactly as before.
	bool bridge = nameFacts.bridge;
	bool road = nameFacts.road;
	// Which signal decided it, for the road-classification log below.
	const char* roadVia = road ? "name" : "no";
	if (!road && rec.roadTex != 0) {
		road = true;
		bridge = rec.roadTex == 2;
		roadVia = "texture";
	}

	// Capture log, one line per unique geometry name: classification plus the
	// diffuse. Began as the road-class log; widened to EVERY capture because
	// the question "was this trishape captured at all, and as what?" keeps
	// being the fork in a diagnosis - RoadChunkS03's ':1' paper sheet is
	// either an uncaptured vanilla snow drape or our own skin on a
	// non-road-named trishape, and only this log can say which.
	if (!nameFacts.capturedLogged) {
		nameFacts.capturedLogged = true;
		const char* diffusePath = "";
		if (auto* logMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material))
			if (auto textureSet = logMaterial->textureSet.get())
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
					diffusePath = path;
		logger::info("[SNOW DEFORMATION] captured '{}' road={} (via {}){} heightfield={} tex='{}'",
			a_pass->geometry->name.c_str(), road ? "yes" : "no", roadVia, bridge ? " (BRIDGE)" : "",
			(road && !bridge && settings.RoadHeightfield) ? "yes" : "no", diffusePath);
	}

	// Vanilla's projected-UV threshold, for the S0 mask view and the S2
	// placement suppressor (SKIN-PLACEMENT-PLAN.md). -1 = no projection data
	// on this draw. Tree-anim meshes are sentineled too: their vertex alpha
	// is wind weight, not a snow mask, and vanilla forces alpha 1 on them.
	float projThreshold = -1.0f;
	float projNoiseScale = 0.0f;
	float projNoiseTiling = 0.0f;
	bool projReal = false;
	{
		const auto& capFlags = a_pass->shaderProperty->flags;
		using CapFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (capFlags.any(CapFlag::kProjectedUV) && !capFlags.any(CapFlag::kTreeAnim)) {
			const auto& projParams = static_cast<RE::BSLightingShaderProperty*>(a_pass->shaderProperty)->projectedUVParams;
			projThreshold = projParams.alpha;
			projNoiseScale = projParams.red;
			projNoiseTiling = projParams.blue;
			projReal = true;
		} else if (!capFlags.any(CapFlag::kTreeAnim) && !fadeExempt) {
			// The classification-key mismatch closed (Josef's fence,
			// 2026-08-29): capture is technique-classified, so everything
			// here IS projected snow to the recolor - but mesh-replacer
			// statics (fences, drifts) carry no kProjectedUV on the
			// PROPERTY and the S4 shell skipped them. Default them fully
			// painted: threshold 0, no noise term, authored alpha still
			// honored where the mesh carries vertex colors (vanilla's own
			// PROJECTED_UV path reads Color.w on these draws too). Trees
			// stay sentineled (wind alpha); the ice family keeps its
			// structural exclusion.
			projThreshold = 0.0f;
		}
	}

	// Mountain/cliff family: force the rounded class. A jagged low-poly
	// cliff's split normals score "flat" under the divergence classifier and
	// the mesh drapes with a rigid plate lifted the full flat depth - the
	// hovering translucent film. Name match like the road class; a false
	// positive forces rounded on something already rounded, a no-op.
	const bool forceRounded = nameFacts.mountainCliff;
	if (decalDepth && !nameFacts.decalLogged) {
		nameFacts.decalLogged = true;
		logger::info("[SNOW DEFORMATION] decal depth mode {} (decal flags {}): '{}'", depthBiasMode, flags.any(Flag::kDecal, Flag::kDynamicDecal) ? 1 : 0, a_pass->geometry->name.c_str());
	}
	if (forceRounded && !nameFacts.roundedLogged) {
		nameFacts.roundedLogged = true;
		logger::info("[SNOW DEFORMATION] forced ROUNDED class (mountain/cliff family): '{}'", a_pass->geometry->name.c_str());
	}
	// Plank family: in authored-relief mode the ONLY flat-class draws
	// (cornice treatment, own fill slider); everything else PD is
	// rounded. Same deterministic name match as the road class.
	const bool plankFamily = nameFacts.plank;
	if (plankFamily && !nameFacts.plankLogged) {
		nameFacts.plankLogged = true;
		logger::info("[SNOW DEFORMATION] plank family (flat class in authored relief): '{}'", a_pass->geometry->name.c_str());
	}

	capturedStatics.push_back({ RE::NiPointer<RE::BSGeometry>(a_pass->geometry), a_pass->geometry->world, road, bridge, fadeExempt || fullCoat, projThreshold, projNoiseScale, projNoiseTiling, forceRounded, plankFamily, projReal, fullCoat, lodBatch, decalDepth });
}

struct SD_BSLightingShader_SetupGeometry
{
	static void thunk(RE::BSLightingShader* shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
	{
		auto& snowDeformation = globals::features::snowDeformation;
		if (snowDeformation.loaded)
			snowDeformation.SetProjectedSnowBit(shader, a_pass);

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
static ID3DBlob* SD_CompileShaderBlob(const wchar_t* a_path, const char* a_target, const char* a_stageDefine, const char* a_extraDefine = nullptr, const char* a_extraDefine2 = nullptr, const char* a_extraDefine3 = nullptr, const char* a_extraDefine4 = nullptr)
{
	// Blob disk cache (see ShaderPrime.cpp): the fixed flag set below is part
	// of the "sdblob" env token, and the full key round-trips through the
	// stored file so a fingerprint or define change reads as a miss.
	auto& snow = globals::features::snowDeformation;
	// The fourth define is appended only when present so every existing
	// cache key keeps its text.
	auto defs = std::format("{};{};{};{}", a_stageDefine,
		a_extraDefine ? a_extraDefine : "", a_extraDefine2 ? a_extraDefine2 : "", a_extraDefine3 ? a_extraDefine3 : "");
	if (a_extraDefine4)
		defs += std::format(";{}", a_extraDefine4);
	const auto pathUtf8 = Util::WStringToString(a_path);
	const auto cacheFile = std::format("{}_{}_blob_{:016x}.bin",
		std::filesystem::path(a_path).stem().string(), a_target,
		SnowDeformation::ShaderKeyHash(std::format("{}|{}|{}", pathUtf8, a_target, defs)));
	const auto cacheKey = std::format("v1|fp{}|{}|{}|env:sdblob|defs:{}",
		snow.ShaderSourcesFingerprint(), pathUtf8, a_target, defs);
	if (auto cached = snow.ShaderCacheLoad(cacheKey, cacheFile))
		return cached.detach();

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
		{ a_extraDefine4 ? a_extraDefine4 : "DX11", "" },
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
	if (blob)
		snow.ShaderCacheStore(cacheKey, cacheFile, blob);
	return blob;
}

void SnowDeformation::FillPatchDrawCB(StaticsCB& a_scb) const
{
	// WorldRow0.xy = snapped patch CENTRE. The grid is warped (see
	// kPatchBandVerts in SnowStaticsShell.hlsl), so the vertex shader
	// places about the centre rather than stepping from a corner, and the
	// snap has to be the COARSEST band step - snapping to the fine step
	// would leave outer vertices off their own band's lattice and quad
	// widths would flip as the camera moves.
	a_scb.WorldRow0 = {
		std::floor(heightWindowCenter.x / kPatchSnap) * kPatchSnap,
		std::floor(heightWindowCenter.y / kPatchSnap) * kPatchSnap, 0.0f, 0.0f
	};
	// Objects carry no depth; roads carry theirs per texel in the skin-depth raster.
	a_scb.ObjectsDepth = 0.0f;
	a_scb.RoundedDepth = 0.0f;
	a_scb.HeightWindowCenter = heightWindowCenter;
	a_scb.HeightHalfExtent = ObjectRasterHalfExtent();
	// The patch stays on the coarse maps: its pass does not bind t33/t34, and
	// a stale non-zero here would send PatchTop to an unbound texture, whose
	// zero reads as an object top at world Z 0.
	a_scb.FineHalfExtent = 0.0f;
	// The march's footprint test (t11 in the visible pass).
	a_scb.HasObjectTop = 1.0f;
	// Global gate here, not a per-draw class: the patch is one draw and
	// reads the road bit per texel from the raster's G channel.
	a_scb.RoadField = settings.RoadHeightfield ? 1.0f : 0.0f;
}

ID3D11VertexShader* SnowDeformation::GetPatchShadowVS()
{
	// Lazy like GetShellShadowVS: the shadow pass runs before DrawShell has
	// had a chance to create the statics shaders on early frames.
	if (!patchShadowVS) {
		constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl";
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "PATCH", "SNOW_SHADOW_CAST"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchShadowVS)))
				Util::SetResourceName(patchShadowVS, "SnowDeformation::TrenchPatchShadowVS");
		}
	}
	return patchShadowVS;
}

bool SnowDeformation::EnsureStaticsShaders()
{
	if (staticsVS && staticsPS)
		return true;
	LoadTraceScope _loadTrace(this, "Statics: EnsureStaticsShaders (compile)");
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

	// No-depth-export twin. Only the parallax carve needs SV_Depth, and only
	// road draws carve, so every other captured static writes back the depth
	// the rasteriser already had - paying the loss of early-Z across the whole
	// pass for nothing.
	// A compile failure here is not fatal: the draw falls back to staticsPS.
	if (!staticsPSNoDepth) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSNoDepth)))
				Util::SetResourceName(staticsPSNoDepth, "SnowDeformation::StaticsShellPS NoDepth");
		}
	}

	// Depth-prepass twin of the no-export shader: the alpha cut with the
	// shading compiled out. Carving draws have no twin on purpose (see the
	// shader). A compile failure just leaves the pass on its single loop.
	if (!staticsPSPrepassNoDepth) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DEPTH_PREPASS"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSPrepassNoDepth)))
				Util::SetResourceName(staticsPSPrepassNoDepth, "SnowDeformation::StaticsShellPS Prepass NoDepth");
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
	if (!heightPeelPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "ps_5_0", "PSHADER", "PEEL"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightPeelPS)))
				Util::SetResourceName(heightPeelPS, "SnowDeformation::HeightPeelPS");
		}
	}
	// Volume snow (VOXEL): brick VS + marching PS. Optional; the draw guards
	// on the pointers.
	if (!voxelShellVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "VOXEL"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &voxelShellVS)))
				Util::SetResourceName(voxelShellVS, "SnowDeformation::VolumeSnowVS");
		}
	}
	if (!voxelShellPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "VOXEL"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &voxelShellPS)))
				Util::SetResourceName(voxelShellPS, "SnowDeformation::VolumeSnowPS");
		}
	}
	constexpr auto processPath = L"Data\\Shaders\\SnowDeformation\\HeightMapProcessCS.hlsl";
	if (!heightScrollCS)
		heightScrollCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ScrollCS"));
	if (!heightCombineCS)
		heightCombineCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "CombineCS"));
	if (!heightConeCS)
		heightConeCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ConeCS"));
	if (!objectConeSeedCS)
		objectConeSeedCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectConeSeedCS"));
	if (!objectConeCS)
		objectConeCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectConeCS"));
	if (!objectSkyOpenCS)
		objectSkyOpenCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectSkyOpenCS"));
	if (!objectConeDiffuseCS)
		objectConeDiffuseCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectConeDiffuseCS"));

	if (!staticsVS || !staticsPS || !heightVS || !heightPS || !heightScrollCS || !heightCombineCS || !heightConeCS) {
		staticsShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Statics skin disabled (shader compilation failed)");
		return false;
	}
	return true;
}

void SnowDeformation::CreateHeightFieldResources()
{
	LoadTraceScope _loadTrace(this, "Statics: CreateHeightFieldResources");
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
	// The peeled second layer: the layer-1 cone seed's plane-continuation test.
	heightTop2Raw[0] = makeHeightTexture("SnowDeformation::HeightTop2Raw0");
	heightTop2Raw[1] = makeHeightTexture("SnowDeformation::HeightTop2Raw1");
	// Near clipmap level 0: same grid, quarter reach, so one texel is one
	// world unit. Two textures, not six - no ghost pair, and the cone chain
	// borrows heightScratch (the passes are sequential and same-sized).
	heightTopRawFine = makeHeightTexture("SnowDeformation::HeightTopRawFine");
	objectSnowConeFine = makeHeightTexture("SnowDeformation::ObjectSnowConeFine");
	// P3: the sky-openness field at half the raster's resolution - a soft
	// field, and half res quarters the bake cost. No RTV: compute-written.
	D3D11_TEXTURE2D_DESC openDesc = heightDesc;
	openDesc.Width = kHeightMapDim / 2;
	openDesc.Height = kHeightMapDim / 2;
	openDesc.Format = DXGI_FORMAT_R8_UNORM;
	openDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC openSrvDesc = heightSrvDesc;
	openSrvDesc.Format = openDesc.Format;
	D3D11_UNORDERED_ACCESS_VIEW_DESC openUavDesc = heightUavDesc;
	openUavDesc.Format = openDesc.Format;
	objectSkyOpen = new Texture2D(openDesc, "SnowDeformation::ObjectSkyOpen");
	objectSkyOpen->CreateSRV(openSrvDesc);
	objectSkyOpen->CreateUAV(openUavDesc);

	// Skin-depth raster: SRV+RTV only (cleared and re-rasterized fresh every
	// frame). TWO channels, same rationale as the shelter mask above:
	// R = the class layer depth this texel wears, G = the world Z of the
	// ROAD surface in this column (sentinel where no road drew).
	//
	// G holds a height rather than a road/not-road bit because the channels
	// MAX-blend INDEPENDENTLY: a bare bit went true wherever a road's
	// footprint reached, including columns whose top belongs to a taller
	// object standing on or beside the road, and the patch then draped
	// road-depth snow over that object (rocks, cairns, walls, a farmhouse
	// over its own walkway). Comparing the road's own top against the
	// column's top settles who OWNS the column, which is the question.
	// R32G32 so the height is exact -- R16F's ulp at Skyrim world Z is
	// coarser than the ownership epsilon.
	D3D11_TEXTURE2D_DESC skinDepthDesc = heightDesc;
	skinDepthDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
	skinDepthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	D3D11_SHADER_RESOURCE_VIEW_DESC skinDepthSrvDesc = heightSrvDesc;
	skinDepthSrvDesc.Format = skinDepthDesc.Format;
	D3D11_RENDER_TARGET_VIEW_DESC skinDepthRtvDesc = heightRtvDesc;
	skinDepthRtvDesc.Format = skinDepthDesc.Format;
	heightSkinDepth = new Texture2D(skinDepthDesc, "SnowDeformation::HeightSkinDepth");
	heightSkinDepth->CreateSRV(skinDepthSrvDesc);
	heightSkinDepth->CreateRTV(skinDepthRtvDesc);
}

bool SnowDeformation::EnsureStaticsRecordCB()
{
	if (staticsRecordChecked)
		return staticsRecordCB[0] && staticsRecordCB[1] && staticsContext1;
	staticsRecordChecked = true;
	static_assert(sizeof(StaticsCB) <= kStaticsRecordStride);
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	if (!device || !context)
		return false;
	D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
	if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) || !options.ConstantBufferOffsetting) {
		logger::info("[SNOW DEFORMATION] statics record buffer: no constant-buffer offsetting; per-draw updates stay");
		return false;
	}
	if (FAILED(context->QueryInterface(IID_PPV_ARGS(staticsContext1.put())))) {
		logger::info("[SNOW DEFORMATION] statics record buffer: no ID3D11DeviceContext1; per-draw updates stay");
		return false;
	}
	// An interposer's proxy may answer the query with itself; log what came back.
	logger::info("[SNOW DEFORMATION] statics record buffer: ID3D11DeviceContext1 {:X} (base context {:X}, {})",
		reinterpret_cast<uintptr_t>(staticsContext1.get()), reinterpret_cast<uintptr_t>(context),
		static_cast<void*>(staticsContext1.get()) == static_cast<void*>(context) ? "same object" : "different object");
	for (int i = 0; i < 2; i++) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kStaticsRecordMax * kStaticsRecordStride;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&desc, nullptr, staticsRecordCB[i].put()))) {
			logger::info("[SNOW DEFORMATION] statics record buffer: creation failed; per-draw updates stay");
			staticsRecordCB[0] = nullptr;
			staticsRecordCB[1] = nullptr;
			staticsContext1 = nullptr;
			return false;
		}
		Util::SetResourceName(staticsRecordCB[i].get(), i ? "SnowDeformation::StaticsRecords1" : "SnowDeformation::StaticsRecords0");
	}
	logger::info("[SNOW DEFORMATION] statics record buffer: {} records x {} bytes, two copies", kStaticsRecordMax, kStaticsRecordStride);
	return true;
}

bool SnowDeformation::UploadStaticsRecords(const StaticsCB* a_records, uint32_t a_count)
{
	if (staticsRecordDisabled || a_count == 0 || a_count > kStaticsRecordMax || !EnsureStaticsRecordCB())
		return false;
	auto* context = globals::d3d::context;
	for (int i = 0; i < 2; i++) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(staticsRecordCB[i].get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		auto* dst = static_cast<uint8_t*>(mapped.pData);
		for (uint32_t r = 0; r < a_count; r++)
			memcpy(dst + size_t(r) * kStaticsRecordStride, &a_records[r], sizeof(StaticsCB));
		context->Unmap(staticsRecordCB[i].get(), 0);
	}
	return true;
}

void SnowDeformation::BindStaticsRecord(uint32_t a_index, bool a_pixelStage, bool a_tessStages, uint32_t& a_parity)
{
	ID3D11Buffer* buffer = staticsRecordCB[a_parity & 1].get();
	a_parity++;
	const UINT first = a_index * (kStaticsRecordStride / 16);
	const UINT count = kStaticsRecordStride / 16;
	staticsContext1->VSSetConstantBuffers1(1, 1, &buffer, &first, &count);
	if (a_pixelStage)
		staticsContext1->PSSetConstantBuffers1(1, 1, &buffer, &first, &count);
	// Tessellated skins read the block in the hull and domain stages too;
	// left on the old buffer they computed every skin from one frozen
	// block (snow on vertical faces, Josef 2026-09-06).
	if (a_tessStages) {
		staticsContext1->HSSetConstantBuffers1(1, 1, &buffer, &first, &count);
		staticsContext1->DSSetConstantBuffers1(1, 1, &buffer, &first, &count);
	}
}

void SnowDeformation::RenderObjectHeightMap()
{
	LoadTraceScope _loadTrace(this, "Statics: RenderObjectHeightMap");
	auto context = globals::d3d::context;

	// Camera-following window, snapped to texel size for stability.
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float halfExtent = ObjectRasterHalfExtent();
	const float texel = halfExtent * 2.0f / kHeightMapDim;
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
	// A reach change re-texels the grid: the scrolled map would be read at
	// the wrong scale, so it clears and rebuilds from this frame's captures.
	if (objectRasterHalfExtentBuilt != halfExtent) {
		objectRasterHalfExtentBuilt = halfExtent;
		heightMapValid = false;
	}
	processData.ClearAll = heightMapValid ? 0u : 1u;
	processData.HeightWindowCenter = newCenter;
	processData.HeightHalfExtent = halfExtent;
	processData.SlopePerUnit = std::clamp(settings.SnowMoundSteepness, 0.5f, 3.0f);
	constexpr float shellCellSize = kShellVertexSpacing * kShellTexelsPerCell;
	processData.TerrainWindowOrigin = { shellWindowCellX * shellCellSize, shellWindowCellY * shellCellSize };
	processData.TerrainTexelSize = kShellVertexSpacing;
	processData.TerrainDim = kShellWindowDim;
	processData.GhostDecay = 0.5f;
	processData.RimStep = kRimStep;
	processData.OverheadIgnore = kOverheadIgnore;
	processData.DiffuseLambda = kDiffuseLambda;
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
		uint32_t gatherTrampleCount = 0;
		uint32_t gatherSealedCount = 0;
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

						// Sealed containers (draugr sarcophagi): an oriented
						// rectangle that kills coverage inside the footprint,
						// so an opened coffin is bare rather than holding a
						// sheet of snow it could not have collected. Placed
						// after the tables above because a sarcophagus matches
						// neither: they melt depth, this one suppresses.
						if (!lowered.empty()) {
							bool sealed = false;
							for (const char* substring : kSealedContainerSubstrings)
								sealed |= lowered.find(substring) != std::string::npos;
							auto* sealedBound = sealed ? base->As<RE::TESBoundObject>() : nullptr;
							if (sealedBound) {
								const auto& bounds = sealedBound->boundData;
								const float scale = a_ref->GetScale();
								const float halfX = (bounds.boundMax.x - bounds.boundMin.x) * 0.5f * scale;
								const float halfY = (bounds.boundMax.y - bounds.boundMin.y) * 0.5f * scale;
								// A degenerate bound would collapse the rectangle
								// to a line and clear nothing; skip rather than
								// substitute a guessed size.
								if (halfX >= 1.0f && halfY >= 1.0f) {
									const float angleZ = a_ref->GetAngleZ();
									const float sinZ = std::sin(angleZ), cosZ = std::cos(angleZ);
									// Bounds are model-space and need not straddle
									// the origin, so the box CENTRE rides the ref's
									// own rotation (+Y is the facing, matching the
									// workspace forward bias above).
									const float localX = (bounds.boundMax.x + bounds.boundMin.x) * 0.5f * scale;
									const float localY = (bounds.boundMax.y + bounds.boundMin.y) * 0.5f * scale;
									auto pos = a_ref->GetPosition();
									staticExclusions.push_back({ { pos.x + localX * cosZ + localY * sinZ,
																	 pos.y - localX * sinZ + localY * cosZ,
																	 pos.z, halfX },
										{ sinZ, cosZ, halfY, 3.0f } });
									gatherSealedCount++;
								}
								return RE::BSContainer::ForEachResult::kContinue;
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
				statSealedCount = gatherSealedCount;

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
	ID3D11ShaderResourceView* nullCsSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullCsUAVs[2] = { nullptr, nullptr };

	// The ghost merge runs AFTER each layer rasterizes, not before: the maps
	// are cleared to their sentinels, the capture writes this frame's truth
	// into them, and this fills what nothing drew into from the previous
	// window. Scrolling in first and MAX-blending the captures over it could
	// only ever RAISE a texel, so a reference that swapped LOD meshes kept
	// the taller silhouette of the mesh it replaced for as long as GhostDecay
	// needed to eat the difference. Rationale for the in-view rule (what
	// stops this un-sheltering a walkway floor) is on ScrollCS itself.
	{
		auto& fb = globals::game::frameBufferCached;
		HeightGhostCB ghostData{};
		ghostData.GhostViewProj = fb.GetCameraViewProj();
		ghostData.GhostCameraPosAdjust = fb.GetCameraPosAdjust();
		heightGhostCB->Update(ghostData);
	}
	ID3D11Buffer* ghostCB = heightGhostCB->CB();
	auto mergeGhost = [&](Texture2D* a_prevTop, Texture2D* a_curTop, Texture2D* a_prevBottom, Texture2D* a_curBottom) {
		if (!a_prevTop || !a_curTop || !a_prevBottom || !a_curBottom)
			return;
		ID3D11ShaderResourceView* mergeSRVs[2] = { a_prevTop->srv.get(), a_prevBottom->srv.get() };
		ID3D11UnorderedAccessView* mergeUAVs[2] = { a_curTop->uav.get(), a_curBottom->uav.get() };
		context->CSSetConstantBuffers(0, 1, &processCB);
		context->CSSetConstantBuffers(2, 1, &ghostCB);
		context->CSSetShaderResources(0, 2, mergeSRVs);
		context->CSSetUnorderedAccessViews(0, 2, mergeUAVs, nullptr);
		context->CSSetShader(heightScrollCS, nullptr, 0);
		context->Dispatch((kHeightMapDim + 7) / 8, (kHeightMapDim + 7) / 8, 1);
		context->CSSetShaderResources(0, 2, nullCsSRVs);
		context->CSSetUnorderedAccessViews(0, 2, nullCsUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullMergeCB = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullMergeCB);
		context->CSSetConstantBuffers(2, 1, &nullMergeCB);
	};

	// Every raw map starts empty, including the peeled layers: the sentinel
	// is what tells the merge which texels this frame's capture owns.
	const float topClear[4] = { kHeightMapEmptyTop, 0.0f, 0.0f, 0.0f };
	const float bottomClear[4] = { kHeightMapEmptyBottom, 0.0f, 0.0f, 0.0f };
	context->ClearRenderTargetView(heightTopRaw[heightCurrent]->rtv.get(), topClear);
	context->ClearRenderTargetView(heightBottomRaw[heightCurrent]->rtv.get(), bottomClear);
	if (heightTop2Raw[heightCurrent])
		context->ClearRenderTargetView(heightTop2Raw[heightCurrent]->rtv.get(), topClear);

	// Rasterize this frame's captures into the cleared maps.
	// G = road top, so it clears to the no-road sentinel, not to zero: zero is
	// a legal world Z and would read as a road at sea level.
	const float skinDepthClear[4] = { 0.0f, kNoRoadTop, 0.0f, 0.0f };
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
	// The peel pixel shader reads the block too (window centre, extent,
	// PeelTol). It used to find staticsCB at b1 by accident, left there by
	// the skin pass; bound here so the fallback path never depends on that,
	// and the offset path binds the pixel stage per draw below.
	context->PSSetConstantBuffers(1, 1, &cb1);
	// The capture PS rejects grounded fragments from the bottoms raster
	// (elevated undersides only); it reads terrain via the process CB.
	ID3D11Buffer* captureCB0 = heightProcessCB->CB();
	context->PSSetConstantBuffers(0, 1, &captureCB0);
	ID3D11ShaderResourceView* captureTerrainSRV = shellTerrainTexture->srv.get();
	context->PSSetShaderResources(2, 1, &captureTerrainSRV);

	// Every static's capture and peel blocks, filled once: the three loops
	// below bind them by offset (or Update from them on the fallback path).
	// The smoothed-normals view is taken here and bound from the same slot
	// in each loop, so HasSmoothedNormals and the view never disagree.
	const uint32_t captureCount = (uint32_t)capturedStatics.size();
	// Three blocks per capture: the coarse raster, the peel, and the near
	// clipmap's raster (the same draw against a quarter-width window).
	const bool fineLevel = !fineLevelDisabled && heightTopRawFine && objectSnowConeFine;
	const uint32_t recordBlocks = fineLevel ? 3u : 2u;
	std::vector<StaticsCB> captureRecords(size_t(captureCount) * recordBlocks);
	// Owning references: the cache clears itself past 1,024 entries, and a
	// raw pointer taken before that clear is a freed view by the time the
	// loops bind it (Josef's driver-thread CTD, 2026-09-06).
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> captureSmoothSRVs(captureCount);
	for (uint32_t ci = 0; ci < captureCount; ci++) {
		const auto& cap = capturedStatics[ci];
		auto* geometry = cap.geometry.get();
		if (!geometry)
			continue;
		if (cap.lodBatch)
			continue;
		auto triShape = geometry->AsTriShape();
		if (!triShape)
			continue;
		auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
			continue;
		if (triShape->GetTrishapeRuntimeData().triangleCount == 0)
			continue;
		auto desc = rendererData->vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
			continue;
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		auto layoutIt = staticsILCache.find(descKey);
		if (layoutIt == staticsILCache.end() || !layoutIt->second)
			continue;
		if (uint32_t(descKey & 0xF) == 0)
			continue;

		const auto& rot = cap.world.rotate;
		const float scale = cap.world.scale;
		const float vertexCountF = float(triShape->GetTrishapeRuntimeData().vertexCount);
		ID3D11ShaderResourceView* smoothSRV = EnsureSmoothedNormals(geometry);
		captureSmoothSRVs[ci].copy_from(smoothSRV);

		StaticsCB& scb = captureRecords[ci];
		scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, cap.world.translate.x };
		scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, cap.world.translate.y };
		scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, cap.world.translate.z };
		// Objects carry no lift into the raster and cones; roads keep theirs.
		scb.ObjectsDepth = cap.road ? settings.RoadMeshesDepth : 0.0f;
		scb.RoundedDepth = cap.road ? settings.RoadMeshesDepth : 0.0f;
		scb.VertexCountF = vertexCountF;
		scb.HeightWindowCenter = heightWindowCenter;
		scb.HeightHalfExtent = halfExtent;
		scb.LegacySkin = cap.road ? 1.0f : 0.0f;
		scb.FadeExempt = cap.fadeExempt ? 1.0f : 0.0f;
		scb.FullCoat = cap.fullCoat ? 1.0f : 0.0f;
		scb.RoadField = (settings.RoadHeightfield && cap.road && !cap.bridge) ? 1.0f : 0.0f;
		scb.ProjThreshold = cap.projThreshold;
		{
			// The S4 path no longer depends on the 3D toggle: that toggle is
			// the RISE only. The coat and its edge lumps live in the S4 draw,
			// so gating the draw on it took the border Josef keeps along with
			// the shell (his test, 2026-09-06).
			const bool s4Shell = !cap.road && cap.projThreshold > -0.5f && SD_ProjNoiseMapSRV();
			scb.ClassOverride = (s4Shell || cap.forceRounded) ? 1.0f : 0.0f;
		}
		scb.HasSmoothedNormals = smoothSRV ? 1.0f : 0.0f;

		StaticsCB& peel = captureRecords[size_t(captureCount) + ci];
		peel.WorldRow0 = scb.WorldRow0;
		peel.WorldRow1 = scb.WorldRow1;
		peel.WorldRow2 = scb.WorldRow2;
		peel.HeightWindowCenter = heightWindowCenter;
		peel.HeightHalfExtent = halfExtent;
		peel.PeelTol = kPeelTol;
		peel.VertexCountF = vertexCountF;
		peel.HasSmoothedNormals = smoothSRV ? 1.0f : 0.0f;

		if (fineLevel) {
			// Same transform, same class depths - only the window narrows.
			StaticsCB& fine = captureRecords[size_t(captureCount) * 2 + ci];
			fine = scb;
			fine.HeightHalfExtent = FineRasterHalfExtent();
		}
	}
	const bool captureRecordsLive = captureCount > 0 && UploadStaticsRecords(captureRecords.data(), captureCount * recordBlocks);
	uint32_t captureParity = 0;

	globals::profiler->BeginPass("SnowDeformation::ObjectHeightMap");
	for (uint32_t ci = 0; ci < captureCount; ci++) {
		const auto& cap = capturedStatics[ci];
		auto* geometry = cap.geometry.get();
		if (!geometry)
			continue;
		if (cap.lodBatch)
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

		ID3D11ShaderResourceView* captureSmoothSRV = captureSmoothSRVs[ci].get();
		context->VSSetShaderResources(10, 1, &captureSmoothSRV);
		if (captureRecordsLive)
			BindStaticsRecord(ci, true, false, captureParity);
		else
			staticsCB->Update(captureRecords[ci]);

		context->DrawIndexed(indexCount, 0, 0);
	}
	globals::profiler->EndPass();

	ID3D11RenderTargetView* nullRTVs[3] = { nullptr, nullptr, nullptr };
	context->OMSetRenderTargets(3, nullRTVs, nullptr);

	// Layer 1 is complete only once the ghost is under it: the peels below
	// test against it, and so does everything downstream.
	mergeGhost(heightTopRaw[previous], heightTopRaw[heightCurrent], heightBottomRaw[previous], heightBottomRaw[heightCurrent]);

	// Near clipmap level 0: the same captures against a quarter-width window,
	// so the layer-1 top lands at one world unit per texel. No ghost and no
	// bottoms - readers fall back to the coarse level outside the window, and
	// the shelter mask is the coarse level's business. RT1/RT2 stay unbound;
	// the capture PS still writes them and the writes are dropped.
	if (fineLevel) {
		context->ClearRenderTargetView(heightTopRawFine->rtv.get(), topClear);
		ID3D11RenderTargetView* fineRTVs[1] = { heightTopRawFine->rtv.get() };
		context->OMSetRenderTargets(1, fineRTVs, nullptr);
		globals::profiler->BeginPass("SnowDeformation::ObjectHeightMapFine");
		for (uint32_t ci = 0; ci < captureCount; ci++) {
			const auto& cap = capturedStatics[ci];
			auto* geometry = cap.geometry.get();
			if (!geometry)
				continue;
			if (cap.lodBatch)
				continue;
			// Everything outside the narrow window would rasterize to nothing;
			// most of the capture list is, so the reject is most of the saving.
			const auto& wb = geometry->worldBound;
			const float fineHalf = FineRasterHalfExtent();
			if (std::abs(wb.center.x - heightWindowCenter.x) > fineHalf + wb.radius ||
				std::abs(wb.center.y - heightWindowCenter.y) > fineHalf + wb.radius)
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
				continue;
			UINT stride = uint32_t(descKey & 0xF) * 4;
			if (stride == 0)
				continue;
			context->IASetInputLayout(layoutIt->second.get());
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			ID3D11ShaderResourceView* fineSmoothSRV = captureSmoothSRVs[ci].get();
			context->VSSetShaderResources(10, 1, &fineSmoothSRV);
			if (captureRecordsLive)
				BindStaticsRecord(captureCount * 2 + ci, true, false, captureParity);
			else
				staticsCB->Update(captureRecords[size_t(captureCount) * 2 + ci]);
			context->DrawIndexed(indexCount, 0, 0);
		}
		globals::profiler->EndPass();
		context->OMSetRenderTargets(3, nullRTVs, nullptr);
	}

	// The layer-2 PEEL: re-rasterize the captures against the finished
	// layer-1 top (now readable), keeping only up-facing fragments below it
	// by the peel tolerance; MAX blending yields the next-highest surface per
	// column. The layer-1 cone seed reads it for its plane-continuation test.
	// Only the transform and the window fields matter here.
	Texture2D* peelTarget = heightTop2Raw[heightCurrent];
	if (heightPeelPS && peelTarget) {
		ID3D11RenderTargetView* peelRTVs[1] = { peelTarget->rtv.get() };
		context->OMSetRenderTargets(1, peelRTVs, nullptr);
		context->PSSetShader(heightPeelPS, nullptr, 0);
		// t3 = layer 1.
		ID3D11ShaderResourceView* peelSRVs[1] = { heightTopRaw[heightCurrent]->srv.get() };
		context->PSSetShaderResources(3, 1, peelSRVs);
		// The peel PS addresses the layer maps through StaticCB's window
		// fields; the capture pass binds b1 to the VS only.
		context->PSSetConstantBuffers(1, 1, &cb1);

		globals::profiler->BeginPass("SnowDeformation::ObjectHeightPeel");
		for (uint32_t ci = 0; ci < captureCount; ci++) {
			const auto& cap = capturedStatics[ci];
			auto* geometry = cap.geometry.get();
			if (!geometry)
				continue;
			if (cap.lodBatch)
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
				continue;
			context->IASetInputLayout(layoutIt->second.get());
			UINT stride = uint32_t(descKey & 0xF) * 4;
			if (stride == 0)
				continue;
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);

			ID3D11ShaderResourceView* peelSmoothSRV = captureSmoothSRVs[ci].get();
			context->VSSetShaderResources(10, 1, &peelSmoothSRV);
			if (captureRecordsLive)
				BindStaticsRecord(captureCount + ci, true, false, captureParity);
			else
				staticsCB->Update(captureRecords[size_t(captureCount) + ci]);
			context->DrawIndexed(indexCount, 0, 0);
		}
		globals::profiler->EndPass();

		context->OMSetRenderTargets(3, nullRTVs, nullptr);
		ID3D11ShaderResourceView* nullPeelSRVs[2] = { nullptr, nullptr };
		context->PSSetShaderResources(3, 2, nullPeelSRVs);

		// Same merge for the peeled layer, before the next peel reads it.
		// The bottom slot is the scratch: ScrollCS writes a bottoms result
		// the cone chains below overwrite anyway.
		mergeGhost(heightTop2Raw[previous], peelTarget, heightBottomRaw[previous], heightScratch);
	}

	RenderVoxelVolume(captureRecords.data(), captureCount, captureRecordsLive, captureParity);

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
		// The seed floor: objects carry no depth, so their columns seed at the
		// coat's own lift; roads seed per texel from InB (see SkinLift.RimT).
		processData.ObjectSnowDepth = 0.1f;
		heightProcessCB->Update(processData);
		context->CSSetShader(objectConeSeedCS, nullptr, 0);
		// InB (t1) = the skin-depth raster: the per-texel cone seed, so roads
		// seed at their own class depth (see ObjectConeSeedCS). InC (t3) =
		// the next layer's top, for the rise rim's continuation test.
		ID3D11ShaderResourceView* seedSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(),
			heightSkinDepth ? heightSkinDepth->srv.get() : nullptr };
		ID3D11ShaderResourceView* seedNextSRV = heightTop2Raw[heightCurrent] ? heightTop2Raw[heightCurrent]->srv.get() : nullptr;
		ID3D11UnorderedAccessView* seedUAV = objectSnowCone->uav.get();
		context->CSSetShaderResources(0, 2, seedSRVs);
		context->CSSetShaderResources(3, 1, &seedNextSRV);
		context->CSSetUnorderedAccessViews(0, 1, &seedUAV, nullptr);
		context->Dispatch(dispatchDim, dispatchDim, 1);
		ID3D11ShaderResourceView* nullSeedSRVs[2] = { nullptr, nullptr };
		context->CSSetShaderResources(0, 2, nullSeedSRVs);
		context->CSSetShaderResources(3, 1, nullCsSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);

		// P4: settle a finished cone - TWO Jacobi diffusion passes. Exactly
		// two, and inside the same ping-pong: the pass count must stay even
		// so the result lands back in the cone's own texture (the kConeSteps
		// parity invariant).
		auto settleCone = [&](Texture2D*& a_in, Texture2D*& a_out) {
			if (!objectConeDiffuseCS)
				return;
			context->CSSetShader(objectConeDiffuseCS, nullptr, 0);
			for (int settleI = 0; settleI < 2; settleI++) {
				ID3D11ShaderResourceView* settleSRV = a_in->srv.get();
				ID3D11UnorderedAccessView* settleUAV = a_out->uav.get();
				context->CSSetShaderResources(0, 1, &settleSRV);
				context->CSSetUnorderedAccessViews(0, 1, &settleUAV, nullptr);
				context->Dispatch(dispatchDim, dispatchDim, 1);
				context->CSSetShaderResources(0, 1, nullCsSRVs);
				context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
				std::swap(a_in, a_out);
			}
		};

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
		settleCone(objIn, objOut);

		// The same seed + repose chain over the near clipmap's top, at one
		// unit per texel. Two inputs differ from the coarse chain: InB (the
		// per-texel skin-depth raster) is unbound, so the seed is the class
		// constant - roads seed per texel on the coarse level only, and the
		// patch reads that one; and InC (the next peeled layer, for the rise
		// rim's "does our plane continue under the cover" test) is the fine
		// top itself, which makes every upward break a plane boundary. That
		// is what a detail level wants: a rock's own steps rim.
		if (fineLevel) {
			HeightProcessCB fineData = processData;
			fineData.HeightHalfExtent = FineRasterHalfExtent();
			fineData.ConeStep = 1;
			heightProcessCB->Update(fineData);
			context->CSSetShader(objectConeSeedCS, nullptr, 0);
			ID3D11ShaderResourceView* fineSeedSRVs[2] = { heightTopRawFine->srv.get(), nullptr };
			ID3D11ShaderResourceView* fineNextSRV = heightTopRawFine->srv.get();
			ID3D11UnorderedAccessView* fineSeedUAV = objectSnowConeFine->uav.get();
			context->CSSetShaderResources(0, 2, fineSeedSRVs);
			context->CSSetShaderResources(3, 1, &fineNextSRV);
			context->CSSetUnorderedAccessViews(0, 1, &fineSeedUAV, nullptr);
			context->Dispatch(dispatchDim, dispatchDim, 1);
			ID3D11ShaderResourceView* nullFineSeedSRVs[2] = { nullptr, nullptr };
			context->CSSetShaderResources(0, 2, nullFineSeedSRVs);
			context->CSSetShaderResources(3, 1, nullCsSRVs);
			context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);

			context->CSSetShader(objectConeCS, nullptr, 0);
			Texture2D* fineIn = objectSnowConeFine;
			Texture2D* fineOut = heightScratch;
			for (uint step : kConeSteps) {
				fineData.ConeStep = step;
				heightProcessCB->Update(fineData);
				ID3D11ShaderResourceView* fineSRV = fineIn->srv.get();
				ID3D11UnorderedAccessView* fineUAV = fineOut->uav.get();
				context->CSSetShaderResources(0, 1, &fineSRV);
				context->CSSetUnorderedAccessViews(0, 1, &fineUAV, nullptr);
				context->Dispatch(dispatchDim, dispatchDim, 1);
				context->CSSetShaderResources(0, 1, nullCsSRVs);
				context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
				std::swap(fineIn, fineOut);
			}
			// The chain must land back in objectSnowConeFine; kConeSteps'
			// even length is what guarantees it, and the coarse chain above
			// depends on the same invariant.
			settleCone(fineIn, fineOut);
			// The coarse chain's constants are what every later pass expects.
			heightProcessCB->Update(processData);
		}

		// P3: bake the sky-openness field from the layer-1 tops, after the
		// cone chains so the raster is final for this frame. Half-res
		// output; the consumers bilinear it.
		if (objectSkyOpenCS && objectSkyOpen) {
			context->CSSetShader(objectSkyOpenCS, nullptr, 0);
			ID3D11ShaderResourceView* openSRV = heightTopRaw[heightCurrent]->srv.get();
			ID3D11UnorderedAccessView* openUAV = objectSkyOpen->uav.get();
			context->CSSetShaderResources(0, 1, &openSRV);
			context->CSSetUnorderedAccessViews(0, 1, &openUAV, nullptr);
			const uint32_t openDim = (kHeightMapDim / 2 + 7) / 8;
			context->Dispatch(openDim, openDim, 1);
			context->CSSetShaderResources(0, 1, nullCsSRVs);
			context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
		}
	}

	ID3D11ShaderResourceView* nullTailSRVs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(2, 2, nullTailSRVs);
	ID3D11Buffer* nullProcessCB = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullProcessCB);
	context->CSSetShader(nullptr, nullptr, 0);

	// Height-field probe (Debugging Options): the seven object maps read
	// back at the player's texel. Copy this frame, map LAST frame's copy
	// with DO_NOT_WAIT, so the readout is one frame old and never stalls.
	if (auto probePlayer = RE::PlayerCharacter::GetSingleton()) {
		if (!probeStaging[0]) {
			D3D11_TEXTURE2D_DESC sdesc{
				.Width = 8,
				.Height = 1,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_FLOAT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_STAGING,
				.BindFlags = 0,
				.CPUAccessFlags = D3D11_CPU_ACCESS_READ,
				.MiscFlags = 0
			};
			globals::d3d::device->CreateTexture2D(&sdesc, nullptr, probeStaging[0].put());
			globals::d3d::device->CreateTexture2D(&sdesc, nullptr, probeStaging[1].put());
			if (probeStaging[0])
				Util::SetResourceName(probeStaging[0].get(), "SnowDeformation::ProbeStaging0");
			if (probeStaging[1])
				Util::SetResourceName(probeStaging[1].get(), "SnowDeformation::ProbeStaging1");
		}
		if (probeStaging[0] && probeStaging[1]) {
			const auto pos = probePlayer->GetPosition();
			probeWorldPos = { pos.x, pos.y, pos.z };
			// Same world->texel mapping as PatchTexel / the capture VS.
			float u = (pos.x - heightWindowCenter.x) / ObjectRasterHalfExtent() * 0.5f + 0.5f;
			float v = 0.5f - (pos.y - heightWindowCenter.y) / ObjectRasterHalfExtent() * 0.5f;
			uint tx = uint(std::clamp(int(u * kHeightMapDim), 0, int(kHeightMapDim) - 1));
			uint ty = uint(std::clamp(int(v * kHeightMapDim), 0, int(kHeightMapDim) - 1));
			Texture2D* probeMaps[3] = { heightTopRaw[heightCurrent], heightTop2Raw[heightCurrent], objectSnowCone };
			D3D11_BOX probeBox{ tx, ty, 0, tx + 1, ty + 1, 1 };
			for (uint i = 0; i < 3; i++)
				if (probeMaps[i] && probeMaps[i]->resource)
					context->CopySubresourceRegion(probeStaging[probeCursor].get(), 0, i, 0, 0, probeMaps[i]->resource.get(), 0, &probeBox);
			probeCursor ^= 1;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(probeStaging[probeCursor].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
				memcpy(probeVals, mapped.pData, sizeof(probeVals));
				context->Unmap(probeStaging[probeCursor].get(), 0);
				probeValid = true;
			}
		}
	}
}

void SnowDeformation::FillSkinDrawCB(const CapturedSnowStatic& a_cap, bool a_s4Shell, float a_vertexCount, bool a_hasSmoothedNormals, bool a_hasObjectTop, bool a_hasSkinNormalCopy, StaticsCB& a_scb) const
{
	const auto& rot = a_cap.world.rotate;
	const float scale = a_cap.world.scale;
	a_scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, a_cap.world.translate.x };
	a_scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, a_cap.world.translate.y };
	a_scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, a_cap.world.translate.z };
	// Objects carry no rise: the S4 draw runs at depth 0, the coat and its
	// lumps alone. Roads are the patch's and keep theirs.
	a_scb.ObjectsDepth = a_cap.road ? settings.RoadMeshesDepth : 0.0f;
	a_scb.RoundedDepth = a_cap.road ? settings.RoadMeshesDepth : 0.0f;
	a_scb.VertexCountF = a_vertexCount;
	a_scb.HeightWindowCenter = heightWindowCenter;
	a_scb.HeightHalfExtent = ObjectRasterHalfExtent();
	a_scb.HasSmoothedNormals = a_hasSmoothedNormals ? 1.0f : 0.0f;
	a_scb.HasObjectTop = a_hasObjectTop ? 1.0f : 0.0f;
	a_scb.LegacySkin = a_cap.road ? 1.0f : 0.0f;
	a_scb.FadeExempt = a_cap.fadeExempt ? 1.0f : 0.0f;
	a_scb.FullCoat = a_cap.fullCoat ? 1.0f : 0.0f;
	a_scb.MoundSteepness = std::clamp(settings.SnowMoundSteepness, 0.5f, 3.0f);
	a_scb.RoadField = (settings.RoadHeightfield && a_cap.road && !a_cap.bridge) ? 1.0f : 0.0f;
	a_scb.ProjThreshold = a_cap.projThreshold;
	a_scb.ClassOverride = (a_s4Shell || a_cap.forceRounded) ? 1.0f : 0.0f;
	a_scb.ProjNoiseScale = a_cap.projNoiseScale;
	a_scb.ProjNoiseTiling = a_cap.projNoiseTiling;
	// 2 = the S4 shell owns this draw; 0 = classic path.
	a_scb.ProjPixelEnable = a_s4Shell ? 2.0f : 0.0f;
	a_scb.PeelTol = kPeelTol;
	a_scb.HasSkinMasksCopy = landMasksCopySRV ? 1.0f : 0.0f;
	a_scb.EdgeFlankWidth = std::clamp(settings.SkinEdgeFlankWidth, 0.0f, 1.0f);
	// Same veto as the Lighting-side recolor (sand and moss keep their
	// look), and only where the property really carries projection data:
	// the mesh-replacer default reconstructs a weight the game never paints.
	// LOD batches read the brightness recolor's written weight instead.
	a_scb.EdgeCoat = (settings.ProjSnowMatch && (a_cap.projReal || a_cap.lodBatch) && a_cap.geometry &&
	                  ClassifyProjectedMato(a_cap.geometry.get()) != MatoClass::kNotSnow) ? 1.0f : 0.0f;
	a_scb.HasSkinNormalCopy = a_hasSkinNormalCopy ? 1.0f : 0.0f;
	// The near clipmap shares the coarse window's centre, so its half-extent
	// is all the shaders need; 0 turns every fine read back into a coarse one.
	a_scb.FineHalfExtent = (!fineLevelDisabled && heightTopRawFine && objectSnowConeFine) ? FineRasterHalfExtent() : 0.0f;
	a_scb.LODBatch = a_cap.lodBatch ? 1.0f : 0.0f;
}

bool SnowDeformation::EnsureSmoothNormalsCS()
{
	if (!smoothAccumulateCS)
		smoothAccumulateCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "ACCUMULATE", "" } }, "cs_5_0"));
	if (!smoothResolveCS)
		smoothResolveCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "RESOLVE", "" } }, "cs_5_0"));
	if (!smoothFlatStatsCS)
		smoothFlatStatsCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "FLATSTATS", "" } }, "cs_5_0"));
	if (!smoothBoundsCS)
		smoothBoundsCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "MESHBOUNDS", "" } }, "cs_5_0"));
	if (!smoothClusterCS)
		smoothClusterCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "CLUSTERBOUNDS", "" } }, "cs_5_0"));
	if (!meshBounds && smoothBoundsCS) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kMeshBoundsSlots * 2 * 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = 16;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = kMeshBoundsSlots * 2;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kMeshBoundsSlots * 2;
		meshBounds = new Buffer(desc, nullptr, "SnowDeformation::MeshBounds");
		meshBounds->CreateSRV(srvDesc);
		meshBounds->CreateUAV(uavDesc);
	}
	return smoothAccumulateCS && smoothResolveCS && smoothFlatStatsCS;
}

uint32_t SnowDeformation::SmoothedBoundsSlot(void* a_vertexBuffer) const
{
	auto it = smoothedNormalsCache.find(a_vertexBuffer);
	return (it != smoothedNormalsCache.end() && it->second.ready) ? it->second.boundsSlot : UINT32_MAX;
}

ID3D11ComputeShader* SnowDeformation::GetClusterCullCS()
{
	if (!clusterCullCS) {
		logger::debug("Compiling DepthSyncCS ClusterCullCS");
		clusterCullCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", { { "SNOW_CLUSTER_CULL", "" } }, "cs_5_0", "ClusterCullCS"));
	}
	return clusterCullCS;
}

bool SnowDeformation::EnsureClusterResources(uint32_t a_scratchIndices)
{
	if (!clusterBounds) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kClusterSlots * 2 * 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = 16;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = kClusterSlots * 2;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = kClusterSlots * 2;
		clusterBounds = new Buffer(desc, nullptr, "SnowDeformation::ClusterBounds");
		clusterBounds->CreateSRV(srvDesc);
		clusterBounds->CreateUAV(uavDesc);
	}
	if (!clusterIndexPool) {
		// Every unique mesh's index data in one buffer: a single dispatch
		// cannot bind one index buffer per skin, so the cull reads them all
		// from here at the mesh's own offset. 16-bit, as the game stores them.
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = kClusterIndexPoolIndices * 2;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		srvDesc.BufferEx.NumElements = kClusterIndexPoolIndices / 2;
		srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		clusterIndexPool = new Buffer(desc, nullptr, "SnowDeformation::ClusterIndexPool");
		clusterIndexPool->CreateSRV(srvDesc);
	}
	const uint32_t wantScratch = std::min(std::max(a_scratchIndices, 1u << 20), kClusterScratchMaxIndices);
	if (clusterScratchIB && clusterScratchCapacity < wantScratch) {
		delete clusterScratchIB;
		clusterScratchIB = nullptr;
	}
	if (!clusterScratchIB) {
		// 32-bit purely for alignment: a cluster's compacted destination is a
		// running sum of surviving index counts, which 16-bit stores could
		// land on an odd halfword.
		clusterScratchCapacity = wantScratch;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = clusterScratchCapacity * 4;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = clusterScratchCapacity;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		clusterScratchIB = new Buffer(desc, nullptr, "SnowDeformation::ClusterScratchIB");
		clusterScratchIB->CreateUAV(uavDesc);
	}
	return clusterBounds->srv && clusterBounds->uav && clusterIndexPool->srv && clusterScratchIB->uav;
}

ID3D11ShaderResourceView* SnowDeformation::EnsureSmoothedNormals(RE::BSGeometry* a_geometry)
{
	LoadTraceScope _loadTrace(this, "Statics: EnsureSmoothedNormals");
	auto triShape = a_geometry->AsTriShape();
	if (!triShape)
		return nullptr;
	auto rendererData = a_geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData || !rendererData->vertexBuffer)
		return nullptr;

	// Pointer reuse after cell unloads could serve stale normals to a new
	// mesh; the cap flushes the cache before that becomes likely.
	if (smoothedNormalsCache.size() > 1024) {
		smoothedNormalsCache.clear();
		meshBoundsNext = 0;
		clusterNext = 0;
		clusterIndexPoolNext = 0;
	}

	auto [it, inserted] = smoothedNormalsCache.try_emplace(rendererData->vertexBuffer);
	auto& entry = it->second;
	if (!inserted)
		return entry.ready ? entry.srv.get() : nullptr;

	// First sight of this geometry: build now (a buffer copy + two small
	// dispatches, once per unique mesh). Failures leave the permanent null
	// entry; no per-frame retries; the VS falls back to raw normals.
	if (!EnsureSmoothNormalsCS() || !smoothCB)
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
	// Authored vertex color for the projected-mask reconstruction; alpha
	// rides OutNormals.w (see SmoothNormalsCS RESOLVE).
	uint32_t colorOffset = 0;
	bool hasColor = false;
	if (desc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS)) {
		colorOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
		hasColor = colorOffset > 0 && colorOffset + 4 <= stride;
	}

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	auto* gameVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* gameIB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);

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
	cb.ColorOffsetBytes = colorOffset;
	cb.HasColor = hasColor ? 1u : 0u;
	const bool wantBounds = smoothBoundsCS && meshBounds && meshBounds->uav && meshBoundsNext < kMeshBoundsSlots;
	cb.BoundsSlot = wantBounds ? meshBoundsNext : 0u;

	// Clusters: at most one thread group's worth per mesh, so the cull pass is
	// a single chunk (a barrier under a buffer-driven trip count is
	// non-uniform flow control and will not compile). 64 triangles each until
	// that would exceed the cap, proportionally larger after.
	const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
	const uint32_t poolOffset = (clusterIndexPoolNext + 7u) & ~7u;
	uint32_t clusterTris = kClusterTrisBase;
	uint32_t clusterCount = (indexCount / 3 + clusterTris - 1) / std::max(clusterTris, 1u);
	if (clusterCount > kClusterGroup) {
		clusterTris = (indexCount / 3 + kClusterGroup - 1) / kClusterGroup;
		clusterCount = (indexCount / 3 + clusterTris - 1) / std::max(clusterTris, 1u);
	}
	const bool wantClusters = smoothClusterCS && gameIB && indexCount >= 3 &&
	                          clusterNext + clusterCount <= kClusterSlots &&
	                          poolOffset + indexCount <= kClusterIndexPoolIndices &&
	                          EnsureClusterResources(clusterScratchNeeded);
	cb.IndexPoolOffset = wantClusters ? poolOffset : 0u;
	cb.IndexCount = wantClusters ? indexCount : 0u;
	cb.ClusterOffset = wantClusters ? clusterNext : 0u;
	cb.ClusterStrideIndices = clusterTris * 3;
	smoothCB->Update(cb);

	if (wantClusters) {
		// The game's index data, copied once into the shared pool.
		D3D11_BOX box{};
		box.left = 0;
		box.right = indexCount * 2;
		box.bottom = 1;
		box.back = 1;
		context->CopySubresourceRegion(clusterIndexPool->resource.get(), 0, poolOffset * 2, 0, 0, gameIB, 0, &box);
	}

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
	if (wantBounds) {
		ID3D11UnorderedAccessView* boundsUAV = meshBounds->uav.get();
		context->CSSetUnorderedAccessViews(2, 1, &boundsUAV, nullptr);
		context->CSSetShader(smoothBoundsCS, nullptr, 0);
		context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* nullBoundsUAV = nullptr;
		context->CSSetUnorderedAccessViews(2, 1, &nullBoundsUAV, nullptr);
		entry.boundsSlot = meshBoundsNext++;
	}
	if (wantClusters) {
		ID3D11ShaderResourceView* poolSRV = clusterIndexPool->srv.get();
		context->CSSetShaderResources(1, 1, &poolSRV);
		ID3D11UnorderedAccessView* clusterUAV = clusterBounds->uav.get();
		context->CSSetUnorderedAccessViews(3, 1, &clusterUAV, nullptr);
		context->CSSetShader(smoothClusterCS, nullptr, 0);
		context->Dispatch((clusterCount + 63) / 64, 1, 1);
		ID3D11ShaderResourceView* nullPoolSRV = nullptr;
		context->CSSetShaderResources(1, 1, &nullPoolSRV);
		ID3D11UnorderedAccessView* nullClusterUAV = nullptr;
		context->CSSetUnorderedAccessViews(3, 1, &nullClusterUAV, nullptr);
		entry.clusterOffset = clusterNext;
		entry.clusterCount = clusterCount;
		entry.indexPoolOffset = poolOffset;
		entry.indexCount = indexCount;
		clusterNext += clusterCount;
		clusterIndexPoolNext = poolOffset + indexCount;
	}

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
	if (SnowShadersPending(2))
		return;
	ServiceLandTriProbe();
	{
		// Order-free: the game hands the passes over in a different order
		// each frame, so entries hash on their own and the sum is the list.
		uint64_t h = 0;
		uint64_t e = 0;
		auto mix = [&](const void* a_p, size_t a_n) {
			const auto* b = static_cast<const uint8_t*>(a_p);
			for (size_t i = 0; i < a_n; i++) {
				e ^= b[i];
				e *= 1099511628211ull;
			}
		};
		for (const auto& cap : capturedStatics) {
			e = 1469598103934665603ull;
			const void* g = cap.geometry.get();
			mix(&g, sizeof(g));
			mix(&cap.world.translate, sizeof(cap.world.translate));
			mix(&cap.road, sizeof(cap.road));
			mix(&cap.bridge, sizeof(cap.bridge));
			mix(&cap.fadeExempt, sizeof(cap.fadeExempt));
			mix(&cap.projThreshold, sizeof(cap.projThreshold));
			mix(&cap.projNoiseScale, sizeof(cap.projNoiseScale));
			mix(&cap.projNoiseTiling, sizeof(cap.projNoiseTiling));
			mix(&cap.forceRounded, sizeof(cap.forceRounded));
			mix(&cap.plankFamily, sizeof(cap.plankFamily));
			mix(&cap.projReal, sizeof(cap.projReal));
			mix(&cap.fullCoat, sizeof(cap.fullCoat));
			h += e;
		}
		cpuCensus.captureHash = h;
		cpuCensus.captureCount = (uint32_t)capturedStatics.size();
	}
	LoadTraceScope _loadTrace(this, "Statics: DrawCapturedStatics");
	// The cover always draws (minimum coat); sliders never disable it.
	if (capturedStatics.empty())
		return;
	// Not on the world map: skins strobed there as the panning camera
	// crossed capture gates, and a skin without the shell beside it only
	// mismatches the map. Mirrors the recolor gate in GetCommonSettingsGPU.
	if (globals::state->isMapMenuOpen)
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

	// Per-draw PS choice below; bound here so a fallback path still has one.
	context->PSSetShader(staticsPS, nullptr, 0);
	ID3D11PixelShader* boundStaticsPS = staticsPS;
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
	// A lambda because the depth prepass's fullscreen fills replace these
	// stages mid-pass and have to put them back. The drape (S4 draws) takes
	// the plain VS: a flat coat has nothing for tessellation to shape, and
	// the rim term tessellates it to the cap everywhere (its cone is under
	// the 4-unit "rim is near" threshold on every texel).
	bool skinStagesTess = false;
	auto bindSkinStages = [&](bool a_tess) {
		skinStagesTess = a_tess;
		if (a_tess) {
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
			// P1: EdgeTessFactor's rim term reads the cone field, so the HS
			// needs t13 like the VS/DS do (bound below for those stages).
			ID3D11ShaderResourceView* hsConeSRV = (objectSnowCone && objectSnowCone->srv) ? objectSnowCone->srv.get() : nullptr;
			context->HSSetShaderResources(13, 1, &hsConeSRV);
			ID3D11ShaderResourceView* dsDeformSRV = GetDeformationSRV();
			context->DSSetShaderResources(1, 1, &dsDeformSRV);
			ID3D11ShaderResourceView* dsHeightSRV = shellSnowHeightSRV.get();
			context->DSSetShaderResources(8, 1, &dsHeightSRV);
			ID3D11SamplerState* dsSampler = shellSnowSampler.get();
			context->DSSetSamplers(0, 1, &dsSampler);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
		} else {
			context->VSSetShader(staticsVS, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}
	};
	bindSkinStages(tessellateSkins);

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
	// Scene depth copy (PS t3): the SSS hug gate and the shell-surface
	// re-march, both ported from the terrain shell, read it. Same helper and
	// same slot the shell binds - a COPY (Terrain Blending's blended depth
	// when that feature owns it, else kPOST_ZPREPASS_COPY), never the bound
	// DSV, so sampling it while this pass writes depth is legal.
	ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->PSSetShaderResources(3, 1, &sceneDepthSRV);
	// Skin-depth raster (PS t12): the road skin runs the patch's own
	// RoadOwnsColumn test before stepping aside, so it never discards into a
	// column the patch declined.
	ID3D11ShaderResourceView* skinDepthPSSRV = (heightSkinDepth && heightSkinDepth->srv) ?
	                                               heightSkinDepth->srv.get() :
	                                               nullptr;
	context->PSSetShaderResources(12, 1, &skinDepthPSSRV);
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
	// Near clipmap (t33 cone, t34 top): the same two maps at one unit per
	// texel over the inner window. Every reader that takes them falls back to
	// t13/t11 outside it, so a null bind here is simply the coarse behaviour.
	ID3D11ShaderResourceView* fineSRVs[2] = {
		(!fineLevelDisabled && objectSnowConeFine && objectSnowConeFine->srv) ? objectSnowConeFine->srv.get() : nullptr,
		(!fineLevelDisabled && heightTopRawFine && heightTopRawFine->srv) ? heightTopRawFine->srv.get() : nullptr
	};
	context->VSSetShaderResources(33, 2, fineSRVs);
	context->DSSetShaderResources(33, 2, fineSRVs);
	context->PSSetShaderResources(33, 2, fineSRVs);
	context->HSSetShaderResources(33, 2, fineSRVs);
	// P3: the sky-openness field (t25), the lift's depth weighting.
	ID3D11ShaderResourceView* skyOpenSRV = (objectSkyOpen && objectSkyOpen->srv) ? objectSkyOpen->srv.get() : nullptr;
	context->VSSetShaderResources(25, 1, &skyOpenSRV);
	context->DSSetShaderResources(25, 1, &skyOpenSRV);
	// Wide exclusion field (t15) + frost crystal patterns (t16/t17): the
	// skin's self-shadow march and spell-mark shading read the landscape
	// shell's slots; the skins draw standalone, so bind explicitly here.
	ID3D11ShaderResourceView* skinExclusionSRV = GetExclusionFieldSRV();
	context->PSSetShaderResources(15, 1, &skinExclusionSRV);
	EnsureFrostPatternTextures();
	ID3D11ShaderResourceView* skinFrostSRVs[2] = { frostPatternNormalSRV.get(), frostPatternDiffuseSRV.get() };
	context->PSSetShaderResources(16, 2, skinFrostSRVs);
	// The S4 shell's per-pixel footprint cut: vanilla's noise map (PS t21)
	// and the pre-shell normals copy (PS t23, per-pixel nz with the
	// interpolated fallback).
	// Bound whenever they exist: the coat's contour is built from these, and
	// the coat draws with the 3D rise off.
	ID3D11ShaderResourceView* projNoiseSRV = SD_ProjNoiseMapSRV();
	context->PSSetShaderResources(21, 1, &projNoiseSRV);
	ID3D11ShaderResourceView* skinNormalsSRV = preSkinNormalsCopySRV.get();
	context->PSSetShaderResources(23, 1, &skinNormalsSRV);
	// The pre-shell Masks copy: the recolor's real projected weight.
	ID3D11ShaderResourceView* skinMasksSRV = landMasksCopySRV.get();
	context->PSSetShaderResources(32, 1, &skinMasksSRV);

	// Depth prepass for the skins. Unlike the shell's, the private test
	// depth is written BY the prepass draws themselves rather than through a
	// colour target: the skins draw with a rasterizer depth bias, and whether
	// that bias reaches the depth a pixel shader sees is a spec detail the
	// EQUAL test must not depend on. Starting the private buffer as a copy
	// of the scene depth and running the identical raster path twice makes
	// the tested value the written value by construction. Carving draws
	// (roads at default settings) take no part in the prepass: their depth is
	// shader-computed, so they draw once in the shading loop under their own
	// LESS_EQUAL + export as they always did, into the same private buffer.
	// The write-back then hands every skin's depth to the main buffer exactly
	// as the single loop would have left it; nothing else writes depth in
	// between.
	auto* fillVS = GetShellFillVS();
	auto* fillPS = fillVS ? GetShellFillPS() : nullptr;
	auto mainDepthSRV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	const bool prepass = !staticsDepthPrepassDisabled && lodDebugView != 1 && staticsPSPrepassNoDepth &&
	                     fillPS && mainDepthSRV && EnsurePrepassResources(mainDepthSRV);
	ID3D11DepthStencilState* boundDepthState = nullptr;

	// Render-target and viewport snapshot. The cull's compute reads the scene
	// depth, so the targets come off first; the prepass fills need a
	// full-range viewport; both restore from here.
	ID3D11RenderTargetView* rawRTVs[8] = {};
	ID3D11DepthStencilView* rawDSV = nullptr;
	context->OMGetRenderTargets(8, rawRTVs, &rawDSV);
	winrt::com_ptr<ID3D11RenderTargetView> rtvs[8];
	ID3D11RenderTargetView* rtvPtrs[8] = {};
	for (uint32_t i = 0; i < 8; i++) {
		rtvs[i].attach(rawRTVs[i]);
		rtvPtrs[i] = rtvs[i].get();
	}
	winrt::com_ptr<ID3D11DepthStencilView> dsv;
	dsv.attach(rawDSV);
	D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&vpCount, vps);
	D3D11_VIEWPORT fillVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	for (UINT i = 0; i < vpCount; i++) {
		fillVps[i] = vps[i];
		fillVps[i].MinDepth = 0.0f;
		fillVps[i].MaxDepth = 1.0f;
	}
	// The game's decal depth range (0.9999720 against the main pass's
	// 0.9999980, RenderDoc 2026-09-10): skins of decal-mode draws rasterise
	// through it so their depth lands where the mesh's own did.
	D3D11_VIEWPORT decalVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
	for (UINT i = 0; i < vpCount; i++) {
		decalVps[i] = vps[i];
		decalVps[i].MaxDepth = kDecalViewportMaxDepth;
	}

	// One filtered list of skin draws, walked by the cull and by both loops.
	struct SkinDraw
	{
		const CapturedSnowStatic* cap;
		RE::BSGeometry* geometry;
		ID3D11Buffer* vb;
		ID3D11Buffer* ib;
		ID3D11InputLayout* layout;
		UINT stride;
		uint32_t indexCount;
		float vertexCount;
		bool s4Shell;
		uint32_t slot;
		uint32_t boundsSlot;
		uint32_t clusterOffset;
		uint32_t clusterCount;
		uint32_t indexPoolOffset;
		uint32_t scratchBase;
		bool decalDepth;
	};
	std::vector<SkinDraw> skinDraws;
	skinDraws.reserve(capturedStatics.size());
	// Grown a frame late from what the last frame asked for: the allocation
	// below has to hand out ranges before the total is known.
	EnsureClusterResources(clusterScratchNeeded);
	uint32_t scratchNext = 0;
	uint32_t scratchWanted = 0;
	for (const auto& cap : capturedStatics) {
		auto* geometry = cap.geometry.get();
		if (!geometry)
			continue;
		// THE OLD OBJECT SHELL IS RETIRED (Josef, 2026-08-29 - at fill 0
		// its no-PD pillows stood alone on the steps, and at fill 100 the
		// S4 shell stacked on top of them). Only two things draw now:
		// roads (their own tuned machinery, always) and the S4 shell
		// (PD-carrying draws, gated by "3D Snow on Objects"). Draws whose
		// PROPERTY carries no projection data get no skin at all - the
		// Lighting recolor still covers the technique-classified ones
		// (fence family) flat. The classic shader path survives only
		// because roads run through it.
		const bool s4Shell = !cap.road && cap.projThreshold > -0.5f && projNoiseSRV;
		if (!cap.road && !s4Shell)
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
		auto* layout = StaticsInputLayoutFor(descKey, desc);
		if (!layout)
			continue;

		// "Recolor Projected Snow" is the DRAPE: the skin finds where the
		// game's projected diffuse is and lays the shell's snow set over it,
		// Edge Lump Reach shaping how far it spreads. That is geometry - the
		// Lighting-pass recolor can change the projected snow's COLOUR but
		// cannot give it the shell's material or an edge - so the draw has
		// to happen for it. Off, nothing of ours is drawn on an object. Roads
		// belong to Road Meshes Depth and keep their skin either way.
		//
		// The layout above is created BEFORE this gate on purpose: the
		// object height raster draws through that same cache, and the
		// landscape shell's lift, the shelter mask and the trench patch all
		// read the raster. Skipping the layout would silently drop objects
		// out of the height field the moment the shell was switched off.
		if (!cap.road && !settings.ProjSnowMatch)
			continue;

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
		// Smoothed normals (and the mesh box beside them) are built on first
		// sight; the draw loop's own call then hits the cache.
		EnsureSmoothedNormals(geometry);
		auto cacheIt = smoothedNormalsCache.find(rendererData->vertexBuffer);
		const bool cached = cacheIt != smoothedNormalsCache.end() && cacheIt->second.ready;
		uint32_t cOffset = 0, cCount = 0, cPool = 0, cBase = 0;
		// Clusters need a contiguous slice of the scratch buffer, handed out
		// here so the pass writes to a known place; a mesh that does not fit
		// simply draws its own index buffer entire.
		if (cached && cacheIt->second.clusterCount != 0 && !clusterCullDisabled)
			scratchWanted += cacheIt->second.indexCount;
		if (cached && cacheIt->second.clusterCount != 0 && !clusterCullDisabled &&
			scratchNext + cacheIt->second.indexCount <= clusterScratchCapacity) {
			cOffset = cacheIt->second.clusterOffset;
			cCount = cacheIt->second.clusterCount;
			cPool = cacheIt->second.indexPoolOffset;
			cBase = scratchNext;
			scratchNext += cacheIt->second.indexCount;
		}
		skinDraws.push_back({ &cap, geometry,
			reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer),
			reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer),
			layout, stride, indexCount,
			float(triShape->GetTrishapeRuntimeData().vertexCount), s4Shell,
			uint32_t(skinDraws.size()), cached ? cacheIt->second.boundsSlot : UINT32_MAX,
			cOffset, cCount, cPool, cBase, cap.decalDepth });
	}

	// Whole-skin occlusion cull: bounding spheres against a max-depth
	// pyramid of the scene, on the GPU, same frame. Writes each skin's
	// indirect draw arguments; the loops below draw through them.
	auto* hiZBuild = skinCullDisabled ? nullptr : GetHiZBuildCS();
	auto* skinCull = hiZBuild ? GetSkinCullCS() : nullptr;
	const bool cullActive = skinCull && !skinDraws.empty() && mainDepthSRV && vpCount > 0 &&
	                        EnsureSkinCullResources(uint32_t(skinDraws.size()), mainDepthSRV);
	if (cullActive) {
		const float liftMargin = settings.RoadMeshesDepth + kSkinCullMargin;
		uint32_t clusterSkins = 0, trisTotal = 0;
		for (const auto& d : skinDraws) {
			trisTotal += d.indexCount / 3;
			if (d.clusterCount != 0)
				clusterSkins++;
		}
		skinCullTrisTotalLast = trisTotal;
		clusterSkinsLast = clusterSkins;
		clusterScratchUsedLast = scratchNext;
		clusterScratchNeeded = scratchWanted;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(skinCullBounds->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			auto* out = static_cast<SkinCullBound*>(mapped.pData);
			for (const auto& d : skinDraws) {
				const auto& wb = d.geometry->worldBound;
				const auto& rot = d.cap->world.rotate;
				const float scale = d.cap->world.scale;
				const bool hasBounds = d.boundsSlot != UINT32_MAX && meshBounds && meshBounds->srv;
				out[d.slot] = { { wb.center.x, wb.center.y, wb.center.z }, wb.radius + liftMargin, d.indexCount,
					hasBounds ? d.boundsSlot : 0u, hasBounds ? 1u : 0u, liftMargin,
					d.clusterOffset, d.clusterCount, d.indexPoolOffset, d.scratchBase,
					{ rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, d.cap->world.translate.x },
					{ rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, d.cap->world.translate.y },
					{ rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, d.cap->world.translate.z } };
			}
			context->Unmap(skinCullBounds->resource.get(), 0);
		}
		SkinCullCB ccb{};
		const auto& fb = globals::game::frameBufferCached;
		ccb.ViewProj = fb.GetCameraViewProj();
		ccb.CameraPosAdjust = fb.GetCameraPosAdjust();
		ccb.Viewport = { vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height };
		ccb.Depth = { vps[0].MinDepth, vps[0].MaxDepth, kSkinCullDepthEps, float(skinCullLevels) };
		ccb.SkinCount = uint32_t(skinDraws.size());

		context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11Buffer* cullCB = skinCullCB->CB();
		context->CSSetConstantBuffers(0, 1, &cullCB);
		context->CSSetShader(hiZBuild, nullptr, 0);
		ID3D11ShaderResourceView* nullCullSRVs[4] = {};
		ID3D11UnorderedAccessView* nullCullUAVs[2] = {};
		for (uint32_t level = 0; level < skinCullLevels; level++) {
			ccb.Level = float(level);
			skinCullCB->Update(ccb);
			// Read level-1 of the chain, write the level's scratch, copy it in:
			// the chain is never bound as input and output of one dispatch.
			ID3D11ShaderResourceView* src = level == 0 ? mainDepthSRV : skinCullHiZSRVs[level - 1].get();
			ID3D11UnorderedAccessView* dst = skinCullHiZScratch[level]->uav.get();
			context->CSSetShaderResources(1, 1, &src);
			context->CSSetUnorderedAccessViews(2, 1, &dst, nullptr);
			const auto& sd = skinCullHiZScratch[level]->desc;
			context->Dispatch((sd.Width + 7) / 8, (sd.Height + 7) / 8, 1);
			context->CSSetShaderResources(1, 1, nullCullSRVs);
			context->CSSetUnorderedAccessViews(2, 1, nullCullUAVs, nullptr);
			context->CopySubresourceRegion(skinCullHiZ->resource.get(), level, 0, 0, 0, skinCullHiZScratch[level]->resource.get(), 0, nullptr);
		}

		context->CSSetShader(skinCull, nullptr, 0);
		ID3D11ShaderResourceView* cullSRVs[3] = { skinCullBounds->srv.get(), skinCullHiZ->srv.get(),
			meshBounds ? meshBounds->srv.get() : nullptr };
		context->CSSetShaderResources(2, 3, cullSRVs);
		ID3D11UnorderedAccessView* argsUAV = skinCullArgs->uav.get();
		context->CSSetUnorderedAccessViews(3, 1, &argsUAV, nullptr);
		context->Dispatch((uint32_t(skinDraws.size()) + 63) / 64, 1, 1);

		// Cluster pass: refines the arguments of every skin the whole-skin
		// test kept, and compacts what survives into the scratch buffer both
		// draw loops read.
		auto* clusterCull = (clusterSkins > 0 && !clusterCullDisabled) ? GetClusterCullCS() : nullptr;
		if (clusterCull && clusterScratchIB && clusterScratchIB->uav) {
			ID3D11ShaderResourceView* clusterSRVs[2] = { clusterBounds->srv.get(), clusterIndexPool->srv.get() };
			context->CSSetShaderResources(5, 2, clusterSRVs);
			ID3D11UnorderedAccessView* scratchUAV = clusterScratchIB->uav.get();
			context->CSSetUnorderedAccessViews(4, 1, &scratchUAV, nullptr);
			context->CSSetShader(clusterCull, nullptr, 0);
			context->Dispatch(uint32_t(skinDraws.size()), 1, 1);
			ID3D11UnorderedAccessView* nullScratchUAV = nullptr;
			context->CSSetUnorderedAccessViews(4, 1, &nullScratchUAV, nullptr);
			context->CSSetShaderResources(5, 2, nullCullSRVs);
		}
		context->CSSetShaderResources(1, 4, nullCullSRVs);
		context->CSSetUnorderedAccessViews(2, 2, nullCullUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);

		// Census: the arguments and the pyramid's top texel two frames back,
		// read without waiting.
		context->CopyResource(skinCullArgsStaging[skinCullRing].get(), skinCullArgs->resource.get());
		if (skinCullHiZTopStaging[skinCullRing])
			context->CopySubresourceRegion(skinCullHiZTopStaging[skinCullRing].get(), 0, 0, 0, 0, skinCullHiZ->resource.get(), skinCullLevels - 1, nullptr);
		skinCullStagingIssued[skinCullRing] = true;
		skinCullStagingCount[skinCullRing] = uint32_t(skinDraws.size());
		const int readRing = (skinCullRing + 1) % kSkinCullRing;
		if (skinCullStagingIssued[readRing]) {
			D3D11_MAPPED_SUBRESOURCE rd{};
			if (SUCCEEDED(context->Map(skinCullArgsStaging[readRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &rd))) {
				const auto* args = static_cast<const uint32_t*>(rd.pData);
				uint32_t drawn = 0, culled = 0, trisDrawn = 0;
				uint32_t reasons[8] = {};
				for (uint32_t i = 0; i < skinCullStagingCount[readRing]; i++) {
					if (args[i * 5 + 1]) {
						drawn++;
						// The cluster pass rewrites this to what survived.
						trisDrawn += args[i * 5] / 3;
					} else
						culled++;
					reasons[std::min(args[i * 5 + 4], 7u)]++;
				}
				context->Unmap(skinCullArgsStaging[readRing].get(), 0);
				skinCullStagingIssued[readRing] = false;
				skinCullDrawnLast = drawn;
				skinCullCulledLast = culled;
				skinCullTrisDrawnLast = trisDrawn;
				memcpy(skinCullReasonLast, reasons, sizeof(reasons));
			}
			D3D11_MAPPED_SUBRESOURCE top{};
			if (skinCullHiZTopStaging[readRing] && SUCCEEDED(context->Map(skinCullHiZTopStaging[readRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &top))) {
				skinCullHiZTopLast = *static_cast<const float*>(top.pData);
				context->Unmap(skinCullHiZTopStaging[readRing].get(), 0);
			}
		}
		skinCullRing = (skinCullRing + 1) % kSkinCullRing;

		context->OMSetRenderTargets(8, rtvPtrs, dsv.get());
	}

	// The skin loop runs once (no prepass) or twice: non-carving skins
	// depth-only into the private copy, then every skin shading against it -
	// non-carving under EQUAL with writes off, carving under the shipping
	// LESS_EQUAL + write.
	// Every skin's block, filled once for both loops; the view taken here is
	// the one bound, so HasSmoothedNormals and the view never disagree.
	std::vector<StaticsCB> skinRecords(skinDraws.size());
	std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> skinSmoothSRVs(skinDraws.size());
	for (size_t si = 0; si < skinDraws.size(); si++) {
		const auto& d = skinDraws[si];
		skinSmoothSRVs[si].copy_from(EnsureSmoothedNormals(d.geometry));
		FillSkinDrawCB(*d.cap, d.s4Shell, d.vertexCount,
			skinSmoothSRVs[si] != nullptr, objectTopSRV != nullptr, skinNormalsSRV != nullptr, skinRecords[si]);
	}
	const bool skinRecordsLive = !skinRecords.empty() && UploadStaticsRecords(skinRecords.data(), (uint32_t)skinRecords.size());
	uint32_t skinParity = 0;

	auto drawSkins = [&](bool a_prepass) {
		boundStaticsPS = nullptr;
		uint32_t drawIndex = 0;
		bool boundDecal = false;
		for (const auto& d : skinDraws) {
			const auto& cap = *d.cap;
			if (d.decalDepth != boundDecal) {
				boundDecal = d.decalDepth;
				context->RSSetState(GetSkinRasterState(boundDecal));
				if (vpCount)
					context->RSSetViewports(vpCount, boundDecal ? decalVps : vps);
			}
			[[maybe_unused]] auto* geometry = d.geometry;
			context->IASetInputLayout(d.layout);
			UINT stride = d.stride;
			UINT offset = 0;
			ID3D11Buffer* vb = d.vb;
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			// Cluster skins draw the compacted stream; everything else draws
			// the mesh's own indices, as before.
			if (cullActive && d.clusterCount != 0)
				context->IASetIndexBuffer(clusterScratchIB->resource.get(), DXGI_FORMAT_R32_UINT, 0);
			else
				context->IASetIndexBuffer(d.ib, DXGI_FORMAT_R16_UINT, 0);

			// Smoothed normals (built once per unique mesh): pillow inflation
			// for flat split-normal surfaces; planks, roofs, pole caps.
			const uint32_t recordIndex = drawIndex++;
			ID3D11ShaderResourceView* skinSmoothSRV = skinSmoothSRVs[recordIndex].get();
			context->VSSetShaderResources(10, 1, &skinSmoothSRV);
			const bool wantTess = tessellateSkins && !d.s4Shell;
			if (wantTess != skinStagesTess)
				bindSkinStages(wantTess);
			if (skinRecordsLive) {
				BindStaticsRecord(recordIndex, true, wantTess, skinParity);
			} else {
				staticsCB->Update(skinRecords[recordIndex]);
				cpuCensus.skinLoopCBUpdates++;
			}

			// Depth export only where the carve can fire: SnowStaticsShell's
			// carveObject is LegacySkin, i.e. cap.road. Everything else writes
			// back the rasterised depth, so dropping the export leaves the same
			// number in the buffer and hands early-Z rejection back to the whole
			// pass. The debug spike forces the no-depth path on every draw, roads
			// included. The prepass twins mirror the same split so the private
			// depth holds exactly what the shipping shaders would have written.
			const bool needsDepth = !staticsEarlyZSpike && cap.road;
			if (a_prepass && needsDepth)
				continue;
			ID3D11PixelShader* wantPS = a_prepass ? staticsPSPrepassNoDepth :
			                                        ((!needsDepth && staticsPSNoDepth) ? staticsPSNoDepth : staticsPS);
			if (wantPS != boundStaticsPS) {
				context->PSSetShader(wantPS, nullptr, 0);
				boundStaticsPS = wantPS;
			}
			if (prepass && !a_prepass) {
				ID3D11DepthStencilState* wantDepth = needsDepth ? shellDepthState.get() : shellPrepassMainDepthState.get();
				if (wantDepth != boundDepthState) {
					context->OMSetDepthStencilState(wantDepth, 0);
					boundDepthState = wantDepth;
				}
			}

			cpuCensus.skinLoopDraws++;
			if (cullActive)
				context->DrawIndexedInstancedIndirect(skinCullArgs->resource.get(), d.slot * kSkinCullArgStride);
			else
				context->DrawIndexed(d.indexCount, 0, 0);
		}
		if (boundDecal) {
			context->RSSetState(GetSkinRasterState());
			if (vpCount)
				context->RSSetViewports(vpCount, vps);
		}
	};

	if (prepass) {
		auto fill = [&](ID3D11DepthStencilView* a_target, ID3D11ShaderResourceView* a_source) {
			context->OMSetRenderTargets(0, nullptr, a_target);
			context->OMSetDepthStencilState(shellFillDepthState.get(), 0);
			context->RSSetState(shellRasterState.get());
			if (vpCount)
				context->RSSetViewports(vpCount, fillVps);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(fillVS, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->PSSetShaderResources(9, 1, &a_source);
			context->PSSetShader(fillPS, nullptr, 0);
			context->Draw(3, 0);
			ID3D11ShaderResourceView* nullFillSRV = nullptr;
			context->PSSetShaderResources(9, 1, &nullFillSRV);
			if (vpCount)
				context->RSSetViewports(vpCount, vps);
			if (auto* skinRaster = GetSkinRasterState())
				context->RSSetState(skinRaster);
			bindSkinStages(skinStagesTess);
		};

		fill(shellTestDepthDSV.get(), mainDepthSRV);
		context->OMSetRenderTargets(0, nullptr, shellTestDepthDSV.get());
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
		drawSkins(true);

		context->OMSetRenderTargets(8, rtvPtrs, shellTestDepthDSV.get());
		boundDepthState = nullptr;
		drawSkins(false);

		fill(dsv.get(), shellTestDepthSRV.get());
		context->OMSetRenderTargets(8, rtvPtrs, dsv.get());
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
	} else {
		drawSkins(false);
	}
	globals::profiler->EndPass();

	// Everything after inherits b1 rather than binding it: put staticsCB
	// back on every stage the offset path rebound, or the trench patch
	// draws against the record buffer at the last skin's offset (shadowed
	// road floors, Josef 2026-09-06).
	if (skinRecordsLive) {
		ID3D11Buffer* restore = staticsCB->CB();
		context->VSSetConstantBuffers(1, 1, &restore);
		context->PSSetConstantBuffers(1, 1, &restore);
		context->HSSetConstantBuffers(1, 1, &restore);
		context->DSSetConstantBuffers(1, 1, &restore);
	}

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
	context->PSSetShaderResources(3, 1, &nullSmoothSRV);
	context->VSSetShaderResources(13, 1, &nullSmoothSRV);
	context->DSSetShaderResources(13, 1, &nullSmoothSRV);
	context->PSSetShaderResources(13, 1, &nullSmoothSRV);
	context->HSSetShaderResources(13, 1, &nullSmoothSRV);
	context->PSSetShaderResources(21, 1, &nullSmoothSRV);
	context->PSSetShaderResources(23, 1, &nullSmoothSRV);
	context->PSSetShaderResources(32, 1, &nullSmoothSRV);
	context->VSSetShaderResources(25, 1, &nullSmoothSRV);
	context->DSSetShaderResources(25, 1, &nullSmoothSRV);

	// trench PATCH: the landscape shell's dense-grid carve applied to object
	// tops; real carved geometry drawn after the skins so it shows through
	// their dithered trench hand-off holes. SV_VertexID grid, no IA state.
	// Per-class trenching emerges from the raster: each captured object
	// writes its own class depth into the skin-depth raster, so a class at
	// 0 produces dead patch texels for its objects only. Roads are the one
	// class with depth, so the gate is theirs (the old > 1 threshold
	// silently disabled the whole patch at depth 1).
	if (patchVS && patchPS && heightSkinDepth && settings.RoadMeshesDepth > 0.5f) {
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
		// Scene depth copy (PS t3), as for the skins: the skin pass unbinds it
		// before this block, so it has to be bound again here.
		ID3D11ShaderResourceView* patchDepthSRV = Util::GetCurrentSceneDepthSRV(false);
		context->PSSetShaderResources(3, 1, &patchDepthSRV);
		// Skin-depth raster (PS t12): the march rebuilds the carved layer on
		// road-owned taps. Bound explicitly - inheriting the skin pass's bind
		// through D3D11 state persistence worked but was one reorder away
		// from a silently unbound read.
		ID3D11ShaderResourceView* patchSkinPSSRV = (heightSkinDepth && heightSkinDepth->srv) ?
		                                               heightSkinDepth->srv.get() :
		                                               nullptr;
		context->PSSetShaderResources(12, 1, &patchSkinPSSRV);

		// ONE recipe with the shadow caster (FillPatchDrawCB): the caster must
		// be the exact surface this draw renders.
		StaticsCB scb{};
		FillPatchDrawCB(scb);
		staticsCB->Update(scb);

		ID3D11ShaderResourceView* patchSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
		context->VSSetShaderResources(11, 2, patchSRVs);
		// Terrain window (t0) + object snow cone (t13) for the road-verge
		// depth blend, in whichever stage evaluates BuildPatchVertex (the VS
		// on the legacy grid, the DS when tessellating). Explicit binds: both
		// were previously reachable only through state left over from earlier
		// passes, which is one reorder from an unbound read.
		ID3D11ShaderResourceView* patchTerrainSRV = shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr;
		ID3D11ShaderResourceView* patchConeSRV = (objectSnowCone && objectSnowCone->srv) ? objectSnowCone->srv.get() : nullptr;
		context->VSSetShaderResources(0, 1, &patchTerrainSRV);
		context->VSSetShaderResources(13, 1, &patchConeSRV);
		if (tessellatePatch) {
			context->DSSetShaderResources(0, 1, &patchTerrainSRV);
			context->DSSetShaderResources(13, 1, &patchConeSRV);
		}
		if (tessellatePatch)
			context->Draw(kPatchGridDim * kPatchGridDim * 4, 0);
		else
			context->Draw(kPatchGridDim * kPatchGridDim * 6, 0);
		if (tessellatePatch) {
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}

		ID3D11ShaderResourceView* nullHeightSRVs[2] = { nullptr, nullptr };
		context->VSSetShaderResources(11, 2, nullHeightSRVs);
		globals::profiler->EndPass();
	}

	// Volume snow last: it depth-tests against everything above, skins
	// included, and shades through the same material.
	DrawVoxelSnow();

	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(1, 1, &nullCB);
	context->PSSetConstantBuffers(1, 1, &nullCB);
}

void SnowDeformation::RenderExclusionField()
{
	LoadTraceScope _loadTrace(this, "Statics: RenderExclusionField");
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

// Position size = distance to the first following attribute (the descriptor's
// offset table is authoritative). The VF_FULLPREC flag is NOT reliable: logged
// runtime buffers carry 16-byte float4 positions with the flag clear, and
// reading them as halfs shreds geometry into screen-wide streaks.
static uint32_t SD_PositionBytes(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc)
{
	uint32_t positionBytes = uint32_t(a_descKey & 0xF) * 4;
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
		if (a_desc.HasFlag(flag)) {
			uint32_t attrOffset = a_desc.GetAttributeOffset(attr);
			if (attrOffset > 0 && attrOffset < positionBytes)
				positionBytes = attrOffset;
		}
	}
	return positionBytes;
}

ID3D11InputLayout* SnowDeformation::ContactSkinInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc)
{
	auto& layout = contactSkinILCache[a_descKey];
	if (!layout && contactSkinVSBlob) {
		// SSE skinning block: four float16 weights then four UNORM byte
		// indices, 12 bytes, at the descriptor's own skinning offset.
		const uint32_t skinOffset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
		const uint32_t positionBytes = SD_PositionBytes(a_descKey, a_desc);
		D3D11_INPUT_ELEMENT_DESC elements[3] = {
			{ "POSITION", 0, positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, skinOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, skinOffset + 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		// Null stays cached: this descriptor is skipped from now on.
		globals::d3d::device->CreateInputLayout(elements, 3, contactSkinVSBlob->GetBufferPointer(), contactSkinVSBlob->GetBufferSize(), layout.put());
	}
	return layout.get();
}

ID3D11InputLayout* SnowDeformation::StaticsInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc)
{
	auto& layout = staticsILCache[a_descKey];
	if (!layout && staticsVSBlob) {
		const uint32_t positionBytes = SD_PositionBytes(a_descKey, a_desc);

		D3D11_INPUT_ELEMENT_DESC elements[2] = {
			{ "POSITION", 0, positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		// Null stays cached: this descriptor is skipped from now on.
		globals::d3d::device->CreateInputLayout(elements, 2, staticsVSBlob->GetBufferPointer(), staticsVSBlob->GetBufferSize(), layout.put());
	}
	return layout.get();
}

struct SD_BSWaterShader_SetupGeometry
{
	static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
	{
		func(a_shader, a_pass, a_flags);
		auto& snowDeformation = globals::features::snowDeformation;
		if (snowDeformation.loaded)
			snowDeformation.BSWaterShader_SetupGeometry(a_pass);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void SnowDeformation::InstallWaterCaptureHook()
{
	logger::info("[SNOW DEFORMATION] Hooking BSWaterShader::SetupGeometry");
	stl::write_vfunc<0x6, SD_BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
}

void SnowDeformation::BSWaterShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->geometry)
		return;
	// One entry per plane: the same geometry sets up once per water pass.
	for (const auto& water : capturedWater)
		if (water.geometry.get() == a_pass->geometry)
			return;
	if (capturedWater.size() >= 512)
		return;
	capturedWater.push_back({ RE::NiPointer<RE::BSGeometry>(a_pass->geometry), a_pass->geometry->world });
}

// Last frame's water planes top-down into the terrain window's frame (128-unit
// texels, MAX blend); the landscape shell ends where its ground lies under it.
void SnowDeformation::RenderWaterCapture()
{
	auto context = globals::d3d::context;
	if (waterCaptureShadersFailed || !staticsCB || !heightMaxBlendState) {
		capturedWater.clear();
		return;
	}
	if (!waterCaptureVS) {
		const auto path = L"Data\\Shaders\\SnowDeformation\\SnowHeightCapture.hlsl";
		waterCaptureVSBlob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "WATER"));
		winrt::com_ptr<ID3DBlob> psBlob;
		psBlob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "WATER"));
		if (!waterCaptureVSBlob || !psBlob ||
			FAILED(globals::d3d::device->CreateVertexShader(waterCaptureVSBlob->GetBufferPointer(), waterCaptureVSBlob->GetBufferSize(), nullptr, &waterCaptureVS)) ||
			FAILED(globals::d3d::device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &waterCapturePS))) {
			logger::error("[SNOW DEFORMATION] Water capture shaders failed; the shell will not end at water");
			waterCaptureShadersFailed = true;
			capturedWater.clear();
			return;
		}
		Util::SetResourceName(waterCaptureVS, "SnowDeformation::WaterCaptureVS");
		Util::SetResourceName(waterCapturePS, "SnowDeformation::WaterCapturePS");
	}
	if (!waterHeightTexture) {
		D3D11_TEXTURE2D_DESC desc = {
			.Width = kShellWindowDim,
			.Height = kShellWindowDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		waterHeightTexture = new Texture2D(desc, "SnowDeformation::WaterWindow");
		waterHeightTexture->CreateSRV(srvDesc);
		waterHeightTexture->CreateRTV(rtvDesc);
		waterWindowCellX = INT_MIN;
	}
	// The terrain window's frame; cleared only when it moves.
	if (waterWindowCellX != shellWindowCellX || waterWindowCellY != shellWindowCellY) {
		const float clear[4] = { kShellMissingHeight, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(waterHeightTexture->rtv.get(), clear);
		waterWindowCellX = shellWindowCellX;
		waterWindowCellY = shellWindowCellY;
	}
	if (capturedWater.empty())
		return;

	globals::profiler->BeginPass("SnowDeformation::WaterCapture");
	ID3D11RenderTargetView* rtv = waterHeightTexture->rtv.get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetBlendState(heightMaxBlendState.get(), nullptr, 0xFFFFFFFF);
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(kShellWindowDim), float(kShellWindowDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(waterCaptureVS, nullptr, 0);
	context->PSSetShader(waterCapturePS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);

	const float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	const float span = kShellVertexSpacing * kShellWindowDim;
	StaticsCB rec{};
	rec.HeightWindowCenter = { shellWindowCellX * cellSize + span * 0.5f, shellWindowCellY * cellSize + span * 0.5f };
	rec.HeightHalfExtent = span * 0.5f;
	for (const auto& water : capturedWater) {
		auto* geometry = water.geometry.get();
		auto rendererData = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
			continue;
		auto triShape = geometry->AsTriShape();
		if (!triShape)
			continue;
		const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		if (indexCount == 0)
			continue;
		auto desc = rendererData->vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
			continue;
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		auto& layout = waterILCache[descKey];
		if (!layout) {
			const uint32_t positionBytes = SD_PositionBytes(descKey, desc);
			D3D11_INPUT_ELEMENT_DESC element = { "POSITION", 0, positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
			globals::d3d::device->CreateInputLayout(&element, 1, waterCaptureVSBlob->GetBufferPointer(), waterCaptureVSBlob->GetBufferSize(), layout.put());
		}
		if (!layout)
			continue;
		context->IASetInputLayout(layout.get());
		const UINT stride = uint32_t(descKey & 0xF) * 4;
		if (stride == 0)
			continue;
		const UINT offset = 0;
		auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
		auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
		context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
		const auto& rot = water.world.rotate;
		const float scale = water.world.scale;
		rec.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, water.world.translate.x };
		rec.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, water.world.translate.y };
		rec.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, water.world.translate.z };
		staticsCB->Update(rec);
		context->DrawIndexed(indexCount, 0, 0);
	}
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	globals::profiler->EndPass();
	capturedWater.clear();
}

bool SnowDeformation::EnsureContactResources()
{
	if (contactShadersFailed)
		return false;
	auto* device = globals::d3d::device;
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowContactCapture.hlsl";
	if (!contactVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &contactVS)))
			Util::SetResourceName(contactVS, "SnowDeformation::ContactCaptureVS");
	}
	if (!contactPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &contactPS)))
			Util::SetResourceName(contactPS, "SnowDeformation::ContactCapturePS");
	}
	if (!contactSkinVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SKINNED"));
		if (blob) {
			// Kept: input layouts must be created against the VS bytecode.
			contactSkinVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &contactSkinVS)))
				Util::SetResourceName(contactSkinVS, "SnowDeformation::ContactCaptureSkinVS");
		}
	}
	if (!contactSkinCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(ContactSkinCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		contactSkinCB = new ConstantBuffer(cbDesc, "SnowDeformation::ContactSkinCB");
	}
	if (!contactVS || !contactPS) {
		contactShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Prop contact capture disabled (shader compilation failed)");
		return false;
	}
	if (!contactHeight) {
		D3D11_TEXTURE2D_DESC desc = {
			.Width = kContactDim,
			.Height = kContactDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		contactHeight = new Texture2D(desc, "SnowDeformation::ContactHeight");
		contactHeight->CreateSRV(srvDesc);
		contactHeight->CreateRTV(rtvDesc);
	}
	if (!contactMinBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_ONE;
		rt.BlendOp = D3D11_BLEND_OP_MIN;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_MIN;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blendDesc, contactMinBlendState.put()))) {
			contactShadersFailed = true;
			return false;
		}
	}
	if (!contactRasterState) {
		// Undersides are the surfaces that touch the snow: no culling.
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&rasterDesc, contactRasterState.put()))) {
			contactShadersFailed = true;
			return false;
		}
	}
	return true;
}

// Rasterizes this frame's contact props (gathered by the prop scan) from
// above into the contact field. Runs in Prepass after the stamp gather and
// before the map update that reads it. Same per-geometry draw the height
// capture uses; the rasterizer state is saved and put back because the
// capture leaves the game's alone and so must this.
void SnowDeformation::DrawContactCapture(ID3D11DeviceContext* a_context)
{
	contactDrawsLast = 0;
	if ((contactProps.empty() && contactActors.empty()) || !EnsureContactResources())
		return;
	auto* context = a_context;

	const float clearValue[4] = { kContactNone, 0.0f, 0.0f, 0.0f };
	context->ClearRenderTargetView(contactHeight->rtv.get(), clearValue);
	ID3D11RenderTargetView* rtv = contactHeight->rtv.get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetBlendState(contactMinBlendState.get(), nullptr, 0xFFFFFFFF);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	context->RSSetState(contactRasterState.get());
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(kContactDim), float(kContactDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(contactVS, nullptr, 0);
	context->PSSetShader(contactPS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);

	// A carried mesh that jumps further than this in one frame was equipped,
	// swapped or teleported, not swung: it prints where it now stands.
	constexpr float kContactSweepTeleport = 200.0f;
	// One rigid mesh into the field, by its own world transform. Props use it
	// for every mesh; actors use it for what they carry. With a_sweep, the
	// mesh is also drawn at interpolated poses back toward where it stood
	// last frame: the field is a snapshot, and a swung blade crosses several
	// texels between frames, which prints as rungs rather than a slash.
	auto drawRigid = [&](RE::BSGeometry* a_geometry, bool a_sweep = false) -> bool {
		auto& runtime = a_geometry->GetGeometryRuntimeData();
		auto* triShape = a_geometry->AsTriShape();
		if (!triShape)
			return false;
		auto* rendererData = runtime.rendererData;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
			return false;
		const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		if (indexCount == 0)
			return false;
		auto desc = rendererData->vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
			return false;
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		auto* layout = StaticsInputLayoutFor(descKey, desc);
		if (!layout)
			return false;
		const UINT stride = uint32_t(descKey & 0xF) * 4;
		if (stride == 0)
			return false;
		UINT offset = 0;
		auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
		auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
		context->IASetInputLayout(layout);
		context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);

		const auto& world = a_geometry->world;
		auto rowsFrom = [&](const RE::NiTransform& a_x, StaticsCB& a_cb) {
			const auto& r = a_x.rotate;
			const float sc = a_x.scale;
			a_cb.WorldRow0 = { r.entry[0][0] * sc, r.entry[0][1] * sc, r.entry[0][2] * sc, a_x.translate.x };
			a_cb.WorldRow1 = { r.entry[1][0] * sc, r.entry[1][1] * sc, r.entry[1][2] * sc, a_x.translate.y };
			a_cb.WorldRow2 = { r.entry[2][0] * sc, r.entry[2][1] * sc, r.entry[2][2] * sc, a_x.translate.z };
			a_cb.HeightWindowCenter = contactCenter;
			a_cb.HeightHalfExtent = kContactHalfExtent;
		};

		uint32_t steps = 1;
		RE::NiTransform previous;
		if (a_sweep) {
			auto& state = contactSweepStates[a_geometry];
			const bool fresh = state.frame + 1 != contactSweepFrame;
			previous = state.world;
			state.world = world;
			state.frame = contactSweepFrame;
			if (!fresh) {
				// How far the mesh's far edge travelled: the origin's own step plus
				// the arc its radius swept. cos(angle) from the relative rotation.
				// Below a texel of travel there is nothing new to print - a sheathed
				// sword on a standing body, or a shield at rest - and not drawing it
				// is what keeps the field empty for the update pass's sleep.
				const float radius = a_geometry->worldBound.radius;
				float trace = 0.0f;
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 3; ++c)
						trace += world.rotate.entry[r][c] * previous.rotate.entry[r][c];
				const float angle = std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f));
				const float travel = world.translate.GetDistance(previous.translate) + radius * angle;
				if (travel < kContactSweepStep)
					return false;
				// A jump this large is a new placement (equip, cell load), not a swing.
				if (travel < kContactSweepTeleport)
					steps = std::clamp(uint32_t(std::ceil(travel / kContactSweepStep)), 1u, kContactMaxSweep);
			}
		}

		StaticsCB scb{};
		for (uint32_t step = 1; step <= steps; ++step) {
			if (step == steps) {
				rowsFrom(world, scb);
			} else {
				// Straight lerp of the rows: over one frame's rotation the chord
				// shortens the mesh by a fraction of a percent, far under a texel.
				const float t = float(step) / float(steps);
				RE::NiTransform blend = world;
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 3; ++c)
						blend.rotate.entry[r][c] = std::lerp(previous.rotate.entry[r][c], world.rotate.entry[r][c], t);
				blend.translate = previous.translate * (1.0f - t) + world.translate * t;
				blend.scale = std::lerp(previous.scale, world.scale, t);
				rowsFrom(blend, scb);
				contactSweepLast++;
			}
			staticsCB->Update(scb);
			context->DrawIndexed(indexCount, 0, 0);
		}
		return true;
	};

	globals::profiler->BeginPass("SnowDeformation::ContactCapture");
	for (const auto& prop : contactProps) {
		// Re-resolved, not held: see ContactProp. A body whose 3D went away
		// this frame simply drops out.
		auto ref = prop.ref.get();
		auto* root = ref ? ref->Get3D(false) : nullptr;
		if (!root)
			continue;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
			if (a_geometry->GetGeometryRuntimeData().skinInstance)
				return RE::BSVisit::BSVisitControl::kContinue;
			if (drawRigid(a_geometry))
				contactDrawsLast++;
			return RE::BSVisit::BSVisitControl::kContinue;
		});
	}
	globals::profiler->EndPass();

	// S1 spike: actors, drawn from their SKINNED meshes. Bone palettes are
	// built here as absolute world transforms (bone node world * that bone's
	// skinToBone), one upload per skin partition, so the VS needs no object
	// transform and none of the game's pivot convention. A missing bone
	// contributes nothing rather than crashing - which is the whole
	// edited-skeleton family, handled by construction.
	contactSkinDrawsLast = 0;
	contactSkinMissingLast = 0;
	contactCarriedLast = 0;
	contactOverlaysLast = 0;
	contactShellsLast = 0;
	contactStillLast = 0;
	contactHiddenPartsLast = 0;
	contactGlobalPartsLast = 0;
	if (contactSkinIndexing.size() > 1024)
		contactSkinIndexing.clear();
	contactSweepLast = 0;
	contactSweepFrame++;
	if (contactSweepStates.size() > 512)
		contactSweepStates.clear();
	if (!contactActors.empty() && contactSkinVS && contactSkinCB) {
		globals::profiler->BeginPass("SnowDeformation::ContactSkin");
		context->VSSetShader(contactSkinVS, nullptr, 0);
		ID3D11Buffer* skinCB = contactSkinCB->CB();
		context->VSSetConstantBuffers(2, 1, &skinCB);
		for (const auto& actor : contactActors) {
			auto ref = actor.ref.get();
			auto* root = ref ? ref->Get3D(false) : nullptr;
			if (!root)
				continue;
			if (actor.still)
				contactStillLast++;
			const uint drawsBeforeActor = contactSkinDrawsLast;
			int geometryIndex = -1;
			bool skinnedVSBound = true;
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
				auto& runtime = a_geometry->GetGeometryRuntimeData();
				auto* skin = runtime.skinInstance.get();
				// RaceMenu overlays ('[Ovl0]'..'[Ovl5]', '[SOvl0]') are clones of the
				// body and hands that exist to layer textures: the same surface
				// again, up to seven times over. One copy is enough.
				if (const char* name = a_geometry->name.c_str(); name && (std::strstr(name, "[Ovl") || std::strstr(name, "[SOvl"))) {
					contactOverlaysLast++;
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				// Creature fur is shells: the body duplicated and pushed out along
				// its normals up to sixteen times, alpha-tested so only strands
				// show. Solid, each is an inflated body; the base mesh under them
				// is the surface. Bethesda names every one '*shell*'. Blended
				// (translucent) geometry is not a surface either.
				if (auto* alpha = runtime.alphaProperty.get()) {
					const char* name = a_geometry->name.c_str();
					const bool shell = alpha->GetAlphaTesting() && name && (std::strstr(name, "shell") || std::strstr(name, "Shell"));
					if (shell || alpha->GetAlphaBlending()) {
						contactShellsLast++;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
				}
				// Draw only what the game draws: a geometry hidden by itself or by
				// any ancestor (dismembered parts, physics helper meshes, alternate
				// variants), or one with no shader to render it, never reaches the
				// screen and must not reach the snow.
				{
					bool hidden = false;
					for (const RE::NiAVObject* n = a_geometry; n && !hidden; n = n->parent)
						hidden = n->GetAppCulled();
					// Effect art - glows, light rays, runes, blood decals, an ENB light's
					// 500-unit billboard - is drawn with an effect shader, never a
					// lighting one. It is not a surface and must not carve snow.
					auto* property = runtime.shaderProperty.get();
					const bool lit = property && property->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get();
					if (hidden || !lit) {
						if (contactSkinHiddenLogged.size() < 32 && contactSkinHiddenLogged.insert(a_geometry).second)
							logger::info("[SNOW DEFORMATION] contact geometry '{}' on '{}' is {} (bound radius {:.0f}); not drawn",
								a_geometry->name.c_str() ? a_geometry->name.c_str() : "", ref->GetDisplayFullName(),
								hidden ? "hidden by the game" : (property ? "effect art, not a surface" : "without a shader"),
								a_geometry->worldBound.radius);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
				}
				// A part whose whole bound floats above the layer carves nothing,
				// and the draws are the cost: on a standing actor that leaves the
				// boots and calves, and a sheathed sword at the hip stays out until
				// a swing brings it down. Corpses lie low and keep everything.
				{
					const auto& gb = a_geometry->worldBound;
					if (gb.radius > 0.0f && gb.center.z - gb.radius > actor.groundZ + actor.layer + kContactSkipMargin)
						return RE::BSVisit::BSVisitControl::kContinue;
				}
				// Carried gear - weapons, shields, torches - is rigid, hung off a
				// bone. It prints by its own world transform when it dips into the
				// snow: a low sword swing cuts a slash.
				if (!skin) {
					if (skinnedVSBound) {
						context->VSSetShader(contactVS, nullptr, 0);
						skinnedVSBound = false;
					}
					if (drawRigid(a_geometry, true))
						contactCarriedLast++;
					return RE::BSVisit::BSVisitControl::kContinue;
				}
				// A still body's print is already in the map: its skin is not drawn.
				if (actor.still)
					return RE::BSVisit::BSVisitControl::kContinue;
				if (!skinnedVSBound) {
					context->VSSetShader(contactSkinVS, nullptr, 0);
					skinnedVSBound = true;
				}
				// Solo: draw one skinned geometry only, so the field shows whose
				// silhouette is which. -1 draws them all.
				++geometryIndex;
				if (debugContactSolo >= 0) {
					if (geometryIndex != debugContactSolo)
						return RE::BSVisit::BSVisitControl::kContinue;
					contactSoloName = a_geometry->name.c_str() ? a_geometry->name.c_str() : "(unnamed)";
				}
				auto* skinData = skin->skinData.get();
				auto* skinPartition = skin->skinPartition.get();
				// Accessors, not the raw members: those are compiled out under
				// cross-VR targeting, and these relocate per runtime.
				// partitions is a bare array behind a count; an instance mid-
				// rebuild can carry a count with no array.
				if (!skinData || !skinPartition || !skin->bones || !skinPartition->partitions.data())
					return RE::BSVisit::BSVisitControl::kContinue;
				const uint32_t boneCount = skinData->GetBoneCount();
				// Dismember partitions carry the game's own visibility: a creature
				// keeps alternate or severable parts in partitions it hides by
				// default, and the bones only those parts use resolve to no node.
				// Drawn anyway, their vertices collapse to the world origin as
				// slivers. Honour the flag, then refuse any partition whose bones
				// resolve to nothing at all as the same case without the flag.
				auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin);
				const RE::BSDismemberSkinInstance::Data* dismemberParts = nullptr;
				uint32_t dismemberCount = 0;
				if (dismember) {
					const auto& rd = dismember->GetRuntimeData();
					if (rd.partitions && rd.numPartitions > 0) {
						dismemberParts = rd.partitions;
						dismemberCount = uint32_t(rd.numPartitions);
					}
				}
				// How this skin's vertices index bones. SSE partitions were assumed to
				// carry partition-local indices, and the shader refuses an index past
				// the partition's count - parking the vertex on slot 0. A mesh whose
				// vertices index the skin's FULL list instead fans every high-boned
				// vertex onto one bone: the mammoth's spikes. Audit once from the raw
				// vertex copy; a skin that ever indexes past a partition's count is
				// drawn from a whole-skin palette.
				uint8_t indexing = 0;
				if (auto it = contactSkinIndexing.find(skin); it != contactSkinIndexing.end()) {
					indexing = it->second;
				} else {
					indexing = 1;
					std::string audit;
					for (uint32_t p = 0; p < skinPartition->numPartitions; ++p) {
						const auto& part = skinPartition->partitions[p];
						auto* buff = part.buffData;
						if (!buff || !buff->rawVertexData || !part.bones || part.numBones == 0)
							continue;
						auto partDesc = buff->vertexDesc;
						if (!partDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
							continue;
						uint64_t key;
						memcpy(&key, &partDesc, sizeof(key));
						const uint32_t stride = uint32_t(key & 0xF) * 4;
						const uint32_t skinOffset = partDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
						if (stride == 0)
							continue;
						uint32_t over = 0, maxIndex = 0;
						for (uint32_t v = 0; v < part.vertices; ++v) {
							const uint8_t* base = buff->rawVertexData + size_t(v) * stride;
							uint16_t wh[4];
							std::memcpy(wh, base + skinOffset, sizeof(wh));
							uint8_t idx[4];
							std::memcpy(idx, base + skinOffset + 8, sizeof(idx));
							bool vOver = false;
							for (int k = 0; k < 4; ++k) {
								if (wh[k] == 0)
									continue;
								maxIndex = std::max(maxIndex, uint32_t(idx[k]));
								vOver |= idx[k] >= part.numBones;
							}
							over += vOver;
						}
						if (over > 0)
							indexing = 2;
						audit += std::format("[p{}: {} bones, max index {}, {} of {} verts past the partition] ", p, part.numBones, maxIndex, over, part.vertices);
					}
					contactSkinIndexing[skin] = indexing;
					if (indexing == 2 || contactSkinIndexing.size() <= 24)
						logger::info("[SNOW DEFORMATION] index audit '{}' on '{}': {} bones in the skin -> {} {}",
							a_geometry->name.c_str() ? a_geometry->name.c_str() : "", ref->GetDisplayFullName(), boneCount,
							indexing == 2 ? "GLOBAL (whole-skin palette)" : "local", audit);
				}
				bool globalUploaded = false;
				auto boneResolves = [&](uint16_t a_bone) -> bool {
					if (a_bone >= boneCount)
						return false;
					if (skin->bones[a_bone])
						return true;
					if (!skin->boneMatrices || a_bone >= skin->numMatrices)
						return false;
					// An unset matrix in the game's palette: all-zero rows, or an
					// identity rotation sitting at the world origin.
					const float* m = reinterpret_cast<const float*>(skin->boneMatrices) + size_t(a_bone) * 12;
					float rot = 0.0f, pos = 0.0f;
					for (int r = 0; r < 3; ++r) {
						for (int c = 0; c < 3; ++c)
							rot += std::abs(m[r * 4 + c]);
						pos += std::abs(m[r * 4 + 3]);
					}
					return rot > 1e-3f && pos > 1.0f;
				};
				// One composed transform per skin bone, built on first use: every
				// partition of this geometry that names the bone reuses it.
				contactPaletteScratch.resize(boneCount);
				contactPaletteBuilt.assign(boneCount, 0);
				auto composedFor = [&](uint16_t a_bone, uint32_t a_partition, uint16_t a_slot) -> const RE::NiTransform& {
					static RE::NiTransform standIn;
					auto* boneNode = a_bone < boneCount ? skin->bones[a_bone] : nullptr;
					const RE::NiTransform* boneWorld = boneNode ? &boneNode->world : nullptr;
					if (!boneWorld && a_bone < skin->numMatrices && skin->boneMatrices) {
						// A slot with no node - creature skins have several - is not a
						// missing bone: the game renders the mesh correctly, so its own
						// uploaded palette holds the right matrix. Traced 2026-09-02:
						// three float4 rows per bone, rotation times scale with the
						// translation in .w, ABSOLUTE world, identical to our composition
						// where both exist. Read it straight; it is at most a frame old.
						if (!contactPaletteBuilt[a_bone]) {
							const float* m = reinterpret_cast<const float*>(skin->boneMatrices) + size_t(a_bone) * 12;
							RE::NiTransform& t = contactPaletteScratch[a_bone];
							for (int r = 0; r < 3; ++r) {
								for (int c = 0; c < 3; ++c)
									t.rotate.entry[r][c] = m[r * 4 + c];
								t.translate[r] = m[r * 4 + 3];
							}
							t.scale = 1.0f;
							contactPaletteBuilt[a_bone] = 1;
						}
						return contactPaletteScratch[a_bone];
					}
					if (!boneWorld) {
						// A bone the skeleton lacks (an editor removed it) or one past the
						// skin data's count cannot be left as zero rows: the vertex would
						// keep its weight on nothing and be pulled toward the world origin
						// by that fraction - the comb. Stand in with the skin's root at the
						// bone's bind pose, which holds the vertex near the body.
						contactSkinMissingLast++;
						RE::NiAVObject* stand = skin->rootParent ? skin->rootParent : root;
						standIn = a_bone < boneCount ? stand->world * skinData->GetBoneDataSkinToBone(a_bone) : stand->world;
						if (contactSkinMissingLogged.size() < 32 && contactSkinMissingLogged.insert(skin).second)
							logger::info("[SNOW DEFORMATION] contact skin '{}' on '{}': partition {} slot {} names bone {} of {} which the skeleton lacks; standing in with '{}'",
								a_geometry->name.c_str() ? a_geometry->name.c_str() : "", ref->GetDisplayFullName(), a_partition, a_slot, a_bone, boneCount,
								stand->name.c_str() ? stand->name.c_str() : "");
						return standIn;
					}
					if (!contactPaletteBuilt[a_bone]) {
						contactPaletteScratch[a_bone] = *boneWorld * skinData->GetBoneDataSkinToBone(a_bone);
						contactPaletteBuilt[a_bone] = 1;
					}
					return contactPaletteScratch[a_bone];
				};
				// The constant buffer is uploaded once per DISTINCT bone list: the
				// partitions of one garment usually share theirs.
				ContactSkinCB cb{};
				cb.SkinWindowCenter = contactCenter;
				cb.SkinHalfExtent = kContactHalfExtent;
				const uint16_t* paletteBones = nullptr;
				uint16_t paletteCount = 0;
				if (contactSkinLogged.size() < 16 && contactSkinLogged.insert(skin).second) {
					std::string layout;
					for (uint32_t p = 0; p < skinPartition->numPartitions; ++p) {
						const auto& part = skinPartition->partitions[p];
						layout += std::format("[{} tris, {} bones, buf {:p}] ", part.triangles, part.numBones, (const void*)part.buffData);
					}
					logger::info("[SNOW DEFORMATION] contact skin '{}': {} partitions {}",
						a_geometry->name.c_str() ? a_geometry->name.c_str() : "", skinPartition->numPartitions, layout);
				}
				// SSE partitions share one shape buffer and each owns a contiguous
				// triangle RANGE of it, in order. Drawing every partition from
				// index 0 skins the first partition's triangles with every other
				// partition's palette: vertices follow the wrong bones, the mesh
				// smears wide and stretched triangles comb the snow. A partition
				// with its own buffer starts at 0; a skipped one still advances.
				RE::BSGraphics::TriShape* rangeBuff = nullptr;
				uint32_t indexStart = 0;
				for (uint32_t p = 0; p < skinPartition->numPartitions; ++p) {
					const auto& part = skinPartition->partitions[p];
					auto* buff = part.buffData;
					if (!buff || !buff->vertexBuffer || !buff->indexBuffer)
						continue;
					if (buff != rangeBuff) {
						rangeBuff = buff;
						indexStart = 0;
					}
					const uint32_t indexCount = uint32_t(part.triangles) * 3;
					const uint32_t thisStart = indexStart;
					indexStart += indexCount;
					if (!part.bones || part.numBones == 0 || part.numBones > kContactMaxBones || indexCount == 0)
						continue;
					if (dismemberParts && p < dismemberCount && !dismemberParts[p].editorVisible) {
						contactHiddenPartsLast++;
						continue;
					}
					if (indexing != 2) {
						bool orphan = false;
						for (uint16_t j = 0; j < part.numBones && !orphan; ++j)
							orphan = !boneResolves(part.bones[j]);
						if (orphan) {
							contactHiddenPartsLast++;
							continue;
						}
					}
					auto partDesc = buff->vertexDesc;
					if (!partDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) ||
						!partDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
						continue;
					uint64_t descKey;
					memcpy(&descKey, &partDesc, sizeof(descKey));
					auto* layout = ContactSkinInputLayoutFor(descKey, partDesc);
					if (!layout)
						continue;
					const UINT stride = uint32_t(descKey & 0xF) * 4;
					if (stride == 0)
						continue;

					if (indexing == 2 && boneCount <= kContactMaxBones) {
						if (!globalUploaded) {
							for (uint32_t b = 0; b < boneCount; ++b) {
								const RE::NiTransform& m = composedFor(uint16_t(b), p, uint16_t(b));
								const auto& rot = m.rotate;
								const float sc = m.scale;
								cb.BoneRows[b * 3 + 0] = { rot.entry[0][0] * sc, rot.entry[0][1] * sc, rot.entry[0][2] * sc, m.translate.x };
								cb.BoneRows[b * 3 + 1] = { rot.entry[1][0] * sc, rot.entry[1][1] * sc, rot.entry[1][2] * sc, m.translate.y };
								cb.BoneRows[b * 3 + 2] = { rot.entry[2][0] * sc, rot.entry[2][1] * sc, rot.entry[2][2] * sc, m.translate.z };
							}
							cb.SkinBoneCount = float(boneCount);
							contactSkinCB->Update(cb);
							globalUploaded = true;
							paletteBones = nullptr;
						}
						contactGlobalPartsLast++;
					} else {
					const bool samePalette = paletteBones && paletteCount == part.numBones &&
					                         std::memcmp(paletteBones, part.bones, size_t(part.numBones) * sizeof(uint16_t)) == 0;
					if (!samePalette) {
						for (uint16_t j = 0; j < part.numBones; ++j) {
							const RE::NiTransform& m = composedFor(part.bones[j], p, j);
							const auto& rot = m.rotate;
							const float sc = m.scale;
							cb.BoneRows[j * 3 + 0] = { rot.entry[0][0] * sc, rot.entry[0][1] * sc, rot.entry[0][2] * sc, m.translate.x };
							cb.BoneRows[j * 3 + 1] = { rot.entry[1][0] * sc, rot.entry[1][1] * sc, rot.entry[1][2] * sc, m.translate.y };
							cb.BoneRows[j * 3 + 2] = { rot.entry[2][0] * sc, rot.entry[2][1] * sc, rot.entry[2][2] * sc, m.translate.z };
						}
						cb.SkinBoneCount = float(part.numBones);
						contactSkinCB->Update(cb);
						paletteBones = part.bones;
						paletteCount = part.numBones;
					}
					}

					UINT offset = 0;
					auto* vb = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer);
					auto* ib = reinterpret_cast<ID3D11Buffer*>(buff->indexBuffer);
					context->IASetInputLayout(layout);
					context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
					context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
					context->DrawIndexed(indexCount, thisStart, 0);
					contactSkinDrawsLast++;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			// The stillness reference: where the body stood when its skin last
			// went into the field. Recorded only on a real draw, so a spawned
			// body whose buffers arrive late is not marked still before it
			// has ever printed.
			if (!actor.still && !actor.corpse && contactSkinDrawsLast > drawsBeforeActor) {
				if (auto it = stampBoneCache.find(ref->GetFormID()); it != stampBoneCache.end()) {
					auto& cache = it->second;
					cache.contactPrev = ref->GetPosition();
					cache.hasContactPrev = true;
					for (auto& foot : cache.feet)
						if (auto* n = foot.node.get()) {
							foot.prev = n->world.translate;
							foot.hasPrev = true;
						}
				}
			}
		}
		globals::profiler->EndPass();
	}

	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	context->RSSetState(savedRaster.get());
}

ID3D11ComputeShader* SnowDeformation::GetHiZBuildCS()
{
	if (!hiZBuildCS) {
		logger::debug("Compiling DepthSyncCS HiZBuildCS");
		hiZBuildCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "HiZBuildCS"));
	}
	return hiZBuildCS;
}

ID3D11ComputeShader* SnowDeformation::GetSkinCullCS()
{
	if (!skinCullCS) {
		logger::debug("Compiling DepthSyncCS SkinCullCS");
		skinCullCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "SkinCullCS"));
	}
	return skinCullCS;
}

bool SnowDeformation::EnsureSkinCullResources(uint32_t a_count, ID3D11ShaderResourceView* a_mainDepthSRV)
{
	auto device = globals::d3d::device;
	if (!skinCullCB)
		skinCullCB = new ConstantBuffer(ConstantBufferDesc<SkinCullCB>(), "SnowDeformation::SkinCullCB");

	// Max-depth pyramid sized from the scene depth: level 0 is half res,
	// the chain runs to 1x1 so a footprint of any size finds its level.
	winrt::com_ptr<ID3D11Resource> depthRes;
	a_mainDepthSRV->GetResource(depthRes.put());
	auto depthTex = depthRes.try_as<ID3D11Texture2D>();
	if (!depthTex)
		return false;
	D3D11_TEXTURE2D_DESC depthDesc{};
	depthTex->GetDesc(&depthDesc);
	const uint32_t w0 = std::max(1u, depthDesc.Width / 2);
	const uint32_t h0 = std::max(1u, depthDesc.Height / 2);
	if (skinCullHiZ && (skinCullHiZ->desc.Width != w0 || skinCullHiZ->desc.Height != h0)) {
		delete skinCullHiZ;
		skinCullHiZ = nullptr;
		for (auto* scratch : skinCullHiZScratch)
			delete scratch;
		skinCullHiZScratch.clear();
		skinCullHiZSRVs.clear();
	}
	if (!skinCullHiZ) {
		uint32_t levels = 1;
		while ((std::max(w0, h0) >> levels) >= 1)
			levels++;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w0;
		desc.Height = h0;
		desc.MipLevels = levels;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		skinCullHiZ = new Texture2D(desc, "SnowDeformation::SkinCullHiZ");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = levels }
		};
		skinCullHiZ->CreateSRV(srvDesc);
		for (uint32_t level = 0; level < levels; level++) {
			D3D11_TEXTURE2D_DESC scratchDesc = desc;
			scratchDesc.Width = std::max(1u, w0 >> level);
			scratchDesc.Height = std::max(1u, h0 >> level);
			scratchDesc.MipLevels = 1;
			scratchDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			auto* scratch = new Texture2D(scratchDesc, "SnowDeformation::SkinCullHiZ scratch");
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			scratch->CreateUAV(uavDesc);
			skinCullHiZScratch.push_back(scratch);
			D3D11_SHADER_RESOURCE_VIEW_DESC levelSrvDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = level, .MipLevels = 1 }
			};
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			if (FAILED(device->CreateShaderResourceView(skinCullHiZ->resource.get(), &levelSrvDesc, srv.put())))
				return false;
			Util::SetResourceName(srv.get(), "SnowDeformation::SkinCullHiZ level SRV");
			skinCullHiZSRVs.push_back(srv);
		}
		skinCullLevels = levels;
		D3D11_TEXTURE2D_DESC topDesc{};
		topDesc.Width = 1;
		topDesc.Height = 1;
		topDesc.MipLevels = 1;
		topDesc.ArraySize = 1;
		topDesc.Format = DXGI_FORMAT_R32_FLOAT;
		topDesc.SampleDesc.Count = 1;
		topDesc.Usage = D3D11_USAGE_STAGING;
		topDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int i = 0; i < kSkinCullRing; i++) {
			skinCullHiZTopStaging[i] = nullptr;
			if (FAILED(device->CreateTexture2D(&topDesc, nullptr, skinCullHiZTopStaging[i].put())))
				return false;
			Util::SetResourceName(skinCullHiZTopStaging[i].get(), "SnowDeformation::SkinCullHiZ top staging");
		}
	}

	// Bounds in, indirect arguments out; grown in steps of 256 skins.
	if (a_count > skinCullCapacity) {
		const uint32_t capacity = (a_count + 255) & ~255u;
		delete skinCullBounds;
		skinCullBounds = nullptr;
		delete skinCullArgs;
		skinCullArgs = nullptr;
		for (int i = 0; i < kSkinCullRing; i++) {
			skinCullArgsStaging[i] = nullptr;
			skinCullStagingIssued[i] = false;
		}

		D3D11_BUFFER_DESC boundsDesc{};
		boundsDesc.Usage = D3D11_USAGE_DYNAMIC;
		boundsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		boundsDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		boundsDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		boundsDesc.StructureByteStride = sizeof(SkinCullBound);
		boundsDesc.ByteWidth = sizeof(SkinCullBound) * capacity;
		D3D11_SHADER_RESOURCE_VIEW_DESC boundsSRV{};
		boundsSRV.Format = DXGI_FORMAT_UNKNOWN;
		boundsSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		boundsSRV.Buffer.FirstElement = 0;
		boundsSRV.Buffer.NumElements = capacity;
		skinCullBounds = new Buffer(boundsDesc, nullptr, "SnowDeformation::SkinCullBounds");
		skinCullBounds->CreateSRV(boundsSRV);

		D3D11_BUFFER_DESC argsDesc{};
		argsDesc.Usage = D3D11_USAGE_DEFAULT;
		argsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		argsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		argsDesc.ByteWidth = kSkinCullArgStride * capacity;
		D3D11_UNORDERED_ACCESS_VIEW_DESC argsUAV{};
		argsUAV.Format = DXGI_FORMAT_R32_TYPELESS;
		argsUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		argsUAV.Buffer.FirstElement = 0;
		argsUAV.Buffer.NumElements = capacity * (kSkinCullArgStride / 4);
		argsUAV.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		skinCullArgs = new Buffer(argsDesc, nullptr, "SnowDeformation::SkinCullArgs");
		skinCullArgs->CreateUAV(argsUAV);

		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = argsDesc.ByteWidth;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (int i = 0; i < kSkinCullRing; i++) {
			if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, skinCullArgsStaging[i].put())))
				return false;
			Util::SetResourceName(skinCullArgsStaging[i].get(), "SnowDeformation::SkinCullArgs staging");
		}
		skinCullCapacity = capacity;
	}
	return skinCullHiZ->srv && skinCullHiZScratch.size() == skinCullLevels && skinCullBounds && skinCullBounds->srv && skinCullArgs && skinCullArgs->uav;
}

void SnowDeformation::CaptureLandTriProbe(RE::BSRenderPass* a_pass)
{
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	// Both the vanilla landscape material and TruePBR's answer
	// kMultiTexLandLODBlend from GetFeature (not kMultiTexLand); 33 is the
	// PBR landscape's own FEATURE id, which TerrainData accepts too.
	auto* material = a_pass->shaderProperty->material;
	if (!material)
		return;
	const auto feature = material->GetFeature();
	if (feature != RE::BSShaderMaterial::Feature::kMultiTexLand &&
		feature != RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend &&
		feature != static_cast<RE::BSShaderMaterial::Feature>(33))
		return;
	if (a_pass->shaderProperty->flags.any(Flag::kLODLandscape))
		return;
	auto* geometry = a_pass->geometry;
	auto triShape = geometry->AsTriShape();
	if (!triShape)
		return;
	auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
		return;
	auto desc = rendererData->vertexDesc;
	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
		return;
	uint64_t descKey;
	memcpy(&descKey, &desc, sizeof(descKey));
	const uint32_t stride = uint32_t(descKey & 0xF) * 4;
	const uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
	const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
	if (stride == 0 || vertexCount == 0 || indexCount == 0)
		return;

	// Position width: the first attribute after it starts where it ends (the
	// same rule EnsureSmoothedNormals uses).
	uint32_t positionBytes = stride;
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

	auto device = globals::d3d::device;
	auto context = globals::d3d::context;
	auto* gameVB = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	auto* gameIB = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	D3D11_BUFFER_DESC vbDesc{};
	gameVB->GetDesc(&vbDesc);
	D3D11_BUFFER_DESC ibDesc{};
	gameIB->GetDesc(&ibDesc);
	auto makeStaging = [&](const D3D11_BUFFER_DESC& a_src, winrt::com_ptr<ID3D11Buffer>& a_out) {
		D3D11_BUFFER_DESC st{};
		st.ByteWidth = a_src.ByteWidth;
		st.Usage = D3D11_USAGE_STAGING;
		st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		a_out = nullptr;
		return SUCCEEDED(device->CreateBuffer(&st, nullptr, a_out.put()));
	};
	if (!makeStaging(vbDesc, landTriProbe.vbStaging) || !makeStaging(ibDesc, landTriProbe.ibStaging)) {
		landTriProbeResult = "probe: staging allocation failed";
		landTriProbeArmed = false;
		return;
	}
	context->CopyResource(landTriProbe.vbStaging.get(), gameVB);
	context->CopyResource(landTriProbe.ibStaging.get(), gameIB);
	landTriProbe.vertexCount = vertexCount;
	landTriProbe.indexCount = indexCount;
	landTriProbe.stride = stride;
	landTriProbe.indexBytes = (ibDesc.ByteWidth >= indexCount * 4) ? 4u : 2u;
	landTriProbe.posFloat32 = positionBytes >= 16;
	landTriProbe.world = geometry->world;
	landTriProbe.name = geometry->name.empty() ? "<unnamed>" : geometry->name.c_str();
	landTriProbe.frame = globals::state->frameCount;
	landTriProbe.pending = true;
	landTriProbeArmed = false;
	landTriProbeResult = "probe: captured, waiting for readback";
}

void SnowDeformation::ServiceLandTriProbe()
{
	if (!landTriProbe.pending || globals::state->frameCount < landTriProbe.frame + 2)
		return;
	auto context = globals::d3d::context;
	D3D11_MAPPED_SUBRESOURCE vb{};
	if (FAILED(context->Map(landTriProbe.vbStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &vb)))
		return;
	D3D11_MAPPED_SUBRESOURCE ib{};
	if (FAILED(context->Map(landTriProbe.ibStaging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ib))) {
		context->Unmap(landTriProbe.vbStaging.get(), 0);
		return;
	}

	const auto& p = landTriProbe;
	const auto* vbytes = static_cast<const uint8_t*>(vb.pData);
	auto localPos = [&](uint32_t v) {
		const uint8_t* base = vbytes + size_t(v) * p.stride;
		if (p.posFloat32) {
			float xyz[3];
			memcpy(xyz, base, sizeof(xyz));
			return DirectX::XMFLOAT3(xyz[0], xyz[1], xyz[2]);
		}
		uint16_t h[3];
		memcpy(h, base, sizeof(h));
		return DirectX::XMFLOAT3(DirectX::PackedVector::XMConvertHalfToFloat(h[0]),
			DirectX::PackedVector::XMConvertHalfToFloat(h[1]),
			DirectX::PackedVector::XMConvertHalfToFloat(h[2]));
	};
	const auto& rot = p.world.rotate;
	const float scale = p.world.scale;
	auto worldXY = [&](const DirectX::XMFLOAT3& l) {
		return DirectX::XMFLOAT2(
			(rot.entry[0][0] * l.x + rot.entry[0][1] * l.y + rot.entry[0][2] * l.z) * scale + p.world.translate.x,
			(rot.entry[1][0] * l.x + rot.entry[1][1] * l.y + rot.entry[1][2] * l.z) * scale + p.world.translate.y);
	};
	auto index = [&](uint32_t i) -> uint32_t {
		if (p.indexBytes == 4)
			return static_cast<const uint32_t*>(ib.pData)[i];
		return static_cast<const uint16_t*>(ib.pData)[i];
	};

	// The mesh's own lattice step: the shortest non-zero axis run among the
	// first triangles' edges (the render mesh is finer than the LAND record).
	const uint32_t triCountAll = p.indexCount / 3;
	float step = 128.0f;
	for (uint32_t t = 0; t < std::min(triCountAll, 64u); t++) {
		DirectX::XMFLOAT2 c[3];
		bool ok = true;
		for (uint32_t k = 0; k < 3; k++) {
			uint32_t vi = index(t * 3 + k);
			if (vi >= p.vertexCount) {
				ok = false;
				break;
			}
			c[k] = worldXY(localPos(vi));
		}
		if (!ok)
			continue;
		for (int e = 0; e < 3; e++) {
			float dx = std::fabs(c[(e + 1) % 3].x - c[e].x), dy = std::fabs(c[(e + 1) % 3].y - c[e].y);
			if (dx > 0.5f)
				step = std::min(step, dx);
			if (dy > 0.5f)
				step = std::min(step, dy);
		}
	}
	step = std::max(1.0f, std::round(step));

	// Per triangle: the longest edge is the quad diagonal; its slope sign says
	// which diagonal. The quad is placed on the world lattice (at the detected
	// step) from its lowest corner, so the pattern is read in world terms.
	const float kLand = step;
	uint32_t slash = 0, backslash = 0, offLattice = 0, degenerate = 0, on64 = 0;
	uint32_t slashEven = 0, slashOdd = 0, backEven = 0, backOdd = 0;
	std::string sample = std::format("  transform: t=({:.1f},{:.1f},{:.1f}) s={:.3f} rot diag=({:.3f},{:.3f},{:.3f}) verts={} stride={}\n",
		p.world.translate.x, p.world.translate.y, p.world.translate.z, scale,
		rot.entry[0][0], rot.entry[1][1], rot.entry[2][2], p.vertexCount, p.stride);
	const uint32_t triCount = triCountAll;
	auto onGrid = [](const DirectX::XMFLOAT2& c, float step) {
		return std::fabs(c.x / step - std::round(c.x / step)) <= 2.0f / step &&
		       std::fabs(c.y / step - std::round(c.y / step)) <= 2.0f / step;
	};
	for (uint32_t t = 0; t < triCount; t++) {
		DirectX::XMFLOAT2 w[3];
		DirectX::XMFLOAT3 l[3];
		bool ok = true;
		for (uint32_t k = 0; k < 3; k++) {
			uint32_t vi = index(t * 3 + k);
			if (vi >= p.vertexCount) {
				ok = false;
				break;
			}
			l[k] = localPos(vi);
			w[k] = worldXY(l[k]);
		}
		if (!ok) {
			degenerate++;
			continue;
		}
		// Raw dump of the first triangles, before any test can drop them.
		if (t < 8)
			sample += std::format("  tri {}: local ({:.1f},{:.1f},{:.1f}) ({:.1f},{:.1f},{:.1f}) ({:.1f},{:.1f},{:.1f}) world ({:.1f},{:.1f}) ({:.1f},{:.1f}) ({:.1f},{:.1f})\n",
				t, l[0].x, l[0].y, l[0].z, l[1].x, l[1].y, l[1].z, l[2].x, l[2].y, l[2].z,
				w[0].x, w[0].y, w[1].x, w[1].y, w[2].x, w[2].y);
		if (onGrid(w[0], 128.0f) && onGrid(w[1], 128.0f) && onGrid(w[2], 128.0f))
			on64++;
		// Lattice check: every corner within 2 units of a 128 multiple.
		bool onLattice = onGrid(w[0], kLand) && onGrid(w[1], kLand) && onGrid(w[2], kLand);
		if (!onLattice) {
			offLattice++;
			continue;
		}
		int best = -1;
		float bestLen = 0.0f;
		float bestDx = 0.0f, bestDy = 0.0f;
		for (int e = 0; e < 3; e++) {
			const auto& a = w[e];
			const auto& b = w[(e + 1) % 3];
			float dx = b.x - a.x, dy = b.y - a.y;
			float len = dx * dx + dy * dy;
			if (len > bestLen) {
				bestLen = len;
				best = e;
				bestDx = dx;
				bestDy = dy;
			}
		}
		if (best < 0 || std::fabs(bestDx) < kLand * 0.5f || std::fabs(bestDy) < kLand * 0.5f) {
			degenerate++;
			continue;
		}
		float minX = std::min({ w[0].x, w[1].x, w[2].x });
		float minY = std::min({ w[0].y, w[1].y, w[2].y });
		const long qx = std::lround(minX / kLand);
		const long qy = std::lround(minY / kLand);
		const bool even = ((qx + qy) & 1) == 0;
		if (bestDx * bestDy > 0.0f) {
			slash++;
			(even ? slashEven : slashOdd)++;
		} else {
			backslash++;
			(even ? backEven : backOdd)++;
		}
		if (t < 8)
			sample += std::format("  tri {} -> {} quad ({},{})\n", t, bestDx * bestDy > 0.0f ? "/" : "\\", qx, qy);
	}
	// Mesh heights against the LAND heightmap the shell bakes from: at heightmap
	// vertices the two must agree; between them the mesh follows whatever
	// interpolation the engine uses, tested here against bilinear, the two
	// triangulations and bicubic Catmull-Rom over the 128 grid.
	std::string fit;
	{
		const std::shared_lock cellLock(shellCellMutex);
		auto landHeight = [&](long gx, long gy, bool& a_ok) -> float {
			const long cellX = static_cast<long>(std::floor(gx / 32.0));
			const long cellY = static_cast<long>(std::floor(gy / 32.0));
			const uint64_t key = (uint64_t(uint32_t(cellX)) << 32) | uint32_t(cellY);
			auto it = shellCells.find(key);
			if (it == shellCells.end()) {
				a_ok = false;
				return 0.0f;
			}
			const long lx = gx - cellX * 32, ly = gy - cellY * 32;
			const float h = it->second.height[size_t(ly) * 33 + size_t(lx)];
			if (h < -50000.0f)
				a_ok = false;
			return h;
		};
		auto catmull = [](float p0, float p1, float p2, float p3, float t) {
			return 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
		};
		double sumSq[4] = {}, maxAbs[4] = {};
		double gridMax = 0.0;
		uint32_t between = 0, onGridN = 0, missing = 0;
		// Catmull-Rom residuals split by where the 4x4 neighbourhood lies: fully
		// inside the vertex's own cell, or reaching a neighbour cell - where the
		// engine may not have had the neighbour and used some edge rule instead.
		// Two candidate edge rules are scored on the edge-reaching points only:
		// clamp (missing neighbour = nearest in-cell height) and mirror (linear
		// extrapolation, p0 = 2 p1 - p2).
		double crInMax = 0.0, crInSq = 0.0, crEdgeMax = 0.0, crEdgeSq = 0.0, clampMax = 0.0, clampSq = 0.0, mirrorMax = 0.0, mirrorSq = 0.0;
		uint32_t nIn = 0, nEdge = 0;
		struct Worst { double d; float fx, fy; float wx, wy; float wz, cr; };
		Worst worst[3] = {};
		for (uint32_t v = 0; v < p.vertexCount; v++) {
			const auto l = localPos(v);
			const auto wxy = worldXY(l);
			const float wz = (rot.entry[2][0] * l.x + rot.entry[2][1] * l.y + rot.entry[2][2] * l.z) * scale + p.world.translate.z;
			const double gxf = wxy.x / 128.0, gyf = wxy.y / 128.0;
			const long gx = static_cast<long>(std::floor(gxf)), gy = static_cast<long>(std::floor(gyf));
			const float fx = float(gxf - gx), fy = float(gyf - gy);
			bool ok = true;
			float h[4][4];
			for (int j = 0; j < 4; j++)
				for (int i = 0; i < 4; i++)
					h[j][i] = landHeight(gx - 1 + i, gy - 1 + j, ok);
			if (!ok) {
				missing++;
				continue;
			}
			const float h00 = h[1][1], h10 = h[1][2], h01 = h[2][1], h11 = h[2][2];
			if (fx < 0.01f && fy < 0.01f) {
				onGridN++;
				gridMax = std::max(gridMax, double(std::fabs(wz - h00)));
				continue;
			}
			between++;
			const float bil = h00 + (h10 - h00) * fx + (h01 - h00) * fy + (h00 - h10 - h01 + h11) * fx * fy;
			const float triA = fy <= fx ? h00 + (h10 - h00) * fx + (h11 - h10) * fy : h00 + (h01 - h00) * fy + (h11 - h01) * fx;
			const float triB = (fx + fy) <= 1.0f ? h00 + (h10 - h00) * fx + (h01 - h00) * fy : h11 + (h10 - h11) * (1.0f - fy) + (h01 - h11) * (1.0f - fx);
			float rows[4];
			for (int j = 0; j < 4; j++)
				rows[j] = catmull(h[j][0], h[j][1], h[j][2], h[j][3], fx);
			const float cr = catmull(rows[0], rows[1], rows[2], rows[3], fy);
			const float pred[4] = { bil, triA, triB, cr };
			for (int m = 0; m < 4; m++) {
				const double d = double(wz) - pred[m];
				sumSq[m] += d * d;
				maxAbs[m] = std::max(maxAbs[m], std::fabs(d));
			}
			// Cell-edge classification of the 4x4 footprint (gx-1..gx+2).
			const long cellX = static_cast<long>(std::floor(gx / 32.0)), cellY = static_cast<long>(std::floor(gy / 32.0));
			const bool reaches = (gx - 1) < cellX * 32 || (gx + 2) > cellX * 32 + 32 || (gy - 1) < cellY * 32 || (gy + 2) > cellY * 32 + 32;
			const double dcr = double(wz) - cr;
			if (!reaches) {
				nIn++;
				crInSq += dcr * dcr;
				crInMax = std::max(crInMax, std::fabs(dcr));
			} else {
				nEdge++;
				crEdgeSq += dcr * dcr;
				crEdgeMax = std::max(crEdgeMax, std::fabs(dcr));
				// Rebuild the neighbourhood under each edge rule.
				float hc[4][4], hm[4][4];
				for (int j = 0; j < 4; j++)
					for (int i = 0; i < 4; i++) {
						long sx = gx - 1 + i, sy = gy - 1 + j;
						const long cx = std::clamp(sx, cellX * 32, cellX * 32 + 32), cy = std::clamp(sy, cellY * 32, cellY * 32 + 32);
						bool ok2 = true;
						hc[j][i] = landHeight(cx, cy, ok2);
						// Mirror: reflect the out-of-cell sample about the edge vertex.
						const long mx = sx < cellX * 32 ? 2 * cellX * 32 - sx : (sx > cellX * 32 + 32 ? 2 * (cellX * 32 + 32) - sx : sx);
						const long my = sy < cellY * 32 ? 2 * cellY * 32 - sy : (sy > cellY * 32 + 32 ? 2 * (cellY * 32 + 32) - sy : sy);
						const float hEdge = landHeight(cx, cy, ok2);
						const float hIn = landHeight(mx, my, ok2);
						hm[j][i] = (sx != mx || sy != my) ? 2.0f * hEdge - hIn : hIn;
					}
				float rc[4], rm[4];
				for (int j = 0; j < 4; j++) {
					rc[j] = catmull(hc[j][0], hc[j][1], hc[j][2], hc[j][3], fx);
					rm[j] = catmull(hm[j][0], hm[j][1], hm[j][2], hm[j][3], fx);
				}
				const double dc = double(wz) - catmull(rc[0], rc[1], rc[2], rc[3], fy);
				const double dm = double(wz) - catmull(rm[0], rm[1], rm[2], rm[3], fy);
				clampSq += dc * dc;
				clampMax = std::max(clampMax, std::fabs(dc));
				mirrorSq += dm * dm;
				mirrorMax = std::max(mirrorMax, std::fabs(dm));
			}
			for (auto& wr : worst) {
				if (std::fabs(dcr) > std::fabs(wr.d)) {
					wr = { dcr, fx, fy, wxy.x, wxy.y, wz, cr };
					break;
				}
			}
		}
		auto rms = [&](int m) { return between ? std::sqrt(sumSq[m] / between) : 0.0; };
		fit = std::format(" | vs LAND: {} on-grid verts max dz {:.2f}; {} between: bilinear max {:.2f} rms {:.2f}, triA max {:.2f} rms {:.2f}, triB max {:.2f} rms {:.2f}, catmull-rom max {:.2f} rms {:.2f}; {} missing",
			onGridN, gridMax, between, maxAbs[0], rms(0), maxAbs[1], rms(1), maxAbs[2], rms(2), maxAbs[3], rms(3), missing);
		fit += std::format(" | CR by footprint: {} in-cell max {:.2f} rms {:.3f}; {} reaching a neighbour cell max {:.2f} rms {:.3f} -> clamp rule max {:.2f} rms {:.3f}, mirror rule max {:.2f} rms {:.3f}",
			nIn, crInMax, nIn ? std::sqrt(crInSq / nIn) : 0.0,
			nEdge, crEdgeMax, nEdge ? std::sqrt(crEdgeSq / nEdge) : 0.0,
			clampMax, nEdge ? std::sqrt(clampSq / nEdge) : 0.0,
			mirrorMax, nEdge ? std::sqrt(mirrorSq / nEdge) : 0.0);
		for (const auto& wr : worst)
			sample += std::format("  worst CR residual {:+.2f} at world ({:.0f},{:.0f}) f=({:.2f},{:.2f}) mesh {:.2f} cr {:.2f}\n", wr.d, wr.wx, wr.wy, wr.fx, wr.fy, wr.wz, wr.cr);

		// One row for offline fitting: mesh heights along the first vertex row
		// and the LAND heights bracketing it.
		if (p.vertexCount >= 17) {
			const auto l0 = localPos(0);
			const auto w0 = worldXY(l0);
			std::string meshRow, landRow;
			for (uint32_t v = 0; v < p.vertexCount; v++) {
				const auto l = localPos(v);
				if (std::fabs(l.y - l0.y) < 0.5f && l.x >= l0.x - 0.5f && l.x < l0.x + 16.5f * step)
					meshRow += std::format("({:.0f}:{:.2f}) ", l.x - l0.x, (rot.entry[2][0] * l.x + rot.entry[2][1] * l.y + rot.entry[2][2] * l.z) * scale + p.world.translate.z);
			}
			const long gx0 = static_cast<long>(std::floor(w0.x / 128.0)), gy0 = static_cast<long>(std::floor(w0.y / 128.0));
			for (long i = -1; i <= 6; i++) {
				bool ok = true;
				const float lh = landHeight(gx0 + i, gy0, ok);
				landRow += ok ? std::format("({:.0f}:{:.2f}) ", float(i) * 128.0f, lh) : std::format("({:.0f}:?) ", float(i) * 128.0f);
			}
			sample += "  row y=" + std::format("{:.0f}", w0.y) + " mesh (dx:z): " + meshRow + "\n  row LAND (dx:z, 128 grid): " + landRow + "\n";
		}
	}

	context->Unmap(landTriProbe.ibStaging.get(), 0);
	context->Unmap(landTriProbe.vbStaging.get(), 0);

	const uint32_t classified = slash + backslash;
	std::string verdict;
	if (classified == 0)
		verdict = "no lattice triangles found (LOD or non-land geometry?)";
	else if (backslash == 0)
		verdict = "UNIFORM '/': every quad splits SW-NE (h00-h11)";
	else if (slash == 0)
		verdict = "UNIFORM '\\': every quad splits NW-SE (h10-h01)";
	else {
		const uint32_t checkerA = slashEven + backOdd;   // '/' on even (qx+qy), '\' on odd
		const uint32_t checkerB = slashOdd + backEven;
		const float fa = float(checkerA) / classified, fb = float(checkerB) / classified;
		if (fa > 0.98f)
			verdict = std::format("CHECKERBOARD: '/' where (qx+qy) even, '\\' where odd ({:.1f}% consistent)", fa * 100.0f);
		else if (fb > 0.98f)
			verdict = std::format("CHECKERBOARD: '\\' where (qx+qy) even, '/' where odd ({:.1f}% consistent)", fb * 100.0f);
		else
			verdict = std::format("MIXED: neither uniform nor a 128-checkerboard (checker fits {:.0f}% / {:.0f}%)", fa * 100.0f, fb * 100.0f);
	}
	landTriProbeResult = std::format("lattice step {:.0f} | {} | {} tris: {} '/', {} '\\', {} off-lattice ({} whole tris on the 128 grid), {} degenerate | '{}' at ({:.0f},{:.0f}) {} pos {}-bit idx{}",
		step, verdict, triCount, slash, backslash, offLattice, on64, degenerate, p.name,
		p.world.translate.x, p.world.translate.y, p.posFloat32 ? "f32" : "f16", p.indexBytes * 8, fit);
	logger::info("[SNOW DEFORMATION] landscape triangulation probe: {}\n{}", landTriProbeResult, sample);
	landTriProbe.pending = false;
	landTriProbe.vbStaging = nullptr;
	landTriProbe.ibStaging = nullptr;
}
