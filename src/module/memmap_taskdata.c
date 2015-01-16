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
#define __NO_VERSION__
//#define MEMMAP_DEBUG

#define MEMMAP_BUF_SIZE 4096
#define MEMMAP_DATA_STATUS_NORMAL 0
//Data won't be written anymore, but need to be saved to file
#define MEMMAP_DATA_STATUS_NEEDFLUSH 1
//Data have been outputed  but FlushData must be called again for EOF
#define MEMMAP_DATA_STATUS_DYING -1
//We can free data (after removing the /proc entry)
#define MEMMAP_DATA_STATUS_ZOMBIE -2
#define MEMMAP_DATA_STATUS_DYING_OR_ZOMBIE(data) ((data)->status < 0)
#define MEMMAP_HASH_BITS 14

int MemMap_taskDataHashBits=MEMMAP_HASH_BITS;
int MemMap_taskDataChunkSize=2*(1<<MEMMAP_HASH_BITS);
int MemMap_nbChunks=20;

#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/cpumask.h> //num_online_cpus
#include <asm/uaccess.h>  /* for copy_*_user */
#include <linux/delay.h>
#include "memmap.h"
#include "memmap_taskdata.h"
#include "memmap_tasks.h"
#include "memmap_hashmap.h"



static struct proc_dir_entry *MemMap_proc_root;

static ssize_t MemMap_FlushData(struct file *filp,  char *buffer,
        size_t length, loff_t * offset);

static const struct file_operations MemMap_taskdata_fops = {
    .owner   = THIS_MODULE,
    .read    = MemMap_FlushData,
};

typedef struct _chunk_entry
{
    void *key; //Address
    int next;
    int countR;
    int countW;
    int cpu;
}*chunk_entry;

typedef struct
{
    hash_map map;
    unsigned long startClock;
    unsigned long endClock;
    int cpu;
    int used;
    spinlock_t lock;
}chunk;

typedef struct _task_data
{
    struct task_struct *task;
    chunk **chunks;
    int cur;
    int currentlyFlushed;
    int internalId;
    int nbflush;
    int status;
    spinlock_t lock;
    struct proc_dir_entry *proc_entry;
}*task_data;

int MemMap_nextTaskId=0;

int MemMap_CurrentChunk(task_data data)
{
    int ret=-1;
    spin_lock(&data->lock);
    ret=data->cur;
    spin_unlock(&data->lock);
    return ret;
}

void MemMap_InitTaskData(void)
{
    //Procfs init
    MemMap_proc_root=proc_mkdir("MemMap", NULL);
    if(!MemMap_proc_root)
        MemMap_Panic("MemMap Unable to create proc root entry");
}

task_data MemMap_InitData(struct task_struct *t)
{
    int i;
    task_data data;
    char buf[10];
    //We must not wait here !
    data=kmalloc(sizeof(struct _task_data),GFP_ATOMIC);
    MEMMAP_DEBUG_PRINT("MemMap Initialising data for task %p\n",t);
    if(!data)
    {
        MemMap_Panic("MemMap unable to allocate data ");
        return NULL;
    }
    data->chunks=kmalloc(sizeof(chunk)*MemMap_nbChunks,GFP_ATOMIC);
    if(!data->chunks)
    {
        kfree(data);
        MemMap_Panic("MemMap unable to allocate chunks");
        return NULL;
    }
    for(i=0;i<MemMap_nbChunks;i++)
    {
        MEMMAP_DEBUG_PRINT("MemMap Initialising data chunk %d for task %p\n",i, t);
        data->chunks[i]=kmalloc(sizeof(chunk),GFP_ATOMIC);
        if(!data->chunks[i])
        {
            MemMap_Panic("MemMap unable to allocate data chunk");
            return NULL;
        }
        data->chunks[i]->startClock=0;
        data->chunks[i]->endClock=0;
        data->chunks[i]->cpu=0;
        data->chunks[i]->used=0;
        data->chunks[i]->map=MemMap_InitHashMap(MemMap_taskDataHashBits,
                MemMap_taskDataChunkSize, sizeof(struct _chunk_entry));
        spin_lock_init(&data->chunks[i]->lock);
    }
    data->task=t;
    data->cur=0;
    data->chunks[0]->startClock=MemMap_GetClock();
    data->internalId=MemMap_nextTaskId++;
    data->nbflush=0;
    data->status=MEMMAP_DATA_STATUS_NORMAL;
    snprintf(buf,10,"task%d",data->internalId);
    data->proc_entry=proc_create_data(buf,0,MemMap_proc_root,
            &MemMap_taskdata_fops,data);
    data->currentlyFlushed=-1;
    spin_lock_init(&data->lock);
    return data;
}

void MemMap_ClearAllData(void)
{
    int i=0, nbTasks,chunkid;
    memmap_task t;
    char buf[10];
    MEMMAP_DEBUG_PRINT("MemMap Cleaning data\n");
    nbTasks=MemMap_GetNumTasks();
    while((t=MemMap_NextTask(&i)))
    {
        MEMMAP_DEBUG_PRINT("MemMap asking data %d %p %p to end\n",i, t->data, t->key);
        t->data->status=MEMMAP_DATA_STATUS_NEEDFLUSH;
    }
    i=0;
    while((t=MemMap_NextTask(&i)))
    {
        //Wait for the task to be dead
        MEMMAP_DEBUG_PRINT("MemMap waiting data %d to end\n",i);
        while(t->data->status!=MEMMAP_DATA_STATUS_ZOMBIE)
            msleep(100);
        MEMMAP_DEBUG_PRINT("MemMap data %d %p %p ended\n",i, t->data, t->key);
        snprintf(buf,10,"task%d",i-1);
        remove_proc_entry(buf, MemMap_proc_root);
        //Clean must be done after removing the proc entry
        for(chunkid=0; chunkid < MemMap_nbChunks;++chunkid)
        {
            MEMMAP_DEBUG_PRINT("Memap Freeing data %p chunk %d\n",
                    t->data, chunkid);
            MemMap_FreeMap(t->data->chunks[chunkid]->map);
            kfree(t->data->chunks[chunkid]);
        }
        kfree(t->data);
        MemMap_RemoveTask(t->key);
        MEMMAP_DEBUG_PRINT("Memap Freed data %d \n", i);
    }
    MEMMAP_DEBUG_PRINT("MemMap Removing proc root\n");
    remove_proc_entry("MemMap", NULL);

    MEMMAP_DEBUG_PRINT("MemMap all data cleaned\n");
}



struct task_struct *MemMap_GetTaskFromData(task_data data)
{
    return data->task;
}

void MemMap_LockChunk(task_data data)
{
    spin_lock(&data->chunks[MemMap_CurrentChunk(data)]->lock);
}

void MemMap_UnlockChunk(task_data data)
{
    spin_unlock(&data->chunks[MemMap_CurrentChunk(data)]->lock);
}

int MemMap_AddToChunk(task_data data, void *addr, int cpu)
{
    int status, cur;
    chunk_entry e;
    cur=MemMap_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return -1;
    }
    MEMMAP_DEBUG_PRINT("MemMap hashmap adding %p to chunk %d %p data %p cpu %d\n",
            addr, cur, data->chunks[cur],data, cpu);
    e=(chunk_entry)MemMap_AddToMap(data->chunks[cur]->map,addr, &status);
    switch(status)
    {
        case MEMMAP_HASHMAP_FULL :
            MemMap_Panic("MemMap hashmap full");
            spin_unlock(&data->chunks[cur]->lock);
            return -1;
            break;
        case MEMMAP_HASHMAP_ERROR :
            MemMap_Panic("MemMap hashmap error");
            spin_unlock(&data->chunks[cur]->lock);
            return -1;
            break;
        case MEMMAP_HASHMAP_ALREADY_IN_MAP :
            MEMMAP_DEBUG_PRINT("MemMap addr already in chunk %p\n", addr);
            ++e->countR;
            ++e->countW;
            break;
        default :
            //Normal add
            e->countR=0;
            e->countW=0;
            break;
    }
    e->cpu|=1<<cpu;
    data->chunks[cur]->cpu|=1<<cpu;
    spin_unlock(&data->chunks[cur]->lock);
    MEMMAP_DEBUG_PRINT("MemMap inserted %p\n", addr);
    return 0;
}

int MemMap_UpdateData(task_data data,int pos, int countR, int countW, int cpu)
{
    chunk_entry e;
    int cur=MemMap_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return -1;
    }
    e=(chunk_entry )MemMap_EntryAtPos(data->chunks[cur]->map,
            pos);
    if(!e)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return 1;
    }

    e->countR+=countR;
    e->countW+=countW;
    e->cpu|=1<<cpu;
    data->chunks[cur]->cpu|=1<<cpu;
    spin_unlock(&data->chunks[cur]->lock);
    return 0;
}

int MemMap_NextChunks(task_data data)
{
    int cur;
    MEMMAP_DEBUG_PRINT("MemMap Goto next chunks %p, %d\n", data, data->cur);
    //Global lock
    spin_lock(&data->lock);
    cur=data->cur;
    spin_lock(&data->chunks[cur]->lock);
    data->chunks[cur]->endClock=MemMap_GetClock();
    data->chunks[cur]->used=1;
    data->cur=(cur+1)%MemMap_nbChunks;
    spin_unlock(&data->chunks[cur]->lock);
    spin_unlock(&data->lock);
    MEMMAP_DEBUG_PRINT("MemMap Goto chunks  %p %d, %d\n", data, data->cur,
            MemMap_nbChunks);
    if(data->chunks[data->cur]->used)
    {
        printk(KERN_ALERT "MemMap no more chunks, stopping trace for task %d\n You can fix that by relaunching MemMap either with a higher number of chunks\n or by decreasing the logging daemon wakeupinterval\n",
                data->internalId);
        return 1;
    }
    else
        data->chunks[data->cur]->startClock=MemMap_GetClock();
    return 0;
}


void *MemMap_AddrInChunkPos(task_data data,int pos)
{
    chunk_entry e;
    int cur=MemMap_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return NULL;
    }
    MEMMAP_DEBUG_PRINT("MemMap Looking for next addr in ch %d, pos %d/%u\n",
            cur, pos, MemMap_NbElementInMap(data->chunks[cur]->map));
    e=(chunk_entry)MemMap_EntryAtPos(data->chunks[cur]->map,
            pos);
    spin_unlock(&data->chunks[cur]->lock);
    if(!e)
        return NULL;
    MEMMAP_DEBUG_PRINT("found adress %p\n", e->key);
    return e->key;
}


int MemMap_CpuMask(int cpu, char *buf, size_t size)
{
    int i=0,pos=num_online_cpus();
    if(!buf)
        return 0;
    while(pos>=0 && i< size)
    {
        buf[i]=(cpu&(1<<pos))?'1':'0';
        ++i;
        --pos;
    }
    if( i>=size)
    {
        MemMap_Panic("MemMap Buffer overflow in CpuMask");
        return 0;
    }
    return i;
}


#define LINE_SZ 80
static ssize_t MemMap_FlushData(struct file *filp,  char *buffer,
        size_t length, loff_t * offset)
{
    ssize_t len=0,sz;
    int chunkid, ind, nelt, complete=1;
    char *MYBUFF;
    chunk_entry e;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    task_data data=(task_data)PDE_DATA(file_inode(filp));
#else
    task_data data=(task_data)PDE(filp->f_path.dentry->d_inode)->data;
#endif
    MEMMAP_DEBUG_PRINT("MemMap_Flushing data %p allowed len %lu\n", data, length);


    MYBUFF=kmalloc(MEMMAP_BUF_SIZE,GFP_ATOMIC);
    if(!MYBUFF)
    {
        //We might already be in a panic: don't panic, just free stuff
        printk(KERN_WARNING "MemMap unable to allocate buffer in flush data\n");
        data->status=MEMMAP_DATA_STATUS_DYING;
    }
    if(MEMMAP_DATA_STATUS_DYING_OR_ZOMBIE(data))
    {
        //Data already flush, do noting and wait for kfreedom
        data->status=MEMMAP_DATA_STATUS_ZOMBIE;
        return 0;
    }

    if(data->nbflush==0)
    {
        //First flush
        MEMMAP_DEBUG_PRINT("MemMap first flush for data %p\n", data);

        sz=snprintf(MYBUFF,MEMMAP_BUF_SIZE,"Taskdata %d %d %p\n",
                data->internalId,task_pid_nr(data->task), data->task);
        MEMMAP_DEBUG_PRINT("buf size %lu\n", sz);
        if(!copy_to_user(buffer, MYBUFF,sz))
            len+=sz;
        MEMMAP_DEBUG_PRINT("user size %lu\n", len);
    }

    //Iterate on all chunks, start where we stopped if needed
    for(chunkid=(data->currentlyFlushed<0)?0:data->currentlyFlushed;
            chunkid < MemMap_nbChunks;++chunkid)
    {
        //If we are resuming a Flush, we are already helding the lock
        if(chunkid!=data->currentlyFlushed)
            spin_lock(&data->chunks[chunkid]->lock);
        if(data->status==MEMMAP_DATA_STATUS_NEEDFLUSH
                || data->chunks[chunkid]->used)
        {
            if((nelt=MemMap_NbElementInMap(data->chunks[chunkid]->map))>0)
            {
                //If we are resuming, no need to output this line again
                if(chunkid!=data->currentlyFlushed)
                {
                    if(chunkid==data->cur)
                        data->chunks[chunkid]->endClock=MemMap_GetClock();
                    //Chunk id  nb element startclock endclock cpumask
                    sz=snprintf(MYBUFF,MEMMAP_BUF_SIZE,"Chunk %d %d %lu %lu ",
                            chunkid+data->nbflush*MemMap_nbChunks,
                            MemMap_NbElementInMap(data->chunks[chunkid]->map),
                            data->chunks[chunkid]->startClock,
                            data->chunks[chunkid]->endClock);
                    sz+=MemMap_CpuMask(data->chunks[chunkid]->cpu, MYBUFF+sz,
                            MEMMAP_BUF_SIZE-sz);
                    MYBUFF[sz++]='\n';
                    if(!copy_to_user(buffer+len,MYBUFF,sz))
                        len+=sz;
                }
                else
                    MEMMAP_DEBUG_PRINT("MemMap resuming flush chunk %d, available space %lu\n",
                            chunkid, length-len);
                ind=0;
                while((e=(chunk_entry)MemMap_NextEntryPos(data->chunks[chunkid]->map,&ind)))
                {
                    if(len+LINE_SZ >= length)
                    {
                        complete=0;
                        break;
                    }
                    //Access address countread countwrite cpumask
                    sz=snprintf(MYBUFF,MEMMAP_BUF_SIZE,"Access %p %d %d ",
                            e->key, e->countR, e->countW);
                    sz+=MemMap_CpuMask(e->cpu,MYBUFF+sz,MEMMAP_BUF_SIZE-sz);
                    MYBUFF[sz++]='\n';
                    if(!copy_to_user(buffer+len,MYBUFF,sz))
                        len+=sz;
                    //Re init data
                    MemMap_RemoveFromMap(data->chunks[chunkid]->map,e->key);
                    e->countR=0;
                    e->countW=0;
                    e->cpu=0;
                }
            }
            if(complete)
            {
                data->chunks[chunkid]->cpu=0;
                data->chunks[chunkid]->used=0;
                //TODO: remove the following lines
                if((nelt=MemMap_NbElementInMap(data->chunks[chunkid]->map))!=0)
                    MEMMAP_DEBUG_PRINT("MemMap Still %d elt, ch %d atfer complete flush\n",
                            nelt, chunkid);
                data->chunks[chunkid]->startClock=MemMap_GetClock();
                data->chunks[chunkid]->endClock=MemMap_GetClock();
            }
        }
        if(complete)
            spin_unlock(&data->chunks[chunkid]->lock);
        else
        {
            MEMMAP_DEBUG_PRINT("MemMap stopping flush chunk %d, available space %lu\n",
                    chunkid, length-len);
            data->currentlyFlushed=chunkid;
            break;
        }
    }
    if(!data->chunks[0]->used)
        data->chunks[0]->startClock=MemMap_GetClock();
    if(complete)
    {
        ++data->nbflush;
        if(data->status==MEMMAP_DATA_STATUS_NEEDFLUSH )
            data->status=MEMMAP_DATA_STATUS_DYING;
        data->currentlyFlushed=-1;
    }
    //Free buffers
    kfree(MYBUFF);
    MEMMAP_DEBUG_PRINT("MemMap Flushing size %lu for data %p\n", len, data);
    return len;
}
