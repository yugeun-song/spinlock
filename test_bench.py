from __future__ import annotations

import argparse
import csv
import io
import json
import os
import platform
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

import numpy as np

REPEATS: int = 7
REPEATS_MAX: int = 14
NOISY_CELL_CV: float = 0.05
WARMUP_LAUNCHES: int = 3
LAUNCH_TIMEOUT_SEC: int = 120
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
DEFAULT_BENCHMARK_BIN: Path = Path("./bin") / f"{platform.machine()}-linux-gcc" / "release" / "spinlock_test"
CSV_PATH: Path = Path("bench_results.csv")
PNG_PATH: Path = Path("bench_result.png")
META_PATH: Path = Path("bench_meta.json")

LOCKS: list[str] = ["ttas", "ttas_park", "mcs", "pspin", "pmutex"]
CSV_FIELDS: list[str] = ["workload_nops", "threads", "lock", "iterations", "run_idx", "launch_index",
                         "run_position", "time_ms_normalized_to_1m_iters", "fairness_min_over_max",
                         "load_1min"]


def mad(arr: np.ndarray) -> float:
    med = np.median(arr)
    return float(np.median(np.abs(arr - med)) * 1.4826)


def robust_cv(values: np.ndarray) -> float:
    median = float(np.median(values))
    return mad(values) / median if median > 0 else 0.0


def read_load_average() -> float:
    try:
        return float(Path("/proc/loadavg").read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return float("nan")


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

    On heterogeneous machines (Intel P/E, aarch64 big.LITTLE) pinning to a single
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


def locks_for(threads: int, core_budget: int) -> list[str]:
    # Drop the MCS contender once threads oversubscribe the cores: a strict
    # FIFO queue lock convoys there (a preempted successor stalls the whole
    # queue), which would dominate wall-clock and starve the other samples.
    return [lock for lock in LOCKS if threads <= core_budget or lock != "mcs"]


def run_launch(benchmark_bin: Path, threads: int, workload: int, pin_cores: list[int],
               lock_order: list[str]) -> dict[str, dict] | None:
    iterations = pick_iterations(workload)
    command = [str(benchmark_bin), "-F", "csv", "-t", str(threads), "-l", str(workload),
               "-i", str(iterations), "-K", ",".join(lock_order)]
    if pin_cores:
        command += ["-C", ",".join(map(str, pin_cores))]
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=LAUNCH_TIMEOUT_SEC)
    except subprocess.TimeoutExpired:
        print(f"\nWarning: launch timed out (threads={threads}, workload={workload})", file=sys.stderr)
        return None
    rows = {row["lock"]: row for row in csv.DictReader(io.StringIO(completed.stdout))}
    if completed.returncode != 0 or set(rows) != set(lock_order) or \
            any(row["count_ok"] != "1" for row in rows.values()):
        print(f"\nWarning: rejected launch (threads={threads}, workload={workload}, "
              f"exit {completed.returncode}) {completed.stderr.strip()[:160]}", file=sys.stderr)
        return None
    scale = 1_000_000 / iterations
    measured: dict[str, dict] = {}
    for lock, row in rows.items():
        elapsed_ms = float(row["elapsed_ms"])
        measured[lock] = {
            "time_ms": elapsed_ms * scale,
            "run_position": int(row["run_position"]),
            "fairness": float(row["earliest_done_ms"]) / elapsed_ms if elapsed_ms > 0 else 1.0,
            "nop_ns": float(row["nop_ns"]),
            "timer_slack_ns": int(row["timer_slack_ns"]),
        }
    return measured


class SweepRunner:
    def __init__(self, benchmark_bin: Path, pin_cores: list[int], core_budget: int,
                 rng: np.random.Generator) -> None:
        self.benchmark_bin = benchmark_bin
        self.pin_cores = pin_cores
        self.core_budget = core_budget
        self.rng = rng
        self.launches_by_cell: dict[tuple[int, int], list[dict]] = defaultdict(list)
        self.launch_index = 0
        self.failed_launches = 0
        self.planned_launches = 0
        self.finished_launches = 0

    def launch(self, workload: int, threads: int) -> tuple[dict[str, dict] | None, float]:
        lock_order = [str(lock) for lock in self.rng.permutation(locks_for(threads, self.core_budget))]
        time.sleep(SETTLE_SEC)
        load_1min = read_load_average()
        return run_launch(self.benchmark_bin, threads, workload, self.pin_cores, lock_order), load_1min

    def warm_up(self, cells: list[tuple[int, int]]) -> None:
        for cell_index in self.rng.choice(len(cells), WARMUP_LAUNCHES):
            workload, threads = cells[int(cell_index)]
            self.launch(workload, threads)

    def run_schedule(self, schedule: list[tuple[int, int]]) -> list[tuple[int, int]]:
        self.planned_launches += len(schedule)
        failed_cells: list[tuple[int, int]] = []
        for workload, threads in schedule:
            measured, load_1min = self.launch(workload, threads)
            self.finished_launches += 1
            if measured is None:
                self.failed_launches += 1
                failed_cells.append((workload, threads))
            else:
                self.launches_by_cell[(workload, threads)].append(
                    {"launch_index": self.launch_index, "load_1min": load_1min, "locks": measured})
                self.launch_index += 1
            percent = self.finished_launches / self.planned_launches * 100
            sys.stdout.write(f"\rProgress: [{'=' * int(percent // 2):<50}] {percent:5.1f}% "
                             f"(launch {self.finished_launches}/{self.planned_launches}, "
                             f"{workload} NOPs, {threads} threads)   ")
            sys.stdout.flush()
        return failed_cells

    def lock_values(self, cell: tuple[int, int], lock: str) -> np.ndarray:
        return np.asarray([record["locks"][lock]["time_ms"] for record in self.launches_by_cell[cell]
                           if lock in record["locks"]], float)

    def cell_spread(self, cell: tuple[int, int]) -> float:
        spreads = [robust_cv(values) for lock in LOCKS
                   if (values := self.lock_values(cell, lock)).size >= 3]
        return max(spreads) if spreads else 0.0

    def shuffled(self, schedule: list[tuple[int, int]]) -> list[tuple[int, int]]:
        return [schedule[int(index)] for index in self.rng.permutation(len(schedule))]


def paired_median_ratio(runner: SweepRunner, cell: tuple[int, int], numerator: str) -> float:
    ratios = [record["locks"][numerator]["time_ms"] / record["locks"]["ttas"]["time_ms"]
              for record in runner.launches_by_cell[cell]
              if numerator in record["locks"] and "ttas" in record["locks"]]
    return float(np.median(ratios)) if ratios else 0.0


def print_report(cpu_model: str, num_cpus: int, l1_cache: str, runner: SweepRunner,
                 cells: list[tuple[int, int]], threads_range: list[int], duration: float,
                 pin_desc: str, seed: int) -> None:
    sep = "=" * 170
    line = "-" * 170
    records = [record for cell in cells for record in runner.launches_by_cell[cell]]
    nop_values = [values["nop_ns"] for record in records for values in record["locks"].values()]
    slack_values = [values["timer_slack_ns"] for record in records for values in record["locks"].values()]
    nop_ns = float(np.median(nop_values)) if nop_values else 0.0
    slack = int(np.median(slack_values)) if slack_values else -1
    loads = np.asarray([record["load_1min"] for record in records], float)
    total_raw_cycles = sum(pick_iterations(workload) * threads * len(record["locks"])
                           for workload, threads in cells for record in runner.launches_by_cell[(workload, threads)])

    print(f"\n{sep}")
    print("SYSTEM & PERFORMANCE REPORT: HYBRID SPINLOCK BENCHMARK")
    print(sep)
    print("HARDWARE SPECIFICATIONS:")
    print(f"  - CPU Model       : {cpu_model}")
    print(f"  - CPU Cores       : {num_cpus} Online / {os.cpu_count()} Logical")
    print(f"  - L1 Cache Line   : {l1_cache} bytes")
    print(f"  - Kernel          : {platform.release()}")
    print(f"  - NOP Cost        : {nop_ns:.4f} ns (measured by the harness)")
    print(f"  - Timer Slack     : {slack} ns")
    print(line)
    print("TEST PARAMETERS:")
    print(f"  - Aggregation     : Median ± MAD of {REPEATS}-{REPEATS_MAX} runs per cell "
          f"(more when robust CV > {NOISY_CELL_CV:.0%}), {WARMUP_LAUNCHES} warmup launches discarded")
    print(f"  - Schedule        : shuffled launches, shuffled lock order per launch, seed {seed}")
    print(f"  - Launches        : {runner.launch_index} kept, {runner.failed_launches} rejected")
    if loads.size and np.isfinite(loads).any():
        print(f"  - Load (1 min)    : median {np.nanmedian(loads):.2f}, max {np.nanmax(loads):.2f}")
    print("  - Normalization   : 1,000,000 Lock/Unlock cycles")
    print(f"  - Total Raw Ops   : {total_raw_cycles:,} cycles performed")
    print(f"  - Pinning         : {pin_desc}")
    print(f"  - Bench Duration  : {duration:.2f} seconds")
    print(sep)
    print(f"{'Workload (NOPs)':<16} | {'CS (ns)':<8} | {'Threads':<8} | {'Runs':<5} | "
          f"{'TTAS (ms)':<18} | {'TTAS park (ms)':<18} | {'MCS (ms)':<18} | "
          f"{'PSpin (ms)':<18} | {'PMutex (ms)':<18} | {'PSpin/TTAS':<10}")
    print(line)

    for workload, threads in cells:
        texts = []
        for lock in LOCKS:
            values = runner.lock_values((workload, threads), lock)
            texts.append(f"{np.median(values):.3f} ±{mad(values):.1f}" if values.size else "— (skipped)")
        runs = len(runner.launches_by_cell[(workload, threads)])
        ratio = paired_median_ratio(runner, (workload, threads), "pspin")
        print(f"{workload:<16} | {workload * nop_ns:<8.1f} | {threads:<8} | {runs:<5} | "
              + " | ".join(f"{text:<18}" for text in texts) + f" | {ratio:.2f}x")
        if threads == threads_range[-1]:
            print(line)


def write_csv(path: Path, runner: SweepRunner, cells: list[tuple[int, int]]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(CSV_FIELDS)
        for workload, threads in cells:
            records = runner.launches_by_cell[(workload, threads)]
            for lock in LOCKS:
                for run_idx, record in enumerate(records):
                    values = record["locks"].get(lock)
                    if values is None:
                        continue
                    w.writerow([workload, threads, lock, pick_iterations(workload), run_idx,
                                record["launch_index"], values["run_position"], f"{values['time_ms']:.6f}",
                                f"{values['fairness']:.4f}", f"{record['load_1min']:.2f}"])


def write_meta(path: Path, cpu_model: str, pin_cores: list[int], threads_range: list[int],
               num_cpus: int, runner: SweepRunner, cells: list[tuple[int, int]], seed: int) -> None:
    """Sidecar so the plot can disclose the run context (pinned core budget, CPU
    label, measured NOP cost) without re-deriving it from the CSV."""
    records = [record for cell in cells for record in runner.launches_by_cell[cell]]
    nop_values = [values["nop_ns"] for record in records for values in record["locks"].values()]
    slack_values = [values["timer_slack_ns"] for record in records for values in record["locks"].values()]
    loads = np.asarray([record["load_1min"] for record in records], float)
    meta = {
        "cpu_model": cpu_model,
        "cores_pinned": len(pin_cores) if pin_cores else num_cpus,
        "pin_desc": pin_desc_str(pin_cores),
        "threads_range": threads_range,
        "kernel": platform.release(),
        "nop_ns": float(np.median(nop_values)) if nop_values else None,
        "timer_slack_ns": int(np.median(slack_values)) if slack_values else None,
        "seed": seed,
        "repeats": REPEATS,
        "repeats_max": REPEATS_MAX,
        "noisy_cell_cv": NOISY_CELL_CV,
        "warmup_launches": WARMUP_LAUNCHES,
        "launches_kept": runner.launch_index,
        "launches_rejected": runner.failed_launches,
        "load_1min": {"median": float(np.nanmedian(loads)), "max": float(np.nanmax(loads))}
        if loads.size and np.isfinite(loads).any() else None,
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
    parser.add_argument(
        "--binary", type=Path, default=DEFAULT_BENCHMARK_BIN,
        help="Benchmark executable to run (default: %(default)s, the 'make' release "
             "build for this host; pass bin/<arch>-linux-clang/release/spinlock_test "
             "for a 'make CC=clang' build).")
    parser.add_argument(
        "--seed", type=int, default=None,
        help="Seed for the shuffled schedule and lock order (default: random, recorded in the meta file).")
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

    if not os.access(cli.binary, os.X_OK):
        print(f"Error: {cli.binary} not found or not executable. Run 'make' first.",
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
    seed = cli.seed if cli.seed is not None else int(np.random.SeedSequence().entropy % 2**32)
    runner = SweepRunner(cli.binary, pin_cores, core_budget, np.random.default_rng(seed))
    cells = [(workload, threads) for workload in WORKLOAD_RANGE for threads in threads_range]
    start_time = time.time()

    print(f"Executing Benchmarks on {cpu_model} (seed {seed})...")
    runner.warm_up(cells)
    retry_cells = runner.run_schedule(runner.shuffled([cell for cell in cells for _ in range(REPEATS)]))
    extra_cells = [cell for cell in cells if runner.cell_spread(cell) > NOISY_CELL_CV
                   for _ in range(REPEATS_MAX - len(runner.launches_by_cell[cell]))]
    runner.run_schedule(runner.shuffled(extra_cells + retry_cells))
    sys.stdout.write("\r" + " " * 140 + "\r")

    duration = time.time() - start_time
    print_report(cpu_model, num_cpus, l1_cache, runner, cells, threads_range, duration, pin_desc, seed)
    # Write the CSV/meta first, then render from them, so the committed data is
    # the single source of truth and the PNG always matches the tracked CSV.
    write_csv(CSV_PATH, runner, cells)
    write_meta(META_PATH, cpu_model, pin_cores, threads_range, num_cpus, runner, cells, seed)
    render_dashboard(cpu_model)

    print(f"[Done] Saved {PNG_PATH}, {CSV_PATH}, and {META_PATH}")


if __name__ == "__main__":
    main()
