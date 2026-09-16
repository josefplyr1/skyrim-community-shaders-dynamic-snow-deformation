Scriptname SnowDeformation Hidden
{Community Shaders - Snow Deformation. Read-only queries for other mods:
how deep the snow is at a point or under a reference, so a mod can slow
an actor, pick a wading animation, or gate an effect by depth.
All functions are safe to call when the feature is missing or off:
they return 0 / false.}

; API version, 2 for this set of functions. Later versions only add.
int Function GetAPIVersion() global native

; True when Snow Deformation is loaded and switched on.
bool Function IsActive() global native

; Snow depth in world units (1 unit = 1.43 cm, 70 units = 1 m) at a world
; XY: the untrampled depth of the terrain snow there, scaled by the current
; snow accumulation. 0 on bare ground, on unbaked land (interiors, cells
; never visited) and when the feature is off. Existing trenches are not
; subtracted: an actor walking carves its own trench, so this is the depth
; it wades through.
float Function GetSnowDepthAt(float worldX, float worldY) global native

; GetSnowDepthAt under a reference, with the stamping gate applied: a
; reference standing more than 70 units above the land (a bridge, a
; walkway, a roof) is not in the snow and reads 0.
float Function GetSnowDepthAtRef(ObjectReference ref) global native

; The snow accumulation scalar, 0..1: 0 at the set depths, 1 at the peak,
; following Accumulation Time, Melt Time and the fade. 0 when accumulation
; is off.
float Function GetSnowAccumulation() global native

; Stains the snow with a soft disc of blood at a world point: radius in
; world units, pigment as linear red/green/blue 0..1 (blood is about
; 0.3, 0.02, 0.01), amount 0..1 = how saturated the centre is. Deposited on
; the next frame; does nothing when the feature or its Blood on Snow setting
; is off, or the point lies outside the trench window. The mark fades under
; snowfall and dries over game hours like the game's own blood on the snow.
; API version 2.
Function DepositBlood(float worldX, float worldY, float worldZ, float radius, float red, float green, float blue, float amount) global native
