#include "Features/SnowDeformation.h"
#include "Features/SnowDeformation/DevSwitches.h"

#include "Deferred.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <cctype>

void SnowDeformation::CreateBloodTextures(const D3D11_TEXTURE2D_DESC& a_mapDesc)
{
	delete bloodMapTexture;
	bloodMapTexture = nullptr;
	delete bloodClockTexture;
	bloodClockTexture = nullptr;
	try {

	D3D11_TEXTURE2D_DESC desc = a_mapDesc;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	bloodMapTexture = new Texture2D(desc, "SnowDeformation::BloodMap");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtv{
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{
			.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		bloodMapTexture->CreateSRV(srv);
		bloodMapTexture->CreateRTV(rtv);
		bloodMapTexture->CreateUAV(uav);
	}
	desc.Format = DXGI_FORMAT_R32G32_FLOAT;
	bloodClockTexture = new Texture2D(desc, "SnowDeformation::BloodClock");
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_RENDER_TARGET_VIEW_DESC rtv{
			.Format = desc.Format,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		bloodClockTexture->CreateSRV(srv);
		bloodClockTexture->CreateRTV(rtv);
		bloodClockTexture->CreateUAV(uav);
	}
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto context = globals::d3d::context;
	context->ClearRenderTargetView(bloodMapTexture->rtv.get(), zero);
	context->ClearRenderTargetView(bloodClockTexture->rtv.get(), zero);
	bloodMapWritten = false;
	bloodSeen.clear();
	bloodTilesDrop = true;
	logger::info("[SNOW DEFORMATION] blood map {}x{} created", desc.Width, desc.Height);
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] blood map creation failed: {} - blood on snow is off", e.what());
		delete bloodMapTexture;
		bloodMapTexture = nullptr;
		delete bloodClockTexture;
		bloodClockTexture = nullptr;
	}
}

ID3D11ComputeShader* SnowDeformation::GetBloodRingCS()
{
	if (!bloodRingCS) {
		logger::debug("Compiling DeformationUpdateCS:BloodRingCS");
		bloodRingCS = static_cast<ID3D11ComputeShader*>(CompileSnowShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0", "BloodRingCS"));
	}
	return bloodRingCS;
}

void SnowDeformation::ReleaseBloodShaders()
{
	if (bloodRingCS)
		bloodRingCS->Release();
	bloodRingCS = nullptr;
	for (auto** shader : { &bloodVS, &bloodSkinVS, &bloodDiscVS }) {
		if (*shader)
			(*shader)->Release();
		*shader = nullptr;
	}
	for (auto** shader : { &bloodPS, &bloodDiscPS }) {
		if (*shader)
			(*shader)->Release();
		*shader = nullptr;
	}
	if (bloodOverlayVS)
		bloodOverlayVS->Release();
	bloodOverlayVS = nullptr;
	if (bloodOverlayPS)
		bloodOverlayPS->Release();
	bloodOverlayPS = nullptr;
	if (runePS)
		runePS->Release();
	runePS = nullptr;
	if (decalMaskVS)
		decalMaskVS->Release();
	decalMaskVS = nullptr;
	if (decalMaskSkinVS)
		decalMaskSkinVS->Release();
	decalMaskSkinVS = nullptr;
	if (decalMaskPS)
		decalMaskPS->Release();
	decalMaskPS = nullptr;
	decalMaskFailed = false;
	runeResourcesFailed = false;
	bloodVSBlob = nullptr;
	bloodSkinVSBlob = nullptr;
	bloodILCache.clear();
	bloodSkinILCache.clear();
	bloodShadersFailed = false;
	ReleaseBloodTiles();
}

bool SnowDeformation::EnsureBloodResources()
{
	if (bloodShadersFailed)
		return false;
	try {
		return EnsureBloodResourcesImpl();
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] blood resources failed: {} - blood on snow is off", e.what());
		bloodShadersFailed = true;
		return false;
	}
}

bool SnowDeformation::EnsureBloodResourcesImpl()
{
	auto* device = globals::d3d::device;
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl";
	if (!bloodVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER"));
		if (blob) {
			bloodVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodVS)))
				Util::SetResourceName(bloodVS, "SnowDeformation::BloodCaptureVS");
		}
	}
	if (!bloodSkinVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SKINNED"));
		if (blob) {
			bloodSkinVSBlob = blob;
			if (SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodSkinVS)))
				Util::SetResourceName(bloodSkinVS, "SnowDeformation::BloodCaptureSkinVS");
		}
	}
	if (!bloodDiscVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "DISC"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodDiscVS)))
			Util::SetResourceName(bloodDiscVS, "SnowDeformation::BloodCaptureDiscVS");
	}
	if (!bloodPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodPS)))
			Util::SetResourceName(bloodPS, "SnowDeformation::BloodCapturePS");
	}
	if (!bloodDiscPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "DISC"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodDiscPS)))
			Util::SetResourceName(bloodDiscPS, "SnowDeformation::BloodCaptureDiscPS");
	}
	if (!bloodOverlayVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "OVERLAY"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlayVS)))
			Util::SetResourceName(bloodOverlayVS, "SnowDeformation::BloodOverlayVS");
	}
	if (!bloodOverlayPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "OVERLAY"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &bloodOverlayPS)))
			Util::SetResourceName(bloodOverlayPS, "SnowDeformation::BloodOverlayPS");
	}
	if (!bloodVS || !bloodSkinVS || !bloodDiscVS || !bloodPS || !bloodDiscPS || !bloodOverlayVS || !bloodOverlayPS) {
		bloodShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Blood on snow disabled (shader compilation failed)");
		return false;
	}
	if (!bloodOverlayBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = TRUE;
		for (int i = 0; i < 8; i++)
			blendDesc.RenderTarget[i].RenderTargetWriteMask = 0;
		for (int i : { 0, 2, 3, 4, 5 }) {
			auto& rt = blendDesc.RenderTarget[i];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D11_BLEND_ONE;
			rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D11_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
			rt.DestBlendAlpha = D3D11_BLEND_ONE;
			rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
		}
		if (FAILED(device->CreateBlendState(&blendDesc, bloodOverlayBlendState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: overlay blend state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodOverlayDepthState) {
		D3D11_DEPTH_STENCIL_DESC dsDesc{};
		dsDesc.DepthEnable = FALSE;
		dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
		if (FAILED(device->CreateDepthStencilState(&dsDesc, bloodOverlayDepthState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: overlay depth state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(BloodCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bloodCB = new ConstantBuffer(cbDesc, "SnowDeformation::BloodCB");
	}
	if (!bloodSkinCB) {
		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = sizeof(BloodSkinCB);
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bloodSkinCB = new ConstantBuffer(cbDesc, "SnowDeformation::BloodSkinCB");
	}
	if (!bloodBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		blendDesc.IndependentBlendEnable = TRUE;
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_ZERO;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_MAX;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		blendDesc.RenderTarget[1].BlendEnable = FALSE;
		blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(device->CreateBlendState(&blendDesc, bloodBlendState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: deposit blend state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodRasterState) {
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&rasterDesc, bloodRasterState.put()))) {
			logger::error("[SNOW DEFORMATION] blood: raster state failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodSampler) {
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&sampDesc, bloodSampler.put()))) {
			logger::error("[SNOW DEFORMATION] blood: sampler failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodDiscBuffer) {
		D3D11_BUFFER_DESC bufDesc{};
		bufDesc.ByteWidth = sizeof(float4) * 2 * kBloodMaxDiscs;
		bufDesc.Usage = D3D11_USAGE_DYNAMIC;
		bufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufDesc.StructureByteStride = sizeof(float4);
		if (FAILED(device->CreateBuffer(&bufDesc, nullptr, bloodDiscBuffer.put()))) {
			logger::error("[SNOW DEFORMATION] blood: disc buffer failed");
			bloodShadersFailed = true;
			return false;
		}
		Util::SetResourceName(bloodDiscBuffer.get(), "SnowDeformation::BloodDiscs");
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 2 * kBloodMaxDiscs;
		if (FAILED(device->CreateShaderResourceView(bloodDiscBuffer.get(), &srvDesc, bloodDiscSRV.put()))) {
			logger::error("[SNOW DEFORMATION] blood: disc view failed");
			bloodShadersFailed = true;
			return false;
		}
	}
	if (!bloodResourcesLogged) {
		bloodResourcesLogged = true;
		logger::info("[SNOW DEFORMATION] blood shaders and states ready");
	}
	return true;
}

void SnowDeformation::RegisterDecalOverlay(const BloodCapture& a_capture)
{
	if (capturedLoaded == 0 && capturedLoadedPrev == 0)
		return;
	auto* geometry = a_capture.geometry.get();
	if (bloodOverlaySet.contains(geometry))
		return;
	if (!decalReachGateOff && !DecalReachesSnow(geometry->worldBound)) {
		decalReachMisses++;
		return;
	}
	const BloodCapture& capture = a_capture;
	if (bloodOverlays.size() < 512 && bloodOverlaySet.insert(geometry).second) {
		RECT rect{};
		UINT viewports = 1;
		globals::d3d::context->RSGetViewports(&viewports, &bloodViewport);
		const bool haveRect = viewports >= 1 && BloodScreenRect(geometry, rect);
		auto unite = [](RECT& a, const RECT& b) {
			a.left = std::min(a.left, b.left);
			a.top = std::min(a.top, b.top);
			a.right = std::max(a.right, b.right);
			a.bottom = std::max(a.bottom, b.bottom);
		};
		if (haveRect) {
			if (bloodRectFrameValid)
				unite(bloodRectFrame, rect);
			else
				bloodRectFrame = rect;
			bloodRectFrameValid = true;
		}
		if (bloodOverlays.empty() && settings.ProjSnowMatch) {
			bool any = haveRect;
			bloodCopyRect = rect;
			if (bloodRectPrevValid) {
				RECT prev = bloodRectPrev;
				const LONG gx = (prev.right - prev.left) / 12 + 8;
				const LONG gy = (prev.bottom - prev.top) / 12 + 8;
				prev.left = std::max(prev.left - gx, 0L);
				prev.top = std::max(prev.top - gy, 0L);
				prev.right = std::min(prev.right + gx, LONG(bloodViewport.Width));
				prev.bottom = std::min(prev.bottom + gy, LONG(bloodViewport.Height));
				if (any)
					unite(bloodCopyRect, prev);
				else
					bloodCopyRect = prev;
				any = true;
			}
			if (!any)
				bloodCopyRect = { 0, 0, LONG(bloodViewport.Width), LONG(bloodViewport.Height) };
			CopyBloodTargets(true);
		}
		bloodOverlays.push_back(capture);
	}
}

void SnowDeformation::MarkCoatReach(float a_x, float a_y, float a_radius)
{
	const float reach = a_radius + settings.RoadMeshesDepth + kSkinCullMargin + kDecalOverlayMaxLift;
	const int x0 = std::max(int(std::floor((a_x - reach - coatReachOrigin[0]) / kCoatReachCell)), 0);
	const int x1 = std::min(int(std::floor((a_x + reach - coatReachOrigin[0]) / kCoatReachCell)), coatReachDim - 1);
	const int y0 = std::max(int(std::floor((a_y - reach - coatReachOrigin[1]) / kCoatReachCell)), 0);
	const int y1 = std::min(int(std::floor((a_y + reach - coatReachOrigin[1]) / kCoatReachCell)), coatReachDim - 1);
	for (int y = y0; y <= y1; ++y) {
		uint64_t* row = coatReachGrid.data() + size_t(y) * coatReachWords;
		for (int w = x0 >> 6; w <= x1 >> 6; ++w)
			row[w] |= (~0ull >> (63 - std::min(x1 - (w << 6), 63))) & (~0ull << std::max(x0 - (w << 6), 0));
	}
}

bool SnowDeformation::DecalReachesSnow(const RE::NiBound& a_bound)
{
	const uint32_t frame = globals::state->frameCount;
	if (settings.VolumeSnow || shellDrawGroundGated || shellEarlyFrame == frame)
		return true;
	if (coatReachFrame != frame) {
		coatReachFrame = frame;
		coatReachMarked = 0;
		coatReachDim = 0;
		int sqMinX, sqMinY, sqMaxX, sqMaxY;
		if (LoadedCellSquare(sqMinX, sqMinY, sqMaxX, sqMaxY)) {
			constexpr float kCellSize = kShellVertexSpacing * 32.0f;
			coatReachOrigin[0] = float(sqMinX - 1) * kCellSize;
			coatReachOrigin[1] = float(sqMinY - 1) * kCellSize;
			coatReachDim = int(float(std::max(sqMaxX - sqMinX, sqMaxY - sqMinY) + 3) * kCellSize / kCoatReachCell);
			coatReachWords = (coatReachDim + 63) >> 6;
			coatReachGrid.assign(size_t(coatReachDim) * coatReachWords, 0ull);
			for (const auto& b : coatReachBounds[1])
				MarkCoatReach(b.x, b.y, b.z);
		}
	}
	if (coatReachDim == 0)
		return true;
	for (; coatReachMarked < coatReachBounds[0].size(); ++coatReachMarked) {
		const auto& b = coatReachBounds[0][coatReachMarked];
		MarkCoatReach(b.x, b.y, b.z);
	}
	const float r = a_bound.radius;
	const int x0 = int(std::floor((a_bound.center.x - r - coatReachOrigin[0]) / kCoatReachCell));
	const int x1 = int(std::floor((a_bound.center.x + r - coatReachOrigin[0]) / kCoatReachCell));
	const int y0 = int(std::floor((a_bound.center.y - r - coatReachOrigin[1]) / kCoatReachCell));
	const int y1 = int(std::floor((a_bound.center.y + r - coatReachOrigin[1]) / kCoatReachCell));
	if (!(r >= 0.0f) || x0 < 0 || y0 < 0 || x1 >= coatReachDim || y1 >= coatReachDim)
		return true;
	for (int y = y0; y <= y1; ++y) {
		const uint64_t* row = coatReachGrid.data() + size_t(y) * coatReachWords;
		for (int w = x0 >> 6; w <= x1 >> 6; ++w)
			if (row[w] & (~0ull >> (63 - std::min(x1 - (w << 6), 63))) & (~0ull << std::max(x0 - (w << 6), 0)))
				return true;
	}
	return false;
}

void SnowDeformation::CaptureDecalOverlay(RE::BSRenderPass* a_pass)
{
	auto* geometry = a_pass->geometry;
	auto* property = a_pass->shaderProperty;
	auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
	if (!geometry || !material || bloodOverlaySet.contains(geometry))
		return;
	auto* texture = material->diffuseTexture.get();
	if (!texture || !texture->rendererTexture || !texture->rendererTexture->resourceView)
		return;
	auto& runtime = geometry->GetGeometryRuntimeData();
	float threshold = -1.0f;
	if (auto* alpha = runtime.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		threshold = float(alpha->alphaThreshold) / 255.0f;
	BloodCapture capture{};
	capture.geometry = RE::NiPointer<RE::BSGeometry>(geometry);
	capture.world = geometry->world;
	capture.diffuse.copy_from(texture->rendererTexture->resourceView);
	capture.texcoord = { material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	capture.alpha = property->alpha * material->materialAlpha;
	capture.alphaThreshold = threshold;
	capture.reveal = 1.0f;
	capture.skinned = false;
	capture.mask = true;
	if (decalOverlayNamesLogged.size() < 16) {
		const char* path = "";
		if (auto textureSet = material->textureSet.get())
			if (auto diffuse = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
				path = diffuse;
		if (decalOverlayNamesLogged.insert(path).second)
			logger::info("[SNOW DEFORMATION] decal overlay takes '{}' tex='{}'", geometry->name.c_str() ? geometry->name.c_str() : "", path);
	}
	RegisterDecalOverlay(capture);
}

void SnowDeformation::CaptureBloodDraw(RE::BSRenderPass* a_pass, bool a_skinned)
{
	auto* geometry = a_pass->geometry;
	auto* property = a_pass->shaderProperty;
	auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
	if (!material)
		return;
	auto* texture = material->diffuseTexture.get();
	if (!texture || !texture->rendererTexture || !texture->rendererTexture->resourceView)
		return;
	auto& runtime = geometry->GetGeometryRuntimeData();
	const void* vb = runtime.rendererData ? runtime.rendererData->vertexBuffer : nullptr;
	if (!vb)
		return;
	const char* diffusePath = "";
	if (auto textureSet = material->textureSet.get())
		if (auto path = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse))
			diffusePath = path;
	std::string pathLower(diffusePath);
	std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	auto* alphaProperty = runtime.alphaProperty.get();
	const bool blendsOver = !alphaProperty || !alphaProperty->GetAlphaBlending() ||
	                        (alphaProperty->GetSrcBlendMode() == RE::NiAlphaProperty::AlphaFunction::kSrcAlpha &&
							 alphaProperty->GetDestBlendMode() == RE::NiAlphaProperty::AlphaFunction::kInvSrcAlpha);
	bool ignored = !blendsOver;
	for (const char* key : { "drop", "drip", "smallsplatter", "blend", "add" })
		ignored = ignored || pathLower.find(key) != std::string::npos;
	if (bloodPathsLogged.size() < 64 && bloodPathsLogged.insert(pathLower).second)
		logger::info("[SNOW DEFORMATION] blood mark '{}' tex='{}' skinned={} blendsOver={} ignored={}",
			geometry->name.c_str() ? geometry->name.c_str() : "", diffusePath, a_skinned ? 1 : 0, blendsOver ? 1 : 0, ignored ? 1 : 0);
	if (ignored)
		return;
	float threshold = -1.0f;
	if (auto* alpha = runtime.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		threshold = float(alpha->alphaThreshold) / 255.0f;
	BloodCapture capture{};
	capture.geometry = RE::NiPointer<RE::BSGeometry>(geometry);
	capture.world = geometry->world;
	capture.diffuse.copy_from(texture->rendererTexture->resourceView);
	capture.texcoord = { material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	capture.alpha = property->alpha * material->materialAlpha;
	capture.alphaThreshold = threshold;
	capture.reveal = 1.0f;
	capture.skinned = a_skinned;
	RegisterDecalOverlay(capture);
	if (!a_skinned) {
		auto& seen = bloodSeen[geometry];
		const auto& position = geometry->world.translate;
		const bool same = seen.frame != 0 && seen.vb == vb && seen.position.GetSquaredDistance(position) < 1.0f;
		const uint32_t frame = globals::state->frameCount;
		seen.frame = frame;
		seen.vb = vb;
		seen.position = position;
		if (!same) {
			seen.firstSeconds = bloodRenderSeconds;
			seen.firstClock = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
			seen.fineEpoch = 0;
			seen.fineMissing = false;
			seen.fineRetryFrame = 0;
			seen.boundsKnown = false;
		}
		capture.clock = seen.firstClock;
		const float spread = std::max(settings.BloodSpreadSeconds, 0.0f);
		const double age = bloodRenderSeconds - seen.firstSeconds;
		const bool revealing = !(same && age > spread + 0.1);
		capture.reveal = spread > 0.0f ? std::clamp(float(age / spread), 0.05f, 1.0f) : 1.0f;
		const bool buried = bloodBurialClock - seen.firstClock.x >= std::max(settings.BloodBurial, 0.01f);
		if (BloodTilesWanted() && !buried && bloodFineQueue.size() < 512 &&
			(revealing || ((seen.fineEpoch != bloodTileEpoch || seen.fineMissing) && frame >= seen.fineRetryFrame)))
			bloodFineQueue.push_back({ capture, geometry, revealing });
		if (!revealing)
			return;
	} else if (BloodTilesWanted() && bloodFineQueue.size() < 512) {
		auto& seen = bloodSeen[geometry];
		const uint32_t frame = globals::state->frameCount;
		if (seen.frame == 0 || frame - seen.frame > 600) {
			seen.firstSeconds = bloodRenderSeconds;
			seen.firstClock = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
			seen.fineEpoch = 0;
		}
		seen.frame = frame;
		seen.position = geometry->world.translate;
		capture.clock = seen.firstClock;
		const bool growing = bloodRenderSeconds - seen.firstSeconds < 60.0;
		if ((growing && ((frame + uint32_t(reinterpret_cast<uintptr_t>(geometry) >> 6)) & 7u) == 0) ||
			(!growing && (seen.fineEpoch != bloodTileEpoch || seen.fineMissing) && frame >= seen.fineRetryFrame))
			bloodFineQueue.push_back({ capture, geometry, growing });
	}
	if (bloodCaptures.size() < 512)
		bloodCaptures.push_back(std::move(capture));
}

bool SnowDeformation::CaptureRuneDraw(RE::BSRenderPass* a_pass, bool a_engineDecal)
{
	auto* geometry = a_pass->geometry;
	auto* property = a_pass->shaderProperty;
	auto* material = property ? static_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
	if (!geometry || !material)
		return false;
	const char* diffusePath = nullptr;
	if (auto textureSet = material->textureSet.get())
		diffusePath = textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
	if (!diffusePath || !*diffusePath)
		return false;
	std::string pathLower(diffusePath);
	std::transform(pathLower.begin(), pathLower.end(), pathLower.begin(), [](unsigned char c) { return c == '/' ? '\\' : char(std::tolower(c)); });
	bool matched = false;
	if (settings.RuneDecalsOnSnow && runeSitesLive.load(std::memory_order_acquire) != 0) {
		{
			std::scoped_lock lock(runeLock);
			for (const auto& site : runeSites)
				matched = matched || pathLower.find(site.decalPath) != std::string::npos;
		}
		if (runeDecalPathsLogged.size() < 32 && runeDecalPathsLogged.insert(pathLower).second)
			logger::info("[SNOW DEFORMATION] rune watch: decal draw '{}' tex='{}' rune={}",
				geometry->name.c_str() ? geometry->name.c_str() : "", diffusePath, matched ? 1 : 0);
	}
	const bool frost = !matched && a_engineDecal && settings.FrostDecalsOnSnow &&
	                   pathLower.find("impactdecals\\") != std::string::npos && pathLower.find("frost") != std::string::npos &&
	                   geometry->GetGeometryRuntimeData().skinInstance == nullptr;
	if (!matched && !frost)
		return false;
	auto& captures = frost ? frostCaptures : runeCaptures;
	if (!runeCaptureSet.insert(geometry).second || (!frost && captures.size() >= 64))
		return true;
	auto* texture = material->diffuseTexture.get();
	if (!texture || !texture->rendererTexture || !texture->rendererTexture->resourceView)
		return true;
	auto& runtime = geometry->GetGeometryRuntimeData();
	float threshold = -1.0f;
	if (auto* alpha = runtime.alphaProperty.get(); alpha && alpha->GetAlphaTesting())
		threshold = float(alpha->alphaThreshold) / 255.0f;
	RuneCapture capture{};
	capture.draw.geometry = RE::NiPointer<RE::BSGeometry>(geometry);
	capture.draw.world = geometry->world;
	capture.draw.diffuse.copy_from(texture->rendererTexture->resourceView);
	capture.draw.texcoord = { material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	capture.draw.alpha = property->alpha * material->materialAlpha;
	capture.draw.alphaThreshold = threshold;
	capture.draw.reveal = 1.0f;
	capture.draw.skinned = false;
	if (auto* normal = material->normalTexture.get(); normal && normal->rendererTexture && normal->rendererTexture->resourceView)
		capture.normal.copy_from(normal->rendererTexture->resourceView);
	const auto& linearLighting = globals::features::linearLighting;
	if (auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property)) {
		const float mult = lighting->emissiveMult;
		capture.emissiveMult = mult;
		if (lighting->emissiveColor) {
			const RE::NiColor color = linearLighting.SRGBToWorking(*lighting->emissiveColor);
			const float scale = linearLighting.IsLinearLightingActive() ? mult * linearLighting.settings.emitColorMult : mult;
			capture.emissive = { color.red * scale, color.green * scale, color.blue * scale };
		}
	}
	if (material->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap) {
		capture.glowMap = true;
		auto* glow = static_cast<RE::BSLightingShaderMaterialGlowmap*>(material)->glowTexture.get();
		if (glow && glow->rendererTexture && glow->rendererTexture->resourceView)
			capture.glow.copy_from(glow->rendererTexture->resourceView);
	}
	if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSpecular)) {
		const float scale = material->specularColorScale;
		const RE::NiColor color = linearLighting.SRGBToWorking(material->specularColor);
		capture.specular = { color.red * scale, color.green * scale, color.blue * scale, material->specularPower };
	}
	capture.centre = geometry->worldBound.center;
	capture.radius = geometry->worldBound.radius;
	captures.push_back(std::move(capture));
	return true;
}

bool SnowDeformation::EnsureDecalMask(uint32_t a_width, uint32_t a_height)
{
	if (decalMaskFailed || !a_width || !a_height)
		return false;
	auto* device = globals::d3d::device;
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl";
	if (!decalMaskVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "MASK"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &decalMaskVS)))
			Util::SetResourceName(decalMaskVS, "SnowDeformation::DecalMaskVS");
	}
	if (!decalMaskSkinVS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "vs_5_0", "VSHADER", "SKINNED", "MASK"));
		if (blob && SUCCEEDED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &decalMaskSkinVS)))
			Util::SetResourceName(decalMaskSkinVS, "SnowDeformation::DecalMaskSkinVS");
	}
	if (!decalMaskPS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(path, "ps_5_0", "PSHADER", "MASK"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &decalMaskPS)))
			Util::SetResourceName(decalMaskPS, "SnowDeformation::DecalMaskPS");
	}
	if (!decalMaskBlendState) {
		D3D11_BLEND_DESC blendDesc{};
		auto& rt = blendDesc.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;
		rt.DestBlend = D3D11_BLEND_INV_SRC_COLOR;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;
		rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
		device->CreateBlendState(&blendDesc, decalMaskBlendState.put());
	}
	if (!decalMaskVS || !decalMaskSkinVS || !decalMaskPS || !decalMaskBlendState) {
		logger::warn("[SNOW DEFORMATION] decal mask disabled (shader or blend state failed): spell marks stay under object snow, the overlay is not gated by its decals");
		decalMaskFailed = true;
		return false;
	}
	if (!decalMaskClearViewChecked) {
		decalMaskClearViewChecked = true;
		D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
		if (SUCCEEDED(globals::d3d::context->QueryInterface(__uuidof(ID3D11DeviceContext1), decalMaskContext1.put_void())) && decalMaskContext1 &&
			SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
			decalMaskClearView = options.ClearView != FALSE;
		logger::info("[SNOW DEFORMATION] decal mask: {}", decalMaskClearView ? "cleared over the decals' rectangle (ClearView)" : "cleared whole (no native ClearView)");
	}
	if (decalMaskTexture && (decalMaskTexture->desc.Width != a_width || decalMaskTexture->desc.Height != a_height)) {
		delete decalMaskTexture;
		decalMaskTexture = nullptr;
	}
	if (!decalMaskTexture) {
		try {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			decalMaskTexture = new Texture2D(desc, "SnowDeformation::DecalMask");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{
				.Format = desc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_RENDER_TARGET_VIEW_DESC rtv{
				.Format = desc.Format,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			decalMaskTexture->CreateSRV(srv);
			decalMaskTexture->CreateRTV(rtv);
		} catch (const std::exception& e) {
			logger::error("[SNOW DEFORMATION] decal mask creation failed: {}", e.what());
			decalMaskFailed = true;
			return false;
		}
	}
	return true;
}

bool SnowDeformation::EnsureRuneResources()
{
	if (runeResourcesFailed)
		return false;
	if (runeAtlasTexture && runeCB && runePS && runeDepthDSV && runeDepthState)
		return true;
	auto* device = globals::d3d::device;
	try {
		if (!runeAtlasTexture) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kRuneTileDim * 2;
			desc.Height = kRuneTileDim * 2;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			runeAtlasTexture = new Texture2D(desc, "SnowDeformation::RuneAtlas");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{
				.Format = DXGI_FORMAT_R16G16B16A16_UNORM,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
			};
			D3D11_RENDER_TARGET_VIEW_DESC rtv{
				.Format = DXGI_FORMAT_R16G16B16A16_UNORM,
				.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = 0 }
			};
			runeAtlasTexture->CreateSRV(srv);
			runeAtlasTexture->CreateRTV(rtv);
		}
		if (!runeCB)
			runeCB = new ConstantBuffer(ConstantBufferDesc<RuneCB>(), "SnowDeformation::RuneCB");
		if (!runeDepthDSV) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kRuneTileDim * 2;
			desc.Height = kRuneTileDim * 2;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_D16_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, runeDepthTexture.put()));
			Util::SetResourceName(runeDepthTexture.get(), "SnowDeformation::RuneAtlasDepth");
			DX::ThrowIfFailed(device->CreateDepthStencilView(runeDepthTexture.get(), nullptr, runeDepthDSV.put()));
			Util::SetResourceName(runeDepthDSV.get(), "SnowDeformation::RuneAtlasDepth DSV");
		}
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] rune atlas creation failed: {} - rune glyphs on snow are off", e.what());
		runeResourcesFailed = true;
		return false;
	}
	if (!runePS) {
		winrt::com_ptr<ID3DBlob> blob;
		blob.attach(SD_CompileShaderBlob(L"Data\\Shaders\\SnowDeformation\\SnowBloodCapture.hlsl", "ps_5_0", "PSHADER", "RUNE"));
		if (blob && SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &runePS)))
			Util::SetResourceName(runePS, "SnowDeformation::RuneCapturePS");
	}
	if (!runeDepthState) {
		D3D11_DEPTH_STENCIL_DESC depthDesc{};
		depthDesc.DepthEnable = TRUE;
		depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
		device->CreateDepthStencilState(&depthDesc, runeDepthState.put());
	}
	if (!runePS || !runeDepthState) {
		logger::warn("[SNOW DEFORMATION] rune glyphs on snow disabled (shader or blend state failed)");
		runeResourcesFailed = true;
		return false;
	}
	return true;
}

namespace
{
	constexpr float kFrostFlatNormalZ = 0.5f;
	constexpr float kFrostFitTolerance = 4.0f;
	constexpr float kFrostTwinDistance = 12.0f;
	constexpr float kFrostTwinSlope = 0.15f;
	constexpr uint32_t kFrostMarkRows = 3;

	struct FrostFit
	{
		double centreX = 0.0;
		double centreY = 0.0;
		double slope[4] = {};
		float reachX = 0.0f;
		float reachY = 0.0f;
		uint32_t quarter = 4;
		uint32_t slot = 0;
		float area = 0.0f;
		float alpha = 0.0f;
		float zLow = 0.0f;
		float zHigh = 0.0f;
		const RE::BSGeometry* geometry = nullptr;
	};

	bool ReadDecalMesh(RE::BSGeometry* a_geometry, const RE::NiTransform& a_world, std::vector<RE::NiPoint3>& a_points, std::vector<float2>& a_uvs,
		const uint16_t*& a_indices, uint32_t& a_triangles)
	{
		auto* triShape = a_geometry ? a_geometry->AsTriShape() : nullptr;
		auto* rendererData = triShape ? a_geometry->GetGeometryRuntimeData().rendererData : nullptr;
		if (!rendererData || !rendererData->rawVertexData || !rendererData->rawIndexData)
			return false;
		const auto desc = rendererData->vertexDesc;
		uint64_t descKey;
		memcpy(&descKey, &desc, sizeof(descKey));
		const uint32_t stride = uint32_t(descKey & 0xF) * 4;
		const uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
		const uint32_t triangleCount = triShape->GetTrishapeRuntimeData().triangleCount;
		if (!stride || !vertexCount || !triangleCount || !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV))
			return false;
		const bool full = SD_PositionBytes(descKey, desc) >= 16;
		const uint32_t uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
		a_points.resize(vertexCount);
		a_uvs.resize(vertexCount);
		for (uint32_t v = 0; v < vertexCount; ++v) {
			const uint8_t* base = rendererData->rawVertexData + size_t(v) * stride;
			RE::NiPoint3 p;
			if (full) {
				float f[3];
				memcpy(f, base, sizeof(f));
				p = { f[0], f[1], f[2] };
			} else {
				uint16_t h[3];
				memcpy(h, base, sizeof(h));
				p = { DirectX::PackedVector::XMConvertHalfToFloat(h[0]), DirectX::PackedVector::XMConvertHalfToFloat(h[1]), DirectX::PackedVector::XMConvertHalfToFloat(h[2]) };
			}
			a_points[v] = a_world * p;
			uint16_t t[2];
			memcpy(t, base + uvOffset, sizeof(t));
			a_uvs[v] = { DirectX::PackedVector::XMConvertHalfToFloat(t[0]), DirectX::PackedVector::XMConvertHalfToFloat(t[1]) };
		}
		a_indices = rendererData->rawIndexData;
		a_triangles = triangleCount;
		return true;
	}

	void WidenDecalSpan(RE::BSGeometry* a_geometry, const RE::NiTransform& a_world, float& a_low, float& a_high)
	{
		thread_local std::vector<RE::NiPoint3> points;
		thread_local std::vector<float2> uvs;
		const uint16_t* indices = nullptr;
		uint32_t triangles = 0;
		if (ReadDecalMesh(a_geometry, a_world, points, uvs, indices, triangles)) {
			for (const auto& p : points) {
				a_low = std::min(a_low, p.z);
				a_high = std::max(a_high, p.z);
			}
		} else if (a_geometry) {
			const auto& bound = a_geometry->worldBound;
			a_low = std::min(a_low, bound.center.z - bound.radius);
			a_high = std::max(a_high, bound.center.z + bound.radius);
		}
	}

	bool FitFrostMark(RE::BSGeometry* a_geometry, const RE::NiTransform& a_world, FrostFit& a_fit)
	{
		thread_local std::vector<RE::NiPoint3> points;
		thread_local std::vector<float2> uvs;
		const uint16_t* indices = nullptr;
		uint32_t triangleCount = 0;
		if (!ReadDecalMesh(a_geometry, a_world, points, uvs, indices, triangleCount))
			return false;
		const uint32_t vertexCount = uint32_t(points.size());
		a_fit.zLow = FLT_MAX;
		a_fit.zHigh = -FLT_MAX;
		for (const auto& p : points) {
			a_fit.zLow = std::min(a_fit.zLow, p.z);
			a_fit.zHigh = std::max(a_fit.zHigh, p.z);
		}
		const double ox = points[0].x, oy = points[0].y;
		double s = 0.0, sx = 0.0, sy = 0.0, su = 0.0, sv = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
		double suu = 0.0, svv = 0.0, sxu = 0.0, syu = 0.0, sxv = 0.0, syv = 0.0;
		float2 uvLo{ FLT_MAX, FLT_MAX }, uvHi{ -FLT_MAX, -FLT_MAX };
		for (uint32_t t = 0; t < triangleCount; ++t) {
			const uint32_t corner[3] = { indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2] };
			if (corner[0] >= vertexCount || corner[1] >= vertexCount || corner[2] >= vertexCount)
				return false;
			const RE::NiPoint3 n = (points[corner[1]] - points[corner[0]]).Cross(points[corner[2]] - points[corner[0]]);
			const float length = n.Length();
			if (length < 1e-4f || std::abs(n.z) < kFrostFlatNormalZ * length)
				continue;
			const double w = std::abs(n.z) / 6.0;
			for (const uint32_t k : corner) {
				const double x = points[k].x - ox, y = points[k].y - oy, u = uvs[k].x, v = uvs[k].y;
				s += w;
				sx += w * x;
				sy += w * y;
				su += w * u;
				sv += w * v;
				sxx += w * x * x;
				syy += w * y * y;
				sxy += w * x * y;
				suu += w * u * u;
				svv += w * v * v;
				sxu += w * x * u;
				syu += w * y * u;
				sxv += w * x * v;
				syv += w * y * v;
				uvLo = { std::min(uvLo.x, uvs[k].x), std::min(uvLo.y, uvs[k].y) };
				uvHi = { std::max(uvHi.x, uvs[k].x), std::max(uvHi.y, uvs[k].y) };
			}
		}
		if (s < 1.0)
			return false;
		const double mx = sx / s, my = sy / s, mu = su / s, mv = sv / s;
		const double cxx = sxx / s - mx * mx, cyy = syy / s - my * my, cxy = sxy / s - mx * my;
		const double cxu = sxu / s - mx * mu, cyu = syu / s - my * mu, cxv = sxv / s - mx * mv, cyv = syv / s - my * mv;
		const double det = cxx * cyy - cxy * cxy;
		if (cxx <= 0.0 || cyy <= 0.0 || det <= 1e-9 * cxx * cyy)
			return false;
		const double bu = (cxu * cyy - cyu * cxy) / det, cu = (cyu * cxx - cxu * cxy) / det;
		const double bv = (cxv * cyy - cyv * cxy) / det, cv = (cyv * cxx - cxv * cxy) / det;
		const double jacobian = bu * cv - cu * bv;
		const double residual = std::max(suu / s - mu * mu - bu * cxu - cu * cyu, 0.0) + std::max(svv / s - mv * mv - bv * cxv - cv * cyv, 0.0);
		if (std::abs(jacobian) < 1e-12 || residual > double(kFrostFitTolerance * kFrostFitTolerance) * std::abs(jacobian))
			return false;
		const int32_t qx = std::clamp(int32_t(std::floor(uvLo.x + uvHi.x)), 0, 1);
		const int32_t qy = std::clamp(int32_t(std::floor(uvLo.y + uvHi.y)), 0, 1);
		constexpr float eps = 0.02f;
		const bool quartered = uvLo.x >= 0.5f * qx - eps && uvHi.x <= 0.5f * qx + 0.5f + eps && uvLo.y >= 0.5f * qy - eps && uvHi.y <= 0.5f * qy + 0.5f + eps;
		a_fit.quarter = quartered ? uint32_t(qx + 2 * qy) : 4u;
		const double radius = quartered ? 0.25 : 0.5;
		const double du = (quartered ? 0.5 * qx + 0.25 : 0.5) - mu;
		const double dv = (quartered ? 0.5 * qy + 0.25 : 0.5) - mv;
		a_fit.centreX = ox + mx + (cv * du - cu * dv) / jacobian;
		a_fit.centreY = oy + my + (bu * dv - bv * du) / jacobian;
		a_fit.slope[0] = bu;
		a_fit.slope[1] = bv;
		a_fit.slope[2] = cu;
		a_fit.slope[3] = cv;
		a_fit.reachX = float(radius * std::sqrt(cv * cv + cu * cu) / std::abs(jacobian));
		a_fit.reachY = float(radius * std::sqrt(bv * bv + bu * bu) / std::abs(jacobian));
		a_fit.area = float(s);
		return true;
	}

	bool FrostTwins(const FrostFit& a_a, const FrostFit& a_b)
	{
		if (a_a.quarter != a_b.quarter || a_a.slot != a_b.slot)
			return false;
		const double dx = a_a.centreX - a_b.centreX, dy = a_a.centreY - a_b.centreY;
		if (dx * dx + dy * dy > double(kFrostTwinDistance * kFrostTwinDistance))
			return false;
		double scale = 0.0, gap = 0.0;
		for (uint32_t k = 0; k < 4; ++k) {
			scale = std::max(scale, std::abs(a_a.slope[k]));
			gap = std::max(gap, std::abs(a_a.slope[k] - a_b.slope[k]));
		}
		return gap <= double(kFrostTwinSlope) * scale;
	}
}

bool SnowDeformation::EnsureFrostPlanes(uint32_t a_marks, uint32_t a_entries)
{
	if (frostPlanesFailed)
		return false;
	auto* device = globals::d3d::device;
	try {
		if (a_marks > frostPlaneCapacity) {
			const uint32_t capacity = std::max(a_marks + a_marks / 2u, 256u);
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = capacity * kFrostMarkRows * sizeof(float4);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(float4);
			frostPlaneBuffer = nullptr;
			frostPlaneSRV = nullptr;
			DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, frostPlaneBuffer.put()));
			Util::SetResourceName(frostPlaneBuffer.get(), "SnowDeformation::FrostPlanes");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = capacity * kFrostMarkRows;
			DX::ThrowIfFailed(device->CreateShaderResourceView(frostPlaneBuffer.get(), &srv, frostPlaneSRV.put()));
			Util::SetResourceName(frostPlaneSRV.get(), "SnowDeformation::FrostPlanes SRV");
			frostPlaneCapacity = capacity;
		}
		if (!frostCellBuffer) {
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kFrostCells * kFrostCells * 2u * sizeof(uint32_t);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = 2u * sizeof(uint32_t);
			DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, frostCellBuffer.put()));
			Util::SetResourceName(frostCellBuffer.get(), "SnowDeformation::FrostCells");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = kFrostCells * kFrostCells;
			DX::ThrowIfFailed(device->CreateShaderResourceView(frostCellBuffer.get(), &srv, frostCellSRV.put()));
			Util::SetResourceName(frostCellSRV.get(), "SnowDeformation::FrostCells SRV");
		}
		if (a_entries > frostListCapacity) {
			const uint32_t capacity = std::max(a_entries + a_entries / 2u, 1024u);
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = capacity * sizeof(uint32_t);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(uint32_t);
			frostListBuffer = nullptr;
			frostListSRV = nullptr;
			DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, frostListBuffer.put()));
			Util::SetResourceName(frostListBuffer.get(), "SnowDeformation::FrostCellList");
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_UNKNOWN;
			srv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srv.Buffer.NumElements = capacity;
			DX::ThrowIfFailed(device->CreateShaderResourceView(frostListBuffer.get(), &srv, frostListSRV.put()));
			Util::SetResourceName(frostListSRV.get(), "SnowDeformation::FrostCellList SRV");
			frostListCapacity = capacity;
		}
	} catch (const std::exception& e) {
		logger::error("[SNOW DEFORMATION] frost plane buffers failed: {} - frost marks stay off the landscape snow", e.what());
		frostPlanesFailed = true;
		return false;
	}
	return true;
}

void SnowDeformation::BindFrostField(ID3D11DeviceContext* a_context, bool a_bind)
{
	ID3D11ShaderResourceView* srvs[11] = {};
	if (a_bind) {
		srvs[0] = frostPlaneSRV.get();
		for (uint32_t i = 0; i < kFrostTextures; ++i) {
			srvs[1 + i] = frostDiffuse[i].get();
			srvs[1 + kFrostTextures + i] = frostNormal[i].get();
		}
		srvs[9] = frostCellSRV.get();
		srvs[10] = frostListSRV.get();
	}
	a_context->PSSetShaderResources(117, 11, srvs);
	constexpr UINT kGlowSlots[kFrostTextures] = { 97, 99, 111, 64 };
	for (uint32_t i = 0; i < kFrostTextures; ++i) {
		ID3D11ShaderResourceView* glow = a_bind ? frostGlow[i].get() : nullptr;
		a_context->PSSetShaderResources(kGlowSlots[i], 1, &glow);
	}
}

void SnowDeformation::BindMarkReaders(ID3D11DeviceContext* a_context, bool a_bind)
{
	ID3D11Buffer* buffer = (a_bind && runeCB) ? runeCB->CB() : nullptr;
	a_context->PSSetConstantBuffers(7, 1, &buffer);
	ID3D11ShaderResourceView* atlas = a_bind ? GetRuneAtlasSRV() : nullptr;
	a_context->PSSetShaderResources(90, 1, &atlas);
	ID3D11ShaderResourceView* textures[2 * kRuneMaxTiles] = {};
	constexpr UINT kGlowSlots[kRuneMaxTiles] = { 88, 89, 95, 96 };
	for (uint32_t i = 0; i < kRuneMaxTiles; ++i) {
		textures[i] = a_bind ? runeTileDiffuse[i].get() : nullptr;
		textures[kRuneMaxTiles + i] = a_bind ? runeTileNormal[i].get() : nullptr;
		ID3D11ShaderResourceView* glow = a_bind ? runeTileGlow[i].get() : nullptr;
		a_context->PSSetShaderResources(kGlowSlots[i], 1, &glow);
	}
	a_context->PSSetShaderResources(80, 2 * kRuneMaxTiles, textures);
	BindFrostField(a_context, a_bind);
}

void SnowDeformation::RenderRuneCapture()
{
	std::vector<RuneSite> sites;
	{
		std::scoped_lock lock(runeLock);
		sites = runeSites;
	}
	runeStatSites = uint32_t(sites.size());
	runeStatCaptures = uint32_t(runeCaptures.size());
	runeStatTiles = 0;
	markDecals.clear();

	if (!sites.empty() && runeCaptures.empty()) {
		if (++runeMissFrames == 180 && !runeMissLogged) {
			runeMissLogged = true;
			logger::warn("[SNOW DEFORMATION] rune in range for 180 frames and no decal draw matched '{}'", sites.front().decalPath);
		}
	} else {
		runeMissFrames = 0;
	}

	std::vector<const RuneCapture*> frost;
	if (settings.FrostDecalsOnSnow) {
		for (const auto& capture : frostCaptures) {
			float surface = 0.0f;
			if (SampleShellSurface(capture.centre.x, capture.centre.y, surface) && capture.centre.z <= surface + kFrostDecalAboveSnow)
				frost.push_back(&capture);
		}
	}

	auto context = globals::d3d::context;
	const bool runes = settings.RuneDecalsOnSnow && !sites.empty() && !runeCaptures.empty() && EnsureBloodResources() && EnsureRuneResources();
	const bool frosted = !frost.empty() && EnsureBloodResources() && EnsureRuneResources();
	RuneCB cbRune{};
	const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (!runes && runeAtlasDirty && runeAtlasTexture) {
		context->ClearRenderTargetView(runeAtlasTexture->rtv.get(), zero);
		runeAtlasDirty = false;
	}
	if (!runes) {
		for (uint32_t i = 0; i < kRuneMaxTiles; ++i) {
			runeTileDiffuse[i] = nullptr;
			runeTileNormal[i] = nullptr;
			runeTileGlow[i] = nullptr;
		}
	}
	if (!frosted) {
		for (uint32_t i = 0; i < kFrostTextures; ++i) {
			frostDiffuse[i] = nullptr;
			frostNormal[i] = nullptr;
			frostGlow[i] = nullptr;
		}
	}
	if (!runes && !frosted) {
		if (runeCB)
			runeCB->Update(cbRune);
		runeCaptures.clear();
		frostCaptures.clear();
		runeCaptureSet.clear();
		return;
	}

	globals::profiler->BeginPass("SnowDeformation::Decals - Capture");
	winrt::com_ptr<ID3D11BlendState> savedBlend;
	float savedBlendFactor[4] = {};
	UINT savedSampleMask = 0;
	context->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepth;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepth.put(), &savedStencilRef);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(runeDepthState.get(), 0);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	context->RSSetState(bloodRasterState.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11SamplerState* sampler = bloodSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->PSSetShader(runePS, nullptr, 0);

	uint32_t tiles = 0;
	if (runes) {
		context->ClearRenderTargetView(runeAtlasTexture->rtv.get(), zero);
		context->ClearDepthStencilView(runeDepthDSV.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
		ID3D11RenderTargetView* rtvs[1] = { runeAtlasTexture->rtv.get() };
		context->OMSetRenderTargets(1, rtvs, runeDepthDSV.get());
		std::vector<BloodCapture> list;
		std::vector<const RuneCapture*> members;
		for (const auto& site : sites) {
			if (tiles >= kRuneMaxTiles)
				break;
			list.clear();
			members.clear();
			for (const auto& capture : runeCaptures) {
				const float dx = capture.centre.x - site.position.x;
				const float dy = capture.centre.y - site.position.y;
				const float reach = capture.radius + kRuneTileHalf;
				if (dx * dx + dy * dy > reach * reach)
					continue;
				list.push_back(capture.draw);
				list.back().decalId = float(list.size());
				members.push_back(&capture);
			}
			if (list.empty())
				continue;
			const RuneCapture* first = members.front();
			const float tileX = float((tiles & 1u) * kRuneTileDim);
			const float tileY = float((tiles >> 1u) * kRuneTileDim);
			D3D11_VIEWPORT viewport{ tileX, tileY, float(kRuneTileDim), float(kRuneTileDim), 0.0f, 1.0f };
			context->RSSetViewports(1, &viewport);
			BloodCB cb{};
			cb.WindowOrigin = { site.position.x - kRuneTileHalf, site.position.y - kRuneTileHalf };
			cb.TexelSize = 2.0f * kRuneTileHalf / float(kRuneTileDim);
			cb.MapDim = float(kRuneTileDim);
			cb.MapOrigin = { 0, 0 };
			cb.Intensity = 1.0f;
			cb.NormalZMin = 0.3f;
			if (DrawBloodList(context, list, cb) == 0)
				continue;
			float low = FLT_MAX, high = -FLT_MAX;
			for (const RuneCapture* member : members) {
				WidenDecalSpan(member->draw.geometry.get(), member->draw.world, low, high);
				markDecals.insert(member->draw.geometry.get());
			}
			cbRune.RuneRects[tiles] = { cb.WindowOrigin.x, cb.WindowOrigin.y, 1.0f / (2.0f * kRuneTileHalf), first->glowMap ? 1.0f : 0.0f };
			cbRune.RuneTints[tiles] = { first->emissive.x, first->emissive.y, first->emissive.z, first->normal ? 1.0f : 0.0f };
			cbRune.RuneSpecular[tiles] = first->specular;
			cbRune.RuneReach[tiles] = { low, high, first->emissiveMult, 0.0f };
			runeTileDiffuse[tiles] = first->draw.diffuse;
			runeTileNormal[tiles] = first->normal;
			runeTileGlow[tiles] = first->glow;
			tiles++;
		}
		for (uint32_t i = tiles; i < kRuneMaxTiles; ++i) {
			runeTileDiffuse[i] = nullptr;
			runeTileNormal[i] = nullptr;
			runeTileGlow[i] = nullptr;
		}
		runeAtlasDirty = true;
		if (tiles && !runePaintLogged) {
			runePaintLogged = true;
			logger::info("[SNOW DEFORMATION] rune atlas: first frame painted {} tile(s) from {} decal draw(s), {} rune(s) in range",
				tiles, runeCaptures.size(), sites.size());
		}
	}

	if (frosted) {
		RE::NiPoint3 centre = frost.front()->centre;
		if (auto* player = globals::game::player)
			centre = player->GetPosition();
		const float half = 0.5f * kFrostCellSize * float(kFrostCells);
		const float2 origin{ std::floor((centre.x - half) / kFrostCellSize) * kFrostCellSize, std::floor((centre.y - half) / kFrostCellSize) * kFrostCellSize };
		uint32_t slots = 0;
		uint32_t unfitted = 0;
		const RuneCapture* slotMaterial[kFrostTextures] = {};
		auto sameMaterial = [](const RuneCapture& a_a, const RuneCapture& a_b) {
			return a_a.draw.diffuse.get() == a_b.draw.diffuse.get() && a_a.normal.get() == a_b.normal.get() && a_a.glow.get() == a_b.glow.get() &&
			       a_a.glowMap == a_b.glowMap && a_a.emissiveMult == a_b.emissiveMult && a_a.emissive.x == a_b.emissive.x && a_a.emissive.y == a_b.emissive.y &&
			       a_a.emissive.z == a_b.emissive.z && a_a.specular.x == a_b.specular.x && a_a.specular.y == a_b.specular.y && a_a.specular.z == a_b.specular.z &&
			       a_a.specular.w == a_b.specular.w;
		};
		std::vector<FrostFit> fits;
		fits.reserve(frost.size());
		for (const RuneCapture* mark : frost) {
			FrostFit fit;
			if (!FitFrostMark(mark->draw.geometry.get(), mark->draw.world, fit)) {
				unfitted++;
				continue;
			}
			uint32_t slot = 0;
			while (slot < slots && !sameMaterial(*slotMaterial[slot], *mark))
				slot++;
			if (slot == slots) {
				if (slots >= kFrostTextures)
					continue;
				slotMaterial[slot] = mark;
				frostDiffuse[slot] = mark->draw.diffuse;
				frostNormal[slot] = mark->normal;
				frostGlow[slot] = mark->glow;
				slots++;
			}
			fit.slot = slot;
			fit.alpha = std::clamp(mark->draw.alpha, 0.0f, 1.0f);
			fit.geometry = mark->draw.geometry.get();
			fits.push_back(fit);
		}
		for (uint32_t i = slots; i < kFrostTextures; ++i) {
			frostDiffuse[i] = nullptr;
			frostNormal[i] = nullptr;
			frostGlow[i] = nullptr;
		}
		std::vector<uint32_t> byX(fits.size());
		for (uint32_t i = 0; i < byX.size(); ++i)
			byX[i] = i;
		std::sort(byX.begin(), byX.end(), [&](uint32_t a_l, uint32_t a_r) { return fits[a_l].centreX < fits[a_r].centreX; });
		std::vector<uint8_t> dropped(fits.size(), 0);
		uint32_t twins = 0;
		for (uint32_t a = 0; a < byX.size(); ++a)
			for (uint32_t b = a + 1; b < byX.size() && fits[byX[b]].centreX - fits[byX[a]].centreX <= kFrostTwinDistance; ++b) {
				const uint32_t i = byX[a], j = byX[b];
				if (dropped[i] || dropped[j] || !FrostTwins(fits[i], fits[j]))
					continue;
				const uint32_t lost = fits[i].area < fits[j].area ? i : j;
				FrostFit& kept = fits[lost == i ? j : i];
				kept.zLow = std::min(kept.zLow, fits[lost].zLow);
				kept.zHigh = std::max(kept.zHigh, fits[lost].zHigh);
				dropped[lost] = 1;
				twins++;
			}
		frostMarkScratch.clear();
		std::vector<float4> reach;
		reach.reserve(fits.size());
		for (uint32_t i = 0; i < fits.size(); ++i) {
			if (dropped[i])
				continue;
			const FrostFit& fit = fits[i];
			const double cu = fit.quarter < 4u ? 0.5 * (fit.quarter & 1u) + 0.25 : 0.5;
			const double cv = fit.quarter < 4u ? 0.5 * (fit.quarter >> 1u) + 0.25 : 0.5;
			const double rx = double(origin.x) - fit.centreX, ry = double(origin.y) - fit.centreY;
			frostMarkScratch.push_back({ float(cu + fit.slope[0] * rx + fit.slope[2] * ry), float(cv + fit.slope[1] * rx + fit.slope[3] * ry), float(fit.slot + 4u * fit.quarter), fit.alpha });
			frostMarkScratch.push_back({ float(fit.slope[0]), float(fit.slope[1]), float(fit.slope[2]), float(fit.slope[3]) });
			frostMarkScratch.push_back({ fit.zLow, fit.zHigh, 0.0f, 0.0f });
			reach.push_back({ float(fit.centreX) - fit.reachX, float(fit.centreY) - fit.reachY, float(fit.centreX) + fit.reachX, float(fit.centreY) + fit.reachY });
		}
		const uint32_t marks = uint32_t(reach.size());
		if (!marks && unfitted && !frostFitMissLogged) {
			frostFitMissLogged = true;
			logger::warn("[SNOW DEFORMATION] frost marks: none of {} frost decal draw(s) could be fitted (not flat on the ground, or the engine kept no CPU vertex copy) - they stay off the landscape snow", unfitted);
		}
		const float inv = 1.0f / kFrostCellSize;
		auto cellRange = [&](uint32_t a_mark, int32_t& a_x0, int32_t& a_y0, int32_t& a_x1, int32_t& a_y1) {
			const float4& bounds = reach[a_mark];
			a_x0 = std::max(int32_t(std::floor((bounds.x - origin.x) * inv)), 0);
			a_y0 = std::max(int32_t(std::floor((bounds.y - origin.y) * inv)), 0);
			a_x1 = std::min(int32_t(std::floor((bounds.z - origin.x) * inv)), int32_t(kFrostCells) - 1);
			a_y1 = std::min(int32_t(std::floor((bounds.w - origin.y) * inv)), int32_t(kFrostCells) - 1);
		};
		frostCellCounts.assign(kFrostCells * kFrostCells, 0u);
		uint32_t entries = 0;
		for (uint32_t i = 0; i < marks; ++i) {
			int32_t x0, y0, x1, y1;
			cellRange(i, x0, y0, x1, y1);
			for (int32_t y = y0; y <= y1; ++y)
				for (int32_t x = x0; x <= x1; ++x, ++entries)
					frostCellCounts[y * kFrostCells + x]++;
		}
		if (marks && EnsureFrostPlanes(marks, std::max(entries, 1u))) {
			std::vector<uint32_t> cells(kFrostCells * kFrostCells * 2u);
			uint32_t offset = 0;
			for (uint32_t c = 0; c < kFrostCells * kFrostCells; ++c) {
				cells[c * 2u] = offset;
				cells[c * 2u + 1u] = 0u;
				offset += frostCellCounts[c];
			}
			frostListScratch.assign(std::max(entries, 1u), 0u);
			for (uint32_t i = 0; i < marks; ++i) {
				int32_t x0, y0, x1, y1;
				cellRange(i, x0, y0, x1, y1);
				for (int32_t y = y0; y <= y1; ++y)
					for (int32_t x = x0; x <= x1; ++x) {
						const uint32_t c = uint32_t(y) * kFrostCells + uint32_t(x);
						frostListScratch[cells[c * 2u] + cells[c * 2u + 1u]++] = i;
					}
			}
			bool uploaded = true;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(frostPlaneBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, frostMarkScratch.data(), frostMarkScratch.size() * sizeof(float4));
				context->Unmap(frostPlaneBuffer.get(), 0);
			} else {
				uploaded = false;
			}
			if (SUCCEEDED(context->Map(frostCellBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, cells.data(), cells.size() * sizeof(uint32_t));
				context->Unmap(frostCellBuffer.get(), 0);
			} else {
				uploaded = false;
			}
			if (SUCCEEDED(context->Map(frostListBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, frostListScratch.data(), frostListScratch.size() * sizeof(uint32_t));
				context->Unmap(frostListBuffer.get(), 0);
			} else {
				uploaded = false;
			}
			cbRune.FrostRect = { origin.x, origin.y, inv, uploaded ? 1.0f : 0.0f };
			float hasNormal[kFrostTextures] = {};
			float emissiveMults[kFrostTextures] = {};
			for (uint32_t i = 0; i < slots; ++i) {
				const RuneCapture& material = *slotMaterial[i];
				hasNormal[i] = material.normal ? 1.0f : 0.0f;
				emissiveMults[i] = material.emissiveMult;
				cbRune.FrostSpecular[i] = material.specular;
				cbRune.FrostTints[i] = { material.emissive.x, material.emissive.y, material.emissive.z, material.glowMap ? 1.0f : 0.0f };
			}
			cbRune.FrostNormals = { hasNormal[0], hasNormal[1], hasNormal[2], hasNormal[3] };
			cbRune.FrostEmissiveMults = { emissiveMults[0], emissiveMults[1], emissiveMults[2], emissiveMults[3] };
			const float windowSize = kFrostCellSize * float(kFrostCells);
			if (uploaded)
				for (const FrostFit& fit : fits)
					if (float(fit.centreX) + fit.reachX >= origin.x && float(fit.centreX) - fit.reachX < origin.x + windowSize &&
						float(fit.centreY) + fit.reachY >= origin.y && float(fit.centreY) - fit.reachY < origin.y + windowSize)
						markDecals.insert(fit.geometry);
			if (uploaded && !frostPaintLogged) {
				frostPaintLogged = true;
				logger::info("[SNOW DEFORMATION] frost marks: first frame {} mark(s) from {} frost decal draw(s): {} twin(s) merged, {} not flat on the ground or without a CPU vertex copy; {} texture(s), {} cell entries",
					marks, frost.size(), twins, unfitted, slots, entries);
			}
		}
	}
	cbRune.RuneParams = { float(tiles), 0.0f, 0.0f, 0.0f };
	runeCB->Update(cbRune);
	runeStatTiles = tiles;

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSRV);
	ID3D11RenderTargetView* nullRTV = nullptr;
	context->OMSetRenderTargets(1, &nullRTV, nullptr);
	context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
	context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
	runeCaptures.clear();
	frostCaptures.clear();
	runeCaptureSet.clear();
}

bool SnowDeformation::BloodScreenRect(const RE::BSGeometry* a_geometry, RECT& a_rect) const
{
	const auto& fb = globals::game::frameBufferCached;
	const auto& m = fb.GetCameraViewProj();
	const auto adjust = fb.GetCameraPosAdjust();
	const auto& bound = a_geometry->worldBound;
	const float r = std::max(bound.radius, 1.0f);
	const float cx = bound.center.x - adjust.x, cy = bound.center.y - adjust.y, cz = bound.center.z - adjust.z;
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	for (int c = 0; c < 8; ++c) {
		const float x = cx + ((c & 1) ? r : -r), y = cy + ((c & 2) ? r : -r), z = cz + ((c & 4) ? r : -r);
		const float w = m.m[3][0] * x + m.m[3][1] * y + m.m[3][2] * z + m.m[3][3];
		if (w < 0.1f) {
			a_rect = { 0, 0, LONG(bloodViewport.Width), LONG(bloodViewport.Height) };
			return a_rect.right > 0 && a_rect.bottom > 0;
		}
		const float nx = (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3]) / w;
		const float ny = (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3]) / w;
		minX = std::min(minX, nx);
		maxX = std::max(maxX, nx);
		minY = std::min(minY, ny);
		maxY = std::max(maxY, ny);
	}
	const float W = bloodViewport.Width, H = bloodViewport.Height;
	if (W < 1.0f || H < 1.0f)
		return false;
	a_rect.left = LONG(std::floor((minX * 0.5f + 0.5f) * W)) - 2;
	a_rect.right = LONG(std::ceil((maxX * 0.5f + 0.5f) * W)) + 2;
	a_rect.top = LONG(std::floor((0.5f - maxY * 0.5f) * H)) - 2;
	a_rect.bottom = LONG(std::ceil((0.5f - minY * 0.5f) * H)) + 2;
	a_rect.left = std::clamp(a_rect.left, 0L, LONG(W));
	a_rect.right = std::clamp(a_rect.right, 0L, LONG(W));
	a_rect.top = std::clamp(a_rect.top, 0L, LONG(H));
	a_rect.bottom = std::clamp(a_rect.bottom, 0L, LONG(H));
	return a_rect.right > a_rect.left && a_rect.bottom > a_rect.top;
}

void SnowDeformation::CopyBloodTargets(bool a_preDecal)
{
	auto* context = globals::d3d::context;
	auto& rtData = globals::game::renderer->GetRuntimeData();
	static constexpr uint32_t kSources[kBloodCopyCount] = { uint32_t(RE::RENDER_TARGETS::kMAIN), uint32_t(NORMALROUGHNESS), uint32_t(ALBEDO), uint32_t(SPECULAR), uint32_t(REFLECTANCE) };
	static constexpr const char* kNames[2][kBloodCopyCount] = {
		{ "SnowDeformation::BloodPreSnow0", "SnowDeformation::BloodPreSnow2", "SnowDeformation::BloodPreSnow3", "SnowDeformation::BloodPreSnow4", "SnowDeformation::BloodPreSnow5" },
		{ "SnowDeformation::BloodPreDecal0", "SnowDeformation::BloodPreDecal2", "SnowDeformation::BloodPreDecal3", "SnowDeformation::BloodPreDecal4", "SnowDeformation::BloodPreDecal5" }
	};
	bool& valid = a_preDecal ? bloodPreDecalValid : bloodPreSnowValid;
	valid = false;
	auto* texs = a_preDecal ? bloodPreDecalTex : bloodPreSnowTex;
	auto* srvs = a_preDecal ? bloodPreDecalSRV : bloodPreSnowSRV;
	const RECT& rc = bloodCopyRect;
	if (rc.right <= rc.left || rc.bottom <= rc.top)
		return;
	const D3D11_BOX box{ UINT(rc.left), UINT(rc.top), 0, UINT(rc.right), UINT(rc.bottom), 1 };
	const D3D11_BOX extent = ScreenRectBox(&bloodViewport, UINT32_MAX, UINT32_MAX, 0);
	ID3D11RenderTargetView* rtvs[8]{};
	ID3D11DepthStencilView* dsv = nullptr;
	context->OMGetRenderTargets(8, rtvs, &dsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	bool ok = true;
	for (uint32_t i = 0; i < kBloodCopyCount; ++i) {
		auto* srv = rtData.renderTargets[kSources[i]].SRV;
		if (!srv) {
			ok = false;
			break;
		}
		CopySRVResource(srv, kNames[a_preDecal ? 1 : 0][i], texs[i], srvs[i], &box, &extent);
		ok = ok && srvs[i];
	}
	if (a_preDecal && ok) {
		auto* masksSRV = rtData.renderTargets[MASKS].SRV;
		if (masksSRV)
			CopySRVResource(masksSRV, "SnowDeformation::BloodPreDecalMasks", bloodPreDecalMasksTex, bloodPreDecalMasksSRV, &box, &extent);
		ok = masksSRV && bloodPreDecalMasksSRV;
	}
	context->OMSetRenderTargets(8, rtvs, dsv);
	for (auto* rtv : rtvs)
		if (rtv)
			rtv->Release();
	if (dsv)
		dsv->Release();
	valid = ok;
}


void SnowDeformation::APIDepositBlood(float a_x, float a_y, float a_z, float a_radius, float a_r, float a_g, float a_b, float a_amount)
{
	if (!APIIsActive() || !settings.BloodOnSnow || !(a_radius > 0.0f) || !(a_amount > 0.0f))
		return;
	std::scoped_lock lock(bloodDiscMutex);
	if (bloodDiscQueue.size() >= kBloodMaxDiscs)
		return;
	bloodDiscQueue.push_back({ a_x, a_y, a_z, a_radius, std::clamp(a_r, 0.0f, 1.0f), std::clamp(a_g, 0.0f, 1.0f), std::clamp(a_b, 0.0f, 1.0f), std::clamp(a_amount, 0.0f, 1.0f) });
}

uint32_t SnowDeformation::DrawBloodList(ID3D11DeviceContext* a_context, const std::vector<BloodCapture>& a_list, BloodCB& a_cb, ID3D11VertexShader* a_rigidVS, UINT a_instances, bool a_ownClock, ID3D11VertexShader* a_skinnedVS)
{
	auto* context = a_context;
	const UINT instances = a_instances;
	uint32_t drawn = 0;

	auto rowsFrom = [](const RE::NiTransform& a_x, BloodCB& a_out) {
		const auto& r = a_x.rotate;
		const float sc = a_x.scale;
		a_out.WorldRow0 = { r.entry[0][0] * sc, r.entry[0][1] * sc, r.entry[0][2] * sc, a_x.translate.x };
		a_out.WorldRow1 = { r.entry[1][0] * sc, r.entry[1][1] * sc, r.entry[1][2] * sc, a_x.translate.y };
		a_out.WorldRow2 = { r.entry[2][0] * sc, r.entry[2][1] * sc, r.entry[2][2] * sc, a_x.translate.z };
	};
	auto layoutFor = [&](uint64_t a_descKey, const RE::BSGraphics::VertexDesc& a_desc, bool a_skinned) -> ID3D11InputLayout* {
		auto& cache = a_skinned ? bloodSkinILCache : bloodILCache;
		auto& blob = a_skinned ? bloodSkinVSBlob : bloodVSBlob;
		auto& layout = cache[a_descKey];
		if (!layout && blob) {
			const uint32_t positionBytes = SD_PositionBytes(a_descKey, a_desc);
			const DXGI_FORMAT positionFormat = positionBytes >= 16 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
			const uint32_t uvOffset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			if (a_skinned) {
				const uint32_t skinOffset = a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
				D3D11_INPUT_ELEMENT_DESC elements[4] = {
					{ "POSITION", 0, positionFormat, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, uvOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, skinOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, skinOffset + 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				};
				globals::d3d::device->CreateInputLayout(elements, 4, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put());
			} else {
				D3D11_INPUT_ELEMENT_DESC elements[3] = {
					{ "POSITION", 0, positionFormat, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, uvOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, a_desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL), D3D11_INPUT_PER_VERTEX_DATA, 0 },
				};
				globals::d3d::device->CreateInputLayout(elements, 3, blob->GetBufferPointer(), blob->GetBufferSize(), layout.put());
			}
		}
		return layout.get();
	};

	for (const auto& capture : a_list) {
		auto* geometry = capture.geometry.get();
		if (!geometry)
			continue;
		auto& runtime = geometry->GetGeometryRuntimeData();
		a_cb.TexcoordOffset = capture.texcoord;
		a_cb.MaterialAlpha = capture.alpha;
		a_cb.AlphaThreshold = capture.alphaThreshold;
		a_cb.Spread = { capture.reveal, capture.mask ? 1.0f : 0.0f, 0.0f, capture.decalId };
		if (a_ownClock && capture.clock.y > 0.0f)
			a_cb.ClockNow = capture.clock;
		ID3D11ShaderResourceView* diffuse = capture.diffuse.get();
		context->PSSetShaderResources(0, 1, &diffuse);

		if (!capture.skinned) {
			auto* triShape = geometry->AsTriShape();
			auto* rendererData = runtime.rendererData;
			if (!triShape || !rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
				continue;
			const uint32_t indexCount = uint32_t(triShape->GetTrishapeRuntimeData().triangleCount) * 3;
			auto desc = rendererData->vertexDesc;
			if (indexCount == 0 || !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL))
				continue;
			uint64_t descKey;
			memcpy(&descKey, &desc, sizeof(descKey));
			auto* layout = layoutFor(descKey, desc, false);
			const UINT stride = uint32_t(descKey & 0xF) * 4;
			if (!layout || stride == 0)
				continue;
			rowsFrom(capture.world, a_cb);
			bloodCB->Update(a_cb);
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
			context->VSSetShader(a_rigidVS ? a_rigidVS : bloodVS, nullptr, 0);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			context->DrawIndexedInstanced(indexCount, instances, 0, 0, 0);
			drawn++;
			continue;
		}

		auto* skin = runtime.skinInstance.get();
		auto* skinData = skin ? skin->skinData.get() : nullptr;
		auto* skinPartition = skin ? skin->skinPartition.get() : nullptr;
		if (!skinData || !skinPartition || !skin->bones || !skinPartition->partitions.data())
			continue;
		const uint32_t boneCount = skinData->GetBoneCount();
		bloodPaletteScratch.assign(boneCount, RE::NiTransform{});
		std::vector<uint8_t> built(boneCount, 0);
		auto composedFor = [&](uint16_t a_bone) -> const RE::NiTransform* {
			if (a_bone >= boneCount)
				return nullptr;
			if (!built[a_bone]) {
				auto* boneNode = skin->bones[a_bone];
				if (boneNode) {
					bloodPaletteScratch[a_bone] = boneNode->world * skinData->GetBoneDataSkinToBone(a_bone);
				} else if (skin->boneMatrices && a_bone < skin->numMatrices) {
					const float* m = reinterpret_cast<const float*>(skin->boneMatrices) + size_t(a_bone) * 12;
					RE::NiTransform& t = bloodPaletteScratch[a_bone];
					for (int r = 0; r < 3; ++r) {
						for (int c = 0; c < 3; ++c)
							t.rotate.entry[r][c] = m[r * 4 + c];
						t.translate[r] = m[r * 4 + 3];
					}
					t.scale = 1.0f;
				} else {
					return nullptr;
				}
				built[a_bone] = 1;
			}
			return &bloodPaletteScratch[a_bone];
		};
		a_cb.WorldRow0 = { 1, 0, 0, 0 };
		a_cb.WorldRow1 = { 0, 1, 0, 0 };
		a_cb.WorldRow2 = { 0, 0, 1, 0 };
		bloodCB->Update(a_cb);
		context->VSSetShader(a_skinnedVS ? a_skinnedVS : bloodSkinVS, nullptr, 0);
		ID3D11Buffer* cb2 = bloodSkinCB->CB();
		context->VSSetConstantBuffers(2, 1, &cb2);
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
			if (!partDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !partDesc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !partDesc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED))
				continue;
			uint64_t descKey;
			memcpy(&descKey, &partDesc, sizeof(descKey));
			auto* layout = layoutFor(descKey, partDesc, true);
			const UINT stride = uint32_t(descKey & 0xF) * 4;
			if (!layout || stride == 0)
				continue;
			BloodSkinCB skinCB{};
			bool complete = true;
			for (uint16_t j = 0; j < part.numBones && complete; ++j) {
				const RE::NiTransform* m = composedFor(part.bones[j]);
				if (!m) {
					complete = false;
					break;
				}
				const auto& rot = m->rotate;
				const float sc = m->scale;
				skinCB.BoneRows[j * 3 + 0] = { rot.entry[0][0] * sc, rot.entry[0][1] * sc, rot.entry[0][2] * sc, m->translate.x };
				skinCB.BoneRows[j * 3 + 1] = { rot.entry[1][0] * sc, rot.entry[1][1] * sc, rot.entry[1][2] * sc, m->translate.y };
				skinCB.BoneRows[j * 3 + 2] = { rot.entry[2][0] * sc, rot.entry[2][1] * sc, rot.entry[2][2] * sc, m->translate.z };
			}
			if (!complete)
				continue;
			skinCB.SkinBoneCount = float(part.numBones);
			bloodSkinCB->Update(skinCB);
			UINT offset = 0;
			auto* vb = reinterpret_cast<ID3D11Buffer*>(buff->vertexBuffer);
			auto* ib = reinterpret_cast<ID3D11Buffer*>(buff->indexBuffer);
			context->IASetInputLayout(layout);
			context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
			context->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
			context->DrawIndexedInstanced(indexCount, instances, thisStart, 0, 0);
			drawn++;
		}
	}
	return drawn;
}

void SnowDeformation::RenderBloodCapture()
{
	bloodDepositsLast = 0;
	bloodDiscsLast = 0;
	bloodRenderSeconds += double(globals::game::deltaTime ? *globals::game::deltaTime : 1.0f / 60.0f);
	const uint32_t frame = globals::state->frameCount;
	if ((frame & 63) == 0 || bloodSeen.size() > 4096)
		std::erase_if(bloodSeen, [&](const auto& a_kv) { return frame - a_kv.second.frame > 600; });
	bloodSeenLive = uint32_t(bloodSeen.size());

	std::vector<BloodDisc> discs;
	{
		std::scoped_lock lock(bloodDiscMutex);
		discs.swap(bloodDiscQueue);
	}
	const bool tileWork = !bloodFineQueue.empty() || bloodTilesLive > 0;
	if (!settings.BloodOnSnow || !bloodMapTexture || !bloodClockTexture || (bloodCaptures.empty() && discs.empty() && !tileWork) || !EnsureBloodResources()) {
		bloodCaptures.clear();
		bloodFineQueue.clear();
		return;
	}
	RenderBloodTiles(discs);
	if (bloodCaptures.empty() && discs.empty())
		return;

	auto context = globals::d3d::context;
	globals::profiler->BeginPass("SnowDeformation::Decals - Capture");
	ID3D11RenderTargetView* rtvs[2] = { bloodMapTexture->rtv.get(), bloodClockTexture->rtv.get() };
	context->OMSetRenderTargets(2, rtvs, nullptr);
	context->OMSetBlendState(bloodBlendState.get(), nullptr, 0xFFFFFFFF);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	context->RSSetState(bloodRasterState.get());
	D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(deformMapDim), float(deformMapDim), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11SamplerState* sampler = bloodSampler.get();
	context->PSSetSamplers(0, 1, &sampler);

	BloodCB cb{};
	cb.WindowOrigin = windowOrigin;
	cb.TexelSize = deformWorldSize / float(deformMapDim);
	cb.MapDim = float(deformMapDim);
	cb.MapOrigin = mapOrigin;
	cb.ClockNow = { bloodBurialClock, gameClockHours.load(std::memory_order_relaxed) };
	cb.Intensity = std::clamp(settings.BloodIntensity, 0.0f, 2.0f);
	cb.NormalZMin = 0.3f;

	context->PSSetShader(bloodPS, nullptr, 0);
	bloodDepositsLast = DrawBloodList(context, bloodCaptures, cb);
	if (bloodDepositsLast && !bloodDepositLogged) {
		bloodDepositLogged = true;
		logger::info("[SNOW DEFORMATION] blood map: first frame deposited {} of {} decals", bloodDepositsLast, bloodCaptures.size());
	}
	bloodCaptures.clear();

	if (!discs.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(bloodDiscBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			auto* rows = static_cast<float4*>(mapped.pData);
			for (size_t i = 0; i < discs.size(); ++i) {
				const auto& d = discs[i];
				rows[i * 2] = { d.x, d.y, d.z, d.radius };
				rows[i * 2 + 1] = { d.r, d.g, d.b, d.amount };
			}
			context->Unmap(bloodDiscBuffer.get(), 0);
			cb.WorldRow0 = { 1, 0, 0, 0 };
			cb.WorldRow1 = { 0, 1, 0, 0 };
			cb.WorldRow2 = { 0, 0, 1, 0 };
			cb.MaterialAlpha = 1.0f;
			cb.AlphaThreshold = -1.0f;
			cb.Spread = { 1.0f, 0.0f, 0.0f, 0.0f };
			bloodCB->Update(cb);
			context->VSSetShader(bloodDiscVS, nullptr, 0);
			context->PSSetShader(bloodDiscPS, nullptr, 0);
			ID3D11ShaderResourceView* discSRV = bloodDiscSRV.get();
			context->VSSetShaderResources(1, 1, &discSRV);
			context->IASetInputLayout(nullptr);
			ID3D11Buffer* nullVB = nullptr;
			UINT zero = 0;
			context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
			context->DrawInstanced(6, uint32_t(discs.size()) * 4, 0, 0);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			context->VSSetShaderResources(1, 1, &nullSRV);
			bloodDiscsLast = uint32_t(discs.size());
		}
	}

	ID3D11ShaderResourceView* nullSRV = nullptr;
	context->PSSetShaderResources(0, 1, &nullSRV);
	ID3D11RenderTargetView* nullRTVs[2] = { nullptr, nullptr };
	context->OMSetRenderTargets(2, nullRTVs, nullptr);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();

	if (bloodDepositsLast > 0 || bloodDiscsLast > 0) {
		const float4 window = { windowOrigin.x, windowOrigin.y, windowOrigin.x + deformWorldSize, windowOrigin.y + deformWorldSize };
		bloodWrittenBox = bloodMapWritten ? float4{ std::min(bloodWrittenBox.x, window.x), std::min(bloodWrittenBox.y, window.y),
												std::max(bloodWrittenBox.z, window.z), std::max(bloodWrittenBox.w, window.w) } :
		                                    window;
		bloodMapWritten = true;
	}
}

void SnowDeformation::DrawBloodOverlay(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_postSkinDepth)
{
	bloodOverlaysLast = 0;
	bloodOverlaySet.clear();
	decalReachMissesLast = std::exchange(decalReachMisses, 0u);
	const bool copies = bloodPreSnowValid && bloodPreDecalValid;
	bloodPreSnowValid = false;
	bloodPreDecalValid = false;
	bloodRectPrev = bloodRectFrame;
	bloodRectPrevValid = bloodRectFrameValid;
	bloodRectFrameValid = false;
	decalMasksLast = 0;
	std::unordered_set<const RE::BSGeometry*> marked;
	marked.swap(markDecals);
	if (bloodOverlays.empty()) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Decals - Overlay");
		return;
	}
	const size_t decals = bloodOverlays.size();
	std::vector<BloodCapture> overlays;
	overlays.swap(bloodOverlays);
	if (!DecalOverlayWanted() || !settings.ProjSnowMatch || !a_postSkinDepth || !copies || !EnsureBloodResources())
		return;
	auto* context = a_context;
	globals::profiler->BeginPass(SnowDevSwitches::DecalOverlayRow("SnowDeformation::Decals - Overlay Mask"));

	winrt::com_ptr<ID3D11BlendState> savedBlend;
	FLOAT savedBlendFactor[4]{};
	UINT savedSampleMask = 0xFFFFFFFF;
	context->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
	winrt::com_ptr<ID3D11DepthStencilState> savedDepth;
	UINT savedStencilRef = 0;
	context->OMGetDepthStencilState(savedDepth.put(), &savedStencilRef);
	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	constexpr UINT kOverlaySlots = 4 + 2 * kBloodCopyCount;
	ID3D11ShaderResourceView* savedSRVs[kOverlaySlots]{};
	context->PSGetShaderResources(3, kOverlaySlots, savedSRVs);
	ID3D11ShaderResourceView* savedDiffuse = nullptr;
	context->PSGetShaderResources(0, 1, &savedDiffuse);
	ID3D11Buffer* savedVSCB1 = nullptr;
	ID3D11Buffer* savedVSCB2 = nullptr;
	ID3D11Buffer* savedPSCB1 = nullptr;
	context->VSGetConstantBuffers(1, 1, &savedVSCB1);
	context->VSGetConstantBuffers(2, 1, &savedVSCB2);
	context->PSGetConstantBuffers(1, 1, &savedPSCB1);

	context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
	context->OMSetDepthStencilState(bloodOverlayDepthState.get(), 0);
	context->RSSetState(bloodRasterState.get());
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(bloodOverlayVS, nullptr, 0);
	context->PSSetShader(bloodOverlayPS, nullptr, 0);

	BloodCB cb{};
	cb.MapDim = 1.0f;
	cb.Intensity = 1.0f;
	const bool shellPaintsOwn = bloodPreSkinDepthThisFrame && BloodTilesLive();
	bloodPreSkinDepthThisFrame = false;
	cb.Spread = { 0.0f, shellPaintsOwn ? 1.0f : 0.0f, 0.0f, 0.0f };
	const float W = std::max(bloodViewport.Width, 1.0f), H = std::max(bloodViewport.Height, 1.0f);
	const auto& eye = globals::game::shadowState->GetRuntimeData().cameraData.getEye();
	cb.OverlayView = { 0.5f * H * eye.projMat(1, 1), bloodViewport.TopLeftX + 0.5f * W, bloodViewport.TopLeftY + 0.5f * H, 0.0f };
	cb.OverlayRect = { float(bloodCopyRect.left) / W * 2.0f - 1.0f, 1.0f - float(bloodCopyRect.top) / H * 2.0f,
		float(bloodCopyRect.right) / W * 2.0f - 1.0f, 1.0f - float(bloodCopyRect.bottom) / H * 2.0f };
	bloodCB->Update(cb);
	ID3D11Buffer* cb1 = bloodCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);
	ID3D11ShaderResourceView* readSRVs[2 + 2 * kBloodCopyCount] = { Util::GetCurrentSceneDepthSRV(false), a_postSkinDepth };
	for (uint32_t i = 0; i < kBloodCopyCount; ++i) {
		readSRVs[2 + i] = bloodPreSnowSRV[i].get();
		readSRVs[2 + kBloodCopyCount + i] = bloodPreDecalSRV[i].get();
	}
	context->PSSetShaderResources(3, 2 + 2 * kBloodCopyCount, readSRVs);
	auto state = globals::state;
	ID3D11Buffer* sharedBuffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, sharedBuffers);

	ID3D11ShaderResourceView* maskSRV = nullptr;
	uint32_t width = 0, height = 0;
	if (auto* mainTexture = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture) {
		D3D11_TEXTURE2D_DESC mainDesc{};
		mainTexture->GetDesc(&mainDesc);
		const D3D11_BOX extent = ScreenRectBox(&bloodViewport, mainDesc.Width, mainDesc.Height, 0);
		width = std::max(extent.right, 1u);
		height = std::max(extent.bottom, 1u);
	}
	if (EnsureDecalMask(width, height)) {
		ID3D11RenderTargetView* savedRTVs[8]{};
		ID3D11DepthStencilView* savedDSV = nullptr;
		context->OMGetRenderTargets(8, savedRTVs, &savedDSV);
		const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		if (decalMaskClearView) {
			const D3D11_RECT clearRect = bloodCopyRect;
			decalMaskContext1->ClearView(decalMaskTexture->rtv.get(), zero, &clearRect, 1);
		} else {
			context->ClearRenderTargetView(decalMaskTexture->rtv.get(), zero);
		}
		ID3D11RenderTargetView* maskRTV = decalMaskTexture->rtv.get();
		context->OMSetRenderTargets(1, &maskRTV, nullptr);
		context->OMSetBlendState(decalMaskBlendState.get(), nullptr, 0xFFFFFFFF);
		context->PSSetShader(decalMaskPS, nullptr, 0);
		winrt::com_ptr<ID3D11SamplerState> savedSampler;
		context->PSGetSamplers(0, 1, savedSampler.put());
		ID3D11SamplerState* sampler = bloodSampler.get();
		context->PSSetSamplers(0, 1, &sampler);
		BloodCB maskCB{};
		maskCB.MapDim = 1.0f;
		maskCB.Intensity = 1.0f;
		maskCB.NormalZMin = -2.0f;
		const auto& fb = globals::game::frameBufferCached;
		const auto& m = fb.GetCameraViewProj();
		const auto adjust = fb.GetCameraPosAdjust();
		maskCB.ViewProjRow0 = { m.m[0][0], m.m[0][1], m.m[0][2], m.m[0][3] };
		maskCB.ViewProjRow1 = { m.m[1][0], m.m[1][1], m.m[1][2], m.m[1][3] };
		maskCB.ViewProjRow2 = { m.m[2][0], m.m[2][1], m.m[2][2], m.m[2][3] };
		maskCB.ViewProjRow3 = { m.m[3][0], m.m[3][1], m.m[3][2], m.m[3][3] };
		maskCB.CameraAdjust = { adjust.x, adjust.y, adjust.z, 0.0f };
		const auto kept = std::remove_if(overlays.begin(), overlays.end(), [&](const BloodCapture& a_capture) { return marked.contains(a_capture.geometry.get()); });
		const bool excluded = kept != overlays.end();
		overlays.erase(kept, overlays.end());
		const uint32_t drawn = DrawBloodList(context, overlays, maskCB, decalMaskVS, 1u, false, decalMaskSkinVS);
		decalMasksLast = uint32_t(std::count_if(overlays.begin(), overlays.end(), [](const BloodCapture& a_capture) { return a_capture.mask; }));
		ID3D11SamplerState* restoreSampler = savedSampler.get();
		context->PSSetSamplers(0, 1, &restoreSampler);
		context->OMSetRenderTargets(8, savedRTVs, savedDSV);
		for (auto* rtv : savedRTVs)
			if (rtv)
				rtv->Release();
		if (savedDSV)
			savedDSV->Release();
		maskSRV = decalMaskTexture->srv.get();
		if (drawn && !decalMaskLogged) {
			decalMaskLogged = true;
			logger::info("[SNOW DEFORMATION] decal overlay: first frame drew {} mask draws for {} decals, {} not blood", drawn, overlays.size(), decalMasksLast);
		}
		cb.Spread.z = (drawn || excluded) ? 1.0f : 0.0f;
		context->OMSetBlendState(bloodOverlayBlendState.get(), nullptr, 0xFFFFFFFF);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(bloodOverlayVS, nullptr, 0);
		context->PSSetShader(bloodOverlayPS, nullptr, 0);
		bloodCB->Update(cb);
	}
	context->PSSetShaderResources(15, 1, &maskSRV);
	ID3D11ShaderResourceView* preSkinSRV = shellPaintsOwn ? bloodPreSkinDepthSRV.get() : nullptr;
	context->PSSetShaderResources(16, 1, &preSkinSRV);

	globals::profiler->EndPass();
	globals::profiler->BeginPass(SnowDevSwitches::DecalOverlayRow("SnowDeformation::Decals - Overlay Composite"));
	context->Draw(6, 0);
	bloodOverlaysLast = uint32_t(decals);
	if (!bloodOverlayLogged) {
		bloodOverlayLogged = true;
		logger::info("[SNOW DEFORMATION] blood overlay: first frame composited {} decals over object snow, rect {}x{}", decals, bloodCopyRect.right - bloodCopyRect.left, bloodCopyRect.bottom - bloodCopyRect.top);
	}

	context->PSSetShaderResources(3, kOverlaySlots, savedSRVs);
	context->PSSetShaderResources(0, 1, &savedDiffuse);
	context->VSSetConstantBuffers(1, 1, &savedVSCB1);
	context->VSSetConstantBuffers(2, 1, &savedVSCB2);
	context->PSSetConstantBuffers(1, 1, &savedPSCB1);
	for (auto* srv : savedSRVs)
		if (srv)
			srv->Release();
	if (savedDiffuse)
		savedDiffuse->Release();
	if (savedVSCB1)
		savedVSCB1->Release();
	if (savedVSCB2)
		savedVSCB2->Release();
	if (savedPSCB1)
		savedPSCB1->Release();
	context->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
	context->OMSetDepthStencilState(savedDepth.get(), savedStencilRef);
	context->RSSetState(savedRaster.get());
	globals::profiler->EndPass();
}
