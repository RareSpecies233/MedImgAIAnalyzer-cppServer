#pragma once

#include <crow.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>
#include <optional>

#include "uuid_utils.h"
#include "time_utils.h"
#include "runtime_logger.h"

namespace fs = std::filesystem;

// 简单的 Project 结构：只包含需要的字段，便于序列化/反序列化
struct Project {
    std::string uuid;
    std::string name;
    std::string createdAt;
    std::string updatedAt;
    std::string note;

    std::string to_json() const {
        // 简单且安全的字符串转义
        auto esc = [](const std::string &s){
            std::string out; out.reserve(s.size()+8);
            for (char c : s) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default: out += c; break;
                }
            }
            return out;
        };
        std::string s = "{";
        s += "\"uuid\":\"" + esc(uuid) + "\",";
        s += "\"name\":\"" + esc(name) + "\",";
        s += "\"createdAt\":\"" + esc(createdAt) + "\",";
        s += "\"updatedAt\":\"" + esc(updatedAt) + "\",";
        s += "\"note\":\"" + esc(note) + "\"";
        s += "}";
        return s;
    }

    static std::optional<Project> from_raw_object(const std::string &obj_text) {
        // 极简解析：在受控环境下解析固定字段的字符串值
        auto extract = [&](const std::string &key)->std::optional<std::string>{
            std::string k = '"' + key + '"';
            auto pos = obj_text.find(k);
            if (pos == std::string::npos) return std::nullopt;
            pos = obj_text.find(':', pos + k.size());
            if (pos == std::string::npos) return std::nullopt;
            // 找第一个双引号
            pos = obj_text.find('"', pos);
            if (pos == std::string::npos) return std::nullopt;
            size_t start = pos + 1;
            std::string out;
            for (size_t i = start; i < obj_text.size(); ++i) {
                char c = obj_text[i];
                if (c == '"') { return out; }
                if (c == '\\' && i + 1 < obj_text.size()) {
                    char n = obj_text[++i];
                    switch (n) {
                        case '"': out += '"'; break;
                        case '\\': out += '\\'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
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
        };

        Project p;
        auto u = extract("uuid");
        auto n = extract("name");
        auto ca = extract("createdAt");
        auto ua = extract("updatedAt");
        auto note = extract("note");
        if (!u || !n || !ca || !ua || !note) return std::nullopt;
        p.uuid = *u; p.name = *n; p.createdAt = *ca; p.updatedAt = *ua; p.note = *note;
        return p;
    }
};

struct InfoStore {
    fs::path base_path;
    fs::path file_path; // base_path 下的 info.json
    std::mutex mtx;
    // 内存索引：uuid -> Project
    std::map<std::string, Project> index;

    void init(const std::string &base)
    {
        RuntimeLogger::info("InfoStore::init 开始，base=" + base);
        base_path = fs::path(base);
        if (!fs::exists(base_path)) {
            RuntimeLogger::info("创建数据库目录: " + base_path.string());
            fs::create_directories(base_path);
        }
        file_path = base_path / "info.json";
        if (!fs::exists(file_path)) {
            // 创建空数组
            RuntimeLogger::info("初始化 info.json: " + file_path.string());
            write_raw("[]");
        }
        load();
        RuntimeLogger::info("InfoStore::init 完成，项目数量=" + std::to_string(index.size()));
    }

    void load()
    {
        std::lock_guard<std::mutex> lk(mtx);
        RuntimeLogger::debug("InfoStore::load 开始: " + file_path.string());
        std::ifstream ifs(file_path);
        if (!ifs) throw std::runtime_error("无法打开 info.json 以读取");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.empty()) content = "[]";
        // 解析：寻找每个对象的花括号块并用 from_raw_object 解析
        index.clear();
        size_t pos = 0;
        while (true) {
            pos = content.find('{', pos);
            if (pos == std::string::npos) break;
            int depth = 0;
            size_t start = pos;
            size_t end = pos;
            for (size_t i = pos; i < content.size(); ++i) {
                if (content[i] == '{') ++depth;
                else if (content[i] == '}') {
                    --depth;
                    if (depth == 0) { end = i; pos = i + 1; break; }
                }
            }
            if (end <= start) break;
            std::string obj_text = content.substr(start, end - start + 1);
            auto maybe = Project::from_raw_object(obj_text);
            if (maybe) {
                index[maybe->uuid] = *maybe;
            }
        }
        RuntimeLogger::debug("InfoStore::load 完成，索引数量=" + std::to_string(index.size()));
    }

    std::vector<Project> list_sorted()
    {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<Project> out;
        out.reserve(index.size());
        for (auto &p : index) out.push_back(p.second);
        std::sort(out.begin(), out.end(), [](const Project &a, const Project &b){
            return a.updatedAt > b.updatedAt; // ISO-8601 可字典序比较
        });
        return out;
    }

    bool exists(const std::string &uuid)
    {
        std::lock_guard<std::mutex> lk(mtx);
        return index.find(uuid) != index.end();
    }

    Project get(const std::string &uuid)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = index.find(uuid);
        if (it == index.end()) throw std::runtime_error("未找到项目");
        return it->second;
    }

    std::string read_project_json(const std::string &uuid)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (index.find(uuid) == index.end()) throw std::runtime_error("未找到项目");
        }
        fs::path path = base_path / uuid / "project.json";
        RuntimeLogger::debug("读取 project.json: uuid=" + uuid + ", path=" + path.string());
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) throw std::runtime_error("project.json not found");
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.empty()) return std::string("{}");
        RuntimeLogger::debug("读取 project.json 完成: uuid=" + uuid + ", bytes=" + std::to_string(content.size()));
        return content;
    }

    Project create(const std::string &name, const std::string &note)
    {
        if (name.empty()) throw std::runtime_error("name 不能为空");
        RuntimeLogger::info("创建项目开始: name=" + name);
        Project p;
        p.uuid = generate_uuid_v4();
        p.name = name;
        p.createdAt = now_iso8601_utc();
        p.updatedAt = p.createdAt;
        p.note = note;
        {
            std::lock_guard<std::mutex> lk(mtx);
            fs::path project_dir = base_path / p.uuid;
            std::error_code ec;
            fs::create_directories(project_dir, ec);
            if (ec) {
                throw std::runtime_error(std::string("创建项目目录失败: ") + ec.message());
            }
            write_project_json(project_dir / "project.json", p.uuid);
            index[p.uuid] = p;
            persist_locked();
        }
        RuntimeLogger::info("创建项目完成: uuid=" + p.uuid + ", name=" + p.name);
        return p;
    }

    Project patch(const std::string &uuid, const std::optional<std::string> &note)
    {
        std::lock_guard<std::mutex> lk(mtx);
        RuntimeLogger::info("更新项目开始: uuid=" + uuid);
        auto it = index.find(uuid);
        if (it == index.end()) throw std::runtime_error("未找到项目");
        if (note) it->second.note = *note;
        it->second.updatedAt = now_iso8601_utc();
        persist_locked();
        RuntimeLogger::info("更新项目完成: uuid=" + uuid);
        return it->second;
    }

    bool remove(const std::string &uuid)
    {
        std::lock_guard<std::mutex> lk(mtx);
        RuntimeLogger::info("删除项目开始: uuid=" + uuid);
        auto it = index.find(uuid);
        if (it == index.end()) return false;
        index.erase(it);
        persist_locked();
        fs::path project_dir = base_path / uuid;
        std::error_code ec;
        fs::remove_all(project_dir, ec);
        if (ec) {
            throw std::runtime_error(std::string("删除项目目录失败: ") + ec.message());
        }
        RuntimeLogger::info("删除项目完成: uuid=" + uuid);
        return true;
    }

private:
    static std::string escape_json_string(const std::string &s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    void write_project_json(const fs::path &path, const std::string &uuid)
    {
        RuntimeLogger::debug("写入 project.json 开始: " + path.string() + ", uuid=" + uuid);
        std::string s;
        s += "{\n";
        s += "  \"uuid\": \"" + escape_json_string(uuid) + "\",\n";
        s += "  \"raw\": false,\n";
        s += "  \"nii\": false,\n";
        s += "  \"dcm\": false,\n";
        s += "  \"semi\": false,\n";
        s += "  \"semi-xL\": -1,\n";
        s += "  \"semi-xR\": -1,\n";
        s += "  \"semi-yL\": -1,\n";
        s += "  \"semi-yR\": -1,\n";
        s += "  \"processed\": false,\n";
        s += "  \"PD\": false,\n";
        s += "  \"PD-nii\": false,\n";
        s += "  \"PD-dcm\": false,\n";
        s += "  \"PD-3d\": false\n";
        s += "}\n";

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs) throw std::runtime_error("无法打开 project.json 进行写入");
        ofs << s;
        ofs.flush();
        if (!ofs) throw std::runtime_error("写入 project.json 失败");
        RuntimeLogger::debug("写入 project.json 完成: " + path.string());
    }

    void persist_locked()
    {
        // 调用者必须持有 mtx 锁
        RuntimeLogger::debug("InfoStore::persist_locked 开始，项目数量=" + std::to_string(index.size()));
        std::string out = "[";
        bool first = true;
        for (auto &p : index) {
            if (!first) out += ",";
            first = false;
            out += p.second.to_json();
        }
        out += "]";
        write_raw(out);
        RuntimeLogger::debug("InfoStore::persist_locked 完成");
    }

    void write_raw(const std::string &s)
    {
        // 原子性写入：先写入临时文件再重命名
        RuntimeLogger::debug("写入 info.json 开始: " + file_path.string() + ", bytes=" + std::to_string(s.size()));
        fs::path tmp = file_path;
        tmp += ".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) throw std::runtime_error("无法打开临时文件进行写入");
            ofs << s;
            ofs.flush();
            if (!ofs) throw std::runtime_error("写入临时文件失败");
        }
        std::error_code ec;
        fs::rename(tmp, file_path, ec);
        if (ec) throw std::runtime_error(std::string("重命名失败: ") + ec.message());
        RuntimeLogger::debug("写入 info.json 完成: " + file_path.string());
    }
};
