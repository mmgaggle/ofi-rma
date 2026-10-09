/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

/* The C interface, from a strict C11 program: tokens, a write and a read
 * between two tcp endpoints, and a window pool. The C++ suite
 * (test_ofi_rma.cc) covers the behavior in depth. */

#include "ofi_rma/ofi_rma.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                        \
	do {                                                               \
		if (!(cond)) {                                             \
			fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
				__LINE__, #cond);                          \
			failures++;                                        \
		}                                                          \
	} while (0)

static int parse(const char *s, struct ofi_rma_token *t)
{
	return ofi_rma_token_parse(s, strlen(s), t);
}

static void test_tokens(void)
{
	struct ofi_rma_token t, p;
	char buf[OFI_RMA_TOKEN_MAX];
	static const unsigned char name[] = {2, 0, 0x12, 0x34, 10, 0, 0, 1, 0, 0xff};
	static const unsigned char job[] = {0x00, 0xa1, 0xb2};

	memset(&t, 0, sizeof(t));
	t.base = 0x7f0012345000u;
	t.size = 0xc00000u;
	strcpy(t.wire, "rxm.1");
	memcpy(t.addr, name, sizeof(name));
	t.addr_len = sizeof(name);
	t.key = 0xdeadbeefu;
	CHECK(ofi_rma_token_format(&t, buf, sizeof(buf)) > 0);
	CHECK(!strcmp(buf, "7f0012345000:c00000:ofi1:rxm.1:020012340a00000100ff:"
		      "deadbeef"));
	CHECK(parse(buf, &p) == 0);
	CHECK(p.base == t.base && p.size == t.size && p.key == t.key);
	CHECK(!strcmp(p.wire, "rxm.1") && p.auth_len == 0);
	CHECK(p.addr_len == sizeof(name) && !memcmp(p.addr, name, sizeof(name)));

	/* with an authorization key */
	memcpy(t.auth, job, sizeof(job));
	t.auth_len = sizeof(job);
	strcpy(t.wire, "uet.10000");
	CHECK(ofi_rma_token_format(&t, buf, sizeof(buf)) > 0);
	CHECK(!strcmp(buf, "7f0012345000:c00000:ofi1:uet.10000:"
		      "020012340a00000100ff:deadbeef:00a1b2"));
	CHECK(parse(buf, &p) == 0);
	CHECK(p.auth_len == 3 && !memcmp(p.auth, job, 3));

	/* snprintf() semantics */
	CHECK(ofi_rma_token_format(&t, buf, 8) == (ssize_t)strlen(
		"7f0012345000:c00000:ofi1:uet.10000:020012340a00000100ff:"
		"deadbeef:00a1b2"));
	CHECK(strlen(buf) == 7);

	/* not ofi1 tokens */
	CHECK(parse("0:100000:uet1:10.88.0.30:1234", &p) == -EINVAL);
	CHECK(parse("7f0000001000:100000:1234:0:abc:1:fe80::1", &p) == -EINVAL);
	CHECK(parse("0:10:ofi2:rxm.1:0200:1", &p) == -EINVAL);
	CHECK(parse("0:10:ofi1:verbs;ofi_rxm:0200:1", &p) == -EINVAL);
	CHECK(parse("0:10:ofi1:tcp:0200:1", &p) == -EINVAL);  /* no version */
	CHECK(parse("0:10:ofi1:Rxm.1:0200:1", &p) == -EINVAL);
	CHECK(parse("0:10:ofi1:rxm.1g:0200:1", &p) == -EINVAL);
	CHECK(parse("0:10:ofi1:rxm.1:0200:1:", &p) == -EINVAL);   /* empty auth */
	CHECK(parse("0:10:ofi1:rxm.1:0200:1:abc", &p) == -EINVAL); /* odd auth */
	CHECK(parse("0:10:ofi1:rxm.1:0200:1:00:00", &p) == -EINVAL);
	CHECK(parse("0:10:ofi1:rxm.1:0200:1", &p) == 0);
	CHECK(parse("0:10:ofi1:rxm.1:0200:1:00a1b2", &p) == 0);

	/* families */
	CHECK(ofi_rma_token_family("0:10:ofi1:rxm.1:0200:1", 22) == 1);
	CHECK(ofi_rma_token_family("0:10:ofi2:x:y:z", 15) == 2);
	CHECK(ofi_rma_token_family("7f00:1000:1234:0:abc:1:fe80::1", 30) == 0);
	CHECK(ofi_rma_token_family("0:10:ofix:a:b:c", 15) == 0);
	CHECK(ofi_rma_token_family("0:10:ofi", 8) == 0);

	CHECK(ofi_rma_wire_valid("rxm.1"));
	CHECK(ofi_rma_wire_valid("prov_verbs_ofi_rxm.0"));
	CHECK(!ofi_rma_wire_valid("rxm"));
	CHECK(!ofi_rma_wire_valid(".1"));
	CHECK(!ofi_rma_wire_valid("rxm.123456789"));
}

static int open_ep(const char *prov, const char *node, size_t stage,
		   int progress, struct ofi_rma_ep **ep)
{
	struct ofi_rma_config c;
	char err[256];
	ofi_rma_config_init(&c);
	c.provider = prov;
	c.node = node;
	c.stage_size = stage;
	c.stage_count = stage ? 2 : 0;
	c.progress_thread = progress != 0;
	c.reads = 1;
	if (ofi_rma_ep_open(&c, ep, err, sizeof(err))) {
		fprintf(stderr, "%s: %s\n", prov, err);
		return -1;
	}
	return 0;
}

static void test_tcp(void)
{
	struct ofi_rma_ep *target, *writer;
	if (open_ep("tcp", "127.0.0.1", 0, 1, &target) ||
	    open_ep("tcp", "127.0.0.1", 1 << 20, 0, &writer)) {
		fprintf(stderr, "tcp unavailable; skipping the endpoint test\n");
		return;
	}
	CHECK(ofi_rma_wire_valid(ofi_rma_ep_wire(target)));
	CHECK(!strcmp(ofi_rma_ep_wire(target), ofi_rma_ep_wire(writer)));
	printf("tcp: wire %s, provider %s\n", ofi_rma_ep_wire(target),
	       ofi_rma_ep_provider(target));

	char *win = calloc(1, 8192);
	struct ofi_rma_window w;
	CHECK(ofi_rma_window_register(target, win, 8192, NULL,
				      OFI_RMA_REMOTE_WRITE | OFI_RMA_REMOTE_READ,
				      &w) == 0);
	char tok[OFI_RMA_TOKEN_MAX];
	const ssize_t n = ofi_rma_window_token(target, &w, 0, 8192, tok,
					       sizeof(tok));
	CHECK(n > 0);
	CHECK(ofi_rma_token_family(tok, (size_t)n) == 1);
	struct ofi_rma_token t;
	CHECK(ofi_rma_token_parse(tok, (size_t)n, &t) == 0);
	CHECK(!strcmp(t.wire, ofi_rma_ep_wire(target)));
	CHECK(ofi_rma_window_token(target, &w, 4096, 8192, tok, sizeof(tok)) ==
	      -ERANGE);

	char src[100];
	for (int i = 0; i < 100; i++)
		src[i] = (char)i;
	struct iovec iov = {src, sizeof(src)};
	struct ofi_rma_range r[2] = {{0, 50, 1000}, {50, 50, 5000}};
	bool posted = false;
	CHECK(ofi_rma_write(writer, &t, &iov, 1, r, 2, 3000, &posted) == 0);
	CHECK(posted);
	ofi_rma_ep_sync(target);
	CHECK(!memcmp(win + 1000, src, 50) && !memcmp(win + 5000, src + 50, 50));

	/* read it back */
	char back[100];
	memset(back, 0, sizeof(back));
	struct iovec biov = {back, sizeof(back)};
	struct ofi_rma_range rr[2] = {{0, 50, 1000}, {50, 50, 5000}};
	if (ofi_rma_ep_reads(writer)) {
		CHECK(ofi_rma_read(writer, &t, &biov, 1, rr, 2, 3000, NULL) == 0);
		CHECK(!memcmp(back, src, sizeof(src)));
	}

	/* another wire, and an authorization key, are refused */
	struct ofi_rma_token o = t;
	strcpy(o.wire, "nosuch.1");
	CHECK(ofi_rma_write(writer, &o, &iov, 1, r, 2, 3000, &posted) ==
	      -EPROTONOSUPPORT);
	CHECK(!posted);
	o = t;
	o.auth[0] = 1;
	o.auth_len = 3;
	CHECK(ofi_rma_write(writer, &o, &iov, 1, r, 2, 3000, &posted) ==
	      -EPROTONOSUPPORT);

	struct ofi_rma_stats s;
	memset(&s, 0, sizeof(s));
	s.size = sizeof(s);
	ofi_rma_ep_get_stats(writer, &s);
	CHECK(s.size == sizeof(s));
	CHECK(s.writes_posted >= 2 && s.bytes_written == 100);

	/* a caller that knows fewer fields gets only those */
	struct ofi_rma_stats small;
	memset(&small, 0xff, sizeof(small));
	small.size = offsetof(struct ofi_rma_stats, bytes_written);
	ofi_rma_ep_get_stats(writer, &small);
	CHECK(small.writes_posted == s.writes_posted);
	CHECK(small.bytes_written == UINT64_MAX);

	char desc[512];
	CHECK(ofi_rma_ep_describe(target, desc, sizeof(desc)) > 0);
	printf("%s\n", desc);

	/* a window pool */
	char *mem = calloc(4, 4096);
	struct ofi_rma_pool *pool;
	char err[256];
	CHECK(ofi_rma_pool_create(target, mem, 4096, 4, &pool, err,
				  sizeof(err)) == 0);
	CHECK(ofi_rma_pool_count(pool) == 4);
	CHECK(ofi_rma_pool_window_size(pool) == 4096);
	struct ofi_rma_lent l;
	CHECK(ofi_rma_pool_acquire(pool, 8192, &l) == -E2BIG);
	CHECK(ofi_rma_pool_acquire(pool, 4096, &l) == 0);
	struct ofi_rma_token pt;
	CHECK(ofi_rma_token_parse(l.token, strlen(l.token), &pt) == 0);
	CHECK(ofi_rma_write(writer, &pt, &iov, 1, r, 1, 3000, NULL) == 0);
	ofi_rma_ep_sync(target);
	CHECK(!memcmp((char *)l.ptr + 1000, src, 50));
	ofi_rma_pool_release(pool, l.id, 0, false);
	struct ofi_rma_pool_stats ps;
	ps.size = sizeof(ps);
	ofi_rma_pool_get_stats(pool, &ps);
	CHECK(ps.acquired == 1);
	ofi_rma_pool_destroy(pool);
	free(mem);

	ofi_rma_window_deregister(target, w.id);
	ofi_rma_ep_close(writer);
	ofi_rma_ep_close(target);
	free(win);
}

int main(void)
{
	CHECK(ofi_rma_version() ==
	      OFI_RMA_VERSION(OFI_RMA_VERSION_MAJOR, OFI_RMA_VERSION_MINOR));
	test_tokens();
	test_tcp();
	if (failures) {
		fprintf(stderr, "%d checks failed\n", failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
