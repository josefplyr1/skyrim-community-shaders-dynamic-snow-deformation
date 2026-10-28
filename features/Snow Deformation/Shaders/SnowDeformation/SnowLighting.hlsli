// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Lighting.hlsl's Snow Deformation blocks, as functions. Included by
// Lighting.hlsl just before main, so its samplers, constants and the PBR
// evaluators are in scope; Lighting keeps the call sites only.

#ifndef __SNOW_LIGHTING_HLSLI__
#define __SNOW_LIGHTING_HLSLI__

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(PROJECTED_UV) || defined(LANDSCAPE) || defined(SNOW_BODY_COAT)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
// Non-TRUE_PBR permutations that re-light snow through the PBR evaluators:
// the LOD terrain family (horizon snow), the land's recolored snow layers,
// projected-snow statics and the body coat. The
// optional-lobe branches inside are TRUE_PBR-gated, so these permutations
// compile the coatless diffuse/lobe core against the vanilla
// MaterialProperties.
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
		// Projected snow: the CPU classified this draw's projected material
		// as snow and the swap ran; the albedo is saved for the write tail.
		bool projMatch;
		float3 projAlbedo;
		// Its material response (ShellRMAOS), for the write tail's re-light.
		float4 projRMAOS;
		// Snow Follows Parallax: mesh point to the parallax texel, in world units.
		float3 parallaxShift;
		// Ground under drawn water takes no recolor and no coat (Masks.y = 2).
		bool underWater;
		// Snow Trenches on Objects: the print's depth share here.
		float printShare;
		float lodReplaceW;
		float3 lodAlbedo;
		// Snow-textured recolor: pixel judged, and its snow weight.
		bool texMatch;
		float texWeight;
		// Total accumulated lights before the tail clobbers diffuseColor.
		float3 projLightTotal;
		// Land Snow Recolor: the shell's snow at this pixel in the layers'
		// own colour domain, its normal xy and RMAOS, and the share of the
		// blend that took it.
		bool landRecolor;
		uint landSnowBits;
		float3 landSnowRaw;
		float2 landSnowNormal;
		float4 landSnowRMAOS;
		float landSnowFrac;
#if defined(SNOW_BODY_COAT)
		// The body coat's opacity here, for the skin's SSS mask.
		float coatAlpha;
#endif
	};

	// How much a texel reads as snow: bright and grey, on the texture's own
	// encoded values. A snowy rocks, grass snow or path layer keeps its rock,
	// grass and dirt texels. Shared by the land and snow-textured recolors.
	float LandSnowTexel(float3 g)
	{
		float lum = dot(g, float3(0.2126, 0.7152, 0.0722));
		float sat = max(g.r, max(g.g, g.b)) - min(g.r, min(g.g, g.b));
		float lo = SharedData::snowDeformationSettings.LandRecolorLumLo;
		return smoothstep(lo, lo + 0.18, lum) * (1.0 - smoothstep(0.10, 0.22, sat));
	}

	// A raw base texture sample as encoded: TRUE_PBR decodes it to linear.
	float3 SampleEncoded(float3 sampled)
	{
#if defined(TRUE_PBR)
		return TransferFunctions::LinearToGamma22(max(sampled, 0.0));
#else
		return sampled;
#endif
	}

#if defined(LANDSCAPE)
	// A land layer as encoded. The layers arrive converted: TRUE_PBR's linear,
	// otherwise ColorManagement::DiffuseToWorking's, inverted here.
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

	// Snow grain on a recolored surface, in the drape's frame. texN: the snow
	// normal map through the plane SampleStochastic took with triWeights
	// (DirectX map, +G along +v; x plane yz, y plane xz, z plane xy).
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

	// The shell's material response (SnowShell.hlsl): roughness and F0 from an
	// RMAOS texel of its set under the set's scales, AO as authored, metallic 0.
	float4 ShellRMAOS(float4 r)
	{
		return float4(clamp(r.x * SharedData::snowDeformationSettings.SnowRoughnessScale, 0.05, 1.0), 0.0, r.z, r.w * SharedData::snowDeformationSettings.SnowSpecularLevel);
	}

	// Its defaults (kSnowRoughness, kSnowF0) where the set has no RMAOS map:
	// DebugTerrainOverlay bit 512 says HorizonSnowRmaos is bound with the set.
	bool ShellHasRMAOS()
	{
		return (uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 512) != 0;
	}
	static const float4 kShellRMAOSDefault = float4(0.6, 0.0, 1.0, 0.028);

#if defined(LANDSCAPE) || ((defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR))
	// The shell's snow set through the shell's own anti-tiling taps
	// (SnowParallax.hlsli ComputeSnowTapsNoGrad) at the shell's tiling, for a
	// camera-relative position: every texel lands where the shell's would.
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

	// worldXY: worldRelXY + CameraPosAdjust. Takes derivatives: call in
	// uniform flow.
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
	// Per-tile snow detection: how much of this pixel's landscape blend is
	// snow material. Only the terrain overlay reads it.
	void LandSnowness(inout State s, float4 blendWeights1, float2 blendWeights2)
	{
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 1) != 0)
		{
#	if defined(TRUE_PBR)
			// PBR terrain replaces the vanilla per-layer snow constants, so the
			// CPU side publishes per-tile snow-material bits via the permutation
			// data (see SnowDeformation::BSLightingShader_SetupMaterial).
			uint snowTileBits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
			float4 snowIsSnow1to4 = float4(snowTileBits & 1, (snowTileBits >> 1) & 1, (snowTileBits >> 2) & 1, (snowTileBits >> 3) & 1);
			float2 snowIsSnow5to6 = float2((snowTileBits >> 4) & 1, (snowTileBits >> 5) & 1);
			s.landSnowness = saturate(dot(blendWeights1, snowIsSnow1to4) + dot(blendWeights2, snowIsSnow5to6));
#	else
			s.landSnowness = saturate(dot(blendWeights1, LandscapeTexture1to4IsSnow) + blendWeights2.x * LandscapeTexture5to6IsSnow.x + blendWeights2.y * LandscapeTexture5to6IsSnow.y);
#	endif
		}
	}

	// Re-run after EM height blending has rewritten the weights, so the
	// verdict matches the layer actually shown.
	void LandSnownessPostBlend(inout State s, float4 blendWeights1, float2 blendWeights2)
	{
		LandSnowness(s, blendWeights1, blendWeights2);
	}

	// Snow on Landscape's edge verdict (DebugTerrainOverlay bit 128): log2 odds
	// of the module's snow layers (the per-draw permutation bits) against the
	// rest, after EM's height blend, clamped to +-16 and packed into the land's
	// (0.004, 1]. Decoded by SnowShell.hlsl LandSnowOdds. Masks is written
	// opaque: under Terrain Blending's alpha the odds mixed with the code of
	// the object base behind the land.
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
	// EM's parallax path fades its height blend over 1024-2048 units of view
	// depth and hands back the vertex weights past it, so the snow edge (and
	// the odds above) became the vertex grid's linear ramp: grey patches with
	// straight sides. Where the land mixes the module's snow layers with
	// others, the blend keeps EM's full strength at any distance.
	void LandSnowEdgeBlend(PS_INPUT input, float viewDepth, float screenNoise, float2 uv, float mipLevels[6], DisplacementParams params[6], StochasticOffsets sharedOffset, inout float weights[6])
	{
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 128) != 0 && SharedData::extendedMaterialSettings.EnableHeightBlending && viewDepth > 1024.0)
		{
			uint bits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
			float snow = dot(input.LandBlendWeights1, float4(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1)) + dot(input.LandBlendWeights2.xy, float2((bits >> 4) & 1, (bits >> 5) & 1));
			float rest = dot(input.LandBlendWeights1, 1.0) + input.LandBlendWeights2.x + input.LandBlendWeights2.y - snow;
			[branch] if (snow > 0.01 && rest > 0.01)
			{
				// EM's full-strength inputs (ExtendedMaterialsParallaxCore.hlsli).
				float full[6];
				ExtendedMaterials::GetTerrainHeight(screenNoise, input, uv, mipLevels, params, 1.0,
					smoothstep(0.0, 1.0, input.LandBlendWeights1), smoothstep(0.0, 1.0, input.LandBlendWeights2.xy), sharedOffset, full);
				// EM's own fade, so the hand-over at 1024 is continuous.
				float far = smoothstep(1024.0 * 1024.0, 2048.0 * 2048.0, viewDepth * viewDepth);
				[unroll] for (uint i = 0; i < 6; i++)
					weights[i] = lerp(weights[i], full[i], far);
			}
		}
	}
#	endif

	// Land Snow Recolor (DebugTerrainOverlay bit 256): the module's snow layers
	// wear the shell's snow set texel by texel, so wherever the land shows
	// through the shell, its snow is the shell's snow.
	void LandRecolorBegin(inout State s, float3 worldRel)
	{
		uint bits = (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLandIsSnowMask) >> Permutation::ExtraFeatureFlags::SnowLandIsSnowShift;
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 256) != 0 && bits != 0)
		{
			float2 worldXY = worldRel.xy + FrameBuffer::CameraPosAdjust.xy;
			const SnowSetTaps taps = SnowSetTapsAt(worldRel.xy, worldXY);

			float3 a = SnowSetSample(SnowDeformation::HorizonSnowAlbedo, taps).rgb;
			// Shell albedo convention (SnowShell.hlsl kSnowAlbedo), then the layer
			// value the land's own Color::Albedo turns back into it.
			float3 shellAlbedo = SnowColor::SetAlbedo(a, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
			// Wet snow (Snow Climate): a print through to the land's snow presses
			// dark and glossy like the shell's thin print beside it. The tint is
			// in the shell's albedo convention; a linear set takes it linearised.
			const float wet = SharedData::snowDeformationSettings.LandWetPrint;
			// Refrozen (Wet Snow and Ice): a pressed print glazes like the shell's,
			// the rest of the snow by the sun crust's share. Mirror kIceTint,
			// kIceRoughCut and kIceSurfaceGlaze in SnowFields.hlsli.
			const float ice = SharedData::snowDeformationSettings.LandIce;
			float press = 0.0;
			float glaze = 0.0;
			[branch] if (max(wet, ice) > 0.001)
			{
				const float pressed = smoothstep(0.1, 0.5, SnowDeformation::GetDeformation(worldXY));
				press = pressed * saturate(wet * 4.0);
				glaze = max(pressed, 0.1) * ice;
			}
			const float3 printTint = lerp(1.0, float3(0.76, 0.77, 0.80) * (1.0 - 0.25 * wet), press) * lerp(1.0, float3(0.93, 0.955, 0.985), glaze);
			shellAlbedo *= printTint;
			// The layers arrive converted (Common/LightingLandscape.hlsli): TRUE_PBR's
			// linear, otherwise already the albedo.
#	if defined(TRUE_PBR)
			s.landSnowRaw = Color::PBRMaterialToLinear(shellAlbedo);
#	else
			s.landSnowRaw = shellAlbedo;
			s.lodAlbedo = shellAlbedo;
#	endif
			// Classification debug (bit 4): the recolored texels in magenta too.
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
			// The shell's material defaults; its RMAOS map where the set has one.
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

	// The shell's grain on the recolored share, the shell's own perturbation
	// (world +X/+Y frame, SnowShell.hlsl). The layers' own grain was flattened
	// out of it above. Vanilla land then re-lights that share like horizon snow.
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
		// Diagnostic overlay: R = outside deformation window, G = raw
		// deformation sample, B = detected snowness.
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 1) != 0)
		{
			float2 debugWorldXY = worldXYRel + FrameBuffer::CameraPosAdjust.xy;
			float2 debugUV = SnowDeformation::GetDeformationUV(debugWorldXY);
			float debugOutside = (all(debugUV > 0.0) && all(debugUV < 1.0)) ? 0.0 : 1.0;
			float debugDeformation = SnowDeformation::GetDeformation(debugWorldXY);
			baseColor = lerp(baseColor, float3(debugOutside, debugDeformation, snowness), 0.75);
		}
	}
#endif  // LANDSCAPE

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
	// Horizon snow: where the game's own LOD terrain bake reads as snow,
	// wear the shell's snow albedo instead, through the shell's taps, so the
	// shell's geometry hands off to identically-dressed terrain beyond its
	// reach. No normal map: the shell's own fades out by 2200 units.
	// Classification runs on the raw (gamma) bake, matching the window fill's
	// thresholds; the world-space normal.z gates cliffs back to rock even where
	// the bake is pale. Shading happens at the write tail (RelightLOD, the
	// shell's recipe on the OUTPUTS).
	void LODRecolor(inout State s, float3 rawBaseColor, float2 uv, float2 worldXYRel, float3 normal)
	{
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
		// Plain object-LOD batches flagged by the statics hook: their baked snow
		// (drifts, roads, piles in the atlas) takes the horizon recipe. The
		// bake covers the mesh whole, so only undersides are kept out, and only
		// solid texels: a texture cut out within two mips of the drawn texel
		// (tree cards, leaves, fences) is never snow.
		bool snowLodOn = SharedData::snowDeformationSettings.LODObjectEnable > 0.5 &&
		                 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLODBakedIsSnow) != 0;
		// Two mips past the draw's own sample; the gradients stay out of flow control.
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
			// Opaque at 8 bits.
			[branch] if (lodReplaceW > 0.003)
				lodReplaceW *= TexColorSampler.SampleGrad(SampColorSampler, uv, coverDx, coverDy).a >= 254.5 / 255.0 ? 1.0 : 0.0;
#	else
			float lodReplaceW = lodSnowScore * lodReplaceT * smoothstep(0.35, 0.65, normal.z);
#	endif
			const SnowSetTaps taps = SnowSetTapsAt(worldXYRel, worldXYRel + FrameBuffer::CameraPosAdjust.xy);
			[branch] if (lodReplaceW > 0.003)
			{
				float3 snowSample = SnowSetSample(SnowDeformation::HorizonSnowAlbedo, taps).rgb;
				// Shell albedo convention (SnowShell.hlsl kSnowAlbedo).
				s.lodReplaceW = lodReplaceW;
				s.lodAlbedo = SnowColor::SetAlbedo(snowSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
				// Classification debug (bit 4): the baked recipe's pixels in magenta too.
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					s.lodAlbedo = float3(1.0, 0.0, 1.0);
			}
		}
	}
#endif

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(LANDSCAPE)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
	// Horizon snow, shell recipe (SNOW-MATCH Phase 1): replaced pixels are
	// re-evaluated through the SAME PBR functions the shell and TruePBR
	// statics use - the context already carries this pixel's normal, view
	// ray, and the EHF/world-shadowed sun; the compensation
	// reconstructs the TRUE_PBR-flavoured light input this non-PBR
	// permutation never applied (PI-CONVENTION-SPIKE.md), and GetDirect-
	// LightInput's Lambert cancels it back out. Direct + Fresnel-weighted
	// lobe x ambient, all x PBRLightingScale, and the sun GGX and environment
	// lobes the shell writes; no vertex color - the shell has none.
	void RelightLOD(State s, DirectContext dirLightContext, IndirectContext indirectContext, float3 directionalAmbientColor,
		inout float3 color, inout float3 outputAlbedo, inout float3 specularColor, inout float3 lobeSpecular, inout float roughness)
	{
		[branch] if (s.lodReplaceW > 0.003)
		{
			MaterialProperties snowMaterial = (MaterialProperties)0;
			snowMaterial.BaseColor = s.lodAlbedo;
			// The shell's material defaults (SnowShell.hlsl kSnowRoughness/kSnowF0)
			// under the same roughness scale: scalar stand-ins for its RMAOS map,
			// which mips flat at LOD range anyway.
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
	// Runtime-applied projections (Seasons of Skyrim) carry the record
	// default max angle (cos 0) and no vertex-alpha mask, so every face short
	// of an overhang paints. Stand in for the mask an author would have
	// painted: a slope cut at the max angle, applied to the alpha so tops
	// keep the projection's own full weight and noise (a threshold
	// subtracted from the weight left only patches) and steeper faces go
	// bare. The game's own paint, the sparkle discard and the recolor all
	// follow it.
	float ProjectionAlpha(State s, float vertexAlpha, float projDot)
	{
		// Seasons' own snow statics carry no authored alpha at all (it reads 0):
		// the mask stands in for it rather than scaling it.
		[flatten] if ((Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedNoAlpha) != 0)
			vertexAlpha = 1.0;
		[flatten] if ((Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedUnauthored) != 0)
			vertexAlpha *= smoothstep(SharedData::snowDeformationSettings.ProjUnauthoredThreshold - 0.1, SharedData::snowDeformationSettings.ProjUnauthoredThreshold + 0.1, projDot);
		// Under drawn water the game's own projection paints nothing either,
		// on snow-classified draws only (the sparkle pass then discards).
		[flatten] if (s.underWater && (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
			vertexAlpha = 0.0;
		return vertexAlpha;
	}

	// Snow Trenches on Objects: in a print the object goes bare at any facing.
	// The weight itself drops, to -1 at the core, under every reader's
	// threshold: the vertex alpha only scales the facing term, which is near 0
	// on a wall. The projection's own noise breaks the edge up. Never raised;
	// printShare is 0 off snow-classified draws.
	float PrintBareWeight(State s, float projWeight, float projNoise)
	{
		return lerp(projWeight, min(projWeight, -1.0), SnowPrints::Bare(s.printShare, projNoise));
	}

#	if defined(EMAT) && !defined(LANDSCAPE) && !defined(SKINNED) && !defined(FACEGEN) && !defined(EYE)
	// Snow Follows Parallax: the projected snow at the mesh point whose texel
	// Extended Materials' parallax drew here, so the paint, its texture and
	// the coat that copies its verdict stay on the drawn surface instead of
	// the mesh. Records the shift for the recolor and returns the noise, which
	// shapes the paint's edge, read there; gradients stay the mesh's, or every
	// step in the parallax would blow the mip up. Vanilla parallax shapes
	// paint nothing, so for them this is dead code.
	float ParallaxProjNoise(inout State s, float projNoise, float3 worldRel, float3 triWeights, float2 uv, float2 uvOriginal)
	{
		[branch] if (SharedData::snowDeformationSettings.ParallaxFollow > 0.5 && SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 &&
					 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
		{
			// Fine derivatives: the coarse ones Lighting takes elsewhere would be
			// merged with these and held live across the shader (+1 register).
			const float3 pDx = ddx_fine(worldRel);
			const float3 pDy = ddy_fine(worldRel);
			s.parallaxShift = SnowParallaxFollow::UVShiftToWorld(uv - uvOriginal, ddx_fine(uvOriginal), ddy_fine(uvOriginal), pDx, pDy);
#		if defined(TRUE_PBR)
			// An interlayer shift shows the layer under the coat; the snow lies on the coat.
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
	// Flatten Parallax Under Snow: the share of Extended Materials' parallax this
	// pixel keeps. The relief fades as the projection covers the surface, from
	// the noise-free weight at which vanilla's material first shows (-0.1) to the
	// one every noise value paints in full (noise scale + 0.1). Smooth on purpose:
	// a depth that followed the noise's patches swirled the rock between them.
	// Lighting scales the march's height by it and raises the self-shadow's base
	// height; the coat's march takes the same share (SnowStaticsShell
	// ObjectParallaxShift).
	float ParallaxUnderSnow(float3 worldRel, float3 vertexNormal, float3 texProj, float vertexAlpha)
	{
		float keep = 1.0;
		[branch] if (SharedData::snowDeformationSettings.ParallaxFlatten > 0.5 && SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 &&
					 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0)
		{
#			if defined(TREE_ANIM)
			vertexAlpha = 1.0;
#			endif
			// Runs before BeginObject: the water test is its own.
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
	// SNOW-MATCH Phase 2 round 5: branch-INDEPENDENT. The authored data
	// picks texture vs flat-color projection in Lighting; when the CPU
	// classified this draw's projected material as snow, both paths converge
	// here onto the shell's snow set. Sits after the SPARKLE branch so the
	// multipass snow pass (technique 14, every surviving pixel already snow)
	// takes the same set.
	void ProjectedRecolor(inout State s, float projWeight, float3 vertexNormal, float3 projWorldPos, float screenNoise,
		inout float projectedMaterialWeight, inout float3 baseColor, inout float4 rawRMAOS, inout float3 worldNormal)
	{
		s.projMatch = SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 && !s.underWater &&
		              (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowProjectedIsSnow) != 0;
		[branch] if (s.projMatch)
		{
			// All or nothing at the game's own half blend (projWeight 0, where
			// vanilla's flat-colour path starts painting), in the object's own
			// shader so no angle is missed. The old cut at the last trace of
			// paint turned a faint dusting into full snow (interior floors,
			// 2026-09-16); the coat's kCoatSolidReal reads the same edge.
			// The snow verdict: the RELIEF and the albedo below both ride it.
			float snowVerdict = smoothstep(-0.01, 0.01, projWeight);
			projectedMaterialWeight = snowVerdict;
			[branch] if (snowVerdict > 0.003)
			{
				// Plane weights from the smooth vertex normal. The derivative face
				// normal behind Lighting's triWeights goes through a hard step()
				// mask and flips planes on the quads straddling mesh creases - a
				// line of a different snow texel along the edge.
				float3 snowTriWeights = Triplanar::GetWeights(vertexNormal, vertexNormal);
				// Read where the paint lies (Snow Follows Parallax), with the mesh's gradients.
				const float3 snowPos = projWorldPos + s.parallaxShift;
				float3 snowGradX, snowGradY;
				Triplanar::ComputeGradients(projWorldPos, 1.0 / SnowDeformation::SnowUVTile, snowGradX, snowGradY);
				// THE RELIEF. Vanilla's own projected normal never reaches the
				// G-buffer: Lighting builds worldNormal from the tangent-space
				// normal BEFORE the projected block, and the block's
				// `normal.xyz = lerp(..., finalProjNormal, ...)` is a dead write
				// (upstream too - see CODE-NOTES). So every projected-snow pixel
				// wears the object's own bump, which reads as snow PAINTED ON
				// the plank; only the drape's coat hid it. The shell's own
				// recipe, applied here to the pixels the coat does not own:
				// same additive perturbation and same +Y reference frame as
				// SnowStaticsShell, so coat and bare PD carry one grain and
				// their junction is a seam of height only. The object's own bump
				// goes first, as vanilla's projected normal replaces it: a glacier
				// slab's ice streaks read through the snow (Alftand, 2026-10-23).
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
				// SINGLE RETURN: an early `return` inside a `[branch]` is the
				// X4000 shape this file has paid for twice. Nested branch.
				[branch] if (projectedMaterialWeight > 0.003)
				{
				// The sparkle pass declares no projected-diffuse sampler (s3 is
				// its own texture there); its colour sampler is the same wrap state.
#		if defined(SPARKLE)
				float3 snowProjSample = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY).xyz;
#		else
				float3 snowProjSample = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowAlbedo, SampProjDiffuseSampler, snowPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, snowGradX, snowGradY).xyz;
#		endif
				// Shell albedo convention (SnowShell.hlsl kSnowAlbedo).
				s.projAlbedo = SnowColor::SetAlbedo(snowProjSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
				// Classification debug: everything this block replaces, in magenta.
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					s.projAlbedo = float3(1.0, 0.0, 1.0);
				// Recolor weight view (bit 32): the weight the recolor really
				// blends by, as a grey ramp.
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0)
					s.projAlbedo = projectedMaterialWeight.xxx;
				// Sampled-albedo view (bit 64): the texture read above, raw. The
				// magenta and weight views both REPLACE snowProjSample, so they
				// say nothing about whether HorizonSnowAlbedo reached the draw -
				// and a recolor whose block runs at full weight while the pixel
				// does not change can only be the sample.
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 64) != 0)
					s.projAlbedo = snowProjSample;
				// The shell's response at the albedo's texel. Its scalar stand-ins
				// (0.6 x roughness scale) left the slabs glossier than the ground.
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
				// PBR pixels are convention-correct already: albedo + the shell's
				// response (rawRMAOS.w IS F0).
				baseColor = lerp(baseColor, s.projAlbedo, projectedMaterialWeight);
				rawRMAOS = lerp(rawRMAOS, s.projRMAOS, projectedMaterialWeight);
#		else
				// Vanilla pixels get the albedo here; the write tail re-lights
				// the snow fraction through the PBR evaluators (hue+brightness).
				baseColor = lerp(baseColor, s.projAlbedo, projectedMaterialWeight);
#		endif
				}
			}
		}
		// Recolor weight view (bit 32): projected draws the recolor does not
		// classify as snow go red, so an unpainted wall reads as "not ours".
		[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0 && !s.projMatch)
			baseColor = float3(1.0, 0.0, 0.0);
	}
#	endif
#endif  // PROJECTED_UV

#if (defined(PROJECTED_UV) || defined(SNOW_BODY_COAT)) && !defined(TRUE_PBR) && !defined(WORLD_MAP)
	// SNOW-MATCH Phase 2 round 3: the projected-snow fraction of non-PBR
	// statics re-lit through the SAME PBR evaluators as the shell and the
	// horizon snow. frame7075 measured the fence's snow pass (vanilla
	// ENVMAP+PROJECTED_UV) writing blue-tilted Diffuse - the Phase 0
	// convention signature; a brightness scale cannot fix hue. Sun and
	// ambient rebuilt on the snow albedo saved at the swap; the point-light
	// share is carried over from the vanilla accumulation (the sun's vanilla
	// term subtracted out), scaled into the same units. The wood fraction
	// keeps the vanilla output untouched. The body coat hands its snow
	// fraction over the same way (BodyCoat below).
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
	// Snow-textured shapes without projection (dirt cliff snow tops, rock pile
	// bases and skirts, season swaps, drifts), flagged by the statics hook:
	// the shell's snow set on the texels LandSnowTexel calls snow, on every
	// texel of a plain landscape snow set (SnowTexturedGround). Facing from
	// the vertex normal.
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
				// The texture's own bump goes with its colour; the snow grain replaces it.
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
	// Masks.y is dead for statics (SSS reads it only where Masks.x > 0):
	// carry the projection's verdict for the object snow shell's coat. The
	// target is an 11-bit float, 64 steps an octave: 2 = known and bare,
	// [4, 32) = bare with the weight, three octaves below the half blend
	// (the coat's cut sinks into it as snow accumulates): [-0.3, 0) over the
	// top 5/6, [-0.8, -0.3) over the bottom 1/6. 48 = painted.
	// Landscape keeps (0, 1] for its grain. Mirror: SnowStaticsShell.hlsl.
	void WriteMasksY(State s, float projWeight, inout float masksY)
	{
#	if defined(PROJECTED_UV)
		[flatten] if (s.projMatch)
		{
			float snowBareT = projWeight >= -0.3 ? 1.0 + projWeight * ((5.0 / 6.0) / 0.3) : (projWeight + 0.8) * ((1.0 / 6.0) / 0.5);
			masksY = projWeight >= 0.0 ? 48.0 : (snowBareT > 0.0 ? 4.0 * exp2(3.0 * snowBareT) : 2.0);
		}
#	else
		// A Seasons of Skyrim multipass object's base pass: no projection, so
		// nothing above writes the weight and the skin's read-back saw 0
		// ("unknown") and reconstructed a coat over every face. 2 = known and
		// unpainted; the sparkle pass writes 48 over it where it paints.
		// Same for any multipass snow MATO (vanilla glaciers and ice, Simplicity
		// of Snow, Stretched Snow Begone) when Multipass Snow Follows Paint is on.
		// And for a plain shape of a snow-MATO object (SnowObjectBarePass): it
		// draws over its projected siblings and would otherwise leave 0 there.
		[flatten] if ((Permutation::ExtraFeatureDescriptor & (Permutation::ExtraFeatureFlags::SnowProjectedUnauthored | Permutation::ExtraFeatureFlags::SnowMultipassBase | Permutation::ExtraFeatureFlags::SnowObjectBarePass)) != 0)
			masksY = 2.0;
#	endif
#	if !defined(WORLD_MAP) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE) && !defined(LODOBJECTS) && !defined(LODOBJECTSHD)
		// The snow-textured recolor's verdict, same encoding, on every texel it
		// judged: an unwritten 0 reads "unknown" and the coat reconstructs. An
		// overlay blends into the note beneath it, so it never claims paint.
		[flatten] if (s.texMatch)
			masksY = (s.texWeight >= 0.5 && (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowTexturedOverlay) == 0) ? 48.0 : 2.0;
#	endif
#	if (defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
		// The LOD brightness recolor's weight, same encoding, for the same coat.
		[flatten] if (s.lodReplaceW > 0.003)
			masksY = s.lodReplaceW >= 0.5 ? 48.0 : 2.0;
#	endif
		// Under drawn water: 2 = known, unpainted, so the coat stays off too.
		[flatten] if (s.underWater)
			masksY = 2.0;
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
		// An object LOD's verdict rides x256 (exact in a float): the skin tells a
		// LOD the game drew alone from one hidden inside its full model.
		[flatten] if (masksY >= 1.5)
			masksY *= 256.0;
#	endif
	}
#endif

#if defined(SNOW_BODY_COAT) && !defined(EYE)
	// Snow on bodies (BODY-SNOW-PLAN.md): the draw's record and the coat atlas,
	// bound by the body coat hook with bit 25. SNOW_BODY_COAT is the pixel key
	// skinned draws take while the coat is live (TruePBR.cpp GetPixelTechnique):
	// SKINNED reaches only model-space-normal skin. Mirror: SnowDeformation.h
	// BodyCoatRecordCB, kBodyCoatRecordSlot, kBodyCoatAtlasSlot.
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
	};
	Texture3D<float4> CoatAtlas : register(t41);
	// State.h ExtraFeatureDescriptors::SnowBodyCoat. Declared here rather than
	// in Permutation.hlsli, so permutations without SNOW_BODY_COAT keep their text.
	static const uint kSnowBodyCoatBit = (1u << 25);
	// SnowDeformation.h kBodyCoatFlag bits, and the atlas (kBodyCoatSlotDim x kBodyCoatSlots).
	static const uint kCoatFlatPaint = 1u;
	static const uint kCoatLiving = 2u;
	static const uint kCoatHairSpecks = 4u;
	static const uint kCoatRelief = 8u;
	static const uint kCoatReliefView = 16u;
	static const float3 kCoatAtlasSize = float3(32.0, 16.0, 1536.0);
	// Round 9 (section 5.17; derivations in CODE-NOTES). CoatLook: x one over the
	// angle of repose in radians, yzw the lumps' hole octaves' noise thresholds (10
	// none); CoatAmounts: x the lumps, y the specks, z hair's specks, w a speck's
	// radius in units. Lumps: patches 8 units, clumps 2.5, flakes 0.5; 8 cm deep
	// on an average crown, as opaque as r d / (1 + r d) at r = 1.4 per cm
	// (Perovich 2007), domed 1.5 units, single over the outer 0.35 of their order,
	// holed at their rims at 0.8, 0.4 and 0.2 units. Specks: half the surface at
	// most, full over half their order (hair: out to 105 degrees). Dots cover 70 %
	// with every ball on; a share takes q = L (0.9212 - 0.0764 L) of them, L =
	// -ln(1 - share) (measured, within 1 % of q). The relief is read at 0.2 units,
	// or the pixel when larger. CoatNoise slopes 0.56 per cell. An edge is never
	// sharper than 0.05 of the relief.
	static const float kCoatReliefShare = 0.5;
	static const float3 kCoatClumpFreq = float3(1.0 / 8.0, 1.0 / 2.5, 1.0 / 0.5);
	static const float3 kCoatClumpWeight = float3(0.45, 0.35, 0.2);
	static const float kCoatClumpContrast = 1.6;
	static const float kCoatFullCm = 8.0;
	static const float kCoatPerCm = 1.4;
	static const float kCoatRimSpan = 0.1;
	static const float kCoatEdge = 0.05;
	static const float kCoatLumpUnits = 1.5;
	static const float kCoatLumpFront = 0.35;
	static const float3 kCoatHoleFreq = float3(1.0 / 0.8, 1.0 / 0.4, 1.0 / 0.2);
	static const float kCoatHoleOpen = 0.54;
	static const float kCoatSpeckMax = 0.5;
	static const float kCoatSpeckReach = 1.8326;
	static const float kCoatSpeckFront = 0.5;
	static const float kCoatDotCover = 0.7;
	static const float2 kCoatDotRank = float2(0.9212, -0.0764);
	static const float kCoatPocketRadius = 0.2;
	static const float kCoatNoiseSlope = 0.56;

	float3 CoatDebugHue(float h)
	{
		float3 k = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
		return 0.25 + 0.75 * k;
	}

	// S1's Body Coat View: 1 bands every 8 units of the frame's height and lines
	// every 16 across, which must run unbroken from body to armour to boots; 2
	// the slot's pattern read back from the atlas, magenta past the slot; 3 each
	// slot's colour, a component's striped.
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

	// A vector's coefficients in a frame of rows, v = k.x F[0] + k.y F[1] + k.z F[2],
	// up to scale: of the determinant only the sign is kept.
	float3 CoatFrameSolve(float3x3 F, float3 v)
	{
		const float3 c0 = cross(F[1], F[2]);
		return float3(dot(c0, v), dot(cross(F[2], F[0]), v), dot(cross(F[0], F[1]), v)) * (dot(F[0], c0) < 0.0 ? -1.0 : 1.0);
	}

	float CoatCorner(uint3 cell, float3 f, uint3 o)
	{
		return dot(Random::perlinGradient(Random::murmur3(cell + o, 0x578437ADu)), f - float3(o));
	}

	// Gradient noise on CS's hash with a quintic fade, smooth where cells meet, so
	// a threshold on it draws no seams. Random::perlinNoise eases one cell edge
	// linearly and the next cell's matching edge smoothly: it jumps between them.
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

	// Specks as dots: two balls of a radius to each lattice cell through rest space,
	// the cell twice the radius, so a point meets only the eight cells about its
	// nearest corner; the surface cuts each ball into a disc. A ball shows while its
	// rank is below the share's fraction, so dots appear or go as the share moves and
	// never shift. Crisp while a pixel spans at most a radius, each edge giving back
	// the area its ramp adds; by a diameter they have become their share.
	float CoatDots(float3 c, float footprint, float share, float radius)
	{
		const float s = min(share, kCoatDotCover);
		const float L = -log(1.0 - s);
		const float q = L * (kCoatDotRank.x + kCoatDotRank.y * L);
		const float blend = saturate(footprint / radius - 1.0);
		float dots = 0.0;
		[branch] if (blend < 1.0)
		{
			const float g = 2.0 * radius;
			const float edge = sqrt(max(radius * radius - footprint * footprint / 12.0, 0.0));
			const int3 base = int3(floor(c / g - 0.5));
			float keep = 1.0;
			[unroll] for (uint o = 0; o < 8; ++o)
			{
				const int3 cell = base + int3(o & 1, (o >> 1) & 1, (o >> 2) & 1);
				const uint3 h = Random::pcg3d(asuint(cell));
				const float4 ball0 = float4((h.xxx >> uint3(22, 12, 2)) & 1023u, (h.y >> 22) & 1023u) / 1024.0;
				const float4 ball1 = float4((h.yy >> uint2(12, 2)) & 1023u, (h.zz >> uint2(22, 12)) & 1023u) / 1024.0;
				const float a0 = saturate((edge - length(c - (cell + ball0.xyz) * g)) / max(footprint, 1e-6) + 0.5);
				const float a1 = saturate((edge - length(c - (cell + ball1.xyz) * g)) / max(footprint, 1e-6) + 0.5);
				keep *= (ball0.w < q ? 1.0 - a0 : 1.0) * (ball1.w < q ? 1.0 - a1 : 1.0);
			}
			dots = 1.0 - keep;
		}
		return lerp(dots, s, blend);
	}

	// The armour's relief around a pixel: its normal map read at three points a
	// radius (rest units) away, each normal's lean toward its point measured in
	// world. Positive where they lean in (a groove, an engraving), negative where
	// they lean out (a rim, a rivet); a groove whose walls rise at a reads 0.87a to a.
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
			const float3 n = normalTex.SampleLevel(normalSampler, uv + offset, lod).xyz * 2.0 - 1.0;
			flux += dot(CoatSafeNormalize(n.x * world[0] + n.y * world[1] + n.z * world[2]), CoatSafeNormalize(dPdu * d.x + dPdv * d.y));
		}
		return valid * asin(clamp(-0.5 * flux, -1.0, 1.0));
	}

	// S2's coat, rounds 3 to 9 (sections 5.10-5.17, after Josef's reads and
	// the study of snow on people). Smooth across triangles and glued to the
	// body: the rest-space normals come from the bind pose's own tangent frame,
	// which the coat's vertex shader passes down, the surface's shape straight
	// and its relief (the shading normal) un-skinned through the world and bind
	// frames. Lumps spread from the surfaces facing the snowfall out to the angle
	// of repose over a relief of their own in rest space, single at their front
	// and closing behind it. Specks, dots of a set size as many as Grain Amount
	// sets, lie in the same order, steered by the armour's own relief; too small
	// to draw, they become an even dusting. Hair takes specks only, as many as its
	// own amount sets. The material is the shell's own set in
	// rest space: its albedo convention, grain and RMAOS, as projected snow takes
	// them. TruePBR pixels blend the material; vanilla pixels hand the snow
	// fraction to RelightProjected, which lights it as the ground's. Returns the
	// opacity, for the glint and the SSS mask.
	float BodyCoat(inout State s, float3 modelPosition, float3 worldPos, float3x3 worldFrame, float3x3 bindFrame, Texture2D<float4> normalTex,
		SamplerState normalSampler, float2 uv, float2 uvMesh, float screenNoise,
		inout float projectedMaterialWeight, inout float3 baseColor, inout float4 rawRMAOS, inout float3 worldNormal)
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
				// Every derivative before the per-pixel branch.
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
				// A back face's shading normal is turned to the viewer; its shape turns with it.
				float3 restShape = CoatSafeNormalize(mul(toCanonical, bind[2]));
				restShape = dot(restShape, restRelief) < 0.0 ? -restShape : restShape;
				const float3 t = float3(dot(CoatModelToAtlas[0], p), dot(CoatModelToAtlas[1], p), dot(CoatModelToAtlas[2], p));
				const bool inside = all(t >= CoatAtlasLo.xyz) && all(t < CoatAtlasHi.xyz);
				// Half a texel inside the slot, so the filter never reads a neighbour.
				const float4 voxel = CoatAtlas.SampleLevel(SampColorSampler, clamp(t, CoatAtlasLo.xyz + 0.5, CoatAtlasHi.xyz - 0.5) / kCoatAtlasSize, 0);
				const float3 deposit = GBuffer::DecodeNormal(voxel.ba);
				const float contact = inside ? voxel.r : 0.0;
				const float fall = inside ? voxel.g : 0.0;
				const bool hairSpecks = (CoatFlags & kCoatHairSpecks) != 0;
				const bool flatPaint = (CoatFlags & kCoatFlatPaint) != 0;
				const float footprint = max(length(dcdx), length(dcdy));
				// The order the snow spreads in: 1 facing the snowfall, 0 at the angle of
				// repose, below 0 past it; contact snow by its strength. Lumps (hair has
				// none) lie where their cover drowns the relief, deep however few, holed
				// only at their rims; a clump octave fades once a pixel spans half of it,
				// a hole octave a quarter.
				float cover = 0.0;
				float crown = 0.0;
				float lumpAlpha = 0.0;
				[branch] if (!hairSpecks && CoatAmounts.x > 0.0)
				{
					const float3 octaves = kCoatClumpWeight * saturate(1.0 - 2.0 * footprint * kCoatClumpFreq) *
										   float3(CoatNoise(c * kCoatClumpFreq.x), CoatNoise(c * kCoatClumpFreq.y), CoatNoise(c * kCoatClumpFreq.z));
					const float clump = saturate(0.5 + kCoatClumpContrast * (octaves.x + octaves.y + octaves.z));
					crown = saturate(0.5 + kCoatClumpContrast * (octaves.x + octaves.y));
					const float lumpSlope = acos(clamp(dot(CoatSafeNormalize(lerp(restShape, restRelief, kCoatReliefShare)), deposit), -1.0, 1.0));
					const float lumpOrder = max(contact - 1e-3, 1.0 - lumpSlope * CoatLook.x - 2.0 * (1.0 - fall));
					const float3 holeFade = saturate(0.5 / max(footprint * kCoatHoleFreq, 1e-6) - 1.0);
					const float3 holeNoise = holeFade * float3(CoatNoise(c * kCoatHoleFreq.x + 17.1), CoatNoise(c * kCoatHoleFreq.y + 43.7), CoatNoise(c * kCoatHoleFreq.z + 71.3));
					const float3 holeStep = max(kCoatNoiseSlope * kCoatHoleFreq * footprint * holeFade, 1e-4);
					cover = saturate((lumpOrder - (1.0 - CoatAmounts.x)) / kCoatLumpFront);
					const float lie = cover * (1.0 + kCoatEdge) - (1.0 - clump);
					const float rim = saturate(lie / kCoatRimSpan);
					const float depthCm = kCoatFullCm * (0.5 + clump) * rim;
					const float3 lumpHoles = saturate((holeNoise - CoatLook.yzw - lerp(-kCoatHoleOpen, kCoatHoleOpen, rim)) / holeStep + 0.5);
					lumpAlpha = saturate(lie / max(fwidth(lie), kCoatEdge)) * kCoatPerCm * depthCm / (1.0 + kCoatPerCm * depthCm) *
								(1.0 - lumpHoles.x) * (1.0 - lumpHoles.y) * (1.0 - lumpHoles.z);
				}
				// The specks: Grain Amount's share of the surface (hair: its own amount) in
				// the same order, full over half of it (hair: its shape, out to 105
				// degrees). The armour's relief steers them, up to twice the share in its
				// grooves and none on its rims.
				const float speckSlope = acos(clamp(dot(hairSpecks ? restShape : restRelief, deposit), -1.0, 1.0));
				const float speckOrder = max(contact - 1e-3, 1.0 - speckSlope * (hairSpecks ? 1.0 / kCoatSpeckReach : CoatLook.x) - 2.0 * (1.0 - fall));
				float pocket = 0.0;
				[branch] if (!hairSpecks && (CoatFlags & (kCoatRelief | kCoatReliefView)) != 0)
					pocket = (dot(worldNormal, world[2]) < 0.0 ? -1.0 : 1.0) *
							 CoatPocket(normalTex, normalSampler, uv, duvdx, duvdy, dcdx, dcdy, worldDx, worldDy, world, max(kCoatPocketRadius, footprint));
				const float steer = (CoatFlags & kCoatRelief) != 0 ? 1.0 + clamp(pocket * CoatLook.x / kCoatSpeckFront, -1.0, 1.0) : 1.0;
				const float speckShare = kCoatSpeckMax * (hairSpecks ? CoatAmounts.z : CoatAmounts.y) * saturate(speckOrder / kCoatSpeckFront) * steer;
				float speckAlpha = 0.0;
				[branch] if (speckShare > 0.0 && !flatPaint)
					speckAlpha = CoatDots(c, footprint, speckShare, CoatAmounts.w);
				alpha = flatPaint ? speckShare : max(lumpAlpha, speckAlpha);
				const float snowCover = max(0.75 * alpha, flatPaint ? 0.0 : lumpAlpha);
				// The snow's surface domes over each lump, steepest at its rim, so a full
				// cover stays lumpy; the specks stay flat.
				const float height = kCoatLumpUnits * sqrt(saturate(cover * (1.0 + kCoatEdge) - (1.0 - crown)));
				[branch] if ((CoatFlags & kCoatReliefView) != 0)
				{
					// Show Relief: grooves blue, rims red, full at 30 degrees; no snow.
					const float v = clamp(pocket / 0.5235988, -1.0, 1.0);
					baseColor = float3(saturate(-v), 0.15 * (1.0 - abs(v)), saturate(v));
					alpha = 0.0;
				}
				const float bevelDx = ddx(height);
				const float bevelDy = ddy(height);
				const float3 setDx = dcdx / SnowDeformation::SnowUVTile;
				const float3 setDy = dcdy / SnowDeformation::SnowUVTile;
				[branch] if (alpha > 0.003)
				{
					const float3 weights = Triplanar::GetWeights(restShape, restShape);
					// The snow's own surface in rest space, the shape beneath with the set's
					// grain, carried back to world through the two frames.
					float3 restSnow = restShape;
					[branch] if (SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
					{
						float3 texN = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowNormal, SampColorSampler, c, weights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, setDx, setDy).xyz * 2.0 - 1.0;
						SnowGrain(restSnow, texN, weights, screenNoise, 1.0);
					}
					const float3 snowNormal = CoatSafeNormalize(mul(CoatFrameSolve(bind, mul(restSnow, toCanonical)), world));
					worldNormal = normalize(lerp(worldNormal, snowNormal, snowCover));
					// The rim stands up by its surface gradient (Mikkelsen 2010), from screen
					// derivatives, so it does not depend on the mesh's uv layout; turned with
					// a back face, and never past 45 degrees.
					const float3 r1 = cross(worldDy, world[2]);
					const float3 r2 = cross(world[2], worldDx);
					const float det = dot(worldDx, r1);
					float3 surfaceGradient = (bevelDx * r1 + bevelDy * r2) * (det < 0.0 ? -1.0 : 1.0) / max(abs(det), 1e-8);
					surfaceGradient *= (dot(worldNormal, world[2]) < 0.0 ? -1.0 : 1.0) * rsqrt(max(1.0, dot(surfaceGradient, surfaceGradient)));
					worldNormal = normalize(worldNormal - surfaceGradient);
					const float3 setSample = Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, c, weights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, setDx, setDy).xyz;
					const float3 coatAlbedo = SnowColor::SetAlbedo(setSample, SharedData::snowDeformationSettings.SnowIsLinear > 0.5);
					float4 coatRMAOS = kShellRMAOSDefault;
					[branch] if (ShellHasRMAOS())
						coatRMAOS = ShellRMAOS(Triplanar::SampleStochasticGrad(SnowDeformation::HorizonSnowRmaos, SampColorSampler, c, weights, 1.0 / SnowDeformation::SnowUVTile, screenNoise, setDx, setDy));
#	if defined(TRUE_PBR)
					baseColor = lerp(baseColor, coatAlbedo, alpha);
					rawRMAOS = lerp(rawRMAOS, coatRMAOS, alpha);
					// Its clearcoat, fuzz and subsurface fade under the snow, as under projected snow.
					projectedMaterialWeight = max(projectedMaterialWeight, alpha);
#	else
					// The armour's own albedo stays: the write tail blends the re-lit snow over it.
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
#endif
}

// Land Snow Recolor's per-layer step (Common/LightingLandscape.hlsli hook),
// expanded inside main where the layer's samples and snowState are in scope.
#if defined(LANDSCAPE)
#	undef LIGHTING_LANDSCAPE_LAYER_HOOK
#	if defined(TRUE_PBR)
#		define LIGHTING_LANDSCAPE_LAYER_HOOK(TILE) SnowLighting::LandRecolorLayer(snowState, TILE, weight, landColorRGB, landNormalRGB, landRMAOS);
#	else
#		define LIGHTING_LANDSCAPE_LAYER_HOOK(TILE) SnowLighting::LandRecolorLayer(snowState, TILE, weight, landColorRGB, landNormalRGB);
#	endif
#endif

#endif  // __SNOW_LIGHTING_HLSLI__
