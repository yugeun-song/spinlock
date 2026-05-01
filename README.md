# Spinlock Implementation & Performance Test

This project provides a custom spinlock implementation using x86-64 inline assembly and compares its performance with POSIX mutex (`pthread_mutex`). The benchmark allows granular control over threading, iteration counts, and workload simulation via command-line arguments.

## Supported Platforms
- **Architecture**: x86-64 (Required for `pause` and `lock cmpxchgl` instructions)
- **OS**: Linux
- **Compilers**: GCC or Clang (Standard: `gnu17`)
- **Build Systems**: Make and CMake (≥ 3.16)
- **Build Modes**:
  - **Release**: `-O3 -Wall -Wextra -fno-omit-frame-pointer -fasynchronous-unwind-tables` — optimized for benchmarking with frame pointers and unwind tables preserved for `perf` and flame graphs.
  - **Trace/Debug**: adds `-O0 -g3 -fno-inline -fno-inline-functions -fno-optimize-sibling-calls -rdynamic` — every `static inline` helper resolves to a real call frame so `uftrace`, `gdb`, `strace`, and `perf` can step into each function.
- **Code Style**: LLVM-based `.clang-format` — right-aligned pointers, Allman function braces, K&R control flow, 100-column soft limit.

## Build Instructions

Either build system produces the same artifacts under `bin/`. Use GCC or Clang interchangeably.

### Make

```bash
make clean
make all              # builds both targets
# or build a single target:
make release          # ./bin/spinlock_test
make trace            # ./bin/spinlock_test_trace
```

Override the compiler with `make CC=clang all`.

### CMake

```bash
cmake -S . -B build
cmake --build build -j

# or with Clang:
CC=clang cmake -S . -B build
cmake --build build -j
```

### Artifacts (in `bin/`)
- `./bin/spinlock_test` — **release build**, used for benchmarking and the headline numbers below.
- `./bin/spinlock_test_trace` — **trace/debug build**, used with `uftrace`, `gdb`, `strace`, `perf`, and other analysis tools. Inlining is fully suppressed and `-rdynamic` exposes all symbols, so every helper appears as a real call frame.

## Usage & Options

Run the binary directly from the command line. If no arguments are provided, default values are used.

```bash
./bin/spinlock_test [options]
```

### Command-Line Arguments

| Option | Argument | Description | Default |
| :--- | :--- | :--- | :--- |
| `-t` | `<threads>` | **Thread Count**: Number of concurrent worker threads to spawn. | `4` |
| `-i` | `<iters>` | **Iterations**: Number of critical section entries per thread. | `1,000,000` |
| `-l` | `<loops>` | **Workload**: Number of `nop` instructions to execute inside the critical section (simulates load). | `500` |
| `-m` | `<min>` | **Min Backoff**: Initial spin count for the exponential backoff algorithm. | `4` |
| `-M` | `<max>` | **Max Backoff**: Maximum spin count before deferring to the scheduler via a bounded `nanosleep`. | `16,000` |
| `-h` | N/A | **Help**: Display usage information and exit. | N/A |

### Execution Examples

#### 1. Default Run
Use standard settings (optimized for general testing).
```bash
./bin/spinlock_test
```

#### 2. High Contention Test
Simulate heavy contention with 8 threads and a very short critical section.
```bash
./bin/spinlock_test -t 8 -l 0
```

#### 3. Long Critical Section Test
Simulate a scenario where the lock is held for a longer duration (10,000 nops), which typically favors Mutex over Spinlock.
```bash
./bin/spinlock_test -l 10000
```

#### 4. Tuning Backoff Algorithm
Adjust the exponential backoff parameters to optimize for specific hardware (e.g., Intel Core Ultra series).
```bash
./bin/spinlock_test -m 16 -M 4096
```

## Profiling & Tracing the Trace Build

The trace build (`./bin/spinlock_test_trace`) suppresses inlining and ships full debug info, so every `static inline` helper resolves to a real call frame. Common workflows:

```bash
# Function-level user-space trace (uftrace)
uftrace ./bin/spinlock_test_trace -t 8 -l 0 -i 100000

# Hot-path sampling with perf (DWARF call graph)
perf record -F 999 --call-graph dwarf ./bin/spinlock_test_trace -t 8 -l 0 -i 100000
perf report

# Single-step into spin_lock under gdb
gdb -ex 'b spin_lock' --args ./bin/spinlock_test_trace -t 2 -l 0 -i 1000

# Syscall summary
strace -c ./bin/spinlock_test_trace -t 4 -l 0 -i 100000
```

The release build (`./bin/spinlock_test`) is what `test_bench.py` exercises and what produces the headline numbers below.

## Benchmark Results
*Test environment: Intel Core Ultra 5 226V (8 cores / 8 threads), Arch Linux. Median of 7 runs (+ 1 discarded warmup), normalized to 1,000,000 lock/unlock cycles.*

### Headline (4 threads)

| Scenario | Lock Type | Time (ms) | Speedup |
| :--- | :--- | :--- | :--- |
| **Extreme contention (0 NOPs)** | Pthread Mutex | 378.7 | 1.0x |
| | **Custom Spinlock** | **51.3** | **7.4x** |
| **Short CS (200 NOPs)** | Pthread Mutex | 659.0 | 1.0x |
| | **Custom Spinlock** | **247.5** | **2.7x** |
| **Medium CS (2,000 NOPs)** | Pthread Mutex | 2,530.7 | 1.0x |
| | **Custom Spinlock** | **1,186.3** | **2.1x** |
| **Long CS (10,000 NOPs)** | Pthread Mutex | 7,554.8 | 1.0x |
| | **Custom Spinlock** | **5,283.0** | **1.4x** |

## Automated Benchmarking & Visualization
A Python-based automated runner (`test_bench.py`) sweeps thread counts × workload intensities, aggregates with **Median ± MAD** over **7 runs (+ 1 discarded warmup)**, and emits both a textual report and a comparison plot. Workloads target modern x86 CPUs: `0` (lock acquire/release alone), `200` (very short CS, ≈50 ns at ~4 GHz), `2,000` (medium CS, ≈500 ns), and `10,000` (long CS, ≈2.5 µs).

### Real-World Performance (Intel Core Ultra 5 226V, 8 cores / 8 threads)

| Workload (NOPs) | Threads | Spin (ms) | Mutex (ms) | Speedup |
| :--- | :--- | :--- | :--- | :--- |
| **0** (extreme contention) | 2 | 15.31 | 136.07 | **8.89x** |
| **0** (extreme contention) | 8 | 181.15 | 853.44 | **4.71x** |
| **200** (short CS) | 4 | 247.47 | 658.97 | **2.66x** |
| **200** (short CS) | 16 (oversub.) | 4,135.37 | 3,112.84 | **0.75x** |
| **2,000** (medium CS) | 4 | 1,186.29 | 2,530.74 | **2.13x** |
| **2,000** (medium CS) | 8 | 2,996.72 | 6,083.92 | **2.03x** |
| **10,000** (long CS) | 8 | 12,107.30 | 16,926.64 | **1.40x** |
| **10,000** (long CS) | 16 (oversub.) | 34,781.83 | 31,208.46 | **0.90x** |

> **Reading the matrix.** The spinlock dominates from low to medium contention up to ~2 ms critical sections. Once the system is **over-subscribed** (more threads than physical cores) and the critical section is non-trivial, the bounded `nanosleep` yield can no longer keep spinning threads from starving the lock holder, and the kernel-mediated mutex matches or wins. Single-thread numbers are essentially tied across all workloads, as expected.

![Benchmark Result](bench_result.png)
