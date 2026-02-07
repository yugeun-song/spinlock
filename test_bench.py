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

# ==========================================
# 1. Configuration & System Detection
# ==========================================

def get_cpu_model():
    try:
        with open("/proc/cpuinfo", "r") as f:
            for line in f:
                if "model name" in line:
                    return line.split(":")[1].strip()
    except:
        return platform.processor()

cpu_model = get_cpu_model()
try:
    num_cpus = os.sysconf('_SC_NPROCESSORS_ONLN')
    l1_cache = subprocess.check_output("getconf LEVEL1_DCACHE_LINESIZE", shell=True).decode().strip()
except:
    num_cpus = 4
    l1_cache = "64"

threads_list = []
curr = num_cpus * 2
while curr >= 1:
    threads_list.append(curr)
    curr //= 2
threads_range = sorted(list(set(threads_list)))

# Workload Intensity (Mock NOP)
workload_range = [0, 500, 2000, 5000]
REPEATS = 5 
START_TIME = time.time()

# ==========================================
# 2. Helper Functions
# ==========================================

def remove_outliers_iqr(data):
    if len(data) < 4:
        return np.median(data)
    q1, q3 = np.percentile(data, [25, 75])
    iqr = q3 - q1
    lower, upper = q1 - (1.5 * iqr), q3 + (1.5 * iqr)
    clean_data = [x for x in data if lower <= x <= upper]
    return np.mean(clean_data) if clean_data else np.mean(data)

def run_bench(threads, workload):
    raw_spin, raw_mutex = [], []
    iterations = 1000000 if workload < 1000 else 400000
    
    for _ in range(REPEATS):
        cmd = f"./bin/spinlock_test -t {threads} -l {workload} -i {iterations}"
        try:
            res = subprocess.check_output(cmd, shell=True, stderr=subprocess.DEVNULL).decode()
            s_m = re.search(r"Custom Hybrid Spinlock \]\s+- Elapsed Time :\s+([\d.]+)", res)
            m_m = re.search(r"POSIX Mutex\s+\]\s+- Elapsed Time :\s+([\d.]+)", res)
            if s_m and m_m:
                scale = 1000000 / iterations
                raw_spin.append(float(s_m.group(1)) * scale)
                raw_mutex.append(float(m_m.group(1)) * scale)
        except: continue
    return remove_outliers_iqr(raw_spin), remove_outliers_iqr(raw_mutex), iterations

# ==========================================
# 3. Execution & Unified Reporting
# ==========================================

results_spin = np.zeros((len(workload_range), len(threads_range)))
results_mutex = np.zeros((len(workload_range), len(threads_range)))
results_ratio = np.zeros((len(workload_range), len(threads_range)))

total_steps = len(workload_range) * len(threads_range)
current_step = 0
total_raw_cycles = 0
report_data = []

print(f"Executing Benchmarks on {cpu_model}...")

for i, workload_count in enumerate(workload_range):
    for j, t in enumerate(threads_range):
        st, mt, actual_iters = run_bench(t, workload_count)
        results_spin[i, j], results_mutex[i, j] = st, mt
        ratio = mt / st if st > 0 else 0.0
        results_ratio[i, j] = ratio
        
        total_raw_cycles += (actual_iters * REPEATS * t)
        report_data.append({
            'workload': workload_count, 't': t, 'st': st, 'mt': mt, 'ratio': ratio, 'iters': actual_iters
        })
        
        current_step += 1
        progress = (current_step / total_steps) * 100
        sys.stdout.write(f"\rProgress: [{'=' * int(progress // 2):<50}] {progress:.1f}% ({workload_count} NOPs, {t} Threads)")
        sys.stdout.flush()

full_report = "\n" + "="*125 + "\n"
full_report += "SYSTEM & PERFORMANCE REPORT: HYBRID SPINLOCK BENCHMARK\n"
full_report += "="*125 + "\n"
full_report += f"HARDWARE SPECIFICATIONS:\n"
full_report += f"  - CPU Model       : {cpu_model}\n"
full_report += f"  - CPU Cores       : {num_cpus} Online / {os.cpu_count()} Logical\n"
full_report += f"  - L1 Cache Line   : {l1_cache} bytes\n"
full_report += "-"*125 + "\n"
full_report += f"TEST PARAMETERS:\n"
full_report += f"  - Noise Filter    : IQR Outlier Removal\n"
full_report += f"  - Normalization   : 1,000,000 Lock/Unlock cycles\n"
full_report += f"  - Total Raw Ops   : {total_raw_cycles:,} cycles performed\n"
full_report += f"  - Bench Duration  : {time.time() - START_TIME:.2f} seconds\n"
full_report += "="*125 + "\n"
full_report += f"{'Workload Intensity (NOPs)':<30} | {'Threads':<8} | {'Raw Iters':<12} | {'Spin(ms)':<15} | {'Mutex(ms)':<15} | {'Speedup':<10}\n"
full_report += "-" * 125 + "\n"

for d in report_data:
    full_report += f"{d['workload']:<30} | {d['t']:<8} | {d['iters']:<12} | {d['st']:<15.3f} | {d['mt']:<15.3f} | {d['ratio']:<10.2f}\n"
    if d['t'] == threads_range[-1]:
        full_report += "-" * 125 + "\n"

sys.stdout.write("\r" + " " * 130 + "\r")
print(full_report)

# ==========================================
# 4. Professional Visualization
# ==========================================

colors = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd']
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 14))

for i, workload_count in enumerate(workload_range):
    c = colors[i % len(colors)]
    ax1.plot(threads_range, results_spin[i, :], label=f'Spin ({workload_count} NOPs)', color=c, marker='o', lw=2)
    ax1.plot(threads_range, results_mutex[i, :], label=f'Mutex ({workload_count} NOPs)', color=c, ls='--', marker='x', lw=1.5, alpha=0.7)

ax1.set_title(f"Execution Latency (Normalized 1M Iters)\nTarget Hardware: {cpu_model}")
ax1.set_ylabel("Total Time (ms)")
ax1.set_xticks(threads_range)
ax1.grid(True, which='both', ls='--', alpha=0.5)
ax1.legend(loc='upper left', ncol=2, fontsize='small')

for i, workload_count in enumerate(workload_range):
    c = colors[i % len(colors)]
    ax2.plot(threads_range, results_ratio[i, :], label=f'{workload_count} NOPs', color=c, marker='s', lw=2)

ax2.axhline(y=1.0, color='red', ls='-', alpha=0.5, label='Baseline (1.0x)')
ax2.set_title("Speedup Analysis: Mutex / Spinlock Ratio")
ax2.set_xlabel("Number of Threads")
ax2.set_ylabel("Ratio (Speedup Multiplier)")
ax2.set_xticks(threads_range)
ax2.grid(True, which='both', ls='--', alpha=0.5)
ax2.legend(title="Workload Intensity (Critical Section)")

plt.subplots_adjust(left=0.1, right=0.95, top=0.92, bottom=0.08, hspace=0.35)

plt.savefig("bench_result.png", dpi=300)
print(f"[Done] Research-grade report and plots saved as 'bench_result.png'")
