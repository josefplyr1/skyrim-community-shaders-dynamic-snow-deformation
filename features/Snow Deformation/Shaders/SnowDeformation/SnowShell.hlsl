// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// Snow shell renderer: a vertex-buffer-less camera-following grid built from
// SV_VertexID, conformed to the baked terrain window, displaced by per-class
// snow depth and carved by the deformation map. Per-pixel normals come from
// the same fields, so trench walls shade smoothly on coarse geometry.
// Camera matrices ride the private ShellCB (b0), mirrored from b12.

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
// Shadows: terrain/cloud via GetWorldShadow, actors via the raw cascade atlas
// copies (SnowShadow.hlsli), falling back to the shared VSM. Not the
// screen-space mask - it holds the terrain behind the shell along the view
// ray, so its shadows slide with the camera.
#	define TERRAIN_SHADOWS
#	define CLOUD_SHADOWS
#	define VOLUMETRIC_SHADOWS
SamplerState ShellLinearSampler : register(s1);
#	define LinearSampler ShellLinearSampler
#	include "Common/ShadowSampling.hlsli"
#	include "ScreenSpaceShadows/ScreenSpaceShadows.hlsli"
#	include "Skylighting/Skylighting.hlsli"
#	include "SnowDeformation/SnowShadow.hlsli"
// POM runs EM's GetParallaxCoords through EM_PARALLAX_CUSTOM_HEIGHT, fetching
// via the anti-tiling taps (SnowParallax.hlsli). Soft-shadow taps stay local:
// the statics shell blends two planar projections' raw occlusions, which EM's
// multiplier API cannot express. LANDSCAPE/TRUE_PBR stay undefined here.
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

	float ShellTriHeight;        // was BorderTrampledFade: mesh-matched terrain height
	float BorderUntrampledFade;  // contact-term slope / outward-dust reach (Border Fade %, remapped 2..64 on upload)
	float ShellCullBare;         // was SeamFadeUnused: cull fully-bare patches
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
	float SkinTessCapSlope;  // statics skins only
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
	// C3 A/B flags on the retired crisp-grain keeper row; layout unchanged.
	float DebugNoFarPad;
	float DebugNoDataMorph;

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
	// march), w = spare (the march takes Extended Materials' step budget).
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
	// them with the active-light set).
	float4 BorderStyle;
	// x spare (compaction matte retired); y = shell-surface SSS
	// re-march, packed: integer part 0 off / 1 on / 2 on + thickness
	// streak fix, fraction * 1000 = caster height cap in units; zw =
	// dynamic-resolution scale for its screen-space taps (FrameBuffer b12
	// is unbound in this pass).
	float4 CompactLook;
	// Stage 3 P5: x = rim lip height (fraction of local depth), y = rim
	// teeth strength; zw spare.
	float4 RimStyle;

	// Baked undulation window: xy = world centre, z = 1/half-extent,
	// w > 0.5 when the bake is live (UndulationFieldCS, t29).
	float4 UndulationFieldWindow;

	// Toroidal deformation-map addressing: physical position of logical
	// texel (0,0). Every DeformationMap Load routes through DeformTexel.
	int2 DeformMapOrigin;
	// x bit 0 horizon march, bit 1 frustum cull, bit 2 land-exact height OFF,
	// bit 3 flip tess diagonal sense, bit 4 far-band ground max LIFT ON
	// (rejected default), bit 5 far relief tessellation OFF. Mirror in
	// SnowDeformation.h.
	int2 ShellFlags;

	// Land-exact height layer: xy = GridOrigin - fine window origin (world),
	// z = fine dim in texels (0 = none), w = fine texel size (32).
	float4 FineWindow;
	float4 SlopeDrape;
	// Bump octave for the live undulation fallback: x = height (units),
	// y = cell (units), z = coverage threshold, w spare. Mirror in
	// SnowDeformation.h AND SnowStaticsShell.hlsl.
	float4 UndulationBumps;
}

// Bow wave: the crest a moving body pushes ahead of and beside its legs.
// Its own buffer, not a ShellCB row - ShellCB is hand-mirrored across two
// shaders and this is landscape-only. Rebuilt every frame from current
// position and velocity, so it emits nothing and needs no per-wave age; the
// berm the module already draws is the settle.
cbuffer BowWaveCB : register(b1)
{
	/// x = live wave count, y = height scale (fraction of local depth),
	/// z = reach scale on the push radius, w = forward bias 0-1
	float4 BowWaveParams;
	/// x = chunkiness 0-1 (how far the crest breaks into lumps), yzw spare
	float4 BowWaveLook;
	// The per-wave position/shape arrays are gone: DeformationUpdateCS MAXes
	// the crest into the map's deposit channel and the shell reads that field,
	// so the buffer carries only the look knobs.
}

Texture2D<float4> TerrainWindow : register(t0);
// The ground as the engine renders it: bicubic Catmull-Rom of the LAND
// heightmap at 32-unit texels, cell edges extrapolated (TerrainFineCS).
// The land mesh is then flat between these points with a checkerboard
// diagonal, which SampleTerrain reproduces.
Texture2D<float> TerrainFine : register(t13);
// 2x2 and 4x4 maxima of TerrainFine (64- and 128-unit texels): the far bands'
// conservative ground, see SampleTerrain.
Texture2D<float4> DeformationMap : register(t1);

// Toroidal map fetch: logical texel (already clamped by the caller) to
// physical. The map's dim is a power of two, so the wrap is a mask. Hardware
// samplers cannot do this - bilinear across the physical seam would mix two
// unrelated world locations - so every read stays Load-based, per tap.
int3 DeformTexel(int2 t, int2 dims)
{
	return int3((t + DeformMapOrigin) & (dims - 1), 0);
}

Texture2D<float4> SnowDiffuse : register(t2);
// Full-scene depth copy (Terrain Blending's blended depth when available),
// never the bound DSV, so sampling during the shell draw is legal.
Texture2D<float> SceneDepth : register(t3);
// Per-vertex surface bake (BakeCS → SNOW_DS_BAKE domain shader): one texel per
// base-grid vertex, (z, coverage, terrainHeight, deformation tap), full
// float, and a second map holding the vertex normal's two height differences.
// The hull (SNOW_HS_BAKE) reads .w for its edge corners.
Texture2D<float4> ShellVertexBake : register(t9);
Texture2D<float2> ShellVertexBakeSlope : register(t23);
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
// x = layer depth (all this shell reads), y = the patch's road-heightfield bit.
Texture2D<float2> ObjectSkinDepthMap : register(t12);
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

// Baked undulation field (UndulationFieldCS): x = height in world units,
// yz = its +-kUndulationGradStep shading gradient, over UndulationFieldWindow.
Texture2D<float4> UndulationFieldMap : register(t29);

SamplerState SnowSampler : register(s0);

// Shared trench-detail shaping, spell-mark readers, field surfaces and the
// frost pattern - the verbatim-identical pieces of both shells live in one
// file.
#include "SnowDeformation/SnowFields.hlsli"


// The game's own landscape tiling: 24 texture repeats per 4096-unit cell,
// measured in-game with the tiling ruler. Same texture at the same world
// rate as the ground beside us; see CODE-NOTES.md. Mirrored in
// SnowStaticsShell.hlsl, SnowDeformation.hlsli and Shell.cpp.
static const float kSnowUVTile = 4096.0 / 24.0;

// Warped band grid (WarpBand/WarpAxis/InverseWarpAxis/GeomorphVertexXY):
// shared with the trench patch, which stands on the same lattice.
#include "SnowDeformation/SnowGrid.hlsli"

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

// Past the seam ShellSurfaceZ parks the surface 32 units under the LOD terrain
// and the fade has already reached zero, so no pixel here can survive; just
// inside it the surface is still at -8 and climbs only as the fade does. Both
// are submerged, so dropping geometry that touches this band cannot remove a
// visible pixel - it removes geometry the depth test was going to reject after
// the pixel shader had already run.
//
// NOT the culling that was rejected on measurement: that one culled inside a
// drawn shell, where the cost is per-pixel tap count. This drops patches that
// produce no pixels at all.
//
// Centre as well as corners: the fade's zero region is the OUTSIDE of a
// rectangle and so is not convex, and a patch can clip a rectangle corner with
// all four of its own corners outside it.
bool ShellBeyondSeam(float2 a, float2 b, float2 c, float2 d)
{
	float2 mid = 0.25 * (a + b + c + d);
	return ShellEdgeFade(a) <= 0.0 && ShellEdgeFade(b) <= 0.0 &&
	       ShellEdgeFade(c) <= 0.0 && ShellEdgeFade(d) <= 0.0 &&
	       ShellEdgeFade(mid) <= 0.0;
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
// Height at a fractional position inside a terrain quad, matching the LANDSCAPE
// MESH rather than averaging it.
//
// Bilinear is exactly the mean of a quad's two possible triangulations, so it
// sits BELOW whichever one the mesh actually uses by the saddle term
// (h00 + h11 - h10 - h01) / 4 at the centre. Flat ground: zero. A steep saddle
// at 128-unit spacing with a few hundred units of corner variation: tens of
// units - deeper than the snow layer - and the shell sinks through the mesh and
// shows bare rock.
//
// The clearance pad used to hide this; its own comment names the case
// ("filling pinholes where the bilinear surface dips under triangle diagonals").
// Round 4 of the distant-shell work made the coarsest band 128 units, which
// drives padWeight to zero in EVERY band, so the pad is off and this half of its
// job went with it. Round 4 retired it for UNDERSAMPLING, which a 1:1 lattice
// genuinely does fix; the triangulation mismatch is a separate defect that a 1:1
// lattice does not touch.
//
// Taking the max of both triangulations is >= whichever the mesh uses, so the
// shell can no longer sink beneath it. Costs only ALU on corners already
// fetched, and collapses to bilinear wherever the quad is planar - flat ground
// does not move at all.
float TriangulatedHeight(float h00, float h10, float h01, float h11, float2 f)
{
	// Diagonal h00-h11, split along f.y == f.x.
	float triA = f.y <= f.x ?
	                 h00 + (h10 - h00) * f.x + (h11 - h10) * f.y :
	                 h00 + (h01 - h00) * f.y + (h11 - h01) * f.x;
	// Diagonal h10-h01, split along f.x + f.y == 1.
	float triB = (f.x + f.y) <= 1.0 ?
	                 h00 + (h10 - h00) * f.x + (h01 - h00) * f.y :
	                 h11 + (h10 - h11) * (1.0 - f.y) + (h01 - h11) * (1.0 - f.x);
	return max(triA, triB);
}

// The ground as the engine renders it, at any grid-local point: flat
// triangles between the fine layer's 32-unit vertices, split '/' where the
// world 32-quad index sum is even (the fine origin is a cell corner, so the
// texel index sum carries the same parity). The missing sentinel outside the
// fine window or over missing data.
// Single exit on purpose: a return inside a [branch] is the X4000 shape, and
// under the vertex stages' inlining depth it made fxc overflow its stack.
float LandHeightFine(float2 gridLocal)
{
	float h = -100000.0;
	float2 tf = (FineWindow.xy + gridLocal) / FineWindow.w;
	bool inside = FineWindow.z > 0.5 && all(tf >= 0.0) && all(tf < FineWindow.z - 1.0);
	[branch] if (inside)
	{
		int2 f0 = (int2)tf;
		float2 ff = tf - f0;
		float g00 = TerrainFine.Load(int3(f0, 0));
		float g10 = TerrainFine.Load(int3(f0 + int2(1, 0), 0));
		float g01 = TerrainFine.Load(int3(f0 + int2(0, 1), 0));
		float g11 = TerrainFine.Load(int3(f0 + int2(1, 1), 0));
		float lo = min(min(g00, g10), min(g01, g11));
		bool slash = ((f0.x + f0.y) & 1) == 0;
		float hA = ff.y <= ff.x ? g00 + (g10 - g00) * ff.x + (g11 - g10) * ff.y : g00 + (g01 - g00) * ff.y + (g11 - g01) * ff.x;
		float hB = (ff.x + ff.y) <= 1.0 ? g00 + (g10 - g00) * ff.x + (g01 - g00) * ff.y : g11 + (g10 - g11) * (1.0 - ff.y) + (g01 - g11) * (1.0 - ff.x);
		h = lo > -50000.0 ? (slash ? hA : hB) : -100000.0;
	}
	return h;
}

// Band step (8..128 world units) of the shell quad a grid-local point lies in.
float ShellBandStep(float2 gridLocal)
{
	float2 u = float2(InverseWarpAxis(gridLocal.x - WarpedHalfSpan), InverseWarpAxis(gridLocal.y - WarpedHalfSpan));
	return GridSpacing * max(WarpBand(abs(u.x)).x, WarpBand(abs(u.y)).x);
}

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

	float3 result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Height follows the mesh's triangulation; depth and coverage stay bilinear,
	// being material blends rather than geometry. Skipped where any corner is the
	// missing-data sentinel: bilinear poisons the result so callers can detect it
	// with `< -50000`, and a max would pick the surviving triangle and hide the
	// gap instead.
	[flatten] if (ShellTriHeight > 0.5 && min(min(s00.x, s10.x), min(s01.x, s11.x)) > -50000.0)
		result.x = TriangulatedHeight(s00.x, s10.x, s01.x, s11.x, f);

	// Land-exact height: the ground as the engine renders it (LandHeightFine).
	// Outside the fine window, or over missing data, the 128-texel height
	// above stands, so nothing changes at the seam.
	[branch] if (FineWindow.z > 0.5 && (ShellFlags.x & 4) == 0)
	{
		float fine = LandHeightFine(gridLocal);
		[flatten] if (fine > -50000.0)
			result.x = fine;

	}
	// the accumulated layer scales depth HERE, at the one point
	// every reader funnels through, so geometry, shading, the berm gate and the
	// self-shadow march cannot disagree about how deep the snow is. Positive
	// only - a negative rampDepth is the submerge toward bare ground at a class
	// border, not snow, and scaling it would move class edges as it snowed.
	result.y = max(result.y, 0.0) * RimStyle.w + min(result.y, 0.0);
	return result;
}

// Height as the terrain window's plain bilinear reads it: the object field's
// own base (HeightMapProcessCS SampleTerrainHeight), so a lift measured
// against it is the field's excess and nothing else.
float SampleTerrainBilinearHeight(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	float s00 = TerrainWindow.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = TerrainWindow.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = TerrainWindow.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = TerrainWindow.Load(int3(t1.x, t1.y, 0)).x;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Bilinear helper at fractional texel coordinates (Load-based).
float SampleDeformationBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);

	float s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).x;
	float s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).x;
	float s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).x;
	float s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).x;

	// Clamped here rather than at each call site: melt writes past 1.0 into
	// the refill headroom, and this is the single tap every consumer goes
	// through (bicubic, fast, and the berm field's 17 taps). The B-spline
	// weights are a convex combination, so clamping here bounds them all.
	return saturate(lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y));
}

// Deposit (.w) sample: snow standing ABOVE the untouched surface, written by
// the bow wave in DeformationUpdateCS and PERSISTENT - it stays on the ground
// it was shouldered onto instead of following the feet that made it. Plain
// bilinear is enough: the field is smooth by construction (a crest is tens of
// units across) and the chunk detail is added analytically at pixel rate
// below, not stored here.
float SampleDeposit(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;
	float2 dims;
	DeformationMap.GetDimensions(dims.x, dims.y);
	float2 t = clamp(uv * dims - 0.5, 0.0, dims - 1.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	float s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims))).w;
	float s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims))).w;
	float s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims))).w;
	float s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims))).w;
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

	float4 s00 = DeformationMap.Load(DeformTexel(int2(t0.x, t0.y), int2(dims)));
	float4 s10 = DeformationMap.Load(DeformTexel(int2(t1.x, t0.y), int2(dims)));
	float4 s01 = DeformationMap.Load(DeformTexel(int2(t0.x, t1.y), int2(dims)));
	float4 s11 = DeformationMap.Load(DeformTexel(int2(t1.x, t1.y), int2(dims)));
	float4 v = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Only MELTED depth is spoil-free. Channel y is signed and negative means
	// scorch, which was displaced and keeps its berm.
	return saturate(v.x - max(v.y, 0.0));
}

// SampleMelted / SampleScorch / SampleCrust live in SnowFields.hlsli
//: both shells read the surface-state channels identically.

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

// March-grade deformation tap: one bilinear (4 loads) where the visible
// surface pays bicubic (16). The horizon march samples 28-1000 units out,
// where the samplers' difference is invisible - the same too-fine-to-matter
// rule that keeps churn and clods out of the marches. The debug A/B
// ("Shell: Bicubic March") restores the surface sampler to measure the trade.
float SampleDeformationMarch(float2 gridLocal)
{
#ifdef SNOW_MARCH_BICUBIC
	return SampleDeformation(gridLocal);
#else
	return SampleDeformationFast(gridLocal);
#endif
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

// Fully-bare test. ShellSurfaceZ's depth is rampDepth + (-8) * bare, so -8 is
// the floor: it is what ground reaches when EVERY layer under it is a non-snow
// class, and no blend can go below it. Anything above -8 is still on the ramp
// that climbs to the +14..+30 of a snow class, and culling there would cut the
// transition and leave a gap where the layer rises out of the ground - so the
// test is against the floor and not a threshold part-way up.
static const float kBareDepthFloor = -7.9;

// The one thing in ShellSurfaceZ that can raise depth back off the floor:
//   float liftForce = smoothstep(6.0, 20.0, field - terrainHeight);
//   rampDepth = max(rampDepth, liftForce * 6.0);
// so a patch is only safe to cull while the object field stands less than 6
// units proud. Tested with margin.
//
// NOT a test for "the field has data": t4 is terrain run through the cone
// transform, so it holds a real height across the whole object window and a
// data test vetoes every near patch - which is where the pixels are.
static const float kBareLiftMargin = 2.0;

// Conservative over the whole patch, which point sampling is NOT: the shell's
// surface is the BILINEAR blend of the terrain texels, so a patch whose corners
// all land in one bare texel can still rise to a snowy neighbour across its
// span. Truncating to the nearest texel misses exactly those, and culling them
// punched holes in open snow.
//
// So take the MAX effective depth over every texel the patch's bilinear can
// reach - floor(min) through floor(max)+1 - and cull only if even that is on
// the floor. Cheap where it matters: patches in the fine bands sit inside one
// texel and cost a single load, and the coarsest band is 128 units against a
// 128-unit texel, so the range is 2x2. The clamp is a loop bound, not a limit
// any real patch reaches.
bool ShellTerrainAllBare(float2 lo, float2 hi)
{
	float2 tLo = (GridToTerrainOffset + lo) / TerrainTexelSize;
	float2 tHi = (GridToTerrainOffset + hi) / TerrainTexelSize;
	float maxTexel = (float)(TerrainDim - 1);
	int2 i0 = (int2)clamp(floor(tLo), 0.0, maxTexel);
	int2 i1 = (int2)clamp(floor(tHi) + 1.0, 0.0, maxTexel);
	i1 = min(i1, i0 + 3);

	float maxDepth = -1e9;
	[loop] for (int y = i0.y; y <= i1.y; ++y)
	{
		[loop] for (int x = i0.x; x <= i1.x; ++x)
		{
			float3 t = TerrainWindow.Load(int3(x, y, 0)).xyz;
			// Sentinel texels carry no data; leave them to the full evaluation.
			[branch] if (t.x < -50000.0)
				return false;
			maxDepth = max(maxDepth, t.y + (-8.0) * saturate(1.0 - saturate(t.z)));
		}
	}
	return maxDepth <= kBareDepthFloor;
}

// Object-lift veto, sampled at corners and centre. The cone field is slope
// limited and changes slowly, and the margin sits well under the 6 units where
// the lift actually starts, so a few taps carry it.
bool ShellObjectLiftsAt(float2 gridLocal)
{
	float field = SampleObjectHeight(GridOrigin + gridLocal);
	[branch] if (field < -50000.0)
		return false;
	return field - SampleTerrainBilinearHeight(gridLocal) > kBareLiftMargin;
}

bool ShellFullyBare(float2 a, float2 b, float2 c, float2 d)
{
	float2 lo = min(min(a, b), min(c, d));
	float2 hi = max(max(a, b), max(c, d));
	[branch] if (!ShellTerrainAllBare(lo, hi))
		return false;

	[branch] if (ObjectLiftCap > 0.0)
	{
		float2 mid = 0.25 * (a + b + c + d);
		if (ShellObjectLiftsAt(a) || ShellObjectLiftsAt(b) || ShellObjectLiftsAt(c) ||
			ShellObjectLiftsAt(d) || ShellObjectLiftsAt(mid))
			return false;
	}
	return true;
}


// SampleExclusionField lives in SnowFields.hlsli.

float2 SampleObjectBottom(float2 worldXY)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	// t5: x = coverage suppression, y = melt fraction. Independent channels, so
	// a door's influence tail cannot discard the melt around it. Outside the
	// window nothing is suppressed or melted - a raw-height sentinel here
	// zeroes VS coverage on every out-of-window vertex instead.
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

// Mirrors of SnowDeformation.h kNoRoadTop / SnowStaticsShell.hlsl kRoadOwnsTop.
static const float kNoRoadTop = -1000000.0;
static const float kRoadOwnsTop = 8.0;

// The object-layer depth cap: the skin depth (max of 4 texels; the raster is
// sentinel-free, 0 where nothing wrote) where a captured object covers the
// texel, or a huge no-cap value where none does or the window does not reach.
//
// Road-owned columns cap to ZERO: the trench patch owns road snow outright
// (ROAD-HEIGHTFIELD-PLAN), and a shell layer drawn there at its own class
// depth is a second surface through one snow layer - Josef's depth sweep
// caught it slicing across the patch's trench walls as a bright ledge
// wherever the two depths disagree. Cap 0 takes the shell's existing
// step-aside path (coverage dies with the depth), which the patch then fills
// by construction. ALL FOUR texels must be road-owned - one-texel erosion, so
// the verge keeps today's overlap and a jitter-shaped gap cannot open at the
// road edge. The G channel is written only while the road heightfield is on,
// so this path is inert with the feature off.
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
	float2 sd00 = ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0));
	float2 sd10 = ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0));
	float2 sd01 = ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0));
	float2 sd11 = ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0));
	float4 roadTops = float4(sd00.y, sd10.y, sd01.y, sd11.y);
	[branch] if (all(roadTops > kNoRoadTop * 0.5) && all(tops - roadTops < kRoadOwnsTop))
		return 0.0;
	return max(max(sd00.x, sd10.x), max(sd01.x, sd11.x));
}

// ---- Surface undulation: wind-settled dunes ----
// Two octaves of world-anchored value noise, added as real geometry via
// ShellSurfaceZ and shaded through its gradient. Amplitude scales with local
// depth, so thin snow, class boundaries and carved floors stay flat.
// Undulation, CarveProfile and kFireMeltFloor live in SnowFields.hlsli, so the
// surface, the shading gradient and both shells' self-shadow marches see one
// shape. Trench floor minimum is live in BorderStyle.y; low values deliberately
// let trampling wear through to the ground.

// Edge berm: displaced snow piles along the trench rim, taller from a deeper
// layer. Shape comes from the BLURRED deformation (BermField) - two rings
// reaching ~40 units past the trail edge, the outer ring's small per-tap
// weight giving a long tail instead of a knife along the stamp falloff.
// BermShape and the tap ring are in SnowFields.hlsli.

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
// ~10-unit lumps of short-wavelength noise on carved or piled snow, weighted
// by the same carve/berm terms the profile uses, so churn dies at the
// untouched surface with no boundary. The self-shadow march skips it - a few
// units is under its step resolution.
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
		// Coarse wander + fine raggedness: the coarse octave is
		// the ORIGINAL 37-unit model but capped at the strength its old 16
		// setting had - enough organic wander, never the lobes/islands the
		// uncapped version folded into; the fine 8-unit octave scales on
		// with the slider for the ragged detail.
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

// Bow wave height at a world point, as a fraction of local snow depth.
// The shape is not computed here: DeformationUpdateCS MAXes the crest into the
// map's deposit channel, so this reads a persistent field and deposited snow
// stays on the ground it was pushed onto. Only the chunk detail is analytic,
// since the map is ~6.8 units per texel and the lumps want to be finer.
float BowWaveHeight(float2 worldXY, float2 gridLocal, float deformation, float uncarvedDepth)
{
	[branch] if (BowWaveParams.y < 0.001)
		return 0.0;

	float crest = SampleDeposit(gridLocal);
	[branch] if (crest < 0.002)
		return 0.0;

	// CHUNKS: many small mountains standing up out of the pushed snow, over
	// the WHOLE deposit rather than a thin leading band - the round-3 band was
	// so narrow that the lumps read as occasional hills instead of a field of
	// broken snow. Additive ridged noise at two fine octaves, powered
	// into isolated peaks, WORLD-anchored so the lumps belong to the ground
	// like the deposit itself does.
	[branch] if (BowWaveLook.x > 0.001)
	{
		const float n1 = ChurnNoiseScaled(worldXY, kClodSizeScale * 0.26);
		const float n2 = ChurnNoiseScaled(worldXY + 71.3, kClodSizeScale * 0.11);
		const float peaks = pow(saturate(n1 * 0.55 + n2 * 0.45 + 0.5), 1.9);
		// Rides the deposit's shoulder, so chunks never float on flat ground.
		// The floor sits low because the wipe owns trail cleanliness; a
		// 0.30-0.70 floor eats most of the lumps, since typical pile values
		// are 0.4-0.7.
		crest += peaks * smoothstep(0.12, 0.45, crest) * BowWaveLook.x * 1.4;
	}

	// Un-dug snow only, and scaled by what is locally there to push.
	return crest * BowWaveParams.y * uncarvedDepth *
	       saturate(1.0 - deformation) * BermDepthGate(uncarvedDepth);
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
		// Effective lattice step here, blended across the owning band toward the
		// next. Shared with the clearance pad, so both describe one surface.
		float padWeight;
		{
			float2 centeredM = gridLocal - WarpedHalfSpan;
			float2 uAxisM = float2(InverseWarpAxis(centeredM.x), InverseWarpAxis(centeredM.y));
			float3 bandX = WarpBand(abs(uAxisM.x));
			float3 bandY = WarpBand(abs(uAxisM.y));
			float2 ringStepM = GridSpacing * float2(bandX.x, bandY.x);
			// Morph weight is now the fraction through the OWNING BAND, not
			// frac(lod): with power-of-two steps every lod is a whole number,
			// so the old expression is identically zero and the morph would
			// never fire. Band position is continuous in radius, which is what
			// makes the hand-off seamless when the origin re-snaps and a patch
			// of ground changes bands.
			float morphT = max(bandX.y, bandY.y);
			// Stands down while the land-exact layer is live: every lattice
			// vertex is then on the land in both bands, so a band change has
			// no vertex pop to hide, and the blend toward the coarse bilinear
			// only pulls the far relief tessellation's vertices off the land.
			bool fineLive = FineWindow.z > 0.5 && (ShellFlags.x & 4) == 0;
			[branch] if (!fineLive && max(ringStepM.x, ringStepM.y) > GridSpacing && morphT > 0.001 && DebugNoDataMorph < 0.5)
			{
				float2 coarseStepM = ringStepM * 2.0;
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

			// Clearance-pad weight: how far this vertex's lattice step has
			// outgrown a terrain texel. Zero while the lattice is at or finer
			// than the data, which the current table always is - kept for any
			// future table that skips texels.
			//
			// The RAW step, never band-blended: a ramp quantises against the
			// 256-unit origin snap and reads as a stepped sink as the camera
			// approaches. A weight constant within a band cannot.
			padWeight = saturate((max(ringStepM.x, ringStepM.y) - TerrainTexelSize) / TerrainTexelSize);
		}

		terrainHeight = terrain.x;
		float rampDepth = terrain.y;
		coverage = saturate(terrain.z);

		// Clearance pad: 4-tap axis max of height+coverage with a capped ridge
		// pad, filling pinholes where the bilinear surface dips under triangle
		// diagonals or a texel reads bare. Load-bearing; disabling it exposes
		// holes immediately.
		//
		// LAW: keyed on the vertex's lattice step, never on camera distance.
		// Undersampling is a property of the lattice, and a distance ramp
		// quantises against the origin snap into visible steps.
		[branch] if (padWeight > 0.001 && DebugNoFarPad < 0.5)
		{
			float farBlend = padWeight;
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

		// The mask carries two independent channels. x = coverage suppression,
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
				// cones rise only from ground showing shell snow (class
				// coverage minus clearings). Gating on the exclusion mask alone
				// misses bare landscape classes and raises a rim where the
				// shell meets dirt.
				float groundSnow = smoothstep(0.1, 0.45, coverage) * (1.0 - saturate(shelterMask.x));
				// The lift is the field's excess over its OWN base, the window's
				// bilinear height - not over the height the shell stands on.
				// With the land-exact layer those differ by the whole chord
				// error, and max(exact, bilinear) is an envelope creased along
				// the 128-unit lattice (stripes, lifted hollows, forced snow).
				float lift = max(field - SampleTerrainBilinearHeight(gridLocal), 0.0) * groundSnow;
				// Where a captured object defines the surface, the layer wears
				// the object's own skin depth instead of the landscape class
				// depth (a thin-skinned rock must not carry a deep landscape
				// layer). Blend by how far the object stands proud of the
				// un-lifted base, so buried objects and the aprons around them
				// keep landscape depth.
				float capT = smoothstep(0.25, 1.0, lift / max(rampDepth, 1.0));
				rampDepth = lerp(rampDepth, min(rampDepth, SampleObjectDepthCap(worldXY)), capT);
				terrainHeight += lift;
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

		// Carves only where the layer is raised; the negative-depth submerge at
		// class edges is untouched. The floor holds at Trench Floor Height (or
		// the uncarved depth when thinner) and tapers toward class borders,
		// where a full floor would leave a hard-edged slab over bare ground.
		// Undulation rides on top, scaled by remaining depth.
		[flatten] if (depth > 0.0)
		{
			float deformation = saturate(SampleDeformation(gridLocal));
			float bermD = BermField(gridLocal);
			float uncarved = depth;
			// The berm is spoil piled on snow that was NOT dug. Unmasked, the
			// blurred field also lifted the trench floor and walls - a narrow
			// trail's disc average is well above zero at its own centre, so
			// raising Berm Height raised the whole trench with it.
			depth = CarveProfile(deformation, uncarved, GridOrigin + gridLocal) +
			        BermShape(bermD) * saturate(1.0 - deformation) * uncarved * BermHeightAmp * BermDepthGate(uncarved);
			depth += BowWaveHeight(GridOrigin + gridLocal, gridLocal, deformation, uncarved);
			depth += UndulationSampled(GridOrigin + gridLocal) * saturate(depth / 8.0);
			// Churn scales away on thin cover: the /10 keeps the dig under 80% of
			// local depth even at the slider's 8-unit maximum.
			depth += ChurnNoise(GridOrigin + gridLocal) * ChurnHeightAmp * ChurnWeight(deformation, bermD) * saturate(depth / 10.0);
			// Berm clods: a coarser octave weighted by BermShape rather than
			// the churn weight, since spoil lands on the crest and churn peaks
			// in the trench. Masked like the berm and gated on material; the
			// self-shadow march skips it, as it skips churn.
			[branch] if (RimStyle.z > 0.01)
				depth += ChurnNoiseScaled(GridOrigin + gridLocal, kClodSizeScale) * RimStyle.z *
				         BermShape(bermD) * saturate(1.0 - deformation) * BermDepthGate(uncarved);
		}

		surfaceZ = terrainHeight + depth;
	}
	return surfaceZ;
}

// Shared vertex tail for the legacy VS and the tessellated domain shader:
// smooth per-vertex terrain normal, coverage alpha, debug plane, camera-
// relative transform and output packing.
// The vertex normal's two height differences: wide 32-unit differences bridge
// the 128-unit data texels, and interpolation removes the faceting of the
// per-pixel piecewise-constant gradient. Four terrain taps, so the bake
// stores the pair rather than making every vertex re-take them.
float2 ShellVertexSlope(float2 gridLocal)
{
	float hxp = SampleTerrain(gridLocal + float2(32.0, 0.0)).x;
	float hxn = SampleTerrain(gridLocal - float2(32.0, 0.0)).x;
	float hyp = SampleTerrain(gridLocal + float2(0.0, 32.0)).x;
	float hyn = SampleTerrain(gridLocal - float2(0.0, 32.0)).x;
	return float2(hxp - hxn, hyp - hyn);
}

// The two height differences arrive as separate scalars, exactly as the four
// taps used to leave them: as a float2 the compiler vectorises the normalize
// below, which is the same arithmetic but not the same bytecode, and the
// untouched paths have to stay provably identical.
VS_OUTPUT FinishShellVertexSloped(float2 gridLocal, float z, float coverage, float terrainHeight, float slopeX, float slopeY)
{
	float3 terrainNormal = normalize(float3(-slopeX / 64.0, -slopeY / 64.0, 1.0));

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
	// Slope drape (Josef's sketch, 2026-09-05): on steep ground the layer
	// stands off along the surface normal instead of straight up, so a
	// near-vertical face keeps the full depth of cover in front of it. A
	// vertical-only offset thins to depth*cos(slope) and the face shows
	// through. The foot (gridLocal) stays on the lattice; only the output
	// position moves, so every sample still reads the vertex's own ground.
	[branch] if (SlopeDrape.x > 0.001 && z > terrainHeight)
	{
		float above = z - terrainHeight;
		float tilt = SlopeDrape.x * (1.0 - smoothstep(SlopeDrape.y, SlopeDrape.z, terrainNormal.z));
		float3 dir = normalize(lerp(float3(0.0, 0.0, 1.0), terrainNormal, tilt));
		absolutePos = float3(GridOrigin + gridLocal, terrainHeight) + dir * above;
	}

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

VS_OUTPUT FinishShellVertex(float2 gridLocal, float z, float coverage, float terrainHeight)
{
	float hxp = SampleTerrain(gridLocal + float2(32.0, 0.0)).x;
	float hxn = SampleTerrain(gridLocal - float2(32.0, 0.0)).x;
	float hyp = SampleTerrain(gridLocal + float2(0.0, 32.0)).x;
	float hyn = SampleTerrain(gridLocal - float2(0.0, 32.0)).x;
	return FinishShellVertexSloped(gridLocal, z, coverage, terrainHeight, hxp - hxn, hyp - hyn);
}

#if defined(VSHADER) && !defined(SNOW_TESS)
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
#ifdef SNOW_GRID_NONINDEXED
	// A/B measurement path: the pre-index-buffer expansion, six vertices per
	// quad from SV_VertexID.
	static const float2 kCorners[6] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const float2 kCornersFlipped[6] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };

	uint quadIndex = vertexID / 6;
	uint2 quadXY = uint2(quadIndex % GridDim, quadIndex / GridDim);
	int2 parityBase = int2(floor(GridOrigin / GridSpacing));
	uint parity = uint(parityBase.x + int(quadXY.x)) ^ uint(parityBase.y + int(quadXY.y));
	float2 corner = ((parity & 1) != 0) ? kCornersFlipped[vertexID % 6] : kCorners[vertexID % 6];
	float2 gridPos = float2(quadXY) + corner;
#else
	// Indexed draw over the (GridDim+1)^2 lattice; vertexID is the absolute
	// lattice index (no BaseVertexLocation: with no vertex buffer bound the
	// hardware does not add it to SV_VertexID). The union-jack triangulation
	// and its world-anchored parity live in the index buffers
	// (DrawShellGridIndexed).
	uint stride = GridDim + 1;
	float2 gridPos = float2(vertexID % stride, vertexID / stride);
#endif
	// Warped placement: gridLocal stays a world-unit offset from GridOrigin
	// (the warped grid's min corner), so all field sampling is unchanged.
	float2 u = gridPos - (float)GridDim * 0.5;
	float2 gridLocal = float2(WarpAxis(u.x), WarpAxis(u.y)) + WarpedHalfSpan;

	// Outer rings geomorph between world-anchored lattices (see
	// GeomorphVertexXY) — vertices slide instead of hopping ring steps.
	gridLocal = GeomorphVertexXY(gridLocal - WarpedHalfSpan, u) + WarpedHalfSpan;

	// Seam cull, per vertex because this path has no patch scope. A NaN kills
	// every triangle touching the vertex, which is safe here for the reason in
	// ShellBeyondSeam: at zero fade the surface is 32 units under the LOD
	// terrain, and just inside it is still at -8 and only climbs as the fade
	// does, so the triangles this drops are submerged along their whole span.
	//
	// The shadow caster takes it too: geometry that far under the terrain is
	// occluded from the sun by the ground above it, so it contributes nothing
	// to the cascade. Its own base sinks further still (see below). Costs no
	// bindings either - ShellEdgeFade is constant-buffer maths, no texture.
	[branch] if (ShellEdgeFade(gridLocal) <= 0.0)
	{
		VS_OUTPUT culled = (VS_OUTPUT)0;
		culled.Position = asfloat(0x7FC00000).xxxx;
		return culled;
	}

	float coverage;
	float terrainHeight;
	float z = ShellSurfaceZ(gridLocal, coverage, terrainHeight);

#ifdef SNOW_SHADOW_CAST
	// Shadow-caster variant: only the excess height above the ambient snow
	// depth casts. Casting the full shell shadows every receiver inside or
	// beneath the layer (the terrain it visually replaces, wading actor
	// legs, grass), which reads as the whole landscape darkening.
	//
	// The base is sunk far below the terrain rather than flattened: the terrain
	// window is bilinear-approximate, and writing it at ground level out-depths
	// the real terrain mesh where the approximation overshoots. Solid snow
	// coverage is required too, so dithered-away snow never casts.
	//
	// The real surface casts wherever it stands meaningfully above the ground
	// and carries snow; everything else collapses to NaN and is dropped at the
	// rasterizer, so no sun angle can resurrect it. Sinking geometry instead of
	// culling it does not work - underground geometry still clips low-sun rays
	// and prints sideways blotches on bare ground.
	float3 rawTerrainCast = SampleTerrain(gridLocal);
	// the far anti-pinhole
	// pass dilates coverage AND height (4-tap max + ridge pad), so beyond
	// ~3000 units a ring around every patch passes both castVis terms while
	// the main view discards those pixels per-texel — and this depth-only
	// caster has no PS to do the same. Gating coverage on the UNDILATED
	// texel under the vertex keeps true snowfields casting and silences
	// the dilation skirt over bare coast.
	float castVis = smoothstep(2.0, 5.0, z - rawTerrainCast.x) * smoothstep(0.2, 0.5, min(coverage, saturate(rawTerrainCast.z)));
	// Melt pits do not cast: snow the melt removed casts nothing, and the pit
	// dissolves at texture resolution while the caster rim collapses at vertex
	// resolution, printing a blocky shadow on the revealed ground. Stamped
	// (spell) melt only - static clearings are the exclusion field's. Keep the
	// thresholds well above the residue floor, or decaying .y lands on the
	// castVis NaN cliff and caster triangles flip once a second.
	castVis *= 1.0 - smoothstep(0.35, 0.6, SampleMelted(gridLocal));
	// the far field keeps printing blotches from
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

// Base-grid vertex placement from the integer grid coordinate alone: the same
// warped placement + world-anchored ring snapping as the legacy VS, keyed so
// that adjacent patches share edge vertices exactly and so the tessellation
// VS and BakeCS put vertex (x, y) on the same bits.
float2 ShellGridVertexLocal(uint2 gridXY)
{
	float2 u = float2(gridXY) - (float)GridDim * 0.5;
	float2 gridLocal = float2(WarpAxis(u.x), WarpAxis(u.y)) + WarpedHalfSpan;
	return GeomorphVertexXY(gridLocal - WarpedHalfSpan, u) + WarpedHalfSpan;
}

struct TessControlPoint
{
	float2 GridLocal : TEXCOORD0;
	uint2 GridXY : TEXCOORD1;
};

#if defined(VSHADER) && defined(SNOW_TESS)
TessControlPoint main(uint vertexID : SV_VertexID)
{
	static const uint2 kPatchCorners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 4;
	uint2 quadXY = uint2(quadIndex % GridDim, quadIndex / GridDim);

	// The tessellator splits a factor-1 quad along its domain (0,0)-(1,1)
	// diagonal. Rotating the corner order by one puts that diagonal on the
	// other world diagonal, so each patch can take its land quad's split:
	// '/' where the world 32-quad index sum is even. Bit 3 flips the sense
	// (the tessellator's own diagonal is assumed, not documented).
	float2 gl0 = ShellGridVertexLocal(quadXY);
	int2 q = (int2)floor((gl0 + 0.5) / 32.0);
	bool slash = ((q.x + q.y) & 1) == 0;
	bool flip = (ShellFlags.x & 8) != 0;
	uint rot = (slash != flip) ? 0u : 1u;
	TessControlPoint cp;
	cp.GridXY = quadXY + kPatchCorners[(vertexID + rot) % 4];
	cp.GridLocal = ShellGridVertexLocal(cp.GridXY);
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
// SNOW_HS_BAKE: the two corner taps come from BakeCS (ShellVertexBake.w,
// same function, same vertex bits); only the midpoint stays live.
#ifdef SNOW_HS_BAKE
float EdgeTessFactor(float2 gridLocalA, float2 gridLocalB, float deformA, float deformB)
#else
float EdgeTessFactor(float2 gridLocalA, float2 gridLocalB)
#endif
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
#ifdef SNOW_HS_BAKE
		float deform = max(max(deformA, deformB), SampleDeformation(midLocal));
#else
		float deform = max(max(SampleDeformation(gridLocalA), SampleDeformation(gridLocalB)), SampleDeformation(midLocal));
#endif
		reach = kTessNear * lerp(reliefBase, kTessReachBoost, smoothstep(0.02, 0.25, deform));
	}
	return clamp(reach / max(dist, 32.0), 1.0, kTessMax);
}

// Split gate. The PS's far-field depth clamp only fires past kClampFarStart, so
// the pass can be drawn twice: NEAR patches take a PS with no depth export and
// get early-Z rejection back, FAR patches keep the export and the clamp.
//
// Boundary: NEAR drops a patch when ANY corner is far, FAR keeps a patch unless
// ALL corners are near - so a straddling patch lands in exactly one pass, with
// no gap and no double-draw. Its near pixels are unaffected either way, because
// the clamp is gated per pixel on the same distance.
#if defined(SNOW_SPLIT_NEAR) || defined(SNOW_SPLIT_FAR)
static const float kClampFarStart = 4000.0;

bool ShellPatchSplitCulled(float2 a, float2 b, float2 c, float2 d)
{
	float2 cam = ShellCameraPosAdjust.xy;
	float da = length(GridOrigin + a - cam);
	float db = length(GridOrigin + b - cam);
	float dc = length(GridOrigin + c - cam);
	float dd = length(GridOrigin + d - cam);
#	ifdef SNOW_SPLIT_NEAR
	return max(max(da, db), max(dc, dd)) > kClampFarStart;
#	else
	return max(max(da, db), max(dc, dd)) <= kClampFarStart;
#	endif
}
#endif

// View-frustum patch cull. A patch outside the camera frustum produces no
// fragments - the rasteriser discards its triangles before any shading - so
// dropping it here changes nothing on screen while skipping the domain
// shader, the primitive setup and, because it runs first, every tap the
// tests and edge factors below would have taken.
//
// Only the four side planes and the eye plane are tested. They are built
// from w +/- x and w +/- y, so they hold whatever direction the depth range
// runs in; the far field is already bounded by ShellBeyondSeam.
//
// NOT reachable from the shadow caster: that is a separate, non-tessellated
// vertex shader drawn with the light's clip matrix, so snow behind the
// camera goes on casting into the frame.
static const float kFrustumZMargin = 512.0;
static const float kFrustumXYMargin = 16.0;

bool ShellOutsideFrustum(float2 a, float2 b, float2 c, float2 d, float zLo, float zHi)
{
	float2 lo = min(min(a, b), min(c, d)) - kFrustumXYMargin;
	float2 hi = max(max(a, b), max(c, d)) + kFrustumXYMargin;
	// Same space as the vertex chain: absolute world, less the camera adjust.
	float3 bMin = float3(GridOrigin + lo, zLo - kFrustumZMargin) - ShellCameraPosAdjust.xyz;
	float3 bMax = float3(GridOrigin + hi, zHi + kFrustumZMargin) - ShellCameraPosAdjust.xyz;

	// Row-major storage with mul(M, v) makes clip.i = dot(M[i], v), so the
	// side half-spaces are M[3] +/- M[0] and M[3] +/- M[1], and M[3] alone is
	// the eye plane. A box is outside a plane when even its corner furthest
	// along the plane normal still lies behind it.
	float4 planes[5] = {
		CameraViewProj[3] + CameraViewProj[0],
		CameraViewProj[3] - CameraViewProj[0],
		CameraViewProj[3] + CameraViewProj[1],
		CameraViewProj[3] - CameraViewProj[1],
		CameraViewProj[3]
	};
	bool outside = false;
	[unroll] for (uint i = 0; i < 5; i++)
	{
		float3 n = planes[i].xyz;
		float3 pv = float3(n.x > 0.0 ? bMax.x : bMin.x,
			n.y > 0.0 ? bMax.y : bMin.y,
			n.z > 0.0 ? bMax.z : bMin.z);
		if (dot(n, pv) + planes[i].w < 0.0)
			outside = true;
	}
	return outside;
}

// Far relief tessellation. Beyond ~1,920 units the shell's quads are 64 and
// 128 units and span two to four of the land's own 32-unit quads; where the
// ground bulges above the chord the shell would cut below it (the distant
// holes). Rather than lift vertices (a box maximum terraces and buries
// rocks), subdivide: an odd factor puts interior vertices at roughly the
// land's own spacing, and every added vertex samples the exact surface. The
// test compares the land against the chord at the land's own lattice points
// along the edge - the edge lies on lattice lines, and the land is linear
// between its vertices, so three samples see the exact maximum bulge. Edge
// tests read only the edge's endpoints, so both patches on an edge agree
// (crack-free); the inside test may raise the inside factor alone. World
// data only: nothing here moves with the camera.
static const float kFarReliefTolerance = 4.0;

float FarReliefEdgeFactor(float2 a, float2 b)
{
	float factor = 1.0;
	float step = ShellBandStep(0.5 * (a + b));
	bool active = FineWindow.z > 0.5 && (ShellFlags.x & 32) == 0 && step >= 64.0;
	[branch] if (active)
	{
		float ha = LandHeightFine(a);
		float hb = LandHeightFine(b);
		float excess = 0.0;
		[unroll] for (int i = 1; i <= 3; i++)
		{
			float t = i * 0.25;
			float land = LandHeightFine(lerp(a, b, t));
			if (land > -50000.0)
				excess = max(excess, land - lerp(ha, hb, t));
		}
		bool valid = min(ha, hb) > -50000.0;
		factor = (valid && excess > kFarReliefTolerance) ? (step >= 128.0 ? 5.0 : 3.0) : 1.0;
	}
	return factor;
}

// Corner order 0=(0,0) 1=(1,0) 2=(1,1) 3=(0,1); the chord is the LOWER of
// the two triangulations of the corners, the conservative surface.
float FarReliefInsideFactor(float2 c0, float2 c1, float2 c2, float2 c3)
{
	float factor = 1.0;
	float2 centre = 0.25 * (c0 + c1 + c2 + c3);
	float step = ShellBandStep(centre);
	bool active = FineWindow.z > 0.5 && (ShellFlags.x & 32) == 0 && step >= 64.0;
	[branch] if (active)
	{
		float h0 = LandHeightFine(c0);
		float h1 = LandHeightFine(c1);
		float h2 = LandHeightFine(c2);
		float h3 = LandHeightFine(c3);
		float excess = 0.0;
		[unroll] for (int j = 1; j <= 3; j++)
		{
			[unroll] for (int i = 1; i <= 3; i++)
			{
				float2 ff = float2(i, j) * 0.25;
				float2 pos = lerp(lerp(c0, c1, ff.x), lerp(c3, c2, ff.x), ff.y);
				float triA = ff.y <= ff.x ? h0 + (h1 - h0) * ff.x + (h2 - h1) * ff.y : h0 + (h3 - h0) * ff.y + (h2 - h3) * ff.x;
				float triB = (ff.x + ff.y) <= 1.0 ? h0 + (h1 - h0) * ff.x + (h3 - h0) * ff.y : h2 + (h1 - h2) * (1.0 - ff.y) + (h3 - h2) * (1.0 - ff.x);
				float land = LandHeightFine(pos);
				if (land > -50000.0)
					excess = max(excess, land - min(triA, triB));
			}
		}
		bool valid = min(min(h0, h1), min(h2, h3)) > -50000.0;
		factor = (valid && excess > kFarReliefTolerance) ? (step >= 128.0 ? 5.0 : 3.0) : 1.0;
	}
	return factor;
}

TessFactors PatchConstants(InputPatch<TessControlPoint, 4> patch)
{
	TessFactors f;

	// The bake's corner records are hoisted above the culls: their z bounds
	// the patch for the frustum test, and their deformation taps feed the
	// edge factors below. Four loads either way.
#ifdef SNOW_HS_BAKE
	float4 v0 = ShellVertexBake.Load(int3(patch[0].GridXY, 0));
	float4 v1 = ShellVertexBake.Load(int3(patch[1].GridXY, 0));
	float4 v2 = ShellVertexBake.Load(int3(patch[2].GridXY, 0));
	float4 v3 = ShellVertexBake.Load(int3(patch[3].GridXY, 0));
	float zLo = min(min(v0.x, v1.x), min(v2.x, v3.x));
	float zHi = max(max(v0.x, v1.x), max(v2.x, v3.x));
#else
	// No baked corner heights: an unbounded column. The side planes still
	// cull everything left and right of the view; only up and down is given
	// away.
	float zLo = -65536.0;
	float zHi = 65536.0;
#endif

	// Frustum first: pure arithmetic, and it gates every tap below - the
	// seam and bare-ground tests sample the terrain window, and each edge
	// factor can take three bicubic deformation taps. Off in the debug views
	// that move a vertex's z away from the baked surface.
	bool culled = false;
	[branch] if ((ShellFlags.x & 2) != 0 && ShellDebugData == 0 && ShellLODDebug == 0)
		culled = ShellOutsideFrustum(patch[0].GridLocal, patch[1].GridLocal, patch[2].GridLocal, patch[3].GridLocal, zLo, zHi);

	// Tested before the edge factors: a culled patch must not pay for the
	// three bicubic deformation taps per edge that EdgeTessFactor can take.
	[branch] if (!culled)
		culled = ShellBeyondSeam(patch[0].GridLocal, patch[1].GridLocal, patch[2].GridLocal, patch[3].GridLocal);
	[branch] if (!culled && ShellCullBare > 0.5)
		culled = ShellFullyBare(patch[0].GridLocal, patch[1].GridLocal, patch[2].GridLocal, patch[3].GridLocal);
#if defined(SNOW_SPLIT_NEAR) || defined(SNOW_SPLIT_FAR)
	culled = culled || ShellPatchSplitCulled(patch[0].GridLocal, patch[1].GridLocal, patch[2].GridLocal, patch[3].GridLocal);
#endif
	[branch] if (culled)
	{
		f.Edge[0] = 0.0;
		f.Edge[1] = 0.0;
		f.Edge[2] = 0.0;
		f.Edge[3] = 0.0;
		f.Inside[0] = 0.0;
		f.Inside[1] = 0.0;
		return f;
	}

	// Quad edge order: [0] u=0, [1] v=0, [2] u=1, [3] v=1, for the domain
	// bilerp corner layout 0=(0,0) 1=(1,0) 2=(1,1) 3=(0,1).
#ifdef SNOW_HS_BAKE
	float4 edges = float4(
		EdgeTessFactor(patch[0].GridLocal, patch[3].GridLocal, v0.w, v3.w),
		EdgeTessFactor(patch[0].GridLocal, patch[1].GridLocal, v0.w, v1.w),
		EdgeTessFactor(patch[1].GridLocal, patch[2].GridLocal, v1.w, v2.w),
		EdgeTessFactor(patch[3].GridLocal, patch[2].GridLocal, v3.w, v2.w));
#else
	float4 edges = float4(
		EdgeTessFactor(patch[0].GridLocal, patch[3].GridLocal),
		EdgeTessFactor(patch[0].GridLocal, patch[1].GridLocal),
		EdgeTessFactor(patch[1].GridLocal, patch[2].GridLocal),
		EdgeTessFactor(patch[3].GridLocal, patch[2].GridLocal));
#endif
	// Far relief: raise edges where the ground bulges above their chord, and
	// the inside where it bulges inside the patch.
	edges = max(edges, float4(
		FarReliefEdgeFactor(patch[0].GridLocal, patch[3].GridLocal),
		FarReliefEdgeFactor(patch[0].GridLocal, patch[1].GridLocal),
		FarReliefEdgeFactor(patch[1].GridLocal, patch[2].GridLocal),
		FarReliefEdgeFactor(patch[3].GridLocal, patch[2].GridLocal)));
	float inner = max(max(max(edges.x, edges.y), max(edges.z, edges.w)),
		FarReliefInsideFactor(patch[0].GridLocal, patch[1].GridLocal, patch[2].GridLocal, patch[3].GridLocal));

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

#if defined(DOMAINSHADER) || defined(COMPUTESHADER)
// The vertex surface: ShellSurfaceZ plus the skirt descent and the
// displacement relief. One function for the domain shader's live path and
// for BakeCS, so a baked corner and a live vertex are the same bits.
float ShellVertexZ(float2 gridLocal, out float coverage, out float terrainHeight)
{
	float z = ShellSurfaceZ(gridLocal, coverage, terrainHeight);

	float camDist = length(GridOrigin + gridLocal - ShellCameraPosAdjust.xy);
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	// Coarser mips with distance: vertex density falls below texel density
	// out there and full-res sampling shimmers.
	float snowMip = clamp(log2(max(camDist, 64.0) / 128.0), 0.0, 6.0);

	// Grain-driven skirt descent (HEIGHT-BLEND-PLAN): across the coverage edge
	// band the surface descends in grain-shaped tongues rather than a knife-cut
	// hover. Half the PS alpha's sharpness - the alpha cuts the outline, while
	// near-binary geometry raises walls that smear the top-projected texture.
	// Deterministic per position, so shared patch-edge vertices agree.
	// The border's raggedness is HORIZONTAL (Josef's sketch): the grain also
	// advances/retards WHERE along the band the sheet lands, so the landing
	// line billows in plan. The retired vertical edge grain (z +- grain) made
	// the same billows by lifting the rim, and every raised bump hovered with
	// its underside exposed; the fringe now touches ground everywhere. Carved
	// trenches and melt floors keep the vertical grain's old exemptions.
	float descentBlend = SnowHeightBlendSharpness(camDist);
	float wEdge = smoothstep(0.0, 0.6, coverage);
	[branch] if (HasSnowHeight > 0.5 && descentBlend > 1.0 && z > terrainHeight && wEdge < 0.999)
	{
		float hDescent = SampleSnowHeight(ComputeSnowTapsNoGrad(snowUV, GridOrigin + gridLocal), 0.0.xx, snowMip);
		float billowW = (1.0 - smoothstep(0.05, 0.4, saturate(SampleDeformation(gridLocal)))) *
		                (1.0 - saturate(SampleExclusionMask(GridOrigin + gridLocal).y * 2.0));
		const float kEdgeBillowShift = 0.35;
		float wBillow = saturate(wEdge + (hDescent - 0.5) * kEdgeBillowShift * billowW);
		float descent = SnowHeightBlendOneSided(wBillow, hDescent, 1.0 + (descentBlend - 1.0) * 0.5);
		// Josef's border spec: the sheet ends BELOW the ground, not on it. As
		// the descent runs out the sheet keeps going to terrain - 8
		// (ShellSurfaceZ's own submerge floor), so the visible border is the
		// sheet's INTERSECTION with the terrain - the depth test clips it -
		// and a hovering rim with an exposed underside cannot exist. The
		// grain still decides WHERE each tongue dives, so the intersection
		// line keeps the billows.
		z = terrainHeight + (z - terrainHeight) * descent - 8.0 * (1.0 - descent);
	}

	// Real relief from the PBR displacement map, through the SAME anti-tiling
	// taps the PS shades with, so the normal map's shading and the geometry
	// describe one surface. (A single un-offset tap here against the PS's
	// three random per-cell offsets decorrelates the two
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

	return z;
}
#endif  // DOMAINSHADER || COMPUTESHADER

#ifdef DOMAINSHADER
[domain("quad")]
VS_OUTPUT main(TessFactors factors, float2 domainUV : SV_DomainLocation, const OutputPatch<TessControlPoint, 4> patch)
{
	float2 gridLocal = lerp(
		lerp(patch[0].GridLocal, patch[1].GridLocal, domainUV.x),
		lerp(patch[3].GridLocal, patch[2].GridLocal, domainUV.x), domainUV.y);

#ifdef SNOW_DS_FLAT
	// A/B measurement path: terrain plus class depth, no field work, so the
	// Shell row's drop bounds what the geometry stages cost.
	{
		float3 flatTerrain = SampleTerrain(gridLocal);
		[branch] if (flatTerrain.x < -50000.0)
		{
			VS_OUTPUT culled = (VS_OUTPUT)0;
			culled.Position = asfloat(0x7FC00000).xxxx;
			return culled;
		}
		return FinishShellVertex(gridLocal, flatTerrain.x + max(flatTerrain.y, 0.0), saturate(flatTerrain.z), flatTerrain.x);
	}
#endif

	float coverage;
	float terrainHeight;
	float z;
#if defined(SNOW_DS_BAKE) || defined(SNOW_DS_BAKE_CHECK)
	// Patch corners are base-grid vertices the bake evaluated this frame;
	// read them by grid index. Points tessellation adds inside the patch are
	// not grid vertices and stay live. At a corner the lerp above is exact
	// (grid coordinates are multiples of the step, far below 2^24), so the
	// live and baked paths see the same gridLocal - the check variant
	// verifies exactly that.
	bool isCorner = (domainUV.x == 0.0 || domainUV.x == 1.0) && (domainUV.y == 0.0 || domainUV.y == 1.0);
	[branch] if (isCorner)
	{
		uint corner = domainUV.x > 0.5 ? (domainUV.y > 0.5 ? 2u : 1u) : (domainUV.y > 0.5 ? 3u : 0u);
		int3 bakeTexel = int3(patch[corner].GridXY, 0);
		float4 baked = ShellVertexBake.Load(bakeTexel);
		z = baked.x;
		coverage = baked.y;
		terrainHeight = baked.z;
		float2 slope = ShellVertexBakeSlope.Load(bakeTexel);
#	ifdef SNOW_DS_BAKE_CHECK
		float liveCoverage;
		float liveTerrain;
		float liveZ = ShellVertexZ(gridLocal, liveCoverage, liveTerrain);
		float2 liveSlope = ShellVertexSlope(gridLocal);
		float liveDeform = SampleDeformation(gridLocal);
		if (asuint(liveZ) != asuint(z) || asuint(liveCoverage) != asuint(coverage) || asuint(liveTerrain) != asuint(terrainHeight) ||
			asuint(liveSlope.x) != asuint(slope.x) || asuint(liveSlope.y) != asuint(slope.y) || asuint(liveDeform) != asuint(baked.w))
			z += 50.0;
#	endif
		return FinishShellVertexSloped(gridLocal, z, coverage, terrainHeight, slope.x, slope.y);
	}
	else
#endif
	{
		z = ShellVertexZ(gridLocal, coverage, terrainHeight);
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
	// Conservative depth carrying the anti-z-fight clamp. NOTE: LessEqual moves
	// depth TOWARD the camera, which can turn a failing fragment into a passing
	// one - so with the pass's LESS_EQUAL depth state this DISABLES early-Z
	// rejection for the whole draw. SNOW_SHELL_NO_DEPTH_EXPORT drops it to
	// measure what that costs.
#	ifndef SNOW_SHELL_NO_DEPTH_EXPORT
	float DepthLE : SV_DepthLessEqual;
#	endif
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

// Depth prepass (SNOW_SHELL_DEPTH_PREPASS): the alpha cut and the export
// clamp, nothing else. DepthLE is the shell's real depth (clamped); the
// colour target receives the RASTER depth of every fragment that wins, which
// the fill pass turns into the shading pass's EQUAL test buffer - a hardware
// EQUAL against the clamped depth would drop every clamped pixel.
struct PS_PREPASS_OUTPUT
{
	float RasterDepth : SV_Target0;
	float DepthLE : SV_DepthLessEqual;
};

// The depth the shell writes: the raster depth, pulled to just in front of
// the scene inside the two clamp windows. Shared by the prepass and the
// export tail so both agree bit for bit. FAR clamp: the z-fight/pinhole
// class loses at the source without moving geometry; far field only.
// NEAR micro-clamp: the edge zone and carved floors ride within window error
// of the mesh; 0.75 units is too thin to overdraw feet or props.
//
// sceneAbove tells a coincident terrain surface from an occluder: the scene
// point on this pixel's ray, in world height over THE SNOW LINE AT ITS OWN
// FOOTPRINT (ShellSceneAboveSnowLine). Terrain the shell z-fights sits on
// that line; a wall, a rock or a roof edge stands above it.
// Without this the far clamp pulled the ground behind a wall in front of the
// wall, the object's skin drawn after lost the depth test, and the ground in
// the wall's own shadow showed where the cap was - per object, past 4000
// units, moving with the camera, deaf to every shading toggle, and tipped
// into view by two units of bump octave (Josef, 2026-09-08).
static const float kShellClampOccluderSlack = 8.0;
// Is the scene surface on this pixel's ray the TERRAIN this shell covers, or
// something standing on it? Take the scene point along the ray and read the
// snow line at ITS OWN footprint: terrain the shell z-fights sits on that
// line, a wall top or a rock cap stands well above it. Measuring against the
// shell fragment's own height instead is not enough - ground banked up behind
// a low wall reads at the wall's height, so the clamp fired anyway and the
// caps stayed dark (Josef, 2026-09-08). Outside the terrain window, and over
// missing data, this says nothing and the clamp keeps its old behaviour.
float ShellSceneAboveSnowLine(float3 worldPosRel, float sceneZ, float shellZ)
{
	float3 sceneRel = worldPosRel * (sceneZ / max(shellZ, 1e-3));
	float2 sceneLocal = sceneRel.xy + ShellCameraPosAdjust.xy - GridOrigin;
	float2 tt = (GridToTerrainOffset + sceneLocal) / TerrainTexelSize;
	bool inWindow = all(tt >= 0.0) && all(tt <= (float)(TerrainDim - 1));
	float3 st = SampleTerrain(sceneLocal);
	float above = (sceneRel.z + ShellCameraPosAdjust.z) - (st.x + max(st.y, 0.0));
	return (inWindow && st.x > -50000.0) ? above : 0.0;
}

float ShellExportDepth(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ, float pixelEffDepth, float pixelCarve, float sceneAbove)
{
	float depth = rasterZ;
	float clampWindow = min(8.0 + shellZ * 0.008, 48.0);
	bool clampMode = (ShellDebugData == 0 || ShellDebugData >= 4) && ShellLODDebug == 0 && sceneAbove < kShellClampOccluderSlack;
	[branch] if (clampMode && shellZ > 4000.0 && shellZ > sceneZ && shellZ - sceneZ < clampWindow)
		depth = min(rasterZ, rawSceneDepth - 1e-5);
	else if (clampMode && (pixelEffDepth < 4.0 || pixelCarve > 0.5) && shellZ > sceneZ && shellZ - sceneZ < 0.75)
		depth = min(rasterZ, rawSceneDepth - 1e-5);
	return depth;
}


// SHELL-SURFACE SSS RE-MARCH (opt-in, CompactLook.y).
//
// The precomputed SSS mask describes only the BURIED ground - it is marched on
// pre-shell depth, so no gate can make it mean anything about the snow surface.
// This marches the same depth buffer from the SHELL surface and admits an
// occluder only if it stands above the snow line at its own footprint: grass
// poking through shadows the snow, a buried plank cannot, and it covers actors
// too, since it tests geometry height rather than capture.
//
// The shell pass does not bind FrameBuffer b12, so the DR scale rides
// CompactLook.zw from the CPU.
#if defined(PSHADER)
float ShellRemarchSSS(float3 relPos, float3 L, float noise, float2 dynRes, bool thicknessWindow, float casterCap)
{
	// Bend SSS's anti-streak device (SurfaceThickness): an occluder shadows
	// only samples within a bounded depth window, never everything behind it.
	// Unbounded, a character between camera and ray point paints their
	// silhouette as a streak across the snow. 48 units is Bend's
	// 0.5%-of-remaining-depth default at Skyrim ranges; thin casters are
	// unaffected, and the trade is mild under-shadowing behind thick objects.
	const float kOccluderThickness = 48.0;
	// Contact range: grass and rails are short casters, so the steps stay
	// tight and grow geometrically rather than reaching for distance.
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
				// Something stands between this step and the camera - and,
				// with the streak fix on, within the thin-shell window of
				// the ray point rather than anywhere in front of it.
				[branch] if (occZ < clip.w - 1.0 && (!thicknessWindow || occZ > clip.w - kOccluderThickness))
				{
					// ...and the SAME view ray puts it here in world space
					// (the depth-ratio reconstruction already used for the
					// contact fade), so ask the snow field how high the
					// surface is under it.
					float3 occRel = sampleRel * (occZ / max(clip.w, 1e-3));
					float2 occLocal = occRel.xy + ShellCameraPosAdjust.xy - GridOrigin;
					float3 st = SampleTerrain(occLocal);
					float snowTop = st.x + max(st.y, 0.0);
					// A band, not a floor. Lower bound (+2 slack) stops
					// coincident surfaces self-shadowing; upper bound (the
					// caster height cap) drops anything already casting
					// through the cascades, whose re-march copy is a doubled
					// soft bleed. Grass lives under ~40 units.
					float occH = occRel.z + ShellCameraPosAdjust.z - snowTop;
					[flatten] if (occH > 2.0 && occH < casterCap)
						occl = max(occl, 1.0 - float(i) * 0.045);
				}
			}
		}
	}
	return 1.0 - occl;
}
#endif

#ifdef SNOW_SHELL_DEPTH_PREPASS
PS_PREPASS_OUTPUT main(VS_OUTPUT input)
#else
PS_OUTPUT main(VS_OUTPUT input)
#endif
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

	// Un-carved class depth ramp; negative means the shell is submerged. The
	// dither rides it, so a boundary toward shallower classes dissolves as the
	// shell thins rather than plunging geometrically. Mirrors the VS
	// object-depth cap so alpha agrees with the capped geometry.
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
	// mid-air as a floating rim.
	float pixelEffDepth = pixelRampDepth > 0.0 ? pixelRampDepth * smoothstep(0.0, 5.0, pixelRampDepth) : pixelRampDepth;
	// Two factors, two jobs. The CLASS GATE enforces the assigned-depth design:
	// blended class depth must exceed ~2 units, so negative-depth classes and
	// their blend plateaus never carry shell, independent of the band slider.
	// The CONTACT term is the touchdown contest: w = 0.5 where the sheet meets
	// the ground, so grain interlocks at the fringe. One number cannot do both.
	float rampTerm = smoothstep(1.0, 3.0, pixelRampDepth) * saturate(0.5 + pixelEffDepth / rampFadeBand);
	float coverageAlpha = smoothstep(0.0, 0.6, pixelCoverage) * psEdgeFade * rampTerm;
	// Josef's border spec: the alpha may not cut geometry still standing
	// above its ground - the sheet dives below the terrain (the VS descent's
	// undershoot) and the depth test against the terrain IS the border.
	// Keyed on the RENDERED height above the terrain data, not the ramp, so
	// low-depth class plateaus hugging the ground under half a unit still
	// die by the class gate instead of z-fighting as a film. The window-edge
	// fade rides along; the overrides and contests below still multiply.
	float sheetAboveGround = smoothstep(0.25, 1.5, input.WorldPos.z + ShellCameraPosAdjust.z - pixelTerrain.x);
	coverageAlpha = max(coverageAlpha, sheetAboveGround * psEdgeFade);

	// Object blending (Terrain Blending-style depth proximity): the shell only
	// knows terrain heights, so this is what makes it meet statics softly
	// instead of slicing across them at the depth test. The gap is measured
	// along the view ray and shifts with the camera; mild distance scaling
	// turns that wobble into a soft fade. Keep the base short and the scaling
	// mild - a long or view-Z-proportional band reads as a translucent margin.
	float objectFadeBand = 5.0 + shellZ * 0.004;
	float proximityFade = saturate((sceneZ - shellZ) / objectFadeBand);
	// The grain-driven descent lays the rim onto its own terrain, which this
	// fade would read as hovering over geometry and crush to specks. So exempt
	// pixels whose backdrop IS the terrain: reconstruct the scene surface's
	// world height along the ray and fade only where something stands above the
	// terrain data. Sentinel terrain (-50000) reads as objectness 1, so data
	// gaps keep the plain fade. Distance-gated like the contest.
	[branch] if (HasSnowHeight > 0.5 && shellZ < 2048.0)
	{
		float sceneSurfaceZ = ShellCameraPosAdjust.z + input.WorldPos.z * (sceneZ / max(shellZ, 1e-3));
		float objectness = smoothstep(1.5, 6.0, sceneSurfaceZ - pixelTerrain.x);
		proximityFade = max(proximityFade, 1.0 - objectness);
	}
	// Two cases hug the geometry behind them and must override the fade, or
	// they dither away view-dependently: carved trench floors, and the shell
	// riding a raised height field. The carve override is also what ends
	// trenches hard at class borders while untrampled snow dissolves softly.
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
	// Trampled Border Fade retired: its default-0 behavior is the
	// keeper, so the window is the constant it resolved to.
	float carveOverride = smoothstep(0.1, 0.5, pixelCarve) * smoothstep(0.5, 1.0, pixelTerrain.y);
	coverageAlpha *= max(proximityFade, saturate(carveOverride + smoothstep(2.0, 10.0, pixelLift) + smoothstep(0.1, 0.4, pixelMelt)));

	// Height-blended edges (HEIGHT-BLEND-PLAN pairs 1+2): shape the COMBINED
	// alpha once, after the overrides, so every partial band commits by grain
	// whatever produced it. Shaping components individually leaves an override's
	// own edge unshaped. Shaping preserves 0 and 1, so overrides still win. Mip
	// outside the branch for derivatives.
	float2 edgeSnowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	float edgeSnowMip = SnowHeightMip(edgeSnowUV);
	// Geometric edge contest: keep a pixel where the sheet's surface (toe'd
	// height + grain) stands above the dirt's grain surface. Gated on our own
	// distance fade, not EM's height-blending checkbox - keying it there let
	// the sheet fall back to a hard cut whenever that was off. Multiplied in,
	// so the class design and the overrides keep their word; dirt can only eat
	// in, and the slice layer carries the outward side.
	float contestFade = 1.0 - smoothstep(1024.0, 2048.0, shellZ);
	[branch] if (HasSnowHeight > 0.5 && contestFade > 0.001 && coverageAlpha > 0.001 && coverageAlpha < 0.999)
	{
		float edgeSnowH = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, GridOrigin + gridLocal), 0.0.xx, edgeSnowMip);
		// Statics behind the shell carry the recolor's weight here as
		// 2 + w (Lighting.hlsl); only (0, 1] is a land grain height.
		float hLandRaw = LandMasksCopy.Load(int3(input.Position.xy, 0)).y;
		// Fixed amplitudes (band-scaled grain magnified the
		// contest with the slider). The land's grain is the POM hit -
		// view-dependent - so it weighs in at HALF strength around neutral:
		// half the camera breathing, most of the detail; the snow-side
		// grain is world-anchored and stays full.
		const float kEdgeGrainAmp = 2.0;
		const float kEdgeDirtAmp = 2.0;
		float hLand01 = hLandRaw > 0.002 && hLandRaw < 1.5 ? saturate((hLandRaw - 0.004) * (1.0 / 0.996)) : 0.5;
		float dirtSurf = (0.5 + (hLand01 - 0.5) * 0.5) * kEdgeDirtAmp;
		float snowSurf = pixelEffDepth + (edgeSnowH - 0.5) * kEdgeGrainAmp;
		float win = smoothstep(-0.25, 0.25, snowSurf - dirtSurf);
		coverageAlpha *= lerp(1.0, win, contestFade);
	}

	// Trench-floor contest: trampled floors run the same geometric contest as
	// the edge - remaining snow against the dirt's grain - so wear-through
	// opens grain-shaped holes by design rather than by window error. Trench
	// Floor Height is the dial: at 3+ the snow always beats the ~2-unit dirt
	// grain; toward 0 trampling wears through. The tight carve gate keeps
	// walls solid. Runs after the carve override, and is the one voice
	// allowed to overrule it.
	[branch] if (HasSnowHeight > 0.5 && contestFade > 0.001 && pixelCarve > 0.75 && coverageAlpha > 0.001)
	{
		float floorEff = min(pixelEffDepth, BorderStyle.y * smoothstep(0.5, 8.0, pixelEffDepth));
		float remaining = max(pixelEffDepth * (1.0 - pixelCarve), floorEff);
		[branch] if (remaining < 4.0)
		{
			float floorGrain = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, GridOrigin + gridLocal), 0.0.xx, edgeSnowMip);
			float hLandFloorRaw = LandMasksCopy.Load(int3(input.Position.xy, 0)).y;
			float hLandFloor01 = hLandFloorRaw > 0.002 && hLandFloorRaw < 1.5 ? saturate((hLandFloorRaw - 0.004) * (1.0 / 0.996)) : 0.5;
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
	// committed sheet; off is a clean binary cut.
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

#ifdef SNOW_SHELL_DEPTH_PREPASS
	PS_PREPASS_OUTPUT prepassOut;
	prepassOut.RasterDepth = input.Position.z;
	prepassOut.DepthLE = ShellExportDepth(input.Position.z, rawSceneDepth, shellZ, sceneZ, pixelEffDepth, pixelCarve, ShellSceneAboveSnowLine(input.WorldPos, sceneZ, shellZ));
	return prepassOut;
#endif
#ifndef SNOW_SHELL_DEPTH_PREPASS

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
	// Shading depth mirrors ShellSurfaceZ's uncarved depth: melt thins toward
	// kFireMeltFloor, then the touch-down toe. Every relief gradient below
	// scales by it, so a print in a melt basin shades as flat as it is built.
	float pixelDepth = lerp(pixelRampDepth, min(pixelRampDepth, kFireMeltFloor), pixelMelt);
	pixelDepth = pixelDepth > 0.0 ? pixelDepth * smoothstep(0.0, 5.0, pixelDepth) : 0.0;
	float2 profileGrad = float2(
		CarveProfile(saturate(dXP), pixelDepth, GridOrigin + gridLocal + float2(step, 0.0)) - CarveProfile(saturate(dXN), pixelDepth, GridOrigin + gridLocal - float2(step, 0.0)),
		CarveProfile(saturate(dYP), pixelDepth, GridOrigin + gridLocal + float2(0.0, step)) - CarveProfile(saturate(dYN), pixelDepth, GridOrigin + gridLocal - float2(0.0, step))) / (2.0 * step);
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
	float2 gradZ = -terrainNormal.xy / max(terrainNormal.z, 0.1) + profileGrad + bermGrad * pixelDepth * BermHeightAmp * BermDepthGate(pixelDepth);

	// Undulation gradient (same field the VS displaced by) shades the dunes.
	float2 worldXYPS = GridOrigin + gridLocal;
	float undScale = saturate(pixelDepth / 8.0);
	[branch] if (undScale > 0.001)
	{
		gradZ += UndulationGradSampled(worldXYPS) * undScale;
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

	// Bow wave gradient (same field the VS displaces by). Wide step: the
	// crest is tens of units across, and a short step reads its smooth
	// flanks as noise.
	[branch] if (BowWaveParams.y > 0.001)
	{
		const float wStep = 6.0;
		float wXP = BowWaveHeight(worldXYPS + float2(wStep, 0.0), gridLocal + float2(wStep, 0.0), pixelCarve, pixelDepth);
		float wXN = BowWaveHeight(worldXYPS - float2(wStep, 0.0), gridLocal - float2(wStep, 0.0), pixelCarve, pixelDepth);
		float wYP = BowWaveHeight(worldXYPS + float2(0.0, wStep), gridLocal + float2(0.0, wStep), pixelCarve, pixelDepth);
		float wYN = BowWaveHeight(worldXYPS - float2(0.0, wStep), gridLocal - float2(0.0, wStep), pixelCarve, pixelDepth);
		gradZ += float2(wXP - wXN, wYP - wYN) / (2.0 * wStep);
	}

	// P6 clod gradient (same field the VS displaces by; centre-weighted
	// like the churn gradient - the clod frequency is far above the berm
	// field's, so the noise gradient dominates).
	float clodW = RimStyle.z > 0.01 ? BermShape(bermCenter) * saturate(1.0 - pixelCarve) * BermDepthGate(pixelDepth) : 0.0;
	[branch] if (clodW > 0.001)
	{
		const float kStep = 4.0;
		float kXP = ChurnNoiseScaled(worldXYPS + float2(kStep, 0.0), kClodSizeScale);
		float kXN = ChurnNoiseScaled(worldXYPS - float2(kStep, 0.0), kClodSizeScale);
		float kYP = ChurnNoiseScaled(worldXYPS + float2(0.0, kStep), kClodSizeScale);
		float kYN = ChurnNoiseScaled(worldXYPS - float2(0.0, kStep), kClodSizeScale);
		gradZ += float2(kXP - kXN, kYP - kYN) / (2.0 * kStep) * RimStyle.z * clodW;
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
	// One quad fetch for both: the scorch read further down wants the same
	// four texels of the same map at the same place, and both sites are
	// unconditional, so sharing costs nothing and skips four loads.
	float scorchRaw, crustRaw;
	SampleScorchCrust(gridLocal, scorchRaw, crustRaw);
	float crustAmount = saturate(crustRaw * SpellShading.y);
	bumpFade *= lerp(1.0, 1.0 - saturate(SpellShading.w), crustAmount);
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	SnowTaps snowTaps = ComputeSnowTaps(snowUV, worldXYPS);
	// Uniform flow: the parallax shadow branch below is divergent, and
	// derivatives taken inside it would be garbage at its edges.
	float snowHeightMip = SnowHeightMip(snowUV);
	// Two-plane projection: top-down uv smears down near-vertical trench walls,
	// so steep pixels blend in a side-plane sample. Keyed on the per-pixel
	// trench normal, captured BEFORE the normal map perturbs normalWS - the
	// side POM march must resolve the view into the same plane these uvs use.
	// Earlier and narrower than the statics ramp: landscape walls live at 40-65
	// degrees, where any top-projection share boils as the camera moves. Side
	// takes over fully by ~57 degrees.
	float snowSteepness = smoothstep(0.75, 0.55, abs(normalWS.z));
	float snowWorldZAbs = input.WorldPos.z + ShellCameraPosAdjust.z;
	bool snowSideDropsX = abs(normalWS.x) > abs(normalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXYPS.y, snowWorldZAbs) : float2(worldXYPS.x, snowWorldZAbs);
	// NO SnowUVOffset on the side plane, and a STATIC 4096-unit fold (exactly
	// 24 tiles). SnowUVOffset compensates gridLocal's rebase, but the side
	// plane is absolute and never rebases, so adding it slides the wall texture
	// every time the grid advances. The fold keeps the uv small for float
	// precision; derivatives and mip come from the UNFOLDED uv.
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
		// Trench walls march NOTHING: a march is only honest when the surface
		// lies in its projection plane, and a 40-60 degree wall lies in neither,
		// so both marches redraw the wall as the camera moves. A steepness scale
		// still leaves most of the top march live there. Hard tilt cut instead,
		// dead by ~40 degrees, side plane sampled unmarched - at a 21-unit wall
		// stability beats parallax.
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

	// Frost crystal, laid on top of the flattened powder grain rather than
	// instead of it: the flattening is what reads as frozen over, this puts the
	// rime back as its own structure. Fetched once and used three times below
	// (normal, albedo, polish) - they must agree about where a crystal is.
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
		float scorch = scorchRaw * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
	// Thin-snow print: a boot through a dusting (melt floors, thin classes)
	// has no wall to light, so it reads by material - pressed wet, darker and
	// smoother. Gone by ~6 units of cover, where relief takes over.
	float wetPrint = smoothstep(0.1, 0.5, pixelCarve) * (1.0 - smoothstep(1.5, 6.0, pixelDepth));
	[branch] if (wetPrint > 0.001)
		kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.76, 0.77, 0.80), wetPrint);
	// PBR snow material: GGX microfacet specular with Fresnel and energy-
	// conserving lobes. Light and ambient stay in the frame's units
	// (DirLightColor is already pi-scaled by pipeline convention, so no
	// Lambert 1/pi on diffuse); the indirect specular lobe goes to the
	// Reflectance RT where the composite applies cubemap and ambient
	// specular like any TruePBR surface.
	float kSnowRoughness = 0.6;
	float3 kSnowF0 = float3(0.028, 0.028, 0.028);

	// Crust: refrozen snow shades as ice, not powder. All three cues are on
	// sliders - physically honest values are far too subtle to see.
	// ORDER: only the colour cast belongs here. Roughness and reflectance must
	// be applied AFTER the RMAOS block below, which overwrites both from the
	// snow material whenever a PBR set is installed.
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
	// Wet print: same ordering rule as crust, after the RMAOS overwrite.
	snowRoughness = lerp(snowRoughness, snowRoughness * 0.76, wetPrint);

	// The crystal has to be the part that shines. A normal map alone tilts
	// facets away from the light and puts the highlight in the gaps between
	// them - matte structure on glossy ground. Modulating polish by the same
	// tap that shaped the normal keeps the highlight on the ice.
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
	[branch] if ((ShellFlags.x & 1) != 0 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
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
		// The receiver REBUILT the way the taps are, so what the reconstruction
		// gets wrong - bilinear terrain against the triangulated mesh, the
		// max-of-four object raster - cancels instead of standing as a false
		// rise over the nearest tap: flat snow shadowed itself in texel-sized
		// blotches at a low sun. One unit of bias absorbs the residue.
		float marchRef = surfZ;
		{
			float3 st0 = SampleTerrain(gridLocal);
			[branch] if (st0.x > -50000.0)
			{
				float depth0 = max(st0.y, 0.0);
				float2 mask0 = SampleExclusionMask(GridOrigin + gridLocal);
				depth0 = lerp(depth0, min(depth0, kFireMeltFloor), saturate(mask0.y));
				float deform0 = SampleDeformationMarch(gridLocal);
				float berm0 = BermBakeActive > 0.5 ? BermFieldBaked(gridLocal) : 0.0;
				depth0 = CarveProfile(deform0, depth0, GridOrigin + gridLocal) +
				         BermShape(berm0) * saturate(1.0 - deform0) * depth0 * BermHeightAmp * BermDepthGate(depth0);
				marchRef = st0.x + depth0 + Undulation(GridOrigin + gridLocal) * saturate(depth0 / 8.0);
			}
		}
		marchRef = max(marchRef, surfZ - 2.0);
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
			float sampleDeform = SampleDeformationMarch(sampleLocal);
			// The berm occluder reads the berm FIELD, as the geometry does.
			// BermShape of a raw deformation value peaks across the trench's
			// sloping wall, which rings every trail with a phantom ridge. One
			// bilinear tap when the bake is live; the unbaked A/B path skips
			// the term rather than paying 17 taps per march step.
			float sampleBerm = BermBakeActive > 0.5 ? BermFieldBaked(sampleLocal) : 0.0;
			sampleDepth = CarveProfile(sampleDeform, sampleDepth, GridOrigin + sampleLocal) +
			              BermShape(sampleBerm) * saturate(1.0 - sampleDeform) * sampleDepth * BermHeightAmp * BermDepthGate(sampleDepth);
			// Live undulation ON PURPOSE: in the march, ALU is free and loads
			// are the bottleneck, so the bake's 4 loads per tap were a
			// regression here (2026-08-30). Geometry and shading keep the bake.
			// No object term: every object under the shell is geometry the
			// cascades already shadow with its real silhouette, and the shell
			// draped over it casts through its own caster.
			float sh = st.x + sampleDepth + Undulation(GridOrigin + sampleLocal) * saturate(sampleDepth / 8.0);
			horizonTan = max(horizonTan, (sh - marchRef - 1.0) / d);
		}
		// Near: a crisp penumbra band. Far: a much wider penumbra plus
		// attenuated strength; the march's per-texel horizon steps stop
		// reading as hard-edged blocks on distant snow.
		// Near softness halved: it existed to hide the tap quantisation the
		// finer first taps now resolve. Far end untouched.
		float soft = lerp(0.03, 0.35, farShadowT);
		// Penumbra CENTRED on the horizon: the old band was the same total
		// width (3*soft) but sat entirely on the LIT side of
		// sunTan == horizonTan, so the shadow always over-reached its
		// geometric edge - worse the lower the sun. Kept identical to the
		// statics march; the two shells must agree across the seam.
		// Fades out toward the horizon: a 4-unit raster and a 32-unit terrain
		// texel cannot resolve contact shadows at a grazing sun, every bump
		// and draped stone became a blotch at sunset, and the cascades own
		// the long shadows there anyway. Same gate on the statics march.
		float lowSun = smoothstep(0.04, 0.12, sunTan);
		sunShadow *= lerp(1.0, lerp(smoothstep(-1.5 * soft, 1.5 * soft, sunTan - horizonTan), 1.0, 0.7 * farShadowT), lowSun);
	}

	// SSS gate diagnostics for ShellDebugData 4: x = mask darkness (what
	// the ground-marched mask wants to print here), y = gate trust,
	// z = the buried-caster probe fired. Stays black when the feature is
	// off or inactive - itself a diagnostic.
	float3 sssDebug = float3(0.0, 0.0, 0.0);

	// Screen-Space Shadows carry distant LOD tree shadows beyond the cascades,
	// but the texture is marched on prepass depth (the ground UNDER the shell),
	// so applying it near paints buried objects through the snow. Cascades own
	// the near field; SSS blends in only beyond them, where it is the only
	// source and the shell hugs the ground the march ran on.
	[branch] if (ScreenSpaceShadowsActive > 0.5)
	{
		// Depth agreement is the gate: trust the mask only where the shell hugs
		// the surface the march saw. Measured VERTICALLY, not along the view
		// ray - the along-ray gap is depth / sin(elevation) and explodes at far
		// grazing views, which forces a distance override that then paints
		// buried shadows through distant drifts. One vertical rule covers every
		// range. The along-ray term survives as a backstop for steep faces seen
		// edge-on. Thresholds cannot separate grass shadows from buried prints;
		// their gaps overlap, so the caster discriminator below does it.
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
					// A buried caster is by definition LOW - planks and rails
					// poke barely above the snow, so a "stands well above the
					// shell" trigger misses exactly the casters that print.
					// Any captured surface from slightly-buried upward counts;
					// grass is never captured, so it cannot be touched here.
					[flatten] if (tapTop > -50000.0 && tapTop > sssSurfZ - 16.0)
					{
						sssBlend = 0.0;
						sssDebug.z = 1.0;
					}
				}
			}
		}
		// The near field takes no SSS at all: even a 35% contact version
		// still reads as shadows from under the snow. Casters' true shadows
		// come from the cascades near; the mask only earns its keep past
		// 2500, where it is the sole source of LOD tree shadows. Known
		// trade: near-field grass shadows on the shell die with it, since
		// grass casts only via this march. This ramp is the dial.
		// Distance gate pinned to the cascades themselves rather than a
		// fixed band: cascades own everything until
		// their own distance fade, the mask ramps in as its exact complement
		// (GetSssHandoff), so the handoff tracks the user's shadow distance
		// with no gap and no ghost overlap. The vertical hug metric and the
		// caster probe above still guard the buried-print cases.
		sssBlend *= SnowShadow::GetSssHandoff(shellZ);
		float sssMask = ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);
		sunShadow *= lerp(1.0, sssMask, sssBlend);
		sssDebug.x = 1.0 - sssMask;
		sssDebug.y = sssBlend;
	}

	// Shell-surface re-march (opt-in): the near-field counterpart to the
	// mask above. Runs where the band leaves off, so the two never double.
	[branch] if (CompactLook.y > 0.5 && ScreenSpaceShadowsActive > 0.5 &&
		SnowShadow::GetSssHandoff(shellZ) < 0.999 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		// Packed: integer part = mode (1 march, 2 march + thickness),
		// fraction * 1000 = the caster height cap in units.
		float remarchCap = frac(CompactLook.y) * 1000.0;
		float remarch = ShellRemarchSSS(input.WorldPos, L, screenNoise, CompactLook.zw, CompactLook.y > 1.5, remarchCap);
		// Faded out across the band the precomputed mask fades in over, so
		// the handover is continuous.
		sunShadow *= lerp(remarch, 1.0, SnowShadow::GetSssHandoff(shellZ));
	}

	// Parallax self-shadow on the snow's own grain: EM's
	// GetParallaxSoftShadowMultiplier, four fixed taps along the light in
	// tangent space, no march. Distinct from the heightfield march above, which
	// shadows at terrain scale; this shadows WITHIN one texture repeat. The
	// fetches are ours, but every constant and the occlusion formula are EM's,
	// so the response matches the ground by construction.
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
		// Faded on the same band as the normal map it occludes, and faded out
		// on walls: the march is jittered by screen-anchored noise, which on a
		// wall's high-contrast side-projected grain crawls as the camera moves.
		// Walls take the trench self-shadow march and the cascades instead.
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}

	float3 sunLight = SharedData::DirLightColor.xyz * sunShadow;

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1): glints, energy conservation and every future
	// TruePBR lobe ride the shared code. Outputs are Lighting-internal units;
	// Color::PBRLightingScale is applied at the write tail below.
	// World-anchored glint uv: snowUV's fmod(GridOrigin) fold shifts by whole
	// tiles as the grid advances - a no-op for the periodic texture, but the
	// glint hash is NOT tile-periodic, so it re-rolls the sparkle field every
	// few metres. Fold on a STATIC 4096-unit block (an exact tile multiple).
	const float2 glintUV = fmod(GridOrigin + gridLocal, 4096.0) / kSnowUVTile;
	// Built once, shared by the sun and every point light (M3).
	SnowMaterialCtx snowMtl = SnowBuildMaterial(normalWS, kSnowAlbedo, snowRoughness, snowF0, snowAO,
		SnowGlintParams, EnableGlints, glintUV, glintDuvdx, glintDuvdy, input.Position.xy);
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
		// G = melt fraction (fires, workspaces, shelter), B = coverage
		// suppression - the sealed-container rectangles read here, so
		// this is the view that shows where a sarcophagus footprint
		// actually landed. Black = untouched by any of them.
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
		// Warp-band view: one color per power-of-two band, brightening across
		// the band so the data morph's hand-off is visible. Band 0 (the 8-unit
		// linear zone) stays gray. Bands are world-anchored now, so these
		// stripes must hold still on the ground as the camera moves - stripes
		// that crawl mean the band table has lost its lattice alignment.
		float2 uAxis = float2(InverseWarpAxis(gridLocal.x - WarpedHalfSpan), InverseWarpAxis(gridLocal.y - WarpedHalfSpan));
		float3 band = WarpBand(max(abs(uAxis.x), abs(uAxis.y)));
		[flatten] if (band.z < 0.5)
			preLit = float3(0.15, 0.15, 0.15);
		else
		{
			static const float3 kRingColors[6] = {
				float3(1.0, 0.2, 0.2), float3(1.0, 0.8, 0.2), float3(0.3, 1.0, 0.3),
				float3(0.2, 0.9, 0.9), float3(0.3, 0.4, 1.0), float3(0.9, 0.3, 0.9)
			};
			preLit = kRingColors[(uint)band.z % 6u] * lerp(0.35, 1.0, band.y);
		}
	}
	else if (ShellLODDebug == 3)
	{
		// Provenance: green = baked cell data; LOD-classified far texels
		// (w = 2 + score) grade brown (classified bare) -> blue-white
		// (classified snow) so the classification itself is inspectable;
		// cyan = no LOD tile (filled bare); red = no data at all
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
	else if (ShellLODDebug == 4)
	{
		// Land-exact delta: the vertex-interpolated terrain height against the
		// fine layer at this pixel. Green within 2 units, amber -> red = shell
		// terrain above the land (red at 64+), teal -> blue = below, gray = no
		// fine data here.
		float land = LandHeightFine(gridLocal);
		float d = input.DebugHeight - land;
		[flatten] if (land < -50000.0)
			preLit = float3(0.3, 0.3, 0.3);
		else if (abs(d) < 2.0)
			preLit = float3(0.1, 0.8, 0.1);
		else if (d > 0.0)
			preLit = lerp(float3(0.7, 0.55, 0.0), float3(1.0, 0.0, 0.0), saturate((d - 2.0) / 62.0));
		else
			preLit = lerp(float3(0.0, 0.55, 0.6), float3(0.0, 0.0, 1.0), saturate((-d - 2.0) / 62.0));
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

	// Conservative depth clamp: where the shell falls just behind the rendered
	// ground, pull its depth to just in front, so the z-fight/pinhole class
	// loses at the source without moving geometry. Fires only within a short
	// window behind the surface, and is skipped in debug views.
	// FAR FIELD ONLY: it cannot tell a legitimate occluder from a coincident
	// terrain surface, so anything standing in the snow would be overdrawn.
#	ifndef SNOW_SHELL_NO_DEPTH_EXPORT
	psout.DepthLE = ShellExportDepth(input.Position.z, rawSceneDepth, shellZ, sceneZ, pixelEffDepth, pixelCarve, ShellSceneAboveSnowLine(input.WorldPos, sceneZ, shellZ));
#	endif

	return psout;
#endif  // !SNOW_SHELL_DEPTH_PREPASS
}
#endif

#ifdef COMPUTESHADER
// LOD shimmer probes: evaluates the ACTUAL mesh surface (warp, snapping,
// ShellSurfaceZ) at world-anchored points. Probing the field at the probe XY
// would miss the vertex hops entirely, since the pops come from vertices
// resampling the field at snapped positions - so quad corners are rebuilt
// exactly as the VS builds them. Anchored to a 512-unit-quantised camera XY,
// which the CPU mirrors.
RWStructuredBuffer<float> ProbeHeights : register(u0);

// Per-vertex surface bake: one ShellVertexZ per base-grid vertex, placed
// exactly as the tessellation VS places it, read back by the domain shader
// at patch corners (SNOW_DS_BAKE). Rebuilt every frame - the surface depends
// on the camera (descent and relief fade with distance) and on the bow
// waves, so nothing here is worth tracking dirty.
RWTexture2D<float4> ShellVertexBakeOut : register(u1);
RWTexture2D<float2> ShellVertexBakeSlopeOut : register(u2);

[numthreads(8, 8, 1)] void BakeCS(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy > GridDim))
		return;
	float2 gridLocal = ShellGridVertexLocal(id.xy);
	float coverage;
	float terrainHeight;
	float z = ShellVertexZ(gridLocal, coverage, terrainHeight);
	ShellVertexBakeOut[id.xy] = float4(z, coverage, terrainHeight, SampleDeformation(gridLocal));
	ShellVertexBakeSlopeOut[id.xy] = ShellVertexSlope(gridLocal);
}

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
