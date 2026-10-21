# lkss-tick2trade

A two-node latency analyzer for embedded Linux, built on a pair of NXP FRDM-IMX93 boards fitted with the LKSS daughter board. One board plays the role of an exchange and emits a stream of small UDP "ticks"; the other plays a trader and echoes each tick back. The exchange measures the round trip, the trader shows what it costs the CPU to answer, and both report live on a 240x240 SPI display driven through the Linux framebuffer.

The name refers to *tick-to-trade* latency, the time a trading system needs to react to a market event. The project does not trade anything. It borrows that setting because it is a clean example of a latency-critical path that crosses every layer of a system: hardware, kernel driver, socket API, network stack, wire, and back. The question it is meant to make visible is where time goes along that path, and what the receiving side gives up to shorten it.

It was developed during the hackathon of NXP's Linux Kernel Summer School (LKSS) 2026. Internally, the two applications are called *RetroQuant/Exchange* and *RetroQuant/Trader*.

## Setup

Two physically separate embedded Linux nodes, connected by a direct Ethernet cable (no switch), each on a static IP in the same subnet:

**Board A (Exchange)** sends a minimal packet, a single packed 32-bit sequence number, to Board B on UDP port 5000, receives the echo, and computes the round-trip time. The latest samples are drawn on a scrolling `lv_chart`, alongside running MIN / AVG / MAX, transmitted and received counts, and a loss counter.

**Board B (Trader)** receives every tick and sends the identical packet back to its source, unconditionally. In parallel it displays CPU, memory and load average, the incoming message rate, and the time spent inside the `sendto` call of the echo. A physical button toggles the receive strategy between blocking (`poll`) and busy-polling, and the effect is immediately visible on the CPU bar.

## How it works

### Measuring latency without synchronized clocks

Comparing a send timestamp taken on one board with a receive timestamp taken on another would require the two clocks to agree to within a few microseconds, which in turn would require PTP or similar. The analyzer avoids the problem by measuring the round trip instead: both timestamps (`send` and `echo received`) are taken on Board A, with `CLOCK_MONOTONIC`, so clock offset and drift between the boards cancel out entirely. The cost is that the result is a round-trip figure; one-way latency can only be approximated as half of it, under the assumption of a symmetric path.

To match echoes to their ticks, each outgoing packet carries a sequence number, and its send time is stored in a 1024-entry ring indexed by `seq % 1024`. An echo is accepted only if the sequence number stored at that slot still matches, so late or stale echoes from a previous lap of the ring are discarded rather than producing a false sample. Ticks that never come back show up as the difference between the transmit and receive counters.

### Blocking vs. busy-poll on the receiver

The trader's network path runs in a dedicated thread, separate from the LVGL rendering loop, so drawing the UI never delays an echo. The socket is non-blocking, and the thread has two modes:

- **Poll (default).** The thread sleeps in `poll()` until the kernel signals that a datagram is pending, then drains the socket. The CPU is idle between packets, but each packet pays for a wake-up: the scheduler has to put the thread back on a core before the echo can be sent.
- **Busy-poll.** The thread skips `poll()` and calls `recvfrom()` in a tight loop. There is no wake-up to pay for, so the reaction is as fast as the core can spin, but the core is now fully occupied whether packets arrive or not.

This is the central trade-off of low-latency systems in miniature: CPU time is exchanged for reaction time. Switching modes on the board makes the exchange rate directly observable, with the CPU bar jumping as soon as busy-poll is enabled. The mode flag and all counters shared between the network thread and the UI thread are C11 atomics, so no locks sit on the echo path.

### From kernel driver to screen

Neither application touches hardware registers directly. The board-level path is layered as follows:

1. **Device tree.** A modified `imx93-11x11-frdm.dts` describes the daughter board (buttons and LEDs on GPIO, the ST7789 panel on LPSPI4, a BMP280 sensor) and enables the FEC Ethernet controller used for the link between the boards.
2. **Kernel drivers.** `hackpad.ko` exposes the four buttons and three LEDs as a misc character device, `/dev/hackpad`. Button edges are captured by GPIO interrupt handlers and queued in a `kfifo` under a spinlock; userspace consumes them through `read()` and `poll()`, and drives the LEDs through `ioctl()`. `st7789fb.ko` presents the SPI panel as a standard framebuffer, `/dev/fb0`, backed by a shadow buffer in RAM that the fbdev deferred-I/O mechanism flushes to the panel over SPI.
3. **Hardware abstraction layer.** `src/common/hal.c` binds LVGL's Linux fbdev backend to `/dev/fb0` and turns `/dev/hackpad` events into latched button presses and LED calls.
4. **Applications.** `chart.c` (Exchange) and `sysmon.c` (Trader).

The drivers and the HAL were provided by the LKSS lab materials and are included so the repository builds into a complete system; the applications, the measurement design and the board integration are the work of this project.

### Controls

| Button | Exchange (Board A)            | Trader (Board B)               |
|--------|-------------------------------|--------------------------------|
| SW1    | reset statistics              | —                              |
| SW2    | cycle chart scale (5–50 ms)   | —                              |
| SW3    | pause / resume the tick stream| toggle poll / busy-poll        |
| SW4    | send a single manual tick     | —                              |

The LEDs give a status reading at a glance: on the exchange, green flashes on each valid echo, red signals a send error or sustained loss, and blue indicates that the stream is paused.

## Methodology notes and limitations

Some properties of the current implementation bound what the numbers mean, and they are worth stating plainly.

- **RTT resolution is limited by the UI loop on Board A.** The exchange sends ticks and reads echoes from the same 50 ms LVGL timer callback, and its socket is non-blocking. An echo that arrives within microseconds is therefore not timestamped until the next callback, so the reported RTT is dominated by the timer period rather than by the network path. Moving the receive side of Board A into a dedicated thread blocking in `poll()`, as is already done on Board B, or using kernel receive timestamps (`SO_TIMESTAMPNS`), would bring the measurement down to the scale of the actual link.
- **The trader's "echo" figure covers only the send.** The value shown on Board B is the duration of the `sendto()` call that returns the packet, not the time from packet arrival to echo.
- **The trading display is synthetic.** The price, position, PnL and BUY signal on the trader screen are derived from the sequence number for presentation purposes and do not reflect any strategy. Only the echo itself carries meaning for the measurement.
- **Userspace timestamps.** All timestamps are taken in userspace, so they include syscall and scheduling overhead in addition to the network stack.

## Requirements

The project targets specific hardware and does not run on a generic Linux machine. It needs:

- two FRDM-IMX93 boards, each with the LKSS daughter board, and an Ethernet cable between them;
- a kernel built with the device tree in `dts/` (FEC support enabled);
- `hackpad.ko` and `st7789fb.ko` compiled for that kernel and loaded with `modprobe`;
- LVGL 9 with the Linux fbdev backend, cross-compiled for the target;
- a static IP on each board, in the same subnet. The trader's address is set by `TRADER_IP` in `src/chart.c`.

## Repository layout

```
dts/              device tree for the FRDM-IMX93 + LKSS daughter board
kernel-drivers/   hackpad (buttons/LEDs) and st7789fb (SPI framebuffer) drivers
src/common/       hardware abstraction layer and the hackpad userspace API
src/chart.c       Board A: exchange, tick generator and RTT analyzer
src/sysmon.c      Board B: trader, echo server and system monitor
```

## References

- Michael Kerrisk, *The Linux Programming Interface*: sockets, `poll`, POSIX threads, clocks and timers.
- The Linux kernel documentation for the GPIO consumer interface, misc devices, `kfifo`, and fbdev deferred I/O.
- The `socket(7)`, `udp(7)`, `poll(2)` and `clock_gettime(2)` man pages.
- The LVGL 9 documentation, for the Linux framebuffer backend and the chart widget.
- The NXP LKSS 2026 lab materials, on which the drivers and HAL are based.
  
