// Snow shell renderer.
//
// A vertex-buffer-less, camera-following grid of real snow geometry: the
// grid is generated from SV_VertexID (6 vertices per quad), conforms to the
// baked terrain data window, is displaced by the per-texture-class snow
// depth, and carved by the deformation map. Per-pixel normals come from the
// same height/deformation fields, so trench walls shade smoothly even where
// the geometry is coarse.
//
// Camera matrices arrive via the private ShellCB (b0), copied from the same
// per-frame data the game uploads to b12. SharedData (b5) is bound by the
// CPU side for lighting.

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
// Deliot & Heitz tiling-and-blending, already implemented for the landscape.
// Reused rather than rewritten: a frost sheet laid by Blizzard or a breath
// covers hundreds of units, and a plain tiled normal map would grid it.
#include "TerrainVariation/TerrainVariation.hlsli"

#ifdef PSHADER
// Shadow sampling for the shell surface: terrain/cloud shadows via
// GetWorldShadow, dynamic (actor) shadows via the raw cascade atlas copies
// (SnowShadow.hlsli) with the VolumetricShadows shared VSM as the fallback
// when the copies are unavailable. (The screen-space shadow mask was tried
// and rejected: it holds values for the terrain BEHIND the shell along the
// view ray, so shadows slide with camera movement.)
#	define TERRAIN_SHADOWS
#	define CLOUD_SHADOWS
#	define VOLUMETRIC_SHADOWS
SamplerState ShellLinearSampler : register(s1);
#	define LinearSampler ShellLinearSampler
#	include "Common/ShadowSampling.hlsli"
#	include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	include "Skylighting/Skylighting.hlsli"
#	include "SnowDeformation/SnowShadow.hlsli"
// Extended Materials' parallax: the POM march runs EM's own GetParallaxCoords
// through the EM_PARALLAX_CUSTOM_HEIGHT injection point (fetches routed via
// the anti-tiling taps; body in SnowParallax.hlsli - M4). The soft-shadow
// taps stay locally reimplemented (the statics shell blends two planar
// projections' RAW occlusions, which EM's multiplier API cannot express).
// LANDSCAPE/TRUE_PBR stay undefined here, so the terrain and PBR branches of
// the header compile out. Extended Materials is CORE, so the include always
// resolves.
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
	// row_major matches the game's FrameBuffer.hlsli declarations; the CPU
	// side copies the b12 bytes verbatim, which are row-major packed.
	row_major float4x4 CameraViewProj;
	row_major float4x4 CameraViewProjUnjittered;
	row_major float4x4 CameraPreviousViewProjUnjittered;
	row_major float4x4 CameraView;

	float4 ShellCameraPosAdjust;
	float4 ShellCameraPreviousPosAdjust;

	float2 GridOrigin;
	float GridSpacing;
	float TerrainTexelSize;

	// Grid-local sampling offsets, precomputed on CPU: absolute world XY at
	// ~1e5 magnitude destroys float32 finite differences (shimmer).
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
	float BorderNoise;   // world-unit domain-warp jitter of class-depth borders
	float BorderSmooth;  // world-unit ramp-widening radius between classes

	float BorderTrampledFade;    // UNUSED since round 18 (slider retired); layout keeper
	float BorderUntrampledFade;  // contact-term slope / outward-dust reach (Border Fade %, remapped 2..64 on upload)
	float SnowSnowFade;          // statics skin: object <-> landscape snow cross-fade band
	float SkinFadeStart;         // statics skin: distance dissolve start (units)

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
	// Dune-field amplitude in world units (0 flattens the undulation).
	float UndulationAmp;

	// Multiplier on the dune field's wavelengths (>1 = broader, calmer waves).
	float UndulationScale;
	// Statics skin: how much heavily trampled trench floors dissolve to the
	// object's own texture (0 = solid snow floors).
	float TrenchFloorFade;
	// LLF cluster buffers bound at t35-t37, point-shadow table at t38.
	float PointLightsActive;
	// Skylighting probe volume bound at t50.
	float SkylightingActive;

	// PBR displacement companion bound at t8.
	float HasSnowHeight;
	// Tessellated relief amplitude in world units (0 disables the path).
	float SnowReliefDepth;
	// Statics debug view: object snow renders decision variables as colors.
	float StaticsDebugView;
	float BermHeightAmp;

	float ChurnHeightAmp;
	float ChurnSizeScale;
	// Crisp grain retired 2026-08-22; layout keepers.
	float CrispScaleV;
	float CrispStrengthV;

	float ObjBermHeightAmp;
	float ObjChurnHeightAmp;
	float ObjChurnSizeScale;
	float ObjCrispScaleV;

	float ObjCrispStrengthV;
	// Distant-snow diagnostics: 0 off, 1 depth-delta heatmap (histogram at
	// u1), 2 warp-ring view, 3 data-provenance view.
	uint ShellLODDebug;
	// 1/width of the depth ramp inside the loaded-cell seam; 0 = no seam
	// data (fall back to the warped-span fade alone).
	float SeamRampInv;
	// >0.5: read the berm field from the bake at t14 instead of recomputing
	// its 17 taps per call.
	float BermBakeActive;

	// Loaded-cell boundary square (minX, minY, maxX, maxY): full terrain
	// inside, LOD terrain outside. The shell ends here and hands off to the
	// horizon recolor, which wears the same snow material.
	float4 SeamBounds;

	// Wide exclusion field window: xy = world centre, z = 1/half extent,
	// w > 0.5 when the field was baked this frame.
	float4 ExclusionFieldWindow;

	// Parallax: x = HeightScale (PBR JSON displacementScale), y = self-shadow
	// strength (0 disables), z = occlusion depth multiplier (0 disables the
	// march), w = coarse march steps.
	float4 SnowParallax;

	// x = scorch darkening strength, y = crust shading strength,
	// z = roughness of fully crusted snow, w = how far crust flattens the
	// snow normal map.
	float4 SpellShading;

	// x = reflectance of fully crusted snow, yz = its colour cast (red, green),
	// w = grazing-angle sheen strength. Blue of the cast rides SpellShading is
	// not needed - see CrustTintBlue below.
	float4 CrustLook;
	// x = blue of the crust colour cast. Its own row rather than crowding
	// CrustLook, which the sheen took. y = frost pattern strength, z = its
	// world tile size, w = whether the pattern loaded at all. Those three ride
	// spare room here rather than growing a buffer mirrored across two shaders.
	float4 CrustLook2;
	// x > 0.5 = outward dust beyond the committed edge; y = trench floor
	// height; zw = sun cascades' REAL atlas slices (the shared atlas moves
	// them with the active-light set - round 22).
	float4 BorderStyle;
	// x = compaction glint suppression (Stage 1); yzw spare (Stage 1
	// darken/roughen and Stage 2 combing both retired).
	float4 CompactLook;
}

Texture2D<float4> TerrainWindow : register(t0);
Texture2D<float4> DeformationMap : register(t1);
Texture2D<float4> SnowDiffuse : register(t2);
// Full-scene depth copy (Terrain Blending's blended depth when available),
// never the bound DSV, so sampling during the shell draw is legal.
Texture2D<float> SceneDepth : register(t3);
// Processed top-down object maps: the slope-limited snow-height FIELD (world
// Z, empty -100000) and the SUPPRESSION mask (1 under floating structures;
// no snow beneath walkways, roofs and bridges).
Texture2D<float> ObjectHeights : register(t4);
Texture2D<float2> ObjectBottoms : register(t5);
// Raw object tops (persistence-scrolled) and this frame's skin-depth raster,
// shared with the trench patch: where the shell rides a captured object, its
// layer wears the object's own skin depth instead of the landscape class
// depth.
Texture2D<float> ObjectTopsRaw : register(t11);
Texture2D<float> ObjectSkinDepthMap : register(t12);
// TruePBR snow companion maps (auto-resolved from the Textures\PBR\ variant
// of the snow path): tangent-space normals (_n) and roughness/metal/AO/spec
// (_rmaos). Gated by HasSnowNormal / HasSnowRmaos.
Texture2D<float4> SnowNormalMap : register(t6);
Texture2D<float4> SnowRmaosMap : register(t7);
// Displacement companion (_p): tessellated relief and the parallax
// self-shadow. float4 to match Extended Materials' own TexParallaxSampler
// convention; the SRV is single-channel, so only .x carries data.
Texture2D<float4> SnowHeightMap : register(t8);
// Pre-shell copy of the MASKS target: y carries the land's EM grain height
// (Lighting.hlsl LANDSCAPE writes it; 0 = no data - POM inactive, grass, or
// an object behind), the missing side of the two-sided edge contest.
Texture2D<float3> LandMasksCopy : register(t10);
// Baked berm field (BermFieldCS): the 17-tap disc average of the deformation
// map, at the map's own resolution and addressing.
Texture2D<float> BermFieldMap : register(t14);
// Wide exclusion field (ExclusionFieldCS): x = door suppression, y = melt, over
// a window that reaches the shell's own extent. The near mask at t5 still owns
// the SHELTER term, which needs geometry and so cannot travel this far.
Texture2D<float2> ExclusionFieldMap : register(t15);

// The game's own frost impact art, painted onto crusted snow.
Texture2D<float4> FrostPatternNormal : register(t16);
Texture2D<float4> FrostPatternDiffuse : register(t17);

SamplerState SnowSampler : register(s0);

// Shared trench-detail shaping, spell-mark readers, field surfaces and the
// frost pattern - the verbatim-identical pieces of both shells live in one
// file (M8; readers folded round 37).
#include "SnowDeformation/SnowFields.hlsli"


// The game's own landscape tiling: 24 texture repeats per 4096-unit cell,
// measured in-game 2026-08-17 (tiling ruler). Same texture at the same world
// rate as the ground beside us; see CODE-NOTES.md. Mirrored in
// SnowStaticsShell.hlsl, SnowDeformation.hlsli and Shell.cpp.
static const float kSnowUVTile = 4096.0 / 24.0;

// Distance warp: inner kWarpInnerVerts vertices per side keep linear
// GridSpacing; beyond them each ring's spacing grows by kWarpGrowth so the
// grid stretches ~26k units from the camera. Must match SnowDeformation.h
// (kShellWarpInnerVerts / kShellWarpGrowth).
static const float kWarpInnerVerts = 256.0;
static const float kWarpGrowth = 1.0902;

// Maps a vertex coordinate relative to the grid center (in vertex units)
// to a world-unit offset from the center.
float WarpAxis(float u)
{
	float a = abs(u);
	float lin = min(a, kWarpInnerVerts);
	float ext = max(a - kWarpInnerVerts, 0.0);
	float outer = kWarpGrowth * (pow(kWarpGrowth, ext) - 1.0) / (kWarpGrowth - 1.0);
	return sign(u) * (lin + outer) * GridSpacing;
}

// Inverse of WarpAxis: world-unit offset from the grid center back to vertex
// units. Shared by the ring debug view and the probe CS.
float InverseWarpAxis(float w)
{
	float a = abs(w) / GridSpacing;
	float ext = 0.0;
	[flatten] if (a > kWarpInnerVerts)
		ext = log2((a - kWarpInnerVerts) * (kWarpGrowth - 1.0) / kWarpGrowth + 1.0) / log2(kWarpGrowth);
	return sign(w) * (min(a, kWarpInnerVerts) + ext);
}

// CDLOD-style geomorph for the warped outer rings: each vertex slides
// between a fine and a 2x coarser lattice by a continuous morph weight.
// Both lattice endpoints are (near-)static world points, so camera motion
// never makes a vertex hop — the old per-ring snapping resampled the field
// in full ring-step jumps, which read as distant up/down flicker and
// popping holes. Lattices are centered on the grid center (inner vertices
// land exactly on the level-0 lattice, so the morph zone joins the linear
// zone without a crack); the center steps 8 units with the camera, so
// endpoints micro-shift by at most 8 units — a sixteenth of a data texel.
// Takes/returns CENTERED coordinates (gridLocal - WarpedHalfSpan).
float2 GeomorphVertexXY(float2 centered, float2 u)
{
	float2 ringStep = GridSpacing * pow(kWarpGrowth, max(abs(u) - kWarpInnerVerts, 0.0));
	float2 lod = log2(max(ringStep / GridSpacing, 1.0));
	float2 fineStep = GridSpacing * exp2(floor(lod));
	// PURE world-lattice snap — vertices never slide in XY. Whole ring bands
	// share one power-of-two lattice snapped on ABSOLUTE world coordinates,
	// so the set of rendered points (the surface) is world-static; camera
	// steps only reassign which vertex index owns which lattice point. The
	// LOD transition happens in the sampled DATA instead (ShellSurfaceZ
	// blends terrain data toward the coarser lattice's surface, clipmaps-
	// style), so nothing crawls across the terrain — which also makes the
	// motion vectors' zero-motion assertion true by construction.
	// Inner linear zone is exact: GridOrigin is GridSpacing-snapped and
	// WarpAxis returns whole steps there, so snapping to fineStep is a no-op.
	float2 absXY = GridOrigin + WarpedHalfSpan + centered;
	return floor(absXY / fineStep + 0.5) * fineStep - (GridOrigin + WarpedHalfSpan);
}

// Shell edge fade: the shell dissolves at the loaded-cell seam, where the
// game swaps full terrain for LOD meshes and the horizon recolor takes over
// in the same snow material. The warped-span fade stays as the fallback for
// the grid's own physical edge (and for frames with no seam data).
float ShellEdgeFade(float2 gridLocal)
{
	float2 delta = abs(gridLocal - WarpedHalfSpan);
	float fade = saturate((WarpedHalfSpan - max(delta.x, delta.y)) / 2048.0);
	[flatten] if (SeamRampInv > 0.0)
	{
		float2 worldXY = GridOrigin + gridLocal;
		float inside = min(min(worldXY.x - SeamBounds.x, worldXY.y - SeamBounds.y),
			min(SeamBounds.z - worldXY.x, SeamBounds.w - worldXY.y));
		fade = min(fade, saturate(inside * SeamRampInv));
	}
	return fade;
}

struct VS_OUTPUT
{
	// noperspective centroid: required interpolation for SV_Position when
	// the PS writes conservative depth (SV_DepthLessEqual).
	linear noperspective centroid float4 Position : SV_POSITION;
	float4 CurrentClip : TEXCOORD0;
	float4 PreviousClip : TEXCOORD1;
	float3 WorldPos : TEXCOORD2;
	float2 GridLocal : TEXCOORD3;
	float Snowness : TEXCOORD4;
	float DebugHeight : TEXCOORD5;
	// xyz: smooth per-vertex terrain normal, w: coverage alpha (taper*fade).
	float4 TerrainNormalAlpha : TEXCOORD6;
};

// Manual bilinear over the terrain window (Load-based, deterministic).
// Returns (height, rampDepth, coverage): rampDepth is the per-texture-class
// depth blend in world units, precomputed at window-rebuild time on the CPU.
// gridLocal = world XY relative to GridOrigin (small, precision-safe).
float3 SampleTerrain(float2 gridLocal)
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

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Bilinear helper at fractional texel coordinates (Load-based).
float SampleDeformationBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = DeformationMap.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = DeformationMap.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = DeformationMap.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = DeformationMap.Load(int3(t1.x, t1.y, 0)).x;

	// Clamped here rather than at each call site: melt writes past 1.0 into
	// the refill headroom, and this is the single tap every consumer goes
	// through (bicubic, fast, and the berm field's 17 taps). The B-spline
	// weights are a convex combination, so clamping here bounds them all.
	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}

// B-spline bicubic sample of the deformation map, built from four bilinear
// taps at fractional offsets. Smooth value and gradient: plain bilinear
// leaves texel-rate creases in trench walls, and value-only smoothing
// terraces the gradient the normals consume.
// Returns 0 outside the deformation window.
float SampleDeformation(float2 gridLocal)
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

	float v00 = SampleDeformationBilinear(float2(h0.x, h0.y), dims);
	float v10 = SampleDeformationBilinear(float2(h1.x, h0.y), dims);
	float v01 = SampleDeformationBilinear(float2(h0.x, h1.y), dims);
	float v11 = SampleDeformationBilinear(float2(h1.x, h1.y), dims);

	return g0.y * (g0.x * v00 + g1.x * v10) + g1.y * (g0.x * v01 + g1.x * v11);
}

// DISPLACED depth at a point: total minus the melted portion. A berm is snow
// that had to go somewhere, and melted snow leaves no spoil, so the berm
// field is built from this rather than from total depth. Doing it here rather
// than scaling the finished berm down means a boot print through a melt basin
// still throws its own ridge.
float SampleDisplacedFast(float2 gridLocal)
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

	float4 s00 = DeformationMap.Load(int3(t0.x, t0.y, 0));
	float4 s10 = DeformationMap.Load(int3(t1.x, t0.y, 0));
	float4 s01 = DeformationMap.Load(int3(t0.x, t1.y, 0));
	float4 s11 = DeformationMap.Load(int3(t1.x, t1.y, 0));
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Only MELTED depth is spoil-free. Channel y is signed and negative means
	// scorch, which was displaced and keeps its berm.
	return saturate(v.x - max(v.y, 0.0));
}

// SampleMelted / SampleScorch / SampleCrust live in SnowFields.hlsli
// (round 37): both shells read the surface-state channels identically.

// Single-bilinear deformation tap: for many-tap averages (BermField) where
// the sum provides the smoothness and bicubic per tap would be waste.
float SampleDeformationFast(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	return SampleDeformationBilinear(uv * dims - 0.5, dims);
}

// Bilinear samples of the object field maps at absolute world XY. The raster
// pass maps +worldY to +ndcY = texture v0 (top), so v mirrors.
float2 ObjectMapTexel(float2 worldXY, out float2 dims, out bool valid)
{
	float2 local = (worldXY - ObjectHeightCenter) / ObjectHeightHalfExtent;
	valid = all(abs(local) < 0.98);
	ObjectHeights.GetDimensions(dims.x, dims.y);
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

float SampleObjectHeight(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	if (!valid)
		return -100000.0;
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = ObjectHeights.Load(int3(t0.x, t0.y, 0));
	float s10 = ObjectHeights.Load(int3(t1.x, t0.y, 0));
	float s01 = ObjectHeights.Load(int3(t0.x, t1.y, 0));
	float s11 = ObjectHeights.Load(int3(t1.x, t1.y, 0));

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// SampleExclusionField lives in SnowFields.hlsli (round 37).

float2 SampleObjectBottom(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	// t5 is the TWO-CHANNEL mask: x = door suppression 0-1, y = melt
	// fraction 0-1 (fires, workspaces, sheltered ground) - independent
	// channels, so a door's influence tail cannot discard the melt around
	// it. Outside the window there is no knowledge, so nothing is
	// suppressed or melted. (A raw-height sentinel here zeroed the VS
	// coverage on every out-of-window vertex, flipping the bare-submerge
	// term on all distant shell geometry.)
	if (!valid)
		return float2(0.0, 0.0);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float2 s00 = ObjectBottoms.Load(int3(t0.x, t0.y, 0));
	float2 s10 = ObjectBottoms.Load(int3(t1.x, t0.y, 0));
	float2 s01 = ObjectBottoms.Load(int3(t0.x, t1.y, 0));
	float2 s11 = ObjectBottoms.Load(int3(t1.x, t1.y, 0));

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Near mask unioned with the wide field. The near one wins where it has data -
// it alone carries shelter (roofs, tents, walkways) and it resolves clearings
// at four times the field's resolution - and the field fills in beyond its
// reach, which is where every clearing used to vanish.
float2 SampleExclusionMask(float2 worldXY)
{
	float2 nearMask = 0.0;
	[branch] if (ObjectLiftCap > 0.0)
		nearMask = SampleObjectBottom(worldXY);
	return max(nearMask, SampleExclusionField(worldXY));
}

// The object-layer depth cap: the skin depth (max of 4 texels; the raster is
// sentinel-free, 0 where nothing wrote) where a captured object covers the
// texel, or a huge no-cap value where none does or the window does not reach.
float SampleObjectDepthCap(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	if (!valid)
		return 1e6;
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float4 tops = float4(
		ObjectTopsRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t0.y, 0)),
		ObjectTopsRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t1.y, 0)));
	[flatten] if (all(tops < -50000.0))
		return 1e6;
	return max(
		max(ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0)), ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0))),
		max(ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0)), ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0))));
}

// ---- Surface undulation: wind-settled dunes ----
// Two octaves of world-anchored value noise, added as real geometry (via
// ShellSurfaceZ, so the VS displaces by it) and shaded per-pixel through
// its gradient. Amplitude scales with local depth so thin snow, class
// boundaries and carved floors stay flat. Wave height and wavelength are
// live controls (UndulationAmp / UndulationScale).
//
// Minimum snow cover on carved trench floors is live (BorderStyle.y, the
// Trench Floor Height slider): the old constant 5 covered the terrain
// window's bilinear error so the mesh never poked through; low values let
// deep trampling wear through to the real ground on purpose.
// Undulation, CarveProfile and kFireMeltFloor live in SnowFields.hlsli
// (round 37): the surface, the shading gradient and BOTH shells' self-shadow
// marches see the same shape.

// Edge berm: displaced snow piles as a rounded hill along the trench rim;
// a deeper layer throws a taller berm. The shape input is the BLURRED
// deformation (BermField): two sample rings reach ~40 units past the
// trail edge, and the outer ring's small per-tap weight gives the hill
// a long, gentle outer tail instead of a knife along the stamp falloff.
// Height is the live BermHeightAmp slider. BermShape and the tap ring live
// in SnowFields.hlsli, shared with the statics shell.

float BermFieldTapped(float2 gridLocal)
{
	float b = SampleDisplacedFast(gridLocal);
	[unroll] for (int i = 0; i < 16; i++)
		b += SampleDisplacedFast(gridLocal + kBermTaps[i]);
	return saturate(b / 17.0);
}

// The berm field dominated the shell's pixel cost at 68 loads a call, and the
// normal alone needs four. BermBakeActive routes to the bake; the tapped path
// stays compiled so the two can be A/B'd from the debug menu.
float BermField(float2 gridLocal)
{
	float field = 0.0;
	[branch] if (BermBakeActive > 0.5)
		field = BermFieldBaked(gridLocal);
	else
		field = BermFieldTapped(gridLocal);
	return field;
}

// ---- Trench churn: chunky broken snow in disturbed zones ----
// Short-wavelength noise added as real geometry where snow was carved or
// piled (RDR2 reference, Josef's design): trench walls, floors and berms
// get ~10-unit lumps; the weight comes from the same carve/berm terms the
// profile uses, so churn dies at the untouched surface with no boundary.
// The self-shadow march skips it: a few units is under its step
// resolution. Amplitude and lump size are live sliders (ChurnHeightAmp /
// ChurnSizeScale).
float ChurnNoise(float2 worldXY)
{
	return ChurnNoiseScaled(worldXY, ChurnSizeScale);
}

// Class borders are hard edges in the baked depth/coverage data: a +30
// snow class meeting a -5 mud class produces a ravine wall along the
// texture seam. BorderNoise domain-warps where the border falls and
// BorderSmooth widens the ramp with a tap cross. Terrain height is always
// sampled at the true position, so the shell keeps conforming.
float3 SampleTerrainShaped(float2 gridLocal)
{
	float3 result = SampleTerrain(gridLocal);
	[branch] if (BorderNoise >= 0.01 || BorderSmooth >= 0.01)
	{
		float2 worldXY = GridOrigin + gridLocal;
		// Coarse wander + fine raggedness (round 18): the coarse octave is
		// the ORIGINAL 37-unit model but capped at the strength its old 16
		// setting had - enough organic wander, never the lobes/islands the
		// uncapped version folded into; the fine 8-unit octave scales on
		// with the slider for the ragged detail (Josef's sketch).
		float coarseAmp = min(BorderNoise, 16.0);
		float2 jitter = float2(
							ShapeNoise(worldXY / 37.0) - 0.5,
							ShapeNoise(worldXY / 37.0 + 111.7) - 0.5) *
		                    (2.0 * coarseAmp) +
		                float2(
							ShapeNoise(worldXY / 8.0) - 0.5,
							ShapeNoise(worldXY / 8.0 + 57.3) - 0.5) *
		                    (1.2 * BorderNoise);
		float2 shapedLocal = gridLocal + jitter;

		float2 depthCoverage = SampleTerrain(shapedLocal).yz;
		[branch] if (BorderSmooth >= 0.01)
		{
			float r = BorderSmooth;
			depthCoverage += SampleTerrain(shapedLocal + float2(r, 0.0)).yz;
			depthCoverage += SampleTerrain(shapedLocal - float2(r, 0.0)).yz;
			depthCoverage += SampleTerrain(shapedLocal + float2(0.0, r)).yz;
			depthCoverage += SampleTerrain(shapedLocal - float2(0.0, r)).yz;
			depthCoverage *= 0.2;
		}
		result.yz = depthCoverage;
	}
	return result;
}

// The shell surface: per-texture-class snow depth carved by deformation.
// Class depths blend by their baked weights on the CPU (window rebuild),
// so boundaries between differently-deep snows are geometric depth ramps;
// negative depths (roads) submerge the shell below the surface.
// Shared by the VS (geometry) and PS (per-pixel normals) so both agree.
float ShellSurfaceZ(float2 gridLocal, out float coverage, out float terrainHeight)
{
	// Beyond the loaded-cell seam the recolored LOD terrain owns the ground:
	// park the vertex under it and skip the field work entirely. Single
	// return keeps fxc's X4000 quiet (early return + out params misfires).
	float edgeFade = ShellEdgeFade(gridLocal);
	float surfaceZ;
	[branch] if (edgeFade <= 0.0)
	{
		terrainHeight = SampleTerrain(gridLocal).x;
		coverage = 0.0;
		surfaceZ = terrainHeight - 32.0;
	}
	else
	{
		float3 terrain = SampleTerrainShaped(gridLocal);

		// Data morph (geometry-clipmaps style): the vertex stays put on its
		// fine world lattice; its terrain data blends toward the 2x-coarser
		// lattice's bilinear surface, reaching it exactly at the band switch —
		// band transitions move ONLY the height signal, by the difference
		// between two resolutions, and never slide geometry across the
		// terrain. Coarse taps are unshaped (border noise is near-field
		// cosmetics).
		{
			float2 centeredM = gridLocal - WarpedHalfSpan;
			float2 uAxisM = float2(InverseWarpAxis(centeredM.x), InverseWarpAxis(centeredM.y));
			float2 ringStepM = GridSpacing * pow(kWarpGrowth, max(abs(uAxisM) - kWarpInnerVerts, 0.0));
			float2 lodM = log2(max(ringStepM / GridSpacing, 1.0));
			float morphT = frac(max(lodM.x, lodM.y));
			[branch] if (max(lodM.x, lodM.y) > 0.0 && morphT > 0.001)
			{
				float2 coarseStepM = GridSpacing * exp2(floor(lodM) + 1.0);
				float2 absXYM = GridOrigin + WarpedHalfSpan + centeredM;
				float2 cBase = floor(absXYM / coarseStepM) * coarseStepM;
				float2 cFrac = (absXYM - cBase) / coarseStepM;
				float2 cLocal = cBase - GridOrigin;
				float3 t00 = SampleTerrain(cLocal);
				float3 t10 = SampleTerrain(cLocal + float2(coarseStepM.x, 0.0));
				float3 t01 = SampleTerrain(cLocal + float2(0.0, coarseStepM.y));
				float3 t11 = SampleTerrain(cLocal + coarseStepM);
				// Any sentinel tap poisons the bilinear (even averaged it can
				// sneak past a threshold) — keep the fine data at data edges.
				float minTap = min(min(t00.x, t10.x), min(t01.x, t11.x));
				[flatten] if (minTap > -50000.0)
					terrain = lerp(terrain, lerp(lerp(t00, t10, cFrac.x), lerp(t01, t11, cFrac.x), cFrac.y), morphT);
			}
		}

		terrainHeight = terrain.x;
		float rampDepth = terrain.y;
		coverage = saturate(terrain.z);

		// Slim anti-pinhole at distance (restored after in-game holes): a
		// 4-tap axis max of height+coverage with a CAPPED ridge pad fills the
		// flickering pinholes where bilinear dips under the mesh's triangle
		// diagonals or single texels read bare. The heavy 8-tap/150-unit-pad/
		// 8-unit-float stack stays gone — the seam caps the range this runs at.
		float camDist = length(gridLocal - WarpedHalfSpan);
		[branch] if (camDist > 3000.0)
		{
			float farBlend = smoothstep(3000.0, 8000.0, camDist);
			float3 n0 = SampleTerrain(gridLocal + float2(TerrainTexelSize, 0.0));
			float3 n1 = SampleTerrain(gridLocal - float2(TerrainTexelSize, 0.0));
			float3 n2 = SampleTerrain(gridLocal + float2(0.0, TerrainTexelSize));
			float3 n3 = SampleTerrain(gridLocal - float2(0.0, TerrainTexelSize));
			float h0 = n0.x > -50000.0 ? n0.x : terrainHeight;
			float h1 = n1.x > -50000.0 ? n1.x : terrainHeight;
			float h2 = n2.x > -50000.0 ? n2.x : terrainHeight;
			float h3 = n3.x > -50000.0 ? n3.x : terrainHeight;
			float maxHeight = max(max(h0, h1), max(h2, h3));
			float c0 = n0.x > -50000.0 ? n0.z : 0.0;
			float c1 = n1.x > -50000.0 ? n1.z : 0.0;
			float c2 = n2.x > -50000.0 ? n2.z : 0.0;
			float c3 = n3.x > -50000.0 ? n3.z : 0.0;
			float maxCoverage = saturate(max(max(c0, c1), max(c2, c3)));
			float ridgePad = min(0.25 * max(abs(h0 - h1), abs(h2 - h3)), 24.0);
			terrainHeight = lerp(terrainHeight, max(terrainHeight, maxHeight) + ridgePad, farBlend);
			coverage = lerp(coverage, max(coverage, maxCoverage), farBlend);
			terrainHeight += farBlend * 3.0 * saturate(coverage);
		}

		// The mask carries two independent channels. x = door suppression,
		// smooth 0-1, so doorstep clearings fade at their edges instead of
		// cutting. y = melt fraction: depth thins toward kFireMeltFloor
		// (coverage untouched), so melted ground keeps a thin snow floor
		// instead of fading to bare ground. Sampled BEFORE the object lift:
		// the wall-drift banks must respect clearings too (see below).
		float2 shelterMask = SampleExclusionMask(GridOrigin + gridLocal);

		// Object height field: t4 holds the SLOPE-LIMITED snow-height field
		// (terrain run through the angle-of-repose cone transform), t5 the
		// shelter mask; 1 under floating structures, so walkways, roofs and
		// bridges keep the ground beneath them bare.
		[branch] if (ObjectLiftCap > 0.0)
		{
			float2 worldXY = GridOrigin + gridLocal;
			float field = SampleObjectHeight(worldXY);
			[flatten] if (field > -50000.0)
			{
				// The lift never crosses the shell's own boundary: banks and
				// cones rise only from ground that shows shell snow (class
				// coverage minus clearings). Gating on the exclusion mask
				// alone missed bare LANDSCAPE classes: the drift raised a rim
				// right where the shell meets the dirt and hung a coverage-
				// forced slab whose shadow caster scaled with Wall Drift
				// Height (castVis crosses its 0.35 cull near WDH 10 — Josef's
				// lever point). Supersedes the wall-base failsafe design pin:
				// banks stop at the coverage edge instead of forcing snow
				// onto bare ground.
				float groundSnow = smoothstep(0.1, 0.45, coverage) * (1.0 - saturate(shelterMask.x));
				field = lerp(min(field, terrainHeight), field, groundSnow);
				// Where a captured object defines the surface, the layer wears
				// the object's own skin depth instead of the landscape class
				// depth (a thin-skinned rock must not carry a deep landscape
				// layer). Blend by how far the object stands proud of the
				// un-lifted base, so buried objects and the aprons around them
				// keep landscape depth.
				float lift = field - terrainHeight;
				float capT = smoothstep(0.25, 1.0, lift / max(rampDepth, 1.0));
				rampDepth = lerp(rampDepth, min(rampDepth, SampleObjectDepthCap(worldXY)), capT);
				terrainHeight = max(terrainHeight, field);
				// Drift failsafe: a field standing well proud IS snow. Force
				// coverage and a minimum depth so banks raised over bare or
				// low-coverage ground (dirt patches at walls) never dither into
				// holes or submerge out of the surface.
				float liftForce = smoothstep(6.0, 20.0, lift);
				coverage = max(coverage, liftForce);
				rampDepth = max(rampDepth, liftForce * 6.0);
			}
		}

		{
			coverage *= saturate(1.0 - shelterMask.x);
			float melt = saturate(shelterMask.y);
			rampDepth = lerp(rampDepth, min(rampDepth, kFireMeltFloor), melt);
		}

		// Bare ground contributes negative depth so the shell submerges toward
		// uncovered terrain as well; edgeFade (computed above) melts the shell
		// into the ground across the seam ramp.
		float bare = saturate(1.0 - coverage);
		float depth = rampDepth + (-8.0) * bare;
		depth = lerp(-8.0, depth, edgeFade);

		// Touch-down toe: compress the last few units of positive depth so
		// the blanket's rim meets the ground at class borders instead of
		// hanging a hovering lip over the bare side (visible under-gap at
		// grazing angles). Deep trench floors pass unchanged; a floor set
		// below ~5 (Trench Floor Height) compresses with the toe.
		[flatten] if (depth > 0.0)
			depth *= smoothstep(0.0, 5.0, depth);

		// Deformation carves only where the layer is actually raised; the
		// negative-depth submerge at class edges is untouched. The carved floor
		// never drops below Trench Floor Height units (or the un-carved depth when
		// thinner), so trench bottoms stay shell snow. The floor tapers away as
		// the uncarved depth thins toward class borders; a full floor there would
		// hold a hard-edged slab over bare ground (border fade itself is alpha,
		// handled in the PS). Undulation rides on top, scaled by the remaining
		// depth so floors and thin edges stay flat.
		[flatten] if (depth > 0.0)
		{
			float deformation = saturate(SampleDeformation(gridLocal));
			float bermD = BermField(gridLocal);
			float uncarved = depth;
			// The berm is spoil piled on snow that was NOT dug. Unmasked, the
			// blurred field also lifted the trench floor and walls - a narrow
			// trail's disc average is well above zero at its own centre, so
			// raising Berm Height raised the whole trench with it.
			depth = CarveProfile(deformation, uncarved) +
			        BermShape(bermD) * saturate(1.0 - deformation) * uncarved * BermHeightAmp;
			depth += Undulation(GridOrigin + gridLocal) * saturate(depth / 8.0);
			// Churn scales away on thin cover: the /10 keeps the dig under 80% of
			// local depth even at the slider's 8-unit maximum.
			depth += ChurnNoise(GridOrigin + gridLocal) * ChurnHeightAmp * ChurnWeight(deformation, bermD) * saturate(depth / 10.0);
		}

		surfaceZ = terrainHeight + depth;
	}
	return surfaceZ;
}

// Shared vertex tail for the legacy VS and the tessellated domain shader:
// smooth per-vertex terrain normal, coverage alpha, debug plane, camera-
// relative transform and output packing.
VS_OUTPUT FinishShellVertex(float2 gridLocal, float z, float coverage, float terrainHeight)
{
	// Smooth terrain normal per-vertex: wide 32-unit differences bridge the
	// 128-unit data texels, and interpolation removes the faceting of the
	// per-pixel piecewise-constant gradient.
	float hxp = SampleTerrain(gridLocal + float2(32.0, 0.0)).x;
	float hxn = SampleTerrain(gridLocal - float2(32.0, 0.0)).x;
	float hyp = SampleTerrain(gridLocal + float2(0.0, 32.0)).x;
	float hyn = SampleTerrain(gridLocal - float2(0.0, 32.0)).x;
	float3 terrainNormal = normalize(float3(-(hxp - hxn) / 64.0, -(hyp - hyn) / 64.0, 1.0));

	// Coverage alpha drives both geometry taper and edge dithering in the PS.
	float taper = smoothstep(0.0, 0.6, coverage);
	float coverageAlpha = taper * ShellEdgeFade(gridLocal);

	// Data debug (modes 1-3): conforming plane well above the sampled
	// terrain height, colored by the sampled values. Modes 4+ diagnose the
	// REAL surface - lifting them made every depth-relative channel in the
	// SSS gate view meaningless (round-6 lesson: the first screenshots
	// measured the gate from a plane 200 units up).
	if (ShellDebugData != 0 && ShellDebugData < 4)
		z = terrainHeight + 200.0;
	// Provenance view: sentinel texels sit ~100k under the world and would be
	// invisible; raise them to eye level so data gaps read as a red sheet.
	if (ShellLODDebug == 3 && terrainHeight < -50000.0)
		z = ShellCameraPosAdjust.z;

	float3 absolutePos = float3(GridOrigin + gridLocal, z);

	// Shader world space is camera-relative; previous frame uses its own adjust.
	float3 rel = absolutePos - ShellCameraPosAdjust.xyz;
	float3 prevRel = absolutePos - ShellCameraPreviousPosAdjust.xyz;

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
	vsout.WorldPos = rel;
	vsout.GridLocal = gridLocal;
	vsout.Snowness = coverage;
	vsout.DebugHeight = terrainHeight;
	vsout.TerrainNormalAlpha = float4(terrainNormal, coverageAlpha);
	return vsout;
}

#if defined(VSHADER) && !defined(SNOW_TESS)
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	static const float2 kCorners[6] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const float2 kCornersFlipped[6] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };

	uint quadIndex = vertexID / 6;
	uint2 quadXY = uint2(quadIndex % GridDim, quadIndex / GridDim);
	// Union-jack triangulation: alternate the quad diagonal on a checkerboard
	// so walls crossing the grid alias half as hard. Parity is anchored to
	// world position, not grid indices: index parity re-phases every camera
	// step and makes walls visibly shift with movement.
	int2 parityBase = int2(floor(GridOrigin / GridSpacing));
	uint parity = uint(parityBase.x + int(quadXY.x)) ^ uint(parityBase.y + int(quadXY.y));
	float2 corner = ((parity & 1) != 0) ? kCornersFlipped[vertexID % 6] : kCorners[vertexID % 6];
	float2 gridPos = float2(quadXY) + corner;
	// Warped placement: gridLocal stays a world-unit offset from GridOrigin
	// (the warped grid's min corner), so all field sampling is unchanged.
	float2 u = gridPos - (float)GridDim * 0.5;
	float2 gridLocal = float2(WarpAxis(u.x), WarpAxis(u.y)) + WarpedHalfSpan;

	// Outer rings geomorph between world-anchored lattices (see
	// GeomorphVertexXY) — vertices slide instead of hopping ring steps.
	gridLocal = GeomorphVertexXY(gridLocal - WarpedHalfSpan, u) + WarpedHalfSpan;

	float coverage;
	float terrainHeight;
	float z = ShellSurfaceZ(gridLocal, coverage, terrainHeight);

#ifdef SNOW_SHADOW_CAST
	// Shadow-caster variant: only the excess height above the ambient snow
	// depth casts. Casting the full shell shadows every receiver inside or
	// beneath the layer (the terrain it visually replaces, wading actor
	// legs, grass), which reads as the whole landscape darkening.
	//
	// The base is sunk far below the terrain, not merely flattened: the
	// terrain window is bilinear-approximate, and writing it at ground level
	// out-depths the game's true terrain mesh wherever the approximation
	// overshoots, leaving false shadow blotches on open ground. The caster
	// also requires solid snow coverage, so field raises whose visible snow
	// is dithered away never cast from invisible snow.
	// Round 21 (Josef's hill + beach evidence): game casters are real
	// geometry at real positions; the old surrogate was neither. Its
	// excess-only rule cast nothing from smooth raised snow (a snowfield's
	// surface IS the ambient depth - zero excess), while its sunk remainder
	// still clipped near-horizontal low-sun rays and printed sideways
	// shadow blotches on bare ground - underground geometry is not
	// invisible to light. The REAL surface now casts wherever it stands
	// meaningfully above the ground and carries snow; everything else is
	// culled by NaN collapse, which drops the triangles at the rasterizer -
	// no sun angle can resurrect them. The world under the layer never
	// sees these shadows: the shell covers it.
	float3 rawTerrainCast = SampleTerrain(gridLocal);
	// Round 33 (Josef's Pale beach phantom shadows): the far anti-pinhole
	// pass dilates coverage AND height (4-tap max + ridge pad), so beyond
	// ~3000 units a ring around every patch passes both castVis terms while
	// the main view discards those pixels per-texel — and this depth-only
	// caster has no PS to do the same. Gating coverage on the UNDILATED
	// texel under the vertex keeps true snowfields casting and silences
	// the dilation skirt over bare coast.
	float castVis = smoothstep(2.0, 5.0, z - rawTerrainCast.x) * smoothstep(0.2, 0.5, min(coverage, saturate(rawTerrainCast.z)));
	// Melt pits do not cast (the coverage principle again: snow the melt
	// removed casts nothing). The visible pit dissolves at texture
	// resolution while the caster rim collapses at vertex resolution; the
	// mismatched rim printed a static blocky shadow onto the revealed
	// ground. The wide exclusion field already sinks static clearings;
	// this is the stamped (spell) melt, ring included. Thresholds sit WELL
	// above the residue floor (round 32): the refill decays .y every frame,
	// and a 0.05 lower edge put decaying residue on the castVis NaN cliff -
	// caster triangles over every old melt site flipped once a second.
	castVis *= 1.0 - smoothstep(0.35, 0.6, SampleMelted(gridLocal));
	// Round 34 (Josef's call): the far field keeps printing blotches from
	// residual data/geometry mismatches the near gates cannot see, and the
	// shell's crisp shadows only matter near the camera anyway — the far
	// field is diffuse-dominated and terrain shadows carry the rest. Fade
	// the caster out entirely over ~40-70 m.
	castVis *= 1.0 - smoothstep(2800.0, 4900.0, length(gridLocal - WarpedHalfSpan));
	if (castVis < 0.35)
		z = asfloat(0x7fc00000);  // NaN: kills every triangle touching this vertex
#endif

	return FinishShellVertex(gridLocal, z, coverage, terrainHeight);
}
#endif

// ---- Tessellated path (SNOW_TESS): near-camera vertex density so the
// deformation map's full resolution and the PBR displacement relief render
// as real geometry. The control-point VS does grid placement only; the
// domain shader runs the full surface evaluation per generated vertex.

struct TessControlPoint
{
	float2 GridLocal : TEXCOORD0;
};

#if defined(VSHADER) && defined(SNOW_TESS)
TessControlPoint main(uint vertexID : SV_VertexID)
{
	static const float2 kPatchCorners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 4;
	uint2 quadXY = uint2(quadIndex % GridDim, quadIndex / GridDim);
	float2 gridPos = float2(quadXY) + kPatchCorners[vertexID % 4];
	// Same warped placement + world-anchored ring snapping as the legacy VS;
	// corners depend only on grid coordinates, so adjacent patches share
	// their edge vertices exactly.
	float2 u = gridPos - (float)GridDim * 0.5;
	float2 gridLocal = float2(WarpAxis(u.x), WarpAxis(u.y)) + WarpedHalfSpan;

	TessControlPoint cp;
	cp.GridLocal = GeomorphVertexXY(gridLocal - WarpedHalfSpan, u) + WarpedHalfSpan;
	return cp;
}
#endif

#if defined(HULLSHADER) || defined(DOMAINSHADER)
struct TessFactors
{
	float Edge[4] : SV_TessFactor;
	float Inside[2] : SV_InsideTessFactor;
};

// Detail reach: full kTessMax within kTessNear/kTessMax units, factor 1 by
// kTessNear. Matches the relief fade band so tessellation is never spent
// where the displacement has already faded out.
static const float kTessNear = 1600.0;
static const float kTessMax = 8.0;
#endif

#ifdef HULLSHADER
// Deformed edges stretch the detail reach by this much (see EdgeTessFactor).
static const float kTessReachBoost = 3.0;
// Past the BOOSTED reach the factor clamps to 1 for every deformation value,
// so the map taps cannot change the answer.
static const float kTessCutoff = kTessNear * kTessReachBoost;

// Edge factor from the edge midpoint's camera distance, computed from the
// shared corners only, so both patches on an edge agree (crack-free).
// Trench-aware: edges carrying deformation get up to kTessReachBoost times the
// detail reach, so trench walls stay smooth well past the base band while
// untouched snowfields keep the cheap factors; the boost reads only edge-
// derived positions, preserving the crack-free property.
float EdgeTessFactor(float2 gridLocalA, float2 gridLocalB)
{
	float2 midLocal = 0.5 * (gridLocalA + gridLocalB);
	float2 midAbs = GridOrigin + midLocal;
	float dist = length(midAbs - ShellCameraPosAdjust.xy);
	// Undeformed ground is subdivided ONLY to carry the displacement-map
	// relief, so with relief off its base reach goes to zero and the factor
	// clamps to 1 - no vertices spent on a surface that is not displaced.
	// Trenches keep their boost either way: the carve is what the tessellated
	// path actually exists for, and it is the half that survives relief being
	// retired. At relief > 0 the curve is exactly the old lerp(1, boost, t).
	float reliefBase = SnowReliefDepth > 0.01 ? 1.0 : 0.0;
	// Past the cutoff even the fully boosted reach clamps to 1, so the taps -
	// three bicubics, sixteen loads each, the bulk of this shader - cannot
	// change the answer and are skipped.
	float reach = kTessNear * reliefBase;
	[branch] if (dist < kTessCutoff)
	{
		float deform = max(max(SampleDeformation(gridLocalA), SampleDeformation(gridLocalB)), SampleDeformation(midLocal));
		reach = kTessNear * lerp(reliefBase, kTessReachBoost, smoothstep(0.02, 0.25, deform));
	}
	return clamp(reach / max(dist, 32.0), 1.0, kTessMax);
}

TessFactors PatchConstants(InputPatch<TessControlPoint, 4> patch)
{
	// Quad edge order: [0] u=0, [1] v=0, [2] u=1, [3] v=1, for the domain
	// bilerp corner layout 0=(0,0) 1=(1,0) 2=(1,1) 3=(0,1).
	float4 edges = float4(
		EdgeTessFactor(patch[0].GridLocal, patch[3].GridLocal),
		EdgeTessFactor(patch[0].GridLocal, patch[1].GridLocal),
		EdgeTessFactor(patch[1].GridLocal, patch[2].GridLocal),
		EdgeTessFactor(patch[3].GridLocal, patch[2].GridLocal));
	float inner = max(max(edges.x, edges.y), max(edges.z, edges.w));

	TessFactors f;
	f.Edge[0] = edges.x;
	f.Edge[1] = edges.y;
	f.Edge[2] = edges.z;
	f.Edge[3] = edges.w;
	f.Inside[0] = inner;
	f.Inside[1] = inner;
	return f;
}

[domain("quad")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("PatchConstants")]
TessControlPoint main(InputPatch<TessControlPoint, 4> patch, uint i : SV_OutputControlPointID)
{
	return patch[i];
}
#endif

#include "SnowDeformation/SnowParallax.hlsli"

#ifdef DOMAINSHADER
[domain("quad")]
VS_OUTPUT main(TessFactors factors, float2 domainUV : SV_DomainLocation, const OutputPatch<TessControlPoint, 4> patch)
{
	float2 gridLocal = lerp(
		lerp(patch[0].GridLocal, patch[1].GridLocal, domainUV.x),
		lerp(patch[3].GridLocal, patch[2].GridLocal, domainUV.x), domainUV.y);

	float coverage;
	float terrainHeight;
	float z = ShellSurfaceZ(gridLocal, coverage, terrainHeight);

	float camDist = length(GridOrigin + gridLocal - ShellCameraPosAdjust.xy);
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	// Coarser mips with distance: vertex density falls below texel density
	// out there and full-res sampling shimmers.
	float snowMip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);

	// Grain-driven skirt descent (HEIGHT-BLEND-PLAN round 5): EM's boundary
	// look is sharpened weights + the parallax march displacing the boundary
	// per view ray; the shell's analog is real geometry. Across the coverage
	// edge band the surface descends to the ground in grain-shaped tongues
	// instead of a knife-cut hover. Half the PS alpha's sharpness: the alpha
	// cuts the crisp outline on top, while near-binary geometry raises
	// vertical walls that smear the top-projected texture. Deterministic per
	// position (grid-local + world-anchored grain), so shared patch-edge
	// vertices agree and the mesh stays crack-free.
	float descentBlend = SnowHeightBlendSharpness(camDist);
	float wEdge = smoothstep(0.0, 0.6, coverage);
	[branch] if (HasSnowHeight > 0.5 && descentBlend > 1.0 && z > terrainHeight && wEdge < 0.999)
	{
		float hDescent = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, GridOrigin + gridLocal), 0.0.xx, snowMip);
		float descent = SnowHeightBlendOneSided(wEdge, hDescent, 1.0 + (descentBlend - 1.0) * 0.5);
		z = terrainHeight + (z - terrainHeight) * descent;
	}

	// Edge grain (round 15, Josef's diagram): the sheet's visible border is
	// often the GEOMETRIC clip of the skirt against the ground, and with
	// relief retired the skirt is smooth - a featureless clip line no
	// matter what the alpha does. Displace the edge zone by the same
	// grain x fringe band the PS contest uses: the clip line itself goes
	// grainy, and geometry and alpha agree by construction. Exempt: carved
	// trenches and melt floors stay calm; fades out one band above ground
	// and with the same 1024-2048 distance range as the contest.
	// FIXED amplitude and reach (round 16): scaling these by the Untrampled
	// band displaced the whole sheet by half the slider - Josef's wave
	// field. Grain relief is a physical ~2 units whatever the dials say.
	const float kEdgeGrainAmp = 2.0;
	float edgeGrainH = z - terrainHeight;
	[branch] if (HasSnowHeight > 0.5 && edgeGrainH > -2.0 && edgeGrainH < 6.0)
	{
		float edgeGrainW = (1.0 - smoothstep(3.0, 6.0, edgeGrainH)) * (1.0 - smoothstep(1024.0, 2048.0, camDist));
		edgeGrainW *= 1.0 - smoothstep(0.05, 0.4, saturate(SampleDeformation(gridLocal)));
		edgeGrainW *= 1.0 - saturate(SampleExclusionMask(GridOrigin + gridLocal).y * 2.0);
		[branch] if (edgeGrainW > 0.001)
		{
			float hEdge = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, GridOrigin + gridLocal), 0.0.xx, snowMip);
			z += (hEdge - 0.5) * kEdgeGrainAmp * edgeGrainW;
		}
	}

	// Real relief from the PBR displacement map, through the SAME anti-tiling
	// taps the PS shades with, so the normal map's shading and the geometry
	// describe one surface. (Until 2026-08-17 this took a single un-offset
	// tap while the PS blended three at random per-cell offsets: the two
	// fields were decorrelated, so geometry bumps sat where the texture had
	// none. That is what made the relief read wrong against its own shading.)
	// Gated by local depth (thin cover and carved floors stay flat), by the
	// deformation (compressed snow is smooth), and faded with the same
	// distance band as the micro-normal.
	[branch] if (HasSnowHeight > 0.5 && SnowReliefDepth > 0.01)
	{
		float reliefFade = 1.0 - smoothstep(600.0, 2200.0, camDist);
		float depthAbove = z - terrainHeight;
		[branch] if (reliefFade > 0.001 && depthAbove > 0.5)
		{
			// Same uv and same world XY the PS feeds ComputeSnowTaps, so the
			// tap set here is the one that will shade this point.
			SnowTaps reliefTaps = ComputeSnowTapsNoGrad(snowUV, GridOrigin + gridLocal);
			float h = SampleSnowHeight(reliefTaps, 0.0.xx, snowMip);
			float carve = saturate(SampleDeformation(gridLocal));
			z += (h - 0.5) * SnowReliefDepth * reliefFade * saturate(depthAbove / 6.0) * (1.0 - carve);
		}
	}

	return FinishShellVertex(gridLocal, z, coverage, terrainHeight);
}
#endif


#ifdef PSHADER

// Depth-delta histogram (heatmap mode): 4 distance bands x 8 signed-delta
// buckets. SM5.0 shares PS UAV slots with the render-target outputs, so the
// heatmap runs as its own permutation with a single SV_Target — matching the
// kMAIN-only binding — and the histogram UAV at u1.
#	ifdef SNOW_LOD_HISTOGRAM
RWStructuredBuffer<uint> LODHistogram : register(u1);
#	endif

struct PS_OUTPUT
{
	float4 Diffuse : SV_Target0;
	// Conservative depth (may only move toward the camera): carries the
	// anti-z-fight clamp while keeping early-Z alive.
	float DepthLE : SV_DepthLessEqual;
#	ifndef SNOW_LOD_HISTOGRAM
	float4 MotionVectors : SV_Target1;
	float4 NormalGlossiness : SV_Target2;
	float4 Albedo : SV_Target3;
	float4 Specular : SV_Target4;
	float4 Reflectance : SV_Target5;
	float4 Masks : SV_Target6;
	float4 Masks2 : SV_Target7;
#	endif
};

PS_OUTPUT main(VS_OUTPUT input)
{
	// Same convention as MotionBlur::GetSSMotionVector.
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	// Coverage alpha recomputed per PIXEL from the terrain field (Terrain
	// Blending-style): smooth at texture resolution, independent of vertex
	// interpolation. The temporally-varying stochastic test then dithers the
	// boundary and TAA resolves it into a true cross-fade.
	float2 gridLocal = input.GridLocal;
	// Shaped (border-noised/smoothed) so the per-pixel coverage and ramp
	// dither agree with the shaped geometry.
	float3 pixelTerrain = SampleTerrainShaped(gridLocal);
	float pixelCoverage = saturate(pixelTerrain.z);
	float psEdgeFade = ShellEdgeFade(gridLocal);

	// Un-carved class depth ramp at this pixel. Where it goes negative the
	// shell is submerged (depth-rejected anyway). The dither rides the ramp:
	// alpha fades over the tail of positive depth, so a boundary toward
	// shallower/negative classes (where coverage stays ~1 and the coverage
	// term can't blend) dissolves stochastically as the shell thins, instead
	// of presenting a bare geometric plunge with a thin z-fight strip.
	// Mirror of the VS object-depth cap, so alpha and dither agree with the
	// capped geometry over captured objects.
	float pixelClassDepth = pixelTerrain.y;
	[branch] if (ObjectLiftCap > 0.0)
	{
		float2 capWorldXY = GridOrigin + gridLocal;
		float capField = SampleObjectHeight(capWorldXY);
		[flatten] if (capField > -50000.0)
		{
			// Boundary gate mirroring ShellSurfaceZ: no lift from bare ground.
			float capGround = smoothstep(0.1, 0.45, pixelCoverage) * (1.0 - saturate(SampleExclusionMask(capWorldXY).x));
			capField = lerp(min(capField, pixelTerrain.x), capField, capGround);
			float capLift = capField - pixelTerrain.x;
			float capT = smoothstep(0.25, 1.0, capLift / max(pixelClassDepth, 1.0));
			pixelClassDepth = lerp(pixelClassDepth, min(pixelClassDepth, SampleObjectDepthCap(capWorldXY)), capT);
			// Drift failsafe, mirroring ShellSurfaceZ: raised banks are snow
			// regardless of the ground class beneath.
			float liftForce = smoothstep(6.0, 20.0, capLift);
			pixelCoverage = max(pixelCoverage, liftForce);
			pixelClassDepth = max(pixelClassDepth, liftForce * 6.0);
		}
	}
	float pixelBare = saturate(1.0 - pixelCoverage);
	float pixelRampDepth = pixelClassDepth + (-8.0) * pixelBare;
	pixelRampDepth = lerp(-8.0, pixelRampDepth, psEdgeFade);

	// Depth reads shared by the edge dissolve, the proximity fade below and
	// the conservative depth clamp at the tail.
	float rawSceneDepth = SceneDepth.Load(int3(input.Position.xy, 0));
	float sceneZ = SharedData::GetScreenDepth(rawSceneDepth);
	float shellZ = input.CurrentClip.w;

	// User-tunable contest fringe (units): how far around the contact point
	// the height contest operates.
	float rampFadeBand = max(2.0, BorderUntrampledFade);
	// Mirror of the VS touch-down toe, so the alpha reads the sheet's
	// ACTUAL height above ground: without it the alpha cut lands while the
	// geometry still has height left, and the committed edge dies in
	// mid-air as a floating rim (round 10, Josef's hover screenshots).
	float pixelEffDepth = pixelRampDepth > 0.0 ? pixelRampDepth * smoothstep(0.0, 5.0, pixelRampDepth) : pixelRampDepth;
	// Two factors doing the two jobs the old smoothstep(0, band, depth) did
	// with one number (which is why fixing its floating cut flooded the
	// shore): the CLASS GATE enforces the assigned-depth design - blended
	// class depth must exceed ~2 units, so negative-depth classes (mud,
	// shoreline, -5) and their shallow blend plateaus never carry shell,
	// independent of the band slider (no recession). The CONTACT term is
	// the touchdown contest: w = 0.5 where the sheet meets the ground, so
	// the grain-vs-grain interlock plays out at the contact fringe (EM's
	// equal-weight boundary case), band = fringe width only.
	float rampTerm = smoothstep(1.0, 3.0, pixelRampDepth) * saturate(0.5 + pixelEffDepth / rampFadeBand);
	float coverageAlpha = smoothstep(0.0, 0.6, pixelCoverage) * psEdgeFade * rampTerm;

	// Object blending (Terrain Blending-style depth proximity): where the
	// shell hovers within a few units in front of any geometry behind it
	// (walkway planks, mesh roads, rocks), dissolve it into the dither. The
	// shell only knows terrain heights; this is what makes it meet statics
	// softly instead of slicing across them at the depth test. The gap is
	// measured along the view ray and shifts with the camera; widening the
	// band with distance turns that parallax wobble into a broad soft fade.
	// Mild distance scaling only: a steep view-Z-proportional band wobbles
	// with camera tilt even up close. Base kept short: a long band reads as
	// a stretched-out translucent margin wherever the shell nears geometry.
	float objectFadeBand = 5.0 + shellZ * 0.004;
	float proximityFade = saturate((sceneZ - shellZ) / objectFadeBand);
	// The grain-driven descent (DS) deliberately lays the rim onto its own
	// terrain, which this fade reads as hovering-over-geometry and crushes
	// to sparse specks (the round-5 dither regression). Under height
	// blending, exempt pixels whose backdrop IS the terrain: reconstruct the
	// scene surface's world height along the view ray and fade only where
	// something stands above the terrain data - planks, rocks, the fade's
	// actual clients. Sentinel terrain (-50000) reads as objectness 1, so
	// data gaps keep the plain fade.
	// Distance-gated like the contest (round 18: this previously keyed off
	// EM's height-blending checkbox via SnowHeightBlendSharpness).
	[branch] if (HasSnowHeight > 0.5 && shellZ < 2048.0)
	{
		float sceneSurfaceZ = ShellCameraPosAdjust.z + input.WorldPos.z * (sceneZ / max(shellZ, 1e-3));
		float objectness = smoothstep(1.5, 6.0, sceneSurfaceZ - pixelTerrain.x);
		proximityFade = max(proximityFade, 1.0 - objectness);
	}
	// Two situations hug the geometry behind them and must override the
	// fade: carved trench floors (terrain, actor feet in the trench) and the
	// shell riding a raised height field a few units above the surface
	// beneath. Without the override they get view-dependently dithered away.
	// The carve override is also what makes trenches end hard at class
	// borders while untrampled snow dissolves softly; Trampled Border Fade
	// scales the override away as the uncarved ramp thins, so walked snow
	// rejoins the soft dissolve at borders.
	float pixelCarve = saturate(SampleDeformation(gridLocal));
	float pixelLift = 0.0;
	float2 pixelShelter = SampleExclusionMask(GridOrigin + gridLocal);
	[branch] if (ObjectLiftCap > 0.0)
	{
		float fieldHeight = SampleObjectHeight(GridOrigin + gridLocal);
		[flatten] if (fieldHeight > -50000.0)
		{
			// Boundary gate mirroring ShellSurfaceZ: a lift the geometry no
			// longer takes must not hold the alpha override either.
			float liftGround = smoothstep(0.1, 0.45, saturate(pixelTerrain.z)) * (1.0 - saturate(pixelShelter.x));
			fieldHeight = lerp(min(fieldHeight, pixelTerrain.x), fieldHeight, liftGround);
			pixelLift = fieldHeight - pixelTerrain.x;
		}
	}
	// Fire-melted floors hug the terrain BY DESIGN (kFireMeltFloor above it);
	// without an override the proximity fade dithers them into translucency
	// like any other near-coincident surface.
	float pixelMelt = saturate(pixelShelter.y);
	// Trampled Border Fade retired (round 18): its default-0 behavior is the
	// keeper, so the window is the constant it resolved to.
	float carveOverride = smoothstep(0.1, 0.5, pixelCarve) * smoothstep(0.5, 1.0, pixelTerrain.y);
	coverageAlpha *= max(proximityFade, saturate(carveOverride + smoothstep(2.0, 10.0, pixelLift) + smoothstep(0.1, 0.4, pixelMelt)));

	// Height-blended edges (HEIGHT-BLEND-PLAN pairs 1+2): shape the COMBINED
	// alpha once, after the overrides - every partial band commits by grain
	// whatever fade produced it: the class ramp, the proximity dissolve, or
	// an override's own edge (the melt ring's 0.1-0.4 ramp printed an
	// unshaped dithered skirt when the components were shaped individually
	// before the overrides). Overrides still win: shaping preserves 0 and 1,
	// so a pixel an override holds at full alpha stays full. Mip outside the
	// branch (derivatives); the fetch fires only on partial alpha with height
	// blending on and the height map bound.
	float2 edgeSnowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	float edgeSnowMip = SnowHeightMip(edgeSnowUV);
	// Geometric edge contest (round 15, Josef's two-layer discovery): the
	// slice layer proved the look, and it never consulted EM's
	// height-blending checkbox - while this block did (edgeBlend), so with
	// that checkbox off the sheet silently fell back to a smooth hard cut
	// and ALL visible border detail came from the slices. The sheet now
	// uses the slices' own survival math: keep a pixel where the sheet's
	// surface (toe'd height + grain) stands above the dirt's grain surface,
	// decided geometrically, gated only by our own distance fade (the same
	// 1024-2048 range EM's terrain POM detail lives in). Multiplied in, so
	// the class design and the melt/carve/lift overrides keep their word;
	// dirt can only eat in, and the slice layer carries the outward side.
	float contestFade = 1.0 - smoothstep(1024.0, 2048.0, shellZ);
	[branch] if (HasSnowHeight > 0.5 && contestFade > 0.001 && coverageAlpha > 0.001 && coverageAlpha < 0.999)
	{
		float edgeSnowH = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, GridOrigin + gridLocal), 0.0.xx, edgeSnowMip);
		float hLandRaw = LandMasksCopy.Load(int3(input.Position.xy, 0)).y;
		// Fixed amplitudes (round 16: band-scaled grain magnified the
		// contest with the slider). The land's grain is the POM hit -
		// view-dependent - so it weighs in at HALF strength around neutral:
		// half the camera breathing, most of the detail; the snow-side
		// grain is world-anchored and stays full.
		const float kEdgeGrainAmp = 2.0;
		const float kEdgeDirtAmp = 2.0;
		float hLand01 = hLandRaw > 0.002 ? saturate((hLandRaw - 0.004) * (1.0 / 0.996)) : 0.5;
		float dirtSurf = (0.5 + (hLand01 - 0.5) * 0.5) * kEdgeDirtAmp;
		float snowSurf = pixelEffDepth + (edgeSnowH - 0.5) * kEdgeGrainAmp;
		float win = smoothstep(-0.25, 0.25, snowSurf - dirtSurf);
		coverageAlpha *= lerp(1.0, win, contestFade);
	}

	// Trench-floor contest (round 27, the second floor->0 gate): heavily
	// trampled floors run the SAME geometric contest as the edge - the
	// REMAINING snow (the carve profile's floor, the exact math the
	// geometry uses) against the dirt's grain - so wear-through opens
	// grain-shaped holes to the real ground by design instead of by window
	// error. Trench Floor Height is the dial with no new settings: at 3+
	// the remaining snow always beats the ~2-unit dirt grain (solid
	// floors, today's default, unchanged); toward 0, full trampling wears
	// through, dirt bumps piercing first. The tight carve gate keeps
	// trench WALLS solid - only trail floors contest. Runs after the carve
	// override on purpose: the override guarantees floors against fades,
	// and this is the one voice allowed to overrule it.
	[branch] if (HasSnowHeight > 0.5 && contestFade > 0.001 && pixelCarve > 0.75 && coverageAlpha > 0.001)
	{
		float floorEff = min(pixelEffDepth, BorderStyle.y * smoothstep(0.5, 8.0, pixelEffDepth));
		float remaining = max(pixelEffDepth * (1.0 - pixelCarve), floorEff);
		[branch] if (remaining < 4.0)
		{
			float floorGrain = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, GridOrigin + gridLocal), 0.0.xx, edgeSnowMip);
			float hLandFloorRaw = LandMasksCopy.Load(int3(input.Position.xy, 0)).y;
			float hLandFloor01 = hLandFloorRaw > 0.002 ? saturate((hLandFloorRaw - 0.004) * (1.0 / 0.996)) : 0.5;
			float dirtSurfFloor = (0.5 + (hLandFloor01 - 0.5) * 0.5) * 2.0;
			float snowSurfFloor = remaining + (floorGrain - 0.5) * 2.0;
			float winFloor = smoothstep(-0.25, 0.25, snowSurfFloor - dirtSurfFloor);
			coverageAlpha *= lerp(1.0, winFloor, contestFade * smoothstep(0.75, 0.95, pixelCarve));
		}
	}

	// Hard alpha test + optional outward dust. Survivors write fully
	// opaque, so no partial alpha reaches the deferred resolve through the
	// .w outputs below. Border Dithering ON scatters a whisker of
	// stochastic snow just BEYOND the cut (the 0.2..0.5 alpha tail) - dust
	// from the intersection onto the ground, never upward into the
	// committed sheet; OFF is a clean binary cut. (Round 18: the checkbox
	// previously gated a retired cross-fade path, which inverted its
	// apparent meaning - OFF showed the dust, ON showed nothing.)
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	[branch] if (ShellLODDebug == 1)
	{
		// Heatmap analyzes the covered snow surface only: bare/submerged
		// shell would read as fake poke-under. Depth test is ALWAYS in this
		// mode, so occlusion is re-created here for anything well behind the
		// scene surface.
		if (coverageAlpha < 0.05 || shellZ > sceneZ + 64.0)
			discard;
	}
	else if ((ShellDebugData == 0 || ShellDebugData >= 4) && ShellLODDebug == 0)
	{
		float dust = BorderStyle.x > 0.5
		                 ? saturate((coverageAlpha - 0.2) * (1.0 / 0.3))
		                 : (coverageAlpha >= 0.5 ? 1.0 : 0.0);
		if (screenNoise * screenNoise >= dust)
			discard;
		coverageAlpha = 1.0;
	}

	// Normal = smooth interpolated terrain normal + per-pixel gradient of
	// the shared carve profile (central differences at the deformation
	// map's resolution), so trench walls AND the edge berm shade by the
	// same shape the geometry displaces.
	const float step = 4.0;
	float dXP = SampleDeformation(gridLocal + float2(step, 0.0));
	float dXN = SampleDeformation(gridLocal - float2(step, 0.0));
	float dYP = SampleDeformation(gridLocal + float2(0.0, step));
	float dYN = SampleDeformation(gridLocal - float2(0.0, step));

	float3 terrainNormal = normalize(input.TerrainNormalAlpha.xyz);
	float pixelDepth = max(pixelRampDepth, 0.0);
	float2 profileGrad = float2(
		CarveProfile(saturate(dXP), pixelDepth) - CarveProfile(saturate(dXN), pixelDepth),
		CarveProfile(saturate(dYP), pixelDepth) - CarveProfile(saturate(dYN), pixelDepth)) / (2.0 * step);
	// Berm shading: numerical gradient of the SAME blurred hill the
	// geometry displaces by, so the light/shadow break sits on the hill's
	// true flanks (the analytic shortcut put the terminator on the crest).
	float bermXP = BermField(gridLocal + float2(step, 0.0));
	float bermXN = BermField(gridLocal - float2(step, 0.0));
	float bermYP = BermField(gridLocal + float2(0.0, step));
	float bermYN = BermField(gridLocal - float2(0.0, step));
	// Differenced WITH the (1 - deformation) mask, so the shading gradient is
	// of the exact surface the vertex path displaces - an unmasked gradient
	// here shades a hill the geometry no longer has.
	float2 bermGrad = float2(
		BermShape(bermXP) * saturate(1.0 - dXP) - BermShape(bermXN) * saturate(1.0 - dXN),
		BermShape(bermYP) * saturate(1.0 - dYP) - BermShape(bermYN) * saturate(1.0 - dYN)) / (2.0 * step);
	float bermCenter = 0.25 * (bermXP + bermXN + bermYP + bermYN);
	float2 gradZ = -terrainNormal.xy / max(terrainNormal.z, 0.1) + profileGrad + bermGrad * pixelDepth * BermHeightAmp;

	// Undulation gradient (same field the VS displaced by) shades the dunes.
	float2 worldXYPS = GridOrigin + gridLocal;
	float undScale = saturate(pixelDepth / 8.0);
	[branch] if (undScale > 0.001)
	{
		const float uStep = 12.0;
		float uXP = Undulation(worldXYPS + float2(uStep, 0.0));
		float uXN = Undulation(worldXYPS - float2(uStep, 0.0));
		float uYP = Undulation(worldXYPS + float2(0.0, uStep));
		float uYN = Undulation(worldXYPS - float2(0.0, uStep));
		gradZ += float2(uXP - uXN, uYP - uYN) / (2.0 * uStep) * undScale;
	}

	// Churn gradient (same field the geometry displaces by).
	float churnW = ChurnWeight(pixelCarve, bermCenter) * saturate(pixelDepth / 10.0);
	[branch] if (churnW > 0.001 && ChurnHeightAmp > 0.01)
	{
		const float cStep = 3.0;
		float cXP = ChurnNoise(worldXYPS + float2(cStep, 0.0));
		float cXN = ChurnNoise(worldXYPS - float2(cStep, 0.0));
		float cYP = ChurnNoise(worldXYPS + float2(0.0, cStep));
		float cYN = ChurnNoise(worldXYPS - float2(0.0, cStep));
		gradZ += float2(cXP - cXN, cYP - cYN) / (2.0 * cStep) * ChurnHeightAmp * churnW;
	}

	float3 normalWS = normalize(float3(gradZ * -1.0, 1.0));

	// Snow texture taps, shared by albedo, normal and RMAOS so every map
	// agrees on the same anti-tiling offsets. Micro-relief fades with
	// distance, where the grain frequency aliases instead of detailing.
	float bumpFade = 1.0 - smoothstep(600.0, 2200.0, shellZ);
	// The distance fade WITHOUT the crust flattening applied. The flattening
	// exists to take the powder grain away; the frost crystal replacing it
	// must not be taken away by the same term.
	const float bumpFadeRaw = bumpFade;

	// Crust, sampled once and used three times. Flattening the normal map is
	// the strongest of the three by a distance: powder reads as grain, and ice
	// reads as a SHEET, so smoothing the surface says "frozen over" far louder
	// than any change to reflectance or colour can. Roughness and specular
	// alone were nearly invisible without it.
	float crustAmount = saturate(SampleCrust(gridLocal) * SpellShading.y);
	bumpFade *= lerp(1.0, 1.0 - saturate(SpellShading.w), crustAmount);
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	SnowTaps snowTaps = ComputeSnowTaps(snowUV, worldXYPS);
	// Uniform flow: the parallax shadow branch below is divergent, and
	// derivatives taken inside it would be garbage at its edges.
	float snowHeightMip = SnowHeightMip(snowUV);
	// Two-plane projection (Stage 2 P3, the statics shell's recipe): trench
	// walls are near-vertical, and the top-down uv stretches down them as
	// smears. Steep pixels blend in a side-plane SAMPLE via SampleSnowPlanar;
	// keyed on the per-pixel trench normal, so only walls pay the second tap
	// set. Captured before the normal map perturbs normalWS - the side POM
	// march must resolve the view into the SAME plane these uvs use.
	// Earlier and narrower than the statics ramp (0.55-0.25) on purpose:
	// landscape trench walls live at 40-65 degrees, and any top-projection
	// share there is a heavily foreshortened smear whose anisotropic
	// footprint boils as the camera moves - the mid-wall shimmer of the
	// round-3 A/B. Side takes over fully by ~57 degrees; statics keeps its
	// ramp because its drape sides are near-vertical and never sit in the
	// band.
	float snowSteepness = smoothstep(0.75, 0.55, abs(normalWS.z));
	float snowWorldZAbs = input.WorldPos.z + ShellCameraPosAdjust.z;
	bool snowSideDropsX = abs(normalWS.x) > abs(normalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXYPS.y, snowWorldZAbs) : float2(worldXYPS.x, snowWorldZAbs);
	// NO SnowUVOffset on the side plane, and a STATIC 4096-unit fold (24
	// tiles exactly - the glint-fold trick). SnowUVOffset compensates
	// gridLocal's rebase, but the side plane is ABSOLUTE coordinates that
	// never rebase - adding the offset slid the wall texture by the scroll
	// amount every time the camera-following grid advanced: a wall that
	// redraws under camera translation and holds still under rotation,
	// which is the round-1..7 wall-shift saga, settled by the mode-5 unlit
	// view still shifting. The fold keeps the uv small for float precision;
	// derivatives and the mip come from the UNFOLDED uv so the fold seam
	// cannot spike them.
	float2 snowUVSideUnfolded = snowSidePlane / kSnowUVTile;
	float2 snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	SnowTaps snowTapsSide = ComputeSnowTaps(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = ddx(snowUVSideUnfolded);
	snowTapsSide.duvdy = ddy(snowUVSideUnfolded);
	float snowHeightMipSide = SnowHeightMip(snowUVSideUnfolded);
	// Tangent basis for the snow maps. The snow uv is a world-XY planar
	// projection, so the frame is axis-aligned by construction: bumpT is
	// world +X (uv.x), bumpB world +Y (uv.y). Built from the geometric normal
	// BEFORE the normal map perturbs it, matching Lighting.hlsl's use of the
	// interpolated TBN. Shared with the parallax shadow below.
	float3 bumpT = normalize(cross(float3(0.0, 1.0, 0.0), normalWS) + float3(1e-5, 0.0, 0.0));
	float3 bumpB = cross(normalWS, bumpT);

	float3 V = -normalize(input.WorldPos);

	// Pre-parallax derivatives for the glint grid, mirroring Lighting.hlsl's
	// uvOriginal: the POM offset below is view-dependent, and glints must not
	// ride it. (The glint uv itself is rebuilt world-anchored at the call.)
	const float2 glintDuvdx = snowTaps.duvdx;
	const float2 glintDuvdy = snowTaps.duvdy;

	// Parallax occlusion: the depth the shell was missing. The normal map
	// only tilts the lighting; this moves the texture itself, so grain
	// occludes grain and the surface reads as thick. Runs BEFORE every snow
	// fetch, and shifts the tap set rather than rebuilding it, so albedo,
	// normal, RMAOS and the parallax shadow all ride the displaced position.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.z > 0.001 && bumpFade > 0.001)
	{
		// bumpT/bumpB ARE the uv axes (world-XY planar projection); EM's
		// marcher builds tangent-space view from this frame itself.
		float3x3 snowTbn = float3x3(bumpT, bumpB, normalWS);
		DisplacementParams pomParams = SnowDisplacementParams();
		pomParams.HeightScale *= SnowParallax.z;
		float2 pomOffset = SnowParallaxOffset(snowTaps, snowUV, V, snowTbn, shellZ, snowHeightMip, screenNoise, pomParams);
		// Trench walls march NOTHING (round 3). A march is only honest when
		// the surface lies in its projection plane; a 40-60 degree wall lies
		// in neither the top plane nor the side plane, so BOTH marches
		// resolve the view through a mismatched frame and redraw the wall
		// whenever the camera changes position (round 2's steepness scale
		// still left 40-90% of the top march live on exactly those walls -
		// the smoothstep only saturates near vertical). The hard tilt cut
		// below keeps the full march on the gentle slopes that always had
		// it, kills it by ~40 degrees, and the side plane samples UNMARCHED
		// - at a 21-unit wall, stability is worth far more than parallax.
		pomOffset *= 1.0 - smoothstep(0.15, 0.35, 1.0 - abs(normalWS.z));
		snowUV += pomOffset;
		snowTaps = OffsetSnowTaps(snowTaps, pomOffset);
	}

	[branch] if (HasSnowNormal > 0.5 && bumpFade > 0.001)
	{
		float3 texN = SampleSnowPlanar(SnowNormalMap, snowTaps, snowTapsSide, snowSteepness).xyz * 2.0 - 1.0;
		texN.z = sqrt(saturate(1.0 - dot(texN.xy, texN.xy)));
		texN.y = -texN.y;  // DDS v grows down; our uv v grows with world +Y
		normalWS = normalize(normalWS + (bumpT * texN.x + bumpB * texN.y) * bumpFade);
	}

	// Frost crystal, laid ON TOP of the flattened powder grain rather than
	// instead of it. The flattening above is still what says "frozen over" -
	// powder is grain and ice is a sheet - and this puts the rime back as its
	// own structure, which is a different thing at a different scale.
	//
	// Fetched ONCE here and used three times below: the normal, the albedo and
	// the polish. They have to agree about where a crystal is, and the polish
	// especially - see the roughness block.
	FrostTaps frost;
	frost.normal = float3(0.0, 0.0, 1.0);
	frost.crystal = 0.0;
	frost.valid = false;
	const float frostAmount = (CrustLook2.w > 0.5) ? crustAmount * saturate(CrustLook2.y) : 0.0;
	[branch] if (frostAmount > 0.001)
	{
		frost = SampleFrostPattern(worldXYPS, CrustLook2.z);
		normalWS = normalize(normalWS +
							 (bumpT * frost.normal.x + bumpB * frost.normal.y) * frostAmount * bumpFadeRaw);
	}
	else if (HasSnowTexture != 0 && bumpFade > 0.001)
	{
		const float kBumpTile = 64.0;
		const float kBumpHeight = 0.55;
		float2 texDims;
		SnowDiffuse.GetDimensions(texDims.x, texDims.y);
		float e = 1.5 / texDims.x;
		float2 detailUV = worldXYPS / kBumpTile;
		const float3 kLum = float3(0.30, 0.45, 0.25);
		float h0 = dot(SnowDiffuse.Sample(SnowSampler, detailUV).rgb, kLum);
		float hx = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(e, 0.0)).rgb, kLum);
		float hy = dot(SnowDiffuse.Sample(SnowSampler, detailUV + float2(0.0, e)).rgb, kLum);
		float2 bumpGrad = float2(hx - h0, hy - h0) * (kBumpHeight / (e * kBumpTile));
		normalWS = normalize(normalWS + float3(-bumpGrad * bumpFade, 0.0));
	}

	float3 viewNormal = normalize(mul((float3x3)CameraView, normalWS));

	// Snow material: the modlist's snow diffuse when available, otherwise a
	// bright, slightly blue constant.
	float3 kSnowAlbedo = float3(0.82, 0.84, 0.88);
	// Wall-material debug (ShellDebugData 5): the blended two-plane albedo
	// exactly as sampled, before any lighting, spell mark or compaction
	// touches it.
	float3 dbgAlbedoRaw = kSnowAlbedo;
	[branch] if (HasSnowTexture != 0)
	{
		kSnowAlbedo = SampleSnowPlanar(SnowDiffuse, snowTaps, snowTapsSide, snowSteepness).rgb;
		dbgAlbedoRaw = kSnowAlbedo;
		// PBR-authored textures store linear color; the rest of this path works
		// in the pipeline's gamma space. Auto-enabled when the PBR set resolved.
		[flatten] if (SnowTextureIsLinear != 0.0)
			kSnowAlbedo = Color::LinearToSrgb(kSnowAlbedo);
	}
	// Scorch: a shock discharge leaves the snow burnt where it struck. Darkened
	// rather than recoloured, and biased slightly warm, so it reads as fouled
	// snow instead of a grey decal painted over it.
	{
		float scorch = SampleScorch(gridLocal) * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
	// Compaction weight (Stage 1): the churn field, so the matte lands
	// exactly on floors, walls and berms and never draws its own boundary.
	// The darken/roughen halves were retired - IBL + DALC already darken
	// trenches - so this feeds the glint suppression alone.
	float churnMat = ChurnWeight(pixelCarve, bermCenter);
	// PBR snow material: GGX microfacet specular with Fresnel and energy-
	// conserving lobes. Light and ambient stay in the frame's units
	// (DirLightColor is already pi-scaled by pipeline convention, so no
	// Lambert 1/pi on diffuse); the indirect specular lobe goes to the
	// Reflectance RT where the composite applies cubemap and ambient
	// specular like any TruePBR surface.
	float kSnowRoughness = 0.6;
	float3 kSnowF0 = float3(0.028, 0.028, 0.028);

	// Crust: snow that melted and refroze is ice, not powder. The normal map
	// was already flattened above; here the highlight tightens, the surface
	// picks up reflectance, and a colour cast is applied - all three on
	// sliders, because how icy this should read is a matter of taste and the
	// physically honest values were far too subtle to see.
	// Only the colour cast belongs here. Roughness and reflectance are applied
	// AFTER the RMAOS block below, which overwrites both from the snow material
	// whenever a PBR set is installed - setting them here threw them away one
	// line later, which is why smoothness and tint were the only two of four
	// crust knobs that did anything.
	[branch] if (crustAmount > 0.001)
		kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(CrustLook.y, CrustLook.z, CrustLook2.x), crustAmount);
	// A whisper of the pattern in the albedo too, so the crystal reads even
	// where nothing is catching a highlight. Kept faint on purpose: snow is
	// already near white, so this can only ever darken the gaps between
	// crystals rather than brighten the crystals themselves.
	[branch] if (frost.valid)
		kSnowAlbedo *= lerp(1.0, lerp(0.94, 1.0, frost.crystal), frostAmount);

	// Per-pixel PBR response from the RMAOS map (TruePBR channel layout:
	// roughness / metallic / AO / specular level), with the landscape
	// config's authored scales.
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


	// Crust polishes whatever the material ended up being, PBR set or not. It
	// has to come after the block above rather than before it, or an installed
	// RMAOS map silently discards both.
	[branch] if (crustAmount > 0.001)
	{
		snowRoughness = lerp(snowRoughness, SpellShading.z, crustAmount);
		snowF0 = lerp(snowF0, CrustLook.xxx, crustAmount);
	}

	// The crystal has to be the part that SHINES, and adding a normal map is
	// what stops it: tilting a facet away from the light moves the highlight
	// off the crystal and onto the flat gaps between them, so the pattern came
	// out inside-out - matte structure on glossy ground. The polish is
	// therefore modulated by the same tap that shaped the normal, so a facet is
	// smoother and more reflective than the ground it stands on, and the
	// highlight lands where the ice actually is.
	[branch] if (frost.valid)
	{
		snowRoughness = saturate(snowRoughness * lerp(1.0, lerp(1.35, 0.45, frost.crystal), frostAmount));
		snowF0 = snowF0 * lerp(1.0, lerp(0.75, 1.7, frost.crystal), frostAmount);
	}

	float3 L = SharedData::DirLightDirection.xyz;
	float satNdotL = saturate(dot(normalWS, L));
	float satNdotV = saturate(abs(dot(normalWS, V)) + 1e-5);

	float worldShadow = ShadowSampling::GetWorldShadow(input.WorldPos, ShellCameraPosAdjust.xyz);
	// Distant shadow softening: the far cascade's texels quantize into hard
	// blocky patches on distant snow. The cascades must NOT be faded out;
	// LOD trees cast into them and bare ground keeps their shadows at range;
	// so the crisp path instead WIDENS its PCF ring with distance: same
	// shadows, soft penumbra blobs instead of blocks.
	float farShadowT = smoothstep(6000.0, 15000.0, length(input.WorldPos));
	float sunShadow;
	[branch] if (CrispShadows > 0.5)
	{
		// Full-resolution comparison PCF against the game's raw cascade
		// atlas: the same crisp tree/actor shadows bare ground receives.
		sunShadow = worldShadow * SnowShadow::GetCascadeShadow(input.WorldPos, normalWS, lerp(1.0, 6.0, farShadowT), uint2((uint)BorderStyle.z, (uint)BorderStyle.w));
	}
	else
	{
		// Fallback: the Volumetric Shadows 512px VSM moments copy (blurry).
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(input.WorldPos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}

	// Heightfield self-shadowing: the shell is a heightfield, so march it
	// toward the sun and find the horizon this pixel must clear. Hills,
	// mounds, field raises and the dune undulation all cast soft shadows
	// onto the snow behind them; contact detail the game's cascades cannot
	// hold. Geometric growth in the tap distances gives sharp close shadows
	// and long soft ones at low sun angles.
	[branch] if (sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		static const float kMarchDist[5] = { 28.0, 70.0, 170.0, 420.0, 1000.0 };
		float sunLen2D = max(length(L.xy), 1e-4);
		float sunTan = L.z / sunLen2D;
		float2 stepDir = L.xy / sunLen2D;
		float surfZ = input.WorldPos.z + ShellCameraPosAdjust.z;
		float horizonTan = -10.0;
		[unroll] for (uint marchI = 0; marchI < 5; marchI++)
		{
			float d = kMarchDist[marchI];
			float2 sampleLocal = gridLocal + stepDir * d;
			float3 st = SampleTerrain(sampleLocal);
			float sampleDepth = max(st.y, 0.0);
			// The march must see the CARVED surface (same floor rule as the
			// geometry): without the carve, a wide trench reads as ringed by
			// full-height snow and sits in permanent shadow even facing the
			// sun. Same rule for MELT bowls (fires, workspace clearings):
			// without the melt, every bowl-floor pixel reads as ringed by
			// full-height snow and the whole bowl darkens.
			float2 sampleMask = SampleExclusionMask(GridOrigin + sampleLocal);
			{
				float sampleMelt = saturate(sampleMask.y);
				sampleDepth = lerp(sampleDepth, min(sampleDepth, kFireMeltFloor), sampleMelt);
			}
			float sampleDeform = saturate(SampleDeformation(sampleLocal));
			// The berm occluder reads the berm FIELD, as the geometry does. It
			// used to read the RAW deformation, and BermShape of a raw value
			// peaks across the trench's sloping wall - so the march saw every
			// trail ringed by a phantom ridge and laid shadow bands on flat,
			// sun-facing snow beside it: Josef's "shadows where there should
			// be none". One bilinear tap when the bake is live; the unbaked
			// A/B path skips the term rather than paying 17 taps per march
			// step, and under-occludes its berms slightly.
			float sampleBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : 0.0;
			sampleDepth = CarveProfile(sampleDeform, sampleDepth) +
			              BermShape(sampleBerm) * saturate(1.0 - sampleDeform) * sampleDepth * BermHeightAmp;
			float sh = st.x + sampleDepth + Undulation(GridOrigin + sampleLocal) * saturate(sampleDepth / 8.0);
			[branch] if (ObjectLiftCap > 0.0)
			{
				float sf = SampleObjectHeight(GridOrigin + sampleLocal);
				[flatten] if (sf > -50000.0)
				{
					// Boundary gate mirroring ShellSurfaceZ: banks the geometry
					// no longer raises must not occlude the march either.
					float sampleGround = smoothstep(0.1, 0.45, saturate(st.z)) * (1.0 - saturate(sampleMask.x));
					sf = lerp(min(sf, st.x), sf, sampleGround);
					sh = max(sh, sf + sampleDepth);
				}
			}
			horizonTan = max(horizonTan, (sh - surfZ) / d);
		}
		// Near: a crisp penumbra band. Far: a much wider penumbra plus
		// attenuated strength; the march's per-texel horizon steps stop
		// reading as hard-edged blocks on distant snow.
		float soft = lerp(0.06, 0.35, farShadowT);
		sunShadow *= lerp(smoothstep(-0.12 - (soft - 0.06) * 2.0, soft, sunTan - horizonTan), 1.0, 0.7 * farShadowT);
	}

	// SSS gate diagnostics for ShellDebugData 4: x = mask darkness (what
	// the ground-marched mask wants to print here), y = gate trust,
	// z = the buried-caster probe fired. Stays black when the feature is
	// off or inactive - itself a diagnostic.
	float3 sssDebug = float3(0.0, 0.0, 0.0);

	// Screen-Space Shadows (the integrated long-range depth march): these
	// carry the distant LOD tree shadows far beyond the two cascades. The
	// texture was marched on the prepass depth (the ground under the
	// shell), so applying it near paints barrel/object shadows straight
	// through the snow. Near, the crisp cascades already shadow the shell
	// correctly; SSS blends in only beyond them, where it is the only
	// shadow source and the shell hugs the very ground the march ran on.
	[branch] if (ScreenSpaceShadowsActive > 0.5)
	{
		// Depth agreement is the gate (round 18): the march ran on the
		// PRE-shell depth, so trust the mask only where the shell hugs the
		// surface the march actually saw. Measured VERTICALLY, not along
		// the view ray: the along-ray gap is snow depth / sin(elevation),
		// which explodes at far grazing views - that explosion is why
		// 38614476 forced SSS fully on past 2500 units, and that override
		// is exactly what painted buried-terrain shadows through distant
		// drifts. The vertical gap does not blow up with view angle, so
		// one rule now covers every range and the override is GONE: far
		// hugging ground keeps its LOD tree shadows, a deep drift
		// suppresses at any distance. The along-ray term survives only as
		// a wide backstop for steep faces seen edge-on, where the marched
		// surface is a genuinely different surface at a small vertical
		// gap. Thresholds cannot separate grass shadows from buried
		// prints (their gaps overlap - three rounds of evidence); the
		// CASTER can, and the discriminator below does.
		sssDebug.z = 0.0;
		float sssRayGap = sceneZ - shellZ;
		float sssVertGap = abs(input.WorldPos.z) * sssRayGap / max(shellZ, 1e-3);
		float sssBlend = (1.0 - smoothstep(8.0, 24.0, sssVertGap)) *
		                 (1.0 - smoothstep(150.0, 400.0, sssRayGap));
		// Buried-caster discriminator: three sunward taps of the object-top
		// raster; any captured surface standing above this pixel's shell
		// means the mask's darkness here is that object's buried shadow.
		[branch] if (ObjectLiftCap > 0.0 && sssBlend > 0.001)
		{
			float2 sunXY = L.xy / max(length(L.xy), 1e-4);
			float sssSurfZ = input.WorldPos.z + ShellCameraPosAdjust.z;
			// Taps start at 15 units, not 40: a caster one plank-width away
			// (the Dawnstar stair boards) slipped BETWEEN the pixel and the
			// old first tap, so all three overshot it and its buried shadow
			// printed in the trench dips - where the hug gate passes by
			// design, because a carved floor hugs the ground. Same far reach.
			[unroll] for (uint sssI = 0; sssI < 4; sssI++)
			{
				float2 tapWorldXY = GridOrigin + gridLocal + sunXY * (15.0 + 55.0 * sssI);
				float2 tapDims;
				bool tapValid;
				float2 tapTexel = ObjectMapTexel(tapWorldXY, tapDims, tapValid);
				[flatten] if (tapValid)
				{
					float tapTop = ObjectTopsRaw.Load(int3((int2)tapTexel, 0));
					// A buried caster is by definition LOW: stair planks and
					// fence rails poke barely above the snow, so the old
					// "stands > 8 above the shell" trigger missed exactly
					// the casters that print (the Dawnstar pair). Any
					// captured surface from slightly-buried upward counts -
					// grass is never captured, so grass shadows cannot be
					// touched by this no matter the threshold.
					[flatten] if (tapTop > -50000.0 && tapTop > sssSurfZ - 16.0)
					{
						sssBlend = 0.0;
						sssDebug.z = 1.0;
					}
				}
			}
		}
		// Near field, the mask is a CONTACT term, not a shadow map: its
		// casters' true shadows already come from the cascades, so at full
		// strength every trusted ground-marched silhouette doubles as a
		// hard offset print - the player's own blob being the uncatchable
		// case, since actors are never captured and no probe can see them.
		// 35% near reads as soft contact darkening and keeps the grass
		// shadows; full strength returns by 2500, where SSS is the only
		// source of LOD tree shadows (r107).
		sssBlend *= lerp(0.35, 1.0, smoothstep(800.0, 2500.0, shellZ));
		float sssMask = ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);
		sunShadow *= lerp(1.0, sssMask, sssBlend);
		sssDebug.x = 1.0 - sssMask;
		sssDebug.y = sssBlend;
	}

	// Parallax self-shadow on the snow's own grain: Extended Materials'
	// GetParallaxSoftShadowMultiplier, the term PBR ground already receives
	// and the shell did not, which is why the shell read flat under low sun
	// beside shaded ground. Four fixed taps along the light in tangent space,
	// no march. Distinct from the heightfield march above: that one shadows at
	// TERRAIN scale (mounds, berms, dunes, 28-1000 units); this one shadows
	// WITHIN one texture repeat.
	//
	// The four fetches are ours (tap-blended) but every constant and the
	// occlusion formula are Extended Materials' own, so the response matches
	// the ground beside us by construction rather than by tuning.
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.y > 0.001 && bumpFade > 0.001 &&
		sunShadow > 0.01 && satNdotL > 0.001)
	{
		// Light into the snow uv's own frame. bumpT/bumpB ARE the uv axes, so
		// this is the planar-projection equivalent of mul(DirLightDirection, tbn).
		float2 lightUV = float2(dot(L, bumpT), dot(L, bumpB));
		float2 lightUVSide = snowSideDropsX ? float2(L.y, L.z) : float2(L.x, L.z);
		float occlusion = SnowParallaxOcclusionPlanar(snowTaps, snowTapsSide, snowSteepness,
			lightUV, lightUVSide, snowHeightMip, snowHeightMipSide,
			SnowParallaxQuality(shellZ), screenNoise, SnowDisplacementParams());

		float parallaxShadow = 1.0 - saturate(occlusion * SnowParallax.y);
		// Faded on the same band as the normal map it occludes: past it the
		// grain is not drawn, so shadowing it would darken nothing visible.
		// ALSO faded out on walls: the march is jittered by screenNoise
		// (screen-anchored, per-frame) with a distance-driven tap count, so
		// on a wall's high-contrast side-projected grain it reads as
		// shading that crawls when the camera moves. Wall shading comes
		// from the trench self-shadow march and the cascades instead.
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}

	float3 sunLight = SharedData::DirLightColor.xyz * sunShadow;

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1): glints, energy conservation and every future
	// TruePBR lobe ride the shared code. Outputs are Lighting-internal units;
	// Color::PBRLightingScale is applied at the write tail below.
	// World-anchored glint uv: snowUV's fmod(GridOrigin) fold shifts by whole
	// tiles as the camera-following grid advances - a no-op for the periodic
	// texture, but the glint hash is NOT tile-periodic, so every ~2.4m of
	// travel re-rolled the whole sparkle field. Fold on a STATIC 4096-unit
	// world block instead (an exact tile multiple): seams are fixed lines in
	// the world, invisible in a stochastic field. Derivatives are unchanged.
	const float2 glintUV = fmod(GridOrigin + gridLocal, 4096.0) / kSnowUVTile;
	// Built once, shared by the sun and every point light (M3).
	// Compaction thins the glint field toward the smooth-GGX fallback: at
	// MinGlintDensity (1.0) the density sits under SnowBuildMaterial's 1.1
	// gate and the sparkle is gone entirely. Log-space lerp, so partial
	// churn reads as sparser glitter rather than dimmer.
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

	// Ice reads at GRAZING angles, where a sheet catches the sky and powder
	// does not. Snow is already near-white, so a specular lobe has almost no
	// headroom left above it and the honest BRDF response is swallowed - this
	// adds the one thing white snow cannot already be doing.
	[branch] if (crustAmount > 0.001)
	{
		float grazing = pow(1.0 - satNdotV, 4.0);
		directSpecular += grazing * crustAmount * CrustLook.w * sunLight;
	}

	// Placed lights (fires, lanterns): the clustered LLF list, with each
	// shadow-casting light's own map sampled at the shell surface.
	[branch] if (PointLightsActive > 0.5)
	{
		float viewZ = mul(CameraView, float4(input.WorldPos, 1.0)).z;
		float4 clip = mul(CameraViewProj, float4(input.WorldPos, 1.0));
		float2 clusterUV = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
		SnowLights::AccumulatePointLights(snowMtl, input.WorldPos, input.WorldPos + ShellCameraPosAdjust.xyz,
			normalWS, V, viewZ, clusterUV, glintUV, glintDuvdx, glintDuvdy, directDiffuse, directSpecular);
	}

	// No AO here: the routed GetIndirectLobeWeights already folds snowAO into
	// diffuseLobe via MultiBounceAO (PBR.hlsli:276), exactly as ground does.
	// Multiplying again was double-counting - it read as darker nights and
	// less-blue days, ambient being where the sky blue lives.
	float3 ambientColor = SnowAmbientColor(normalWS);
	float3 ambientPart = ambientColor * diffuseLobe;
	// The land's real baked vertex AO under this pixel, by ground's recipe
	// (Lighting.hlsl:2633-2636): linearized max component, VertexAOStrength
	// lerp. Skylighting darkens only BEYOND it (the function divides), and
	// the composite's SSGI reads its complement from Masks2.
	float landVertexAO = Color::ColorToLinear(SampleTerrainVertexAO(gridLocal).xxx).x;
	landVertexAO = lerp(1.0, landVertexAO, SharedData::truePBRSettings.VertexAOStrength);
	// Skylighting parity with Lighting.hlsl's deferred tail: the ambient is
	// darkened by the probe volume with the same multi-bounce term terrain
	// uses (ApplySkylighting passes the SCALED albedo).
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

	[branch] if (ShellDebugData == 2)
	{
		// Exclusion debug: R = drift field lift (48 units = full red),
		// G = melt fraction (fires, workspaces, shelter), B = door
		// suppression. Black = untouched by any of them.
		float2 dbgWorldXY = GridOrigin + gridLocal;
		float2 dbgMask = SampleExclusionMask(dbgWorldXY);
		float dbgField = SampleObjectHeight(dbgWorldXY);
		float dbgLift = dbgField > -50000.0 ? max(dbgField - pixelTerrain.x, 0.0) : 0.0;
		preLit = float3(saturate(dbgLift / 48.0), saturate(dbgMask.y), saturate(dbgMask.x) * 0.7);
	}
	else if (ShellDebugData == 5)
	{
		// Wall material view, on the REAL surface with real discards: the
		// raw two-plane albedo, unlit - no sun, no shadows, no glints, no
		// self-shadow march - plus a red wash showing the side-projection
		// weight. THE strafe test: if the wall still shifts in THIS view,
		// the texture path is guilty (taps/uv/projection); if this view is
		// rock-solid and the normal view shifts, a LIGHTING term is guilty.
		preLit = lerp(dbgAlbedoRaw, float3(1.0, 0.1, 0.1), snowSteepness * 0.25);
	}
	else if (ShellDebugData == 4)
	{
		// SSS gate view. RED = the mask's darkness at this shell pixel
		// (marched on the ground BENEATH the shell), GREEN = how much the
		// vertical hug gate trusts it, BLUE = the buried-caster probe
		// fired and killed it. A shadow print on the snow = red together
		// with green and NO blue. All black = the mask never reached the
		// shell here (feature off / inactive).
		preLit = sssDebug;
	}
	else if (ShellDebugData == 3)
	{
		// Border-field debug: hue bands the shaped class depth (the field
		// the cut and the slice ribbon key off), brightness rides the snow
		// grain, WHITE marks the cut contour (raw ~2), and a magenta grid
		// marks pixels whose pre-shell G-buffer carries land grain data.
		// Reading it: how far the orange ribbon wanders from the white line
		// IS the bleed; missing magenta explains one-sided fallbacks.
		float dbgRaw = pixelTerrain.y;
		float3 dbgBand =
			dbgRaw < -0.5 ? float3(0.5, 0.05, 0.05) :
			dbgRaw < 1.0  ? float3(0.9, 0.5, 0.1) :
			dbgRaw < 3.0  ? float3(0.1, 0.8, 0.2) :
			dbgRaw < 8.0  ? float3(0.1, 0.7, 0.8) :
							float3(0.15, 0.2, 0.9);
		float dbgGrain = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, GridOrigin + gridLocal), 0.0.xx, edgeSnowMip);
		preLit = dbgBand * (0.4 + 0.6 * dbgGrain);
		[flatten] if (abs(dbgRaw - 2.0) < 0.15)
			preLit = float3(1.0, 1.0, 1.0);
		float dbgLand = LandMasksCopy.Load(int3(input.Position.xy, 0)).y;
		[flatten] if (dbgLand > 0.002 && frac(input.Position.x / 8.0) < 0.15 && frac(input.Position.y / 8.0) < 0.15)
			preLit = lerp(preLit, float3(1.0, 0.0, 1.0), 0.7);
	}
	else if (ShellDebugData != 0)
	{
		// R = height, G = per-pixel coverage, B = ramp depth (40 units = full
		// blue); class boundaries show as blue-intensity steps.
		float heightNorm = saturate((input.DebugHeight + 4000.0) / 8000.0);
		bool isSentinel = input.DebugHeight < -50000.0;
		preLit = isSentinel ? float3(0.0, 0.0, 0.5) : float3(heightNorm * 0.25, pixelCoverage * 0.5, saturate(pixelTerrain.y / 40.0));
	}

	[branch] if (ShellLODDebug == 1)
	{
		// Signed vertical gap between the shell surface and whatever the
		// scene rendered along this pixel's ray (positive = shell above the
		// ground; approximate at grazing angles). Buckets feed the histogram
		// and the color bands: reds = shell buried under the rendered
		// ground, yellow = inside z-fight range, greens/blues = clearance.
		// Slope correction: project the ray gap onto the terrain normal, so
		// steep faces stop misreading horizontal offset as burial depth.
		float3 gapVec = input.WorldPos * (1.0 - sceneZ / max(shellZ, 1e-3));
		float deltaUp = dot(gapVec, normalize(input.TerrainNormalAlpha.xyz));
		float camDist = length(gridLocal - WarpedHalfSpan);
		uint band = camDist < 4000.0 ? 0u : (camDist < 8000.0 ? 1u : (camDist < 16000.0 ? 2u : 3u));
		uint bucket = deltaUp < -32.0 ? 0u : deltaUp < -8.0 ? 1u :
		                                 deltaUp < -2.0     ? 2u :
		                                 deltaUp < 2.0      ? 3u :
		                                 deltaUp < 8.0      ? 4u :
		                                 deltaUp < 32.0     ? 5u :
		                                 deltaUp < 128.0    ? 6u :
		                                                      7u;
#	ifdef SNOW_LOD_HISTOGRAM
		InterlockedAdd(LODHistogram[band * 8u + bucket], 1u);
#	endif
		static const float3 kBucketColors[8] = {
			float3(0.55, 0.0, 0.0), float3(1.0, 0.2, 0.0), float3(1.0, 0.55, 0.0), float3(1.0, 1.0, 0.15),
			float3(0.2, 0.85, 0.2), float3(0.0, 0.7, 0.9), float3(0.15, 0.3, 1.0), float3(0.55, 0.15, 0.85)
		};
		preLit = kBucketColors[bucket];
	}
	else if (ShellLODDebug == 2)
	{
		// Warp-ring view: inner linear region gray; outer rings cycle six
		// colors by ring index, dimmed where the world-snap weight is still
		// partial (the camera-relative morph zone).
		float2 uAxis = float2(InverseWarpAxis(gridLocal.x - WarpedHalfSpan), InverseWarpAxis(gridLocal.y - WarpedHalfSpan));
		float ringF = max(abs(uAxis.x), abs(uAxis.y)) - kWarpInnerVerts;
		[flatten] if (ringF <= 0.0)
			preLit = float3(0.15, 0.15, 0.15);
		else
		{
			float snapWeight = saturate(pow(kWarpGrowth, ringF) - 1.0);
			static const float3 kRingColors[6] = {
				float3(1.0, 0.2, 0.2), float3(1.0, 0.8, 0.2), float3(0.3, 1.0, 0.3),
				float3(0.2, 0.9, 0.9), float3(0.3, 0.4, 1.0), float3(0.9, 0.3, 0.9)
			};
			preLit = kRingColors[(uint)ringF % 6u] * lerp(0.35, 1.0, snapWeight);
		}
	}
	else if (ShellLODDebug == 3)
	{
		// Provenance: green = baked cell data; LOD-classified far texels
		// (w = 2 + score) grade brown (classified bare) -> blue-white
		// (classified snow) so the classification itself is inspectable;
		// cyan = snow-line fallback (no LOD tile); red = no data at all
		// (outside the worldspace; the VS raises these to eye level so the
		// gap reads as a sheet).
		float2 provT = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
		float4 provTexel = TerrainWindow.Load(int3((int2)clamp(provT, 0.0, (float)(TerrainDim - 1)), 0));
		[flatten] if (provTexel.x <= -50000.0)
			preLit = float3(0.9, 0.1, 0.1);
		else if (provTexel.w > 1.5)
			preLit = lerp(float3(0.45, 0.2, 0.08), float3(0.55, 0.75, 1.0), saturate(provTexel.w - 2.0));
		else if (provTexel.w > 0.5)
			preLit = float3(0.1, 0.7, 0.7) * (0.4 + 0.6 * pixelCoverage);
		else preLit = float3(0.05, 0.25 + 0.75 * pixelCoverage, 0.1);
	}

	// Terrain Blending-style output: alpha rides every .w and the stochastic
	// blend mask goes to NormalGlossiness.w, exactly as Lighting.hlsl's
	// deferred tail encodes it for the temporal resolve.
	float alpha = ((ShellDebugData != 0 && ShellDebugData < 4) || ShellLODDebug != 0) ? 1.0 : coverageAlpha;
	float stochasticBlend = (screenNoise * screenNoise) < alpha ? 1.0 : 0.0;

	PS_OUTPUT psout;
	psout.Diffuse = float4(preLit, alpha);
#	ifndef SNOW_LOD_HISTOGRAM
	psout.MotionVectors = float4(motionVector, 0.0, alpha);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(viewNormal), 1.0 - snowRoughness, stochasticBlend);
	// Albedo carries the diffuse lobe (Lighting's PBR tail writes the same),
	// Specular the direct GGX lobe, Reflectance the environment lobe weight.
	psout.Albedo = float4(diffuseLobe, alpha);
	psout.Specular = float4(directSpecular, alpha);
	psout.Reflectance = float4(specularLobe, alpha);
	// Masks.z carries the final ambient luma for the composite. Lighting's
	// masksZ is albedo-multiplied and skylit (directionalAmbientColor *=
	// outputAlbedo, then ApplySkylighting); ambientPart matches that.
	psout.Masks = float4(0.0, 0.0, Color::RGBToYCoCg(ambientPart).x, alpha);
	// Stored as 1 - vertexAO, matching Lighting's convention (the composite
	// divides SSGI's AO by it).
	psout.Masks2 = float4(1.0 - landVertexAO, 0.0, 0.0, alpha);
#	endif

	// Conservative depth clamp: where the shell falls just behind the
	// rendered ground (bilinear dips, LOD decimation in the seam overlap),
	// pull its depth to just in front — the z-fight/pinhole class loses at
	// the source without moving geometry. Legitimate occlusion is preserved:
	// the clamp only fires within a short world-space window behind the
	// surface. Skipped in debug views so the heatmap measures raw deltas.
	// FAR FIELD ONLY: anything standing in the snow (actor legs, props) sits
	// just in front of the shell surface and would otherwise be overdrawn by
	// the clamp — it cannot tell a legitimate occluder from a coincident
	// terrain surface. Z-fighting is a distance problem, so the clamp starts
	// well beyond anything the player stands next to.
	psout.DepthLE = input.Position.z;
	float clampWindow = min(8.0 + shellZ * 0.008, 48.0);
	[branch] if ((ShellDebugData == 0 || ShellDebugData >= 4) && ShellLODDebug == 0 && shellZ > 4000.0 && shellZ > sceneZ && shellZ - sceneZ < clampWindow)
		psout.DepthLE = min(input.Position.z, rawSceneDepth - 1e-5);
	// Near-field micro-clamp (round 16): the edge zone rides within window
	// error of the mesh and z-fights as view-dependent holes. 0.75 units is
	// too thin to overdraw feet or props, wide enough to settle coincident
	// surfaces; edge zone only, so deep interiors keep the raw depth.
	// Round 18: carved floors too - pixelEffDepth is the UNCARVED ramp, so
	// low Trench Floor Height floors were excluded and their wear-through
	// hole rims shimmered with the camera.
	else if ((ShellDebugData == 0 || ShellDebugData >= 4) && ShellLODDebug == 0 && (pixelEffDepth < 4.0 || pixelCarve > 0.5) && shellZ > sceneZ && shellZ - sceneZ < 0.75)
		psout.DepthLE = min(input.Position.z, rawSceneDepth - 1e-5);

	return psout;
}
#endif

#ifdef COMPUTESHADER
// LOD shimmer probes: evaluates the ACTUAL shell mesh surface (warped
// placement, ring snapping, ShellSurfaceZ) at world-anchored points, so the
// CPU can measure frame-to-frame surface stability. Probing the field at the
// probe XY directly would miss the vertex hops entirely — the pops come from
// vertices resampling the field at snapped positions, so the quad corners
// are rebuilt exactly as the VS builds them and interpolated.
// Probes anchor to a 512-unit-quantized camera XY; the CPU mirrors the
// quantization and skips deltas across anchor changes.
RWStructuredBuffer<float> ProbeHeights : register(u0);

static const uint kProbeAzimuths = 24;
static const uint kProbeRadii = 12;
// Must match SnowDeformation.h kLODProbeRadius.
static const float kProbeRadius[12] = { 1500, 2500, 3500, 5000, 6500, 8000, 10000, 12500, 15000, 18000, 21000, 24000 };
static const float kProbeInvalid = 3.0e38;

float2 SnappedVertexXY(float2 u)
{
	float2 centered = float2(WarpAxis(u.x), WarpAxis(u.y));
	return GridOrigin + WarpedHalfSpan + GeomorphVertexXY(centered, u);
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= kProbeAzimuths * kProbeRadii)
		return;
	uint az = id.x % kProbeAzimuths;
	uint ri = id.x / kProbeAzimuths;

	float2 anchor = floor(ShellCameraPosAdjust.xy / 512.0) * 512.0;
	float ang = (float)az * (6.28318530 / kProbeAzimuths);
	float2 probeXY = anchor + float2(cos(ang), sin(ang)) * kProbeRadius[ri];

	// Locate the quad by unsnapped vertex units, then rebuild its corners
	// exactly as the VS does (snapping moves them by up to half a ring step).
	float2 center = GridOrigin + WarpedHalfSpan;
	float2 u = float2(InverseWarpAxis(probeXY.x - center.x), InverseWarpAxis(probeXY.y - center.y));
	float2 q = floor(u);
	if (max(abs(q.x), abs(q.y)) >= (float)GridDim * 0.5 - 1.0) {
		ProbeHeights[id.x] = kProbeInvalid;
		return;
	}

	float2 p00 = SnappedVertexXY(q);
	float2 p10 = SnappedVertexXY(q + float2(1, 0));
	float2 p01 = SnappedVertexXY(q + float2(0, 1));
	float2 p11 = SnappedVertexXY(q + float2(1, 1));

	float cov, th;
	float z00 = ShellSurfaceZ(p00 - GridOrigin, cov, th);
	bool valid = th > -50000.0;
	float z10 = ShellSurfaceZ(p10 - GridOrigin, cov, th);
	valid = valid && th > -50000.0;
	float z01 = ShellSurfaceZ(p01 - GridOrigin, cov, th);
	valid = valid && th > -50000.0;
	float z11 = ShellSurfaceZ(p11 - GridOrigin, cov, th);
	valid = valid && th > -50000.0;
	if (!valid) {
		ProbeHeights[id.x] = kProbeInvalid;
		return;
	}

	// Bilinear over the snapped quad. The rasterizer splits it into two
	// triangles, but bilinear tracks every corner hop the same way; the
	// temporal delta is what the meter measures, not the absolute surface.
	float2 span = max(p11 - p00, 1.0);
	float2 f = saturate((probeXY - p00) / span);
	ProbeHeights[id.x] = lerp(lerp(z00, z10, f.x), lerp(z01, z11, f.x), f.y);
}
#endif
