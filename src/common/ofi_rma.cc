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

#include <poll.h>

namespace ceph::ofi {

namespace {

/// largest single write; plans are a stripe or a shard extent, so this
/// only splits unusually large ranges
constexpr uint64_t MAX_WRITE = 8ull << 20;
/// longest endpoint name a token may carry, in bytes
constexpr size_t MAX_NAME = 192;
/// peers kept in the address vector before idle ones are dropped
constexpr size_t MAX_PEERS = 1024;
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

  char* stage = nullptr;
  region_t stage_mr;
  std::vector<bool> stage_busy;

  /// The address vector's peers. They have a lock of their own, so that
  /// a write can wait for a peer's insert without mtx, which an insert
  /// on a FI_THREAD_DOMAIN domain holds. Lock order: mtx, then peer_mtx.
  enum class peer_state { pending, ready, failed };
  struct peer_t {
    /// the name, zero-padded so the provider never reads past it, and
    /// kept for the entry's life (some providers keep the pointer)
    std::string addr_buf;
    fi_addr_t addr = FI_ADDR_NOTAVAIL;
    peer_state state = peer_state::pending;
    int err = 0;  ///< why the insert failed
    std::chrono::steady_clock::time_point retry_at{};
    /// writes that hold the address: from the lookup until they retire.
    /// The entry is not dropped while any do.
    uint32_t refs = 0;
  };
  std::mutex peer_mtx;
  std::condition_variable peer_cv;    ///< a pending insert finished
  std::condition_variable insert_cv;  ///< insert_q has work
  std::map<std::string, peer_t> peers;
  std::deque<std::string> insert_q;
  std::thread insert_thr;

  struct plan_t;
  /// one posted write; the context must stay put until it completes
  struct op_t {
    fi_context2 ctx;
    plan_t* plan = nullptr;
  };
  struct plan_t {
    std::vector<op_t> ops;  ///< reserved up front, never reallocated
    uint32_t outstanding = 0;
    int err = 0;
    size_t slot = 0;
    std::string peer;
    std::list<std::unique_ptr<plan_t>>::iterator self;
  };
  std::list<std::unique_ptr<plan_t>> plans;
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

  /// set when a cut-off could not reopen the endpoint; every write
  /// fails from then on
  bool broken = false;
  std::atomic<uint64_t> resets{0};
  /// how long a cut-off takes, at most, as measured, in milliseconds; a
  /// write stops waiting this much before its budget runs out
  std::atomic<int64_t> reset_cost_ms{100};

  ~Impl();
  /// open the completion queue and the endpoint, bind and enable them,
  /// and read the endpoint's name
  int open_ep(std::string* err);
  /// cut off every write in flight by closing and reopening the
  /// endpoint; plans still waiting fail with -ECANCELED
  void reset_locked();
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
  /// the insert thread: adds queued peers to the address vector
  void insert_loop();
  /// drop idle peers when the address vector is full; under peer_mtx,
  /// and under mtx too unless the domain is thread-safe
  void evict_locked();
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
  // waits for an insert still inside the provider
  if (insert_thr.joinable()) {
    insert_thr.join();
  }
  // the endpoint first: it cancels whatever is still in flight, after
  // which no write references the regions
  if (ep) fi_close(&ep->fid);
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

void Endpoint::Impl::retire_locked(plan_t* p)
{
  stage_busy[p->slot] = false;
  put_peer(p->peer);
  plans.erase(p->self);
}

void Endpoint::Impl::complete_locked(op_t* op, int err)
{
  plan_t* p = op->plan;
  if (!p || p->outstanding == 0) {
    return;
  }
  p->outstanding--;
  if (err) {
    writes_failed++;
    if (!p->err) {
      p->err = err;
    }
  }
  if (p->outstanding == 0) {
    done_cv.notify_all();
  }
}

size_t Endpoint::Impl::poll_locked()
{
  size_t got = 0;
  if (!cq) {
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

int Endpoint::Impl::get_peer(const std::string& name, time_point deadline,
			     fi_addr_t* out)
{
  std::unique_lock pl(peer_mtx);
  if (stopping) {
    return -ESHUTDOWN;
  }
  auto [it, fresh] = peers.try_emplace(name);
  peer_t& p = it->second;
  const auto now = std::chrono::steady_clock::now();
  if (fresh) {
    p.addr_buf.assign(std::max(name.size(), MAX_NAME), '\0');
    std::memcpy(p.addr_buf.data(), name.data(), name.size());
  }
  if (fresh || (p.state == peer_state::failed && now >= p.retry_at)) {
    p.state = peer_state::pending;
    p.err = 0;
    insert_q.push_back(name);
    insert_cv.notify_one();
  }
  // the reference keeps the entry while this write waits on it
  p.refs++;
  const bool settled = peer_cv.wait_until(pl, deadline, [&] {
    return p.state != peer_state::pending || stopping;
  });
  if (!settled) {
    p.refs--;
    return -ETIMEDOUT;
  }
  if (p.state != peer_state::ready) {
    p.refs--;
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

void Endpoint::Impl::evict_locked()
{
  if (peers.size() <= MAX_PEERS) {
    return;
  }
  for (auto it = peers.begin(); it != peers.end(); ) {
    peer_t& p = it->second;
    if (p.refs == 0 && p.state != peer_state::pending) {
      if (p.state == peer_state::ready) {
	fi_av_remove(av, &p.addr, 1, 0);
      }
      it = peers.erase(it);
    } else {
      ++it;
    }
  }
}

void Endpoint::Impl::insert_loop()
{
  while (true) {
    std::string name;
    char* addr_buf;
    {
      std::unique_lock pl(peer_mtx);
      insert_cv.wait(pl, [this] { return stopping || !insert_q.empty(); });
      if (stopping) {
	return;
      }
      name = std::move(insert_q.front());
      insert_q.pop_front();
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
	l.unlock();
	std::this_thread::sleep_for(std::chrono::milliseconds(1));
	l.lock();
      }
    }
    fi_addr_t addr = FI_ADDR_NOTAVAIL;
    int r = -FI_ESHUTDOWN;
    if (!stopping) {
      {
	std::lock_guard pl(peer_mtx);
	evict_locked();
      }
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
  return 0;
}

void Endpoint::Impl::reset_locked()
{
  const auto t0 = std::chrono::steady_clock::now();
  resets++;
  // Cut off: closing the endpoint cancels every operation it still has
  // outstanding, so none of them retries into a peer's window later.
  // The completion queue goes too, with any completions of those
  // operations, whose contexts are about to be released.
  if (ep) {
    fi_close(&ep->fid);
    ep = nullptr;
  }
  if (cq) {
    fi_close(&cq->fid);
    cq = nullptr;
    cq_fd = -1;
  }
  // every plan with writes in flight has lost them, and its writer has
  // to hear it. One with none in flight has lost nothing: it is done, or
  // its writer is still gathering or posting and goes on with the new
  // endpoint.
  for (auto& p : plans) {
    if (p->outstanding == 0) {
      continue;
    }
    p->outstanding = 0;
    if (!p->err) {
      p->err = -ECANCELED;
    }
  }
  done_cv.notify_all();
  // regions bound to the old endpoint are gone with it
  const bool rebind = mr_mode & FI_MR_ENDPOINT;
  if (rebind) {
    for (auto& [id, w] : windows) {
      if (w.mr) {
	fi_close(&w.mr->fid);
	w.mr = nullptr;
      }
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
      if (reg(w.ptr, w.len, FI_REMOTE_WRITE, &fresh) < 0) {
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
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - t0);
  reset_cost_ms = std::max<int64_t>(reset_cost_ms, took.count() + 50);
}

Endpoint::Endpoint(std::unique_ptr<Impl> i) : impl(std::move(i)) {}

Endpoint::~Endpoint() = default;

std::unique_ptr<Endpoint> Endpoint::open(const config_t& cfg, std::string* err)
{
  auto d = std::make_unique<Impl>();
  d->cfg = cfg;
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
  av_attr.count = 0;  // the provider's default; shm caps it below MAX_PEERS
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
    d->insert_thr = std::thread([raw] { raw->insert_loop(); });
  }

  if (cfg.progress_thread) {
    Impl* raw = d.get();
    d->progress_thr = std::thread([raw] {
      while (!raw->stopping) {
	{
	  std::lock_guard l(raw->mtx);
	  raw->poll_locked();
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
  }
  return s;
}

int Endpoint::register_window(char* ptr, size_t len, window_t* out)
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  Impl::region_t reg;
  if (int r = d.reg(ptr, len, FI_REMOTE_WRITE, &reg); r < 0) {
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
  if (it->second.mr) {
    fi_close(&it->second.mr->fid);
  }
  d.windows.erase(it);
  d.nwindows = d.windows.size();
}

std::string Endpoint::window_token(const window_t& w, uint64_t ofs,
				   uint64_t len) const
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  auto it = d.windows.find(w.id);
  if (it == d.windows.end() || !it->second.mr || ofs + len > it->second.len) {
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
  // stop waiting early enough to cut the writes off within the budget
  const std::chrono::milliseconds reset_cost{d.reset_cost_ms.load()};
  if (budget <= reset_cost) {
    return -ETIMEDOUT;
  }
  const auto deadline = std::chrono::steady_clock::now() + budget -
    reset_cost;
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
  l.lock();
  if (d.broken || !d.ep) {
    // a cut-off failed to reopen the endpoint while we copied
    d.retire_locked(plan);
    return -EIO;
  }

  for (const auto& w : writes) {
    for (uint64_t o = 0; o < w.len && !plan->err; ) {
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
	if (plan->err || d.broken) {
	  break;  // a cut-off took what this plan had posted
	}
      }
      if (r || plan->err || d.broken) {
	plan->ops.pop_back();
	if (!plan->err) {
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
    if (plan->err) {
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
  while (true) {
    if (plan->outstanding == 0) {
      leave();
      const int res = plan->err;
      if (!res) {
	d.bytes_written += total;
      }
      d.retire_locked(plan);
      return res;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      // a last look: what completed during the wait counts
      d.poll_locked();
      if (plan->outstanding == 0) {
	continue;
      }
      // out of budget with writes in flight: cut them off, so none of
      // them lands in the peer's window after the caller gave up
      leave();
      d.timeouts++;
      d.set_err("writes still in flight at the deadline; cut off");
      d.reset_locked();
      d.retire_locked(plan);
      return -ETIMEDOUT;
    }
    if (!polling && !d.poller) {
      polling = d.poller = true;
      spin_until = std::chrono::steady_clock::now() + SPIN_TIME;
    }
    if (!polling) {
      d.done_cv.wait_until(l, deadline);
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const size_t got = d.poll_locked();
    const auto t1 = std::chrono::steady_clock::now();
    d.poll_floor = std::min(d.poll_floor, t1 - t0);
    if (got || t1 - t0 > 2 * d.poll_floor + POLL_BUSY_MARGIN) {
      spin_until = t1 + SPIN_TIME;
    }
    if (plan->outstanding) {
      d.wait_cq(l, deadline, spin_until);
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
  s.windows = d.nwindows;
  return s;
}

std::string Endpoint::last_error() const
{
  return impl->get_err();
}

} // namespace ceph::ofi
