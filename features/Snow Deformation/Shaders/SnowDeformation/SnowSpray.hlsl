/**
 * @file SnowSpray.hlsl
 * @brief The puff of loose snow a foot plant throws into the air.
 *
 * RDR2's snow reads as walked THROUGH rather than walked over largely on the
 * strength of this one effect (TRENCH-REALISM-PLAN Stage 4, observation O6).
 * Scoped exactly as their version is: camera-facing sprites at the foot,
 * drifting and fading over about a second. No collision, no simulation, no
 * feedback into the deformation map - ROADMAP's research-tier "true 3D
 * particle displacement" is a different feature and deliberately NOT this.
 *
 * The vehicle is the lightning arc's: geometry from SV_VertexID after the
 * deferred composite, no vertex buffer, no engine particle hooks. Unlike the
 * bolt this is NOT emissive - it is airborne snow and must sit in the scene's
 * light - so it is lit the way CS lights its own effect particles
 * (Effect.hlsl GetLightingColor): directional light with a real cascade
 * shadow, the ambient recipe, and Light Limit Fix's clustered point lights
 * with Inverse Square attenuation, each piece the same shared module the
 * shells already route through. Lights carrying the Shadow flag are skipped,
 * exactly as Effect.hlsl skips them - that convention is what lets this pass
 * ignore the point-shadow table entirely.
 *
 * Purely procedural: a soft round puff with a hash breakup. No texture on
 * purpose - the arc's precedent is that a missing texture must cost detail,
 * not paint a black band, and a puff this soft never needed the art.
 */

#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"

#ifdef PSHADER
#	include "Common/ShadowSampling.hlsli"
#	include "SnowDeformation/SnowShadow.hlsli"
#	include "LightLimitFix/LightLimitFix.hlsli"
#	include "InverseSquareLighting/InverseSquareLighting.hlsli"
#endif

#define SPRAY_MAX_BURSTS 48
#define SPRAY_SPRITES 12
#define SPRAY_VERTS_PER_SPRITE 6

cbuffer SprayCB : register(b0)
{
	row_major float4x4 CameraViewProj;
	float4 SprayCameraPosAdjust;  ///< xyz = rebase origin, same pair as the matrix
	/// x = opacity scale, y > 0.5 = LLF clustered lights bound, z > 0.5 =
	/// cascade atlas copies bound, w = brightness scale
	float4 SprayParams;
	float4 SpraySlices;  ///< xy = the sun cascades' REAL atlas slices (round 22)
	float4 BurstPosRad[SPRAY_MAX_BURSTS];   ///< xyz world position, w stamp radius
	float4 BurstAnim[SPRAY_MAX_BURSTS];     ///< x 0-1 age, y seed, z strength, w unused
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 Local : TEXCOORD0;    ///< -1..1 across the sprite
	float3 WorldRel : TEXCOORD1;  ///< rebased world position of the sprite centre
	float2 Fade : TEXCOORD2;      ///< x = alpha envelope, y = sprite seed
};

float Hash11(float a_x)
{
	return frac(sin(a_x * 127.1) * 43758.5453);
}

#ifdef VSHADER
VS_OUTPUT main(uint a_vertexID : SV_VertexID)
{
	VS_OUTPUT vsout;

	const uint sprite = a_vertexID / SPRAY_VERTS_PER_SPRITE;
	const uint corner = a_vertexID % SPRAY_VERTS_PER_SPRITE;
	const uint burst = sprite / SPRAY_SPRITES;
	const uint puff = sprite % SPRAY_SPRITES;

	const float4 posRad = BurstPosRad[burst];
	const float4 anim = BurstAnim[burst];
	const float age = saturate(anim.x);
	const float radius = posRad.w;

	// Each puff gets its own direction, speed and size off one hash chain.
	const float seed = anim.y * 61.0 + puff * 7.13;
	const float ang = Hash11(seed + 1.7) * 6.2831853;
	const float outward = 0.35 + 0.75 * Hash11(seed + 3.1);
	const float upward = 0.55 + 0.85 * Hash11(seed + 5.9);

	// Kick out and up, then settle: the rise wins early, a soft pseudo
	// gravity wins late. Distances are fractions of the STAMP radius, so a
	// mammoth's plant throws a bigger sheet than a fox's.
	const float reach = radius * (1.6 + 1.4 * anim.z);
	const float3 vel = float3(cos(ang) * outward, sin(ang) * outward, upward);
	float3 world = posRad.xyz + vel * (reach * age) - float3(0.0, 0.0, reach * 0.55 * age * age);

	// Camera-facing quad, in the same rebased space as the arc.
	const float3 rel = world - SprayCameraPosAdjust.xyz;
	const float3 toEye = normalize(-rel);
	// A stable screen-aligned frame from the view direction alone.
	const float3 right = normalize(cross(float3(0.0, 0.0, 1.0), toEye) + 1e-4);
	const float3 up = cross(toEye, right);

	const uint quadIndex = (corner == 0) ? 0 : (corner == 1 || corner == 4) ? 1 :
	                       (corner == 2 || corner == 3)                     ? 2 :
	                                                                          3;
	const float2 local = float2((quadIndex & 1) ? 1.0 : -1.0, (quadIndex & 2) ? 1.0 : -1.0);

	// Puffs grow as they thin - thrown snow disperses, it does not travel as
	// a ball.
	const float size = radius * (0.30 + 1.05 * age) * (0.65 + 0.7 * Hash11(seed + 9.4));

	// Snap in, fade out squared - a burst, not a fade-up (arc precedent).
	const float envelope = saturate(age * 9.0) * (1.0 - age) * (1.0 - age);

	vsout.Position = mul(CameraViewProj, float4(rel + (right * local.x + up * local.y) * size, 1.0));
	vsout.Local = local;
	vsout.WorldRel = rel;
	vsout.Fade = float2(envelope * anim.z, seed);
	return vsout;
}
#endif

#ifdef PSHADER
float4 main(VS_OUTPUT input) : SV_TARGET
{
	// Soft round puff, edges broken by a static per-sprite hash so twelve
	// discs read as one ragged cloud. The breakup is in sprite-local space,
	// so it rides the sprite instead of boiling.
	const float r = length(input.Local);
	float soft = smoothstep(1.0, 0.25, r);
	const float2 cell = floor(input.Local * 3.0 + input.Fade.y);
	soft *= 0.7 + 0.5 * Hash11(cell.x * 17.3 + cell.y * 31.7 + input.Fade.y);

	float alpha = soft * input.Fade.x * SprayParams.x * 0.45;
	clip(alpha - 0.003);

	// Lit as CS lights its own effect particles (Effect.hlsl recipe), from
	// the same shared modules the shells route through. The puff has no
	// meaningful surface, so the ambient normal is straight up and the sun
	// term is unwrapped by a fixed 0.75 scatter - airborne snow forward-
	// scatters; a Lambert against +Z would black it out at low sun.
	const float3 ambient = Color::Ambient(max(0, SharedData::GetAmbient(float3(0.0, 0.0, 1.0))));

	float sunShadow = 1.0;
	[branch] if (SprayParams.z > 0.5)
		sunShadow = SnowShadow::GetCascadeShadow(input.WorldRel, float3(0.0, 0.0, 1.0), 2.0,
			uint2((uint)SpraySlices.x, (uint)SpraySlices.y));
	float3 color = ambient + SharedData::DirLightColor.xyz * (0.75 * sunShadow);

	// Clustered point lights: LLF's own list and Inverse Square attenuation,
	// skipping Shadow-flagged lights exactly as Effect.hlsl does - which is
	// what frees this pass from the point-shadow table.
	[branch] if (SprayParams.y > 0.5)
	{
		const float3 viewPosition = mul(FrameBuffer::CameraView, float4(input.WorldRel, 1.0)).xyz;
		const float2 screenUV = FrameBuffer::ViewToUV(viewPosition);
		uint clusterIndex = 0;
		if (LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex)) {
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

	// Fresh snow albedo, slightly blue like the shell's constant fallback.
	color *= float3(0.82, 0.84, 0.88) * SprayParams.w;
	return float4(color, alpha);
}
#endif
