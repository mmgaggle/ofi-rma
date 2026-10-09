/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

#include "ofi_rma_int.h"

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>

#include <ctype.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <strings.h>

/* largest single write; plans are a stripe or a shard extent, so this
 * only splits unusually large ranges */
#define MAX_WRITE (UINT64_C(8) << 20)
/* how long a failed address insert is remembered: writes to the peer
 * fail at once until then, instead of queueing another slow insert */
#define INSERT_RETRY_NS NS_PER_S
/* A write that polls for the others sleeps between polls only when the
 * completion queue has no wait object. It polls with only a yield in
 * between until SPIN_TIME has passed without a sign of work, then sleeps
 * WAIT_SLEEP between polls. A sign of work is a completion, or a poll that
 * took clearly longer than an idle one (POLL_BUSY_MARGIN beyond twice the
 * shortest poll seen): a provider that progresses only while polled, as
 * the UET reference provider does, moves data in its polls long before
 * the write completes. A sleep lasts about 50 us more than asked, the
 * kernel's default timer slack. */
#define SPIN_TIME_NS (200 * NS_PER_US)
#define WAIT_SLEEP_NS (50 * NS_PER_US)
#define POLL_BUSY_MARGIN_NS (2 * NS_PER_US)
/* registrations a window may go through for a key out of quarantine */
#define REKEY_ATTEMPTS 8
/* how long closing an endpoint polls its completion queue first */
#define DRAIN_BEFORE_CLOSE_NS (50 * NS_PER_MS)
/* Options of the UET libfabric provider, at FI_OPT_ENDPOINT, read with
 * fi_getopt() into a bool (FI_PROV_SPECIFIC, 1 << 31, plus a code; see its
 * prov/uet_fi.h): whether closing the endpoint discards the writes it has
 * outstanding, and whether fi_cancel() discards one. Other providers do
 * not know them. */
#define OPT_CLOSE_DISCARDS ((int)((1U << 31) | 0x5545U))
#define OPT_CANCEL_DISCARDS ((int)((1U << 31) | 0x5543U))
/* fi_control() command of the UET libfabric provider (FI_UET_MR_REKEY in
 * its prov/uet_fi.h): give a region a new key in place, returned in a
 * uint64_t, the old key dead when the call returns; -FI_ENOSYS from a
 * provider or device that cannot */
#define MR_REKEY ((int)((1U << 31) | 0x554bU))
/* the least time a write must have left to post; see ofi_rma_write() */
#define POST_MARGIN_NS NS_PER_MS
/* plans this large or larger teach the endpoint how fast plans move;
 * smaller ones take as long as the network's latency, whatever their
 * size */
#define RATE_MIN_BYTES (UINT64_C(1) << 20)
/* the scheduling slack a write keeps ahead of its budget's end before
 * any cut-off was late, and the least it keeps; see note_notice_delay() */
#define SLACK_INITIAL_MS 50
#define SLACK_MIN_MS 10
/* a waiting write that finds no poll for this long polls itself, and cuts
 * off what is due: the poller may be off the CPU */
#define POLLER_STALE_NS (5 * NS_PER_MS)
/* longest a write blocks on the completion queue's wait object before it
 * looks again; bounds how late it notices a cut-off */
#define WAIT_MAX_NS NS_PER_MS
/* longest a call waiting for the endpoint lock blocks before it looks
 * whether an insert on a FI_THREAD_DOMAIN domain is what holds it */
#define QUIET_POLL_NS NS_PER_MS
/* the API version asked for: old enough for a provider built against 1.x
 * headers, new enough for FI_CONTEXT2, the mr_mode bits used and dma-buf
 * registration (FI_MR_DMABUF, 1.20) */
#define API_VERSION FI_VERSION(1, 20)
#define ERR_MAX 1024

struct region {
	struct fid_mr *mr;
	char *ptr;
	size_t len;
	uint64_t key;
	void *desc;
	struct ofi_rma_memory mem;
	/* what peers may do with it: FI_REMOTE_WRITE, FI_REMOTE_READ, or a
	 * local buffer's FI_WRITE and FI_READ; kept across re-registrations */
	uint64_t access;
};

struct window_ent {
	uint64_t id;
	struct region r;
};

/* The address vector's peers. They have a lock of their own, so that a
 * write can wait for a peer's insert without mtx, which an insert on a
 * FI_THREAD_DOMAIN domain holds. No provider call runs under it. Lock
 * order: mtx, then peer_mtx. */
enum peer_state {
	PEER_PENDING,  /* queued for an insert, or being inserted */
	PEER_READY,
	PEER_FAILED,   /* the insert failed; tried again after retry_at */
	PEER_REMOVING, /* evicted, and being removed from the address vector */
};

struct peer {
	struct peer *hnext; /* hash chain */
	struct peer *qnext; /* insert or remove queue; in one at most */
	uint64_t hash;
	/* the name, zero-padded so the provider never reads past it, and
	 * kept for the entry's life (some providers keep the pointer) */
	uint8_t addr_buf[OFI_RMA_ADDR_MAX];
	size_t name_len;
	fi_addr_t addr;
	enum peer_state state;
	int err;  /* why the insert failed */
	int64_t retry_at;
	int64_t used;
	/* writes that hold the address: from the lookup until they retire.
	 * The entry is not dropped while any do. */
	uint32_t refs;
};

struct peer_queue {
	struct peer *head, *tail;
};

struct plan;
/* one posted write or read; the context must stay put until it
 * completes, and comes first, as completions carry its address */
struct op {
	struct fi_context2 ctx;
	struct plan *plan;
	bool done;  /* its completion arrived */
};

struct plan {
	struct plan *prev, *next;
	struct op *ops;  /* allocated up front, never reallocated */
	size_t n_ops;
	uint32_t outstanding;
	int err;
	/* when the caller's budget runs out: nothing may land after it */
	int64_t budget_end;
	/* when its first write was posted */
	int64_t posted_at;
	size_t slot;
	struct peer *peer;
	/* its operations read from the peer into the slot, rather than
	 * write the slot to the peer */
	bool reading;
	/* its writer cancelled it and left; it is retired when the last of
	 * its cancelled operations completes, or when the endpoint closes */
	bool abandoned;
	/* when its writes are to be cut off if still in flight */
	int64_t deadline;
	/* cut off by cancelling its operations, by its writer or on its
	 * behalf, with what its writer returns; hooked when a test's cancel
	 * hook stood for the provider, which keeps the plan */
	bool cut;
	bool hooked;
	bool released;  /* hooked: the endpoint closed, slot given back */
	int cut_result;
	/* the plan whose deadline a reset is for */
	bool due;
};

struct plan_list {
	struct plan *head, *tail;
};

enum verdict { IN_TIME, LATE, PAST_TOLERANCE, UNSAFE };

struct ofi_rma_ep {
	struct ofi_rma_config cfg;  /* strings point at the copies below */
	char *s_provider, *s_domain, *s_node, *s_service;
	struct ofi_rma_hooks hooks;
	struct fi_info *info;
	struct fid_fabric *fabric;
	struct fid_domain *domain;
	struct fid_av *av;
	struct fid_cq *cq;
	struct fid_ep *ep;
	/* the completion queue's wait object, or -1 when it has none */
	int cq_fd;
	char *prov;
	char wire[OFI_RMA_WIRE_MAX];
	/* this endpoint's name; under name_mtx, since a cut-off changes it */
	pthread_mutex_t name_mtx;
	uint8_t my_name[OFI_RMA_ADDR_MAX];
	size_t my_name_len;
	uint64_t mr_mode;
	bool dc;
	bool hmem;   /* the provider offers FI_HMEM */
	bool reads;  /* the provider offers RMA reads */
	/* the domain is FI_THREAD_SAFE: an address insert needs no lock */
	bool thread_safe;
	uint64_t max_write;
	uint64_t next_key;

	/* every libfabric call on this endpoint runs under this lock, except
	 * an address insert on a thread-safe domain, and so does every touch
	 * of the state below. A write waits for it only until its deadline,
	 * and other calls only briefly while an insert holds it. */
	pthread_mutex_t mtx;
	/* set while an insert on a FI_THREAD_DOMAIN domain waits for the
	 * writes in flight to finish, and while it runs: no write starts.
	 * Written under mtx; read without it by calls that wait for an
	 * insert at most cfg.insert_wait_ms. */
	_Atomic bool quiesce;
	/* when quiesce was last set; written before it */
	_Atomic int64_t quiet_since_ns;
	/* set while such an insert is in the provider, holding mtx: nothing
	 * else calls into the domain, or polls it, until it returns */
	_Atomic bool inserting;
	/* when the latest window a pool still has lent was lent; 0 when none
	 * is */
	_Atomic int64_t lent_ns;
	/* calls that went on without the endpoint, or a write without its
	 * new peer, after insert_wait_ms for an insert */
	_Atomic uint64_t insert_busy;

	struct window_ent *windows;  /* sorted by id */
	size_t nwin, capwin;
	_Atomic uint64_t nwindows;  /* nwin, readable without mtx */
	uint64_t next_window;
	/* keys of windows' regions that left service, until when each is in
	 * quarantine: not accepted again for a window */
	struct u64map retired;
	struct ring retired_order;
	_Atomic uint64_t windows_rekeyed;
	_Atomic uint64_t key_collisions;
	_Atomic uint64_t rekeys_in_place;
	_Atomic uint64_t rekeys_reregistered;
	/* the provider re-keys a region in place: 1 yes, 0 no, -1 not
	 * tried; once it says it cannot, it is not asked again */
	_Atomic int rekey_in_place;

	char *stage;
	struct region stage_mr;
	bool *stage_busy;

	pthread_mutex_t peer_mtx;
	pthread_cond_t peer_cv;    /* a peer settled */
	pthread_cond_t insert_cv;  /* insert_q or remove_q has work */
	struct peer **buckets;
	size_t nbuckets;
	size_t npeers;
	struct peer_queue insert_q;
	struct peer_queue remove_q;
	size_t pending;   /* peers in state pending */
	size_t removing;  /* peers in state removing */
	_Atomic uint64_t inserts_refused;
	pthread_t *insert_thrs;
	unsigned n_insert_thrs;

	struct plan_list plans;
	/* plans a test's cancel hook cancelled: the provider still holds
	 * their operations, and reads their staging, until the endpoint
	 * closes. They give their slots back then, and stay until the
	 * endpoint is destroyed, since their writers may still look at
	 * them. */
	struct plan_list hook_cancelled;
	/* fi_cancel() discards a write (OPT_CANCEL_DISCARDS); probed
	 * whenever the endpoint opens */
	_Atomic bool cancel_discards;
	/* closing the endpoint discards its writes (OPT_CLOSE_DISCARDS): 1
	 * yes, 0 no, -1 the provider does not say */
	_Atomic int close_discards;
	_Atomic uint64_t plans_cut_off;
	_Atomic uint64_t cancels_failed;
	/* Waiting writes. One of them, the poller, polls the completion queue
	 * for all; the others sleep on done_cv until a plan finishes, the
	 * poller leaves, or a cut-off fails them. */
	pthread_cond_t done_cv;
	bool poller;
	/* the shortest poll the poller has timed: what a poll costs when the
	 * provider has nothing to do */
	int64_t poll_floor;
	/* passes over the completion queue, each released when it is done:
	 * ofi_rma_ep_sync() orders its caller's reads after them when an
	 * insert keeps it from mtx */
	_Atomic uint64_t polls;

	pthread_t progress_thr;
	bool progress_started;
	_Atomic bool stopping;

	/* the provider's text for the latest failure; a lock of its own, so
	 * that reading it never waits for an insert that holds mtx */
	pthread_mutex_t err_mtx;
	char last_err[ERR_MAX];
	_Atomic uint64_t writes_posted;
	_Atomic uint64_t writes_failed;
	_Atomic uint64_t bytes_written;
	_Atomic uint64_t reads_posted;
	_Atomic uint64_t reads_failed;
	_Atomic uint64_t bytes_read;
	_Atomic uint64_t peers_inserted;
	_Atomic uint64_t staging_busy;
	_Atomic uint64_t timeouts;
	_Atomic uint64_t peer_timeouts;

	/* set when a cut-off could not reopen the endpoint, or when the
	 * endpoint is unsafe; every write fails from then on */
	_Atomic bool broken;
	/* A cut-off failed, or ended after a budget it protected: writes may
	 * have landed, or may still land, after their callers gave up.
	 * Nothing calls into the provider for this endpoint again, except to
	 * close it. */
	_Atomic bool unsafe;
	/* the endpoint did not close in a cut-off, and is still open */
	bool zombie;
	_Atomic uint64_t resets;
	_Atomic uint64_t cutoffs_failed;
	_Atomic uint64_t cutoffs_late;
	/* what a cut-off is expected to cost, in milliseconds; a write stops
	 * waiting this much before its budget runs out */
	_Atomic int64_t cutoff_cost_ms;
	/* the scheduling slack, in milliseconds: how late cut-offs start */
	_Atomic int64_t cutoff_slack_ms;
	_Atomic uint64_t budget_refused;
	_Atomic uint64_t cutoffs_on_behalf;
	_Atomic uint64_t max_lateness_ms;
	_Atomic int64_t last_late_ns;
	_Atomic uint64_t cutoffs_past_tolerance;
	_Atomic int64_t last_past_ns;
	/* the gap between the last two polls of the endpoint, the longest,
	 * and when that was: a long one means its threads did not run */
	int64_t last_poll_gap;
	_Atomic uint64_t max_poll_gap_ms;
	_Atomic int64_t last_long_gap_ns;
	/* how late the cut-off being judged started, for its message */
	int64_t notice_delay;
	/* when the completion queue was last polled */
	int64_t last_poll;
	_Atomic uint64_t late_starts;
	/* how fast plans of RATE_MIN_BYTES or more complete, from the first
	 * post to the last completion, in bytes per nanosecond, averaged; 0
	 * until one has */
	double plan_rate;
};

/* ---------------------------------------------------------------------- */

/* libfabric's codes below 256 are errno values; the rest are its own */
static int to_errno(ssize_t r)
{
	const ssize_t e = r < 0 ? -r : r;
	if (e > 0 && e < 256)
		return -(int)e;
	return -EIO;
}

static const char *fi_err(ssize_t r)
{
	return fi_strerror((int)(r < 0 ? -r : r));
}

static void set_err(struct ofi_rma_ep *d, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void set_err(struct ofi_rma_ep *d, const char *fmt, ...)
{
	char buf[ERR_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	pthread_mutex_lock(&d->err_mtx);
	memcpy(d->last_err, buf, sizeof(buf));
	pthread_mutex_unlock(&d->err_mtx);
}

static void get_err(struct ofi_rma_ep *d, char *buf, size_t len)
{
	pthread_mutex_lock(&d->err_mtx);
	fmt_into(buf, len, "%s", d->last_err);
	pthread_mutex_unlock(&d->err_mtx);
}

static void plan_link_head(struct plan_list *l, struct plan *p)
{
	p->prev = NULL;
	p->next = l->head;
	if (l->head)
		l->head->prev = p;
	else
		l->tail = p;
	l->head = p;
}

static void plan_link_tail(struct plan_list *l, struct plan *p)
{
	p->next = NULL;
	p->prev = l->tail;
	if (l->tail)
		l->tail->next = p;
	else
		l->head = p;
	l->tail = p;
}

static void plan_unlink(struct plan_list *l, struct plan *p)
{
	if (p->prev)
		p->prev->next = p->next;
	else
		l->head = p->next;
	if (p->next)
		p->next->prev = p->prev;
	else
		l->tail = p->prev;
	p->prev = p->next = NULL;
}

static void plan_free(struct plan *p)
{
	free(p->ops);
	free(p);
}

static void q_push(struct peer_queue *q, struct peer *p)
{
	p->qnext = NULL;
	if (q->tail)
		q->tail->qnext = p;
	else
		q->head = p;
	q->tail = p;
}

static struct peer *q_pop(struct peer_queue *q)
{
	struct peer *p = q->head;
	if (p) {
		q->head = p->qnext;
		if (!q->head)
			q->tail = NULL;
		p->qnext = NULL;
	}
	return p;
}

static struct peer *peer_find(struct ofi_rma_ep *d, const void *name,
			      size_t len, uint64_t h)
{
	for (struct peer *p = d->buckets[h & (d->nbuckets - 1)]; p; p = p->hnext) {
		if (p->hash == h && p->name_len == len &&
		    !memcmp(p->addr_buf, name, len))
			return p;
	}
	return NULL;
}

static void peer_link(struct ofi_rma_ep *d, struct peer *p)
{
	struct peer **b = &d->buckets[p->hash & (d->nbuckets - 1)];
	p->hnext = *b;
	*b = p;
	d->npeers++;
}

static void peer_erase(struct ofi_rma_ep *d, struct peer *p)
{
	struct peer **b = &d->buckets[p->hash & (d->nbuckets - 1)];
	while (*b && *b != p)
		b = &(*b)->hnext;
	if (*b)
		*b = p->hnext;
	d->npeers--;
	free(p);
}

static struct window_ent *window_find(struct ofi_rma_ep *d, uint64_t id)
{
	size_t lo = 0, hi = d->nwin;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (d->windows[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo < d->nwin && d->windows[lo].id == id ? &d->windows[lo] : NULL;
}

/* ---------------------------------------------------------------------- */
/* registration                                                           */
/* ---------------------------------------------------------------------- */

/* register memory; a region others write into needs a key, a local write
 * source only a descriptor */
static int reg(struct ofi_rma_ep *d, char *ptr, size_t len, uint64_t access,
	       struct region *out, const struct ofi_rma_memory *mem)
{
	struct ofi_rma_memory host;
	if (!mem) {
		ofi_rma_memory_init(&host);
		mem = &host;
	}
	const bool device = mem->iface != OFI_RMA_IFACE_SYSTEM;
	/* also with a dma-buf: tcp, for one, registers a dma-buf without
	 * FI_HMEM, and then cannot place a write into it */
	if (device && !d->hmem) {
		set_err(d, "%s does not offer FI_HMEM, so a window cannot live "
			"in device memory", d->prov);
		return -EOPNOTSUPP;
	}
	if (d->hooks.reg)
		d->hooks.reg(d->hooks.ctx, ptr, len, mem);
	const uint64_t req_key = (d->mr_mode & FI_MR_PROV_KEY) ? 0 : d->next_key++;
	struct fid_mr *mr = NULL;
	int r;
	if (!device) {
		r = fi_mr_reg(d->domain, ptr, len, access, 0, req_key, 0, &mr,
			      NULL);
	} else {
		struct iovec iov = {ptr, len};
		/* base_addr is the address of the dma-buf's first byte: verbs
		 * hands ibv_reg_dmabuf_mr() the iova base_addr + offset, which
		 * must be ptr */
		struct fi_mr_dmabuf dmabuf = {
			.fd = mem->dmabuf_fd,
			.offset = mem->dmabuf_offset,
			.len = len,
			.base_addr = ptr - mem->dmabuf_offset,
		};
		struct fi_mr_attr attr;
		memset(&attr, 0, sizeof(attr));
		if (mem->dmabuf_fd >= 0)
			attr.dmabuf = &dmabuf;
		else
			attr.mr_iov = &iov;
		attr.iov_count = 1;
		attr.access = access;
		attr.requested_key = req_key;
		switch (mem->iface) {
		case OFI_RMA_IFACE_CUDA:
			attr.iface = FI_HMEM_CUDA;
			attr.device.cuda = mem->device;
			break;
		case OFI_RMA_IFACE_ZE:
			attr.iface = FI_HMEM_ZE;
			attr.device.ze = mem->device;
			break;
		default:
			/* ROCr finds the device from the address; the union
			 * stays zero, as older libfabric headers have no member
			 * for it */
			attr.iface = FI_HMEM_ROCR;
			break;
		}
		r = fi_mr_regattr(d->domain, &attr,
				  mem->dmabuf_fd >= 0 ? FI_MR_DMABUF : 0, &mr);
	}
	if (r) {
		/* a provider can offer FI_HMEM while libfabric itself was built
		 * without the runtime for this kind of device memory */
		set_err(d, "%s: %s%s", device ? "fi_mr_regattr" : "fi_mr_reg",
			fi_err(r),
			device && r == -FI_ENOSYS ?
			" (this libfabric has no support for the device memory: "
			"see FI_LOG_LEVEL=info for the hmem interfaces it "
			"initialized)" : "");
		return to_errno(r);
	}
	if (d->mr_mode & FI_MR_ENDPOINT) {
		r = fi_mr_bind(mr, &d->ep->fid, 0);
		if (!r)
			r = fi_mr_enable(mr);
		if (r) {
			set_err(d, "binding a region to the endpoint: %s",
				fi_err(r));
			fi_close(&mr->fid);
			return to_errno(r);
		}
	}
	const uint64_t key = fi_mr_key(mr);
	if ((access & (FI_REMOTE_WRITE | FI_REMOTE_READ)) &&
	    key == FI_KEY_NOTAVAIL) {
		set_err(d, "the provider gave the region no key");
		fi_close(&mr->fid);
		return -EOPNOTSUPP;
	}
	out->mr = mr;
	out->ptr = ptr;
	out->len = len;
	out->key = key;
	out->desc = fi_mr_desc(mr);
	out->mem = *mem;
	out->access = access;
	return 0;
}

/* register a window's memory, under a key out of quarantine */
static int reg_window_locked(struct ofi_rma_ep *d, char *ptr, size_t len,
			     const struct ofi_rma_memory *mem, uint64_t access,
			     struct region *out)
{
	const int64_t now = now_ns();
	while (!ring_empty(&d->retired_order) &&
	       ring_front(&d->retired_order).until <= now) {
		const struct ring_ent e = ring_front(&d->retired_order);
		ring_pop(&d->retired_order);
		int64_t until;
		if (u64map_get(&d->retired, e.key, &until) && until == e.until)
			u64map_del(&d->retired, e.key);
	}
	for (int attempt = 0; attempt < REKEY_ATTEMPTS; attempt++) {
		struct region fresh;
		const int r = reg(d, ptr, len, access, &fresh, mem);
		if (r < 0)
			return r;
		if (!u64map_get(&d->retired, fresh.key, NULL)) {
			*out = fresh;
			return 0;
		}
		/* a late write may still carry this key: drop it, and the
		 * provider gives another */
		d->key_collisions++;
		fi_close(&fresh.mr->fid);
	}
	set_err(d, "the provider kept giving memory keys still in quarantine");
	return -EAGAIN;
}

/* close a window's region, and put its key in quarantine */
static int close_window_region_locked(struct ofi_rma_ep *d, struct region *r)
{
	if (!r->mr)
		return 0;
	/* quarantined first, so that a key whose region does close is never
	 * left out; one whose close fails stays out a while for nothing */
	const int64_t until = add_sat(now_ns(),
				      d->cfg.key_quarantine_ms * NS_PER_MS);
	if (u64map_put(&d->retired, r->key, until) ||
	    ring_push(&d->retired_order, until, r->key)) {
		set_err(d, "closing a window's region: out of memory");
		return -ENOMEM;
	}
	const int e = fi_close(&r->mr->fid);
	if (e) {
		set_err(d, "closing a window's region: %s", fi_err(e));
		return to_errno(e);
	}
	r->mr = NULL;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* peers                                                                  */
/* ---------------------------------------------------------------------- */

static void put_peer(struct ofi_rma_ep *d, struct peer *p)
{
	pthread_mutex_lock(&d->peer_mtx);
	if (p->refs)
		p->refs--;
	pthread_mutex_unlock(&d->peer_mtx);
}

/* under peer_mtx */
static void queue_insert_locked(struct ofi_rma_ep *d, struct peer *p)
{
	p->state = PEER_PENDING;
	p->err = 0;
	d->pending++;
	q_push(&d->insert_q, p);
	pthread_cond_signal(&d->insert_cv);
}

static int cmp_used(const void *a, const void *b)
{
	const struct peer *pa = *(struct peer *const *)a;
	const struct peer *pb = *(struct peer *const *)b;
	return pa->used < pb->used ? -1 : pa->used > pb->used;
}

/* make room for a new peer by evicting idle ones; under peer_mtx. False
 * when the table is full of peers in use. */
static bool make_room_locked(struct ofi_rma_ep *d)
{
	if (d->npeers - d->removing < d->cfg.max_peers)
		return true;
	/* the idle peers, least recently used first; a peer being added,
	 * being removed, or held by a write stays */
	struct peer **idle = malloc(d->npeers * sizeof(*idle));
	if (!idle)
		return false;
	size_t n = 0;
	for (size_t b = 0; b < d->nbuckets; b++) {
		for (struct peer *p = d->buckets[b]; p; p = p->hnext) {
			if (p->refs == 0 && (p->state == PEER_READY ||
					     p->state == PEER_FAILED))
				idle[n++] = p;
		}
	}
	qsort(idle, n, sizeof(*idle), cmp_used);
	/* down to 7/8 of the table, so that each new peer does not evict one */
	const size_t keep = d->cfg.max_peers - d->cfg.max_peers / 8 - 1;
	for (size_t i = 0; i < n; i++) {
		if (d->npeers - d->removing <= keep)
			break;
		struct peer *p = idle[i];
		if (p->state == PEER_FAILED) {
			peer_erase(d, p);  /* not in the address vector */
		} else {
			p->state = PEER_REMOVING;
			d->removing++;
			q_push(&d->remove_q, p);
			pthread_cond_signal(&d->insert_cv);
		}
	}
	free(idle);
	return d->npeers - d->removing < d->cfg.max_peers;
}

/* On a FI_THREAD_DOMAIN domain, when a call that began at start stops
 * waiting for an insert: insert_wait_ms after it began, or after the
 * endpoint went quiet, whichever came first. Once an insert has held the
 * endpoint that long, a call gives up on it at once. */
static int64_t insert_give_up(const struct ofi_rma_ep *d, int64_t start)
{
	/* timed per insert, not per call: under load, every call paying the
	 * whole wait would hold up the op threads all the same */
	int64_t from = start;
	if (atomic_load(&d->quiesce))
		from = min64(from, atomic_load(&d->quiet_since_ns));
	return add_sat(from, d->cfg.insert_wait_ms * NS_PER_MS);
}

/* the peer's address, waiting for its insert until deadline, and on a
 * FI_THREAD_DOMAIN domain no longer than insert_give_up(start) (-EBUSY);
 * takes a reference, which put_peer() drops */
static int get_peer(struct ofi_rma_ep *d, const uint8_t *name, size_t len,
		    int64_t deadline, int64_t start, fi_addr_t *out,
		    struct peer **outp)
{
	const uint64_t h = hash_bytes(name, len);
	pthread_mutex_lock(&d->peer_mtx);
	if (d->stopping) {
		pthread_mutex_unlock(&d->peer_mtx);
		return -ESHUTDOWN;
	}
	const int64_t now = now_ns();
	struct peer *p = peer_find(d, name, len, h);
	if (!p) {
		/* a first contact: refused when too many are under way, or
		 * when the table is full of peers in use, so neither grows with
		 * the number of names clients send */
		if (d->pending >= d->cfg.max_pending_inserts ||
		    !make_room_locked(d)) {
			d->inserts_refused++;
			pthread_mutex_unlock(&d->peer_mtx);
			return -EBUSY;
		}
		p = calloc(1, sizeof(*p));
		if (!p) {
			pthread_mutex_unlock(&d->peer_mtx);
			return -ENOMEM;
		}
		memcpy(p->addr_buf, name, len);
		p->name_len = len;
		p->hash = h;
		p->addr = FI_ADDR_NOTAVAIL;
		peer_link(d, p);
		queue_insert_locked(d, p);
	} else if (p->state == PEER_FAILED && now >= p->retry_at) {
		if (d->pending >= d->cfg.max_pending_inserts) {
			d->inserts_refused++;
			pthread_mutex_unlock(&d->peer_mtx);
			return -EBUSY;
		}
		queue_insert_locked(d, p);
	}
	/* the reference keeps the entry while this write waits on it; a peer
	 * being removed is queued again once it is out */
	p->refs++;
	p->used = now;
	/* On a FI_THREAD_DOMAIN domain the inserts run one at a time, each
	 * stopping the endpoint for as long as the provider takes: the
	 * caller, an OSD op thread say, waits for its peer's no longer than
	 * for any other insert, and not behind those queued ahead of it. The
	 * insert goes on, so that a later write finds the peer ready. */
	const int64_t until = d->thread_safe ? deadline :
		min64(deadline, insert_give_up(d, start));
	bool settled;
	for (;;) {
		settled = d->stopping || p->state == PEER_READY ||
			p->state == PEER_FAILED;
		if (settled)
			break;
		if (cond_wait_until(&d->peer_cv, &d->peer_mtx, until) == ETIMEDOUT) {
			settled = d->stopping || p->state == PEER_READY ||
				p->state == PEER_FAILED;
			break;
		}
	}
	if (!settled || p->state != PEER_READY) {
		p->refs--;
		int r;
		if (!settled) {
			if (until < deadline) {
				d->insert_busy++;
				r = -EBUSY;
			} else {
				r = -ETIMEDOUT;
			}
		} else {
			r = p->state == PEER_FAILED ? p->err : -ESHUTDOWN;
		}
		pthread_mutex_unlock(&d->peer_mtx);
		return r;
	}
	*out = p->addr;
	*outp = p;
	pthread_mutex_unlock(&d->peer_mtx);
	return 0;
}

/* remove an evicted peer from the address vector; without peer_mtx */
static void remove_peer(struct ofi_rma_ep *d, struct peer *p)
{
	pthread_mutex_lock(&d->peer_mtx);
	fi_addr_t addr = p->addr;
	pthread_mutex_unlock(&d->peer_mtx);
	/* a FI_THREAD_DOMAIN domain allows no other call meanwhile; a removal
	 * is quick, and no write uses the peer */
	if (!d->thread_safe)
		pthread_mutex_lock(&d->mtx);
	if (!d->broken)
		fi_av_remove(d->av, &addr, 1, 0);
	if (!d->thread_safe)
		pthread_mutex_unlock(&d->mtx);
	pthread_mutex_lock(&d->peer_mtx);
	d->removing--;
	if (p->refs) {
		/* a write wants it again meanwhile */
		p->addr = FI_ADDR_NOTAVAIL;
		queue_insert_locked(d, p);
	} else {
		peer_erase(d, p);
	}
	pthread_mutex_unlock(&d->peer_mtx);
}

static size_t poll_locked(struct ofi_rma_ep *d);

static bool insert_busy_locked(struct ofi_rma_ep *d)
{
	if (d->plans.head)
		return true;
	const int64_t lent = atomic_load(&d->lent_ns);
	return lent && now_ns() < add_sat(lent, d->cfg.lent_wait_ms * NS_PER_MS);
}

/* add a queued peer to the address vector; without peer_mtx */
static void insert_peer(struct ofi_rma_ep *d, struct peer *p)
{
	/* a pending entry is never dropped, so its buffer stays put */
	const bool locked = !d->thread_safe;
	if (locked) {
		/* FI_THREAD_DOMAIN: no other call on the domain may run during
		 * the insert, and the insert can take seconds. Let the writes in
		 * flight finish first, within their budgets, so that the insert
		 * delays none of their cut-offs; writes that start meanwhile,
		 * and calls that cannot get mtx while it is in the provider,
		 * wait for it no longer than insert_give_up(). Let the windows
		 * lent before it come back too, within lent_wait_ms of their
		 * lending, polling so that the peers' pushes into them land:
		 * nothing polls during the insert, and a peer pushing meanwhile
		 * would wait for it. */
		pthread_mutex_lock(&d->mtx);
		atomic_store(&d->quiet_since_ns, now_ns());
		atomic_store(&d->quiesce, true);
		while (insert_busy_locked(d) && !d->stopping) {
			/* reap completions: the writers may all be waiting for
			 * the endpoint now, and a plan whose writer left after
			 * cancelling it waits for its cancellations to complete */
			poll_locked(d);
			if (!insert_busy_locked(d))
				break;
			pthread_mutex_unlock(&d->mtx);
			sleep_ns(NS_PER_MS);
			pthread_mutex_lock(&d->mtx);
		}
	}
	fi_addr_t addr = FI_ADDR_NOTAVAIL;
	int r = d->broken ? -FI_EIO : -FI_ESHUTDOWN;
	if (!d->stopping && !d->broken) {
		/* on a FI_THREAD_DOMAIN domain nothing else runs from here until
		 * the provider returns */
		atomic_store(&d->inserting, locked);
		if (d->hooks.insert)
			d->hooks.insert(d->hooks.ctx, p->addr_buf, p->name_len);
		r = fi_av_insert(d->av, p->addr_buf, 1, &addr, 0, NULL);
		atomic_store(&d->inserting, false);
	}
	if (r != 1) {
		if (r < 0)
			set_err(d, "fi_av_insert: %s", fi_err(r));
		else
			set_err(d, "fi_av_insert: rejected");
	}
	/* under mtx too on a FI_THREAD_DOMAIN domain: writes start again only
	 * once the peer is settled */
	pthread_mutex_lock(&d->peer_mtx);
	d->pending--;
	if (r == 1) {
		p->addr = addr;
		p->state = PEER_READY;
		d->peers_inserted++;
	} else {
		p->state = PEER_FAILED;
		p->err = r < 0 ? to_errno(r) : -EHOSTUNREACH;
		p->retry_at = now_ns() + INSERT_RETRY_NS;
	}
	pthread_mutex_unlock(&d->peer_mtx);
	if (locked) {
		atomic_store(&d->quiesce, false);
		pthread_mutex_unlock(&d->mtx);
	}
	pthread_cond_broadcast(&d->peer_cv);
}

/* an insert thread: removes evicted peers from the address vector and
 * adds queued ones */
static void *insert_loop(void *arg)
{
	struct ofi_rma_ep *d = arg;
	for (;;) {
		struct peer *p;
		bool remove;
		pthread_mutex_lock(&d->peer_mtx);
		while (!d->stopping && !d->remove_q.head && !d->insert_q.head)
			pthread_cond_wait(&d->insert_cv, &d->peer_mtx);
		if (d->stopping) {
			pthread_mutex_unlock(&d->peer_mtx);
			return NULL;
		}
		/* removals first: they are quick, and they make room */
		remove = d->remove_q.head != NULL;
		p = q_pop(remove ? &d->remove_q : &d->insert_q);
		pthread_mutex_unlock(&d->peer_mtx);
		if (remove)
			remove_peer(d, p);
		else
			insert_peer(d, p);
	}
}

/* take mtx for a write that began at start, unless the deadline passes
 * first (-ETIMEDOUT) or an insert on a FI_THREAD_DOMAIN domain keeps the
 * endpoint quiet past insert_give_up(start) (-EBUSY) */
static int lock_for_write(struct ofi_rma_ep *d, int64_t deadline, int64_t start)
{
	if (d->thread_safe) {
		/* inserts there run without mtx, and never keep the endpoint
		 * quiet */
		return mutex_lock_until(&d->mtx, deadline) ? -ETIMEDOUT : 0;
	}
	/* An insert on a FI_THREAD_DOMAIN domain first waits for the writes
	 * in flight, then holds mtx for as long as the provider takes,
	 * seconds perhaps. The writer, an OSD op thread say, has other
	 * clients' work queued behind it: it waits for the insert only
	 * briefly, not for as long as its budget would allow, and the write
	 * is not made. */
	for (;;) {
		const int64_t now = now_ns();
		if (now >= deadline)
			return -ETIMEDOUT;
		if (atomic_load(&d->quiesce)) {
			const int64_t give_up = insert_give_up(d, start);
			if (now >= give_up) {
				d->insert_busy++;
				return -EBUSY;
			}
			sleep_ns(min64(QUIET_POLL_NS,
				       min64(give_up - now, deadline - now)));
			continue;
		}
		/* a slice at a time, to notice an insert that takes mtx
		 * meanwhile */
		if (!mutex_lock_until(&d->mtx,
				      min64(deadline, now + QUIET_POLL_NS))) {
			if (!atomic_load(&d->quiesce))
				return 0;
			pthread_mutex_unlock(&d->mtx);
		}
	}
}

/* take mtx for a call other than a write: as long as that takes, but no
 * longer than insert_give_up() while an insert on a FI_THREAD_DOMAIN
 * domain is in the provider, which holds mtx for as long as it takes.
 * False, and counted, when that ran out. */
static bool lock_briefly(struct ofi_rma_ep *d)
{
	if (d->thread_safe) {
		pthread_mutex_lock(&d->mtx);
		return true;
	}
	const int64_t start = now_ns();
	while (pthread_mutex_trylock(&d->mtx)) {
		/* only the insert itself holds mtx for long; a write, a poll, a
		 * cut-off, or an insert waiting for the writes in flight, is
		 * waited for as ever. Once an insert has held the endpoint past
		 * the wait, a call does not wait for it at all. */
		if (atomic_load(&d->inserting) &&
		    now_ns() >= insert_give_up(d, start)) {
			d->insert_busy++;
			return false;
		}
		if (!mutex_lock_until(&d->mtx, now_ns() + QUIET_POLL_NS))
			break;
	}
	return true;
}

/* ---------------------------------------------------------------------- */
/* completions                                                            */
/* ---------------------------------------------------------------------- */

static void retire_locked(struct ofi_rma_ep *d, struct plan *p)
{
	d->stage_busy[p->slot] = false;
	put_peer(d, p->peer);
	plan_unlink(&d->plans, p);
	plan_free(p);
}

static void complete_locked(struct ofi_rma_ep *d, struct op *op, int err)
{
	struct plan *p = op->plan;
	if (!p || p->outstanding == 0 || op->done)
		return;
	op->done = true;
	p->outstanding--;
	if (err) {
		if (p->reading)
			d->reads_failed++;
		else
			d->writes_failed++;
		if (!p->err)
			p->err = err;
	}
	if (p->outstanding == 0) {
		if (p->abandoned) {
			retire_locked(d, p);  /* its writer left; nobody waits */
			return;
		}
		pthread_cond_broadcast(&d->done_cv);
	}
}

/* one pass over the completion queue; returns the completions read */
static size_t poll_locked(struct ofi_rma_ep *d)
{
	const int64_t now = now_ns();
	if (d->last_poll) {
		d->last_poll_gap = now - d->last_poll;
		const int64_t ms = d->last_poll_gap / NS_PER_MS;
		if (ms >= 1000) {
			atomic_store(&d->last_long_gap_ns, now);
			if ((uint64_t)ms > atomic_load(&d->max_poll_gap_ms))
				atomic_store(&d->max_poll_gap_ms, (uint64_t)ms);
		}
	}
	d->last_poll = now;
	size_t got = 0;
	if (!d->cq || d->zombie)
		return got;
	struct fi_cq_entry ent[16];
	for (int round = 0; round < 8; round++) {
		const ssize_t n = fi_cq_read(d->cq, ent, 16);
		if (n > 0) {
			got += (size_t)n;
			for (ssize_t i = 0; i < n; i++) {
				if (ent[i].op_context)
					complete_locked(d, ent[i].op_context, 0);
			}
			if (n < 16)
				break;
			continue;
		}
		if (n == -FI_EAVAIL) {
			struct fi_cq_err_entry e;
			memset(&e, 0, sizeof(e));
			if (fi_cq_readerr(d->cq, &e, 0) < 0)
				break;
			got++;
			const char *text = fi_cq_strerror(d->cq, e.prov_errno,
							  e.err_data, NULL, 0);
			set_err(d, "%s%s%s%s", fi_err(e.err), text ? " (" : "",
				text ? text : "", text ? ")" : "");
			if (e.op_context)
				complete_locked(d, e.op_context,
						e.err ? to_errno(e.err) : -EIO);
			continue;
		}
		break;  /* -FI_EAGAIN: nothing more, or an error with no entry */
	}
	/* what a provider placed in this pass, for a sync kept from mtx */
	atomic_fetch_add_explicit(&d->polls, 1, memory_order_release);
	return got;
}

/* between two polls of a waiting write: drop the lock until the
 * completion queue has something, or briefly, never past deadline */
static void wait_cq(struct ofi_rma_ep *d, int64_t deadline, int64_t spin_until)
{
	const int64_t now = now_ns();
	const int64_t left = deadline - now;
	if (left <= 0)
		return;
	if (d->cq_fd >= 0) {
		/* the provider signals the wait object when a completion is
		 * ready; fi_trywait() says whether one already is */
		struct fid *f = &d->cq->fid;
		if (fi_trywait(d->fabric, &f, 1) != FI_SUCCESS) {
			/* something to read already; let others at the endpoint
			 * first */
			pthread_mutex_unlock(&d->mtx);
			sched_yield();
			pthread_mutex_lock(&d->mtx);
			return;
		}
		const struct timespec ts = ns_to_ts(min64(left, WAIT_MAX_NS));
		struct pollfd pfd = {d->cq_fd, POLLIN, 0};
		pthread_mutex_unlock(&d->mtx);
		/* a cut-off can close the fd meanwhile; the wait then just runs
		 * out */
		ppoll(&pfd, 1, &ts, NULL);
		pthread_mutex_lock(&d->mtx);
		return;
	}
	/* No wait object: the provider makes progress only while it is
	 * polled (manual progress), or tells nobody when it has. Poll again
	 * soon, but without burning a core on a queue that stays empty. */
	pthread_mutex_unlock(&d->mtx);
	if (now < spin_until)
		sched_yield();
	else
		sleep_ns(min64(left, WAIT_SLEEP_NS));
	pthread_mutex_lock(&d->mtx);
}

/* ---------------------------------------------------------------------- */
/* opening, closing and cut-offs                                          */
/* ---------------------------------------------------------------------- */

static int getopt_bool(struct fid_ep *ep, int opt)
{
	bool b = false;
	size_t blen = sizeof(b);
	if (fi_getopt(&ep->fid, FI_OPT_ENDPOINT, opt, &b, &blen) ||
	    blen != sizeof(b))
		return -1;
	return b ? 1 : 0;
}

/* open the completion queue and the endpoint, bind and enable them, and
 * read the endpoint's name */
static int open_ep(struct ofi_rma_ep *d, char *err, size_t errlen)
{
	struct fi_cq_attr cq_attr;
	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cq_attr.size = 4096;
	/* a completion queue with a file descriptor to wait on, if the
	 * provider has one, so that a waiting write need not poll */
	d->cq_fd = -1;
	cq_attr.wait_obj = FI_WAIT_FD;
	int r = fi_cq_open(d->domain, &cq_attr, &d->cq, NULL);
	if (!r && (fi_control(&d->cq->fid, FI_GETWAIT, &d->cq_fd) ||
		   d->cq_fd < 0)) {
		d->cq_fd = -1;
		fi_close(&d->cq->fid);
		r = -FI_ENOSYS;
	}
	if (r) {
		cq_attr.wait_obj = FI_WAIT_NONE;
		if ((r = fi_cq_open(d->domain, &cq_attr, &d->cq, NULL))) {
			d->cq = NULL;
			fmt_into(err, errlen, "fi_cq_open: %s", fi_err(r));
			return to_errno(r);
		}
	}
	if ((r = fi_endpoint(d->domain, d->info, &d->ep, NULL))) {
		d->ep = NULL;
		fmt_into(err, errlen, "fi_endpoint: %s", fi_err(r));
		return to_errno(r);
	}
	if ((r = fi_ep_bind(d->ep, &d->av->fid, 0)) ||
	    (r = fi_ep_bind(d->ep, &d->cq->fid, FI_TRANSMIT | FI_RECV)) ||
	    (r = fi_enable(d->ep))) {
		fmt_into(err, errlen, "enabling the endpoint: %s", fi_err(r));
		return to_errno(r);
	}
	size_t len = 0;
	r = fi_getname(&d->ep->fid, NULL, &len);
	if (r != -FI_ETOOSMALL || len == 0 || len > OFI_RMA_ADDR_MAX) {
		fmt_into(err, errlen, "fi_getname: %s",
			 r ? fi_err(r) : "bad length");
		return -EINVAL;
	}
	uint8_t name[OFI_RMA_ADDR_MAX];
	if ((r = fi_getname(&d->ep->fid, name, &len))) {
		fmt_into(err, errlen, "fi_getname: %s", fi_err(r));
		return to_errno(r);
	}
	pthread_mutex_lock(&d->name_mtx);
	memcpy(d->my_name, name, len);
	d->my_name_len = len;
	pthread_mutex_unlock(&d->name_mtx);
	/* what the provider promises about discarding writes */
	atomic_store(&d->close_discards, getopt_bool(d->ep, OPT_CLOSE_DISCARDS));
	atomic_store(&d->cancel_discards, d->cfg.per_plan_cutoff &&
		     (d->hooks.cancel ||
		      getopt_bool(d->ep, OPT_CANCEL_DISCARDS) == 1));
	return 0;
}

/* cancel the operations of a plan that have not completed; *simulated
 * when a test's cancel hook stood for the provider. 0, or the libfabric
 * error of the first cancel that failed. */
static int cancel_plan_locked(struct ofi_rma_ep *d, struct plan *p,
			      bool *simulated)
{
	*simulated = false;
	for (size_t i = 0; i < p->n_ops; i++) {
		struct op *op = &p->ops[i];
		if (op->done)
			continue;
		int r;
		if (d->hooks.cancel) {
			r = d->hooks.cancel(d->hooks.ctx);
			*simulated = true;
		} else {
			r = (int)fi_cancel(&d->ep->fid, &op->ctx);
			if (r == -FI_ENOENT)
				r = 0;  /* no longer outstanding: completing */
		}
		if (r)
			return r;
	}
	return 0;
}

/* close the endpoint and its completion queue, and free what only the
 * endpoint still held; on failure the endpoint stays open, a zombie
 * nothing calls into again */
static int close_ep_locked(struct ofi_rma_ep *d)
{
	if (d->ep) {
		int r = d->hooks.cutoff_close ?
			d->hooks.cutoff_close(d->hooks.ctx) : 0;
		if (r == 0)
			r = fi_close(&d->ep->fid);
		if (r) {
			d->zombie = true;
			return r;
		}
		d->ep = NULL;
	}
	if (d->cq) {
		const int r = fi_close(&d->cq->fid);
		if (r) {
			/* the endpoint is closed, so its writes are cut off; the
			 * queue only stays allocated */
			set_err(d, "closing the completion queue in a cut-off: %s",
				fi_err(r));
		}
		d->cq = NULL;
		d->cq_fd = -1;
	}
	/* nothing references the operations of plans whose writers left any
	 * more, nor the staging they read */
	for (struct plan *p = d->plans.head, *next; p; p = next) {
		next = p->next;
		if (p->abandoned) {
			p->outstanding = 0;
			retire_locked(d, p);
		}
	}
	for (struct plan *p = d->hook_cancelled.head; p; p = p->next) {
		if (!p->released) {
			p->released = true;
			d->stage_busy[p->slot] = false;
			put_peer(d, p->peer);
		}
	}
	return 0;
}

/* a cut-off was too late: take no more writes, close the endpoint to stop
 * the writes still in flight, and fail those whose budget ran out with
 * -ENOTRECOVERABLE */
static void stop_unsafe_locked(struct ofi_rma_ep *d, const char *why)
{
	atomic_store(&d->unsafe, true);
	atomic_store(&d->broken, true);
	set_err(d, "%s", why);
	if (close_ep_locked(d)) {
		/* the endpoint stays open, and its writes may land at any time */
		for (struct plan *p = d->plans.head; p; p = p->next) {
			if (p->outstanding) {
				p->outstanding = 0;
				p->err = -ENOTRECOVERABLE;
			}
		}
	} else {
		/* closed: the writes in flight are cut off now, late for a plan
		 * whose budget already ran out */
		const int64_t cut = now_ns();
		for (struct plan *p = d->plans.head; p; p = p->next) {
			if (p->outstanding) {
				p->outstanding = 0;
				p->err = cut > p->budget_end ? -ENOTRECOVERABLE :
					p->err ? p->err : -ECANCELED;
			}
		}
	}
	pthread_cond_broadcast(&d->done_cv);
}

/* a cut-off went wrong: fail every waiting plan with -ENOTRECOVERABLE and
 * take no more writes */
static void make_unsafe_locked(struct ofi_rma_ep *d, const char *why)
{
	atomic_store(&d->unsafe, true);
	atomic_store(&d->broken, true);
	set_err(d, "%s", why);
	for (struct plan *p = d->plans.head; p; p = p->next) {
		if (p->outstanding) {
			p->outstanding = 0;
			p->err = -ENOTRECOVERABLE;
		}
	}
	pthread_cond_broadcast(&d->done_cv);
}

/* fold a clean cut-off's duration into cutoff_cost_ms */
static void note_cutoff_cost(struct ofi_rma_ep *d, int64_t took_ns)
{
	const int64_t ms = took_ns / NS_PER_MS;
	const int64_t sample = min64(2 * ms + 20, d->cfg.cutoff_cost_max_ms);
	const int64_t cur = atomic_load(&d->cutoff_cost_ms);
	atomic_store(&d->cutoff_cost_ms,
		     sample >= cur ? sample : cur - (cur - sample) / 4);
}

/* judge a cut-off that ran from t0 to cut against the earliest budget end
 * of the plans it cut: record lateness, and say why in the last error */
static enum verdict judge_cutoff_locked(struct ofi_rma_ep *d, int64_t t0,
					int64_t cut, int64_t budget_end)
{
	const int64_t took = cut - t0;
	const int64_t late = budget_end == INT64_MAX ? 0 : cut - budget_end;
	const int64_t late_ms = late / NS_PER_MS;
	if (late > 0) {
		d->cutoffs_late++;
		if (late_ms > 0 &&
		    (uint64_t)late_ms > atomic_load(&d->max_lateness_ms))
			atomic_store(&d->max_lateness_ms, (uint64_t)late_ms);
		atomic_store(&d->last_late_ns, cut);
	}
	char what[512];
	int n = snprintf(what, sizeof(what),
			 "a cut-off took %lld ms and ended %lld ms after the "
			 "budget it protected", (long long)(took / NS_PER_MS),
			 (long long)late_ms);
	if (late > 0 && n > 0 && (size_t)n < sizeof(what)) {
		const int64_t started_ms = d->notice_delay / NS_PER_MS;
		const int64_t gap_ms = d->last_poll_gap / NS_PER_MS;
		n += snprintf(what + n, sizeof(what) - (size_t)n,
			      "; it started %lld ms after its writes' deadline",
			      (long long)started_ms);
		if (gap_ms > 0 && gap_ms + 50 >= started_ms &&
		    (size_t)n < sizeof(what)) {
			snprintf(what + n, sizeof(what) - (size_t)n,
				 ", and nothing polled the endpoint for the "
				 "%lld ms before: the process, or its threads, "
				 "did not run (stopped, paused, swapped out or "
				 "starved)", (long long)gap_ms);
		}
	}
	const long long tolerance = (long long)d->cfg.late_tolerance_ms;
	if (took > d->cfg.late_tolerance_ms * NS_PER_MS) {
		/* the provider itself takes that long to cut writes off, as one
		 * that drains them instead of discarding does: every cut-off
		 * would be late */
		set_err(d, "%s. The close or cancel itself took longer than "
			"the tolerance of %lld ms; writes may have landed late",
			what, tolerance);
		return UNSAFE;
	}
	if (late > d->cfg.late_tolerance_ms * NS_PER_MS) {
		/* started late; the endpoint is clean now, since the close or
		 * cancel returned */
		d->cutoffs_past_tolerance++;
		atomic_store(&d->last_past_ns, cut);
		set_err(d, "%s, beyond the tolerance of %lld ms; writes may "
			"have landed late%s", what, tolerance,
			d->cfg.late_fail_closed ? "" : ", and the endpoint goes on");
		return d->cfg.late_fail_closed ? UNSAFE : PAST_TOLERANCE;
	}
	if (late > 0) {
		set_err(d, "%s, within the tolerance of %lld ms", what, tolerance);
		return LATE;
	}
	return IN_TIME;
}

/* cut off every write in flight by closing and reopening the endpoint;
 * plans still waiting fail with -ECANCELED, or with -ENOTRECOVERABLE when
 * the cut-off failed or ended after their budget */
static void reset_locked(struct ofi_rma_ep *d)
{
	const int64_t t0 = now_ns();
	d->resets++;
	/* Cut off: closing the endpoint cancels every operation it still has
	 * outstanding, so none of them retries into a peer's window later.
	 * The completion queue goes too, with any completions of those
	 * operations, whose contexts are about to be released. */
	const int cr = close_ep_locked(d);
	if (cr) {
		/* the endpoint is still open, and so are its writes: they were
		 * not cut off and can land at any time */
		d->cutoffs_failed++;
		char why[256];
		snprintf(why, sizeof(why), "a cut-off failed: closing the "
			 "endpoint: %s; its writes may still land", fi_err(cr));
		make_unsafe_locked(d, why);
		return;
	}
	/* Every plan with writes in flight has lost them, and its writer has
	 * to hear it. One with none in flight has lost nothing: it is done,
	 * or its writer is still gathering or posting and goes on with the
	 * new endpoint. A plan whose budget ran out before the writes were
	 * cut off may have had some land after it. */
	const int64_t cut = now_ns();
	int64_t earliest = INT64_MAX;
	for (struct plan *p = d->plans.head; p; p = p->next) {
		if (p->outstanding)
			earliest = min64(earliest, p->budget_end);
	}
	const enum verdict v = judge_cutoff_locked(d, t0, cut, earliest);
	const bool unsafe_now = v == UNSAFE;
	for (struct plan *p = d->plans.head; p; p = p->next) {
		if (p->outstanding == 0)
			continue;
		p->outstanding = 0;
		if (unsafe_now && cut > p->budget_end)
			p->err = -ENOTRECOVERABLE;
		else if (p->due || cut > p->budget_end)
			p->err = -ETIMEDOUT;
		else if (!p->err)
			p->err = -ECANCELED;
		p->due = false;
	}
	pthread_cond_broadcast(&d->done_cv);
	if (unsafe_now) {
		/* closed, and not opened again: take no more writes */
		atomic_store(&d->unsafe, true);
		atomic_store(&d->broken, true);
		return;
	}
	/* regions bound to the old endpoint are gone with it */
	const bool rebind = d->mr_mode & FI_MR_ENDPOINT;
	if (rebind) {
		for (size_t i = 0; i < d->nwin; i++)
			close_window_region_locked(d, &d->windows[i].r);
		if (d->stage_mr.mr) {
			fi_close(&d->stage_mr.mr->fid);
			d->stage_mr.mr = NULL;
		}
	}
	char err[256];
	if (open_ep(d, err, sizeof(err)) < 0) {
		atomic_store(&d->broken, true);
		set_err(d, "reopening after a cut-off: %s", err);
		return;
	}
	if (rebind) {
		for (size_t i = 0; i < d->nwin; i++) {
			struct region *w = &d->windows[i].r;
			struct region fresh;
			if (reg_window_locked(d, w->ptr, w->len, &w->mem,
					      w->access, &fresh) < 0) {
				atomic_store(&d->broken, true);
				return;
			}
			*w = fresh;
		}
		if (d->stage) {
			struct region fresh;
			const uint64_t acc = FI_WRITE | (d->reads ? FI_READ : 0);
			if (reg(d, d->stage, d->cfg.stage_size * d->cfg.stage_count,
				acc, &fresh, NULL) < 0) {
				atomic_store(&d->broken, true);
				return;
			}
			d->stage_mr = fresh;
		}
	}
	note_cutoff_cost(d, now_ns() - t0);
}

/* whether a plan of this many bytes can still be posted and complete
 * before deadline, by plan_rate */
static bool time_to_post_locked(const struct ofi_rma_ep *d, uint64_t bytes,
				int64_t deadline)
{
	int64_t need = POST_MARGIN_NS;
	if (d->plan_rate > 0)
		need += (int64_t)((double)(2 * bytes) / d->plan_rate);
	return now_ns() + need < deadline;
}

/* fold how long after a plan's deadline its cut-off started into the
 * scheduling slack */
static void note_notice_delay(struct ofi_rma_ep *d, int64_t delay_ns)
{
	/* twice the delay plus a little, at once when that is more, a
	 * quarter of the way when it is less: as for the cut-off's cost */
	const int64_t ms = max64(0, delay_ns / NS_PER_MS);
	const int64_t sample = min64(max64(2 * ms + 5, SLACK_MIN_MS),
				     d->cfg.cutoff_cost_max_ms);
	const int64_t cur = atomic_load(&d->cutoff_slack_ms);
	atomic_store(&d->cutoff_slack_ms,
		     sample >= cur ? sample : cur - (cur - sample) / 4);
}

/* cut a plan's writes off: cancel them when the provider discards a
 * cancelled write, else reset the endpoint */
static void cut_off_locked(struct ofi_rma_ep *d, struct plan *p,
			   int64_t noticed)
{
	d->timeouts++;
	d->notice_delay = noticed - p->deadline;
	note_notice_delay(d, d->notice_delay);
	const int64_t t0 = now_ns();
	if (atomic_load(&d->cancel_discards)) {
		/* only this write's operations, if the provider discards them */
		bool simulated = false;
		const int r = cancel_plan_locked(d, p, &simulated);
		const int64_t cut = now_ns();
		if (r == 0) {
			d->plans_cut_off++;
			p->cut = true;
			if (simulated) {
				/* the provider still holds the operations: keep
				 * them, and the staging they read, until the
				 * endpoint closes */
				p->hooked = true;
				p->outstanding = 0;
				plan_unlink(&d->plans, p);
				plan_link_tail(&d->hook_cancelled, p);
			}
			if (judge_cutoff_locked(d, t0, cut, p->budget_end) ==
			    UNSAFE) {
				p->cut_result = cut > p->budget_end ?
					-ENOTRECOVERABLE : -ETIMEDOUT;
				char why[ERR_MAX];
				get_err(d, why, sizeof(why));
				stop_unsafe_locked(d, why);
				return;
			}
			p->cut_result = -ETIMEDOUT;
			note_cutoff_cost(d, cut - t0);
			pthread_cond_broadcast(&d->done_cv);
			return;
		}
		/* the provider kept the operations: close the endpoint
		 * instead, which discards them all */
		d->cancels_failed++;
		set_err(d, "cancelling a late write failed: %s; cutting it off "
			"by closing the endpoint", fi_err(r));
	} else {
		set_err(d, "writes still in flight at the deadline; cut off");
	}
	p->due = true;
	reset_locked(d);
}

/* cut off every plan whose deadline passed, on its writer's behalf */
static void cut_due_locked(struct ofi_rma_ep *d)
{
	const int64_t now = now_ns();
	/* one at a time: a reset for an earlier one may take later ones too,
	 * and a cut-off moves a plan only out of the list */
	for (;;) {
		if (d->broken)
			return;
		struct plan *due = NULL;
		for (struct plan *p = d->plans.head; p; p = p->next) {
			if (p->outstanding && !p->cut && !p->abandoned &&
			    now > p->deadline) {
				due = p;
				break;
			}
		}
		if (!due)
			return;
		d->cutoffs_on_behalf++;
		cut_off_locked(d, due, now);
	}
}

/* ---------------------------------------------------------------------- */
/* open and close                                                         */
/* ---------------------------------------------------------------------- */

void ofi_rma_config_init(struct ofi_rma_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->size = sizeof(*cfg);
	cfg->thread_safe = true;
	cfg->cutoff_cost_initial_ms = 100;
	cfg->cutoff_cost_max_ms = 2000;
	cfg->late_tolerance_ms = 1000;
	cfg->key_quarantine_ms = 10000;
	cfg->mr_cnt = 16384;
	cfg->max_peers = 1024;
	cfg->max_pending_inserts = 64;
	cfg->insert_threads = 4;
	cfg->insert_wait_ms = 10;
	cfg->lent_wait_ms = 0;
	cfg->per_plan_cutoff = true;
}

void ofi_rma_memory_init(struct ofi_rma_memory *mem)
{
	memset(mem, 0, sizeof(*mem));
	mem->iface = OFI_RMA_IFACE_SYSTEM;
	mem->dmabuf_fd = -1;
}

static void ep_free(struct ofi_rma_ep *d)
{
	pthread_mutex_lock(&d->peer_mtx);
	atomic_store(&d->stopping, true);
	pthread_mutex_unlock(&d->peer_mtx);
	pthread_cond_broadcast(&d->insert_cv);
	pthread_cond_broadcast(&d->peer_cv);
	if (d->progress_started)
		pthread_join(d->progress_thr, NULL);
	/* waits for inserts still inside the provider */
	for (unsigned i = 0; i < d->n_insert_thrs; i++)
		pthread_join(d->insert_thrs[i], NULL);
	/* verbs;ofi_rxm only: read what the completion queue still holds
	 * before closing. ofi_rxm in libfabric 2.3.1 closes a connection's
	 * verbs endpoint before it drains the queue, and a credit completion
	 * found then reposts a receive on the closed endpoint, which crashes
	 * the process (fixed upstream in 697688386). Polling first lets the
	 * provider handle those completions while its connections are still
	 * open. Not for a provider that places incoming writes in software
	 * when it is polled, such as tcp: the application may already have
	 * freed a window's memory, and closing without polling discards what
	 * is still arriving. The verbs NIC places RMA writes whether polled
	 * or not. Bounded. */
	if (d->ep && d->cq && !d->zombie && d->prov &&
	    !strncmp(d->prov, "verbs;", 6)) {
		pthread_mutex_lock(&d->mtx);
		const int64_t until = now_ns() + DRAIN_BEFORE_CLOSE_NS;
		int idle = 0;
		while (idle < 3 && now_ns() < until)
			idle = poll_locked(d) ? 0 : idle + 1;
		pthread_mutex_unlock(&d->mtx);
	}
	/* the endpoint first: it cancels whatever is still in flight, after
	 * which no write references the regions */
	if (d->ep && fi_close(&d->ep->fid)) {
		/* still open after a failed cut-off, and it may still read the
		 * staging buffer or write the windows, or complete operations
		 * whose contexts plans hold: leave all of it */
		return;
	}
	for (size_t i = 0; i < d->nwin; i++) {
		/* a window loses its region when re-registering it after a
		 * cut-off failed */
		if (d->windows[i].r.mr)
			fi_close(&d->windows[i].r.mr->fid);
	}
	if (d->stage_mr.mr)
		fi_close(&d->stage_mr.mr->fid);
	free(d->stage);
	if (d->cq)
		fi_close(&d->cq->fid);
	if (d->av)
		fi_close(&d->av->fid);
	if (d->domain)
		fi_close(&d->domain->fid);
	if (d->fabric)
		fi_close(&d->fabric->fid);
	if (d->info)
		fi_freeinfo(d->info);

	for (struct plan *p = d->plans.head, *next; p; p = next) {
		next = p->next;
		plan_free(p);
	}
	for (struct plan *p = d->hook_cancelled.head, *next; p; p = next) {
		next = p->next;
		plan_free(p);
	}
	if (d->buckets) {
		for (size_t b = 0; b < d->nbuckets; b++) {
			for (struct peer *p = d->buckets[b], *next; p; p = next) {
				next = p->hnext;
				free(p);
			}
		}
		free(d->buckets);
	}
	free(d->insert_thrs);
	free(d->windows);
	free(d->stage_busy);
	u64map_free(&d->retired);
	ring_free(&d->retired_order);
	free(d->prov);
	free(d->s_provider);
	free(d->s_domain);
	free(d->s_node);
	free(d->s_service);
	pthread_cond_destroy(&d->done_cv);
	pthread_cond_destroy(&d->insert_cv);
	pthread_cond_destroy(&d->peer_cv);
	pthread_mutex_destroy(&d->peer_mtx);
	pthread_mutex_destroy(&d->err_mtx);
	pthread_mutex_destroy(&d->name_mtx);
	pthread_mutex_destroy(&d->mtx);
	free(d);
}

void ofi_rma_ep_close(struct ofi_rma_ep *ep)
{
	if (ep)
		ep_free(ep);
}

static void *progress_loop(void *arg)
{
	struct ofi_rma_ep *d = arg;
	while (!atomic_load(&d->stopping)) {
		pthread_mutex_lock(&d->mtx);
		poll_locked(d);
		cut_due_locked(d);
		pthread_mutex_unlock(&d->mtx);
		sleep_ns(50 * NS_PER_US);
	}
	return NULL;
}

static char *dup_opt(const char *s)
{
	return s && *s ? strdup(s) : NULL;
}

/* the wire protocol an endpoint speaks: libfabric's name for its
 * protocol, and the version */
static void derive_wire(struct ofi_rma_ep *d)
{
	const struct fi_info *info = d->info;
	const uint32_t proto = (uint32_t)info->ep_attr->protocol;
	const uint32_t ver = info->ep_attr->protocol_version;
	char buf[32];
	const char *name = proto_name(proto);
	/* UEC 1.0 has every UET provider report the fabric name "UET" and
	 * FI_PROTO_UET, which libfabric does not define yet: until it does,
	 * the fabric name says it */
	if (!name && info->fabric_attr->name &&
	    !strcasecmp(info->fabric_attr->name, "UET"))
		name = "uet";
	if (!name && proto != FI_PROTO_UNSPEC) {
		snprintf(buf, sizeof(buf), "proto%x", proto);
		name = buf;
	}
	if (!name) {
		/* a provider that names no protocol speaks its own */
		size_t n = 0;
		const char *p = d->prov ? d->prov : "";
		n = (size_t)snprintf(buf, sizeof(buf), "prov_");
		for (; *p && n < 31; p++) {
			const unsigned char c = (unsigned char)*p;
			buf[n++] = isalnum(c) ? (char)tolower(c) : '_';
		}
		buf[n] = '\0';
		name = buf;
	}
	snprintf(d->wire, sizeof(d->wire), "%s.%x", name, ver);
}

int ofi_rma_ep_open(const struct ofi_rma_config *cfg_in,
		    struct ofi_rma_ep **out, char *err, size_t errlen)
{
	if (err && errlen)
		err[0] = '\0';
	if (!cfg_in || !out || cfg_in->size <
	    offsetof(struct ofi_rma_config, provider) + sizeof(char *)) {
		fmt_into(err, errlen, "no configuration");
		return -EINVAL;
	}
	*out = NULL;
	struct ofi_rma_ep *d = calloc(1, sizeof(*d));
	if (!d) {
		fmt_into(err, errlen, "out of memory");
		return -ENOMEM;
	}
	ofi_rma_config_init(&d->cfg);
	memcpy(&d->cfg, cfg_in,
	       cfg_in->size < sizeof(d->cfg) ? cfg_in->size : sizeof(d->cfg));
	d->cfg.size = sizeof(d->cfg);
	if (d->cfg.hooks)
		d->hooks = *d->cfg.hooks;
	d->cfg.hooks = NULL;
	d->s_provider = dup_opt(d->cfg.provider);
	d->s_domain = dup_opt(d->cfg.domain);
	d->s_node = dup_opt(d->cfg.node);
	d->s_service = dup_opt(d->cfg.service);
	d->cfg.provider = d->s_provider;
	d->cfg.domain = d->s_domain;
	d->cfg.node = d->s_node;
	d->cfg.service = d->s_service;
	pthread_mutex_init(&d->mtx, NULL);
	pthread_mutex_init(&d->name_mtx, NULL);
	pthread_mutex_init(&d->err_mtx, NULL);
	pthread_mutex_init(&d->peer_mtx, NULL);
	cond_init_monotonic(&d->peer_cv);
	cond_init_monotonic(&d->insert_cv);
	cond_init_monotonic(&d->done_cv);
	d->cq_fd = -1;
	d->next_key = 1;
	d->max_write = MAX_WRITE;
	d->poll_floor = INT64_MAX;
	atomic_store(&d->close_discards, -1);
	atomic_store(&d->rekey_in_place, -1);
	atomic_store(&d->cutoff_slack_ms, SLACK_INITIAL_MS);
	atomic_store(&d->cutoff_cost_ms, min64(d->cfg.cutoff_cost_initial_ms,
					       d->cfg.cutoff_cost_max_ms));
	d->nbuckets = 64;
	while (d->nbuckets < d->cfg.max_peers && d->nbuckets < (1u << 20))
		d->nbuckets *= 2;
	d->buckets = calloc(d->nbuckets, sizeof(*d->buckets));
	if (!d->buckets) {
		fmt_into(err, errlen, "out of memory");
		ep_free(d);
		return -ENOMEM;
	}
	if (!d->s_provider) {
		fmt_into(err, errlen, "no libfabric provider named");
		ep_free(d);
		return -EINVAL;
	}

	struct fi_info *hints = fi_allocinfo();
	if (!hints) {
		fmt_into(err, errlen, "fi_allocinfo failed");
		ep_free(d);
		return -ENOMEM;
	}
	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_RMA | FI_WRITE | FI_REMOTE_WRITE;
	hints->mode = FI_CONTEXT | FI_CONTEXT2;
	hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR |
		FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT;
	hints->fabric_attr->prov_name = strdup(d->s_provider);
	if (d->s_domain)
		hints->domain_attr->name = strdup(d->s_domain);
	const char *node = d->s_node;
	const char *service = d->s_service;
	const uint64_t flags = (node || service) ? FI_SOURCE : 0;
	/* Ask for completions that mean "placed in the target's memory"; a
	 * provider that cannot promise it is still usable, see
	 * ofi_rma_ep_delivery_complete(). Ask for a thread-safe domain too,
	 * so that a slow address insert holds up no other write; that
	 * matters less, so a provider without one is preferred to giving up
	 * delivery-complete.
	 *
	 * Device memory, when asked for, matters more than either: an owner
	 * that asks for it lends windows on a GPU, and would otherwise have
	 * to stage every transfer through host memory. Without it, the
	 * windows are in host memory only (see ofi_rma_ep_hmem()).
	 *
	 * Reads, when asked for, come next: an endpoint that cannot read
	 * cannot pull a PUT's payload at all, while one without
	 * delivery-complete writes or a thread-safe domain only fences and
	 * inserts less well. */
	int r = -FI_ENODATA;
	bool asked_reads = false;
	const bool hmem_tries[2] = {d->cfg.hmem, false};
	const bool read_tries[2] = {d->cfg.reads, false};
	const uint64_t op_tries[2] = {FI_DELIVERY_COMPLETE, 0};
	const enum fi_threading thr_tries[2] = {FI_THREAD_SAFE, FI_THREAD_DOMAIN};
	for (int hi = 0; hi < 2; hi++) {
		const bool hmem = hmem_tries[hi];
		if (hmem) {
			hints->caps |= FI_HMEM;
			hints->domain_attr->mr_mode |= FI_MR_HMEM;
		} else {
			hints->caps &= ~FI_HMEM;
			hints->domain_attr->mr_mode &= ~FI_MR_HMEM;
		}
		for (int ri = 0; ri < 2; ri++) {
			const bool reads = read_tries[ri];
			if (reads)
				hints->caps |= FI_READ | FI_REMOTE_READ;
			else
				hints->caps &= ~(uint64_t)(FI_READ | FI_REMOTE_READ);
			asked_reads = reads;
			for (int oi = 0; oi < 2; oi++) {
				for (int ti = 0; ti < 2; ti++) {
					if (thr_tries[ti] == FI_THREAD_SAFE &&
					    !d->cfg.thread_safe)
						continue;
					hints->tx_attr->op_flags = op_tries[oi];
					hints->domain_attr->threading = thr_tries[ti];
					r = fi_getinfo(API_VERSION, node, service,
						       flags, hints, &d->info);
					if (r != -FI_ENODATA)
						break;
				}
				if (r != -FI_ENODATA)
					break;
			}
			if (r != -FI_ENODATA || !reads)
				break;
		}
		if (r != -FI_ENODATA || !hmem)
			break;
	}
	if (!r && d->cfg.mr_cnt && d->info->domain_attr->mr_cnt &&
	    d->info->domain_attr->mr_cnt < d->cfg.mr_cnt) {
		/* a provider with a fixed table of regions: ask for a larger
		 * one, with the same attributes, and keep what it gave if it
		 * will not */
		hints->domain_attr->mr_cnt = d->cfg.mr_cnt;
		struct fi_info *bigger = NULL;
		if (fi_getinfo(API_VERSION, node, service, flags, hints,
			       &bigger) == 0) {
			fi_freeinfo(d->info);
			d->info = bigger;
		}
	}
	fi_freeinfo(hints);
	if (r) {
		fmt_into(err, errlen, "fi_getinfo(%s): %s", d->s_provider,
			 fi_err(r));
		d->info = NULL;
		ep_free(d);
		return to_errno(r);
	}
	struct fi_info *info = d->info;
	d->prov = strdup(info->fabric_attr->prov_name ?
			 info->fabric_attr->prov_name : "");
	d->mr_mode = info->domain_attr->mr_mode;
	d->dc = info->tx_attr->op_flags & FI_DELIVERY_COMPLETE;
	d->hmem = info->caps & FI_HMEM;
	d->reads = asked_reads && (info->caps & FI_READ) &&
		(info->caps & FI_REMOTE_READ);
	d->thread_safe = info->domain_attr->threading == FI_THREAD_SAFE;
	if (info->ep_attr->max_msg_size)
		d->max_write = minu64(MAX_WRITE, info->ep_attr->max_msg_size);
	if (!d->prov) {
		fmt_into(err, errlen, "out of memory");
		ep_free(d);
		return -ENOMEM;
	}
	if ((d->mr_mode & FI_MR_RAW) || info->domain_attr->mr_key_size > 8) {
		fmt_into(err, errlen, "%s needs raw memory keys, which tokens "
			 "cannot carry", d->prov);
		ep_free(d);
		return -EOPNOTSUPP;
	}
	derive_wire(d);

	if ((r = fi_fabric(info->fabric_attr, &d->fabric, NULL))) {
		fmt_into(err, errlen, "fi_fabric: %s", fi_err(r));
		d->fabric = NULL;
		ep_free(d);
		return to_errno(r);
	}
	if ((r = fi_domain(d->fabric, info, &d->domain, NULL))) {
		fmt_into(err, errlen, "fi_domain: %s", fi_err(r));
		d->domain = NULL;
		ep_free(d);
		return to_errno(r);
	}
	struct fi_av_attr av_attr;
	memset(&av_attr, 0, sizeof(av_attr));
	av_attr.type = info->domain_attr->av_type != FI_AV_UNSPEC ?
		info->domain_attr->av_type : FI_AV_TABLE;
	av_attr.count = 0;  /* the provider's default; shm caps it below
			     * max_peers */
	if ((r = fi_av_open(d->domain, &av_attr, &d->av, NULL))) {
		fmt_into(err, errlen, "fi_av_open: %s", fi_err(r));
		d->av = NULL;
		ep_free(d);
		return to_errno(r);
	}
	if ((r = open_ep(d, err, errlen)) < 0) {
		ep_free(d);
		return r;
	}

	if (d->cfg.stage_size && d->cfg.stage_count) {
		const size_t total = d->cfg.stage_size * d->cfg.stage_count;
		void *p = NULL;
		if (posix_memalign(&p, 4096, total) != 0 || !p) {
			fmt_into(err, errlen, "cannot allocate %zu staging bytes",
				 total);
			ep_free(d);
			return -ENOMEM;
		}
		d->stage = p;
		const uint64_t acc = FI_WRITE | (d->reads ? FI_READ : 0);
		if ((r = reg(d, d->stage, total, acc, &d->stage_mr, NULL))) {
			char e[ERR_MAX];
			get_err(d, e, sizeof(e));
			fmt_into(err, errlen, "registering staging: %s", e);
			ep_free(d);
			return r;
		}
		d->stage_busy = calloc(d->cfg.stage_count, sizeof(bool));
		const unsigned n = d->thread_safe ?
			(d->cfg.insert_threads ? d->cfg.insert_threads : 1) : 1;
		d->insert_thrs = calloc(n, sizeof(pthread_t));
		if (!d->stage_busy || !d->insert_thrs) {
			fmt_into(err, errlen, "out of memory");
			ep_free(d);
			return -ENOMEM;
		}
		/* a writer adds peers to the address vector on threads of the
		 * endpoint's own */
		for (unsigned i = 0; i < n; i++) {
			if (pthread_create(&d->insert_thrs[i], NULL, insert_loop,
					   d)) {
				fmt_into(err, errlen, "cannot start an insert "
					 "thread");
				ep_free(d);
				return -EAGAIN;
			}
			d->n_insert_thrs++;
		}
	}

	if (d->cfg.progress_thread) {
		if (pthread_create(&d->progress_thr, NULL, progress_loop, d)) {
			fmt_into(err, errlen, "cannot start the progress thread");
			ep_free(d);
			return -EAGAIN;
		}
		d->progress_started = true;
	}
	*out = d;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* accessors                                                              */
/* ---------------------------------------------------------------------- */

const char *ofi_rma_ep_provider(const struct ofi_rma_ep *ep)
{
	return ep->prov;
}

const char *ofi_rma_ep_wire(const struct ofi_rma_ep *ep)
{
	return ep->wire;
}

size_t ofi_rma_ep_name(struct ofi_rma_ep *ep, void *buf, size_t buflen)
{
	pthread_mutex_lock(&ep->name_mtx);
	const size_t len = ep->my_name_len;
	if (buf)
		memcpy(buf, ep->my_name, len < buflen ? len : buflen);
	pthread_mutex_unlock(&ep->name_mtx);
	return len;
}

bool ofi_rma_ep_delivery_complete(const struct ofi_rma_ep *ep)
{
	return ep->dc;
}

bool ofi_rma_ep_hmem(const struct ofi_rma_ep *ep)
{
	return ep->hmem;
}

bool ofi_rma_ep_reads(const struct ofi_rma_ep *ep)
{
	return ep->reads;
}

size_t ofi_rma_ep_describe(const struct ofi_rma_ep *ep, char *buf,
			   size_t buflen)
{
	const struct fi_info *info = ep->info;
	char s[1024];
	int n = snprintf(s, sizeof(s), "provider %s, wire %s", ep->prov,
			 ep->wire);
#define ADD(...) \
	do { \
		if (n >= 0 && (size_t)n < sizeof(s)) \
			n += snprintf(s + n, sizeof(s) - (size_t)n, __VA_ARGS__); \
	} while (0)
	if (info->fabric_attr->name)
		ADD(", fabric %s", info->fabric_attr->name);
	if (info->domain_attr->name)
		ADD(", domain %s", info->domain_attr->name);
	ADD("%s", (ep->mr_mode & FI_MR_VIRT_ADDR) ? ", virtual-address regions" :
	    ", offset regions");
	ADD("%s", ep->dc ? ", delivery-complete writes" :
	    ", transmit-complete writes");
	if (ep->hmem)
		ADD(", device-memory windows");
	if (ep->reads)
		ADD(", reads");
	if (ep->stage) {
		ADD("%s", ep->thread_safe ? ", concurrent peer inserts" :
		    ", peer inserts pause writes");
		ADD("%s", atomic_load(&ep->cancel_discards) ?
		    ", late writes cancelled one by one" :
		    ", late writes cut off by reopening the endpoint");
		if (atomic_load(&ep->close_discards) == 0)
			ADD(", closing does not discard writes");
	}
#undef ADD
	const size_t len = strlen(s);
	if (buflen) {
		const size_t c = len < buflen ? len : buflen - 1;
		memcpy(buf, s, c);
		buf[c] = '\0';
	}
	return len;
}

size_t ofi_rma_ep_last_error(struct ofi_rma_ep *ep, char *buf, size_t buflen)
{
	pthread_mutex_lock(&ep->err_mtx);
	const size_t len = strlen(ep->last_err);
	if (buflen) {
		const size_t c = len < buflen ? len : buflen - 1;
		memcpy(buf, ep->last_err, c);
		buf[c] = '\0';
	}
	pthread_mutex_unlock(&ep->err_mtx);
	return len;
}

bool ofi_rma_ep_unsafe(const struct ofi_rma_ep *ep)
{
	return atomic_load(&ep->unsafe);
}

int64_t ofi_rma_ep_key_quarantine_ms(const struct ofi_rma_ep *ep)
{
	return ep->cfg.key_quarantine_ms;
}

/* ---------------------------------------------------------------------- */
/* windows                                                                */
/* ---------------------------------------------------------------------- */

int ofi_rma_window_register(struct ofi_rma_ep *d, void *ptr, size_t len,
			    const struct ofi_rma_memory *mem, unsigned access,
			    struct ofi_rma_window *out)
{
	if (!access || (access & ~(unsigned)(OFI_RMA_REMOTE_WRITE |
					     OFI_RMA_REMOTE_READ)))
		return -EINVAL;
	if ((access & OFI_RMA_REMOTE_READ) && !d->reads) {
		set_err(d, "%s offers no RMA reads here (see the reads "
			"configuration), so a window cannot be read by peers",
			d->prov);
		return -EOPNOTSUPP;
	}
	const uint64_t fi_access =
		((access & OFI_RMA_REMOTE_WRITE) ? FI_REMOTE_WRITE : 0) |
		((access & OFI_RMA_REMOTE_READ) ? FI_REMOTE_READ : 0);
	pthread_mutex_lock(&d->mtx);
	if (d->nwin == d->capwin) {
		const size_t cap = d->capwin ? 2 * d->capwin : 16;
		struct window_ent *w = realloc(d->windows, cap * sizeof(*w));
		if (!w) {
			pthread_mutex_unlock(&d->mtx);
			return -ENOMEM;
		}
		d->windows = w;
		d->capwin = cap;
	}
	struct region rg;
	const int r = reg_window_locked(d, ptr, len, mem, fi_access, &rg);
	if (r < 0) {
		pthread_mutex_unlock(&d->mtx);
		return r;
	}
	const uint64_t id = d->next_window++;
	d->windows[d->nwin].id = id;
	d->windows[d->nwin].r = rg;
	d->nwin++;
	atomic_store(&d->nwindows, d->nwin);
	pthread_mutex_unlock(&d->mtx);
	out->id = id;
	out->ptr = ptr;
	out->len = len;
	return 0;
}

void ofi_rma_window_deregister(struct ofi_rma_ep *d, uint64_t id)
{
	pthread_mutex_lock(&d->mtx);
	struct window_ent *w = window_find(d, id);
	if (w) {
		close_window_region_locked(d, &w->r);
		const size_t i = (size_t)(w - d->windows);
		memmove(&d->windows[i], &d->windows[i + 1],
			(d->nwin - i - 1) * sizeof(*w));
		d->nwin--;
		atomic_store(&d->nwindows, d->nwin);
	}
	pthread_mutex_unlock(&d->mtx);
}

int ofi_rma_window_rekey(struct ofi_rma_ep *d, const struct ofi_rma_window *w)
{
	if (!lock_briefly(d)) {
		/* an insert holds a FI_THREAD_DOMAIN domain, and no other call
		 * may run meanwhile */
		return -EINPROGRESS;
	}
	int ret;
	if (d->broken) {
		ret = -EIO;
		goto out;
	}
	struct window_ent *we = window_find(d, w->id);
	if (!we || !we->r.mr) {
		ret = -ENOENT;
		goto out;
	}
	struct region *cur = &we->r;
	if (atomic_load(&d->rekey_in_place) != 0) {
		/* in place: the region keeps its memory, registration and
		 * binding */
		uint64_t key = 0;
		const int r = fi_control(&cur->mr->fid, MR_REKEY, &key);
		if (r == 0 && key != cur->key && key != FI_KEY_NOTAVAIL &&
		    !u64map_get(&d->retired, key, NULL)) {
			/* the provider never gives the old key out again, so it
			 * needs no quarantine */
			atomic_store(&d->rekey_in_place, 1);
			cur->key = key;
			d->windows_rekeyed++;
			d->rekeys_in_place++;
			ret = 0;
			goto out;
		}
		if (r == -FI_ENOSYS || r == -FI_EOPNOTSUPP) {
			atomic_store(&d->rekey_in_place, 0);
		} else if (r == 0) {
			/* a key that is still in quarantine, or none at all: the
			 * old key is dead already; register the memory again for
			 * a sound one */
			d->key_collisions++;
			cur->key = key;
		} else {
			set_err(d, "re-keying a window in place: %s; registering "
				"it again", fi_err(r));
		}
	}
	/* the new region first, while the old one still holds its key, so
	 * the provider cannot hand the old key straight back. -EINPROGRESS
	 * is kept for a re-key that an insert put off, so a provider's own
	 * comes back as -EIO. */
	struct region fresh;
	int r = reg_window_locked(d, cur->ptr, cur->len, &cur->mem, cur->access,
				  &fresh);
	if (r < 0) {
		ret = r == -EINPROGRESS ? -EIO : r;
		goto out;
	}
	if (fresh.key == cur->key) {
		/* the provider handed back the region it already had: a
		 * registration cache (verbs and efa keep one by default) keeps
		 * regions registered, so closing the old one would leave its
		 * key working. Report it rather than a re-key that did not
		 * happen. */
		fi_close(&fresh.mr->fid);
		set_err(d, "the provider gave the memory registered again its "
			"old key: its registration cache keeps the region, so "
			"the old key stays valid; set "
			"FI_MR_CACHE_MONITOR=disabled");
		ret = -EOPNOTSUPP;
		goto out;
	}
	r = close_window_region_locked(d, cur);
	if (r < 0) {
		/* the old key still works; keep the window as it was */
		fi_close(&fresh.mr->fid);
		ret = r == -EINPROGRESS ? -EIO : r;
		goto out;
	}
	*cur = fresh;
	d->windows_rekeyed++;
	d->rekeys_reregistered++;
	ret = 0;
out:
	pthread_mutex_unlock(&d->mtx);
	return ret;
}

ssize_t ofi_rma_window_token(struct ofi_rma_ep *d,
			     const struct ofi_rma_window *w, uint64_t ofs,
			     uint64_t len, char *buf, size_t buflen)
{
	if (!lock_briefly(d))
		return -EINPROGRESS;  /* an insert holds the endpoint */
	ssize_t ret;
	struct window_ent *we = window_find(d, w->id);
	if (d->broken) {
		ret = -EIO;
	} else if (!we || !we->r.mr) {
		ret = -ENOENT;
	} else if (ofs > we->r.len || len > we->r.len - ofs) {
		ret = -ERANGE;
	} else {
		struct ofi_rma_token t;
		memset(&t, 0, sizeof(t));
		t.base = (d->mr_mode & FI_MR_VIRT_ADDR) ?
			(uint64_t)(uintptr_t)we->r.ptr + ofs : ofs;
		t.size = len;
		memcpy(t.wire, d->wire, sizeof(t.wire));
		pthread_mutex_lock(&d->name_mtx);
		memcpy(t.addr, d->my_name, d->my_name_len);
		t.addr_len = d->my_name_len;
		pthread_mutex_unlock(&d->name_mtx);
		t.key = we->r.key;
		ret = ofi_rma_token_format(&t, buf, buflen);
	}
	pthread_mutex_unlock(&d->mtx);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* transfers                                                              */
/* ---------------------------------------------------------------------- */

struct tile {
	uint64_t ofs, len;
};

static int cmp_tile(const void *a, const void *b)
{
	const struct tile *x = a, *y = b;
	if (x->ofs != y->ofs)
		return x->ofs < y->ofs ? -1 : 1;
	return x->len < y->len ? -1 : x->len > y->len;
}

/* the wait between polls, for a waiting writer that does not poll */
static void call_wait_hook(struct ofi_rma_ep *d, struct plan *plan)
{
	if (d->hooks.wait) {
		pthread_mutex_unlock(&d->mtx);
		d->hooks.wait(d->hooks.ctx, plan->deadline);
		pthread_mutex_lock(&d->mtx);
	}
}

/* write and read: gather iov into a staging slot and write it to the
 * peer's window, or read the peer's window into the slot and scatter it
 * into iov */
static int transfer(struct ofi_rma_ep *d, bool reading,
		    const struct ofi_rma_token *dst, const struct iovec *iov,
		    size_t iovcnt, const struct ofi_rma_range *ranges,
		    size_t nranges, int64_t budget_ms, bool *posted)
{
	/* dst is the peer's window, which a read reads from */
	if (posted)
		*posted = false;
	if (!d->stage || (reading && !d->reads))
		return -EOPNOTSUPP;
	if (strncmp(dst->wire, d->wire, sizeof(d->wire)))
		return -EPROTONOSUPPORT;
	if (dst->auth_len) {
		set_err(d, "the token carries an authorization key, which this "
			"version does not use yet; nothing sent");
		return -EPROTONOSUPPORT;
	}
	if (dst->addr_len == 0 || dst->addr_len > OFI_RMA_ADDR_MAX)
		return -EINVAL;
	uint64_t total = 0;
	for (size_t i = 0; i < iovcnt; i++)
		total += iov[i].iov_len;
	size_t chunks = 0;
	for (size_t i = 0; i < nranges; i++) {
		const struct ofi_rma_range *w = &ranges[i];
		if (w->len > total || w->local > total - w->len ||
		    w->len > dst->size || w->remote > dst->size - w->len)
			return -ERANGE;
		chunks += (w->len + d->max_write - 1) / d->max_write;
	}
	if (reading) {
		/* every destination byte must come from a read: the slot still
		 * holds the bytes of an earlier transfer, maybe another
		 * client's, and a gap would scatter those into iov */
		struct tile *tiles = malloc((nranges ? nranges : 1) *
					    sizeof(*tiles));
		if (!tiles)
			return -ENOMEM;
		for (size_t i = 0; i < nranges; i++)
			tiles[i] = (struct tile){ranges[i].local, ranges[i].len};
		qsort(tiles, nranges, sizeof(*tiles), cmp_tile);
		uint64_t next = 0;
		bool ok = true;
		for (size_t i = 0; i < nranges && ok; i++) {
			if (tiles[i].ofs != next)
				ok = false;
			next += tiles[i].len;
		}
		free(tiles);
		if (!ok || next != total)
			return -ERANGE;
	}
	if (total > d->cfg.stage_size)
		return -E2BIG;
	/* stop waiting early enough to cut the writes off within the budget:
	 * by what a cut-off costs, and by how late cut-offs tend to start */
	const int64_t margin = atomic_load(&d->cutoff_cost_ms) +
		atomic_load(&d->cutoff_slack_ms);
	if (budget_ms <= margin) {
		d->budget_refused++;
		set_err(d, "a budget of %lld ms is no longer than a cut-off is "
			"expected to take, with scheduling slack (%lld ms); "
			"nothing sent", (long long)budget_ms, (long long)margin);
		return -ETIMEDOUT;
	}
	const int64_t start = now_ns();
	const int64_t budget_end = add_sat(start, budget_ms > INT64_MAX / NS_PER_MS ?
					   INT64_MAX : budget_ms * NS_PER_MS);
	const int64_t deadline = budget_end - margin * NS_PER_MS;
	/* the peer's address first, without the endpoint lock: the insert of
	 * a new peer can take seconds */
	fi_addr_t addr = FI_ADDR_NOTAVAIL;
	struct peer *peer = NULL;
	int r = get_peer(d, dst->addr, dst->addr_len, deadline, start, &addr,
			 &peer);
	if (r < 0) {
		if (r == -ETIMEDOUT)
			d->peer_timeouts++;
		return r;
	}
	r = lock_for_write(d, deadline, start);
	if (r < 0) {
		put_peer(d, peer);
		if (r == -ETIMEDOUT)
			d->peer_timeouts++;
		return r;
	}
	if (d->broken || !d->ep) {
		pthread_mutex_unlock(&d->mtx);
		put_peer(d, peer);
		return -EIO;
	}
	/* waiting for the peer or the endpoint may have eaten the budget */
	if (!time_to_post_locked(d, total, deadline)) {
		d->late_starts++;
		pthread_mutex_unlock(&d->mtx);
		put_peer(d, peer);
		set_err(d, "too little budget left to start the write; nothing "
			"sent");
		return -ETIMEDOUT;
	}
	size_t slot = 0;
	while (slot < d->cfg.stage_count && d->stage_busy[slot])
		slot++;
	if (slot == d->cfg.stage_count) {
		d->staging_busy++;
		pthread_mutex_unlock(&d->mtx);
		put_peer(d, peer);
		return -EBUSY;
	}
	struct plan *plan = calloc(1, sizeof(*plan));
	if (plan)
		plan->ops = calloc(chunks ? chunks : 1, sizeof(*plan->ops));
	if (!plan || !plan->ops) {
		free(plan);
		pthread_mutex_unlock(&d->mtx);
		put_peer(d, peer);
		return -ENOMEM;
	}
	/* the plan holds the slot and the peer reference from here on; being
	 * listed, it also keeps a FI_THREAD_DOMAIN insert from starting */
	d->stage_busy[slot] = true;
	plan_link_head(&d->plans, plan);
	plan->slot = slot;
	plan->peer = peer;
	plan->reading = reading;
	plan->budget_end = budget_end;
	plan->deadline = deadline;

	/* gather the source into the slot without the lock: it can be
	 * megabytes, and the slot is ours alone. A read fills the slot
	 * instead. */
	char *buf = d->stage + slot * d->cfg.stage_size;
	pthread_mutex_unlock(&d->mtx);
	if (!reading) {
		char *p = buf;
		for (size_t i = 0; i < iovcnt; i++) {
			memcpy(p, iov[i].iov_base, iov[i].iov_len);
			p += iov[i].iov_len;
		}
	}
	if (d->hooks.pre_post)
		d->hooks.pre_post(d->hooks.ctx);
	pthread_mutex_lock(&d->mtx);
	if (d->broken || !d->ep) {
		/* a cut-off failed to reopen the endpoint while we copied */
		retire_locked(d, plan);
		pthread_mutex_unlock(&d->mtx);
		return -EIO;
	}
	/* the last look before posting: a write posted with no time left
	 * would be cut off at once, and the cut-off would cancel every other
	 * write in flight on the endpoint */
	if (!time_to_post_locked(d, total, deadline)) {
		d->late_starts++;
		set_err(d, "too little budget left to start the write; nothing "
			"sent");
		retire_locked(d, plan);
		pthread_mutex_unlock(&d->mtx);
		return -ETIMEDOUT;
	}
	plan->posted_at = now_ns();

	for (size_t i = 0; i < nranges; i++) {
		const struct ofi_rma_range *w = &ranges[i];
		for (uint64_t o = 0; o < w->len && !plan->err && !plan->cut;) {
			const uint64_t n = minu64(w->len - o, d->max_write);
			struct op *op = &plan->ops[plan->n_ops++];
			memset(op, 0, sizeof(*op));
			op->plan = plan;
			ssize_t pr;
			for (;;) {
				pr = reading ?
					fi_read(d->ep, buf + w->local + o, n,
						d->stage_mr.desc, addr,
						dst->base + w->remote + o,
						dst->key, &op->ctx) :
					fi_write(d->ep, buf + w->local + o, n,
						 d->stage_mr.desc, addr,
						 dst->base + w->remote + o,
						 dst->key, &op->ctx);
				if (pr != -FI_EAGAIN)
					break;
				/* the transmit queue is full: progress, which
				 * completes what is out, then retry, letting
				 * others at the endpoint meanwhile */
				poll_locked(d);
				if (now_ns() > deadline)
					break;
				pthread_mutex_unlock(&d->mtx);
				sched_yield();
				pthread_mutex_lock(&d->mtx);
				if (plan->err || plan->cut || d->broken)
					break;  /* a cut-off took what this plan
						 * had posted */
			}
			if (pr || plan->err || plan->cut || d->broken) {
				plan->n_ops--;
				if (!plan->err && !plan->cut) {
					if (d->broken)
						set_err(d, "the endpoint is broken");
					else
						set_err(d, "%s: %s",
							reading ? "fi_read" : "fi_write",
							fi_err(pr));
					if (reading)
						d->reads_failed++;
					else
						d->writes_failed++;
					plan->err = pr == -FI_EAGAIN ? -ETIMEDOUT :
						d->broken ? -EIO : to_errno(pr);
				}
				break;
			}
			plan->outstanding++;
			if (reading)
				d->reads_posted++;
			else
				d->writes_posted++;
			if (posted)
				*posted = true;
			o += n;
		}
		if (plan->err || plan->cut)
			break;
	}

	/* Wait for the writes. One waiting write polls the completion queue
	 * for all of them, which drives the provider if it progresses only
	 * while polled; the others sleep until a plan finishes, the poller
	 * leaves, or a cut-off fails them, and each wakes at its deadline. */
	bool polling = false;
	int64_t spin_until = 0;
#define LEAVE() \
	do { \
		if (polling) { \
			polling = false; \
			d->poller = false; \
			pthread_cond_broadcast(&d->done_cv); \
		} \
	} while (0)
	for (;;) {
		if (plan->cut) {
			/* cut off by cancelling its operations, by this write or
			 * on its behalf */
			LEAVE();
			const int res = plan->cut_result;
			if (plan->hooked) {
				/* a test's cancel hook stood in; the plan stays */
				pthread_mutex_unlock(&d->mtx);
				return res;
			}
			if (plan->outstanding)
				poll_locked(d);  /* the cancellations complete,
						  * usually at once */
			if (plan->outstanding)
				plan->abandoned = true;  /* whoever polls next
							  * retires it */
			else
				retire_locked(d, plan);
			pthread_mutex_unlock(&d->mtx);
			return res;
		}
		if (plan->outstanding == 0) {
			LEAVE();
			const int res = plan->err;
			if (!res && reading) {
				/* every read completed: scatter the slot without
				 * the lock, as the gather does; the plan holds the
				 * slot until it retires */
				pthread_mutex_unlock(&d->mtx);
				const char *q = buf;
				for (size_t i = 0; i < iovcnt; i++) {
					memcpy(iov[i].iov_base, q, iov[i].iov_len);
					q += iov[i].iov_len;
				}
				pthread_mutex_lock(&d->mtx);
			}
			if (!res) {
				if (reading)
					d->bytes_read += total;
				else
					d->bytes_written += total;
				const int64_t took = now_ns() - plan->posted_at;
				if (total >= RATE_MIN_BYTES && took > 0) {
					const double rate = (double)total / (double)took;
					d->plan_rate = d->plan_rate > 0 ?
						d->plan_rate + (rate - d->plan_rate) / 8 :
						rate;
				}
			}
			/* a reset that cut its writes off left -ETIMEDOUT here,
			 * or -ENOTRECOVERABLE when it went wrong */
			retire_locked(d, plan);
			pthread_mutex_unlock(&d->mtx);
			return res;
		}
		const int64_t now = now_ns();
		if (now > plan->deadline) {
			/* a last look: what completed during the wait counts */
			poll_locked(d);
			if (plan->outstanding == 0)
				continue;
			/* out of budget with writes in flight: cut them off, so
			 * none of them lands in the peer's window after the
			 * caller gave up */
			LEAVE();
			cut_off_locked(d, plan, now);
			continue;
		}
		if (!polling && !d->poller) {
			polling = d->poller = true;
			spin_until = now + SPIN_TIME_NS;
		}
		if (!polling) {
			/* wake at the deadline, and now and then to take over
			 * from a poller the scheduler left off the CPU */
			cond_wait_until(&d->done_cv, &d->mtx,
					min64(plan->deadline, now + POLLER_STALE_NS));
			call_wait_hook(d, plan);
			if (now_ns() - d->last_poll > POLLER_STALE_NS) {
				poll_locked(d);
				cut_due_locked(d);
			}
			continue;
		}
		const int64_t t0 = now_ns();
		const size_t got = poll_locked(d);
		const int64_t t1 = now_ns();
		d->poll_floor = min64(d->poll_floor, t1 - t0);
		if (got || t1 - t0 > 2 * d->poll_floor + POLL_BUSY_MARGIN_NS)
			spin_until = t1 + SPIN_TIME_NS;
		/* writes whose writers are not running are cut off in time
		 * anyway */
		cut_due_locked(d);
		if (plan->outstanding && !plan->cut) {
			wait_cq(d, plan->deadline, spin_until);
			call_wait_hook(d, plan);
		}
	}
#undef LEAVE
}

int ofi_rma_write(struct ofi_rma_ep *ep, const struct ofi_rma_token *dst,
		  const struct iovec *iov, size_t iovcnt,
		  const struct ofi_rma_range *ranges, size_t nranges,
		  int64_t budget_ms, bool *posted)
{
	return transfer(ep, false, dst, iov, iovcnt, ranges, nranges,
			budget_ms, posted);
}

int ofi_rma_read(struct ofi_rma_ep *ep, const struct ofi_rma_token *src,
		 const struct iovec *iov, size_t iovcnt,
		 const struct ofi_rma_range *ranges, size_t nranges,
		 int64_t budget_ms, bool *posted)
{
	return transfer(ep, true, src, iov, iovcnt, ranges, nranges, budget_ms,
			posted);
}

void ofi_rma_ep_progress(struct ofi_rma_ep *ep)
{
	pthread_mutex_lock(&ep->mtx);
	poll_locked(ep);
	pthread_mutex_unlock(&ep->mtx);
}

void ofi_rma_ep_sync(struct ofi_rma_ep *ep)
{
	/* a provider that places data in software does so inside
	 * fi_cq_read, which runs under this lock */
	if (lock_briefly(ep)) {
		pthread_mutex_unlock(&ep->mtx);
		return;
	}
	/* an insert in the provider holds the lock, and nothing polls until
	 * it returns: each poll before it released polls when it was
	 * through */
	(void)atomic_load_explicit(&ep->polls, memory_order_acquire);
}

bool ofi_rma_ep_quiet(const struct ofi_rma_ep *ep)
{
	return atomic_load(&ep->quiesce);
}

void ofi_rma_ep_windows_lent(struct ofi_rma_ep *ep, int64_t latest_ns)
{
	atomic_store(&ep->lent_ns, latest_ns);
}

void ofi_rma_ep_get_stats(struct ofi_rma_ep *d, struct ofi_rma_stats *out)
{
	struct ofi_rma_stats s;
	memset(&s, 0, sizeof(s));
	s.writes_posted = d->writes_posted;
	s.writes_failed = d->writes_failed;
	s.bytes_written = d->bytes_written;
	s.reads_posted = d->reads_posted;
	s.reads_failed = d->reads_failed;
	s.bytes_read = d->bytes_read;
	s.peers_inserted = d->peers_inserted;
	s.staging_busy = d->staging_busy;
	s.timeouts = d->timeouts;
	s.resets = d->resets;
	s.peer_timeouts = d->peer_timeouts;
	s.inserts_refused = d->inserts_refused;
	pthread_mutex_lock(&d->peer_mtx);
	s.peers = d->npeers;
	s.pending_inserts = d->pending;
	pthread_mutex_unlock(&d->peer_mtx);
	s.insert_busy = d->insert_busy;
	s.cutoffs_failed = d->cutoffs_failed;
	s.cutoffs_late = d->cutoffs_late;
	s.budget_refused = d->budget_refused;
	s.cutoff_cost_ms = (uint64_t)atomic_load(&d->cutoff_cost_ms);
	s.max_cutoff_lateness_ms = d->max_lateness_ms;
	s.last_late_cutoff_ns = d->last_late_ns;
	s.cutoffs_on_behalf = d->cutoffs_on_behalf;
	s.cutoffs_past_tolerance = d->cutoffs_past_tolerance;
	s.last_past_cutoff_ns = d->last_past_ns;
	s.max_poll_gap_ms = d->max_poll_gap_ms;
	s.last_long_poll_gap_ns = d->last_long_gap_ns;
	s.cutoff_slack_ms = (uint64_t)atomic_load(&d->cutoff_slack_ms);
	s.late_tolerance_ms = (uint64_t)d->cfg.late_tolerance_ms;
	s.late_starts = d->late_starts;
	s.plans_cut_off = d->plans_cut_off;
	s.cancels_failed = d->cancels_failed;
	s.cancel_discards = d->cancel_discards;
	s.close_discards = d->close_discards;
	s.windows_rekeyed = d->windows_rekeyed;
	s.key_collisions = d->key_collisions;
	s.rekeys_in_place = d->rekeys_in_place;
	s.rekeys_reregistered = d->rekeys_reregistered;
	s.rekey_in_place = d->rekey_in_place;
	s.unsafe = d->unsafe;
	s.broken = d->broken;
	s.windows = d->nwindows;
	const size_t size = out->size;
	const size_t n = size < sizeof(s) ? size : sizeof(s);
	memcpy(out, &s, n);
	out->size = size;
}
