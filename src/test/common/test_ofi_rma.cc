// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp

#include "common/ofi_rma.h"

#include <cerrno>
#include <cstring>
#include <vector>

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
  c.op_timeout = std::chrono::milliseconds(3000);
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
  ASSERT_EQ(0, p.writer->write(*tok, iov, 2, ws)) << p.writer->last_error();
  p.target->sync();
  EXPECT_EQ(0, memcmp(win.data(), src.data() + N / 2, N / 2));
  EXPECT_EQ(0, memcmp(win.data() + N / 2, src.data(), N / 2));

  // a token for part of the window addresses from that part's start
  auto sub = parse_token(p.target->window_token(w, 4096, 8192));
  ASSERT_TRUE(sub);
  std::vector<Endpoint::write_t> one = {{0, 100, 8000}};
  ASSERT_EQ(0, p.writer->write(*sub, iov, 1, one));
  p.target->sync();
  EXPECT_EQ(0, memcmp(win.data() + 4096 + 8000, src.data(), 100));

  // writes that leave the window or the source are refused up front
  std::vector<Endpoint::write_t> past_window = {{0, 100, 8100}};
  EXPECT_EQ(-ERANGE, p.writer->write(*sub, iov, 1, past_window));
  std::vector<Endpoint::write_t> past_source = {{N / 3 - 10, 20, 0}};
  EXPECT_EQ(-ERANGE, p.writer->write(*tok, iov, 1, past_source));

  // another provider's token is not ours to serve
  auto other = *tok;
  other.provider = "nosuchprov";
  EXPECT_EQ(-EPROTONOSUPPORT, p.writer->write(other, iov, 1, one));

  // a source larger than a staging buffer is refused
  std::vector<char> big(5 << 20);
  iovec bigv = {big.data(), big.size()};
  std::vector<Endpoint::write_t> bw = {{0, 10, 0}};
  EXPECT_EQ(-E2BIG, p.writer->write(*tok, &bigv, 1, bw));

  // an endpoint that only lends windows cannot write
  EXPECT_EQ(-EOPNOTSUPP, p.target->write(*tok, iov, 1, one));

  const auto s = p.writer->stats();
  EXPECT_EQ(s.writes_failed, 0u);
  EXPECT_EQ(s.peers_inserted, 1u);
  EXPECT_EQ(s.bytes_written, N + N / 3);
}

INSTANTIATE_TEST_SUITE_P(
  Providers, OfiWrite,
  ::testing::Values(std::make_pair("tcp", "127.0.0.1"),
		    std::make_pair("shm", "")),
  [](const auto& info) { return std::string(info.param.first); });
