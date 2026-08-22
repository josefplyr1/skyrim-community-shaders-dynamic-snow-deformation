// Statics snow skin: re-draws captured projected-snow statics (cliffs, rocks,
// drifts, roofs, logs) inflated along their vertex normals, with the same
// snow material as the terrain shell so the two read as one blanket.
//
// Drawn inside SnowDeformation::DrawShell right after the terrain shell, so
// it inherits that pass's bindings: ShellCB (b0), terrain window (t0),
// deformation map (t1), snow maps (t2/t6/t7), sampler s0 and the b4-b6 shared
// data. Only the input layout, vertex/index buffers, shaders and StaticCB
// (b1) change per object.
//
// The VS consumes only POSITION and NORMAL; D3D11 accepts input layouts
// carrying more elements than the shader reads, so one layout per vertex
// descriptor covers every static mesh format.
//
// cbuffer ShellCB must stay layout-identical to SnowShell.hlsl (and ShellCB
// in SnowDeformation.h).

#include "Common/BRDF.hlsli"
#include "Common/Color.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

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

	float BorderTrampledFade;
	float BorderUntrampledFade;
	float SnowSnowFade;   // object-skin <-> landscape-shell cross-fade band
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
	// Crisp grain retired 2026-08-22; layout keepers.
	float CrispScaleV;
	float CrispStrengthV;

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
	// REAL atlas slices (round 22). Do not drop them.
	float4 SpellShading;
	float4 CrustLook;
	float4 CrustLook2;
	// x/y landscape border dials (unused here); zw = sun cascade atlas
	// slices for the crisp shadow path.
	float4 BorderStyle;
	// Compacted snow (Stage 1): x glint suppression, y albedo darkening
	// fraction at full churn, z roughness rise. One constant, both shells.
	float4 CompactLook;
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
	float2 padStatics;
}

Texture2D<float4> DeformationMap : register(t1);
// Baked berm field (BermFieldCS): the 17-tap disc average of the deformation
// map, at the map's own resolution and addressing.
Texture2D<float> BermFieldMap : register(t14);
// Wide exclusion field + frost crystal patterns; the landscape shell's slots
// (t15-t17) and readers, bound by the skin draw since round 35.
Texture2D<float2> ExclusionFieldMap : register(t15);
Texture2D<float4> FrostPatternNormal : register(t16);
Texture2D<float4> FrostPatternDiffuse : register(t17);

// The domain shader samples the displacement companion for tessellated
// relief, so the material block is visible to it as well as the PS.
#if defined(PSHADER) || defined(DOMAINSHADER)
Texture2D<float4> TerrainWindow : register(t0);
Texture2D<float4> SnowDiffuse : register(t2);
// Full-scene depth copy taken before the shell pass (see SnowShell.hlsl).
Texture2D<float> SceneDepth : register(t3);
// TruePBR snow companion maps (see SnowShell.hlsl); inherited bindings.
Texture2D<float4> SnowNormalMap : register(t6);
Texture2D<float4> SnowRmaosMap : register(t7);
// Displacement companion (_p): tessellated relief and the parallax
// self-shadow. float4 to match Extended Materials' TexParallaxSampler
// convention; the SRV is single-channel, so only .x carries data.
Texture2D<float4> SnowHeightMap : register(t8);
// Depth after the terrain shell drew (its surface included); the skin's
// view-ray reference for cross-fading into the landscape shell.
Texture2D<float> ShellDepthCopy : register(t9);
SamplerState SnowSampler : register(s0);
#endif

// Shared trench-detail shaping (noise, berm shape/bake tap, churn) - the
// verbatim-identical pieces of both shells live in one file (M8).
#include "SnowDeformation/SnowFields.hlsli"

// Must match kSnowUVTile in SnowShell.hlsl (the game's landscape tiling:
// 24 repeats per 4096-unit cell).
static const float kSnowUVTile = 4096.0 / 24.0;

// Minimum lift. At exactly zero the skin is coincident with its source mesh
// and z-fights it invisible; a tenth of a unit clears that without reading as
// a coat, so a class slider at 0 is a flat sheet rather than a 1-unit layer.
static const float kMinSkinLift = 0.1;

// World width of the cornice roll on flat plates, and the band over which a
// surface standing below another counts as sheltered from snowfall.
static const float kCorniceRoll = 4.0;
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

	float s00 = DeformationMap.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = DeformationMap.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = DeformationMap.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = DeformationMap.Load(int3(t1.x, t1.y, 0)).x;

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
// pattern all live in SnowFields.hlsli (round 37), shared with the
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
	float s00 = DeformationMap.Load(int3(t0.x, t0.y, 0)).x;
	float s10 = DeformationMap.Load(int3(t1.x, t0.y, 0)).x;
	float s01 = DeformationMap.Load(int3(t0.x, t1.y, 0)).x;
	float s11 = DeformationMap.Load(int3(t1.x, t1.y, 0)).x;
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
};

#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER)
// Top-down object top-surface raster. The patch drapes over it (the VS places
// geometry, the PS clips the silhouette overhang); the skin PS uses it to
// separate its own rim wall from a bare object face.
Texture2D<float> ObjectTopRaw : register(t11);
// Cone-transformed snow surface over the same window: the angle of repose
// already applied, so the edge taper is one read instead of a ring walk.
Texture2D<float> ObjectSnowCone : register(t13);
#endif
#ifdef PATCH
Texture2D<float> ObjectSkinDepth : register(t12);
#endif

#if defined(PATCH) || defined(PSHADER) || defined(VSHADER) || defined(DOMAINSHADER)

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
#endif

#if (defined(VSHADER) || defined(HULLSHADER) || defined(DOMAINSHADER)) && defined(PATCH)

float PatchSkinDepth(float2 worldXY)
{
	float2 dims;
	ObjectSkinDepth.GetDimensions(dims.x, dims.y);
	float2 t = PatchTexel(worldXY, dims);
	int2 t0 = (int2)t;
	int2 t1 = min(t0 + 1, int2(dims) - 1);
	return max(max(ObjectSkinDepth.Load(int3(t0.x, t0.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t0.y, 0))),
		max(ObjectSkinDepth.Load(int3(t0.x, t1.y, 0)), ObjectSkinDepth.Load(int3(t1.x, t1.y, 0))));
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

	float top;
	float skinDepth;
	float skinEdgeMin;
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
		// midway down the facade, which the rim test then kills erratically
		// at tessellated density (sawtooth facade teeth). Both fall back to
		// the center sample so the whole cell resolves like the legacy grid
		// and dies or lives coherently.
		float tMin = min(min(t00, t10), min(t01, t11));
		float tMax = max(max(t00, t10), max(t01, t11));
		[flatten] if (tMin < -50000.0 || (tMax - tMin) > 100.0)
			top = PatchTop(worldXY);
		float s00 = PatchSkinDepth(base);
		float s10 = PatchSkinDepth(base + float2(kHeightTexel, 0.0));
		float s01 = PatchSkinDepth(base + float2(0.0, kHeightTexel));
		float s11 = PatchSkinDepth(base + float2(kHeightTexel, kHeightTexel));
		skinDepth = lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
		// Weakest lattice corner: a footprint boundary crossing this cell.
		skinEdgeMin = min(min(s00, s10), min(s01, s11));
	}
	else
	{
		top = PatchTop(worldXY);
		skinDepth = PatchSkinDepth(worldXY);
		skinEdgeMin = skinDepth;
	}
	float2 gridLocal = v.GridLocal;

	// Rim test: a vertex whose column towers over any neighbor column is
	// the top edge of a tall structure (roof or wall rim); its triangles
	// stretch down the facade as giant white sheets. valid neighbors only:
	// a sentinel neighbor (off the footprint) must NOT count as a rim;
	// that culls the patch's edge ring along every road chunk, punching
	// trench holes at road edges. Facade sheets still die: an off-footprint
	// vertex is killed by its own sentinel top (outline-spanning triangles
	// go with it), and within-footprint roof-to-ground drops are caught by
	// the height delta.
	float topXP = PatchTop(worldXY + float2(kHeightTexel, 0.0));
	float topXN = PatchTop(worldXY - float2(kHeightTexel, 0.0));
	float topYP = PatchTop(worldXY + float2(0.0, kHeightTexel));
	float topYN = PatchTop(worldXY - float2(0.0, kHeightTexel));
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
	bool rim = minNeighborTop < 1e8 && (top - minNeighborTop) > 100.0;
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
		rim = rim || length(topGrad) > 0.5;
	}
	// Wall-base de-jut: the last LIVE ring at the foot of a culled facade
	// still samples tops partway up the smeared ramp and rises as a jagged
	// rim along the wall. Clamping to the lowest valid neighbor plus a
	// normal-slope allowance flattens the rim to the ground it belongs to.
	[flatten] if (minNeighborTop < 1e8)
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
		float tallTop = PatchTop(worldXY + kTallRays[tallI]);
		[flatten] if (tallTop > -50000.0 && (tallTop - top) > 100.0)
			nearTall = true;
	}
	rim = rim || nearTall;

	// Neighborhood trample test: the patch lives only around trails. The
	// coarse 8-unit grid samples a 1.5-cell margin as a 16-ray star (at
	// radius 12 the rays sit 22.5 degrees apart, so even the thinnest trail
	// cannot slip between rays); dense tessellated vertices sit a unit or
	// two apart and a 5-tap cross covers their footprint.
	float aliveDeform = SampleDeformation(gridLocal);
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

	// Single-return structure: an early return inside a [branch] trips
	// fxc's X4000 and CI enforces zero warnings.
	[branch] if (top > -50000.0 && skinDepth >= 1.0 && !rim && aliveDeform >= 0.005)
	{
		// Bicubic, like the landscape shell; rounded trench walls.
		float deform = saturate(SampleDeformationSmooth(gridLocal));

		// Minimum snow floor: the legacy full-carve sank trampled floors
		// under the object, which the coarse 8-unit grid's interpolation
		// happened to hide; dense tessellated evaluation honors the sink
		// exactly and erased whole road-trail floors. Trampled floors now
		// hold a thin snow cover ABOVE the object, precision-padded with
		// distance so neither side ever z-fights; exposing the object
		// through worn floors is the Trench Floor See-Through slider's job.
		float depth = skinDepth * (1.0 - deform);
		float camDist = length(float3(worldXY, top) - ShellCameraPosAdjust.xyz);
		// The minimum floor tapers away where the raster data thins (the
		// footprint boundary): held at full strength there, the raised
		// floor ends in an 8-unit staircase rim along the footprint edge.
		// The weakest-corner term catches boundaries that jump 0-to-full
		// inside one texel, which the interpolated depth alone never sees.
		float floorMin = min(skinDepth, 0.8 + camDist * 0.004) * smoothstep(1.0, 4.0, skinDepth) * smoothstep(0.25, 2.0, skinEdgeMin);
		depth = max(depth, floorMin);

		// Churn: broken lumps on the carved walls. The room factor keeps the
		// dig under 80% of the cover above the minimum floor even at the
		// slider maximum, so lumps can never expose the object beneath; fully
		// trampled floors (depth = floorMin) stay smooth by the same term.
		float churnW = smoothstep(0.05, 0.5, deform) * saturate((depth - floorMin) / 10.0);
		[branch] if (ObjChurnHeightAmp > 0.01 && churnW > 0.001)
			depth += ChurnNoise(worldXY) * ObjChurnHeightAmp * churnW;
		v.WorldAbs = float3(worldXY, top + depth - 0.4);

		// Carved-surface shading normal from the SMOOTH deformation gradient;
		// the geometry carries the shape, this rounds the shading with the
		// same curve the depth uses. The churn term shades at vertex rate:
		// dense patch vertices sit 1-2 units apart near the camera.
		float2 grad = float2(
			SampleDeformationSmooth(gridLocal + float2(4.0, 0.0)) - SampleDeformationSmooth(gridLocal - float2(4.0, 0.0)),
			SampleDeformationSmooth(gridLocal + float2(0.0, 4.0)) - SampleDeformationSmooth(gridLocal - float2(0.0, 4.0))) / 8.0;
		float2 churnGrad = float2(0.0, 0.0);
		[branch] if (ObjChurnHeightAmp > 0.01 && churnW > 0.001)
		{
			const float cs = 3.0;
			churnGrad = float2(
				ChurnNoise(worldXY + float2(cs, 0.0)) - ChurnNoise(worldXY - float2(cs, 0.0)),
				ChurnNoise(worldXY + float2(0.0, cs)) - ChurnNoise(worldXY - float2(0.0, cs))) / (2.0 * cs) * ObjChurnHeightAmp * churnW;
		}
		v.NormalWS = normalize(float3(grad * skinDepth * 0.6 - churnGrad, 1.0));
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
	// patch does not otherwise use for shading.
	vsout.Coverage = StaticsDebugView != 0.0 ? v.Deform : 1.0;
	vsout.Flat = StaticsDebugView != 0.0 ? saturate(v.SkinDepth / 8.0) : 0.0;
	// The patch is exempt from the lift gates; its walls are real geometry.
	vsout.Lift = 1e6;
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
// Trench patch (PATCH define): the landscape shell's recipe applied to objects; a dense
// 8-unit grid (256x256 quads, +-1024 units around the camera) draped over
// the top-down object height raster and carved per vertex by the
// deformation map. real geometry: real silhouettes, floors that hold at
// every camera angle, no parallax. The skin dithers itself away over
// trails to hand off (see the PS).
VS_OUTPUT main(uint vertexID : SV_VertexID)
{
	static const float2 kCorners[6] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	uint quadIndex = vertexID / 6;
	float2 gridXY = float2(quadIndex % 256, quadIndex / 256) + kCorners[vertexID % 6];
	// WorldRow0.xy carries the snapped patch origin (see the CPU fill).
	float2 worldXY = WorldRow0.xy + gridXY * 8.0;
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
	float2 gridXY = float2(quadIndex % 256, quadIndex / 256) + kPatchCorners[vertexID % 4];
	TessControlPointPatch cp;
	cp.WorldXY = WorldRow0.xy + gridXY * 8.0;
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
		if (PatchTop(w) > -50000.0 && PatchSkinDepth(w) >= 1.0)
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
};

SkinLift ApplySkinLift(float3 worldBase, float3 nrmWS, float3 smoothWS, float isFlat)
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

	// Snow accumulates on up-facing surfaces (steep shingles and walls stay
	// bare, matching the vanilla projection's extent). flat meshes gate hard
	// on the raw normal so plank sides stay clean; rounded meshes ramp over
	// almost the whole up-facing range of the SMOOTHED normal.
	// The layer stays geometrically uncarved: trench relief is traced per
	// pixel in the PS instead.
	float upFacing = isFlat > 0.5 ? smoothstep(0.4, 0.7, nrmWS.z) : smoothstep(0.05, 0.85, smoothWS.z);
	float depth = depthBase * upFacing;

	// Geometry LOD: collapse the layer to nothing BEFORE the material dissolve
	// (SkinFadeStart/End) begins, so the hand-off to the object's own projected
	// snow has no silhouette left to pop. Range scales with class depth against
	// the depth sliders' 25-unit maximum.
	// Roads are exempt: their height must stay in step with the landscape
	// shell they meet at the verge, and that shell does not collapse.
	// Edge taper: the cone field already holds the highest snow surface the
	// angle of repose permits at each column, so the layer thins toward every
	// rim and keeps full depth in the middle. Sampling a grid transform is
	// isotropic; the ring walk it replaces approximated the same distance from
	// eight directions and faceted every curved rim into spikes.
	// Both classes read the field, but they read different SHAPES out of it.
	// The depth guard skips the read wherever the layer is already gone.
	float support = 1.0;
	float rimT = 1.0;
	[branch] if (HasObjectTop > 0.5 && LegacySkin < 0.5 && depth > 0.001)
	{
		// The field is seeded with the deepest class in play, so normalizing
		// by that recovers distance-to-rim: 0 at the rim, 1 in the interior.
		float coneSeed = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift);
		float steep = clamp(MoundSteepness, 0.5, 3.0);
		rimT = saturate(ObjectConeDepth(worldBase.xy) / coneSeed);
		float allowed;
		[flatten] if (isFlat > 0.5)
		{
			// Cornice. Snow on a thin plate has the cohesion to carry full
			// depth out to the rim and roll over in the last sliver; slumping
			// it across the plate would erase the overhang that makes a
			// snowed plank or eave read correctly. Quarter-circle rollover.
			// The roll is a FIXED world width, not a fraction of the ramp:
			// the ramp's length scales with depth, so a proportional roll is
			// wider than the plank itself at deep settings and the whole
			// plate turns into rollover.
			float rollFrac = saturate(kCorniceRoll * steep / coneSeed);
			float u = saturate(rimT / max(rollFrac, 1e-3));
			float toRim = 1.0 - u;
			rimT = u;
			allowed = depthBase * sqrt(saturate(1.0 - toRim * toRim));
		}
		else
		{
			// Slump at the angle of repose, which is what a rock wants.
			allowed = depthBase * rimT;
		}
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
		}
	}

	// The shape is settled here; everything past this point is distance LOD.
	float coverDepth = depth;

	// Geometry LOD: collapse the layer to nothing BEFORE the material dissolve
	// (SkinFadeStart/End) begins, so the hand-off to the object's own projected
	// snow has no silhouette left to pop. Range scales with class depth against
	// the depth sliders' 25-unit maximum.
	// Roads are exempt: their height must stay in step with the landscape
	// shell they meet at the verge, and that shell does not collapse.
	[branch] if (SkinHeightFadeEnd > 1.0 && LegacySkin < 0.5)
	{
		float collapseEnd = SkinCollapseEnd(depthBase);
		float camDist = length(worldBase - ShellCameraPosAdjust.xyz);
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

	SkinLift o;
	o.WorldAbs = worldBase + liftWS * depth;
	o.Depth = depth;
	o.CoverDepth = coverDepth;
	o.RimT = rimT;
	o.Support = support;
	o.UpFacing = upFacing;
	return o;
}

// Displaced snow shades by the smooth surface it forms, not the flat face
// beneath; undisplaced vertices keep the raw normal.
float3 SkinShadingNormal(float3 nrmWS, float3 smoothWS, float isFlat, float depth, float rimT)
{
	float depthBase = max(lerp(RoundedDepth, ObjectsDepth, isFlat), kMinSkinLift);
	[branch] if (isFlat > 0.5)
	{
		// Flat snow shades by the plate it sits on — a per-plank gradient
		// stamps the same lighting onto every instance. The cornice roll is
		// the exception: geometry that curves over but shades flat reads as a
		// cut edge, so the last sliver before the rim bends toward the
		// smoothed normal and nothing inboard of it changes.
		float roll = 1.0 - smoothstep(0.0, 0.35, rimT);
		return normalize(lerp(nrmWS, smoothWS, roll * 0.8));
	}
	return normalize(lerp(nrmWS, smoothWS, saturate(depth / max(depthBase, 0.01)) * 0.85));
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
			inflateMS = smoothEntry.xyz;
	}

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
	return v;
}

#if !defined(SNOW_TESS)
VS_OUTPUT main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	SkinLift lift = ApplySkinLift(v.WorldBase, v.NormalWS, v.SmoothWS, v.Flat);

	float3 rel = lift.WorldAbs - ShellCameraPosAdjust.xyz;
	float3 prevRel = lift.WorldAbs - ShellCameraPreviousPosAdjust.xyz;

	VS_OUTPUT vsout;
	vsout.Position = mul(CameraViewProj, float4(rel, 1.0));
	vsout.CurrentClip = mul(CameraViewProjUnjittered, float4(rel, 1.0));
	vsout.PreviousClip = mul(CameraPreviousViewProjUnjittered, float4(prevRel, 1.0));
	vsout.WorldPos = rel;
	vsout.NormalWS = SkinShadingNormal(v.NormalWS, v.SmoothWS, v.Flat, lift.Depth, lift.RimT);
	// raw normal Z, interpolated; the PS runs the up-facing smoothstep per
	// pixel. Thresholding here makes low-poly rocks flip whole FACES between
	// snowed and bare; thresholding the interpolated normal varies smoothly.
	// Debug view: smuggle the two lift masks through the shading interpolants.
	[flatten] if (StaticsDebugView > 2.5)
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
};

TessControlPoint main(VS_INPUT input)
{
	SkinVertex v = BuildSkinVertex(input);
	TessControlPoint cp;
	cp.WorldBase = v.WorldBase;
	cp.NormalWS = v.NormalWS;
	cp.SmoothWS = v.SmoothWS;
	cp.Flat = v.Flat;
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

	// The lift is evaluated HERE, per generated vertex: the up-facing mask and
	// the edge taper get tessellated density instead of being interpolated
	// across a source face.
	SkinLift lift = ApplySkinLift(worldBase, normalWS, inflateWS, isFlat);
	float3 worldAbs = lift.WorldAbs;
	float2 gridLocal = worldAbs.xy - GridOrigin;
	normalWS = SkinShadingNormal(normalWS, inflateWS, isFlat, lift.Depth, lift.RimT);

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
	[flatten] if (StaticsDebugView > 2.5)
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
	return vsout;
}
#endif

#ifdef PSHADER



// Two-plane projection blend, on the SAMPLES. Flat-topped pixels never touch
// the side plane, so the second set of stochastic taps is only paid for on
// slopes and rims.
float4 SampleSnowPlanar(Texture2D<float4> tex, SnowTaps topTaps, SnowTaps sideTaps, float sideWeight)
{
	float4 c = SampleSnowMap(tex, topTaps);
	[branch] if (sideWeight > 0.001)
		c = lerp(c, SampleSnowMap(tex, sideTaps), sideWeight);
	return c;
}

// --- Parallax self-shadow. Mirrors SnowShell.hlsl; keep the two in step. ---


// Two-plane occlusion, blended on the RESULTS. Unlike the sample blend above
// this cannot share one ray: each projection has its own uv axes, so the
// light resolves to a different 2D direction in each. Flat-topped pixels skip
// the side plane entirely, as with SampleSnowPlanar.
float SnowParallaxOcclusionPlanar(SnowTaps topTaps, SnowTaps sideTaps, float sideWeight,
	float2 lightUVTop, float2 lightUVSide, float mipTop, float mipSide,
	float quality, float noise, DisplacementParams params)
{
	float o = SnowParallaxOcclusion(topTaps, lightUVTop, mipTop, quality, noise, params);
	[branch] if (sideWeight > 0.001)
		o = lerp(o, SnowParallaxOcclusion(sideTaps, lightUVSide, mipSide, quality, noise, params), sideWeight);
	return o;
}


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
#	ifndef PATCH
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

	return lerp(lerp(s00, s10, f.x), lerp(s01, s11, f.x), f.y);
}

PS_OUTPUT main(VS_OUTPUT input)
{
	float2 motionVector = float2(-0.5, 0.5) * (input.CurrentClip.xy / input.CurrentClip.w - input.PreviousClip.xy / input.PreviousClip.w);

	float3 normalWS = normalize(input.NormalWS);
	float2 worldXY = GridOrigin + input.GridLocal;
	float pixelDist = length(input.WorldPos);

	// Rim wall: the band where a lifted cap reaches back down to the object's
	// edge is near-vertical whatever the depth (its geometry is the object's
	// own side face with the top edge dragged up), so the steepness gates
	// below erase it and the cap loses its side. The object's top raster
	// separates the two cases exactly: shell ABOVE the object's own top
	// surface is rim wall, shell at or below it is a bare object face.
	// View-independent, and it leaves house walls and boulder flanks (both
	// far below their object's top) to the gates.
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
	// Geometric steepness gate; one fix for three symptoms (wall fog,
	// boulder-flank sheets, trunk-bark streaks): on huge low-poly triangles
	// the interpolated normal smears one top vertex's up-ness down the whole
	// face, and sloped faces (30-60 degrees) sail over the vertical sliver
	// cull below. The derivative normal knows each pixel's true facing:
	// snow sheds off anything steeper than ~65 degrees regardless of
	// interpolation. The band sits below the interpolated gate's range, so
	// rounded snow edges (z 0.4+) stay interpolation-shaped and per-face
	// blockiness cannot return. The PATCH is exempt: its trench walls are
	// legitimately steep real geometry.
	float3 dPosX = ddx(input.WorldPos);
	float3 dPosY = ddy(input.WorldPos);
	float3 geoFacing = normalize(cross(dPosY, dPosX));
	// World units spanned by this pixel. Every LOD term below keys off it
	// rather than off camera distance, so they track resolution and FOV.
	float footprint = length(abs(dPosX) + abs(dPosY));
	float liftBase = max(lerp(RoundedDepth, ObjectsDepth, input.Flat), kMinSkinLift);

	// Facing LOD. The interpolated normal over-reports up-ness on low-poly
	// meshes (one top vertex's up-ness smeared down a whole face), which is
	// why a distant rock reads as solid white: every flank passes the gate
	// below. geoFacing is the true face orientation. It was rejected for near
	// coverage because it is constant per triangle and quantizes rims into
	// sawtooth, but that is a NEAR-field artifact: once a pixel spans the
	// taper the facets are sub-pixel, and the face normal is the only slope
	// signal left. Handover scales with the taper's own world length
	// (coneSeed / steepness), so it follows the depth and repose sliders.
	// The blend is capped short of 1: interpolation always contributes, which
	// keeps the facet contours soft through the transition.
	float coneRamp = max(max(RoundedDepth, ObjectsDepth), kMinSkinLift) / clamp(MoundSteepness, 0.5, 3.0);
	float faceLOD = kFacingLODMax * SkinDistantBareness * smoothstep(0.5, 2.0, footprint / max(coneRamp, 1.0));
	[branch] if (faceLOD > 0.001)
	{
		// cross() handedness is not reliable here (the trench gate below takes
		// abs for the same reason); align to the shading normal before reading z.
		float geoUp = geoFacing.z * (dot(geoFacing, normalWS) < 0.0 ? -1.0 : 1.0);
		pixelCoverage = smoothstep(0.4, 0.7, lerp(input.Coverage, geoUp, faceLOD));
	}

	// Coverage follows the layer's own HEIGHT, not the geometric face normal.
	// geoFacing is a screen-derivative of world position and therefore
	// constant across a triangle, so thresholding it quantizes coverage per
	// triangle and tears every rim into sawtooth teeth at any tessellation
	// density. Interpolated lift varies smoothly, and the edge taper already
	// drives it to zero on rims and vertical faces, so walls stay bare
	// without a facing test.
	// Narrow band, and jittered in world space. A wide ramp puts a broad area
	// into partial alpha, which the dither resolves into the translucent film
	// that used to sheet down house walls; and a clean threshold on a linearly
	// interpolated field traces the mesh's own polygons, so the contour comes
	// out faceted. The jitter breaks that contour without widening the band.
	float liftEdge = 0.06 * liftBase * (0.6 + 0.8 * CoverageNoise(worldXY * 3.0));
	// Rim contour LOD. The band is a contour of an interpolated field, so its
	// screen width is liftEdge / fwidth(Lift): it thins below a pixel and
	// averages into the blanket while the taper still has run left. Push the
	// contour inboard to hold roughly a pixel, keeping the partial-alpha width
	// FIXED - widening that is what dithers into a translucent film. Capped,
	// because the taper is all the range this field has: past it the facing
	// LOD above is the only source of bare surface.
	// Not scaled by SkinDistantBareness: that slider tunes the facing handover
	// in the far field, and sharing it here silently drops the contour below a
	// pixel (its whole point) at any setting that suits the far field.
	float liftBand = 0.45 * liftEdge;
	float liftEdgeLOD = min(max(liftEdge, kRimBandPx * fwidth(input.Lift)), kRimBandMax * liftBase);
	float liftCoverage = smoothstep(liftEdgeLOD - liftBand, liftEdgeLOD, input.Lift);
	pixelCoverage *= liftCoverage;
#	endif
	// Applied after every steepness multiply; the rim wall is exempt from all
	// of them.
	pixelCoverage = max(pixelCoverage, shoulderWall);
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
	bool carveObject = ObjectTrenches > 0.5 || LegacySkin > 0.5;
	float2 trenchGridLocal = input.GridLocal;
	float3 viewDirWS = normalize(input.WorldPos);
	// Ray parameter (world units along the view ray) to the parallax hit;
	// 0 means no carve; drives the SV_Depth push at the end.
	float trenchHitS = 0.0;
#	ifndef PATCH
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
	// seam blends below decide.
	coverageAlpha *= max(smoothstep(-0.05, 0.1, input.Coverage), shoulderWall);

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
	// the camera (round 31: trench walls crawled under camera-only motion
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
	// blanket. Two constructions, near to far.
	//
	// PAIR-3 CONTEST (HEIGHT-BLEND-PLAN, near field): where the landscape
	// shell VISIBLY renders behind this pixel (pre-vs-post shell depth
	// divergence - the one gate that can only ever dissolve snow into snow;
	// a height band alone could dissolve the skin over its own mesh and
	// expose the bare road beneath), the cut is geometric: the skin
	// survives where its surface stands above the blanket surface
	// reconstructed along the view ray, the crossing displaced by the
	// world-anchored grain so the meeting line runs in grain fingers
	// instead of a level contour. Both sides sample the SAME snow field -
	// a two-sided grain difference cancels exactly at the crease - so the
	// one-sided displacement IS the raggedness. No BorderNoise here: the
	// cut belongs on the visible crease (the round-10 touchdown lesson),
	// grain supplies the wander. Committed 0/1 (Border Dithering ON keeps
	// a dust tail below the crossing, outward-only) because every .w
	// output feeds the deferred temporal resolve, which re-dithers any
	// partial alpha whatever shaped it. Reconstruction error at grazing
	// angles is self-correcting: it grows with the ray gap, and a large
	// gap means the skin stands proud and wins outright anyway.
	//
	// FALLBACK (far field, no height map, or shell not visibly behind):
	// the analytic band vs the terrain window's shell top (Border
	// Smoothness / Border Noise dials, one-sided shaping) times the smooth
	// SnowSnowFade ray band - the prior construction, unchanged, and still
	// the whole story for pair 4's bare-land hand-off.
	float postShellZ = SharedData::GetScreenDepth(ShellDepthCopy.Load(int3(input.Position.xy, 0)));
	float preShellZ = SharedData::GetScreenDepth(SceneDepth.Load(int3(input.Position.xy, 0)));
	float skinZ = input.CurrentClip.w;
	float pixelAbsZ = input.WorldPos.z + ShellCameraPosAdjust.z;
	bool shellBehind = preShellZ - postShellZ > 1.0;
	float contestFade = (SnowSnowFade > 0.01 && HasSnowHeight > 0.5 && shellBehind)
	                        ? 1.0 - smoothstep(1024.0, 2048.0, pixelDist)
	                        : 0.0;
	float seamTotal = 1.0;
	float3 groundData = SampleTerrainStatics(input.GridLocal);
	[branch] if (contestFade < 0.999)
	{
		[flatten] if (groundData.x > -50000.0)
		{
			float groundShellZ = groundData.x + max(groundData.y, 0.0);
			// Pinned band, decoupled from the Border Noise / Border Smoothness
			// sliders (round 31, Josef's finding: noise 0 + smoothness 64 is
			// the look for THIS seam - noise detaches the band from the real
			// meeting line, and the wide band gives the soft rise of ground
			// snow up the object - while the landscape class border wants the
			// sliders). Values are the slider math at exactly 0 / 64.
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
		[branch] if (SnowSnowFade > 0.01 && shellBehind)
			seamTotal *= smoothstep(0.0, max(SnowSnowFade, 1.0), postShellZ - skinZ);
	}
	[branch] if (contestFade > 0.001)
	{
		float shellSurfZ = ShellCameraPosAdjust.z + input.WorldPos.z * (postShellZ / max(skinZ, 1e-3));
		float grainSkin = SampleSnowHeight(ComputeSnowTapsNoGrad(edgeSnowUV, worldXY), 0.0.xx, edgeSnowMip);
		// One grain scale and one dust reach across every snow border
		// (kEdgeGrainAmp and the landscape edge's tail).
		const float kSeamGrainAmp = 2.0;
		const float kSeamDustReach = 2.0;
		float margin = pixelAbsZ - shellSurfZ + (grainSkin - 0.5) * kSeamGrainAmp;
		float seamContest = BorderStyle.x > 0.5
		                        ? saturate(margin / kSeamDustReach + 1.0)
		                        : (margin >= 0.0 ? 1.0 : 0.0);
		seamTotal = lerp(seamTotal, seamContest, contestFade);
	}
	coverageAlpha *= seamTotal;
	dbgSeam *= seamTotal;

	// Shading continuity across the meeting line (round 30, "still very
	// edgy"): with the cut committed, what remains visible of the seam is
	// the LIGHTING discontinuity - the skin's macro normal against the
	// blanket's. Ease the skin's normal toward the blanket's analytic
	// surface normal through the last units above the blanket top, so the
	// two surfaces agree by the line and the crease reads as one snowfield.
	// Micro detail stays continuous by construction: both sides sample the
	// same world-anchored snow normal map, applied after this. Keyed to the
	// analytic height field, not the ray gate - the gate's boundary (the
	// blanket's silhouette behind the pixel) would print its own edge into
	// a normal blend. Blanket depth > 0.5 keeps pair 4's bare-ground
	// hand-off out of this: flattening the rim toward bare dirt is not
	// continuity, there is no blanket to agree with.
	[branch] if (HasSnowHeight > 0.5 && groundData.x > -50000.0 && groundData.y > 0.5 && pixelDist < 2048.0)
	{
		float blanketTopZ = groundData.x + max(groundData.y, 0.0);
		float dzTop = pixelAbsZ - blanketTopZ;
		// TWO-SIDED thin band, up-facing pixels only (round 32, settled by
		// the RenderDoc receiver replay): the one-sided full-strength blend
		// hijacked everything BELOW the blanket top - carved walls, floors,
		// and the rock's sun-facing flank at the seam, whose true normal
		// catches the low sun exactly like the blanket rim beside it.
		// Flattening it printed a dim skin stripe against a glowing rim -
		// the "bright seam band". Flanks and recesses keep their normals;
		// only near-top, up-facing pixels ease into the blanket.
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
	{
		float2 clipDims;
		ObjectTopRaw.GetDimensions(clipDims.x, clipDims.y);
		float2 clipLocal = (worldXY - HeightWindowCenter) / HeightHalfExtent;
		float2 clipUV = float2(clipLocal.x * 0.5 + 0.5, 0.5 - clipLocal.y * 0.5);
		float2 clipT = clamp(clipUV * clipDims - 0.5, 0.0, clipDims.x - 1.001);
		int2 c0 = (int2)clipT;
		float2 cf = clipT - c0;
		int2 c1 = min(c0 + 1, int2(clipDims) - 1);
		float top00 = ObjectTopRaw.Load(int3(c0.x, c0.y, 0));
		float top10 = ObjectTopRaw.Load(int3(c1.x, c0.y, 0));
		float top01 = ObjectTopRaw.Load(int3(c0.x, c1.y, 0));
		float top11 = ObjectTopRaw.Load(int3(c1.x, c1.y, 0));
		float maxTop = max(max(top00, top10), max(top01, top11));
		// Per-texel drop vs the supporting top, sentinel-clamped.
		float4 drops = min(maxTop - float4(top00, top10, top01, top11), 200.0);
		float drop = lerp(lerp(drops.x, drops.y, cf.x), lerp(drops.z, drops.w, cf.x), cf.y);
		coverageAlpha *= 1.0 - smoothstep(8.0, 24.0, drop);
	}
#	endif

	// Distance dissolve: from SkinFadeStart the skin stochastically thins
	// back into the object's own material, fully gone by SkinFadeEnd (the
	// capture range); distant objects keep their real look instead of
	// turning blank white. Glacier/iceberg captures are exempt: their own
	// baked snow never matches the shell, so the skin persists at every
	// loaded distance (geometry still collapses to flat paint by
	// SkinHeightFadeEnd).
	[flatten] if (FadeExempt < 0.5)
		coverageAlpha *= 1.0 - smoothstep(SkinFadeStart, SkinFadeEnd, pixelDist);

	// Captured before the override: mode 2 renders the value the dither sees.
	float dbgAlpha = coverageAlpha;
	// Debug view: full visibility; the dither must not hide geometry the
	// diagnosis needs to see.
	[branch] if (StaticsDebugView != 0.0)
		coverageAlpha = 1.0;
	float screenNoise = Random::InterleavedGradientNoise(input.Position.xy, SharedData::FrameCount);
	if (screenNoise * screenNoise >= coverageAlpha)
		discard;

	// Snow texture taps; shared by albedo, normal and RMAOS, sampled at the
	// parallax-corrected position so the texture rides the relief. Steep
	// drape sides re-project along the facing wall plane: the top-down
	// projection stretches down a puffed shell's flanks.
	// Two projections, blended as SAMPLES rather than as coordinates. Lerping
	// the UVs produces a coordinate field that belongs to neither plane, so
	// the whole transition band smears; the sides are also where the shell's
	// rim lives, which is where that smear reads as streaks.
	float2 snowUV = (SnowUVOffset + trenchGridLocal) / kSnowUVTile;
	float snowSteepness = smoothstep(0.55, 0.25, abs(normalWS.z));
	float snowWorldZAbs = input.WorldPos.z + ShellCameraPosAdjust.z;
	// Captured, not recomputed: normalWS is perturbed further below (berm
	// ridge, normal map), and the parallax shadow must resolve the light into
	// the SAME plane these uvs were built on.
	bool snowSideDropsX = abs(normalWS.x) > abs(normalWS.y);
	float2 snowSidePlane = snowSideDropsX ? float2(worldXY.y, snowWorldZAbs) : float2(worldXY.x, snowWorldZAbs);
	float2 snowUVSide = (SnowUVOffset + snowSidePlane) / kSnowUVTile;
	float bumpFade = 1.0 - smoothstep(600.0, 2200.0, pixelDist);
	// The distance fade WITHOUT the crust flattening applied: the frost
	// crystal replacing the powder grain must not fade with it.
	const float bumpFadeRaw = bumpFade;
	// Spell marks (round 35, landscape parity): crust flattens the powder
	// grain here; albedo/polish/grazing terms follow below. Stable grid
	// position for the fetch (round-31 lesson).
	float crustAmount = saturate(SampleCrust(input.GridLocal) * SpellShading.y);
	bumpFade *= lerp(1.0, 1.0 - saturate(SpellShading.w), crustAmount);
	SnowTaps snowTaps = ComputeSnowTaps(snowUV, worldXY);
	SnowTaps snowTapsSide = ComputeSnowTaps(snowUVSide, snowSidePlane);
	// Uniform flow: the parallax shadow branch below is divergent, and
	// derivatives taken inside it would be garbage at its edges.
	float snowHeightMip = SnowHeightMip(snowUV);
	float snowHeightMipSide = SnowHeightMip(snowUVSide);

	// Object trench detail: shading-only berm ridge along trails; also the
	// compaction weight's berm term. Geometry berm waits for the skin
	// rework.
	float bermC = 0.0;
	[branch] if (ObjBermHeightAmp > 0.005 || CompactLook.x > 0.001)
		bermC = BermField(trenchGridLocal);
	[branch] if (ObjBermHeightAmp > 0.005 && bermC > 0.003)
	{
		const float bStep = 4.0;
		float2 bermGrad = float2(
			BermShape(BermField(trenchGridLocal + float2(bStep, 0.0))) - BermShape(BermField(trenchGridLocal - float2(bStep, 0.0))),
			BermShape(BermField(trenchGridLocal + float2(0.0, bStep))) - BermShape(BermField(trenchGridLocal - float2(0.0, bStep)))) / (2.0 * bStep);
		float bermDepth = min(lerp(RoundedDepth, ObjectsDepth, input.Flat), 12.0);
		// Centre-masked rather than per-tap: this berm is shading-only, and
		// the mask's job is just to keep the ridge off the dug floor.
		normalWS = normalize(normalWS + float3(-bermGrad * saturate(1.0 - pixelDeform) * bermDepth * ObjBermHeightAmp, 0.0));
	}

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
	float sunShadow;
	[branch] if (CrispShadows > 0.5)
	{
		// Full-resolution comparison PCF; same path as the terrain shell.
		// (Round 32: the round-31 seamShadowLift receiver raise is REVERTED -
		// the RenderDoc replay proved no cascade shadow was missing at the
		// seam, so the lift only risked boundary drift.)
		sunShadow = worldShadow * SnowShadow::GetCascadeShadow(input.WorldPos, normalWS, 1.0, uint2((uint)BorderStyle.z, (uint)BorderStyle.w));
	}
	else
	{
		float detailedShadow;
		float dynamicShadow = ShadowSampling::GetLightingShadow(input.WorldPos, detailedShadow);
		sunShadow = worldShadow * min(dynamicShadow, detailedShadow);
	}
	// Heightfield self-shadowing, the landscape shell's 5-tap horizon march
	// (round 35): hills, berms and drift rims cast the same soft shadows onto
	// object snow as onto the ground beside it, and a trench's own rim
	// darkens its interior. Same tap ring, same carved-surface rule; the
	// melt term reads the wide exclusion field alone (no near mask bound
	// here). Object tops from the skin's own raster window join the horizon.
	float farShadowT = smoothstep(6000.0, 15000.0, pixelDist);
	[branch] if (sunShadow > 0.01 && satNdotL > 0.001 && L.z > 0.01)
	{
		static const float kMarchDist[5] = { 28.0, 70.0, 170.0, 420.0, 1000.0 };
		float sunLen2D = max(length(L.xy), 1e-4);
		float sunTan = L.z / sunLen2D;
		float2 stepDir = L.xy / sunLen2D;
		float surfZ = input.WorldPos.z + ShellCameraPosAdjust.z;
		float horizonTan = -10.0;
		// The top raster stores only the HIGHEST surface per texel, so under
		// a multi-level object's overhang it records the deck ABOVE the
		// receiver and every tap reads "inside a hill" — full shadow in
		// raster-texel steps on surfaces plainly in the sun (round 37,
		// Josef's glacier-ledge evidence). The raster cannot see under
		// roofs: where it stands well above the surface being shaded, drop
		// its term and let the cascades/SSS own the shading here.
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
				// 12 units: on normal tops the raster sits AT or BELOW the
				// lifted skin surface (selfTop - surfZ is negative by the
				// skin depth), so even a small positive margin only fires
				// under genuine upper decks — 32 missed low ledges (Josef's
				// cliff evidence).
				if (selfTop > -50000.0 && selfTop > surfZ + 12.0)
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
						// shadow at any low sun (round 36, Josef's screens).
						sh = topH + 2.0;
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
				sampleDepth = CarveProfile(sampleDeform, sampleDepth) +
				              BermShape(sampleBerm) * saturate(1.0 - sampleDeform) * sampleDepth * BermHeightAmp;
				// Sentinel terrain contributes a hugely negative horizon: a
				// no-op through the max below, same as the landscape's edge.
				sh = st.x + sampleDepth + Undulation(GridOrigin + sampleLocal) * saturate(sampleDepth / 8.0);
			}
			horizonTan = max(horizonTan, (sh - surfZ) / d);
		}
		float soft = lerp(0.06, 0.35, farShadowT);
		sunShadow *= lerp(smoothstep(-0.12 - (soft - 0.06) * 2.0, soft, sunTan - horizonTan), 1.0, 0.7 * farShadowT);
	}
	// Screen-Space Shadows: same long-range term bare ground multiplies in,
	// distance-blended past the cascades like the landscape shell (the SSS
	// march ran on the PREPASS depth; near, it belongs to the surface
	// UNDER the skin, and the crisp cascades already cover the skin).
	[branch] if (ScreenSpaceShadowsActive > 0.5)
	{
		float sssBlend = smoothstep(4000.0, 9000.0, pixelDist);
		sunShadow *= lerp(1.0, ScreenSpaceShadows::GetScreenSpaceShadow(input.Position.xyz, float2(0.0, 0.0), 0.0), sssBlend);
	}
	// Parallax self-shadow on the snow grain, same term and constants as the
	// terrain shell so object snow and ground snow shadow identically across
	// the SnowSnowFade cross-fade. Object snow needs it in both projections:
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
		sunShadow *= lerp(1.0, parallaxShadow, bumpFade);
	}

	// Sun BRDF + indirect lobes through CS's own PBR path (SnowShading.hlsli,
	// ROUTING-ROADMAP M1); same call as the terrain shell so object snow and
	// ground snow shade identically across the SnowSnowFade cross-fade.
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
	// same source as the terrain shell keeps the SnowSnowFade cross-fade flat.
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
		preLit = float3(saturate(input.Coverage), saturate(input.Flat), 0.0);
#else
		[branch] if (StaticsDebugView > 2.5)
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
#	ifndef PATCH
	// Depth: unchanged pixels echo the rasterized depth; carved pixels
	// project the parallax hit point through the same (jittered) matrix
	// the VS used, so the trench floor is real to the z-buffer.
	psout.Depth = input.Position.z;
	[branch] if (trenchHitS > 0.0)
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
