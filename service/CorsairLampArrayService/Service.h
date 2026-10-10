/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Windows service host.
 */
#ifndef CLA_SERVICE_H
#define CLA_SERVICE_H

#include "Devices/DeviceManager.h"
#include "Logging/Logger.h"

#include <string>

namespace cla {

struct ServiceOptions
{
    DeviceManagerOptions Devices;
    /* Use FakeBackend instead of iCUE.  Lets the whole pipeline be exercised
     * on a machine with no Corsair hardware (project Phase 4: verify Windows
     * recognition before Corsair integration). */
    bool UseFakeBackend = false;
    std::string LogPath;
    /* Qualified: the member shares the enum's name, so an unqualified
     * LogLevel:: in the initialiser would name the member, not the type. */
    cla::LogLevel LogLevel = cla::LogLevel::Info;
    /* Run in the console instead of as a service. */
    bool Console = false;
};

/* Runs until stopped.  Returns a process exit code. */
int RunService(const ServiceOptions &options);
int RunConsole(const ServiceOptions &options);

/* Service control plumbing. */
bool InstallService(std::string &error);
bool UninstallService(std::string &error);

extern const wchar_t *const kServiceName;
extern const wchar_t *const kServiceDisplayName;

} // namespace cla

#endif /* CLA_SERVICE_H */
