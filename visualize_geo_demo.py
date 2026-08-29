"""
Demo: geometric tiling visualizer — draws actual polygons glued together.
Usage: python visualize_geo_demo.py --k 1 --max-frames 30 --vertex-types 0
"""
from __future__ import annotations
import argparse

from euclidean_tiling import (
    VertexTypeCatalog, PartialSolution, EuclideanSolver,
)
from visualize_geo import GeoVisualizer


def _make_initial(vt: int) -> PartialSolution:
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


def _tightest_free(state):
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
                seen.add(left); cnt += 1
                left = state.glue[right]
                if left != -1:
                    right = state.right_neighbor[left]
                else:
                    slack = v_stable - cnt
                    if slack < best[1]:
                        best = (stable, slack)
                    break
        else:
            left = start; right = state.right_neighbor[left]
            while True:
                seen.add(left)
                left = state.glue[right]
                if left == start: break
                right = state.right_neighbor[left]
    return best


def _copy_state(st):
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


def _extend_geo(viz, state, max_polygons, depth=0, max_depth=3, max_frames=200):
    if depth >= max_depth or viz.frame >= max_frames:
        return
    n = len(state.right_neighbor)
    if n == 0:
        return

    ff, slack = _tightest_free(state)
    if ff < 0:
        return

    if state.label[ff][0] == "*":
        ff = state.mirror[ff]
    mirrored = state.mirror[ff] == ff

    # (a) pair with existing free edge
    for i in range(n):
        if state.glue[i] != -1:
            continue
        if (state.mirror[i] == i) != mirrored:
            continue

        cand = _copy_state(state)
        cand.glue[ff] = i; cand.glue[i] = ff
        if not mirrored:
            cand.glue[state.mirror[ff]] = state.mirror[i]
            cand.glue[state.mirror[i]] = state.mirror[ff]

        is_valid = EuclideanSolver._is_valid_partial(
            cand.right_neighbor, cand.polygon_size, cand.glue)

        viz.draw_attempt(state, ff, i, is_valid, f"d={depth}")
        if not is_valid:
            continue

        all_glued = all(g != -1 for g in cand.glue)
        if all_glued:
            viz.draw_state(cand, f"COMPLETE (d={depth})")
            continue

        viz.draw_state(cand, f"partial (d={depth})")
        _extend_geo(viz, cand, max_polygons, depth+1, max_depth, max_frames)

    # (b) attach new vertex
    if state.num_polygons < max_polygons:
        first_type = state.vertex_types[0]
        for gr in range(first_type, VertexTypeCatalog.count()):
            offset = n
            sl = len(VertexTypeCatalog.left_neighbors[gr])
            limit = VertexTypeCatalog.attachment_limit(gr)

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

            for i in range(offset, offset + limit):
                if (cand.mirror[i] == i) != mirrored:
                    continue
                cand.glue[ff] = i; cand.glue[i] = ff
                if not mirrored:
                    cand.glue[cand.mirror[ff]] = cand.mirror[i]
                    cand.glue[cand.mirror[i]] = cand.mirror[ff]

                is_valid = EuclideanSolver._is_valid_partial(
                    cand.right_neighbor, cand.polygon_size, cand.glue)

                viz.draw_attempt(cand, ff, i, is_valid,
                                 f"attach {VertexTypeCatalog.symbols[gr]} d={depth}")
                if not is_valid:
                    cand.glue[ff] = -1; cand.glue[i] = -1
                    if not mirrored:
                        cand.glue[cand.mirror[ff]] = -1
                        cand.glue[cand.mirror[i]] = -1
                    continue

                all_glued = all(g != -1 for g in cand.glue)
                if all_glued:
                    viz.draw_state(cand, f"COMPLETE {VertexTypeCatalog.symbols[gr]}")
                else:
                    viz.draw_state(cand, f"attach {VertexTypeCatalog.symbols[gr]}")
                    _extend_geo(viz, cand, max_polygons, depth+1, max_depth, max_frames)

                cand.glue[ff] = -1; cand.glue[i] = -1
                if not mirrored:
                    cand.glue[cand.mirror[ff]] = -1
                    cand.glue[cand.mirror[i]] = -1


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--k", type=int, default=1)
    p.add_argument("--max-frames", type=int, default=200)
    p.add_argument("--max-depth", type=int, default=3)
    p.add_argument("--output", default="frames_geo")
    p.add_argument("--vertex-types", type=str, default="0,1,2")
    args = p.parse_args()

    viz = GeoVisualizer(args.output)
    vt_indices = [int(x.strip()) for x in args.vertex_types.split(",")]

    for vt in vt_indices:
        if viz.frame >= args.max_frames:
            break
        state = _make_initial(vt)
        viz.draw_state(state, f"Initial: {VertexTypeCatalog.symbols[vt]}")
        _extend_geo(viz, state, args.k, max_depth=args.max_depth,
                    max_frames=args.max_frames)

    print(f"Generated {viz.frame} frames in '{args.output}/'")


if __name__ == "__main__":
    main()
