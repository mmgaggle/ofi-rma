project('ofi-rma', 'cpp',
  version: '0.1.0',
  license: 'LGPL-2.1 OR LGPL-3.0',
  meson_version: '>=0.60.0',
  default_options: ['cpp_std=c++17', 'warning_level=1'])

fabric_dep = dependency('libfabric')
threads_dep = dependency('threads')
inc = include_directories('include')

ofi_rma_lib = library('ofi_rma', 'src/ofi_rma.cc',
  include_directories: inc,
  dependencies: [fabric_dep, threads_dep],
  version: meson.project_version(),
  install: true)

install_headers('include/ofi_rma/ofi_rma.h', subdir: 'ofi_rma')

# what a project that uses this one as a subproject links with; NIXL
# finds it as dependency('ofi-rma'), installed or from a wrap
ofi_rma_dep = declare_dependency(
  link_with: ofi_rma_lib,
  include_directories: inc,
  dependencies: [threads_dep])
meson.override_dependency('ofi-rma', ofi_rma_dep)

import('pkgconfig').generate(ofi_rma_lib,
  name: 'ofi-rma',
  description: 'Out-of-band RMA windows and ofi1 tokens over libfabric')

gtest_dep = dependency('gtest', main: true, required: get_option('tests'))
if gtest_dep.found()
  test('unittest_ofi_rma',
    executable('unittest_ofi_rma', 'test/test_ofi_rma.cc',
      dependencies: [ofi_rma_dep, gtest_dep]),
    timeout: 900)
endif
