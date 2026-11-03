#ifndef __SNOW_LIGHTING_HLSLI__
#define __SNOW_LIGHTING_HLSLI__

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(PROJECTED_UV) || defined(LANDSCAPE) || defined(SNOW_BODY_COAT)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
#	include "Common/PBR.hlsli"
#endif
#if defined(EMAT) && !defined(LANDSCAPE) && !defined(SKINNED) && !defined(FACEGEN) && !defined(EYE)
#	include "SnowDeformation/SnowParallaxFollow.hlsli"
#endif
#include "SnowDeformation/SnowColor.hlsli"

namespace SnowLighting
{
	struct State
	{
		float landSnowness;
		bool projMatch;
		float3 projAlbedo;
		float4 projRMAOS;
		float3 parallaxShift;
		bool underWater;
		float printShare;
		float lodReplaceW;
		float3 lodAlbedo;
		bool texMatch;
		float texWeight;
		float3 projLightTotal;
		bool landRecolor;
		uint landSnowBits;
		float3 landSnowRaw;
		float2 landSnowNormal;
		float4 landSnowRMAOS;
		float landSnowFrac;
#if defined(SNOW_BODY_COAT)
		float coatAlpha;
#endif
	};

	float LandSnowTexel(float3 g)
	{
		float lum = dot(g, float3(0.2126, 0.7152, 0.0722));
		float sat = max(g.r, max(g.g, g.b)) - min(g.r, min(g.g, g.b));
		float lo = SharedData::snowDeformationSettings.LandRecolorLumLo;
		return smoothstep(lo, lo + 0.18, lum) * (1.0 - smoothstep(0.10, 0.22, sat));
	}

	float3 SampleEncoded(float3 sampled)
	{
#if defined(TRUE_PBR)
		return TransferFunctions::LinearToGamma22(max(sampled, 0.0));
#else
		return sampled;
#endif
	}

#if defined(LANDSCAPE)
	float3 LayerEncoded(float3 layerColor)
	{
#	if defined(TRUE_PBR)
		return TransferFunctions::LinearToGamma22(max(layerColor, 0.0));
#	elif defined(ENABLE_LL)
#		if defined(ENABLE_ACESCG)
		layerColor = AP1TosRGB(layerColor);
#		endif
		const float white = SharedData::linearLightingSettings.diffuseWhiteReflectance;
		const float k = SharedData::linearLightingSettings.diffuseCurve;
		const float3 y = clamp(layerColor, 0.0, white * 0.999);
		return pow(saturate(y / max(white * k - y * (k - 1.0), 1e-6)), 1.0 / SharedData::linearLightingSettings.diffuseGamma);
#	else
		return layerColor;
#	endif
	}
#endif

	void SnowGrain(inout float3 worldNormal, float3 texN, float3 triWeights, float screenNoise, float weight)
	{
		const bool planeX = screenNoise < triWeights.x;
		const bool planeZ = !planeX && screenNoise >= triWeights.x + triWeights.y;
		float3 bumpT = planeX ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
		float3 bumpB = planeZ ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, 1.0);
		bumpT -= worldNormal * dot(worldNormal, bumpT);
		bumpB -= worldNormal * dot(worldNormal, bumpB);
		bumpT *= rsqrt(max(dot(bumpT, bumpT), 1e-8));
		bumpB *= rsqrt(max(dot(bumpB, bumpB), 1e-8));
		worldNormal = normalize(worldNormal + (bumpT * texN.x + bumpB * texN.y) * weight);
	}

	float4 ShellRMAOS(float4 r)
	{
		return float4(clamp(r.x * SharedData::snowDeformationSettings.SnowRoughnessScale, 0.05, 1.0), 0.0, r.z, r.w * SharedData::snowDeformationSettings.SnowSpecularLevel);
	}

	bool ShellHasRMAOS()
	{
		return (uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 512) != 0;
	}
	static const float4 kShellRMAOSDefault = float4(0.6, 0.0, 1.0, 0.028);

#if defined(LANDSCAPE) || ((defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR))
	struct SnowSetTaps
	{
		float2 uv0, uv1, uv2;
		float3 weights;
		float2 dx, dy;
	};

	float2 SnowSetHash(float2 cell)
	{
		float3 p3 = frac(float3(cell.x, cell.y, cell.x) * float3(0.1031, 0.1030, 0.0973));
		p3 += dot(p3, p3.yzx + 33.33);
		return frac(float2((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y));
	}

	SnowSetTaps SnowSetTapsAt(float2 worldRelXY, float2 worldXY)
	{
		float2 uv = frac(FrameBuffer::CameraPosAdjust.xy / SnowDeformation::SnowUVTile) + worldRelXY / SnowDeformation::SnowUVTile;
		SnowSetTaps t;
		t.dx = ddx(uv);
		t.dy = ddy(uv);
		float2 lattice = mul(float2x2(1.0, -0.57735027, 0.0, 1.15470054), worldXY * (0.6 / 256.0));
		float2 cellBase = floor(lattice);
		float2 f = frac(lattice);
		bool upper = f.x + f.y >= 1.0;
		float2 v0 = cellBase + (upper ? float2(1.0, 1.0) : float2(0.0, 0.0));
		float2 v1 = cellBase + (upper ? float2(0.0, 1.0) : float2(1.0, 0.0));
		float2 v2 = cellBase + (upper ? float2(1.0, 0.0) : float2(0.0, 1.0));
		float3 bary = upper ? float3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y) : float3(1.0 - f.x - f.y, f.x, f.y);
		bary = pow(bary, 4.0);
		bary /= dot(bary, 1.0);
		t.weights = bary;
		t.uv0 = uv + SnowSetHash(v0);
		t.uv1 = uv + SnowSetHash(v1);
		t.uv2 = uv + SnowSetHash(v2);
		return t;
	}

	float4 SnowSetSample(Texture2D<float4> tex, SnowSetTaps t)
	{
		return t.weights.x * tex.SampleGrad(SampColorSampler, t.uv0, t.dx, t.dy) + t.weights.y * tex.SampleGrad(SampColorSampler, t.uv1, t.dx, t.dy) +
		       t.weights.z * tex.SampleGrad(SampColorSampler, t.uv2, t.dx, t.dy);
	}
#endif

#if defined(LANDSCAPE)
	void LandSnowness(inout State s, float4 blendWeights1, float2 blendWeights2)
	{
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 1) != 0)
		{
#	if defined(TRUE_PBR)
			uint snowTileBits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
			float4 snowIsSnow1to4 = float4(snowTileBits & 1, (snowTileBits >> 1) & 1, (snowTileBits >> 2) & 1, (snowTileBits >> 3) & 1);
			float2 snowIsSnow5to6 = float2((snowTileBits >> 4) & 1, (snowTileBits >> 5) & 1);
			s.landSnowness = saturate(dot(blendWeights1, snowIsSnow1to4) + dot(blendWeights2, snowIsSnow5to6));
#	else
			s.landSnowness = saturate(dot(blendWeights1, LandscapeTexture1to4IsSnow) + blendWeights2.x * LandscapeTexture5to6IsSnow.x + blendWeights2.y * LandscapeTexture5to6IsSnow.y);
#	endif
		}
	}

	void LandSnownessPostBlend(inout State s, float4 blendWeights1, float2 blendWeights2)
	{
		LandSnowness(s, blendWeights1, blendWeights2);
	}

	void WriteLandSnowOdds(float4 blendWeights1, float2 blendWeights2, inout float4 masks)
	{
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 128) != 0)
		{
			uint bits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
			float snow = dot(blendWeights1, float4(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1)) + dot(blendWeights2, float2((bits >> 4) & 1, (bits >> 5) & 1));
			float rest = max(dot(blendWeights1, 1.0) + blendWeights2.x + blendWeights2.y - snow, 0.0);
			float odds = clamp(log2(max(snow, 1e-12)) - log2(max(rest, 1e-12)), -16.0, 16.0);
			masks.y = 0.004 + 0.996 * (odds + 16.0) * (1.0 / 32.0);
			masks.w = 1.0;
		}
	}

#	if defined(EMAT) && defined(DEFERRED)
	void LandSnowEdgeBlend(PS_INPUT input, float viewDepth, float screenNoise, float2 uv, float mipLevels[6], DisplacementParams params[6], StochasticOffsets sharedOffset, inout float weights[6])
	{
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 128) != 0 && SharedData::extendedMaterialSettings.EnableHeightBlending && viewDepth > 1024.0)
		{
			uint bits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
			float snow = dot(input.LandBlendWeights1, float4(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1)) + dot(input.LandBlendWeights2.xy, float2((bits >> 4) & 1, (bits >> 5) & 1));
			float rest = dot(input.LandBlendWeights1, 1.0) + input.LandBlendWeights2.x + input.LandBlendWeights2.y - snow;
			[branch] if (snow > 0.01 && rest > 0.01)
			{
				float full[6];
				ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, params, 1.0,
					smoothstep(0.0, 1.0, input.LandBlendWeights1), smoothstep(0.0, 1.0, input.LandBlendWeights2.xy), sharedOffset, full);
				float far = smoothstep(1024.0 * 1024.0, 2048.0 * 2048.0, viewDepth * viewDepth);
				[unroll] for (uint i = 0; i < 6; i++)
					weights[i] = lerp(weights[i], full[i], far);
			}
		}
	}
#	endif

	void LandRecolorBegin(inout State s, float3 worldRel)
	{
		uint bits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 256) != 0 && bits != 0)
		{
			float2 worldXY = worldRel.xy + FrameBuffer::CameraPosAdjust.xy;
			const SnowSetTaps taps = SnowSetTapsAt(worldRel.xy, worldXY);

			float3 a = SnowSetSample(SnowDeformation::HorizonSnowAlbedo, taps).rgb;
			float3 shellAlbedo = SnowColor::SetAlbedo(a, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
			const float wet = SharedData::snowDeformationSettings.LandWetPrint;
			const float ice = SharedData::snowDeformationSettings.LandIce;
			float press = 0.0;
			float glaze = 0.0;
			[branch] if (max(wet, ice) > 0.001)
			{
				const float pressed = smoothstep(0.1, 0.5, SnowDeformation::GetDeformation(worldXY));
				press = pressed * saturate(wet * 4.0);
				float since = 0.0;
				[branch] if (pressed > 0.001 && ice > 0.001)
					since = max(ice - SnowIce::AtCut(SnowDeformation::GetCutDate(worldXY)).y, 0.0);
				glaze = max(pressed * since, 0.1 * ice);
			}
			const float3 printTint = lerp(1.0, float3(0.76, 0.77, 0.80) * (1.0 - 0.25 * wet), press) * lerp(1.0, float3(0.93, 0.955, 0.985), glaze);
			shellAlbedo *= printTint;
#	if defined(TRUE_PBR)
			s.landSnowRaw = Color::PBRMaterialToLinear(shellAlbedo);
#	else
			s.landSnowRaw = shellAlbedo;
			s.lodAlbedo = shellAlbedo;
#	endif
			[flatten] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 4) != 0)
			{
				s.landSnowRaw = float3(1.0, 0.0, 1.0);
				s.lodAlbedo = float3(1.0, 0.0, 1.0);
			}
			s.landSnowNormal = 0.0;
			[branch] if (SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
			{
				float2 n = SnowSetSample(SnowDeformation::HorizonSnowNormal, taps).xy * 2.0 - 1.0;
				s.landSnowNormal = float2(n.x, -n.y);
			}
			s.landSnowRMAOS = kShellRMAOSDefault;
			[branch] if (ShellHasRMAOS())
				s.landSnowRMAOS = ShellRMAOS(SnowSetSample(SnowDeformation::HorizonSnowRmaos, taps));
			s.landSnowRMAOS.x *= 1.0 - max(press * (0.24 + 0.26 * wet), glaze * 0.8);
			s.landSnowBits = bits;
			s.landRecolor = true;
		}
	}

	void LandRecolorLayer(inout State s, uint tile, float weight, inout float3 layerColor, inout float3 layerNormal, inout float4 layerRMAOS)
	{
		[branch] if (s.landRecolor && ((s.landSnowBits >> tile) & 1) != 0)
		{
			float w = LandSnowTexel(LayerEncoded(layerColor));
			layerColor = lerp(layerColor, s.landSnowRaw, w);
			layerNormal = lerp(layerNormal, float3(0.5, 0.5, 1.0), w);
			layerRMAOS = lerp(layerRMAOS, s.landSnowRMAOS, w);
			s.landSnowFrac += weight * w;
		}
	}

	void LandRecolorLayer(inout State s, uint tile, float weight, inout float3 layerColor, inout float3 layerNormal)
	{
		[branch] if (s.landRecolor && ((s.landSnowBits >> tile) & 1) != 0)
		{
			float w = LandSnowTexel(LayerEncoded(layerColor));
			layerColor = lerp(layerColor, s.landSnowRaw, w);
			layerNormal = lerp(layerNormal, float3(0.5, 0.5, 1.0), w);
			s.landSnowFrac += weight * w;
		}
	}

	void LandRecolorNormal(inout State s, inout float3 worldNormal)
	{
		[branch] if (s.landSnowFrac > 0.003 && SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
		{
			float3 bumpT = normalize(cross(float3(0.0, 1.0, 0.0), worldNormal) + float3(1e-5, 0.0, 0.0));
			float3 bumpB = cross(worldNormal, bumpT);
			worldNormal = normalize(worldNormal + (bumpT * s.landSnowNormal.x + bumpB * s.landSnowNormal.y) * saturate(s.landSnowFrac));
		}
#	if !defined(TRUE_PBR) && !defined(WORLD_MAP)
		s.lodReplaceW = s.landRecolor ? saturate(s.landSnowFrac) : 0.0;
#	endif
	}

	void DebugLandOverlay(inout float3 baseColor, float2 worldXYRel, float snowness)
	{
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 1) != 0)
		{
			float2 debugWorldXY = worldXYRel + FrameBuffer::CameraPosAdjust.xy;
			float2 debugUV = SnowDeformation::GetDeformationUV(debugWorldXY);
			float debugOutside = (all(debugUV > 0.0) && all(debugUV < 1.0)) ? 0.0 : 1.0;
			float debugDeformation = SnowDeformation::GetDeformation(debugWorldXY);
			baseColor = lerp(baseColor, float3(debugOutside, debugDeformation, snowness), 0.75);
		}
	}
#endif

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
	void LODRecolor(inout State s, float3 rawBaseColor, float2 uv, float2 worldXYRel, float3 normal)
	{
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
		bool snowLodOn = SharedData::snowDeformationSettings.LODObjectEnable > 0.5 &&
		                 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLODBakedIsSnow) != 0;
		const float coverScale = exp2(SharedData::MipBias + 2.0);
		const float2 coverDx = ddx(uv) * coverScale;
		const float2 coverDy = ddy(uv) * coverScale;
#	else
		bool snowLodOn = SharedData::snowDeformationSettings.LODReplaceEnable > 0.5;
#	endif
		[branch] if (snowLodOn)
		{
			float lodSnowScore = SnowDeformation::ClassifyLODSnow(rawBaseColor);
			float lodReplaceT = saturate((length(worldXYRel) - SharedData::snowDeformationSettings.LODReplaceStart) * SharedData::snowDeformationSettings.LODReplaceFadeInv);
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
			float lodReplaceW = lodSnowScore * lodReplaceT * smoothstep(0.0, 0.3, normal.z);
			[branch] if (lodReplaceW > 0.003)
				lodReplaceW *= TexColorSampler.SampleGrad(SampColorSampler, uv, coverDx, coverDy).a >= 254.5 / 255.0 ? 1.0 : 0.0;
#	else
			float lodReplaceW = lodSnowScore * lodReplaceT * smoothstep(0.35, 0.65, normal.z);
#	endif
			const SnowSetTaps taps = SnowSetTapsAt(worldXYRel, worldXYRel + FrameBuffer::CameraPosAdjust.xy);
			[branch] if (lodReplaceW > 0.003)
			{
				float3 snowSample = SnowSetSample(SnowDeformation::HorizonSnowAlbedo, taps).rgb;
				s.lodReplaceW = lodReplaceW;
				s.lodAlbedo = SnowColor::SetAlbedo(snowSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					s.lodAlbedo = float3(1.0, 0.0, 1.0);
			}
		}
	}
#endif

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(LANDSCAPE)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
	void RelightLOD(State s, DirectContext dirLightContext, IndirectContext indirectContext, float3 directionalAmbientColor,
		inout float3 color, inout float3 outputAlbedo, inout float3 specularColor, inout float3 lobeSpecular, inout float roughness)
	{
		[branch] if (s.lodReplaceW > 0.003)
		{
			MaterialProperties snowMaterial = (MaterialProperties)0;
			snowMaterial.BaseColor = s.lodAlbedo;
			snowMaterial.Roughness = clamp(0.6 * SharedData::snowDeformationSettings.SnowRoughnessScale, 0.05, 1.0);
			snowMaterial.F0 = 0.028;
			snowMaterial.AO = 1.0;
			DirectContext snowContext = dirLightContext;
			snowContext.lightColor *= Color::PBRLightingCompensation;
			DirectLightingOutput snowLit;
			PBR::GetDirectLightInput(snowLit, snowContext, snowMaterial, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1), 0.0.xx);
			IndirectLobeWeights snowLobes;
			PBR::GetIndirectLobeWeights(snowLobes, indirectContext, snowMaterial);
			float3 snowColor = (snowLit.diffuse * snowMaterial.BaseColor + snowLobes.diffuse * directionalAmbientColor) * Color::PBRLightingScale;
			color = lerp(color, snowColor, s.lodReplaceW);
			outputAlbedo = lerp(outputAlbedo, snowLobes.diffuse * Color::PBRLightingScale, s.lodReplaceW);
			specularColor = lerp(specularColor, snowLit.specular * Color::PBRLightingScale, s.lodReplaceW);
			lobeSpecular = lerp(lobeSpecular, snowLobes.specular, s.lodReplaceW);
			roughness = lerp(roughness, snowMaterial.Roughness, s.lodReplaceW);
		}
	}
#endif

#if !defined(LANDSCAPE)
	void BeginObject(inout State s, float3 worldRel)
	{
		[branch] if (SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 || SharedData::snowDeformationSettings.SnowTexturedEnable > 0.5)
			s.underWater = SnowDeformation::UnderWater(worldRel + FrameBuffer::CameraPosAdjust.xyz);
#	if defined(PROJECTED_UV)
		[branch] if (SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 && (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
			s.printShare = SnowPrints::Share(worldRel + FrameBuffer::CameraPosAdjust.xyz);
#	endif
	}
#endif

#if defined(PROJECTED_UV)
	float ProjectionAlpha(State s, float vertexAlpha, float projDot)
	{
		[flatten] if ((Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedNoAlpha) != 0)
			vertexAlpha = 1.0;
		[flatten] if ((Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedUnauthored) != 0)
			vertexAlpha *= smoothstep(SharedData::snowDeformationSettings.ProjUnauthoredThreshold - 0.1, SharedData::snowDeformationSettings.ProjUnauthoredThreshold + 0.1, projDot);
		[flatten] if (s.underWater && (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
			vertexAlpha = 0.0;
		return vertexAlpha;
	}

	float PrintBareWeight(State s, float projWeight, float projNoise)
	{
		return lerp(projWeight, min(projWeight, -1.0), SnowPrints::Bare(s.printShare, projNoise));
	}

#	if defined(EMAT) && !defined(LANDSCAPE) && !defined(SKINNED) && !defined(FACEGEN) && !defined(EYE)
	float ParallaxProjNoise(inout State s, float projNoise, float3 worldRel, float3 triWeights, float2 uv, float2 uvOriginal)
	{
		[branch] if (SharedData::snowDeformationSettings.ParallaxFollow > 0.5 && SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 &&
					 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
		{
			const float3 pDx = ddx_fine(worldRel);
			const float3 pDy = ddy_fine(worldRel);
			s.parallaxShift = SnowParallaxFollow::UVShiftToWorld(uv - uvOriginal, ddx_fine(uvOriginal), ddy_fine(uvOriginal), pDx, pDy);
#		if defined(TRUE_PBR)
			[flatten] if ((PBRFlags & PBR::Flags::InterlayerParallax) != 0)
				s.parallaxShift = 0.0;
#		endif
			[branch] if (any(s.parallaxShift != 0.0))
			{
				const float tiling = ProjectedUVParams.z;
				projNoise = Triplanar::SampleGrad(TexCharacterLightProjNoiseSampler, SampCharacterLightProjNoiseSampler, worldRel + FrameBuffer::CameraPosAdjust.xyz + s.parallaxShift,
					triWeights, tiling, pDx * tiling, pDy * tiling).x;
			}
		}
		return projNoise;
	}

#		if !defined(MODELSPACENORMALS) && !defined(MULTI_LAYER_PARALLAX) && !defined(PARALLAX)
#			define SNOW_FLATTEN_PARALLAX
	float ParallaxUnderSnow(float3 worldRel, float3 vertexNormal, float3 texProj, float vertexAlpha)
	{
		float keep = 1.0;
		[branch] if (SharedData::snowDeformationSettings.ParallaxFlatten > 0.5 && SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 &&
					 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
		{
#			if defined(TREE_ANIM)
			vertexAlpha = 1.0;
#			endif
			State s = (State)0;
			s.underWater = SnowDeformation::UnderWater(worldRel + FrameBuffer::CameraPosAdjust.xyz);
			const float projDot = dot(vertexNormal, normalize(texProj));
			const float coverWeight = projDot * ProjectionAlpha(s, vertexAlpha, projDot) - ProjectedUVParams.w;
			keep = 1.0 - smoothstep(-0.1, ProjectedUVParams.x + 0.1, coverWeight);
		}
		return keep;
	}
#		endif
#	endif

#	if !defined(FACEGEN) && !defined(MULTI_LAYER_PARALLAX) && !defined(PARALLAX)
	void ProjectedRecolor(inout State s, float projWeight, float3 vertexNormal, float3 projWorldPos, float screenNoise,
		inout float projectedMaterialWeight, inout float3 baseColor, inout float4 rawRMAOS, inout float3 worldNormal)
	{
		s.projMatch = SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 && !s.underWater &&
		              (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0;
		[branch] if (s.projMatch)
		{
			float snowVerdict = smoothstep(-0.01, 0.01, projWeight);
			projectedMaterialWeight = snowVerdict;
			[branch] if (snowVerdict > 0.003)
			{
				float3 snowTriWeights = Triplanar::GetWeights(vertexNormal, vertexNormal);
				const float3 snowPos = projWorldPos + s.parallaxShift;
				float3 snowGradX, snowGradY;
				Triplanar::ComputeGradients(projWorldPos, 1.0 / SnowDeformation::SnowUVTile, snowGradX, snowGradY);
				[branch] if (SharedData::snowDeformationSettings.ProjSnowNormalEnable > 0.5 && SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
				{
					worldNormal = normalize(lerp(worldNormal, vertexNormal, snowVerdict));
#		if defined(SPARKLE)
					float3 texN = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowNormal, SampColorSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY).xyz * 2.0 - 1.0;
#		else
					float3 texN = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowNormal, SampProjDiffuseSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY).xyz * 2.0 - 1.0;
#		endif
					SnowGrain(worldNormal, texN, snowTriWeights, screenNoise, snowVerdict);
				}
				[branch] if (projectedMaterialWeight > 0.003)
				{
#		if defined(SPARKLE)
				float3 snowProjSample = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY).xyz;
#		else
				float3 snowProjSample = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowAlbedo, SampProjDiffuseSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY).xyz;
#		endif
				s.projAlbedo = SnowColor::SetAlbedo(snowProjSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					s.projAlbedo = float3(1.0, 0.0, 1.0);
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0)
					s.projAlbedo = projectedMaterialWeight.xxx;
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 64) != 0)
					s.projAlbedo = snowProjSample;
				s.projRMAOS = kShellRMAOSDefault;
				[branch] if (ShellHasRMAOS())
				{
#		if defined(SPARKLE)
					s.projRMAOS = ShellRMAOS(Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowRmaos, SampColorSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY));
#		else
					s.projRMAOS = ShellRMAOS(Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowRmaos, SampProjDiffuseSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY));
#		endif
				}
#		if defined(TRUE_PBR)
				baseColor = lerp(baseColor, s.projAlbedo, projectedMaterialWeight);
				rawRMAOS = lerp(rawRMAOS, s.projRMAOS, projectedMaterialWeight);
#		else
				baseColor = lerp(baseColor, s.projAlbedo, projectedMaterialWeight);
#		endif
				}
			}
		}
		[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0 && !s.projMatch)
			baseColor = float3(1.0, 0.0, 0.0);
	}
#	endif
#endif

#if (defined(PROJECTED_UV) || defined(SNOW_BODY_COAT)) && !defined(TRUE_PBR) && !defined(WORLD_MAP)
	void RelightProjected(State s, float projectedMaterialWeight, DirectContext dirLightContext, IndirectContext indirectContext, float3 directionalAmbientColor,
		float3 dirLightColor, float3 worldNormal, float dirDetailedShadow,
		inout float3 color, inout float3 outputAlbedo, inout float3 specularColor, inout float3 lobeSpecular, inout float roughness)
	{
		[branch] if (s.projMatch && projectedMaterialWeight > 0.003)
		{
			MaterialProperties snowMaterial = (MaterialProperties)0;
			snowMaterial.BaseColor = s.projAlbedo;
			snowMaterial.Roughness = s.projRMAOS.x;
			snowMaterial.F0 = s.projRMAOS.w;
			snowMaterial.AO = s.projRMAOS.z;
			DirectContext snowContext = dirLightContext;
			snowContext.lightColor *= Color::PBRLightingCompensation;
			DirectLightingOutput snowLit;
			PBR::GetDirectLightInput(snowLit, snowContext, snowMaterial, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1), 0.0.xx);
			IndirectLobeWeights snowLobes;
			PBR::GetIndirectLobeWeights(snowLobes, indirectContext, snowMaterial);
			float3 vanillaSunTerm = dirLightColor * saturate(dot(worldNormal, DirLightDirection.xyz)) * dirDetailedShadow;
			float3 pointLightShare = max(0.0, s.projLightTotal - vanillaSunTerm);
			float3 snowColor = (snowLit.diffuse * snowMaterial.BaseColor + pointLightShare * snowMaterial.BaseColor + snowLobes.diffuse * directionalAmbientColor) * Color::PBRLightingScale;
			color = lerp(color, snowColor, projectedMaterialWeight);
			outputAlbedo = lerp(outputAlbedo, snowLobes.diffuse * Color::PBRLightingScale, projectedMaterialWeight);
			specularColor = lerp(specularColor, snowLit.specular * Color::PBRLightingScale, projectedMaterialWeight);
			lobeSpecular = lerp(lobeSpecular, snowLobes.specular, projectedMaterialWeight);
			roughness = lerp(roughness, snowMaterial.Roughness, projectedMaterialWeight);
		}
	}
#endif

#if !defined(WORLD_MAP) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE) && !defined(LODOBJECTS) && !defined(LODOBJECTSHD)
	void SnowTexturedRecolor(inout State s, float3 rawBaseColor, float3 vertexNormal, float3 worldRel, float screenNoise,
		inout float3 baseColor, inout float4 rawRMAOS, inout float3 worldNormal)
	{
		[branch] if (SharedData::snowDeformationSettings.SnowTexturedEnable > 0.5 && !s.underWater &&
					 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLODBakedIsSnow) != 0)
		{
			s.texMatch = true;
			float texel = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowTexturedGround) != 0 ? 1.0 : LandSnowTexel(SampleEncoded(rawBaseColor));
			s.texWeight = texel * smoothstep(0.35, 0.65, vertexNormal.z);
			[branch] if (s.texWeight > 0.003)
			{
				float3 snowTexWorld = worldRel + FrameBuffer::CameraPosAdjust.xyz;
				float3 snowTexWeights = Triplanar::GetWeights(vertexNormal, vertexNormal);
				[branch] if (SharedData::snowDeformationSettings.ProjSnowNormalEnable > 0.5 && SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
				{
					worldNormal = normalize(lerp(worldNormal, vertexNormal, s.texWeight));
					float3 texN = Triplanar::SampleStochastic(SnowDeformation::HorizonSnowNormal, SampColorSampler, snowTexWorld, snowTexWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise).xyz * 2.0 - 1.0;
					SnowGrain(worldNormal, texN, snowTexWeights, screenNoise, s.texWeight);
				}
				float3 snowTexSample = Triplanar::SampleStochastic(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, snowTexWorld, snowTexWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise).xyz;
				float3 snowTexAlbedo = SnowColor::SetAlbedo(snowTexSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					snowTexAlbedo = float3(1.0, 0.0, 1.0);
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0)
					snowTexAlbedo = s.texWeight.xxx;
#	if defined(TRUE_PBR)
				baseColor = lerp(baseColor, snowTexAlbedo, s.texWeight);
				float4 snowTexRMAOS = kShellRMAOSDefault;
				[branch] if (ShellHasRMAOS())
					snowTexRMAOS = ShellRMAOS(Triplanar::SampleStochastic(SnowDeformation::HorizonSnowRmaos, SampColorSampler, snowTexWorld, snowTexWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise));
				rawRMAOS = lerp(rawRMAOS, snowTexRMAOS, s.texWeight);
#	else
				baseColor = lerp(baseColor, snowTexAlbedo, s.texWeight);
#	endif
			}
		}
	}
#endif

#if !defined(LANDSCAPE)
	void WriteMasksY(State s, float projWeight, inout float masksY)
	{
#	if defined(PROJECTED_UV)
		[flatten] if (s.projMatch)
		{
			float snowBareT = projWeight >= -0.3 ? 1.0 + projWeight * ((5.0 / 6.0) / 0.3) : (projWeight + 0.8) * ((1.0 / 6.0) / 0.5);
			masksY = projWeight >= 0.0 ? 48.0 : (snowBareT > 0.0 ? 4.0 * exp2(3.0 * snowBareT) : 2.0);
		}
#	else
		[flatten] if ((Permutation::ExtraFeatureDescriptor & (Permutation::ExtraFeatureFlags::SnowProjectedUnauthored | Permutation::ExtraFeatureFlags::SnowMultipassBase | Permutation::ExtraFeatureFlags::SnowObjectBarePass)) != 0)
			masksY = 2.0;
#	endif
#	if !defined(WORLD_MAP) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE) && !defined(LODOBJECTS) && !defined(LODOBJECTSHD)
		[flatten] if (s.texMatch)
			masksY = (s.texWeight >= 0.5 && (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowTexturedOverlay) == 0) ? 48.0 : 2.0;
#	endif
#	if (defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
		[flatten] if (s.lodReplaceW > 0.003)
			masksY = s.lodReplaceW >= 0.5 ? 48.0 : 2.0;
#	endif
		[flatten] if (s.underWater)
			masksY = 2.0;
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
		[flatten] if (masksY >= 1.5)
			masksY *= 256.0;
#	endif
	}
#endif

#if defined(SNOW_BODY_COAT) && !defined(EYE)
	cbuffer BodyCoatRecord : register(b8)
	{
		float4 CoatModelToCanonical[3];
		float4 CoatModelToAtlas[3];
		float4 CoatAtlasLo;
		float4 CoatAtlasHi;
		uint CoatView;
		uint CoatSlot;
		uint CoatComponent;
		uint CoatFlags;
		float4 CoatGlint;
		float4 CoatLook;
		float4 CoatAmounts;
		float4 CoatFall;
	};
	Texture3D<float4> CoatAtlas : register(t41);
	Texture3D<float4> CoatField : register(t44);
	static const uint kSnowBodyCoatBit = (1u << 25);
	static const uint kCoatFlatPaint = 1u;
	static const uint kCoatLiving = 2u;
	static const uint kCoatHairSpecks = 4u;
	static const uint kCoatRelief = 8u;
	static const uint kCoatReliefView = 16u;
	static const uint kCoatHairBeads = 64u;
	static const uint kCoatLumpDepth = 32u;
	static const uint kCoatLumpBlend = 128u;
	static const float3 kCoatAtlasSize = float3(64.0, 32.0, 1536.0);
	static const float kCoatImpactX = 32.0;
	static const float kCoatFieldCells = 12.0;
	static const float kCoatFieldTexels = 72.0;
	static const float kCoatFieldGrad = 3.2;
	static const float kCoatFieldSd = 0.2577;
	static const float3 kCoatClumpFreq = float3(1.0 / 8.0, 1.0 / 2.5, 1.0 / 0.5);
	static const float3 kCoatClumpWeight = float3(0.45, 0.35, 0.2);
	static const float kCoatClumpContrast = 1.6;
	static const float kCoatFullCm = 8.0;
	static const float kCoatPerCm = 1.4;
	static const float kCoatRimSpan = 0.1;
	static const float kCoatEdge = 0.05;
	static const float kCoatLumpUnits = 1.5;
	static const float kCoatNoiseSd = 0.2698;
	static const float2 kCoatNormalQuantile = float2(4.91, 0.14);
	static const float3 kCoatHoleFreq = float3(1.0 / 0.8, 1.0 / 0.4, 1.0 / 0.2);
	static const float kCoatHoleOpen = 0.5156;
	static const float kCoatSpeckMax = 0.5;
	static const float kCoatSpeckReach = 1.8326;
	static const float kCoatSpeckFront = 0.5;
	static const float kCoatDotCover = 0.7;
	static const float2 kCoatDotRank = float2(0.9212, -0.0764);
	static const float kCoatDotInflate = 0.0587;
	static const float kCoatSpeckLevels = 7.0;
	static const float2 kCoatBead = float2(1.2, 0.45);
	static const float kCoatRimCut = 0.3236;
	static const float kCoatPocketRadius = 0.2;
	static const float kCoatNoiseSlope = 0.537;
	static const float kCoatGrainUnits = 0.5 / kCoatClumpFreq.y;

	float3 CoatDebugHue(float h)
	{
		float3 k = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
		return 0.25 + 0.75 * k;
	}

	float3 CoatDebugView(float4 p, float3 c)
	{
		float3 colour;
		[branch] if (CoatView == 2)
		{
			float3 t = float3(dot(CoatModelToAtlas[0], p), dot(CoatModelToAtlas[1], p), dot(CoatModelToAtlas[2], p));
			bool inside = all(t >= CoatAtlasLo.xyz) && all(t < CoatAtlasHi.xyz);
			colour = inside ? CoatAtlas.Load(int4(int3(t), 0)).rgb : float3(1.0, 0.0, 1.0);
		}
		else
		{
			colour = CoatDebugHue(CoatView == 3 ? 0.618034 * CoatSlot : floor(c.z / 8.0) * 0.17);
			colour *= abs(frac(c.z / 8.0) - 0.5) > 0.45 ? 0.15 : 1.0;
			colour = frac(c.x / 16.0) < 0.06 ? float3(1.0, 1.0, 1.0) : colour;
			colour *= (CoatView == 3 && CoatComponent > 0 && frac((c.x + c.y + c.z) / 6.0) < 0.5) ? 0.4 : 1.0;
		}
		return colour;
	}

	float3 CoatSafeNormalize(float3 v)
	{
		return v * rsqrt(max(dot(v, v), 1e-16));
	}

	float3 CoatFrameSolve(float3x3 F, float3 v)
	{
		const float3 c0 = cross(F[1], F[2]);
		return float3(dot(c0, v), dot(cross(F[2], F[0]), v), dot(cross(F[0], F[1]), v)) * (dot(F[0], c0) < 0.0 ? -1.0 : 1.0);
	}

	float CoatCorner(uint3 cell, float3 f, uint3 o)
	{
		return dot(Random::perlinGradient(Random::murmur3(cell + o, 0x578437ADu)), f - float3(o));
	}

	float CoatNoise(float3 p)
	{
		const float3 i = floor(p);
		const float3 f = p - i;
		const uint3 cell = asuint(int3(i));
		const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
		return lerp(
			lerp(lerp(CoatCorner(cell, f, uint3(0, 0, 0)), CoatCorner(cell, f, uint3(1, 0, 0)), u.x),
				lerp(CoatCorner(cell, f, uint3(0, 1, 0)), CoatCorner(cell, f, uint3(1, 1, 0)), u.x), u.y),
			lerp(lerp(CoatCorner(cell, f, uint3(0, 0, 1)), CoatCorner(cell, f, uint3(1, 0, 1)), u.x),
				lerp(CoatCorner(cell, f, uint3(0, 1, 1)), CoatCorner(cell, f, uint3(1, 1, 1)), u.x), u.y),
			u.z);
	}

	void CoatCellBalls(int3 cell, uint set, out float4 ball0, out float4 ball1, out float2 size)
	{
		const uint3 h = Random::pcg3d(asuint(cell) + uint3(0u, 0u, set * 0x9E3779B9u));
		ball0 = float4((h.xxx >> uint3(22, 12, 2)) & 1023u, (h.y >> 22) & 1023u) / 1024.0;
		ball1 = float4((h.yy >> uint2(12, 2)) & 1023u, (h.zz >> uint2(22, 12)) & 1023u) / 1024.0;
		size = (float2((h.zz >> uint2(7, 2)) & 31u) + 0.5) / 32.0;
	}

	float CoatBallFraction(float share, float footprint, float smallest, float aspect)
	{
		const float s = min(share / (1.0 + kCoatDotInflate * footprint * footprint / (smallest * smallest)), kCoatDotCover);
		const float L = -log(1.0 - s);
		return L * (kCoatDotRank.x + kCoatDotRank.y * L) * aspect * aspect * CoatLook.w;
	}

	float CoatBalls(float3 c, float footprint, float q, float smallest, float largest, float3 strand, float aspect, uint set)
	{
		const float g = 2.0 * largest * aspect;
		const int3 base = int3(floor(c / g - 0.5));
		float keep = 1.0;
		[unroll] for (uint o = 0; o < 8; ++o)
		{
			const int3 cell = base + int3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
			float4 ball0, ball1;
			float2 size;
			CoatCellBalls(cell, set, ball0, ball1, size);
			const float2 radius = lerp(smallest, largest, size);
			const float3 d0 = c - (cell + ball0.xyz) * g;
			const float3 d1 = c - (cell + ball1.xyz) * g;
			const float2 al = float2(dot(d0, strand), dot(d1, strand));
			const float2 e = sqrt(al * al / (radius * radius * aspect * aspect) + max(float2(dot(d0, d0), dot(d1, d1)) - al * al, 0.0) / (radius * radius));
			const float2 edge = sqrt(max(1.0 - footprint * footprint / (12.0 * radius * radius), 0.0));
			const float2 a = saturate((edge - e) * radius / max(footprint, 1e-6) + 0.5);
			keep *= (ball0.w < q ? 1.0 - a.x : 1.0) * (ball1.w < q ? 1.0 - a.y : 1.0);
		}
		return 1.0 - keep;
	}

	float CoatSpecks(float3 c, float footprint, float share, float smallest, float largest, float3 strand, float aspect, uint set)
	{
		const float m = clamp(log2(max(footprint, 1e-6) / (2.0 * smallest)), 0.0, kCoatSpeckLevels);
		const float k = floor(m);
		const float t = m - k;
		const float scale = exp2(k);
		float alpha = (1.0 - t) * CoatBalls(c, footprint, CoatBallFraction(share, footprint, smallest * scale, aspect), smallest * scale, largest * scale, strand, aspect, set);
		[branch] if (t > 0.0)
			alpha += t * CoatBalls(c, footprint, CoatBallFraction(share, footprint, 2.0 * smallest * scale, aspect), 2.0 * smallest * scale, 2.0 * largest * scale, strand, aspect, set);
		return alpha;
	}

	float CoatPocket(Texture2D<float4> normalTex, SamplerState normalSampler, float2 uv, float2 duvdx, float2 duvdy,
		float3 dcdx, float3 dcdy, float3 worldDx, float3 worldDy, float3x3 world, float radius)
	{
		const float det = duvdx.x * duvdy.y - duvdy.x * duvdx.y;
		const float valid = abs(det) > 1e-14 ? 1.0 : 0.0;
		const float inv = valid / (valid > 0.0 ? det : 1.0);
		const float3 dcdu = (dcdx * duvdy.y - dcdy * duvdx.y) * inv;
		const float3 dcdv = (dcdy * duvdx.x - dcdx * duvdy.x) * inv;
		const float3 dPdu = (worldDx * duvdy.y - worldDy * duvdx.y) * inv;
		const float3 dPdv = (worldDy * duvdx.x - worldDx * duvdy.x) * inv;
		float2 dims;
		normalTex.GetDimensions(dims.x, dims.y);
		const float pixelLod = log2(max(max(length(duvdx * dims), length(duvdy * dims)), 1.0));
		float flux = 0.0;
		[unroll] for (uint i = 0; i < 3; ++i)
		{
			float2 d;
			sincos(2.0943951 * i, d.y, d.x);
			const float2 offset = d * (radius / max(length(dcdu * d.x + dcdv * d.y), 1e-6));
			const float lod = max(pixelLod, log2(max(length(offset * dims), 1.0)) - 1.0);
#	if defined(MODELSPACENORMALS)
			const float3 n = normalTex.SampleLevel(normalSampler, uv + offset, lod).xzy * 2.0 - 1.0;
#	else
			const float3 n = normalTex.SampleLevel(normalSampler, uv + offset, lod).xyz * 2.0 - 1.0;
#	endif
			flux += dot(CoatSafeNormalize(n.x * world[0] + n.y * world[1] + n.z * world[2]), CoatSafeNormalize(dPdu * d.x + dPdv * d.y));
		}
		return valid * asin(clamp(-0.5 * flux, -1.0, 1.0));
	}

	float4 CoatFieldRead(float3 cells)
	{
		const float3 uvw = (frac(cells / kCoatFieldCells) * kCoatFieldTexels + 0.5) / (kCoatFieldTexels + 1.0);
		const float4 t = CoatField.SampleLevel(SampColorSampler, uvw, 0) * 2.0 - 1.0;
		return float4(t.x, t.yzw * kCoatFieldGrad);
	}

	float4 CoatCrown(float3 q, float2 fade)
	{
		const float2 w = kCoatClumpContrast * kCoatClumpWeight.xy * fade;
		const float4 a = CoatFieldRead(q * kCoatClumpFreq.x);
		const float4 b = CoatFieldRead(q * kCoatClumpFreq.y);
		return float4(0.5 + w.x * a.x + w.y * b.x, w.x * kCoatClumpFreq.x * a.yzw + w.y * kCoatClumpFreq.y * b.yzw);
	}

	float CoatDome(float crown, float cut)
	{
		return kCoatLumpUnits * sqrt(saturate(saturate(crown) - cut));
	}

	float3 CoatLumpMarch(float3 c, float3 N, float3 V, float2 fade, float cut, float footprint, out float4 crown)
	{
		const float vn = max(dot(V, N), 0.2);
		const float3 per = (V - N * dot(V, N)) / vn;
		const float top = CoatDome(1.0, cut);
		float3 hit = c;
		[branch] if (length(per) * top >= 0.5 * footprint)
		{
			const float steps = floor(lerp(8.0, 16.0, saturate(1.0 - vn)));
			const float stepH = top / steps;
			float rayH = top;
			float surf = CoatDome(CoatCrown(c + per * rayH, fade).x, cut);
			float prevRay = rayH;
			float prevSurf = surf;
			[loop] for (float i = 0.0; i < steps && surf < rayH; i += 1.0)
			{
				prevRay = rayH;
				prevSurf = surf;
				rayH -= stepH;
				surf = CoatDome(CoatCrown(c + per * rayH, fade).x, cut);
			}
			const float before = prevRay - prevSurf;
			const float after = surf - rayH;
			hit = c + per * lerp(prevRay, rayH, saturate(before / max(before + after, 1e-5)));
		}
		crown = CoatCrown(hit, fade);
		return hit;
	}

	float BodyCoat(inout State s, float3 modelPosition, float3 worldPos, float3x3 worldFrame, float3x3 bindFrame, Texture2D<float4> normalTex,
		SamplerState normalSampler, Texture2D<float4> colorTex, SamplerState colorSampler, float2 alphaTest, float2 uv, float2 uvMesh, float3 strand,
		float screenNoise, float3 viewDirection, inout float projectedMaterialWeight, inout float3 baseColor, inout float4 rawRMAOS, inout float3 worldNormal)
	{
		float alpha = 0.0;
		[branch] if ((Permutation::ExtraFeatureDescriptor & kSnowBodyCoatBit) != 0)
		{
			float4 p = float4(modelPosition, 1.0);
			float3 c = float3(dot(CoatModelToCanonical[0], p), dot(CoatModelToCanonical[1], p), dot(CoatModelToCanonical[2], p));
			[branch] if (CoatView != 0)
			{
				baseColor = CoatDebugView(p, c);
			}
			else
			{
				const float3 dcdx = ddx(c);
				const float3 dcdy = ddy(c);
				const float3 worldDx = ddx(worldPos);
				const float3 worldDy = ddy(worldPos);
				const float2 duvdx = ddx(uvMesh);
				const float2 duvdy = ddy(uvMesh);
				const float3x3 toCanonical = float3x3(CoatModelToCanonical[0].xyz, CoatModelToCanonical[1].xyz, CoatModelToCanonical[2].xyz);
				const float3x3 world = float3x3(CoatSafeNormalize(worldFrame[0]), CoatSafeNormalize(worldFrame[1]), CoatSafeNormalize(worldFrame[2]));
				const float3x3 bind = float3x3(CoatSafeNormalize(bindFrame[0]), CoatSafeNormalize(bindFrame[1]), CoatSafeNormalize(bindFrame[2]));
				const float3 restRelief = CoatSafeNormalize(mul(toCanonical, mul(CoatFrameSolve(world, worldNormal), bind)));
#	if defined(MODELSPACENORMALS)
				const float3 bindShape = CoatSafeNormalize(normalTex.SampleGrad(normalSampler, uv, duvdx, duvdy).xzy * 2.0 - 1.0);
				const float3 worldShape = CoatSafeNormalize(mul(bindShape, world));
#		define COAT_BIND_SHAPE bindShape
#		define COAT_WORLD_SHAPE worldShape
#	else
#		define COAT_BIND_SHAPE bind[2]
#		define COAT_WORLD_SHAPE world[2]
#	endif
				float3 restShape = CoatSafeNormalize(mul(toCanonical, COAT_BIND_SHAPE));
				restShape = dot(restShape, restRelief) < 0.0 ? -restShape : restShape;
				const float3 t = float3(dot(CoatModelToAtlas[0], p), dot(CoatModelToAtlas[1], p), dot(CoatModelToAtlas[2], p));
				const bool inside = all(t >= CoatAtlasLo.xyz) && all(t < CoatAtlasHi.xyz);
				const float3 at = clamp(t, CoatAtlasLo.xyz + 0.5, CoatAtlasHi.xyz - 0.5);
				const float4 voxel = CoatAtlas.SampleLevel(SampColorSampler, at / kCoatAtlasSize, 0);
				const float3 deposit = -GBuffer::DecodeNormal(voxel.ba);
				const float contact = inside ? voxel.r : 0.0;
				const float fall = inside ? voxel.g : 0.0;
				float impact = 0.0;
				[branch] if (contact > 0.0)
					impact = CoatAtlas.SampleLevel(SampColorSampler, (at + float3(kCoatImpactX, 0.0, 0.0)) / kCoatAtlasSize, 0).r;
				const bool hairSpecks = (CoatFlags & kCoatHairSpecks) != 0;
				const bool flatPaint = (CoatFlags & kCoatFlatPaint) != 0;
				const float fallSpecks = saturate(fall / CoatFall.x);
				const float fallThick = hairSpecks ? 0.0 : saturate((fall - CoatFall.x) / (1.0 - CoatFall.x));
				const float footprint = max(length(dcdx), length(dcdy));
#	if defined(DO_ALPHA_TEST)
				const float2 tapU = duvdx * (0.5 * kCoatGrainUnits / max(length(dcdx), 1e-6));
				const float2 tapV = duvdy * (0.5 * kCoatGrainUnits / max(length(dcdy), 1e-6));
				const float4 tapAlpha = alphaTest.x * float4(colorTex.SampleGrad(colorSampler, uvMesh + tapU, duvdx, duvdy).a, colorTex.SampleGrad(colorSampler, uvMesh - tapU, duvdx, duvdy).a,
														 colorTex.SampleGrad(colorSampler, uvMesh + tapV, duvdx, duvdy).a, colorTex.SampleGrad(colorSampler, uvMesh - tapV, duvdx, duvdy).a);
				const float cutAway = dot(float4(tapAlpha < alphaTest.y), 0.25);
				const float furSpecks = cutAway * saturate((2.0 * CoatLook.z - footprint) / max(2.0 * (CoatLook.z - CoatLook.y), 1e-4));
#	else
				const float furSpecks = 0.0;
#	endif
				float cover = 0.0;
				float lumpShare = 0.0;
				float dome = 0.0;
				float lumpAlpha = 0.0;
				float grainShare = 0.0;
				float3 lumpNormal = restShape;
				[branch] if (!hairSpecks && CoatAmounts.x > 0.0 && impact > 0.0)
				{
					const bool inDepth = (CoatFlags & kCoatLumpDepth) != 0;
					const bool blended = (CoatFlags & kCoatLumpBlend) != 0;
					const bool baked = inDepth || blended;
					const float impactShare = CoatAmounts.x * CoatAmounts.w * saturate((impact - 1e-3) / kCoatSpeckFront);
					const float3 clumpFade = blended ? float3(1.0, 1.0, saturate(1.0 - 2.0 * footprint * kCoatClumpFreq.z)) : saturate(1.0 - 2.0 * footprint * kCoatClumpFreq);
					const float3 octaveSd = baked ? kCoatFieldSd : kCoatNoiseSd;
					const float impactCut = 0.5 + kCoatClumpContrast * length(kCoatClumpWeight * clumpFade * octaveSd) * kCoatNormalQuantile.x *
												  (pow(saturate(1.0 - impactShare), kCoatNormalQuantile.y) - pow(saturate(impactShare), kCoatNormalQuantile.y));
					cover = saturate((1.0 - impactCut) / (1.0 + kCoatEdge));
					lumpShare = cover;
					cover *= 1.0 - furSpecks;
					const float cut = 1.0 - cover * (1.0 + kCoatEdge);
					float3 lumpPoint = c;
					float clump;
					float edge = kCoatEdge;
					float3 domeNormal = restShape;
					[branch] if (baked)
					{
						clump = 0.0;
						[branch] if (cover > 0.0)
						{
							float4 crown;
							[branch] if (inDepth)
							{
								const float3 restView = CoatSafeNormalize(mul(toCanonical, mul(CoatFrameSolve(world, viewDirection), bind)));
								lumpPoint = CoatLumpMarch(c, restShape, restView, clumpFade.xy, cut, footprint, crown);
							}
							else
							{
								crown = CoatCrown(c, clumpFade.xy);
							}
							float flakes = 0.0;
							[branch] if (clumpFade.z > 0.0)
								flakes = CoatFieldRead(lumpPoint.yzx * kCoatClumpFreq.z + 5.3).x;
							clump = saturate(crown.x + kCoatClumpContrast * kCoatClumpWeight.z * clumpFade.z * flakes);
							const float3 slope = crown.yzw - restShape * dot(crown.yzw, restShape);
							const float lie2 = saturate(crown.x) - cut;
							float3 tilt = crown.x > 0.0 && crown.x < 1.0 && lie2 > 0.0 ? slope * (0.5 * kCoatLumpUnits * rsqrt(max(lie2, 1e-4))) : 0.0;
							tilt *= rsqrt(max(1.0, dot(tilt, tilt)));
							domeNormal = CoatSafeNormalize(restShape - tilt);
							edge = max(footprint * (length(slope) + kCoatClumpContrast * kCoatClumpWeight.z * clumpFade.z * kCoatNoiseSlope * kCoatClumpFreq.z), kCoatEdge);
							[branch] if (blended && cut < 1.0)
								grainShare = saturate(1.0 - (cut - clump) / max(length(slope), 1e-3) / kCoatGrainUnits);
						}
					}
					else
					{
						const float3 octaves = kCoatClumpWeight * clumpFade *
											   float3(CoatNoise(c * kCoatClumpFreq.x), CoatNoise(c * kCoatClumpFreq.y), CoatNoise(c * kCoatClumpFreq.z));
						clump = saturate(0.5 + kCoatClumpContrast * (octaves.x + octaves.y + octaves.z));
						dome = CoatDome(0.5 + kCoatClumpContrast * (octaves.x + octaves.y), cut);
					}
					const float3 holeFade = saturate(0.5 / max(footprint * kCoatHoleFreq, 1e-6) - 1.0);
					float3 holeNoise = 0.0;
					[branch] if (cover > 0.0 && any(holeFade > 0.0))
						holeNoise = holeFade * float3(CoatFieldRead(lumpPoint * kCoatHoleFreq.x + 17.1).x, CoatFieldRead(lumpPoint.zxy * kCoatHoleFreq.y + 43.7).x,
												 CoatFieldRead(lumpPoint.yzx * kCoatHoleFreq.z + 71.3).x);
					const float3 holeStep = max(kCoatNoiseSlope * kCoatHoleFreq * footprint * holeFade, 1e-4);
					const float lie = clump - cut;
					const float rim = saturate(lie / kCoatRimSpan);
					const float depthCm = kCoatFullCm * (0.5 + clump) * rim;
					const float3 lumpHoles = saturate((holeNoise - kCoatRimCut - lerp(-kCoatHoleOpen, kCoatHoleOpen, rim)) / holeStep + 0.5);
					lumpAlpha = saturate(lie / (baked ? edge : max(fwidth(lie), kCoatEdge))) * kCoatPerCm * depthCm / (1.0 + kCoatPerCm * depthCm) *
								(1.0 - lumpHoles.x) * (1.0 - lumpHoles.y) * (1.0 - lumpHoles.z);
					lumpNormal = CoatSafeNormalize(lerp(restShape, domeNormal, lumpAlpha));
				}
				const float speckSlope = acos(clamp(dot(hairSpecks ? restShape : restRelief, deposit), -1.0, 1.0));
				const float fallOrder = fall > 0.0 ? 1.0 - speckSlope / (hairSpecks ? kCoatSpeckReach : lerp(CoatFall.y, CoatFall.z, fallSpecks)) : -1.0;
				const float speckCover = max(saturate((contact - 1e-3) / kCoatSpeckFront), fallSpecks * saturate(fallOrder / kCoatSpeckFront));
				float pocket = 0.0;
				[branch] if (!hairSpecks && (CoatFlags & (kCoatRelief | kCoatReliefView)) != 0)
					pocket = (dot(worldNormal, COAT_WORLD_SHAPE) < 0.0 ? -1.0 : 1.0) *
							 CoatPocket(normalTex, normalSampler, uv, duvdx, duvdy, dcdx, dcdy, worldDx, worldDy, world, max(kCoatPocketRadius, footprint));
				const float steer = (CoatFlags & kCoatRelief) != 0 ? 1.0 + clamp(pocket * CoatLook.x / kCoatSpeckFront, -1.0, 1.0) : 1.0;
				const float speckShare =
					max(kCoatSpeckMax * (hairSpecks ? CoatAmounts.z : CoatAmounts.y) * speckCover * steer, kCoatSpeckMax * max(lumpShare * furSpecks, grainShare));
				const float thickShare = kCoatSpeckMax * CoatAmounts.y * fallThick * saturate(fallOrder / kCoatSpeckFront) * steer;
				float speckAlpha = 0.0;
				[branch] if (speckShare > 0.0 && !flatPaint)
				{
					float3 restStrand = 0.0;
					float aspect = 1.0;
					float2 sizes = CoatLook.yz;
					[branch] if (hairSpecks && (CoatFlags & kCoatHairBeads) != 0)
					{
						restStrand = mul(toCanonical, mul(CoatFrameSolve(world, strand), bind));
						restStrand = CoatSafeNormalize(restStrand - restShape * dot(restStrand, restShape));
						aspect = kCoatBead.x / kCoatBead.y;
						sizes *= kCoatBead.y;
					}
					speckAlpha = CoatSpecks(c, footprint, speckShare, sizes.x, sizes.y, restStrand, aspect, 0u);
					[branch] if (thickShare > 0.0)
						speckAlpha = 1.0 - (1.0 - speckAlpha) * (1.0 - CoatSpecks(c, footprint, thickShare, sizes.x, sizes.y, restStrand, aspect, 1u));
				}
				alpha = flatPaint ? 1.0 - (1.0 - speckShare) * (1.0 - thickShare) : max(lumpAlpha, speckAlpha);
				const float snowCover = max(0.75 * alpha, flatPaint ? 0.0 : lumpAlpha);
				[branch] if ((CoatFlags & kCoatReliefView) != 0)
				{
					const float v = clamp(pocket / 0.5235988, -1.0, 1.0);
					baseColor = float3(saturate(-v), 0.15 * (1.0 - abs(v)), saturate(v));
					alpha = 0.0;
				}
				const float bevelDx = ddx(dome);
				const float bevelDy = ddy(dome);
				const float3 setDx = dcdx / SnowDeformation::SnowUVTile;
				const float3 setDy = dcdy / SnowDeformation::SnowUVTile;
				[branch] if (alpha > 0.003)
				{
					const float3 weights = Triplanar::GetWeights(restShape, restShape);
					float3 restSnow = lumpNormal;
					[branch] if (SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
					{
						float3 texN = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowNormal, SampColorSampler, c, weights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, setDx, setDy).xyz * 2.0 - 1.0;
						SnowGrain(restSnow, texN, weights, screenNoise, 1.0);
					}
					const float3 snowNormal = CoatSafeNormalize(mul(CoatFrameSolve(bind, mul(restSnow, toCanonical)), world));
					worldNormal = normalize(lerp(worldNormal, snowNormal, snowCover));
					const float3 r1 = cross(worldDy, COAT_WORLD_SHAPE);
					const float3 r2 = cross(COAT_WORLD_SHAPE, worldDx);
					const float det = dot(worldDx, r1);
					float3 surfaceGradient = (bevelDx * r1 + bevelDy * r2) * (det < 0.0 ? -1.0 : 1.0) / max(abs(det), 1e-8);
					surfaceGradient *= (dot(worldNormal, COAT_WORLD_SHAPE) < 0.0 ? -1.0 : 1.0) * rsqrt(max(1.0, dot(surfaceGradient, surfaceGradient)));
					worldNormal = normalize(worldNormal - surfaceGradient);
					const float3 setSample = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, c, weights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, setDx, setDy).xyz;
					const float3 coatAlbedo = SnowColor::SetAlbedo(setSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
					float4 coatRMAOS = kShellRMAOSDefault;
					[branch] if (ShellHasRMAOS())
						coatRMAOS = ShellRMAOS(Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowRmaos, SampColorSampler, c, weights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, setDx, setDy));
#	if defined(TRUE_PBR)
					baseColor = lerp(baseColor, coatAlbedo, alpha);
					rawRMAOS = lerp(rawRMAOS, coatRMAOS, alpha);
					projectedMaterialWeight = max(projectedMaterialWeight, alpha);
#	else
					s.projMatch = true;
					s.projAlbedo = coatAlbedo;
					s.projRMAOS = coatRMAOS;
					projectedMaterialWeight = 1.0 - (1.0 - projectedMaterialWeight) * (1.0 - alpha);
#	endif
				}
			}
		}
		s.coatAlpha = alpha;
		return alpha;
	}
#	undef COAT_BIND_SHAPE
#	undef COAT_WORLD_SHAPE
#endif
}

#if defined(LANDSCAPE)
#	undef LIGHTING_LANDSCAPE_LAYER_HOOK
#	if defined(TRUE_PBR)
#		define LIGHTING_LANDSCAPE_LAYER_HOOK(TILE) SnowLighting::LandRecolorLayer(snowState, TILE, weight, landColorRGB, landNormalRGB, landRMAOS);
#	else
#		define LIGHTING_LANDSCAPE_LAYER_HOOK(TILE) SnowLighting::LandRecolorLayer(snowState, TILE, weight, landColorRGB, landNormalRGB);
#	endif
#endif

#endif
