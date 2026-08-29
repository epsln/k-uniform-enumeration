"""
Geometric tiling visualizer — draws actual polygons with edge geometry.
No petal/sector fallback.  For partial states, draws known vertices and
glued edges; for complete rings, draws full polygon outlines.
"""
from __future__ import annotations

import math, os
from typing import List, Tuple, Dict, Optional

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon as MPolygon, FancyArrowPatch

from euclidean_tiling import PartialSolution, VertexTypeCatalog

POLY_COLORS = {3: "#e74c3c", 4: "#3498db", 6: "#2ecc71", 12: "#f39c12"}
EDGE = 1.0


def _poly_radius(sides: int) -> float:
    return EDGE / (2.0 * math.sin(math.pi / sides))


def _interior(sides: int) -> float:
    return math.pi * (sides - 2) / sides


def _poly_verts(cx, cy, sides, rotate=0.0):
    r = _poly_radius(sides)
    return [(cx + r * math.cos(rotate + 2 * math.pi * i / sides),
             cy + r * math.sin(rotate + 2 * math.pi * i / sides))
            for i in range(sides)]


# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------
def _compute_layout(state: PartialSolution):
    """
    BFS layout.  Also identifies all polygon rings (including incomplete ones)
    and computes their edge geometry.
    
    Returns:
      vert_pos:   list of (x,y) per vertex (None = unplaced)
      poly_edges: list of [(x1,y1, x2,y2), ...] per polygon ring (glued edges)
      poly_centers: list of (cx, cy, rot, sides) per polygon ring (for complete ones)
    """
    n_slots = len(state.glue)

    # Map slots to vertices
    slot_vert = [0] * n_slots
    off = 0
    for t, vt in enumerate(state.vertex_types):
        sl = len(VertexTypeCatalog.left_neighbors[vt])
        for s in range(sl):
            slot_vert[off + s] = t
        off += sl

    # Identify polygon rings (complete or not)
    seen = [False] * n_slots
    poly_rings = []       # list of (slot_list, sides)
    for start in range(n_slots):
        if seen[start]:
            continue
        left = start
        while state.glue[left] != -1 and state.glue[left] != state.right_neighbor[start]:
            left = state.left_neighbor[state.glue[left]]
        if state.glue[left] == -1:
            r0 = state.right_neighbor[left]
            sz = state.polygon_size[r0]
            slots = []
            cur = left
            for _ in range(sz + 1):
                seen[cur] = True
                slots.append(cur)
                cur = state.glue[state.right_neighbor[cur]]
                if cur == -1 or cur == left:
                    break
            poly_rings.append((slots, sz))
        else:
            cur = left
            while True:
                seen[cur] = True
                cur = state.glue[state.right_neighbor[cur]]
                if cur == left:
                    break

    # --- BFS layout from vertex 0 ---
    vert_pos = [None] * len(state.vertex_types)
    vert_pos[0] = (0.0, 0.0)
    queue = [0]
    visited = {0}
    placed_edge_dirs = {}  # (v, slot) -> angle of edge away from vertex v

    while queue:
        vtile = queue.pop(0)
        vx, vy = vert_pos[vtile]
        vt = state.vertex_types[vtile]
        sl = len(VertexTypeCatalog.left_neighbors[vt])
        base = sum(len(VertexTypeCatalog.left_neighbors[state.vertex_types[t]])
                   for t in range(vtile))

        # Compute angular orientation of each flag around this vertex.
        # Walk rneig to get cumulative angles from interior angles.
        slot_angle = {}  # slot -> edge direction angle
        start_s = base
        cur = start_s
        cum = 0.0
        for _ in range(sl):
            r = state.right_neighbor[cur]
            sz = state.polygon_size[r]
            ia = _interior(sz)
            slot_angle[cur] = cum  # edge direction for this slot
            cum += ia
            cur = state.right_neighbor[cur]
            if cur == start_s:
                break

        # Follow glued edges to adjacent vertices
        for s in range(base, base + sl):
            r = state.right_neighbor[s]
            g = state.glue[r]
            if g == -1:
                continue
            other = slot_vert[g]
            angle = slot_angle.get(s, 0.0)

            if vert_pos[other] is None:
                # Place the other vertex at distance EDGE along angle
                ox = vx + EDGE * math.cos(angle)
                oy = vy + EDGE * math.sin(angle)
                vert_pos[other] = (ox, oy)

            if other not in visited:
                visited.add(other)
                queue.append(other)

    # --- Compute polygon edge geometry ---
    poly_edges = []    # list of edge lists per polygon
    poly_centers = []  # (cx, cy, rot, sz) for complete polygons
    
    for slots, sz in poly_rings:
        edges = []
        cur = slots[0]
        is_complete = True
        for i in range(sz):
            r = state.right_neighbor[cur]
            g = state.glue[r]
            v_cur = slot_vert[cur]
            if vert_pos[v_cur] is None:
                is_complete = False
                cur = g
                if cur == -1 or cur == slots[0]:
                    break
                continue
            x1, y1 = vert_pos[v_cur]
            if g != -1 and vert_pos[slot_vert[g]] is not None:
                x2, y2 = vert_pos[slot_vert[g]]
                edges.append((x1, y1, x2, y2))
            else:
                is_complete = False
            cur = g
            if cur == -1 or cur == slots[0]:
                break
        poly_edges.append(edges)

        if is_complete and len(edges) == sz:
            # Compute polygon center as centroid of vertices
            xs = [e[0] for e in edges] + [e[2] for e in edges]
            ys = [e[1] for e in edges] + [e[3] for e in edges]
            cx = sum(xs) / len(xs)
            cy = sum(ys) / len(ys)
            poly_centers.append((cx, cy, 0.0, sz))
        else:
            poly_centers.append(None)

    return vert_pos, poly_edges, poly_centers


# ---------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------
def _draw_tiling(ax, state, vert_pos, poly_edges, poly_centers,
                 highlight_pair=None):
    # 1. Compute flag tip positions for every slot
    flag_tips = {}
    off = 0
    for t, vt in enumerate(state.vertex_types):
        pos = vert_pos[t]
        sl = len(VertexTypeCatalog.left_neighbors[vt])
        if pos is None:
            off += sl; continue
        vx, vy = pos
        # Compute cumulative interior angles
        psize = VertexTypeCatalog.polygon_sizes[vt]
        cum = 0.0
        for s in range(sl):
            r = state.right_neighbor[off + s]
            sz = psize[s]
            ia = _interior(sz)
            # flag tip is at edge direction cum, distance EDGE from vertex
            tx = vx + EDGE * math.cos(cum)
            ty = vy + EDGE * math.sin(cum)
            flag_tips[off + s] = (tx, ty)
            cum += ia
        off += sl

    # 2. Draw glued edges
    seen_glue = set()
    for i in range(len(state.glue)):
        g = state.glue[i]
        if g == -1 or i in seen_glue or g in seen_glue:
            continue
        seen_glue.add(i); seen_glue.add(g)
        if i not in flag_tips or g not in flag_tips:
            continue
        x1, y1 = flag_tips[i]
        x2, y2 = flag_tips[g]
        vi = None; vj = None
        so = 0
        for t, vt in enumerate(state.vertex_types):
            sl2 = len(VertexTypeCatalog.left_neighbors[vt])
            if so <= i < so + sl2: vi = t
            if so <= g < so + sl2: vj = t
            so += sl2

        hl = (highlight_pair is not None and
              (i == highlight_pair[0] or i == highlight_pair[1]))
        color = "#ff6600" if hl else "#666666"
        lw = 3.5 if hl else 1.8

        if vi is not None and vj is not None and vi == vj and vert_pos[vi] is not None:
            # Self-loop: draw an arc around the vertex
            cx, cy = vert_pos[vi]
            a1 = math.atan2(y1 - cy, x1 - cx)
            a2 = math.atan2(y2 - cy, x2 - cx)
            if a2 < a1: a2 += 2 * math.pi
            arc_r = math.hypot(x1 - cx, y1 - cy)
            arc_pts = [(cx + arc_r * math.cos(a), cy + arc_r * math.sin(a))
                       for a in [a1 + (a2-a1)*t/30 for t in range(31)]]
            ax.plot([p[0] for p in arc_pts], [p[1] for p in arc_pts],
                    color=color, lw=lw, zorder=4)
        else:
            ax.plot([x1, x2], [y1, y2], color=color, lw=lw, zorder=4,
                    solid_capstyle="round")

    # 3. Complete polygon outlines
    for entry in poly_centers:
        if entry is None:
            continue
        cx, cy, rot, sz = entry
        col = POLY_COLORS.get(sz, "#888")
        pts = _poly_verts(cx, cy, sz, rot)
        p = MPolygon(pts, closed=True, facecolor="none",
                     edgecolor=col, lw=2.5, zorder=3, alpha=0.85)
        ax.add_patch(p)
        ax.text(cx, cy, str(sz), ha="center", va="center", fontsize=11,
                fontweight="bold", color=col, zorder=8,
                bbox=dict(facecolor="white", edgecolor="none", alpha=0.7, pad=1))

    # 3. Highlight attempted pair
    if highlight_pair is not None:
        a, b = highlight_pair
        va = None; vb = None
        for t, pos in enumerate(vert_pos):
            if pos is None: continue
        # Find which vertices contain flags a and b
        off = 0
        for t, vt in enumerate(state.vertex_types):
            sl = len(VertexTypeCatalog.left_neighbors[vt])
            if off <= a < off + sl:
                va = t
            if off <= b < off + sl:
                vb = t
            off += sl
        if va is not None and vb is not None:
            if vert_pos[va] and vert_pos[vb]:
                x1, y1 = vert_pos[va]
                x2, y2 = vert_pos[vb]
                ax.plot([x1, x2], [y1, y2], color="#ff6600", lw=4.0, zorder=9, alpha=0.9,
                        solid_capstyle="round")

    # 4. Vertex dots + labels
    off = 0
    for t, vt in enumerate(state.vertex_types):
        pos = vert_pos[t]
        if pos is None:
            off += sl; continue
        vx, vy = pos
        sl = len(VertexTypeCatalog.left_neighbors[vt])

        # Vertex dot
        ax.plot(vx, vy, "o", color="#222", markersize=10, zorder=6,
                markeredgecolor="white", markeredgewidth=1.5)
        ax.text(vx, vy - 0.35, f"V{t}", ha="center", fontsize=7, color="#555", zorder=7)

        off += sl

    # Autoscale
    all_x, all_y = [], []
    for pos in vert_pos:
        if pos: all_x.append(pos[0]); all_y.append(pos[1])
    for (tx, ty) in flag_tips.values():
        all_x.append(tx); all_y.append(ty)
    for entry in poly_centers:
        if entry is not None:
            cx, cy = entry[0], entry[1]
            all_x.append(cx); all_y.append(cy)
    if all_x:
        pad = 2.5
        ax.set_xlim(min(all_x) - pad, max(all_x) + pad)
        ax.set_ylim(min(all_y) - pad, max(all_y) + pad)
    else:
        ax.set_xlim(-3, 3); ax.set_ylim(-3, 3)
    ax.set_aspect("equal")
    ax.axis("off")


# ---------------------------------------------------------------------------
# Visualizer
# ---------------------------------------------------------------------------
class GeoVisualizer:
    def __init__(self, output_dir="frames"):
        self.output_dir = output_dir
        self.frame = 0
        os.makedirs(output_dir, exist_ok=True)

    def _save(self, fig):
        path = os.path.join(self.output_dir, f"img_{self.frame:04d}.png")
        fig.savefig(path, dpi=120, bbox_inches=None, pad_inches=0.2)
        plt.close(fig)
        self.frame += 1

    def _make_fig(self, title, border=None):
        fig, ax = plt.subplots(figsize=(10, 10))
        ax.set_title(title, fontsize=11, color="#333" if border is None else border)
        if border:
            rect = plt.Rectangle((0, 0), 1, 1, transform=fig.transFigure,
                                 facecolor="none", edgecolor=border, lw=4)
            fig.add_artist(rect)
        return fig, ax

    def draw_state(self, state: PartialSolution, title=""):
        label = ", ".join(VertexTypeCatalog.symbols[v] for v in state.vertex_types)
        t = f"{label}\n{title}" if title else label
        fig, ax = self._make_fig(t)
        try:
            vp, pe, pc = _compute_layout(state)
        except Exception as e:
            ax.text(0, 0, f"Layout error:\n{e}", ha="center", va="center", fontsize=10, color="red")
            vp, pe, pc = [], [], []
        _draw_tiling(ax, state, vp, pe, pc)
        self._save(fig)

    def draw_attempt(self, state, ff, target, is_valid, title=""):
        label = ", ".join(VertexTypeCatalog.symbols[v] for v in state.vertex_types)
        status = "VALID" if is_valid else "INVALID"
        border = "#2ecc71" if is_valid else "#e74c3c"
        t = f"{label}\nglue {state.label[ff]}↔{state.label[target]}: {status}"
        if title:
            t += f"\n{title}"
        fig, ax = self._make_fig(t, border)
        try:
            vp, pe, pc = _compute_layout(state)
        except Exception as e:
            ax.text(0, 0, f"Layout error:\n{e}", ha="center", va="center", fontsize=10, color="red")
            vp, pe, pc = [], [], []
        _draw_tiling(ax, state, vp, pe, pc, (ff, target))
        self._save(fig)
