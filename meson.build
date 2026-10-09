# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: MIT

project('ofi-rma', 'c',
  version: '0.3.0',
  license: 'MIT',
  meson_version: '>=0.60.0',
  default_options: ['c_std=gnu11', 'warning_level=2'])

fabric_dep = dependency('libfabric')
threads_dep = dependency('threads')
inc = include_directories('include')

ofi_rma_lib = library('ofi_rma',
  'src/endpoint.c', 'src/pool.c', 'src/token.c', 'src/util.c',
  include_directories: inc,
  c_args: ['-D_GNU_SOURCE'],
  dependencies: [fabric_dep, threads_dep],
  version: meson.project_version(),
  install: true)

install_headers('include/ofi_rma/ofi_rma.h', 'include/ofi_rma/ofi_rma.hpp',
  subdir: 'ofi_rma')
install_data('LICENSE', install_dir: get_option('datadir') / 'doc' / 'ofi-rma')

# what a project that uses this one as a subproject links with; NIXL
# finds it as dependency('ofi-rma'), installed or from a wrap. C++ code
# includes ofi_rma/ofi_rma.hpp, which needs C++17.
ofi_rma_dep = declare_dependency(
  link_with: ofi_rma_lib,
  include_directories: inc,
  dependencies: [threads_dep])
meson.override_dependency('ofi-rma', ofi_rma_dep)

import('pkgconfig').generate(ofi_rma_lib,
  name: 'ofi-rma',
  description: 'Out-of-band RMA windows and ofi1 tokens over libfabric')

if not get_option('tests').disabled()
  # the C interface on its own, built as strict C11
  test('unittest_ofi_rma_c',
    executable('unittest_ofi_rma_c', 'test/test_c_api.c',
      c_args: ['-std=c11', '-Wpedantic', '-Werror'],
      dependencies: [ofi_rma_dep]),
    timeout: 120)
endif

have_cpp = add_languages('cpp', required: false, native: false)
gtest_dep = dependency('', required: false)
if have_cpp
  gtest_dep = dependency('gtest', main: true, required: get_option('tests'))
endif
if gtest_dep.found()
  test('unittest_ofi_rma',
    executable('unittest_ofi_rma', 'test/test_ofi_rma.cc',
      cpp_args: ['-std=c++17'],
      dependencies: [ofi_rma_dep, gtest_dep]),
    timeout: 900)
endif

# windows in AMD GPU memory, over shm with FI_HMEM_ROCR; the tests skip
# when the GPU, or libfabric's ROCr support, is missing
# the test calls only the HIP runtime's host API, so it links
# libamdhip64 and compiles as plain C++
hip_dep = dependency('', required: false)
if have_cpp and not get_option('hip').disabled()
  cpp = meson.get_compiler('cpp')
  hip_args = ['-D__HIP_PLATFORM_AMD__']
  if import('fs').is_dir('/opt/rocm/include')
    hip_args += ['-isystem', '/opt/rocm/include']
  endif
  hip_lib = cpp.find_library('amdhip64', dirs: ['/opt/rocm/lib'],
    required: false)
  hsa_lib = cpp.find_library('hsa-runtime64', dirs: ['/opt/rocm/lib'],
    required: false)
  if hip_lib.found() and hsa_lib.found() and cpp.has_header('hip/hip_runtime_api.h', args: hip_args)
    hip_dep = declare_dependency(dependencies: [hip_lib, hsa_lib], compile_args: hip_args)
  endif
endif
if gtest_dep.found() and hip_dep.found() and not get_option('hip').disabled()
  test('unittest_ofi_rma_hip',
    executable('unittest_ofi_rma_hip', 'test/test_ofi_rma_hip.cc',
      cpp_args: ['-std=c++17'],
      dependencies: [ofi_rma_dep, gtest_dep, hip_dep]),
    timeout: 300)
elif get_option('hip').enabled()
  error('hip=enabled, but the HIP runtime or GoogleTest is missing')
endif
