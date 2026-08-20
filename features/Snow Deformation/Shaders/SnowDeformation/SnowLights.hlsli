#ifndef __SNOW_LIGHTS_DEPENDENCY_HLSL__
#define __SNOW_LIGHTS_DEPENDENCY_HLSL__

// Point lights for the snow shells, from Light Limit Fix's clustered
// visible-light list. The shells draw in the deferred pass, outside the
// game's per-geometry light plumbing, so the strict-light cbuffer (b3) is
// not available; the cluster list carries every visible placed light and
// is the sole source here. Shadow-casting lights sample their own shadow
// maps via SnowShadow::GetPointLightShadow, so include SnowShadow.hlsli
// first. Per-light shading routes through SnowEvaluateLightPBR, so
// SnowShading.hlsli must ALSO be included first.

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"

// LLF's own header: the cluster buffers (t35-37, the registers the CPU side
// binds for the shell draws), structs, and GetClusterIndex - previously a
// local copy, deleted once b12 was measured bound during the deferred pass
// (its FrameBuffer read is the first-person fix). StrictLightData (b3) comes
// along unbound; nothing here reads it - the cluster list is the sole source
// in the deferred pass.
#include "LightLimitFix/LightLimitFix.hlsli"

#include "InverseSquareLighting/InverseSquareLighting.hlsli"

namespace SnowLights
{
	// Adds the clustered point lights to the shell's direct lobes, each
	// light shaded through the SAME routed PBR path as the sun
	// (SnowEvaluateLightPBR - ROUTING-ROADMAP M3), so point lights carry
	// glints and every future TruePBR lobe automatically. worldPos
	// camera-relative, worldPosAbs absolute; clusterUV is the pixel's
	// projection-space screen UV. Shadow-casting lights sample their own
	// shadow map at the shell surface, so shadow length is correct for the
	// raised snow. Room/portal culling is skipped: the shell only exists in
	// exteriors. GetAttenuation self-selects inverse-square vs vanilla
	// falloff per light flags, so ISL parity is automatic.
	void AccumulatePointLights(
		SnowMaterialCtx mtl,
		float3 worldPos, float3 worldPosAbs, float3 normalWS, float3 V, float viewZ,
		float2 clusterUV, float2 glintUV, float2 uvDDX, float2 uvDDY,
		inout float3 diffuse, inout float3 specular)
	{
		uint clusterIndex = 0;
		[branch] if (!LightLimitFix::GetClusterIndex(clusterUV, viewZ, clusterIndex))
			return;

		LightLimitFix::LightGrid grid = LightLimitFix::lightGrid[clusterIndex];

		[loop] for (uint i = 0; i < grid.lightCount; i++)
		{
			LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[grid.offset + i]];

			float3 lightDirection = light.positionWS.xyz - worldPos;
			float lightDist = length(lightDirection);
			float attenuation = InverseSquareLighting::GetAttenuation(lightDist, light);
			if (attenuation < 1e-5)
				continue;

			const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
			float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * attenuation * light.fade;

			float lightShadow = 1.0;
			[branch] if (light.lightFlags & LightLimitFix::LightFlags::Shadow)
				lightShadow = SnowShadow::GetPointLightShadow(worldPosAbs, light.shadowLightIndex, light.radius);

			float3 L = normalize(lightDirection);
			if (dot(normalWS, L) <= 0.0 || lightShadow <= 0.0)
				continue;

			SnowEvaluateLightPBR(mtl, normalWS, V, L, lightColor, lightShadow,
				glintUV, uvDDX, uvDDY, diffuse, specular);
		}
	}
}

#endif  //__SNOW_LIGHTS_DEPENDENCY_HLSL__
