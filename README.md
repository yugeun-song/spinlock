# Spinlock Implementation & Performance Test

This project provides a custom spinlock implementation using architecture-specific inline assembly (x86-64 and arm64) and compares its performance head-to-head with the POSIX spinlock (`pthread_spin_lock`). The benchmark allows granular control over threading, iteration counts, and workload simulation via command-line arguments.

## Supported Platforms
- **Architecture**: x86-64 and arm64 (AArch64). Each acquire/release primitive is emitted as architecture-specific inline assembly selected at compile time (`#if defined(__x86_64__)` / `__aarch64__`); any other target stops with a `#error`.
  - **x86-64**: `pause` spin hint, `lock cmpxchgl` acquire, and a plain store release — sufficient under the strong x86-TSO memory model.
  - **arm64**: `yield` spin hint and an `stlr` store-release (a plain store is *not* a release on the weakly-ordered arm64 memory model). The acquire is chosen at compile time:
    - **ARMv8.1-A LSE** (`__ARM_FEATURE_ATOMICS` defined): a single-instruction `casa` (load-acquire compare-and-swap). With no exclusive monitor to lose, it has no retry loop and scales far better under heavy contention. Enabled when the toolchain targets an LSE-capable CPU (e.g. `-march=armv8.1-a` or `-march=armv8-a+lse`).
    - **Baseline ARMv8-A** (no LSE): an `ldaxr`/`stlxr` load-acquire exclusive CAS retry loop with `clrex` on mismatch — the portable fallback used when LSE is unavailable.
- **OS**: Linux
- **Compilers**: GCC or Clang (Standard: `gnu99`)
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

### arm64 (AArch64)

On an arm64 host, `make` and `cmake` work unchanged — the build flags carry no x86-specific options. To cross-build from an x86 host and run under QEMU:

```bash
aarch64-linux-gnu-gcc -O3 -std=gnu99 -Wall -Wextra -static \
    spinlock_test.c test.c -o spinlock_test_arm64 -pthread -lrt
qemu-aarch64 ./spinlock_test_arm64 -t 8 -l 0 -i 100000
```

By default the acquire uses the portable `ldaxr`/`stlxr` LL/SC loop. To emit the single-instruction LSE `casa` acquire instead, target an LSE-capable architecture — add `-march=armv8.1-a` (or `-march=armv8-a+lse`, or an `-mcpu=` for a core that implements LSE such as `neoverse-n1`):

```bash
aarch64-linux-gnu-gcc -O3 -std=gnu99 -Wall -Wextra -static -march=armv8.1-a \
    spinlock_test.c test.c -o spinlock_test_arm64_lse -pthread -lrt
```

You can confirm which path was compiled in by disassembling: `casa` means the LSE path, `ldaxr`/`stlxr` means the LL/SC fallback.

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
Simulate a scenario where the lock is held for a longer duration (10,000 nops), which narrows the gap between the two spinlocks as raw acquire cost is amortized over the critical section.
```bash
./bin/spinlock_test -l 10000
```

#### 4. Tuning Backoff Algorithm
Adjust the exponential backoff parameters to optimize for specific hardware (e.g., Intel Core Ultra or ARM Cortex/Neoverse cores).
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

# Valgrind: memory errors, thread races, cache profile
valgrind --tool=memcheck --leak-check=full ./bin/spinlock_test_trace -t 2 -l 0 -i 1000
valgrind --tool=helgrind                    ./bin/spinlock_test_trace -t 4 -l 0 -i 1000
valgrind --tool=drd                         ./bin/spinlock_test_trace -t 4 -l 0 -i 1000
valgrind --tool=cachegrind                  ./bin/spinlock_test_trace -t 2 -l 500 -i 10000
```

All commands above run unchanged on an arm64 host. To drive a cross-built arm64 binary from an x86 host, wrap it in QEMU's gdbstub and attach the cross debugger:

```bash
qemu-aarch64 -g 1234 ./spinlock_test_arm64_trace -t 2 -l 0 -i 1000 &
aarch64-linux-gnu-gdb -ex 'target remote :1234' -ex 'b spin_lock' ./spinlock_test_arm64_trace
```

Use `make distclean` to scrub every debugger / profiler / tracer artifact (cores, valgrind dumps, perf.data, uftrace.data, `__pycache__`, CMake residue, …) on top of `make clean`'s build-only sweep.

The release build (`./bin/spinlock_test`) is what `test_bench.py` exercises and what produces the headline numbers below.

## Benchmark Results
*Test environment: Intel Core Ultra 5 226V (8 cores / 8 threads), Arch Linux. Median of 7 runs (+ 1 discarded warmup), normalized to 1,000,000 lock/unlock cycles. Each measurement runs in a page-locked process (`mlockall`) and settles between runs (speedup = POSIX spin / custom spin; above 1.0 the custom spinlock wins).*

### Headline: across the spinlock regime (4 threads)

A spinlock is the right tool only for *tiny* critical sections — a flag flip, a pointer swap, a few struct fields — so the table below sweeps that regime densely (CS time ≈ 0.125 ns/NOP). At 4 contending threads the custom lock's read-only spin + exponential backoff + bounded `nanosleep` yield wins across the **entire realistic range**, and the advantage decays smoothly as the critical section grows, crossing break-even only near a **64 ns** CS:

| CS work (NOPs) | ≈ CS time | Typical operation | Custom (ms) | POSIX (ms) | Speedup |
| :--- | :--- | :--- | :--- | :--- | :--- |
| 0 | 0 ns | pure lock contention | **46.4** | 374.7 | **8.08x** |
| 16 | ≈ 2 ns | flag flip / pointer swap | **38.6** | 307.5 | **7.96x** |
| 32 | ≈ 4 ns | set 1–2 struct fields | **46.0** | 298.3 | **6.48x** |
| 64 | ≈ 8 ns | set a few fields | **64.3** | 269.6 | **4.19x** |
| 128 | ≈ 16 ns | small struct update | **182.1** | 240.9 | **1.32x** |
| 256 | ≈ 32 ns | larger tiny-CS | **213.0** | 296.6 | **1.39x** |
| 512 | ≈ 64 ns | small CS | 421.0 | 388.3 | 0.92x |
| 1,024 | ≈ 128 ns | medium CS (context) | 661.2 | 630.3 | 0.95x |

The key correction over a coarse `[0, 200, 2000]` sweep: the custom lock does **not** only win at the artificial 0-NOP point. It wins by **roughly 4–8x throughout the realistic 0–8 ns regime** and still leads at a 16–32 ns CS; only past ≈ 64 ns do the two converge, after which the simpler `pthread_spin_lock` is marginally faster (its tighter loop is pure win once contention on the lock word is rare). Under heavier contention the advantage extends to even larger critical sections — see below. Both locks are measured with identical cache-line isolation, so the comparison reflects the locking algorithm, not stack layout.

## Automated Benchmarking & Visualization
A Python-based automated runner (`test_bench.py`) sweeps thread counts × workload intensities, aggregates with **Median ± MAD** over **7 runs (+ 1 discarded warmup)**, and emits a textual report, an OHLC-style **candlestick** plot (`bench_result.png`), and a raw CSV of every measurement (`bench_results.csv`). Each candle encodes the full 7-run distribution per (threads, workload, lock) cell:

- **Body** = IQR (Q1 – Q3) — the typical run-to-run range
- **Wick** = min – max — the noise envelope (how far an outlier can drag a run)
- **Tick across the body** = median (the headline number)
- The two candles at each thread count are the **custom spinlock** (left) and the **POSIX spinlock** (right); the winner (lower median latency) is coloured — **red** if the custom spinlock wins, **blue** if the POSIX spinlock wins — and the loser is muted to gray.

The bottom panel shows the corresponding speedup (POSIX spin / custom spin) as a line plot per workload; above 1.0, the custom spinlock is faster.

The eight workload panels are laid out as a 4×2 grid of log-scale plots with an alternating shaded lane behind each thread count, so the candle pairs stay clearly separated even at high thread counts; the median of each cell is printed above its candle. Run `python3 test_bench.py --plot-only` to regenerate `bench_result.png` from an existing `bench_results.csv` without re-running the benchmark (the CSV is read, never modified).

The critical section is emulated with N filler NOPs (measured at ≈ 0.125 ns/NOP on this CPU). The workload range is a powers-of-two sweep, deliberately dense in the regime where a spinlock is the right tool — small, fast updates to shared state: `0` (pure lock contention, no CS), `16`/`32`/`64` (≈ 2–8 ns: a flag flip, a pointer swap, a couple of struct fields), `128`/`256` (≈ 16–32 ns: a small struct), `512` (≈ 64 ns), and `1,024` (≈ 128 ns, a medium CS shown for context, where the two locks converge). Heavier critical sections are intentionally omitted: holding a busy-wait spinlock that long is the wrong design, so benchmarking it would not inform a real spinlock choice.

**Measurement isolation.** To keep external interference out of each measurement, every benchmark process locks its resident pages into RAM (`mlockall(MCL_CURRENT)`, best-effort), discards a warm-up run, and inserts a quiescent settle gap both between the two in-process lock measurements (`SETTLE_DELAY_MS`, default 100 ms) and between consecutive process launches (`SETTLE_SEC`, default 0.3 s) so one run cannot bias the next.

### How contention scales the advantage (speedup = POSIX spin / custom spin)

The headline is at 4 threads; contention changes *how far* the custom lock's edge reaches. The grid below is the median speedup at each (CS size × thread count) — the deeper into contention, the larger the critical section the custom lock keeps winning at:

| CS work | 2 threads | 4 threads | 8 threads | 16 (oversub.) |
| :--- | :--- | :--- | :--- | :--- |
| **32 NOPs** (≈ 4 ns, realistic tiny CS) | 3.52x | 6.48x | 7.71x | 6.57x |
| **256 NOPs** (≈ 32 ns, small CS) | 1.11x | 1.39x | 2.45x | 3.25x |
| **1,024 NOPs** (≈ 128 ns, medium CS) | 1.00x | 0.95x | 1.04x | 0.98x |

> **Reading the matrix.** Both locks are user-space busy-wait spinlocks; the only difference is the wait strategy. In the regime spinlocks are actually used — **tiny critical sections (≈ 0–8 ns)** — the custom lock's read-only spin + exponential backoff + bounded `nanosleep` yield beats `pthread_spin_lock`'s tight test-and-set by **4–8x under contention**, because backed-off waiters stop hammering the contended cache line and let the holder make progress while the POSIX lock keeps every waiter spinning on the line. As the critical section grows the edge shrinks (acquire cost is amortized over the work done under the lock), but **more contention pushes the break-even point to larger critical sections**: at a 4 ns CS the custom lock wins at every thread count; at a 32 ns CS it is roughly tied at low contention yet grows to **2.5–3x faster at 8–16 threads**; only by a ≈ 64 ns CS do the two truly converge. Past that, the simpler POSIX spinlock is marginally faster — e.g. at a **64 ns CS** (512 NOPs) it edges ahead by ~5–10% at low-to-mid contention, where the custom lock's `nanosleep` backoff adds latency the tighter loop avoids. Single-thread numbers are within ±10% across all workloads, as expected. The wick length in `bench_result.png` (and the log y-axis) exposes the variance jump at over-subscription. **Takeaway:** for the small, fast shared-state updates a spinlock is meant for, the custom lock is the clear winner; the POSIX spinlock only catches up once the critical section is too long to belong under a spinlock in the first place.

The full raw measurement table (560 rows: 8 workloads × 5 thread counts × 2 locks — custom spinlock and POSIX spinlock — × 7 runs) is shipped as [`bench_results.csv`](bench_results.csv) for downstream analysis.

![Benchmark Result](bench_result.png)

## Stability & Sanity Checks

The trace build (`./bin/spinlock_test_trace`) was exercised under several validators:

| Tool | Scope | Result |
| :--- | :--- | :--- |
| **Stress matrix** (5 thread × 3 workload × 2 iter × 2 lock = 60 runs) | atomic-count correctness | **60/60 OK** |
| **valgrind memcheck** (`--leak-check=full`) | memory errors / leaks | **0 errors** |
| **valgrind drd** | data races | **0 errors** |
| **valgrind helgrind** | data races | 24 errors / 4 contexts (false positives) |
| **AddressSanitizer + UBSan** (`-fsanitize=address,undefined`) | memory + UB | **clean, atomic count OK** |
| **ThreadSanitizer** (`-fsanitize=thread`) | data races | 4 warnings (false positives), atomic count OK |

The helgrind / TSan warnings are **expected**: both detectors only recognise synchronization expressed through `pthread` primitives or C11 `<stdatomic.h>`, and our spinlock acquires the lock through raw atomic instructions (`lock cmpxchgl` on x86-64; an `ldaxr`/`stlxr` load-acquire CAS — or a single `casa` load-acquire CAS when built for ARMv8.1-A LSE — with an `stlr` release on arm64) over a `volatile`-qualified flag, which they cannot pattern-match. `drd` ignores them because of how it tracks vector clocks per memory access. None of the tools reported a memory error and every run produced the expected atomic count.

The results above are from the x86-64 build. On arm64, correctness is established by construction: both acquire paths are confirmed by disassembly (the LSE `casa` under `-march=armv8.1-a`, and the `ldaxr`/`stlxr` LL/SC loop with `stlr` release on baseline `-march=armv8-a`), and the atomic-count oracle passes under QEMU (`qemu-aarch64`) for **both** builds across high-contention and over-subscribed thread counts. Note that QEMU-user does not reproduce weak-memory reordering, so the guarantee rests on the architecturally-correct acquire/release barriers rather than on the emulator.
