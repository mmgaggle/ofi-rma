// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

#include "common/ofi_rma.h"

#include <cerrno>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
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
  bool open = false;
  std::map<std::string, int> calls;
  clk::time_point held_at{};

  void enter(const std::string& name) {
    std::unique_lock l(m);
    calls[name]++;
    if (name == hold) {
      held_at = clk::now();
      cv.notify_all();
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
};

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
  // while it runs must give up at their budgets, not when it ends.
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

  // first contact with A while it is in flight
  std::atomic<int> ra{1};
  std::thread ta([&] { ra = writer->write(a.tok, &zv, 1, one, ms(20000)); });
  ASSERT_TRUE(gate->wait_held(ms(5000)));
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

  // while the insert holds the endpoint, a write to B gives up at its
  // budget instead of waiting for the insert
  const auto t0 = clk::now();
  EXPECT_EQ(-ETIMEDOUT, writer->write(b.tok, &zv, 1, one, ms(400)));
  EXPECT_LT(clk::now() - t0, ms(1000));
  EXPECT_GE(writer->stats().peer_timeouts, 1u);

  gate->release();
  ta.join();
  EXPECT_EQ(0, ra.load()) << writer->last_error();
  EXPECT_EQ(0, writer->write(b.tok, &iov, 1, all, BUDGET)) << writer->last_error();
  b.ep->sync();
  EXPECT_EQ(0, memcmp(b.mem.data(), src.data(), N));
  EXPECT_EQ(1, gate->count(a.tok.name));
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

  bool open(std::function<int()> hook) {
    s.ep = open_target("tcp", "127.0.0.1", false);
    config_t c;
    c.provider = "tcp";
    c.node = "127.0.0.1";
    c.stage_size = S;
    c.stage_count = 4;
    c.cutoff_close_hook = std::move(hook);
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
  if (!t.open([] { std::this_thread::sleep_for(ms(400)); return 0; })) {
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
