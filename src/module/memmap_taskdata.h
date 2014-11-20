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
#ifndef __MEMMAP_TASK_DATA__
#define __MEMMAP_TASK_DATA__

#define MEMMAP_ACCESS_NONE 0b0
#define MEMMAP_ACCESS_R 0b1
#define MEMMAP_ACCESS_W 0b10
#define MEMMAP_ACCESS_RW (MEMMAP_ACCESS_R & MEMMAP_ACCESS_W)

typedef struct _task_data *task_data;

extern task_data *MemMap_tasksData;

// Current number of monitored tasks
extern int MemMap_GetNumTasks(void);

task_data MemMap_InitData(struct task_struct *t);
void MemMap_ClearData(task_data data);

struct task_struct *MemMap_GetTaskFromData(task_data data);

//Macros to select chunk in the following functions
void MemMap_AddToChunk(task_data data, void *addr,int cpu, int chunkid);
int MemMap_IsInChunk(task_data data, void *addr, int chunkid);
// Return the chunkids of the current or previous chunks
int MemMap_CurrentChunk(task_data data);
int MemMap_PreviousChunk(task_data data);

int MemMap_UpdateAdressData(task_data data,void *Addr, int type,int count);

// Start working on the next chunks
// If required, flush data
// returns 1 if data were flushed, 0 else
int MemMap_NextChunks(task_data data, unsigned long long *clocks);



// This is an easy accessor to walk on each entry of a table:
// usage:
//  int pos=0
//  void *addr
//  while((addr=MemMap_NextAddrInChunk(myData, &pos, mychunkid))!=NULL)
//      do_stuff(addr);
//
void *MemMap_NextAddrInChunk(task_data data,int *pos, int chunkid);

// None of the function above are atomic, however the following calls allows
// you to ensure mutual exclusion when it is required
void MemMap_LockData(task_data data);
void MemMap_unLockData(task_data data);
#endif //__MEMMAP_TASK_DATA__
