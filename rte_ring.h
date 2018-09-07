/* SPDX-License-Identifier: BSD-3-Clause
*
* Copyright (c) 2010-2017 Intel Corporation
* Copyright (c) 2007-2009 Kip Macy kmacy@freebsd.org
* All rights reserved.
* Derived from FreeBSD's bufring.h
* Used as BSD-3 Licensed with permission from Kip Macy.
*/

#ifndef _RTE_RING_H_
#define _RTE_RING_H_

/**
* @file
* RTE Ring
*
* The Ring Manager is a fixed-size queue, implemented as a table of
* pointers. Head and tail pointers are modified atomically, allowing
* concurrent access to it. It has the following features:
*
* - FIFO (First In First Out)
* - Maximum size is fixed; the pointers are stored in a table.
* - Lockless implementation.
* - Multi- or single-consumer dequeue.
* - Multi- or single-producer enqueue.
* - Bulk dequeue.
* - Bulk enqueue.
*
* Note: the ring implementation is not preemptible. Refer to Programmer's
* guide/Environment Abstraction Layer/Multiple pthread/Known Issues/rte_ring
* for more information.
*
*/

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <emmintrin.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/cdefs.h>

#define RTE_CACHE_LINE_SIZE 64
#define __rte_cache_aligned __attribute__((__aligned__(RTE_CACHE_LINE_SIZE)))
#define __rte_always_inline inline __attribute__((always_inline))
#define __rte_noinline  __attribute__((noinline))
#define offsetof(t, m) ((size_t) &((t *)0)->m)
#define likely(cond)  __glibc_likely(cond)
#define unlikely(cond)  __glibc_unlikely(cond)

	enum rte_ring_queue_behavior {
		RTE_RING_QUEUE_FIXED = 0, /* Enq/Deq a fixed number of items from a ring */
		RTE_RING_QUEUE_VARIABLE   /* Enq/Deq as many items as possible from ring */
	};

	/**
	 * An RTE ring structure.
	 *
	 * The producer and the consumer have a head and a tail index. The particularity
	 * of these index is that they are not between 0 and size(ring). These indexes
	 * are between 0 and 2^32, and we mask their value when we access the ring[]
	 * field. Thanks to this assumption, we can do subtractions between 2 index
	 * values in a modulo-32bit base: that's why the overflow of the indexes is not
	 * a problem.
	 */
	struct rte_ring {
		/*
		 * Note: this field kept the RTE_MEMZONE_NAMESIZE size due to ABI
		 * compatibility requirements, it could be changed to RTE_RING_NAMESIZE
		 * next time the ABI changes
		 */
		uint32_t size;           /**< Size of ring. */
		uint32_t mask;           /**< Mask (size-1) of ring. */
		uint32_t capacity;       /**< Usable size of ring */
		uint32_t elemlen;

		/** Ring producer status. */
		volatile uint32_t* prod_head;
		volatile uint32_t* prod_tail;
		volatile uint32_t* cons_head;
		volatile uint32_t* cons_tail;

		void* data;
	}__rte_cache_aligned;


	static __rte_always_inline void
		update_tail(uint32_t* head, uint32_t* tail, uint32_t old_val, uint32_t new_val)
	{
		/*
		* If there are other enqueues/dequeues in progress that preceded us,
		* we need to wait for them to complete
		*/
		//if (!single)
		while (unlikely(*tail != old_val))
			_mm_pause();

		__atomic_store_n(tail, new_val, __ATOMIC_RELEASE);
	}

	/**
	* @internal This function updates the producer head for enqueue
	*
	* @param r
	*   A pointer to the ring structure
	* @param is_sp
	*   Indicates whether multi-producer path is needed or not
	* @param n
	*   The number of elements we will want to enqueue, i.e. how far should the
	*   head be moved
	* @param behavior
	*   RTE_RING_QUEUE_FIXED:    Enqueue a fixed number of items from a ring
	*   RTE_RING_QUEUE_VARIABLE: Enqueue as many items as possible from ring
	* @param old_head
	*   Returns head value as it was before the move, i.e. where enqueue starts
	* @param new_head
	*   Returns the current/new head value i.e. where enqueue finishes
	* @param free_entries
	*   Returns the amount of free space in the ring BEFORE head was moved
	* @return
	*   Actual number of objects enqueued.
	*   If behavior == RTE_RING_QUEUE_FIXED, this will be 0 or n only.
	*/
	static __rte_always_inline unsigned int
		__rte_ring_move_prod_head(struct rte_ring *r,
			unsigned int n, enum rte_ring_queue_behavior behavior,
			uint32_t *old_head, uint32_t *new_head)
	{
		//prod_head = old_head, prod_next=new_head
		const uint32_t size = r->size;
		const uint32_t capacity = r->capacity;
		unsigned int max = n;
		int success;
		int free_entries = 0;

		do {
			/* Reset n to the initial burst count */
			n = max;

			*old_head = __atomic_load_n(r->prod_head,
				__ATOMIC_ACQUIRE);

			/*
			*  The subtraction is done between two unsigned 32bits value
			* (the result is always modulo 32 bits even if we have
			* *old_head > cons_tail). So 'free_entries' is always between 0
			* and capacity (which is < size).
			*/
			//printf("free_entries:%d %d %d\n",free_entries,capacity + *r->cons_tail - *old_head,size);
			free_entries = (capacity + *r->cons_tail - *old_head) % size;

			/* check that we have enough room in ring */
			if (unlikely(n > free_entries))
				n = (behavior == RTE_RING_QUEUE_FIXED) ?
				0 : free_entries;

			if (n == 0)
				return 0;

			*new_head = (*old_head + n) % size;
			//if (is_sp)
			//	r->prod.head = *new_head, success = 1;
			//else
			//multiproduction cann't make sure success by one step
			success = __atomic_compare_exchange_n(r->prod_head,
				old_head, *new_head,
				0, __ATOMIC_ACQUIRE,
				__ATOMIC_RELAXED);
		} while (unlikely(success == 0));
		return n;
	}

	/**
	* @internal This function updates the consumer head for dequeue
	*
	* @param r
	*   A pointer to the ring structure
	* @param is_sc
	*   Indicates whether multi-consumer path is needed or not
	* @param n
	*   The number of elements we will want to enqueue, i.e. how far should the
	*   head be moved
	* @param behavior
	*   RTE_RING_QUEUE_FIXED:    Dequeue a fixed number of items from a ring
	*   RTE_RING_QUEUE_VARIABLE: Dequeue as many items as possible from ring
	* @param old_head
	*   Returns head value as it was before the move, i.e. where dequeue starts
	* @param new_head
	*   Returns the current/new head value i.e. where dequeue finishes
	* @param entries
	*   Returns the number of entries in the ring BEFORE head was moved
	* @return
	*   - Actual number of objects dequeued.
	*     If behavior == RTE_RING_QUEUE_FIXED, this will be 0 or n only.
	*/
	static __rte_always_inline unsigned int
		__rte_ring_move_cons_head(struct rte_ring *r,
			unsigned int n, enum rte_ring_queue_behavior behavior,
			uint32_t *old_head, uint32_t *new_head,
			uint32_t *entries)
	{
		const uint32_t size = r->size;
		const uint32_t capacity = r->capacity;
		unsigned int max = n;
		int success;

		/* move cons.head atomically */
		do {
			/* Restore n as it may change every loop */
			n = max;
			*old_head = __atomic_load_n(r->cons_head,
				__ATOMIC_ACQUIRE);

			/* The subtraction is done between two unsigned 32bits value
			* (the result is always modulo 32 bits even if we have
			* cons_head > prod_tail). So 'entries' is always between 0
			* and size(ring)-1.
			*/
			*entries = (*r->prod_tail - *old_head + size) % size;

			/* Set the actual entries for dequeue */
			if (n > *entries)
				n = (behavior == RTE_RING_QUEUE_FIXED) ? 0 : *entries;

			if (unlikely(n == 0))
				return 0;

			*new_head = (*old_head + n) % size;
			//if (is_sc)
			//	*r->cons.head = *new_head, success = 1;
			//else
			success = __atomic_compare_exchange_n(r->cons_head,
				old_head, *new_head,
				0, __ATOMIC_ACQUIRE,
				__ATOMIC_RELAXED);
		} while (unlikely(success == 0));
		return n;
	}

	/**
	 * @internal Enqueue several objects on the ring
	 *
	  * @param r
	 *   A pointer to the ring structure.
	 * @param obj_table
	 *   A pointer to a table of void * pointers (objects).
	 * @param n
	 *   The number of objects to add in the ring from the obj_table.
	 * @param behavior
	 *   RTE_RING_QUEUE_FIXED:    Enqueue a fixed number of items from a ring
	 *   RTE_RING_QUEUE_VARIABLE: Enqueue as many items as possible from ring
	 * @param is_sp
	 *   Indicates whether to use single producer or multi-producer head update
	 * @param free_space
	 *   returns the amount of space after the enqueue operation has finished
	 * @return
	 *   Actual number of objects enqueued.
	 *   If behavior == RTE_RING_QUEUE_FIXED, this will be 0 or n only.
	 */
	static __rte_always_inline unsigned int
		__rte_ring_do_enqueue(struct rte_ring *r, void * obj_table,
			unsigned int n, enum rte_ring_queue_behavior behavior)
	{
		const uint32_t size = r->size;
		int i = 0;
		uint32_t prod_head, prod_next;

		n = __rte_ring_move_prod_head(r, n, behavior,
			&prod_head, &prod_next);
		if (n == 0)
			return n;

		for (i = 0; i < n; i++)
		{
			printf("enqueue index:%d %d %d %d\n", (prod_head + i) % size, prod_head, i, size);
			memcpy(r->data + ((prod_head + i) % size) * r->elemlen, obj_table + i*r->elemlen, r->elemlen);
		}
		//ENQUEUE_PTRS(r, &r[1], prod_head, obj_table, n, void *);//memcpy data to prod_head

		update_tail(r->prod_head, r->prod_tail, prod_head, prod_next);
		return n;
	}

	/**
	 * @internal Dequeue several objects from the ring
	 *
	 * @param r
	 *   A pointer to the ring structure.
	 * @param obj_table
	 *   A pointer to a table of void * pointers (objects).
	 * @param n
	 *   The number of objects to pull from the ring.
	 * @param behavior
	 *   RTE_RING_QUEUE_FIXED:    Dequeue a fixed number of items from a ring
	 *   RTE_RING_QUEUE_VARIABLE: Dequeue as many items as possible from ring
	 * @param is_sc
	 *   Indicates whether to use single consumer or multi-consumer head update
	 * @param available
	 *   returns the number of remaining ring entries after the dequeue has finished
	 * @return
	 *   - Actual number of objects dequeued.
	 *     If behavior == RTE_RING_QUEUE_FIXED, this will be 0 or n only.
	 */
	static __rte_always_inline unsigned int
		__rte_ring_do_dequeue(struct rte_ring *r, void *obj_table,
			unsigned int n, enum rte_ring_queue_behavior behavior)
	{
		const uint32_t size = r->size;
		uint32_t cons_head, cons_next;
		uint32_t entries;
		int i = 0;

		n = __rte_ring_move_cons_head(r, n, behavior,
			&cons_head, &cons_next, &entries);
		if (n == 0)
			return n;
		for (i = 0; i < n; i++)
		{
			memcpy(obj_table + i*r->elemlen, r->data + ((cons_head + i) % size)* r->elemlen, r->elemlen);
		}

		update_tail(r->cons_head, r->cons_tail, cons_head, cons_next);
		return n;
	}

	/**
	 * Enqueue several objects on the ring (multi-producers safe).
	 *
	 * This function uses a "compare and set" instruction to move the
	 * producer index atomically.
	 *
	 * @param r
	 *   A pointer to the ring structure.
	 * @param obj_table
	 *   A pointer to a table of void * pointers (objects).
	 * @param n
	 *   The number of objects to add in the ring from the obj_table.
	 * @param free_space
	 *   if non-NULL, returns the amount of space in the ring after the
	 *   enqueue operation has finished.
	 * @return
	 *   The number of objects enqueued, either 0 or n
	 */
	static __rte_always_inline unsigned int
		rte_ring_enqueue_bulk(struct rte_ring *r, void * obj_table,
			unsigned int n)
	{
		return __rte_ring_do_enqueue(r, obj_table, n, RTE_RING_QUEUE_VARIABLE);
	}

	/**
	 * Enqueue one object on a ring (multi-producers safe).
	 *
	 * This function uses a "compare and set" instruction to move the
	 * producer index atomically.
	 *
	 * @param r
	 *   A pointer to the ring structure.
	 * @param obj
	 *   A pointer to the object to be added.
	 * @return
	 *   - 0: Success; objects enqueued.
	 _*   - -ENOBUFS: Not enough room in the ring to enqueue; no object is enqueued.
	 */
	static __rte_always_inline int
		rte_ring_enqueue(struct rte_ring *r, void *obj)
	{
		return rte_ring_enqueue_bulk(r, obj, 1);
	}

	/**
	 * Dequeue several objects from a ring (multi-consumers safe).
	 *
	 * This function uses a "compare and set" instruction to move the
	 * consumer index atomically.
	 *
	 * @param r
	 *   A pointer to the ring structure.
	 * @param obj_table
	 *   A pointer to a table of void * pointers (objects) that will be filled.
	 * @param n
	 *   The number of objects to dequeue from the ring to the obj_table.
	 * @param available
	 *   If non-NULL, returns the number of remaining ring entries after the
	 *   dequeue has finished.
	 * @return
	 *   The number of objects dequeued, either 0 or n
	 */
	static __rte_always_inline unsigned int
		rte_ring_dequeue_bulk(struct rte_ring *r, void *obj_table,
			unsigned int n)
	{
		return __rte_ring_do_dequeue(r, obj_table, n, RTE_RING_QUEUE_FIXED);
	}

	/**
	 * Dequeue one object from a ring (multi-consumers safe).
	 *
	 * This function uses a "compare and set" instruction to move the
	 * consumer index atomically.
	 *
	 * @param r
	 *   A pointer to the ring structure.
	 * @param obj_p
	 *   A pointer to a void * pointer (object) that will be filled.
	 * @return
	 *   - 0: Success; objects dequeued.
	 *   - -ENOENT: Not enough entries in the ring to dequeue; no object is
	 *     dequeued.
	 */
	static __rte_always_inline int
		rte_ring_dequeue(struct rte_ring *r, void *obj_p)
	{
		return rte_ring_dequeue_bulk(r, obj_p, 1);
	}

	/**
	* @param r
	*  A pointer to the ring structure.
	* @param p
	*  A pointer to a shared memory
	* @param totalen
	*  size of the shared memory
	* @param elemlen
	*  size of the element
	*/
	struct rte_ring *
		rte_ring_create(void* p, int totallen, int elemlen)
	{
		struct rte_ring* r = malloc(sizeof(struct rte_ring));
		r->size = (totallen - 512) / elemlen;//64*4 global cons_head,cons_tail,prod_head,prod_tail;
		r->mask = r->size - 1;
		r->capacity = r->size - 1;
		r->elemlen = elemlen;

		r->prod_head = p;
		r->prod_tail = p + 64;
		r->cons_head = p + 128;
		r->cons_tail = p + 192;
		r->data = p + 512;

		memset(p, 0, totallen);
		return r;
	}

	/**
	* De-allocate all memory used by the ring.
	*
	* @param r
	*   Ring to free
	*/
	void
		rte_ring_free(struct rte_ring *r)
	{
		free(r);
		return;
	}

	void
		ring_info(struct rte_ring *r)
	{
		printf("ring size:%d\n", r->size);
		printf("ring usage:%d\n", (*r->prod_head - *r->cons_head + r->size) % r->size);
		printf("prod_head:%d, prod_tail:%d, cons_head:%d, cons_tail:%d\n", *r->prod_head, *r->prod_tail, *r->cons_head, *r->cons_tail);
	}

#ifdef __cplusplus
}
#endif

#endif /* _RTE_RING_H_ */
