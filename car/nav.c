<<<<<<< HEAD
// nav.c - navigation actor (comfort job, managed by the task manager)
//
// What it does: runs on core 2, the SAME core as the decoder, at RR
// priority 12 (below the decoder's 15). Every 100 ms it does 20 ms of
// "route work" (about 20% of the core).
//
// Its role in the demo: the victim.
//   - Decoder storms at 15 -> nav (12) can't run, its frames start late.
//   - Task manager demotes the decoder to 2 -> nav runs first again.
// The misses it reports to the TM show this directly.
//
// Run as root:   ./nav

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define NAV_NAME    "nav"
#define NAV_PRIO    12                  // below decoder (15)
#define NAV_BUDGET  30                  // % of one core
#define NAV_PERIOD  (100 * NS_PER_MS)   // one route update every 100 ms
#define NAV_WORK    (20 * NS_PER_MS)    // 20 ms of work per update
#define NAV_MISS    (20 * NS_PER_MS)    // started > 20 ms late = a miss

volatile sig_atomic_t stop = 0;

void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. core 2, round-robin, priority 12
    if (actor_setup(RMASK_POOL, SCHED_RR, NAV_PRIO) == -1) return 1;

    // 2. a route update every 100 ms
    actor_timer_t timer;
    if (actor_timer_start(&timer, NAV_PERIOD, NAV_PRIO) == -1) return 1;

    // 3. lateness scoreboard
    lat_stats_t stats;
    lat_init(&stats, NAV_MISS);

    // 4. introduce myself to the task manager
    tm_link_t tm;
    tm_link_init(&tm, NAV_NAME, TM_CLASS_INFOTAINMENT, NAV_PRIO, NAV_BUDGET, 100000);
    tm_link_register(&tm);

    printf("nav: pid %d running, prio %d, core 2\n", getpid(), NAV_PRIO);
    fflush(stdout);

    // 5. the loop
    while (!stop) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse");
            break;
        }
        lat_add(&stats, late);

        actor_spin_ns(NAV_WORK, &stop);

        if (timer.tick % 10 == 0)                   // about once a second
            tm_link_report(&tm, (uint32_t)stats.misses);
    }

    // 6. clean up, print once
    actor_timer_stop(&timer);
    tm_link_close(&tm);
    lat_print(NAV_NAME, &stats);
    return 0;
}
=======
// nav.c - navigation actor (comfort job, managed by the task manager)
//
// What it does: runs on core 2, the SAME core as the decoder, at RR
// priority 12 (below the decoder's 15). Every 100 ms it does 20 ms of
// "route work" (about 20% of the core).
//
// Its role in the demo: the victim.
//   - Decoder storms at 15 -> nav (12) can't run, its frames start late.
//   - Task manager demotes the decoder to 2 -> nav runs first again.
// The misses it reports to the TM show this directly.
//
// Run as root:   ./nav

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define NAV_NAME    "nav"
#define NAV_PRIO    12                  // below decoder (15)
#define NAV_BUDGET  30                  // % of one core
#define NAV_PERIOD  (100 * NS_PER_MS)   // one route update every 100 ms
#define NAV_WORK    (20 * NS_PER_MS)    // 20 ms of work per update
#define NAV_MISS    (20 * NS_PER_MS)    // started > 20 ms late = a miss

volatile sig_atomic_t stop = 0;

void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. core 2, round-robin, priority 12
    if (actor_setup(RMASK_POOL, SCHED_RR, NAV_PRIO) == -1) return 1;

    // 2. a route update every 100 ms
    actor_timer_t timer;
    if (actor_timer_start(&timer, NAV_PERIOD, NAV_PRIO) == -1) return 1;

    // 3. lateness scoreboard
    lat_stats_t stats;
    lat_init(&stats, NAV_MISS);

    // 4. introduce myself to the task manager
    tm_link_t tm;
    tm_link_init(&tm, NAV_NAME, TM_CLASS_INFOTAINMENT, NAV_PRIO, NAV_BUDGET, 100000);
    tm_link_register(&tm);

    printf("nav: pid %d running, prio %d, core 2\n", getpid(), NAV_PRIO);
    fflush(stdout);

    // 5. the loop
    while (!stop) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse");
            break;
        }
        lat_add(&stats, late);

        actor_spin_ns(NAV_WORK, &stop);

        if (timer.tick % 10 == 0)                   // about once a second
            tm_link_report(&tm, (uint32_t)stats.misses);
    }

    // 6. clean up, print once
    actor_timer_stop(&timer);
    tm_link_close(&tm);
    lat_print(NAV_NAME, &stats);
    return 0;
}
>>>>>>> 49a13ce (updates)
