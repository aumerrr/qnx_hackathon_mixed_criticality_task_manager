// nav.c - navigation actor (comfort job, managed by the task manager)
//
// Core 2 (comfort pool), SCHED_RR priority 12, 100 ms period, 20 ms of work
// (about 20% of one core). Registered with the task manager, budget 30%.
//
// Role in the demo: the victim. It shares core 2 with the decoder (15).
//   - Decoder storm at 15: nav (12) cannot run, its frames start late (misses).
//   - Task manager demotes the decoder to 2: nav runs first again.
//
// Run as root:  ./nav

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define NAV_NAME    "nav"
#define NAV_PRIO    12                  // below decoder (15), above ui (10)
#define NAV_BUDGET  30                  // % of one core before the TM steps in
#define NAV_PERIOD  (100 * NS_PER_MS)   // one route update every 100 ms
#define NAV_WORK    (20 * NS_PER_MS)    // work per update
#define NAV_MISS    (20 * NS_PER_MS)    // frame started > 20 ms late = miss

volatile sig_atomic_t stop = 0;         // set by SIGINT/SIGTERM

void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

int main(void)
{
    // Clean stop on SIGINT/SIGTERM so the summary is printed.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. Pin to the comfort core, RR 12.
    if (actor_setup(RMASK_POOL, SCHED_RR, NAV_PRIO) == -1) return 1;

    // 2. Periodic 100 ms timer pulse.
    actor_timer_t timer;
    if (actor_timer_start(&timer, NAV_PERIOD, NAV_PRIO) == -1) return 1;

    // 3. Lateness statistics.
    lat_stats_t stats;
    lat_init(&stats, NAV_MISS);

    // 4. Register with the task manager (failure is fine: report() retries).
    tm_link_t tm;
    tm_link_init(&tm, NAV_NAME, TM_CLASS_INFOTAINMENT, NAV_PRIO, NAV_BUDGET, 100000);
    tm_link_register(&tm);

    printf("nav: pid %d running, prio %d, core 2\n", getpid(), NAV_PRIO);
    fflush(stdout);

    // 5. Loop: wait for the tick, record lateness, do the route work.
    while (!stop) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse");
            break;
        }
        lat_add(&stats, late);

        actor_spin_ns(NAV_WORK, &stop);

        if (timer.tick % 10 == 0)                   // once a second: report misses
            tm_link_report(&tm, (uint32_t)stats.misses);
    }

    // 6. Release timer and TM connection, print the timing summary.
    actor_timer_stop(&timer);
    tm_link_close(&tm);
    lat_print(NAV_NAME, &stats);
    return 0;
}
