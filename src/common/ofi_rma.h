// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
  /// for tests: runs on the insert thread just before each
  /// fi_av_insert(), with the peer's name, under the same locks
  std::function<void(const std::string&)> insert_hook;
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
   * still in flight when it runs out is cut off: the endpoint closes
   * and reopens itself, which cancels every operation it has
   * outstanding, so no retransmission reaches the peer later. The
   * cut-off fails every other write in flight on the endpoint too. The
   * endpoint stops waiting early by the measured cost of a cut-off.
   * Windows stay registered across a cut-off, but the endpoint's name
   * can change with it, so a token issued before it can stop working.
   *
   * The first write to a peer adds it to the address vector. Some
   * providers take long for that: the UET reference provider pings the
   * peer, for up to 10 seconds. A thread of the endpoint's own does the
   * insert, and writes to the same peer wait for it, each only until
   * its own budget runs out (-ETIMEDOUT, with nothing sent). The insert
   * goes on, so a later write finds the peer ready. On a thread-safe
   * domain, writes to other peers go on meanwhile. A provider that
   * offers only FI_THREAD_DOMAIN allows no other call during the
   * insert: the insert waits until the writes in flight are done, so
   * none of them misses its cut-off, and writes that start meanwhile
   * wait for it, again only within their budgets.
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
    /// cut-offs whose close failed, and cut-offs that ended after a
    /// budget they protected
    uint64_t cutoffs_failed = 0;
    uint64_t cutoffs_late = 0;
    /// writes refused, with nothing sent, for a budget no longer than
    /// the cut-off cost estimate, and that estimate
    uint64_t budget_refused = 0;
    uint64_t cutoff_cost_ms = 0;
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

  struct Impl;
private:
  explicit Endpoint(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};

} // namespace ceph::ofi
