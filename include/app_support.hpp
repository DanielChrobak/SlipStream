#pragma once
#include "common.hpp"

struct Config { std::string username, passwordHash, salt; };

extern std::atomic<bool> g_running;
extern std::atomic<bool> g_exitRequested;
extern Config g_config;
extern JWTAuth g_jwt;
extern RateLimiter g_rateLimiter;

BOOL WINAPI ConsoleHandler(DWORD sig);
void RefreshMonitorList();
std::string LoadFile(const char* path);
void SetupConfig();
void HandleAuth(const httplib::Request& req, httplib::Response& res);
void SetupCORS(const httplib::Request& req, httplib::Response& r);
std::vector<std::string> GetLocalIPAddresses();
void JsonError(httplib::Response& res, int status, const std::string& err);
std::function<void(const httplib::Request&, httplib::Response&)> AuthRequired(
    std::function<void(const httplib::Request&, httplib::Response&, const std::string&)> h);
