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

#include <sys/param.h>
#include "unittest.h"

#define	LOG_SIZE (size_t)(1024*1024)

#define	NSTRINGS 6
const char *str[NSTRINGS] = {
	"1st test string\n",
	"2nd test string\n",
	"3rd test string\n",
	"4th test string\n",
	"5th test string\n",
	"6th test string\n"
};

typedef struct log_s {
	PMEMoid	 data;
	size_t	 size;
	uint64_t offset;
	PMEMmutex mutex;
} log_t;

/*
 * objlog_create_log -- create and initialize the log
 */
int
objlog_create_log(PMEMobjpool *pool, size_t size)
{
	log_t *log = pmemobj_root_direct(pool, sizeof (*log));

	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin_lock(pool, NULL, &log->mutex);

	uint64_t offset = 0;
	PMEMoid dataoid = pmemobj_alloc(size);
	PMEMOBJ_SET(log->data, dataoid);
	PMEMOBJ_SET(log->size, size);
	PMEMOBJ_SET(log->offset, offset);

	pmemobj_tx_commit();

	return 0;
}

/*
 * objlog_append -- append 'count' bytes from 'buf' to the log
 */
int
objlog_append(PMEMobjpool *pool, const void *buf, size_t count)
{
	log_t *log = pmemobj_root_direct(pool, sizeof (*log));

	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin_lock(pool, NULL, &log->mutex);

	size_t size = log->size;
	uint64_t offset = log->offset;

	if (offset + count > size)
		return pmemobj_tx_abort(-1);

	PMEMoid dataoid = log->data;
	char *data = pmemobj_direct(dataoid);

	pmemobj_memcpy(data + offset, (void *)buf, count);
	offset += count;
	PMEMOBJ_SET(log->offset, offset);

	pmemobj_tx_commit();

	return 0;
}

/*
 * objlog_rewind -- rewind the log
 */
int
objlog_rewind(PMEMobjpool *pool)
{
	struct log_s *log = pmemobj_root_direct(pool, sizeof (*log));

	jmp_buf env;
	if (setjmp(env))
		return -1;

	pmemobj_tx_begin_lock(pool, NULL, &log->mutex);

	uint64_t offset = 0;
	PMEMOBJ_SET(log->offset, offset);

	pmemobj_tx_commit();

	return 0;
}

/*
 * objlog_nbyte -- return usable size of the log
 */
size_t
objlog_nbyte(PMEMobjpool *pool)
{
	struct log_s *log = pmemobj_root_direct(pool, sizeof (*log));

	pmemobj_mutex_lock(&log->mutex);

	size_t size = log->size;

	pmemobj_mutex_unlock(&log->mutex);

	return size;
}

/*
 * objlog_tell -- return the current write point in the log
 */
uint64_t
objlog_tell(PMEMobjpool *pool)
{
	struct log_s *log = pmemobj_root_direct(pool, sizeof (*log));

	pmemobj_mutex_lock(&log->mutex);

	uint64_t offset = log->offset;

	pmemobj_mutex_unlock(&log->mutex);

	return offset;
}

/*
 * objlog_walk -- walk through all data in the log
 */
void
objlog_walk(PMEMobjpool *pool, size_t chunksize,
		int (*process_chunk)(const void *buf, size_t len, void *arg),
		void *arg)
{
	struct log_s *log = pmemobj_root_direct(pool, sizeof (*log));

	pmemobj_mutex_lock(&log->mutex);

	PMEMoid dataoid = log->data;
	char *data = pmemobj_direct(dataoid);
	uint64_t offset = log->offset;

	size_t len;
	if (chunksize == 0) {
		len = offset;
		(*process_chunk)(data, len, arg);
	} else {
		uint64_t position = 0;
		while (position < offset) {
			len = MIN(chunksize, offset - position);
			if (!(*process_chunk)(&data[position], len, arg))
				break;
			position += chunksize;
		}
	}

	pmemobj_mutex_unlock(&log->mutex);
}

/*
 * printit -- a walker function prints 'buf' of length 'len'
 */
int
printit(const void *buf, size_t len, void *arg)
{
	char *str = alloca(len + 1);

	strncpy(str, buf, len);
	str[len] = '\0';
	OUT("%s", str);

	return 1;
}

int
main(int argc, char **argv)
{
	START(argc, argv, "obj_log_basic");

	if (argc < 2)
		FATAL("usage: %s file", argv[0]);

	PMEMobjpool *pool = pmemobj_pool_open(argv[1]);

	objlog_create_log(pool, LOG_SIZE);

	int i;
	for (i = 0; i < NSTRINGS; i++)
		objlog_append(pool, str[i], strlen(str[i]));

	OUT("Size:");
	objlog_nbyte(pool);

	OUT("Walk all:");
	objlog_walk(pool, 0, printit, NULL);

	OUT("Walk by 16:");
	objlog_walk(pool, 16, printit, NULL);

	OUT("Current write point: %lu\n", objlog_tell(pool));

	OUT("Rewind\n");
	objlog_rewind(pool);

	OUT("Current write point after rewind: %lu\n", objlog_tell(pool));

	OUT("Walk all (should be empty):");
	objlog_walk(pool, 0, printit, NULL);

	for (i = 0; i < NSTRINGS; i++) {
		objlog_rewind(pool);
		objlog_append(pool, str[i], strlen(str[i]));
	}

	OUT("Walk all (should be '6th test string'):");
	objlog_walk(pool, 0, printit, NULL);

	pmemobj_pool_close(pool);

	DONE(NULL);
}
