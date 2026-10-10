// actors.c - helpers shared by all actors
//
// Read top to bottom: the functions are numbered in the order you'd
// write them and in the order a sim uses them.

#include <errno.h>          // errno, EINTR
#include <pthread.h>        // pthread_self, pthread_setschedparam
#include <sched.h>          // struct sched_param, SCHED_FIFO, SCHED_RR
#include <signal.h>         // struct sigevent, SIGEV_PULSE_INIT
#include <stdint.h>         // int64_t, uintptr_t
#include <stdio.h>          // printf, perror
#include <string.h>         // memset, strerror
#include <time.h>           // clock_gettime, timer_create, timer_settime
#include <sys/neutrino.h>   // ThreadCtl, ChannelCreate, ConnectAttach,
                            // MsgReceivePulse, MsgSend, TimerTimeout
#include <sys/dispatch.h>   // name_open, name_close

#include "actors.h"

// Every pulse carries a small "code" number so the receiver knows what it is.
// Codes below _PULSE_CODE_MINAVAIL are reserved for QNX, so ours starts there.
#define TICK_CODE _PULSE_CODE_MINAVAIL

// Longest we ever wait for the task manager to answer
#define TM_CALL_LIMIT_NS (200 * NS_PER_MS)


// ---------------------------------------------------------------------------
// 1. Put this thread on some cores, at some priority
//
//    runmask : which cores I may run on (see actors.h)
//    policy  : SCHED_FIFO = keep the CPU until I sleep (safety jobs)
//              SCHED_RR   = take turns with equal priorities (comfort jobs)
//    prio    : higher number = runs first and interrupts lower ones
// ---------------------------------------------------------------------------
int actor_setup(unsigned runmask, int policy, int prio)
{
    // Pin. This only affects the thread that calls it. That's fine,
    // because every actor has exactly one thread.
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)runmask) == -1) {
        perror("ThreadCtl");
        return -1;
    }

    // Priority. Clear the whole struct first, then fill in the one field.
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;

    // pthread_* functions RETURN the error number (0 = OK), not -1 + errno.
    int rc = pthread_setschedparam(pthread_self(), policy, &sp);
    if (rc != 0) {
        printf("pthread_setschedparam: %s\n", strerror(rc));   // EPERM = not root
        return -1;
    }
    return 0;
}


// ---------------------------------------------------------------------------
// 2. Current time as one number of nanoseconds (CLOCK_MONOTONIC never jumps)
// ---------------------------------------------------------------------------
int64_t actor_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NS_PER_S + ts.tv_nsec;
}


// ---------------------------------------------------------------------------
// 3. Start a timer that sends me a pulse every period_ns
//    The chain:  mailbox  ->  line into it  ->  "what to send"  ->  timer
// ---------------------------------------------------------------------------
int actor_timer_start(actor_timer_t *t, int64_t period_ns, int prio)
{
    t->tick = 0;
    t->period_ns = period_ns;

    // a) My private mailbox.
    //    _NTO_CHF_FIXED_PRIORITY: normally a thread that receives a pulse
    //    takes on the pulse's priority. Without this flag the timer pulse
    //    would lift a demoted actor straight back up every tick and undo
    //    the task manager's SchedSet.
    t->chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY);
    if (t->chid == -1) {
        perror("ChannelCreate");
        return -1;
    }

    // b) A line into my own mailbox.
    t->coid = ConnectAttach(0, 0, t->chid, _NTO_SIDE_CHANNEL, 0);
    if (t->coid == -1) {
        perror("ConnectAttach");
        return -1;
    }

    // c) What the timer sends: a pulse on my line, my code, my priority.
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, t->coid, prio, TICK_CODE, 0);

    // d) Create the timer on the monotonic clock.
    if (timer_create(CLOCK_MONOTONIC, &ev, &t->timer) == -1) {
        perror("timer_create");
        return -1;
    }

    // e) Absolute first beat at t0 + period, then every period: no drift.
    t->t0_ns = actor_now_ns();
    int64_t first = t->t0_ns + period_ns;

    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec     = first / NS_PER_S;
    its.it_value.tv_nsec    = first % NS_PER_S;
    its.it_interval.tv_sec  = period_ns / NS_PER_S;
    its.it_interval.tv_nsec = period_ns % NS_PER_S;

    if (timer_settime(t->timer, TIMER_ABSTIME, &its, NULL) == -1) {
        perror("timer_settime");
        return -1;
    }
    return 0;
}


// ---------------------------------------------------------------------------
// 4. Wait for the next beat, then say how late I woke up.
//    Returns 0, or -1 with errno set (EINTR = a signal woke me).
// ---------------------------------------------------------------------------
int actor_wait_tick(actor_timer_t *t, int64_t *late_ns)
{
    struct _pulse p;

    if (MsgReceivePulse(t->chid, &p, sizeof p, NULL) == -1) {
        return -1;
    }

    t->tick++;
    int64_t due = t->t0_ns + t->tick * t->period_ns;
    *late_ns = actor_now_ns() - due;
    return 0;
}


// ---------------------------------------------------------------------------
// 5. Stop the timer and close everything (reverse order of step 3)
// ---------------------------------------------------------------------------
void actor_timer_stop(actor_timer_t *t)
{
    timer_delete(t->timer);
    ConnectDetach(t->coid);
    ChannelDestroy(t->chid);
}


// ---------------------------------------------------------------------------
// 6. Lateness scoreboard (lat_add is cheap: no printing, no allocation)
// ---------------------------------------------------------------------------
void lat_init(lat_stats_t *s, int64_t miss_limit_ns)
{
    memset(s, 0, sizeof *s);
    s->min_ns = INT64_MAX;
    s->max_ns = INT64_MIN;
    s->miss_limit_ns = miss_limit_ns;
}

void lat_add(lat_stats_t *s, int64_t late_ns)
{
    if (late_ns < s->min_ns) s->min_ns = late_ns;
    if (late_ns > s->max_ns) s->max_ns = late_ns;
    s->sum_ns += late_ns;
    s->count++;
    if (late_ns > s->miss_limit_ns) s->misses++;
}

void lat_print(const char *who, lat_stats_t *s)
{
    if (s->count == 0) {
        printf("%s: no cycles\n", who);
        return;
    }
    printf("%s: cycles %llu  min %.1f us  avg %.1f us  max %.1f us  misses %llu\n",
           who,
           (unsigned long long)s->count,
           (double)s->min_ns / NS_PER_US,
           (double)s->sum_ns / s->count / NS_PER_US,
           (double)s->max_ns / NS_PER_US,
           (unsigned long long)s->misses);
}


// ---------------------------------------------------------------------------
// 7. Burn CPU for 'ns' nanoseconds (wall time) = pretend work
// ---------------------------------------------------------------------------
void actor_spin_ns(int64_t ns, volatile sig_atomic_t *stop)
{
    int64_t end = actor_now_ns() + ns;
    while (actor_now_ns() < end) {
        if (stop != NULL && *stop) {
            return;
        }
    }
}


// ---------------------------------------------------------------------------
// 8. Task manager link (comfort actors only)
//
//    TimerTimeout arms a one-shot limit for the NEXT blocking kernel call
//    only. SEND|REPLY = give up if the TM doesn't take or answer the message
//    within 200 ms; MsgSend then fails with ETIMEDOUT instead of hanging.
// ---------------------------------------------------------------------------
static int tm_arm_limit(void)
{
    struct sigevent ev;
    SIGEV_UNBLOCK_INIT(&ev);
    uint64_t limit = TM_CALL_LIMIT_NS;
    return TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                        &ev, &limit, NULL);
}

void tm_link_init(tm_link_t *l, const char *name, int tm_class, int prio,
                  unsigned budget_pct, unsigned period_us)
{
    memset(l, 0, sizeof *l);
    l->coid = -1;
    l->handle = -1;
    snprintf(l->name, sizeof l->name, "%s", name);
    l->tm_class = tm_class;
    l->prio = prio;
    l->budget_pct = budget_pct;
    l->period_us = period_us;
}

int tm_link_register(tm_link_t *l)
{
    if (l->coid != -1)
        return 0;                                   // already linked

    int coid = name_open(TM_NAME, 0);               // find the TM
    if (coid == -1)
        return -1;                                  // TM not running: try later

    tm_register_t r;
    memset(&r, 0, sizeof r);
    r.type        = TM_REGISTER;
    r.tm_class    = (uint16_t)l->tm_class;
    r.tid         = (int32_t)pthread_self();        // on QNX this IS the thread id
    r.base_prio   = l->prio;
    r.budget_pct  = l->budget_pct;
    r.period_us   = l->period_us;
    r.deadline_us = l->period_us;
    memcpy(r.name, l->name, TM_NAME_LEN);

    tm_register_reply_t rep;
    if (tm_arm_limit() == -1 || MsgSend(coid, &r, sizeof r, &rep, sizeof rep) == -1) {
        printf("%s: TM register failed: %s\n", l->name, strerror(errno));
        fflush(stdout);
        name_close(coid);
        return -1;
    }
    l->coid = coid;
    l->handle = rep.handle;
    printf("%s: registered with TM (handle %d, prio %d, budget %u%%)\n",
           l->name, l->handle, l->prio, l->budget_pct);
    fflush(stdout);
    return 0;
}

void tm_link_report(tm_link_t *l, uint32_t misses)
{
    if (l->coid == -1) {                            // not linked: try again
        tm_link_register(l);
        return;
    }

    tm_report_load_t m;
    memset(&m, 0, sizeof m);
    m.type = TM_REPORT_LOAD;
    m.handle = l->handle;
    m.deadline_misses = misses;

    if (tm_arm_limit() == -1)
        return;
    if (MsgSend(l->coid, &m, sizeof m, NULL, 0) == -1) {
        if (errno == EINTR)                         // a signal (e.g. storm switch):
            return;                                 // harmless, keep the link
        printf("%s: lost TM (%s), will re-register\n", l->name, strerror(errno));
        fflush(stdout);
        name_close(l->coid);
        l->coid = -1;
        l->handle = -1;
    }
}

void tm_link_close(tm_link_t *l)
{
    if (l->coid != -1)
        name_close(l->coid);
    l->coid = -1;
    l->handle = -1;
}
