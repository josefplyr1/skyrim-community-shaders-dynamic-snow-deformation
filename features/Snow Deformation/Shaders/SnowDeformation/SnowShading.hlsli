// Sun BRDF + indirect lobes routed through CS's own PBR path
// (PBR::GetDirectLightInput / GetIndirectLobeWeights via LightingEval),
// shared by SnowShell.hlsl and SnowStaticsShell.hlsl. ROUTING-ROADMAP M1.
//
// MUST BE THE LAST INCLUDE of each shell's PSHADER block. TRUE_PBR is
// defined here and only here: the includes before this one must compile
// WITHOUT it (ExtendedMaterials' TRUE_PBR branches read Lighting.hlsl
// cbuffers; Color.hlsli was already frozen by its include guard), and
// LightingCommon/PBR below require it. Because Color.hlsli is compiled
// without TRUE_PBR, its Light() applies no pi compensation - it is applied
// explicitly here via Color::PBRLightingCompensation, which Color.hlsli
// defines unconditionally. See PI-CONVENTION-SPIKE.md for the convention
// audit this implements.
//
// Units contract with the callers: outputs are Lighting.hlsl-internal
// units. The write tail must multiply directDiffuse, directSpecular, the
// ambient term and the Albedo RT payload by Color::PBRLightingScale, and
// leave specularLobe (Reflectance RT) unscaled - mirroring
// Lighting.hlsl:2766-2774.

#define TRUE_PBR
#define GLINT

// PBR.hlsli reads Lighting.hlsl's per-material PBRFlags; the shells have no
// per-material CB, so a zero global compiles the same code with every
// optional lobe (subsurface, coat, fuzz, hair) off.
static uint PBRFlags = 0;

#include "Common/LightingEval.hlsli"

struct SnowSunLighting
{
	float3 directDiffuse;   // albedo-multiplied, incl. transmission
	float3 directSpecular;  // GGX or glint NDF
	float3 diffuseLobe;     // -> Albedo RT + ambient term
	float3 specularLobe;    // -> Reflectance RT (composite expects unscaled)
};

// glintParams = (logMicrofacetDensity, microfacetRoughness,
// densityRandomization, screenSpaceScale) - the ShellCB packing.
SnowSunLighting SnowEvaluateSunPBR(float3 normalWS, float3 V, float sunShadow,
	float3 albedo, float roughness, float3 F0, float ao,
	float4 glintParams, float glintActive,
	float2 glintUV, float2 uvDDX, float2 uvDDY, float2 pixelPos)
{
	// raw x pi (LL off) / gamma-corrected x pi x mults (LL on): exactly what
	// Lighting.hlsl feeds its dir light context.
	float3 sunColor = Color::DirectionalLight(SharedData::DirLightColor.xyz) * Color::PBRLightingCompensation;

	MaterialProperties material = (MaterialProperties)0;
	material.BaseColor = albedo;
	material.Roughness = roughness;
	material.F0 = F0;
	material.AO = ao;
	material.CoatColor = 1.0;

	// Same clamps as Lighting.hlsl:1841-1844; the CPU-disabled convention
	// (density 0) lands on the floor, below the >1.1 gate inside
	// SpecularMicrofacetWithGlint, which then falls back to smooth GGX.
	material.GlintScreenSpaceScale = max(1.0, glintParams.w);
	material.GlintLogMicrofacetDensity = glintActive > 0.5 ? clamp(glintParams.x, PBR::Constants::MinGlintDensity, PBR::Constants::MaxGlintDensity) : PBR::Constants::MinGlintDensity;
	material.GlintMicrofacetRoughness = clamp(glintParams.y, PBR::Constants::MinGlintRoughness, PBR::Constants::MaxGlintRoughness);
	material.GlintDensityRandomization = clamp(glintParams.z, PBR::Constants::MinGlintDensityRandomization, PBR::Constants::MaxGlintDensityRandomization);
	float glintNoise = Random::R1Modified(float(SharedData::FrameCount), (Random::pcg2d(uint2(pixelPos)) / 4294967296.0).x);
	material.Noise = glintNoise;
	[branch] if (material.GlintLogMicrofacetDensity > 1.1)
		Glints::PrecomputeGlints(glintNoise, glintUV, uvDDX, uvDDY, material.GlintScreenSpaceScale, material.GlintCache);

	// Snow has no authored uv tangents; the glint grid rides the same
	// arbitrary-but-stable world-Y frame the hand-rolled path used.
	float3 glintT = normalize(cross(float3(0.0, 1.0, 0.0), normalWS) + float3(1e-5, 0.0, 0.0));
	float3 glintB = cross(normalWS, glintT);
	float3x3 tbnTr = float3x3(glintT, glintB, normalWS);

	float3 L = SharedData::DirLightDirection.xyz;
	DirectContext context = CreateDirectLightingContext(normalWS, normalWS, normalWS, V, V, L, L, sunColor, sunShadow, sunShadow);

	DirectLightingOutput lightingOutput;
	EvaluateLighting(context, material, tbnTr, glintUV, uvDDX, uvDDY, lightingOutput);

	IndirectContext indirectContext = CreateIndirectLightingContext(normalWS, normalWS, V);
	IndirectLobeWeights lobeWeights;
	GetIndirectLobeWeights(lobeWeights, indirectContext, material, glintUV);

	SnowSunLighting o;
	o.directDiffuse = lightingOutput.diffuse * material.BaseColor + lightingOutput.transmission;
	o.directSpecular = lightingOutput.specular;
	o.diffuseLobe = lobeWeights.diffuse;
	o.specularLobe = lobeWeights.specular;
	return o;
}
