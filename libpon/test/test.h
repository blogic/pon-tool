/* SPDX-License-Identifier: LGPL-2.1-only */
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
#ifndef LIBPON_TEST_H
#define LIBPON_TEST_H

#include <stdio.h>
#include <string.h>

#define CHECK(cond) do {						\
	t_checks++;							\
	if (!(cond)) {							\
		t_failures++;						\
		fprintf(stderr, "%s:%d: check failed: %s\n",		\
			__FILE__, __LINE__, #cond);			\
	}								\
} while (0)

#define CHECK_EQ(actual, expected) do {					\
	long long t_a = (long long)(actual),				\
		  t_b = (long long)(expected);				\
	t_checks++;							\
	if (t_a != t_b) {						\
		t_failures++;						\
		fprintf(stderr,						\
			"%s:%d: %s = %lld, expected %s = %lld\n",	\
			__FILE__, __LINE__, #actual, t_a,		\
			#expected, t_b);				\
	}								\
} while (0)

#define CHECK_STR(actual, expected) do {				\
	const char *t_sa = (actual), *t_sb = (expected);		\
	t_checks++;							\
	if (strcmp(t_sa, t_sb)) {					\
		t_failures++;						\
		fprintf(stderr,						\
			"%s:%d: %s = \"%s\", expected \"%s\"\n",	\
			__FILE__, __LINE__, #actual, t_sa, t_sb);	\
	}								\
} while (0)

static int t_failures;
static int t_checks;

/**
 * t_report - print the totals of a test program
 * @name: the name of the program
 *
 * Return: 0 when every check passed, else 1.
 */
static inline int t_report(const char *name)
{
	fprintf(stderr, "%s: %d checks, %d failures\n", name, t_checks,
		t_failures);
	return t_failures ? 1 : 0;
}

#endif
