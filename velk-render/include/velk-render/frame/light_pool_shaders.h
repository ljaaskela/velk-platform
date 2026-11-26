#ifndef VELK_RENDER_FRAME_LIGHT_POOL_SHADERS_H
#define VELK_RENDER_FRAME_LIGHT_POOL_SHADERS_H

#include <velk/string_view.h>

#include <velk-render/frame/light_pool_layout.h>

// The light pool's shaders. Their text is in velk-render/shaders, each file built in as the macro
// VELK_<NAME>_GLSL that its generated header <velk-render/glsl/<name>.inc> defines.

namespace velk {

// Reading the pool, for a shader that defines VELK_LIGHT_WORDS as the culling arena's buffer: its
// constants, RGB9E5 colours, pages and records, blocker codes, points and roots, the pieces' grids,
// a pixel's way down the pieces, the point lists and one values.
#include <velk-render/glsl/light_pool_defs.inc>
#include <velk-render/glsl/rgb9e5.inc>
#include <velk-render/glsl/light_pool_pages.inc>
#include <velk-render/glsl/light_blocker_codes.inc>
#include <velk-render/glsl/light_pool_points.inc>
#include <velk-render/glsl/light_piece_grid.inc>
#include <velk-render/glsl/light_piece_way.inc>
#include <velk-render/glsl/light_point_lists.inc>
#include <velk-render/glsl/light_one_values.inc>
#define VELK_LIGHT_POOL_GLSL VELK_LIGHT_POOL_HEADER_GLSL VELK_LIGHT_PAGE_HEADER_GLSL VELK_LIGHT_PIECE_PAGE_GLSL        \
    VELK_LIGHT_POOL_DEFS_GLSL VELK_RGB9E5_GLSL VELK_LIGHT_POOL_PAGES_GLSL VELK_LIGHT_BLOCKER_CODES_GLSL                 \
    VELK_LIGHT_POOL_POINTS_GLSL VELK_LIGHT_PIECE_GRID_GLSL VELK_LIGHT_PIECE_WAY_GLSL VELK_LIGHT_POINT_LISTS_GLSL       \
    VELK_LIGHT_ONE_VALUES_GLSL

// Putting lights on the trace list, after VELK_LIGHT_POOL_GLSL, for the shaders that list them
// (the claim and the search); apart, since it writes the pool and shaders that only read it
// declare it read-only.
#include <velk-render/glsl/light_list.inc>

// The lights a place keeps, for a shader built on the deferred prelude with the light grid
// (VELK_LIGHT_GRID_GLSL): Light, velk_light_at, VELK_GLOBALS and pc.lights_base. Positions are
// relative to the camera.
#include <velk-render/glsl/light_slots.inc>

// Adding up the lights the points of light give a pixel or a ray's hit: needs
// VELK_LIGHT_POOL_GLSL and VELK_LIGHT_SLOTS_GLSL first.
#include <velk-render/glsl/light_add.inc>

// What a pixel's light is, for the lighting pass: VELK_LIGHT_ADD_GLSL and velk_light_direct, which
// needs velk_instances too.
#include <velk-render/glsl/light_read_body.inc>
#define VELK_LIGHT_READ_GLSL VELK_LIGHT_ADD_GLSL VELK_LIGHT_READ_BODY_GLSL

/**
 * The light trace, three dispatches by VELK_LIGHT_TRACE_MODE: 1, a group that sizes the other two
 * from what the claims listed (their indirect args in the pool's header); 0, the one values on
 * the trace list (light_claim_src lists them), a group of threads each, a ray a light from its
 * centre; 2, the points of light on the point lists: their due lights traced over the lanes
 * pc.spare1 gives each (velk_points_trace_point). Built on the light grid's code
 * (light_grid_src), whose velk_light_cell_words is the culling arena; pc.spare3 is the pool.
 */
#include <velk-render/glsl/light_trace_main.inc>

// The rest of the light trace's shaders, after VELK_LIGHT_TRACE_MAIN_GLSL: apart, as the whole is
// past what one string the compiler takes may be.
#include <velk-render/glsl/light_trace_main_tail.inc>

/// The light trace's three shaders (VELK_LIGHT_TRACE_MAIN_GLSL), each only its own part, so that
/// each holds only its own code: the sizes of the other two, the one values, the points. Each is
/// its head here followed by light_trace_tail_src.
inline constexpr string_view light_trace_args_src = "#define VELK_LIGHT_TRACE_MODE 1\n" VELK_LIGHT_TRACE_MAIN_GLSL;
inline constexpr string_view light_trace_ones_src = "#define VELK_LIGHT_TRACE_MODE 0\n" VELK_LIGHT_TRACE_MAIN_GLSL;
inline constexpr string_view light_trace_points_src = "#define VELK_LIGHT_TRACE_MODE 2\n" VELK_LIGHT_TRACE_MAIN_GLSL;
inline constexpr string_view light_trace_tail_src = VELK_LIGHT_TRACE_MAIN_TAIL_GLSL;

/**
 * Makes the light of each kept cut record that a change this frame could alter due again, a
 * thread per record of the cut at pc.spare2 of the top tree at pc.spare1: own points stale, to
 * be traced from none again, coarse ones first, a one value traced again, a quad's on its node
 * and the nodes above it. A change of a light alters what uses that light; a box alters what it could shadow
 * or stop shadowing, a record whose way to one of its lights passes through the box. Light that
 * nothing changed is not traced again. Built on the deferred prelude with the light grid;
 * velk_light_cell_words is the culling arena, pc.spare3 the pool and pc.spare4 the change list.
 */
#include <velk-render/glsl/light_changes_main.inc>
inline constexpr string_view light_changes_main_src = VELK_LIGHT_CHANGES_MAIN_GLSL;

// A probe's light (VELK_PROBE_LIGHT), for a shader that defines VELK_LIGHT_WORDS as the buffer
// the probes are in: read, written and seen from a normal.
#include <velk-render/glsl/probe.inc>

// Writing a probe whole (VELK_PROBE), after VELK_PROBE_GLSL, for a shader whose VELK_LIGHT_WORDS
// is writable.
#include <velk-render/glsl/probe_write.inc>

// The probes' cells (kProbeCellSlots), for a shader with VELK_PROBE_GLSL and VELK_PROBE_WRITE_GLSL:
// a cell's words, and finding one.
#include <velk-render/glsl/probe_cell.inc>

/**
 * The screen probes' gather, in two dispatches. The first, a thread a probe, decides what each
 * probe does with its cell (velk_probes_decide): those that read it are done, those that cast
 * rays go to a list, which sizes the second, an indirect dispatch of four probes of the list a
 * group of kLightProbeGroupThreads, a ray a thread, kLightProbeRays a probe (velk_probes_trace),
 * so that its cost follows the probes that cast rays. The rays are spread over the hemisphere
 * around the probe's normal by cosine: each takes its own quarter of a 4 by 4 grid of directions,
 * a different cell of it each frame so that four frames cover all sixteen, staggered by the
 * probe's place so that each 2 by 2 block of probes covers them all every frame, moved within the
 * cell by a value of the probe's own. A hit brings what the light points there say reaches it and
 * its indirect light (velk_probe_hit_indirect), times its albedo, and what it gives off; a miss
 * the environment. The probe's light from its rays goes to the ray store; light_probes_filter_src
 * then filters it.
 *
 * A hit's albedo and emission come from its draw's shading summary (velk_probe_surface), so it is
 * composed by compose_rt_trace, without the materials' code. pc.shapes_base is the light pool,
 * pc.env.z the light header (the grid), pc.env.w the last frame's probe store, pc.extras the ray
 * store, the G-buffer's size (x low, y high), its world position and its normal, pc.more the
 * cells, the frame everything last changed, this frame's change list and the probe list (bit 31
 * set for the trace).
 */
#include <velk-render/glsl/light_probes_main.inc>
#include <velk-render/glsl/light_eval.inc>
#include <velk-render/glsl/light_area.inc>
inline constexpr string_view light_probes_main_src = VELK_LIGHT_PROBES_MAIN_GLSL;

/**
 * The reflections' resolve, after their trace (light_probes_main_src with VELK_REFLECT): a thread a
 * pixel, 8 by 8 a group. The trace left a sample a 2 by 2 block (pc.extras.x), from the block's
 * pixel of this frame. A pixel that reflects finds the last frame's result where its reflection
 * was then: through its image along the view ray, behind the surface by how far the ray went, or
 * for a miss the view ray's direction; else where the surface was. A mirror takes its own sample
 * where it is the pixel traced, else that last result, else its block's sample. A glossy surface
 * takes the samples of the 3 by 3 blocks around, by how much the surfaces they were traced from
 * agree with its own, blended with the last result kept to their spread. Into this frame's result
 * (pc.more.z): the light, and how many frames it holds, 0 for no reflection. Traces nothing:
 * composed after rt_compute_prelude_src alone. pc.extras.y is the G-buffer's size (x low, y
 * high), z its positions, w its normals; pc.more.x its material parameters, y the last frame's
 * result, w not 0 when there is none; pc.env.w the most frames a glossy result blends
 * (kReflectMovingFrames, kReflectSettledFrames).
 */
#include <velk-render/glsl/reflect_resolve.inc>
inline constexpr string_view reflect_resolve_src = VELK_REFLECT_RESOLVE_GLSL;

/**
 * The screen probes' filter, after the gather (light_probes_main_src): a thread a probe. Its light
 * is the mean of its own and its 3 by 3 neighbours' light, each by how near its surface is to the
 * probe's plane and the probe to its, and how much their normals agree. What a probe brings over
 * frames is its cell's (VELK_PROBE_CELL_GLSL), which keeps it in the world while the camera
 * moves. With pc.extras.w 1 it instead lets go of cells no probe read for VELK_CELL_KEEP frames, a
 * thread a slot, after the filter, so that the next gather only takes free slots. Traces nothing:
 * composed after rt_compute_prelude_src alone. pc.extras.x is the ray store, pc.extras.y the
 * G-buffer's size (x low, y high), pc.extras.z this frame's probe store, pc.env.w the cells,
 * pc.more.w the gather's probe list, which it empties.
 */
#include <velk-render/glsl/light_probes_filter.inc>
inline constexpr string_view light_probes_filter_src = VELK_LIGHT_PROBES_FILTER_GLSL;

/**
 * Gives each record a pixel shows its light, the first time one does, as kLightNone says: one
 * value, a quad's node, or a place on the search list for a cluster wide enough for points, which
 * waits as kLightPending until light_search_src lays it out. A record with one value that has
 * grown to twice the light spacing goes on the list too. In a record with pieces, the pixel's way
 * down its triangle's levels (velk_piece_way, velk_light_list_way): the piece it reaches is read
 * this frame, and where the level is too coarse and has nothing under it the pixel asks on the
 * split list for its children, or for the triangle's root under its vertex points. A thread per
 * pixel. `light_pixels` is kLightPixels.
 *
 * Run again after the search with `again` set when the search had records or asks for pieces: it
 * lists the points of the records and pieces just made, so that their coarse points are traced
 * this frame, and puts nothing on the search or split list, which the next frame's search would
 * not see.
 */
// The claim's own code, shared by its pass over the visibility buffer's pixels (light_claim_src)
// and its pass over blended surfaces' fragments (light_claim_blended_src). Who includes it has
// `light_words` writable as VELK_LIGHT_WORDS, push constants `pc` with pool_word, light_pixels,
// lights and again, and says before it where the pixel is in its triangle (velk_claim_weights),
// how long its triangle's longest edge is on screen (velk_claim_tri_px) and its instance's world
// (velk_claim_world).
#include <velk-render/glsl/light_claim.inc>

#include <velk-render/glsl/light_claim_main.inc>
inline constexpr string_view light_claim_src = VELK_LIGHT_CLAIM_MAIN_GLSL;

// Where a fragment of a draw by cluster is in its triangle, for shaders over blended surfaces'
// fragments, which the visibility buffer does not hold: from its triangle's corners, from the
// camera as its vertices are (velk_default_varyings). Needs velk-ui.glsl.
#include <velk-render/glsl/fragment_triangle.inc>

/**
 * The claim (light_claim_src) for the fragments of blended surfaces, which the visibility buffer
 * does not hold: a raster pass over their clusters' draws against the visibility buffer's depth,
 * with no colour, whose fragment shader this is, before the claim's search and again after it.
 * A fragment's ids are those the visibility pass would write (VELK_WRITE_VISIBILITY); where it is
 * in its triangle comes from its triangle's corners. Its push constants are the draw's, then the
 * claim's.
 */
#include <velk-render/glsl/light_claim_blended.inc>
inline constexpr string_view light_claim_blended_src = VELK_LIGHT_CLAIM_BLENDED_GLSL;

/**
 * Lays out the records on the search list (light_claim_src), a work group each: a point per
 * vertex, and with its draw's indices a root word per triangle, none yet (kLightPieced, else
 * kLightOwn). A record that cannot be laid out this frame (no room) gets its light back and asks
 * again.
 *
 * Then, in kLightSplitCapacity more groups, makes the pieces the split list asks for, a work
 * group an ask: a triangle's root, or a piece's children, down to the depth asked along the
 * asking pixel's way (VELK_SPLIT_PATH_SHIFT): at each level the four children of the piece it is
 * in, so that every piece has its four children or none. Their points start untraced, but for a root's corners, which take the vertices' points'
 * light. They are linked in last, so that nothing reads them before they are written.
 */
#include <velk-render/glsl/light_search.inc>
inline constexpr string_view light_search_src = VELK_LIGHT_SEARCH_GLSL;

/**
 * Marks the pages the records of a kept cut name, and counts its records with pages and with one
 * value. Lists the one values a
 * screen probe's ray asked for (kLightOneAsked) that are due, a record's or a quad's node's, as
 * later traces. A thread per record of the cut's pool, a group a page up to the last in use
 * (VELK_CUT_DRAW_GROUPS). Runs after the sweep's begin, so before the claim lists this frame's.
 */
#include <velk-render/glsl/light_mark.inc>
inline constexpr string_view light_mark_src = VELK_LIGHT_MARK_GLSL;

/**
 * Keeps the pieces in use, after the records' mark (light_mark_src) and before the sweep, a thread
 * per page of the pool, twice. First (`phase` 0) it unlinks what no pixel read for
 * kLightPieceKeepFrames: a piece's four children, when all are without children of their own and
 * none was read, and a root without children likewise, from its record; and a piece's four
 * children without children of their own, made where light differed (VELK_PIECE_PLACE_EDGE), whose
 * light has been the same as the piece's for kLightPieceSameFrames and one more (velk_pieces_same), so that pixels read the piece again and
 * ask for children again where its light differs. Then (`phase` 1) it marks
 * every piece still linked, from its parent or its record, whose record is marked; the sweep frees
 * the rest. Only childless pieces are unlinked, so a piece's parent is in use while it is.
 */
#include <velk-render/glsl/light_pieces.inc>
inline constexpr string_view light_pieces_src = VELK_LIGHT_PIECES_GLSL;

/**
 * Puts every page no mark says is in use on the free stack, no longer a piece, and clears the
 * marks for the next frame. A thread per 32 pages. Starts and ends the frame's marking: with
 * `begin` set, one thread empties the stack, the mark's counts and the search and split lists
 * instead, and moves the pool's frame on.
 */
#include <velk-render/glsl/light_sweep.inc>
inline constexpr string_view light_sweep_src = VELK_LIGHT_SWEEP_GLSL;

} // namespace velk

#endif // VELK_RENDER_FRAME_LIGHT_POOL_SHADERS_H
