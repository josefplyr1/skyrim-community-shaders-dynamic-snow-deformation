// Warped camera-following grid: the shared band lattice both snow surfaces
// place their vertices on.
//
// Extracted from SnowShell.hlsl (2026-08-26) so the trench patch can stand on
// the same lattice instead of its own flat one; see ROAD-HEIGHTFIELD-PLAN S2.
// The shell's bytecode is unchanged by the move (14 permutations, DXBC hash).
//
// Reads GridOrigin, GridSpacing and WarpedHalfSpan from ShellCB, which both
// SnowShell.hlsl and SnowStaticsShell.hlsl already mirror.

// Distance warp, power-of-two bands: band b holds kWarpBandVerts[b] vertices
// spaced kWarpBandMul[b] * GridSpacing apart, reaching ~17k units per side.
// Must match SnowDeformation.h (kShellWarpBandVerts / kShellWarpBandMul).
//
// INVARIANT: every step is an exact power of two of the base step and every
// band start is a multiple of both its own step and the origin snap, so a
// vertex lands on its band's lattice with no rounding left over. Break it and
// quad widths flip as the camera moves (distant up/down jumping).
#define kWarpBands 5
static const float kWarpBandVerts[kWarpBands] = { 192.0, 8.0, 8.0, 8.0, 104.0 };
static const float kWarpBandMul[kWarpBands] = { 1.0, 2.0, 4.0, 8.0, 16.0 };

// Band lookup for vertex |u|: x = step multiplier in GridSpacing units,
// y = fraction through the band (0 at its inner edge, 1 at its outer). The
// fraction drives the data morph, so a band hand-off is continuous in RADIUS -
// ground reaches the coarser lattice's surface exactly where it changes bands.
// Vertices past the table extend at the coarsest step.
// z = band index, for the ring debug view.
float3 WarpBand(float au)
{
	float3 found = float3(kWarpBandMul[kWarpBands - 1], 1.0, (float)(kWarpBands - 1));
	bool done = false;
	float acc = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float prev = acc;
		acc += kWarpBandVerts[band];
		[flatten] if (!done && au < acc)
		{
			found = float3(kWarpBandMul[band], saturate((au - prev) / max(kWarpBandVerts[band], 1.0)), (float)band);
			done = true;
		}
	}
	return found;
}

// Table-driven form: same walk, on a caller-supplied band table and base step.
// The trench patch runs its own table (a fine core sized to the object raster
// rather than to the horizon) through this, so there is one implementation of
// the walk and not two.
float WarpAxisT(float u, float bandVerts[kWarpBands], float bandMul[kWarpBands], float spacing)
{
	float a = abs(u);
	float off = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float take = min(a, bandVerts[band]);
		off += take * bandMul[band];
		a -= take;
	}
	off += a * bandMul[kWarpBands - 1];
	return sign(u) * off * spacing;
}

// Maps a vertex coordinate relative to the grid center (in vertex units)
// to a world-unit offset from the center, on the shell's own table.
//
// Deliberately NOT `return WarpAxisT(u, kWarpBandVerts, kWarpBandMul,
// GridSpacing)`, which is what it looks like it should be. Routing the shell
// through the table-driven form costs the shadow-cast VS one instruction and
// reshuffles 451 asm lines - a change the DXBC harness cannot certify as a
// no-op, on the caster that has already been parked once for cascade bugs.
// The twin is 10 lines and the two must be edited together; that is a smaller
// risk than an unverifiable change to the caster. Revisit with the runtime A/B
// harness (docs/development/shader-runtime-ab.md), not by "tidying" this away.
float WarpAxis(float u)
{
	float a = abs(u);
	float off = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float take = min(a, kWarpBandVerts[band]);
		off += take * kWarpBandMul[band];
		a -= take;
	}
	off += a * kWarpBandMul[kWarpBands - 1];
	return sign(u) * off * GridSpacing;
}

// Inverse of WarpAxis: world-unit offset from the grid center back to vertex
// units. Shared by the ring debug view and the probe CS.
float InverseWarpAxis(float w)
{
	float a = abs(w) / GridSpacing;
	float u = 0.0;
	[unroll] for (int band = 0; band < kWarpBands; ++band)
	{
		float span = kWarpBandVerts[band] * kWarpBandMul[band];
		float take = min(a, span);
		u += take / kWarpBandMul[band];
		a -= take;
	}
	u += a / kWarpBandMul[kWarpBands - 1];
	return sign(w) * u;
}

// CDLOD-style geomorph for the warped outer rings: each vertex slides
// between a fine and a 2x coarser lattice by a continuous morph weight. Both
// endpoints are near-static world points, so camera motion never hops a vertex.
// Lattices are centred on the grid centre, which steps 8 units with the camera
// - a sixteenth of a data texel. Takes/returns centred coordinates.
float2 GeomorphVertexXY(float2 centered, float2 u)
{
	float2 fineStep = GridSpacing * float2(WarpBand(abs(u.x)).x, WarpBand(abs(u.y)).x);
	// World-lattice snap: vertices never slide in XY, so the rendered surface is
	// world-static and camera steps only reassign which index owns which point.
	// The LOD transition happens in the sampled data instead (ShellSurfaceZ,
	// clipmap-style). A no-op while the band table stays power-of-two aligned;
	// kept as the guard rail for when it does not.
	float2 absXY = GridOrigin + WarpedHalfSpan + centered;
	return floor(absXY / fineStep + 0.5) * fineStep - (GridOrigin + WarpedHalfSpan);
}
