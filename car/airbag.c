// airbag.c - airbag monitor actor (safety-critical, second priority on core 1)
//
// Core 1 (safety core), SCHED_FIFO priority 62, 10 ms period, 1 ms of work.
// Shares the safety core with ABS (63), which always preempts it.
// Not registered with the task manager, so the TM policy can never demote it.
// Timing/workload demonstration only, not a real airbag controller.
//
// Run as root:  ./airbag      2000 cycles (20 s)
//               ./airbag 0    run until SIGINT/SIGTERM

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define AIRBAG_PRIO    62                   // just below ABS
#define AIRBAG_PERIOD  (10 * NS_PER_MS)     // 10 ms period
#define AIRBAG_WORK    (1 * NS_PER_MS)      // simulated work per period
#define AIRBAG_MISS    (1 * NS_PER_MS)      // wake-up later than 1 ms = miss

static volatile sig_atomic_t stop = 0;      // set by SIGINT/SIGTERM

static void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char *argv[])
{
    // Optional argument: number of cycles, 0 = forever.
    long cycles = 2000;
    if (argc > 1)
        cycles = atol(argv[1]);

    // Clean stop on SIGINT/SIGTERM so the summary is printed.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Pin to the safety core, FIFO 62.
    if (actor_setup(RMASK_SAFETY, SCHED_FIFO, AIRBAG_PRIO) == -1)
        return 1;

    // Periodic 10 ms timer pulse at our own priority.
    actor_timer_t timer;
    if (actor_timer_start(&timer, AIRBAG_PERIOD, AIRBAG_PRIO) == -1)
        return 1;

    lat_stats_t stats;
    lat_init(&stats, AIRBAG_MISS);

    printf("airbag: pid %d running on safety core, FIFO %d, %ld cycles\n",
           getpid(), AIRBAG_PRIO, cycles);
    fflush(stdout);

    // Loop: wait for the tick, record lateness, do 1 ms of work.
    while (!stop && (cycles == 0 || stats.count < (uint64_t)cycles)) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR)
                continue;               // signal: loop condition checks 'stop'
            perror("MsgReceivePulse");
            break;
        }

        lat_add(&stats, late);
        actor_spin_ns(AIRBAG_WORK, &stop);
    }

    // Release the timer and print the timing summary.
    actor_timer_stop(&timer);
    lat_print("airbag", &stats);
    return 0;
}
