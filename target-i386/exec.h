/*
 *  i386 execution defines
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA  02110-1301 USA
 */
#include "config.h"
#include "dyngen-exec.h"

/* XXX: factorize this mess */
#ifdef TARGET_X86_64
#define TARGET_LONG_BITS 64
#else
#define TARGET_LONG_BITS 32
#endif

#include "exec/cpu-defs.h"

GLOBAL_REGISTER_VARIABLE_DECL struct CPUX86State *env asm(AREG0);

#include "qemu-common.h"
#include "qemu/log.h"

#include "cpu.h"
#include "exec/exec-all.h"

/* op_helper.c */
void QEMU_NORETURN raise_exception_err(int exception_index, int error_code);
void QEMU_NORETURN raise_exception(int exception_index);

/* n must be a constant to be efficient */
static inline target_long lshift(target_long x, int n)
{
    if (n >= 0)
        return x << n;
    else
        return x >> (-n);
}

#include "helper.h"

#if !defined(CONFIG_USER_ONLY)

#include "exec/softmmu_exec.h"

#endif /* !defined(CONFIG_USER_ONLY) */

#define RC_MASK         0xc00
#define RC_NEAR		0x000
#define RC_DOWN		0x400
#define RC_UP		0x800
#define RC_CHOP		0xc00

#define MAXTAN 9223372036854775808.0

/* the following deal with x86 long double-precision numbers */
#define MAXEXPD 0x7fff
#define EXPBIAS 16383
#define EXPD(fp)	(fp.l.upper & 0x7fff)
#define SIGND(fp)	((fp.l.upper) & 0x8000)
#define MANTD(fp)       (fp.l.lower)
#define BIASEXPONENT(fp) fp.l.upper = (fp.l.upper & ~(0x7fff)) | EXPBIAS

static inline void fpush(void)
{
    env->fpstt = (env->fpstt - 1) & 7;
    env->fptags[env->fpstt] = 0; /* validate stack entry */
}

static inline void fpop(void)
{
    env->fptags[env->fpstt] = 1; /* invvalidate stack entry */
    env->fpstt = (env->fpstt + 1) & 7;
}

static inline floatx80 helper_fldt(target_ulong ptr)
{
    floatx80 temp;

    temp.low = cpu_ldq_data(env, ptr);
    temp.high = cpu_lduw_data(env, ptr + 8);
    return temp;
}

static inline void helper_fstt(floatx80 f, target_ulong ptr)
{
    cpu_stq_data(env, ptr, f.low);
    cpu_stw_data(env, ptr + 8, f.high);
}

#define FPUS_IE (1 << 0)
#define FPUS_DE (1 << 1)
#define FPUS_ZE (1 << 2)
#define FPUS_OE (1 << 3)
#define FPUS_UE (1 << 4)
#define FPUS_PE (1 << 5)
#define FPUS_SF (1 << 6)
#define FPUS_SE (1 << 7)
#define FPUS_B  (1 << 15)

#define FPUC_EM 0x3f

static inline uint32_t compute_eflags(void)
{
    return env->eflags | helper_cc_compute_all(CC_OP) | (DF & DF_MASK);
}

/* NOTE: CC_OP must be modified manually to CC_OP_EFLAGS */
static inline void load_eflags(int eflags, int update_mask)
{
    CC_SRC = eflags & (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C);
    DF = 1 - (2 * ((eflags >> 10) & 1));
    env->eflags = (env->eflags & ~update_mask) |
        (eflags & update_mask) | 0x2;
}

static inline bool cpu_has_work(CPUX86State *env)
{
    int work;

    work = (env->interrupt_request & CPU_INTERRUPT_HARD) &&
           (env->eflags & IF_MASK);
    work |= env->interrupt_request & CPU_INTERRUPT_NMI;
    work |= env->interrupt_request & CPU_INTERRUPT_INIT;
    work |= env->interrupt_request & CPU_INTERRUPT_SIPI;

    return work;
}

static inline int cpu_halted(CPUX86State *env) {
    /* handle exit of HALTED state */
    if (!env->halted)
        return 0;
    /* disable halt condition */
    if (cpu_has_work(env)) {
        env->halted = 0;
        return 0;
    }
    return EXCP_HALTED;
}

/* load efer and update the corresponding hflags. XXX: do consistency
   checks with cpuid bits ? */
static inline void cpu_load_efer(CPUX86State *env, uint64_t val)
{
    env->efer = val;
    env->hflags &= ~(HF_LMA_MASK | HF_SVME_MASK);
    if (env->efer & MSR_EFER_LMA)
        env->hflags |= HF_LMA_MASK;
    if (env->efer & MSR_EFER_SVME)
        env->hflags |= HF_SVME_MASK;
}