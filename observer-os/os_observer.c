/*
 * Weave Stage 0 - Observation Level 3: OS-level Observer.
 *
 * Goal: find out what exists BELOW the process boundary that Java/JMX and
 * the native process observer could not get to, and be explicit about what
 * is and is not available WITHOUT elevated privileges - because "you need
 * sudo / an entitlement for this" is itself an important experimental
 * result about the cost of going lower.
 *
 * Three tiers of information are attempted, from least to most privileged:
 *
 *   (A) System-wide VM statistics via host_statistics64(HOST_VM_INFO64).
 *       Always available to any user. NOT per-process - this is the whole
 *       machine's page fault / page-in / page-out / free-memory picture.
 *       Useful as CONTEXT (is the whole box under memory pressure?) but it
 *       cannot attribute pressure to the Target specifically if anything
 *       else is running on the machine.
 *
 *   (B) Per-process state via sysctl(KERN_PROC, KERN_PROC_PID). Normally
 *       available without root for processes you own. Gives you the BSD
 *       scheduler's view of process state (running / sleeping / stopped /
 *       zombie), priority/nice, and start time - genuinely new information
 *       neither JMX nor the native observer directly exposed (the native
 *       observer inferred liveness from kill(pid,0); this gives the actual
 *       scheduler state).
 *
 *   (C) Per-process Mach task info via task_for_pid() + task_info(). This
 *       is the classic "true OS-level, below everything" API - per-process
 *       page fault counts, suspend count, IPC message counts. On modern
 *       macOS this call is heavily restricted: even for a process you own,
 *       an unentitled, non-root caller will typically get EPERM/KERN_FAILURE
 *       here. We attempt it anyway and report the failure explicitly,
 *       because "this exists but is not accessible to a normal observer"
 *       is exactly the kind of boundary Stage 0 is supposed to map.
 *
 * Build:  make
 * Run:    ./os_observer <pid> <interval_ms> [sample_count]
 * Root:   run once as your normal user, then run again with `sudo` and
 *         diff the two logs - that comparison IS one of the Stage 0
 *         results (see README.md).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_time.h>

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

/* (A) System-wide VM stats. Always available. */
static void sample_system_vm(long sample_index) {
    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                          (host_info64_t) &vmstat, &count);
    if (kr != KERN_SUCCESS) {
        print_timestamp(stdout);
        printf("OS_SAMPLE_SYSTEM_VM_FAILED idx=%ld kern_return=%d\n", sample_index, kr);
        return;
    }

    double loadavg[3] = {0, 0, 0};
    getloadavg(loadavg, 3);

    print_timestamp(stdout);
    printf("OS_SAMPLE_SYSTEM_VM idx=%ld "
           "freePages=%u activePages=%u inactivePages=%u wirePages=%u "
           "faults=%u pageins=%u pageouts=%u "
           "loadavg1=%.2f loadavg5=%.2f loadavg15=%.2f\n",
           sample_index,
           vmstat.free_count, vmstat.active_count, vmstat.inactive_count, vmstat.wire_count,
           vmstat.faults, vmstat.pageins, vmstat.pageouts,
           loadavg[0], loadavg[1], loadavg[2]);
}

/* (B) Per-process BSD scheduler state via libproc's PROC_PIDTBSDINFO.
 * Usually no root needed for processes you own. We use libproc's
 * proc_bsdinfo here (rather than raw sysctl(KERN_PROC) / kinfo_proc)
 * because its layout is the one Apple documents and keeps stable in
 * <libproc.h>; kinfo_proc's exact layout is explicitly "may change between
 * releases" per Apple's own headers. */
static int sample_process_state(pid_t pid, long sample_index) {
    struct proc_bsdinfo bsdinfo;
    int ret = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsdinfo, sizeof(bsdinfo));

    if (ret != sizeof(bsdinfo)) {
        /* proc_pidinfo returns 0 (not an errno-style failure) both when the
         * pid does not exist and, sometimes, when permission is denied.
         * We cannot always tell those apart from this call alone - which
         * is itself a documented Stage 0 finding. */
        print_timestamp(stdout);
        printf("OS_SAMPLE_PROC_STATE_UNAVAILABLE idx=%ld pid=%d ret=%d errno=%d (%s)\n",
               sample_index, pid, ret, errno, strerror(errno));
        return (ret == 0) ? 0 : -1;
    }

    const char *state_name;
    switch (bsdinfo.pbi_status) {
        case SIDL:   state_name = "SIDL(being created)"; break;
        case SRUN:   state_name = "SRUN(runnable)"; break;
        case SSLEEP: state_name = "SSLEEP(sleeping)"; break;
        case SSTOP:  state_name = "SSTOP(stopped)"; break;
        case SZOMB:  state_name = "SZOMB(zombie/exited-not-reaped)"; break;
        default:     state_name = "UNKNOWN"; break;
    }

    print_timestamp(stdout);
    printf("OS_SAMPLE_PROC_STATE idx=%ld pid=%d state=%s nice=%d "
           "startTimeSec=%llu\n",
           sample_index, pid, state_name,
           bsdinfo.pbi_nice,
           (unsigned long long) bsdinfo.pbi_start_tvsec);
    return 1;
}

/* (C) Per-process Mach task info. Usually requires root or an entitlement
 * on modern macOS, even for a process you own. We attempt it and report
 * whatever happens - success OR failure is a valid Stage 0 result. */
static void sample_task_info(pid_t pid, long sample_index) {
    task_t task;
    kern_return_t kr = task_for_pid(mach_task_self(), pid, &task);
    if (kr != KERN_SUCCESS) {
        print_timestamp(stdout);
        printf("OS_SAMPLE_TASK_FOR_PID_DENIED idx=%ld pid=%d kern_return=%d "
               "(expected without root/entitlement on modern macOS)\n",
               sample_index, pid, kr);
        return;
    }

    task_events_info_data_t events;
    mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
    kr = task_info(task, TASK_EVENTS_INFO, (task_info_t) &events, &count);
    if (kr != KERN_SUCCESS) {
        print_timestamp(stdout);
        printf("OS_SAMPLE_TASK_EVENTS_INFO_FAILED idx=%ld pid=%d kern_return=%d\n",
               sample_index, pid, kr);
        mach_port_deallocate(mach_task_self(), task);
        return;
    }

    print_timestamp(stdout);
    printf("OS_SAMPLE_TASK_EVENTS idx=%ld pid=%d faults=%d pageins=%d cowFaults=%d "
           "messagesSent=%d messagesReceived=%d syscallsMach=%d syscallsUnix=%d\n",
           sample_index, pid, events.faults, events.pageins, events.cow_faults,
           events.messages_sent, events.messages_received,
           events.syscalls_mach, events.syscalls_unix);

    mach_port_deallocate(mach_task_self(), task);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <pid> <interval_ms> [sample_count]\n", argv[0]);
        return 2;
    }
    pid_t target_pid = (pid_t) atoi(argv[1]);
    long interval_ms = atol(argv[2]);
    long sample_count = (argc > 3) ? atol(argv[3]) : 0;

    print_timestamp(stdout);
    printf("OS_OBSERVER_START pid=%d intervalMs=%ld sampleCount=%ld euid=%d "
           "(euid==0 means running as root/sudo)\n",
           target_pid, interval_ms, sample_count, geteuid());

    long sample_index = 0;
    while (sample_count == 0 || sample_index < sample_count) {
        sample_system_vm(sample_index);
        int state = sample_process_state(target_pid, sample_index);
        sample_task_info(target_pid, sample_index);

        if (state == 0) {
            print_timestamp(stdout);
            printf("OS_OBSERVER_TARGET_GONE pid=%d sampleIndex=%ld\n", target_pid, sample_index);
            break;
        }

        sample_index++;
        struct timespec req;
        req.tv_sec = interval_ms / 1000;
        req.tv_nsec = (interval_ms % 1000) * 1000000L;
        nanosleep(&req, NULL);
    }

    print_timestamp(stdout);
    printf("OS_OBSERVER_EXIT sampleIndex=%ld\n", sample_index);
    return 0;
}
