#ifndef _VECT__H
#define _VECT__H 1

#include <string.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define VECT(type) struct { int size; int used; type *elems; }
#define VNULL { 0, 0, NULL }

#define VINIT(v) do {                                                   \
        (v).size = (v).used = 0;                                        \
        (v).elems = NULL;                                               \
} while (0)

#define VELEMSIZE(v) (sizeof(v).elems[0])

#define VECT_INITIAL_SIZE 64
#define VECT_INITIAL_NELEMS(v) (MAX(1, VECT_INITIAL_SIZE / VELEMSIZE(v)))

/* Resize v to n elements */
#define VRESIZE(v, n) do {                                              \
        (v).used = (n);                                                 \
        if ((v).size < (v).used) {                                      \
		if ((v).size == 0)                                              \
                        (v).size = VECT_INITIAL_NELEMS(v);              \
                while ((v).size < (v).used)                             \
                        (v).size *= 2;                                  \
                (v).elems = realloc((v).elems,                          \
                                    (v).size * VELEMSIZE(v));           \
        }                                                               \
} while (0)

/* Resize v to n elements, freeing unused space */
#define VRESIZE_FREE(v, n) do {                                         \
    	(v).used = (v).size = (n);                                      \
        (v).elems = realloc((v).elems,                                  \
                               ((v).size                                \
                                * sizeof (v).elems[0]));                \
} while (0)

/* Add e to vector v */
#define VADD(v, e) do {                                                 \
        int __n = VLEN(v);                                              \
        if (__n == (v).size) {                                          \
                if (__n == 0) {                                         \
                        (v).size = VECT_INITIAL_NELEMS(v);              \
                        (v).elems = malloc((v).size * VELEMSIZE(v));    \
                } else {                                                \
                        do {                                            \
                                (v).size *= 2;                          \
                        } while ((v).size <= __n);                      \
                        (v).elems = realloc((v).elems,                  \
                                            (v).size * VELEMSIZE(v));   \
                }                                                       \
        }                                                               \
        (v).used = __n + 1;                                             \
        VSET(v, __n, e);                                                \
} while (0)

/* Remove i-th elem, not keeping vector order (fast) */
#define VREMOVE(v, i) do {                                              \
        int __idx = (i);                                                \
        if ((v).used-- > __idx)                                         \
                (v).elems[__idx] = (v).elems[(v).used];                 \
} while (0)

/* Remove v[i], keeping vector order (slower) */
#define VDELETE_ORDER(v, i) do {                                        \
        int __idx = (i);                                                \
                                                                        \
        if (--(v).used > __idx)                                         \
                memmove((v).elems + __idx,                              \
                        (v).elems + __idx + 1,                          \
                        ((v).used - __idx) * VELEMSIZE(v));             \
} while (0)

/* Insert e at position i of vector v, keeping vector order */
#define VINSERT(v, i, e) do {                                           \
        int __idx = (i);                                                \
        int __n = VLEN(v);                                              \
        VRESIZE(v, __n + 1);                                            \
        if (__n - __idx > 0)                                            \
                memmove((v).elems + __idx + 1,                          \
                        (v).elems + __idx,                              \
                        (__n - __idx) * VELEMSIZE(v));                  \
        VSET(v, __idx, e);                                              \
} while (0)

#define VSET(v, i, e)	((v).elems[i] = (e))
#define VGET(v, i)	((v).elems[i])

/* Remove last element and return it */
#define VPOP(v)		((v).elems[--(v).used])

/* Remove last element (no return value) */
#define VDROPLAST(v)    ((v).used--)

#define VLAST(v)        ((v).elems[(v).used - 1])
#define VSETLAST(v, e)  ((v).elems[(v).used - 1] = (e))

#define VLEN(v)		((int)(v).used)
#define VVEC(v)		((v).elems)

#define VGROW(v, n)     VRESIZE((v), VLEN(v) + (n))
#define VSHRINK(v, n)   VRESIZE((v), VLEN(v) - (n))

/* Let iter point to each of the elements in turn. */
#define VFOREACH(v, iter)                                               \
	for ((iter) = (v).elems;                                            \
	     (iter) < (v).elems + (v).used;                                 \
             ++(iter))

/* Let i iterate over all valid indices. Declares i locally.*/
#define VFORI(v, i)	for (i = 0; i < (v).used; i++)

/* Free associated storage and reduce size of v to zero */
#define VFREE(v) do {                                                   \
        free((v).elems);                                                \
	VINIT(v);                                                           \
} while (0)

/* Truncate vector to shorter size, but do not free any storage */
#define VTRUNCATE(v, n) ((v).used = (n))

/* Reduce size of v to zero, but do not free any storage */
#define VCLEAR(v) VTRUNCATE(v, 0)

/* Copy the contents of src_v to dst_v. */
#define VCOPY(dst_v, src_v) do {                                        \
        VRESIZE(dst_v, VLEN(src_v));                                    \
        memcpy((dst_v).elems,                                           \
               (src_v).elems,                                           \
               VLEN(src_v) * VELEMSIZE(src_v));                         \
} while (0)

/*
 * Queue data type
 * Can be extended to dequeue functionality with little effort
 */

#define QUEUE(type)                                                     \
struct {                                                                \
        VECT(type) v;                                                   \
        unsigned mask;          /* circular index mask */               \
        unsigned next_add;      /* where to add next element */         \
        unsigned next_remove;   /* which element to remove next */      \
}

/*
 * Invariants:
 * - VLEN(q.v) is always 0 or a power of 2
 * - q.mask == (VLEN(q.v) == 0) ? 0 : VLEN(q.v) - 1
 * - q.next_add >= q.next_remove
 * - q.next_add - q.next_remove == number of elements in queue
 * - q.next_add & q.mask is where to add next element
 * - q.next_remove & q.mask is what element to remove next
 */

#define QNULL { VNULL, 0, 0, 0 }

/* Initialise a queue */
#define QINIT(q) do {                                                   \
        VINIT((q).v);                                                   \
        (q).mask = (q).next_add = (q).next_remove = 0;                  \
} while (0)

/* Return true iff q is empty */
#define QEMPTY(q) ((q).next_add == (q).next_remove)

/* Return number of elements in queue */
#define QLEN(q) (((q).next_add - (q).next_remove) & (q).mask)

/* Add element to front of queue */
#define QADD(q, e) do {                                                   \
        unsigned __mask = (q).mask;                                       \
        if ((q).next_add - (q).next_remove == __mask) {                   \
                /* queue full - expand */                                 \
                unsigned __newsize = (__mask + 1) << 1;                   \
                (q).next_add &= __mask;                                   \
                (q).next_remove &= __mask;                                \
                VRESIZE((q).v, __newsize);                                \
                if ((q).next_add < (q).next_remove) {                     \
                        /* move lower part of queue up past upper part */ \
                        memcpy(VVEC((q).v) + (__mask + 1),                \
                               VVEC((q).v),                               \
                               (q).next_add * VELEMSIZE((q).v));          \
                        (q).next_add += __mask + 1;                       \
                }                                                         \
                (q).mask = __mask = __newsize - 1;                        \
        }                                                                 \
        VSET((q).v, (q).next_add & __mask, e);                            \
        (q).next_add++;                                                   \
} while (0)

/* Remove and return element */
#define QREMOVE(q) VGET((q).v, (q).next_remove++ & (q).mask)

/* retrieve (but do not remove) ith element of q, most recently added last */
#define QGET(q, i) VGET((q).v, ((q).next_remove + (i)) & (q).mask)

/* Free queue storage; will then be ready for re-use. */
#define QFREE(q)                                                        \
do {                                                                    \
        VFREE((q).v);                                                   \
        (q).mask = (q).next_add = (q).next_remove = 0;                  \
} while (0)

/* Clear queue, but do not free storage; will then be ready for re-use. */
#define QCLEAR(q)                                                       \
do {                                                                    \
        VTRUNCATE((q).v, 0);                                            \
        (q).mask = (q).next_add = (q).next_remove = 0;                  \
} while (0)

#endif /* _VECT__H */
