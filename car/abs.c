// abs.c - brake actor (safety-critical)
//
// What it does: runs alone on the safety core (core 1) at FIFO priority 63,
// wakes up every 5 ms like a real ABS loop, and measures how late each
// wake-up was.
//
// Why it's safe:
//   - Highest priority on its own core: nothing there can interrupt it.
//   - It NEVER talks to the task manager. MsgSend waits for a reply, so a
//     dead or stuck task manager could freeze it. No message = no risk.
//   - No printf inside the loop (printing is slow and can block).
//
// Run as root:   ./abs        2000 cycles = 10 s
//                ./abs 0      forever, until Ctrl-C or kill
// Expected:      about 194 us average lateness, 0 misses

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

// All the numbers you might tune, in one place
#define ABS_PRIO    63                  // highest normal priority
#define ABS_PERIOD  (5 * NS_PER_MS)     // wake every 5 ms
#define ABS_MISS    (1 * NS_PER_MS)     // more than 1 ms late = a miss

// Set to 1 by Ctrl-C / kill. 'volatile sig_atomic_t' is the safe type for a
// variable that a signal handler writes and normal code reads.
volatile sig_atomic_t stop = 0;

// Signal handler: only set the flag. (printf is not safe inside a handler.)
void on_signal(int sig)
{
    stop = 1;
}

int main(int argc, char *argv[])
{
    // How many cycles: "./abs 2000" (default 2000), "./abs 0" = forever
    long cycles = 2000;
    if (argc > 1) cycles = atol(argv[1]);

    // Make Ctrl-C (SIGINT) and kill (SIGTERM) call on_signal instead of
    // killing us instantly, so we can still print the summary.
    // A signal also wakes MsgReceivePulse with errno = EINTR.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. Safety core, FIFO 63. Done BEFORE the timer, so even the first
    //    beat is handled on the right core at the right priority.
    if (actor_setup(RMASK_SAFETY, SCHED_FIFO, ABS_PRIO) == -1) return 1;

    // 2. The 5 ms drummer. Pulse priority = our own priority (63).
    actor_timer_t timer;
    if (actor_timer_start(&timer, ABS_PERIOD, ABS_PRIO) == -1) return 1;

    // 3. Empty scoreboard
    lat_stats_t stats;
    lat_init(&stats, ABS_MISS);

    // Last print before the loop. fflush = show it now, even in a log file.
    printf("abs: pid %d running, %ld cycles\n", getpid(), cycles);
    fflush(stdout);

    // 4. The loop: wait for a beat -> measure -> (later: act)
    while (!stop && (cycles == 0 || stats.count < cycles)) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;   // a signal; the loop test sees 'stop'
            perror("MsgReceivePulse");
            break;                          // real error: stop, don't spin at
        }                                   // priority 63 and freeze the core
        lat_add(&stats, late);

        // later (v2): read the button, set the brake LED when it changes
        // later (v3): copy 'stats' to shared memory for the dashboard
    }

    // 5. Clean up, then print the one summary line
    actor_timer_stop(&timer);
    lat_print("abs", &stats);
    return 0;
}/*
 * abs.c
 *
 *  Created on: 09-Oct-2026
 *      Author: abmes
 */


