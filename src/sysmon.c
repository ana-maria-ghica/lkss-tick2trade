#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include "hal.h"
#include "hackpad.h"

#define UDP_PORT 5000

struct tick_packet {
    uint32_t seq;
} __attribute__((packed));

static lv_obj_t *term_label;
static lv_style_t term_style;

static int udp_fd = -1;
static pthread_t net_tid;

static atomic_bool busy_poll = ATOMIC_VAR_INIT(false);
static atomic_uint msg_counter = ATOMIC_VAR_INIT(0);
static atomic_uint msg_sec = ATOMIC_VAR_INIT(0);
static atomic_uint total_packets = ATOMIC_VAR_INIT(0);
static atomic_uint last_seq = ATOMIC_VAR_INIT(0);
static atomic_uint echo_us = ATOMIC_VAR_INIT(0);
static atomic_int strategy_ttl = ATOMIC_VAR_INIT(0);

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static void make_bar(char out[11], int pct)
{
    int filled = pct / 10;

    if (filled < 0)
        filled = 0;
    if (filled > 10)
        filled = 10;

    for (int i = 0; i < 10; i++)
        out[i] = (i < filled) ? '#' : '.';

    out[10] = '\0';
}

static int read_cpu_percent(void)
{
    static unsigned long long prev_total = 0;
    static unsigned long long prev_idle = 0;

    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return 0;

    unsigned long long user, nice, system, idle, iowait;
    unsigned long long irq, softirq, steal;

    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &system, &idle, &iowait,
                   &irq, &softirq, &steal);
    fclose(f);

    if (n < 8)
        return 0;

    unsigned long long idle_all = idle + iowait;
    unsigned long long total = user + nice + system + idle + iowait +
                               irq + softirq + steal;

    if (prev_total == 0) {
        prev_total = total;
        prev_idle = idle_all;
        return 0;
    }

    unsigned long long total_delta = total - prev_total;
    unsigned long long idle_delta = idle_all - prev_idle;

    prev_total = total;
    prev_idle = idle_all;

    if (total_delta == 0)
        return 0;

    int usage = (int)((total_delta - idle_delta) * 100 / total_delta);

    if (usage < 0)
        usage = 0;
    if (usage > 100)
        usage = 100;

    return usage;
}

static int read_mem_percent(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;

    char key[64];
    char unit[32];
    long value;
    long total = -1;
    long available = -1;

    while (fscanf(f, "%63s %ld %31s", key, &value, unit) == 3) {
        if (strcmp(key, "MemTotal:") == 0)
            total = value;
        else if (strcmp(key, "MemAvailable:") == 0)
            available = value;

        if (total > 0 && available > 0)
            break;
    }

    fclose(f);

    if (total <= 0 || available < 0)
        return 0;

    int used = (int)((total - available) * 100 / total);

    if (used < 0)
        used = 0;
    if (used > 100)
        used = 100;

    return used;
}

static double read_loadavg(void)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f)
        return 0.0;

    double load = 0.0;
    fscanf(f, "%lf", &load);
    fclose(f);

    return load;
}

static unsigned long read_uptime_sec(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    if (!f)
        return 0;

    double up = 0.0;
    fscanf(f, "%lf", &up);
    fclose(f);

    return (unsigned long)up;
}

static void *net_thread(void *arg)
{
    (void)arg;

    uint8_t buf[64];

    while (1) {
        if (!atomic_load(&busy_poll)) {
            struct pollfd pfd = {
                .fd = udp_fd,
                .events = POLLIN,
                .revents = 0,
            };

            int r = poll(&pfd, 1, 100);
            if (r <= 0)
                continue;
        }

        while (1) {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);

            ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&src, &slen);

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;

                perror("recvfrom");
                break;
            }

            if (n > 0) {
                uint32_t seq = 0;

                if (n >= (ssize_t)sizeof(struct tick_packet))
                    memcpy(&seq, buf, sizeof(seq));

                uint64_t t0 = now_us();

                /*
                 * Important: ecou exact același pachet către Board A.
                 * Nu condiționăm de BUY/IDLE.
                 */
                sendto(udp_fd, buf, n, 0,
                       (struct sockaddr *)&src, slen);

                uint64_t dt = now_us() - t0;

                atomic_store(&last_seq, seq);
                atomic_store(&echo_us, (unsigned int)dt);
                atomic_fetch_add(&msg_counter, 1);

                unsigned int total = atomic_fetch_add(&total_packets, 1) + 1;

                if (total % 20 == 0)
                    atomic_store(&strategy_ttl, 4);
            }
        }
    }

    return NULL;
}

static void net_init(void)
{
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        perror("socket");
        return;
    }

    int yes = 1;
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));

    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(UDP_PORT);

    if (bind(udp_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        perror("bind");
        close(udp_fd);
        udp_fd = -1;
        return;
    }

    int flags = fcntl(udp_fd, F_GETFL, 0);
    fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);

    if (pthread_create(&net_tid, NULL, net_thread, NULL) != 0) {
        perror("pthread_create");
        close(udp_fd);
        udp_fd = -1;
        return;
    }

    pthread_detach(net_tid);
}

static void create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_style_init(&term_style);
    lv_style_set_text_color(&term_style, lv_color_hex(0x55FF55));
    lv_style_set_bg_opa(&term_style, LV_OPA_TRANSP);
    lv_style_set_text_letter_space(&term_style, 0);
    lv_style_set_text_line_space(&term_style, 1);

#if LV_FONT_UNSCII_8
    lv_style_set_text_font(&term_style, &lv_font_unscii_8);
#elif LV_FONT_MONTSERRAT_10
    lv_style_set_text_font(&term_style, &lv_font_montserrat_10);
#endif

    term_label = lv_label_create(scr);
    lv_obj_add_style(term_label, &term_style, 0);
    lv_obj_set_pos(term_label, 4, 4);
    lv_obj_set_width(term_label, 232);
    lv_label_set_long_mode(term_label, LV_LABEL_LONG_CLIP);

    lv_label_set_text(term_label, "RETROQUANT/TRADER\nbooting...");
}

static void update_ui_timer(lv_timer_t *timer)
{
    (void)timer;

    static uint64_t last_sec_ms = 0;

    if (hal_button_pressed(HACKPAD_BTN_SW3)) {
        bool old = atomic_load(&busy_poll);
        atomic_store(&busy_poll, !old);
    }

    uint64_t t = now_ms();

    if (last_sec_ms == 0)
        last_sec_ms = t;

    if (t - last_sec_ms >= 1000) {
        unsigned int v = atomic_exchange(&msg_counter, 0);
        atomic_store(&msg_sec, v);
        last_sec_ms = t;
    }

    int cpu = read_cpu_percent();
    int mem = read_mem_percent();
    double load = read_loadavg();

    char cpu_bar[11];
    char mem_bar[11];

    make_bar(cpu_bar, cpu);
    make_bar(mem_bar, mem);

    unsigned long up = read_uptime_sec();
    unsigned long h = up / 3600;
    unsigned long m = (up / 60) % 60;
    unsigned long s = up % 60;

    bool mode_busy = atomic_load(&busy_poll);

    int ttl = atomic_load(&strategy_ttl);
    bool buy = ttl > 0;

    if (ttl > 0)
        atomic_fetch_sub(&strategy_ttl, 1);

    hal_led(HACKPAD_LED_GREEN, buy);

    unsigned int net = atomic_load(&msg_sec);
    unsigned int seq = atomic_load(&last_seq);
    unsigned int eus = atomic_load(&echo_us);

    double price = 104.00 + (seq % 100) * 0.01;
    double pnl = ((int)(seq % 200) - 100) / 10.0;
    double sig = buy ? 0.85 : 0.12;

    const char *mode = mode_busy ? "BUSY" : "POLL";
    const char *strat = buy ? "EXEC BUY" : "IDLE";
    const char *signal = buy ? "BUY" : "WAIT";

    char buf[768];

    snprintf(buf, sizeof(buf),
        "RETROQUANT/TRADER\n"
        "up %02lu:%02lu:%02lu c%3d m%3d\n"
        "CPU [%s] %3d%%\n"
        "MEM [%s] %3d%%\n"
        "load %.2f net %4u/s\n"
        "----------------------------\n"
        "PRICE  %7.2f ACTIVE\n"
        "POS    %-7s FILLED\n"
        "PNL    %+7.2f %-5s\n"
        "SIG    %-7s %.2f\n"
        "ECHO   %4u us OK\n"
        "MODE   %-7s SW3\n"
        "SEQ    %-7u\n"
        "----------------------------\n"
        "STRAT  %-7s\n",
        h, m, s,
        cpu, mem,
        cpu_bar, cpu,
        mem_bar, mem,
        load, net,
        price,
        seq ? "LONG10" : "NONE",
        pnl, pnl >= 0 ? "UP" : "DOWN",
        signal, sig,
        eus,
        mode,
        seq,
        strat
    );

    lv_label_set_text(term_label, buf);
}

int main(void)
{
    hal_init();

    create_ui();

    net_init();

    lv_timer_create(update_ui_timer, 250, NULL);

    hal_run();

    return 0;
}
