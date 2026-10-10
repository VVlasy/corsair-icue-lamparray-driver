# Compiles and links the kernel driver sources against stub WDK headers.
#
# The driver can only be built properly with Visual Studio and the WDK, which
# means on most machines it is never compiled at all -- and code that is never
# compiled rots.  This target catches typos, missing declarations, signature
# mismatches and duplicate symbols on any platform, in seconds.
#
# It is explicitly NOT a substitute for a WDK build: the stubs approximate the
# WDK's shape, not its behaviour.  See stubs/README.md.
#
# MSVC is excluded: on Windows the real WDK headers should be used instead, and
# the stubs would collide with the platform SDK.
if(NOT MSVC)
    add_executable(DriverSyntaxCheck
        main.cpp
        StubImplementations.cpp
        ${CMAKE_SOURCE_DIR}/driver/CorsairLampArray/Driver.cpp
        ${CMAKE_SOURCE_DIR}/driver/CorsairLampArray/Device.cpp
        ${CMAKE_SOURCE_DIR}/driver/CorsairLampArray/VhfDevice.cpp
        ${CMAKE_SOURCE_DIR}/driver/CorsairLampArray/Reports.cpp
        ${CMAKE_SOURCE_DIR}/driver/CorsairLampArray/Ioctl.cpp
    )

    target_include_directories(DriverSyntaxCheck PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${CMAKE_SOURCE_DIR}/driver/CorsairLampArray
        ${CMAKE_SOURCE_DIR}/inc
    )

    # _KERNEL_MODE selects the kernel branch of ClaTypes.h, so the core is
    # compiled the same way the driver compiles it.
    target_compile_definitions(DriverSyntaxCheck PRIVATE _KERNEL_MODE)

    target_link_libraries(DriverSyntaxCheck PRIVATE cla_core)

    # The driver is built with warnings as errors under the WDK; hold the same
    # line here so problems surface on any developer's machine, not only on one
    # with a WDK installed.
    # -Wno-multichar: driver pool tags are four-character constants ('cALC'),
    # which is the standard idiom under MSVC and what !poolused displays.
    target_compile_options(DriverSyntaxCheck PRIVATE
        -Wall -Wextra -Werror -Wno-multichar -fno-exceptions -fno-rtti)

    add_test(NAME DriverSyntaxCheck COMMAND DriverSyntaxCheck)
endif()
