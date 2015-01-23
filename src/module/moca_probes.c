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
#define MOCA_DEBUG

#include <linux/kprobes.h>
#include "moca.h"
#include "moca_tasks.h"
#include "moca_taskdata.h"
#include "moca_probes.h"
#include "moca_page.h"
#include "moca_false_pf.h"

void Moca_MmFaultHandler(struct mm_struct *mm, struct vm_area_struct *vma,
        unsigned long address, unsigned int flags)
{
    pte_t *pte;

    task_data data;
    moca_task tsk;
    if(!(data=Moca_GetData(current)))
    {
        if(!(tsk=Moca_AddTaskIfNeeded(current)))
            jprobe_return();
        data=tsk->data;
    }
    pte=Moca_PteFromAdress(address,mm);
    // Track only user pages
    if(!MOCA_USEFULL_PTE(pte))
        jprobe_return();
    MOCA_DEBUG_PRINT("Moca Pte fault task %p\n", current);
    MOCA_PRINT_FLAGS(pte);
    Moca_AddToChunk(data,(void *)(address&PAGE_MASK),get_cpu());
    //If pte exists, try to fix false pagefault
    if (pte && MOCA_FALSE_PF(*pte))
    {
        MOCA_CLEAR_FALSE_PF(*pte);
        MOCA_DEBUG_PRINT("Moca fixing fake pagefault pte %p flags %x page %lx\n",
                pte, (unsigned int)pte_flags(*pte), (*(unsigned long*)pte)&PTE_PFN_MASK);
    }
    Moca_UpdateClock();
    jprobe_return();

}

static struct jprobe Moca_PteFaultjprobe = {
    .entry = Moca_MmFaultHandler,
    .kp.symbol_name = "handle_mm_fault",
};

int Moca_RegisterProbes(void)
{
    int ret;
    if ((ret=register_jprobe(&Moca_PteFaultjprobe)))
        Moca_Panic("Moca Unable to register pte fault probe");
    return ret;
}


void Moca_UnregisterProbes(void)
{
    unregister_jprobe(&Moca_PteFaultjprobe);
}
