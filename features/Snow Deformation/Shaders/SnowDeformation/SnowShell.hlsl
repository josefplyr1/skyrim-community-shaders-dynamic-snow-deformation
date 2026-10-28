// Snow Deformation for Community Shaders.
// Copyright (c) 2026 josefplyr1. GPL-3.0-or-later.
// Source: github.com/community-shaders/skyrim-community-shaders/pull/2659

// The ground snow's surface and material: the landscape's own meshes,
// replayed by LandscapeSnowShell.hlsl, raised by per-class snow depth and
// carved by the deformation map; a depth prepass writes the deferred
// attributes and one full-screen pass shades them. Per-pixel normals come from
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
#include "SnowDeformation/SnowColor.hlsli"

#ifdef PSHADER
// Shadows: terrain/cloud via GetWorldShadow, actors via the game's raw cascade
// atlas read in place (SnowShadow.hlsli), falling back to the shared VSM. The screen-space
// mask applies near only when it was marched on the snow (ScreenSpaceShadowsActive
// 2); otherwise it holds the terrain behind the shell along the view ray.
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

#if defined(PSHADER) && defined(SNOW_SHELL_DEFERRED)
// Deferred shading: the prepass's own coarse derivatives of GridLocal and of
// WorldPos.z, read back per pixel. A full-screen pass has no triangle to take
// them from; every derivative the shading needs is a chain-rule product of
// these two.
static float2 gDeferredGridDx;
static float2 gDeferredGridDy;
static float gDeferredZDx;
static float gDeferredZDy;
// The prepass's coverage ramp (AttrDepth.w).
static float gDeferredRampDepth;
// The point at view depth a_w on the view ray through pixel a_pos: rows 0, 1
// and 3 of the jittered view-projection solved for a_rel (Cramer), the pixel
// mapped through the render viewport. A macro: as a function fxc schedules
// the deferred pass differently.
#	define SHELL_DEFERRED_REL(a_rel, a_pos, a_w)                                                                                               \
		{                                                                                                                                      \
			const float2 ndc_ = float2((a_pos).x / TessScreen.y - 1.0, 1.0 - (a_pos).y / TessScreen.z);                                        \
			const float3 r0_ = CameraViewProj[0].xyz;                                                                                          \
			const float3 r1_ = CameraViewProj[1].xyz;                                                                                          \
			const float3 r3_ = CameraViewProj[3].xyz;                                                                                          \
			const float3 c0_ = cross(r1_, r3_);                                                                                                \
			const float3 c1_ = cross(r3_, r0_);                                                                                                \
			const float3 c2_ = cross(r0_, r1_);                                                                                                \
			const float3 b_ = float3(ndc_.x * (a_w) - CameraViewProj[0].w, ndc_.y * (a_w) - CameraViewProj[1].w, (a_w) - CameraViewProj[3].w); \
			a_rel = (b_.x * c0_ + b_.y * c1_ + b_.z * c2_) / dot(r0_, c0_);                                                                    \
		}
#endif

// SNOW_SURFACE_ONLY: included for its surface recipe alone (LandscapeSnow.hlsl),
// with ShellCB moved off b0 and no entry points or stage structs. The vertex,
// hull and domain entry points are LandscapeSnowShell.hlsl's.
#ifndef SNOW_SHELL_CB_REGISTER
#	define SNOW_SHELL_CB_REGISTER b0
#endif
cbuffer ShellCB : register(SNOW_SHELL_CB_REGISTER)
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
	uint LayoutKeeper6;  // layout keeper, 0
	uint TerrainDim;
	uint TrenchDetailView;  // 1: trench detail coverage view

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
	float TrenchDetailRelief;     // detail tile height and slopes x this; 0 = off
	float TrenchDetailRoughness;  // roughness x this over the detail mask

	float LayoutKeeper5;         // layout keeper, 0
	float TrenchDetailCoverage;  // TrenchDetailMask cut: 0 floor .. 1 past the rim .. 2 berm
	float ShellCullBare;         // was SeamFadeUnused: cull fully-bare triangles
	float SkinFadeStart;         // statics skin: distance dissolve start (units)

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

	float SpeedSpoil;     // Settings::SpeedSpoil, 0 or 1
	float LayoutKeeper9;  // retired Detail Set blend, uploaded as 0
	// Layout keepers, 0.
	float DebugNoFarPad;
	float LayoutKeeper4;

	float LayoutKeeper7;       // layout keeper, 0
	float TrenchDetailShadow;  // the detail's sun self-shadow, 0 = off
	float LayoutKeeper3;       // layout keeper, 0
	// Units across which the sheet and an object's coat meet, half on each
	// side of the junction; 0 = off.
	float ObjectMeetBand;

	// 1 / Berm Steepness: the berm tap reach (BermGradStep).
	float BermReach;
	uint LayoutKeeper10;  // retired Detail Set slice, uploaded as 0
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
	// march), w spare (the retired Screen-Space Parallax Trenches' reach).
	float4 SnowParallax;

	// x = scorch darkening strength; yzw spare (the retired crust look).
	float4 SpellShading;
	// Track age (TrackAgeOf): x = the ageing clock now, y = age per clock
	// unit (0 = tracks never age); zw spare. Was the retired crust look's row.
	float4 TrackAge;
	// Spare: the retired crust look's second row, kept for the layout.
	float4 padCrustLook2;
	// xy layout keepers; zw = sun cascades' REAL atlas slices (the shared
	// atlas moves them with the active-light set).
	float4 BorderStyle;
	// x spare; yz = the land contest's depth bonus and carve penalty; w =
	// trench wall slough (Snow Climate, CarveProfile).
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
	// x bit 0 horizon march; y bit 0 road tops in the skin-depth raster.
	// Mirror in SnowDeformation.h.
	int2 ShellFlags;

	// Land-exact height layer: xy = GridOrigin - fine window origin (world),
	// z = fine dim in texels (0 = none), w = fine texel size (32).
	float4 FineWindow;
	float4 SlopeDrape;
	float4 DebugSkinDepth;  // object skins only (SnowStaticsShell.hlsl)
	// Blood: x = intensity, y = burial clock now, z = game hours now,
	// w = snowfall that buries a mark (fraction of a refill).
	float4 BloodLook;
	// x = hours a mark takes to dry, y = wet sheen, z > 0.5 = map live,
	// w > 0.5 = detail tiles live.
	float4 BloodLook2;
	// xy spare; z = the detail tiles' cell in units, w = their texel in units.
	float4 BloodLook3;
	// Trampled ground: x spare, y = packed floor detail relief scale (1 = off),
	// z = one pass's share of the layer, w spare. See TrampleDetailScale.
	float4 TrampleStyle;
	// x = tessellation target, pixels per segment (0 = distance only);
	// yz = viewport half size in pixels, w = viewport depth range.
	float4 TessScreen;
	// GetDimensions of the bound field maps (0 = unbound): x deformation (t1),
	// y berm field (t14), z object maps (t4), w exclusion field (t15).
	float4 FieldDims;
	// Snow climate at the player: x = air temperature (C), y = cooling per
	// world unit of height, z = the player's world z, w = freshness.
	float4 SnowClimate;
	// x = wind packing, y = wetness at the player, z = condition view,
	// w = 1 - Snow Climate (the fixed thin print's weight).
	float4 SnowClimate2;
	// Overrides painted everywhere, < 0 = per pixel: x wet, y sticky, z firm.
	float4 SnowClimateOverride;
	// The climate look by the pixel's temperature, rows at the player's - 2,
	// + 0 and + 2 C: xyz spare (the retired churn ramp), w = wetness x Snow
	// Climate. Read through SnowClimateRow.
	float4 SnowClimateLUT[3];
}


Texture2D<float4> TerrainWindow : register(t0);
// Drawn water's height per terrain texel, MAX of the bodies touching it,
// -100000 where none. TerrainWindow's frame, but rasterised +Y-up like the
// object maps, so its row is (TerrainDim - 1) - the terrain row.
Texture2D<float> WaterWindow : register(t27);  // data route: lone wet texels dropped
Texture2D<float> WaterRawWindow : register(t28);  // the shore cap: every rasterised body
Texture2D<uint> WaterNear : register(t19);  // squared texels to the nearest vertex under the water (WaterCoverCS)
// Water this shallow over the ground is not water: a placed pond plane's
// rectangle skims the land around its shore. Also where the shore cap's
// zero lands, so keep it small or the toe ends under the surface.
static const float kWaterSkimDepth = 2.0;
// A texel's vertex must be this far under to read as a bare class: shallower
// margins are the cap's job per pixel, and a lone shallow vertex would
// otherwise turn its whole texel into a square.
static const float kWaterTexelDepth = 8.0;

// A texel whose ground lies under drawn water reads as a non-snow class
// (-8), so the shoreline is a class border: same noise, smoothing, dither
// and rim as any texture seam.
float3 WaterBareTexel(float3 texel, int2 t)
{
	float water = WaterWindow.Load(int3(t.x, (int)TerrainDim - 1 - t.y, 0));
	texel.y = texel.x < water - kWaterTexelDepth ? min(texel.y, -8.0) : texel.y;
	return texel;
}
// TerrainWindow with WaterBareTexel already applied (TerrainCutCS; the raw
// window until the cut has run): SampleTerrain's taps.
Texture2D<float4> TerrainWindowWet : register(t58);
// Land-blended edges: 1 - the land's snow-texture weight per window texel;
// unbound reads 0, all snow.
Texture2D<float> TerrainBare : register(t26);
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
// Processed top-down object map: the SUPPRESSION mask (1 under floating
// structures; no snow beneath walkways, roofs and bridges).
Texture2D<float2> ObjectBottoms : register(t5);
// Raw object tops (persistence-scrolled) and this frame's skin-depth raster,
// shared with the trench patch: the road reads and the buried-caster test.
Texture2D<float> ObjectTopsRaw : register(t11);
// x = layer depth, y = the highest road top (kNoRoadTop where no road drew).
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
// Pre-snow copy of the MASKS target: y carries the land's snow odds
// (Lighting.hlsl WriteLandSnowOdds; 0 = no data - grass, or an object
// behind), the land's side of the contest.
Texture2D<float3> LandMasksCopy : register(t10);
// Baked berm field (BermFieldCS): x = the 17-tap disc average of the
// deformation map, y = the carve's short blur, zw = the carve's and the
// berm's dates on the ageing clock, at the map's own resolution and
// addressing.
Texture2D<float4> BermFieldMap : register(t14);
// 1 per 8x8 physical map tile where the map and the berm field read zero
// within one tile (BermFieldCS UntouchedCS). Unbound reads 0 = touched.
Texture2D<uint> UntouchedTiles : register(t57);
// Blood map + deposit clocks, the deformation map's torus (SnowFields.hlsli).
Texture2D<float4> BloodMap : register(t30);
Texture2D<float2> BloodClock : register(t31);
// Wide exclusion field (ExclusionFieldCS): x = door suppression, y = melt, over
// a window that reaches the shell's own extent. The near mask at t5 still owns
// the SHELTER term, which needs geometry and so cannot travel this far.
Texture2D<float2> ExclusionFieldMap : register(t15);

// Baked undulation field (UndulationFieldCS): x = height in world units,
// yz = its +-kUndulationGradStep shading gradient, over UndulationFieldWindow.
Texture2D<float4> UndulationFieldMap : register(t29);

SamplerState SnowSampler : register(s0);
#if defined(PSHADER) && defined(SNOW_SHELL_DEFERRED)
// The shading reads the trail map and the berm field through s0's bilinear
// (s0 wraps like DeformTexel). Its coarser sub-texel weights tilt a few rim
// normals: judged acceptable for the cost. The teeth noise reads the hash
// table through it too, and the trench detail its tiles.
#	define SNOW_DEFORM_SAMPLER
#	define SNOW_NOISE_TABLE
#endif

// Shared trench-detail shaping, spell-mark readers and field surfaces - the
// verbatim-identical pieces of both shells live in one file.
// The landscape shell paints rune glyphs and reads the blood detail tiles
// (SnowFields.hlsli).
#define SNOW_RUNE_GLYPHS
#define SNOW_BLOOD_TILES
#define SNOW_FIELD_DIMS
#include "SnowDeformation/SnowFields.hlsli"


// The game's own landscape tiling: 24 texture repeats per 4096-unit cell,
// measured in game. Same texture at the same world
// rate as the ground beside us; see CODE-NOTES.md. Mirrored in
// SnowStaticsShell.hlsl, SnowDeformation.hlsli and Shell.cpp.
static const float kSnowUVTile = 4096.0 / 24.0;

// Distance bands (WarpBand, InverseWarpAxis): ShellBandStep's spacing.
#include "SnowDeformation/SnowGrid.hlsli"

#ifndef SNOW_SURFACE_ONLY
struct VS_OUTPUT
{
	// noperspective centroid: required interpolation for SV_Position when
	// the PS writes conservative depth (SV_DepthLessEqual).
	linear noperspective centroid float4 Position : SV_POSITION;
	float4 CurrentClip : TEXCOORD0;
	float4 PreviousClip : TEXCOORD1;
	float3 WorldPos : TEXCOORD2;
	float2 GridLocal : TEXCOORD3;
	// xyz: smooth per-vertex terrain normal, w: coverage alpha (taper*fade).
	float4 TerrainNormalAlpha : TEXCOORD6;
};
#endif

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

// The vertex spacing the replay tessellates to at a grid-local point (8..128
// world units): the distance bands of SnowGrid.hlsli.
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

	float3 s00 = TerrainWindowWet.Load(int3(t0.x, t0.y, 0)).xyz;
	float3 s10 = TerrainWindowWet.Load(int3(t1.x, t0.y, 0)).xyz;
	float3 s01 = TerrainWindowWet.Load(int3(t0.x, t1.y, 0)).xyz;
	float3 s11 = TerrainWindowWet.Load(int3(t1.x, t1.y, 0)).xyz;

	float3 result = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
	// Land-exact height: the ground as the engine renders it (LandHeightFine).
	// Outside the fine window, or over missing data, the 128-texel height
	// below stands, so nothing changes at the seam.
	float fine = -100000.0;
	[branch] if (FineWindow.z > 0.5)
		fine = LandHeightFine(gridLocal);
	// Height follows the mesh's triangulation; depth and coverage stay bilinear,
	// being material blends rather than geometry. Skipped where any corner is the
	// missing-data sentinel: bilinear poisons the result so callers can detect it
	// with `< -50000`, and a max would pick the surviving triangle and hide the
	// gap instead.
	[branch] if (fine > -50000.0)
		result.x = fine;
	else if (min(min(s00.x, s10.x), min(s01.x, s11.x)) > -50000.0)
		result.x = TriangulatedHeight(s00.x, s10.x, s01.x, s11.x, f);
	// the accumulated layer scales depth HERE, at the one point
	// every reader funnels through, so geometry, shading, the berm gate and the
	// self-shadow march cannot disagree about how deep the snow is. Positive
	// only - a negative rampDepth is the submerge toward bare ground at a class
	// border, not snow, and scaling it would move class edges as it snowed.
	result.y = max(result.y, 0.0) * RimStyle.w + min(result.y, 0.0);
	return result;
}

// Holds a tap inside the loaded square. Past the seam the window carries only
// the far fill (TerrainWindowFillCS: the Terrain Shadows heightmap, BC4 over
// +-262136 units, about 2000 units a step), not the land, so taps the ground
// snow takes for its own surface stay on its side.
float2 ShellSeamClamp(float2 gridLocal)
{
	[flatten] if (SeamRampInv > 0.0)
		gridLocal = clamp(gridLocal, SeamBounds.xy - GridOrigin, SeamBounds.zw - GridOrigin);
	return gridLocal;
}

// Bilinear helper at fractional texel coordinates (Load-based).
float SampleDeformationBilinear(float2 t, float2 dims)
{
	t = clamp(t, 0.0, dims - 1.001);
#ifdef SNOW_DEFORM_SAMPLER
	return saturate(DeformationMap.SampleLevel(SnowSampler, (t + 0.5 + float2(DeformMapOrigin)) / dims, 0.0).x);
#else
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
#endif
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

	SNOW_DEFORM_DIMS(dims);
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

// SampleMelted / SampleScorch live in SnowFields.hlsli
//: both shells read the surface-state channels identically.

// Single-bilinear deformation tap: for many-tap averages (BermField) where
// the sum provides the smoothness and bicubic per tap would be waste.
float SampleDeformationFast(float2 gridLocal)
{
	float2 uv = (GridToDeformOffset + gridLocal) * DeformInvWorldSize;
	if (any(uv < 0.0) || any(uv > 1.0))
		return 0.0;

	SNOW_DEFORM_DIMS(dims);
	return SampleDeformationBilinear(uv * dims - 0.5, dims);
}

// March-grade deformation tap: one bilinear (4 loads) where the visible
// surface pays bicubic (16). The horizon march samples 28-1000 units out,
// where the samplers' difference is invisible - the same too-fine-to-matter
// rule that keeps the trench detail out of the marches.
float SampleDeformationMarch(float2 gridLocal)
{
	return SampleDeformationFast(gridLocal);
}

// Bilinear samples of the object field maps at absolute world XY. The raster
// pass maps +worldY to +ndcY = texture v0 (top), so v mirrors.
float2 ObjectMapTexel(float2 worldXY, out float2 dims, out bool valid)
{
	float2 local = (worldXY - ObjectHeightCenter) / ObjectHeightHalfExtent;
	valid = all(abs(local) < 0.98);
	dims = FieldDims.zz;
	float2 uv = float2(local.x * 0.5 + 0.5, 0.5 - local.y * 0.5);
	return clamp(uv * dims - 0.5, 0.0, dims.x - 1.001);
}

// Fully-bare test. ShellSurfaceZ's depth is rampDepth + (-8) * bare, so -8 is
// the floor: it is what ground reaches when EVERY layer under it is a non-snow
// class, and no blend can go below it. Anything above -8 is still on the ramp
// that climbs to the +14..+30 of a snow class, and culling there would cut the
// transition and leave a gap where the layer rises out of the ground - so the
// test is against the floor and not a threshold part-way up.
static const float kBareDepthFloor = -7.9;

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
			float3 t = TerrainWindowWet.Load(int3(x, y, 0)).xyz;
			// Sentinel texels carry no data; leave them to the full evaluation.
			[branch] if (t.x < -50000.0)
				return false;
			maxDepth = max(maxDepth, t.y + (-8.0) * saturate(1.0 - saturate(t.z)));
		}
	}
	return maxDepth <= kBareDepthFloor;
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

// Mirror of SnowDeformation.h kNoRoadTop.
static const float kNoRoadTop = -1000000.0;

// The road raster at a point, max of four texels like the patch: x = road
// layer depth, y = road top (kNoRoadTop where no road drew).
float2 RoadRasterAt(float2 worldXY)
{
	float2 s = float2(0.0, kNoRoadTop);
	float2 dims;
	bool valid;
	const float2 t = ObjectMapTexel(worldXY, dims, valid);
	[branch] if (valid)
	{
		const int2 t0 = (int2)t;
		const int2 t1 = min(t0 + 1, int2(dims) - 1);
		s = max(max(ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0)), ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0))),
			max(ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0)), ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0))));
	}
	return s;
}

// RoadRasterAt's footprint bilinear over its road texels alone: x = how much
// of it is road (0-1), y = the road's top, z = its layer depth (y and z where
// x > 0). Continuous where the max-of-4 steps at every texel boundary.
float3 RoadRasterBilinear(float2 worldXY)
{
	float3 r = float3(0.0, 0.0, 0.0);
	float2 dims;
	bool valid;
	const float2 t = ObjectMapTexel(worldXY, dims, valid);
	[branch] if (valid)
	{
		const int2 t0 = (int2)t;
		const int2 t1 = min(t0 + 1, int2(dims) - 1);
		const float2 f = t - t0;
		const float2 s00 = ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0));
		const float2 s10 = ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0));
		const float2 s01 = ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0));
		const float2 s11 = ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0));
		const float4 w = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y) *
		                 float4((s00.y > kNoRoadTop * 0.5 && s00.x >= 1.0) ? 1.0 : 0.0, (s10.y > kNoRoadTop * 0.5 && s10.x >= 1.0) ? 1.0 : 0.0,
							 (s01.y > kNoRoadTop * 0.5 && s01.x >= 1.0) ? 1.0 : 0.0, (s11.y > kNoRoadTop * 0.5 && s11.x >= 1.0) ? 1.0 : 0.0);
		r.x = dot(w, 1.0);
		r.y = dot(w, float4(s00.y, s10.y, s01.y, s11.y)) / max(r.x, 1e-6);
		r.z = dot(w, float4(s00.x, s10.x, s01.x, s11.x)) / max(r.x, 1e-6);
	}
	return r;
}

// The road trench patch's drawn surface over a road column (RoadRasterAt), as
// BuildPatchVertex lays it: the road's top, the carve through the road layer,
// the dunes.
float RoadPatchZ(float2 road, float deform, float blur, float und, float2 worldXY, float3 look)
{
	const float carved = CarveProfile(deform, blur, road.x, worldXY, look);
	return road.y + carved + und * saturate(carved / 8.0) - 0.4;
}

// The patch's macro slope (its normal's top term, BuildPatchVertex): 16-unit
// differences of the road's top, each side weighed by how much road its tap
// holds (RoadRasterBilinear .x), so it turns one-sided at the road's edge
// smoothly; clamped to 45 degrees.
float2 RoadTopSlope(float3 a_c, float3 a_px, float3 a_nx, float3 a_py, float3 a_ny, float step)
{
	const float2 g = float2((a_px.x * (a_px.y - a_c.y) + a_nx.x * (a_c.y - a_nx.y)) / max(a_px.x + a_nx.x, 1e-3),
						 (a_py.x * (a_py.y - a_c.y) + a_ny.x * (a_c.y - a_ny.y)) / max(a_py.x + a_ny.x, 1e-3)) /
	                 step;
	return g / max(1.0, length(g));
}

// Bilinear raw object top at world XY; sentinel where a texel is empty.
// a_skipRoads also returns it over road-owned columns (the patch's ground).
float SampleObjectTopRaw(float2 worldXY, bool a_skipRoads)
{
	float2 dims;
	bool valid;
	float2 t = ObjectMapTexel(worldXY, dims, valid);
	float top = -100000.0;
	[branch] if (valid)
	{
		int2 t0 = (int2)t;
		float2 f = t - t0;
		int2 t1 = min(t0 + 1, int2(dims) - 1);
		float4 tops = float4(
			ObjectTopsRaw.Load(int3(t0.x, t0.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t0.y, 0)),
			ObjectTopsRaw.Load(int3(t0.x, t1.y, 0)), ObjectTopsRaw.Load(int3(t1.x, t1.y, 0)));
		[branch] if (all(tops > -50000.0))
		{
			bool road = false;
			[branch] if (a_skipRoads)
				road = max(max(ObjectSkinDepthMap.Load(int3(t0.x, t0.y, 0)).y, ObjectSkinDepthMap.Load(int3(t1.x, t0.y, 0)).y),
						   max(ObjectSkinDepthMap.Load(int3(t0.x, t1.y, 0)).y, ObjectSkinDepthMap.Load(int3(t1.x, t1.y, 0)).y)) > kNoRoadTop * 0.5;
			[flatten] if (!road)
				top = lerp(lerp(tops.x, tops.y, f.x), lerp(tops.z, tops.w, f.x), f.y);
		}
	}
	return top;
}

// Where the sheet lies over a captured object: x = meeting weight (1 on the
// junction, 0 from half of ObjectMeetBand of perpendicular snow thickness
// up; each side takes half the band), yz = the object's macro slope. Both
// from the raw top raster at the pixel's own world XY: no object texture
// detail, nothing a camera can move. The 8-unit step spans whole texels,
// which cancels the MAX raster's stair. An upper deck (gap well below zero)
// and faces too steep to hold a coat weigh nothing.
float3 ShellObjectMeet(float2 worldXY, float sheetZ)
{
	float3 result = float3(0.0, 0.0, 0.0);
	float halfBand = ObjectMeetBand * 0.5;
	[branch] if (halfBand > 0.0)
	{
		float meetTop = SampleObjectTopRaw(worldXY, true);
		float meetGap = sheetZ - meetTop;
		[branch] if (meetTop > -50000.0 && meetGap < halfBand * 3.0 && meetGap > -16.0)
		{
			const float mStep = 8.0;
			float mXP = SampleObjectTopRaw(worldXY + float2(mStep, 0.0), false);
			float mXN = SampleObjectTopRaw(worldXY - float2(mStep, 0.0), false);
			float mYP = SampleObjectTopRaw(worldXY + float2(0.0, mStep), false);
			float mYN = SampleObjectTopRaw(worldXY - float2(0.0, mStep), false);
			[flatten] if (min(min(mXP, mXN), min(mYP, mYN)) > -50000.0)
			{
				float2 objectGrad = float2(mXP - mXN, mYP - mYN) / (2.0 * mStep);
				float objectNz = rsqrt(1.0 + dot(objectGrad, objectGrad));
				float meet = (1.0 - smoothstep(0.0, halfBand, meetGap * objectNz)) *
				             (1.0 - smoothstep(8.0, 16.0, -meetGap)) * smoothstep(0.35, 0.6, objectNz);
				result = float3(meet, objectGrad);
			}
		}
	}
	return result;
}

// ---- Surface undulation: wind-settled dunes ----
// Two octaves of world-anchored value noise, added as real geometry via
// ShellSurfaceZ and shaded through its gradient. Amplitude scales with local
// depth, so thin snow, class boundaries and carved floors stay flat.
// Undulation, CarveProfile and kFireMeltFloor live in SnowFields.hlsli, so the
// surface, the shading gradient and both shells' self-shadow marches see one
// shape. Trenches carve to the ground.

// Edge berm: displaced snow piles along the trench rim, taller from a deeper
// layer. Shape comes from the BLURRED deformation (BermField) - two rings
// reaching ~40 units (x BermReach) past the trail edge, the outer ring's small per-tap
// weight giving a long tail instead of a knife along the stamp falloff.
// BermShape and the tap ring are in SnowFields.hlsli.

// The baked berm field; the bake exists wherever the shells draw (it is
// created with the map). x = berm, y = the carve's short blur, zw = the
// carve's and the berm's dates.
float4 BermFieldPair(float2 gridLocal)
{
	return BermFieldBaked(gridLocal);
}

float BermField(float2 gridLocal)
{
	return BermFieldPair(gridLocal).x;
}

// Water level over the texels a point touches, from the raw raster: the max,
// so a shore texel answers with its body's level and the sentinel never
// blends in. .y = bilinear presence of the four texels, so the cap can fade
// across the raster's last texel instead of stopping on its edge.
float2 SampleWaterHeight(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	int r0 = (int)TerrainDim - 1 - t0.y;
	int r1 = (int)TerrainDim - 1 - t1.y;
	float w00 = WaterRawWindow.Load(int3(t0.x, r0, 0));
	float w10 = WaterRawWindow.Load(int3(t1.x, r0, 0));
	float w01 = WaterRawWindow.Load(int3(t0.x, r1, 0));
	float w11 = WaterRawWindow.Load(int3(t1.x, r1, 0));
	float4 present = step(-50000.0, float4(w00, w10, w01, w11));
	return float2(max(max(w00, w10), max(w01, w11)), lerp(lerp(present.x, present.y, f.x), lerp(present.z, present.w, f.x), f.y));
}

// One near-field texel in texels, capped at WaterCoverCS's kWaterNearMax.
float WaterNearTexels(int3 a_t)
{
	const uint sq = WaterNear.Load(a_t);
	return sq > 9u ? 3.0 : sqrt((float)sq);
}

// Texels from a point to the nearest vertex under the water, bilinear over
// the near field (rows flipped like the water window).
float SampleWaterNear(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	int r0 = (int)TerrainDim - 1 - t0.y;
	int r1 = (int)TerrainDim - 1 - t1.y;
	float n00 = WaterNearTexels(int3(t0.x, r0, 0));
	float n10 = WaterNearTexels(int3(t1.x, r0, 0));
	float n01 = WaterNearTexels(int3(t0.x, r1, 0));
	float n11 = WaterNearTexels(int3(t1.x, r1, 0));
	return lerp(lerp(n00, n10, f.x), lerp(n01, n11, f.x), f.y);
}

// Touch-down toe: positive depth eases to the ground below 9 units, tangent
// at 3, so a thinning layer lands ON the ground instead of ending 1-2 units
// up as a lip. Applied to geometry, the PS effective depth and the shading
// depth alike.
float TouchDownToe(float depth)
{
	return depth > 0.0 ? depth * smoothstep(3.0, 9.0, depth) : depth;
}

// min(a, b) through a quadratic knee of radius k, so a cap meeting a ramp
// leaves no crease.
float KneeMin(float a, float b, float k)
{
	float e = a - b;
	float knee = e > k ? e : (e < -k ? 0.0 : (e + k) * (e + k) / (4.0 * k));
	return a - knee;
}

// Bilinear land height from the 128-texel window, -1e5 when a corner is
// unbaked.
float TerrainHeightAt(float2 gridLocal)
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
	[flatten] if (min(min(s00, s10), min(s01, s11)) <= -50000.0)
		return -100000.0;
	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

// Land-blended edges: on mixed land the landscape's own height blend decides
// snow or ground (Lighting WriteLandSnowOdds), the snow lies as a film where
// the land has any snow weight, and trenches reach the ground.
// LANDSCAPE-SNOW-PLAN Phase 5.
static const float kLandFilm = 0.5;
// Remaining depth under which the snow is a film: the land's own verdict
// decides it pixel by pixel, objects included.
static const float kLandFilmDepth = 1.0;
// Thinner snow on a fire or shock mark is left to the game's draws, so the
// ground and its scorch exist under it for the decal overlay to show
// through. Past SnowBloodCapture.hlsl kDecalHiddenOver: the overlay's height,
// measured over the real ground, runs down to ~0.75 of this remaining depth.
static const float kDecalShowDepth = 14.0;

// The land's snow-texture weight, bilinear like TerrainHeightAt.
float LandSnowWeight(float2 gridLocal)
{
	float2 t = (GridToTerrainOffset + gridLocal) / TerrainTexelSize;
	t = clamp(t, 0.0, (float)(TerrainDim - 1) - 0.001);
	int2 t0 = (int2)t;
	float2 f = t - t0;
	int2 t1 = min(t0 + 1, int2(TerrainDim - 1, TerrainDim - 1));
	float b00 = TerrainBare.Load(int3(t0.x, t0.y, 0));
	float b10 = TerrainBare.Load(int3(t1.x, t0.y, 0));
	float b01 = TerrainBare.Load(int3(t0.x, t1.y, 0));
	float b11 = TerrainBare.Load(int3(t1.x, t1.y, 0));
	return 1.0 - lerp(lerp(b00, b10, f.x), lerp(b01, b11, f.x), f.y);
}

// No land snow weight on any texel a bilinear tap over [lo, hi] can reach
// (ShellTerrainAllBare's texel range).
bool LandRectNoSnow(float2 lo, float2 hi)
{
	float2 tLo = (GridToTerrainOffset + lo) / TerrainTexelSize;
	float2 tHi = (GridToTerrainOffset + hi) / TerrainTexelSize;
	float maxTexel = (float)(TerrainDim - 1);
	int2 i0 = (int2)clamp(floor(tLo), 0.0, maxTexel);
	int2 i1 = (int2)clamp(floor(tHi) + 1.0, 0.0, maxTexel);
	i1 = min(i1, i0 + 3);
	float bare = 1.0;
	[loop] for (int y = i0.y; y <= i1.y; ++y)
	{
		[loop] for (int x = i0.x; x <= i1.x; ++x)
			bare = min(bare, TerrainBare.Load(int3(x, y, 0)));
	}
	return bare >= 0.998;
}

// The land's snow odds in Masks.y; -16 where something else drew there (an
// object's base, grass), so thin snow never claims it.
float LandSnowOdds(float masksY)
{
	return masksY > 0.002 && masksY < 1.5 ? saturate((masksY - 0.004) * (1.0 / 0.996)) * 32.0 - 16.0 : -16.0;
}

// The snow's side of the contest: remaining depth buries the land's grain
// (CompactLook.y per unit), a carve pushes thin snow away (CompactLook.z).
float LandSnowSide(float remaining, float carve)
{
	return CompactLook.y * remaining - CompactLook.z * smoothstep(0.3, 1.0, carve);
}

// Rise above the waterline over which the cap runs the class ramp's 38
// units (-8 to +30): the snow is full about 8 units above the water on any
// bank. Keyed on height alone so it is monotone away from the water; a
// horizontal-distance estimate (rise / local slope) fell again wherever a
// bank steepened inland, a trough on a straight texel edge.
static const float kShoreRise = 10.0;

// The snow ends AT the waterline with the class border's own depth ramp.
// The wet texels (-8) give the shore a class ramp, but that ramp is anchored
// to the land lattice, so its zero can fall either side of the line; this
// caps the depth by rise above the line from the 128-texel bilinear alone
// (the same from every range), zero at the line, negative past it. The cap
// is the larger of that and a horizontal term, the class ramp run from one
// texel past the nearest vertex under the water: ground two texels from
// the line is free however little it stands above the level (a cave floor
// 5 units over a lake plane was thinned everywhere, in heightmap-quantised
// rectangles), and both terms only grow away from the water. Rounded knee
// where the cap meets the class ramp. Once per consumer.
float3 EndSnowAtWater(float3 terrain, float2 gridLocal)
{
	[branch] if (terrain.x > -50000.0 && terrain.y > -7.9)
	{
		float2 waterAt = SampleWaterHeight(gridLocal);
		float water = waterAt.x;
		float waterWeight = waterAt.y;
		[branch] if (waterWeight > 0.001)
		{
			float h = TerrainHeightAt(gridLocal);
			[branch] if (h > -50000.0)
			{
				float rise = h - water + kWaterSkimDepth;
				float capRise = rise * (38.0 / kShoreRise);
				float capNear = (SampleWaterNear(gridLocal) - 1.0) * 38.0;
				// Past the raster's last texel the cap lifts clear of the ramp.
				float cap = max(capRise, capNear) + (1.0 - waterWeight) * 60.0;
				terrain.y = max(KneeMin(terrain.y, cap, 4.0), -8.0);
			}
		}
	}
	return terrain;
}


// The shell surface: per-texture-class snow depth carved by deformation.
// Class depths blend by their baked weights on the CPU (window rebuild),
// so boundaries between differently-deep snows are geometric depth ramps;
// negative depths (roads) submerge the shell below the surface.
// Shared by the geometry and the prepass so both agree.

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
		float3 terrain = SampleTerrain(gridLocal);
		terrain = EndSnowAtWater(terrain, gridLocal);

		terrainHeight = terrain.x;
		float rampDepth = terrain.y;
		coverage = saturate(terrain.z);

		// The mask carries two independent channels. x = coverage suppression,
		// smooth 0-1, so doorstep clearings fade at their edges instead of
		// cutting. y = melt fraction: depth thins toward kFireMeltFloor
		// (coverage untouched), so melted ground keeps a thin snow floor
		// instead of fading to bare ground.
		float2 shelterMask = SampleExclusionMask(GridOrigin + gridLocal);

		coverage *= saturate(1.0 - shelterMask.x);

		// Bare ground contributes negative depth so the shell submerges toward
		// uncovered terrain as well; edgeFade (computed above) lays the shell
		// down onto the ground across the seam ramp.
		float bare = saturate(1.0 - coverage);
		float depth = rampDepth + (-8.0) * bare;
		depth = ShellEdgeDepth(depth, edgeFade);

		// Deep trench floors pass unchanged; a floor set below ~9 (a low
		// Trench Floor fraction) compresses with the toe.
		depth = TouchDownToe(depth);
		// Melt AFTER the toe: the floor stands kFireMeltFloor over the
		// ground. Melted first, the toe took it to exactly 0 and the sheet
		// fought the land for every pixel of the basin.
		depth = lerp(depth, min(depth, kFireMeltFloor), saturate(shelterMask.y));

		// Carves only where the layer is raised; the negative-depth submerge at
		// class edges is untouched. Undulation rides on top, scaled by
		// remaining depth.
		[flatten] if (depth > 0.0)
		{
			float deformation = saturate(SampleDeformation(gridLocal));
			float4 bermPair = BermFieldPair(gridLocal);
			float bermD = bermPair.x;
			float uncarved = depth;
			// The berm is spoil piled on snow that was NOT dug. Unmasked, the
			// blurred field also lifted the trench floor and walls - a narrow
			// trail's disc average is well above zero at its own centre, so
			// raising Berm Height raised the whole trench with it.
			depth = CarveProfile(deformation, bermPair.y, uncarved, GridOrigin + gridLocal, TrackCarveLook(TrackAgeOf(bermPair.z))) +
			        BermShape(bermD) * saturate(1.0 - deformation) * uncarved * BermHeightAmp * BermDepthGate(uncarved) * TrackBermScale(TrackAgeOf(bermPair.w));
			depth += UndulationSampled(GridOrigin + gridLocal) * saturate(depth / 8.0);
		}

		surfaceZ = terrainHeight + depth;
	}
	return surfaceZ;
}

// The vertex normal's two height differences: wide 32-unit differences bridge
// the 128-unit data texels, and interpolation removes the faceting of the
// per-pixel piecewise-constant gradient. At the seam a tap stops on it and
// the one-sided difference is scaled to the 64-unit span.
float2 ShellVertexSlope(float2 gridLocal)
{
	const float2 c = ShellSeamClamp(gridLocal);
	const float2 p = ShellSeamClamp(c + 32.0);
	const float2 n = ShellSeamClamp(c - 32.0);
	float hxp = SampleTerrain(float2(p.x, c.y)).x;
	float hxn = SampleTerrain(float2(n.x, c.y)).x;
	float hyp = SampleTerrain(float2(c.x, p.y)).x;
	float hyn = SampleTerrain(float2(c.x, n.y)).x;
	return float2(hxp - hxn, hyp - hyn) * (64.0 / max(p - n, 1.0));
}

// Shared with the raised landscape (LandscapeSnow.hlsl). Macros: as called
// functions fxc schedules the shell's vertex stages differently.
#define SHELL_TERRAIN_NORMAL(slopeX, slopeY) normalize(float3(-(slopeX) / 64.0, -(slopeY) / 64.0, 1.0))

// Slope drape (Josef's sketch, 2026-09-05): on steep ground the layer
// stands off along the surface normal instead of straight up, so a
// near-vertical face keeps the full depth of cover in front of it. A
// vertical-only offset thins to depth*cos(slope) and the face shows
// through. The foot stays on the lattice; only the output position moves,
// so every sample still reads the vertex's own ground. footXY is the
// foot in position's frame (absolute world for the shell).
#define SHELL_DRAPE(position, footXY, z, terrainHeight, terrainNormal)                                      \
	[branch] if (SlopeDrape.x > 0.001 && (z) > (terrainHeight))                                             \
	{                                                                                                       \
		float drapeAbove = (z) - (terrainHeight);                                                           \
		float drapeTilt = SlopeDrape.x * (1.0 - smoothstep(SlopeDrape.y, SlopeDrape.z, (terrainNormal).z)); \
		float3 drapeDir = normalize(lerp(float3(0.0, 0.0, 1.0), (terrainNormal), drapeTilt));               \
		position = float3((footXY), (terrainHeight)) + drapeDir * drapeAbove;                               \
	}

// The raised landscape's sun-cascade visibility (LandscapeSnow.hlsl); below
// 0.35 nothing casts. Its terms:
// - The far anti-pinhole pass dilates coverage AND height (4-tap max + ridge
//   pad), so beyond ~3000 units a ring around every patch passes both castVis
//   terms while the main view discards those pixels per-texel — and this
//   depth-only caster has no PS to do the same. Gating coverage on the
//   UNDILATED texel under the vertex keeps true snowfields casting and
//   silences the dilation skirt over bare coast.
// - Melt pits do not cast: snow the melt removed casts nothing, and the pit
//   dissolves at texture resolution while the caster rim collapses at vertex
//   resolution, printing a blocky shadow on the revealed ground. Stamped
//   (spell) melt only - static clearings are the exclusion field's. Keep the
//   thresholds well above the residue floor, or decaying .y lands on the
//   castVis NaN cliff and caster triangles flip once a second.
// - The far field keeps printing blotches from residual data/geometry
//   mismatches the near gates cannot see, and the shell's crisp shadows only
//   matter near the camera anyway — the far field is diffuse-dominated and
//   terrain shadows carry the rest. Fade the caster out entirely over ~40-70 m.
#define SHELL_CAST_VIS(castVis, gridLocal, z, coverage)                                                                               \
	float3 rawTerrainCast = SampleTerrain(gridLocal);                                                                                 \
	float castVis = smoothstep(2.0, 5.0, (z) - rawTerrainCast.x) * smoothstep(0.2, 0.5, min((coverage), saturate(rawTerrainCast.z))); \
	castVis *= 1.0 - smoothstep(0.35, 0.6, SampleMelted(gridLocal));                                                                  \
	castVis *= 1.0 - smoothstep(2800.0, 4900.0, length((gridLocal) - WarpedHalfSpan));

#ifndef SNOW_SURFACE_ONLY
// The replay domain shader's vertex tail: terrain normal, slope drape,
// camera-relative transform and the few outputs the prepass reads. The two
// height differences arrive as separate scalars: as a float2 the compiler
// vectorises the normalize below, the same arithmetic but not the same
// bytecode.
VS_OUTPUT FinishShellVertexSloped(float2 gridLocal, float z, float coverage, float terrainHeight, float slopeX, float slopeY)
{
	float3 terrainNormal = SHELL_TERRAIN_NORMAL(slopeX, slopeY);

	float3 absolutePos = float3(GridOrigin + gridLocal, z);
	SHELL_DRAPE(absolutePos, GridOrigin + gridLocal, z, terrainHeight, terrainNormal)

	// Shader world space is camera-relative.
	float3 rel = absolutePos - ShellCameraPosAdjust.xyz;

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.WorldPos = rel;
	vsout.GridLocal = gridLocal;
	// The prepass PS reads CurrentClip.w and the normal's xyz of the rest.
	vsout.CurrentClip = float4(0.0, 0.0, 0.0, mul(CameraViewProjUnjittered, float4(rel, 1.0)).w);
	vsout.PreviousClip = 0.0;
	vsout.TerrainNormalAlpha = float4(terrainNormal, 0.0);
	return vsout;
}

VS_OUTPUT FinishShellVertex(float2 gridLocal, float z, float coverage, float terrainHeight)
{
	const float2 slope = ShellVertexSlope(gridLocal);
	return FinishShellVertexSloped(gridLocal, z, coverage, terrainHeight, slope.x, slope.y);
}
#endif

#if defined(HULLSHADER) || defined(SNOW_SURFACE_ONLY)
// Detail reach (LandscapeSnowTess.hlsli LsDetailFactor): full kTessMax within
// kTessNear/kTessMax units, factor 1 by kTessNear. Deformed edges stretch the
// reach by kTessReachBoost; past the BOOSTED reach the factor clamps to 1 for
// every deformation value, so the map taps cannot change the answer.
static const float kTessNear = 1600.0;
static const float kTessMax = 8.0;
static const float kTessReachBoost = 3.0;
static const float kTessCutoff = kTessNear * kTessReachBoost;
#endif

#include "SnowDeformation/SnowParallax.hlsli"

#if defined(DOMAINSHADER) || defined(SNOW_SURFACE_ONLY)
// The vertex surface: ShellSurfaceZ plus the skirt descent and the
// displacement relief. One function for the replay's domain shader and the
// raised landscape, so both stand on the same bits.
float ShellVertexZFrom(float2 gridLocal, float z, float coverage, float terrainHeight)
{
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

float ShellVertexZ(float2 gridLocal, out float coverage, out float terrainHeight)
{
	float z = ShellSurfaceZ(gridLocal, coverage, terrainHeight);
	return ShellVertexZFrom(gridLocal, z, coverage, terrainHeight);
}
#endif  // DOMAINSHADER || SNOW_SURFACE_ONLY



#ifdef PSHADER

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
};

// Depth prepass (SNOW_SHELL_DEPTH_PREPASS): the alpha cut and the export
// clamp, nothing else. DepthLE is the shell's real depth (clamped); the
// colour target receives the RASTER depth of every fragment that wins, which
// the fill pass turns into the shading pass's EQUAL test buffer - a hardware
// EQUAL against the clamped depth would drop every clamped pixel.
struct PS_PREPASS_OUTPUT
{
	float RasterDepth : SV_Target0;
#	ifdef SNOW_PREPASS_ATTR
	// Deferred shading's inputs for each winning pixel: GridLocal and the
	// terrain normal's xy; GridLocal's coarse derivatives; the view depth and
	// WorldPos.z's coarse derivatives.
	float4 AttrGrid : SV_Target1;
	float4 AttrGridDeriv : SV_Target2;
	float4 AttrDepth : SV_Target3;
#		ifdef SNOW_PREPASS_EARLY
	// A pending pixel's inputs for the resolve: y = the snow's side of the land
	// contest; w = 2 if the export clamped + 8 if the land contest runs + 32
	// if a static under the snow draws its own pixel.
	float4 Pending : SV_Target4;
#		endif
#	endif
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
//
// A projected-snow static (Masks.y >= 2; land keeps (0, 1]) is an occluder at
// any height: its foot stands inside the slack, and a sheet clamped over it
// reads the cascades from inside the object - a dark rim past 4000 units.
float ShellSceneAboveSnowLine(float3 worldPosRel, float sceneZ, float shellZ, float2 pixel)
{
	float3 sceneRel = worldPosRel * (sceneZ / max(shellZ, 1e-3));
	float2 sceneLocal = sceneRel.xy + ShellCameraPosAdjust.xy - GridOrigin;
	float2 tt = (GridToTerrainOffset + sceneLocal) / TerrainTexelSize;
	bool inWindow = all(tt >= 0.0) && all(tt <= (float)(TerrainDim - 1));
	float3 st = SampleTerrain(sceneLocal);
	float above = (sceneRel.z + ShellCameraPosAdjust.z) - (st.x + max(st.y, 0.0));
	above = (inWindow && st.x > -50000.0) ? above : 0.0;
#ifdef SNOW_PREPASS_EARLY
	// No land masks yet: a clamp a static could cancel is left to the resolve.
	return above;
#else
	bool sceneIsStatic = LandMasksCopy.Load(int3(pixel, 0)).y >= 1.5;
	return sceneIsStatic ? 1e6 : above;
#endif
}

// View depth of a value in the scene's depth buffer. GetScreenDepth assumes a
// depth range of 1; the main pass draws to 0.999998 (TessScreen.w), which read
// as range 1 puts a surface 0.9 units near at view depth 2600.
float ShellSceneViewDepth(float a_rawDepth)
{
	return SharedData::GetScreenDepth(a_rawDepth / (TessScreen.w > 0.5 ? TessScreen.w : 1.0));
}

bool ShellFarClampWindow(float shellZ, float sceneZ)
{
	float clampWindow = min(8.0 + shellZ * 0.008, 48.0);
	return shellZ > 4000.0 && shellZ > sceneZ && shellZ - sceneZ < clampWindow;
}

bool ShellNearClampWindow(float shellZ, float sceneZ)
{
	return shellZ > sceneZ && shellZ - sceneZ < 0.75;
}

float ShellExportDepth(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ, float pixelEffDepth, float pixelCarve, float sceneAbove)
{
	float depth = rasterZ;
	bool clampMode = sceneAbove < kShellClampOccluderSlack;
	[branch] if (clampMode && ShellFarClampWindow(shellZ, sceneZ))
		depth = min(rasterZ, rawSceneDepth - 1e-5);
	else if (clampMode && (pixelEffDepth < 4.0 || pixelCarve > 0.5) && ShellNearClampWindow(shellZ, sceneZ))
		depth = min(rasterZ, rawSceneDepth - 1e-5);
	return depth;
}

// Out of both windows the export is rasterZ whatever sceneAbove says.
float ShellExportDepthAt(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ, float pixelEffDepth, float pixelCarve, float3 worldPosRel, float2 pixel)
{
	float sceneAbove = 0.0;
	[branch] if (ShellFarClampWindow(shellZ, sceneZ) || ShellNearClampWindow(shellZ, sceneZ))
		sceneAbove = ShellSceneAboveSnowLine(worldPosRel, sceneZ, shellZ, pixel);
	return ShellExportDepth(rasterZ, rawSceneDepth, shellZ, sceneZ, pixelEffDepth, pixelCarve, sceneAbove);
}

// Out of both windows the export is the raster depth; behind the scene (the
// blended depth is the main depth to within rounding) it fails LESS_EQUAL.
bool ShellPrepassHidden(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ)
{
	return !ShellFarClampWindow(shellZ, sceneZ) && !ShellNearClampWindow(shellZ, sceneZ) && rasterZ > rawSceneDepth + 1e-6;
}

// Every lane of the quad is hidden, so the exit is quad-uniform and the
// derivatives fxc sinks below it stay valid.
bool ShellPrepassQuadHidden(float rasterZ, float rawSceneDepth, float shellZ, float sceneZ)
{
	float hidden = ShellPrepassHidden(rasterZ, rawSceneDepth, shellZ, sceneZ) ? 1.0 : 0.0;
	float hx = ddx_fine(hidden);
	return hidden > 0.5 && hx == 0.0 && ddy_fine(hidden) == 0.0 && ddy_fine(hx) == 0.0;
}

#ifdef SNOW_SHELL_DEPTH_PREPASS
// Terrain Blending's mask: the main depth after the depth prepass, land within
// its 1024-unit reach pushed back, objects at their own depth. Null without it.
Texture2D<float> TBMaskDepth : register(t110);
static const float kTBMaskReach = 1000.0;

// Masks here is not the land's own write: an object is the surface (the mask
// holds it at the scene depth), or a surface stands in front of the snow
// inside the near clamp window.
bool ShellMasksNotLand(float3 worldPosRel, float2 pixel, float shellZ, float sceneZ)
{
	bool notLand = !ShellFarClampWindow(shellZ, sceneZ) && ShellNearClampWindow(shellZ, sceneZ);
	[branch] if (!notLand && dot(worldPosRel, worldPosRel) < kTBMaskReach * kTBMaskReach)
	{
		const float mask = TBMaskDepth.Load(int3(pixel, 0));
		notLand = mask > 0.0 && ShellSceneViewDepth(mask) - sceneZ < 0.05;
	}
	return notLand;
}
#endif

#if defined(SNOW_SHELL_DEFERRED)
// LLF cluster lookup coordinates for a camera-relative position.
void ShellClusterCoords(float3 relPos, out float viewZ, out float2 clusterUV)
{
	viewZ = mul(CameraView, float4(relPos, 1.0)).z;
	float4 clip = mul(CameraViewProj, float4(relPos, 1.0));
	clusterUV = clip.xy / max(clip.w, 1e-4) * float2(0.5, -0.5) + 0.5;
}

// Detail grain from the diffuse's luminance, for snow sets without a normal map.
float3 ShellLuminanceBump(float3 normalWS, float2 worldXY, float bumpFade)
{
	const float kBumpTile = 64.0;
	const float kBumpHeight = 0.55;
	float2 texDims;
	SnowDiffuse.GetDimensions(texDims.x, texDims.y);
	float e = 1.5 / texDims.x;
	float2 detailUV = worldXY / kBumpTile;
	const float3 kLum = float3(0.30, 0.45, 0.25);
	const float2 detailDx = gDeferredGridDx / kBumpTile;
	const float2 detailDy = gDeferredGridDy / kBumpTile;
	float h0 = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV, detailDx, detailDy).rgb, kLum);
	float hx = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(e, 0.0), detailDx, detailDy).rgb, kLum);
	float hy = dot(SnowDiffuse.SampleGrad(SnowSampler, detailUV + float2(0.0, e), detailDx, detailDy).rgb, kLum);
	float2 bumpGrad = float2(hx - h0, hy - h0) * (kBumpHeight / (e * kBumpTile));
	return normalize(normalWS + float3(-bumpGrad * bumpFade, 0.0));
}
#endif

#if defined(SNOW_S1_RESOLVE)
// S1 resolve (DrawShell, before the deferred pass): finishes the early
// prepass's pending pixels with what the game's opaque draws left - the land's
// verdict, the clamp against a static - and writes every winner's depth into
// the main buffer under LESS_EQUAL, so a pixel something drew in front of
// since the claim loses. The marks are DepthSync's and the private fill's.
Texture2D<float> EarlyRasterDepth : register(t61);
Texture2D<float2> EarlyPending : register(t62);  // x landSide, y the flags x2

struct S1_RESOLVE_OUTPUT
{
	float Marks : SV_Target0;
	float Depth : SV_Depth;
};

S1_RESOLVE_OUTPUT main(float4 pos : SV_Position)
{
	const int3 px = int3(pos.xy, 0);
	const float raster = EarlyRasterDepth.Load(px);
	if (raster == 0.0)
		discard;
	float depth = abs(raster);
	[branch] if (raster < 0.0)
	{
		const float2 pending = EarlyPending.Load(px);
		const uint flags = (uint)(pending.y * 0.5);
		const float hLandRaw = LandMasksCopy.Load(px).y;
		// The land's own verdict against the snow's side.
		[branch] if ((flags & 4) != 0)
		{
			if (LandSnowOdds(hLandRaw) + pending.x < 0.0)
				discard;
		}
		if ((flags & 1) != 0 && hLandRaw < 1.5)
			depth = min(depth, SceneDepth.Load(px) - 1e-5);
	}
	S1_RESOLVE_OUTPUT o;
	o.Marks = abs(raster);
	o.Depth = depth;
	return o;
}
#else

#if defined(SNOW_SHELL_DEFERRED)
PS_OUTPUT ShellShade(VS_OUTPUT input)
#else
PS_PREPASS_OUTPUT main(VS_OUTPUT input)
#endif
{
	// Same convention as MotionBlur::GetSSMotionVector.
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	float2 gridLocal = input.GridLocal;
	// Depth reads shared by the hidden-fragment exit, the proximity fade and
	// the conservative depth clamp at the tail.
	float rawSceneDepth = SceneDepth.Load(int3(input.Position.xy, 0));
	float sceneZ = ShellSceneViewDepth(rawSceneDepth);
	float shellZ = input.CurrentClip.w;
	// The prepass's only derivatives.
#ifdef SNOW_PREPASS_ATTR
	const float4 attrGridDeriv = float4(ddx_coarse(input.GridLocal), ddy_coarse(input.GridLocal));
	const float2 attrZDeriv = float2(ddx_coarse(input.WorldPos.z), ddy_coarse(input.WorldPos.z));
#endif
#if defined(SNOW_SHELL_DEPTH_PREPASS) && !defined(SNOW_PREPASS_KEEP_HIDDEN)
	// The depth export denies early-Z; a hidden quad skips the alpha cut.
	[branch] if (ShellPrepassQuadHidden(input.Position.z, rawSceneDepth, shellZ, sceneZ))
	{
		discard;
		return (PS_PREPASS_OUTPUT)0;
	}
#endif

	// The terrain field per PIXEL: the ramp the deferred attributes carry and
	// the inputs of the cut below.
#ifdef SNOW_SHELL_DEFERRED
	// The prepass computed the ramp from the same GridLocal bits; with the
	// cut trusted, nothing else of this block reaches the outputs.
	float pixelRampDepth = gDeferredRampDepth;
	float3 pixelTerrain = 0.0;
	float pixelCoverage = 1.0;
	float pixelClassDepth = 0.0;
#else
	float3 pixelTerrain = EndSnowAtWater(SampleTerrain(gridLocal), gridLocal);
	float pixelCoverage = saturate(pixelTerrain.z);
	float psEdgeFade = ShellEdgeFade(gridLocal);

	// Un-carved class depth ramp; negative means the snow is submerged.
	float pixelClassDepth = pixelTerrain.y;
	float pixelBare = saturate(1.0 - pixelCoverage);
	float pixelRampDepth = pixelClassDepth + (-8.0) * pixelBare;
	pixelRampDepth = ShellEdgeDepth(pixelRampDepth, psEdgeFade);
#endif

	// Mirror of the geometry's touch-down toe, so the cut reads the sheet's
	// ACTUAL height above ground.
	float pixelEffDepth = TouchDownToe(pixelRampDepth);
#ifdef SNOW_SHELL_DEFERRED
	float coverageAlpha = 1.0;
#else
	// Wherever the land has snow weight the pixel is a candidate; on mixed
	// land the land's own verdict decides it below.
	const float pixelSnowWeight = LandSnowWeight(gridLocal);
	float coverageAlpha = pixelSnowWeight > 0.002 ? 1.0 : 0.0;
#endif
	// Josef's border spec: the alpha may not cut geometry still standing
	// above its ground - the sheet dives below the terrain (the descent's
	// undershoot) and the depth test against the terrain IS the border.
	// Keyed on the RENDERED height above the terrain data, not the ramp. The
	// overrides and the land contest below still multiply.
	float sheetAboveGround = smoothstep(0.25, 1.5, input.WorldPos.z + ShellCameraPosAdjust.z - pixelTerrain.x);
	coverageAlpha = max(coverageAlpha, sheetAboveGround);

	// Object blending (Terrain Blending-style depth proximity): the shell only
	// knows terrain heights, so this is what makes it meet statics softly
	// instead of slicing across them at the depth test. The gap is measured
	// along the view ray and shifts with the camera; mild distance scaling
	// turns that wobble into a soft fade. Keep the base short and the scaling
	// mild - a long or view-Z-proportional band reads as a translucent margin.
	float objectFadeBand = 5.0 + shellZ * 0.004;
	float proximityFade = saturate((sceneZ - shellZ) / objectFadeBand);
	// The fade softens where the shell CUTS THROUGH something standing above
	// the snow; a backdrop lying below the snow surface is buried and the shell
	// over it is opaque. Reconstruct the scene surface's world height along
	// the ray and compare it with the shell's own surface here, not with the
	// terrain: a rock top a few units under the shell is under the snow, and
	// measured against the terrain it read as an object and dithered into a
	// tint that never occluded it. Objects standing proud keep the soft
	// contact; the rim's descent onto its own terrain is below the shell too.
	[branch] if (shellZ < 8192.0)
	{
		float sceneSurfaceZ = ShellCameraPosAdjust.z + input.WorldPos.z * (sceneZ / max(shellZ, 1e-3));
		float shellSurfaceZ = ShellCameraPosAdjust.z + input.WorldPos.z;
		float objectness = smoothstep(-1.0, 2.0, sceneSurfaceZ - shellSurfaceZ);
		proximityFade = max(proximityFade, 1.0 - objectness);
	}
	// Carved trench floors hug the geometry behind them and must override the
	// fade, or they dither away view-dependently.
	const bool pixelUntouched = ShellUntouched(gridLocal);
	float pixelCarve = 0.0;
	[branch] if (!pixelUntouched)
		pixelCarve = saturate(SampleDeformation(gridLocal));
	float2 pixelShelter = SampleExclusionMask(GridOrigin + gridLocal);
	// Fire-melted floors hug the terrain BY DESIGN (kFireMeltFloor above it);
	// without an override the proximity fade dithers them into translucency
	// like any other near-coincident surface.
	float pixelMelt = saturate(pixelShelter.y);
	// Trampled Border Fade retired: its default-0 behavior is the
	// keeper, so the window is the constant it resolved to.
	float carveOverride = smoothstep(0.1, 0.5, pixelCarve) * smoothstep(0.5, 1.0, pixelTerrain.y);
	// Land-blended edges: a film is the land's own snow, so the land's verdict,
	// not the view-dependent proximity fade, decides it next to objects.
	bool landFilm = false;
	float landRemaining = 0.0;
	// Bodies pack the snow down but never part it: against the land it keeps
	// the depth only melt took from it, and only melt pushes it aside.
	float landBurial = 0.0;
	float landMelt = 0.0;
#ifndef SNOW_SHELL_DEFERRED
	[branch] if (coverageAlpha > 0.001)
	{
		landRemaining = CarveProfile(pixelCarve, pixelCarve, max(pixelEffDepth, 0.0), GridOrigin + gridLocal, TrackCarveLook(0.0));
		landFilm = landRemaining < kLandFilmDepth;
		landMelt = min(SampleMelted(gridLocal), pixelCarve);
		landBurial = landMelt > 0.0 ? CarveProfile(landMelt, landMelt, max(pixelEffDepth, 0.0), GridOrigin + gridLocal, TrackCarveLook(0.0)) : max(pixelEffDepth, 0.0);
	}
#endif
	coverageAlpha *= landFilm ? 1.0 : max(proximityFade, saturate(carveOverride + smoothstep(0.1, 0.4, pixelMelt)));

#ifndef SNOW_SHELL_DEFERRED
#	ifdef SNOW_SHELL_DEPTH_PREPASS
	// The land's verdict is read from Masks only where the land wrote it: an
	// object's code there (skin's 1, a blended pass diluting 2) decodes as odds.
	// Snow whose side wins any verdict skips the contest and buries objects.
	[branch] if (coverageAlpha > 0.001 && (landFilm || pixelSnowWeight < 0.998) && LandSnowSide(landRemaining, pixelCarve) < 16.0)
	{
		[flatten] if (ShellMasksNotLand(input.WorldPos, input.Position.xy, shellZ, sceneZ))
			coverageAlpha = 0.0;
	}
#	endif
#	ifdef SNOW_PREPASS_EARLY
	// The land's verdict is drawn after this pass: the snow's side is recorded
	// and the resolve runs the contest. It only lowers the alpha, so the cut
	// below on the uncontested alpha discards nothing it would keep.
	bool landContest = false;
	float landSide = 0.0;
	[branch] if (coverageAlpha > 0.001 && (landFilm || pixelSnowWeight < 0.998))
	{
		landSide = LandSnowSide(landBurial, landMelt);
		landContest = landSide < 16.0;
	}
#	else
	[branch] if (coverageAlpha > 0.001 && (landFilm || pixelSnowWeight < 0.998))
	{
		[flatten] if (LandSnowOdds(LandMasksCopy.Load(int3(input.Position.xy, 0)).y) + LandSnowSide(landBurial, landMelt) < 0.0)
			coverageAlpha = 0.0;
	}
#	endif
#endif
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	float3 objectMeet = float3(0.0, 0.0, 0.0);
	[branch] if (shellZ < 2048.0)
	{
		objectMeet = ShellObjectMeet(GridOrigin + gridLocal, input.WorldPos.z + ShellCameraPosAdjust.z);
		objectMeet.x *= 1.0 - smoothstep(1024.0, 2048.0, shellZ);
	}
#ifdef SNOW_SHELL_DEFERRED
	// Only the prepass's winners reach this pass, and it already ran the cut.
	coverageAlpha = 1.0;
#else
	// Hard alpha test plus a whisker of stochastic dust just BEYOND the cut
	// (the 0.2..0.5 alpha tail), never upward into the committed sheet.
	// Survivors write fully opaque.
	{
		float dust = saturate((coverageAlpha - 0.2) * (1.0 / 0.3));
		if (screenNoise * screenNoise >= dust)
			discard;
		coverageAlpha = 1.0;
	}
#endif

	// A melted floor is as thin as an edge: it takes the near micro-clamp
	// too, which the unmelted depth alone never asked for.
	float exportEffDepth = lerp(pixelEffDepth, min(pixelEffDepth, kFireMeltFloor), pixelMelt);
#ifdef SNOW_SHELL_DEPTH_PREPASS
	PS_PREPASS_OUTPUT prepassOut;
	prepassOut.RasterDepth = input.Position.z;
#	ifdef SNOW_PREPASS_ATTR
	prepassOut.AttrGrid = float4(input.GridLocal, normalize(input.TerrainNormalAlpha.xyz).xy);
	prepassOut.AttrGridDeriv = attrGridDeriv;
	prepassOut.AttrDepth = float4(input.CurrentClip.w, attrZDeriv, pixelRampDepth);
#	endif
	prepassOut.DepthLE = ShellExportDepthAt(input.Position.z, rawSceneDepth, shellZ, sceneZ, exportEffDepth, pixelCarve, input.WorldPos, input.Position.xy);
#	ifdef SNOW_PREPASS_EARLY
	// Pending: the land contest to run, or a clamp a static behind would
	// cancel. Marked by a negative raster depth; the claim fill skips it.
	// Thin snow on a fire or shock mark goes pending too, unflagged: the
	// ground and its scorch draw under it, and the decal overlay lets them
	// through as the snow thins (SnowBloodCapture.hlsl SnowHeightOver).
	const bool clampPending = prepassOut.DepthLE < input.Position.z;
	bool markPending = false;
	[branch] if (landRemaining < kDecalShowDepth)
		markPending = SampleSpellMark(gridLocal) > 0.0;
	prepassOut.Pending = 0.0;
	[branch] if (clampPending || landContest || markPending)
	{
		prepassOut.RasterDepth = -input.Position.z;
		prepassOut.Pending = float4(landSide, (clampPending ? 2.0 : 0.0) + (landContest ? 8.0 : 0.0), 0.0, 0.0);
	}
#	endif
	return prepassOut;
#endif
#ifndef SNOW_SHELL_DEPTH_PREPASS

	// Normal = smooth interpolated terrain normal + per-pixel gradient of
	// the shared carve profile (central differences at the deformation
	// map's resolution), so trench walls AND the edge berm shade by the
	// same shape the geometry displaces.
	float3 terrainNormal = normalize(input.TerrainNormalAlpha.xyz);
	// Shading depth mirrors ShellSurfaceZ's uncarved depth: the touch-down
	// toe, then melt thins toward kFireMeltFloor. Every relief gradient below
	// scales by it, so a print in a melt basin shades as flat as it is built.
	float pixelDepth = max(TouchDownToe(pixelRampDepth), 0.0);
	pixelDepth = lerp(pixelDepth, min(pixelDepth, kFireMeltFloor), pixelMelt);
	// Meeting a road's trench patch (Object Meeting Blend): where the two
	// surfaces cross, the sheet's trench walls go halfway to the patch's, the
	// patch comes the other half (SnowStaticsShell.hlsl BuildPatchVertex); the
	// macro slope goes the whole way to the road's here, the patch keeping its
	// own. Weighed by the height gap, not the road's edge: the sheet runs on
	// over the road and meets the patch wherever the two pass through one
	// height. The road is read bilinear over its texels, as the dense patch
	// lays it, and the meet fades out across the raster's last texel: the
	// max-of-4 steps at texel boundaries and drew straight seams. Shading only.
	float roadMeet = 0.0;
	float roadMeetDepth = 0.0;
	float2 roadMeetSlope = 0.0;
	[branch] if (ObjectMeetBand > 0.0 && ShellFlags.y != 0 && shellZ < 2048.0)
	{
		const float2 roadXY = GridOrigin + gridLocal;
		const float2 road = RoadRasterAt(roadXY);
		[branch] if (road.y > kNoRoadTop * 0.5 && road.x >= 1.0)
		{
			const float3 roadB = RoadRasterBilinear(roadXY);
			const float2 roadDT = roadB.x > 0.0 ? roadB.zy : road;
			const float2 centreBlur = ((CompactLook.w > 0.0 || TrackAge.y > 0.0) && !pixelUntouched) ? BermFieldPair(gridLocal).yz : float2(pixelCarve, TrackAge.x);
			const float roadZ = RoadPatchZ(roadDT, pixelCarve, centreBlur.x, UndulationSampled(roadXY), roadXY, TrackCarveLook(TrackAgeOf(centreBlur.y)));
			roadMeet = (1.0 - smoothstep(0.0, ObjectMeetBand * 0.5, abs(input.WorldPos.z + ShellCameraPosAdjust.z - roadZ))) *
			           (1.0 - smoothstep(1024.0, 2048.0, shellZ)) * smoothstep(0.0, 1.0, roadB.x);
			roadMeetDepth = roadDT.x;
			[branch] if (roadMeet > 0.001)
			{
				const float tStep = 16.0;
				roadMeetSlope = RoadTopSlope(roadB, RoadRasterBilinear(roadXY + float2(tStep, 0.0)), RoadRasterBilinear(roadXY - float2(tStep, 0.0)),
					RoadRasterBilinear(roadXY + float2(0.0, tStep)), RoadRasterBilinear(roadXY - float2(0.0, tStep)), tStep);
			}
		}
	}
	// The relief's depth where the sheet meets a road's patch: halfway to the road's.
	const float meetDepth = lerp(pixelDepth, roadMeetDepth, 0.5 * roadMeet);
	// Untouched: every profile tap is pixelDepth and every berm tap zero, so
	// both gradients and the centre are +0.
	float2 profileGrad = 0.0;
	float2 bermGrad = 0.0;
	float bermCenter = 0.0;
	// Track age at the pixel, the carve's (TrackAgeOf); 0 while tracks never
	// age, so nothing is read for it. The berm's ages the berm here and is done.
	float pixelAge = 0.0;
	[branch] if (!pixelUntouched)
	{
		float bermAge = 0.0;
		[branch] if (TrackAge.y > 0.0)
		{
			const float2 dates = BermFieldPair(gridLocal).zw;
			pixelAge = TrackAgeOf(dates.x);
			bermAge = TrackAgeOf(dates.y);
		}
		const float step = 4.0;
		float dXP = SampleDeformation(gridLocal + float2(step, 0.0));
		float dXN = SampleDeformation(gridLocal - float2(step, 0.0));
		float dYP = SampleDeformation(gridLocal + float2(0.0, step));
		float dYN = SampleDeformation(gridLocal - float2(0.0, step));
		// Snow Climate's slough and age lay the walls back toward the carve's
		// short blur, read only while the snow is loose or the track old.
		const float3 carveLook = TrackCarveLook(pixelAge);
		float4 blur = saturate(float4(dXP, dXN, dYP, dYN));
		[branch] if (CompactLook.w > 0.0 || carveLook.x > 0.0)
		{
			blur = float4(BermFieldPair(gridLocal + float2(step, 0.0)).y, BermFieldPair(gridLocal - float2(step, 0.0)).y,
				BermFieldPair(gridLocal + float2(0.0, step)).y, BermFieldPair(gridLocal - float2(0.0, step)).y);
		}
		profileGrad = float2(
						  CarveProfile(saturate(dXP), blur.x, meetDepth, GridOrigin + gridLocal + float2(step, 0.0), carveLook) - CarveProfile(saturate(dXN), blur.y, meetDepth, GridOrigin + gridLocal - float2(step, 0.0), carveLook),
						  CarveProfile(saturate(dYP), blur.z, meetDepth, GridOrigin + gridLocal + float2(0.0, step), carveLook) - CarveProfile(saturate(dYN), blur.w, meetDepth, GridOrigin + gridLocal - float2(0.0, step), carveLook)) /
		              (2.0 * step);
		// Berm shading: numerical gradient of the SAME blurred hill the
		// geometry displaces by, so the light/shadow break sits on the hill's
		// true flanks (the analytic shortcut put the terminator on the crest).
		// A steeper berm can shrink its step below the profile's; the mask then
		// needs its own taps at the same points.
		const float bStep = BermGradStep();
		float mXP = dXP, mXN = dXN, mYP = dYP, mYN = dYN;
		[branch] if (bStep < step - 0.01)
		{
			mXP = SampleDeformation(gridLocal + float2(bStep, 0.0));
			mXN = SampleDeformation(gridLocal - float2(bStep, 0.0));
			mYP = SampleDeformation(gridLocal + float2(0.0, bStep));
			mYN = SampleDeformation(gridLocal - float2(0.0, bStep));
		}
		float bermXP = BermField(gridLocal + float2(bStep, 0.0));
		float bermXN = BermField(gridLocal - float2(bStep, 0.0));
		float bermYP = BermField(gridLocal + float2(0.0, bStep));
		float bermYN = BermField(gridLocal - float2(0.0, bStep));
		// Differenced WITH the (1 - deformation) mask, so the shading gradient is
		// of the exact surface the vertex path displaces - an unmasked gradient
		// here shades a hill the geometry no longer has.
		bermGrad = float2(
			BermShape(bermXP) * saturate(1.0 - mXP) - BermShape(bermXN) * saturate(1.0 - mXN),
			BermShape(bermYP) * saturate(1.0 - mYP) - BermShape(bermYN) * saturate(1.0 - mYN)) / (2.0 * bStep) * TrackBermScale(bermAge);
		bermCenter = 0.25 * (bermXP + bermXN + bermYP + bermYN);
	}
	// Meeting a captured object: the sheet's MACRO slope goes halfway to the
	// object's (ShellObjectMeet); the coat comes the other half
	// (SnowStaticsShell.hlsl). Relief and grain stay the sheet's own.
	float2 macroGrad = -terrainNormal.xy / max(terrainNormal.z, 0.1);
	macroGrad = lerp(macroGrad, objectMeet.yz, 0.5 * objectMeet.x);
	macroGrad = lerp(macroGrad, roadMeetSlope, roadMeet);
	float2 gradZ = macroGrad + profileGrad + bermGrad * meetDepth * BermHeightAmp * BermDepthGate(meetDepth);

	// Undulation gradient (same field the VS displaced by) shades the dunes.
	float2 worldXYPS = GridOrigin + gridLocal;
	float undScale = saturate(pixelDepth / 8.0);
	[branch] if (undScale > 0.001)
	{
		gradZ += UndulationGradSampled(worldXYPS) * undScale;
	}

	// Trench detail tiles, by parallax occlusion: the mask across the trench
	// profile at the sheet point; the view ray marched through the detail
	// heights over the tangent plane, and the point it meets shaded with the
	// snow set's own maps, glints and lights. Fades out with the march and
	// into an object's coat where the two meet; its relief also with thin
	// cover and on trampled floors. Snow fire has melted (its marks, the heat
	// round fires) wears the impact set at full relief; an impact (a sprint, a
	// landing, a strike, a shove) blends toward it by how hard it hit and lifts
	// the relief up to 1.5x on top of the thin-cover and trampled rules.
	const float detailFade = 1.0 - smoothstep(1200.0, 2000.0, shellZ);
	float detailMask = 0.0;
	float detailRelief = 0.0;
	TrenchDetailTaps detailTaps = (TrenchDetailTaps)0;
	float detailLod = 0.0;
	float detailHitH = 0.0;
	float detailImpact = 0.0;
	const float2 detailPlane = gradZ;
	float3 shadePos = input.WorldPos;
#if defined(SNOW_NOISE_TABLE)
	[branch] if (!pixelUntouched && detailFade > 0.0 && (TrenchDetailRelief > 0.001 || TrenchDetailView != 0 || abs(TrenchDetailRoughness - 1.0) > 0.001))
	{
		const float4 detailField = BermFieldPair(gridLocal);
		const float detailReach = detailFade * (1.0 - objectMeet.x);
		detailMask = TrenchDetailMask(pixelCarve, detailField.xy, TrenchDetailCoverage) * detailReach;
		[branch] if (detailMask > 0.0 || SpeedSpoil > 0.5)
		{
			const float2 marks = SampleDetailMarks(gridLocal);
			detailMask = max(detailMask, SpeedSpoil * TrenchSpoilMask(marks.y) * detailReach);
			[branch] if (detailMask > 0.0)
			{
				const float meltWet = max(smoothstep(kSpellMarkStart, 0.15, marks.x), smoothstep(0.1, 0.4, pixelMelt));
				// An old track's clods are under new snow; fire's melt is not. The
				// carve's age in the trench, the berm's on its apron.
				const float detailAge = lerp(TrackAgeOf(detailField.w), TrackAgeOf(detailField.z), saturate(10.0 * pixelCarve));
				const float impact = marks.y * TrackDetailImpactScale(detailAge);
				detailImpact = max(meltWet, impact);
				detailRelief = TrenchDetailRelief * detailMask * max(saturate(pixelDepth / 10.0), meltWet) * TrampleDetailScale(worldXYPS, pixelCarve) *
				               (1.0 + kImpactReliefBoost * impact) * TrackDetailReliefScale(detailAge);
				detailTaps = TrenchDetailTapsAt(worldXYPS);
				detailLod = TrenchDetailLod(gDeferredGridDx, gDeferredGridDy);
			}
		}
	}
	[branch] if (detailRelief > 0.05)
	{
		float3 dir = normalize(input.WorldPos);
		const float4 duv = TrenchDetailStep(detailTaps, dir.xy);
		float t = TrenchDetailParallaxT(dir, detailPlane, detailTaps, duv,
			float3(detailRelief, detailLod, lerp(0.6, 2.0, saturate(shellZ / 1500.0))), detailImpact, screenNoise);
		detailHitH = (dir.z - dot(dir.xy, detailPlane)) * t;
		shadePos = input.WorldPos + dir * t;
		worldXYPS += dir.xy * t;
		gridLocal += dir.xy * t;
		detailTaps.uv += duv * t;
	}
	[branch] if (detailRelief > 0.001)
		gradZ += TrenchDetailGrad(detailTaps, detailLod, detailImpact) * detailRelief;
#endif

	float3 normalWS = normalize(float3(gradZ * -1.0, 1.0));
	// The surface without the trench detail: the snow maps' projection, frame
	// and march key on it, as on the geometric normal under a normal map.
	const float3 frameNormalWS = normalize(float3(-detailPlane, 1.0));

	// Snow texture taps, shared by albedo, normal and RMAOS so every map
	// agrees on the same anti-tiling offsets. Micro-relief fades with
	// distance, where the grain frequency aliases instead of detailing.
	float bumpFade = 1.0 - smoothstep(600.0, 2200.0, shellZ);
	float scorchRaw = 0.0;
	[branch] if (!pixelUntouched)
		scorchRaw = SampleScorch(gridLocal);
	float2 snowUV = (SnowUVOffset + gridLocal) / kSnowUVTile;
	SnowTaps snowTaps = ComputeSnowTapsNoGrad(snowUV, worldXYPS);
	snowTaps.duvdx = gDeferredGridDx / kSnowUVTile;
	snowTaps.duvdy = gDeferredGridDy / kSnowUVTile;
	float snowHeightMip = SnowHeightMipGrad(snowTaps.duvdx, snowTaps.duvdy);
	// Two-plane projection: top-down uv smears down near-vertical trench walls,
	// so steep pixels blend in a side-plane sample. Keyed on the per-pixel
	// trench normal, captured BEFORE the normal map perturbs normalWS - the
	// side POM march must resolve the view into the same plane these uvs use.
	// Earlier and narrower than the statics ramp: landscape walls live at 40-65
	// degrees, where any top-projection share boils as the camera moves. Side
	// takes over fully by ~57 degrees.
	float snowSteepness = smoothstep(0.75, 0.55, abs(frameNormalWS.z));
	float snowWorldZAbs = shadePos.z + ShellCameraPosAdjust.z;
	bool snowSideDropsX = abs(frameNormalWS.x) > abs(frameNormalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXYPS.y, snowWorldZAbs) : float2(worldXYPS.x, snowWorldZAbs);
	// NO SnowUVOffset on the side plane, and a STATIC 4096-unit fold (exactly
	// 24 tiles). SnowUVOffset compensates gridLocal's rebase, but the side
	// plane is absolute and never rebases, so adding it slides the wall texture
	// every time the grid advances. The fold keeps the uv small for float
	// precision.
	float2 snowUVSide = (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile;
	SnowTaps snowTapsSide = ComputeSnowTapsNoGrad(snowUVSide, snowSidePlane);
	snowTapsSide.duvdx = (snowSideDropsX ? float2(gDeferredGridDx.y, gDeferredZDx) : float2(gDeferredGridDx.x, gDeferredZDx)) / kSnowUVTile;
	snowTapsSide.duvdy = (snowSideDropsX ? float2(gDeferredGridDy.y, gDeferredZDy) : float2(gDeferredGridDy.x, gDeferredZDy)) / kSnowUVTile;
	float snowHeightMipSide = SnowHeightMipGrad(snowTapsSide.duvdx, snowTapsSide.duvdy);
	// Tangent basis for the snow maps. The snow uv is a world-XY planar
	// projection, so the frame is axis-aligned by construction: bumpT is
	// world +X (uv.x), bumpB world +Y (uv.y). Built from the geometric normal
	// BEFORE the normal map perturbs it, matching Lighting.hlsl's use of the
	// interpolated TBN. Shared with the parallax shadow below.
	float3 bumpT = normalize(cross(float3(0.0, 1.0, 0.0), frameNormalWS) + float3(1e-5, 0.0, 0.0));
	float3 bumpB = cross(frameNormalWS, bumpT);

	float3 V = -normalize(input.WorldPos);

	// Pre-parallax derivatives for the glint grid, mirroring Lighting.hlsl's
	// uvOriginal: the POM offset below is view-dependent, and glints must not
	// ride it. (The glint uv itself is rebuilt world-anchored at the call.)
	// Steep pixels take the side plane, like the albedo: the top plane's
	// footprint on a wall is a needle, and the glint NDF then resolves single
	// microfacets per pixel.
	const bool glintSide = snowSteepness > 0.5;
	const float2 glintDuvdx = glintSide ? snowTapsSide.duvdx : snowTaps.duvdx;
	const float2 glintDuvdy = glintSide ? snowTapsSide.duvdy : snowTaps.duvdy;

	// Parallax occlusion: the depth the shell was missing. The normal map
	// only tilts the lighting; this moves the texture itself, so grain
	// occludes grain and the surface reads as thick. Runs BEFORE every snow
	// fetch, and shifts the tap set rather than rebuilding it, so albedo,
	// normal, RMAOS and the parallax shadow all ride the displaced position.
	// Glazed snow takes less of Parallax Depth and Parallax Shadow.
	const float iceParallax = SnowIceParallax(pixelCarve, pixelMelt, input.WorldPos.z + ShellCameraPosAdjust.z, gridLocal, true);
	[branch] if (HasSnowHeight > 0.5 && SnowParallax.z > 0.001 && bumpFade > 0.001)
	{
		// bumpT/bumpB ARE the uv axes (world-XY planar projection); EM's
		// marcher builds tangent-space view from this frame itself.
		float3x3 snowTbn = float3x3(bumpT, bumpB, frameNormalWS);
		DisplacementParams pomParams = SnowDisplacementParams();
		pomParams.HeightScale *= SnowParallax.z * iceParallax;
		float2 pomOffset = SnowParallaxOffset(snowTaps, snowUV, V, snowTbn, shellZ, snowHeightMip, screenNoise, pomParams);
		// Trench walls march NOTHING: a march is only honest when the surface
		// lies in its projection plane, and a 40-60 degree wall lies in neither,
		// so both marches redraw the wall as the camera moves. A steepness scale
		// still leaves most of the top march live there. Hard tilt cut instead,
		// dead by ~40 degrees, side plane sampled unmarched - at a 21-unit wall
		// stability beats parallax.
		pomOffset *= 1.0 - smoothstep(0.15, 0.35, 1.0 - abs(frameNormalWS.z));
		snowUV += pomOffset;
		snowTaps = OffsetSnowTaps(snowTaps, pomOffset);
	}

	[branch] if (HasSnowNormal > 0.5 && bumpFade > 0.001)
		normalWS = normalize(normalWS + bumpFade * SampleSnowNormalOffset(SnowNormalMap, snowTaps, snowTapsSide, snowSteepness, normalWS,
										   bumpT, bumpB, snowSideDropsX));
	else if (HasSnowTexture != 0 && bumpFade > 0.001)
		normalWS = ShellLuminanceBump(normalWS, worldXYPS, bumpFade);

	float3 viewNormal = normalize(mul((float3x3)CameraView, normalWS));

	// Snow material: the modlist's snow diffuse when available, otherwise a
	// bright, slightly blue constant.
	float3 kSnowAlbedo = float3(0.82, 0.84, 0.88);
	[branch] if (HasSnowTexture != 0)
	{
		kSnowAlbedo = SampleSnowPlanar(SnowDiffuse, snowTaps, snowTapsSide, snowSteepness).rgb;
		// The colour space the ground reads this texture in.
		kSnowAlbedo = SnowColor::SetAlbedo(kSnowAlbedo, SnowTextureIsLinear != 0.0);
	}
	// Scorch: a shock discharge leaves the snow burnt where it struck. Darkened
	// rather than recoloured, and biased slightly warm, so it reads as fouled
	// snow instead of a grey decal painted over it.
	{
		float scorch = scorchRaw * SpellShading.x;
		[branch] if (scorch > 0.001)
			kSnowAlbedo = lerp(kSnowAlbedo, kSnowAlbedo * float3(0.30, 0.27, 0.26), saturate(scorch));
	}
	// Blood lies on the snow the way the game's decal lies on the ground: its
	// colour over the albedo by its alpha, from a detail tile where one lies
	// and from the blood map elsewhere, so a tile's edge does not show. Drying
	// turns it maroon.
	float bloodFresh = 0.0;
	float4 blood = SampleBlood(input.GridLocal, bloodFresh);
	float4 bloodPaint;
	SampleBloodTiles(worldXYPS, gDeferredGridDx, gDeferredGridDy, blood, bloodFresh, bloodPaint);
	// Under a tile the map's reading is zero, elsewhere the tile's.
	blood += bloodPaint;
	[branch] if (blood.a > 0.002)
	{
		float3 paintRgb = SnowColor::SetAlbedo(blood.rgb, true);
		paintRgb = lerp(paintRgb, paintRgb * float3(0.75, 0.55, 0.55), 1.0 - bloodFresh);
		kSnowAlbedo = lerp(kSnowAlbedo, paintRgb, saturate(blood.a * BloodLook.x));
	}
	const float bloodCover = blood.a;
	// A rune's glyph or a frost mark lies on the snow as the game's decal lies
	// on the ground: its colour and normal over the snow's by its alpha, its
	// specular, gloss and glow put in after lighting.
	RuneGlyph rune = SampleRunes(worldXYPS, gDeferredGridDx, gDeferredGridDy, kMarkAnyBase);
	[branch] if (rune.colour.a > 0.004)
	{
		kSnowAlbedo = lerp(kSnowAlbedo, rune.colour.rgb, rune.colour.a);
		float3 runeNormal = normalize(rune.tangent * rune.normalTS.x + rune.bitangent * rune.normalTS.y + normalWS * rune.normalTS.z);
		normalWS = normalize(lerp(normalWS, runeNormal, rune.colour.a));
	}
	// Thin-snow and wet print, wet snow and ice (SnowWetPrint): x the roughness
	// cut, y the glints left.
	const float2 wetLook = SnowWetPrint(kSnowAlbedo, pixelCarve, pixelMelt, pixelDepth, input.WorldPos.z + ShellCameraPosAdjust.z, gridLocal, true);
	// PBR snow material: GGX microfacet specular with Fresnel and energy-
	// conserving lobes. Light and ambient stay in the frame's units
	// (DirLightColor is already pi-scaled by pipeline convention, so no
	// Lambert 1/pi on diffuse); the indirect specular lobe goes to the
	// Reflectance RT where the composite applies cubemap and ambient
	// specular like any TruePBR surface.
	float kSnowRoughness = 0.6;
	float3 kSnowF0 = float3(0.028, 0.028, 0.028);

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
	// Trench detail: its roughness crank over the mask.
	snowRoughness = clamp(snowRoughness * lerp(1.0, TrenchDetailRoughness, detailMask), 0.05, 1.0);

	// Wet print: after the RMAOS overwrite, which would discard it.
	snowRoughness *= 1.0 - wetLook.x;
	// Fresh blood is wet and glossy; dried blood is matte like the snow.
	snowRoughness = lerp(snowRoughness, 0.22, saturate(bloodCover * 2.0) * bloodFresh * BloodLook2.y);
	// Soaked snow, ice and blood do not sparkle.
	const float glintsLeft = wetLook.y * (1.0 - saturate(bloodCover * 3.0));

	float3 L = SharedData::DirLightDirection.xyz;
	float satNdotL = saturate(dot(normalWS, L));
	float satNdotV = saturate(abs(dot(normalWS, V)) + 1e-5);

	float worldShadow = ShadowSampling::GetWorldShadow(shadePos, ShellCameraPosAdjust.xyz);
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
		sunShadow = worldShadow * SnowShadow::GetCascadeShadow(shadePos, normalWS, lerp(1.0, 6.0, farShadowT), uint2((uint)BorderStyle.z, (uint)BorderStyle.w));
	}
	else
	{
		// Fallback: the Volumetric Shadows 512px VSM moments copy (blurry).
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(shadePos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}

	// Heightfield self-shadowing: the shell is a heightfield, so march it
	// toward the sun and find the horizon this pixel must clear. Hills,
	// mounds, field raises and the dune undulation all cast soft shadows
	// onto the snow behind them; contact detail the game's cascades cannot
	// hold. Geometric growth in the tap distances gives sharp close shadows
	// and long soft ones at low sun angles.
	float pixelVertexAO = -1.0;
	[branch] if ((ShellFlags.x & 1) != 0 && sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		// Redistributed toward the NEAR field (kSunMarchDist). The first tap set
		// the finest boundary the horizon can resolve, so at 28 units every
		// shadow edge was smeared over at least that distance and the softness
		// below had to be wide enough to hide it. Same tap COUNT, same 1000-unit
		// reach.
		const float sunLen2D = max(length(L.xy), 1e-4);
		const float sunTan = L.z / sunLen2D;
		const float2 stepDir = L.xy / sunLen2D;
		const float surfZ = shadePos.z + ShellCameraPosAdjust.z;
		// The occluder heights are baked (MarchHeightCS: this march's own tap
		// recipe, carve, berm, melt and dunes included): per trail-map texel
		// near, per land-layer vertex past the trail map. A march that leaves
		// both casts nothing.
		const bool nearCache = MarchCacheCovers(gridLocal, stepDir, kSunMarchDist[4]);
		[branch] if (nearCache || MarchFarCovers(gridLocal, stepDir, kSunMarchDist[4]))
		{
			// Near: a crisp penumbra band. Far: a much wider penumbra plus
			// attenuated strength; the march's per-texel horizon steps stop
			// reading as hard-edged blocks on distant snow.
			const float soft = lerp(0.03, 0.35, farShadowT);
			float marchLight;
			// The receiver comes from the same cache as the taps, so what the
			// resampling gets wrong cancels instead of standing as a false rise
			// over the nearest tap; one unit of bias absorbs the residue.
			[branch] if (nearCache)
			{
				const float h0 = MarchHeightNear(gridLocal);
				const float marchRef = max(h0 > -50000.0 ? h0 : surfZ, surfZ - 2.0);
				marchLight = MarchNearLight(MarchNearHorizon(gridLocal, stepDir, sunTan, marchRef), sunTan, soft, lerp(2.0, 8.0, farShadowT));
			}
			else
			{
				float h[6];
				MarchCachedHeights(gridLocal, stepDir, false, h);
				const float marchRef = max(h[0] > -50000.0 ? h[0] : surfZ, surfZ - 2.0);
				float horizonTan = -10.0;
				[unroll] for (uint marchI = 0; marchI < 5; marchI++)
					horizonTan = max(horizonTan, (h[marchI + 1] - marchRef - 1.0) / kSunMarchDist[marchI]);
				marchLight = smoothstep(-1.5 * soft, 1.5 * soft, sunTan - horizonTan);
			}
			// Penumbra CENTRED on the horizon: the old band was the same total
			// width (3*soft) but sat entirely on the LIT side of
			// sunTan == horizonTan, so the shadow always over-reached its
			// geometric edge - worse the lower the sun. Kept identical to the
			// statics march; the two shells must agree across the seam.
			// Fades out toward the horizon: a 4-unit raster and a 32-unit terrain
			// texel cannot resolve contact shadows at a grazing sun, every bump
			// and draped stone became a blotch at sunset, and the cascades own
			// the long shadows there anyway. Same gate on the statics march.
			const float lowSun = smoothstep(0.04, 0.12, sunTan);
			sunShadow *= lerp(1.0, lerp(marchLight, 1.0, 0.7 * farShadowT), lowSun);
		}
	}

	// 2: this frame's mask was marched on the snow surface; applied as
	// Lighting.hlsl applies it, without the gates below.
	const bool sssSeesSnow = ScreenSpaceShadowsActive > 1.5;
	[branch] if (sssSeesSnow && !SharedData::InInterior && dot(normalWS, L) >= 0.0)
		sunShadow *= ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0);

	// A mask that did not see the snow was marched on the ground UNDER the
	// shell, so applying it near paints buried objects through the snow.
	// Cascades own the near field; SSS blends in only beyond them, where it
	// carries the distant LOD tree shadows and the shell hugs the ground the
	// march ran on.
	// A zero handoff leaves sunShadow exactly as it is.
	[branch] if (!sssSeesSnow && ScreenSpaceShadowsActive > 0.5 && SnowShadow::GetSssHandoff(shellZ) > 0.0)
	{
		// Depth agreement is the gate: trust the mask only where the shell hugs
		// the surface the march saw. Measured VERTICALLY, not along the view
		// ray - the along-ray gap is depth / sin(elevation) and explodes at far
		// grazing views, which forces a distance override that then paints
		// buried shadows through distant drifts. One vertical rule covers every
		// range. The along-ray term survives as a backstop for steep faces seen
		// edge-on. Thresholds cannot separate grass shadows from buried prints;
		// their gaps overlap, so the caster discriminator below does it.
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
						sssBlend = 0.0;
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

		float parallaxShadow = 1.0 - saturate(occlusion * SnowParallax.y * iceParallax);
		// Faded on the same band as the normal map it occludes, and faded out
		// on walls: the march is jittered by screen-anchored noise, which on a
		// wall's high-contrast side-projected grain crawls as the camera moves.
		// Walls take the trench self-shadow march and the cascades instead.
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade * (1.0 - snowSteepness));
	}
#if defined(SNOW_NOISE_TABLE)
	// The trench detail shadows itself: the sun's ray from the point the march
	// met, through the detail heights, as strong as Parallax Shadow makes the
	// snow texture's. TrenchDetailShadow is saturate(Parallax Depth).
	[branch] if (detailRelief > 0.05 && TrenchDetailShadow > 0.001 && SnowParallax.y > 0.001 && sunShadow > 0.01 && satNdotL > 0.001)
		sunShadow *= lerp(1.0, TrenchDetailSunShadow(detailTaps, detailHitH, L, detailPlane, float3(detailRelief, detailLod, SnowParallax.y * iceParallax), detailImpact, screenNoise),
			min(TrenchDetailShadow, SnowParallax.z * iceParallax));
#endif

	float3 sunLight = SharedData::DirLightColor.xyz * sunShadow;

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1): glints, energy conservation and every future
	// TruePBR lobe ride the shared code. Outputs are Lighting-internal units;
	// Color::PBRLightingScale is applied at the write tail below.
	// World-anchored glint uv: snowUV's fmod(GridOrigin) fold shifts by whole
	// tiles as the grid advances - a no-op for the periodic texture, but the
	// glint hash is NOT tile-periodic, so it re-rolls the sparkle field every
	// few metres. Fold on a STATIC 4096-unit block (an exact tile multiple).
	const float2 glintUV = glintSide ? (snowSidePlane - 4096.0 * floor(snowSidePlane / 4096.0)) / kSnowUVTile :
	                                   fmod(GridOrigin + gridLocal, 4096.0) / kSnowUVTile;
	// Built once, shared by the sun and every point light (M3). Soaked snow
	// does not sparkle. Glints reach only the direct lobes: the sun's, zero
	// without sunShadow, and the cluster's lights.
	bool glintsLit = sunShadow != 0.0;
	[branch] if (!glintsLit && PointLightsActive > 0.5)
	{
		float glintViewZ;
		float2 glintClusterUV;
		ShellClusterCoords(input.WorldPos, glintViewZ, glintClusterUV);
		glintsLit = SnowLights::ClusterLightCount(glintClusterUV, glintViewZ) > 0;
	}
	const float glintsHere = glintsLit ? EnableGlints * glintsLeft : 0.0;
	SnowMaterialCtx snowMtl = SnowBuildMaterial(normalWS, kSnowAlbedo, snowRoughness, snowF0, snowAO,
		SnowGlintParams, glintsHere, glintUV, glintDuvdx, glintDuvdy, input.Position.xy);
	SnowSunLighting sunLit = SnowEvaluateSunPBR(snowMtl, normalWS, V, shadePos, ShellCameraPosAdjust.xyz, sunShadow,
		glintUV, glintDuvdx, glintDuvdy, rune.specular);
	float3 specularLobe = sunLit.specularLobe;
	float3 diffuseLobe = sunLit.diffuseLobe;
	float3 directDiffuse = sunLit.directDiffuse;
	float3 directSpecular = sunLit.directSpecular;
	float3 markSpecular = sunLit.markSpecular;

	// Placed lights (fires, lanterns): the clustered LLF list, with each
	// shadow-casting light's own map sampled at the shell surface.
	[branch] if (PointLightsActive > 0.5)
	{
		float viewZ;
		float2 clusterUV;
		ShellClusterCoords(input.WorldPos, viewZ, clusterUV);
		SnowLights::AccumulatePointLights(snowMtl, shadePos, shadePos + ShellCameraPosAdjust.xyz,
			normalWS, V, viewZ, clusterUV, glintUV, glintDuvdx, glintDuvdy, float4(-1.0, 0.0, 0.0, 0.0), rune.specular,
			directDiffuse, directSpecular, markSpecular);
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
	[branch] if (pixelVertexAO < 0.0)
		pixelVertexAO = SampleTerrainVertexAO(gridLocal);
	float landVertexAO = ColorManagement::SRGBToWorking(pixelVertexAO);
	landVertexAO = lerp(1.0, landVertexAO, SharedData::truePBRSettings.VertexAOStrength);
	// Skylighting parity with Lighting.hlsl's deferred tail: the ambient is
	// darkened by the probe volume with the same multi-bounce term terrain
	// uses (ApplySkylighting passes the SCALED albedo).
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
	// The mark is a vanilla surface: its own specular and glow in vanilla
	// units, no reflectance and no gloss (Lighting.hlsl:1882).
	directSpecular = lerp(directSpecular, markSpecular, rune.colour.a);
	specularLobe *= 1.0 - rune.colour.a;
	float3 preLit = ambientPart + directDiffuse + rune.emission;

	// Terrain Blending-style output: alpha rides every .w and the stochastic
	// blend mask goes to NormalGlossiness.w, exactly as Lighting.hlsl's
	// deferred tail encodes it for the temporal resolve.
	float alpha = coverageAlpha;
	float stochasticBlend = (screenNoise * screenNoise) < alpha ? 1.0 : 0.0;

	PS_OUTPUT psout;
	psout.Diffuse = float4(preLit, alpha);
	psout.MotionVectors = float4(motionVector, 0.0, alpha);
	psout.NormalGlossiness = float4(GBuffer::EncodeNormal(viewNormal), (1.0 - snowRoughness) * (1.0 - rune.colour.a), stochasticBlend);
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
#if defined(SNOW_SHELL_DEFERRED) && defined(SNOW_CONDITION_VIEW)
	{
		const float4 trackField = BermFieldPair(gridLocal);
		const float2 track = SnowClimate2.z < 6.5 ? float2(lerp(TrackAgeOf(trackField.w), TrackAgeOf(trackField.z), smoothstep(0.0, 0.1, pixelCarve)), saturate(10.0 * max(pixelCarve, bermCenter))) :
		                                             float2(SnowIceLook(pixelCarve, pixelMelt, input.WorldPos.z + ShellCameraPosAdjust.z, gridLocal, true), 0.0);
		const float3 conditionColor = SnowConditionColor(SnowConditionAt(input.WorldPos.z + ShellCameraPosAdjust.z), SnowClimate2.z, track);
		psout.Diffuse.rgb = conditionColor * 2.0;
		psout.Albedo.rgb = 0.0;
		psout.Specular.rgb = 0.0;
		psout.Reflectance.rgb = 0.0;
	}
#endif
#if defined(SNOW_SHELL_DEFERRED)
	[branch] if (TrenchDetailView != 0)
	{
		// Orange where the trench detail tiles apply, over grey shading.
		const float shade = dot(psout.Diffuse.rgb, float3(0.2126, 0.7152, 0.0722));
		psout.Diffuse.rgb = lerp(shade.xxx, float3(1.0, 0.35, 0.08) * (shade * 1.6 + 0.08), detailMask);
		psout.Albedo.rgb = 0.0;
		psout.Specular.rgb = 0.0;
		psout.Reflectance.rgb = 0.0;
	}
#endif

	return psout;
#endif  // !SNOW_SHELL_DEPTH_PREPASS
}

#ifdef SNOW_SHELL_DEFERRED
// Deferred shading: one full-screen triangle at depth 0, tested LESS against
// the fill's private depth, so it shades exactly the prepass's winners and
// every quad is whole. The prepass's attributes stand in for the interpolants.
Texture2D<float4> DeferredGrid : register(t91);
Texture2D<float4> DeferredGridDeriv : register(t92);
Texture2D<float4> DeferredDepth : register(t93);

PS_OUTPUT main(float4 pos : SV_Position)
{
	const int3 px = int3(pos.xy, 0);
	const float4 grid = DeferredGrid.Load(px);
	const float4 gridDeriv = DeferredGridDeriv.Load(px);
	const float4 depth = DeferredDepth.Load(px);
	gDeferredGridDx = gridDeriv.xy;
	gDeferredGridDy = gridDeriv.zw;
	gDeferredZDx = depth.y;
	gDeferredZDy = depth.z;
	gDeferredRampDepth = depth.w;

	// The raster point back from its view depth.
	const float w = depth.x;
	float3 rel;
	SHELL_DEFERRED_REL(rel, pos.xy, w)

	VS_OUTPUT input = (VS_OUTPUT)0;
	input.Position = float4(pos.xy, 0.0, 1.0);
	input.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	input.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(rel + ShellCameraPosAdjust.xyz - ShellCameraPreviousPosAdjust.xyz, 1.0));
	input.WorldPos = rel;
	input.GridLocal = grid.xy;
	input.TerrainNormalAlpha = float4(grid.zw, sqrt(saturate(1.0 - dot(grid.zw, grid.zw))), 1.0);
	return ShellShade(input);
}
#endif
#endif  // !SNOW_S1_RESOLVE
#endif

#ifdef COMPUTESHADER
#ifdef SNOW_TERRAIN_CUT_CS
// TerrainWindowWet's builder: one WaterBareTexel per texel, run when the
// terrain or the water window changes rather than in every terrain tap.
RWTexture2D<float4> TerrainWetOut : register(u4);

[numthreads(8, 8, 1)] void TerrainCutCS(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= TerrainDim))
		return;
	float4 texel = TerrainWindow.Load(int3(id.xy, 0));
	texel.xyz = WaterBareTexel(texel.xyz, int2(id.xy));
	TerrainWetOut[id.xy] = texel;
}
#endif

#ifdef SNOW_MARCH_HEIGHT_CS
// The sun march's occluder heights (SnowFields.hlsli MarchHeightGround and
// MarchHeightFar): at each deformation texel's centre in the map's torus, over
// the berm bake's tile list (the map or the berm field changed there) or a
// band of rows; and at each land-layer vertex over the FineWindow.
RWTexture2D<float> MarchHeightOut : register(u0);
StructuredBuffer<uint> MarchTilesIn : register(t63);
cbuffer MarchCacheCB : register(b2)
{
	uint MarchRowStart;
	uint MarchRowCount;
	uint2 MarchCachePad;
}

// The surface the march must see: the CARVED surface (same floor rule as the
// geometry - without the carve a wide trench reads as ringed by full-height
// snow and sits in permanent shadow even facing the sun), the MELT bowls
// (fires, clearings) for the same reason, the berm FIELD (a raw deformation
// value's BermShape would ring every trail with a phantom ridge across its
// wall; the unbaked A/B path leaves the term out), and the dunes. No object
// term: every object under the shell is geometry the cascades already shadow
// with its real silhouette, and the shell draped over it casts through its
// own caster. Past the seam the surface continues level from the seam: the far
// fill there would stand up to ~2000 units off the land as phantom walls.
float MarchTapHeight(float2 a_gridLocal)
{
	const float2 gridLocal = ShellSeamClamp(a_gridLocal);
	const float3 st = SampleTerrain(gridLocal);
	float depth = max(st.y, 0.0);
	depth = lerp(depth, min(depth, kFireMeltFloor), saturate(SampleExclusionMask(GridOrigin + gridLocal).y));
	const float deform = SampleDeformationMarch(gridLocal);
	const float4 berm = BermBakeActive > 0.5 ? BermFieldBaked(gridLocal) : float4(0.0, deform, TrackAge.x, TrackAge.x);
	depth = CarveProfileNoTeeth(deform, berm.y, depth, TrackCarveLook(TrackAgeOf(berm.z))) +
	        BermShape(berm.x) * saturate(1.0 - deform) * depth * BermHeightAmp * BermDepthGate(depth) * TrackBermScale(TrackAgeOf(berm.w));
	return st.x + depth + Undulation(GridOrigin + gridLocal) * saturate(depth / 8.0);
}

void MarchHeightTexel(uint2 phys)
{
	SNOW_DEFORM_DIMS(dims);
	if (any(float2(phys) >= dims))
		return;
	const float2 logical = float2((int2(phys) - DeformMapOrigin) & (int2(dims) - 1));
	MarchHeightOut[phys] = MarchTapHeight((logical + 0.5) / dims / DeformInvWorldSize - GridToDeformOffset);
}

[numthreads(8, 8, 1)] void MarchHeightTilesCS(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	const uint listIndex = Gid.y * 1024u + Gid.x;
	if (listIndex >= MarchTilesIn[0])
		return;
	const uint packed = MarchTilesIn[1 + listIndex];
	MarchHeightTexel(uint2(packed & 0xFFFFu, packed >> 16u) * 8u + GTid.xy);
}

[numthreads(8, 8, 1)] void MarchHeightRowsCS(uint3 DTid : SV_DispatchThreadID)
{
	SNOW_DEFORM_DIMS(dims);
	if (DTid.y >= MarchRowCount)
		return;
	MarchHeightTexel(uint2(DTid.x, (MarchRowStart + DTid.y) % (uint)dims.y));
}

// The far cache: texel i on land-layer vertex i, no torus.
[numthreads(8, 8, 1)] void MarchHeightFarRowsCS(uint3 DTid : SV_DispatchThreadID)
{
	uint2 dims;
	MarchHeightOut.GetDimensions(dims.x, dims.y);
	if (DTid.x >= dims.x || DTid.y >= MarchRowCount)
		return;
	const uint2 t = uint2(DTid.x, (MarchRowStart + DTid.y) % dims.y);
	MarchHeightOut[t] = MarchTapHeight(float2(t) * FineWindow.w - FineWindow.xy);
}
#endif

#ifdef SNOW_MARCH_SUN_CS
// The sun envelope (SnowFields.hlsli MarchSunEnvelope): at each deformation
// texel's centre, the highest ground cache height toward the sun less its run
// times the sun's tangent, over SunEnvelopeRun; a run ends at the window's
// edge. Over the swept berm-bake tiles or a band of rows.
RWTexture2D<float> MarchSunOut : register(u0);
StructuredBuffer<uint> MarchTilesIn : register(t63);
cbuffer MarchCacheCB : register(b2)
{
	uint MarchRowStart;
	uint MarchRowCount;
	uint2 MarchCachePad;
	// xy = the sun's horizontal direction (unit), z = its tangent.
	float4 MarchSun;
}

void MarchSunTexel(int2 a_phys)
{
	SNOW_DEFORM_DIMS(dims);
	const int2 phys = a_phys & (int2(dims) - 1);
	const float2 logical = float2((phys - DeformMapOrigin) & (int2(dims) - 1));
	const float texel = 1.0 / (DeformInvWorldSize * dims.x);
	const float2 gridLocal = (logical + 0.5) * texel - GridToDeformOffset;
	const float2 margin = 1.5 / dims;
	float envelope = -1e30;
	bool inside = true;
	[loop] for (uint k = 1; k <= kSunEnvelopeSteps && inside; k++)
	{
		const float s = SunEnvelopeRun(k) * texel;
		const float2 p = gridLocal + MarchSun.xy * s;
		const float2 uv = (GridToDeformOffset + p) * DeformInvWorldSize;
		inside = all(uv >= margin) && all(uv <= 1.0 - margin);
		[branch] if (inside)
		{
			float2 f;
			envelope = max(envelope, MarchCacheBilinear(MarchCacheTexels(MarchHeightGround, p, f), f) - s * MarchSun.z);
		}
	}
	MarchSunOut[phys] = envelope;
}

// The segment from a_p along a_v meets the box [a_lo, a_hi].
bool SunRunMeetsBox(float2 a_p, float2 a_v, float2 a_lo, float2 a_hi)
{
	float t0 = 0.0;
	float t1 = 1.0;
	bool meets = true;
	[unroll] for (uint i = 0; i < 2; i++)
	{
		[flatten] if (abs(a_v[i]) < 1e-6)
		{
			meets = meets && a_p[i] >= a_lo[i] && a_p[i] <= a_hi[i];
		}
		else
		{
			const float u0 = (a_lo[i] - a_p[i]) / a_v[i];
			const float u1 = (a_hi[i] - a_p[i]) / a_v[i];
			t0 = max(t0, min(u0, u1));
			t1 = min(t1, max(u0, u1));
		}
	}
	return meets && t0 <= t1;
}

// A changed tile moves the envelope of every texel whose run crosses it: the
// tile swept away from the sun by the reach, a texel of bilinear margin.
[numthreads(8, 8, 1)] void MarchSunTilesCS(uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
	const uint listIndex = Gid.y * 1024u + Gid.x;
	if (listIndex >= MarchTilesIn[0])
		return;
	const uint packed = MarchTilesIn[1 + listIndex];
	const int2 tile = int2(packed & 0xFFFFu, packed >> 16u) * 8;
	const float2 run = MarchSun.xy * (SunEnvelopeRun(kSunEnvelopeSteps) + 1.0);
	const int2 lo = tile + int2(floor(min(-run, 0.0))) - 1;
	const int2 hi = tile + 8 + int2(ceil(max(-run, 0.0))) + 1;
	const float2 boxLo = float2(tile) - 1.0;
	const float2 boxHi = float2(tile) + 9.0;
	[loop] for (int y = lo.y + (int)GTid.y; y < hi.y; y += 8)
	{
		[loop] for (int x = lo.x + (int)GTid.x; x < hi.x; x += 8)
		{
			[branch] if (SunRunMeetsBox(float2(x, y) + 0.5, run, boxLo, boxHi))
				MarchSunTexel(int2(x, y));
		}
	}
}

[numthreads(8, 8, 1)] void MarchSunRowsCS(uint3 DTid : SV_DispatchThreadID)
{
	SNOW_DEFORM_DIMS(dims);
	if (DTid.x >= (uint)dims.x || DTid.y >= MarchRowCount)
		return;
	MarchSunTexel(int2(DTid.x, (MarchRowStart + DTid.y) % (uint)dims.y));
}
#endif
#endif
