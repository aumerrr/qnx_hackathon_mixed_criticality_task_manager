#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/dispatch.h>

#include "actors.h"

#define PANEL_PRIO   10
#define PANEL_TICK   (20 * NS_PER_MS)
#define LED_EVERY    5
#define RED_HOLD     (1000 * NS_PER_MS)
#define QUERY_LIMIT  (50 * NS_PER_MS)
#define BTN_STABLE   5
#define BTN_LOCKOUT  (1000 * NS_PER_MS)

#define PIN_GREEN    17
#define PIN_YELLOW   27
#define PIN_RED      22
#define PIN_BUTTON   5

enum { L_OFF, L_GREEN, L_YELLOW, L_RED };

static const char *lname[] = {"OFF (TM offline)", "GREEN (all under budget)",
                              "YELLOW (demoted)", "RED (greedy)"};
static const int   led_pin[3] = {PIN_GREEN, PIN_YELLOW, PIN_RED};
static int         led_now[3] = {-1, -1, -1};
static int         led_warned;
static tm_state_reply_t st;
static struct termios saved_tio;
static int         tio_saved;

volatile sig_atomic_t stop = 0;

static void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

static int gpio_write(int pin, const char *cmd)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/gpio/%d", pin);
    int fd = open(path, O_WRONLY);
    if (fd == -1)
        return -1;
    size_t n = strlen(cmd);
    ssize_t w = write(fd, cmd, n);
    int saved = errno;
    close(fd);
    errno = saved;
    return (w == (ssize_t)n) ? 0 : -1;
}

static int gpio_read_raw(int pin, char *buf, size_t len)
{
    char path[32];
    snprintf(path, sizeof path, "/dev/gpio/%d", pin);
    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    int saved = errno;
    close(fd);
    errno = saved;
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return 0;
}

static int parse_level(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    if (s[0] == '1') return 1;
    if (s[0] == '0') return 0;
    if (strncmp(s, "on", 2) == 0 || strncmp(s, "high", 4) == 0) return 1;
    if (strncmp(s, "off", 3) == 0 || strncmp(s, "low", 3) == 0) return 0;
    return -1;
}

static int key_init(void)
{
    if (!isatty(STDIN_FILENO)) {
        printf("panel: key mode needs a terminal, run it in the foreground (no '&', no '>')\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &saved_tio) == -1) {
        printf("panel: tcgetattr: %s\n", strerror(errno));
        return -1;
    }
    struct termios t = saved_tio;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == -1) {
        printf("panel: tcsetattr: %s\n", strerror(errno));
        return -1;
    }
    tio_saved = 1;
    return 0;
}

static void key_restore(void)
{
    if (tio_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    tio_saved = 0;
}

static int key_read(void)
{
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    return (n == 1) ? (unsigned char)c : -1;
}

static void led_set(int i, int on)
{
    if (led_now[i] == on)
        return;
    if (gpio_write(led_pin[i], on ? "on\n" : "off\n") == 0) {
        led_now[i] = on;
    } else if (!led_warned) {
        printf("panel: GPIO %d write failed: %s\n", led_pin[i], strerror(errno));
        fflush(stdout);
        led_warned = 1;
    }
}

static void show(int s)
{
    led_set(0, s == L_GREEN);
    led_set(1, s == L_YELLOW);
    led_set(2, s == L_RED);
}

static int query_tm(int coid)
{
    tm_query_t q;
    memset(&q, 0, sizeof q);
    q.type = TM_QUERY_STATE;
    struct sigevent ev;
    SIGEV_UNBLOCK_INIT(&ev);
    uint64_t limit = QUERY_LIMIT;
    if (TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                     &ev, &limit, NULL) == -1)
        return -1;
    if (MsgSend(coid, &q, sizeof q, &st, sizeof st) == -1)
        return -1;
    return 0;
}

static int classify(void)
{
    int greedy = 0;
    int demoted = 0;
    for (int i = 0; i < st.ntasks && i < TM_MAX_TASKS; i++) {
        tm_task_info_t *t = &st.tasks[i];
        if (t->demoted)
            demoted = 1;
        else if (t->tm_class >= TM_CLASS_ASSIST && t->budget_pct > 0 &&
                 t->share_pct > t->budget_pct)
            greedy = 1;
    }
    if (greedy)
        return L_RED;
    if (demoted)
        return L_YELLOW;
    return L_GREEN;
}

/* Stress targets. Only these two comfort actors can be stressed from the
 * panel: abs and airbag are never given to the panel, so it cannot signal them. */
typedef struct {
    const char *name;
    pid_t       pid;     /* 0 = not available */
    int         storm;   /* our view: flipped by every SIGUSR1 we send */
} target_t;

static target_t targets[2] = { {"decoder", 0, 0}, {"ui", 0, 0} };
static int      selected = 0;

static void show_selection(void)
{
    target_t *t = &targets[selected];
    printf("panel: selected %s (pid %d), stress %s   [1 = decoder, 2 = ui, s = stress, q = quit]\n",
           t->name, (int)t->pid, t->storm ? "ON" : "OFF");
    fflush(stdout);
}

static void select_target(int i)
{
    if (targets[i].pid <= 0)
        printf("panel: %s is not running, keeping %s\n", targets[i].name, targets[selected].name);
    else
        selected = i;
    show_selection();
}

static void toggle_storm(const char *how)
{
    target_t *t = &targets[selected];
    if (t->pid <= 0) {
        printf("panel: no %s to stress\n", t->name);
    } else if (kill(t->pid, SIGUSR1) == -1) {
        printf("panel: %s (pid %d) not reachable: %s\n", t->name, (int)t->pid, strerror(errno));
        if (errno == ESRCH)
            t->pid = 0;                 /* gone: don't try again */
    } else {
        t->storm = !t->storm;
        printf("panel: %s -> %s stress %s\n", how, t->name, t->storm ? "ON" : "OFF");
    }
    fflush(stdout);
}

int main(int argc, char *argv[])
{
    int use_key = 0;
    int npid = 0;
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        char *end;
        long v = strtol(argv[i], &end, 10);
        if (strcmp(argv[i], "key") == 0)
            use_key = 1;
        else if (npid < 2 && *argv[i] != '\0' && *end == '\0' && v >= 0)
            targets[npid++].pid = (pid_t)v;
        else
            bad = 1;
    }
    if (bad || npid == 0) {
        printf("usage: %s <decoder pid> [ui pid] [key]   (pid 0 = not running)\n", argv[0]);
        return 1;
    }
    int valid = 0;
    for (int i = 1; i >= 0; i--) {          /* ends with the first valid one selected */
        if (targets[i].pid > 0 && kill(targets[i].pid, 0) == -1) {
            printf("panel: %s pid %d not running (%s), ignored\n",
                   targets[i].name, (int)targets[i].pid, strerror(errno));
            targets[i].pid = 0;
        }
        if (targets[i].pid > 0) {
            selected = i;
            valid++;
        }
    }
    if (valid == 0) {
        printf("panel: neither decoder nor ui is running\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (actor_setup(RMASK_VIEW, SCHED_RR, PANEL_PRIO) == -1) return 1;

    for (int i = 0; i < 3; i++) {
        if (gpio_write(led_pin[i], "out\n") == -1) {
            printf("panel: GPIO %d (LED) setup failed: %s\n", led_pin[i], strerror(errno));
            return 1;
        }
    }

    char raw[16];
    int level = 0;
    if (!use_key) {
        if (gpio_write(PIN_BUTTON, "in\n") == -1)
            printf("panel: warning, could not set GPIO %d to input: %s\n",
                   PIN_BUTTON, strerror(errno));
        if (gpio_read_raw(PIN_BUTTON, raw, sizeof raw) == -1) {
            printf("panel: cannot read GPIO %d (button): %s\n", PIN_BUTTON, strerror(errno));
            return 1;
        }
        level = parse_level(raw);
        raw[strcspn(raw, "\r\n")] = '\0';
        printf("panel: button GPIO %d reads '%s' (level %d), should be 0 when not pressed\n",
               PIN_BUTTON, raw, level);
        if (level == -1) {
            printf("panel: unknown button value format, check 'cat /dev/gpio/%d'\n", PIN_BUTTON);
            return 1;
        }
    }

    show(L_GREEN);  usleep(300000);
    show(L_YELLOW); usleep(300000);
    show(L_RED);    usleep(300000);
    show(L_OFF);

    actor_timer_t timer;
    if (actor_timer_start(&timer, PANEL_TICK, PANEL_PRIO) == -1) {
        show(L_OFF);
        return 1;
    }

    if (use_key && key_init() == -1) {
        actor_timer_stop(&timer);
        show(L_OFF);
        return 1;
    }

    printf("panel: pid %d on core 3, %s\n", getpid(),
           use_key ? "KEY MODE" : "button toggles stress of the selected actor");
    show_selection();

    int coid = -1;
    int last = -1;
    int64_t red_until = 0;
    int stable = level;
    int candidate = level;
    int count = BTN_STABLE;
    int btn_warned = 0;
    int64_t last_press = 0;

    while (!stop) {
        int64_t late;
        if (actor_wait_tick(&timer, &late) == -1) {
            if (errno == EINTR) continue;
            perror("MsgReceivePulse");
            break;
        }

        if (use_key) {
            int c;
            while ((c = key_read()) != -1) {
                if (c == 's' || c == 'S')
                    toggle_storm("key 's'");
                else if (c == '1')
                    select_target(0);
                else if (c == '2')
                    select_target(1);
                else if (c == 'q' || c == 'Q')
                    stop = 1;
            }
        } else if (gpio_read_raw(PIN_BUTTON, raw, sizeof raw) == 0) {
            int v = parse_level(raw);
            if (v != -1) {
                if (v == candidate) {
                    if (count < BTN_STABLE)
                        count++;
                } else {
                    candidate = v;
                    count = 1;
                }
                if (count == BTN_STABLE && candidate != stable) {
                    stable = candidate;
                    if (stable == 1 && actor_now_ns() - last_press >= BTN_LOCKOUT) {
                        last_press = actor_now_ns();
                        toggle_storm("button pressed");
                    }
                }
            }
        } else if (!btn_warned) {
            printf("panel: button read failed: %s\n", strerror(errno));
            fflush(stdout);
            btn_warned = 1;
        }

        if (timer.tick % LED_EVERY != 0)
            continue;

        if (coid == -1)
            coid = name_open(TM_NAME, 0);

        int s = L_OFF;
        if (coid != -1) {
            if (query_tm(coid) == 0) {
                s = classify();
            } else {
                name_close(coid);
                coid = -1;
            }
        }

        int64_t now = actor_now_ns();
        if (s == L_RED)
            red_until = now + RED_HOLD;
        else if (s != L_OFF && now < red_until)
            s = L_RED;

        show(s);

        if (s != last) {
            printf("panel: LED %s\n", lname[s]);
            fflush(stdout);
            last = s;
        }
    }

    for (int i = 0; i < 2; i++) {           /* switch off any stress we left on */
        if (targets[i].storm && targets[i].pid > 0 && kill(targets[i].pid, SIGUSR1) == 0)
            printf("panel: %s stress OFF (panel exit)\n", targets[i].name);
    }
    key_restore();
    show(L_OFF);
    actor_timer_stop(&timer);
    if (coid != -1)
        name_close(coid);
    printf("panel: stopped\n");
    return 0;
}
