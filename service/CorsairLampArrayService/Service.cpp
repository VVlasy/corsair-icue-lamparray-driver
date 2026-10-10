/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Windows service host.
 *
 * The service's job after startup is mostly to stay out of the way: frames
 * arrive on the DriverLink pump thread and go straight to the backend.  The
 * main loop only polls for device and session changes, which is why it can sit
 * on a one-second wait rather than spinning.
 */

#include "Service.h"
#include "Corsair/FakeBackend.h"
#include "Corsair/IcueSdkBackend.h"
#include "Logging/Logger.h"

#include <windows.h>

#include <memory>

namespace cla {

const wchar_t *const kServiceName = L"CorsairLampArrayService";
const wchar_t *const kServiceDisplayName = L"Corsair Dynamic Lighting Bridge";

namespace {

SERVICE_STATUS g_serviceStatus{};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
ServiceOptions g_options;

void ReportStatus(DWORD state, DWORD exitCode, DWORD waitHint)
{
    static DWORD checkPoint = 1;

    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    g_serviceStatus.dwControlsAccepted =
        (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
    {
        g_serviceStatus.dwCheckPoint = 0;
    }
    else
    {
        g_serviceStatus.dwCheckPoint = checkPoint++;
    }

    if (g_statusHandle != nullptr)
    {
        SetServiceStatus(g_statusHandle, &g_serviceStatus);
    }
}

std::unique_ptr<ICorsairBackend> MakeBackend(const ServiceOptions &options)
{
    if (options.UseFakeBackend)
    {
        CLA_LOG_INFO("Using the fake backend (no iCUE, no hardware)");
        auto backend = std::make_unique<FakeBackend>();
        /* A plausible full-size keyboard, so the whole path can be exercised
         * end to end without Corsair hardware. */
        backend->AddKeyboard("fake-keyboard-1", 6, 18);
        return backend;
    }

    auto backend = std::make_unique<IcueSdkBackend>();
    backend->SetLayerPriority(options.Devices.LayerPriority);
    return backend;
}

/*
 * The main loop, shared by service and console modes.
 *
 * `stopEvent` is signalled to request shutdown.
 */
int RunLoop(const ServiceOptions &options, HANDLE stopEvent)
{
    std::unique_ptr<ICorsairBackend> backend = MakeBackend(options);
    DriverLink driver;
    DeviceManager manager(*backend, driver, options.Devices);

    std::string error;
    if (!manager.Start(error))
    {
        CLA_LOG_ERROR("Startup failed: %s", error.c_str());
        return 1;
    }

    CLA_LOG_INFO("Running. Driver interface: %s", driver.InterfacePath().c_str());

    for (;;)
    {
        /*
         * One second between polls.  Frames do not come through here -- they
         * arrive on the pump thread -- so this only paces device discovery and
         * reconnect attempts, where a second's latency is imperceptible and
         * the cost of polling faster would be real.
         */
        const DWORD result = WaitForSingleObject(stopEvent, 1000);
        if (result == WAIT_OBJECT_0)
        {
            break;
        }
        if (result != WAIT_TIMEOUT)
        {
            CLA_LOG_ERROR("Wait failed (%lu); stopping", GetLastError());
            break;
        }
        manager.Poll();
    }

    CLA_LOG_INFO("Stopping");
    manager.Stop();
    return 0;
}

void WINAPI ServiceControlHandler(DWORD control)
{
    switch (control)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        if (g_stopEvent != nullptr)
        {
            SetEvent(g_stopEvent);
        }
        break;
    default:
        break;
    }
}

void WINAPI ServiceMain(DWORD argc, LPWSTR *argv)
{
    (void)argc;
    (void)argv;

    g_statusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceControlHandler);
    if (g_statusHandle == nullptr)
    {
        return;
    }

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 5000);

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent == nullptr)
    {
        ReportStatus(SERVICE_STOPPED, GetLastError(), 0);
        return;
    }

    ReportStatus(SERVICE_RUNNING, NO_ERROR, 0);

    const int result = RunLoop(g_options, g_stopEvent);

    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
    ReportStatus(SERVICE_STOPPED, (result == 0) ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR, 0);
}

HANDLE g_consoleStopEvent = nullptr;

BOOL WINAPI ConsoleControlHandler(DWORD type)
{
    switch (type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_consoleStopEvent != nullptr)
        {
            SetEvent(g_consoleStopEvent);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

int RunService(const ServiceOptions &options)
{
    g_options = options;

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherW(table))
    {
        const DWORD code = GetLastError();
        if (code == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
        {
            /* Launched from a console rather than by the SCM.  Running
             * interactively is almost certainly what was meant. */
            CLA_LOG_INFO("Not started by the service controller; running in the console");
            return RunConsole(options);
        }
        CLA_LOG_ERROR("StartServiceCtrlDispatcher failed (%lu)", code);
        return 1;
    }
    return 0;
}

int RunConsole(const ServiceOptions &options)
{
    g_consoleStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_consoleStopEvent == nullptr)
    {
        CLA_LOG_ERROR("CreateEvent failed (%lu)", GetLastError());
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleControlHandler, TRUE);

    CLA_LOG_INFO("Press Ctrl+C to stop");
    const int result = RunLoop(options, g_consoleStopEvent);

    SetConsoleCtrlHandler(ConsoleControlHandler, FALSE);
    CloseHandle(g_consoleStopEvent);
    g_consoleStopEvent = nullptr;
    return result;
}

bool InstallService(std::string &error)
{
    error.clear();

    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
    {
        error = "GetModuleFileName failed";
        return false;
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (manager == nullptr)
    {
        error = "OpenSCManager failed; run elevated";
        return false;
    }

    std::wstring commandLine = L"\"";
    commandLine += modulePath;
    commandLine += L"\" --service";

    SC_HANDLE service = CreateServiceW(
        manager, kServiceName, kServiceDisplayName,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        commandLine.c_str(), nullptr, nullptr, nullptr,
        /* LocalSystem: the driver's device interface is restricted to SYSTEM
         * and Administrators by the INF's SDDL. */
        nullptr, nullptr);

    if (service == nullptr)
    {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        error = (code == ERROR_SERVICE_EXISTS) ? "service is already installed"
                                               : "CreateService failed";
        return false;
    }

    /* Restart on failure: the service is a background bridge and a crash
     * should not silently leave lighting dead until the next reboot. */
    SERVICE_FAILURE_ACTIONSW actions{};
    SC_ACTION restartActions[3];
    restartActions[0] = { SC_ACTION_RESTART, 5000 };
    restartActions[1] = { SC_ACTION_RESTART, 15000 };
    restartActions[2] = { SC_ACTION_RESTART, 60000 };
    actions.dwResetPeriod = 86400;
    actions.cActions = 3;
    actions.lpsaActions = restartActions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &actions);

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return true;
}

bool UninstallService(std::string &error)
{
    error.clear();

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        error = "OpenSCManager failed; run elevated";
        return false;
    }

    SC_HANDLE service = OpenServiceW(manager, kServiceName, DELETE | SERVICE_STOP);
    if (service == nullptr)
    {
        CloseServiceHandle(manager);
        error = "service is not installed";
        return false;
    }

    SERVICE_STATUS status{};
    ControlService(service, SERVICE_CONTROL_STOP, &status);

    const BOOL deleted = DeleteService(service);
    CloseServiceHandle(service);
    CloseServiceHandle(manager);

    if (!deleted)
    {
        error = "DeleteService failed";
        return false;
    }
    return true;
}

} // namespace cla
