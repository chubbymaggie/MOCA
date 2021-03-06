/*
 * Copyright (C) 2015  Beniamine, David <David@Beniamine.net>
 * Author: Beniamine, David <David@Beniamine.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define __NO_VERSION__
/* #define MOCA_DEBUG */


 //msleep
#include "moca.h"
#include "moca_tasks.h"
#include "moca_taskdata.h"
#include "moca_probes.h"
#include "moca_false_pf.h"


void Moca_MmFaultHandler(struct mm_struct *mm, struct vm_area_struct *vma,
        unsigned long address, unsigned int flags)
{

    task_data data;
    moca_task tsk;
    if(!(data=Moca_GetData(current)))
    {
        if(!Moca_IsActivated() || !(tsk=Moca_AddTaskIfNeeded(current)))
            jprobe_return();
        data=tsk->data;
    }
    Moca_RLockPf();
    if(Moca_IsActivated())
        Moca_AddToChunk(data,(void *)(address),get_cpu(),flags&FAULT_FLAG_WRITE?1:0);
    MOCA_DEBUG_PRINT("Moca Pte fault task %p\n", current);
    if(Moca_FixFalsePf(mm,address,get_cpu())!=0)
        MOCA_DEBUG_PRINT("Moca true page fault at %p %p \n", addr, mm);
    Moca_UpdateClock();
    Moca_RUnlockPf();
    jprobe_return();

}

void Moca_ExitHandler(struct mmu_gather *tlb, struct vm_area_struct *start_vma,
         unsigned long start, unsigned long end)
{
    if(Moca_GetData(current))
    {
        MOCA_DEBUG_PRINT("Exit handler handler task %p, mm %p\n", NULL, NULL);
        Moca_RLockPf();
        Moca_FixAllFalsePf(start_vma->vm_mm,get_cpu());
        Moca_RUnlockPf();
    }
    jprobe_return();
}

static struct jprobe Moca_Faultjprobe = {
    .entry = Moca_MmFaultHandler,
    .kp.symbol_name = "handle_mm_fault",
};


static struct jprobe Moca_ExitProbe = {
    .entry=Moca_ExitHandler,
    .kp.symbol_name = "unmap_vmas",
};


int Moca_RegisterProbes(void)
{
    int ret;
    MOCA_DEBUG_PRINT("Moca registering probes\n");

    if ((ret=register_jprobe(&Moca_ExitProbe)))
        return ret;

    if ((ret=register_jprobe(&Moca_Faultjprobe)))
        unregister_jprobe(&Moca_ExitProbe);
    else
        MOCA_DEBUG_PRINT("Moca registered probes:%d\n", ret);
    return ret;
}


void Moca_UnregisterProbes(void)
{
    unregister_jprobe(&Moca_ExitProbe);
    unregister_jprobe(&Moca_Faultjprobe);
}
