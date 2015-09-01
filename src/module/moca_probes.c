/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * Moca is a kernel module designed to track memory access
 *
 * Copyright (C) 2010 David Beniamine
 * Author: David Beniamine <David.Beniamine@imag.fr>
 */
#define __NO_VERSION__
/* #define MOCA_DEBUG */

#include <linux/kprobes.h>
#include <linux/delay.h> //msleep
#include "moca.h"
#include "moca_tasks.h"
#include "moca_taskdata.h"
#include "moca_probes.h"
#include "moca_page.h"
#include "moca_false_pf.h"


void Moca_MmFaultHandler(struct mm_struct *mm, struct vm_area_struct *vma,
        unsigned long address, unsigned int flags)
{
    task_data data;
    moca_task tsk;
	//if (IS_KERNEL_ADDR(address)) //do not handle kernel faults
    //    jprobe_return();
    if(!(data=Moca_GetData(current)))
    {
        if(!Moca_IsActivated() || !(tsk=Moca_AddTaskIfNeeded(current)))
            jprobe_return();
        data=tsk->data;
    }
    if(Moca_IsActivated())
        Moca_AddToChunk(data,(void *)address,get_cpu());
    MOCA_DEBUG_PRINT("Moca Pte fault task %p\n", current);
    Moca_UpdateClock();
    Moca_RLockPf();
    if(Moca_FixFalsePf(mm,address)!=0)
        MOCA_DEBUG_PRINT("Moca true fault at a: %p a&m: %p mm: %p\n",
                (void *)address, (void *)(address&PAGE_MASK),mm);
    jprobe_return();
}

void Moca_ExitHandler(struct mmu_gather *tlb, struct vm_area_struct *start_vma,
		unsigned long start, unsigned long end)
{
    struct mm_struct *mm=start_vma->vm_mm;
    if(current && Moca_GetData(current)!=NULL)
    {
        MOCA_DEBUG_PRINT("Moca Exit handler handler task %p, mm %p\n", current, mm);
        Moca_RLockPf();
        Moca_FixAllFalsePf(mm);
    }
    jprobe_return();
}

static int Moca_HandlerPost(struct kretprobe_instance *ri,
        struct pt_regs *regs)
{
    if(current && Moca_GetData(current)!=NULL)
        Moca_RUnlockPf();
    return 0;
}

static struct jprobe Moca_PteFaultjprobe = {
    .entry = Moca_MmFaultHandler,
    .kp.symbol_name = "handle_mm_fault",
};

static struct kretprobe Moca_PostFaultProbe = {
    .handler=Moca_HandlerPost,
    .maxactive=2*NR_CPUS,
};

static struct jprobe Moca_ExitProbe = {
    .entry=Moca_ExitHandler,
    .kp.symbol_name="unmap_vmas",
};

static struct kretprobe Moca_PostExitProbe = {
    .handler=Moca_HandlerPost,
    .maxactive=2*NR_CPUS,
};

int Moca_RegisterProbes(void)
{
    int ret;
    MOCA_DEBUG_PRINT("Moca registering probes\n");

    Moca_PostExitProbe.kp.symbol_name="unmap_vmas";
    if ((ret=register_kretprobe(&Moca_PostExitProbe)))
        Moca_Panic("Unable to register post exit probe");


    if ((ret=register_jprobe(&Moca_ExitProbe)))
        Moca_Panic("Unable to register do exit probe");

    Moca_PostFaultProbe.kp.symbol_name = "handle_mm_fault";
    if ((ret=register_kretprobe(&Moca_PostFaultProbe)))
        Moca_Panic("Unable to register post fault probe");

    if ((ret=register_jprobe(&Moca_PteFaultjprobe)))
        Moca_Panic("Moca Unable to register pte fault probe");

    MOCA_DEBUG_PRINT("Moca registered probes\n");
    return ret;
}


void Moca_UnregisterProbes(void)
{
    unregister_jprobe(&Moca_PteFaultjprobe);
    unregister_jprobe(&Moca_ExitProbe);
    // might need a delay here
    msleep(2*MOCA_DEFAULT_WAKEUP_INTERVAL);
    unregister_kretprobe(&Moca_PostFaultProbe);
    unregister_kretprobe(&Moca_PostExitProbe);
}
