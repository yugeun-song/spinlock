from __future__ import annotations

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
WORKLOAD_RANGE: list[int] = [0, 200, 2000, 10000]
BENCHMARK_BIN: Path = Path("./bin/spinlock_test")

RE_SPIN = re.compile(r"Custom Hybrid Spinlock \]\s+- Elapsed Time :\s+([\d.]+)")
RE_MUTEX = re.compile(r"POSIX Mutex\s+\]\s+- Elapsed Time :\s+([\d.]+)")

SPIN_COLOR = "#1f77b4"
MUTEX_COLOR = "#ff7f0e"
SPEEDUP_PALETTE = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd"]


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


def run_bench(threads: int, workload: int) -> tuple[list[float], list[float], int]:
    iterations = pick_iterations(workload)
    scale = 1_000_000 / iterations
    raw_spin: list[float] = []
    raw_mutex: list[float] = []

    for run_idx in range(REPEATS + WARMUP_RUNS):
        cmd = [
            str(BENCHMARK_BIN),
            "-t", str(threads),
            "-l", str(workload),
            "-i", str(iterations),
        ]
        try:
            res = subprocess.run(
                cmd,
                capture_output=True, text=True, check=True, timeout=120,
            ).stdout
        except subprocess.TimeoutExpired:
            print(f"\nWarning: benchmark timed out (threads={threads}, workload={workload})",
                  file=sys.stderr)
            continue
        except subprocess.CalledProcessError as e:
            print(f"\nWarning: benchmark failed (threads={threads}, workload={workload}): {e}",
                  file=sys.stderr)
            continue

        s_m = RE_SPIN.search(res)
        m_m = RE_MUTEX.search(res)
        if not (s_m and m_m):
            continue

        if run_idx < WARMUP_RUNS:
            continue

        raw_spin.append(float(s_m.group(1)) * scale)
        raw_mutex.append(float(m_m.group(1)) * scale)

    return raw_spin, raw_mutex, iterations


def print_report(cpu_model: str, num_cpus: int, l1_cache: str, report_data: list[dict],
                 threads_range: list[int], total_raw_cycles: int, duration: float) -> None:
    sep = "=" * 120
    line = "-" * 120

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
    print(f"  - Bench Duration  : {duration:.2f} seconds")
    print(sep)
    print(f"{'Workload (NOPs)':<20} | {'Threads':<8} | {'Iters':<10} | "
          f"{'Spin (ms)':<20} | {'Mutex (ms)':<20} | {'Speedup':<10}")
    print(line)

    for d in report_data:
        spin_str = f"{d['spin_med']:.3f} ±{d['spin_mad']:.1f}"
        mutex_str = f"{d['mutex_med']:.3f} ±{d['mutex_mad']:.1f}"
        print(f"{d['workload']:<20} | {d['t']:<8} | {d['iters']:<10} | "
              f"{spin_str:<20} | {mutex_str:<20} | {d['ratio']:.2f}x")
        if d['t'] == threads_range[-1]:
            print(line)


def _draw_candle(ax: plt.Axes, x: float, arr: np.ndarray, width: float,
                 color: str, *, label: str | None = None) -> None:
    """OHLC-style candle: body = IQR (Q1..Q3), wick = min..max, tick = median."""
    if arr.size == 0:
        return
    q1 = float(np.percentile(arr, 25))
    q3 = float(np.percentile(arr, 75))
    med = float(np.median(arr))
    lo = float(arr.min())
    hi = float(arr.max())

    ax.plot([x, x], [lo, hi], color="black", lw=0.9, zorder=2)
    body_h = max(q3 - q1, (hi - lo) * 1e-3, 1e-9)
    ax.add_patch(plt.Rectangle(
        (x - width / 2, q1), width, body_h,
        facecolor=color, edgecolor="black", lw=0.9, alpha=0.85,
        zorder=3, label=label,
    ))
    ax.plot([x - width / 2, x + width / 2], [med, med],
            color="black", lw=1.6, zorder=4)


def plot_results(results_spin_raw: list[list[list[float]]],
                 results_mutex_raw: list[list[list[float]]],
                 results_ratio: np.ndarray, threads_range: list[int],
                 cpu_model: str) -> None:
    n_wl = len(WORKLOAD_RANGE)
    n_th = len(threads_range)

    fig = plt.figure(figsize=(13, 3.6 * n_wl + 4))
    gs = fig.add_gridspec(n_wl + 1, 1, height_ratios=[1.0] * n_wl + [1.3])

    for i, wl in enumerate(WORKLOAD_RANGE):
        ax = fig.add_subplot(gs[i])
        for j, _t in enumerate(threads_range):
            spin_arr = np.asarray(results_spin_raw[i][j], dtype=float)
            mutex_arr = np.asarray(results_mutex_raw[i][j], dtype=float)
            spin_label = "Spinlock (body=IQR, wick=min-max, tick=median)" if (i == 0 and j == 0) else None
            mutex_label = "POSIX Mutex" if (i == 0 and j == 0) else None
            _draw_candle(ax, j - 0.20, spin_arr, 0.34, SPIN_COLOR, label=spin_label)
            _draw_candle(ax, j + 0.20, mutex_arr, 0.34, MUTEX_COLOR, label=mutex_label)

        ax.set_xticks(range(n_th))
        ax.set_xticklabels([str(t) for t in threads_range])
        ax.set_xlim(-0.6, n_th - 0.4)
        ax.set_ylabel("Time (ms)")
        ax.set_title(f"Latency distribution — Workload = {wl} NOPs", loc="left", fontsize=10)
        ax.grid(True, axis="y", ls="--", alpha=0.4)
        ax.margins(y=0.08)
        if i == 0:
            ax.legend(loc="upper left", fontsize="small")
        if i == n_wl - 1:
            ax.set_xlabel("Threads")

    ax = fig.add_subplot(gs[-1])
    for i, wl in enumerate(WORKLOAD_RANGE):
        c = SPEEDUP_PALETTE[i % len(SPEEDUP_PALETTE)]
        ax.plot(threads_range, results_ratio[i, :],
                label=f"{wl} NOPs", color=c, marker="s", lw=2)
    ax.axhline(y=1.0, color="red", ls="-", alpha=0.5, label="Baseline (1.0x)")
    ax.set_title(f"Speedup (Mutex / Spinlock) — {cpu_model}", loc="left", fontsize=10)
    ax.set_xlabel("Threads")
    ax.set_ylabel("Speedup ratio")
    ax.set_xticks(threads_range)
    ax.grid(True, ls="--", alpha=0.4)
    ax.legend(title="Workload (NOPs)", fontsize="small")

    fig.suptitle("Hybrid Spinlock vs POSIX Mutex — 7-run candlestick", fontsize=12, y=0.995)
    plt.tight_layout(rect=(0, 0, 1, 0.985))
    plt.savefig("bench_result.png", dpi=200)


def main() -> None:
    if not os.access(BENCHMARK_BIN, os.X_OK):
        print(f"Error: {BENCHMARK_BIN} not found or not executable. Run 'make' first.",
              file=sys.stderr)
        sys.exit(1)

    cpu_model, num_cpus, l1_cache = get_system_info()
    threads_range = build_threads_range(num_cpus)
    start_time = time.time()

    n_wl = len(WORKLOAD_RANGE)
    n_th = len(threads_range)

    results_spin_raw: list[list[list[float]]] = [[[] for _ in range(n_th)] for _ in range(n_wl)]
    results_mutex_raw: list[list[list[float]]] = [[[] for _ in range(n_th)] for _ in range(n_wl)]
    results_ratio = np.zeros((n_wl, n_th))

    total_steps = n_wl * n_th
    current_step = 0
    total_raw_cycles = 0
    report_data: list[dict] = []

    print(f"Executing Benchmarks on {cpu_model}...")

    for i, wl in enumerate(WORKLOAD_RANGE):
        for j, t in enumerate(threads_range):
            raw_spin, raw_mutex, iters = run_bench(t, wl)
            results_spin_raw[i][j] = raw_spin
            results_mutex_raw[i][j] = raw_mutex

            spin_arr = np.asarray(raw_spin) if raw_spin else np.zeros(1)
            mutex_arr = np.asarray(raw_mutex) if raw_mutex else np.zeros(1)
            spin_med = float(np.median(spin_arr))
            mutex_med = float(np.median(mutex_arr))
            spin_mad_v = mad(spin_arr) if raw_spin else 0.0
            mutex_mad_v = mad(mutex_arr) if raw_mutex else 0.0
            ratio = mutex_med / spin_med if spin_med > 0 else 0.0
            results_ratio[i, j] = ratio

            total_raw_cycles += iters * REPEATS * t
            report_data.append({
                "workload": wl, "t": t,
                "spin_med": spin_med, "mutex_med": mutex_med,
                "spin_mad": spin_mad_v, "mutex_mad": mutex_mad_v,
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
                 total_raw_cycles, duration)
    plot_results(results_spin_raw, results_mutex_raw, results_ratio,
                 threads_range, cpu_model)

    print("[Done] Report and plots saved as 'bench_result.png'")


if __name__ == "__main__":
    main()
