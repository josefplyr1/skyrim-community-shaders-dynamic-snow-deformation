/**
 * @file LightningArc.hlsl
 * @brief The visible arc between a shock cloak's wearer and the ground it just struck.
 *
 * Purely visual, and it must stay that way: blasts are detected by watching a
 * projectile leave the projectile manager, so a real bolt spawned for the look
 * would be read as a detonation, pock the snow and arc again.
 *
 * Geometry comes from SV_VertexID alone, as the shell's does: a strip of quads
 * from caster to strike, each joint pushed off the straight line by a hash so
 * the bolt forks. Camera-facing, so it has no thickness to see edge-on.
 */

#define ARC_SEGMENTS 32
#define ARC_VERTS_PER_SEGMENT 6

cbuffer ArcCB : register(b0)
{
	row_major float4x4 CameraViewProj;
	float4 ArcCameraPosAdjust;  ///< xyz = camera position the world is rebased on
	float4 ArcFrom;             ///< xyz = caster emission point
	float4 ArcTo;               ///< xyz = the spot that was struck
	/// x = width in world units, y = brightness, z = 0-1 age, w = per-arc seed
	float4 ArcParams;
	float4 ArcTint;             ///< rgb = colour, a = 1 when a texture is bound
};

Texture2D<float4> ArcTexture : register(t0);
SamplerState LinearSampler : register(s0);

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float2 TexCoord : TEXCOORD0;
	float Taper : TEXCOORD1;
};

/// Cheap hash. Two joints on the same bolt must not agree, and two bolts must
/// not fork identically, so the seed rides in alongside the joint index.
float Hash11(float a_x)
{
	return frac(sin(a_x * 127.1) * 43758.5453);
}

float3 JointOffset(float a_t, float a_seed, float3 a_side, float3 a_up, float a_length)
{
	// Zero at both ends: a bolt is anchored at the hand and at the ground, and
	// a fork that misses either end reads as a stray ribbon rather than a
	// strike. sin() gives that pinning for free.
	const float pin = sin(a_t * 3.14159265);
	const float joint = a_t * ARC_SEGMENTS;

	// FINE: a fresh kink at every joint. This carries the character - lightning
	// is a mostly straight line interrupted often, not a few wild swings.
	const float f1 = Hash11(joint * 1.7 + a_seed * 31.0) - 0.5;
	const float f2 = Hash11(joint * 2.3 + a_seed * 57.0) - 0.5;

	// COARSE: one slow wander every several joints, so the line drifts off true
	// rather than vibrating around it. Quantised on purpose, so it changes AT a
	// joint and reads as a kink instead of a curve.
	const float band = floor(joint / 6.0);
	const float c1 = Hash11(band * 7.1 + a_seed * 11.0) - 0.5;
	const float c2 = Hash11(band * 9.3 + a_seed * 19.0) - 0.5;

	// Both amplitudes are fractions of the bolt's own LENGTH. Scaling the
	// wander off the ribbon WIDTH instead - which is what this did first - made
	// a thick bolt thrash and a thin one barely move, and that is backwards:
	// how far lightning strays has nothing to do with how thick it is drawn.
	const float fine = a_length * 0.018;
	const float wander = a_length * 0.030;
	return (a_side * (f1 * fine + c1 * wander) + a_up * (f2 * fine + c2 * wander)) * pin;
}

#ifdef VSHADER
VS_OUTPUT main(uint a_vertexID : SV_VertexID)
{
	VS_OUTPUT vsout;

	const uint segment = a_vertexID / ARC_VERTS_PER_SEGMENT;
	const uint corner = a_vertexID % ARC_VERTS_PER_SEGMENT;

	// Two triangles per segment, as a quad: 0,1,2 / 2,1,3.
	const uint quadIndex = (corner == 0) ? 0 : (corner == 1 || corner == 4) ? 1 :
	                       (corner == 2 || corner == 3)                     ? 2 :
	                                                                          3;
	const float along = (quadIndex & 1) ? 1.0 : 0.0;
	const float side = (quadIndex & 2) ? 1.0 : -1.0;

	const float t = (segment + along) / (float)ARC_SEGMENTS;

	const float3 from = ArcFrom.xyz;
	const float3 to = ArcTo.xyz;
	const float3 axis = to - from;
	const float axisLen = max(length(axis), 1e-3);
	const float3 dir = axis / axisLen;

	// The bolt's own frame, so the jitter is perpendicular to its travel
	// rather than to the world.
	float3 side3 = normalize(cross(dir, float3(0.0, 0.0, 1.0)) + 1e-4);
	const float3 up3 = normalize(cross(side3, dir));

	const float seed = ArcParams.w;
	float3 world = lerp(from, to, t) + JointOffset(t, seed, side3, up3, axisLen);

	// Face the camera. Rebased space, so the camera sits at the origin and the
	// view direction is the position itself.
	const float3 rel = world - ArcCameraPosAdjust.xyz;
	const float3 toEye = normalize(-rel);
	float3 ribbon = cross(dir, toEye);
	const float ribbonLen = length(ribbon);
	// Edge-on: any perpendicular will do, and the quad is invisible anyway.
	ribbon = ribbonLen > 1e-4 ? ribbon / ribbonLen : side3;

	// Thin at the ends, fat in the middle - the shape an electrical channel
	// actually has, and it hides the joins with the hand and the ground.
	const float taper = 0.35 + 0.65 * sin(t * 3.14159265);
	const float halfWidth = ArcParams.x * 0.5 * taper;

	vsout.Position = mul(CameraViewProj, float4(rel + ribbon * side * halfWidth, 1.0));
	// The vanilla art here is a TILE (shockbolttile01 and friends), so U repeats
	// at a fixed world rate rather than stretching once over the whole bolt - a
	// long strike would otherwise smear the pattern and a short one squash it.
	// Sampler wraps on U for exactly this.
	vsout.TexCoord = float2(t * max(axisLen / 192.0, 1.0), side * 0.5 + 0.5);
	vsout.Taper = taper;
	return vsout;
}
#endif

#ifdef PSHADER
float4 main(VS_OUTPUT input) : SV_TARGET
{
	// Across the ribbon: a hot core with a soft falloff. This is what makes it
	// read as light rather than as a painted strip, and it is also why the
	// effect stands up with no texture at all - a guessed vanilla path that
	// resolves to nothing must not leave a black band in the air.
	const float across = abs(input.TexCoord.y * 2.0 - 1.0);
	float profile = saturate(1.0 - across);
	profile = profile * profile;
	const float core = pow(profile, 8.0);

	float shape = profile * 0.35 + core;

	if (ArcTint.a > 0.5) {
		// A texture, when one is supplied, modulates the channel rather than
		// replacing it: a lightning sheet is mostly empty, and multiplying
		// keeps the bolt continuous where the art happens to be blank.
		const float4 tex = ArcTexture.SampleLevel(LinearSampler, input.TexCoord, 0);
		shape *= 0.35 + 0.65 * max(tex.a, dot(tex.rgb, float3(0.299, 0.587, 0.114)));
	}

	// Flare in, fade out. A strike is not a fade-up.
	const float age = saturate(ArcParams.z);
	const float envelope = saturate(1.0 - age) * saturate(age * 12.0);

	const float3 colour = ArcTint.rgb * (ArcParams.y * shape * envelope * input.Taper);
	return float4(colour, 1.0);
}
#endif
