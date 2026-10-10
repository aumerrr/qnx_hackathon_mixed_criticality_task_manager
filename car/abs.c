// abs.c - ABS brake actor (safety-critical, highest priority in the system)
//
// Core 1 (safety core), SCHED_FIFO priority 63, 5 ms period.
// Every period it wakes on a kernel timer pulse and records how late it woke.
//
// Safety design:
//   - Priority 63 on the safety core: no comfort job can preempt it.
//   - Never sends a message to the task manager. MsgSend blocks until a reply,
//     so a dead or stuck task manager can never stall the brake loop.
//   - No printf inside the loop; results are printed once, after the loop.
//
// Run as root:  ./abs      2000 cycles (10 s)
//               ./abs 0    run until SIGINT/SIGTERM

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define ABS_PRIO    63                  // highest non-privileged priority
#define ABS_PERIOD  (5 * NS_PER_MS)     // 5 ms control period
#define ABS_MISS    (1 * NS_PER_MS)     // wake-up later than 1 ms = deadline miss

// Set by the signal handler, read by the loop (sig_atomic_t = safe for this).
volatile sig_atomic_t stop = 0;

// SIGINT/SIGTERM: only set the flag, the loop exits and prints the summary.
void on_signal(int sig)
{
    stop = 1;
}

int main(int argc, char *argv[])
{
    // Optional argument: number of cycles, 0 = forever.
    long cycles = 2000;
    if (argc > 1) cycles = atol(argv[1]);

    // Catch SIGINT/SIGTERM so we stop cleanly and still print statistics.
    // The signal also interrupts MsgReceivePulse with EINTR.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. Pin to the safety core and set FIFO 63 before any timing starts.
    if (actor_setup(RMASK_SAFETY, SCHED_FIFO, ABS_PRIO) == -1) return 1;

    // 2. Periodic 5 ms timer that delivers a pulse at priority 63.
    actor_timer_t timer;
    if (actor_timer_start(&timer, ABS_PERIOD, ABS_PRIO) == -1) return 1;

    // 3. Lateness statistics: min / avg / max / misses.
    lat_stats_t stats;
    lat_init(&stats, ABS_MISS);

    printf("abs: pid %d running, %ld cycles\n", getpid(), cycles);
    fflush(stdout);

    // 4. Control loop: block until the next tick, then record its lateness.
    while (!stop && (cycles == 0 || stats.count < cycles)) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;   // signal: loop condition checks 'stop'
            perror("MsgReceivePulse");
            break;                          // real error: exit instead of spinning
        }                                   // at priority 63 on the safety core
        lat_add(&stats, late);
    }

    // 5. Release the timer and print the one-line timing summary.
    actor_timer_stop(&timer);
    lat_print("abs", &stats);
    return 0;
}
