// Crisp cascaded shadows for the snow shell.
//
// The Volumetric Shadows feature exposes the sun cascades only as a 512px
// blurred VSM moments copy; fine for volumetrics, but on the shell it turns
// tree branches and actor silhouettes into smudges while the bare ground next
// to it (vanilla forward path) shows crisp cascade shadows. This samples the
// game's raw shadow atlas instead: cascade = array slice, per-slice [0,1] UVs
// matching the ShadowProj matrices from DirectionalShadowLights (t98), with
// hardware comparison PCF at full atlas resolution.
//
// Two source textures, min'd like VolumetricShadows' downsample does: the
// main atlas and its ESRAM partner target, both captured as copies during
// the game's shadow-mask pass (see SnowDeformation::CaptureShadowAtlas).
// Cascade selection, blend and distance fade mirror
// VolumetricShadows::GetVSMShadow2D so the crisp and fallback paths agree
// about where shadows exist.
//
// Include after Common/ShadowSampling.hlsli; needs DirectionalShadowLights,
// SharedData and FrameBuffer from it.

Texture2DArray<float> SnowShadowAtlas : register(t22);
Texture2DArray<float> SnowShadowAtlasESRAM : register(t23);
SamplerComparisonState SnowShadowCmpSampler : register(s2);

// Shadow-casting local lights (fires, lanterns, torches). Their maps live
// in their own atlas, copied at the local lights' mask passes (NOT the
// sun cascade atlas: local slice indices collide with the cascades');
// this table carries each light's descriptor transform and slice, indexed
// by the light's shadow-mask channel. LightType 0 marks an empty slot
// (also used when the local atlas copy is unavailable this frame).
struct PointShadowLight
{
	column_major float4x4 LightTransform;
	uint SliceIndex;
	uint LightType;  // 0 empty, 1 spot, 2 paraboloid, 3 dual paraboloid
	float2 padPSL;
};
StructuredBuffer<PointShadowLight> PointShadowLights : register(t38);
Texture2DArray<float> SnowPointShadowAtlas : register(t39);

namespace SnowShadow
{
	// atlasSlice: the cascade's REAL slice in the SHARED sun atlas
	// (BorderStyle.zw, captured from shadowmapIndex at mask time - the
	// slices move as local shadow lights come and go with the view; round
	// 22). The ESRAM partner is a dedicated cascade resource, so it keeps
	// direct cascade indexing.
	float SampleCascadeCmp(float3 posLS, uint atlasSlice, uint cascade)
	{
		float lit = SnowShadowAtlas.SampleCmpLevelZero(SnowShadowCmpSampler, float3(posLS.xy, atlasSlice), posLS.z);
		float litEsram = SnowShadowAtlasESRAM.SampleCmpLevelZero(SnowShadowCmpSampler, float3(posLS.xy, cascade), posLS.z);
		return min(lit, litEsram);
	}

	// Center + 4 rotated taps, each hardware-bilinear 2x2 comparison; a
	// small soft edge like the vanilla receiver, nothing like the VSM blur.
	// a_spread widens the tap ring: >1 turns the far cascade's blocky texels
	// into soft penumbra blobs without dropping the shadows (LOD trees cast
	// into these cascades; fading them out erases their shadows from
	// distant snow).
	float SampleCascadePCF(float3 posLS, uint cascade, uint atlasSlice, float2 texel, float a_spread)
	{
		float shadow = SampleCascadeCmp(posLS, atlasSlice, cascade);
		const float2 kTaps[4] = { { 1.4, 0.4 }, { -0.4, 1.4 }, { -1.4, -0.4 }, { 0.4, -1.4 } };
		[unroll] for (uint tapI = 0; tapI < 4; tapI++)
			shadow += SampleCascadeCmp(float3(posLS.xy + kTaps[tapI] * texel * a_spread, posLS.z), atlasSlice, cascade);
		return shadow * 0.2;
	}

	// positionRel is camera-relative. The receiver is offset along the normal
	// (instead of a large depth bias) so flat sunlit snow shows no acne while
	// contact shadows stay attached.
	// a_atlasSlices: the REAL shared-atlas slices of cascades 0/1
	// (BorderStyle.zw at the call sites - this include precedes the ShellCB
	// declaration, so the CB cannot be read here).
	float GetCascadeShadow(float3 positionRel, float3 normalWS, float a_spread, uint2 a_atlasSlices)
	{
		DirectionalShadowLightData sd = DirectionalShadowLights[0];

		float shadowMapDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(positionRel));

		// If/else result instead of an early out: fxc raises a false-positive
		// X4000 (potentially uninitialized variable) on return-then-sample
		// patterns, and CI requires warning-free shaders.
		float result = 1.0;
		[branch] if (shadowMapDepth < sd.EndSplitDistances.y)
		{
			float3 positionWS = positionRel + FrameBuffer::CameraPosAdjust.xyz + normalWS * 3.0;

			float atlasW, atlasH, atlasSlices;
			SnowShadowAtlas.GetDimensions(atlasW, atlasH, atlasSlices);
			float2 texel = 1.0 / float2(atlasW, atlasH);

			// Cascade selection and blend; identical to GetVSMShadow2D.
			float cascadeSelect = saturate((shadowMapDepth - sd.StartSplitDistances.y) / (sd.EndSplitDistances.x - sd.StartSplitDistances.y));
			uint primaryCascade = uint(cascadeSelect);

			float3 posLS = mul(sd.ShadowProj[primaryCascade], float4(positionWS, 1)).xyz;
			posLS.xy = saturate(posLS.xy);
			posLS.z -= 0.0008 * (primaryCascade + 1.0);
			float shadow = SampleCascadePCF(posLS, primaryCascade, primaryCascade == 0 ? a_atlasSlices.x : a_atlasSlices.y, texel, a_spread);

			[branch] if (cascadeSelect > 0.0 && cascadeSelect < 1.0)
			{
				uint secondaryCascade = 1 - primaryCascade;
				posLS = mul(sd.ShadowProj[secondaryCascade], float4(positionWS, 1)).xyz;
				posLS.xy = saturate(posLS.xy);
				posLS.z -= 0.0008 * (secondaryCascade + 1.0);
				float shadowBlend = SampleCascadePCF(posLS, secondaryCascade, secondaryCascade == 0 ? a_atlasSlices.x : a_atlasSlices.y, texel, a_spread);
				shadow = lerp(shadow, shadowBlend, smoothstep(0, 1, cascadeSelect));
			}

			// Distance fade matching the VSM path: beyond the cascades the
			// world shadows (terrain/cloud) carry on alone, with no visible
			// boundary.
			float fade = saturate(shadowMapDepth / sd.EndSplitDistances.y);
			result = lerp(1.0, shadow, 1.0 - pow(fade * fade, 8));
		}
		return result;
	}

	static const float kSpotShadowBias = 0.0015;
	static const float kRadialShadowBias = 0.003;

	float SamplePointCmp(float2 uv, uint slice, float cmp)
	{
		return SnowPointShadowAtlas.SampleCmpLevelZero(SnowShadowCmpSampler, float3(uv, slice), cmp);
	}

	// Local-light shadow evaluated at the receiver's REAL position (the
	// raised snow surface), so shadow length is correct for the shell
	// rather than the buried ground. The three projections transcribe the
	// game's own shadow-mask passes (Utility.hlsl RENDER_SHADOWMASKSPOT /
	// PB / DPB): spot = perspective map; paraboloid depth compares are
	// RADIAL distance normalized by the light radius, not projected z.
	// positionWS absolute; lightRadius from the light's cluster data.
	float GetPointLightShadow(float3 positionWS, uint shadowLightIndex, float lightRadius)
	{
		PointShadowLight sl = PointShadowLights[shadowLightIndex];
		float vis = 1.0;
		[branch] if (sl.LightType == 1)
		{
			float4 positionLS = mul(sl.LightTransform, float4(positionWS, 1.0));
			[branch] if (positionLS.w > 1e-4)
			{
				positionLS.xyz /= positionLS.w;
				float2 uv = positionLS.xy * 0.5 + 0.5;
				vis = SamplePointCmp(uv, sl.SliceIndex, positionLS.z - kSpotShadowBias);
				// Cone edge falloff. Vanilla exponentiates by the light's
				// authored falloff; a fixed exponent stands in (the final
				// saturate makes outside-cone fully dark, matching the
				// spot's illumination cone).
				vis -= pow(2.0 * length(0.5 * positionLS.xy), 8.0) * vis;
			}
		}
		else if (sl.LightType == 2)
		{
			float4 unadjusted = mul(sl.LightTransform, float4(positionWS, 1.0));
			[branch] if (unadjusted.z * 0.5 + 0.5 >= 0.0)
			{
				float3 positionLS = unadjusted.xyz / unadjusted.w;
				float3 lightDirection = normalize(normalize(positionLS) + float3(0, 0, 1));
				float2 uv = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
				float cmp = saturate(length(positionLS) / lightRadius) - kRadialShadowBias;
				vis = SamplePointCmp(uv, sl.SliceIndex, cmp);
			}
		}
		else if (sl.LightType == 3)
		{
			// Dual paraboloid (torches, campfires): hemispheres packed into
			// the slice's vertical halves.
			float3 positionLS = mul(sl.LightTransform, float4(positionWS, 1.0)).xyz;
			bool lowerHalf = positionLS.z * 0.5 + 0.5 < 0.0;
			float3 lightDirection = normalize(normalize(positionLS) + (lowerHalf ? float3(0, 0, -1) : float3(0, 0, 1)));
			float2 uv = lightDirection.xy / lightDirection.z * 0.5 + 0.5;
			uv.y = lowerHalf ? 1.0 - 0.5 * uv.y : 0.5 * uv.y;
			float cmp = saturate(length(positionLS) / lightRadius) - kRadialShadowBias;
			vis = SamplePointCmp(uv, sl.SliceIndex, cmp);
		}
		return saturate(vis);
	}
}
