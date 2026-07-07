# lkss-tick2trade

Tick-to-trade latency analyzer built on two FRDM-IMX93 + LKSS Daughter Board setups, developed during the hackathon of NXP's Linux Kernel Summer School (LKSS) 2026.

The main configuration:
Two fully separate embedded Linux nodes communicate over a direct Ethernet cable
The goal is to trace a full system path - from a hardware interrupt (a physical button on the hackpad), through the kernel driver, through userspace, through a UDP socket, over the network, and back - and measure where time is spent at each stage.

Board A (Exchange) - periodically sends a minimal UDP packet (a "tick") to Board B, receives the echo back, and computes round-trip latency using a single local clock, with no need for clock synchronization between the two boards. Latency is displayed live on a scrolling chart (lv_chart), along with MIN/MAX/AVG stats. 

Board B (Trader) - receives every tick and echoes it back immediately, unconditionally. In parallel, it displays the board's CPU/memory usage and lets you toggle between blocking (poll) and busy-poll network reads via a physical button - visibly reflected on the CPU bar.


Prerequisites
This project does not run standalone on a generic Linux machine. It requires:

- an FRDM-IMX93 board with the LKSS Daughter Board
- a kernel built with the device tree in dts/ (FEC support enabled)
- hackpad.ko and st7789fb.ko compiled for that kernel and loaded (modprobe)
- LVGL cross-compiled for the target
- static IP configured on each board, on the same subnet
