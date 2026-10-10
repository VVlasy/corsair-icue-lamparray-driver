# Windows only: uses the Windows.Devices.Lights WinRT API via C++/WinRT, which
# ships with the Windows SDK.
if(WIN32 AND MSVC)
    add_executable(LampArrayTest main.cpp)
    # C++/WinRT coroutines need C++20: under C++17 it falls back to
    # <experimental/coroutine>, which current MSVC rejects (STL1011).
    target_compile_features(LampArrayTest PRIVATE cxx_std_20)
    target_link_libraries(LampArrayTest PRIVATE windowsapp)
endif()
