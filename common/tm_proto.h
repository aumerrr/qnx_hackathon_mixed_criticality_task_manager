#ifndef TM_PROTO_H
#define TM_PROTO_H

#include <stdint.h>
#include <sys/types.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#define TM_NAME                "taskmgr"
#define TM_NAME_LEN            16
#define TM_MAX_TASKS           16
#define TM_MAX_CPUS            4
#define TM_EVENT_RING          16

#define TM_REGISTER            (_IO_MAX + 1)
#define TM_REPORT_LOAD         (_IO_MAX + 2)
#define TM_REQUEST             (_IO_MAX + 3)
#define TM_QUERY_STATE         (_IO_MAX + 4)

#define PULSE_MONITOR_TICK     (_PULSE_CODE_MINAVAIL + 0)
#define PULSE_REQ_TIMER        (_PULSE_CODE_MINAVAIL + 1)

#define TM_CLASS_CRITICAL      0
#define TM_CLASS_SAFETY        1
#define TM_CLASS_ASSIST        2
#define TM_CLASS_INFOTAINMENT  3
#define TM_CLASS_BACKGROUND    4

#define TM_STATE_NORMAL        0
#define TM_STATE_PRESSURE      1
#define TM_STATE_PROTECT       2
#define TM_STATE_RECOVER       3

#define TM_EV_REGISTER         1
#define TM_EV_GONE             2
#define TM_EV_DEMOTE           3
#define TM_EV_RESTORE          4
#define TM_EV_STATE            5
#define TM_EV_REQ_DONE         6
#define TM_EV_REQ_TIMEOUT      7
#define TM_EV_REQ_REJECT       8
#define TM_EV_CORE0_ALARM      9
#define TM_EV_REPAIR           10

#define TM_OP_PLAY_SONG        1

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

typedef struct {
    int32_t handle;
} tm_register_reply_t;

typedef struct {
    uint16_t type;
    uint16_t pad;
    int32_t  handle;
    uint32_t deadline_misses;
} tm_report_load_t;

typedef struct {
    uint16_t type;
    uint16_t pad;
    uint32_t req_id;
    uint32_t op;
    uint32_t max_time_ms;
    uint32_t work_ms;
} tm_request_t;

typedef struct {
    uint32_t req_id;
    uint32_t elapsed_ms;
} tm_request_reply_t;

typedef struct {
    uint16_t type;
    uint16_t pad;
} tm_query_t;

typedef struct {
    char     name[TM_NAME_LEN];
    int32_t  pid;
    int32_t  tid;
    uint16_t tm_class;
    uint16_t demoted;
    int32_t  base_prio;
    int32_t  cur_prio;
    uint32_t budget_pct;
    uint32_t share_pct;
    uint32_t period_us;
    uint32_t deadline_us;
    uint32_t deadline_misses;
    uint32_t demote_count;
} tm_task_info_t;

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

typedef struct {
    uint32_t       time_ms;
    uint32_t       tick;
    uint16_t       state;
    uint16_t       ntasks;
    uint16_t       ncpu;
    uint16_t       enforcing;
    uint32_t       core_load[TM_MAX_CPUS];
    uint32_t       pool_load;
    uint32_t       event_seq;
    tm_task_info_t tasks[TM_MAX_TASKS];
    tm_event_t     events[TM_EVENT_RING];
} tm_state_reply_t;

typedef union {
    uint16_t         type;
    struct _pulse    pulse;
    tm_register_t    reg;
    tm_report_load_t rep;
    tm_request_t     req;
    tm_query_t       query;
} tm_msg_t;

#endif
