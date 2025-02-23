/*-
 * Copyright (c) 2005-2008 Poul-Henning Kamp <phk@FreeBSD.org>
 * All rights reserved.
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
 * Functions for assembling a bytestream into text-lines and calling
 * a function on each.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vdef.h"

#include "vas.h"	// XXX Flexelint "not used" - but req'ed for assert()
#include "miniobj.h"

#include "vlu.h"

struct vlu {
	unsigned	magic;
#define LINEUP_MAGIC	0x8286661
	char		*buf;
	unsigned	bufl;
	unsigned	bufp;
	void		*priv;
	vlu_f	*func;
};

struct vlu *
VLU_New(vlu_f *func, void *priv, unsigned bufsize)
{
	struct vlu *l;

	if (bufsize == 0)
		bufsize = BUFSIZ;
	ALLOC_OBJ(l, LINEUP_MAGIC);
	if (l != NULL) {
		l->func = func;
		l->priv = priv;
		l->bufl = bufsize - 1;
		l->buf = malloc(l->bufl + 1L);
		if (l->buf == NULL)
			FREE_OBJ(l);
	}
	return (l);
}

void
VLU_Reset(struct vlu *l)
{
	CHECK_OBJ_NOTNULL(l, LINEUP_MAGIC);
	l->bufp = 0;
}

void
VLU_Destroy(struct vlu **lp)
{
	struct vlu *l;

	TAKE_OBJ_NOTNULL(l, lp, LINEUP_MAGIC);
	free(l->buf);
	FREE_OBJ(l);
}

static int
LineUpProcess(struct vlu *l)
{
	char *p, *q;
	int i;

	l->buf[l->bufp] = '\0';
	for (p = l->buf; *p != '\0'; p = q) {
		/* Find first CR or NL */
		for (q = p; *q != '\0'; q++) {
			if (*q == '\n' || *q == '\r')
				break;
		}
		if (*q == '\0')
			break;
		*q++ = '\0';
		i = l->func(l->priv, p);
		if (i != 0)
			return (i);
	}
	if (*p != '\0') {
		q = strchr(p, '\0');
		assert(q != NULL);
		l->bufp = (unsigned)(q - p);
		memmove(l->buf, p, l->bufp);
		l->buf[l->bufp] = '\0';
	} else
		l->bufp = 0;
	return (0);
}

int
VLU_Fd(struct vlu *l, int fd)
{
	int i;

	CHECK_OBJ_NOTNULL(l, LINEUP_MAGIC);
	i = read(fd, l->buf + l->bufp, l->bufl - l->bufp);
	if (i == 0)
		return (-2);
	if (i < 0)
		return (-1);
	l->bufp += i;
	return (LineUpProcess(l));
}

int
VLU_File(int fd, vlu_f *func, void *priv, unsigned bufsize)
{
	struct vlu *vlu;
	int i;

	vlu = VLU_New(func, priv, bufsize);
	AN(vlu);
	do {
		i = VLU_Fd(vlu, fd);
	} while (i == 0);
	VLU_Destroy(&vlu);
	return (i);
}

int
VLU_Feed(struct vlu *l, const char *ptr, int len)
{
	int i = 0;
	unsigned u;

	CHECK_OBJ_NOTNULL(l, LINEUP_MAGIC);
	AN(ptr);
	assert(len > 0);
	while (len > 0) {
		u = len;
		if (u > l->bufl - l->bufp)
			u = l->bufl - l->bufp;
		memcpy(l->buf + l->bufp, ptr, u);
		len -= u;
		ptr += u;
		l->bufp += u;
		i = LineUpProcess(l);
		if (i)
			return (i);
	}
	return (i);
}
