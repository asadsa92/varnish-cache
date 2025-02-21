/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Martin Blix Grydeland <martin@varnish-software.com>
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
 */

#include "config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>

#include "vdef.h"
#include "vas.h"
#include "miniobj.h"

#include "vbm.h"
#include "vnum.h"
#include "vqueue.h"
#include "vre.h"
#include "vsb.h"

#include "vapi/vsl.h"

#include "vsl_api.h"
#include "vxp.h"

static void vxp_expr_or(struct vxp *vxp, struct vex **pvex);

static struct vex *
vex_alloc(const struct vxp *vxp)
{
	struct vex *vex;

	ALLOC_OBJ(vex, VEX_MAGIC);
	AN(vex);
	vex->options = vxp->vex_options;
	return (vex);
}

static void
vxp_expr_lhs(struct vxp *vxp, struct vex_lhs **plhs)
{
	char *p;
	int i;

	AN(plhs);
	AZ(*plhs);
	ALLOC_OBJ(*plhs, VEX_LHS_MAGIC);
	AN(*plhs);
	(*plhs)->tags = vbit_new(SLT__MAX);
	(*plhs)->level = -1;

	if (vxp->t->tok == '{') {
		/* Transaction level limits */
		vxp_NextToken(vxp);
		if (vxp->t->tok != VAL) {
			VSB_printf(vxp->sb, "Expected integer got '%.*s' ",
			    PF(vxp->t));
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		(*plhs)->level = (int)strtol(vxp->t->dec, &p, 0);
		if ((*plhs)->level < 0) {
			VSB_cat(vxp->sb, "Expected positive integer ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		if (*p == '-') {
			(*plhs)->level_pm = -1;
			p++;
		} else if (*p == '+') {
			(*plhs)->level_pm = 1;
			p++;
		}
		if (*p) {
			VSB_cat(vxp->sb, "Syntax error in level limit ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		vxp_NextToken(vxp);
		ExpectErr(vxp, '}');
		vxp_NextToken(vxp);
	}

	while (1) {
		/* The tags this expression applies to */
		if (vxp->t->tok == VXID) {
			(*plhs)->vxid++;
			i = 0;
		} else if (vxp->t->tok != VAL) {
			VSB_printf(vxp->sb, "Expected VSL tag name got '%.*s' ",
			    PF(vxp->t));
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		} else {
			(*plhs)->taglist++;
			i = VSL_Glob2Tags(vxp->t->dec, -1, vsl_vbm_bitset,
			    (*plhs)->tags);
		}
		if (i == -1) {
			VSB_cat(vxp->sb, "Tag name matches zero tags ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		if (i == -2) {
			VSB_cat(vxp->sb, "Tag name is ambiguous ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		if (i == -3) {
			VSB_cat(vxp->sb, "Syntax error in tag name ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		assert(i > 0 || vxp->t->tok == VXID);
		vxp_NextToken(vxp);
		if (vxp->t->tok != ',')
			break;
		vxp_NextToken(vxp);
	}

	if (vxp->t->tok == ':') {
		/* Record prefix */
		vxp_NextToken(vxp);
		if (vxp->t->tok != VAL) {
			VSB_printf(vxp->sb, "Expected string got '%.*s' ",
			    PF(vxp->t));
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		AN(vxp->t->dec);
		(*plhs)->prefix = strdup(vxp->t->dec);
		AN((*plhs)->prefix);
		(*plhs)->prefixlen = strlen((*plhs)->prefix);
		vxp_NextToken(vxp);
	}

	if (vxp->t->tok == '[') {
		/* LHS field [] */
		vxp_NextToken(vxp);
		if (vxp->t->tok != VAL) {
			VSB_printf(vxp->sb, "Expected integer got '%.*s' ",
			    PF(vxp->t));
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		(*plhs)->field = (int)strtol(vxp->t->dec, &p, 0);
		if (*p || (*plhs)->field <= 0) {
			VSB_cat(vxp->sb, "Expected positive integer ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
		vxp_NextToken(vxp);
		ExpectErr(vxp, ']');
		vxp_NextToken(vxp);
	}

	if ((*plhs)->vxid == 0)
		return;

	if ((*plhs)->vxid > 1 || (*plhs)->level >= 0 ||
	    (*plhs)->field > 0 || (*plhs)->prefixlen > 0 ||
	    (*plhs)->taglist > 0) {
		VSB_cat(vxp->sb, "Unexpected taglist selection for vxid ");
		vxp_ErrWhere(vxp, vxp->t, -1);
	}
}

static void
vxp_expr_num(struct vxp *vxp, struct vex_rhs **prhs, unsigned vxid)
{
	char *endptr;

	AN(prhs);
	AZ(*prhs);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected number got '%.*s' ", PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	AN(vxp->t->dec);
	ALLOC_OBJ(*prhs, VEX_RHS_MAGIC);
	AN(*prhs);
	if (strchr(vxp->t->dec, '.')) {
		(*prhs)->type = VEX_FLOAT;
		(*prhs)->val_float = VNUM(vxp->t->dec);
		if (isnan((*prhs)->val_float)) {
			VSB_cat(vxp->sb, "Floating point parse error ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
	} else {
		(*prhs)->type = VEX_INT;
		(*prhs)->val_int = strtoll(vxp->t->dec, &endptr, 0);
		while (isspace(*endptr))
			endptr++;
		if (*endptr != '\0') {
			VSB_cat(vxp->sb, "Integer parse error ");
			vxp_ErrWhere(vxp, vxp->t, -1);
			return;
		}
	}
	if (vxid && (*prhs)->type != VEX_INT) {
		VSB_printf(vxp->sb, "Expected integer got '%.*s' ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, 0);
		return;
	}
	vxp_NextToken(vxp);
}

static void
vxp_expr_str(struct vxp *vxp, struct vex_rhs **prhs)
{

	AN(prhs);
	AZ(*prhs);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected string got '%.*s' ", PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	AN(vxp->t->dec);
	ALLOC_OBJ(*prhs, VEX_RHS_MAGIC);
	AN(*prhs);
	(*prhs)->type = VEX_STRING;
	(*prhs)->val_string = strdup(vxp->t->dec);
	AN((*prhs)->val_string);
	(*prhs)->val_stringlen = strlen((*prhs)->val_string);
	vxp_NextToken(vxp);
}

static void
vxp_expr_regex(struct vxp *vxp, struct vex_rhs **prhs)
{
	int err, erroff;

	/* XXX: Caseless option */

	AN(prhs);
	AZ(*prhs);
	if (vxp->t->tok != VAL) {
		VSB_printf(vxp->sb, "Expected regular expression got '%.*s' ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	AN(vxp->t->dec);
	ALLOC_OBJ(*prhs, VEX_RHS_MAGIC);
	AN(*prhs);
	(*prhs)->type = VEX_REGEX;
	(*prhs)->val_string = strdup(vxp->t->dec);
	(*prhs)->val_regex = VRE_compile(vxp->t->dec, vxp->vre_options,
	    &err, &erroff, 1);
	if ((*prhs)->val_regex == NULL) {
		VSB_cat(vxp->sb, "Regular expression error: ");
		AZ(VRE_error(vxp->sb, err));
		VSB_putc(vxp->sb, ' ');
		vxp_ErrWhere(vxp, vxp->t, erroff);
		return;
	}
	vxp_NextToken(vxp);
}

static void
vxp_vxid_cmp(struct vxp *vxp)
{

	switch (vxp->t->tok) {
	/* Valid operators */
	case T_EQ:		/* == */
	case '<':		/* < */
	case '>':		/* > */
	case T_GEQ:		/* >= */
	case T_LEQ:		/* <= */
	case T_NEQ:		/* != */
		break;

	/* Error */
	default:
		VSB_printf(vxp->sb, "Expected vxid operator got '%.*s' ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
	}
}

/*
 * SYNTAX:
 *   expr_cmp:
 *     lhs
 *     lhs <operator> num|str|regex
 */

static void
vxp_expr_cmp(struct vxp *vxp, struct vex **pvex)
{

	AN(pvex);
	AZ(*pvex);
	*pvex = vex_alloc(vxp);
	AN(*pvex);
	vxp_expr_lhs(vxp, &(*pvex)->lhs);
	ERRCHK(vxp);

	if ((*pvex)->lhs->vxid) {
		vxp_vxid_cmp(vxp);
		ERRCHK(vxp);
	}

	/* Test operator */
	switch (vxp->t->tok) {

	/* Single lhs expressions don't take any more tokens */
	case EOI:
	case T_AND:
	case T_OR:
	case ')':
		(*pvex)->tok = T_TRUE;
		return;

	/* Valid operators */
	case T_EQ:		/* == */
	case '<':		/* < */
	case '>':		/* > */
	case T_GEQ:		/* >= */
	case T_LEQ:		/* <= */
	case T_NEQ:		/* != */
	case T_SEQ:		/* eq */
	case T_SNEQ:		/* ne */
	case '~':		/* ~ */
	case T_NOMATCH:		/* !~ */
		(*pvex)->tok = vxp->t->tok;
		break;

	/* Error */
	default:
		VSB_printf(vxp->sb, "Expected operator got '%.*s' ",
		    PF(vxp->t));
		vxp_ErrWhere(vxp, vxp->t, -1);
		return;
	}
	vxp_NextToken(vxp);
	ERRCHK(vxp);

	/* Value */
	switch ((*pvex)->tok) {
	case '\0':
		WRONG("Missing token");
		break;
	case T_EQ:		/* == */
	case '<':		/* < */
	case '>':		/* > */
	case T_GEQ:		/* >= */
	case T_LEQ:		/* <= */
	case T_NEQ:		/* != */
		vxp_expr_num(vxp, &(*pvex)->rhs, (*pvex)->lhs->vxid);
		break;
	case T_SEQ:		/* eq */
	case T_SNEQ:		/* ne */
		vxp_expr_str(vxp, &(*pvex)->rhs);
		break;
	case '~':		/* ~ */
	case T_NOMATCH:		/* !~ */
		vxp_expr_regex(vxp, &(*pvex)->rhs);
		break;
	default:
		INCOMPL();
	}
}

/*
 * SYNTAX:
 *   expr_group:
 *     '(' expr_or ')'
 *     expr_not
 */

static void
vxp_expr_group(struct vxp *vxp, struct vex **pvex)
{

	AN(pvex);
	AZ(*pvex);

	if (vxp->t->tok == '(') {
		SkipToken(vxp, '(');
		vxp_expr_or(vxp, pvex);
		ERRCHK(vxp);
		SkipToken(vxp, ')');
		return;
	}

	vxp_expr_cmp(vxp, pvex);
}

/*
 * SYNTAX:
 *   expr_not:
 *     'not' expr_group
 *     expr_group
 */

static void
vxp_expr_not(struct vxp *vxp, struct vex **pvex)
{

	AN(pvex);
	AZ(*pvex);

	if (vxp->t->tok == T_NOT) {
		*pvex = vex_alloc(vxp);
		AN(*pvex);
		(*pvex)->tok = vxp->t->tok;
		vxp_NextToken(vxp);
		vxp_expr_group(vxp, &(*pvex)->a);
		return;
	}

	vxp_expr_group(vxp, pvex);
}

/*
 * SYNTAX:
 *   expr_and:
 *     expr_not { 'and' expr_not }*
 */

static void
vxp_expr_and(struct vxp *vxp, struct vex **pvex)
{
	struct vex *a;

	AN(pvex);
	AZ(*pvex);
	vxp_expr_not(vxp, pvex);
	ERRCHK(vxp);
	while (vxp->t->tok == T_AND) {
		a = *pvex;
		*pvex = vex_alloc(vxp);
		AN(*pvex);
		(*pvex)->tok = vxp->t->tok;
		(*pvex)->a = a;
		vxp_NextToken(vxp);
		ERRCHK(vxp);
		vxp_expr_not(vxp, &(*pvex)->b);
		ERRCHK(vxp);
	}
}

/*
 * SYNTAX:
 *   expr_or:
 *     expr_and { 'or' expr_and }*
 */

static void
vxp_expr_or(struct vxp *vxp, struct vex **pvex)
{
	struct vex *a;

	AN(pvex);
	AZ(*pvex);
	vxp_expr_and(vxp, pvex);
	ERRCHK(vxp);
	while (vxp->t->tok == T_OR) {
		a = *pvex;
		*pvex = vex_alloc(vxp);
		AN(*pvex);
		(*pvex)->tok = vxp->t->tok;
		(*pvex)->a = a;
		vxp_NextToken(vxp);
		ERRCHK(vxp);
		vxp_expr_and(vxp, &(*pvex)->b);
		ERRCHK(vxp);
	}
}

/*
 * SYNTAX:
 *   expr:
 *     expr_or EOI { 'or' expr_or EOI }?
 */

static void
vxp_expr(struct vxp *vxp, struct vex **pvex)
{
	struct vex *a = NULL, *or;

	if (*pvex == NULL) {
		vxp_expr_or(vxp, pvex);
		ERRCHK(vxp);
		ExpectErr(vxp, EOI);
		return;
	}

	vxp_expr(vxp, &a);
	ERRCHK(vxp);

	or = vex_alloc(vxp);
	AN(or);
	or->tok = T_OR;
	or->b = *pvex;
	or->a = a;
	*pvex = or;
}

/*
 * Build a struct vex tree from the token list in vxp
 */

struct vex *
vxp_Parse(struct vxp *vxp)
{
	struct vex *vex = NULL;

	AZ(vxp->err);
	vxp->t = VTAILQ_FIRST(&vxp->tokens);

	while (vxp->t != NULL) {
		/* Ignore empty queries */
		while (vxp->t != NULL && vxp->t->tok == EOI)
			vxp->t = VTAILQ_NEXT(vxp->t, list);

		if (vxp->t == NULL)
			break;

		vxp_expr(vxp, &vex);

		if (vxp->err) {
			if (vex)
				vex_Free(&vex);
			AZ(vex);
			return (NULL);
		}

		vxp->t = VTAILQ_NEXT(vxp->t, list);
	}

	return (vex);
}

/*
 * Free a struct vex tree
 */

void
vex_Free(struct vex **pvex)
{
	struct vex *vex;
	struct vex_lhs *lhs;
	struct vex_rhs *rhs;

	TAKE_OBJ_NOTNULL(vex, pvex, VEX_MAGIC);

	if (vex->lhs) {
		CAST_OBJ(lhs, vex->lhs, VEX_LHS_MAGIC);
		if (lhs->tags)
			vbit_destroy(lhs->tags);
		if (lhs->prefix)
			free(lhs->prefix);
		FREE_OBJ(lhs);
	}
	if (vex->rhs) {
		CAST_OBJ(rhs, vex->rhs, VEX_RHS_MAGIC);
		if (rhs->val_string)
			free(rhs->val_string);
		if (rhs->val_regex)
			VRE_free(&rhs->val_regex);
		FREE_OBJ(rhs);
	}
	if (vex->a) {
		vex_Free(&vex->a);
		AZ(vex->a);
	}
	if (vex->b) {
		vex_Free(&vex->b);
		AZ(vex->b);
	}
	FREE_OBJ(vex);
}

#ifdef VXP_DEBUG

static void
vex_print_rhs(const struct vex_rhs *rhs)
{

	CHECK_OBJ_NOTNULL(rhs, VEX_RHS_MAGIC);
	fprintf(stderr, "rhs=");
	switch (rhs->type) {
	case VEX_INT:
		fprintf(stderr, "INT(%jd)", (intmax_t)rhs->val_int);
		break;
	case VEX_FLOAT:
		fprintf(stderr, "FLOAT(%f)", rhs->val_float);
		break;
	case VEX_STRING:
		AN(rhs->val_string);
		fprintf(stderr, "STRING(%s)", rhs->val_string);
		break;
	case VEX_REGEX:
		AN(rhs->val_string);
		AN(rhs->val_regex);
		fprintf(stderr, "REGEX(%s)", rhs->val_string);
		break;
	default:
		WRONG("rhs type");
		break;
	}
}

static void
vex_print_tags(const struct vbitmap *vbm)
{
	int i;
	int first = 1;

	for (i = 0; i < SLT__MAX; i++) {
		if (VSL_tags[i] == NULL)
			continue;
		if (!vbit_test(vbm, i))
			continue;
		if (first)
			first = 0;
		else
			fprintf(stderr, ",");
		fprintf(stderr, "%s", VSL_tags[i]);
	}
}

static void
vex_print(const struct vex *vex, int indent)
{
	CHECK_OBJ_NOTNULL(vex, VEX_MAGIC);

	fprintf(stderr, "%*s%s", indent, "", vxp_tnames[vex->tok]);
	if (vex->lhs != NULL) {
		CHECK_OBJ(vex->lhs, VEX_LHS_MAGIC);
		AN(vex->lhs->tags);
		fprintf(stderr, " lhs=");
		if (vex->lhs->level >= 0)
			fprintf(stderr, "{%d%s}", vex->lhs->level,
			    vex->lhs->level_pm < 0 ? "-" :
			    vex->lhs->level_pm > 0 ? "+" : "");
		fprintf(stderr, "(");
		vex_print_tags(vex->lhs->tags);
		fprintf(stderr, ")");
		if (vex->lhs->prefix) {
			assert(vex->lhs->prefixlen == strlen(vex->lhs->prefix));
			fprintf(stderr, ":%s", vex->lhs->prefix);
		}
		if (vex->lhs->field > 0)
			fprintf(stderr, "[%d]", vex->lhs->field);
	}
	if (vex->rhs != NULL) {
		fprintf(stderr, " ");
		vex_print_rhs(vex->rhs);
	}
	fprintf(stderr, "\n");
	if (vex->a != NULL)
		vex_print(vex->a, indent + 2);
	if (vex->b != NULL)
		vex_print(vex->b, indent + 2);
}

void
vex_PrintTree(const struct vex *vex)
{

	CHECK_OBJ_NOTNULL(vex, VEX_MAGIC);
	fprintf(stderr, "VEX tree:\n");
	vex_print(vex, 2);
}

#endif /* VXP_DEBUG */
