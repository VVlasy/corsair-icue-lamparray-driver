/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * CorsairLampArrayService entry point and command line.
 */

#include "Service.h"
#include "Logging/Logger.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void PrintUsage()
{
    std::printf(
        "CorsairLampArrayService -- Corsair Dynamic Lighting Bridge\n"
        "\n"
        "Exposes Corsair RGB devices to Windows Dynamic Lighting as HID\n"
        "LampArray devices, via the CorsairLampArray driver and the iCUE SDK.\n"
        "\n"
        "Usage:\n"
        "  CorsairLampArrayService [options]\n"
        "\n"
        "Modes:\n"
        "  --service              Run as a Windows service (used by the SCM).\n"
        "  --console              Run in this console; Ctrl+C to stop. Default\n"
        "                         when not started by the service controller.\n"
        "  --install              Install the Windows service (needs elevation).\n"
        "  --uninstall            Remove the Windows service (needs elevation).\n"
        "\n"
        "Options:\n"
        "  --fake                 Use the built-in fake backend instead of iCUE.\n"
        "                         Exposes a synthetic keyboard so the driver and\n"
        "                         Windows integration can be verified with no\n"
        "                         Corsair hardware present.\n"
        "  --exclusive            Take exclusive lighting control from iCUE.\n"
        "                         Off by default: shared control lets iCUE keep\n"
        "                         running the user's own profiles underneath.\n"
        "  --priority <0-255>     SDK layer priority (default 128; iCUE is 127).\n"
        "  --interval <us>        Minimum update interval in microseconds\n"
        "                         (default 16667, i.e. 60Hz).\n"
        "  --log <path>           Write a log file to this path.\n"
        "  --verbose              Log at debug level.\n"
        "  --trace                Log at trace level.\n"
        "  --help                 Show this help.\n");
}

/* %ProgramData%\CorsairLampArray\service.log, or empty if unavailable. */
std::string DefaultLogPath()
{
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)))
    {
        return std::string();
    }

    std::wstring directory(programData);
    CoTaskMemFree(programData);
    directory += L"\\CorsairLampArray";
    CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring path = directory + L"\\service.log";
    const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return std::string();
    }
    std::string narrow(static_cast<size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow.data(), size,
                        nullptr, nullptr);
    return narrow;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    cla::ServiceOptions options;
    bool runAsService = false;
    bool runConsole = false;
    bool install = false;
    bool uninstall = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::wstring argument = argv[i];
        auto next = [&](unsigned long &out) -> bool {
            if (i + 1 >= argc) return false;
            out = wcstoul(argv[++i], nullptr, 10);
            return true;
        };

        if (argument == L"--service")        runAsService = true;
        else if (argument == L"--console")   runConsole = true;
        else if (argument == L"--install")   install = true;
        else if (argument == L"--uninstall") uninstall = true;
        else if (argument == L"--fake")      options.UseFakeBackend = true;
        else if (argument == L"--exclusive") options.Devices.ExclusiveControl = true;
        else if (argument == L"--verbose")   options.LogLevel = cla::LogLevel::Debug;
        else if (argument == L"--trace")     options.LogLevel = cla::LogLevel::Trace;
        else if (argument == L"--priority")
        {
            unsigned long value = 0;
            if (!next(value)) { std::fprintf(stderr, "--priority needs a value\n"); return 2; }
            options.Devices.LayerPriority = static_cast<unsigned int>(value);
        }
        else if (argument == L"--interval")
        {
            unsigned long value = 0;
            if (!next(value)) { std::fprintf(stderr, "--interval needs a value\n"); return 2; }
            options.Devices.MinUpdateIntervalMicroseconds = static_cast<cla_u32>(value);
        }
        else if (argument == L"--log")
        {
            if (i + 1 >= argc) { std::fprintf(stderr, "--log needs a path\n"); return 2; }
            const std::wstring path = argv[++i];
            const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                                 nullptr, 0, nullptr, nullptr);
            if (size > 0)
            {
                options.LogPath.resize(static_cast<size_t>(size) - 1);
                WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1,
                                    options.LogPath.data(), size, nullptr, nullptr);
            }
        }
        else if (argument == L"--help" || argument == L"-h" || argument == L"/?")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::fwprintf(stderr, L"Unknown option: %ls\n\n", argument.c_str());
            PrintUsage();
            return 2;
        }
    }

    if (options.LogPath.empty())
    {
        options.LogPath = DefaultLogPath();
    }

    cla::Logger::Instance().SetLevel(options.LogLevel);
    if (!options.LogPath.empty())
    {
        if (!cla::Logger::Instance().OpenFile(options.LogPath))
        {
            std::fprintf(stderr, "Warning: could not open log file %s; "
                                 "continuing with console logging only\n",
                         options.LogPath.c_str());
        }
    }
    /* A service has no console; echoing there is only useful interactively. */
    cla::Logger::Instance().SetEchoToStderr(!runAsService);

    if (install && uninstall)
    {
        std::fprintf(stderr, "--install and --uninstall are mutually exclusive\n");
        return 2;
    }

    if (install)
    {
        std::string error;
        if (!cla::InstallService(error))
        {
            std::fprintf(stderr, "Install failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("Service installed. Start it with: sc start CorsairLampArrayService\n");
        return 0;
    }

    if (uninstall)
    {
        std::string error;
        if (!cla::UninstallService(error))
        {
            std::fprintf(stderr, "Uninstall failed: %s\n", error.c_str());
            return 1;
        }
        std::printf("Service removed.\n");
        return 0;
    }

    if (runConsole)
    {
        return cla::RunConsole(options);
    }
    if (runAsService)
    {
        return cla::RunService(options);
    }

    /* No mode given: try the service dispatcher, which falls back to console
     * mode when we were not launched by the SCM. */
    return cla::RunService(options);
}
