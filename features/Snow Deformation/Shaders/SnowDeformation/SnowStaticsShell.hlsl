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
#	include "ExtendedMaterials/ExtendedMaterials.hlsli"
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
	uint GridDim;
	uint TerrainDim;
	uint ShellDebugData;

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
	float BorderNoise;
	float BorderSmooth;

	float ShellTriHeight;  // was BorderTrampledFade; landscape shell only, unread here
	float BorderUntrampledFade;
	float ShellCullBare;   // was SeamFadeUnused; landscape shell only, unread here
	float SkinFadeStart;  // statics-skin distance dissolve band (units)

	float SkinFadeEnd;
	// Also the enable gate for the object height field (>0 = field bound).
	float ObjectLiftCap;
	float2 ObjectHeightCenter;

	float ObjectHeightHalfExtent;
	// The raw cascade-atlas copy is bound at t22 this frame (else the
	// shader falls back to the blurred VSM path).
	float CrispShadows;
	// Screen-Space Shadows output is bound at t45: the long-range
	// depth-marched shadows that carry distant LOD tree shadows beyond the
	// two cascades.
	float ScreenSpaceShadowsActive;
	// Dune-field amplitude in world units (landscape shell only).
	float UndulationAmp;

	// Multiplier on the dune field's wavelengths (landscape shell only).
	float UndulationScale;
	// How much heavily trampled trench floors dissolve to the object's own
	// texture (0 = solid snow floors).
	float TrenchFloorFade;
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

	float ChurnHeightAmp;
	float ChurnSizeScale;
	// Landscape-shell C3 A/B flags; declared so ShellCB's layout matches.
	float DebugNoFarPad;
	float DebugNoDataMorph;

	float ObjBermHeightAmp;
	float ObjChurnHeightAmp;
	float ObjChurnSizeScale;
	float ObjCrispScaleV;

	float ObjCrispStrengthV;
	// Landscape-shell only; declared so the tail below keeps ShellCB's layout.
	uint ShellLODDebug;
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
	float4 CrustLook;
	float4 CrustLook2;
	// x/y landscape border dials (unused here); zw = sun cascade atlas
	// slices for the crisp shadow path.
	float4 BorderStyle;
	// x = compaction glint suppression (Stage 1); y = shell-surface SSS
	// re-march enable, zw = its DR scale (landscape shell only).
	float4 CompactLook;
	// Stage 3 P5 rim lip / teeth; consumed via CarveProfile in the march.
	float4 RimStyle;

	// Baked undulation window (see SnowShell.hlsl): xy = world centre,
	// z = 1/half-extent, w > 0.5 when the bake is live.
	float4 UndulationFieldWindow;

	// Toroidal deformation-map addressing (see SnowShell.hlsl).
	int2 DeformMapOrigin;
	int2 DeformTorusPad;
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

	// Distance (world units) by which the geometric height has collapsed to
	// zero at the deepest class; the material dissolve runs past it.
	float SkinHeightFadeEnd;

	// >0.5: keep the tuned pre-rework skin behaviour (road and bridge meshes).
	float LegacySkin;

	// Angle of repose (1.0 = 45 degrees); sets the edge taper width.
	float MoundSteepness;

	// >0.5: trenches are carved on this draw. Roads always carve; other
	// objects are gated by the setting until the object trench work lands.
	float ObjectTrenches;

	// Strength of the coverage LOD terms (facing handover, rim contour push).
	// 0 reproduces the pre-LOD gates exactly.
	float SkinDistantBareness;
	// >0.5: skip the SkinFade distance dissolve (glacier/iceberg captures,
	// whose own baked snow never matches the shell). Mirror in
	// SnowDeformation.h StaticsCB.
	float FadeExempt;
	// >0.5: road heightfield on. On skin draws it also means THIS draw is a
	// road-heightfield object, so the skin steps aside for the patch; on the
	// patch's own draw it is the global gate and the per-texel bit comes from
	// the skin-depth raster's y channel. Mirror in SnowDeformation.h.
	float RoadField;
	// Vanilla projected-UV threshold (projectedUVParams.w) for this draw;
	// -1 = no kProjectedUV on the property (or a tree-anim mesh, whose
	// vertex alpha is wind weight). Feeds debug mode 5 and the S2
	// suppressor. Mirror in SnowDeformation.h.
	float ProjThreshold;
	// >0.5: ApplySkinLift multiplies up-facing by the authored
	// projected-snow term. Mirror in SnowDeformation.h.
	float ProjMaskEnable;
	// >0.5: depth scales with the authored density (graded factor replaces
	// the sharp gate). Mirror in SnowDeformation.h.
	float ProjDensityEnable;
	// Class override code: 0 = flat classifier decides, 1 = force ROUNDED
	// (mountain/cliff family - a jagged cliff's split normals score "flat";
	// and EVERY PD draw in authored-relief mode, per Josef's call to retire
	// the statistical classifier there), 2 = force FLAT (plank family, the
	// cornice treatment). Mirror in SnowDeformation.h.
	float ClassOverride;
	// projectedUVParams.x - strength of vanilla's projected-noise term for
	// this draw; 0 without projection data. Mirror in SnowDeformation.h.
	float ProjNoiseScale;
	// Snow Fill, 0..1 (mirror of SettingsGPU::ProjSnowFill, carried here
	// because b6 is not bound to the skin VS/DS): the S4 shell grows only
	// on the fill's angular slice. Took the retired OpaqueCoverage slot.
	// Mirror in SnowDeformation.h.
	float ProjSnowFillSk;
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
	// >0.5: pre-shell copy of the NORMALROUGHNESS target bound at PS t23 -
	// the per-pixel nz for the authored-relief coverage cut comes from the
	// game's own shaded normal (normal maps included), which is where the
	// purple view's per-stone detail lives. Mirror in SnowDeformation.h.
	float HasSkinNormalCopy;
	// cos(max shell slope): minimum normal Z that grows the S4 shell -
	// the up-facing gate, user-tunable. Mirror in SnowDeformation.h.
	float ShellMinNz;
	// Peel tolerance ("Plane Merge Height" knob): surfaces within this
	// z-band of a layer's top belong to that layer's plane. Mirror in
	// SnowHeightCapture.hlsl / SnowDeformation.h.
	float PeelTol;
	// "Ignore Cover Above" (user knob): cover more than this far above a
	// vertex is a separate world - the plane keeps its full uniform
	// height and clips through it instead of deferring to a peeled
	// layer's narrow footprint. Mirror in SnowHeightCapture.hlsl /
	// SnowDeformation.h.
	float OverheadIgnore;
	// "Meld Co-Planar Surfaces" for the skin: >0.5 lets side faces at
	// MELDED boundaries lift, closing the slit between co-planar shells
	// with vertical snow. Mirror in SnowHeightCapture.hlsl /
	// SnowDeformation.h.
	float MeldPlanesSk;

	// "Pile Height Ratio" (the width failsafe): a dome may stand at most
	// this many times the repose height its footprint supports. Mirror in
	// SnowHeightCapture.hlsl / SnowDeformation.h.
	float PileHeightRatio;
	// P3: strength of the sky-exposure depth weighting (Settings::
	// SkyExposurePct / 100). Took a padPile slot; layout unchanged. Mirror
	// in SnowHeightCapture.hlsl / SnowDeformation.h.
	float SkyExposureSk;
	// C0 spike (CONTAINER-SHELL-PLAN): >0.5 = draw the S4 shell as a
	// CONSTANT-height container and find the surface per pixel. Mirror in
	// SnowHeightCapture.hlsl / SnowDeformation.h.
	float ContainerSpike;
	// Which peeled layer THIS patch pass draws (0 = the top surface, 1/2 =
	// the peeled layers under cover). Mirror in SnowHeightCapture.hlsl /
	// SnowDeformation.h.
	float PatchLayer;

	// THE DRAPE PIVOT's A/B: object columns take the full lattice surface the
	// way road columns already do, and the S4 skins do not draw at all.
	float ObjectDrape;
	// P5's cornice lip: how far the rim overhangs the object's silhouette,
	// as a fraction of the class depth.
	float ObjCorniceLip;
	// Snow Breakup: fraction of the class depth taken off as a negative base
	// and handed back through world noise. 0 = the uniform coat.
	float SkinBreakup;
	// Tier 1 seam weld: how far the FLAT class's up-facing gate slides from
	// the per-vertex raw normal to the position-welded one. 0 = today.
	float SkinWeld;
}

Texture2D<float4> DeformationMap : register(t1);

// Toroidal map fetch: logical texel (already clamped by the caller) to
// physical; wrap is a mask (pow2 dim). Load-based on purpose - a hardware
// sampler would bilinear across the physical seam. Mirror of SnowShell.hlsl.
int3 DeformTexel(int2 t, int2 dims)
{
	return int3((t + DeformMapOrigin) & (dims - 1), 0);
}

// Baked berm field (BermFieldCS): the 17-tap disc average of the deformation
// map, at the map's own resolution and addressing.
Texture2D<float> BermFieldMap : register(t14);
// Wide exclusion field + frost crystal patterns; the landscape shell's slots
// (t15-t17) and readers, bound by the skin draw.
Texture2D<float2> ExclusionFieldMap : register(t15);
Texture2D<float4> FrostPatternNormal : register(t16);
Texture2D<float4> FrostPatternDiffuse : register(t17);
// Baked undulation field (UndulationFieldCS): x = amp-free dune height,
// yz = its +-12-unit shading gradient, over UndulationFieldWindow.
Texture2D<float4> UndulationFieldMap : register(t29);
// Vanilla's projected-UV noise map (the BSGraphics default the game's own
// Lighting.hlsl samples for projWeight), bound by the skin draw when
// ProjPixelEnable is set; null and unread otherwise.
Texture2D<float4> ProjNoiseMap : register(t21);
// Pre-shell copy of the NORMALROUGHNESS target (octahedral view-space, same
// encode this PS writes): the scene's per-pixel shaded normal under each
// shell pixel, before any shell overwrote it. Skin PS only.
Texture2D<float4> PreSkinNormals : register(t23);

// The terrain window also reaches the patch VS: the road-verge depth blend
// needs the landscape class depth per vertex.
#if defined(PSHADER) || defined(DOMAINSHADER) || ((defined(VSHADER) || defined(HULLSHADER)) && defined(PATCH))
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

// Shared trench-detail shaping (noise, berm shape/bake tap, churn) - the
// verbatim-identical pieces of both shells live in one file (M8).
#include "SnowDeformation/SnowFields.hlsli"

// Warped band grid, shared with the landscape shell.
#include "SnowDeformation/SnowGrid.hlsli"

// The patch's own band table, run through the shell's walk (WarpAxisT).
// Sized to the OBJECT RASTER, not to the horizon: past HeightHalfExtent there
// is no top surface to drape on, so reach beyond it buys nothing.
//
// 8-unit core out to 1024 - unchanged from the flat grid it replaces, and
// already at the 4-unit raster's resolution - then power-of-two steps out to
// 4224, which covers the raster's 4096 with slack for the centre snap:
//   1024 (128 x 8) | 1152 | 1408 | 1920 | 4224 (18 x 128)
// INVARIANT (see SnowGrid.hlsli): every band start is a multiple of its own
// step and of the origin snap. 1024/16, 1152/32, 1408/64, 1920/128 are all
// exact, and the CPU snaps the patch centre to kPatchSnap.
static const float kPatchBandVerts[kWarpBands] = { 128.0, 8.0, 8.0, 8.0, 18.0 };
static const float kPatchBandMul[kWarpBands] = { 1.0, 2.0, 4.0, 8.0, 16.0 };
static const float kPatchStep = 8.0;
// Quads per axis; mirrored as kPatchGridDim in SnowDeformation.h, which sizes
// the draw. 2 x (128 + 8 + 8 + 8 + 18).
#define kPatchGridDim 340

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
// Snow Breakup's feature size, world units. Larger than the churn grain (16/7)
// so it reads as patches of bare rather than as texture, and comparable to the
// class depth so a bare spot is about as wide as the layer is thick.
static const float kSkinBreakupScale = 24.0;
// Snow Breakup's ramp width in NOISE units, and the share of the surface the
// slider's top end takes to bare. Wide ramp on purpose - see the note at the
// use site; the caster shares this lift and a hard mask makes it cast slivers.
static const float kBreakupSoft = 0.35;
static const float kBreakupMaxBare = 0.18;
// Lift-gradient debug view: full red at this MULTIPLE of the steepest slope
// the shell is designed to have. That reference is the cornice roll, which
// descends the whole class depth across kCorniceRoll world units - a slope of
// depth/kCorniceRoll, i.e. 6.25:1 at depth 25. A flat threshold cannot work:
// set below the roll it lights every legitimate silhouette, and the roll's
// slope legitimately scales with the depth slider while a tear does not.
static const float kPleatOverRoll = 3.0;
static const float kShelterNear = 8.0;
static const float kShelterFar = 32.0;
// Depth a fully sheltered surface keeps. The landscape shell thins under
// roofs to a dusting rather than killing coverage; matching it stops the
// object's own projected snow being exposed where the skin steps aside.
static const float kShelterDust = 1.0;

// Coverage LOD (see the facing-LOD block in the PS). Blend ceiling toward the
// geometric face normal, target screen width of the rim contour in pixels, and
// the hard cap on how far that contour may travel inboard, as a fraction of
// class depth.
// Ceiling set from the in-game tuning pass: past ~0.21 effective blend the
// per-triangle quantization reads as jagged rock and visible facet seams, so
// the slider spans 0-0.34 and its default sits just above the measured best.
static const float kFacingLODMax = 0.34;
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

// ---- Object trench detail (berm shading, churn, crisp grain) ----
// The landscape shell's recipes with the independent Obj* knobs. Berm is
// shading-only on objects: skin topology is the source mesh's (no vertices
// to carry a ridge) and a geometry berm would straddle the patch/skin
// height seam.

float BermFieldTapped(float2 gridLocal)
{
	float b = SampleDeformation(gridLocal);
	[unroll] for (int i = 0; i < 16; i++)
		b += SampleDeformation(gridLocal + kBermTaps[i]);
	return saturate(b / 17.0);
}

float BermField(float2 gridLocal)
{
	float field = 0.0;
	[branch] if (BermBakeActive > 0.5)
		field = BermFieldBaked(gridLocal);
	else
		field = BermFieldTapped(gridLocal);
	return field;
}

float ChurnNoise(float2 worldXY)
{
	return ChurnNoiseScaled(worldXY, ObjChurnSizeScale);
}

// Spell-mark readers (SampleScorch/SampleCrust/SampleMelted), the wide
// exclusion field, kFireMeltFloor, Undulation, CarveProfile and the frost
// pattern all live in SnowFields.hlsli, shared with the
// landscape shell.

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
};

// HULLSHADER included bare (P1, edge-research study): the skin HS reads the
// cone field to size tessellation against rim proximity.
#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER) || defined(HULLSHADER)
// Top-down object top-surface raster. The patch drapes over it (the VS places
// geometry, the PS clips the silhouette overhang); the skin PS uses it to
// separate its own rim wall from a bare object face.
Texture2D<float> ObjectTopRaw : register(t11);
// Cone-transformed snow surface over the same window: the angle of repose
// already applied, so the edge taper is one read instead of a ring walk.
Texture2D<float> ObjectSnowCone : register(t13);
// S4 phase 2 - the PEELED second layer: the highest up-facing surface
// more than the peel tolerance below layer 1 per column, with its own
// cone. A vertex whose height matches layer 2 takes its roll from here,
// so a tread under a railing or a beam under a roof gets ITS OWN
// plane's rims instead of borrowing the plane above (the beam-streak /
// staircase-hole root).
Texture2D<float> ObjectTop2Raw : register(t24);
// P3: per-column sky openness (1 = open sky), baked by ObjectSkyOpenCS at
// half the raster's resolution from the layer-1 tops.
Texture2D<float> ObjectSkyOpen : register(t25);
Texture2D<float> ObjectSnowCone2 : register(t26);
// K=3: the third peeled layer for roof-over-beam-over-floor columns.
Texture2D<float> ObjectTop3Raw : register(t27);
Texture2D<float> ObjectSnowCone3 : register(t28);
// THE AIR TEST: per peeled layer, the lowest surface standing ABOVE that
// layer's top. Sentinel (kHeightMapEmptyBottom) means nothing above at all.
Texture2D<float> ObjectCoverBottom2 : register(t30);
Texture2D<float> ObjectCoverBottom3 : register(t31);
#endif
// Bound to the patch's VS/HS/DS and, so the skin PS can run the SAME
// ownership test the patch does, to the skin PS as well: the skin must step
// aside exactly where the patch draws and nowhere else.
#if defined(PATCH) || defined(PSHADER)
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

#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER) || defined(HULLSHADER)

// One texel of the object height raster in world units; must match
// kHeightMapHalfExtent * 2 / kHeightMapDim (SnowDeformation.h).
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

float PatchTop(float2 worldXY)
{
	// Outside the window: sentinel, never the clamped edge texel. The patch
	// grid never leaves the window, but skin draws reach the full capture
	// range, where a clamped read returns an unrelated object's top.
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return -1000000.0;

	float2 dims;
	ObjectTopRaw.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	return max(max(ObjectTopRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopRaw.Load(int3(t1.x, t0.y, 0))),
		max(ObjectTopRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopRaw.Load(int3(t1.x, t1.y, 0))));
}

// Snow DEPTH the repose field allows above the local surface, bilinear over
// the window. Outside it there is no data, so nothing is limited.
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

// P3: baked sky openness, bilinear (the map is half the raster's resolution;
// PatchTexel scales by the map's own dims, so nothing here cares). Outside
// the window there is no data - open sky is the no-op.
float SampleSkyOpenness(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return 1.0;

	float2 dims;
	ObjectSkyOpen.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = ObjectSkyOpen.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectSkyOpen.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectSkyOpen.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectSkyOpen.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Layer-2 twins (S4 phase 2). HLSL SM5 cannot parameterize the texture,
// so these mirror PatchTop / ObjectConeDepth verbatim on the peeled maps.
float PatchTop2(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return -1000000.0;

	float2 dims;
	ObjectTop2Raw.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	return max(max(ObjectTop2Raw.Load(int3(t0.x, t0.y, 0)), ObjectTop2Raw.Load(int3(t1.x, t0.y, 0))),
		max(ObjectTop2Raw.Load(int3(t0.x, t1.y, 0)), ObjectTop2Raw.Load(int3(t1.x, t1.y, 0))));
}

float ObjectConeDepth2(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return 1000000.0;

	float2 dims;
	ObjectSnowCone2.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = ObjectSnowCone2.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectSnowCone2.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectSnowCone2.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectSnowCone2.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

float PatchTop3(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return -1000000.0;

	float2 dims;
	ObjectTop3Raw.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	return max(max(ObjectTop3Raw.Load(int3(t0.x, t0.y, 0)), ObjectTop3Raw.Load(int3(t1.x, t0.y, 0))),
		max(ObjectTop3Raw.Load(int3(t0.x, t1.y, 0)), ObjectTop3Raw.Load(int3(t1.x, t1.y, 0))));
}

float ObjectConeDepth3(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return 1000000.0;

	float2 dims;
	ObjectSnowCone3.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = ObjectSnowCone3.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectSnowCone3.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectSnowCone3.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectSnowCone3.Load(int3(t1.x, t1.y, 0));
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// ---- C0 container spike (CONTAINER-SHELL-PLAN) --------------------------
// Vertical extent of the container: the tallest the snow can stand for this
// draw, plus a margin so the ray always starts in clear air above it.
float ContainerHeight()
{
	// The margin (last term) is what the lid keeps between itself and the
	// tallest possible snow. It has to survive INTERPOLATION as well as the
	// field: the lid's height is computed per vertex and interpolated
	// linearly across a triangle, while the field between those vertices is
	// not linear, so a thin margin lets the lid dip under the surface
	// mid-triangle. 8 units is two raster texels.
	return max(max(RoundedDepth, ObjectsDepth), kMinSkinLift) * max(PileHeightRatio, 1.0) + 8.0;
}

// The snow surface as a PURE FUNCTION OF WORLD XY - the object top plus the
// rolling-ball fillet over the repose cone. This is what makes marching
// possible at all: the surface has a closed form the ray can be tested
// against, independent of any mesh.
//
// SPIKE SIMPLIFICATIONS, deliberate: no peeled-layer select, no crest
// freeze, no sky exposure. C0 only has to answer whether the fences die and
// whether the roll reads as a curve; moving the whole field per pixel is
// C1, and doing it here first would confuse a shape difference with an
// architecture difference.
float ContainerSnowZ(float2 worldXY)
{
	float top = PatchTop(worldXY);
	[flatten] if (top < -50000.0)
		return -1000000.0;
	float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
	float rollT = saturate(ObjectConeDepth(worldXY) / coneSeed);
	float toRim = 1.0 - rollT;
	return top + coneSeed * sqrt(saturate(1.0 - toRim * toRim));
}

// NEAREST-texel layer tops, for the lift's layer select only. The
// MAX-of-4 twins above spread a higher neighbour one texel outward,
// which flipped every vertex within a texel of a stair riser onto the
// UPPER tread's layer - a band of confused spikes along each seam.
// The select wants the top of the vertex's own column, nothing wider.
float PatchTopPoint(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return -1000000.0;
	float2 dims;
	ObjectTopRaw.GetDimensions(dims.x, dims.y);
	int2 t = int2(PatchTexel(worldXY, dims) + 0.5);
	return ObjectTopRaw.Load(int3(t, 0));
}

float PatchTop2Point(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return -1000000.0;
	float2 dims;
	ObjectTop2Raw.GetDimensions(dims.x, dims.y);
	int2 t = int2(PatchTexel(worldXY, dims) + 0.5);
	return ObjectTop2Raw.Load(int3(t, 0));
}

float PatchTop3Point(float2 worldXY)
{
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return -1000000.0;
	float2 dims;
	ObjectTop3Raw.GetDimensions(dims.x, dims.y);
	int2 t = int2(PatchTexel(worldXY, dims) + 0.5);
	return ObjectTop3Raw.Load(int3(t, 0));
}
#endif

#if ((defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)) || defined(PSHADER)

// x = layer depth, y = road top. MAX-of-4 on both, matching PatchTop: a
// sentinel neighbour must not drag the column off the road.
float2 PatchSkinDepth(float2 worldXY)
{
	// Outside the window, sentinel - PatchTexel CLAMPS, so without this the
	// read returns an unrelated edge texel. PatchTop guards itself the same
	// way; the two must agree or a comparison between them is meaningless.
	float2 windowLocal = abs(worldXY - HeightWindowCenter);
	if (max(windowLocal.x, windowLocal.y) > HeightHalfExtent)
		return float2(0.0, kNoRoadTop);

	float2 dims;
	ObjectSkinDepth.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	return max(max(ObjectSkinDepth.Load(int3(t0.x, t0.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t0.y, 0))),
		max(ObjectSkinDepth.Load(int3(t0.x, t1.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t1.y, 0))));
}

// How far this column's supporting top stands below the highest of its four
// texels. The patch dissolves on this (its silhouette clip): the max-of-4
// placement extends object tops up to a texel past the real silhouette, and
// the bilinear top's drop below the max marks that overhang.
//
// Shared with RoadOwnsColumn because it is the SECOND reason the patch may
// not draw, and the skin has to know about both.
// Point load from whichever peeled top raster this pass is drawing. Layer 1
// keeps the plain texture, so roads and skins are byte-identical.
float LayerTopLoad(int2 c)
{
	float r = ObjectTopRaw.Load(int3(c.x, c.y, 0));
	[branch] if (PatchLayer > 0.5)
		r = (PatchLayer < 1.5) ? ObjectTop2Raw.Load(int3(c.x, c.y, 0)) :
		                         ObjectTop3Raw.Load(int3(c.x, c.y, 0));
	return r;
}

float PatchSilhouetteDrop(float2 worldXY)
{
	// Domain-warp WHERE the silhouette falls, on the landscape shell's own
	// border recipe (BorderJitter, SnowFields.hlsli). Without it the cut
	// follows the 4-unit raster lattice and a road's end reads as a
	// staircase; with it the edge wanders off the lattice and reads ragged.
	//
	// A quarter of the shell's amplitude: a class border is soft data and can
	// wander tens of units, but an object footprint is real geometry, and a
	// large warp would pull a roof's top into a ground column.
	//
	// Applied HERE, inside the one function the patch's clip and the skin's
	// RoadOwnsColumn both call, so the two cannot disagree about where the
	// boundary is. Jittering them separately is how the holes came back twice.
	worldXY += BorderJitter(worldXY) * 0.25;

	float2 dims;
	ObjectTopRaw.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 c0 = (int2)t;
	float2 cf = t - c0;
	int2 c1 = min(c0 + 1, int2(dims) - 1);
	// The drop must be measured on the layer being DRAWN. Read on layer 1 it
	// describes the roof's silhouette while a layer-2 pass drapes the walkway
	// beneath, so the dissolve fires at the roof's edge and spares the real
	// one. Same law as the neighbour tests and the depth authority.
	float top00 = LayerTopLoad(int2(c0.x, c0.y));
	float top10 = LayerTopLoad(int2(c1.x, c0.y));
	float top01 = LayerTopLoad(int2(c0.x, c1.y));
	float top11 = LayerTopLoad(int2(c1.x, c1.y));
	float maxTop = max(max(top00, top10), max(top01, top11));
	float4 drops = min(maxTop - float4(top00, top10, top01, top11), 200.0);
	return lerp(lerp(drops.x, drops.y, cf.x), lerp(drops.z, drops.w, cf.x), cf.y);
}

// Does the road own this column? The single predicate the patch's carve gate
// and the skin's step-aside both run, so the skin can never discard into a
// column the patch declined.
bool RoadOwnsColumn(float2 worldXY)
{
	// The top MUST be real. A sentinel top (outside the window, or no object
	// captured here) makes top - roadTop hugely negative, which passes the
	// height test for free - and the skin then steps aside for a patch that
	// cannot draw, stripping distant roads of snow entirely.
	float top = PatchTop(worldXY);
	if (top < -50000.0)
		return false;
	float roadTop = PatchSkinDepth(worldXY).y;
	if (roadTop <= kNoRoadTop * 0.5 || (top - roadTop) >= kRoadOwnsTop)
		return false;
	// The silhouette clip is the other way the patch declines a column it
	// otherwise owns, and it fires on the road's own edge texels - where the
	// road IS the supporting top, so the ownership test above passes happily.
	// Stepping aside there left a hole straight through to the road mesh.
	// 8.0 is where the clip's smoothstep starts biting; below it the patch is
	// at full coverage. In the 8-24 band both draw and the patch wins on top,
	// which is a thin double layer rather than a hole.
	return PatchSilhouetteDrop(worldXY) < 8.0;
}
#endif

#if (defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)

// ---- PER-LAYER DRAPE (CONTAINER-SHELL-PLAN pivot) -----------------------
// A drape is ONE SURFACE PER COLUMN. Roads have exactly one, which is why
// they were the easy case; a walkway under a roof has two, and the lower
// one has to keep its snow. The answer is to draw the lattice ONCE PER
// PEELED LAYER, each pass reading that layer's own top and cone, so the
// walkway gets its own drape instead of being hidden under the roof's.
//
// EVERY raster read in the vertex builder must follow the same layer.
// A partial redirect would compare layer 2's surface against layer 1's
// neighbours: the roof towers 100+ units over the walkway, the tall-ray
// test would fire on every vertex, and the entire under-cover drape would
// be culled before it drew anything.
// SINGLE RETURN, INITIALIZED. These three accessors first shipped with early
// returns inside [branch], which is the one form this file's own patch gate
// documents as unsafe: fxc raised X4000 "potentially uninitialized" on all
// three, and the drape they feed drew nothing for three rounds. fxc exits 0
// with warnings, so a sweep that checks only the exit code passes it.
float PatchTopL(float2 worldXY)
{
	float r = -1e9;
	[branch] if (PatchLayer < 0.5)
		r = PatchTop(worldXY);
	else [branch] if (PatchLayer < 1.5)
		r = PatchTop2(worldXY);
	else
		r = PatchTop3(worldXY);
	return r;
}

float ObjectConeDepthL(float2 worldXY)
{
	float r = 0.0;
	[branch] if (PatchLayer < 0.5)
		r = ObjectConeDepth(worldXY);
	else [branch] if (PatchLayer < 1.5)
		r = ObjectConeDepth2(worldXY);
	else
		r = ObjectConeDepth3(worldXY);
	return r;
}

// Lowest surface standing above this layer's top, nearest texel. Layer 1 has
// nothing above it by definition, so it reports open sky and never gates.
float CoverBottomL(float2 worldXY)
{
	float r = 100000.0;
	[branch] if (PatchLayer > 0.5)
	{
		float2 dims;
		ObjectCoverBottom2.GetDimensions(dims.x, dims.y);
		float2 tc = PatchTexel(worldXY, dims);
		int2 c = int2(clamp(tc, 0.0, dims.x - 1.001));
		// MIN over a 3x3, which DILATES cover exactly as PatchTop's max-of-4
		// dilates tops - lower cover means more covered, so min is the
		// conservative direction. The raster is 4 units while the lattice is
		// finer, so a vertex at a rock's edge can land on a texel the rock's
		// geometry just missed, read nothing-above, and draw: the sliver under
		// the cairn, the line down a post, the patch under a fallen boat. One
		// texel of dilation costs the drape a texel at the edge of genuinely
		// covered ground, which is the cheap direction to be wrong in.
		[unroll] for (int dy = -1; dy <= 1; dy++)
		{
			[unroll] for (int dx = -1; dx <= 1; dx++)
			{
				int2 sc = int2(clamp(c.x + dx, 0, int(dims.x) - 1),
					clamp(c.y + dy, 0, int(dims.y) - 1));
				float cv = (PatchLayer < 1.5) ? ObjectCoverBottom2.Load(int3(sc.x, sc.y, 0)) :
				                                ObjectCoverBottom3.Load(int3(sc.x, sc.y, 0));
				r = min(r, cv);
			}
		}
	}
	return r;
}

// THE PEEL INDEX IS A STACKING ORDINAL, NOT A HEIGHT STRATUM. One column's
// layer 3 is a walkway while its neighbour's layer 3 is a roof beam, purely
// because the neighbour carries more surfaces above it. Every neighbour test
// in the vertex builder asks whether the surface next door stands far above
// or below this one, and reading the neighbour at the SAME INDEX answers a
// different question entirely on a peeled layer: the rim clamp, the facade
// slope kill, the de-jut and the tall-ray scan then all fire on nearly every
// vertex, and the drape is culled before it draws anything (round 35, the
// first prototype's null result). Match by HEIGHT instead - whichever of the
// neighbour's three layers lies nearest this vertex - and report a sentinel
// when none lies within the band, so an unrelated stack reads as "no
// neighbour here" rather than as a cliff. Layer 1 keeps the plain lookup:
// there the highest surface IS coherent across columns, which is why the
// existing kills work, and roads must not shift by a texel.
static const float kPeelNeighborBand = 256.0;
// Clearance a peeled layer needs above it to count as open. A roof over a
// walkway clears by hundreds; a wall's own side faces sit within a couple of
// units of the surface they stand on.
static const float kCoverAirGap = 32.0;
float PatchTopNeighbor(float2 worldXY, float refZ)
{
	float r = PatchTop(worldXY);
	[branch] if (PatchLayer > 0.5)
	{
		float cand[3] = { r, PatchTop2(worldXY), PatchTop3(worldXY) };
		float best = -1e9;
		float bestDist = 1e9;
		[unroll] for (uint peelI = 0; peelI < 3; peelI++)
		{
			[flatten] if (cand[peelI] > -50000.0 && abs(cand[peelI] - refZ) < bestDist)
			{
				bestDist = abs(cand[peelI] - refZ);
				best = cand[peelI];
			}
		}
		r = bestDist <= kPeelNeighborBand ? best : -1e9;
	}
	return r;
}

// THE DOME (Josef's 0/10/20/30 sketch), and the same recipe the skin already
// runs rather than a second copy of it: a rolling-ball fillet over the repose
// cone, whose RADIUS freezes at the feature's own crest. The moment the rolls
// from both edges meet in the middle, growth stops whatever the depth slider
// says - at ratio 1 the frozen shape is the half-dome exactly filling the
// width. Without the freeze a narrow feature grows a fin instead of saturating.
//
// The cone reads through the layer accessor, so a peeled drape under a roof
// gets its own layer's cone rather than the top surface's.
float ObjectDomeDepth(float2 worldXY, float depthBase, out float3 domeNrm)
{
	domeNrm = float3(0.0, 0.0, 1.0);
	float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
	float cone = ObjectConeDepthL(worldXY);

	// Crest freeze: the largest cone within half a roll radius. A feature
	// narrow enough to saturate has its crest in reach; a wide one reads large
	// and passes unclamped, keeping the plain fillet.
	float tapR = 0.5 * coneSeed;
	float tapD = tapR * 0.7071;
	float crest = cone;
	crest = max(crest, ObjectConeDepthL(worldXY + float2(tapR, 0.0)));
	crest = max(crest, ObjectConeDepthL(worldXY - float2(tapR, 0.0)));
	crest = max(crest, ObjectConeDepthL(worldXY + float2(0.0, tapR)));
	crest = max(crest, ObjectConeDepthL(worldXY - float2(0.0, tapR)));
	crest = max(crest, ObjectConeDepthL(worldXY + float2(tapD, tapD)));
	crest = max(crest, ObjectConeDepthL(worldXY - float2(tapD, tapD)));
	crest = max(crest, ObjectConeDepthL(worldXY + float2(tapD, -tapD)));
	crest = max(crest, ObjectConeDepthL(worldXY - float2(tapD, -tapD)));

	float hEff = max(min(coneSeed, PileHeightRatio * crest), kMinSkinLift);
	float heightScale = hEff / coneSeed;
	float rollT = saturate(cone / hEff);
	float rimIn = 1.0 - rollT;

	// The dome's shape lives in the CONE field, so a finite difference of the
	// depth cannot see it - the shell shaded flat before the skin took its
	// normal from here instead. Analytic surface normal: the cone gradient
	// through the fillet's slope, clamped near the vertical rim so the rim
	// does not blow the derivative up.
	const float gs = 4.0;
	float cXP = min(ObjectConeDepthL(worldXY + float2(gs, 0.0)), coneSeed);
	float cXN = min(ObjectConeDepthL(worldXY - float2(gs, 0.0)), coneSeed);
	float cYP = min(ObjectConeDepthL(worldXY + float2(0.0, gs)), coneSeed);
	float cYN = min(ObjectConeDepthL(worldXY - float2(0.0, gs)), coneSeed);
	float2 coneGrad = float2(cXP - cXN, cYP - cYN) / (2.0 * gs);
	float dhdc = rimIn / max(sqrt(saturate(1.0 - rimIn * rimIn)), 0.2);
	domeNrm = normalize(float3(-coneGrad * dhdc, 1.0));

	return depthBase * heightScale * sqrt(saturate(1.0 - rimIn * rimIn));
}

// Patch surface evaluation, shared by the legacy VS and the tessellated
// domain shader. dense = tessellated call sites: generated vertices sit a
// unit or two apart, so the trail-margin test uses a cheap 5-tap cross
// instead of the 16-ray star the coarse 8-unit grid needs.
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

PatchVertex BuildPatchVertex(float2 worldXY, uniform bool dense)
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
	// against `top` below: a road-OWNED column drops the trample gate, so the
	// patch covers the whole road surface rather than only trails.
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
		float t00 = PatchTopL(base);
		float t10 = PatchTopL(base + float2(kHeightTexel, 0.0));
		float t01 = PatchTopL(base + float2(0.0, kHeightTexel));
		float t11 = PatchTopL(base + float2(kHeightTexel, kHeightTexel));
		top = lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
		// A sentinel lattice corner poisons the bilinear, and a large drop
		// across the cell (roof or wall edge) would interpolate vertices
		// midway down the facade, which the rim test then kills erratically
		// at tessellated density (sawtooth facade teeth). Both fall back to
		// the center sample so the whole cell resolves like the legacy grid
		// and dies or lives coherently.
		float tMin = min(min(t00, t10), min(t01, t11));
		float tMax = max(max(t00, t10), max(t01, t11));
		[flatten] if (tMin < -50000.0 || (tMax - tMin) > 100.0)
			top = PatchTopL(worldXY);
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
		top = PatchTopL(worldXY);
		skinDepth = skin.x;
		skinEdgeMin = skinDepth;
		roadTop = skin.y;
	}
	// A PEELED LAYER CANNOT BE ARBITRATED BY A LAYER-1 QUANTITY. The skin-depth
	// raster holds ONE value per column - the class depth of whatever the
	// capture saw on TOP - so under a roof it describes the roof. Gating a
	// layer-2 or layer-3 vertex on `skinDepth >= 1` and then carving it to that
	// depth asks the roof how much snow the walkway gets, and where the roof's
	// own draw wrote nothing the gate kills the whole column on every layer.
	// The per-layer cone is the same quantity that DOES exist per layer: Josef's
	// probe reads 4.9 on all three layers under a roof where this single raster
	// cannot speak for the lower two.
	[branch] if (PatchLayer > 0.5)
	{
		float layerCone = ObjectConeDepthL(worldXY);
		skinDepth = max(skinDepth, layerCone);
		skinEdgeMin = max(skinEdgeMin, layerCone);
	}
	float2 gridLocal = v.GridLocal;

	// Everything below reads the raster 30-50 more times per vertex, and on a
	// grid that now reaches the raster's full extent MOST vertices stand over
	// nothing. Two loads already answered that, so gate on them: a sentinel
	// top or a dead class depth can never reach the carve branch anyway.
	bool rim = false;
	float aliveDeform = 0.0;
	bool roadField = false;
	bool owns = false;
	bool objectField = false;
	[branch] if (top > -50000.0 && skinDepth >= 1.0)
	{
		// OWNERSHIP FIRST, because the facade kills below must not fire on
		// a road. Same predicate as RoadOwnsColumn, against this vertex's
		// already-sampled top/roadTop. Evaluated before the de-jut can
		// lower `top`, which only makes it more conservative: a real road
		// column reads top == roadTop either way.
		// Roads live on layer 1 only: the skin-depth raster's road channel is
		// per COLUMN, so a peeled layer under a bridge would inherit the
		// bridge's road ownership and take the road path's rim exemptions.
		owns = PatchLayer < 0.5 && roadTop > kNoRoadTop * 0.5 && (top - roadTop) < kRoadOwnsTop;

		// OBJECT OWNERSHIP, resolved here for the same reason road ownership
		// is: the facade kills below must know about it BEFORE they run.
		objectField = ObjectDrape > 0.5 && !owns;

		// THE DEPTH AUTHORITY, and the fifth time the wrong one was asked. The
		// skin-depth raster MAX-blends class depth across every column a
		// footprint overlaps, so a rock standing on a road inherits the ROAD's
		// depth - Josef's tell: the Road Meshes slider lifted a boulder's
		// shell. Harmless while such a column could never draw; object
		// ownership opened that gate, at the wrong class. A column the road
		// does not own takes the object class from the CB, the only depth that
		// is its own. The raster still arbitrates road columns, where its
		// value IS the road's.
		[flatten] if (objectField)
			skinDepth = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);

	// Rim test: a vertex whose column towers over a neighbour is the top edge
	// of a tall structure, whose triangles stretch down the facade as white
	// sheets. VALID neighbours only - a sentinel neighbour must not count as a
	// rim, or the patch's edge ring is culled along every road chunk. Facade
	// sheets still die by their own sentinel top.
	float topXP = PatchTopNeighbor(worldXY + float2(kHeightTexel, 0.0), top);
	float topXN = PatchTopNeighbor(worldXY - float2(kHeightTexel, 0.0), top);
	float topYP = PatchTopNeighbor(worldXY + float2(0.0, kHeightTexel), top);
	float topYN = PatchTopNeighbor(worldXY - float2(0.0, kHeightTexel), top);
	float minNeighborTop = 1e9;
	if (topXP > -50000.0)
		minNeighborTop = min(minNeighborTop, topXP);
	if (topXN > -50000.0)
		minNeighborTop = min(minNeighborTop, topXN);
	if (topYP > -50000.0)
		minNeighborTop = min(minNeighborTop, topYP);
	if (topYN > -50000.0)
		minNeighborTop = min(minNeighborTop, topYN);
	// 100 units: house facades still cull (wall drops are 300+), but steep
	// boulder crests do not lose their trench-edge vertices (small notch
	// triangles at rock rims).
	rim = minNeighborTop < 1e8 && (top - minNeighborTop) > 100.0;
	// Facade slope kill: tall walls whose raster drop is SMEARED over
	// several texels evade the single-step rim threshold (the debug view
	// showed the patch draped down building walls as sawtooth sheets). A
	// steep central gradient marks them regardless of how the drop is
	// distributed; trench-bearing boulder tops stay under the threshold.
	[flatten] if (topXP > -50000.0 && topXN > -50000.0 && topYP > -50000.0 && topYN > -50000.0)
	{
		// 0.5 (~27 degrees): the whole smeared flank dies, not just its
		// steep core; the surviving band was the jagged wall-base sheath.
		// Trench-bearing surfaces (roads, walkable boulder tops) sit well
		// under this; steep flanks belong to the skin.
		float2 topGrad = float2(topXP - topXN, topYP - topYN) / (2.0 * kHeightTexel);
		// OBJECTS ARE EXEMPT FROM THIS ONE KILL, AND ONLY THIS ONE. A gradient
		// of 0.5 across eight units is a four-unit drop, which ANY rock edge
		// clears by construction - this is the kill that ate the boulder's rim
		// and left Josef's sawtooth. The 100-unit neighbour clamp above and the
		// tall-ray scan below still apply to objects, and they are what keep
		// the lattice off a building facade: exempting objects from those too
		// drowned every wall in Dawnstar in green curtains.
		rim = rim || (length(topGrad) > 0.5 && !objectField);
	}
	// Wall-base de-jut: the last LIVE ring at the foot of a culled facade
	// still samples tops partway up the smeared ramp and rises as a jagged
	// rim along the wall. Clamping to the lowest valid neighbor plus a
	// normal-slope allowance flattens the rim to the ground it belongs to.
	// NOT on a road: at a road's own edge the lowest valid neighbour is the
	// terrain beside it, and clamping to it drags the road's snow down by
	// the whole kerb height - the mismatched/floating road edges.
	[flatten] if (minNeighborTop < 1e8 && !owns && !objectField)
		top = min(top, minNeighborTop + 2.0 * kHeightTexel);

	// Untrenchable band around much-taller structures: the raster smear
	// leaves elevated plateaus hugging walls that evade both the slope
	// kill (locally flat) and the neighbor clamp (all neighbors on the
	// plateau). Any surface towering over this vertex within the band
	// kills it outright. The height threshold is the building detector:
	// houses, walls and cliffs tower by hundreds of units; benches and
	// low rocks can never trigger it.
	static const float2 kTallRays[8] = {
		{ 24.0, 0.0 }, { 17.0, 17.0 }, { 0.0, 24.0 }, { -17.0, 17.0 },
		{ -24.0, 0.0 }, { -17.0, -17.0 }, { 0.0, -24.0 }, { 17.0, -17.0 }
	};
	bool nearTall = false;
	[unroll] for (uint tallI = 0; tallI < 8; tallI++)
	{
		float tallTop = PatchTopNeighbor(worldXY + kTallRays[tallI], top);
		[flatten] if (tallTop > -50000.0 && (tallTop - top) > 100.0)
			nearTall = true;
	}
	rim = rim || nearTall;

	// A ROAD NEVER RIMS (Josef: road shells must never have holes, "even
	// at their own edges"). Every kill above exists to keep the patch off
	// building facades and off plateaus hugging walls - and a road's OWN
	// edge trips the facade slope test by construction, since a RoadChunk
	// stands proud of the terrain and drops far more than four units
	// across the eight the gradient measures. The killed vertex takes the
	// six triangles around it with it, and the road skin has already
	// stepped aside on that column (RoadOwnsColumn is true there, the road
	// IS the top), so nothing is left: the hexagonal holes photographed
	// from underneath. The patch's silhouette dissolve still clips a real
	// overhang in the PS, so dropping the vertex kill costs no protection.
	//
	// AND NEITHER DOES AN OBJECT, for the identical reason (Josef: the rock
	// wears the RoadChunk bug). A boulder's own edge trips the facade slope
	// test by construction - it stands proud of the ground and drops far more
	// than four units across the eight the gradient measures - so the kills
	// ate the vertices around its rim and left the sawtooth holes with the
	// rock showing through. The killed vertex takes six triangles with it.
	//
	// It is also what makes each object its OWN shell rather than one sheet
	// welded to the road's: cutting in the PS by silhouette drop ends the
	// rock's snow at the rock's edge and lets the ground's continue
	// underneath, exactly as a road chunk stops being welded to the terrain.
	//
	// The exemption is NARROW for objects - the slope kill only, applied
	// above. Roads keep the blanket exemption because a road is low; objects
	// include three-hundred-unit facades, and exempting them from the
	// neighbour clamp and the tall-ray scan drowned every wall in Dawnstar.
	rim = rim && !owns;

	// The bisection is over - the drape draws, and these kills are exactly what
	// it needs. With them off, a peeled layer sheeted straight down every wall
	// and rock standing on a road (Josef's yellow curtains): the facade slope
	// kill and the rim clamp are the rules that keep a lattice off vertical
	// faces. They are height-matched now rather than index-matched, so they
	// finally mean on a peeled layer what they always meant on layer 1.

	// Neighborhood trample test: the patch lives only around trails. The
	// coarse 8-unit grid samples a 1.5-cell margin as a 16-ray star (at
	// radius 12 the rays sit 22.5 degrees apart, so even the thinnest trail
	// cannot slip between rays); dense tessellated vertices sit a unit or
	// two apart and a 5-tap cross covers their footprint.
	aliveDeform = SampleDeformation(gridLocal);
	if (dense) {
		aliveDeform = max(aliveDeform, SampleDeformation(gridLocal + float2(6.0, 0.0)));
		aliveDeform = max(aliveDeform, SampleDeformation(gridLocal - float2(6.0, 0.0)));
		aliveDeform = max(aliveDeform, SampleDeformation(gridLocal + float2(0.0, 6.0)));
		aliveDeform = max(aliveDeform, SampleDeformation(gridLocal - float2(0.0, 6.0)));
	} else {
		static const float2 kAliveRays[16] = {
			{ 12.0, 0.0 }, { 11.09, 4.59 }, { 8.49, 8.49 }, { 4.59, 11.09 },
			{ 0.0, 12.0 }, { -4.59, 11.09 }, { -8.49, 8.49 }, { -11.09, 4.59 },
			{ -12.0, 0.0 }, { -11.09, -4.59 }, { -8.49, -8.49 }, { -4.59, -11.09 },
			{ 0.0, -12.0 }, { 4.59, -11.09 }, { 8.49, -8.49 }, { 11.09, -4.59 }
		};
		[unroll] for (uint rayI = 0; rayI < 16; rayI++)
			aliveDeform = max(aliveDeform, SampleDeformation(gridLocal + kAliveRays[rayI]));
	}

	// Road heightfield: the patch IS the road's snow, so it must exist over
	// the whole surface, not just around trails. Untrampled road resolves to
	// deform 0 below, i.e. a flat full-depth surface - the same expression,
	// evaluated everywhere.
	//
	// OWNERSHIP, not presence: a road's footprint reaches every column it
	// overlaps in plan view, including those whose top belongs to a rock,
	// cairn, wall or building standing on it - and draping road-depth snow
	// over those was the plate regression. The road owns the column only
	// where the column's top IS the road.
	// `owns` is computed at the top of this block now - the facade kills
	// have to know about it before they run.
	roadField = RoadField > 0.5 && owns;

	// OBJECT OWNERSHIP - the drape pivot itself. A captured column whose top
	// is not a road takes the WHOLE surface, exactly as a road-owned column
	// does, instead of only the trench around a footprint. That single line is
	// what turns the lattice from "roads plus trails" into "roads plus trails
	// plus every object it can stand on".
	//
	// Ownership decides COVERAGE, not whether the lattice may stand on a
	// facade: the rim clamp and the facade slope kill above still delete the
	// vertices that would sheet down a wall, and they run before this.
	}  // end cheap gate

	// The DEPTH channel bleeds exactly as the road bit did: a road's footprint
	// MAX-blends its class depth across every column it overlaps, so a rock or
	// cairn standing on a road inherits a carvable depth it was never granted
	// (its own capture writes zero while Trenches on Objects is off). Ownership
	// fixed the untrampled case; without it here too, walking on such a rock
	// still cut a trench into it.
	//
	// So while the heightfield owns roads, a road is the only thing allowed to
	// carve, and a trampled column no road owns is raster bleed. Trenches on
	// Objects re-opens the old path deliberately - it is the experimental
	// toggle this whole plan is the rework of - and turning the heightfield OFF
	// restores the pre-heightfield behaviour exactly, so the A/B stays honest.
	// THE AIR TEST (Josef's Option-B question): a peeled layer draws only where
	// the thing standing above it actually CLEARS the surface. A roof over a
	// walkway leaves open space and the walkway keeps its snow; a wall standing
	// on a road is solid to the ground, and draping that column paints road
	// snow INSIDE the stone - his "any object above you also gets the shell".
	// The two are identical in the tops-only rasters, so no height threshold
	// can separate them; this reads the cover raster built for the question.
	bool airOK = true;
	[branch] if (PatchLayer > 0.5)
		airOK = (CoverBottomL(worldXY) - top) > kCoverAirGap;

	bool mayTrample = (RoadField < 0.5) || ObjectTrenches > 0.5 || owns;
	// A PEELED LAYER DRAWS ITS WHOLE SURFACE, not just its trails. The
	// trample gate exists because the layer-1 patch is the TRENCH layer -
	// it lives around footprints and the skin owns everything else. A
	// layer-2 drape has no skin behind it: it exists precisely to put snow
	// on a surface the roof above has been hiding, so gating it on
	// deformation would leave the walkway bare, which is the whole point.
	bool trampled = roadField || objectField || (aliveDeform >= 0.005 && mayTrample) || PatchLayer > 0.5;
	v.RoadBit = roadField ? 1.0 : 0.0;

	// Single-return structure: an early return inside a [branch] trips
	// fxc's X4000 and CI enforces zero warnings.
	[branch] if (top > -50000.0 && skinDepth >= 1.0 && !rim && trampled && airOK)
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
		[branch] if (roadField)
		{
			float landDepth = max(SampleTerrainStatics(gridLocal).y, 0.0);
			float cone = ObjectConeDepthL(worldXY);
			skinDepth = max(min(cone, skinDepth), min(landDepth, skinDepth));
		}

		// Bicubic, like the landscape shell; rounded trench walls.
		float deform = saturate(SampleDeformationSmooth(gridLocal));

		// Carve through the SHARED profile, so object trenches and landscape
		// trenches are one shape: the depth remap, the rim teeth and the lip all
		// arrive, and the floor rides Trench Floor Height exactly as the ground's
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
		float bermD = 0.0;
		[branch] if (ObjBermHeightAmp > 0.005)
			bermD = BermField(gridLocal);
		// An object column takes the DOME; the carve profile is the trench
		// layer's shape and reads as a flat-topped slab on a boulder. The berm
		// is a trail feature and has no business on an object either.
		float3 domeNrm = float3(0.0, 0.0, 1.0);
		float depth;
		[branch] if (objectField)
		{
			depth = ObjectDomeDepth(worldXY, skinDepth, domeNrm);
		}
		else
		{
			depth = CarveProfile(deform, skinDepth, worldXY) +
			        BermShape(bermD) * saturate(1.0 - deform) * skinDepth * ObjBermHeightAmp * BermDepthGate(skinDepth);
		}

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

		// Churn: broken lumps on carved snow, the landscape shell's weighting
		// verbatim (ChurnWeight x depth/10) so trench floors read trampled
		// exactly as the ground's do. The old room factor (headroom above the
		// floor) zeroed churn AT the floor, and fully trampled road floors
		// shaded as pristine top snow. The object-exposure guard is now the
		// pad clamp instead: lumps dig toward the pad, never through it.
		float churnW = ChurnWeight(deform, bermD) * saturate(depth / 10.0);
		[branch] if (ObjChurnHeightAmp > 0.01 && churnW > 0.001)
			depth = max(depth + ChurnNoise(worldXY) * ObjChurnHeightAmp * churnW, pad);

		// Berm clods: a coarser octave weighted by BermShape rather than the churn
		// weight, since spoil lands on the crest while churn peaks in the trench.
		// Mirrors ShellSurfaceZ; the self-shadow march skips it, as it skips churn.
		[branch] if (RimStyle.z > 0.01 && bermD > 0.003)
			depth += ChurnNoiseScaled(worldXY, kClodSizeScale) * RimStyle.z *
			         BermShape(bermD) * saturate(1.0 - deform) * BermDepthGate(skinDepth);

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

#ifdef SNOW_SHADOW_CAST
		// The landscape caster's conventions (SnowShell.hlsl SNOW_SHADOW_CAST):
		// only snow standing meaningfully above its ground casts - a dusting
		// shadowing the object beneath it reads as the object darkening - and
		// the caster fades out over the same 40-70 m band so the two
		// surfaces' shadows end together at the verge. Distance from the
		// patch centre, which tracks the camera; killed vertices take the
		// same NaN path as dead ones.
		float castVis = smoothstep(2.0, 5.0, depth) *
		                (1.0 - smoothstep(2800.0, 4900.0, length(worldXY - WorldRow0.xy)));
		[flatten] if (castVis < 0.35)
			v.Killed = 1.0;
#endif

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
		// Churn shading moved to the PS: at ChurnSize 0.25 the lumps sit at
		// 4/1.75 units, under even the dense band's vertex spacing, so the
		// vertex-rate gradient interpolated to smooth and trampled floors
		// shaded as pristine top snow (Josef's road-floor report). Geometry
		// keeps the coarse displacement above; the normal carries the look.
		const float gStep = 4.0;
		float dXP = saturate(SampleDeformationSmooth(gridLocal + float2(gStep, 0.0)));
		float dXN = saturate(SampleDeformationSmooth(gridLocal - float2(gStep, 0.0)));
		float dYP = saturate(SampleDeformationSmooth(gridLocal + float2(0.0, gStep)));
		float dYN = saturate(SampleDeformationSmooth(gridLocal - float2(0.0, gStep)));
		float2 profGrad = float2(
			CarveProfile(dXP, skinDepth, worldXY + float2(gStep, 0.0)) - CarveProfile(dXN, skinDepth, worldXY - float2(gStep, 0.0)),
			CarveProfile(dYP, skinDepth, worldXY + float2(0.0, gStep)) - CarveProfile(dYN, skinDepth, worldXY - float2(0.0, gStep))) / (2.0 * gStep);
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
		float2 bermGrad = float2(0.0, 0.0);
		[branch] if (ObjBermHeightAmp > 0.005 && bermD > 0.003)
		{
			const float bStep = 4.0;
			bermGrad = float2(
				BermShape(BermField(gridLocal + float2(bStep, 0.0))) - BermShape(BermField(gridLocal - float2(bStep, 0.0))),
				BermShape(BermField(gridLocal + float2(0.0, bStep))) - BermShape(BermField(gridLocal - float2(0.0, bStep)))) / (2.0 * bStep) *
			          saturate(1.0 - deform) * skinDepth * ObjBermHeightAmp * BermDepthGate(skinDepth);
		}
		// Surface z = top + profile, so normal.xy = -d(profile); the other
		// fields RAISE the surface and subtract for the same reason.
		// The dome carries its own analytic normal; the profile gradient is
		// blind to a shape that lives in the cone field.
		v.NormalWS = objectField ? domeNrm :
		                           normalize(float3(-profGrad - undGrad - bermGrad, 1.0));
		v.SkinDepth = skinDepth;
		v.Deform = deform;
		v.Killed = 0.0;
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
	// Mode 6 covers the PATCH too now: which draw layer put this pixel here,
	// in the skin's own green/yellow/red convention so one screenshot compares
	// the two paths. The mode used to paint the patch dim gray, which is why a
	// null drape result was indistinguishable from a culled one.
	[flatten] if (StaticsDebugView > 5.5)
	{
		vsout.Coverage = (PatchLayer + 0.5) / 8.0;
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
		float3 prevRel = v.WorldAbs - ShellCameraPreviousPosAdjust.xyz;
		vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
		vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
		vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
		vsout.WorldPos = rel;
	}
	return vsout;
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
	static const float2 kCorners[6] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 6;
	float2 gridXY = float2(quadIndex % kPatchGridDim, quadIndex / kPatchGridDim) + kCorners[vertexID % 6];
	// WorldRow0.xy carries the snapped patch CENTRE (see the CPU fill).
	float2 worldXY = WorldRow0.xy + PatchWarpXY(gridXY - kPatchGridDim * 0.5);
	return FinishPatchVertex(BuildPatchVertex(worldXY, false));
}
#elif defined(VSHADER) && defined(PATCH)
// Tessellated patch control points: placement only.
struct TessControlPointPatch
{
	float2 WorldXY : TEXCOORD0;
};

TessControlPointPatch main(uint vertexID : SV_VertexID)
{
	static const float2 kPatchCorners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 4;
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

struct TessFactorsPatch
{
	float Edge[4] : SV_TessFactor;
	float Inside[2] : SV_InsideTessFactor;
};
#endif

#include "SnowDeformation/SnowParallax.hlsli"

#if defined(HULLSHADER) && defined(PATCH)
// Same trench-aware quad factors as the landscape shell: the patch IS the
// trench layer, so deformed edges get extended detail reach.
float PatchEdgeTessFactor(float2 worldA, float2 worldB)
{
	float2 mid = 0.5 * (worldA + worldB);
	float dist = length(mid - ShellCameraPosAdjust.xy);
	float deform = max(max(SampleDeformation(worldA - GridOrigin), SampleDeformation(worldB - GridOrigin)), SampleDeformation(mid - GridOrigin));
	// Undeformed edges subdivide only to carry the relief; at relief 0 their
	// base reach goes to zero and the factor clamps to 1. Kept in step with
	// EdgeTessFactor in SnowShell.hlsl. Still edge-derived only, so crack-free.
	float reliefBase = SnowReliefDepth > 0.01 ? 1.0 : 0.0;
	float reach = 1600.0 * lerp(reliefBase, 3.0, smoothstep(0.02, 0.25, deform));
	return clamp(reach / max(dist, 32.0), 1.0, 8.0);
}

TessFactorsPatch PatchConstants(InputPatch<TessControlPointPatch, 4> patch)
{
	TessFactorsPatch f;
	// Cull patches with no live corner (off the footprint or untrampled);
	// the cheap kill terms only, the domain shader kills per vertex.
	bool anyLive = false;
	[unroll] for (uint i = 0; i < 4; i++)
	{
		float2 w = patch[i].WorldXY;
		if (PatchTop(w) > -50000.0 && PatchSkinDepth(w).x >= 1.0)
			anyLive = true;
	}
	if (!anyLive) {
		f.Edge[0] = f.Edge[1] = f.Edge[2] = f.Edge[3] = 0.0;
		f.Inside[0] = f.Inside[1] = 0.0;
		return f;
	}
	f.Edge[0] = PatchEdgeTessFactor(patch[0].WorldXY, patch[3].WorldXY);
	f.Edge[1] = PatchEdgeTessFactor(patch[0].WorldXY, patch[1].WorldXY);
	f.Edge[2] = PatchEdgeTessFactor(patch[1].WorldXY, patch[2].WorldXY);
	f.Edge[3] = PatchEdgeTessFactor(patch[3].WorldXY, patch[2].WorldXY);
	float inner = max(max(f.Edge[0], f.Edge[1]), max(f.Edge[2], f.Edge[3]));
	f.Inside[0] = inner;
	f.Inside[1] = inner;
	return f;
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("PatchConstants")]
TessControlPointPatch main(InputPatch<TessControlPointPatch, 4> patch, uint i : SV_OutputControlPointID)
{
	return patch[i];
}
#endif

#if defined(DOMAINSHADER) && defined(PATCH)
[domain("quad")]
VS_OUTPUT main(TessFactorsPatch factors, float2 domainUV : SV_DomainLocation, const OutputPatch<TessControlPointPatch, 4> patch)
{
	float2 worldXY = lerp(
		lerp(patch[0].WorldXY, patch[1].WorldXY, domainUV.x),
		lerp(patch[3].WorldXY, patch[2].WorldXY, domainUV.x), domainUV.y);
	PatchVertex v = BuildPatchVertex(worldXY, true);

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
float SkinCollapseEnd(float depthBase)
{
	return max(SkinHeightFadeEnd, 1.0) * lerp(0.4, 1.0, saturate(depthBase / 25.0));
}
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
	// Debug (Shell Layers view): which peeled plane owned this vertex.
	// 0 = not an S4 draw, 1/2/3 = the layer, 4 = below all three.
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

	// Snow Breakup: a NEGATIVE base plus positive noise, the shape the DefoQ
	// reference ships (its thickness is -0.016 against noise strength +0.099).
	// A uniform positive depth reads as paint; taking a constant fraction off
	// and handing it back through world noise lets cover thin to BARE at the
	// mesh's own scale, so the break-up belongs to the layer instead of needing
	// a mask of its own. Renormalised so full noise still reaches 1, and 0 is
	// exactly the uniform coat. World-anchored on purpose: the shadow caster
	// evaluates the same function at the same worldBase, so shadow and shell
	// cannot disagree. Roads are exempt - their height stays in step with the
	// landscape shell at the verge, which does not break up. Consumed by BOTH
	// the classic upFacing and the S4 mask; see the note at each site.
	// The cut walks up from BELOW the noise's range, so at slider 0 the factor
	// is 1 for every value of the noise and there is no step off zero. The
	// first version was saturate((1+b)*n - b), which at b=0.01 is ~n - i.e. it
	// jumped straight from "uniform" to "the whole surface varies 0..1", which
	// is what blotched a cliff at one hundredth of the slider.
	//
	// kBreakupSoft is deliberately WIDE. A hard mask in a DEPTH field builds
	// vertical walls, the shadow caster shares this lift, and those walls are
	// what cast the sliver self-shadows the first version showed. The visible
	// EDGE stays crisp anyway: the shape gates binarize coverage at 0.5
	// downstream, so a smooth depth ramp still ends in a hard contour.
	float breakupFactor = 1.0;
	[branch] if (SkinBreakup > 0.001 && LegacySkin < 0.5)
	{
		float breakNoise = ShapeNoise(worldBase.xy / kSkinBreakupScale);
		float cut = lerp(-kBreakupSoft, kBreakupMaxBare, saturate(SkinBreakup));
		breakupFactor = smoothstep(cut, cut + kBreakupSoft, breakNoise);
	}

	// Snow accumulates on up-facing surfaces (steep shingles and walls stay
	// bare, matching the vanilla projection's extent). flat meshes gate hard
	// on the raw normal so plank sides stay clean; rounded meshes ramp over
	// almost the whole up-facing range of the SMOOTHED normal.
	// The layer stays geometrically uncarved: trench relief is traced per
	// pixel in the PS instead.
	// TIER 1 SEAM WELD (SkinWeld, 0 = the behaviour above, unchanged).
	//
	// The lift is single-valued per POSITION in every term except this one:
	// depthBase is per-mesh, and the taper, shelter, sky and cone terms are all
	// functions of worldBase.xy. Only upFacing reads a per-VERTEX quantity, and
	// only on the flat class - so at a plank's top edge the top twin gates ~1
	// and lifts a full class depth while the side twin gates ~0 and lifts
	// nothing, FROM THE SAME POSITION. Zero base travel, a class depth of
	// disagreement: the triangle spanning them is drawn as a wall, and that is
	// the fence family the lift-gradient view lights up.
	//
	// smoothWS is the position-hash weld SmoothNormalsCS already builds, so it
	// is IDENTICAL for every vertex sharing a position by construction. Sliding
	// the gate toward it makes the twins agree exactly at 1.0 - which is the
	// whole mechanism, not a tuning curve. Unresolved vertices fall back to the
	// raw normal in BuildSkinVertex, so the lerp is a no-op there.
	//
	// The raw normal is here ON PURPOSE ("plank sides stay clean") and welding
	// trades that away: a side face's twin now sees a partly up-facing normal
	// and can take cover. That trade IS the vertical snow wall a thick layer
	// should have at a plank's edge - but it is a taste call, hence the dial.
	float weldNz = lerp(nrmWS.z, smoothWS.z, saturate(SkinWeld));
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
	// Shell Layers debug view: which peeled plane owned this vertex.
	float debugLayer = 0.0;
	// S4 dome shading normal; non-S4 paths keep the raw normal.
	float3 shadeNormal = nrmWS;
	// Flat PD shell (S3 round 10, Josef's split): this draw's cover is a
	// CONSTANT coat inflated along the sealed smooth normal - set at the
	// END of this function; the depth pipeline below belongs to the 3D
	// layer and the coat does not participate in it. The PS owns placement
	// entirely: vanilla's weight rebuilt per pixel (G-buffer normal,
	// authored alpha, noise term always in full - the footprint IS the
	// purple), sliced by Snow Fill's angular knob. ProjLinear rides
	// TEXCOORD8 carrying the AUTHORED VERTEX ALPHA - the PS needs the raw
	// authored term, not a pre-mixed weight. Vertical-lift history and why
	// it could never cover every angle: SKIN-PLACEMENT-PLAN rounds 4-10.
	[branch] if (ProjPixelEnable > 1.5)
	{
		projLinear = vertexAlpha;
		upFacing = 0.0;
	}
	else [branch] if (ProjThreshold > -0.5)
	{
		float projWeight = nrmWS.z * vertexAlpha - max(ProjThreshold, 0.0);
		[flatten] if (ProjDensityEnable > 0.5)
			projFactor = saturate(projWeight) * smoothstep(0.06, 0.16, projWeight);
		else [flatten] if (ProjMaskEnable > 0.5)
			projFactor = saturate(5.0 * projWeight);
		upFacing *= projFactor;
	}
	// Snow Breakup applies to the COVERAGE MASK, not to the depth, and that is
	// not a detail: the S4 block below REBUILDS depth from depthBase (see
	// "depth = depthBase * heightScale * ..."), so anything written into depth
	// up here is discarded on exactly the draws the feature exists for. The
	// masks are what both paths carry through to the end - classic multiplies
	// depthBase by upFacing, S4 multiplies by mask and then exports mask AS
	// upFacing - so scaling them takes depth and coverage together and the
	// shell cannot end up thinned but still claiming to cover (the film-round
	// failure, paid for four times).
	upFacing *= breakupFactor;
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
	[branch] if (HasObjectTop > 0.5 && LegacySkin < 0.5 && depth > 0.001)
	{
		// The field is seeded with the deepest class in play, so normalizing
		// by that recovers distance-to-rim: 0 at the rim, 1 in the interior.
		float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
		float steep = clamp(MoundSteepness, 0.5, 3.0);
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
		float rollFrac = saturate(kCorniceRoll * steep / coneSeed);
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
	// THE CORNICE'S PROPORTIONAL CAP. hEff is the height this feature's own
	// width can support - the crest freeze already computes it - so scaling the
	// throw by it makes a narrow post get a narrow lip instead of the same
	// absolute throw a boulder gets, which is what turned fence posts into
	// mushroom discs. Falls back to the class depth where no dome ran.
	float lipHeight = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);

	// Geometry LOD: collapse the layer to nothing BEFORE the material dissolve
	// (SkinFadeStart/End) begins, so the hand-off to the object's own projected
	// snow has no silhouette left to pop. Range scales with class depth against
	// the depth sliders' 25-unit maximum.
	// Roads are exempt: their height must stay in step with the landscape
	// shell they meet at the verge, and that shell does not collapse.
	[branch] if (SkinHeightFadeEnd > 1.0 && LegacySkin < 0.5)
	{
		float collapseEnd = SkinCollapseEnd(depthBase);
#if defined(SHADOWCAST)
		// Caster pass: ShellCameraPosAdjust is zeroed (absolute world), so
		// measure from the height window's centre, which tracks the camera -
		// the patch caster's own convention. The caster must collapse WITH
		// the visible skin: with the collapse disabled CB-side it kept full
		// height past the skin range and threw full shadows over shells the
		// eye no longer sees (Josef's distance streaks).
		float camDist = length(worldBase.xy - HeightWindowCenter);
#else
		float camDist = length(worldBase - ShellCameraPosAdjust.xyz);
#endif
		depth *= 1.0 - smoothstep(collapseEnd * 0.55, collapseEnd, camDist);
		// Floor at the minimum coat instead of zero. Collapsing all the way
		// puts the skin vertex EXACTLY on its source vertex, where it z-fights
		// its own mesh and rasterises nothing at all - so distant objects lost
		// their snow outright instead of flattening into a painted layer. Same
		// hazard kMinSkinLift guards depthBase against at line ~944; the
		// collapse multiplies after it, so it needs its own floor. Scaled by
		// upFacing so genuinely steep faces still stay bare.
		depth = max(depth, kMinSkinLift * upFacing);
	}

	// S4 phase 1 - the NEW 3D shell (Josef's 0/10/20/30 sketch): a
	// ROLLING-BALL FILLET whose radius IS the height, grown VERTICALLY
	// over the fill-covered ("cyan") slice of the projected footprint.
	// Applied LAST because the classic depth pipeline (taper, shelter,
	// collapse) belongs to the classic layer - the fillet IS this shell's
	// taper. h(t) = H*sqrt(1-(1-t)^2) over the cone field's normalized
	// distance-to-rim: the roll's run equals H, so the rounding lengthens
	// with the slider by construction, flattening into the blanket
	// interior; MoundSteepness naturally modulates roll tightness. The
	// placement mask (vertex frequency; the PS refines it per pixel):
	// inside the projected footprint, inside the fill's angular slice,
	// and TOP-VISIBLE - only what the downward camera (the object-top
	// raster) can see grows the shell; the recolored PD continues
	// underneath everywhere else.
	[branch] if (ProjPixelEnable > 1.5)
	{
		// TIER 1: all three of this block's normal gates read weldNz, not
		// nrmWS.z. mask becomes BOTH the depth and (via upFacing = mask) the
		// coverage, so welding only the classic upFacing above left S4 draws -
		// every walkway, deck and roof board - completely untouched. Same trap
		// as Snow Breakup's first build: this block rebuilds what the code
		// above it computed. Vertex ALPHA stays raw per S1.3 and is a second,
		// separate source of twin disagreement if seams survive this.
		float wLin = weldNz * vertexAlpha - max(ProjThreshold, 0.0) + 0.1;
		// maskBase = the PD footprint and fill gates alone; the up-facing
		// gate multiplies in below, and the meld wall bypasses ONLY it.
		float maskBase = smoothstep(0.0, 0.05, wLin);
		float fillNzCut = 1.0 - 2.0 * ProjSnowFillSk;
		maskBase *= smoothstep(fillNzCut - 0.05, fillNzCut + 0.05, weldNz);
		float mask = maskBase;
		// Vertical growth is only meaningful on up-facing surfaces - a wall
		// lifted along +Z slides along itself, and at fill 100% the +0.1
		// bias floored whole walls into the mask (Josef's whitewashed
		// boards). Steep faces are the RECOLOR's job; the shell's geometry
		// is the tops', and the cutoff is Josef's slider (ShellMinNz =
		// cos of the max slope).
		mask *= smoothstep(ShellMinNz, ShellMinNz + 0.15, weldNz);
		// NO top-visibility cut (Josef, 2026-08-29): comparing against the
		// GLOBAL column top made anything under a roof or railing lose its
		// shell with a hard mid-plank cliff - the walkway's red-outlined
		// edges. Snow builds wherever the projected footprint says, roofs
		// included; the proper sheltering ("no snow under tents") returns
		// later as its own mechanism. Undersides stay harmless: their
		// up-displaced faces land inside their own geometry.
		debugLayer = 1.0;
		float rollT = 1.0;
		float meldWall = 0.0;
		float heightScale = 1.0;
		float3 domeNormal = nrmWS;
		// Kept for P5's lip: the cone gradient points INWARD (the cone rises
		// away from a rim), so its negation is the outward direction the rim
		// has to bulge along.
		float2 domeConeGrad = float2(0.0, 0.0);
		// The drawn surface's DEPTH gradient (dd/dx, dd/dy), zero until the
		// dome runs. Carried explicitly rather than decoded back out of
		// domeNormal: that is SEEDED with nrmWS, so on any draw where the
		// dome never ran a mesh normal would decode as a bogus gradient.
		float2 domeHeightGrad = float2(0.0, 0.0);
		[branch] if (HasObjectTop > 0.5)
		{
			float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
			float cone = ObjectConeDepth(worldBase.xy);
			// Layer select (S4 phase 2, K=3): a vertex belongs to the
			// topmost PEELED plane whose height matches its own. Below
			// a layer by more than the peel tolerance, the roll comes
			// from the next layer's cone; below all three, no plane
			// owns the surface and it gets NO roll data rather than a
			// full-height interior borrowed from someone else's plane
			// - which was the beam-streak and staircase-hole failure.
			float top1 = PatchTopPoint(worldBase.xy);
			[branch] if (top1 > -50000.0 && worldBase.z < top1 - PeelTol)
			{
				// "Ignore Cover Above" (Josef's rule): cover more than
				// the clearance overhead is a separate world - a roof
				// over a porch, a bench over its floor. The plane keeps
				// its FULL uniform height and clips through, instead of
				// deferring to a peeled layer's narrow tattered
				// footprint - but ONLY where the plane actually EXISTS
				// beneath the cover: a peeled layer top at the vertex's
				// own height proves it. A tread edge whose nearest texel
				// belongs to the wall has no plane of its own there -
				// full height would run the dome off the end as a lifted
				// open shelf (the left-rolls/right-floats stair bug), so
				// it gets no roll data and rounds off instead. Cover
				// WITHIN the clearance (a tread over a tread, a low
				// ledge) descends the cascade as usual.
				float coneDeep;
				[branch] if (top1 - worldBase.z > OverheadIgnore)
				{
					float top2 = PatchTop2Point(worldBase.xy);
					float top3 = PatchTop3Point(worldBase.xy);
					bool planeHere = (top2 > -50000.0 && abs(worldBase.z - top2) <= PeelTol) ||
					                 (top3 > -50000.0 && abs(worldBase.z - top3) <= PeelTol);
					coneDeep = planeHere ? coneSeed : 0.0;
					[flatten] if (!planeHere)
						debugLayer = 4.0;
				}
				else
				{
					float top2 = PatchTop2Point(worldBase.xy);
					float cone2v = ObjectConeDepth2(worldBase.xy);
					coneDeep = cone2v;
					debugLayer = 2.0;
					[branch] if (top2 < -50000.0 || worldBase.z < top2 - PeelTol)
					{
						float top3 = PatchTop3Point(worldBase.xy);
						float cone3v = ObjectConeDepth3(worldBase.xy);
						debugLayer = 3.0;
						[flatten] if (top3 < -50000.0 || worldBase.z < top3 - PeelTol)
						{
							cone3v = 0.0;
							debugLayer = 4.0;
						}
						// Same smooth hand-off as below, one layer deeper.
						float f2 = top2 > -50000.0 ? smoothstep(PeelTol, PeelTol * 2.0, top2 - worldBase.z) : 1.0;
						coneDeep = lerp(cone2v, cone3v, f2);
					}
				}
				// SMOOTH LAYER HAND-OFF (Josef's paper-sheet find): a
				// binary per-vertex layer choice pleats the surface into
				// an accordion of thin vertical sheets wherever the
				// cover's raster boundary jitters texel to texel - under
				// roofs, beside posts and benches, on deep shells. The
				// deep result blends in over one peel tolerance instead
				// of switching, so the shell stays one smooth surface
				// through every hand-off.
				cone = lerp(cone, coneDeep, smoothstep(PeelTol, PeelTol * 2.0, top1 - worldBase.z));
			}
			// WIDTH FAILSAFE, take 2 (Josef's "peak rounded shape" spec):
			// the dome keeps the FILLET shape always, but its RADIUS
			// freezes at the feature's own CREST - the moment the rolls
			// from both edges meet in the middle, growth stops whatever
			// the depth slider says; at ratio 1 the frozen shape is the
			// perfect half-dome exactly filling the width. The crest is
			// the feature's maximum cone, approximated by a tap ring at
			// half the roll radius: features narrow enough to saturate
			// have their crest within reach, wider ones read large values
			// and pass unclamped (their classic fillet is untouched).
			// Round 13's local-cone envelope warped the whole profile
			// into a cone-follower - pyramids at strict ratio.
			float crest = cone;
			{
				float tapR = 0.5 * coneSeed;
				float tapD = tapR * 0.7071;
				crest = max(crest, ObjectConeDepth(worldBase.xy + float2(tapR, 0.0)));
				crest = max(crest, ObjectConeDepth(worldBase.xy - float2(tapR, 0.0)));
				crest = max(crest, ObjectConeDepth(worldBase.xy + float2(0.0, tapR)));
				crest = max(crest, ObjectConeDepth(worldBase.xy - float2(0.0, tapR)));
				crest = max(crest, ObjectConeDepth(worldBase.xy + float2(tapD, tapD)));
				crest = max(crest, ObjectConeDepth(worldBase.xy - float2(tapD, tapD)));
				crest = max(crest, ObjectConeDepth(worldBase.xy + float2(tapD, -tapD)));
				crest = max(crest, ObjectConeDepth(worldBase.xy - float2(tapD, -tapD)));
			}
			float hEff = max(min(coneSeed, PileHeightRatio * crest), kMinSkinLift);
			lipHeight = hEff;
			heightScale = hEff / coneSeed;
			rollT = saturate(cone / hEff);
			// DOME SHADING (Josef: lee flanks must go dark like the
			// landscape shell's). Analytic surface normal from the cone
			// gradient through the fillet's slope, clamped near the
			// vertical rim; taps clamped to the seed so the window-edge
			// sentinel cannot poison the gradient.
			{
				const float gs = 4.0;
				float cXP = min(ObjectConeDepth(worldBase.xy + float2(gs, 0.0)), coneSeed);
				float cXN = min(ObjectConeDepth(worldBase.xy - float2(gs, 0.0)), coneSeed);
				float cYP = min(ObjectConeDepth(worldBase.xy + float2(0.0, gs)), coneSeed);
				float cYN = min(ObjectConeDepth(worldBase.xy - float2(0.0, gs)), coneSeed);
				float2 coneGrad = float2(cXP - cXN, cYP - cYN) / (2.0 * gs);
				float rimIn0 = 1.0 - rollT;
				float dhdc = rimIn0 / max(sqrt(saturate(1.0 - rimIn0 * rimIn0)), 0.2);
				domeNormal = normalize(float3(-coneGrad * dhdc, 1.0));
				domeConeGrad = coneGrad;
				domeHeightGrad = coneGrad * dhdc;
			}
			// MELD WALL (Josef's gap-close sketch): the shell is displaced
			// mesh geometry, so nothing can span the physical void between
			// two co-planar objects - but the meshes' own SIDE FACES can
			// stand in. At a MELDED boundary the seed left no rim, so the
			// cone is still full at the edge; there, the side face's top
			// band (within the peel tolerance of its column top) lifts at
			// full depth too, and the slit between the two shells closes
			// behind a facing pair of vertical snow walls. A rolled
			// (rimmed) edge keeps its bare sides, so cling mode and true
			// silhouettes are untouched.
			[flatten] if (MeldPlanesSk > 0.5 && top1 > -50000.0 && worldBase.z > top1 - PeelTol)
				meldWall = smoothstep(0.85, 0.95, rollT) * heightScale;
		}
		mask = max(mask, maskBase * meldWall);
		// Snow Breakup, S4's site. AFTER the meld max, so a melded wall cannot
		// smuggle full cover back into a broken-up column, and BEFORE depth is
		// rebuilt, so depth, coverDepth and upFacing all inherit it from the
		// one multiply and stay consistent.
		mask *= breakupFactor;
		float rimIn = 1.0 - rollT;
		depth = depthBase * heightScale * sqrt(saturate(1.0 - rimIn * rimIn)) * mask;
		coverDepth = depth;
		upFacing = mask;
		// OPTION 1 (Josef, 2026-09-01): shade by the gradient of the surface
		// this shell ACTUALLY DRAWS - which is what the landscape shell does,
		// and the reason the two matched only at depth 0.
		//
		// The skin IS its source mesh displaced VERTICALLY by d(x,y). Under
		// that map the surface normal shears exactly:
		//     n' = (nx - nz*dd/dx, ny - nz*dd/dy, nz)
		// (the inverse-transpose of the displacement Jacobian). No taps, no
		// tuning - it is the true normal of the drawn geometry.
		//
		// This is the GENERAL form of the dome normal, not a swap for it:
		//   - on a flat top (n = 0,0,1) it reduces EXACTLY to (-dx, -dy, 1),
		//     the heightfield normal the landscape shell builds;
		//   - on a vertical face (nz = 0) it leaves the mesh normal ALONE.
		// That second degeneracy is why the earlier wholesale swap to
		// domeNormal failed: domeNormal is up-hemisphere by construction, so
		// it top-projected every steep rock-family shell and smeared its
		// lighting grey at distance. The shear cannot do that - it tilts in
		// proportion to nz, so it vanishes precisely where that swap broke.
		//
		// It also subsumes the roll-wall term the old domeTilt hacked in:
		// dhdc rises toward the rim, so the fillet's own gradient tilts the
		// normal outward there and the PS picks the side plane on its own.
		//
		// kSkinShadeSmooth (0) still selects the BASE this shears - the mesh
		// normal at 0, the position-averaged one at 1 - so the dial survives.
		float3 baseShade = normalize(lerp(nrmWS, smoothWS, saturate(depth / max(depthBase, 0.01)) * kSkinShadeSmooth));
		float2 shadeGrad = domeHeightGrad * mask;
		shadeNormal = normalize(float3(baseShade.xy - baseShade.z * shadeGrad, baseShade.z));
	}

	// P3 (edge-research study): SKY EXPOSURE weights the depth - the
	// literature's accumulation field, the half this pipeline never had.
	// Two terms: the baked horizontal openness (neighbouring tops shading
	// the column) for every path, and the vertical cover term (a surface
	// standing under a higher top in its OWN column takes a dusting) for
	// the S4 shell only - the graded return of the S4 sheltering whose
	// binary placement cut cliffed mid-plank; classic draws already carry
	// their own vertical shelter in the taper above. Applied to the FINAL
	// depth so the fillet, crest freeze and taper compress uniformly, and
	// thinning toward the dusting rather than zero, matching the landscape
	// shell's under-roof rule. The casters share this path, so shadow and
	// shape stay one surface.
	[branch] if (SkyExposureSk > 0.001 && HasObjectTop > 0.5 && LegacySkin < 0.5 && depth > 0.001)
	{
		float open = SampleSkyOpenness(worldBase.xy);
		[flatten] if (ProjPixelEnable > 1.5)
		{
			float coverTop = PatchTop(worldBase.xy);
			[flatten] if (coverTop > -50000.0)
				open = min(open, 1.0 - smoothstep(kShelterNear, kShelterFar, coverTop - worldBase.z));
		}
		float sheltered = (1.0 - open) * SkyExposureSk;
		depth = lerp(depth, min(depth, kShelterDust), sheltered);
		coverDepth = lerp(coverDepth, min(coverDepth, kShelterDust), sheltered);
		depthTarget = lerp(depthTarget, min(depthTarget, kShelterDust), sheltered);
	}

	SkinLift o;
	o.WorldAbs = worldBase + liftWS * depth;

	// P5 (edge study), the cheap form that fits the skin we actually ship:
	// THE CORNICE LIP. A displaced skin can never overhang, because its
	// vertices ARE the object's vertices - the snow's outline is forced to be
	// the object's outline, and the roll has nowhere to go but inward. That is
	// why the edge reads as paint rather than as snow however well the fillet
	// is tuned. Pushing the rim band OUTWARD along the cone gradient as well as
	// up bulges the outermost ring past the silhouette, which is the whole
	// shape a cornice is.
	//
	// The weight vanishes at BOTH ends deliberately. In the interior there is
	// no edge to overhang. At the exact rim the depth is zero, and a flange
	// pushed out at ground level would z-fight the terrain. What is left is the
	// steep mid-fillet - exactly where a real cornice's lip sits.
	//
	// The study's own warning is triangle inversion at concave rims, so the
	// throw is a fraction of the depth and dies with it: a rim that grew no
	// snow cannot move at all.
	// BOTH THE DIRECTION AND THE PARAMETER COME FROM THE SMOOTHED NORMAL.
	// Everything raster-derived shattered here, and for one reason: near a rim
	// the cone goes to zero and its gradient is dominated by 4-unit
	// quantization, so NORMALIZING it amplified noise into wildly varying
	// directions and adjacent triangles splayed - Josef's bush of shards. The
	// magnitude had the same disease, a bump over a quantized raster top,
	// which flipped ordering between neighbours and folded the surface.
	//
	// The smoothed normal is smooth BY CONSTRUCTION - it is the shading normal
	// the shell already trusts - and on a rounded object its horizontal part
	// points outward exactly where the surface turns over the edge. That IS
	// the cornice band, so the same quantity gives both where to push and
	// which way, with no raster in the loop at all.
	//
	// Tilt is |horizontal part|: 0 on a flat top, about 0.7 where the surface
	// rolls over a rim, 1 on a vertical face. Push the turnover band only -
	// nothing on the top, which keeps its rounded approach, and nothing on the
	// face below, which is what hung the striped curtain down the rock last
	// round. The surface returns inward on its own beneath the widest point.
	[branch] if (ObjCorniceLip > 0.001 && depth > 0.01)
	{
		float2 outXY = smoothWS.xy;
		float tilt = length(outXY);
		[flatten] if (tilt > 0.05)
		{
			// ONE SMOOTH PEAK, no plateau. The previous band rose fast, held flat
			// between 0.55 and 0.75, then fell fast - and a plateau in the throw
			// is a flat disc in the geometry, which is the mushroom cap. A single
			// peak with no flat section gives a profile that curves the whole way
			// over instead of jutting out and hooking.
			float u = saturate((tilt - 0.05) / 0.90);
			float band = 4.0 * u * (1.0 - u);
			float2 outDir = outXY / tilt;
			o.WorldAbs.xy += outDir * (ObjCorniceLip * lipHeight * band);

			// THE SIDE PROJECTION, by the same lever the roll walls use. The
			// overhang is real geometry turning past vertical, but the shading
			// normal still points up there, so the two-plane pick stays on the
			// top plane and the texture smears down the face. Leaning the shade
			// normal outward across the band engages the side plane exactly where
			// the surface is steep - a perturbation confined to the band, never a
			// replacement, since the plane pick rides this normal.
			shadeNormal = normalize(lerp(shadeNormal, float3(outDir, 0.0),
				saturate(band * 0.65)));
		}
	}

#if !defined(SHADOWCAST)
	// C0 CONTAINER SPIKE (CONTAINER-SHELL-PLAN). Every vertex of the draw
	// rises by the SAME constant - no field is sampled to place it, so no
	// two vertices can disagree about height and the pleat is structurally
	// impossible. What we draw is no longer the snow surface, it is a box
	// that CONTAINS it; the pixel shader marches the real surface inside.
	// Deliberately outside every mask: a container with holes in it lets
	// the ray miss snow that is really there. The caster is excluded, so
	// shadows keep the old shape rather than becoming a solid block.
	[flatten] if (ContainerSpike > 0.5 && ProjPixelEnable > 1.5)
	{
		// THE LID MUST CLEAR THE FIELD, NOT THE MESH. v2 raised each vertex
		// a constant above ITS OWN position, so the container was the
		// object's FACETED MESH translated upward - while the snow surface
		// it is supposed to enclose comes from the 4-unit raster, max-of-4
		// dilated. Two different surfaces, disagreeing by several units.
		// Wherever a facet sagged more than the margin below the raster's
		// top, the ray STARTED BELOW the snow and the pixel missed, and
		// that sign flips facet by facet - the triangles in the
		// coverage-alpha view, which in container mode IS the hit/miss
		// mask. It also explains triangles at depth 0, where there is no
		// dome to pleat and only a mesh-versus-raster mismatch is left.
		//
		// Anchoring the lid to PatchTop - the same source ContainerSnowZ
		// reads - makes the clearance exact everywhere and takes the mesh's
		// faceting out of the decision entirely. The vertex's own height is
		// kept as a floor so nothing sinks into the object it covers.
		float containerH = ContainerHeight();
		float lidBase = max(worldBase.z, PatchTop(worldBase.xy));
		o.WorldAbs = float3(worldBase.xy, lidBase + containerH);
		o.Depth = containerH;
		o.CoverDepth = containerH;
	}
#endif
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

#if defined(VSHADER) && !defined(PATCH)
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

#if defined(SHADOWCAST)
// Depth-only shadow caster VS (sun cascade injection): the FULL lift
// math - the caster must be the exact surface the visible shell renders
// or the shadow offsets from its own snow - but none of the shading
// interpolants. During the caster pass ShellCB carries the light's clip
// matrix in CameraViewProj with ShellCameraPosAdjust zeroed (absolute-
// world rendering, same contract as the landscape shell's caster), so
// the standard position chain lands in light clip untouched. The
// distance collapse RUNS here, measured from the height window's centre
// (ApplySkinLift's SHADOWCAST branch): the zeroed camera adjust cannot
// be the reference (it reads "80 km away" and flattens everything), but
// skipping the collapse outright kept casters at full height past the
// skin range - full shadows over shells the eye no longer sees.
float4 main(VS_INPUT input) : SV_POSITION
{
	SkinVertex v = BuildSkinVertex(input);
	SkinLift lift = ApplySkinLift(v.WorldBase, v.NormalWS, v.SmoothWS, v.Flat, v.VertexAlpha);
	float3 rel = lift.WorldAbs - ShellCameraPosAdjust.xyz;
	return mul(CameraViewProj, float4(rel, 1.0));
}
#elif !defined(SNOW_TESS)
VS_OUTPUT main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	SkinLift lift = ApplySkinLift(v.WorldBase, v.NormalWS, v.SmoothWS, v.Flat, v.VertexAlpha);

	float3 rel = lift.WorldAbs - ShellCameraPosAdjust.xyz;
	float3 prevRel = lift.WorldAbs - ShellCameraPreviousPosAdjust.xyz;

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
	vsout.WorldPos = rel;
	// S4 draws shade by the analytic dome normal (the dome's shape lives
	// in the cone field, invisible to the mesh normals).
	vsout.NormalWS = ProjPixelEnable > 1.5 ? lift.ShadeNormal : SkinShadingNormal(v.NormalWS, v.SmoothWS, v.Flat, lift.Depth, lift.RimT);
	// raw normal Z, interpolated; the PS runs the up-facing smoothstep per
	// pixel. Thresholding here makes low-poly rocks flip whole FACES between
	// snowed and bare; thresholding the interpolated normal varies smoothly.
	// Debug view: smuggle the two lift masks through the shading interpolants.
	// Mode 5 pairs the current up-facing mask with vanilla's reconstructed
	// projection mask so one screenshot says whether the authored vertex
	// alpha carries information the normal test lacks (SKIN-PLACEMENT-PLAN).
	// Mode 6 (Shell Layers): which peeled plane owned the vertex + the depth
	// it was granted, the two questions every S4 report reduces to.
	[flatten] if (StaticsDebugView > 5.5)
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
float EdgeTessFactor(float3 worldA, float3 worldB, float collapseEnd)
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
	// Retire subdivision over the geometry range, reaching no subdivision at
	// the distance where the layer itself has collapsed.
	float rangeFade = 1.0 - smoothstep(0.0, collapseEnd, dist);
	return clamp(length(worldA - worldB) / targetLen * rangeFade, 1.0, 16.0);
}

TessFactors PatchConstants(InputPatch<TessControlPoint, 3> patch)
{
	TessFactors f;
	// Class is a per-mesh constant, so any control point answers for the patch.
	float collapseEnd = SkinCollapseEnd(max(lerp(RoundedDepth, ObjectsDepth, patch[0].Flat), kMinSkinLift));
	// Tri-domain edge order: edge i is opposite control point i.
	f.Edge[0] = EdgeTessFactor(patch[1].WorldBase, patch[2].WorldBase, collapseEnd);
	f.Edge[1] = EdgeTessFactor(patch[2].WorldBase, patch[0].WorldBase, collapseEnd);
	f.Edge[2] = EdgeTessFactor(patch[0].WorldBase, patch[1].WorldBase, collapseEnd);
	f.Inside = max(max(f.Edge[0], f.Edge[1]), f.Edge[2]);
	return f;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")]
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
	// Same S4 dome-normal selection as the untessellated VS.
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
	[flatten] if (StaticsDebugView > 5.5)
	{
		// Mode 6: identical encoding to the untessellated VS.
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
	float Depth : SV_Depth;
#	endif
};

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

// SHELL-SURFACE SSS RE-MARCH, ported verbatim from SnowShell.hlsl (Josef
// 2026-09-01: both shells must run the same shadow configuration). The only
// change is SampleTerrainStatics for SampleTerrain - the same window, the same
// bilinear, this shader's own accessor.
//
// The precomputed SSS mask describes only the BURIED surface: it is marched on
// pre-shell depth, so no gate can make it mean anything about the snow on top.
// This marches the same depth buffer from the SKIN surface and admits an
// occluder only if it stands above the snow line at its own footprint.
float SkinRemarchSSS(float3 relPos, float3 L, float noise, float2 dynRes, bool thicknessWindow, float casterCap)
{
	const float kOccluderThickness = 48.0;
	static const float kRemarchStep[8] = { 6.0, 13.0, 23.0, 38.0, 60.0, 92.0, 140.0, 210.0 };
	float occl = 0.0;
	[unroll] for (uint i = 0; i < 8; i++)
	{
		float3 sampleRel = relPos + L * (kRemarchStep[i] * (0.7 + 0.6 * noise));
		float4 clip = mul(CameraViewProjUnjittered, float4(sampleRel, 1.0));
		[branch] if (clip.w > 1.0)
		{
			float2 uv = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
			[branch] if (all(uv > 0.0) && all(uv < 1.0))
			{
				int2 px = int2(uv * dynRes * SharedData::BufferDim.xy);
				float occZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(px, 0)));
				[branch] if (occZ < clip.w - 1.0 && (!thicknessWindow || occZ > clip.w - kOccluderThickness))
				{
					float3 occRel = sampleRel * (occZ / max(clip.w, 1e-3));
					float2 occLocal = occRel.xy + ShellCameraPosAdjust.xy - GridOrigin;
					float3 st = SampleTerrainStatics(occLocal);
					float snowTop = st.x + max(st.y, 0.0);
					float occH = occRel.z + ShellCameraPosAdjust.z - snowTop;
					[flatten] if (occH > 2.0 && occH < casterCap)
						occl = max(occl, 1.0 - float(i) * 0.045);
				}
			}
		}
	}
	return 1.0 - occl;
}

PS_OUTPUT main(VS_OUTPUT input)
{
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	float3 normalWS = normalize(input.NormalWS);

	// C0 CONTAINER SPIKE: find the snow surface INSIDE the container, per
	// pixel. The rasterised position is the container's lid, not a surface
	// - march the view ray down from it until it crosses ContainerSnowZ,
	// then move this pixel's whole shading context to the crossing point.
	// Everything downstream reads WorldPos and GridLocal, so relocating
	// those two re-points the entire shader at the true surface, and the
	// normal comes from the field's own gradient rather than a mesh.
	//
	// A miss does NOT discard here: killing pixels before the gradient
	// operators further down would leave the quad's derivatives undefined.
	// It records the miss and zeroes coverage at the gate instead.
	bool containerHit = false;
	bool containerMode = false;
#if !defined(PATCH)
	[branch] if (ContainerSpike > 0.5 && ProjPixelEnable > 1.5)
	{
		containerMode = true;
		float3 rayDir = normalize(input.WorldPos);
		float3 originAbs = input.WorldPos + ShellCameraPosAdjust.xyz;
		float containerH = ContainerHeight();
		// Span: enough to traverse the container vertically at this ray's
		// pitch, bounded by a fixed WORLD reach rather than a multiple of
		// the height. The v1 cap of 8x height was Josef's view-dependent
		// hole: at low camera angles the true crossing lies further along
		// the ray than the cap reached, so lowering the camera GREW the
		// miss region over the dome's top.
		float span = min(containerH / max(abs(rayDir.z), 0.02), 320.0);
		const int kContainerSteps = 48;
		float stepLen = span / (float)kContainerSteps;
		float prevT = 0.0;
		float prevGap = originAbs.z - ContainerSnowZ(originAbs.xy);
		float hitT = -1.0;
		// Entry below the field. With the lid anchored to PatchTop this is
		// no longer a side skirt over a flank (v2's meaning, which had to
		// miss) - the lid has no skirts. It can now only mean the lid dipped
		// under the surface between two vertices, where the interpolation is
		// linear and the field is not. Degrade to the field directly beneath
		// the entry: a continuous surface, off by at most the dip, instead
		// of a hole or a painted lid. Rare by construction, and never a
		// facet-following pattern.
		[branch] if (prevGap <= 0.0)
		{
			float3 fallbackAbs = float3(originAbs.xy, ContainerSnowZ(originAbs.xy));
			input.WorldPos = fallbackAbs - ShellCameraPosAdjust.xyz;
			input.GridLocal = fallbackAbs.xy - GridOrigin;
			const float gsf = 4.0;
			float2 gradF = float2(
				ContainerSnowZ(fallbackAbs.xy + float2(gsf, 0.0)) - ContainerSnowZ(fallbackAbs.xy - float2(gsf, 0.0)),
				ContainerSnowZ(fallbackAbs.xy + float2(0.0, gsf)) - ContainerSnowZ(fallbackAbs.xy - float2(0.0, gsf))) / (2.0 * gsf);
			normalWS = normalize(float3(-gradF, 1.0));
			containerHit = true;
		}
		else
		{
			[loop] for (int ci = 1; ci <= kContainerSteps; ci++)
			{
				float t = stepLen * (float)ci;
				float3 p = originAbs + rayDir * t;
				float gap = p.z - ContainerSnowZ(p.xy);
				[branch] if (gap <= 0.0)
				{
					// Bisect the bracket. The v1 single linear guess left
					// the hit quantized to the step length, which banded
					// the texture into blocks at grazing views (smooth
					// from above, blocky from the side). Five halvings
					// take a ~7-unit step down to ~0.2 units.
					float lo = prevT;
					float hi = t;
					[unroll] for (int bi = 0; bi < 5; bi++)
					{
						float mid = 0.5 * (lo + hi);
						float3 pm = originAbs + rayDir * mid;
						[flatten] if (pm.z - ContainerSnowZ(pm.xy) > 0.0)
							lo = mid;
						else
							hi = mid;
					}
					hitT = 0.5 * (lo + hi);
					break;
				}
				prevT = t;
				prevGap = gap;
			}
		}
		[branch] if (hitT >= 0.0)
		{
			float3 hitAbs = originAbs + rayDir * hitT;
			input.WorldPos = hitAbs - ShellCameraPosAdjust.xyz;
			input.GridLocal = hitAbs.xy - GridOrigin;
			// Analytic normal from the field gradient - the surface knows
			// its own orientation, so the two-plane texture selection and
			// the lighting both get the truth with no special case. Step =
			// one raster texel: shorter steps read the bilinear facets
			// inside a texel instead of the field's slope.
			const float gs = 4.0;
			float2 grad = float2(
				ContainerSnowZ(hitAbs.xy + float2(gs, 0.0)) - ContainerSnowZ(hitAbs.xy - float2(gs, 0.0)),
				ContainerSnowZ(hitAbs.xy + float2(0.0, gs)) - ContainerSnowZ(hitAbs.xy - float2(0.0, gs))) / (2.0 * gs);
			normalWS = normalize(float3(-grad, 1.0));
			containerHit = true;
		}
	}
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
	[branch] if (HasObjectTop > 0.5)
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

	// Facing LOD: the interpolated normal over-reports up-ness on low-poly
	// meshes, so every flank passes the gate below and a distant rock reads as
	// solid white. geoFacing is the true face orientation - unusable near,
	// where it is constant per triangle and quantises rims into sawtooth, but
	// once a pixel spans the taper those facets are sub-pixel and it is the
	// only slope signal left. Handover scales with the taper's world length, so
	// it follows the depth and repose sliders. Capped short of 1 so
	// interpolation always contributes and facet contours stay soft.
	float coneRamp = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift) / clamp(MoundSteepness, 0.5, 3.0);
	float faceLOD = kFacingLODMax * SkinDistantBareness * smoothstep(0.5, 2.0, footprint / max(coneRamp, 1.0));
	[branch] if (faceLOD > 0.001)
	{
		// cross() handedness is not reliable here (the trench gate below takes
		// abs for the same reason); align to the shading normal before reading z.
		float geoUp = geoFacing.z * (dot(geoFacing, normalWS) < 0.0 ? -1.0 : 1.0);
		pixelCoverage = smoothstep(0.4, 0.7, lerp(input.Coverage, geoUp, faceLOD));
	}

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
	[branch] if (pdMode)
	{
		// Fallback for pixels the copy cannot answer (copy missing, or the
		// shell's silhouette overhangs past its object onto sky/ground):
		// the interpolated shading normal.
		float nzPix = normalWS.z;
		[branch] if (HasSkinNormalCopy > 0.5)
		{
			// A cleared texel (sky, or anything that never wrote normals)
			// reads (0,0); keep the interpolated fallback there rather than
			// decoding garbage - the silhouette-overhang miss suspect.
			float2 rawN = PreSkinNormals.Load(int3(input.Position.xy, 0)).xy;
			[flatten] if (abs(rawN.x) + abs(rawN.y) > 1e-4)
			{
				// Inverse of this PS's own encode: viewN = mul(CameraView, worldN).
				nzPix = mul(GBuffer::DecodeNormal(rawN), (float3x3)CameraView).z;
			}
		}
		// The footprint: vanilla's weight in FULL, noise always included -
		// the coat's pattern IS the purple, at every fill level. Superset
		// margin on the cut: the reconstruction can never be
		// pixel-identical to vanilla's (triplanar weights and the sample
		// position differ slightly) and the purple view tints right at
		// weight zero, so the cut sits a hair below - purple may only ever
		// peek through a genuine reconstruction hole.
		float3 triW = Triplanar::GetWeights(normalWS, geoFacing);
		float noise = Triplanar::SampleGrad(ProjNoiseMap, SnowSampler, projWorldPos, triW, ProjNoiseTiling, projGradX, projGradY).x;
		float wpix = nzPix * input.ProjFactor - max(ProjThreshold, 0.0) + 0.1 - ProjNoiseScale * noise;
		// Snow Fill = the ANGULAR slice of that footprint (Josef's
		// percentage spec): nz runs 1 (up) to -1 (straight down), and the
		// slider sweeps the acceptance threshold across that whole range -
		// most up-facing parts first, the midpoint covers the up-facing
		// hemisphere, the top covers every angle.
		float nzCut = 1.0 - 2.0 * ProjSnowFillSk;
		// Soft borders, take 2 (Josef: a GRADUAL fade, not dither steps).
		// The widened noisy band failed because the noise term owned the
		// fade: wpix oscillates inside the band, so alpha broke into
		// mid-level islands - his "100 -> 50 -> 0 steps". The fade
		// envelope now rides the SMOOTH half of the weight (noise
		// excluded), descending monotonically across the border, while
		// the noisy cut stays narrow and only keeps the edge ragged.
		float wSmooth = nzPix * input.ProjFactor - max(ProjThreshold, 0.0) + 0.1;
		pdCoverage = smoothstep(-0.03, 0.0, wpix) * smoothstep(-0.18, 0.08, wSmooth) * smoothstep(nzCut - 0.05, nzCut + 0.05, nzPix);
		// Match the geometry's up-facing gate per pixel: the shell's
		// material belongs to top surfaces; steep faces keep the recolor.
		// EXCEPT the meld wall: the lift raises side faces at melded
		// boundaries to close the slit between co-planar shells, and the
		// pixel gate must let them through where the cone confirms a
		// melded (unrimmed) column and the vertex actually lifted.
		float upGateP = smoothstep(ShellMinNz, ShellMinNz + 0.15, nzPix);
		[branch] if (MeldPlanesSk > 0.5 && upGateP < 0.99 && input.Lift > 0.3 * coatRef)
		{
			float2 pixXY = input.WorldPos.xy + ShellCameraPosAdjust.xy;
			float coneP = ObjectConeDepth(pixXY);
			float topP = PatchTopPoint(pixXY);
			float seedP = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
			[flatten] if (topP > -50000.0 && coneP >= seedP * 0.85)
				upGateP = 1.0;
		}
		pdCoverage *= upGateP;
		// S4 roll edge: the fillet's geometry reaches h=0 at the rim, and
		// the last sliver would shade coincident with the surface below it
		// - cut the material where the lift drops under the clearance and
		// let the recolored PD carry on underneath (the two systems agree
		// by construction, so the hand-off is a seam of height only).
		pdCoverage *= smoothstep(0.3 * coatRef, coatRef, input.Lift);
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
	else [flatten] if (ProjDensityEnable > 0.5 && ProjThreshold > -0.5)
		pixelCoverage *= smoothstep(0.06, 0.14, input.ProjFactor);

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
	// range this field has. NOT scaled by SkinDistantBareness - that tunes the
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
	// repair torn geometry. The tear is a vertex-rate disagreement; the
	// fix is the container/march architecture, see CONTAINER-SHELL-PLAN.md.)

	// C0: in container mode the march IS the coverage decision. Every gate
	// above reconstructs where snow belongs from vertex data and the
	// pre-shell G-buffer; the ray either found the surface or it did not,
	// and that answer is exact. Binary on purpose - the silhouette is
	// whatever the field's own outline says, which is the point of C0.
	[flatten] if (containerMode)
		pixelCoverage = containerHit ? 1.0 : 0.0;
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
	// Object trenching is parked until it can be done properly; roads keep
	// theirs, since theirs is the tuned case.
#ifdef PATCH
	// The patch only has texels where the VS already permitted carving, so the
	// per-pixel gate would only re-ask a settled question. Constant here so
	// StaticsCB.ObjectTrenches can carry the REAL setting for the VS, which
	// needs it to tell a road-owned column from raster bleed.
	bool carveObject = true;
#else
	bool carveObject = ObjectTrenches > 0.5 || LegacySkin > 0.5;
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
	[branch] if (RoadField > 0.5 && RoundedDepth > 1.0 && RoadOwnsColumn(worldXY))
		discard;

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
	[branch] if (HasSnowHeight > 0.5 && groundData.x > -50000.0 && groundData.y > 0.5 && pixelDist < 2048.0)
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

	// Guaranteed snow floor in object trenches; the statics-skin mirror of
	// the landscape shell's trench floor: a carved, solidly-covered pixel
	// must never dissolve to the object's own texture, whatever the seam
	// blends above decided.
	coverageAlpha = max(coverageAlpha, smoothstep(0.15, 0.5, pixelDeform) * smoothstep(0.35, 0.6, pixelCoverage));

	// Floor wear: TrenchFloorFade dissolves heavily trampled floors back to
	// the object's own surface (rock, log, planks). Applied multiplicatively
	// after the floor guarantee; the coverage gates hold alpha at 1 on
	// floors, so relaxing the guarantee alone changes nothing. Up-facing
	// pixels only: the top-down map column carries the trail on flanks too,
	// and wearing those punches see-through holes in trench walls.
	[branch] if (carveObject && TrenchFloorFade > 0.001)
	{
		coverageAlpha *= 1.0 - TrenchFloorFade * smoothstep(0.45, 0.95, pixelDeform) * smoothstep(0.35, 0.65, normalWS.z);
	}

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
	// loaded distance (geometry still collapses to flat paint by
	// SkinHeightFadeEnd).
	// Held SEPARATE from the shape gates: the shape cut below is hard, and a
	// hard cut on a fade that runs over hundreds of units pops every distant
	// skin in one frame.
	float fadeAlpha = 1.0;
	[flatten] if (FadeExempt < 0.5)
		fadeAlpha = 1.0 - smoothstep(SkinFadeStart, SkinFadeEnd, pixelDist);

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
	float snowSteepness = smoothstep(0.75, 0.55, abs(normalWS.z));
#endif
	float snowWorldZAbs = input.WorldPos.z + ShellCameraPosAdjust.z;
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
	// The distance fade WITHOUT the crust flattening applied: the frost
	// crystal replacing the powder grain must not fade with it.
	const float bumpFadeRaw = bumpFade;
	// Spell marks (landscape parity): crust flattens the powder
	// grain here; albedo/polish/grazing terms follow below. Stable grid
	// position for the fetch (round-31 lesson).
	float crustAmount = saturate(SampleCrust(input.GridLocal) * SpellShading.y);
	bumpFade *= lerp(1.0, 1.0 - saturate(SpellShading.w), crustAmount);
	SnowTaps snowTaps = ComputeSnowTaps(snowUV, worldXY);
	SnowTaps snowTapsSide = ComputeSnowTaps(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = ddx(snowUVSideUnfolded);
	snowTapsSide.duvdy = ddy(snowUVSideUnfolded);
	// Uniform flow: the parallax shadow branch below is divergent, and
	// derivatives taken inside it would be garbage at its edges.
	float snowHeightMip = SnowHeightMip(snowUV);
	float snowHeightMipSide = SnowHeightMip(snowUVSideUnfolded);

	// Object trench detail: shading-only berm ridge along trails; also the
	// compaction weight's berm term. Geometry berm waits for the skin
	// rework.
	float bermC = 0.0;
	[branch] if (ObjBermHeightAmp > 0.005 || CompactLook.x > 0.001)
		bermC = BermField(trenchGridLocal);
#ifdef PATCH
	// The patch's berm is real geometry (BuildPatchVertex), shaded by the
	// vertex normal it displaced. Adding the shading ridge here too would
	// double it.
	[branch] if (false)
#else
	[branch] if (ObjBermHeightAmp > 0.005 && bermC > 0.003)
#endif
	{
		const float bStep = 4.0;
		float2 bermGrad = float2(
			BermShape(BermField(trenchGridLocal + float2(bStep, 0.0))) - BermShape(BermField(trenchGridLocal - float2(bStep, 0.0))),
			BermShape(BermField(trenchGridLocal + float2(0.0, bStep))) - BermShape(BermField(trenchGridLocal - float2(0.0, bStep)))) / (2.0 * bStep);
		float bermDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 12.0);
		// Centre-masked rather than per-tap: this berm is shading-only, and
		// the mask's job is just to keep the ridge off the dug floor.
		normalWS = normalize(normalWS + float3(-bermGrad * saturate(1.0 - pixelDeform) * bermDepth * ObjBermHeightAmp * BermDepthGate(bermDepth), 0.0));
		// P6 clods, shading-only like this whole ridge (geometry berm
		// waits for the skin rework); same weight recipe as the landscape.
		[branch] if (RimStyle.z > 0.01)
		{
			float2 clodXY = GridOrigin + trenchGridLocal;
			float kXP = ChurnNoiseScaled(clodXY + float2(bStep, 0.0), kClodSizeScale);
			float kXN = ChurnNoiseScaled(clodXY - float2(bStep, 0.0), kClodSizeScale);
			float kYP = ChurnNoiseScaled(clodXY + float2(0.0, bStep), kClodSizeScale);
			float kYN = ChurnNoiseScaled(clodXY - float2(0.0, bStep), kClodSizeScale);
			normalWS = normalize(normalWS + float3(-float2(kXP - kXN, kYP - kYN) / (2.0 * bStep) *
				RimStyle.z * BermShape(bermC) * saturate(1.0 - pixelDeform) * BermDepthGate(bermDepth), 0.0));
		}
	}

#ifdef PATCH
	// Churn shading at PIXEL rate, the landscape PS's own recipe (weight =
	// ChurnWeight x depth/10). Not in the vertex normal: at ChurnSize 0.25
	// the lumps sit at 4/1.75 units, under even the dense band's vertex
	// spacing, and the interpolated gradient shaded trampled floors as
	// pristine top snow. Geometry keeps its coarse displacement; the normal
	// carries the look, as with the landscape's dunes.
	// UNCARVED depth, exactly as the landscape weighs it (its pixelDepth is
	// the ramp depth, not the carve): trench floors shade at full churn
	// whenever the layer is deep enough to churn at all. The carved depth
	// here read floors at ~30% of the landscape's - Josef's "boosted" gap.
	[branch] if (ObjChurnHeightAmp > 0.01)
	{
		float churnWPix = ChurnWeight(pixelDeform, bermC) * saturate(PatchSkinDepth(worldXY).x / 10.0);
		[branch] if (churnWPix > 0.001)
		{
			const float cStep = 3.0;
			float2 churnGradPix = float2(
				ChurnNoise(worldXY + float2(cStep, 0.0)) - ChurnNoise(worldXY - float2(cStep, 0.0)),
				ChurnNoise(worldXY + float2(0.0, cStep)) - ChurnNoise(worldXY - float2(0.0, cStep))) / (2.0 * cStep);
			normalWS = normalize(normalWS + float3(-churnGradPix * ObjChurnHeightAmp * churnWPix, 0.0));
		}
	}
#endif

	// Tangent basis for the TOP projection's uv axes (see SnowShell.hlsl).
	// Built from the geometric normal before the normal map perturbs it, and
	// shared with the parallax shadow below.
	float3 bumpT = normalize(cross(float3(0.0, 1.0, 0.0), normalWS) + float3(1e-5, 0.0, 0.0));
	float3 bumpB = cross(normalWS, bumpT);

	float3 V = -normalize(input.WorldPos);

	// Pre-parallax derivatives for the glint grid (Lighting.hlsl's uvOriginal
	// pattern; see SnowShell.hlsl): the POM offset is view-dependent and
	// glints must not ride it. The uv itself is rebuilt world-anchored below.
	const float2 glintDuvdx = snowTaps.duvdx;
	const float2 glintDuvdy = snowTaps.duvdy;

	// Parallax occlusion, same marcher the landscape shell uses (shared in
	// SnowParallax.hlsli, so the two cannot drift). Object snow needs it in
	// BOTH projections, and unlike SampleSnowPlanar the two cannot share one
	// march: each projection has its own uv axes, so the view resolves to a
	// different 2D direction in each and the offsets are not interchangeable.
	// Each plane therefore marches itself and shifts its OWN tap set; the
	// existing sample blend then mixes them exactly as before.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.z > 0.001 && bumpFade > 0.001)
	{
		DisplacementParams pomParams = SnowDisplacementParams();
		pomParams.HeightScale *= SnowParallax.z;

		// Top plane: bumpT/bumpB ARE its uv axes.
		float3x3 tbnTop = float3x3(bumpT, bumpB, normalWS);
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
			float3 sideN = normalize(snowSideDropsX ? float3(normalWS.x, 0.0, 0.0) : float3(0.0, normalWS.y, 0.0));
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
	{
		float3 texN = SampleSnowPlanar(SnowNormalMap, snowTaps, snowTapsSide, snowSteepness).xyz * 2.0 - 1.0;
		texN.z = sqrt(saturate(1.0 - dot(texN.xy, texN.xy)));
		texN.y = -texN.y;
		normalWS = normalize(normalWS + (bumpT * texN.x + bumpB * texN.y) * bumpFade);
	}
	else if (HasSnowTexture != 0 && bumpFade > 0.001)
	{
		const float kBumpTile = 64.0;
		const float kBumpHeight = 0.55;
		float2 texDims;
		SnowDiffuse.GetDimensions(texDims.x, texDims.y);
		float e = 1.5 / texDims.x;
		float2 detailUV = worldXY / kBumpTile;
		const float3 kLum = float3(0.30, 0.45, 0.25);
		float h0 = dot(SnowDiffuse.Sample(SnowSampler, detailUV).rgb, kLum);
		float hx = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(e, 0.0)).rgb, kLum);
		float hy = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(0.0, e)).rgb, kLum);
		float2 bumpGrad = float2(hx - h0, hy - h0) * (kBumpHeight / (e * kBumpTile));
		normalWS = normalize(normalWS + float3(-bumpGrad * bumpFade, 0.0));
	}

	// Frost crystal (landscape recipe): the pattern normal rides bumpFadeRaw
	// so the crystal survives the crust's flattening of the powder grain.
	FrostTaps frost;
	frost.normal = float3(0.0, 0.0, 1.0);
	frost.crystal = 0.0;
	frost.valid = false;
	const float frostAmount = (CrustLook2.w > 0.5) ? crustAmount * saturate(CrustLook2.y) : 0.0;
	[branch] if (frostAmount > 0.001)
	{
		frost = SampleFrostPattern(worldXY, CrustLook2.z);
		normalWS = normalize(normalWS +
		                     (bumpT * frost.normal.x + bumpB * frost.normal.y) * frostAmount * bumpFadeRaw);
	}

	float3 viewNormal = normalize(mul((float3x3)CameraView, normalWS));

	// Snow material; same albedo path as the terrain shell.
	float3 kSnowAlbedo = float3(0.82, 0.84, 0.88);
	[branch] if (HasSnowTexture != 0)
	{
		kSnowAlbedo = SampleSnowPlanar(SnowDiffuse, snowTaps, snowTapsSide, snowSteepness).rgb;
		[flatten] if (SnowTextureIsLinear != 0.0)
			kSnowAlbedo = Color::LinearToSrgb(kSnowAlbedo);
	}
	// Compaction weight (Stage 1), shared constant with the terrain shell;
	// feeds the glint suppression alone (the darken/roughen halves were
	// retired - IBL + DALC already darken trenches).
	float churnMat = ChurnWeight(pixelDeform, bermC);

	// Spell marks on the albedo; the landscape recipes verbatim.
	{
		float scorch = SampleScorch(input.GridLocal) * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
	[branch] if (crustAmount > 0.001)
		kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(CrustLook.y, CrustLook.z, CrustLook2.x), crustAmount);
	[branch] if (frost.valid)
		kSnowAlbedo *= lerp(1.0, lerp(0.94, 1.0, frost.crystal), frostAmount);

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

	// Crust polishes whatever the material ended up being; after the RMAOS
	// block or an installed map silently discards it (landscape lesson).
	[branch] if (crustAmount > 0.001)
	{
		snowRoughness = lerp(snowRoughness, SpellShading.z, crustAmount);
		snowF0 = lerp(snowF0, CrustLook.xxx, crustAmount);
	}
	[branch] if (frost.valid)
	{
		snowRoughness = saturate(snowRoughness * lerp(1.0, lerp(1.35, 0.45, frost.crystal), frostAmount));
		snowF0 = snowF0 * lerp(1.0, lerp(0.75, 1.7, frost.crystal), frostAmount);
	}

	float3 L = SharedData::DirLightDirection.xyz;
	float satNdotL = saturate(dot(normalWS, L));
	float satNdotV = saturate(abs(dot(normalWS, V)) + 1e-5);

	float worldShadow = ShadowSampling::GetWorldShadow(input.WorldPos, ShellCameraPosAdjust.xyz);
	// Hoisted above the cascade call: the crisp path widens its PCF ring with
	// distance exactly as the terrain shell does, or the far cascade's texels
	// quantise into blocky patches on object snow while the ground beside it
	// shows soft penumbra.
	float farShadowT = smoothstep(6000.0, 15000.0, pixelDist);
	float sunShadow;
	[branch] if (CrispShadows > 0.5)
	{
		// Full-resolution comparison PCF; same path as the terrain shell.
		// (the round-31 seamShadowLift receiver raise is REVERTED -
		// the RenderDoc replay proved no cascade shadow was missing at the
		// seam, so the lift only risked boundary drift.)
		sunShadow = worldShadow * SnowShadow::GetCascadeShadow(input.WorldPos, normalWS, lerp(1.0, 6.0, farShadowT), uint2((uint)BorderStyle.z, (uint)BorderStyle.w));
	}
	else
	{
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(input.WorldPos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}
	// Heightfield self-shadowing, the landscape shell's 5-tap horizon march
	//: hills, berms and drift rims cast the same soft shadows onto
	// object snow as onto the ground beside it, and a trench's own rim
	// darkens its interior. Same tap ring, same carved-surface rule; the
	// melt term reads the wide exclusion field alone (no near mask bound
	// here). Object tops from the skin's own raster window join the horizon.
	// March diagnostics for debug view 4: x = how much the march darkened
	// this pixel, y = fraction of taps that rebuilt the road's carved
	// surface, z = fraction that used the flat dusting. Ran = the guard
	// below passed at all (a pixel the cascades already darkened, or a sun
	// too low, never marches - the view paints those dim magenta so "march
	// skipped" cannot be misread as "march found nothing").
	float3 dbgMarch = float3(0.0, 0.0, 0.0);
	float dbgMarchRan = 0.0;
	[branch] if (sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		// Redistributed toward the NEAR field. The first tap set the finest
		// boundary the horizon can resolve, so at 28 units every shadow edge
		// was smeared over at least that distance and the softness below had
		// to be wide enough to hide it. Same tap COUNT, same 1000-unit reach.
		static const float kMarchDist[5] = { 12.0, 32.0, 90.0, 300.0, 1000.0 };
		float sunLen2D = max(length(L.xy), 1e-4);
		float sunTan = L.z / sunLen2D;
		float2 stepDir = L.xy / sunLen2D;
		float surfZ = input.WorldPos.z + ShellCameraPosAdjust.z;
		float horizonTan = -10.0;
		// The top raster stores only the HIGHEST surface per texel, so under
		// a multi-level object's overhang it records the deck ABOVE the
		// receiver and every tap reads "inside a hill" — full shadow in
		// raster-texel steps on surfaces plainly in the sun. The raster
		// cannot see under
		// roofs: where it stands well above the surface being shaded, drop
		// its term and let the cascades/SSS own the shading here.
		// The S4 skip is LIFTED. With the object taps off, skins took no
		// heightfield occlusion at all (Josef, debug view 4: every skin black,
		// the road patch green) while the landscape march occluded the ground
		// beside them at full depth - so objects read brighter than the snow
		// around them. The blockiness that motivated the skip was the POINT
		// load, fixed below, not the taps.
		bool objectTopUsable = HasObjectTop > 0.5;
		[branch] if (objectTopUsable)
		{
			float2 selfLocal = (GridOrigin + input.GridLocal - HeightWindowCenter) / HeightHalfExtent;
			[flatten] if (all(abs(selfLocal) < 0.98))
			{
				float2 topDims;
				ObjectTopRaw.GetDimensions(topDims.x, topDims.y);
				float2 selfUV = float2(selfLocal.x * 0.5 + 0.5, 0.5 - selfLocal.y * 0.5);
				float selfTop = ObjectTopRaw.Load(int3((int2)clamp(selfUV * topDims, 0.0, topDims - 1.0), 0));
				// 6 units: about one raster texel above the floor.
				// On normal tops the raster sits at or below the lifted skin
				// surface (selfTop - surfZ is negative by the skin depth),
				// so a small positive margin only fires under genuine upper
				// decks; 32 missed low ledges, 12 still missed some.
				// FLOOR: the raster is 4-unit texels holding the HIGHEST
				// surface per texel, so on a steep facet a tap can legitimately
				// read a few units above its receiver. Below ~5 the guard
				// starts firing on that quantisation alone and object tops
				// stop shadowing themselves at all — do not go lower without
				// a finer raster.
				if (selfTop > -50000.0 && selfTop > surfZ + 6.0)
					objectTopUsable = false;
			}
		}
		[unroll] for (uint marchI = 0; marchI < 5; marchI++)
		{
			float d = kMarchDist[marchI];
			float2 sampleLocal = input.GridLocal + stepDir * d;
			float sh = -100000.0;
			bool tapOnObject = false;
			[branch] if (objectTopUsable)
			{
				float2 topLocal = (GridOrigin + sampleLocal - HeightWindowCenter) / HeightHalfExtent;
				[flatten] if (all(abs(topLocal) < 0.98))
				{
					float2 topDims;
					ObjectTopRaw.GetDimensions(topDims.x, topDims.y);
					float2 topUV = float2(topLocal.x * 0.5 + 0.5, 0.5 - topLocal.y * 0.5);
					float topH = ObjectTopRaw.Load(int3((int2)clamp(topUV * topDims, 0.0, topDims - 1.0), 0));
					[flatten] if (topH > -50000.0)
					{
						// Inside an object's footprint the surface is its top
						// plus a skin dusting. The terrain window's class-ramp
						// surface does not exist here: marching against it
						// fabricated a snow slab a class depth above every
						// skin, and the round-35 top term then stacked the
						// ramp on the top as well - object snow fell into
						// shadow at any low sun.
						//
						// Occluder = the CONE field, not a fixed dusting. A flat 2.0
						// modelled every dome as 2 units tall, so skins never
						// self-shadowed at their real height while the landscape march
						// occluded the ground beside them at FULL depth. ObjectSkinDepth
						// is unusable here (the capture parks it at 0 for every non-road
						// object); the cone is the same angle-of-repose field the dome's
						// own taper reads.
						float2 tapWorld = GridOrigin + sampleLocal;
						// Clamped to the deepest class in play: the cone raster returns
						// a huge sentinel outside its window, and an unclamped occluder
						// would put the whole scene in shadow.
						float tapConeRaw = ObjectConeDepth(tapWorld);
						float tapSnow = clamp(tapConeRaw, kMinSkinLift, max(max(RoundedDepth, ObjectsDepth), kMinSkinLift));
						sh = topH + tapSnow;
						dbgMarch.z += 0.2;

						// Except on ROAD-OWNED columns, which carry a full
						// carved layer: a dusting occluder leaves the trench
						// floor unshadowed by its own walls - the bright
						// streak down every road trail. Rebuild the surface
						// the patch draws, from BILINEAR reads: attempt one
						// (reverted) fed the point-Load top and the max-of-4
						// depth into the carve and the occluder stepped in
						// 4-unit texels, which read as blocky shadows.
						// Bilinear over the same lattice is the smoothness
						// class of the patch's own drawn geometry. All four
						// top texels must be valid (a sentinel poisons the
						// interpolation) and the road must own the column;
						// everywhere else - rocks, cairns, walls - the
						// dusting above stands, so skins cannot regress.
						float2 bt = PatchTexel(tapWorld, topDims);
						int2 bt0 = (int2)bt;
						float2 btf = bt - bt0;
						int2 bt1 = min(bt0 + 1, int2(topDims) - 1);
						float4 tapTops = float4(
							ObjectTopRaw.Load(int3(bt0.x, bt0.y, 0)), ObjectTopRaw.Load(int3(bt1.x, bt0.y, 0)),
							ObjectTopRaw.Load(int3(bt0.x, bt1.y, 0)), ObjectTopRaw.Load(int3(bt1.x, bt1.y, 0)));
						[branch] if (all(tapTops > -50000.0))
						{
							float2 sd00 = ObjectSkinDepth.Load(int3(bt0.x, bt0.y, 0));
							float2 sd10 = ObjectSkinDepth.Load(int3(bt1.x, bt0.y, 0));
							float2 sd01 = ObjectSkinDepth.Load(int3(bt0.x, bt1.y, 0));
							float2 sd11 = ObjectSkinDepth.Load(int3(bt1.x, bt1.y, 0));
							float topSmooth = lerp(lerp(tapTops.x, tapTops.y, btf.x), lerp(tapTops.z, tapTops.w, btf.x), btf.y);
							float depthSmooth = lerp(lerp(sd00.x, sd10.x, btf.x), lerp(sd01.x, sd11.x, btf.x), btf.y);
							float tapRoadTop = max(max(sd00.y, sd10.y), max(sd01.y, sd11.y));
							[branch] if (tapRoadTop > kNoRoadTop * 0.5 && (topSmooth - tapRoadTop) < kRoadOwnsTop && depthSmooth >= 1.0)
							{
								// Same assembly as the off-object branch below:
								// carve + berm, undulation riding on the result;
								// churn and clods skipped, as both marches skip
								// them. Baked berm only, the march's own
								// convention - the 17-tap live field is not
								// worth 5 taps of it per pixel.
								// Same verge blend as BuildPatchVertex, or the
								// occluder regrows the walls the geometry
								// tapered - the recurring shape/shadow split.
								float tapLand = max(SampleTerrainStatics(sampleLocal).y, 0.0);
								float tapCone = tapConeRaw;
								depthSmooth = max(min(tapCone, depthSmooth), min(tapLand, depthSmooth));
								float tapDeform = SampleDeformation(sampleLocal);
								float tapBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : 0.0;
								float tapDepth = CarveProfile(tapDeform, depthSmooth, tapWorld) +
								                 BermShape(tapBerm) * saturate(1.0 - tapDeform) * depthSmooth * ObjBermHeightAmp * BermDepthGate(depthSmooth);
								// Live undulation on purpose - see SnowShell's march note.
								sh = topSmooth + tapDepth + Undulation(tapWorld) * saturate(tapDepth / 8.0);
								dbgMarch.y += 0.2;
								dbgMarch.z -= 0.2;
							}
							else
							{
								// Non-road object column: the SAME bilinear top the road path
								// uses, so the occluder is as smooth as the drawn dome instead
								// of stepping in 4-unit texels. The blockiness that got the
								// object taps disabled on S4 was the POINT load, not the taps.
								sh = topSmooth + tapSnow;
							}
						}
						tapOnObject = true;
					}
				}
			}
			[branch] if (!tapOnObject)
			{
				float3 st = SampleTerrainStatics(sampleLocal);
				float sampleDepth = max(st.y, 0.0);
				{
					float sampleMelt = saturate(SampleExclusionField(GridOrigin + sampleLocal).y);
					sampleDepth = lerp(sampleDepth, min(sampleDepth, kFireMeltFloor), sampleMelt);
				}
				float sampleDeform = SampleDeformation(sampleLocal);
				float sampleBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : 0.0;
				sampleDepth = CarveProfile(sampleDeform, sampleDepth, GridOrigin + sampleLocal) +
				              BermShape(sampleBerm) * saturate(1.0 - sampleDeform) * sampleDepth * BermHeightAmp * BermDepthGate(sampleDepth);
				// Sentinel terrain contributes a hugely negative horizon: a
				// no-op through the max below, same as the landscape's edge.
				sh = st.x + sampleDepth + Undulation(GridOrigin + sampleLocal) * saturate(sampleDepth / 8.0);
			}
			horizonTan = max(horizonTan, (sh - surfZ) / d);
		}
		// Near softness halved: it existed to hide the tap quantisation the
		// finer first taps now resolve. Far end untouched.
		float soft = lerp(0.03, 0.35, farShadowT);
		// Penumbra CENTRED on the horizon. The old band ran
		// [-0.12 - (soft-0.06)*2, +soft]: the same total width (3*soft) but
		// entirely on the LIT side of sunTan == horizonTan, so the shadow always
		// over-reached its geometric edge by h/(sunTan-soft) - h/sunTan on the
		// ground. That grows fast as the sun drops - the low-sun bleed past
		// drift crests. Width preserved exactly; the bias is gone.
		float marchFactor = lerp(smoothstep(-1.5 * soft, 1.5 * soft, sunTan - horizonTan), 1.0, 0.7 * farShadowT);
		sunShadow *= marchFactor;
		dbgMarch.x = 1.0 - marchFactor;
		dbgMarchRan = 1.0;
	}
	// Screen-Space Shadows: same long-range term bare ground multiplies in,
	// distance-blended past the cascades like the landscape shell (the SSS
	// march ran on the PREPASS depth; near, it belongs to the surface
	// UNDER the skin, and the crisp cascades already cover the skin).
	[branch] if (ScreenSpaceShadowsActive > 0.5)
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
		sunShadow *= lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0), sssBlend);
	}

	// Shell-surface re-march: the near-field counterpart to the mask above,
	// same gate, same hand-off band, so the two never double.
	[branch] if (CompactLook.y > 0.5 && ScreenSpaceShadowsActive > 0.5 &&
		SnowShadow::GetSssHandoff(input.CurrentClip.w) < 0.999 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		// Packed: integer part = mode (1 march, 2 march + thickness),
		// fraction * 1000 = the caster height cap in units.
		float remarchCap = frac(CompactLook.y) * 1000.0;
		float remarch = SkinRemarchSSS(input.WorldPos, L, screenNoise, CompactLook.zw, CompactLook.y > 1.5, remarchCap);
		sunShadow *= lerp(remarch, 1.0, SnowShadow::GetSssHandoff(input.CurrentClip.w));
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

		float parallaxShadow = 1.0 - saturate(occlusion * SnowParallax.y);
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1); same call as the terrain shell so object snow and
	// ground snow shade identically across the seam where they meet.
	// World-anchored glint uv on a static 4096-unit fold; see SnowShell.hlsl
	// for why the GridOrigin-folded snowUV re-rolled the sparkle field.
	const float2 glintUV = fmod(input.WorldPos.xy + ShellCameraPosAdjust.xy, 4096.0) / kSnowUVTile;
	// Built once, shared by the sun and every point light (M3).
	// Compaction thins the glint field toward the smooth-GGX fallback
	// (same recipe as SnowShell.hlsl; density under the 1.1 gate = no
	// glints at all).
	float4 glintParamsC = SnowGlintParams;
	glintParamsC.x = lerp(glintParamsC.x, PBR::Constants::MinGlintDensity, saturate(CompactLook.x * churnMat));
	SnowMaterialCtx snowMtl = SnowBuildMaterial(normalWS, kSnowAlbedo, snowRoughness, snowF0, snowAO,
		glintParamsC, EnableGlints, glintUV, glintDuvdx, glintDuvdy, input.Position.xy);
	SnowSunLighting sunLit = SnowEvaluateSunPBR(snowMtl, normalWS, V, input.WorldPos, ShellCameraPosAdjust.xyz, sunShadow,
		glintUV, glintDuvdx, glintDuvdy);
	float3 specularLobe = sunLit.specularLobe;
	float3 diffuseLobe = sunLit.diffuseLobe;
	float3 directDiffuse = sunLit.directDiffuse;
	float3 directSpecular = sunLit.directSpecular;

	// Ice reads at GRAZING angles (landscape recipe): the one thing white
	// snow cannot already be doing.
	[branch] if (crustAmount > 0.001)
	{
		float grazing = pow(1.0 - satNdotV, 4.0);
		directSpecular += grazing * crustAmount * CrustLook.w * SharedData::DirLightColor.xyz * sunShadow;
	}

	// Placed lights: same clustered path as the terrain shell, with each
	// shadow-casting light's own map sampled at the skin/patch surface.
	[branch] if (PointLightsActive > 0.5)
	{
		float viewZ = mul(CameraView, float4(input.WorldPos, 1.0)).z;
		float4 clip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float2 screenUV = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		SnowLights::AccumulatePointLights(snowMtl, input.WorldPos, input.WorldPos + ShellCameraPosAdjust.xyz,
			normalWS, V, viewZ, screenUV, glintUV, glintDuvdx, glintDuvdy, directDiffuse, directSpecular);
	}

	// No AO here: the routed lobes already carry it (see SnowShell.hlsl).
	float3 ambientColor = SnowAmbientColor(normalWS);
	float3 ambientPart = ambientColor * diffuseLobe;
	// The land's baked vertex AO under the object (see SnowShell.hlsl): snow
	// on a rock in a dark grove shares the grove's baked shade, and using the
	// same source as the terrain shell keeps the seam flat.
	float2 terrainLocal = (input.WorldPos.xy + ShellCameraPosAdjust.xy) - GridOrigin;
	float landVertexAO = Color::ColorToLinear(SampleTerrainVertexAO(terrainLocal).xxx).x;
	landVertexAO = lerp(1.0, landVertexAO, SharedData::truePBRSettings.VertexAOStrength);
	// Skylighting parity; same path as the terrain shell.
	[branch] if (SkylightingActive > 0.5)
	{
		sh2 skylightingSH = Skylighting::Sample(input.WorldPos, normalWS);
		float skylightingDiffuse = Skylighting::GetSkylightingDiffuse(skylightingSH, input.WorldPos, normalWS, landVertexAO);
		ambientPart = Color::IrradianceToGamma(Color::IrradianceToLinear(ambientPart) * MultiBounceAO(diffuseLobe * Color::PBRLightingScale, skylightingDiffuse));
	}
	// TruePBR G-buffer units (Lighting.hlsl:2766-2774): diffuse, specular,
	// ambient and the Albedo payload carry PBRLightingScale; the Reflectance
	// lobe does not - the composite assumes exactly this split.
	ambientPart *= Color::PBRLightingScale;
	directDiffuse *= Color::PBRLightingScale;
	directSpecular *= Color::PBRLightingScale;
	diffuseLobe *= Color::PBRLightingScale;
	float3 preLit = ambientPart + directDiffuse;

	// Debug view: decision data as flat colors. Patch: R = trample,
	// G = skin depth (packed by FinishPatchVertex). Skins: teal, brightness
	// by up-facing coverage. Absent pixels = absent geometry.
	[branch] if (StaticsDebugView != 0.0)
	{
#ifdef PATCH
		[branch] if (StaticsDebugView > 6.5)
		{
			// Lift Gradient mode. The patch is a lattice whose Lift is a
			// constant, so its gradient is zero BY CONSTRUCTION - it cannot
			// pleat and there is nothing here to measure. Dim gray, so a
			// screenshot cannot be misread as "the patch is clean too".
			preLit = float3(0.1, 0.1, 0.1);
		}
		else [branch] if (StaticsDebugView > 5.5)
		{
			// Shell Layers: which DRAW LAYER drew this patch pixel. Green =
			// layer 1 (the surface the patch has always drawn), yellow =
			// layer 2, red = layer 3 - the same colors the skin uses for its
			// peeled planes, so the two paths read as one picture.
			float drawLayer = floor(saturate(input.Coverage) * 8.0);
			preLit = drawLayer < 0.5 ? float3(0.1, 1.0, 0.1) :
			                           (drawLayer < 1.5 ? float3(1.0, 0.9, 0.1) : float3(1.0, 0.15, 0.1));
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
		[branch] if (StaticsDebugView > 6.5)
		{
			// Lift Gradient mode (S-A Tier 0): how far this pixel's LIFT
			// disagrees with its neighbours, as a fraction of the class depth.
			// A coherent dome wall spends the class depth over many pixels; a
			// sliver triangle spans it in one or two, so the screen-space lift
			// gradient IS the tell - the same quantity round 30's shred rule
			// thresholded before it was removed. Green = agreeing, amber = a
			// real slope, red = a tear. MEASUREMENT ONLY: nothing consumes it,
			// and it does not change what the shell draws.
			// Normalised by the surface's OWN screen-space extent, not by the
			// class depth. Dividing by liftBase made the same geometry read
			// red at depth 0 and green at depth 25, because the divisor grows
			// with the slider while a tear does not - Josef's 0-vs-25 pair,
			// where the red vanished and every artifact it had been marking
			// stayed. World units of lift change per world unit of surface
			// change is dimensionless, so it reads the same at any depth AND
			// at any distance. StaticsDebugView is a constant, so these
			// derivatives sit in uniform control flow.
			// pleat = world units of LIFT change per world unit of BASE
			// SURFACE travel. The base is WorldPos with the lift subtracted
			// back out of z - and that subtraction is the whole instrument.
			// Lift is baked into the drawn WorldPos, so dividing by the raw
			// position travel divides the lift change by a quantity that
			// CONTAINS the lift change: on a vertical wall the two move in
			// lockstep and the ratio is bounded at ~1 however violent the
			// tear. That bound is why iteration 2 (threshold 1.5) painted
			// every wall red and iteration 3 (threshold ~19) painted the same
			// walls green - the measure could never leave [0,1]. Against the
			// base, a tear is two vertices at nearly the SAME base position
			// with lifts a class depth apart: base travel ~0, ratio explodes.
			// A real mesh wall or the roll moves its base too, and stays low.
			float3 baseDX = ddx(input.WorldPos);
			float3 baseDY = ddy(input.WorldPos);
			baseDX.z -= ddx(input.Lift);
			baseDY.z -= ddy(input.Lift);
			float pleat = max(abs(ddx(input.Lift)) / max(length(baseDX), 1e-3),
			                  abs(ddy(input.Lift)) / max(length(baseDY), 1e-3));
			// Scored against the steepest slope the shell is SUPPOSED to have:
			// the cornice roll drops the whole class depth over kCorniceRoll
			// units, so depth/kCorniceRoll is a legitimate silhouette and must
			// not saturate. A flat threshold lit every plank end and rock rim,
			// which is why the un-scored view could not be read as a work
			// queue. NOT rim-gated on purpose: round 30 established the fences
			// ARE at the object's edges, so excluding rims would delete exactly
			// the thing this view exists to find. Magnitude separates them -
			// the roll spends the depth over 4 units, a sliver spends it over
			// one triangle.
			float rollSlope = max(liftBase, kMinSkinLift) / kCorniceRoll;
			float hot = saturate(pleat / max(rollSlope * kPleatOverRoll, 1e-3));
			// BRIGHTNESS = whether the shell actually DRAWS here. Every debug
			// mode forces coverageAlpha and fadeAlpha to 1 ("full visibility;
			// the dither must not hide geometry the diagnosis needs to see"),
			// so all of them paint the whole shell mesh, discards included -
			// which is why an object reads as solid colour in every mode and
			// why that is NOT a finding. dbgAlpha is that pass's real coverage,
			// captured before the override. Bright red is a tear in snow you
			// can see; dark red is a tear in geometry currently discarded -
			// still worth knowing, not currently visible.
			preLit = float3(hot, 1.0 - hot, 0.15 * (1.0 - hot)) * (0.2 + 0.8 * saturate(dbgAlpha));
		}
		else [branch] if (StaticsDebugView > 5.5)
		{
			// Shell Layers mode: WHICH peeled plane owns each pixel and
			// what depth it was granted. Green = layer 1, yellow = layer 2,
			// red = layer 3, magenta = below all three (no plane owns it),
			// dim blue-gray = a non-S4 draw (roads/classic). Brightness =
			// granted depth as a fraction of the slider; a dim pure color
			// is a plane that got NO height - the exact signature of every
			// starved-floor and cut-surface report.
			float layer = floor(saturate(input.Coverage) * 8.0);
			float bright = 0.25 + 0.75 * saturate(input.Flat);
			float3 layerColor = float3(0.1, 0.15, 0.25);
			[flatten] if (layer > 3.5)
				layerColor = float3(1.0, 0.0, 1.0);
			else [flatten] if (layer > 2.5)
				layerColor = float3(1.0, 0.1, 0.1);
			else [flatten] if (layer > 1.5)
				layerColor = float3(1.0, 1.0, 0.1);
			else [flatten] if (layer > 0.5)
				layerColor = float3(0.1, 1.0, 0.1);
			preLit = layerColor * bright;
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
			bool noProjData = ProjThreshold < -0.5;
			preLit = float3(saturate(input.Coverage), noProjData ? 0.0 : saturate(input.Flat), noProjData ? 1.0 : 0.0);
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
	psout.Depth = input.Position.z;
	// C0: the container's lid was rasterised, but this pixel shades the
	// surface found inside it - WorldPos already IS that point, so project
	// it directly. Without this the snow would z-test as though it stood at
	// the top of the container.
	[branch] if (containerHit)
	{
		float4 boxClip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		psout.Depth = boxClip.z / max(boxClip.w, 1e-4);
	}
	else [branch] if (trenchHitS > 0.0)
	{
		float4 hitClip = mul(CameraViewProj, float4(input.WorldPos + viewDirWS * trenchHitS, 1.0));
		psout.Depth = hitClip.z / max(hitClip.w, 1e-4);
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
	return psout;
}
#endif
