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
 * obj.c -- transactional object store implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <time.h>
#include <uuid/uuid.h>
#include <endian.h>
#include <pthread.h>
#include <libpmem.h>
#include "pmem.h"
#include "util.h"
#include "out.h"
#include "allocator.h"
#include "obj.h"


static uint64_t Runid;		/* unique "run ID" for this program run */

typedef enum {
	TXOP_ALLOC,
	TXOP_FREE,
	TXOP_SET,
} op_t;

struct txop {
	op_t op;
	void *next;
	void *prev;
	union txop_args {
		struct {
			uint64_t addr;
		} alloc;
		struct {
			uint64_t addr;
		} free;
		struct {
			void *addr;
			uint64_t data;
			size_t len;
		} set;
	} args;
};

struct tx {
	int valid_env;
	jmp_buf env;
	PMEMmutex *mutexp;
	PMEMrwlock *rwlockp;
	PMEMobjpool *pool;

	struct tx *nexttx;	/* outer transaction when nested */
	/* one of these is pushed for each operation in a transaction */
	struct txop *head;
	struct txop *tail;
};

typedef void (*pmemobj_txop_onaction_t)(struct tx *txp, union txop_args args);

static __thread struct txinfo {
	struct tx *txp;
} *Curthread_txinfop;		/* current transaction for this thread */

/*
 * obj_init -- load-time initialization for obj
 *
 * Called automatically by the run-time loader.
 */
__attribute__((constructor))
static void
obj_init(void)
{
	out_init(LOG_PREFIX, LOG_LEVEL_VAR, LOG_FILE_VAR);
	LOG(3, NULL);
	util_init();

	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		LOG(1, "!clock_gettime");
		srandom((unsigned)time(0));
		Runid = (uint64_t)random();
	} else {
		Runid = ts.tv_sec * 1000000000 + ts.tv_nsec;
	}
	LOG(4, "Runid %" PRIx64, Runid);
}

/*
 * pmemobj_pool_open -- open a transactional memory pool
 */
PMEMobjpool *
pmemobj_pool_open(const char *path)
{
	LOG(3, "path \"%s\"", path);

	struct stat stbuf;
	if (stat(path, &stbuf) < 0) {
		LOG(1, "!%s", path);
		return NULL;
	}

	if (stbuf.st_size < PMEMOBJ_MIN_POOL) {
		LOG(1, "size %zu smaller than %zu",
				stbuf.st_size, PMEMOBJ_MIN_POOL);
		errno = EINVAL;
		return NULL;
	}

	int fd;
	if ((fd = open(path, O_RDWR)) < 0) {
		LOG(1, "!%s", path);
		return NULL;
	}

	void *addr;
	if ((addr = util_map(fd, stbuf.st_size, 0)) == NULL)
		return NULL;	/* util_map() set errno, called LOG */

	close(fd);

	/* check if the mapped region is located in persistent memory */
	int is_pmem = pmem_is_pmem(addr, stbuf.st_size);

	/* opaque info lives at the beginning of mapped memory pool */
	struct pmemobjpool *pop = addr;

	struct pool_hdr hdr;
	memcpy(&hdr, &pop->hdr, sizeof (hdr));

	if (util_convert_hdr(&hdr)) {
		/*
		 * valid header found
		 */
		if (strncmp(hdr.signature, OBJ_HDR_SIG, POOL_HDR_SIG_LEN)) {
			LOG(1, "wrong pool type: \"%s\"", hdr.signature);

			errno = EINVAL;
			goto err;
		}

		if (hdr.major != OBJ_FORMAT_MAJOR) {
			LOG(1, "obj pool version %d (library expects %d)",
				hdr.major, OBJ_FORMAT_MAJOR);

			errno = EINVAL;
			goto err;
		}

		int retval = util_feature_check(&hdr, OBJ_FORMAT_INCOMPAT,
							OBJ_FORMAT_RO_COMPAT,
							OBJ_FORMAT_COMPAT);
		if (retval < 0)
		    goto err;
		else if (retval == 0) {
			/* XXX switch to read-only mode */
		}
	} else {
		/*
		 * no valid header was found
		 */
		LOG(3, "creating new obj memory pool");

		struct pool_hdr *hdrp = &pop->hdr;

		memset(hdrp, '\0', sizeof (*hdrp));
		strncpy(hdrp->signature, OBJ_HDR_SIG, POOL_HDR_SIG_LEN);
		hdrp->major = htole32(OBJ_FORMAT_MAJOR);
		hdrp->compat_features = htole32(OBJ_FORMAT_COMPAT);
		hdrp->incompat_features = htole32(OBJ_FORMAT_INCOMPAT);
		hdrp->ro_compat_features = htole32(OBJ_FORMAT_RO_COMPAT);
		uuid_generate(hdrp->uuid);
		hdrp->crtime = htole64((uint64_t)time(NULL));
		util_checksum(hdrp, sizeof (*hdrp), &hdrp->checksum, 1);
		hdrp->checksum = htole64(hdrp->checksum);

		/* store pool's header */
		libpmem_persist(is_pmem, hdrp, sizeof (*hdrp));

		/* initialize pool metadata */
		memset(&pop->rootlock, '\0', sizeof (pop->rootlock));
		pop->root.off = 0;
	}

	/* use some of the memory pool area for run-time info */
	pop->addr = addr;
	pop->size = stbuf.st_size;

	allocator_init(&pop->allocator, sizeof (struct pmemobjpool), is_pmem);

	/*
	 * If possible, turn off all permissions on the pool header page.
	 *
	 * The prototype PMFS doesn't allow this when large pages are in
	 * use not it is not considered an error if this fails.
	 */
	util_range_none(addr, sizeof (struct pool_hdr));

	/* the rest should be kept read-only for debug version */
	RANGE_RW(addr + sizeof (struct pool_hdr),
			stbuf.st_size - sizeof (struct pool_hdr));

	LOG(3, "pop %p", pop);
	return pop;

err:
	LOG(4, "error clean up");
	int oerrno = errno;
	util_unmap(addr, stbuf.st_size);
	errno = oerrno;
	return NULL;
}

/*
 * pmemobj_pool_open_mirrored -- open a mirrored pool
 */
PMEMobjpool *
pmemobj_pool_open_mirrored(const char *path1, const char *path2)
{
	return NULL;
}

/*
 * pmemobj_pool_close -- close a transactional memory pool
 */
void
pmemobj_pool_close(PMEMobjpool *pop)
{
	LOG(3, "pop %p", pop);

	util_unmap(pop->addr, pop->size);
}

/*
 * pmemobj_pool_check -- transactional memory pool consistency check
 */
int
pmemobj_pool_check(const char *path)
{
	LOG(3, "path \"%s\"", path);

	/* XXX stub */
	return 0;
}

/*
 * pmemobj_pool_check_mirrored -- mirrored memory pool consistency check
 */
int
pmemobj_pool_check_mirrored(const char *path1, const char *path2)
{
	LOG(3, "path1 \"%s\", path2 \"%s\"", path1, path2);

	/* XXX stub */
	return 0;
}

/*
 * tx_error -- (internal) set error state
 *
 * This is typically called for pmemobj error cases like this:
 *	return tx_error(tid, errnum);
 * A tid of zero is passed to mean the current tid for this thread.
 *
 * If there's no transaction in progress or if the transaction does
 * not have a jmp_buf defined, tx_error sets errno and returns -1.
 * If there's a jmp_buf defined, tx_error sets errno and longjmps.
 */
static int
tx_error(int tid, int errnum)
{
	errno = errnum;
	/* XXX if jmp_buf defined, longjmp */
	return -1;
}

/*
 * mutexof -- (internal) find or allocate a pthread_mutex_t
 *
 * The first time mutexof() is used on a zeroed PMEMmutex, or
 * the first time it is used during this run of the  program,
 * mutexof() allocates a new pthread_mutex_t in DRAM and initializes
 * it for use.  On subsequent calls, mutexof() returns the existing
 * pthread_mutex_t.
 *
 * NULL is returned if the pthread_mutex_t cannot be allocated or initialized.
 */
pthread_mutex_t *
mutexof(PMEMmutex *mutexp)
{
	if (mutexp->runid == Runid)
		return mutexp->pthread_mutexp;	/* already allocated */
	else if ((mutexp->pthread_mutexp =
				Malloc(sizeof (pthread_mutex_t))) == NULL)
		return NULL;
	else if ((errno = pthread_mutex_init(mutexp->pthread_mutexp, NULL)))
		return NULL;
	else
		return mutexp->pthread_mutexp;	/* newly allocated */
}

/*
 * rwlockof -- (internal) find or allocate a pthread_rwlock_t
 *
 * NULL is returned if the pthread_rwlock_t cannot be allocated or initialized.
 */
pthread_rwlock_t *
rwlockof(PMEMrwlock *rwlockp)
{
	if (rwlockp->runid == Runid)
		return rwlockp->pthread_rwlockp;	/* already allocated */
	else if ((rwlockp->pthread_rwlockp =
				Malloc(sizeof (pthread_rwlock_t))) == NULL)
		return NULL;
	else if ((errno = pthread_rwlock_init(rwlockp->pthread_rwlockp, NULL)))
		return NULL;
	else
		return rwlockp->pthread_rwlockp;	/* newly allocated */
}

/*
 * condof -- (internal) find or allocate a pthread_cond_t
 *
 * NULL is returned if the pthread_cond_t cannot be allocated or initialized.
 */
pthread_cond_t *
condof(PMEMcond *condp)
{
	if (condp->runid == Runid)
		return condp->pthread_condp;	/* already allocated */
	else if ((condp->pthread_condp =
				Malloc(sizeof (pthread_cond_t))) == NULL)
		return NULL;
	else if ((errno = pthread_cond_init(condp->pthread_condp, NULL)))
		return NULL;
	else
		return condp->pthread_condp;	/* newly allocated */
}

/*
 * pmemobj_mutex_init -- initialize a PMEMmutex
 *
 * Calling pmemobj_mutex_init() is only necessary on new allocations,
 * and even then only if the PMEMmutex has not been zeroed.  Unlike
 * pthread_mutex_t, PMEMmutexes are considered initialized when they
 * are zeroed (so PMEMmutexes allocated via pmemobj_zalloc() need not be
 * initialized, for example).  In addition, they are automatically
 * re-initialized each time the memory pool is opened (any state stored
 * in pmem for a PMEMmutex resets).
 *
 * Unlike pthread_mutex_init(), no attr argument is supported.
 */
int
pmemobj_mutex_init(PMEMmutex *mutexp)
{
	pthread_mutex_t *pthread_mutexp = mutexof(mutexp);

	if (pthread_mutexp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_mutex_init(pthread_mutexp, NULL);
}

/*
 * pmemobj_mutex_lock -- lock a PMEMmutex
 */
int
pmemobj_mutex_lock(PMEMmutex *mutexp)
{
	pthread_mutex_t *pthread_mutexp = mutexof(mutexp);

	if (pthread_mutexp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_mutex_lock(pthread_mutexp);
}

/*
 * pmemobj_mutex_trylock -- try to lock a PMEMmutex
 */
int
pmemobj_mutex_trylock(PMEMmutex *mutexp)
{
	pthread_mutex_t *pthread_mutexp = mutexof(mutexp);

	if (pthread_mutexp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_mutex_trylock(pthread_mutexp);
}

/*
 * pmemobj_mutex_unlock -- unlock a PMEMmutex
 */
int
pmemobj_mutex_unlock(PMEMmutex *mutexp)
{
	pthread_mutex_t *pthread_mutexp = mutexof(mutexp);

	if (pthread_mutexp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_mutex_unlock(pthread_mutexp);
}

/*
 * pmemobj_rwlock_init -- initialize a PMEMrwlock
 */
int
pmemobj_rwlock_init(PMEMrwlock *rwlockp)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_init(pthread_rwlockp, NULL);
}

/*
 * pmemobj_rwlock_rdlock -- read lock a PMEMrwlock
 */
int
pmemobj_rwlock_rdlock(PMEMrwlock *rwlockp)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_rdlock(pthread_rwlockp);
}

/*
 * pmemobj_rwlock_wrlock -- write lock a PMEMrwlock
 */
int
pmemobj_rwlock_wrlock(PMEMrwlock *rwlockp)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_wrlock(pthread_rwlockp);
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_timedrdlock(PMEMrwlock *restrict rwlockp,
		const struct timespec *restrict abs_timeout)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_timedrdlock(pthread_rwlockp, abs_timeout);
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_timedwrlock(PMEMrwlock *restrict rwlockp,
		const struct timespec *restrict abs_timeout)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_timedwrlock(pthread_rwlockp, abs_timeout);
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_tryrdlock(PMEMrwlock *rwlockp)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_tryrdlock(pthread_rwlockp);
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_trywrlock(PMEMrwlock *rwlockp)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_trywrlock(pthread_rwlockp);
}

/*
 * pmemobj_rwlock_unlock -- unlock a PMEMrwlock
 */
int
pmemobj_rwlock_unlock(PMEMrwlock *rwlockp)
{
	pthread_rwlock_t *pthread_rwlockp = rwlockof(rwlockp);

	if (pthread_rwlockp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_rwlock_unlock(pthread_rwlockp);
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_init(PMEMcond *condp)
{
	pthread_cond_t *pthread_condp = condof(condp);

	if (pthread_condp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_cond_init(pthread_condp, NULL);
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_broadcast(PMEMcond *condp)
{
	pthread_cond_t *pthread_condp = condof(condp);

	if (pthread_condp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_cond_broadcast(pthread_condp);
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_signal(PMEMcond *condp)
{
	pthread_cond_t *pthread_condp = condof(condp);

	if (pthread_condp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_cond_signal(pthread_condp);
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_timedwait(PMEMcond *restrict condp,
		PMEMmutex *restrict mutexp,
		const struct timespec *restrict abstime)
{
	pthread_cond_t *pthread_condp = condof(condp);
	pthread_mutex_t *pthread_mutexp = mutexof(mutexp);

	if (pthread_condp == NULL || pthread_mutexp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_cond_timedwait(pthread_condp, pthread_mutexp, abstime);
}

/*
 * pmemobj_cond_init -- initialize a PMEMcond
 */
int
pmemobj_cond_wait(PMEMcond *condp, PMEMmutex *restrict mutexp)
{
	pthread_cond_t *pthread_condp = condof(condp);
	pthread_mutex_t *pthread_mutexp = mutexof(mutexp);

	if (pthread_condp == NULL || pthread_mutexp == NULL)
		return tx_error(0, ENOMEM);

	return pthread_cond_wait(pthread_condp, pthread_mutexp);
}

/*
 * zalloc -- (internal) fake version zeroed malloc
 */
static void *
zalloc(size_t size)
{
	char *ptr = Malloc(size);
	if (ptr)
		memset(ptr, '\0', size);
	return (void *)ptr;
}

/*
 * pmemobj_root_direct -- return direct access to root object
 *
 * The root object is special.  If it doesn't exist, a pre-zeroed instance
 * is created, persisted, and then returned.  If it does exist, the
 * instance already in pmem is returned.  Creation is done atomically, so
 * two threads calling pmemobj_root_direct() concurrently will get back
 * the same pointer to the same object, even if it has to be created.  But
 * beyond that there's no protection against concurrent updates and the
 * object almost certainly needs to contain a lock to make updates to it
 * MT-safe.
 *
 * The argument "size" is used to determine the size of the root object,
 * the first time this is called, but after that the object already exists
 * and size is used to verify the caller knows the correct size.
 */
void *
pmemobj_root_direct(PMEMobjpool *pop, size_t size)
{
	pmemobj_mutex_lock(&pop->rootlock);
	if (pop->root.off == 0) {
		pop->root.pool = (uint64_t)pop->addr;
		pmalloc(&(pop->allocator), &(pop->root.off), size);
	}
	pmemobj_mutex_unlock(&pop->rootlock);
	return pmemobj_direct(pop->root);
}

/*
 * pmemobj_root_resize -- set the root object size
 *
 * This is for the (extremely rare) case where the root object needs
 * to change size.  If the object grows in size, the new portion of
 * the object is zeroed.
 */
int
pmemobj_root_resize(PMEMobjpool *pop, size_t newsize)
{
	/* XXX stub */
	errno = ENOMEM;
	return -1;
}

/*
 * pmemobj_tx_begin -- begin a transaction
 */
PMEMtid
pmemobj_tx_begin(PMEMobjpool *pop, jmp_buf env)
{
	struct tx *txp = zalloc(sizeof (*txp));
	txp->pool = pop;

	if (env) {
		txp->valid_env = 1;
		memcpy((void *)txp->env, (void *)env, sizeof (jmp_buf));
	}

	if (Curthread_txinfop == NULL) {
		struct txinfo *txinfop = zalloc(sizeof (*txinfop));
		txinfop->txp = txp;
		Curthread_txinfop = txinfop;
		txp->nexttx = NULL;
	} else {
		txp->nexttx = Curthread_txinfop->txp;
		Curthread_txinfop->txp = txp;
	}

	return (PMEMtid)txp;
}

/*
 * pmemobj_tx_begin_lock -- begin a transaction, locking a mutex
 */
PMEMtid
pmemobj_tx_begin_lock(PMEMobjpool *pop, jmp_buf env, PMEMmutex *mutexp)
{
	struct tx *txp = (struct tx *)pmemobj_tx_begin(pop, env);
	pmemobj_mutex_lock(mutexp);
	txp->mutexp = mutexp;
	return (PMEMtid)txp;
}

/*
 * pmemobj_tx_begin_wrlock -- begin a transaction, write locking an rwlock
 */
PMEMtid
pmemobj_tx_begin_wrlock(PMEMobjpool *pop, jmp_buf env, PMEMrwlock *rwlockp)
{
	struct tx *txp = (struct tx *)pmemobj_tx_begin(pop, env);
	pmemobj_rwlock_wrlock(rwlockp);
	txp->rwlockp = rwlockp;
	return (PMEMtid)txp;
}

/*
 * pmemobj_tx_commit -- commit transaction, implicit tid
 */
int
pmemobj_tx_commit(void)
{
	return pmemobj_tx_commit_tid((PMEMtid)Curthread_txinfop->txp);
}

void
pmemobj_txop_oncommit_alloc(struct tx *txp, union txop_args args)
{
}

void
pmemobj_txop_oncommit_free(struct tx *txp, union txop_args args)
{
	pfree(&(txp->pool->allocator), args.free.addr);
}

void
pmemobj_txop_oncommit_set(struct tx *txp, union txop_args args)
{
	pfree(&(txp->pool->allocator), args.set.data);
}

pmemobj_txop_onaction_t oncommit_funcs[] = {
	pmemobj_txop_oncommit_alloc,
	pmemobj_txop_oncommit_free,
	pmemobj_txop_oncommit_set
};

int
pmemobj_tx_action_tid(PMEMtid tid, pmemobj_txop_onaction_t *actions)
{
	struct tx *tx = (struct tx *)tid;
	if (tx->nexttx == NULL) {
		struct txop *op = tx->tail;
		for (; op != NULL; op = op->prev) {
			actions[op->op](tx, op->args);
		}
		free(tx);
		free(Curthread_txinfop);
		Curthread_txinfop = NULL;
	} else {
		tx->head->prev = tx->nexttx->tail;
		tx->nexttx->tail->next = tx->head;
		tx->nexttx->tail = tx->tail;
		Curthread_txinfop->txp = tx->nexttx;
		free(tx);
	}

	return 0;
}

/*
 * pmemobj_tx_commit_tid -- commit transaction
 */
int
pmemobj_tx_commit_tid(PMEMtid tid)
{
	return pmemobj_tx_action_tid(tid, oncommit_funcs);
}

/*
 * pmemobj_tx_commit_multi -- commit multiple transactions
 *
 * A list of tids is provided varargs-style, terminated by 0
 */
int
pmemobj_tx_commit_multi(PMEMtid tid, ...)
{
	return 0;
}

/*
 * pmemobj_tx_commit_multiv -- commit multiple transactions, on array of tids
 *
 * A list of tids is provided as an array, terminated by a 0 entry
 */
int
pmemobj_tx_commit_multiv(PMEMtid tids[])
{
	return 0;
}

void
pmemobj_txop_onabort_alloc(struct tx *txp, union txop_args args)
{
	pfree(&(txp->pool->allocator), args.alloc.addr);
}

void
pmemobj_txop_onabort_free(struct tx *txp, union txop_args args)
{
}

void
pmemobj_txop_onabort_set(struct tx *txp, union txop_args args)
{
	uint64_t base = (uint64_t)txp->pool->addr;
	memcpy(args.set.addr, (void *)(base + args.set.data), args.set.len);
}

pmemobj_txop_onaction_t onabort_funcs[] = {
	pmemobj_txop_onabort_alloc,
	pmemobj_txop_onabort_free,
	pmemobj_txop_onabort_set
};

/*
 * pmemobj_tx_abort -- abort transaction, implicit tid
 */
int
pmemobj_tx_abort(int errnum)
{
	int status = -1;
	while (Curthread_txinfop != NULL) {
		status = pmemobj_tx_abort_tid(
			(PMEMtid)Curthread_txinfop->txp, errnum);
	}

	return status;
}

/*
 * pmemobj_tx_abort_tid -- abort transaction
 */
int
pmemobj_tx_abort_tid(PMEMtid tid, int errnum)
{
	return pmemobj_tx_action_tid(tid, onabort_funcs);
}

static struct txop *
pmemobj_log_prepare_alloc(uint64_t **addrpp)
{
	struct txop *txop = zalloc(sizeof (struct txop));
	txop->args.alloc.addr = 0;
	txop->op = TXOP_ALLOC;
	*addrpp = &(txop->args.alloc.addr);
	return txop;
}

static struct txop *
pmemobj_log_prepare_free(uint64_t addr)
{
	struct txop *txop = zalloc(sizeof (struct txop));
	txop->args.free.addr = addr;
	txop->op = TXOP_FREE;
	return txop;
}

static struct txop *
pmemobj_log_prepare_set(void *addr, uint64_t data, size_t len)
{
	struct txop *txop = zalloc(sizeof (struct txop));
	txop->args.set.addr = addr;
	txop->args.set.data = data;
	txop->args.set.len = len;
	txop->op = TXOP_SET;
	return txop;
}

static void
pmemobj_log_add(PMEMtid tid, struct txop *txop)
{
	struct tx *tx = (struct tx *)tid;
	if (tx->head == NULL) {
		tx->head = txop;
	} else {
		tx->tail->next = txop;
		txop->prev = tx->tail;
	}
	tx->tail = txop;
}

static void
pmemobj_log_add_alloc(PMEMtid tid, uint64_t **addrpp)
{
	struct txop *txop = pmemobj_log_prepare_alloc(addrpp);
	if (txop) {
		pmemobj_log_add(tid, txop);
	}
}

static void
pmemobj_log_add_free(PMEMtid tid, uint64_t addr)
{
	struct txop *txop = pmemobj_log_prepare_free(addr);
	if (txop) {
		pmemobj_log_add(tid, txop);
	}
}

static void
pmemobj_log_add_set(PMEMtid tid, void *addr, uint64_t data, size_t len)
{
	struct txop *txop = pmemobj_log_prepare_set(addr, data, len);
	if (txop) {
		pmemobj_log_add(tid, txop);
	}
}

/*
 * pmemobj_alloc -- transactional allocate, implicit tid
 */
PMEMoid
pmemobj_alloc(size_t size)
{
	return pmemobj_alloc_tid((PMEMtid)Curthread_txinfop->txp, size);
}

/*
 * pmemobj_zalloc -- transactional allocate, zeroed, implicit tid
 */
PMEMoid
pmemobj_zalloc(size_t size)
{
	return pmemobj_zalloc_tid((PMEMtid)Curthread_txinfop->txp, size);
}

/*
 * pmemobj_realloc -- transactional realloc, implicit tid
 */
PMEMoid
pmemobj_realloc(PMEMoid oid, size_t size)
{
	return pmemobj_realloc_tid((PMEMtid)Curthread_txinfop->txp, oid, size);
}

/*
 * pmemobj_aligned_alloc -- transactional alloc of aligned memory, implicit tid
 */
PMEMoid
pmemobj_aligned_alloc(size_t alignment, size_t size)
{
	return pmemobj_aligned_alloc_tid((PMEMtid)Curthread_txinfop->txp,
							alignment, size);
}

/*
 * pmemobj_strdup -- transactional strdup of non-pmem string, implicit tid
 */
PMEMoid
pmemobj_strdup(const char *s)
{
	return pmemobj_strdup_tid((PMEMtid)Curthread_txinfop->txp, s);
}

/*
 * pmemobj_free -- transactional free, implicit tid
 */
int
pmemobj_free(PMEMoid oid)
{
	return pmemobj_free_tid((PMEMtid)Curthread_txinfop->txp, oid);
}

/*
 * pmemobj_size -- return the current size of an object
 *
 * No lock or transaction is required for this call, but of course
 * if the object is not protected by some sort of locking, another
 * thread may change the size before the caller uses the return value.
 */
size_t
pmemobj_size(PMEMoid oid)
{
	return 0;
}

/*
 * pmemobj_alloc_tid -- transactional allocate
 */
PMEMoid
pmemobj_alloc_tid(PMEMtid tid, size_t size)
{
	struct tx *tx = (struct tx *)tid;
	PMEMoid n = { 0 };
	uint64_t *ptrp;

	n.pool = (uint64_t)tx->pool->addr;
	pmemobj_log_add_alloc(tid, &ptrp);
	pmalloc(&(tx->pool->allocator), ptrp, size);
	n.off = *ptrp;
	return n;
}

/*
 * pmemobj_zalloc_tid -- transactional allocate, zeroed
 */
PMEMoid
pmemobj_zalloc_tid(PMEMtid tid, size_t size)
{
	struct tx *tx = (struct tx *)tid;
	PMEMoid n = { 0 };
	uint64_t *ptrp;

	n.pool = (uint64_t)tx->pool->addr;
	pmemobj_log_add_alloc(tid, &ptrp);
	pmalloc(&(tx->pool->allocator), ptrp, size);
	n.off = *ptrp;
	memset((void *)(n.pool + n.off), 0, size);
	return n;
}

/*
 * pmemobj_realloc_tid -- transactional realloc
 */
PMEMoid
pmemobj_realloc_tid(PMEMtid tid, PMEMoid oid, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_aligned_alloc -- transactional alloc of aligned memory
 */
PMEMoid
pmemobj_aligned_alloc_tid(PMEMtid tid, size_t alignment, size_t size)
{
	PMEMoid n = { 0 };

	return n;
}

/*
 * pmemobj_strdup_tid -- transactional strdup of non-pmem string
 */
PMEMoid
pmemobj_strdup_tid(PMEMtid tid, const char *s)
{
	struct tx *tx = (struct tx *)tid;
	size_t size = strlen(s) + 1;
	PMEMoid n = { 0 };
	uint64_t *ptrp;

	n.pool = (uint64_t)tx->pool->addr;
	pmemobj_log_add_alloc(tid, &ptrp);
	pmalloc(&(tx->pool->allocator), ptrp, size);
	n.off = *ptrp;
	strncpy((char *)(n.pool + n.off), s, size);
	return n;
}

/*
 * pmemobj_free_tid -- transactional free
 */
int
pmemobj_free_tid(PMEMtid tid, PMEMoid oid)
{
	pmemobj_log_add_free(tid, oid.off);
	return 0;
}

/*
 * pmemobj_direct -- return direct access to an object
 *
 * The direct access is for fetches only, stores must be done by
 * pmemobj_memcpy() or PMEMOBJ_SET().  When debugging is enabled,
 * attempting to store to the pointer returned by this call will
 * result in a SEGV.
 */
void *
pmemobj_direct(PMEMoid oid)
{
	return (void *)(oid.pool + oid.off);
}

/*
 * pmemobj_direct_ntx -- return direct access to an object, non-transactional
 */
void *
pmemobj_direct_ntx(PMEMoid oid)
{
	return (void *)(oid.pool + oid.off);
}

/*
 * pmemobj_nulloid -- true if object ID is the NULL object
 */
int
pmemobj_nulloid(PMEMoid oid)
{
	return (oid.off == 0);
}

/*
 * pmemobj_memcpy -- change a range, making undo log entries, implicit tid
 */
int
pmemobj_memcpy(void *dstp, void *srcp, size_t size)
{
	return pmemobj_memcpy_tid((PMEMtid)Curthread_txinfop->txp, dstp,
								srcp, size);
}

/*
 * pmemobj_memcpy_tid -- change a range, making undo log entries
 */
int
pmemobj_memcpy_tid(PMEMtid tid, void *dstp, void *srcp, size_t size)
{
	struct tx *tx = (struct tx *)tid;
	uint64_t base, *oldp;

	pmemobj_log_add_alloc(tid, &oldp);
	pmalloc(&(tx->pool->allocator), oldp, size);

	base = (uint64_t)tx->pool->addr;
	memcpy((void *)(base + *oldp), dstp, size);
	pmemobj_log_add_set(tid, dstp, *oldp, size);
	memcpy(dstp, srcp, size);
	return 0;
}

void pmem_assign_void(void *lval, void *rval) {
	PMEMOBJ_SET(lval, rval);
}
