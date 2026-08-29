"""
Tiling gluing visualizer — renders partial solutions as petal diagrams.
Output: frames/img_NNNN.png with green/red borders for valid/invalid attempts.
"""
from __future__ import annotations

import os
import math
from typing import List, Tuple, Optional

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.path import Path
import matplotlib.patheffects as pe

from euclidean_tiling import PartialSolution, VertexTypeCatalog

# ---------------------------------------------------------------------------
# Colour map
# ---------------------------------------------------------------------------
POLY_COLORS = {3: "#e74c3c", 4: "#3498db", 6: "#2ecc71", 12: "#f39c12"}
POLY_LIGHT  = {3: "#fadbd8", 4: "#d6eaf8", 6: "#d5f5e3", 12: "#fdebd0"}

# ---------------------------------------------------------------------------
# Layout helpers
# ---------------------------------------------------------------------------
def _vertex_angle(i: int, total: int) -> float:
    """Place vertex i on a circle. Vertex 0 at 12-o'clock."""
    return math.pi / 2 - 2 * math.pi * i / total


def _flag_angles(vertype: int, vertex_angle: float) -> List[Tuple[float, float, int, int]]:
    """Return (angle_start, angle_end, polygon_size, flag_index) for each flag
    of this vertex, in slot-index order (0..sl-1)."""
    sl = len(VertexTypeCatalog.left_neighbors[vertype])
    psize = VertexTypeCatalog.polygon_sizes[vertype]

    gap = 0.02
    sweep = 2 * math.pi / sl
    base = vertex_angle - math.pi / 2

    result = []
    for fi in range(sl):
        a0 = base + fi * sweep + gap / 2
        a1 = a0 + sweep - gap
        result.append((a0, a1, psize[fi], fi))
    return result


def _arc_path(
    ax, x1, y1, x2, y2, color: str, lw: float = 2.5, highlight: bool = False,
):
    """Draw a cubic bezier arc between two points. For same-vertex arcs,
    bulge outward from center. For cross-vertex arcs, bulge toward origin."""
    dx = x2 - x1
    dy = y2 - y1
    dist = math.hypot(dx, dy)

    if dist < 1e-6:  # self-loop
        ctrl1 = (x1 + 0.4, y1 - 0.4)
        ctrl2 = (x1 + 0.4, y2 + 0.4)
    else:
        mx = (x1 + x2) / 2
        my = (y1 + y2) / 2
        nx = -dy / dist * dist * 0.35
        ny = dx / dist * dist * 0.35
        ctrl1 = (x1 + dx * 0.3 + nx, y1 + dy * 0.3 + ny)
        ctrl2 = (x2 - dx * 0.3 + nx, y2 - dy * 0.3 + ny)

    verts = [(x1, y1), ctrl1, ctrl2, (x2, y2)]
    codes = [Path.MOVETO, Path.CURVE4, Path.CURVE4, Path.CURVE4]
    path = Path(verts, codes)

    if highlight:
        ax.plot([x1, x2], [y1, y2], color="#ffd700", lw=lw + 4, alpha=0.6, zorder=10)
        ax.plot([x1, x2], [y1, y2], color="#ff6600", lw=lw + 2, alpha=0.8, zorder=11)

    patch = mpatches.PathPatch(path, edgecolor=color, facecolor="none", lw=lw, zorder=5)
    ax.add_patch(patch)


# ---------------------------------------------------------------------------
# Drawing
# ---------------------------------------------------------------------------
def _flag_tip_position(
    vertex_xy: Tuple[float, float], angle: float, radius: float = 2.2
) -> Tuple[float, float]:
    return (vertex_xy[0] + radius * math.cos(angle),
            vertex_xy[1] + radius * math.sin(angle))


def _label_position(
    vertex_xy: Tuple[float, float], angle: float, radius: float = 1.5
) -> Tuple[float, float]:
    return (vertex_xy[0] + radius * math.cos(angle),
            vertex_xy[1] + radius * math.sin(angle))


# ---------------------------------------------------------------------------
# Per-vertex flag offsets
# ---------------------------------------------------------------------------
def _compute_slot_positions(state: PartialSolution) -> dict:
    """Map global slot index -> (x, y, angle, polygon_size) for flag-tip position."""
    pos = {}
    # Determine which slots belong to which vertex
    slot = 0
    for tile_idx, vt in enumerate(state.vertex_types):
        sl = len(VertexTypeCatalog.left_neighbors[vt])
        vx, vy = _vertex_position(tile_idx, len(state.vertex_types))
        va = _vertex_angle(tile_idx, len(state.vertex_types))
        flags = _flag_angles(vt, va)
        for s in range(sl):
            a0, a1, psz, fi = flags[s]
            mid = (a0 + a1) / 2
            px, py = _flag_tip_position((vx, vy), mid)
            pos[slot + s] = (px, py, mid, psz)
        slot += sl
    return pos


def _vertex_position(i: int, total: int) -> Tuple[float, float]:
    if total <= 1:
        return (0.0, 0.0)
    angle = _vertex_angle(i, total)
    radius = total * 1.6
    return (radius * math.cos(angle), radius * math.sin(angle))


def _draw_vertex(ax, state: PartialSolution, tile_idx: int, slot_positions: dict):
    """Draw one vertex as a circle with colored petal sectors."""
    vt = state.vertex_types[tile_idx]
    total = len(state.vertex_types)
    vx, vy = _vertex_position(tile_idx, total)
    va = _vertex_angle(tile_idx, total)

    # Find which slots belong to this vertex
    slot_start = 0
    for t in range(tile_idx):
        slot_start += len(VertexTypeCatalog.left_neighbors[state.vertex_types[t]])
    sl = len(VertexTypeCatalog.left_neighbors[vt])

    flags = _flag_angles(vt, va)

    for (a0, a1, psz, fi), s in zip(flags, range(sl)):
        color = POLY_COLORS.get(psz, "#888888")
        light = POLY_LIGHT.get(psz, "#eeeeee")

        # Sector (petal)
        wedge = mpatches.Wedge(
            (vx, vy), 2.2, math.degrees(a0), math.degrees(a1),
            width=0.6, facecolor=light, edgecolor=color, lw=1.0, zorder=2,
        )
        ax.add_patch(wedge)

        # Label
        mid = (a0 + a1) / 2
        lx, ly = _label_position((vx, vy), mid, 1.5)
        ax.text(lx, ly, str(psz), ha="center", va="center", fontsize=8,
                fontweight="bold", color=color, zorder=6)

    # Center dot
    ax.plot(vx, vy, "o", color="#333333", markersize=10, zorder=7)
    ax.text(vx, vy - 0.35, f"V{tile_idx}", ha="center", va="top",
            fontsize=7, color="#555555", zorder=7)


def _draw_glue_arcs(ax, state: PartialSolution, slot_positions: dict,
                    highlight_pair: Optional[Tuple[int, int]] = None):
    """Draw bezier arcs for each glued pair."""
    seen = set()
    for i in range(len(state.glue)):
        g = state.glue[i]
        if g == -1 or i in seen:
            continue
        seen.add(i)
        seen.add(g)

        p1 = slot_positions[i]
        p2 = slot_positions[g]

        hl = (highlight_pair is not None and
              (i == highlight_pair[0] or i == highlight_pair[1]))
        _arc_path(ax, p1[0], p1[1], p2[0], p2[1],
                  color="#555555", lw=1.5, highlight=hl)


def _vertex_type_label(state: PartialSolution) -> str:
    parts = [VertexTypeCatalog.symbols[vt] for vt in state.vertex_types]
    return ", ".join(parts)


# ---------------------------------------------------------------------------
# Visualizer class
# ---------------------------------------------------------------------------
class Visualizer:
    def __init__(self, output_dir: str = "frames", figsize=(8, 8)):
        self.output_dir = output_dir
        self.figsize = figsize
        self.frame = 0
        os.makedirs(output_dir, exist_ok=True)

    def _save(self, fig: plt.Figure):
        path = os.path.join(self.output_dir, f"img_{self.frame:04d}.png")
        fig.savefig(path, dpi=120, bbox_inches=None, pad_inches=0.2)
        plt.close(fig)
        self.frame += 1

    def _draw(
        self,
        state: PartialSolution,
        title: str = "",
        border_color: str = "#ffffff",
        highlight_pair: Optional[Tuple[int, int]] = None,
    ):
        fig, ax = plt.subplots(figsize=self.figsize, subplot_kw={"aspect": "equal"})

        nv = len(state.vertex_types)
        radius = max(nv * 1.8, 3.5)
        ax.set_xlim(-radius, radius)
        ax.set_ylim(-radius, radius)
        ax.axis("off")

        slot_positions = _compute_slot_positions(state)

        # Draw glue arcs first (behind vertices)
        _draw_glue_arcs(ax, state, slot_positions, highlight_pair)

        # Draw vertices
        for t in range(nv):
            _draw_vertex(ax, state, t, slot_positions)

        # Title
        ax.set_title(title, fontsize=11, pad=12, color="#333333")

        # Border
        rect = plt.Rectangle(
            (0, 0), 1, 1, transform=fig.transFigure,
            facecolor="none", edgecolor=border_color, lw=4,
        )
        fig.add_artist(rect)

        # Conway string footer
        cw = _conway_str(state)
        fig.text(0.5, 0.01, cw, ha="center", fontsize=7, color="#888888")

        self._save(fig)

    def draw_state(self, state: PartialSolution, title: str = ""):
        """Draw a partial solution with white border."""
        label = _vertex_type_label(state)
        full_title = f"{label}\n{title}" if title else label
        self._draw(state, full_title, border_color="#ffffff")

    def draw_attempt(
        self,
        state: PartialSolution,
        ff: int,
        target: int,
        is_valid: bool,
        title: str = "",
    ):
        """Draw an attempt: highlight ff↔target, green/red border."""
        label = _vertex_type_label(state)
        status = "VALID" if is_valid else "INVALID"
        border = "#2ecc71" if is_valid else "#e74c3c"
        full = f"{label}\nglue {state.label[ff]}↔{state.label[target]}: {status}"
        if title:
            full += f"\n{title}"
        self._draw(state, full, border_color=border,
                   highlight_pair=(ff, target))


def _conway_str(state: PartialSolution) -> str:
    """Minimal Conway symbol for footer."""
    seen = set()
    parts = []
    for i in range(len(state.glue)):
        g = state.glue[i]
        if g == -1 or i in seen:
            continue
        seen.add(i)
        seen.add(g)
        a, b = state.label[i], state.label[g]
        mc = int(a.startswith("*")) + int(b.startswith("*"))
        a0 = a.lstrip("*")
        b0 = b.lstrip("*")
        if mc == 1:
            parts.append(f"[{a0} {b0}]" if a0 != b0 else f"[{a0}]")
        else:
            parts.append(f"({a0} {b0})" if a0 != b0 else f"({a0})")
    return "".join(parts) if parts else "(empty)"
