"""
Demo: run a few gluing steps and render each attempt as a frame.
Usage: python visualize_demo.py [--k N] [--max-frames N] [--output frames]
"""
from __future__ import annotations
import argparse

from euclidean_tiling import (
    VertexTypeCatalog, PartialSolution, EuclideanSolver,
)

from visualize import Visualizer

# ---------------------------------------------------------------------------
# Replicate extend_into logic with frame capture
# ---------------------------------------------------------------------------
def _make_initial(vt: int) -> PartialSolution:
    """Mirrors EuclideanSolver.make_initial."""
    sl = len(VertexTypeCatalog.left_neighbors[vt])
    return PartialSolution(
        right_neighbor=list(VertexTypeCatalog.right_neighbors[vt]),
        left_neighbor=list(VertexTypeCatalog.left_neighbors[vt]),
        polygon_size=list(VertexTypeCatalog.polygon_sizes[vt]),
        mirror=list(VertexTypeCatalog.mirrors[vt]),
        glue=[-1] * sl,
        label=list(VertexTypeCatalog.edge_label_templates[vt]),
        vertex_types=[vt],
        num_polygons=1,
    )


def _extend_visual(
    viz: Visualizer,
    state: PartialSolution,
    max_polygons: int,
    depth: int = 0,
    max_depth: int = 3,
    max_frames: int = 200,
):
    """Walk the same logic as extend_into, capturing every attempt as a frame."""
    if depth >= max_depth or viz.frame >= max_frames:
        return

    n = len(state.right_neighbor)
    if n == 0:
        return

    # Find tightest free edge (same logic as analyze_cycles)
    ff, slack = _tightest_free(state)
    if ff < 0:
        return

    if state.label[ff][0] == "*":
        ff = state.mirror[ff]
    mirrored = state.mirror[ff] == ff

    # (a) pair with existing free edges
    for i in range(n):
        if state.glue[i] != -1:
            continue
        if (state.mirror[i] == i) != mirrored:
            continue

        cand = _copy_state(state)
        cand.glue[ff] = i
        cand.glue[i] = ff
        if not mirrored:
            cand.glue[state.mirror[ff]] = state.mirror[i]
            cand.glue[state.mirror[i]] = state.mirror[ff]

        is_valid = EuclideanSolver._is_valid_partial(
            cand.right_neighbor, cand.polygon_size, cand.glue,
        )

        viz.draw_attempt(state, ff, i, is_valid,
                         title=f"depth={depth}")

        if not is_valid:
            continue

        # Check if complete
        all_glued = all(g != -1 for g in cand.glue)
        if all_glued:
            viz.draw_state(cand, f"COMPLETE (depth={depth})")
            continue

        # Draw the valid partial
        viz.draw_state(cand, f"partial (depth={depth})")

        # Recurse
        _extend_visual(viz, cand, max_polygons, depth + 1, max_depth, max_frames)

    # (b) attach new vertex
    if state.num_polygons < max_polygons:
        first_type = state.vertex_types[0]
        for gr in range(first_type, VertexTypeCatalog.count()):
            offset = n
            sl = len(VertexTypeCatalog.left_neighbors[gr])

            cand = _copy_state(state)
            cand.right_neighbor.extend(offset + x for x in VertexTypeCatalog.right_neighbors[gr])
            cand.left_neighbor.extend(offset + x for x in VertexTypeCatalog.left_neighbors[gr])
            cand.mirror.extend(offset + x for x in VertexTypeCatalog.mirrors[gr])
            cand.polygon_size.extend(VertexTypeCatalog.polygon_sizes[gr])

            for s in range(sl):
                tmpl = VertexTypeCatalog.edge_label_templates[gr][s]
                if state.num_polygons > 3:
                    lab = tmpl + "@" + str(state.num_polygons)
                else:
                    lab = tmpl + "'" * state.num_polygons
                cand.label.append(lab)

            cand.glue.extend([-1] * sl)
            cand.vertex_types.append(gr)
            cand.num_polygons += 1

            limit = VertexTypeCatalog.attachment_limit(gr)
            for i in range(offset, offset + limit):
                if (cand.mirror[i] == i) != mirrored:
                    continue

                cand.glue[ff] = i
                cand.glue[i] = ff
                if not mirrored:
                    cand.glue[cand.mirror[ff]] = cand.mirror[i]
                    cand.glue[cand.mirror[i]] = cand.mirror[ff]

                is_valid = EuclideanSolver._is_valid_partial(
                    cand.right_neighbor, cand.polygon_size, cand.glue,
                )

                viz.draw_attempt(cand, ff, i, is_valid,
                                 title=f"attach {VertexTypeCatalog.symbols[gr]} d={depth}")

                if not is_valid:
                    # Undo gluing
                    cand.glue[ff] = -1
                    cand.glue[i] = -1
                    if not mirrored:
                        cand.glue[cand.mirror[ff]] = -1
                        cand.glue[cand.mirror[i]] = -1
                    continue

                all_glued = all(g != -1 for g in cand.glue)
                if all_glued:
                    viz.draw_state(cand, f"COMPLETE (attach {VertexTypeCatalog.symbols[gr]})")
                else:
                    viz.draw_state(cand, f"attach {VertexTypeCatalog.symbols[gr]}")
                    _extend_visual(viz, cand, max_polygons, depth + 1, max_depth, max_frames)

                # Undo gluing for next candidate
                cand.glue[ff] = -1
                cand.glue[i] = -1
                if not mirrored:
                    cand.glue[cand.mirror[ff]] = -1
                    cand.glue[cand.mirror[i]] = -1


def _tightest_free(state: PartialSolution):
    """Simplified: just find the first unglued edge with smallest slack."""
    seen = set()
    best = (-1, 13)
    for start in range(len(state.glue)):
        if start in seen:
            continue
        left = start
        while state.glue[left] != -1 and state.glue[left] != state.right_neighbor[start]:
            left = state.left_neighbor[state.glue[left]]

        if state.glue[left] == -1:
            stable = left
            right = state.right_neighbor[left]
            v_stable = state.polygon_size[right]
            cnt = 0
            while True:
                seen.add(left)
                cnt += 1
                left = state.glue[right]
                if left != -1:
                    right = state.right_neighbor[left]
                else:
                    slack = v_stable - cnt
                    if slack < best[1]:
                        best = (stable, slack)
                    break
        else:
            left = start
            right = state.right_neighbor[left]
            while True:
                seen.add(left)
                left = state.glue[right]
                if left == start:
                    break
                right = state.right_neighbor[left]
    return best


def _copy_state(st: PartialSolution) -> PartialSolution:
    return PartialSolution(
        right_neighbor=st.right_neighbor.copy(),
        left_neighbor=st.left_neighbor.copy(),
        polygon_size=st.polygon_size.copy(),
        mirror=st.mirror.copy(),
        glue=st.glue.copy(),
        label=st.label.copy(),
        vertex_types=st.vertex_types.copy(),
        num_polygons=st.num_polygons,
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser()
    p.add_argument("--k", type=int, default=1, help="max polygons")
    p.add_argument("--max-frames", type=int, default=200, help="frame limit")
    p.add_argument("--max-depth", type=int, default=3, help="recursion depth")
    p.add_argument("--output", default="frames")
    p.add_argument("--vertex-types", type=str, default="0,1,2",
                   help="comma-separated vertex type indices to demo (default 0,1,2)")
    args = p.parse_args()

    viz = Visualizer(args.output)
    vt_indices = [int(x.strip()) for x in args.vertex_types.split(",")]

    for vt in vt_indices:
        if viz.frame >= args.max_frames:
            break

        state = _make_initial(vt)
        viz.draw_state(state, f"Initial: {VertexTypeCatalog.symbols[vt]}")

        _extend_visual(viz, state, args.k, depth=0, max_depth=args.max_depth,
                       max_frames=args.max_frames)

    print(f"Generated {viz.frame} frames in '{args.output}/'")


if __name__ == "__main__":
    main()
