// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include <DirectXPackedVector.h>
#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/DevSwitches.h"

#include <d3dcompiler.h>

#include "Deferred.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/IBL.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Utils/D3D.h"

#include <bit>

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
			bound.radius, bound.center.z + bound.radius, distXY, capture.road });
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

// Non-snow materials, matched on MATO editor IDs, on DynDOLOD's LOD batch names
// (objAsh on Solstheim) and on precipitation records (the ash storm).
static constexpr std::array kNotSnowKeywords{ "sand", "moss", "dirt", "mud", "gravel", "ash", "coast" };

const char* SnowDeformation::NotSnowKeywordIn(const char* a_name)
{
	for (const auto* keyword : kNotSnowKeywords)
		if (ContainsNoCase(a_name, keyword))
			return keyword;
	return nullptr;
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
	// DynDOLOD batch (obj<Material>[HD][-LargeRef]) of a non-snow projected
	// material: no reference behind it, so the name is the only MATO signal.
	bool lodNotSnow = false;
	// Dynamic Bloodpool Framework's skinned pool quads (Decal:NN / DecalExt:NN).
	bool bloodPool = false;
	// The engine's own runtime decals: scorch, runes, blood.
	bool engineDecal = false;
	bool capturedLogged = false;
	bool roundedLogged = false;
	bool plankLogged = false;
	bool decalLogged = false;
	bool lodVetoLogged = false;
};

// One multiply; std::hash of a pointer is FNV-1a over all eight bytes. The hooks probe on every draw.
struct SD_PointerHash
{
	size_t operator()(const void* a_pointer) const noexcept
	{
		const uint64_t h = (uint64_t)(uintptr_t)a_pointer * 0x9E3779B97F4A7C15ull;
		return (size_t)(h ^ (h >> 32));
	}
};

// The per-draw caches: open addressing with each value beside its key, so a
// probe reads one cache line where std::unordered_map reads the bucket array
// and then a node. Callers clear past 16384 entries, so a probe always meets
// an empty slot.
template <class V>
class SD_InlinePointerTable
{
public:
	static constexpr uint32_t kSlots = 32768;

	size_t size() const { return count; }

	void clear()
	{
		if (slots)
			for (uint32_t i = 0; i < kSlots; i++)
				slots[i].key = nullptr;
		count = 0;
	}

	std::pair<V*, bool> try_emplace(const void* a_key)
	{
		if (!slots)
			slots = std::make_unique<Slot[]>(kSlots);
		for (uint32_t i = static_cast<uint32_t>(SD_PointerHash{}(a_key));; i++) {
			Slot& slot = slots[i & (kSlots - 1)];
			if (slot.key == a_key)
				return { &slot.value, false };
			if (!slot.key) {
				slot.key = a_key;
				slot.value = V{};
				count++;
				return { &slot.value, true };
			}
		}
	}

private:
#pragma warning(push)
#pragma warning(disable: 4324)  // padded to a cache line on purpose
	struct alignas(64) Slot
	{
		const void* key = nullptr;
		V value{};
	};
#pragma warning(pop)
	std::unique_ptr<Slot[]> slots;
	uint32_t count = 0;
};

// File scope: a function-local static pays the thread-safe init check on every call.
static SD_InlinePointerTable<GeometryNameFacts> sNameFactsCache;

bool SnowDeformation::IsEngineDecalName(const char* a_name)
{
	return a_name && (_stricmp(a_name, "Decal") == 0 || _stricmp(a_name, "Permanent Decal") == 0 || _stricmp(a_name, "SkinnedDecal") == 0);
}

static GeometryNameFacts& NameFactsOf(RE::BSGeometry* a_geometry)
{
	auto& cache = sNameFactsCache;
	static GeometryNameFacts empty;
	// Same one-entry memo as RecordOf: the hook asks 2-3 times per draw.
	static const void* lastGeometry = nullptr;
	static const char* lastName = nullptr;
	static GeometryNameFacts* lastFacts = nullptr;
	const char* name = a_geometry ? a_geometry->name.c_str() : nullptr;
	if (!name || !*name)
		return empty;
	if (lastFacts && lastGeometry == a_geometry && lastName == name)
		return *lastFacts;
	if (cache.size() > 16384) {
		cache.clear();
		lastFacts = nullptr;
	}
	const uint32_t length = (uint32_t)strlen(name);
	uint64_t head = 0;
	memcpy(&head, name, std::min<size_t>(length, sizeof(head)));
	auto [facts, inserted] = cache.try_emplace(name);
	auto& f = *facts;
	if (inserted || f.length != length || f.head != head) {
		f = {};
		f.length = length;
		f.head = head;
		f.bridge = ContainsNoCase(name, "bridge");
		f.road = ContainsNoCase(name, "road");
		f.mountainCliff = ContainsNoCase(name, "mountain") || ContainsNoCase(name, "cliff");
		f.plank = ContainsNoCase(name, "plank") || ContainsNoCase(name, "walkway") || ContainsNoCase(name, "catwalk");
		f.iceFamily = (std::tolower((unsigned char)name[0]) == 'i' && std::tolower((unsigned char)name[1]) == 'c' && std::tolower((unsigned char)name[2]) == 'e') ||
		              ContainsNoCase(name, "glacier") || ContainsNoCase(name, "iceberg");
		// Snow drifts, not shore driftwood (a twig-card class on its diffuse).
		f.drift = ContainsNoCase(name, "drift") && !ContainsNoCase(name, "driftwood");
		f.bloodPool = _strnicmp(name, "Decal:", 6) == 0 || _strnicmp(name, "DecalExt:", 9) == 0;
		f.engineDecal = SnowDeformation::IsEngineDecalName(name);
		f.lodNotSnow = false;
		if (length > 3 && std::tolower((unsigned char)name[0]) == 'o' && std::tolower((unsigned char)name[1]) == 'b' && std::tolower((unsigned char)name[2]) == 'j') {
			for (const auto* keyword : kNotSnowKeywords)
				f.lodNotSnow = f.lodNotSnow || ContainsNoCase(name, keyword);
		}
	}
	lastGeometry = a_geometry;
	lastName = name;
	lastFacts = &f;
	return f;
}

// Per-loop filter for the IA slots and VS t10, which instances of one mesh
// repeat. Device state on entry is unknown, so the first call binds; the
// loops never pass a null layout or buffer, so null reads as "not yet bound".
struct SD_DrawBinds
{
	ID3D11InputLayout* layout = nullptr;
	ID3D11Buffer* vb = nullptr;
	UINT stride = 0;
	ID3D11Buffer* ib = nullptr;
	DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
	bool smoothBound = false;
	ID3D11ShaderResourceView* smooth = nullptr;

	void Layout(ID3D11DeviceContext* a_context, ID3D11InputLayout* a_layout)
	{
		if (layout == a_layout)
			return;
		a_context->IASetInputLayout(a_layout);
		layout = a_layout;
	}
	void Vertex(ID3D11DeviceContext* a_context, ID3D11Buffer* a_vb, UINT a_stride)
	{
		if (vb == a_vb && stride == a_stride)
			return;
		UINT offset = 0;
		a_context->IASetVertexBuffers(0, 1, &a_vb, &a_stride, &offset);
		vb = a_vb;
		stride = a_stride;
	}
	void Index(ID3D11DeviceContext* a_context, ID3D11Buffer* a_ib, DXGI_FORMAT a_format)
	{
		if (ib == a_ib && ibFormat == a_format)
			return;
		a_context->IASetIndexBuffer(a_ib, a_format, 0);
		ib = a_ib;
		ibFormat = a_format;
	}
	// t10 may legitimately be null (no smoothed normals), hence the flag.
	void Smooth(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_srv)
	{
		if (smoothBound && smooth == a_srv)
			return;
		a_context->VSSetShaderResources(10, 1, &a_srv);
		smooth = a_srv;
		smoothBound = true;
	}
};

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
		// Seasons of Skyrim's multipass snow MATO, swapped onto the base at
		// clone time: snow, but with no authored max angle.
		kSnowSeasons,
		kNotSnow
	};

	// The reference's STAT directional-material record vetoes non-snow
	// projections. Negative keywords only: requiring "snow" in the path
	// vetoed every MATO whose replacer names it differently.
	// Sand/moss/ash keep their veto; an
	// unrecognized path passes. Cached per MATO form (Seasons of Skyrim
	// swaps the MATO under the same base each season); each new entry is
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
		auto* stat = base->As<RE::TESObjectSTAT>();
		auto* mato = stat ? stat->data.materialObj : nullptr;
		static std::unordered_map<RE::FormID, MatoClass> matoClassCache;
		if (matoClassCache.size() > 4096)
			matoClassCache.clear();
		auto [it, inserted] = matoClassCache.try_emplace(mato ? mato->GetFormID() : 0, MatoClass::kNoMato);
		if (inserted) {
			if (mato) {
				// The MODL path is junk on vanilla MATO records (Bethesda left
				// 'shadertests\shaderbox.nif' in most of them — the whole
				// modlist logged that one path), so the editor ID carries the
				// real identity (SnowMaterialObjectNoise1P, SandMaterialObject
				// ...). Runtime editor IDs need po3 Tweaks; when absent the
				// signal degrades to the path alone.
				std::string matoPath;
				if (const char* edid = mato->GetFormEditorID())
					matoPath = edid;
				matoPath += '|';
				matoPath += mato->GetModel();
				std::transform(matoPath.begin(), matoPath.end(), matoPath.begin(),
					[](unsigned char c) { return (char)std::tolower(c); });
				if (matoPath.starts_with("sos_win_snowmaterialobject")) {
					it->second = MatoClass::kSnowSeasons;
				} else if (matoPath.find("snow") != std::string::npos) {
					it->second = MatoClass::kSnow;
				} else {
					it->second = MatoClass::kNoMato;
					for (const auto* keyword : kNotSnowKeywords) {
						if (matoPath.find(keyword) != std::string::npos) {
							it->second = MatoClass::kNotSnow;
							break;
						}
					}
				}
				logger::info("[SNOW DEFORMATION] Projected MATO {:08X} (base {:08X}): '{}' -> {}", mato->GetFormID(), base->GetFormID(), matoPath,
					it->second == MatoClass::kSnow ? "snow" : (it->second == MatoClass::kSnowSeasons ? "snow (Seasons, unauthored)" : (it->second == MatoClass::kNotSnow ? "NOT snow (vetoed)" : "neutral")));
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
		// "snow" in the diffuse FILE name, any folder (season swaps' alternate sets, dirt cliffs' snow tops).
		bool snowNamed = false;
		// A plain landscape snow set (snow01, snow02, snowrocks01, snowstone01;
		// landscape root, blend\ or statics\): ground material, not trees,
		// grass, plants or roads.
		bool groundSnow = false;
		// Architecture diffuse that is not foliage: an alpha-tested roof or
		// plank end is a surface, not a card.
		bool architecture = false;
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
		auto* tes = globals::game::tes;
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

	// Capture verdict log (Log All Capture Verdicts): the geometry's facts and
	// which gate ended this pass's journey, once per geometry+outcome; the
	// outcome must be a string literal (the dedup keys on its address).
	// Render thread only, like the other caches.
	void LogCaptureVerdict(RE::BSRenderPass* a_pass, const char* a_outcome)
	{
		const bool logAll = globals::features::snowDeformation.debugLogAllJourneys;
		static std::unordered_set<uint64_t> logged;
		// A fresh tick starts a fresh budget.
		static bool lastLogAll = false;
		if (logAll && !lastLogAll)
			logged.clear();
		lastLogAll = logAll;
		if (!logAll)
			return;
		// Flora and LOD are the spam that ate the budget before the buildings
		// were reached.
		using LFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (a_pass->shaderProperty->flags.any(LFlag::kTreeAnim, LFlag::kLODObjects, LFlag::kHDLODObjects, LFlag::kLODLandscape))
			return;
		if (logged.size() > 65536u)
			return;
		auto* geometry = a_pass->geometry;
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
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
		auto* alpha = geometry->GetGeometryRuntimeData().alphaProperty.get();
		logger::info("[SNOW DEFORMATION] ice journey '{}' r={:.0f} proj={} snow={} lodObj={} hdLod={} lodLand={} decalMode={} alphaTest={} alphaBlend={} ref={:08X} tex='{}' -> {}",
			geometry->name.empty() ? "<unnamed>" : geometry->name.c_str(),
			geometry->worldBound.radius,
			flags.all(Flag::kProjectedUV) ? 1 : 0, flags.all(Flag::kSnow) ? 1 : 0,
			flags.all(Flag::kLODObjects) ? 1 : 0, flags.all(Flag::kHDLODObjects) ? 1 : 0,
			flags.all(Flag::kLODLandscape) ? 1 : 0,
			globals::game::shadowState->GetRuntimeData().rasterStateDepthBiasMode,
			alpha && alpha->GetAlphaTesting() ? 1 : 0, alpha && alpha->GetAlphaBlending() ? 1 : 0,
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
					// Berries and the no-snow variants are the known false friends.
					const size_t slash = lowered.find_last_of("\\/");
					const std::string file = slash == std::string::npos ? lowered : lowered.substr(slash + 1);
					it->second.snowNamed = file.find("snow") != std::string::npos &&
					                       file.find("snowberr") == std::string::npos &&
					                       file.find("nosnow") == std::string::npos;
					const std::string folder = slash == std::string::npos ? std::string() : lowered.substr(0, slash);
					it->second.groundSnow = it->second.snowNamed && file.rfind("snow", 0) == 0 &&
					                        folder.find("landscape") != std::string::npos &&
					                        folder.find("tree") == std::string::npos && folder.find("grass") == std::string::npos &&
					                        folder.find("plant") == std::string::npos && folder.find("road") == std::string::npos &&
					                        folder.find("flower") == std::string::npos;
					it->second.architecture = lowered.find("architecture") != std::string::npos &&
					                          lowered.find("ivy") == std::string::npos && lowered.find("vine") == std::string::npos &&
					                          lowered.find("leaf") == std::string::npos && lowered.find("leaves") == std::string::npos &&
					                          lowered.find("grass") == std::string::npos && lowered.find("branch") == std::string::npos &&
					                          lowered.find("moss") == std::string::npos;
				}
			}
		}
		return it->second;
	}

	// Does another geometry of this one's model take the road class by name,
	// and pass the material gate (projection data or a landscape snow
	// diffuse)? A road replacer's alpha-tested strips over the road shape
	// (TMD Dirt Roads) inherit that verdict.
	bool ModelRoadPassesGate(RE::BSGeometry* a_geometry)
	{
		RE::NiAVObject* root = a_geometry->parent;
		for (RE::NiAVObject* node = a_geometry; node; node = node->parent) {
			if (node->GetUserData()) {
				root = node;
				break;
			}
		}
		if (!root)
			return false;
		bool found = false;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_mate) -> RE::BSVisit::BSVisitControl {
			const char* name = a_mate->name.c_str();
			if (a_mate == a_geometry || !name || !ContainsNoCase(name, "road") || ContainsNoCase(name, "bridge"))
				return RE::BSVisit::BSVisitControl::kContinue;
			auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(a_mate->GetGeometryRuntimeData().shaderProperty.get());
			auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
			auto* textureSet = material ? material->textureSet.get() : nullptr;
			const char* diffuse = textureSet ? textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
			if (!diffuse || ContainsNoCase(diffuse, "bridge"))
				return RE::BSVisit::BSVisitControl::kContinue;
			const bool paving = (ContainsNoCase(diffuse, "floor") || ContainsNoCase(diffuse, "slate")) && !ContainsNoCase(diffuse, "roof");
			if (!ContainsNoCase(diffuse, "landscape") && !paving)
				return RE::BSVisit::BSVisitControl::kContinue;
			found = property->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kProjectedUV) || ClassifySnowPath(material).base;
			return found ? RE::BSVisit::BSVisitControl::kStop : RE::BSVisit::BSVisitControl::kContinue;
		});
		return found;
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
	bool pathArchitecture = false;
	bool pathSnowNamed = false;
	bool pathGroundSnow = false;
	bool iceName = false;
	bool shard = false;
	// Diffuse path names blood: a decal-mode draw of it is a blood mark.
	bool bloodTex = false;
	uint8_t roadTex = 0;  // 0 none, 1 road, 2 bridge (an exclusion, never a road signal)
	// Alpha-tested, not road-named, in a model whose road shape passes the
	// material gate: part of that road's surface (ModelRoadPassesGate).
	bool roadLayer = false;
	// Diffuse under a landscape folder, or a paving texture (floor/slate,
	// not roof): the road-name signal needs one of them.
	bool landscapeTex = false;
	bool pavingTex = false;
	MatoClass mato = MatoClass::kNoReference;
	// Projection applied at runtime by Seasons of Skyrim: its multipass MATO
	// or its single-pass marker (SOS_SNOW_SHADER extra data on the root).
	bool seasonsProj = false;
	// The reference's base is one of Seasons' own snow statics: a snow MATO
	// on a mesh with no authored vertex alpha (the alpha reads 0, not 1).
	bool seasonsStatic = false;
	// Nominal snow depth at the bound centre, valid while snowDepthVersion holds.
	float depthX = 0.0f;
	float depthY = 0.0f;
	float depth = 0.0f;
	uint32_t depthVersion = 0;
};

static SD_InlinePointerTable<GeometryRecord> sGeometryRecordCache;

// The pass blends over the surface beneath it (decal or alpha blend): its
// Masks write is blended too, and a coat cannot follow its fade.
static bool PassBlendsOver(RE::BSRenderPass* a_pass)
{
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	if (a_pass->shaderProperty->flags.any(Flag::kDecal, Flag::kDynamicDecal))
		return true;
	auto* alpha = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
	return alpha && alpha->GetAlphaBlending();
}

static GeometryRecord& RecordOf(RE::BSGeometry* a_geometry, RE::BSLightingShaderMaterialBase* a_material)
{
	auto& cache = sGeometryRecordCache;
	static const void* lastGeometry = nullptr;
	static GeometryRecord* lastRecord = nullptr;
	const char* name = a_geometry->name.c_str();
	if (lastRecord && lastGeometry == a_geometry && lastRecord->material == a_material && lastRecord->name == name)
		return *lastRecord;
	if (cache.size() > 16384) {
		cache.clear();
		lastRecord = nullptr;
	}
	auto [record, inserted] = cache.try_emplace(a_geometry);
	auto& r = *record;
	if (inserted || r.material != a_material || r.name != name) {
		r = {};
		r.material = a_material;
		r.name = name;
		auto& nameFacts = NameFactsOf(a_geometry);
		r.iceName = nameFacts.iceFamily;
		if (a_material) {
			const SnowPathMatch& path = ClassifySnowPath(a_material);
			r.pathBase = path.base;
			r.pathNatural = path.naturalFeature;
			r.pathMountain = path.mountain;
			r.pathArchitecture = path.architecture;
			r.pathSnowNamed = path.snowNamed;
			r.pathGroundSnow = path.groundSnow;
			if (auto textureSet = a_material->textureSet.get()) {
				if (auto diffuse = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					r.shard = ContainsNoCase(diffuse, "branchpile") || ContainsNoCase(diffuse, "driftwood");
					r.roadTex = ContainsNoCase(diffuse, "bridge") ? 2 : (ContainsNoCase(diffuse, "road") ? 1 : 0);
					r.landscapeTex = ContainsNoCase(diffuse, "landscape");
					r.pavingTex = (ContainsNoCase(diffuse, "floor") || ContainsNoCase(diffuse, "slate")) && !ContainsNoCase(diffuse, "roof");
					r.bloodTex = ContainsNoCase(diffuse, "blood");
				}
			}
		}
		r.mato = ClassifyProjectedMato(a_geometry);
		if (r.mato == MatoClass::kNoReference && nameFacts.lodNotSnow) {
			r.mato = MatoClass::kNotSnow;
			if (!nameFacts.lodVetoLogged) {
				nameFacts.lodVetoLogged = true;
				logger::info("[SNOW DEFORMATION] LOD batch '{}' -> NOT snow (vetoed by name)", name);
			}
		}
		r.seasonsProj = r.mato == MatoClass::kSnowSeasons;
		if (!r.seasonsProj) {
			static const RE::BSFixedString seasonsMarker("SOS_SNOW_SHADER");
			for (RE::NiAVObject* node = a_geometry; node && !r.seasonsProj; node = node->parent)
				r.seasonsProj = node->GetExtraData(seasonsMarker) != nullptr;
		}
		// Seasons' own snow statics (SnowOverSkyrim.esp): a vanilla snow MATO
		// on a mesh whose vertex alpha was never authored for it, so the
		// projection paints nothing; same mask as the runtime projection.
		if (!r.seasonsProj) {
			static std::unordered_map<uint32_t, bool> sosBase;
			RE::TESObjectREFR* refr = nullptr;
			for (RE::NiAVObject* node = a_geometry; node && !refr; node = node->parent)
				refr = static_cast<RE::TESObjectREFR*>(node->GetUserData());
			if (auto* base = refr ? refr->GetBaseObject() : nullptr) {
				auto [sosIt, sosInserted] = sosBase.try_emplace(base->GetFormID(), false);
				if (sosInserted) {
					if (auto* file = base->GetFile(0); file && file->fileName)
						sosIt->second = ContainsNoCase(file->fileName, "snowoverskyrim.esp");
				}
				r.seasonsStatic = sosIt->second;
				r.seasonsProj = sosIt->second;
			}
		}
		auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
		if (alpha && alpha->GetAlphaTesting() && !nameFacts.road && !nameFacts.bridge && r.roadTex != 2)
			r.roadLayer = ModelRoadPassesGate(a_geometry);
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
	extraDescriptor &= ~(uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow) | uint32_t(State::ExtraFeatureDescriptors::SnowLODBakedIsSnow) |
						 uint32_t(State::ExtraFeatureDescriptors::SnowProjectedUnauthored) | uint32_t(State::ExtraFeatureDescriptors::SnowProjectedNoAlpha) |
						 uint32_t(State::ExtraFeatureDescriptors::SnowMultipassBase) | uint32_t(State::ExtraFeatureDescriptors::SnowObjectBarePass) |
						 uint32_t(State::ExtraFeatureDescriptors::SnowTexturedOverlay) | uint32_t(State::ExtraFeatureDescriptors::SnowTexturedGround));
	if (!a_shader || !a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	if (!settings.EnableSnowDeformation || !shellSnowDiffuseSRV)
		return;
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	bool bindSnowSet = false;
	bool projectedSnow = false;
	if (settings.ProjSnowMatch) {
		const bool passProjected = (a_shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) != 0;
		const bool treeAnim = a_pass->shaderProperty->flags.all(Flag::kTreeAnim);
		const auto& rec = RecordOf(a_pass->geometry, static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material));
		// Seasons of Skyrim's runtime projection: flagged on EVERY pass of the
		// object. A multipass object's base pass carries no projection, and
		// without this bit Lighting wrote no mask there, so the skin read
		// "unknown" and reconstructed a coat over every face; with it the base
		// pass writes "known, unpainted" and only the sparkle pass raises it.
		if (rec.seasonsProj && !treeAnim)
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowProjectedUnauthored);
		if (rec.seasonsStatic && !treeAnim)
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowProjectedNoAlpha);
		// An authored multipass snow MATO draws the same way Seasons' does: a
		// base pass with no projection that leaves the read-back mask at 0,
		// which the skin took for "unknown" and coated every face of
		// (baldpatch79's Nordic walls, 2026-09-14). The authored angle and
		// alpha stay: only the base-pass mask write is borrowed.
		if (settings.MultipassSnowFollowsPaint && !rec.seasonsProj && !treeAnim && rec.mato != MatoClass::kNotSnow &&
			a_pass->shaderProperty->flags.all(Flag::kMultiIndexSnow))
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowMultipassBase);
		// A shape the game draws with NO projection, on an object whose MATO
		// really is snow (Inn01:49 under XPMF, measured 2026-09-22: it lost
		// its ProjectedUV bit, drew over the porch and wrote Masks.y 0 across
		// the verdict Inn01:5 had written, which the drape read as licence and
		// sleeved the door posts). "Known, unpainted" is the true reading: the
		// game drew something that is not snow here. Only a POSITIVE snow MATO
		// qualifies, so mesh-replacer statics and drifts - no MATO at all, and
		// they depend on the unknown-means-reconstruct path - are untouched,
		// and engine decals keep their own veto.
		if (settings.UnprojectedShapesBare && !passProjected && !treeAnim &&
			(rec.mato == MatoClass::kSnow || rec.mato == MatoClass::kSnowSeasons) &&
			!a_pass->shaderProperty->flags.any(Flag::kLODObjects, Flag::kHDLODObjects) &&
			!NameFactsOf(a_pass->geometry).engineDecal)
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowObjectBarePass);
		if (!passProjected || treeAnim) {
			statProjNoProjection++;
		} else if (rec.mato == MatoClass::kNotSnow) {
			statProjVetoed++;
		} else if (NameFactsOf(a_pass->geometry).engineDecal) {
			// A runtime decal inherits its target's projected technique; it is a mark on the snow, not snow.
			// By the engine's own geometry name, never kDynamicDecal: DynDOLOD's passthru LOD batches carry that flag.
			statProjVetoed++;
		} else {
			statProjMatched++;
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowProjectedIsSnow);
			bindSnowSet = true;
			projectedSnow = true;
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
	// Snow-textured shapes without projection (a dirt cliff's snow01 top, a
	// rock pile's base and skirt, a season swap's alternate set, drifts): the
	// baked recipe, so the weight is written like projected snow.
	if (settings.SnowTexturedRecolor && settings.ProjSnowMatch && !bindSnowSet &&
		!a_pass->shaderProperty->flags.any(Flag::kLODObjects, Flag::kHDLODObjects, Flag::kTreeAnim) &&
		(a_shader->currentRawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ProjectedUV)) == 0) {
		const auto& rec = RecordOf(a_pass->geometry, static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material));
		if (rec.pathSnowNamed && rec.mato != MatoClass::kNotSnow) {
			extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowLODBakedIsSnow);
			if (rec.pathGroundSnow)
				extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowTexturedGround);
			if (PassBlendsOver(a_pass))
				extraDescriptor |= uint32_t(State::ExtraFeatureDescriptors::SnowTexturedOverlay);
			bindSnowSet = true;
		}
	}
	if (bindSnowSet) {
		// The Prepass-time snow set bind does NOT survive to the Lighting
		// draws (frame7075: null at every player-view draw — stomped around
		// the cubemap pass; t101 only survives via its later re-bind).
		// Re-bind per classified draw, where it is actually sampled, with the
		// water raster for the underwater veto, same fate.
		auto* context = globals::d3d::context;
		ID3D11ShaderResourceView* albedoSRV = shellSnowDiffuseSRV.get();
		ID3D11ShaderResourceView* normalSRV = shellSnowNormalSRV.get();
		ID3D11ShaderResourceView* waterSRV = waterFineTexture ? waterFineTexture->srv.get() : nullptr;
		ID3D11ShaderResourceView* rmaosSRV = shellSnowRmaosSRV.get();
		context->PSSetShaderResources(kHorizonSnowAlbedoSlot, 1, &albedoSRV);
		context->PSSetShaderResources(kHorizonSnowNormalSlot, 1, &normalSRV);
		context->PSSetShaderResources(kWaterWindowSlot, 1, &waterSRV);
		context->PSSetShaderResources(kHorizonSnowRmaosSlot, 1, &rmaosSRV);
		// The print readers (t114-t115), same fate; only projected snow reads them.
		if (projectedSnow && printRectPublished.z > printRectPublished.x)
			BindPrintReaders(globals::d3d::context);
	}
}

// The Masks channel is not colour: it is the note Lighting leaves for the
// drape saying whether the game painted snow on this pixel. A decal-mode draw
// blends EVERY bound target, so that note gets averaged with the decal's own
// value and the drape reads the result as bare. Snapshot it before the frame's
// first decal-mode draw; the coat merges the two (SnowStaticsShell SkinMasksY).
void SnowDeformation::CapturePreDecalMasks()
{
	auto* context = globals::d3d::context;
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;
	auto* masksSRV = renderer->GetRuntimeData().renderTargets[MASKS].SRV;
	if (!masksSRV)
		return;
	// Mid-pass: the game's targets are unbound around the copy and put back.
	ID3D11RenderTargetView* rtvs[8]{};
	ID3D11DepthStencilView* dsv = nullptr;
	context->OMGetRenderTargets(8, rtvs, &dsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	// The bound viewport is the main pass's: the skins read inside it, +-1 pixel.
	UINT vpCount = 1;
	D3D11_VIEWPORT vp{};
	context->RSGetViewports(&vpCount, &vp);
	winrt::com_ptr<ID3D11Resource> masksRes;
	masksSRV->GetResource(masksRes.put());
	D3D11_TEXTURE2D_DESC masksDesc{};
	if (auto masksTex = masksRes.try_as<ID3D11Texture2D>())
		masksTex->GetDesc(&masksDesc);
	const D3D11_BOX masksBox = ScreenRectBox(vpCount ? &vp : nullptr, masksDesc.Width, masksDesc.Height, 1);
	// Called from inside the game's draws: an open terrain-run row would nest.
	CloseLandscapeRun();
	globals::profiler->BeginPass("SnowDeformation::Ground - Depth & Mask Copies");
	CopySRVResource(masksSRV, "SnowDeformation::PreDecalMasks", preDecalMasksTex, preDecalMasksSRV, &masksBox);
	globals::profiler->EndPass();
	context->OMSetRenderTargets(8, rtvs, dsv);
	for (auto* rtv : rtvs)
		if (rtv)
			rtv->Release();
	if (dsv)
		dsv->Release();
}

void SnowDeformation::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass, uint32_t a_rawTechnique)
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

	// Before the frame's first decal-mode draw blends the Masks note away.
	// Same test the capture path uses for decalDepth; the hook runs BEFORE
	// the draw, so the targets still hold the note the surface wrote.
	if (preDecalMasksFrame != globals::state->frameCount &&
		!(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))) {
		using DecalFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto biasMode = globals::game::shadowState->GetRuntimeData().rasterStateDepthBiasMode;
		if (biasMode != 0 || a_pass->shaderProperty->flags.any(DecalFlag::kDecal, DecalFlag::kDynamicDecal)) {
			preDecalMasksFrame = globals::state->frameCount;
			CapturePreDecalMasks();
		}
	}

	// One verdict per geometry per frame: the same geometry arrives once per
	// pass that draws it, and nothing below depends on the pass. A capture
	// already returned at the set insert; this returns the rejections too.
	// The clean gate: projected-UV + snow flags together; covers rocks,
	// roofs, logs, stumps and never flora, because foliage is not
	// snow-PROJECTED. Drifts (no flags at all) qualify via a NARROW texture
	// match; a catch-all "snow" match drags frosted bushes in, whose leaf
	// cards shard under the skin.
	auto& rec = RecordOf(a_pass->geometry, static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material));
	const auto& passFacts = NameFactsOf(a_pass->geometry);
	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	const auto& flags = a_pass->shaderProperty->flags;
	// Blood decals go to the blood map, never to the statics list. Skinned
	// blood is a wound decal on a body unless it is a pool framework quad.
	// The engine's runtime decals that are not blood - scorch, frost, shock,
	// rune glyphs: the object-snow coat draws over them, so they join the
	// overlay that puts a decal back over the coat.
	// By name, never by kDynamicDecal: DynDOLOD's passthru LOD batches carry the
	// flag and draw mid-pass, which put the overlay's pre-decal snapshot ahead of
	// half the frame and the stale paint mask under the road patch (2026-09-18).
	const bool engineDecal = !rec.bloodTex && passFacts.engineDecal &&
	                         !flags.any(Flag::kLODObjects, Flag::kHDLODObjects, Flag::kLODLandscape);
	if (engineDecal && settings.DecalsOnObjectSnow && settings.ProjSnowMatch &&
		a_pass->geometry->GetGeometryRuntimeData().skinInstance == nullptr)
		CaptureDecalOverlay(a_pass);
	// A live rune's decal, or a frost mark, goes to the rune atlas.
	if (!rec.bloodTex && ((settings.RuneDecalsOnSnow && runeSitesLive.load(std::memory_order_acquire) != 0) || (engineDecal && settings.FrostDecalsOnSnow))) {
		const bool decalMode = globals::game::shadowState->GetRuntimeData().rasterStateDepthBiasMode != 0 ||
		                       flags.any(Flag::kDecal, Flag::kDynamicDecal);
		if (decalMode && CaptureRuneDraw(a_pass, engineDecal))
			return;
	}
	if (engineDecal)
		return;
	if (rec.bloodTex) {
		const bool poolQuad = NameFactsOf(a_pass->geometry).bloodPool;
		const bool decalMode = globals::game::shadowState->GetRuntimeData().rasterStateDepthBiasMode != 0 ||
		                       flags.any(Flag::kDecal, Flag::kDynamicDecal);
		if (decalMode || poolQuad) {
			if (settings.BloodOnSnow) {
				const bool skinnedBlood = a_pass->geometry->GetGeometryRuntimeData().skinInstance != nullptr;
				if (!skinnedBlood || poolQuad)
					CaptureBloodDraw(a_pass, skinnedBlood);
			}
			return;
		}
	}
	// Animated flora never qualifies: card meshes shard under the skin.
	if (flags.all(Flag::kTreeAnim)) {
		LogCaptureVerdict(a_pass, "rejected: tree-anim flag");
		return;
	}
	// Skinned geometry never qualifies: with the family acceptance no
	// longer LOD-only, an "Ice"-prefixed actor mesh (ice wraith) would
	// otherwise capture and drag a static skin behind a moving creature.
	if (a_pass->geometry->GetGeometryRuntimeData().skinInstance != nullptr) {
		LogCaptureVerdict(a_pass, "rejected: skinned geometry");
		return;
	}
	// Ash, sand, moss: not snow to the recolor, so no skin, no caster and no
	// height-map entry either (Solstheim's ash rocks were shadowed by an
	// invisible skin).
	if (rec.mato == MatoClass::kNotSnow) {
		LogCaptureVerdict(a_pass, "rejected: MATO not snow");
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
	const uint32_t depthBiasMode = globals::game::shadowState->GetRuntimeData().rasterStateDepthBiasMode;
	const bool decalDepth = depthBiasMode != 0 || flags.any(Flag::kDecal, Flag::kDynamicDecal);
	// A snow-textured overlay (rock skirt) keeps its own fade: recolor only,
	// never a coat (SnowTexturedOverlay).
	const bool snowTexOverlay = settings.SnowTexturedRecolor && rec.pathSnowNamed && PassBlendsOver(a_pass);

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
				LogCaptureVerdict(a_pass, "rejected: containment (big, camera inside, no owning reference found)");
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
		// carry non-family texture paths. The MATO only vetoes (above);
		// requiring positive snow MATOs wrongly rejected glaciers, whose
		// snow is baked and needs no projection record.
		// The mountain/cliff family too, on snowy ground: LOD batches carry
		// the family's texture paths and no projection data.
		const auto& wbCenter = a_pass->geometry->worldBound.center;
		bool mountainFeature = rec.pathMountain || nameFacts.mountainCliff;
		if (mountainFeature) {
			// Two shared locks per call; a static's answer only moves with the
			// cell data, so it is kept on the record and stamped.
			const uint32_t depthVersion = snowDepthVersion.load(std::memory_order_acquire);
			if (rec.depthVersion != depthVersion || rec.depthX != wbCenter.x || rec.depthY != wbCenter.y) {
				rec.depth = GetNominalSnowDepthAt(wbCenter.x, wbCenter.y, 0.0f);
				rec.depthVersion = depthVersion;
				rec.depthX = wbCenter.x;
				rec.depthY = wbCenter.y;
			}
			mountainFeature = rec.depth > 0.5f;
		}
		const bool naturalFeature = rec.pathNatural || rec.iceName || mountainFeature;
		// Family accepts at any range, not just LOD: loaded glacier/ice meshes
		// carry no proj/snow flags when PBR glacier textures replace the
		// vanilla projected-snow setup, while their LOD counterparts capture
		// normally. LOD-only acceptance was the whole
		// bare-glacier bug). The MATO veto stands.
		// Plain object LOD passes whole: Lighting recolors every such batch by
		// texel brightness (SetProjectedSnowBit) and writes the weight back,
		// and the coat reads that weight and nothing else (the atlas holds
		// ships and walls beside the mountains).
		// Snow-named textures capture under the toggle: their skin coats off
		// the recolor's written weight.
		const bool snowNamedCapture = settings.SnowTexturedRecolor && rec.pathSnowNamed && !snowTexOverlay;
		if (!(rec.pathBase || naturalFeature || isObjectLOD || snowNamedCapture || rec.roadLayer)) {
			LogCaptureVerdict(a_pass, "rejected: no family signal at material gate (name/texture both missed)");
			return;
		}
	}


	// Twig-card shape class (branch piles, shore driftwood): vanilla flags
	// them snow-projected so they pass the flag gate, but the capture sees
	// sparse cards and the skin wraps them into broken shards. Name-matched
	// on the diffuse path; extend the list as offenders surface.
	if (rec.shard) {
		LogCaptureVerdict(a_pass, "rejected: twig-card shape class");
		return;
	}
	// Alpha-TESTED cards (grass, roots, leaves) never take a skin: the coat
	// over a card is a translucent plane wherever the card is transparent.
	// Seasons projects onto every geometry of a model, and season packs swap
	// in variants whose MATO projects onto their cards too, so the gate is on
	// the geometry, not on who projected. Decal-mode geometry keeps its
	// skin whatever its alpha: a rock skirt (test + blend) is a soft overlay
	// lying ON its trim, drawn in the game's decal depth mode, and its skin
	// in that same mode is what covers it wherever the trim's own skin loses
	// the depth fight to it. Cards stand off the surface and never draw as
	// decals. LOD atlases keep their coat; it reads the recolor's own weight.
	// Architecture surfaces (a thatch roof's fringe, plank ends) test alpha
	// too and are not cards; foliage textures under architecture still are.
	// Nor are a road model's strips: they lie on the road (rec.roadLayer).
	auto* alphaProperty = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
	const bool alphaTested = alphaProperty && alphaProperty->GetAlphaTesting();
	if (!lodBatch && !decalDepth && !rec.pathArchitecture && alphaTested && !rec.roadLayer) {
		LogCaptureVerdict(a_pass, "rejected: alpha-tested card");
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
	// Road-mesh model class: deterministic NAME + texture-path match. The
	// name check matters: road models are built from MULTIPLE trishapes
	// ('RoadChunk...:0', ':2'), and only some wear road textures; matching
	// textures alone splits one road across two depth settings, stacking a
	// second hovering shell.
	// "bridge" (name or diffuse) EXCLUDES the road class: a bridge mesh does
	// not split its road stones from its walls, so the road skin sheeted
	// the whole thing (Josef, 2026-09-12). Bridges are ordinary objects;
	// the RoadChunks laid over a deck keep their own road class.
	// The name alone is not enough: Whiterun's district meshes are named
	// WRMainRoad* and carry walls, roofs, stalls and the stream bed, so the
	// name counts only on a landscape-textured trishape (the road kit's
	// road01 / snow01 pieces) or a paving one (wrstonefloor, wrslate; not
	// wrroofslate), never on walls, planks, trims or plaster.
	const bool bridge = nameFacts.bridge || rec.roadTex == 2;
	bool road = !bridge && nameFacts.road && (rec.landscapeTex || rec.pavingTex);
	// Which signal decided it, for the road-classification log below.
	const char* roadVia = road ? "name" : (bridge ? "bridge excluded" : "no");
	// A road replacer's strips are the surface the game draws over its road
	// shape: left out, the road top sat up to 15 units under them and the
	// road's snow lost the depth test to the dirt (TMD Dirt Roads).
	if (!road && !bridge && rec.roadLayer) {
		road = true;
		roadVia = "road model layer";
	}
	if (!road && !bridge && rec.roadTex == 1) {
		road = true;
		roadVia = "texture";
	}
	// Cities: no road class at all. The paving is an ordinary object there
	// (skin, recolor, volume), like the walls and stairs beside it.
	if (road && GroundShellsSuspended()) {
		road = false;
		roadVia = "city";
	}
	// Off the road class a strip is an alpha-tested card again.
	if (rec.roadLayer && !road) {
		LogCaptureVerdict(a_pass, "rejected: road model layer outside the road class");
		return;
	}

	// Measured to the bound's nearest point, not the origin: a long wall or
	// a building's shape can sit inside the range with its origin outside it.
	const auto& translate = a_pass->geometry->world.translate;
	const auto& rangeBound = a_pass->geometry->worldBound;
	float dx = rangeBound.center.x - eye.x;
	float dy = rangeBound.center.y - eye.y;
	const float captureRange = SkinFadeEndUnits() + std::max(rangeBound.radius, 0.0f);
	// Road skins belong to the ground: no Object Snow range cap, no distance dissolve.
	if (!fadeExempt && !fullCoat && !road && dx * dx + dy * dy > captureRange * captureRange) {
		LogCaptureVerdict(a_pass, "rejected: range cap despite family signal (fadeExempt did not fire)");
		return;
	}

	// Object snow needs snow-capable ground near it, unless the object IS
	// the snow (ice family, drifts) or carries projected snow, which takes
	// the drape wherever it stands. Two cells of reach clipped to the
	// loaded square; a still-baking cell counts as snow.
	if (!fadeExempt && !fullCoat && !flags.any(Flag::kProjectedUV) && !GroundNearHasSnow(translate.x, translate.y)) {
		LogCaptureVerdict(a_pass, "rejected: no snow ground within two loaded cells");
		return;
	}

	// The same geometry renders through multiple passes; capture once.
	if (!capturedStaticsSet.insert(a_pass->geometry).second)
		return;
	LogCaptureVerdict(a_pass, fadeExempt ? "CAPTURED (fadeExempt)" : "CAPTURED (range-faded)");

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
		logger::info("[SNOW DEFORMATION] captured '{}' road={} (via {}) tex='{}'",
			a_pass->geometry->name.c_str(), road ? "yes" : "no", roadVia, diffusePath);
	}

	// Vanilla's projected-UV threshold, for the S0 mask view and the S2
	// placement suppressor (SKIN-PLACEMENT-PLAN.md). -1 = no projection data
	// on this draw. Tree-anim meshes are sentineled too: their vertex alpha
	// is wind weight, not a snow mask, and vanilla forces alpha 1 on them.
	float projThreshold = -1.0f;
	float projNoiseScale = 0.0f;
	float projNoiseTiling = 0.0f;
	float projNoiseWeight = 0.0f;
	bool projReal = false;
	{
		const auto& capFlags = a_pass->shaderProperty->flags;
		using CapFlag = RE::BSShaderProperty::EShaderPropertyFlag;
		if (capFlags.any(CapFlag::kProjectedUV) && !capFlags.any(CapFlag::kTreeAnim)) {
			const auto& projParams = static_cast<RE::BSLightingShaderProperty*>(a_pass->shaderProperty)->projectedUVParams;
			projThreshold = projParams.alpha;
			// = Lighting's ProjectedUVParams.x.
			projNoiseWeight = projParams.red * (1.0f - projParams.alpha);
			// Runtime projection (Seasons of Skyrim): the record's 0 stands
			// for "no angle"; the skin reconstructs with the angle Lighting
			// masks by, so the drape and the recolor agree.
			if (rec.seasonsProj)
				projThreshold = std::max(projThreshold, std::cos(std::clamp(settings.SeasonsSnowMaxAngle, 0.0f, 90.0f) * (DirectX::XM_PI / 180.0f)));
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

	const bool snowTex = settings.SnowTexturedRecolor && !projReal && !lodBatch && rec.pathSnowNamed && !snowTexOverlay;
	// Snow Follows Parallax: the height map Extended Materials marches for this
	// draw, read from the material as CS's TruePBR and the game bind it - the
	// displacement, or the env mask a complex material keeps its height in -
	// and the scale, channel and mip rule Lighting gives it. Vanilla parallax
	// shapes paint nothing; object LOD takes no parallax.
	winrt::com_ptr<ID3D11ShaderResourceView> parallaxSRV;
	float4 parallaxUV = {};
	float4 parallax = {};
	if (settings.SnowFollowsParallax && !lodBatch && globals::features::extendedMaterials.loaded) {
		const auto& em = globals::features::extendedMaterials.settings;
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		RE::NiSourceTexture* height = nullptr;
		// TruePBR's own test (TruePBR::BSLightingShader_SetupMaterial): the pass's
		// flag, not GetFeature(), which reports kDefault for PBR materials.
		using LightingTechnique = SIE::ShaderCache::LightingShaderTechniques;
		const auto technique = static_cast<LightingTechnique>(0x3F & (a_rawTechnique >> 24));
		const bool truePbr = (a_rawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::TruePbr)) != 0;
		if (material && truePbr && (technique == LightingTechnique::None || technique == LightingTechnique::TreeAnim)) {
			auto* pbr = static_cast<BSLightingShaderMaterialPBR*>(material);
			if (em.EnableParallax && pbr->displacementTexture != nullptr && pbr->displacementTexture != globals::game::graphicsState->GetRuntimeData().defaultTextureBlack &&
				!pbr->pbrFlags.any(PBRFlags::InterlayerParallax)) {
				height = pbr->displacementTexture.get();
				parallax = { pbr->GetDisplacementScale(), 0.0f, 2.0f, 1.0f };
			}
		} else if (material && !truePbr && technique == LightingTechnique::Envmap && material->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap &&
				   (a_rawTechnique & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::ModelSpaceNormals)) == 0) {
			auto* envmap = static_cast<RE::BSLightingShaderMaterialEnvmap*>(material);
			if (em.EnableComplexMaterial && envmap->envMaskTexture) {
				height = envmap->envMaskTexture.get();
				parallax = { 1.0f, 3.0f, 1.0f, 0.0f };
			}
		}
		if (height && height->rendererTexture && height->rendererTexture->resourceView) {
			parallaxSRV.copy_from(height->rendererTexture->resourceView);
			const uint32_t transform = globals::game::smState->textureTransformCurrentBuffer;
			parallaxUV = { material->texCoordOffset[transform].x, material->texCoordOffset[transform].y,
				material->texCoordScale[transform].x, material->texCoordScale[transform].y };
		}
	}
	const bool mayParallax = parallaxSRV != nullptr;
	capturedStatics.push_back({ RE::NiPointer<RE::BSGeometry>(a_pass->geometry), a_pass->geometry->world, road, fadeExempt || fullCoat || road, projThreshold, projNoiseScale, projNoiseTiling, projNoiseWeight, forceRounded, plankFamily, projReal, fullCoat, lodBatch, decalDepth, alphaTested, snowTex, rec.mato != MatoClass::kNotSnow, mayParallax });
	capturedStatics.back().parallaxSRV = std::move(parallaxSRV);
	capturedStatics.back().parallaxUV = parallaxUV;
	capturedStatics.back().parallax = parallax;
	capturedRoads += road ? 1u : 0u;
	capturedLoaded += lodBatch ? 0u : 1u;
	if (!lodBatch) {
		const auto& wb = a_pass->geometry->worldBound;
		coatReachBounds[0].push_back({ wb.center.x, wb.center.y, wb.radius });
	}
}

struct SD_BSLightingShader_SetupGeometry
{
	static void thunk(RE::BSLightingShader* shader, RE::BSRenderPass* a_pass, uint32_t a_flags)
	{
		auto& snowDeformation = globals::features::snowDeformation;
		if (snowDeformation.loaded) {
			snowDeformation.OnLightingDraw(a_pass);
			snowDeformation.SetProjectedSnowBit(shader, a_pass);
			snowDeformation.SetBodyCoatBit(a_pass);
		}

		func(shader, a_pass, a_flags);

		if (snowDeformation.loaded) {
			snowDeformation.BSLightingShader_SetupGeometry(a_pass, shader ? shader->currentRawTechnique : 0);
			snowDeformation.LandscapeSnowSetup(a_pass, true);
			snowDeformation.ProbeBodyCoatDraw(a_pass);
		}
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
ID3DBlob* SD_CompileShaderBlob(const wchar_t* a_path, const char* a_target, const char* a_stageDefine, const char* a_extraDefine, const char* a_extraDefine2, const char* a_extraDefine3, const char* a_extraDefine4, const char* a_extraDefine5, const char* a_extraDefine6)
{
	// Blob disk cache (see ShaderPrime.cpp): the fixed flag set below is part
	// of the "sdblob" env token, and the full key round-trips through the
	// stored file so a fingerprint or define change reads as a miss.
	auto& snow = globals::features::snowDeformation;
	// The fourth and fifth defines are appended only when present so every
	// existing cache key keeps its text.
	auto defs = std::format("{};{};{};{}", a_stageDefine,
		a_extraDefine ? a_extraDefine : "", a_extraDefine2 ? a_extraDefine2 : "", a_extraDefine3 ? a_extraDefine3 : "");
	if (a_extraDefine4)
		defs += std::format(";{}", a_extraDefine4);
	if (a_extraDefine5)
		defs += std::format(";{}", a_extraDefine5);
	if (a_extraDefine6)
		defs += std::format(";{}", a_extraDefine6);
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
		{ a_extraDefine5 ? a_extraDefine5 : "DX11", "" },
		{ a_extraDefine6 ? a_extraDefine6 : "DX11", "" },
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

bool SnowDeformation::RoadMapsWanted() const
{
	// Every drawn non-road skin is S4, which reads neither outside a debug
	// view; the probe readout prints the cone.
	return capturedRoads > 0 || staticsDebugView != 0 || globals::state->frameCount - heightProbeShownFrame < 3;
}

float2 SnowDeformation::PatchLatticeCentre() const
{
	return { std::floor(heightWindowCenter.x / kPatchSnap) * kPatchSnap, std::floor(heightWindowCenter.y / kPatchSnap) * kPatchSnap };
}

void SnowDeformation::FillPatchDrawCB(StaticsCB& a_scb) const
{
	// WorldRow0.xy = snapped patch CENTRE. The grid is warped (see
	// kPatchBandVerts in SnowStaticsShell.hlsl), so the vertex shader
	// places about the centre rather than stepping from a corner, and the
	// snap has to be the COARSEST band step - snapping to the fine step
	// would leave outer vertices off their own band's lattice and quad
	// widths would flip as the camera moves.
	const float2 centre = PatchLatticeCentre();
	a_scb.WorldRow0 = { centre.x, centre.y, 0.0f, 0.0f };
	// Objects carry no depth; roads carry theirs per texel in the skin-depth raster.
	a_scb.ObjectsDepth = 0.0f;
	a_scb.RoundedDepth = 0.0f;
	// Roads are ground: no Object Snow distance dissolve.
	a_scb.FadeExempt = 1.0f;
	a_scb.HeightWindowCenter = heightWindowCenter;
	a_scb.HeightHalfExtent = ObjectRasterHalfExtent();
	// The patch stays on the coarse maps: its pass does not bind t33/t34, and
	// a stale non-zero here would send PatchTop to an unbound texture, whose
	// zero reads as an object top at world Z 0.
	a_scb.FineHalfExtent = 0.0f;
	// The far road level (t40) IS bound by the patch pass and the caster.
	a_scb.FarWindowCenterX = farWindowCenter.x;
	a_scb.FarWindowCenterY = farWindowCenter.y;
	a_scb.FarHalfExtent = farHalfExtentLive;
	a_scb.FarRoadDepth = settings.RoadMeshesDepth;
	// The march's footprint test (t11 in the visible pass).
	a_scb.HasObjectTop = 1.0f;
	// The patch is road snow throughout; it reads the road bit per texel from
	// the raster's G channel, not from here.
	a_scb.RoadField = 1.0f;
}

uint32_t SnowDeformation::FillPatchCullLines(const ShellCB& a_cb, PatchCullCB& a_out) const
{
	a_out = {};
	// Inside where row . (p - cam, 1) >= 0, rewritten over absolute world p.
	const auto& m = a_cb.CameraViewProj;
	const double cam[3] = { a_cb.CameraPosAdjust.x, a_cb.CameraPosAdjust.y, a_cb.CameraPosAdjust.z };
	auto plane = [&](int a_row, double a_sign) {
		std::array<double, 4> p{};
		for (int c = 0; c < 4; c++)
			p[c] = double(m.m[3][c]) + a_sign * (a_row < 3 ? double(m.m[a_row][c]) : 0.0);
		p[3] -= p[0] * cam[0] + p[1] * cam[1] + p[2] * cam[2];
		return p;
	};
	const std::array<double, 4> planes[5] = { plane(0, 1.0), plane(0, -1.0), plane(1, 1.0), plane(1, -1.0), plane(3, 0.0) };
	uint32_t count = 0;
	auto addLine = [&](double a_x, double a_y, double a_d) {
		const double len = std::hypot(a_x, a_y);
		if (len < 1e-9 || count >= 6)
			return;
		// Slack over the shader's float rounding at world-sized coordinates.
		a_out.Lines[count++] = { float(a_x / len), float(a_y / len), float(a_d / len + 4.0), 0.0f };
	};
	// z eliminated (Fourier-Motzkin): each plane without z is a line as is,
	// and every pair bounding z from opposite sides gives one.
	for (const auto& p : planes)
		if (p[2] == 0.0)
			addLine(p[0], p[1], p[3]);
	for (const auto& lo : planes) {
		if (lo[2] <= 0.0)
			continue;
		for (const auto& hi : planes) {
			if (hi[2] >= 0.0)
				continue;
			addLine(lo[2] * hi[0] - hi[2] * lo[0], lo[2] * hi[1] - hi[2] * lo[1], lo[2] * hi[3] - hi[2] * lo[3]);
		}
	}
	a_out.LineCount = count;
	return count;
}

uint32_t SnowDeformation::PatchLatticeBands(const PatchCullCB& a_lines, float2 a_centre, PatchBand (&a_bands)[kPatchBands]) const
{
	// PatchOutsideFootprint over 8x8 blocks: a block's corner range bounds its
	// patches' (the warp is monotonic), and 8 units over the hull's 16 cover
	// its float corners. A patch outside every band would get zero factors.
	static constexpr uint32_t kBlock = 8;
	static constexpr uint32_t kBlocks = (kPatchGridDim + kBlock - 1) / kBlock;
	static constexpr double kMargin = 16.0 + 8.0;
	const auto warp = [](int a_u) {
		static constexpr double kBandVerts[5] = { 128.0, 16.0, 8.0, 4.0, 57.0 };
		static constexpr double kBandMul[5] = { 1.0, 2.0, 4.0, 8.0, 32.0 };
		double a = std::abs(double(a_u));
		double off = 0.0;
		for (int band = 0; band < 5; band++) {
			const double take = std::min(a, kBandVerts[band]);
			off += take * kBandMul[band];
			a -= take;
		}
		return (a_u < 0 ? -1.0 : 1.0) * (off + a * kBandMul[4]) * 8.0;
	};
	double edgeX[kBlocks + 1];
	double edgeY[kBlocks + 1];
	for (uint32_t b = 0; b <= kBlocks; b++) {
		const double w = warp(int(std::min(b * kBlock, kPatchGridDim)) - int(kPatchGridDim / 2));
		edgeX[b] = double(a_centre.x) + w;
		edgeY[b] = double(a_centre.y) + w;
	}
	// Per block row, the span of its kept blocks.
	uint32_t rowX0[kBlocks];
	uint32_t rowX1[kBlocks];
	uint32_t by0 = kBlocks, by1 = 0;
	for (uint32_t by = 0; by < kBlocks; by++) {
		rowX0[by] = kBlocks;
		rowX1[by] = 0;
		for (uint32_t bx = 0; bx < kBlocks; bx++) {
			const double lo[2] = { edgeX[bx] - kMargin, edgeY[by] - kMargin };
			const double hi[2] = { edgeX[bx + 1] + kMargin, edgeY[by + 1] + kMargin };
			bool outside = false;
			for (uint32_t i = 0; i < std::min(a_lines.LineCount, 6u) && !outside; i++) {
				const auto& l = a_lines.Lines[i];
				const double px = l.x > 0.0f ? hi[0] : lo[0];
				const double py = l.y > 0.0f ? hi[1] : lo[1];
				outside = double(l.x) * px + double(l.y) * py + double(l.z) < 0.0;
			}
			if (outside)
				continue;
			rowX0[by] = std::min(rowX0[by], bx);
			rowX1[by] = std::max(rowX1[by], bx + 1);
		}
		if (rowX0[by] < rowX1[by]) {
			by0 = std::min(by0, by);
			by1 = std::max(by1, by + 1);
		}
	}
	// Equal shares of the kept rows; a band spans the union of its rows' spans.
	uint32_t count = 0;
	for (uint32_t b = 0; b < kPatchBands && by0 < by1; b++) {
		const uint32_t r0 = by0 + (by1 - by0) * b / kPatchBands;
		const uint32_t r1 = by0 + (by1 - by0) * (b + 1) / kPatchBands;
		uint32_t x0 = kBlocks, x1 = 0;
		for (uint32_t by = r0; by < r1; by++) {
			x0 = std::min(x0, rowX0[by]);
			x1 = std::max(x1, rowX1[by]);
		}
		if (x0 >= x1)
			continue;
		a_bands[count++] = { x0 * kBlock, std::min(x1 * kBlock, kPatchGridDim), r0 * kBlock, std::min(r1 * kBlock, kPatchGridDim) };
	}
	return count;
}

ID3D11VertexShader* SnowDeformation::GetPatchShadowVS()
{
	// Lazy: the shadow pass runs before DrawShell has
	// had a chance to create the statics shaders on early frames.
	if (!patchShadowVS) {
		constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl";
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "PATCH", "SNOW_SHADOW_CAST", "SNOW_PATCH_INDEXED"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchShadowVS)))
				Util::SetResourceName(patchShadowVS, "SnowDeformation::TrenchPatchShadowVS");
		}
	}
	return patchShadowVS;
}

bool SnowDeformation::EnsurePatchCasterIB()
{
	if (patchCasterIB)
		return true;
	// Six indices per quad in the visible patch VS's corner order, so the
	// triangles and their order are the same. A triangle with a corner
	// past the caster's distance kill (SnowStaticsShell.hlsl
	// BuildPatchVertex) never rasterises, and lattice offsets from the
	// patch centre are fixed, so those are left out. No lattice vertex
	// lies within 4 units of the kill radius (~4063).
	constexpr uint32_t stride = kPatchGridDim + 1;
	static constexpr uint32_t kCorners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	const auto warp = [](int a_u) {
		static constexpr double kBandVerts[5] = { 128.0, 16.0, 8.0, 4.0, 57.0 };
		static constexpr double kBandMul[5] = { 1.0, 2.0, 4.0, 8.0, 32.0 };
		double a = std::abs(double(a_u));
		double off = 0.0;
		for (int band = 0; band < 5; band++) {
			const double take = std::min(a, kBandVerts[band]);
			off += take * kBandMul[band];
			a -= take;
		}
		return (off + a * kBandMul[4]) * 8.0;
	};
	std::vector<uint8_t> casts(size_t(stride) * stride);
	for (uint32_t y = 0; y < stride; y++) {
		for (uint32_t x = 0; x < stride; x++) {
			const double dist = std::hypot(warp(int(x) - int(kPatchGridDim / 2)), warp(int(y) - int(kPatchGridDim / 2)));
			const double t = std::clamp((dist - 2800.0) / 2100.0, 0.0, 1.0);
			casts[size_t(y) * stride + x] = (1.0 - t * t * (3.0 - 2.0 * t)) >= 0.35 - 1e-4;
		}
	}
	std::vector<uint32_t> indices;
	indices.reserve(size_t(kPatchGridDim) * kPatchGridDim * 6);
	for (uint32_t qy = 0; qy < kPatchGridDim; qy++) {
		for (uint32_t qx = 0; qx < kPatchGridDim; qx++) {
			for (uint32_t tri = 0; tri < 2; tri++) {
				uint32_t idx[3];
				bool keep = true;
				for (uint32_t k = 0; k < 3; k++) {
					const auto& c = kCorners[tri * 3 + k];
					idx[k] = (qy + c[1]) * stride + qx + c[0];
					keep = keep && casts[idx[k]];
				}
				if (keep)
					indices.insert(indices.end(), idx, idx + 3);
			}
		}
	}
	if (indices.empty())
		return false;
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
	D3D11_SUBRESOURCE_DATA data{ indices.data(), 0, 0 };
	if (FAILED(globals::d3d::device->CreateBuffer(&desc, &data, patchCasterIB.put())))
		return false;
	Util::SetResourceName(patchCasterIB.get(), "SnowDeformation::TrenchPatchCasterIB");
	patchCasterIndexCount = UINT(indices.size());
	return true;
}

bool SnowDeformation::EnsurePatchDeferredResources()
{
	if (!EnsureDeferredResources())
		return false;
	const auto& base = shellRasterDepth->desc;
	if (patchDeferredMark && (patchDeferredMark->desc.Width != base.Width || patchDeferredMark->desc.Height != base.Height)) {
		delete patchDeferredMark;
		patchDeferredMark = nullptr;
	}
	if (!patchDeferredMark) {
		D3D11_TEXTURE2D_DESC desc = base;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
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
		patchDeferredMark = new Texture2D(desc, "SnowDeformation::TrenchPatchDeferredMark");
		patchDeferredMark->CreateSRV(srvDesc);
		patchDeferredMark->CreateRTV(rtvDesc);
	}
	if (!patchDeferredDepthState) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = FALSE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		if (FAILED(globals::d3d::device->CreateDepthStencilState(&dsDesc, patchDeferredDepthState.put())))
			return false;
		Util::SetResourceName(patchDeferredDepthState.get(), "SnowDeformation::TrenchPatchDeferredDepthState");
	}
	return patchDeferredMark->srv && patchDeferredMark->rtv;
}

ID3D11PixelShader* SnowDeformation::CompilePatchDeferredPS(int a_which)
{
	static constexpr const char* kDefine[3] = { "SNOW_PATCH_ATTR", "SNOW_PATCH_DEFERRED", "SNOW_PATCH_DEFERRED" };
	static constexpr const char* kName[3] = { "SnowDeformation::TrenchPatchPS Attributes", "SnowDeformation::TrenchPatchPS Deferred", "SnowDeformation::TrenchPatchPS Condition View" };
	const char* ehfDefine = globals::features::exponentialHeightFog.loaded ? "SNOW_EXP_HEIGHT_FOG" : nullptr;
	const char* iblDefine = globals::features::ibl.loaded ? "SNOW_IBL" : nullptr;
	winrt::com_ptr<ID3DBlob> blob;
	blob.attach(SD_CompileShaderBlob(L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl", "ps_5_0", "PSHADER", "PATCH", ehfDefine, iblDefine, kDefine[a_which],
		a_which == 2 ? "SNOW_CONDITION_VIEW" : nullptr));
	ID3D11PixelShader* ps = nullptr;
	if (blob && SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ps)))
		Util::SetResourceName(ps, kName[a_which]);
	else
		logger::warn("[SNOW DEFORMATION] trench patch: the {} PS failed to compile, the patch draws forward", kDefine[a_which]);
	return ps;
}

void SnowDeformation::PrimePatchDeferred()
{
	for (int i = 0; i < 2; i++) {
		if (patchDeferredPS[i] || patchDeferredTried[i])
			continue;
		patchDeferredTried[i] = true;
		patchDeferredPS[i] = CompilePatchDeferredPS(i);
	}
}

bool SnowDeformation::PatchDeferredReady()
{
	for (int i = 0; i < 2; i++) {
		if (patchDeferredPS[i])
			continue;
		if (patchDeferredJob[i].valid()) {
			if (patchDeferredJob[i].wait_for(std::chrono::seconds(0)) == std::future_status::ready)
				patchDeferredPS[i] = patchDeferredJob[i].get();
		} else if (!patchDeferredTried[i]) {
			patchDeferredTried[i] = true;
			patchDeferredJob[i] = std::async(std::launch::async, [i] { return CompilePatchDeferredPS(i); });
		}
	}
	return patchDeferredPS[0] && patchDeferredPS[1];
}

bool SnowDeformation::PatchDeferredCompiling() const
{
	return patchDeferredJob[0].valid() || patchDeferredJob[1].valid();
}

ID3D11PixelShader* SnowDeformation::PatchConditionViewPS()
{
	if (debugConditionView <= 0)
		return nullptr;
	if (!patchConditionViewPS) {
		if (patchConditionViewJob.valid()) {
			if (patchConditionViewJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
				patchConditionViewPS = patchConditionViewJob.get();
		} else if (!patchConditionViewTried) {
			patchConditionViewTried = true;
			patchConditionViewJob = std::async(std::launch::async, [] { return CompilePatchDeferredPS(2); });
		}
	}
	return patchConditionViewPS;
}

void SnowDeformation::ReleasePatchDeferred()
{
	for (int i = 0; i < 2; i++) {
		// A compile in flight finishes first; its shader is dropped with the rest.
		if (patchDeferredJob[i].valid()) {
			if (auto* ps = patchDeferredJob[i].get())
				ps->Release();
		}
		if (patchDeferredPS[i])
			patchDeferredPS[i]->Release();
		patchDeferredPS[i] = nullptr;
		patchDeferredTried[i] = false;
	}
	if (patchConditionViewJob.valid()) {
		if (auto* ps = patchConditionViewJob.get())
			ps->Release();
	}
	if (patchConditionViewPS)
		patchConditionViewPS->Release();
	patchConditionViewPS = nullptr;
	patchConditionViewTried = false;
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

	// Drape twins of the pair above (SNOW_STATICS_DRAPE). A failure keeps
	// drapes on the general pair.
	if (!staticsPSDrapeNoDepth) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DRAPE"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSDrapeNoDepth)))
				Util::SetResourceName(staticsPSDrapeNoDepth, "SnowDeformation::StaticsShellPS Drape NoDepth");
		}
	}
	if (!staticsPSDrapePrepass) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DEPTH_PREPASS", "SNOW_STATICS_DRAPE"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSDrapePrepass)))
				Util::SetResourceName(staticsPSDrapePrepass, "SnowDeformation::StaticsShellPS Drape Prepass");
		}
	}
	// The drapes' print twin (SNOW_OBJECT_PRINTS), for draws over prints. A
	// failure keeps them on the drape twin.
	if (!staticsPSDrapePrintsNoDepth) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DRAPE", "SNOW_OBJECT_PRINTS"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSDrapePrintsNoDepth)))
				Util::SetResourceName(staticsPSDrapePrintsNoDepth, "SnowDeformation::StaticsShellPS Drape Prints NoDepth");
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
	if (!heightPSTopOnly) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "ps_5_0", "PSHADER", "TOP_ONLY"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightPSTopOnly)))
				Util::SetResourceName(heightPSTopOnly, "SnowDeformation::HeightCapturePS TopOnly");
		}
	}
	if (!heightPSSkinDepth) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(heightPath, "ps_5_0", "PSHADER", "SKIN_DEPTH"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &heightPSSkinDepth)))
				Util::SetResourceName(heightPSSkinDepth, "SnowDeformation::HeightCapturePS SkinDepth");
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
	if (!heightScrollTopCS)
		heightScrollTopCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, { { "SCROLL_TOP_ONLY", "" } }, "cs_5_0", "ScrollCS"));
	if (!heightScrollRasterCS)
		heightScrollRasterCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, { { "SCROLL_RASTER_RG", "" } }, "cs_5_0", "ScrollCS"));
	if (!heightCombineCS)
		heightCombineCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "CombineCS"));
	if (!heightWaterCoverCS)
		heightWaterCoverCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "WaterCoverCS"));
	if (!objectConeSeedCS)
		objectConeSeedCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectConeSeedCS"));
	if (!objectConeCS)
		objectConeCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectConeCS"));
	if (!objectSkyOpenCS)
		objectSkyOpenCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectSkyOpenCS"));
	if (!objectConeDiffuseCS)
		objectConeDiffuseCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ObjectConeDiffuseCS"));
	if (!objectConeFusedCS)
		objectConeFusedCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(processPath, {}, "cs_5_0", "ConeFusedCS"));

	if (!staticsVS || !staticsPS || !heightVS || !heightPS || !heightPSSkinDepth || !heightScrollCS || !heightScrollRasterCS || !heightCombineCS) {
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
	{
		D3D11_TEXTURE2D_DESC rasterDesc = heightDesc;
		rasterDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		rasterDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SHADER_RESOURCE_VIEW_DESC rasterSrvDesc = heightSrvDesc;
		rasterSrvDesc.Format = rasterDesc.Format;
		D3D11_RENDER_TARGET_VIEW_DESC rasterRtvDesc = heightRtvDesc;
		rasterRtvDesc.Format = rasterDesc.Format;
		heightRaster = new Texture2D(rasterDesc, "SnowDeformation::HeightRaster");
		heightRaster->CreateSRV(rasterSrvDesc);
		heightRaster->CreateRTV(rasterRtvDesc);
	}
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
	objectSnowCone = makeHeightTexture("SnowDeformation::ObjectSnowCone");
	// The peeled second layer: the layer-1 cone seed's plane-continuation test.
	heightTop2Raw[0] = makeHeightTexture("SnowDeformation::HeightTop2Raw0");
	heightTop2Raw[1] = makeHeightTexture("SnowDeformation::HeightTop2Raw1");
	// Near clipmap level 0: same grid, quarter reach, so one texel is one
	// world unit. The top alone: no ghost pair, and no cone (roads read the
	// coarse one).
	heightTopRawFine = makeHeightTexture("SnowDeformation::HeightTopRawFine");
	// Far road level: the road captures alone over the loaded-cell square.
	// Same grid, so a texel is 12 units at uGridsToLoad 5 and 16 at 7.
	heightTopRawFar = makeHeightTexture("SnowDeformation::HeightTopRawFar");
	// The tile grid below starts from the sky-openness field's shape (P3: half
	// the raster's resolution, compute-written; made on Volume Snow's first run).
	D3D11_TEXTURE2D_DESC openDesc = heightDesc;
	openDesc.Width = kHeightMapDim / 2;
	openDesc.Height = kHeightMapDim / 2;
	openDesc.Format = DXGI_FORMAT_R8_UNORM;
	openDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	static_assert(kHeightMapDim % 8 == 0, "HeightMapProcessCS clamps tiles to dims / 8");
	D3D11_TEXTURE2D_DESC tileDesc = openDesc;
	tileDesc.Width = kHeightMapDim / 8;
	tileDesc.Height = kHeightMapDim / 8;
	tileDesc.Format = DXGI_FORMAT_R8_UINT;
	D3D11_SHADER_RESOURCE_VIEW_DESC tileSrvDesc = heightSrvDesc;
	tileSrvDesc.Format = tileDesc.Format;
	D3D11_UNORDERED_ACCESS_VIEW_DESC tileUavDesc = heightUavDesc;
	tileUavDesc.Format = tileDesc.Format;
	heightTileEmpty = new Texture2D(tileDesc, "SnowDeformation::HeightTileEmpty");
	heightTileEmpty->CreateSRV(tileSrvDesc);
	heightTileEmpty->CreateUAV(tileUavDesc);

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

// Per compared target: the count, then the first differing texels as texel,
// kept .xy, fresh .xy (RasterCompareCS).
static constexpr UINT kHeightVerifySamples = 8;
static constexpr UINT kHeightVerifySlotWords = 4 + kHeightVerifySamples * 8;
static_assert(kHeightVerifySlotWords * sizeof(uint32_t) % D3D11_RAW_UAV_SRV_BYTE_ALIGNMENT == 0);
// Layer-1 raster, skin depth, fine, far (each with a scratch twin), then the
// object and road cones (against coneVerifyTex) and the shelter mask.
static constexpr UINT kHeightVerifySlots = 7;
static constexpr UINT kHeightVerifyRasterSlots = 4;
// The verifiedLevels bit each target's compare sets.
static constexpr uint32_t kHeightVerifyTargetLevel[kHeightVerifySlots] = { 0, 0, 1, 2, 3, 4, 5 };

bool SnowDeformation::EnsureHeightRasterVerify()
{
	if (heightRasterVerifyStaging[2] && heightRasterCompareCS)
		return true;
	auto device = globals::d3d::device;
	if (!heightRasterCompareCS)
		heightRasterCompareCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\HeightMapProcessCS.hlsl", { { "RASTER_COMPARE", "" } }, "cs_5_0", "RasterCompareCS"));
	if (!heightRasterCompareCS || !heightRaster || !heightSkinDepth || !heightTopRawFine || !heightTopRawFar)
		return false;
	Texture2D* const twins[kHeightVerifyRasterSlots] = { heightRaster, heightSkinDepth, heightTopRawFine, heightTopRawFar };
	static constexpr const char* kNames[kHeightVerifyRasterSlots] = { "SnowDeformation::HeightVerifyRaster", "SnowDeformation::HeightVerifySkinDepth",
		"SnowDeformation::HeightVerifyFine", "SnowDeformation::HeightVerifyFar" };
	for (UINT i = 0; i < kHeightVerifyRasterSlots; i++) {
		if (heightRasterVerifyTex[i])
			continue;
		D3D11_TEXTURE2D_DESC desc = twins[i]->desc;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } };
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { .Format = desc.Format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } };
		heightRasterVerifyTex[i] = new Texture2D(desc, kNames[i]);
		heightRasterVerifyTex[i]->CreateSRV(srvDesc);
		heightRasterVerifyTex[i]->CreateRTV(rtvDesc);
	}
	if (!heightRasterVerifyCount) {
		// A raw view starts on a 16-byte boundary; each slot is a multiple of 16 bytes.
		D3D11_BUFFER_DESC countDesc{};
		countDesc.ByteWidth = kHeightVerifySlots * kHeightVerifySlotWords * sizeof(uint32_t);
		countDesc.Usage = D3D11_USAGE_DEFAULT;
		countDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		countDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		winrt::com_ptr<ID3D11Buffer> count;
		winrt::com_ptr<ID3D11UnorderedAccessView> allUAV, slotUAVs[kHeightVerifySlots];
		if (FAILED(device->CreateBuffer(&countDesc, nullptr, count.put())))
			return false;
		Util::SetResourceName(count.get(), "SnowDeformation::HeightVerifyCount");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		uavDesc.Buffer.NumElements = kHeightVerifySlots * kHeightVerifySlotWords;
		if (FAILED(device->CreateUnorderedAccessView(count.get(), &uavDesc, allUAV.put())))
			return false;
		uavDesc.Buffer.NumElements = kHeightVerifySlotWords;
		for (UINT slot = 0; slot < kHeightVerifySlots; slot++) {
			uavDesc.Buffer.FirstElement = slot * kHeightVerifySlotWords;
			if (FAILED(device->CreateUnorderedAccessView(count.get(), &uavDesc, slotUAVs[slot].put())))
				return false;
		}
		Util::SetResourceName(count.get(), "SnowDeformation::HeightVerifyCount");
		heightRasterVerifyCount = count;
		heightRasterVerifyAllUAV = allUAV;
		for (UINT slot = 0; slot < kHeightVerifySlots; slot++)
			heightRasterVerifySlotUAV[slot] = slotUAVs[slot];
	}
	for (int i = 0; i < 3; i++) {
		if (heightRasterVerifyStaging[i])
			continue;
		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = kHeightVerifySlots * kHeightVerifySlotWords * sizeof(uint32_t);
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, heightRasterVerifyStaging[i].put())))
			return false;
		Util::SetResourceName(heightRasterVerifyStaging[i].get(), "SnowDeformation::HeightVerifyStaging");
	}
	return true;
}

bool SnowDeformation::EnsureConeVerify()
{
	if (coneVerifyTex)
		return true;
	if (!objectSnowCone)
		return false;
	D3D11_TEXTURE2D_DESC desc = objectSnowCone->desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } };
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = { .Format = desc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } };
	coneVerifyTex = new Texture2D(desc, "SnowDeformation::ConeVerify");
	coneVerifyTex->CreateSRV(srvDesc);
	coneVerifyTex->CreateUAV(uavDesc);
	return true;
}

bool SnowDeformation::EnsureCombineVerify()
{
	if (combineVerifyTex)
		return true;
	if (!heightBottomFiltered)
		return false;
	D3D11_TEXTURE2D_DESC desc = heightBottomFiltered->desc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } };
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = { .Format = desc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } };
	combineVerifyTex = new Texture2D(desc, "SnowDeformation::ShelterMaskVerify");
	combineVerifyTex->CreateSRV(srvDesc);
	combineVerifyTex->CreateUAV(uavDesc);
	return true;
}

bool SnowDeformation::EnsurePatchVerify(ID3D11DepthStencilView* a_dsv)
{
	if (!a_dsv || !shellDeferredAttr[0] || !shellDeferredAttr[1] || !shellDeferredAttr[2] || !patchDeferredMark)
		return false;
	auto device = globals::d3d::device;
	if (!heightRasterCompareCS)
		heightRasterCompareCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\HeightMapProcessCS.hlsl", { { "RASTER_COMPARE", "" } }, "cs_5_0", "RasterCompareCS"));
	if (!heightRasterCompareCS)
		return false;
	winrt::com_ptr<ID3D11Resource> depthResource;
	a_dsv->GetResource(depthResource.put());
	auto depthTexture = depthResource.try_as<ID3D11Texture2D>();
	if (!depthTexture)
		return false;
	D3D11_TEXTURE2D_DESC depthDesc{};
	depthTexture->GetDesc(&depthDesc);
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
	a_dsv->GetDesc(&dsvDesc);
	if (!(depthDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || dsvDesc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D)
		return false;
	DXGI_FORMAT depthSRVFormat = DXGI_FORMAT_UNKNOWN;
	switch (dsvDesc.Format) {
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		depthSRVFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		break;
	case DXGI_FORMAT_D32_FLOAT:
		depthSRVFormat = DXGI_FORMAT_R32_FLOAT;
		break;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		depthSRVFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		break;
	default:
		return false;
	}
	const D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc = { .Format = depthSRVFormat, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } };
	if (patchVerifyRealDepth != depthTexture.get()) {
		patchVerifyRealDepthSRV = nullptr;
		patchVerifyRealDepth = nullptr;
		if (FAILED(device->CreateShaderResourceView(depthTexture.get(), &depthSRVDesc, patchVerifyRealDepthSRV.put())))
			return false;
		patchVerifyRealDepth = depthTexture.get();
	}
	if (patchVerifyDepth) {
		D3D11_TEXTURE2D_DESC have{};
		patchVerifyDepth->GetDesc(&have);
		if (have.Width != depthDesc.Width || have.Height != depthDesc.Height || have.Format != depthDesc.Format) {
			patchVerifyDepth = nullptr;
			patchVerifyDSV = nullptr;
			patchVerifyDepthSRV = nullptr;
		}
	}
	if (!patchVerifyDepth) {
		D3D11_TEXTURE2D_DESC copyDesc = depthDesc;
		copyDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.MiscFlags = 0;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, patchVerifyDepth.put())) ||
			FAILED(device->CreateDepthStencilView(patchVerifyDepth.get(), &dsvDesc, patchVerifyDSV.put())) ||
			FAILED(device->CreateShaderResourceView(patchVerifyDepth.get(), &depthSRVDesc, patchVerifyDepthSRV.put())))
			return false;
		Util::SetResourceName(patchVerifyDepth.get(), "SnowDeformation::TrenchPatchVerifyDepth");
	}
	Texture2D* const reals[4] = { shellDeferredAttr[0], shellDeferredAttr[1], shellDeferredAttr[2], patchDeferredMark };
	static constexpr const char* kNames[4] = { "SnowDeformation::TrenchPatchVerifyAttr0", "SnowDeformation::TrenchPatchVerifyAttr1",
		"SnowDeformation::TrenchPatchVerifyAttr2", "SnowDeformation::TrenchPatchVerifyMark" };
	for (uint32_t i = 0; i < 4; i++) {
		if (patchVerifyRT[i] && (patchVerifyRT[i]->desc.Width != reals[i]->desc.Width || patchVerifyRT[i]->desc.Height != reals[i]->desc.Height)) {
			delete patchVerifyRT[i];
			patchVerifyRT[i] = nullptr;
		}
		if (patchVerifyRT[i])
			continue;
		D3D11_TEXTURE2D_DESC desc = reals[i]->desc;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		const D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { .Format = desc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } };
		const D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { .Format = desc.Format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } };
		patchVerifyRT[i] = new Texture2D(desc, kNames[i]);
		patchVerifyRT[i]->CreateSRV(srvDesc);
		patchVerifyRT[i]->CreateRTV(rtvDesc);
	}
	if (!patchVerifyCount) {
		D3D11_BUFFER_DESC countDesc{};
		countDesc.ByteWidth = 5 * kHeightVerifySlotWords * sizeof(uint32_t);
		countDesc.Usage = D3D11_USAGE_DEFAULT;
		countDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		countDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		winrt::com_ptr<ID3D11Buffer> count;
		if (FAILED(device->CreateBuffer(&countDesc, nullptr, count.put())))
			return false;
		Util::SetResourceName(count.get(), "SnowDeformation::TrenchPatchVerifyCount");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		uavDesc.Buffer.NumElements = 5 * kHeightVerifySlotWords;
		winrt::com_ptr<ID3D11UnorderedAccessView> allUAV;
		if (FAILED(device->CreateUnorderedAccessView(count.get(), &uavDesc, allUAV.put())))
			return false;
		uavDesc.Buffer.NumElements = kHeightVerifySlotWords;
		for (UINT slot = 0; slot < 5; slot++) {
			uavDesc.Buffer.FirstElement = slot * kHeightVerifySlotWords;
			if (FAILED(device->CreateUnorderedAccessView(count.get(), &uavDesc, patchVerifySlotUAV[slot].put())))
				return false;
		}
		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = countDesc.ByteWidth;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& staging : patchVerifyStaging) {
			if (FAILED(device->CreateBuffer(&stagingDesc, nullptr, staging.put())))
				return false;
		}
		Util::SetResourceName(count.get(), "SnowDeformation::TrenchPatchVerifyCount");
		patchVerifyCount = count;
		patchVerifyAllUAV = allUAV;
	}
	return true;
}

bool SnowDeformation::EnsureStaticsRecordCB(uint32_t a_count)
{
	auto* device = globals::d3d::device;
	if (!staticsRecordChecked) {
		staticsRecordChecked = true;
		static_assert(sizeof(StaticsCB) <= kStaticsRecordStride);
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
	}
	if (!staticsContext1 || !device)
		return false;
	if (a_count <= staticsRecordCapacity)
		return true;
	uint32_t capacity = std::max(staticsRecordCapacity, kStaticsRecordStart);
	while (capacity < a_count)
		capacity *= 2;
	if (staticsRecordRefused && capacity >= staticsRecordRefused)
		return false;
	winrt::com_ptr<ID3D11Buffer> buffers[2];
	for (int i = 0; i < 2; i++) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = capacity * kStaticsRecordStride;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&desc, nullptr, buffers[i].put()))) {
			staticsRecordRefused = capacity;
			if (!staticsRecordCapacity) {
				logger::info("[SNOW DEFORMATION] statics record buffer: creation failed; per-draw updates stay");
				staticsContext1 = nullptr;
			} else {
				logger::info("[SNOW DEFORMATION] statics record buffer: {} records refused; larger frames update per draw", capacity);
			}
			return false;
		}
		Util::SetResourceName(buffers[i].get(), i ? "SnowDeformation::StaticsRecords1" : "SnowDeformation::StaticsRecords0");
	}
	staticsRecordCB[0] = buffers[0];
	staticsRecordCB[1] = buffers[1];
	staticsRecordCapacity = capacity;
	logger::info("[SNOW DEFORMATION] statics record buffer: {} records x {} bytes, two copies", capacity, kStaticsRecordStride);
	return true;
}

bool SnowDeformation::UploadStaticsRecords(const StaticsCB* a_records, uint32_t a_count, const uint8_t* a_needed)
{
	if (a_count == 0)
		return false;
	if (a_count > (staticsRecordGrowOff ? kStaticsRecordStart : kStaticsRecordMax) || !EnsureStaticsRecordCB(a_count)) {
		staticsRecordFallbacks++;
		return false;
	}
	auto* context = globals::d3d::context;
	for (int i = 0; i < 2; i++) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(staticsRecordCB[i].get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		auto* dst = static_cast<uint8_t*>(mapped.pData);
		for (uint32_t r = 0; r < a_count; r++)
			if (!a_needed || a_needed[r])
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

void SnowDeformation::SkipObjectHeightMap()
{
	// The next run clears and rebuilds, as after a teleport.
	heightMapValid = false;
	globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Height Maps");
	globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
}

bool SnowDeformation::HeightMapIdleReady() const
{
	return !heightGhostsLive && heightMapValid && objectRasterHalfExtentBuilt == ObjectRasterHalfExtent();
}

void SnowDeformation::IdleObjectHeightMap()
{
	// A run would scroll sentinels into sentinels; the window, the walk's
	// cadence and its jump test advance exactly as a run's.
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float halfExtent = ObjectRasterHalfExtent();
	const float texel = halfExtent * 2.0f / kHeightMapDim;
	const float2 newCenter = {
		std::floor(eye.x / texel) * texel,
		std::floor(eye.y / texel) * texel
	};
	const int scrollX = (int)std::lround((newCenter.x - heightWindowCenter.x) / texel);
	const int scrollY = (int)std::lround((newCenter.y - heightWindowCenter.y) / texel);
	heightWindowCenter = newCenter;
	const bool windowJumped = std::abs(scrollX) > kGatherJumpTexels || std::abs(scrollY) > kGatherJumpTexels;
	if (windowJumped || (doorRefreshCounter++ % 60) == 0)
		StartExclusionGather();
	{
		ScopedTicks _gatherTicks(cpuCensus.spanTicks[CpuCensus::kSpanExclusionGather], cpuCensus.spanCalls[CpuCensus::kSpanExclusionGather]);
		StepExclusionGather();
	}
	heightIdleFrames++;
	globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Height Maps");
	globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
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
	processData.SlopePerUnit = kObjectConeSlope;
	constexpr float shellCellSize = kShellVertexSpacing * kShellTexelsPerCell;
	processData.TerrainWindowOrigin = { shellWindowCellX * shellCellSize, shellWindowCellY * shellCellSize };
	processData.TerrainTexelSize = kShellVertexSpacing;
	processData.TerrainDim = kShellWindowDim;
	processData.GhostDecay = 0.5f;
	processData.RimStep = kRimStep;
	processData.OverheadIgnore = kOverheadIgnore;
	processData.DiffuseLambda = kDiffuseLambda;
	processData.ShelterMaxHeight = std::clamp(settings.ShelterMaxHeight, 100.0f, 2000.0f);
	processData.ShelterBandHeight = std::max(kShelterBandHeight, processData.ShelterMaxHeight);
	processData.ShelterConeTan = std::tan(std::clamp(settings.ShelterDriftAngle, 5.0f, 70.0f) * (DirectX::XM_PI / 180.0f));
	processData.DeadGhostTiles = deadGhostTilesOff ? 0u : 1u;
	heightProcessCB->Update(processData);
	heightWindowCenter = newCenter;
	heightMapValid = true;
	// The layer-1 draws below re-extend it.
	if (processData.ClearAll || heightGhostBox.x > newCenter.x + halfExtent || heightGhostBox.z < newCenter.x - halfExtent ||
		heightGhostBox.y > newCenter.y + halfExtent || heightGhostBox.w < newCenter.y - halfExtent)
		heightGhostsLive = false;

	// Exclusion zones. Static sources (campfires, heat sources, dropped
	// torches, workspaces, sealed containers) come from a walk over every
	// loaded reference: started once a second and at once on a window jump
	// (load, teleport, reach change), spread over the following frames a
	// few cells at a time, swapped in whole when it ends. Heat sources come
	// from Survival Mode's warm-up-objects formlist when the plugin is
	// present, plus model-path matching that also covers non-Survival
	// installs.
	const bool windowJumped = processData.ClearAll != 0 ||
	                          std::abs(processData.ScrollDelta.x) > kGatherJumpTexels || std::abs(processData.ScrollDelta.y) > kGatherJumpTexels;
	if (windowJumped || (doorRefreshCounter++ % 60) == 0)
		StartExclusionGather();
	{
		ScopedTicks _gatherTicks(cpuCensus.spanTicks[CpuCensus::kSpanExclusionGather], cpuCensus.spanCalls[CpuCensus::kSpanExclusionGather]);
		StepExclusionGather();
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
		{
			uint64_t h = 1469598103934665603ull;
			auto mix = [&](const void* a_p, size_t a_n) {
				const auto* b = static_cast<const uint8_t*>(a_p);
				for (size_t i = 0; i < a_n; i++) {
					h ^= b[i];
					h *= 1099511628211ull;
				}
			};
			mix(exclusionData.PosRadius, sizeof(float4) * exclusionCount);
			mix(exclusionData.DirExtType, sizeof(float4) * exclusionCount);
			mix(&exclusionCount, sizeof(exclusionCount));
			if (h != exclusionListHash || !doorsCBUploaded) {
				doorsCB->Update(exclusionData);
				exclusionListBuffer->Update(&exclusionData, sizeof(float4) * kMaxExclusions * 2);
				doorsCBUploaded = true;
			}
			exclusionListHash = h;
		}
		// Every frame, never on a cadence (the window follows the camera);
		// the bake itself skips while its inputs match.
		RenderExclusionField(exclusionData);
	}

	// A capture wholly outside a window rasterizes to nothing there; the list
	// reaches the object-snow range, far past the raster windows.
	auto outsideWindow = [&](const RE::BSGeometry* a_geometry, float a_halfExtent) {
		const auto& wb = a_geometry->worldBound;
		return std::abs(wb.center.x - heightWindowCenter.x) > a_halfExtent + wb.radius ||
		       std::abs(wb.center.y - heightWindowCenter.y) > a_halfExtent + wb.radius;
	};
	// LOD batches never rasterize: with none of the rest in the window and no ghost, the merges
	// scroll sentinels into sentinels and the cone seeds zero, so the last such run's maps stand.
	const bool emptyWindow = !heightGhostsLive && std::ranges::none_of(capturedStatics, [&](const auto& a_cap) {
		return a_cap.geometry && !a_cap.lodBatch && !outsideWindow(a_cap.geometry.get(), halfExtent);
	});
	const bool keepEmpty = emptyWindow && heightMapsEmpty && !processData.ClearAll && !heightRasterCacheOff && !coneVerify;

	uint previous = heightCurrent;
	if (!keepEmpty)
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
	// Null bottoms = the top-only variant. a_tiles (bottoms only) takes the
	// shelter mask's per-tile flags. With a_raster (bottoms only) the raster
	// is read there (t6, heightRaster) and the current maps take the merge alone;
	// without, it is merged in place.
	auto mergeGhost = [&](Texture2D* a_prevTop, Texture2D* a_curTop, Texture2D* a_prevBottom, Texture2D* a_curBottom, Texture2D* a_tiles,
						  Texture2D* a_raster = nullptr) {
		if (!a_prevTop || !a_curTop)
			return;
		const bool bottoms = a_prevBottom && a_curBottom;
		if (!bottoms && !heightScrollTopCS)
			return;
		const bool fromRaster = bottoms && a_raster;
		ID3D11ShaderResourceView* mergeSRVs[2] = { a_prevTop->srv.get(), bottoms ? a_prevBottom->srv.get() : nullptr };
		ID3D11UnorderedAccessView* mergeUAVs[2] = { a_curTop->uav.get(), bottoms ? a_curBottom->uav.get() : nullptr };
		ID3D11UnorderedAccessView* tileUAV = bottoms && a_tiles ? a_tiles->uav.get() : nullptr;
		context->CSSetConstantBuffers(0, 1, &processCB);
		context->CSSetConstantBuffers(2, 1, &ghostCB);
		context->CSSetShaderResources(0, 2, mergeSRVs);
		if (fromRaster) {
			ID3D11ShaderResourceView* rasterSRV = a_raster->srv.get();
			context->CSSetShaderResources(6, 1, &rasterSRV);
		}
		context->CSSetUnorderedAccessViews(0, 2, mergeUAVs, nullptr);
		context->CSSetUnorderedAccessViews(3, 1, &tileUAV, nullptr);
		context->CSSetShader(fromRaster ? heightScrollRasterCS : (bottoms ? heightScrollCS : heightScrollTopCS), nullptr, 0);
		context->Dispatch((kHeightMapDim + 7) / 8, (kHeightMapDim + 7) / 8, 1);
		if (fromRaster)
			context->CSSetShaderResources(6, 1, nullCsSRVs);
		context->CSSetShaderResources(0, 2, nullCsSRVs);
		context->CSSetUnorderedAccessViews(0, 2, nullCsUAVs, nullptr);
		context->CSSetUnorderedAccessViews(3, 1, nullCsUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullMergeCB = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullMergeCB);
		context->CSSetConstantBuffers(2, 1, &nullMergeCB);
	};

	// Every raster starts empty, including the peeled layers: the sentinel
	// is what tells the merge which texels this frame's capture owns. The
	// layer-1 merge writes every texel of heightTopRaw/heightBottomRaw.
	// Levels kept from an earlier run (heightRasterKey) skip their clears.
	const float topClear[4] = { kHeightMapEmptyTop, 0.0f, 0.0f, 0.0f };
	const float rasterClear[4] = { kHeightMapEmptyTop, -kHeightMapEmptyBottom, 0.0f, 0.0f };
	if (heightTop2Raw[heightCurrent] && !keepEmpty)
		context->ClearRenderTargetView(heightTop2Raw[heightCurrent]->rtv.get(), topClear);

	// G = road top, so it clears to the no-road sentinel, not to zero: zero is
	// a legal world Z and would read as a road at sea level.
	const float skinDepthClear[4] = { 0.0f, kNoRoadTop, 0.0f, 0.0f };
	// Only road records write the skin depth anything but its clear
	// (SnowHeightCapture), so its pass draws the roads alone.
	const bool roadRaster = capturedRoads > 0;
	context->OMSetBlendState(heightRasterBlendState.get(), nullptr, 0xFFFFFFFF);

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
	const bool roadMaps = RoadMapsWanted();
	const bool fineLevel = roadMaps && FineLevelOn() && heightTopRawFine;
	const uint32_t recordBlocks = fineLevel ? 3u : 2u;
	// Far road level: the road captures alone against the loaded-cell square,
	// one block each, appended after the per-capture blocks. The window snaps
	// to its own texel; one cell of slack past the square's half-width reaches
	// its far edge from anywhere in the camera's cell.
	// Its readers (patch, patch caster, road skins) are road draws only.
	const bool farLevel = roadMaps && FarLevelActive();
	farHalfExtentLive = 0.0f;
	std::vector<uint32_t> farRecordIndex(captureCount, ~0u);
	uint32_t farCount = 0;
	if (farLevel) {
		int sqMinX, sqMinY, sqMaxX, sqMaxY;
		float halfCells = 2.0f;
		if (LoadedCellSquare(sqMinX, sqMinY, sqMaxX, sqMaxY))
			halfCells = float(sqMaxX - sqMinX) * 0.5f;
		const float farHalf = std::min((halfCells + 1.0f) * 4096.0f, kFarPatchReach);
		const float farTexel = farHalf * 2.0f / kHeightMapDim;
		const auto farEye = globals::game::frameBufferCached.GetCameraPosAdjust();
		farWindowCenter = { std::floor(farEye.x / farTexel) * farTexel, std::floor(farEye.y / farTexel) * farTexel };
		farHalfExtentLive = farHalf;
		for (uint32_t ci = 0; ci < captureCount; ci++)
			if (capturedStatics[ci].road && !capturedStatics[ci].lodBatch)
				farRecordIndex[ci] = farCount++;
	}
	const uint32_t farRecordBase = captureCount * recordBlocks;
	// Kept across frames: a fresh vector of this size (MBs at thousands of
	// captures) paid its allocation and page faults on top of the zero fill.
	static std::vector<StaticsCB> sCaptureRecords;
	sCaptureRecords.assign(size_t(farRecordBase) + farCount, StaticsCB{});
	auto& captureRecords = sCaptureRecords;
	// The records a draw below binds: the loops' own tests. The volume binds every coarse record; far records are all kept.
	std::vector<uint8_t> recordNeeded(captureRecords.size(), 0);
	// Read once: a volume the mask did not cover takes the per-draw update path.
	const bool volumeRecords = settings.VolumeSnow;
	if (volumeRecords)
		std::fill_n(recordNeeded.begin(), captureCount, uint8_t(1));
	std::fill(recordNeeded.begin() + farRecordBase, recordNeeded.end(), uint8_t(1));
	// The largest depth the skin-depth raster can hold this frame: only road
	// records write one (SnowHeightCapture), MAX-blended from a zero clear.
	float maxSeedDepth = 0.0f;
	// Entries used this frame outlive the pass (EnsureSmoothedNormals never evicts them).
	std::vector<ID3D11ShaderResourceView*> captureSmoothSRVs(captureCount, nullptr);
	for (uint32_t ci = 0; ci < captureCount; ci++) {
		auto& cap = capturedStatics[ci];
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

		const bool inWindow = !outsideWindow(geometry, halfExtent);
		recordNeeded[ci] |= uint8_t(inWindow);
		recordNeeded[size_t(captureCount) + ci] = uint8_t(inWindow);
		if (fineLevel)
			recordNeeded[size_t(captureCount) * 2 + ci] = uint8_t(!outsideWindow(geometry, FineRasterHalfExtent()));

		const auto& rot = cap.world.rotate;
		const float scale = cap.world.scale;
		const float vertexCountF = float(triShape->GetTrishapeRuntimeData().vertexCount);
		const auto* smooth = SmoothedNormalsFor(cap);
		ID3D11ShaderResourceView* smoothSRV = smooth && smooth->ready ? smooth->srv.get() : nullptr;
		captureSmoothSRVs[ci] = smoothSRV;

		StaticsCB& scb = captureRecords[ci];
		scb.WorldRow0 = { rot.entry[0][0] * scale, rot.entry[0][1] * scale, rot.entry[0][2] * scale, cap.world.translate.x };
		scb.WorldRow1 = { rot.entry[1][0] * scale, rot.entry[1][1] * scale, rot.entry[1][2] * scale, cap.world.translate.y };
		scb.WorldRow2 = { rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, cap.world.translate.z };
		// Objects carry no lift into the raster and cones; roads keep theirs.
		scb.ObjectsDepth = cap.road ? settings.RoadMeshesDepth : 0.0f;
		scb.RoundedDepth = cap.road ? settings.RoadMeshesDepth : 0.0f;
		maxSeedDepth = std::max(maxSeedDepth, std::max(scb.ObjectsDepth, scb.RoundedDepth));
		scb.VertexCountF = vertexCountF;
		scb.HeightWindowCenter = heightWindowCenter;
		scb.HeightHalfExtent = halfExtent;
		scb.LegacySkin = cap.road ? 1.0f : 0.0f;
		scb.FadeExempt = cap.fadeExempt ? 1.0f : 0.0f;
		scb.FullCoat = cap.fullCoat ? 1.0f : 0.0f;
		scb.RoadField = cap.road ? 1.0f : 0.0f;
		scb.ProjThreshold = cap.projThreshold;
		{
			// The S4 path no longer depends on the 3D toggle: that toggle is
			// the RISE only. The coat and its edge lumps live in the S4 draw,
			// so gating the draw on it took the border Josef keeps along with
			// the shell (his test, 2026-09-06).
			const bool s4Shell = !cap.road && cap.projThreshold > -0.9f && SD_ProjNoiseMapSRV();
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
		if (farRecordIndex[ci] != ~0u) {
			// Same draw against the far window.
			StaticsCB& farBlock = captureRecords[size_t(farRecordBase) + farRecordIndex[ci]];
			farBlock = scb;
			farBlock.HeightWindowCenter = farWindowCenter;
			farBlock.HeightHalfExtent = farHalfExtentLive;
		}
	}
	const bool captureRecordsLive = captureCount > 0 && UploadStaticsRecords(captureRecords.data(), uint32_t(captureRecords.size()), recordNeeded.data());
	uint32_t captureParity = 0;
	// The top/bottom and top-only PSs read no b1: the raster loops bind the VS alone and the PS takes the last record after them.
	uint32_t lastRasterRecord = ~0u;
	uint32_t lastRasterParity = 0;
	auto bindRasterRecord = [&](uint32_t a_index) {
		lastRasterRecord = a_index;
		lastRasterParity = captureParity;
		BindStaticsRecord(a_index, false, false, captureParity);
	};

	// Each raster level's draws in draw order. A level whose key (every draw's
	// binds and record bytes, the inherited state and the level's extras)
	// matches the one its targets were last drawn with keeps them: the same
	// draws, records, state and clears give the same raster under MAX.
	struct HeightRasterDraw
	{
		ID3D11InputLayout* layout;
		ID3D11Buffer* vb;
		ID3D11Buffer* ib;
		UINT stride;
		uint32_t indexCount;
		uint32_t capture;
		uint32_t record;
	};
	auto collectDraw = [&](uint32_t a_ci, uint32_t a_record, std::vector<HeightRasterDraw>& a_out) {
		auto* geometry = capturedStatics[a_ci].geometry.get();
		auto triShape = geometry ? geometry->AsTriShape() : nullptr;
		if (!triShape)
			return;
		auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
			return;
		const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		if (indexCount == 0)
			return;
		auto desc = rendererData->vertexDesc;
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
			return;
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		auto layoutIt = staticsILCache.find(descKey);
		if (layoutIt == staticsILCache.end() || !layoutIt->second)
			return;  // layouts are created by the skin pass; reuse only
		const UINT stride = uint32_t(descKey & 0xF) * 4;
		if (stride == 0)
			return;
		a_out.push_back({ layoutIt->second.get(), reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer),
			reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer), stride, indexCount, a_ci, a_record });
	};
	// The rasters inherit the pass's rasterizer state, topology and HS/DS/GS.
	std::vector<uint8_t> inheritedState;
	{
		auto put = [&](const void* a_p, size_t a_n) {
			const auto* bytes = static_cast<const uint8_t*>(a_p);
			inheritedState.insert(inheritedState.end(), bytes, bytes + a_n);
		};
		winrt::com_ptr<ID3D11RasterizerState> rasterState;
		context->RSGetState(rasterState.put());
		D3D11_RASTERIZER_DESC rasterDesc{};
		const uint32_t hasRasterState = rasterState ? 1u : 0u;
		if (rasterState)
			rasterState->GetDesc(&rasterDesc);
		put(&hasRasterState, sizeof(hasRasterState));
		put(&rasterDesc, sizeof(rasterDesc));
		if (rasterState && rasterDesc.ScissorEnable) {
			D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			context->RSGetScissorRects(&scissorCount, scissors);
			put(&scissorCount, sizeof(scissorCount));
			put(scissors, sizeof(D3D11_RECT) * scissorCount);
		}
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		context->IAGetPrimitiveTopology(&topology);
		put(&topology, sizeof(topology));
		winrt::com_ptr<ID3D11HullShader> hs;
		winrt::com_ptr<ID3D11DomainShader> ds;
		winrt::com_ptr<ID3D11GeometryShader> gs;
		context->HSGetShader(hs.put(), nullptr, nullptr);
		context->DSGetShader(ds.put(), nullptr, nullptr);
		context->GSGetShader(gs.put(), nullptr, nullptr);
		const uint64_t stages[3] = { reinterpret_cast<uint64_t>(hs.get()), reinterpret_cast<uint64_t>(ds.get()), reinterpret_cast<uint64_t>(gs.get()) };
		put(stages, sizeof(stages));
	}
	auto rasterKey = [&](const std::vector<HeightRasterDraw>& a_draws, std::initializer_list<uint64_t> a_extras) {
		std::vector<uint8_t> key;
		key.reserve(inheritedState.size() + a_extras.size() * 8 + a_draws.size() * (56 + sizeof(StaticsCB)));
		auto put = [&](const void* a_p, size_t a_n) {
			const auto* bytes = static_cast<const uint8_t*>(a_p);
			key.insert(key.end(), bytes, bytes + a_n);
		};
		put(inheritedState.data(), inheritedState.size());
		for (uint64_t extra : a_extras)
			put(&extra, sizeof(extra));
		for (const auto& d : a_draws) {
			const auto& cap = capturedStatics[d.capture];
			const uint64_t drawBinds[6] = { reinterpret_cast<uint64_t>(d.layout), reinterpret_cast<uint64_t>(d.vb), reinterpret_cast<uint64_t>(d.ib),
				reinterpret_cast<uint64_t>(captureSmoothSRVs[d.capture]), reinterpret_cast<uint64_t>(cap.geometry.get()), cap.road ? 1u : 0u };
			const uint32_t counts[2] = { d.stride, d.indexCount };
			put(drawBinds, sizeof(drawBinds));
			put(counts, sizeof(counts));
			put(&captureRecords[d.record], sizeof(StaticsCB));
		}
		return key;
	};
	auto levelKept = [&](uint32_t a_level, std::vector<uint8_t> a_key) {
		const bool kept = !heightRasterCacheOff && a_key == heightRasterKey[a_level];
		if (kept) {
			heightRasterKeptBits |= 1u << a_level;
			heightRasterKept[a_level]++;
		} else {
			heightRasterKey[a_level] = std::move(a_key);
		}
		return kept;
	};
	auto drawList = [&](const std::vector<HeightRasterDraw>& a_draws, bool a_roadsOnly) {
		SD_DrawBinds binds;
		for (const auto& d : a_draws) {
			if (a_roadsOnly && !capturedStatics[d.capture].road)
				continue;
			binds.Layout(context, d.layout);
			binds.Vertex(context, d.vb, d.stride);
			binds.Index(context, d.ib, DXGI_FORMAT_R16_UINT);
			binds.Smooth(context, captureSmoothSRVs[d.capture]);
			if (captureRecordsLive)
				bindRasterRecord(d.record);
			else
				staticsCB->Update(captureRecords[d.record]);
			context->DrawIndexed(d.indexCount, 0, 0);
		}
	};
	// A kept level binds nothing; the PS still takes the record it would have.
	auto noteLastRecord = [&](const std::vector<HeightRasterDraw>& a_draws) {
		if (!a_draws.empty() && captureRecordsLive) {
			lastRasterRecord = a_draws.back().record;
			lastRasterParity = captureParity;
		}
	};
	heightRasterKeptBits = 0;
	heightRasterRuns++;
	const bool verifyOn = heightRasterVerify && EnsureHeightRasterVerify();
	const bool coneVerifyOn = coneVerify && EnsureHeightRasterVerify() && EnsureConeVerify();
	const bool combineVerifyOn = combineVerify && EnsureHeightRasterVerify() && EnsureCombineVerify();
	uint32_t verifiedLevels = 0;
	if (verifyOn || coneVerifyOn || combineVerifyOn) {
		const UINT zero4[4] = {};
		context->ClearUnorderedAccessViewUint(heightRasterVerifyAllUAV.get(), zero4);
	}
	ID3D11Buffer* compareCB = heightProcessCB->CB();
	auto compareKept = [&](Texture2D* a_kept, Texture2D* a_fresh, uint32_t a_slot) {
		ID3D11ShaderResourceView* compareSRVs[2] = { a_kept->srv.get(), a_fresh->srv.get() };
		ID3D11ShaderResourceView* nullCompareSRVs[2] = {};
		ID3D11UnorderedAccessView* countUAV = heightRasterVerifySlotUAV[a_slot].get();
		ID3D11Buffer* nullCompareCB = nullptr;
		context->CSSetConstantBuffers(0, 1, &compareCB);
		context->CSSetShaderResources(8, 2, compareSRVs);
		context->CSSetUnorderedAccessViews(4, 1, &countUAV, nullptr);
		context->CSSetShader(heightRasterCompareCS, nullptr, 0);
		context->Dispatch((a_kept->desc.Width + 7) / 8, (a_kept->desc.Height + 7) / 8, 1);
		context->CSSetShaderResources(8, 2, nullCompareSRVs);
		context->CSSetUnorderedAccessViews(4, 1, nullCsUAVs, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullCompareCB);
		context->CSSetShader(nullptr, nullptr, 0);
	};

	// Layer 1: every capture into heightRaster, then the roads alone into the
	// skin depth; each pass binds RT0 only, and MAX does not depend on order.
	std::vector<HeightRasterDraw> layer1Draws;
	for (uint32_t ci = 0; ci < captureCount; ci++) {
		const auto& cap = capturedStatics[ci];
		if (!cap.geometry || cap.lodBatch || outsideWindow(cap.geometry.get(), halfExtent))
			continue;
		const size_t drawsBefore = layer1Draws.size();
		collectDraw(ci, ci, layer1Draws);
		if (layer1Draws.size() != drawsBefore) {
			const auto& wb = cap.geometry->worldBound;
			const float reach = wb.radius + 4.0f * texel;
			const float4 drawBox = { wb.center.x - reach, wb.center.y - reach, wb.center.x + reach, wb.center.y + reach };
			heightGhostBox = heightGhostsLive ? float4{ std::min(heightGhostBox.x, drawBox.x), std::min(heightGhostBox.y, drawBox.y),
													std::max(heightGhostBox.z, drawBox.z), std::max(heightGhostBox.w, drawBox.w) } :
			                                    drawBox;
			heightGhostsLive = true;
		}
	}
	auto drawLayer1 = [&](ID3D11RenderTargetView* a_raster, ID3D11RenderTargetView* a_skin) {
		context->ClearRenderTargetView(a_raster, rasterClear);
		context->ClearRenderTargetView(a_skin, skinDepthClear);
		context->OMSetRenderTargets(1, &a_raster, nullptr);
		context->PSSetShader(heightPS, nullptr, 0);
		drawList(layer1Draws, false);
		if (roadRaster) {
			context->OMSetRenderTargets(1, &a_skin, nullptr);
			context->PSSetShader(heightPSSkinDepth, nullptr, 0);
			drawList(layer1Draws, true);
		}
	};
	const bool layer1Kept = levelKept(0, rasterKey(layer1Draws, { reinterpret_cast<uint64_t>(heightVS), reinterpret_cast<uint64_t>(heightPS),
															 reinterpret_cast<uint64_t>(heightPSSkinDepth), reinterpret_cast<uint64_t>(captureTerrainSRV), shellTerrainVersion,
															 std::bit_cast<uint32_t>(processData.TerrainWindowOrigin.x), std::bit_cast<uint32_t>(processData.TerrainWindowOrigin.y),
															 std::bit_cast<uint32_t>(processData.TerrainTexelSize), processData.TerrainDim, roadRaster ? 1u : 0u }));
	ID3D11RenderTargetView* nullRTVs[3] = { nullptr, nullptr, nullptr };
	globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Layer 1"));
	if (!layer1Kept)
		drawLayer1(heightRaster->rtv.get(), heightSkinDepth->rtv.get());
	else
		noteLastRecord(layer1Draws);
	const bool verifyLayer1 = verifyOn && (layer1Kept || heightRasterVerifyMiss);
	if (verifyLayer1)
		drawLayer1(heightRasterVerifyTex[0]->rtv.get(), heightRasterVerifyTex[1]->rtv.get());
	globals::profiler->EndPass();

	context->OMSetRenderTargets(3, nullRTVs, nullptr);
	context->OMSetBlendState(heightMaxBlendState.get(), nullptr, 0xFFFFFFFF);
	context->PSSetShader(heightPS, nullptr, 0);
	if (verifyLayer1) {
		compareKept(heightRaster, heightRasterVerifyTex[0], 0);
		compareKept(heightSkinDepth, heightRasterVerifyTex[1], 1);
		verifiedLevels |= 1u;
	}

	// Layer 1 is complete only once the ghost is under it: the peels below
	// test against it, and so does everything downstream.
	globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Ghost Merge"));
	if (!keepEmpty)
		mergeGhost(heightTopRaw[previous], heightTopRaw[heightCurrent], heightBottomRaw[previous], heightBottomRaw[heightCurrent], heightTileEmpty, heightRaster);
	globals::profiler->EndPass();

	// The top-only levels: 1 near clipmap, 2 far roads. RT0 alone, no ghost.
	// Verify draws a kept level into its scratch twin (a_level + 1).
	ID3D11PixelShader* topOnlyPS = heightPSTopOnly ? heightPSTopOnly : heightPS;
	auto drawTopOnly = [&](uint32_t a_level, Texture2D* a_target, const std::vector<HeightRasterDraw>& a_draws) {
		const bool kept = levelKept(a_level, rasterKey(a_draws, { reinterpret_cast<uint64_t>(heightVS), reinterpret_cast<uint64_t>(topOnlyPS) }));
		const bool verify = verifyOn && (kept || heightRasterVerifyMiss);
		context->PSSetShader(topOnlyPS, nullptr, 0);
		globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow(a_level == 1 ? "SnowDeformation::Objects - Height Maps Fine" : "SnowDeformation::Objects - Height Maps Far"));
		auto draw = [&](Texture2D* a_into) {
			context->ClearRenderTargetView(a_into->rtv.get(), topClear);
			ID3D11RenderTargetView* levelRTVs[1] = { a_into->rtv.get() };
			context->OMSetRenderTargets(1, levelRTVs, nullptr);
			drawList(a_draws, false);
		};
		if (!kept)
			draw(a_target);
		else
			noteLastRecord(a_draws);
		if (verify)
			draw(heightRasterVerifyTex[a_level + 1]);
		globals::profiler->EndPass();
		context->OMSetRenderTargets(3, nullRTVs, nullptr);
		if (verify) {
			compareKept(a_target, heightRasterVerifyTex[a_level + 1], a_level + 1);
			verifiedLevels |= 1u << a_level;
		}
	};
	// Near clipmap level 0: the same captures against a quarter-width window,
	// so the layer-1 top lands at one world unit per texel. No ghost and no
	// bottoms - readers fall back to the coarse level outside the window, and
	// the shelter mask is the coarse level's business.
	if (fineLevel) {
		std::vector<HeightRasterDraw> fineDraws;
		for (uint32_t ci = 0; ci < captureCount; ci++) {
			const auto& cap = capturedStatics[ci];
			if (!cap.geometry || cap.lodBatch || outsideWindow(cap.geometry.get(), FineRasterHalfExtent()))
				continue;
			collectDraw(ci, captureCount * 2 + ci, fineDraws);
		}
		drawTopOnly(1, heightTopRawFine, fineDraws);
	} else {
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Height Maps");
	}

	// Far road level: the road captures alone, MAX-rasterized over the
	// loaded-cell square into one top map. Rebuilt from this frame's list -
	// roads never move, and an off-screen road needs no patch - so no ghost
	// and no scroll.
	if (farLevel) {
		std::vector<HeightRasterDraw> farDraws;
		for (uint32_t ci = 0; ci < captureCount; ci++)
			if (farRecordIndex[ci] != ~0u && capturedStatics[ci].geometry)
				collectDraw(ci, farRecordBase + farRecordIndex[ci], farDraws);
		drawTopOnly(2, heightTopRawFar, farDraws);
	} else {
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Height Maps");
	}
	if (lastRasterRecord != ~0u) {
		// Unbind first: the runtime filters a rebind of the same buffer at a new offset.
		ID3D11Buffer* none = nullptr;
		context->PSSetConstantBuffers(1, 1, &none);
		ID3D11Buffer* buffer = staticsRecordCB[lastRasterParity & 1].get();
		const UINT first = lastRasterRecord * (kStaticsRecordStride / 16);
		const UINT count = kStaticsRecordStride / 16;
		staticsContext1->PSSetConstantBuffers1(1, 1, &buffer, &first, &count);
	}

	// The layer-2 PEEL: re-rasterize the captures against the finished
	// layer-1 top (now readable), keeping only up-facing fragments below it
	// by the peel tolerance; MAX blending yields the next-highest surface per
	// column. The layer-1 cone seed reads it for its plane-continuation test.
	// Only the transform and the window fields matter here.
	Texture2D* peelTarget = heightTop2Raw[heightCurrent];
	if (heightPeelPS && peelTarget && !keepEmpty) {
		ID3D11RenderTargetView* peelRTVs[1] = { peelTarget->rtv.get() };
		context->OMSetRenderTargets(1, peelRTVs, nullptr);
		context->PSSetShader(heightPeelPS, nullptr, 0);
		// t3 = layer 1.
		ID3D11ShaderResourceView* peelSRVs[1] = { heightTopRaw[heightCurrent]->srv.get() };
		context->PSSetShaderResources(3, 1, peelSRVs);
		// The peel PS addresses the layer maps through StaticCB's window
		// fields; the capture pass binds b1 to the VS only.
		context->PSSetConstantBuffers(1, 1, &cb1);

		globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Peel"));
		SD_DrawBinds peelBinds;
		for (uint32_t ci = 0; ci < captureCount; ci++) {
			const auto& cap = capturedStatics[ci];
			auto* geometry = cap.geometry.get();
			if (!geometry)
				continue;
			if (cap.lodBatch)
				continue;
			if (outsideWindow(geometry, halfExtent))
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
			peelBinds.Layout(context, layoutIt->second.get());
			UINT stride = uint32_t(descKey & 0xF) * 4;
			if (stride == 0)
				continue;
			peelBinds.Vertex(context, reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer), stride);
			peelBinds.Index(context, reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer), DXGI_FORMAT_R16_UINT);

			peelBinds.Smooth(context, captureSmoothSRVs[ci]);
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

		// Same merge for the peeled layer, before the next peel reads it;
		// tops only, nothing reads a peeled layer's bottoms.
		globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Peel Merge"));
		if (heightScrollTopCS) {
			mergeGhost(heightTop2Raw[previous], peelTarget, nullptr, nullptr, nullptr);
		} else {
			// Without the tops-only merge the bottoms need an output of their own.
			if (!heightScratch) {
				const DXGI_FORMAT format = heightBottomRaw[0]->desc.Format;
				heightScratch = new Texture2D(heightBottomRaw[0]->desc, "SnowDeformation::HeightPeelScratch");
				heightScratch->CreateSRV({ .Format = format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
				heightScratch->CreateRTV({ .Format = format, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
				heightScratch->CreateUAV({ .Format = format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
			}
			mergeGhost(heightTop2Raw[previous], peelTarget, heightBottomRaw[previous], heightScratch, nullptr);
		}
		globals::profiler->EndPass();
	}

	// P3: the sky-openness field from a layer-1 top (half-res; the consumers
	// bilinear it). Only Volume Snow reads it, and the volume below reads the
	// last run's: a run that turns the bake back on first bakes that one from
	// the previous top, which this run has only read.
	// Made on Volume Snow's first run: nothing else reads it.
	if (settings.VolumeSnow && objectSkyOpenCS && !objectSkyOpen) {
		D3D11_TEXTURE2D_DESC openDesc = heightTopRaw[0]->desc;
		openDesc.Width = kHeightMapDim / 2;
		openDesc.Height = kHeightMapDim / 2;
		openDesc.Format = DXGI_FORMAT_R8_UNORM;
		openDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		objectSkyOpen = new Texture2D(openDesc, "SnowDeformation::ObjectSkyOpen");
		objectSkyOpen->CreateSRV({ .Format = openDesc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
		objectSkyOpen->CreateUAV({ .Format = openDesc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
	}
	const bool skyWanted = settings.VolumeSnow && objectSkyOpenCS && objectSkyOpen;
	auto bakeSkyOpen = [&](Texture2D* a_top) {
		globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Sky"));
		context->CSSetShader(objectSkyOpenCS, nullptr, 0);
		ID3D11ShaderResourceView* openSRV = a_top->srv.get();
		ID3D11UnorderedAccessView* openUAV = objectSkyOpen->uav.get();
		context->CSSetShaderResources(0, 1, &openSRV);
		context->CSSetUnorderedAccessViews(0, 1, &openUAV, nullptr);
		const uint32_t openDim = (kHeightMapDim / 2 + 7) / 8;
		context->Dispatch(openDim, openDim, 1);
		context->CSSetShaderResources(0, 1, nullCsSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		globals::profiler->EndPass();
	};
	if (settings.VolumeSnow && skyWanted && !skyOpenCurrent && heightTopRaw[previous])
		bakeSkyOpen(heightTopRaw[previous]);

	RenderVoxelVolume(captureRecords.data(), captureCount, captureRecordsLive && volumeRecords, captureParity);

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

	// Combine: raw tops/bottoms -> shelter mask (bottomFiltered); bare ground
	// under floating walkways/roofs/bridges.
	const UINT dispatchDim = (kHeightMapDim + 7) / 8;
	globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Combine"));
	ID3D11ShaderResourceView* terrainSRV = shellTerrainTexture->srv.get();
	context->CSSetConstantBuffers(0, 1, &processCB);
	context->CSSetShaderResources(2, 1, &terrainSRV);
	{
		ID3D11ShaderResourceView* combineSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightBottomRaw[heightCurrent]->srv.get() };
		ID3D11Buffer* exclusionCB = doorsCB->CB();
		// t4: the layer-1 merge's tile flags (unbound reads 0, which never skips).
		ID3D11ShaderResourceView* tileSRV = heightTileEmpty ? heightTileEmpty->srv.get() : nullptr;
		ID3D11ShaderResourceView* exclusionListSRV = exclusionListBuffer->SRV();
		context->CSSetConstantBuffers(1, 1, &exclusionCB);
		context->CSSetShaderResources(0, 2, combineSRVs);
		context->CSSetShaderResources(5, 1, &exclusionListSRV);
		context->CSSetShader(heightCombineCS, nullptr, 0);
		auto combine = [&](Texture2D* a_mask, ID3D11ShaderResourceView* a_tiles) {
			// Two-channel mask at u2 (u1 belongs to ScrollCS's raw bottoms; u0
			// and u1 stay clear here).
			ID3D11UnorderedAccessView* combineUAVs[3] = { nullptr, nullptr, a_mask->uav.get() };
			context->CSSetShaderResources(4, 1, &a_tiles);
			context->CSSetUnorderedAccessViews(0, 3, combineUAVs, nullptr);
			context->Dispatch(dispatchDim, dispatchDim, 1);
		};
		combine(heightBottomFiltered, tileSRV);
		if (combineVerifyOn)
			combine(combineVerifyTex, nullptr);
		context->CSSetShaderResources(0, 2, nullCsSRVs);
		context->CSSetShaderResources(4, 2, nullCsSRVs);
		ID3D11UnorderedAccessView* nullCombineUAVs[3] = { nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, 3, nullCombineUAVs, nullptr);
		ID3D11Buffer* nullExclusionCB = nullptr;
		context->CSSetConstantBuffers(1, 1, &nullExclusionCB);
	}

	globals::profiler->EndPass();
	if (combineVerifyOn) {
		compareKept(heightBottomFiltered, combineVerifyTex, 6);
		verifiedLevels |= 1u << 5;
	}

	// Angle of repose for the object cones: multi-scale min-plus passes
	// (large steps first), ping-ponging with the previous bottoms, dead once
	// the layer-1 merge has run. Steps are texels:
	// the leading 64 is the cone's world reach at the 4-unit texel; the
	// trailing repeat keeps the pass count even.
	static constexpr uint kConeSteps[] = { 64, 32, 16, 8, 4, 2, 1, 1 };
	// ConeFusedCS's CONE_TILE.
	static constexpr uint kConeFusedTile = 16;
	// A pass can only lower a texel from a neighbour one step away plus the
	// slope over that distance, and every value lies in [0, maxSeed]: a step
	// reaching past maxSeed / slope is a no-op and is skipped. The margin
	// keeps a boundary case on the safe side of the shader's own rounding.
	auto activeConeSteps = [&](const HeightProcessCB& a_data, float a_maxSeed) {
		std::vector<uint> steps;
		const float texel = a_data.HeightHalfExtent * 2.0f / float(kHeightMapDim);
		for (uint step : kConeSteps)
			if (float(step) * texel * a_data.SlopePerUnit <= a_maxSeed * 1.001f)
				steps.push_back(step);
		return steps;
	};

	// Object snow cone: the same repose transform run over the OBJECT top
	// raster, giving the skin's edge taper a ready-made snow surface to read.
	// Seeded at full class depth over covered columns and at terrain height
	// off them, so each object's layer slopes down to the ground at its rim.
	// The seed floor: objects carry no depth, so their columns seed at the
	// coat's own lift; roads seed per texel from InB (see SkinLift.RimT).
	// Later passes read these constants whether or not the chains run.
	processData.ObjectSnowDepth = 0.1f;
	heightProcessCB->Update(processData);
	conePassesCoarse = 0;
	bool coneBuilt = false;
	if (!roadMaps || keepEmpty) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Height Maps");
	} else if (objectConeSeedCS && objectConeCS && objectSnowCone && heightTopRaw[heightCurrent]) {
		coneBuilt = true;
		globals::profiler->BeginPass(SnowDevSwitches::HeightMapRow("SnowDeformation::Objects - Height Maps Object Cone"));
		const auto coarseSteps = activeConeSteps(processData, std::max(maxSeedDepth, processData.ObjectSnowDepth));
		conePassesCoarse = uint32_t(coarseSteps.size());
		coneRuns++;

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

		// The repose chain over a seeded cone; it lands back in a_cone.
		auto repose = [&](Texture2D* a_cone, Texture2D* a_seeded, HeightProcessCB& a_data, const std::vector<uint>& a_steps) {
			Texture2D* in = a_seeded;
			Texture2D* out = a_seeded == a_cone ? heightBottomRaw[previous] : a_cone;
			context->CSSetShader(objectConeCS, nullptr, 0);
			for (uint step : a_steps) {
				a_data.ConeStep = step;
				heightProcessCB->Update(a_data);
				ID3D11ShaderResourceView* passSRV = in->srv.get();
				ID3D11UnorderedAccessView* passUAV = out->uav.get();
				context->CSSetShaderResources(0, 1, &passSRV);
				context->CSSetUnorderedAccessViews(0, 1, &passUAV, nullptr);
				context->Dispatch(dispatchDim, dispatchDim, 1);
				context->CSSetShaderResources(0, 1, nullCsSRVs);
				context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
				std::swap(in, out);
			}
			settleCone(in, out);
		};

		// The separate passes: the seed, then the repose chain. The chain lands
		// back in its cone texture only over an even number of passes, the two
		// settles included: an odd count seeds the scratch. b0 is bound again
		// per chain, the verify's compare unbinds it.
		auto chainCone = [&](Texture2D* a_cone, ID3D11ComputeShader* a_seedCS, const auto& a_bindSeed) {
			Texture2D* seeded = (coarseSteps.size() % 2) ? heightBottomRaw[previous] : a_cone;
			heightProcessCB->Update(processData);
			context->CSSetConstantBuffers(0, 1, &processCB);
			context->CSSetShader(a_seedCS, nullptr, 0);
			a_bindSeed(true);
			ID3D11UnorderedAccessView* seedUAV = seeded->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &seedUAV, nullptr);
			context->Dispatch(dispatchDim, dispatchDim, 1);
			a_bindSeed(false);
			context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
			repose(a_cone, seeded, processData, coarseSteps);
		};
		// No repose step: the chain is the seed and two settles, which
		// ConeFusedCS runs per 16x16 tile in one dispatch, bit for bit.
		const bool fuse = coarseSteps.empty() && objectConeDiffuseCS && !coneFusedOff;
		auto fusedCone = [&](Texture2D* a_cone, ID3D11ComputeShader* a_fusedCS, const auto& a_bindSeed) {
			heightProcessCB->Update(processData);
			context->CSSetConstantBuffers(0, 1, &processCB);
			context->CSSetShader(a_fusedCS, nullptr, 0);
			a_bindSeed(true);
			ID3D11UnorderedAccessView* coneUAV = a_cone->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &coneUAV, nullptr);
			context->Dispatch(kHeightMapDim / kConeFusedTile, kHeightMapDim / kConeFusedTile, 1);
			a_bindSeed(false);
			context->CSSetUnorderedAccessViews(0, 1, nullCsUAVs, nullptr);
		};

		// InB (t1) = the skin-depth raster: the per-texel cone seed, so roads
		// seed at their own class depth (see ObjectConeSeedCS). InC (t3) =
		// the next layer's top, for the rise rim's continuation test.
		ID3D11ShaderResourceView* objectSeedSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(),
			heightSkinDepth ? heightSkinDepth->srv.get() : nullptr };
		ID3D11ShaderResourceView* objectSeedNextSRV = heightTop2Raw[heightCurrent] ? heightTop2Raw[heightCurrent]->srv.get() : nullptr;
		auto bindObjectSeed = [&](bool a_bind) {
			ID3D11ShaderResourceView* nullSeedSRVs[2] = { nullptr, nullptr };
			context->CSSetShaderResources(0, 2, a_bind ? objectSeedSRVs : nullSeedSRVs);
			context->CSSetShaderResources(3, 1, a_bind ? &objectSeedNextSRV : nullCsSRVs);
		};
		const bool objectFused = fuse && objectConeFusedCS;
		if (objectFused) {
			fusedCone(objectSnowCone, objectConeFusedCS, bindObjectSeed);
			coneFusedRuns++;
		} else {
			chainCone(objectSnowCone, objectConeSeedCS, bindObjectSeed);
		}
		if (coneVerifyOn && objectFused) {
			chainCone(coneVerifyTex, objectConeSeedCS, bindObjectSeed);
			compareKept(objectSnowCone, coneVerifyTex, 4);
			verifiedLevels |= 1u << 3;
		}
		globals::profiler->EndPass();

	}
	heightMapsEmpty = emptyWindow && (keepEmpty || coneBuilt);

	if ((verifyOn || coneVerifyOn || combineVerifyOn) && verifiedLevels) {
		auto& ring = heightRasterVerifyRing;
		if (!heightRasterVerifyStagingLevels[ring % 3]) {
			context->CopyResource(heightRasterVerifyStaging[ring % 3].get(), heightRasterVerifyCount.get());
			heightRasterVerifyStagingLevels[ring % 3] = verifiedLevels;
			ring++;
		}
	}
	if (heightRasterVerifyStaging[0]) {
		// Copies in flight, DO_NOT_WAIT: a busy one is read on a later run.
		for (uint32_t slot = 0; slot < 3; slot++) {
			const uint32_t levels = heightRasterVerifyStagingLevels[slot];
			if (!levels)
				continue;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(heightRasterVerifyStaging[slot].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
				continue;
			const auto* words = static_cast<const uint32_t*>(mapped.pData);
			for (uint32_t level = 0; level < 6; level++)
				if (levels & (1u << level))
					heightRasterVerified[level]++;
			bool sampled = false;
			for (uint32_t target = 0; target < kHeightVerifySlots; target++) {
				const uint32_t level = kHeightVerifyTargetLevel[target];
				const uint32_t count = words[target * kHeightVerifySlotWords];
				if (!(levels & (1u << level)) || !count)
					continue;
				heightRasterMismatch[target] += count;
				const std::scoped_lock debugLock(heightRasterDebugLock);
				if (!sampled) {
					heightRasterSamples.clear();
					sampled = true;
				}
				for (uint32_t i = 0; i < std::min(count, kHeightVerifySamples); i++) {
					const uint32_t* s = words + target * kHeightVerifySlotWords + 4 + i * 8;
					heightRasterSamples.push_back({ target, s[0] & 0xFFFF, s[0] >> 16, std::bit_cast<float>(s[1]), std::bit_cast<float>(s[2]),
						std::bit_cast<float>(s[3]), std::bit_cast<float>(s[4]) });
				}
			}
			context->Unmap(heightRasterVerifyStaging[slot].get(), 0);
			heightRasterVerifyStagingLevels[slot] = 0;
		}
	}

	// This run's field, for the next run's volume.
	if (skyWanted && heightTopRaw[heightCurrent])
		bakeSkyOpen(heightTopRaw[heightCurrent]);
	else
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Height Maps");
	skyOpenCurrent = skyWanted && heightTopRaw[heightCurrent];

	ID3D11ShaderResourceView* nullTailSRVs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(2, 2, nullTailSRVs);
	ID3D11Buffer* nullProcessCB = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullProcessCB);
	context->CSSetShader(nullptr, nullptr, 0);

	// Height-field probe (Debugging Options): the seven object maps read
	// back at the player's texel. Copy this frame, map LAST frame's copy
	// with DO_NOT_WAIT, so the readout is one frame old and never stalls.
	// Only while the readout is on screen.
	if (globals::state->frameCount - heightProbeShownFrame >= 3) {
		probeValid = false;
	} else if (auto probePlayer = globals::game::player) {
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
			// Slot 3: the water window, in the TERRAIN window's frame (+Y-up
			// raster, so the row counts down from the top).
			if (waterHeightTexture && waterHeightTexture->resource && waterWindowCellX != INT_MIN) {
				const float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
				const int wx = int(std::floor((pos.x - waterWindowCellX * cellSize) / kShellVertexSpacing));
				const int wyUp = int(std::floor((pos.y - waterWindowCellY * cellSize) / kShellVertexSpacing));
				const uint wtx = uint(std::clamp(wx, 0, kShellWindowDim - 1));
				const uint wty = uint(std::clamp(kShellWindowDim - 1 - wyUp, 0, kShellWindowDim - 1));
				D3D11_BOX waterBox{ wtx, wty, 0, wtx + 1, wty + 1, 1 };
				context->CopySubresourceRegion(probeStaging[probeCursor].get(), 0, 3, 0, 0, waterHeightTexture->resource.get(), 0, &waterBox);
			}
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

void SnowDeformation::FillSkinDrawCB(const CapturedSnowStatic& a_cap, bool a_s4Shell, float a_vertexCount, bool a_hasSmoothedNormals, bool a_hasObjectTop, StaticsCB& a_scb) const
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
	a_scb.RoadField = a_cap.road ? 1.0f : 0.0f;
	a_scb.ProjThreshold = a_cap.projThreshold;
	a_scb.ClassOverride = (a_s4Shell || a_cap.forceRounded) ? 1.0f : 0.0f;
	a_scb.ProjNoiseScale = a_cap.projNoiseScale;
	a_scb.ProjNoiseTiling = a_cap.projNoiseTiling;
	a_scb.ProjNoiseWeight = a_cap.projNoiseWeight;
	a_scb.ParallaxFollow = (settings.SnowFollowsParallax && a_cap.mayParallax) ? 1.0f : 0.0f;
	// 2 = the S4 shell owns this draw; 0 = classic path.
	a_scb.ProjPixelEnable = a_s4Shell ? 2.0f : 0.0f;
	a_scb.PeelTol = kPeelTol;
	a_scb.HasSkinMasksCopy = landMasksCopySRV ? 1.0f : 0.0f;
	// A frame with no decal-mode draw leaves an older frame's snapshot bound.
	a_scb.HasPreDecalMasks = (preDecalMasksSRV && preDecalMasksFrame == globals::state->frameCount) ? 1.0f : 0.0f;
	// The accumulation scalar is already 0..1 over Accumulation Time, melt
	// and fade, independent of the peak - exactly the reach's timeline.
	a_scb.EdgeFlankWidth = (settings.EnableSnowAccumulation && settings.RecoloredSnowAccumulates) ?
	                           snowAccumulation.load(std::memory_order_relaxed) :
	                           std::clamp(settings.SkinEdgeFlankWidth, 0.0f, 1.0f);
	// Same veto as the Lighting-side recolor (sand and moss keep their
	// look), and only where the property really carries projection data:
	// the mesh-replacer default reconstructs a weight the game never paints.
	// LOD batches read the brightness recolor's written weight instead.
	a_scb.SteepThin = std::clamp(settings.SteepFaceThinning, 0.0f, 1.0f);
	a_scb.EdgeCoat = (settings.ProjSnowMatch && (a_cap.projReal || a_cap.lodBatch || a_cap.snowTex) && a_cap.geometry && a_cap.matoSnow) ? 1.0f : 0.0f;
	// The near clipmap shares the coarse window's centre, so its half-extent
	// is all the shaders need; 0 turns every fine read back into a coarse one.
	a_scb.FineHalfExtent = (FineLevelOn() && heightTopRawFine) ? FineRasterHalfExtent() : 0.0f;
	// Far road level (t40, bound by the skin pass): the road skin's
	// RoadOwnsColumn steps aside on far roads through it.
	a_scb.FarWindowCenterX = farWindowCenter.x;
	a_scb.FarWindowCenterY = farWindowCenter.y;
	a_scb.FarHalfExtent = farHalfExtentLive;
	a_scb.FarRoadDepth = settings.RoadMeshesDepth;
	a_scb.LODBatch = a_cap.lodBatch ? 1.0f : 0.0f;
	a_scb.TexturedSnow = a_cap.snowTex ? 1.0f : 0.0f;
	a_scb.AlphaTested = a_cap.alphaTested ? 1.0f : 0.0f;
	a_scb.DecalMode = a_cap.decalDepth ? 1.0f : 0.0f;
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

void SnowDeformation::RangeAllocator::Reset(uint32_t a_capacity)
{
	free.clear();
	if (a_capacity)
		free.emplace(0u, a_capacity);
}

uint32_t SnowDeformation::RangeAllocator::Allocate(uint32_t a_count, uint32_t a_align)
{
	if (a_count == 0 || a_align == 0)
		return UINT32_MAX;
	for (auto it = free.begin(); it != free.end(); ++it) {
		const uint64_t rangeStart = it->first;
		const uint64_t rangeEnd = rangeStart + it->second;
		const uint64_t start = (rangeStart + a_align - 1) / a_align * a_align;
		if (start + a_count > rangeEnd)
			continue;
		free.erase(it);
		if (start > rangeStart)
			free.emplace(uint32_t(rangeStart), uint32_t(start - rangeStart));
		if (rangeEnd > start + a_count)
			free.emplace(uint32_t(start + a_count), uint32_t(rangeEnd - start - a_count));
		return uint32_t(start);
	}
	return UINT32_MAX;
}

void SnowDeformation::RangeAllocator::Release(uint32_t a_offset, uint32_t a_count)
{
	if (a_count == 0)
		return;
	uint32_t start = a_offset;
	uint32_t end = a_offset + a_count;
	auto next = free.lower_bound(start);
	if (next != free.begin()) {
		auto prev = std::prev(next);
		if (prev->first + prev->second == start) {
			start = prev->first;
			free.erase(prev);
		}
	}
	if (next != free.end() && next->first == end) {
		end += next->second;
		free.erase(next);
	}
	free.emplace(start, end - start);
}

void SnowDeformation::ResetSmoothedNormals(bool a_release)
{
	smoothedNormalsCache.clear();
	smoothedNormalsLru.clear();
	smoothedNormalsResets++;
	meshBoundsFree.Reset(kMeshBoundsSlots);
	clusterFree.Reset(kClusterSlots);
	clusterIndexPoolFree.Reset(kClusterIndexPoolIndices);
	if (a_release) {
		smoothedNormalsChunks.clear();
		smoothSource = nullptr;
		smoothSourceSRV = nullptr;
		smoothSourceBytes = 0;
		smoothTable = nullptr;
		smoothTableUAVs.clear();
		smoothTableSlots = 0;
	} else {
		for (auto& chunk : smoothedNormalsChunks)
			chunk.ranges.Reset(chunk.capacity);
	}
	smoothAllocatorsLive = true;
}

void SnowDeformation::EvictSmoothedNormals(std::unordered_map<void*, SmoothedNormalsEntry>::iterator a_it)
{
	auto& e = a_it->second;
	if (e.chunk < smoothedNormalsChunks.size())
		smoothedNormalsChunks[e.chunk].ranges.Release(e.chunkOffset, e.chunkCount);
	if (e.boundsSlot != UINT32_MAX)
		meshBoundsFree.Release(e.boundsSlot, 1);
	if (e.clusterCount != 0) {
		clusterFree.Release(e.clusterOffset, e.clusterCount);
		clusterIndexPoolFree.Release(e.indexPoolOffset, e.indexCount);
	}
	smoothedNormalsLru.erase(e.lru);
	smoothedNormalsCache.erase(a_it);
}

bool SnowDeformation::EnsureSmoothBuildScratch(uint32_t a_sourceBytes, uint32_t a_tableSlots)
{
	auto device = globals::d3d::device;
	if (a_sourceBytes > smoothSourceBytes) {
		if (a_sourceBytes > (1u << 30))
			return false;
		uint32_t bytes = std::max(smoothSourceBytes, 1u << 20);
		while (bytes < a_sourceBytes)
			bytes <<= 1;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = bytes;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		winrt::com_ptr<ID3D11Buffer> buffer;
		if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
			return false;
		Util::SetResourceName(buffer.get(), "SnowDeformation::SmoothSource");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		srvDesc.BufferEx.NumElements = bytes / 4;
		srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (FAILED(device->CreateShaderResourceView(buffer.get(), &srvDesc, srv.put())))
			return false;
		Util::SetResourceName(srv.get(), "SnowDeformation::SmoothSource SRV");
		smoothSource = buffer;
		smoothSourceSRV = srv;
		smoothSourceBytes = bytes;
	}
	if (a_tableSlots > smoothTableSlots) {
		if (a_tableSlots > (1u << 24))
			return false;
		uint32_t slots = std::max(smoothTableSlots, 1u << 15);
		while (slots < a_tableSlots)
			slots <<= 1;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = slots * 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		winrt::com_ptr<ID3D11Buffer> buffer;
		if (FAILED(device->CreateBuffer(&desc, nullptr, buffer.put())))
			return false;
		Util::SetResourceName(buffer.get(), "SnowDeformation::SmoothTable");
		smoothTable = buffer;
		smoothTableUAVs.clear();
		smoothTableSlots = slots;
	}
	// Every table view starts at 0: TableMask addresses the first a_tableSlots slots.
	const uint32_t level = uint32_t(std::countr_zero(a_tableSlots));
	if (smoothTableUAVs.size() <= level)
		smoothTableUAVs.resize(level + 1);
	if (!smoothTableUAVs[level]) {
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = a_tableSlots * 4;
		uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		if (FAILED(device->CreateUnorderedAccessView(smoothTable.get(), &uavDesc, smoothTableUAVs[level].put())))
			return false;
		Util::SetResourceName(smoothTableUAVs[level].get(), "SnowDeformation::SmoothTable UAV");
	}
	return true;
}

bool SnowDeformation::AllocateSmoothedNormals(uint32_t a_count, uint32_t& a_chunk, uint32_t& a_offset)
{
	for (uint32_t i = 0; i < smoothedNormalsChunks.size(); i++) {
		const uint32_t offset = smoothedNormalsChunks[i].ranges.Allocate(a_count);
		if (offset != UINT32_MAX) {
			a_chunk = i;
			a_offset = offset;
			return true;
		}
	}
	const uint32_t capacity = std::max(kSmoothedNormalsChunkElements, a_count);
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = capacity * 16;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = 16;
	SmoothedNormalsChunk chunk;
	auto device = globals::d3d::device;
	if (FAILED(device->CreateBuffer(&desc, nullptr, chunk.buffer.put())))
		return false;
	Util::SetResourceName(chunk.buffer.get(), "SnowDeformation::SmoothedNormals");
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uavDesc.Buffer.NumElements = capacity;
	if (FAILED(device->CreateUnorderedAccessView(chunk.buffer.get(), &uavDesc, chunk.uav.put())))
		return false;
	Util::SetResourceName(chunk.uav.get(), "SnowDeformation::SmoothedNormals UAV");
	chunk.capacity = capacity;
	chunk.ranges.Reset(capacity);
	a_offset = chunk.ranges.Allocate(a_count);
	a_chunk = uint32_t(smoothedNormalsChunks.size());
	smoothedNormalsChunks.push_back(std::move(chunk));
	logger::info("[SNOW DEFORMATION] Smoothed-normal pool: buffer {} created ({} elements) at {} entries", a_chunk, capacity, smoothedNormalsCache.size());
	return a_offset != UINT32_MAX;
}

SnowDeformation::SmoothedNormalsEntry* SnowDeformation::SmoothedNormalsFor(CapturedSnowStatic& a_cap)
{
	const uint32_t frame = globals::state->frameCount;
	if (a_cap.smoothFrame != frame || a_cap.smoothResets != smoothedNormalsResets) {
		a_cap.smoothEntry = a_cap.geometry ? EnsureSmoothedNormals(a_cap.geometry.get()) : nullptr;
		a_cap.smoothFrame = frame;
		a_cap.smoothResets = smoothedNormalsResets;
	}
	return a_cap.smoothEntry;
}

SnowDeformation::SmoothedNormalsEntry* SnowDeformation::EnsureSmoothedNormals(RE::BSGeometry* a_geometry)
{
	auto triShape = a_geometry->AsTriShape();
	if (!triShape)
		return nullptr;
	auto rendererData = a_geometry->GetGeometryRuntimeData().rendererData;
	if (!rendererData || !rendererData->vertexBuffer)
		return nullptr;
	if (!smoothAllocatorsLive)
		ResetSmoothedNormals(false);

	auto desc = rendererData->vertexDesc;
	uint64_t descKey;
	memcpy(&descKey, &desc, sizeof(descKey));
	const uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
	const uint32_t triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
	const uint32_t frame = globals::state->frameCount;

	// A vertex buffer pointer the game recycled for another mesh fails the
	// check and rebuilds; within one frame the entry stands, as its range is
	// bound by this frame's draws.
	if (auto found = smoothedNormalsCache.find(rendererData->vertexBuffer); found != smoothedNormalsCache.end()) {
		auto& hit = found->second;
		if (hit.lastUsedFrame == frame ||
			(hit.descKey == descKey && hit.gameIB == rendererData->indexBuffer && hit.vertexCount == vertexCount && hit.triangleCount == triangleCount)) {
			if (hit.lastUsedFrame != frame) {
				hit.lastUsedFrame = frame;
				smoothedNormalsLru.splice(smoothedNormalsLru.end(), smoothedNormalsLru, hit.lru);
			}
			return &hit;
		}
		EvictSmoothedNormals(found);
	}
	// Builds only; the scope costs more than a hit.
	LoadTraceScope _loadTrace(this, "Statics: EnsureSmoothedNormals (build)");
	// Past the cap the least recently used entries make room; nothing used
	// this frame is evicted.
	while (smoothedNormalsCache.size() >= kSmoothedNormalsCap && !smoothedNormalsLru.empty()) {
		auto oldest = smoothedNormalsCache.find(smoothedNormalsLru.front());
		if (oldest == smoothedNormalsCache.end()) {
			smoothedNormalsLru.pop_front();
			continue;
		}
		if (oldest->second.lastUsedFrame == frame)
			break;
		EvictSmoothedNormals(oldest);
	}
	auto& entry = smoothedNormalsCache[rendererData->vertexBuffer];
	entry.descKey = descKey;
	entry.gameIB = rendererData->indexBuffer;
	entry.vertexCount = vertexCount;
	entry.triangleCount = triangleCount;
	entry.lastUsedFrame = frame;
	entry.lru = smoothedNormalsLru.insert(smoothedNormalsLru.end(), rendererData->vertexBuffer);

	// First sight of this geometry: build now into the pooled buffers (a
	// buffer copy and a few small dispatches, once per unique mesh). Failures
	// leave the null entry until it is evicted; the VS falls back to raw normals.
	if (!EnsureSmoothNormalsCS() || !smoothCB)
		return nullptr;

	if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
		return nullptr;
	const uint32_t stride = uint32_t(descKey & 0xF) * 4;
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

	// The game's own buffers carry no shader-resource bind flag: the vertex
	// stream is copied into the shared source buffer (raw view). The shader
	// reads vertexCount * stride bytes of it, all inside the copy.
	D3D11_BUFFER_DESC srcDesc{};
	gameVB->GetDesc(&srcDesc);
	if (uint64_t(vertexCount) * stride > srcDesc.ByteWidth)
		return nullptr;
	uint32_t tableSlots = 64;
	while (tableSlots < vertexCount * 2)
		tableSlots <<= 1;
	if (!EnsureSmoothBuildScratch(srcDesc.ByteWidth, tableSlots))
		return nullptr;
	// One extra element past the vertices: the mesh's flatness stats, read by
	// the VS to pick the flat or rounded snow behavior without any CPU
	// readback.
	uint32_t chunk = UINT32_MAX;
	uint32_t chunkOffset = 0;
	if (!AllocateSmoothedNormals(vertexCount + 1, chunk, chunkOffset))
		return nullptr;
	entry.chunk = chunk;
	entry.chunkOffset = chunkOffset;
	entry.chunkCount = vertexCount + 1;
	D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc{};
	outSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	outSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	outSrvDesc.Buffer.FirstElement = chunkOffset;
	outSrvDesc.Buffer.NumElements = vertexCount + 1;
	winrt::com_ptr<ID3D11ShaderResourceView> outSRV;
	if (FAILED(device->CreateShaderResourceView(smoothedNormalsChunks[chunk].buffer.get(), &outSrvDesc, outSRV.put())))
		return nullptr;

	const D3D11_BOX sourceBox{ 0, 0, 0, srcDesc.ByteWidth, 1, 1 };
	context->CopySubresourceRegion(smoothSource.get(), 0, 0, 0, 0, gameVB, 0, &sourceBox);
	ID3D11UnorderedAccessView* tableUAV = smoothTableUAVs[std::countr_zero(tableSlots)].get();
	const UINT clearZero[4] = { 0, 0, 0, 0 };
	context->ClearUnorderedAccessViewUint(tableUAV, clearZero);

	SmoothCB cb{};
	cb.VertexCount = vertexCount;
	cb.StrideBytes = stride;
	cb.NormalOffsetBytes = normalOffset;
	cb.PosIsFloat32 = positionBytes >= 16 ? 1u : 0u;
	cb.TableMask = tableSlots - 1;
	cb.ColorOffsetBytes = colorOffset;
	cb.HasColor = hasColor ? 1u : 0u;
	cb.OutBase = chunkOffset;
	const uint32_t boundsSlot = (smoothBoundsCS && meshBounds && meshBounds->uav) ? meshBoundsFree.Allocate(1) : UINT32_MAX;
	const bool wantBounds = boundsSlot != UINT32_MAX;
	cb.BoundsSlot = wantBounds ? boundsSlot : 0u;

	// Clusters: at most one thread group's worth per mesh, so the cull pass is
	// a single chunk (a barrier under a buffer-driven trip count is
	// non-uniform flow control and will not compile). 64 triangles each until
	// that would exceed the cap, proportionally larger after.
	const uint32_t indexCount = triangleCount * 3;
	uint32_t clusterTris = kClusterTrisBase;
	uint32_t clusterCount = (indexCount / 3 + clusterTris - 1) / std::max(clusterTris, 1u);
	if (clusterCount > kClusterGroup) {
		clusterTris = (indexCount / 3 + kClusterGroup - 1) / kClusterGroup;
		clusterCount = (indexCount / 3 + clusterTris - 1) / std::max(clusterTris, 1u);
	}
	bool wantClusters = smoothClusterCS && gameIB && indexCount >= 3 && EnsureClusterResources(clusterScratchNeeded);
	uint32_t clusterSlot = UINT32_MAX;
	uint32_t poolOffset = UINT32_MAX;
	if (wantClusters) {
		clusterSlot = clusterFree.Allocate(clusterCount);
		if (clusterSlot != UINT32_MAX)
			poolOffset = clusterIndexPoolFree.Allocate(indexCount, 8);
		if (poolOffset == UINT32_MAX) {
			if (clusterSlot != UINT32_MAX)
				clusterFree.Release(clusterSlot, clusterCount);
			wantClusters = false;
		}
	}
	cb.IndexPoolOffset = wantClusters ? poolOffset : 0u;
	cb.IndexCount = wantClusters ? indexCount : 0u;
	cb.ClusterOffset = wantClusters ? clusterSlot : 0u;
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
	ID3D11ShaderResourceView* csSRV = smoothSourceSRV.get();
	context->CSSetShaderResources(0, 1, &csSRV);
	ID3D11UnorderedAccessView* csUAVs[2] = { tableUAV, smoothedNormalsChunks[chunk].uav.get() };
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
		entry.boundsSlot = boundsSlot;
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
		entry.clusterOffset = clusterSlot;
		entry.clusterCount = clusterCount;
		entry.indexPoolOffset = poolOffset;
		entry.indexCount = indexCount;
	}

	ID3D11ShaderResourceView* nullCsSRV = nullptr;
	context->CSSetShaderResources(0, 1, &nullCsSRV);
	ID3D11UnorderedAccessView* nullCsUAVs[2] = { nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, nullCsUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	entry.srv = outSRV;
	entry.ready = true;
	return &entry;
}

void SnowDeformation::DrawCapturedStatics()
{
	// Every early exit ends the decal overlay's frame: a list left standing
	// keeps DrawShell's pre-snow copies running with nothing to read them.
	auto endOverlay = [this] { DrawBloodOverlay(globals::d3d::context, nullptr); };
	if (SnowShadersPending(2)) {
		endOverlay();
		return;
	}
	if (cpuCensusArmed) {
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
			mix(&cap.fadeExempt, sizeof(cap.fadeExempt));
			mix(&cap.projThreshold, sizeof(cap.projThreshold));
			mix(&cap.projNoiseScale, sizeof(cap.projNoiseScale));
			mix(&cap.projNoiseTiling, sizeof(cap.projNoiseTiling));
			mix(&cap.projNoiseWeight, sizeof(cap.projNoiseWeight));
			mix(&cap.forceRounded, sizeof(cap.forceRounded));
			mix(&cap.plankFamily, sizeof(cap.plankFamily));
			mix(&cap.projReal, sizeof(cap.projReal));
			mix(&cap.fullCoat, sizeof(cap.fullCoat));
			mix(&cap.alphaTested, sizeof(cap.alphaTested));
			mix(&cap.snowTex, sizeof(cap.snowTex));
			h += e;
		}
		cpuCensus.captureHash = h;
		cpuCensus.captureCount = (uint32_t)capturedStatics.size();
	}
	LoadTraceScope _loadTrace(this, "Statics: DrawCapturedStatics");
	// The cover always draws (minimum coat); sliders never disable it.
	if (capturedStatics.empty()) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Objects - Setup & Cull");
		globals::profiler->MarkPassSkipped("SnowDeformation::Decals - Overlay");
		globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Trench Patch");
		endOverlay();
		return;
	}
	// Not on the world map: skins strobed there as the panning camera
	// crossed capture gates, and a skin without the shell beside it only
	// mismatches the map. Mirrors the recolor gate in GetCommonSettingsGPU.
	if (globals::state->isMapMenuOpen || !EnsureStaticsShaders()) {
		endOverlay();
		return;
	}

	// The skins' depth prepass (see the fills below), decided here: the
	// pre-snow copy DrawShell left to this pass rides its first fill.
	auto* fillVS = GetShellFillVS();
	auto* fillPS = fillVS ? GetShellFillPS() : nullptr;
	auto mainDepthSRV = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	const bool prepass = staticsPSPrepassNoDepth && fillPS && mainDepthSRV && EnsurePrepassResources(mainDepthSRV);
	// No statics draw has written the main depth yet. The first fill writes
	// the copy where its viewport is exactly the copy's rectangle; otherwise
	// it is taken now.
	bool fillPreSnow = false;
	if (preSnowDepthDeferred) {
		preSnowDepthDeferred = false;
		D3D11_VIEWPORT vp0{};
		UINT vpOne = 1;
		globals::d3d::context->RSGetViewports(&vpOne, &vp0);
		D3D11_TEXTURE2D_DESC preDesc{};
		preSnowDepth->GetDesc(&preDesc);
		const D3D11_BOX fillBox = ScreenRectBox(vpOne ? &vp0 : nullptr, preDesc.Width, preDesc.Height, 0);
		const bool wholePixels = vpOne && vp0.TopLeftX == std::floor(vp0.TopLeftX) && vp0.TopLeftY == std::floor(vp0.TopLeftY) &&
		                         vp0.Width == std::floor(vp0.Width) && vp0.Height == std::floor(vp0.Height);
		fillPreSnow = prepass && preSnowDepthRTV && GetShellFillPSPreSnow() && wholePixels &&
		              std::memcmp(&fillBox, &preSnowRect, sizeof(D3D11_BOX)) == 0;
		if (!fillPreSnow)
			TakePreSnowCopy(globals::d3d::context, mainDepthSRV);
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
	// The Snow Follows Parallax VS is bound (drapes with a height map).
	bool skinVSParallax = false;
	auto bindSkinStages = [&](bool a_tess) {
		skinStagesTess = a_tess;
		skinVSParallax = false;
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

	// Volume seed debug view: level 0's occupancy, field, heightmap and the
	// seed maps for the skin PS, the level's constants at b3.
	ID3D11Buffer* savedPSCB3 = nullptr;
	const bool volumeSeedView = staticsDebugView == 10 && settings.VolumeSnow && voxelLevels[0].valid &&
	                            voxelLevels[0].volume[voxelLevels[0].current] && voxelLevels[0].field && voxelLevels[0].heightMap && voxelLevels[0].heightMapUnder;
	if (volumeSeedView) {
		if (!voxelDebugCB) {
			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = sizeof(VoxelVolumeCB);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			voxelDebugCB = new ConstantBuffer(cbDesc, "SnowDeformation::VoxelSeedDebugCB");
		}
		VoxelVolumeCB vcb{};
		FillVoxelCB(0, vcb);
		voxelDebugCB->Update(vcb);
		context->PSGetConstantBuffers(3, 1, &savedPSCB3);
		ID3D11Buffer* cb3 = voxelDebugCB->CB();
		context->PSSetConstantBuffers(3, 1, &cb3);
		auto& lv = voxelLevels[0];
		ID3D11ShaderResourceView* seedSRVs[4] = { lv.volume[lv.current]->srv.get(), lv.field->srv.get(), lv.heightMap->srv.get(),
			(heightBottomFiltered && heightBottomFiltered->srv) ? heightBottomFiltered->srv.get() : nullptr };
		context->PSSetShaderResources(46, 4, seedSRVs);
		ID3D11ShaderResourceView* skySRV = (objectSkyOpen && objectSkyOpen->srv) ? objectSkyOpen->srv.get() : nullptr;
		context->PSSetShaderResources(51, 1, &skySRV);
		ID3D11ShaderResourceView* underSRV = lv.heightMapUnder->srv.get();
		context->PSSetShaderResources(54, 1, &underSRV);
	}

	globals::profiler->BeginPass("SnowDeformation::Objects - Setup & Cull");
	// One-shot skip diagnostics: geometries that capture but cannot draw are
	// the "why is THIS rock bare" cases; name the reason in the log.
	static std::unordered_set<std::string> loggedSkips;
	auto logSkip = [this](RE::BSGeometry* a_geometry, const char* a_reason) {
		if (skinCullLogArmed || (loggedSkips.size() < 24 && loggedSkips.insert(std::string(a_geometry->name.c_str()) + a_reason).second))
			logger::info("[SNOW DEFORMATION] Statics skip '{}': {}", a_geometry->name.c_str(), a_reason);
	};

	// Object top raster (PS t11): the skin PS reads it to keep its rim wall
	// alive through the steepness gates. Same raster the patch binds to its
	// VS further down.
	ID3D11ShaderResourceView* objectTopSRV = (heightTopRaw[heightCurrent] && heightTopRaw[heightCurrent]->srv) ?
	                                             heightTopRaw[heightCurrent]->srv.get() :
	                                             nullptr;
	context->PSSetShaderResources(11, 1, &objectTopSRV);
	// Untouched tiles (PS t57): the march skips carve and berm reads over
	// them, as the ground's does. DrawShell unbinds the slot; the skins, the
	// patch and the volume take it from here, and the pass unbinds it last.
	ID3D11ShaderResourceView* untouchedSRV = untouchedTilesSRV.get();
	context->PSSetShaderResources(kUntouchedTilesSlot, 1, &untouchedSRV);
	// Scene depth copy (PS t3): the SSS hug gate, ported from the terrain
	// shell, reads it. Same helper and same slot the shell binds - a COPY
	// (Terrain Blending's blended depth when that feature owns it, else
	// kPOST_ZPREPASS_COPY), never the bound DSV, so sampling it while this
	// pass writes depth is legal.
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
	// Land-exact layer (t52): the drape finds the landscape sheet's drawn surface with it.
	ID3D11ShaderResourceView* landFineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
	context->PSSetShaderResources(52, 1, &landFineSRV);
	// Near clipmap top (t34): the top raster at one unit per texel over the
	// inner window. Every reader falls back to t11 outside it, so a null bind
	// here is simply the coarse behaviour.
	ID3D11ShaderResourceView* fineTopSRV = (FineLevelOn() && heightTopRawFine && heightTopRawFine->srv) ? heightTopRawFine->srv.get() : nullptr;
	context->VSSetShaderResources(34, 1, &fineTopSRV);
	context->DSSetShaderResources(34, 1, &fineTopSRV);
	context->PSSetShaderResources(34, 1, &fineTopSRV);
	// Far road level (t40): PatchTop's fallback past the coarse window. Null
	// when the level is off; FarHalfExtent is 0 then, so nothing reads it.
	ID3D11ShaderResourceView* farTopSRV = (farHalfExtentLive > 0.0f && heightTopRawFar && heightTopRawFar->srv) ? heightTopRawFar->srv.get() : nullptr;
	context->VSSetShaderResources(40, 1, &farTopSRV);
	context->DSSetShaderResources(40, 1, &farTopSRV);
	context->PSSetShaderResources(40, 1, &farTopSRV);
	context->HSSetShaderResources(40, 1, &farTopSRV);
	// Wide exclusion field (t15): the skin's self-shadow march reads the
	// landscape shell's slot; the skins draw standalone, so bind explicitly here.
	ID3D11ShaderResourceView* skinExclusionSRV = GetExclusionFieldSRV();
	context->PSSetShaderResources(15, 1, &skinExclusionSRV);
	ID3D11ShaderResourceView* skinBloodSRVs[2] = { GetBloodMapSRV(), GetBloodClockSRV() };
	context->PSSetShaderResources(30, 2, skinBloodSRVs);
	// The S4 shell's per-pixel footprint cut: vanilla's noise map (PS t21).
	// Bound whenever it exists: the coat's contour is built from it, and the
	// coat draws with the 3D rise off.
	ID3D11ShaderResourceView* projNoiseSRV = SD_ProjNoiseMapSRV();
	context->PSSetShaderResources(21, 1, &projNoiseSRV);
	// The pre-shell Masks copy: the recolor's real projected weight.
	ID3D11ShaderResourceView* skinMasksSRV = landMasksCopySRV.get();
	context->PSSetShaderResources(32, 1, &skinMasksSRV);
	// The same note from before the frame's first decal-mode draw blended it.
	ID3D11ShaderResourceView* preDecalSRV = preDecalMasksSRV.get();
	context->PSSetShaderResources(53, 1, &preDecalSRV);
	// The game's shadow mask: the drape lies on the geometry it was drawn
	// for and takes its shadows from it (sun in x, local lights by mask
	// index), as Lighting does at t14.
	ID3D11ShaderResourceView* gameShadowMaskSRV = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK].SRV;
	context->PSSetShaderResources(24, 1, &gameShadowMaskSRV);
	// The print map and its tiles (t114-t115), for the drapes' print twin.
	BindPrintReaders(context);
	// Rune glyphs and frost marks, on object snow lying on a surface their
	// decal covers - with the overlay that would otherwise put the decal back.
	BindMarkReaders(context, CoatPaintsMarks());

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
		ID3D11ShaderResourceView* smoothSRV;
		// A drape over prints: shaded by the print twin.
		bool prints;
		// Snow Follows Parallax: a drape with a height map draws through the
		// parallax VS with this layout (null otherwise).
		ID3D11InputLayout* layoutParallax;
	};
	std::vector<SkinDraw> skinDraws;
	skinDraws.reserve(capturedStatics.size());
	const bool coatParallax = settings.SnowFollowsParallax && EnsureCoatParallaxShaders();
	// Grown a frame late from what the last frame asked for: the allocation
	// below has to hand out ranges before the total is known.
	EnsureClusterResources(clusterScratchNeeded);
	uint32_t scratchNext = 0;
	uint32_t scratchWanted = 0;
	uint32_t printTwinDraws = 0;
	uint32_t parallaxCaptures = 0;
	uint32_t parallaxCoatDraws = 0;
	for (auto& cap : capturedStatics) {
		auto* geometry = cap.geometry.get();
		if (!geometry)
			continue;
		parallaxCaptures += cap.parallaxSRV ? 1u : 0u;
		// THE OLD OBJECT SHELL IS RETIRED (Josef, 2026-08-29 - at fill 0
		// its no-PD pillows stood alone on the steps, and at fill 100 the
		// S4 shell stacked on top of them). Only two things draw now:
		// roads (their own tuned machinery, always) and the S4 shell
		// (PD-carrying draws, gated by "3D Snow on Objects"). Draws whose
		// PROPERTY carries no projection data get no skin at all - the
		// Lighting recolor still covers the technique-classified ones
		// (fence family) flat. The classic shader path survives only
		// because roads run through it.
		// The sentinel is -1; an authored bias can sit at exactly -0.5 (the
		// farmhouse snow material), which the old > -0.5 read as no data.
		const bool s4Shell = !cap.road && cap.projThreshold > -0.9f && projNoiseSRV;
		if (!cap.road && !s4Shell) {
			if (skinCullLogArmed)
				logger::info("[SNOW DEFORMATION] skin list skip (no projection data, threshold {:.2f}): '{}'", cap.projThreshold, geometry->name.c_str());
			continue;
		}
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
		// sight; the draw loop reads the view kept here.
		const auto* smooth = SmoothedNormalsFor(cap);
		const bool cached = smooth && smooth->ready;
		ID3D11ShaderResourceView* smoothSRV = cached ? smooth->srv.get() : nullptr;
		uint32_t cOffset = 0, cCount = 0, cPool = 0, cBase = 0;
		// Clusters need a contiguous slice of the scratch buffer, handed out
		// here so the pass writes to a known place; a mesh that does not fit
		// simply draws its own index buffer entire.
		if (cached && smooth->clusterCount != 0)
			scratchWanted += smooth->indexCount;
		if (cached && smooth->clusterCount != 0 &&
			scratchNext + smooth->indexCount <= clusterScratchCapacity) {
			cOffset = smooth->clusterOffset;
			cCount = smooth->clusterCount;
			cPool = smooth->indexPoolOffset;
			cBase = scratchNext;
			scratchNext += smooth->indexCount;
		}
		skinDraws.push_back({ &cap, geometry,
			reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer),
			reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer),
			layout, stride, indexCount,
			float(triShape->GetTrishapeRuntimeData().vertexCount), s4Shell,
			uint32_t(skinDraws.size()), cached ? smooth->boundsSlot : UINT32_MAX,
			cOffset, cCount, cPool, cBase, cap.decalDepth, smoothSRV, s4Shell && PrintsUnder(geometry->worldBound),
			(coatParallax && s4Shell && cap.parallaxSRV) ? StaticsParallaxInputLayoutFor(descKey, desc) : nullptr });
		printTwinDraws += skinDraws.back().prints ? 1u : 0u;
		parallaxCoatDraws += skinDraws.back().layoutParallax ? 1u : 0u;
	}
	printTwinDrawsLast = printTwinDraws;
	parallaxCapturesLast = parallaxCaptures;
	parallaxCoatDrawsLast = parallaxCoatDraws;

	auto statsRow = [](const char* a_name) {
		globals::profiler->EndPass();
		globals::profiler->BeginPass(a_name);
	};
	// Whole-skin occlusion cull: bounding spheres against a max-depth
	// pyramid of the scene, on the GPU, same frame. Writes each skin's
	// indirect draw arguments; the loops below draw through them.
	auto* hiZBuild = GetHiZBuildCS();
	auto* skinCull = hiZBuild ? GetSkinCullCS() : nullptr;
	// The pyramid covers the rendered rectangle (dynamic resolution draws into
	// a corner of the display-size depth); every read is clamped to it.
	const uint32_t hiZExtentW = vpCount == 0 ? UINT32_MAX : uint32_t(std::ceil(vps[0].TopLeftX + vps[0].Width));
	const uint32_t hiZExtentH = vpCount == 0 ? UINT32_MAX : uint32_t(std::ceil(vps[0].TopLeftY + vps[0].Height));
	const bool cullActive = skinCull && !skinDraws.empty() && mainDepthSRV && vpCount > 0 &&
	                        EnsureSkinCullResources(uint32_t(skinDraws.size()), mainDepthSRV, hiZExtentW, hiZExtentH);
	// Every drawn skin added its footprint: the culls ran over all of them.
	bool skinFootprintLive = false;
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
		// Debug views draw every skin at full visibility, past the range too.
		const float skinFadeEnd = staticsDebugView != 0 ? 0.0f : SkinFadeEndUnits();
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
					{ rot.entry[2][0] * scale, rot.entry[2][1] * scale, rot.entry[2][2] * scale, d.cap->world.translate.z },
					d.cap->fadeExempt ? 0.0f : skinFadeEnd };
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
		ID3D11ShaderResourceView* nullCullSRVs[4] = {};
		ID3D11UnorderedAccessView* nullCullUAVs[2] = {};
		// The scratch-copy chain below stands in when the UAV build fails to compile.
		auto* hiZBuildUAV = GetHiZBuildCS(true);
		if (hiZBuildUAV && skinCullHiZUAVs.size() == skinCullLevels) {
			// Level 0 folds the viewport-masked depth into mip 0; every later
			// level reads the previous mip through its UAV and writes its own,
			// so the chain is never bound as an SRV while it is written.
			ccb.Level = 0.0f;
			skinCullCB->Update(ccb);
			context->CSSetShader(hiZBuild, nullptr, 0);
			ID3D11UnorderedAccessView* mip0 = skinCullHiZUAVs[0].get();
			context->CSSetShaderResources(1, 1, &mainDepthSRV);
			context->CSSetUnorderedAccessViews(2, 1, &mip0, nullptr);
			context->Dispatch((skinCullHiZ->desc.Width + 7) / 8, (skinCullHiZ->desc.Height + 7) / 8, 1);
			context->CSSetShaderResources(1, 1, nullCullSRVs);
			context->CSSetUnorderedAccessViews(2, 1, nullCullUAVs, nullptr);
			context->CSSetShader(hiZBuildUAV, nullptr, 0);
			for (uint32_t level = 1; level < skinCullLevels; level++) {
				ID3D11UnorderedAccessView* prev = skinCullHiZUAVs[level - 1].get();
				ID3D11UnorderedAccessView* dst = skinCullHiZUAVs[level].get();
				context->CSSetUnorderedAccessViews(6, 1, &prev, nullptr);
				context->CSSetUnorderedAccessViews(2, 1, &dst, nullptr);
				const uint32_t w = std::max(1u, skinCullHiZ->desc.Width >> level);
				const uint32_t h = std::max(1u, skinCullHiZ->desc.Height >> level);
				context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
			}
			context->CSSetUnorderedAccessViews(6, 1, nullCullUAVs, nullptr);
			context->CSSetUnorderedAccessViews(2, 1, nullCullUAVs, nullptr);
		} else {
			context->CSSetShader(hiZBuild, nullptr, 0);
			for (uint32_t level = 0; level < skinCullLevels; level++) {
				ccb.Level = float(level);
				skinCullCB->Update(ccb);
				// Read level-1 of the chain, write the level's scratch, copy it in.
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
		}

		context->CSSetShader(skinCull, nullptr, 0);
		ID3D11ShaderResourceView* cullSRVs[3] = { skinCullBounds->srv.get(), skinCullHiZ->srv.get(),
			meshBounds ? meshBounds->srv.get() : nullptr };
		context->CSSetShaderResources(2, 3, cullSRVs);
		ID3D11UnorderedAccessView* argsUAV = skinCullArgs->uav.get();
		context->CSSetUnorderedAccessViews(3, 1, &argsUAV, nullptr);
		ID3D11UnorderedAccessView* footprintUAV = skinFootprint->uav.get();
		const UINT footprintClear[4] = {};
		context->ClearUnorderedAccessViewUint(footprintUAV, footprintClear);
		context->CSSetUnorderedAccessViews(7, 1, &footprintUAV, nullptr);
		context->Dispatch((uint32_t(skinDraws.size()) + 63) / 64, 1, 1);

		// Cluster pass: refines the arguments of every skin the whole-skin
		// test kept, and compacts what survives into the scratch buffer both
		// draw loops read.
		auto* clusterCull = clusterSkins > 0 ? GetClusterCullCS() : nullptr;
		const bool clusterPass = clusterCull && clusterScratchIB && clusterScratchIB->uav;
		skinFootprintLive = clusterSkins == 0 || clusterPass;
		if (clusterPass) {
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
		context->CSSetUnorderedAccessViews(7, 1, nullCullUAVs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);

		// Census: the arguments and the pyramid's top texel two frames back,
		// read without waiting.
		context->CopyResource(skinCullArgsStaging[skinCullRing].get(), skinCullArgs->resource.get());
		if (skinCullHiZTopStaging[skinCullRing])
			context->CopySubresourceRegion(skinCullHiZTopStaging[skinCullRing].get(), 0, 0, 0, 0, skinCullHiZ->resource.get(), skinCullLevels - 1, nullptr);
		skinCullStagingIssued[skinCullRing] = true;
		skinCullStagingCount[skinCullRing] = uint32_t(skinDraws.size());
		if (skinCullLogArmed) {
			auto& names = skinCullLogNames[skinCullRing];
			names.assign(skinDraws.size(), {});
			for (const auto& d : skinDraws)
				if (d.slot < names.size())
					names[d.slot] = std::format("{}{}", d.decalDepth ? "[decal] " : "", d.geometry ? d.geometry->name.c_str() : "?");
			skinCullLogArmed = false;
		}
		const int readRing = (skinCullRing + 1) % kSkinCullRing;
		if (skinCullStagingIssued[readRing]) {
			D3D11_MAPPED_SUBRESOURCE rd{};
			if (SUCCEEDED(context->Map(skinCullArgsStaging[readRing].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &rd))) {
				const auto* args = static_cast<const uint32_t*>(rd.pData);
				uint32_t drawn = 0, culled = 0, trisDrawn = 0;
				uint32_t reasons[kSkinCullReasons] = {};
				for (uint32_t i = 0; i < skinCullStagingCount[readRing]; i++) {
					if (args[i * 5 + 1]) {
						drawn++;
						// The cluster pass rewrites this to what survived.
						trisDrawn += args[i * 5] / 3;
					} else
						culled++;
					reasons[std::min(args[i * 5 + 4], kSkinCullReasons - 1)]++;
				}
				auto& names = skinCullLogNames[readRing];
				if (!names.empty()) {
					static constexpr const char* kReason[kSkinCullReasons] = { "visible", "unprojectable", "no occluder", "outside view", "past far", "occluded", "eye plane", "huge", "past range" };
					for (uint32_t i = 0; i < skinCullStagingCount[readRing] && i < names.size(); i++)
						if (args[i * 5 + 1] == 0 || names[i].starts_with("[decal]"))
							logger::info("[SNOW DEFORMATION] skin cull slot {} inst {} idx {} reason {} ({}): '{}'", i, args[i * 5 + 1], args[i * 5], args[i * 5 + 4], kReason[std::min(args[i * 5 + 4], kSkinCullReasons - 1)], names[i]);
					names.clear();
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
	// Decided before the records: SkinSssSeen tells each skin which mask it reads.
	const bool skinSssMarch = prepass && !skinDraws.empty() && (fillPreSnow || preSnowDepthThisFrame) && SkinSssMarchReady(vpCount ? &vps[0] : nullptr);
	if (!skinSssMarch) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Shadows - Snow Depth Sync");
	}
	// Kept across frames, as sCaptureRecords.
	static std::vector<StaticsCB> sSkinRecords;
	sSkinRecords.assign(skinDraws.size(), StaticsCB{});
	auto& skinRecords = sSkinRecords;
	std::vector<ID3D11ShaderResourceView*> skinSmoothSRVs(skinDraws.size(), nullptr);
	for (size_t si = 0; si < skinDraws.size(); si++) {
		const auto& d = skinDraws[si];
		skinSmoothSRVs[si] = d.smoothSRV;
		FillSkinDrawCB(*d.cap, d.s4Shell, d.vertexCount,
			skinSmoothSRVs[si] != nullptr, objectTopSRV != nullptr, skinRecords[si]);
		skinRecords[si].SkinSssSeen = skinSssMarch ? 1.0f : 0.0f;
		if (d.layoutParallax) {
			skinRecords[si].ObjParallaxUV = d.cap->parallaxUV;
			skinRecords[si].ObjParallax = d.cap->parallax;
		}
		// Debug views only. Stable per object: the submission order moves
		// with the camera.
		skinRecords[si].DebugSkinId = 0.0f;
		if (staticsDebugView != 0) {
			const auto& tr = d.cap->world.translate;
			uint32_t idHash = uint32_t(std::hash<std::string_view>{}(d.geometry ? std::string_view(d.geometry->name.c_str()) : std::string_view{}));
			idHash ^= std::bit_cast<uint32_t>(tr.x) * 0x9E3779B1u ^ std::bit_cast<uint32_t>(tr.y) * 0x85EBCA77u ^ std::bit_cast<uint32_t>(tr.z);
			skinRecords[si].DebugSkinId = (d.decalDepth ? -1.0f : 1.0f) * float(1 + idHash % 4093u);
		}
	}
	const bool skinRecordsLive = !skinRecords.empty() && UploadStaticsRecords(skinRecords.data(), (uint32_t)skinRecords.size());
	uint32_t skinParity = 0;

	auto drawSkins = [&](bool a_prepass) {
		boundStaticsPS = nullptr;
		uint32_t drawIndex = 0;
		bool boundDecal = false;
		ID3D11ShaderResourceView* boundParallaxSRV = nullptr;
		SD_DrawBinds binds;
		for (const auto& d : skinDraws) {
			const auto& cap = *d.cap;
			const uint32_t recordIndex = drawIndex++;
			// Depth export only where the carve can fire: SnowStaticsShell's
			// carveObject is LegacySkin, i.e. cap.road. Everything else writes
			// back the rasterised depth, so dropping the export leaves the same
			// number in the buffer and hands early-Z rejection back to the whole
			// pass. The prepass twins mirror the same split so the private
			// depth holds exactly what the shipping shaders would have written.
			// Depth fight view: every skin through the prepass, roads too.
			const bool needsDepth = staticsDebugView != 9 && cap.road;
			// Skipped before any state: each drawn skin sets all it reads.
			if (a_prepass && needsDepth)
				continue;
			if (d.decalDepth != boundDecal) {
				boundDecal = d.decalDepth;
				context->RSSetState(GetSkinRasterState(boundDecal));
				if (vpCount)
					context->RSSetViewports(vpCount, boundDecal ? decalVps : vps);
			}
			[[maybe_unused]] auto* geometry = d.geometry;
			binds.Layout(context, d.layoutParallax ? d.layoutParallax : d.layout);
			binds.Vertex(context, d.vb, d.stride);
			// Cluster skins draw the compacted stream; everything else draws
			// the mesh's own indices, as before.
			if (cullActive && d.clusterCount != 0)
				binds.Index(context, clusterScratchIB->resource.get(), DXGI_FORMAT_R32_UINT);
			else
				binds.Index(context, d.ib, DXGI_FORMAT_R16_UINT);

			// Smoothed normals (built once per unique mesh): pillow inflation
			// for flat split-normal surfaces; planks, roofs, pole caps.
			ID3D11ShaderResourceView* skinSmoothSRV = skinSmoothSRVs[recordIndex];
			binds.Smooth(context, skinSmoothSRV);
			const bool wantTess = tessellateSkins && !d.s4Shell;
			if (wantTess != skinStagesTess)
				bindSkinStages(wantTess);
			// Both loops: the prepass and the shading draw must raster the same VS.
			const bool wantParallaxVS = !wantTess && d.layoutParallax != nullptr;
			if (wantParallaxVS != skinVSParallax) {
				context->VSSetShader(wantParallaxVS ? staticsVSParallax : staticsVS, nullptr, 0);
				skinVSParallax = wantParallaxVS;
			}
			if (!a_prepass) {
				ID3D11ShaderResourceView* parallaxSRV = d.layoutParallax ? cap.parallaxSRV.get() : nullptr;
				if (parallaxSRV != boundParallaxSRV) {
					context->PSSetShaderResources(59, 1, &parallaxSRV);
					boundParallaxSRV = parallaxSRV;
				}
			}
			if (skinRecordsLive) {
				BindStaticsRecord(recordIndex, true, wantTess, skinParity);
			} else {
				staticsCB->Update(skinRecords[recordIndex]);
				cpuCensus.skinLoopCBUpdates++;
			}

			// Drapes take the SNOW_STATICS_DRAPE twins, which fold exactly
			// what FillSkinDrawCB gives every S4 draw, with no debug view;
			// drapes over prints shade through the print twin.
			const bool drapePS = d.s4Shell && staticsDebugView == 0;
			ID3D11PixelShader* drapeShadePS = (d.prints && staticsPSDrapePrintsNoDepth) ? staticsPSDrapePrintsNoDepth : staticsPSDrapeNoDepth;
			if (d.layoutParallax)
				drapeShadePS = (d.prints && staticsPSDrapePrintsNoDepth) ? (staticsPSDrapePrintsParallax ? staticsPSDrapePrintsParallax : staticsPSDrapePrintsNoDepth) : staticsPSDrapeParallax;
			ID3D11PixelShader* wantPS = nullptr;
			if (a_prepass)
				wantPS = (drapePS && staticsPSDrapePrepass) ? staticsPSDrapePrepass : staticsPSPrepassNoDepth;
			else if (!needsDepth && staticsPSNoDepth)
				wantPS = (drapePS && drapeShadePS) ? drapeShadePS : staticsPSNoDepth;
			else
				wantPS = staticsPS;
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
		// DepthSyncCS FillFootprint.
		static constexpr UINT kFillFootprintSlot = 62;
		auto* fillRectVS = skinFootprintLive ? GetShellFillRectVS() : nullptr;
		// a_footprint: over the drawn skins' footprint alone.
		auto fill = [&](ID3D11DepthStencilView* a_target, ID3D11ShaderResourceView* a_source, bool a_preSnow, bool a_footprint) {
			ID3D11RenderTargetView* preSnowRTV = preSnowDepthRTV.get();
			context->OMSetRenderTargets(a_preSnow ? 1 : 0, a_preSnow ? &preSnowRTV : nullptr, a_target);
			context->OMSetDepthStencilState(shellFillDepthState.get(), 0);
			context->RSSetState(shellRasterState.get());
			if (vpCount)
				context->RSSetViewports(vpCount, fillVps);
			context->IASetInputLayout(nullptr);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			context->VSSetShader(a_footprint ? fillRectVS : fillVS, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->PSSetShaderResources(9, 1, &a_source);
			context->PSSetShader(a_preSnow ? GetShellFillPSPreSnow() : fillPS, nullptr, 0);
			if (a_footprint) {
				winrt::com_ptr<ID3D11ShaderResourceView> savedFootprintSRV;
				context->VSGetShaderResources(kFillFootprintSlot, 1, savedFootprintSRV.put());
				ID3D11ShaderResourceView* footprintSRV = skinFootprint->srv.get();
				context->VSSetShaderResources(kFillFootprintSlot, 1, &footprintSRV);
				context->Draw(6, 0);
				footprintSRV = savedFootprintSRV.get();
				context->VSSetShaderResources(kFillFootprintSlot, 1, &footprintSRV);
			} else {
				context->Draw(3, 0);
			}
			ID3D11ShaderResourceView* nullFillSRV = nullptr;
			context->PSSetShaderResources(9, 1, &nullFillSRV);
			if (vpCount)
				context->RSSetViewports(vpCount, vps);
			if (auto* skinRaster = GetSkinRasterState())
				context->RSSetState(skinRaster);
			bindSkinStages(skinStagesTess);
		};

		// The decal overlay leaves the landscape shell's pixels to the blood
		// detail tiles; it tells them by the depth the skins did not touch.
		bloodPreSkinDepthThisFrame = false;
		const D3D11_BOX skinExtent = ScreenRectBox(vpCount ? &vps[0] : nullptr, shellRasterDepth->desc.Width, shellRasterDepth->desc.Height, 0);
		if (BloodTilesLive() && !bloodOverlays.empty() && GetPreSnowCopyCS() && depthSyncRectCB &&
			EnsureBloodPreSkinDepth(std::max(skinExtent.right, 1u), std::max(skinExtent.bottom, 1u))) {
			statsRow(SnowDevSwitches::DecalOverlayRow("SnowDeformation::Decals - Overlay Depth Copy"));
			// The composite reads it inside the decals' rectangle only.
			const RECT& rc = bloodCopyRect;
			CopyDepthRect(context, mainDepthSRV, D3D11_BOX{ UINT(rc.left), UINT(rc.top), 0, UINT(rc.right), UINT(rc.bottom), 1 }, bloodPreSkinDepthUAV.get());
			bloodPreSkinDepthThisFrame = true;
		} else {
			globals::profiler->MarkPassSkipped("SnowDeformation::Decals - Overlay");
		}
		statsRow("SnowDeformation::Objects - Depth Prepass");
		// Off the footprint the private depth is read only by the pre-snow copy,
		// the skins' second SSS march, the decal overlay and the depth-fight view.
		const bool firstFillFootprint = fillRectVS && !fillPreSnow && !skinSssMarch && bloodOverlays.empty() && staticsDebugView != 9;
		fill(shellTestDepthDSV.get(), mainDepthSRV, fillPreSnow, firstFillFootprint);
		if (fillPreSnow) {
			globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Depth & Mask Copies");
			preSnowDepthThisFrame = true;
		}
		context->OMSetRenderTargets(0, nullptr, shellTestDepthDSV.get());
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
		// The hidden-quad exit reads the depth the private buffer was filled from.
		context->PSSetShaderResources(43, 1, &mainDepthSRV);
		drawSkins(true);
		ID3D11ShaderResourceView* nullPreSkinSRV = nullptr;
		context->PSSetShaderResources(43, 1, &nullPreSkinSRV);
		if (skinSssMarch) {
			statsRow("SnowDeformation::Shadows - Snow Depth Sync");
			MarchSkinSss(shellTestDepthSRV.get(), sceneDepthSRV, vpCount ? &vps[0] : nullptr);
		}
		statsRow("SnowDeformation::Objects - Shading");

		if (staticsDebugView == 9) {
			// Depth fight view: no depth test; every skin paints its verdict
			// against the main depth it was tested on (t43) and the prepass
			// result (t44).
			context->OMSetRenderTargets(8, rtvPtrs, nullptr);
			ID3D11ShaderResourceView* fightSRVs[2] = { mainDepthSRV, shellTestDepthSRV.get() };
			context->PSSetShaderResources(43, 2, fightSRVs);
			boundDepthState = nullptr;
			drawSkins(false);
			ID3D11ShaderResourceView* nullFightSRVs[2] = {};
			context->PSSetShaderResources(43, 2, nullFightSRVs);
		} else {
			context->OMSetRenderTargets(8, rtvPtrs, shellTestDepthDSV.get());
			boundDepthState = nullptr;
			drawSkins(false);
		}

		statsRow("SnowDeformation::Objects - Depth Prepass");
		fill(dsv.get(), shellTestDepthSRV.get(), false, fillRectVS != nullptr);
		context->OMSetRenderTargets(8, rtvPtrs, dsv.get());
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
	} else {
		statsRow("SnowDeformation::Objects - Shading");
		drawSkins(false);
	}
	globals::profiler->EndPass();
	// The private test depth holds the skins only after the prepass path.
	DrawBloodOverlay(context, prepass ? shellTestDepthSRV.get() : nullptr);

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
	context->PSSetShaderResources(59, 1, &nullSmoothSRV);
	context->HSSetShaderResources(13, 1, &nullSmoothSRV);
	context->PSSetShaderResources(21, 1, &nullSmoothSRV);
	context->PSSetShaderResources(24, 1, &nullSmoothSRV);
	context->PSSetShaderResources(32, 1, &nullSmoothSRV);
	context->PSSetShaderResources(52, 1, &nullSmoothSRV);
	if (volumeSeedView) {
		ID3D11ShaderResourceView* nullSeedSRVs[6] = {};
		context->PSSetShaderResources(46, 6, nullSeedSRVs);
		context->PSSetShaderResources(54, 1, nullSeedSRVs);
		context->PSSetConstantBuffers(3, 1, &savedPSCB3);
		if (savedPSCB3)
			savedPSCB3->Release();
	}
	// With the cull off the census never ran to disarm the one-shot.
	skinCullLogArmed = false;

	// trench PATCH: the landscape shell's dense-grid carve applied to object
	// tops; real carved geometry drawn after the skins so it shows through
	// their dithered trench hand-off holes. SV_VertexID grid, no IA state.
	// Per-class trenching emerges from the raster: each captured object
	// writes its own class depth into the skin-depth raster, so a class at
	// 0 produces dead patch texels for its objects only. Roads are the one
	// class with depth, so the gate is theirs (the old > 1 threshold
	// silently disabled the whole patch at depth 1). Both road rasters are
	// rebuilt from this frame's captures, so no road captured = every patch
	// vertex killed.
	const bool patchWanted = patchVS && patchPS && heightSkinDepth && settings.RoadMeshesDepth > 0.5f && !GroundShellsSuspended();
	if (patchWanted && capturedRoads == 0)
		globals::profiler->MarkPassSkipped("SnowDeformation::Trails - Trench Patch");
	if (patchWanted && capturedRoads > 0) {
		globals::profiler->BeginPass("SnowDeformation::Trails - Trench Patch");
		// Tessellated patch: quad patches with trench-aware factors, so the
		// object trenches pick up the same wall smoothness and rim relief as
		// the landscape shell. Self-sufficient bindings, same rationale as
		// the skins.
		const bool tessellatePatch = settings.Tessellation && patchTessVS && patchHS && patchDS;
		PatchBand patchBands[kPatchBands] = { { 0, kPatchGridDim, 0, kPatchGridDim } };
		uint32_t patchBandCount = 1;
		PatchCullCB cullData{};
		winrt::com_ptr<ID3D11Buffer> prevPatchVSCB11;
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
			context->HSSetShaderResources(40, 1, &farTopSRV);
			context->DSSetShaderResources(40, 1, &farTopSRV);
			ID3D11SamplerState* dsSampler = shellSnowSampler.get();
			context->DSSetSamplers(0, 1, &dsSampler);
			// The hull's frustum footprint (b11), from the b0 this draw projects
			// with; no lines culls nothing.
			if (!patchCullCB)
				patchCullCB = new ConstantBuffer(ConstantBufferDesc<PatchCullCB>(), "SnowDeformation::PatchCullCB");
			if (lastShellCBData)
				FillPatchCullLines(*lastShellCBData, cullData);
			// Only the lattice bands the lines can reach: the patches left out
			// would get zero factors, and rows stay in order.
			if (!patchBoxOff && cullData.LineCount)
				patchBandCount = PatchLatticeBands(cullData, PatchLatticeCentre(), patchBands);
			cullData.RowStart = patchBands[0].y0;
			cullData.ColStart = patchBands[0].x0;
			patchCullCB->Update(cullData);
			ID3D11Buffer* cullCB = patchCullCB->CB();
			context->HSSetConstantBuffers(11, 1, &cullCB);
			context->VSGetConstantBuffers(11, 1, prevPatchVSCB11.put());
			context->VSSetConstantBuffers(11, 1, &cullCB);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
		} else {
			context->VSSetShader(patchVS, nullptr, 0);
		}
		// Deferred: the geometry keeps the coverage cut alone, into the ground's
		// deferred targets, which are free once the ground has shaded; one
		// full-screen pass then shades each kept pixel once. Forward under the
		// debug views and until both deferred shaders exist.
		bool deferredPatch = staticsDebugView == 0 && PatchDeferredReady() && EnsurePatchDeferredResources();
		ID3D11VertexShader* patchFillVS = deferredPatch ? GetShellFillVS() : nullptr;
		deferredPatch = deferredPatch && patchFillVS;
		context->PSSetShader(deferredPatch ? patchDeferredPS[0] : patchPS, nullptr, 0);
		// Object top raster (PS t11): the patch's self-shadow march needs the
		// same footprint test as the skins - inside a footprint the tap
		// surface is the object top, not the terrain window's class ramp.
		ID3D11ShaderResourceView* patchTopSRV = heightTopRaw[heightCurrent]->srv.get();
		context->PSSetShaderResources(11, 1, &patchTopSRV);
		context->PSSetShaderResources(40, 1, &farTopSRV);
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
		// Rune glyphs and frost marks on the road's snow, as the landscape shell paints them.
		BindMarkReaders(context, true);

		// ONE recipe with the shadow caster (FillPatchDrawCB): the caster must
		// be the exact surface this draw renders.
		StaticsCB scb{};
		FillPatchDrawCB(scb);
		staticsCB->Update(scb);

		ID3D11ShaderResourceView* patchSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
		context->VSSetShaderResources(11, 2, patchSRVs);
		// Far road level (t40): the outer bands drape on it.
		context->VSSetShaderResources(40, 1, &farTopSRV);
		// Terrain window (t0) + object snow cone (t13) for the road-verge
		// depth blend, in whichever stage evaluates BuildPatchVertex (the VS
		// on the legacy grid, the DS when tessellating). Explicit binds: both
		// were previously reachable only through state left over from earlier
		// passes, which is one reorder from an unbound read.
		ID3D11ShaderResourceView* patchTerrainSRV = shellTerrainTexture ? shellTerrainTexture->srv.get() : nullptr;
		ID3D11ShaderResourceView* patchConeSRV = (objectSnowCone && objectSnowCone->srv) ? objectSnowCone->srv.get() : nullptr;
		context->VSSetShaderResources(0, 1, &patchTerrainSRV);
		context->VSSetShaderResources(13, 1, &patchConeSRV);
		// The march's road taps read the same cone.
		context->PSSetShaderResources(13, 1, &patchConeSRV);
		// Land-exact layer (t52): the landscape meet finds the sheet's drawn
		// surface with it, in whichever stage builds the patch vertex.
		ID3D11ShaderResourceView* patchLandFineSRV = (shellFineValid && shellTerrainFine) ? shellTerrainFine->srv.get() : nullptr;
		context->VSSetShaderResources(52, 1, &patchLandFineSRV);
		if (tessellatePatch) {
			context->DSSetShaderResources(0, 1, &patchTerrainSRV);
			context->DSSetShaderResources(13, 1, &patchConeSRV);
			context->DSSetShaderResources(52, 1, &patchLandFineSRV);
			// The hull's screen-size cap stands corners off any road on the terrain.
			context->HSSetShaderResources(0, 1, &patchTerrainSRV);
		}
		// The targets, depth and raster state the forward draw would have used;
		// the deferred passes borrow and restore them.
		winrt::com_ptr<ID3D11RenderTargetView> patchRTVs[8];
		ID3D11RenderTargetView* patchRTVPtrs[8] = {};
		winrt::com_ptr<ID3D11DepthStencilView> patchDSV;
		winrt::com_ptr<ID3D11DepthStencilState> patchDepthState;
		UINT patchStencilRef = 0;
		winrt::com_ptr<ID3D11RasterizerState> patchRaster;
		bool patchVerifyOn = false;
		if (deferredPatch) {
			ID3D11DepthStencilView* rawPatchDSV = nullptr;
			context->OMGetRenderTargets(8, patchRTVPtrs, &rawPatchDSV);
			for (uint32_t i = 0; i < 8; i++)
				patchRTVs[i].attach(patchRTVPtrs[i]);
			patchDSV.attach(rawPatchDSV);
			context->OMGetDepthStencilState(patchDepthState.put(), &patchStencilRef);
			context->RSGetState(patchRaster.put());
			const float markClear[4] = {};
			context->ClearRenderTargetView(patchDeferredMark->rtv.get(), markClear);
			if (patchVerify && tessellatePatch && EnsurePatchVerify(patchDSV.get())) {
				// The targets as the bands find them, for the whole lattice to draw over.
				context->OMSetRenderTargets(0, nullptr, nullptr);
				for (uint32_t i = 0; i < 3; i++)
					context->CopyResource(patchVerifyRT[i]->resource.get(), shellDeferredAttr[i]->resource.get());
				context->CopyResource(patchVerifyRT[3]->resource.get(), patchDeferredMark->resource.get());
				context->CopyResource(patchVerifyDepth.get(), patchVerifyRealDepth);
				patchVerifyOn = true;
			}
			ID3D11RenderTargetView* attrRTVs[4] = { shellDeferredAttr[0]->rtv.get(), shellDeferredAttr[1]->rtv.get(), shellDeferredAttr[2]->rtv.get(), patchDeferredMark->rtv.get() };
			context->OMSetRenderTargets(4, attrRTVs, patchDSV.get());
		}
		if (patchStatsArmed) {
			context->Begin(patchStatsQuery[shellStatsRing][0].get());
			context->Begin(patchStatsQuery[shellStatsRing][1].get());
		}
		if (tessellatePatch) {
			auto drawWholeLattice = [&]() {
				cullData.RowStart = 0;
				cullData.ColStart = 0;
				patchCullCB->Update(cullData);
				context->Draw(kPatchGridDim * kPatchGridDim * 4, 0);
			};
			patchBoxPatches = 0;
			if (patchBoxOff) {
				drawWholeLattice();
				patchBoxPatches = kPatchGridDim * kPatchGridDim;
			} else {
				for (uint32_t b = 0; b < patchBandCount; b++) {
					const auto& band = patchBands[b];
					if (b > 0) {
						cullData.RowStart = band.y0;
						cullData.ColStart = band.x0;
						patchCullCB->Update(cullData);
					}
					context->DrawInstanced((band.x1 - band.x0) * 4, band.y1 - band.y0, 0, 0);
					patchBoxPatches += (band.x1 - band.x0) * (band.y1 - band.y0);
				}
			}
			if (patchVerifyOn) {
				ID3D11RenderTargetView* verifyRTVs[4] = { patchVerifyRT[0]->rtv.get(), patchVerifyRT[1]->rtv.get(), patchVerifyRT[2]->rtv.get(), patchVerifyRT[3]->rtv.get() };
				context->OMSetRenderTargets(4, verifyRTVs, patchVerifyDSV.get());
				drawWholeLattice();
				context->OMSetRenderTargets(0, nullptr, nullptr);
				const UINT zero4[4] = {};
				context->ClearUnorderedAccessViewUint(patchVerifyAllUAV.get(), zero4);
				ID3D11ShaderResourceView* const pairs[5][2] = {
					{ shellDeferredAttr[0]->srv.get(), patchVerifyRT[0]->srv.get() }, { shellDeferredAttr[1]->srv.get(), patchVerifyRT[1]->srv.get() },
					{ shellDeferredAttr[2]->srv.get(), patchVerifyRT[2]->srv.get() }, { patchDeferredMark->srv.get(), patchVerifyRT[3]->srv.get() },
					{ patchVerifyRealDepthSRV.get(), patchVerifyDepthSRV.get() }
				};
				winrt::com_ptr<ID3D11ComputeShader> prevVerifyCS;
				context->CSGetShader(prevVerifyCS.put(), nullptr, nullptr);
				context->CSSetShader(heightRasterCompareCS, nullptr, 0);
				for (uint32_t t = 0; t < 5; t++) {
					ID3D11ShaderResourceView* srvs[2] = { pairs[t][0], pairs[t][1] };
					ID3D11UnorderedAccessView* uav = patchVerifySlotUAV[t].get();
					context->CSSetShaderResources(8, 2, srvs);
					context->CSSetUnorderedAccessViews(4, 1, &uav, nullptr);
					const auto& d = t < 3 ? shellDeferredAttr[t]->desc : patchDeferredMark->desc;
					context->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
				}
				ID3D11ShaderResourceView* nullPair[2] = {};
				ID3D11UnorderedAccessView* nullVerifyUAV = nullptr;
				context->CSSetShaderResources(8, 2, nullPair);
				context->CSSetUnorderedAccessViews(4, 1, &nullVerifyUAV, nullptr);
				context->CSSetShader(prevVerifyCS.get(), nullptr, 0);
				auto& ring = patchVerifyRing;
				if (!patchVerifyStagingBusy[ring % 3]) {
					context->CopyResource(patchVerifyStaging[ring % 3].get(), patchVerifyCount.get());
					patchVerifyStagingBusy[ring % 3] = true;
					ring++;
				}
			}
		} else {
			context->Draw(kPatchGridDim * kPatchGridDim * 6, 0);
		}
		if (tessellatePatch) {
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			ID3D11Buffer* nullCullCB = nullptr;
			context->HSSetConstantBuffers(11, 1, &nullCullCB);
			ID3D11Buffer* restoreVSCB11 = prevPatchVSCB11.get();
			context->VSSetConstantBuffers(11, 1, &restoreVSCB11);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}
		if (deferredPatch) {
			// Shading: every pixel the mark kept, against the forward draw's
			// bindings (b0, b1, the same SRVs and samplers).
			context->OMSetRenderTargets(8, patchRTVPtrs, nullptr);
			context->OMSetDepthStencilState(patchDeferredDepthState.get(), 0);
			context->RSSetState(shellRasterState.get());
			context->VSSetShader(patchFillVS, nullptr, 0);
			ID3D11PixelShader* patchConditionView = PatchConditionViewPS();
			context->PSSetShader(patchConditionView ? patchConditionView : patchDeferredPS[1], nullptr, 0);
			ID3D11ShaderResourceView* savedAttrSRVs[4] = {};
			context->PSGetShaderResources(91, 4, savedAttrSRVs);
			ID3D11ShaderResourceView* attrSRVs[4] = { shellDeferredAttr[0]->srv.get(), shellDeferredAttr[1]->srv.get(), shellDeferredAttr[2]->srv.get(), patchDeferredMark->srv.get() };
			context->PSSetShaderResources(91, 4, attrSRVs);
			context->Draw(3, 0);
			context->PSSetShaderResources(91, 4, savedAttrSRVs);
			for (auto* srv : savedAttrSRVs) {
				if (srv)
					srv->Release();
			}
			context->OMSetRenderTargets(8, patchRTVPtrs, patchDSV.get());
			context->OMSetDepthStencilState(patchDepthState.get(), patchStencilRef);
			context->RSSetState(patchRaster.get());
		}
		if (patchStatsArmed) {
			context->End(patchStatsQuery[shellStatsRing][1].get());
			context->End(patchStatsQuery[shellStatsRing][0].get());
			patchStatsArmed = false;
		}
		for (uint32_t slot = 0; slot < 3; slot++) {
			if (!patchVerifyStagingBusy[slot])
				continue;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(patchVerifyStaging[slot].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
				continue;
			const auto* words = static_cast<const uint32_t*>(mapped.pData);
			for (uint32_t t = 0; t < 5; t++)
				patchVerifyMismatch[t] += words[t * kHeightVerifySlotWords];
			patchVerifyFrames++;
			context->Unmap(patchVerifyStaging[slot].get(), 0);
			patchVerifyStagingBusy[slot] = false;
		}

		ID3D11ShaderResourceView* nullHeightSRVs[2] = { nullptr, nullptr };
		context->VSSetShaderResources(11, 2, nullHeightSRVs);
		context->PSSetShaderResources(13, 1, nullHeightSRVs);
		context->VSSetShaderResources(52, 1, nullHeightSRVs);
		context->DSSetShaderResources(52, 1, nullHeightSRVs);
		globals::profiler->EndPass();
	}

	// Volume snow last: it depth-tests against everything above, skins
	// included, and shades through the same material - marks as the skins take them.
	BindMarkReaders(context, CoatPaintsMarks());
	DrawVoxelSnow();
	BindMarkReaders(context, false);

	ID3D11ShaderResourceView* nullUntouchedSRV = nullptr;
	context->PSSetShaderResources(kUntouchedTilesSlot, 1, &nullUntouchedSRV);
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(1, 1, &nullCB);
	context->PSSetConstantBuffers(1, 1, &nullCB);
}

void SnowDeformation::RenderExclusionField(const ExclusionsCB& a_list)
{
	LoadTraceScope _loadTrace(this, "Statics: RenderExclusionField");
	if (!exclusionFieldTexture || !exclusionFieldCB || !shellTerrainTexture) {
		exclusionFieldValid = false;
		return;
	}
	auto* cs = GetExclusionFieldCS();
	if (!cs) {
		exclusionFieldValid = false;
		return;
	}

	auto context = globals::d3d::context;
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();

	// Snap the window to whole texels so a texel keeps covering the same patch
	// of world as the camera moves; an unsnapped window resamples the bowls
	// every frame and their noisy rims crawl.
	constexpr float texelSize = kExclusionFieldHalfExtent * 2.0f / kExclusionFieldDim;
	const float2 center = {
		std::floor(eye.x / texelSize) * texelSize,
		std::floor(eye.y / texelSize) * texelSize
	};
	// Everything the CS reads: cb0 (centre, window origin), cb1 (the list),
	// t0 (the terrain window). All unchanged = the texture already holds
	// the same bits.
	const bool same = exclusionFieldValid && exclusionBakeList &&
	                  center.x == exclusionFieldCenter.x && center.y == exclusionFieldCenter.y &&
	                  shellWindowCellX == exclusionBakeCellX && shellWindowCellY == exclusionBakeCellY &&
	                  shellTerrainVersion == exclusionBakeTerrainVersion &&
	                  memcmp(&a_list, exclusionBakeList.get(), sizeof(ExclusionsCB)) == 0;
	if (same) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
		return;
	}
	exclusionFieldValid = false;
	exclusionFieldCenter = center;

	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	ExclusionFieldCB cbData{};
	cbData.FieldCenter = exclusionFieldCenter;
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

	globals::profiler->BeginPass("SnowDeformation::Ground - Field Bakes");
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
	exclusionBakeCellX = shellWindowCellX;
	exclusionBakeCellY = shellWindowCellY;
	exclusionBakeTerrainVersion = shellTerrainVersion;
	if (!exclusionBakeList)
		exclusionBakeList = std::make_unique<ExclusionsCB>();
	memcpy(exclusionBakeList.get(), &a_list, sizeof(ExclusionsCB));
}

// Position size = distance to the first following attribute (the descriptor's
// offset table is authoritative). The VF_FULLPREC flag is NOT reliable: logged
// runtime buffers carry 16-byte float4 positions with the flag clear, and
// reading them as halfs shreds geometry into screen-wide streaks.
uint32_t SD_PositionBytes(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc)
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

ID3D11InputLayout* SnowDeformation::ContactSkinInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc, uint32_t a_dynamicPositionBytes)
{
	// Flag bits sit at 44..53; the dynamic variants key above them.
	const uint64_t cacheKey = a_descKey | (a_dynamicPositionBytes ? (uint64_t(a_dynamicPositionBytes) << 56) : 0);
	auto& layout = contactSkinILCache[cacheKey];
	if (!layout && contactSkinVSBlob) {
		// SSE skinning block: four float16 weights then four UNORM byte
		// indices, 12 bytes, at the descriptor's own skinning offset.
		const uint32_t skinOffset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
		const uint32_t positionBytes = a_dynamicPositionBytes ? a_dynamicPositionBytes : SD_PositionBytes(a_descKey, a_desc);
		const UINT positionSlot = a_dynamicPositionBytes ? 1 : 0;
		D3D11_INPUT_ELEMENT_DESC elements[3] = {
			{ "POSITION", 0, positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, positionSlot, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
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

ID3D11InputLayout* SnowDeformation::StaticsParallaxInputLayoutFor(uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc)
{
	using Vertex = RE::BSGraphics::Vertex;
	if (!staticsVSParallaxBlob || !a_desc.HasFlag(Vertex::VF_UV) || !a_desc.HasFlag(Vertex::VF_TANGENT))
		return nullptr;
	auto& layout = staticsILParallaxCache[a_descKey];
	if (!layout) {
		const uint32_t positionBytes = SD_PositionBytes(a_descKey, a_desc);
		// Lighting's own attributes: the tangent frame spans POSITION.w, NORMAL.w and BINORMAL.
		D3D11_INPUT_ELEMENT_DESC elements[4] = {
			{ "POSITION", 0, positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, a_desc.GetAttributeOffset(Vertex::VA_TEXCOORD0), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, a_desc.GetAttributeOffset(Vertex::VA_NORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "BINORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, a_desc.GetAttributeOffset(Vertex::VA_BINORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		globals::d3d::device->CreateInputLayout(elements, 4, staticsVSParallaxBlob->GetBufferPointer(), staticsVSParallaxBlob->GetBufferSize(), layout.put());
	}
	return layout.get();
}

bool SnowDeformation::EnsureCoatParallaxShaders()
{
	if (!coatParallaxTried) {
		coatParallaxTried = true;
		constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowStaticsShell.hlsl";
		const char* ehfDefine = globals::features::exponentialHeightFog.loaded ? "SNOW_EXP_HEIGHT_FOG" : nullptr;
		const char* iblDefine = globals::features::ibl.loaded ? "SNOW_IBL" : nullptr;
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SNOW_COAT_PARALLAX"));
		if (blob && SUCCEEDED(globals::d3d::device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsVSParallax))) {
			staticsVSParallaxBlob = blob;
			Util::SetResourceName(staticsVSParallax, "SnowDeformation::StaticsShellVS Parallax");
		}
		blob = nullptr;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DRAPE", "SNOW_COAT_PARALLAX"));
		if (blob && SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSDrapeParallax)))
			Util::SetResourceName(staticsPSDrapeParallax, "SnowDeformation::StaticsShellPS Drape Parallax");
		blob = nullptr;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", ehfDefine, iblDefine, "SNOW_STATICS_NO_DEPTH_EXPORT", "SNOW_STATICS_DRAPE", "SNOW_OBJECT_PRINTS", "SNOW_COAT_PARALLAX"));
		if (blob && SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &staticsPSDrapePrintsParallax)))
			Util::SetResourceName(staticsPSDrapePrintsParallax, "SnowDeformation::StaticsShellPS Drape Prints Parallax");
		logger::info("[SNOW DEFORMATION] Snow Follows Parallax coat shaders: VS {}, drape {}, prints {}",
			staticsVSParallax ? "ok" : "FAILED", staticsPSDrapeParallax ? "ok" : "FAILED", staticsPSDrapePrintsParallax ? "ok" : "FAILED");
	}
	return staticsVSParallax && staticsPSDrapeParallax;
}

// The engine's own list of live water bodies: every loaded cell's water and
// every placed body, culled or not, each with its shape. What the water
// SHADER draws is a superset - the LOD quadtree, a mod's skirts, the ripple
// displacement mesh and the waterline-crossing effect at the camera's own
// level - and capturing draws put every one of those into the raster.
void SnowDeformation::GatherWaterObjects()
{
	capturedWater.clear();
	auto* waterSystem = RE::TESWaterSystem::GetSingleton();
	if (!waterSystem || !waterSystem->enabled)
		return;
	for (const auto& object : waterSystem->waterObjects) {
		if (!object || !object->shape)
			continue;
		auto* shape = object->shape.get();
		if (shape->GetAppCulled())
			continue;
		bool seen = false;
		for (const auto& water : capturedWater)
			if (water.geometry.get() == shape) {
				seen = true;
				break;
			}
		if (seen)
			continue;
		capturedWater.push_back({ RE::NiPointer<RE::BSGeometry>(shape), shape->world, object->plane.constant });
	}
}

// The engine's water bodies top-down into the terrain window's frame (128-unit
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
		waterBakeValid = false;
		{
			D3D11_TEXTURE2D_DESC visDesc = desc;
			visDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			waterVisibleTexture = new Texture2D(visDesc, "SnowDeformation::WaterVisible");
			waterVisibleTexture->CreateSRV(srvDesc);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			waterVisibleTexture->CreateUAV(uavDesc);
			// Squared texels, 0-9 and 10 for none in reach: one byte.
			D3D11_TEXTURE2D_DESC nearDesc = visDesc;
			nearDesc.Format = DXGI_FORMAT_R8_UINT;
			waterNearTexture = new Texture2D(nearDesc, "SnowDeformation::WaterNear");
			waterNearTexture->CreateSRV({ .Format = nearDesc.Format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
			waterNearTexture->CreateUAV({ .Format = nearDesc.Format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
			waterCoverDirty = true;
		}
		desc.Width = kWaterFineDim;
		desc.Height = kWaterFineDim;
		waterFineTexture = new Texture2D(desc, "SnowDeformation::WaterFine");
		waterFineTexture->CreateSRV(srvDesc);
		waterFineTexture->CreateRTV(rtvDesc);
		waterFineValid = false;
	}
	GatherWaterObjects();
	statWaterCaptured = (uint32_t)capturedWater.size();
	// The draw list exactly as the loop below consumes it. Same list at the
	// same window cell = the texture already holds the same bits.
	waterBakeScratch.clear();
	waterBakeScratch.reserve(capturedWater.size());
	for (const auto& water : capturedWater) {
		WaterBakeKey key;
		memset(&key, 0, sizeof(key));
		auto* geometry = water.geometry.get();
		auto rendererData = geometry ? geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (rendererData) {
			key.vb = rendererData->vertexBuffer;
			key.ib = rendererData->indexBuffer;
			memcpy(&key.descKey, &rendererData->vertexDesc, sizeof(key.descKey));
		}
		if (auto triShape = geometry ? geometry->AsTriShape() : nullptr)
			key.indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
		key.world = water.world;
		key.planeConstant = water.planeConstant;
		waterBakeScratch.push_back(key);
	}
	const bool same = waterBakeValid && waterWindowCellX == shellWindowCellX && waterWindowCellY == shellWindowCellY &&
	                  waterBakeKeys.size() == waterBakeScratch.size() &&
	                  (waterBakeScratch.empty() || memcmp(waterBakeKeys.data(), waterBakeScratch.data(), waterBakeScratch.size() * sizeof(WaterBakeKey)) == 0);
	// The fine raster follows the object height window as well as the list.
	const float2 fineCenter = heightWindowCenter;
	const float fineHalf = ObjectRasterHalfExtent();
	const bool fineNeeded = waterFineTexture && (!same || !waterFineValid ||
											   fineCenter.x != waterFineCenter.x || fineCenter.y != waterFineCenter.y || fineHalf != waterFineHalf);
	if (same && !fineNeeded) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Ground - Field Bakes");
		capturedWater.clear();
		return;
	}
	if (!same) {
		waterBakeKeys.swap(waterBakeScratch);
		waterBakeValid = true;
		waterCoverDirty = true;
		waterWindowVersion++;
	}
	// The terrain window's frame, rebuilt whenever the list or the cell
	// moves (never kept across a change: a plane from an earlier frame - the
	// waterline effect at the camera's level, a plane seen once from a bad
	// angle - poisoned the area until the next cell crossing). A body's plane
	// is set up whenever the ground over it is in view, so nothing the
	// camera can see is lost.
	const float clear[4] = { kShellMissingHeight, 0.0f, 0.0f, 0.0f };
	if (!same) {
		context->ClearRenderTargetView(waterHeightTexture->rtv.get(), clear);
		waterWindowCellX = shellWindowCellX;
		waterWindowCellY = shellWindowCellY;
	}
	statWaterDrawn = 0;
	if (capturedWater.empty()) {
		waterFineValid = false;
		return;
	}

	globals::profiler->BeginPass("SnowDeformation::Ground - Field Bakes");
	// The fine raster is Lighting's water raster; off the PS before it becomes a target.
	ID3D11ShaderResourceView* nullWaterSRV = nullptr;
	context->PSSetShaderResources(kWaterWindowSlot, 1, &nullWaterSRV);
	context->OMSetBlendState(heightMaxBlendState.get(), nullptr, 0xFFFFFFFF);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(waterCaptureVS, nullptr, 0);
	context->PSSetShader(waterCapturePS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);

	// One loop, two frames: the terrain window (coarse) and the object
	// height window (fine).
	auto drawPlanes = [&](ID3D11RenderTargetView* a_rtv, uint32_t a_dim, float2 a_center, float a_halfExtent) {
	context->OMSetRenderTargets(1, &a_rtv, nullptr);
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(a_dim), float(a_dim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	StaticsCB rec{};
	rec.HeightWindowCenter = a_center;
	rec.HeightHalfExtent = a_halfExtent;
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
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		if (!waterFirstLogged) {
			waterFirstLogged = true;
			logger::info("[SNOW DEFORMATION] water capture: first plane '{}' desc {:#x} vertex {} tris {} at ({:.0f} {:.0f} {:.0f}) window origin cell ({}, {})",
				geometry->name.c_str(), descKey, desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX), triShape->GetTrishapeRuntimeData().triangleCount,
				water.world.translate.x, water.world.translate.y, water.world.translate.z, shellWindowCellX, shellWindowCellY);
		}
		if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
			continue;
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
		statWaterDrawn++;
	}
	};
	if (!same) {
		const float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
		const float span = kShellVertexSpacing * kShellWindowDim;
		drawPlanes(waterHeightTexture->rtv.get(), kShellWindowDim,
			{ shellWindowCellX * cellSize + span * 0.5f, shellWindowCellY * cellSize + span * 0.5f }, span * 0.5f);
	}
	if (fineNeeded) {
		context->ClearRenderTargetView(waterFineTexture->rtv.get(), clear);
		drawPlanes(waterFineTexture->rtv.get(), kWaterFineDim, fineCenter, fineHalf);
		waterFineCenter = fineCenter;
		waterFineHalf = fineHalf;
		waterFineValid = true;
	}
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	globals::profiler->EndPass();
	capturedWater.clear();
}

// When the water raster or the terrain window changed: the data route's
// water window is the raw raster minus its lone wet texels, and the near
// field is the texel distance to the nearest vertex under the water
// (WaterCoverCS). Without the pass the raw window is copied through and
// the near field reads "far".
void SnowDeformation::CoverWaterWindow()
{
	if (!waterHeightTexture || !waterVisibleTexture || !waterVisibleTexture->uav || !waterNearTexture || !waterNearTexture->uav)
		return;
	if (!waterCoverDirty && waterCoverTerrainVersion == shellTerrainVersion)
		return;
	waterCoverDirty = false;
	waterCoverTerrainVersion = shellTerrainVersion;
	waterWindowVersion++;
	auto context = globals::d3d::context;
	if (!heightWaterCoverCS || !shellTerrainTexture || !shellTerrainTexture->srv || !heightProcessCB) {
		context->CopyResource(waterVisibleTexture->resource.get(), waterHeightTexture->resource.get());
		const UINT farNear[4] = { 10, 10, 10, 10 };
		context->ClearUnorderedAccessViewUint(waterNearTexture->uav.get(), farNear);
		return;
	}
	globals::profiler->BeginPass("SnowDeformation::Ground - Field Bakes");
	ID3D11ShaderResourceView* srvs[3] = { waterHeightTexture->srv.get(), nullptr, shellTerrainTexture->srv.get() };
	ID3D11UnorderedAccessView* uavs[2] = { waterVisibleTexture->uav.get(), waterNearTexture->uav.get() };
	ID3D11Buffer* processCB = heightProcessCB->CB();
	context->CSSetConstantBuffers(0, 1, &processCB);
	context->CSSetShaderResources(0, 3, srvs);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(heightWaterCoverCS, nullptr, 0);
	context->Dispatch((kShellWindowDim + 7) / 8, (kShellWindowDim + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, 3, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
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
			.Format = DXGI_FORMAT_R32G32_FLOAT,
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
	// One MIN blend per channel: bodies write R, loose props G.
	auto makeMinBlend = [&](winrt::com_ptr<ID3D11BlendState>& a_state, UINT8 a_mask) {
		if (a_state)
			return true;
		D3D11_BLEND_DESC blendDesc{};
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_ONE;
		rt.BlendOp = D3D11_BLEND_OP_MIN;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_MIN;
		rt.RenderTargetWriteMask = a_mask;
		return SUCCEEDED(device->CreateBlendState(&blendDesc, a_state.put()));
	};
	if (!makeMinBlend(contactMinBlendBodies, D3D11_COLOR_WRITE_ENABLE_RED) || !makeMinBlend(contactMinBlendProps, D3D11_COLOR_WRITE_ENABLE_GREEN)) {
		contactShadersFailed = true;
		return false;
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

	const float clearValue[4] = { kContactNone, kContactNone, 0.0f, 0.0f };
	context->ClearRenderTargetView(contactHeight->rtv.get(), clearValue);
	ID3D11RenderTargetView* rtv = contactHeight->rtv.get();
	context->OMSetRenderTargets(1, &rtv, nullptr);
	context->OMSetBlendState(contactMinBlendProps.get(), nullptr, 0xFFFFFFFF);
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
			const bool fresh = state.frame + 1 != contactSweepFrame || state.buffer != rendererData->vertexBuffer;
			previous = state.world;
			state.world = world;
			state.frame = contactSweepFrame;
			state.buffer = rendererData->vertexBuffer;
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

	globals::profiler->BeginPass("SnowDeformation::Trails - Gather & Contact");
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
	contactHiddenPartsLast = 0;
	contactGlobalPartsLast = 0;
	if (contactSkinIndexing.size() > 1024)
		contactSkinIndexing.clear();
	contactSweepLast = 0;
	contactSweepFrame++;
	if (contactSweepStates.size() > 512)
		contactSweepStates.clear();
	if (!contactActors.empty() && contactSkinVS && contactSkinCB) {
		globals::profiler->BeginPass("SnowDeformation::Trails - Gather & Contact");
		context->OMSetBlendState(contactMinBlendBodies.get(), nullptr, 0xFFFFFFFF);
		context->VSSetShader(contactSkinVS, nullptr, 0);
		ID3D11Buffer* skinCB = contactSkinCB->CB();
		context->VSSetConstantBuffers(2, 1, &skinCB);
		// Tracks the bound VS across actors: a rigid part leaves contactVS bound for the next actor.
		bool skinnedVSBound = true;
		const auto* playerCamera = RE::PlayerCamera::GetSingleton();
		const bool firstPerson = playerCamera && playerCamera->IsInFirstPerson();
		for (const auto& actor : contactActors) {
			auto ref = actor.ref.get();
			auto* root = ref ? ref->Get3D(false) : nullptr;
			if (!root)
				continue;
			const uint drawsBeforeActor = contactSkinDrawsLast;
			ScopedTicks _actorTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactActor], cpuCensus.spanCalls[CpuCensus::kSpanContactActor]);
			// First person hides the player's third-person root, but that skeleton
			// still animates: the body under the camera is what walks in the snow.
			const bool firstPersonPlayer = firstPerson && ref->IsPlayerRef();
			struct
			{
				uint hidden = 0, unlit = 0, proxy = 0, above = 0, skinned = 0, dynamic = 0, dynamicUnread = 0;
			} tally;
			int geometryIndex = -1;
			auto visitGeometry = [&](RE::BSGeometry* a_geometry) -> RE::BSVisit::BSVisitControl {
				ScopedTicks _geometryTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactGeometry], cpuCensus.spanCalls[CpuCensus::kSpanContactGeometry]);
				auto& runtime = a_geometry->GetGeometryRuntimeData();
				auto* skin = runtime.skinInstance.get();
				// A part whose whole bound floats above the layer carves nothing,
				// and the draws are the cost: on a standing actor that leaves the
				// boots and calves, and a sheathed sword at the hip stays out until
				// a swing brings it down. Corpses lie low and keep everything. Every
				// test here only rejects, so this cheapest one goes first.
				{
					const auto& gb = a_geometry->worldBound;
					if (gb.radius > 0.0f && gb.center.z - gb.radius > actor.groundZ + actor.layer + kContactSkipMargin) {
						tally.above++;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
				}
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
						hidden = n->GetAppCulled() && !(firstPersonPlayer && n == root);
					// Effect art - glows, light rays, runes, blood decals, an ENB light's
					// 500-unit billboard - is drawn with an effect shader, never a
					// lighting one. It is not a surface and must not carve snow.
					auto* property = runtime.shaderProperty.get();
					const bool lit = property && property->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get();
					// A refraction mesh is a distortion volume drawn only into the
					// refraction mask: a master spell's ritual haze is 183 units across.
					const bool refraction = lit && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction);
					if (hidden || !lit || refraction) {
						if (contactSkinHiddenLogged.size() < 32 && contactSkinHiddenLogged.insert(a_geometry).second)
							logger::info("[SNOW DEFORMATION] contact geometry '{}' on '{}' is {} (bound radius {:.0f}); not drawn",
								a_geometry->name.c_str() ? a_geometry->name.c_str() : "", ref->GetDisplayFullName(),
								hidden ? "hidden by the game" : (refraction ? "a refraction volume, not a surface" : (property ? "effect art, not a surface" : "without a shader")),
								a_geometry->worldBound.radius);
						++(hidden ? tally.hidden : tally.unlit);
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					// Physics proxies and overlay layers draw no surface of their own.
					if (IsProxyOrOverlayGeometry(a_geometry, property)) {
						tally.proxy++;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
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
				tally.skinned++;
				ScopedTicks _skinnedTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactSkinned], cpuCensus.spanCalls[CpuCensus::kSpanContactSkinned]);
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
				// A BSDynamicTriShape (RaceMenu BodyMorph / OBody bodies, facegen
				// heads) keeps its positions CPU-side in dynamicData and the game
				// streams them per draw; its static buffer has none at offset 0.
				// Drawn from the static buffer alone the body lands nowhere, the
				// draw still counts, and the actor carves nothing. Positions go
				// up as a second stream; a size that does not read as one
				// position per vertex is logged and the geometry left undrawn, so
				// the bone fallback below takes it.
				uint32_t dynamicPositionBytes = 0;
				if (auto* dynamicShape = a_geometry->AsDynamicTriShape()) {
					tally.dynamic++;
					auto& dd = dynamicShape->GetDynamicTrishapeRuntimeData();
					const uint32_t vertexCount = dynamicShape->GetTrishapeRuntimeData().vertexCount;
					const uint32_t perVertex = vertexCount ? dd.dataSize / vertexCount : 0;
					const bool readable = dd.dynamicData && vertexCount > 0 && dd.dataSize == vertexCount * perVertex && (perVertex == 16 || perVertex == 8);
					if (contactDynamicLogged.size() < 16 && contactDynamicLogged.insert(a_geometry).second) {
						std::string firstPart = "no partition buffer";
						if (skinPartition->numPartitions > 0)
							if (auto* buff = skinPartition->partitions[0].buffData) {
								uint64_t key;
								memcpy(&key, &buff->vertexDesc, sizeof(key));
								uint32_t vbBytes = 0;
								if (auto* vb = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer)) {
									D3D11_BUFFER_DESC bd{};
									vb->GetDesc(&bd);
									vbBytes = bd.ByteWidth;
								}
								firstPart = std::format("desc {:016X} size nibble {} dynamic nibble {} skin offset {} flags{}{} | static VB {} bytes = {:.1f}/vertex, raw copy {}",
									key, uint32_t(key & 0xF) * 4, uint32_t((key >> 4) & 0xF) * 4,
									buff->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING),
									buff->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) ? " VERTEX" : "",
									buff->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC) ? " FULLPREC" : "",
									vbBytes, vertexCount ? float(vbBytes) / vertexCount : 0.0f, buff->rawVertexData ? "yes" : "no");
							}
						logger::info("[SNOW DEFORMATION] dynamic tri shape '{}' on '{}': {} vertices, dynamicData {} bytes = {}/vertex -> {} | {}",
							a_geometry->name.c_str() ? a_geometry->name.c_str() : "", ref->GetDisplayFullName(),
							vertexCount, dd.dataSize, perVertex, readable ? "drawn from dynamic positions" : "NOT drawn (size does not read as positions)", firstPart);
					}
					if (!readable) {
						tally.dynamicUnread++;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					if (contactDynamicVBBytes < dd.dataSize) {
						contactDynamicVB = nullptr;
						D3D11_BUFFER_DESC vbDesc{};
						vbDesc.ByteWidth = std::max(dd.dataSize, 64u * 1024u);
						vbDesc.Usage = D3D11_USAGE_DYNAMIC;
						vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
						vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
						if (FAILED(globals::d3d::device->CreateBuffer(&vbDesc, nullptr, contactDynamicVB.put()))) {
							contactDynamicVBBytes = 0;
							tally.dynamicUnread++;
							return RE::BSVisit::BSVisitControl::kContinue;
						}
						Util::SetResourceName(contactDynamicVB.get(), "SnowDeformation::ContactDynamicPositions");
						contactDynamicVBBytes = vbDesc.ByteWidth;
					}
					ScopedTicks _dynamicTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactDynamic], cpuCensus.spanCalls[CpuCensus::kSpanContactDynamic]);
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(contactDynamicVB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
						tally.dynamicUnread++;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					{
						RE::BSSpinLockGuard guard(dd.lock);
						std::memcpy(mapped.pData, dd.dynamicData, dd.dataSize);
					}
					context->Unmap(contactDynamicVB.get(), 0);
					dynamicPositionBytes = perVertex;
				}
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
				if (auto it = contactSkinIndexing.find(skin); it != contactSkinIndexing.end() && it->second.partition == skinPartition) {
					indexing = it->second.indexing;
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
					contactSkinIndexing[skin] = { skinPartition, indexing };
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
					if ((!dynamicPositionBytes && !partDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) ||
						!partDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
						continue;
					uint64_t descKey;
					memcpy(&descKey, &partDesc, sizeof(descKey));
					auto* layout = ContactSkinInputLayoutFor(descKey, partDesc, dynamicPositionBytes);
					if (!layout)
						continue;
					const UINT stride = uint32_t(descKey & 0xF) * 4;
					if (stride == 0)
						continue;

					if (indexing == 2 && boneCount <= kContactMaxBones) {
						if (!globalUploaded) {
							ScopedTicks _uploadTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactUpload], cpuCensus.spanCalls[CpuCensus::kSpanContactUpload]);
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
						ScopedTicks _uploadTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactUpload], cpuCensus.spanCalls[CpuCensus::kSpanContactUpload]);
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

					ScopedTicks _drawTicks(cpuCensus.spanTicks[CpuCensus::kSpanContactDraw], cpuCensus.spanCalls[CpuCensus::kSpanContactDraw]);
					UINT offset = 0;
					auto* vb = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer);
					auto* ib = reinterpret_cast<ID3D11Buffer*>(buff->indexBuffer);
					context->IASetInputLayout(layout);
					context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
					if (dynamicPositionBytes) {
						auto* dynamicVB = contactDynamicVB.get();
						context->IASetVertexBuffers(1, 1, &dynamicVB, &dynamicPositionBytes, &offset);
					}
					context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
					context->DrawIndexed(indexCount, thisStart, 0);
					contactSkinDrawsLast++;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			};
			// By reference: the walk copies its std::function at every node, and
			// this closure is past the small buffer, so each copy was a heap one.
			RE::BSVisit::TraverseScenegraphGeometries(root, std::ref(visitGeometry));
			// Nothing printed from a body the game draws: hand the actor to the bone
			// stamps for a while rather than leave it carving nothing at all.
			const bool rootHidden = root->GetAppCulled() && !firstPersonPlayer;
			if (!actor.corpse && contactSkinDrawsLast == drawsBeforeActor && !rootHidden && debugContactSolo < 0) {
				if (auto it = stampBoneCache.find(ref->GetFormID()); it != stampBoneCache.end()) {
					auto& cache = it->second;
					cache.contactStarved = kContactStarveFrames;
					if (!cache.contactStarveLogged && contactStarveLines < 32) {
						cache.contactStarveLogged = true;
						contactStarveLines++;
						logger::info("[SNOW DEFORMATION] contact draw printed nothing for '{}' ({:08X}){}; stamping by bones. skinned drawn-eligible {}, hidden {}, unshaded/effect {}, proxy {}, above the layer {}, dynamic {} (unreadable {})",
							ref->GetDisplayFullName(), ref->GetFormID(), firstPersonPlayer ? " in first person" : "",
							tally.skinned, tally.hidden, tally.unlit, tally.proxy, tally.above, tally.dynamic, tally.dynamicUnread);
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

ID3D11ComputeShader* SnowDeformation::GetHiZBuildCS(bool a_fromUAV)
{
	auto*& slot = a_fromUAV ? hiZBuildUAVCS : hiZBuildCS;
	if (!slot) {
		logger::debug("Compiling DepthSyncCS HiZBuildCS{}", a_fromUAV ? " (from UAV)" : "");
		std::vector<std::pair<const char*, const char*>> defines;
		if (a_fromUAV)
			defines.push_back({ "HIZ_FROM_UAV", "" });
		slot = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", defines, "cs_5_0", "HiZBuildCS"));
	}
	return slot;
}

ID3D11ComputeShader* SnowDeformation::GetSkinCullCS()
{
	if (!skinCullCS) {
		logger::debug("Compiling DepthSyncCS SkinCullCS");
		skinCullCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "SkinCullCS"));
	}
	return skinCullCS;
}

bool SnowDeformation::EnsureSkinCullResources(uint32_t a_count, ID3D11ShaderResourceView* a_mainDepthSRV, uint32_t a_extentW, uint32_t a_extentH)
{
	auto device = globals::d3d::device;
	if (!skinCullCB)
		skinCullCB = new ConstantBuffer(ConstantBufferDesc<SkinCullCB>(), "SnowDeformation::SkinCullCB");

	// Max-depth pyramid over the scene depth's first a_extent pixels: level 0
	// is half res, the chain runs to 1x1 so a footprint of any size finds its
	// level. An odd extent short of the texture rounds up (the pixel past it
	// is masked to 0); at the texture's edge the build's odd-tail fold covers
	// the last pixel, as for the whole texture.
	winrt::com_ptr<ID3D11Resource> depthRes;
	a_mainDepthSRV->GetResource(depthRes.put());
	auto depthTex = depthRes.try_as<ID3D11Texture2D>();
	if (!depthTex)
		return false;
	D3D11_TEXTURE2D_DESC depthDesc{};
	depthTex->GetDesc(&depthDesc);
	auto halfExtent = [](uint32_t a_extent, uint32_t a_size) {
		const uint32_t extent = std::clamp(a_extent, 1u, a_size);
		return std::max(1u, extent / 2 + ((extent & 1) && extent < a_size ? 1u : 0u));
	};
	const uint32_t w0 = halfExtent(a_extentW, depthDesc.Width);
	const uint32_t h0 = halfExtent(a_extentH, depthDesc.Height);
	if (skinCullHiZ && (skinCullHiZ->desc.Width != w0 || skinCullHiZ->desc.Height != h0)) {
		delete skinCullHiZ;
		skinCullHiZ = nullptr;
		for (auto* scratch : skinCullHiZScratch)
			delete scratch;
		skinCullHiZScratch.clear();
		skinCullHiZSRVs.clear();
		skinCullHiZUAVs.clear();
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
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
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
			D3D11_UNORDERED_ACCESS_VIEW_DESC levelUavDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = level }
			};
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
			if (FAILED(device->CreateUnorderedAccessView(skinCullHiZ->resource.get(), &levelUavDesc, uav.put())))
				return false;
			Util::SetResourceName(uav.get(), "SnowDeformation::SkinCullHiZ level UAV");
			skinCullHiZUAVs.push_back(uav);
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

	if (!skinFootprint) {
		D3D11_BUFFER_DESC footDesc{};
		footDesc.Usage = D3D11_USAGE_DEFAULT;
		footDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		footDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		footDesc.ByteWidth = sizeof(uint32_t) * 4;
		D3D11_SHADER_RESOURCE_VIEW_DESC footSRV{};
		footSRV.Format = DXGI_FORMAT_R32_TYPELESS;
		footSRV.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		footSRV.BufferEx.NumElements = 4;
		footSRV.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		D3D11_UNORDERED_ACCESS_VIEW_DESC footUAV{};
		footUAV.Format = DXGI_FORMAT_R32_TYPELESS;
		footUAV.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		footUAV.Buffer.NumElements = 4;
		footUAV.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
		skinFootprint = new Buffer(footDesc, nullptr, "SnowDeformation::SkinFootprint");
		skinFootprint->CreateSRV(footSRV);
		skinFootprint->CreateUAV(footUAV);
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
	return skinCullHiZ->srv && skinCullHiZScratch.size() == skinCullLevels && skinCullBounds && skinCullBounds->srv && skinCullArgs && skinCullArgs->uav &&
	       skinFootprint->srv && skinFootprint->uav;
}

// Mirrors RE::TES::ForEachReferenceInRange: the interior cell alone, or the
// loaded grid (x outer, y inner, attached cells whose square meets the
// radius) and then the sky cell. Slot i is one of those cells.
void SnowDeformation::StartExclusionGather()
{
	auto* player = globals::game::player;
	auto* tes = globals::game::tes;
	if (!player || !tes) {
		exclusionGatherSlot = 0;
		return;
	}
	if (!survivalHeatSourcesResolved) {
		survivalHeatSourcesResolved = true;
		if (auto* dataHandler = RE::TESDataHandler::GetSingleton())
			survivalHeatSources = dataHandler->LookupForm<RE::BGSListForm>(0x0008AA, "ccQDRSSE001-SurvivalMode.esl");
	}
	exclusionPending.clear();
	exclusionPendingFlames.clear();
	exclusionPendingGuards.clear();
	exclusionPendingHeat.clear();
	exclusionPendingTrample = 0;
	exclusionPendingSealed = 0;
	exclusionGatherOrigin = player->GetPosition();
	exclusionGatherInterior = tes->interiorCell != nullptr;
	const uint32_t gridLength = tes->gridCells ? tes->gridCells->length : 0;
	exclusionGatherSlots = exclusionGatherInterior ? 1 : gridLength * gridLength + 1;
	exclusionGatherSlot = 1;
}

void SnowDeformation::StepExclusionGather()
{
	if (exclusionGatherSlot == 0)
		return;
	auto* tes = globals::game::tes;
	if (!tes || (tes->interiorCell != nullptr) != exclusionGatherInterior) {
		exclusionGatherSlot = 0;
		return;
	}
	// Reaches the WIDE field's window, not just the near mask's: a campfire
	// only shows at 200 m if it was gathered at 200 m.
	const float radius = kExclusionFieldHalfExtent + 512.0f;
	const auto& origin = exclusionGatherOrigin;
	auto visit = [this](RE::TESObjectREFR* a_ref) { return ExclusionVisit(a_ref); };
	// The walk spreads over ~50 frames.
	const uint32_t perStep = std::max(1u, (exclusionGatherSlots + 49) / 50);
	const uint32_t gridLength = tes->gridCells ? tes->gridCells->length : 0;
	for (uint32_t n = 0; n < perStep && exclusionGatherSlot <= exclusionGatherSlots; n++, exclusionGatherSlot++) {
		const uint32_t i = exclusionGatherSlot - 1;
		if (exclusionGatherInterior) {
			if (tes->interiorCell)
				tes->interiorCell->ForEachReferenceInRange(origin, radius, visit);
		} else if (i < gridLength * gridLength) {
			auto* cell = tes->gridCells->GetCell(i / gridLength, i % gridLength);
			if (cell && cell->IsAttached()) {
				if (const auto* coords = cell->GetCoordinates()) {
					if (coords->worldX < origin.x + radius && coords->worldX + 4096.0f > origin.x - radius &&
						coords->worldY < origin.y + radius && coords->worldY + 4096.0f > origin.y - radius)
						cell->ForEachReferenceInRange(origin, radius, visit);
				}
			}
		} else if (auto* worldSpace = tes->GetRuntimeData2().worldSpace) {
			if (auto* skyCell = worldSpace->GetSkyCell())
				skyCell->ForEachReferenceInRange(origin, radius, visit);
		}
	}
	if (exclusionGatherSlot > exclusionGatherSlots) {
		FinishExclusionGather();
		exclusionGatherSlot = 0;
	}
}

RE::BSContainer::ForEachResult SnowDeformation::ExclusionVisit(RE::TESObjectREFR* a_ref)
{
	auto* tes = globals::game::tes;
	// Collect past the CB cap: the nearest-first sort below
	// picks the winners, so a far sconce can never evict a
	// near door. 4x cap bounds the pathological case.
	if (exclusionPending.size() >= kMaxExclusions * 4)
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
			exclusionPending.push_back({ { pos.x, pos.y, pos.z, kTorchClearRadius }, { 0.0f, 0.0f, 1.0f, 1.0f } });
			exclusionPendingHeat.push_back({ pos.x, pos.y, pos.z, kTorchClearRadius });
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
			exclusionPendingFlames.push_back(exclusionPending.size());
		exclusionPending.push_back({ { pos.x, pos.y, pos.z, heatRadius }, { 0.0f, 0.0f, 1.0f, 1.0f } });
		exclusionPendingHeat.push_back({ pos.x, pos.y, pos.z, heatRadius });
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
				exclusionPending.push_back({ { pos.x + std::sin(angleZ) * spec.forwardBias * scale,
												 pos.y + std::cos(angleZ) * spec.forwardBias * scale,
												 pos.z, spec.radius * scale * zoneScale },
					{ std::sin(angleZ) * elongation, std::cos(angleZ) * elongation, meltStrength,
						spec.smoothEdge ? 2.0f : 1.0f } });
				exclusionPendingTrample++;
				// Physical footprint, NOT slider-scaled: the
				// flame sits inside the structure no matter
				// how small the clearing is tuned.
				if (spec.ownsFlames)
					exclusionPendingGuards.push_back({ pos.x, pos.y, pos.z, spec.radius * scale * 0.5f });
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
				exclusionPending.push_back({ { pos.x + localX * cosZ + localY * sinZ,
												 pos.y - localX * sinZ + localY * cosZ,
												 pos.z, halfX },
					{ sinZ, cosZ, halfY, 3.0f } });
				exclusionPendingSealed++;
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
		exclusionPending.push_back({ { pos.x, pos.y, pos.z, radius * a_ref->GetScale() }, { 0.0f, 0.0f, 1.0f, 1.0f } });
		exclusionPendingHeat.push_back({ pos.x, pos.y, pos.z, radius * a_ref->GetScale() });
	}
	return RE::BSContainer::ForEachResult::kContinue;
}

void SnowDeformation::FinishExclusionGather()
{
	// Drop flame melt spots that sit inside a flame-owning station
	// (descending index order keeps the remaining indices valid).
	if (!exclusionPendingGuards.empty() && !exclusionPendingFlames.empty()) {
		for (auto it = exclusionPendingFlames.rbegin(); it != exclusionPendingFlames.rend(); ++it) {
			const auto& flame = exclusionPending[*it].first;
			for (const auto& guard : exclusionPendingGuards) {
				const float dx = flame.x - guard.x, dy = flame.y - guard.y;
				if (dx * dx + dy * dy < guard.w * guard.w && std::abs(flame.z - guard.z) < 300.0f) {
					exclusionPending.erase(exclusionPending.begin() + *it);
					break;
				}
			}
		}
	}

	statTrampleCount = exclusionPendingTrample;
	statSealedCount = exclusionPendingSealed;

	// Overflow: keep the sources nearest the walk's origin.
	if (exclusionPending.size() > kMaxExclusions) {
		const auto playerPos = exclusionGatherOrigin;
		std::partial_sort(exclusionPending.begin(), exclusionPending.begin() + kMaxExclusions, exclusionPending.end(),
			[&](const auto& a_lhs, const auto& a_rhs) {
				const float lhsDx = a_lhs.first.x - playerPos.x, lhsDy = a_lhs.first.y - playerPos.y;
				const float rhsDx = a_rhs.first.x - playerPos.x, rhsDy = a_rhs.first.y - playerPos.y;
				return lhsDx * lhsDx + lhsDy * lhsDy < rhsDx * rhsDx + rhsDy * rhsDy;
			});
		exclusionPending.resize(kMaxExclusions);
	}
	staticExclusions.swap(exclusionPending);
	exclusionPending.clear();
	exclusionPendingFlames.clear();
	exclusionPendingGuards.clear();
	heatSources.swap(exclusionPendingHeat);
	exclusionPendingHeat.clear();
}
