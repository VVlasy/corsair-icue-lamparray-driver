/*
 * Copyright (c) Corsair Dynamic Lighting Bridge contributors.
 * SPDX-License-Identifier: MIT
 */

#include "Logging/Logger.h"

#include <cstdarg>
#include <ctime>
#include <cstring>

namespace cla {

Logger &Logger::Instance()
{
    static Logger logger;
    return logger;
}

Logger::~Logger()
{
    Close();
}

bool Logger::OpenFile(const std::string &path)
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_file != nullptr)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }

    m_path = path;
    m_file = std::fopen(path.c_str(), "ab");
    if (m_file == nullptr)
    {
        return false;
    }

    std::fseek(m_file, 0, SEEK_END);
    m_bytesWritten = std::ftell(m_file);
    return true;
}

void Logger::Close()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_file != nullptr)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
}

void Logger::RotateIfNeeded()
{
    /* Caller holds the mutex. */
    if (m_file == nullptr || m_bytesWritten < kMaxBytes || m_path.empty())
    {
        return;
    }

    std::fclose(m_file);
    m_file = nullptr;

    const std::string previous = m_path + ".1";
    std::remove(previous.c_str());
    std::rename(m_path.c_str(), previous.c_str());

    m_file = std::fopen(m_path.c_str(), "wb");
    m_bytesWritten = 0;
}

void Logger::Write(LogLevel level, const char *format, ...)
{
    if (level < m_level || level == LogLevel::None)
    {
        return;
    }

    static const char *kNames[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
    const char *name = kNames[static_cast<int>(level)];

    char message[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &parts);

    std::lock_guard<std::mutex> guard(m_mutex);

    if (m_file != nullptr)
    {
        RotateIfNeeded();
    }
    if (m_file != nullptr)
    {
        const int written = std::fprintf(m_file, "%s %s %s\n", stamp, name, message);
        if (written > 0)
        {
            m_bytesWritten += written;
        }
        std::fflush(m_file);
    }
    if (m_echoToStderr || m_file == nullptr)
    {
        std::fprintf(stderr, "%s %s %s\n", stamp, name, message);
    }
}

} // namespace cla
