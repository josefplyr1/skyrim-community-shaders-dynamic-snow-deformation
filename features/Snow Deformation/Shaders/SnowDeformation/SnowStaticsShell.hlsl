// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Statics snow skin: re-draws captured projected-snow statics (cliffs, rocks,
// drifts, roofs, logs) inflated along their vertex normals, with the same
// snow material as the terrain shell so the two read as one blanket.
//
// Drawn inside DrawShell right after the terrain shell, inheriting its
// bindings: ShellCB (b0), terrain window (t0), deformation map (t1), snow maps
// (t2/t6/t7), s0 and the b4-b6 shared data. Only the input layout, buffers,
// shaders and StaticCB (b1) change per object.
//
// The VS consumes only POSITION and NORMAL, and D3D11 accepts layouts with
// more elements than the shader reads, so one layout per vertex descriptor
// covers every mesh format.
//
// ShellCB must stay layout-identical to SnowShell.hlsl and SnowDeformation.h.

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Triplanar.hlsli"

// Stochastic anti-tiling sampler (same include the terrain shell uses); the
// frost crystal pattern scatters with it.
#include "TerrainVariation/TerrainVariation.hlsli"
#include "SnowDeformation/SnowColor.hlsli"

#ifdef PSHADER
// Same shadow stack as the terrain shell (see SnowShell.hlsl).
#	define TERRAIN_SHADOWS
#	define CLOUD_SHADOWS
#	define VOLUMETRIC_SHADOWS
SamplerState ShellLinearSampler : register(s1);
#	define LinearSampler ShellLinearSampler
#	include "Common/ShadowSampling.hlsli"
#	include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	include "Skylighting/Skylighting.hlsli"
#	include "SnowDeformation/SnowShadow.hlsli"
// Extended Materials' parallax; see SnowShell.hlsl - the POM march routes
// through EM's marcher via the injection point, the soft-shadow taps stay
// local for the two-plane raw-occlusion blend.
#	define EM_PARALLAX_CUSTOM_HEIGHT
float EMParallaxCustomHeight(float2 uv, float mip);
// TerrainVariation.hlsli is already included, so EM takes StochasticOffsets from it.
#	ifndef TERRAIN_VARIATION
#		define TERRAIN_VARIATION
#		define SNOW_EM_TERRAIN_VARIATION
#	endif
#	include "ExtendedMaterials/ExtendedMaterials.hlsli"
#	ifdef SNOW_EM_TERRAIN_VARIATION
#		undef TERRAIN_VARIATION
#		undef SNOW_EM_TERRAIN_VARIATION
#	endif
// Routed PBR tail (defines TRUE_PBR + GLINT for everything it pulls in,
// including the glint NDF) - must come after the includes above (they must
// compile without TRUE_PBR); SnowLights calls into it, so it comes last.
#	include "SnowDeformation/SnowShading.hlsli"
#	include "SnowDeformation/SnowLights.hlsli"
#endif

cbuffer ShellCB : register(b0)
{
	row_major float4x4 CameraViewProj;
	row_major float4x4 CameraViewProjUnjittered;
	row_major float4x4 CameraPreviousViewProjUnjittered;
	row_major float4x4 CameraView;

	float4 ShellCameraPosAdjust;
	float4 ShellCameraPreviousPosAdjust;

	float2 GridOrigin;
	float GridSpacing;
	float TerrainTexelSize;

	float2 GridToTerrainOffset;
	float2 GridToDeformOffset;

	float WarpedHalfSpan;
	uint LayoutKeeper6;  // layout keeper, 0
	uint TerrainDim;
	uint TrenchDetailView;

	float DeformInvWorldSize;
	uint HasSnowTexture;
	float SnowTextureIsLinear;
	float HasSnowNormal;

	float HasSnowRmaos;
	float SnowRoughnessScale;
	float2 SnowUVOffset;

	float4 SnowGlintParams;  // x logDensity, y microfacetRoughness, z densityRandomization, w screenSpaceScale

	float SnowSpecularLevel;
	float EnableGlints;
	float TrenchDetailRelief;
	float TrenchDetailRoughness;

	float LayoutKeeper5;  // layout keeper, 0
	float TrenchDetailCoverage;
	float ShellCullBare;   // was SeamFadeUnused; landscape shell only, unread here
	float SkinFadeStart;  // statics-skin distance dissolve band (units)

	float SkinFadeEnd;
	// Object field maps: 0 = unbound, 0.5 = bound. A gate, never a magnitude.
	float ObjectLiftCap;
	float2 ObjectHeightCenter;

	float ObjectHeightHalfExtent;
	// The game's cascade atlas is bound at t22 this frame, read in place
	// (else the shader falls back to the blurred VSM path).
	float CrispShadows;
	// Screen-Space Shadows output is bound at t45: the long-range
	// depth-marched shadows that carry distant LOD tree shadows beyond the
	// two cascades.
	float ScreenSpaceShadowsActive;
	// Dune-field amplitude in world units (landscape shell only).
	float UndulationAmp;

	// Multiplier on the dune field's wavelengths (landscape shell only).
	float UndulationScale;
	float SkinTessCapSlope;  // skin tess: min segment length per unit of camera distance (0 = off)
	// LLF cluster buffers bound at t35-t37, point-shadow table at t38.
	float PointLightsActive;
	// Skylighting probe volume bound at t50.
	float SkylightingActive;

	// PBR displacement companion bound at t8.
	float HasSnowHeight;
	// Tessellated relief amplitude in world units (landscape shell only).
	float SnowReliefDepth;
	// Statics debug view: object snow renders decision variables as colors.
	float StaticsDebugView;
	float BermHeightAmp;

	// The second detail set's slice and its weight (see SnowShell.hlsl).
	float SpeedSpoil;
	float LayoutKeeper9;
	// Layout keepers, 0.
	float DebugNoFarPad;
	float LayoutKeeper4;

	float LayoutKeeper7;  // layout keeper, 0
	// The detail's sun self-shadow, 0 = off.
	float TrenchDetailShadow;
	float LayoutKeeper3;
	// The meeting with the landscape sheet (SnowShell.hlsl): band in units,
	// half on each side of the junction.
	float ObjectMeetBand;

	// 1 / Berm Steepness (see SnowShell.hlsl).
	float BermReach;
	// The dominant detail set's slice: 0 powder, 1 wet.
	uint LayoutKeeper10;
	float SeamRampInv;
	// >0.5: read the berm field from the bake at t14 instead of recomputing
	// its 17 taps per call.
	float BermBakeActive;

	// Landscape-shell only; declared so SnowParallax lands on ShellCB's
	// offset (560). Do not drop them.
	float4 SeamBounds;
	float4 ExclusionFieldWindow;

	// Parallax: x = HeightScale, y = self-shadow strength. zw are the
	// landscape shell's occlusion march params; object snow does not march.
	float4 SnowParallax;

	// Landscape-shell rows declared only so BorderStyle lands on ShellCB's
	// offset (624): SnowShadow.hlsli reads its zw for the sun cascades'
	// REAL atlas slices. Do not drop them.
	float4 SpellShading;
	// Track age (SnowFields.hlsli TrackAgeOf), the landscape shell's own.
	float4 TrackAge;
	float4 padCrustLook2;
	// x/y landscape border dials (unused here); zw = sun cascade atlas
	// slices for the crisp shadow path.
	float4 BorderStyle;
	// x spare; w = trench wall slough (Snow Climate, CarveProfile).
	float4 CompactLook;
	// Stage 3 P5 rim lip / teeth; consumed via CarveProfile in the march.
	float4 RimStyle;

	// Baked undulation window (see SnowShell.hlsl): xy = world centre,
	// z = 1/half-extent, w > 0.5 when the bake is live.
	float4 UndulationFieldWindow;

	// Toroidal deformation-map addressing (see SnowShell.hlsl).
	int2 DeformMapOrigin;
	int2 ShellFlags;  // x bit 0: horizon march on (see SnowShell.hlsl)

	float4 FineWindow;  // landscape shell only (SnowShell.hlsl)
	float4 SlopeDrape;  // landscape shell only (SnowShell.hlsl)
	// Debug view 9: x = SkinDepthBias (ULPs), y = SkinSlopeDepthBias.
	float4 DebugSkinDepth;
	// Blood (see SnowShell.hlsl): x = intensity, y = burial clock now,
	// z = game hours now, w = burial snowfall; BloodLook2 x = drying hours,
	// y = wet sheen, z > 0.5 = map live.
	float4 BloodLook;
	float4 BloodLook2;
	float4 BloodLook3;  // landscape shell only (SnowShell.hlsl)
	// Trampled ground (see SnowShell.hlsl); consumed inside TrampleDetailScale.
	float4 TrampleStyle;
	// x = the tessellation target in render px, yz = render viewport half size.
	float4 TessScreen;
	float4 FieldDims;  // landscape shell only (SnowShell.hlsl)
	// Snow Climate (see SnowShell.hlsl): the road's wet print and the
	// condition view read it.
	float4 SnowClimate;
	float4 SnowClimate2;
	float4 SnowClimateOverride;
	float4 SnowClimateLUT[3];
}

cbuffer StaticCB : register(b1)
{
	// Object world transform rows (rotation*scale in xyz, translation in w,
	// absolute world coordinates).
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float ObjectsDepth;  // flat-class depth (walkways, roofs, planks)
	float2 HeightWindowCenter;  // top-down height window (see SnowHeightCapture)
	float HeightHalfExtent;

	// >0.5: SmoothedNormals (VS t10) holds position-averaged normals for
	// this object; pillow inflation for flat split-normal meshes.
	float HasSmoothedNormals;
	float RoundedDepth;  // rounded-class depth (rocks, drifts, logs)
	float VertexCountF;  // index of the flatness-stats element in SmoothedNormals
	// >0.5: the object top raster is bound at PS t11 this draw.
	float HasObjectTop;

	// Debug views 8/9: skin draw ordinal + 1, negated for a decal-state skin.
	float DebugSkinId;

	// >0.5: keep the tuned pre-rework skin behaviour (road and bridge meshes).
	float LegacySkin;

	// was MoundSteepness (Mound Steepness, retired). Slot kept for layout.
	float padMoundSteepness;

	// >0.5: the mesh alpha-tests; only pixels on the scene surface survive.
	float AlphaTested;

	// Drift meshes: coat every facing, no collapse, dissolve or range retire.
	float FullCoat;
	// >0.5: skip the SkinFade distance dissolve (glacier/iceberg captures,
	// whose own baked snow never matches the shell). Mirror in
	// SnowDeformation.h StaticsCB.
	float FadeExempt;
	// >0.5: this skin draw is a road, so the skin steps aside where the patch
	// owns the column. The patch takes its per-texel bit from the skin-depth
	// raster's y channel and does not read this. Mirror in SnowDeformation.h.
	float RoadField;
	// Vanilla projected-UV threshold (projectedUVParams.w) for this draw;
	// -1 = no kProjectedUV on the property (or a tree-anim mesh, whose
	// vertex alpha is wind weight). Feeds debug mode 5 and the S2
	// suppressor. Mirror in SnowDeformation.h.
	float ProjThreshold;
	// The noise term's weight in Lighting's projWeight (ProjectedUVParams.x);
	// 0 without projection data. Mirror in SnowDeformation.h.
	float ProjNoiseWeight;
	// >0.5: Snow Follows Parallax may have moved Lighting's noise read off the
	// mesh on this draw, so the reach keeps the game's noise (no swap). Mirror
	// in SnowDeformation.h.
	float ParallaxFollow;
	// Class override code: 0 = flat classifier decides, 1 = force ROUNDED
	// (mountain/cliff family - a jagged cliff's split normals score "flat";
	// and EVERY PD draw in authored-relief mode, per Josef's call to retire
	// the statistical classifier there), 2 = force FLAT (plank family, the
	// cornice treatment). Mirror in SnowDeformation.h.
	float ClassOverride;
	// projectedUVParams.x - strength of vanilla's projected-noise term for
	// this draw; 0 without projection data. Mirror in SnowDeformation.h.
	float ProjNoiseScale;
	// Steep Face Thinning: share of the reach a vertical face gives up.
	// Mirror in SnowDeformation.h.
	float SteepThin;
	// projectedUVParams.z - the noise map's world-space tiling. Mirror in
	// SnowDeformation.h.
	float ProjNoiseTiling;
	// 2 = the S4 shell owns this draw ("3D Snow on Objects" + projection
	// data): the rolling-ball fillet grown vertically over the
	// fill-covered slice of the projected footprint, coverage = vanilla's
	// weight rebuilt per pixel. 0 = classic path (no projection data, or
	// a road). Encoded as 2 so the >1.5 tests survive any future middle
	// state. Set only with the noise map bound at t21. Mirror in
	// SnowDeformation.h.
	float ProjPixelEnable;
	float padSkinNormalCopy;
	float padShellMinNz;
	// Peel tolerance (kPeelTol; the slider was retired at its default):
	// surfaces within this z-band of a layer's top belong to that layer's
	// plane. Mirror in SnowHeightCapture.hlsl / SnowDeformation.h.
	float PeelTol;
	// > 0.5: this frame's screen-space shadow mask was marched with the
	// object snow in the depth. Mirror in SnowHeightCapture.hlsl / SnowDeformation.h.
	float SkinSssSeen;
	float padMeldSk;

	// Far road level (t40): the road-only top raster over the loaded-cell
	// square, with its own window. FarHalfExtent 0 = off. Roads write one
	// class depth, so the level stores none; FarRoadDepth stands in for it.
	float FarWindowCenterX;
	float FarWindowCenterY;
	float FarHalfExtent;
	float FarRoadDepth;
	// >0.5: drawn in the game's decal mode, after the blended depth was final.
	float DecalMode;

	// >0.5: PreSkinMasks is bound. Mirror in SnowHeightCapture.hlsl /
	// SnowDeformation.h.
	float HasSkinMasksCopy;
	// >0.5: PreDecalMasks at t53 holds the note from before the frame's
	// first decal-mode draw blended it (took padLumpSize's slot).
	// Mirror in SnowDeformation.h and SnowHeightCapture.hlsl.
	float HasPreDecalMasks;
	// How far past the edge the lumps reach: a fraction of the projected
	// weight's own fade on coated draws, a normal-z band elsewhere.
	float EdgeFlankWidth;
	// >0.5 (Recolor Projected Snow): the shell's material coats every
	// pixel the projected snow paints solidly, at zero lift.
	float EdgeCoat;
	// Near clipmap: half-extent of the fine object window, 0 = off. Shares
	// the coarse window's centre.
	float FineHalfExtent;
	// Large-reference LOD batch (0/1), and a snow-textured shape (0/1, Recolor
	// Snow-Textured Meshes): both coat off the written verdict alone. Mirror
	// in SnowHeightCapture.hlsl / SnowDeformation.h.
	float LODBatch;
	float TexturedSnow;

	// Snow Follows Parallax, read by the SNOW_COAT_PARALLAX variants: the
	// object's uv transform (Lighting's TexcoordOffset: offset xy, scale zw)
	// and its parallax: x HeightScale, y the height channel (0 displacement,
	// 3 complex material), zw the dims scale and mip shift that make
	// GetMipLevelFromDims the source's own mip rule. Mirror in SnowDeformation.h.
	float4 ObjParallaxUV;
	float4 ObjParallax;
}

#ifdef SNOW_STATICS_DRAPE
// Drape (S4) draws with the debug views off: what FillSkinDrawCB gives every
// one of them, folded so the paths they switch off compile out.
#	define ObjectsDepth 0.0
#	define RoundedDepth 0.0
#	define LegacySkin 0.0
#	define RoadField 0.0
#	define ClassOverride 1.0
#	define ProjPixelEnable 2.0
#	define StaticsDebugView 0.0
#endif

Texture2D<float4> DeformationMap : register(t1);

// Toroidal map fetch: logical texel (already clamped by the caller) to
// physical; wrap is a mask (pow2 dim). Load-based on purpose - a hardware
// sampler would bilinear across the physical seam. Mirror of SnowShell.hlsl.
int3 DeformTexel(int2 t, int2 dims)
{
	return int3((t + DeformMapOrigin) & (dims - 1), 0);
}

// Baked berm field (BermFieldCS): x = the 17-tap disc average of the
// deformation map, y = the carve's short blur, at the map's own resolution
// and addressing.
Texture2D<float4> BermFieldMap : register(t14);
// 1 per 8x8 physical map tile where the map and the berm field read zero
// within one tile (BermFieldCS UntouchedCS). Unbound reads 0 = touched.
Texture2D<uint> UntouchedTiles : register(t57);
// Blood map + deposit clocks, the deformation map's torus (SnowFields.hlsli).
Texture2D<float4> BloodMap : register(t30);
Texture2D<float2> BloodClock : register(t31);
// Wide exclusion field: the landscape shell's slot (t15) and reader, bound by
// the skin draw.
Texture2D<float2> ExclusionFieldMap : register(t15);
// Baked undulation field (UndulationFieldCS): x = height in world units,
// yz = its +-kUndulationGradStep shading gradient, over UndulationFieldWindow.
Texture2D<float4> UndulationFieldMap : register(t29);
// Vanilla's projected-UV noise map (the BSGraphics default the game's own
// Lighting.hlsl samples for projWeight), bound by the skin draw when
// ProjPixelEnable is set; null and unread otherwise.
Texture2D<float4> ProjNoiseMap : register(t21);
// The game's shadow mask (Lighting's t14): x = sun, one channel per shadow
// light by mask index. Drawn for the scene's own geometry, so only the drape,
// which lies on it, may read it.
Texture2D<float4> GameShadowMask : register(t24);
// Pre-shell MASKS copy (the terrain shell's LandMasksCopy): y carries the
// landscape grain height in (0,1] and, on classified projected-snow statics,
// the projection's verdict (SkinRecolorEnc). The coat and its reach key off
// the paint the game really applied, not a reconstruction of it.
Texture2D<float3> PreSkinMasks : register(t32);
Texture2D<float3> PreDecalMasks : register(t53);

// The terrain window also reaches the patch VS: the road-verge depth blend
// needs the landscape class depth per vertex.
#if defined(PSHADER) || defined(DOMAINSHADER) || ((defined(VSHADER) || defined(HULLSHADER)) && defined(PATCH)) || defined(SNOW_ROAD_MARCH_CS)
Texture2D<float4> TerrainWindow : register(t0);

// Terrain window sample (height, rampDepth, coverage), matching the terrain
// shell's math, for blending the object skin into the ground shell.
float3 SampleTerrainStatics(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));

	float3 s00 = TerrainWindow.Load(int3(t0.x, t0.y, 0)).xyz;
	float3 s10 = TerrainWindow.Load(int3(t1.x, t0.y, 0)).xyz;
	float3 s01 = TerrainWindow.Load(int3(t0.x, t1.y, 0)).xyz;
	float3 s11 = TerrainWindow.Load(int3(t1.x, t1.y, 0)).xyz;

	float3 result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Same scale as the landscape shell's, and NOT because object snow
	// accumulates - it does not, the cap stays pinned. These three call sites
	// are this shader's reference to where the GROUND is: the snow-snow seam
	// band at object bases, the blanket-normal blend across it, and the march's
	// ground horizon. Left unscaled they would describe a landscape that is no
	// longer there, and object bases would seam and sink as it snowed.
	result.y = max(result.y, 0.0) * RimStyle.w + min(result.y, 0.0);
	return result;
}
#endif

// The domain shader samples the displacement companion for tessellated
// relief, so the material block is visible to it as well as the PS.
#if defined(PSHADER) || defined(DOMAINSHADER)
Texture2D<float4> SnowDiffuse : register(t2);
// TruePBR snow companion maps (see SnowShell.hlsl); inherited bindings.
Texture2D<float4> SnowNormalMap : register(t6);
Texture2D<float4> SnowRmaosMap : register(t7);
// Displacement companion (_p): tessellated relief and the parallax
// self-shadow. float4 to match Extended Materials' TexParallaxSampler
// convention; the SRV is single-channel, so only .x carries data.
Texture2D<float4> SnowHeightMap : register(t8);
SamplerState SnowSampler : register(s0);
#endif

// Shared trench-detail shaping (noise, berm shape/bake tap, detail) - the
// verbatim-identical pieces of both shells live in one file (M8).
// Rune glyphs and frost marks, as the landscape shell paints them: on the
// road patch, and on object snow lying on a surface their decal covers
// (SkinShadeInput::markBase), whose decal the overlay then leaves out.
#ifdef PSHADER
#	define SNOW_RUNE_GLYPHS
#endif
#if defined(PSHADER) && (defined(PATCH) || defined(SNOW_OBJECT_PRINTS))
// The patch's shading reads the noise hash table and the trench detail tiles,
// as the ground's does; so does the drapes' print twin around a print.
#	define SNOW_NOISE_TABLE
#endif
#if defined(PSHADER) && defined(SNOW_PATCH_DEFERRED)
// Deferred trench patch: the attribute pass's coarse derivatives of GridLocal
// and of WorldPos.z, read back per pixel (the ground's deferred pass, same
// names). A full-screen pass has no triangle to take them from.
static float2 gDeferredGridDx;
static float2 gDeferredGridDy;
static float gDeferredZDx;
static float gDeferredZDy;
#endif
#if defined(PSHADER) && !defined(SNOW_CONDITION_VIEW) && !defined(PATCH)
// Object snow paints Debugging Options > Snow Condition at runtime (SnowClimate2.z).
#	define SNOW_CONDITION_VIEW_RUNTIME
#endif
#include "SnowDeformation/SnowFields.hlsli"
#if defined(PSHADER) && (defined(SNOW_OBJECT_PRINTS) || defined(VOXEL))
// Snow Trenches on Objects: the drape's print twin, chosen per draw where the
// print map holds prints; Lighting has already bared the print's core. The
// volume's PS takes the same readers (bound by DrawVoxelSnow) for when it
// follows the projected snow onto objects.
#	define SNOW_PRINT_RIM
#	include "SnowDeformation/SnowPrints.hlsli"
#endif
#if defined(PSHADER) && defined(SNOW_OBJECT_PRINTS)
// The dent under a full print, in units (Object Print Depth); the stored
// depth already carries Snow Resistance's share.
static const float kPrintDentDepth = 4.0;
#endif

// Warped band grid, shared with the landscape shell.
#include "SnowDeformation/SnowGrid.hlsli"

// The patch's own band table, run through the shell's walk (WarpAxisT).
// 8-unit core out to 1024 - already at the 4-unit raster's resolution - then
// power-of-two steps to the far road level's reach, 16384 (the far edge of a
// uGridsToLoad 7 square from the camera). Past the coarse raster only roads
// have data, so the outer band is coarse and the hull shader tessellates
// live road quads back to ~16-unit vertices.
//   1024 (128 x 8) | 1280 (16 x 16) | 1536 (8 x 32) | 1792 (4 x 64) | 16384 (57 x 256)
// INVARIANT (see SnowGrid.hlsli): every band start is a multiple of its own
// step and of the origin snap (kPatchSnap = 256): 1024, 1280, 1536 and 1792
// all are.
static const float kPatchBandVerts[kWarpBands] = { 128.0, 16.0, 8.0, 4.0, 57.0 };
static const float kPatchBandMul[kWarpBands] = { 1.0, 2.0, 4.0, 8.0, 32.0 };
static const float kPatchStep = 8.0;
// Quads per axis; mirrored as kPatchGridDim in SnowDeformation.h, which sizes
// the draw. 2 x (128 + 16 + 8 + 4 + 57).
#define kPatchGridDim 426

float2 PatchWarpXY(float2 u)
{
	return float2(WarpAxisT(u.x, kPatchBandVerts, kPatchBandMul, kPatchStep),
		WarpAxisT(u.y, kPatchBandVerts, kPatchBandMul, kPatchStep));
}

// Must match kSnowUVTile in SnowShell.hlsl (the game's landscape tiling:
// 24 repeats per 4096-unit cell).
static const float kSnowUVTile = 4096.0 / 24.0;

// Minimum lift. At exactly zero the skin is coincident with its source mesh
// and z-fights it invisible; a tenth of a unit clears that without reading as
// a coat, so a class slider at 0 is a flat sheet rather than a 1-unit layer.
static const float kMinSkinLift = 0.1;

// S4 shell: the roll edge's z-clearance. The fillet's geometry reaches
// h=0 at the rim; below this lift the material is cut (the last sliver
// would shade coincident with the surface it covers) and the recolored
// PD carries on underneath. Two units (~3 cm).
static const float kProjCoatLift = 2.0;
// How far a LIFTED skin's shading normal leans off the source mesh normal
// toward the position-averaged smoothed one, at full class depth.
//
// SET TO 0 (Josef, 2026-09-01). At "3D Snow Shell Depth" 0 the object and
// landscape shells match perfectly and above it they diverge, which isolated
// the mismatch to exactly this term - it is the only depth-gated shading input
// (bumpFade, the normal-map application, the snow UV, the glint fold and the
// vertex-AO position are all byte-identical between the shells). A
// mesh-AVERAGED normal is a different KIND of normal from the landscape's
// field gradient: on a convex rock it is flatter than the real surface, so
// object snow read uniform and bright beside the ground. Zero makes the
// shading normal depth-INDEPENDENT and equal to the depth-0 case Josef
// verified.
//
// TWO call sites share it on purpose: the S4 dome block re-implements this
// formula inline (SkinShadingNormal is defined further down the file, after
// ApplySkinLift), and the pair silently drifting is what made the X4000 fix
// to the function invisible on the S4 shell - S4 draws set ProjPixelEnable = 2
// and never call it.
static const float kSkinShadeSmooth = 0.0;

// World width of the cornice roll on flat plates, and the band over which a
// surface standing below another counts as sheltered from snowfall.
static const float kCorniceRoll = 4.0;
// View-ray bias (world units) that keeps a zero-lift coat/lump pixel off
// the object's own z.
static const float kEdgeFlankLift = 0.4;
// Reconstructed projected weight (vanilla projWeight + 0.1; the game's blend
// is smoothstep(0,1,5w), 0 at w=0, 1 at w=0.2) at which the coat is solid.
static const float kCoatSolidW = 0.15;
// Edge Lump Reach 1: how far below the game's half blend the coat's cut sinks,
// in projected weight. Under kRecolorWeightSpan, or reach 1 coats every face.
static const float kEdgeReachWeightShift = 0.25;
// Bare weights Lighting encodes below the half blend (mirror: SnowLighting.hlsli
// WriteMasksY): [-kRecolorNearSpan, 0) over the top kRecolorNearShare of the
// band, down to -kRecolorWeightSpan (the noise swap's 0.25 + 0.84 x
// ProjNoiseWeight) below it.
static const float kRecolorWeightSpan = 0.8;
static const float kRecolorNearSpan = 0.3;
static const float kRecolorNearShare = 5.0 / 6.0;
// Reach at which the cut's noise term is fully the hex-tiled read.
static const float kDetileRamp = 0.5;
// Hex-tiled read: triangle-grid cells per noise-map tile (2 sqrt 3).
static const float kDetileGrid = 3.4641016;
// The coat's facing gate: off. The read-back already proves the game painted
// the pixel, and the recolor whitened it, wherever it faces.
static const float kCoatMinNz = -1.0;
// The game's projected weight at which the coat counts a pixel painted: the
// recolor's half blend, so the coat's edge sits where the recolor reads as
// snow rather than at its last trace.
static const float kCoatSolidReal = 0.5;
// Masks.y read-back (Lighting.hlsl, 11-bit float): 2 = known and bare,
// [4, 32) = bare with the projection's weight, 48 = painted; x256 = an object
// LOD the game drew. Returns 0 unknown, 2 bare, 3 painted.
float SkinRecolorEnc(float enc)
{
	enc = enc >= 256.0 ? enc / 256.0 : enc;
	return enc < 1.5 ? 0.0 : (enc >= 40.0 ? 3.0 : 2.0);
}
// The projection's weight under a bare pixel, <= 0; -1 = none written.
float SkinRecolorWeight(float enc)
{
	enc = enc >= 256.0 ? enc / 256.0 : enc;
	const float t = log2(enc * 0.25) / 3.0;
	const float w = t >= 1.0 - kRecolorNearShare ? (t - 1.0) * (kRecolorNearSpan / kRecolorNearShare) :
	                                               t * ((kRecolorWeightSpan - kRecolorNearSpan) / (1.0 - kRecolorNearShare)) - kRecolorWeightSpan;
	return (enc >= 3.5 && enc < 40.0) ? w : -1.0;
}
#ifdef PSHADER
// Projection noise map hex-tiled (Heitz & Neyret 2018): per-vertex hashed
// offsets, squared barycentric blend, variance kept.
float2 DetileOffset(int2 a_vertex)
{
	return float2(Random::pcg2d(asuint(a_vertex))) * (1.0 / 4294967296.0);
}

float DetiledNoisePlane(float2 a_uv, float2 a_dx, float2 a_dy, float a_mean)
{
	const float2 g = a_uv * kDetileGrid;
	const float2 skew = float2(g.x - 0.57735027 * g.y, 1.15470054 * g.y);
	const float2 cell = floor(skew);
	const float2 f = skew - cell;
	const float fz = 1.0 - f.x - f.y;
	const int2 c = int2(cell);
	const bool lower = fz > 0.0;
	float3 w = lower ? float3(fz, f.y, f.x) : float3(-fz, 1.0 - f.y, 1.0 - f.x);
	const int2 v1 = lower ? c : c + int2(1, 1);
	const int2 v2 = lower ? c + int2(0, 1) : c + int2(1, 0);
	const int2 v3 = lower ? c + int2(1, 0) : c + int2(0, 1);
	w *= w;
	w /= dot(w, 1.0);
	const float3 n = float3(ProjNoiseMap.SampleGrad(SnowSampler, a_uv + DetileOffset(v1), a_dx, a_dy).x,
		ProjNoiseMap.SampleGrad(SnowSampler, a_uv + DetileOffset(v2), a_dx, a_dy).x,
		ProjNoiseMap.SampleGrad(SnowSampler, a_uv + DetileOffset(v3), a_dx, a_dy).x);
	return a_mean + dot(w, n - a_mean) * rsqrt(dot(w, w));
}

// The map as Lighting reads it. a_p and the gradients pre-scaled by
// ProjNoiseTiling.
float ProjNoiseGame(float3 a_p, float3 a_triW, float3 a_dx, float3 a_dy)
{
	float n = 0.0;
	[branch] if (a_triW.x > 0.001)
		n += a_triW.x * ProjNoiseMap.SampleGrad(SnowSampler, a_p.yz, a_dx.yz, a_dy.yz).x;
	[branch] if (a_triW.y > 0.001)
		n += a_triW.y * ProjNoiseMap.SampleGrad(SnowSampler, a_p.xz, a_dx.xz, a_dy.xz).x;
	[branch] if (a_triW.z > 0.001)
		n += a_triW.z * ProjNoiseMap.SampleGrad(SnowSampler, a_p.xy, a_dx.xy, a_dy.xy).x;
	return n;
}

// Hex-tiled, clamped to the map's [0, 1].
float ProjNoiseDetiled(float3 a_p, float3 a_triW, float3 a_dx, float3 a_dy)
{
	const float mean = ProjNoiseMap.SampleLevel(SnowSampler, 0.5.xx, 16.0).x;
	float n = 0.0;
	[branch] if (a_triW.x > 0.001)
		n += a_triW.x * DetiledNoisePlane(a_p.yz, a_dx.yz, a_dy.yz, mean);
	[branch] if (a_triW.y > 0.001)
		n += a_triW.y * DetiledNoisePlane(a_p.xz, a_dx.xz, a_dy.xz, mean);
	[branch] if (a_triW.z > 0.001)
		n += a_triW.z * DetiledNoisePlane(a_p.xy, a_dx.xy, a_dy.xy, mean);
	return saturate(n);
}
#endif
// The note through the decal blend: decal mode blends every bound target, Masks
// included. The more painted of the pre-decal snapshot and the live copy wins.
// Lighting writes nothing in (32, 40), so a live note there over a bare
// snapshot is a snow decal's 48 blended in at (live - pre) / (48 - pre):
// painted from half cover. On a snow-textured shape a decal that turned a
// judged note unknown or bare covered it whole (a cliff's roots skirt): bare.
float SkinMasksY(int2 a_px)
{
	float live = PreSkinMasks.Load(int3(a_px, 0)).y;
	[branch] if (HasPreDecalMasks > 0.5)
	{
		float pre = PreDecalMasks.Load(int3(a_px, 0)).y;
		float liveEnc = SkinRecolorEnc(live);
		float preEnc = SkinRecolorEnc(pre);
		float liveN = live >= 256.0 ? live / 256.0 : live;
		float preN = pre >= 256.0 ? pre / 256.0 : pre;
		bool lifted = liveEnc == 2.0 && preEnc < 3.0 && liveN > 32.25 && liveN >= 24.0 + 0.5 * preN;
		bool covered = TexturedSnow > 0.5 && preN >= 1.5 && liveN < 2.5;
		[flatten] if (!covered && (preEnc > liveEnc || (preEnc == liveEnc && SkinRecolorWeight(pre) > SkinRecolorWeight(live))))
			live = pre;
		[flatten] if (covered)
			live = 2.0;
		[flatten] if (lifted)
			live = 48.0;
	}
	return live;
}

static const float kShelterNear = 8.0;
static const float kShelterFar = 32.0;
// Depth a fully sheltered surface keeps. The landscape shell thins under
// roofs to a dusting rather than killing coverage; matching it stops the
// object's own projected snow being exposed where the skin steps aside.
static const float kShelterDust = 1.0;

static const float kRimBandPx = 1.2;
static const float kRimBandMax = 0.25;

// Bilinear deformation sample from grid-local XY (world - GridOrigin);
// matches the terrain shell's window math. Returns 0 outside the window.
float SampleDeformation(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).x;
	float s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).x;
	float s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).x;
	float s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).x;

	// Saturated: melt writes past 1.0 into the refill headroom.
	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}

// ---- Object trench detail (berm shading, crisp grain) ----
// The landscape shell's recipes with the independent Obj* knobs. Berm is
// shading-only on objects: skin topology is the source mesh's (no vertices
// to carry a ridge) and a geometry berm would straddle the patch/skin
// height seam.

// The bake exists wherever the skins draw (it is created with the map).
// x = berm, y = the carve's short blur, zw = the carve's and the berm's dates.
float4 BermFieldPair(float2 gridLocal)
{
	return BermFieldBaked(gridLocal);
}

float BermField(float2 gridLocal)
{
	return BermFieldPair(gridLocal).x;
}

// Spell-mark readers (SampleScorch/SampleMelted), the wide exclusion field,
// kFireMeltFloor, Undulation and CarveProfile all live in SnowFields.hlsli,
// shared with the landscape shell.

#ifdef PATCH
// B-spline bicubic deformation sample; the landscape shell's smoothing,
// ported so patch trench walls CURVE the way landscape trench walls do
// instead of showing bilinear facets.
float PatchDeformBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).x;
	float s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).x;
	float s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).x;
	float s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).x;
	// Saturated: melt writes past 1.0 into the refill headroom.
	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}

float SampleDeformationSmooth(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	float2 t = uv * dims - 0.5;
	float2 i = floor(t);
	float2 f = t - i;

	float2 f2 = f * f;
	float2 f3 = f2 * f;
	float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	float2 w3 = f3 / 6.0;

	float2 g0 = w0 + w1;
	float2 g1 = w2 + w3;
	float2 h0 = i - 1.0 + w1 / g0;
	float2 h1 = i + 1.0 + w3 / g1;

	float v00 = PatchDeformBilinear(float2(h0.x, h0.y), dims);
	float v10 = PatchDeformBilinear(float2(h1.x, h0.y), dims);
	float v01 = PatchDeformBilinear(float2(h0.x, h1.y), dims);
	float v11 = PatchDeformBilinear(float2(h1.x, h1.y), dims);

	return g0.y * (g0.x * v00 + g1.x * v10) + g1.y * (g0.x * v01 + g1.x * v11);
}
#endif

struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
#ifdef SNOW_COAT_PARALLAX
	float2 TexCoord0 : TEXCOORD0;
	float4 Bitangent : BINORMAL0;
#endif
	uint VertexID : SV_VertexID;
};

#ifdef VSHADER
// Position-averaged normals (model space) built by SmoothNormalsCS, indexed
// by vertex id. w=0 entries are unresolved; fall back to the raw normal.
StructuredBuffer<float4> SmoothedNormals : register(t10);
#endif

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float4 CurrentClip : TEXCOORD0;
	float4 PreviousClip : TEXCOORD1;
	float3 WorldPos : TEXCOORD2;
	float3 NormalWS : TEXCOORD3;
	float2 GridLocal : TEXCOORD4;
	float Coverage : TEXCOORD5;
	float Flat : TEXCOORD6;
	// Lift height in world units. Interpolated, so unlike the geometric face
	// normal it varies smoothly across a triangle.
	float Lift : TEXCOORD7;
	// Authored projected-snow factor from ApplySkinLift (1 = no data or
	// disabled). In density mode the PS gates coverage on THIS instead of
	// the facing band: the weight already contains the slope term, and the
	// band's mid-zone partial alpha dithers into a film on low-poly meshes.
	float ProjFactor : TEXCOORD8;
	// Post-shelter depth target for this column (no facing gate, no taper,
	// no distance collapse). The PS rim band scales to it.
	float LiftTarget : TEXCOORD9;
#ifdef SNOW_COAT_PARALLAX
	// The object's uv and tangent frame as Lighting builds them, for marching
	// its parallax. Last, so the variants without them still link.
	float2 ObjUV : TEXCOORD10;
	float3 ObjT : TEXCOORD11;
	float3 ObjB : TEXCOORD12;
	float3 ObjN : TEXCOORD13;
#endif
};

// HULLSHADER included bare (P1, edge-research study): the skin HS reads the
// cone field to size tessellation against rim proximity.
#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER) || defined(HULLSHADER) || defined(SNOW_ROAD_MARCH_CS)
// Top-down object top-surface raster. The patch drapes over it (the VS places
// geometry, the PS clips the silhouette overhang); the skin PS uses it to
// separate its own rim wall from a bare object face.
Texture2D<float> ObjectTopRaw : register(t11);
// Cone-transformed snow surface over the same window: the angle of repose
// already applied, so the edge taper is one read instead of a ring walk.
Texture2D<float> ObjectSnowCone : register(t13);
// Near clipmap level 0: the same top raster over a quarter-width window, at a
// quarter of the coarse texel. Shares HeightWindowCenter; the reach is
// FineHalfExtent (0 = the level is off and every read below stays coarse).
Texture2D<float> ObjectTopFine : register(t34);
// Far road level: the road captures alone, rasterized over the loaded-cell
// square into one top map, with its own window (FarWindowCenterX/Y,
// FarHalfExtent). Readers take it only OUTSIDE the coarse window, so the two
// never disagree where both have data.
Texture2D<float> ObjectTopFar : register(t40);
#endif
// Bound to the patch's VS/HS/DS and, so the skin PS can run the SAME
// ownership test the patch does, to the skin PS as well: the skin must step
// aside exactly where the patch draws and nowhere else.
#if defined(PATCH) || defined(PSHADER) || defined(SNOW_ROAD_MARCH_CS)
// x = class layer depth, y = the highest ROAD surface in the column
// (kNoRoadTop where no road drew).
Texture2D<float2> ObjectSkinDepth : register(t12);
// Mirror of SnowDeformation.h kNoRoadTop.
static const float kNoRoadTop = -1000000.0;
// How far the column's top may stand above the road's own top and still count
// as road-owned. One raster texel is 4 units, so this is two texels of slack
// for camber and for the max-of-4 sampling; anything standing proud of a road
// by more than this is a rock, a wall or a building, not the road.
static const float kRoadOwnsTop = 8.0;
#endif

#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER) || defined(HULLSHADER) || defined(SNOW_ROAD_MARCH_CS)

// Lattice step, world units, of the dense patch's bilinear top and skin-depth
// taps. Fixed: the raster's own texel follows HeightHalfExtent.
static const float kHeightTexel = 4.0;

// Max-of-4 texel sample: bilinear would poison against sentinel texels at
// object edges; MAX both ignores them and keeps the patch on the highest
// (safest) surface.
float2 PatchTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - HeightWindowCenter) / HeightHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

// Texel of the FINE window, which shares the coarse window's centre.
float2 FineTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - HeightWindowCenter) / FineHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

// The fine top, MAX of four like PatchTop; the empty sentinel where the fine
// level has nothing (outside its window, or a column no capture reached).
float FineTop(float2 worldXY)
{
	float result = -1000000.0;
	float2 d = abs(worldXY - HeightWindowCenter);
	[branch] if (FineHalfExtent > 0.0 && max(d.x, d.y) <= FineHalfExtent)
	{
		float2 dims;
		ObjectTopFine.GetDimensions(dims.x, dims.y);
		float2 tf = FineTexel(worldXY, dims);
		int2 t0 = (int2)tf;
#ifdef PSHADER
		const float4 tops = ObjectTopFine.GatherRed(ShellLinearSampler, (float2(t0) + 1.0) / dims);
		result = max(max(tops.x, tops.y), max(tops.z, tops.w));
#else
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		result = max(max(ObjectTopFine.Load(int3(t0.x, t0.y, 0)), ObjectTopFine.Load(int3(t1.x, t0.y, 0))),
			max(ObjectTopFine.Load(int3(t0.x, t1.y, 0)), ObjectTopFine.Load(int3(t1.x, t1.y, 0))));
#endif
	}
	return result;
}

// Texel of the FAR window, which has its own centre.
float2 FarTexel(float2 worldXY, float2 dims)
{
	float2 local = (worldXY - float2(FarWindowCenterX, FarWindowCenterY)) / FarHalfExtent;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

// Far road top, MAX of four like PatchTop; the empty sentinel outside the far
// window or with the level off. PatchTop routes here only past the coarse
// window, so a column the coarse raster answers never reads this.
float FarTop(float2 worldXY)
{
	float result = -1000000.0;
	float2 d = abs(worldXY - float2(FarWindowCenterX, FarWindowCenterY));
	[branch] if (FarHalfExtent > 0.0 && max(d.x, d.y) <= FarHalfExtent)
	{
		float2 dims;
		ObjectTopFar.GetDimensions(dims.x, dims.y);
		float2 tf = FarTexel(worldXY, dims);
		int2 t0 = (int2)tf;
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		result = max(max(ObjectTopFar.Load(int3(t0.x, t0.y, 0)), ObjectTopFar.Load(int3(t1.x, t0.y, 0))),
			max(ObjectTopFar.Load(int3(t0.x, t1.y, 0)), ObjectTopFar.Load(int3(t1.x, t1.y, 0))));
	}
	return result;
}

// MIN of the same four: the road interior at the far level's resolution.
float FarTopMin(float2 worldXY)
{
	float result = -1000000.0;
	float2 d = abs(worldXY - float2(FarWindowCenterX, FarWindowCenterY));
	[branch] if (FarHalfExtent > 0.0 && max(d.x, d.y) <= FarHalfExtent)
	{
		float2 dims;
		ObjectTopFar.GetDimensions(dims.x, dims.y);
		float2 tf = FarTexel(worldXY, dims);
		int2 t0 = (int2)tf;
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		result = min(min(ObjectTopFar.Load(int3(t0.x, t0.y, 0)), ObjectTopFar.Load(int3(t1.x, t0.y, 0))),
			min(ObjectTopFar.Load(int3(t0.x, t1.y, 0)), ObjectTopFar.Load(int3(t1.x, t1.y, 0))));
	}
	return result;
}

float PatchTop(float2 worldXY)
{
	// The fine level first where it has this column; it rasterizes the same
	// captures, so the two agree to within a texel and the switch needs no
	// blend. Empty there (a ghost the fine level carries none of) falls back.
	// Outside the coarse window: the far road level, or its sentinel - never
	// the clamped edge texel, which would hand the skin draws that reach the
	// full capture range an unrelated object's top.
	float result = FineTop(worldXY);
	[branch] if (result <= -50000.0)
	{
		float2 windowLocal = abs(worldXY - HeightWindowCenter);
		[branch] if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		{
			result = FarTop(worldXY);
		}
		else
		{
			float2 dims;
			ObjectTopRaw.GetDimensions(dims.x, dims.y);
			float2 t = PatchTexel(worldXY, dims);
			int2 t0 = (int2)t;
#ifdef PSHADER
			const float4 tops = ObjectTopRaw.GatherRed(ShellLinearSampler, (float2(t0) + 1.0) / dims);
			result = max(max(tops.x, tops.y), max(tops.z, tops.w));
#else
			int2 t1 = min(t0 + 1, int2(dims) - 1);
			result = max(max(ObjectTopRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopRaw.Load(int3(t1.x, t0.y, 0))),
				max(ObjectTopRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopRaw.Load(int3(t1.x, t1.y, 0))));
#endif
		}
	}
	return result;
}

// Snow DEPTH the repose field allows above the local surface, bilinear over
// the window. Outside it there is no data, so nothing is limited. Coarse
// level only: roads seed it per texel, and only road readers remain.
float ObjectConeDepth(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return 1000000.0;

	float2 dims;
	ObjectSnowCone.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = ObjectSnowCone.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectSnowCone.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectSnowCone.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectSnowCone.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

#endif

#if ((defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)) || defined(PSHADER)

// x = layer depth, y = road top. MAX-of-4 on both, matching PatchTop: a
// sentinel neighbour must not drag the column off the road.
float2 PatchSkinDepth(float2 worldXY)
{
	// Outside the window: the far road level (roads only, one class depth),
	// else sentinel - PatchTexel CLAMPS, so without this the read returns an
	// unrelated edge texel. PatchTop takes the same turn; the two must agree
	// or a comparison between them is meaningless.
	float2 result = float2(0.0, kNoRoadTop);
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	[branch] if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
	{
		float farTop = FarTop(worldXY);
		[flatten] if (farTop > -50000.0)
			result = float2(FarRoadDepth, farTop);
	}
	else
	{
		float2 dims;
		ObjectSkinDepth.GetDimensions(dims.x, dims.y);
		float2 t = PatchTexel(worldXY, dims);
		int2 t0 = (int2)t;
#ifdef PSHADER
		const float2 uv = (float2(t0) + 1.0) / dims;
		const float4 depths = ObjectSkinDepth.GatherRed(ShellLinearSampler, uv);
		const float4 roadTops = ObjectSkinDepth.GatherGreen(ShellLinearSampler, uv);
		result = float2(max(max(depths.x, depths.y), max(depths.z, depths.w)), max(max(roadTops.x, roadTops.y), max(roadTops.z, roadTops.w)));
#else
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		result = max(max(ObjectSkinDepth.Load(int3(t0.x, t0.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t0.y, 0))),
			max(ObjectSkinDepth.Load(int3(t0.x, t1.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t1.y, 0))));
#endif
	}
	return result;
}

// The top the patch stands on at this column: the road's own top where
// something taller stands over a road (the patch VS's rule for its centre),
// else the object top. Sentinel where neither is real.
float PatchSurfaceTop(float2 worldXY)
{
	float top = PatchTop(worldXY);
	float roadTop = PatchSkinDepth(worldXY).y;
	[flatten] if (roadTop > kNoRoadTop * 0.5 && (top - roadTop) >= kRoadOwnsTop)
		top = roadTop;
	return top;
}

// One-sided at a sentinel neighbour, zero with none: the edge ring keeps
// the slope it can see rather than rolling over toward the verge.
float PatchTopSlope(float p, float n, float c, float step)
{
	bool vp = p > -50000.0;
	bool vn = n > -50000.0;
	[flatten] if (vp && vn)
		return (p - n) / (2.0 * step);
	[flatten] if (vp)
		return (p - c) / step;
	[flatten] if (vn)
		return (c - n) / step;
	return 0.0;
}

// How far this column's supporting top stands below the highest of its four
// texels. The patch dissolves on this (its silhouette clip): the max-of-4
// placement extends object tops up to a texel past the real silhouette, and
// the bilinear top's drop below the max marks that overhang.
//
// Shared with RoadOwnsColumn because it is the SECOND reason the patch may
// not draw, and the skin has to know about both.
float PatchSilhouetteDrop(float2 worldXY)
{
	// Domain-warp WHERE the silhouette falls (BorderJitter, SnowFields.hlsli).
	// Without it the cut follows the 4-unit raster lattice and a road's end
	// reads as a staircase; with it the edge wanders off the lattice and reads
	// ragged.
	//
	// A quarter of the retired Border Noise's default 8: an object footprint
	// is real geometry, and a large warp would pull a roof's top into a ground
	// column.
	//
	// Applied HERE, inside the one function the patch's clip and the skin's
	// RoadOwnsColumn both call, so the two cannot disagree about where the
	// boundary is. Jittering them separately is how the holes came back twice.
	worldXY += BorderJitter(worldXY, 8.0) * 0.25;

	// The far road level has no silhouette to clip: one road top per texel
	// and nothing standing over it. Past the coarse window the per-vertex
	// kill is the edge, and the skin's step-aside reads the same level.
	float result = 0.0;
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	[branch] if (max(windowLocal.x, windowLocal.y) <= HeightHalfExtent)
	{
		float2 dims;
		ObjectTopRaw.GetDimensions(dims.x, dims.y);
		float2 t = PatchTexel(worldXY, dims);
		int2 c0 = (int2)t;
		float2 cf = t - c0;
#ifdef PSHADER
		const float4 tops = ObjectTopRaw.GatherRed(ShellLinearSampler, (float2(c0) + 1.0) / dims).wzxy;
		float top00 = tops.x;
		float top10 = tops.y;
		float top01 = tops.z;
		float top11 = tops.w;
#else
		int2 c1 = min(c0 + 1, int2(dims) - 1);
		float top00 = ObjectTopRaw.Load(int3(c0.x, c0.y, 0));
		float top10 = ObjectTopRaw.Load(int3(c1.x, c0.y, 0));
		float top01 = ObjectTopRaw.Load(int3(c0.x, c1.y, 0));
		float top11 = ObjectTopRaw.Load(int3(c1.x, c1.y, 0));
#endif
		// On a road column the silhouette is the ROAD's, not that of whatever
		// stands over it: a wall's top would clip the patch running under it.
		float2 sdims;
		ObjectSkinDepth.GetDimensions(sdims.x, sdims.y);
		float2 st = PatchTexel(worldXY, sdims);
		int2 s0 = (int2)st;
#ifdef PSHADER
		float4 roadTops = ObjectSkinDepth.GatherGreen(ShellLinearSampler, (float2(s0) + 1.0) / sdims).wzxy;
#else
		int2 s1 = min(s0 + 1, int2(sdims) - 1);
		float4 roadTops = float4(ObjectSkinDepth.Load(int3(s0.x, s0.y, 0)).y, ObjectSkinDepth.Load(int3(s1.x, s0.y, 0)).y,
			ObjectSkinDepth.Load(int3(s0.x, s1.y, 0)).y, ObjectSkinDepth.Load(int3(s1.x, s1.y, 0)).y);
#endif
		bool4 onRoad = roadTops > kNoRoadTop * 0.5;
		[flatten] if (any(onRoad))
		{
			top00 = onRoad.x ? roadTops.x : top00;
			top10 = onRoad.y ? roadTops.y : top10;
			top01 = onRoad.z ? roadTops.z : top01;
			top11 = onRoad.w ? roadTops.w : top11;
		}
		float maxTop = max(max(top00, top10), max(top01, top11));
		float4 drops = min(maxTop - float4(top00, top10, top01, top11), 200.0);
		result = lerp(lerp(drops.x, drops.y, cf.x), lerp(drops.z, drops.w, cf.x), cf.y);
	}
	return result;
}

// Does the road own this column? The single predicate the patch's carve gate
// and the skin's step-aside both run, so the skin can never discard into a
// column the patch declined.
bool RoadOwnsColumn(float2 worldXY)
{
	bool result = false;
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	[branch] if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
	{
		// Far road level: MIN of the four texels, so the skin steps aside only
		// where every texel around the pixel is road. The patch draws on the
		// MAX (its vertices die per texel), so along a far road edge the two
		// overlap rather than leave a strip of bare mesh between them.
		result = FarTopMin(worldXY) > -50000.0;
	}
	else
	{
		// The top MUST be real. A sentinel top (no object captured here) makes
		// top - roadTop hugely negative, which passes the height test for free
		// - and the skin then steps aside for a patch that cannot draw,
		// stripping the road of snow entirely.
		float top = PatchTop(worldXY);
		// A road owns every column it overlaps, whatever stands on or over it:
		// the road's snow runs on under walls, houses and rocks (Josef, 2026-09-04)
		// rather than stopping in a hard edge at their footprint.
		float roadTop = PatchSkinDepth(worldXY).y;
		// The silhouette clip is the other way the patch declines a column it
		// otherwise owns, and it fires on the road's own edge texels - where the
		// road IS the supporting top, so the ownership test above passes happily.
		// Stepping aside there left a hole straight through to the road mesh.
		// 8.0 is where the clip's smoothstep starts biting; below it the patch is
		// at full coverage. In the 8-24 band both draw and the patch wins on top,
		// which is a thin double layer rather than a hole.
		result = top > -50000.0 && roadTop > kNoRoadTop * 0.5 && PatchSilhouetteDrop(worldXY) < 8.0;
	}
	return result;
}
#endif

#if defined(PSHADER) || ((defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH))
// Land-exact layer, the ground the landscape sheet stands on (SnowShell.hlsl
// LandHeightFine; FineWindow rides ShellCB). The patch's vertex stages read it
// for the landscape meet (BuildPatchVertex).
Texture2D<float> TerrainFine : register(t52);

// Land-exact ground at a grid-local point (SnowShell.hlsl LandHeightFine); the
// missing sentinel outside the fine window or over missing data.
float SheetLandFine(float2 gridLocal)
{
	float land = -100000.0;
	float2 tf = (FineWindow.xy + gridLocal) / FineWindow.w;
	[branch] if (FineWindow.z > 0.5 && all(tf >= 0.0) && all(tf < FineWindow.z - 1.0))
	{
		int2 f0 = (int2)tf;
		float2 ff = tf - f0;
#ifdef PSHADER
		const float4 g = TerrainFine.GatherRed(ShellLinearSampler, (float2(f0) + 1.0) / FineWindow.z).wzxy;
		float g00 = g.x;
		float g10 = g.y;
		float g01 = g.z;
		float g11 = g.w;
#else
		float g00 = TerrainFine.Load(int3(f0, 0));
		float g10 = TerrainFine.Load(int3(f0 + int2(1, 0), 0));
		float g01 = TerrainFine.Load(int3(f0 + int2(0, 1), 0));
		float g11 = TerrainFine.Load(int3(f0 + int2(1, 1), 0));
#endif
		bool slash = ((f0.x + f0.y) & 1) == 0;
		float hA = ff.y <= ff.x ? g00 + (g10 - g00) * ff.x + (g11 - g10) * ff.y : g00 + (g01 - g00) * ff.y + (g11 - g01) * ff.x;
		float hB = (ff.x + ff.y) <= 1.0 ? g00 + (g10 - g00) * ff.x + (g01 - g00) * ff.y : g11 + (g10 - g11) * (1.0 - ff.y) + (g01 - g11) * (1.0 - ff.x);
		[flatten] if (min(min(g00, g10), min(g01, g11)) > -50000.0)
			land = slash ? hA : hB;
	}
	return land;
}

// The landscape sheet's drawn surface at a grid-local point, as SnowShell's
// ShellSurfaceZ builds open snow: land-exact ground, class depth, melt, carve,
// berm, undulation. The analytic top (window height + depth) stands 4-9 units
// off it. Sentinel where no sheet stands, and where its upper bound (land plus
// the most carve, lip, berm and dunes can add) stays at or below zFloor.
// a_depth: the layer's uncarved depth there. a_ownDates reads the track's age
// from the berm field; otherwise the caller's own, read at the same point,
// comes in (a_carveAge, and what it leaves of the berm, a_bermScale).
float SheetSurfaceZ(float2 gridLocal, float zFloor, out float a_depth, uniform bool a_ownDates, float a_carveAge, float a_bermScale)
{
	float z = -100000.0;
	float3 st = SampleTerrainStatics(gridLocal);
	float depth = ShellEdgeDepth(st.y - 8.0 * saturate(1.0 - st.z), ShellEdgeFade(gridLocal));
	a_depth = depth;
	[branch] if (st.x > -50000.0 && depth > 4.0)
	{
		const float fine = SheetLandFine(gridLocal);
		float land = fine > -50000.0 ? fine : st.x;
		float zMax = land + depth * (1.0 + max(RimStyle.x, 0.0) + max(BermHeightAmp, 0.0)) + abs(UndulationAmp);
		zMax += 0.01 + 1e-6 * abs(zMax);
		[branch] if (zMax > zFloor)
		{
			float2 worldXY = GridOrigin + gridLocal;
			depth = lerp(depth, min(depth, kFireMeltFloor), saturate(SampleExclusionField(worldXY).y));
			a_depth = depth;
#ifdef PATCH
			// The sheet's own bicubic, for the landscape meet.
			float deform = saturate(SampleDeformationSmooth(gridLocal));
#else
			float deform = SampleDeformation(gridLocal);
#endif
			float4 berm = BermBakeActive > 0.5 ? BermFieldBaked(gridLocal) : float4(0.0, deform, TrackAge.x, TrackAge.x);
			const float carveAge = a_ownDates ? TrackAgeOf(berm.z) : a_carveAge;
			const float bermScale = a_ownDates ? TrackBermScale(TrackAgeOf(berm.w)) : a_bermScale;
			float carved = CarveProfile(deform, berm.y, depth, worldXY, TrackCarveLook(carveAge)) +
			               BermShape(berm.x) * saturate(1.0 - deform) * depth * BermHeightAmp * BermDepthGate(depth) * bermScale;
			z = land + carved + UndulationSampled(worldXY) * saturate(carved / 8.0);
		}
	}
	return z;
}
#endif

#if (defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)

// Patch surface evaluation, shared by the legacy VS and the tessellated
// domain shader. dense = tessellated call sites, whose vertices sit
// between the raster texels and read them bilinear.
struct PatchVertex
{
	float3 WorldAbs;
	float2 GridLocal;
	float3 NormalWS;
	float SkinDepth;
	float Deform;
	float Killed;
	// Debug view only; see FinishPatchVertex.
	float RoadBit;
};

// a_trackAge: the track's age where it was read already (the hull's corners,
// interpolated), x the carve's and y what it leaves of the berm; the legacy
// grid (dense false) reads its own.
PatchVertex BuildPatchVertex(float2 worldXY, uniform bool dense, float2 a_trackAge)
{
	PatchVertex v;
	v.WorldAbs = float3(worldXY, 0.0);
	v.GridLocal = worldXY - GridOrigin;
	v.NormalWS = float3(0.0, 0.0, 1.0);
	v.SkinDepth = 0.0;
	v.Deform = 0.0;
	v.Killed = 1.0;
	v.RoadBit = 0.0;

	float top;
	float skinDepth;
	float skinEdgeMin;
	// Highest road surface in this cell, kNoRoadTop where no road drew. Tested
	// against `top` below: only a road-OWNED column carries the patch.
	float roadTop;
	[branch] if (dense)
	{
		// Tessellated vertices sample BETWEEN the 8-unit raster texels,
		// where raw max-of-4 sampling reads a higher surface than the
		// legacy grid's linear interpolation; carved floors then poke
		// through their 0.4-unit tuck and z-fight the object below, angle-
		// dependently. Bilinear over the same 8-aligned lattice points the
		// legacy grid sampled reproduces its exact floor geometry.
		float2 base = floor(worldXY / kHeightTexel) * kHeightTexel;
		float2 f = saturate((worldXY - base) / kHeightTexel);
		float t00 = PatchTop(base);
		float t10 = PatchTop(base + float2(kHeightTexel, 0.0));
		float t01 = PatchTop(base + float2(0.0, kHeightTexel));
		float t11 = PatchTop(base + float2(kHeightTexel, kHeightTexel));
		top = lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
		// A sentinel lattice corner poisons the bilinear, and a large drop
		// across the cell (roof or wall edge) would interpolate vertices
		// midway down the facade. Both fall back to the center sample so the
		// whole cell resolves like the legacy grid.
		float tMin = min(min(t00, t10), min(t01, t11));
		float tMax = max(max(t00, t10), max(t01, t11));
		[flatten] if (tMin < -50000.0 || (tMax - tMin) > 100.0)
			top = PatchTop(worldXY);
		float2 s00 = PatchSkinDepth(base);
		float2 s10 = PatchSkinDepth(base + float2(kHeightTexel, 0.0));
		float2 s01 = PatchSkinDepth(base + float2(0.0, kHeightTexel));
		float2 s11 = PatchSkinDepth(base + float2(kHeightTexel, kHeightTexel));
		skinDepth = lerp(lerp(s00.x, s10.x, f.x), lerp(s01.x, s11.x, f.x), f.y);
		// Weakest lattice corner: a footprint boundary crossing this cell.
		skinEdgeMin = min(min(s00.x, s10.x), min(s01.x, s11.x));
		// MAX, not the bilinear: a cell straddling the road edge must resolve
		// as road for every one of its vertices or the gate splits the cell.
		roadTop = max(max(s00.y, s10.y), max(s01.y, s11.y));
	}
	else
	{
		float2 skin = PatchSkinDepth(worldXY);
		top = PatchTop(worldXY);
		skinDepth = skin.x;
		skinEdgeMin = skinDepth;
		roadTop = skin.y;
	}
	float2 gridLocal = v.GridLocal;

	// The patch is the road's snow: it covers every road-owned column,
	// trampled or not (untrampled road resolves to deform 0 below, a flat
	// full-depth surface), and nothing else. A sentinel top or a dead class
	// depth never qualifies, so ownership is asked only past those loads.
	//
	// OWNERSHIP, not presence: a road's footprint reaches every column it
	// overlaps in plan view, including those whose top belongs to a rock,
	// cairn, wall or building standing on it. The road owns the column only
	// where the column's top IS the road - the RoadOwnsColumn predicate,
	// against this vertex's already-sampled top/roadTop. The depth channel
	// bleeds the same way (a road's footprint MAX-blends its class depth
	// across every column it overlaps), so an unowned column never carves.
	bool owns = false;
	[branch] if (top > -50000.0 && skinDepth >= 1.0)
	{
		owns = roadTop > kNoRoadTop * 0.5;
		// Under a wall or a house the column's top is the building; the road
		// patch keeps going on the road's own top instead (clipping through).
		[flatten] if (owns && (top - roadTop) >= kRoadOwnsTop)
			top = roadTop;
	}
	v.RoadBit = owns ? 1.0 : 0.0;

	// Single-return structure: an early return inside a [branch] trips
	// fxc's X4000 and CI enforces zero warnings.
	[branch] if (top > -50000.0 && skinDepth >= 1.0 && owns)
	{
		// Road verge: ride the repose cone down to the landscape class depth.
		// A trench crossing the road edge then keeps ONE cross-section - the
		// walls shrink smoothly to landscape scale by the boundary instead of
		// jumping between the two class depths at the silhouette (Josef's
		// crossing seam). Interior, the cone stands above the road depth and
		// nothing changes; the road never exceeds its own class, and never
		// drops below the smaller of the two. SampleTerrainStatics carries
		// the accumulation scale, so the verge stays continuous as depths
		// grow. Undisturbed verges change from a step to a bank, which is
		// what snow does at a cleared edge anyway.
		{
			float landDepth = max(SampleTerrainStatics(gridLocal).y, 0.0);
			float cone = ObjectConeDepth(worldXY);
			skinDepth = max(min(cone, skinDepth), min(landDepth, skinDepth));
		}

		// Bicubic, like the landscape shell; rounded trench walls.
		float deform = saturate(SampleDeformationSmooth(gridLocal));

		// Carve through the SHARED profile, so object trenches and landscape
		// trenches are one shape: the depth remap, the rim teeth and the lip all
		// arrive, and the floor rides the Trench Floor fraction exactly as the ground's
		// does instead of a bespoke constant. This shader's own self-shadow march
		// already assumed this shape - it calls CarveProfile - while the geometry
		// was cutting a raw linear ramp, so shape and shadow disagreed.
		// Geometric berm: spoil piled along the trench rim, mirroring
		// ShellSurfaceZ. Deferred until now for two stated reasons - irregular
		// skin topology with no vertices to carry a ridge, and a berm crossing
		// the patch/skin height seam - and S0-S2 removed both: roads have no skin
		// left and the patch stands on a uniform lattice. This is the shape the
		// deferral itself recommended, patch geometry with the skin still
		// shading-only.
		//
		// Scaled by the per-texel skinDepth, NOT the draw's class constant: the
		// patch draw carries the shallow object depth in RoundedDepth, and
		// BermDepthGate of a shallow depth is exactly zero - which is why the
		// pixel shader's shading berm has been silently inert on the patch.
		// The carve's short blur rides the same read (Snow Climate's slough),
		// and the track's dates (its age).
		float4 bermPair = float4(0.0, deform, TrackAge.x, TrackAge.x);
		[branch] if (BermHeightAmp > 0.005 || CompactLook.w > 0.0 || (!dense && TrackAge.y > 0.0))
			bermPair = BermFieldPair(gridLocal);
		const float bermD = BermHeightAmp > 0.005 ? bermPair.x : 0.0;
		// Two scalars carry the age on through the vertex: the carve's, and what
		// it leaves of the berm.
		const float patchAge = dense ? a_trackAge.x : TrackAgeOf(bermPair.z);
		const float patchBermScale = dense ? a_trackAge.y : TrackBermScale(TrackAgeOf(bermPair.w));
		float depth = CarveProfile(deform, bermPair.y, skinDepth, worldXY, TrackCarveLook(patchAge)) +
		              BermShape(bermD) * saturate(1.0 - deform) * skinDepth * BermHeightAmp * BermDepthGate(skinDepth) * patchBermScale;

		// Precision pad, NOT a floor. The patch stands on real geometry, so even
		// a fully worn floor has to clear the object under it or the two z-fight,
		// and the margin grows with distance. Tapered where the raster data thins
		// (the footprint boundary): held at full strength there, the raised floor
		// ends in an 8-unit staircase rim along the object's edge. Wearing
		// through to the object stays the Trench Floor See-Through slider's job,
		// which dissolves coverage rather than moving geometry.
#ifdef SNOW_SHADOW_CAST
		// Caster pass renders in absolute world with ShellCameraPosAdjust
		// zeroed; measured against it, every vertex sits ~80k units away and
		// the pad balloons to full depth - an uncarved slab shadowing the
		// whole trench. The patch centre snaps to the camera; use it.
		float camDist = length(worldXY - WorldRow0.xy);
#else
		float camDist = length(float3(worldXY, top) - ShellCameraPosAdjust.xyz);
#endif
		float pad = min(skinDepth, 0.8 + camDist * 0.004) * smoothstep(0.25, 2.0, skinEdgeMin);
		depth = max(depth, pad);

		// Undulation, on the landscape shell's own terms (SnowShell.hlsl's
		// ShellSurfaceZ): same shared field, same depth scaling. Without it
		// the patch is geometrically DEAD FLAT while the ground beside it
		// carries two octaves of dunes, and the two then disagree about light
		// in a way that only shows with the sun BEHIND the camera - a rough
		// surface shows its sun-facing faces and reads brighter, a flat one
		// cannot. Looking into the sun the dunes turn their shadowed sides
		// and the difference closes, which is exactly what Josef reported.
		depth += UndulationSampled(worldXY) * saturate(depth / 8.0);

		v.WorldAbs = float3(worldXY, top + depth - 0.4);

		// Carved-surface shading normal: finite differences of the SAME
		// function the depth uses, exactly as the landscape PS differences
		// CarveProfile. S1 moved the depth onto CarveProfile but left this
		// on the raw deformation gradient x 0.6, and the two curves
		// disagree hardest at mid-wall: the remap's slope reaches 1.875
		// where the old factor was 0.6, so at depth 30 the wall was lit as
		// if ~3x flatter than it stands - the bright band along road
		// trench walls. At 64 the raw term saturated the tilt regardless,
		// which is why the band vanished there; at 10 the profile is
		// genuinely soft and the two curves agree. The march debug view
		// cleared the shadow path first: engagement green, wall-base
		// shadow landing - the band was never the march's.
		// The caster is depth-only: no normal.
#ifndef SNOW_SHADOW_CAST
		const float gStep = 4.0;
		float dXP = saturate(SampleDeformationSmooth(gridLocal + float2(gStep, 0.0)));
		float dXN = saturate(SampleDeformationSmooth(gridLocal - float2(gStep, 0.0)));
		float dYP = saturate(SampleDeformationSmooth(gridLocal + float2(0.0, gStep)));
		float dYN = saturate(SampleDeformationSmooth(gridLocal - float2(0.0, gStep)));
		float4 blur = float4(dXP, dXN, dYP, dYN);
		const float3 patchLook = TrackCarveLook(patchAge);
		[branch] if (CompactLook.w > 0.0 || patchLook.x > 0.0)
		{
			blur = float4(BermFieldPair(gridLocal + float2(gStep, 0.0)).y, BermFieldPair(gridLocal - float2(gStep, 0.0)).y,
				BermFieldPair(gridLocal + float2(0.0, gStep)).y, BermFieldPair(gridLocal - float2(0.0, gStep)).y);
		}
		// Meeting the landscape sheet (Object Meeting Blend): where the two
		// surfaces cross, the walls shade halfway to the landscape layer's
		// depth; the sheet comes the other half and takes the road's macro
		// slope (SnowShell.hlsl). Weighed by the height gap to the sheet's
		// drawn surface: the sheet runs on over the road. Shading only.
		float profDepth = skinDepth;
		[branch] if (ObjectMeetBand > 0.0 && camDist < 2048.0)
		{
			float landDepth;
			const float sheetZ = SheetSurfaceZ(gridLocal, v.WorldAbs.z - ObjectMeetBand * 0.5, landDepth, false, patchAge, patchBermScale);
			const float meet = (1.0 - smoothstep(0.0, ObjectMeetBand * 0.5, abs(v.WorldAbs.z - sheetZ))) * (1.0 - smoothstep(1024.0, 2048.0, camDist));
			[flatten] if (sheetZ > -50000.0)
				profDepth = lerp(skinDepth, landDepth, 0.5 * meet);
		}
		float2 profGrad = float2(
							  CarveProfile(dXP, blur.x, profDepth, worldXY + float2(gStep, 0.0), patchLook) - CarveProfile(dXN, blur.y, profDepth, worldXY - float2(gStep, 0.0), patchLook),
							  CarveProfile(dYP, blur.z, profDepth, worldXY + float2(0.0, gStep), patchLook) - CarveProfile(dYN, blur.w, profDepth, worldXY - float2(0.0, gStep), patchLook)) /
		                  (2.0 * gStep);
		// Undulation gradient, the same field the depth above displaced by -
		// mirrors SnowShell.hlsl's PS block. The geometry alone is not enough:
		// at patch vertex spacing the dunes are far coarser than the shading
		// needs, and it is the NORMAL that carries the sun-direction response.
		float2 undGrad = float2(0.0, 0.0);
		float undScale = saturate(depth / 8.0);
		[branch] if (undScale > 0.001)
		{
			undGrad = UndulationGradSampled(worldXY) * undScale;
		}
		// Berm gradient, the same field the depth above piled by. The pixel
		// shader's berm is skipped for the patch now that the geometry owns it.
		// Differenced with the (1 - deformation) mask, as the landscape PS does:
		// the berm drops with the wall. Meet depth like the profile.
		float2 bermGrad = float2(0.0, 0.0);
		[branch] if (BermHeightAmp > 0.005 && bermD > 0.003)
		{
			const float bStep = BermGradStep();
			float mXP = dXP, mXN = dXN, mYP = dYP, mYN = dYN;
			[branch] if (bStep < gStep - 0.01)
			{
				mXP = saturate(SampleDeformationSmooth(gridLocal + float2(bStep, 0.0)));
				mXN = saturate(SampleDeformationSmooth(gridLocal - float2(bStep, 0.0)));
				mYP = saturate(SampleDeformationSmooth(gridLocal + float2(0.0, bStep)));
				mYN = saturate(SampleDeformationSmooth(gridLocal - float2(0.0, bStep)));
			}
			bermGrad = float2(
				BermShape(BermField(gridLocal + float2(bStep, 0.0))) * (1.0 - mXP) - BermShape(BermField(gridLocal - float2(bStep, 0.0))) * (1.0 - mXN),
				BermShape(BermField(gridLocal + float2(0.0, bStep))) * (1.0 - mYP) - BermShape(BermField(gridLocal - float2(0.0, bStep))) * (1.0 - mYN)) / (2.0 * bStep) *
			          profDepth * BermHeightAmp * BermDepthGate(profDepth) * patchBermScale;
		}
		// The top's own slope. Without it a road climbing a hill shaded as
		// flat while the landscape shell beside it carried the hill: ~16
		// degrees apart at the verge, and the patch read grey (RenderDoc
		// edge-pair diff, 2026-09-04). 16-unit step like the terrain normal's
		// smoothing; clamped to 45 degrees so the verge drop cannot roll
		// the edge ring over.
		float2 topGrad = float2(0.0, 0.0);
		{
			const float tStep = 16.0;
			float tXP = PatchSurfaceTop(worldXY + float2(tStep, 0.0));
			float tXN = PatchSurfaceTop(worldXY - float2(tStep, 0.0));
			float tYP = PatchSurfaceTop(worldXY + float2(0.0, tStep));
			float tYN = PatchSurfaceTop(worldXY - float2(0.0, tStep));
			topGrad = float2(PatchTopSlope(tXP, tXN, top, tStep), PatchTopSlope(tYP, tYN, top, tStep));
			topGrad /= max(1.0, length(topGrad));
		}
		// Surface z = top + profile, so normal.xy = -d(top) - d(profile); the
		// other fields RAISE the surface and subtract for the same reason.
		v.NormalWS = normalize(float3(-topGrad - profGrad - undGrad - bermGrad, 1.0));
#endif
		v.SkinDepth = skinDepth;
		v.Deform = deform;
		v.Killed = 0.0;
#ifdef SNOW_SHADOW_CAST
		// The landscape caster's fade-out band (40-70 m), measured from the
		// patch centre. No depth kill: a trampled road floor would take the
		// trench walls' triangles, and their shadow, with it.
		[flatten] if (1.0 - smoothstep(2800.0, 4900.0, length(worldXY - WorldRow0.xy)) < 0.35)
			v.Killed = 1.0;
#endif
	}

	return v;
}

// Packs a finished patch vertex into the PS interpolants.
VS_OUTPUT FinishPatchVertex(PatchVertex v)
{
	VS_OUTPUT vsout;
	vsout.CurrentClip = float4(0.0, 0.0, 0.0, 1.0);
	vsout.PreviousClip = float4(0.0, 0.0, 0.0, 1.0);
	vsout.WorldPos = float3(0.0, 0.0, 0.0);
	vsout.NormalWS = v.NormalWS;
	vsout.GridLocal = v.GridLocal;
	// Debug view: smuggle the decision data through the PS interpolants the
	// patch does not otherwise use for shading. Green carries skin depth in
	// its lower half and ROAD OWNERSHIP in its upper: >= 0.5 means the road
	// owns this column, so green on top of a rock is the bleed regression
	// and green confined to the road surface is correct.
	vsout.Coverage = StaticsDebugView != 0.0 ? v.Deform : 1.0;
	vsout.Flat = StaticsDebugView != 0.0 ?
	                 saturate(v.SkinDepth / 8.0) * 0.49 + (v.RoadBit > 0.5 ? 0.5 : 0.0) :
	                 0.0;
	// Modes 8-10: layer 1, full depth.
	[flatten] if (StaticsDebugView > 7.5)
	{
		vsout.Coverage = 0.5 / 8.0;
		vsout.Flat = 1.0;
	}
	// The patch is exempt from the lift gates; its walls are real geometry.
	vsout.Lift = 1e6;
	vsout.LiftTarget = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
	vsout.ProjFactor = 1.0;
	[branch] if (v.Killed > 0.5)
	{
		// NaN position: the rasterizer culls every primitive touching it,
		// the same kill the legacy grid used.
		float nan = asfloat(0x7fc00000);
		vsout.Position = float4(nan, nan, nan, nan);
	}
	else
	{
		float3 rel = v.WorldAbs - ShellCameraPosAdjust.xyz;
		vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
#ifndef SNOW_SHADOW_CAST
		float3 prevRel = v.WorldAbs - ShellCameraPreviousPosAdjust.xyz;
		vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
		vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
		vsout.WorldPos = rel;
#endif
	}
	return vsout;
}
#endif

#if ((defined(VSHADER) && defined(SNOW_TESS)) || defined(HULLSHADER)) && defined(PATCH)
// The view frustum's footprint on the world XY plane: its five planes with z
// eliminated (Fourier-Motzkin), filled per frame on the CPU from CameraViewProj.
// A patch's vertices keep its corners' XY hull, so a patch whose XY box is
// outside one line has no point in the frustum at any height.
cbuffer PatchCullCB : register(b11)
{
	float4 PatchCullLines[6];  // xy normal, z offset: inside where dot(xy, p) + z >= 0
	uint PatchCullLineCount;
	// A band of the tessellated draw: one instance per lattice row from
	// PatchRowStart, columns from PatchColStart (Statics.cpp PatchLatticeBands).
	uint PatchRowStart;
	uint PatchColStart;
	uint patchCullPad;
}
#endif

#if defined(VSHADER) && defined(PATCH) && !defined(SNOW_TESS)
// Trench patch (PATCH define): the landscape shell's recipe applied to objects,
// on the landscape shell's own warped lattice - an 8-unit core stepping out to
// the object raster's full +-4096, draped over the top-down height raster and
// carved per vertex by the deformation map. Real geometry: real silhouettes,
// floors that hold at every camera angle, no parallax. The skin steps aside
// wherever the patch owns the column (see the PS).
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
#	ifdef SNOW_PATCH_INDEXED
	// Indexed draw over the (kPatchGridDim + 1)^2 lattice: vertexID is the lattice index.
	const uint latticeStride = kPatchGridDim + 1;
	float2 gridXY = float2(vertexID % latticeStride, vertexID / latticeStride);
#	else
	static const float2 kCorners[6] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 6;
	float2 gridXY = float2(quadIndex % kPatchGridDim, quadIndex / kPatchGridDim) + kCorners[vertexID % 6];
#	endif
	// WorldRow0.xy carries the snapped patch CENTRE (see the CPU fill).
	float2 worldXY = WorldRow0.xy + PatchWarpXY(gridXY - kPatchGridDim * 0.5);
	return FinishPatchVertex(BuildPatchVertex(worldXY, false, float2(0.0, 1.0)));
}
#elif defined(VSHADER) && defined(PATCH)
// Tessellated patch control points: placement only.
struct TessControlPointPatch
{
	float2 WorldXY : TEXCOORD0;
};

TessControlPointPatch main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	static const float2 kPatchCorners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = (PatchRowStart + instanceID) * kPatchGridDim + PatchColStart + vertexID / 4;
	float2 gridXY = float2(quadIndex % kPatchGridDim, quadIndex / kPatchGridDim) + kPatchCorners[vertexID % 4];
	TessControlPointPatch cp;
	cp.WorldXY = WorldRow0.xy + PatchWarpXY(gridXY - kPatchGridDim * 0.5);
	return cp;
}
#endif

#if (defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)
struct TessControlPointPatch
{
	float2 WorldXY : TEXCOORD0;
};

// The hull's corners carry the track's age (x the carve's, y what it leaves
// of the berm), read once a corner and interpolated by the domain like the
// corner's place: neighbouring patches share corners, so they meet on the
// same values. The age moves over tens of units, a near patch spans 16.
struct TessControlPointPatchAged
{
	float2 WorldXY : TEXCOORD0;
	float2 CornerAge : TEXCOORD1;
};

struct TessFactorsPatch
{
	float Edge[4] : SV_TessFactor;
	float Inside[2] : SV_InsideTessFactor;
};
#endif

#if defined(PSHADER) && defined(SNOW_COAT_PARALLAX)
// Snow Follows Parallax: the object's own height map (TruePBR displacement or
// complex-material env mask), bound per draw.
Texture2D<float4> ObjectParallaxMap : register(t59);
// While set, Extended Materials' march reads the object's map
// (EMParallaxCustomHeight) instead of the snow's.
static bool gObjectParallaxMarch = false;
float ObjectParallaxHeight(float2 uv, float mip)
{
	const float4 h = ObjectParallaxMap.SampleLevel(SnowSampler, uv, mip);
	return ObjParallax.y > 1.5 ? h.w : h.x;
}
#endif

#include "SnowDeformation/SnowParallax.hlsli"

#if defined(PSHADER) && defined(SNOW_COAT_PARALLAX)
#	include "SnowDeformation/SnowParallaxFollow.hlsli"
// The world offset from this coat pixel's mesh point to the one whose texel
// the object's parallax drew here: Extended Materials' own march on the
// object's map, with Lighting's inputs, so the coat's snow moves with it.
float3 ObjectParallaxShift(VS_OUTPUT input)
{
	float3 shift = 0.0;
	[branch] if (ObjParallax.x > 0.0)
	{
		const float2 uv = input.ObjUV;
		const float2 uvDx = ddx_fine(uv);
		const float2 uvDy = ddy_fine(uv);
		const float3 posDx = ddx_fine(input.WorldPos);
		const float3 posDy = ddy_fine(input.WorldPos);
		const float3 viewDirection = -normalize(input.WorldPos);
		const float3x3 tbnTr = float3x3(normalize(input.ObjT), normalize(input.ObjB), normalize(input.ObjN));
		DisplacementParams params;
		params.DisplacementScale = 1.0;
		params.DisplacementOffset = 0.0;
		params.HeightScale = ObjParallax.x;
		// Flatten Parallax Under Snow: Lighting's share (SnowLighting
		// ParallaxUnderSnow), off the noise-free weight the coat rebuilds.
		[flatten] if (SharedData::snowDeformationSettings.ParallaxFlatten > 0.5 && SharedData::snowDeformationSettings.ProjSnowEnable > 0.5 && ProjThreshold > -0.9)
			params.HeightScale *= 1.0 - smoothstep(-0.1, ProjNoiseScale + 0.1, tbnTr[2].z * input.ProjFactor - max(ProjThreshold, 0.0));
		params.FlattenAmount = 0.0;
		[branch] if (SharedData::extendedMaterialSettings.EnableParallaxWarpingFix)
			params.FlattenAmount = ExtendedMaterials::GetObjectFlattenAmount(tbnTr[2], input.WorldPos, viewDirection);
		float2 dims;
		ObjectParallaxMap.GetDimensions(dims.x, dims.y);
		const float mip = max(ExtendedMaterials::GetMipLevelFromDims(uv, dims * ObjParallax.z) - ObjParallax.w, 0.0);
		bool march = true;
		[branch] if (ObjParallax.y > 1.5)
			march = ExtendedMaterials::IsComplexMaterial(ObjectParallaxMap.SampleLevel(SnowSampler, uv, 15)) &&
			        ExtendedMaterials::HasComplexHeight(ObjectParallaxMap.Sample(SnowSampler, uv).w);
		[branch] if (march)
		{
			gObjectParallaxMarch = true;
			float pixelOffset;
			const float2 parallaxUV = ExtendedMaterials::GetParallaxCoords(input.CurrentClip.w, uv, mip, viewDirection, tbnTr, 0.0,
				ObjectParallaxMap, SnowSampler, 0, params, false, (StochasticOffsets)0, pixelOffset);
			gObjectParallaxMarch = false;
			shift = SnowParallaxFollow::UVShiftToWorld(parallaxUV - uv, uvDx, uvDy, posDx, posDy);
		}
	}
	return shift;
}
#endif

#if defined(HULLSHADER) && defined(PATCH)
bool PatchOutsideFootprint(float2 a, float2 b, float2 c, float2 d)
{
	float2 lo = min(min(a, b), min(c, d)) - 16.0;
	float2 hi = max(max(a, b), max(c, d)) + 16.0;
	bool outside = false;
	[unroll] for (uint i = 0; i < 6; i++)
	{
		float4 l = PatchCullLines[i];
		float2 pv = float2(l.x > 0.0 ? hi.x : lo.x, l.y > 0.0 ? hi.y : lo.y);
		if (i < PatchCullLineCount && dot(l.xy, pv) + l.z < 0.0)
			outside = true;
	}
	return outside;
}

// Road-edge subdivision's share of the tessellation target: vertices live or
// die per raster texel, so a coarse far segment steps the road's silhouette.
static const float kPatchEdgeTargetScale = 0.25;

// Same trench-aware quad factors as the landscape shell: the patch IS the
// trench layer, so deformed edges get extended detail reach.
float PatchEdgeTessFactor(float2 worldA, float2 worldB, float zA, float zB, bool screenCap)
{
	float2 mid = 0.5 * (worldA + worldB);
	float dist = length(mid - ShellCameraPosAdjust.xy);
	float deform = max(max(SampleDeformation(worldA - GridOrigin), SampleDeformation(worldB - GridOrigin)), SampleDeformation(mid - GridOrigin));
	// Undeformed edges subdivide only to carry the relief; at relief 0 their
	// base reach goes to zero and the factor clamps to 1. Kept in step with
	// EdgeTessFactor in SnowShell.hlsl. Still edge-derived only, so crack-free.
	float reliefBase = SnowReliefDepth > 0.01 ? 1.0 : 0.0;
	float reach = 1600.0 * lerp(reliefBase, 3.0, smoothstep(0.02, 0.25, deform));
	float factor = clamp(reach / max(dist, 32.0), 1.0, 8.0);
	// Outer bands: quads are 32-256 units and a vertex lives or dies per
	// raster texel, so a coarse quad straddling a road edge loses its whole
	// triangle fan. Subdivide live quads back to ~16-unit vertices; the
	// constant function has already culled the quads with no road under them.
	float quadFactor = clamp(length(worldA - worldB) / 16.0, 1.0, 16.0);
	// Screen-size caps, the landscape hull's rule. Symmetric in the
	// endpoints, so crack-free.
	[branch] if (screenCap && max(factor, quadFactor) > 1.0)
	{
		float4 clipA = mul(CameraViewProj, float4(float3(worldA, zA) - ShellCameraPosAdjust.xyz, 1.0));
		float4 clipB = mul(CameraViewProj, float4(float3(worldB, zB) - ShellCameraPosAdjust.xyz, 1.0));
		[flatten] if (min(clipA.w, clipB.w) > 16.0)
		{
			float edgePx = length((clipA.xy / clipA.w - clipB.xy / clipB.w) * TessScreen.yz);
			factor = min(factor, max(edgePx / TessScreen.x, 1.0));
			quadFactor = min(quadFactor, max(edgePx / (TessScreen.x * kPatchEdgeTargetScale), 1.0));
		}
	}
	return max(factor, quadFactor);
}

TessFactorsPatch PatchConstants(InputPatch<TessControlPointPatch, 4> patch)
{
	TessFactorsPatch f;
	// Cull patches with no live corner (off the footprint or untrampled);
	// the cheap kill terms only, the domain shader kills per vertex.
	bool anyLive = false;
	float cornerZ[4] = { -1000000.0, -1000000.0, -1000000.0, -1000000.0 };
	[branch] if (!PatchOutsideFootprint(patch[0].WorldXY, patch[1].WorldXY, patch[2].WorldXY, patch[3].WorldXY))
	{
		[unroll] for (uint i = 0; i < 4; i++)
		{
			float2 w = patch[i].WorldXY;
			cornerZ[i] = PatchTop(w);
			if (cornerZ[i] > -50000.0 && PatchSkinDepth(w).x >= 1.0)
				anyLive = true;
		}
		// A road narrower than a coarse quad's diagonal can cross it between all
		// four corners; the centre catches that (any road wider than ~180 units
		// hits a corner or the centre of a 256-unit quad).
		float2 centre = 0.25 * (patch[0].WorldXY + patch[1].WorldXY + patch[2].WorldXY + patch[3].WorldXY);
		if (PatchTop(centre) > -50000.0 && PatchSkinDepth(centre).x >= 1.0)
			anyLive = true;
	}
	if (!anyLive) {
		f.Edge[0] = f.Edge[1] = f.Edge[2] = f.Edge[3] = 0.0;
		f.Inside[0] = f.Inside[1] = 0.0;
		return f;
	}
	// The caps project each corner at the top it stands on, or the terrain
	// where it stands on nothing.
	const bool screenCap = TessScreen.x > 0.0;
	[branch] if (screenCap)
	{
		[unroll] for (uint c = 0; c < 4; c++)
		{
			[branch] if (cornerZ[c] <= -50000.0)
				cornerZ[c] = SampleTerrainStatics(patch[c].WorldXY - GridOrigin).x;
		}
	}
	f.Edge[0] = PatchEdgeTessFactor(patch[0].WorldXY, patch[3].WorldXY, cornerZ[0], cornerZ[3], screenCap);
	f.Edge[1] = PatchEdgeTessFactor(patch[0].WorldXY, patch[1].WorldXY, cornerZ[0], cornerZ[1], screenCap);
	f.Edge[2] = PatchEdgeTessFactor(patch[1].WorldXY, patch[2].WorldXY, cornerZ[1], cornerZ[2], screenCap);
	f.Edge[3] = PatchEdgeTessFactor(patch[3].WorldXY, patch[2].WorldXY, cornerZ[3], cornerZ[2], screenCap);
	// u runs along edges 1 and 3, v along 0 and 2: the short side of a long
	// band quad keeps its own factor instead of the long side's.
	f.Inside[0] = max(f.Edge[1], f.Edge[3]);
	f.Inside[1] = max(f.Edge[0], f.Edge[2]);
	return f;
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("PatchConstants")]
[maxtessfactor(16.0)]  // PatchEdgeTessFactor clamps to 16
TessControlPointPatchAged main(InputPatch<TessControlPointPatch, 4> patch, uint i : SV_OutputControlPointID)
{
	TessControlPointPatchAged cp;
	cp.WorldXY = patch[i].WorldXY;
	cp.CornerAge = float2(0.0, 1.0);
	[branch] if (TrackAge.y > 0.0)
	{
		const float2 dates = BermFieldPair(patch[i].WorldXY - GridOrigin).zw;
		cp.CornerAge = float2(TrackAgeOf(dates.x), TrackBermScale(TrackAgeOf(dates.y)));
	}
	return cp;
}
#endif

#if defined(DOMAINSHADER) && defined(PATCH)
[domain("quad")]
VS_OUTPUT main(TessFactorsPatch factors, float2 domainUV : SV_DomainLocation, const OutputPatch<TessControlPointPatchAged, 4> patch)
{
	float2 worldXY = lerp(
		lerp(patch[0].WorldXY, patch[1].WorldXY, domainUV.x),
		lerp(patch[3].WorldXY, patch[2].WorldXY, domainUV.x), domainUV.y);
	const float2 trackAge = lerp(
		lerp(patch[0].CornerAge, patch[1].CornerAge, domainUV.x),
		lerp(patch[3].CornerAge, patch[2].CornerAge, domainUV.x), domainUV.y);
	PatchVertex v = BuildPatchVertex(worldXY, true, trackAge);

	// Relief on the uncarved rim, so the patch's snow lip matches the
	// tessellated skin it tucks under; carved floors stay smooth.
	[branch] if (v.Killed < 0.5 && HasSnowHeight > 0.5 && SnowReliefDepth > 0.01)
	{
		float camDist = length(v.WorldAbs - ShellCameraPosAdjust.xyz);
		float reliefFade = 1.0 - smoothstep(600.0, 2200.0, camDist);
		[branch] if (reliefFade > 0.001)
		{
			float2 snowUV = (SnowUVOffset + v.GridLocal) / kSnowUVTile;
			float mip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);
			// Through the PS's anti-tiling taps, not a single un-offset fetch
			// (see SnowShell.hlsl's domain shader for why).
			float h = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, v.WorldAbs.xy), 0.0.xx, mip);
			v.WorldAbs.z += (h - 0.5) * SnowReliefDepth * reliefFade * saturate(v.SkinDepth / 6.0) * (1.0 - v.Deform);
		}
	}

	return FinishPatchVertex(v);
}
#endif

#if (defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && !defined(PATCH)
// Distance at which this class's geometric layer has fully collapsed. The
// hull shader retires tessellation over the same range: once the layer has no
// height there is no rim shape left to resolve.
#endif

#if (defined(VSHADER) || defined(DOMAINSHADER)) && !defined(PATCH)
// Lift evaluation: class depth, up-facing mask, edge taper and distance
// collapse. Called per GENERATED vertex by the domain shader, so the mask and
// the taper resolve at tessellated density; evaluating them at source vertices
// and interpolating smears the transition diagonally across whole faces, which
// on low-poly meshes is the width of the object.
struct SkinLift
{
	float3 WorldAbs;
	float Depth;
	// Depth before the distance collapse. Coverage keys off this so the
	// material keeps drawing to the capture range after the geometry has
	// flattened; gating on the collapsed depth deletes distant shells.
	float CoverDepth;
	// Distance to the nearest rim, normalized: 0 at the rim, 1 in the
	// interior. Drives the cornice roll's shading.
	float RimT;
	// Debug view only: the two masks, unmultiplied.
	float Support;
	float UpFacing;
	// The authored-placement multiplier applied to UpFacing (1 = no data or
	// disabled); the PS's density-mode coverage gate reads it interpolated.
	float ProjFactor;
	// Authored-relief mode: the raw authored vertex alpha, exported so the
	// PS can rebuild vanilla's weight per pixel from the G-buffer normal.
	float ProjLinear;
	// Debug views 8-10: 1 on an S4 draw, else 0.
	float DebugLayer;
	// Post-shelter depth target: the class depth with only the two shelter
	// clamps applied. What this column would carry if it were interior and
	// up-facing; the PS rim band scales to it.
	float Target;
	// S4 dome shading normal: the dome's shape lives in the CONE field,
	// not the mesh normals, so without this the shell shaded flat - lee
	// flanks as bright as sun-facing ones (Josef's report). Analytic
	// surface normal from the cone gradient through the fillet's slope.
	float3 ShadeNormal;
};

SkinLift ApplySkinLift(float3 worldBase, float3 nrmWS, float3 smoothWS, float isFlat, float vertexAlpha)
{
	// The layer grows straight up for every class. Displacing along the
	// normal expands a mesh in all directions at once, so a rock gains girth
	// with its cover and the silhouette bloats; a vertical lift leaves the
	// footprint alone. It also thins the layer measured perpendicular to a
	// slope by cos(slope), which is how snow actually settles.
	float3 liftWS = float3(0.0, 0.0, 1.0);
	// Roads keep the pre-rework direction (see LegacySkin); flat-class roads
	// already lifted vertically, so only rounded road meshes differ here.
	[flatten] if (LegacySkin > 0.5 && isFlat < 0.5)
		liftWS = smoothWS;
	// Minimum coat: at depth 0 the skin sits coincident with its own source
	// mesh and z-fights itself invisible; the snow cover stays visible as a
	// thin coat no matter the class sliders.
	float depthBase = max(lerp(RoundedDepth, ObjectsDepth, isFlat), kMinSkinLift);
	// depthBase carried through the SHELTER clamps only - no facing gate, no
	// taper, no distance collapse. Exported as o.Target for the PS rim band.
	float depthTarget = depthBase;


	// Snow accumulates on up-facing surfaces (steep shingles and walls stay
	// bare, matching the vanilla projection's extent). flat meshes gate hard
	// on the raw normal so plank sides stay clean; rounded meshes ramp over
	// almost the whole up-facing range of the SMOOTHED normal.
	// The layer stays geometrically uncarved: trench relief is traced per
	// pixel in the PS instead.
	float weldNz = nrmWS.z;
	float upFacing = isFlat > 0.5 ? smoothstep(0.4, 0.7, weldNz) : smoothstep(0.05, 0.85, smoothWS.z);
	// The NIF's authored projected-snow term as a SUPPRESSOR (S2/S2b,
	// SKIN-PLACEMENT-PLAN): it multiplies, never adds, so agreement zones
	// keep their snow and vanilla-only zones (leaning walls, whose raw
	// weight lacks the noise term's break-up) gain none. Surfaces authored
	// bare - posts, railings, rims - shed the skin; the lift going to zero
	// also zeroes the PS's liftCoverage material gate, so no separate
	// coverage plumbing. Sentinel threshold = no data, unchanged. Density
	// mode grades depth by the authored weight itself - thick where the
	// paint is solid, a dusting where it fades - and SUPERSEDES the sharp
	// gate: multiplying both would double-punish sparse paint.
	// The cut-in kills sparse paint OUTRIGHT instead of rendering it thin,
	// and the factor is exported for the PS's density-mode coverage gate.
	float projFactor = 1.0;
	float projLinear = 1.0;
	// Debug views 8-10: the draw's layer.
	float debugLayer = 0.0;
	// S4 dome shading normal; non-S4 paths keep the raw normal.
	float3 shadeNormal = nrmWS;
	// Flat PD shell (S3 round 10, Josef's split): this draw's cover is a
	// CONSTANT coat inflated along the sealed smooth normal - set at the
	// END of this function; the depth pipeline below belongs to the 3D
	// layer and the coat does not participate in it. The PS owns placement
	// entirely: vanilla's weight rebuilt per pixel (G-buffer normal,
	// authored alpha, noise term always in full - the footprint IS the
	// purple). ProjLinear rides
	// TEXCOORD8 carrying the AUTHORED VERTEX ALPHA - the PS needs the raw
	// authored term, not a pre-mixed weight. Vertical-lift history and why
	// it could never cover every angle: SKIN-PLACEMENT-PLAN rounds 4-10.
	[branch] if (ProjPixelEnable > 1.5)
	{
		projLinear = vertexAlpha;
		upFacing = 0.0;
	}
	else [branch] if (ProjThreshold > -0.9)
	{
		float projWeight = nrmWS.z * vertexAlpha - max(ProjThreshold, 0.0);
		projFactor = saturate(projWeight) * smoothstep(0.06, 0.16, projWeight);
		upFacing *= projFactor;
	}
	// The whole drift mesh is snow: coated at every angle.
	[flatten] if (FullCoat > 0.5)
		upFacing = 1.0;
	float depth = depthBase * upFacing;

	// Geometry LOD: collapse the layer BEFORE the material dissolve begins, so
	// the hand-off to the object's own projected snow has no silhouette to pop.
	// Range scales with class depth against the sliders' 25-unit maximum. Roads
	// are exempt - their height must stay in step with the landscape shell at
	// the verge, and that does not collapse.
	//
	// Edge taper: the cone field holds the highest surface the angle of repose
	// permits per column, so the layer thins toward every rim. Isotropic, unlike
	// a ring walk, which facets curved rims into spikes.
	float support = 1.0;
	float rimT = 1.0;
	// The S4 branch below resets depth; rimT and support reach only non-S4
	// shading and the debug views; the shelter clamp is a no-op at or under kShelterDust.
	[branch] if (HasObjectTop > 0.5 && LegacySkin < 0.5 && depth > 0.001 &&
				 (ProjPixelEnable < 1.5 || StaticsDebugView != 0.0 || depthTarget > kShelterDust))
	{
		// The field is seeded with the deepest class in play, so normalizing
		// by that recovers distance-to-rim: 0 at the rim, 1 in the interior.
		float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
		rimT = saturate(ObjectConeDepth(worldBase.xy) / coneSeed);
		// Cornice for EVERY class (P2, edge-research study): full depth
		// carried to within kCorniceRoll units of the rim, quarter-circle
		// down. Flat plates always worked this way; rocks and cliffs used to
		// slump LINEARLY across the whole cone ramp - tens of units on a big
		// boulder - which read as shrink-wrap, not snow. The roll is a FIXED
		// world width: the ramp scales with depth, so a proportional roll
		// exceeds thin features at deep settings. Where the ramp is narrower
		// than the roll, rollFrac saturates and the fillet spans the whole
		// ramp - it degrades to the dome profile, never to a spike.
		float rollFrac = saturate(kCorniceRoll / coneSeed);
		float u = saturate(rimT / max(rollFrac, 1e-3));
		float toRim = 1.0 - u;
		rimT = u;
		float allowed = depthBase * sqrt(saturate(1.0 - toRim * toRim));
		depth = min(depth, allowed);
		support = saturate(allowed / max(depthBase, 0.01));

		// Shelter: a surface standing well below the topmost object at its own
		// column has something over it — a walkway under a roof, a stone tucked
		// beneath the next one up — and takes little snowfall. On a convex body
		// a flank point IS the top of its own column, so rocks are unaffected.
		// Thinned to a dusting rather than removed: the landscape shell never
		// kills coverage under shelter either, and stepping aside entirely
		// exposes the object's own projected snow, which agrees with nothing.
		float objTop = PatchTop(worldBase.xy);
		[flatten] if (objTop > -50000.0)
		{
			float shelterAmt = smoothstep(kShelterNear, kShelterFar, objTop - worldBase.z);
			depth = lerp(depth, min(depth, kShelterDust), shelterAmt);
			depthTarget = lerp(depthTarget, min(depthTarget, kShelterDust), shelterAmt);
		}
	}

	// The shape is settled here; everything past this point is distance LOD.
	float coverDepth = depth;

	// The S4 draw is the drape: a flat coat at the minimum lift, shaded by
	// the mesh normal, coverage decided per pixel by the coat block off the
	// game's real paint.
	[branch] if (ProjPixelEnable > 1.5)
	{
		debugLayer = 1.0;
		depth = depthBase;
		coverDepth = depth;
		upFacing = 1.0;
	}

	SkinLift o;
	o.WorldAbs = worldBase + liftWS * depth;

	o.Depth = depth;
	o.CoverDepth = coverDepth;
	o.Target = max(depthTarget, kMinSkinLift);
	o.RimT = rimT;
	o.Support = support;
	o.UpFacing = upFacing;
	o.ProjFactor = projFactor;
	o.ProjLinear = projLinear;
	o.DebugLayer = debugLayer;
	o.ShadeNormal = shadeNormal;
	return o;
}

// Displaced snow shades by the smooth surface it forms, not the flat face
// beneath; undisplaced vertices keep the raw normal.
float3 SkinShadingNormal(float3 nrmWS, float3 smoothWS, float isFlat, float depth, float rimT)
{
	float depthBase = max(lerp(RoundedDepth, ObjectsDepth, isFlat), kMinSkinLift);
	// SINGLE RETURN, blend initialized first. An early `return` inside a
	// `[branch]` raised X4000 here in the skin VS and the DS - the same shape
	// that silently broke the three layer accessors, and the last one left in
	// this shader. Behaviour is unchanged: same two blends, same operands.
	//
	// Rounded meshes: displaced snow shades by the smooth surface it forms,
	// not the flat face beneath. Flat plates shade by the plate itself - a
	// per-plank gradient stamps the same lighting onto every instance - except
	// the cornice roll, where geometry that curves over but shades flat reads
	// as a cut edge, so the last sliver before the rim bends toward the
	// smoothed normal and nothing inboard of it changes.
	float blend = saturate(depth / max(depthBase, 0.01)) * kSkinShadeSmooth;
	[flatten] if (isFlat > 0.5)
		blend = (1.0 - smoothstep(0.0, 0.35, rimT)) * 0.8;
	return normalize(lerp(nrmWS, smoothWS, blend));
}

// Vanilla's projected-UV weight, reconstructed from the same inputs
// (Lighting.hlsl projWeight); positive part, GRADED. Not vanilla's
// smoothstep(5*(0.1+w)): the +0.1 bias floors at 0.5 wherever the weight
// is zero and vanilla cancels it with the noise term this omits (round 2
// proved the thresholds here are ~0). Ungraded 5x amplification lit
// thin-trim beams as bright as solid fields, so the debug view could not
// distinguish "wants a dusting" from "wants full snow". Debug mode 5 only.
float ReconstructedProjMask(float nz, float vertexAlpha, float threshold)
{
	return saturate(nz * vertexAlpha - max(threshold, 0.0));
}
#endif

#if defined(VOXEL)
// Volume snow draw (VOLUME-SNOW-PLAN V1b): one brick AABB per instance, the
// snow field marched per pixel inside it. Layout must match VoxelDrawCB in
// SnowDeformation.h.
cbuffer VoxelDrawCB : register(b2)
{
	int4 VoxOrigin;    // xyz = window origin in voxels
	float4 VoxParams;  // x voxel size, y dim, z field threshold, w march step (voxels)
	float4 VoxFade;         // xy inner hand-over band, zw outer band (Chebyshev units from the window centres)
	float4 VoxCentre;       // xyz = this window's centre, relative to the camera
	float4 VoxInnerCentre;  // xyz = the next-finer window's centre, relative to the camera
	float4 VoxDebug;        // x = this level's index, y > 0.5 = tint the snow by level, z = max draw distance, w = its fade width
	float4 VoxDims;         // xyz = voxels a side, w = x / z (a wide ring's z reach is shorter by this)
}
StructuredBuffer<uint> VoxelBricks : register(t41);
Texture3D<float> VoxelFieldTex : register(t42);
SamplerState VoxelWrapSampler : register(s3);

struct VOXEL_VS_OUTPUT
{
	// noperspective centroid: what SV_DepthGreaterEqual requires of the
	// position input (X8000 otherwise, at game launch, silently).
	noperspective centroid float4 Position : SV_POSITION;
	// Camera-relative, on the brick's surface.
	float3 WorldPos : TEXCOORD0;
	nointerpolation float3 BrickMin : TEXCOORD1;
	// Which of the brick's eight 4^3 sub-cells hold a crossing.
	nointerpolation uint Mask : TEXCOORD2;
};
#endif

#if defined(VSHADER) && defined(VOXEL)
VOXEL_VS_OUTPUT main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	// 12 triangles over the 8 corners (bit 0 = x, 1 = y, 2 = z), six faces
	// of six vertices: +Z, -Z, +X, -X, +Y, -Y. Winding is irrelevant: the
	// faces are chosen here, not by the rasteriser.
	static const uint kCube[36] = { 4, 6, 7, 4, 7, 5, 0, 1, 3, 0, 3, 2, 1, 5, 7, 1, 7, 3,
		0, 2, 6, 0, 6, 4, 2, 3, 7, 2, 7, 6, 0, 4, 5, 0, 5, 1 };
	static const float3 kFaceNormal[6] = { float3(0, 0, 1), float3(0, 0, -1), float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0) };
	uint packed = VoxelBricks[instanceID];
	int3 brick = int3(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF);
	float3 minAbs = (float3)(brick * 8 + VoxOrigin.xyz) * VoxParams.x;
	uint corner = kCube[vertexID % 36];
	float3 c = float3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
	float brickSize = 8.0 * VoxParams.x;
	float3 rel = minAbs + c * brickSize - ShellCameraPosAdjust.xyz;
	VOXEL_VS_OUTPUT o;
	o.Position = mul(CameraViewProj, float4(rel, 1.0));
	// FRONT FACES ONLY, so the PS's SV_DepthGreaterEqual promise holds (a hit
	// lies at or past the face it was rasterised through) and the hardware
	// rejects, before the march, every fragment already behind what is
	// drawn - the bricks behind the first hit, which used to march in full.
	// A camera inside the brick sees only back faces: those are kept and
	// rasterised AT THE NEAR PLANE, which every hit is at or past.
	float3 camLocal = ShellCameraPosAdjust.xyz - minAbs;
	bool inside = all(camLocal > 0.0) && all(camLocal < brickSize);
	float3 n = kFaceNormal[(vertexID % 36) / 6];
	bool front = dot(n, camLocal) > max(dot(n, brickSize.xxx), 0.0);
	[flatten] if (inside)
		o.Position.z = 0.0;
	else if (!front)
		o.Position = float4(0.0, 0.0, 0.0, 1.0);
	// The hole for the next-finer level is cut HERE, every frame, not in
	// the brick list: a lazy ring rebuilds its list every 2^L frames, and a
	// hole cut then sits where the finer window WAS. A brick wholly inside
	// the finer window's core collapses to nothing.
	float halfBrick = 4.0 * VoxParams.x;
	float3 fromInner = abs(minAbs + halfBrick - ShellCameraPosAdjust.xyz - VoxInnerCentre.xyz);
	[flatten] if (all((fromInner + halfBrick) * float3(1.0, 1.0, VoxDims.w) < VoxFade.x))
		o.Position = float4(0.0, 0.0, 0.0, 1.0);
	o.WorldPos = rel;
	o.BrickMin = minAbs - ShellCameraPosAdjust.xyz;
	o.Mask = packed >> 24;
	return o;
}
#endif

#if defined(VSHADER) && !defined(PATCH) && !defined(VOXEL)
// Object -> world transform and the smoothed-normal lookup. The lift is
// applied separately so the domain shader can evaluate it per generated
// vertex.
struct SkinVertex
{
	float3 WorldBase;
	float3 NormalWS;
	float3 SmoothWS;
	float Flat;
	// Authored NIF vertex alpha (vanilla's projected-mask term), decoded
	// from OutNormals.w = 1 + alpha; 1 where the mesh carries none.
	float VertexAlpha;
};

SkinVertex BuildSkinVertex(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 nrmMS = input.Normal.xyz * 2.0 - 1.0;

	// Pillow inflation: displace along POSITION-AVERAGED normals where
	// available. Split-normal flat meshes (planks, roofs, pole caps) get the
	// smooth normals they lack; shared-position twins displace identically
	// (rim cracks sealed by construction), plank edges mushroom outward like
	// pole caps. Already-smooth meshes are unchanged (average == raw).
	float3 inflateMS = nrmMS;
	float isFlat = 0.0;
	float vertexAlpha = 1.0;
	[branch] if (HasSmoothedNormals > 0.5)
	{
		// Mesh-level flatness stats (element appended past the last vertex):
		// split-normal plates; walkways, roofs, planks; score a high
		// divergent fraction and get completely flat snow (straight-up
		// offset, raw shading normal, separate depth slider). Organically
		// smooth meshes keep the pillow. Divergence-only on purpose:
		// alignment-based extensions misclassify real compound meshes (see
		// FlatStatsCS); roofs classifying rounded is the accepted cost.
		float4 flatStats = SmoothedNormals[(uint)VertexCountF];
		[flatten] if (flatStats.w > 0.5 && flatStats.x > 0.5)
			isFlat = 1.0;
		float4 smoothEntry = SmoothedNormals[input.VertexID];
		[flatten] if (smoothEntry.w > 0.5)
		{
			inflateMS = smoothEntry.xyz;
			vertexAlpha = saturate(smoothEntry.w - 1.0);
		}
	}
	// CPU class overrides, outside the smoothed-normals guard so a mesh
	// without the buffer still classifies (matching the capture raster):
	// 1 = rounded (mountain/cliff; every PD draw in authored-relief mode),
	// 2 = flat (plank family, the cornice treatment).
	[flatten] if (ClassOverride > 1.5)
		isFlat = 1.0;
	else [flatten] if (ClassOverride > 0.5)
		isFlat = 0.0;

	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);
	float3 nrmWS = normalize(float3(
		dot(WorldRow0.xyz, nrmMS),
		dot(WorldRow1.xyz, nrmMS),
		dot(WorldRow2.xyz, nrmMS)));
	// Position-averaged normal in world space. The mask, the shading normal
	// and the domain shader's relief all key off the SURFACE; only the lift
	// uses a direction of its own.
	float3 smoothWS = normalize(float3(
		dot(WorldRow0.xyz, inflateMS),
		dot(WorldRow1.xyz, inflateMS),
		dot(WorldRow2.xyz, inflateMS)));
	SkinVertex v;
	v.WorldBase = worldAbs;
	v.NormalWS = nrmWS;
	v.SmoothWS = smoothWS;
	v.Flat = isFlat;
	v.VertexAlpha = vertexAlpha;
	return v;
}

#if !defined(SNOW_TESS)
VS_OUTPUT main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	SkinLift lift = ApplySkinLift(v.WorldBase, v.NormalWS, v.SmoothWS, v.Flat, v.VertexAlpha);

	float3 rel = lift.WorldAbs - ShellCameraPosAdjust.xyz;
	float3 prevRel = lift.WorldAbs - ShellCameraPreviousPosAdjust.xyz;
	// S4 coat: the whole shell kEdgeFlankLift toward the eye, IN GEOMETRY,
	// so it wins the depth test against its own object at grazing views.
	// The PS push (edgeFlankLift / coatPush) reaches the depth buffer only
	// on the depth-exporting variant, which trench-less skins never bind -
	// Josef's roof planks stayed bright through that fix (2026-09-04).
	[flatten] if (ProjPixelEnable > 1.5)
	{
		float3 toEye = normalize(rel);
		rel -= toEye * kEdgeFlankLift;
		prevRel -= toEye * kEdgeFlankLift;
	}

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
	vsout.WorldPos = rel;
	// S4 draws shade by the mesh normal.
	vsout.NormalWS = ProjPixelEnable > 1.5 ? lift.ShadeNormal : SkinShadingNormal(v.NormalWS, v.SmoothWS, v.Flat, lift.Depth, lift.RimT);
	// raw normal Z, interpolated; the PS runs the up-facing smoothstep per
	// pixel. Thresholding here makes low-poly rocks flip whole FACES between
	// snowed and bare; thresholding the interpolated normal varies smoothly.
	// Debug view: smuggle the two lift masks through the shading interpolants.
	// Mode 5 pairs the current up-facing mask with vanilla's reconstructed
	// projection mask so one screenshot says whether the authored vertex
	// alpha carries information the normal test lacks (SKIN-PLACEMENT-PLAN).
	// Modes 8-10: the draw's layer and the depth it was granted.
	[flatten] if (StaticsDebugView > 7.5)
	{
		vsout.Coverage = (lift.DebugLayer + 0.5) / 8.0;
		vsout.Flat = saturate(lift.Depth / max(lerp(RoundedDepth, ObjectsDepth, v.Flat), kMinSkinLift));
	}
	else [flatten] if (StaticsDebugView > 4.5)
	{
		vsout.Coverage = lift.UpFacing;
		vsout.Flat = ReconstructedProjMask(v.NormalWS.z, v.VertexAlpha, ProjThreshold);
	}
	else [flatten] if (StaticsDebugView > 2.5)
	{
		vsout.Coverage = v.SmoothWS.z * 0.5 + 0.5;
		vsout.Flat = v.Flat;
	}
	else
	{
		vsout.Coverage = StaticsDebugView != 0.0 ? lift.Support : v.NormalWS.z;
		vsout.Flat = StaticsDebugView != 0.0 ? lift.UpFacing : v.Flat;
	}
	vsout.GridLocal = lift.WorldAbs.xy - GridOrigin;
	vsout.Lift = lift.CoverDepth;
	vsout.LiftTarget = lift.Target;
	// Authored-relief mode repurposes the interpolant: the raw authored
	// vertex alpha, for the PS's per-pixel weight rebuild.
	vsout.ProjFactor = ProjPixelEnable > 1.5 ? lift.ProjLinear : lift.ProjFactor;
#	ifdef SNOW_COAT_PARALLAX
	vsout.ObjUV = input.TexCoord0 * ObjParallaxUV.zw + ObjParallaxUV.xy;
	const float3x3 world3 = float3x3(WorldRow0.xyz, WorldRow1.xyz, WorldRow2.xyz);
	vsout.ObjT = normalize(mul(world3, float3(input.Position.w, input.Normal.w * 2.0 - 1.0, input.Bitangent.w * 2.0 - 1.0)));
	vsout.ObjB = normalize(mul(world3, input.Bitangent.xyz * 2.0 - 1.0));
	vsout.ObjN = normalize(mul(world3, input.Normal.xyz * 2.0 - 1.0));
#	endif
	return vsout;
}
#else
// Tessellated control-point VS: transform only, UNDISPLACED. The lift is the
// domain shader's job so it lands on generated vertices.
struct TessControlPoint
{
	float3 WorldBase : TEXCOORD0;
	float3 NormalWS : TEXCOORD1;
	float3 SmoothWS : TEXCOORD2;
	float Flat : TEXCOORD3;
	float VertexAlpha : TEXCOORD4;
};

TessControlPoint main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	TessControlPoint cp;
	cp.WorldBase = v.WorldBase;
	cp.NormalWS = v.NormalWS;
	cp.SmoothWS = v.SmoothWS;
	cp.Flat = v.Flat;
	cp.VertexAlpha = v.VertexAlpha;
	return cp;
}
#endif
#endif

#if (defined(HULLSHADER) || defined(DOMAINSHADER)) && !defined(PATCH)
struct TessControlPoint
{
	float3 WorldBase : TEXCOORD0;
	float3 NormalWS : TEXCOORD1;
	float3 SmoothWS : TEXCOORD2;
	float Flat : TEXCOORD3;
	float VertexAlpha : TEXCOORD4;
};

struct TessFactors
{
	float Edge[3] : SV_TessFactor;
	float Inside : SV_InsideTessFactor;
};
#endif

#if defined(HULLSHADER) && !defined(PATCH)
// Edge factor from edge length over a distance-scaled target triangle
// size: object triangles vary from centimeters to many meters, so a pure
// distance rule would waste factors on tiny triangles and starve huge
// ones. Shared mesh edges carry identical control points on both sides,
// so the symmetric rule is crack-free.
float EdgeTessFactor(float3 worldA, float3 worldB)
{
	float3 mid = 0.5 * (worldA + worldB);
	float dist = length(mid - ShellCameraPosAdjust.xyz);
	float targetLen = max(4.0, dist * 0.01);
	// P1 (edge-research study): the cornice roll is kCorniceRoll world units
	// wide, and the distance rule alone gives it ~ONE segment at any range -
	// a quarter-circle sampled once is a straight ramp, which is the
	// stretched rim. Where the cone says a rim is close, the target edge
	// length drops toward 1 unit so the roll gets the vertices the fillet
	// needs; interiors and the far field keep the old rule. Sampled at both
	// endpoints and the midpoint - shared edges see the same three points
	// from either side, so the rule stays crack-free.
	[branch] if (HasObjectTop > 0.5)
	{
		float coneNear = min(ObjectConeDepth(mid.xy),
			min(ObjectConeDepth(worldA.xy), ObjectConeDepth(worldB.xy)));
		float rimBoost = (1.0 - smoothstep(4.0, 12.0, coneNear)) *
		                 (1.0 - smoothstep(1200.0, 2400.0, dist));
		targetLen = lerp(targetLen, 1.0, rimBoost);
	}
	// Pixel floor: a segment is never shorter on screen than the cap. Zero
	// off; below the base rule's ~20 px it only trims the rim term at range.
	targetLen = max(targetLen, dist * SkinTessCapSlope);
	return clamp(length(worldA - worldB) / targetLen, 1.0, 16.0);
}

TessFactors PatchConstants(InputPatch<TessControlPoint, 3> patch)
{
	TessFactors f;
	// Tri-domain edge order: edge i is opposite control point i.
	f.Edge[0] = EdgeTessFactor(patch[1].WorldBase, patch[2].WorldBase);
	f.Edge[1] = EdgeTessFactor(patch[2].WorldBase, patch[0].WorldBase);
	f.Edge[2] = EdgeTessFactor(patch[0].WorldBase, patch[1].WorldBase);
	f.Inside = max(max(f.Edge[0], f.Edge[1]), f.Edge[2]);
	return f;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")]
// EdgeTessFactor clamps to 16. Without it fxc instances the fork phase per
// edge and bakes edge 0's vertex reads into all three (CODE-NOTES).
[maxtessfactor(16.0)]
TessControlPoint main(InputPatch<TessControlPoint, 3> patch, uint i : SV_OutputControlPointID)
{
	return patch[i];
}
#endif

#if defined(DOMAINSHADER) && !defined(PATCH)
[domain("tri")]
VS_OUTPUT main(TessFactors factors, float3 bary : SV_DomainLocation, const OutputPatch<TessControlPoint, 3> patch)
{
	float3 worldBase = patch[0].WorldBase * bary.x + patch[1].WorldBase * bary.y + patch[2].WorldBase * bary.z;
	// Guarded normalization: control points with opposing normals (hard
	// mesh edges) interpolate to near-zero vectors whose normalization
	// explodes into arbitrary directions; those regions also get no relief.
	float3 nSum = patch[0].NormalWS * bary.x + patch[1].NormalWS * bary.y + patch[2].NormalWS * bary.z;
	float3 iSum = patch[0].SmoothWS * bary.x + patch[1].SmoothWS * bary.y + patch[2].SmoothWS * bary.z;
	float interpHealth = min(length(nSum), length(iSum));
	float3 normalWS = nSum / max(length(nSum), 1e-3);
	float3 inflateWS = iSum / max(length(iSum), 1e-3);
	float isFlat = patch[0].Flat * bary.x + patch[1].Flat * bary.y + patch[2].Flat * bary.z;
	float vertexAlpha = patch[0].VertexAlpha * bary.x + patch[1].VertexAlpha * bary.y + patch[2].VertexAlpha * bary.z;

	// The lift is evaluated HERE, per generated vertex: the up-facing mask and
	// the edge taper get tessellated density instead of being interpolated
	// across a source face.
	SkinLift lift = ApplySkinLift(worldBase, normalWS, inflateWS, isFlat, vertexAlpha);
	float3 worldAbs = lift.WorldAbs;
	float2 gridLocal = worldAbs.xy - GridOrigin;
	// Same S4 normal selection as the untessellated VS.
	normalWS = ProjPixelEnable > 1.5 ? lift.ShadeNormal : SkinShadingNormal(normalWS, inflateWS, isFlat, lift.Depth, lift.RimT);

	// Relief from the displacement map, same recipe as the landscape shell:
	// top-projected snow UV, gated by the inflated depth (bare and thin
	// spots stay put), carved smooth by deformation, biased to up-facing
	// surfaces, faded with the micro-normal distance band.
	[branch] if (HasSnowHeight > 0.5 && SnowReliefDepth > 0.01)
	{
		float camDist = length(worldAbs - ShellCameraPosAdjust.xyz);
		float reliefFade = 1.0 - smoothstep(600.0, 2200.0, camDist);
		[branch] if (reliefFade > 0.001 && lift.Depth > 0.5)
		{
			float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
			float mip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);
			// Through the PS's anti-tiling taps. Top-plane taps specifically:
			// the relief is already biased to up-facing surfaces by
			// saturate(inflateWS.z), and that is exactly where the PS's
			// two-plane blend hands the pixel to the top plane too.
			float h = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, worldAbs.xy), 0.0.xx, mip);
			float carve = saturate(SampleDeformation(gridLocal));
			worldAbs += inflateWS * ((h - 0.5) * SnowReliefDepth * reliefFade * saturate(lift.Depth / 6.0) * (1.0 - carve) * saturate(inflateWS.z) * smoothstep(0.3, 0.7, interpHealth));
		}
	}

	float3 rel = worldAbs - ShellCameraPosAdjust.xyz;
	float3 prevRel = worldAbs - ShellCameraPreviousPosAdjust.xyz;
	// S4 coat: the whole shell kEdgeFlankLift toward the eye, IN GEOMETRY,
	// so it wins the depth test against its own object at grazing views.
	// The PS push (edgeFlankLift / coatPush) reaches the depth buffer only
	// on the depth-exporting variant, which trench-less skins never bind -
	// Josef's roof planks stayed bright through that fix (2026-09-04).
	[flatten] if (ProjPixelEnable > 1.5)
	{
		float3 toEye = normalize(rel);
		rel -= toEye * kEdgeFlankLift;
		prevRel -= toEye * kEdgeFlankLift;
	}

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
	vsout.WorldPos = rel;
	vsout.NormalWS = normalWS;
	// Debug view: smuggle the two lift masks through the shading interpolants.
	// Mode 3 swaps in upFacing's OWN two inputs instead (smoothed normal z,
	// and the flat/rounded class), because those are the only things that can
	// zero the lift; a surface that looks up-facing but reads UpFacing 0 is
	// then traceable to whichever of the two is lying.
	float smoothZ = nSum.z / max(length(nSum), 1e-3);
	[flatten] if (StaticsDebugView > 7.5)
	{
		// Modes 8-10: identical encoding to the untessellated VS.
		vsout.Coverage = (lift.DebugLayer + 0.5) / 8.0;
		vsout.Flat = saturate(lift.Depth / max(lerp(RoundedDepth, ObjectsDepth, isFlat), kMinSkinLift));
	}
	else [flatten] if (StaticsDebugView > 4.5)
	{
		// Mode 5: identical encoding to the untessellated VS. smoothZ is the
		// RAW interpolated normal's z (normalWS holds the shading normal by
		// now), which is the vertex-level analog of vanilla's worldNormal.
		vsout.Coverage = lift.UpFacing;
		vsout.Flat = ReconstructedProjMask(smoothZ, vertexAlpha, ProjThreshold);
	}
	else [flatten] if (StaticsDebugView > 2.5)
	{
		vsout.Coverage = smoothZ * 0.5 + 0.5;
		vsout.Flat = isFlat;
	}
	else
	{
		vsout.Coverage = StaticsDebugView != 0.0 ? lift.Support : smoothZ;
		vsout.Flat = StaticsDebugView != 0.0 ? lift.UpFacing : isFlat;
	}
	vsout.GridLocal = gridLocal;
	vsout.Lift = lift.CoverDepth;
	vsout.LiftTarget = lift.Target;
	// Same repurposing as the untessellated VS: raw authored alpha in
	// authored-relief mode.
	vsout.ProjFactor = ProjPixelEnable > 1.5 ? lift.ProjLinear : lift.ProjFactor;
	return vsout;
}
#endif

#ifdef PSHADER




// SampleSnowPlanar / SnowParallaxOcclusionPlanar moved to SnowParallax.hlsli
// (Stage 2 P3): the landscape shell runs the same two-plane blend now.


struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
	float4 Masks2 : SV_Target7;
#	if !defined(PATCH) && !defined(SNOW_STATICS_NO_DEPTH_EXPORT)
	// Written so the parallax trench relief is real to the depth buffer:
	// the carved floor's projected depth replaces the flat top's, so feet
	// and props z-test against the trench instead of vanishing under it,
	// and camera motion sees a geometrically consistent surface. The PATCH
	// is real geometry and skips it; keeping early-z, which is what makes
	// its full-span coverage cheap (hidden pixels reject before shading).
	// The VOXEL march promises its hit is at or past the face it came in
	// through, so early-z rejects bricks behind an earlier hit.
#		if defined(VOXEL)
	float Depth : SV_DepthGreaterEqual;
#		else
	float Depth : SV_Depth;
#		endif
#	endif
};

#ifdef SNOW_PATCH_ATTR
// The deferred trench patch's attribute pass, into the ground's deferred
// targets: GridLocal + the shading normal's xy; GridLocal's coarse
// derivatives; view depth + WorldPos.z's coarse derivatives. Mark: 1 on every
// pixel the cut kept, in a cleared R8 target the shading pass tests first.
struct PATCH_ATTR_OUTPUT
{
	float4 Grid : SV_Target0;
	float4 Deriv : SV_Target1;
	float4 Depth : SV_Target2;
	float Mark : SV_Target3;
};
#endif

// Smooth value noise (~24-unit cells) modulating the coverage edge, standing
// in for the projection's noise texture so snow extent looks organic rather
// than a hard slope threshold.
float CoverageNoise(float2 worldXY)
{
	float2 c = worldXY / 24.0;
	float2 i = floor(c);
	float2 f = frac(c);
	f = f * f * (3.0 - 2.0 * f);
	float n00 = StochasticHash(i).x;
	float n10 = StochasticHash(i + float2(1, 0)).x;
	float n01 = StochasticHash(i + float2(0, 1)).x;
	float n11 = StochasticHash(i + float2(1, 1)).x;
	return lerp(lerp(n00, n10, f.x), lerp(n01, n11, f.x), f.y);
}

// Scene depth COPY (kPOST_ZPREPASS_COPY, or Terrain Blending's blended depth
// when that feature owns it) - the same source, the same slot and the same
// helper the terrain shell binds at t3. Never the bound DSV, so sampling it
// while writing depth is legal.
Texture2D<float> SceneDepth : register(t3);
// The main depth the prepass copied from (the prepass's hidden-quad exit and
// debug view 9), and, view 9 only, the private copy after the prepass.
Texture2D<float> PreSkinMainDepth : register(t43);
Texture2D<float> DebugPrepassDepth : register(t44);

// Debug view 10 only: volume level 0 (mirror of VoxelVolumeCB, Dbg-prefixed
// so the names stay clear of StaticCB's height window).
cbuffer VoxelSeedDebugCB : register(b3)
{
	int4 DbgOriginVox;
	int4 DbgScrollDelta;
	float DbgVoxelSize;
	float DbgDecay;
	int DbgPadDim;
	int DbgSliceAxis;
	int DbgSliceIndex;
	int DbgSliceXray;
	int DbgSliceSource;
	int DbgBlurAxis;
	float2 DbgHeightWindowCenter;
	float DbgHeightHalfExtent;
	float DbgShelterDust;
	float DbgDepthVox;
	float DbgFieldThreshold;
	float DbgRoundSigma;
	float DbgSlopeMinNz;
	float4 DbgCentreVox;
	float DbgSkyStrength;
	float DbgForceDirty;
	float DbgOverhangVox;
	float DbgHeadroomVox;
	float4 DbgEdgeParams;
	float4 DbgLipParams;
	int4 DbgDims;
}
Texture3D<float> DbgVoxOcc : register(t46);
Texture3D<float> DbgVoxField : register(t47);
Texture2D<uint> DbgVoxHeight : register(t48);
Texture2DArray<uint> DbgVoxHeightUnder : register(t54);
// Mirrors kVoxelUnderLayers / SnowVoxelCapture's VOXEL_UNDER_LAYERS.
#define VOXEL_UNDER_LAYERS 4
Texture2D<float2> DbgShelter : register(t49);
Texture2D<float> DbgSky : register(t51);


// THE skin material and lighting, factored out of the skin's main: taps,
// two-plane projection, parallax, normal map, RMAOS, cascades +
// heightfield march + screen-space shadows, sun PBR, point lights,
// skylighting.
struct SkinShadeInput
{
	float3 WorldPos;
	float4 Position;
	float4 CurrentClip;
	float Flat;
	float2 GridLocal;
	float2 trenchGridLocal;
	float2 worldXY;
	float pixelDist;
	float pixelDeform;
	float screenNoise;
	// >0 on coat/lump pixels: cascade occluders nearer than this along the
	// light are the raised shell's own rim over its object and are ignored.
	float selfShadowReject;
	// 1 = the drape: on its object's own surface, no lift. It takes the
	// shadows the game drew for that surface (mask + screen-space shadows)
	// instead of sampling maps from a position of its own.
	float onObject;
	// 1 = the full material. Toward 0 the parts invisible at range - the
	// parallax marches, the berm relief, the horizon march - drop out. The
	// skins pass 1; the volume draw fades it with distance.
	float detail;
	// 1 = parallax-occlusion relief. The volume passes 0: fresh snow's height
	// variation is the field's own, and the glints stay regardless.
	float pom;
	// |z| of the facet's own normal (screen derivatives), or -1 where there
	// is no facet (patch, volume). The texture plane trusts the interpolated
	// normal only within 0.3 of it: smooth-shaded box meshes carry corner
	// normals that read a vertical face as half up-facing.
	float geoNz;
	// Absolute z of the surface the snow lies on: a mark reaches the pixel
	// where its decal covers that height (SnowFields.hlsli MarkReaches).
	// kMarkAnyBase on the road patch.
	float markBase;
#ifdef SNOW_COAT_PARALLAX
	// Snow Follows Parallax: where the snow's material is read, relative to
	// the coat pixel (ObjectParallaxShift).
	float3 surfaceShift;
#endif
};
struct SkinShadeResult
{
	float3 normalWS;
	float3 viewNormal;
	float3 ambientPart;
	float3 diffuseLobe;
	float3 directSpecular;
	float3 specularLobe;
	float3 preLit;
	float3 dbgMarch;
	float dbgMarchRan;
	float landVertexAO;
	float snowRoughness;
	// The trench detail's mask (patch), for its coverage view.
	float detailMask;
};
#if defined(SNOW_CONDITION_VIEW) && defined(PATCH)
// Snow Condition View's track term on the road patch, as the ground's: the
// trench's age (6) or its glaze (7).
float2 PatchConditionTrack(float2 a_trenchGridLocal, float2 a_worldXY, float a_deform, float a_worldZ)
{
	float2 track = 0.0;
	[branch] if (SnowClimate2.z < 6.5)
	{
		const float4 field = BermFieldPair(a_trenchGridLocal);
		track = float2(lerp(TrackAgeOf(field.w), TrackAgeOf(field.z), smoothstep(0.0, 0.1, a_deform)), saturate(10.0 * max(a_deform, field.x)));
	}
	else
		track.x = SnowIceLook(a_deform, saturate(SampleExclusionField(a_worldXY).y), a_worldZ);
	return track;
}
#endif

SkinShadeResult SkinShadeSurface(SkinShadeInput input, float3 normalWS)
{
	// Where lighting and shadows are evaluated; depth readers stay on the sheet.
	float3 shadePos = input.WorldPos;
#ifdef SNOW_OBJECT_PRINTS
	// The print under this coat pixel, depth and slope, at the coat's own point.
	// The coat has already stepped off the print's bare core (Masks.y), so what
	// shades here is the rim. Its slope, shadows and detail are a
	// heightfield's: tops only.
	const float3 printAt = float3(input.worldXY, input.WorldPos.z + ShellCameraPosAdjust.z);
	const float3 print = SnowPrints::DepthSlope(printAt);
	// The broken snow around it (Object Print Rim), as a share of a full print.
	const float printRim = SnowPrints::Rim(printAt) / max(SharedData::snowDeformationSettings.PrintFullDepth, 0.001);
	const float printTop = smoothstep(0.5, 0.75, input.geoNz);
#endif
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
	// Trench detail tiles by parallax occlusion, the ground's own: the mask
	// across the trench profile at the sheet point, the view ray marched
	// through the detail heights and the point it meets shaded. Fire marks and
	// impacts wear the impact set, impacts lifting the relief, as on the ground.
	// The print twin runs it across a print's rim.
#	ifdef SNOW_PATCH_DEFERRED
	const float detailLod = TrenchDetailLod(gDeferredGridDx, gDeferredGridDy);
#	else
	const float detailLod = TrenchDetailLod(ddx(input.worldXY), ddy(input.worldXY));
#	endif
	const float detailFade = 1.0 - smoothstep(1200.0, 2000.0, input.CurrentClip.w);
	float detailMask = 0.0;
	float detailRelief = 0.0;
	TrenchDetailTaps trenchTaps = (TrenchDetailTaps)0;
	float detailHitH = 0.0;
	float detailImpact = 0.0;
	const float2 detailPlane = -normalWS.xy / max(normalWS.z, 0.1);
	[branch] if (detailFade > 0.0 && (TrenchDetailRelief > 0.001 || TrenchDetailView != 0 || abs(TrenchDetailRoughness - 1.0) > 0.001))
	{
#	ifdef PATCH
		const float4 patchField = BermFieldPair(input.trenchGridLocal);
		detailMask = TrenchDetailMask(input.pixelDeform, patchField.xy, TrenchDetailCoverage) * detailFade;
		// Speed Spoil, as on the ground: thrown snow past the trench.
		float2 marks = 0.0;
		[branch] if (SpeedSpoil > 0.5)
		{
			marks = SampleDetailMarks(input.trenchGridLocal);
			detailMask = max(detailMask, TrenchSpoilMask(marks.y) * detailFade);
		}
#	else
		// No berm on an object: the depth as a share of a full print is the
		// carve, and the rim, which reaches past it, the blur around it.
		const float printRel = print.x / max(SharedData::snowDeformationSettings.PrintFullDepth, 0.001);
		detailMask = TrenchDetailMask(printRel, float2(0.0, max(printRel, printRim)), TrenchDetailCoverage) * detailFade * printTop;
#	endif
		[branch] if (detailMask > 0.001)
		{
#	ifdef PATCH
			[branch] if (SpeedSpoil < 0.5)
				marks = SampleDetailMarks(input.trenchGridLocal);
			const float meltWet = smoothstep(kSpellMarkStart, 0.15, marks.x);
			// An old track's clods are under new snow; fire's melt is not. The
			// carve's age in the trench, the berm's on its apron.
			const float detailAge = lerp(TrackAgeOf(patchField.w), TrackAgeOf(patchField.z), saturate(10.0 * input.pixelDeform));
			const float impact = marks.y * TrackDetailImpactScale(detailAge);
			detailImpact = max(meltWet, impact);
			detailRelief = TrenchDetailRelief * detailMask * max(saturate(PatchSkinDepth(input.worldXY).x / 10.0), meltWet) * TrampleDetailScale(input.worldXY, input.pixelDeform) *
			               (1.0 + kImpactReliefBoost * impact) * TrackDetailReliefScale(detailAge);
#	else
			// The patch's thin-cover rule at the dent's depth, up to the full
			// relief where the rim is strong; prints take twice the slider.
			detailRelief = 2.0 * TrenchDetailRelief * detailMask * max(saturate(kPrintDentDepth / 10.0), saturate(printRim));
#	endif
			trenchTaps = TrenchDetailTapsAt(input.worldXY);
		}
	}
	[branch] if (detailRelief > 0.05)
	{
		float3 dir = normalize(input.WorldPos);
		const float4 duv = TrenchDetailStep(trenchTaps, dir.xy);
		float t = TrenchDetailParallaxT(dir, detailPlane, trenchTaps, duv,
			float3(detailRelief, detailLod, lerp(0.6, 2.0, saturate(input.CurrentClip.w / 1500.0))), detailImpact, input.screenNoise);
		detailHitH = (dir.z - dot(dir.xy, detailPlane)) * t;
		shadePos = input.WorldPos + dir * t;
		input.worldXY += dir.xy * t;
		input.trenchGridLocal += dir.xy * t;
		input.GridLocal += dir.xy * t;
		trenchTaps.uv += duv * t;
	}
#endif
	const float2 trenchGridLocal = input.trenchGridLocal;
	const float2 worldXY = input.worldXY;
	const float pixelDist = input.pixelDist;
	const float pixelDeform = input.pixelDeform;
	const float screenNoise = input.screenNoise;
	// Glazed snow takes less of Parallax Depth and Parallax Shadow, as on the
	// ground; a coat is never pressed.
#ifdef PATCH
	const float patchMelt = saturate(SampleExclusionField(worldXY).y);
	const float iceParallax = SnowIceParallax(pixelDeform, patchMelt, input.WorldPos.z + ShellCameraPosAdjust.z);
#elif defined(SNOW_OBJECT_PRINTS)
	const float iceParallax = SnowIceParallax(print.x, 0.0, input.WorldPos.z + ShellCameraPosAdjust.z);
#else
	const float iceParallax = 1.0;
#endif
	// Snow texture taps, shared by albedo, normal and RMAOS, sampled at the
	// parallax-corrected position. Steep drape sides re-project along the
	// facing wall plane, since the top-down projection stretches down flanks.
	// Blended as SAMPLES, never as coordinates: lerping UVs gives a field
	// belonging to neither plane, so the whole transition band smears.
	float2 snowUV = (SnowUVOffset + trenchGridLocal) / kSnowUVTile;
#ifdef PATCH
	// The landscape ramp, not the statics one. The patch is the landscape
	// recipe on objects and its trench walls live in the same 40-65 degree
	// band the shell's comment describes - at 30-unit depth a wall's n.z is
	// ~0.45, which the shell hands fully to the side plane while the statics
	// ramp below leaves it two-thirds top-projected: stretched grain, a
	// grain-shadow march over stretched UVs, the bright warped band along
	// road trench walls. At 64 the wall is steep enough that both ramps
	// agree, which is why the band vanished there; at 10 neither engages.
	float snowSteepness = smoothstep(0.75, 0.55, abs(normalWS.z));
#else
	// PARITY with the terrain shell and the patch (Josef 2026-09-01: object
	// snow read a visibly different colour from the landscape beside it).
	// This drives the two-plane blend for albedo, normal AND rmaos, so a
	// different ramp is a different material on the same slope. WAS
	// smoothstep(0.55, 0.25): a deliberate tune, to hold the top projection
	// longer across rock flanks at n.z 0.4-0.7. Revert this line first if
	// flanks now read stretched.
	// The interpolated normal within 0.3 of the facet's: a smooth-shaded wall
	// mesh interpolates its cube-corner normals to nz ~0.7 across a vertical
	// face and took the top plane (Josef's striated pillar, RenderDoc frame
	// 2506); rock normals never stray that far from their facet, so they
	// keep the smooth ramp and grow no facet seams.
	float nzPlane = abs(normalWS.z);
	[flatten] if (input.geoNz >= 0.0)
		nzPlane = clamp(nzPlane, input.geoNz - 0.3, input.geoNz + 0.3);
	float snowSteepness = smoothstep(0.75, 0.55, nzPlane);
#endif
	float snowWorldZAbs = shadePos.z + ShellCameraPosAdjust.z;
	// Captured, not recomputed: normalWS is perturbed further below (berm
	// ridge, normal map), and the parallax shadow must resolve the light into
	// the SAME plane these uvs were built on.
	bool snowSideDropsX = abs(normalWS.x) > abs(normalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXY.y, snowWorldZAbs) : float2(worldXY.x, snowWorldZAbs);
	// NO SnowUVOffset here + static 4096-unit fold, in step with the
	// landscape shell (see its comment): the offset compensates a rebasing
	// coordinate, and this plane is absolute - adding it slid drape-side
	// texture on every grid scroll, just too subtly to notice on small
	// near-vertical sides.
	float2 snowUVSideUnfolded = snowSidePlane / kSnowUVTile;
	float2 snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	float bumpFade = 1.0 - smoothstep(600.0, 2200.0, pixelDist);
#ifdef SNOW_COAT_PARALLAX
	// Snow Follows Parallax: the material where the object's parallax drew
	// its texel, at the mesh's own derivatives. Glints ride shadePos.
	const float4 snowUVDeriv = float4(ddx(snowUV), ddy(snowUV));
	const float4 snowUVSideDeriv = float4(ddx(snowUVSideUnfolded), ddy(snowUVSideUnfolded));
	const float2 sideShift = snowSideDropsX ? input.surfaceShift.yz : input.surfaceShift.xz;
	snowUV += input.surfaceShift.xy / kSnowUVTile;
	snowSidePlane += sideShift;
	snowUVSideUnfolded += sideShift / kSnowUVTile;
	snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	shadePos += input.surfaceShift;
	SnowTaps snowTaps = ComputeSnowTapsNoGrad(snowUV, worldXY + input.surfaceShift.xy);
	snowTaps.duvdx = snowUVDeriv.xy;
	snowTaps.duvdy = snowUVDeriv.zw;
	SnowTaps snowTapsSide = ComputeSnowTapsNoGrad(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = snowUVSideDeriv.xy;
	snowTapsSide.duvdy = snowUVSideDeriv.zw;
	float snowHeightMip = SnowHeightMipGrad(snowUVDeriv.xy, snowUVDeriv.zw);
	float snowHeightMipSide = SnowHeightMipGrad(snowUVSideDeriv.xy, snowUVSideDeriv.zw);
#elif defined(SNOW_PATCH_DEFERRED)
	// The attribute pass's derivatives (the detail-parallax shift's own is
	// dropped, as on the ground).
	SnowTaps snowTaps = ComputeSnowTapsNoGrad(snowUV, worldXY);
	snowTaps.duvdx = gDeferredGridDx / kSnowUVTile;
	snowTaps.duvdy = gDeferredGridDy / kSnowUVTile;
	SnowTaps snowTapsSide = ComputeSnowTapsNoGrad(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = (snowSideDropsX ? float2(gDeferredGridDx.y, gDeferredZDx) : float2(gDeferredGridDx.x, gDeferredZDx)) / kSnowUVTile;
	snowTapsSide.duvdy = (snowSideDropsX ? float2(gDeferredGridDy.y, gDeferredZDy) : float2(gDeferredGridDy.x, gDeferredZDy)) / kSnowUVTile;
	float snowHeightMip = SnowHeightMipGrad(snowTaps.duvdx, snowTaps.duvdy);
	float snowHeightMipSide = SnowHeightMipGrad(snowTapsSide.duvdx, snowTapsSide.duvdy);
#else
	SnowTaps snowTaps = ComputeSnowTaps(snowUV, worldXY);
	SnowTaps snowTapsSide = ComputeSnowTaps(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = ddx(snowUVSideUnfolded);
	snowTapsSide.duvdy = ddy(snowUVSideUnfolded);
	// Uniform flow: the parallax shadow branch below is divergent, and
	// derivatives taken inside it would be garbage at its edges.
	float snowHeightMip = SnowHeightMip(snowUV);
	float snowHeightMipSide = SnowHeightMip(snowUVSideUnfolded);
#endif

	// Object trench detail: shading-only berm ridge along trails. Geometry
	// berm waits for the skin rework.
	float4 bermField = 0.0;
	[branch] if (BermHeightAmp > 0.005)
		bermField = BermFieldPair(trenchGridLocal);
	const float bermC = bermField.x;
#ifdef PATCH
	// The patch's berm is real geometry (BuildPatchVertex), shaded by the
	// vertex normal it displaced. Adding the shading ridge here too would
	// double it.
	[branch] if (false)
#else
	[branch] if (BermHeightAmp > 0.005 && bermC > 0.003 && input.detail > 0.001)
#endif
	{
		const float bStep = BermGradStep();
		float2 bermGrad = float2(
			BermShape(BermField(trenchGridLocal + float2(bStep, 0.0))) - BermShape(BermField(trenchGridLocal - float2(bStep, 0.0))),
			BermShape(BermField(trenchGridLocal + float2(0.0, bStep))) - BermShape(BermField(trenchGridLocal - float2(0.0, bStep)))) / (2.0 * bStep);
		float bermDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 12.0);
		// Centre-masked rather than per-tap: this berm is shading-only, and
		// the mask's job is just to keep the ridge off the dug floor.
		normalWS = normalize(normalWS + float3(-bermGrad * saturate(1.0 - pixelDeform) * bermDepth * BermHeightAmp * BermDepthGate(bermDepth) * TrackBermScale(TrackAgeOf(bermField.w)), 0.0));
	}

	// The surface without the trench detail: the snow maps' frame and march
	// key on it, as on the geometric normal under a normal map.
	const float3 frameNormalWS = normalWS;
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
	// The detail's slopes at the shaded point, scaled with its height: the
	// ground's gradient, so the two agree where they meet.
	[branch] if (detailRelief > 0.001)
	{
		const float2 g = -normalWS.xy / max(normalWS.z, 0.1) + TrenchDetailGrad(trenchTaps, detailLod, detailImpact) * detailRelief;
		normalWS = normalize(float3(-g, 1.0));
	}
#endif
#ifdef SNOW_OBJECT_PRINTS
	// The rim slopes into the print: the dent (-kPrintDentDepth x depth) added
	// to the surface's slope as the patch adds its detail.
	[branch] if (printTop > 0.0 && any(abs(print.yz) > 1e-4))
	{
		const float2 g = -normalWS.xy / max(normalWS.z, 0.1) - kPrintDentDepth * printTop * print.yz;
		normalWS = normalize(float3(-g, 1.0));
	}
	// The dent and the detail shadow the rim at a low sun, as strong as the
	// snow's parallax shadow (Parallax Shadow). Taken before the lighting's
	// state is live, which saves registers.
	float printLit = 1.0;
	[branch] if (printTop > 0.0 && print.x > 0.005 && SnowParallax.y > 0.001)
		printLit = SnowPrints::DentSunShadow(printAt, SharedData::DirLightDirection.xyz, -frameNormalWS.xy / max(frameNormalWS.z, 0.1), kPrintDentDepth,
			SnowParallax.y * printTop * iceParallax, screenNoise);
	[branch] if (detailRelief > 0.05 && TrenchDetailShadow > 0.001 && SnowParallax.y > 0.001)
		printLit *= lerp(1.0, TrenchDetailSunShadow(trenchTaps, detailHitH, SharedData::DirLightDirection.xyz, detailPlane, float3(detailRelief, detailLod, SnowParallax.y * iceParallax), detailImpact, screenNoise),
			min(TrenchDetailShadow, SnowParallax.z * iceParallax));
#endif

	// Tangent basis for the TOP projection's uv axes (see SnowShell.hlsl).
	// Built from the geometric normal before the normal map perturbs it, and
	// shared with the parallax shadow below.
	float3 bumpT = normalize(cross(float3(0.0, 1.0, 0.0), frameNormalWS) + float3(1e-5, 0.0, 0.0));
	float3 bumpB = cross(frameNormalWS, bumpT);

	float3 V = -normalize(input.WorldPos);

	// Pre-parallax derivatives for the glint grid (Lighting.hlsl's uvOriginal
	// pattern; see SnowShell.hlsl): the POM offset is view-dependent and
	// glints must not ride it. The uv itself is rebuilt world-anchored below.
	// Steep pixels take the side plane, like the albedo: the top plane's
	// footprint on a wall is a needle, and the glint NDF then resolves single
	// microfacets per pixel - the sparkle storm on lit walls.
	const bool glintSide = snowSteepness > 0.5;
	const float2 glintDuvdx = glintSide ? snowTapsSide.duvdx : snowTaps.duvdx;
	const float2 glintDuvdy = glintSide ? snowTapsSide.duvdy : snowTaps.duvdy;

	// Parallax occlusion, same marcher the landscape shell uses (shared in
	// SnowParallax.hlsli, so the two cannot drift). Object snow needs it in
	// BOTH projections, and unlike SampleSnowPlanar the two cannot share one
	// march: each projection has its own uv axes, so the view resolves to a
	// different 2D direction in each and the offsets are not interchangeable.
	// Each plane therefore marches itself and shifts its OWN tap set; the
	// existing sample blend then mixes them exactly as before.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.z > 0.001 && bumpFade > 0.001 && input.detail > 0.001 && input.pom > 0.5)
	{
		DisplacementParams pomParams = SnowDisplacementParams();
		pomParams.HeightScale *= SnowParallax.z * iceParallax;

		// Top plane: bumpT/bumpB ARE its uv axes.
		float3x3 tbnTop = float3x3(bumpT, bumpB, frameNormalWS);
		float2 offsetTop = SnowParallaxOffset(snowTaps, snowUV, V, tbnTop, pixelDist, snowHeightMip, screenNoise, pomParams);
		// Faded with steepness, in step with the landscape shell: on a steep
		// side the top TBN follows the surface normal while snowUV stays a
		// top-down projection, and marching that mismatched frame redraws
		// the face whenever the camera changes position.
		offsetTop *= 1.0 - snowSteepness;
		snowUV += offsetTop;
		snowTaps = OffsetSnowTaps(snowTaps, offsetTop);

		// Side plane: raw world axes by construction, matching how
		// snowSidePlane was built. Only steep pixels pay for it. The plane
		// normal is flipped toward the viewer (the old path took abs of the
		// view's N component for the same tolerance).
		[branch] if (snowSteepness > 0.001)
		{
			float3 sideT = snowSideDropsX ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
			float3 sideB = float3(0.0, 0.0, 1.0);
			float3 sideN = normalize(snowSideDropsX ? float3(frameNormalWS.x, 0.0, 0.0) : float3(0.0, frameNormalWS.y, 0.0));
			sideN = dot(V, sideN) < 0.0 ? -sideN : sideN;
			float3x3 tbnSide = float3x3(sideT, sideB, sideN);
			float2 offsetSide = SnowParallaxOffset(snowTapsSide, snowUVSide, V, tbnSide, pixelDist, snowHeightMipSide, screenNoise, pomParams);
			snowUVSide += offsetSide;
			snowTapsSide = OffsetSnowTaps(snowTapsSide, offsetSide);
		}
	}

	// Micro-relief; identical recipe to the terrain shell so ground and
	// object snow carry the same grain: real PBR normal map when available,
	// luminance height-proxy fallback otherwise. Applied after the coverage
	// gate: bending the normal first would jitter the up-facing test into
	// speckled edges.
	[branch] if (HasSnowNormal > 0.5 && bumpFade > 0.001)
		normalWS = normalize(normalWS + bumpFade * SampleSnowNormalOffset(SnowNormalMap, snowTaps, snowTapsSide, snowSteepness, normalWS,
										   bumpT, bumpB, snowSideDropsX));
	else if (HasSnowTexture != 0 && bumpFade > 0.001)
	{
		const float kBumpTile = 64.0;
		const float kBumpHeight = 0.55;
		float2 texDims;
		SnowDiffuse.GetDimensions(texDims.x, texDims.y);
		float e = 1.5 / texDims.x;
		float2 detailUV = worldXY / kBumpTile;
		const float3 kLum = float3(0.30, 0.45, 0.25);
#ifdef SNOW_PATCH_DEFERRED
		const float2 detailDx = gDeferredGridDx / kBumpTile;
		const float2 detailDy = gDeferredGridDy / kBumpTile;
		float h0 = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV, detailDx, detailDy).rgb, kLum);
		float hx = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(e, 0.0), detailDx, detailDy).rgb, kLum);
		float hy = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(0.0, e), detailDx, detailDy).rgb, kLum);
#else
		float h0 = dot(SnowDiffuse.Sample(SnowSampler, detailUV).rgb, kLum);
		float hx = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(e, 0.0)).rgb, kLum);
		float hy = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(0.0, e)).rgb, kLum);
#endif
		float2 bumpGrad = float2(hx - h0, hy - h0) * (kBumpHeight / (e * kBumpTile));
		normalWS = normalize(normalWS + float3(-bumpGrad * bumpFade, 0.0));
	}

#ifdef SNOW_RUNE_GLYPHS
	// A rune's glyph or a frost mark; the landscape shell's recipe.
#ifdef SNOW_PATCH_DEFERRED
	RuneGlyph rune = SampleRunes(worldXY, gDeferredGridDx, gDeferredGridDy, input.markBase);
#else
	RuneGlyph rune = SampleRunes(worldXY, ddx(worldXY), ddy(worldXY), input.markBase);
#endif
	[branch] if (rune.colour.a > 0.004)
	{
		float3 runeNormal = normalize(rune.tangent * rune.normalTS.x + rune.bitangent * rune.normalTS.y + normalWS * rune.normalTS.z);
		normalWS = normalize(lerp(normalWS, runeNormal, rune.colour.a));
	}
#endif
	float3 viewNormal = normalize(mul((float3x3)CameraView, normalWS));

	// Snow material; same albedo path as the terrain shell.
	float3 kSnowAlbedo = float3(0.82, 0.84, 0.88);
	[branch] if (HasSnowTexture != 0)
	{
		kSnowAlbedo = SampleSnowPlanar(SnowDiffuse, snowTaps, snowTapsSide, snowSteepness).rgb;
		kSnowAlbedo = SnowColor::SetAlbedo(kSnowAlbedo, SnowTextureIsLinear != 0.0);
	}
#ifdef SNOW_RUNE_GLYPHS
	[branch] if (rune.colour.a > 0.004)
		kSnowAlbedo = lerp(kSnowAlbedo, rune.colour.rgb, rune.colour.a);
#endif
	// Spell marks on the albedo; the landscape recipes verbatim.
	{
		float scorch = SampleScorch(input.GridLocal) * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
	// Blood stains the landscape shell only (SnowShell.hlsl); on object snow
	// the game's own decal is put back on top by the overlay (Blood.cpp), so
	// a soak here would darken it twice.
#ifdef PATCH
	// Thin-snow and wet print, wet snow and ice, the ground's (SnowWetPrint), through the road layer.
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, pixelDeform, patchMelt, PatchSkinDepth(worldXY).x,
		input.WorldPos.z + ShellCameraPosAdjust.z, input.trenchGridLocal, true);
#elif defined(SNOW_OBJECT_PRINTS)
	// The pressed rim, the ground's thin-snow print in a dusting of the dent's
	// depth. Prints on objects keep no dates.
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, print.x, 0.0, kPrintDentDepth, input.WorldPos.z + ShellCameraPosAdjust.z, 0.0, false);
#else
	// No prints on a coat; wet snow and its sparkle as the ground's.
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, 0.0, 0.0, 100.0, input.WorldPos.z + ShellCameraPosAdjust.z, 0.0, false);
#endif

	// PBR response; identical constants to the terrain shell.
	static const float kSnowRoughness = 0.6;
	static const float3 kSnowF0 = float3(0.028, 0.028, 0.028);

	float snowRoughness = kSnowRoughness;
	float3 snowF0 = kSnowF0;
	float snowAO = 1.0;
	[branch] if (HasSnowRmaos > 0.5)
	{
		float4 rmaos = SampleSnowPlanar(SnowRmaosMap, snowTaps, snowTapsSide, snowSteepness);
		snowRoughness = clamp(rmaos.x * SnowRoughnessScale, 0.05, 1.0);
		snowAO = rmaos.z;
		snowF0 = rmaos.w * SnowSpecularLevel;
	}
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
	// Trench detail: its roughness crank over the mask.
	snowRoughness = clamp(snowRoughness * lerp(1.0, TrenchDetailRoughness, detailMask), 0.05, 1.0);
#endif

	// Wet print: after the RMAOS overwrite, which would discard it.
	snowRoughness *= 1.0 - wetLook.x;

	float3 L = SharedData::DirLightDirection.xyz;
	float satNdotL = saturate(dot(normalWS, L));
	float satNdotV = saturate(abs(dot(normalWS, V)) + 1e-5);

	float worldShadow = ShadowSampling::GetWorldShadow(shadePos, ShellCameraPosAdjust.xyz);
	// Hoisted above the cascade call: the crisp path widens its PCF ring with
	// distance exactly as the terrain shell does, or the far cascade's texels
	// quantise into blocky patches on object snow while the ground beside it
	// shows soft penumbra.
	float farShadowT = smoothstep(6000.0, 15000.0, pixelDist);
	float4 gameShadowMask = float4(-1.0, 0.0, 0.0, 0.0);
	float2 gameMaskDim;
	GameShadowMask.GetDimensions(gameMaskDim.x, gameMaskDim.y);
	[branch] if (input.onObject > 0.5 && gameMaskDim.x > 0.5)
		gameShadowMask = GameShadowMask.Load(int3(input.Position.xy, 0));
	float sunShadow;
	[branch] if (gameShadowMask.x >= 0.0)
	{
		// No bias, no receiver offset: the mask has neither, so a shadow
		// reaches its caster's foot from any sun angle as it does on the
		// object underneath.
		sunShadow = worldShadow * (ShadowSampling::HasDirectionalShadows() ? gameShadowMask.x : 1.0);
	}
	else [branch] if (CrispShadows > 0.5)
	{
		// Full-resolution comparison PCF; same path as the terrain shell.
		// (the round-31 seamShadowLift receiver raise is REVERTED -
		// the RenderDoc replay proved no cascade shadow was missing at the
		// seam, so the lift only risked boundary drift.)
		sunShadow = worldShadow * SnowShadow::GetCascadeShadowReject(shadePos, normalWS, lerp(1.0, 6.0, farShadowT), uint2((uint)BorderStyle.z, (uint)BorderStyle.w), L, input.selfShadowReject);
	}
	else
	{
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(shadePos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}
	// Heightfield self-shadowing, the landscape shell's 5-tap horizon march
	//: hills, berms and drift rims cast the same soft shadows onto
	// object snow as onto the ground beside it, and a trench's own rim
	// darkens its interior. The same baked heights as the landscape march;
	// road tops from the skin's own raster window join the horizon.
	// March diagnostics for debug view 4: x = how much the march darkened
	// this pixel, y = fraction of taps that took the road's carved surface,
	// z unused. Ran = the guard below passed at all (a pixel the cascades
	// already darkened, a sun too low, or a march past both caches never
	// marches - the view paints those dim magenta so "march skipped" cannot
	// be misread as "march found nothing").
	float3 dbgMarch = float3(0.0, 0.0, 0.0);
	float dbgMarchRan = 0.0;
	[branch] if ((ShellFlags.x & 1) != 0 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01 && input.detail > 0.001)
	{
		const float sunLen2D = max(length(L.xy), 1e-4);
		const float sunTan = L.z / sunLen2D;
		const float2 stepDir = L.xy / sunLen2D;
		const float surfZ = shadePos.z + ShellCameraPosAdjust.z;
		// The ground's occluder heights are the landscape march's own, baked
		// (MarchHeightCS): per trail-map texel near, per land-layer vertex past
		// the trail map. A march that leaves both casts nothing.
		const bool nearCache = MarchCacheCovers(input.GridLocal, stepDir, kSunMarchDist[4]);
		[branch] if (nearCache || MarchFarCovers(input.GridLocal, stepDir, kSunMarchDist[4]))
		{
			const float h0 = nearCache ? MarchHeightNear(input.GridLocal) : MarchHeightFarAt(input.GridLocal);
			// Never below the drawn surface by more than the reconstruction's own
			// slack: a skin standing on a rock or a glacier is far above the
			// terrain the taps read, and terrain relief must not shadow it.
			const float refTer = max(h0 > -50000.0 ? h0 : surfZ, surfZ - 2.0);
			// Near softness halved: it existed to hide the tap quantisation the
			// finer first taps now resolve. Far end untouched.
			const float soft = lerp(0.03, 0.35, farShadowT);
			// Object taps exist for ROAD columns only: the road's carved surface
			// is a snow shape the cascades cannot see. Any other object's top is
			// geometry the cascades already shadow with its real silhouette, and
			// a 4-unit raster copy of that shadow never lined up with it (the
			// statue, the distant glaciers). Baked per trail-map texel
			// (RoadMarchHeightCS: the sentinel where a point is no road tap), so
			// near the camera only. ShellFlags.y bit 0: a road wrote road tops
			// into the skin-depth raster this frame.
			bool objectTopUsable = nearCache && HasObjectTop > 0.5 && (ShellFlags.y & 1) != 0;
			// The top raster stores only the HIGHEST surface per texel, so under
			// a multi-level object's overhang it records the deck ABOVE the
			// receiver and every tap reads "inside a hill". Where it stands well
			// above the surface being shaded, drop the object taps and let the
			// cascades/SSS own the shading here. 6 units: about one raster texel
			// above the floor (below ~5 the guard fires on the 4-unit raster's
			// quantisation alone and object tops stop shadowing themselves).
			[branch] if (objectTopUsable)
			{
				float2 selfLocal = (GridOrigin + input.GridLocal - HeightWindowCenter) / HeightHalfExtent;
				[flatten] if (all(abs(selfLocal) < 0.98))
				{
					float2 topDims;
					ObjectTopRaw.GetDimensions(topDims.x, topDims.y);
					float2 selfUV = float2(selfLocal.x * 0.5 + 0.5, 0.5 - selfLocal.y * 0.5);
					float selfTop = ObjectTopRaw.Load(int3((int2)clamp(selfUV * topDims, 0.0, topDims - 1.0), 0));
					if (selfTop > -50000.0 && selfTop > surfZ + 6.0)
						objectTopUsable = false;
				}
			}
			float marchLight;
			[branch] if (nearCache)
			{
				float2 horizon = MarchNearHorizon(input.GridLocal, stepDir, sunTan, refTer);
				// Road taps at the near distances, against the drawn surface.
				[branch] if (objectTopUsable)
				{
					[unroll] for (uint marchI = 0; marchI < 3; marchI++)
					{
						const float d = kSunMarchDist[marchI];
						const float2 sampleLocal = input.GridLocal + stepDir * d;
						const float2 topLocal = (GridOrigin + sampleLocal - HeightWindowCenter) / HeightHalfExtent;
						float2 fRoad;
						const float4 road = MarchCacheTexels(MarchHeightRoad, sampleLocal, fRoad);
						[flatten] if (all(abs(topLocal) < 0.98) && all(road > -50000.0))
						{
							horizon.x = max(horizon.x, (MarchCacheBilinear(road, fRoad) - surfZ - 1.0) / d);
							dbgMarch.y += 0.2;
						}
					}
				}
				marchLight = MarchNearLight(horizon, sunTan, soft, lerp(2.0, 8.0, farShadowT));
			}
			else
			{
				float h[6];
				MarchCachedHeights(input.GridLocal, stepDir, false, h);
				float horizonTan = -10.0;
				[unroll] for (uint marchI = 0; marchI < 5; marchI++)
					horizonTan = max(horizonTan, (h[marchI + 1] - refTer - 1.0) / kSunMarchDist[marchI]);
				marchLight = smoothstep(-1.5 * soft, 1.5 * soft, sunTan - horizonTan);
			}
			// Penumbra CENTRED on the horizon (see SnowShell.hlsl): same width,
			// no bias toward the lit side.
			float marchFactor = lerp(marchLight, 1.0, 0.7 * farShadowT);
			// Fades out toward the horizon, as the terrain march does.
			marchFactor = lerp(1.0, marchFactor, smoothstep(0.04, 0.12, sunTan));
			sunShadow *= marchFactor;
			dbgMarch.x = 1.0 - marchFactor;
			dbgMarchRan = 1.0;
		}
	}	// A mask marched with the object snow in the depth applies as
	// Lighting.hlsl applies it, without the gates below.
	const bool sssSeesSkin = SkinSssSeen > 0.5;
	[branch] if (ScreenSpaceShadowsActive > 0.5 && sssSeesSkin && !SharedData::InInterior && dot(normalWS, L) >= 0.0)
		sunShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);
	// Screen-Space Shadows: same long-range term bare ground multiplies in,
	// distance-blended past the cascades like the landscape shell (the SSS
	// march ran on the PREPASS depth; near, it belongs to the surface
	// UNDER the skin, and the crisp cascades already cover the skin).
	[branch] if (ScreenSpaceShadowsActive > 0.5 && !sssSeesSkin)
	{
		// THE TERRAIN SHELL'S GATES, not a bare distance hand-off. The mask was
		// marched on PRE-shell depth, so it describes the surface UNDER the
		// skin; applied with only the distance term it printed that surface's
		// shadows onto risen snow, which is the shadow that reads as bleeding
		// past an edge. Measured VERTICALLY, not along the view ray: the
		// along-ray gap is depth / sin(elevation) and explodes at far grazing
		// views. A thin coat on a plank hugs its surface and keeps the mask; a
		// dome standing off a rock does not, and drops it.
		float sceneZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
		float shellZ = input.CurrentClip.w;
		float sssRayGap = sceneZ - shellZ;
		float sssVertGap = abs(input.WorldPos.z) * sssRayGap / max(shellZ, 1e-3);
		float sssBlend = (1.0 - smoothstep(8.0, 24.0, sssVertGap)) *
		                 (1.0 - smoothstep(150.0, 400.0, sssRayGap));
		// The terrain shell's buried-caster probe is deliberately NOT ported:
		// its trigger (a captured top within 16 units above the receiver) is
		// calibrated to a shell floating over bare ground, and on a skin the
		// object's OWN raster sits exactly there - it would fire on every
		// shallow skin and kill the mask outright rather than where a caster
		// explains it.
		sssBlend *= SnowShadow::GetSssHandoff(shellZ);
		// The drape IS the surface the mask was marched for: all of it, at
		// every range, as Lighting applies it.
		[flatten] if (gameShadowMask.x >= 0.0)
			sssBlend = SharedData::InInterior ? 0.0 : 1.0;
		sunShadow *= lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0), sssBlend);
	}

	// Parallax self-shadow on the snow grain, same term and constants as the
	// terrain shell so object snow and ground snow shadow identically across
	// the seam where they meet. Object snow needs it in both projections:
	// a rock's flank is exactly where the side plane owns the pixel.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.y > 0.001 && bumpFade > 0.001 &&
		sunShadow > 0.01 && satNdotL > 0.001)
	{
		// Top plane's uv axes are bumpT/bumpB. The side plane is raw world
		// axes by construction, and it only owns near-vertical pixels, where
		// the wall and the projection plane nearly coincide.
		float2 lightUVTop = float2(dot(L, bumpT), dot(L, bumpB));
		float2 lightUVSide = snowSideDropsX ? float2(L.y, L.z) : float2(L.x, L.z);

		float occlusion = SnowParallaxOcclusionPlanar(snowTaps, snowTapsSide, snowSteepness,
			lightUVTop, lightUVSide, snowHeightMip, snowHeightMipSide,
			SnowParallaxQuality(pixelDist), screenNoise, SnowDisplacementParams());

		float parallaxShadow = 1.0 - saturate(occlusion * SnowParallax.y * iceParallax);
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}
#ifdef PATCH
	// The trench detail shadows itself, as on the ground.
	[branch] if (detailRelief > 0.05 && TrenchDetailShadow > 0.001 && SnowParallax.y > 0.001 && sunShadow > 0.01 && satNdotL > 0.001)
		sunShadow *= lerp(1.0, TrenchDetailSunShadow(trenchTaps, detailHitH, L, detailPlane, float3(detailRelief, detailLod, SnowParallax.y * iceParallax), detailImpact, screenNoise),
			min(TrenchDetailShadow, SnowParallax.z * iceParallax));
#endif
#ifdef SNOW_OBJECT_PRINTS
	sunShadow *= printLit;
#endif

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1); same call as the terrain shell so object snow and
	// ground snow shade identically across the seam where they meet.
	// World-anchored glint uv on a static 4096-unit fold; see SnowShell.hlsl
	// for why the GridOrigin-folded snowUV re-rolled the sparkle field.
	const float2 glintUV = glintSide ? (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile :
	                                   fmod(shadePos.xy + ShellCameraPosAdjust.xy, 4096.0) / kSnowUVTile;
	// Built once, shared by the sun and every point light (M3). Glints reach
	// only the direct lobes: the sun's, zero without sunShadow, and the
	// cluster's lights (the same cluster AccumulatePointLights reads below).
	bool glintsLit = sunShadow != 0.0;
	[branch] if (!glintsLit && PointLightsActive > 0.5)
	{
		float glintViewZ = mul(CameraView, float4(input.WorldPos, 1.0)).z;
		float4 glintClip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float2 glintClusterUV = glintClip.xy / max(glintClip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		glintsLit = SnowLights::ClusterLightCount(glintClusterUV, glintViewZ) > 0;
	}
	const float glintsHere = glintsLit ? EnableGlints * wetLook.y : 0.0;
	SnowMaterialCtx snowMtl = SnowBuildMaterial(normalWS, kSnowAlbedo, snowRoughness, snowF0, snowAO,
		SnowGlintParams, glintsHere, glintUV, glintDuvdx, glintDuvdy, input.Position.xy);
#ifdef SNOW_RUNE_GLYPHS
	const float4 markSpec = rune.specular;
#else
	const float4 markSpec = 0.0;
#endif
	SnowSunLighting sunLit = SnowEvaluateSunPBR(snowMtl, normalWS, V, shadePos, ShellCameraPosAdjust.xyz, sunShadow,
		glintUV, glintDuvdx, glintDuvdy, markSpec);
	float3 specularLobe = sunLit.specularLobe;
	float3 diffuseLobe = sunLit.diffuseLobe;
	float3 directDiffuse = sunLit.directDiffuse;
	float3 directSpecular = sunLit.directSpecular;
	float3 markSpecular = sunLit.markSpecular;

	// Placed lights: same clustered path as the terrain shell, with each
	// shadow-casting light's own map sampled at the skin/patch surface.
	[branch] if (PointLightsActive > 0.5)
	{
		float viewZ = mul(CameraView, float4(input.WorldPos, 1.0)).z;
		float4 clip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float2 screenUV = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		SnowLights::AccumulatePointLights(snowMtl, shadePos, shadePos + ShellCameraPosAdjust.xyz,
			normalWS, V, viewZ, screenUV, glintUV, glintDuvdx, glintDuvdy, gameShadowMask, markSpec,
			directDiffuse, directSpecular, markSpecular);
	}

	// No AO here: the routed lobes already carry it (see SnowShell.hlsl).
	float3 ambientColor = SnowAmbientColor(normalWS);
	float3 ambientPart = ambientColor * diffuseLobe;
	// The land's baked vertex AO under the object (see SnowShell.hlsl): snow
	// on a rock in a dark grove shares the grove's baked shade, and using the
	// same source as the terrain shell keeps the seam flat.
	float2 terrainLocal = (shadePos.xy + ShellCameraPosAdjust.xy) - GridOrigin;
	float landVertexAO = ColorManagement::SRGBToWorking(SampleTerrainVertexAO(terrainLocal));
	landVertexAO = lerp(1.0, landVertexAO, SharedData::truePBRSettings.VertexAOStrength);
	// Skylighting parity; same path as the terrain shell.
	[branch] if (SkylightingActive > 0.5)
	{
		sh2 skylightingSH = Skylighting::Sample(shadePos, normalWS);
		float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, shadePos, normalWS, landVertexAO);
		ambientPart = ColorManagement::SceneColor::ScaleByLinear(ambientPart, MultiBounceAO(diffuseLobe * Color::PBRLightingScale, skylightingDiffuse));
	}
	// TruePBR G-buffer units (Lighting.hlsl:2766-2774): diffuse, specular,
	// ambient and the Albedo payload carry PBRLightingScale; the Reflectance
	// lobe does not - the composite assumes exactly this split.
	ambientPart *= Color::PBRLightingScale;
	directDiffuse *= Color::PBRLightingScale;
	directSpecular *= Color::PBRLightingScale;
	diffuseLobe *= Color::PBRLightingScale;
	float3 preLit = ambientPart + directDiffuse;
#ifdef SNOW_RUNE_GLYPHS
	// The mark is a vanilla surface: its own specular and glow in vanilla
	// units, no reflectance and no gloss (SnowShell.hlsl).
	directSpecular = lerp(directSpecular, markSpecular, rune.colour.a);
	specularLobe *= 1.0 - rune.colour.a;
	snowRoughness = lerp(snowRoughness, 1.0, rune.colour.a);
	preLit += rune.emission;
#endif

	// Debug view: decision data as flat colors. Patch: R = trample,
	// G = skin depth (packed by FinishPatchVertex). Skins: teal, brightness
	// by up-facing coverage. Absent pixels = absent geometry.
	SkinShadeResult r;
	r.normalWS = normalWS;
	r.viewNormal = viewNormal;
	r.ambientPart = ambientPart;
	r.diffuseLobe = diffuseLobe;
	r.directSpecular = directSpecular;
	r.specularLobe = specularLobe;
	r.preLit = preLit;
	r.dbgMarch = dbgMarch;
	r.dbgMarchRan = dbgMarchRan;
	r.landVertexAO = landVertexAO;
	r.snowRoughness = snowRoughness;
#if defined(PATCH) || defined(SNOW_OBJECT_PRINTS)
	r.detailMask = detailMask;
#else
	r.detailMask = 0.0;
#endif
	return r;
}

#if defined(VOXEL)
// Trilinear read of the field at a camera-relative position. Logical
// coordinates are clamped, then the WRAP sampler does the torus (the
// rotation keeps adjacency everywhere but the clamped window border).
float VoxelFieldAt(float3 relPos)
{
	float3 logical = (relPos + ShellCameraPosAdjust.xyz) / VoxParams.x - (float3)VoxOrigin.xyz;
	logical = clamp(logical, 0.5, VoxDims.xyz - 0.5);
	float3 uvw = frac((logical + (float3)VoxOrigin.xyz) / VoxDims.xyz);
	return VoxelFieldTex.SampleLevel(VoxelWrapSampler, uvw, 0.0);
}

// The 8-tap trilinear combination with per-axis (ga, gb) weights at (ha, hb).
float VoxelTri8(float3 ga, float3 gb, float3 ha, float3 hb)
{
	float v = 0.0;
	v += ga.x * ga.y * ga.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(ha.x, ha.y, ha.z)), 0.0);
	v += gb.x * ga.y * ga.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(hb.x, ha.y, ha.z)), 0.0);
	v += ga.x * gb.y * ga.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(ha.x, hb.y, ha.z)), 0.0);
	v += gb.x * gb.y * ga.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(hb.x, hb.y, ha.z)), 0.0);
	v += ga.x * ga.y * gb.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(ha.x, ha.y, hb.z)), 0.0);
	v += gb.x * ga.y * gb.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(hb.x, ha.y, hb.z)), 0.0);
	v += ga.x * gb.y * gb.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(ha.x, hb.y, hb.z)), 0.0);
	v += gb.x * gb.y * gb.z * VoxelFieldTex.SampleLevel(VoxelWrapSampler, frac(float3(hb.x, hb.y, hb.z)), 0.0);
	return v;
}

// Cubic value AND gradient (per voxel) at once: the same 8-tap trick with
// the B-spline's derivative weights on one axis at a time (Sigg &
// Hadwiger) - 32 taps for both, where refining by bisection and then
// differencing the normal took 104. The derivative pair sums (dg0 < 0,
// dg1 > 0) never reach zero on [0, 1), so the offset ratios are safe.
float4 VoxelFieldCubicGrad(float3 relPos)
{
	float3 dim = VoxDims.xyz;
	float3 logical = (relPos + ShellCameraPosAdjust.xyz) / VoxParams.x - (float3)VoxOrigin.xyz;
	logical = clamp(logical, 1.5, dim - 1.5);
	float3 x = logical - 0.5;
	float3 i = floor(x);
	float3 f = x - i;
	float3 f2 = f * f;
	float3 f3 = f2 * f;
	float3 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	float3 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	float3 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	float3 w3 = f3 / 6.0;
	float3 g0 = w0 + w1;
	float3 g1 = w2 + w3;
	float3 o = (float3)VoxOrigin.xyz + 0.5;
	float3 h0 = (i - 1.0 + w1 / g0 + o) / dim;
	float3 h1 = (i + 1.0 + w3 / g1 + o) / dim;
	float3 d0 = -0.5 * (1.0 - f) * (1.0 - f);
	float3 d1 = 0.5 * (3.0 * f2 - 4.0 * f);
	float3 d2 = 0.5 * (1.0 + 2.0 * f - 3.0 * f2);
	float3 d3 = 0.5 * f2;
	float3 dg0 = d0 + d1;
	float3 dg1 = d2 + d3;
	float3 dh0 = (i - 1.0 + d1 / dg0 + o) / dim;
	float3 dh1 = (i + 1.0 + d3 / dg1 + o) / dim;
	float4 r;
	r.w = VoxelTri8(g0, g1, h0, h1);
	r.x = VoxelTri8(float3(dg0.x, g0.y, g0.z), float3(dg1.x, g1.y, g1.z), float3(dh0.x, h0.y, h0.z), float3(dh1.x, h1.y, h1.z));
	r.y = VoxelTri8(float3(g0.x, dg0.y, g0.z), float3(g1.x, dg1.y, g1.z), float3(h0.x, dh0.y, h0.z), float3(h1.x, dh1.y, h1.z));
	r.z = VoxelTri8(float3(g0.x, g0.y, dg0.z), float3(g1.x, g1.y, dg1.z), float3(h0.x, h0.y, dh0.z), float3(h1.x, h1.y, dh1.z));
	return r;
}

// Whatever the march hits shades through SkinShadeSurface exactly as a
// skin pixel would, from a synthesised interpolant set: the material is the
// skins' own, so the two layers cannot disagree in colour.
PS_OUTPUT main(VOXEL_VS_OUTPUT input)
{
	const float voxel = VoxParams.x;
	const float brickSize = 8.0 * voxel;
	float tFrag = length(input.WorldPos);
	float3 rayDir = input.WorldPos / max(tFrag, 1e-4);
	float3 dSafe = (abs(rayDir) < 1e-6) ? float3(1e-6, 1e-6, 1e-6) : rayDir;
	float3 invD = 1.0 / dSafe;
	float3 bmin = input.BrickMin;
	float3 bmax = bmin + brickSize;
	float3 t0 = bmin * invD;
	float3 t1 = bmax * invD;
	float tNearRaw = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), min(t0.z, t1.z));
	float tFar = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), max(t0.z, t1.z));
	// The VS sends front faces, or back faces when the camera is inside the
	// brick; either way the ray starts at the entry, or at the eye.
	float tNear = max(tNearRaw, 0.0);
	// Nothing behind what is already drawn.
	float sceneZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	float rayViewZ = mul(CameraView, float4(rayDir, 0.0)).z;
	float tScene = sceneZ / max(rayViewZ, 1e-4);
	tFar = min(tFar, tScene);
	[branch] if (tFar <= tNear)
		discard;

	float noise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	const float threshold = VoxParams.z;
	const float stepLen = max(VoxParams.w, 0.1) * voxel;
	// Entering already inside the snow: the surface lies in an earlier brick.
	[branch] if (VoxelFieldAt(rayDir * tNear) >= threshold)
		discard;
	// SAMPLE ON A GLOBAL LATTICE, never from the brick's own entry point.
	// Phased per brick, two bricks sharing a ray sample the field at
	// different offsets and resolve the surface a step apart - a seam on
	// every brick face, sliding with the camera because the ray does
	// (Josef's moving squares, 2026-09-06). Quantising t to a multiple of
	// the step makes the sample positions identical whichever brick owns
	// them, so the surface is continuous across the boundary.
	// AND SAMPLE ONE LATTICE POINT PAST THE EXIT. Stopping at the exit
	// leaves the last sub-step before every brick face to nobody: this
	// brick breaks before sampling it, the next finds itself already
	// inside and discards - a hole along every brick boundary, the dark
	// lines forming squares. The overshoot lands in the next brick, which
	// is fine: the field is global, the depth is right, and the neighbour
	// discards that pixel as already inside.
	float tPrev = tNear;
	float t = (floor(tNear / stepLen) + 1.0) * stepLen;
	bool hit = false;
	const bool skipEmpty = VoxCentre.w > 0.5;
	const float cell = 4.0 * voxel;
	[loop] for (int i = 0; i < 48; i++)
	{
		if (t > tFar + stepLen)
			break;
		// SUB-CELL SKIP. The brick list marks which of the brick's eight 4^3
		// cells hold a crossing; a cell without one, entered from air, is
		// air throughout, so the ray jumps to the lattice point past its
		// exit without sampling. The lattice stays global, so the surface
		// is still continuous across cells and bricks.
		[branch] if (skipEmpty)
		{
			float3 local = rayDir * t - bmin;
			int3 sc = clamp((int3)floor(local / cell), 0, 1);
			uint bit = 1u << (uint)(sc.x | (sc.y << 1) | (sc.z << 2));
			[branch] if ((input.Mask & bit) == 0u)
			{
				float3 cmin = bmin + (float3)sc * cell;
				float3 tb = (cmin + step(0.0, rayDir) * cell) * invD;
				float tExit = min(tb.x, min(tb.y, tb.z));
				t = (floor(max(tExit, t) / stepLen) + 1.0) * stepLen;
				tPrev = t - stepLen;
				continue;
			}
		}
		if (VoxelFieldAt(rayDir * t) >= threshold) {
			hit = true;
			break;
		}
		tPrev = t;
		t += stepLen;
	}
	[branch] if (!hit)
		discard;
	float a = tPrev;
	float b = t;
	[unroll] for (int k = 0; k < 4; k++)
	{
		float m = 0.5 * (a + b);
		if (VoxelFieldAt(rayDir * m) >= threshold)
			b = m;
		else
			a = m;
	}
	float tHit = 0.5 * (a + b);
	// Refine on the CUBIC reconstruction. The trilinear isosurface creases
	// at every voxel boundary, and at a distance the creases read as
	// blocks; the B-spline's is C2. One evaluation of value and gradient
	// at the trilinear hit, one Newton step along the ray toward the
	// cubic's threshold (clamped to half a voxel: a blob the smoothing
	// flattens below the threshold keeps its trilinear surface rather than
	// vanishing), and the gradient is the normal.
	float3 g;
	{
		float4 vg = VoxelFieldCubicGrad(rayDir * tHit);
		g = vg.xyz / voxel;
		float slope = dot(g, rayDir);
		// Only where the cubic agrees with the trilinear hit. On a far ring
		// the snow is a third of a voxel thick, and the B-spline smooths a
		// layer that thin below the threshold; the Newton step then pushed
		// the hit into the object and the depth test ate it - the lattice
		// of holes on distant roofs (Josef, 2026-09-07). A large correction
		// means the cubic has no surface here; the trilinear one stands.
		[flatten] if (slope > 1e-4)
		{
			float dt = (vg.w - threshold) / slope;
			[flatten] if (abs(dt) <= 0.35 * voxel)
				tHit -= dt;
		}
	}
	float3 P = rayDir * tHit;
	// Clipmap hand-over, dithered: over a level's outer band this level
	// keeps the pixels whose noise is under its weight, and the level
	// outside keeps the rest of the SAME noise - one owner per pixel, so
	// no z-fight, and TAA reads the dither as a blend between the two
	// shapes. The bands are cubes (Chebyshev), as the rings are. The
	// finest level has no inner band; the outermost dithers to nothing.
	// Chebyshev in the window's own shape: z counts for more on a wide ring.
	float3 rIn = abs(P - VoxInnerCentre.xyz) * float3(1.0, 1.0, VoxDims.w);
	float3 rOut = abs(P - VoxCentre.xyz) * float3(1.0, 1.0, VoxDims.w);
	float wIn = 1.0 - smoothstep(VoxFade.x, VoxFade.y, max(rIn.x, max(rIn.y, rIn.z)));
	float wOut = 1.0 - smoothstep(VoxFade.z, VoxFade.w, max(rOut.x, max(rOut.y, rOut.z)));
	// The user's own horizon, the same dither: past it the object shell
	// stands alone, which at that range it does as well.
	wOut = min(wOut, 1.0 - smoothstep(VoxDebug.z, VoxDebug.z + VoxDebug.w, tHit));
	[branch] if (noise >= wOut || noise < wIn)
		discard;

	// The field grows into the snow, so the surface normal is minus its
	// gradient - the cubic's, taken above with the value.
	float3 normalWS = normalize(-g + float3(0.0, 0.0, 1e-5));

	float2 worldXY = P.xy + ShellCameraPosAdjust.xy;
	float4 clip = mul(CameraViewProj, float4(P, 1.0));
	float4 currentClip = mul(CameraViewProjUnjittered, float4(P, 1.0));
	float3 prevRel = P + ShellCameraPosAdjust.xyz - ShellCameraPreviousPosAdjust.xyz;
	float4 previousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
	float2 motionVector = float2(-0.5, 0.5) * (currentClip.xy / currentClip.w - previousClip.xy / previousClip.w);

	SkinShadeInput ssi;
	ssi.WorldPos = P;
	ssi.Position = float4(input.Position.xy, clip.z / clip.w, clip.w);
	ssi.CurrentClip = currentClip;
	ssi.Flat = 0.0;
	ssi.GridLocal = worldXY - GridOrigin;
	ssi.trenchGridLocal = ssi.GridLocal;
	ssi.worldXY = worldXY;
	ssi.pixelDist = tHit;
	ssi.pixelDeform = 0.0;
	ssi.screenNoise = noise;
	ssi.selfShadowReject = 0.0;
	ssi.onObject = 0.0;
	ssi.geoNz = -1.0;
	// Full material inside the detail distance, the range-invisible parts
	// gone by one and a half times it.
	ssi.detail = 1.0 - smoothstep(VoxInnerCentre.w, VoxInnerCentre.w * 1.5, tHit);
	// No parallax relief on fresh snow: its height variation is the field's
	// own and the edge noise's (Josef, 2026-09-07); the glints stay.
	ssi.pom = 0.0;
	// The volume's top stands Volume Snow Depth (ObjectsDepth) over its surface.
	ssi.markBase = P.z + ShellCameraPosAdjust.z - ObjectsDepth;
	SkinShadeResult r = SkinShadeSurface(ssi, normalWS);

	PS_OUTPUT psout;
	// Never under the face it came in through: the conservative-depth promise.
	psout.Depth = max(clip.z / clip.w, input.Position.z);
	// Debug: which ring drew this pixel, as a tint on the lit colour and the
	// albedo - the hand-over bands and any ring-bound artefact then read.
	[branch] if (VoxDebug.y > 0.5)
	{
		static const float3 kRingTint[6] = { float3(0.5, 1.0, 0.5), float3(0.5, 0.6, 1.0), float3(1.0, 1.0, 0.4), float3(1.0, 0.5, 1.0), float3(0.4, 1.0, 1.0), float3(1.0, 0.5, 0.5) };
		float3 tint = kRingTint[clamp((int)VoxDebug.x, 0, 5)];
		r.preLit *= tint;
		r.diffuseLobe *= tint;
	}
	psout.Diffuse = float4(r.preLit, 1.0);
	psout.MotionVectors = float4(motionVector, 0.0, 1.0);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(r.viewNormal), 1.0 - r.snowRoughness, 0.0);
	psout.Albedo = float4(r.diffuseLobe, 1.0);
	psout.Specular = float4(r.directSpecular, 1.0);
	psout.Reflectance = float4(r.specularLobe, 1.0);
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(r.ambientPart).x, 1.0);
	psout.Masks2 = float4(1.0 - r.landVertexAO, 0.0, 0.0, 1.0);
	return psout;
}
#endif

#if !defined(VOXEL) && !defined(PATCH)
float3 DrapeSheetMeet(float3 normalWS, VS_OUTPUT input, bool pdMode, float pixelDist, float3 groundData, float pixelAbsZ)
{
	float sheetMeet = 0.0;
	[branch] if (pdMode && ObjectMeetBand > 0.0 && pixelDist < 2048.0 && groundData.x > -50000.0)
	{
		float sheetDepth;
		float sheetZ = SheetSurfaceZ(input.GridLocal, pixelAbsZ - ObjectMeetBand * 0.5, sheetDepth, true, 0.0, 1.0);
		float sheetGap = pixelAbsZ - sheetZ;
		[branch] if (sheetZ > -50000.0 && sheetGap < ObjectMeetBand * 0.5)
		{
			sheetMeet = (1.0 - smoothstep(0.0, ObjectMeetBand * 0.5, sheetGap)) * smoothstep(0.35, 0.6, normalize(input.NormalWS).z) *
			            (1.0 - smoothstep(1024.0, 2048.0, pixelDist));
			const float mStep = 4.0;
			float3 mXP = SampleTerrainStatics(input.GridLocal + float2(mStep, 0.0));
			float3 mXN = SampleTerrainStatics(input.GridLocal - float2(mStep, 0.0));
			float3 mYP = SampleTerrainStatics(input.GridLocal + float2(0.0, mStep));
			float3 mYN = SampleTerrainStatics(input.GridLocal - float2(0.0, mStep));
			[flatten] if (min(min(mXP.x, mXN.x), min(mYP.x, mYN.x)) > -50000.0)
			{
				float3 sheetN = normalize(float3(-((mXP.x + max(mXP.y, 0.0)) - (mXN.x + max(mXN.y, 0.0))) / (2.0 * mStep),
					-((mYP.x + max(mYP.y, 0.0)) - (mYN.x + max(mYN.y, 0.0))) / (2.0 * mStep), 1.0));
				normalWS = normalize(lerp(normalWS, sheetN, 0.5 * sheetMeet));
			}
		}
	}
	return normalWS;
}
#endif

#if !defined(VOXEL) && !defined(SNOW_PATCH_DEFERRED)
#ifdef SNOW_STATICS_DEPTH_PREPASS
// Every lane of the quad lies behind the depth the private buffer started
// from, even pulled by the most the skin raster state's bias can (r at its
// largest for z <= 1; the non-decal state clamps at 1e-5), so each fails
// LESS_EQUAL. Voting per quad keeps later derivatives valid.
bool SkinQuadHidden(float z, float2 pixel)
{
	float slope = max(abs(ddx_fine(z)), abs(ddy_fine(z)));
	float reach = DecalMode > 0.5 ? (abs(DebugSkinDepth.x) + 1.5) * exp2(-23.0) + (abs(DebugSkinDepth.y) + 0.65) * slope * 1.01 : 1e-5;
	float hidden = (z - reach > PreSkinMainDepth.Load(int3(pixel, 0)) + 2e-7) ? 1.0 : 0.0;
	float hx = ddx_fine(hidden);
	return hidden > 0.5 && hx == 0.0 && ddy_fine(hidden) == 0.0 && ddy_fine(hx) == 0.0;
}
#endif

// Depth prepass (SNOW_STATICS_DEPTH_PREPASS): the alpha cut and nothing else,
// no colour, no export - the hardware writes the raster depth exactly as the
// shipping no-export twin does. Only non-carving draws take it; a carving
// draw's exported depth is a shader-computed value, and two compiles of one
// expression are not promised the same bits, so those draws keep their own
// export and late-Z in the shading loop rather than be matched against a
// prepass copy of themselves.
#ifdef SNOW_STATICS_DEPTH_PREPASS
void main(VS_OUTPUT input)
#elif defined(SNOW_PATCH_ATTR)
PATCH_ATTR_OUTPUT main(VS_OUTPUT input)
#else
PS_OUTPUT main(VS_OUTPUT input)
#endif
{
#ifdef SNOW_STATICS_DEPTH_PREPASS
	// No early-Z under the alpha cut's discard; a hidden quad skips the cut.
	[branch] if (SkinQuadHidden(input.Position.z, input.Position.xy))
	{
		discard;
		return;
	}
#endif
#ifdef SNOW_PATCH_ATTR
	// The deferred pass's only derivatives, taken before any discard.
	const float4 attrGridDeriv = float4(ddx_coarse(input.GridLocal), ddy_coarse(input.GridLocal));
	const float2 attrZDeriv = float2(ddx_coarse(input.WorldPos.z), ddy_coarse(input.WorldPos.z));
#endif
	// Past the range the distance fade below discards every pixel; skip the coat's work.
	[branch] if (FadeExempt < 0.5 && StaticsDebugView == 0.0 && length(input.WorldPos.xy) >= SkinFadeEnd)
		discard;
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	float3 normalWS = normalize(input.NormalWS);
#ifndef PATCH
	// Double-sided cards (thatch, stall canopies) carry a twin triangle
	// facing down at the same depth; its coat shades from below and the
	// later twin wins the depth test, from above and from below alike. No
	// coat forms on a down-facing surface; the rolled rim stays above -0.3.
	[branch] if (normalWS.z < -0.3)
		discard;
#endif

	float2 worldXY = GridOrigin + input.GridLocal;
	float pixelDist = length(input.WorldPos);
	// Shared scope: the seam, down-kill and march gates below read this in
	// BOTH variants. The patch never carries the S4 mode (its draws leave
	// ProjPixelEnable at 0), so it reads false there - but declaring it
	// inside the skin-only region broke the PATCH compile silently (the
	// runtime compiler fails without a build error; the trench patch was
	// simply absent in-game).
	bool pdMode = ProjPixelEnable > 1.5;
	// A large-reference LOD batch is drawn by the game one segment per
	// reference, and no distance rule says which (the large-reference bug
	// keeps segments on inside the loaded grid). Its choice is read from the
	// depth buffer: where it drew the segment the scene depth IS this hull's
	// (same vertices, same raster - bit-identical in the capture); where it
	// did not, the hull stands off whatever is really there. Compared in
	// depth-buffer units: a linearised read is off by the viewport range's
	// 2e-6, a third of a percent of the distance at 20 k units. The band is
	// 1 unit + 0.05% of the distance, converted with dd/dz = fn/((f-n) z^2),
	// floored at ~100 ULPs for the skin's own bias.
	[branch] if (LODBatch > 0.5)
	{
		// Inside the loaded cells the full model draws too, and a LOD that
		// lies within the coincidence band of its own model wins the depth
		// test along every plane cut - a band the width of the LOD triangle
		// across the model's coat (Josef's zigzag streaks, RenderDoc frame
		// 3907). The seam square (SeamBounds) is where the game stops
		// drawing full models.
		// Not every cell of the square draws its models (the outer ring can
		// stand as LOD alone): the pixel's owner decides, x256 = a LOD draw.
		[flatten] if (SeamRampInv > 0.0)
		{
			bool lodOwnsPixel = HasSkinMasksCopy > 0.5 && PreSkinMasks.Load(int3(input.Position.xy, 0)).y >= 256.0;
			[flatten] if (!lodOwnsPixel && all(worldXY > SeamBounds.xy) && all(worldXY < SeamBounds.zw))
				discard;
		}
		float lodZ = input.CurrentClip.w;
		float lodBand = (1.0 + 0.0005 * lodZ) * SharedData::CameraData.w / (SharedData::CameraData.z * lodZ * lodZ);
		float lodSceneD = SceneDepth.Load(int3(input.Position.xy, 0));
		if (abs(lodSceneD - input.Position.z) > max(lodBand, 6e-6))
			discard;
	}
	// Alpha-tested meshes (thatch fringes): the game discarded the
	// transparent texels, so the scene surface there is whatever lies
	// behind; a skin pixel standing off it by more than its own lift is a
	// sheet in the air. Not decal-mode meshes: they draw after the blended
	// depth was final, so it holds the floor under a snow plane, and the
	// plane's coat vanished wherever that floor dipped 6 units.
	else [branch] if (AlphaTested > 0.5 && DecalMode < 0.5)
	{
		float atSceneZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
		float3 atView = normalize(input.WorldPos);
		float atGrazing = max(abs(dot(atView, normalize(input.NormalWS))), 0.25);
		if (atSceneZ - input.CurrentClip.w > (max(input.Lift, 0.0) + 6.0) / atGrazing)
			discard;
	}

	// Rim wall: where a lifted cap reaches back down to the object's edge it is
	// near-vertical at any depth, so the steepness gates below would erase it
	// and the cap would lose its side. The object's top raster separates the
	// cases exactly: shell ABOVE the object's own top is rim wall, at or below
	// it is a bare face. View-independent, and it leaves house walls and
	// boulder flanks to the gates.
	float shoulderWall = 0.0;
	// Gate inputs kept at function scope for the debug view below.
	float wallClearance = 0.0;
	float wallHasData = 0.0;
#	ifndef PATCH
	// An S4 draw zeroes coverageAlpha below and carves only as LegacySkin, so
	// there the rim wall reaches nothing but the debug views.
	[branch] if (HasObjectTop > 0.5 && (!pdMode || LegacySkin > 0.5 || StaticsDebugView != 0.0))
	{
		float objTop = PatchTop(worldXY);
		[flatten] if (objTop > -50000.0)
		{
			// Fraction of the class depth, and the band opens essentially at
			// the object's surface: the rim wall has to reach all the way down
			// to the rock or it hangs with a gap under it. Safe only because
			// the lift is vertical â€” a flank vertex barely rises (upFacing
			// approaches zero there), so it cannot clear the top surface the
			// way normal inflation used to push it out and over.
			float liftBase = max(lerp(RoundedDepth, ObjectsDepth, input.Flat), kMinSkinLift);
			wallClearance = ((input.WorldPos.z + ShellCameraPosAdjust.z) - objTop) / liftBase;
			wallHasData = 1.0;
			shoulderWall = smoothstep(0.0, 0.15, wallClearance);
		}
	}
#	endif

	// Per-pixel up-facing gate from the interpolated raw normal (see the VS
	// note on Coverage): smooth accumulation edges on low-poly meshes.
	// strict gate: nothing steeper than ~66 degrees wears snow. A wide gate
	// matching the drape ramp smears translucent snow onto steep faces
	// (wall fog, boulder-flank sheets, bark streaks; TAA resolves the
	// partial dither into a wet-looking film). The drape's geometry still
	// pins shell edges to the mesh; only the lip's visibility fades here.
	float pixelCoverage = smoothstep(0.4, 0.7, input.Coverage);
#	ifndef PATCH
	// Geometric steepness gate: on huge low-poly triangles the interpolated
	// normal smears one top vertex's up-ness down the whole face, so sloped
	// faces sail over the vertical sliver cull below. The derivative normal
	// knows each pixel's true facing, and snow sheds past ~65 degrees. The band
	// sits below the interpolated gate's range, so rounded edges stay
	// interpolation-shaped. The PATCH is exempt - its walls are legitimately
	// steep real geometry.
	float3 dPosX = ddx(input.WorldPos);
	float3 dPosY = ddy(input.WorldPos);
	float3 geoFacing = normalize(cross(dPosY, dPosX));
	// World units spanned by this pixel. Every LOD term below keys off it
	// rather than off camera distance, so they track resolution and FOV.
	float footprint = length(abs(dPosX) + abs(dPosY));
	// Screen derivatives of the SMOOTH projected weight (vertex normal, no
	// noise, no normal map) and of the vertex normal z: the edge lumps turn
	// "how far below the solid weight" into world units past the solid
	// contour with these. Uniform flow; the users sit inside branches.
	// The column's OWN post-shelter depth target, NOT the class slider.
	// Under a roof the target drops to kShelterDust while the slider does
	// not, so a class-scaled rim band came out WIDER than the sheltered
	// shell is thick and dissolved its whole interior; the fwidth term
	// then finished it off at grazing angles. Only the rim band reads this.
	float liftBase = max(input.LiftTarget, kMinSkinLift);
	// The coat reference, likewise capped by the column's own target. Every
	// gate below thresholds the lift against kProjCoatLift; under a roof the
	// lift is clamped to kShelterDust, which is BELOW that constant, so the
	// gates scored a sheltered shell at a fraction of its coverage and the
	// dither turned it into speckle. Unchanged in the open, where the target
	// is the class depth and this saturates at kProjCoatLift.
	float coatRef = min(kProjCoatLift, liftBase);

	// Density mode, PD-carrying draws: the authored factor MULTIPLIES the
	// facing gates - suppressor only, the same principle as the lift. It
	// kills the mountain-flank film (that ring is SPARSE paint, factor 0)
	// while the facing gates keep rendering the partial trim on beams and
	// slopes exactly as with the mode off. Film round 2 REPLACED the gates
	// instead, and full opaque coverage on every positively-painted beam
	// turned vanilla's thin trim into fat snow ropes (gable screenshots,
	// 2026-08-28) - replacement grants, and the authored data may only
	// ever take away.
	// Authored relief (S3 round 3): vanilla's weight rebuilt PER PIXEL -
	// nz from the game's own shaded normal (the pre-shell G-buffer copy,
	// normal maps included: on a low-poly stone wall the interpolated
	// vertex normal is one flat value, and the purple view's per-stone
	// patchwork lives entirely in the normal map), the authored alpha
	// interpolated through TEXCOORD8, and the noise term. This is what
	// makes the footprint match the purple pixel for pixel instead of the
	// round-2 blanket the +0.1 bias painted over whole walls. The noise
	// retires as the local dome thickens (input.Lift is smooth now), so
	// raising depth bridges cracks center-out, fringe last. Noise sampled
	// at the base surface (lift subtracted); gradients computed outside all
	// flow control - implicit-derivative sampling is illegal inside it, and
	// ddx/ddy are gradient ops themselves.
	float3 projWorldPos = input.WorldPos + ShellCameraPosAdjust.xyz;
	projWorldPos.z -= input.Lift;
	float3 projGradX, projGradY;
	Triplanar::ComputeGradients(projWorldPos, ProjNoiseTiling, projGradX, projGradY);
	float pdCoverage = 0.0;
	// The normal z the slope cut reads, the reconstructed projected weight
	// and its fill gate: the edge breakup and the coat key off the same
	// values the gates do, so their edge is the shell's.
	float edgeNz = normalWS.z;
	float edgeW = -1.0;
	float edgeThr = kCoatSolidW;
	[branch] if (pdMode)
	{
		// Fallback for pixels the copy cannot answer (copy missing, or the
		// shell's silhouette overhangs past its object onto sky/ground):
		// the interpolated shading normal.
		// The SMOOTH normal, at every depth. The pre-shell normal copy used
		// to stand in here above depth 0.05 only, so the footprint changed
		// with the depth slider; and that copy holds the projected snow's
		// own flatter normal on painted pixels, which widened the
		// reconstruction past the game's edge. The real paint now comes
		// from the Masks read-back below.
		float nzPix = normalWS.z;
		edgeNz = nzPix;
		// The footprint: vanilla's weight in FULL, noise always included -
		// the coat's pattern IS the purple, at every fill level. Superset
		// margin on the cut: the reconstruction can never be
		// pixel-identical to vanilla's (triplanar weights and the sample
		// position differ slightly) and the purple view tints right at
		// weight zero, so the cut sits a hair below - purple may only ever
		// peek through a genuine reconstruction hole.
		// Plane weights from the smooth normal, not geoFacing: the derivative
		// facing's hard step() mask flips planes on the quads straddling mesh
		// creases, and the noise jump drew a line along every edge.
		float3 triW = Triplanar::GetWeights(normalWS, normalWS);
		float noise = Triplanar::SampleGrad(ProjNoiseMap, SnowSampler, projWorldPos, triW, ProjNoiseTiling, projGradX, projGradY).x;
		float wpix = nzPix * input.ProjFactor - max(ProjThreshold, 0.0) + 0.1 - ProjNoiseScale * noise;
		// Soft borders, take 2 (Josef: a GRADUAL fade, not dither steps).
		// The widened noisy band failed because the noise term owned the
		// fade: wpix oscillates inside the band, so alpha broke into
		// mid-level islands - his "100 -> 50 -> 0 steps". The fade
		// envelope now rides the SMOOTH half of the weight (noise
		// excluded), descending monotonically across the border, while
		// the noisy cut stays narrow and only keeps the edge ragged.
		float wSmooth = nzPix * input.ProjFactor - max(ProjThreshold, 0.0) + 0.1;
		// As the recolor applies it: everything the game paints at all is
		// solid, so the footprint floors above the coat threshold.
		float wFill = wpix > 0.003 ? max(wpix, 0.2) : wpix;
		float wSmoothFill = wSmooth > 0.003 ? max(wSmooth, 0.2) : wSmooth;
		pdCoverage = smoothstep(edgeThr - 0.03, edgeThr, wFill) * smoothstep(edgeThr - 0.18, edgeThr + 0.08, wSmoothFill);
		edgeW = wFill;
		// S4 roll edge: the fillet's geometry reaches h=0 at the rim, and
		// the last sliver would shade coincident with the surface below it
		// - cut the material where the lift drops under the clearance and
		// let the recolored PD carry on underneath (the two systems agree
		// by construction, so the hand-off is a seam of height only).
		pdCoverage *= smoothstep(0.3 * coatRef, coatRef, input.Lift);
		// The game's own paint, read back: where this pixel's own surface is
		// what stands behind it - low lift - the real weight cuts the
		// footprint, the same at every depth.
		[branch] if (HasSkinMasksCopy > 0.5 && input.Lift < 2.0 * coatRef)
		{
			float realEnc = SkinRecolorEnc(SkinMasksY(int2(input.Position.xy)));
			[flatten] if (realEnc >= 1.5)
				pdCoverage *= (saturate(realEnc - 2.0) >= kCoatSolidReal) ? 1.0 : 0.0;
		}
		// THE LIFT FLOOR, over ALL the screen-space gates (Josef's
		// occlusion find, decoded by his coverage-alpha shot): every gate
		// above reads nzPix / wpix from the PRE-SHELL G-buffer - the
		// surface BEHIND the shell on screen, not the shell's own. That
		// reconstruction is right for a hugging coat, but the 3D shell
		// RISES: its pixels overlap benches, walls and posts, the
		// background normal there is vertical, and the gates dissolved
		// the shell exactly where it was supposed to occlude something -
		// discarded fragments write no depth, so it "rendered behind
		// everything". Where the GEOMETRY stands at full lift, the vertex
		// gates already enforced footprint, fill and slope policy on the
		// shell's OWN surface; the per-pixel reconstruction only owns the
		// edges, where the fillet carries the lift down through this
		// floor's band on its way to zero.
		// EITHER form engages the floor, whichever fires first.
		//
		// The ABSOLUTE band exists because P3's sheltering lowers a
		// shell's TARGET: a half-height sheltered shell read as "edge"
		// forever under a fraction-of-class rule, the pre-shell gates
		// then owned whole under-roof areas, and the risen snow rendered
		// behind everything again (Josef's walkway shot).
		//
		// The FRACTIONAL form has to stay beside it, because absolute
		// alone is WEAKER on shallow classes and that regressed them: at
		// Objects Snow Depth 5 the fraction engages at 2.5 units while
		// the absolute band has not started, so shells in that window
		// lost their floor, fell back to the screen-space gates and
		// dithered into scattered holes. Taking the max is the only form
		// that cannot be less permissive than either rule alone.
		float liftFrac = input.Lift / max(input.LiftTarget, kMinSkinLift);
		pdCoverage = max(pdCoverage, max(
										smoothstep(0.5, 0.85, liftFrac),
										smoothstep(1.5 * coatRef, 4.0 * coatRef, input.Lift)));
	}
	else [flatten] if (ProjThreshold > -0.9)
		pixelCoverage *= smoothstep(0.06, 0.14, input.ProjFactor);
	// Drifts: whole mesh coated, no projection weight or vertex alpha cut.
	[flatten] if (FullCoat > 0.5)
		pixelCoverage = 1.0;

	// Coverage follows the layer's own HEIGHT, not the geometric face normal:
	// geoFacing is constant across a triangle, so thresholding it tears every
	// rim into sawtooth. Interpolated lift varies smoothly and the edge taper
	// already drives it to zero on rims, so walls stay bare without a facing
	// test. Narrow band, jittered in world space - a wide ramp dithers into a
	// translucent film, and a clean threshold traces the mesh's own polygons.
	float liftEdge = 0.06 * liftBase * (0.6 + 0.8 * CoverageNoise(worldXY * 3.0));
	// Rim contour LOD: the band's screen width is liftEdge / fwidth(Lift), so
	// it thins below a pixel and averages into the blanket while the taper
	// still has run left. Push the contour inboard to hold roughly a pixel and
	// keep the partial-alpha width FIXED. Capped, since the taper is all the
	// range this field has. NOT scaled by any dial - the facing LOD that once did the
	// far-field facing handover, and sharing it drops the contour below a pixel.
	float liftBand = 0.45 * liftEdge;
	float liftEdgeLOD = min(max(liftEdge, kRimBandPx * fwidth(input.Lift)), kRimBandMax * liftBase);
	float liftCoverage = smoothstep(liftEdgeLOD - liftBand, liftEdgeLOD, input.Lift);
	pixelCoverage *= liftCoverage;

	// Authored relief REPLACES the shape gates on PD draws - facing band,
	// facing LOD and the lift-band contour all yield to the reconstructed
	// per-pixel weight, because the footprint has to be able to reach where
	// they refuse (steep flanks the purple view paints) and its edge is cut
	// by the noise, not by the lift threshold. Sanctioned replacement: the
	// earlier rounds' replacements granted (beam ropes) because the noise
	// term was missing; with it reconstructed, granting exactly what vanilla
	// grants is the point. The rim wall (below) and the trench, road and
	// distance machinery are untouched.
	[flatten] if (pdMode)
		pixelCoverage = pdCoverage;
#	endif
	// Applied after every steepness multiply; the rim wall is exempt from all
	// of them.
	pixelCoverage = max(pixelCoverage, shoulderWall);
	// (The shred rule lived here and was REMOVED: dissolving pleated
	// pixels only inverted the artifact - white slivers became dark
	// triangular holes - which is the proof that a per-pixel rule cannot
	// repair torn geometry. The tear is a vertex-rate disagreement.)

	// Coverage debug: the facing gates' product, and the two seam blends,
	// captured separately so the rim band's owner is readable at a glance.
	float dbgFacing = pixelCoverage;
	float dbgSeam = 1.0;

	// Per-pixel trench relief: the vertex layer stays uncarved (cliff edges
	// measure 20-160 units; no vertex ever lands inside a trail), so the
	// trench is traced per pixel: parallax-march the view ray down into the
	// deformation heightfield and shade from where the ray actually hits
	// the carved surface, then write that hit's real depth (SV_Depth) so
	// feet and props z-test into the trench. PATCH pixels have real carved
	// geometry and a VS gradient normal; neither applies there.
	float pixelDeform = saturate(SampleDeformation(input.GridLocal));
	// Only roads carve; object trenching returns with the volume snow.
#ifdef PATCH
	// The patch only has texels where the VS already permitted carving, so the
	// per-pixel gate would only re-ask a settled question.
	bool carveObject = true;
#else
	bool carveObject = LegacySkin > 0.5;
#endif
	float2 trenchGridLocal = input.GridLocal;
	float3 viewDirWS = normalize(input.WorldPos);
	// Ray parameter (world units along the view ray) to the parallax hit;
	// 0 means no carve; drives the SV_Depth push at the end.
	float trenchHitS = 0.0;
#	ifndef PATCH
	// Road heightfield: this draw has no snow of its own - the patch carries
	// the whole surface, trampled or not - so the skin leaves outright
	// instead of dithering out over trails. Same two structural gates as the
	// trail hand-off below: up-facing only (the patch cannot represent a kerb
	// face) and inside the patch grid (past 950 there is nothing behind the
	// hole; the range hand-off is S2, ROAD-HEIGHTFIELD-PLAN D).
	// RoadOwnsColumn is the same predicate the patch's carve gate runs, so the
	// skin cannot discard into a column the patch declined - a rock standing
	// on the road takes its column back, and the road's skin has to stay
	// under it or the rock's footprint becomes a hole.
	//
	// No distance gate any more: the patch reaches the object raster's full
	// extent, and RoadOwnsColumn reads that same raster, so it already returns
	// false everywhere the patch cannot draw. One predicate owns the hand-off
	// instead of a radius that had to be kept in step with the grid by hand.
	// No facing gate at all: ownership alone decides. Two rounds of facing
	// tests failed on the baked snow drapes inside RoadChunk*Snow nifs -
	// facet normals (geoFacing) tilt past any threshold on their low-poly
	// crinkles, and road skins run the LEGACY path whose NormalWS is the
	// mesh's own vertex normal, crinkled just the same - so the drape skins
	// kept hovering over carved trenches as paper sheets whose height rode
	// the Road Meshes slider. The gates only ever existed to keep a flank
	// from discarding into a visible gap, and on a road-owned column there
	// is no gap: the patch drapes that column by construction, flanks
	// included. Where ownership cannot answer - beyond the raster window -
	// it already answers "keep the skin", which is the far-field hand-off.
	// Nested: HLSL's && evaluates both sides.
	[branch] if (RoadField > 0.5 && RoundedDepth > 1.0)
	{
		[branch] if (RoadOwnsColumn(worldXY))
			discard;
	}

	// Hand-off to the trench patch: trampled rounded pixels near the camera
	// dissolve out so the patch's real carved geometry beneath shows
	// through. Three gates keep the hand-off airtight:
	// - RoundedDepth > 1: the patch culls sub-1-unit layers, so the skin
	//   must not discard into nothing (vanished floors at shallow depths).
	// - geometrically up-facing pixels only: the top-down patch can never
	//   replace a flank; discarding a log's side pixels (whose map column
	//   carries the trail) punches see-through holes.
	// Far trails and painted-flat trails keep the parallax relief instead.
	[branch] if (carveObject && input.Flat < 0.5 && RoundedDepth > 1.0 && pixelDeform > 0.005 && length(input.WorldPos.xy) < 950.0)
	{
		// Sharp hand-off band: the skin stays FULL height (its carve lives
		// in the patch), so every percent of trample it survives is a
		// full-height ledge overhanging the already-carved patch; hovering
		// rim sheets. Fully gone by 6% trample keeps the ledge under ~2
		// units.
		if (abs(geoFacing.z) > 0.55 && Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount) < smoothstep(0.01, 0.06, pixelDeform))
			discard;
	}

	// Parallax relief for the trails the patch does not cover. flat-class
	// trails never parallax; a raised flat overlay would occlude its own
	// illusion; they keep only the compression darkening.
	[branch] if (carveObject && input.Flat < 0.5 && pixelDeform > 0.001 && pixelCoverage > 0.35)
	{
		float pomDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 25.0);
		[branch] if (pomDepth > 0.5)
		{
			// No minimum floor on object trenches: at full trample the march
			// lands on the object's own surface (the patch sinks there too).
			float carveCap = pomDepth;
			// At depth t below the snow top the ray has drifted by
			// view.xy/-view.z * t in world XY. In air while t < carve(xy);
			// the hit is refined linearly between the straddling samples.
			// The -view.z clamp keeps grazing rays from smearing.
			float invRayZ = 1.0 / max(-viewDirWS.z, 0.25);
			float2 stepXY = viewDirWS.xy * invRayZ;
			float tPrev = 0.0;
			float carvePrev = min(pomDepth * pixelDeform, carveCap);
			[loop] for (uint pomI = 1; pomI <= 8; pomI++) {
				float t = carveCap * float(pomI) / 8.0;
				float2 xy = input.GridLocal + stepXY * t;
				float carve = min(pomDepth * saturate(SampleDeformation(xy)), carveCap);
				[branch] if (t >= carve)
				{
					float w = saturate((carvePrev - tPrev) / max((carvePrev - tPrev) + (t - carve), 1e-4));
					float tHit = lerp(tPrev, t, w);
					trenchGridLocal = input.GridLocal + stepXY * tHit;
					trenchHitS = tHit * invRayZ;
					break;
				}
				tPrev = t;
				carvePrev = carve;
			}
			// Fallback: the ray stayed under the (capped) surface through
			// every sample; land it on the floor.
			[flatten] if (trenchHitS == 0.0 && carvePrev > 0.0)
			{
				trenchGridLocal = input.GridLocal + stepXY * carveCap;
				trenchHitS = carveCap * invRayZ;
			}
			pixelDeform = saturate(SampleDeformation(trenchGridLocal));
		}

		const float step = 4.0;
		float dXP = SampleDeformation(trenchGridLocal + float2(step, 0.0));
		float dXN = SampleDeformation(trenchGridLocal - float2(step, 0.0));
		float dYP = SampleDeformation(trenchGridLocal + float2(0.0, step));
		float dYN = SampleDeformation(trenchGridLocal - float2(0.0, step));
		float2 deformGradient = float2(dXP - dXN, dYP - dYN) / (2.0 * step);

		// Surface drops by depth*deform toward the trench: tilt the normal
		// up the slope so walls shade like real dents. Tilt is capped and
		// softened; full-depth gradients carve black gashes on deep-snow
		// cliffs.
		float pixelDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 12.0) * pixelCoverage;
		normalWS = normalize(normalWS + float3(deformGradient * pixelDepth * 0.6, 0.0));
	}
#	endif

	// Per-pixel coverage: noisy up-facing gate (vanilla-projection-like
	// extent). NO deformation carve in alpha: cutting holes would reveal the
	// bright projected-diffuse beneath; trampling only dents the shading.
	// The noise only MODULATES existing coverage; it must never create
	// snow from nothing, or undersides and walls pick up dithered dabs.
	float coverageGate = saturate(pixelCoverage + (CoverageNoise(worldXY) - 0.5) * 0.3 * saturate(pixelCoverage * 4.0));
	float coverageAlpha = smoothstep(0.05, 0.35, coverageGate);
	// Hard down-facing kill: snow accumulates on TOPS only. The interpolated
	// raw normal is negative on every underside pixel, whatever the noise or
	// seam blends below decide. In pdMode the reconstructed coverage owns
	// steepness policy (and the meld wall is legitimately vertical), so the
	// kill narrows to genuine undersides there.
	float downKill = smoothstep(-0.05, 0.1, input.Coverage);
	[flatten] if (pdMode)
		downKill = smoothstep(-0.08, -0.02, input.Coverage);
	coverageAlpha *= max(downKill, shoulderWall);

	// Height-blended edges (HEIGHT-BLEND-PLAN pairs 6+4): reshape the rim
	// coverage fade and the ground hand-off band below by the snow grain,
	// one-sided vs a fade-swept bar. Shape first; the trench-floor guarantee
	// and floor wear below win exactly as over the plain fades. Mip outside
	// the branches (derivatives); fetches fire only on partial alpha with
	// height blending on and the height map bound. The two sites' fetches
	// are identical expressions and CSE into one.
	float edgeBlend = SnowHeightBlendSharpness(pixelDist);
	// STABLE grid position, not the trench-POM-corrected one: these fetches
	// decide alpha survival, and a cut keyed to a parallax hit swims with
	// the camera (trench walls crawled under camera-only motion
	// once the EM un-gate activated this shaping; the round-16 hLand lesson,
	// same class). Parallax positions are for texture detail only.
	float2 edgeSnowUV = (SnowUVOffset + input.GridLocal) / kSnowUVTile;
	float edgeSnowMip = SnowHeightMip(edgeSnowUV);
	bool edgeBlendOn = HasSnowHeight > 0.5 && edgeBlend > 1.0;
	[branch] if (edgeBlendOn && coverageAlpha > 0.001 && coverageAlpha < 0.999)
	{
		float edgeSnowH = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, worldXY), 0.0.xx, edgeSnowMip);
		coverageAlpha = SnowHeightBlendOneSided(coverageAlpha, edgeSnowH, edgeBlend);
	}

	// Blend into the ground shell: where this pixel sits at or below the
	// terrain shell's snow surface, dissolve so the two shells meet as one
	// blanket. One construction: the analytic band against the terrain
	// window's shell top, which is also the whole story for pair 4's
	// bare-land hand-off. The depth-ray pair that used to sit here (a smooth
	// fade band and the near-field pair-3 geometric contest) was removed with
	// its slider; restoring either needs the post-shell depth copy back
	// first. See HEIGHT-BLEND-PLAN.md pair 3.
	float pixelAbsZ = input.WorldPos.z + ShellCameraPosAdjust.z;
	float seamTotal = 1.0;
	float3 groundData = SampleTerrainStatics(input.GridLocal);
	[flatten] if (groundData.x > -50000.0)
	{
		float groundShellZ = groundData.x + max(groundData.y, 0.0);
		// Pinned band, decoupled from the Border Noise / Border Smoothness
		// sliders. Values are the slider math at exactly 0 / 64.
		const float kSeamBandLow = -36.0;
		const float kSeamBandHigh = 10.0;
		float groundBand = smoothstep(kSeamBandLow, kSeamBandHigh, pixelAbsZ - groundShellZ);
		if (edgeBlendOn && groundBand > 0.001 && groundBand < 0.999)
		{
			float edgeSnowH = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, worldXY), 0.0.xx, edgeSnowMip);
			groundBand = SnowHeightBlendOneSided(groundBand, edgeSnowH, edgeBlend);
		}
		seamTotal = groundBand;
	}
	// S4 shells never dissolve into the ground blanket (Josef's bench
	// find): terrain runs on UNDER buildings, often standing - with its
	// snow depth - ABOVE an elevated deck built into a hillside, so
	// "below the terrain shell's surface" was true for whole roofed
	// porches and the band discarded every fragment there; with no depth
	// written, later draws (the bench) rendered in front of snow that
	// should bury them. The fillet already rolls to zero at silhouettes,
	// which is the meeting this dissolve fakes; true burial belongs to
	// the z-buffer.
	[flatten] if (pdMode)
		seamTotal = 1.0;
	coverageAlpha *= seamTotal;
	dbgSeam *= seamTotal;

	// Shading continuity across the meeting line: with the cut committed, what
	// remains visible is the LIGHTING discontinuity between the skin's macro
	// normal and the blanket's. Ease the skin's normal toward the blanket's
	// analytic surface normal through the last units above the blanket top.
	// Micro detail is continuous by construction - both sides sample the same
	// world-anchored normal map. Keyed to the analytic height field, NOT the
	// ray gate, whose boundary would print its own edge into a normal blend.
	// Blanket depth > 0.5 keeps pair 4's bare-ground hand-off out.
	// Not in pdMode: the ease is the dissolve's companion, and a zero-lift
	// coat neither dissolves nor meets the sheet at the analytic top, so the
	// band printed a flat-lit stripe on the object above the real junction.
	// Skins only: the patch's carved normal must not ease to the uncarved blanket.
#	ifndef PATCH
	[branch] if (!pdMode && HasSnowHeight > 0.5 && groundData.x > -50000.0 && groundData.y > 0.5 && pixelDist < 2048.0)
	{
		float blanketTopZ = groundData.x + max(groundData.y, 0.0);
		float dzTop = pixelAbsZ - blanketTopZ;
		// TWO-SIDED thin band, up-facing pixels only. A one-sided full-strength
		// blend hijacks everything below the blanket top - carved walls, floors
		// and the sun-facing flank at the seam - and flattening those prints a
		// dim stripe against a glowing rim. Flanks and recesses keep their
		// normals.
		float normalBand = smoothstep(-6.0, -2.0, dzTop) * (1.0 - smoothstep(0.5, 6.0, dzTop));
		normalBand *= smoothstep(0.3, 0.6, normalWS.z);
		[branch] if (normalBand > 0.001)
		{
			const float nStep = 4.0;
			float3 gXP = SampleTerrainStatics(input.GridLocal + float2(nStep, 0.0));
			float3 gXN = SampleTerrainStatics(input.GridLocal - float2(nStep, 0.0));
			float3 gYP = SampleTerrainStatics(input.GridLocal + float2(0.0, nStep));
			float3 gYN = SampleTerrainStatics(input.GridLocal - float2(0.0, nStep));
			[flatten] if (min(min(gXP.x, gXN.x), min(gYP.x, gYN.x)) > -50000.0)
			{
				float zXP = gXP.x + max(gXP.y, 0.0);
				float zXN = gXN.x + max(gXN.y, 0.0);
				float zYP = gYP.x + max(gYP.y, 0.0);
				float zYN = gYN.x + max(gYN.y, 0.0);
				float3 blanketN = normalize(float3(-(zXP - zXN) / (2.0 * nStep), -(zYP - zYN) / (2.0 * nStep), 1.0));
				normalWS = normalize(lerp(normalWS, blanketN, normalBand * (1.0 - smoothstep(1024.0, 2048.0, pixelDist))));
			}
		}
	}
#	endif

	// The drape's half of the meeting with the landscape sheet: its normal
	// goes halfway to the sheet's through the last half of ObjectMeetBand above
	// the sheet's DRAWN surface (one-sided: everything at or under that surface
	// holds the full weight, so no stripe can float over the junction). The
	// sheet comes the other half (SnowShell.hlsl ShellObjectMeet).
#	ifndef PATCH
#	ifndef SNOW_STATICS_DEPTH_PREPASS
	normalWS = DrapeSheetMeet(normalWS, input, pdMode, pixelDist, groundData, pixelAbsZ);
#	endif
#	endif

	// Guaranteed snow floor in object trenches; the statics-skin mirror of
	// the landscape shell's trench floor: a carved, solidly-covered pixel
	// must never dissolve to the object's own texture, whatever the seam
	// blends above decided.
	coverageAlpha = max(coverageAlpha, smoothstep(0.15, 0.5, pixelDeform) * smoothstep(0.35, 0.6, pixelCoverage));

#	ifndef PATCH
	// Vertical cull, middle strength: with the drape ramp the connective
	// snow lips are sloped geometry (z well above 0.2) and survive, while
	// truly vertical surfaces die; the interpolation-smeared white veils
	// down house walls and pole sides. (A harder cull cuts gap windows into
	// the drape's skirts; full relaxation lets the veils through.) The
	// PATCH is exempt: its trench walls are steep real geometry.
	coverageAlpha *= max(liftCoverage, shoulderWall);
#	else
	// Silhouette clip, view-independent: the max-of-4 raster placement
	// extends object tops up to a texel past the silhouette, so rim
	// triangles drape into the air as blankets. In the interior all four
	// texel tops agree; within a texel of the edge the bilinear top pulls
	// toward the low/sentinel neighbors, and its drop below the max-based
	// placement marks the overhang. Dissolve on that drop, so the patch
	// ends where the object ends (to raster resolution).
	// PatchSilhouetteDrop is shared with RoadOwnsColumn, which has to decline
	// exactly the columns this dissolves or the skin steps aside into a hole.
	coverageAlpha *= 1.0 - smoothstep(8.0, 24.0, PatchSilhouetteDrop(worldXY));
#	endif

	// Distance dissolve: from SkinFadeStart the skin stochastically thins
	// back into the object's own material, fully gone by SkinFadeEnd (the
	// capture range); distant objects keep their real look instead of
	// turning blank white. Glacier/iceberg captures are exempt: their own
	// baked snow never matches the shell, so the skin persists at every
	// loaded distance.
	// Held SEPARATE from the shape gates: the shape cut below is hard, and a
	// hard cut on a fade that runs over hundreds of units pops every distant
	// skin in one frame.
	float fadeAlpha = 1.0;
	[flatten] if (FadeExempt < 0.5)
		fadeAlpha = 1.0 - smoothstep(SkinFadeStart, SkinFadeEnd, length(input.WorldPos.xy));

#	ifndef PATCH
	// THE COAT AND THE EDGE LUMPS (projected-snow draws whose property
	// really carries projection data, Recolor Projected Snow on). The coat
	// draws the shell's material over the paint the game REALLY applied,
	// read back from the pre-shell Masks copy (Lighting writes its verdict
	// on classified statics), lifted off the object along the view ray for
	// the z-test. Past that paint's edge the coat reaches by WEIGHT: a bare
	// pixel carries the projection's own weight, and Edge Lump Reach sinks
	// the cut into it (kEdgeReachWeightShift at 1). The solid part is never
	// touched. Without the read-back the reconstruction stands in for the
	// coat and nothing reaches.
	float edgeFlankLift = 0.0;
	// Solid coat pixels export a depth pushed toward the camera like the
	// lumps do, kept apart from edgeFlankLift so the near-occluder shadow
	// rejection stays with the lumps. A coat a tenth of a unit above its
	// plank separates from it by that times the cosine of the view angle:
	// nothing at a grazing view, and the plank won by interpolation noise -
	// Josef's shack roofs, bright with vanilla's snow rim light from one
	// angle, correct straight down or close (2026-09-04).
	float coatPush = 0.0;
	// S4 draws have no sheet of their own: every pdMode pixel reads as
	// !inside below and only the coat block, off the game's real paint, can
	// raise it again (the drape). Classic draws keep their own coverage.
	[flatten] if (pdMode)
		coverageAlpha = 0.0;
	// Drifts carry no projected diffuse: nothing for the coat to read back,
	// so with the sheet off nothing can raise coverage again. Whole mesh, any
	// facing - the pixel-side twin of the lift's FullCoat gate.
	[flatten] if (FullCoat > 0.5)
		coverageAlpha = 1.0;
	bool coatOn = pdMode && EdgeCoat > 0.5;
	bool lumpsOn = coatOn && EdgeFlankWidth > 0.001;
	[branch] if (LegacySkin < 0.5 && (coatOn || fadeAlpha < 0.5))
	{
		const float fadeIn = fadeAlpha;
		// Coverage 0.5 is the old cut's own median, so this is the edge.
		bool inside = coverageAlpha >= 0.5;
		bool solid = inside;
		bool needField = inside && fadeIn < 0.5;
		float nearPaint = 0.0;
		[branch] if (!inside && coatOn)
		{
			// Bilinear reads of the copy: under TAA the frame jitters by a
			// fraction of a pixel every frame, and point loads flipped the
			// decisions below while the camera stood still.
			float2 masksDim;
			PreSkinMasks.GetDimensions(masksDim.x, masksDim.y);
			float2 masksUV = input.Position.xy / masksDim;
			// This pixel's OWN texel: the copy and the shell share the screen
			// position and the jitter. Bilinear here blended a plank's 2 + w
			// with the 0 of the gap beside it into an "unpainted" 1.4, and
			// thin planks at an oblique distance lost their coat to the
			// rim-lit projected snow behind them (Josef's shack roofs,
			// 2026-09-04). Where the shell overhangs an unclassified pixel
			// the nearest classified neighbour stands in.
			float realEnc = 0.0;
			float realW = -1.0;
			[branch] if (HasSkinMasksCopy > 0.5)
			{
				int2 mp = int2(input.Position.xy);
				float rawEnc = SkinMasksY(mp);
				realEnc = SkinRecolorEnc(rawEnc);
				realW = SkinRecolorWeight(rawEnc);
				[branch] if (realEnc < 1.5)
				{
					int2 mmax = int2(masksDim) - 1;
					float4 rawN = float4(SkinMasksY(min(mp + int2(1, 0), mmax)),
						SkinMasksY(max(mp - int2(1, 0), 0)),
						SkinMasksY(min(mp + int2(0, 1), mmax)),
						SkinMasksY(max(mp - int2(0, 1), 0)));
					realEnc = max(max(SkinRecolorEnc(rawN.x), SkinRecolorEnc(rawN.y)), max(SkinRecolorEnc(rawN.z), SkinRecolorEnc(rawN.w)));
					realW = max(max(SkinRecolorWeight(rawN.x), SkinRecolorWeight(rawN.y)), max(SkinRecolorWeight(rawN.z), SkinRecolorWeight(rawN.w)));
				}
			}
			bool realKnown = realEnc >= 1.5;
			// LOD hulls coat off the written weight alone: the reconstruction
			// would paint the atlas whole, ships and walls with the mountains.
			// An alpha-tested mesh too: its unknown pixels are the texels the
			// game discarded (a dungeon's snow planes wrote 48 where their
			// alpha passed and nothing elsewhere), and the reconstruction
			// filled the whole plane. A snow-textured shape has no projection
			// to reconstruct.
			bool painted = realKnown ? (saturate(realEnc - 2.0) >= kCoatSolidReal) : (LODBatch < 0.5 && AlphaTested < 0.5 && TexturedSnow < 0.5 && edgeW >= edgeThr);
			// The slope gate on the SMOOTH normal: a bump on a vertical wall
			// faces up per pixel, but the wall does not.
			solid = painted && input.Coverage >= kCoatMinNz;
			[branch] if (!painted && lumpsOn && realKnown && input.Coverage > kCoatMinNz - 0.1)
			{
				// The cut sinks on the game's own weight at this pixel's own
				// texel: the same from every view, the paint itself at reach 0.
				// Steep faces give up the reach, never the paint: the face's own
				// slope (smooth normal) scales the drop, so at reach 0 nothing
				// changes and a wall stays as the game painted it while the
				// roof beside it fills in.
				float reachW = kEdgeReachWeightShift * EdgeFlankWidth;
				// Noise swap: realW's tiling noise term traded for the hex-tiled read,
				// both at the scene point. Each stage is skipped where no read left in
				// [0, 1] can move the cut across [-0.01, 0.02]; unswapped, the verdict
				// is the same there. Inputs must match the prepass: no bent normalWS.
				// No swap where Lighting may have read its noise off the mesh
				// (Snow Follows Parallax): the scene point is not where realW's term came from.
				const float swapW = ParallaxFollow > 0.5 ? 0.0 : ProjNoiseWeight * saturate(EdgeFlankWidth / kDetileRamp);
				const float steepMin = 1.0 - SteepThin;
				[branch] if (swapW > 0.0 && realW > -0.9 && realW + reachW + swapW >= -0.01 && realW + max(reachW - swapW, 0.0) * steepMin <= 0.02)
				{
					const float sceneViewZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
					const float3 sceneP = (input.WorldPos * (sceneViewZ / input.CurrentClip.w) + ShellCameraPosAdjust.xyz) * ProjNoiseTiling;
					const float3 swapN = normalize(input.NormalWS);
					const float3 swapTriW = Triplanar::GetWeights(swapN, swapN);
					const float gameNoise = ProjNoiseGame(sceneP, swapTriW, projGradX, projGradY);
					[branch] if (realW + reachW + swapW * gameNoise >= -0.01 && realW + max(reachW + swapW * (gameNoise - 1.0), 0.0) * steepMin <= 0.02)
						reachW += swapW * (gameNoise - ProjNoiseDetiled(sceneP, swapTriW, projGradX, projGradY));
				}
#ifdef SNOW_STATICS_DEPTH_PREPASS
				// The sheet-meet bend reaches the prepass only through
				// steepKeep, and the cut is monotone in it: evaluate the cut at
				// both ends of its range and bend only when they disagree.
				const float npLo = saturate((realW + max(reachW * (1.0 - SteepThin), 0.0)) * 50.0 + 0.5);
				const float npHi = saturate((realW + max(reachW, 0.0)) * 50.0 + 0.5);
				const float kLo = (npLo - 0.4) * 2.5 - 1.5 * (1.0 - fadeIn);
				const float kHi = (npHi - 0.4) * 2.5 - 1.5 * (1.0 - fadeIn);
				const bool surelyKeep = npLo > 0.15 + 1e-4 && kLo >= 1e-4;
				const bool surelyCut = !(npHi > 0.15 - 1e-4 && kHi >= -1e-4);
				float steepKeep = surelyKeep ? 1.0 : 1.0 - SteepThin;
				[branch] if (!surelyKeep && !surelyCut)
				{
					float3 bentN = DrapeSheetMeet(normalWS, input, pdMode, pixelDist, groundData, pixelAbsZ);
					steepKeep = 1.0 - SteepThin * (1.0 - smoothstep(0.15, 0.5, bentN.z));
				}
#else
				float steepKeep = 1.0 - SteepThin * (1.0 - smoothstep(0.15, 0.5, normalWS.z));
#endif
				nearPaint = saturate((realW + max(reachW * steepKeep, 0.0)) * 50.0 + 0.5);
			}
			needField = solid ? (fadeIn < 0.5) : (nearPaint > 0.15);
		}
		fadeAlpha = 1.0;
		float keep = solid ? 1.0 : -1.0;
		// Solid: only the distance fade (the -0.7 is the retired lump
		// field's neutral 0.5 less the fade's own 1.2). Band: on the
		// contour (half the disc painted) most of it holds and the bays
		// open; a third of the disc keeps cores only; a fifth keeps nothing.
		[branch] if (needField)
		{
			[flatten] if (solid)
				keep = 2.4 * fadeIn - 0.7;
			else
				keep = (nearPaint - 0.4) * 2.5 - 1.5 * (1.0 - fadeIn);
		}
		[flatten] if (inside && keep < 0.0)
			coverageAlpha = 0.0;
		[flatten] if (inside && keep >= 0.0)
			coatPush = kEdgeFlankLift;
		[flatten] if (!inside && keep >= 0.0)
		{
			coverageAlpha = 1.0;
			edgeFlankLift = kEdgeFlankLift;
		}
	}
#	endif

	// Captured before the override: mode 2 renders the value the dither sees.
	float dbgAlpha = coverageAlpha * fadeAlpha;
	// Debug view: full visibility; the dither must not hide geometry the
	// diagnosis needs to see.
	[branch] if (StaticsDebugView != 0.0)
	{
		coverageAlpha = 1.0;
		fadeAlpha = 1.0;
	}
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	// pdMode dithers LINEARLY: the squared reference survives low alphas
	// at sqrt density, which brightened the fade's sparse end into a
	// visible mid-level plateau (part of Josef's "steps"). The classic
	// paths keep their tuned curve.
	// THE SHAPE GATES NO LONGER DITHER (Josef, 2026-09-01: "dithering should
	// be disabled, period"). Partial shape alpha used to resolve to a
	// stochastic discard, which on any surface the gates scored below 1 -
	// every sheltered shell, every rim, every grazing view - showed as
	// see-through speckle rather than as snow. A hard contour instead: the
	// shell ends where coverage crosses half, and everything inside it is
	// OPAQUE. 0.5 is the old dither's own median (its reference is
	// noise^2, so expected coverage was sqrt(alpha)).
	if (coverageAlpha < 0.5)
		discard;
	coverageAlpha = 1.0;
	// The distance fade keeps its dither - it IS a fade, and it is the only
	// gate whose partial alpha spans enough screen area to need one.
	float ditherRef = pdMode ? screenNoise : screenNoise * screenNoise;
	if (ditherRef >= fadeAlpha)
		discard;

#ifdef SNOW_STATICS_DEPTH_PREPASS
	return;
#endif
#ifdef SNOW_PATCH_ATTR
	PATCH_ATTR_OUTPUT attr;
	attr.Grid = float4(input.GridLocal, normalWS.xy);
	attr.Deriv = attrGridDeriv;
	attr.Depth = float4(input.CurrentClip.w, attrZDeriv, 1.0);
	attr.Mark = 1.0;
	return attr;
#endif
#if !defined(SNOW_STATICS_DEPTH_PREPASS) && !defined(SNOW_PATCH_ATTR)
	SkinShadeInput ssi;
	ssi.WorldPos = input.WorldPos;
	ssi.Position = input.Position;
	ssi.CurrentClip = input.CurrentClip;
	ssi.Flat = input.Flat;
	ssi.GridLocal = input.GridLocal;
	ssi.trenchGridLocal = trenchGridLocal;
	ssi.worldXY = worldXY;
	ssi.pixelDist = pixelDist;
	ssi.pixelDeform = pixelDeform;
	ssi.screenNoise = screenNoise;
#	ifndef PATCH
	// Only where this draw still has a raised shell to ignore (roads). The
	// drape has none: at class depth 0 the rejection dropped every contact
	// shadow within 8 units of its caster.
	const float raisedDepth = max(RoundedDepth, ObjectsDepth);
	ssi.selfShadowReject = (edgeFlankLift > 0.0 && raisedDepth > kMinSkinLift) ? raisedDepth + 8.0 : 0.0;
	ssi.onObject = raisedDepth > kMinSkinLift ? 0.0 : 1.0;
#	else
	ssi.selfShadowReject = 0.0;
	ssi.onObject = 0.0;
#	endif
	ssi.detail = 1.0;
	ssi.pom = 1.0;
#ifdef PATCH
	ssi.geoNz = -1.0;
	ssi.markBase = kMarkAnyBase;
#else
	ssi.geoNz = abs(geoFacing.z);
	ssi.markBase = input.WorldPos.z + ShellCameraPosAdjust.z - input.Lift;
#endif
#ifdef SNOW_COAT_PARALLAX
	ssi.surfaceShift = ObjectParallaxShift(input);
#endif
	SkinShadeResult ssr = SkinShadeSurface(ssi, normalWS);
	normalWS = ssr.normalWS;
	float3 viewNormal = ssr.viewNormal;
	float3 ambientPart = ssr.ambientPart;
	float3 diffuseLobe = ssr.diffuseLobe;
	float3 directSpecular = ssr.directSpecular;
	float3 specularLobe = ssr.specularLobe;
	float3 preLit = ssr.preLit;
	float3 dbgMarch = ssr.dbgMarch;
	float dbgMarchRan = ssr.dbgMarchRan;
	float landVertexAO = ssr.landVertexAO;
	float snowRoughness = ssr.snowRoughness;
	[branch] if (StaticsDebugView != 0.0)
	{
#ifdef PATCH
		[branch] if (StaticsDebugView > 7.5)
		{
			// Skin identity / depth fight: skins only. Dim gray.
			preLit = float3(0.1, 0.1, 0.1);
		}
		else [branch] if (StaticsDebugView > 4.5)
		{
			// Projected-mask mode compares skin data; the patch is outside
			// it. Dim gray.
			preLit = float3(0.1, 0.1, 0.1);
		}
		else [branch] if (StaticsDebugView > 3.5)
		{
			// March mode. R = march darkening, G = road-surface taps,
			// B = dusting taps; dim magenta = the march never ran here.
			preLit = dbgMarchRan > 0.5 ? saturate(dbgMarch) : float3(0.15, 0.0, 0.15);
		}
		else
		{
			preLit = float3(saturate(input.Coverage), saturate(input.Flat), 0.0);
		}
#else
		[branch] if (StaticsDebugView > 9.5)
		{
			// Volume seed: the seed pass's verdict at this surface, level 0,
			// re-evaluated the way VoxelSeedCS does. Green = the field holds
			// snow here; red = headroom blocked; blue = facing; yellow = seeded
			// but weak (doors, shelter, sky); magenta = seeded, no field (the
			// sweep or threshold took it); grey = not in the occupancy at all.
			float3 absPos = input.WorldPos + ShellCameraPosAdjust.xyz;
			int3 logical = (int3)floor(absPos / max(DbgVoxelSize, 1e-3)) - DbgOriginVox.xyz;
			int3 dmask = DbgDims.xyz - 1;
			float3 dbgCol = float3(0.12, 0.12, 0.12);
			[branch] if (DbgVoxelSize > 0.0 && all(logical >= 0) && all(logical < DbgDims.xyz))
			{
				uint3 ph = (uint3)((logical + DbgOriginVox.xyz) & dmask);
				// The field first, this voxel and two above: the ground truth for
				// "snow here", whatever the occupancy read says.
				float f = 0.0;
				[unroll] for (int fz = 0; fz <= 2; fz++)
				{
					int3 fv = logical + int3(0, 0, fz);
					[flatten] if (fv.z < DbgDims.z)
						f = max(f, DbgVoxField[(uint3)((fv + DbgOriginVox.xyz) & dmask)]);
				}
				float occ = DbgVoxOcc[ph];
				[flatten] if (occ <= 0.0 && logical.z > 0)
				{
					logical.z -= 1;
					ph = (uint3)((logical + DbgOriginVox.xyz) & dmask);
					occ = DbgVoxOcc[ph];
				}
				[branch] if (f >= DbgFieldThreshold)
					dbgCol = float3(0.1, 1.0, 0.1) * (0.4 + 0.6 * saturate(f));
				else [branch] if (occ <= 0.0)
				{
					// Nothing at this voxel: cyan if the occupancy holds something
					// within a voxel of it (an addressing offset), dark cyan if
					// only elsewhere in this column, light grey if the whole column
					// is empty (a dead read: no captured column is ever empty).
					float near = 0.0;
					[unroll] for (int nz = -1; nz <= 1; nz++)
						[unroll] for (int ny = -1; ny <= 1; ny++)
							[unroll] for (int nx = -1; nx <= 1; nx++)
								near = max(near, DbgVoxOcc[(uint3)((logical + int3(nx, ny, nz) + DbgOriginVox.xyz) & dmask)]);
					float column = 0.0;
					[loop] for (int cz = 0; cz < DbgDims.z; cz++)
						column = max(column, DbgVoxOcc[(uint3)((int3(logical.xy, cz) + DbgOriginVox.xyz) & dmask)]);
					dbgCol = near > 0.0 ? float3(0.1, 0.9, 0.9) : (column > 0.0 ? float3(0.05, 0.4, 0.45) : float3(0.35, 0.35, 0.35));
				}
				else
				{
					{
						bool open = true;
						int headroom = (int)DbgHeadroomVox;
						[loop] for (int k = 1; k <= headroom && open; k++)
						{
							int3 a = logical + int3(0, 0, k);
							open = a.z >= DbgDims.z || DbgVoxOcc[(uint3)((a + DbgOriginVox.xyz) & dmask)] <= 0.0;
						}
						[branch] if (!open)
							dbgCol = float3(1.0, 0.1, 0.1);
						else
						{
							uint2 hb = ph.xy * 4;
							float hmax = -1.0e4;
							uint keys[16];
							[unroll] for (int i = 0; i < 16; i++)
							{
								keys[i] = DbgVoxHeight[hb + uint2(i & 3, i >> 2)];
								[flatten] if (keys[i] != 0u)
									hmax = max(hmax, (asfloat(keys[i] & ~0xFu) - 32768.0) / DbgVoxelSize - (float)DbgOriginVox.z);
							}
							uint q = (uint)round(occ * 255.0);
							float nz = (float)(q >> 4) / 15.0 * 2.0 - 1.0;
							[branch] if (hmax > -1.0e3 && hmax < (float)logical.z + 1.25)
							{
								float nsum = 0.0;
								float nn = 0.0;
								[unroll] for (int j = 0; j < 16; j++)
								{
									[flatten] if (keys[j] != 0u)
									{
										float hz = (asfloat(keys[j] & ~0xFu) - 32768.0) / DbgVoxelSize - (float)DbgOriginVox.z;
										[flatten] if (hz > hmax - 1.0)
										{
											nsum += (float)(keys[j] & 15u) / 15.0 * 2.0 - 1.0;
											nn += 1.0;
										}
									}
								}
								nz = nsum / max(nn, 1.0);
							}
							else
							{
								// Under cover: the layers under the top (VoxelSeedCS).
								uint2 hb2 = ph.xy * 2;
								float h2max = -1.0e4;
								uint keys2[4 * VOXEL_UNDER_LAYERS];
								[unroll] for (int i2 = 0; i2 < 4 * VOXEL_UNDER_LAYERS; i2++)
								{
									keys2[i2] = DbgVoxHeightUnder[uint3(hb2 + uint2(i2 & 1, (i2 >> 1) & 1), i2 >> 2)];
									[flatten] if (keys2[i2] != 0u)
									{
										float hz = (asfloat(keys2[i2] & ~0xFu) - 32768.0) / DbgVoxelSize - (float)DbgOriginVox.z;
										[flatten] if (hz < (float)logical.z + 1.25)
											h2max = max(h2max, hz);
									}
								}
								[branch] if (h2max > (float)logical.z - 1.25)
								{
									float nsum = 0.0;
									float nn = 0.0;
									[unroll] for (int j2 = 0; j2 < 4 * VOXEL_UNDER_LAYERS; j2++)
									{
										[flatten] if (keys2[j2] != 0u)
										{
											float hz = (asfloat(keys2[j2] & ~0xFu) - 32768.0) / DbgVoxelSize - (float)DbgOriginVox.z;
											[flatten] if (hz > h2max - 1.0 && hz < (float)logical.z + 1.25)
											{
												nsum += (float)(keys2[j2] & 15u) / 15.0 * 2.0 - 1.0;
												nn += 1.0;
											}
										}
									}
									nz = nsum / max(nn, 1.0);
								}
							}
							float facing = smoothstep(DbgSlopeMinNz - 0.05, DbgSlopeMinNz + 0.05, nz);
							[branch] if (facing <= 0.0)
								dbgCol = float3(0.2, 0.3, 1.0);
							else
							{
								float2 wxy = ((float2)(logical.xy + DbgOriginVox.xy) + 0.5) * DbgVoxelSize;
								float2 shelter = 0.0;
								float sky = 1.0;
								[branch] if (DbgHeightHalfExtent > 0.0 && max(abs(wxy.x - DbgHeightWindowCenter.x), abs(wxy.y - DbgHeightWindowCenter.y)) < DbgHeightHalfExtent)
								{
									float2 local = (wxy - DbgHeightWindowCenter) / DbgHeightHalfExtent;
									float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
									float2 sd;
									DbgShelter.GetDimensions(sd.x, sd.y);
									shelter = DbgShelter.Load(int3((int2)clamp(uv * sd, 0.0, sd - 1.0), 0));
									float2 kd;
									DbgSky.GetDimensions(kd.x, kd.y);
									sky = DbgSky.Load(int3((int2)clamp(uv * kd, 0.0, kd - 1.0), 0));
								}
								float exposure = lerp(1.0, min(sky, 1.0 - shelter.y), DbgSkyStrength);
								float seed = lerp(DbgShelterDust, 1.0, exposure) * (1.0 - shelter.x) * facing;
								dbgCol = seed < 0.15 ? float3(1.0, 0.85, 0.1) * (0.4 + 4.0 * seed) : float3(1.0, 0.1, 1.0);
							}
						}
					}
				}
			}
			preLit = dbgCol;
		}
		else [branch] if (StaticsDebugView > 8.5)
		{
			// Depth fight. The prepass decided the pixel: private depth below
			// the main depth = a skin won it. The D3D bias formula on the raw
			// SV_Position z says whether it was this skin, and the margin in
			// ULPs how close the fight was. Decal-state skins striped.
			float z = input.Position.z;
			float slope = max(abs(ddx(z)), abs(ddy(z)));
			float ulp = exp2(floor(log2(max(z, 1e-30))) - 23.0);
			bool decal = DebugSkinId < 0.0;
			float bias = decal ? max((-DebugSkinDepth.x - 1.0) * ulp - (DebugSkinDepth.y + 0.65) * slope, -100.0) :
			                     max(-DebugSkinDepth.x * ulp - DebugSkinDepth.y * slope, -1e-5);
			float own = z + bias;
			float mainZ = PreSkinMainDepth.Load(int3(input.Position.xy, 0));
			float wonZ = DebugPrepassDepth.Load(int3(input.Position.xy, 0));
			float margin = mainZ - own;
			bool won = wonZ < mainZ;
			// Two readings of "this skin wrote the prepass": z + bias, or z
			// alone (the PS already sees the biased z). Green / cyan says
			// which one the hardware agrees with.
			bool mineBiased = abs(own - wonZ) < 4.0 * ulp;
			bool mineRaw = abs(z - wonZ) < 4.0 * ulp;
			if (won && !mineBiased && !mineRaw)
				discard;
			if (!won && margin < -4e-4)
				discard;
			float bright = 0.25 + 0.75 * saturate(log2(1.0 + abs(margin) / ulp) / 10.0);
			preLit = (won ? (mineBiased ? float3(0.1, 1.0, 0.1) : float3(0.1, 0.8, 1.0)) : float3(1.0, 0.1, 0.1)) * bright;
			if (decal)
				preLit *= frac((input.Position.x + input.Position.y) / 12.0) < 0.5 ? 1.0 : 0.35;
		}
		else [branch] if (StaticsDebugView > 7.5)
		{
			// Skin identity: hue from the draw ordinal, decal-state skins
			// striped. Bare object under snow = no skin won that pixel.
			float h = frac(abs(DebugSkinId) * 0.618034);
			preLit = saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
			if (DebugSkinId < 0.0)
				preLit *= frac((input.Position.x + input.Position.y) / 12.0) < 0.5 ? 1.0 : 0.35;
		}
		else [branch] if (StaticsDebugView > 4.5)
		{
			// Projected-mask mode. R = the live geometry mask (the authored
			// factor already folded in when a mode is on). G = the authored
			// weight GRADED - brightness is how much snow the data wants,
			// not just its sign: the 5x-amplified G lit thin-trim beams as
			// bright as solid fields and read as "full snow expected here"
			// (Josef's beam report). ZEROED on no-data draws (the +0.1 bias
			// floors at 0.5 otherwise). B = no projected-UV data. Yellow =
			// agree; red-only = we place where vanilla says bare; dim green
			// = vanilla wants a dusting; bright green = vanilla wants full
			// snow we do not place.
			// R since round 10 = the RECONSTRUCTED game blend at this pixel
			// (smoothstep(0,1,5w), solid floor included) - hold it against
			// the Lighting recolor's own weight view (Recolor Weight Tint,
			// object snow off) to find where the two disagree.
			bool noProjData = ProjThreshold < -0.5;
			preLit = float3(noProjData ? 0.0 : smoothstep(0.0, 1.0, 5.0 * edgeW), noProjData ? 0.0 : saturate(input.Flat), noProjData ? 1.0 : 0.0);
		}
		else [branch] if (StaticsDebugView > 3.5)
		{
			// March mode, identical encoding to the patch: the march is
			// shared, and a skin pixel beside a patch pixel must be
			// comparable in one screenshot.
			preLit = dbgMarchRan > 0.5 ? saturate(dbgMarch) : float3(0.15, 0.0, 0.15);
		}
		else [branch] if (StaticsDebugView > 2.5)
		{
			// Normals mode. R = smoothed normal z remapped (0.5 = horizontal,
			// 1 = straight up), G = the flat/rounded class. An up-facing
			// surface reading R near 0.5 means the normal data is wrong, not
			// the geometry.
			preLit = float3(saturate(input.Coverage), saturate(input.Flat), 0.0);
		}
		else [branch] if (StaticsDebugView > 1.5)
		{
			// Coverage mode. R = coverageAlpha as the dither sees it, G = the
			// facing gates' product, B = the two seam blends. A rim band that
			// is dark in R but bright in G and B is owned by neither, so the
			// owner is one of the remaining multiplies.
			preLit = float3(dbgAlpha, dbgFacing, dbgSeam);
		}
		else
		{
			// Taper mode. R = height the taper allows as a fraction of class
			// depth (1 where nothing limited it), G = up-facing mask, B = the
			// raster returned no data here.
			preLit = float3(saturate(input.Coverage), saturate(input.Flat), 1.0 - wallHasData);
		}
#endif
	}

	float stochasticBlend = (screenNoise * screenNoise) < coverageAlpha ? 1.0 : 0.0;

	PS_OUTPUT psout;
#	if !defined(PATCH) && !defined(SNOW_STATICS_NO_DEPTH_EXPORT)
	// Depth: unchanged pixels echo the rasterized depth; carved pixels
	// project the parallax hit point through the same (jittered) matrix
	// the VS used, so the trench floor is real to the z-buffer.
	// Exported as a DELTA on the rasterised depth, never as an absolute
	// re-projection: SV_Depth is written raw while the rasteriser's depth is
	// viewport-mapped, and the re-projection also carries its own precision,
	// so an absolute export sat a constant ~3e-5 NDC nearer than the surface
	// it stood on - a third of a unit at 4 m, two thousand units at 450 m,
	// where it put a mountain's shell in front of the mist meant to wrap it
	// (RenderDoc pixel history vs re-rasterised triangles, 2026-09-04). The
	// difference of two re-projections cancels both.
	psout.Depth = input.Position.z;
	float pushS = trenchHitS > 0.0 ? trenchHitS : -max(edgeFlankLift, coatPush);
	[branch] if (pushS != 0.0)
	{
		float4 rawClip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float4 pushClip = mul(CameraViewProj, float4(input.WorldPos + viewDirWS * pushS, 1.0));
		psout.Depth = input.Position.z + (pushClip.z / max(pushClip.w, 1e-4) - rawClip.z / max(rawClip.w, 1e-4));
	}
#	endif
	psout.Diffuse = float4(preLit, coverageAlpha);
	psout.MotionVectors = float4(motionVector, 0.0, coverageAlpha);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(viewNormal), 1.0 - snowRoughness, stochasticBlend);
	psout.Albedo = float4(diffuseLobe, coverageAlpha);
	psout.Specular = float4(directSpecular, coverageAlpha);
	psout.Reflectance = float4(specularLobe, coverageAlpha);
	// Albedo-multiplied, skylit ambient luma; matches Lighting's masksZ.
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(ambientPart).x, coverageAlpha);
	// Stored as 1 - vertexAO, matching Lighting's convention (the composite
	// divides SSGI's AO by it).
	psout.Masks2 = float4(1.0 - landVertexAO, 0.0, 0.0, coverageAlpha);
#	if defined(SNOW_CONDITION_VIEW_RUNTIME) && !defined(PATCH)
	// Debugging Options > Snow Condition, as the ground paints it, on object
	// snow: no trenches there, and the ice view shows the coat's crust (a
	// print's own glaze would cost the print twin registers). The road patch
	// has a view variant of its own.
	[branch] if (SnowClimate2.z > 0.5)
	{
		const float viewZ = input.WorldPos.z + ShellCameraPosAdjust.z;
		psout.Diffuse.rgb = SnowConditionColor(SnowConditionAt(viewZ), SnowClimate2.z, float2(SnowIceLook(0.0, 0.0, viewZ), 0.0)) * 2.0;
		psout.Albedo.rgb = 0.0;
		psout.Specular.rgb = 0.0;
		psout.Reflectance.rgb = 0.0;
	}
#	endif
	return psout;
#endif  // !SNOW_STATICS_DEPTH_PREPASS && !SNOW_PATCH_ATTR
}
#endif  // !VOXEL && !SNOW_PATCH_DEFERRED

#if defined(SNOW_PATCH_DEFERRED) && defined(PATCH)
// The deferred trench patch: one full-screen triangle shades the pixels the
// attribute pass kept, from what it wrote (the ground's deferred targets).
Texture2D<float4> PatchAttrGrid : register(t91);
Texture2D<float4> PatchAttrDeriv : register(t92);
Texture2D<float4> PatchAttrDepth : register(t93);
Texture2D<float> PatchAttrMark : register(t94);

PS_OUTPUT main(float4 pos : SV_Position)
{
	const int3 px = int3(pos.xy, 0);
	if (PatchAttrMark.Load(px) < 0.5)
		discard;
	const float4 depth = PatchAttrDepth.Load(px);
	const float4 grid = PatchAttrGrid.Load(px);
	const float4 deriv = PatchAttrDeriv.Load(px);
	gDeferredGridDx = deriv.xy;
	gDeferredGridDy = deriv.zw;
	gDeferredZDx = depth.y;
	gDeferredZDy = depth.z;
	// The point at view depth depth.x on the view ray through this pixel: rows
	// 0, 1 and 3 of the jittered view-projection solved for it (SnowShell.hlsl
	// SHELL_DEFERRED_REL).
	const float2 ndc = float2(pos.x / TessScreen.y - 1.0, 1.0 - pos.y / TessScreen.z);
	const float3 r0 = CameraViewProj[0].xyz;
	const float3 r1 = CameraViewProj[1].xyz;
	const float3 r3 = CameraViewProj[3].xyz;
	const float3 b = float3(ndc.x * depth.x - CameraViewProj[0].w, ndc.y * depth.x - CameraViewProj[1].w, depth.x - CameraViewProj[3].w);
	const float3 c0 = cross(r1, r3);
	const float3 rel = (b.x * c0 + b.y * cross(r3, r0) + b.z * cross(r0, r1)) / dot(r0, c0);
	const float4 currentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	const float4 previousClip = mul(CameraPreviousViewProjUnjittered, float4(rel + ShellCameraPosAdjust.xyz - ShellCameraPreviousPosAdjust.xyz, 1.0));
	const float2 motionVector = float2(-0.5, 0.5) * (currentClip.xy / currentClip.w - previousClip.xy / previousClip.w);

	SkinShadeInput ssi;
	ssi.WorldPos = rel;
	ssi.Position = float4(pos.xy, 0.0, 1.0);
	ssi.CurrentClip = currentClip;
	ssi.Flat = 0.0;
	ssi.GridLocal = grid.xy;
	ssi.trenchGridLocal = grid.xy;
	ssi.worldXY = GridOrigin + grid.xy;
	ssi.pixelDist = length(rel);
	ssi.pixelDeform = saturate(SampleDeformation(grid.xy));
	ssi.screenNoise = Random::InterleavedGradientNoise(pos.xy, SharedData::FrameCount);
	ssi.selfShadowReject = 0.0;
	ssi.onObject = 0.0;
	ssi.detail = 1.0;
	ssi.pom = 1.0;
	ssi.geoNz = -1.0;
	ssi.markBase = kMarkAnyBase;
	// A heightfield's normal faces up: z from the stored xy.
	const SkinShadeResult ssr = SkinShadeSurface(ssi, float3(grid.zw, sqrt(saturate(1.0 - dot(grid.zw, grid.zw)))));

	PS_OUTPUT psout;
	psout.Diffuse = float4(ssr.preLit, 1.0);
	psout.MotionVectors = float4(motionVector, 0.0, 1.0);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(ssr.viewNormal), 1.0 - ssr.snowRoughness, 1.0);
	psout.Albedo = float4(ssr.diffuseLobe, 1.0);
	psout.Specular = float4(ssr.directSpecular, 1.0);
	psout.Reflectance = float4(ssr.specularLobe, 1.0);
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(ssr.ambientPart).x, 1.0);
	psout.Masks2 = float4(1.0 - ssr.landVertexAO, 0.0, 0.0, 1.0);
#ifdef SNOW_CONDITION_VIEW
	// Debugging Options > Snow Condition, as the ground paints it.
	{
		psout.Diffuse.rgb = SnowConditionColor(SnowConditionAt(rel.z + ShellCameraPosAdjust.z), SnowClimate2.z,
								PatchConditionTrack(grid.xy, GridOrigin + grid.xy, ssi.pixelDeform, rel.z + ShellCameraPosAdjust.z)) * 2.0;
		psout.Albedo.rgb = 0.0;
		psout.Specular.rgb = 0.0;
		psout.Reflectance.rgb = 0.0;
	}
#endif
	[branch] if (TrenchDetailView != 0)
	{
		// Orange where the trench detail tiles apply, over grey shading (the ground's view).
		const float shade = dot(psout.Diffuse.rgb, float3(0.2126, 0.7152, 0.0722));
		psout.Diffuse.rgb = lerp(shade.xxx, float3(1.0, 0.35, 0.08) * (shade * 1.6 + 0.08), ssr.detailMask);
		psout.Albedo.rgb = 0.0;
		psout.Specular.rgb = 0.0;
		psout.Reflectance.rgb = 0.0;
	}
	return psout;
}
#endif
#endif

#if defined(COMPUTESHADER) && defined(SNOW_ROAD_MARCH_CS)
// MarchHeightRoad's builder: the statics march's road tap at each deformation
// texel's centre, in the map's torus; the sentinel where that tap is not a
// road tap. Over the berm bake's tile list, or a band of rows.
RWTexture2D<float> RoadMarchOut : register(u0);
StructuredBuffer<uint> MarchTilesIn : register(t63);
cbuffer MarchCacheCB : register(b2)
{
	uint MarchRowStart;
	uint MarchRowCount;
	uint2 MarchCachePad;
}

// The statics march's road occluder at a point. Inside an object's footprint
// the surface is its top plus the road's snow: the terrain window's class-ramp
// surface does not exist there (marching against it fabricated a slab a class
// depth above every skin). Depth = the column's skin depth, held under the
// angle-of-repose cone and the land's own class depth, then carved, bermed
// and undulated like the ground's. Only where all four texels are real and a
// road owns the column; the sentinel elsewhere.
float RoadTapHeight(float2 sampleLocal)
{
	float sh = -100000.0;
	const float2 topLocal = (GridOrigin + sampleLocal - HeightWindowCenter) / HeightHalfExtent;
	[branch] if (all(abs(topLocal) < 0.98))
	{
		float2 topDims;
		ObjectTopRaw.GetDimensions(topDims.x, topDims.y);
		const float2 tapWorld = GridOrigin + sampleLocal;
		const float2 bt = PatchTexel(tapWorld, topDims);
		const int2 bt0 = (int2)bt;
		const float2 btf = bt - bt0;
		const int2 bt1 = min(bt0 + 1, int2(topDims) - 1);
		const float4 tapTops = float4(
			ObjectTopRaw.Load(int3(bt0.x, bt0.y, 0)), ObjectTopRaw.Load(int3(bt1.x, bt0.y, 0)),
			ObjectTopRaw.Load(int3(bt0.x, bt1.y, 0)), ObjectTopRaw.Load(int3(bt1.x, bt1.y, 0)));
		[branch] if (all(tapTops > -50000.0))
		{
			const float2 sd00 = ObjectSkinDepth.Load(int3(bt0.x, bt0.y, 0));
			const float2 sd10 = ObjectSkinDepth.Load(int3(bt1.x, bt0.y, 0));
			const float2 sd01 = ObjectSkinDepth.Load(int3(bt0.x, bt1.y, 0));
			const float2 sd11 = ObjectSkinDepth.Load(int3(bt1.x, bt1.y, 0));
			const float topSmooth = lerp(lerp(tapTops.x, tapTops.y, btf.x), lerp(tapTops.z, tapTops.w, btf.x), btf.y);
			float depthSmooth = lerp(lerp(sd00.x, sd10.x, btf.x), lerp(sd01.x, sd11.x, btf.x), btf.y);
			const float tapRoadTop = max(max(sd00.y, sd10.y), max(sd01.y, sd11.y));
			[branch] if (tapRoadTop > kNoRoadTop * 0.5 && depthSmooth >= 1.0)
			{
				const float topRoad = (topSmooth - tapRoadTop) >= kRoadOwnsTop ? tapRoadTop : topSmooth;
				const float tapLand = max(SampleTerrainStatics(sampleLocal).y, 0.0);
				const float tapCone = ObjectConeDepth(tapWorld);
				depthSmooth = max(min(tapCone, depthSmooth), min(tapLand, depthSmooth));
				const float tapDeform = SampleDeformation(sampleLocal);
				const float4 tapBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : float4(0.0, tapDeform, TrackAge.x, TrackAge.x);
				const float tapDepth = CarveProfileNoTeeth(tapDeform, tapBerm.y, depthSmooth, TrackCarveLook(TrackAgeOf(tapBerm.z))) +
				                       BermShape(tapBerm.x) * saturate(1.0 - tapDeform) * depthSmooth * BermHeightAmp * BermDepthGate(depthSmooth) * TrackBermScale(TrackAgeOf(tapBerm.w));
				sh = topRoad + tapDepth + Undulation(tapWorld) * saturate(tapDepth / 8.0);
			}
		}
	}
	return sh;
}

void RoadMarchTexel(uint2 phys)
{
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	if (any(float2(phys) >= dims))
		return;
	const float2 logical = float2((int2(phys) - DeformMapOrigin) & (int2(dims) - 1));
	RoadMarchOut[phys] = RoadTapHeight((logical + 0.5) / dims / DeformInvWorldSize - GridToDeformOffset);
}

[numthreads(8, 8, 1)] void RoadMarchTilesCS(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	const uint listIndex = Gid.y * 1024u + Gid.x;
	if (listIndex >= MarchTilesIn[0])
		return;
	const uint packed = MarchTilesIn[1 + listIndex];
	RoadMarchTexel(uint2(packed & 0xFFFFu, packed >> 16u) * 8u + GTid.xy);
}

[numthreads(8, 8, 1)] void RoadMarchRowsCS(uint3 DTid : SV_DispatchThreadID)
{
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	if (DTid.y >= MarchRowCount)
		return;
	RoadMarchTexel(uint2(DTid.x, (MarchRowStart + DTid.y) % (uint)dims.y));
}
#endif