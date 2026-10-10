# Tests for everything that can be exercised without Windows, a WDK, iCUE or
# Corsair hardware.  This is the bulk of the protocol-critical logic.

function(cla_add_test target)
    add_executable(${target} ${CMAKE_CURRENT_SOURCE_DIR}/TestMain.cpp ${ARGN})
    target_link_libraries(${target} PRIVATE cla_core cla_mapping cla_backend)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${target} COMMAND ${target})
endfunction()

cla_add_test(HidTests
    HidTests/DescriptorTests.cpp
    HidTests/ReportTests.cpp
    HidTests/FuzzTests.cpp
)

cla_add_test(IpcTests
    IpcTests/ContractTests.cpp
)

cla_add_test(MappingTests
    MappingTests/MappingTests.cpp
)

cla_add_test(CorsairTests
    CorsairTests/FakeBackendTests.cpp
    CorsairTests/DeviceClassTests.cpp
)
add_subdirectory(DriverSyntax)
