// ui.c - UI / infotainment actor (comfort job, managed by the task manager)
//
// Core 2 (comfort pool), SCHED_RR priority 10 (lowest comfort job, below nav
// and decoder), 100 ms period, 10 ms of work. Registered, budget 20%.
// SIGUSR1 toggles stress mode: the actor then burns the rest of every frame,
// goes over budget, and the task manager demotes it.
//
// Run as root:  ./ui            registers as "ui"
//               ./ui <name>     registers under another name
//               kill -USR1 <pid>   toggle stress

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define UI_NAME       "ui"
#define UI_PRIO       10                    // lowest comfort priority
#define UI_BUDGET     20                    // % of one core before the TM steps in
#define UI_PERIOD     (100 * NS_PER_MS)     // one frame every 100 ms
#define UI_WORK       (10 * NS_PER_MS)      // work per frame in normal mode
#define UI_MISS       (20 * NS_PER_MS)      // frame started > 20 ms late = miss

static volatile sig_atomic_t stop = 0;      // set by SIGINT/SIGTERM
static volatile sig_atomic_t storm = 0;     // flipped by SIGUSR1

static void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

static void on_storm(int sig)
{
    (void)sig;
    storm = !storm;
}

int main(int argc, char *argv[])
{
    const char *name = (argc > 1) ? argv[1] : UI_NAME;

    // SIGINT/SIGTERM = clean stop, SIGUSR1 = stress on/off.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_storm;
    sigaction(SIGUSR1, &sa, NULL);

    // Pin to the comfort core, RR 10.
    if (actor_setup(RMASK_POOL, SCHED_RR, UI_PRIO) == -1)
        return 1;

    // Periodic 100 ms timer pulse.
    actor_timer_t timer;
    if (actor_timer_start(&timer, UI_PERIOD, UI_PRIO) == -1)
        return 1;

    lat_stats_t stats;
    lat_init(&stats, UI_MISS);

    // Register with the task manager (failure is fine: report() retries).
    tm_link_t tm;
    tm_link_init(&tm, name, TM_CLASS_INFOTAINMENT, UI_PRIO, UI_BUDGET, 100000);
    (void)tm_link_register(&tm);

    printf("%s: pid %d running on comfort core, prio %d, budget %u%% "
           "(kill -USR1 %d toggles stress)\n",
           name, getpid(), UI_PRIO, UI_BUDGET, getpid());
    fflush(stdout);

    while (!stop) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR)
                continue;
            perror("MsgReceivePulse");
            break;
        }

        lat_add(&stats, late);

        if (storm) {
            // Stress: work until the next frame is due (~100% of the core).
            int64_t next_due = timer.t0_ns +
                               (int64_t)(timer.tick + 1) * timer.period_ns;
            actor_spin_ns(next_due - actor_now_ns(), &stop);
        } else {
            // Normal: bounded 10 ms of work (~10%).
            actor_spin_ns(UI_WORK, &stop);
        }

        if (timer.tick % 10 == 0)                   // once a second: report misses
            (void)tm_link_report(&tm, (uint32_t)stats.misses);
    }

    // Release timer and TM connection, print the timing summary.
    actor_timer_stop(&timer);
    tm_link_close(&tm);
    lat_print(name, &stats);
    return 0;
}
