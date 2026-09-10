package weave.target;

import java.lang.management.ManagementFactory;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.Random;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Weave Stage 0 - Target JVM.
 *
 * This is the system-under-observation. It is deliberately simple:
 *   - runs a single allocation loop with three phases: NORMAL, HIGH, EXTREME
 *   - the *sequence* of allocations is deterministic (fixed seed, fixed tick
 *     counts per phase), even though wall-clock timing will vary machine to
 *     machine
 *   - it keeps a short-lived "live set" so allocated objects are not
 *     instantly garbage (closer to a real workload) and then drops them so
 *     the collector actually has work to do
 *   - it registers a shutdown hook so we can distinguish graceful shutdown
 *     (SIGTERM / Ctrl-C handled) from abrupt termination (SIGKILL, crash)
 *
 * JMX is NOT configured inside this program. It is enabled with JVM flags
 * at launch time (see scripts/run-target.sh and README.md). This keeps the
 * "observation surface" honest: what the Java Observer sees is exactly what
 * the standard platform MBeans expose, nothing custom-built to make the
 * experiment easier.
 */
public final class Target {

    private enum Phase {
        NORMAL(1_000, 2_000, 200),
        HIGH(8_000, 4_000, 60),
        EXTREME(32_000, 8_000, 15);

        final int objectsPerTick;   // how many small objects to allocate this tick
        final int arraySize;        // size in longs of each "chunk" object
        final int liveSetCap;       // how many recent chunks we keep alive (rest become garbage)

        Phase(int objectsPerTick, int arraySize, int liveSetCap) {
            this.objectsPerTick = objectsPerTick;
            this.arraySize = arraySize;
            this.liveSetCap = liveSetCap;
        }
    }

    private static final Phase[] PHASE_ORDER = { Phase.NORMAL, Phase.HIGH, Phase.EXTREME };
    private static final int TICKS_PER_PHASE = 400;
    private static final long TICK_SLEEP_MILLIS = 50; // nominal; not guaranteed by the JVM

    private static final long SEED = 42L; // deterministic allocation content

    private static final AtomicBoolean shuttingDown = new AtomicBoolean(false);
    private static final AtomicReference<Phase> currentPhase = new AtomicReference<>(Phase.NORMAL);

    public static void main(String[] args) throws Exception {
        long pid = ProcessHandle.current().pid();
        log("TARGET_START pid=" + pid
                + " jvm=" + System.getProperty("java.vm.name")
                + " version=" + System.getProperty("java.version"));

        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            shuttingDown.set(true);
            log("TARGET_SHUTDOWN_HOOK pid=" + pid + " phaseAtShutdown=" + currentPhase.get());
        }, "target-shutdown-hook"));

        Random rng = new Random(SEED);
        Deque<long[]> liveSet = new ArrayDeque<>();
        long tick = 0;

        outer:
        while (true) {
            for (Phase phase : PHASE_ORDER) {
                currentPhase.set(phase);
                log("PHASE_BEGIN phase=" + phase + " tick=" + tick);
                for (int t = 0; t < TICKS_PER_PHASE; t++) {
                    if (shuttingDown.get()) {
                        break outer;
                    }
                    runTick(phase, rng, liveSet);
                    tick++;
                    Thread.sleep(TICK_SLEEP_MILLIS);
                }
                log("PHASE_END phase=" + phase + " tick=" + tick);
            }
            // loop forever through NORMAL -> HIGH -> EXTREME -> NORMAL ...
        }

        log("TARGET_MAIN_LOOP_EXIT pid=" + pid + " lastTick=" + tick);
    }

    private static void runTick(Phase phase, Random rng, Deque<long[]> liveSet) {
        for (int i = 0; i < phase.objectsPerTick; i++) {
            long[] chunk = new long[phase.arraySize];
            // Touch the memory (deterministically) so it is not optimized
            // away and so pages are actually resident, not just reserved.
            for (int j = 0; j < chunk.length; j += 64) {
                chunk[j] = rng.nextLong();
            }
            liveSet.addLast(chunk);
        }
        // Evict old entries so most allocations become garbage quickly,
        // but a bounded "live set" persists (closer to a real workload
        // than pure allocate-and-drop, and it exercises the old
        // generation / tenuring behavior of whatever collector is active).
        while (liveSet.size() > phase.liveSetCap) {
            liveSet.removeFirst();
        }
    }

    private static void log(String message) {
        System.out.println(Instant.now() + " " + message);
    }
}
