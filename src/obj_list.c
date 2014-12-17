/*
 * Copyright (c) 2014, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj_list.c -- implementation of a simple doubly linked list
 */

#include <stdio.h>
#include <libpmem.h>
#include "obj_list.h"

/*
 * pmemobj_list_init_head -- init the head of doubly linked list
 */
int
pmemobj_list_init_head(PMEMobjpool *pool, PMEMLIST head)
{
	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin(pool, env);

	PMEMlist *dhead = pmemobj_direct(head);
	PMEMOBJ_SET(dhead->next, head);
	PMEMOBJ_SET(dhead->prev, head);

	pmemobj_tx_commit();

	return 0;
}

/*
 * __pmemobj_list_add -- (internal) insert a new item between two known
 *                       consecutive items
 */
static int
__pmemobj_list_add(PMEMobjpool *pool, PMEMLIST new,
			PMEMLIST prev, PMEMLIST next)
{
	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin(pool, env);

	PMEMlist *dnew  = pmemobj_direct(new);
	PMEMlist *dprev = pmemobj_direct(prev);
	PMEMlist *dnext = pmemobj_direct(next);

	PMEMOBJ_SET(dnext->prev, new);
	PMEMOBJ_SET(dnew->next,  next);
	PMEMOBJ_SET(dnew->prev,  prev);
	PMEMOBJ_SET(dprev->next, new);

	pmemobj_tx_commit();

	return 0;
}

/*
 * pmemobj_list_add -- insert a new item after the specified head
 */
int
pmemobj_list_add(PMEMobjpool *pool, PMEMLIST new, PMEMLIST head)
{
	PMEMlist *dhead  = pmemobj_direct(head);
	return __pmemobj_list_add(pool, new, head, dhead->next);
}

/*
 * pmemobj_list_add_tail -- insert a new item before the specified head
 */
int
pmemobj_list_add_tail(PMEMobjpool *pool, PMEMLIST new, PMEMLIST head)
{
	PMEMlist *dhead  = pmemobj_direct(head);
	return __pmemobj_list_add(pool, new, dhead->prev, head);
}

/*
 * __pmemobj_list_del -- (internal) delete a list item by making the prev/next
 *                       items point to each other
 */
static int
__pmemobj_list_del(PMEMobjpool *pool, PMEMLIST prev, PMEMLIST next)
{
	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin(pool, env);

	PMEMlist *dprev = pmemobj_direct(prev);
	PMEMlist *dnext = pmemobj_direct(next);

	PMEMOBJ_SET(dnext->prev, prev);
	PMEMOBJ_SET(dprev->next, next);

	pmemobj_tx_commit();

	return 0;
}

/*
 * pmemobj_list_del -- deletes an item from list
 */
int
pmemobj_list_del(PMEMobjpool *pool, PMEMLIST item)
{
	PMEMlist *ditem  = pmemobj_direct(item);
	return __pmemobj_list_del(pool, ditem->prev, ditem->next);
}

/*
 * pmemobj_list_replace -- replace an old item by a new one
 */
int
pmemobj_list_replace(PMEMobjpool *pool, PMEMLIST old, PMEMLIST new)
{
	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin(pool, env);

	PMEMlist *dold = pmemobj_direct(old);
	PMEMlist *dold_next = pmemobj_direct(dold->next);
	PMEMlist *dold_prev = pmemobj_direct(dold->prev);
	PMEMlist *dnew = pmemobj_direct(new);

	PMEMOBJ_SET(dnew->next, dold->next);
	PMEMOBJ_SET(dold_next->prev, new);
	PMEMOBJ_SET(dnew->prev, dold->prev);
	PMEMOBJ_SET(dold_prev->next, new);

	pmemobj_tx_commit();

	return 0;
}

/*
 * pmemobj_list_is_last -- tests if an item is the last item in list
 */
int
pmemobj_list_is_last(PMEMLIST item, PMEMLIST head)
{
	PMEMlist *ditem = pmemobj_direct(item);
	return pmemobj_oids_equal(ditem->next, head);
}

/*
 * pmemobj_list_empty -- tests if a list is empty
 */
int
pmemobj_list_empty(PMEMLIST head)
{
	PMEMlist *dhead = pmemobj_direct(head);
	return 	pmemobj_oids_equal(dhead->next, head);
}
