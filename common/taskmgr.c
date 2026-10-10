// taskmgr.c - Task Manager: CPU-budget enforcement for the comfort pool.
//
// Runs alone on core 0 at FIFO 50 as a QNX message-passing server ("taskmgr").
//   - Comfort actors register (TM_REGISTER) with a class, base priority and
//     CPU budget. The pid comes from the kernel (_msg_info), so it can't be faked.
//   - Every 100 ms (timer pulse) it measures each job's CPU share from its
//     thread CPU clock, and each core's load from the idle threads' clocks.
//   - Policy: a job over budget for OVER_TICKS ticks is demoted to priority 2
//     with SchedSet; it is restored to its base priority after it (or the
//     pool) stays calm. QNX 8.0 has no adaptive partitioning, so budgets are
//     enforced through priority.
//   - Safety jobs (class < ASSIST) and unregistered jobs (abs, airbag) are
//     never touched.
//   - TM_QUERY_STATE returns a full snapshot (panel, dashboard).
//   - TM_REQUEST: a timed request that always gets an answer (OK or ETIMEDOUT).
//
// Usage:  ./taskmgr [--off] [-q]   --off = measure only, never demote
//                                  -q    = no console output

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>
#include <sys/syspage.h>
#include "tm_proto.h"

#define TM_CORE_MASK     0x1u  // TM runs alone on core 0
#define TM_PRIO          50  // above every comfort job, below safety
#define TICK_NS          100000000L  // monitor period: 100 ms
#define DEMOTE_PRIO      2  // priority given to a demoted job
#define OVER_TICKS       3  // ticks over budget before demotion
#define CALM_TICKS       5  // calm ticks per strike before restore
#define MAX_STRIKES      8  // cap on repeat-offender penalty
#define FORGIVE_TICKS    50  // calm ticks (5 s) that clear the strikes
#define HOT_TICKS        2  // pool-hot ticks before PRESSURE state
#define POOL_HOT_PCT     90  // pool load counted as hot
#define POOL_CALM_PCT    70  // pool load counted as calm
#define SAFETY_CORE      1  // core watched for the safety alarm
#define SAFETY_ALARM_PCT 50  // safety core load that raises the alarm
#define POOL_FIRST       2  // comfort pool = cores POOL_FIRST..POOL_LAST
#define POOL_LAST        2
#define IDLE_PID         1  // procnto: tid c+1 is the idle thread of core c
#define MAX_PENDING      8  // TM_REQUESTs in progress at once
#define STATUS_EVERY     10  // console status every 10 ticks (1 s)

// One registered job.
typedef struct {
    int       used;
    char      name[TM_NAME_LEN];
    pid_t     pid;
    int       tid;
    unsigned  tm_class;
    int       base_prio;
    int       saved_policy;  // policy to restore after demotion
    int       cur_prio;
    unsigned  budget_pct;
    unsigned  period_us;
    unsigned  deadline_us;
    clockid_t clk;  // thread CPU-time clock (ClockId)
    uint64_t  cpu_last;  // CPU ns at the previous tick
    uint64_t  wall_last;  // wall ns at the previous tick
    unsigned  share_pct;  // CPU share last tick, % of one core
    int       demoted;
    unsigned  over;  // consecutive ticks over budget
    unsigned  under;  // consecutive ticks within budget
    unsigned  strikes;  // times demoted recently: longer calm needed
    unsigned  demote_count;
    unsigned  misses;  // deadline misses reported by the job
} task_t;

// One TM_REQUEST in progress: the client stays reply-blocked until it ends.
typedef struct {
    int      used;
    rcvid_t  rcvid;
    uint32_t req_id;
    int      outcome;  // EOK or ETIMEDOUT, decided at arrival
    uint64_t start;
    uint64_t fire_at;  // when the request timer is due
    timer_t  timer;
} pending_t;

// ---- server state (single-threaded: no locking needed) ----
static task_t           tasks[TM_MAX_TASKS];
static pending_t        pend[MAX_PENDING];
static tm_event_t       ring[TM_EVENT_RING];
static uint32_t         ev_seq;
static tm_state_reply_t qr;  // snapshot buffer for TM_QUERY_STATE

static int       ncpu;
static clockid_t idle_clk[TM_MAX_CPUS];  // per-core idle-thread CPU clocks
static uint64_t  idle_last[TM_MAX_CPUS];
static uint64_t  mon_wall_last;
static unsigned  core_load[TM_MAX_CPUS];
static unsigned  pool_load;
static unsigned  pool_hot;  // consecutive hot ticks
static unsigned  pool_calm;  // consecutive calm ticks
static int       safety_alarm;

static name_attach_t *attach;
static int       chid = -1;
static int       self_coid = -1;
static timer_t   tick_timer;
static int       tick_timer_ok;
static uint64_t  t0;
static uint32_t  tick;
static int       state = TM_STATE_NORMAL;
static int       enforce = 1;  // 0 with --off: measure only
static int       quiet = 0;
static volatile sig_atomic_t stop_flag;

static const char *state_name[] = {"NORMAL", "PRESSURE", "PROTECT", "RECOVER"};
static const char *ev_name[] = {"?", "register", "gone", "demote", "restore", "state",
                                "req_done", "req_timeout", "req_reject", "safety_alarm", "repair"};

// Monotonic time in ns, and ms since the TM started.
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint32_t ms_now(void)
{
    return (uint32_t)((now_ns() - t0) / 1000000ULL);
}

// Append an event to the ring (read by TM_QUERY_STATE) and print it.
static void log_event(int kind, const task_t *t, int from, int to, unsigned value)
{
    tm_event_t *e = &ring[ev_seq % TM_EVENT_RING];
    memset(e, 0, sizeof *e);
    ev_seq++;
    e->seq = ev_seq;
    e->time_ms = ms_now();
    e->kind = (uint16_t)kind;
    e->from = from;
    e->to = to;
    e->value = value;
    if (t != NULL) {
        e->tm_class = (uint16_t)t->tm_class;
        e->pid = t->pid;
        e->tid = t->tid;
        memcpy(e->name, t->name, TM_NAME_LEN);
    } else {
        strcpy(e->name, "-");
    }
    if (!quiet) {
        printf("[%8u ms] %-12s %-15s pid=%d tid=%d %d -> %d val=%u\n",
               e->time_ms, ev_name[kind], e->name, e->pid, e->tid, from, to, value);
        fflush(stdout);
    }
}

// Every received message gets exactly one reply or error, so no client
// is left reply-blocked. ESRCH = client already gone: ignore.
static void reply_ok(rcvid_t rcvid, const void *buf, size_t len)
{
    if (MsgReply(rcvid, EOK, buf, len) == -1 && errno != ESRCH)
        fprintf(stderr, "taskmgr: MsgReply: %s\n", strerror(errno));
}

static void reply_err(rcvid_t rcvid, int err)
{
    if (MsgError(rcvid, err) == -1 && errno != ESRCH)
        fprintf(stderr, "taskmgr: MsgError: %s\n", strerror(errno));
}

// Pin the TM to core 0 and run at FIFO 50.
static int pin_and_prioritize(void)
{
    if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)TM_CORE_MASK) == -1) {
        fprintf(stderr, "taskmgr: runmask failed: %s\n", strerror(errno));
        return -1;
    }
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = TM_PRIO;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc != EOK) {
        fprintf(stderr, "taskmgr: set priority failed: %s\n", strerror(rc));
        return -1;
    }
    printf("taskmgr: mask 0x%x, on CPU %d, FIFO %d\n", TM_CORE_MASK, SchedGetCpuNum(), TM_PRIO);
    return 0;
}

// Core-load monitor: open the CPU clock of each core's idle thread
// (procnto pid 1, tid c+1) and take a baseline reading.
static int cpumon_init(void)
{
    ncpu = _syspage_ptr->num_cpu;
    if (ncpu > TM_MAX_CPUS)
        ncpu = TM_MAX_CPUS;
    for (int c = 0; c < ncpu; c++) {
        struct sched_param sp;
        if (SchedGet(IDLE_PID, c + 1, &sp) == -1) {
            fprintf(stderr, "cpumon: SchedGet(1,%d): %s\n", c + 1, strerror(errno));
            return -1;
        }
        if (sp.sched_priority != 0)
            fprintf(stderr, "cpumon: WARNING pid1 tid%d prio %d, not an idle thread?\n",
                    c + 1, sp.sched_priority);
        idle_clk[c] = ClockId(IDLE_PID, c + 1);
        if (idle_clk[c] == -1) {
            fprintf(stderr, "cpumon: ClockId(1,%d): %s\n", c + 1, strerror(errno));
            return -1;
        }
        if (ClockTime(idle_clk[c], NULL, &idle_last[c]) == -1) {
            fprintf(stderr, "cpumon: ClockTime core %d: %s\n", c, strerror(errno));
            return -1;
        }
    }
    mon_wall_last = now_ns();
    printf("taskmgr: watching %d cores\n", ncpu);
    return 0;
}

// Core load = 100 - idle time / elapsed wall time. Pool load = average of
// cores POOL_FIRST..POOL_LAST.
static int cpumon_sample(uint64_t now)
{
    uint64_t idle_now[TM_MAX_CPUS];
    uint64_t wall = now - mon_wall_last;
    if (wall == 0)
        return -1;
    for (int c = 0; c < ncpu; c++) {
        if (ClockTime(idle_clk[c], NULL, &idle_now[c]) == -1) {
            fprintf(stderr, "cpumon: ClockTime core %d: %s\n", c, strerror(errno));
            return -1;
        }
    }
    for (int c = 0; c < ncpu; c++) {
        uint64_t idle = idle_now[c] - idle_last[c];
        if (idle > wall)
            idle = wall;
        core_load[c] = (unsigned)(100 - (idle * 100) / wall);
        idle_last[c] = idle_now[c];
    }
    mon_wall_last = now;
    unsigned sum = 0, n = 0;
    for (int c = POOL_FIRST; c <= POOL_LAST && c < ncpu; c++) {
        sum += core_load[c];
        n++;
    }
    pool_load = n ? sum / n : 0;
    return 0;
}

// Only comfort jobs with a budget are subject to the policy.
static int manageable(const task_t *t)
{
    return t->used && t->tm_class >= TM_CLASS_ASSIST && t->budget_pct > 0;
}

// Change another process's thread priority (needs root).
static int set_prio(pid_t pid, int tid, int policy, int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    if (SchedSet(pid, tid, policy, &sp) == -1) {
        fprintf(stderr, "taskmgr: SchedSet(%d,%d,prio %d): %s\n", pid, tid, prio, strerror(errno));
        return -1;
    }
    return 0;
}

// Demote: save the current policy, drop to RR priority 2, count a strike.
static void demote(task_t *t)
{
    struct sched_param sp;
    int pol = SchedGet(t->pid, t->tid, &sp);
    if (pol == -1)
        return;
    if (set_prio(t->pid, t->tid, SCHED_RR, DEMOTE_PRIO) == -1)
        return;
    t->saved_policy = pol;
    t->demoted = 1;
    t->over = 0;
    t->under = 0;
    if (t->strikes < MAX_STRIKES)
        t->strikes++;
    t->demote_count++;
    t->cur_prio = DEMOTE_PRIO;
    log_event(TM_EV_DEMOTE, t, sp.sched_priority, DEMOTE_PRIO, t->share_pct);
}

// Restore the saved policy and base priority.
static void restore(task_t *t)
{
    if (set_prio(t->pid, t->tid, t->saved_policy, t->base_prio) == -1)
        return;
    t->demoted = 0;
    t->over = 0;
    t->under = 0;
    t->cur_prio = t->base_prio;
    log_event(TM_EV_RESTORE, t, DEMOTE_PRIO, t->base_prio, t->share_pct);
}

// Policy, once per tick:
//  1. demote every job over budget for OVER_TICKS ticks;
//  2. restore at most ONE demoted job per tick (highest base priority first),
//     once it or the pool has been calm for CALM_TICKS * strikes ticks.
//     One per tick avoids a new storm when everything is restored at once.
static void apply_policy(void)
{
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (manageable(t) && !t->demoted && t->over >= OVER_TICKS)
            demote(t);
    }
    task_t *best = NULL;
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (!manageable(t) || !t->demoted)
            continue;
        unsigned need = CALM_TICKS * t->strikes;
        if (t->under >= need || pool_calm >= need) {
            if (best == NULL || t->base_prio > best->base_prio)
                best = t;
        }
    }
    if (best != NULL)
        restore(best);
}

// System state: PROTECT/RECOVER while a job is demoted, PRESSURE when the
// pool is hot, otherwise NORMAL. Each change is logged as an event.
static void update_state(void)
{
    int nd = 0;
    for (int i = 0; i < TM_MAX_TASKS; i++)
        if (tasks[i].used && tasks[i].demoted)
            nd++;
    int ns;
    if (nd > 0)
        ns = (pool_calm >= CALM_TICKS) ? TM_STATE_RECOVER : TM_STATE_PROTECT;
    else if (pool_hot >= HOT_TICKS)
        ns = TM_STATE_PRESSURE;
    else
        ns = TM_STATE_NORMAL;
    if (ns != state) {
        log_event(TM_EV_STATE, NULL, state, ns, pool_load);
        state = ns;
    }
}

// One status block per second (parsed by dashboard.py; keep the format).
static void print_status(void)
{
    printf("t=%7.1fs %-8s pool %3u%% |", tick / 10.0, state_name[state], pool_load);
    for (int c = 0; c < ncpu; c++)
        printf(" C%d %3u%%", c, core_load[c]);
    printf("\n");
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (!t->used)
            continue;
        printf("    %-15s cls %u prio %3d/%3d share %3u%% budget %3u%% miss %u%s\n",
               t->name, t->tm_class, t->cur_prio, t->base_prio, t->share_pct,
               t->budget_pct, t->misses, t->demoted ? "  DEMOTED" : "");
    }
    fflush(stdout);
}

// 100 ms monitor tick: core loads, safety alarm, each job's CPU share, policy.
static void on_tick(void)
{
    uint64_t now = now_ns();
    tick++;
    if (cpumon_sample(now) == 0) {
        pool_hot = (pool_load >= POOL_HOT_PCT) ? pool_hot + 1 : 0;
        pool_calm = (pool_load < POOL_CALM_PCT) ? pool_calm + 1 : 0;
        // Safety-core alarm with 10% hysteresis (observe only, never acts on core 1).
        if (!safety_alarm && core_load[SAFETY_CORE] >= SAFETY_ALARM_PCT) {
            safety_alarm = 1;
            log_event(TM_EV_CORE0_ALARM, NULL, 0, 1, core_load[SAFETY_CORE]);
        } else if (safety_alarm && core_load[SAFETY_CORE] + 10 < SAFETY_ALARM_PCT) {
            safety_alarm = 0;
            log_event(TM_EV_CORE0_ALARM, NULL, 1, 0, core_load[SAFETY_CORE]);
        }
    }
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (!t->used)
            continue;
        uint64_t cpu;
        // Clock read fails = thread exited: free its slot.
        if (ClockTime(t->clk, NULL, &cpu) == -1) {
            log_event(TM_EV_GONE, t, t->cur_prio, 0, 0);
            memset(t, 0, sizeof *t);
            continue;
        }
        uint64_t wall = now - t->wall_last;
        // Share = CPU time used / wall time elapsed, as % of one core.
        unsigned share = wall ? (unsigned)(((cpu - t->cpu_last) * 100) / wall) : 0;
        if (share > 100)
            share = 100;
        t->share_pct = share;
        t->cpu_last = cpu;
        t->wall_last = now;
        struct sched_param sp;
        // Record the real current priority.
        if (SchedGet(t->pid, t->tid, &sp) != -1)
            t->cur_prio = sp.sched_priority;
        if (manageable(t)) {
            if (share > t->budget_pct) {
                t->over++;
                t->under = 0;
            } else {
                t->under++;
                t->over = 0;
            }
            // A long calm period clears the repeat-offender strikes.
            if (!t->demoted && t->strikes > 0 && t->under >= FORGIVE_TICKS)
                t->strikes = 0;
        }
    }
    if (enforce)
        apply_policy();
    update_state();
    if (!quiet && tick % STATUS_EVERY == 0)
        print_status();
}

// TM_REGISTER: validate, add the job (pid from the kernel), reply with its slot.
static void handle_register(rcvid_t rcvid, const tm_msg_t *msg, const struct _msg_info *info)
{
    const tm_register_t *m = &msg->reg;
    if (info->msglen < sizeof *m) {
        reply_err(rcvid, EBADMSG);
        return;
    }
    if (m->tm_class > TM_CLASS_BACKGROUND || m->budget_pct > 100 ||
        m->base_prio < 0 || m->base_prio > 255) {
        reply_err(rcvid, EINVAL);
        return;
    }
    // Already registered (re-register after a lost reply): return the same slot.
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        if (tasks[i].used && tasks[i].pid == info->pid && tasks[i].tid == m->tid) {
            tm_register_reply_t r;
            r.handle = i;
            reply_ok(rcvid, &r, sizeof r);
            return;
        }
    }
    struct sched_param sp;
    int pol = SchedGet(info->pid, m->tid, &sp);
    if (pol == -1) {
        reply_err(rcvid, ESRCH);
        return;
    }
    int slot = -1;
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        if (!tasks[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        reply_err(rcvid, ENOSPC);
        return;
    }
    // CPU clock of the job's thread, plus a baseline reading.
    clockid_t clk = ClockId(info->pid, m->tid);
    if (clk == -1) {
        reply_err(rcvid, errno);
        return;
    }
    uint64_t cpu;
    if (ClockTime(clk, NULL, &cpu) == -1) {
        reply_err(rcvid, errno);
        return;
    }
    task_t *t = &tasks[slot];
    memset(t, 0, sizeof *t);
    t->used = 1;
    memcpy(t->name, m->name, TM_NAME_LEN);
    t->name[TM_NAME_LEN - 1] = '\0';
    t->pid = info->pid;
    t->tid = m->tid;
    t->tm_class = m->tm_class;
    t->base_prio = (m->base_prio > 0) ? m->base_prio : sp.sched_priority;
    t->saved_policy = pol;
    t->cur_prio = sp.sched_priority;
    t->budget_pct = m->budget_pct;
    t->period_us = m->period_us;
    t->deadline_us = m->deadline_us;
    t->clk = clk;
    t->cpu_last = cpu;
    t->wall_last = now_ns();
    // Job not at its declared base priority (e.g. left demoted by a previous
    // TM that crashed): repair it.
    if (t->tm_class >= TM_CLASS_ASSIST && sp.sched_priority != t->base_prio) {
        if (set_prio(t->pid, t->tid, pol, t->base_prio) == 0) {
            log_event(TM_EV_REPAIR, t, sp.sched_priority, t->base_prio, 0);
            t->cur_prio = t->base_prio;
        }
    }
    log_event(TM_EV_REGISTER, t, 0, t->cur_prio, t->budget_pct);
    tm_register_reply_t r;
    r.handle = slot;
    reply_ok(rcvid, &r, sizeof r);
}

// TM_REPORT_LOAD: store the job's miss count. Only the owner pid may report.
static void handle_report(rcvid_t rcvid, const tm_msg_t *msg, const struct _msg_info *info)
{
    const tm_report_load_t *m = &msg->rep;
    if (info->msglen < sizeof *m) {
        reply_err(rcvid, EBADMSG);
        return;
    }
    if (m->handle < 0 || m->handle >= TM_MAX_TASKS) {
        reply_err(rcvid, EINVAL);
        return;
    }
    task_t *t = &tasks[m->handle];
    if (!t->used || t->pid != info->pid) {
        reply_err(rcvid, ESRCH);
        return;
    }
    t->misses = m->deadline_misses;
    reply_ok(rcvid, NULL, 0);
}

// End a TM_REQUEST: disarm its timer, reply OK or the error, free the slot.
static void finish_pending(pending_t *p, int outcome)
{
    struct itimerspec off;
    memset(&off, 0, sizeof off);
    if (timer_settime(p->timer, 0, &off, NULL) == -1)
        fprintf(stderr, "taskmgr: timer disarm: %s\n", strerror(errno));
    uint32_t el = (uint32_t)((now_ns() - p->start) / 1000000ULL);
    if (outcome == EOK) {
        tm_request_reply_t r;
        r.req_id = p->req_id;
        r.elapsed_ms = el;
        reply_ok(p->rcvid, &r, sizeof r);
        log_event(TM_EV_REQ_DONE, NULL, 0, (int)el, p->req_id);
    } else {
        reply_err(p->rcvid, outcome);
        log_event(TM_EV_REQ_TIMEOUT, NULL, 0, (int)el, p->req_id);
    }
    p->used = 0;
}

// TM_REQUEST: reject max_time 0 (EINVAL); otherwise hold the reply and arm a
// timer for min(work, max_time). Outcome is OK if work fits, else ETIMEDOUT,
// so the client always gets an answer and never waits forever.
static void handle_request(rcvid_t rcvid, const tm_msg_t *msg, const struct _msg_info *info)
{
    const tm_request_t *m = &msg->req;
    if (info->msglen < sizeof *m) {
        reply_err(rcvid, EBADMSG);
        return;
    }
    if (m->op != TM_OP_PLAY_SONG) {
        reply_err(rcvid, ENOSYS);
        return;
    }
    if (m->max_time_ms == 0) {
        log_event(TM_EV_REQ_REJECT, NULL, 0, 0, m->req_id);
        reply_err(rcvid, EINVAL);
        return;
    }
    if (m->work_ms == 0) {
        tm_request_reply_t r;
        r.req_id = m->req_id;
        r.elapsed_ms = 0;
        reply_ok(rcvid, &r, sizeof r);
        log_event(TM_EV_REQ_DONE, NULL, 0, 0, m->req_id);
        return;
    }
    int slot = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pend[i].used) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        reply_err(rcvid, EAGAIN);
        return;
    }
    pending_t *p = &pend[slot];
    int done_in_time = (m->work_ms <= m->max_time_ms);
    uint32_t delay = done_in_time ? m->work_ms : m->max_time_ms;
    uint64_t now = now_ns();
    p->used = 1;
    p->rcvid = rcvid;
    p->req_id = m->req_id;
    p->outcome = done_in_time ? EOK : ETIMEDOUT;
    p->start = now;
    p->fire_at = now + (uint64_t)delay * 1000000ULL;
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = delay / 1000;
    its.it_value.tv_nsec = (long)(delay % 1000) * 1000000L;
    if (timer_settime(p->timer, 0, &its, NULL) == -1) {
        fprintf(stderr, "taskmgr: request timer: %s\n", strerror(errno));
        p->used = 0;
        reply_err(rcvid, EAGAIN);
    }
}

// TM_QUERY_STATE: copy state, core loads, all jobs and the event ring.
static void handle_query(rcvid_t rcvid, const struct _msg_info *info)
{
    if (info->msglen < sizeof(tm_query_t)) {
        reply_err(rcvid, EBADMSG);
        return;
    }
    memset(&qr, 0, sizeof qr);
    qr.time_ms = ms_now();
    qr.tick = tick;
    qr.state = (uint16_t)state;
    qr.ncpu = (uint16_t)ncpu;
    qr.enforcing = (uint16_t)enforce;
    for (int c = 0; c < ncpu; c++)
        qr.core_load[c] = core_load[c];
    qr.pool_load = pool_load;
    qr.event_seq = ev_seq;
    int n = 0;
    for (int i = 0; i < TM_MAX_TASKS; i++) {
        task_t *t = &tasks[i];
        if (!t->used)
            continue;
        tm_task_info_t *o = &qr.tasks[n++];
        memcpy(o->name, t->name, TM_NAME_LEN);
        o->pid = t->pid;
        o->tid = t->tid;
        o->tm_class = (uint16_t)t->tm_class;
        o->demoted = (uint16_t)t->demoted;
        o->base_prio = t->base_prio;
        o->cur_prio = t->cur_prio;
        o->budget_pct = t->budget_pct;
        o->share_pct = t->share_pct;
        o->period_us = t->period_us;
        o->deadline_us = t->deadline_us;
        o->deadline_misses = t->misses;
        o->demote_count = t->demote_count;
    }
    qr.ntasks = (uint16_t)n;
    memcpy(qr.events, ring, sizeof ring);
    reply_ok(rcvid, &qr, sizeof qr);
}

// Pulses: monitor tick, request timer, client unblock (gave up waiting),
// client disconnect.
static void handle_pulse(const struct _pulse *pl)
{
    switch (pl->code) {
    case PULSE_MONITOR_TICK:
        on_tick();
        break;
    case PULSE_REQ_TIMER: {
        int s = pl->value.sival_int;
        if (s < 0 || s >= MAX_PENDING || !pend[s].used)
            break;
        // Stale pulse from a re-armed timer: ignore.
        if (now_ns() + 1000000ULL < pend[s].fire_at)
            break;
        finish_pending(&pend[s], pend[s].outcome);
        break;
    }
    case _PULSE_CODE_UNBLOCK: {
        rcvid_t r_ptr = (rcvid_t)(intptr_t)pl->value.sival_ptr;
        int r_int = pl->value.sival_int;
        for (int s = 0; s < MAX_PENDING; s++) {
            if (pend[s].used && (pend[s].rcvid == r_ptr || (int)pend[s].rcvid == r_int)) {
                finish_pending(&pend[s], ETIMEDOUT);
                break;
            }
        }
        break;
    }
    case _PULSE_CODE_DISCONNECT:
        if (ConnectDetach(pl->scoid) == -1)
            fprintf(stderr, "taskmgr: ConnectDetach: %s\n", strerror(errno));
        break;
    default:
        break;
    }
}

// Dispatch a message on its 16-bit type. Unknown types get ENOSYS.
static void handle_message(rcvid_t rcvid, const tm_msg_t *msg, const struct _msg_info *info)
{
    if (info->msglen < sizeof(uint16_t)) {
        reply_err(rcvid, EBADMSG);
        return;
    }
    switch (msg->type) {
    case _IO_CONNECT:
        reply_ok(rcvid, NULL, 0);
        break;
    case TM_REGISTER:
        handle_register(rcvid, msg, info);
        break;
    case TM_REPORT_LOAD:
        handle_report(rcvid, msg, info);
        break;
    case TM_REQUEST:
        handle_request(rcvid, msg, info);
        break;
    case TM_QUERY_STATE:
        handle_query(rcvid, info);
        break;
    default:
        reply_err(rcvid, ENOSYS);
        break;
    }
}

// One periodic 100 ms tick timer + one one-shot timer per request slot.
// All deliver pulses to our own channel at TM priority.
static int setup_timers(void)
{
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, self_coid, TM_PRIO, PULSE_MONITOR_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &tick_timer) == -1) {
        fprintf(stderr, "taskmgr: tick timer_create: %s\n", strerror(errno));
        return -1;
    }
    tick_timer_ok = 1;
    for (int i = 0; i < MAX_PENDING; i++) {
        SIGEV_PULSE_INIT(&ev, self_coid, TM_PRIO, PULSE_REQ_TIMER, 0);
        ev.sigev_value.sival_int = i;
        if (timer_create(CLOCK_MONOTONIC, &ev, &pend[i].timer) == -1) {
            fprintf(stderr, "taskmgr: request timer_create %d: %s\n", i, strerror(errno));
            return -1;
        }
    }
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = TICK_NS;
    its.it_interval.tv_nsec = TICK_NS;
    if (timer_settime(tick_timer, 0, &its, NULL) == -1) {
        fprintf(stderr, "taskmgr: tick timer_settime: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

// SIGINT/SIGTERM: leave the receive loop and shut down cleanly.
static void on_signal(int signo)
{
    (void)signo;
    stop_flag = 1;
}

// Clean exit: restore every demoted job (none stays at priority 2), cancel
// pending requests, release timers and the registered name.
static void shutdown_tm(void)
{
    for (int i = 0; i < TM_MAX_TASKS; i++)
        if (tasks[i].used && tasks[i].demoted)
            restore(&tasks[i]);
    for (int i = 0; i < MAX_PENDING; i++)
        if (pend[i].used)
            finish_pending(&pend[i], ECANCELED);
    if (tick_timer_ok)
        timer_delete(tick_timer);
    if (self_coid != -1)
        ConnectDetach(self_coid);
    if (attach != NULL)
        name_detach(attach, 0);
    printf("taskmgr: stopped cleanly\n");
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--off") == 0)
            enforce = 0;
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else {
            fprintf(stderr, "usage: %s [--off] [-q]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGINT, &sa, NULL) == -1) {
        fprintf(stderr, "taskmgr: sigaction: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    t0 = now_ns();
    if (pin_and_prioritize() == -1)
        return EXIT_FAILURE;
    if (cpumon_init() == -1)
        return EXIT_FAILURE;

    // Register "taskmgr" so clients can name_open() it.
    attach = name_attach(NULL, TM_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "taskmgr: name_attach: %s (already running? slay taskmgr)\n", strerror(errno));
        return EXIT_FAILURE;
    }
    chid = attach->chid;

    // Connection to our own channel, used by the timers to send pulses.
    self_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (self_coid == -1) {
        fprintf(stderr, "taskmgr: ConnectAttach: %s\n", strerror(errno));
        shutdown_tm();
        return EXIT_FAILURE;
    }
    if (setup_timers() == -1) {
        shutdown_tm();
        return EXIT_FAILURE;
    }

    printf("taskmgr: ready (%s)\n", enforce ? "enforcing" : "watch only --off");
    fflush(stdout);

    // Server loop: rcvid 0 = pulse, otherwise a message that needs a reply.
    tm_msg_t msg;
    struct _msg_info info;
    while (!stop_flag) {
        rcvid_t rcvid = MsgReceive(chid, &msg, sizeof msg, &info);
        if (rcvid == -1) {
            if (errno != EINTR)
                fprintf(stderr, "taskmgr: MsgReceive: %s\n", strerror(errno));
            continue;
        }
        if (rcvid == 0)
            handle_pulse(&msg.pulse);
        else
            handle_message(rcvid, &msg, &info);
    }

    shutdown_tm();
    return EXIT_SUCCESS;
}
