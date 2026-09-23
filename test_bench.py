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

import json

import numpy as np

REPEATS: int = 7
WARMUP_RUNS: int = 1
# Quiescent gap between consecutive benchmark process launches, so each run
# starts from a settled system (scheduler drained, CPU frequency relaxed,
# previous run's cache/turbo residue dissipated) instead of inheriting the
# state the prior run left behind.
SETTLE_SEC: float = 0.3
# Workload = filler NOPs executed while the lock is held, emulating the critical
# section. The harness measures the cost of this loop at startup and prints it
# as "NOP Cost" (about 0.11 ns/NOP on the reference host), so the powers-of-two
# range below spans roughly 0-115 ns there, densely sampling the regime where
# spinlocks are actually used: tiny shared-state updates (flag flip ~2 ns, a few
# struct fields ~4-16 ns) up to a medium CS where the locks converge. Anything
# heavier is the wrong tool for a spinlock, so it is intentionally not sampled.
WORKLOAD_RANGE: list[int] = [ 0, 16, 32, 64, 128, 256, 512, 1024 ]
BENCHMARK_BIN: Path = Path("./bin/spinlock_test")
CSV_PATH: Path = Path("bench_results.csv")
PNG_PATH: Path = Path("bench_result.png")
META_PATH: Path = Path("bench_meta.json")

# CSV/plot keys in the harness's -K spelling, with the block title each prints.
LOCKS: list[str] = ["ttas", "ttas_park", "mcs", "pspin", "pmutex"]
LOCK_TITLE: dict[str, str] = {
    "ttas": "Custom TTAS Spinlock",
    "ttas_park": "Custom TTAS Spin+Park",
    "mcs": "Custom MCS Spinlock",
    "pspin": "POSIX Spinlock",
    "pmutex": "POSIX Mutex",
}
RE_ELAPSED: dict[str, re.Pattern] = {
    lock: re.compile(re.escape(title) + r"\s+\]\s+- Elapsed Time :\s+([\d.]+)")
    for lock, title in LOCK_TITLE.items()
}
RE_NOP_NS = re.compile(r"NOP Cost\s+:\s+([\d.]+) ns")
RE_SLACK_NS = re.compile(r"Timer Slack\s+:\s+(\d+) ns")


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


def run_bench(threads: int, workload: int, pin_cores: list[int], run_mcs: bool,
              env: dict[str, list]) -> tuple[dict[str, list[float]], int]:
    iterations = pick_iterations(workload)
    scale = 1_000_000 / iterations
    locks = [lock for lock in LOCKS if run_mcs or lock != "mcs"]
    raw: dict[str, list[float]] = {lock: [] for lock in LOCKS}

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
        cmd += ["-K", ",".join(locks)]
        try:
            res = subprocess.run(
                cmd,
                capture_output=True, text=True, check=True, timeout=120,
            ).stdout
        except subprocess.TimeoutExpired as e:
            # Should not happen now MCS is skipped when it would convoy; keep any
            # output flushed before the timeout (MCS runs last) as a net.
            res = e.stdout or ""
            print(f"\nWarning: benchmark timed out (threads={threads}, workload={workload})",
                  file=sys.stderr)
        except subprocess.CalledProcessError as e:
            print(f"\nWarning: benchmark failed (threads={threads}, workload={workload}): {e}",
                  file=sys.stderr)
            continue

        found = {lock: RE_ELAPSED[lock].search(res) for lock in locks}
        if not all(found.values()):
            continue

        if run_idx < WARMUP_RUNS:
            continue

        for lock, m in found.items():
            raw[lock].append(float(m.group(1)) * scale)
        for key, rx in (("nop_ns", RE_NOP_NS), ("timer_slack_ns", RE_SLACK_NS)):
            m = rx.search(res)
            if m:
                env.setdefault(key, []).append(float(m.group(1)))

    return raw, iterations


def print_report(cpu_model: str, num_cpus: int, l1_cache: str, report_data: list[dict],
                 threads_range: list[int], total_raw_cycles: int, duration: float,
                 pin_desc: str, env: dict[str, list]) -> None:
    sep = "=" * 170
    line = "-" * 170
    nop_ns = float(np.median(env["nop_ns"])) if env.get("nop_ns") else 0.0
    slack = int(np.median(env["timer_slack_ns"])) if env.get("timer_slack_ns") else -1

    print(f"\n{sep}")
    print("SYSTEM & PERFORMANCE REPORT: HYBRID SPINLOCK BENCHMARK")
    print(sep)
    print(f"HARDWARE SPECIFICATIONS:")
    print(f"  - CPU Model       : {cpu_model}")
    print(f"  - CPU Cores       : {num_cpus} Online / {os.cpu_count()} Logical")
    print(f"  - L1 Cache Line   : {l1_cache} bytes")
    print(f"  - Kernel          : {platform.release()}")
    print(f"  - NOP Cost        : {nop_ns:.4f} ns (measured by the harness)")
    print(f"  - Timer Slack     : {slack} ns")
    print(line)
    print(f"TEST PARAMETERS:")
    print(f"  - Aggregation     : Median ± MAD of {REPEATS} runs (+ {WARMUP_RUNS} warmup discarded)")
    print(f"  - Normalization   : 1,000,000 Lock/Unlock cycles")
    print(f"  - Total Raw Ops   : {total_raw_cycles:,} cycles performed")
    print(f"  - Pinning         : {pin_desc}")
    print(f"  - Bench Duration  : {duration:.2f} seconds")
    print(sep)
    print(f"{'Workload (NOPs)':<16} | {'CS (ns)':<8} | {'Threads':<8} | {'Iters':<9} | "
          f"{'TTAS (ms)':<18} | {'TTAS park (ms)':<18} | {'MCS (ms)':<18} | "
          f"{'PSpin (ms)':<18} | {'PMutex (ms)':<18} | {'TTAS/PSpin':<10}")
    print(line)

    for d in report_data:
        cells = []
        for lock in LOCKS:
            med, mad_v = d["med"][lock], d["mad"][lock]
            cells.append(f"{med:.3f} ±{mad_v:.1f}" if med is not None else "— (skipped)")
        print(f"{d['workload']:<16} | {d['workload'] * nop_ns:<8.1f} | {d['t']:<8} | {d['iters']:<9} | "
              + " | ".join(f"{c:<18}" for c in cells) + f" | {d['ratio']:.2f}x")
        if d['t'] == threads_range[-1]:
            print(line)


def write_csv(path: Path, results_raw: list[list[dict[str, list[float]]]],
              results_iters: list[list[int]], threads_range: list[int]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["workload_nops", "threads", "lock", "iterations",
                    "run_idx", "time_ms_normalized_to_1m_iters"])
        for i, wl in enumerate(WORKLOAD_RANGE):
            for j, t in enumerate(threads_range):
                iters = results_iters[i][j]
                for lock in LOCKS:
                    for k, v in enumerate(results_raw[i][j][lock]):
                        w.writerow([wl, t, lock, iters, k, f"{v:.6f}"])


def write_meta(path: Path, cpu_model: str, pin_cores: list[int],
               threads_range: list[int], num_cpus: int, env: dict[str, list]) -> None:
    """Sidecar so the plot can disclose the run context (pinned core budget, CPU
    label, measured NOP cost) without re-deriving it from the CSV."""
    meta = {
        "cpu_model": cpu_model,
        "cores_pinned": len(pin_cores) if pin_cores else num_cpus,
        "pin_desc": pin_desc_str(pin_cores),
        "threads_range": threads_range,
        "kernel": platform.release(),
        "nop_ns": float(np.median(env["nop_ns"])) if env.get("nop_ns") else None,
        "timer_slack_ns": int(np.median(env["timer_slack_ns"])) if env.get("timer_slack_ns") else None,
    }
    path.write_text(json.dumps(meta, indent=2) + "\n")


def render_dashboard(cpu_model: str | None) -> bool:
    """Delegate visualisation to the standalone plot module. Imported lazily so a
    headless benchmark run still writes the CSV/meta even without matplotlib."""
    try:
        import plot
    except ImportError as exc:
        print(f"Warning: skipped plotting ({exc}); {CSV_PATH} still written. "
              f"Install matplotlib, then run 'python3 plot.py'.", file=sys.stderr)
        return False
    plot.render(CSV_PATH, PNG_PATH, cpu_model, META_PATH)
    return True


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark the custom spinlocks vs the POSIX spinlock and mutex; "
                    "writes the CSV/meta and renders the dashboard via plot.py.")
    parser.add_argument(
        "--plot-only", action="store_true",
        help=f"Skip benchmarking and redraw {PNG_PATH} from an existing "
             f"{CSV_PATH} by delegating to plot.py (the CSV is read, never "
             f"modified). Equivalent to running 'python3 plot.py'.")
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
        # cpu_model=None lets plot.py read bench_meta.json (or fall back).
        if render_dashboard(cli.cpu_model):
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

    results_raw: list[list[dict[str, list[float]]]] = [[{} for _ in range(n_th)] for _ in range(n_wl)]
    results_iters: list[list[int]] = [[0] * n_th for _ in range(n_wl)]
    env: dict[str, list] = {}

    total_steps = n_wl * n_th
    current_step = 0
    total_raw_cycles = 0
    report_data: list[dict] = []

    print(f"Executing Benchmarks on {cpu_model}...")

    for i, wl in enumerate(WORKLOAD_RANGE):
        for j, t in enumerate(threads_range):
            raw, iters = run_bench(t, wl, pin_cores, t <= core_budget, env)
            results_raw[i][j] = raw
            results_iters[i][j] = iters

            # None when a lock was skipped at this point (MCS under
            # oversubscription), so the report shows it as skipped rather than a
            # misleading 0.000 ms.
            med = {lock: (float(np.median(np.asarray(v))) if v else None) for lock, v in raw.items()}
            mad_v = {lock: (mad(np.asarray(v)) if v else None) for lock, v in raw.items()}
            ratio = (med["pspin"] / med["ttas"]) if med["ttas"] and med["pspin"] else 0.0

            total_raw_cycles += iters * REPEATS * t
            report_data.append({
                "workload": wl, "t": t, "med": med, "mad": mad_v,
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
                 total_raw_cycles, duration, pin_desc, env)
    # Write the CSV/meta first, then render from them, so the committed data is
    # the single source of truth and the PNG always matches the tracked CSV.
    write_csv(CSV_PATH, results_raw, results_iters, threads_range)
    write_meta(META_PATH, cpu_model, pin_cores, threads_range, num_cpus, env)
    render_dashboard(cpu_model)

    print(f"[Done] Saved {PNG_PATH}, {CSV_PATH}, and {META_PATH}")


if __name__ == "__main__":
    main()
