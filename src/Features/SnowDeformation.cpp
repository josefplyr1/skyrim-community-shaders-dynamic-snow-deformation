#include "SnowDeformation.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowDeformation::Settings,
	EnableSnowDeformation,
	StampRadius,
	RefillTime,
	RefillOnlyWhenSnowing,
	SnowClassDepths,
	SnowTexturePath,
	SnowTextureLinear,
	SnowBorderNoise,
	SnowBorderSmoothness,
	SnowBorderTrampledFade,
	SnowBorderUntrampledFade,
	RangeShellM,
	RangeTrenchesM)

void SnowDeformation::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>(), "SnowDeformation::PerFrame");

	D3D11_TEXTURE2D_DESC texDesc = {
		.Width = kTextureDim,
		.Height = kTextureDim,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	};

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
		.Texture2D = {
			.MostDetailedMip = 0,
			.MipLevels = 1 }
	};

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = texDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
		.Texture2D = { .MipSlice = 0 }
	};

	for (uint i = 0; i < 2; i++) {
		deformationTextures[i] = new Texture2D(texDesc, i == 0 ? "SnowDeformation::DeformationMap0" : "SnowDeformation::DeformationMap1");
		deformationTextures[i]->CreateSRV(srvDesc);
		deformationTextures[i]->CreateUAV(uavDesc);
	}

	{
		D3D11_TEXTURE2D_DESC terrainDesc = {
			.Width = kShellWindowDim,
			.Height = kShellWindowDim,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE
		};

		D3D11_SHADER_RESOURCE_VIEW_DESC terrainSrvDesc = {
			.Format = terrainDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};

		shellTerrainTexture = new Texture2D(terrainDesc, "SnowDeformation::ShellTerrainWindow");
		shellTerrainTexture->CreateSRV(terrainSrvDesc);
	}

	shellCB = new ConstantBuffer(ConstantBufferDesc<ShellCB>(), "SnowDeformation::ShellCB");

	auto device = globals::d3d::device;

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = TRUE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, shellRasterState.put()));
	Util::SetResourceName(shellRasterState.get(), "SnowDeformation::ShellRasterState");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = TRUE;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, shellDepthState.put()));
	Util::SetResourceName(shellDepthState.get(), "SnowDeformation::ShellDepthState");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxAnisotropy = 8;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, shellSnowSampler.put()));
	Util::SetResourceName(shellSnowSampler.get(), "SnowDeformation::ShellSnowSampler");
}

SnowDeformation::SettingsGPU SnowDeformation::GetCommonBufferData(bool a_inWorld)
{
	// Advance the window once per frame and only from the in-world upload:
	// reflection/early uploads carry probe cameras that must not steer it.
	static Util::FrameChecker frameChecker;
	if (a_inWorld && frameChecker.IsNewFrame()) {
		// Snap to whole texels so scrolling never resamples the map. The
		// cached FrameBuffer camera position is what the lighting pixel
		// shader sees as CameraPosAdjust, so map and terrain agree.
		auto eyePosFB = globals::game::frameBufferCached.GetCameraPosAdjust();
		const float deformTexel = deformWorldSize / kTextureDim;
		float2 desiredOrigin = {
			std::floor((eyePosFB.x - deformWorldSize * 0.5f) / deformTexel) * deformTexel,
			std::floor((eyePosFB.y - deformWorldSize * 0.5f) / deformTexel) * deformTexel
		};

		pendingScrollDelta.x += (int)std::lround((desiredOrigin.x - windowOrigin.x) / deformTexel);
		pendingScrollDelta.y += (int)std::lround((desiredOrigin.y - windowOrigin.y) / deformTexel);
		windowOrigin = desiredOrigin;
	}

	SettingsGPU data{};
	data.WindowOrigin = windowOrigin;
	data.InvWorldSize = 1.0f / deformWorldSize;
	data.EnableSnowDeformation = settings.EnableSnowDeformation;
	data.DebugTerrainOverlay = debugTerrainOverlay ? 1u : 0u;
	return data;
}

void SnowDeformation::ApplyRangeSettings()
{
	// Trench window: content is scale-relative, so a resize clears the map.
	if (trenchRangeDirty || !rangeInitApplied) {
		float newWorldSize = std::clamp(settings.RangeTrenchesM, 29.0f, 200.0f) * 2.0f * kUnitsPerMeter;
		if (std::abs(newWorldSize - deformWorldSize) > 1.0f) {
			deformWorldSize = newWorldSize;
			clearRequested = true;
		}
		trenchRangeDirty = false;
	}
	rangeInitApplied = true;
}

void SnowDeformation::Prepass()
{
	ApplyRangeSettings();

	auto context = globals::d3d::context;

	// Keep t101 bound even while paused or disabled: the lighting shader
	// samples it whenever the feature is compiled in (and gates on
	// EnableSnowDeformation from FeatureData).
	ID3D11ShaderResourceView* deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);

	if (settings.EnableSnowDeformation && globals::state->inWorld)
		UpdateShellTerrainWindow();

	if (!settings.EnableSnowDeformation)
		return;

	auto ui = globals::game::ui;
	if (ui && ui->GameIsPaused())
		return;

	PerFrame perFrameData{};

	// The window origin was advanced in GetCommonBufferData (during
	// UpdateSharedData); only consume the stored state here.
	perFrameData.ScrollDelta = pendingScrollDelta;
	pendingScrollDelta = { 0, 0 };

	perFrameData.WindowOrigin = windowOrigin;
	perFrameData.TexelSize = deformWorldSize / kTextureDim;

	float deltaTime = *globals::game::deltaTime;
	// With RefillOnlyWhenSnowing, snow only recovers while the current
	// weather carries snow; interiors have no sky and do not refill.
	bool refillActive = !settings.RefillOnlyWhenSnowing;
	if (!refillActive)
		if (auto* sky = RE::Sky::GetSingleton())
			if (auto* weather = sky->currentWeather)
				refillActive = weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow);
	perFrameData.RefillAmount = (refillActive && settings.RefillTime > 0.0f) ? deltaTime / settings.RefillTime : 0.0f;

	perFrameData.ClearMap = clearRequested;
	clearRequested = false;

	GatherStamps(perFrameData);

	perFrame->Update(perFrameData);

	uint previousTexture = currentTexture;
	currentTexture = 1 - currentTexture;

	{
		ID3D11Buffer* buffers[1] = { perFrame->CB() };
		context->CSSetConstantBuffers(0, 1, buffers);

		ID3D11ShaderResourceView* srvs[] = { deformationTextures[previousTexture]->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { deformationTextures[currentTexture]->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetDeformationUpdateCS(), nullptr, 0);
		globals::profiler->BeginPass("SnowDeformation::DeformationUpdate");
		context->Dispatch(kTextureDim / 8, kTextureDim / 8, 1);
		globals::profiler->EndPass();
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &nullBuffer);

	ID3D11ShaderResourceView* nullSrvs[1] = { nullptr };
	context->CSSetShaderResources(0, 1, nullSrvs);

	ID3D11UnorderedAccessView* nullUavs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);

	// Rebind: after the ping-pong flip this points at the freshly written map.
	deformationSRV = GetDeformationSRV();
	context->PSSetShaderResources(101, 1, &deformationSRV);
}

ID3D11ComputeShader* SnowDeformation::GetDeformationUpdateCS()
{
	if (!deformationUpdateCS) {
		logger::debug("Compiling DeformationUpdateCS");
		deformationUpdateCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\SnowDeformation\\DeformationUpdateCS.hlsl", {}, "cs_5_0"));
	}
	return deformationUpdateCS;
}

void SnowDeformation::ClearShaderCache()
{
	if (deformationUpdateCS)
		deformationUpdateCS->Release();
	deformationUpdateCS = nullptr;
	if (shellVS)
		shellVS->Release();
	shellVS = nullptr;
	if (shellPS)
		shellPS->Release();
	shellPS = nullptr;
	if (depthSyncCS)
		depthSyncCS->Release();
	depthSyncCS = nullptr;
}

void SnowDeformation::LoadSettings(json& o_json)
{
	settings = o_json;
}

void SnowDeformation::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SnowDeformation::RestoreDefaultSettings()
{
	settings = {};
	clearRequested = true;
}
