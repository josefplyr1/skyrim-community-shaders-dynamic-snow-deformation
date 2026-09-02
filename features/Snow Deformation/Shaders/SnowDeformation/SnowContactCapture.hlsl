// Contact height capture for moving props: each drawn render mesh writes its
// world Z from directly above into an R32F target with MIN blending, so a
// column holds the lowest surface standing over it this frame. The stamp
// pass carves where that surface reaches into the snow layer. Drawn with
// culling off: the underside is the surface that touches the snow.
//
// Layout prefix of StaticsCB (SnowDeformation.h) - only these fields are
// read, and a bound buffer may be larger than its declaration.
cbuffer StaticCB : register(b1)
{
	float4 WorldRow0;
	float4 WorldRow1;
	float4 WorldRow2;

	float ObjectsDepth;
	float2 HeightWindowCenter;
	float HeightHalfExtent;
}

struct VS_INPUT
{
	float4 Position : POSITION0;
	float4 Normal : NORMAL0;
};

struct VS_OUTPUT
{
	float4 Position : SV_POSITION;
	float WorldZ : TEXCOORD0;
};

#ifdef VSHADER
VS_OUTPUT main(VS_INPUT input)
{
	float3 posMS = input.Position.xyz;
	float3 worldAbs = float3(
		dot(WorldRow0.xyz, posMS) + WorldRow0.w,
		dot(WorldRow1.xyz, posMS) + WorldRow1.w,
		dot(WorldRow2.xyz, posMS) + WorldRow2.w);

	// Ortho top-down, the height capture's convention: +worldY maps to +ndcY,
	// texture v = 0 at the top. The stamp pass mirrors this when it samples.
	float2 ndc = (worldAbs.xy - HeightWindowCenter) / HeightHalfExtent;

	VS_OUTPUT output;
	output.Position = float4(ndc, 0.5, 1.0);
	output.WorldZ = worldAbs.z;
	return output;
}
#endif

#ifdef PSHADER
float main(VS_OUTPUT input) : SV_Target
{
	return input.WorldZ;
}
#endif
