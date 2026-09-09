package weave.observer.jvm;

import javax.management.MBeanServerConnection;
import javax.management.remote.JMXConnector;
import javax.management.remote.JMXConnectorFactory;
import javax.management.remote.JMXServiceURL;
import java.io.IOException;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.lang.management.MemoryMXBean;
import java.lang.management.MemoryUsage;
import java.lang.management.ThreadMXBean;
import java.time.Instant;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;

import com.sun.management.OperatingSystemMXBean;

/**
 * Weave Stage 0 - Observation Level 1: Java / JMX Observer.
 *
 * Runs as a completely separate JVM from the Target. Connects only through
 * the standard JMX remote management interface, i.e. it never touches the
 * Target's memory directly - everything here comes through
 * javax.management.remote.
 *
 * Deliberately simple, on purpose (per Stage 0 instructions):
 *   - no reconnect logic
 *   - no prediction
 *   - no smoothing / filtering
 *   - if the connection breaks, we log it and stop
 *
 * Generic over collector/pool names: we ask the platform for whatever
 * GarbageCollectorMXBeans and MemoryPoolMXBeans it actually exposes, and we
 * do not assume G1, Serial, Parallel, or any other specific collector.
 *
 * Usage:
 *   java -jar weave-observer-jvm.jar <jmxHost> <jmxPort> <intervalMillis> [--self-stress]
 *
 * `--self-stress` is the Test-4 (observer impairment) switch: it makes the
 * observer itself burn CPU and allocate garbage between samples, so we can
 * see whether the observer's OWN runtime distorts its measurements.
 */
public final class JmxObserver {

    // previous cumulative values, keyed by collector/pool name, so we can
    // report INTERVAL (delta) metrics, not raw cumulative ones.
    private static final Map<String, Long> prevCollectionCount = new HashMap<>();
    private static final Map<String, Long> prevCollectionTime = new HashMap<>();
    private static long prevProcessCpuTimeNanos = -1;
    private static long prevSampleNanoTime = -1;

    public static void main(String[] args) throws Exception {
        if (args.length < 3) {
            System.err.println("usage: JmxObserver <jmxHost> <jmxPort> <intervalMillis> [--self-stress]");
            System.exit(2);
        }
        String host = args[0];
        int port = Integer.parseInt(args[1]);
        long intervalMillis = Long.parseLong(args[2]);
        boolean selfStress = args.length > 3 && args[3].equals("--self-stress");

        String urlPath = "service:jmx:rmi:///jndi/rmi://" + host + ":" + port + "/jmxrmi";
        log("OBSERVER_START url=" + urlPath + " intervalMillis=" + intervalMillis + " selfStress=" + selfStress);

        JMXServiceURL url = new JMXServiceURL(urlPath);
        long connectAttemptStart = System.nanoTime();
        JMXConnector connector;
        try {
            connector = JMXConnectorFactory.connect(url);
        } catch (IOException e) {
            log("OBSERVER_CONNECT_FAILED error=" + e.getMessage());
            System.exit(1);
            return;
        }
        long connectAttemptNanos = System.nanoTime() - connectAttemptStart;
        log("OBSERVER_CONNECTED connectLatencyMicros=" + (connectAttemptNanos / 1000));

        MBeanServerConnection mbsc = connector.getMBeanServerConnection();

        // --- Test 1 material: enumerate what this JVM actually exposes ---
        describeOnce(mbsc);

        // Self-monitoring beans (this observer's OWN JVM), used only for Test 4.
        ThreadMXBean selfThreads = ManagementFactory.getThreadMXBean();
        MemoryMXBean selfMemory = ManagementFactory.getMemoryMXBean();
        Random rng = new Random(7);
        java.util.ArrayDeque<byte[]> selfGarbage = new java.util.ArrayDeque<>();

        int sampleIndex = 0;
        while (true) {
            long sampleStartNanos = System.nanoTime();
            Instant wallNow = Instant.now();
            try {
                sample(mbsc, sampleIndex, wallNow, sampleStartNanos);
            } catch (IOException e) {
                // This is Test 2's key moment: the Target disappeared (or the
                // connection otherwise broke). We log it and STOP. We do not
                // try to reconnect - reconnection behavior is a later-stage
                // concern, not a Stage-0 baseline concern.
                long detectedAtNanos = System.nanoTime();
                log("OBSERVER_DISCONNECTED sampleIndex=" + sampleIndex
                        + " wallTime=" + Instant.now()
                        + " error=" + e.getClass().getSimpleName() + ":" + e.getMessage());
                break;
            }

            if (selfStress) {
                // Deliberately impair ourselves: burn CPU and allocate
                // garbage in THIS process, between samples, to see whether
                // it distorts our own sampling cadence / measurements.
                for (int i = 0; i < 200_000; i++) {
                    rng.nextDouble();
                }
                byte[] garbage = new byte[512_000];
                selfGarbage.addLast(garbage);
                if (selfGarbage.size() > 20) {
                    selfGarbage.removeFirst();
                }
                long selfHeapUsed = selfMemory.getHeapMemoryUsage().getUsed();
                int selfThreadCount = selfThreads.getThreadCount();
                log("OBSERVER_SELF sampleIndex=" + sampleIndex
                        + " selfHeapUsedBytes=" + selfHeapUsed
                        + " selfThreadCount=" + selfThreadCount);
            }

            sampleIndex++;

            long elapsedMillis = (System.nanoTime() - sampleStartNanos) / 1_000_000;
            long sleepMillis = intervalMillis - elapsedMillis;
            long sleepGapMillis = -sleepMillis; // negative sleepMillis means we overran the interval
            if (sleepGapMillis > 0) {
                log("OBSERVER_OVERRUN sampleIndex=" + sampleIndex + " overrunMillis=" + sleepGapMillis);
            }
            if (sleepMillis > 0) {
                Thread.sleep(sleepMillis);
            }
        }

        try {
            connector.close();
        } catch (IOException ignored) {
            // already broken; nothing more to do
        }
        log("OBSERVER_EXIT");
    }

    private static void describeOnce(MBeanServerConnection mbsc) throws Exception {
        List<GarbageCollectorMXBean> gcBeans =
                ManagementFactory.getPlatformMXBeans(mbsc, GarbageCollectorMXBean.class);
        StringBuilder gcNames = new StringBuilder();
        for (GarbageCollectorMXBean b : gcBeans) {
            if (gcNames.length() > 0) gcNames.append(",");
            gcNames.append(b.getName());
        }
        log("OBSERVER_DESCRIBE collectors=[" + gcNames + "]");

        List<java.lang.management.MemoryPoolMXBean> poolBeans =
                ManagementFactory.getPlatformMXBeans(mbsc, java.lang.management.MemoryPoolMXBean.class);
        StringBuilder poolNames = new StringBuilder();
        for (java.lang.management.MemoryPoolMXBean b : poolBeans) {
            if (poolNames.length() > 0) poolNames.append(",");
            poolNames.append(b.getName()).append("(").append(b.getType()).append(")");
        }
        log("OBSERVER_DESCRIBE pools=[" + poolNames + "]");

        java.lang.management.RuntimeMXBean runtime =
                ManagementFactory.getPlatformMXBean(mbsc, java.lang.management.RuntimeMXBean.class);
        log("OBSERVER_DESCRIBE targetJvm=" + runtime.getVmName()
                + " targetVersion=" + runtime.getVmVersion()
                + " targetPid=" + runtime.getName()); // name is typically "<pid>@host"
    }

    private static void sample(MBeanServerConnection mbsc, int sampleIndex, Instant wallNow, long nowNanos)
            throws IOException {

        List<GarbageCollectorMXBean> gcBeans;
        List<java.lang.management.MemoryPoolMXBean> poolBeans;
        ThreadMXBean threadBean;
        OperatingSystemMXBean osBean;
        MemoryMXBean memBean;
        try {
            gcBeans = ManagementFactory.getPlatformMXBeans(mbsc, GarbageCollectorMXBean.class);
            poolBeans = ManagementFactory.getPlatformMXBeans(mbsc, java.lang.management.MemoryPoolMXBean.class);
            threadBean = ManagementFactory.getPlatformMXBean(mbsc, ThreadMXBean.class);
            osBean = ManagementFactory.getPlatformMXBean(mbsc, OperatingSystemMXBean.class);
            memBean = ManagementFactory.getPlatformMXBean(mbsc, MemoryMXBean.class);
        } catch (java.io.UncheckedIOException e) {
            // getPlatformMXBeans wraps connection failures as UncheckedIOException
            throw (IOException) e.getCause();
        }

        StringBuilder gcLine = new StringBuilder();
        for (GarbageCollectorMXBean b : gcBeans) {
            String name = b.getName();
            long count = b.getCollectionCount(); // cumulative, ABSOLUTE metric
            long time = b.getCollectionTime();    // cumulative millis, ABSOLUTE metric

            long deltaCount = prevCollectionCount.containsKey(name) ? count - prevCollectionCount.get(name) : 0;
            long deltaTime = prevCollectionTime.containsKey(name) ? time - prevCollectionTime.get(name) : 0;
            prevCollectionCount.put(name, count);
            prevCollectionTime.put(name, time);

            gcLine.append(name).append(":count=").append(count)
                    .append(",deltaCount=").append(deltaCount)
                    .append(",timeMs=").append(time)
                    .append(",deltaTimeMs=").append(deltaTime)
                    .append(";");
        }

        MemoryUsage heap = memBean.getHeapMemoryUsage();
        MemoryUsage nonHeap = memBean.getNonHeapMemoryUsage();

        double processCpuLoad = osBean.getProcessCpuLoad(); // 0.0-1.0, or -1 if unavailable
        long processCpuTimeNanos = osBean.getProcessCpuTime(); // cumulative, ABSOLUTE, or -1

        long deltaCpuNanos = -1;
        double intervalSeconds = -1;
        if (prevProcessCpuTimeNanos >= 0 && processCpuTimeNanos >= 0) {
            deltaCpuNanos = processCpuTimeNanos - prevProcessCpuTimeNanos;
            intervalSeconds = (nowNanos - prevSampleNanoTime) / 1_000_000_000.0;
        }
        prevProcessCpuTimeNanos = processCpuTimeNanos;
        prevSampleNanoTime = nowNanos;

        int threadCount = threadBean.getThreadCount();
        int peakThreadCount = threadBean.getPeakThreadCount();

        log("SAMPLE idx=" + sampleIndex + " wall=" + wallNow
                + " heapUsed=" + heap.getUsed() + " heapCommitted=" + heap.getCommitted()
                + " nonHeapUsed=" + nonHeap.getUsed()
                + " threadCount=" + threadCount + " peakThreadCount=" + peakThreadCount
                + " processCpuLoad=" + processCpuLoad
                + " processCpuTimeNanos=" + processCpuTimeNanos
                + " deltaCpuNanos=" + deltaCpuNanos
                + " intervalSeconds=" + intervalSeconds
                + " gc=[" + gcLine + "]");
    }

    private static void log(String message) {
        System.out.println(Instant.now() + " " + message);
    }
}
