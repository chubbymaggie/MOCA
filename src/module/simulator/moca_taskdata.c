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

#define MOCA_DATA_STATUS_NORMAL 0
//Data won't be written anymore, but need to be saved to file
#define MOCA_DATA_STATUS_NEEDFLUSH 1
//Data have been outputed  but FlushData must be called again for EOF
#define MOCA_DATA_STATUS_DYING -1
//We can free data (after removing the /proc entry)
#define MOCA_DATA_STATUS_ZOMBIE -2
#define MOCA_DATA_STATUS_DYING_OR_ZOMBIE(data) ((data)->status < 0)
#define MOCA_HASH_BITS 15

#define MOCA_CHUNK_NORMAL 0
#define MOCA_CHUNK_ENDING 1
#define MOCA_CHUNK_USED 2

int Moca_taskDataHashBits=MOCA_HASH_BITS;
int Moca_taskDataChunkSize=1<<(MOCA_HASH_BITS+1);
int Moca_nbChunks=30;





 //num_online_cpus
  /* for copy_*_user */

#include "moca.h"
#include "moca_taskdata.h"
#include "moca_tasks.h"
#include "moca_hashmap.h"
#include "moca_false_pf.h"




static struct proc_dir_entry *Moca_proc_root=NULL;

static ssize_t Moca_FlushData(struct file *filp,  char *buffer,
        size_t length, loff_t * offset);

static const struct file_operations Moca_taskdata_fops = {
    .owner   = THIS_MODULE,
    .read    = Moca_FlushData,
};

typedef struct _chunk_entry
{
    void *key; //Virtual Address
    int next;
    int countR;
    int countW;
    int cpu;
}*chunk_entry;

typedef struct
{
    hash_map map;
    long startClock;
    long endClock;
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
    int currentlyFlushedPos;
    int internalId;
    int nbflush;
    int status;
    spinlock_t lock;
    struct proc_dir_entry *proc_entry;
}*task_data;

static atomic_t Moca_nextTaskId=ATOMIC_INIT(0);

int Moca_CurrentChunk(task_data data)
{
    int ret=-1;
    spin_lock(&data->lock);
    ret=data->cur;
    spin_unlock(&data->lock);
    return ret;
}

int Moca_InitTaskData(void)
{
    //Procfs init
    Moca_proc_root=proc_mkdir("Moca", NULL);
    if(!Moca_proc_root)
        return 1;
    return 0;
}

void Moca_ChunkEntryInitializer(void *e)
{
    chunk_entry ce=(chunk_entry)e;
    ce->countR=0;
    ce->countW=0;
    ce->cpu=0;
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
        goto fail;
    data->chunks=kcalloc(Moca_nbChunks,sizeof(chunk),GFP_ATOMIC);
    if(!data->chunks)
    {
        goto clean;
    }
    for(i=0;i<Moca_nbChunks;i++)
    {
        MOCA_DEBUG_PRINT("Moca Initialising data chunk %d for task %p\n",i, t);
        data->chunks[i]=kcalloc(1,sizeof(chunk),GFP_ATOMIC);
        if(!data->chunks[i])
            goto cleanChunks;
        data->chunks[i]->cpu=0;
        data->chunks[i]->startClock=-1;
        data->chunks[i]->used=MOCA_CHUNK_NORMAL;
        data->chunks[i]->map=Moca_InitHashMap(Moca_taskDataHashBits,
                Moca_taskDataChunkSize, sizeof(struct _chunk_entry), NULL,
                Moca_ChunkEntryInitializer);
        if(!data->chunks[i]->map)
            goto cleanChunks;
        spin_lock_init(&data->chunks[i]->lock);
    }
    data->task=t;
    data->cur=0;
    data->internalId=atomic_inc_return(&Moca_nextTaskId)-1;
    data->nbflush=0;
    data->status=MOCA_DATA_STATUS_NORMAL;
    snprintf(buf,10,"task%d",data->internalId);
    data->proc_entry=proc_create_data(buf,0,Moca_proc_root,
            &Moca_taskdata_fops,data);
    data->currentlyFlushed=-1;
    data->currentlyFlushedPos=-1;
    spin_lock_init(&data->lock);
    return data;

cleanChunks:
    i=0;
    while(i< Moca_nbChunks && data->chunks[i]!=NULL)
    {
        if(data->chunks[i]->map!=NULL)
            Moca_FreeMap(data->chunks[i]->map);
        kfree(data->chunks[i]);
        ++i;
    }
clean:
    kfree(data);
fail:
        printk("Moca fail initializing data for task %p\n",t);
        return NULL;

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
        spin_lock(&t->data->lock);
        t->data->status=MOCA_DATA_STATUS_NEEDFLUSH;
        spin_unlock(&t->data->lock);
    }
    MOCA_DEBUG_PRINT("Moca flushing\n");
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
        kfree(t->data->chunks);
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
    if(!data)
        return NULL;
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

int Moca_AddToChunk(task_data data, void *addr, int cpu, int write)
{
    int status, cur;
    struct _chunk_entry tmp;
    chunk_entry e;
    cur=Moca_CurrentChunk(data);
    spin_lock(&data->chunks[cur]->lock);
    if(data->chunks[cur]->used!=MOCA_CHUNK_NORMAL)
    {
        spin_unlock(&data->chunks[cur]->lock);
        return -1;
    }
    MOCA_DEBUG_PRINT("Moca hashmap adding %p to chunk %d %p data %p cpu %d\n",
            addr, cur, data->chunks[cur],data, cpu);
    tmp.key=addr;
    e=(chunk_entry)Moca_AddToMap(data->chunks[cur]->map,(hash_entry)&tmp, &status);
    switch(status)
    {
        case MOCA_HASHMAP_FULL :
            spin_unlock(&data->chunks[cur]->lock);
            printk(KERN_INFO "Moca chunk full, part of the trace will be lost");
            return -1;
            break;
        case MOCA_HASHMAP_ERROR :
            spin_unlock(&data->chunks[cur]->lock);
            printk("Moca hashmap error");
            return -1;
            break;
        case MOCA_HASHMAP_ALREADY_IN_MAP :
            MOCA_DEBUG_PRINT("Moca addr already in chunk %p\n", addr);
            ++e->countR;
            e->countW+=write;
            break;
        default :
            //Normal add
            e->countR=1;
            e->countW=write;
            break;
    }
    e->cpu|=1<<cpu;
    data->chunks[cur]->cpu|=1<<cpu;
    data->chunks[cur]->endClock=Moca_GetClock();
    if(data->chunks[cur]->startClock==-1)
        data->chunks[cur]->startClock=data->chunks[cur]->endClock;
    spin_unlock(&data->chunks[cur]->lock);
    return 0;
}

int Moca_NextChunks(task_data data)
{
    int cur,old,ret;
    MOCA_DEBUG_PRINT("Moca Goto next chunks %p, %d\n", data, data->cur);
    //Global lock
    spin_lock(&data->lock);
    ret=old=data->cur;
    spin_lock(&data->chunks[old]->lock);
    data->chunks[old]->used=MOCA_CHUNK_ENDING;
    cur=old;
    do{
        spin_unlock(&data->chunks[cur]->lock);
        cur=(cur+1)%Moca_nbChunks;
        spin_lock(&data->chunks[cur]->lock);
    }while(data->chunks[cur]->used!=MOCA_CHUNK_NORMAL && cur != old);
    data->cur=cur;
    spin_unlock(&data->chunks[cur]->lock);
    spin_unlock(&data->lock);
    MOCA_DEBUG_PRINT("Moca Goto chunks  %p %d, %d\n", data, cur,
            Moca_nbChunks);
    if(cur==old)
    {
        printk(KERN_ALERT "Moca no more chunks, stopping trace for task %d\n You can fix that by relaunching Moca either with a higher number of chunks\n or by decreasing the logging daemon wakeupinterval\n",
                data->internalId);
    }
    return ret;
}

void Moca_EndChunk(task_data data, int id)
{
    spin_lock(&data->chunks[id]->lock);
    data->chunks[id]->used=MOCA_CHUNK_USED;
    spin_unlock(&data->chunks[id]->lock);
}



void *Moca_AddrInChunkPos(task_data data,int *pos, int ch)
{
    chunk_entry e;
    if(ch < 0 || ch > Moca_nbChunks)
        return NULL;
    spin_lock(&data->chunks[ch]->lock);
    if(data->chunks[ch]->used==MOCA_CHUNK_USED)
    {
        spin_unlock(&data->chunks[ch]->lock);
        return NULL;
    }
    MOCA_DEBUG_PRINT("Moca Looking for next addr in ch %d, pos %d/%d\n",
            ch, *pos, Moca_NbElementInMap(data->chunks[ch]->map));
    e=(chunk_entry)Moca_NextEntryPos(data->chunks[ch]->map,pos);
    spin_unlock(&data->chunks[ch]->lock);
    if(!e)
        return NULL;
    MOCA_DEBUG_PRINT("Moca found adress %p at pos %d\n", e->key, *pos-1);
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
        printk("Moca Buffer overflow in CpuMask");
        return 0;
    }
    return i;
}

#define LINE_WIDTH 320
static ssize_t Moca_FlushData(struct file *filp,  char *buffer,
        size_t length, loff_t * offp)
{
    // size in kernel buffer, user buffer, total user buffer
    int chunkid, ind, nelt, complete=1;
    size_t sz=0;
    chunk_entry e;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    task_data data=(task_data)PDE_DATA(file_inode(filp));
#else
    task_data data=(task_data)PDE(filp->f_path.dentry->d_inode)->data;
#endif
    MOCA_DEBUG_PRINT("Moca_Flushing data %p allowed len %lu\n", data, length);

    if(MOCA_DATA_STATUS_DYING_OR_ZOMBIE(data))
    {
        //Data already flush, do noting and wait for kfreedom
        data->status=MOCA_DATA_STATUS_ZOMBIE;
        return 0;
    }

    if(data->nbflush==0 && data->currentlyFlushed==-1)
    {
        //First flush
        MOCA_DEBUG_PRINT("Moca first flush for data %p\n", data);

        //Task id pid
        if(data->internalId!=0){
            sz+=snprintf(buffer,length,"T %d %d\n",
                    data->internalId,task_pid_nr(data->task));
        }else{
            sz+=snprintf(buffer,length,"T %d %d %lu\n",
                    data->internalId,task_pid_nr(data->task), PAGE_SIZE);
        }
        MOCA_DEBUG_PRINT("Moca header size %lu\n", sz);
    }

    //Iterate on all chunks, start where we stopped if needed
    for(chunkid=(data->currentlyFlushed<0)?0:data->currentlyFlushed;
            chunkid < Moca_nbChunks;++chunkid)
    {
        //If we are resuming a Flush, we are already helding the lock
        if(chunkid!=data->currentlyFlushed)
            spin_lock(&data->chunks[chunkid]->lock);
        if(data->status==MOCA_DATA_STATUS_NEEDFLUSH
                || data->chunks[chunkid]->used==MOCA_CHUNK_USED)
        {
            if((nelt=Moca_NbElementInMap(data->chunks[chunkid]->map))>0)
            {
                //If we are resuming, no need to output this line again
                if(chunkid!=data->currentlyFlushed)
                {
                    if(data->chunks[chunkid]->endClock==data->chunks[chunkid]->startClock)
                        ++data->chunks[chunkid]->endClock;
                    //Chunk id  nb element startclock endclock cpumask
                    sz+=snprintf(buffer+sz,length-sz,"C %d %d %lu %lu ", chunkid,
                            Moca_NbElementInMap(data->chunks[chunkid]->map),
                            data->chunks[chunkid]->startClock,
                            data->chunks[chunkid]->endClock);
                    sz+=Moca_CpuMask(data->chunks[chunkid]->cpu, buffer+sz,
                            length-sz);
                    buffer[sz++]='\n';
                    ind=0;
                }
                else
                {
                    MOCA_DEBUG_PRINT("Moca resuming flush chunk %d, available space %lu\n",
                            chunkid, length);
                    ind=data->currentlyFlushedPos;
                }
                while((e=(chunk_entry)Moca_NextEntryPos(data->chunks[chunkid]->map,&ind)))
                {
                    if(LINE_WIDTH >= length-sz)
                    {
                        complete=0;
                        break;
                    }
                    //Access @Virt @Phy countread countwrite cpumask
                    sz+=snprintf(buffer+sz,length-sz,"A %p %p %d %d ",
                            e->key,
                            Moca_PhyFromVirt(e->key, data->task->mm),
                            e->countR, e->countW);
                    sz+=Moca_CpuMask(e->cpu,buffer+sz,length-sz);
                    buffer[sz++]='\n';
                    //Re init data
                    e->countR=0;
                    e->countW=0;
                    e->cpu=0;
                }
            }
            if(complete)
            {
                Moca_ClearMap(data->chunks[chunkid]->map);
                data->chunks[chunkid]->cpu=0;
                data->chunks[chunkid]->startClock=-1;
                data->chunks[chunkid]->used=MOCA_CHUNK_NORMAL;
            }
        }
        if(complete)
        {
            spin_unlock(&data->chunks[chunkid]->lock);
        }
        else
        {
            MOCA_DEBUG_PRINT("Moca stopping flush chunk %d, available space %lu\n",
                    chunkid, length);
            data->currentlyFlushed=chunkid;
            data->currentlyFlushedPos=ind-1;
            break;
        }
    }
    if(complete)
    {
        spin_lock(&data->lock);
        ++data->nbflush;
        data->currentlyFlushed=-1;
        data->currentlyFlushedPos=-1;
        if(data->status==MOCA_DATA_STATUS_NEEDFLUSH )
            data->status=MOCA_DATA_STATUS_DYING;
        MOCA_DEBUG_PRINT("Moca Complete Flush data %p %d\n",
                data,data->status);
        spin_unlock(&data->lock);
    }

    // We should handle properly copy_to_user return value
    MOCA_DEBUG_PRINT("Moca Flushing size %lu for data %p chunk %d curflushed %d\n",
            sz, data,chunkid,data->currentlyFlushed);

    return sz;
}
