#!/usr/bin/env python3
"""Render the typed darts of a random pruned tiling as an SVG.

The pruned database (``wl/tilings.sqlite3``, TES schema 2) stores tile-level
Conway symbols only, not the solver's vertex-level ``State``.  This script
rebuilds an equivalent typed dart map from the TES orbifold symbol:

* a dart is a half-edge at a vertex; dart ``(t, i, +)`` starts side ``i`` of
  tile ``t``, dart ``(t, i, -)`` is its mirror image (both chiralities are
  kept, like the solver's ``*`` labels);
* ``rneig`` steps to the next half-edge around the vertex (arrows);
* ``glue`` pairs the two half-edges of one edge (dashed lines);
* each dart is typed by the size of the polygon between it and ``rneig``.

Dart indices are therefore not the solver's State indices.

Usage:
    scripts/visualize_darts.py [--db PATH] [--id N | --seed S] [--combo NAME] [-o out.svg]
"""

import argparse
import math
import random
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

DEFAULT_DB = Path.home() / "data" / "k25-enum" / "wl" / "tilings.sqlite3"
COLORS = {3: "#f6c85f", 4: "#6fb7e0", 6: "#9fd17a", 12: "#e58fb5"}


# ---------------------------------------------------------------- database
def get_varint(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return value, pos


def get_field(data, pos):
    length, pos = get_varint(data, pos)
    return data[pos:pos + length].decode(), pos + length


def zstd_decompress(blob):
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompress(blob)
    except ImportError:
        return subprocess.run(["zstd", "-dqc"], input=blob, stdout=subprocess.PIPE,
                              check=True).stdout


def load_entry(db_path, entry_id=None, combo=None, rng=random):
    db = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    version = db.execute("SELECT value FROM metadata WHERE key='schema_version'").fetchone()
    if not version or version[0] != "2":
        sys.exit("only TES schema 2 databases are supported")
    if entry_id is None:
        where, args = "", ()
        if combo:
            where, args = "WHERE combo_id=(SELECT id FROM combos WHERE name=?)", (combo,)
        lo, hi = db.execute(f"SELECT MIN(first_entry), MAX(first_entry+record_count-1) "
                            f"FROM chunks {where}", args).fetchone()
        if lo is None:
            sys.exit(f"no entries for combo {combo!r}")
        # Pick a random id; retry if it falls outside the combo's chunks.
        while True:
            entry_id = rng.randint(lo, hi)
            row = db.execute(f"SELECT first_entry, record_count, data, combo_id FROM chunks "
                             f"WHERE first_entry<=? ORDER BY first_entry DESC LIMIT 1",
                             (entry_id,)).fetchone()
            if row and entry_id < row[0] + row[1] and (
                    not combo or db.execute("SELECT name FROM combos WHERE id=?",
                                            (row[3],)).fetchone()[0] == combo):
                break
    else:
        row = db.execute("SELECT first_entry, record_count, data, combo_id FROM chunks "
                         "WHERE first_entry<=? ORDER BY first_entry DESC LIMIT 1",
                         (entry_id,)).fetchone()
        if not row or entry_id >= row[0] + row[1]:
            sys.exit(f"entry {entry_id} not found")
    first, _, blob, combo_id = row
    combo_name = db.execute("SELECT name FROM combos WHERE id=?", (combo_id,)).fetchone()[0]
    raw, pos = zstd_decompress(blob), 0
    for i in range(entry_id - first + 1):
        signature, pos = get_field(raw, pos)
        filename, pos = get_field(raw, pos)
        document, pos = get_field(raw, pos)
    return dict(id=entry_id, combo=combo_name, signature=signature,
                filename=filename, document=document)


# ---------------------------------------------------------------- TES parsing
def parse_edge(text):
    m = re.fullmatch(r"(\d+)('*)(?:@(\d+))?", text)
    if not m:
        raise ValueError(f"bad Conway edge {text!r}")
    tile = int(m.group(3)) if m.group(3) else len(m.group(2))
    return tile, int(m.group(1))


def parse_tes(document):
    sizes, repeats, conway = [], {}, None
    for line in document.splitlines():
        if line.startswith("unittile("):
            sizes.append(line.count(",") + 1)
        elif line.startswith("repeat("):
            t, r = map(int, re.findall(r"\d+", line))
            repeats[t] = r
        elif line.startswith("conway("):
            conway = re.search(r'"(.*)"', line).group(1)
    gluings = []  # (mirror, edge_a, edge_b)
    for bracket, body in re.findall(r"([(\[])([^)\]]*)[)\]]", conway):
        parts = [parse_edge(p) for p in body.split()]
        gluings.append((bracket == "[", parts[0], parts[-1]))
    sides = [0] * len(sizes)
    for _, a, b in gluings:
        for t, i in (a, b):
            sides[t] = max(sides[t], i + 1)
    for t, n in enumerate(sides):
        if n * repeats.get(t, 1) != sizes[t]:
            raise ValueError(f"tile {t}: {n} sides x repeat {repeats.get(t, 1)} != {sizes[t]}")
    return sizes, sides, gluings


# ---------------------------------------------------------------- dart map
def build_darts(sizes, sides, gluings):
    index = {}
    darts = []  # (tile, side, chirality)
    for chir in (+1, -1):
        for t, n in enumerate(sides):
            for i in range(n):
                index[(t, i, chir)] = len(darts)
                darts.append((t, i, chir))
    n = len(darts)
    glue = [None] * n

    def pair(x, y):
        a, b = index[x], index[y]
        if glue[a] is not None or glue[b] is not None:
            raise ValueError("edge glued twice")
        glue[a], glue[b] = b, a

    for mirror, (ta, ia), (tb, ib) in gluings:
        for c in (+1, -1):
            if not mirror:  # rotation/translation: same chirality
                pair((ta, ia, c), (tb, ib, c))
            elif c == +1:   # reflection/glide: swaps chirality
                pair((ta, ia, +1), (tb, ib, -1))
                if (ta, ia) != (tb, ib):
                    pair((tb, ib, +1), (ta, ia, -1))
    if any(g is None for g in glue):
        raise ValueError("unglued dart")
    # rneig: + copy turns towards the previous side, - copy towards the next.
    rneig = [glue[index[(t, (i - c) % sides[t], c)]] for (t, i, c) in darts]
    if sorted(rneig) != list(range(n)):
        raise ValueError("rneig is not a permutation")
    psize = [sizes[t] for (t, _, _) in darts]
    mirro = [index[(t, i, -c)] for (t, i, c) in darts]
    return darts, glue, rneig, psize, mirro


def vertex_cycles(rneig):
    seen, cycles = set(), []
    for d in range(len(rneig)):
        if d in seen:
            continue
        cyc, x = [], d
        while x not in seen:
            seen.add(x)
            cyc.append(x)
            x = rneig[x]
        cycles.append(cyc)
    return cycles


# ---------------------------------------------------------------- SVG
def render_svg(entry, darts, glue, rneig, psize, cycles):
    box_w, box_h, cell_pad = 46, 30, 60
    radius = lambda m: max(48.0, m * (box_w + 14) / (2 * math.pi))
    cols = max(1, math.ceil(math.sqrt(len(cycles))))
    pitch = 2 * max(radius(len(c)) for c in cycles) + box_w + cell_pad
    top = 90
    width = cols * pitch + 40
    height = top + math.ceil(len(cycles) / cols) * pitch + 40

    pos, cells, out = {}, [], []
    for k, cyc in enumerate(cycles):
        cx = 20 + (k % cols + 0.5) * pitch
        cy = top + (k // cols + 0.5) * pitch
        r = radius(len(cyc))
        chir = "+" if darts[cyc[0]][2] > 0 else "\u2212"
        fan = ",".join(str(psize[d]) for d in cyc)
        cells.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r + box_w * 0.8:.1f}" '
                   f'class="cell"/>')
        out.append(f'<text x="{cx:.1f}" y="{cy - 4:.1f}" class="vlabel">v{k}{chir}</text>')
        out.append(f'<text x="{cx:.1f}" y="{cy + 12:.1f}" class="vfan">({fan})</text>')
        for j, d in enumerate(cyc):
            a = -math.pi / 2 + 2 * math.pi * j / len(cyc)  # clockwise on screen
            pos[d] = (cx + r * math.cos(a), cy + r * math.sin(a), cx, cy)
        for j, d in enumerate(cyc):  # rneig arrows
            if len(cyc) == 1:
                x, y, _, _ = pos[d]
                out.append(f'<path d="M{x + box_w / 2:.1f},{y:.1f} c30,-30 30,30 0,{box_h / 2:.1f}" '
                           f'class="rn" marker-end="url(#arr)"/>')
                continue
            x1, y1, _, _ = pos[d]
            x2, y2, _, _ = pos[rneig[d]]
            a1 = math.atan2(y2 - y1, x2 - x1)
            sx, sy = x1 + 0.5 * box_w * math.cos(a1) * 0.9, y1 + 0.5 * box_h * math.sin(a1) * 1.2
            ex, ey = x2 - 0.5 * box_w * math.cos(a1) * 1.0, y2 - 0.5 * box_h * math.sin(a1) * 1.4
            mx, my = (sx + ex) / 2, (sy + ey) / 2
            ox, oy = mx - cx, my - cy
            norm = math.hypot(ox, oy) or 1
            bulge = 0.18 * math.hypot(ex - sx, ey - sy)
            qx, qy = mx + ox / norm * bulge, my + oy / norm * bulge
            out.append(f'<path d="M{sx:.1f},{sy:.1f} Q{qx:.1f},{qy:.1f} {ex:.1f},{ey:.1f}" '
                       f'class="rn" marker-end="url(#arr)"/>')

    glues = []
    for d, g in enumerate(glue):
        x1, y1, c1x, c1y = pos[d]
        if g == d:  # edge folded onto itself (half-turn centre on the edge)
            ox, oy = x1 - c1x, y1 - c1y
            nrm = math.hypot(ox, oy) or 1
            ux, uy = ox / nrm, oy / nrm
            px, py = x1 + ux * box_w * 0.55, y1 + uy * box_h * 0.8
            glues.append(f'<path d="M{px:.1f},{py:.1f} c{ux * 40 - uy * 25:.1f},{uy * 40 + ux * 25:.1f} '
                         f'{ux * 40 + uy * 25:.1f},{uy * 40 - ux * 25:.1f} 0,0" class="gl"/>')
        elif d < g:
            x2, y2, c2x, c2y = pos[g]
            if (c1x, c1y) == (c2x, c2y):
                qx, qy = c1x, c1y
            else:  # bow outward from both cell centres
                qx = (x1 + x2) / 2 + 0.25 * ((x1 - c1x) + (x2 - c2x))
                qy = (y1 + y2) / 2 + 0.25 * ((y1 - c1y) + (y2 - c2y))
            glues.append(f'<path d="M{x1:.1f},{y1:.1f} Q{qx:.1f},{qy:.1f} {x2:.1f},{y2:.1f}" '
                         f'class="gl"><title>glue {d} \u2194 {g}</title></path>')

    boxes = []
    for d, (x, y, _, _) in pos.items():
        t, i, c = darts[d]
        col = COLORS.get(psize[d], "#ccc")
        boxes.append(
            f'<g><title>dart {d}: tile {t} side {i} {"+" if c > 0 else "-"}, '
            f'polygon {psize[d]}, rneig {rneig[d]}, glue {glue[d]}</title>'
            f'<rect x="{x - box_w / 2:.1f}" y="{y - box_h / 2:.1f}" width="{box_w}" '
            f'height="{box_h}" rx="5" fill="{col}" class="dart"/>'
            f'<text x="{x:.1f}" y="{y - 2:.1f}" class="idx">{d}</text>'
            f'<text x="{x:.1f}" y="{y + 10:.1f}" class="ps">{psize[d]}-gon</text></g>')

    legend = " ".join(
        f'<rect x="{20 + 70 * j}" y="56" width="14" height="14" fill="{c}" class="dart"/>'
        f'<text x="{38 + 70 * j}" y="67" class="leg">{s}-gon</text>'
        for j, (s, c) in enumerate(sorted(COLORS.items())))
    title = (f'entry {entry["id"]} \u2014 {entry["combo"]} \u2014 {len(darts)} darts, '
             f'{len(cycles)} vertex cells')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" \
viewBox="0 0 {width:.0f} {height:.0f}" font-family="sans-serif">
<defs><marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" \
orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#333"/></marker></defs>
<style>
.cell{{fill:#fafafa;stroke:#bbb;stroke-width:1}}
.dart{{stroke:#333;stroke-width:1}}
.idx{{font-size:12px;font-weight:bold;text-anchor:middle}}
.ps{{font-size:9px;text-anchor:middle;fill:#222}}
.vlabel{{font-size:13px;font-weight:bold;text-anchor:middle;fill:#555}}
.vfan{{font-size:10px;text-anchor:middle;fill:#777}}
.rn{{fill:none;stroke:#333;stroke-width:1.3}}
.gl{{fill:none;stroke:#c0392b;stroke-width:1;stroke-dasharray:5,4;opacity:.55}}
.leg{{font-size:11px}}
</style>
<rect width="100%" height="100%" fill="white"/>
<text x="20" y="24" font-size="15" font-weight="bold">{escape(title)}</text>
<text x="20" y="44" font-size="11" fill="#555">{escape(entry["signature"])}</text>
{legend}
<text x="{20 + 70 * len(COLORS)}" y="67" class="leg">solid arrow: rneig; red dashed: glue</text>
{chr(10).join(cells)}
{chr(10).join(glues)}
{chr(10).join(out)}
{chr(10).join(boxes)}
</svg>
'''


def smacof(dist, w, iters, seed, mds=True, init=None):
    """Weighted stress majorization from `init`, classical MDS or a random
    placement."""
    import numpy as np
    n = len(dist)
    if n == 1:
        return np.zeros((1, 2))
    rng = np.random.default_rng(seed)
    if init is not None:
        x = init.copy()
    elif mds:
        j = np.eye(n) - 1.0 / n
        vals, vecs = np.linalg.eigh(-0.5 * j @ (dist ** 2) @ j)
        x = vecs[:, -2:] * np.sqrt(np.maximum(vals[-2:], 1e-9))
        x += rng.normal(scale=1e-2, size=x.shape)
    else:
        x = rng.uniform(0, dist.max(), size=(n, 2))
    lap = -w.copy()
    np.fill_diagonal(lap, w.sum(axis=1))
    lap_pinv = np.linalg.pinv(lap)
    for _ in range(iters):
        cur = np.sqrt(((x[:, None, :] - x[None, :, :]) ** 2).sum(-1))
        ratio = np.where(cur > 1e-12, w * dist / np.maximum(cur, 1e-12), 0.0)
        bz = -ratio
        np.fill_diagonal(bz, ratio.sum(axis=1))
        new = lap_pinv @ (bz @ x)
        if np.abs(new - x).max() < 1e-7:
            return new
        x = new
    return x


def count_crossings(x, segments):
    """Number of properly crossing pairs among segments (index pairs)."""
    import numpy as np
    if len(segments) < 2:
        return 0
    seg = np.array(segments)
    p, r = x[seg[:, 0]], x[seg[:, 1]] - x[seg[:, 0]]
    cross = lambda a, b: a[..., 0] * b[..., 1] - a[..., 1] * b[..., 0]
    qp = p[None, :, :] - p[:, None, :]
    rxs = cross(r[:, None, :], r[None, :, :])
    with np.errstate(divide="ignore", invalid="ignore"):
        t = cross(qp, r[None, :, :]) / rxs
        u = cross(qp, r[:, None, :]) / rxs
    eps = 1e-6
    hit = (np.abs(rxs) > 1e-12) & (t > eps) & (t < 1 - eps) & (u > eps) & (u < 1 - eps)
    return int(np.triu(hit, 1).sum())


def snap_rings(x, rings, dist, w, iters):
    """Replace each ring by the best-fitting regular polygon (rotation and
    reflection fitted by Procrustes), then relax again.  This removes twisted
    rings that stress majorization alone can get stuck in."""
    import numpy as np
    x = x.copy()
    for ring in rings:
        k = len(ring)
        if k < 3:
            continue
        radius = dist[ring[0], ring[1]] / (2 * math.sin(math.pi / k))
        ang = 2 * math.pi * np.arange(k) / k
        model = radius * np.stack([np.cos(ang), np.sin(ang)], axis=1)
        cur = x[ring]
        centre = cur.mean(axis=0)
        best = None
        for flip in (1, -1):
            m = model * np.array([1, flip])
            u, _, vt = np.linalg.svd(m.T @ (cur - centre))
            rot = u @ vt
            if np.linalg.det(rot) < 0:  # keep the chosen handedness
                u[:, -1] *= -1
                rot = u @ vt
            fit = m @ rot + centre
            err = ((fit - cur) ** 2).sum()
            if best is None or err < best[0]:
                best = (err, fit)
        x[ring] = best[1]
    return smacof(dist, w, iters, 0, init=x)


def graph_layout(n, glue, rneig, cycles, rneig_len=1.0, glue_len=1.8,
                 ring_weight=100.0, iters=600, seed=0, restarts=8):
    """Stress-majorization layout of the dart graph.

    Darts are free nodes.  Target distances are weighted shortest-path lengths
    (rneig edges short, glue edges long), which untangles the glue edges; the
    darts of one vertex are additionally pinned to a regular polygon so rings
    stay convex.  Connected components (e.g. the two mirror copies of a
    chiral tiling) are laid out separately and packed side by side.
    """
    import numpy as np
    from heapq import heappush, heappop

    adj = [[] for _ in range(n)]
    for d in range(n):
        if rneig[d] != d:
            adj[d].append((rneig[d], rneig_len))
            adj[rneig[d]].append((d, rneig_len))
        if glue[d] != d:
            adj[d].append((glue[d], glue_len))
    ring_of = {}
    for cyc in cycles:
        for k, d in enumerate(cyc):
            ring_of[d] = (cyc, k)

    comp, comps = [-1] * n, []
    for s in range(n):
        if comp[s] < 0:
            comp[s], stack, members = len(comps), [s], []
            while stack:
                u = stack.pop()
                members.append(u)
                for v, _ in adj[u]:
                    if comp[v] < 0:
                        comp[v] = comp[s]
                        stack.append(v)
            comps.append(sorted(members))

    pos = np.zeros((n, 2))
    offset = 0.0
    for members in comps:
        idx = {d: i for i, d in enumerate(members)}
        m = len(members)
        dist = np.full((m, m), np.inf)
        for s in members:  # Dijkstra
            row, heap = dist[idx[s]], [(0.0, s)]
            row[idx[s]] = 0.0
            while heap:
                du, u = heappop(heap)
                if du > row[idx[u]]:
                    continue
                for v, wgt in adj[u]:
                    if du + wgt < row[idx[v]]:
                        row[idx[v]] = du + wgt
                        heappush(heap, (du + wgt, v))
        w = np.where(dist > 0, 1.0 / np.maximum(dist, 1e-9) ** 2, 0.0)
        for cyc in {id(ring_of[d][0]): ring_of[d][0] for d in members}.values():
            k = len(cyc)
            if k < 2:
                continue
            radius = rneig_len / (2 * math.sin(math.pi / k)) if k > 2 else rneig_len / 2
            for a in range(k):
                for b in range(k):
                    if a != b:
                        chord = 2 * radius * math.sin(math.pi * abs(a - b) / k)
                        i, j = idx[cyc[a]], idx[cyc[b]]
                        dist[i, j] = chord
                        w[i, j] = ring_weight / chord ** 2
        rings = [[idx[d] for d in cyc] for cyc in
                 {id(ring_of[d][0]): ring_of[d][0] for d in members}.values()]
        segs = sorted({tuple(sorted((idx[d], idx[e]))) for d in members
                       for e in (rneig[d], glue[d]) if e != d})
        ring_segs = sorted({tuple(sorted((idx[d], idx[rneig[d]]))) for d in members
                            if rneig[d] != d})
        best = None
        for r in range(restarts):
            cand = smacof(dist, w, iters, seed * restarts + r, mds=(r == 0))
            cand = snap_rings(cand, rings, dist, w, iters)
            # a twisted vertex ring is far worse than a crossing glue edge
            score = count_crossings(cand, segs) + 1000 * count_crossings(cand, ring_segs)
            if best is None or score < best[0]:
                best = (score, cand)
            if score == 0:
                break
        x = best[1]
        # mirror the component so most rings run clockwise on screen (y down)
        area = 0.0
        for cyc in {id(ring_of[d][0]): ring_of[d][0] for d in members}.values():
            pts = [x[idx[d]] for d in cyc]
            area += sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(pts, pts[1:] + pts[:1]))
        if area < 0:
            x[:, 0] = -x[:, 0]
        x -= x.min(axis=0)
        x[:, 0] += offset
        offset = x[:, 0].max() + 2.5 * glue_len
        for d in members:
            pos[d] = x[idx[d]]
    return pos


def render_graph_svg(entry, darts, glue, rneig, psize, cycles, seed=0, restarts=8):
    import numpy as np
    n = len(darts)
    box_w, box_h, margin, top = 46, 30, 60, 90
    x = graph_layout(n, glue, rneig, cycles, seed=seed, restarts=restarts)
    # scale so the shortest rneig edge is comfortably longer than a box
    lens = [np.linalg.norm(x[d] - x[rneig[d]]) for d in range(n) if rneig[d] != d]
    scale = 80.0 / max(np.median(lens) if lens else 1.0, 1e-6)
    x = (x - x.min(axis=0)) * scale
    pos = {d: (x[d, 0] + margin, x[d, 1] + margin + top) for d in range(n)}
    width = x[:, 0].max() + 2 * margin
    height = x[:, 1].max() + 2 * margin + top

    cells, labels, arrows, glues, boxes = [], [], [], [], []
    for k, cyc in enumerate(cycles):
        pts = [pos[d] for d in cyc]
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        chir = "+" if darts[cyc[0]][2] > 0 else "\u2212"
        fan = ",".join(str(psize[d]) for d in cyc)
        path = "M" + " L".join(f"{px:.1f},{py:.1f}" for px, py in pts) + " Z"
        cells.append(f'<path d="{path}" class="cell"/>')
        labels.append(f'<text x="{cx:.1f}" y="{cy - 2:.1f}" class="vlabel">v{k}{chir}</text>'
                      f'<text x="{cx:.1f}" y="{cy + 11:.1f}" class="vfan">({fan})</text>')

    def clip(p, q):
        """Point where segment p->q leaves the box centred at p."""
        dx, dy = q[0] - p[0], q[1] - p[1]
        t = min(box_w / 2 / abs(dx) if dx else math.inf,
                box_h / 2 / abs(dy) if dy else math.inf)
        return p[0] + dx * min(t, 1), p[1] + dy * min(t, 1)

    for d in range(n):
        p = pos[d]
        r = rneig[d]
        if r == d:
            arrows.append(f'<path d="M{p[0] + box_w / 2:.1f},{p[1]:.1f} c30,-30 30,30 0,'
                          f'{box_h / 2:.1f}" class="rn" marker-end="url(#arr)"/>')
        else:
            q = pos[r]
            (sx, sy), (ex, ey) = clip(p, q), clip(q, p)
            arrows.append(f'<line x1="{sx:.1f}" y1="{sy:.1f}" x2="{ex:.1f}" y2="{ey:.1f}" '
                          f'class="rn" marker-end="url(#arr)"/>')
        g = glue[d]
        if g == d:
            glues.append(f'<circle cx="{p[0]:.1f}" cy="{p[1] - box_h / 2 - 9:.1f}" r="9" '
                         f'class="gl"><title>glue {d} \u2194 {d}</title></circle>')
        elif d < g:
            q = pos[g]
            glues.append(f'<line x1="{p[0]:.1f}" y1="{p[1]:.1f}" x2="{q[0]:.1f}" '
                         f'y2="{q[1]:.1f}" class="gl"><title>glue {d} \u2194 {g}</title></line>')
        t, i, c = darts[d]
        boxes.append(
            f'<g><title>dart {d}: tile {t} side {i} {"+" if c > 0 else "-"}, '
            f'polygon {psize[d]}, rneig {rneig[d]}, glue {glue[d]}</title>'
            f'<rect x="{p[0] - box_w / 2:.1f}" y="{p[1] - box_h / 2:.1f}" width="{box_w}" '
            f'height="{box_h}" rx="5" fill="{COLORS.get(psize[d], "#ccc")}" class="dart"/>'
            f'<text x="{p[0]:.1f}" y="{p[1] - 2:.1f}" class="idx">{d}</text>'
            f'<text x="{p[0]:.1f}" y="{p[1] + 10:.1f}" class="ps">{psize[d]}-gon</text></g>')
    return svg_document(entry, darts, cycles, width, height,
                        cells + glues + arrows + labels + boxes)


def svg_document(entry, darts, cycles, width, height, body):
    legend = " ".join(
        f'<rect x="{20 + 70 * j}" y="56" width="14" height="14" fill="{c}" class="dart"/>'
        f'<text x="{38 + 70 * j}" y="67" class="leg">{s}-gon</text>'
        for j, (s, c) in enumerate(sorted(COLORS.items())))
    title = (f'entry {entry["id"]} \u2014 {entry["combo"]} \u2014 {len(darts)} darts, '
             f'{len(cycles)} vertex cells')
    width = max(width, 640)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width:.0f}" height="{height:.0f}" \
viewBox="0 0 {width:.0f} {height:.0f}" font-family="sans-serif">
<defs><marker id="arr" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" \
orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#333"/></marker></defs>
<style>
.cell{{fill:#eef1f5;stroke:#eef1f5;stroke-width:70;stroke-linejoin:round;opacity:.9}}
.dart{{stroke:#333;stroke-width:1}}
.idx{{font-size:12px;font-weight:bold;text-anchor:middle}}
.ps{{font-size:9px;text-anchor:middle;fill:#222}}
.vlabel{{font-size:12px;font-weight:bold;text-anchor:middle;fill:#555}}
.vfan{{font-size:9px;text-anchor:middle;fill:#777}}
.rn{{fill:none;stroke:#333;stroke-width:1.3}}
.gl{{fill:none;stroke:#c0392b;stroke-width:1.2;stroke-dasharray:5,4;opacity:.7}}
.leg{{font-size:11px}}
</style>
<rect width="100%" height="100%" fill="white"/>
<text x="20" y="24" font-size="15" font-weight="bold">{escape(title)}</text>
<text x="20" y="44" font-size="11" fill="#555">{escape(entry["signature"])}</text>
{legend}
<text x="{20 + 70 * len(COLORS)}" y="67" class="leg">solid arrow: rneig; red dashed: glue</text>
{chr(10).join(body)}
</svg>
'''


def escape(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--db", type=Path, default=DEFAULT_DB)
    ap.add_argument("--id", type=int, help="entry id (default: random)")
    ap.add_argument("--seed", type=int, help="random seed")
    ap.add_argument("--combo", help="restrict random choice to a combo, e.g. 05_34")
    ap.add_argument("-o", "--output", type=Path, default=Path("darts.svg"))
    ap.add_argument("--restarts", type=int, default=8,
                    help="graph layout: random restarts, keep fewest crossings (default 8)")
    ap.add_argument("--layout", choices=("graph", "cells"), default="graph",
                    help="graph: stress-relaxed dart graph (default); cells: grid of vertex rings")
    args = ap.parse_args()

    entry = load_entry(args.db, args.id, args.combo, random.Random(args.seed))
    sizes, sides, gluings = parse_tes(entry["document"])
    darts, glue, rneig, psize, _ = build_darts(sizes, sides, gluings)
    cycles = vertex_cycles(rneig)
    cycles.sort(key=lambda c: (darts[c[0]][2] < 0, min(c)))
    cycles = [c[c.index(min(c)):] + c[:c.index(min(c))] for c in cycles]
    if args.layout == "graph":
        svg = render_graph_svg(entry, darts, glue, rneig, psize, cycles,
                               seed=args.seed or 0, restarts=args.restarts)
    else:
        svg = render_svg(entry, darts, glue, rneig, psize, cycles)
    args.output.write_text(svg)
    print(f"entry {entry['id']} ({entry['combo']}): {len(darts)} darts, "
          f"{len(cycles)} vertices -> {args.output}")


if __name__ == "__main__":
    main()
