/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

#include "ofi_rma_int.h"

#include <stdio.h>

/* libfabric's fi_ep_attr protocols, by value. The values are part of
 * libfabric's ABI and only ever appended to, so the table does not depend
 * on the headers a program builds against. */
static const char *const PROTO_NAMES[] = {
	NULL,            /* FI_PROTO_UNSPEC */
	"rdma_cm_ib_rc", /* FI_PROTO_RDMA_CM_IB_RC */
	"iwarp",         /* FI_PROTO_IWARP */
	"ib_ud",         /* FI_PROTO_IB_UD */
	"psmx",          /* FI_PROTO_PSMX */
	"udp",           /* FI_PROTO_UDP */
	"sock_tcp",      /* FI_PROTO_SOCK_TCP */
	"mxm",           /* FI_PROTO_MXM */
	"iwarp_rdm",     /* FI_PROTO_IWARP_RDM */
	"ib_rdm",        /* FI_PROTO_IB_RDM */
	"gni",           /* FI_PROTO_GNI */
	"rxm",           /* FI_PROTO_RXM */
	"rxd",           /* FI_PROTO_RXD */
	"mlx",           /* FI_PROTO_MLX */
	"networkdirect", /* FI_PROTO_NETWORKDIRECT */
	"psmx2",         /* FI_PROTO_PSMX2 */
	"shm",           /* FI_PROTO_SHM */
	"mrail",         /* FI_PROTO_MRAIL */
	"rstream",       /* FI_PROTO_RSTREAM */
	"rdma_cm_ib_xrc", /* FI_PROTO_RDMA_CM_IB_XRC */
	"efa",           /* FI_PROTO_EFA */
	"psmx3",         /* FI_PROTO_PSMX3 */
	"rxm_tcp",       /* FI_PROTO_RXM_TCP */
	"opx",           /* FI_PROTO_OPX */
	"cxi",           /* FI_PROTO_CXI */
	"xnet",          /* FI_PROTO_XNET */
	"coll",          /* FI_PROTO_COLL */
	"ucx",           /* FI_PROTO_UCX */
	"sm2",           /* FI_PROTO_SM2 */
	"cxi_rnr",       /* FI_PROTO_CXI_RNR */
	"lpp",           /* FI_PROTO_LPP */
	"lnx",           /* FI_PROTO_LNX */
};

const char *proto_name(uint32_t proto)
{
	if (proto < sizeof(PROTO_NAMES) / sizeof(PROTO_NAMES[0]))
		return PROTO_NAMES[proto];
	return NULL;
}

uint32_t ofi_rma_version(void)
{
	return OFI_RMA_VERSION(OFI_RMA_VERSION_MAJOR, OFI_RMA_VERSION_MINOR);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool parse_hex64(const char *s, size_t len, uint64_t *out)
{
	if (len == 0 || len > 16)
		return false;
	uint64_t v = 0;
	for (size_t i = 0; i < len; i++) {
		const int d = hexval(s[i]);
		if (d < 0)
			return false;
		v = (v << 4) | (uint64_t)d;
	}
	*out = v;
	return true;
}

/** hex digits into bytes: an even, nonzero count, at most max bytes */
static bool parse_bytes(const char *s, size_t len, uint8_t *out, size_t max,
			size_t *n)
{
	if (len == 0 || len % 2 || len / 2 > max)
		return false;
	for (size_t i = 0; i < len / 2; i++) {
		const int hi = hexval(s[2 * i]);
		const int lo = hexval(s[2 * i + 1]);
		if (hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	*n = len / 2;
	return true;
}

static bool wire_valid_n(const char *w, size_t len)
{
	/* <name>.<version>: a name of 1 to 31 of [a-z0-9_], a version of 1 to
	 * 8 hex digits */
	if (len == 0 || len >= OFI_RMA_WIRE_MAX)
		return false;
	size_t i = 0;
	while (i < len && ((w[i] >= 'a' && w[i] <= 'z') ||
			   (w[i] >= '0' && w[i] <= '9') || w[i] == '_'))
		i++;
	if (i == 0 || i > 31 || i == len || w[i] != '.')
		return false;
	const size_t ver = len - i - 1;
	if (ver == 0 || ver > 8)
		return false;
	for (size_t j = i + 1; j < len; j++) {
		if (!((w[j] >= '0' && w[j] <= '9') || (w[j] >= 'a' && w[j] <= 'f')))
			return false;
	}
	return true;
}

bool ofi_rma_wire_valid(const char *wire)
{
	return wire && wire_valid_n(wire, strnlen(wire, OFI_RMA_WIRE_MAX));
}

/** split s at colons into at most max fields; the number of fields, or
 * max + 1 when there are more */
static size_t split(const char *s, size_t len, const char **f, size_t *flen,
		    size_t max)
{
	size_t n = 0, start = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i < len && s[i] != ':')
			continue;
		if (n == max)
			return max + 1;
		f[n] = s + start;
		flen[n] = i - start;
		n++;
		start = i + 1;
	}
	return n;
}

int ofi_rma_token_parse(const char *s, size_t len, struct ofi_rma_token *t)
{
	const char *f[7];
	size_t fl[7];
	if (!s || !t || len >= OFI_RMA_TOKEN_MAX)
		return -EINVAL;
	const size_t n = split(s, len, f, fl, 7);
	if (n != 6 && n != 7)
		return -EINVAL;
	const size_t taglen = sizeof(OFI_RMA_TOKEN_TAG) - 1;
	if (fl[2] != taglen || memcmp(f[2], OFI_RMA_TOKEN_TAG, taglen) ||
	    !wire_valid_n(f[3], fl[3]))
		return -EINVAL;
	struct ofi_rma_token tok;
	memset(&tok, 0, sizeof(tok));
	if (!parse_hex64(f[0], fl[0], &tok.base) ||
	    !parse_hex64(f[1], fl[1], &tok.size) ||
	    !parse_hex64(f[5], fl[5], &tok.key) ||
	    !parse_bytes(f[4], fl[4], tok.addr, OFI_RMA_ADDR_MAX, &tok.addr_len))
		return -EINVAL;
	if (n == 7 &&
	    !parse_bytes(f[6], fl[6], tok.auth, OFI_RMA_AUTH_MAX, &tok.auth_len))
		return -EINVAL;
	memcpy(tok.wire, f[3], fl[3]);
	tok.wire[fl[3]] = '\0';
	*t = tok;
	return 0;
}

ssize_t ofi_rma_token_format(const struct ofi_rma_token *t, char *buf,
			     size_t buflen)
{
	static const char digits[] = "0123456789abcdef";
	if (!t || !ofi_rma_wire_valid(t->wire) || t->addr_len == 0 ||
	    t->addr_len > OFI_RMA_ADDR_MAX || t->auth_len > OFI_RMA_AUTH_MAX)
		return -EINVAL;
	/* the longest token fits OFI_RMA_TOKEN_MAX: 16+1+16+1+4+1+47+1+384+1+
	 * 16+1+128 bytes and a NUL, 618 */
	char out[OFI_RMA_TOKEN_MAX];
	int n = snprintf(out, sizeof(out), "%llx:%llx:%s:%s:",
			 (unsigned long long)t->base,
			 (unsigned long long)t->size, OFI_RMA_TOKEN_TAG, t->wire);
	size_t pos = (size_t)n;
	for (size_t i = 0; i < t->addr_len; i++) {
		out[pos++] = digits[t->addr[i] >> 4];
		out[pos++] = digits[t->addr[i] & 0xf];
	}
	n = snprintf(out + pos, sizeof(out) - pos, ":%llx",
		     (unsigned long long)t->key);
	pos += (size_t)n;
	if (t->auth_len) {
		out[pos++] = ':';
		for (size_t i = 0; i < t->auth_len; i++) {
			out[pos++] = digits[t->auth[i] >> 4];
			out[pos++] = digits[t->auth[i] & 0xf];
		}
	}
	out[pos] = '\0';
	if (buflen) {
		const size_t c = pos < buflen ? pos : buflen - 1;
		memcpy(buf, out, c);
		buf[c] = '\0';
	}
	return (ssize_t)pos;
}

unsigned ofi_rma_token_family(const char *s, size_t len)
{
	const char *f[4];
	size_t fl[4];
	if (!s)
		return 0;
	/* only the third field matters; a token has more than three */
	size_t n = 0, start = 0;
	for (size_t i = 0; i <= len && n < 4; i++) {
		if (i < len && s[i] != ':')
			continue;
		f[n] = s + start;
		fl[n] = i - start;
		n++;
		start = i + 1;
	}
	if (n < 4 || fl[2] < 4 || memcmp(f[2], "ofi", 3))
		return 0;
	unsigned v = 0;
	for (size_t i = 3; i < fl[2]; i++) {
		const char c = f[2][i];
		if (c < '0' || c > '9' || v > 100000)
			return 0;
		v = v * 10 + (unsigned)(c - '0');
	}
	return v;
}
