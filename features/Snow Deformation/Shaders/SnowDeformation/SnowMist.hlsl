/**
 * @file SnowMist.hlsl
 * @brief The airborne half of the bow wave: powder thrown off the crest.
 *
 * ROADMAP #35 phase TWO, and it only exists because phase one does. Real
 * displaced snow separates: the bulk is PUSHED (the crest, in the deformation
 * map's deposit channel) and the fine powder aerosolises off the top of that
 * pushed mass. The retired Stage 4 spray built this half with the other half
 * missing - mist with nothing pushing it is smoke by definition - so this
 * rides the crest rather than standing alone.
 *
 * THREE RULES, all learned the hard way, none negotiable:
 *
 * 1. NOTHING IS BORN. Every sprite's slot is PARAMETRIC - derived from its
 *    index and the wave's current position - so no sprite has a history and
 *    nothing can be left behind. A spawned puff stays where it was born while
 *    the walker moves on, and a row of those is the "choo-choo train" of
 *    stationary plumes marking a path.
 *
 * 2. SNOW DOES NOT RISE. Smoke rises because it is buoyant; powder is denser
 *    than air - it goes where it is thrown and falls. The retired spray gave
 *    every puff an upward kick, which manufactured smoke by construction.
 *    Here the height is a fixed CURTAIN PROFILE, tallest where the leg is
 *    driving through and falling away outward; nothing climbs.
 *
 * 3. THE MATERIAL IS WORLD-ANCHORED even though the slots are not. Opacity,
 *    size and breakup are all driven by noise sampled at the sprite's WORLD
 *    position, so as the actor advances the powder appears to flow THROUGH
 *    the curtain rather than ride along inside it like a rigid attachment.
 *    This is the same trick that made the crest's chunks read as snow.
 *
 * Lit as CS lights its own effect particles (Effect.hlsl's GetLightingColor),
 * reassembled from the shared modules the shells already route through.
 * Cluster lookup is done from this pass's own CameraViewProj rather than
 * FrameBuffer, which is not bound here.
 */

#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"

#ifdef PSHADER
#	include "Common/ShadowSampling.hlsli"
#	include "SnowDeformation/SnowShadow.hlsli"
#	include "LightLimitFix/LightLimitFix.hlsli"
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

#define MIST_MAX_WAVES 16
#define MIST_SPRITES 24
#define MIST_VERTS_PER_SPRITE 6

cbuffer MistCB : register(b0)
{
	row_major float4x4 CameraViewProj;
	float4 MistCameraPosAdjust;
	/// x = live wave count, y = opacity, z = curtain height (x radius),
	/// w = brightness
	float4 MistParams;
	/// x = reach (forward extent, shared with the crest), y = forward bias,
	/// z = lateral spread, w > 0.5 = cascade atlas bound
	float4 MistShape;
	/// xy = the sun cascades' REAL atlas slices; zw > 0.5 = LLF bound
	float4 MistSlices;
	/// xyz = foot position, w = push radius
	float4 MistFootRad[MIST_MAX_WAVES];
	/// xy = unit travel direction, z = strength 0-1, w spare
	float4 MistDirStr[MIST_MAX_WAVES];
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 Local : TEXCOORD0;     ///< -1..1 across the sprite
	float3 WorldRel : TEXCOORD1;  ///< rebased world position of the sprite
	float2 Fade : TEXCOORD2;      ///< x = alpha, y = per-sprite seed
};

float MistHash(float a_x)
{
	return frac(sin(a_x * 127.1) * 43758.5453);
}

/// World-anchored value noise. The powder's material lives here: sprites move
/// with the actor, but what they SHOW is sampled from the world, so the snow
/// reads as flowing through the curtain rather than travelling inside it.
float MistNoise(float2 p)
{
	// Wrapped so frac() stays inside float precision at world-scale inputs.
	p -= 512.0 * floor(p / 512.0);
	float2 i = floor(p);
	float2 f = p - i;
	f = f * f * (3.0 - 2.0 * f);
	float a = MistHash(i.x * 1.7 + i.y * 31.3);
	float b = MistHash((i.x + 1.0) * 1.7 + i.y * 31.3);
	float c = MistHash(i.x * 1.7 + (i.y + 1.0) * 31.3);
	float d = MistHash((i.x + 1.0) * 1.7 + (i.y + 1.0) * 31.3);
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

#ifdef VSHADER
VS_OUTPUT main(uint a_vertexID : SV_VertexID)
{
	VS_OUTPUT vsout;

	const uint sprite = a_vertexID / MIST_VERTS_PER_SPRITE;
	const uint corner = a_vertexID % MIST_VERTS_PER_SPRITE;
	const uint wave = sprite / MIST_SPRITES;
	const uint slot = sprite % MIST_SPRITES;

	const float4 footRad = MistFootRad[wave];
	const float4 dirStr = MistDirStr[wave];
	const float radius = footRad.w;
	const float strength = dirStr.z;

	// PARAMETRIC slot - index in, position out, no state. The same slot is
	// the same place relative to the wave on every frame it exists.
	const float h1 = MistHash(slot * 1.73 + wave * 0.37);
	const float h2 = MistHash(slot * 3.11 + wave * 0.71);
	const float h3 = MistHash(slot * 5.37 + wave * 1.13);
	const float h4 = MistHash(slot * 7.19 + wave * 1.79);

	const float2 fwd = dirStr.xy;
	const float2 side = float2(-fwd.y, fwd.x);

	// Along-track: mostly ahead of the foot, a little trailing. Reach is the
	// crest's own crank, so the powder ends where the pushed snow ends.
	const float f = lerp(-0.25, 1.15, h1) * max(MistShape.x, 0.25);
	// Across-track: the V. Narrow at the front where the leg is driving,
	// spreading toward the back as the powder is shed sideways.
	const float widen = lerp(1.25, 0.45, saturate(f));
	const float a = (h2 * 2.0 - 1.0) * widen * max(MistShape.z, 0.05);

	float3 world;
	world.xy = footRad.xy + fwd * (f * radius) + side * (a * radius);
	// CURTAIN PROFILE, not a launch: tallest just ahead of the leg where the
	// snow is being driven, falling away toward the edges. Nothing here rises
	// over time, because nothing here HAS a time.
	const float lift = (0.30 + 0.85 * sin(saturate(f) * 3.14159265)) *
	                   (0.45 + 0.9 * h3) * (1.0 - 0.55 * abs(a));
	world.z = footRad.z + lift * radius * MistParams.z;

	const float3 rel = world - MistCameraPosAdjust.xyz;
	const float3 toEye = normalize(-rel);
	const float3 right = normalize(cross(float3(0.0, 0.0, 1.0), toEye) + 1e-4);
	const float3 up = cross(toEye, right);

	const uint quadIndex = (corner == 0) ? 0 : (corner == 1 || corner == 4) ? 1 :
	                       (corner == 2 || corner == 3)                     ? 2 :
	                                                                          3;
	const float2 local = float2((quadIndex & 1) ? 1.0 : -1.0, (quadIndex & 2) ? 1.0 : -1.0);

	// Puffs grow toward the trailing edge: powder disperses as it sheds.
	const float size = radius * (0.22 + 0.30 * h4) * lerp(1.5, 0.8, saturate(f));

	// Dissipation: thin at the front lip, thinner still past the crest and
	// out at the flanks, so the curtain has no hard boundary anywhere.
	float alpha = strength * MistParams.y;
	alpha *= saturate(1.0 - smoothstep(0.85, 1.15, saturate(f)));
	alpha *= 1.0 - smoothstep(0.55, 1.0, abs(a));
	alpha *= 0.35 + 0.65 * h4;
	// The forward-bias crank thins the trailing half rather than moving it,
	// matching how it reads on the crest.
	alpha *= lerp(1.0, saturate(f * 1.4 + 0.25), MistShape.y);

	vsout.Position = mul(CameraViewProj, float4(rel + (right * local.x + up * local.y) * size, 1.0));
	vsout.Local = local;
	vsout.WorldRel = rel;
	vsout.Fade = float2(alpha, h3 * 97.0);
	return vsout;
}
#endif

#ifdef PSHADER
float4 main(VS_OUTPUT input) : SV_TARGET
{
	const float r = length(input.Local);
	float soft = smoothstep(1.0, 0.15, r);

	// WORLD-anchored material. The sprite's slot travels with the actor; what
	// it shows is sampled where it currently stands, so powder flows through
	// the curtain instead of riding inside it.
	const float2 worldXY = input.WorldRel.xy + MistCameraPosAdjust.xy;
	const float coarse = MistNoise(worldXY / 26.0);
	const float fine = MistNoise(worldXY / 9.0 + input.Fade.y);
	soft *= saturate(0.25 + 1.05 * coarse * (0.55 + 0.75 * fine));

	float alpha = soft * input.Fade.x;
	clip(alpha - 0.004);

	// Lit by CS's own effect-particle recipe. Airborne powder has no surface,
	// so ambient takes a straight-up normal and the sun is unwrapped by a
	// fixed forward-scatter - a Lambert against +Z would black the curtain
	// out at exactly the low sun angles where blown snow looks best.
	const float3 ambient = Color::Ambient(max(0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0))));

	float sunShadow = 1.0;
	[branch] if (MistShape.w > 0.5)
		sunShadow = SnowShadow::GetCascadeShadow(input.WorldRel, float3(0.0, 0.0, 1.0), 2.0,
			uint2((uint)MistSlices.x, (uint)MistSlices.y));
	float3 color = ambient + SharedData::DirLightColor.xyz * (0.8 * sunShadow);

	// Clustered point lights, LLF's own list, Shadow-flagged lights skipped
	// exactly as Effect.hlsl skips them - which is what frees this pass from
	// the point-shadow table. Cluster UV comes from this pass's own matrix;
	// FrameBuffer is not bound here.
	[branch] if (MistSlices.z > 0.5)
	{
		const float4 clip4 = mul(CameraViewProj, float4(input.WorldRel, 1.0));
		[branch] if (clip4.w > 1.0)
		{
			const float2 screenUV = (clip4.xy / clip4.w) * float2(0.5, -0.5) + 0.5;
			uint clusterIndex = 0;
			if (LightLimitFix::GetClusterIndex(screenUV, clip4.w, clusterIndex)) {
				const uint lightCount = LightLimitFix::lightGrid[clusterIndex].lightCount;
				const uint lightOffset = LightLimitFix::lightGrid[clusterIndex].offset;
				[loop] for (uint i = 0; i < lightCount; i++)
				{
					LightLimitFix::Light light = LightLimitFix::lights[LightLimitFix::lightList[lightOffset + i]];
					if (LightLimitFix::IsLightIgnored(light) || light.lightFlags & LightLimitFix::LightFlags::Shadow)
						continue;
					const float lightDist = length(light.positionWS.xyz - input.WorldRel);
					const float atten = InverseSquareLighting::GetAttenuation(lightDist, light);
					const bool isLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
					color += Color::PointLight(light.color.xyz, isLinear) * (atten * 0.5 * light.fade);
				}
			}
		}
	}

	// Fresh snow albedo, the shell's own constant fallback.
	color *= float3(0.82, 0.84, 0.88) * MistParams.w;
	return float4(color, alpha);
}
#endif
