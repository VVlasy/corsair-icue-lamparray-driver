# Windows only: opens real HID devices.
if(WIN32)
    add_executable(HidTestClient main.cpp
        ${CMAKE_SOURCE_DIR}/service/CorsairLampArrayService/Ipc/DriverLink.cpp)
    target_include_directories(HidTestClient PRIVATE
        ${CMAKE_SOURCE_DIR}/service/CorsairLampArrayService
        ${CMAKE_SOURCE_DIR}/inc)
    target_link_libraries(HidTestClient PRIVATE
        cla_backend cla_core cla_mapping
        setupapi hid cfgmgr32)
endif()
