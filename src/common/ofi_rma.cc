// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

#include "common/ofi_rma.h"

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <poll.h>

namespace ceph::ofi {

namespace {

/// largest single write; plans are a stripe or a shard extent, so this
/// only splits unusually large ranges
constexpr uint64_t MAX_WRITE = 8ull << 20;
/// longest endpoint name a token may carry, in bytes
constexpr size_t MAX_NAME = 192;
/// how long a failed address insert is remembered: writes to the peer
/// fail at once until then, instead of queueing another slow insert
constexpr std::chrono::seconds INSERT_RETRY{1};
/// A write that polls for the others sleeps between polls only when the
/// completion queue has no wait object. It polls with only a yield in
/// between until SPIN_TIME has passed without a sign of work, then sleeps
/// WAIT_SLEEP between polls. A sign of work is a completion, or a poll
/// that took clearly longer than an idle one (POLL_BUSY_MARGIN beyond
/// twice the shortest poll seen): a provider that progresses only while
/// polled, as the UET reference provider does, moves data in its polls
/// long before the write completes. A sleep lasts about 50 us more than
/// asked, the kernel's default timer slack.
constexpr std::chrono::microseconds SPIN_TIME{200};
constexpr std::chrono::microseconds WAIT_SLEEP{50};
constexpr std::chrono::microseconds POLL_BUSY_MARGIN{2};
/// registrations a window may go through for a key out of quarantine
constexpr int REKEY_ATTEMPTS = 8;
/// Options of the UET libfabric provider, at FI_OPT_ENDPOINT, read with
/// fi_getopt() into a bool (FI_PROV_SPECIFIC, 1 << 31, plus a code; see
/// its prov/uet_fi.h): whether closing the endpoint discards the writes it
/// has outstanding, and whether fi_cancel() discards one. Other providers
/// do not know them.
constexpr int OPT_CLOSE_DISCARDS = static_cast<int>((1U << 31) | 0x5545U);
constexpr int OPT_CANCEL_DISCARDS = static_cast<int>((1U << 31) | 0x5543U);
/// fi_control() command of the UET libfabric provider (FI_UET_MR_REKEY in
/// its prov/uet_fi.h): give a region a new key in place, returned in a
/// uint64_t, the old key dead when the call returns; -FI_ENOSYS from a
/// provider or device that cannot
constexpr int MR_REKEY = static_cast<int>((1U << 31) | 0x554bU);
/// the least time a write must have left to post; see write()
constexpr std::chrono::milliseconds POST_MARGIN{1};
/// plans this large or larger teach the endpoint how fast plans move;
/// smaller ones take as long as the network's latency, whatever their size
constexpr uint64_t RATE_MIN_BYTES = 1 << 20;
/// the scheduling slack a write keeps ahead of its budget's end before
/// any cut-off was late, and the least it keeps; see note_notice_delay()
constexpr std::chrono::milliseconds SLACK_INITIAL{50};
constexpr std::chrono::milliseconds SLACK_MIN{10};
/// a waiting write that finds no poll for this long polls itself, and
/// cuts off what is due: the poller may be off the CPU
constexpr std::chrono::milliseconds POLLER_STALE{5};
/// longest a write blocks on the completion queue's wait object before
/// it looks again; bounds how late it notices a cut-off
constexpr std::chrono::milliseconds WAIT_MAX{1};
/// the API version asked for: old enough for a provider built against
/// 1.x headers, new enough for FI_CONTEXT2 and the mr_mode bits used
constexpr uint32_t API_VERSION = FI_VERSION(1, 18);

std::optional<uint64_t> parse_hex64(std::string_view s)
{
  if (s.empty() || s.size() > 16) {
    return std::nullopt;
  }
  uint64_t v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return std::nullopt;
    v = (v << 4) | d;
  }
  return v;
}

int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool valid_provider(std::string_view p)
{
  if (p.empty() || p.size() > 64) {
    return false;
  }
  return std::all_of(p.begin(), p.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == ';' ||
      c == '_' || c == '-' || c == '.';
  });
}

/// libfabric's codes below 256 are errno values; the rest are its own
int to_errno(ssize_t r)
{
  const ssize_t e = r < 0 ? -r : r;
  if (e > 0 && e < 256) {
    return -static_cast<int>(e);
  }
  return -EIO;
}

std::string fi_err(ssize_t r)
{
  return fi_strerror(static_cast<int>(r < 0 ? -r : r));
}

} // anonymous namespace

std::optional<token_t> parse_token(std::string_view token)
{
  std::string_view f[6];
  size_t n = 0;
  while (true) {
    if (n == 6) {
      return std::nullopt;
    }
    const auto colon = token.find(':');
    f[n++] = token.substr(0, colon);
    if (colon == std::string_view::npos) {
      break;
    }
    token.remove_prefix(colon + 1);
  }
  if (n != 6 || f[2] != TOKEN_TAG || !valid_provider(f[3])) {
    return std::nullopt;
  }
  auto base = parse_hex64(f[0]);
  auto size = parse_hex64(f[1]);
  auto key = parse_hex64(f[5]);
  const auto hex = f[4];
  if (!base || !size || !key || hex.empty() || hex.size() % 2 ||
      hex.size() > 2 * MAX_NAME) {
    return std::nullopt;
  }
  token_t t;
  t.base = *base;
  t.size = *size;
  t.key = *key;
  t.provider = std::string{f[3]};
  t.name.resize(hex.size() / 2);
  for (size_t i = 0; i < t.name.size(); i++) {
    const int hi = hexval(hex[2 * i]);
    const int lo = hexval(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    t.name[i] = static_cast<char>((hi << 4) | lo);
  }
  return t;
}

std::string format_token(const token_t& t)
{
  static constexpr char digits[] = "0123456789abcdef";
  char head[64];
  snprintf(head, sizeof(head), "%llx:%llx:",
	   static_cast<unsigned long long>(t.base),
	   static_cast<unsigned long long>(t.size));
  std::string s = head;
  s += TOKEN_TAG;
  s += ':';
  s += t.provider;
  s += ':';
  for (unsigned char c : t.name) {
    s += digits[c >> 4];
    s += digits[c & 0xf];
  }
  char tail[24];
  snprintf(tail, sizeof(tail), ":%llx", static_cast<unsigned long long>(t.key));
  s += tail;
  return s;
}

struct Endpoint::Impl {
  config_t cfg;
  fi_info* info = nullptr;
  fid_fabric* fabric = nullptr;
  fid_domain* domain = nullptr;
  fid_av* av = nullptr;
  fid_cq* cq = nullptr;
  fid_ep* ep = nullptr;
  /// the completion queue's wait object, or -1 when it has none
  int cq_fd = -1;
  std::string prov;
  std::string my_name;
  uint64_t mr_mode = 0;
  bool dc = false;
  /// the domain is FI_THREAD_SAFE: an address insert needs no lock
  bool thread_safe = false;
  uint64_t max_write = MAX_WRITE;
  uint64_t next_key = 1;

  /// every libfabric call on this endpoint runs under this lock, except
  /// an address insert on a thread-safe domain, and so does every touch
  /// of the state below. Timed, so that a write waits for it only until
  /// its deadline.
  mutable std::timed_mutex mtx;
  /// set while an insert on a FI_THREAD_DOMAIN domain waits for the
  /// writes in flight to finish, and while it runs: no write starts
  bool quiesce = false;

  struct region_t {
    fid_mr* mr = nullptr;
    char* ptr = nullptr;
    size_t len = 0;
    uint64_t key = 0;
    void* desc = nullptr;
  };
  std::map<uint64_t, region_t> windows;
  std::atomic<uint64_t> nwindows{0};  ///< windows.size(), readable without mtx
  uint64_t next_window = 0;
  /// keys of windows' regions that left service, until when each is in
  /// quarantine: not accepted again for a window
  std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> retired;
  std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>>
    retired_order;
  std::atomic<uint64_t> windows_rekeyed{0};
  std::atomic<uint64_t> key_collisions{0};
  std::atomic<uint64_t> rekeys_in_place{0};
  std::atomic<uint64_t> rekeys_reregistered{0};
  /// the provider re-keys a region in place: 1 yes, 0 no, -1 not tried;
  /// once it says it cannot, it is not asked again
  std::atomic<int> rekey_in_place{-1};
  /// register a window's memory, under a key out of quarantine
  int reg_window_locked(char* ptr, size_t len, region_t* out);
  /// close a window's region, and put its key in quarantine
  int close_window_region_locked(region_t& r);

  char* stage = nullptr;
  region_t stage_mr;
  std::vector<bool> stage_busy;

  /// The address vector's peers. They have a lock of their own, so that
  /// a write can wait for a peer's insert without mtx, which an insert
  /// on a FI_THREAD_DOMAIN domain holds. No provider call runs under it.
  /// Lock order: mtx, then peer_mtx.
  enum class peer_state {
    pending,   ///< queued for an insert, or being inserted
    ready,
    failed,    ///< the insert failed; tried again after retry_at
    removing,  ///< evicted, and being removed from the address vector
  };
  struct peer_t {
    /// the name, zero-padded so the provider never reads past it, and
    /// kept for the entry's life (some providers keep the pointer)
    std::string addr_buf;
    fi_addr_t addr = FI_ADDR_NOTAVAIL;
    peer_state state = peer_state::pending;
    int err = 0;  ///< why the insert failed
    std::chrono::steady_clock::time_point retry_at{};
    std::chrono::steady_clock::time_point used{};
    /// writes that hold the address: from the lookup until they retire.
    /// The entry is not dropped while any do.
    uint32_t refs = 0;
  };
  std::mutex peer_mtx;
  std::condition_variable peer_cv;    ///< a peer settled
  std::condition_variable insert_cv;  ///< insert_q or remove_q has work
  std::map<std::string, peer_t> peers;
  std::deque<std::string> insert_q;
  std::deque<std::string> remove_q;
  size_t pending = 0;   ///< peers in state pending
  size_t removing = 0;  ///< peers in state removing
  std::atomic<uint64_t> inserts_refused{0};
  std::vector<std::thread> insert_thrs;

  struct plan_t;
  /// one posted write; the context must stay put until it completes
  struct op_t {
    fi_context2 ctx;
    plan_t* plan = nullptr;
    bool done = false;  ///< its completion arrived
  };
  struct plan_t {
    std::vector<op_t> ops;  ///< reserved up front, never reallocated
    uint32_t outstanding = 0;
    int err = 0;
    /// when the caller's budget runs out: nothing may land after it
    std::chrono::steady_clock::time_point budget_end{};
    /// when its first write was posted
    std::chrono::steady_clock::time_point posted_at{};
    size_t slot = 0;
    std::string peer;
    /// its writer cancelled it and left; it is retired when the last of
    /// its cancelled operations completes, or when the endpoint closes
    bool abandoned = false;
    /// when its writes are to be cut off if still in flight
    std::chrono::steady_clock::time_point deadline{};
    /// cut off by cancelling its operations, by its writer or on its
    /// behalf, with what its writer returns; hooked when a test's cancel
    /// hook stood for the provider, which keeps the plan
    bool cut = false;
    bool hooked = false;
    bool released = false;  ///< hooked: the endpoint closed, slot given back
    int cut_result = 0;
    /// the plan whose deadline a reset is for
    bool due = false;
    std::list<std::unique_ptr<plan_t>>::iterator self;
  };
  std::list<std::unique_ptr<plan_t>> plans;
  /// plans a test's cancel_hook cancelled: the provider still holds their
  /// operations, and reads their staging, until the endpoint closes. They
  /// give their slots back then, and stay until the endpoint is destroyed,
  /// since their writers may still look at them.
  std::list<std::unique_ptr<plan_t>> hook_cancelled;
  /// fi_cancel() discards a write (OPT_CANCEL_DISCARDS); probed whenever
  /// the endpoint opens
  std::atomic<bool> cancel_discards{false};
  /// closing the endpoint discards its writes (OPT_CLOSE_DISCARDS): 1 yes,
  /// 0 no, -1 the provider does not say
  std::atomic<int> close_discards{-1};
  std::atomic<uint64_t> plans_cut_off{0};
  std::atomic<uint64_t> cancels_failed{0};
  /// Waiting writes. One of them, the poller, polls the completion queue
  /// for all; the others sleep on done_cv until a plan finishes, the
  /// poller leaves, or a cut-off fails them.
  std::condition_variable_any done_cv;
  bool poller = false;
  /// the shortest poll the poller has timed: what a poll costs when the
  /// provider has nothing to do
  std::chrono::steady_clock::duration poll_floor =
    std::chrono::steady_clock::duration::max();

  std::thread progress_thr;
  std::atomic<bool> stopping{false};

  /// the provider's text for the latest failure; a lock of its own, so
  /// that reading it never waits for an insert that holds mtx
  mutable std::mutex err_mtx;
  std::string last_err;
  void set_err(std::string e) {
    std::lock_guard el(err_mtx);
    last_err = std::move(e);
  }
  std::string get_err() const {
    std::lock_guard el(err_mtx);
    return last_err;
  }
  std::atomic<uint64_t> writes_posted{0};
  std::atomic<uint64_t> writes_failed{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> peers_inserted{0};
  std::atomic<uint64_t> staging_busy{0};
  std::atomic<uint64_t> timeouts{0};
  std::atomic<uint64_t> peer_timeouts{0};

  /// set when a cut-off could not reopen the endpoint, or when the
  /// endpoint is unsafe; every write fails from then on
  std::atomic<bool> broken{false};
  /// A cut-off failed, or ended after a budget it protected: writes may
  /// have landed, or may still land, after their callers gave up. Nothing
  /// calls into the provider for this endpoint again, except to close it.
  std::atomic<bool> unsafe{false};
  /// the endpoint did not close in a cut-off, and is still open
  bool zombie = false;
  std::atomic<uint64_t> resets{0};
  std::atomic<uint64_t> cutoffs_failed{0};
  std::atomic<uint64_t> cutoffs_late{0};
  /// what a cut-off is expected to cost, in milliseconds; a write stops
  /// waiting this much before its budget runs out. See config_t.
  std::atomic<int64_t> cutoff_cost_ms{100};
  /// the scheduling slack, in milliseconds: how late cut-offs start
  std::atomic<int64_t> cutoff_slack_ms{SLACK_INITIAL.count()};
  std::atomic<uint64_t> budget_refused{0};
  std::atomic<uint64_t> cutoffs_on_behalf{0};
  std::atomic<uint64_t> max_lateness_ms{0};
  std::atomic<int64_t> last_late_ns{0};  ///< steady clock, since its epoch
  std::atomic<uint64_t> cutoffs_past_tolerance{0};
  std::atomic<int64_t> last_past_ns{0};
  /// the gap between the last two polls of the endpoint, the longest, and
  /// when that was: a long one means its threads did not run
  std::chrono::steady_clock::duration last_poll_gap{};
  std::atomic<uint64_t> max_poll_gap_ms{0};
  std::atomic<int64_t> last_long_gap_ns{0};
  /// how late the cut-off being judged started, for its message
  std::chrono::steady_clock::duration notice_delay{};
  /// when the completion queue was last polled
  std::chrono::steady_clock::time_point last_poll{};
  /// fold how long after a plan's deadline its cut-off started into the
  /// scheduling slack
  void note_notice_delay(std::chrono::steady_clock::duration d);
  /// cut a plan's writes off: cancel them when the provider discards a
  /// cancelled write, else reset the endpoint
  void cut_off_locked(plan_t* p, std::chrono::steady_clock::time_point noticed);
  /// cut off every plan whose deadline passed, on its writer's behalf
  void cut_due_locked();
  /// judge a cut-off that ran from t0 to cut against the earliest budget
  /// end of the plans it cut: record lateness, and say why in
  /// last_error(); false when it is beyond the tolerance
  enum class verdict { in_time, late, past_tolerance, unsafe };
  verdict judge_cutoff_locked(std::chrono::steady_clock::time_point t0,
			   std::chrono::steady_clock::time_point cut,
			   std::chrono::steady_clock::time_point budget_end);
  std::atomic<uint64_t> late_starts{0};
  /// how fast plans of RATE_MIN_BYTES or more complete, from the first
  /// post to the last completion, in bytes per nanosecond, averaged; 0
  /// until one has
  double plan_rate = 0;
  /// whether a plan of this many bytes can still be posted and complete
  /// before deadline, by plan_rate
  bool time_to_post_locked(uint64_t bytes,
			   std::chrono::steady_clock::time_point deadline) const;
  /// fold a clean cut-off's duration into cutoff_cost_ms
  void note_cutoff_cost(std::chrono::steady_clock::duration took);

  ~Impl();
  /// open the completion queue and the endpoint, bind and enable them,
  /// and read the endpoint's name
  int open_ep(std::string* err);
  /// cut off every write in flight by closing and reopening the
  /// endpoint; plans still waiting fail with -ECANCELED, or with
  /// -ENOTRECOVERABLE when the cut-off failed or ended after their budget
  void reset_locked();
  /// a cut-off went wrong: fail every waiting plan with -ENOTRECOVERABLE
  /// and take no more writes
  void make_unsafe_locked(std::string why);
  /// a cut-off was too late: take no more writes, close the endpoint to
  /// stop the writes still in flight, and fail those whose budget ran
  /// out with -ENOTRECOVERABLE
  void stop_unsafe_locked(std::string why);
  /// close the endpoint and its completion queue, and free what only the
  /// endpoint still held; on failure the endpoint stays open, a zombie
  /// nothing calls into again
  int close_ep_locked();
  /// cancel the operations of a plan that have not completed; *simulated
  /// when a test's cancel_hook stood for the provider. 0, or the
  /// libfabric error of the first cancel that failed.
  int cancel_plan_locked(plan_t* p, bool* simulated);
  /// register memory; a region others write into needs a key, a local
  /// write source only a descriptor
  int reg(char* ptr, size_t len, uint64_t access, region_t* out);
  /// one pass over the completion queue; returns the completions read
  size_t poll_locked();
  void complete_locked(op_t* op, int err);
  /// between two polls of a waiting write: drop the lock until the
  /// completion queue has something, or briefly, never past deadline
  void wait_cq(std::unique_lock<std::timed_mutex>& l,
	       std::chrono::steady_clock::time_point deadline,
	       std::chrono::steady_clock::time_point spin_until);
  void retire_locked(plan_t* p);

  using time_point = std::chrono::steady_clock::time_point;
  /// the peer's address, waiting for its insert until deadline; takes a
  /// reference, which put_peer() drops
  int get_peer(const std::string& name, time_point deadline, fi_addr_t* out);
  void put_peer(const std::string& name);
  /// an insert thread: removes evicted peers from the address vector
  /// and adds queued ones
  void insert_loop();
  /// queue a peer's insert; under peer_mtx
  void queue_insert_locked(const std::string& name, peer_t& p);
  /// make room for a new peer by evicting idle ones; under peer_mtx.
  /// False when the table is full of peers in use.
  bool make_room_locked();
  /// remove an evicted peer from the address vector; without peer_mtx
  void remove_peer(const std::string& name);
  /// add a queued peer to the address vector; without peer_mtx
  void insert_peer(const std::string& name);
  /// take mtx for a write, unless the deadline passes first or an
  /// insert on a FI_THREAD_DOMAIN domain keeps the endpoint quiet
  bool lock_for_write(std::unique_lock<std::timed_mutex>& l,
		      time_point deadline);
};

Endpoint::Impl::~Impl()
{
  {
    std::lock_guard pl(peer_mtx);
    stopping = true;
  }
  insert_cv.notify_all();
  peer_cv.notify_all();
  if (progress_thr.joinable()) {
    progress_thr.join();
  }
  // waits for inserts still inside the provider
  for (auto& t : insert_thrs) {
    t.join();
  }
  // the endpoint first: it cancels whatever is still in flight, after
  // which no write references the regions
  if (ep && fi_close(&ep->fid)) {
    // still open after a failed cut-off, and it may still read the
    // staging buffer or write the windows: leave all of it
    return;
  }
  for (auto& [id, w] : windows) {
    // a window loses its region when re-registering it after a cut-off
    // failed
    if (w.mr) fi_close(&w.mr->fid);
  }
  if (stage_mr.mr) fi_close(&stage_mr.mr->fid);
  std::free(stage);
  if (cq) fi_close(&cq->fid);
  if (av) fi_close(&av->fid);
  if (domain) fi_close(&domain->fid);
  if (fabric) fi_close(&fabric->fid);
  if (info) fi_freeinfo(info);
}

int Endpoint::Impl::reg(char* ptr, size_t len, uint64_t access, region_t* out)
{
  const uint64_t req_key = (mr_mode & FI_MR_PROV_KEY) ? 0 : next_key++;
  fid_mr* mr = nullptr;
  int r = fi_mr_reg(domain, ptr, len, access, 0, req_key, 0, &mr, nullptr);
  if (r) {
    set_err("fi_mr_reg: " + fi_err(r));
    return to_errno(r);
  }
  if (mr_mode & FI_MR_ENDPOINT) {
    r = fi_mr_bind(mr, &ep->fid, 0);
    if (!r) {
      r = fi_mr_enable(mr);
    }
    if (r) {
      set_err("binding a region to the endpoint: " + fi_err(r));
      fi_close(&mr->fid);
      return to_errno(r);
    }
  }
  const uint64_t key = fi_mr_key(mr);
  if ((access & FI_REMOTE_WRITE) && key == FI_KEY_NOTAVAIL) {
    set_err("the provider gave the region no key");
    fi_close(&mr->fid);
    return -EOPNOTSUPP;
  }
  *out = region_t{mr, ptr, len, key, fi_mr_desc(mr)};
  return 0;
}

int Endpoint::Impl::reg_window_locked(char* ptr, size_t len, region_t* out)
{
  const auto now = std::chrono::steady_clock::now();
  while (!retired_order.empty() && retired_order.front().first <= now) {
    const auto [until, key] = retired_order.front();
    retired_order.pop_front();
    if (auto it = retired.find(key); it != retired.end() && it->second == until) {
      retired.erase(it);
    }
  }
  for (int attempt = 0; attempt < REKEY_ATTEMPTS; attempt++) {
    region_t fresh;
    if (int r = reg(ptr, len, FI_REMOTE_WRITE, &fresh); r < 0) {
      return r;
    }
    if (!retired.count(fresh.key)) {
      *out = fresh;
      return 0;
    }
    // a late write may still carry this key: drop it, and the provider
    // gives another
    key_collisions++;
    fi_close(&fresh.mr->fid);
  }
  set_err("the provider kept giving memory keys still in quarantine");
  return -EAGAIN;
}

int Endpoint::Impl::close_window_region_locked(region_t& r)
{
  if (!r.mr) {
    return 0;
  }
  if (int e = fi_close(&r.mr->fid); e) {
    set_err("closing a window's region: " + fi_err(e));
    return to_errno(e);
  }
  const auto until = std::chrono::steady_clock::now() + cfg.key_quarantine;
  retired[r.key] = until;
  retired_order.emplace_back(until, r.key);
  r.mr = nullptr;
  return 0;
}

void Endpoint::Impl::retire_locked(plan_t* p)
{
  stage_busy[p->slot] = false;
  put_peer(p->peer);
  plans.erase(p->self);
}

void Endpoint::Impl::complete_locked(op_t* op, int err)
{
  plan_t* p = op->plan;
  if (!p || p->outstanding == 0 || op->done) {
    return;
  }
  op->done = true;
  p->outstanding--;
  if (err) {
    writes_failed++;
    if (!p->err) {
      p->err = err;
    }
  }
  if (p->outstanding == 0) {
    if (p->abandoned) {
      retire_locked(p);  // its writer left; nobody waits for it
      return;
    }
    done_cv.notify_all();
  }
}

size_t Endpoint::Impl::poll_locked()
{
  {
    const auto now = std::chrono::steady_clock::now();
    if (last_poll != std::chrono::steady_clock::time_point{}) {
      last_poll_gap = now - last_poll;
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	last_poll_gap).count();
      if (ms >= 1000) {
	last_long_gap_ns = now.time_since_epoch().count();
	max_poll_gap_ms = std::max<uint64_t>(max_poll_gap_ms, ms);
      }
    }
    last_poll = now;
  }
  size_t got = 0;
  if (!cq || zombie) {
    return got;
  }
  fi_cq_entry ent[16];
  for (int round = 0; round < 8; round++) {
    const ssize_t n = fi_cq_read(cq, ent, 16);
    if (n > 0) {
      got += n;
      for (ssize_t i = 0; i < n; i++) {
	if (ent[i].op_context) {
	  complete_locked(static_cast<op_t*>(ent[i].op_context), 0);
	}
      }
      if (n < 16) {
	return got;
      }
      continue;
    }
    if (n == -FI_EAVAIL) {
      fi_cq_err_entry e{};
      if (fi_cq_readerr(cq, &e, 0) < 0) {
	return got;
      }
      got++;
      const char* text = fi_cq_strerror(cq, e.prov_errno, e.err_data,
					nullptr, 0);
      set_err(fi_err(e.err) + (text ? std::string(" (") + text + ")" : ""));
      if (e.op_context) {
	complete_locked(static_cast<op_t*>(e.op_context),
			e.err ? to_errno(e.err) : -EIO);
      }
      continue;
    }
    return got;  // -FI_EAGAIN: nothing more, or an error with no entry
  }
  return got;
}

void Endpoint::Impl::wait_cq(std::unique_lock<std::timed_mutex>& l,
			     std::chrono::steady_clock::time_point deadline,
			     std::chrono::steady_clock::time_point spin_until)
{
  const auto now = std::chrono::steady_clock::now();
  const auto left = deadline - now;
  if (left <= left.zero()) {
    return;
  }
  if (cq_fd >= 0) {
    // the provider signals the wait object when a completion is ready;
    // fi_trywait() says whether one already is
    fid* f = &cq->fid;
    if (fi_trywait(fabric, &f, 1) != FI_SUCCESS) {
      // something to read already; let others at the endpoint first
      l.unlock();
      std::this_thread::yield();
      l.lock();
      return;
    }
    const int fd = cq_fd;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::min<std::chrono::steady_clock::duration>(left, WAIT_MAX)).count();
    const timespec ts{static_cast<time_t>(ns / 1000000000),
		      static_cast<long>(ns % 1000000000)};
    pollfd pfd{fd, POLLIN, 0};
    l.unlock();
    // a cut-off can close the fd meanwhile; the wait then just runs out
    ppoll(&pfd, 1, &ts, nullptr);
    l.lock();
    return;
  }
  // No wait object: the provider makes progress only while it is polled
  // (manual progress), or tells nobody when it has. Poll again soon, but
  // without burning a core on a queue that stays empty.
  l.unlock();
  if (now < spin_until) {
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(
      std::min<std::chrono::steady_clock::duration>(left, WAIT_SLEEP));
  }
  l.lock();
}

void Endpoint::Impl::queue_insert_locked(const std::string& name, peer_t& p)
{
  p.state = peer_state::pending;
  p.err = 0;
  pending++;
  insert_q.push_back(name);
  insert_cv.notify_one();
}

bool Endpoint::Impl::make_room_locked()
{
  if (peers.size() - removing < cfg.max_peers) {
    return true;
  }
  // the idle peers, least recently used first; a peer being added, being
  // removed, or held by a write stays
  std::vector<std::map<std::string, peer_t>::iterator> idle;
  for (auto it = peers.begin(); it != peers.end(); ++it) {
    const peer_t& p = it->second;
    if (p.refs == 0 &&
	(p.state == peer_state::ready || p.state == peer_state::failed)) {
      idle.push_back(it);
    }
  }
  std::sort(idle.begin(), idle.end(), [](auto& a, auto& b) {
    return a->second.used < b->second.used;
  });
  // down to 7/8 of the table, so that each new peer does not evict one
  const size_t keep = cfg.max_peers - cfg.max_peers / 8 - 1;
  for (auto it : idle) {
    if (peers.size() - removing <= keep) {
      break;
    }
    if (it->second.state == peer_state::failed) {
      peers.erase(it);  // not in the address vector
    } else {
      it->second.state = peer_state::removing;
      removing++;
      remove_q.push_back(it->first);
      insert_cv.notify_one();
    }
  }
  return peers.size() - removing < cfg.max_peers;
}

int Endpoint::Impl::get_peer(const std::string& name, time_point deadline,
			     fi_addr_t* out)
{
  std::unique_lock pl(peer_mtx);
  if (stopping) {
    return -ESHUTDOWN;
  }
  const auto now = std::chrono::steady_clock::now();
  auto it = peers.find(name);
  if (it == peers.end()) {
    // a first contact: refused when too many are under way, or when the
    // table is full of peers in use, so neither grows with the number of
    // names clients send
    if (pending >= cfg.max_pending_inserts || !make_room_locked()) {
      inserts_refused++;
      return -EBUSY;
    }
    it = peers.try_emplace(name).first;
    peer_t& p = it->second;
    p.addr_buf.assign(std::max(name.size(), MAX_NAME), '\0');
    std::memcpy(p.addr_buf.data(), name.data(), name.size());
    queue_insert_locked(name, p);
  } else if (it->second.state == peer_state::failed &&
	     now >= it->second.retry_at) {
    if (pending >= cfg.max_pending_inserts) {
      inserts_refused++;
      return -EBUSY;
    }
    queue_insert_locked(name, it->second);
  }
  peer_t& p = it->second;
  // the reference keeps the entry while this write waits on it; a peer
  // being removed is queued again once it is out
  p.refs++;
  p.used = now;
  const bool settled = peer_cv.wait_until(pl, deadline, [&] {
    return stopping || p.state == peer_state::ready ||
      p.state == peer_state::failed;
  });
  if (!settled || p.state != peer_state::ready) {
    p.refs--;
    if (!settled) {
      return -ETIMEDOUT;
    }
    return p.state == peer_state::failed ? p.err : -ESHUTDOWN;
  }
  *out = p.addr;
  return 0;
}

void Endpoint::Impl::put_peer(const std::string& name)
{
  std::lock_guard pl(peer_mtx);
  if (auto it = peers.find(name); it != peers.end() && it->second.refs) {
    it->second.refs--;
  }
}

void Endpoint::Impl::remove_peer(const std::string& name)
{
  fi_addr_t addr;
  {
    std::lock_guard pl(peer_mtx);
    addr = peers.at(name).addr;
  }
  {
    // a FI_THREAD_DOMAIN domain allows no other call meanwhile; a removal
    // is quick, and no write uses the peer
    std::unique_lock l(mtx, std::defer_lock);
    if (!thread_safe) {
      l.lock();
    }
    if (!broken) {
      fi_av_remove(av, &addr, 1, 0);
    }
  }
  std::lock_guard pl(peer_mtx);
  auto it = peers.find(name);
  removing--;
  if (it->second.refs) {
    // a write wants it again meanwhile
    it->second.addr = FI_ADDR_NOTAVAIL;
    queue_insert_locked(name, it->second);
  } else {
    peers.erase(it);
  }
}

void Endpoint::Impl::insert_peer(const std::string& name)
{
  char* addr_buf;
  {
    std::lock_guard pl(peer_mtx);
    // a pending entry is never dropped, so its buffer stays put
    addr_buf = peers.at(name).addr_buf.data();
  }
  std::unique_lock l(mtx, std::defer_lock);
  if (!thread_safe) {
    // FI_THREAD_DOMAIN: no other call on the domain may run during the
    // insert, and the insert can take seconds. Let the writes in flight
    // finish first, within their budgets, so that the insert delays
    // none of their cut-offs; writes that start meanwhile wait.
    l.lock();
    quiesce = true;
    while (!plans.empty() && !stopping) {
      // reap completions: the writers may all be waiting for the endpoint
      // now, and a plan whose writer left after cancelling it waits for
      // its cancellations to complete
      poll_locked();
      if (plans.empty()) {
	break;
      }
      l.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      l.lock();
    }
  }
  fi_addr_t addr = FI_ADDR_NOTAVAIL;
  int r = broken ? -FI_EIO : -FI_ESHUTDOWN;
  if (!stopping && !broken) {
    if (cfg.insert_hook) {
      cfg.insert_hook(name);
    }
    r = fi_av_insert(av, addr_buf, 1, &addr, 0, nullptr);
  }
  if (r != 1) {
    set_err("fi_av_insert: " + (r < 0 ? fi_err(r) : std::string("rejected")));
  }
  {
    // under mtx too on a FI_THREAD_DOMAIN domain: writes start again
    // only once the peer is settled
    std::lock_guard pl(peer_mtx);
    peer_t& p = peers.at(name);
    pending--;
    if (r == 1) {
      p.addr = addr;
      p.state = peer_state::ready;
      peers_inserted++;
    } else {
      p.state = peer_state::failed;
      p.err = r < 0 ? to_errno(r) : -EHOSTUNREACH;
      p.retry_at = std::chrono::steady_clock::now() + INSERT_RETRY;
    }
  }
  if (l.owns_lock()) {
    quiesce = false;
    l.unlock();
  }
  peer_cv.notify_all();
}

void Endpoint::Impl::insert_loop()
{
  while (true) {
    std::string name;
    bool remove;
    {
      std::unique_lock pl(peer_mtx);
      insert_cv.wait(pl, [this] {
	return stopping || !remove_q.empty() || !insert_q.empty();
      });
      if (stopping) {
	return;
      }
      // removals first: they are quick, and they make room
      remove = !remove_q.empty();
      auto& q = remove ? remove_q : insert_q;
      name = std::move(q.front());
      q.pop_front();
    }
    if (remove) {
      remove_peer(name);
    } else {
      insert_peer(name);
    }
  }
}

bool Endpoint::Impl::lock_for_write(std::unique_lock<std::timed_mutex>& l,
				    time_point deadline)
{
  while (true) {
    if (!l.try_lock_until(deadline)) {
      return false;
    }
    if (!quiesce) {
      return true;
    }
    l.unlock();
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(
      std::min<std::chrono::steady_clock::duration>(
	std::chrono::milliseconds(1), deadline - now));
  }
}

int Endpoint::Impl::open_ep(std::string* err)
{
  fi_cq_attr cq_attr{};
  cq_attr.format = FI_CQ_FORMAT_CONTEXT;
  cq_attr.size = 4096;
  // a completion queue with a file descriptor to wait on, if the
  // provider has one, so that a waiting write need not poll
  cq_fd = -1;
  cq_attr.wait_obj = FI_WAIT_FD;
  int r = fi_cq_open(domain, &cq_attr, &cq, nullptr);
  if (!r && (fi_control(&cq->fid, FI_GETWAIT, &cq_fd) || cq_fd < 0)) {
    cq_fd = -1;
    fi_close(&cq->fid);
    r = -FI_ENOSYS;
  }
  if (r) {
    cq_attr.wait_obj = FI_WAIT_NONE;
    if ((r = fi_cq_open(domain, &cq_attr, &cq, nullptr))) {
      cq = nullptr;
      *err = "fi_cq_open: " + fi_err(r);
      return to_errno(r);
    }
  }
  if ((r = fi_endpoint(domain, info, &ep, nullptr))) {
    ep = nullptr;
    *err = "fi_endpoint: " + fi_err(r);
    return to_errno(r);
  }
  if ((r = fi_ep_bind(ep, &av->fid, 0)) ||
      (r = fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV)) ||
      (r = fi_enable(ep))) {
    *err = "enabling the endpoint: " + fi_err(r);
    return to_errno(r);
  }
  size_t len = 0;
  r = fi_getname(&ep->fid, nullptr, &len);
  if (r != -FI_ETOOSMALL || len == 0 || len > MAX_NAME) {
    *err = "fi_getname: " + (r ? fi_err(r) : std::string("bad length"));
    return -EINVAL;
  }
  my_name.resize(len);
  if ((r = fi_getname(&ep->fid, my_name.data(), &len))) {
    *err = "fi_getname: " + fi_err(r);
    return to_errno(r);
  }
  my_name.resize(len);
  // what the provider promises about discarding writes
  auto getopt_bool = [this](int opt) {
    bool b = false;
    size_t blen = sizeof(b);
    if (fi_getopt(&ep->fid, FI_OPT_ENDPOINT, opt, &b, &blen) ||
	blen != sizeof(b)) {
      return -1;
    }
    return b ? 1 : 0;
  };
  close_discards = getopt_bool(OPT_CLOSE_DISCARDS);
  cancel_discards = cfg.per_plan_cutoff &&
    (cfg.cancel_hook || getopt_bool(OPT_CANCEL_DISCARDS) == 1);
  return 0;
}

int Endpoint::Impl::cancel_plan_locked(plan_t* p, bool* simulated)
{
  *simulated = false;
  for (auto& op : p->ops) {
    if (op.done) {
      continue;
    }
    int r;
    if (cfg.cancel_hook) {
      r = cfg.cancel_hook();
      *simulated = true;
    } else {
      r = fi_cancel(&ep->fid, &op.ctx);
      if (r == -FI_ENOENT) {
	r = 0;  // no longer outstanding: its completion is on its way
      }
    }
    if (r) {
      return r;
    }
  }
  return 0;
}

int Endpoint::Impl::close_ep_locked()
{
  if (ep) {
    int r = cfg.cutoff_close_hook ? cfg.cutoff_close_hook() : 0;
    if (r == 0) {
      r = fi_close(&ep->fid);
    }
    if (r) {
      zombie = true;
      return r;
    }
    ep = nullptr;
  }
  if (cq) {
    if (int r = fi_close(&cq->fid); r) {
      // the endpoint is closed, so its writes are cut off; the queue
      // only stays allocated
      set_err("closing the completion queue in a cut-off: " + fi_err(r));
    }
    cq = nullptr;
    cq_fd = -1;
  }
  // nothing references the operations of plans whose writers left any
  // more, nor the staging they read
  for (auto it = plans.begin(); it != plans.end(); ) {
    plan_t* p = (it++)->get();
    if (p->abandoned) {
      p->outstanding = 0;
      retire_locked(p);
    }
  }
  for (auto& p : hook_cancelled) {
    if (!p->released) {
      p->released = true;
      stage_busy[p->slot] = false;
      put_peer(p->peer);
    }
  }
  return 0;
}

void Endpoint::Impl::stop_unsafe_locked(std::string why)
{
  unsafe = true;
  broken = true;
  set_err(std::move(why));
  if (close_ep_locked()) {
    // the endpoint stays open, and its writes may land at any time
    for (auto& p : plans) {
      if (p->outstanding) {
	p->outstanding = 0;
	p->err = -ENOTRECOVERABLE;
      }
    }
  } else {
    // closed: the writes in flight are cut off now, late for a plan whose
    // budget already ran out
    const auto cut = std::chrono::steady_clock::now();
    for (auto& p : plans) {
      if (p->outstanding) {
	p->outstanding = 0;
	p->err = cut > p->budget_end ? -ENOTRECOVERABLE :
	  p->err ? p->err : -ECANCELED;
      }
    }
  }
  done_cv.notify_all();
}

void Endpoint::Impl::make_unsafe_locked(std::string why)
{
  unsafe = true;
  broken = true;
  set_err(std::move(why));
  for (auto& p : plans) {
    if (p->outstanding) {
      p->outstanding = 0;
      p->err = -ENOTRECOVERABLE;
    }
  }
  done_cv.notify_all();
}

void Endpoint::Impl::reset_locked()
{
  const auto t0 = std::chrono::steady_clock::now();
  resets++;
  // Cut off: closing the endpoint cancels every operation it still has
  // outstanding, so none of them retries into a peer's window later.
  // The completion queue goes too, with any completions of those
  // operations, whose contexts are about to be released.
  if (int r = close_ep_locked(); r) {
    // the endpoint is still open, and so are its writes: they were not
    // cut off and can land at any time
    cutoffs_failed++;
    make_unsafe_locked("a cut-off failed: closing the endpoint: " +
		       fi_err(r) + "; its writes may still land");
    return;
  }
  // Every plan with writes in flight has lost them, and its writer has
  // to hear it. One with none in flight has lost nothing: it is done, or
  // its writer is still gathering or posting and goes on with the new
  // endpoint. A plan whose budget ran out before the writes were cut off
  // may have had some land after it.
  const auto cut = std::chrono::steady_clock::now();
  auto earliest = std::chrono::steady_clock::time_point::max();
  for (auto& p : plans) {
    if (p->outstanding) {
      earliest = std::min(earliest, p->budget_end);
    }
  }
  const verdict v = judge_cutoff_locked(t0, cut, earliest);
  const bool unsafe_now = v == verdict::unsafe;
  for (auto& p : plans) {
    if (p->outstanding == 0) {
      continue;
    }
    p->outstanding = 0;
    if (unsafe_now && cut > p->budget_end) {
      p->err = -ENOTRECOVERABLE;
    } else if (p->due || cut > p->budget_end) {
      p->err = -ETIMEDOUT;
    } else if (!p->err) {
      p->err = -ECANCELED;
    }
    p->due = false;
  }
  done_cv.notify_all();
  if (unsafe_now) {
    // closed, and not opened again: take no more writes
    unsafe = true;
    broken = true;
    return;
  }
  // regions bound to the old endpoint are gone with it
  const bool rebind = mr_mode & FI_MR_ENDPOINT;
  if (rebind) {
    for (auto& [id, w] : windows) {
      close_window_region_locked(w);
    }
    if (stage_mr.mr) {
      fi_close(&stage_mr.mr->fid);
      stage_mr.mr = nullptr;
    }
  }
  std::string err;
  if (open_ep(&err) < 0) {
    broken = true;
    set_err("reopening after a cut-off: " + err);
    return;
  }
  if (rebind) {
    for (auto& [id, w] : windows) {
      region_t fresh;
      if (reg_window_locked(w.ptr, w.len, &fresh) < 0) {
        broken = true;
        return;
      }
      w = fresh;
    }
    if (stage) {
      region_t fresh;
      if (reg(stage, cfg.stage_size * cfg.stage_count, FI_WRITE, &fresh) < 0) {
        broken = true;
        return;
      }
      stage_mr = fresh;
    }
  }
  note_cutoff_cost(std::chrono::steady_clock::now() - t0);
}

bool Endpoint::Impl::time_to_post_locked(
  uint64_t bytes, std::chrono::steady_clock::time_point deadline) const
{
  std::chrono::steady_clock::duration need = POST_MARGIN;
  if (plan_rate > 0) {
    need += std::chrono::nanoseconds(static_cast<int64_t>(2 * bytes / plan_rate));
  }
  return std::chrono::steady_clock::now() + need < deadline;
}

Endpoint::Impl::verdict Endpoint::Impl::judge_cutoff_locked(
  std::chrono::steady_clock::time_point t0,
  std::chrono::steady_clock::time_point cut,
  std::chrono::steady_clock::time_point budget_end)
{
  using std::chrono::duration_cast;
  using std::chrono::milliseconds;
  const auto took = cut - t0;
  const auto late = budget_end == std::chrono::steady_clock::time_point::max() ?
    std::chrono::steady_clock::duration::zero() : cut - budget_end;
  const int64_t late_ms = duration_cast<milliseconds>(late).count();
  if (late.count() > 0) {
    cutoffs_late++;
    max_lateness_ms = std::max<uint64_t>(max_lateness_ms, late_ms);
    last_late_ns = cut.time_since_epoch().count();
  }
  std::string what = "a cut-off took " +
    std::to_string(duration_cast<milliseconds>(took).count()) +
    " ms and ended " + std::to_string(late_ms) +
    " ms after the budget it protected";
  if (late.count() > 0) {
    const int64_t started_ms =
      duration_cast<milliseconds>(notice_delay).count();
    const int64_t gap_ms = duration_cast<milliseconds>(last_poll_gap).count();
    what += "; it started " + std::to_string(started_ms) +
      " ms after its writes' deadline";
    if (gap_ms > 0 && gap_ms + 50 >= started_ms) {
      what += ", and nothing polled the endpoint for the " +
	std::to_string(gap_ms) + " ms before: the process, or its "
	"threads, did not run (stopped, paused, swapped out or starved)";
    }
  }
  const std::string tolerance = std::to_string(cfg.late_tolerance.count());
  if (took > cfg.late_tolerance) {
    // the provider itself takes that long to cut writes off, as one that
    // drains them instead of discarding does: every cut-off would be late
    set_err(what + ". The close or cancel itself took longer than the "
	    "tolerance of " + tolerance + " ms; writes may have landed late");
    return verdict::unsafe;
  }
  if (late > cfg.late_tolerance) {
    // started late; the endpoint is clean now, since the close or cancel
    // returned
    cutoffs_past_tolerance++;
    last_past_ns = cut.time_since_epoch().count();
    set_err(what + ", beyond the tolerance of " + tolerance +
	    " ms; writes may have landed late" +
	    (cfg.late_fail_closed ? "" : ", and the endpoint goes on"));
    return cfg.late_fail_closed ? verdict::unsafe : verdict::past_tolerance;
  }
  if (late.count() > 0) {
    set_err(what + ", within the tolerance of " + tolerance + " ms");
    return verdict::late;
  }
  return verdict::in_time;
}

void Endpoint::Impl::note_notice_delay(std::chrono::steady_clock::duration d)
{
  // twice the delay plus a little, at once when that is more, a quarter
  // of the way when it is less: as for the cut-off's cost
  const int64_t ms = std::max<int64_t>(
    0, std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
  const int64_t sample = std::min<int64_t>(
    std::max<int64_t>(2 * ms + 5, SLACK_MIN.count()),
    cfg.cutoff_cost_max.count());
  const int64_t cur = cutoff_slack_ms;
  cutoff_slack_ms = sample >= cur ? sample : cur - (cur - sample) / 4;
}

void Endpoint::Impl::cut_off_locked(plan_t* p,
				    std::chrono::steady_clock::time_point noticed)
{
  timeouts++;
  notice_delay = noticed - p->deadline;
  note_notice_delay(notice_delay);
  const auto t0 = std::chrono::steady_clock::now();
  if (cancel_discards) {
    // only this write's operations, if the provider discards them
    bool simulated = false;
    const int r = cancel_plan_locked(p, &simulated);
    const auto cut = std::chrono::steady_clock::now();
    if (r == 0) {
      plans_cut_off++;
      p->cut = true;
      if (simulated) {
	// the provider still holds the operations: keep them, and the
	// staging they read, until the endpoint closes
	p->hooked = true;
	p->outstanding = 0;
	hook_cancelled.push_back(std::move(*p->self));
	plans.erase(p->self);
      }
      if (judge_cutoff_locked(t0, cut, p->budget_end) == verdict::unsafe) {
	p->cut_result = cut > p->budget_end ? -ENOTRECOVERABLE : -ETIMEDOUT;
	stop_unsafe_locked(get_err());
	return;
      }
      p->cut_result = -ETIMEDOUT;
      note_cutoff_cost(cut - t0);
      done_cv.notify_all();
      return;
    }
    // the provider kept the operations: close the endpoint instead,
    // which discards them all
    cancels_failed++;
    set_err("cancelling a late write failed: " + fi_err(r) +
	    "; cutting it off by closing the endpoint");
  } else {
    set_err("writes still in flight at the deadline; cut off");
  }
  p->due = true;
  reset_locked();
}

void Endpoint::Impl::cut_due_locked()
{
  const auto now = std::chrono::steady_clock::now();
  std::vector<plan_t*> due;
  for (auto& p : plans) {
    if (p->outstanding && !p->cut && !p->abandoned && now > p->deadline) {
      due.push_back(p.get());
    }
  }
  for (plan_t* p : due) {
    // a reset for an earlier one may have taken this one too
    if (broken || p->cut || p->outstanding == 0) {
      continue;
    }
    cutoffs_on_behalf++;
    cut_off_locked(p, now);
  }
}

void Endpoint::Impl::note_cutoff_cost(std::chrono::steady_clock::duration took)
{
  const int64_t ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(took).count();
  const int64_t sample = std::min<int64_t>(2 * ms + 20,
					   cfg.cutoff_cost_max.count());
  const int64_t cur = cutoff_cost_ms;
  cutoff_cost_ms = sample >= cur ? sample : cur - (cur - sample) / 4;
}

Endpoint::Endpoint(std::unique_ptr<Impl> i) : impl(std::move(i)) {}

Endpoint::~Endpoint() = default;

std::unique_ptr<Endpoint> Endpoint::open(const config_t& cfg, std::string* err)
{
  auto d = std::make_unique<Impl>();
  d->cfg = cfg;
  d->cutoff_cost_ms = std::min(cfg.cutoff_cost_initial,
			       cfg.cutoff_cost_max).count();
  if (cfg.provider.empty()) {
    *err = "no libfabric provider named";
    return nullptr;
  }
  fi_info* hints = fi_allocinfo();
  if (!hints) {
    *err = "fi_allocinfo failed";
    return nullptr;
  }
  hints->ep_attr->type = FI_EP_RDM;
  hints->caps = FI_RMA | FI_WRITE | FI_REMOTE_WRITE;
  hints->mode = FI_CONTEXT | FI_CONTEXT2;
  hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_VIRT_ADDR |
    FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT;
  hints->fabric_attr->prov_name = strdup(cfg.provider.c_str());
  if (!cfg.domain.empty()) {
    hints->domain_attr->name = strdup(cfg.domain.c_str());
  }
  const char* node = cfg.node.empty() ? nullptr : cfg.node.c_str();
  const char* service = cfg.service.empty() ? nullptr : cfg.service.c_str();
  const uint64_t flags = (node || service) ? FI_SOURCE : 0;
  // Ask for completions that mean "placed in the target's memory"; a
  // provider that cannot promise it is still usable, see
  // delivery_complete(). Ask for a thread-safe domain too, so that a slow
  // address insert holds up no other write; that matters less, so a
  // provider without one is preferred to giving up delivery-complete.
  int r = -FI_ENODATA;
  for (uint64_t op_flags : {uint64_t{FI_DELIVERY_COMPLETE}, uint64_t{0}}) {
    for (fi_threading threading : {FI_THREAD_SAFE, FI_THREAD_DOMAIN}) {
      if (threading == FI_THREAD_SAFE && !cfg.thread_safe) {
	continue;
      }
      hints->tx_attr->op_flags = op_flags;
      hints->domain_attr->threading = threading;
      r = fi_getinfo(API_VERSION, node, service, flags, hints, &d->info);
      if (r != -FI_ENODATA) {
	break;
      }
    }
    if (r != -FI_ENODATA) {
      break;
    }
  }
  if (!r && cfg.mr_cnt && d->info->domain_attr->mr_cnt &&
      d->info->domain_attr->mr_cnt < cfg.mr_cnt) {
    // a provider with a fixed table of regions: ask for a larger one,
    // with the same attributes, and keep what it gave if it will not
    hints->domain_attr->mr_cnt = cfg.mr_cnt;
    fi_info* bigger = nullptr;
    if (fi_getinfo(API_VERSION, node, service, flags, hints, &bigger) == 0) {
      fi_freeinfo(d->info);
      d->info = bigger;
    }
  }
  fi_freeinfo(hints);
  if (r) {
    *err = "fi_getinfo(" + cfg.provider + "): " + fi_err(r);
    d->info = nullptr;
    return nullptr;
  }
  fi_info* info = d->info;
  d->prov = info->fabric_attr->prov_name ? info->fabric_attr->prov_name : "";
  d->mr_mode = info->domain_attr->mr_mode;
  d->dc = info->tx_attr->op_flags & FI_DELIVERY_COMPLETE;
  d->thread_safe = info->domain_attr->threading == FI_THREAD_SAFE;
  if (info->ep_attr->max_msg_size) {
    d->max_write = std::min<uint64_t>(MAX_WRITE, info->ep_attr->max_msg_size);
  }
  if ((d->mr_mode & FI_MR_RAW) || info->domain_attr->mr_key_size > 8) {
    *err = d->prov + " needs raw memory keys, which tokens cannot carry";
    return nullptr;
  }

  if ((r = fi_fabric(info->fabric_attr, &d->fabric, nullptr))) {
    *err = "fi_fabric: " + fi_err(r);
    return nullptr;
  }
  if ((r = fi_domain(d->fabric, info, &d->domain, nullptr))) {
    *err = "fi_domain: " + fi_err(r);
    return nullptr;
  }
  fi_av_attr av_attr{};
  av_attr.type = info->domain_attr->av_type != FI_AV_UNSPEC ?
    info->domain_attr->av_type : FI_AV_TABLE;
  av_attr.count = 0;  // the provider's default; shm caps it below max_peers
  if ((r = fi_av_open(d->domain, &av_attr, &d->av, nullptr))) {
    *err = "fi_av_open: " + fi_err(r);
    return nullptr;
  }
  if (d->open_ep(err) < 0) {
    return nullptr;
  }

  if (cfg.stage_size && cfg.stage_count) {
    const size_t total = cfg.stage_size * cfg.stage_count;
    void* p = nullptr;
    if (posix_memalign(&p, 4096, total) != 0 || !p) {
      *err = "cannot allocate " + std::to_string(total) + " staging bytes";
      return nullptr;
    }
    d->stage = static_cast<char*>(p);
    if ((r = d->reg(d->stage, total, FI_WRITE, &d->stage_mr))) {
      *err = "registering staging: " + d->get_err();
      return nullptr;
    }
    d->stage_busy.assign(cfg.stage_count, false);
    // a writer adds peers to the address vector on a thread of its own
    Impl* raw = d.get();
    const unsigned n = d->thread_safe ? std::max(1u, cfg.insert_threads) : 1;
    for (unsigned i = 0; i < n; i++) {
      d->insert_thrs.emplace_back([raw] { raw->insert_loop(); });
    }
  }

  if (cfg.progress_thread) {
    Impl* raw = d.get();
    d->progress_thr = std::thread([raw] {
      while (!raw->stopping) {
	{
	  std::lock_guard l(raw->mtx);
	  raw->poll_locked();
	  raw->cut_due_locked();
	}
	std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    });
  }
  return std::unique_ptr<Endpoint>(new Endpoint(std::move(d)));
}

const std::string& Endpoint::provider() const
{
  return impl->prov;
}

const std::string& Endpoint::name() const
{
  return impl->my_name;
}

bool Endpoint::delivery_complete() const
{
  return impl->dc;
}

std::string Endpoint::describe() const
{
  const auto* info = impl->info;
  std::string s = "provider " + impl->prov;
  if (info->fabric_attr->name) {
    s += std::string(", fabric ") + info->fabric_attr->name;
  }
  if (info->domain_attr->name) {
    s += std::string(", domain ") + info->domain_attr->name;
  }
  s += (impl->mr_mode & FI_MR_VIRT_ADDR) ? ", virtual-address regions" :
    ", offset regions";
  s += impl->dc ? ", delivery-complete writes" : ", transmit-complete writes";
  if (impl->stage) {
    s += impl->thread_safe ? ", concurrent peer inserts" :
      ", peer inserts pause writes";
    s += impl->cancel_discards ? ", late writes cancelled one by one" :
      ", late writes cut off by reopening the endpoint";
    if (impl->close_discards == 0) {
      s += ", closing does not discard writes";
    }
  }
  return s;
}

int Endpoint::register_window(char* ptr, size_t len, window_t* out)
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  Impl::region_t reg;
  if (int r = d.reg_window_locked(ptr, len, &reg); r < 0) {
    return r;
  }
  const uint64_t id = d.next_window++;
  d.windows[id] = reg;
  d.nwindows = d.windows.size();
  *out = window_t{id, ptr, len};
  return 0;
}

void Endpoint::deregister_window(uint64_t id)
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  auto it = d.windows.find(id);
  if (it == d.windows.end()) {
    return;
  }
  d.close_window_region_locked(it->second);
  d.windows.erase(it);
  d.nwindows = d.windows.size();
}

int Endpoint::rekey_window(const window_t& w)
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  if (d.broken) {
    return -EIO;
  }
  auto it = d.windows.find(w.id);
  if (it == d.windows.end() || !it->second.mr) {
    return -ENOENT;
  }
  Impl::region_t& cur = it->second;
  if (d.rekey_in_place != 0) {
    // in place: the region keeps its memory, registration and binding
    uint64_t key = 0;
    const int r = fi_control(&cur.mr->fid, MR_REKEY, &key);
    if (r == 0 && key != cur.key && key != FI_KEY_NOTAVAIL &&
	!d.retired.count(key)) {
      // the provider never gives the old key out again, so it needs no
      // quarantine
      d.rekey_in_place = 1;
      cur.key = key;
      d.windows_rekeyed++;
      d.rekeys_in_place++;
      return 0;
    }
    if (r == -FI_ENOSYS || r == -FI_EOPNOTSUPP) {
      d.rekey_in_place = 0;
    } else if (r == 0) {
      // a key that is still in quarantine, or none at all: the old key is
      // dead already; register the memory again for a sound one
      d.key_collisions++;
      cur.key = key;
    } else {
      d.set_err("re-keying a window in place: " + fi_err(r) +
		"; registering it again");
    }
  }
  // the new region first, while the old one still holds its key, so the
  // provider cannot hand the old key straight back
  Impl::region_t fresh;
  if (int r = d.reg_window_locked(cur.ptr, cur.len, &fresh); r < 0) {
    return r;
  }
  if (int r = d.close_window_region_locked(cur); r < 0) {
    // the old key still works; keep the window as it was
    fi_close(&fresh.mr->fid);
    return r;
  }
  cur = fresh;
  d.windows_rekeyed++;
  d.rekeys_reregistered++;
  return 0;
}

std::string Endpoint::window_token(const window_t& w, uint64_t ofs,
				   uint64_t len) const
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  auto it = d.windows.find(w.id);
  if (d.broken || it == d.windows.end() || !it->second.mr ||
      ofs + len > it->second.len) {
    return {};
  }
  token_t t;
  t.base = (d.mr_mode & FI_MR_VIRT_ADDR) ?
    reinterpret_cast<uint64_t>(it->second.ptr) + ofs : ofs;
  t.size = len;
  t.provider = d.prov;
  t.name = d.my_name;
  t.key = it->second.key;
  return format_token(t);
}

int Endpoint::write(const token_t& dst, const struct iovec* iov,
		    size_t iovcnt, const std::vector<write_t>& writes,
		    std::chrono::milliseconds budget)
{
  auto& d = *impl;
  if (!d.stage) {
    return -EOPNOTSUPP;
  }
  if (dst.provider != d.prov) {
    return -EPROTONOSUPPORT;
  }
  uint64_t total = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    total += iov[i].iov_len;
  }
  size_t chunks = 0;
  for (const auto& w : writes) {
    if (w.len > total || w.src_ofs > total - w.len ||
	w.len > dst.size || w.dst_ofs > dst.size - w.len) {
      return -ERANGE;
    }
    chunks += (w.len + d.max_write - 1) / d.max_write;
  }
  if (total > d.cfg.stage_size) {
    return -E2BIG;
  }
  // stop waiting early enough to cut the writes off within the budget:
  // by what a cut-off costs, and by how late cut-offs tend to start
  const std::chrono::milliseconds margin{d.cutoff_cost_ms.load() +
					 d.cutoff_slack_ms.load()};
  if (budget <= margin) {
    d.budget_refused++;
    d.set_err("a budget of " + std::to_string(budget.count()) +
	      " ms is no longer than a cut-off is expected to take, with "
	      "scheduling slack (" + std::to_string(margin.count()) +
	      " ms); nothing sent");
    return -ETIMEDOUT;
  }
  const auto budget_end = std::chrono::steady_clock::now() + budget;
  const auto deadline = budget_end - margin;
  // the peer's address first, without the endpoint lock: the insert of a
  // new peer can take seconds
  fi_addr_t addr;
  if (int r = d.get_peer(dst.name, deadline, &addr); r < 0) {
    if (r == -ETIMEDOUT) {
      d.peer_timeouts++;
    }
    return r;
  }
  std::unique_lock l(d.mtx, std::defer_lock);
  if (!d.lock_for_write(l, deadline)) {
    d.put_peer(dst.name);
    d.peer_timeouts++;
    return -ETIMEDOUT;
  }
  if (d.broken || !d.ep) {
    d.put_peer(dst.name);
    return -EIO;
  }
  // waiting for the peer or the endpoint may have eaten the budget
  if (!d.time_to_post_locked(total, deadline)) {
    d.late_starts++;
    d.put_peer(dst.name);
    d.set_err("too little budget left to start the write; nothing sent");
    return -ETIMEDOUT;
  }
  size_t slot = 0;
  while (slot < d.stage_busy.size() && d.stage_busy[slot]) {
    slot++;
  }
  if (slot == d.stage_busy.size()) {
    d.staging_busy++;
    d.put_peer(dst.name);
    return -EBUSY;
  }
  // the plan holds the slot and the peer reference from here on; being
  // listed, it also keeps a FI_THREAD_DOMAIN insert from starting
  d.stage_busy[slot] = true;
  d.plans.push_front(std::make_unique<Impl::plan_t>());
  Impl::plan_t* plan = d.plans.front().get();
  plan->self = d.plans.begin();
  plan->slot = slot;
  plan->peer = dst.name;
  plan->budget_end = budget_end;
  plan->deadline = deadline;
  plan->ops.reserve(chunks);

  // gather the source into the slot without the lock: it can be
  // megabytes, and the slot is ours alone
  char* buf = d.stage + slot * d.cfg.stage_size;
  l.unlock();
  {
    char* p = buf;
    for (size_t i = 0; i < iovcnt; i++) {
      std::memcpy(p, iov[i].iov_base, iov[i].iov_len);
      p += iov[i].iov_len;
    }
  }
  if (d.cfg.pre_post_hook) {
    d.cfg.pre_post_hook();
  }
  l.lock();
  if (d.broken || !d.ep) {
    // a cut-off failed to reopen the endpoint while we copied
    d.retire_locked(plan);
    return -EIO;
  }
  // the last look before posting: a write posted with no time left would
  // be cut off at once, and the cut-off would cancel every other write in
  // flight on the endpoint
  if (!d.time_to_post_locked(total, deadline)) {
    d.late_starts++;
    d.set_err("too little budget left to start the write; nothing sent");
    d.retire_locked(plan);
    return -ETIMEDOUT;
  }
  plan->posted_at = std::chrono::steady_clock::now();

  for (const auto& w : writes) {
    for (uint64_t o = 0; o < w.len && !plan->err && !plan->cut; ) {
      const uint64_t n = std::min(w.len - o, d.max_write);
      auto& op = plan->ops.emplace_back();
      op.plan = plan;
      ssize_t r;
      while ((r = fi_write(d.ep, buf + w.src_ofs + o, n, d.stage_mr.desc, addr,
			   dst.base + w.dst_ofs + o, dst.key, &op.ctx)) ==
	     -FI_EAGAIN) {
	// the transmit queue is full: progress, which completes what
	// is out, then retry, letting others at the endpoint meanwhile
	d.poll_locked();
	if (std::chrono::steady_clock::now() > deadline) {
	  break;
	}
	l.unlock();
	std::this_thread::yield();
	l.lock();
	if (plan->err || plan->cut || d.broken) {
	  break;  // a cut-off took what this plan had posted
	}
      }
      if (r || plan->err || plan->cut || d.broken) {
	plan->ops.pop_back();
	if (!plan->err && !plan->cut) {
	  d.set_err(d.broken ? std::string("the endpoint is broken") :
		    "fi_write: " + fi_err(r));
	  d.writes_failed++;
	  plan->err = r == -FI_EAGAIN ? -ETIMEDOUT :
	    d.broken ? -EIO : to_errno(r);
	}
	break;
      }
      plan->outstanding++;
      d.writes_posted++;
      o += n;
    }
    if (plan->err || plan->cut) {
      break;
    }
  }

  // Wait for the writes. One waiting write polls the completion queue
  // for all of them, which drives the provider if it progresses only
  // while polled; the others sleep until a plan finishes, the poller
  // leaves, or a cut-off fails them, and each wakes at its deadline.
  bool polling = false;
  auto spin_until = std::chrono::steady_clock::time_point{};
  auto leave = [&] {
    if (polling) {
      polling = false;
      d.poller = false;
      d.done_cv.notify_all();
    }
  };
  auto hook = [&] {
    if (d.cfg.wait_hook) {
      l.unlock();
      d.cfg.wait_hook(plan->deadline);
      l.lock();
    }
  };
  while (true) {
    if (plan->cut) {
      // cut off by cancelling its operations, by this write or on its
      // behalf
      leave();
      const int res = plan->cut_result;
      if (plan->hooked) {
	return res;  // a test's cancel hook stood in; the plan stays
      }
      if (plan->outstanding) {
	d.poll_locked();  // the cancellations complete, usually at once
      }
      if (plan->outstanding) {
	plan->abandoned = true;  // whoever polls next retires it
      } else {
	d.retire_locked(plan);
      }
      return res;
    }
    if (plan->outstanding == 0) {
      leave();
      const int res = plan->err;
      if (!res) {
	d.bytes_written += total;
	const auto took = std::chrono::steady_clock::now() - plan->posted_at;
	if (total >= RATE_MIN_BYTES && took.count() > 0) {
	  const double rate = double(total) /
	    std::chrono::duration_cast<std::chrono::nanoseconds>(took).count();
	  d.plan_rate = d.plan_rate > 0 ? d.plan_rate + (rate - d.plan_rate) / 8 :
	    rate;
	}
      }
      // a reset that cut its writes off left -ETIMEDOUT here, or
      // -ENOTRECOVERABLE when it went wrong
      d.retire_locked(plan);
      return res;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now > plan->deadline) {
      // a last look: what completed during the wait counts
      d.poll_locked();
      if (plan->outstanding == 0) {
	continue;
      }
      // out of budget with writes in flight: cut them off, so none of
      // them lands in the peer's window after the caller gave up
      leave();
      d.cut_off_locked(plan, now);
      continue;
    }
    if (!polling && !d.poller) {
      polling = d.poller = true;
      spin_until = now + SPIN_TIME;
    }
    if (!polling) {
      // wake at the deadline, and now and then to take over from a
      // poller the scheduler left off the CPU
      d.done_cv.wait_until(l, std::min(plan->deadline, now + POLLER_STALE));
      hook();
      if (std::chrono::steady_clock::now() - d.last_poll > POLLER_STALE) {
	d.poll_locked();
	d.cut_due_locked();
      }
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const size_t got = d.poll_locked();
    const auto t1 = std::chrono::steady_clock::now();
    d.poll_floor = std::min(d.poll_floor, t1 - t0);
    if (got || t1 - t0 > 2 * d.poll_floor + POLL_BUSY_MARGIN) {
      spin_until = t1 + SPIN_TIME;
    }
    // writes whose writers are not running are cut off in time anyway
    d.cut_due_locked();
    if (plan->outstanding && !plan->cut) {
      d.wait_cq(l, plan->deadline, spin_until);
      hook();
    }
  }
}

void Endpoint::progress()
{
  std::lock_guard l(impl->mtx);
  impl->poll_locked();
}

void Endpoint::sync()
{
  // a provider that places data in software does so inside fi_cq_read,
  // which runs under this lock
  std::lock_guard l(impl->mtx);
}

Endpoint::stats_t Endpoint::stats() const
{
  auto& d = *impl;
  stats_t s;
  s.writes_posted = d.writes_posted;
  s.writes_failed = d.writes_failed;
  s.bytes_written = d.bytes_written;
  s.peers_inserted = d.peers_inserted;
  s.staging_busy = d.staging_busy;
  s.timeouts = d.timeouts;
  s.resets = d.resets;
  s.peer_timeouts = d.peer_timeouts;
  s.inserts_refused = d.inserts_refused;
  {
    std::lock_guard pl(d.peer_mtx);
    s.peers = d.peers.size();
    s.pending_inserts = d.pending;
  }
  s.cutoffs_failed = d.cutoffs_failed;
  s.cutoffs_late = d.cutoffs_late;
  s.budget_refused = d.budget_refused;
  s.cutoff_cost_ms = d.cutoff_cost_ms;
  s.max_cutoff_lateness_ms = d.max_lateness_ms;
  s.last_late_cutoff = std::chrono::steady_clock::time_point(
    std::chrono::steady_clock::duration(d.last_late_ns.load()));
  s.cutoffs_on_behalf = d.cutoffs_on_behalf;
  s.cutoffs_past_tolerance = d.cutoffs_past_tolerance;
  s.last_past_cutoff = std::chrono::steady_clock::time_point(
    std::chrono::steady_clock::duration(d.last_past_ns.load()));
  s.max_poll_gap_ms = d.max_poll_gap_ms;
  s.last_long_poll_gap = std::chrono::steady_clock::time_point(
    std::chrono::steady_clock::duration(d.last_long_gap_ns.load()));
  s.cutoff_slack_ms = d.cutoff_slack_ms;
  s.late_tolerance_ms = d.cfg.late_tolerance.count();
  s.late_starts = d.late_starts;
  s.plans_cut_off = d.plans_cut_off;
  s.cancels_failed = d.cancels_failed;
  s.cancel_discards = d.cancel_discards;
  s.close_discards = d.close_discards;
  s.windows_rekeyed = d.windows_rekeyed;
  s.key_collisions = d.key_collisions;
  s.rekeys_in_place = d.rekeys_in_place;
  s.rekeys_reregistered = d.rekeys_reregistered;
  s.rekey_in_place = d.rekey_in_place;
  s.unsafe = d.unsafe;
  s.broken = d.broken;
  s.windows = d.nwindows;
  return s;
}

std::string Endpoint::last_error() const
{
  return impl->get_err();
}

std::chrono::milliseconds Endpoint::key_quarantine() const
{
  return impl->cfg.key_quarantine;
}

bool Endpoint::unsafe() const
{
  return impl->unsafe;
}

std::unique_ptr<WindowPool> WindowPool::create(Endpoint& ep, char* mem,
					       size_t size, size_t count,
					       std::string* err)
{
  std::unique_ptr<WindowPool> p(new WindowPool(ep, size));
  p->slots.resize(count);
  for (size_t i = 0; i < count; i++) {
    if (int r = ep.register_window(mem + i * size, size, &p->slots[i].w); r < 0) {
      *err = "registering window " + std::to_string(i) + ": " + ep.last_error();
      p->slots.resize(i);
      return nullptr;
    }
  }
  return p;
}

WindowPool::~WindowPool()
{
  for (auto& s : slots) {
    ep.deregister_window(s.w.id);
  }
}

std::optional<WindowPool::lent_t> WindowPool::acquire(size_t size)
{
  if (size > slot_size) {
    return std::nullopt;
  }
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard l(mtx);
  for (size_t i = 0; i < slots.size(); i++) {
    auto& s = slots[i];
    if (s.in_use || now < s.quarantined_until) {
      continue;
    }
    std::string token = ep.window_token(s.w, 0, slot_size);
    if (token.empty()) {
      return std::nullopt;  // the endpoint lends no more
    }
    s.in_use = true;
    st.acquired++;
    return lent_t{i, s.w.ptr, slot_size, std::move(token)};
  }
  st.exhausted++;
  return std::nullopt;
}

void WindowPool::release(uint64_t id, std::chrono::milliseconds quarantine,
			 bool rekey)
{
  std::lock_guard l(mtx);
  if (id >= slots.size()) {
    return;
  }
  auto& s = slots[id];
  s.in_use = false;
  if (rekey) {
    if (ep.rekey_window(s.w) == 0) {
      // nothing meant for the last operation can land any more
      st.rekeyed++;
      s.quarantined_until = {};
      return;
    }
    // writes with the old key may land until it has left the network
    st.rekey_failed++;
    quarantine = std::max(quarantine, ep.key_quarantine());
  }
  if (quarantine.count() > 0) {
    s.quarantined_until = std::chrono::steady_clock::now() + quarantine;
  }
}

WindowPool::stats_t WindowPool::stats() const
{
  std::lock_guard l(mtx);
  return st;
}

} // namespace ceph::ofi
