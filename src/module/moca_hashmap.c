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

#include <linux/slab.h>
#include <linux/hash.h>
#include "moca_hashmap.h"


#define MOCA_HASHMAP_END -1
#define MOCA_HASHMAP_UNUSED -2
#define tableElt(map, ind) \
    ( (hash_entry)((char *)((map)->table)+((ind)*((map)->elt_size)) ))

typedef struct _hash_map
{
    int *hashs;
    hash_entry *table;
    unsigned int nbentry;
    unsigned long hash_bits;
    unsigned long size;
    unsigned long tableSize;
    size_t elt_size;
}*hash_map;

unsigned int Moca_FindNextAvailPosMap(hash_map map)
{
    unsigned int i=0;
    while(i< map->tableSize && tableElt(map,i)->next!=MOCA_HASHMAP_UNUSED)
        ++i;
    return i;
}

hash_map Moca_InitHashMap(unsigned long hash_bits, int nb_elt,
        size_t elt_size)
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
    MOCA_DEBUG_PRINT("Moca allocationg hash size %lu\n", map->size);
    if(!(map->hashs=kmalloc(sizeof(int)*map->size,GFP_ATOMIC)))
    {
        kfree(map);
        return NULL;
    }
    for(i=0;i<map->size;++i)
        map->hashs[i]=MOCA_HASHMAP_END;
    if(!(map->table=kcalloc(map->tableSize,elt_size,GFP_ATOMIC)))
    {
        kfree(map->hashs);
        kfree(map);
        return NULL;
    }
    for(i=0;i<map->tableSize;++i)
        tableElt(map,i)->next=MOCA_HASHMAP_UNUSED;
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
int Moca_PosInMap(hash_map map,void *key)
{
    unsigned long h;
    int ind=0;
    if(!map)
        return -1;
    h=hash_ptr(key, map->hash_bits);
    ind=map->hashs[h];
    while(ind>=0 && tableElt(map,ind)->key!=key )
        ind=tableElt(map,ind)->next;
    return ind;
}

/*
 * Return the hash entry corresponding to key,
 *        NULL if key is not in the map
 */
hash_entry Moca_EntryFromKey(hash_map map, void *key)
{
    unsigned long h;
    int ind=0;
    if(!map)
        return NULL;
    h=hash_ptr(key, map->hash_bits);
    ind=map->hashs[h];
    while(ind>=0 && tableElt(map,ind)->key!=key )
        ind=tableElt(map,ind)->next;
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
hash_entry Moca_AddToMap(hash_map map, void *key, int *status)
{
    unsigned long h;
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
    MOCA_DEBUG_PRINT("Moca inserting %p ind %d/%lu\n",
            key,nextPos,map->tableSize);
    if((unsigned)nextPos >= map->tableSize)
    {
        *status=MOCA_HASHMAP_ERROR;
        Moca_Panic("Moca hashmap BUG in AddToMap");
        return NULL;
    }
    //Update the link
    h=hash_ptr(key, map->hash_bits);
    ind=map->hashs[h];
    if(ind<0)
    {
        tableElt(map,nextPos)->key=key;
        tableElt(map,nextPos)->next=MOCA_HASHMAP_END;
        map->hashs[h]=nextPos;
    }
    else
    {
        while(tableElt(map,ind)->key!=key && tableElt(map,ind)->next>=0)
            ind=tableElt(map,ind)->next;
        if(tableElt(map,ind)->key==key)
        {
            MOCA_DEBUG_PRINT("Moca %p already in map %p\n", key, map);
            *status=MOCA_HASHMAP_ALREADY_IN_MAP;
            tableElt(map,nextPos)->key=NULL;
            return tableElt(map,ind);
        }
        MOCA_DEBUG_PRINT("Moca collision in map %p key %p\n", key, map);
        tableElt(map,nextPos)->key=key;
        tableElt(map,nextPos)->next=MOCA_HASHMAP_END;
        tableElt(map,ind)->next=nextPos;
    }
    ++map->nbentry;
    MOCA_DEBUG_PRINT("Moca Inserted %p in map %p\n", key, map);
    *status=nextPos;
    return tableElt(map,nextPos);
}

/*
 * Returns the hash entry at position pos
 *         Null is pos is invalid or there is no entry at this position
 */
hash_entry Moca_EntryAtPos(hash_map map, unsigned int pos)
{
    if(!map || pos >= map->tableSize ||
            tableElt(map,pos)->next==MOCA_HASHMAP_UNUSED)
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
hash_entry Moca_NextEntryPos(hash_map map, unsigned int *pos)
{
    unsigned int i=*pos;
    MOCA_DEBUG_PRINT("Moca Searching element in map %p after %d /%d\n",
            map, i, Moca_NbElementInMap(map));
    while(i< map->tableSize && tableElt(map,i)->next==MOCA_HASHMAP_UNUSED)
        ++i;
    *pos=i+1;
    MOCA_DEBUG_PRINT("Moca found  %p at %d\n",
            i>=map->tableSize?NULL:tableElt(map,i),i);
    if(i >=map->tableSize)
        return NULL;
    return tableElt(map,i);
}


hash_entry Moca_RemoveFromMap(hash_map map,void *key)
{
    unsigned long h;
    int ind, ind_prev=MOCA_HASHMAP_END;
    if(!map)
        return NULL;
    MOCA_DEBUG_PRINT("Moca removing %p from %p\n", key, map);
    h=hash_ptr(key, map->hash_bits);
    ind=map->hashs[h];
    while(ind>=0 && tableElt(map,ind)->key!=key )
    {
        ind_prev=ind;
        ind=tableElt(map,ind)->next;
    }
    MOCA_DEBUG_PRINT("Moca removing %p from %p ind %d prev %d\n", key, map,
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
    tableElt(map,ind)->next=MOCA_HASHMAP_UNUSED;
    --map->nbentry;
    MOCA_DEBUG_PRINT("Moca removing %p from %p ind %d ok\n", key, map, ind);
    return tableElt(map,ind);
}


// Clear map, after this call, map is still usable
void Moca_ClearMap(hash_map map)
{
    unsigned int i;
    unsigned long h;
    if(!map)
        return;
    for(i=0;i<map->nbentry;++i)
    {
        h=hash_ptr(tableElt(map,i)->key, map->hash_bits);
        map->hashs[h]=MOCA_HASHMAP_END;
        tableElt(map,i)->next=MOCA_HASHMAP_UNUSED;
    }
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