// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

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
		if (!a_geometry || a_geometry->name.empty())
			return false;
		std::string lowered(a_geometry->name.c_str());
		std::transform(lowered.begin(), lowered.end(), lowered.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		return lowered.rfind("ice", 0) == 0 ||
		       lowered.find("glacier") != std::string::npos ||
		       lowered.find("iceberg") != std::string::npos;
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
	void LogIceJourney(RE::BSRenderPass* a_pass, const char* a_outcome)
	{
		auto* geometry = a_pass->geometry;
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		const char* texPath = "";
		if (material) {
			if (auto textureSet = material->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse); path)
					texPath = path;
			}
		}
		if (!IceFamilySignal(geometry, material))
			return;
		static std::unordered_set<uint64_t> logged;
		if (logged.size() > 512)
			return;
		if (!logged.insert((uint64_t)(uintptr_t)geometry ^ ((uint64_t)(uintptr_t)a_outcome << 1)).second)
			return;
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
				}
			}
		}
		return it->second;
	}
}

void SnowDeformation::SetProjectedSnowBit(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass)
{
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
	extraDescriptor &= ~(uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow) |
						 uint32_t(State::ExtraFeatureDescriptors::SnowBakedIsSnow));
	if (!a_shader || !a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	if (!settings.EnableSnowDeformation || !shellSnowDiffuseSRV)
		return;
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	bool bindSnowSet = false;
	// Baked-snow (glacier) match: classified by the ice-family signal alone,
	// independent of technique — glacier snow is baked into the mesh and its
	// draws carry no projected pass (the journey log's proj=0 snow=0 rows).
	if (settings.GlacierSnowMatch &&
		IceFamilySignal(a_pass->geometry, static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material))) {
		extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowBakedIsSnow);
		bindSnowSet = true;
	}
	if (settings.ProjSnowMatch) {
		const bool passProjected = (a_shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) != 0;
		if (!passProjected || a_pass->shaderProperty->flags.all(Flag::kTreeAnim)) {
			statProjNoProjection.fetch_add(1, std::memory_order_relaxed);
		} else if (ClassifyProjectedMato(a_pass->geometry) == MatoClass::kNotSnow) {
			statProjVetoed.fetch_add(1, std::memory_order_relaxed);
		} else {
			statProjMatched.fetch_add(1, std::memory_order_relaxed);
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow);
			bindSnowSet = true;
		}
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
	if (flags.all(Flag::kTreeAnim)) {
		LogIceJourney(a_pass, "rejected: tree-anim flag");
		return;
	}
	// Skinned geometry never qualifies: with the family acceptance no
	// longer LOD-only, an "Ice"-prefixed actor mesh (ice wraith) would
	// otherwise capture and drag a static skin behind a moving creature.
	if (a_pass->geometry->GetGeometryRuntimeData().skinInstance != nullptr) {
		LogIceJourney(a_pass, "rejected: skinned geometry");
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
			bool iceSheet = false;
			if (auto* sheetMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material))
				iceSheet = ClassifySnowPath(sheetMaterial).naturalFeature;
			if (!referenced && !iceSheet) {
				LogIceJourney(a_pass, "rejected: containment (big, camera inside, no owning reference found)");
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

		const SnowPathMatch& pathMatch = ClassifySnowPath(material);
		// Texture family OR mesh-name family: several glacier/ice meshes
		// carry non-family texture paths. The MATO only vetoes (kNotSnow,
		// the sand-shore rocks); requiring positive
		// snow MATOs wrongly rejected glaciers, whose snow is baked and
		// needs no projection record.
		bool naturalFeature = pathMatch.naturalFeature || IsIceFamilyGeometry(a_pass->geometry);
		bool matoVetoed = false;
		if (naturalFeature && ClassifyProjectedMato(a_pass->geometry) == MatoClass::kNotSnow) {
			naturalFeature = false;
			matoVetoed = true;
		}
		// Family accepts at any range, not just LOD: loaded glacier/ice meshes
		// carry no proj/snow flags when PBR glacier textures replace the
		// vanilla projected-snow setup, while their LOD counterparts capture
		// normally. LOD-only acceptance was the whole
		// bare-glacier bug). The MATO veto stands.
		if (!(pathMatch.base || naturalFeature)) {
			if (matoVetoed)
				LogIceJourney(a_pass, "rejected: family matched but MATO vetoed (kNotSnow)");
			else
				LogIceJourney(a_pass, "rejected: no family signal at material gate (name/texture both missed)");
			if (isObjectLOD)
				SampleMaterialReject(a_pass->geometry, material);
			return;
		}
	}

	// One skin per family mesh. Every trishape of an ice pile otherwise
	// captures - slab, ice wall and snow cap - so the tops wear stacked skins,
	// read brighter than the shell, and move independently under the sliders.
	// Loaded family trishapes skin only where their own diffuse is snow (the
	// sculpted caps); slab and ice trishapes go skinless and the baked-snow
	// recolor owns their embedded snow patches. LOD family batches keep their
	// skins.
	if (settings.GlacierSnowMatch && !flags.any(Flag::kLODObjects, Flag::kHDLODObjects)) {
		auto* familyMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		if (IceFamilySignal(a_pass->geometry, familyMaterial)) {
			static std::unordered_map<const void*, bool> snowDiffuseCache;
			if (snowDiffuseCache.size() > 4096)
				snowDiffuseCache.clear();
			auto [sdIt, sdInserted] = snowDiffuseCache.try_emplace(familyMaterial, false);
			if (sdInserted && familyMaterial) {
				if (auto textureSet = familyMaterial->textureSet.get()) {
					if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse); path) {
						std::string lowered(path);
						std::transform(lowered.begin(), lowered.end(), lowered.begin(),
							[](unsigned char c) { return (char)std::tolower(c); });
						sdIt->second = lowered.find("snow") != std::string::npos;
					}
				}
			}
			if (!sdIt->second) {
				LogIceJourney(a_pass, "skipped: family slab/ice trishape (recolor route; snow caps keep the skin)");
				return;
			}
		}
	}

	// Twig-card shape class (branch piles, shore driftwood): vanilla flags
	// them snow-projected so they pass the flag gate, but the capture sees
	// sparse cards and the skin wraps them into broken shards. Name-matched
	// on the diffuse path; extend the list as offenders surface.
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
		if (shardIt->second) {
			LogIceJourney(a_pass, "rejected: twig-card shape class");
			return;
		}
	}

	// Range cap (Object Snow slider): distant mountains are snow-projected
	// everywhere in Skyrim; the skin only matters within the chosen range.
	// Glacier/iceberg captures are exempt: their baked snow is far whiter
	// than the shell and no
	// projection exists for the match to recolor, so between the skin range
	// and cell unload they stood out bright; the skin now covers them at
	// every loaded distance and skips the fade to match.
	auto* captureMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
	const bool fadeExempt = (captureMaterial && ClassifySnowPath(captureMaterial).naturalFeature) ||
	                        IsIceFamilyGeometry(a_pass->geometry);
	const auto& translate = a_pass->geometry->world.translate;
	float dx = translate.x - eye.x;
	float dy = translate.y - eye.y;
	const float captureRange = settings.RangeSkinsM * kUnitsPerMeter;
	if (!fadeExempt && dx * dx + dy * dy > captureRange * captureRange) {
		LogIceJourney(a_pass, "rejected: range cap despite family signal (fadeExempt did not fire)");
		return;
	}

	// The same geometry renders through multiple passes; capture once.
	if (!capturedStaticsSet.insert(a_pass->geometry).second)
		return;
	LogIceJourney(a_pass, fadeExempt ? "CAPTURED (fadeExempt)" : "CAPTURED (range-faded)");

	// Road-mesh model class: deterministic NAME + texture-path match. The
	// name check matters: road models are built from MULTIPLE trishapes
	// ('RoadChunk...:0', ':2'), and only some wear road textures; matching
	// textures alone splits one road across two depth settings, stacking a
	// second hovering shell.
	// `bridge` is tracked apart from `road` for the road heightfield only:
	// both classes share RoadMeshesDepth exactly as before.
	bool road = false;
	bool bridge = false;
	// Which signal decided it, for the road-classification log below.
	const char* roadVia = "no";
	std::string roadTexPath;
	{
		std::string loweredName(a_pass->geometry->name.c_str());
		std::transform(loweredName.begin(), loweredName.end(), loweredName.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		bridge = loweredName.find("bridge") != std::string::npos;
		road = bridge || loweredName.find("road") != std::string::npos;
		if (road)
			roadVia = "name";
	}
	if (!road) {
		if (auto* roadMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material)) {
			// 0 = no match, 1 = road, 2 = bridge.
			static std::unordered_map<const void*, uint8_t> roadMaterialCache;
			if (roadMaterialCache.size() > 4096)
				roadMaterialCache.clear();
			auto [roadIt, roadInserted] = roadMaterialCache.try_emplace(roadMaterial, uint8_t(0));
			if (roadInserted) {
				if (auto textureSet = roadMaterial->textureSet.get()) {
					if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
						std::string lowered(path);
						std::transform(lowered.begin(), lowered.end(), lowered.begin(),
							[](unsigned char c) { return (char)std::tolower(c); });
						if (lowered.find("bridge") != std::string::npos)
							roadIt->second = 2;
						else if (lowered.find("road") != std::string::npos)
							roadIt->second = 1;
						if (roadIt->second != 0)
							roadTexPath = lowered;
					}
				}
			}
			road = roadIt->second != 0;
			bridge = roadIt->second == 2;
			if (road)
				roadVia = "texture";
		}
	}

	// Capture log, one line per unique geometry name: classification plus the
	// diffuse. Began as the road-class log; widened to EVERY capture because
	// the question "was this trishape captured at all, and as what?" keeps
	// being the fork in a diagnosis - RoadChunkS03's ':1' paper sheet is
	// either an uncaptured vanilla snow drape or our own skin on a
	// non-road-named trishape, and only this log can say which.
	{
		static std::unordered_set<std::string> loggedCaptureNames;
		std::string name(a_pass->geometry->name.c_str());
		if (loggedCaptureNames.size() > 4096)
			loggedCaptureNames.clear();
		if (loggedCaptureNames.insert(name).second) {
			const char* diffusePath = "";
			if (auto* logMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material))
				if (auto textureSet = logMaterial->textureSet.get())
					if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
						diffusePath = path;
			logger::info("[SNOW DEFORMATION] captured '{}' road={} (via {}){} heightfield={} tex='{}'",
				name, road ? "yes" : "no", roadVia, bridge ? " (BRIDGE)" : "",
				(road && !bridge && settings.RoadHeightfield) ? "yes" : "no", diffusePath);
		}
	}

	// Vanilla's projected-UV threshold, for the S0 mask view and the S2
	// placement suppressor (SKIN-PLACEMENT-PLAN.md). -1 = no projection data
	// on this draw. Tree-anim meshes are sentineled too: their vertex alpha
	// is wind weight, not a snow mask, and vanilla forces alpha 1 on them.
	float projThreshold = -1.0f;
	float projNoiseScale = 0.0f;
	float projNoiseTiling = 0.0f;
	{
		const auto& capFlags = a_pass->shaderProperty->flags;
		using CapFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (capFlags.any(CapFlag::kProjectedUV) && !capFlags.any(CapFlag::kTreeAnim)) {
			const auto& projParams = static_cast<RE::BSLightingShaderProperty*>(a_pass->shaderProperty)->projectedUVParams;
			projThreshold = projParams.alpha;
			projNoiseScale = projParams.red;
			projNoiseTiling = projParams.blue;
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
	bool forceRounded = false;
	bool plankFamily = false;
	bool driftFamily = false;
	{
		std::string loweredName(a_pass->geometry->name.c_str());
		std::transform(loweredName.begin(), loweredName.end(), loweredName.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		forceRounded = loweredName.find("mountain") != std::string::npos ||
		               loweredName.find("cliff") != std::string::npos;
		if (forceRounded) {
			static std::unordered_set<std::string> loggedRoundedNames;
			if (loggedRoundedNames.size() > 4096)
				loggedRoundedNames.clear();
			if (loggedRoundedNames.insert(loweredName).second)
				logger::info("[SNOW DEFORMATION] forced ROUNDED class (mountain/cliff family): '{}'", loweredName);
		}
		// Plank family: in authored-relief mode the ONLY flat-class draws
		// (cornice treatment, own fill slider); everything else PD is
		// rounded. Same deterministic name match as the road class.
		plankFamily = loweredName.find("plank") != std::string::npos ||
		              loweredName.find("walkway") != std::string::npos ||
		              loweredName.find("catwalk") != std::string::npos;
		// Blob Snow Shell: a drift or snow pile has no bare edge to round.
		driftFamily = loweredName.find("drift") != std::string::npos ||
		              loweredName.find("snowpile") != std::string::npos ||
		              loweredName.find("roadchunk") != std::string::npos;
		// A mesh that already IS snow (drifts, the snow overlays and decals
		// Bethesda lays over roads and drifts) takes no sheet, whatever it is
		// named: the material classifier's landscape-snow match, plus decals.
		if (!driftFamily && captureMaterial && ClassifySnowPath(captureMaterial).base)
			driftFamily = true;
		if (!driftFamily) {
			const auto& exFlags = a_pass->shaderProperty->flags;
			using ExFlag = RE::BSShaderProperty::EShaderPropertyFlag;
			if (exFlags.any(ExFlag::kDecal) || exFlags.any(ExFlag::kDynamicDecal))
				driftFamily = true;
		}
		if (!driftFamily)
			if (auto* driftMaterial = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material))
				if (auto driftTextures = driftMaterial->textureSet.get())
					if (auto driftPath = driftTextures->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
						std::string loweredPath(driftPath);
						std::transform(loweredPath.begin(), loweredPath.end(), loweredPath.begin(),
							[](unsigned char c) { return (char)std::tolower(c); });
						driftFamily = loweredPath.find("drift") != std::string::npos ||
						              loweredPath.find("snowpile") != std::string::npos;
					}
		// (Drift-family special-casing removed: drifts ride the general
		// fully-painted default above, like every technique-classified
		// draw without property-level projection data.)
		if (plankFamily) {
			static std::unordered_set<std::string> loggedPlankNames;
			if (loggedPlankNames.size() > 4096)
				loggedPlankNames.clear();
			if (loggedPlankNames.insert(loweredName).second)
				logger::info("[SNOW DEFORMATION] plank family (flat class in authored relief): '{}'", loweredName);
		}
	}

	capturedStatics.push_back({ RE::NiPointer<RE::BSGeometry>(a_pass->geometry), a_pass->geometry->world, road, bridge, fadeExempt, projThreshold, projNoiseScale, projNoiseTiling, forceRounded, plankFamily, driftFamily });
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
static ID3DBlob* SD_CompileShaderBlob(const wchar_t* a_path, const char* a_target, const char* a_stageDefine, const char* a_extraDefine = nullptr, const char* a_extraDefine2 = nullptr, const char* a_extraDefine3 = nullptr)
{
	// Blob disk cache (see ShaderPrime.cpp): the fixed flag set below is part
	// of the "sdblob" env token, and the full key round-trips through the
	// stored file so a fingerprint or define change reads as a miss.
	auto& snow = globals::features::snowDeformation;
	const auto defs = std::format("{};{};{};{}", a_stageDefine,
		a_extraDefine ? a_extraDefine : "", a_extraDefine2 ? a_extraDefine2 : "", a_extraDefine3 ? a_extraDefine3 : "");
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
	a_scb.ObjectsDepth = settings.ObjectsSnowDepth;
	a_scb.RoundedDepth = settings.ObjectsSnowDepth;
	a_scb.HeightWindowCenter = heightWindowCenter;
	a_scb.HeightHalfExtent = kHeightMapHalfExtent;
	// The march's footprint test (t11 in the visible pass).
	a_scb.HasObjectTop = 1.0f;
	// The REAL setting, not the forced 1.0 this used to carry: the patch VS
	// needs it to tell a road-owned column from a rock that only inherited
	// a carvable depth through the raster's MAX blend. The per-pixel gate
	// that the 1.0 was suppressing is now a compile-time constant in the
	// PATCH pixel shader instead.
	a_scb.ObjectTrenches = settings.ObjectTrenches ? 1.0f : 0.0f;
	// Global gate here, not a per-draw class: the patch is one draw and
	// reads the road bit per texel from the raster's G channel.
	a_scb.RoadField = settings.RoadHeightfield ? 1.0f : 0.0f;
	// Layer 0 unless a per-layer pass overrides it after the fill. The
	// shadow caster shares this recipe and draws the top surface only.
	a_scb.PatchLayer = 0.0f;
	// THE DRAPE PIVOT's A/B. Object columns take the whole lattice surface
	// rather than only the trench around footprints; the skins step aside in
	// the same breath, so the two can never fight for the depth buffer.
	a_scb.ObjectDrape = settings.ObjectDrapeShell ? 1.0f : 0.0f;
	// B0: rides the patch recipe so the caster evaluates the same field.
	a_scb.BlobDrape = settings.BlobObjectSnow ? 1.0f : 0.0f;
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

	// No-depth-export twin. Only the parallax carve needs SV_Depth, and it is
	// gated on ObjectTrenches or the draw being a road, so at default settings
	// every other captured static writes back the depth the rasteriser already
	// had - paying the loss of early-Z across the whole pass for nothing.
	// A compile failure here is not fatal: the draw falls back to staticsPS.
	if (!staticsPSNoDepth) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSNoDepth)))
				Util::SetResourceName(staticsPSNoDepth, "SnowDeformation::StaticsShellPS NoDepth");
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
	if (!meldVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "MELD"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &meldVS)))
				Util::SetResourceName(meldVS, "SnowDeformation::BlobMeldVS");
		}
	}
	if (!meldPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "MELD", ehfDefine, iblDefine));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &meldPS)))
				Util::SetResourceName(meldPS, "SnowDeformation::BlobMeldPS");
		}
	}
	{
		constexpr auto meldPath = L"Data\\Shaders\\SnowDeformation\\BlobMeldCS.hlsl";
		if (!meldDilateCS)
			meldDilateCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(meldPath, {}, "cs_5_0", "MeldDilateCS"));
		if (!meldSmoothCS)
			meldSmoothCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(meldPath, {}, "cs_5_0", "MeldSmoothCS"));
		if (!meldSheetCS)
			meldSheetCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(meldPath, {}, "cs_5_0", "MeldSheetCS"));
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
	if (!heightCoverPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "ps_5_0", "PSHADER", "COVERBOT"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightCoverPS)))
				Util::SetResourceName(heightCoverPS, "SnowDeformation::HeightCoverPS");
		}
	}
	if (!heightPeel2PS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "ps_5_0", "PSHADER", "PEEL2"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightPeel2PS)))
				Util::SetResourceName(heightPeel2PS, "SnowDeformation::HeightPeel2PS");
		}
	}
	if (!skinShadowVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SHADOWCAST"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &skinShadowVS)))
				Util::SetResourceName(skinShadowVS, "SnowDeformation::SkinShadowVS");
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
	if (!blobSeedCS)
		blobSeedCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "BlobSeedCS"));

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
	// S4 phase 2: the peeled layers and their cones (K=3).
	heightTop2Raw[0] = makeHeightTexture("SnowDeformation::HeightTop2Raw0");
	heightTop2Raw[1] = makeHeightTexture("SnowDeformation::HeightTop2Raw1");
	objectSnowCone2 = makeHeightTexture("SnowDeformation::ObjectSnowCone2");
	heightTop3Raw[0] = makeHeightTexture("SnowDeformation::HeightTop3Raw0");
	heightTop3Raw[1] = makeHeightTexture("SnowDeformation::HeightTop3Raw1");
	objectSnowCone3 = makeHeightTexture("SnowDeformation::ObjectSnowCone3");
	objectCoverBottom2 = makeHeightTexture("SnowDeformation::ObjectCoverBottom2");
	objectCoverBottom3 = makeHeightTexture("SnowDeformation::ObjectCoverBottom3");
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

	// ---- Blob Snow Shell (Spike 1) ----
	{
		// Per-layer placement masks, R = mask, G = this frame's fragment height
		// (camera-relative, so a sphere never sits on a decayed ghost). Cleared
		// each frame like the skin depth.
		D3D11_TEXTURE2D_DESC blobMaskDesc = heightDesc;
		blobMaskDesc.Format = DXGI_FORMAT_R16G16_UNORM;
		blobMaskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SHADER_RESOURCE_VIEW_DESC blobMaskSrvDesc = heightSrvDesc;
		blobMaskSrvDesc.Format = blobMaskDesc.Format;
		D3D11_RENDER_TARGET_VIEW_DESC blobMaskRtvDesc = heightRtvDesc;
		blobMaskRtvDesc.Format = blobMaskDesc.Format;
		const char* blobMaskNames[6] = { "SnowDeformation::BlobMask1", "SnowDeformation::BlobMask2", "SnowDeformation::BlobMask3",
			"SnowDeformation::BlobMask4", "SnowDeformation::BlobMask5", "SnowDeformation::BlobMask6" };
		for (int i = 0; i < 6; i++) {
			blobMask[i] = new Texture2D(blobMaskDesc, blobMaskNames[i]);
			blobMask[i]->CreateSRV(blobMaskSrvDesc);
			blobMask[i]->CreateRTV(blobMaskRtvDesc);
		}
		// Layers 4-6: same shape as the peeled tops, rebuilt per frame.
		blobTop[0] = makeHeightTexture("SnowDeformation::BlobTop4");
		blobTop[1] = makeHeightTexture("SnowDeformation::BlobTop5");
		blobTop[2] = makeHeightTexture("SnowDeformation::BlobTop6");
	}
}



void SnowDeformation::EnsureMeldResources(uint32_t a_width, uint32_t a_height)
{
	if (blobMeldDepth[0] && blobMeldDepth[1] && blobMeldW == a_width && blobMeldH == a_height)
		return;
	for (auto& t : blobMeldDepth) {
		delete t;
		t = nullptr;
	}
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_width;
	desc.Height = a_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32_FLOAT;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	rtvDesc.Format = desc.Format;
	rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = desc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	const char* names[2] = { "SnowDeformation::BlobMeldDepth0", "SnowDeformation::BlobMeldDepth1" };
	for (int i = 0; i < 2; i++) {
		blobMeldDepth[i] = new Texture2D(desc, names[i]);
		blobMeldDepth[i]->CreateSRV(srvDesc);
		blobMeldDepth[i]->CreateRTV(rtvDesc);
		blobMeldDepth[i]->CreateUAV(uavDesc);
	}
	blobMeldW = a_width;
	blobMeldH = a_height;
}


void SnowDeformation::DrawBlobShell()
{
	if (!settings.EnableBlobShell || !blobSeedCS || !meldVS || !meldPS || !meldDilateCS || !meldSmoothCS || !meldSheetCS || !blobCB || !meldCB || !meldSeedCB)
		return;
	if (!heightTopRaw[heightCurrent] || !heightTop2Raw[heightCurrent] || !heightTop3Raw[heightCurrent] || !heightProcessCB)
		return;
	for (int i = 0; i < 6; i++)
		if (!blobMask[i] || (i < 3 && !blobTop[i]))
			return;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	const auto& fb = globals::game::frameBufferCached;
	auto& mainRT = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	if (!mainRT.texture)
		return;
	D3D11_TEXTURE2D_DESC mainDesc{};
	mainRT.texture->GetDesc(&mainDesc);
	EnsureMeldResources(mainDesc.Width, mainDesc.Height);
	if (!blobMeldDepth[0] || !blobMeldDepth[1])
		return;
	const float4 dynRes = fb.GetDynamicResolutionParams1();
	const uint32_t dw = std::max(1u, uint32_t(float(mainDesc.Width) * std::clamp(dynRes.x, 0.05f, 1.0f)));
	const uint32_t dh = std::max(1u, uint32_t(float(mainDesc.Height) * std::clamp(dynRes.y, 0.05f, 1.0f)));

	// The game's output state, restored for the composite and afterwards.
	ID3D11RenderTargetView* prevRTVs[8] = {};
	ID3D11DepthStencilView* prevDSV = nullptr;
	context->OMGetRenderTargets(8, prevRTVs, &prevDSV);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	auto& sceneDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	ID3D11ShaderResourceView* sceneDepthSRV = sceneDepth.depthSRV;

	globals::profiler->BeginPass("SnowDeformation::ScreenSpaceSnow");
	// Pass 1: seed from the scene depth through the layer masks.
	{
		BlobCB cb{};
		cb.NoiseScale = std::clamp(settings.BlobSpacing, 1.0f, 512.0f);
		cb.Thickness = std::clamp(settings.BlobSize, 0.25f, 64.0f);
		cb.ThicknessNoise = std::clamp(settings.BlobSizeNoise, 0.0f, 1.0f);
		cb.MaskThreshold = std::clamp(settings.BlobMaskThreshold, 0.0f, 1.0f);
		cb.Layers = float(std::clamp(settings.BlobLayers, 1, 6));
		cb.Radius = std::clamp(settings.BlobRadius, 64.0f, kHeightMapHalfExtent - 8.0f);
		cb.RefZ = blobRefZ;
		// The fitted shell's pixels sit up to the class depth above the raster
		// top; letting them match folds the shell into the field, so sheet and
		// shell roll into one surface.
		// Soft match (half weight at half this gap), wide enough for the shell's
		// lift on top of the raster top plus a margin the jitter cannot cross.
		cb.LayerTol = 16.0f + std::max(0.0f, std::max(settings.ObjectsSnowDepth, settings.RoadMeshesDepth));
		cb.MaxSlopeNz = std::cos(std::clamp(settings.BlobMaxSlopeDeg, 0.0f, 90.0f) * 3.14159265f / 180.0f);
		cb.RockMaxSlopeNz = std::cos(std::clamp(settings.BlobRockMaxSlopeDeg, 0.0f, 90.0f) * 3.14159265f / 180.0f);
		cb.BorderNoise = std::clamp(settings.BlobBorderNoise, 0.0f, 1.0f);
		blobCB->Update(cb);
		MeldSeedCB sc{};
		sc.ProjInverse = fb.GetCameraProjInverse();
		sc.ViewInverse = fb.GetCameraViewInverse();
		sc.CamPosAdjust = fb.GetCameraPosAdjust();
		sc.Dims = { float(dw), float(dh) };
		meldSeedCB->Update(sc);
		ID3D11Buffer* cbs[3] = { heightProcessCB->CB(), blobCB->CB(), meldSeedCB->CB() };
		context->CSSetConstantBuffers(0, 3, cbs);
		ID3D11ShaderResourceView* srvs[14] = {
			heightTopRaw[heightCurrent]->srv.get(),
			heightTop2Raw[heightCurrent]->srv.get(),
			nullptr,
			heightTop3Raw[heightCurrent]->srv.get(),
			blobMask[0]->srv.get(),
			blobMask[1]->srv.get(),
			blobMask[2]->srv.get(),
			blobTop[0]->srv.get(),
			blobTop[1]->srv.get(),
			blobTop[2]->srv.get(),
			blobMask[3]->srv.get(),
			blobMask[4]->srv.get(),
			blobMask[5]->srv.get(),
			sceneDepthSRV
		};
		context->CSSetShaderResources(0, 14, srvs);
		ID3D11UnorderedAccessView* seedUAV = blobMeldDepth[0]->uav.get();
		context->CSSetUnorderedAccessViews(3, 1, &seedUAV, nullptr);
		context->CSSetShader(blobSeedCS, nullptr, 0);
		context->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetUnorderedAccessViews(3, 1, &nullUAV, nullptr);
		ID3D11ShaderResourceView* nullSRVs[14] = {};
		context->CSSetShaderResources(0, 14, nullSRVs);
		ID3D11Buffer* nullCBs[3] = {};
		context->CSSetConstantBuffers(0, 3, nullCBs);
	}

	// Passes 2-4: dilate by each seed's thickness (H, V), smooth, sheet test.
	MeldCB m{};
	m.Proj = fb.GetCameraProj();
	m.ProjInverse = fb.GetCameraProjInverse();
	m.ViewInverse = fb.GetCameraViewInverse();
	m.Dims = { float(dw), float(dh) };
	m.DepthRange = std::clamp(settings.BlobMeldDepthRange, 0.5f, 256.0f);
	m.SmoothRange = std::clamp(settings.BlobMeldSmoothRange, 0.5f, 256.0f);
	m.MaxRadiusPx = float(std::clamp(settings.BlobMeldMaxRadiusPx, 1, 64));
	m.Debug = float(std::clamp(settings.BlobMeldDebug, 0, 2));
	m.Smoothing = std::clamp(settings.BlobMeldSmoothing, 0.0f, 32.0f);
	m.Anchor = 0.0f;
	m.FootBias = 0.1f;
	m.VerticalRange = std::clamp(settings.BlobMeldVerticalRange, 0.0f, 256.0f);
	int src = 0;
	auto fieldPass = [&](ID3D11ComputeShader* a_cs, float a_dirX, float a_dirY, float a_seed) {
		m.Dir = { a_dirX, a_dirY };
		m.Seed = a_seed;
		meldCB->Update(m);
		ID3D11Buffer* mcb = meldCB->CB();
		context->CSSetConstantBuffers(0, 1, &mcb);
		ID3D11ShaderResourceView* ins[2] = { blobMeldDepth[src]->srv.get(), sceneDepthSRV };
		ID3D11UnorderedAccessView* out = blobMeldDepth[1 - src]->uav.get();
		context->CSSetShaderResources(0, 2, ins);
		context->CSSetUnorderedAccessViews(0, 1, &out, nullptr);
		context->CSSetShader(a_cs, nullptr, 0);
		context->Dispatch((dw + 7) / 8, (dh + 7) / 8, 1);
		ID3D11ShaderResourceView* nullIns[2] = { nullptr, nullptr };
		ID3D11UnorderedAccessView* nullOut = nullptr;
		context->CSSetShaderResources(0, 2, nullIns);
		context->CSSetUnorderedAccessViews(0, 1, &nullOut, nullptr);
		src = 1 - src;
	};
	fieldPass(meldDilateCS, 1.0f, 0.0f, 1.0f);
	fieldPass(meldDilateCS, 0.0f, 1.0f, 0.0f);
	if (m.Smoothing > 0.0f) {
		const int iterations = std::clamp(settings.BlobMeldIterations, 1, 4);
		for (int it = 0; it < iterations; it++) {
			fieldPass(meldSmoothCS, 1.0f, 0.0f, 0.0f);
			fieldPass(meldSmoothCS, 0.0f, 1.0f, 0.0f);
		}
	}
	fieldPass(meldSheetCS, 1.0f, 0.0f, 0.0f);
	ID3D11Buffer* nullCsCB = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCsCB);
	context->CSSetShader(nullptr, nullptr, 0);

	// Composite as one surface into the game's targets, depth written.
	context->OMSetRenderTargets(8, prevRTVs, prevDSV);
	context->VSSetShader(meldVS, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(meldPS, nullptr, 0);
	ID3D11Buffer* cb0 = shellCB->CB();
	context->VSSetConstantBuffers(0, 1, &cb0);
	context->PSSetConstantBuffers(0, 1, &cb0);
	m.Dir = { 0.0f, 0.0f };
	meldCB->Update(m);
	ID3D11Buffer* mcb2 = meldCB->CB();
	context->PSSetConstantBuffers(2, 1, &mcb2);
	ID3D11ShaderResourceView* meldSRV = blobMeldDepth[src]->srv.get();
	context->PSSetShaderResources(33, 1, &meldSRV);
	context->IASetInputLayout(nullptr);
	ID3D11Buffer* nullVB = nullptr;
	const UINT zero = 0;
	context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->Draw(3, 0);
	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(33, 1, &nullSRV);
	ID3D11Buffer* nullCB2 = nullptr;
	context->PSSetConstantBuffers(2, 1, &nullCB2);
	globals::profiler->EndPass();

	for (auto* rtv : prevRTVs)
		if (rtv)
			rtv->Release();
	if (prevDSV)
		prevDSV->Release();
}

void SnowDeformation::RenderObjectHeightMap()
{
	LoadTraceScope _loadTrace(this, "Statics: RenderObjectHeightMap");
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
	processData.RimStep = std::clamp(settings.PlaneSplitStep, 1.0f, 32.0f);
	processData.OverheadIgnore = std::clamp(settings.OverheadClearance, 0.0f, 200.0f);
	processData.MeldPlanes = settings.MeldCoPlanar ? 1.0f : 0.0f;
	// P4: lambda 0..0.5 - the stability bound for the 4-neighbour Jacobi.
	processData.DiffuseLambda = std::clamp(settings.SnowSettlingPct, 0.0f, 100.0f) * 0.005f;
	heightProcessCB->Update(processData);
	heightWindowCenter = newCenter;
	blobRefZ = eye.z;
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

	// S4 phase 2: scroll the peeled layer tops the same way. ScrollCS's
	// bottom slot reads last frame's bottoms (harmless) and writes into
	// heightScratch as a throwaway (the scratch is fully overwritten by
	// the cone chains below), so the CS runs unmodified.
	{
		Texture2D* peelPrev[2] = { heightTop2Raw[previous], heightTop3Raw[previous] };
		Texture2D* peelCur[2] = { heightTop2Raw[heightCurrent], heightTop3Raw[heightCurrent] };
		for (int peelLayer = 0; peelLayer < 2; peelLayer++) {
			if (!peelPrev[peelLayer] || !peelCur[peelLayer])
				continue;
			ID3D11ShaderResourceView* scroll2SRVs[2] = { peelPrev[peelLayer]->srv.get(), heightBottomRaw[previous]->srv.get() };
			ID3D11UnorderedAccessView* scroll2UAVs[2] = { peelCur[peelLayer]->uav.get(), heightScratch->uav.get() };
			context->CSSetShaderResources(0, 2, scroll2SRVs);
			context->CSSetUnorderedAccessViews(0, 2, scroll2UAVs, nullptr);
			context->CSSetShader(heightScrollCS, nullptr, 0);
			context->Dispatch((kHeightMapDim + 7) / 8, (kHeightMapDim + 7) / 8, 1);
			context->CSSetShaderResources(0, 2, nullCsSRVs);
			context->CSSetUnorderedAccessViews(0, 2, nullCsUAVs, nullptr);
		}
	}
	ID3D11Buffer* nullProcessCB = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullProcessCB);
	context->CSSetShader(nullptr, nullptr, 0);

	// Rasterize this frame's captures on top of the scrolled maps.
	// G = road top, so it clears to the no-road sentinel, not to zero: zero is
	// a legal world Z and would read as a road at sea level.
	const float skinDepthClear[4] = { 0.0f, kNoRoadTop, 0.0f, 0.0f };
	context->ClearRenderTargetView(heightSkinDepth->rtv.get(), skinDepthClear);
	// Blob Snow Shell: the per-layer placement masks are per-frame, like the
	// skin depth. Layer 1 rides the base capture as RT3; the peels below
	// bind their own layer's mask in the same slot.
	const float maskClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	for (int i = 0; i < 6; i++)
		if (blobMask[i])
			context->ClearRenderTargetView(blobMask[i]->rtv.get(), maskClear);
	ID3D11RenderTargetView* heightRTVs[4] = { heightTopRaw[heightCurrent]->rtv.get(), heightBottomRaw[heightCurrent]->rtv.get(), heightSkinDepth->rtv.get(),
		blobMask[0] ? blobMask[0]->rtv.get() : nullptr };
	context->OMSetRenderTargets(4, heightRTVs, nullptr);
	context->OMSetBlendState(heightMaxBlendState.get(), nullptr, 0xFFFFFFFF);

	D3D11_VIEWPORT heightViewport{ 0.0f, 0.0f, float(kHeightMapDim), float(kHeightMapDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &heightViewport);

	context->VSSetShader(heightVS, nullptr, 0);
	context->PSSetShader(heightPS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	// The PS reads BlobRefZ (fresh-top channel of the blob mask) from b1 too;
	// unbound here it read 0 and every layer-1 height clamped out of range.
	context->PSSetConstantBuffers(1, 1, &cb1);
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
		scb.RoundedDepth = cap.road ? settings.RoadMeshesDepth : settings.ObjectsSnowDepth;
		scb.VertexCountF = float(triShape->GetTrishapeRuntimeData().vertexCount);
		scb.HeightWindowCenter = heightWindowCenter;
		scb.HeightHalfExtent = kHeightMapHalfExtent;
		// The raster VS zeroes skin depth for objects that may not carve, so
		// both gates have to reach this pass; without them every object reads
		// as non-carving and the trench patch dies everywhere, roads included.
		scb.LegacySkin = cap.road ? 1.0f : 0.0f;
		// Lifts the depth park for non-road objects, so a rock carries its own
		// class depth instead of borrowing the road's through the MAX blend.
		scb.ObjectDrape = settings.ObjectDrapeShell ? 1.0f : 0.0f;
		scb.FadeExempt = cap.fadeExempt ? 1.0f : 0.0f;
		scb.ObjectTrenches = settings.ObjectTrenches ? 1.0f : 0.0f;
		scb.RoadField = (settings.RoadHeightfield && cap.road && !cap.bridge) ? 1.0f : 0.0f;
		scb.ProjThreshold = cap.projThreshold;
		scb.ProjMaskEnable = settings.ProjMaskPlacement ? 1.0f : 0.0f;
		// Blob Snow Shell mask inputs: no spheres on roads, bridges or drifts;
		// the skin's own repose gate; the fresh-top reference.
		scb.BlobExclude = (cap.road || cap.bridge || cap.driftFamily) ? 1.0f : 0.0f;
		scb.BlobRefZ = blobRefZ;
		scb.BlobRockClass = cap.forceRounded ? 1.0f : 0.0f;
		{
			const float blobSlopeDeg = cap.forceRounded ? settings.RockMaxSlopeDeg : settings.ShellMaxSlopeDeg;
			scb.ShellMinNz = std::cos(std::clamp(blobSlopeDeg, 0.0f, 90.0f) * 3.14159265f / 180.0f);
		}
		scb.ProjDensityEnable = settings.ProjDepthDensity ? 1.0f : 0.0f;
		scb.ProjSnowFillSk = std::clamp(settings.ProjSnowFillPct / 100.0f, 0.0f, 1.0f);
		// Same class pick as the skin: S4 shell draws are all ROUNDED.
		{
			const bool s4Shell = settings.ObjectSnow3D && !settings.ObjectDrapeShell && !cap.road &&
			                     cap.projThreshold > -0.5f && SD_ProjNoiseMapSRV();
			scb.ClassOverride = (s4Shell || cap.forceRounded) ? 1.0f : 0.0f;
		}
		// Flat/rounded stats for the skin-depth output (RT2): the raster VS
		// reads the same classification the skin uses.
		ID3D11ShaderResourceView* rasterSmoothSRV = EnsureSmoothedNormals(geometry);
		context->VSSetShaderResources(10, 1, &rasterSmoothSRV);
		scb.HasSmoothedNormals = rasterSmoothSRV ? 1.0f : 0.0f;
		staticsCB->Update(scb);

		context->DrawIndexed(indexCount, 0, 0);
	}
	globals::profiler->EndPass();

	ID3D11RenderTargetView* nullRTVs[4] = { nullptr, nullptr, nullptr, nullptr };
	context->OMSetRenderTargets(4, nullRTVs, nullptr);

	// S4 phase 2 - the layer PEELS (K=3): re-rasterize the captures
	// against the completed layers above (now readable), keeping only
	// up-facing fragments below them by the peel tolerance; MAX blending
	// yields the next-highest snow-bearing surface per column. Each pass
	// needs the previous one finished, so they run sequentially. Only the
	// transform and the window fields matter here.
	//
	// THE AIR TEST's passes ride the same loop (Josef's distinction, 2026-08-31):
	// after each peeled top exists, a COVERBOT pass MIN-blends the height of
	// everything standing above it, which is what separates a roof over a
	// walkway (open space beneath) from a wall standing on a road (solid to the
	// ground). They must come after their own layer's peel, and they cost a
	// full re-rasterization each, so they run ONLY while the drape is on.
	const int coverPasses = (settings.LayeredObjectDrape && heightCoverPS &&
								objectCoverBottom2 && objectCoverBottom3) ?
	                            2 :
	                            0;
	// Blob Snow Shell layers 4-6: three more peels of the same shape (PEEL2
	// against the previous layer), rebuilt per frame, only while asked for.
	Texture2D* layerTops[6] = { heightTopRaw[heightCurrent], heightTop2Raw[heightCurrent], heightTop3Raw[heightCurrent],
		blobTop[0], blobTop[1], blobTop[2] };
	const int extraPeels = (settings.EnableBlobShell && heightPeel2PS && blobTop[0] && blobTop[1] && blobTop[2]) ?
	                           std::max(0, std::clamp(settings.BlobLayers, 1, 6) - 3) :
	                           0;
	const int peelPasses = 2 + extraPeels;
	static const char* const peelPassNames[5] = { "SnowDeformation::ObjectHeightPeel", "SnowDeformation::ObjectHeightPeel2",
		"SnowDeformation::ObjectHeightPeel4", "SnowDeformation::ObjectHeightPeel5", "SnowDeformation::ObjectHeightPeel6" };
	for (int pass = 0; pass < peelPasses + coverPasses; pass++) {
		const bool coverPass = pass >= peelPasses;
		const int peelLayer = coverPass ? pass - peelPasses : pass;
		ID3D11PixelShader* peelPS = coverPass ? heightCoverPS :
		                                        (peelLayer == 0 ? heightPeelPS : heightPeel2PS);
		Texture2D* peelTarget = coverPass ?
		                            (peelLayer == 0 ? objectCoverBottom2 : objectCoverBottom3) :
		                            layerTops[peelLayer + 1];
		if (!peelPS || !peelTarget)
			break;
		if (!coverPass && peelLayer >= 2) {
			// Not scrolled like layers 2 and 3: starts empty every frame.
			const float emptyTop[4] = { -100000.0f, -100000.0f, -100000.0f, -100000.0f };
			context->ClearRenderTargetView(peelTarget->rtv.get(), emptyTop);
		}
		if (coverPass) {
			// The MIN op lives on RT1 in the capture's blend state, so bind the
			// target THERE with RT0 null and let the existing state supply it
			// rather than authoring a second blend state that could drift.
			// Cleared to the empty sentinel: no cover at all reads as open sky.
			const float openSky[4] = { kHeightMapEmptyBottom, kHeightMapEmptyBottom,
				kHeightMapEmptyBottom, kHeightMapEmptyBottom };
			context->ClearRenderTargetView(peelTarget->rtv.get(), openSky);
			ID3D11RenderTargetView* coverRTVs[2] = { nullptr, peelTarget->rtv.get() };
			context->OMSetRenderTargets(2, coverRTVs, nullptr);
		} else {
			// RT3 = this layer's blob placement mask (layer 2 for the first
			// peel, layer 3 for the second); the cover passes leave it alone.
			ID3D11RenderTargetView* peelRTVs[4] = { peelTarget->rtv.get(), nullptr, nullptr,
				blobMask[peelLayer + 1] ? blobMask[peelLayer + 1]->rtv.get() : nullptr };
			context->OMSetRenderTargets(4, peelRTVs, nullptr);
		}
		context->PSSetShader(peelPS, nullptr, 0);
		// Peel: t3 = layer 1, and the layer-3 pass adds t4 = the finished layer
		// 2 (never bound while it is still the pass's own render target).
		// Cover: t3 = the layer this pass measures cover FOR.
		ID3D11ShaderResourceView* peelSRVs[2] = {
			coverPass ?
				(peelLayer == 0 ? heightTop2Raw[heightCurrent]->srv.get() : heightTop3Raw[heightCurrent]->srv.get()) :
				heightTopRaw[heightCurrent]->srv.get(),
			(!coverPass && peelLayer >= 1) ? layerTops[peelLayer]->srv.get() : nullptr
		};
		context->PSSetShaderResources(3, 2, peelSRVs);
		// The peel PS addresses the layer maps through StaticCB's window
		// fields; the capture pass binds b1 to the VS only.
		context->PSSetConstantBuffers(1, 1, &cb1);

		globals::profiler->BeginPass(coverPass ?
				(peelLayer == 0 ? "SnowDeformation::ObjectCoverBottom2" : "SnowDeformation::ObjectCoverBottom3") :
				peelPassNames[peelLayer]);
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

			StaticsCB scb{};
			const auto& rot = cap.world.rotate;
			const float scale = cap.world.scale;
			scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, cap.world.translate.x };
			scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, cap.world.translate.y };
			scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, cap.world.translate.z };
			scb.HeightWindowCenter = heightWindowCenter;
			scb.HeightHalfExtent = kHeightMapHalfExtent;
			scb.PeelTol = std::clamp(settings.PlaneMergeHeight, 1.0f, 32.0f);
			// Blob placement mask inputs (the base pass sets the same three).
			scb.ProjThreshold = cap.projThreshold;
			scb.ProjSnowFillSk = std::clamp(settings.ProjSnowFillPct / 100.0f, 0.0f, 1.0f);
			scb.BlobExclude = (cap.road || cap.bridge || cap.driftFamily) ? 1.0f : 0.0f;
			scb.BlobRefZ = blobRefZ;
			scb.BlobRockClass = cap.forceRounded ? 1.0f : 0.0f;
			{
				const float blobSlopeDeg = cap.forceRounded ? settings.RockMaxSlopeDeg : settings.ShellMaxSlopeDeg;
				scb.ShellMinNz = std::cos(std::clamp(blobSlopeDeg, 0.0f, 90.0f) * 3.14159265f / 180.0f);
			}
			scb.VertexCountF = float(triShape->GetTrishapeRuntimeData().vertexCount);
			ID3D11ShaderResourceView* peelSmoothSRV = EnsureSmoothedNormals(geometry);
			context->VSSetShaderResources(10, 1, &peelSmoothSRV);
			scb.HasSmoothedNormals = peelSmoothSRV ? 1.0f : 0.0f;
			staticsCB->Update(scb);
			context->DrawIndexed(indexCount, 0, 0);
		}
		globals::profiler->EndPass();

		context->OMSetRenderTargets(4, nullRTVs, nullptr);
		ID3D11ShaderResourceView* nullPeelSRVs[2] = { nullptr, nullptr };
		context->PSSetShaderResources(3, 2, nullPeelSRVs);
	}

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
		// One shared field for both classes: seed it with the deeper of the
		// two and let each class normalize against it (see SkinLift.RimT).
		processData.ObjectSnowDepth = std::max(settings.ObjectsSnowDepth, 0.1f);
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
			if (!objectConeDiffuseCS || settings.SnowSettlingPct <= 0.5f)
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

		// S4 phase 2: the same seed + repose chain over each PEELED layer
		// top, so every below-top plane gets its own rims and distances.
		Texture2D* peelTops[2] = { heightTop2Raw[heightCurrent], heightTop3Raw[heightCurrent] };
		Texture2D* peelCones[2] = { objectSnowCone2, objectSnowCone3 };
		for (int peelLayer = 0; peelLayer < 2; peelLayer++) {
			if (!peelCones[peelLayer] || !peelTops[peelLayer])
				continue;
			context->CSSetShader(objectConeSeedCS, nullptr, 0);
			ID3D11ShaderResourceView* seed2SRVs[2] = { peelTops[peelLayer]->srv.get(),
				heightSkinDepth ? heightSkinDepth->srv.get() : nullptr };
			// The continuation test's "next layer": L3 for the L2 chain;
			// the L3 chain has nothing deeper and reads itself (its own
			// neighbour value never matches a tall riser, so tall cover
			// over an L3 sliver rims - the safe default).
			ID3D11ShaderResourceView* seed2NextSRV = heightTop3Raw[heightCurrent] ? heightTop3Raw[heightCurrent]->srv.get() : nullptr;
			ID3D11UnorderedAccessView* seed2UAV = peelCones[peelLayer]->uav.get();
			context->CSSetShaderResources(0, 2, seed2SRVs);
			context->CSSetShaderResources(3, 1, &seed2NextSRV);
			context->CSSetUnorderedAccessViews(0, 1, &seed2UAV, nullptr);
			context->Dispatch(dispatchDim, dispatchDim, 1);
			ID3D11ShaderResourceView* nullSeed2SRVs[2] = { nullptr, nullptr };
			context->CSSetShaderResources(0, 2, nullSeed2SRVs);
			context->CSSetShaderResources(3, 1, nullCsSRVs);
			context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);

			context->CSSetShader(objectConeCS, nullptr, 0);
			Texture2D* obj2In = peelCones[peelLayer];
			Texture2D* obj2Out = heightScratch;
			for (uint step : kConeSteps) {
				processData.ConeStep = step;
				heightProcessCB->Update(processData);
				ID3D11ShaderResourceView* obj2SRV = obj2In->srv.get();
				ID3D11UnorderedAccessView* obj2UAV = obj2Out->uav.get();
				context->CSSetShaderResources(0, 1, &obj2SRV);
				context->CSSetUnorderedAccessViews(0, 1, &obj2UAV, nullptr);
				context->Dispatch(dispatchDim, dispatchDim, 1);
				context->CSSetShaderResources(0, 1, nullCsSRVs);
				context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
				std::swap(obj2In, obj2Out);
			}
			settleCone(obj2In, obj2Out);
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
			float u = (pos.x - heightWindowCenter.x) / kHeightMapHalfExtent * 0.5f + 0.5f;
			float v = 0.5f - (pos.y - heightWindowCenter.y) / kHeightMapHalfExtent * 0.5f;
			uint tx = uint(std::clamp(int(u * kHeightMapDim), 0, int(kHeightMapDim) - 1));
			uint ty = uint(std::clamp(int(v * kHeightMapDim), 0, int(kHeightMapDim) - 1));
			Texture2D* probeMaps[6] = { heightTopRaw[heightCurrent], heightTop2Raw[heightCurrent], heightTop3Raw[heightCurrent],
				objectSnowCone, objectSnowCone2, objectSnowCone3 };
			D3D11_BOX probeBox{ tx, ty, 0, tx + 1, ty + 1, 1 };
			for (uint i = 0; i < 6; i++)
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
	a_scb.ObjectsDepth = a_cap.road ? settings.RoadMeshesDepth : settings.ObjectsSnowDepth;
	a_scb.RoundedDepth = a_cap.road ? settings.RoadMeshesDepth : settings.ObjectsSnowDepth;
	a_scb.VertexCountF = a_vertexCount;
	a_scb.HeightWindowCenter = heightWindowCenter;
	a_scb.HeightHalfExtent = kHeightMapHalfExtent;
	a_scb.HasSmoothedNormals = a_hasSmoothedNormals ? 1.0f : 0.0f;
	a_scb.HasObjectTop = a_hasObjectTop ? 1.0f : 0.0f;
	a_scb.SkinHeightFadeEnd = settings.RangeSkinsGeometryM * kUnitsPerMeter;
	a_scb.LegacySkin = a_cap.road ? 1.0f : 0.0f;
	a_scb.FadeExempt = a_cap.fadeExempt ? 1.0f : 0.0f;
	a_scb.MoundSteepness = std::clamp(settings.SnowMoundSteepness, 0.5f, 3.0f);
	a_scb.ObjectTrenches = settings.ObjectTrenches ? 1.0f : 0.0f;
	a_scb.SkinDistantBareness = settings.SkinDistantBareness;
	a_scb.RoadField = (settings.RoadHeightfield && a_cap.road && !a_cap.bridge) ? 1.0f : 0.0f;
	a_scb.ProjThreshold = a_cap.projThreshold;
	a_scb.ProjMaskEnable = settings.ProjMaskPlacement ? 1.0f : 0.0f;
	a_scb.ProjDensityEnable = settings.ProjDepthDensity ? 1.0f : 0.0f;
	a_scb.ClassOverride = (a_s4Shell || a_cap.forceRounded) ? 1.0f : 0.0f;
	a_scb.ProjNoiseScale = a_cap.projNoiseScale;
	a_scb.ProjNoiseTiling = a_cap.projNoiseTiling;
	// 2 = the S4 shell owns this draw; 0 = classic path.
	a_scb.ProjPixelEnable = a_s4Shell ? 2.0f : 0.0f;
	a_scb.ProjSnowFillSk = std::clamp(settings.ProjSnowFillPct / 100.0f, 0.0f, 1.0f);
	// The rock family (mountain/cliff name match) carries its own max
	// slope: rocks were the only sufferers of a low global slope.
	const float maxSlopeDeg = a_cap.forceRounded ? settings.RockMaxSlopeDeg : settings.ShellMaxSlopeDeg;
	a_scb.ShellMinNz = std::cos(std::clamp(maxSlopeDeg, 0.0f, 90.0f) * 3.14159265f / 180.0f);
	a_scb.PeelTol = std::clamp(settings.PlaneMergeHeight, 1.0f, 32.0f);
	a_scb.OverheadIgnore = std::clamp(settings.OverheadClearance, 0.0f, 200.0f);
	a_scb.MeldPlanesSk = settings.MeldCoPlanar ? 1.0f : 0.0f;
	a_scb.PileHeightRatio = std::clamp(settings.PileHeightRatio, 1.0f, 8.0f);
	// P5's cornice lip. Rides FillSkinDrawCB so the shadow caster overhangs by
	// exactly the same amount the visible shell does - a caster that kept the
	// old silhouette would shadow an edge that is no longer there.
	a_scb.ObjCorniceLip = std::clamp(settings.ObjCorniceLipAmt, 0.0f, 1.5f);
	// Snow Breakup rides FillSkinDrawCB for the same reason the lip does: the
	// caster must break up exactly where the visible shell does, or a shadow
	// falls from snow that is no longer there.
	a_scb.SkinBreakup = std::clamp(settings.SkinBreakupAmt, 0.0f, 1.0f);
	// Tier 1 seam weld. Rides FillSkinDrawCB with the rest: the caster must weld
	// identically or its silhouette parts company with the shell's.
	a_scb.SkinWeld = std::clamp(settings.SkinWeldAmt, 0.0f, 1.0f);
	a_scb.SkyExposureSk = std::clamp(settings.SkyExposurePct / 100.0f, 0.0f, 1.0f);
	a_scb.ContainerSpike = settings.ContainerShellSpike ? 1.0f : 0.0f;
	a_scb.HasSkinNormalCopy = a_hasSkinNormalCopy ? 1.0f : 0.0f;
}

bool SnowDeformation::EnsureSmoothNormalsCS()
{
	if (!smoothAccumulateCS)
		smoothAccumulateCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "ACCUMULATE", "" } }, "cs_5_0"));
	if (!smoothResolveCS)
		smoothResolveCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "RESOLVE", "" } }, "cs_5_0"));
	if (!smoothFlatStatsCS)
		smoothFlatStatsCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\SmoothNormalsCS.hlsl", { { "FLATSTATS", "" } }, "cs_5_0"));
	return smoothAccumulateCS && smoothResolveCS && smoothFlatStatsCS;
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
	if (smoothedNormalsCache.size() > 1024)
		smoothedNormalsCache.clear();

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
	if (SnowShadersPending(2))
		return;
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
	// S4 phase 2 peeled-layer maps (t24/t26 = layer-2 top/cone, t27/t28 =
	// layer-3): the lift's per-vertex layer select. VS + DS only.
	ID3D11ShaderResourceView* top2SRV = (heightTop2Raw[heightCurrent] && heightTop2Raw[heightCurrent]->srv) ?
	                                        heightTop2Raw[heightCurrent]->srv.get() :
	                                        nullptr;
	ID3D11ShaderResourceView* cone2SRV = (objectSnowCone2 && objectSnowCone2->srv) ? objectSnowCone2->srv.get() : nullptr;
	context->VSSetShaderResources(24, 1, &top2SRV);
	context->DSSetShaderResources(24, 1, &top2SRV);
	context->VSSetShaderResources(26, 1, &cone2SRV);
	context->DSSetShaderResources(26, 1, &cone2SRV);
	ID3D11ShaderResourceView* layer3SRVs[2] = {
		(heightTop3Raw[heightCurrent] && heightTop3Raw[heightCurrent]->srv) ? heightTop3Raw[heightCurrent]->srv.get() : nullptr,
		(objectSnowCone3 && objectSnowCone3->srv) ? objectSnowCone3->srv.get() : nullptr
	};
	context->VSSetShaderResources(27, 2, layer3SRVs);
	context->DSSetShaderResources(27, 2, layer3SRVs);
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
	ID3D11ShaderResourceView* projNoiseSRV = settings.ObjectSnow3D ? SD_ProjNoiseMapSRV() : nullptr;
	context->PSSetShaderResources(21, 1, &projNoiseSRV);
	ID3D11ShaderResourceView* skinNormalsSRV = settings.ObjectSnow3D ? preSkinNormalsCopySRV.get() : nullptr;
	context->PSSetShaderResources(23, 1, &skinNormalsSRV);

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
		const bool s4Shell = settings.ObjectSnow3D && !settings.ObjectDrapeShell && !cap.road &&
		                     cap.projThreshold > -0.5f && projNoiseSRV;
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
		context->IASetInputLayout(layout);

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

		// Smoothed normals (built once per unique mesh): pillow inflation
		// for flat split-normal surfaces; planks, roofs, pole caps.
		ID3D11ShaderResourceView* smoothSRV = EnsureSmoothedNormals(geometry);
		context->VSSetShaderResources(10, 1, &smoothSRV);
		StaticsCB scb{};
		FillSkinDrawCB(cap, s4Shell, float(triShape->GetTrishapeRuntimeData().vertexCount),
			smoothSRV != nullptr, objectTopSRV != nullptr, skinNormalsSRV != nullptr, scb);
		staticsCB->Update(scb);

		// Depth export only where the carve can fire: SnowStaticsShell's
		// carveObject is ObjectTrenches || LegacySkin, and LegacySkin is
		// cap.road. Everything else writes back the rasterised depth, so
		// dropping the export leaves the same number in the buffer and hands
		// early-Z rejection back to the whole pass. The debug spike forces the
		// no-depth path on every draw, roads included.
		const bool needsDepth = !staticsEarlyZSpike && (settings.ObjectTrenches || cap.road);
		ID3D11PixelShader* wantPS = (!needsDepth && staticsPSNoDepth) ? staticsPSNoDepth : staticsPS;
		if (wantPS != boundStaticsPS) {
			context->PSSetShader(wantPS, nullptr, 0);
			boundStaticsPS = wantPS;
		}

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
	context->PSSetShaderResources(3, 1, &nullSmoothSRV);
	context->VSSetShaderResources(13, 1, &nullSmoothSRV);
	context->DSSetShaderResources(13, 1, &nullSmoothSRV);
	context->PSSetShaderResources(13, 1, &nullSmoothSRV);
	context->HSSetShaderResources(13, 1, &nullSmoothSRV);
	context->PSSetShaderResources(21, 1, &nullSmoothSRV);
	context->PSSetShaderResources(23, 1, &nullSmoothSRV);
	context->VSSetShaderResources(24, 1, &nullSmoothSRV);
	context->DSSetShaderResources(24, 1, &nullSmoothSRV);
	ID3D11ShaderResourceView* nullLayerSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
	context->VSSetShaderResources(25, 4, nullLayerSRVs);
	context->DSSetShaderResources(25, 4, nullLayerSRVs);

	// trench PATCH: the landscape shell's dense-grid carve applied to object
	// tops; real carved geometry drawn after the skins so it shows through
	// their dithered trench hand-off holes. SV_VertexID grid, no IA state.
	// Per-class trenching emerges from the raster: each captured object
	// writes its own class depth into the skin-depth raster, so a class at
	// 0 produces dead patch texels for its objects only. The pass gate just
	// needs ANY class active (the old > 1 threshold silently disabled the
	// whole patch at depth 1).
	if (patchVS && patchPS && heightSkinDepth && (settings.ObjectsSnowDepth > 0.5f || settings.RoadMeshesDepth > 0.5f)) {
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
		// PER-LAYER DRAPE prototype: the peeled layers' tops and cones, so a
		// pass can drape a surface the layer-1 raster hides under a roof.
		// t24/t26 = layer 2 top/cone, t27/t28 = layer 3, matching the skin's
		// slots so one set of shader accessors serves both paths.
		if (settings.LayeredObjectDrape) {
			ID3D11ShaderResourceView* peelSRVs[2] = {
				(heightTop2Raw[heightCurrent] && heightTop2Raw[heightCurrent]->srv) ? heightTop2Raw[heightCurrent]->srv.get() : nullptr,
				(objectSnowCone2 && objectSnowCone2->srv) ? objectSnowCone2->srv.get() : nullptr
			};
			ID3D11ShaderResourceView* peel3SRVs[2] = {
				(heightTop3Raw[heightCurrent] && heightTop3Raw[heightCurrent]->srv) ? heightTop3Raw[heightCurrent]->srv.get() : nullptr,
				(objectSnowCone3 && objectSnowCone3->srv) ? objectSnowCone3->srv.get() : nullptr
			};
			context->VSSetShaderResources(24, 1, &peelSRVs[0]);
			context->VSSetShaderResources(26, 1, &peelSRVs[1]);
			context->VSSetShaderResources(27, 2, peel3SRVs);
			// t30/t31 = the air test: the lowest surface standing above each
			// peeled layer, so a peeled pass can tell a roof from a wall.
			ID3D11ShaderResourceView* coverSRVs[2] = {
				(objectCoverBottom2 && objectCoverBottom2->srv) ? objectCoverBottom2->srv.get() : nullptr,
				(objectCoverBottom3 && objectCoverBottom3->srv) ? objectCoverBottom3->srv.get() : nullptr
			};
			context->VSSetShaderResources(30, 2, coverSRVs);
			if (tessellatePatch) {
				context->DSSetShaderResources(24, 1, &peelSRVs[0]);
				context->DSSetShaderResources(26, 1, &peelSRVs[1]);
				context->DSSetShaderResources(27, 2, peel3SRVs);
				context->DSSetShaderResources(30, 2, coverSRVs);
			}
		}
		// One pass per peeled layer. Layer 0 is the surface the patch has
		// always drawn; 1 and 2 exist only under cover, so their lattices
		// kill almost every vertex on an open scene - the cost is real but
		// concentrated where architecture is.
		const uint32_t layerPasses = settings.LayeredObjectDrape ? 3u : 1u;
		for (uint32_t layer = 0; layer < layerPasses; layer++) {
			scb.PatchLayer = float(layer);
			staticsCB->Update(scb);
			if (tessellatePatch)
				context->Draw(kPatchGridDim * kPatchGridDim * 4, 0);
			else
				context->Draw(kPatchGridDim * kPatchGridDim * 6, 0);
		}
		if (tessellatePatch) {
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}
		if (settings.LayeredObjectDrape) {
			ID3D11ShaderResourceView* nullPeel[2] = { nullptr, nullptr };
			context->VSSetShaderResources(24, 1, &nullPeel[0]);
			context->VSSetShaderResources(26, 1, &nullPeel[0]);
			context->VSSetShaderResources(27, 2, nullPeel);
			context->DSSetShaderResources(24, 1, &nullPeel[0]);
			context->DSSetShaderResources(26, 1, &nullPeel[0]);
			context->DSSetShaderResources(27, 2, nullPeel);
		}

		ID3D11ShaderResourceView* nullHeightSRVs[2] = { nullptr, nullptr };
		context->VSSetShaderResources(11, 2, nullHeightSRVs);
		globals::profiler->EndPass();
	}

	DrawBlobShell();

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

	globals::profiler->BeginPass("SnowDeformation::ContactCapture");
	for (const auto& prop : contactProps) {
		// Re-resolved, not held: see ContactProp. A body whose 3D went away
		// this frame simply drops out.
		auto ref = prop.ref.get();
		auto* root = ref ? ref->Get3D(false) : nullptr;
		if (!root)
			continue;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
			auto& runtime = a_geometry->GetGeometryRuntimeData();
			if (runtime.skinInstance)
				return RE::BSVisit::BSVisitControl::kContinue;
			auto* triShape = a_geometry->AsTriShape();
			if (!triShape)
				return RE::BSVisit::BSVisitControl::kContinue;
			auto* rendererData = runtime.rendererData;
			if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				return RE::BSVisit::BSVisitControl::kContinue;
			const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
			if (indexCount == 0)
				return RE::BSVisit::BSVisitControl::kContinue;
			auto desc = rendererData->vertexDesc;
			if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
				return RE::BSVisit::BSVisitControl::kContinue;
			uint64_t descKey;
			memcpy(&descKey, &desc, sizeof(descKey));
			auto* layout = StaticsInputLayoutFor(descKey, desc);
			if (!layout)
				return RE::BSVisit::BSVisitControl::kContinue;
			const UINT stride = uint32_t(descKey & 0xF) * 4;
			if (stride == 0)
				return RE::BSVisit::BSVisitControl::kContinue;
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);

			StaticsCB scb{};
			const auto& world = a_geometry->world;
			const auto& rot = world.rotate;
			const float scale = world.scale;
			scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, world.translate.x };
			scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, world.translate.y };
			scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, world.translate.z };
			scb.HeightWindowCenter = contactCenter;
			scb.HeightHalfExtent = kContactHalfExtent;
			staticsCB->Update(scb);

			context->DrawIndexed(indexCount, 0, 0);
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
	if (!contactActors.empty() && contactSkinVS && contactSkinCB) {
		globals::profiler->BeginPass("SnowDeformation::ContactSkin");
		context->VSSetShader(contactSkinVS, nullptr, 0);
		ID3D11Buffer* skinCB = contactSkinCB->CB();
		context->VSSetConstantBuffers(2, 1, &skinCB);
		contactYawTraceFrames = (contactYawTraceFrames + 1) % 60;
		for (const auto& actor : contactActors) {
			auto ref = actor.ref.get();
			auto* root = ref ? ref->Get3D(false) : nullptr;
			if (!root)
				continue;
			contactYawTraceAngle = ref->GetAngleZ();
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
				auto& runtime = a_geometry->GetGeometryRuntimeData();
				auto* skin = runtime.skinInstance.get();
				if (!skin)
					return RE::BSVisit::BSVisitControl::kContinue;
				auto* skinData = skin->skinData.get();
				auto* skinPartition = skin->skinPartition.get();
				// Accessors, not the raw members: those are compiled out under
				// cross-VR targeting, and these relocate per runtime.
				// partitions is a bare array behind a count; an instance mid-
				// rebuild can carry a count with no array.
				if (!skinData || !skinPartition || !skin->bones || !skinPartition->partitions.data())
					return RE::BSVisit::BSVisitControl::kContinue;
				const uint32_t boneCount = skinData->GetBoneCount();
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

					ContactSkinCB cb{};
					cb.SkinWindowCenter = contactCenter;
					cb.SkinHalfExtent = kContactHalfExtent;
					for (uint16_t j = 0; j < part.numBones; ++j) {
						const uint16_t b = part.bones[j];
						if (b >= boneCount)
							continue;
						auto* boneNode = skin->bones[b];
						if (!boneNode)
							continue;  // an editor removed it; this bone simply moves nothing
						const RE::NiTransform m = boneNode->world * skinData->GetBoneDataSkinToBone(b);
						const auto& rot = m.rotate;
						const float sc = m.scale;
						// Yaw trace, once a second while the field view is up: does the
						// composed row turn with the actor? Each factor logged apart,
						// so the log names the one that drops the facing.
						if (debugContactView && contactYawTraceFrames == 0 && p == 0 && j == 0) {
							const auto& bw = boneNode->world.rotate;
							const auto& s2b = skinData->GetBoneDataSkinToBone(b).rotate;
							const char* boneName = boneNode->name.c_str() ? boneNode->name.c_str() : "";
							logger::info("[SNOW DEFORMATION] yaw trace '{}' bone '{}': actor yaw {:.2f} rad | bone world row0 ({:.2f} {:.2f} {:.2f}) row1 ({:.2f} {:.2f} {:.2f}) | skinToBone row0 ({:.2f} {:.2f} {:.2f}) | composed row0 ({:.2f} {:.2f} {:.2f}) row1 ({:.2f} {:.2f} {:.2f}) scale {:.3f}",
								a_geometry->name.c_str() ? a_geometry->name.c_str() : "", boneName, contactYawTraceAngle,
								bw.entry[0][0], bw.entry[0][1], bw.entry[0][2], bw.entry[1][0], bw.entry[1][1], bw.entry[1][2],
								s2b.entry[0][0], s2b.entry[0][1], s2b.entry[0][2],
								rot.entry[0][0], rot.entry[0][1], rot.entry[0][2], rot.entry[1][0], rot.entry[1][1], rot.entry[1][2], sc);
						}
						cb.BoneRows[j * 3 + 0] = { rot.entry[0][0] * sc, rot.entry[0][1] * sc, rot.entry[0][2] * sc, m.translate.x };
						cb.BoneRows[j * 3 + 1] = { rot.entry[1][0] * sc, rot.entry[1][1] * sc, rot.entry[1][2] * sc, m.translate.y };
						cb.BoneRows[j * 3 + 2] = { rot.entry[2][0] * sc, rot.entry[2][1] * sc, rot.entry[2][2] * sc, m.translate.z };
					}
					contactSkinCB->Update(cb);

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
		}
		globals::profiler->EndPass();
	}

	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	context->RSSetState(savedRaster.get());
}
