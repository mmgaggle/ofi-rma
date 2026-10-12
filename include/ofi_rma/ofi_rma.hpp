// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp
// SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
// SPDX-License-Identifier: MIT

#pragma once

/**
 * The C++ interface to ofi-rma: a header-only wrapper over the C library
 * in ofi_rma.h, which documents the behavior in full. It needs C++17 and
 * links with the C library alone, so a C++ program does not depend on the
 * library's own C++ ABI: there is none.
 */

#include <atomic>
#include <cerrno>
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

#include "ofi_rma/ofi_rma.h"

namespace ofi_rma {

/**
 * A libfabric delivery descriptor (see ofi_rma.h):
 *
 *   <base>:<size>:ofi1:<wire>:<address>:<key>[:<auth>]
 *
 * wire names the protocol on the wire and its version, such as "rxm.1";
 * name holds the owner's endpoint name bytes, and auth the bytes of the
 * authorization key the region is bound to, empty for none.
 */
struct token_t {
  uint64_t base = 0;
  uint64_t size = 0;
  std::string wire;
  std::string name;  ///< raw endpoint name bytes
  uint64_t key = 0;
  std::string auth;  ///< raw authorization key bytes, or empty
};

inline constexpr std::string_view TOKEN_TAG = OFI_RMA_TOKEN_TAG;

inline std::optional<token_t> parse_token(std::string_view token)
{
  ofi_rma_token t;
  if (ofi_rma_token_parse(token.data(), token.size(), &t) != 0) {
    return std::nullopt;
  }
  token_t o;
  o.base = t.base;
  o.size = t.size;
  o.wire = t.wire;
  o.name.assign(reinterpret_cast<const char*>(t.addr), t.addr_len);
  o.key = t.key;
  o.auth.assign(reinterpret_cast<const char*>(t.auth), t.auth_len);
  return o;
}

namespace detail {

/// a token_t as the C library takes it; false when a field does not fit
inline bool to_c(const token_t& t, ofi_rma_token* c)
{
  if (t.wire.size() >= sizeof(c->wire) || t.name.size() > sizeof(c->addr) ||
      t.auth.size() > sizeof(c->auth)) {
    return false;
  }
  *c = ofi_rma_token{};
  c->base = t.base;
  c->size = t.size;
  t.wire.copy(c->wire, t.wire.size());
  c->wire[t.wire.size()] = '\0';
  t.name.copy(reinterpret_cast<char*>(c->addr), t.name.size());
  c->addr_len = t.name.size();
  c->key = t.key;
  t.auth.copy(reinterpret_cast<char*>(c->auth), t.auth.size());
  c->auth_len = t.auth.size();
  return true;
}

inline std::chrono::steady_clock::time_point to_time(int64_t ns)
{
  return std::chrono::steady_clock::time_point(
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::nanoseconds(ns)));
}

inline int64_t to_ns(std::chrono::steady_clock::time_point t)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    t.time_since_epoch()).count();
}

/// the text a function with snprintf() semantics writes
template <typename F>
std::string text(F&& f)
{
  char small[256];
  const size_t n = f(small, sizeof(small));
  if (n < sizeof(small)) {
    return std::string(small, n);
  }
  std::string s(n + 1, '\0');
  f(s.data(), s.size());
  s.resize(n);
  return s;
}

} // namespace detail

/// the token's text; empty for a token whose fields cannot be written
inline std::string format_token(const token_t& t)
{
  ofi_rma_token c;
  if (!detail::to_c(t, &c)) {
    return {};
  }
  char buf[OFI_RMA_TOKEN_MAX];
  const ssize_t n = ofi_rma_token_format(&c, buf, sizeof(buf));
  return n < 0 ? std::string() : std::string(buf, static_cast<size_t>(n));
}

/// N for a token of the ofi<N> family, 0 for any other descriptor
inline unsigned token_family(std::string_view token)
{
  return ofi_rma_token_family(token.data(), token.size());
}

/**
 * Where a window's memory lives. Host memory needs nothing more; see
 * ofi_rma_memory for device memory and dma-bufs.
 */
struct memory_t {
  /// libfabric's fi_hmem_iface, for the kinds this library passes on
  enum class iface_t { system, cuda, rocr, ze };
  iface_t iface = iface_t::system;
  /// the device ordinal, for cuda and ze
  int device = 0;
  /// a dma-buf that exports the memory, or -1
  int dmabuf_fd = -1;
  /// where the memory starts in the dma-buf
  uint64_t dmabuf_offset = 0;
};

namespace detail {

inline ofi_rma_memory to_c(const memory_t& m)
{
  ofi_rma_memory c;
  ofi_rma_memory_init(&c);
  switch (m.iface) {
  case memory_t::iface_t::cuda: c.iface = OFI_RMA_IFACE_CUDA; break;
  case memory_t::iface_t::rocr: c.iface = OFI_RMA_IFACE_ROCR; break;
  case memory_t::iface_t::ze: c.iface = OFI_RMA_IFACE_ZE; break;
  default: c.iface = OFI_RMA_IFACE_SYSTEM; break;
  }
  c.device = m.device;
  c.dmabuf_fd = m.dmabuf_fd;
  c.dmabuf_offset = m.dmabuf_offset;
  return c;
}

inline memory_t from_c(const ofi_rma_memory& c)
{
  memory_t m;
  switch (c.iface) {
  case OFI_RMA_IFACE_CUDA: m.iface = memory_t::iface_t::cuda; break;
  case OFI_RMA_IFACE_ROCR: m.iface = memory_t::iface_t::rocr; break;
  case OFI_RMA_IFACE_ZE: m.iface = memory_t::iface_t::ze; break;
  default: m.iface = memory_t::iface_t::system; break;
  }
  m.device = c.device;
  m.dmabuf_fd = c.dmabuf_fd;
  m.dmabuf_offset = c.dmabuf_offset;
  return m;
}

} // namespace detail

/// See ofi_rma_config for every field.
struct config_t {
  std::string provider;  ///< libfabric provider name; required
  std::string domain;    ///< domain (device or interface) name; optional
  std::string node;      ///< local address to bind; optional
  std::string service;   ///< local service (port) to bind; optional
  size_t stage_size = 0;
  size_t stage_count = 0;
  /// staging in the caller's memory: see ofi_rma_config.stage_mem.
  /// stage_alloc returns 0 and the offset of a buffer of at least len
  /// bytes, or -EBUSY; both run with the endpoint's lock held
  void* stage_mem = nullptr;
  size_t stage_mem_len = 0;
  std::function<int(size_t len, uint64_t* offset)> stage_alloc;
  std::function<void(uint64_t offset, size_t len)> stage_free;
  bool progress_thread = false;
  bool hmem = false;
  bool reads = false;
  bool thread_safe = true;
  std::chrono::milliseconds cutoff_cost_initial{100};
  std::chrono::milliseconds cutoff_cost_max{2000};
  std::chrono::milliseconds late_tolerance{1000};
  bool late_fail_closed = false;
  /// for tests: see ofi_rma_hooks.wait
  std::function<void(std::chrono::steady_clock::time_point)> wait_hook;
  std::chrono::milliseconds key_quarantine{10000};
  size_t mr_cnt = 16384;
  size_t max_peers = 1024;
  size_t max_pending_inserts = 64;
  unsigned insert_threads = 4;
  std::chrono::milliseconds insert_wait{10};
  std::chrono::milliseconds lent_wait{0};
  /// for tests: see ofi_rma_hooks.insert
  std::function<void(const std::string&)> insert_hook;
  /// for tests: see ofi_rma_hooks.pre_post
  std::function<void()> pre_post_hook;
  bool per_plan_cutoff = true;
  /// for tests: see ofi_rma_hooks.cancel
  std::function<int()> cancel_hook;
  /// for tests: see ofi_rma_hooks.reg
  std::function<void(const char*, size_t, const memory_t&)> reg_hook;
  /// for tests: see ofi_rma_hooks.cutoff_close
  std::function<int()> cutoff_close_hook;
};

class WindowPool;

class Endpoint {
public:
  /// open the endpoint; on failure returns null and sets *err
  static std::unique_ptr<Endpoint> open(const config_t& cfg, std::string* err)
  {
    auto hooks = std::make_unique<hooks_t>();
    hooks->wait = cfg.wait_hook;
    hooks->insert = cfg.insert_hook;
    hooks->pre_post = cfg.pre_post_hook;
    hooks->cancel = cfg.cancel_hook;
    hooks->reg = cfg.reg_hook;
    hooks->cutoff_close = cfg.cutoff_close_hook;
    hooks->stage_alloc = cfg.stage_alloc;
    hooks->stage_free = cfg.stage_free;
    ofi_rma_hooks h{};
    h.ctx = hooks.get();
    if (hooks->wait) h.wait = &wait_tramp;
    if (hooks->insert) h.insert = &insert_tramp;
    if (hooks->pre_post) h.pre_post = &pre_post_tramp;
    if (hooks->cancel) h.cancel = &cancel_tramp;
    if (hooks->reg) h.reg = &reg_tramp;
    if (hooks->cutoff_close) h.cutoff_close = &cutoff_close_tramp;

    ofi_rma_config c;
    ofi_rma_config_init(&c);
    c.provider = cfg.provider.c_str();
    c.domain = cfg.domain.c_str();
    c.node = cfg.node.c_str();
    c.service = cfg.service.c_str();
    c.stage_size = cfg.stage_size;
    c.stage_count = cfg.stage_count;
    c.progress_thread = cfg.progress_thread;
    c.hmem = cfg.hmem;
    c.reads = cfg.reads;
    c.thread_safe = cfg.thread_safe;
    c.cutoff_cost_initial_ms = cfg.cutoff_cost_initial.count();
    c.cutoff_cost_max_ms = cfg.cutoff_cost_max.count();
    c.late_tolerance_ms = cfg.late_tolerance.count();
    c.late_fail_closed = cfg.late_fail_closed;
    c.key_quarantine_ms = cfg.key_quarantine.count();
    c.mr_cnt = cfg.mr_cnt;
    c.max_peers = cfg.max_peers;
    c.max_pending_inserts = cfg.max_pending_inserts;
    c.insert_threads = cfg.insert_threads;
    c.insert_wait_ms = cfg.insert_wait.count();
    c.lent_wait_ms = cfg.lent_wait.count();
    c.per_plan_cutoff = cfg.per_plan_cutoff;
    c.hooks = &h;
    if (cfg.stage_mem) {
      c.stage_mem = cfg.stage_mem;
      c.stage_mem_len = cfg.stage_mem_len;
      c.stage_alloc = cfg.stage_alloc ? &stage_alloc_tramp : nullptr;
      c.stage_free = cfg.stage_free ? &stage_free_tramp : nullptr;
      c.stage_ctx = hooks.get();
    }
    ofi_rma_ep* ep = nullptr;
    char e[1024];
    if (ofi_rma_ep_open(&c, &ep, e, sizeof(e)) != 0) {
      if (err) {
        *err = e;
      }
      return nullptr;
    }
    return std::unique_ptr<Endpoint>(new Endpoint(ep, std::move(hooks)));
  }

  ~Endpoint() { ofi_rma_ep_close(ep_); }

  Endpoint(const Endpoint&) = delete;
  Endpoint& operator=(const Endpoint&) = delete;

  /// the C handle, for code that mixes the two interfaces
  ofi_rma_ep* handle() const { return ep_; }

  /// the provider's own name for itself
  const std::string& provider() const { return prov_; }
  /// the wire protocol this endpoint speaks, as tokens carry it
  const std::string& wire() const { return wire_; }
  /// this endpoint's name (fi_getname bytes); it can change with a cut-off
  std::string name() const
  {
    char buf[OFI_RMA_ADDR_MAX];
    const size_t n = ofi_rma_ep_name(ep_, buf, sizeof(buf));
    return std::string(buf, n < sizeof(buf) ? n : sizeof(buf));
  }
  /// fabric, domain and memory-registration mode, for logs
  std::string describe() const
  {
    return detail::text([this](char* b, size_t n) {
      return ofi_rma_ep_describe(ep_, b, n);
    });
  }
  bool delivery_complete() const { return ofi_rma_ep_delivery_complete(ep_); }

  /// a registered window peers may write into
  struct window_t {
    uint64_t id = 0;
    char* ptr = nullptr;
    size_t len = 0;
  };
  bool hmem() const { return ofi_rma_ep_hmem(ep_); }
  bool reads() const { return ofi_rma_ep_reads(ep_); }
  /// what a peer holding a window's token may do with it
  enum access_t : unsigned {
    remote_write = OFI_RMA_REMOTE_WRITE,
    remote_read = OFI_RMA_REMOTE_READ,
  };
  int register_window(char* ptr, size_t len, window_t* out)
  {
    return register_window(ptr, len, memory_t{}, remote_write, out);
  }
  int register_window(char* ptr, size_t len, const memory_t& mem,
                      window_t* out)
  {
    return register_window(ptr, len, mem, remote_write, out);
  }
  int register_window(char* ptr, size_t len, const memory_t& mem,
                      unsigned access, window_t* out)
  {
    const ofi_rma_memory m = detail::to_c(mem);
    ofi_rma_window w;
    const int r = ofi_rma_window_register(ep_, ptr, len, &m, access, &w);
    if (r == 0) {
      *out = window_t{w.id, static_cast<char*>(w.ptr), w.len};
    }
    return r;
  }
  void deregister_window(uint64_t id) { ofi_rma_window_deregister(ep_, id); }
  /// see ofi_rma_window_rekey()
  int rekey_window(const window_t& w)
  {
    const ofi_rma_window c{w.id, w.ptr, w.len};
    return ofi_rma_window_rekey(ep_, &c);
  }
  /// the token naming [ofs, ofs+len) of a registered window; empty when
  /// ofi_rma_window_token() fails
  std::string window_token(const window_t& w, uint64_t ofs, uint64_t len) const
  {
    const ofi_rma_window c{w.id, w.ptr, w.len};
    char buf[OFI_RMA_TOKEN_MAX];
    const ssize_t n = ofi_rma_window_token(ep_, &c, ofs, len, buf,
                                           sizeof(buf));
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf)) {
      return {};
    }
    return std::string(buf, static_cast<size_t>(n));
  }

  struct write_t {
    uint64_t src_ofs = 0;  ///< offset in the gathered source bytes
    uint64_t len = 0;
    uint64_t dst_ofs = 0;  ///< offset in the destination window
  };
  /// see ofi_rma_write(); a token whose fields do not fit is -EINVAL
  int write(const token_t& dst, const struct iovec* iov, size_t iovcnt,
            const std::vector<write_t>& writes,
            std::chrono::milliseconds budget, bool* posted = nullptr)
  {
    if (posted) {
      *posted = false;
    }
    ofi_rma_token t;
    if (!detail::to_c(dst, &t)) {
      return -EINVAL;
    }
    std::vector<ofi_rma_range> r;
    r.reserve(writes.size());
    for (const auto& w : writes) {
      r.push_back(ofi_rma_range{w.src_ofs, w.len, w.dst_ofs});
    }
    return ofi_rma_write(ep_, &t, iov, iovcnt, r.data(), r.size(),
                         budget.count(), posted);
  }

  struct read_t {
    uint64_t src_ofs = 0;  ///< offset in the source window
    uint64_t len = 0;
    uint64_t dst_ofs = 0;  ///< offset in the scattered destination bytes
  };
  /// see ofi_rma_read()
  int read(const token_t& src, const struct iovec* iov, size_t iovcnt,
           const std::vector<read_t>& reads,
           std::chrono::milliseconds budget, bool* posted = nullptr)
  {
    if (posted) {
      *posted = false;
    }
    ofi_rma_token t;
    if (!detail::to_c(src, &t)) {
      return -EINVAL;
    }
    std::vector<ofi_rma_range> r;
    r.reserve(reads.size());
    for (const auto& x : reads) {
      r.push_back(ofi_rma_range{x.dst_ofs, x.len, x.src_ofs});
    }
    return ofi_rma_read(ep_, &t, iov, iovcnt, r.data(), r.size(),
                        budget.count(), posted);
  }

  void progress() { ofi_rma_ep_progress(ep_); }
  void sync() { ofi_rma_ep_sync(ep_); }
  bool quiet() const { return ofi_rma_ep_quiet(ep_); }
  void windows_lent(std::chrono::steady_clock::time_point latest)
  {
    ofi_rma_ep_windows_lent(
      ep_, latest == std::chrono::steady_clock::time_point{} ? 0 :
      detail::to_ns(latest));
  }

  /// see ofi_rma_stats
  struct stats_t {
    uint64_t writes_posted = 0;
    uint64_t writes_failed = 0;
    uint64_t bytes_written = 0;
    uint64_t reads_posted = 0;
    uint64_t reads_failed = 0;
    uint64_t bytes_read = 0;
    uint64_t peers_inserted = 0;
    uint64_t staging_busy = 0;
    uint64_t timeouts = 0;
    uint64_t resets = 0;
    uint64_t peer_timeouts = 0;
    uint64_t inserts_refused = 0;
    uint64_t peers = 0;
    uint64_t pending_inserts = 0;
    uint64_t insert_busy = 0;
    uint64_t cutoffs_failed = 0;
    uint64_t cutoffs_late = 0;
    uint64_t max_cutoff_lateness_ms = 0;
    std::chrono::steady_clock::time_point last_late_cutoff{};
    uint64_t cutoffs_on_behalf = 0;
    uint64_t cutoffs_past_tolerance = 0;
    std::chrono::steady_clock::time_point last_past_cutoff{};
    uint64_t max_poll_gap_ms = 0;
    std::chrono::steady_clock::time_point last_long_poll_gap{};
    uint64_t cutoff_slack_ms = 0;
    uint64_t late_tolerance_ms = 0;
    uint64_t plans_cut_off = 0;
    uint64_t cancels_failed = 0;
    bool cancel_discards = false;
    int close_discards = -1;
    uint64_t budget_refused = 0;
    uint64_t cutoff_cost_ms = 0;
    uint64_t late_starts = 0;
    uint64_t windows_rekeyed = 0;
    uint64_t key_collisions = 0;
    uint64_t rekeys_in_place = 0;
    uint64_t rekeys_reregistered = 0;
    int rekey_in_place = -1;
    bool unsafe = false;
    bool broken = false;
    uint64_t windows = 0;
  };
  stats_t stats() const
  {
    ofi_rma_stats c{};
    c.size = sizeof(c);
    ofi_rma_ep_get_stats(ep_, &c);
    stats_t s;
    s.writes_posted = c.writes_posted;
    s.writes_failed = c.writes_failed;
    s.bytes_written = c.bytes_written;
    s.reads_posted = c.reads_posted;
    s.reads_failed = c.reads_failed;
    s.bytes_read = c.bytes_read;
    s.peers_inserted = c.peers_inserted;
    s.staging_busy = c.staging_busy;
    s.timeouts = c.timeouts;
    s.resets = c.resets;
    s.peer_timeouts = c.peer_timeouts;
    s.inserts_refused = c.inserts_refused;
    s.peers = c.peers;
    s.pending_inserts = c.pending_inserts;
    s.insert_busy = c.insert_busy;
    s.cutoffs_failed = c.cutoffs_failed;
    s.cutoffs_late = c.cutoffs_late;
    s.max_cutoff_lateness_ms = c.max_cutoff_lateness_ms;
    s.last_late_cutoff = detail::to_time(c.last_late_cutoff_ns);
    s.cutoffs_on_behalf = c.cutoffs_on_behalf;
    s.cutoffs_past_tolerance = c.cutoffs_past_tolerance;
    s.last_past_cutoff = detail::to_time(c.last_past_cutoff_ns);
    s.max_poll_gap_ms = c.max_poll_gap_ms;
    s.last_long_poll_gap = detail::to_time(c.last_long_poll_gap_ns);
    s.cutoff_slack_ms = c.cutoff_slack_ms;
    s.late_tolerance_ms = c.late_tolerance_ms;
    s.plans_cut_off = c.plans_cut_off;
    s.cancels_failed = c.cancels_failed;
    s.cancel_discards = c.cancel_discards;
    s.close_discards = c.close_discards;
    s.budget_refused = c.budget_refused;
    s.cutoff_cost_ms = c.cutoff_cost_ms;
    s.late_starts = c.late_starts;
    s.windows_rekeyed = c.windows_rekeyed;
    s.key_collisions = c.key_collisions;
    s.rekeys_in_place = c.rekeys_in_place;
    s.rekeys_reregistered = c.rekeys_reregistered;
    s.rekey_in_place = c.rekey_in_place;
    s.unsafe = c.unsafe;
    s.broken = c.broken;
    s.windows = c.windows;
    return s;
  }
  /// the provider's text for the most recent failure
  std::string last_error() const
  {
    return detail::text([this](char* b, size_t n) {
      return ofi_rma_ep_last_error(ep_, b, n);
    });
  }
  bool unsafe() const { return ofi_rma_ep_unsafe(ep_); }
  std::chrono::milliseconds key_quarantine() const
  {
    return std::chrono::milliseconds(ofi_rma_ep_key_quarantine_ms(ep_));
  }

private:
  struct hooks_t {
    std::function<void(std::chrono::steady_clock::time_point)> wait;
    std::function<void(const std::string&)> insert;
    std::function<void()> pre_post;
    std::function<int()> cancel;
    std::function<void(const char*, size_t, const memory_t&)> reg;
    std::function<int()> cutoff_close;
    std::function<int(size_t, uint64_t*)> stage_alloc;
    std::function<void(uint64_t, size_t)> stage_free;
  };
  static int stage_alloc_tramp(void* c, size_t len, uint64_t* offset)
  {
    auto* h = static_cast<hooks_t*>(c);
    return h->stage_alloc ? h->stage_alloc(len, offset) : -EBUSY;
  }
  static void stage_free_tramp(void* c, uint64_t offset, size_t len)
  {
    auto* h = static_cast<hooks_t*>(c);
    if (h->stage_free) {
      h->stage_free(offset, len);
    }
  }
  static void wait_tramp(void* c, int64_t ns)
  {
    static_cast<hooks_t*>(c)->wait(detail::to_time(ns));
  }
  static void insert_tramp(void* c, const void* name, size_t len)
  {
    static_cast<hooks_t*>(c)->insert(
      std::string(static_cast<const char*>(name), len));
  }
  static void pre_post_tramp(void* c) { static_cast<hooks_t*>(c)->pre_post(); }
  static int cancel_tramp(void* c) { return static_cast<hooks_t*>(c)->cancel(); }
  static void reg_tramp(void* c, const void* ptr, size_t len,
                        const ofi_rma_memory* mem)
  {
    static_cast<hooks_t*>(c)->reg(static_cast<const char*>(ptr), len,
                                  detail::from_c(*mem));
  }
  static int cutoff_close_tramp(void* c)
  {
    return static_cast<hooks_t*>(c)->cutoff_close();
  }

  Endpoint(ofi_rma_ep* ep, std::unique_ptr<hooks_t> hooks)
    : hooks_(std::move(hooks)), ep_(ep),
      prov_(ofi_rma_ep_provider(ep)), wire_(ofi_rma_ep_wire(ep)) {}

  // the hooks outlive the C endpoint, which the destructor closes first
  std::unique_ptr<hooks_t> hooks_;
  ofi_rma_ep* ep_;
  std::string prov_;
  std::string wire_;
};

/// See ofi_rma_pool.
class WindowPool {
public:
  /// count windows of size bytes each, at mem; on failure, returns null
  /// and sets *err
  static std::unique_ptr<WindowPool> create(Endpoint& ep, char* mem,
                                            size_t size, size_t count,
                                            std::string* err)
  {
    ofi_rma_pool* p = nullptr;
    char e[1024];
    if (ofi_rma_pool_create(ep.handle(), mem, size, count, &p, e,
                            sizeof(e)) != 0) {
      if (err) {
        *err = e;
      }
      return nullptr;
    }
    return std::unique_ptr<WindowPool>(new WindowPool(p));
  }
  ~WindowPool() { ofi_rma_pool_destroy(pool_); }

  WindowPool(const WindowPool&) = delete;
  WindowPool& operator=(const WindowPool&) = delete;

  struct lent_t {
    uint64_t id = 0;
    char* ptr = nullptr;
    size_t size = 0;
    std::string token;  ///< for the whole window
  };
  /// a free window of at least size bytes, or nullopt; nullopt also while
  /// an insert on a FI_THREAD_DOMAIN domain keeps the endpoint quiet
  std::optional<lent_t> acquire(size_t size)
  {
    ofi_rma_lent l;
    if (ofi_rma_pool_acquire(pool_, size, &l) != 0) {
      return std::nullopt;
    }
    return lent_t{l.id, static_cast<char*>(l.ptr), l.size, l.token};
  }
  /// see ofi_rma_pool_release()
  void release(uint64_t id, std::chrono::milliseconds quarantine, bool rekey)
  {
    ofi_rma_pool_release(pool_, id, quarantine.count(), rekey);
  }

  struct stats_t {
    uint64_t acquired = 0;
    uint64_t exhausted = 0;
    uint64_t rekeyed = 0;
    uint64_t rekeyed_quarantined = 0;
    uint64_t rekey_failed = 0;
    uint64_t declined_insert = 0;
    uint64_t rekeys_put_off = 0;
  };
  stats_t stats() const
  {
    ofi_rma_pool_stats c{};
    c.size = sizeof(c);
    ofi_rma_pool_get_stats(pool_, &c);
    return stats_t{c.acquired, c.exhausted, c.rekeyed, c.rekeyed_quarantined,
                   c.rekey_failed, c.declined_insert, c.rekeys_put_off};
  }
  size_t count() const { return ofi_rma_pool_count(pool_); }
  size_t size() const { return ofi_rma_pool_window_size(pool_); }

private:
  explicit WindowPool(ofi_rma_pool* p) : pool_(p) {}
  ofi_rma_pool* pool_;
};

} // namespace ofi_rma
