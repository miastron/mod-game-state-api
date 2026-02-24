/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "HttpGameStateServer.h"
#include "GameStateAPI.h"
#include "GameStateUtilities.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "World.h"
#include "GameTime.h"
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#endif

using json = nlohmann::json;

HttpGameStateServer::HttpGameStateServer(const std::string& host, uint16 port, const std::string& allowedOrigin)
    : _host(host), _port(port), _allowedOrigin(allowedOrigin), _running(false)
{
    _server = std::make_unique<httplib::Server>();

    // Set up CORS middleware for all requests
    _server->set_pre_routing_handler([this](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Handle OPTIONS requests for CORS preflight
    _server->Options(".*", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        SetCorsHeaders(res);
        res.status = 200;
    });

    // API endpoints
    _server->Get("/api/health", [this](const httplib::Request& req, httplib::Response& res) {
        HandleHealthCheck(req, res);
    });

    _server->Get("/api/server", [this](const httplib::Request& req, httplib::Response& res) {
        HandleServerInfo(req, res);
    });

    _server->Get("/api/host", [this](const httplib::Request& req, httplib::Response& res) {
        HandleHostInfo(req, res);
    });

    _server->Get("/api/players", [this](const httplib::Request& req, httplib::Response& res) {
        HandleOnlinePlayers(req, res);
    });

    _server->Get("/api/player/([^/]+)", [this](const httplib::Request& req, httplib::Response& res) {
        HandlePlayerInfo(req, res);
    });

    _server->Get("/api/player/([^/]+)/stats", [this](const httplib::Request& req, httplib::Response& res) {
        HandlePlayerStats(req, res);
    });

    _server->Get("/api/player/([^/]+)/equipment", [this](const httplib::Request& req, httplib::Response& res) {
        HandlePlayerEquipment(req, res);
    });

    _server->Get("/api/player/([^/]+)/skills", [this](const httplib::Request& req, httplib::Response& res) {
        HandlePlayerSkills(req, res);
    });

    _server->Get("/api/player/([^/]+)/skills-full", [this](const httplib::Request& req, httplib::Response& res) {
        HandlePlayerSkillsFull(req, res);
    });

    _server->Get("/api/player/([^/]+)/quests", [this](const httplib::Request& req, httplib::Response& res) {
        HandlePlayerQuests(req, res);
    });

    // Set up CORS and error handling
    _server->set_pre_routing_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        return httplib::Server::HandlerResponse::Unhandled;
    });
}

HttpGameStateServer::~HttpGameStateServer()
{
    Stop();
}

bool HttpGameStateServer::Start()
{
    if (_running.load())
    {
        LOG_WARN("module.gamestate_api", "HTTP server is already running");
        return false;
    }

    _serverThread = std::make_unique<std::thread>([this]() {
        LOG_INFO("module.gamestate_api", "Starting HTTP server on {}:{}", _host, _port);
        _running.store(true);

        if (!_server->listen(_host, _port))
        {
            LOG_ERROR("module.gamestate_api", "Failed to start HTTP server on {}:{}", _host, _port);
            _running.store(false);
        }
        else
        {
            LOG_INFO("module.gamestate_api", "HTTP server stopped");
            _running.store(false);
        }
    });

    // Give the server a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (_running.load())
    {
        LOG_INFO("module.gamestate_api", "Game State API HTTP server started successfully on {}:{}", _host, _port);
        return true;
    }
    else
    {
        LOG_ERROR("module.gamestate_api", "Failed to start Game State API HTTP server");
        if (_serverThread && _serverThread->joinable())
        {
            _serverThread->join();
        }
        return false;
    }
}

void HttpGameStateServer::Stop()
{
    if (!_running.load())
    {
        return;
    }

    LOG_INFO("module.gamestate_api", "Stopping HTTP server...");

    if (_server)
    {
        _server->stop();
    }

    if (_serverThread && _serverThread->joinable())
    {
        _serverThread->join();
    }

    _running.store(false);
    LOG_INFO("module.gamestate_api", "HTTP server stopped");
}

void HttpGameStateServer::HandleHealthCheck(const httplib::Request& /*req*/, httplib::Response& res)
{
    json response = {
        {"status", "ok"},
        {"timestamp", std::time(nullptr)},
        {"uptime_seconds", GameTime::GetUptime().count()}
    };

    SendJsonResponse(res, response.dump(2));
}

void HttpGameStateServer::HandleHostInfo(const httplib::Request& /*req*/, httplib::Response& res)
{
    // Static variables to track peak values since server start
    static double peakCpuUsage = 0.0;
    static uint64_t peakMemUsage = 0;

    try
    {
        json response;
        double currentCpuUsage = 0.0;
        uint64_t currentMemUsage = 0;

#ifdef _WIN32
        // Windows implementation

        // Host uptime
        ULONGLONG uptimeMs = GetTickCount64();
        response["uptime_seconds"] = uptimeMs / 1000;

        // Get current CPU usage using GetSystemTimes
        FILETIME idleTime, kernelTime, userTime;
        static FILETIME prevIdleTime = {0}, prevKernelTime = {0}, prevUserTime = {0};
        static bool firstCall = true;

        if (GetSystemTimes(&idleTime, &kernelTime, &userTime))
        {
            if (firstCall)
            {
                prevIdleTime = idleTime;
                prevKernelTime = kernelTime;
                prevUserTime = userTime;
                firstCall = false;
                currentCpuUsage = 0.0;
            }
            else
            {
                ULONGLONG idle = (((ULONGLONG)idleTime.dwHighDateTime << 32) | idleTime.dwLowDateTime) -
                                 (((ULONGLONG)prevIdleTime.dwHighDateTime << 32) | prevIdleTime.dwLowDateTime);
                ULONGLONG kernel = (((ULONGLONG)kernelTime.dwHighDateTime << 32) | kernelTime.dwLowDateTime) -
                                   (((ULONGLONG)prevKernelTime.dwHighDateTime << 32) | prevKernelTime.dwLowDateTime);
                ULONGLONG user = (((ULONGLONG)userTime.dwHighDateTime << 32) | userTime.dwLowDateTime) -
                                 (((ULONGLONG)prevUserTime.dwHighDateTime << 32) | prevUserTime.dwLowDateTime);

                ULONGLONG total = kernel + user;
                currentCpuUsage = (total > 0) ? (1.0 - ((double)idle / (double)total)) * 100.0 : 0.0;
                currentCpuUsage = std::round(currentCpuUsage * 100.0) / 100.0;

                prevIdleTime = idleTime;
                prevKernelTime = kernelTime;
                prevUserTime = userTime;
            }
        }

        // Memory information
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo))
        {
            currentMemUsage = memInfo.ullTotalPhys - memInfo.ullAvailPhys;
        }

#else
        // Linux implementation

        // Host uptime
        struct sysinfo si;
        if (sysinfo(&si) == 0)
        {
            response["uptime_seconds"] = si.uptime;
        }
        else
        {
            response["uptime_seconds"] = 0;
        }

        // Memory usage from /proc/meminfo (container-aware on Proxmox)
        {
            unsigned long long memTotal = 0, memAvailable = 0;
            std::ifstream memInfoFile("/proc/meminfo");
            if (memInfoFile.is_open())
            {
                std::string line;
                while (std::getline(memInfoFile, line))
                {
                    unsigned long long value;
                    if (sscanf(line.c_str(), "MemTotal: %llu kB", &value) == 1)
                        memTotal = value * 1024ULL;
                    else if (sscanf(line.c_str(), "MemAvailable: %llu kB", &value) == 1)
                        memAvailable = value * 1024ULL;
                }
                memInfoFile.close();
            }

            response["total_mem"] = memTotal;
            currentMemUsage = memTotal - memAvailable;
        }

        // CPU usage from /proc/stat
        static unsigned long long prevTotal = 0, prevIdle = 0;
        static bool firstCall = true;

        std::ifstream statFile("/proc/stat");
        if (statFile.is_open())
        {
            std::string line;
            std::getline(statFile, line);
            statFile.close();

            unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
            if (sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) >= 4)
            {
                unsigned long long totalIdle = idle + iowait;
                unsigned long long total = user + nice + system + idle + iowait + irq + softirq + steal;

                if (firstCall)
                {
                    prevTotal = total;
                    prevIdle = totalIdle;
                    firstCall = false;
                    currentCpuUsage = 0.0;
                }
                else
                {
                    unsigned long long totalDiff = total - prevTotal;
                    unsigned long long idleDiff = totalIdle - prevIdle;

                    currentCpuUsage = (totalDiff > 0) ? (1.0 - ((double)idleDiff / (double)totalDiff)) * 100.0 : 0.0;
                    currentCpuUsage = std::round(currentCpuUsage * 100.0) / 100.0;

                    prevTotal = total;
                    prevIdle = totalIdle;
                }
            }
        }
#endif

        // Update peak values
        if (currentCpuUsage > peakCpuUsage)
        {
            peakCpuUsage = currentCpuUsage;
        }
        if (currentMemUsage > peakMemUsage)
        {
            peakMemUsage = currentMemUsage;
        }

        // Build response
        response["current_cpu"] = currentCpuUsage;
        response["max_cpu"] = peakCpuUsage;
        response["current_mem"] = currentMemUsage;
        response["max_mem"] = peakMemUsage;

        response["timestamp"] = std::time(nullptr);
        SendJsonResponse(res, response.dump(2));
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.gamestate_api", "Error getting host info: {}", e.what());
        json error = {{"error", "Internal server error"}, {"status", 500}};
        SendJsonResponse(res, error.dump(2), 500);
    }
}

void HttpGameStateServer::HandleServerInfo(const httplib::Request& /*req*/, httplib::Response& res)
{
    try
    {
        json serverData = GameStateUtilities::GetServerData();
        SendJsonResponse(res, serverData.dump(2));
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.gamestate_api", "Error getting server info: {}", e.what());
        json error = {{"error", "Internal server error"}, {"status", 500}};
        SendJsonResponse(res, error.dump(2), 500);
    }
}

void HttpGameStateServer::HandleOnlinePlayers(const httplib::Request& req, httplib::Response& res)
{
    try
    {
        // Check for equipment parameter
        bool includeEquipment = req.has_param("equipment") && req.get_param_value("equipment") == "true";

        json playersData = GameStateUtilities::GetAllPlayersData(includeEquipment);

        json response = {
            {"count", playersData.size()},
            {"players", playersData}
        };

        SendJsonResponse(res, response.dump(2));
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("module.gamestate_api", "Error getting players list: {}", e.what());
        json error = {{"error", "Internal server error"}, {"status", 500}};
        SendJsonResponse(res, error.dump(2), 500);
    }
}

void HttpGameStateServer::HandlePlayerInfo(const httplib::Request& req, httplib::Response& res)
{
    std::string playerName = req.matches[1];

    if (playerName.empty())
    {
        SendErrorResponse(res, "Player name is required", 400);
        return;
    }

    // Find the player using GameStateUtilities
    Player* player = GameStateUtilities::FindPlayerByName(playerName);
    if (!player || !player->IsInWorld())
    {
        SendErrorResponse(res, "Player not found or not online", 404);
        return;
    }

    // Check if equipment should be included
    bool includeEquipment = req.has_param("include") &&
                           req.get_param_value("include").find("equipment") != std::string::npos;

    // Get player data using GameStateUtilities
    json playerJson = GameStateUtilities::GetPlayerData(player, includeEquipment);

    SendJsonResponse(res, playerJson.dump());
}

void HttpGameStateServer::HandlePlayerStats(const httplib::Request& req, httplib::Response& res)
{
    std::string playerName = req.matches[1];

    if (playerName.empty())
    {
        SendErrorResponse(res, "Player name is required", 400);
        return;
    }

    Player* player = GameStateUtilities::FindPlayerByName(playerName);
    if (!player || !player->IsInWorld())
    {
        SendErrorResponse(res, "Player not found or not online", 404);
        return;
    }

    json statsJson = GameStateUtilities::GetPlayerStats(player);
    SendJsonResponse(res, statsJson.dump());
}

void HttpGameStateServer::HandlePlayerEquipment(const httplib::Request& req, httplib::Response& res)
{
    std::string playerName = req.matches[1];

    if (playerName.empty())
    {
        SendErrorResponse(res, "Player name is required", 400);
        return;
    }

    Player* player = GameStateUtilities::FindPlayerByName(playerName);
    if (!player || !player->IsInWorld())
    {
        SendErrorResponse(res, "Player not found or not online", 404);
        return;
    }

    json equipmentJson = GameStateUtilities::GetPlayerEquipment(player);
    SendJsonResponse(res, equipmentJson.dump());
}

void HttpGameStateServer::HandlePlayerSkills(const httplib::Request& req, httplib::Response& res)
{
    std::string playerName = req.matches[1];

    if (playerName.empty())
    {
        SendErrorResponse(res, "Player name is required", 400);
        return;
    }

    Player* player = GameStateUtilities::FindPlayerByName(playerName);
    if (!player || !player->IsInWorld())
    {
        SendErrorResponse(res, "Player not found or not online", 404);
        return;
    }

    json skillsJson = GameStateUtilities::GetPlayerSkills(player);
    SendJsonResponse(res, skillsJson.dump());
}

void HttpGameStateServer::HandlePlayerSkillsFull(const httplib::Request& req, httplib::Response& res)
{
    std::string playerName = req.matches[1];

    if (playerName.empty())
    {
        SendErrorResponse(res, "Player name is required", 400);
        return;
    }

    Player* player = GameStateUtilities::FindPlayerByName(playerName);
    if (!player || !player->IsInWorld())
    {
        SendErrorResponse(res, "Player not found or not online", 404);
        return;
    }

    json skillsFullJson = GameStateUtilities::GetPlayerSkillsFull(player);
    SendJsonResponse(res, skillsFullJson.dump());
}

void HttpGameStateServer::HandlePlayerQuests(const httplib::Request& req, httplib::Response& res)
{
    std::string playerName = req.matches[1];

    if (playerName.empty())
    {
        SendErrorResponse(res, "Player name is required", 400);
        return;
    }

    Player* player = GameStateUtilities::FindPlayerByName(playerName);
    if (!player || !player->IsInWorld())
    {
        SendErrorResponse(res, "Player not found or not online", 404);
        return;
    }

    json questsJson = GameStateUtilities::GetPlayerQuests(player);
    SendJsonResponse(res, questsJson.dump());
}

void HttpGameStateServer::SetCorsHeaders(httplib::Response& res)
{
    res.set_header("Access-Control-Allow-Origin", _allowedOrigin);
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
    res.set_header("Access-Control-Max-Age", "86400");
}

void HttpGameStateServer::SendJsonResponse(httplib::Response& res, const std::string& json, int status)
{
    res.status = status;
    res.set_content(json, "application/json");
}

void HttpGameStateServer::SendErrorResponse(httplib::Response& res, const std::string& message, int status)
{
    json error = {
        {"error", message},
        {"timestamp", std::time(nullptr)}
    };
    SendJsonResponse(res, error.dump(), status);
}

