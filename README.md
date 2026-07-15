# Spinlock Implementation & Performance Test

This project provides custom spinlocks using architecture-specific inline assembly (x86-64 and arm64) and compares them head-to-head with the POSIX spinlock (`pthread_spin_lock`). Two disciplines are exposed, each named for its algorithm, behind the same single-argument API: `spin_lock_ttas` (test-and-test-and-set with exponential backoff — the recommended default for short critical sections) and `spin_lock_mcs` (an MCS queue lock). The benchmark allows granular control over threading, iteration counts, workload simulation, CPU pinning, and which contenders run, via command-line arguments.

## Supported Platforms
- **Architecture**: x86-64 and arm64 (AArch64). Each acquire/release primitive is emitted as architecture-specific inline assembly selected at compile time (`#if defined(__x86_64__)` / `__aarch64__`); any other target stops with a `#error`.
  - **x86-64**: `pause` spin hint, `lock cmpxchgl` acquire, and a plain store release — sufficient under the strong x86-TSO memory model.
  - **arm64**: `yield` spin hint and an `stlr` store-release (a plain store is *not* a release on the weakly-ordered arm64 memory model). The acquire is chosen at compile time:
    - **ARMv8.1-A LSE** (`__ARM_FEATURE_ATOMICS` defined): a single-instruction `casa` (load-acquire compare-and-swap). With no exclusive monitor to lose, it has no retry loop and scales far better under heavy contention. Enabled when the toolchain targets an LSE-capable CPU (e.g. `-march=armv8.1-a` or `-march=armv8-a+lse`).
    - **Baseline ARMv8-A** (no LSE): an `ldaxr`/`stlxr` load-acquire exclusive CAS retry loop with `clrex` on mismatch — the portable fallback used when LSE is unavailable.
- **Lock disciplines** (same single-argument `spin_*` API, both on x86-64 and arm64):
  - **`spin_lock_ttas`** — test-and-test-and-set with exponential backoff. The recommended default. Fastest for the short, lightly-held critical sections a spinlock targets: an uncontended acquire is a single CAS, and a just-freed lock can be re-taken with no cross-core cache-line transfer.
  - **`spin_lock_mcs`** — an MCS queue lock with a thread-local waiter node (tail swap via `xchg` / LSE `swpal` / LL-SC, hand-off via `stlr` / release store). Each waiter spins on its own cache line, so it stays FIFO-fair and storm-free at high core counts — but for short critical sections at low-to-modest contention it measures several times slower, and it convoys under oversubscription (a preempted successor stalls the whole queue). Reach for it only when many cores hammer the same lock and fairness matters.
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

On an arm64 host, `make` and `cmake` work unchanged. To cross-build from an x86 host, just point the build at the cross compiler: the Makefile reads the target triple from `$(CC) -dumpmachine`, so an aarch64 compiler automatically gets the ARMv8-A baseline `-march=armv8-a` (the portable `ldaxr`/`stlxr` LL/SC atomics). Run the result under QEMU:

```bash
make CC=aarch64-linux-gnu-gcc
qemu-aarch64 -L /usr/aarch64-linux-gnu ./bin/spinlock_test -t 4 -l 0 -i 100000
```

To emit the single-instruction LSE atomics (`casa` acquire, `swpal`/`casl` for the MCS queue) instead of the LL/SC fallback, target an LSE-capable architecture via `ARCH_CFLAGS` (Make) or `-DARCH_LSE=ON` (CMake):

```bash
make CC=aarch64-linux-gnu-gcc ARCH_CFLAGS='-march=armv8.1-a'   # or -mcpu=neoverse-n1
cmake -S . -B build -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DARCH_LSE=ON && cmake --build build -j
```

A fully static binary (no sysroot needed to run under QEMU) can be built directly:

```bash
aarch64-linux-gnu-gcc -O3 -std=gnu99 -Wall -Wextra -static -march=armv8.1-a \
    spinlock_test.c test.c -o spinlock_test_arm64_lse -pthread -lrt
```

Confirm which path was compiled in by disassembling: `casa`/`swpal` means the LSE path, `ldaxr`/`stlxr` means the LL/SC fallback.

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
| `-C` | `<cpulist>` | **Pin**: Bind workers round-robin to these cores (e.g. `0-3` or `0,2,4`) for deterministic one-thread-per-core placement. Pass at least as many cores as threads from one homogeneous class (all P or all E) to control P/E-core variance. | none |
| `-K` | `<locks>` | **Contenders**: Comma-separated subset of `ttas,mcs,pspin` to run. Drop `mcs` at thread counts that oversubscribe the cores, where the queue lock convoys. | all three |
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
Simulate a scenario where the lock is held for a longer duration (10,000 nops), which narrows the gap between the spinlocks as raw acquire cost is amortized over the critical section.
```bash
./bin/spinlock_test -l 10000
```

#### 4. Tuning Backoff Algorithm
Adjust the exponential backoff parameters to optimize for specific hardware (e.g., Intel Core Ultra or ARM Cortex/Neoverse cores).
```bash
./bin/spinlock_test -m 16 -M 4096
```

#### 5. Controlled Run (pinning + contender selection)
Pin all workers to the four P-cores for low-variance, deterministic placement, and compare only the TTAS lock against POSIX (dropping the MCS queue lock):
```bash
./bin/spinlock_test -t 4 -C 0-3 -K ttas,pspin
```

## Profiling & Tracing the Trace Build

The trace build (`./bin/spinlock_test_trace`) suppresses inlining and ships full debug info, so every `static inline` helper resolves to a real call frame. Common workflows:

```bash
# Function-level user-space trace (uftrace)
uftrace ./bin/spinlock_test_trace -t 8 -l 0 -i 100000

# Hot-path sampling with perf (DWARF call graph)
perf record -F 999 --call-graph dwarf ./bin/spinlock_test_trace -t 8 -l 0 -i 100000
perf report

# Single-step into spin_lock_ttas under gdb
gdb -ex 'b spin_lock_ttas' --args ./bin/spinlock_test_trace -t 2 -l 0 -i 1000

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
aarch64-linux-gnu-gdb -ex 'target remote :1234' -ex 'b spin_lock_ttas' ./spinlock_test_arm64_trace
```

Use `make distclean` to scrub every debugger / profiler / tracer artifact (cores, valgrind dumps, perf.data, uftrace.data, `__pycache__`, CMake residue, …) on top of `make clean`'s build-only sweep.

The release build (`./bin/spinlock_test`) is what `test_bench.py` exercises and what produces the headline numbers below.

## Benchmark Results
*Test environment: Intel Core Ultra 5 226V (4 P-cores @ 4.5 GHz + 4 E-cores @ 3.5 GHz), Arch Linux. Workers pinned to the four P-cores (`-C 0-3`), so the thread sweep runs 1–4 subscribed and 8 at 2× oversubscription. Median of 7 runs (+ 1 discarded warmup), normalized to 1,000,000 lock/unlock cycles. Each measurement runs in a page-locked process (`mlockall`) and settles between runs (speedup = POSIX spin / custom TTAS spin; above 1.0 the custom TTAS lock wins).*

### Headline: across the spinlock regime (4 threads)

A spinlock is the right tool only for *tiny* critical sections — a flag flip, a pointer swap, a few struct fields — so the table below sweeps that regime densely (CS time ≈ 0.125 ns/NOP), pinned to the four P-cores. At 4 contending threads the custom **TTAS** lock's read-only spin + exponential backoff + bounded `nanosleep` yield wins against POSIX across the **entire realistic range**, and the advantage decays smoothly as the critical section grows, crossing break-even only near a **64 ns** CS. The **MCS** queue lock trails badly throughout — a queue discipline pays a cache-line transfer on every hand-off, which is pure overhead when the critical section is short:

| CS work (NOPs) | ≈ CS time | Typical operation | TTAS (ms) | MCS (ms) | POSIX (ms) | POSIX/TTAS |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 0 | 0 ns | pure lock contention | **34.4** | 395.0 | 375.4 | **10.92x** |
| 16 | ≈ 2 ns | flag flip / pointer swap | **30.5** | 413.5 | 307.4 | **10.09x** |
| 32 | ≈ 4 ns | set 1–2 struct fields | **45.1** | 350.0 | 292.1 | **6.48x** |
| 64 | ≈ 8 ns | set a few fields | **72.8** | 354.6 | 306.7 | **4.21x** |
| 128 | ≈ 16 ns | small struct update | **172.4** | 370.5 | 220.2 | **1.28x** |
| 256 | ≈ 32 ns | larger tiny-CS | **230.3** | 445.4 | 282.6 | **1.23x** |
| 512 | ≈ 64 ns | small CS | 330.1 | 604.0 | 327.3 | 0.99x |
| 1,024 | ≈ 128 ns | medium CS (context) | 584.6 | 898.8 | 582.9 | 1.00x |

The custom TTAS lock does **not** only win at the artificial 0-NOP point: it leads by **~10x through the 0–2 ns regime**, ~6x at a 4 ns CS, and still ~4x at 8 ns; only past ≈ 64 ns do TTAS and POSIX converge, after which the simpler `pthread_spin_lock` is marginally faster (its tighter loop is pure win once contention on the lock word is rare). The **MCS queue lock is 2–14x slower than TTAS** across this whole regime (395 vs 34 ms at 0 NOPs, still ~1.5x behind at a 128 ns CS): its mandatory per-hand-off cache-line transfer is dead weight for a short critical section, and at 8-thread oversubscription it convoys so badly (a preempted successor stalls the whole FIFO queue) that it is dropped from the sweep. All three locks are measured with identical cache-line isolation, so the comparison reflects the locking algorithm, not stack layout.

## Automated Benchmarking & Visualization
A Python-based automated runner (`test_bench.py`) sweeps thread counts × workload intensities, aggregates with **Median ± MAD** over **7 runs (+ 1 discarded warmup)**, and emits a textual report, an OHLC-style **candlestick** plot (`bench_result.png`), and a raw CSV of every measurement (`bench_results.csv`). To control variance it auto-detects the highest-frequency core group (the P-cores on a heterogeneous P/E or big.LITTLE machine) and pins every worker to it via the `-C` option, sizing the thread sweep to that pinned set; `--no-pin` disables this. Each candle encodes the full 7-run distribution per (threads, workload, lock) cell:

- **Body** = IQR (Q1 – Q3) — the typical run-to-run range
- **Wick** = min – max — the noise envelope (how far an outlier can drag a run)
- **Tick across the body** = median (the headline number)
- The three candles at each thread count are **Custom TTAS** (red, left), **Custom MCS** (green, middle), and the **POSIX spinlock** (blue, right); the winner (lowest median latency) gets a thick black border. MCS is omitted at thread counts that oversubscribe the pinned cores, where a strict FIFO queue lock convoys.

The bottom panel shows the corresponding speedup (POSIX spin / Custom TTAS) as a line plot per workload; above 1.0, the custom TTAS lock is faster.

The eight workload panels are laid out as a 4×2 grid of log-scale plots with an alternating shaded lane behind each thread count, so the candle pairs stay clearly separated even at high thread counts; the median of each cell is printed above its candle. Run `python3 test_bench.py --plot-only` to regenerate `bench_result.png` from an existing `bench_results.csv` without re-running the benchmark (the CSV is read, never modified).

The critical section is emulated with N filler NOPs (measured at ≈ 0.125 ns/NOP on this CPU). The workload range is a powers-of-two sweep, deliberately dense in the regime where a spinlock is the right tool — small, fast updates to shared state: `0` (pure lock contention, no CS), `16`/`32`/`64` (≈ 2–8 ns: a flag flip, a pointer swap, a couple of struct fields), `128`/`256` (≈ 16–32 ns: a small struct), `512` (≈ 64 ns), and `1,024` (≈ 128 ns, a medium CS shown for context, where TTAS and POSIX converge). Heavier critical sections are intentionally omitted: holding a busy-wait spinlock that long is the wrong design, so benchmarking it would not inform a real spinlock choice.

**Measurement isolation.** To keep external interference out of each measurement, every benchmark process pins its workers to a homogeneous core set (see above), locks its resident pages into RAM (`mlockall(MCL_CURRENT)`, best-effort), warms every contender, and inserts a quiescent settle gap both between consecutive in-process lock measurements (`SETTLE_DELAY_MS`, default 100 ms) and between consecutive process launches (`SETTLE_SEC`, default 0.3 s) so one run cannot bias the next.

### How contention scales the advantage (speedup = POSIX spin / Custom TTAS)

The headline is at 4 threads; contention is what *creates* the custom TTAS lock's edge. The grid below is the median speedup at each (CS size × thread count) — at a single thread the two are near parity, and contention opens the gap widest for the shortest critical sections:

| CS work | 1 thread | 2 threads | 4 threads | 8 (oversub.) |
| :--- | :--- | :--- | :--- | :--- |
| **32 NOPs** (≈ 4 ns, realistic tiny CS) | 1.39x | 5.44x | 6.48x | 5.88x |
| **256 NOPs** (≈ 32 ns, small CS) | 1.03x | 1.13x | 1.23x | 1.06x |
| **1,024 NOPs** (≈ 128 ns, medium CS) | 1.00x | 1.01x | 1.00x | 0.98x |

> **Reading the matrix.** Both locks are user-space busy-wait spinlocks; the only difference is the wait strategy. At a single thread there is no contention and the two are within ~1.0–1.4x. The moment the lock is contended (2+ threads), TTAS's read-only spin + exponential backoff + bounded `nanosleep` yield pulls ahead sharply for short critical sections — **~5–11x at a 0–4 ns CS** — because backed-off waiters stop hammering the contended cache line and let the holder make progress, while `pthread_spin_lock` keeps every waiter spinning on the line. The edge shrinks as the critical section grows and the acquire cost is amortized over the work done under the lock: by a **32 ns CS** the two are within ~1.2x at every thread count, and by ≈ 64 ns they converge; past that the tighter POSIX loop is marginally faster, where the custom lock's `nanosleep` backoff adds latency it avoids. The wick length in `bench_result.png` (and the log y-axis) exposes the variance jump at over-subscription. **Takeaway:** for the small, fast shared-state updates a spinlock is meant for, TTAS is the clear winner under contention; the POSIX spinlock only catches up once the critical section is too long to belong under a spinlock in the first place.

The full raw measurement table (8 workloads × 4 thread counts × 3 locks — Custom TTAS, Custom MCS, and POSIX — × 7 runs, with MCS omitted at the oversubscribed 8-thread point) is shipped as [`bench_results.csv`](bench_results.csv) for downstream analysis.

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

The helgrind / TSan warnings are **expected**: both detectors only recognise synchronization expressed through `pthread` primitives or C11 `<stdatomic.h>`, and our spinlocks acquire through raw atomic instructions (TTAS: `lock cmpxchgl` on x86-64; an `ldaxr`/`stlxr` load-acquire CAS — or a single `casa` load-acquire CAS under ARMv8.1-A LSE — with an `stlr` release on arm64. MCS: `xchg`/`lock cmpxchg` on x86-64; `swpal`/`casl` under LSE or `ldaxr`/`stlxr` LL/SC, with `ldar`/`stlr` hand-off, on arm64) over `volatile`-qualified words, which they cannot pattern-match. `drd` ignores them because of how it tracks vector clocks per memory access. None of the tools reported a memory error and every run produced the expected atomic count.

The results above are from the x86-64 build. On arm64, correctness is established by construction: for **both** lock disciplines every acquire/release path is confirmed by disassembly (the LSE `casa`/`swpal`/`casl` under `-march=armv8.1-a`, and the `ldaxr`/`stlxr` LL/SC loops with `stlr` release on baseline `-march=armv8-a`), and the atomic-count oracle passes under QEMU (`qemu-aarch64`) for both builds and both locks across high-contention and over-subscribed thread counts. Note that QEMU-user does not reproduce weak-memory reordering, so the guarantee rests on the architecturally-correct acquire/release barriers rather than on the emulator.
