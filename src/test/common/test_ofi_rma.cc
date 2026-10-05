// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

#include "common/ofi_rma.h"

#include <cerrno>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <array>
#include <mutex>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/resource.h>

#include <gtest/gtest.h>

using namespace ceph::ofi;

TEST(OfiToken, RoundTrip)
{
  token_t t;
  t.base = 0x7f0012345000;
  t.size = 0xc00000;
  t.provider = "verbs;ofi_rxm";
  t.name = std::string("\x02\x00\x12\x34\x0a\x00\x00\x01\x00\xff", 10);
  t.key = 0xdeadbeef;
  const auto s = format_token(t);
  EXPECT_EQ(s, "7f0012345000:c00000:ofi1:verbs;ofi_rxm:020012340a00000100ff:"
	    "deadbeef");
  auto p = parse_token(s);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->base, t.base);
  EXPECT_EQ(p->size, t.size);
  EXPECT_EQ(p->provider, t.provider);
  EXPECT_EQ(p->name, t.name);
  EXPECT_EQ(p->key, t.key);
}

TEST(OfiToken, Rejects)
{
  // the other transports' descriptors
  EXPECT_FALSE(parse_token("0:100000:uet1:10.88.0.30:1234"));
  EXPECT_FALSE(parse_token("7f0000001000:100000:1234:0:abc:1:fe80::1"));
  // shape
  EXPECT_FALSE(parse_token(""));
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp:0200:1:extra"));
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp:0200"));
  EXPECT_FALSE(parse_token("0:10:ofi2:tcp:0200:1"));
  // fields
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp:020:1"));      // odd hex
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp:02zz:1"));     // not hex
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp::1"));         // no name
  EXPECT_FALSE(parse_token("0:10:ofi1::0200:1"));        // no provider
  EXPECT_FALSE(parse_token("0:10:ofi1:t p:0200:1"));     // provider charset
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp:0200:"));      // no key
  EXPECT_FALSE(parse_token("12345678901234567:10:ofi1:tcp:0200:1"));
  EXPECT_FALSE(parse_token("0:10:ofi1:tcp:" + std::string(386, 'a') + ":1"));
  EXPECT_TRUE(parse_token("0:10:ofi1:tcp:" + std::string(384, 'a') + ":1"));
}

namespace {

constexpr std::chrono::milliseconds BUDGET{3000};

struct pair_t {
  std::unique_ptr<Endpoint> target;
  std::unique_ptr<Endpoint> writer;
};

/// a window owner and a writer on one provider, or nothing when the
/// provider is not available here
pair_t open_pair(const std::string& prov, const std::string& node)
{
  config_t c;
  c.provider = prov;
  c.node = node;
  config_t tc = c;
  tc.progress_thread = true;
  config_t wc = c;
  wc.stage_size = 4 << 20;
  wc.stage_count = 2;
  std::string err;
  pair_t p;
  p.target = Endpoint::open(tc, &err);
  if (p.target) {
    p.writer = Endpoint::open(wc, &err);
  }
  if (!p.target || !p.writer) {
    std::cerr << prov << ": " << err << std::endl;
    return {};
  }
  return p;
}

class OfiWrite : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};

} // anonymous namespace

TEST_P(OfiWrite, PlacesRanges)
{
  auto [prov, node] = GetParam();
  auto p = open_pair(prov, node);
  if (!p.target) {
    GTEST_SKIP() << prov << " is not available";
  }
  const size_t N = 3 << 20;
  std::vector<char> win(N, 0), src(N);
  for (size_t i = 0; i < N; i++) {
    src[i] = static_cast<char>(i * 7 + 3);
  }
  Endpoint::window_t w;
  ASSERT_EQ(0, p.target->register_window(win.data(), N, &w));
  auto tok = parse_token(p.target->window_token(w, 0, N));
  ASSERT_TRUE(tok);
  EXPECT_EQ(tok->provider, p.writer->provider());

  // two source buffers, written to the window with their halves swapped
  iovec iov[2] = {{src.data(), N / 3}, {src.data() + N / 3, N - N / 3}};
  std::vector<Endpoint::write_t> ws = {{0, N / 2, N / 2}, {N / 2, N / 2, 0}};
  ASSERT_EQ(0, p.writer->write(*tok, iov, 2, ws, BUDGET)) << p.writer->last_error();
  p.target->sync();
  EXPECT_EQ(0, memcmp(win.data(), src.data() + N / 2, N / 2));
  EXPECT_EQ(0, memcmp(win.data() + N / 2, src.data(), N / 2));

  // a token for part of the window addresses from that part's start
  auto sub = parse_token(p.target->window_token(w, 4096, 8192));
  ASSERT_TRUE(sub);
  std::vector<Endpoint::write_t> one = {{0, 100, 8000}};
  ASSERT_EQ(0, p.writer->write(*sub, iov, 1, one, BUDGET));
  p.target->sync();
  EXPECT_EQ(0, memcmp(win.data() + 4096 + 8000, src.data(), 100));

  // writes that leave the window or the source are refused up front
  std::vector<Endpoint::write_t> past_window = {{0, 100, 8100}};
  EXPECT_EQ(-ERANGE, p.writer->write(*sub, iov, 1, past_window, BUDGET));
  std::vector<Endpoint::write_t> past_source = {{N / 3 - 10, 20, 0}};
  EXPECT_EQ(-ERANGE, p.writer->write(*tok, iov, 1, past_source, BUDGET));

  // another provider's token is not ours to serve
  auto other = *tok;
  other.provider = "nosuchprov";
  EXPECT_EQ(-EPROTONOSUPPORT, p.writer->write(other, iov, 1, one, BUDGET));

  // a source larger than a staging buffer is refused
  std::vector<char> big(5 << 20);
  iovec bigv = {big.data(), big.size()};
  std::vector<Endpoint::write_t> bw = {{0, 10, 0}};
  EXPECT_EQ(-E2BIG, p.writer->write(*tok, &bigv, 1, bw, BUDGET));

  // an endpoint that only lends windows cannot write
  EXPECT_EQ(-EOPNOTSUPP, p.target->write(*tok, iov, 1, one, BUDGET));

  const auto s = p.writer->stats();
  EXPECT_EQ(s.writes_failed, 0u);
  EXPECT_EQ(s.peers_inserted, 1u);
  EXPECT_EQ(s.bytes_written, N + N / 3);
}

TEST(OfiWriteCutOff, Tcp)
{
  // a window owner that never polls: over tcp, a write into it cannot
  // complete, so the writer must cut it off at its budget and still be
  // usable afterwards
  config_t c;
  c.provider = "tcp";
  c.node = "127.0.0.1";
  std::string err;
  auto stalled = Endpoint::open(c, &err);
  config_t tc = c;
  tc.progress_thread = true;
  auto healthy = Endpoint::open(tc, &err);
  config_t wc = c;
  wc.stage_size = 8 << 20;
  wc.stage_count = 2;
  auto writer = Endpoint::open(wc, &err);
  if (!stalled || !healthy || !writer) {
    GTEST_SKIP() << "tcp: " << err;
  }
  const size_t N = 8 << 20;
  std::vector<char> w1(N), w2(N), src(N, 'x');
  Endpoint::window_t a, b;
  ASSERT_EQ(0, stalled->register_window(w1.data(), N, &a));
  ASSERT_EQ(0, healthy->register_window(w2.data(), N, &b));
  auto ta = parse_token(stalled->window_token(a, 0, N));
  auto tb = parse_token(healthy->window_token(b, 0, N));
  ASSERT_TRUE(ta && tb);
  iovec iov{src.data(), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};

  // connect: the stalled owner polls only while this warm-up runs
  {
    std::atomic<bool> stop{false};
    std::thread poller([&] { while (!stop) stalled->progress(); });
    std::vector<char> z(4096, 0);
    iovec zv{z.data(), z.size()};
    std::vector<Endpoint::write_t> one = {{0, z.size(), 0}};
    EXPECT_EQ(0, writer->write(*ta, &zv, 1, one, BUDGET));
    stop = true;
    poller.join();
  }

  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(-ETIMEDOUT, writer->write(*ta, &iov, 1, all,
				      std::chrono::milliseconds(1000)));
  const auto took = std::chrono::steady_clock::now() - t0;
  // it gave up within its budget, cut-off included
  EXPECT_LT(took, std::chrono::milliseconds(1000));
  EXPECT_EQ(1u, writer->stats().resets);
  EXPECT_EQ(1u, writer->stats().timeouts);

  // a budget smaller than a cut-off costs is refused before anything
  // is sent
  EXPECT_EQ(-ETIMEDOUT, writer->write(*tb, &iov, 1, all,
				      std::chrono::milliseconds(1)));

  // the reopened endpoint writes again
  ASSERT_EQ(0, writer->write(*tb, &iov, 1, all, BUDGET))
    << writer->last_error();
  healthy->sync();
  EXPECT_EQ(0, memcmp(w2.data(), src.data(), N));
}

namespace {

using clk = std::chrono::steady_clock;
using ms = std::chrono::milliseconds;

/// An insert hook that counts inserts per peer and can hold one peer's
/// insert, as a provider that resolves the peer slowly would, until the
/// test releases it.
struct gate_t {
  std::mutex m;
  std::condition_variable cv;
  std::string hold;
  bool hold_all = false;
  bool open = false;
  std::map<std::string, int> calls;
  clk::time_point held_at{};

  void enter(const std::string& name) {
    std::unique_lock l(m);
    calls[name]++;
    total++;
    cv.notify_all();
    if (hold_all || name == hold) {
      held_at = clk::now();
      cv.wait(l, [this] { return open; });
    }
  }
  /// wait until the held peer's insert has started
  bool wait_held(ms timeout) {
    std::unique_lock l(m);
    return cv.wait_for(l, timeout, [this] { return calls[hold] > 0; });
  }
  void release() {
    {
      std::lock_guard l(m);
      open = true;
    }
    cv.notify_all();
  }
  int count(const std::string& name) {
    std::lock_guard l(m);
    return calls[name];
  }
  /// wait until n inserts have started
  bool wait_total(int n, ms timeout) {
    std::unique_lock l(m);
    return cv.wait_for(l, timeout, [&] { return total >= n; });
  }
  int total = 0;
};

/// wait until the endpoint has no insert queued or running
bool settled(const Endpoint& ep, ms timeout)
{
  const auto until = clk::now() + timeout;
  while (ep.quiet() || ep.stats().pending_inserts) {
    if (clk::now() >= until) {
      return false;
    }
    std::this_thread::sleep_for(ms(1));
  }
  return true;
}

std::unique_ptr<Endpoint> open_target(const std::string& prov,
				      const std::string& node,
				      bool progress = true)
{
  config_t c;
  c.provider = prov;
  c.node = node;
  c.progress_thread = progress;
  std::string err;
  auto ep = Endpoint::open(c, &err);
  if (!ep) {
    std::cerr << prov << ": " << err << std::endl;
  }
  return ep;
}

std::unique_ptr<Endpoint> open_writer(const std::string& prov,
				      const std::string& node,
				      bool thread_safe, size_t stage_size,
				      size_t stage_count,
				      std::shared_ptr<gate_t> gate)
{
  config_t c;
  c.provider = prov;
  c.node = node;
  c.thread_safe = thread_safe;
  c.stage_size = stage_size;
  c.stage_count = stage_count;
  c.insert_hook = [gate](const std::string& name) { gate->enter(name); };
  std::string err;
  auto ep = Endpoint::open(c, &err);
  if (!ep) {
    std::cerr << prov << ": " << err << std::endl;
  }
  return ep;
}

struct window_owner_t {
  // the memory first, so that it outlives the endpoint lending it
  std::vector<char> mem;
  std::unique_ptr<Endpoint> ep;
  token_t tok;
};

bool lend(window_owner_t& o, size_t n)
{
  o.mem.assign(n, 0);
  Endpoint::window_t w;
  if (o.ep->register_window(o.mem.data(), n, &w) != 0) {
    return false;
  }
  auto t = parse_token(o.ep->window_token(w, 0, n));
  if (!t) {
    return false;
  }
  o.tok = *t;
  return true;
}

class OfiPeerInsert : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};

} // anonymous namespace

TEST_P(OfiPeerInsert, SlowInsertHoldsUpNoOtherPeer)
{
  // the first write to peer A waits in a slow address insert; writes to
  // peer B, already known, must go on meanwhile, and further writes to A
  // must share A's insert and give up at their own budgets
  auto [prov, node] = GetParam();
  auto gate = std::make_shared<gate_t>();
  window_owner_t a, b;
  a.ep = open_target(prov, node);
  b.ep = open_target(prov, node);
  auto writer = open_writer(prov, node, true, 1 << 20, 4, gate);
  if (!a.ep || !b.ep || !writer) {
    GTEST_SKIP() << prov << " is not available";
  }
  if (writer->describe().find("concurrent peer inserts") == std::string::npos) {
    GTEST_SKIP() << prov << " offers no thread-safe domain: "
		 << writer->describe();
  }
  const size_t N = 1 << 20;
  ASSERT_TRUE(lend(a, N));
  ASSERT_TRUE(lend(b, N));
  gate->hold = a.tok.name;
  std::vector<char> src(N);
  for (size_t i = 0; i < N; i++) {
    src[i] = static_cast<char>(i * 13 + 5);
  }
  iovec iov{src.data(), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};

  // B becomes a known peer
  ASSERT_EQ(0, writer->write(b.tok, &iov, 1, all, BUDGET)) << writer->last_error();

  // the first write to A, held in its insert
  std::atomic<int> ra{1};
  std::thread ta([&] { ra = writer->write(a.tok, &iov, 1, all, ms(20000)); });
  ASSERT_TRUE(gate->wait_held(ms(5000)));

  // B is not held up
  auto t0 = clk::now();
  ASSERT_EQ(0, writer->write(b.tok, &iov, 1, all, BUDGET)) << writer->last_error();
  EXPECT_LT(clk::now() - t0, ms(1000));

  // another write to A waits for the same insert, only within its budget
  t0 = clk::now();
  EXPECT_EQ(-ETIMEDOUT, writer->write(a.tok, &iov, 1, all, ms(400)));
  const auto waited = clk::now() - t0;
  EXPECT_GE(waited, ms(200));
  EXPECT_LT(waited, ms(1000));
  EXPECT_EQ(1u, writer->stats().peer_timeouts);
  EXPECT_EQ(0u, writer->stats().resets);  // nothing was sent, nothing to cut off

  // the insert finishes, and the first write to A with it
  gate->release();
  ta.join();
  EXPECT_EQ(0, ra.load()) << writer->last_error();
  a.ep->sync();
  EXPECT_EQ(0, memcmp(a.mem.data(), src.data(), N));
  EXPECT_EQ(1, gate->count(a.tok.name));
  EXPECT_EQ(1, gate->count(b.tok.name));
  EXPECT_EQ(2u, writer->stats().peers_inserted);

  // no staging buffer leaked: every one of them still serves a write
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(0, writer->write(a.tok, &iov, 1, all, BUDGET)) << writer->last_error();
  }
  EXPECT_EQ(0u, writer->stats().staging_busy);
}

TEST_P(OfiPeerInsert, CoalescesFirstContact)
{
  // several writes reach a new peer at once: one insert serves them all
  auto [prov, node] = GetParam();
  auto gate = std::make_shared<gate_t>();
  window_owner_t a;
  a.ep = open_target(prov, node);
  auto writer = open_writer(prov, node, true, 1 << 20, 4, gate);
  if (!a.ep || !writer) {
    GTEST_SKIP() << prov << " is not available";
  }
  const size_t N = 4 << 20;
  ASSERT_TRUE(lend(a, N));
  gate->hold = a.tok.name;
  std::vector<char> src(N);
  for (size_t i = 0; i < N; i++) {
    src[i] = static_cast<char>(i * 31 + 1);
  }
  std::vector<std::thread> ts;
  std::atomic<int> failed{0};
  for (int i = 0; i < 4; i++) {
    ts.emplace_back([&, i] {
      const size_t q = N / 4;
      iovec iov{src.data() + i * q, q};
      std::vector<Endpoint::write_t> one = {{0, q, i * q}};
      if (writer->write(a.tok, &iov, 1, one, ms(20000)) != 0) {
	failed++;
      }
    });
  }
  ASSERT_TRUE(gate->wait_held(ms(5000)));
  std::this_thread::sleep_for(ms(200));  // let every writer reach the wait
  gate->release();
  for (auto& t : ts) {
    t.join();
  }
  EXPECT_EQ(0, failed.load()) << writer->last_error();
  a.ep->sync();
  EXPECT_EQ(0, memcmp(a.mem.data(), src.data(), N));
  EXPECT_EQ(1, gate->count(a.tok.name));
  EXPECT_EQ(1u, writer->stats().peers_inserted);
}

TEST(OfiPeerInsertDomain, Tcp)
{
  // FI_THREAD_DOMAIN: an insert stops the endpoint. It must wait for the
  // write in flight to be cut off at its budget, and writes that start
  // while it runs, to its own peer or another, must give up after
  // insert_wait, not when it ends.
  auto gate = std::make_shared<gate_t>();
  window_owner_t a, b, s;
  a.ep = open_target("tcp", "127.0.0.1");
  b.ep = open_target("tcp", "127.0.0.1");
  s.ep = open_target("tcp", "127.0.0.1", false);  // never polls
  auto writer = open_writer("tcp", "127.0.0.1", false, 8 << 20, 2, gate);
  if (!a.ep || !b.ep || !s.ep || !writer) {
    GTEST_SKIP() << "tcp is not available";
  }
  EXPECT_NE(writer->describe().find("peer inserts pause writes"),
	    std::string::npos) << writer->describe();
  const size_t N = 8 << 20;
  ASSERT_TRUE(lend(a, N));
  ASSERT_TRUE(lend(b, N));
  ASSERT_TRUE(lend(s, N));
  gate->hold = a.tok.name;
  std::vector<char> src(N, 'y');
  iovec iov{src.data(), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};
  std::vector<char> z(4096, 0);
  iovec zv{z.data(), z.size()};
  std::vector<Endpoint::write_t> one = {{0, z.size(), 0}};

  // B known; S known and connected while it polls, then left stalled
  ASSERT_EQ(0, writer->write(b.tok, &zv, 1, one, BUDGET)) << writer->last_error();
  {
    std::atomic<bool> stop{false};
    std::thread poller([&] { while (!stop) s.ep->progress(); });
    EXPECT_EQ(0, writer->write(s.tok, &zv, 1, one, BUDGET)) << writer->last_error();
    stop = true;
    poller.join();
  }

  // a write into S that cannot complete: it is in flight until cut off
  std::atomic<int> rs{1};
  clk::time_point s_end;
  const auto s_start = clk::now();
  std::thread tsw([&] {
    rs = writer->write(s.tok, &iov, 1, all, ms(1000));
    s_end = clk::now();
  });
  std::this_thread::sleep_for(ms(100));

  // first contact with A while it is in flight: the insert waits for it,
  // and the write to A gives up on the insert after insert_wait, having
  // sent nothing, instead of waiting for it within its budget
  std::atomic<int> ra{1};
  std::atomic<bool> pa{true};
  clk::duration a_took{};
  std::thread ta([&] {
    bool posted = true;
    const auto t = clk::now();
    ra = writer->write(a.tok, &zv, 1, one, ms(20000), &posted);
    a_took = clk::now() - t;
    pa = posted;
  });
  ASSERT_TRUE(gate->wait_held(ms(5000)));
  ta.join();
  EXPECT_EQ(-EBUSY, ra.load());
  EXPECT_FALSE(pa.load());
  EXPECT_LT(a_took, ms(250));
  tsw.join();
  EXPECT_EQ(-ETIMEDOUT, rs.load());
  // cut off within its budget: the insert did not delay it
  EXPECT_LT(s_end - s_start, ms(1000));
  {
    // the insert waited for it: it began at the cut-off, near the end of
    // the budget, not when A's write arrived 100 ms in
    std::lock_guard l(gate->m);
    EXPECT_GE(gate->held_at - s_start, ms(700));
  }

  // while the insert holds the endpoint, a write to B gives up after
  // insert_wait, having sent nothing, instead of waiting for the insert
  // or for its budget
  const auto t0 = clk::now();
  bool posted = true;
  EXPECT_EQ(-EBUSY, writer->write(b.tok, &zv, 1, one, ms(2000), &posted));
  EXPECT_LT(clk::now() - t0, ms(250));
  EXPECT_FALSE(posted);
  EXPECT_EQ(2u, writer->stats().insert_busy);  // A's and B's
  EXPECT_EQ(0u, writer->stats().peer_timeouts);

  // the insert goes on, and A takes writes once it is done
  gate->release();
  ASSERT_TRUE(settled(*writer, ms(5000)));
  EXPECT_EQ(0, writer->write(a.tok, &zv, 1, one, BUDGET)) << writer->last_error();
  EXPECT_EQ(0, writer->write(b.tok, &iov, 1, all, BUDGET)) << writer->last_error();
  b.ep->sync();
  EXPECT_EQ(0, memcmp(b.mem.data(), src.data(), N));
  EXPECT_EQ(1, gate->count(a.tok.name));
}

TEST(OfiPeerInsertDomain, OpThreadsDoNotWait)
{
  // FI_THREAD_DOMAIN, on an endpoint that writes and lends gather windows,
  // as an OSD's does. While an insert holds it, as a slow provider's does
  // for seconds, an op thread that lends or returns a window, reads one,
  // or writes to the new peer or a known one must not wait for the
  // insert longer than insert_wait, counted from when the endpoint went
  // quiet: once the insert has held it that long, each call goes on
  // without the endpoint at once. Once the insert is done, the endpoint
  // and the pool are whole again, each acquire re-keying one window
  // whose re-key the insert put off.
  auto gate = std::make_shared<gate_t>();
  window_owner_t a, b;
  a.ep = open_target("tcp", "127.0.0.1");
  b.ep = open_target("tcp", "127.0.0.1");
  const size_t W = 64 << 10;
  // three gather windows, and a window outside the pool; the memory
  // first, so that it outlives the endpoint lending it
  std::vector<char> mem(4 * W, 0);
  config_t c;
  c.provider = "tcp";
  c.node = "127.0.0.1";
  c.thread_safe = false;
  c.stage_size = W;
  c.stage_count = 2;
  c.progress_thread = true;
  c.insert_wait = ms(300);
  c.insert_hook = [gate](const std::string& name) { gate->enter(name); };
  std::string err;
  auto osd = Endpoint::open(c, &err);
  if (!a.ep || !b.ep || !osd) {
    GTEST_SKIP() << "tcp is not available: " << err;
  }
  ASSERT_NE(osd->describe().find("peer inserts pause writes"),
	    std::string::npos) << osd->describe();
  ASSERT_TRUE(lend(a, W));
  ASSERT_TRUE(lend(b, W));
  auto pool = WindowPool::create(*osd, mem.data(), W, 3, &err);
  ASSERT_TRUE(pool) << err;
  Endpoint::window_t w;
  ASSERT_EQ(0, osd->register_window(mem.data() + 3 * W, W, &w));
  std::vector<char> src(W, 'q');
  iovec iov{src.data(), W};
  std::vector<Endpoint::write_t> all = {{0, W, 0}};

  // B known; two gathers in flight, their windows lent before the insert
  ASSERT_EQ(0, osd->write(b.tok, &iov, 1, all, BUDGET)) << osd->last_error();
  auto lent = pool->acquire(W);
  auto lent2 = pool->acquire(W);
  ASSERT_TRUE(lent && lent2);
  const auto k1 = parse_token(lent->token)->key;
  const auto k2 = parse_token(lent2->token)->key;

  // first contact with A: its insert holds the endpoint until released.
  // Should a call below wait for it after all, the insert is let go in
  // the end, and the test fails instead of hanging.
  gate->hold = a.tok.name;
  std::thread backstop([gate] {
    {
      std::unique_lock l(gate->m);
      gate->cv.wait_for(l, ms(5000), [&] { return gate->open; });
    }
    gate->release();
  });
  // the write to A waits for its own insert no longer than insert_wait,
  // and sends nothing
  bool posted = true;
  auto t0 = clk::now();
  EXPECT_EQ(-EBUSY, osd->write(a.tok, &iov, 1, all, ms(20000), &posted));
  EXPECT_LT(clk::now() - t0, c.insert_wait + ms(250));
  EXPECT_FALSE(posted);
  EXPECT_TRUE(gate->wait_held(ms(5000)));
  EXPECT_TRUE(osd->quiet());
  {
    // the endpoint has been quiet for insert_wait at least
    std::unique_lock l(gate->m);
    const auto held_at = gate->held_at;
    l.unlock();
    std::this_thread::sleep_until(held_at + c.insert_wait);
  }

  // what an op thread does meanwhile; none of it waits for the insert,
  // nor pays insert_wait again
  const auto brief = c.insert_wait / 2;
  t0 = clk::now();
  // no window, though one is free: the shard replies inline
  EXPECT_FALSE(pool->acquire(W));
  EXPECT_LT(clk::now() - t0, brief);
  for (auto* l : {&lent, &lent2}) {
    t0 = clk::now();
    pool->release((*l)->id, ms(0), true);  // its re-key put off
    EXPECT_LT(clk::now() - t0, brief);
  }
  t0 = clk::now();
  osd->sync();
  EXPECT_LT(clk::now() - t0, brief);
  t0 = clk::now();
  EXPECT_TRUE(osd->window_token(w, 0, W).empty());
  EXPECT_LT(clk::now() - t0, brief);
  t0 = clk::now();
  EXPECT_EQ(-EINPROGRESS, osd->rekey_window(w));
  EXPECT_LT(clk::now() - t0, brief);
  for (auto* peer : {&b.tok, &a.tok}) {
    t0 = clk::now();
    posted = true;
    EXPECT_EQ(-EBUSY, osd->write(*peer, &iov, 1, all, BUDGET, &posted));
    EXPECT_LT(clk::now() - t0, brief);
    EXPECT_FALSE(posted);
  }
  auto ps = pool->stats();
  EXPECT_EQ(1u, ps.declined_insert);
  EXPECT_EQ(2u, ps.rekeys_put_off);
  EXPECT_EQ(0u, ps.rekeyed);
  // A's writes, the sync, the token, the re-key and B's write; the pool
  // did not call into the endpoint
  EXPECT_EQ(6u, osd->stats().insert_busy);
  EXPECT_EQ(0u, osd->stats().peer_timeouts);

  gate->release();
  backstop.join();
  ASSERT_TRUE(settled(*osd, ms(5000)));

  // each acquire does one re-key that was put off, and a window is lent
  // only once it has its new key
  auto again = pool->acquire(W);
  ASSERT_TRUE(again);
  EXPECT_EQ(lent->id, again->id);
  EXPECT_NE(k1, parse_token(again->token)->key);
  ps = pool->stats();
  EXPECT_EQ(1u, ps.rekeyed);
  EXPECT_EQ(0u, ps.rekey_failed);
  EXPECT_EQ(3u, ps.acquired);
  auto again2 = pool->acquire(W);
  ASSERT_TRUE(again2);
  EXPECT_EQ(lent2->id, again2->id);
  EXPECT_NE(k2, parse_token(again2->token)->key);
  ps = pool->stats();
  EXPECT_EQ(2u, ps.rekeyed);
  EXPECT_EQ(4u, ps.acquired);
  pool->release(again->id, ms(0), true);
  pool->release(again2->id, ms(0), true);
  EXPECT_EQ(4u, pool->stats().rekeyed);

  // and the rest works again, A included
  EXPECT_FALSE(osd->window_token(w, 0, W).empty());
  EXPECT_EQ(0, osd->rekey_window(w)) << osd->last_error();
  EXPECT_EQ(0, osd->write(a.tok, &iov, 1, all, BUDGET)) << osd->last_error();
  EXPECT_EQ(0, osd->write(b.tok, &iov, 1, all, BUDGET)) << osd->last_error();
  a.ep->sync();
  b.ep->sync();
  EXPECT_EQ(0, memcmp(a.mem.data(), src.data(), W));
  EXPECT_EQ(0, memcmp(b.mem.data(), src.data(), W));
  EXPECT_EQ(6u, osd->stats().insert_busy);
  EXPECT_EQ(1, gate->count(a.tok.name));
}

TEST(OfiPeerInsertDomain, FirstContactsDoNotQueue)
{
  // FI_THREAD_DOMAIN: the inserts run one at a time. A first write to a
  // peer waits for an insert no longer than insert_wait: not for its own
  // peer's, and not for another's queued ahead of it, whose turn comes
  // first. The inserts go on, and later writes find both peers ready.
  auto gate = std::make_shared<gate_t>();
  window_owner_t a, b;
  a.ep = open_target("tcp", "127.0.0.1");
  b.ep = open_target("tcp", "127.0.0.1");
  auto writer = open_writer("tcp", "127.0.0.1", false, 1 << 20, 4, gate);
  if (!a.ep || !b.ep || !writer) {
    GTEST_SKIP() << "tcp is not available";
  }
  const size_t N = 1 << 20;
  ASSERT_TRUE(lend(a, N));
  ASSERT_TRUE(lend(b, N));
  std::vector<char> src(N, 'f');
  iovec iov{src.data(), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};
  gate->hold = a.tok.name;
  std::thread backstop([gate] {
    {
      std::unique_lock l(gate->m);
      gate->cv.wait_for(l, ms(5000), [&] { return gate->open; });
    }
    gate->release();
  });

  // A's insert starts, and is held
  bool posted = true;
  auto t0 = clk::now();
  EXPECT_EQ(-EBUSY, writer->write(a.tok, &iov, 1, all, ms(20000), &posted));
  EXPECT_LT(clk::now() - t0, ms(250));
  EXPECT_FALSE(posted);
  EXPECT_TRUE(gate->wait_held(ms(5000)));

  // B's insert is queued behind it
  posted = true;
  t0 = clk::now();
  EXPECT_EQ(-EBUSY, writer->write(b.tok, &iov, 1, all, ms(20000), &posted));
  EXPECT_LT(clk::now() - t0, ms(250));
  EXPECT_FALSE(posted);
  EXPECT_EQ(0, gate->count(b.tok.name));
  auto st = writer->stats();
  EXPECT_EQ(2u, st.insert_busy);
  EXPECT_EQ(0u, st.peer_timeouts);
  EXPECT_EQ(2u, st.pending_inserts);

  gate->release();
  backstop.join();
  ASSERT_TRUE(settled(*writer, ms(5000)));
  EXPECT_EQ(0, writer->write(a.tok, &iov, 1, all, BUDGET)) << writer->last_error();
  EXPECT_EQ(0, writer->write(b.tok, &iov, 1, all, BUDGET)) << writer->last_error();
  a.ep->sync();
  b.ep->sync();
  EXPECT_EQ(0, memcmp(a.mem.data(), src.data(), N));
  EXPECT_EQ(0, memcmp(b.mem.data(), src.data(), N));
  EXPECT_EQ(1, gate->count(a.tok.name));
  EXPECT_EQ(1, gate->count(b.tok.name));
  EXPECT_EQ(2u, writer->stats().peers_inserted);
  EXPECT_EQ(2u, writer->stats().insert_busy);
}

namespace {

/// CPU time this process used so far, user and system
std::chrono::duration<double> cpu_used()
{
  rusage r;
  getrusage(RUSAGE_SELF, &r);
  return std::chrono::duration<double>(
    r.ru_utime.tv_sec + r.ru_stime.tv_sec +
    (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6);
}

class OfiConcurrentWrites : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};

} // anonymous namespace

TEST_P(OfiConcurrentWrites, AllComplete)
{
  // many writers share the endpoint: every write completes, whichever of
  // them polls, and every byte lands where it should
  auto [prov, node] = GetParam();
  auto gate = std::make_shared<gate_t>();
  window_owner_t a;
  a.ep = open_target(prov, node);
  const int T = 16, K = 20;
  const size_t S = 256 << 10;
  auto writer = open_writer(prov, node, true, S, T, gate);
  if (!a.ep || !writer) {
    GTEST_SKIP() << prov << " is not available";
  }
  ASSERT_TRUE(lend(a, T * S));
  std::vector<std::vector<char>> src(T, std::vector<char>(S));
  for (int t = 0; t < T; t++) {
    for (size_t i = 0; i < S; i++) {
      src[t][i] = static_cast<char>(i * 3 + t * 101);
    }
  }
  std::atomic<int> failed{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < T; t++) {
    ts.emplace_back([&, t] {
      iovec iov{src[t].data(), S};
      std::vector<Endpoint::write_t> one = {{0, S, t * S}};
      for (int k = 0; k < K; k++) {
	if (writer->write(a.tok, &iov, 1, one, BUDGET) != 0) {
	  failed++;
	}
      }
    });
  }
  for (auto& t : ts) {
    t.join();
  }
  EXPECT_EQ(0, failed.load()) << writer->last_error();
  a.ep->sync();
  for (int t = 0; t < T; t++) {
    EXPECT_EQ(0, memcmp(a.mem.data() + t * S, src[t].data(), S)) << "writer " << t;
  }
  const auto st = writer->stats();
  EXPECT_EQ(uint64_t(T * K), st.writes_posted);
  EXPECT_EQ(0u, st.resets);
  EXPECT_EQ(0u, st.staging_busy);
}

TEST(OfiConcurrentWritesStalled, Tcp)
{
  // Writes into a window owner that stopped polling cannot complete.
  // While they wait, they must not burn CPU: one of them polls, the
  // others sleep. The first to reach its deadline cuts them all off at
  // once, and the others hear of it then, not at their own deadlines.
  auto gate = std::make_shared<gate_t>();
  window_owner_t s;
  s.ep = open_target("tcp", "127.0.0.1", false);
  const int T = 8;
  const size_t S = 4 << 20;
  auto writer = open_writer("tcp", "127.0.0.1", true, S, T, gate);
  if (!s.ep || !writer) {
    GTEST_SKIP() << "tcp is not available";
  }
  ASSERT_TRUE(lend(s, T * S));
  std::vector<char> src(S, 'z');
  iovec iov{src.data(), S};
  {
    // connect while the owner polls
    std::atomic<bool> stop{false};
    std::thread poller([&] { while (!stop) s.ep->progress(); });
    std::vector<Endpoint::write_t> one = {{0, 4096, 0}};
    EXPECT_EQ(0, writer->write(s.tok, &iov, 1, one, BUDGET)) << writer->last_error();
    stop = true;
    poller.join();
  }
  const auto resets0 = writer->stats().resets;
  std::vector<int> rc(T, 1);
  std::vector<std::chrono::steady_clock::duration> took(T);
  const auto cpu0 = cpu_used();
  const auto t0 = clk::now();
  std::vector<std::thread> ts;
  for (int t = 0; t < T; t++) {
    ts.emplace_back([&, t] {
      std::vector<Endpoint::write_t> one = {{0, S, t * S}};
      // the first writer has the shortest budget
      const ms budget(t == 0 ? 800 : 2000);
      const auto a = clk::now();
      rc[t] = writer->write(s.tok, &iov, 1, one, budget);
      took[t] = clk::now() - a;
    });
    if (t == 0) {
      std::this_thread::sleep_for(ms(20));  // it posts first
    }
  }
  for (auto& t : ts) {
    t.join();
  }
  const auto wall = std::chrono::duration<double>(clk::now() - t0);
  const auto cpu = cpu_used() - cpu0;
  EXPECT_EQ(-ETIMEDOUT, rc[0]);
  EXPECT_LT(took[0], ms(800));    // within its budget, cut-off included
  EXPECT_GT(took[0], ms(400));    // and not much before it
  for (int t = 1; t < T; t++) {
    // cut off with the first, long before their own deadlines
    EXPECT_TRUE(rc[t] == -ECANCELED || rc[t] == -ETIMEDOUT) << rc[t];
    EXPECT_LT(took[t], ms(1200)) << "writer " << t;
  }
  EXPECT_EQ(resets0 + 1, writer->stats().resets);
  // eight writes waited for most of a second: well under one core between
  // them (spinning, they used about six)
  EXPECT_LT(cpu.count(), 0.5 * wall.count())
    << "cpu " << cpu.count() << " s over " << wall.count() << " s";
}

namespace {

/// A window owner that stopped polling after the writer connected, and
/// a writer whose cut-offs go through hook: writes into the owner cannot
/// complete over tcp, so each one ends in a cut-off.
struct stalled_t {
  window_owner_t s;
  std::unique_ptr<Endpoint> writer;
  static constexpr size_t S = 4 << 20;
  std::vector<char> src = std::vector<char>(S, 'k');

  bool open(std::function<int()> hook,
	    std::function<void(config_t&)> tweak = nullptr) {
    s.ep = open_target("tcp", "127.0.0.1", false);
    config_t c;
    c.provider = "tcp";
    c.node = "127.0.0.1";
    c.stage_size = S;
    c.stage_count = 4;
    c.cutoff_close_hook = std::move(hook);
    if (tweak) {
      tweak(c);
    }
    std::string err;
    writer = Endpoint::open(c, &err);
    if (!s.ep || !writer || !lend(s, 4 * S)) {
      return false;
    }
    return connect();
  }
  /// connect while the owner polls; a cut-off reopens the writer's
  /// endpoint, which then has to connect again
  bool connect() {
    std::atomic<bool> stop{false};
    std::thread poller([&] { while (!stop) s.ep->progress(); });
    iovec iov{src.data(), 4096};
    std::vector<Endpoint::write_t> one = {{0, 4096, 0}};
    const int r = writer->write(s.tok, &iov, 1, one, BUDGET);
    stop = true;
    poller.join();
    return r == 0;
  }
  /// write slot i of the owner's window with the given budget
  int write(int i, ms budget) {
    iovec iov{src.data(), S};
    std::vector<Endpoint::write_t> one = {{0, S, i * S}};
    return writer->write(s.tok, &iov, 1, one, budget);
  }
};

} // anonymous namespace

namespace {

/// A late write into a stalled owner (slot 0, 600 ms) next to a long one
/// (slot 1, 3 s), with writes to a healthy owner going on throughout.
struct neighbors_t {
  int r_late = 1, r_long = 1;
  clk::duration took_late{}, took_long{};
  int healthy_ok = 0, healthy_failed = 0;
  int healthy_first_rc = 0;

  void run(stalled_t& t, window_owner_t& h) {
    std::atomic<bool> stop{false};
    std::thread healthy([&] {
      iovec iov{t.src.data(), 64 << 10};
      std::vector<Endpoint::write_t> one = {{0, 64 << 10, 0}};
      while (!stop) {
	const int r = t.writer->write(h.tok, &iov, 1, one, BUDGET);
	if (r == 0) {
	  healthy_ok++;
	} else {
	  if (!healthy_failed++) {
	    healthy_first_rc = r;
	  }
	}
	std::this_thread::sleep_for(ms(5));
      }
    });
    std::thread long_one([&] {
      const auto a = clk::now();
      r_long = t.write(1, ms(3000));
      took_long = clk::now() - a;
    });
    std::this_thread::sleep_for(ms(20));
    const auto a = clk::now();
    r_late = t.write(0, ms(600));
    took_late = clk::now() - a;
    long_one.join();
    stop = true;
    healthy.join();
  }
};

} // anonymous namespace

TEST(OfiPerPlanCutOff, OthersKeepGoing)
{
  // A provider that discards a cancelled write: a late write is cut off
  // alone. The long write next to it runs to its own deadline, writes to
  // a healthy owner never fail, and the endpoint is never reset.
  stalled_t t;
  if (!t.open(nullptr, [](config_t& c) {
	c.stage_count = 8;
	c.cancel_hook = [] { return 0; };
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  EXPECT_TRUE(t.writer->stats().cancel_discards);
  EXPECT_NE(std::string::npos,
	    t.writer->describe().find("late writes cancelled one by one"));
  window_owner_t h;
  h.ep = open_target("tcp", "127.0.0.1");
  ASSERT_TRUE(h.ep && lend(h, 64 << 10));
  neighbors_t n;
  n.run(t, h);
  EXPECT_EQ(-ETIMEDOUT, n.r_late);
  EXPECT_LT(n.took_late, ms(600));
  EXPECT_EQ(-ETIMEDOUT, n.r_long);   // its own deadline, not cancelled
  EXPECT_GT(n.took_long, ms(2500));
  EXPECT_EQ(0, n.healthy_failed) << n.healthy_first_rc;
  EXPECT_GT(n.healthy_ok, 20);
  const auto st = t.writer->stats();
  EXPECT_EQ(2u, st.plans_cut_off);
  EXPECT_EQ(0u, st.resets);
  EXPECT_FALSE(st.unsafe);
}

TEST(OfiPerPlanCutOff, WithoutDiscardTheEndpointResets)
{
  // the same, where cancelling would not discard: the late write's
  // cut-off resets the endpoint, which takes the long write with it
  stalled_t t;
  if (!t.open(nullptr, [](config_t& c) { c.stage_count = 8; })) {
    GTEST_SKIP() << "tcp is not available";
  }
  EXPECT_FALSE(t.writer->stats().cancel_discards);
  EXPECT_NE(std::string::npos,
	    t.writer->describe().find("cut off by reopening the endpoint"));
  window_owner_t h;
  h.ep = open_target("tcp", "127.0.0.1");
  ASSERT_TRUE(h.ep && lend(h, 64 << 10));
  neighbors_t n;
  n.run(t, h);
  EXPECT_EQ(-ETIMEDOUT, n.r_late);
  EXPECT_EQ(-ECANCELED, n.r_long);
  EXPECT_LT(n.took_long, ms(1500));
  const auto st = t.writer->stats();
  EXPECT_EQ(0u, st.plans_cut_off);
  EXPECT_EQ(1u, st.resets);
}

TEST(OfiPerPlanCutOff, FailedCancelResets)
{
  // a cancel that fails falls back to closing the endpoint, which
  // discards every write
  stalled_t t;
  if (!t.open(nullptr, [](config_t& c) {
	c.stage_count = 8;
	c.cancel_hook = [] { return -EIO; };
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  window_owner_t h;
  h.ep = open_target("tcp", "127.0.0.1");
  ASSERT_TRUE(h.ep && lend(h, 64 << 10));
  neighbors_t n;
  n.run(t, h);
  EXPECT_EQ(-ETIMEDOUT, n.r_late);
  EXPECT_EQ(-ECANCELED, n.r_long);
  const auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cancels_failed);
  EXPECT_EQ(1u, st.resets);
  EXPECT_EQ(0u, st.plans_cut_off);
  EXPECT_FALSE(st.unsafe);
}

TEST(OfiPerPlanCutOff, FailedCancelAndCloseIsUnsafe)
{
  // neither the cancel nor the close discards: the writes may still land
  stalled_t t;
  if (!t.open([] { return -EBUSY; }, [](config_t& c) {
	c.cancel_hook = [] { return -EIO; };
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  EXPECT_EQ(-ENOTRECOVERABLE, t.write(0, ms(600)));
  const auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cancels_failed);
  EXPECT_EQ(1u, st.cutoffs_failed);
  EXPECT_TRUE(st.unsafe);
}

TEST(OfiPerPlanCutOff, LateCancelIsUnsafe)
{
  // a cancel that returns after the write's budget ended may have let
  // bytes land late
  stalled_t t;
  if (!t.open(nullptr, [](config_t& c) {
	c.cancel_hook = [] { std::this_thread::sleep_for(ms(400)); return 0; };
	c.late_tolerance = ms(200);
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  EXPECT_EQ(-ENOTRECOVERABLE, t.write(0, ms(600)));
  const auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cutoffs_late);
  EXPECT_TRUE(st.unsafe);
  EXPECT_EQ(-EIO, t.write(1, ms(2000)));
}

TEST(OfiPerPlanCutOff, ProvidersThatDoNotSay)
{
  // tcp and shm know neither option: they reset, and do not claim that
  // closing discards
  for (const char* prov : {"tcp", "shm"}) {
    auto gate = std::make_shared<gate_t>();
    auto w = open_writer(prov, std::string(prov) == "tcp" ? "127.0.0.1" : "",
			 true, 4096, 1, gate);
    if (!w) {
      continue;
    }
    const auto st = w->stats();
    EXPECT_FALSE(st.cancel_discards) << prov;
    EXPECT_EQ(-1, st.close_discards) << prov;
  }
}

namespace {

/// a wait hook that, once, on the thread that set mine, sleeps for d as
/// soon as the writer is within 20 ms of its deadline: a writer the
/// scheduler left idle just when it should cut its writes off
struct idle_writer_t {
  std::atomic<bool> slept{false};
  ms d;
  explicit idle_writer_t(ms d) : d(d) {}
  static inline thread_local bool mine = false;
  std::function<void(clk::time_point)> hook() {
    return [this](clk::time_point deadline) {
      if (mine && clk::now() > deadline - ms(20) && !slept.exchange(true)) {
	std::this_thread::sleep_for(d);
      }
    };
  }
};

} // anonymous namespace

TEST(OfiLateCutOff, WithinToleranceIsCounted)
{
  // A lone writer is left idle past its deadline, so its cut-off starts
  // late and ends after its budget, but within the tolerance: the write
  // is cut off as usual, the lateness counted, the endpoint goes on, and
  // the scheduling slack it keeps grows.
  idle_writer_t idle(ms(300));
  stalled_t t;
  if (!t.open(nullptr, [&](config_t& c) { c.wait_hook = idle.hook(); })) {
    GTEST_SKIP() << "tcp is not available";
  }
  const auto slack0 = t.writer->stats().cutoff_slack_ms;
  idle_writer_t::mine = true;
  EXPECT_EQ(-ETIMEDOUT, t.write(0, ms(600)));
  idle_writer_t::mine = false;
  const auto st = t.writer->stats();
  EXPECT_TRUE(idle.slept);
  EXPECT_EQ(1u, st.cutoffs_late);
  EXPECT_GT(st.max_cutoff_lateness_ms, 0u);
  EXPECT_LT(st.max_cutoff_lateness_ms, 1000u);
  EXPECT_GT(st.cutoff_slack_ms, slack0);
  EXPECT_FALSE(st.unsafe) << t.writer->last_error();
  EXPECT_NE(std::string::npos, t.writer->last_error().find("within the tolerance"))
    << t.writer->last_error();
  // nothing else polled meanwhile: the message names the pause
  EXPECT_NE(std::string::npos, t.writer->last_error().find("did not run"))
    << t.writer->last_error();
  EXPECT_EQ(0u, st.cutoffs_past_tolerance);
}

TEST(OfiLateCutOff, PausedPastToleranceGoesOn)
{
  // The only thread of the writer is left idle 1.2 s near its deadline,
  // as a stopped or paused process would be: the cut-off starts long
  // after it, and ends past the 200 ms tolerance, but the close is quick
  // and leaves the endpoint clean. It is counted and named as a pause,
  // and the endpoint goes on.
  idle_writer_t idle(ms(1200));
  stalled_t t;
  if (!t.open(nullptr, [&](config_t& c) {
	c.wait_hook = idle.hook();
	c.late_tolerance = ms(200);
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  idle_writer_t::mine = true;
  EXPECT_EQ(-ETIMEDOUT, t.write(0, ms(600)));
  idle_writer_t::mine = false;
  auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cutoffs_late);
  EXPECT_EQ(1u, st.cutoffs_past_tolerance);
  EXPECT_GE(st.max_poll_gap_ms, 1000u);
  EXPECT_FALSE(st.unsafe);
  const auto e = t.writer->last_error();
  EXPECT_NE(std::string::npos, e.find("beyond the tolerance")) << e;
  EXPECT_NE(std::string::npos, e.find("did not run")) << e;
  EXPECT_NE(std::string::npos, e.find("the endpoint goes on")) << e;
  // and it still cuts writes off, and delivers
  ASSERT_TRUE(t.connect()) << t.writer->last_error();
  EXPECT_EQ(-ETIMEDOUT, t.write(1, ms(600)));
  EXPECT_FALSE(t.writer->unsafe());
}

TEST(OfiLateCutOff, PausedPastToleranceFailClosed)
{
  // the same with late_fail_closed: the endpoint turns unsafe
  idle_writer_t idle(ms(1200));
  stalled_t t;
  if (!t.open(nullptr, [&](config_t& c) {
	c.wait_hook = idle.hook();
	c.late_tolerance = ms(200);
	c.late_fail_closed = true;
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  idle_writer_t::mine = true;
  EXPECT_EQ(-ENOTRECOVERABLE, t.write(0, ms(600)));
  idle_writer_t::mine = false;
  const auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cutoffs_late);
  EXPECT_TRUE(st.unsafe);
}

TEST(OfiLateCutOff, AnIdleWriterIsCutOffOnTime)
{
  // A writer is left idle for 1.5 s just before its deadline, far past
  // its budget. Another writer of the endpoint polls meanwhile, and cuts
  // the idle one's writes off on its behalf, on time: nothing is late.
  idle_writer_t idle(ms(1500));
  stalled_t t;
  if (!t.open(nullptr, [&](config_t& c) {
	c.wait_hook = idle.hook();
	c.stage_count = 8;
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  window_owner_t h;
  h.ep = open_target("tcp", "127.0.0.1");
  ASSERT_TRUE(h.ep && lend(h, 64 << 10));
  std::atomic<bool> stop{false};
  std::thread healthy([&] {
    iovec iov{t.src.data(), 64 << 10};
    std::vector<Endpoint::write_t> one = {{0, 64 << 10, 0}};
    while (!stop) {
      t.writer->write(h.tok, &iov, 1, one, BUDGET);
      std::this_thread::sleep_for(ms(2));
    }
  });
  int r = 1;
  clk::duration took{};
  std::thread idle_one([&] {
    idle_writer_t::mine = true;
    const auto a = clk::now();
    r = t.write(0, ms(800));
    took = clk::now() - a;
  });
  idle_one.join();
  stop = true;
  healthy.join();
  EXPECT_TRUE(idle.slept);
  EXPECT_EQ(-ETIMEDOUT, r);
  EXPECT_GT(took, ms(1500));  // it slept through its budget
  const auto st = t.writer->stats();
  EXPECT_EQ(0u, st.cutoffs_late);
  EXPECT_GE(st.cutoffs_on_behalf, 1u);
  EXPECT_FALSE(st.unsafe) << t.writer->last_error();
}

TEST(OfiLateCutOff, ManyWritersLeftIdle)
{
  // Eight writes into a stalled owner, with deadlines between 0.4 and
  // 1.1 s, each writer left idle once near its deadline for up to 0.4 s,
  // over a provider that cuts writes off one by one. The writers that
  // run cut the idle ones off on their behalf; whatever is late stays
  // within the tolerance, and the endpoint never turns unsafe.
  std::array<std::unique_ptr<idle_writer_t>, 8> idle;
  std::mt19937 rng(42);
  for (auto& i : idle) {
    i = std::make_unique<idle_writer_t>(ms(rng() % 400));
  }
  thread_local int who = -1;
  stalled_t t;
  if (!t.open(nullptr, [&](config_t& c) {
	c.stage_count = 10;
	c.cancel_hook = [] { return 0; };
	c.wait_hook = [&](clk::time_point deadline) {
	  if (who >= 0) {
	    idle_writer_t::mine = true;
	    idle[who]->hook()(deadline);
	  }
	};
      })) {
    GTEST_SKIP() << "tcp is not available";
  }
  t.s.mem.assign(8 * stalled_t::S, 0);
  Endpoint::window_t w;
  ASSERT_EQ(0, t.s.ep->register_window(t.s.mem.data(), t.s.mem.size(), &w));
  t.s.tok = *parse_token(t.s.ep->window_token(w, 0, t.s.mem.size()));
  ASSERT_TRUE(t.connect());
  std::vector<int> rc(8, 1);
  std::vector<std::thread> ts;
  for (int i = 0; i < 8; i++) {
    ts.emplace_back([&, i] {
      who = i;
      rc[i] = t.write(i, ms(400 + 100 * i));
    });
  }
  for (auto& th : ts) {
    th.join();
  }
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(-ETIMEDOUT, rc[i]) << "writer " << i;
  }
  const auto st = t.writer->stats();
  EXPECT_FALSE(st.unsafe) << t.writer->last_error();
  EXPECT_EQ(8u, st.plans_cut_off);
  EXPECT_EQ(0u, st.resets);
  EXPECT_LT(st.max_cutoff_lateness_ms, 1000u);
}

TEST(OfiWritePosted, SaysWhetherAnythingWentOut)
{
  // the caller learns whether any write reached the provider: an OSD
  // tells its client that no transfer started, so the client need not
  // wait out the fence
  stalled_t t;
  if (!t.open(nullptr)) {
    GTEST_SKIP() << "tcp is not available";
  }
  window_owner_t h;
  h.ep = open_target("tcp", "127.0.0.1");
  ASSERT_TRUE(h.ep && lend(h, 4096));
  iovec iov{t.src.data(), 4096};
  std::vector<Endpoint::write_t> one = {{0, 4096, 0}};
  bool posted = true;
  // refused for its budget: nothing went out
  EXPECT_EQ(-ETIMEDOUT, t.writer->write(h.tok, &iov, 1, one, ms(5), &posted));
  EXPECT_FALSE(posted);
  // out of range: nothing went out
  std::vector<Endpoint::write_t> past = {{0, 4096, 8192}};
  posted = true;
  EXPECT_EQ(-ERANGE, t.writer->write(h.tok, &iov, 1, past, BUDGET, &posted));
  EXPECT_FALSE(posted);
  // delivered
  EXPECT_EQ(0, t.writer->write(h.tok, &iov, 1, one, BUDGET, &posted));
  EXPECT_TRUE(posted);
  // posted, then cut off: it went out
  posted = false;
  iovec big{t.src.data(), stalled_t::S};
  std::vector<Endpoint::write_t> all = {{0, stalled_t::S, 0}};
  EXPECT_EQ(-ETIMEDOUT, t.writer->write(t.s.tok, &big, 1, all, ms(600), &posted));
  EXPECT_TRUE(posted);
}

TEST(OfiCutOffFails, Tcp)
{
  // the provider fails to close the endpoint in a cut-off: its writes are
  // not cut off and may still land. No write may report a clean cut-off,
  // and the endpoint must take no more writes.
  stalled_t t;
  if (!t.open([] { return -EBUSY; })) {
    GTEST_SKIP() << "tcp is not available";
  }
  std::atomic<int> r0{1}, r1{1};
  std::thread a([&] { r0 = t.write(0, ms(600)); });
  std::this_thread::sleep_for(ms(20));
  std::thread b([&] { r1 = t.write(1, ms(5000)); });
  a.join();
  b.join();
  EXPECT_EQ(-ENOTRECOVERABLE, r0.load());
  EXPECT_EQ(-ENOTRECOVERABLE, r1.load());
  EXPECT_TRUE(t.writer->unsafe());
  const auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cutoffs_failed);
  EXPECT_TRUE(st.unsafe);
  EXPECT_TRUE(st.broken);
  EXPECT_NE(std::string::npos, t.writer->last_error().find("cut-off failed"))
    << t.writer->last_error();

  // nothing more goes out
  window_owner_t h;
  h.ep = open_target("tcp", "127.0.0.1");
  ASSERT_TRUE(h.ep && lend(h, 4096));
  iovec iov{t.src.data(), 4096};
  std::vector<Endpoint::write_t> one = {{0, 4096, 0}};
  EXPECT_EQ(-EIO, t.writer->write(h.tok, &iov, 1, one, BUDGET));
}

TEST(OfiCutOffLate, Tcp)
{
  // a cut-off that ends after a write's budget may have let bytes land
  // after it: that write fails with -ENOTRECOVERABLE and the endpoint
  // turns unsafe. A write whose budget the cut-off still met is merely
  // cancelled.
  stalled_t t;
  if (!t.open([] { std::this_thread::sleep_for(ms(400)); return 0; },
	      [](config_t& c) { c.late_tolerance = ms(200); })) {
    GTEST_SKIP() << "tcp is not available";
  }
  std::atomic<int> r0{1}, r1{1};
  std::thread a([&] { r0 = t.write(0, ms(600)); });
  std::this_thread::sleep_for(ms(20));
  std::thread b([&] { r1 = t.write(1, ms(20000)); });
  a.join();
  b.join();
  EXPECT_EQ(-ENOTRECOVERABLE, r0.load());
  EXPECT_EQ(-ECANCELED, r1.load());
  EXPECT_TRUE(t.writer->unsafe());
  const auto st = t.writer->stats();
  EXPECT_EQ(1u, st.cutoffs_late);
  EXPECT_EQ(0u, st.cutoffs_failed);
  EXPECT_EQ(-EIO, t.write(2, ms(2000)));
}

class OfiRekey : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};

TEST_P(OfiRekey, OldTokenStopsWorking)
{
  // a window given a new key takes no write that carries the old one: a
  // late duplicate of a write that completed before the window was reused
  // must not land in it
  auto [prov, node] = GetParam();
  auto gate = std::make_shared<gate_t>();
  window_owner_t a;
  a.ep = open_target(prov, node);
  auto writer = open_writer(prov, node, true, 1 << 20, 2, gate);
  if (!a.ep || !writer) {
    GTEST_SKIP() << prov << " is not available";
  }
  const size_t N = 1 << 20;
  a.mem.assign(N, 0);
  Endpoint::window_t w;
  ASSERT_EQ(0, a.ep->register_window(a.mem.data(), N, &w));
  auto t1 = parse_token(a.ep->window_token(w, 0, N));
  ASSERT_TRUE(t1);
  std::vector<char> src(N, 'o');
  iovec iov{src.data(), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};
  ASSERT_EQ(0, writer->write(*t1, &iov, 1, all, BUDGET)) << writer->last_error();

  // the window is reused: new key, then new contents
  EXPECT_EQ(-1, a.ep->stats().rekey_in_place);  // not asked yet
  ASSERT_EQ(0, a.ep->rekey_window(w)) << a.ep->last_error();
  auto t2 = parse_token(a.ep->window_token(w, 0, N));
  ASSERT_TRUE(t2);
  EXPECT_NE(t1->key, t2->key);
  auto st = a.ep->stats();
  EXPECT_EQ(1u, st.windows_rekeyed);
  // tcp and shm cannot re-key in place: the memory was registered again,
  // and the provider is not asked again
  EXPECT_EQ(0, st.rekey_in_place);
  EXPECT_EQ(0u, st.rekeys_in_place);
  EXPECT_EQ(1u, st.rekeys_reregistered);
  ASSERT_EQ(0, a.ep->rekey_window(w)) << a.ep->last_error();
  auto t3 = parse_token(a.ep->window_token(w, 0, N));
  ASSERT_TRUE(t3);
  EXPECT_NE(t2->key, t3->key);
  EXPECT_EQ(2u, a.ep->stats().rekeys_reregistered);
  t2 = t3;
  a.ep->sync();
  std::fill(a.mem.begin(), a.mem.end(), 'n');

  // a write with the old key fails, and nothing of it lands
  std::vector<char> late(N, 'L');
  iovec liov{late.data(), N};
  EXPECT_NE(0, writer->write(*t1, &liov, 1, all, BUDGET));
  a.ep->sync();
  EXPECT_EQ(std::string::npos,
	    std::string_view(a.mem.data(), N).find('L'));
  // the new key works
  EXPECT_EQ(0, writer->write(*t2, &iov, 1, all, BUDGET)) << writer->last_error();
  a.ep->sync();
  EXPECT_EQ(0, memcmp(a.mem.data(), src.data(), N));
}

TEST_P(OfiRekey, KeysDoNotRepeat)
{
  // many re-keys of one window, and of a window that comes and goes:
  // no key is given out twice while in quarantine
  auto [prov, node] = GetParam();
  window_owner_t a;
  a.ep = open_target(prov, node);
  if (!a.ep) {
    GTEST_SKIP() << prov << " is not available";
  }
  ASSERT_TRUE(lend(a, 1 << 20));
  Endpoint::window_t w;
  ASSERT_EQ(0, a.ep->register_window(a.mem.data(), 4096, &w));
  std::set<uint64_t> keys;
  for (int i = 0; i < 200; i++) {
    auto t = parse_token(a.ep->window_token(w, 0, 4096));
    ASSERT_TRUE(t);
    EXPECT_TRUE(keys.insert(t->key).second) << "key " << t->key << " again";
    ASSERT_EQ(0, a.ep->rekey_window(w)) << a.ep->last_error();
  }
  std::vector<char> other(4096);
  for (int i = 0; i < 50; i++) {
    Endpoint::window_t x;
    ASSERT_EQ(0, a.ep->register_window(other.data(), other.size(), &x));
    auto t = parse_token(a.ep->window_token(x, 0, other.size()));
    ASSERT_TRUE(t);
    EXPECT_TRUE(keys.insert(t->key).second) << "key " << t->key << " again";
    a.ep->deregister_window(x.id);
  }
}

INSTANTIATE_TEST_SUITE_P(
  Providers, OfiRekey,
  ::testing::Values(std::make_pair("tcp", "127.0.0.1"),
		    std::make_pair("shm", "")),
  [](const auto& info) { return std::string(info.param.first); });

TEST_P(OfiRekey, WindowPoolRekeysOnRelease)
{
  // a pool of windows lent and reused, as an OSD's gather windows are: a
  // window released with rekey comes back under a new key, and a write
  // meant for its last use fails instead of landing in the next one
  auto [prov, node] = GetParam();
  auto gate = std::make_shared<gate_t>();
  window_owner_t a;
  a.ep = open_target(prov, node);
  auto writer = open_writer(prov, node, true, 256 << 10, 2, gate);
  if (!a.ep || !writer) {
    GTEST_SKIP() << prov << " is not available";
  }
  const size_t W = 256 << 10;
  a.mem.assign(2 * W, 0);
  std::string err;
  auto pool = WindowPool::create(*a.ep, a.mem.data(), W, 2, &err);
  ASSERT_TRUE(pool) << err;
  std::vector<char> src(W, 'g'), late(W, 'L');
  iovec iov{src.data(), W}, liov{late.data(), W};
  std::vector<Endpoint::write_t> all = {{0, W, 0}};

  auto first = pool->acquire(W);
  ASSERT_TRUE(first);
  auto t1 = parse_token(first->token);
  ASSERT_TRUE(t1);
  ASSERT_EQ(0, writer->write(*t1, &iov, 1, all, BUDGET)) << writer->last_error();
  pool->release(first->id, ms(0), true);
  EXPECT_EQ(1u, pool->stats().rekeyed);

  // both windows are free, and the released one is lent again at once
  auto second = pool->acquire(W);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->id, second->id);
  auto t2 = parse_token(second->token);
  ASSERT_TRUE(t2);
  EXPECT_NE(t1->key, t2->key);
  a.ep->sync();
  std::fill(second->ptr, second->ptr + W, 'n');
  EXPECT_NE(0, writer->write(*t1, &liov, 1, all, BUDGET));
  a.ep->sync();
  EXPECT_EQ(std::string::npos, std::string_view(second->ptr, W).find('L'));

  // without rekey the key stays, and a quarantine keeps the window out
  pool->release(second->id, ms(0), false);
  auto third = pool->acquire(W);
  ASSERT_TRUE(third);
  EXPECT_EQ(t2->key, parse_token(third->token)->key);
  pool->release(third->id, ms(60000), false);
  auto fourth = pool->acquire(W);
  ASSERT_TRUE(fourth);
  EXPECT_NE(third->id, fourth->id);
  EXPECT_FALSE(pool->acquire(W));  // both out: one lent, one quarantined
  EXPECT_EQ(1u, pool->stats().exhausted);
  EXPECT_FALSE(pool->acquire(W + 1));  // too large for any window
}

TEST_P(OfiRekey, WindowPoolKeepsAFailedPushOut)
{
  // a window released after a push that failed or was cut off gets a new
  // key and still stays out for its quarantine, and for the key
  // quarantine at least: the new key keeps that push's late packets out
  // only if the provider drops what carries the old one
  auto [prov, node] = GetParam();
  window_owner_t a;
  a.ep = open_target(prov, node);
  if (!a.ep) {
    GTEST_SKIP() << prov << " is not available";
  }
  const size_t W = 64 << 10;
  a.mem.assign(3 * W, 0);
  std::string err;
  auto pool = WindowPool::create(*a.ep, a.mem.data(), W, 3, &err);
  ASSERT_TRUE(pool) << err;

  auto failed = pool->acquire(W);
  ASSERT_TRUE(failed);
  const auto k1 = parse_token(failed->token)->key;
  pool->release(failed->id, ms(60000), true);
  auto st = pool->stats();
  EXPECT_EQ(1u, st.rekeyed);
  EXPECT_EQ(1u, st.rekeyed_quarantined);

  // a short quarantine is raised to the key quarantine (10 s)
  auto cut = pool->acquire(W);
  ASSERT_TRUE(cut);
  EXPECT_NE(failed->id, cut->id);
  pool->release(cut->id, ms(1), true);
  std::this_thread::sleep_for(ms(20));

  // a clean release is free at once, under a new key
  auto clean = pool->acquire(W);
  ASSERT_TRUE(clean);
  EXPECT_NE(failed->id, clean->id);
  EXPECT_NE(cut->id, clean->id);
  const auto k3 = parse_token(clean->token)->key;
  pool->release(clean->id, ms(0), true);
  auto again = pool->acquire(W);
  ASSERT_TRUE(again);
  EXPECT_EQ(clean->id, again->id);
  EXPECT_NE(k3, parse_token(again->token)->key);
  EXPECT_NE(k1, parse_token(again->token)->key);
  EXPECT_FALSE(pool->acquire(W));  // the other two are still out
  st = pool->stats();
  EXPECT_EQ(3u, st.rekeyed);
  EXPECT_EQ(2u, st.rekeyed_quarantined);
}

namespace {

/// Gathers run side by side the way an OSD's do: each borrows a window
/// from the pool, the peer pushes a pattern no other push has into it,
/// and the owner takes what it finds there and gives the window back
/// with a new key. A push that failed gives its window back unclean.
/// Every push that succeeded must have left exactly its own bytes, and
/// nothing else may land in a window while it is lent.
struct gather_cycle_t {
  static constexpr size_t W = 128 << 10;
  WindowPool& pool;
  Endpoint& owner;
  Endpoint& pusher;
  std::atomic<int> taken{0}, failed{0}, wrong{0}, stray{0}, exhausted{0};
  std::atomic<int> first_rc{0};

  /// rounds per gather at least, and on until *until when given
  void run(size_t threads, size_t rounds, ms budget,
	   const std::atomic<bool>* until = nullptr) {
    std::vector<std::thread> th;
    for (size_t t = 0; t < threads; t++) {
      th.emplace_back([&, t] {
	std::vector<uint64_t> src(W / 8);
	for (size_t round = 0; round < rounds || (until && !*until);
	     round++) {
	  auto lent = pool.acquire(W);
	  if (!lent) {
	    exhausted++;
	    std::this_thread::sleep_for(ms(1));
	    continue;
	  }
	  auto tok = parse_token(lent->token);
	  // pushes of different lengths, with the gather's thread and
	  // round in every word
	  const size_t len = W - (round % 4) * 4096;
	  for (size_t i = 0; i < len / 8; i++) {
	    src[i] = (uint64_t(t + 1) << 56) | (uint64_t(round & 0xffffff) << 32) | i;
	  }
	  iovec iov{src.data(), len};
	  std::vector<Endpoint::write_t> all = {{0, len, 0}};
	  const int r = pusher.write(*tok, &iov, 1, all, budget);
	  if (r != 0) {
	    int z = 0;
	    first_rc.compare_exchange_strong(z, r);
	    failed++;
	    pool.release(lent->id, ms(0), true);
	    continue;
	  }
	  owner.sync();
	  std::vector<char> copy(lent->ptr, lent->ptr + len);
	  if (std::memcmp(copy.data(), src.data(), len) != 0) {
	    wrong++;
	  }
	  // still lent: nothing may write here now
	  std::this_thread::sleep_for(std::chrono::microseconds(200));
	  owner.sync();
	  if (std::memcmp(lent->ptr, copy.data(), len) != 0) {
	    stray++;
	  }
	  taken++;
	  pool.release(lent->id, ms(0), true);
	}
      });
    }
    for (auto& x : th) {
      x.join();
    }
  }
};

} // anonymous namespace

TEST_P(OfiRekey, GatherCycleTakesOnlyWhatWasPushed)
{
  auto [prov, node] = GetParam();
  auto gate = std::make_shared<gate_t>();
  window_owner_t a;
  a.ep = open_target(prov, node);
  constexpr size_t T = 4, NW = 6, ROUNDS = 200;
  auto writer = open_writer(prov, node, true, gather_cycle_t::W, T, gate);
  if (!a.ep || !writer) {
    GTEST_SKIP() << prov << " is not available";
  }
  a.mem.assign(NW * gather_cycle_t::W, 0);
  std::string err;
  auto pool = WindowPool::create(*a.ep, a.mem.data(), gather_cycle_t::W, NW,
				 &err);
  ASSERT_TRUE(pool) << err;
  gather_cycle_t g{*pool, *a.ep, *writer};
  g.run(T, ROUNDS, BUDGET);
  EXPECT_EQ(0, g.wrong.load());
  EXPECT_EQ(0, g.stray.load());
  EXPECT_EQ(0, g.failed.load()) << g.first_rc << ": " << writer->last_error();
  EXPECT_EQ(0, g.exhausted.load());  // more windows than gathers
  EXPECT_EQ(int(T * ROUNDS), g.taken.load());
  EXPECT_EQ(T * ROUNDS, pool->stats().rekeyed);
}

TEST(OfiGatherCycle, CutOffsAlongside)
{
  // the same, with the pusher's writes into a stalled owner cut off
  // alongside the gathers, by cancelling them one by one and then by
  // resetting the endpoint: a reset takes the gathers' pushes in flight
  // with it, and their windows come back unclean. A window is lent again
  // under a new key, so nothing of a push that failed may land in it.
  for (bool per_plan : {true, false}) {
    SCOPED_TRACE(per_plan ? "per-plan cut-off" : "reset");
    stalled_t t;
    if (!t.open(nullptr, [per_plan](config_t& c) {
	  c.stage_count = 16;
	  if (per_plan) {
	    c.cancel_hook = [] { return 0; };
	  }
	})) {
      GTEST_SKIP() << "tcp is not available";
    }
    window_owner_t a;
    a.ep = open_target("tcp", "127.0.0.1");
    ASSERT_TRUE(a.ep);
    constexpr size_t T = 4, NW = 6, ROUNDS = 150;
    a.mem.assign(NW * gather_cycle_t::W, 0);
    std::string err;
    auto pool = WindowPool::create(*a.ep, a.mem.data(), gather_cycle_t::W,
				   NW, &err);
    ASSERT_TRUE(pool) << err;
    gather_cycle_t g{*pool, *a.ep, *t.writer};
    std::atomic<bool> cut_done{false};
    int cut = 0;
    std::thread cutter([&] {
      // short budgets into the stalled owner, while the gathers go on:
      // each ends in a cut-off
      for (int i = 0; i < 4; i++) {
	if (t.write(i % 4, ms(600)) == -ETIMEDOUT) {
	  cut++;
	}
	if (!per_plan) {
	  t.connect();  // the reset dropped the connection
	}
      }
      cut_done = true;
    });
    g.run(T, ROUNDS, BUDGET, &cut_done);
    cutter.join();
    EXPECT_GT(cut, 0);
    EXPECT_EQ(0, g.wrong.load());
    EXPECT_EQ(0, g.stray.load());
    EXPECT_GT(g.taken.load(), int(T * ROUNDS / 2));
    const auto st = t.writer->stats();
    EXPECT_EQ(0u, st.budget_refused);
    if (per_plan) {
      EXPECT_EQ(4u, st.plans_cut_off);
      EXPECT_EQ(0, g.failed.load()) << g.first_rc;
      EXPECT_EQ(0u, st.resets);
    } else {
      EXPECT_EQ(4u, st.resets);
    }
  }
}

namespace {

/// n window owners on tcp, and a writer with the given limits whose
/// inserts go through gate
struct many_t {
  std::vector<window_owner_t> owners;
  std::unique_ptr<Endpoint> writer;
  std::vector<char> src = std::vector<char>(4096, 'm');

  bool open(int n, size_t max_peers, size_t max_pending,
	    std::shared_ptr<gate_t> gate) {
    owners.resize(n);
    for (auto& o : owners) {
      o.ep = open_target("tcp", "127.0.0.1");
      if (!o.ep || !lend(o, 4096)) {
	return false;
      }
    }
    config_t c;
    c.provider = "tcp";
    c.node = "127.0.0.1";
    c.stage_size = 4096;
    c.stage_count = 2 * n;
    c.max_peers = max_peers;
    c.max_pending_inserts = max_pending;
    c.insert_hook = [gate](const std::string& name) { gate->enter(name); };
    std::string err;
    writer = Endpoint::open(c, &err);
    return static_cast<bool>(writer);
  }
  int write(int i, ms budget) {
    iovec iov{src.data(), src.size()};
    std::vector<Endpoint::write_t> one = {{0, src.size(), 0}};
    return writer->write(owners[i].tok, &iov, 1, one, budget);
  }
  const std::string& name(int i) const { return owners[i].tok.name; }
};

} // anonymous namespace

TEST(OfiPeerLimits, PendingInsertsAreBounded)
{
  // clients choose their endpoint names; first contacts beyond the bound
  // are refused at once, instead of queueing behind slow inserts
  auto gate = std::make_shared<gate_t>();
  gate->hold_all = true;
  many_t t;
  if (!t.open(6, 64, 4, gate)) {
    GTEST_SKIP() << "tcp is not available";
  }
  std::vector<std::thread> ts;
  std::vector<int> rc(4, 1);
  for (int i = 0; i < 4; i++) {
    ts.emplace_back([&, i] { rc[i] = t.write(i, ms(5000)); });
  }
  // the four are queued or inserting; a fifth and sixth new peer are not
  // taken
  for (int i = 0; i < 100 && t.writer->stats().pending_inserts < 4; i++) {
    std::this_thread::sleep_for(ms(10));
  }
  EXPECT_EQ(4u, t.writer->stats().pending_inserts);
  const auto t0 = clk::now();
  EXPECT_EQ(-EBUSY, t.write(4, ms(5000)));
  EXPECT_EQ(-EBUSY, t.write(5, ms(5000)));
  EXPECT_LT(clk::now() - t0, ms(1000));
  EXPECT_EQ(2u, t.writer->stats().inserts_refused);
  gate->release();
  for (auto& th : ts) {
    th.join();
  }
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(0, rc[i]) << "writer " << i << ": " << t.writer->last_error();
  }
  // with room again, the refused ones get in
  EXPECT_EQ(0, t.write(4, BUDGET)) << t.writer->last_error();
  EXPECT_EQ(0, t.write(5, BUDGET)) << t.writer->last_error();
  EXPECT_EQ(0u, t.writer->stats().pending_inserts);
}

TEST(OfiPeerLimits, EvictsIdlePeersOnly)
{
  // a full table evicts the least recently used idle peer, never one
  // being added; an evicted peer is added again when written to
  auto gate = std::make_shared<gate_t>();
  many_t t;
  if (!t.open(4, 2, 8, gate)) {
    GTEST_SKIP() << "tcp is not available";
  }
  ASSERT_EQ(0, t.write(0, BUDGET)) << t.writer->last_error();
  std::this_thread::sleep_for(ms(5));
  ASSERT_EQ(0, t.write(1, BUDGET)) << t.writer->last_error();
  EXPECT_EQ(2u, t.writer->stats().peers);

  // the third peer's insert is held; peer 0, the least recently used,
  // makes room for it
  gate->hold = t.name(2);
  std::atomic<int> r2{1};
  std::thread a([&] { r2 = t.write(2, ms(10000)); });
  ASSERT_TRUE(gate->wait_held(ms(5000)));
  // a fourth new peer finds no idle peer to evict but peer 1, and peer 2
  // is being added, so it evicts peer 1 and does not touch peer 2
  EXPECT_EQ(0, t.write(3, BUDGET)) << t.writer->last_error();
  gate->release();
  a.join();
  EXPECT_EQ(0, r2.load()) << t.writer->last_error();
  EXPECT_EQ(1, gate->count(t.name(2)));
  EXPECT_LE(t.writer->stats().peers, 3u);
  // an evicted peer comes back on its next write
  const auto inserted = t.writer->stats().peers_inserted;
  EXPECT_EQ(0, t.write(0, BUDGET)) << t.writer->last_error();
  EXPECT_EQ(inserted + 1, t.writer->stats().peers_inserted);
  EXPECT_EQ(2, gate->count(t.name(0)));
}

TEST(OfiPeerLimits, DestroyWithInsertsPending)
{
  // an endpoint destroyed while inserts are queued and one is still in
  // the provider waits for that one, drops the rest, and does not crash
  auto gate = std::make_shared<gate_t>();
  gate->hold_all = true;
  many_t t;
  if (!t.open(6, 64, 64, gate)) {
    GTEST_SKIP() << "tcp is not available";
  }
  for (int i = 0; i < 6; i++) {
    EXPECT_EQ(-ETIMEDOUT, t.write(i, ms(250)));
  }
  EXPECT_EQ(6u, t.writer->stats().pending_inserts);
  ASSERT_TRUE(gate->wait_total(1, ms(5000)));
  std::thread opener([&] {
    std::this_thread::sleep_for(ms(300));
    gate->release();
  });
  const auto t0 = clk::now();
  t.writer.reset();
  EXPECT_LT(clk::now() - t0, ms(5000));
  opener.join();
  // the four insert threads had started at most four inserts
  EXPECT_LE(gate->total, 4);
}

TEST(OfiLateStart, DoesNotCutOffOthers)
{
  // A write that reaches the point of posting with its budget nearly
  // spent (here the test holds it there) must give up having sent
  // nothing. Posting would cut its writes off at once, and with them the
  // writes of everyone else in flight.
  window_owner_t s;
  s.ep = open_target("tcp", "127.0.0.1", false);
  std::atomic<int> calls{0};
  config_t c;
  c.provider = "tcp";
  c.node = "127.0.0.1";
  c.stage_size = 4 << 20;
  c.stage_count = 4;
  c.pre_post_hook = [&] {
    if (++calls == 3) {  // the late write: past its 600 ms budget
      std::this_thread::sleep_for(ms(700));
    }
  };
  std::string err;
  auto writer = Endpoint::open(c, &err);
  if (!s.ep || !writer) {
    GTEST_SKIP() << "tcp is not available";
  }
  const size_t S = 4 << 20;
  ASSERT_TRUE(lend(s, 4 * S));
  std::vector<char> src(S, 'p');
  {
    std::atomic<bool> stop{false};
    std::thread poller([&] { while (!stop) s.ep->progress(); });
    iovec iov{src.data(), 4096};
    std::vector<Endpoint::write_t> one = {{0, 4096, 0}};
    EXPECT_EQ(0, writer->write(s.tok, &iov, 1, one, BUDGET));  // call 1
    stop = true;
    poller.join();
  }
  // a write in flight into the stalled owner, with plenty of budget
  std::atomic<int> r_long{1};
  std::chrono::steady_clock::duration took_long{};
  std::thread a([&] {
    iovec iov{src.data(), S};
    std::vector<Endpoint::write_t> one = {{0, S, 0}};
    const auto t0 = clk::now();
    r_long = writer->write(s.tok, &iov, 1, one, ms(2500));  // call 2
    took_long = clk::now() - t0;
  });
  std::this_thread::sleep_for(ms(50));
  iovec iov{src.data(), S};
  std::vector<Endpoint::write_t> one = {{0, S, S}};
  EXPECT_EQ(-ETIMEDOUT, writer->write(s.tok, &iov, 1, one, ms(600)));  // 3
  EXPECT_EQ(1u, writer->stats().late_starts);
  EXPECT_EQ(0u, writer->stats().resets);  // nothing was cut off
  a.join();
  // the long write ran to its own deadline: cut off then, not cancelled
  EXPECT_EQ(-ETIMEDOUT, r_long.load());
  EXPECT_GT(took_long, ms(2000));
  EXPECT_EQ(1u, writer->stats().resets);
  EXPECT_FALSE(writer->unsafe());
}

TEST(OfiCutOffCost, LearnsFromCutOffs)
{
  // the cut-off cost starts as a guess of 100 ms; a budget no longer
  // than that is refused, and counted. Cut-offs that take less pull the
  // estimate down, until that budget is taken again.
  stalled_t t;
  if (!t.open(nullptr)) {
    GTEST_SKIP() << "tcp is not available";
  }
  auto st = t.writer->stats();
  EXPECT_EQ(100u, st.cutoff_cost_ms);
  EXPECT_EQ(-ETIMEDOUT, t.write(0, ms(90)));
  EXPECT_EQ(1u, t.writer->stats().budget_refused);
  EXPECT_EQ(0u, t.writer->stats().resets);  // refused: nothing was sent

  uint64_t prev = st.cutoff_cost_ms;
  for (int i = 0; i < 4; i++) {
    ASSERT_TRUE(t.connect()) << t.writer->last_error();
    EXPECT_EQ(-ETIMEDOUT, t.write(i % 4, ms(400)));
    st = t.writer->stats();
    EXPECT_EQ(uint64_t(i + 1), st.resets);
    EXPECT_LE(st.cutoff_cost_ms, prev) << "cut-off " << i;
    prev = st.cutoff_cost_ms;
  }
  // tcp closes and reopens in a few ms, so the estimate fell well below
  // the guess; a budget just above it is taken now, and cut off
  ASSERT_LT(st.cutoff_cost_ms, 90u);
  EXPECT_FALSE(t.writer->unsafe());
  const auto refused = st.budget_refused;
  ASSERT_TRUE(t.connect()) << t.writer->last_error();
  EXPECT_EQ(-ETIMEDOUT, t.write(1, ms(st.cutoff_cost_ms + 60)));
  st = t.writer->stats();
  EXPECT_EQ(refused, st.budget_refused);
  EXPECT_EQ(5u, st.resets);
  EXPECT_FALSE(t.writer->unsafe()) << t.writer->last_error();
}

INSTANTIATE_TEST_SUITE_P(
  Providers, OfiConcurrentWrites,
  ::testing::Values(std::make_pair("tcp", "127.0.0.1"),
		    std::make_pair("shm", "")),
  [](const auto& info) { return std::string(info.param.first); });

INSTANTIATE_TEST_SUITE_P(
  Providers, OfiPeerInsert,
  ::testing::Values(std::make_pair("tcp", "127.0.0.1"),
		    std::make_pair("shm", "")),
  [](const auto& info) { return std::string(info.param.first); });

INSTANTIATE_TEST_SUITE_P(
  Providers, OfiWrite,
  ::testing::Values(std::make_pair("tcp", "127.0.0.1"),
		    std::make_pair("shm", "")),
  [](const auto& info) { return std::string(info.param.first); });
