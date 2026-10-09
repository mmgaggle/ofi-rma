/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

/* Helpers shared by the library's sources: time, locks with deadlines,
 * and the few containers the endpoint needs. Not installed. */

#ifndef OFI_RMA_INT_H
#define OFI_RMA_INT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ofi_rma/ofi_rma.h"

/* The helpers below are not static, so they carry a prefix: a program
 * that links the static library may have functions of these names. */
#define sleep_ns orma_sleep_ns
#define mutex_lock_until orma_mutex_lock_until
#define cond_init_monotonic orma_cond_init_monotonic
#define cond_wait_until orma_cond_wait_until
#define fmt_into orma_fmt_into
#define ring_push orma_ring_push
#define ring_pop orma_ring_pop
#define ring_free orma_ring_free
#define u64map_put orma_u64map_put
#define u64map_get orma_u64map_get
#define u64map_del orma_u64map_del
#define u64map_free orma_u64map_free
#define hash_bytes orma_hash_bytes
#define proto_name orma_proto_name

#define NS_PER_US INT64_C(1000)
#define NS_PER_MS INT64_C(1000000)
#define NS_PER_S INT64_C(1000000000)

static inline int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}

static inline int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }
static inline int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static inline uint64_t minu64(uint64_t a, uint64_t b) { return a < b ? a : b; }
static inline uint64_t maxu64(uint64_t a, uint64_t b) { return a > b ? a : b; }

/** a + b, saturating at INT64_MAX: deadlines far off stay far off */
static inline int64_t add_sat(int64_t a, int64_t b)
{
	if (b > 0 && a > INT64_MAX - b)
		return INT64_MAX;
	return a + b;
}

static inline struct timespec ns_to_ts(int64_t ns)
{
	struct timespec ts;
	if (ns < 0)
		ns = 0;
	ts.tv_sec = (time_t)(ns / NS_PER_S);
	ts.tv_nsec = (long)(ns % NS_PER_S);
	return ts;
}

/** sleep for ns nanoseconds; 0 or less returns at once */
void sleep_ns(int64_t ns);

/** lock m, unless the CLOCK_MONOTONIC deadline passes first: 0 when
 * locked, ETIMEDOUT when not */
int mutex_lock_until(pthread_mutex_t *m, int64_t deadline_ns);

/** a condition variable on CLOCK_MONOTONIC */
int cond_init_monotonic(pthread_cond_t *c);
/** wait on c until signalled or the CLOCK_MONOTONIC deadline passes: 0,
 * or ETIMEDOUT */
int cond_wait_until(pthread_cond_t *c, pthread_mutex_t *m, int64_t deadline_ns);

/** snprintf() into a fixed buffer, never failing */
void fmt_into(char *buf, size_t len, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/** a growable FIFO of (until, key) pairs */
struct ring_ent {
	int64_t until;
	uint64_t key;
};
struct ring {
	struct ring_ent *v;
	size_t head, count, cap;
};
int ring_push(struct ring *r, int64_t until, uint64_t key);
static inline bool ring_empty(const struct ring *r) { return r->count == 0; }
static inline struct ring_ent ring_front(const struct ring *r)
{
	return r->v[r->head];
}
void ring_pop(struct ring *r);
void ring_free(struct ring *r);

/** a map from 64-bit keys to 64-bit values, open addressing */
struct u64map {
	uint64_t *keys;
	int64_t *vals;
	uint8_t *state;  /* 0 empty, 1 full, 2 deleted */
	size_t cap, count, used;
};
int u64map_put(struct u64map *m, uint64_t key, int64_t val);
/** the value of key into *val; false when there is none */
bool u64map_get(const struct u64map *m, uint64_t key, int64_t *val);
void u64map_del(struct u64map *m, uint64_t key);
void u64map_free(struct u64map *m);

/** FNV-1a */
uint64_t hash_bytes(const void *p, size_t len);

/* tokens: the wire name for a libfabric protocol, or NULL */
const char *proto_name(uint32_t proto);

#endif /* OFI_RMA_INT_H */
