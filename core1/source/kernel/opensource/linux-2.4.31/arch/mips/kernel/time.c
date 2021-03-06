/*
 * Copyright 2001 MontaVista Software Inc.
 * Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 * Copyright (c) 2003, 2004  Maciej W. Rozycki
 *
 * Common time service routines for MIPS machines. See
 * Documentation/mips/time.README.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/param.h>
#include <linux/time.h>
#include <linux/timex.h>
#include <linux/smp.h>
#include <linux/kernel_stat.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/hrtime.h>

#include <linux/trace.h>

#include <asm/bootinfo.h>
#include <asm/compiler.h>
#include <asm/cpu.h>
#include <asm/time.h>
#include <asm/hardirq.h>
#include <asm/div64.h>
#include <asm/debug.h>

#ifdef CONFIG_ILATENCY
#include <linux/ilatency.h>
#define ILATENCY_RET_FROM_EXCEPTION() intr_ret_from_exception()
#else
#define ILATENCY_RET_FROM_EXCEPTION()
#endif


/*
 * The integer part of the number of usecs per jiffy is taken from tick,
 * but the fractional part is not recorded, so we calculate it using the
 * initial value of HZ.  This aids systems where tick isn't really an
 * integer (e.g. for HZ = 128).
 */
#define USECS_PER_JIFFY		(1000000/HZ)
#define USECS_PER_JIFFY_FRAC	((unsigned long)(u32)((1000000ULL << 32) / HZ))

/*
 * forward reference
 */
extern rwlock_t xtime_lock;
extern volatile unsigned long wall_jiffies;

spinlock_t rtc_lock = SPIN_LOCK_UNLOCKED;

/*
 * whether we emulate local_timer_interrupts for SMP machines.
 */
int emulate_local_timer_interrupt;


/*
 * By default we provide the null RTC ops
 */
static unsigned long null_rtc_get_time(void)
{
	return mktime(2000, 1, 1, 0, 0, 0);
}

static int null_rtc_set_time(unsigned long sec)
{
	return 0;
}

unsigned long (*rtc_get_time)(void) = null_rtc_get_time;
int (*rtc_set_time)(unsigned long) = null_rtc_set_time;
int (*rtc_set_mmss)(unsigned long);


/* usecs per counter cycle, shifted to left by 32 bits */
static unsigned int sll32_usecs_per_cycle;

/* how many counter cycles in a jiffy */
unsigned long cycles_per_jiffy;

/* Cycle counter value at the previous timer interrupt.. */
static unsigned int timerhi, timerlo;

/* expirelo is the count value for next CPU timer interrupt */
static unsigned int expirelo;


/*
 * Null timer ack for systems not needing one (e.g. i8254).
 */
static void null_timer_ack(void) { /* nothing */ }

/*
 * Null high precision timer functions for systems lacking one.
 */
static unsigned int null_hpt_read(void)
{
	return 0;
}

static void null_hpt_init(unsigned int count) { /* nothing */ }


/*
 * Timer ack for an R4k-compatible timer of a known frequency.
 */
static void c0_timer_ack(void)
{
	unsigned int count;

	/* Ack this timer interrupt and set the next one.  */
	expirelo += cycles_per_jiffy;
	write_c0_compare(expirelo);

	/* Check to see if we have missed any timer interrupts.  */
	count = read_c0_count();
	if ((count - expirelo) < 0x7fffffff) {
		/* missed_timer_count++; */
		expirelo = count + cycles_per_jiffy;
		write_c0_compare(expirelo);
	}
}

/*
 * High precision timer functions for a R4k-compatible timer.
 */
static unsigned int c0_hpt_read(void)
{
	return read_c0_count();
}

/* For use solely as a high precision timer.  */
static void c0_hpt_init(unsigned int count)
{
	write_c0_count(read_c0_count() - count);
}

/* For use both as a high precision timer and an interrupt source.  */
static void c0_hpt_timer_init(unsigned int count)
{
	count = read_c0_count() - count;
	expirelo = (count / cycles_per_jiffy + 1) * cycles_per_jiffy;
	write_c0_count(expirelo - cycles_per_jiffy);
	write_c0_compare(expirelo);
	write_c0_count(count);
}

int (*mips_timer_state)(void);
void (*mips_timer_ack)(void);
unsigned int (*mips_hpt_read)(void);
void (*mips_hpt_init)(unsigned int);


/*
 * timeofday services, for syscalls.
 */
void do_gettimeofday(struct timeval *tv)
{
	unsigned long flags, lost;

	read_lock_irqsave(&xtime_lock, flags);

	*tv = xtime;
	tv->tv_usec += do_gettimeoffset();
	/*
	 * xtime is atomically updated in timer_bh.  jiffies - wall_jiffies
	 * is nonzero if the timer bottom half hasn't executed yet.
	 */
	lost = jiffies - wall_jiffies;
	if (lost)
		tv->tv_usec += lost * USECS_PER_JIFFY;

	read_unlock_irqrestore(&xtime_lock, flags);

	while (tv->tv_usec >= 1000000) {
		tv->tv_usec -= 1000000;
		tv->tv_sec++;
	}
}

void do_settimeofday(struct timeval *tv)
{
	write_lock_irq(&xtime_lock);

	/*
	 * This is revolting.  We need to set "xtime" correctly.  However,
	 * the value in this location is the value at the most recent update
	 * of wall time.  Discover what correction gettimeofday() would have
	 * made, and then undo it!
	 */
	tv->tv_usec -= do_gettimeoffset();
	tv->tv_usec -= (jiffies - wall_jiffies) * USECS_PER_JIFFY;

	while (tv->tv_usec < 0) {
		tv->tv_usec += 1000000;
		tv->tv_sec--;
	}

        wall_to_monotonic.tv_sec += xtime.tv_sec - tv->tv_sec;
        wall_to_monotonic.tv_nsec += (xtime.tv_usec - tv->tv_usec) *
                NSEC_PER_USEC;

        if (wall_to_monotonic.tv_nsec > NSEC_PER_SEC) {
                wall_to_monotonic.tv_nsec -= NSEC_PER_SEC;
                wall_to_monotonic.tv_sec++;
        }
        if (wall_to_monotonic.tv_nsec < 0) {
                wall_to_monotonic.tv_nsec += NSEC_PER_SEC;
                wall_to_monotonic.tv_sec--;
        }

	xtime = *tv;
	time_adjust = 0;			/* stop active adjtime() */
	time_status |= STA_UNSYNC;
	time_maxerror = NTP_PHASE_LIMIT;
	time_esterror = NTP_PHASE_LIMIT;

	write_unlock_irq(&xtime_lock);

        clock_was_set();
}


/*
 * Gettimeoffset routines.  These routines returns the time duration
 * since last timer interrupt in usecs.
 *
 * If the exact CPU counter frequency is known, use fixed_rate_gettimeoffset.
 * Otherwise use calibrate_gettimeoffset()
 *
 * If the CPU does not have the counter register, you can either supply
 * your own gettimeoffset() routine, or use null_gettimeoffset(), which
 * gives the same resolution as HZ.
 */

static unsigned long null_gettimeoffset(void)
{
	return 0;
}


/* The function pointer to one of the gettimeoffset funcs.  */
unsigned long (*do_gettimeoffset)(void) = null_gettimeoffset;


static unsigned long fixed_rate_gettimeoffset(void)
{
	u32 count;
	unsigned long res;

	/* Get last timer tick in absolute kernel time */
	count = mips_hpt_read();

	/* .. relative to previous jiffy (32 bits is enough) */
	count -= timerlo;

	__asm__("multu	%1,%2"
		: "=h" (res)
		: "r" (count), "r" (sll32_usecs_per_cycle)
		: "lo", GCC_REG_ACCUM);

	/*
	 * Due to possible jiffies inconsistencies, we need to check
	 * the result so that we'll get a timer that is monotonic.
	 */
	if (res >= USECS_PER_JIFFY)
		res = USECS_PER_JIFFY - 1;

	return res;
}


/*
 * Cached "1/(clocks per usec) * 2^32" value.
 * It has to be recalculated once each jiffy.
 */
static unsigned long cached_quotient;

/* Last jiffy when calibrate_divXX_gettimeoffset() was called. */
static unsigned long last_jiffies;

/*
 * This is moved from dec/time.c:do_ioasic_gettimeoffset() by Maciej.
 */
static unsigned long calibrate_div32_gettimeoffset(void)
{
	u32 count;
	unsigned long res, tmp;
	unsigned long quotient;

	tmp = jiffies;

	quotient = cached_quotient;

	if (last_jiffies != tmp) {
		last_jiffies = tmp;
		if (last_jiffies != 0) {
			unsigned long r0;
			do_div64_32(r0, timerhi, timerlo, tmp);
			do_div64_32(quotient, USECS_PER_JIFFY,
				    USECS_PER_JIFFY_FRAC, r0);
			cached_quotient = quotient;
		}
	}

	/* Get last timer tick in absolute kernel time */
	count = mips_hpt_read();

	/* .. relative to previous jiffy (32 bits is enough) */
	count -= timerlo;

	__asm__("multu  %1,%2"
		: "=h" (res)
		: "r" (count), "r" (quotient)
		: "lo", GCC_REG_ACCUM);

	/*
	 * Due to possible jiffies inconsistencies, we need to check
	 * the result so that we'll get a timer that is monotonic.
	 */
	if (res >= USECS_PER_JIFFY)
		res = USECS_PER_JIFFY - 1;

	return res;
}

static unsigned long calibrate_div64_gettimeoffset(void)
{
	u32 count;
	unsigned long res, tmp;
	unsigned long quotient;

	tmp = jiffies;

	quotient = cached_quotient;

	if (last_jiffies != tmp) {
		last_jiffies = tmp;
		if (last_jiffies) {
			unsigned long r0;
			__asm__(".set	push\n\t"
				".set	mips3\n\t"
				"lwu	%0,%3\n\t"
				"dsll32	%1,%2,0\n\t"
				"or	%1,%1,%0\n\t"
				"ddivu	$0,%1,%4\n\t"
				"mflo	%1\n\t"
				"dsll32	%0,%5,0\n\t"
				"or	%0,%0,%6\n\t"
				"ddivu	$0,%0,%1\n\t"
				"mflo	%0\n\t"
				".set	pop"
				: "=&r" (quotient), "=&r" (r0)
				: "r" (timerhi), "m" (timerlo),
				  "r" (tmp), "r" (USECS_PER_JIFFY),
				  "r" (USECS_PER_JIFFY_FRAC)
				: "hi", "lo", GCC_REG_ACCUM);
			cached_quotient = quotient;
		}
	}

	/* Get last timer tick in absolute kernel time */
	count = mips_hpt_read();

	/* .. relative to previous jiffy (32 bits is enough) */
	count -= timerlo;

	__asm__("multu	%1,%2"
		: "=h" (res)
		: "r" (count), "r" (quotient)
		: "lo", GCC_REG_ACCUM);

	/*
	 * Due to possible jiffies inconsistencies, we need to check
	 * the result so that we'll get a timer that is monotonic.
	 */
	if (res >= USECS_PER_JIFFY)
		res = USECS_PER_JIFFY - 1;

	return res;
}


#if defined(CONFIG_MIPS_COMMON_HRT)
int hr_time_resolution = CONFIG_HIGH_RES_RESOLUTION;

int get_arch_cycles(unsigned long ref_jiffies)
{
        int ret;
        int temp;
        unsigned long old_jiffies;
        unsigned long jiffy_expire;

        /* loop until we get an consistent reading of both vars */
        do {
                old_jiffies = jiffies;
                jiffy_expire = expirelo;
        } while (old_jiffies != jiffies);

        db_assert(time_before_eq(ref_jiffies, old_jiffies));
        db_assert(time_after_eq(ref_jiffies + 10, old_jiffies));

        ret = read_c0_count() + arch_cycles_per_jiffy - jiffy_expire;
        db_assert(ret > 0);
        if (unlikely(temp=old_jiffies - ref_jiffies))
                ret += temp * arch_cycles_per_jiffy;

        return ret;
}

static inline unsigned
calc_next_expire(unsigned ref_jiffies, unsigned cycles)
{
        unsigned ret;
        int temp;
        unsigned long old_jiffies;
        unsigned long jiffy_expire;

        /* loop until we get an consistent reading of both vars */
        do {
                old_jiffies = jiffies;
                jiffy_expire = expirelo;
        } while (old_jiffies != jiffies);

        /* they should be off too much */
        db_assert(time_before_eq(ref_jiffies, old_jiffies+10));
        db_assert(time_after_eq(ref_jiffies + 10, old_jiffies));

        ret = jiffy_expire - arch_cycles_per_jiffy + cycles;
        if (unlikely(temp=ref_jiffies - old_jiffies))
                ret += temp * arch_cycles_per_jiffy;

        return ret;
}

int schedule_hr_timer_int(unsigned long ref_jiffies, int cycles)
{
        unsigned count;

        db_assert(intr_off());
        db_assert(time_before_eq(ref_jiffies, jiffies+10));
        db_assert(time_after_eq(ref_jiffies + 10, jiffies));
        db_assert(cycles <= arch_cycles_per_jiffy);

        count = (calc_next_expire(ref_jiffies, (unsigned)cycles));
        write_c0_compare(count);
        return time_after(read_c0_count(), count);
}

static inline void schedule_timer_int_asap(void)
{
        unsigned inc = 0;
        unsigned inc_base=2;
        unsigned compare;

        db_assert(intr_off());

        do {
                inc += inc_base;
                compare = read_c0_count() + inc;
                write_c0_compare(compare);
        } while (time_after(read_c0_count(), compare));
}

int schedule_jiffies_int(unsigned long ref_jiffies)
{
        int ret = schedule_hr_timer_int(ref_jiffies, arch_cycles_per_jiffy);
        if (ret)
                schedule_timer_int_asap();
        return ret;
}
/*
 * return (u32)( ((u64)x * (u64)y) >> shift ), where 0 < shift <= 32
 */
static inline int scaled_mult(int x, int y, int shift)
{
        int hi;
        unsigned lo;

        db_assert(shift <= 32);
        db_assert(shift > 0);

        __asm__("mult\t%2,%3\n\t"
                "mfhi\t%0\n\t"
                "mflo\t%1"
                :"=r" (hi), "=r" (lo)
                :"r" (x), "r" (y));

        if (shift == 32)
                return hi;
        else
                return (hi << (32 - shift)) | (lo >> shift);
}

#define nsec2cycle_shift        32
#define cycle2nsec_shift        24
unsigned scaled_cycles_per_nsec, scaled_nsecs_per_cycle;

int nsec_to_arch_cycle(int nsec)
{
        return scaled_mult(nsec, scaled_cycles_per_nsec, nsec2cycle_shift);
}

int arch_cycle_to_nsec(int cycles)
{
        return scaled_mult(cycles, scaled_nsecs_per_cycle, cycle2nsec_shift);
}


static void hr_time_init(void)
{
        u64 temp;

        /* make sure no overflow for the scale factors */
        db_assert((mips_hpt_frequency >> (32- nsec2cycle_shift)) <
                        1000000000);
        db_assert((1000000000 >> (32 - cycle2nsec_shift)) <
                        mips_hpt_frequency);

        /*
         * the current setting allow counter freq to range from
         * a few MHz to 1GHz.
         */

        temp = (u64)mips_hpt_frequency << nsec2cycle_shift;
        do_div64_32(scaled_cycles_per_nsec, (u32)(temp >> 32), (u32)temp, 1000000000);

        temp = (u64) 1000000000 << cycle2nsec_shift;
        do_div64_32(scaled_nsecs_per_cycle, (u32)(temp >> 32), (u32)temp, mips_hpt_frequency);
}

static unsigned long hrt_gettimeoffset(void)
{
        int ret;

        /* xtime_lock should be held if we are called */
        ret = get_arch_cycles(jiffies);
        ret = arch_cycle_to_nsec(ret) / 1000;

        /*
        if (ret >= USECS_PER_JIFFY)
                ret = USECS_PER_JIFFY-1;
        */
        return ret;
}
#endif /* defined(CONFIG_MIPS_COMMON_HRT) */


/* last time when xtime and rtc are sync'ed up */
static long last_rtc_update;

/*
 * local_timer_interrupt() does profiling and process accounting
 * on a per-CPU basis.
 *
 * In UP mode, it is invoked from the (global) timer_interrupt.
 *
 * In SMP mode, it might invoked by per-CPU timer interrupt, or
 * a broadcasted inter-processor interrupt which itself is triggered
 * by the global timer interrupt.
 */
void local_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	if (!user_mode(regs)) {
		if (prof_buffer && current->pid) {
			extern int _stext;
			unsigned long pc = regs->cp0_epc;

			pc -= (unsigned long) &_stext;
			pc >>= prof_shift;
			/*
			 * Dont ignore out-of-bounds pc values silently,
			 * put them into the last histogram slot, so if
			 * present, they will show up as a sharp peak.
			 */
			if (pc > prof_len - 1)
				pc = prof_len - 1;
			atomic_inc((atomic_t *)&prof_buffer[pc]);
		}
	}

#ifdef CONFIG_SMP
	/* in UP mode, update_process_times() is invoked by do_timer() */
	update_process_times(user_mode(regs));
#endif
}

/*
 * High-level timer interrupt service routines.  This function
 * is set as irqaction->handler and is invoked through do_IRQ.
 */

static inline void do_timer_interrupt(struct pt_regs *regs)
{
        do_timer(regs);
        local_timer_interrupt(0, NULL, regs);
}

void timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned long j;
	unsigned int count;

	count = mips_hpt_read();


#if defined(CONFIG_HIGH_RES_TIMERS)
                int do_timer_flag=0;
                for(;;) {
                        write_c0_compare(expirelo);
                        count = read_c0_count();
                        if (time_before_eq(count, expirelo))
                                break;

                        do_timer_interrupt(regs);
                        expirelo += cycles_per_jiffy;
                        do_timer_flag++;
                }

       /* if we have not done any jiffy interrupts, this must be a hrt int */
       if (!do_timer_flag) {
//               write_sequnlock(&xtime_lock);
               write_c0_compare(read_c0_compare());    /* ack the interrupt */
               do_hr_timer_int();
               return IRQ_HANDLED;
       }

#else 

	mips_timer_ack();

	do_timer(regs);
#endif

	/* Update timerhi/timerlo for intra-jiffy calibration. */
	timerhi += count < timerlo;			/* Wrap around */
	timerlo = count;

	/*
	 * If we have an externally synchronized Linux clock, then update
	 * CMOS clock accordingly every ~11 minutes. rtc_set_time() has to be
	 * called as close as possible to 500 ms before the new second starts.
	 */
	read_lock(&xtime_lock);
	if ((time_status & STA_UNSYNC) == 0 &&
	    xtime.tv_sec > last_rtc_update + 660 &&
	    xtime.tv_usec >= 500000 - ((unsigned) tick) / 2 &&
	    xtime.tv_usec <= 500000 + ((unsigned) tick) / 2) {
		if (rtc_set_mmss(xtime.tv_sec) == 0) {
			last_rtc_update = xtime.tv_sec;
		} else {
			/* do it again in 60 s */
			last_rtc_update = xtime.tv_sec - 600;
		}
	}
	read_unlock(&xtime_lock);

	/*
	 * If jiffies has overflown in this timer_interrupt, we must
	 * update the timer[hi]/[lo] to make fast gettimeoffset funcs
	 * quotient calc still valid. -arca
	 *
	 * The first timer interrupt comes late as interrupts are
	 * enabled long after timers are initialized.  Therefore the
	 * high precision timer is fast, leading to wrong gettimeoffset()
	 * calculations.  We deal with it by setting it based on the
	 * number of its ticks between the second and the third interrupt.
	 * That is still somewhat imprecise, but it's a good estimate.
	 * --macro
	 */
	j = jiffies;
	if (j < 4) {
		static unsigned int prev_count;
		static int hpt_initialized;

		switch (j) {
		case 0:
			timerhi = timerlo = 0;
			mips_hpt_init(count);
			break;
		case 2:
			prev_count = count;
			break;
		case 3:
			if (!hpt_initialized) {
				unsigned int c3 = 3 * (count - prev_count);

				timerhi = 0;
				timerlo = c3;
				mips_hpt_init(count - c3);
				hpt_initialized = 1;
			}
			break;
		default:
			break;
		}
	}

}

asmlinkage void ll_timer_interrupt(int irq, struct pt_regs *regs)
{
	int cpu = smp_processor_id();

	TRACE_IRQ_ENTRY(irq, ((regs->cp0_status & ST0_KSU) == KSU_KERNEL));

	irq_enter(cpu, irq);
	kstat.irqs[cpu][irq]++;

	/* we keep interrupt disabled all the time */
	timer_interrupt(irq, NULL, regs);

	irq_exit(cpu, irq);

	TRACE_IRQ_EXIT();

	if (softirq_pending(cpu))
		do_softirq();
        ILATENCY_RET_FROM_EXCEPTION();
}

asmlinkage void ll_local_timer_interrupt(int irq, struct pt_regs *regs)
{
	int cpu = smp_processor_id();

	irq_enter(cpu, irq);

	if (cpu != 0)
		kstat.irqs[cpu][irq]++;

	/* we keep interrupt disabled all the time */
	local_timer_interrupt(irq, NULL, regs);

	irq_exit(cpu, irq);

	if (softirq_pending(cpu))
		do_softirq();
}

/*
 * time_init() - it does the following things.
 *
 * 1) board_time_init() -
 * 	a) (optional) set up RTC routines,
 *      b) (optional) calibrate and set the mips_hpt_frequency
 *	    (only needed if you intended to use fixed_rate_gettimeoffset
 *	     or use cpu counter as timer interrupt source)
 * 2) setup xtime based on rtc_get_time().
 * 3) choose a appropriate gettimeoffset routine.
 * 4) calculate a couple of cached variables for later usage
 * 5) board_timer_setup() -
 *	a) (optional) over-write any choices made above by time_init().
 *	b) machine specific code should setup the timer irqaction.
 *	c) enable the timer interrupt
 */

void (*board_time_init)(void);
void (*board_timer_setup)(struct irqaction *irq);

unsigned int mips_hpt_frequency;

static struct irqaction timer_irqaction = {
	.handler = timer_interrupt,
	.flags = SA_INTERRUPT,
	.name = "timer",
};

static unsigned int __init calibrate_hpt(void)
{
	u64 frequency;
	u32 hpt_start, hpt_end, hpt_count, hz;

	const int loops = HZ / 10;
	int log_2_loops = 0;
	int i;

	/*
	 * We want to calibrate for 0.1s, but to avoid a 64-bit
	 * division we round the number of loops up to the nearest
	 * power of 2.
	 */
	while (loops > 1 << log_2_loops)
		log_2_loops++;
	i = 1 << log_2_loops;

	/*
	 * Wait for a rising edge of the timer interrupt.
	 */
	while (mips_timer_state());
	while (!mips_timer_state());

	/*
	 * Now see how many high precision timer ticks happen
	 * during the calculated number of periods between timer
	 * interrupts.
	 */
	hpt_start = mips_hpt_read();
	do {
		while (mips_timer_state());
		while (!mips_timer_state());
	} while (--i);
	hpt_end = mips_hpt_read();

	hpt_count = hpt_end - hpt_start;
	hz = HZ;
	frequency = (u64)hpt_count * (u64)hz;

	return frequency >> log_2_loops;
}

void __init time_init(void)
{
	if (board_time_init)
		board_time_init();

	if (!rtc_set_mmss)
		rtc_set_mmss = rtc_set_time;

	xtime.tv_sec = rtc_get_time();
	xtime.tv_usec = 0;

        wall_to_monotonic.tv_sec = -xtime.tv_sec;
        wall_to_monotonic.tv_nsec = 0;


	/* Choose appropriate high precision timer routines.  */
	if (!cpu_has_counter && !mips_hpt_read) {
		/* No high precision timer -- sorry.  */
		mips_hpt_read = null_hpt_read;
		mips_hpt_init = null_hpt_init;
	} else if (!mips_hpt_frequency && !mips_timer_state) {
		/* A high precision timer of unknown frequency.  */
		if (!mips_hpt_read) {
			/* No external high precision timer -- use R4k.  */
			mips_hpt_read = c0_hpt_read;
			mips_hpt_init = c0_hpt_init;
		}

		if ((current_cpu_data.isa_level == MIPS_CPU_ISA_M32) ||
			 (current_cpu_data.isa_level == MIPS_CPU_ISA_I) ||
			 (current_cpu_data.isa_level == MIPS_CPU_ISA_II))
			/*
			 * We need to calibrate the counter but we don't have
			 * 64-bit division.
			 */
			do_gettimeoffset = calibrate_div32_gettimeoffset;
		else
			/*
			 * We need to calibrate the counter but we *do* have
			 * 64-bit division.
			 */
			do_gettimeoffset = calibrate_div64_gettimeoffset;
	} else {
		/* We know counter frequency.  Or we can get it.  */
		if (!mips_hpt_read) {
			/* No external high precision timer -- use R4k.  */
			mips_hpt_read = c0_hpt_read;

			if (mips_timer_state)
				mips_hpt_init = c0_hpt_init;
			else {
				/* No external timer interrupt -- use R4k.  */
				mips_hpt_init = c0_hpt_timer_init;
				mips_timer_ack = c0_timer_ack;
			}
		}
		if (!mips_hpt_frequency)
			mips_hpt_frequency = calibrate_hpt();

		do_gettimeoffset = fixed_rate_gettimeoffset;

#if defined(CONFIG_MIPS_COMMON_HRT)
        do_gettimeoffset = hrt_gettimeoffset;
#endif

		/* Calculate cache parameters.  */
		cycles_per_jiffy = (mips_hpt_frequency + HZ / 2) / HZ;

		/* sll32_usecs_per_cycle = 10^6 * 2^32 / mips_counter_freq  */
		do_div64_32(sll32_usecs_per_cycle,
			    1000000, mips_hpt_frequency / 2,
			    mips_hpt_frequency);

		/* Report the high precision timer rate for a reference.  */
		printk("Using %u.%03u MHz high precision timer.\n",
		       ((mips_hpt_frequency + 500) / 1000) / 1000,
		       ((mips_hpt_frequency + 500) / 1000) % 1000);
	}

	if (!mips_timer_ack)
		/* No timer interrupt ack (e.g. i8254).  */
		mips_timer_ack = null_timer_ack;

	/* This sets up the high precision timer for the first interrupt.  */
	mips_hpt_init(mips_hpt_read());

	/*
	 * Call board specific timer interrupt setup.
	 *
	 * this pointer must be setup in machine setup routine.
	 *
	 * Even if a machine chooses to use a low-level timer interrupt,
	 * it still needs to setup the timer_irqaction.
	 * In that case, it might be better to set timer_irqaction.handler
	 * to be NULL function so that we are sure the high-level code
	 * is not invoked accidentally.
	 */
	board_timer_setup(&timer_irqaction);

#if defined(CONFIG_MIPS_COMMON_HRT)
        if ( (read_c0_status() & CAUSEF_IP7) == 0) {
                /* we enable high res timers, but we are using CPU counter! */
                panic("Enabling high res timers without using CPU counter!");
        }

        hr_time_init();
#endif /* defined(CONFIG_MIPS_COMMON_HRT) */

}


#define FEBRUARY		2
#define STARTOFTIME		1970
#define SECDAY			86400L
#define SECYR			(SECDAY * 365)
#define leapyear(y)		((!((y) % 4) && ((y) % 100)) || !((y) % 400))
#define days_in_year(y)		(leapyear(y) ? 366 : 365)
#define days_in_month(m)	(month_days[(m) - 1])

static int month_days[12] = {
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

void to_tm(unsigned long tim, struct rtc_time *tm)
{
	long hms, day, gday;
	int i;

	gday = day = tim / SECDAY;
	hms = tim % SECDAY;

	/* Hours, minutes, seconds are easy */
	tm->tm_hour = hms / 3600;
	tm->tm_min = (hms % 3600) / 60;
	tm->tm_sec = (hms % 3600) % 60;

	/* Number of years in days */
	for (i = STARTOFTIME; day >= days_in_year(i); i++)
		day -= days_in_year(i);
	tm->tm_year = i;

	/* Number of months in days left */
	if (leapyear(tm->tm_year))
		days_in_month(FEBRUARY) = 29;
	for (i = 1; day >= days_in_month(i); i++)
		day -= days_in_month(i);
	days_in_month(FEBRUARY) = 28;
	tm->tm_mon = i - 1;		/* tm_mon starts from 0 to 11 */

	/* Days are what is left over (+1) from all that. */
	tm->tm_mday = day + 1;

	/*
	 * Determine the day of week
	 */
	tm->tm_wday = (gday + 4) % 7;	/* 1970/1/1 was Thursday */
}

/*
 * calibrate the mips_hpt_frequency
 */
#define         CALIBRATION_CYCLES              20
void calibrate_mips_counter(void)
{
        volatile unsigned long ticks;
        volatile unsigned long countStart, countEnd;

        if (mips_hpt_frequency) {
                /* it is already specified by the board */
                printk("MIPS CPU counter frequency is fixed at %d Hz\n",
                        mips_hpt_frequency);
                return;
        }

        if (!cpu_has_fpu) {
                /* we don't have cpu counter */
                return;
        }

        printk("calibrating MIPS CPU counter frequency ...");

        /* wait for the change of jiffies */
        ticks = jiffies;
        while (ticks == jiffies);

        /* read cpu counter */
        countStart = read_c0_count();

        /* loop for another n jiffies */
        ticks += CALIBRATION_CYCLES + 1;
        while (ticks != jiffies);

        /* read counter again */
        countEnd = read_c0_count();

        /* assuming HZ is 10's multiple */
        db_assert((HZ % CALIBRATION_CYCLES) == 0);

        mips_hpt_frequency =
                (countEnd - countStart) * (HZ / CALIBRATION_CYCLES);
        printk(" %d Hz\n", mips_hpt_frequency);
}



EXPORT_SYMBOL(rtc_lock);
EXPORT_SYMBOL(to_tm);
EXPORT_SYMBOL(rtc_set_time);
EXPORT_SYMBOL(rtc_get_time);
#if defined(CONFIG_MIPS_COMMON_HRT)
EXPORT_SYMBOL(nsec_to_arch_cycle);
EXPORT_SYMBOL(arch_cycle_to_nsec);
#endif

