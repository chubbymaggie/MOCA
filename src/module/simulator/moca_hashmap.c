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

 //malloc

 //memcpy
#include "moca_hashmap.h"

#define HASH(k,m) (((int)hash_ptr((k),(m)->hash_bits))%(m)->size)

#define MOCA_HASHMAP_END -1
#define tableElt(map, ind) \
    ( (hash_entry)((char *)((map)->table)+((ind)*((map)->elt_size)) ))

typedef struct _hash_map
{
    int *hashs;
    int nbentry;
    int hash_bits;
    int size;
    int tableSize;
    size_t elt_size;
    comp_fct_t comp;
    init_fct_t init;
    struct _hash_entry *table;
}*hash_map;

int Moca_DefaultHashMapComp(hash_entry e1, hash_entry e2)
{
    unsigned long k1=(unsigned long)(e1->key),k2=(unsigned long)(e2->key);
    return k1-k2;
}

unsigned int Moca_FindNextAvailPosMap(hash_map map)
{
    int i=0;
    while(i< map->tableSize && tableElt(map,i)->key!=NULL)
        ++i;
    return i;
}

hash_map Moca_InitHashMap(unsigned long hash_bits, int nb_elt,
        size_t elt_size, comp_fct_t comp, init_fct_t init)
{
    unsigned int i;
    hash_map map=kmalloc(sizeof(struct _hash_map), GFP_ATOMIC);
    if(!map)
        return NULL;
    map->hash_bits=hash_bits;
    map->size=1<<hash_bits;
    map->tableSize=nb_elt;
    map->nbentry=0;
    map->elt_size=elt_size;
    map->init=init;
    if(comp)
        map->comp=comp;
    else
        map->comp=&Moca_DefaultHashMapComp;

    MOCA_DEBUG_PRINT("Moca allocationg hash size %lu\n", map->size);
    if(!(map->hashs=kmalloc(sizeof(int)*map->size,GFP_ATOMIC)))
    {
        kfree(map);
        return NULL;
    }
    if(!(map->table=kcalloc(map->tableSize,elt_size,GFP_ATOMIC)))
    {
        kfree(map->hashs);
        kfree(map);
        return NULL;
    }
    // Init entries
    for(i=0;i<map->size;++i)
        map->hashs[i]=MOCA_HASHMAP_END;
    for(i=0;i<map->tableSize;++i)
    {
        if(map->init!=NULL)
            map->init(tableElt(map,i));
        tableElt(map,i)->next=MOCA_HASHMAP_END;
        tableElt(map,i)->key=NULL;
    }
    return map;
}

int Moca_NbElementInMap(hash_map map)
{
    if(!map)
        return -1;
    return map->nbentry;
}

/*
 * Returns -1 if key is not in map
 *         the position of key in the map if it is present
 */
int Moca_PosInMap(hash_map map,hash_entry e)
{
    int h;
    int ind=0;
    if(!map || !e)
        return -1;
    h=HASH(e->key, map);
    ind=map->hashs[h];
    while(ind>=0 && map->comp(tableElt(map,ind),e)!=0 )
        ind=tableElt(map,ind)->next;
    return ind;
}

/*
 * Return the hash entry corresponding to key,
 *        NULL if key is not in the map
 */
hash_entry Moca_EntryFromKey(hash_map map, hash_entry e)
{
    int h;
    int ind=0;
    if(!map || !e)
        return NULL;
    h=HASH(e->key, map);
    ind=map->hashs[h];
    while(ind>=0 && ind < map->tableSize && map->comp(tableElt(map,ind),e)!=0 )
        ind=tableElt(map,ind)->next;
    if(ind > 0 &&  ind >= map->tableSize)
    {
        printk("Moca Hashmap ind %d >= max: %u\n",ind, map->tableSize);
        dump_stack();
        return NULL;
    }
    if(ind >=0)
        return tableElt(map,ind);
    return NULL;
}

/*
 * Insert key in map
 * Returns A pointer to the hash_entry corresponding to key
 *         Null in case of error
 * status is set to:
 *         The position of hash_entry in case of success
 *         One of the following in case of errors:
 *          MOCA_HASHMAP_ALREADY_IN_MAP
 *          MOCA_HASHMAP_FULL
 *          MOCA_HASHMAP_ERROR
 */
hash_entry Moca_AddToMap(hash_map map, hash_entry e, int *status)
{
    int h;
    int ind=0, nextPos;
    if(!map)
    {
        *status=MOCA_HASHMAP_ERROR;
        return NULL;
    }
    if(map->nbentry==map->tableSize)
    {
        *status=MOCA_HASHMAP_FULL;
        return NULL;
    }
    //Do the insertion
    nextPos=Moca_FindNextAvailPosMap(map);
    MOCA_DEBUG_PRINT("Moca inserting %p ind %d/%lu total %d\n",
            e->key,nextPos,map->tableSize, map->nbentry);
    if(nextPos >= map->tableSize)
    {
        *status=MOCA_HASHMAP_ERROR;
        return NULL;
    }
    //Update the link
    h=HASH(e->key, map);
    ind=map->hashs[h];
    //memcpy(tableElt(map,nextPos),e,map->elt_size);
    tableElt(map,nextPos)->key=e->key;
    tableElt(map,nextPos)->next=MOCA_HASHMAP_END;
    if(ind<0)
    {
        //Normal insert
        map->hashs[h]=nextPos;
    }
    else
    {
        // Conflict
        // Go to the queue of conflicting hash
        while(map->comp(tableElt(map,ind),e)!=0 && tableElt(map,ind)->next>=0)
            ind=tableElt(map,ind)->next;
        if(map->comp(tableElt(map,ind),e)==0)
        {
            MOCA_DEBUG_PRINT("Moca %p already in map %p\n", e->key, map);
            *status=MOCA_HASHMAP_ALREADY_IN_MAP;
            // Cancel insertion
            if(map->init)
                map->init(tableElt(map,nextPos)); // WUUUT ???
            tableElt(map,nextPos)->key=NULL;
            return tableElt(map,ind);
        }
        // Insert in queue
        MOCA_DEBUG_PRINT("Moca collision in map %p key %p\n", map, e->key);
        tableElt(map,ind)->next=nextPos;
    }
    ++map->nbentry;
    MOCA_DEBUG_PRINT("Moca Inserted %p in map %p\n", e->key, map);
    *status=nextPos;
    return tableElt(map,nextPos);
}

/*
 * Returns the hash entry at position pos
 *         Null is pos is invalid or there is no entry at this position
 */
hash_entry Moca_EntryAtPos(hash_map map, int pos)
{
    if(!map || pos >= map->tableSize || pos < 0 ||
            tableElt(map,pos)->key==NULL)
        return NULL;
    return tableElt(map,pos);
}
/*
 * Find the first available entry from pos
 * Returns the entry on success
 *         NULL if there is no more entry
 * After the call to Memmap_NextEntryPos, pos is the position of the returned
 * entry +1
 * This function can be used as an iterator
 */
hash_entry Moca_NextEntryPos(hash_map map, int *pos)
{
    int i=*pos;
    MOCA_DEBUG_PRINT("Moca Searching element in map %p after %d /%d\n",
            map, i, Moca_NbElementInMap(map));
    while(i< map->tableSize && tableElt(map,i)->key==NULL)
        ++i;
    *pos=i+1;
    MOCA_DEBUG_PRINT("Moca found  %p at %d\n",
            i>=map->tableSize?NULL:tableElt(map,i),i);
    if(i >=map->tableSize)
        return NULL;
    return tableElt(map,i);
}

int Moca_ConditionalRemove(hash_map map, int (*fct)(void*))
{
    int h, ind, prev=MOCA_HASHMAP_END, remove=0;
    hash_entry e;
    for(ind=0;ind<map->tableSize;++ind)
    {
        e=tableElt(map,ind);
        if(e->key!=NULL && fct(e)==0)
        {
            h=HASH(e->key,map);
            prev=map->hashs[h];
            if(prev==ind)
            {
                // Normal remove
                map->hashs[h]=e->next;
            }
            else
            {
                // Find actual predecessor
                while(prev>=0 && tableElt(map,prev)->next!=ind)
                    prev=tableElt(map,prev)->next;
                if(prev==MOCA_HASHMAP_END)
                    return -1;
                tableElt(map,prev)->next=e->next;
            }
            e->key=NULL;
            e->next=MOCA_HASHMAP_END;
            --map->nbentry;
            ++remove;
        }
    }
    return remove;
}

hash_entry Moca_RemoveFromMap(hash_map map,hash_entry e)
{
    int h;
    int ind, ind_prev=MOCA_HASHMAP_END;
    if(!map)
        return NULL;
    MOCA_DEBUG_PRINT("Moca removing %p from %p\n", e->key, map);
    h=HASH(e->key, map);
    ind=map->hashs[h];
    while(ind>=0 && map->comp(tableElt(map,ind),e)!=0 )
    {
        ind_prev=ind;
        ind=tableElt(map,ind)->next;
    }
    MOCA_DEBUG_PRINT("Moca removing %p from %p ind %d prev %d\n", e->key, map,
            ind, ind_prev);
    //key wasn't in map
    if(ind<0 )
        return NULL;
    //Remove from list
    if(ind_prev>=0)
    {
        tableElt(map,ind_prev)->next=tableElt(map,ind)->next;
    }
    else
    {
        map->hashs[h]=tableElt(map,ind)->next;
    }
    tableElt(map,ind)->key=NULL;
    tableElt(map,ind)->next=MOCA_HASHMAP_END;
    --map->nbentry;
    MOCA_DEBUG_PRINT("Moca removing %p from %p ind %d ok\n", e->key, map, ind);
    return tableElt(map,ind);
}


// Clear map, after this call, map is still usable
void Moca_ClearMap(hash_map map)
{
    int i;
    if(!map)
        return;
    for(i=0;i<map->size;++i)
        map->hashs[i]=MOCA_HASHMAP_END;
    for(i=0;i<map->tableSize;++i)
    {
        map->init(tableElt(map,i));
        tableElt(map,i)->key=NULL;
        tableElt(map,i)->next=MOCA_HASHMAP_END;
    }
    map->nbentry=0;
}

void Moca_FreeMap(hash_map map)
{
    if(!map)
        return;
    if(map->table)
        kfree(map->table);
    if(map->hashs)
        kfree(map->hashs);
    kfree(map);
}
