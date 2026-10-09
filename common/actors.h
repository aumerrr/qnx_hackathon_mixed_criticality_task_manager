<<<<<<< HEAD
// actors.h - helpers shared by all actors (abs, decoder, nav, ...)
//
// Every actor includes this file. The code for these functions is in
// actors.c, and the Makefile links actors.c into every sim.

#ifndef ACTORS_H       // "include guard": stops this file being read twice
#define ACTORS_H

#include <signal.h>    // sig_atomic_t (the type of the "stop" flags)
#include <stdint.h>    // int64_t, uint64_t (integers with a fixed size)
#include <time.h>      // timer_t
#include "tm_proto.h"  // task manager messages (used by tm_link_*)

// ---------------------------------------------------------------------------
// Core plan. A runmask is a bit pattern: bit 0 = core 0, bit 1 = core 1 ...
//
//   core 0      0x1 = 0001  task manager (alone)
//   core 1      0x2 = 0010  safety: abs (later airbag, steering)
//   core 2      0x4 = 0100  comfort pool: decoder (15), nav (12)
//   core 3      0x8 = 1000  viewers: logger, dashboard
//
// QNX 8.0 on this image only accepts single-core runmasks (0xC gave
// "Invalid argument"), so every thread is pinned to exactly one core.
// If the plan changes, change it HERE only. Sims never type raw numbers.
// ---------------------------------------------------------------------------
#define RMASK_TM     0x1u
#define RMASK_SAFETY 0x2u
#define RMASK_POOL   0x4u
#define RMASK_VIEW   0x8u

// Time units. Everything in this project is in nanoseconds (ns).
// "LL" makes them 64-bit numbers, so big multiplications can't overflow.
#define NS_PER_US 1000LL         // 1 microsecond
#define NS_PER_MS 1000000LL      // 1 millisecond
#define NS_PER_S  1000000000LL   // 1 second

// ---------------------------------------------------------------------------
// The "drummer": a kernel timer that drops a pulse (tiny note) into my
// private mailbox every period_ns. I sleep at the mailbox until it arrives.
// ---------------------------------------------------------------------------
typedef struct {
    int      chid;       // my mailbox (channel id)
    int      coid;       // my line into my own mailbox (the timer uses it)
    timer_t  timer;      // the kernel timer itself
    int64_t  t0_ns;      // time when I started; beat k is due at t0 + k*period
    int64_t  period_ns;  // time between two beats
    uint64_t tick;       // how many beats I have received so far (= k)
} actor_timer_t;

// ---------------------------------------------------------------------------
// Lateness scoreboard. Only numbers, so updating it is very cheap.
// ---------------------------------------------------------------------------
typedef struct {
    int64_t  min_ns;         // smallest lateness seen
    int64_t  max_ns;         // biggest lateness seen
    int64_t  sum_ns;         // total of all lateness (for the average)
    uint64_t count;          // how many cycles measured
    uint64_t misses;         // how many cycles were later than the limit
    int64_t  miss_limit_ns;  // what counts as "too late"
} lat_stats_t;

// ---------------------------------------------------------------------------
// Link to the task manager (COMFORT actors only, NEVER abs).
// Every call has a 200 ms limit, so a dead or stuck TM can't freeze us.
// ---------------------------------------------------------------------------
typedef struct {
    int      coid;                 // connection to the TM, -1 = not linked
    int      handle;               // ticket the TM gave us at registration
    char     name[TM_NAME_LEN];    // name shown in the TM
    int      tm_class;             // TM_CLASS_INFOTAINMENT etc.
    int      prio;                 // my normal priority (TM restores to this)
    unsigned budget_pct;           // % of one core I'm allowed
    unsigned period_us;            // my frame period
} tm_link_t;

// 1. put this thread on 'runmask' cores, with this policy and priority
int     actor_setup(unsigned runmask, int policy, int prio);

// 2. current time in ns
int64_t actor_now_ns(void);

// 3-5. the drummer: start it, wait for a beat, stop it
int  actor_timer_start(actor_timer_t *t, int64_t period_ns, int prio);
int  actor_wait_tick(actor_timer_t *t, int64_t *late_ns);
void actor_timer_stop(actor_timer_t *t);

// 6. the scoreboard
void lat_init(lat_stats_t *s, int64_t miss_limit_ns);
void lat_add(lat_stats_t *s, int64_t late_ns);
void lat_print(const char *who, lat_stats_t *s);

// 7. burn CPU for 'ns' nanoseconds (pretend work for comfort actors).
//    Stops early if *stop becomes 1 (pass NULL if you don't need that).
void actor_spin_ns(int64_t ns, volatile sig_atomic_t *stop);

// 8. task manager link: init once, register, report every ~1 s, close at end.
//    tm_link_report re-registers by itself if the TM was restarted.
void tm_link_init(tm_link_t *l, const char *name, int tm_class, int prio,
                  unsigned budget_pct, unsigned period_us);
int  tm_link_register(tm_link_t *l);
void tm_link_report(tm_link_t *l, uint32_t misses);
void tm_link_close(tm_link_t *l);

#endif
=======
// actors.h - helpers shared by all actors (abs, decoder, nav, ...)
//
// Every actor includes this file. The code for these functions is in
// actors.c, and the Makefile links actors.c into every sim.

#ifndef ACTORS_H       // "include guard": stops this file being read twice
#define ACTORS_H

#include <signal.h>    // sig_atomic_t (the type of the "stop" flags)
#include <stdint.h>    // int64_t, uint64_t (integers with a fixed size)
#include <time.h>      // timer_t
#include "tm_proto.h"  // task manager messages (used by tm_link_*)

// ---------------------------------------------------------------------------
// Core plan. A runmask is a bit pattern: bit 0 = core 0, bit 1 = core 1 ...
//
//   core 0      0x1 = 0001  task manager (alone)
//   core 1      0x2 = 0010  safety: abs (later airbag, steering)
//   core 2      0x4 = 0100  comfort pool: decoder (15), nav (12)
//   core 3      0x8 = 1000  viewers: logger, dashboard
//
// QNX 8.0 on this image only accepts single-core runmasks (0xC gave
// "Invalid argument"), so every thread is pinned to exactly one core.
// If the plan changes, change it HERE only. Sims never type raw numbers.
// ---------------------------------------------------------------------------
#define RMASK_TM     0x1u
#define RMASK_SAFETY 0x2u
#define RMASK_POOL   0x4u
#define RMASK_VIEW   0x8u

// Time units. Everything in this project is in nanoseconds (ns).
// "LL" makes them 64-bit numbers, so big multiplications can't overflow.
#define NS_PER_US 1000LL         // 1 microsecond
#define NS_PER_MS 1000000LL      // 1 millisecond
#define NS_PER_S  1000000000LL   // 1 second

// ---------------------------------------------------------------------------
// The "drummer": a kernel timer that drops a pulse (tiny note) into my
// private mailbox every period_ns. I sleep at the mailbox until it arrives.
// ---------------------------------------------------------------------------
typedef struct {
    int      chid;       // my mailbox (channel id)
    int      coid;       // my line into my own mailbox (the timer uses it)
    timer_t  timer;      // the kernel timer itself
    int64_t  t0_ns;      // time when I started; beat k is due at t0 + k*period
    int64_t  period_ns;  // time between two beats
    uint64_t tick;       // how many beats I have received so far (= k)
} actor_timer_t;

// ---------------------------------------------------------------------------
// Lateness scoreboard. Only numbers, so updating it is very cheap.
// ---------------------------------------------------------------------------
typedef struct {
    int64_t  min_ns;         // smallest lateness seen
    int64_t  max_ns;         // biggest lateness seen
    int64_t  sum_ns;         // total of all lateness (for the average)
    uint64_t count;          // how many cycles measured
    uint64_t misses;         // how many cycles were later than the limit
    int64_t  miss_limit_ns;  // what counts as "too late"
} lat_stats_t;

// ---------------------------------------------------------------------------
// Link to the task manager (COMFORT actors only, NEVER abs).
// Every call has a 200 ms limit, so a dead or stuck TM can't freeze us.
// ---------------------------------------------------------------------------
typedef struct {
    int      coid;                 // connection to the TM, -1 = not linked
    int      handle;               // ticket the TM gave us at registration
    char     name[TM_NAME_LEN];    // name shown in the TM
    int      tm_class;             // TM_CLASS_INFOTAINMENT etc.
    int      prio;                 // my normal priority (TM restores to this)
    unsigned budget_pct;           // % of one core I'm allowed
    unsigned period_us;            // my frame period
} tm_link_t;

// 1. put this thread on 'runmask' cores, with this policy and priority
int     actor_setup(unsigned runmask, int policy, int prio);

// 2. current time in ns
int64_t actor_now_ns(void);

// 3-5. the drummer: start it, wait for a beat, stop it
int  actor_timer_start(actor_timer_t *t, int64_t period_ns, int prio);
int  actor_wait_tick(actor_timer_t *t, int64_t *late_ns);
void actor_timer_stop(actor_timer_t *t);

// 6. the scoreboard
void lat_init(lat_stats_t *s, int64_t miss_limit_ns);
void lat_add(lat_stats_t *s, int64_t late_ns);
void lat_print(const char *who, lat_stats_t *s);

// 7. burn CPU for 'ns' nanoseconds (pretend work for comfort actors).
//    Stops early if *stop becomes 1 (pass NULL if you don't need that).
void actor_spin_ns(int64_t ns, volatile sig_atomic_t *stop);

// 8. task manager link: init once, register, report every ~1 s, close at end.
//    tm_link_report re-registers by itself if the TM was restarted.
void tm_link_init(tm_link_t *l, const char *name, int tm_class, int prio,
                  unsigned budget_pct, unsigned period_us);
int  tm_link_register(tm_link_t *l);
void tm_link_report(tm_link_t *l, uint32_t misses);
void tm_link_close(tm_link_t *l);

#endif
>>>>>>> 49a13ce (updates)
