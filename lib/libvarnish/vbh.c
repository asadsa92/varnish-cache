/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Implementation of a binary heap API
 *
 * See also:
 *	http://dl.acm.org/citation.cfm?doid=1785414.1785434
 *	(or: http://queue.acm.org/detail.cfm?id=1814327)
 */

#include "config.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "miniobj.h"
#include "vdef.h"
#include "vas.h"
#include "vbh.h"

/* Parameters --------------------------------------------------------*/

/*
 * The number of elements in a row has to be a compromise between
 * wasted space and number of memory allocations.
 * With 64k objects per row, there will be at least 5...10 seconds
 * between row additions on a very busy server.
 * At the same time, the worst case amount of wasted memory is kept
 * at a reasonable 1 MB -- two rows on 64bit system.
 * Finally, but without practical significance: 16 bits should be
 * easier for the compiler to optimize.
 */
#define ROW_SHIFT		16


#undef PARANOIA

/* Private definitions -----------------------------------------------*/

#define ROOT_IDX		1

#define ROW_WIDTH		(1 << ROW_SHIFT)

/*lint -emacro(572, ROW) shift 0 >> by 16 */
/*lint -emacro(835, ROW) 0 left of >> */
/*lint -emacro(778, ROW) const >> evaluates to zero */
#define ROW(b, n)		((b)->array[(n) >> ROW_SHIFT])

/*lint -emacro(835, A) 0 left of & */
#define A(b, n)			ROW(b, n)[(n) & (ROW_WIDTH - 1)]

struct vbh {
	unsigned		magic;
#define VBH_MAGIC		0xf581581aU	/* from /dev/random */
	void			*priv;
	vbh_cmp_t		*cmp;
	vbh_update_t		*update;
	void			***array;
	unsigned		rows;
	unsigned		length;
	unsigned		next;
	unsigned		page_size;
	unsigned		page_mask;
	unsigned		page_shift;
};

#define VM_AWARE

#ifdef VM_AWARE

static  unsigned
parent(const struct vbh *bh, unsigned u)
{
	unsigned po;
	unsigned v;

	assert(u != UINT_MAX);
	po = u & bh->page_mask;

	if (u < bh->page_size || po > 3) {
		v = (u & ~bh->page_mask) | (po >> 1);
	} else if (po < 2) {
		v = (u - bh->page_size) >> bh->page_shift;
		v += v & ~(bh->page_mask >> 1);
		v |= bh->page_size / 2;
	} else {
		v = u - 2;
	}
	return (v);
}

static void
child(const struct vbh *bh, unsigned u, unsigned *a, unsigned *b)
{
	uintmax_t uu;

	if (u > bh->page_mask && (u & (bh->page_mask - 1)) == 0) {
		/* First two elements are magical except on the first page */
		*a = *b = u + 2;
	} else if (u & (bh->page_size >> 1)) {
		/* The bottom row is even more magical */
		*a = (u & ~bh->page_mask) >> 1;
		*a |= u & (bh->page_mask >> 1);
		*a += 1;
		uu = (uintmax_t)*a << bh->page_shift;
		*a = uu;
		if (*a == uu) {
			*b = *a + 1;
		} else {
			/*
			 * An unsigned is not big enough: clamp instead
			 * of truncating.  We do not support adding
			 * more than UINT_MAX elements anyway, so this
			 * is without consequence.
			 */
			*a = UINT_MAX;
			*b = UINT_MAX;
		}
	} else {
		/* The rest is as usual, only inside the page */
		*a = u + (u & bh->page_mask);
		*b = *a + 1;
	}
#ifdef PARANOIA
	assert(*a > 0);
	assert(*b > 0);
	if (*a != UINT_MAX) {
		assert(parent(bh, *a) == u);
		assert(parent(bh, *b) == u);
	}
#endif
}


#else

static unsigned
parent(const struct vbh *bh, unsigned u)
{

	(void)bh;
	return (u / 2);
}

static void
child(const struct vbh *bh, unsigned u, unsigned *a, unsigned *b)
{

	(void)bh;
	*a = u * 2;
	*b = *a + 1;
}

#endif

/* Implementation ----------------------------------------------------*/

static void
vbh_addrow(struct vbh *bh)
{
	unsigned u;

	/* First make sure we have space for another row */
	if (&ROW(bh, bh->length) >= bh->array + bh->rows) {
		u = bh->rows * 2;
		bh->array = realloc(bh->array, sizeof(*bh->array) * u);
		assert(bh->array != NULL);

		/* NULL out new pointers */
		while (bh->rows < u)
			bh->array[bh->rows++] = NULL;
	}
	assert(ROW(bh, bh->length) == NULL);
	ROW(bh, bh->length) = malloc(sizeof(**bh->array) * ROW_WIDTH);
	assert(ROW(bh, bh->length));
	bh->length += ROW_WIDTH;
}

struct vbh *
VBH_new(void *priv, vbh_cmp_t *cmp_f, vbh_update_t *update_f)
{
	struct vbh *bh;
	unsigned u;

	ALLOC_OBJ(bh, VBH_MAGIC);
	if (bh == NULL)
		return (bh);
	bh->priv = priv;

	bh->page_size = (unsigned)getpagesize() / sizeof (void *);
	bh->page_mask = bh->page_size - 1;
	AZ(bh->page_size & bh->page_mask);	/* power of two */
	for (u = 1; (1U << u) != bh->page_size; u++)
		;
	bh->page_shift = u;
	assert(bh->page_size <= (sizeof(**bh->array) * ROW_WIDTH));

	bh->cmp = cmp_f;
	bh->update = update_f;
	bh->next = ROOT_IDX;
#ifdef TEST_DRIVER
	bh->rows = 1;		/* A tiny-ish number */
#else
	bh->rows = 16;		/* A tiny-ish number */
#endif
	bh->array = calloc(bh->rows, sizeof *bh->array);
	assert(bh->array != NULL);
	vbh_addrow(bh);
	A(bh, ROOT_IDX) = NULL;
	bh->magic = VBH_MAGIC;
	return (bh);
}

void
VBH_destroy(struct vbh **bhp)
{
	struct vbh *bh;
	unsigned u;

	TAKE_OBJ_NOTNULL(bh, bhp, VBH_MAGIC);
	AZ(VBH_root(bh));

	for (u = 0; u < bh->length; u += ROW_WIDTH)
		free(ROW(bh, u));
	free(bh->array);
	FREE_OBJ(bh);
}

static void
vbh_update(const struct vbh *bh, unsigned u)
{
	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(u < bh->next);
	assert(A(bh, u) != NULL);
	if (bh->update != NULL)
		bh->update(bh->priv, A(bh, u), u);
}

static void
binhead_swap(const struct vbh *bh, unsigned u, unsigned v)
{
	void *p;

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(u < bh->next);
	assert(A(bh, u) != NULL);
	assert(v < bh->next);
	assert(A(bh, v) != NULL);
	p = A(bh, u);
	A(bh, u) = A(bh, v);
	A(bh, v) = p;
	vbh_update(bh, u);
	vbh_update(bh, v);
}

static unsigned
vbh_trickleup(const struct vbh *bh, unsigned u)
{
	unsigned v;

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(u < bh->next);
	assert(A(bh, u) != NULL);

	while (u > ROOT_IDX) {
		assert(u < bh->next);
		assert(A(bh, u) != NULL);
		v = parent(bh, u);
		assert(v < u);
		assert(v < bh->next);
		assert(A(bh, v) != NULL);
		if (!bh->cmp(bh->priv, A(bh, u), A(bh, v)))
			break;
		binhead_swap(bh, u, v);
		u = v;
	}
	return (u);
}

static unsigned
vbh_trickledown(const struct vbh *bh, unsigned u)
{
	unsigned v1, v2;

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(u < bh->next);
	assert(A(bh, u) != NULL);

	while (1) {
		assert(u < bh->next);
		assert(A(bh, u) != NULL);
		child(bh, u, &v1, &v2);
		assert(v1 > 0);
		assert(v2 > 0);
		assert(v1 <= v2);

		if (v1 >= bh->next)
			return (u);

		assert(A(bh, v1) != NULL);
		if (v1 != v2 && v2 < bh->next) {
			assert(A(bh, v2) != NULL);
			if (bh->cmp(bh->priv, A(bh, v2), A(bh, v1)))
				v1 = v2;
		}
		assert(v1 < bh->next);
		assert(A(bh, v1) != NULL);
		if (bh->cmp(bh->priv, A(bh, u), A(bh, v1)))
			return (u);
		binhead_swap(bh, u, v1);
		u = v1;
	}
}

void
VBH_insert(struct vbh *bh, void *p)
{
	unsigned u;

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(bh->length >= bh->next);
	if (bh->length == bh->next)
		vbh_addrow(bh);
	assert(bh->length > bh->next);
	u = bh->next++;
	A(bh, u) = p;
	vbh_update(bh, u);
	(void)vbh_trickleup(bh, u);
	assert(u < bh->next);
	assert(A(bh, u) != NULL);
}


#ifdef PARANOIA
static void
chk(const struct vbh *bh)
{
	unsigned u, v;

	for (u = 2; u < bh->next; u++) {
		v = parent(bh, u);
		AZ(bh->cmp(bh->priv, A(bh, u), A(bh, v)));
	}
}
#endif

void *
VBH_root(const struct vbh *bh)
{

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
#ifdef PARANOIA
	chk(bh);
#endif
	return (A(bh, ROOT_IDX));
}

/*
 * It may seem counter-intuitive that we delete by replacement with
 * the tail object. "That's almost certain to not belong there, in
 * particular when we delete the root ?" is the typical reaction.
 *
 * If we tried to trickle up into the empty position, we would,
 * eventually, end up with a hole in the bottom row, at which point
 * we would move the tail object there.
 * But there is no guarantee that the tail object would not need to
 * trickle up from that position, in fact, it might be the new root
 * of this half of the subtree.
 * The total number of operations is guaranteed to be at least
 * N{height} downward selections, because we have to get the hole
 * all the way down, but in addition to that, we may get up to
 * N{height}-1 upward trickles.
 *
 * When we fill the hole with the tail object, the worst case is
 * that it trickles all the way up to of this half-tree, or down
 * to become the tail object again.
 *
 * In other words worst case is N{height} up or downward trickles.
 * But there is a decent chance that it does not make it all the way.
 */

void
VBH_delete(struct vbh *bh, unsigned idx)
{

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(bh->next > ROOT_IDX);
	assert(idx < bh->next);
	assert(idx > 0);
	assert(A(bh, idx) != NULL);
	bh->update(bh->priv, A(bh, idx), VBH_NOIDX);
	if (idx == --bh->next) {
		A(bh, bh->next) = NULL;
		return;
	}
	A(bh, idx) = A(bh, bh->next);
	A(bh, bh->next) = NULL;
	vbh_update(bh, idx);
	idx = vbh_trickleup(bh, idx);
	assert(idx < bh->next);
	assert(idx > 0);
	assert(A(bh, idx) != NULL);
	idx = vbh_trickledown(bh, idx);
	assert(idx < bh->next);
	assert(idx > 0);
	assert(A(bh, idx) != NULL);

	/*
	 * We keep a hysteresis of one full row before we start to
	 * return space to the OS to avoid silly behaviour around
	 * row boundaries.
	 */
	if (bh->next + 2 * ROW_WIDTH <= bh->length) {
		free(ROW(bh, bh->length - 1));
		ROW(bh, bh->length - 1) = NULL;
		bh->length -= ROW_WIDTH;
	}
}

/*
 * Move an item up/down after changing its key value
 */

void
VBH_reorder(const struct vbh *bh, unsigned idx)
{

	CHECK_OBJ_NOTNULL(bh, VBH_MAGIC);
	assert(bh->next > ROOT_IDX);
	assert(idx < bh->next);
	assert(idx > 0);
	assert(A(bh, idx) != NULL);
	idx = vbh_trickleup(bh, idx);
	assert(idx < bh->next);
	assert(idx > 0);
	assert(A(bh, idx) != NULL);
	idx = vbh_trickledown(bh, idx);
	assert(idx < bh->next);
	assert(idx > 0);
	assert(A(bh, idx) != NULL);
}

#ifdef TEST_DRIVER

#include <stdio.h>
#include <string.h>

#include "vrnd.h"

/* Test driver -------------------------------------------------------*/

struct foo {
	unsigned	magic;
#define FOO_MAGIC	0x23239823
	unsigned	idx;
	unsigned	key;
	unsigned	n;
};

#define M 500083	/* Number of operations */
#define N 131101	/* Number of items */
#define R -1		/* Random modulus */

static struct foo *ff[N];

static int v_matchproto_(vbh_cmp_t)
cmp(void *priv, const void *a, const void *b)
{
	const struct foo *fa, *fb;

	(void)priv;
	CAST_OBJ_NOTNULL(fa, a, FOO_MAGIC);
	CAST_OBJ_NOTNULL(fb, b, FOO_MAGIC);
	return (fa->key < fb->key);
}

static void v_matchproto_(vbh_update_t)
update(void *priv, void *a, unsigned u)
{
	struct foo *fa;

	(void)priv;
	CAST_OBJ_NOTNULL(fa, a, FOO_MAGIC);
	fa->idx = u;
}

#ifdef CHECK2
static void
chk2(struct vbh *bh)
{
	unsigned u, v;
	struct foo *fa, *fb;

	for (u = 2; u < bh->next; u++) {
		v = parent(bh, u);
		fa = A(bh, u);
		fb = A(bh, v);
		assert(fa->key >= fb->key);
	}
}
#endif

static void
vrnd_lock(void)
{
}

int
main(void)
{
	struct vbh *bh;
	unsigned j, u, v, lr, n;
	struct foo *fp;

	VRND_SeedAll();
	VRND_SeedTestable(1);
	VRND_Lock = vrnd_lock;
	VRND_Unlock = vrnd_lock;

	bh = VBH_new(NULL, cmp, update);
	for (n = 2; n; n += n) {
		child(bh, n - 1, &u, &v);
		child(bh, n, &u, &v);
		child(bh, n + 1, &u, &v);
	}

	lr = 0; /* unconfuse some compilers... */

	for (j = 0; j < 2; j++) {
		/* First insert our N elements */
		for (u = 0; u < N; u++) {
			lr = VRND_RandomTestable() % R;
			ALLOC_OBJ(ff[u], FOO_MAGIC);
			assert(ff[u] != NULL);
			ff[u]->key = lr;
			ff[u]->n = u;
			VBH_insert(bh, ff[u]);

			fp = VBH_root(bh);
			assert(fp->idx == 1);
			assert(fp->key <= lr);
		}
		fprintf(stderr, "%d inserts OK\n", N);
		/* For M cycles, pick the root, insert new */
		for (u = 0; u < M; u++) {
			fp = VBH_root(bh);
			CHECK_OBJ_NOTNULL(fp, FOO_MAGIC);
			assert(fp->idx == 1);

			/*
			 * It cannot possibly be larger than the last
			 * value we added
			 */
			assert(fp->key <= lr);
			VBH_delete(bh, fp->idx);

			n = fp->n;
			ALLOC_OBJ(ff[n], FOO_MAGIC);
			assert(ff[n] != NULL);
			FREE_OBJ(fp);
			fp = ff[n];
			fp->n = n;

			lr = VRND_RandomTestable() % R;
			fp->key = lr;
			VBH_insert(bh, fp);
		}
		fprintf(stderr, "%d replacements OK\n", M);
		/* The remove everything */
		lr = 0;
		for (u = 0; u < N; u++) {
			fp = VBH_root(bh);
			CHECK_OBJ_NOTNULL(fp, FOO_MAGIC);
			assert(fp->idx == 1);
			assert(fp->key >= lr);
			lr = fp->key;
			VBH_delete(bh, fp->idx);
			ff[fp->n] = NULL;
			FREE_OBJ(fp);
		}
		fprintf(stderr, "%d removes OK\n", N);

		for (u = 0; u < M; u++) {
			v = VRND_RandomTestable() % N;
			if (ff[v] != NULL) {
				CHECK_OBJ(ff[v], FOO_MAGIC);
				AN(ff[v]->idx);
				if (ff[v]->key & 1) {
					VBH_delete(bh, ff[v]->idx);
					assert(ff[v]->idx == VBH_NOIDX);
					FREE_OBJ(ff[v]);
					ff[v] = NULL;
				} else {
					ff[v]->key = VRND_RandomTestable() % R;
					VBH_reorder(bh, ff[v]->idx);
				}
			} else {
				ALLOC_OBJ(ff[v], FOO_MAGIC);
				assert(ff[v] != NULL);
				ff[v]->key = VRND_RandomTestable() % R;
				VBH_insert(bh, ff[v]);
				CHECK_OBJ_NOTNULL(ff[v], FOO_MAGIC);
				AN(ff[v]->idx);
			}
#ifdef CHECK2
			chk2(bh);
#endif
		}
		fprintf(stderr, "%d updates OK\n", M);
	}
	while ((fp = VBH_root(bh)) != NULL) {
		CHECK_OBJ(fp, FOO_MAGIC);
		VBH_delete(bh, fp->idx);
		FREE_OBJ(fp);
	}
	VBH_destroy(&bh);
	AZ(bh);
	return (0);
}
#endif
