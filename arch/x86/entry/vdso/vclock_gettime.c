// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fast user context implementation of clock_gettime, gettimeofday, and time.
 *
 * Copyright 2006 Andi Kleen, SUSE Labs.
 * Copyright 2019 ARM Limited
 *
 * 32 Bit compat layer by Stefani Seibold <stefani@seibold.net>
 *  sponsored by Rohde & Schwarz GmbH & Co. KG Munich/Germany
 */
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include "../../../../lib/vdso/gettimeofday.c"

extern int __vdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz);
extern __kernel_old_time_t __vdso_time(__kernel_old_time_t *t);

int __vdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz)
{
	return __cvdso_gettimeofday(tv, tz);
}

extern long __sbpf_get_PSS_features(void);
extern long __sbpf_set_PSS_features(u64 PSS_features);

long __sbpf_get_PSS_features(void)
{
	struct sbpf_data* sbd = __arch_get_sbpf_data();
	long PSS_features = (long)sbd->PSS_features;

	if(PSS_features > 0xf || PSS_features < 0) return -1;
		
	return PSS_features;
}

long sbpf_get_PSS_features(void)
	__attribute__((weak, alias("__sbpf_get_PSS_features")));


long __sbpf_set_PSS_features(u64 PSS_features)
{
	if(PSS_features > 0xf || PSS_features < 0) return -1;

	struct sbpf_data* sbd = __arch_get_sbpf_data();
	sbd->PSS_features = (u64)PSS_features;
	
	return PSS_features;
}

long sbpf_set_PSS_features(u64 PSS_features)
	__attribute__((weak, alias("__sbpf_set_PSS_features")));


extern unsigned long __testvdso(void);
int gettimeofday(struct __kernel_old_timeval *, struct timezone *)
	__attribute__((weak, alias("__vdso_gettimeofday")));

__kernel_old_time_t __vdso_time(__kernel_old_time_t *t)
{
	return __cvdso_time(t);
}

__kernel_old_time_t time(__kernel_old_time_t *t)	__attribute__((weak, alias("__vdso_time")));


// Perceptron paramaters
#define PERC_ENTRIES 64   //Upto 12-bit addressing in hashed perceptron
#define PERC_FEATURES 4
#define PERC_COUNTER_MAX 15 //-16 to +15: 5 bits counter
#define PERC_THRESHOLD_HI -5
#define PERC_THRESHOLD_LO -15
#define POS_UPDT_THRESHOLD 90
#define NEG_UPDT_THRESHOLD -80

void get_perc_index(int *input, int len, int perc_set[PERC_FEATURES])
{
    unsigned pre_hash[PERC_FEATURES];
	int i;
    for (i = 0; i < len; i++)
    {
        pre_hash[i] = input[i];
    }

    for (i = 0; i < len; i++)
    {
        // perc_set[i] = (pre_hash[i]) % perc.PERC_DEPTH[i]; // Variable depths
        perc_set[i] = (pre_hash[i]) % 32; // Variable depths
    }
}
// extern struct vdso_data _vdso_data[CS_BASES] __attribute__((visibility("hidden")));
notrace int __vdso_query(int* input, int len)
{
	// unsigned *ptr = (unsigned *)(&_vdso_data) + 0xBC;
	// return *ptr;

    int perc_set[PERC_FEATURES];
    // Get the indexes in perc_set[]
    get_perc_index(input, len, perc_set);

    int sum = 0;
	int i;
    for (i = 0; i < len; i++)
    {
        sum += __arch_get_vdso_data()->perc[perc_set[i]][i];
        // Calculate Sum
    }
    // Return the sum
    return sum;

	// return __arch_get_vdso_data()->weight[x];
}

#if defined(CONFIG_X86_64) && !defined(BUILD_VDSO32_64)
/* both 64-bit and x32 use these */
extern int __vdso_clock_gettime(clockid_t clock, struct __kernel_timespec *ts);
extern int __vdso_clock_getres(clockid_t clock, struct __kernel_timespec *res);

int __vdso_clock_gettime(clockid_t clock, struct __kernel_timespec *ts)
{
	return __cvdso_clock_gettime(clock, ts);
}

int clock_gettime(clockid_t, struct __kernel_timespec *)
	__attribute__((weak, alias("__vdso_clock_gettime")));

int __vdso_clock_getres(clockid_t clock,
			struct __kernel_timespec *res)
{
	return __cvdso_clock_getres(clock, res);
}
int clock_getres(clockid_t, struct __kernel_timespec *)
	__attribute__((weak, alias("__vdso_clock_getres")));

#else
/* i386 only */
extern int __vdso_clock_gettime(clockid_t clock, struct old_timespec32 *ts);
extern int __vdso_clock_getres(clockid_t clock, struct old_timespec32 *res);

int __vdso_clock_gettime(clockid_t clock, struct old_timespec32 *ts)
{
	return __cvdso_clock_gettime32(clock, ts);
}

int clock_gettime(clockid_t, struct old_timespec32 *)
	__attribute__((weak, alias("__vdso_clock_gettime")));

int __vdso_clock_gettime64(clockid_t clock, struct __kernel_timespec *ts)
{
	return __cvdso_clock_gettime(clock, ts);
}

int clock_gettime64(clockid_t, struct __kernel_timespec *)
	__attribute__((weak, alias("__vdso_clock_gettime64")));

int __vdso_clock_getres(clockid_t clock, struct old_timespec32 *res)
{
	return __cvdso_clock_getres_time32(clock, res);
}

int clock_getres(clockid_t, struct old_timespec32 *)
	__attribute__((weak, alias("__vdso_clock_getres")));
#endif
