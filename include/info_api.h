#pragma once

#include <crow.h>
#include "info_store.h"

inline void set_json_headers(crow::response &r) {
    r.set_header("Content-Type", "application/json");
    r.set_header("Access-Control-Allow-Origin", "*");
}

// 从原始 JSON 文本中提取字符串字段的极简解析器（仅用于受控 demo 请求）
static std::optional<std::string> extract_string_field(const std::string &body, const std::string &key) {
    std::string k = '"' + key + '"';
    auto pos = body.find(k);
    if (pos == std::string::npos) return std::nullopt;
    pos = body.find(':', pos + k.size());
    if (pos == std::string::npos) return std::nullopt;
    pos = body.find('"', pos);
    if (pos == std::string::npos) return std::nullopt;
    size_t start = pos + 1;
    std::string out;
    for (size_t i = start; i < body.size(); ++i) {
        char c = body[i];
        if (c == '"') return out;
        if (c == '\\' && i + 1 < body.size()) {
            char n = body[++i];
            switch (n) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += n; break;
            }
            continue;
        }
        out += c;
    }
    return std::nullopt;
}

inline void register_info_routes(crow::SimpleApp &app, InfoStore &store) {
    // 列表
    CROW_ROUTE(app, "/api/projects/info.json").methods(crow::HTTPMethod::GET)([&store]() {
        try {
            auto arr = store.list_sorted();
            std::string body = "[";
            for (size_t i = 0; i < arr.size(); ++i) {
                body += arr[i].to_json();
                if (i + 1 < arr.size()) body += ",";
            }
            body += "]";
            crow::response r{body};
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 500;
            set_json_headers(r);
            return r;
        }
    });

    // 获取单个
    CROW_ROUTE(app, "/api/projects/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            auto obj = store.get(uuid);
            crow::response r{obj.to_json()};
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"project not found\"}")};
            r.code = 404;
            set_json_headers(r);
            return r;
        }
    });

    // 删除
    CROW_ROUTE(app, "/api/projects/<string>").methods(crow::HTTPMethod::DELETE)([&store](const std::string &uuid){
        try {
            bool ok = store.remove(uuid);
            if (!ok) {
                crow::response r{std::string("{\"error\":\"not found\"}")};
                r.code = 404;
                set_json_headers(r);
                return r;
            }
            crow::response r{""};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 500;
            set_json_headers(r);
            return r;
        }
    });

    // 创建（POST）
    CROW_ROUTE(app, "/api/projects").methods(crow::HTTPMethod::POST)([&store](const crow::request &req){
        try {
            auto maybe_name = extract_string_field(req.body, "name");
            if (!maybe_name) {
                crow::response r{std::string("{\"error\":\"invalid json or missing name\"}")};
                r.code = 400; set_json_headers(r); return r;
            }
            auto maybe_note = extract_string_field(req.body, "note");
            std::string note = maybe_note ? *maybe_note : std::string("");
            auto created = store.create(*maybe_name, note);
            crow::response r{created.to_json()}; r.code = 201; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"}; r.code = 400; set_json_headers(r); return r;
        }
    });

    // 更新（PATCH）
    CROW_ROUTE(app, "/api/projects/<string>").methods(crow::HTTPMethod::PATCH)([&store](const crow::request &req, const std::string &uuid){
        try {
            auto maybe_note = extract_string_field(req.body, "note");
            if (!maybe_note) {
                crow::response r{std::string("{\"error\":\"invalid json\"}")}; r.code = 400; set_json_headers(r); return r;
            }
            auto updated = store.patch(uuid, *maybe_note);
            crow::response r{updated.to_json()}; r.code = 200; set_json_headers(r); return r;
        } catch (const std::runtime_error &re) {
            crow::response r{std::string("{\"error\":\"") + std::string(re.what()) + "\"}"}; r.code = 404; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"}; r.code = 400; set_json_headers(r); return r;
        }
    });

    // CORS 预检（OPTIONS）
    CROW_ROUTE(app, "/api/projects/<string>").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/projects").methods(crow::HTTPMethod::OPTIONS)([](){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });
}
