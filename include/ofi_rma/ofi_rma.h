/* SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation */
/* SPDX-License-Identifier: MIT */

#ifndef OFI_RMA_OFI_RMA_H
#define OFI_RMA_OFI_RMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

/**
 * Out-of-band RMA over libfabric.
 *
 * One transport-neutral endpoint for moving data out of band into memory
 * that a peer lends. A window owner registers memory and hands out a
 * token that names it; any writer that holds the token and speaks the
 * same wire protocol writes into the memory, with no connection to the
 * owner. Ceph uses it for every out-of-band leg it runs itself: an OSD
 * writing a read reply into a client's memory, a shard pushing its
 * sub-read into a gathering primary, an OSD placing a stripe in a
 * gateway's relay window. A client such as an S3-over-RDMA data path
 * lends the memory. The libfabric provider picks the wire: tcp or shm
 * with no special hardware, verbs (RC under the rxm utility provider),
 * efa (SRD), or a UET provider.
 *
 * The library is written in C and depends only on libfabric and POSIX
 * threads. ofi_rma.hpp wraps this interface for C++.
 *
 * Every function that can fail returns 0, or a nonnegative count, on
 * success and a negative errno on failure. Times are in milliseconds,
 * and points in time are CLOCK_MONOTONIC nanoseconds, 0 for none.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define OFI_RMA_VERSION_MAJOR 0
#define OFI_RMA_VERSION_MINOR 4
#define OFI_RMA_VERSION(major, minor) (((major) << 16) | (minor))

/** OFI_RMA_VERSION() of the library that is running */
uint32_t ofi_rma_version(void);

/* ---------------------------------------------------------------------- */
/* Tokens                                                                 */
/* ---------------------------------------------------------------------- */

/**
 * A libfabric delivery descriptor:
 *
 *   <base>:<size>:ofi1:<wire>:<address>:<key>[:<auth>]
 *
 * Every field but the tag and the wire is lowercase hex. The base:size
 * prefix is the one every delivery token starts with, a cuObject
 * descriptor's too, so a server that only forwards a token can read the
 * window's size without parsing the rest. The tag tells the two apart: a
 * cuObject descriptor's third field is its memory key, in hex, and "ofi"
 * is not hex.
 *
 * The base is the remote address of window byte 0 as the window owner's
 * provider interprets it: a virtual address when the provider uses
 * FI_MR_VIRT_ADDR, an offset into the memory region otherwise. A writer
 * never needs to know which: it writes window byte i at base + i.
 *
 * The wire names the protocol on the wire and its version, as
 * <name>.<version hex>: libfabric's fi_ep_attr protocol and
 * protocol_version, for example "rxm.1" for verbs under ofi_rxm, "xnet.1"
 * for the tcp provider and "uet.10000" for UET (FI_VERSION(1, 0)). It
 * does not name the provider: two providers that speak the same protocol,
 * such as two vendors' UET providers, take each other's tokens. A writer
 * takes a token only when its own wire is the same.
 *
 * The address is the owner's fi_getname() bytes, which the writer passes
 * to fi_av_insert(), and the key is the region's fi_mr_key(). The auth
 * field, when present, is the libfabric authorization key the region is
 * bound to: a UET JobID, 3 bytes. It grants nothing by itself, since the
 * writer's own driver decides which keys the writer may use.
 */
#define OFI_RMA_TOKEN_TAG "ofi1"
/** longest token ofi_rma_token_format() writes, with its NUL: 618 bytes
 * with the longest of every field */
#define OFI_RMA_TOKEN_MAX 640
/** longest wire, with its NUL: a name of up to 31 characters, a dot and up
 * to 8 hex digits */
#define OFI_RMA_WIRE_MAX 48
/** longest endpoint address and authorization key, in bytes */
#define OFI_RMA_ADDR_MAX 192
#define OFI_RMA_AUTH_MAX 64

struct ofi_rma_token {
	uint64_t base;
	uint64_t size;
	char wire[OFI_RMA_WIRE_MAX];      /**< NUL-terminated */
	uint8_t addr[OFI_RMA_ADDR_MAX];   /**< the owner's endpoint name */
	size_t addr_len;
	uint64_t key;
	uint8_t auth[OFI_RMA_AUTH_MAX];   /**< empty when auth_len is 0 */
	size_t auth_len;
};

/**
 * Parse len bytes of s. Returns 0, or -EINVAL for anything that is not a
 * well-formed ofi1 token: another family's descriptor, another version's
 * token, a field out of range.
 */
int ofi_rma_token_parse(const char *s, size_t len, struct ofi_rma_token *t);

/**
 * Write t to buf as a NUL-terminated token. Returns the token's length
 * without the NUL, as snprintf() does, also when buflen is too small; or
 * -EINVAL for a token whose fields cannot be written.
 */
ssize_t ofi_rma_token_format(const struct ofi_rma_token *t, char *buf,
			     size_t buflen);

/**
 * The version of the ofi token family that s belongs to: N when its third
 * field is "ofi<N>", and 0 when it is not an ofi token at all, as a
 * cuObject descriptor is not. A daemon routes a token by this before it
 * parses it, so that a token of a version it does not know is declined
 * rather than handed to another family's code.
 */
unsigned ofi_rma_token_family(const char *s, size_t len);

/**
 * Whether a wire string is well formed: a name of lowercase letters,
 * digits and underscores, a dot, and a version in hex.
 */
bool ofi_rma_wire_valid(const char *wire);

/* ---------------------------------------------------------------------- */
/* Endpoints                                                              */
/* ---------------------------------------------------------------------- */

struct ofi_rma_ep;

/**
 * Where a window's memory lives. Host memory needs nothing more. Memory
 * on a GPU or another device needs a provider that offers FI_HMEM (see
 * ofi_rma_config.hmem), and a NIC and platform that can reach the
 * device's memory; a software provider such as shm copies into it
 * instead.
 */
enum ofi_rma_iface {
	OFI_RMA_IFACE_SYSTEM = 0,
	OFI_RMA_IFACE_CUDA,
	OFI_RMA_IFACE_ROCR,
	OFI_RMA_IFACE_ZE,
};

struct ofi_rma_memory {
	enum ofi_rma_iface iface;
	/** the device ordinal, for cuda and ze; rocr and system ignore it,
	 * since ROCr finds the device from the address */
	int device;
	/**
	 * A dma-buf that exports the memory, or -1. With one, the region is
	 * registered from the dma-buf (FI_MR_DMABUF), which a verbs provider
	 * hands to ibv_reg_dmabuf_mr(): the NIC then writes into the
	 * device's memory, and libfabric need not export it itself. Its own
	 * ROCr export needs FI_HMEM_ROCR_USE_DMABUF=1 and a kernel built
	 * with CONFIG_DMABUF_MOVE_NOTIFY, which some distribution kernels
	 * lack. The fd stays the caller's; keep it open while the window is
	 * registered, since a re-key or a cut-off registers the memory
	 * again. The provider must still offer FI_HMEM (ofi_rma_ep_hmem()).
	 */
	int dmabuf_fd;
	/** where the memory starts in the dma-buf: a window may begin inside
	 * the allocation that the dma-buf exports */
	uint64_t dmabuf_offset;
};

/** host memory: iface system, no dma-buf */
void ofi_rma_memory_init(struct ofi_rma_memory *mem);

/**
 * Callbacks for tests, each with the context ctx. A NULL callback is not
 * called.
 */
struct ofi_rma_hooks {
	void *ctx;
	/** runs in a waiting write, without the endpoint's lock, each time it
	 * wakes, with the time it is to cut its writes off; a test sleeps in
	 * it to stand for a writer the scheduler left idle */
	void (*wait)(void *ctx, int64_t deadline_ns);
	/** runs on the insert thread just before each fi_av_insert(), with
	 * the peer's name, under the same locks */
	void (*insert)(void *ctx, const void *name, size_t len);
	/** runs in a write after the source is gathered, without the
	 * endpoint's lock, just before the last look at the time left */
	void (*pre_post)(void *ctx);
	/** stands in for fi_cancel() of each outstanding operation of a late
	 * write, as a provider that discards would, and makes the endpoint
	 * cut off late writes one by one. Return 0 for discarded, or a
	 * negative errno for a cancel that failed. The provider itself keeps
	 * the operations, and the staging they read, until the endpoint
	 * closes. */
	int (*cancel)(void *ctx);
	/** runs for each memory region the endpoint registers, a window's or
	 * the staging buffer's, with its memory, under the endpoint's lock.
	 * On a GPU whose memory the host can also reach, a window registered
	 * again as host memory would still take writes, so a test asks this
	 * how it was registered. */
	void (*reg)(void *ctx, const void *ptr, size_t len,
		    const struct ofi_rma_memory *mem);
	/** runs in a cut-off just before the endpoint is closed. A nonzero
	 * return stands for fi_close() failing with it, and the endpoint
	 * stays open. */
	int (*cutoff_close)(void *ctx);
};

/**
 * How to open an endpoint. Fill it with ofi_rma_config_init(), which sets
 * size and every default, then change what you need. The library reads
 * only the first size bytes, so a program built against an older header
 * keeps working with a newer library.
 */
struct ofi_rma_config {
	size_t size;             /**< sizeof(struct ofi_rma_config) */
	const char *provider;    /**< libfabric provider name; required */
	const char *domain;      /**< domain (device or interface); or NULL */
	const char *node;        /**< local address to bind; or NULL */
	const char *service;     /**< local service (port) to bind; or NULL */
	/** staging for outgoing writes: stage_count buffers of stage_size
	 * bytes each; 0 for an endpoint that only lends windows. With
	 * stage_mem, stage_size is the largest buffer a transfer can take,
	 * and stage_count is not used. */
	size_t stage_size;
	size_t stage_count;
	/** poll the completion queue from a thread of our own. Providers
	 * that progress manually place incoming writes only while polled, so
	 * an endpoint that lends windows needs this unless the application
	 * polls. */
	bool progress_thread;
	/** ask the provider for FI_HMEM, so that windows can live in device
	 * memory (see ofi_rma_memory). A provider without it is still used,
	 * with host windows only: see ofi_rma_ep_hmem(). Off by default,
	 * since the question can make a provider choose other attributes. */
	bool hmem;
	/** ask the provider for RMA reads (FI_READ and FI_REMOTE_READ):
	 * ofi_rma_read() pulls bytes out of a peer's window, and a window
	 * registered with OFI_RMA_REMOTE_READ lets peers pull out of it, as
	 * an OSD pulls the payload of a PUT out of a client's memory. A
	 * provider without them is still used, for writes only: see
	 * ofi_rma_ep_reads(). Off by default. */
	bool reads;
	/** ask for a thread-safe domain (FI_THREAD_SAFE), so that adding a
	 * new peer to the address vector, which can be slow, holds up no
	 * other write. A provider that offers only FI_THREAD_DOMAIN still
	 * works: see ofi_rma_write(). False asks for FI_THREAD_DOMAIN
	 * outright. On by default. */
	bool thread_safe;
	/** What a cut-off is expected to cost: a write stops waiting this
	 * long before its budget runs out, to cut its writes off in time,
	 * and is refused when its budget is no longer than that. The first
	 * value is a guess (100); each clean cut-off then moves the estimate
	 * to twice what it took plus 20 ms, at once when that is more, a
	 * quarter of the way when it is less. It never exceeds
	 * cutoff_cost_max_ms (2000), so a budget above that is never
	 * refused. */
	int64_t cutoff_cost_initial_ms;
	int64_t cutoff_cost_max_ms;
	/** A cut-off that ends after the budget it protects, by no more than
	 * this (1000), and that itself took no longer, is counted
	 * (cutoffs_late) and the endpoint goes on. One whose close or cancel
	 * itself took longer makes the endpoint unsafe; for a quick one that
	 * ended later, see late_fail_closed. A cut-off is usually late
	 * because the thread that should start it was not running. Those who
	 * wait for an OSD's reply are not affected, since the reply follows
	 * the cut-off; a client that gave a request up must allow this much
	 * beyond the pool's lease and drain. */
	int64_t late_tolerance_ms;
	/** A cut-off that took no longer than late_tolerance_ms but ended
	 * later than it, because the thread that should have started it did
	 * not run in time, leaves the endpoint clean: the close or cancel
	 * returned, so nothing of the writes it cut off is sent any more. By
	 * default it is counted (cutoffs_past_tolerance), said in the last
	 * error, and the endpoint goes on. True makes the endpoint unsafe
	 * instead. */
	bool late_fail_closed;
	/** how long a memory key that left service (see
	 * ofi_rma_window_rekey()) is not accepted again for a window: the
	 * longest a packet can stay in the network (10000). A provider that
	 * reuses keys, as the UET reference provider does, could otherwise
	 * give a late write's key to another window. */
	int64_t key_quarantine_ms;
	/** memory regions to ask a provider for when it offers fewer
	 * (16384). Keys in quarantine hold their place in a provider that
	 * reuses them, so this bounds re-keys to about mr_cnt per
	 * key_quarantine_ms. */
	size_t mr_cnt;
	/** Peers in the address vector at most (1024). A first write to yet
	 * another peer evicts the least recently used ones that no write
	 * uses, and is refused (-EBUSY) when there are none. */
	size_t max_peers;
	/** first contacts queued or running at once (64); a first write to
	 * yet another peer is refused (-EBUSY), so the queue cannot grow with
	 * the number of clients, which choose their endpoint names */
	size_t max_pending_inserts;
	/** threads adding peers, on a thread-safe domain (4); a
	 * FI_THREAD_DOMAIN domain has one, since an insert there stops every
	 * other call */
	unsigned insert_threads;
	/** On a FI_THREAD_DOMAIN domain an insert holds the endpoint for as
	 * long as the provider takes, which can be seconds. Calls that need
	 * the endpoint meanwhile, or the peer being added, wait for it at
	 * most this long (10) and then go on without it, so that the thread
	 * making them, an OSD op thread say, is not held up. The wait counts
	 * from the call, or from when the endpoint went quiet, whichever came
	 * first. */
	int64_t insert_wait_ms;
	/** On a FI_THREAD_DOMAIN domain, how long after a window pool lent a
	 * window an insert waits for it to come back before it stops the
	 * endpoint, polling meanwhile (0: no window). Pushes into a window
	 * come until about this long after it was lent: for an OSD, the
	 * pool's lease plus drain. */
	int64_t lent_wait_ms;
	/** Cut off a late write by cancelling only its own operations, and
	 * keep the endpoint and every other write in flight, when the
	 * provider promises that fi_cancel() discards an outstanding write.
	 * The UET provider says so through an endpoint option (see
	 * cancel_discards in ofi_rma_stats). Otherwise, or with this false, a
	 * cut-off closes and reopens the endpoint, which fails every write in
	 * flight on it. On by default. */
	bool per_plan_cutoff;
	/** callbacks for tests, copied when the endpoint opens; or NULL */
	const struct ofi_rma_hooks *hooks;
	/**
	 * Staging in the caller's memory, so that one arena can serve
	 * several transports. When stage_mem is set, the endpoint registers
	 * its stage_mem_len bytes once, for its own access, instead of
	 * allocating staging buffers itself. Each write or read takes a
	 * buffer of it from stage_alloc, at most stage_size bytes, and gives
	 * it back with stage_free once no operation can touch it any more:
	 * after a cut-off, that can be when the endpoint closes.
	 * stage_alloc returns 0 and the offset of a buffer of at least len
	 * bytes, or -EBUSY when none is free. Both are called with the
	 * endpoint's lock held, so they must not call into the endpoint. The
	 * memory must stay valid until ofi_rma_ep_close() returns.
	 */
	void *stage_mem;
	size_t stage_mem_len;
	int (*stage_alloc)(void *ctx, size_t len, uint64_t *offset);
	void (*stage_free)(void *ctx, uint64_t offset, size_t len);
	void *stage_ctx;
};

/** every default, and size */
void ofi_rma_config_init(struct ofi_rma_config *cfg);

/**
 * Open an endpoint. On failure returns a negative errno and writes why
 * into err, when errlen is not 0.
 */
int ofi_rma_ep_open(const struct ofi_rma_config *cfg, struct ofi_rma_ep **ep,
		    char *err, size_t errlen);
/** close the endpoint and free it; NULL is ignored */
void ofi_rma_ep_close(struct ofi_rma_ep *ep);

/** the provider's own name for itself (fabric_attr->prov_name) */
const char *ofi_rma_ep_provider(const struct ofi_rma_ep *ep);
/** the wire protocol this endpoint speaks, as tokens carry it */
const char *ofi_rma_ep_wire(const struct ofi_rma_ep *ep);
/**
 * Copy this endpoint's name (fi_getname bytes) into buf. Returns its
 * length, also when buflen is smaller. The name can change with a
 * cut-off.
 */
size_t ofi_rma_ep_name(struct ofi_rma_ep *ep, void *buf, size_t buflen);
/** fabric, domain and memory-registration mode, for logs; snprintf()
 * semantics */
size_t ofi_rma_ep_describe(const struct ofi_rma_ep *ep, char *buf,
			   size_t buflen);
/** true when the provider promised FI_DELIVERY_COMPLETE: a write's
 * completion means its bytes are in the target's memory */
bool ofi_rma_ep_delivery_complete(const struct ofi_rma_ep *ep);
/** the provider offers FI_HMEM: windows may live in device memory */
bool ofi_rma_ep_hmem(const struct ofi_rma_ep *ep);
/** the provider offers RMA reads (see ofi_rma_config.reads):
 * ofi_rma_read() works, and windows can be registered with
 * OFI_RMA_REMOTE_READ */
bool ofi_rma_ep_reads(const struct ofi_rma_ep *ep);
/** the most recent failure, in words; snprintf() semantics */
size_t ofi_rma_ep_last_error(struct ofi_rma_ep *ep, char *buf, size_t buflen);
/** a cut-off failed (see ofi_rma_write()); the endpoint takes no more
 * writes, and writes it took may still land */
bool ofi_rma_ep_unsafe(const struct ofi_rma_ep *ep);
/** ofi_rma_config.key_quarantine_ms */
int64_t ofi_rma_ep_key_quarantine_ms(const struct ofi_rma_ep *ep);

/* ---------------------------------------------------------------------- */
/* Windows                                                                */
/* ---------------------------------------------------------------------- */

/** a registered window peers may access */
struct ofi_rma_window {
	uint64_t id;
	void *ptr;
	size_t len;
};

/** what a peer holding a window's token may do with it */
enum {
	OFI_RMA_REMOTE_WRITE = 1, /**< write into it, as an OSD delivering a GET */
	OFI_RMA_REMOTE_READ = 2,  /**< read out of it, as an OSD pulling a PUT */
};

/**
 * Register [ptr, ptr+len) of the memory mem names (NULL: host memory) as
 * a window that peers may access as access says, a mask of
 * OFI_RMA_REMOTE_WRITE and OFI_RMA_REMOTE_READ. Device memory without
 * ofi_rma_ep_hmem() is refused with -EOPNOTSUPP, and so is
 * OFI_RMA_REMOTE_READ without ofi_rma_ep_reads(). A token for a readable
 * window lets whoever holds it read the window's memory, so re-key the
 * window (ofi_rma_window_rekey()) once the operation it was lent for is
 * over. The window keeps its memory and access across re-keys and
 * cut-offs.
 */
int ofi_rma_window_register(struct ofi_rma_ep *ep, void *ptr, size_t len,
			    const struct ofi_rma_memory *mem, unsigned access,
			    struct ofi_rma_window *out);
void ofi_rma_window_deregister(struct ofi_rma_ep *ep, uint64_t id);

/**
 * Give a window a new memory key before its memory is reused for another
 * operation. A provider that can (the UET provider's FI_UET_MR_REKEY)
 * changes the key in place, keeping the memory's registration; otherwise
 * the memory is registered again and the old region closed. Every token
 * issued for it before stops working: a write that still carries the old
 * key fails instead of landing, as a late duplicate of a write that
 * completed long ago can over a provider that retransmits without
 * connection state (UET's RUDI). Call it only when no write of the
 * window's current operation is still expected.
 *
 * Returns 0, or a negative errno with the window and its key unchanged;
 * do not reuse the memory then until key_quarantine_ms has passed.
 * -EOPNOTSUPP means the provider's registration cache handed back the old
 * key; with such a provider (verbs and efa by default), closing a region
 * does not revoke its key either. Set FI_MR_CACHE_MONITOR=disabled in the
 * environment, before the first libfabric call. -EINPROGRESS is the
 * exception: an insert on a FI_THREAD_DOMAIN domain held the endpoint, in
 * the provider, past insert_wait_ms, and a later call that succeeds, once
 * ofi_rma_ep_quiet() is false, makes the memory reusable. No other
 * failure returns it.
 */
int ofi_rma_window_rekey(struct ofi_rma_ep *ep, const struct ofi_rma_window *w);

/**
 * Write the token naming [ofs, ofs+len) of a registered window to buf.
 * Returns its length, as ofi_rma_token_format() does, or a negative
 * errno: -ENOENT for a window it does not know, -ERANGE for a range
 * outside it, -EIO when the endpoint takes no more writes, -EINPROGRESS
 * when an insert on a FI_THREAD_DOMAIN domain held the endpoint, in the
 * provider, past insert_wait_ms.
 */
ssize_t ofi_rma_window_token(struct ofi_rma_ep *ep,
			     const struct ofi_rma_window *w, uint64_t ofs,
			     uint64_t len, char *buf, size_t buflen);

/* ---------------------------------------------------------------------- */
/* Writes and reads                                                       */
/* ---------------------------------------------------------------------- */

/** one range of a transfer: local is the offset in the caller's gathered
 * (write) or scattered (read) bytes, remote the offset in the peer's
 * window */
struct ofi_rma_range {
	uint64_t local;
	uint64_t len;
	uint64_t remote;
};

/**
 * Gather iov into a staging buffer and write each range into the window
 * dst names. Blocks until every write completed, or until the budget runs
 * out. Returns 0 or a negative errno: -EPROTONOSUPPORT for a token of
 * another wire, or one with an authorization key, which this version does
 * not use yet; -EBUSY when no staging buffer is free or an insert keeps
 * the endpoint (see below), -E2BIG when the source does not fit one,
 * -ETIMEDOUT when the budget ran out, -ECANCELED when another write's
 * cut-off took this one's writes with it, -EIO when the endpoint takes no
 * more writes. After a failure the window may hold some of the bytes.
 *
 * *posted, when posted is not NULL, says whether any write was handed to
 * the provider: false means that nothing of this call reached the window,
 * whatever it returns.
 *
 * -ENOTRECOVERABLE means a cut-off went wrong: closing the endpoint
 * failed, so its writes were not cut off, or a cut-off that made the
 * endpoint unsafe ended after this write's budget. Bytes may land in the
 * window after the caller gave up. The endpoint is then unsafe and takes
 * no more writes.
 *
 * The budget bounds when the writes may still land. A write that is
 * still in flight when it runs out is cut off. When the provider promises
 * that cancelling a write discards it, only that write's operations are
 * cancelled, and every other write goes on (see per_plan_cutoff).
 * Otherwise the endpoint closes and reopens itself, which cancels every
 * operation it has outstanding, so no retransmission reaches the peer
 * later; that cut-off fails every other write in flight on the endpoint
 * too. A cancel that fails falls back to closing the endpoint. Windows
 * stay registered across a cut-off, but the endpoint's name can change
 * with it, so a token issued before it can stop working.
 *
 * A write that has too little of its budget left when it is about to
 * post, after waiting for its peer or the endpoint and gathering the
 * source, returns -ETIMEDOUT having sent nothing, so that it does not
 * post only to be cut off at once, with every other write in flight. Too
 * little is less than 1 ms beyond twice the time recent writes of 1 MiB
 * or more took for as many bytes.
 *
 * A write stops waiting, and cuts its writes off, ahead of its budget's
 * end by what a cut-off is expected to cost plus a scheduling slack,
 * learned from how late cut-offs actually started. Any thread that polls
 * the endpoint cuts off every write whose time has come, on its writer's
 * behalf, so a writer the scheduler left idle is cut off in time anyway.
 * A cut-off that still ends after the budget, within late_tolerance_ms,
 * is counted and the write returns -ETIMEDOUT. One that ends later still,
 * though the close or cancel itself was quick, is counted as past the
 * tolerance, and the write returns -ETIMEDOUT; the endpoint goes on,
 * unless late_fail_closed. A close or cancel that itself takes longer
 * than the tolerance makes the endpoint unsafe, and the writes whose
 * budgets ran out return -ENOTRECOVERABLE.
 *
 * The first write to a peer adds it to the address vector. Some
 * providers take long for that: the UET reference provider waits for the
 * peer's next hop to resolve, for up to a second. A thread of the
 * endpoint's own does the insert, and writes to the same peer wait for
 * it. On a thread-safe domain each waits only until its own budget runs
 * out (-ETIMEDOUT, with nothing sent), and writes to other peers go on
 * meanwhile. The insert goes on, so a later write finds the peer ready. A
 * provider that offers only FI_THREAD_DOMAIN allows no other call during
 * the insert, and has the inserts run one at a time: the insert waits
 * until the writes in flight are done, so none of them misses its
 * cut-off, and for the windows a pool lent (see lent_wait_ms), and then
 * holds the endpoint until the provider returns. A write that starts
 * meanwhile, to any peer, and a write whose new peer is not ready wait
 * for an insert at most insert_wait_ms, not for their budgets, and then
 * return -EBUSY, having sent nothing. A first write to a new peer returns
 * -EBUSY at once, having sent nothing, when max_pending_inserts are
 * already under way, or when max_peers are known and all of them are in
 * use.
 */
int ofi_rma_write(struct ofi_rma_ep *ep, const struct ofi_rma_token *dst,
		  const struct iovec *iov, size_t iovcnt,
		  const struct ofi_rma_range *ranges, size_t nranges,
		  int64_t budget_ms, bool *posted);

/**
 * Read each range of the window src names into a staging buffer, and
 * once every read completed, scatter the staged bytes into iov. Blocks
 * until then, or until the budget runs out. The window must have been
 * registered with OFI_RMA_REMOTE_READ; the provider fails the reads of
 * one that was not, and the call returns its error. The ranges' local
 * offsets must cover the bytes of iov exactly once (-ERANGE otherwise),
 * so that no byte of an earlier transfer's staging reaches iov.
 *
 * Returns 0 or a negative errno, as ofi_rma_write() does, and -EOPNOTSUPP
 * when the endpoint has no staging or no reads. Nothing is copied into
 * iov unless every read completed: on failure iov is untouched. A read
 * still in flight when the budget runs out is cut off as a write is, and
 * can only land in its staging, which no other transfer uses until the
 * read completes or the endpoint closes. Reads and writes share the
 * staging buffers, the peers, the budget rules and the cut-offs.
 */
int ofi_rma_read(struct ofi_rma_ep *ep, const struct ofi_rma_token *src,
		 const struct iovec *iov, size_t iovcnt,
		 const struct ofi_rma_range *ranges, size_t nranges,
		 int64_t budget_ms, bool *posted);

/** one pass over the completion queue */
void ofi_rma_ep_progress(struct ofi_rma_ep *ep);
/**
 * Order the caller's reads of window memory after writes this endpoint
 * placed while being polled. Nothing polls it while an insert on a
 * FI_THREAD_DOMAIN domain is in the provider: after insert_wait_ms, the
 * reads are ordered after the last poll before the insert. For a window
 * in device memory this orders the host's view only.
 */
void ofi_rma_ep_sync(struct ofi_rma_ep *ep);
/** an insert on a FI_THREAD_DOMAIN domain keeps the endpoint quiet: it
 * waits for the writes in flight, or runs, and no write starts */
bool ofi_rma_ep_quiet(const struct ofi_rma_ep *ep);
/** for a window pool: when the latest of its windows still lent was
 * lent, or 0 when none is. An insert on a FI_THREAD_DOMAIN domain waits
 * until lent_wait_ms after it. */
void ofi_rma_ep_windows_lent(struct ofi_rma_ep *ep, int64_t latest_ns);

struct ofi_rma_stats {
	size_t size;  /**< set by the caller: sizeof(struct ofi_rma_stats) */
	uint64_t writes_posted;
	uint64_t writes_failed;
	uint64_t bytes_written;
	uint64_t reads_posted;
	uint64_t reads_failed;
	uint64_t bytes_read;
	uint64_t peers_inserted;
	uint64_t staging_busy;
	uint64_t timeouts;
	uint64_t resets;  /**< cut-offs: endpoint closed and reopened */
	/** writes that gave up before sending anything: their peer's
	 * address insert, or the endpoint, did not get ready in the budget
	 * (on a FI_THREAD_DOMAIN domain, waits for an insert count as
	 * insert_busy instead) */
	uint64_t peer_timeouts;
	/** first writes to a new peer refused for max_pending_inserts or
	 * max_peers, with nothing sent; the peers known and being added */
	uint64_t inserts_refused;
	uint64_t peers;
	uint64_t pending_inserts;
	/** calls that gave up on an insert on a FI_THREAD_DOMAIN domain,
	 * having waited for it at most insert_wait_ms */
	uint64_t insert_busy;
	/** cut-offs whose close failed, and cut-offs that ended after a
	 * budget they protected */
	uint64_t cutoffs_failed;
	/** cut-offs that ended after the budget they protected, within
	 * late_tolerance_ms or not; the worst lateness, and when the last
	 * one was; cut-offs done on a waiting writer's behalf by another
	 * thread */
	uint64_t cutoffs_late;
	uint64_t max_cutoff_lateness_ms;
	int64_t last_late_cutoff_ns;
	uint64_t cutoffs_on_behalf;
	/** of cutoffs_late, those beyond late_tolerance_ms that left the
	 * endpoint clean, and when the last was; the longest the endpoint
	 * went unpolled, a sign of its threads not running, and when */
	uint64_t cutoffs_past_tolerance;
	int64_t last_past_cutoff_ns;
	uint64_t max_poll_gap_ms;
	int64_t last_long_poll_gap_ns;
	/** the scheduling slack a write keeps ahead of its budget's end, on
	 * top of cutoff_cost_ms, and the tolerance */
	uint64_t cutoff_slack_ms;
	uint64_t late_tolerance_ms;
	/** late writes cut off one by one, by cancelling their operations;
	 * and cancels that failed, after which the endpoint was reset */
	uint64_t plans_cut_off;
	uint64_t cancels_failed;
	/** the provider promises that fi_cancel() discards a write; and that
	 * closing the endpoint discards its writes: 1 yes, 0 no, -1 it does
	 * not say */
	bool cancel_discards;
	int close_discards;
	/** writes refused, with nothing sent, for a budget no longer than
	 * the cut-off cost estimate, and that estimate */
	uint64_t budget_refused;
	uint64_t cutoff_cost_ms;
	/** writes that did not start, with nothing sent, because too little
	 * of their budget was left to post them */
	uint64_t late_starts;
	/** windows given a new key, and keys a provider handed out again
	 * while still in quarantine, which were dropped */
	uint64_t windows_rekeyed;
	uint64_t key_collisions;
	/** re-keys done in place, and by registering the memory again; and
	 * whether the provider re-keys in place: 1 yes, 0 no, -1 not tried */
	uint64_t rekeys_in_place;
	uint64_t rekeys_reregistered;
	int rekey_in_place;
	bool unsafe;
	/** takes no more writes: unsafe, or a cut-off could not reopen it */
	bool broken;
	uint64_t windows;
};

/** fill the first s->size bytes of *s */
void ofi_rma_ep_get_stats(struct ofi_rma_ep *ep, struct ofi_rma_stats *s);

/* ---------------------------------------------------------------------- */
/* Window pools                                                           */
/* ---------------------------------------------------------------------- */

/**
 * Windows of one size that peers write into, lent for one operation at a
 * time and then reused, as an OSD's gather windows are. Each window has a
 * key of its own. On release a window can get a new one, so that no write
 * meant for an earlier operation lands in a later one: not a peer's that
 * missed its operation, and not a provider's late duplicate of one that
 * completed. A window released with a quarantine, or that could not get a
 * new key, stays out of use that long.
 *
 * Neither lending nor returning a window waits for an insert on a
 * FI_THREAD_DOMAIN domain: no window is lent while one keeps the endpoint
 * quiet, and a window returned meanwhile gets its new key from an
 * acquire after it. Such an insert waits for the windows lent before it
 * to come back, within lent_wait_ms, so that the pushes into them land
 * first.
 */
struct ofi_rma_pool;

/**
 * count windows of size bytes each, at mem, on ep, which must outlive the
 * pool. On failure returns a negative errno and writes why into err.
 */
int ofi_rma_pool_create(struct ofi_rma_ep *ep, void *mem, size_t size,
			size_t count, struct ofi_rma_pool **pool, char *err,
			size_t errlen);
/** deregister the windows and free the pool; NULL is ignored */
void ofi_rma_pool_destroy(struct ofi_rma_pool *pool);

struct ofi_rma_lent {
	uint64_t id;
	void *ptr;
	size_t size;
	char token[OFI_RMA_TOKEN_MAX];  /**< for the whole window */
};

/**
 * Lend a free window of at least size bytes. Returns 0, or -E2BIG for a
 * size larger than the windows, -EBUSY when none is free, -EINPROGRESS
 * while an insert on a FI_THREAD_DOMAIN domain keeps the endpoint quiet,
 * and -EIO when the endpoint lends no more.
 */
int ofi_rma_pool_acquire(struct ofi_rma_pool *pool, size_t size,
			 struct ofi_rma_lent *out);

/**
 * Return a window. rekey gives it a new key first. A window released
 * without a quarantine - its operation's writes all completed - is then
 * free at once: a late duplicate of one carries the old key.
 *
 * A quarantine says the operation's writes did not all complete: one
 * failed, was cut off, or never reported back, and its packets may still
 * be on the way. The new key keeps them out only if the provider drops
 * what carries the old one, so the window stays out of use for the
 * quarantine anyway, and for the endpoint's key quarantine at least when
 * it was re-keyed or the new key failed.
 *
 * A re-key that an insert on a FI_THREAD_DOMAIN domain keeps from the
 * endpoint is put off: an acquire that finds the endpoint free does it,
 * one window a call, and the window is not lent before. Its quarantine
 * then counts from the re-key.
 */
void ofi_rma_pool_release(struct ofi_rma_pool *pool, uint64_t id,
			  int64_t quarantine_ms, bool rekey);

struct ofi_rma_pool_stats {
	size_t size;  /**< set by the caller */
	uint64_t acquired;
	uint64_t exhausted;
	uint64_t rekeyed;
	/** re-keyed, and quarantined all the same: released after a write
	 * that did not complete */
	uint64_t rekeyed_quarantined;
	uint64_t rekey_failed;
	/** acquires that lent nothing while an insert on a FI_THREAD_DOMAIN
	 * domain kept the endpoint quiet, one per window asked for, and
	 * re-keys such an insert put off */
	uint64_t declined_insert;
	uint64_t rekeys_put_off;
};

void ofi_rma_pool_get_stats(struct ofi_rma_pool *pool,
			    struct ofi_rma_pool_stats *s);
size_t ofi_rma_pool_count(const struct ofi_rma_pool *pool);
size_t ofi_rma_pool_window_size(const struct ofi_rma_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* OFI_RMA_OFI_RMA_H */
