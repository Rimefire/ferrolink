# ferrolink

[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-23-blue)](#requirements)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)](#requirements)
[![Header-only](https://img.shields.io/badge/header--only-yes-brightgreen)](#why-ferrolink)

**Lock-free, single-producer/single-consumer IPC transport over POSIX shared memory.**
No sockets. No syscalls on the hot path. No copies. Two processes, one ring buffer,
`std::atomic` doing all the work. This is the transport layer underneath every
Rimefire product that moves data between two processes and cannot afford to wait —
market data fan-out, order gateways, anything where a microsecond of syscall or
memcpy overhead shows up as a line item.

This isn't a toy benchmark repo dressed up as a library. It's a small, brutally
specific piece of infrastructure that does one job — move trivially-copyable
messages from one process to another as fast as the hardware allows — and refuses
to do anything else. Read the [Architectural contracts](#architectural-contracts)
below before you touch it. They are not suggestions.

```
Process A                          Process B
┌──────────────┐   /dev/shm/xxx   ┌──────────────┐
│ Producer      │ ───────────────▶│  Consumer     │
│ try_send(msg) │  ring buffer    │ try_recv(out) │
└──────────────┘                  └──────────────┘
```

## Is this for you?

**Use ferrolink if:** you have two Linux processes on the same host, one strict
producer and one strict consumer, moving fixed-shape trivially-copyable messages,
and every microsecond you can shave off the transport is money or latency budget
you get back. That's the entire use case. It is extremely good at it.

**Don't use ferrolink if:** you need multiple producers or multiple consumers on one
channel (see [Contract 2](#contract-2-exactly-one-producer-exactly-one-consumer--always)),
you need cross-host transport (this is `/dev/shm`, not a network stack), your
messages aren't trivially copyable, or you want a transport that degrades gracefully
instead of aborting on misconfiguration (see
[Contract 3](#contract-3-unrecoverable-setup-failures-abort--there-is-no-error-code-path)).
If any of those are true, this library will actively fight you — use something else.

## Why ferrolink

- **Zero-copy, lock-free.** A fixed-capacity SPSC ring buffer with `push`/`pop`
  implemented purely on top of `std::atomic` acquire/release pairs — no mutexes,
  no futexes, no syscalls on the hot path.
- **Real IPC, not just threads.** The same ring buffer that works between two threads
  in one process also works verbatim between two independent OS processes, mapped
  into the same POSIX shared-memory segment via `mmap`.
- **Crash-safe by default.** A process-global registry tracks every shared-memory
  segment a process has created and installs signal handlers that `shm_unlink` them
  before the process goes down — so a crashed producer doesn't leave garbage behind
  in `/dev/shm`. (This has sharp edges of its own — see
  [Contract 5: Signal Handling](#contract-5-signal-handling-shm_registry-owns-sigint-sigterm-sigabrt-sigsegv).)
- **Header-only.** `#include` it, link `ferrolink::ipc`, done.
- **Built for measurement, not vibes.** Ships with Google Benchmark–based latency
  (ping-pong RTT, percentiles) and throughput benchmarks, plus an extensive
  GoogleTest suite covering single-process, threaded, and real fork()-based
  cross-process scenarios.
- **Fails loud, not slow.** No hidden fallback path, no silent degradation. If the
  contracts below are violated, the process dies immediately and obviously — not
  three days later as a mystery latency spike in production.

## Requirements

- **Linux only.** Built directly on POSIX `shm_open`/`mmap`/`mlock`; `CMakeLists.txt`
  refuses to configure on anything but Linux.
- **C++23 compiler.** GCC or Clang. Uses `std::print`, `std::format`, and C++20/23
  concepts throughout.
- **CMake ≥ 3.25.**
- **Sufficient `RLIMIT_MEMLOCK`.** See
  [Contract 4: mlock and RLIMIT_MEMLOCK](#contract-4-mlock-and-rlimit_memlock-this-will-abort-in-a-fresh-container)
  before you deploy this anywhere with default container/systemd limits.
- Tests and benchmarks fetch **GoogleTest** and **Google Benchmark** automatically via
  `FetchContent`.

## Getting started

```bash
cmake -B build -S .
cmake --build build -j
ctest --test-dir build --output-on-failure
```

| Option                              | Effect                                              |
|--------------------------------------|------------------------------------------------------|
| `FERROLINK_BUILD_TESTS`             | Build the GoogleTest suite                          |
| `FERROLINK_BUILD_BENCHMARKS`        | Build the Google Benchmark latency/throughput binaries |
| `FERROLINK_BUILD_EXAMPLES`          | Build the `producer`/`consumer` example binaries     |
| `FERROLINK_ENABLE_SANITIZERS`       | ASan+UBSan in Debug builds only                      |
| `FERROLINK_NATIVE_ARCH`             | Build with `-march=native -mtune=native`             |
| `FERROLINK_ENABLE_LTO`              | Interprocedural optimization in Release/RelWithDebInfo |
| `FERROLINK_WARNINGS_AS_ERRORS`      | Treat every compiler warning as a build failure      |

> In-source builds (`cmake -B . -S .`) are rejected outright — always build out of tree.

## Quick start

`examples/` contains a complete, runnable producer/consumer pair — not toy stubs.
They stream a `MarketTick` struct (`examples/market_tick.hpp`) over a real
shared-memory channel, print sequence-gap warnings if the consumer falls behind,
and handle Ctrl+C cleanly. Build them and run each in its own terminal:

```bash
cmake -B build -S . -DFERROLINK_BUILD_EXAMPLES=ON
cmake --build build -j --target producer consumer

# terminal 1 — MUST be started first, see Contract 1 below
./build/examples/producer --rate-hz 1000

# terminal 2 — start only after producer has printed "channel ready"
./build/examples/consumer
```

Read `examples/producer.cpp` and `examples/consumer.cpp` top to bottom before writing
your own producer/consumer — the comments there work through the ordering and
signal-handling contracts below in situ, including the exact bug you get if you
get the signal-handler install order wrong.

The minimal shape of the API:

```cpp
#include <ipc/spsc_shm_channel.hpp>
#include <ipc/transport.hpp>

using Channel = ferrolink::ipc::shm_channel::SPSCShmChannel<MyMsg, /*Capacity=*/1024>;
using Trans   = ferrolink::ipc::transport::Transport<Channel, MyMsg>;

// Process A (must run first — Contract 1):
auto producer = Trans::create("channel_name");
producer.try_send(msg);

// Process B (must run second):
auto consumer = Trans::attach("channel_name");
consumer.try_recv(out);
```

## Architectural contracts

These are not suggestions. Every one of these is enforced by `abort()`, undefined
behavior, or silent data corruption if violated — there is no soft-failure mode.
Read all five before integrating `ferrolink` into a service.

### Contract 1: `create()` MUST run, and complete, before any peer's `attach()`

**This is the single most important rule in this library. The producer (creator)
process must always be started — and must always finish calling `Transport::create()`
/ `SPSCShmChannel::create()` — before the consumer (attacher) process starts and
calls `attach()`.**

Why this isn't optional: `create()` allocates a brand-new POSIX shared-memory object
(`O_CREAT | O_EXCL`) and placement-news a fresh `SPSCBuffer` into it — this is the
call that actually runs the buffer's constructor and initializes its atomics.
`attach()` opens an *existing* segment (`O_RDWR`, **no** `O_CREAT`) and
`reinterpret_cast`s it straight back into an `SPSCBuffer*`, deliberately **without**
re-running the constructor — re-running it would reset the atomics out from under a
producer that might already be using them.

Consequences if you get the order wrong:

- If the consumer's `attach()` runs before the producer's `create()` has been called
  at all: the shared-memory object doesn't exist yet → `shm_open` fails →
  `SPSCShmChannel::attach()` **aborts the process**. Loud, immediate, easy to
  diagnose in a fresh deployment.
- If the producer calls `create()` twice for the same name (e.g. a restart racing a
  stale segment, or two producer instances misconfigured with the same channel
  name): `O_CREAT | O_EXCL` fails with `EEXIST` → `SPSCShmChannel::create()`
  **aborts the process**.

There is **no retry loop, no polling, no wait-and-back-off** built into `attach()`
for the "not created yet" case, by design — silently retrying would hide a genuine
ordering bug behind a delay instead of surfacing it. If your deployment can't
guarantee program order (e.g. both processes are started concurrently by an
orchestrator with no ordering guarantee), you are responsible for adding your own
out-of-band synchronization (a readiness file, a semaphore, a health check the
consumer waits on) before calling `attach()`. `examples/producer.cpp` and
`examples/consumer.cpp` intentionally do **not** do this for you — they document, in
their own startup banners, that starting them in the wrong order will abort, and
leave real synchronization to you.

This also means: **`create()` always yields the producer role, `attach()` always
yields the consumer role**, in this codebase's example/API convention — but note
`role()` on `SPSCShmChannel` reflects *creator vs. attacher* (i.e. who owns
`shm_unlink` on cleanup), which is a **separate, orthogonal concept** from *who calls
push vs. pop*. `Transport` locks these together for you (`TransportProducer` only
exposes `try_send`, `TransportConsumer` only exposes `try_recv`); if you use the
lower-level `SPSCShmChannel` directly, nothing stops the attacher from calling
`buffer()->push(...)` — see Contract 2.

### Contract 2: exactly one producer, exactly one consumer — always

`SPSCBuffer`, `SPSCShmChannel`, and `Transport` are correct for **exactly one**
producer thread/process calling `push`/`try_send` and **exactly one** consumer
thread/process calling `pop`/`try_recv`, concurrently, at a time. This is not a
soft guideline — the memory-ordering scheme (each side treats its own index as
`relaxed` and the other side's index as `acquire`/`release`) is only correct under
single-writer-per-index conditions. Two producers racing `push()` on the same buffer
will corrupt the ring; two consumers racing `pop()` will do the same.

Nothing in the API prevents a third process from calling `SPSCShmChannel::attach()`
on an already-attached segment — `attach()` itself will succeed for any number of
callers (see `ShmChannelCrossProcessTest.MultipleAttachersCanReadIndependentlyIsNotMPSC`
in the test suite). What is **not** safe is more than one of those attachers actually
calling `push`/`pop` concurrently. If you need fan-out or fan-in, put a dedicated
`SPSCShmChannel` per producer/consumer pair — do not share one segment across
multiple writers or multiple readers.

### Contract 3: unrecoverable setup failures `abort()` — there is no error-code path

`shm_open`, `ftruncate`, `mmap`, and `mlock` failures inside `create()`/`attach()`,
along with signal-handler installation failures and shm-registry slot exhaustion,
all call `std::abort()`. None of these return an error code or throw an exception
you can catch. This is deliberate: in a latency-critical system, a channel that
silently degraded (fell back to something slower, or came up without the memory
guarantees it needs) is a worse failure mode than an immediate, loud crash that gets
caught in CI or on first deploy.

**Practically:** do not wrap `Transport::create()`/`Transport::attach()` in a
try/catch expecting to recover and retry — there is nothing to catch, and the
process is already gone by the time you'd handle it. Treat any abort from this
library as a configuration or ordering bug (wrong `RLIMIT_MEMLOCK`, violated
Contract 1, duplicate channel name) to be fixed, not a runtime condition to
gracefully route around. The **only** two calls in this library's public API that
return a `bool` for a condition you're expected to handle in a normal control-flow
sense are `try_send`/`try_recv` returning `false` for a full/empty buffer — that
one *is* routine and expected to happen constantly under backpressure.

### Contract 4: `mlock` and `RLIMIT_MEMLOCK` — this WILL abort in a fresh container

`SPSCShmChannel::create()` and `attach()` both call `mlock()` on the mapped segment
to pin it resident in RAM (preventing a page fault from injecting latency into
exactly the measurement this library exists to avoid). **Unlike the `madvise` hints
next to it, this `mlock()` call is treated as fatal — if it fails, the process
aborts.**

The default `RLIMIT_MEMLOCK` on most Linux distributions, systemd units, and
container runtimes is small (commonly 64 KB, sometimes as low as 8 MB) — far below
what a realistically-sized ring buffer needs. **If you do nothing, this library will
abort on startup in a fresh Docker container, a fresh systemd service, or a fresh
Kubernetes pod**, with an error that looks like an internal bug but is actually a
missing `ulimit`. Check before you deploy:

```bash
ulimit -l          # current soft limit, in KB — "unlimited" is what you want,
                    # or at minimum: comfortably above sizeof(SPSCBuffer<YourMsg, YourCapacity>)
```

How to fix it per environment:

- **Bare-metal / VM (systemd service):** add to the unit file:
  ```ini
  [Service]
  LimitMEMLOCK=infinity
  ```
- **Docker:**
  ```bash
  docker run --ulimit memlock=-1:-1 ...
  ```
  or in `docker-compose.yml`:
  ```yaml
  ulimits:
    memlock:
      soft: -1
      hard: -1
  ```
- **Kubernetes:** there is no first-class `memlock` field on `securityContext` —
  either grant `CAP_IPC_LOCK` on the container and ensure the node's containerd/
  Docker default `ulimit` for memlock is raised, or use a privileged init container
  / DaemonSet to raise the host-level limit before the pod starts. Validate this in
  a real cluster before shipping; do not assume it inherits from the node's shell
  `ulimit -l`.
- **Local development shell:** `ulimit -l unlimited` (or a specific large value) in
  the shell that launches the process, or persist it via `/etc/security/limits.conf`.

This is **the single most common "why does it abort immediately on a machine I
didn't test on" report** you should expect integrating this library — put the
`ulimit -l` check in your service's preflight/health-check tooling.

### Contract 5: Signal handling — `shm_registry` owns SIGINT, SIGTERM, SIGABRT, SIGSEGV

The first time any process in your service calls `SPSCShmChannel::create()` or
`attach()`, `ferrolink::detail::shm_registry::ensure_signal_handlers_registered()`
runs internally and installs its own handler for **`SIGABRT`, `SIGTERM`, `SIGSEGV`,
and `SIGINT`**, via `sigaction(signum, &sig_act, nullptr)`.

**Read that call signature again: the third argument — where the previously
installed handler would be saved — is `nullptr`. This is not a bug, but it means the
registration is unconditional and non-chaining: whatever handler your process had
installed for any of these four signals before this point is silently discarded and
never invoked again.** There is no fallback, no composition, no way to ask
`shm_registry` to call your handler afterward.

This will bite you if your service does any of the following **before** the first
`create()`/`attach()` call in the process:

- Installs its own `SIGINT`/`SIGTERM` handler for graceful shutdown (draining
  requests, flushing logs, closing other resources).
- Uses a crash reporter that installs a `SIGSEGV`/`SIGABRT` handler (Breakpad,
  Crashpad, Sentry's native handler, `google::InstallFailureSignalHandler`, etc.) to
  capture a backtrace before the process dies.
- Relies on a supervisor sending `SIGTERM` and expects your own cleanup code to run.

**The only correct pattern is: install your own handlers for these signals *after*
the first `create()`/`attach()` call in the process, not before.** This is exactly
what `examples/producer.cpp` and `examples/consumer.cpp` do — read
`install_stop_handler()` and its call site in both files; the comments there spell
out, with the specific bug you get if you reorder it ("Ctrl+C silently does shm
cleanup only and never actually stops the send loop"), why the ordering matters.

What you get if you follow this pattern correctly:

- Your handler is the one left active for `SIGINT`/`SIGTERM` for the rest of the
  process's life (it was installed last, so it wins).
- `shm_registry`'s handler remains the effective backstop only for signals your
  service doesn't itself re-handle after that point — realistically `SIGABRT` and
  `SIGSEGV` in most services, since almost nobody installs custom handlers for
  those, and it's exactly those two you generally *want* the shared-memory cleanup
  for.
- `SIGKILL` can never be caught by anyone, including `shm_registry` — a segment
  killed via `SIGKILL` is cleaned up only when the next process to `create()`/
  `attach()` under the same name runs, or manually via `rm /dev/shm/<name>`.

One more nuance if you inspect the behavior directly (see
`tests/test_shm_registry.cpp`'s `ShmRegistrySignalSafetyTest` suite for the verified
contract): `shm_registry`'s own handler is installed with `SA_RESETHAND`. If it is
still the active handler when a signal fires (i.e. you never overrode it per this
contract), the **first** `SIGTERM`/`SIGINT` unlinks registered segments and the
process **survives** — disposition resets to `SIG_DFL`, and only a **second**
`SIGTERM`/`SIGINT` actually terminates the process. If you're relying on
`shm_registry`'s handler as your only signal handling (i.e. you skipped installing
your own), do not assume a single `SIGTERM` kills the process — it doesn't, by
default.

### Memory layout & false sharing (not a hard contract, but load-bearing)

`SPSCBuffer`'s three members — the consumer-owned read index, the producer-owned
write index, and the backing storage — are each `alignas`'d to a 64-byte cache line
(`detail::cacheline::DestructiveSize`) and laid out in that order, so a write to one
side's index never invalidates the cache line the other side is reading. `Capacity`
must be a power of two (enforced with `static_assert`) so the ring wraps via a cheap
bitmask instead of a modulo.

## Benchmarks

`benchmarks/` contains two Google Benchmark binaries:

- **`bench_latency`** — pins a client and a server to two separate cores and measures
  round-trip time over one million individually-timed ping-pong exchanges, reporting
  min/mean/stddev and p50/p90/p99/p99.9/p99.99/max in the `rtt_*_ns` and
  `one_way_*_ns` counters.
- **`bench_throughput`** — pins a producer and a consumer to two separate cores and
  drains a large, fixed message count end-to-end, reporting aggregate messages/sec
  and bytes/sec, plus a `consumer_only_Mmsg_s` counter to help identify which side of
  the pipeline is the bottleneck.

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target bench_latency bench_throughput
./build/benchmarks/bench_latency
./build/benchmarks/bench_throughput
```

We don't publish fixed latency/throughput numbers here — results are highly
sensitive to CPU generation, NUMA topology, isolated/`nohz_full` cores, turbo/C-state
settings, and general machine noise, and a number copied from one box is actively
misleading on another. Run the benchmarks on your own target hardware (ideally with
cores isolated via `isolcpus`) and treat the output as your baseline.
`bench_latency`'s `try_realtime_priority()` helper (`SCHED_FIFO`, in
`benchmarks/common/pin.hpp`) is available but **not** enabled by default — read its
doc comment before turning it on, since misusing `SCHED_FIFO` in a busy-spin loop can
lock up a single-core or virtualized machine.

## Project layout

```
include/
  detail/           # internal helpers, not part of the public API surface
    cache_line.hpp     — portable cache-line size constants
    shm_registry.hpp   — crash-safety registry + signal handlers (Contract 5)
  ipc/
    spsc_buffer.hpp    — the lock-free ring buffer itself (thread-level SPSC)
    spsc_shm_channel.hpp — maps an SPSCBuffer into POSIX shared memory (process-level)
    transport.hpp      — role-locked Producer/Consumer wrapper over any Channel
benchmarks/          # Google Benchmark latency + throughput binaries
examples/            # runnable producer/consumer demo — start here
  market_tick.hpp       — shared wire-format message type for both examples
  producer.cpp           — creates the channel, streams simulated ticks
  consumer.cpp            — attaches, prints ticks, detects sequence gaps
tests/               # GoogleTest suite: unit, threaded, and fork()-based cross-process
```

## Testing

The suite (`tests/`) covers, per component:

- **`test_spsc.cpp`** — the ring buffer in isolation: FIFO ordering, full/empty
  boundaries, wraparound correctness at scale, cache-line alignment, and threaded
  producer/consumer races (useful under TSAN, since it doesn't need real processes).
- **`test_shm_channel.cpp`** — lifecycle and ownership (move semantics, who unlinks
  what and when), contract violations that must `abort()` (Contracts 1–3), and real
  `fork()`-based cross-process integrity checks with checksum-verified payloads.
- **`test_shm_registry.cpp`** — slot allocation/reuse, name truncation, capacity
  exhaustion, and the actual empirically-verified signal-handling behavior described
  in Contract 5 (including the `SA_RESETHAND` two-signal nuance) — read the comment
  block at the top of that file before changing anything there.
- **`test_transport.cpp`** — the `Channel`/`Buffer` concepts, role-locking guarantees,
  and the same cross-process integrity checks driven through the `Transport` API.

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug   # enables ASan+UBSan by default
cmake --build build -j
ctest --test-dir build --output-on-failure
```

IPC/`fork()`-based tests are registered with `RUN_SERIAL TRUE` since concurrent runs
touching the same `/dev/shm` object names can race on `O_EXCL` — don't parallelize
`ctest` runs for this suite beyond what `gtest_discover_tests` already sets up.

## License

MIT — see [LICENSE](LICENSE). Copyright © 2026 Rimefire Systems.
