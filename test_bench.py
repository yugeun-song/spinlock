import matplotlib
matplotlib.use('Agg')

import subprocess
import re
import os
import sys
import numpy as np
import matplotlib.pyplot as plt
import platform
import time

REPEATS = 5
WORKLOAD_RANGE = [0, 500, 2000, 5000]
BENCHMARK_BIN = "./bin/spinlock_test"


def mad(arr):
    med = np.median(arr)
    return np.median(np.abs(arr - med)) * 1.4826


def get_cpu_model():
    try:
        with open("/proc/cpuinfo", "r") as f:
            for line in f:
                if "model name" in line:
                    return line.split(":")[1].strip()
    except FileNotFoundError:
        pass
    return platform.processor()


def get_system_info():
    cpu_model = get_cpu_model()
    try:
        num_cpus = os.sysconf('_SC_NPROCESSORS_ONLN')
    except (ValueError, OSError):
        num_cpus = os.cpu_count() or 4
    try:
        l1_cache = subprocess.check_output(
            ["getconf", "LEVEL1_DCACHE_LINESIZE"], stderr=subprocess.DEVNULL
        ).decode().strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        l1_cache = "64"
    return cpu_model, num_cpus, l1_cache


def build_threads_range(num_cpus):
    threads = set()
    curr = num_cpus * 2
    while curr >= 1:
        threads.add(curr)
        curr //= 2
    return sorted(threads)


def run_bench(threads, workload):
    raw_spin = []
    raw_mutex = []
    iterations = 1000000 if workload < 1000 else 400000
    scale = 1000000 / iterations

    for _ in range(REPEATS):
        cmd = [BENCHMARK_BIN, "-t", str(threads), "-l", str(workload), "-i", str(iterations)]
        try:
            res = subprocess.check_output(cmd, stderr=subprocess.DEVNULL, timeout=120).decode()
        except subprocess.TimeoutExpired:
            print(f"\nWarning: benchmark timed out (threads={threads}, workload={workload})",
                  file=sys.stderr)
            continue
        except subprocess.CalledProcessError as e:
            print(f"\nWarning: benchmark failed (threads={threads}, workload={workload}): {e}",
                  file=sys.stderr)
            continue

        s_m = re.search(r"Custom Hybrid Spinlock \]\s+- Elapsed Time :\s+([\d.]+)", res)
        m_m = re.search(r"POSIX Mutex\s+\]\s+- Elapsed Time :\s+([\d.]+)", res)

        if s_m and m_m:
            raw_spin.append(float(s_m.group(1)) * scale)
            raw_mutex.append(float(m_m.group(1)) * scale)

    if not raw_spin or not raw_mutex:
        return 0.0, 0.0, 0.0, 0.0, iterations

    return (np.median(raw_spin), np.median(raw_mutex),
            mad(raw_spin), mad(raw_mutex), iterations)


def print_report(cpu_model, num_cpus, l1_cache, report_data, threads_range,
                 total_raw_cycles, duration):
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
    print(f"  - Aggregation     : Median \u00b1 MAD of {REPEATS} runs")
    print(f"  - Normalization   : 1,000,000 Lock/Unlock cycles")
    print(f"  - Total Raw Ops   : {total_raw_cycles:,} cycles performed")
    print(f"  - Bench Duration  : {duration:.2f} seconds")
    print(sep)
    print(f"{'Workload (NOPs)':<20} | {'Threads':<8} | {'Iters':<10} | "
          f"{'Spin (ms)':<20} | {'Mutex (ms)':<20} | {'Speedup':<10}")
    print(line)

    for d in report_data:
        spin_str = f"{d['spin_med']:.3f} \u00b1{d['spin_mad']:.1f}"
        mutex_str = f"{d['mutex_med']:.3f} \u00b1{d['mutex_mad']:.1f}"
        print(f"{d['workload']:<20} | {d['t']:<8} | {d['iters']:<10} | "
              f"{spin_str:<20} | {mutex_str:<20} | {d['ratio']:.2f}x")
        if d['t'] == threads_range[-1]:
            print(line)


def plot_results(results_spin, results_mutex, results_spin_mad, results_mutex_mad,
                 results_ratio, threads_range, cpu_model):
    colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd']
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 14))

    for i, wl in enumerate(WORKLOAD_RANGE):
        c = colors[i % len(colors)]
        ax1.errorbar(threads_range, results_spin[i, :], yerr=results_spin_mad[i, :],
                     label=f'Spin ({wl} NOPs)', color=c, marker='o', lw=2, capsize=3)
        ax1.errorbar(threads_range, results_mutex[i, :], yerr=results_mutex_mad[i, :],
                     label=f'Mutex ({wl} NOPs)', color=c, ls='--', marker='x',
                     lw=1.5, alpha=0.7, capsize=3)

    ax1.set_title(f"Execution Latency (Normalized 1M Iters)\nTarget Hardware: {cpu_model}")
    ax1.set_ylabel("Total Time (ms)")
    ax1.set_xticks(threads_range)
    ax1.grid(True, which='both', ls='--', alpha=0.5)
    ax1.legend(loc='upper left', ncol=2, fontsize='small')

    for i, wl in enumerate(WORKLOAD_RANGE):
        c = colors[i % len(colors)]
        ax2.plot(threads_range, results_ratio[i, :], label=f'{wl} NOPs', color=c, marker='s', lw=2)

    ax2.axhline(y=1.0, color='red', ls='-', alpha=0.5, label='Baseline (1.0x)')
    ax2.set_title("Speedup Analysis: Mutex / Spinlock Ratio")
    ax2.set_xlabel("Number of Threads")
    ax2.set_ylabel("Ratio (Speedup Multiplier)")
    ax2.set_xticks(threads_range)
    ax2.grid(True, which='both', ls='--', alpha=0.5)
    ax2.legend(title="Workload Intensity (Critical Section)")

    plt.subplots_adjust(left=0.1, right=0.95, top=0.92, bottom=0.08, hspace=0.35)
    plt.savefig("bench_result.png", dpi=300)


def main():
    if not os.access(BENCHMARK_BIN, os.X_OK):
        print(f"Error: {BENCHMARK_BIN} not found or not executable. Run 'make' first.",
              file=sys.stderr)
        sys.exit(1)

    cpu_model, num_cpus, l1_cache = get_system_info()
    threads_range = build_threads_range(num_cpus)
    start_time = time.time()

    n_wl = len(WORKLOAD_RANGE)
    n_th = len(threads_range)

    results_spin = np.zeros((n_wl, n_th))
    results_mutex = np.zeros((n_wl, n_th))
    results_spin_mad = np.zeros((n_wl, n_th))
    results_mutex_mad = np.zeros((n_wl, n_th))
    results_ratio = np.zeros((n_wl, n_th))

    total_steps = n_wl * n_th
    current_step = 0
    total_raw_cycles = 0
    report_data = []

    print(f"Executing Benchmarks on {cpu_model}...")

    for i, wl in enumerate(WORKLOAD_RANGE):
        for j, t in enumerate(threads_range):
            spin_med, mutex_med, spin_mad, mutex_mad, iters = run_bench(t, wl)

            results_spin[i, j] = spin_med
            results_mutex[i, j] = mutex_med
            results_spin_mad[i, j] = spin_mad
            results_mutex_mad[i, j] = mutex_mad
            ratio = mutex_med / spin_med if spin_med > 0 else 0.0
            results_ratio[i, j] = ratio

            total_raw_cycles += iters * REPEATS * t
            report_data.append({
                'workload': wl, 't': t,
                'spin_med': spin_med, 'mutex_med': mutex_med,
                'spin_mad': spin_mad, 'mutex_mad': mutex_mad,
                'ratio': ratio, 'iters': iters
            })

            current_step += 1
            pct = (current_step / total_steps) * 100
            bar = '=' * int(pct // 2)
            sys.stdout.write(f"\rProgress: [{bar:<50}] {pct:.1f}% ({wl} NOPs, {t} Threads)")
            sys.stdout.flush()

    sys.stdout.write("\r" + " " * 120 + "\r")

    duration = time.time() - start_time
    print_report(cpu_model, num_cpus, l1_cache, report_data, threads_range,
                 total_raw_cycles, duration)
    plot_results(results_spin, results_mutex, results_spin_mad, results_mutex_mad,
                 results_ratio, threads_range, cpu_model)

    print("[Done] Report and plots saved as 'bench_result.png'")


if __name__ == "__main__":
    main()
