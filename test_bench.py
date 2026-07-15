from __future__ import annotations

import argparse
import csv
import os
import platform
import re
import subprocess
import sys
import time
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np


REPEATS: int = 7
WARMUP_RUNS: int = 1
# Quiescent gap between consecutive benchmark process launches, so each run
# starts from a settled system (scheduler drained, CPU frequency relaxed,
# previous run's cache/turbo residue dissipated) instead of inheriting the
# state the prior run left behind.
SETTLE_SEC: float = 0.3
# Workload = filler NOPs executed while the lock is held, emulating the critical
# section. At ~0.125 ns/NOP on this class of CPU the powers-of-two range below
# spans roughly 0-128 ns, densely sampling the regime where spinlocks are
# actually used: tiny shared-state updates (flag flip ~2 ns, a few struct fields
# ~4-16 ns) up to a medium CS (~128 ns) where the two locks converge. Anything
# heavier is the wrong tool for a spinlock, so it is intentionally not sampled.
WORKLOAD_RANGE: list[int] = [0, 16, 32, 64, 128, 256, 512, 1024]
BENCHMARK_BIN: Path = Path("./bin/spinlock_test")
CSV_PATH: Path = Path("bench_results.csv")
PNG_PATH: Path = Path("bench_result.png")

RE_TTAS = re.compile(r"Custom TTAS Spinlock\s+\]\s+- Elapsed Time :\s+([\d.]+)")
RE_MCS = re.compile(r"Custom MCS Spinlock\s+\]\s+- Elapsed Time :\s+([\d.]+)")
RE_PSPIN = re.compile(r"POSIX Spinlock\s+\]\s+- Elapsed Time :\s+([\d.]+)")

SPIN_COLOR = "#1f77b4"
PSPIN_COLOR = "#ff7f0e"
# One maximally distinct colour per workload line in the speedup panel. Chosen
# for guaranteed separability on a white background: every hue is strong and
# dark enough to read (the old palette repeated after five workloads, and
# tab10's blue/cyan pair was too close). These are the ColorBrewer "Set1" hues
# minus its white-invisible yellow, with a dark teal as the eighth.
SPEEDUP_PALETTE = [
    "#e41a1c",  # red
    "#377eb8",  # blue
    "#4daf4a",  # green
    "#984ea3",  # purple
    "#ff7f00",  # orange
    "#a65628",  # brown
    "#f781bf",  # pink
    "#008080",  # teal
]
# Distinct marker per line, reinforcing the colour so that where the high-NOP
# lines bunch up near 1.0x each is still individually identifiable.
SPEEDUP_MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]

# One fixed colour per lock for the latency candles. With three contenders the
# old "colour the winner, gray the loser" scheme no longer distinguishes the two
# non-winners, so each lock keeps a stable identity instead and the winner at a
# given thread count is marked with a thick black candle border.
TTAS_COLOR = "#d62728"   # red   — custom TTAS spinlock
MCS_COLOR = "#2ca02c"    # green — custom MCS spinlock
PSPIN_COLOR2 = "#1f77b4" # blue  — POSIX pthread_spin_lock
LOCK_ORDER = ["ttas", "mcs", "pspin"]
LOCK_COLOR = {"ttas": TTAS_COLOR, "mcs": MCS_COLOR, "pspin": PSPIN_COLOR2}
LOCK_NAME = {"ttas": "Custom TTAS", "mcs": "Custom MCS", "pspin": "POSIX Spinlock"}
LABEL_COLOR = {TTAS_COLOR: "#a01b1c", MCS_COLOR: "#1f7a1f", PSPIN_COLOR2: "#155b8a"}


def mad(arr: np.ndarray) -> float:
    med = np.median(arr)
    return float(np.median(np.abs(arr - med)) * 1.4826)


def get_cpu_model() -> str:
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if "model name" in line:
                    return line.split(":", 1)[1].strip()
    except FileNotFoundError:
        pass
    return platform.processor()


def get_system_info() -> tuple[str, int, str]:
    cpu_model = get_cpu_model()
    try:
        num_cpus = os.sysconf("_SC_NPROCESSORS_ONLN")
    except (ValueError, OSError):
        num_cpus = os.cpu_count() or 4
    try:
        l1_cache = subprocess.run(
            ["getconf", "LEVEL1_DCACHE_LINESIZE"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        l1_cache = "64"
    return cpu_model, num_cpus, l1_cache


def detect_perf_cores() -> list[int]:
    """Return the CPUs in the highest-max-frequency group (the P-cores).

    On heterogeneous machines (Intel P/E, ARM big.LITTLE) pinning to a single
    homogeneous, fastest class is the largest single lever on run-to-run
    variance: it stops the scheduler from scattering workers across fast and
    slow cores. Returns [] when the machine is homogeneous or cpufreq is
    unreadable (VM/container), in which case the caller leaves pinning off.
    """
    base = Path("/sys/devices/system/cpu")
    groups: dict[int, list[int]] = {}
    for d in sorted(base.glob("cpu[0-9]*")):
        try:
            khz = int((d / "cpufreq/cpuinfo_max_freq").read_text())
        except (OSError, ValueError):
            return []
        groups.setdefault(khz, []).append(int(d.name[3:]))
    if len(groups) < 2:
        return []
    return sorted(groups[max(groups)])


def pin_desc_str(pin_cores: list[int]) -> str:
    return ("cpus " + ",".join(map(str, pin_cores))) if pin_cores else "none (unpinned)"


def build_threads_range(num_cpus: int) -> list[int]:
    threads: set[int] = set()
    curr = num_cpus * 2
    while curr >= 1:
        threads.add(curr)
        curr //= 2
    return sorted(threads)


def pick_iterations(workload: int) -> int:
    if workload < 500:
        return 1_000_000
    if workload < 5000:
        return 400_000
    return 100_000


def run_bench(threads: int, workload: int, pin_cores: list[int], run_mcs: bool
              ) -> tuple[list[float], list[float], list[float], int]:
    iterations = pick_iterations(workload)
    scale = 1_000_000 / iterations
    raw_ttas: list[float] = []
    raw_mcs: list[float] = []
    raw_pspin: list[float] = []

    for run_idx in range(REPEATS + WARMUP_RUNS):
        # Let the machine settle between launches so one run cannot perturb the
        # next; skip the wait before the very first launch.
        if run_idx > 0:
            time.sleep(SETTLE_SEC)
        cmd = [
            str(BENCHMARK_BIN),
            "-t", str(threads),
            "-l", str(workload),
            "-i", str(iterations),
        ]
        if pin_cores:
            cmd += ["-C", ",".join(map(str, pin_cores))]
        # Drop the MCS contender once threads oversubscribe the cores: a strict
        # FIFO queue lock convoys there (a preempted successor stalls the whole
        # queue), which would dominate wall-clock and starve the other samples.
        cmd += ["-K", "ttas,mcs,pspin" if run_mcs else "ttas,pspin"]
        try:
            res = subprocess.run(
                cmd,
                capture_output=True, text=True, check=True, timeout=120,
            ).stdout
        except subprocess.TimeoutExpired as e:
            # Should not happen now MCS is skipped when it would convoy; keep any
            # output flushed before the timeout (TTAS/POSIX run first) as a net.
            res = e.stdout or ""
            print(f"\nWarning: benchmark timed out (threads={threads}, workload={workload})",
                  file=sys.stderr)
        except subprocess.CalledProcessError as e:
            print(f"\nWarning: benchmark failed (threads={threads}, workload={workload}): {e}",
                  file=sys.stderr)
            continue

        t_m = RE_TTAS.search(res)
        p_m = RE_PSPIN.search(res)
        if not (t_m and p_m):
            continue

        if run_idx < WARMUP_RUNS:
            continue

        raw_ttas.append(float(t_m.group(1)) * scale)
        raw_pspin.append(float(p_m.group(1)) * scale)
        m_m = RE_MCS.search(res)
        if run_mcs and m_m:
            raw_mcs.append(float(m_m.group(1)) * scale)

    return raw_ttas, raw_mcs, raw_pspin, iterations


def print_report(cpu_model: str, num_cpus: int, l1_cache: str, report_data: list[dict],
                 threads_range: list[int], total_raw_cycles: int, duration: float,
                 pin_desc: str) -> None:
    sep = "=" * 132
    line = "-" * 132

    print(f"\n{sep}")
    print("SYSTEM & PERFORMANCE REPORT: HYBRID SPINLOCK BENCHMARK")
    print(sep)
    print(f"HARDWARE SPECIFICATIONS:")
    print(f"  - CPU Model       : {cpu_model}")
    print(f"  - CPU Cores       : {num_cpus} Online / {os.cpu_count()} Logical")
    print(f"  - L1 Cache Line   : {l1_cache} bytes")
    print(line)
    print(f"TEST PARAMETERS:")
    print(f"  - Aggregation     : Median ± MAD of {REPEATS} runs (+ {WARMUP_RUNS} warmup discarded)")
    print(f"  - Normalization   : 1,000,000 Lock/Unlock cycles")
    print(f"  - Total Raw Ops   : {total_raw_cycles:,} cycles performed")
    print(f"  - Pinning         : {pin_desc}")
    print(f"  - Bench Duration  : {duration:.2f} seconds")
    print(sep)
    print(f"{'Workload (NOPs)':<16} | {'Threads':<8} | {'Iters':<9} | "
          f"{'TTAS (ms)':<18} | {'MCS (ms)':<18} | {'PSpin (ms)':<18} | {'TTAS/PSpin':<10}")
    print(line)

    for d in report_data:
        ttas_str = f"{d['spin_med']:.3f} ±{d['spin_mad']:.1f}"
        mcs_str = (f"{d['mcs_med']:.3f} ±{d['mcs_mad']:.1f}"
                   if d['mcs_med'] is not None else "— (skipped)")
        pspin_str = f"{d['pspin_med']:.3f} ±{d['pspin_mad']:.1f}"
        print(f"{d['workload']:<16} | {d['t']:<8} | {d['iters']:<9} | "
              f"{ttas_str:<18} | {mcs_str:<18} | {pspin_str:<18} | {d['ratio']:.2f}x")
        if d['t'] == threads_range[-1]:
            print(line)


def _draw_candle(ax: plt.Axes, x: float, arr: np.ndarray, width: float,
                 color: str, *, label: str | None = None,
                 median_label: bool = False, label_color: str | None = None,
                 winner: bool = False) -> None:
    """OHLC-style candle: body = IQR (Q1..Q3), wick = min..max, tick = median.

    The winner (lowest median at this thread count) gets a thick black border so
    it reads at a glance among the three fixed-colour contenders.
    """
    if arr.size == 0:
        return
    q1 = float(np.percentile(arr, 25))
    q3 = float(np.percentile(arr, 75))
    med = float(np.median(arr))
    lo = float(arr.min())
    hi = float(arr.max())

    # Wick: the min..max noise envelope.
    ax.plot([x, x], [lo, hi], color="#1a1a1a", lw=1.4, zorder=4,
            solid_capstyle="round")
    # Body: the inter-quartile range, drawn opaque with a crisp dark border so
    # adjacent candles never blur together.
    body_h = max(q3 - q1, med * 2e-3, 1e-9)
    ax.add_patch(plt.Rectangle(
        (x - width / 2, q1), width, body_h,
        facecolor=color, edgecolor="#000000" if winner else "#111111",
        lw=2.8 if winner else 1.5, alpha=1.0,
        zorder=5.5 if winner else 5, label=label,
    ))
    # Median tick: a white halo under a black core so the headline number stays
    # legible on top of the coloured body.
    ax.plot([x - width / 2, x + width / 2], [med, med],
            color="white", lw=3.2, zorder=6, solid_capstyle="butt")
    ax.plot([x - width / 2, x + width / 2], [med, med],
            color="black", lw=1.4, zorder=7, solid_capstyle="butt")
    if median_label:
        txt = f"{med:.0f}" if med >= 100 else f"{med:.1f}"
        ax.annotate(txt, xy=(x, hi), xytext=(0, 3), textcoords="offset points",
                    ha="center", va="bottom", fontsize=6.5,
                    color=label_color or color, fontweight="bold", zorder=8)


def plot_results(results_ttas_raw: list[list[list[float]]],
                 results_mcs_raw: list[list[list[float]]],
                 results_pspin_raw: list[list[list[float]]],
                 results_ratio: np.ndarray, threads_range: list[int],
                 cpu_model: str, pin_desc: str) -> None:
    n_wl = len(WORKLOAD_RANGE)
    n_th = len(threads_range)
    ncols = 2 if n_wl > 1 else 1
    nrows = (n_wl + ncols - 1) // ncols

    fig = plt.figure(figsize=(7.4 * ncols, 4.2 * nrows + 4.0))
    gs = fig.add_gridspec(nrows + 1, ncols,
                          height_ratios=[1.0] * nrows + [0.95],
                          hspace=0.34, wspace=0.16)

    for i, wl in enumerate(WORKLOAD_RANGE):
        ax = fig.add_subplot(gs[i // ncols, i % ncols])
        ax.set_axisbelow(True)
        # Give every thread count its own lane: an alternating background band
        # plus a separator line so neighbouring candle pairs are unmistakably
        # distinct groups.
        for j in range(n_th):
            if j % 2 == 1:
                ax.axvspan(j - 0.5, j + 0.5, color="#4c4c4c", alpha=0.06, zorder=0)
            if j < n_th - 1:
                ax.axvline(j + 0.5, color="#d0d0d0", lw=0.9, zorder=1)
        panel_hi, panel_lo = 0.0, float("inf")
        raw_by_lock = {"ttas": results_ttas_raw, "mcs": results_mcs_raw,
                       "pspin": results_pspin_raw}
        cand_off = {"ttas": -0.26, "mcs": 0.0, "pspin": 0.26}
        for j, _t in enumerate(threads_range):
            arrs = {k: np.asarray(raw_by_lock[k][i][j], dtype=float) for k in LOCK_ORDER}
            for arr in arrs.values():
                if arr.size:
                    panel_hi = max(panel_hi, float(arr.max()))
                    panel_lo = min(panel_lo, float(arr.min()))
            # Winner at this thread count = lowest median latency; it gets the
            # thick border. Each lock keeps its fixed colour so the three stay
            # individually identifiable.
            meds = {k: (float(np.median(a)) if a.size else float("inf"))
                    for k, a in arrs.items()}
            winner = min(meds, key=meds.get)
            for k in LOCK_ORDER:
                _draw_candle(ax, j + cand_off[k], arrs[k], 0.24, LOCK_COLOR[k],
                             median_label=True, label_color=LABEL_COLOR[LOCK_COLOR[k]],
                             winner=(k == winner and meds[k] != float("inf")))

        ax.set_xticks(range(n_th))
        ax.set_xticklabels([str(t) for t in threads_range])
        ax.set_xlim(-0.6, n_th - 0.4)
        ax.set_yscale("log")
        if panel_hi > 0.0 and panel_lo < float("inf"):
            # Headroom on the log axis so the tallest candle and its median
            # label clear the top frame instead of colliding with it.
            ax.set_ylim(panel_lo / 1.7, panel_hi * 2.6)
        ax.set_ylabel("Time (ms, log)")
        ax.set_xlabel("Threads")
        ax.set_title(f"Critical-section work = {wl} NOPs", loc="left",
                     fontsize=11, fontweight="bold")
        ax.grid(True, axis="y", which="major", ls="--", alpha=0.45)
        ax.grid(True, axis="y", which="minor", ls=":", alpha=0.2)
        if i == 0:
            legend_handles = [
                plt.Rectangle((0, 0), 1, 1, facecolor=LOCK_COLOR[k],
                              edgecolor="#111111", label=LOCK_NAME[k])
                for k in LOCK_ORDER
            ]
            ax.legend(handles=legend_handles, loc="upper left", fontsize=8.5,
                      framealpha=0.95,
                      title="bars per group: TTAS · MCS · POSIX (left to right)\n"
                            "body=IQR · wick=min–max · tick=median · bold border=winner",
                      title_fontsize=7.5)

    ax = fig.add_subplot(gs[nrows, :])
    xpos = list(range(n_th))
    for i, wl in enumerate(WORKLOAD_RANGE):
        c = SPEEDUP_PALETTE[i % len(SPEEDUP_PALETTE)]
        m = SPEEDUP_MARKERS[i % len(SPEEDUP_MARKERS)]
        ax.plot(xpos, results_ratio[i, :], label=f"{wl} NOPs",
                color=c, marker=m, lw=2.2, ms=7.5,
                markeredgecolor="white", markeredgewidth=0.7)
    ax.axhline(y=1.0, color="#333333", ls="--", alpha=0.85, lw=1.6,
               label="break-even (1.0x)")
    ax.set_xticks(xpos)
    ax.set_xticklabels([str(t) for t in threads_range])
    ax.set_xlim(-0.3, n_th - 0.7)
    ax.set_title("Speedup (POSIX Spinlock / Custom TTAS) — above 1.0, the custom TTAS lock wins",
                 loc="left", fontsize=11, fontweight="bold")
    ax.set_xlabel("Threads")
    ax.set_ylabel("Speedup (x)")
    ax.grid(True, ls="--", alpha=0.4)
    ax.legend(title="Critical-section work (NOPs)", fontsize=9, ncol=3,
              loc="upper right")

    fig.suptitle(f"Custom TTAS vs Custom MCS vs POSIX Spinlock — {cpu_model}",
                 fontsize=14, fontweight="bold", y=0.998)
    fig.text(0.5, 0.008,
             "Critical-section work is emulated with N filler NOP instructions executed while the "
             "lock is held (NOP = no-op: burns cycles, does no real work).   "
             f"Pinning: {pin_desc}.",
             ha="center", fontsize=9, color="#555555", style="italic")
    fig.savefig(PNG_PATH, dpi=170, bbox_inches="tight")
    plt.close(fig)


def write_csv(path: Path, results_ttas_raw: list[list[list[float]]],
              results_mcs_raw: list[list[list[float]]],
              results_pspin_raw: list[list[list[float]]],
              results_iters: list[list[int]],
              threads_range: list[int]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["workload_nops", "threads", "lock", "iterations",
                    "run_idx", "time_ms_normalized_to_1m_iters"])
        for i, wl in enumerate(WORKLOAD_RANGE):
            for j, t in enumerate(threads_range):
                iters = results_iters[i][j]
                for k, v in enumerate(results_ttas_raw[i][j]):
                    w.writerow([wl, t, "ttas", iters, k, f"{v:.6f}"])
                for k, v in enumerate(results_mcs_raw[i][j]):
                    w.writerow([wl, t, "mcs", iters, k, f"{v:.6f}"])
                for k, v in enumerate(results_pspin_raw[i][j]):
                    w.writerow([wl, t, "pspin", iters, k, f"{v:.6f}"])


def load_results_from_csv(
    path: Path,
) -> tuple[list[list[list[float]]], list[list[list[float]]], list[list[list[float]]],
           np.ndarray, list[int]]:
    """Rebuild the nested raw-result structure (and median speedups) from a CSV.

    Lets the plot be regenerated from a committed measurement set without
    re-running the benchmark, so the tracked CSV stays the source of truth.
    """
    ttas: dict[tuple[int, int], list[float]] = {}
    mcs: dict[tuple[int, int], list[float]] = {}
    pspin: dict[tuple[int, int], list[float]] = {}
    threads_set: set[int] = set()
    unknown_locks: set[str] = set()

    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            lock = row["lock"]
            # Only the three locks this benchmark produces are accepted; "spin"
            # is honoured as the legacy label for the TTAS lock so older CSVs
            # still plot. Anything else (e.g. a stale "mutex" row) is skipped
            # rather than silently mislabeled.
            if lock in ("ttas", "spin"):
                bucket = ttas
            elif lock == "mcs":
                bucket = mcs
            elif lock == "pspin":
                bucket = pspin
            else:
                unknown_locks.add(lock)
                continue
            wl = int(row["workload_nops"])
            t = int(row["threads"])
            val = float(row["time_ms_normalized_to_1m_iters"])
            threads_set.add(t)
            bucket.setdefault((wl, t), []).append(val)

    if unknown_locks:
        print(f"Warning: {path} contains unrecognized lock label(s) "
              f"{sorted(unknown_locks)}; those rows were skipped. "
              f"Re-run a full sweep to regenerate it.", file=sys.stderr)

    threads_range = sorted(threads_set)
    n_wl, n_th = len(WORKLOAD_RANGE), len(threads_range)

    results_ttas_raw = [[ttas.get((wl, t), []) for t in threads_range]
                        for wl in WORKLOAD_RANGE]
    results_mcs_raw = [[mcs.get((wl, t), []) for t in threads_range]
                       for wl in WORKLOAD_RANGE]
    results_pspin_raw = [[pspin.get((wl, t), []) for t in threads_range]
                         for wl in WORKLOAD_RANGE]
    results_ratio = np.zeros((n_wl, n_th))
    for i in range(n_wl):
        for j in range(n_th):
            s, p = results_ttas_raw[i][j], results_pspin_raw[i][j]
            ttas_med = float(np.median(s)) if s else 0.0
            pspin_med = float(np.median(p)) if p else 0.0
            results_ratio[i, j] = (pspin_med / ttas_med) if ttas_med > 0 else 0.0

    return results_ttas_raw, results_mcs_raw, results_pspin_raw, results_ratio, threads_range


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark the custom hybrid spinlock vs pthread_spin_lock and plot it.")
    parser.add_argument(
        "--plot-only", action="store_true",
        help=f"Skip benchmarking and redraw {PNG_PATH} from an existing "
             f"{CSV_PATH} (the CSV is read, never modified).")
    parser.add_argument(
        "--cpu-model", default=None,
        help="Override the CPU label in the plot title; useful with --plot-only "
             "when re-rendering data captured on another machine.")
    parser.add_argument(
        "--no-pin", action="store_true",
        help="Disable automatic P-core pinning (by default the sweep pins to the "
             "highest-frequency core group on heterogeneous P/E or big.LITTLE machines).")
    cli = parser.parse_args()

    if cli.plot_only:
        if not CSV_PATH.exists():
            print(f"Error: {CSV_PATH} not found; run a full sweep first.",
                  file=sys.stderr)
            sys.exit(1)
        cpu_model = cli.cpu_model or get_cpu_model()
        rt, rm, rp, rr, threads_range = load_results_from_csv(CSV_PATH)
        plot_results(rt, rm, rp, rr, threads_range, cpu_model, "from CSV")
        print(f"[Done] Re-plotted {PNG_PATH} from {CSV_PATH} "
              f"(no benchmarks run, CSV untouched)")
        return

    if not os.access(BENCHMARK_BIN, os.X_OK):
        print(f"Error: {BENCHMARK_BIN} not found or not executable. Run 'make' first.",
              file=sys.stderr)
        sys.exit(1)

    cpu_model, num_cpus, l1_cache = get_system_info()
    if cli.cpu_model:
        cpu_model = cli.cpu_model
    # Pin to the fastest homogeneous core group to control variance (the biggest
    # lever on a P/E or big.LITTLE machine). When pinned, size the thread sweep
    # to the pinned set so the oversubscription axis is meaningful relative to it.
    pin_cores = [] if cli.no_pin else detect_perf_cores()
    pin_desc = pin_desc_str(pin_cores)
    core_budget = len(pin_cores) if pin_cores else num_cpus
    threads_range = build_threads_range(core_budget)
    start_time = time.time()

    n_wl = len(WORKLOAD_RANGE)
    n_th = len(threads_range)

    results_ttas_raw: list[list[list[float]]] = [[[] for _ in range(n_th)] for _ in range(n_wl)]
    results_mcs_raw: list[list[list[float]]] = [[[] for _ in range(n_th)] for _ in range(n_wl)]
    results_pspin_raw: list[list[list[float]]] = [[[] for _ in range(n_th)] for _ in range(n_wl)]
    results_iters: list[list[int]] = [[0] * n_th for _ in range(n_wl)]
    results_ratio = np.zeros((n_wl, n_th))

    total_steps = n_wl * n_th
    current_step = 0
    total_raw_cycles = 0
    report_data: list[dict] = []

    print(f"Executing Benchmarks on {cpu_model}...")

    for i, wl in enumerate(WORKLOAD_RANGE):
        for j, t in enumerate(threads_range):
            raw_ttas, raw_mcs, raw_pspin, iters = run_bench(t, wl, pin_cores, t <= core_budget)
            results_ttas_raw[i][j] = raw_ttas
            results_mcs_raw[i][j] = raw_mcs
            results_pspin_raw[i][j] = raw_pspin
            results_iters[i][j] = iters

            ttas_arr = np.asarray(raw_ttas) if raw_ttas else np.zeros(1)
            pspin_arr = np.asarray(raw_pspin) if raw_pspin else np.zeros(1)
            ttas_med = float(np.median(ttas_arr))
            pspin_med = float(np.median(pspin_arr))
            ttas_mad_v = mad(ttas_arr) if raw_ttas else 0.0
            pspin_mad_v = mad(pspin_arr) if raw_pspin else 0.0
            # None when MCS was skipped at this point (oversubscription), so the
            # report shows it as skipped rather than a misleading 0.000 ms.
            mcs_med = float(np.median(np.asarray(raw_mcs))) if raw_mcs else None
            mcs_mad_v = mad(np.asarray(raw_mcs)) if raw_mcs else None
            ratio = pspin_med / ttas_med if ttas_med > 0 else 0.0
            results_ratio[i, j] = ratio

            total_raw_cycles += iters * REPEATS * t
            report_data.append({
                "workload": wl, "t": t,
                "spin_med": ttas_med, "mcs_med": mcs_med, "pspin_med": pspin_med,
                "spin_mad": ttas_mad_v, "mcs_mad": mcs_mad_v, "pspin_mad": pspin_mad_v,
                "ratio": ratio, "iters": iters,
            })

            current_step += 1
            pct = (current_step / total_steps) * 100
            bar = "=" * int(pct // 2)
            sys.stdout.write(f"\rProgress: [{bar:<50}] {pct:.1f}% ({wl} NOPs, {t} Threads)")
            sys.stdout.flush()

    sys.stdout.write("\r" + " " * 120 + "\r")

    duration = time.time() - start_time
    print_report(cpu_model, num_cpus, l1_cache, report_data, threads_range,
                 total_raw_cycles, duration, pin_desc)
    plot_results(results_ttas_raw, results_mcs_raw, results_pspin_raw, results_ratio,
                 threads_range, cpu_model, pin_desc)
    write_csv(CSV_PATH, results_ttas_raw, results_mcs_raw, results_pspin_raw,
              results_iters, threads_range)

    print(f"[Done] Saved {PNG_PATH} and {CSV_PATH}")


if __name__ == "__main__":
    main()
