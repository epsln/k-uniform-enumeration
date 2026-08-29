"""Euclidean k-uniform tiling solver and pruner.

This module enumerates Euclidean k-uniform tilings by exhaustively extending
partial vertex-configuration assignments edge by edge (the `EuclideanSolver`),
then deduplicates the resulting solutions by collapsing labelings that are
related by a graph automorphism (the `SolutionPruner`).

This is a class-based, type-hinted refactor of two original scripts
(`euclidean_solver_mega.py` and `euclidean_pruner.py`), merged so that the
pruner runs directly on the solver's output without any manual file-path
bookkeeping.

Background on the representation
---------------------------------
A tiling is built up as a set of "vertex types" (Euclidean Conway-style
vertex configurations, e.g. ``(3,12,12)A``), each contributing a fixed
number of *edge slots*. Every edge slot has:

  * a ``polygon_size``: the size of the polygon that lies to its right,
  * a ``right_neighbor`` / ``left_neighbor``: the next slot clockwise /
    counter-clockwise around the same vertex,
  * a ``mirror``: the slot's mirror-symmetric counterpart,
  * a ``label``: a short text identifier used to print Conway gluing symbols.

Building a tiling means *gluing* pairs of edge slots together (the ``glue``
array) until every slot is paired and every implied polygon closes at the
correct size.
"""

from __future__ import annotations

import io
import multiprocessing
import os
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, TextIO, Tuple, Union


# =============================================================================
# Static vertex-type geometry catalogue (shared by the solver and the pruner)
# =============================================================================


class VertexTypeCatalog:
    """The fixed catalogue of all Euclidean vertex types the program knows
    about, and the per-slot geometric data describing each one.

    All class-level lists are indexed first by vertex-type id (0..count()-1)
    and then, where applicable, by edge-slot id within that vertex type.
    """

    #: Conway-style symbol for each vertex type, e.g. "(3,12,12)A".
    symbols: List[str] = [
        "(3,12,12)A", "(3,12,12)F", "(4,6,12)", "(6,6,6)S", "(6,6,6)R", "(6,6,6)A", "(6,6,6)F",
        "(3,3,4,12)", "(3,4,3,12)A", "(3,4,3,12)F", "(3,3,6,6)A", "(3,3,6,6)F", "(3,6,3,6)S",
        "(3,6,3,6)R", "(3,6,3,6)A1", "(3,6,3,6)A2", "(3,6,3,6)F", "(3,4,4,6)", "(3,4,6,4)A",
        "(3,4,6,4)F", "(4,4,4,4)S4", "(4,4,4,4)R4", "(4,4,4,4)S2a", "(4,4,4,4)S2b",
        "(4,4,4,4)R2", "(4,4,4,4)A1", "(4,4,4,4)A2", "(4,4,4,4)F", "(3,3,3,3,6)A",
        "(3,3,3,3,6)F", "(3,3,3,4,4)A", "(3,3,3,4,4)F", "(3,3,4,3,4)A", "(3,3,4,3,4)F",
        "(3,3,3,3,3,3)S6", "(3,3,3,3,3,3)R6", "(3,3,3,3,3,3)S3a", "(3,3,3,3,3,3)S3b",
        "(3,3,3,3,3,3)R3", "(3,3,3,3,3,3)S2", "(3,3,3,3,3,3)R2", "(3,3,3,3,3,3)A1",
        "(3,3,3,3,3,3)A2", "(3,3,3,3,3,3)F",
    ]

    #: Raw per-slot label templates (e.g. "0", "1", "*0"). When a vertex of
    #: this type is attached as the Nth polygon in a tiling, each template is
    #: passed through `ConwayCycleWriter.edge_label(template, N)` to produce
    #: the slot's final, tiling-unique label.
    edge_label_templates: List[List[str]] = [
        ["0", "1", "*0"], ["0", "1", "2", "*0", "*1", "*2"], ["0", "1", "2", "*0", "*1", "*2"],
        ["0"], ["0", "*0"], ["0", "1", "*1"], ["0", "1", "2", "*0", "*1", "*2"],
        ["0", "1", "2", "3", "*0", "*1", "*2", "*3"], ["0", "*0", "2", "*2"],
        ["0", "1", "2", "3", "*0", "*1", "*2", "*3"], ["0", "1", "*0", "3"],
        ["0", "1", "2", "3", "*0", "*1", "*2", "*3"], ["0", "*0"], ["0", "1", "*0", "*1"],
        ["0", "1", "*1", "*0"], ["0", "*0", "2", "*2"], ["0", "1", "2", "3", "*0", "*1", "*2", "*3"],
        ["0", "1", "2", "3", "*0", "*1", "*2", "*3"], ["0", "*0", "2", "*2"],
        ["0", "1", "2", "3", "*0", "*1", "*2", "*3"], ["0"], ["0", "*0"], ["0", "1"], ["0", "*0"],
        ["0", "1", "*0", "*1"], ["0", "1", "2", "*1"], ["0", "*0", "2", "*2"],
        ["0", "1", "2", "3", "*0", "*1", "*2", "*3"], ["0", "*0", "2", "3", "*2"],
        ["0", "1", "2", "3", "4", "*0", "*1", "*2", "*3", "*4"], ["0", "1", "*0", "3", "*3"],
        ["0", "1", "2", "3", "4", "*0", "*1", "*2", "*3", "*4"], ["0", "1", "*1", "*0", "4"],
        ["0", "1", "2", "3", "4", "*0", "*1", "*2", "*3", "*4"], ["0"], ["0", "*0"], ["0", "1"],
        ["0", "*0"], ["0", "1", "*0", "*1"], ["0", "1", "*1"], ["0", "1", "2", "*0", "*1", "*2"],
        ["0", "1", "2", "3", "*2", "*1"], ["0", "1", "2", "*2", "*1", "*0"],
        ["0", "1", "2", "3", "4", "5", "*0", "*1", "*2", "*3", "*4", "*5"],
    ]

    #: For each slot, the index (within the same vertex) of the next slot
    #: counter-clockwise.
    left_neighbors: List[List[int]] = [
        [2, 0, 1], [2, 0, 1, 4, 5, 3], [2, 0, 1, 4, 5, 3], [0], [0, 1], [2, 0, 1],
        [2, 0, 1, 4, 5, 3], [3, 0, 1, 2, 5, 6, 7, 4], [3, 0, 1, 2], [3, 0, 1, 2, 5, 6, 7, 4],
        [3, 0, 1, 2], [3, 0, 1, 2, 5, 6, 7, 4], [1, 0], [1, 0, 3, 2], [3, 0, 1, 2], [3, 0, 1, 2],
        [3, 0, 1, 2, 5, 6, 7, 4], [3, 0, 1, 2, 5, 6, 7, 4], [3, 0, 1, 2], [3, 0, 1, 2, 5, 6, 7, 4],
        [0], [0, 1], [1, 0], [1, 0], [1, 0, 3, 2], [3, 0, 1, 2], [3, 0, 1, 2],
        [3, 0, 1, 2, 5, 6, 7, 4], [4, 0, 1, 2, 3], [4, 0, 1, 2, 3, 6, 7, 8, 9, 5], [4, 0, 1, 2, 3],
        [4, 0, 1, 2, 3, 6, 7, 8, 9, 5], [4, 0, 1, 2, 3], [4, 0, 1, 2, 3, 6, 7, 8, 9, 5], [0],
        [0, 1], [1, 0], [1, 0], [1, 0, 3, 2], [2, 0, 1], [2, 0, 1, 4, 5, 3],
        [5, 0, 1, 2, 3, 4], [5, 0, 1, 2, 3, 4], [5, 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 6],
    ]

    #: For each slot, the index (within the same vertex) of the next slot
    #: clockwise.
    right_neighbors: List[List[int]] = [
        [1, 2, 0], [1, 2, 0, 5, 3, 4], [1, 2, 0, 5, 3, 4], [0], [0, 1], [1, 2, 0],
        [1, 2, 0, 5, 3, 4], [1, 2, 3, 0, 7, 4, 5, 6], [1, 2, 3, 0], [1, 2, 3, 0, 7, 4, 5, 6],
        [1, 2, 3, 0], [1, 2, 3, 0, 7, 4, 5, 6], [1, 0], [1, 0, 3, 2], [1, 2, 3, 0], [1, 2, 3, 0],
        [1, 2, 3, 0, 7, 4, 5, 6], [1, 2, 3, 0, 7, 4, 5, 6], [1, 2, 3, 0], [1, 2, 3, 0, 7, 4, 5, 6],
        [0], [0, 1], [1, 0], [1, 0], [1, 0, 3, 2], [1, 2, 3, 0], [1, 2, 3, 0],
        [1, 2, 3, 0, 7, 4, 5, 6], [1, 2, 3, 4, 0], [1, 2, 3, 4, 0, 9, 5, 6, 7, 8], [1, 2, 3, 4, 0],
        [1, 2, 3, 4, 0, 9, 5, 6, 7, 8], [1, 2, 3, 4, 0], [1, 2, 3, 4, 0, 9, 5, 6, 7, 8], [0],
        [0, 1], [1, 0], [1, 0], [1, 0, 3, 2], [1, 2, 0], [1, 2, 0, 5, 3, 4],
        [1, 2, 3, 4, 5, 0], [1, 2, 3, 4, 5, 0], [1, 2, 3, 4, 5, 0, 11, 6, 7, 8, 9, 10],
    ]

    #: For each slot, the index (within the same vertex) of its mirror-image
    #: slot.
    mirrors: List[List[int]] = [
        [2, 1, 0], [3, 4, 5, 0, 1, 2], [3, 4, 5, 0, 1, 2], [0], [1, 0], [0, 2, 1],
        [3, 4, 5, 0, 1, 2], [4, 5, 6, 7, 0, 1, 2, 3], [1, 0, 3, 2], [4, 5, 6, 7, 0, 1, 2, 3],
        [2, 1, 0, 3], [4, 5, 6, 7, 0, 1, 2, 3], [1, 0], [2, 3, 0, 1], [3, 2, 1, 0], [1, 0, 3, 2],
        [4, 5, 6, 7, 0, 1, 2, 3], [4, 5, 6, 7, 0, 1, 2, 3], [1, 0, 3, 2], [4, 5, 6, 7, 0, 1, 2, 3],
        [0], [1, 0], [0, 1], [1, 0], [2, 3, 0, 1], [0, 3, 2, 1], [1, 0, 3, 2],
        [4, 5, 6, 7, 0, 1, 2, 3], [1, 0, 4, 3, 2], [5, 6, 7, 8, 9, 0, 1, 2, 3, 4], [2, 1, 0, 4, 3],
        [5, 6, 7, 8, 9, 0, 1, 2, 3, 4], [3, 2, 1, 0, 4], [5, 6, 7, 8, 9, 0, 1, 2, 3, 4], [0],
        [1, 0], [0, 1], [1, 0], [2, 3, 0, 1], [0, 2, 1], [3, 4, 5, 0, 1, 2],
        [0, 5, 4, 3, 2, 1], [5, 4, 3, 2, 1, 0], [6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5],
    ]

    #: For each slot, the size (3, 4, 6, 12, ...) of the polygon that lies to
    #: its right.
    polygon_sizes: List[List[int]] = [
        [3, 12, 12], [3, 12, 12, 12, 12, 3], [4, 12, 6, 12, 6, 4], [6], [6, 6], [6, 6, 6],
        [6, 6, 6, 6, 6, 6], [3, 12, 4, 3, 12, 4, 3, 3], [3, 12, 3, 4], [3, 12, 3, 4, 12, 3, 4, 3],
        [3, 6, 6, 3], [3, 6, 6, 3, 6, 6, 3, 3], [3, 6], [3, 6, 6, 3], [3, 6, 3, 6], [3, 6, 3, 6],
        [3, 6, 3, 6, 6, 3, 6, 3], [3, 6, 4, 4, 6, 4, 4, 3], [4, 6, 4, 3], [4, 6, 4, 3, 6, 4, 3, 4],
        [4], [4, 4], [4, 4], [4, 4], [4, 4, 4, 4], [4, 4, 4, 4], [4, 4, 4, 4],
        [4, 4, 4, 4, 4, 4, 4, 4], [3, 6, 3, 3, 3], [3, 6, 3, 3, 3, 6, 3, 3, 3, 3],
        [3, 4, 4, 3, 3], [3, 4, 4, 3, 3, 4, 4, 3, 3, 3], [3, 4, 3, 4, 3],
        [3, 4, 3, 4, 3, 4, 3, 4, 3, 3], [3], [3, 3], [3, 3], [3, 3], [3, 3, 3, 3], [3, 3, 3],
        [3, 3, 3, 3, 3, 3], [3, 3, 3, 3, 3, 3], [3, 3, 3, 3, 3, 3],
        [3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3],
    ]
    idx = 18 
    print(f"rn: {right_neighbors[idx]}, ln: {left_neighbors[idx]}, mir: {mirrors[idx]}, ps: {polygon_sizes[idx]}")
    assert False

    #: A short alphanumeric code per vertex type, used to build compact
    #: filenames.
    codes: List[str] = [
        "3a", "3b", "3c", "3d", "3e", "3f", "3g", "4a", "4b", "4c", "4d", "4e", "4f", "4g", "4h",
        "4i", "4j", "4k", "4l", "4m", "4n", "4o", "4p", "4q", "4r", "4s", "4t", "4u", "5a", "5b",
        "5c", "5d", "5e", "5f", "6a", "6b", "6c", "6d", "6e", "6f", "6g", "6h", "6i", "6j",
    ]

    # Vertex types whose nominal attachment symmetry means only a fraction of
    # their slots ever need to be tried as the "first" attachment point when
    # adding a brand-new vertex; trying the rest only produces duplicates.
    _half_symmetry = {
        "(3,12,12)F", "(6,6,6)R", "(3,4,3,12)F", "(3,3,6,6)F", "(3,6,3,6)R", "(3,4,6,4)F",
        "(4,4,4,4)R4", "(4,4,4,4)S2a", "(4,4,4,4)S2b", "(4,4,4,4)A1", "(3,3,3,3,6)F",
        "(3,3,3,4,4)F", "(3,3,4,3,4)F", "(3,3,3,3,3,3)R6", "(3,3,3,3,3,3)S3a",
        "(3,3,3,3,3,3)S3b", "(3,3,3,3,3,3)A1", "(3,3,3,3,3,3)A2",
    }
    _quarter_symmetry = {"(3,6,3,6)F", "(4,4,4,4)R2", "(4,4,4,4)A2", "(3,3,3,3,3,3)R3"}
    _sixth_symmetry = {"(6,6,6)F", "(3,3,3,3,3,3)R2"}
    _eighth_symmetry = {"(4,4,4,4)F"}
    _twelfth_symmetry = {"(3,3,3,3,3,3)F"}

    @classmethod
    def count(cls) -> int:
        """Number of known vertex types."""
        return len(cls.symbols)

    @classmethod
    def attachment_limit(cls, vertex_type: int) -> int:
        """How many of a newly-attached vertex's slots are worth trying as
        the attachment point.

        Certain vertex types have fewer-than-nominal symmetry, so several of
        their slots are equivalent ways of attaching the same new vertex.
        Trying only the canonical fraction avoids generating solutions that
        are trivial relabelings of each other.
        """
        symbol = cls.symbols[vertex_type]
        slot_count = len(cls.left_neighbors[vertex_type])
        if symbol in cls._half_symmetry:
            return slot_count // 2
        if symbol in cls._quarter_symmetry:
            return slot_count // 4
        if symbol in cls._sixth_symmetry:
            return slot_count // 6
        if symbol in cls._eighth_symmetry:
            return slot_count // 8
        if symbol in cls._twelfth_symmetry:
            return slot_count // 12
        return slot_count


# =============================================================================
# Partial / complete tiling state
# =============================================================================


@dataclass
class PartialSolution:
    """A (possibly incomplete) tiling: a set of vertex-type instances and a
    partial gluing of their edge slots.

    Every list below is indexed by a single, flat "slot id" running across
    all vertex instances placed so far (vertex 0's slots first, then vertex
    1's, etc).
    """

    right_neighbor: List[int]
    left_neighbor: List[int]
    polygon_size: List[int]
    mirror: List[int]
    glue: List[int]        #: -1 for an unglued (free) slot, else the paired slot id.
    label: List[str]
    vertex_types: List[int]  #: vertex-type id of each placed vertex instance, in order.
    num_polygons: int        #: number of vertex instances placed so far.

    def copy(self) -> "PartialSolution":
        return PartialSolution(
            right_neighbor=self.right_neighbor.copy(),
            left_neighbor=self.left_neighbor.copy(),
            polygon_size=self.polygon_size.copy(),
            mirror=self.mirror.copy(),
            glue=self.glue.copy(),
            label=self.label.copy(),
            vertex_types=self.vertex_types.copy(),
            num_polygons=self.num_polygons,
        )


# =============================================================================
# Text rendering helpers
# =============================================================================


class SignatureFormatter:
    """Human-readable text renderings of a vertex-type multiset."""

    @staticmethod
    def verbal_vertices(vertex_types: List[int]) -> str:
        """E.g. "(3,12,12)A, (3,12,12)A, (4,6,12)" -- one entry per vertex,
        in placement order, with repeats spelled out."""
        return ", ".join(VertexTypeCatalog.symbols[i] for i in vertex_types)

    @staticmethod
    def _render_counts(counts: List[int]) -> str:
        parts = []
        for i, count in enumerate(counts):
            if count > 0:
                text = VertexTypeCatalog.symbols[i]
                if count > 1:
                    text += f"x{count}"
                parts.append(text)
        return ", ".join(parts)

    @classmethod
    def signature(cls, vertex_types: List[int]) -> str:
        """E.g. "(3,12,12)Ax2, (4,6,12)" -- vertex types grouped and counted,
        in catalogue order. All solutions with the same vertex multiset share
        the same signature."""
        counts = [0] * VertexTypeCatalog.count()
        for vertex_type in vertex_types:
            counts[vertex_type] += 1
        return cls._render_counts(counts)

    @classmethod
    def file_signature(cls, vertex_types: List[int]) -> str:
        """Like `signature`, but using the short vertex-type codes; used to
        build compact filenames."""
        counts = [0] * VertexTypeCatalog.count()
        for vertex_type in vertex_types:
            counts[vertex_type] += 1
        parts = []
        for i, count in enumerate(counts):
            if count > 0:
                text = VertexTypeCatalog.codes[i]
                if count > 1:
                    text += str(count)
                parts.append(text)
        return " ".join(parts)


class ConwayCycleWriter:
    """Renders a completed tiling's polygon cycles as Conway gluing symbols,
    and writes the corresponding HyperRogue ``.tes`` file.

    This logic is shared between the solver (which calls it once per
    discovered solution) and the pruner (which calls it again after
    deduplication, on the surviving solutions).
    """

    @staticmethod
    def edge_label(edge: Union[str, int], tile: int) -> str:
        """Text representation of edge slot `edge`, belonging to the `tile`-th
        placed vertex. Tiles 0-3 are distinguished by trailing apostrophes;
        higher tile numbers use an explicit "@tile" suffix."""
        text = str(edge)
        if tile > 3:
            text += f"@{tile}"
        else:
            text += "'" * tile
        return text

    @staticmethod
    def conway_symbol(first: str, second: str) -> str:
        """Renders the pairing of two edge labels as a Conway symbol: plain
        parentheses for a normal pairing, square brackets if exactly one side
        is a mirrored edge."""
        mirror_count = 0
        if first.startswith("*"):
            mirror_count += 1
            first = first[1:]
        if second.startswith("*"):
            mirror_count += 1
            second = second[1:]
        body = first if first == second else f"{first} {second}"
        return f"[{body}]" if mirror_count == 1 else f"({body})"

    @classmethod
    def write_conway(cls, mirror: List[int], glue: List[int], label: List[str]) -> str:
        """The full Conway symbol describing every glued edge pair."""
        seen: Set[int] = set()
        parts = []
        for i in range(len(glue)):
            if i not in seen and glue[i] != -1:
                parts.append(cls.conway_symbol(label[i], label[glue[i]]))
                seen.update((i, glue[i], mirror[i], glue[mirror[i]]))
        return "".join(parts)

    @classmethod
    def write_cycle_final(
        cls,
        state: PartialSolution,
        out: TextIO,
        tes_path: str,
        solution_label: str,
    ) -> None:
        """Writes a human-readable description of every polygon cycle in a
        *completed* tiling to `out`, then assembles and writes its ``.tes``
        file at `tes_path`."""
        right_neighbor, polygon_size, mirror, glue, label = (
            state.right_neighbor, state.polygon_size, state.mirror, state.glue, state.label,
        )
        seen: Set[int] = set()
        main_lines: List[str] = []
        # 0 = standalone cycle, 1 = first half of a mirror pair, 2 = second half.
        sub_kind: List[int] = []
        ultrachiral = True
        repeat_list: List[int] = []

        for start in range(len(glue)):
            if start in seen:
                continue
            text, count, min_mirror, v = cls._walk_cycle(
                start, start, right_neighbor, polygon_size, mirror, glue, label, seen,
            )
            ratio = v // count
            repeat_list.append(ratio)
            main_lines.append(f"[{text}]x{ratio}" if ratio != 1 else text)

            if min_mirror in seen:
                sub_kind.append(0)
                ultrachiral = False
            else:
                text, count, _, v = cls._walk_cycle(
                    min_mirror, min_mirror, right_neighbor, polygon_size, mirror, glue, label, seen,
                )
                repeat_list.append(ratio)
                main_lines.append(f"[{text}]x{ratio}" if ratio != 1 else text)
                sub_kind += [1, 2]

        subheader = ""
        for m, (text, kind) in enumerate(zip(main_lines, sub_kind)):
            if kind == 0:
                out.write(f"{m}: {text}")
            elif kind == 1:
                header = f"{m}/{m + 1}: " if not ultrachiral else f"{m // 2}: "
                subheader = " " * len(header)
                out.write(header + text)
            else:
                out.write(subheader + text)
            out.write("\n")
        out.write("---\n")

        if ultrachiral:
            conway, polygon_sizes = cls._assemble_chiral(main_lines, repeat_list)
        else:
            conway, polygon_sizes = cls._assemble_achiral(main_lines, repeat_list)
        out.write(conway + "\n")
        cls._write_tes_file(tes_path, conway, polygon_sizes, repeat_list, solution_label)

    @staticmethod
    def _walk_cycle(
        start: int,
        left: int,
        right_neighbor: List[int],
        polygon_size: List[int],
        mirror: List[int],
        glue: List[int],
        label: List[str],
        seen: Set[int],
    ) -> Tuple[str, int, int, int]:
        """Walks one polygon cycle starting at slot `left`, marking every
        visited slot in `seen`. Returns (description text, slot count, lowest
        mirror-slot seen along the way, polygon size)."""
        right = right_neighbor[left]
        v = polygon_size[right]
        min_mirror = len(glue)
        text = ""
        count = 0
        while True:
            seen.add(left)
            if mirror[right] < min_mirror:
                min_mirror = mirror[right]
            text += f"{label[left]}/{label[right]}({polygon_size[right]})-"
            count += 1
            left = glue[right]
            if left != start:
                right = right_neighbor[left]
            else:
                break
        return text[:-1], count, min_mirror, v

    @classmethod
    def _build_edge_lists(
        cls, main_lines: List[str], repeat_list: List[int],
    ) -> Tuple[List[str], List[str], List[str], List[int]]:
        """Splits each polygon's cycle description into individual tile
        edges, ready for matching into a tile-adjacency Conway symbol."""
        left_edges: List[str] = []
        right_edges: List[str] = []
        edges: List[str] = []
        polygon_sizes: List[int] = []
        for m, raw_text in enumerate(main_lines):
            text = raw_text
            if repeat_list[m] > 1:
                text = text[1:text.index("]")]
            size = int(text[text.index("(") + 1:text.index(")")])
            polygon_sizes.append(size)
            text += "-"
            # Rotate the walk so it starts right after its last "/".
            rev = len(text) - 1
            while text[rev] != "/":
                rev -= 1
            text = text[rev + 1:] + text[:rev + 1]
            edge_index = 0
            while text:
                ind = text.index("/")
                chunk, text = text[:ind + 1], text[ind + 1:]
                left_edges.append(chunk[:chunk.index("(")])
                right_edges.append(chunk[chunk.index("-") + 1:-1])
                edges.append(cls.edge_label(edge_index, m))
                edge_index += 1
        return left_edges, right_edges, edges, polygon_sizes

    @staticmethod
    def _match_edges(
        left_edges: List[str], right_edges: List[str], edges: List[str], allow_mirror_fallback: bool,
    ) -> str:
        """Greedily pairs up tile edges into a Conway tile-adjacency symbol.
        A normal pairing (edge appears as someone's right edge) uses round
        brackets; for chiral assemblies, an edge with no right-edge match is
        paired with its mirror-image edge instead, using square brackets."""
        conway = ""
        while left_edges:
            if right_edges.count(left_edges[0]) > 0:
                match = right_edges.index(left_edges[0])
                open_b, close_b = "(", ")"
            elif allow_mirror_fallback:
                mirror_match = left_edges[0][1:] if left_edges[0][0] == "*" else "*" + left_edges[0]
                match = left_edges.index(mirror_match) if left_edges.count(mirror_match) > 0 else 0
                open_b, close_b = "[", "]"
            else:
                raise ValueError(f"No matching edge found for {left_edges[0]!r}")
            if match == 0:
                conway += f"{open_b}{edges[0]}{close_b}"
                del left_edges[0], right_edges[0], edges[0]
            else:
                conway += f"{open_b}{edges[0]} {edges[match]}{close_b}"
                del left_edges[match], right_edges[match], edges[match]
                del left_edges[0], right_edges[0], edges[0]
        return conway

    @classmethod
    def _assemble_chiral(
        cls, main_lines: List[str], repeat_list: List[int],
    ) -> Tuple[str, List[int]]:
        """Assembles a tile-adjacency symbol for a chiral solution, where
        only every other cycle (the non-mirrored half) is needed."""
        half = len(main_lines) // 2
        selected_lines = [main_lines[2 * i] for i in range(half)]
        selected_repeats = [repeat_list[2 * i] for i in range(half)]
        left_edges, right_edges, edges, polygon_sizes = cls._build_edge_lists(
            selected_lines, selected_repeats,
        )
        conway = cls._match_edges(left_edges, right_edges, edges, allow_mirror_fallback=True)
        return conway, polygon_sizes

    @classmethod
    def _assemble_achiral(
        cls, main_lines: List[str], repeat_list: List[int],
    ) -> Tuple[str, List[int]]:
        """Assembles a tile-adjacency symbol for an achiral solution, using
        every cycle."""
        left_edges, right_edges, edges, polygon_sizes = cls._build_edge_lists(main_lines, repeat_list)
        conway = cls._match_edges(left_edges, right_edges, edges, allow_mirror_fallback=False)
        return conway, polygon_sizes

    @staticmethod
    def _write_tes_file(
        path: str, conway: str, polygon_sizes: List[int], repeat_list: List[int], solution_label: str,
    ) -> None:
        """Writes a HyperRogue ``.tes`` file describing the tiling."""
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as tes:
            tes.write(f"## Euclidean, {solution_label}\n")
            tes.write("e2.\n")
            tes.write("angleunit(deg)\n")
            for size in polygon_sizes:
                angles = ",".join(str(180 - 360 // size) for _ in range(size))
                tes.write(f"unittile({angles})\n")
            tes.write(f'conway("{conway}")\n')
            for i, repeat in enumerate(repeat_list):
                if repeat > 1:
                    tes.write(f"repeat({i},{repeat})\n")


# =============================================================================
# Solver
# =============================================================================


class EuclideanSolver:
    """Enumerates k-uniform Euclidean tilings by exhaustively extending
    partial vertex-configuration assignments, edge by edge, using an explicit
    work queue of `PartialSolution` states (depth/breadth governed entirely
    by the order states are appended and popped).
    """

    def __init__(self, max_polygons: int = 15, output_dir: str = "solutions") -> None:
        #: Maximum number of vertices (polygons) a solution may contain.
        self.max_polygons = max_polygons
        self.output_dir = output_dir

        self._queue: List[PartialSolution] = self._initial_states()
        self._log: Optional[TextIO] = None

        # Bookkeeping, populated as solutions are found.
        self._run_totals: Dict[str, int] = {}                        # polygon-combo code -> count
        self._vertex_combo_counts: Dict[Tuple[int, ...], int] = {}    # exact vertex multiset -> count
        self.solution_files: Dict[str, str] = {}                      # polygon-combo code -> file path
        self.partials_checked = 0
        self.solutions_found = 0

    # -- top-level run -------------------------------------------------------

    def run(self) -> None:
        """Runs the full search to completion, writing solution files and a
        final summary into `self.output_dir`."""
        os.makedirs(self.output_dir, exist_ok=True)
        log_path = os.path.join(self.output_dir, "euoutput.txt")
        with open(log_path, "w") as log:
            self._log = log
            while self._queue:
                self._extend(0)
                self.partials_checked += 1
                del self._queue[0]
        self._log = None
        self._write_summary()

    def run_parallel(self, num_workers: Optional[int] = None) -> None:
        """Parallel variant of `run`.

        Distributes the 44 initial vertex-type states across *num_workers*
        processes (default: ``os.cpu_count()``).  Because every solution's
        vertex list is non-decreasing in vertex-type index (new vertices are
        only ever added with type ≥ the first vertex's type), each search
        subtree rooted at a distinct initial state is completely disjoint.
        Workers therefore never produce duplicate solutions, and their output
        files can simply be concatenated.

        Each worker writes into a private ``_worker_N`` subdirectory so that
        concurrent file writes never conflict.  The parent process merges all
        worker output into the top-level ``output_dir`` after the pool joins,
        then writes the combined summary.
        """
        num_workers = num_workers or os.cpu_count() or 1
        os.makedirs(self.output_dir, exist_ok=True)

        # Round-robin distribution of the 44 initial states across workers.
        all_states = self._initial_states()
        chunks: List[List[PartialSolution]] = [[] for _ in range(num_workers)]
        for i, state in enumerate(all_states):
            chunks[i % num_workers].append(state)

        worker_dirs = [
            os.path.join(self.output_dir, f"_worker_{i}") for i in range(num_workers)
        ]
        worker_args = [
            (chunk, self.max_polygons, worker_dir)
            for chunk, worker_dir in zip(chunks, worker_dirs)
        ]

        with multiprocessing.Pool(num_workers) as pool:
            results = pool.map(_run_solver_worker, worker_args)

        # Merge worker outputs into the main output directory.
        for solution_files, partials, solutions, run_totals, combo_counts in results:
            self.partials_checked += partials
            self.solutions_found += solutions

            # Merge combo-code counters.
            for code, count in run_totals.items():
                self._run_totals[code] = self._run_totals.get(code, 0) + count

            # Merge vertex-combo counters (subtrees are disjoint, so keys
            # across workers never collide; we still use .get for safety).
            for key, count in combo_counts.items():
                self._vertex_combo_counts[key] = (
                    self._vertex_combo_counts.get(key, 0) + count
                )

            # Append each worker's solution file into the shared file.
            for combo_code, worker_path in solution_files.items():
                dest_path = os.path.join(self.output_dir, f"eusolver_{combo_code}.txt")
                self.solution_files[combo_code] = dest_path
                with open(worker_path, "r") as src, open(dest_path, "a") as dst:
                    dst.write(src.read())

        self._write_summary()

    # -- setup -----------------------------------------------------------

    @staticmethod
    def _initial_states() -> List[PartialSolution]:
        """One partial solution per vertex type, consisting of just that
        single, unglued vertex."""
        states = []
        for vertex_type in range(VertexTypeCatalog.count()):
            slot_count = len(VertexTypeCatalog.left_neighbors[vertex_type])
            states.append(PartialSolution(
                right_neighbor=list(VertexTypeCatalog.right_neighbors[vertex_type]),
                left_neighbor=list(VertexTypeCatalog.left_neighbors[vertex_type]),
                polygon_size=list(VertexTypeCatalog.polygon_sizes[vertex_type]),
                mirror=list(VertexTypeCatalog.mirrors[vertex_type]),
                glue=[-1] * slot_count,
                label=list(VertexTypeCatalog.edge_label_templates[vertex_type]),
                vertex_types=[vertex_type],
                num_polygons=1,
            ))
        return states

    # -- validity / cycle analysis -------------------------------------------

    @staticmethod
    def _is_valid_partial(right_neighbor: List[int], polygon_size: List[int], glue: List[int]) -> bool:
        """A partial solution is invalid if walking around any polygon either:
          1) exceeds its nominal size before closing, or
          2) closes with a slot count that does not evenly divide its
             nominal size (closing early is fine -- a hexagon may close as a
             cycle of 6, 3, 2, or 1 slots -- but not at, say, 5).
        """
        for i in range(len(right_neighbor)):
            free = i
            r_free = right_neighbor[free]
            main_size = polygon_size[r_free]
            count = 1
            while True:
                free = glue[r_free]
                if free == -1:
                    if count > main_size:
                        return False
                    break
                if free == i:
                    if main_size % count != 0:
                        return False
                    break
                r_free = right_neighbor[free]
                count += 1
                if polygon_size[r_free] != main_size:
                    return False
        return True

    @staticmethod
    def _analyze_cycles_for_extension(state: PartialSolution) -> Tuple[str, Tuple[int, int]]:
        """Walks every polygon cycle in the partial solution, returning a
        human-readable log of them plus the "tightest" still-open polygon:
        the (free_edge_slot, slack) pair where slack is how many more slots
        that polygon could still take before exceeding its nominal size. This
        identifies which free edge `_extend` should try to close next.
        """
        right_neighbor, left_neighbor, polygon_size, glue, label = (
            state.right_neighbor, state.left_neighbor, state.polygon_size, state.glue, state.label,
        )
        tightest = (-1, 13)
        seen: Set[int] = set()
        lines = []

        for start in range(len(glue)):
            if start in seen:
                continue
            text = ""
            count = 0
            complete = False
            left = start
            # Walk backwards to find an unglued edge to start the forward walk from.
            while glue[left] != -1 and glue[left] != right_neighbor[start]:
                left = left_neighbor[glue[left]]

            if glue[left] == -1:
                stable = left
                right = right_neighbor[left]
                v_stable = polygon_size[right]
                while True:
                    seen.add(left)
                    text += f"{label[left]}/{label[right]}({polygon_size[right]})-"
                    count += 1
                    left = glue[right]
                    if left != -1:
                        right = right_neighbor[left]
                    else:
                        slack = v_stable - count
                        if slack < tightest[1]:
                            tightest = (stable, slack)
                        break
            else:
                left = start
                right = right_neighbor[left]
                v = polygon_size[right]
                complete = True
                while True:
                    seen.add(left)
                    text += f"{label[left]}/{label[right]}({polygon_size[right]})-"
                    count += 1
                    left = glue[right]
                    if left != start:
                        right = right_neighbor[left]
                    else:
                        break

            if complete:
                text = text[:-1]
                ratio = v // count
                text = f" [{text}]x{ratio}" if ratio != 1 else f" {text}"
            lines.append(text)

        return "\n".join(lines) + "\n", tightest

    # -- the main search step -------------------------------------------------

    def _extend(self, n: int) -> None:
        """Extends partial solution `n` by identifying its tightest free edge
        and trying every way it could be glued: to another already-free edge
        of the same partial solution, or to a newly attached vertex of any
        type from its first vertex type onward (this floor avoids generating
        permutations of the same vertex set in a different order)."""
        state = self._queue[n]
        log = self._log
        assert log is not None

        log.write(f"Extending {n}:\n")
        log.write(SignatureFormatter.verbal_vertices(state.vertex_types) + "\n")
        log.write(SignatureFormatter.signature(state.vertex_types) + "\n")
        log.write(f"{state.num_polygons}\n")
        log.write(ConwayCycleWriter.write_conway(state.mirror, state.glue, state.label) + "\n")

        cycle_text, (first_free, slack) = self._analyze_cycles_for_extension(state)
        log.write(cycle_text)

        if state.label[first_free][0] == "*":
            first_free = state.mirror[first_free]
        mirrored = state.mirror[first_free] == first_free

        log.write(
            f"firstfree = {first_free}({state.label[first_free]}), between "
            f"{state.polygon_size[first_free]} and {state.polygon_size[state.mirror[first_free]]}. "
            f"Difference = {slack}\n"
        )

        added = 0
        new_state_summaries: List[str] = []

        # (a) glue the free edge to another free edge of the same partial solution.
        for i in range(len(state.right_neighbor)):
            if state.glue[i] != -1:
                continue
            if mirrored != (state.mirror[i] == i):
                continue
            candidate_glue = state.glue.copy()
            candidate_glue[first_free] = i
            candidate_glue[i] = first_free
            if not mirrored:
                candidate_glue[state.mirror[first_free]] = state.mirror[i]
                candidate_glue[state.mirror[i]] = state.mirror[first_free]
            if not self._is_valid_partial(state.right_neighbor, state.polygon_size, candidate_glue):
                continue

            new_state = state.copy()
            new_state.glue = candidate_glue
            if candidate_glue.count(-1) == 0:
                self._write_solution(new_state)
            else:
                added += 1
                self._queue.append(new_state)
                new_state_summaries.append(
                    ConwayCycleWriter.conway_symbol(state.label[first_free], state.label[i])
                )

        # (b) attach a brand-new vertex and glue the free edge to one of its slots.
        if state.num_polygons < self.max_polygons:
            for vertex_type in range(state.vertex_types[0], VertexTypeCatalog.count()):
                offset = len(state.right_neighbor)
                right_neighbor = state.right_neighbor.copy()
                left_neighbor = state.left_neighbor.copy()
                polygon_size = state.polygon_size.copy()
                mirror = state.mirror.copy()
                label = state.label.copy()
                vertex_types = state.vertex_types.copy()

                slot_count = len(VertexTypeCatalog.left_neighbors[vertex_type])
                for slot in range(slot_count):
                    right_neighbor.append(offset + VertexTypeCatalog.right_neighbors[vertex_type][slot])
                    left_neighbor.append(offset + VertexTypeCatalog.left_neighbors[vertex_type][slot])
                    mirror.append(offset + VertexTypeCatalog.mirrors[vertex_type][slot])
                    polygon_size.append(VertexTypeCatalog.polygon_sizes[vertex_type][slot])
                    label.append(ConwayCycleWriter.edge_label(
                        VertexTypeCatalog.edge_label_templates[vertex_type][slot], state.num_polygons,
                    ))
                vertex_types.append(vertex_type)

                limit = VertexTypeCatalog.attachment_limit(vertex_type)
                for i in range(offset, offset + limit):
                    if mirrored != (mirror[i] == i):
                        continue
                    candidate_glue = state.glue.copy()
                    candidate_glue.extend([-1] * slot_count)
                    candidate_glue[first_free] = i
                    candidate_glue[i] = first_free
                    if not mirrored:
                        candidate_glue[mirror[first_free]] = mirror[i]
                        candidate_glue[mirror[i]] = mirror[first_free]
                    if not self._is_valid_partial(right_neighbor, polygon_size, candidate_glue):
                        continue

                    new_state = PartialSolution(
                        right_neighbor=right_neighbor.copy(), left_neighbor=left_neighbor.copy(),
                        polygon_size=polygon_size.copy(), mirror=mirror.copy(), glue=candidate_glue,
                        label=label.copy(), vertex_types=vertex_types.copy(),
                        num_polygons=state.num_polygons + 1,
                    )
                    if candidate_glue.count(-1) == 0:
                        self._write_solution(new_state)
                    else:
                        added += 1
                        self._queue.append(new_state)
                        new_state_summaries.append(
                            ConwayCycleWriter.conway_symbol(label[first_free], label[i])
                            + " " + VertexTypeCatalog.symbols[vertex_type]
                        )

        log.write(f"Added {added} partial solution{'s' if added != 1 else ''}, "
                  f"solutions to check: {len(self._queue)}\n")
        if new_state_summaries:
            log.write("; ".join(new_state_summaries) + "\n")
        log.write("\n")

    # -- solution output -------------------------------------------------

    @staticmethod
    def _pad2(n: int) -> str:
        return f"0{n}" if n < 10 else str(n)

    @staticmethod
    def _polygon_combo_code(state: PartialSolution) -> str:
        """A short code summarizing which polygon shapes (triangle, square,
        hexagon, 12-gon) appear in the solution, used to group solutions of
        similar "flavour" into the same output file."""
        code = EuclideanSolver._pad2(state.num_polygons) + "_"
        sizes = set(state.polygon_size)
        for size, letter in ((3, "3"), (4, "4"), (6, "6"), (12, "c")):
            if size in sizes:
                code += letter
        return code

    def _record_vertex_combo(self, vertex_types: List[int]) -> int:
        """Tracks how many solutions share this exact vertex-type multiset
        (regardless of polygon combo code), returning the running count."""
        counts = [0] * VertexTypeCatalog.count()
        for vertex_type in vertex_types:
            counts[vertex_type] += 1
        key = tuple(counts)
        self._vertex_combo_counts[key] = self._vertex_combo_counts.get(key, 0) + 1
        return self._vertex_combo_counts[key]

    def _write_solution(self, state: PartialSolution) -> None:
        """Records a freshly completed tiling: updates run totals, appends
        a description to its combo's output file, and writes a ``.tes`` file."""
        self.solutions_found += 1
        combo_code = self._polygon_combo_code(state)
        path = os.path.join(self.output_dir, f"eusolver_{combo_code}.txt")
        is_new_file = combo_code not in self._run_totals
        self._run_totals[combo_code] = self._run_totals.get(combo_code, 0) + 1
        self.solution_files[combo_code] = path

        solution_index = self._record_vertex_combo(state.vertex_types)
        signature_text = SignatureFormatter.signature(state.vertex_types)
        file_sig = SignatureFormatter.file_signature(state.vertex_types)

        tes_relpath = os.path.join(
            self._pad2(state.num_polygons), combo_code, file_sig,
            f"eu raw {file_sig} {solution_index}.tes",
        )
        tes_path = os.path.join(self.output_dir, tes_relpath)
        solution_label = f"{signature_text}, solution {solution_index}"

        with open(path, "w" if is_new_file else "a") as out:
            out.write(f"Number of polygons: {state.num_polygons}\n")
            out.write(SignatureFormatter.verbal_vertices(state.vertex_types) + "\n")
            out.write(signature_text + "\n")
            out.write(f"TES file: {tes_relpath}\n")
            out.write(ConwayCycleWriter.write_conway(state.mirror, state.glue, state.label) + "\n")
            ConwayCycleWriter.write_cycle_final(state, out, tes_path, solution_label)
            out.write("\n")

    def _write_summary(self) -> None:
        """Writes the final tally of solutions per polygon-combo code and per
        exact vertex multiset, the latter sorted by total vertex count (and,
        for ties, by the relative weighting of its vertex types)."""
        path = os.path.join(self.output_dir, "eu_final_results.txt")
        with open(path, "w") as out:
            for code, count in self._run_totals.items():
                out.write(f"{code}: {count}\n")
            out.write("\n")

            ordered = sorted(
                self._vertex_combo_counts.items(),
                key=lambda item: (sum(item[0]), tuple(-c for c in item[0])),
            )
            for counts, count in ordered:
                out.write(f"{SignatureFormatter._render_counts(list(counts))}: {count}\n")


# =============================================================================
# Pruner
# =============================================================================


class ConwayParser:
    """Parses Conway gluing symbols (as written by `ConwayCycleWriter`) back
    into a numeric `glue` array, the inverse of `ConwayCycleWriter.write_conway`.
    """

    @staticmethod
    def _decipher_symbol(symbol: str) -> Tuple[str, str]:
        """Splits a single Conway symbol like "(0 1)" or "[2]" into its two
        (possibly identical) edge-label texts."""
        mirror = symbol[0] == "["
        i = 1
        first = ""
        while symbol[i] not in (" ", ")", "]"):
            first += symbol[i]
            i += 1
        if symbol[i] == " ":
            i += 1
            second = ""
            while symbol[i] not in (")", "]"):
                second += symbol[i]
                i += 1
        else:
            second = first
        if mirror:
            second = "*" + second
        return first, second

    @staticmethod
    def _decipher_edge(text: str) -> Tuple[bool, int, int]:
        """Parses a single edge-label text like "*3@7" into
        (is_mirrored, slot_number, tile_number)."""
        s = text + " "
        i = 0
        mirror = False
        number = 0
        tile = 0
        if s[i] == "*":
            mirror = True
            i += 1
        while s[i].isdigit():
            number = number * 10 + int(s[i])
            i += 1
        while s[i] == "'":
            tile += 1
            i += 1
        if s[i] == "@":
            i += 1
            while s[i].isdigit():
                tile = tile * 10 + int(s[i])
                i += 1
        return mirror, number, tile

    @staticmethod
    def _find_closing_index(conway: str) -> int:
        for i, ch in enumerate(conway):
            if ch in (")", "]"):
                return i
        return -1

    @classmethod
    def make_glue(cls, conway: str, mirror: List[int], label: List[str]) -> List[int]:
        """Reconstructs a `glue` array from a Conway gluing-symbol string."""
        glue = [-1] * len(mirror)
        remaining = conway
        while len(remaining) > 1:
            end = cls._find_closing_index(remaining)
            symbol, remaining = remaining[:end + 1], remaining[end + 1:]
            first_text, second_text = cls._decipher_symbol(symbol)
            indices = []
            for text in (first_text, second_text):
                is_mirror, number, tile = cls._decipher_edge(text)
                slot_label = ("*" if is_mirror else "") + ConwayCycleWriter.edge_label(number, tile)
                indices.append(label.index(slot_label))
            a, b = indices
            glue[a] = b
            glue[b] = a
            glue[mirror[a]] = mirror[b]
            glue[mirror[b]] = mirror[a]
        return glue


class SolutionPruner:
    """Deduplicates the raw solutions produced by `EuclideanSolver`.

    Each solution can be labeled (vertices/edges numbered) in multiple
    equivalent ways. For each input file, this:

      1. discards every labeling that isn't the canonical ("simplest") one
         for its underlying tiling, via `_is_canonical_labeling`;
      2. discards canonical labelings that are isomorphic to a tiling already
         kept (possibly from an earlier file), via `_solutions_match`;
      3. writes the survivors, with their ``.tes`` files, to `output_dir`.
    """

    def __init__(self, output_dir: str = os.path.join("solutions", "pruned")) -> None:
        self.output_dir = output_dir
        # Kept solutions, grouped by signature (vertex multiset) for fast lookup.
        self._solutions_by_signature: Dict[str, List[PartialSolution]] = {}
        # Number of unique solutions written to disk, keyed by k (vertex count).
        self.solutions_per_k: Dict[int, int] = {}

    def run(self, listfile_paths: List[str], num_workers: Optional[int] = None) -> None:
        """Deduplicates all files, using a multiprocessing pool for the
        CPU-intensive canonicality and isomorphism checks.

        Files must still be processed **sequentially** (each new solution is
        checked against an accumulating store of previously accepted ones),
        but within each file the independent checks are dispatched in parallel.

        Args:
            listfile_paths: Paths to the solver-generated solution list files.
            num_workers: Pool size; defaults to ``os.cpu_count()``.
        """
        num_workers = num_workers or os.cpu_count() or 1
        os.makedirs(self.output_dir, exist_ok=True)
        with multiprocessing.Pool(num_workers) as pool:
            for path in listfile_paths:
                self._process_file(path, pool)

    # -- decoding -------------------------------------------------------

    @staticmethod
    def _count_digit(x: int) -> str:
        if x == 10:
            return "a"
        if x == 11:
            return "b"
        return str(x)

    @classmethod
    def _parse_vertex_types(cls, vertex_line: str) -> Tuple[List[int], str]:
        """Parses a `SignatureFormatter.verbal_vertices` line back into
        vertex-type indices, plus a short "count signature" summarizing how
        many distinct base polygon symbols (ignoring the trailing
        orientation letter) are present and how often each repeats."""
        tokens = vertex_line.rstrip("\n").split(", ")
        vertex_types = [VertexTypeCatalog.symbols.index(tok) for tok in tokens]

        base_counts: Dict[str, int] = {}
        for tok in tokens:
            base = tok[:tok.index(")") + 1]
            base_counts[base] = base_counts.get(base, 0) + 1

        count_signature = str(len(base_counts))
        multiplicities = sorted(base_counts.values(), reverse=True)
        if any(m > 1 for m in multiplicities):
            digits = "".join(cls._count_digit(m) for m in multiplicities)
            count_signature += f" ({digits})"
        return vertex_types, count_signature

    @classmethod
    def _decode_solution(cls, vertex_line: str, conway_line: str) -> Tuple[PartialSolution, str]:
        """Reconstructs a full `PartialSolution` from the text a solution was
        written with, plus its count signature."""
        vertex_types, count_signature = cls._parse_vertex_types(vertex_line)

        right_neighbor: List[int] = []
        left_neighbor: List[int] = []
        polygon_size: List[int] = []
        mirror: List[int] = []
        label: List[str] = []
        for tile_index, vertex_type in enumerate(vertex_types):
            offset = len(right_neighbor)
            slot_count = len(VertexTypeCatalog.left_neighbors[vertex_type])
            for slot in range(slot_count):
                right_neighbor.append(offset + VertexTypeCatalog.right_neighbors[vertex_type][slot])
                left_neighbor.append(offset + VertexTypeCatalog.left_neighbors[vertex_type][slot])
                mirror.append(offset + VertexTypeCatalog.mirrors[vertex_type][slot])
                polygon_size.append(VertexTypeCatalog.polygon_sizes[vertex_type][slot])
                label.append(ConwayCycleWriter.edge_label(
                    VertexTypeCatalog.edge_label_templates[vertex_type][slot], tile_index,
                ))

        glue = ConwayParser.make_glue(conway_line.rstrip("\n"), mirror, label)
        state = PartialSolution(
            right_neighbor=right_neighbor, left_neighbor=left_neighbor, polygon_size=polygon_size,
            mirror=mirror, glue=glue, label=label, vertex_types=vertex_types,
            num_polygons=len(vertex_types),
        )
        return state, count_signature

    # -- isomorphism checks -------------------------------------------------

    @staticmethod
    def _is_canonical_labeling(state: PartialSolution, raw_log: TextIO) -> bool:
        """A tiling can be labeled in many equivalent ways. This iteratively
        narrows, for each edge slot, the set of other slots it *could* be an
        automorphic image of (matching polygon size, mirror, glue, and
        neighbor structure). If every slot ends up uniquely matched to
        itself, this is the single canonical labeling and should be kept;
        otherwise some other labeling of the same tiling is equally valid,
        and (by convention) only one of them -- found elsewhere in the run --
        is kept.
        """
        n = len(state.right_neighbor)
        alias: List[Set[int]] = [set(range(n)) for _ in range(n)]
        unique = [False] * n
        changed = True
        while changed:
            changed = False
            for i in range(n):
                for j in list(alias[i]):
                    if (
                        state.polygon_size[i] != state.polygon_size[j]
                        or i not in alias[j]
                        or state.mirror[j] not in alias[state.mirror[i]]
                        or state.glue[j] not in alias[state.glue[i]]
                        or state.right_neighbor[j] not in alias[state.right_neighbor[i]]
                        or state.left_neighbor[j] not in alias[state.left_neighbor[i]]
                    ):
                        alias[i].discard(j)
                        changed = True
                if len(alias[i]) == 1:
                    unique[i] = True

        if all(unique):
            return True

        seen: Set[int] = set()
        groups = []
        for i in range(n):
            if i not in seen and len(alias[i]) > 1:
                groups.append("[" + "/".join(state.label[j] for j in alias[i]) + "]")
                seen.update(alias[i])
        raw_log.write(" ".join(groups) + "\n")
        return False

    @staticmethod
    def _solutions_match(state: PartialSolution, other: PartialSolution, raw_log: TextIO) -> bool:
        """Determines whether `state` and `other` describe the same tiling,
        by checking whether a consistent edge-to-edge correspondence (graph
        isomorphism) exists between them: every slot of `state` must end up
        aliased to exactly one slot of `other`, and vice versa.
        """
        n = len(state.right_neighbor)
        alias: List[Set[int]] = [set(range(n, 2 * n)) | {i} for i in range(n)]
        alias += [set(range(n)) | {n + i} for i in range(n)]

        right_neighbor = state.right_neighbor + [n + x for x in other.right_neighbor]
        left_neighbor = state.left_neighbor + [n + x for x in other.left_neighbor]
        mirror = state.mirror + [n + x for x in other.mirror]
        glue = state.glue + [n + x for x in other.glue]
        polygon_size = state.polygon_size + other.polygon_size
        label = state.label + other.label

        total = 2 * n
        unique = [False] * total
        changed = True
        while changed:
            changed = False
            for i in range(total):
                for j in list(alias[i]):
                    if (
                        polygon_size[i] != polygon_size[j]
                        or i not in alias[j]
                        or mirror[j] not in alias[mirror[i]]
                        or glue[j] not in alias[glue[i]]
                        or right_neighbor[j] not in alias[right_neighbor[i]]
                        or left_neighbor[j] not in alias[left_neighbor[i]]
                    ):
                        alias[i].discard(j)
                        changed = True
                if len(alias[i]) == 1:
                    unique[i] = True

        # False when some alias sets still contain cross-solution candidates,
        # meaning a valid edge bijection between the two tilings was found:
        # they are isomorphic, i.e. this is a duplicate.
        is_not_matching = not all(unique)
        if is_not_matching:
            seen: Set[int] = set()
            groups = []
            for i in range(total):
                if i not in seen and len(alias[i]) > 1:
                    groups.append("[" + "=".join(label[j] for j in alias[i]) + "]")
                    seen.update(alias[i])
            raw_log.write(" ".join(groups) + "\n")
        return is_not_matching 

    def _find_existing_match(self, state: PartialSolution, signature: str, raw_log: TextIO) -> bool:
        for other in self._solutions_by_signature.get(signature, []):
            if self._solutions_match(state, other, raw_log):
                return True
        return False

    def _store_solution(self, state: PartialSolution, signature: str) -> None:
        self._solutions_by_signature.setdefault(signature, []).append(state)

    # -- file processing -------------------------------------------------

    def _parse_all_solutions(
        self, lines: List[str],
    ) -> List[Tuple[str, str, str, str, PartialSolution, str]]:
        """Parses every solution record from a solver-output file into a
        structured tuple (vertex_line, signature_line, tes_line, conway_line,
        state, count_signature).

        Separating parsing from processing allows the caller to batch-submit
        all `PartialSolution` objects to a pool before doing any sequential
        work.
        """
        records = []
        i = 0
        n = len(lines)
        while i < n:
            i += 1  # "Number of polygons: N"
            vertex_line = lines[i];    i += 1
            signature_line = lines[i]; i += 1
            tes_line = lines[i];       i += 1
            conway_line = lines[i];    i += 1
            while not lines[i].startswith("-"):
                i += 1
            i += 3  # skip "---", assembled-conway line, blank separator

            state, count_signature = self._decode_solution(vertex_line, conway_line)
            records.append((vertex_line, signature_line, tes_line, conway_line,
                            state, count_signature))
        return records

    def _process_file(self, path: str, pool: multiprocessing.Pool) -> None:
        """Reads one solver-generated solution-list file, writes its
        deduplicated survivors (with fresh ``.tes`` files) to `output_dir`,
        and writes a debug log of every collapsed labeling/duplicate.

        Parallelism strategy
        --------------------
        1. **Batch canonical check** — `_is_canonical_labeling` is independent
           for every solution in the file, so all of them are dispatched to
           the pool at once with ``pool.map``.  Only the canonical subset
           proceeds to step 2.

        2. **Per-candidate match check** — for each canonical solution, all
           comparisons against the already-accepted store can be done in
           parallel with ``pool.map``.  As soon as any comparison returns
           True the candidate is rejected; otherwise it is added to the store.
           (The store itself is updated sequentially to maintain correctness.)
        """
        with open(path, "r") as f:
            lines = f.readlines()

        combo_code = os.path.splitext(os.path.basename(path))[0].replace("eusolver_", "")
        out_dir = os.path.join(self.output_dir, combo_code)
        os.makedirs(out_dir, exist_ok=True)
        pruned_path = os.path.join(out_dir, "eupruned.txt")
        raw_path = os.path.join(out_dir, "euraw.txt")

        records = self._parse_all_solutions(lines)

        # --- Phase 1: batch parallel canonical check -------------------------
        # Each worker returns (is_canonical: bool, log_text: str).
        states = [r[4] for r in records]
        canonical_results: List[Tuple[bool, str]] = pool.map(_canonical_check_worker, states)

        # --- Phase 2: sequential match checking with parallel inner loop -----
        # Must be sequential because the store accumulates accepted solutions.
        with open(pruned_path, "w") as pruned_out, open(raw_path, "w") as raw_out:
            for record, (is_canonical, canon_log) in zip(records, canonical_results):
                vertex_line, signature_line, tes_line, conway_line, state, count_sig = record

                raw_out.write(vertex_line)
                raw_out.write(signature_line)
                raw_out.write(tes_line)
                raw_out.write(conway_line)
                raw_out.write(canon_log)

                if not is_canonical:
                    raw_out.write("\n")
                    continue
                raw_out.write("Simplest\n")

                signature = signature_line.rstrip("\n")
                stored = self._solutions_by_signature.get(signature, [])

                # Parallel: check this candidate against every stored solution.
                # Each worker returns (is_match: bool, log_text: str).
                if stored:
                    match_results: List[Tuple[bool, str]] = pool.map(
                        _match_check_worker, [(state, other) for other in stored],
                    )
                    is_dup = False
                    for is_not_matching, match_log in match_results:
                        raw_out.write(match_log)
                        if is_not_matching:
                            is_dup = True
                            # Do not break: write all logs for completeness.
                else:
                    is_dup = False

                if is_dup:
                    raw_out.write("\n")
                    continue

                # New unique solution — add to store and write outputs.
                self._store_solution(state, signature)
                k = state.num_polygons
                self.solutions_per_k[k] = self.solutions_per_k.get(k, 0) + 1

                tes_marker = tes_line.index("eu")
                old_tes_relpath = tes_line[tes_marker:-1]
                tes_filename = "eu " + old_tes_relpath[len("eu raw "):]
                tes_path = os.path.join(out_dir, tes_filename)

                pruned_out.write(vertex_line)
                pruned_out.write(signature_line)
                pruned_out.write(f"Count type: {count_sig}\n")
                pruned_out.write(tes_line)
                pruned_out.write(conway_line)
                ConwayCycleWriter.write_cycle_final(state, pruned_out, tes_path, signature)
                pruned_out.write("\n")
                raw_out.write("\n")


# =============================================================================
# Weisfeiler-Lehman graph hash
# =============================================================================


class WLHasher:
    """Computes a Weisfeiler-Lehman (1-WL) graph hash for a ``PartialSolution``.

    Graph representation
    --------------------
    The tiling's combinatorial map is read directly from the four slot arrays,
    with no intermediate graph object needed:

    * Each slot index ``i`` is a **node**.
    * Four typed edges leave every node, one per structural relation::

          right_neighbor[i]  →  clockwise neighbour (permutation σ)
          left_neighbor[i]   →  counter-clockwise neighbour (σ⁻¹)
          mirror[i]          →  mirror-image slot (involution α)
          glue[i]            →  gluing partner (involution β)

    * The initial node colour is ``polygon_size[i]``.

    Including ``left_neighbor`` separately captures the *direction* of σ:
    without it, WL would treat σ and σ⁻¹ as identical, causing false
    matches between a tiling and its mirror reflection.

    The edge **type prefix** (``"R"``, ``"L"``, ``"M"``, ``"G"``) is
    prepended to each neighbour's label before hashing.  Without prefixes,
    a mirror edge and a glue edge to the same neighbour look identical to WL
    and valid distinct tilings would be incorrectly merged.

    WL refinement (following networkx's implementation)
    ---------------------------------------------------
    Each iteration replaces every node's label with::

        hash( current_label  +  sorted([ type + neighbour_label, ... ]) )

    Sorting the neighbour list makes the hash invariant to the arbitrary
    ordering of the four edge types.  After ``iterations`` rounds the final
    hash is the MD5 of the sorted multiset of all node labels — identical for
    any relabelling of an isomorphic tiling.

    Relationship to ``_solutions_match``
    ------------------------------------
    WL hashing replaces the O(n²) pairwise ``_solutions_match`` comparisons
    with an O(iterations × n) hash computation per solution and an O(1) set
    lookup.  ``_is_canonical_labeling`` must still run first because WL only
    equates graphs of the *same size*, while ``_is_canonical_labeling``
    merges representations of the same tiling that happen to use a different
    number of fundamental-domain slots.

    Because 1-WL is a *necessary but not sufficient* condition for
    isomorphism, hash collisions between genuinely non-isomorphic tilings are
    theoretically possible.  ``WLPruner`` falls back to ``_solutions_match``
    on any collision to guarantee correctness.
    """

    @staticmethod
    def _node_hash(label: str, typed_neighbour_labels: List[str]) -> str:
        """Single-node WL refinement step: hash the node's own label together
        with the sorted list of its typed neighbour labels."""
        from hashlib import md5
        payload = label + "".join(sorted(typed_neighbour_labels))
        return md5(payload.encode(), usedforsecurity=False).hexdigest()

    @classmethod
    def hash(cls, state: PartialSolution, iterations: int = 3) -> str:
        """Returns the WL graph hash of *state* as a hex string.

        Args:
            state:      A completed ``PartialSolution`` (all slots glued).
            iterations: Number of WL refinement rounds.  3 is sufficient for
                        all Euclidean tiling graphs encountered in practice;
                        increase if false-positive collisions are observed.
        """
        from hashlib import md5
        from collections import Counter

        n = len(state.right_neighbor)

        # Initialise node colours from polygon_size.
        labels: List[str] = [str(state.polygon_size[i]) for i in range(n)]

        for _ in range(iterations):
            new_labels: List[str] = []
            for i in range(n):
                # Four typed neighbour labels — prefixes distinguish edge types
                # so WL never confuses a glue edge with a mirror edge.
                typed = [
                    "R" + labels[state.right_neighbor[i]],   # clockwise
                    "L" + labels[state.left_neighbor[i]],    # counter-clockwise
                    "M" + labels[state.mirror[i]],           # mirror image
                    "G" + labels[state.glue[i]],             # gluing partner
                ]
                new_labels.append(cls._node_hash(labels[i], typed))
            labels = new_labels

        # Final hash: the sorted multiset of all node labels.
        # Sorting makes it invariant to the arbitrary slot numbering.
        counts = Counter(labels)
        payload = str(sorted(counts.items()))
        return md5(payload.encode(), usedforsecurity=False).hexdigest()


# =============================================================================
# WL-hash-based pruner
# =============================================================================


class WLPruner(SolutionPruner):
    """Deduplicates solver output using Weisfeiler-Lehman graph hashing.

    This replaces the O(k²) ``_solutions_match`` loop with:

    1. ``_is_canonical_labeling`` (unchanged) — filters non-canonical
       fundamental-domain representations from the solver output.
    2. ``WLHasher.hash`` — O(iterations × n) per solution; computed in
       parallel for all solutions in each file.
    3. Hash-set lookup — O(1); replaces ``_solutions_match`` for the
       common case.
    4. ``_solutions_match`` fallback — only invoked on hash collisions (a
       genuine 1-WL false positive).  In practice this never fires for
       Euclidean tiling graphs, but it guarantees correctness.

    Overall complexity: O(k × iterations × n) instead of O(k² × n²).
    """

    def __init__(self, output_dir: str = os.path.join("solutions", "wl")) -> None:
        # Bypass SolutionPruner.__init__: we maintain a hash-bucketed store
        # rather than a signature-bucketed store.
        self.output_dir = output_dir
        # Maps WL hash -> list of accepted PartialSolutions with that hash.
        # The list is almost always length 0 or 1; length > 1 only on a
        # genuine WL false positive.
        self._solutions_by_hash: Dict[str, List[PartialSolution]] = {}
        self.solutions_per_k: Dict[int, int] = {}

    def run(self, listfile_paths: List[str], num_workers: Optional[int] = None) -> None:
        """Deduplicates all solver output files using WL hashing.

        Files are processed sequentially (the hash store accumulates across
        files), but within each file canonical checks and WL hashes are
        computed in parallel.
        """
        num_workers = num_workers or os.cpu_count() or 1
        os.makedirs(self.output_dir, exist_ok=True)
        with multiprocessing.Pool(num_workers) as pool:
            for path in listfile_paths:
                self._process_file(path, pool)

    def _process_file(self, path: str, pool: multiprocessing.Pool) -> None:
        """Reads one solver output file, deduplicates via WL hashing, and
        writes survivors to disk.

        Each solution goes through three gates:

        1. **Canonical check** (parallel) — ``_is_canonical_labeling`` drops
           solutions whose labeling is redundant with a simpler one already
           present in the solver output for the same tiling.
        2. **WL hash lookup** (sequential, O(1)) — drops solutions whose
           hash matches a previously accepted solution.
        3. **Collision fallback** (sequential, rare) — on a hash collision,
           ``_solutions_match`` confirms whether it is a true duplicate.

        Gates 1 and 2 are combined into a single ``pool.map`` call so that
        no solution pays the WL cost unless it first passes the canonical
        check.
        """
        with open(path, "r") as f:
            lines = f.readlines()

        combo_code = os.path.splitext(os.path.basename(path))[0].replace("eusolver_", "")
        out_dir = os.path.join(self.output_dir, combo_code)
        os.makedirs(out_dir, exist_ok=True)
        pruned_path = os.path.join(out_dir, "eupruned.txt")
        raw_path    = os.path.join(out_dir, "euraw.txt")

        records = self._parse_all_solutions(lines)
        if not records:
            return

        # Parallel: canonical check + WL hash in one worker call per solution.
        states = [r[4] for r in records]
        worker_results: List[Tuple[bool, str, str]] = pool.map(
            _canonical_and_wl_worker, states,
        )

        with open(pruned_path, "w") as pruned_out, open(raw_path, "w") as raw_out:
            for record, (is_canonical, canon_log, wl_hash) in zip(records, worker_results):
                vertex_line, signature_line, tes_line, conway_line, state, count_sig = record

                raw_out.write(vertex_line)
                raw_out.write(signature_line)
                raw_out.write(tes_line)
                raw_out.write(conway_line)
                raw_out.write(canon_log)

                if not is_canonical:
                    raw_out.write("\n")
                    continue
                raw_out.write("Simplest\n")

                # Gate 2: WL hash lookup.
                bucket = self._solutions_by_hash.get(wl_hash, [])
                is_dup = False
                for stored in bucket:
                    buf = io.StringIO()
                    if SolutionPruner._solutions_match(state, stored, buf):
                        raw_out.write(buf.getvalue())
                        raw_out.write("(WL collision confirmed as duplicate)\n")
                        is_dup = True
                        break
                    raw_out.write(buf.getvalue())
                    raw_out.write("(WL collision — not a duplicate)\n")

                if is_dup:
                    raw_out.write("\n")
                    continue

                # New unique solution.
                self._solutions_by_hash.setdefault(wl_hash, []).append(state)
                k = state.num_polygons
                self.solutions_per_k[k] = self.solutions_per_k.get(k, 0) + 1

                tes_marker = tes_line.index("eu")
                old_tes_relpath = tes_line[tes_marker:-1]
                tes_filename = "eu " + old_tes_relpath[len("eu raw "):]
                tes_path = os.path.join(out_dir, tes_filename)

                pruned_out.write(vertex_line)
                pruned_out.write(signature_line)
                pruned_out.write(f"Count type: {count_sig}\n")
                pruned_out.write(tes_line)
                pruned_out.write(conway_line)
                ConwayCycleWriter.write_cycle_final(state, pruned_out, tes_path, signature_line.rstrip())
                pruned_out.write("\n")
                raw_out.write("\n")



class TilingCanonicalizer:
    """Encodes a completed ``PartialSolution`` as a vertex-coloured undirected
    graph and computes its nauty canonical certificate via *pynauty*.

    Encoding
    --------
    The tiling is a **combinatorial map**: n flag-slots with three relations:

    * ``right_neighbor`` — a directed permutation σ (clockwise cycle around
      each vertex),
    * ``mirror``         — an involution α (reflection symmetry),
    * ``glue``           — an involution β (edge pairing between vertices),

    plus a ``polygon_size`` colouring on the flags.

    To represent directed and labelled edges in a plain undirected graph we
    introduce three classes of **gadget vertices**, one per relation:

    +-----------------------------------------+---------------------------+
    | Vertices 0 .. n-1                        | flag vertices             |
    +-----------------------------------------+---------------------------+
    | Vertices n .. 2n-1                        | right-neighbour gadgets   |
    +-----------------------------------------+---------------------------+
    | Vertices 2n .. 2n+m-1                    | mirror gadgets            |
    +-----------------------------------------+---------------------------+
    | Vertices 2n+m .. 2n+m+g-1               | glue gadgets              |
    +-----------------------------------------+---------------------------+

    where m = number of mirror pairs (i, mirror[i]) with i < mirror[i], and
    g = number of glue pairs (i, glue[i]) with i < glue[i].

    **Right-neighbour gadgets** encode the directed permutation σ.
    Gadget n+i sits between flag[i] (source) and flag[right_neighbor[i]]
    (target).  Because every flag vertex has exactly one "outgoing" gadget
    (n+i) and one "incoming" gadget (n+left_neighbor[i]), and their other
    neighbours differ, nauty can reconstruct the direction from topology
    alone.

    **Mirror / glue gadgets** sit between the two flag endpoints of each
    pair.  The three gadget classes receive distinct colours so nauty never
    confuses a mirror edge with a glue edge or a right-neighbour step.

    **Self-mirror slots** (``mirror[i] == i``) receive no gadget; instead
    their flag vertex is placed in a separate colour sub-class.

    Correctness guarantee
    ---------------------
    Two ``PartialSolution`` objects represent isomorphic tilings if and only
    if their ``certificate()`` values are equal.  This replaces *both* the
    ``_is_canonical_labeling`` and ``_solutions_match`` checks of
    ``SolutionPruner``, and reduces the overall deduplication from O(n²) to
    O(n).
    """

    @staticmethod
    def _build_graph(state: PartialSolution):
        """Constructs and returns the pynauty ``Graph`` for *state*.

        Vertex layout
        -------------
        ::

            0 .. n-1                     flag vertices
            n .. 2n-1                    right-neighbour gadgets
            2n .. 2n+m-1                 mirror gadgets
            2n+m .. 2n+m+g-1             glue gadgets
            2n+m+g ..                    polygon-size chain vertices
                                         (see below)

        Polygon-size encoding
        ---------------------
        Nauty's canonical certificate only captures the *partition* of vertices
        into colour classes, not the labels of those classes.  Two colour
        schemes that produce the same partition yield the same certificate even
        if the "meaning" of each class differs.  Polygon size must therefore be
        encoded in the **graph structure**, not just the colouring.

        For each distinct polygon size *p* present in the tiling we add one
        shared **chain** of ``p - 2`` auxiliary vertices.  Every flag vertex
        with that polygon size is connected to the start of its chain.  Chains
        of different lengths are structurally distinct, so nauty correctly
        distinguishes polygon sizes 3 (chain length 1), 4 (length 2), 6
        (length 4), and 12 (length 10) without any special colour-class tricks.
        All chain vertices share a single colour class, keeping the coloring
        simple and encoding the size purely through topology.

        Self-mirror encoding
        --------------------
        A self-mirror flag vertex (``mirror[i] == i``) has no mirror-gadget
        neighbour, while a non-self-mirror one does.  This structural difference
        is automatically captured by the graph without extra colour sub-classes.
        """
        from pynauty import Graph

        n = len(state.right_neighbor)

        mirror_pairs: List[Tuple[int, int]] = [
            (i, state.mirror[i]) for i in range(n) if i < state.mirror[i]
        ]
        glue_pairs: List[Tuple[int, int]] = [
            (i, state.glue[i]) for i in range(n) if i < state.glue[i]
        ]

        m = len(mirror_pairs)
        g = len(glue_pairs)

        # Build polygon-size chains.
        # For each distinct p, we allocate a contiguous block of (p-2) vertices.
        distinct_sizes = sorted(set(state.polygon_size))
        chain_start: Dict[int, int] = {}   # polygon_size -> index of chain start vertex
        chain_vertex_set: Set[int] = set() # all chain vertex indices

        next_vertex = 2 * n + m + g
        for p in distinct_sizes:
            chain_len = p - 2   # p=3 → 1, p=4 → 2, p=6 → 4, p=12 → 10
            chain_start[p] = next_vertex
            chain_vertex_set.update(range(next_vertex, next_vertex + chain_len))
            next_vertex += chain_len

        total = next_vertex

        # Build adjacency sets (sets deduplicate any degenerate cases such as
        # a single-slot vertex where right_neighbor[0] == 0).
        adj: Dict[int, Set[int]] = {v: set() for v in range(total)}

        # Right-neighbour: flag[i] — gadget(n+i) — flag[right_neighbor[i]]
        for i in range(n):
            gadget = n + i
            rn = state.right_neighbor[i]
            adj[i].add(gadget);   adj[gadget].add(i)
            adj[gadget].add(rn);  adj[rn].add(gadget)

        # Mirror: flag[i] — mirror_gadget — flag[mirror[i]]
        for j, (i, mi) in enumerate(mirror_pairs):
            gadget = 2 * n + j
            adj[i].add(gadget);   adj[gadget].add(i)
            adj[gadget].add(mi);  adj[mi].add(gadget)

        # Glue: flag[i] — glue_gadget — flag[glue[i]]
        for j, (i, gi) in enumerate(glue_pairs):
            gadget = 2 * n + m + j
            adj[i].add(gadget);   adj[gadget].add(i)
            adj[gadget].add(gi);  adj[gi].add(gadget)

        # Polygon-size chains: flag[i] — chain_start[p] — ... — chain_end[p]
        # Chain vertices are linked in a path; each flag vertex connects to the
        # start of its polygon-size chain (the chain is shared across all flag
        # vertices with the same polygon size).
        for p in distinct_sizes:
            chain_len = p - 2
            start = chain_start[p]
            # Internal chain edges
            for k in range(chain_len - 1):
                a, b = start + k, start + k + 1
                adj[a].add(b); adj[b].add(a)
        for i in range(n):
            p = state.polygon_size[i]
            cs = chain_start[p]
            adj[i].add(cs); adj[cs].add(i)

        # Vertex colouring.
        coloring: List[Set[int]] = [
            set(range(n)),                          # flag vertices
            set(range(n, 2 * n)),                   # right-neighbour gadgets
        ]
        if m:
            coloring.append(set(range(2 * n, 2 * n + m)))           # mirror gadgets
        if g:
            coloring.append(set(range(2 * n + m, 2 * n + m + g)))   # glue gadgets
        if chain_vertex_set:
            coloring.append(chain_vertex_set)                         # all chain vertices

        return Graph(
            number_of_vertices=total,
            directed=False,
            adjacency_dict={v: list(neighbors) for v, neighbors in adj.items()},
            vertex_coloring=coloring,
        )

    @classmethod
    def certificate(cls, state: PartialSolution) -> bytes:
        """Returns the nauty canonical certificate for *state*.

        The certificate is a compact byte string that is **identical for all
        isomorphic tilings** and **distinct for non-isomorphic ones**.  It can
        be used directly as a dictionary key or set element for O(1) lookup.
        """
        from pynauty import certificate as _nauty_certificate
        return _nauty_certificate(cls._build_graph(state))


# =============================================================================
# Nauty-based pruner (replaces SolutionPruner)
# =============================================================================


class NautyPruner(SolutionPruner):
    """Deduplicates solver output using nauty canonical certificates.

    This completely replaces the two-phase ``SolutionPruner`` pipeline:

    * **Phase 1 eliminated** — ``_is_canonical_labeling`` is no longer needed.
      Any labeling of a tiling yields the same certificate, so we never have
      to check whether a particular labeling is the "canonical" one.

    * **Phase 2 replaced** — instead of checking each new solution against
      every previously accepted one (O(n²) total), we compute a nauty
      certificate for every solution (parallelised via ``pool.map``) and look
      it up in a hash set (O(1) per solution, O(n) total).

    All file-parsing and output-writing logic is inherited unchanged from
    ``SolutionPruner``; only ``run`` and ``_process_file`` are overridden.
    """

    def __init__(self, output_dir: str = os.path.join("solutions", "nauty")) -> None:
        # We intentionally bypass SolutionPruner.__init__ because we do not
        # need _solutions_by_signature; a flat certificate set is sufficient.
        self.output_dir = output_dir
        self._seen_certificates: Set[bytes] = set()
        self.solutions_per_k: Dict[int, int] = {}

    def run(self, listfile_paths: List[str], num_workers: Optional[int] = None) -> None:
        """Deduplicates all solver output files.

        Files are processed sequentially (the certificate set accumulates
        across all files), but within each file all certificate computations
        are dispatched to the pool simultaneously.

        Args:
            listfile_paths: Paths to the solver-generated solution list files.
            num_workers: Pool size; defaults to ``os.cpu_count()``.
        """
        num_workers = num_workers or os.cpu_count() or 1
        os.makedirs(self.output_dir, exist_ok=True)
        with multiprocessing.Pool(num_workers) as pool:
            for path in listfile_paths:
                self._process_file(path, pool)

    def _process_file(self, path: str, pool: multiprocessing.Pool) -> None:
        """Reads one solver output file and writes unique tilings to disk.

        Algorithm
        ---------
        1. **Decode** all solutions from the file (sequential; cheap).
        2. **Compute certificates** for all solutions in parallel via
           ``pool.map(_compute_certificate_worker, states)``.
        3. **Hash-set filter**: iterate the (solution, certificate) pairs
           sequentially, skipping any certificate already in
           ``_seen_certificates`` and writing the rest.

        The sequential step 3 is O(n) in the number of solutions and
        dominated by I/O, not computation.
        """
        with open(path, "r") as f:
            lines = f.readlines()

        combo_code = os.path.splitext(os.path.basename(path))[0].replace("eusolver_", "")
        out_dir = os.path.join(self.output_dir, combo_code)
        os.makedirs(out_dir, exist_ok=True)
        pruned_path = os.path.join(out_dir, "eupruned.txt")

        records = self._parse_all_solutions(lines)
        if not records:
            return

        # Step 2: parallel certificate computation.
        states = [r[4] for r in records]
        certificates: List[bytes] = pool.map(_compute_certificate_worker, states)

        # Step 3: sequential hash-set deduplication and output.
        with open(pruned_path, "w") as pruned_out:
            for record, cert in zip(records, certificates):
                vertex_line, signature_line, tes_line, conway_line, state, count_sig = record

                if cert in self._seen_certificates:
                    continue
                self._seen_certificates.add(cert)

                k = state.num_polygons
                self.solutions_per_k[k] = self.solutions_per_k.get(k, 0) + 1

                tes_marker = tes_line.index("eu")
                old_tes_relpath = tes_line[tes_marker:-1]
                tes_filename = "eu " + old_tes_relpath[len("eu raw "):]
                tes_path = os.path.join(out_dir, tes_filename)

                pruned_out.write(vertex_line)
                pruned_out.write(signature_line)
                pruned_out.write(f"Count type: {count_sig}\n")
                pruned_out.write(tes_line)
                pruned_out.write(conway_line)
                ConwayCycleWriter.write_cycle_final(state, pruned_out, tes_path, signature_line.rstrip())
                pruned_out.write("\n")



def _canonical_and_wl_worker(state: PartialSolution) -> Tuple[bool, str, str]:
    """Combined worker: canonical-labeling check + WL hash in one pool call.

    Returns ``(is_canonical, log_text, wl_hash)``.  The WL hash is only
    computed when ``is_canonical`` is True; otherwise it is the empty string.
    This avoids paying the hashing cost for solutions that will be discarded
    by the canonical check.
    """
    buf = io.StringIO()
    is_canonical = SolutionPruner._is_canonical_labeling(state, buf)
    wl_hash = WLHasher.hash(state) if is_canonical else ""
    return is_canonical, buf.getvalue(), wl_hash



    """Computes the nauty canonical certificate for *state* in a worker process.

    Module-level so it is picklable by ``multiprocessing``.
    """
    return TilingCanonicalizer.certificate(state)


def _run_solver_worker(
    args: Tuple[List[PartialSolution], int, str],
) -> Tuple[Dict[str, str], int, int, Dict[str, int], Dict[Tuple[int, ...], int]]:
    """Runs the solver from a pre-built list of initial states in its own
    process, writing output into a dedicated worker subdirectory.

    Returns the solution-file map, counts, and accumulator dicts so that the
    parent process can merge results from all workers.
    """
    initial_states, max_polygons, output_dir = args
    solver = EuclideanSolver(max_polygons=max_polygons, output_dir=output_dir)
    solver._queue = list(initial_states)
    solver.run()
    return (
        solver.solution_files,
        solver.partials_checked,
        solver.solutions_found,
        solver._run_totals,
        solver._vertex_combo_counts,
    )


def _canonical_check_worker(state: PartialSolution) -> Tuple[bool, str]:
    """Checks whether *state* has a canonical labeling in its own process.

    Returns (is_canonical, log_text) so that the parent can write the log
    without needing a shared file handle.
    """
    buf = io.StringIO()
    result = SolutionPruner._is_canonical_labeling(state, buf)
    return result, buf.getvalue()


def _match_check_worker(
    args: Tuple[PartialSolution, PartialSolution],
) -> Tuple[bool, str]:
    """Checks whether two solutions are isomorphic in its own process.

    Returns (is_match, log_text) so that the parent can write the log
    without needing a shared file handle.
    """
    state, other = args
    buf = io.StringIO()
    result = SolutionPruner._solutions_match(state, other, buf)
    return result, buf.getvalue()


# =============================================================================
# Entry point
# =============================================================================


def main() -> None:
    num_workers = os.cpu_count() or 1

    solver = EuclideanSolver(max_polygons=5, output_dir="solutions")
    solver.run_parallel(num_workers=num_workers)
    print(
        f"Solver: {num_workers} workers, "
        f"{solver.partials_checked} partial solutions checked, "
        f"{solver.solutions_found} complete tilings found."
    )

    pruner = WLPruner(output_dir=os.path.join("solutions", "wl"))
    pruner.run(list(solver.solution_files.values()), num_workers=num_workers)

    print("Unique tilings found after pruning (WL hash):")
    total = 0
    for k in sorted(pruner.solutions_per_k):
        count = pruner.solutions_per_k[k]
        total += count
        print(f"  k = {k} -> {count} solution{'s' if count != 1 else ''}")
    print(f"  total: {total}")


if __name__ == "__main__":
    main()
