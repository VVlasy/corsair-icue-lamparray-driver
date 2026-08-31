/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 *
 * Minimal levelled logger.
 *
 * A Windows service has no console, so diagnostics have to go somewhere
 * durable or they may as well not exist.  This writes to a rotating file and,
 * when attached to a debugger or run interactively, to stderr as well.
 */
#ifndef CLA_LOGGER_H
#define CLA_LOGGER_H

#include <string>
#include <mutex>
#include <cstdio>

namespace cla {

enum class LogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4,
    None = 5
};

class Logger
{
public:
    static Logger &Instance();

    void SetLevel(LogLevel level) { m_level = level; }
    LogLevel Level() const { return m_level; }

    /* Also mirror to stderr.  On by default when there is no log file. */
    void SetEchoToStderr(bool echo) { m_echoToStderr = echo; }

    /* Opens (or creates) the log file.  Returns false if it could not be
     * opened, in which case logging continues to stderr only -- losing logs is
     * never a reason to fail the service. */
    bool OpenFile(const std::string &path);
    void Close();

    void Write(LogLevel level, const char *format, ...);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    void RotateIfNeeded();

    std::mutex m_mutex;
    std::FILE *m_file = nullptr;
    std::string m_path;
    LogLevel m_level = LogLevel::Info;
    bool m_echoToStderr = true;
    long m_bytesWritten = 0;

    /* Rotate at 4MB, keeping one previous file.  A lighting bridge that logged
     * without bound would eventually be the biggest thing on the disk. */
    static constexpr long kMaxBytes = 4 * 1024 * 1024;
};

} // namespace cla

#define CLA_LOG_TRACE(...) ::cla::Logger::Instance().Write(::cla::LogLevel::Trace, __VA_ARGS__)
#define CLA_LOG_DEBUG(...) ::cla::Logger::Instance().Write(::cla::LogLevel::Debug, __VA_ARGS__)
#define CLA_LOG_INFO(...)  ::cla::Logger::Instance().Write(::cla::LogLevel::Info, __VA_ARGS__)
#define CLA_LOG_WARN(...)  ::cla::Logger::Instance().Write(::cla::LogLevel::Warning, __VA_ARGS__)
#define CLA_LOG_ERROR(...) ::cla::Logger::Instance().Write(::cla::LogLevel::Error, __VA_ARGS__)

#endif /* CLA_LOGGER_H */
