#include "Features/SnowDeformation.h"

#include <d3dcompiler.h>

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

void SnowDeformation::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->shaderProperty || !a_pass->geometry)
		return;
	if (!settings.EnableSnowDeformation || (settings.ObjectsSnowDepth <= 0.01f && settings.SnowMeshesDepth <= 0.01f && settings.RoadMeshesDepth <= 0.01f))
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
	if (!(flags.all(Flag::kProjectedUV) && flags.all(Flag::kSnow))) {
		auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
		if (!material)
			return;

		static std::unordered_map<const void*, bool> driftMaterialCache;
		if (driftMaterialCache.size() > 4096)
			driftMaterialCache.clear();
		auto [it, inserted] = driftMaterialCache.try_emplace(material, false);
		if (inserted) {
			if (auto textureSet = material->textureSet.get()) {
				if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse)) {
					std::string lowered(path);
					std::transform(lowered.begin(), lowered.end(), lowered.begin(),
						[](unsigned char c) { return (char)std::tolower(c); });
					// Drifts wear plain LANDSCAPE snow textures (no "drift" in
					// the path); requiring the landscape folder keeps frosted
					// plants (plant/tree folders) out.
					it->second = lowered.find("drift") != std::string::npos ||
					             (lowered.find("landscape") != std::string::npos && lowered.find("snow") != std::string::npos);
				}
			}
		}
		if (!it->second)
			return;
	}

	// Range cap (Object Snow slider): distant mountains are snow-projected
	// everywhere in Skyrim; the skin only matters within the chosen range.
	auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
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
		func(shader, a_pass, a_flags);

		auto& snowDeformation = globals::features::snowDeformation;
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
static ID3DBlob* SD_CompileShaderBlob(const wchar_t* a_path, const char* a_target, const char* a_stageDefine, const char* a_extraDefine = nullptr)
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
	if (!staticsPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER"));
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
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "PATCH"));
		if (blob) {
			if (SUCCEEDED(globals::d3d::device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &patchPS)))
				Util::SetResourceName(patchPS, "SnowDeformation::TrenchPatchPS");
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
	if (!heightScrollCS)
		heightScrollCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\HeightMapProcessCS.hlsl", {}, "cs_5_0", "ScrollCS"));

	if (!staticsVS || !staticsPS || !heightVS || !heightPS || !heightScrollCS) {
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
	processData.GhostDecay = 0.5f;
	heightProcessCB->Update(processData);
	heightWindowCenter = newCenter;
	heightMapValid = true;

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
	ID3D11ShaderResourceView* nullSmoothSRV = nullptr;
	context->VSSetShaderResources(10, 1, &nullSmoothSRV);
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
	if (capturedStatics.empty() || (settings.ObjectsSnowDepth <= 0.01f && settings.SnowMeshesDepth <= 0.01f && settings.RoadMeshesDepth <= 0.01f))
		return;
	if (!EnsureStaticsShaders())
		return;

	auto context = globals::d3d::context;
	auto device = globals::d3d::device;

	context->VSSetShader(staticsVS, nullptr, 0);
	context->PSSetShader(staticsPS, nullptr, 0);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);

	globals::profiler->BeginPass("SnowDeformation::StaticsShell");
	// One-shot skip diagnostics: geometries that capture but cannot draw are
	// the "why is THIS rock bare" cases; name the reason in the log.
	static std::unordered_set<std::string> loggedSkips;
	auto logSkip = [](RE::BSGeometry* a_geometry, const char* a_reason) {
		if (loggedSkips.size() < 24 && loggedSkips.insert(std::string(a_geometry->name.c_str()) + a_reason).second)
			logger::info("[SNOW DEFORMATION] Statics skip '{}': {}", a_geometry->name.c_str(), a_reason);
	};

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
		staticsCB->Update(scb);

		context->DrawIndexed(indexCount, 0, 0);
	}
	globals::profiler->EndPass();

	// Leave IA clean, mirroring DrawShell's convention (game state manager
	// rebinds via DIRTY_RENDERTARGET).
	ID3D11Buffer* nullVB = nullptr;
	UINT zero = 0;
	context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
	context->IASetInputLayout(nullptr);
	ID3D11ShaderResourceView* nullSmoothSRV = nullptr;
	context->VSSetShaderResources(10, 1, &nullSmoothSRV);

	// trench PATCH: the landscape shell's dense-grid carve applied to object
	// tops; real carved geometry drawn after the skins so it shows through
	// their dithered trench hand-off holes. SV_VertexID grid, no IA state.
	if (patchVS && patchPS && heightSkinDepth && (settings.SnowMeshesDepth > 1.0f || settings.RoadMeshesDepth > 1.0f)) {
		globals::profiler->BeginPass("SnowDeformation::TrenchPatch");
		context->VSSetShader(patchVS, nullptr, 0);
		context->PSSetShader(patchPS, nullptr, 0);

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
		staticsCB->Update(scb);

		ID3D11ShaderResourceView* patchSRVs[2] = { heightTopRaw[heightCurrent]->srv.get(), heightSkinDepth->srv.get() };
		context->VSSetShaderResources(11, 2, patchSRVs);
		context->Draw(256 * 256 * 6, 0);

		ID3D11ShaderResourceView* nullHeightSRVs[2] = { nullptr, nullptr };
		context->VSSetShaderResources(11, 2, nullHeightSRVs);
		globals::profiler->EndPass();
	}

	ID3D11Buffer* nullCB = nullptr;
	context->VSSetConstantBuffers(1, 1, &nullCB);
	context->PSSetConstantBuffers(1, 1, &nullCB);
}
