// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Lighting.hlsl's Snow Deformation blocks, as functions. Included by
// Lighting.hlsl just before main, so its samplers, constants and the PBR
// evaluators are in scope; Lighting keeps the call sites only.

#ifndef __SNOW_LIGHTING_HLSLI__
#define __SNOW_LIGHTING_HLSLI__

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD) || defined(PROJECTED_UV)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
// Non-TRUE_PBR permutations that re-light snow through the PBR evaluators:
// the LOD terrain family (horizon snow) and projected-snow statics. The
// optional-lobe branches inside are TRUE_PBR-gated, so these permutations
// compile the coatless diffuse/lobe core against the vanilla
// MaterialProperties.
#	include "Common/PBR.hlsli"
#endif

namespace SnowLighting
{
	struct State
	{
		float landSnowness;
		// Projected snow: the CPU classified this draw's projected material
		// as snow and the swap ran; the albedo is saved for the write tail.
		bool projMatch;
		float3 projAlbedo;
		// Ground under drawn water takes no recolor and no coat (Masks.y = 2).
		bool underWater;
		float lodReplaceW;
		float3 lodAlbedo;
		float texWeight;
		// Total accumulated lights before the tail clobbers diffuseColor.
		float3 projLightTotal;
	};

#if defined(LANDSCAPE)
	// Per-tile snow detection: how much of this pixel's landscape blend is
	// snow material. Also computed for the debug overlay while the feature is
	// disabled - snow detection is exactly what the overlay exists to verify.
	void LandSnowness(inout State s, float4 blendWeights1, float2 blendWeights2)
	{
		[branch] if (SharedData::snowDeformationSettings.EnableSnowDeformation || (SharedData::snowDeformationSettings.DebugTerrainOverlay & 1) != 0)
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

	void DebugLandOverlay(inout float3 baseColor, float2 worldXYRel, float snowness, float2 uvOriginal)
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

		// Tiling ruler: three grids on the same ground, for measuring the land
		// texture's world-space repeat against the shell's kSnowUVTile.
		// Red = one landscape texture repeat, green = 256 world units (the shell's
		// tile), blue = 4096 (cell boundary; the scale anchor that proves the
		// world XY is right). Red per green IS the tiling ratio.
		[branch] if ((SharedData::snowDeformationSettings.DebugTerrainOverlay & 2) != 0)
		{
			float2 rulerWorldXY = worldXYRel + FrameBuffer::CameraPosAdjust.xy;
			// Constant ~1px lines: distance to the nearest gridline, in units of
			// that grid's own screen-space derivative.
#	define SNOW_RULER_LINE(COORD) \
		(1.0 - smoothstep(0.0, 1.0, (0.5 - abs(frac(COORD) - 0.5)) / max(fwidth(COORD), 1e-9)))

			float2 landLineXY = SNOW_RULER_LINE(uvOriginal);
			float2 tileLineXY = SNOW_RULER_LINE(rulerWorldXY / 256.0);
			float2 cellLineXY = SNOW_RULER_LINE(rulerWorldXY / 4096.0);
#	undef SNOW_RULER_LINE

			float3 rulerColor = 0.0;
			rulerColor.x = max(landLineXY.x, landLineXY.y);
			rulerColor.y = max(tileLineXY.x, tileLineXY.y);
			rulerColor.z = max(cellLineXY.x, cellLineXY.y);
			baseColor = lerp(baseColor, rulerColor, saturate(dot(rulerColor, 1.0)));
		}
	}
#endif  // LANDSCAPE

#if (defined(LODLANDSCAPE) || defined(LODLANDNOISE) || defined(LODOBJECTS) || defined(LODOBJECTSHD)) && !defined(WORLD_MAP) && !defined(TRUE_PBR)
	// Horizon snow: where the game's own LOD terrain bake reads as snow,
	// wear the shell's snow material instead - same albedo, same world
	// tiling - so the shell's geometry hands off to identically-dressed
	// terrain beyond its reach. Classification runs on the raw (gamma)
	// bake, matching the window fill's thresholds; LOD meshes carry
	// model-space normals ~ world space, so normal.z gates cliffs back to
	// rock even where the bake is pale. Shading happens at the write tail
	// (RelightLOD, the shell's recipe on the OUTPUTS).
	void LODRecolor(inout State s, float3 rawBaseColor, float2 worldXYRel, inout float3 normal)
	{
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
		// Plain object-LOD batches flagged by the statics hook: their baked snow
		// (drifts, roads, piles in the atlas) takes the horizon recipe. The
		// bake covers the mesh whole, so only undersides are kept out.
		bool snowLodOn = SharedData::snowDeformationSettings.LODObjectEnable > 0.5 &&
		                 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLODBakedIsSnow) != 0;
#	else
		bool snowLodOn = SharedData::snowDeformationSettings.LODReplaceEnable > 0.5;
#	endif
		[branch] if (snowLodOn)
		{
			float lodSnowScore = SnowDeformation::ClassifyLODSnow(rawBaseColor);
			float lodReplaceT = saturate((length(worldXYRel) - SharedData::snowDeformationSettings.LODReplaceStart) * SharedData::snowDeformationSettings.LODReplaceFadeInv);
#	if defined(LODOBJECTS) || defined(LODOBJECTSHD)
			float lodReplaceW = lodSnowScore * lodReplaceT * smoothstep(0.0, 0.3, normal.z);
#	else
			float lodReplaceW = lodSnowScore * lodReplaceT * smoothstep(0.35, 0.65, normal.z);
#	endif
			[branch] if (lodReplaceW > 0.003)
			{
				// frac + explicit gradients: correct mip selection across the
				// tile seam regardless of the sampler's address mode.
				float2 snowRawUV = (worldXYRel + FrameBuffer::CameraPosAdjust.xy) / SnowDeformation::SnowUVTile;
				float3 snowSample = SnowDeformation::HorizonSnowAlbedo.SampleGrad(SampColorSampler, frac(snowRawUV), ddx(snowRawUV), ddy(snowRawUV)).rgb;
				// Shell albedo convention (SnowShell.hlsl kSnowAlbedo):
				// sRGB-encoded, no vanilla Diffuse() processing.
				s.lodReplaceW = lodReplaceW;
				s.lodAlbedo = SharedData::snowDeformationSettings.SnowIsLinear > 0.5 ? Color::LinearToSrgb(snowSample) : snowSample;
				// Classification debug (bit 4): the baked recipe's pixels in magenta too.
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					s.lodAlbedo = float3(1.0, 0.0, 1.0);
				// Normal-map parity with the shell: perturb the LOD normal by the
				// snow normal at the same world tiling. LOD normals are world-
				// space up-ish, so a world-axis tangent frame is stable here.
				// Feeds N.L and the DALC/IBL ambient.
				[branch] if (SharedData::snowDeformationSettings.SnowHasNormal > 0.5)
				{
					float3 snowNormalTS = SnowDeformation::HorizonSnowNormal.SampleGrad(SampColorSampler, frac(snowRawUV), ddx(snowRawUV), ddy(snowRawUV)).xyz * 2.0 - 1.0;
					float3 lodTangent = normalize(cross(float3(0.0, 1.0, 0.0), normal));
					float3 lodBitangent = cross(normal, lodTangent);
					float3 snowWorldNormal = normalize(snowNormalTS.x * lodTangent + snowNormalTS.y * lodBitangent + max(snowNormalTS.z, 0.05) * normal);
					normal = normalize(lerp(normal, snowWorldNormal, lodReplaceW));
				}
			}
		}
	}

	// Horizon snow, shell recipe (SNOW-MATCH Phase 1): replaced pixels are
	// re-evaluated through the SAME PBR functions the shell and TruePBR
	// statics use - the context already carries this pixel's perturbed
	// normal, view ray, and the EHF/world-shadowed sun; the compensation
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

#	if !defined(FACEGEN) && !defined(MULTI_LAYER_PARALLAX) && !defined(PARALLAX)
	// SNOW-MATCH Phase 2 round 5: branch-INDEPENDENT. The authored data
	// picks texture vs flat-color projection in Lighting; when the CPU
	// classified this draw's projected material as snow, both paths converge
	// here onto the shell's snow set. Sits after the SPARKLE branch so the
	// multipass snow pass (technique 14, every surviving pixel already snow)
	// takes the same set.
	void ProjectedRecolor(inout State s, float projWeight, float3 vertexNormal, float3 projWorldPos, float screenNoise,
		inout float projectedMaterialWeight, inout float3 baseColor, inout float4 rawRMAOS)
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
			projectedMaterialWeight = smoothstep(-0.01, 0.01, projWeight);
			[branch] if (projectedMaterialWeight > 0.003)
			{
				// Plane weights from the smooth vertex normal. The derivative face
				// normal behind Lighting's triWeights goes through a hard step()
				// mask and flips planes on the quads straddling mesh creases - a
				// line of a different snow texel along the edge.
				float3 snowTriWeights = Triplanar::GetWeights(vertexNormal, vertexNormal);
				// The sparkle pass declares no projected-diffuse sampler (s3 is
				// its own texture there); its colour sampler is the same wrap state.
#		if defined(SPARKLE)
				float3 snowProjSample = Triplanar::SampleStochastic(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, projWorldPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise).xyz;
#		else
				float3 snowProjSample = Triplanar::SampleStochastic(SnowDeformation::HorizonSnowAlbedo, SampProjDiffuseSampler, projWorldPos, snowTriWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise).xyz;
#		endif
				// Shell albedo convention: sRGB-encoded (SnowShell.hlsl:1592).
				s.projAlbedo = SharedData::snowDeformationSettings.SnowIsLinear > 0.5 ? Color::LinearToSrgb(snowProjSample) : snowProjSample;
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
#		if defined(TRUE_PBR)
				// PBR pixels are convention-correct already: albedo + the shell's
				// response stand-ins (rawRMAOS.w IS F0; 0.028 = shell kSnowF0).
				baseColor = lerp(baseColor, Color::ColorToLinear(s.projAlbedo), projectedMaterialWeight);
				rawRMAOS.xyw = lerp(rawRMAOS.xyw, float3(SharedData::snowDeformationSettings.SnowRoughnessScale, 0, 0.028), projectedMaterialWeight);
#		else
				// Vanilla pixels get the albedo here; the write tail re-lights
				// the snow fraction through the PBR evaluators (hue+brightness).
				baseColor = lerp(baseColor, Color::ColorToLinear(s.projAlbedo) * Color::VanillaDiffuseColorMult(), projectedMaterialWeight);
#		endif
			}
		}
		// Recolor weight view (bit 32): projected draws the recolor does not
		// classify as snow go red, so an unpainted wall reads as "not ours".
		[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0 && !s.projMatch)
			baseColor = float3(1.0, 0.0, 0.0);
	}
#	endif

#	if !defined(TRUE_PBR) && !defined(WORLD_MAP)
	// SNOW-MATCH Phase 2 round 3: the projected-snow fraction of non-PBR
	// statics re-lit through the SAME PBR evaluators as the shell and the
	// horizon snow. frame7075 measured the fence's snow pass (vanilla
	// ENVMAP+PROJECTED_UV) writing blue-tilted Diffuse - the Phase 0
	// convention signature; a brightness scale cannot fix hue. Sun and
	// ambient rebuilt on the snow albedo saved at the swap; the point-light
	// share is carried over from the vanilla accumulation (the sun's vanilla
	// term subtracted out), scaled into the same units. The wood fraction
	// keeps the vanilla output untouched.
	void RelightProjected(State s, float projectedMaterialWeight, DirectContext dirLightContext, IndirectContext indirectContext, float3 directionalAmbientColor,
		float3 dirLightColor, float3 worldNormal, float dirDetailedShadow,
		inout float3 color, inout float3 outputAlbedo, inout float3 specularColor, inout float3 lobeSpecular, inout float roughness)
	{
		[branch] if (s.projMatch && projectedMaterialWeight > 0.003)
		{
			MaterialProperties snowMaterial = (MaterialProperties)0;
			snowMaterial.BaseColor = s.projAlbedo;
			snowMaterial.Roughness = clamp(0.6 * SharedData::snowDeformationSettings.SnowRoughnessScale, 0.05, 1.0);
			snowMaterial.F0 = 0.028;
			snowMaterial.AO = 1.0;
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
#	endif
#endif  // PROJECTED_UV

#if !defined(WORLD_MAP) && !defined(LANDSCAPE) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE) && !defined(LODOBJECTS) && !defined(LODOBJECTSHD)
	// Snow-textured shapes without projection (a dirt cliff's snow01 top, a
	// season swap's alternate set, drifts), flagged by the statics hook: the
	// shell's snow set at the texel's own brightness, in the object's own
	// shader. The weight is written back like projected snow for the coat.
	void SnowTexturedRecolor(inout State s, float3 rawBaseColor, float3 worldNormal, float3 worldRel, float screenNoise,
		inout float3 baseColor, inout float4 rawRMAOS)
	{
		[branch] if (SharedData::snowDeformationSettings.SnowTexturedEnable > 0.5 && !s.underWater &&
					 (Permutation::ExtraFeatureDescriptor & Permutation::ExtraFeatureFlags::SnowLODBakedIsSnow) != 0)
		{
			s.texWeight = SnowDeformation::ClassifyLODSnow(rawBaseColor) * smoothstep(0.35, 0.65, worldNormal.z);
			[branch] if (s.texWeight > 0.003)
			{
				float3 snowTexWorld = worldRel + FrameBuffer::CameraPosAdjust.xyz;
				float3 snowTexWeights = Triplanar::GetWeights(worldNormal, worldNormal);
				float3 snowTexSample = Triplanar::SampleStochastic(SnowDeformation::HorizonSnowAlbedo, SampColorSampler, snowTexWorld, snowTexWeights, 1.0 / SnowDeformation::SnowUVTile, screenNoise).xyz;
				float3 snowTexAlbedo = SharedData::snowDeformationSettings.SnowIsLinear > 0.5 ? Color::LinearToSrgb(snowTexSample) : snowTexSample;
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 4) != 0)
					snowTexAlbedo = float3(1.0, 0.0, 1.0);
				[flatten] if ((uint(SharedData::snowDeformationSettings.DebugTerrainOverlay) & 32) != 0)
					snowTexAlbedo = s.texWeight.xxx;
#	if defined(TRUE_PBR)
				baseColor = lerp(baseColor, Color::ColorToLinear(snowTexAlbedo), s.texWeight);
				rawRMAOS.xyw = lerp(rawRMAOS.xyw, float3(SharedData::snowDeformationSettings.SnowRoughnessScale, 0, 0.028), s.texWeight);
#	else
				baseColor = lerp(baseColor, Color::ColorToLinear(snowTexAlbedo) * Color::VanillaDiffuseColorMult(), s.texWeight);
#	endif
			}
		}
	}
#endif

#if !defined(LANDSCAPE)
	// Masks.y is dead for statics (SSS reads it only where Masks.x > 0):
	// carry the projection's verdict for the object snow shell's coat. The
	// target is an 11-bit float, 64 steps an octave: 2 = known and bare,
	// [4, 32) = bare with the weight, three octaves over 0.3 below the half
	// blend (the coat's cut sinks into it as snow accumulates), 48 = painted.
	// Landscape keeps (0, 1] for its grain. Mirror: SnowStaticsShell.hlsl.
	void WriteMasksY(State s, float projWeight, inout float masksY)
	{
#	if defined(PROJECTED_UV)
		[flatten] if (s.projMatch)
		{
			float snowBareT = saturate(1.0 + projWeight / 0.3);
			masksY = projWeight >= 0.0 ? 48.0 : (snowBareT > 0.0 ? 4.0 * exp2(3.0 * snowBareT) : 2.0);
		}
#	else
		// A Seasons of Skyrim multipass object's base pass: no projection, so
		// nothing above writes the weight and the skin's read-back saw 0
		// ("unknown") and reconstructed a coat over every face. 2 = known and
		// unpainted; the sparkle pass writes 48 over it where it paints.
		// Same for any multipass snow MATO (vanilla glaciers and ice, Simplicity
		// of Snow, Stretched Snow Begone) when Multipass Snow Follows Paint is on.
		[flatten] if ((Permutation::ExtraFeatureDescriptor & (Permutation::ExtraFeatureFlags::SnowProjectedUnauthored | Permutation::ExtraFeatureFlags::SnowMultipassBase)) != 0)
			masksY = 2.0;
#	endif
#	if !defined(WORLD_MAP) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE) && !defined(LODOBJECTS) && !defined(LODOBJECTSHD)
		// The snow-textured recolor's weight, same encoding, for the same coat.
		[flatten] if (s.texWeight > 0.003)
			masksY = s.texWeight >= 0.5 ? 48.0 : 2.0;
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
}

#endif  // __SNOW_LIGHTING_HLSLI__
