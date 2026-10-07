// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab ft=cpp
// SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
// SPDX-License-Identifier: MIT

// Windows in AMD GPU memory, registered through FI_HMEM_ROCR. The shm
// provider places the writes with ROCr copies, so these tests need a GPU
// and ROCm, but no RDMA NIC.

#include "ofi_rma/ofi_rma.h"

#include <hip/hip_runtime_api.h>
#include <hsa/hsa_ext_amd.h>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string_view>
#include <vector>

using namespace ofi_rma;

namespace {

constexpr std::chrono::milliseconds BUDGET{3000};
const memory_t ROCR{memory_t::iface_t::rocr, 0};

bool have_gpu()
{
  int n = 0;
  return hipGetDeviceCount(&n) == hipSuccess && n > 0;
}

/// zeroed device memory
struct gpu_buf_t {
  char* p = nullptr;
  size_t n = 0;
  explicit gpu_buf_t(size_t len) : n(len) {
    void* v = nullptr;
    if (hipMalloc(&v, len) != hipSuccess) {
      return;
    }
    p = static_cast<char*>(v);
    if (hipMemset(p, 0, len) != hipSuccess ||
        hipDeviceSynchronize() != hipSuccess) {
      (void)hipFree(p);
      p = nullptr;
    }
  }
  ~gpu_buf_t() {
    if (p) {
      (void)hipFree(p);
    }
  }
  gpu_buf_t(const gpu_buf_t&) = delete;
  gpu_buf_t& operator=(const gpu_buf_t&) = delete;
  std::vector<char> read() const {
    std::vector<char> v(n);
    EXPECT_EQ(hipSuccess, hipMemcpy(v.data(), p, n, hipMemcpyDeviceToHost));
    return v;
  }
};

std::unique_ptr<Endpoint> open_owner(const char* prov, bool hmem,
                                     std::string* err,
                                     decltype(config_t::reg_hook) hook = {})
{
  config_t c;
  c.provider = prov;
  c.progress_thread = true;
  c.hmem = hmem;
  c.reg_hook = std::move(hook);
  return Endpoint::open(c, err);
}

std::unique_ptr<Endpoint> open_writer(const char* prov, std::string* err)
{
  config_t c;
  c.provider = prov;
  c.stage_size = 4 << 20;
  c.stage_count = 2;
  return Endpoint::open(c, err);
}

/// register a GPU window, or say why the test cannot: a libfabric built
/// without ROCr offers FI_HMEM on shm all the same, and refuses the
/// registration with -ENOSYS
int register_gpu(Endpoint& ep, gpu_buf_t& g, Endpoint::window_t* w)
{
  return ep.register_window(g.p, g.n, ROCR, w);
}

std::vector<char> pattern(size_t n, int seed)
{
  std::vector<char> v(n);
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<char>(i * 7 + seed);
  }
  return v;
}

} // anonymous namespace

TEST(OfiDeviceMemory, HostOnlyEndpointRefusesGpuWindows)
{
  if (!have_gpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  int tried = 0;
  for (const char* prov : {"tcp", "shm"}) {
    std::string err;
    auto ep = open_owner(prov, false, &err);
    if (!ep) {
      continue;
    }
    tried++;
    EXPECT_FALSE(ep->hmem()) << prov;
    gpu_buf_t g(1 << 16);
    ASSERT_TRUE(g.p);
    Endpoint::window_t w;
    EXPECT_EQ(-EOPNOTSUPP, ep->register_window(g.p, g.n, ROCR, &w)) << prov;
    EXPECT_NE(std::string::npos, ep->last_error().find("FI_HMEM")) << prov;
    std::vector<char> host(4096);
    EXPECT_EQ(0, ep->register_window(host.data(), host.size(), &w)) << prov;
  }
  if (!tried) {
    GTEST_SKIP() << "neither tcp nor shm is available";
  }
}

TEST(OfiDeviceMemory, ShmPlacesIntoGpuMemory)
{
  if (!have_gpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  std::string err;
  auto owner = open_owner("shm", true, &err);
  if (!owner) {
    GTEST_SKIP() << "shm: " << err;
  }
  if (!owner->hmem()) {
    GTEST_SKIP() << "shm offers no FI_HMEM here: " << owner->describe();
  }
  auto writer = open_writer("shm", &err);
  ASSERT_TRUE(writer) << err;
  EXPECT_NE(std::string::npos, owner->describe().find("device-memory"));

  const size_t N = 3 << 20;
  gpu_buf_t g(N);
  ASSERT_TRUE(g.p);
  const auto src = pattern(N, 3);
  Endpoint::window_t w;
  if (int r = register_gpu(*owner, g, &w); r == -ENOSYS) {
    GTEST_SKIP() << owner->last_error();
  } else {
    ASSERT_EQ(0, r) << owner->last_error();
  }
  auto tok = parse_token(owner->window_token(w, 0, N));
  ASSERT_TRUE(tok);

  // the source's halves land swapped
  iovec iov{const_cast<char*>(src.data()), N};
  std::vector<Endpoint::write_t> swapped = {{0, N / 2, N / 2},
                                            {N / 2, N / 2, 0}};
  ASSERT_EQ(0, writer->write(*tok, &iov, 1, swapped, BUDGET))
    << writer->last_error();
  owner->sync();
  auto got = g.read();
  EXPECT_EQ(0, memcmp(got.data(), src.data() + N / 2, N / 2));
  EXPECT_EQ(0, memcmp(got.data() + N / 2, src.data(), N / 2));

  // a token for part of the window addresses from that part's start
  auto sub = parse_token(owner->window_token(w, 4096, 8192));
  ASSERT_TRUE(sub);
  const auto small = pattern(100, 11);
  iovec siov{const_cast<char*>(small.data()), small.size()};
  std::vector<Endpoint::write_t> one = {{0, 100, 8000}};
  ASSERT_EQ(0, writer->write(*sub, &siov, 1, one, BUDGET))
    << writer->last_error();
  owner->sync();
  got = g.read();
  EXPECT_EQ(0, memcmp(got.data() + 4096 + 8000, small.data(), 100));
  EXPECT_EQ(0u, writer->stats().writes_failed);
}

TEST(OfiDeviceMemory, RekeyKeepsTheWindowOnTheGpu)
{
  // a window registered again for its new key must stay device memory:
  // re-registered as host memory, the provider would write the GPU
  // address as if it were a host one. A GPU whose memory the host can
  // reach, an APU's, takes such writes all the same, so the test also
  // asks how each region of the window was registered.
  if (!have_gpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  const size_t N = 1 << 20;
  gpu_buf_t g(N);
  ASSERT_TRUE(g.p);
  std::vector<memory_t::iface_t> regs;  // the window's, under the lock
  auto hook = [&](const char* p, size_t, const memory_t& m) {
    if (p == g.p) {
      regs.push_back(m.iface);
    }
  };
  std::string err;
  auto owner = open_owner("shm", true, &err, hook);
  if (!owner || !owner->hmem()) {
    GTEST_SKIP() << "shm with FI_HMEM is not available here";
  }
  auto writer = open_writer("shm", &err);
  ASSERT_TRUE(writer) << err;

  Endpoint::window_t w;
  if (int r = register_gpu(*owner, g, &w); r == -ENOSYS) {
    GTEST_SKIP() << owner->last_error();
  } else {
    ASSERT_EQ(0, r) << owner->last_error();
  }
  auto t1 = parse_token(owner->window_token(w, 0, N));
  ASSERT_TRUE(t1);
  std::vector<Endpoint::write_t> all = {{0, N, 0}};
  std::vector<char> first(N, 'o');
  iovec fiov{first.data(), N};
  ASSERT_EQ(0, writer->write(*t1, &fiov, 1, all, BUDGET)) << writer->last_error();

  ASSERT_EQ(0, owner->rekey_window(w)) << owner->last_error();
  auto t2 = parse_token(owner->window_token(w, 0, N));
  ASSERT_TRUE(t2);
  EXPECT_NE(t1->key, t2->key);
  // shm cannot re-key in place: the memory was registered again, as GPU
  // memory both times
  owner->sync();
  ASSERT_EQ(2u, regs.size());
  for (auto iface : regs) {
    EXPECT_EQ(memory_t::iface_t::rocr, iface);
  }

  // the old key no longer lands
  std::vector<char> late(N, 'L');
  iovec liov{late.data(), N};
  EXPECT_NE(0, writer->write(*t1, &liov, 1, all, BUDGET));
  owner->sync();
  auto got = g.read();
  EXPECT_EQ(std::string::npos, std::string_view(got.data(), N).find('L'));

  // the new one does, into the GPU
  const auto second = pattern(N, 5);
  iovec siov{const_cast<char*>(second.data()), N};
  ASSERT_EQ(0, writer->write(*t2, &siov, 1, all, BUDGET)) << writer->last_error();
  owner->sync();
  got = g.read();
  EXPECT_EQ(0, memcmp(got.data(), second.data(), N));
}

TEST(OfiDeviceMemory, VerbsWritesIntoADmabufWindow)
{
  // A NIC writing straight into GPU memory: the window is a dma-buf that
  // ROCr exported, registered with FI_MR_DMABUF, which needs no FI_HMEM
  // export from libfabric. Over verbs;ofi_rxm on the device whose address
  // OFI_RMA_TEST_VERBS_NODE names; both endpoints sit on it, so the NIC
  // loops the writes back. Soft-RoCE takes no dma-buf: the test skips.
  const char* node = getenv("OFI_RMA_TEST_VERBS_NODE");
  if (!node || !*node) {
    GTEST_SKIP() << "OFI_RMA_TEST_VERBS_NODE is not set";
  }
  if (!have_gpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  config_t oc;
  oc.provider = "verbs;ofi_rxm";
  oc.node = node;
  oc.progress_thread = true;
  oc.hmem = true;
  config_t wc;
  wc.provider = "verbs;ofi_rxm";
  wc.node = node;
  wc.stage_size = 4 << 20;
  wc.stage_count = 2;
  std::string err;
  auto owner = Endpoint::open(oc, &err);
  auto writer = owner ? Endpoint::open(wc, &err) : nullptr;
  if (!owner || !writer) {
    GTEST_SKIP() << "verbs;ofi_rxm at " << node << ": " << err;
  }

  const size_t N = 4 << 20;
  gpu_buf_t g(N);
  ASSERT_TRUE(g.p);
  int fd = -1;
  uint64_t off = 0;
  ASSERT_EQ(HSA_STATUS_SUCCESS, hsa_amd_portable_export_dmabuf(g.p, N, &fd, &off));
  Endpoint::window_t w;
  const memory_t dmabuf{memory_t::iface_t::rocr, 0, fd, off};
  if (int r = owner->register_window(g.p, N, dmabuf, &w); r != 0) {
    close(fd);
    GTEST_SKIP() << "no dma-buf window on this device: " << owner->last_error();
  }
  auto tok = parse_token(owner->window_token(w, 0, N));
  ASSERT_TRUE(tok);
  const auto src = pattern(N, 17);
  iovec iov{const_cast<char*>(src.data()), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};
  ASSERT_EQ(0, writer->write(*tok, &iov, 1, all, BUDGET)) << writer->last_error();
  owner->sync();
  auto got = g.read();
  EXPECT_EQ(0, memcmp(got.data(), src.data(), N));
  owner->deregister_window(w.id);
  close(fd);
}

TEST(OfiDeviceMemory, VerbsWritesIntoPartOfADmabuf)
{
  // A window that starts inside its allocation: the dma-buf covers the
  // whole allocation, and the window begins at an offset into it, as when
  // a caller registers one tensor of a larger pool. The NIC must place the
  // writes at the window's own address.
  const char* node = getenv("OFI_RMA_TEST_VERBS_NODE");
  if (!node || !*node) {
    GTEST_SKIP() << "OFI_RMA_TEST_VERBS_NODE is not set";
  }
  if (!have_gpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  config_t oc;
  oc.provider = "verbs;ofi_rxm";
  oc.node = node;
  oc.progress_thread = true;
  oc.hmem = true;
  config_t wc;
  wc.provider = "verbs;ofi_rxm";
  wc.node = node;
  wc.stage_size = 4 << 20;
  wc.stage_count = 2;
  std::string err;
  auto owner = Endpoint::open(oc, &err);
  auto writer = owner ? Endpoint::open(wc, &err) : nullptr;
  if (!owner || !writer) {
    GTEST_SKIP() << "verbs;ofi_rxm at " << node << ": " << err;
  }

  const size_t N = 4 << 20;
  gpu_buf_t g(2 * N);
  ASSERT_TRUE(g.p);
  char* second_half = g.p + N;
  int fd = -1;
  uint64_t off = 0;
  ASSERT_EQ(HSA_STATUS_SUCCESS,
            hsa_amd_portable_export_dmabuf(second_half, N, &fd, &off));
  EXPECT_EQ(N, off);
  Endpoint::window_t w;
  const memory_t dmabuf{memory_t::iface_t::rocr, 0, fd, off};
  if (int r = owner->register_window(second_half, N, dmabuf, &w); r != 0) {
    close(fd);
    GTEST_SKIP() << "no dma-buf window on this device: " << owner->last_error();
  }
  auto tok = parse_token(owner->window_token(w, 0, N));
  ASSERT_TRUE(tok);
  const auto src = pattern(N, 23);
  iovec iov{const_cast<char*>(src.data()), N};
  std::vector<Endpoint::write_t> all = {{0, N, 0}};
  ASSERT_EQ(0, writer->write(*tok, &iov, 1, all, BUDGET)) << writer->last_error();
  owner->sync();
  auto got = g.read();
  EXPECT_EQ(0, memcmp(got.data() + N, src.data(), N));
  EXPECT_EQ(std::vector<char>(N, 0), std::vector<char>(got.begin(), got.begin() + N));
  owner->deregister_window(w.id);
  close(fd);
}

TEST(OfiDeviceMemory, VerbsReadsFromPartOfADmabuf)
{
  // The PUT direction: a peer pulls the bytes of a GPU buffer, registered
  // for remote reads as a dma-buf that starts before the window does
  const char* node = getenv("OFI_RMA_TEST_VERBS_NODE");
  if (!node || !*node) {
    GTEST_SKIP() << "OFI_RMA_TEST_VERBS_NODE is not set";
  }
  if (!have_gpu()) {
    GTEST_SKIP() << "no HIP device";
  }
  config_t oc;
  oc.provider = "verbs;ofi_rxm";
  oc.node = node;
  oc.progress_thread = true;
  oc.hmem = true;
  oc.reads = true;
  config_t rc;
  rc.provider = "verbs;ofi_rxm";
  rc.node = node;
  rc.reads = true;
  rc.stage_size = 4 << 20;
  rc.stage_count = 2;
  std::string err;
  auto owner = Endpoint::open(oc, &err);
  auto reader = owner ? Endpoint::open(rc, &err) : nullptr;
  if (!owner || !reader) {
    GTEST_SKIP() << "verbs;ofi_rxm at " << node << ": " << err;
  }
  if (!owner->reads() || !reader->reads() || !owner->hmem()) {
    GTEST_SKIP() << "no reads or no FI_HMEM: " << owner->describe();
  }

  const size_t N = 4 << 20;
  gpu_buf_t g(2 * N);
  ASSERT_TRUE(g.p);
  char* second_half = g.p + N;
  const auto src = pattern(N, 41);
  ASSERT_EQ(hipSuccess, hipMemcpy(second_half, src.data(), N,
                                  hipMemcpyHostToDevice));
  int fd = -1;
  uint64_t off = 0;
  ASSERT_EQ(HSA_STATUS_SUCCESS,
            hsa_amd_portable_export_dmabuf(second_half, N, &fd, &off));
  Endpoint::window_t w;
  const memory_t dmabuf{memory_t::iface_t::rocr, 0, fd, off};
  if (int r = owner->register_window(second_half, N, dmabuf,
                                     Endpoint::remote_read, &w); r != 0) {
    close(fd);
    GTEST_SKIP() << "no dma-buf window on this device: " << owner->last_error();
  }
  auto tok = parse_token(owner->window_token(w, 0, N));
  ASSERT_TRUE(tok);
  std::vector<char> got(N, 0);
  iovec iov{got.data(), N};
  ASSERT_EQ(0, reader->read(*tok, &iov, 1, {{0, N, 0}}, BUDGET))
    << reader->last_error();
  EXPECT_EQ(0, memcmp(got.data(), src.data(), N));
  owner->deregister_window(w.id);
  close(fd);
}
