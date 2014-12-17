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
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * obj_dll_basic.c -- doubly linked list unit test for pmemobj
 *
 * usage: obj_dll_basic file
 */

#include "unittest.h"

const PMEMLIST NULL_PMEMLIST = { 0 };

typedef enum {
	PMEMLIST_HEAD,
	PMEMLIST_TAIL
} PMEMlist_dir;

/* struct node is the element in the doubly linked list */
struct node {
	PMEMlist linkage;
	int value;
};

/* struct base keeps track of the beginning of the list */
struct base {
	PMEMLIST head;		/* object ID of the head of the list */
	PMEMmutex mutex;	/* lock covering entire list */
};

/*
 * list_init -- init the doubly linked list
 */
int
list_init(PMEMobjpool *pool)
{
	struct base *bp = pmemobj_root_direct(pool, sizeof (*bp));

	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin_lock(pool, env, &bp->mutex);

	/* allocate the head */
	PMEMLIST head = pmemobj_alloc(sizeof (struct node));
	PMEMOBJ_SET(bp->head, head);

	/* init the head of the list */
	if (pmemobj_list_init_head(pool, bp->head))
		pmemobj_tx_abort(-1);

	pmemobj_tx_commit();

	return 0;
}

/*
 * list_insert -- allocate a new node, and insert it to the list
 */
PMEMLIST
list_insert(PMEMobjpool *pool, int val, PMEMlist_dir dir)
{
	struct base *bp = pmemobj_root_direct(pool, sizeof (*bp));

	jmp_buf env;
	if (setjmp(env))
		return NULL_PMEMLIST;

	pmemobj_tx_begin_lock(pool, env, &bp->mutex);

	/* allocate the new node to be inserted */
	PMEMLIST newoid = pmemobj_alloc(sizeof (struct node));
	struct node *newnode = pmemobj_direct(newoid);

	PMEMOBJ_SET(newnode->value, val);

	switch (dir) {
	case PMEMLIST_HEAD:
		if (pmemobj_list_add(pool, newoid, bp->head))
			pmemobj_tx_abort(-1);
		break;
	case PMEMLIST_TAIL:
		if (pmemobj_list_add_tail(pool, newoid, bp->head))
			pmemobj_tx_abort(-1);
		break;
	default:
		pmemobj_tx_abort(-1);
		break;
	}

	pmemobj_tx_commit();

	return newoid;
}

/*
 * list_free -- free whole list
 */
int
list_free(PMEMobjpool *pool)
{
	struct base *bp = pmemobj_root_direct(pool, sizeof (*bp));

	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin_lock(pool, env, &bp->mutex);

	PMEMLIST item;
	PMEMOBJ_LIST_FOREACH(item, bp->head) {
		pmemobj_free(item);
	}
	pmemobj_free(bp->head);

	pmemobj_tx_commit();

	return 0;
}

/*
 * free_item -- free one PMEMLIST item (not belonging to the list)
 */
int
free_item(PMEMobjpool *pool, PMEMLIST item)
{
	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin(pool, env);

	pmemobj_free(item);

	pmemobj_tx_commit();

	return 0;
}

/*
 * list_print -- print the entire list
 */
void
list_print(PMEMobjpool *pool)
{
	struct base *bp = pmemobj_root_direct(pool, sizeof (*bp));

	OUT("list contains:");

	pmemobj_mutex_lock(&bp->mutex);

	PMEMLIST item;
	PMEMOBJ_LIST_FOREACH(item, bp->head) {
		struct node *ditem = pmemobj_direct(item);
		OUT("    value %d", ditem->value);
	}

	pmemobj_mutex_unlock(&bp->mutex);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "obj_dll_basic");

	if (argc < 2)
		FATAL("usage: %s file", argv[0]);

	/* open the pool */
	PMEMobjpool *pool = pmemobj_pool_open(argv[1]);

	/* init the DLL list */
	list_init(pool);

	/* insert 6 elements to the head of the list */
			list_insert(pool, 1, PMEMLIST_HEAD);
			list_insert(pool, 2, PMEMLIST_HEAD);
	PMEMLIST item3 = list_insert(pool, 3, PMEMLIST_HEAD);
	PMEMLIST item4 = list_insert(pool, 4, PMEMLIST_HEAD);
			list_insert(pool, 5, PMEMLIST_HEAD);
			list_insert(pool, 6, PMEMLIST_HEAD);
	list_print(pool);

	/* delete item3 element */
	pmemobj_list_del(pool, item3);
	list_print(pool);

	/* replace item4 with item3 element */
	pmemobj_list_replace(pool, item4, item3);
	/* free item removed from the list */
	free_item(pool, item4);
	list_print(pool);

	/* insert 3 elements to the tail of the list */
	list_insert(pool, 70, PMEMLIST_TAIL);
	list_insert(pool, 80, PMEMLIST_TAIL);
	list_insert(pool, 90, PMEMLIST_TAIL);
	list_print(pool);

	/* free whole list */
	list_free(pool);

	/* close the pool */
	pmemobj_pool_close(pool);

	DONE(NULL);
}
