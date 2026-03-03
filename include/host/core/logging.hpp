#pragma once

#include <atomic>

inline bool g_debugLogging = false;

void InitLogging();
void ShutdownLogging();
void LogPrint(const char* level, bool toStderr, const char* fmt, ...);

#define LOG(f,...) LogPrint("LOG", false, f, ##__VA_ARGS__)
#define ERR(f,...) LogPrint("ERR", true, f, ##__VA_ARGS__)
#define WARN(f,...) LogPrint("WARN", true, f, ##__VA_ARGS__)
#define DBG(f,...) do { if(g_debugLogging) LogPrint("DBG", false, f, ##__VA_ARGS__); } while(0)
