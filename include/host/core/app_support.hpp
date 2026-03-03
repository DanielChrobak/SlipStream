#pragma once
#include "host/core/common.hpp"

struct Config { std::string username, passwordHash, salt; };

struct AppContext {
    std::atomic<bool> running{true};
    std::atomic<bool> exitRequested{false};
    Config config;
    JWTAuth jwt;
    RateLimiter rateLimiter;
};

AppContext& GetAppContext();

BOOL WINAPI ConsoleHandler(DWORD sig);
void RefreshMonitorList();
std::string LoadFile(const char* path);
void SetupConfig();
void HandleAuth(const httplib::Request& req, httplib::Response& res);
void SetupCORS(const httplib::Request& req, httplib::Response& r);
void JsonError(httplib::Response& res, int status, const std::string& err);
std::function<void(const httplib::Request&, httplib::Response&)> AuthRequired(
    std::function<void(const httplib::Request&, httplib::Response&, const std::string&)> h);
