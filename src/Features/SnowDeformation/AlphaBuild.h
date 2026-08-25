#pragma once

// Package switch for the Snow Deformation menu.
//
// 0 = developer build: the Debugging Options tree is present.
// 1 = public/alpha package: it is compiled OUT.
//
// Flipped by Build-AlphaPackage.ps1 in the workspace root, which builds the
// package, deploys it, then flips this back and rebuilds the dev AIO. Kept in
// its own ASCII-only file so that script can rewrite it without a PowerShell
// round-trip over a source file carrying non-ASCII text (see CLAUDE.md).
//
// The debug state this hides all defaults to the shipped behaviour and none of
// it is serialised, so a package built with this at 1 behaves exactly as the
// dev build does with every debug control untouched.
#define SNOW_ALPHA_BUILD 0
