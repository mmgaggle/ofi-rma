/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

#include "ofi_rma_int.h"

#include <stdio.h>

void sleep_ns(int64_t ns)
{
	if (ns <= 0)
		return;
	struct timespec ts = ns_to_ts(ns);
	while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR)
		;
}

int mutex_lock_until(pthread_mutex_t *m, int64_t deadline_ns)
{
	if (deadline_ns == INT64_MAX)
		return pthread_mutex_lock(m);
#if defined(__GLIBC__) && \
	(__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
	struct timespec ts = ns_to_ts(deadline_ns);
	return pthread_mutex_clocklock(m, CLOCK_MONOTONIC, &ts);
#else
	/* pthread_mutex_timedlock() counts on CLOCK_REALTIME: carry the time
	 * left over, as C++ timed mutexes do on such a C library */
	for (;;) {
		const int64_t left = deadline_ns - now_ns();
		if (left <= 0)
			return pthread_mutex_trylock(m) == 0 ? 0 : ETIMEDOUT;
		struct timespec rt;
		clock_gettime(CLOCK_REALTIME, &rt);
		const int64_t until = (int64_t)rt.tv_sec * NS_PER_S + rt.tv_nsec +
			left;
		struct timespec ts = ns_to_ts(until);
		const int r = pthread_mutex_timedlock(m, &ts);
		if (r != ETIMEDOUT)
			return r;
	}
#endif
}

int cond_init_monotonic(pthread_cond_t *c)
{
	pthread_condattr_t a;
	int r = pthread_condattr_init(&a);
	if (r)
		return r;
	r = pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	if (!r)
		r = pthread_cond_init(c, &a);
	pthread_condattr_destroy(&a);
	return r;
}

int cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, int64_t deadline_ns)
{
	if (deadline_ns == INT64_MAX)
		return pthread_cond_wait(c, m);
	struct timespec ts = ns_to_ts(deadline_ns);
	return pthread_cond_timedwait(c, m, &ts);
}

void fmt_into(char *buf, size_t len, const char *fmt, ...)
{
	if (!len)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, len, fmt, ap);
	va_end(ap);
}

int ring_push(struct ring *r, int64_t until, uint64_t key)
{
	if (r->count == r->cap) {
		const size_t cap = r->cap ? 2 * r->cap : 64;
		struct ring_ent *v = malloc(cap * sizeof(*v));
		if (!v)
			return -ENOMEM;
		for (size_t i = 0; i < r->count; i++)
			v[i] = r->v[(r->head + i) % r->cap];
		free(r->v);
		r->v = v;
		r->cap = cap;
		r->head = 0;
	}
	r->v[(r->head + r->count) % r->cap] = (struct ring_ent){until, key};
	r->count++;
	return 0;
}

void ring_pop(struct ring *r)
{
	if (!r->count)
		return;
	r->head = (r->head + 1) % r->cap;
	r->count--;
}

void ring_free(struct ring *r)
{
	free(r->v);
	memset(r, 0, sizeof(*r));
}

static uint64_t mix64(uint64_t k)
{
	k ^= k >> 33;
	k *= UINT64_C(0xff51afd7ed558ccd);
	k ^= k >> 33;
	k *= UINT64_C(0xc4ceb9fe1a85ec53);
	k ^= k >> 33;
	return k;
}

static size_t u64map_find(const struct u64map *m, uint64_t key, bool *found)
{
	/* the slot that holds key, or else the first free one on its probe */
	size_t i = mix64(key) & (m->cap - 1);
	size_t free_slot = SIZE_MAX;
	for (size_t n = 0; n < m->cap; n++, i = (i + 1) & (m->cap - 1)) {
		if (m->state[i] == 0) {
			*found = false;
			return free_slot != SIZE_MAX ? free_slot : i;
		}
		if (m->state[i] == 2) {
			if (free_slot == SIZE_MAX)
				free_slot = i;
		} else if (m->keys[i] == key) {
			*found = true;
			return i;
		}
	}
	*found = false;
	return free_slot;
}

static int u64map_grow(struct u64map *m)
{
	struct u64map n = {0};
	n.cap = m->cap ? 2 * m->cap : 64;
	/* only full slots move, so a table of tombstones shrinks back */
	while (n.cap / 2 < m->count + 1)
		n.cap *= 2;
	n.keys = calloc(n.cap, sizeof(*n.keys));
	n.vals = calloc(n.cap, sizeof(*n.vals));
	n.state = calloc(n.cap, 1);
	if (!n.keys || !n.vals || !n.state) {
		free(n.keys);
		free(n.vals);
		free(n.state);
		return -ENOMEM;
	}
	for (size_t i = 0; i < m->cap; i++) {
		if (m->state[i] != 1)
			continue;
		bool found;
		const size_t j = u64map_find(&n, m->keys[i], &found);
		n.keys[j] = m->keys[i];
		n.vals[j] = m->vals[i];
		n.state[j] = 1;
		n.count++;
		n.used++;
	}
	u64map_free(m);
	*m = n;
	return 0;
}

int u64map_put(struct u64map *m, uint64_t key, int64_t val)
{
	if (!m->cap || (m->used + 1) * 4 > m->cap * 3) {
		const int r = u64map_grow(m);
		if (r)
			return r;
	}
	bool found;
	const size_t i = u64map_find(m, key, &found);
	if (!found) {
		if (m->state[i] == 0)
			m->used++;
		m->state[i] = 1;
		m->keys[i] = key;
		m->count++;
	}
	m->vals[i] = val;
	return 0;
}

bool u64map_get(const struct u64map *m, uint64_t key, int64_t *val)
{
	if (!m->cap)
		return false;
	bool found;
	const size_t i = u64map_find(m, key, &found);
	if (found && val)
		*val = m->vals[i];
	return found;
}

void u64map_del(struct u64map *m, uint64_t key)
{
	if (!m->cap)
		return;
	bool found;
	const size_t i = u64map_find(m, key, &found);
	if (found) {
		m->state[i] = 2;
		m->count--;
	}
}

void u64map_free(struct u64map *m)
{
	free(m->keys);
	free(m->vals);
	free(m->state);
	memset(m, 0, sizeof(*m));
}

uint64_t hash_bytes(const void *p, size_t len)
{
	const uint8_t *b = p;
	uint64_t h = UINT64_C(0xcbf29ce484222325);
	for (size_t i = 0; i < len; i++) {
		h ^= b[i];
		h *= UINT64_C(0x100000001b3);
	}
	return h;
}
