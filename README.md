# Weave — Stage 0

Stage 0 answers one question, at three observation depths:

> **What can an external observer actually see, how reliably can it see it,
> and how much do we gain by moving the observer below the JVM boundary?**

It does **not** try to show that any signal predicts distributed-system liveness failure. It builds the smallest possible rig to find out what is observable at all, and it applies the **same five tests** to all three observers so the results are comparable.

---

## 1. Layout

```
weave/
  weave-target/     Target JVM (Maven) — the system being observed
  observer-jvm/      Observation Level 1 — Java/JMX Observer (Maven)
  observer-native/   Observation Level 2 — Native process observer (C)
  observer-os/        Observation Level 3 — OS-level observer (C)
  scripts/            build + the five test drivers
  results/            created at runtime, one timestamped folder per test run
```

Each observer is a **separate process**, deliberately. The Java observer
never touches the Target's memory — everything comes through JMX. The
native and OS observers never run inside a JVM at all.

---

## 2. Prerequisites (macOS)

```bash
# JDK 21+ (Temurin, or via Homebrew)
brew install --cask temurin@21

# Maven
brew install maven

# C compiler — Xcode Command Line Tools (gives you `cc`, libproc, Mach headers)
xcode-select --install
```

You can also do all of this from IntelliJ IDEA for the two Java modules
(open each `pom.xml` as a project / module) and just use the CLI for the C
observers and the test scripts.

---

## 3. Build everything

```bash
cd weave
chmod +x scripts/*.sh
scripts/build-all.sh
```

This runs, explicitly:

```bash
(cd weave-target   && mvn -q -DskipTests package)   # -> weave-target/target/weave-target.jar
(cd observer-jvm    && mvn -q -DskipTests package)   # -> observer-jvm/target/weave-observer-jvm.jar
(cd observer-native && make)                          # -> observer-native/native_observer
(cd observer-os      && make)                          # -> observer-os/os_observer
```

### If something doesn't compile

- `observer-native/native_observer.c` and `observer-os/os_observer.c` were
  written against documented macOS APIs (`<libproc.h>`, `<mach/mach.h>`,
  `struct proc_taskinfo`, `struct proc_bsdinfo`, `struct rusage_info_v4`,
  `vm_statistics64_data_t`). Struct field availability has drifted slightly
  across macOS SDK versions in the past (e.g. `rusage_info_v4` fields were
  added in 10.9+; newer SDKs sometimes rename constants). If `make` fails on
  a specific field, that field itself is a Stage 0 result worth recording
  ("X was not available on macOS <version>") — comment it out, note it, and
  keep going. Don't spend Stage 0 time chasing perfect portability.

---

## 4. Run the Target by hand (sanity check before running tests)

```bash
scripts/run-target.sh 9010
```

You should see lines like:

```
2026-09-09T11:00:03.823Z TARGET_START pid=12345 jvm=OpenJDK 64-Bit Server VM version=21.0.12
2026-09-09T11:00:03.840Z PHASE_BEGIN phase=NORMAL tick=0
```

Leave it running, and in another terminal:

```bash
java -jar observer-jvm/target/weave-observer-jvm.jar 127.0.0.1 9010 1000
```

You should see `OBSERVER_CONNECTED`, then `OBSERVER_DESCRIBE collectors=[...]`
— **write down what collector names your JVM actually reports.** On a
default macOS JDK 21 this will likely be `G1 Young Generation` /
`G1 Old Generation`; in the sandbox used to build this it was Serial GC's
`Copy` / `MarkSweepCompact`. The observer code does not care which — it
asks the platform for whatever `GarbageCollectorMXBean`s exist and reports
by name. That's the point of Test 1: record what's actually exposed, don't
assume.

Stop both with Ctrl-C when you're satisfied it works.

---

## 5. Run the five tests

Each script writes a timestamped folder under `results/`. Run them in
order; each one tells you at the end which files to look at and what to
compare.

```bash
# Test 1 — normal observation (default 90s)
scripts/test1_normal.sh 90 9010

# Test 2 — target failure, run BOTH modes
scripts/test2_target_failure.sh graceful 20 9010
scripts/test2_target_failure.sh abrupt   20 9010

# Test 3 — degradation across NORMAL/HIGH/EXTREME (default 240s, ~4 cycles)
scripts/test3_degradation.sh 240 9010

# Test 4 — observer impairment (JMX observer stresses itself; native/OS are the control)
scripts/test4_observer_impairment.sh 90 9010

# Test 5 — target restart / continuity (two independent Target lifetimes)
scripts/test5_restart_continuity.sh 30 9010
```

Run each of these a few times if you can — a single run tells you what
*can* happen; several runs start to tell you what's typical vs. what's
noise.

### Why the scripts don't do more than this

Per Stage-0 instructions: no reconnection logic, no smoothing, no
prediction, no dashboards. Each script starts processes, waits, kills
things, and hands you raw logs. All the actual "did we learn something"
work happens by reading the logs and filling in the comparison table
below — that's intentional, not a shortcut.

---

## 6. What each component measures, and why it's built that way

### Target (`weave-target`)

- A single allocation loop cycling **NORMAL → HIGH → EXTREME → repeat**,
  each phase running a fixed number of ticks (400) so the *sequence* of
  allocation work is deterministic even though wall-clock phase duration
  will vary by machine.
- Keeps a small bounded "live set" of recently allocated arrays per phase,
  so most allocations become garbage quickly but a live set persists —
  closer to a real workload than pure allocate-and-drop, and it exercises
  whatever generational/tenuring behavior the active collector has.
- JMX is enabled **only** via JVM flags at launch (see `run-target.sh`),
  not inside the program, so the Java Observer sees exactly what a normal
  remote-JMX configuration exposes.
- Registers a shutdown hook, so `TARGET_SHUTDOWN_HOOK` in its log appears
  **only** on graceful termination (SIGTERM/Ctrl-C), never on `kill -9` or
  a crash — this asymmetry is the ground truth for Test 2.

### Java/JMX Observer (`observer-jvm`)

- Connects via `javax.management.remote` only — never touches Target
  memory.
- Uses `ManagementFactory.getPlatformMXBeans(connection, X.class)` so it
  enumerates whatever `GarbageCollectorMXBean`s / `MemoryPoolMXBean`s the
  target JVM actually has, by name, with no hardcoded G1 assumptions.
- Reports **interval (delta)** metrics for cumulative counters (GC count,
  GC time, process CPU time), computed as `current - previous`, not raw
  cumulative values — see "Measurement discipline" below.
- No reconnect logic: on any `IOException` mid-sample it logs
  `OBSERVER_DISCONNECTED` with a timestamp and exits. That's Test 2's
  detection-latency data point.
- `--self-stress` flag (used only in Test 4) makes the observer burn CPU
  and allocate garbage in its own JVM between samples, to see whether an
  observer's own runtime can distort its own measurements.

### Native Observer (`observer-native`, C, macOS)

- Not a JVM. Uses:
  - `proc_pidinfo(PROC_PIDTASKINFO, ...)` — virtual/resident size, thread
    count, running-thread count, cumulative user/system CPU time (Mach
    ticks, converted to nanoseconds via `mach_timebase_info`), page
    faults, page-ins, copy-on-write faults, scheduling priority.
  - `proc_pid_rusage(RUSAGE_INFO_V4, ...)` — a second, independent
    BSD-rusage-style view: CPU time, resident size, physical footprint,
    page-ins, process start time.
  - `kill(pid, 0)` — explicitly distinguishes "process gone" (`ESRCH`)
    from "exists but I can't signal it" (`EPERM`); the latter is never
    silently treated as "healthy."
- Also resolves the target's executable path via `proc_pidpath` at
  startup, as a cheap identity check (useful if you ever see PID reuse
  during Test 5).
- What it explicitly cannot see: anything inside the JVM (GC events, heap
  pool detail, thread names) — that information lives inside the JVM and
  is only exposed via JMX (or far more intrusive techniques this project
  deliberately avoids).

### OS-level Observer (`observer-os`, C, macOS)

Three tiers, from least to most privileged, all attempted every sample:

1. **System-wide VM stats** (`host_statistics64(HOST_VM_INFO64)`) — always
   available to any user. Free/active/inactive/wired page counts,
   faults/pageins/pageouts, load average. **Not per-process** — this is
   the whole machine, so it's context, not attribution.
2. **Per-process BSD scheduler state** (`proc_pidinfo(PROC_PIDTBSDINFO)`)
   — usually available without root for your own processes. Actual
   scheduler state (`SRUN`/`SSLEEP`/`SSTOP`/`SZOMB`), nice value, process
   start time. This is genuinely new information neither JMX nor the
   native observer directly had (the native observer *inferred* liveness
   from `kill(pid,0)`; this is the scheduler's own view).
3. **Per-process Mach task info** (`task_for_pid` + `task_info`) — the
   classic "true OS-level" API: per-process page faults, IPC message
   counts, syscall counts. On modern macOS this is heavily restricted:
   expect `task_for_pid` to fail (`KERN_FAILURE`/`EPERM`) for an
   unprivileged, unentitled caller even against your own process. The
   observer logs this failure explicitly rather than skipping it silently
   — **"this exists but a normal observer can't reach it without root" is
   itself a Stage 0 result.**

**Run it once as your normal user and once with `sudo`, and diff the
logs** — whether tier 3 starts succeeding under `sudo` (and what that
costs you operationally, since Weave presumably shouldn't require root in
production) is directly relevant to the final architecture decision.

---

## 7. Measurement discipline (applies to every observer)

- **Absolute vs. interval metrics.** GC count and GC time are cumulative
  since JVM start. CPU time (JMX `getProcessCpuTime()`, native
  `pti_total_user`/`pti_total_system`, rusage `ri_user_time`) is
  cumulative since process start. Every observer here computes
  `delta = current - previous` for these and reports both the raw
  cumulative value and the delta — never treat a raw cumulative number as
  "this interval's value."
- **Monotonic clocks for elapsed time.** All interval/latency math uses
  `System.nanoTime()` (Java) or `CLOCK_MONOTONIC` (C). Wall-clock
  (`Instant.now()` / `CLOCK_REALTIME`) is recorded separately, only for
  cross-referencing logs against each other and against humans reading
  them.
- **Counters belong to a process lifetime, not to "the Target."** Test 5
  exists specifically to make this concrete: Target #2's GC count starts
  over from a small number, it is not continuous with Target #1's. Do not
  build anything downstream that assumes otherwise.

---

## 8. After running the tests: fill this in

Copy this table into your own notes after running Tests 1–5 at least once
each (ideally 2–3 times for Tests 2 and 5, since failure/restart behavior
is exactly where run-to-run variance matters most).

| Capability                | Java/JMX | Native | OS |
|----------------------------|----------|--------|----|
| JVM GC information         |          |        |    |
| JVM memory information      |          |        |    |
| Thread information          |          |        |    |
| Process CPU                |          |        |    |
| Scheduling information      |          |        |    |
| Memory pressure             |          |        |    |
| Page faults                 |          |        |    |
| I/O                         |          |        |    |
| Target failure detection    |          |        |    |
| Target restart awareness    |          |        |    |
| Sampling reliability        |          |        |    |
| Observer independence       |          |        |    |
| Measurement overhead        |          |        |    |
| Important limitations       |          |        |    |

Then answer, with evidence from your actual logs (not from what you'd
expect in theory):

1. What information can Java/JMX provide?
2. What information does Native add?
3. What information does OS-level observation add?
4. What information remains unavailable (at any level, without root, etc.)?
5. Which observation signals appear useful for Weave?
6. Which signals are noisy or misleading?
7. What observer failure modes exist (Test 4)?
8. What target failure modes exist (Test 2)?
9. What measurement limitations exist (resolution, overhead, privilege)?
10. **What is the minimum observation architecture Weave should carry
    forward?**

The honest answer might be "Java/JMX is sufficient" — that's a legitimate
Stage 0 conclusion, not a failure to justify the other two layers. Decide
based on what your logs actually show.

---

## 9. Explicit non-goals (per the Stage 0 brief)

No dashboards, no web app, no database, no microservices/Kubernetes, no
ML, no distributed tracing, no alerting platform, no complex config
system, and no automatic recovery/reconnection anywhere yet. Every
limitation you hit during these five tests is a **result to record**, not
a bug to immediately fix. Improvements are a Stage 1 conversation, after
you've decided — from evidence — which observation level(s) Weave actually
needs.
