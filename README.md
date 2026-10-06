# ofi-rma

ofi-rma moves data out of band into memory that a peer lends, over libfabric. It depends only on libfabric and the C++17 standard library.

A window owner registers memory and hands out a token that names it. Any writer that holds the token and runs the same libfabric provider writes into that memory. The writer needs no connection to the owner, and the owner never adds the writer to its address vector.

Ceph uses ofi-rma for the out-of-band legs that its OSDs and gateway run. A client of Ceph's S3-over-RDMA data path, such as a NIXL object engine, uses it to lend the memory that the OSDs write into.

## Token format

```
<base hex>:<size hex>:ofi1:<provider>:<endpoint name hex>:<key hex>
```

- `base` is the remote address of window byte 0, as the owner's provider reads it. That is a virtual address under `FI_MR_VIRT_ADDR` and a region offset otherwise. A writer writes window byte `i` at `base + i`.
- `size` is the length of the window.
- `provider` is `fabric_attr->prov_name`, for example `tcp` or `verbs;ofi_rxm`. Both ends must run the same provider.
- `endpoint name` is the owner's `fi_getname()` bytes, at most 192 bytes.
- `key` is the region's `fi_mr_key()`. A provider with keys longer than 8 bytes, or with `FI_MR_RAW`, is refused.

The token starts with the same `addr:size` fields as a cuObject descriptor. A server that only forwards tokens can read the window size without parsing the rest.

## Providers

| Provider | Notes |
|----------|-------|
| `tcp` | Needs no special hardware. It cannot cut off a late write, so use it for tests only. |
| `shm` | Works between processes on one host. |
| `verbs;ofi_rxm` | RDMA reliable connections on any verbs device. On soft-RoCE, set `FI_UNIVERSE_SIZE=16` in every process. |
| `uet` | Ultra Ethernet, through a libfabric wrapper for the UEC reference provider. |

A provider with manual progress places incoming data only while the owner polls its endpoint. Set `config_t::progress_thread` for an endpoint that lends windows, or poll it with `progress()`.

## Device memory

A window owner can lend a window in GPU memory. Set `config_t::hmem`, then register the window with a `memory_t` that names the memory kind: `cuda`, `rocr` or `ze`.

- The provider must offer `FI_HMEM`, and `Endpoint::hmem()` says whether it does. Without it, the registration fails with `-EOPNOTSUPP`.
- libfabric itself must have the runtime for that memory kind. A provider can offer `FI_HMEM` while libfabric lacks the runtime, and the registration then fails with `-ENOSYS`. For example, the Fedora libfabric 2.3.1 package has no ROCr support. Run with `FI_LOG_LEVEL=info` to see the memory interfaces that libfabric initialized.
- A writer does not ask for device memory. It writes into a GPU window as into a host window.
- For a NIC to write straight into GPU memory, the provider and the NIC driver must support it, usually through dma-buf. The platform must also give the NIC a PCIe path to the GPU. A software provider such as `shm` copies into the GPU instead.
- A window keeps its memory kind when `rekey_window()` or a cut-off registers it again.
- `WindowPool` lends host memory only.

The GPU tests in `test/test_ofi_rma_hip.cc` run over `shm` on an AMD GPU. They skip if there is no GPU, or if libfabric has no ROCr support. To build libfabric with ROCr support, configure it with `--with-rocr=/usr`, or with the path of your ROCm installation.

## Build

You need libfabric 1.18 or later, with its headers. The tests also need GoogleTest.

To build and test with CMake:

```
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build
```

To build and test with meson (the `tests` and `hip` options are features, `auto` by default):

```
meson setup build-meson
meson test -C build-meson
```

The CMake build has these options:

| Option | Default for a top-level build | Default under `add_subdirectory` |
|--------|-------------------------------|----------------------------------|
| `OFI_RMA_SHARED` | `ON` | `OFF`: a static library with position-independent code |
| `OFI_RMA_BUILD_TESTS` | `ON` | `OFF` |
| `OFI_RMA_INSTALL` | `ON` | `OFF` |
| `OFI_RMA_HIP` | `AUTO`: build the GPU tests if the HIP runtime is found | `AUTO`, with tests off |

The tests run over `tcp` and `shm`. They skip a provider that is not available. A full run takes about 40 seconds.

## Use from another project

An installed ofi-rma ships a pkg-config file and a CMake package.

- pkg-config: `pkg-config --cflags --libs ofi-rma`.
- CMake: `find_package(ofi_rma)`, then link `ofi_rma::ofi_rma`.
- CMake, from source: `add_subdirectory(ofi-rma)`, then link `ofi_rma`.
- meson: `dependency('ofi-rma')`. With a wrap file, the dependency also resolves from a subproject.

Include the header as `#include <ofi_rma/ofi_rma.h>`. The namespace is `ofi_rma`.

## Origin and license

This code started in Ceph as `src/common/ofi_rma.{h,cc}` and `src/test/common/test_ofi_rma.cc`. The history of those three files came along with them.

ofi-rma is licensed under the MIT license. See `LICENSE`. In Ceph the code was under the LGPL. The author of every commit relicensed it under MIT when it left Ceph.
