#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "Utils/D3D.h"

bool SnowDeformation::EnsureVoxelResources()
{
	const uint levels = VoxelLevelsLive();
	bool levelsReady = true;
	for (uint i = 0; i < levels; i++) {
		const auto& lv = voxelLevels[i];
		const auto want = VoxelDimsForLevel(i);
		levelsReady = levelsReady && lv.dims.x == want.x && lv.dims.y == want.y && lv.dims.z == want.z && lv.volume[0] && lv.volume[1] && lv.field && lv.heightMap && lv.heightMapUnder && lv.bricks && lv.drawArgs && lv.dirty && lv.flags && lv.lists;
	}
	const auto wantMax = VoxelDimsMax();
	if (levelsReady && voxelSupport && voxelSupportDims.x == wantMax.x && voxelSupportDims.y == wantMax.y && voxelSupportDims.z == wantMax.z && voxelSliceTexture && voxelCB && voxelDrawCB && voxelRasterState && voxelWrapSampler &&
		voxelVS && voxelGS && voxelPS && voxelScrollCS && voxelSliceCS && voxelSeedCS && voxelBlurZCS && voxelBlurCS && voxelBrickListCS &&
		voxelOccFlagsCS && voxelDecayCS && voxelDirtyColsCS && voxelBrickFlagsCS)
		return true;
	if (voxelShadersFailed)
		return false;
	LoadTraceScope _loadTrace(this, "Volume: EnsureVoxelResources");
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	static int r8TypedLoads = -1;
	if (r8TypedLoads < 0) {
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{};
		support2.InFormat = DXGI_FORMAT_R8_UNORM;
		const bool ok = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2))) &&
		                (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
		r8TypedLoads = ok ? 1 : 0;
		if (!ok)
			logger::warn("[SNOW DEFORMATION] Voxel volume disabled: no typed UAV loads for R8_UNORM on this GPU");
	}
	if (!r8TypedLoads) {
		voxelShadersFailed = true;
		return false;
	}

	const float clearZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	auto makeVolume = [&](const std::string& a_name, DirectX::XMUINT3 a_dims) {
		D3D11_TEXTURE3D_DESC volDesc{
			.Width = a_dims.x,
			.Height = a_dims.y,
			.Depth = a_dims.z,
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
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = a_dims.z }
		};
		auto* tex = new Texture3D(volDesc, a_name.c_str());
		tex->CreateSRV(volSrvDesc);
		tex->CreateUAV(volUavDesc);
		context->ClearUnorderedAccessViewFloat(tex->uav.get(), clearZero);
		return tex;
	};
	for (uint i = 0; i < levels; i++) {
		auto& lv = voxelLevels[i];
		const auto dim = VoxelDimsForLevel(i);
		if (lv.dims.x != dim.x || lv.dims.y != dim.y || lv.dims.z != dim.z) {
			for (int p = 0; p < 2; p++) {
				delete lv.volume[p];
				lv.volume[p] = nullptr;
			}
			delete lv.field;
			lv.field = nullptr;
			delete lv.heightMap;
			lv.heightMap = nullptr;
			delete lv.heightMapUnder;
			lv.heightMapUnder = nullptr;
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
			lv.dims = dim;
			lv.valid = false;
		}
		for (int p = 0; p < 2; p++)
			if (!lv.volume[p])
				lv.volume[p] = makeVolume(std::format("SnowDeformation::VoxelVolume{}{}", i, p), dim);
		if (!lv.field)
			lv.field = makeVolume(std::format("SnowDeformation::VoxelField{}", i), dim);
		if (!lv.heightMap) {
			const uint hdim = dim.x * 4;
			D3D11_TEXTURE2D_DESC desc{
				.Width = hdim,
				.Height = hdim,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_UINT,
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
			lv.heightMap = new Texture2D(desc, std::format("SnowDeformation::VoxelHeightMap{}", i).c_str());
			lv.heightMap->CreateSRV(srvDesc);
			lv.heightMap->CreateUAV(uavDesc);
			const UINT zeros[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(lv.heightMap->uav.get(), zeros);
		}
		if (!lv.heightMapUnder) {
			const uint hdim = dim.x * 2;
			D3D11_TEXTURE2D_DESC desc{
				.Width = hdim,
				.Height = hdim,
				.MipLevels = 1,
				.ArraySize = kVoxelUnderLayers,
				.Format = DXGI_FORMAT_R32_UINT,
				.SampleDesc = { .Count = 1 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
			};
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
				.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = kVoxelUnderLayers }
			};
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
				.Format = desc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
				.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = kVoxelUnderLayers }
			};
			lv.heightMapUnder = new Texture2D(desc, std::format("SnowDeformation::VoxelHeightMapUnder{}", i).c_str());
			lv.heightMapUnder->CreateSRV(srvDesc);
			lv.heightMapUnder->CreateUAV(uavDesc);
			const UINT zeros[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(lv.heightMapUnder->uav.get(), zeros);
		}
		if (!lv.bricks) {
			const uint capacity = (dim.x / 8) * (dim.y / 8) * (dim.z / 8);
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
			const uint32_t args[4] = { 36, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA init{ args, 0, 0 };
			D3D11_BUFFER_DESC desc{};
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.ByteWidth = sizeof(args);
			desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
			lv.drawArgs = new Buffer(desc, &init, std::format("SnowDeformation::VoxelDrawArgs{}", i).c_str());
		}
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
			const uint brickBytes = (dim.x / 8) * (dim.y / 8) * (dim.z / 8) * sizeof(uint32_t);
			if (!lv.dirty)
				lv.dirty = makeRaw(std::format("SnowDeformation::VoxelDirty{}", i), brickBytes, false);
			if (!lv.flags)
				lv.flags = makeRaw(std::format("SnowDeformation::VoxelBrickFlags{}", i), brickBytes, false);
			if (!lv.lists)
				lv.lists = makeRaw(std::format("SnowDeformation::VoxelDirtyLists{}", i), kVoxelListBytes, true);
		}
		lv.valid = false;
	}
	if (voxelSupport && (voxelSupportDims.x != wantMax.x || voxelSupportDims.y != wantMax.y || voxelSupportDims.z != wantMax.z)) {
		delete voxelSupport;
		voxelSupport = nullptr;
	}
	if (!voxelSupport) {
		voxelSupport = makeVolume("SnowDeformation::VoxelSupport", wantMax);
		voxelSupportDims = wantMax;
	}
	if (!voxelSliceTexture) {
		D3D11_TEXTURE2D_DESC desc{
			.Width = kVoxelDimWide,
			.Height = kVoxelDimWide,
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
		context->ClearUnorderedAccessViewFloat(voxelSliceTexture->uav.get(), clearZero);
	}
	if (!voxelCB)
		voxelCB = new ConstantBuffer(ConstantBufferDesc<VoxelVolumeCB>(), "SnowDeformation::VoxelVolumeCB");
	if (!voxelDrawCB)
		voxelDrawCB = new ConstantBuffer(ConstantBufferDesc<VoxelDrawCB>(), "SnowDeformation::VoxelDrawCB");
	if (!voxelWrapSampler) {
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
	compileCS(voxelOccFlagsCS, "VoxelOccFlagsCS", "SnowDeformation::VoxelOccFlagsCS");
	compileCS(voxelDecayCS, "VoxelDecayCS", "SnowDeformation::VoxelDecayCS");
	compileCS(voxelDirtyColsCS, "VoxelDirtyColsCS", "SnowDeformation::VoxelDirtyColsCS");
	compileCS(voxelBrickFlagsCS, "VoxelBrickFlagsCS", "SnowDeformation::VoxelBrickFlagsCS");
	if (!voxelVS || !voxelGS || !voxelPS || !voxelScrollCS || !voxelSliceCS || !voxelSeedCS || !voxelBlurZCS || !voxelBlurCS || !voxelBrickListCS ||
		!voxelOccFlagsCS || !voxelDecayCS || !voxelDirtyColsCS || !voxelBrickFlagsCS) {
		voxelShadersFailed = true;
		logger::warn("[SNOW DEFORMATION] Voxel volume disabled (shader compilation failed: VS {} GS {} PS {} ScrollCS {} SliceCS {} SeedCS {} BlurZCS {} BlurCS {} BrickListCS {} OccFlagsCS {} DecayCS {} DirtyColsCS {} BrickFlagsCS {})",
			voxelVS ? "ok" : "FAILED", voxelGS ? "ok" : "FAILED", voxelPS ? "ok" : "FAILED",
			voxelScrollCS ? "ok" : "FAILED", voxelSliceCS ? "ok" : "FAILED",
			voxelSeedCS ? "ok" : "FAILED", voxelBlurZCS ? "ok" : "FAILED", voxelBlurCS ? "ok" : "FAILED", voxelBrickListCS ? "ok" : "FAILED",
			voxelOccFlagsCS ? "ok" : "FAILED", voxelDecayCS ? "ok" : "FAILED", voxelDirtyColsCS ? "ok" : "FAILED", voxelBrickFlagsCS ? "ok" : "FAILED");
		return false;
	}
	return true;
}

DirectX::XMUINT3 SnowDeformation::VoxelDimsMax() const
{
	const int shape = std::clamp(settings.VolumeRingShape, 0, 2);
	const uint xy = shape == 0 ? kVoxelDim : kVoxelDimWide;
	const uint z = shape == 2 ? kVoxelDimFlatZ : kVoxelDim;
	return { xy, xy, z };
}

DirectX::XMUINT3 SnowDeformation::VoxelDimsForLevel(uint a_level) const
{
	const uint fine = (uint)std::clamp(settings.VolumeFineLevels, 1, (int)kVoxelMaxLevels);
	auto d = VoxelDimsMax();
	if (a_level >= fine)
		d = { d.x / 2, d.y / 2, d.z / 2 };
	return d;
}

float SnowDeformation::VoxelSizeForLevel(uint a_level) const
{
	const uint fine = (uint)std::clamp(settings.VolumeFineLevels, 1, (int)kVoxelMaxLevels);
	return VoxelSizeLive() * float(1u << a_level) * (a_level < fine ? 1.0f : 2.0f);
}

DirectX::XMFLOAT3 SnowDeformation::VoxelExtentForLevel(uint a_level) const
{
	const auto d = VoxelDimsMax();
	const float unit = VoxelSizeLive() * float(1u << a_level);
	return { d.x * unit, d.y * unit, d.z * unit };
}

void SnowDeformation::VoxelReachBand(uint a_level, float& a_start, float& a_end) const
{
	const uint fine = (uint)std::clamp(settings.VolumeFineLevels, 1, (int)kVoxelMaxLevels);
	const float frac = a_level < fine ? kVoxelReachFrac : kVoxelReachFracFar;
	a_end = VoxelExtentForLevel(a_level).x * 0.5f * frac;
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
	a_cb.Dims[0] = (int)lv.dims.x;
	a_cb.Dims[1] = (int)lv.dims.y;
	a_cb.Dims[2] = (int)lv.dims.z;
	a_cb.Dims[3] = 0;
	a_cb.SliceAxis = std::clamp(voxelSliceAxis, 0, 2);
	a_cb.SliceXray = voxelSliceXray ? 1 : 0;
	a_cb.SliceSource = std::clamp(voxelSliceSource, 0, 2);
	{
		const float along = a_cb.SliceAxis == 0 ? eye.z : (a_cb.SliceAxis == 1 ? eye.y : eye.x);
		const int originAlong = a_cb.SliceAxis == 0 ? lv.origin.z : (a_cb.SliceAxis == 1 ? lv.origin.y : lv.origin.x);
		const int fixedDim = a_cb.SliceAxis == 0 ? (int)lv.dims.z : (a_cb.SliceAxis == 1 ? (int)lv.dims.y : (int)lv.dims.x);
		a_cb.SliceIndex = std::clamp((int)std::floor((along + voxelSliceOffset) / voxelSize) - originAlong, 0, fixedDim - 1);
	}
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	a_cb.HeightWindowCenter = heightWindowCenter;
	a_cb.HeightHalfExtent = seedMaps ? ObjectRasterHalfExtent() : 0.0f;
	a_cb.ShelterDust = kVoxelShelterDust;
	a_cb.DepthVox = std::max(settings.VolumeSnowDepth, 0.0f) / voxelSize;
	a_cb.FieldThreshold = VoxelThresholdForLevel(a_level);
	const float roundU = std::clamp(settings.VolumeSnowRounding, 0.0f, 32.0f);
	const float roundVox = roundU / voxelSize;
	const float airWeight = std::clamp((roundVox - 0.5f) / 1.5f, 0.0f, 1.0f);
	a_cb.RoundSigma = std::max(roundVox, airWeight > 0.0f ? 0.5f : 1.0f);
	a_cb.EdgeParams[0] = 0.0f;
	a_cb.EdgeParams[1] = kVoxelEdgeNoiseCell;
	a_cb.EdgeParams[2] = std::log(2.0f) / std::max(roundVox, 2.0f);
	a_cb.EdgeParams[3] = std::clamp(settings.VolumeSnowOverhang, 0.0f, 16.0f);
	a_cb.LipParams[0] = airWeight;
	a_cb.LipParams[1] = a_cb.LipParams[2] = a_cb.LipParams[3] = 0.0f;
	a_cb.OverhangVox = (float)std::clamp((int)std::lround(std::clamp(settings.VolumeSnowOverhang, 0.0f, 16.0f) / voxelSize), 0, 7);
	a_cb.HeadroomVox = (float)std::max(1, (int)std::lround(kVoxelHeadroomUnits / voxelSize));
	a_cb.SlopeMinNz = std::cos(DirectX::XMConvertToRadians(std::clamp(settings.VolumeSnowMaxSlopeDeg, 0.0f, 90.0f)));
	a_cb.SkyStrength = std::clamp(settings.VolumeSkyExposurePct / 100.0f, 0.0f, 1.0f);
	const uint32_t period = VoxelLevelPeriod(a_level);
	const uint32_t tickEvery = std::max(1u, (uint32_t)std::lround(voxelMemorySeconds * 60.0f / 15.0f) / period);
	a_cb.Decay = ((voxelFrame / period) % tickEvery == 0) ? 1.0f : 0.0f;
	const auto centre = VoxelLevelCentre(a_level);
	a_cb.CentreVox[0] = centre.x / voxelSize;
	a_cb.CentreVox[1] = centre.y / voxelSize;
	a_cb.CentreVox[2] = centre.z / voxelSize;
	float bandStart = 0.0f, bandEnd = 0.0f;
	VoxelReachBand(a_level, bandStart, bandEnd);
	a_cb.CentreVox[3] = bandEnd / voxelSize;
	a_cb.ForceDirty = 0.0f;
}

uint32_t SnowDeformation::VoxelLevelPeriod(uint a_level) const
{
	return 1u << a_level;
}

float SnowDeformation::VoxelThresholdForLevel(uint a_level) const
{
	const float coverage = std::clamp(settings.VolumeSnowCoverage, 0.02f, 0.5f);
	const float roundVox = std::clamp(settings.VolumeSnowRounding, 0.0f, 32.0f) / VoxelSizeForLevel(a_level);
	const float airWeight = std::clamp((roundVox - 0.5f) / 1.5f, 0.0f, 1.0f);
	const float gain = std::min(1.0f + 7.0f * (1.0f - airWeight), 0.5f / coverage);
	return coverage * gain;
}

DirectX::XMFLOAT3 SnowDeformation::VoxelLevelCentre(uint a_level) const
{
	const auto& o = voxelLevels[a_level].origin;
	const auto& d = voxelLevels[a_level].dims;
	const float voxelSize = VoxelSizeForLevel(a_level);
	return { (o.x + d.x * 0.5f) * voxelSize, (o.y + d.y * 0.5f) * voxelSize, (o.z + d.z * 0.5f) * voxelSize };
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

	DirectX::XMINT3 levelDelta[kVoxelMaxLevels]{};
	bool levelClear[kVoxelMaxLevels]{};
	bool anyUpdated = false;
	bool anyDue = false;
	voxelIdleBlockers = 0;
	auto mix = [](uint64_t& a_h, const void* a_p, size_t a_n) {
		const auto* b = static_cast<const uint8_t*>(a_p);
		for (size_t i = 0; i < a_n; i++) {
			a_h ^= b[i];
			a_h *= 1099511628211ull;
		}
	};
	uint64_t captureHash = 0;
	for (uint32_t ci = 0; ci < a_captureCount; ci++) {
		const auto& cap = capturedStatics[ci];
		uint64_t e = 1469598103934665603ull;
		const void* g = cap.geometry.get();
		mix(e, &g, sizeof(g));
		mix(e, &cap.world.translate, sizeof(cap.world.translate));
		mix(e, &cap.road, sizeof(cap.road));
		captureHash += e;
	}
	const bool empty = a_captureCount == 0;
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		const uint32_t period = VoxelLevelPeriod(L);
		const uint32_t phase = period > 1 ? period / 2 : 0;
		lv.skipped = false;
		lv.updated = !lv.valid || (voxelFrame % period) == phase;
		if (!lv.updated)
			continue;
		anyDue = true;
		const float voxelSize = VoxelSizeForLevel(L);
		const auto extent = VoxelExtentForLevel(L);
		const float half = extent.x * 0.5f;
		const float halfZ = extent.z * 0.5f;
		const float ahead = std::clamp(settings.VolumeForwardBias, 0.0f, 0.45f) * 2.0f * half;
		const float brickUnits = 8.0f * voxelSize;
		const float snapBias = 4.0f * voxelSize;
		const DirectX::XMINT3 origin{
			(int)std::floor((eye.x + fwdX * ahead - half + snapBias) / brickUnits) * 8,
			(int)std::floor((eye.y + fwdY * ahead - half + snapBias) / brickUnits) * 8,
			(int)std::floor((eye.z - std::clamp(settings.VolumeVerticalBias, -0.3f, 0.3f) * 2.0f * halfZ - halfZ + snapBias) / brickUnits) * 8
		};
		VoxelVolumeCB in{};
		FillVoxelCB(L, in);
		in.OriginVox = {};
		in.ScrollDelta = {};
		in.Decay = 0.0f;
		in.SliceAxis = in.SliceIndex = in.SliceXray = in.SliceSource = in.BlurAxis = 0;
		in.CentreVox[0] = in.CentreVox[1] = in.CentreVox[2] = in.CentreVox[3] = 0.0f;
		in.ForceDirty = 0.0f;
		if (empty) {
			in.HeightWindowCenter = {};
			in.HeightHalfExtent = 0.0f;
		}
		uint64_t cbHash = 1469598103934665603ull;
		mix(cbHash, &in, sizeof(in));
		mix(cbHash, &voxelMemorySeconds, sizeof(voxelMemorySeconds));
		mix(cbHash, &settings.VolumeStaggeredCapture, sizeof(settings.VolumeStaggeredCapture));
		mix(cbHash, &settings.VolumeDirtyBricks, sizeof(settings.VolumeDirtyBricks));
		const uint64_t exclusionHash = empty ? 0ull : exclusionListHash;
		const bool originSame = empty || (origin.x == lv.origin.x && origin.y == lv.origin.y && origin.z == lv.origin.z);
		uint32_t blockers = 0;
		if (!lv.valid)
			blockers |= 64u;
		if (!originSame)
			blockers |= 1u;
		if (captureHash != lv.captureHashLast || a_captureCount != lv.captureCountLast)
			blockers |= 2u;
		if (exclusionHash != lv.exclusionHashLast)
			blockers |= 4u;
		if (cbHash != lv.cbHashLast)
			blockers |= 8u;
		lv.captureHashLast = captureHash;
		lv.captureCountLast = a_captureCount;
		lv.exclusionHashLast = exclusionHash;
		lv.cbHashLast = cbHash;
		lv.stableRebuilds = blockers == 0 ? lv.stableRebuilds + 1 : 0;
		const uint32_t tickEvery = std::max(1u, (uint32_t)std::lround(voxelMemorySeconds * 60.0f / 15.0f) / period);
		const uint32_t rasterPeriod = settings.VolumeStaggeredCapture ? std::max(1u, 8u >> L) : 1u;
		const uint32_t settle = rasterPeriod + 15u * tickEvery + 1u;
		if (blockers == 0 && lv.stableRebuilds < settle)
			blockers |= 16u;
		if (debugForceVolumeRebuild)
			blockers |= 32u;
		voxelIdleBlockers |= blockers;
		if (blockers == 0) {
			lv.updated = false;
			lv.skipped = true;
			continue;
		}
		levelDelta[L] = { origin.x - lv.origin.x, origin.y - lv.origin.y, origin.z - lv.origin.z };
		levelClear[L] = !lv.valid;
		lv.cleared = levelClear[L];
		lv.origin = origin;
		lv.valid = true;
		anyUpdated = true;
	}
	voxelIdleSkipped = anyDue && !anyUpdated;
	voxelSkipTallyFrames++;
	if (voxelIdleSkipped)
		voxelSkipTallySkipped++;
	if (voxelSkipTallyFrames >= 300) {
		voxelSkipRate = (float)voxelSkipTallySkipped / (float)voxelSkipTallyFrames;
		voxelSkipTallyFrames = 0;
		voxelSkipTallySkipped = 0;
	}
	if (!anyUpdated) {
		globals::profiler->MarkPassSkipped("SnowDeformation::Volume - Bake");
	}

	if (anyUpdated)
		globals::profiler->BeginPass("SnowDeformation::Volume - Bake");
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		if (!lv.updated)
			continue;
		const UINT gx = lv.dims.x / 8, gy = lv.dims.y / 8, gz = lv.dims.z / 8;
		const auto& delta = levelDelta[L];
		const bool clearAll = levelClear[L];
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		cb.OriginVox.w = clearAll ? 1 : 0;
		cb.ScrollDelta = { delta.x, delta.y, delta.z, 0 };
		voxelCB->Update(cb);
		{
			const UINT zeros[4] = { 0, 0, 0, 0 };
			context->ClearUnorderedAccessViewUint(lv.dirty->uav.get(), zeros);
			const uint32_t zb = lv.dims.z / 8;
			const uint32_t header[16] = { 0, zb, 1, 0, 1, 1, 0, zb, 1, 0, zb, 1, 0, zb, 1, 0 };
			D3D11_BOX box{ 0, 0, 0, kVoxelListHeaderBytes, 1, 1 };
			context->UpdateSubresource(lv.lists->resource.get(), 0, &box, header, 0, 0);
		}

		{
			ID3D11UnorderedAccessView* volumeUAV = lv.volume[0]->uav.get();
			ID3D11UnorderedAccessView* dirtyUAV = lv.dirty->uav.get();
			ID3D11UnorderedAccessView* flagsUAV = lv.flags->uav.get();
			ID3D11UnorderedAccessView* heightMapUAV = lv.heightMap->uav.get();
			ID3D11UnorderedAccessView* heightMapUnderUAV = lv.heightMapUnder->uav.get();
			context->CSSetConstantBuffers(0, 1, &cbPtr);
			context->CSSetUnorderedAccessViews(0, 1, &volumeUAV, nullptr);
			context->CSSetUnorderedAccessViews(5, 1, &dirtyUAV, nullptr);
			context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);
			context->CSSetUnorderedAccessViews(8, 1, &heightMapUAV, nullptr);
			context->CSSetUnorderedAccessViews(9, 1, &heightMapUnderUAV, nullptr);
			context->CSSetShader(voxelScrollCS, nullptr, 0);
			const bool whole = clearAll || std::abs(delta.x) >= (int)lv.dims.x || std::abs(delta.y) >= (int)lv.dims.y || std::abs(delta.z) >= (int)lv.dims.z;
			if (whole) {
				cb.ScrollDelta.w = 3;
				voxelCB->Update(cb);
				context->Dispatch(gx, gy, gz);
			} else {
				const int d[3] = { delta.x, delta.y, delta.z };
				for (int axis = 0; axis < 3; axis++) {
					if (d[axis] == 0)
						continue;
					cb.ScrollDelta.w = axis;
					voxelCB->Update(cb);
					const UINT slab = (UINT)std::abs(d[axis]) / 8;
					context->Dispatch(slab, axis == 0 ? gy : gx, axis == 2 ? gy : gz);
				}
			}
			if (cb.Decay > 0.5f) {
				const bool countHere = counting && L == 0;
				ID3D11UnorderedAccessView* countUAV = countHere ? voxelCountUAV.get() : nullptr;
				if (countHere) {
					const UINT zeros[4] = { 0, 0, 0, 0 };
					context->ClearUnorderedAccessViewUint(voxelCountUAV.get(), zeros);
				}
				context->CSSetUnorderedAccessViews(2, 1, &countUAV, nullptr);
				context->CSSetShader(voxelDecayCS, nullptr, 0);
				context->Dispatch(gx, gy, gz);
				context->CSSetUnorderedAccessViews(2, 1, &nullUAV, nullptr);
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
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetUnorderedAccessViews(5, 1, &nullUAV, nullptr);
			context->CSSetUnorderedAccessViews(6, 1, &nullUAV, nullptr);
			context->CSSetUnorderedAccessViews(8, 1, &nullUAV, nullptr);
			context->CSSetUnorderedAccessViews(9, 1, &nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &nullCB);
		}
	}
	if (anyUpdated)
		globals::profiler->EndPass();

	if (anyUpdated)
		globals::profiler->BeginPass("SnowDeformation::Volume - Bake");
	if (anyUpdated) {
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
			D3D11_VIEWPORT viewports[2] = {
				{ 0.0f, 0.0f, float(lv.dims.x), float(lv.dims.x), 0.0f, 1.0f },
				{ 0.0f, 0.0f, float(lv.dims.x * 4), float(lv.dims.x * 4), 0.0f, 1.0f }
			};
			context->RSSetViewports(2, viewports);
			ID3D11UnorderedAccessView* rasterUAVs[5] = { lv.volume[0]->uav.get(), lv.heightMapUnder->uav.get(), lv.heightMap->uav.get(), lv.dirty->uav.get(), lv.flags->uav.get() };
			context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 5, rasterUAVs, nullptr);
			const uint32_t rasterPeriod = (settings.VolumeStaggeredCapture && !lv.cleared) ? std::max(1u, 8u >> L) : 1u;

			for (uint32_t ci = 0; ci < a_captureCount; ci++) {
				if (rasterPeriod > 1 && ((ci + lv.rebuilds) % rasterPeriod) != 0)
					continue;
				const auto& cap = capturedStatics[ci];
				if (cap.road)
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

			ID3D11UnorderedAccessView* nullRasterUAVs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
			context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 5, nullRasterUAVs, nullptr);
		}
		context->GSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* restoreVSCB0 = savedVSCB0.get();
		context->VSSetConstantBuffers(0, 1, &restoreVSCB0);
		context->GSSetConstantBuffers(0, 1, &nullCB);
		context->PSSetConstantBuffers(0, 1, &nullCB);
		context->RSSetState(savedRaster.get());
		globals::profiler->EndPass();
	}

	if (anyUpdated)
		globals::profiler->BeginPass("SnowDeformation::Volume - Bake");
	const bool seedMaps = heightBottomFiltered && heightBottomFiltered->srv && objectSkyOpen && objectSkyOpen->srv;
	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		if (!lv.updated)
			continue;
		const UINT gx = lv.dims.x / 8, gy = lv.dims.y / 8, gz = lv.dims.z / 8;
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		lv.rebuilds++;
		const bool forceAll = !settings.VolumeDirtyBricks || (lv.rebuilds % kVoxelFullRefreshRebuilds) == 0;
		cb.ForceDirty = forceAll ? 1.0f : 0.0f;
		Texture3D* scratch = lv.volume[1];
		Texture3D* occupancy = lv.volume[0];
		ID3D11ShaderResourceView* occSRV = occupancy->srv.get();
		ID3D11ShaderResourceView* scratchSRV = scratch->srv.get();
		ID3D11ShaderResourceView* fieldSRV = lv.field->srv.get();
		ID3D11ShaderResourceView* supportSRV = voxelSupport->srv.get();
		ID3D11ShaderResourceView* listsSRV = lv.lists->srv.get();
		ID3D11ShaderResourceView* dirtySRV = lv.dirty->srv.get();
		ID3D11UnorderedAccessView* scratchUAV = scratch->uav.get();
		ID3D11UnorderedAccessView* fieldUAV = lv.field->uav.get();
		ID3D11UnorderedAccessView* supportUAV = voxelSupport->uav.get();
		ID3D11UnorderedAccessView* listsUAV = lv.lists->uav.get();
		ID3D11UnorderedAccessView* flagsUAV = lv.flags->uav.get();
		ID3D11Buffer* listsBuffer = lv.lists->resource.get();
		cb.BlurAxis = 2;
		voxelCB->Update(cb);
		context->CSSetConstantBuffers(0, 1, &cbPtr);

		if (forceAll) {
			context->CSSetShaderResources(0, 1, &occSRV);
			context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);
			context->CSSetShader(voxelOccFlagsCS, nullptr, 0);
			context->Dispatch(gx, gy, gz);
			context->CSSetUnorderedAccessViews(6, 1, &nullUAV, nullptr);
			context->CSSetShaderResources(0, 1, &nullSRV);
		}
		context->CSSetShaderResources(6, 1, &dirtySRV);
		context->CSSetUnorderedAccessViews(7, 1, &listsUAV, nullptr);
		context->CSSetShader(voxelDirtyColsCS, nullptr, 0);
		context->Dispatch((gx + 31) / 32, (gy + 31) / 32, 1);
		context->CSSetUnorderedAccessViews(7, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(6, 1, &nullSRV);
		context->CSSetShaderResources(7, 1, &listsSRV);
		context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);

		ID3D11ShaderResourceView* heightMapSRVs[2] = { lv.heightMap->srv.get(), lv.heightMapUnder->srv.get() };
		context->CSSetShaderResources(10, 2, heightMapSRVs);
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
		context->CSSetShaderResources(4, 1, &occSRV);
		context->CSSetShaderResources(0, 1, &scratchSRV);
		context->CSSetUnorderedAccessViews(0, 1, &fieldUAV, nullptr);
		context->CSSetShader(voxelBlurZCS, nullptr, 0);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsZ);
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
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
		cb.BlurAxis = 1;
		voxelCB->Update(cb);
		context->CSSetShaderResources(0, 1, &scratchSRV);
		context->CSSetShaderResources(5, 1, &supportSRV);
		context->CSSetUnorderedAccessViews(0, 1, &fieldUAV, nullptr);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsY);
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetShaderResources(5, 1, &nullSRV);
		context->CSSetShaderResources(4, 1, &nullSRV);
		ID3D11ShaderResourceView* nullHeightSRVs[2] = { nullptr, nullptr };
		context->CSSetShaderResources(10, 2, nullHeightSRVs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(3, 1, &fieldSRV);
		context->CSSetUnorderedAccessViews(6, 1, &flagsUAV, nullptr);
		context->CSSetShader(voxelBrickFlagsCS, nullptr, 0);
		context->DispatchIndirect(listsBuffer, kVoxelListArgsFlags);
		context->CSSetUnorderedAccessViews(6, 1, &nullUAV, nullptr);
		context->CSSetShaderResources(3, 1, &nullSRV);
		context->CSSetShaderResources(7, 1, &nullSRV);
		context->CSSetShader(nullptr, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &nullCB);
	}
	if (anyUpdated)
		globals::profiler->EndPass();

	for (uint L = 0; L < levels; L++) {
		auto& lv = voxelLevels[L];
		if (!lv.updated && !lv.skipped)
			continue;
		VoxelVolumeCB cb{};
		FillVoxelCB(L, cb);
		voxelCB->Update(cb);
		context->CSSetConstantBuffers(0, 1, &cbPtr);
		const UINT zeroCount = 0;
		ID3D11UnorderedAccessView* listUAV = lv.bricks->uav.get();
		ID3D11ShaderResourceView* flagsSRV = lv.flags->srv.get();
		context->CSSetShaderResources(8, 1, &flagsSRV);
		context->CSSetUnorderedAccessViews(3, 1, &listUAV, &zeroCount);
		context->CSSetShader(voxelBrickListCS, nullptr, 0);
		context->Dispatch(std::max(1u, lv.dims.x / 64), std::max(1u, lv.dims.y / 64), std::max(1u, lv.dims.z / 64));
		context->CSSetShaderResources(8, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(3, 1, &nullUAV, nullptr);
		context->CopyStructureCount(lv.drawArgs->resource.get(), 4, listUAV);
		context->CSSetShader(nullptr, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &nullCB);
	}

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
	context->Dispatch(kVoxelDimWide / 8, kVoxelDimWide / 8, 1);

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11UnorderedAccessView* nullUAVs[2] = { nullptr, nullptr };
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 1, &nullSRV);
	context->CSSetShaderResources(3, 1, &nullSRV);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &nullCB);
}

bool SnowDeformation::EnsureVoxelStatsQueries()
{
	if (voxelStatsQuery[kShellStatsRing - 1][1])
		return true;
	auto device = globals::d3d::device;
	for (int ring = 0; ring < kShellStatsRing; ++ring) {
		for (int kind = 0; kind < 2; ++kind) {
			D3D11_QUERY_DESC desc{};
			desc.Query = kind == 0 ? D3D11_QUERY_PIPELINE_STATISTICS : D3D11_QUERY_OCCLUSION;
			if (FAILED(device->CreateQuery(&desc, voxelStatsQuery[ring][kind].put()))) {
				logger::error("SnowDeformation: volume statistics query creation failed");
				for (auto& r : voxelStatsQuery)
					for (auto& q : r)
						q = nullptr;
				return false;
			}
		}
	}
	return true;
}

void SnowDeformation::ReadVoxelStatsQueries(ID3D11DeviceContext* a_context)
{
	const int ring = (voxelStatsRing + 1) % kShellStatsRing;
	if (!voxelStatsIssued[ring])
		return;
	D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
	UINT64 samples = 0;
	if (a_context->GetData(voxelStatsQuery[ring][0].get(), &stats, sizeof(stats), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
		a_context->GetData(voxelStatsQuery[ring][1].get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
		return;
	voxelStatsLast.vsInvocations = stats.VSInvocations;
	voxelStatsLast.psInvocations = stats.PSInvocations;
	voxelStatsLast.rasterizedPrimitives = stats.CInvocations;
	voxelStatsLast.samplesPassed = samples;
	voxelStatsLast.valid = true;
	voxelStatsIssued[ring] = false;
}

void SnowDeformation::DrawVoxelSnow()
{
	if (!settings.VolumeSnow || !voxelDrawCB || !voxelWrapSampler || !voxelShellVS || !voxelShellPS)
		return;
	const uint levels = VoxelLevelsLive();
	if (!voxelLevels[0].valid)
		return;
	auto* context = globals::d3d::context;

	StaticsCB scb{};
	scb.ObjectsDepth = settings.VolumeSnowDepth;
	scb.RoundedDepth = settings.VolumeSnowDepth;
	scb.HeightWindowCenter = heightWindowCenter;
	scb.HeightHalfExtent = ObjectRasterHalfExtent();
	scb.HasObjectTop = 1.0f;
	scb.ClassOverride = 1.0f;
	staticsCB->Update(scb);
	ID3D11Buffer* cb1 = staticsCB->CB();
	context->VSSetConstantBuffers(1, 1, &cb1);
	context->PSSetConstantBuffers(1, 1, &cb1);

	ID3D11SamplerState* wrap = voxelWrapSampler.get();
	context->PSSetSamplers(3, 1, &wrap);
	ID3D11ShaderResourceView* sceneDepthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->PSSetShaderResources(3, 1, &sceneDepthSRV);
	ID3D11ShaderResourceView* topSRV = (heightTopRaw[heightCurrent] && heightTopRaw[heightCurrent]->srv) ? heightTopRaw[heightCurrent]->srv.get() : nullptr;
	context->PSSetShaderResources(11, 1, &topSRV);
	ID3D11ShaderResourceView* skinDepthSRV = (heightSkinDepth && heightSkinDepth->srv) ? heightSkinDepth->srv.get() : nullptr;
	context->PSSetShaderResources(12, 1, &skinDepthSRV);
	ID3D11ShaderResourceView* coneSRV = (objectSnowCone && objectSnowCone->srv) ? objectSnowCone->srv.get() : nullptr;
	context->PSSetShaderResources(13, 1, &coneSRV);
	BindPrintReaders(context);

	winrt::com_ptr<ID3D11RasterizerState> savedRaster;
	context->RSGetState(savedRaster.put());
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

	const bool stats = voxelPipelineStatsEnabled && EnsureVoxelStatsQueries();
	if (stats) {
		ReadVoxelStatsQueries(context);
		context->Begin(voxelStatsQuery[voxelStatsRing][0].get());
		context->Begin(voxelStatsQuery[voxelStatsRing][1].get());
	}
	globals::profiler->BeginPass("SnowDeformation::Volume - Draw");
	ID3D11Buffer* cb2 = voxelDrawCB->CB();
	for (uint L = 0; L < levels; L++) {
		const auto& lv = voxelLevels[L];
		if (!lv.valid || !lv.field || !lv.bricks || !lv.drawArgs)
			continue;
		const float voxelSize = VoxelSizeForLevel(L);
		VoxelDrawCB d{};
		d.VoxOrigin = { lv.origin.x, lv.origin.y, lv.origin.z, 0 };
		d.VoxParams = { voxelSize, float(lv.dims.x), VoxelThresholdForLevel(L), std::clamp(settings.VolumeMarchStep, 0.25f, 2.0f) };
		d.VoxDims = { float(lv.dims.x), float(lv.dims.y), float(lv.dims.z), float(lv.dims.x) / float(lv.dims.z) };
		float inStart = -2.0f, inEnd = -1.0f, outStart = 0.0f, outEnd = 0.0f;
		const auto eye = globals::game::frameBufferCached.GetCameraPosAdjust();
		const auto centre = VoxelLevelCentre(L);
		d.VoxCentre = { centre.x - eye.x, centre.y - eye.y, centre.z - eye.z, settings.VolumeSkipEmptyCells ? 1.0f : 0.0f };
		const float detailDist = std::clamp(settings.VolumeDetailDistance, 100.0f, 8000.0f);
		d.VoxInnerCentre = { 0.0f, 0.0f, 0.0f, detailDist };
		if (L > 0) {
			VoxelReachBand(L - 1, inStart, inEnd);
			const auto inner = VoxelLevelCentre(L - 1);
			d.VoxInnerCentre = { inner.x - eye.x, inner.y - eye.y, inner.z - eye.z, detailDist };
		}
		VoxelReachBand(L, outStart, outEnd);
		d.VoxFade = { inStart, inEnd, outStart, outEnd };
		const float maxDist = std::clamp(settings.VolumeMaxDistance, 500.0f, 8000.0f);
		d.VoxDebug = { float(L), showVolumeRings ? 1.0f : 0.0f, maxDist, maxDist * 0.25f };
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
	if (stats) {
		context->End(voxelStatsQuery[voxelStatsRing][1].get());
		context->End(voxelStatsQuery[voxelStatsRing][0].get());
		voxelStatsIssued[voxelStatsRing] = true;
		voxelStatsRing = (voxelStatsRing + 1) % kShellStatsRing;
	}

	ID3D11ShaderResourceView* nullSRV = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->VSSetShaderResources(41, 1, &nullSRV);
	context->PSSetShaderResources(42, 1, &nullSRV);
	context->PSSetShaderResources(3, 1, &nullSRV);
	context->PSSetShaderResources(13, 1, &nullSRV);
	context->PSSetSamplers(3, 1, &nullSampler);
	context->VSSetConstantBuffers(2, 1, &nullCB);
	context->PSSetConstantBuffers(2, 1, &nullCB);
	context->RSSetState(savedRaster.get());
}
