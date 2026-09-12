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

	constexpr SnowDeformationAPI_V1 kAPIv1{
		SNOWDEFORMATION_API_VERSION,
		CAPI_IsActive,
		CAPI_GetSnowDepthAt,
		CAPI_GetSnowDepthAtRef,
		CAPI_GetSnowAccumulation
	};

	// ---- Papyrus ----
	int32_t Papyrus_GetAPIVersion(RE::StaticFunctionTag*) { return int32_t(SNOWDEFORMATION_API_VERSION); }
	bool Papyrus_IsActive(RE::StaticFunctionTag*) { return Snow().APIIsActive(); }
	float Papyrus_GetSnowDepthAt(RE::StaticFunctionTag*, float a_x, float a_y) { return Snow().APISnowDepthAt(a_x, a_y); }
	float Papyrus_GetSnowDepthAtRef(RE::StaticFunctionTag*, RE::TESObjectREFR* a_ref) { return Snow().APISnowDepthAtRef(a_ref); }
	float Papyrus_GetSnowAccumulation(RE::StaticFunctionTag*) { return Snow().APISnowAccumulation(); }

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		constexpr std::string_view kScript = "SnowDeformation";
		a_vm->RegisterFunction("GetAPIVersion", kScript, Papyrus_GetAPIVersion);
		a_vm->RegisterFunction("IsActive", kScript, Papyrus_IsActive);
		a_vm->RegisterFunction("GetSnowDepthAt", kScript, Papyrus_GetSnowDepthAt);
		a_vm->RegisterFunction("GetSnowDepthAtRef", kScript, Papyrus_GetSnowDepthAtRef);
		a_vm->RegisterFunction("GetSnowAccumulation", kScript, Papyrus_GetSnowAccumulation);
		logger::info("[SNOW DEFORMATION] Papyrus API v{} registered on script '{}'", SNOWDEFORMATION_API_VERSION, kScript);
		return true;
	}
}

// Resolve with GetProcAddress(GetModuleHandleA("CommunityShaders.dll"),
// "SnowDeformation_GetAPI"); null for a version this build does not serve.
extern "C" __declspec(dllexport) const void* SnowDeformation_GetAPI(uint32_t a_version)
{
	return a_version == SNOWDEFORMATION_API_VERSION ? static_cast<const void*>(&kAPIv1) : nullptr;
}

void SnowDeformation::InstallAPI()
{
	if (auto* papyrus = SKSE::GetPapyrusInterface(); papyrus && papyrus->Register(RegisterPapyrus))
		logger::info("[SNOW DEFORMATION] Papyrus API registration queued");
	else
		logger::warn("[SNOW DEFORMATION] Papyrus API registration FAILED (no Papyrus interface)");
}
