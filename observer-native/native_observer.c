/*
 * Weave Stage 0 - Observation Level 2: Native Observer.
 *
 * A small, standalone C program (NOT a JVM) that observes a Target process
 * purely from the outside, using macOS process-level APIs:
 *
 *   - libproc's proc_pidinfo(PROC_PIDTASKINFO, ...) for per-process
 *     virtual/resident size, thread count, page faults, page-ins, and
 *     cumulative user/system CPU time (in Mach absolute time units).
 *   - proc_pid_rusage(RUSAGE_INFO_V4) for a second, independent view of
 *     CPU time, resident size, and page-ins (BSD rusage-style accounting).
 *   - kill(pid, 0) to distinguish "process not found" (ESRCH) from
 *     "process exists but I lack permission" (EPERM) - this distinction
 *     matters directly for Test 2 (Target failure).
 *
 * Deliberately does NOT attempt to reproduce the JMX observer's view (GC
 * counts, heap pools, etc.) - those live inside the JVM and are not visible
 * to a process-external observer without going back through JMX or
 * something far more intrusive (e.g. reading JVM-internal memory, which we
 * explicitly do not do). What THIS observer measures is exactly the
 * process-level information the OS is willing to hand out without
 * elevated privileges.
 *
 * Build:
 *   make            (see Makefile in this directory)
 *
 * Run:
 *   ./native_observer <pid> <interval_ms> [sample_count]
 *
 *   sample_count = 0 or omitted means "run until the target disappears or
 *   the observer is killed".
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libproc.h>
#include <mach/mach_time.h>

static mach_timebase_info_data_t g_timebase;

static uint64_t mach_ticks_to_nanos(uint64_t ticks) {
    return ticks * g_timebase.numer / g_timebase.denom;
}

/* Wall-clock timestamp string, ISO-8601-ish, for cross-referencing logs
 * across the Target / JMX observer / native observer / OS observer. */
static void print_timestamp(FILE *out) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    fprintf(out, "%s.%03ldZ ", buf, ts.tv_nsec / 1000000);
}

static uint64_t monotonic_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/*
 * Returns:
 *   1  -> process exists and is (at least nominally) accessible
 *   0  -> process does not exist (ESRCH) - a confident "target is gone"
 *  -1  -> process exists but we cannot signal-check it in a meaningful way
 *         (EPERM) - NOTE: on macOS this is uncommon for kill(pid,0) across
 *         same-user processes, but we check it explicitly anyway, because
 *         "cannot observe" must never be silently treated as "is healthy".
 */
static int process_liveness(pid_t pid) {
    if (kill(pid, 0) == 0) {
        return 1;
    }
    if (errno == ESRCH) {
        return 0;
    }
    if (errno == EPERM) {
        return -1;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> <interval_ms> [sample_count]\n", argv[0]);
        return 2;
    }

    pid_t target_pid = (pid_t) atoi(argv[1]);
    long interval_ms = atol(argv[2]);
    long sample_count = (argc > 3) ? atol(argv[3]) : 0; /* 0 = unbounded */

    mach_timebase_info(&g_timebase);

    print_timestamp(stdout);
    printf("NATIVE_OBSERVER_START pid=%d intervalMs=%ld sampleCount=%ld\n",
           target_pid, interval_ms, sample_count);

    /* proc_pidpath is a cheap existence + identity check: if the target pid
     * gets reused by an unrelated process (Test 5 territory if you are not
     * careful with PID reuse), the *path* tells you it's not the same
     * program anymore even though the PID number matches. */
    char pathbuf[PROC_PIDPATHINFO_MAXSIZE];
    int pathlen = proc_pidpath(target_pid, pathbuf, sizeof(pathbuf));
    if (pathlen > 0) {
        print_timestamp(stdout);
        printf("NATIVE_OBSERVER_TARGET_PATH pid=%d path=%s\n", target_pid, pathbuf);
    } else {
        print_timestamp(stdout);
        printf("NATIVE_OBSERVER_TARGET_PATH_UNAVAILABLE pid=%d errno=%d (%s)\n",
               target_pid, errno, strerror(errno));
    }

    long sample_index = 0;
    uint64_t prev_user_nanos = 0, prev_sys_nanos = 0;
    int have_prev = 0;
    uint64_t prev_sample_nanos = 0;

    while (sample_count == 0 || sample_index < sample_count) {
        int liveness = process_liveness(target_pid);
        if (liveness == 0) {
            print_timestamp(stdout);
            printf("NATIVE_OBSERVER_TARGET_GONE pid=%d sampleIndex=%ld (ESRCH via kill(pid,0))\n",
                   target_pid, sample_index);
            break;
        }
        if (liveness == -1) {
            print_timestamp(stdout);
            printf("NATIVE_OBSERVER_TARGET_UNCERTAIN pid=%d sampleIndex=%ld errno=%d (%s)\n",
                   target_pid, sample_index, errno, strerror(errno));
            /* Do NOT treat this as "target is dead". Keep trying. */
        }

        struct proc_taskinfo tinfo;
        int ti_ret = proc_pidinfo(target_pid, PROC_PIDTASKINFO, 0, &tinfo, sizeof(tinfo));

        struct rusage_info_v4 ru;
        int ru_ret = proc_pid_rusage(target_pid, RUSAGE_INFO_V4, (rusage_info_t *) &ru);

        uint64_t now_nanos = monotonic_nanos();

        if (ti_ret == sizeof(tinfo)) {
            uint64_t user_nanos = mach_ticks_to_nanos(tinfo.pti_total_user);
            uint64_t sys_nanos = mach_ticks_to_nanos(tinfo.pti_total_system);

            long long delta_user_nanos = -1, delta_sys_nanos = -1;
            double interval_seconds = -1;
            if (have_prev) {
                delta_user_nanos = (long long) user_nanos - (long long) prev_user_nanos;
                delta_sys_nanos = (long long) sys_nanos - (long long) prev_sys_nanos;
                interval_seconds = (now_nanos - prev_sample_nanos) / 1e9;
            }
            prev_user_nanos = user_nanos;
            prev_sys_nanos = sys_nanos;
            have_prev = 1;

            print_timestamp(stdout);
            printf("NATIVE_SAMPLE idx=%ld pid=%d "
                   "virtualSize=%llu residentSize=%llu threadCount=%d runningThreads=%d "
                   "userTimeNanos=%llu sysTimeNanos=%llu "
                   "deltaUserNanos=%lld deltaSysNanos=%lld intervalSeconds=%.4f "
                   "faults=%d pageins=%d cowFaults=%d priority=%d\n",
                   sample_index, target_pid,
                   (unsigned long long) tinfo.pti_virtual_size,
                   (unsigned long long) tinfo.pti_resident_size,
                   tinfo.pti_threadnum, tinfo.pti_numrunning,
                   (unsigned long long) user_nanos, (unsigned long long) sys_nanos,
                   delta_user_nanos, delta_sys_nanos, interval_seconds,
                   tinfo.pti_faults, tinfo.pti_pageins, tinfo.pti_cow_faults,
                   tinfo.pti_priority);
        } else {
            print_timestamp(stdout);
            printf("NATIVE_SAMPLE_TASKINFO_FAILED idx=%ld pid=%d ret=%d errno=%d (%s)\n",
                   sample_index, target_pid, ti_ret, errno, strerror(errno));
        }

        if (ru_ret == 0) {
            print_timestamp(stdout);
            printf("NATIVE_SAMPLE_RUSAGE idx=%ld pid=%d "
                   "ruUserTimeNanos=%llu ruSystemTimeNanos=%llu "
                   "ruResidentSize=%llu ruPhysFootprint=%llu "
                   "ruPageins=%llu ruProcStartAbsTime=%llu\n",
                   sample_index, target_pid,
                   (unsigned long long) ru.ri_user_time,
                   (unsigned long long) ru.ri_system_time,
                   (unsigned long long) ru.ri_resident_size,
                   (unsigned long long) ru.ri_phys_footprint,
                   (unsigned long long) ru.ri_pageins,
                   (unsigned long long) ru.ri_proc_start_abstime);
        } else {
            print_timestamp(stdout);
            printf("NATIVE_SAMPLE_RUSAGE_FAILED idx=%ld pid=%d ret=%d errno=%d (%s)\n",
                   sample_index, target_pid, ru_ret, errno, strerror(errno));
        }

        prev_sample_nanos = now_nanos;
        sample_index++;

        struct timespec req;
        req.tv_sec = interval_ms / 1000;
        req.tv_nsec = (interval_ms % 1000) * 1000000L;
        nanosleep(&req, NULL);
    }

    print_timestamp(stdout);
    printf("NATIVE_OBSERVER_EXIT sampleIndex=%ld\n", sample_index);
    return 0;
}
