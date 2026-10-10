# Copyright (c) Corsair Dynamic Lighting Bridge contributors.
# SPDX-License-Identifier: MIT
#
# Build for everything that is NOT the kernel driver.
#
# The kernel driver (driver/CorsairLampArray) is built with MSBuild and the
# WDK -- see driver/CorsairLampArray/CorsairLampArray.vcxproj and
# docs/development.md.  It compiles the same core/ sources this project builds
# here, which is why the core is kept free of the CRT and the STL.
#
# The core library and its tests are deliberately portable, so the protocol and
# mapping logic can be built and tested on Linux/macOS CI without a Windows
# machine, a WDK, iCUE, or Corsair hardware.

cmake_minimum_required(VERSION 3.16)
project(CorsairDynamicLightingBridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

option(CLA_BUILD_TESTS "Build the automated tests" ON)
option(CLA_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# The protocol parser runs in kernel mode, where an out-of-bounds access is a
# bugcheck rather than a segfault.  Running the same code under the sanitizers
# in CI is how that class of bug gets caught somewhere survivable.
if(CLA_SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
    endif()
endif()
option(CLA_BUILD_TOOLS "Build the diagnostic tools" ON)

add_subdirectory(core)

if(CLA_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(CLA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The service.  Its backend abstraction and fake backend are portable and
# always build, so they can be tested on CI without Windows, iCUE or hardware;
# the iCUE backend and the Windows service host inside only build on Windows.
add_subdirectory(service)
