// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/D3D.h"

// Voxel snow volume (VOLUME-SNOW-PLAN): the captured statics rasterised
// into camera-anchored 3D windows - a clipmap of them, each level twice the
// voxel of the one inside it, drawn only where no finer level reaches.

bool SnowDeformation::EnsureVoxelResources()
{
	const uint levels = VoxelLevelsLive();
	bool levelsReady = true;
	for (uint i = 0; i < levels; i++) {
		const auto& lv = voxelLevels[i];
		levelsReady = levelsReady && lv.dim == VoxelDimForLevel(i) && lv.volume[0] && lv.volume[1] && lv.field && lv.support && lv.height && lv.bricks && lv.drawArgs && lv.dirty && lv.flags && lv.lists;
	}
	if (levelsReady && voxelSliceTexture && voxelCB && voxelDrawCB && voxelRasterState && voxelWrapSampler &&
		voxelVS && voxelGS && voxelPS && voxelScrollCS && voxelSliceCS && voxelSeedCS && voxelBlurZCS && voxelBlurCS && voxelBrickListCS &&
		voxelDiffCS && voxelDirtyColsCS && voxelBrickFlagsCS)
		return true;
	if (voxelShadersFailed)
		return false;
	LoadTraceScope _loadTrace(this, "Volume: EnsureVoxelResources");
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;

	// Fresh VRAM is not blank: cleared so a never-written volume reads
	// empty rather than as whatever lived there before.
	const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto makeVolume = [&](const std::string& a_name, uint a_dim) {
		D3D11_TEXTURE3D_DESC volDesc{
			.Width = a_dim,
			.Height = a_dim,
			.Depth = a_dim,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R8_UNORM,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC volSrvDesc = {
			.Format = volDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC volUavDesc = {
			.Format = volDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = a_dim }
		};
		auto* tex = new Texture3D(volDesc, a_name.c_str());
		tex->CreateSRV(volSrvDesc);
		tex->CreateUAV(volUavDesc);
		context->ClearUnorderedAccessViewFloat(tex->uav.get(), clearZero);
		return tex;
	};
	for (uint i = 0; i < levels; i++) {
		auto& lv = voxelLevels[i];
		const uint dim = VoxelDimForLevel(i);
		// A level whose grid size changed (Fine Levels moved) is rebuilt
		// from nothing; its old textures are the wrong size for every pass.
		if (lv.dim != dim) {
			for (int p = 0; p < 2; p++) {
				delete lv.volume[p];
				lv.volume[p] = nullptr;
			}
			delete lv.field;
			lv.field = nullptr;
			delete lv.support;
			lv.support = nullptr;
			delete lv.height;
			lv.height = nullptr;
			delete lv.bricks;
			lv.bricks = nullptr;
			delete lv.drawArgs;
			lv.drawArgs = nullptr;
			delete lv.dirty;
			lv.dirty = nullptr;
			delete lv.flags;
			lv.flags = nullptr;
			delete lv.lists;
			lv.lists = nullptr;
			lv.dim = dim;
		}
		for (int p = 0; p < 2; p++)
			if (!lv.volume[p])
				lv.volume[p] = makeVolume(std::format("SnowDeformation::VoxelVolume{}{}", i, p), dim);
		if (!lv.field)
			lv.field = makeVolume(std::format("SnowDeformation::VoxelField{}", i), dim);
		if (!lv.support)
			lv.support = makeVolume(std::format("SnowDeformation::VoxelSupport{}", i), dim);
		if (!lv.height)
			lv.height = makeVolume(std::format("SnowDeformation::VoxelHeight{}", i), dim);
		if (!lv.bricks) {
			const uint bricksPerAxis = dim / 8;
			const uint capacity = bricksPerAxis * bricksPerAxis * bricksPerAxis;
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(uint32_t);
			desc.ByteWidth = sizeof(uint32_t) * capacity;
			lv.bricks = new Buffer(desc, nullptr, std::format("SnowDeformation::VoxelBricks{}", i).c_str());
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = capacity;
			lv.bricks->CreateSRV(srvDesc);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_UNKNOWN;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = 0;
			uavDesc.Buffer.NumElements = capacity;
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
			lv.bricks->CreateUAV(uavDesc);
		}
		if (!lv.drawArgs) {
			// {VertexCountPerInstance, InstanceCount, StartVertex, StartInstance};
			// only the count changes, copied from the list's counter.
			const uint32_t args[4] = { 36, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA init{ args, 0, 0 };
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.ByteWidth = sizeof(args);
			desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			lv.drawArgs = new Buffer(desc, &init, std::format("SnowDeformation::VoxelDrawArgs{}", i).c_str());
		}
		// Dirty-brick bookkeeping: raw buffers, zeroed. The lists buffer is
		// also the partial passes' DispatchIndirect args.
		auto makeRaw = [&](const std::string& a_name, uint a_bytes, bool a_args) {
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.ByteWidth = a_bytes;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | (a_args ? (UINT)D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0u);
			auto* buf = new Buffer(desc, nullptr, a_name.c_str());
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
			srvDesc.BufferEx.FirstElement = 0;
			srvDesc.BufferEx.NumElements = a_bytes / 4;
			srvDesc.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
			buf->CreateSRV(srvDesc);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.FirstElement = 0;
			uavDesc.Buffer.NumElements = a_bytes / 4;
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			buf->CreateUAV(uavDesc);
			const UINT zeros[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(buf->uav.get(), zeros);
			return buf;
		};
		{
			const uint bricksPerAxis = dim / 8;
			const uint brickBytes = bricksPerAxis * bricksPerAxis * bricksPerAxis * sizeof(uint32_t);
			if (!lv.dirty)
				lv.dirty = makeRaw(std::format("SnowDeformation::VoxelDirty{}", i), brickBytes, false);
			if (!lv.flags)
				lv.flags = makeRaw(std::format("SnowDeformation::VoxelBrickFlags{}", i), brickBytes, false);
			if (!lv.lists)
				lv.lists = makeRaw(std::format("SnowDeformation::VoxelDirtyLists{}", i), kVoxelListBytes, true);
		}
		lv.valid = false;
	}
	if (!voxelSliceTexture) {
		D3D11_TEXTURE2D_DESC desc{
			.Width = kVoxelDim,
			.Height = kVoxelDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R8_UNORM,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		voxelSliceTexture = new Texture2D(desc, "SnowDeformation::VoxelSlice");
		voxelSliceTexture->CreateSRV(srvDesc);
		voxelSliceTexture->CreateUAV(uavDesc);
		// Shown by the menu before the first write; black, not stale VRAM.
		context->ClearUnorderedAccessViewFloat(voxelSliceTexture->uav.get(), clearZero);
	}
	if (!voxelCB)
		voxelCB = new ConstantBuffer(ConstantBufferDesc<VoxelVolumeCB>(), "SnowDeformation::VoxelVolumeCB");
	if (!voxelDrawCB)
		voxelDrawCB = new ConstantBuffer(ConstantBufferDesc<VoxelDrawCB>(), "SnowDeformation::VoxelDrawCB");
	if (!voxelWrapSampler) {
		// Trilinear + WRAP: the torus, after the shader clamps in logical space.
		D3D11_SAMPLER_DESC sampDesc{};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (SUCCEEDED(device->CreateSamplerState(&sampDesc, voxelWrapSampler.put())))
			Util::SetResourceName(voxelWrapSampler.get(), "SnowDeformation::VoxelWrapSampler");
	}
	if (!voxelCountBuffer) {
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = 16;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		HRESULT hr = device->CreateBuffer(&desc, nullptr, voxelCountBuffer.put());
		if (SUCCEEDED(hr)) {
			Util::SetResourceName(voxelCountBuffer.get(), "SnowDeformation::VoxelOccupancyCount");
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = 4;
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			hr = device->CreateUnorderedAccessView(voxelCountBuffer.get(), &uavDesc, voxelCountUAV.put());
			D3D11_BUFFER_DESC stagingDesc{};
			stagingDesc.ByteWidth = 16;
			stagingDesc.Usage = D3D11_USAGE_STAGING;
			stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < 2 && SUCCEEDED(hr); i++) {
				hr = device->CreateBuffer(&stagingDesc, nullptr, voxelCountStaging[i].put());
				if (SUCCEEDED(hr))
					Util::SetResourceName(voxelCountStaging[i].get(), "SnowDeformation::VoxelOccupancyStaging");
			}
		}
		if (FAILED(hr))
			logger::warn("[SNOW DEFORMATION] Voxel occupancy counter unavailable (HRESULT {:#x}); the volume still runs", (unsigned)hr);
	}
	if (!voxelRasterState) {
		// Both faces: undersides are what the roof test is about.
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		if (FAILED(device->CreateRasterizerState(&rasterDesc, voxelRasterState.put()))) {
			voxelShadersFailed = true;
			return false;
		}
		Util::SetResourceName(voxelRasterState.get(), "SnowDeformation::VoxelRaster");
	}

	// Util::CompileShader adds the stage define from the target itself.
	constexpr auto path = L"Data\\Shaders\\SnowDeformation\\SnowVoxelCapture.hlsl";
	auto compileCS = [&](ID3D11ComputeShader*& a_cs, const char* a_entry, const char* a_name) {
		if (a_cs)
			return;
		a_cs = static_cast<ID3D11ComputeShader*>(CompileSnowShader(path, {}, "cs_5_0", a_entry));
		if (a_cs)
			Util::SetResourceName(a_cs, a_name);
	};
	if (!voxelVS) {
		voxelVS = static_cast<ID3D11VertexShader*>(CompileSnowShader(path, {}, "vs_5_0"));
		if (voxelVS)
			Util::SetResourceName(voxelVS, "SnowDeformation::VoxelCaptureVS");
	}
	if (!voxelGS) {
		voxelGS = static_cast<ID3D11GeometryShader*>(CompileSnowShader(path, {}, "gs_5_0"));
		if (voxelGS)
			Util::SetResourceName(voxelGS, "SnowDeformation::VoxelCaptureGS");
	}
	if (!voxelPS) {
		voxelPS = static_cast<ID3D11PixelShader*>(CompileSnowShader(path, {}, "ps_5_0"));
		if (voxelPS)
			Util::SetResourceName(voxelPS, "SnowDeformation::VoxelCapturePS");
	}
	compileCS(voxelScrollCS, "VoxelScrollCS", "SnowDeformation::VoxelScrollCS");
	compileCS(voxelSliceCS, "VoxelSliceCS", "SnowDeformation::VoxelSliceCS");
	compileCS(voxelSeedCS, "VoxelSeedCS", "SnowDeformation::VoxelSeedCS");
	compileCS(voxelBlurZCS, "VoxelBlurZCS", "SnowDeformation::VoxelBlurZCS");
	compileCS(voxelBlurCS, "VoxelBlurCS", "SnowDeformation::VoxelBlurCS");
	compileCS(voxelBrickListCS, "VoxelBrickListCS", "SnowDeformation::VoxelBrickListCS");
	compileCS(voxelDiffCS, "VoxelDiffCS", "SnowDeformation::VoxelDiffCS");
	compileCS(voxelDirtyColsCS, "VoxelDirtyColsCS", "SnowDeformation::VoxelDirtyColsCS");
	compileCS(voxelBrickFlagsCS, "VoxelBrickFlagsCS", "SnowDeformation::VoxelBrickFlagsCS");
	if (!voxelVS || !voxelGS || !voxelPS || !voxelScrollCS || !voxelSliceCS || !voxelSeedCS || !voxelBlurZCS || !voxelBlurCS || !voxelBrickListCS ||
		!voxelDiffCS || !voxelDirtyColsCS || !voxelBrickFlagsCS) {
		voxelShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Voxel volume disabled (shader compilation failed: VS {} GS {} PS {} ScrollCS {} SliceCS {} SeedCS {} BlurZCS {} BlurCS {} BrickListCS {} DiffCS {} DirtyColsCS {} BrickFlagsCS {})",
			voxelVS ? "ok" : "FAILED", voxelGS ? "ok" : "FAILED", voxelPS ? "ok" : "FAILED",
			voxelScrollCS ? "ok" : "FAILED", voxelSliceCS ? "ok" : "FAILED",
			voxelSeedCS ? "ok" : "FAILED", voxelBlurZCS ? "ok" : "FAILED", voxelBlurCS ? "ok" : "FAILED", voxelBrickListCS ? "ok" : "FAILED",
			voxelDiffCS ? "ok" : "FAILED", voxelDirtyColsCS ? "ok" : "FAILED", voxelBrickFlagsCS ? "ok" : "FAILED");
		return false;
	}
	return true;
}

// A far ring has half the cubes a side at twice the voxel, so its extent
// is what a full ring's would be: the reach doubles per level throughout,
// the far rings at an eighth of the work and memory.
uint SnowDeformation::VoxelDimForLevel(uint a_level) const
{
	const uint fine = (uint)std::clamp(settings.VolumeFineLevels, 1, (int)kVoxelMaxLevels);
	return a_level < fine ? kVoxelDim : kVoxelFarDim;
}

float SnowDeformation::VoxelSizeForLevel(uint a_level) const
{
	return VoxelSizeLive() * float(1u << a_level) * float(kVoxelDim / VoxelDimForLevel(a_level));
}

float SnowDeformation::VoxelExtentForLevel(uint a_level) const
{
	return kVoxelDim * VoxelSizeLive() * float(1u << a_level);
}

void SnowDeformation::VoxelReachBand(uint a_level, float& a_start, float& a_end) const
{
	const float frac = VoxelDimForLevel(a_level) == kVoxelDim ? kVoxelReachFrac : kVoxelReachFracFar;
	a_end = VoxelExtentForLevel(a_level) * 0.5f * frac;
	a_start = a_end * (1.0f - kVoxelBandFrac);
}

void SnowDeformation::FillVoxelCB(uint a_level, VoxelVolumeCB& a_cb) const
{
	const auto& lv = voxelLevels[a_level];
	const float voxelSize = VoxelSizeForLevel(a_level);
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();

	a_cb.OriginVox = { lv.origin.x, lv.origin.y, lv.origin.z, 0 };
	a_cb.ScrollDelta = { 0, 0, 0, 0 };
	a_cb.VoxelSize = voxelSize;
	a_cb.Dim = (int)lv.dim;
	a_cb.SliceAxis = std::clamp(voxelSliceAxis, 0, 2);
	a_cb.SliceXray = voxelSliceXray ? 1 : 0;
	a_cb.SliceSource = std::clamp(voxelSliceSource, 0, 2);
	{
		const float along = a_cb.SliceAxis == 0 ? eye.z : (a_cb.SliceAxis == 1 ? eye.y : eye.x);
		const int originAlong = a_cb.SliceAxis == 0 ? lv.origin.z : (a_cb.SliceAxis == 1 ? lv.origin.y : lv.origin.x);
		a_cb.SliceIndex = std::clamp((int)std::floor((along + voxelSliceOffset) / voxelSize) - originAlong, 0, (int)lv.dim - 1);
	}
	// The seed gate's maps live in the height window; without them every
	// column reads as open sky (HalfExtent 0 fails the window test).
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	a_cb.HeightWindowCenter = heightWindowCenter;
	a_cb.HeightHalfExtent = seedMaps ? kHeightMapHalfExtent : 0.0f;
	a_cb.ShelterDust = kVoxelShelterDust;
	// The ramp field: the depth in this level's voxels, unfloored, and a
	// fixed crossing at 0.5. Rounding likewise unfloored - under about 0.4
	// voxel the kernel radius rounds to zero and the coarse ring keeps its
	// crisp voxel edges rather than eroding a whole voxel at every one.
	a_cb.DepthVox = std::max(settings.VolumeSnowDepth, 0.0f) / voxelSize;
	a_cb.FieldThreshold = VoxelThresholdForLevel(a_level);
	// The Rounding in this level's voxels. The smoothing kernel takes it
	// floored at half a voxel, so a far ring's columns meld across their
	// steps. The field's exponential rate is ln 2 over it floored at TWO
	// voxels: a half-air neighbourhood then drops the snow by the Rounding
	// (or two voxels, where that is more), and the crossing between the two
	// centres bracketing the top interpolates within a tenth of a voxel.
	// Air neighbours vote only where the voxel is small against the
	// Rounding: on a ring whose voxel is the Rounding or more, dilution
	// would take a whole voxel off every rim, so there they do not.
	const float roundU = std::clamp(settings.VolumeSnowRounding, 0.0f, 32.0f);
	const float roundVox = roundU / voxelSize;
	a_cb.RoundSigma = std::max(roundVox, 0.5f);
	a_cb.EdgeParams[0] = std::clamp(settings.VolumeEdgeNoise, 0.0f, 16.0f);
	a_cb.EdgeParams[1] = kVoxelEdgeNoiseCell;
	a_cb.EdgeParams[2] = std::log(2.0f) / std::max(roundVox, 2.0f);
	a_cb.EdgeParams[3] = std::clamp(settings.VolumeSnowOverhang, 0.0f, 16.0f);
	a_cb.LipParams[0] = std::clamp((roundVox - 0.5f) / 1.5f, 0.0f, 1.0f);
	a_cb.LipParams[1] = a_cb.LipParams[2] = a_cb.LipParams[3] = 0.0f;
	a_cb.OverhangVox = (float)std::clamp((int)std::lround(std::clamp(settings.VolumeSnowOverhang, 0.0f, 16.0f) / voxelSize), 0, 7);
	a_cb.HeadroomVox = (float)std::max(1, (int)std::lround(kVoxelHeadroomUnits / voxelSize));
	a_cb.SlopeMinNz = std::cos(DirectX::XMConvertToRadians(std::clamp(settings.VolumeSnowMaxSlopeDeg, 0.0f, 90.0f)));
	a_cb.SkyStrength = std::clamp(settings.VolumeSkyExposurePct / 100.0f, 0.0f, 1.0f);
	// Memory: the life nibble ticks down 15 times over voxelMemorySeconds,
	// counted in this level's own rebuilds - a lazy ring sees a 2^L-th of
	// the frames.
	const uint32_t period = VoxelLevelPeriod(a_level);
	const uint32_t tickEvery = std::max(1u, (uint32_t)std::lround(voxelMemorySeconds * 60.0f / 15.0f) / period);
	a_cb.Decay = ((voxelFrame / period) % tickEvery == 0) ? 1.0f : 0.0f;
	const auto centre = VoxelLevelCentre(a_level);
	a_cb.CentreVox[0] = centre.x / voxelSize;
	a_cb.CentreVox[1] = centre.y / voxelSize;
	a_cb.CentreVox[2] = centre.z / voxelSize;
	// Bricks reach the end of this level's band, Chebyshev. The hole for the
	// level inside is the draw VS's, per frame.
	float bandStart = 0.0f, bandEnd = 0.0f;
	VoxelReachBand(a_level, bandStart, bandEnd);
	a_cb.CentreVox[3] = bandEnd / voxelSize;
	a_cb.ForceDirty = 0.0f;
}

uint32_t SnowDeformation::VoxelLevelPeriod(uint a_level) const
{
	return settings.VolumeLazyRings ? (1u << a_level) : 1u;
}

float SnowDeformation::VoxelThresholdForLevel(uint a_level) const
{
	// Where air votes, the lip's survival is the ratio k >= Coverage / gain
	// of the field's saturation, so the gain stays 1; where it does not,
	// nothing dilutes and the gain only buys R8 precision for a thin layer.
	const float coverage = std::clamp(settings.VolumeSnowCoverage, 0.02f, 0.5f);
	const float roundVox = std::clamp(settings.VolumeSnowRounding, 0.0f, 32.0f) / VoxelSizeForLevel(a_level);
	const float airWeight = std::clamp((roundVox - 0.5f) / 1.5f, 0.0f, 1.0f);
	const float gain = std::min(1.0f + 7.0f * (1.0f - airWeight), 0.5f / coverage);
	return coverage * gain;
}

DirectX::XMFLOAT3 SnowDeformation::VoxelLevelCentre(uint a_level) const
{
	const auto& o = voxelLevels[a_level].origin;
	const float voxelSize = VoxelSizeForLevel(a_level);
	const float halfVox = voxelLevels[a_level].dim * 0.5f;
	return { (o.x + halfVox) * voxelSize, (o.y + halfVox) * voxelSize, (o.z + halfVox) * voxelSize };
}

void SnowDeformation::RenderVoxelVolume(const StaticsCB* a_records, uint32_t a_captureCount, bool a_recordsLive, uint32_t& a_parity)
{
	if (!settings.VolumeSnow) {
		for (auto& lv : voxelLevels)
			lv.valid = false;
		return;
	}
	if (!EnsureVoxelResources())
		return;
	auto* context = globals::d3d::context;

	// The window origins are in VOXEL units, so a size change makes every
	// stored voxel's world position wrong: start the windows over.
	const float voxelSize0 = VoxelSizeLive();
	if (voxelSize0 != voxelSizeBuilt) {
		voxelSizeBuilt = voxelSize0;
		for (auto& lv : voxelLevels)
			lv.valid = false;
	}
	const uint levels = VoxelLevelsLive();
	const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	const bool counting = voxelCountUAV && voxelCountStaging[0] && voxelCountStaging[1];
	voxelFrame++;
	// The camera's heading (TerrainData's derivation): the windows sit ahead
	// of the eye, since what is behind it is not in the capture list anyway.
	float fwdX = 0.0f, fwdY = 0.0f;
	if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		const auto& r = cam->cameraRoot->world.rotate;
		const float fx = r.entry[0][1], fy = r.entry[1][1];
		const float len = std::sqrt(fx * fx + fy * fy);
		if (len > 1e-3f) {
			fwdX = fx / len;
			fwdY = fy / len;
		}
	}

	// ---- Occupancy: scroll + decay per rebuilding level ----
	globals::profiler->BeginPass("SnowDeformation::VoxelScroll");
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		// Lazy rings: level L rebuilds every 2^L frames, phased at 2^(L-1)
		// so no frame carries more than two levels; a level with nothing
		// built yet rebuilds now. Between rebuilds it keeps its origin,
		// its field and its bricks, and draws off them.
		const uint32_t period = VoxelLevelPeriod(L);
		const uint32_t phase = period > 1 ? period / 2 : 0;
		lv.updated = !lv.valid || (voxelFrame % period) == phase;
		if (!lv.updated)
			continue;
		const float voxelSize = VoxelSizeForLevel(L);
		const float half = VoxelExtentForLevel(L) * 0.5f;
		const UINT groups = lv.dim / 8;
		// The cube ahead of the camera, its origin snapped to WHOLE BRICKS
		// (centred, so the snap is within half a brick): the dirty-brick
		// bookkeeping is per physical brick, which is one logical brick only
		// while the origin is a multiple of eight.
		const float ahead = kVoxelForwardFrac * 2.0f * half;
		const float brickUnits = 8.0f * voxelSize;
		const float snapBias = 4.0f * voxelSize;
		const DirectX::XMINT3 origin{
			(int)std::floor((eye.x + fwdX * ahead - half + snapBias) / brickUnits) * 8,
			(int)std::floor((eye.y + fwdY * ahead - half + snapBias) / brickUnits) * 8,
			(int)std::floor((eye.z - half + snapBias) / brickUnits) * 8
		};
		const DirectX::XMINT3 delta{ origin.x - lv.origin.x, origin.y - lv.origin.y, origin.z - lv.origin.z };
		const bool clearAll = !lv.valid;
		lv.cleared = clearAll;
		lv.origin = origin;
		lv.valid = true;
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		cb.OriginVox.w = clearAll ? 1 : 0;
		cb.ScrollDelta = { delta.x, delta.y, delta.z, 0 };
		voxelCB->Update(cb);
		// Dirty bookkeeping for this rebuild: no bricks dirty yet, and the
		// partial passes' dispatch args at zero groups (y = bricks of z).
		{
			const UINT zeros[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(lv.dirty->uav.get(), zeros);
			const uint32_t zb = lv.dim / 8;
			const uint32_t header[16] = { 0, zb, 1, 0, 1, 1, 0, zb, 1, 0, zb, 1, 0, zb, 1, 0 };
			D3D11_BOX box{ 0, 0, 0, kVoxelListHeaderBytes, 1, 1 };
			context->UpdateSubresource(lv.lists->resource.get(), 0, &box, header, 0, 0);
		}

		const uint previous = lv.current;
		lv.current ^= 1;
		{
			// Level 0 carries the menu's occupancy count.
			const bool countHere = counting && L == 0;
			if (countHere) {
				const UINT zeros[4] = { 0, 0, 0, 0 };
				context->ClearUnorderedAccessViewUint(voxelCountUAV.get(), zeros);
			}
			ID3D11ShaderResourceView* srv = lv.volume[previous]->srv.get();
			ID3D11UnorderedAccessView* uavs[3] = { lv.volume[lv.current]->uav.get(), nullptr, countHere ? voxelCountUAV.get() : nullptr };
			ID3D11UnorderedAccessView* dirtyUAV = lv.dirty->uav.get();
			context->CSSetConstantBuffers(0, 1, &cbPtr);
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
			context->CSSetUnorderedAccessViews(5, 1, &dirtyUAV, nullptr);
			context->CSSetShader(voxelScrollCS, nullptr, 0);
			context->Dispatch(groups, groups, groups);
			ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
			context->CSSetUnorderedAccessViews(5, 1, &nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			// Copy this frame's count, map LAST frame's copy without waiting.
			if (countHere) {
				context->CopyResource(voxelCountStaging[voxelCountCursor].get(), voxelCountBuffer.get());
				voxelCountCursor ^= 1;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (SUCCEEDED(context->Map(voxelCountStaging[voxelCountCursor].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
					voxelOccupancy = *static_cast<const uint32_t*>(mapped.pData);
					context->Unmap(voxelCountStaging[voxelCountCursor].get(), 0);
					voxelOccupancyValid = true;
				}
			}
		}

	}
	globals::profiler->EndPass();

	// ---- This frame's captures, per rebuilding level ----
	// UAV-only raster: no target, the viewport sizes it. The game's VS b0
	// goes back afterwards; the capture pass never touched it. Its own
	// profiler row: with the field passes at a quarter of a millisecond,
	// this is where the building's time goes.
	globals::profiler->BeginPass("SnowDeformation::VoxelRaster");
	{
		winrt::com_ptr<ID3D11RasterizerState> savedRaster;
		context->RSGetState(savedRaster.put());
		winrt::com_ptr<ID3D11Buffer> savedVSCB0;
		context->VSGetConstantBuffers(0, 1, savedVSCB0.put());
		context->RSSetState(voxelRasterState.get());
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(voxelVS, nullptr, 0);
		context->GSSetShader(voxelGS, nullptr, 0);
		context->PSSetShader(voxelPS, nullptr, 0);
		ID3D11Buffer* cb1 = staticsCB->CB();
		context->VSSetConstantBuffers(1, 1, &cb1);
		for (uint L = 0; L < levels; L++) {
			auto& lv = voxelLevels[L];
			if (!lv.updated)
				continue;
			VoxelVolumeCB cb{};
			FillVoxelCB(L, cb);
			voxelCB->Update(cb);
			context->VSSetConstantBuffers(0, 1, &cbPtr);
			context->GSSetConstantBuffers(0, 1, &cbPtr);
			context->PSSetConstantBuffers(0, 1, &cbPtr);
			D3D11_VIEWPORT viewport{ 0.0f, 0.0f, float(lv.dim), float(lv.dim), 0.0f, 1.0f };
			context->RSSetViewports(1, &viewport);
			// u0 occupancy, u1 the surface height within the voxel.
			ID3D11UnorderedAccessView* rasterUAVs[2] = { lv.volume[lv.current]->uav.get(), lv.height->uav.get() };
			context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, rasterUAVs, nullptr);
			// STAGGERED CAPTURE. A static already in the volume stays there on
			// the memory nibble (a tick every 16 of this level's rebuilds, 15
			// to expire), so drawing it every rebuild only paid for the same
			// voxels again - after dirty bricks, the one full-cost pass left. A
			// known record draws every 8th rebuild on the finest ring (4th,
			// 2nd on the next two; the far rings rebuild rarely anyway); a
			// level starting from nothing draws everything; a new record waits
			// at most a period.
			const uint32_t rasterPeriod = (settings.VolumeStaggeredCapture && !lv.cleared) ? std::max(1u, 8u >> L) : 1u;

			for (uint32_t ci = 0; ci < a_captureCount; ci++) {
				if (rasterPeriod > 1 && ((ci + lv.rebuilds) % rasterPeriod) != 0)
					continue;
				const auto& cap = capturedStatics[ci];
				// Roads and bridges belong to the trench patch, which drapes the
				// road heightfield itself; the S4 skin excludes them for the same
				// reason. Voxelising them put volume snow over every RoadChunk
				// (Josef, 2026-09-06).
				if (cap.road || cap.bridge)
					continue;
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
				if (a_recordsLive)
					BindStaticsRecord(ci, false, false, a_parity);
				else
					staticsCB->Update(a_records[ci]);
				context->DrawIndexed(indexCount, 0, 0);
			}

			ID3D11UnorderedAccessView* nullRasterUAVs[2] = { nullptr, nullptr };
			context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, nullRasterUAVs, nullptr);
		}
		context->GSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* restoreVSCB0 = savedVSCB0.get();
		context->VSSetConstantBuffers(0, 1, &restoreVSCB0);
		context->GSSetConstantBuffers(0, 1, &nullCB);
		context->PSSetConstantBuffers(0, 1, &nullCB);
		context->RSSetState(savedRaster.get());
	}
	globals::profiler->EndPass();

	// ---- The snow field, then the draw's brick list, per level ----
	// DIRTY BRICKS: the field is world-anchored (a physical slot is the
	// same world voxel until the window scrolls 256 past it), so only the
	// brick columns whose occupancy changed - plus the blur's reach around
	// them - are recomputed; the rest keep last rebuild's field. The scroll
	// marked the slots it reused, the compare marks what the raster
	// changed, and one small pass turns the marks into column lists that
	// drive the passes as DispatchIndirect. Seeds go into the dead ping-pong
	// volume (its previous occupancy is compared first, then overwritten),
	// then Z, X, Y bouncing between it and the field. UP FIRST: the sideways
	// passes then run in the air above surfaces, where the solid blocker
	// (occupancy at t4) can stop them at a riser or a wall.
	globals::profiler->BeginPass("SnowDeformation::VoxelField");
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		if (!lv.updated)
			continue;
		const UINT groups = lv.dim / 8;
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		lv.rebuilds++;
		const bool forceAll = !settings.VolumeDirtyBricks || (lv.rebuilds % kVoxelFullRefreshRebuilds) == 0;
		cb.ForceDirty = float((forceAll ? 1u : 0u) | (settings.VolumeSparseBricks ? 0u : 2u));
		Texture3D* scratch = lv.volume[lv.current ^ 1];
		Texture3D* occupancy = lv.volume[lv.current];
		ID3D11ShaderResourceView* occSRV = occupancy->srv.get();
		ID3D11ShaderResourceView* scratchSRV = scratch->srv.get();
		ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
		ID3D11ShaderResourceView* supportSRV = lv.support->srv.get();
		ID3D11ShaderResourceView* listsSRV = lv.lists->srv.get();
		ID3D11ShaderResourceView* dirtySRV = lv.dirty->srv.get();
		ID3D11ShaderResourceView* flagsSRV = lv.flags->srv.get();
		ID3D11UnorderedAccessView* scratchUAV = scratch->uav.get();
		ID3D11UnorderedAccessView* fieldUAV = lv.field->uav.get();
		ID3D11UnorderedAccessView* supportUAV = lv.support->uav.get();
		ID3D11UnorderedAccessView* dirtyUAV = lv.dirty->uav.get();
		ID3D11UnorderedAccessView* listsUAV = lv.lists->uav.get();
		ID3D11UnorderedAccessView* flagsUAV = lv.flags->uav.get();
		ID3D11Buffer* listsBuffer = lv.lists->resource.get();
		cb.BlurAxis = 2;
		voxelCB->Update(cb);
		context->CSSetConstantBuffers(0, 1, &cbPtr);

		// What the raster changed: this rebuild's occupancy (t0) against
		// the last one's (t4), still in the dead volume. Every rebuild,
		// dirty or not: it also keeps each brick's OCC bit in the flags.
		{
			context->CSSetShaderResources(0, 1, &occSRV);
			context->CSSetShaderResources(4, 1, &scratchSRV);
			context->CSSetUnorderedAccessViews(5, 1, &dirtyUAV, nullptr);
			context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);
			context->CSSetShader(voxelDiffCS, nullptr, 0);
			context->Dispatch(groups, groups, groups);
			context->CSSetUnorderedAccessViews(5, 1, &nullUAV, nullptr);
			context->CSSetUnorderedAccessViews(6, 1, &nullUAV, nullptr);
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetShaderResources(4, 1, &nullSRV);
		}
		// The dirty columns and their dilations -> dispatch args + lists.
		context->CSSetShaderResources(6, 1, &dirtySRV);
		context->CSSetUnorderedAccessViews(7, 1, &listsUAV, nullptr);
		context->CSSetShader(voxelDirtyColsCS, nullptr, 0);
		context->Dispatch(1, 1, 1);
		context->CSSetUnorderedAccessViews(7, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(6, 1, &nullSRV);
		context->CSSetShaderResources(7, 1, &listsSRV);
		// The flags stay bound (u6) through the field passes: their active-
		// brick gates read OCC/FIELD from it, Z writes FIELD, the flags pass
		// writes the mask. Unbound before the compaction reads it at t8.
		context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);

		// Seeds: occupancy + the shelter/sky maps -> scratch, on D0.
		ID3D11ShaderResourceView* mapSRVs[2] = {
			seedMaps ? heightBottomFiltered->srv.get() : nullptr,
			seedMaps ? objectSkyOpen->srv.get() : nullptr
		};
		context->CSSetShaderResources(1, 2, mapSRVs);
		context->CSSetShaderResources(0, 1, &occSRV);
		context->CSSetUnorderedAccessViews(0, 1, &scratchUAV, nullptr);
		context->CSSetShader(voxelSeedCS, nullptr, 0);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsSeed);
		ID3D11ShaderResourceView* nullMapSRVs[2] = { nullptr, nullptr };
		context->CSSetShaderResources(1, 2, nullMapSRVs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		// Every field pass reads the occupancy at t4: Z stops at the first
		// solid beneath, X and Y at the first solid beside.
		context->CSSetShaderResources(4, 1, &occSRV);
		// Z: scratch -> field, one thread per column, on D0; the raster's
		// surface heights at t9.
		ID3D11ShaderResourceView* heightSRV = lv.height->srv.get();
		context->CSSetShaderResources(0, 1, &scratchSRV);
		context->CSSetShaderResources(9, 1, &heightSRV);
		context->CSSetUnorderedAccessViews(0, 1, &fieldUAV, nullptr);
		context->CSSetShader(voxelBlurZCS, nullptr, 0);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsZ);
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetShaderResources(9, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		// X: field -> scratch, and its 1D distance -> support (u4), on D3.
		cb.BlurAxis = 0;
		voxelCB->Update(cb);
		context->CSSetShaderResources(0, 1, &fieldSRV);
		context->CSSetUnorderedAccessViews(0, 1, &scratchUAV, nullptr);
		context->CSSetUnorderedAccessViews(4, 1, &supportUAV, nullptr);
		context->CSSetShader(voxelBlurCS, nullptr, 0);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsX);
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetUnorderedAccessViews(4, 1, &nullUAV, nullptr);
		// Y: scratch -> field, reading support (t5) for the cap, on D2.
		cb.BlurAxis = 1;
		voxelCB->Update(cb);
		context->CSSetShaderResources(0, 1, &scratchSRV);
		context->CSSetShaderResources(5, 1, &supportSRV);
		context->CSSetUnorderedAccessViews(0, 1, &fieldUAV, nullptr);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsY);
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetShaderResources(5, 1, &nullSRV);
		context->CSSetShaderResources(4, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		// The brick flags on D3's bricks (kept between rebuilds)...
		context->CSSetShaderResources(3, 1, &fieldSRV);
		context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);
		context->CSSetShader(voxelBrickFlagsCS, nullptr, 0);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsFlags);
		context->CSSetUnorderedAccessViews(6, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(3, 1, &nullSRV);
		context->CSSetShaderResources(7, 1, &nullSRV);
		// ...then the draw list from every flag in reach. Binding the append
		// UAV with a zero initial count resets its counter; the count then
		// becomes the instance count.
		{
			const UINT zeroCount = 0;
			ID3D11UnorderedAccessView* listUAV = lv.bricks->uav.get();
			context->CSSetShaderResources(8, 1, &flagsSRV);
			context->CSSetUnorderedAccessViews(3, 1, &listUAV, &zeroCount);
			context->CSSetShader(voxelBrickListCS, nullptr, 0);
			const UINT brickGroups = lv.dim / 64;
			context->Dispatch(brickGroups, brickGroups, brickGroups);
			context->CSSetShaderResources(8, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(3, 1, &nullUAV, nullptr);
			context->CopyStructureCount(lv.drawArgs->resource.get(), 4, listUAV);
		}
		context->CSSetShader(nullptr, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &nullCB);
	}
	globals::profiler->EndPass();

	if (showVoxelSlice)
		UpdateVoxelSliceTexture();
}

void SnowDeformation::UpdateVoxelSliceTexture()
{
	const uint level = std::min((uint)std::max(voxelSliceLevel, 0), VoxelLevelsLive() - 1);
	const auto& lv = voxelLevels[level];
	if (!voxelSliceCS || !voxelSliceTexture || !voxelCB || !lv.valid || !lv.volume[lv.current] || !lv.field)
		return;
	auto* context = globals::d3d::context;
	VoxelVolumeCB cb{};
	FillVoxelCB(level, cb);
	voxelCB->Update(cb);
	ID3D11Buffer* cbPtr = voxelCB->CB();
	ID3D11ShaderResourceView* srv = lv.volume[lv.current]->srv.get();
	ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
	ID3D11UnorderedAccessView* uavs[2] = { nullptr, voxelSliceTexture->uav.get() };
	context->CSSetConstantBuffers(0, 1, &cbPtr);
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetShaderResources(3, 1, &fieldSRV);
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	context->CSSetShader(voxelSliceCS, nullptr, 0);
	// A far ring fills only a corner of the 256^2 slice; clear the rest.
	const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	if (lv.dim < kVoxelDim)
		context->ClearUnorderedAccessViewFloat(voxelSliceTexture->uav.get(), clearZero);
	context->Dispatch(lv.dim / 8, lv.dim / 8, 1);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetShaderResources(3, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &nullCB);
}

void SnowDeformation::DrawVoxelSnow()
{
	if (!settings.VolumeSnow || !voxelDrawCB || !voxelWrapSampler || !voxelShellVS || !voxelShellPS)
		return;
	const uint levels = VoxelLevelsLive();
	if (!voxelLevels[0].valid)
		return;
	auto* context = globals::d3d::context;

	// One block for the whole draw, as the patch does: rounded class at the
	// volume's depth, the window for the footprint reads, no coat, no relief.
	StaticsCB scb{};
	scb.ObjectsDepth = settings.VolumeSnowDepth;
	scb.RoundedDepth = settings.VolumeSnowDepth;
	scb.HeightWindowCenter = heightWindowCenter;
	scb.HeightHalfExtent = kHeightMapHalfExtent;
	scb.HasObjectTop = 1.0f;
	scb.ClassOverride = 1.0f;
	staticsCB->Update(scb);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);

	ID3D11SamplerState* wrap = voxelWrapSampler.get();
	context->PSSetSamplers(3, 1, &wrap);
	// What the skin pass unbinds and the patch rebinds: the march's depth
	// cap and SkinShadeSurface's footprint reads.
	ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->PSSetShaderResources(3, 1, &sceneDepthSRV);
	ID3D11ShaderResourceView* topSRV = (heightTopRaw[heightCurrent] && heightTopRaw[heightCurrent]->srv) ? heightTopRaw[heightCurrent]->srv.get() : nullptr;
	context->PSSetShaderResources(11, 1, &topSRV);
	ID3D11ShaderResourceView* skinDepthSRV = (heightSkinDepth && heightSkinDepth->srv) ? heightSkinDepth->srv.get() : nullptr;
	context->PSSetShaderResources(12, 1, &skinDepthSRV);

	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
	// Both faces: the PS keeps the back one by distance (see the shader).
	context->RSSetState(voxelRasterState.get());
	context->OMSetDepthStencilState(shellDepthState.get(), 0);
	context->IASetInputLayout(nullptr);
	ID3D11Buffer* nullVB = nullptr;
	UINT zero = 0;
	context->IASetVertexBuffers(0, 1, &nullVB, &zero, &zero);
	context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R16_UINT, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(voxelShellVS, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->PSSetShader(voxelShellPS, nullptr, 0);

	globals::profiler->BeginPass("SnowDeformation::VolumeSnow");
	ID3D11Buffer* cb2 = voxelDrawCB->CB();
	for (uint L = 0; L < levels; L++) {
		const auto& lv = voxelLevels[L];
		if (!lv.valid || !lv.field || !lv.bricks || !lv.drawArgs)
			continue;
		const float voxelSize = VoxelSizeForLevel(L);
		VoxelDrawCB d{};
		d.VoxOrigin = { lv.origin.x, lv.origin.y, lv.origin.z, 0 };
		d.VoxParams = { voxelSize, float(lv.dim), VoxelThresholdForLevel(L), std::clamp(settings.VolumeMarchStep, 0.25f, 2.0f) };
		// Hand-over bands: in over the finer level's outer band (none on
		// level 0: a band below zero reads as fully in), out over this one's.
		float inStart = -2.0f, inEnd = -1.0f, outStart = 0.0f, outEnd = 0.0f;
		const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
		const auto centre = VoxelLevelCentre(L);
		// w = the sub-cell skip switch.
		d.VoxCentre = { centre.x - eye.x, centre.y - eye.y, centre.z - eye.z, settings.VolumeSkipEmptyCells ? 1.0f : 0.0f };
		// w = the shading detail distance.
		const float detailDist = std::clamp(settings.VolumeDetailDistance, 100.0f, 8000.0f);
		d.VoxInnerCentre = { 0.0f, 0.0f, 0.0f, detailDist };
		if (L > 0) {
			VoxelReachBand(L - 1, inStart, inEnd);
			const auto inner = VoxelLevelCentre(L - 1);
			d.VoxInnerCentre = { inner.x - eye.x, inner.y - eye.y, inner.z - eye.z, detailDist };
		}
		VoxelReachBand(L, outStart, outEnd);
		d.VoxFade = { inStart, inEnd, outStart, outEnd };
		voxelDrawCB->Update(d);
		context->VSSetConstantBuffers(2, 1, &cb2);
		context->PSSetConstantBuffers(2, 1, &cb2);
		ID3D11ShaderResourceView* bricksSRV = lv.bricks->srv.get();
		context->VSSetShaderResources(41, 1, &bricksSRV);
		ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
		context->PSSetShaderResources(42, 1, &fieldSRV);
		context->DrawInstancedIndirect(lv.drawArgs->resource.get(), 0);
	}
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetShaderResources(41, 1, &nullSRV);
	context->PSSetShaderResources(42, 1, &nullSRV);
	context->PSSetShaderResources(3, 1, &nullSRV);
	context->PSSetSamplers(3, 1, &nullSampler);
	context->VSSetConstantBuffers(2, 1, &nullCB);
	context->PSSetConstantBuffers(2, 1, &nullCB);
	context->RSSetState(savedRaster.get());
}
