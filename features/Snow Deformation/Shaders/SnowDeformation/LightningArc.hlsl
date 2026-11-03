#define ARC_SEGMENTS 32
#define ARC_VERTS_PER_SEGMENT 6

cbuffer ArcCB : register(b0)
{
	row_major float4x4 CameraViewProj;
	float4 ArcCameraPosAdjust;
	float4 ArcFrom;
	float4 ArcTo;
	float4 ArcParams;
	float4 ArcTint;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float Across : TEXCOORD0;
	float Taper : TEXCOORD1;
};

float Hash11(float a_x)
{
	return frac(sin(a_x * 127.1) * 43758.5453);
}

float3 JointOffset(float a_t, float a_seed, float3 a_side, float3 a_up, float a_length)
{
	const float pin = sin(a_t * 3.14159265);
	const float joint = a_t * ARC_SEGMENTS;

	const float f1 = Hash11(joint * 1.7 + a_seed * 31.0) - 0.5;
	const float f2 = Hash11(joint * 2.3 + a_seed * 57.0) - 0.5;

	const float band = floor(joint / 6.0);
	const float c1 = Hash11(band * 7.1 + a_seed * 11.0) - 0.5;
	const float c2 = Hash11(band * 9.3 + a_seed * 19.0) - 0.5;

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

	float3 side3 = normalize(cross(dir, float3(0.0, 0.0, 1.0)) + 1e-4);
	const float3 up3 = normalize(cross(side3, dir));

	const float seed = ArcParams.w;
	float3 world = lerp(from, to, t) + JointOffset(t, seed, side3, up3, axisLen);

	const float3 rel = world - ArcCameraPosAdjust.xyz;
	const float3 toEye = normalize(-rel);
	float3 ribbon = cross(dir, toEye);
	const float ribbonLen = length(ribbon);
	ribbon = ribbonLen > 1e-4 ? ribbon / ribbonLen : side3;

	const float taper = 0.35 + 0.65 * sin(t * 3.14159265);
	const float halfWidth = ArcParams.x * 0.5 * taper;

	vsout.Position = mul(CameraViewProj, float4(rel + ribbon * side * halfWidth, 1.0));
	vsout.Across = side * 0.5 + 0.5;
	vsout.Taper = taper;
	return vsout;
}
#endif

#ifdef PSHADER
float4 main(VS_OUTPUT input) : SV_TARGET
{
	const float across = abs(input.Across * 2.0 - 1.0);
	float profile = saturate(1.0 - across);
	profile = profile * profile;
	const float core = pow(profile, 8.0);

	const float shape = profile * 0.35 + core;

	const float age = saturate(ArcParams.z);
	const float envelope = saturate(1.0 - age) * saturate(age * 12.0);

	const float3 colour = ArcTint.rgb * (ArcParams.y * shape * envelope * input.Taper);
	return float4(colour, 1.0);
}
#endif
