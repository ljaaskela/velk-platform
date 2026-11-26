// What a point's last ray to a light found, 4 bits: a light due a ray, never traced at the point
// or made due by a change (what the point has of it stays until its next ray); one whose ray
// something blocked, VELK_LIGHT_BLOCKER_NEAREST to VELK_LIGHT_BLOCKER_FARTHEST by how far away
// that was (velk_light_blocker_code); one whose ray found nothing in the way.
#define VELK_LIGHT_BLOCKER_DUE 0u
#define VELK_LIGHT_BLOCKER_NEAREST 1u
#define VELK_LIGHT_BLOCKER_FARTHEST 14u
#define VELK_LIGHT_BLOCKER_CLEAR 15u

// Whether blocker code `code` says the light's ray was blocked.
bool velk_light_code_blocked(uint code)
{
    return code >= VELK_LIGHT_BLOCKER_NEAREST && code <= VELK_LIGHT_BLOCKER_FARTHEST;
}

// The blocker codes span a factor of two each. For a directional light, how far away what was in
// the way is, in world units, from 2^-4 to 2^9; for a light at a place, that over how far away the
// light is, from 2^-13 to 1.
#define VELK_LIGHT_BLOCKER_FAR_LOG2 9.0
#define VELK_LIGHT_BLOCKER_NEAR_LOG2 0.0

// The blocker code of a ray blocked `blocker_t` along it, to a light `light_dist` away; a
// directional light's `light_dist` is 0. 0 or less for `blocker_t` is right at the start.
uint velk_light_blocker_code(float blocker_t, float light_dist)
{
    float top = light_dist > 0.0 ? VELK_LIGHT_BLOCKER_NEAR_LOG2 : VELK_LIGHT_BLOCKER_FAR_LOG2;
    float reach = light_dist > 0.0 ? blocker_t / light_dist : blocker_t;
    float steps = reach > 0.0 ? top - log2(reach) : 1e9;
    return VELK_LIGHT_BLOCKER_FARTHEST - uint(clamp(floor(steps), 0.0, 13.0));
}

// How far away what was in the way is by blocker code `code` (velk_light_blocker_code): in world
// units for a directional light (`directional`), else over how far away the light is.
float velk_light_blocker_reach(uint code, bool directional)
{
    float top = directional ? VELK_LIGHT_BLOCKER_FAR_LOG2 : VELK_LIGHT_BLOCKER_NEAR_LOG2;
    float c = float(clamp(code, VELK_LIGHT_BLOCKER_NEAREST, VELK_LIGHT_BLOCKER_FARTHEST));
    // The middle of the code's factor of two.
    return exp2(top - (float(VELK_LIGHT_BLOCKER_FARTHEST) - c) - 0.5);
}

// Whether any light of blocker codes `codes` is due (VELK_LIGHT_BLOCKER_DUE): any 4 bits 0.
bool velk_light_any_due(uint codes)
{
    return (((codes - 0x11111111u) & ~codes) & 0x88888888u) != 0u;
}
