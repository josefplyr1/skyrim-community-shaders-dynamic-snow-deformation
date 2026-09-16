// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

#include "Features/SnowDeformation.h"

#include "Globals.h"
#include "SnowDeformationAPI.h"

// Public API for other mods, one set of facts on two transports: Papyrus
// globals on the `SnowDeformation` script (Scripts/SnowDeformation.pex, its
// source beside it) and a versioned C struct from the exported
// SnowDeformation_GetAPI for SKSE plugins. Read-only, callable from any
// thread: the depth query takes the cell data's shared locks and the
// accumulation scalar is atomic. Documented in SNOW-API.md.

bool SnowDeformation::APIIsActive() const
{
	return loaded && settings.EnableSnowDeformation;
}

float SnowDeformation::APISnowDepthAt(float a_x, float a_y)
{
	if (!APIIsActive())
		return 0.0f;
	// Nominal = the class-weighted untrampled depth, negative for submerged
	// classes; the shells clamp the same way.
	return std::max(GetNominalSnowDepthAt(a_x, a_y, 0.0f), 0.0f) * GetAccumulationDepthScale();
}

float SnowDeformation::APISnowDepthAtRef(RE::TESObjectREFR* a_ref)
{
	if (!a_ref || !APIIsActive())
		return 0.0f;
	const RE::NiPoint3 position = a_ref->GetPosition();
	// The stamps' elevated gate: a walkway, bridge or roof is not in the snow.
	float landZ = position.z;
	if (auto* tes = RE::TES::GetSingleton())
		tes->GetLandHeight(position, landZ);
	if (position.z - landZ > kElevatedStampCutoff)
		return 0.0f;
	return APISnowDepthAt(position.x, position.y);
}

float SnowDeformation::APISnowAccumulation() const
{
	if (!APIIsActive() || !settings.EnableSnowAccumulation)
		return 0.0f;
	return snowAccumulation.load(std::memory_order_relaxed);
}

namespace
{
	SnowDeformation& Snow()
	{
		return globals::features::snowDeformation;
	}

	// ---- C API ----
	bool CAPI_IsActive() { return Snow().APIIsActive(); }
	float CAPI_GetSnowDepthAt(float a_x, float a_y) { return Snow().APISnowDepthAt(a_x, a_y); }
	float CAPI_GetSnowDepthAtRef(void* a_ref) { return Snow().APISnowDepthAtRef(static_cast<RE::TESObjectREFR*>(a_ref)); }
	float CAPI_GetSnowAccumulation() { return Snow().APISnowAccumulation(); }
	void CAPI_DepositBlood(float a_x, float a_y, float a_z, float a_radius, float a_r, float a_g, float a_b, float a_amount)
	{
		Snow().APIDepositBlood(a_x, a_y, a_z, a_radius, a_r, a_g, a_b, a_amount);
	}

	constexpr SnowDeformationAPI_V1 kAPIv1{
		1u,
		CAPI_IsActive,
		CAPI_GetSnowDepthAt,
		CAPI_GetSnowDepthAtRef,
		CAPI_GetSnowAccumulation
	};
	constexpr SnowDeformationAPI_V2 kAPIv2{
		2u,
		CAPI_IsActive,
		CAPI_GetSnowDepthAt,
		CAPI_GetSnowDepthAtRef,
		CAPI_GetSnowAccumulation,
		CAPI_DepositBlood
	};

	// ---- Papyrus ----
	int32_t Papyrus_GetAPIVersion(RE::StaticFunctionTag*) { return int32_t(SNOWDEFORMATION_API_VERSION); }
	bool Papyrus_IsActive(RE::StaticFunctionTag*) { return Snow().APIIsActive(); }
	float Papyrus_GetSnowDepthAt(RE::StaticFunctionTag*, float a_x, float a_y) { return Snow().APISnowDepthAt(a_x, a_y); }
	float Papyrus_GetSnowDepthAtRef(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref) { return Snow().APISnowDepthAtRef(a_ref); }
	float Papyrus_GetSnowAccumulation(RE::StaticFunctionTag*) { return Snow().APISnowAccumulation(); }
	void Papyrus_DepositBlood(RE::StaticFunctionTag*, float a_x, float a_y, float a_z, float a_radius, float a_r, float a_g, float a_b, float a_amount)
	{
		Snow().APIDepositBlood(a_x, a_y, a_z, a_radius, a_r, a_g, a_b, a_amount);
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		constexpr std::string_view kScript = "SnowDeformation";
		a_vm->RegisterFunction("GetAPIVersion", kScript, Papyrus_GetAPIVersion);
		a_vm->RegisterFunction("IsActive", kScript, Papyrus_IsActive);
		a_vm->RegisterFunction("GetSnowDepthAt", kScript, Papyrus_GetSnowDepthAt);
		a_vm->RegisterFunction("GetSnowDepthAtRef", kScript, Papyrus_GetSnowDepthAtRef);
		a_vm->RegisterFunction("GetSnowAccumulation", kScript, Papyrus_GetSnowAccumulation);
		a_vm->RegisterFunction("DepositBlood", kScript, Papyrus_DepositBlood);
		logger::info("[SNOW DEFORMATION] Papyrus API v{} registered on script '{}'", SNOWDEFORMATION_API_VERSION, kScript);
		return true;
	}
}

// Resolve with GetProcAddress(GetModuleHandleA("CommunityShaders.dll"),
// "SnowDeformation_GetAPI"); null for a version this build does not serve.
extern "C" __declspec(dllexport) const void* SnowDeformation_GetAPI(uint32_t a_version)
{
	switch (a_version) {
	case 1u:
		return &kAPIv1;
	case 2u:
		return &kAPIv2;
	default:
		return nullptr;
	}
}

void SnowDeformation::InstallAPI()
{
	if (auto* papyrus = SKSE::GetPapyrusInterface(); papyrus && papyrus->Register(RegisterPapyrus))
		logger::info("[SNOW DEFORMATION] Papyrus API registration queued");
	else
		logger::warn("[SNOW DEFORMATION] Papyrus API registration FAILED (no Papyrus interface)");
}
