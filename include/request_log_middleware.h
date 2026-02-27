#pragma once

#include <crow.h>
#include <chrono>
#include <sstream>
#include <string>

#include "runtime_logger.h"

struct RequestLogMiddleware {
    struct context {
        uint64_t request_id = 0;
        std::chrono::steady_clock::time_point start;
    };

    static std::string method_name(crow::HTTPMethod method)
    {
        switch (method) {
            case crow::HTTPMethod::GET: return "GET";
            case crow::HTTPMethod::POST: return "POST";
            case crow::HTTPMethod::PUT: return "PUT";
            case crow::HTTPMethod::DELETE: return "DELETE";
            case crow::HTTPMethod::PATCH: return "PATCH";
            case crow::HTTPMethod::OPTIONS: return "OPTIONS";
            case crow::HTTPMethod::HEAD: return "HEAD";
            default: return "UNKNOWN";
        }
    }

    void before_handle(crow::request &req, crow::response &, context &ctx)
    {
        ctx.request_id = RuntimeLogger::instance().next_request_id();
        ctx.start = std::chrono::steady_clock::now();

        std::ostringstream oss;
        oss << "[REQ#" << ctx.request_id << "] "
            << method_name(req.method) << ' ' << req.url
            << " content-type='" << req.get_header_value("Content-Type") << "'"
            << " body_bytes=" << req.body.size();
        RuntimeLogger::info(oss.str());

        if (!req.body.empty()) {
            RuntimeLogger::debug("[REQ#" + std::to_string(ctx.request_id) + "] body=" + RuntimeLogger::preview(req.body));
        }
    }

    void after_handle(crow::request &req, crow::response &res, context &ctx)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ctx.start
        ).count();

        std::ostringstream oss;
        oss << "[REQ#" << ctx.request_id << "] "
            << method_name(req.method) << ' ' << req.url
            << " -> status=" << res.code
            << " response_bytes=" << res.body.size()
            << " elapsed_ms=" << elapsed;

        if (res.code >= 500) {
            RuntimeLogger::error(oss.str());
        } else if (res.code >= 400) {
            RuntimeLogger::warn(oss.str());
        } else {
            RuntimeLogger::info(oss.str());
        }

        if (res.code >= 400 && !res.body.empty()) {
            RuntimeLogger::error("[REQ#" + std::to_string(ctx.request_id) + "] error_body=" + RuntimeLogger::preview(res.body));
        }
    }
};
