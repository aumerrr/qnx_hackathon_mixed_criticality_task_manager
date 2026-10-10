// decoder.c - music decoder actor (comfort job, managed by the task manager)
//
// Core 2 (comfort pool), SCHED_RR priority 15 (highest comfort job), 100 ms
// frames. Registered with the task manager, budget 40%.
//
// Modes (first argument):
//   light : 20 ms of work per frame          ~20% of one core
//   busy  : works the whole frame            ~100%
//   burst : 10 s busy, 10 s light, repeating (shows demote AND restore)
// SIGUSR1 toggles storm: forces busy regardless of the mode.
//
// When it storms, the TM demotes it to priority 2 so nav (12) and ui (10)
// get the core back; when it calms down, the TM restores it to 15.
//
// Run as root:  ./decoder [light|busy|burst] [name]

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define DEC_NAME    "decoder"
#define DEC_PRIO    15                  // highest comfort priority
#define DEC_BUDGET  40                  // % of one core before the TM steps in
#define DEC_PERIOD  (100 * NS_PER_MS)   // one frame every 100 ms
#define DEC_LIGHT   (20 * NS_PER_MS)    // work per frame in light mode
#define DEC_MISS    (20 * NS_PER_MS)    // frame started > 20 ms late = miss
#define DEC_BURST   (10 * NS_PER_S)     // length of each burst phase

#define MODE_LIGHT 0
#define MODE_BUSY  1
#define MODE_BURST 2

volatile sig_atomic_t stop  = 0;        // set by SIGINT/SIGTERM
volatile sig_atomic_t storm = 0;        // flipped by SIGUSR1

void on_stop(int sig)  { (void)sig; stop = 1; }
void on_storm(int sig) { (void)sig; storm = !storm; }

// "light"/"busy"/"burst" -> MODE_*, -1 if unknown.
int parse_mode(const char *s)
{
    if (strcmp(s, "light") == 0) return MODE_LIGHT;
    if (strcmp(s, "busy")  == 0) return MODE_BUSY;
    if (strcmp(s, "burst") == 0) return MODE_BURST;
    return -1;
}

int main(int argc, char *argv[])
{
    // Arguments: [mode] [name]
    int mode = MODE_LIGHT;
    const char *mode_name = "light";
    const char *name = DEC_NAME;

    if (argc > 1) {
        mode = parse_mode(argv[1]);
        if (mode == -1) {
            printf("usage: %s [light|busy|burst] [name]\n", argv[0]);
            return 1;
        }
        mode_name = argv[1];
    }
    if (argc > 2) name = argv[2];

    // SIGINT/SIGTERM = clean stop, SIGUSR1 = storm on/off.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_storm;
    sigaction(SIGUSR1, &sa, NULL);

    // 1. Pin to the comfort core, RR 15.
    if (actor_setup(RMASK_POOL, SCHED_RR, DEC_PRIO) == -1) return 1;

    // 2. Periodic 100 ms timer pulse.
    actor_timer_t timer;
    if (actor_timer_start(&timer, DEC_PERIOD, DEC_PRIO) == -1) return 1;

    // 3. Lateness statistics.
    lat_stats_t stats;
    lat_init(&stats, DEC_MISS);

    // 4. Register with the task manager (failure is fine: report() retries).
    tm_link_t tm;
    tm_link_init(&tm, name, TM_CLASS_INFOTAINMENT, DEC_PRIO, DEC_BUDGET, 100000);
    tm_link_register(&tm);

    printf("%s: pid %d running, mode %s, prio %d (kill -USR1 %d = storm)\n",
           name, getpid(), mode_name, DEC_PRIO, getpid());
    fflush(stdout);

    // 5. Frame loop.
    while (!stop) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse");
            break;
        }
        lat_add(&stats, late);

        // Heavy frame if storming, in busy mode, or in an even burst phase.
        int heavy = 0;
        if (storm) heavy = 1;
        if (mode == MODE_BUSY) heavy = 1;
        if (mode == MODE_BURST) {
            int64_t since_start = actor_now_ns() - timer.t0_ns;
            if ((since_start / DEC_BURST) % 2 == 0) heavy = 1;
        }

        if (heavy) {
            // Work until the next frame is due: the core never rests.
            int64_t next_due = timer.t0_ns + (int64_t)(timer.tick + 1) * timer.period_ns;
            actor_spin_ns(next_due - actor_now_ns(), &stop);
        } else {
            actor_spin_ns(DEC_LIGHT, &stop);
        }

        if (timer.tick % 10 == 0)                   // once a second: report misses
            tm_link_report(&tm, (uint32_t)stats.misses);
    }

    // 6. Release timer and TM connection, print the timing summary.
    actor_timer_stop(&timer);
    tm_link_close(&tm);
    lat_print(name, &stats);
    return 0;
}
