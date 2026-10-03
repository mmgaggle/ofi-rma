// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/uio.h>

/**
 * Out-of-band RMA over libfabric.
 *
 * One transport-neutral endpoint for every out-of-band leg Ceph runs
 * itself: an OSD writing a read reply into a client's memory, a shard
 * pushing its sub-read into a gathering primary, an OSD placing a
 * stripe in a gateway's relay window. The libfabric provider picks the
 * wire: tcp or shm with no special hardware, verbs (RC under the rxm
 * utility provider), efa (SRD), or a UET provider.
 *
 * The code has no Ceph dependencies, so test clients can link it alone.
 */
namespace ceph::ofi {

/**
 * A libfabric delivery descriptor:
 *
 *   <base hex>:<size hex>:ofi1:<provider>:<endpoint name hex>:<key hex>
 *
 * The addr:size prefix is the one every delivery token starts with. The
 * base is the remote address of window byte 0 as the window owner's
 * provider interprets it: a virtual address when the provider uses
 * FI_MR_VIRT_ADDR, an offset into the memory region otherwise. A writer
 * never needs to know which: it writes window byte i at base + i. The
 * provider is libfabric's name for it (fabric_attr->prov_name, for
 * example "tcp" or "verbs;ofi_rxm"), and both ends must run the same
 * one. The endpoint name is the owner's fi_getname() bytes, which the
 * writer passes to fi_av_insert(); the key is the region's fi_mr_key().
 */
struct token_t {
  uint64_t base = 0;
  uint64_t size = 0;
  std::string provider;
  std::string name;  ///< raw endpoint name bytes
  uint64_t key = 0;
};

inline constexpr std::string_view TOKEN_TAG = "ofi1";

std::optional<token_t> parse_token(std::string_view token);
std::string format_token(const token_t& t);

struct config_t {
  std::string provider;  ///< libfabric provider name; required
  std::string domain;    ///< domain (device or interface) name; optional
  std::string node;      ///< local address to bind; optional
  std::string service;   ///< local service (port) to bind; optional
  /// staging for outgoing writes: stage_count buffers of stage_size
  /// bytes each; 0 for an endpoint that only lends windows
  size_t stage_size = 0;
  size_t stage_count = 0;
  /// poll the completion queue from a thread of our own. Providers that
  /// progress manually place incoming writes only while polled, so an
  /// endpoint that lends windows needs this unless the application
  /// polls.
  bool progress_thread = false;
  /// ask for a thread-safe domain (FI_THREAD_SAFE), so that adding a new
  /// peer to the address vector, which can be slow, holds up no other
  /// write. A provider that offers only FI_THREAD_DOMAIN still works:
  /// see write(). False asks for FI_THREAD_DOMAIN outright.
  bool thread_safe = true;
  /// What a cut-off is expected to cost: a write stops waiting this long
  /// before its budget runs out, to cut its writes off in time, and is
  /// refused when its budget is no longer than that. The first value is a
  /// guess; each clean cut-off then moves the estimate to twice what it
  /// took plus 20 ms, at once when that is more, a quarter of the way
  /// when it is less. It never exceeds cutoff_cost_max, so a budget above
  /// that is never refused. A cut-off that takes longer than the estimate
  /// ends after the budget it protects: see write().
  std::chrono::milliseconds cutoff_cost_initial{100};
  std::chrono::milliseconds cutoff_cost_max{2000};
  /// A cut-off that ends after the budget it protects, by no more than
  /// this, and that itself took no longer, is counted (cutoffs_late) and
  /// the endpoint goes on; one beyond it makes the endpoint unsafe. A cut-
  /// off is usually late because the thread that should start it was not
  /// running: a loaded host, a scheduling delay. Those who wait for an
  /// OSD's reply are not affected, since the reply follows the cut-off;
  /// a client that gave a request up must allow this much beyond the
  /// pool's lease and drain.
  std::chrono::milliseconds late_tolerance{1000};
  /// A cut-off that took no longer than late_tolerance but ended later
  /// than it, because the thread that should have started it did not run
  /// in time (a process stopped, paused, swapped out or starved), leaves
  /// the endpoint clean: the close or cancel returned, so nothing of the
  /// writes it cut off is sent any more. By default it is counted
  /// (cutoffs_past_tolerance), said in last_error(), and the endpoint
  /// goes on. True makes the endpoint unsafe instead.
  bool late_fail_closed = false;
  /// for tests: runs in a waiting write, without the endpoint's lock,
  /// each time it wakes, with the time it is to cut its writes off; a
  /// test sleeps in it to stand for a writer the scheduler left idle
  std::function<void(std::chrono::steady_clock::time_point)> wait_hook;
  /// how long a memory key that left service (see rekey_window()) is not
  /// accepted again for a window: the longest a packet can stay in the
  /// network. A provider that reuses keys, as the UET reference provider
  /// does, could otherwise give a late write's key to another window.
  std::chrono::milliseconds key_quarantine{10000};
  /// memory regions to ask a provider for when it offers fewer. Keys in
  /// quarantine hold their place in a provider that reuses them, so this
  /// bounds re-keys to about mr_cnt per key_quarantine.
  size_t mr_cnt = 16384;
  /// Peers in the address vector at most. A first write to yet another
  /// peer evicts the least recently used ones that no write uses, and is
  /// refused (-EBUSY) when there are none.
  size_t max_peers = 1024;
  /// first contacts queued or running at once; a first write to yet
  /// another peer is refused (-EBUSY), so the queue cannot grow with the
  /// number of clients, which choose their endpoint names
  size_t max_pending_inserts = 64;
  /// threads adding peers, on a thread-safe domain; a FI_THREAD_DOMAIN
  /// domain has one, since an insert there stops every other call
  unsigned insert_threads = 4;
  /// for tests: runs on the insert thread just before each
  /// fi_av_insert(), with the peer's name, under the same locks
  std::function<void(const std::string&)> insert_hook;
  /// for tests: runs in write() after the source is gathered, without
  /// the endpoint's lock, just before the last look at the time left
  std::function<void()> pre_post_hook;
  /// Cut off a late write by cancelling only its own operations, and keep
  /// the endpoint and every other write in flight, when the provider
  /// promises that fi_cancel() discards an outstanding write: no packet
  /// of it is sent once the call returns, and it completes with
  /// FI_ECANCELED. The UET provider says so through an endpoint option
  /// (see cancel_discards in stats_t). Otherwise, or with this false, a
  /// cut-off closes and reopens the endpoint, which fails every write in
  /// flight on it.
  bool per_plan_cutoff = true;
  /// for tests: stands in for fi_cancel() of each outstanding operation
  /// of a late write, as a provider that discards would, and makes the
  /// endpoint cut off late writes one by one. Return 0 for discarded, or
  /// a negative errno for a cancel that failed. The provider itself keeps
  /// the operations, and the staging they read, until the endpoint
  /// closes.
  std::function<int()> cancel_hook;
  /// for tests: runs in a cut-off just before the endpoint is closed. A
  /// nonzero return stands for fi_close() failing with it, and the
  /// endpoint stays open.
  std::function<int()> cutoff_close_hook;
};

class Endpoint {
public:
  /// open the endpoint; on failure returns null and sets *err
  static std::unique_ptr<Endpoint> open(const config_t& cfg, std::string* err);
  ~Endpoint();

  Endpoint(const Endpoint&) = delete;
  Endpoint& operator=(const Endpoint&) = delete;

  /// the provider's own name for itself, as tokens carry it
  const std::string& provider() const;
  /// this endpoint's name (fi_getname bytes)
  const std::string& name() const;
  /// fabric, domain and memory-registration mode, for logs
  std::string describe() const;
  /// true when the provider promised FI_DELIVERY_COMPLETE: a write's
  /// completion means its bytes are in the target's memory
  bool delivery_complete() const;

  /// a registered window peers may write into
  struct window_t {
    uint64_t id = 0;
    char* ptr = nullptr;
    size_t len = 0;
  };
  /// register [ptr, ptr+len) for remote writes
  int register_window(char* ptr, size_t len, window_t* out);
  void deregister_window(uint64_t id);
  /**
   * Give a window a new memory key before its memory is reused for
   * another operation. A provider that can (the UET provider's
   * FI_UET_MR_REKEY) changes the key in place, keeping the memory's
   * registration; otherwise the memory is registered again and the old
   * region closed. Every token issued for it before stops working: a
   * write that still carries the old key fails instead of landing, as a
   * late duplicate of a write that completed long ago can over a provider
   * that retransmits without connection state (UET's RUDI). Call it only
   * when no write of the window's current operation is still expected.
   * Returns 0, or a negative errno with the window and its key unchanged;
   * do not reuse the memory then until key_quarantine has passed.
   */
  int rekey_window(const window_t& w);
  /// the token naming [ofs, ofs+len) of a registered window
  std::string window_token(const window_t& w, uint64_t ofs, uint64_t len) const;

  struct write_t {
    uint64_t src_ofs = 0;  ///< offset in the gathered source bytes
    uint64_t len = 0;
    uint64_t dst_ofs = 0;  ///< offset in the destination window
  };
  /**
   * Gather iov into a staging buffer and write each range into the
   * window dst names. Blocks until every write completed, or until the
   * budget runs out. Returns 0 or a negative errno: -EPROTONOSUPPORT
   * for another provider's token, -EBUSY when no staging buffer is
   * free, -E2BIG when the source does not fit one, -ETIMEDOUT when the
   * budget ran out, -ECANCELED when another write's cut-off took this
   * one's writes with it, -EIO when the endpoint takes no more writes.
   * After a failure the window may hold some of the bytes.
   *
   * -ENOTRECOVERABLE means a cut-off went wrong: closing the endpoint
   * failed, so its writes were not cut off, or the cut-off ended after
   * this write's budget. Bytes may land in the window after the caller
   * gave up. The endpoint is then unsafe() and takes no more writes.
   *
   * The budget bounds when the writes may still land. A write that is
   * still in flight when it runs out is cut off. When the provider
   * promises that cancelling a write discards it, only that write's
   * operations are cancelled, and every other write goes on (see
   * config_t::per_plan_cutoff). Otherwise the endpoint closes and
   * reopens itself, which cancels every operation it has outstanding,
   * so no retransmission reaches the peer later; that cut-off fails
   * every other write in flight on the endpoint too. A cancel that fails
   * falls back to closing the endpoint. The
   * endpoint stops waiting early by the measured cost of a cut-off.
   * Windows stay registered across a cut-off, but the endpoint's name
   * can change with it, so a token issued before it can stop working.
   *
   * A write that has too little of its budget left when it is about to
   * post, after waiting for its peer or the endpoint and gathering the
   * source, returns -ETIMEDOUT having sent nothing, so that it does not
   * post only to be cut off at once, with every other write in flight.
   * Too little is less than 1 ms beyond twice the time recent writes of
   * 1 MiB or more took for as many bytes.
   *
   * A write stops waiting, and cuts its writes off, ahead of its budget's
   * end by what a cut-off is expected to cost plus a scheduling slack,
   * learned from how late cut-offs actually started. Any thread that
   * polls the endpoint cuts off every write whose time has come, on its
   * writer's behalf, so a writer the scheduler left idle is cut off in
   * time anyway. A cut-off that still ends after the budget, within
   * config_t::late_tolerance, is counted and the write returns
   * -ETIMEDOUT. One that ends later still, though the close or cancel
   * itself was quick, is counted as past the tolerance, and the write
   * returns -ETIMEDOUT; the endpoint goes on, unless
   * config_t::late_fail_closed. A close or cancel that itself takes
   * longer than the tolerance makes the endpoint unsafe, and the writes
   * whose budgets ran out return -ENOTRECOVERABLE.
   *
   * The first write to a peer adds it to the address vector. Some
   * providers take long for that: the UET reference provider waits for
   * the peer's next hop to resolve, for up to a second, and older
   * versions pinged it, for up to 10 seconds. A thread of the endpoint's
   * own does the
   * insert, and writes to the same peer wait for it, each only until
   * its own budget runs out (-ETIMEDOUT, with nothing sent). The insert
   * goes on, so a later write finds the peer ready. On a thread-safe
   * domain, writes to other peers go on meanwhile. A provider that
   * offers only FI_THREAD_DOMAIN allows no other call during the
   * insert: the insert waits until the writes in flight are done, so
   * none of them misses its cut-off, and writes that start meanwhile
   * wait for it, again only within their budgets. So do progress(),
   * sync(), window_token() and the window calls, and with them a
   * progress thread that places incoming data. A first write to a new
   * peer returns -EBUSY at once, having sent nothing, when
   * max_pending_inserts are already under way, or when max_peers are
   * known and all of them are in use.
   */
  int write(const token_t& dst, const struct iovec* iov, size_t iovcnt,
	    const std::vector<write_t>& writes,
	    std::chrono::milliseconds budget);

  /// one pass over the completion queue
  void progress();
  /// order the caller's reads of window memory after writes this
  /// endpoint placed while being polled
  void sync();

  struct stats_t {
    uint64_t writes_posted = 0;
    uint64_t writes_failed = 0;
    uint64_t bytes_written = 0;
    uint64_t peers_inserted = 0;
    uint64_t staging_busy = 0;
    uint64_t timeouts = 0;
    uint64_t resets = 0;  ///< cut-offs: endpoint closed and reopened
    /// writes that gave up before sending anything: their peer's
    /// address insert, or the endpoint, did not get ready in the budget
    uint64_t peer_timeouts = 0;
    /// first writes to a new peer refused for max_pending_inserts or
    /// max_peers, with nothing sent; the peers known and being added
    uint64_t inserts_refused = 0;
    uint64_t peers = 0;
    uint64_t pending_inserts = 0;
    /// cut-offs whose close failed, and cut-offs that ended after a
    /// budget they protected
    uint64_t cutoffs_failed = 0;
    /// cut-offs that ended after the budget they protected, within
    /// late_tolerance or not; the worst lateness, and when the last one
    /// was; cut-offs done on a waiting writer's behalf by another thread
    uint64_t cutoffs_late = 0;
    uint64_t max_cutoff_lateness_ms = 0;
    std::chrono::steady_clock::time_point last_late_cutoff{};
    uint64_t cutoffs_on_behalf = 0;
    /// of cutoffs_late, those beyond late_tolerance that left the endpoint
    /// clean, and when the last was; the longest the endpoint went
    /// unpolled, a sign of its threads not running, and when
    uint64_t cutoffs_past_tolerance = 0;
    std::chrono::steady_clock::time_point last_past_cutoff{};
    uint64_t max_poll_gap_ms = 0;
    std::chrono::steady_clock::time_point last_long_poll_gap{};
    /// the scheduling slack a write keeps ahead of its budget's end, on
    /// top of cutoff_cost_ms, and the tolerance
    uint64_t cutoff_slack_ms = 0;
    uint64_t late_tolerance_ms = 0;
    /// late writes cut off one by one, by cancelling their operations; and
    /// cancels that failed, after which the endpoint was reset instead
    uint64_t plans_cut_off = 0;
    uint64_t cancels_failed = 0;
    /// the provider promises that fi_cancel() discards a write (see
    /// config_t::per_plan_cutoff); and that closing the endpoint discards
    /// its writes: 1 yes, 0 no, -1 it does not say
    bool cancel_discards = false;
    int close_discards = -1;
    /// writes refused, with nothing sent, for a budget no longer than
    /// the cut-off cost estimate, and that estimate
    uint64_t budget_refused = 0;
    uint64_t cutoff_cost_ms = 0;
    /// writes that did not start, with nothing sent, because too little
    /// of their budget was left to post them
    uint64_t late_starts = 0;
    /// windows given a new key, and keys a provider handed out again
    /// while still in quarantine, which were dropped
    uint64_t windows_rekeyed = 0;
    uint64_t key_collisions = 0;
    /// re-keys done in place, and by registering the memory again; and
    /// whether the provider re-keys in place: 1 yes, 0 no, -1 not tried
    uint64_t rekeys_in_place = 0;
    uint64_t rekeys_reregistered = 0;
    int rekey_in_place = -1;
    bool unsafe = false;
    /// takes no more writes: unsafe, or a cut-off could not reopen it
    bool broken = false;
    uint64_t windows = 0;
  };
  stats_t stats() const;
  /// the provider's text for the most recent failed completion
  std::string last_error() const;
  /// a cut-off failed or ended late (see write()); the endpoint takes no
  /// more writes, and writes it took may still land
  bool unsafe() const;
  /// config_t::key_quarantine
  std::chrono::milliseconds key_quarantine() const;

  struct Impl;
private:
  explicit Endpoint(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};

/**
 * Windows of one size that peers write into, lent for one operation at a
 * time and then reused, as an OSD's gather windows are. Each window has a
 * key of its own. On release a window can get a new one, so that no write
 * meant for an earlier operation lands in a later one: not a peer's that
 * missed its operation, and not a provider's late duplicate of one that
 * completed. A window that could not get a new key, or is released with
 * a quarantine, stays out of use that long.
 */
class WindowPool {
public:
  /// count windows of size bytes each, at mem; on failure, returns null
  /// and sets *err
  static std::unique_ptr<WindowPool> create(Endpoint& ep, char* mem,
					    size_t size, size_t count,
					    std::string* err);
  ~WindowPool();

  struct lent_t {
    uint64_t id = 0;
    char* ptr = nullptr;
    size_t size = 0;
    std::string token;  ///< for the whole window
  };
  /// a free window of at least size bytes, or nullopt
  std::optional<lent_t> acquire(size_t size);
  /**
   * Return a window. rekey gives it a new key first, after which no write
   * meant for its last operation can land: it is free at once. Without
   * rekey, or when the new key fails, it stays out of use for quarantine,
   * and for the endpoint's key_quarantine at least when the new key
   * failed. Call it only when no write of the operation is still
   * expected.
   */
  void release(uint64_t id, std::chrono::milliseconds quarantine, bool rekey);

  struct stats_t {
    uint64_t acquired = 0;
    uint64_t exhausted = 0;
    uint64_t rekeyed = 0;
    uint64_t rekey_failed = 0;
  };
  stats_t stats() const;
  size_t count() const { return slots.size(); }
  size_t size() const { return slot_size; }

private:
  WindowPool(Endpoint& ep, size_t size) : ep(ep), slot_size(size) {}
  struct slot_t {
    Endpoint::window_t w;
    bool in_use = false;
    std::chrono::steady_clock::time_point quarantined_until{};
  };
  Endpoint& ep;
  const size_t slot_size;
  mutable std::mutex mtx;
  std::vector<slot_t> slots;
  stats_t st;
};

} // namespace ceph::ofi
