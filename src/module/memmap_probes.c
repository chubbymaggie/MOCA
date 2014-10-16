/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * MemMap is a kernel module designed to track memory access
 *
 * Copyright (C) 2010 David Beniamine
 * Author: David Beniamine <David.Beniamine@imag.fr>
 */

#include <linux/kprobes.h>
#include "memmap.h"
#include "memmap_pid.h"

int MemMap_ForkHandler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    return MemMap_AddPidIfNeeded(regs_return_value(regs));
}

static struct kretprobe MemMap_ForkProbe = {
    .handler = MemMap_ForkHandler,
    .kp.symbol_name = "do_fork",
};


void MemMap_RegisterProbes(void)
{
    int ret;
    if ((ret=register_kretprobe(&MemMap_ForkProbe))){
        MemMap_Panic("Unable to register fork prove");
    }
}


void MemMap_UnregisterProbes(void)
{
    unregister_kretprobe(&MemMap_ForkProbe);
}
