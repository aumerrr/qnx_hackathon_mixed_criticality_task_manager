// tm_proto.h - message contract between the task manager and its clients.
// Shared by taskmgr.c, actors.c (comfort actors) and panel.c. Any change here
// requires rebuilding every program (the Makefile makes them all depend on it).
// Every message starts with a 16-bit 'type' so the TM can dispatch on it.

#ifndef TM_PROTO_H
#define TM_PROTO_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

// Limits and the name the TM registers with name_attach().
#define TM_NAME                "taskmgr"
#define TM_NAME_LEN            16
#define TM_MAX_TASKS           16
#define TM_MAX_CPUS            4
#define TM_EVENT_RING          16                 // events kept for TM_QUERY_STATE

// Message types (above _IO_MAX so they never clash with QNX resource-manager messages).
#define TM_REGISTER            (_IO_MAX + 1)  // actor -> TM: join, get a handle
#define TM_REPORT_LOAD         (_IO_MAX + 2)  // actor -> TM: periodic deadline-miss report
#define TM_REQUEST             (_IO_MAX + 3)  // client -> TM: timed service request
#define TM_QUERY_STATE         (_IO_MAX + 4)  // panel/monitor -> TM: full state snapshot

// Pulse codes the TM sends to itself.
#define PULSE_MONITOR_TICK     (_PULSE_CODE_MINAVAIL + 0)  // 100 ms monitor tick
#define PULSE_REQ_TIMER        (_PULSE_CODE_MINAVAIL + 1)  // TM_REQUEST timer expired

// Criticality classes. Only class >= TM_CLASS_ASSIST can be demoted.
#define TM_CLASS_CRITICAL      0
#define TM_CLASS_SAFETY        1
#define TM_CLASS_ASSIST        2
#define TM_CLASS_INFOTAINMENT  3
#define TM_CLASS_BACKGROUND    4

// TM system state (drives the LED panel and dashboard banner).
#define TM_STATE_NORMAL        0     // nothing demoted, pool not hot
#define TM_STATE_PRESSURE      1     // pool >= 90% for 2 ticks
#define TM_STATE_PROTECT       2     // at least one job demoted
#define TM_STATE_RECOVER       3     // demoted jobs, pool calm: restoring

// Event kinds in the TM event log (tm_event_t.kind).
#define TM_EV_REGISTER         1
#define TM_EV_GONE             2
#define TM_EV_DEMOTE           3
#define TM_EV_RESTORE          4
#define TM_EV_STATE            5
#define TM_EV_REQ_DONE         6
#define TM_EV_REQ_TIMEOUT      7
#define TM_EV_REQ_REJECT       8
#define TM_EV_CORE0_ALARM      9    // safety core load above the alarm threshold
#define TM_EV_REPAIR           10   // TM reset a job to its declared base priority

#define TM_OP_PLAY_SONG        1     // the only TM_REQUEST operation

// TM_REGISTER request. The TM takes the pid from the kernel (_msg_info), not from here.
typedef struct {
    uint16_t type;
    uint16_t tm_class;
    int32_t  tid;
    int32_t  base_prio;
    uint32_t budget_pct;
    uint32_t period_us;
    uint32_t deadline_us;
    char     name[TM_NAME_LEN];
} tm_register_t;

// TM_REGISTER reply: handle = slot index used in later reports.
typedef struct {
    int32_t handle;
} tm_register_reply_t;

// TM_REPORT_LOAD: actor's cumulative deadline misses.
typedef struct {
    uint16_t type;
    uint16_t pad;
    int32_t  handle;
    uint32_t deadline_misses;
} tm_report_load_t;

// TM_REQUEST: the TM replies OK after work_ms, or ETIMEDOUT at max_time_ms.
typedef struct {
    uint16_t type;
    uint16_t pad;
    uint32_t req_id;
    uint32_t op;
    uint32_t max_time_ms;
    uint32_t work_ms;
} tm_request_t;

// TM_REQUEST reply.
typedef struct {
    uint32_t req_id;
    uint32_t elapsed_ms;
} tm_request_reply_t;

// TM_QUERY_STATE request (no payload).
typedef struct {
    uint16_t type;
    uint16_t pad;
} tm_query_t;

// One registered job, as reported in tm_state_reply_t.
typedef struct {
    char     name[TM_NAME_LEN];
    int32_t  pid;
    int32_t  tid;
    uint16_t tm_class;
    uint16_t demoted;          // 1 = currently demoted by the TM
    int32_t  base_prio;
    int32_t  cur_prio;         // priority right now (2 when demoted)
    uint32_t budget_pct;
    uint32_t share_pct;        // measured CPU share last tick, % of one core
    uint32_t period_us;
    uint32_t deadline_us;
    uint32_t deadline_misses;
    uint32_t demote_count;
} tm_task_info_t;

// One TM event (ring of TM_EVENT_RING entries).
typedef struct {
    uint32_t seq;
    uint32_t time_ms;
    uint16_t kind;
    uint16_t tm_class;
    int32_t  pid;
    int32_t  tid;
    int32_t  from;
    int32_t  to;
    uint32_t value;
    char     name[TM_NAME_LEN];
} tm_event_t;

// TM_QUERY_STATE reply: complete TM snapshot.
typedef struct {
    uint32_t       time_ms;
    uint32_t       tick;
    uint16_t       state;
    uint16_t       ntasks;
    uint16_t       ncpu;
    uint16_t       enforcing;
    uint32_t       core_load[TM_MAX_CPUS];   // % busy per core, from idle-thread clocks
    uint32_t       pool_load;                // average load of the comfort pool
    uint32_t       event_seq;
    tm_task_info_t tasks[TM_MAX_TASKS];
    tm_event_t     events[TM_EVENT_RING];
} tm_state_reply_t;

// Receive buffer of the TM: large enough for any message or a pulse.
typedef union {
    uint16_t         type;
    struct _pulse    pulse;
    tm_register_t    reg;
    tm_report_load_t rep;
    tm_request_t     req;
    tm_query_t       query;
} tm_msg_t;

#endif
