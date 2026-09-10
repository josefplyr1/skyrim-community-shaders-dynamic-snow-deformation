// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include <DDSTextureLoader.h>

#include "Features/TerrainShadows.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

/** @brief Lowercased diffuse path of a land texture, empty when it has none. */
static std::string LandTexturePath(RE::TESLandTexture* a_landTexture)
{
	if (auto textureSet = a_landTexture->textureSet) {
		if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
			std::string lowered(path);
			std::transform(lowered.begin(), lowered.end(), lowered.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			return lowered;
		}
	}
	return {};
}

/** @brief Classifies a land texture into a kSnowClasses index by diffuse filename substring (first match wins), falling back on the snow material check. Only supplies the DEFAULT depth now; per-texture overrides win. */
static int ClassifySnowClass(RE::TESLandTexture* a_landTexture, const std::string& a_path)
{
	for (uint32_t classI = 0; classI < SnowDeformation::kSnowClassCount; ++classI) {
		const char* match = SnowDeformation::kSnowClasses[classI].match;
		if (match[0] != '\0' && a_path.find(match) != std::string::npos)
			return (int)classI;
	}

	bool snowMaterial = a_landTexture->materialType &&
	                    (a_landTexture->materialType->materialID == RE::MATERIAL_ID::kSnow ||
							a_landTexture->materialType->materialID == RE::MATERIAL_ID::kSnowStairs);
	return snowMaterial ? 3 /* Snow 01 */ : (int)SnowDeformation::kSnowClassCount - 1 /* Other */;
}

/** @brief Display label for the texture list: filename without directory or extension. */
static std::string LandTextureLabel(const std::string& a_path)
{
	size_t begin = a_path.find_last_of("\\/");
	begin = begin == std::string::npos ? 0 : begin + 1;
	size_t end = a_path.find_last_of('.');
	if (end == std::string::npos || end < begin)
		end = a_path.size();
	return a_path.substr(begin, end - begin);
}

uint16_t SnowDeformation::RegisterLandTexture(RE::TESLandTexture* a_landTexture)
{
	if (!a_landTexture || a_landTexture->formID == 0)
		return kNoLandTexture;

	{
		const std::shared_lock lock(landTextureMutex);
		if (auto it = landTextureByForm.find(a_landTexture->formID); it != landTextureByForm.end())
			return it->second;
	}

	// Two LTEX forms can share one diffuse (vanilla does it), and the settings
	// key is the path, so both forms must land on the same registry entry.
	const std::string path = LandTexturePath(a_landTexture);
	if (path.empty())
		return kNoLandTexture;

	const std::unique_lock lock(landTextureMutex);
	if (auto it = landTextureByForm.find(a_landTexture->formID); it != landTextureByForm.end())
		return it->second;

	for (size_t i = 0; i < landTextures.size(); ++i) {
		if (landTextures[i].path == path) {
			landTextureByForm[a_landTexture->formID] = (uint16_t)i;
			return (uint16_t)i;
		}
	}

	if (landTextures.size() >= kMaxLandTextures) {
		static bool warned = false;
		if (!std::exchange(warned, true))
			logger::warn("[SNOW DEFORMATION] Land texture registry full at {}; further textures read as bare ground", kMaxLandTextures);
		return kNoLandTexture;
	}

	LandTextureEntry entry;
	entry.path = path;
	entry.label = LandTextureLabel(path);
	entry.classIndex = ClassifySnowClass(a_landTexture, path);
	for (const auto& shipped : kTextureDefaults) {
		if (path.find(shipped.match) != std::string::npos) {
			entry.shipped = true;
			entry.shippedDepth = shipped.depth;
			break;
		}
	}
	ResolveLandTextureDepthLocked(entry);

	const uint16_t index = (uint16_t)landTextures.size();
	landTextures.push_back(entry);
	landTextureByForm[a_landTexture->formID] = index;

	logger::info("[SNOW DEFORMATION] LTEX {:08X} [{}] {:.0f} units{} {}", a_landTexture->formID,
		kSnowClasses[entry.classIndex].label, entry.depth, entry.overridden ? " (override)" : "", path);
	return index;
}

void SnowDeformation::ResolveLandTextureDepthLocked(LandTextureEntry& a_entry)
{
	// User value, then a shipped per-texture default, then the family slider.
	auto it = settings.TextureDepths.find(a_entry.path);
	a_entry.overridden = it != settings.TextureDepths.end();
	a_entry.depth = a_entry.overridden ? it->second :
	                a_entry.shipped    ? a_entry.shippedDepth :
	                                     settings.SnowClassDepths[a_entry.classIndex];
}

void SnowDeformation::ResolveLandTextureDepthsLocked()
{
	for (auto& entry : landTextures)
		ResolveLandTextureDepthLocked(entry);
}

void SnowDeformation::RefreshLandTextureDepths()
{
	const std::unique_lock lock(landTextureMutex);
	ResolveLandTextureDepthsLocked();
}

void SnowDeformation::SetLandTextureOverride(const std::string& a_path, std::optional<float> a_depth)
{
	const std::unique_lock lock(landTextureMutex);
	if (a_depth)
		settings.TextureDepths[a_path] = *a_depth;
	else
		settings.TextureDepths.erase(a_path);
	ResolveLandTextureDepthsLocked();
}

std::vector<float> SnowDeformation::LandTextureDepthSnapshot()
{
	const std::shared_lock lock(landTextureMutex);
	std::vector<float> depths(landTextures.size());
	for (size_t i = 0; i < landTextures.size(); ++i)
		depths[i] = landTextures[i].depth;
	return depths;
}

namespace
{
	// A texture counts as snow for the terrain-shader mask when it currently
	// carries a raised layer. The mask is cached per material and the game
	// re-runs SetupMaterial constantly, so a slider that crosses zero corrects
	// itself within seconds; no re-bake is needed.
	bool IsSnowDepth(float a_depth) { return a_depth > 0.0f; }
}

void SnowDeformation::TESObjectLAND_SetupMaterial(RE::TESObjectLAND* land)
{
	if (land == nullptr || land->loadedData == nullptr || land->loadedData->mesh[0] == nullptr)
		return;

	for (uint32_t quadI = 0; quadI < 4; ++quadI) {
		if (land->loadedData->mesh[quadI] == nullptr)
			continue;

		// Runs after TruePBR's detour, so this is the final material used for
		// drawing (TruePBR allocates a new property + material per quad).
		const auto& children = land->loadedData->mesh[quadI]->GetChildren();
		auto geometry = children.empty() ? nullptr : static_cast<RE::BSGeometry*>(children[0].get());
		if (geometry == nullptr)
			continue;

		const auto shaderProp = static_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
		if (shaderProp == nullptr || shaderProp->material == nullptr)
			continue;

		// Bit 0 = base texture, bits 1-5 = the quad's layer textures.
		uint16_t quadTextures[6] = { RegisterLandTexture(land->loadedData->defQuadTextures[quadI]) };
		for (uint32_t textureI = 0; textureI < 5; ++textureI)
			quadTextures[textureI + 1] = RegisterLandTexture(land->loadedData->quadTextures[quadI][textureI]);

		uint8_t mask = 0;
		{
			const std::shared_lock textureLock(landTextureMutex);
			for (uint32_t textureI = 0; textureI < 6; ++textureI) {
				const uint16_t index = quadTextures[textureI];
				if (index != kNoLandTexture && IsSnowDepth(landTextures[index].depth))
					mask |= uint8_t(1 << textureI);
			}
		}

		const std::unique_lock lock(snowMaskMutex);
		// Materials are freed on cell unload; bound the map against stale pointers.
		if (snowMasks.size() > 16384)
			snowMasks.clear();
		snowMasks[reinterpret_cast<uintptr_t>(shaderProp->material)] = mask;
	}

	BakeShellCell(land);
}

void SnowDeformation::BakeShellCell(RE::TESObjectLAND* land)
{
	LoadTraceScope _loadTrace(this, "TerrainData: BakeShellCell");
	auto cell = land->GetSaveParentCell();
	if (!cell)
		return;
	auto coords = cell->GetCoordinates();
	if (!coords)
		return;

	ShellCellData data{};
	data.height.fill(kShellMissingHeight);
	for (auto& vertexTextures : data.layerTexture)
		vertexTextures.fill(kNoLandTexture);
	for (auto& vertexWeights : data.layerWeight)
		vertexWeights.fill(0);
	data.vertexAO.fill(255);
	if (auto* worldspace = cell->GetRuntimeData().worldSpace)
		data.worldspaceID = worldspace->GetFormID();

	auto loadedData = land->loadedData;

	// heights[] are relative to the cell's mid-height; the absolute base is
	// the midpoint of heightExtents. World transforms are not composed yet
	// when this hook runs and cannot be used for placement.
	float cellBaseZ = (loadedData->heightExtents.x + loadedData->heightExtents.y) * 0.5f;

	// Filler-plane detection, resolved after the vertex loop.
	bool painted = false;
	float minHeight = FLT_MAX;
	float maxHeight = -FLT_MAX;

	for (uint32_t quadI = 0; quadI < 4; ++quadI) {
		// Index-based quad layout: 0=SW, 1=SE, 2=NW, 3=NE, vertices x-fastest.
		uint32_t quadX = quadI & 1;
		uint32_t quadY = quadI >> 1;

		uint16_t baseTexture = RegisterLandTexture(loadedData->defQuadTextures[quadI]);
		uint16_t layerTexture[6];
		for (uint32_t layerI = 0; layerI < 6; ++layerI)
			layerTexture[layerI] = RegisterLandTexture(loadedData->quadTextures[quadI][layerI]);

		for (uint32_t vertexI = 0; vertexI < 289; ++vertexI) {
			uint32_t vx = vertexI % 17;
			uint32_t vy = vertexI / 17;
			uint32_t cellX = quadX * 16 + vx;
			uint32_t cellY = quadY * 16 + vy;
			uint32_t cellIdx = cellY * 33 + cellX;

			data.height[cellIdx] = loadedData->heights[quadI][vertexI] + cellBaseZ;
			minHeight = std::min(minHeight, data.height[cellIdx]);
			maxHeight = std::max(maxHeight, data.height[cellIdx]);

			// Same 0-255-in-int8 storage trick as percents below; ground's
			// vertexAO is the max component of the land vertex color.
			const auto& vertexColor = loadedData->colors[quadI][vertexI];
			data.vertexAO[cellIdx] = std::max({ static_cast<uint8_t>(vertexColor[0]),
				static_cast<uint8_t>(vertexColor[1]), static_cast<uint8_t>(vertexColor[2]) });

			// Gather this vertex's weight per distinct texture: at most the
			// base plus 6 layers, and layers of the same texture merge. An
			// absent layer texture still spends its weight, so unpainted
			// ground keeps its coverage gap and the shell submerges there.
			uint16_t vertexTexture[7];
			float vertexWeight[7];
			uint32_t textureCount = 0;
			auto addWeight = [&](uint16_t texture, float weight) {
				if (texture == kNoLandTexture || weight <= 0.0f)
					return;
				for (uint32_t i = 0; i < textureCount; ++i) {
					if (vertexTexture[i] == texture) {
						vertexWeight[i] += weight;
						return;
					}
				}
				vertexTexture[textureCount] = texture;
				vertexWeight[textureCount] = weight;
				textureCount++;
			};

			float layerSum = 0.0f;
			for (uint32_t layerI = 0; layerI < 6; ++layerI) {
				// percents is declared std::int8_t but holds 0-255: a fully
				// painted layer reads as -1 without the unsigned cast.
				float weight = static_cast<uint8_t>(loadedData->percents[quadI][vertexI][layerI]) / 255.0f;
				layerSum += weight;
				addWeight(layerTexture[layerI], weight);
			}
			painted = painted || layerSum > 0.0f;
			addWeight(baseTexture, std::max(0.0f, 1.0f - layerSum));

			// Keep the heaviest kShellVertexLayers: a vertex painted with more
			// textures than that has the rest below noise, and the slot count
			// is what holds the per-cell footprint at the old 12-class size.
			for (uint32_t slot = 0; slot < kShellVertexLayers && slot < textureCount; ++slot) {
				uint32_t best = slot;
				for (uint32_t i = slot + 1; i < textureCount; ++i)
					if (vertexWeight[i] > vertexWeight[best])
						best = i;
				std::swap(vertexTexture[slot], vertexTexture[best]);
				std::swap(vertexWeight[slot], vertexWeight[best]);
				data.layerTexture[cellIdx][slot] = vertexTexture[slot];
				data.layerWeight[cellIdx][slot] = (uint8_t)std::clamp(vertexWeight[slot] * 255.0f + 0.5f, 0.0f, 255.0f);
			}
		}
	}

	// Exterior cells with no LAND record still get a TESObjectLAND: the engine
	// fills them with a dead-flat, unpainted plane at the worldspace's default
	// land height, which in a city worldspace sits a thousand units under the
	// built city (Windhelm bakes at -13240 with the ground at -12300). Baking
	// it hands the shell a full snow surface nobody can ever see. Real terrain
	// carries relief or painted layers; this carries neither.
	if (!painted && maxHeight - minHeight < 1.0f) {
		const std::unique_lock lock(shellCellMutex);
		uint64_t fillerKey = (uint64_t(uint32_t(coords->cellX)) << 32) | uint32_t(coords->cellY);
		// Tombstone: the snow-presence gate fails open on cells it has never
		// looked at, and without this record a city's whole floor would read
		// as never-looked-at and keep the deformation passes running there.
		if (shellFillerCells.size() > 4096)
			shellFillerCells.clear();
		shellFillerCells[fillerKey] = data.worldspaceID;
		if (auto it = shellCells.find(fillerKey); it != shellCells.end() && it->second.worldspaceID == data.worldspaceID) {
			shellCells.erase(it);
			shellDataDirty.store(true, std::memory_order_release);
		}
		return;
	}

	uint64_t key = (uint64_t(uint32_t(coords->cellX)) << 32) | uint32_t(coords->cellY);
	{
		const std::unique_lock lock(shellCellMutex);
		if (shellCells.size() > 4096)
			shellCells.clear();
		// Land material setup re-runs frequently; only mark the window dirty
		// when the baked data actually changed.
		auto it = shellCells.find(key);
		if (it != shellCells.end() && it->second.worldspaceID == data.worldspaceID &&
			it->second.height == data.height && it->second.layerTexture == data.layerTexture &&
			it->second.layerWeight == data.layerWeight && it->second.vertexAO == data.vertexAO)
			return;
		shellCells[key] = data;
		// A real bake supersedes this worldspace's filler tombstone. Another
		// worldspace's tombstone at the same coords (city vs its parent) stands.
		if (auto ft = shellFillerCells.find(key); ft != shellFillerCells.end() && ft->second == data.worldspaceID)
			shellFillerCells.erase(ft);
	}
	shellDataDirty.store(true, std::memory_order_release);
}

float SnowDeformation::GetNominalSnowDepthAt(float a_x, float a_y, float a_missing)
{
	ScopedTicks _depth(cpuCensus.depthTicks, cpuCensus.depthCalls);
	// A cell is 33 vertices = 32 intervals of kShellVertexSpacing.
	constexpr float kCellSize = kShellVertexSpacing * 32.0f;
	const int cellX = (int)std::floor(a_x / kCellSize);
	const int cellY = (int)std::floor(a_y / kCellSize);
	const int vx = std::clamp((int)std::lround((a_x - cellX * kCellSize) / kShellVertexSpacing), 0, 32);
	const int vy = std::clamp((int)std::lround((a_y - cellY * kCellSize) / kShellVertexSpacing), 0, 32);

	const uint64_t key = (uint64_t(uint32_t(cellX)) << 32) | uint32_t(cellY);
	const std::shared_lock lock(shellCellMutex);
	const auto it = shellCells.find(key);
	if (it == shellCells.end() || it->second.worldspaceID != activeWorldspace.load(std::memory_order_acquire))
		return a_missing;

	// Same layer resolve the window rebuild uses, for one vertex.
	const uint32_t idx = uint32_t(vy) * 33 + uint32_t(vx);
	float depth = 0.0f;
	const std::shared_lock textureLock(landTextureMutex);
	for (uint32_t slot = 0; slot < kShellVertexLayers; ++slot) {
		const uint16_t texture = it->second.layerTexture[idx][slot];
		if (texture < landTextures.size())
			depth += it->second.layerWeight[idx][slot] / 255.0f * landTextures[texture].depth;
	}
	return depth;
}

SnowDeformation::ShellProbe SnowDeformation::ProbeShellData(float a_x, float a_y)
{
	ShellProbe probe;
	probe.worldX = a_x;
	probe.worldY = a_y;
	probe.activeWorldspaceID = activeWorldspace.load(std::memory_order_acquire);
	probe.windowWorldspaceID = shellWindowWorldspace;

	constexpr float kCellSize = kShellVertexSpacing * 32.0f;
	probe.cellX = (int)std::floor(a_x / kCellSize);
	probe.cellY = (int)std::floor(a_y / kCellSize);
	probe.vertexX = std::clamp((int)std::lround((a_x - probe.cellX * kCellSize) / kShellVertexSpacing), 0, 32);
	probe.vertexY = std::clamp((int)std::lround((a_y - probe.cellY * kCellSize) / kShellVertexSpacing), 0, 32);

	const uint64_t key = (uint64_t(uint32_t(probe.cellX)) << 32) | uint32_t(probe.cellY);
	const std::shared_lock lock(shellCellMutex);
	const auto it = shellCells.find(key);
	if (it == shellCells.end())
		return probe;

	probe.cellFound = true;
	probe.cellWorldspace = it->second.worldspaceID;
	probe.worldspaceMatch = probe.cellWorldspace == probe.activeWorldspaceID;

	const uint32_t idx = uint32_t(probe.vertexY) * 33 + uint32_t(probe.vertexX);
	probe.height = it->second.height[idx];

	const std::shared_lock textureLock(landTextureMutex);
	for (uint32_t slot = 0; slot < kShellVertexLayers; ++slot) {
		const uint16_t texture = it->second.layerTexture[idx][slot];
		const float weight = it->second.layerWeight[idx][slot] / 255.0f;
		if (texture >= landTextures.size() || weight <= 0.0f)
			continue;
		probe.layers.push_back({ landTextures[texture].label, weight, landTextures[texture].depth });
		probe.rampDepth += weight * landTextures[texture].depth;
		probe.coverage += weight;
	}
	return probe;
}

void SnowDeformation::UpdateActiveWorldspace()
{
	auto* tes = RE::TES::GetSingleton();
	auto* worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	// Interiors keep the last exterior's state: a shop visit must not wipe the
	// tracks outside its door.
	if (!worldspace)
		return;

	const uint32_t id = worldspace->GetFormID();
	if (id == activeWorldspace.exchange(id, std::memory_order_acq_rel))
		return;

	// City worldspaces share their parent's cell coordinates AND world XY
	// (WindhelmWorld sits on the same 28-36 / 6-12 block as the Tamriel
	// terrain outside its gate), so every world-anchored cache now describes
	// the worldspace we just left. Baked cells carry their worldspace and are
	// filtered below; the object raster and the deformation map have no such
	// tag and have to be dropped.
	shellDataDirty.store(true, std::memory_order_release);
	heightMapValid = false;
	clearRequested = true;
	logger::debug("[SNOW DEFORMATION] Worldspace changed to {:08X}: dropping world-anchored caches", id);
}

void SnowDeformation::UpdateShellTerrainWindow()
{
	LoadTraceScope _loadTrace(this, "TerrainData: UpdateShellTerrainWindow");
	auto eyeFB = globals::game::frameBufferCached.GetCameraPosAdjust();
	int camCellX = (int)std::floor(eyeFB.x / (kShellVertexSpacing * kShellTexelsPerCell));
	int camCellY = (int)std::floor(eyeFB.y / (kShellVertexSpacing * kShellTexelsPerCell));

	int desiredOriginX = camCellX - kShellWindowCells / 2;
	int desiredOriginY = camCellY - kShellWindowCells / 2;

	bool originChanged = desiredOriginX != shellWindowCellX || desiredOriginY != shellWindowCellY;
	// A heightmap swap (worldspace change, or Terrain Shadows finishing its
	// load after this window was built) invalidates the far fill even when
	// the origin is unchanged.
	auto& terrainShadows = globals::features::terrainShadows;
	const std::string fillWorldspace = (terrainShadows.loaded && terrainShadows.IsHeightMapReady()) ? terrainShadows.cachedHeightmap->worldspace : std::string{};
	bool fillChanged = lastFillWorldspace != fillWorldspace;
	// A worldspace with no landscape of its own bakes nothing, so nothing
	// would ever mark the window dirty and the previous worldspace's texels
	// would stay resident.
	const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
	bool worldspaceChanged = worldspace != shellWindowWorldspace;
	if (!originChanged && !fillChanged && !worldspaceChanged && !shellDataDirty.exchange(false, std::memory_order_acq_rel))
		return;

	lodWindowRebuilds++;  // C3 event counter: whole-window height re-upload.
	shellWindowCellX = desiredOriginX;
	shellWindowCellY = desiredOriginY;
	shellWindowWorldspace = worldspace;
	shellDataDirty.store(false, std::memory_order_release);

	shellUploadScratch.resize(size_t(kShellWindowDim) * kShellWindowDim * 4);
	// One snapshot for the whole texel loop; the bake thread may register new
	// textures while it runs, and those cells are not in this window yet.
	const std::vector<float> textureDepths = LandTextureDepthSnapshot();

	uint32_t statSnowTexels = 0;
	// Cells whose blended depth goes positive somewhere: the only ground the
	// deformation map can ever show. Filled from this loop's own arithmetic,
	// so it costs one comparison per texel.
	std::unordered_set<uint64_t> snowyCells;
	float statMinH = FLT_MAX;
	float statMaxH = -FLT_MAX;
	std::unordered_set<uint64_t> statCells;

	{
		const std::shared_lock lock(shellCellMutex);
		for (int ty = 0; ty < kShellWindowDim; ++ty) {
			int cellY = shellWindowCellY + ty / kShellTexelsPerCell;
			int vy = ty % kShellTexelsPerCell;
			const ShellCellData* rowCell = nullptr;
			uint64_t rowKey = ~0ull;
			for (int tx = 0; tx < kShellWindowDim; ++tx) {
				int cellX = shellWindowCellX + tx / kShellTexelsPerCell;
				int vx = tx % kShellTexelsPerCell;

				uint64_t key = (uint64_t(uint32_t(cellX)) << 32) | uint32_t(cellY);
				if (key != rowKey) {
					auto it = shellCells.find(key);
					// A cell baked in another worldspace is not this window's
					// ground: city worldspaces reuse the coordinates of the
					// terrain outside them, so an unfiltered hit drapes the
					// countryside through the city at its own elevation.
					rowCell = it != shellCells.end() && it->second.worldspaceID == worldspace ? &it->second : nullptr;
					rowKey = key;
				}

				float* texel = &shellUploadScratch[(size_t(ty) * kShellWindowDim + tx) * 4];
				if (rowCell) {
					uint32_t idx = uint32_t(vy) * 33 + uint32_t(vx);
					// Texture depths are applied here, so the sliders retune
					// the shell from cached weights without a re-bake.
					float rampDepth = 0.0f;
					float coverage = 0.0f;
					for (uint32_t slot = 0; slot < kShellVertexLayers; ++slot) {
						const uint16_t texture = rowCell->layerTexture[idx][slot];
						if (texture >= textureDepths.size())
							continue;
						float w = rowCell->layerWeight[idx][slot] / 255.0f;
						rampDepth += w * textureDepths[texture];
						coverage += w;
					}
					texel[0] = rowCell->height[idx];
					texel[1] = rampDepth;
					texel[2] = std::min(coverage, 1.0f);
					// Land vertex AO packed into [0, 0.499): the LOD-fill CS
					// writes its provenance codes at >= 0.5 and skips baked
					// texels (x wins), so both meanings coexist in one channel.
					texel[3] = rowCell->vertexAO[idx] * (0.499f / 255.0f);

					statCells.insert(rowKey);
					if (rampDepth > 0.0f)
						snowyCells.insert(rowKey);
					statMinH = std::min(statMinH, texel[0]);
					statMaxH = std::max(statMaxH, texel[0]);
					if (texel[2] > 0.05f)
						statSnowTexels++;
				} else {
					texel[0] = kShellMissingHeight;
					texel[1] = 0.0f;
					texel[2] = 0.0f;
					texel[3] = 0.0f;
				}
			}
		}
	}

	{
		const std::unique_lock snowLock(shellSnowyCellMutex);
		shellSnowyCells = std::move(snowyCells);
	}
	shellStatCellsInWindow = (uint32_t)statCells.size();
	shellStatSnowTexels = statSnowTexels;
	shellStatMinHeight = statMinH == FLT_MAX ? 0.0f : statMinH;
	shellStatMaxHeight = statMaxH == -FLT_MAX ? 0.0f : statMaxH;

	logger::debug("[SNOW DEFORMATION] Shell window rebuilt: origin cell ({}, {}), {} cells in window, {} snow texels, height range [{:.0f}, {:.0f}]",
		shellWindowCellX, shellWindowCellY, shellStatCellsInWindow, shellStatSnowTexels, shellStatMinHeight, shellStatMaxHeight);

	globals::d3d::context->UpdateSubresource(shellTerrainTexture->resource.get(), 0, nullptr,
		shellUploadScratch.data(), kShellWindowDim * 4 * sizeof(float), 0);

	FillShellWindowFromHeightmap();
	BuildTerrainFineWindow();
}

ID3D11ComputeShader* SnowDeformation::GetTerrainFineCS()
{
	if (!terrainFineCS) {
		logger::debug("Compiling DepthSyncCS TerrainFineCS");
		terrainFineCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DepthSyncCS.hlsl", {}, "cs_5_0", "TerrainFineCS"));
	}
	return terrainFineCS;
}

void SnowDeformation::BuildTerrainFineWindow()
{
	LoadTraceScope _loadTrace(this, "TerrainData: BuildTerrainFineWindow");
	shellFineValid = false;
	auto* cs = GetTerrainFineCS();
	if (!cs || !shellTerrainTexture || !shellTerrainTexture->srv)
		return;
	auto context = globals::d3d::context;
	if (!shellTerrainFine) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kShellFineDim;
		desc.Height = kShellFineDim;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		shellTerrainFine = new Texture2D(desc, "SnowDeformation::ShellTerrainFine");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		shellTerrainFine->CreateSRV(srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		shellTerrainFine->CreateUAV(uavDesc);
	}
	if (!terrainFineCB)
		terrainFineCB = new ConstantBuffer(ConstantBufferDesc<TerrainFineCB>(), "SnowDeformation::TerrainFineCB");
	if (!shellTerrainFine->srv || !shellTerrainFine->uav)
		return;

	// Nine cells centred on the camera's: the shell's seam is at most 14,336
	// units out, and four whole cells either side is at least 16,384.
	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	const int camCellX = (int)std::floor(eye.x / cellSize);
	const int camCellY = (int)std::floor(eye.y / cellSize);
	shellFineOriginX = float(camCellX - kShellFineCells / 2) * cellSize;
	shellFineOriginY = float(camCellY - kShellFineCells / 2) * cellSize;

	TerrainFineCB cb{};
	cb.FineOriginWorld = { shellFineOriginX, shellFineOriginY };
	cb.WindowOriginWorld = { shellWindowCellX * cellSize, shellWindowCellY * cellSize };
	cb.FineDim = kShellFineDim;
	cb.WindowDim = kShellWindowDim;
	cb.TexelSize = kShellVertexSpacing;
	cb.FineTexel = kShellFineTexel;
	terrainFineCB->Update(cb);

	ID3D11Buffer* cbuf = terrainFineCB->CB();
	ID3D11ShaderResourceView* src = shellTerrainTexture->srv.get();
	ID3D11UnorderedAccessView* dst = shellTerrainFine->uav.get();
	context->CSSetConstantBuffers(1, 1, &cbuf);
	context->CSSetShaderResources(8, 1, &src);
	context->CSSetUnorderedAccessViews(5, 1, &dst, nullptr);
	context->CSSetShader(cs, nullptr, 0);
	globals::profiler->BeginPass("SnowDeformation::TerrainFine");
	context->Dispatch((kShellFineDim + 7) / 8, (kShellFineDim + 7) / 8, 1);
	globals::profiler->EndPass();
	ID3D11Buffer* nullCB = nullptr;
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	context->CSSetConstantBuffers(1, 1, &nullCB);
	context->CSSetShaderResources(8, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(5, 1, &nullUAV, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);
	shellFineValid = true;
}

ID3D11ComputeShader* SnowDeformation::GetWindowFillCS()
{
	if (!windowFillCS) {
		logger::debug("Compiling TerrainWindowFillCS");
		windowFillCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\TerrainWindowFillCS.hlsl", {}, "cs_5_0"));
	}
	return windowFillCS;
}

ID3D11ShaderResourceView* SnowDeformation::GetLODTile(const std::string& a_worldspace, int a_cellX, int a_cellY)
{
	LoadTraceScope _loadTrace(this, "TerrainData: GetLODTile");
	const uint64_t key = (uint64_t(uint32_t(a_cellX)) << 32) | uint32_t(a_cellY);
	if (auto it = lodTileCache.find(key); it != lodTileCache.end())
		return it->second.get();
	if (lodTileMisses.contains(key))
		return nullptr;

	// sRGB ignored so classification runs on the stored gamma-space values
	// regardless of how the DDS declares itself.
	std::string path = std::format("Data\\textures\\terrain\\{}\\{}.32.{}.{}.dds", a_worldspace, a_worldspace, a_cellX, a_cellY);
	std::wstring widePath(path.begin(), path.end());
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	if (FAILED(DirectX::CreateDDSTextureFromFileEx(globals::d3d::device, widePath.c_str(), 0,
			D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0,
			DirectX::DDS_LOADER_IGNORE_SRGB, nullptr, srv.put()))) {
		logger::info("[SNOW DEFORMATION] LOD tile missing: {}", path);
		lodTileMisses.insert(key);
		return nullptr;
	}
	logger::debug("[SNOW DEFORMATION] LOD tile loaded: {}", path);
	lodTileCache[key] = srv;
	return srv.get();
}

void SnowDeformation::FillShellWindowFromHeightmap()
{
	LoadTraceScope _loadTrace(this, "TerrainData: FillShellWindowFromHeightmap");
	auto& terrainShadows = globals::features::terrainShadows;
	if (!terrainShadows.loaded || !terrainShadows.texHeightMap || !terrainShadows.IsHeightMapReady()) {
		lastFillWorldspace.clear();
		return;
	}
	auto cs = GetWindowFillCS();
	if (!cs || !shellTerrainTexture->uav)
		return;

	const auto* heightmap = terrainShadows.cachedHeightmap;
	if (lastFillWorldspace != heightmap->worldspace) {
		lodTileCache.clear();
		lodTileMisses.clear();
	}

	WindowFillCB cbData{};
	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	cbData.WindowOriginWorld = { shellWindowCellX * cellSize, shellWindowCellY * cellSize };
	cbData.TexelSize = kShellVertexSpacing;
	cbData.WindowDim = kShellWindowDim;
	cbData.HeightMapScale = { 1.0f / (heightmap->pos1.x - heightmap->pos0.x), 1.0f / (heightmap->pos1.y - heightmap->pos0.y) };
	cbData.HeightMapOffset = { -heightmap->pos0.x * cbData.HeightMapScale.x, -heightmap->pos0.y * cbData.HeightMapScale.y };
	// pos0.z/pos1.z = the file's normalization range (ShadowUpdate.cs.hlsl
	// decode convention), NOT zRange (the content min/max).
	cbData.HeightRange = { heightmap->pos0.z, heightmap->pos1.z };
	cbData.SnowDepthUnits = std::max(settings.SnowClassDepths[3], 0.0f);  // "Snow 01"

	// 2x2 block of level-32 tiles (each spans exactly the window's 32 cells)
	// anchored at the window origin's tile; covers the window at any offset.
	constexpr int kTileCells = 32;
	const int tileX0 = (int)std::floor((float)shellWindowCellX / kTileCells) * kTileCells;
	const int tileY0 = (int)std::floor((float)shellWindowCellY / kTileCells) * kTileCells;
	cbData.LODTileBase = { tileX0 * cellSize, tileY0 * cellSize };
	cbData.LODTileSpan = kTileCells * cellSize;
	cbData.LODSnowSensitivity = std::clamp(settings.LODSnowSensitivity, 0.0f, 1.0f);
	ID3D11ShaderResourceView* tileSRVs[4] = {};
	for (int tileI = 0; tileI < 4; ++tileI) {
		tileSRVs[tileI] = GetLODTile(heightmap->worldspace, tileX0 + (tileI & 1) * kTileCells, tileY0 + (tileI >> 1) * kTileCells);
		(&cbData.LODTileValid.x)[tileI] = tileSRVs[tileI] ? 1.0f : 0.0f;
	}
	windowFillCB->Update(cbData);

	auto context = globals::d3d::context;
	ID3D11Buffer* cb = windowFillCB->CB();
	ID3D11ShaderResourceView* fillSRVs[5] = { terrainShadows.texHeightMap->srv.get(), tileSRVs[0], tileSRVs[1], tileSRVs[2], tileSRVs[3] };
	ID3D11UnorderedAccessView* windowUAV = shellTerrainTexture->uav.get();
	ID3D11SamplerState* sampler = shellLinearSampler.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetShaderResources(0, 5, fillSRVs);
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetUnorderedAccessViews(0, 1, &windowUAV, nullptr);
	context->CSSetShader(cs, nullptr, 0);
	globals::profiler->BeginPass("SnowDeformation::WindowFill");
	context->Dispatch((kShellWindowDim + 7) / 8, (kShellWindowDim + 7) / 8, 1);
	globals::profiler->EndPass();

	ID3D11Buffer* nullCB = nullptr;
	ID3D11ShaderResourceView* nullSRVs[5] = {};
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetShaderResources(0, 5, nullSRVs);
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	lastFillWorldspace = heightmap->worldspace;
}

void SnowDeformation::BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material)
{
	auto state = globals::state;

	// Clear first so bits never leak from the previous landscape draw.
	state->permutationData.ExtraFeatureDescriptor &= ~uint(State::ExtraFeatureDescriptors::SnowLandIsSnowMask);

	if (material == nullptr)
		return;

	uint8_t mask = 0;
	{
		const std::shared_lock lock(snowMaskMutex);
		auto it = snowMasks.find(reinterpret_cast<uintptr_t>(material));
		if (it == snowMasks.end()) {
			// Count misses only for landscape materials, where a miss is a bug.
			auto feature = material->GetFeature();
			if (feature == RE::BSShaderMaterial::Feature::kMultiTexLand || feature == static_cast<RE::BSShaderMaterial::Feature>(33))
				landMaskMisses.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		mask = it->second;
	}

	landMaskHits.fetch_add(1, std::memory_order_relaxed);
	state->permutationData.ExtraFeatureDescriptor |= uint32_t(mask) << 10;
}

struct SD_TESObjectLAND_SetupMaterial
{
	static bool thunk(RE::TESObjectLAND* land)
	{
		bool result = func(land);

		auto& snowDeformation = globals::features::snowDeformation;
		if (result && snowDeformation.loaded)
			snowDeformation.TESObjectLAND_SetupMaterial(land);

		return result;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

struct SD_BSLightingShader_SetupMaterial
{
	static void thunk(RE::BSLightingShader* shader, RE::BSLightingShaderMaterialBase const* material)
	{
		if (!material)
			return;

		func(shader, material);

		auto& snowDeformation = globals::features::snowDeformation;
		if (snowDeformation.loaded)
			snowDeformation.BSLightingShader_SetupMaterial(material);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void SnowDeformation::PostPostLoad()
{
	// Same detour target as TruePBR, whose PostPostLoad runs earlier in the
	// feature list. Detours are LIFO, so attaching now makes this hook outer:
	// it sees the final, possibly TruePBR-replaced, quad materials.
	logger::info("[SNOW DEFORMATION] Hooking TESObjectLAND");
	stl::detour_thunk<SD_TESObjectLAND_SetupMaterial>(REL::RelocationID(18368, 18791));

	logger::info("[SNOW DEFORMATION] Hooking BSLightingShader::SetupMaterial");
	stl::write_vfunc<0x4, SD_BSLightingShader_SetupMaterial>(RE::VTABLE_BSLightingShader[0]);

	InstallStaticsCaptureHook();
	InstallWaterCaptureHook();

	// Claims the co-save records. Here rather than later because a save can be
	// loaded straight from the main menu, and an unclaimed record is skipped.
	RegisterTrenchCoSave();
	RegisterAccumulationCoSave();
}

// Cell-granular on purpose: the per-texel answer lives in a GPU texture, and a
// readback would cost more than the passes this saves. Cells are 4096 units
// against a deformation window of a few hundred metres, so the overlap set is
// tiny and the test errs toward "has snow".
bool SnowDeformation::WindowHasSnow(float a_halfExtentUnits, bool a_unknownIsSnowy, uint32_t* a_verdictOut) const
{
	constexpr float kCellSize = kShellVertexSpacing * 32.0f;
	// Two cells of lead beyond the window. The snowy set only refreshes when
	// the terrain window rebuilds, which waits on the bake thread, so testing
	// the window exactly meant walking onto snow and waiting about a second
	// for the first footprint. The margin resumes while the snow is still
	// ~120 m off and the map is warm by the time it is stood on.
	constexpr float kResumeMargin = kCellSize * 2.0f;
	// Centre of the deformation window, which every caller measures out from.
	const float centreX = windowOrigin.x + deformWorldSize * 0.5f;
	const float centreY = windowOrigin.y + deformWorldSize * 0.5f;
	const float reach = a_halfExtentUnits + kResumeMargin;
	const float minX = centreX - reach;
	const float minY = centreY - reach;
	const float maxX = centreX + reach;
	const float maxY = centreY + reach;

	const int cellMinX = (int)std::floor(minX / kCellSize);
	const int cellMaxX = (int)std::floor(maxX / kCellSize);
	const int cellMinY = (int)std::floor(minY / kCellSize);
	const int cellMaxY = (int)std::floor(maxY / kCellSize);

	if (a_verdictOut)
		*a_verdictOut = kSnowGateBare;
	{
		const std::shared_lock lock(shellSnowyCellMutex);
		// No cells in the window means it has not been built yet (or holds no
		// terrain at all); answer conservatively rather than skipping on no data.
		if (shellSnowyCells.empty() && shellStatCellsInWindow == 0) {
			if (a_verdictOut)
				*a_verdictOut = kSnowGateUnknown;
			return true;
		}
		for (int cy = cellMinY; cy <= cellMaxY; ++cy) {
			for (int cx = cellMinX; cx <= cellMaxX; ++cx) {
				const uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(cy);
				if (shellSnowyCells.find(key) != shellSnowyCells.end()) {
					if (a_verdictOut)
						*a_verdictOut = kSnowGateSnowy;
					return true;
				}
			}
		}
	}

	// No snowy cell in reach. "Bare" may only be claimed about ground actually
	// looked at: a cell neither baked for the active worldspace nor tombstoned
	// as filler has simply not been seen yet - after a city gate, a door or
	// fast travel that state lasts seconds, and failing closed on it suspends
	// stamping while the player already stands on snow.
	if (a_unknownIsSnowy) {
		const uint32_t worldspace = activeWorldspace.load(std::memory_order_acquire);
		const std::shared_lock lock(shellCellMutex);
		for (int cy = cellMinY; cy <= cellMaxY; ++cy) {
			for (int cx = cellMinX; cx <= cellMaxX; ++cx) {
				const uint64_t key = (uint64_t(uint32_t(cx)) << 32) | uint32_t(cy);
				if (auto it = shellCells.find(key); it != shellCells.end() && it->second.worldspaceID == worldspace)
					continue;
				if (auto ft = shellFillerCells.find(key); ft != shellFillerCells.end() && ft->second == worldspace)
					continue;
				if (a_verdictOut)
					*a_verdictOut = kSnowGateUnknown;
				return true;
			}
		}
	}
	return false;
}

// Fine-layer readback probe (see the header). Blocking maps: one shot, on
// request, from the debug menu.
void SnowDeformation::ProbeFineLayer(const ShellCB& a_cb)
{
	fineProbeResult.clear();
	if (!shellFineValid || !shellTerrainFine || !shellTerrainTexture) {
		fineProbeResult = "fine probe: no fine window";
		return;
	}
	auto context = globals::d3d::context;
	auto device = globals::d3d::device;
	auto stage = [&](Texture2D* tex, DXGI_FORMAT fmt, uint32_t dim) {
		D3D11_TEXTURE2D_DESC d{};
		d.Width = dim;
		d.Height = dim;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_STAGING;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Texture2D> s;
		if (FAILED(device->CreateTexture2D(&d, nullptr, s.put())))
			return winrt::com_ptr<ID3D11Texture2D>{};
		context->CopyResource(s.get(), tex->resource.get());
		return s;
	};
	auto fineStage = stage(shellTerrainFine, DXGI_FORMAT_R32_FLOAT, kShellFineDim);
	auto winStage = stage(shellTerrainTexture, DXGI_FORMAT_R32G32B32A32_FLOAT, kShellWindowDim);
	if (!fineStage || !winStage) {
		fineProbeResult = "fine probe: staging alloc failed";
		return;
	}
	D3D11_MAPPED_SUBRESOURCE fm{}, wm{};
	if (FAILED(context->Map(fineStage.get(), 0, D3D11_MAP_READ, 0, &fm))) {
		fineProbeResult = "fine probe: map failed";
		return;
	}
	if (FAILED(context->Map(winStage.get(), 0, D3D11_MAP_READ, 0, &wm))) {
		context->Unmap(fineStage.get(), 0);
		fineProbeResult = "fine probe: map failed";
		return;
	}
	auto fineAt = [&](int x, int y) {
		return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(fm.pData) + size_t(y) * fm.RowPitch + size_t(x) * 4);
	};
	auto winAt = [&](int x, int y) {
		return reinterpret_cast<const float*>(static_cast<const uint8_t*>(wm.pData) + size_t(y) * wm.RowPitch + size_t(x) * 16);
	};
	constexpr float cellSize = kShellVertexSpacing * kShellTexelsPerCell;
	const int fineCellX0 = int(std::lround(shellFineOriginX / cellSize));
	const int fineCellY0 = int(std::lround(shellFineOriginY / cellSize));
	const float windowOriginX = shellWindowCellX * cellSize;
	const float windowOriginY = shellWindowCellY * cellSize;
	std::string out = std::format("fine probe: ShellFlags {} morphOff {:.0f} GridOrigin ({:.0f},{:.0f}) FineWindow ({:.0f},{:.0f},{:.0f},{:.0f}) fineOrigin ({:.0f},{:.0f}) windowOrigin ({:.0f},{:.0f}) GridToTerrainOffset ({:.0f},{:.0f})\n",
		a_cb.ShellFlags.x, a_cb.DebugNoDataMorph, a_cb.GridOrigin.x, a_cb.GridOrigin.y, a_cb.FineWindow.x, a_cb.FineWindow.y, a_cb.FineWindow.z, a_cb.FineWindow.w,
		shellFineOriginX, shellFineOriginY, windowOriginX, windowOriginY, a_cb.GridToTerrainOffset.x, a_cb.GridToTerrainOffset.y);

	// A: window texels against the baked cells, over the fine window's cells.
	uint32_t cellsBaked = 0, cellsMissing = 0, fillMismatch = 0, lodTexels = 0, sentinelBaked = 0;
	double fillMax = 0.0;
	{
		const std::shared_lock lock(shellCellMutex);
		for (int cy = 0; cy < kShellFineCells; cy++) {
			for (int cx = 0; cx < kShellFineCells; cx++) {
				const int cellX = fineCellX0 + cx, cellY = fineCellY0 + cy;
				const uint64_t key = (uint64_t(uint32_t(cellX)) << 32) | uint32_t(cellY);
				auto it = shellCells.find(key);
				const bool baked = it != shellCells.end() && it->second.worldspaceID == shellWindowWorldspace;
				(baked ? cellsBaked : cellsMissing)++;
				for (int vy = 0; vy < 32; vy++) {
					for (int vx = 0; vx < 32; vx++) {
						const int tx = (cellX - shellWindowCellX) * 32 + vx, ty = (cellY - shellWindowCellY) * 32 + vy;
						if (tx < 0 || ty < 0 || tx >= kShellWindowDim || ty >= kShellWindowDim)
							continue;
						const float* t = winAt(tx, ty);
						if (t[3] >= 0.5f)
							lodTexels++;
						if (!baked)
							continue;
						const float h = it->second.height[size_t(vy) * 33 + size_t(vx)];
						if (t[0] < -50000.0f) {
							sentinelBaked++;
							continue;
						}
						const double d = std::fabs(double(t[0]) - h);
						fillMax = std::max(fillMax, d);
						if (d > 0.01)
							fillMismatch++;
					}
				}
			}
		}
	}
	out += std::format("A fill: {} cells baked, {} missing; window vs cells max {:.3f}, {} texels off by >0.01, {} sentinel where baked, {} LOD/heightmap texels in the fine footprint\n",
		cellsBaked, cellsMissing, fillMax, fillMismatch, sentinelBaked, lodTexels);

	// B: fine texture against a CPU copy of TerrainFineCS over the window.
	auto catmull = [](float p0, float p1, float p2, float p3, float t) {
		return 0.5f * (2.0f * p1 + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
	};
	auto csReplica = [&](float worldX, float worldY) -> float {
		const float ufx = (worldX - windowOriginX) / kShellVertexSpacing, ufy = (worldY - windowOriginY) / kShellVertexSpacing;
		const int ix = int(std::floor(ufx)), iy = int(std::floor(ufy));
		const float fx = ufx - ix, fy = ufy - iy;
		const int lox = (ix >> 5) << 5, loy = (iy >> 5) << 5;
		float h[4][4];
		for (int j = 0; j < 4; j++) {
			for (int k = 0; k < 4; k++) {
				const int sx = ix + k - 1, sy = iy + j - 1;
				const int cx = std::clamp(sx, lox, lox + 32), cy = std::clamp(sy, loy, loy + 32);
				const int mx = 2 * cx - sx, my = 2 * cy - sy;
				const int wmax = kShellWindowDim - 1;
				const float he = winAt(std::clamp(cx, 0, wmax), std::clamp(cy, 0, wmax))[0];
				const float hm = winAt(std::clamp(mx, 0, wmax), std::clamp(my, 0, wmax))[0];
				if (he < -50000.0f || hm < -50000.0f)
					return -100000.0f;
				h[j][k] = (sx != cx || sy != cy) ? 2.0f * he - hm : he;
			}
		}
		float rows[4];
		for (int r = 0; r < 4; r++)
			rows[r] = catmull(h[r][0], h[r][1], h[r][2], h[r][3], fx);
		return catmull(rows[0], rows[1], rows[2], rows[3], fy);
	};
	uint32_t csMismatch = 0, fineSentinels = 0, csChecked = 0;
	double csMax = 0.0, csSq = 0.0;
	std::string firstBad;
	for (int y = 0; y < (int)kShellFineDim; y++) {
		for (int x = 0; x < (int)kShellFineDim; x++) {
			const float g = fineAt(x, y);
			const float ref = csReplica(shellFineOriginX + x * kShellFineTexel, shellFineOriginY + y * kShellFineTexel);
			if (g < -50000.0f)
				fineSentinels++;
			if (g < -50000.0f || ref < -50000.0f) {
				if ((g < -50000.0f) != (ref < -50000.0f) && csMismatch++ < 3)
					firstBad += std::format("  sentinel disagreement at fine ({},{}): gpu {:.1f} cpu {:.1f}\n", x, y, g, ref);
				continue;
			}
			csChecked++;
			const double d = std::fabs(double(g) - ref);
			csMax = std::max(csMax, d);
			csSq += d * d;
			if (d > 0.05 && csMismatch++ < 3)
				firstBad += std::format("  fine ({},{}) world ({:.0f},{:.0f}): gpu {:.2f} cpu {:.2f}\n", x, y, shellFineOriginX + x * kShellFineTexel, shellFineOriginY + y * kShellFineTexel, g, ref);
		}
	}
	out += std::format("B pass: {} fine texels checked, {} sentinel; gpu vs cpu max {:.3f} rms {:.4f}, {} off by >0.05\n{}",
		csChecked, fineSentinels, csMax, csChecked ? std::sqrt(csSq / csChecked) : 0.0, csMismatch, firstBad);

	// C: the shader's lookup along the camera's forward line, against the
	// cells directly (the rule the landscape probe measured) and against the
	// 128-texel window's triangulated height.
	auto shaderFine = [&](float gridLocalX, float gridLocalY) -> float {
		const float tfx = (a_cb.FineWindow.x + gridLocalX) / a_cb.FineWindow.w, tfy = (a_cb.FineWindow.y + gridLocalY) / a_cb.FineWindow.w;
		if (a_cb.FineWindow.z < 0.5f || tfx < 0.0f || tfy < 0.0f || tfx >= a_cb.FineWindow.z - 1.0f || tfy >= a_cb.FineWindow.z - 1.0f)
			return -100000.0f;
		const int f0x = int(tfx), f0y = int(tfy);
		const float ffx = tfx - f0x, ffy = tfy - f0y;
		const float g00 = fineAt(f0x, f0y), g10 = fineAt(f0x + 1, f0y), g01 = fineAt(f0x, f0y + 1), g11 = fineAt(f0x + 1, f0y + 1);
		if (std::min({ g00, g10, g01, g11 }) <= -50000.0f)
			return -100000.0f;
		const bool slash = ((f0x + f0y) & 1) == 0;
		const float hA = ffy <= ffx ? g00 + (g10 - g00) * ffx + (g11 - g10) * ffy : g00 + (g01 - g00) * ffy + (g11 - g01) * ffx;
		const float hB = (ffx + ffy) <= 1.0f ? g00 + (g10 - g00) * ffx + (g01 - g00) * ffy : g11 + (g10 - g11) * (1.0f - ffy) + (g01 - g11) * (1.0f - ffx);
		return slash ? hA : hB;
	};
	auto windowTri = [&](float worldX, float worldY) -> float {
		const float tx = (worldX - windowOriginX) / kShellVertexSpacing, ty = (worldY - windowOriginY) / kShellVertexSpacing;
		const int t0x = int(std::floor(tx)), t0y = int(std::floor(ty));
		if (t0x < 0 || t0y < 0 || t0x + 1 >= kShellWindowDim || t0y + 1 >= kShellWindowDim)
			return -100000.0f;
		const float fx = tx - t0x, fy = ty - t0y;
		const float h00 = winAt(t0x, t0y)[0], h10 = winAt(t0x + 1, t0y)[0], h01 = winAt(t0x, t0y + 1)[0], h11 = winAt(t0x + 1, t0y + 1)[0];
		if (std::min({ h00, h10, h01, h11 }) <= -50000.0f)
			return -100000.0f;
		// Same parity rule as the shell's quad diagonals.
		const long qx = std::lround(std::floor(worldX / kShellVertexSpacing)), qy = std::lround(std::floor(worldY / kShellVertexSpacing));
		const bool slash = ((qx + qy) & 1) == 0;
		const float hA = fy <= fx ? h00 + (h10 - h00) * fx + (h11 - h10) * fy : h00 + (h01 - h00) * fy + (h11 - h01) * fx;
		const float hB = (fx + fy) <= 1.0f ? h00 + (h10 - h00) * fx + (h01 - h00) * fy : h11 + (h10 - h11) * (1.0f - fy) + (h01 - h11) * (1.0f - fx);
		return slash ? hA : hB;
	};
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	float fwdX = 1.0f, fwdY = 0.0f;
	if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		const auto& r = cam->cameraRoot->world.rotate;
		const float fx = r.entry[0][1], fy = r.entry[1][1];
		const float len = std::sqrt(fx * fx + fy * fy);
		if (len > 1e-3f) {
			fwdX = fx / len;
			fwdY = fy / len;
		}
	}
	out += std::format("C line: eye ({:.0f},{:.0f}) fwd ({:.2f},{:.2f}); dist: shader-fine | cpu-fine(cells) | window-tri\n", eye.x, eye.y, fwdX, fwdY);
	{
		const std::shared_lock lock(shellCellMutex);
		auto cellHeight = [&](long gx, long gy, bool& a_ok) -> float {
			const long cellX = static_cast<long>(std::floor(gx / 32.0)), cellY = static_cast<long>(std::floor(gy / 32.0));
			const uint64_t key = (uint64_t(uint32_t(cellX)) << 32) | uint32_t(cellY);
			auto it = shellCells.find(key);
			if (it == shellCells.end() || it->second.worldspaceID != shellWindowWorldspace) {
				a_ok = false;
				return 0.0f;
			}
			const float h = it->second.height[size_t(gy - cellY * 32) * 33 + size_t(gx - cellX * 32)];
			if (h < -50000.0f)
				a_ok = false;
			return h;
		};
		auto cellsCR = [&](float worldX, float worldY) -> float {
			const double gxf = worldX / 128.0, gyf = worldY / 128.0;
			const long ix = static_cast<long>(std::floor(gxf)), iy = static_cast<long>(std::floor(gyf));
			const float fx = float(gxf - ix), fy = float(gyf - iy);
			const long lox = static_cast<long>(std::floor(ix / 32.0)) * 32, loy = static_cast<long>(std::floor(iy / 32.0)) * 32;
			float h[4][4];
			bool ok = true;
			for (int j = 0; j < 4; j++) {
				for (int k = 0; k < 4; k++) {
					const long sx = ix + k - 1, sy = iy + j - 1;
					const long cx = std::clamp(sx, lox, lox + 32), cy = std::clamp(sy, loy, loy + 32);
					const float he = cellHeight(cx, cy, ok);
					const float hm = cellHeight(2 * cx - sx, 2 * cy - sy, ok);
					h[j][k] = (sx != cx || sy != cy) ? 2.0f * he - hm : he;
				}
			}
			if (!ok)
				return -100000.0f;
			float rows[4];
			for (int r = 0; r < 4; r++)
				rows[r] = catmull(h[r][0], h[r][1], h[r][2], h[r][3], fx);
			return catmull(rows[0], rows[1], rows[2], rows[3], fy);
		};
		for (int k = 1; k <= 16; k++) {
			const float d = 256.0f * k;
			const float wx = eye.x + fwdX * d, wy = eye.y + fwdY * d;
			const float sf = shaderFine(wx - a_cb.GridOrigin.x, wy - a_cb.GridOrigin.y);
			const float cf = cellsCR(wx, wy);
			const float wt = windowTri(wx, wy);
			out += std::format("  {:5.0f}: {:9.1f} | {:9.1f} | {:9.1f}{}\n", d, sf, cf, wt,
				(sf > -50000.0f && cf > -50000.0f && std::fabs(sf - cf) > 0.5f) ? "  <-- shader != cells" : "");
		}

		// D: the vertex bake along the view axis - the height the vertex stage
		// stood on LAST frame (the bake runs after this fill) - against the
		// cells and against the data morph's coarse bilinear, with the band.
		if (shellVertexBake && shellVertexBake->resource) {
			const uint32_t bakeDim = shellVertexBake->desc.Width;
			auto bakeStage = stage(shellVertexBake, DXGI_FORMAT_R32G32B32A32_FLOAT, bakeDim);
			D3D11_MAPPED_SUBRESOURCE bm{};
			if (bakeStage && SUCCEEDED(context->Map(bakeStage.get(), 0, D3D11_MAP_READ, 0, &bm))) {
				auto bakeAt = [&](int x, int y) {
					return reinterpret_cast<const float*>(static_cast<const uint8_t*>(bm.pData) + size_t(y) * bm.RowPitch + size_t(x) * 16);
				};
				auto warpAxis = [](int u) {
					float a = float(std::abs(u));
					float off = 0.0f;
					for (int band = 0; band < kShellWarpBands; ++band) {
						const float take = std::min(a, kShellWarpBandVerts[band]);
						off += take * kShellWarpBandMul[band];
						a -= take;
					}
					off += a * kShellWarpBandMul[kShellWarpBands - 1];
					return (u < 0 ? -1.0f : (u > 0 ? 1.0f : 0.0f)) * off * kShellGridSpacing;
				};
				auto bandOf = [](int u, float& a_t) {
					const float a = float(std::abs(u));
					float prev = 0.0f;
					for (int band = 0; band < kShellWarpBands; ++band) {
						const float acc = prev + kShellWarpBandVerts[band];
						if (a < acc) {
							a_t = std::clamp((a - prev) / std::max(kShellWarpBandVerts[band], 1.0f), 0.0f, 1.0f);
							return kShellWarpBandMul[band] * kShellGridSpacing;
						}
						prev = acc;
					}
					a_t = 1.0f;
					return kShellWarpBandMul[kShellWarpBands - 1] * kShellGridSpacing;
				};
				const bool alongX = std::fabs(fwdX) >= std::fabs(fwdY);
				const int sgn = (alongX ? fwdX : fwdY) < 0.0f ? -1 : 1;
				const int c = int(kShellGridDim / 2);
				const float halfSpan = ShellWarpedHalfSpan();
				out += std::format("D bake (last frame, {}{} axis): dist step morphT | bake terrain | cells | coarse bilinear | bake z\n", sgn < 0 ? "-" : "+", alongX ? "x" : "y");
				float nextPrint = 128.0f;
				for (int u = 1; u <= c; u++) {
					const float off = warpAxis(u * sgn);
					const float dist = std::fabs(off);
					if (dist < nextPrint)
						continue;
					if (dist > 4200.0f)
						break;
					nextPrint = dist + 128.0f;
					float t = 0.0f;
					const float step = bandOf(u, t);
					const int gx = alongX ? c + u * sgn : c;
					const int gy = alongX ? c : c + u * sgn;
					if (gx < 0 || gy < 0 || gx >= int(bakeDim) || gy >= int(bakeDim))
						break;
					const float wx = a_cb.GridOrigin.x + halfSpan + (alongX ? off : 0.0f);
					const float wy = a_cb.GridOrigin.y + halfSpan + (alongX ? 0.0f : off);
					const float* b = bakeAt(gx, gy);
					const float cr = cellsCR(wx, wy);
					float coarse = -100000.0f;
					if (step > kShellGridSpacing) {
						const float cs = step * 2.0f;
						const float bx = std::floor(wx / cs) * cs, by = std::floor(wy / cs) * cs;
						const float fx = (wx - bx) / cs, fy = (wy - by) / cs;
						const float h00 = cellsCR(bx, by), h10 = cellsCR(bx + cs, by), h01 = cellsCR(bx, by + cs), h11 = cellsCR(bx + cs, by + cs);
						if (std::min({ h00, h10, h01, h11 }) > -50000.0f)
							coarse = std::lerp(std::lerp(h00, h10, fx), std::lerp(h01, h11, fx), fy);
					}
					out += std::format("  {:5.0f} {:4.0f} {:.2f} | {:9.1f} | {:9.1f} | {:9.1f} | {:9.1f}{}\n", dist, step, t, b[2], cr, coarse, b[0],
						(cr > -50000.0f && std::fabs(b[2] - cr) > 2.0f) ? "  <-- bake != cells" : "");
				}
				context->Unmap(bakeStage.get(), 0);
			}
		}
	}
	context->Unmap(winStage.get(), 0);
	context->Unmap(fineStage.get(), 0);
	fineProbeResult = out;
	logger::info("[SNOW DEFORMATION] {}", out);
}
