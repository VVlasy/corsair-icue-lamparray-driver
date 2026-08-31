# The user-mode service.
#
# Split so that everything testable without Windows actually gets tested:
#
#   cla_backend  - portable: the ICorsairBackend abstraction, the fake backend,
#                  the frame pipeline (coalescing, dirty-lamp tracking,
#                  brightness).  Built and tested everywhere.
#
#   the service  - Windows only: the service host, the driver link, and the
#                  iCUE SDK backend.
add_library(cla_backend STATIC
    CorsairLampArrayService/Corsair/FakeBackend.cpp
    CorsairLampArrayService/Corsair/BackendTypes.cpp
    CorsairLampArrayService/Devices/FramePipeline.cpp
    CorsairLampArrayService/Logging/Logger.cpp
)
target_include_directories(cla_backend PUBLIC CorsairLampArrayService)
target_link_libraries(cla_backend PUBLIC cla_core cla_mapping)
set_target_properties(cla_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    add_executable(CorsairLampArrayService
        CorsairLampArrayService/Program.cpp
        CorsairLampArrayService/Service.cpp
        CorsairLampArrayService/Ipc/DriverLink.cpp
        CorsairLampArrayService/Corsair/IcueSdkBackend.cpp
        CorsairLampArrayService/Devices/DeviceManager.cpp
    )
    target_include_directories(CorsairLampArrayService PRIVATE
        CorsairLampArrayService
        ${CMAKE_SOURCE_DIR}/inc
    )
    target_link_libraries(CorsairLampArrayService PRIVATE
        cla_backend cla_core cla_mapping
        setupapi cfgmgr32
    )
endif()
