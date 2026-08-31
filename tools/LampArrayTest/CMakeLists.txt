# Windows only: uses the Windows.Devices.Lights WinRT API via C++/WinRT, which
# ships with the Windows SDK.
if(WIN32 AND MSVC)
    add_executable(LampArrayTest main.cpp)
    target_compile_features(LampArrayTest PRIVATE cxx_std_17)
    target_link_libraries(LampArrayTest PRIVATE windowsapp)
endif()
