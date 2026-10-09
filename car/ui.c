// ui.c - infotainment/UI actor in the comfort pool.
//
// Runs on core 2 below decoder and navigation. Normal mode performs a small
// amount of periodic work. Pressing SIGUSR1 toggles stress mode, in which the
// actor consumes the remainder of each frame to create measurable contention.
// The Task Manager may demote this actor when its CPU share exceeds its budget.
//
// Run: ./ui
//      kill -USR1 <ui-pid>   toggle stress mode
//      ./ui <name>           optional registration name

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define UI_NAME       "ui"
#define UI_PRIO       10
#define UI_BUDGET     20
#define UI_PERIOD     (100 * NS_PER_MS)
#define UI_WORK       (10 * NS_PER_MS)
#define UI_MISS       (20 * NS_PER_MS)

static volatile sig_atomic_t stop = 0;
static volatile sig_atomic_t storm = 0;

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

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_storm;
    sigaction(SIGUSR1, &sa, NULL);

    if (actor_setup(RMASK_POOL, SCHED_RR, UI_PRIO) == -1)
        return 1;

    actor_timer_t timer;
    if (actor_timer_start(&timer, UI_PERIOD, UI_PRIO) == -1)
        return 1;

    lat_stats_t stats;
    lat_init(&stats, UI_MISS);

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
            int64_t next_due = timer.t0_ns +
                               (int64_t)(timer.tick + 1) * timer.period_ns;
            actor_spin_ns(next_due - actor_now_ns(), &stop);
        } else {
            actor_spin_ns(UI_WORK, &stop);
        }

        if (timer.tick % 10 == 0)
            (void)tm_link_report(&tm, (uint32_t)stats.misses);
    }

    actor_timer_stop(&timer);
    tm_link_close(&tm);
    lat_print(name, &stats);
    return 0;
}
