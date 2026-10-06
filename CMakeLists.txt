# SPDX-FileCopyrightText: Copyright (c) 2026 IBM Corporation
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(ofi_rma VERSION 0.1.0 LANGUAGES CXX)

# A project that adds this directory (Ceph does, as a submodule) gets a
# static library and no tests or install rules unless it asks for them.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(OFI_RMA_TOP_LEVEL ON)
else()
  set(OFI_RMA_TOP_LEVEL OFF)
endif()

option(OFI_RMA_SHARED "Build a shared library" ${OFI_RMA_TOP_LEVEL})
option(OFI_RMA_BUILD_TESTS "Build the tests" ${OFI_RMA_TOP_LEVEL})
option(OFI_RMA_INSTALL "Install the library, header and package files"
  ${OFI_RMA_TOP_LEVEL})
set(OFI_RMA_HIP "AUTO" CACHE STRING
  "Test windows in AMD GPU memory with HIP: ON, OFF or AUTO")

include(GNUInstallDirs)
find_package(Threads REQUIRED)

# libfabric: pkg-config first, then a plain search, as distributions ship
# either
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(OFI_RMA_FABRIC QUIET libfabric)
endif()
find_path(FABRIC_INCLUDE_DIR rdma/fabric.h
  HINTS ${OFI_RMA_FABRIC_INCLUDE_DIRS})
find_library(FABRIC_LIBRARY fabric HINTS ${OFI_RMA_FABRIC_LIBRARY_DIRS})
if(NOT FABRIC_INCLUDE_DIR OR NOT FABRIC_LIBRARY)
  message(FATAL_ERROR "ofi_rma needs libfabric (rdma/fabric.h and libfabric)")
endif()

if(OFI_RMA_SHARED)
  add_library(ofi_rma SHARED src/ofi_rma.cc)
  set_target_properties(ofi_rma PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
else()
  add_library(ofi_rma STATIC src/ofi_rma.cc)
  set_target_properties(ofi_rma PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
add_library(ofi_rma::ofi_rma ALIAS ofi_rma)
target_compile_features(ofi_rma PUBLIC cxx_std_17)
target_include_directories(ofi_rma
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${FABRIC_INCLUDE_DIR})
# the public header does not include libfabric's, so a shared library
# keeps it private; a static one passes it on for the final link
if(OFI_RMA_SHARED)
  target_link_libraries(ofi_rma PRIVATE ${FABRIC_LIBRARY}
    PUBLIC Threads::Threads)
else()
  target_link_libraries(ofi_rma PUBLIC ${FABRIC_LIBRARY} Threads::Threads)
endif()

if(OFI_RMA_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(unittest_ofi_rma test/test_ofi_rma.cc)
  target_link_libraries(unittest_ofi_rma ofi_rma GTest::gtest_main)
  add_test(NAME unittest_ofi_rma COMMAND unittest_ofi_rma)

  if(NOT OFI_RMA_HIP STREQUAL "OFF")
    find_path(HIP_RUNTIME_INCLUDE_DIR hip/hip_runtime_api.h
      HINTS /opt/rocm/include)
    find_library(HIP_RUNTIME_LIBRARY amdhip64 HINTS /opt/rocm/lib)
    find_library(HSA_RUNTIME_LIBRARY hsa-runtime64 HINTS /opt/rocm/lib)
    if(HIP_RUNTIME_INCLUDE_DIR AND HIP_RUNTIME_LIBRARY AND HSA_RUNTIME_LIBRARY)
      add_executable(unittest_ofi_rma_hip test/test_ofi_rma_hip.cc)
      target_compile_definitions(unittest_ofi_rma_hip PRIVATE
        __HIP_PLATFORM_AMD__)
      target_include_directories(unittest_ofi_rma_hip PRIVATE
        ${HIP_RUNTIME_INCLUDE_DIR})
      target_link_libraries(unittest_ofi_rma_hip ofi_rma
        ${HIP_RUNTIME_LIBRARY} ${HSA_RUNTIME_LIBRARY} GTest::gtest_main)
      add_test(NAME unittest_ofi_rma_hip COMMAND unittest_ofi_rma_hip)
    elseif(OFI_RMA_HIP STREQUAL "ON")
      message(FATAL_ERROR "OFI_RMA_HIP=ON, but the HIP runtime is missing")
    endif()
  endif()
endif()

if(OFI_RMA_INSTALL)
  include(CMakePackageConfigHelpers)
  install(TARGETS ofi_rma EXPORT ofi_rmaTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES include/ofi_rma/ofi_rma.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ofi_rma)
  install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
  install(EXPORT ofi_rmaTargets NAMESPACE ofi_rma::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ofi_rma)
  configure_package_config_file(cmake/ofi_rmaConfig.cmake.in
    ${PROJECT_BINARY_DIR}/ofi_rmaConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ofi_rma)
  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/ofi_rmaConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${PROJECT_BINARY_DIR}/ofi_rmaConfig.cmake
    ${PROJECT_BINARY_DIR}/ofi_rmaConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ofi_rma)
  if(OFI_RMA_SHARED)
    set(OFI_RMA_PC_REQUIRES_PRIVATE "libfabric")
    set(OFI_RMA_PC_REQUIRES "")
  else()
    set(OFI_RMA_PC_REQUIRES_PRIVATE "")
    set(OFI_RMA_PC_REQUIRES "libfabric")
  endif()
  configure_file(ofi-rma.pc.in ${PROJECT_BINARY_DIR}/ofi-rma.pc @ONLY)
  install(FILES ${PROJECT_BINARY_DIR}/ofi-rma.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()
