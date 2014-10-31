/*
 * Copyright (c) 2014 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <dev/timer/arm_cortex_a9.h>

#include <debug.h>
#include <sys/types.h>
#include <err.h>
#include <stdio.h>
#include <assert.h>
#include <trace.h>
#include <lib/fixed_point.h>
#include <kernel/thread.h>
#include <platform.h>
#include <platform/interrupts.h>
#include <platform/timer.h>

/* driver for cortex-a9's private timer */
#define LOCAL_TRACE 0

/* interrupts for cortex-a9 timer and watchdog */
#define CPU_GLOB_TIMER_INT 27
#define CPU_PRIV_TIMER_INT 29
#define CPU_WATCHDOG_INT   30

/* offsets into the per cpu private scu base */
#define GLOBAL_TIMER_OFFSET (0x0200)
#define PRIV_TIMER_OFFSET   (0x0600)

#define TIMREG(reg)     (*REG32(scu_control_base + PRIV_TIMER_OFFSET + (reg)))

#define TIMER_LOAD      (0x00)
#define TIMER_COUNTER   (0x04)
#define TIMER_CONTROL   (0x08)
#define TIMER_ISR       (0x0c)
#define WDOG_LOAD       (0x20)
#define WDOG_COUNTER    (0x24)
#define WDOG_CONTROL    (0x28)
#define WDOG_ISR        (0x2c)

#define GTIMREG(reg)    (*REG32(scu_control_base + GLOBAL_TIMER_OFFSET + (reg)))

#define GTIMER_COUNT_LO (0x00)
#define GTIMER_COUNT_HI (0x04)
#define GTIMER_CONTROL  (0x08)
#define GTIMER_ISR      (0x0c)
#define GTIMER_COMPARE_LO (0x10)
#define GTIMER_COMPARE_HI (0x14)
#define GTIMER_INCREMENT (0x18)

static platform_timer_callback t_callback;
static addr_t scu_control_base;

static lk_time_t periodic_interval;
static lk_time_t oneshot_interval;
static uint32_t timer_freq;
static struct fp_32_64 timer_freq_msec_conversion;
static struct fp_32_64 timer_freq_usec_conversion_inverse;
static struct fp_32_64 timer_freq_msec_conversion_inverse;

uint64_t get_global_val(void)
{
    uint32_t lo, hi;

retry:
    hi = GTIMREG(GTIMER_COUNT_HI);
    lo = GTIMREG(GTIMER_COUNT_LO);
    if (GTIMREG(GTIMER_COUNT_HI) != hi)
        goto retry;

    return ((uint64_t)hi << 32 | lo);
}

lk_bigtime_t current_time_hires(void)
{
    lk_bigtime_t time;

    time = u64_mul_u64_fp32_64(get_global_val(), timer_freq_usec_conversion_inverse);

    return time;
}

lk_time_t current_time(void)
{
    lk_time_t time;

    time = u32_mul_u64_fp32_64(get_global_val(), timer_freq_msec_conversion_inverse);

    return time;
}

status_t platform_set_periodic_timer(platform_timer_callback callback, void *arg, lk_time_t interval)
{
    LTRACEF("callback %p, arg %p, interval %lu\n", callback, arg, interval);

    uint64_t ticks = u64_mul_u64_fp32_64(interval, timer_freq_msec_conversion);
    if (unlikely(ticks == 0))
        ticks = 1;
    if (unlikely(ticks > 0xffffffff))
        ticks = 0xffffffff;

    enter_critical_section();

    t_callback = callback;

    periodic_interval = interval;

    // disable timer
    TIMREG(TIMER_CONTROL) = 0;

    TIMREG(TIMER_LOAD) = ticks;
    TIMREG(TIMER_CONTROL) = (1<<2) | (1<<1) | (1<<0); // irq enable, autoreload, enable

    exit_critical_section();

    return NO_ERROR;
}

status_t platform_set_oneshot_timer (platform_timer_callback callback, void *arg, lk_time_t interval)
{
    LTRACEF("callback %p, arg %p, timeout %lu\n", callback, arg, interval);

    uint64_t ticks = u64_mul_u64_fp32_64(interval, timer_freq_msec_conversion);
    if (unlikely(ticks == 0))
        ticks = 1;
    if (unlikely(ticks > 0xffffffff))
        ticks = 0xffffffff;

    enter_critical_section();

    t_callback = callback;
    oneshot_interval = interval;

    // disable timer
    TIMREG(TIMER_CONTROL) = 0;

    TIMREG(TIMER_LOAD) = ticks;
    TIMREG(TIMER_CONTROL) = (1<<2) | (1<<0) | (1<<0); // irq enable, oneshot, enable

    exit_critical_section();

    return NO_ERROR;
}

void platform_stop_timer(void)
{
    LTRACE;

    TIMREG(TIMER_CONTROL) = 0;
}

static enum handler_return platform_tick(void *arg)
{
    LTRACE;

    TIMREG(TIMER_ISR) = 1; // ack the irq

    if (t_callback) {
        return t_callback(arg, current_time());
    } else {
        return INT_NO_RESCHEDULE;
    }
}

void arm_cortex_a9_timer_init(addr_t _scu_control_base, uint32_t freq)
{
    scu_control_base = _scu_control_base;

    /* disable timer */
    TIMREG(TIMER_CONTROL) = 0;

    /* kill the watchdog */
    TIMREG(WDOG_CONTROL) = 0;

    /* ack any irqs that may be pending */
    TIMREG(TIMER_ISR) = 1;

    /* save the timer frequency for later calculations */
    timer_freq = freq;

    /* precompute the conversion factor for global time to real time */
    fp_32_64_div_32_32(&timer_freq_msec_conversion, timer_freq, 1000);
    fp_32_64_div_32_32(&timer_freq_usec_conversion_inverse, 1000000, timer_freq);
    fp_32_64_div_32_32(&timer_freq_msec_conversion_inverse, 1000, timer_freq);

    register_int_handler(CPU_PRIV_TIMER_INT, &platform_tick, NULL);
    unmask_interrupt(CPU_PRIV_TIMER_INT);
}

/* vim: set ts=4 sw=4 expandtab: */
