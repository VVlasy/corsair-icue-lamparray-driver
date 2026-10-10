# Windows only: drives the real iCUE SDK.
if(WIN32)
    add_executable(CorsairProbe main.cpp
        ${CMAKE_SOURCE_DIR}/service/CorsairLampArrayService/Corsair/IcueSdkBackend.cpp)
    target_include_directories(CorsairProbe PRIVATE
        ${CMAKE_SOURCE_DIR}/service/CorsairLampArrayService
        ${CMAKE_SOURCE_DIR}/inc)
    target_link_libraries(CorsairProbe PRIVATE cla_backend cla_core cla_mapping)
endif()
