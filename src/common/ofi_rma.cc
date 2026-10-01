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
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace ceph::ofi {

namespace {

/// largest single write; plans are a stripe or a shard extent, so this
/// only splits unusually large ranges
constexpr uint64_t MAX_WRITE = 8ull << 20;
/// longest endpoint name a token may carry, in bytes
constexpr size_t MAX_NAME = 192;
/// peers kept in the address vector before idle ones are dropped
constexpr size_t MAX_PEERS = 1024;
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
  std::string prov;
  std::string my_name;
  uint64_t mr_mode = 0;
  bool dc = false;
  uint64_t max_write = MAX_WRITE;
  uint64_t next_key = 1;

  /// every libfabric call on this endpoint runs under this lock
  /// (FI_THREAD_DOMAIN), and so does every touch of the state below
  mutable std::mutex mtx;

  struct region_t {
    fid_mr* mr = nullptr;
    char* ptr = nullptr;
    size_t len = 0;
    uint64_t key = 0;
    void* desc = nullptr;
  };
  std::map<uint64_t, region_t> windows;
  uint64_t next_window = 0;

  char* stage = nullptr;
  region_t stage_mr;
  std::vector<bool> stage_busy;

  struct peer_t {
    /// the name, zero-padded so the provider never reads past it, and
    /// kept for the entry's life (some providers keep the pointer)
    std::string addr_buf;
    fi_addr_t addr = FI_ADDR_NOTAVAIL;
    uint32_t inflight = 0;
  };
  std::map<std::string, peer_t> peers;

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
    bool abandoned = false;  ///< its caller timed out and left
    size_t slot = 0;
    std::string peer;
    std::list<std::unique_ptr<plan_t>>::iterator self;
  };
  std::list<std::unique_ptr<plan_t>> plans;

  std::thread progress_thr;
  std::atomic<bool> stopping{false};

  std::string last_err;
  std::atomic<uint64_t> writes_posted{0};
  std::atomic<uint64_t> writes_failed{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> peers_inserted{0};
  std::atomic<uint64_t> staging_busy{0};
  std::atomic<uint64_t> timeouts{0};

  ~Impl();
  /// register memory; a region others write into needs a key, a local
  /// write source only a descriptor
  int reg(char* ptr, size_t len, uint64_t access, region_t* out);
  void poll_locked();
  void complete_locked(op_t* op, int err);
  void retire_locked(plan_t* p);
  int peer_locked(const std::string& name, fi_addr_t* out);
};

Endpoint::Impl::~Impl()
{
  stopping = true;
  if (progress_thr.joinable()) {
    progress_thr.join();
  }
  // the endpoint first: it cancels whatever an abandoned plan still has
  // in flight, after which no write references the regions
  if (ep) fi_close(&ep->fid);
  for (auto& [id, w] : windows) {
    fi_close(&w.mr->fid);
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
    last_err = "fi_mr_reg: " + fi_err(r);
    return to_errno(r);
  }
  if (mr_mode & FI_MR_ENDPOINT) {
    r = fi_mr_bind(mr, &ep->fid, 0);
    if (!r) {
      r = fi_mr_enable(mr);
    }
    if (r) {
      last_err = "binding a region to the endpoint: " + fi_err(r);
      fi_close(&mr->fid);
      return to_errno(r);
    }
  }
  const uint64_t key = fi_mr_key(mr);
  if ((access & FI_REMOTE_WRITE) && key == FI_KEY_NOTAVAIL) {
    last_err = "the provider gave the region no key";
    fi_close(&mr->fid);
    return -EOPNOTSUPP;
  }
  *out = region_t{mr, ptr, len, key, fi_mr_desc(mr)};
  return 0;
}

void Endpoint::Impl::retire_locked(plan_t* p)
{
  stage_busy[p->slot] = false;
  if (auto it = peers.find(p->peer); it != peers.end() && it->second.inflight) {
    it->second.inflight--;
  }
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
  if (p->abandoned && p->outstanding == 0) {
    retire_locked(p);
  }
}

void Endpoint::Impl::poll_locked()
{
  fi_cq_entry ent[16];
  for (int round = 0; round < 8; round++) {
    const ssize_t n = fi_cq_read(cq, ent, 16);
    if (n > 0) {
      for (ssize_t i = 0; i < n; i++) {
	if (ent[i].op_context) {
	  complete_locked(static_cast<op_t*>(ent[i].op_context), 0);
	}
      }
      if (n < 16) {
	return;
      }
      continue;
    }
    if (n == -FI_EAVAIL) {
      fi_cq_err_entry e{};
      if (fi_cq_readerr(cq, &e, 0) < 0) {
	return;
      }
      const char* text = fi_cq_strerror(cq, e.prov_errno, e.err_data,
					nullptr, 0);
      last_err = fi_err(e.err) + (text ? std::string(" (") + text + ")" : "");
      if (e.op_context) {
	complete_locked(static_cast<op_t*>(e.op_context),
			e.err ? to_errno(e.err) : -EIO);
      }
      continue;
    }
    return;  // -FI_EAGAIN: nothing more, or an error with no entry
  }
}

int Endpoint::Impl::peer_locked(const std::string& name, fi_addr_t* out)
{
  if (auto it = peers.find(name); it != peers.end()) {
    *out = it->second.addr;
    return 0;
  }
  if (peers.size() >= MAX_PEERS) {
    for (auto it = peers.begin(); it != peers.end(); ) {
      if (it->second.inflight == 0) {
	fi_av_remove(av, &it->second.addr, 1, 0);
	it = peers.erase(it);
      } else {
	++it;
      }
    }
  }
  auto [it, inserted] = peers.emplace(name, peer_t{});
  auto& p = it->second;
  p.addr_buf.assign(std::max(name.size(), MAX_NAME), '\0');
  std::memcpy(p.addr_buf.data(), name.data(), name.size());
  const int r = fi_av_insert(av, p.addr_buf.data(), 1, &p.addr, 0, nullptr);
  if (r != 1) {
    last_err = "fi_av_insert: " + (r < 0 ? fi_err(r) : std::string("rejected"));
    peers.erase(it);
    return r < 0 ? to_errno(r) : -EHOSTUNREACH;
  }
  peers_inserted++;
  *out = p.addr;
  return 0;
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
  hints->domain_attr->threading = FI_THREAD_DOMAIN;
  hints->fabric_attr->prov_name = strdup(cfg.provider.c_str());
  if (!cfg.domain.empty()) {
    hints->domain_attr->name = strdup(cfg.domain.c_str());
  }
  const char* node = cfg.node.empty() ? nullptr : cfg.node.c_str();
  const char* service = cfg.service.empty() ? nullptr : cfg.service.c_str();
  const uint64_t flags = (node || service) ? FI_SOURCE : 0;
  // ask for completions that mean "placed in the target's memory"; a
  // provider that cannot promise it is still usable, see
  // delivery_complete()
  hints->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
  int r = fi_getinfo(API_VERSION, node, service, flags, hints, &d->info);
  if (r == -FI_ENODATA) {
    hints->tx_attr->op_flags = 0;
    r = fi_getinfo(API_VERSION, node, service, flags, hints, &d->info);
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
  fi_cq_attr cq_attr{};
  cq_attr.format = FI_CQ_FORMAT_CONTEXT;
  cq_attr.size = 4096;
  cq_attr.wait_obj = FI_WAIT_NONE;
  if ((r = fi_cq_open(d->domain, &cq_attr, &d->cq, nullptr))) {
    *err = "fi_cq_open: " + fi_err(r);
    return nullptr;
  }
  if ((r = fi_endpoint(d->domain, info, &d->ep, nullptr))) {
    *err = "fi_endpoint: " + fi_err(r);
    return nullptr;
  }
  if ((r = fi_ep_bind(d->ep, &d->av->fid, 0)) ||
      (r = fi_ep_bind(d->ep, &d->cq->fid, FI_TRANSMIT | FI_RECV)) ||
      (r = fi_enable(d->ep))) {
    *err = "enabling the endpoint: " + fi_err(r);
    return nullptr;
  }
  size_t len = 0;
  r = fi_getname(&d->ep->fid, nullptr, &len);
  if (r != -FI_ETOOSMALL || len == 0 || len > MAX_NAME) {
    *err = "fi_getname: " + (r ? fi_err(r) : std::string("bad length"));
    return nullptr;
  }
  d->my_name.resize(len);
  if ((r = fi_getname(&d->ep->fid, d->my_name.data(), &len))) {
    *err = "fi_getname: " + fi_err(r);
    return nullptr;
  }
  d->my_name.resize(len);

  if (cfg.stage_size && cfg.stage_count) {
    const size_t total = cfg.stage_size * cfg.stage_count;
    void* p = nullptr;
    if (posix_memalign(&p, 4096, total) != 0 || !p) {
      *err = "cannot allocate " + std::to_string(total) + " staging bytes";
      return nullptr;
    }
    d->stage = static_cast<char*>(p);
    if ((r = d->reg(d->stage, total, FI_WRITE, &d->stage_mr))) {
      *err = "registering staging: " + d->last_err;
      return nullptr;
    }
    d->stage_busy.assign(cfg.stage_count, false);
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
  fi_close(&it->second.mr->fid);
  d.windows.erase(it);
}

std::string Endpoint::window_token(const window_t& w, uint64_t ofs,
				   uint64_t len) const
{
  auto& d = *impl;
  std::lock_guard l(d.mtx);
  auto it = d.windows.find(w.id);
  if (it == d.windows.end() || ofs + len > it->second.len) {
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
		    size_t iovcnt, const std::vector<write_t>& writes)
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
  const auto deadline = std::chrono::steady_clock::now() + d.cfg.op_timeout;

  std::unique_lock l(d.mtx);
  size_t slot = 0;
  while (slot < d.stage_busy.size() && d.stage_busy[slot]) {
    slot++;
  }
  if (slot == d.stage_busy.size()) {
    d.staging_busy++;
    return -EBUSY;
  }
  char* buf = d.stage + slot * d.cfg.stage_size;
  {
    char* p = buf;
    for (size_t i = 0; i < iovcnt; i++) {
      std::memcpy(p, iov[i].iov_base, iov[i].iov_len);
      p += iov[i].iov_len;
    }
  }
  fi_addr_t addr;
  if (int r = d.peer_locked(dst.name, &addr); r < 0) {
    return r;
  }
  d.stage_busy[slot] = true;
  d.peers[dst.name].inflight++;
  d.plans.push_front(std::make_unique<Impl::plan_t>());
  Impl::plan_t* plan = d.plans.front().get();
  plan->self = d.plans.begin();
  plan->slot = slot;
  plan->peer = dst.name;
  plan->ops.reserve(chunks);

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
	// is out, then retry
	d.poll_locked();
	if (std::chrono::steady_clock::now() > deadline) {
	  break;
	}
      }
      if (r) {
	d.last_err = "fi_write: " + fi_err(r);
	d.writes_failed++;
	plan->err = r == -FI_EAGAIN ? -ETIMEDOUT : to_errno(r);
	plan->ops.pop_back();
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

  while (true) {
    d.poll_locked();
    if (plan->outstanding == 0) {
      const int res = plan->err;
      if (!res) {
	d.bytes_written += total;
      }
      d.retire_locked(plan);
      return res;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      // the staging buffer stays claimed until the writes still in
      // flight complete; whoever polls then retires the plan
      plan->abandoned = true;
      d.timeouts++;
      return -ETIMEDOUT;
    }
    l.unlock();
    std::this_thread::yield();
    l.lock();
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
  std::lock_guard l(d.mtx);
  s.windows = d.windows.size();
  return s;
}

std::string Endpoint::last_error() const
{
  std::lock_guard l(impl->mtx);
  return impl->last_err;
}

} // namespace ceph::ofi
