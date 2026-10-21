// SPDX-License-Identifier: GPL-2.0
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "hal.h"
#include "hackpad.h"

#define TRADER_IP "192.168.1.101"
#define UDP_PORT 5000

#define POINTS 60
#define SEQ_TABLE_SIZE 1024

struct tick_packet {
	uint32_t seq;
} __attribute__((packed));

static lv_obj_t *title_lbl;
static lv_obj_t *stats_lbl;
static lv_obj_t *chart;
static lv_chart_series_t *ser;
static lv_obj_t *help_lbl;

static int udp_fd = -1;
static struct sockaddr_in trader_addr;

static uint32_t seq_no = 1;
static uint32_t tx_count;
static uint32_t rx_count;

static uint32_t seq_table[SEQ_TABLE_SIZE];
static uint64_t send_time_us[SEQ_TABLE_SIZE];

static uint32_t last_rtt_us;
static uint32_t min_us = 0xffffffff;
static uint32_t max_us;
static uint64_t sum_us;

static bool paused;
static int scale_idx = 1;

/* chart scale in microseconds */
static const int scales_us[] = {
	5000,   /* 5 ms */
	10000,  /* 10 ms */
	25000,  /* 25 ms */
	50000,  /* 50 ms */
};

static int green_ttl;
static int red_ttl;

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
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

	memset(&trader_addr, 0, sizeof(trader_addr));
	trader_addr.sin_family = AF_INET;
	trader_addr.sin_port = htons(UDP_PORT);

	if (inet_pton(AF_INET, TRADER_IP, &trader_addr.sin_addr) != 1) {
		perror("inet_pton");
		close(udp_fd);
		udp_fd = -1;
		return;
	}
}

static void reset_stats(void)
{
	tx_count = 0;
	rx_count = 0;

	last_rtt_us = 0;
	min_us = 0xffffffff;
	max_us = 0;
	sum_us = 0;

	seq_no = 1;

	memset(seq_table, 0, sizeof(seq_table));
	memset(send_time_us, 0, sizeof(send_time_us));

	lv_chart_set_all_value(chart, ser, 0);
	lv_chart_refresh(chart);

	green_ttl = 0;
	red_ttl = 4;
}

static void send_tick(void)
{
	if (udp_fd < 0 || paused)
		return;

	struct tick_packet pkt;

	pkt.seq = seq_no++;

	uint32_t idx = pkt.seq % SEQ_TABLE_SIZE;

	seq_table[idx] = pkt.seq;
	send_time_us[idx] = now_us();

	ssize_t n = sendto(udp_fd, &pkt, sizeof(pkt), 0,
			   (struct sockaddr *)&trader_addr,
			   sizeof(trader_addr));

	if (n == sizeof(pkt))
		tx_count++;
	else
		red_ttl = 4;
}

static void recv_echoes(void)
{
	if (udp_fd < 0)
		return;

	while (1) {
		struct tick_packet pkt;
		struct sockaddr_in src;
		socklen_t slen = sizeof(src);

		ssize_t n = recvfrom(udp_fd, &pkt, sizeof(pkt), 0,
				     (struct sockaddr *)&src, &slen);

		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			perror("recvfrom");
			red_ttl = 4;
			break;
		}

		if (n == sizeof(pkt)) {
			uint32_t idx = pkt.seq % SEQ_TABLE_SIZE;

			if (seq_table[idx] == pkt.seq) {
				uint64_t rtt = now_us() - send_time_us[idx];

				if (rtt > 999999)
					rtt = 999999;

				last_rtt_us = (uint32_t)rtt;

				if (last_rtt_us < min_us)
					min_us = last_rtt_us;

				if (last_rtt_us > max_us)
					max_us = last_rtt_us;

				sum_us += last_rtt_us;
				rx_count++;

				lv_chart_set_next_value(chart, ser, last_rtt_us);

				green_ttl = 3;
			}
		}
	}
}

static void create_ui(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	title_lbl = lv_label_create(scr);
	lv_obj_set_pos(title_lbl, 4, 3);
	lv_obj_set_width(title_lbl, 232);
	lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x55FF55), 0);

#if LV_FONT_UNSCII_8
	lv_obj_set_style_text_font(title_lbl, &lv_font_unscii_8, 0);
#else
	lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_10, 0);
#endif

	lv_label_set_text(title_lbl, "RETROQUANT/EXCHANGE");

	stats_lbl = lv_label_create(scr);
	lv_obj_set_pos(stats_lbl, 4, 18);
	lv_obj_set_width(stats_lbl, 232);
	lv_obj_set_style_text_color(stats_lbl, lv_color_hex(0x55FF55), 0);
	lv_obj_set_style_text_line_space(stats_lbl, 1, 0);

#if LV_FONT_UNSCII_8
	lv_obj_set_style_text_font(stats_lbl, &lv_font_unscii_8, 0);
#else
	lv_obj_set_style_text_font(stats_lbl, &lv_font_montserrat_10, 0);
#endif

	chart = lv_chart_create(scr);
	lv_obj_set_pos(chart, 4, 95);
	lv_obj_set_size(chart, 232, 105);

	lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(chart, POINTS);
	lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
	lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y,
				0, scales_us[scale_idx]);

	lv_obj_set_style_bg_color(chart, lv_color_black(), 0);
	lv_obj_set_style_border_color(chart, lv_color_hex(0x55FF55), 0);
	lv_obj_set_style_text_color(chart, lv_color_hex(0x55FF55), 0);
	lv_obj_set_style_line_color(chart, lv_color_hex(0x225522), LV_PART_MAIN);
	lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);

	ser = lv_chart_add_series(chart, lv_color_hex(0x55FF55),
				  LV_CHART_AXIS_PRIMARY_Y);

	help_lbl = lv_label_create(scr);
	lv_obj_set_pos(help_lbl, 4, 205);
	lv_obj_set_width(help_lbl, 232);
	lv_obj_set_style_text_color(help_lbl, lv_color_hex(0x55FF55), 0);

#if LV_FONT_UNSCII_8
	lv_obj_set_style_text_font(help_lbl, &lv_font_unscii_8, 0);
#else
	lv_obj_set_style_text_font(help_lbl, &lv_font_montserrat_10, 0);
#endif

	lv_label_set_text(help_lbl,
			  "SW1 reset  SW2 scale\n"
			  "SW3 pause  SW4 ping");
}

static void update_timer(lv_timer_t *t)
{
	LV_UNUSED(t);

	if (hal_button_pressed(HACKPAD_BTN_SW1)) {
		reset_stats();
	}

	if (hal_button_pressed(HACKPAD_BTN_SW2)) {
		scale_idx++;

		if (scale_idx >= (int)(sizeof(scales_us) / sizeof(scales_us[0])))
			scale_idx = 0;

		lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y,
					0, scales_us[scale_idx]);

		red_ttl = 2;
	}

	if (hal_button_pressed(HACKPAD_BTN_SW3)) {
		paused = !paused;
	}

	if (hal_button_pressed(HACKPAD_BTN_SW4)) {
		/* manual tick */
		send_tick();
	}

	recv_echoes();

	if (!paused)
		send_tick();

	uint32_t avg_us = rx_count ? (uint32_t)(sum_us / rx_count) : 0;
	uint32_t loss = tx_count > rx_count ? tx_count - rx_count : 0;

	if (loss > 20)
		red_ttl = 2;

	char buf[512];

	snprintf(buf, sizeof(buf),
		 "peer %s\n"
		 "tx %-5u rx %-5u loss %-3u\n"
		 "RTT %6u us\n"
		 "MIN %6u us\n"
		 "AVG %6u us\n"
		 "MAX %6u us\n"
		 "SCALE %2d ms  %s",
		 TRADER_IP,
		 tx_count,
		 rx_count,
		 loss,
		 last_rtt_us,
		 min_us == 0xffffffff ? 0 : min_us,
		 avg_us,
		 max_us,
		 scales_us[scale_idx] / 1000,
		 paused ? "PAUSED" : "LIVE");

	lv_label_set_text(stats_lbl, buf);

	hal_led(HACKPAD_LED_GREEN, green_ttl > 0);
	hal_led(HACKPAD_LED_RED, red_ttl > 0);
	hal_led(HACKPAD_LED_BLUE, paused);

	if (green_ttl > 0)
		green_ttl--;

	if (red_ttl > 0)
		red_ttl--;
}

int main(void)
{
	hal_init();

	create_ui();

	net_init();

	lv_timer_create(update_timer, 50, NULL);

	hal_run();

	return 0;
}
