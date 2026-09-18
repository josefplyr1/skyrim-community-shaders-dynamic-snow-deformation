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

#ifdef SKINNED
// Bone palette for one skin partition, built CPU-side as absolute world
// transforms - deliberately NOT the game's Bones buffer, whose rows carry a
// pivot subtracted out. Three float4 rows per bone, so a UNORM bone index
// times 765 addresses its first row, exactly as Lighting.hlsl does it.
cbuffer ContactSkinCB : register(b2)
{
	float4 BoneRows[240];
	float2 SkinWindowCenter;
	float SkinHalfExtent;
	// Bones in this partition's palette; an index past it names no bone.
	float SkinBoneCount;
}

struct VS_INPUT_SKIN
{
	float4 Position : POSITION0;
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
};
#endif

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
#ifdef SKINNED
VS_OUTPUT main(VS_INPUT_SKIN input)
{
	// Bone indices arrive as UNORM bytes; 765.01 = 255 * 3.0004 turns each
	// into the index of its first row, the game's own convention.
	int4 rows = int4(765.01 * input.BoneIndices);
	// The weights are half floats and sum to 1 only to about 2e-3. The rows
	// carry ABSOLUTE world translations (~1e5), so an unnormalized blend
	// shifts a vertex by that error times the world position - hundreds of
	// units, along the world position's own direction. The game's rows are
	// pivot-relative and never see this; ours must renormalize.
	// A weight on an index past the palette, or a vertex with no weight at
	// all, would blend zero rows and slide toward the world origin - the
	// comb. The game hides such vertices at its pivot; here they ride the
	// partition's first bone instead.
	float4 w = input.BoneWeights;
	const int lastRow = int(SkinBoneCount) * 3;
	w.x = rows.x < lastRow ? w.x : 0.0;
	w.y = rows.y < lastRow ? w.y : 0.0;
	w.z = rows.z < lastRow ? w.z : 0.0;
	w.w = rows.w < lastRow ? w.w : 0.0;
	const float wsum = dot(w, 1.0);
	[flatten] if (wsum < 1e-3)
	{
		w = float4(1.0, 0.0, 0.0, 0.0);
		rows = int4(0, 0, 0, 0);
	}
	else
		w /= wsum;
	float3x4 m =
		float3x4(BoneRows[rows.x], BoneRows[rows.x + 1], BoneRows[rows.x + 2]) * w.x +
		float3x4(BoneRows[rows.y], BoneRows[rows.y + 1], BoneRows[rows.y + 2]) * w.y +
		float3x4(BoneRows[rows.z], BoneRows[rows.z + 1], BoneRows[rows.z + 2]) * w.z +
		float3x4(BoneRows[rows.w], BoneRows[rows.w + 1], BoneRows[rows.w + 2]) * w.w;

	// The rows are absolute world, so this is the world position outright -
	// no object transform, and no pivot to add back.
	float3 worldAbs = mul(float4(input.Position.xyz, 1.0), transpose(m));

	float2 ndc = (worldAbs.xy - SkinWindowCenter) / SkinHalfExtent;

	VS_OUTPUT output;
	output.Position = float4(ndc, 0.5, 1.0);
	output.WorldZ = worldAbs.z;
	return output;
}
#else
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
#endif

#ifdef PSHADER
float main(VS_OUTPUT input) : SV_Target
{
	return input.WorldZ;
}
#endif
