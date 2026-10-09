/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

#include "ofi_rma_int.h"

#include <stdatomic.h>
#include <stdio.h>

struct slot {
	struct ofi_rma_window w;
	bool in_use;
	int64_t quarantined_until;
	/* released while an insert held the endpoint: not lent until
	 * re-keyed, and then quarantined this long */
	bool rekey_due;
	int64_t due_quarantine_ms;
	/* when it was lent; 0 when it is not */
	int64_t lent_at;
};

struct ofi_rma_pool {
	struct ofi_rma_ep *ep;
	size_t slot_size;
	pthread_mutex_t mtx;
	struct slot *slots;
	size_t count;
	/* slots with rekey_due; written under mtx, read without it */
	_Atomic size_t rekeys_due;
	struct ofi_rma_pool_stats st;
};

int ofi_rma_pool_create(struct ofi_rma_ep *ep, void *mem, size_t size,
			size_t count, struct ofi_rma_pool **out, char *err,
			size_t errlen)
{
	if (err && errlen)
		err[0] = '\0';
	struct ofi_rma_pool *p = calloc(1, sizeof(*p));
	if (p)
		p->slots = calloc(count ? count : 1, sizeof(*p->slots));
	if (!p || !p->slots) {
		free(p);
		fmt_into(err, errlen, "out of memory");
		return -ENOMEM;
	}
	p->ep = ep;
	p->slot_size = size;
	pthread_mutex_init(&p->mtx, NULL);
	for (size_t i = 0; i < count; i++) {
		const int r = ofi_rma_window_register(ep, (char *)mem + i * size,
						      size, NULL,
						      OFI_RMA_REMOTE_WRITE,
						      &p->slots[i].w);
		if (r < 0) {
			char e[512];
			ofi_rma_ep_last_error(ep, e, sizeof(e));
			fmt_into(err, errlen, "registering window %zu: %s", i, e);
			p->count = i;
			ofi_rma_pool_destroy(p);
			return r;
		}
		p->count = i + 1;
	}
	*out = p;
	return 0;
}

void ofi_rma_pool_destroy(struct ofi_rma_pool *p)
{
	if (!p)
		return;
	ofi_rma_ep_windows_lent(p->ep, 0);
	for (size_t i = 0; i < p->count; i++)
		ofi_rma_window_deregister(p->ep, p->slots[i].w.id);
	pthread_mutex_destroy(&p->mtx);
	free(p->slots);
	free(p);
}

/* tell the endpoint when the latest window still lent was lent */
static void note_lent_locked(struct ofi_rma_pool *p)
{
	int64_t latest = 0;
	for (size_t i = 0; i < p->count; i++)
		latest = max64(latest, p->slots[i].lent_at);
	ofi_rma_ep_windows_lent(p->ep, latest);
}

/* a window back from use, re-keyed with result r when rekey */
static void settle_locked(struct ofi_rma_pool *p, struct slot *s,
			  int64_t quarantine_ms, bool rekey, int r)
{
	if (rekey) {
		if (r == 0) {
			p->st.rekeyed++;
			if (quarantine_ms == 0) {
				/* every write of the last operation completed,
				 * and a late duplicate of one carries the old key */
				s->quarantined_until = 0;
				return;
			}
			/* a write that did not complete may still have packets
			 * on the way: the new key stops them only where the
			 * provider drops what carries the old one, so keep the
			 * window out until they are gone anyway */
			p->st.rekeyed_quarantined++;
		} else {
			p->st.rekey_failed++;
		}
		/* writes with the old key may land until it has left the
		 * network */
		quarantine_ms = max64(quarantine_ms,
				      ofi_rma_ep_key_quarantine_ms(p->ep));
	}
	if (quarantine_ms > 0)
		s->quarantined_until = add_sat(now_ns(), quarantine_ms * NS_PER_MS);
}

/* re-key a window whose re-key an insert put off; without mtx */
static void rekey_put_off(struct ofi_rma_pool *p)
{
	/* one window a call: the op thread that lends pays for one re-key, as
	 * the one that returned it would have, and those after it for the
	 * rest */
	size_t i = 0;
	pthread_mutex_lock(&p->mtx);
	while (i < p->count && !(p->slots[i].rekey_due && !p->slots[i].in_use))
		i++;
	if (i == p->count) {
		pthread_mutex_unlock(&p->mtx);
		return;
	}
	p->slots[i].in_use = true;  /* ours while it is re-keyed, without mtx */
	pthread_mutex_unlock(&p->mtx);
	const int r = ofi_rma_ep_quiet(p->ep) ? -EINPROGRESS :
		ofi_rma_window_rekey(p->ep, &p->slots[i].w);
	pthread_mutex_lock(&p->mtx);
	struct slot *s = &p->slots[i];
	s->in_use = false;
	if (r != -EINPROGRESS) {
		s->rekey_due = false;
		p->rekeys_due--;
		settle_locked(p, s, s->due_quarantine_ms, true, r);
	}
	/* else another insert: still due */
	pthread_mutex_unlock(&p->mtx);
}

int ofi_rma_pool_acquire(struct ofi_rma_pool *p, size_t size,
			 struct ofi_rma_lent *out)
{
	if (size > p->slot_size)
		return -E2BIG;
	if (atomic_load(&p->rekeys_due) && !ofi_rma_ep_quiet(p->ep))
		rekey_put_off(p);
	const int64_t now = now_ns();
	pthread_mutex_lock(&p->mtx);
	if (ofi_rma_ep_quiet(p->ep)) {
		/* An insert on a FI_THREAD_DOMAIN domain holds the endpoint, for
		 * seconds perhaps: a window lent now could not get a new key
		 * until it is done, and a provider that places data only while
		 * polled would not fill it meanwhile. The shard replies
		 * inline. */
		p->st.declined_insert++;
		pthread_mutex_unlock(&p->mtx);
		return -EINPROGRESS;
	}
	for (size_t i = 0; i < p->count; i++) {
		struct slot *s = &p->slots[i];
		if (s->in_use || s->rekey_due || now < s->quarantined_until)
			continue;
		const ssize_t n = ofi_rma_window_token(p->ep, &s->w, 0,
						       p->slot_size, out->token,
						       sizeof(out->token));
		const bool made = n > 0 && (size_t)n < sizeof(out->token);
		if (made) {
			/* lent from here, for an insert that begins now to wait
			 * for */
			s->in_use = true;
			s->lent_at = now_ns();
			note_lent_locked(p);
		}
		if (ofi_rma_ep_quiet(p->ep)) {
			/* an insert began meanwhile, and the token may have been
			 * made while it waited for the writes in flight: no
			 * window is lent once one has */
			if (s->in_use) {
				s->in_use = false;
				s->lent_at = 0;
				note_lent_locked(p);
			}
			p->st.declined_insert++;
			pthread_mutex_unlock(&p->mtx);
			return -EINPROGRESS;
		}
		if (!made) {
			pthread_mutex_unlock(&p->mtx);
			return -EIO;  /* the endpoint lends no more */
		}
		p->st.acquired++;
		out->id = i;
		out->ptr = s->w.ptr;
		out->size = p->slot_size;
		pthread_mutex_unlock(&p->mtx);
		return 0;
	}
	p->st.exhausted++;
	pthread_mutex_unlock(&p->mtx);
	return -EBUSY;
}

void ofi_rma_pool_release(struct ofi_rma_pool *p, uint64_t id,
			  int64_t quarantine_ms, bool rekey)
{
	if (id >= p->count)
		return;
	/* Re-keyed without mtx, which acquire takes: the re-key calls into
	 * the provider, and waits for the endpoint. The window is still in
	 * use meanwhile, so nothing else touches it. */
	int r = 0;
	if (rekey) {
		/* put off at once while an insert keeps the endpoint quiet */
		r = ofi_rma_ep_quiet(p->ep) ? -EINPROGRESS :
			ofi_rma_window_rekey(p->ep, &p->slots[id].w);
	}
	pthread_mutex_lock(&p->mtx);
	struct slot *s = &p->slots[id];
	s->in_use = false;
	if (s->lent_at) {
		s->lent_at = 0;
		note_lent_locked(p);
	}
	if (r == -EINPROGRESS) {
		/* an acquire that finds the endpoint free re-keys it, and the
		 * window is not lent before */
		s->rekey_due = true;
		s->due_quarantine_ms = quarantine_ms;
		p->rekeys_due++;
		p->st.rekeys_put_off++;
		pthread_mutex_unlock(&p->mtx);
		return;
	}
	settle_locked(p, s, quarantine_ms, rekey, r);
	pthread_mutex_unlock(&p->mtx);
}

void ofi_rma_pool_get_stats(struct ofi_rma_pool *p,
			    struct ofi_rma_pool_stats *out)
{
	pthread_mutex_lock(&p->mtx);
	struct ofi_rma_pool_stats s = p->st;
	pthread_mutex_unlock(&p->mtx);
	const size_t size = out->size;
	memcpy(out, &s, size < sizeof(s) ? size : sizeof(s));
	out->size = size;
}

size_t ofi_rma_pool_count(const struct ofi_rma_pool *p)
{
	return p->count;
}

size_t ofi_rma_pool_window_size(const struct ofi_rma_pool *p)
{
	return p->slot_size;
}
