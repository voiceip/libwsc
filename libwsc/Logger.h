/*
 *  Logger.h
 *  Dual-mode logger: FreeSWITCH, console or syslog
 * 
 *  Author: Milan M.
 *  Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <cstdio>
#include <cstring>
#include <syslog.h>
#include <unistd.h>
#include <sys/time.h>
#include <ctime>

// Check if FreeSWITCH headers are available
#ifdef SWITCH_LOG_DEBUG
    #include <switch.h>
    #define LIBWSC_USE_FREESWITCH_LOG 1
#else
    #define LIBWSC_USE_FREESWITCH_LOG 0
#endif

/**
 * \brief Determines if logs should go to syslog (non-interactive) or console.
 */
inline bool logger_use_syslog() {
    static bool initialized = false;
    static bool use_syslog = false;
    if (!initialized) {
        initialized = true;
        use_syslog = !isatty(STDOUT_FILENO);
        if (use_syslog) openlog(nullptr, LOG_PID, LOG_DAEMON);
    }
    return use_syslog;
}

/**
 * \brief Fill buf with current timestamp "YYYY-MM-DD HH:MM:SS.mmm".
 */
inline void current_timestamp(char* buf, size_t len) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    int ms = tv.tv_usec / 1000;
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_info);
    size_t sl = strlen(buf);
    snprintf(buf + sl, len - sl, ".%03d", ms);
}

/**
 * \brief Internal debug logger: writes to FreeSWITCH, stdout or syslog.
 */
inline void log_debug_impl(const char* message) {
#ifdef LIBWSC_USE_DEBUG
#if LIBWSC_USE_FREESWITCH_LOG
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "mod_audio_stream[libwsc]: %s\n", message);
#else
    char ts[32]; current_timestamp(ts, sizeof(ts));
    if (logger_use_syslog()) {
        syslog(LOG_DEBUG, "%s", message);
    } else {
        fprintf(stdout, "[DEBUG %s] %s\n", ts, message);
        fflush(stdout);
    }
#endif
#else
    (void)message;
#endif
}

/**
 * \brief Internal error logger: writes to FreeSWITCH, stderr or syslog.
 */
inline void log_error_impl(const char* message) {
#if LIBWSC_USE_FREESWITCH_LOG
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_audio_stream[libwsc]: %s\n", message);
#else
    char ts[32]; current_timestamp(ts, sizeof(ts));
    if (logger_use_syslog()) {
        syslog(LOG_ERR, "%s", message);
    } else {
        fprintf(stderr, "[ERROR %s] %s\n", ts, message);
        fflush(stderr);
    }
#endif
}

/**
 * \brief Internal info logger: writes to FreeSWITCH, stdout or syslog.
 * Always enabled (not dependent on LIBWSC_USE_DEBUG).
 */
inline void log_info_impl(const char* message) {
#if LIBWSC_USE_FREESWITCH_LOG
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "mod_audio_stream[libwsc]: %s\n", message);
#else
    char ts[32]; current_timestamp(ts, sizeof(ts));
    if (logger_use_syslog()) {
        syslog(LOG_INFO, "%s", message);
    } else {
        fprintf(stdout, "[INFO %s] %s\n", ts, message);
        fflush(stdout);
    }
#endif
}

// Zero-arg overloads to avoid format-security warnings
inline void log_debug_fmt_impl(const char* fmt) {
    log_debug_impl(fmt);
}
inline void log_error_fmt_impl(const char* fmt) {
    log_error_impl(fmt);
}
inline void log_info_fmt_impl(const char* fmt) {
    log_info_impl(fmt);
}

// Templated overloads for formatting
template<typename... Args>
inline void log_debug_fmt_impl(const char* fmt, Args... args) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, args...);
    log_debug_impl(buf);
}

template<typename... Args>
inline void log_error_fmt_impl(const char* fmt, Args... args) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, args...);
    log_error_impl(buf);
}

template<typename... Args>
inline void log_info_fmt_impl(const char* fmt, Args... args) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, args...);
    log_info_impl(buf);
}

// Public macros
#ifdef LIBWSC_USE_DEBUG
    #define log_debug(...)   log_debug_fmt_impl(__VA_ARGS__)
#else
    #define log_debug(...)   ((void)0)
#endif

#define log_error(...)     log_error_fmt_impl(__VA_ARGS__)
#define log_info(...)      log_info_fmt_impl(__VA_ARGS__)

#endif // LOGGER_H