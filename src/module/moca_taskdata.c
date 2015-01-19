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

#define MOCA_BUF_SIZE 4096
#define MOCA_DATA_STATUS_NORMAL 0
//Data won't be written anymore, but need to be saved to file
#define MOCA_DATA_STATUS_NEEDFLUSH 1
//Data have been outputed  but FlushData must be called again for EOF
#define MOCA_DATA_STATUS_DYING -1
//We can free data (after removing the /proc entry)
#define MOCA_DATA_STATUS_ZOMBIE -2
#define MOCA_DATA_STATUS_DYING_OR_ZOMBIE(data) ((data)->status < 0)
#define MOCA_HASH_BITS 14

int Moca_taskDataHashBits=MOCA_HASH_BITS;
int Moca_taskDataChunkSize=2*(1<<MOCA_HASH_BITS);
int Moca_nbChunks=20;

#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/cpumask.h> //num_online_cpus
#include <asm/uaccess.h>  /* for copy_*_user */
#include <linux/delay.h>
#include "moca.h"
#include "moca_taskdata.h"
#include "moca_tasks.h"
#include "moca_hashmap.h"



static struct proc_dir_entry *Moca_proc_root;

static ssize_t Moca_FlushData(struct file *filp,  char *buffer,
        size_t length, loff_t * offset);

static const struct file_operations Moca_taskdata_fops = {
    .owner   = THIS_MODULE,
    .read    = Moca_FlushData,
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

int Moca_nextTaskId=0;

int Moca_CurrentChunk(task_data data)
{
    int ret=-1;
    spin_lock(&data->lock);
    ret=data->cur;
    spin_unlock(&data->lock);
    return ret;
}

void Moca_InitTaskData(void)
{
    //Procfs init
    Moca_proc_root=proc_mkdir("Moca", NULL);
    if(!Moca_proc_root)
        Moca_Panic("Moca Unable to create proc root entry");
}

task_data Moca_InitData(struct task_struct *t)
{
    int i;
    task_data data;
    char buf[10];
    //We must not wait here !
    data=kmalloc(sizeof(struct _task_data),GFP_ATOMIC);
    MOCA_DEBUG_PRINT("Moca Initialising data for task %p\n",t);
    if(!data)
    {
        Moca_Panic("Moca unable to allocate data ");
        return NULL;
    }
    data->chunks=kmalloc(sizeof(chunk)*Moca_nbChunks,GFP_ATOMIC);
    if(!data->chunks)
    {
        kfree(data);
        Moca_Panic("Moca unable to allocate chunks");
        return NULL;
    }
    for(i=0;i<Moca_nbChunks;i++)
    {
        MOCA_DEBUG_PRINT("Moca Initialising data chunk %d for task %p\n",i, t);
        data->chunks[i]=kmalloc(sizeof(chunk),GFP_ATOMIC);
        if(!data->chunks[i])
        {
            Moca_Panic("Moca unable to allocate data chunk");
            return NULL;
        }
        data->chunks[i]->startClock=0;
        data->chunks[i]->endClock=0;
        data->chunks[i]->cpu=0;
        data->chunks[i]->used=0;
        data->chunks[i]->map=Moca_InitHashMap(Moca_taskDataHashBits,
                Moca_taskDataChunkSize, sizeof(struct _chunk_entry));
        spin_lock_init(&data->chunks[i]->lock);
    }
    data->task=t;
    data->cur=0;
    data->chunks[0]->startClock=Moca_GetClock();
    data->internalId=Moca_nextTaskId++;
    data->nbflush=0;
    data->status=MOCA_DATA_STATUS_NORMAL;
    snprintf(buf,10,"task%d",data->internalId);
    data->proc_entry=proc_create_data(buf,0,Moca_proc_root,
            &Moca_taskdata_fops,data);
    data->currentlyFlushed=-1;
    spin_lock_init(&data->lock);
    return data;
}

void Moca_ClearAllData(void)
{
    int i=0, nbTasks,chunkid;
    moca_task t;
    char buf[10];
    MOCA_DEBUG_PRINT("Moca Cleaning data\n");
    nbTasks=Moca_GetNumTasks();
    while((t=Moca_NextTask(&i)))
    {
        MOCA_DEBUG_PRINT("Moca asking data %d %p %p to end\n",i, t->data, t->key);
        t->data->status=MOCA_DATA_STATUS_NEEDFLUSH;
    }
    i=0;
    while((t=Moca_NextTask(&i)))
    {
        //Wait for the task to be dead
        MOCA_DEBUG_PRINT("Moca waiting data %d to end\n",i);
        while(t->data->status!=MOCA_DATA_STATUS_ZOMBIE)
            msleep(100);
        MOCA_DEBUG_PRINT("Moca data %d %p %p ended\n",i, t->data, t->key);
        snprintf(buf,10,"task%d",i-1);
        remove_proc_entry(buf, Moca_proc_root);
        //Clean must be done after removing the proc entry
        for(chunkid=0; chunkid < Moca_nbChunks;++chunkid)
        {
            MOCA_DEBUG_PRINT("Memap Freeing data %p chunk %d\n",
                    t->data, chunkid);
            Moca_FreeMap(t->data->chunks[chunkid]->map);
            kfree(t->data->chunks[chunkid]);
        }
        kfree(t->data);
        Moca_RemoveTask(t->key);
        MOCA_DEBUG_PRINT("Memap Freed data %d \n", i);
    }
    MOCA_DEBUG_PRINT("Moca Removing proc root\n");
    remove_proc_entry("Moca", NULL);

    MOCA_DEBUG_PRINT("Moca all data cleaned\n");
}



struct task_struct *Moca_GetTaskFromData(task_data data)
{
    return data->task;
}

void Moca_LockChunk(task_data data)
{
    spin_lock(&data->chunks[Moca_CurrentChunk(data)]->lock);
}

void Moca_UnlockChunk(task_data data)
{
    spin_unlock(&data->chunks[Moca_CurrentChunk(data)]->lock);
}

int Moca_AddToChunk(task_data data, void *addr, int cpu)
{
    int status, cur;
    chunk_entry e;
    cur=Moca_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return -1;
    }
    MOCA_DEBUG_PRINT("Moca hashmap adding %p to chunk %d %p data %p cpu %d\n",
            addr, cur, data->chunks[cur],data, cpu);
    e=(chunk_entry)Moca_AddToMap(data->chunks[cur]->map,addr, &status);
    switch(status)
    {
        case MOCA_HASHMAP_FULL :
            Moca_Panic("Moca hashmap full");
            spin_unlock(&data->chunks[cur]->lock);
            return -1;
            break;
        case MOCA_HASHMAP_ERROR :
            Moca_Panic("Moca hashmap error");
            spin_unlock(&data->chunks[cur]->lock);
            return -1;
            break;
        case MOCA_HASHMAP_ALREADY_IN_MAP :
            MOCA_DEBUG_PRINT("Moca addr already in chunk %p\n", addr);
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
    MOCA_DEBUG_PRINT("Moca inserted %p\n", addr);
    return 0;
}

int Moca_UpdateData(task_data data,int pos, int countR, int countW, int cpu)
{
    chunk_entry e;
    int cur=Moca_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return -1;
    }
    e=(chunk_entry )Moca_EntryAtPos(data->chunks[cur]->map,
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

int Moca_NextChunks(task_data data)
{
    int cur;
    MOCA_DEBUG_PRINT("Moca Goto next chunks %p, %d\n", data, data->cur);
    //Global lock
    spin_lock(&data->lock);
    cur=data->cur;
    spin_lock(&data->chunks[cur]->lock);
    data->chunks[cur]->endClock=Moca_GetClock();
    data->chunks[cur]->used=1;
    data->cur=(cur+1)%Moca_nbChunks;
    spin_unlock(&data->chunks[cur]->lock);
    spin_unlock(&data->lock);
    MOCA_DEBUG_PRINT("Moca Goto chunks  %p %d, %d\n", data, data->cur,
            Moca_nbChunks);
    if(data->chunks[data->cur]->used)
    {
        printk(KERN_ALERT "Moca no more chunks, stopping trace for task %d\n You can fix that by relaunching Moca either with a higher number of chunks\n or by decreasing the logging daemon wakeupinterval\n",
                data->internalId);
        return 1;
    }
    else
        data->chunks[data->cur]->startClock=Moca_GetClock();
    return 0;
}


void *Moca_AddrInChunkPos(task_data data,int pos)
{
    chunk_entry e;
    int cur=Moca_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return NULL;
    }
    MOCA_DEBUG_PRINT("Moca Looking for next addr in ch %d, pos %d/%u\n",
            cur, pos, Moca_NbElementInMap(data->chunks[cur]->map));
    e=(chunk_entry)Moca_EntryAtPos(data->chunks[cur]->map,
            pos);
    spin_unlock(&data->chunks[cur]->lock);
    if(!e)
        return NULL;
    MOCA_DEBUG_PRINT("Moca found adress %p\n", e->key);
    return e->key;
}


int Moca_CpuMask(int cpu, char *buf, size_t size)
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
        Moca_Panic("Moca Buffer overflow in CpuMask");
        return 0;
    }
    return i;
}


#define LINE_SZ 80
static ssize_t Moca_FlushData(struct file *filp,  char *buffer,
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
    MOCA_DEBUG_PRINT("Moca_Flushing data %p allowed len %lu\n", data, length);


    MYBUFF=kmalloc(MOCA_BUF_SIZE,GFP_ATOMIC);
    if(!MYBUFF)
    {
        //We might already be in a panic: don't panic, just free stuff
        printk(KERN_WARNING "Moca unable to allocate buffer in flush data\n");
        data->status=MOCA_DATA_STATUS_DYING;
    }
    if(MOCA_DATA_STATUS_DYING_OR_ZOMBIE(data))
    {
        //Data already flush, do noting and wait for kfreedom
        data->status=MOCA_DATA_STATUS_ZOMBIE;
        return 0;
    }

    if(data->nbflush==0)
    {
        //First flush
        MOCA_DEBUG_PRINT("Moca first flush for data %p\n", data);

        sz=snprintf(MYBUFF,MOCA_BUF_SIZE,"Taskdata %d %d %p\n",
                data->internalId,task_pid_nr(data->task), data->task);
        MOCA_DEBUG_PRINT("Moca buf size %lu\n", sz);
        if(!copy_to_user(buffer, MYBUFF,sz))
            len+=sz;
        MOCA_DEBUG_PRINT("Moca user size %lu\n", len);
    }

    //Iterate on all chunks, start where we stopped if needed
    for(chunkid=(data->currentlyFlushed<0)?0:data->currentlyFlushed;
            chunkid < Moca_nbChunks;++chunkid)
    {
        //If we are resuming a Flush, we are already helding the lock
        if(chunkid!=data->currentlyFlushed)
            spin_lock(&data->chunks[chunkid]->lock);
        if(data->status==MOCA_DATA_STATUS_NEEDFLUSH
                || data->chunks[chunkid]->used)
        {
            if((nelt=Moca_NbElementInMap(data->chunks[chunkid]->map))>0)
            {
                //If we are resuming, no need to output this line again
                if(chunkid!=data->currentlyFlushed)
                {
                    if(chunkid==data->cur)
                        data->chunks[chunkid]->endClock=Moca_GetClock();
                    //Chunk id  nb element startclock endclock cpumask
                    sz=snprintf(MYBUFF,MOCA_BUF_SIZE,"Chunk %d %d %lu %lu ",
                            chunkid+data->nbflush*Moca_nbChunks,
                            Moca_NbElementInMap(data->chunks[chunkid]->map),
                            data->chunks[chunkid]->startClock,
                            data->chunks[chunkid]->endClock);
                    sz+=Moca_CpuMask(data->chunks[chunkid]->cpu, MYBUFF+sz,
                            MOCA_BUF_SIZE-sz);
                    MYBUFF[sz++]='\n';
                    if(!copy_to_user(buffer+len,MYBUFF,sz))
                        len+=sz;
                }
                else
                    MOCA_DEBUG_PRINT("Moca resuming flush chunk %d, available space %lu\n",
                            chunkid, length-len);
                ind=0;
                while((e=(chunk_entry)Moca_NextEntryPos(data->chunks[chunkid]->map,&ind)))
                {
                    if(len+LINE_SZ >= length)
                    {
                        complete=0;
                        break;
                    }
                    //Access address countread countwrite cpumask
                    sz=snprintf(MYBUFF,MOCA_BUF_SIZE,"Access %p %d %d ",
                            e->key, e->countR, e->countW);
                    sz+=Moca_CpuMask(e->cpu,MYBUFF+sz,MOCA_BUF_SIZE-sz);
                    MYBUFF[sz++]='\n';
                    if(!copy_to_user(buffer+len,MYBUFF,sz))
                        len+=sz;
                    //Re init data
                    Moca_RemoveFromMap(data->chunks[chunkid]->map,e->key);
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
                if((nelt=Moca_NbElementInMap(data->chunks[chunkid]->map))!=0)
                    MOCA_DEBUG_PRINT("Moca Still %d elt, ch %d atfer complete flush\n",
                            nelt, chunkid);
                data->chunks[chunkid]->startClock=Moca_GetClock();
                data->chunks[chunkid]->endClock=Moca_GetClock();
            }
        }
        if(complete)
            spin_unlock(&data->chunks[chunkid]->lock);
        else
        {
            MOCA_DEBUG_PRINT("Moca stopping flush chunk %d, available space %lu\n",
                    chunkid, length-len);
            data->currentlyFlushed=chunkid;
            break;
        }
    }
    if(!data->chunks[0]->used)
        data->chunks[0]->startClock=Moca_GetClock();
    if(complete)
    {
        ++data->nbflush;
        if(data->status==MOCA_DATA_STATUS_NEEDFLUSH )
            data->status=MOCA_DATA_STATUS_DYING;
        data->currentlyFlushed=-1;
    }
    //Free buffers
    kfree(MYBUFF);
    MOCA_DEBUG_PRINT("Moca Flushing size %lu for data %p\n", len, data);
    return len;
}