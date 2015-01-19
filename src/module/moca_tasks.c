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
//#define MOCA_DEBUG

#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/hash.h>
#include <linux/slab.h>
#include "moca.h"
#include "moca_tasks.h"
#include <linux/spinlock.h>


// The first bits are not random enough, 14 bits should be enough for pids
unsigned long Moca_tasksHashBits=14;
int Moca_AddTaskWaiting=0;
spinlock_t Moca_tasksLock;

// Monitored process
hash_map Moca_tasksMap;
struct task_struct *Moca_initTask=NULL;

moca_task Moca_AddTask(struct task_struct *t);

int Moca_InitProcessManagment(int id)
{
    // Monitored pids
    struct pid *pid;
    spin_lock_init(&Moca_tasksLock);
    Moca_tasksMap=Moca_InitHashMap(Moca_tasksHashBits,
            2*(1<<Moca_tasksHashBits), sizeof(struct _moca_task));
    rcu_read_lock();
    pid=find_vpid(id);
    if(!pid)
    {
        // Skip internal process
        rcu_read_unlock();
        Moca_Panic("Moca unable to find pid for init task");
        return 1;
    }
    Moca_initTask=pid_task(pid, PIDTYPE_PID);
    rcu_read_unlock();
    Moca_InitTaskData();
    return 0;

}

void Moca_CleanProcessData(void)
{
    if(Moca_tasksMap)
    {
        Moca_ClearAllData();
        Moca_FreeMap(Moca_tasksMap);
    }
}

// Add pid to the monitored process if pid is a monitored process
moca_task Moca_AddTaskIfNeeded(struct task_struct *t)
{
    moca_task tsk=NULL;
    spin_lock(&Moca_tasksLock);
    if(t->real_parent == Moca_initTask  ||
            Moca_EntryFromKey(Moca_tasksMap, t->real_parent)!=NULL )
        tsk=Moca_AddTask(t);
    spin_unlock(&Moca_tasksLock);
    return tsk;
}

// Current number of monitored pids
int Moca_GetNumTasks(void)
{
    int nb=0;
    spin_lock(&Moca_tasksLock);
    nb=Moca_NbElementInMap(Moca_tasksMap);
    spin_unlock(&Moca_tasksLock);
    return nb;
}

moca_task Moca_NextTask(int *pos)
{
    moca_task ret=NULL;
    MOCA_DEBUG_PRINT("Moca Looking for task at %d\n", *pos);
    spin_lock(&Moca_tasksLock);
    ret=(moca_task)Moca_NextEntryPos(Moca_tasksMap,pos);
    MOCA_DEBUG_PRINT("Moca found task %p at %d\n", ret, *pos);
    spin_unlock(&Moca_tasksLock);
    return ret;
}

task_data Moca_GetData(struct task_struct *t)
{
    int pos;
    task_data ret=NULL;
    spin_lock(&Moca_tasksLock);
    if((pos=Moca_PosInMap(Moca_tasksMap ,t))!=-1)
        ret=((moca_task)Moca_EntryAtPos(Moca_tasksMap,pos))->data;
    spin_unlock(&Moca_tasksLock);
    return ret;
}


// Add t to the monitored pids
moca_task Moca_AddTask(struct task_struct *t)
{
    task_data data;
    moca_task tsk;
    int status;
    MOCA_DEBUG_PRINT("Moca Adding task %p\n",t );


    //Create the task data
    data=Moca_InitData(t);
    get_task_struct(t);
    if(!data)
        return NULL;

    tsk=(moca_task)Moca_AddToMap(Moca_tasksMap,t,&status);
    switch(status)
    {
        case MOCA_HASHMAP_FULL:
            Moca_Panic("Moca Too many pids");
            break;
        case MOCA_HASHMAP_ERROR:
            Moca_Panic("Moca unhandeled hashmap error");
            break;
        case  MOCA_HASHMAP_ALREADY_IN_MAP:
            Moca_Panic("Moca Adding an already exixsting task");
            break;
        default:
            //normal add
            tsk->data=data;
            MOCA_DEBUG_PRINT("Moca Added task %p at pos %d \n", t, status);
            break;
    }
    return tsk;
}

void Moca_RemoveTask(struct task_struct *t)
{
    spin_lock(&Moca_tasksLock);
    Moca_RemoveFromMap(Moca_tasksMap, t);
    spin_unlock(&Moca_tasksLock);
    put_task_struct(t);
}