# The portable protocol core.
#
# These sources are compiled BOTH into the kernel driver (by MSBuild/WDK) and
# into user-mode tools and tests (here).  They must therefore avoid the CRT,
# the STL, exceptions, RTTI and dynamic allocation.  Keeping them in a
# standalone library that CI builds on Linux is what keeps that discipline
# honest.

add_library(cla_core STATIC
    src/LampArrayDescriptor.cpp
    src/LampArrayDevice.cpp
    src/HidDescriptorParser.cpp
    src/IpcContract.cpp
)

target_include_directories(cla_core PUBLIC include)

# No exceptions or RTTI: this code runs at DISPATCH_LEVEL in the driver.
if(MSVC)
    target_compile_options(cla_core PRIVATE /GR-)
else()
    target_compile_options(cla_core PRIVATE -fno-exceptions -fno-rtti)
endif()

set_target_properties(cla_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Corsair topology -> LampArray mapping.
#
# Deliberately a SEPARATE library: it uses floating point and the STL, neither
# of which belongs in a kernel driver (floating point in kernel mode needs
# KeSaveFloatingPointState).  The driver links cla_core only, so this boundary
# is enforced by the build rather than by convention.
add_library(cla_mapping STATIC
    src/CorsairMapping.cpp
)
target_link_libraries(cla_mapping PUBLIC cla_core)
set_target_properties(cla_mapping PROPERTIES POSITION_INDEPENDENT_CODE ON)
