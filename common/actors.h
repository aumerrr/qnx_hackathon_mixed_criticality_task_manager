// actors.h - library shared by every actor (abs, airbag, decoder, nav, ui, panel)
//
// Provides: core pinning + priority, periodic timer pulses, lateness
// statistics, simulated CPU work, and the client side of the task manager
// protocol. Implemented in actors.c, linked into every actor by the Makefile.

#ifndef ACTORS_H
#define ACTORS_H

#include <signal.h>    // sig_atomic_t
#include <stdint.h>    // int64_t, uint64_t
#include <time.h>      // timer_t
#include "tm_proto.h"  // task manager message formats (used by tm_link_*)

// ---------------------------------------------------------------------------
// Core plan (runmask bit N = core N). The only place core numbers are defined.
//
//   core 0  0x1  task manager (alone)
//   core 1  0x2  safety pool:  abs (FIFO 63), airbag (FIFO 62)
//   core 2  0x4  comfort pool: decoder (RR 15), nav (RR 12), ui (RR 10)
//   core 3  0x8  monitoring:   panel, dashboard
//
// Each thread is pinned to exactly one core: on this QNX 8.0 image a
// multi-core runmask (0xC) was rejected with EINVAL.
// ---------------------------------------------------------------------------
#define RMASK_TM     0x1u
#define RMASK_SAFETY 0x2u
#define RMASK_POOL   0x4u
#define RMASK_VIEW   0x8u

// All times in the project are int64 nanoseconds (LL = 64-bit, no overflow).
#define NS_PER_US 1000LL         // 1 microsecond
#define NS_PER_MS 1000000LL      // 1 millisecond
#define NS_PER_S  1000000000LL   // 1 second

// ---------------------------------------------------------------------------
// Periodic timer: a kernel timer delivers a pulse to a private channel every
// period_ns; the actor blocks in MsgReceivePulse until it arrives.
// ---------------------------------------------------------------------------
typedef struct {
    int      chid;       // private channel that receives the pulses
    int      coid;       // connection to that channel, used by the timer
    timer_t  timer;      // kernel timer
    int64_t  t0_ns;      // start time; tick k is due at t0 + k * period
    int64_t  period_ns;  // period
    uint64_t tick;       // ticks received so far (= k)
} actor_timer_t;

// ---------------------------------------------------------------------------
// Lateness statistics (wake-up time minus due time), updated every cycle.
// ---------------------------------------------------------------------------
typedef struct {
    int64_t  min_ns;         // minimum lateness
    int64_t  max_ns;         // maximum lateness
    int64_t  sum_ns;         // sum, for the average
    uint64_t count;          // cycles measured
    uint64_t misses;         // cycles later than miss_limit_ns
    int64_t  miss_limit_ns;  // deadline-miss threshold
} lat_stats_t;

// ---------------------------------------------------------------------------
// Client link to the task manager. Comfort actors only: safety actors
// (abs, airbag) never message the TM. Every call is limited to 200 ms, so a
// dead or stuck TM can delay an actor but never block it.
// ---------------------------------------------------------------------------
typedef struct {
    int      coid;                 // connection to the TM, -1 = not registered
    int      handle;               // slot index returned by TM_REGISTER
    char     name[TM_NAME_LEN];    // name shown by the TM
    int      tm_class;             // TM_CLASS_*; >= ASSIST may be demoted
    int      prio;                 // base priority the TM restores to
    unsigned budget_pct;           // CPU budget, % of one core
    unsigned period_us;            // frame period
} tm_link_t;

// Pin the calling thread to 'runmask' and set its policy and priority.
int     actor_setup(unsigned runmask, int policy, int prio);

// CLOCK_MONOTONIC time in ns.
int64_t actor_now_ns(void);

// Periodic timer: start, block until the next tick (returns lateness), stop.
int  actor_timer_start(actor_timer_t *t, int64_t period_ns, int prio);
int  actor_wait_tick(actor_timer_t *t, int64_t *late_ns);
void actor_timer_stop(actor_timer_t *t);

// Lateness statistics: reset, add one sample, print the summary line.
void lat_init(lat_stats_t *s, int64_t miss_limit_ns);
void lat_add(lat_stats_t *s, int64_t late_ns);
void lat_print(const char *who, lat_stats_t *s);

// Busy-wait 'ns' of wall time (simulated work); returns early if *stop is set.
void actor_spin_ns(int64_t ns, volatile sig_atomic_t *stop);

// TM link: init once, register, report misses every ~1 s, close at exit.
// tm_link_report re-registers automatically after a TM restart.
void tm_link_init(tm_link_t *l, const char *name, int tm_class, int prio,
                  unsigned budget_pct, unsigned period_us);
int  tm_link_register(tm_link_t *l);
void tm_link_report(tm_link_t *l, uint32_t misses);
void tm_link_close(tm_link_t *l);

#endif
