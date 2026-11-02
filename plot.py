"""Visualisation for the spinlock benchmark — a dark dashboard.

Separated from the benchmark runner (test_bench.py) so the plot can be iterated
on without touching measurement code. Reads bench_results.csv (and an optional
bench_meta.json sidecar) and renders bench_result.png.

    python3 plot.py                       # redraw from the committed CSV
    python3 plot.py --cpu-model "..."     # override the title's CPU label

Design notes / statistical choices:
  * Metric = nanoseconds per lock/unlock, amortised across ALL threads
    (elapsed / total_ops). The raw CSV normalises per thread, so the stored
    value is multiplied by the thread count internally; dividing it back by the
    thread count yields a latency that is comparable across thread counts, so a
    rising line means genuine contention overhead, not just more work retired.
  * Central mark = median of the runs (robust for right-skewed, floor-bounded
    latency). Each lock's runs form a candle (body = interquartile range,
    wicks = min/max), showing spread without an error bar posing as a CI.
  * Threads beyond the pinned core count are an oversubscribed regime (shaded)
    that hits every lock, not just MCS; disclosed rather than hidden.
  * Heatmap ratios are medians of per-launch paired ratios. Cells that are not
    significant at a 5% false discovery rate (paired bootstrap p-values,
    Benjamini-Hochberg over both heatmaps) are greyed and hatched.
"""
from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm

DEFAULT_CSV = Path("bench_results.csv")
DEFAULT_META = Path("bench_meta.json")
DEFAULT_PNG = Path("bench_result.png")

# --- Dark theme (Binance-style design-system tokens) --------------------------
PAGE_BG = "#0b0e11"     # canvas-dark   — page background
PANEL_BG = "#1e2329"    # surface-card  — panel card background
PANEL_EDGE = "#2b3139"  # surface-elevated — panel border / hatched cells
GRID = "#2b3139"        # subtle gridlines
INK = "#eaecef"         # body-on-dark  — primary text
INK_MUTED = "#929aa5"   # muted-strong  — secondary text
INK_FAINT = "#707a8a"   # muted         — tertiary text
CAUTION = "#fcd535"     # primary (Binance yellow) — the oversubscribed regime
ACCENT = "#2dbdb6"      # accent-turquoise — sparing KPI accent

# One colour per lock, from the trading-semantics tokens (up / down / info),
# plus amber for the parking TTAS and cyan for the mutex baseline (validated
# as distinguishable from the three originals on the dark panel). Reinforced
# with a distinct marker each so identity is never colour-alone.
LOCKS = ["mcs", "ttas", "ttas_park", "pspin", "pmutex"]  # legend/draw order
COLOR = {"ttas": "#0ecb81", "mcs": "#f6465d", "pspin": "#3b82f6",
         "ttas_park": "#e0a800", "pmutex": "#22d3ee"}
MARKER = {"ttas": "o", "mcs": "s", "pspin": "^", "ttas_park": "D", "pmutex": "v"}
NAME = {"ttas": "Custom TTAS", "mcs": "Custom MCS", "pspin": "POSIX Spinlock",
        "ttas_park": "Custom TTAS Spin+Park", "pmutex": "POSIX Mutex"}
CANDLE_SPACING = 0.12
CANDLE_WIDTH = 0.085
TILE_INSET = 0.08

# Diverging map for the speedup heatmaps: trading-down red (TTAS slower) ↔ neutral
# panel ↔ trading-up green (TTAS faster), so speedup reads with the up/down
# semantic. Break-even sinks into the panel colour; the per-cell ratio number is
# the secondary encoding that carries direction independent of hue.
SPEEDUP_CMAP = LinearSegmentedColormap.from_list(
    "trading_div", ["#f6465d", "#5c2730", PANEL_BG, "#0f5c42", "#0ecb81"])


def load_csv(path: Path):
    raw: dict[tuple[int, int, str], dict[int, float]] = defaultdict(dict)
    workloads: set[int] = set()
    threads: set[int] = set()
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            lock = row["lock"]
            lock = "ttas" if lock == "spin" else lock  # legacy label
            if lock not in LOCKS:
                continue
            wl = int(row["workload_nops"])
            t = int(row["threads"])
            # Amortise across all threads: stored value is per-thread-normalised
            # ms/1M-ops, which numerically equals ns/op * thread_count. Divide it
            # back out so the y-axis is a true per-acquisition latency.
            ns_per_op = float(row["time_ms_normalized_to_1m_iters"]) / t
            raw[(wl, t, lock)][int(row["run_idx"])] = ns_per_op
            workloads.add(wl)
            threads.add(t)
    return raw, sorted(workloads), sorted(threads)


def load_meta(path: Path) -> dict:
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def cell(raw, wl, t, lock):
    v = raw.get((wl, t, lock))
    return np.asarray(list(v.values()), float) if v else None


# Pretendard (a bold humanist sans) matches the requested reference weight; fall
# back to Noto Sans / DejaVu where it is not installed. Heavier weights are
# separate family names in matplotlib, referenced directly where wanted.
FONT_STACK = ["Pretendard", "Noto Sans", "DejaVu Sans"]
FONT_HEAVY = "Pretendard ExtraBold"


def apply_theme():
    plt.rcParams.update({
        "figure.facecolor": PAGE_BG,
        "savefig.facecolor": PAGE_BG,
        "axes.facecolor": PANEL_BG,
        "axes.edgecolor": PANEL_EDGE,
        "axes.labelcolor": INK_MUTED,
        "axes.titlecolor": INK,
        "axes.titleweight": "bold",
        "axes.labelweight": "bold",
        "text.color": INK,
        "xtick.color": INK_MUTED,
        "ytick.color": INK_MUTED,
        "grid.color": GRID,
        "font.size": 10,
        "font.weight": "bold",
        "font.family": FONT_STACK,
    })


def shrink_to_fit(text, renderer, max_width=None, max_height=None, min_fontsize=6.0):
    fontsize = text.get_fontsize()
    while fontsize > min_fontsize:
        extent = text.get_window_extent(renderer=renderer)
        fits_width = max_width is None or extent.width <= max_width
        fits_height = max_height is None or extent.height <= max_height
        if fits_width and fits_height:
            break
        fontsize = round(fontsize - 0.25, 2)
        text.set_fontsize(fontsize)
    return fontsize


def _panel_frame(ax):
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(PANEL_EDGE)
    # length=0 on BOTH major and minor: the log axis's minor ticks (2..9 per
    # decade) otherwise read as stray hairs on the spine.
    ax.tick_params(which="both", length=0)


def draw_stat_tile(ax, value, unit, label, color):
    """A Grafana 'stat panel': a big coloured number over a small label on a flat
    filled panel (no border — the panel fill alone separates it from the page)."""
    ax.set_facecolor(PANEL_BG)
    ax.set_xticks([])
    ax.set_yticks([])
    for s in ax.spines.values():
        s.set_visible(False)
    x0 = TILE_INSET  # shared left edge for the number, its unit, and the label
    val_t = ax.text(x0, 0.60, value, transform=ax.transAxes, ha="left",
                    va="center", fontsize=26, fontfamily=FONT_HEAVY, color=color)
    if unit:
        # Place the unit just past the number's true right edge (measured), so the
        # inline unit stays glued to numbers of different widths.
        rend = ax.figure.canvas.get_renderer()
        x_right = ax.transAxes.inverted().transform(
            (val_t.get_window_extent(renderer=rend).x1, 0.0))[0]
        ax.text(x_right + 0.03, 0.555, unit, transform=ax.transAxes, ha="left",
                va="center", fontsize=12, color=INK_MUTED)
    return ax.text(x0, 0.27, label, transform=ax.transAxes, ha="left", va="center",
                   fontsize=10, color=INK_MUTED)


def draw_scaling_panel(ax, raw, wl, threads, ylim, cores, legend=False, nop_ns=None):
    _panel_frame(ax)
    xpos = list(range(len(threads)))

    # Shade the oversubscribed regime (threads > pinned cores) for every lock.
    over_idx = [j for j, t in enumerate(threads) if t > cores]
    if over_idx:
        left = min(over_idx) - 0.5
        ax.axvspan(left, len(threads) - 0.4, color=CAUTION, alpha=0.06, zorder=0)
        ax.axvline(left, color=CAUTION, alpha=0.30, lw=1.0, ls=":", zorder=1)
        if legend:  # label the regime once, near the legend panel
            ax.text(left + 0.04, ylim[1], "oversubscribed", color=CAUTION,
                    fontsize=7.0, ha="left", va="top", style="italic", zorder=6,
                    alpha=0.8)

    for lock_index, lock in enumerate(LOCKS):
        # NaN at missing thread counts so the line breaks at any gap (interior or
        # tail) instead of bridging a straight segment across an absent point.
        offset = (lock_index - (len(LOCKS) - 1) / 2) * CANDLE_SPACING
        positions = [j + offset for j in range(len(threads))]
        meds = []
        any_pt = False
        for j, t in enumerate(threads):
            arr = cell(raw, wl, t, lock)
            if arr is None:
                meds.append(np.nan)
                continue
            any_pt = True
            low, q1, med, q3, high = np.percentile(arr, [0, 25, 50, 75, 100])
            meds.append(float(med))
            ax.vlines(positions[j], low, high, color=COLOR[lock], lw=1.2, alpha=0.95, zorder=3)
            ax.add_patch(plt.Rectangle((positions[j] - CANDLE_WIDTH / 2, q1), CANDLE_WIDTH,
                                       max(q3 - q1, q1 * 0.002), facecolor=COLOR[lock],
                                       edgecolor=COLOR[lock], lw=0.8, alpha=0.7, zorder=4))
        if not any_pt:
            continue
        ax.plot(positions, meds, color=COLOR[lock], marker=MARKER[lock],
                ms=3.8, mec=PANEL_BG, mew=0.5, lw=1.4, zorder=5,
                label=NAME[lock] if legend else None)

    ax.set_yscale("log")
    ax.set_ylim(*ylim)
    ax.set_xticks(xpos)
    ax.set_xticklabels([str(t) for t in threads])
    ax.set_xlim(-0.5, len(threads) - 0.4)
    cs = f"CS = {wl} NOPs" + (f"  (≈ {wl * nop_ns:.0f} ns)" if nop_ns else "")
    ax.set_title(cs, loc="left", fontsize=10.5, fontweight="bold", color=INK, pad=6)
    ax.grid(True, axis="y", which="major", ls="-", lw=0.6, alpha=0.5)
    ax.grid(False, axis="x")
    ax.set_axisbelow(True)
    if legend:
        leg = ax.legend(loc="upper left", fontsize=8, framealpha=0.0,
                        labelcolor=INK, handlelength=1.4, borderpad=0.3)
        leg.set_zorder(7)


def speedup_matrix(raw, workloads, threads, numer, rng, nboot=2000):
    """Median of the per-launch ratios numer/ttas per (workload, thread), plus a
    two-sided paired bootstrap p-value against break-even (1.0)."""
    M = np.full((len(workloads), len(threads)), np.nan)
    p_values = np.full_like(M, np.nan)
    for i, wl in enumerate(workloads):
        for j, t in enumerate(threads):
            numerator_runs = raw.get((wl, t, numer)) or {}
            ttas_runs = raw.get((wl, t, "ttas")) or {}
            shared_runs = sorted(set(numerator_runs) & set(ttas_runs))
            if len(shared_runs) < 2:
                continue
            log_ratios = np.log2([numerator_runs[run] / ttas_runs[run] for run in shared_runs])
            M[i, j] = float(2.0 ** np.median(log_ratios))
            boot = np.median(rng.choice(log_ratios, (nboot, log_ratios.size)), axis=1)
            tail = min(float(np.mean(boot <= 0.0)), float(np.mean(boot >= 0.0)))
            p_values[i, j] = min(1.0, 2.0 * max(tail, 1.0 / nboot))
    return M, p_values


def benjamini_hochberg(p_values, false_discovery_rate=0.05):
    tested = np.isfinite(p_values)
    sorted_p = np.sort(p_values[tested])
    if sorted_p.size == 0:
        return np.zeros(p_values.shape, dtype=bool)
    thresholds = false_discovery_rate * np.arange(1, sorted_p.size + 1) / sorted_p.size
    passing = sorted_p <= thresholds
    cutoff = sorted_p[passing].max() if passing.any() else -1.0
    return tested & (p_values <= cutoff)


def heatmap_norm(*matrices):
    """One shared diverging norm across every heatmap, so a single colorbar is
    honest for all of them and colours are comparable panel-to-panel."""
    vals = []
    for M in matrices:
        vals.append(np.abs(np.log2(M[np.isfinite(M)])))
    stacked = np.concatenate(vals) if vals else np.array([])
    vmax = float(stacked.max()) if stacked.size else 1.0
    vmax = min(vmax, np.log2(16))     # clip so a few huge cells don't wash out the rest
    vmax = max(vmax, 1e-6)            # guard the all-break-even degenerate case
    return TwoSlopeNorm(vmin=-vmax, vcenter=0.0, vmax=vmax), vmax


def draw_heatmap(ax, M, sig, workloads, threads, title, norm, vmax):
    _panel_frame(ax)
    ax.set_facecolor(PANEL_BG)
    with np.errstate(invalid="ignore"):
        L = np.log2(M)
    disp = np.where(sig, L, np.nan)  # only significant cells get colour
    im = ax.imshow(disp, cmap=SPEEDUP_CMAP, norm=norm, aspect="auto")

    for i in range(len(workloads)):
        for j in range(len(threads)):
            r = M[i, j]
            if np.isnan(r):
                ax.add_patch(plt.Rectangle((j - 0.5, i - 0.5), 1, 1,
                             facecolor="#15181c", edgecolor=PANEL_BG, lw=1,
                             hatch="///", zorder=2))
                ax.text(j, i, "n/a", ha="center", va="center", fontsize=7.5,
                        color=INK_FAINT, zorder=3)
                continue
            if not sig[i, j]:
                ax.add_patch(plt.Rectangle((j - 0.5, i - 0.5), 1, 1,
                             facecolor=PANEL_EDGE, edgecolor=PANEL_BG, lw=1,
                             hatch="...", zorder=2))
            strong = sig[i, j] and abs(np.log2(r)) / (vmax + 1e-9) > 0.45
            col = "#ffffff" if strong else INK_MUTED
            ax.text(j, i, f"{r:.2f}x", ha="center", va="center", fontsize=8.0,
                    color=col, fontweight="bold", zorder=3)

    ax.set_xticks(range(len(threads)))
    ax.set_xticklabels([str(t) for t in threads])
    ax.set_yticks(range(len(workloads)))
    ax.set_yticklabels([str(w) for w in workloads])
    ax.set_xlabel("Threads", color=INK_MUTED)
    ax.set_ylabel("Critical-section (NOPs)", color=INK_MUTED)
    ax.set_title(title, loc="left", fontsize=10.5, fontweight="bold", color=INK,
                 pad=6)
    ax.tick_params(length=0)
    return im


def compute_headlines(raw, workloads, threads, mp, mm):
    """Numbers for the stat tiles: peak wins, break-even CS, absolute anchor."""
    def peak(M):
        if not np.isfinite(M).any():
            return 0.0, None, None
        idx = np.unravel_index(np.nanargmax(M), M.shape)
        return float(M[idx]), workloads[idx[0]], threads[idx[1]]

    peak_p, _, _ = peak(mp)
    peak_m, _, _ = peak(mm)
    # Break-even CS: smallest workload whose POSIX/TTAS ratio is within 5% of 1.0
    # at every thread count (advantage has effectively vanished from there up).
    breakeven = None
    for i, wl in enumerate(workloads):
        rowv = mp[i, :]
        if np.isfinite(rowv).all() and np.all(rowv < 1.05):
            breakeven = wl
            break
    base = cell(raw, workloads[0], threads[0], "ttas")
    base_ns = float(np.median(base)) if base is not None else 0.0
    return peak_p, peak_m, breakeven, base_ns


def render(csv_path=DEFAULT_CSV, png_path=DEFAULT_PNG, cpu_model=None,
           meta_path=DEFAULT_META):
    apply_theme()
    raw, workloads, threads = load_csv(csv_path)
    meta = load_meta(meta_path)
    cpu = cpu_model or meta.get("cpu_model", "unknown CPU")
    cores = int(meta.get("cores_pinned") or (max(threads) // 2 if threads else 1))
    pin_desc = meta.get("pin_desc", f"{cores} cores")
    nop_ns = meta.get("nop_ns")
    rng = np.random.default_rng(20240716)

    # Shared y-limits across all scaling panels (honest cross-panel comparison).
    lo, hi = np.inf, 0.0
    for wl in workloads:
        for t in threads:
            for lock in LOCKS:
                arr = cell(raw, wl, t, lock)
                if arr is not None and arr.size:
                    lo = min(lo, float(arr.min()))
                    hi = max(hi, float(arr.max()))
    ylim = (lo / 1.7, hi * 1.9)

    mp, pp = speedup_matrix(raw, workloads, threads, "pspin", rng)
    mm, pm = speedup_matrix(raw, workloads, threads, "mcs", rng)
    sp, sm = benjamini_hochberg(np.stack([pp, pm]))
    peak_p, peak_m, breakeven, base_ns = compute_headlines(
        raw, workloads, threads, mp, mm)

    n_wl = len(workloads)
    ncol = 2
    nrow_panels = (n_wl + ncol - 1) // ncol
    fig = plt.figure(figsize=(13.0, 3.1 + 2.55 * nrow_panels + 5.2))
    gs = fig.add_gridspec(
        1 + nrow_panels + 1, ncol,
        height_ratios=[0.62] + [1.0] * nrow_panels + [2.15],
        hspace=0.5, wspace=0.13,
        left=0.06, right=0.965, top=0.930, bottom=0.05)

    # --- Row 0: KPI stat tiles ------------------------------------------------
    tile_gs = gs[0, :].subgridspec(1, 5, wspace=0.14)
    tiles = [
        (f"{peak_p:.1f}", "x", "POSIX/TTAS elapsed ratio, peak", COLOR["ttas"]),
        (f"{peak_m:.1f}", "x", "MCS/TTAS elapsed ratio, peak", COLOR["ttas"]),
        (f"{breakeven}" if breakeven is not None else ">1024", "NOPs",
         "Break-even critical section", ACCENT),
        (f"{base_ns:.1f}", "ns", "TTAS cost, 1 thread", INK),
        (f"{cores}", "cores", "Pinned P-cores", INK_MUTED),
    ]
    renderer = fig.canvas.get_renderer()
    tile_labels = []
    for k, (val, unit, label, col) in enumerate(tiles):
        tile_ax = fig.add_subplot(tile_gs[0, k])
        tile_labels.append((tile_ax, draw_stat_tile(tile_ax, val, unit, label, col)))
    label_fontsize = min(
        shrink_to_fit(text, renderer,
                      max_width=tile_ax.get_window_extent(renderer).width * (1 - 2 * TILE_INSET))
        for tile_ax, text in tile_labels)
    for _, text in tile_labels:
        text.set_fontsize(label_fontsize)

    # --- Scaling panels -------------------------------------------------------
    for i, wl in enumerate(workloads):
        ax = fig.add_subplot(gs[1 + i // ncol, i % ncol])
        draw_scaling_panel(ax, raw, wl, threads, ylim, cores, legend=(i == 0), nop_ns=nop_ns)
        if i % ncol == 0:
            ax.set_ylabel("ns / lock-unlock  (log)", color=INK_MUTED)
        if i // ncol == nrow_panels - 1:
            ax.set_xlabel("Threads", color=INK_MUTED)

    # --- Heatmaps -------------------------------------------------------------
    axh1 = fig.add_subplot(gs[1 + nrow_panels, 0])
    axh2 = fig.add_subplot(gs[1 + nrow_panels, 1])
    hm_norm, hm_vmax = heatmap_norm(mp, mm)  # one norm → one honest shared colorbar
    im = draw_heatmap(axh1, mp, sp, workloads, threads,
                      "POSIX/TTAS elapsed-time ratio   (>1 = TTAS finished sooner)", hm_norm, hm_vmax)
    draw_heatmap(axh2, mm, sm, workloads, threads,
                 "MCS/TTAS elapsed-time ratio   (>1 = TTAS finished sooner)", hm_norm, hm_vmax)
    cb = fig.colorbar(im, ax=[axh1, axh2], fraction=0.03, pad=0.02)
    cb.set_label("log2(elapsed ratio) · 0 = break-even · grey = not significant (5% FDR)",
                 color=INK_MUTED, fontsize=8.5)
    shrink_to_fit(cb.ax.yaxis.label, renderer, max_height=cb.ax.get_window_extent(renderer).height)
    cb.ax.yaxis.set_tick_params(color=INK_MUTED)
    plt.setp(cb.ax.get_yticklabels(), color=INK_MUTED)
    cb.outline.set_edgecolor(PANEL_EDGE)

    title_text = fig.suptitle(
        f"Custom TTAS / TTAS Spin+Park / MCS vs POSIX Spinlock / Mutex   —   {cpu}",
        fontsize=17, fontweight="bold", color=INK, x=0.06, ha="left", y=0.983)
    subtitle_text = fig.text(
        0.06, 0.953,
        "lock/unlock latency amortised across all threads · candle = all runs (body = interquartile "
        "range, wick = min–max), line = median · shaded = oversubscribed (threads > pinned cores)",
        ha="left", fontsize=12, color=INK_MUTED)
    footer_text = fig.text(
        0.06, 0.016,
        f"Pinning: {pin_desc}.  Critical section = N filler NOPs held under "
        "the lock.  Ratio = median of per-launch paired elapsed-time ratios; cells not significant "
        "at 5% FDR (paired bootstrap, Benjamini-Hochberg) are greyed.  MCS is skipped where it "
        "convoys under oversubscription.",
        ha="left", fontsize=8, color=INK_FAINT, style="italic")
    content_width = (gs.right - gs.left) * fig.bbox.width
    for text in (title_text, subtitle_text, footer_text):
        shrink_to_fit(text, renderer, max_width=content_width)

    fig.savefig(png_path, dpi=150)
    plt.close(fig)
    return png_path


def main():
    ap = argparse.ArgumentParser(
        description="Render the spinlock benchmark dashboard from a CSV.")
    ap.add_argument("--csv", type=Path, default=DEFAULT_CSV)
    ap.add_argument("--out", type=Path, default=DEFAULT_PNG)
    ap.add_argument("--meta", type=Path, default=DEFAULT_META)
    ap.add_argument("--cpu-model", default=None,
                    help="Override the CPU label in the title.")
    cli = ap.parse_args()
    if not cli.csv.exists():
        raise SystemExit(f"Error: {cli.csv} not found; run the benchmark first.")
    render(cli.csv, cli.out, cli.cpu_model, cli.meta)
    print(f"[Done] Rendered {cli.out} from {cli.csv}")


if __name__ == "__main__":
    main()
