// airbag.c - protected airbag-monitor actor in the safety pool.
//
// This is a timing/workload demonstration, not a real airbag controller.
// It shares core 1 with ABS at a lower FIFO priority and does not register
// with the Task Manager, so comfort-pool policy cannot demote it.
//
// Run: ./airbag
//      ./airbag 0   run until stopped (default is 2000 cycles)

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "actors.h"

#define AIRBAG_PRIO    62
#define AIRBAG_PERIOD  (10 * NS_PER_MS)
#define AIRBAG_WORK    (1 * NS_PER_MS)
#define AIRBAG_MISS    (1 * NS_PER_MS)

static volatile sig_atomic_t stop = 0;

static void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char *argv[])
{
    long cycles = 2000;
    if (argc > 1)
        cycles = atol(argv[1]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (actor_setup(RMASK_SAFETY, SCHED_FIFO, AIRBAG_PRIO) == -1)
        return 1;

    actor_timer_t timer;
    if (actor_timer_start(&timer, AIRBAG_PERIOD, AIRBAG_PRIO) == -1)
        return 1;

    lat_stats_t stats;
    lat_init(&stats, AIRBAG_MISS);

    printf("airbag: pid %d running on safety core, FIFO %d, %ld cycles\n",
           getpid(), AIRBAG_PRIO, cycles);
    fflush(stdout);

    while (!stop && (cycles == 0 || stats.count < (uint64_t)cycles)) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR)
                continue;
            perror("MsgReceivePulse");
            break;
        }

        lat_add(&stats, late);
        actor_spin_ns(AIRBAG_WORK, &stop);
    }

    actor_timer_stop(&timer);
    lat_print("airbag", &stats);
    return 0;
}
