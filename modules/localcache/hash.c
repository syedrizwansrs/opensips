/*
 * $Id: hash.c $
 *
 * memory cache system module
 *
 * Copyright (C) 2009 Anca Vamanu
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * History:
 * --------
 *  2009-01-29  initial version (Anca Vamanu)
 */

#include <stdlib.h>
#include <string.h>

#include "../../dprint.h"
#include "../../timer.h"
#include "../../mem/mem.h"
#include "../../mem/shm_mem.h"
#include "localcache.h"
#include "hash.h"


void lcache_htable_remove_safe(str attr, lcache_entry_t** it);

int lcache_htable_init(int size)
{
	int i = 0, j;

	cache_htable = (lcache_t*)shm_malloc(size * sizeof(lcache_t));
	if(cache_htable == NULL)
	{
		LM_ERR("no more shared memory\n");
		return -1;
	}
	memset(cache_htable, 0, size * sizeof(lcache_t));

	for(i= 0; i< size; i++)
	{
		if(lock_init(&cache_htable[i].lock)== 0)
		{
			LM_ERR("failed to initialize lock [%d]\n", i);
			goto error;
		}
	}

	return 0;

error:
	for(j = 0; j< i; j++)
	{
		lock_destroy(&cache_htable[j].lock);
	}
	shm_free(cache_htable);
	cache_htable = NULL;
	return -1;
}

void lcache_htable_destroy(void)
{
	int i;
	lcache_entry_t* me1, *me2;

	if(cache_htable == NULL)
		return;

	for(i = 0; i< cache_htable_size; i++)
	{
		lock_destroy(&cache_htable[i].lock)
		me1 = cache_htable[i].entries;
		while(me1)
		{
			me2 = me1->next;
			shm_free(me1);
			me1 = me2;
		}
	}
	shm_free(cache_htable);
	cache_htable = NULL;
}

int lcache_htable_insert(char* attr, char* value, unsigned int expires)
{
	lcache_entry_t* me, *it;
	str attr_str = {attr, strlen(attr)};
	str value_str = {value, strlen(value)};
	int hash_code;
	int size;

	size= sizeof(lcache_entry_t) + attr_str.len + value_str.len +1;

	me = (lcache_entry_t*)shm_malloc(size);
	if(me == NULL)
	{
		LM_ERR("no more shared memory\n");
		return -1;
	}
	memset(me, 0, size);

	me->attr.s = (char*)me + (sizeof(lcache_entry_t));
	memcpy(me->attr.s, attr, attr_str.len);
	me->attr.len = attr_str.len;

	me->value.s = (char*)me + (sizeof(lcache_entry_t)) + attr_str.len;
	memcpy(me->value.s, value, value_str.len);
	me->value.s[value_str.len] = '\0';
	me->value.len = value_str.len;
	if( expires != 0)
		me->expires = get_ticks() + expires;

	hash_code= core_hash(&attr_str, 0, cache_htable_size);
	lock_get(&cache_htable[hash_code].lock);

	it = cache_htable[hash_code].entries;

	/* if a previous record for the same attr delete it */
	lcache_htable_remove_safe(attr_str, &it);

	me->next = it;
	cache_htable[hash_code].entries = me;
	

	lock_release(&cache_htable[hash_code].lock);

	return 1;
}

void lcache_htable_remove_safe(str attr, lcache_entry_t** it_p)
{
	lcache_entry_t* me = NULL, *it= *it_p;

	while(it)
	{
		if(it->attr.len == attr.len && 
				(strncmp(it->attr.s, attr.s, attr.len) == 0))
		{
			shm_free(it);
			
			if(me)
				me->next = it->next;
			else
				*it_p = NULL;
			
			return;
		}
		me = it;
		it = it->next;
	}
	LM_DBG("entry not found\n");
}

void lcache_htable_remove(char* attr)
{
	int hash_code;
	str attr_str = {attr, strlen(attr)};

	hash_code= core_hash(&attr_str, 0, cache_htable_size);
	lock_get(&cache_htable[hash_code].lock);

	lcache_htable_remove_safe(attr_str, &cache_htable[hash_code].entries);

	lock_release(&cache_htable[hash_code].lock);

}

int lcache_htable_fetch(char* attr, char** res)
{
	int hash_code;
	lcache_entry_t* it = NULL, *it_aux = NULL;
	str attr_str = {attr, strlen(attr)};
	char* value;

	hash_code= core_hash(&attr_str, 0, cache_htable_size);
	lock_get(&cache_htable[hash_code].lock);

	it = cache_htable[hash_code].entries;

	while(it)
	{
		if(it->attr.len == attr_str.len && 
				(strncmp(it->attr.s, attr_str.s, attr_str.len) == 0))
		{
			if( it->expires != 0 && it->expires < get_ticks())
			{
				/* found an expired entry  -> delete it */
				shm_free(it);
			
				if(it_aux)
					it_aux->next = it->next;
				else
					cache_htable[hash_code].entries = NULL;

				lock_release(&cache_htable[hash_code].lock);
				return -1;
			}
			value = (char*)pkg_malloc(it->value.len + 1);
			if(value == NULL)
			{
				LM_ERR("no more memory\n");
				lock_release(&cache_htable[hash_code].lock);
				return -1;
			}
			memcpy(value, it->value.s, it->value.len);
			value[it->value.len] = '\0';
			lock_release(&cache_htable[hash_code].lock);
			*res = value;
			return 1;
		}

		it_aux = it;
		it = it->next;
	}
	
	lock_release(&cache_htable[hash_code].lock);
	return -1;
}
