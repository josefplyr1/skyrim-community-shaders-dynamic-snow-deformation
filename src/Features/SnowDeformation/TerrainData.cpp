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
