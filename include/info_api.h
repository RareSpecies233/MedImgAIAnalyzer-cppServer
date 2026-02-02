#pragma once

#include <crow.h>
#include <crow/multipart.h>
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include "cnpy.h"
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

static std::optional<int> extract_int_field(const std::string &body, const std::string &key) {
    std::string k = '"' + key + '"';
    auto pos = body.find(k);
    if (pos == std::string::npos) return std::nullopt;
    pos = body.find(':', pos + k.size());
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    bool neg = false;
    if (pos < body.size() && body[pos] == '-') { neg = true; ++pos; }
    if (pos >= body.size() || !std::isdigit(static_cast<unsigned char>(body[pos]))) return std::nullopt;
    long long val = 0;
    while (pos < body.size() && std::isdigit(static_cast<unsigned char>(body[pos]))) {
        val = val * 10 + (body[pos] - '0');
        ++pos;
    }
    if (neg) val = -val;
    return static_cast<int>(val);
}

static inline std::string read_text_file(const fs::path &path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("无法读取文件");
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return content;
}

static inline void write_text_file(const fs::path &path, const std::string &content)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) throw std::runtime_error("无法写入文件");
    ofs << content;
    ofs.flush();
    if (!ofs) throw std::runtime_error("写入文件失败");
}

static inline std::string to_lower_copy(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return out;
}

static inline std::string update_json_field(std::string json, const std::string &key, const std::string &value_literal)
{
    std::string k = "\"" + key + "\"";
    auto pos = json.find(k);
    if (pos == std::string::npos) throw std::runtime_error("project.json 缺少字段: " + key);
    pos = json.find(':', pos + k.size());
    if (pos == std::string::npos) throw std::runtime_error("project.json 字段格式错误");
    size_t start = pos + 1;
    while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
    size_t end = start;
    if (start < json.size() && json[start] == '"') {
        ++end;
        while (end < json.size()) {
            if (json[end] == '"' && json[end - 1] != '\\') { ++end; break; }
            ++end;
        }
    } else {
        while (end < json.size() && json[end] != ',' && json[end] != '}' && !std::isspace(static_cast<unsigned char>(json[end]))) ++end;
    }
    json.replace(start, end - start, value_literal);
    return json;
}

static inline void update_project_json_fields(const fs::path &project_json, const std::map<std::string, std::string> &kv)
{
    std::string json = read_text_file(project_json);
    for (const auto &it : kv) {
        json = update_json_field(json, it.first, it.second);
    }
    write_text_file(project_json, json);
}

static inline cv::Mat npy_to_mat_8u(const cnpy::NpyArray &arr)
{
    if (arr.word_size != sizeof(double) || arr.shape.size() != 2) {
        throw std::runtime_error("Only 2D float64 supported");
    }
    const size_t h = arr.shape[0];
    const size_t w = arr.shape[1];
    const double *data = arr.data<double>();

    cv::Mat img(static_cast<int>(h), static_cast<int>(w), CV_8UC1);
    if (arr.fortran_order) {
        for (size_t r = 0; r < h; ++r) {
            for (size_t c = 0; c < w; ++c) {
                double v = data[c * h + r];
                v = std::min(1.0, std::max(0.0, v));
                img.at<uchar>(static_cast<int>(r), static_cast<int>(c)) = static_cast<uchar>(v * 255.0 + 0.5);
            }
        }
    } else {
        for (size_t r = 0; r < h; ++r) {
            for (size_t c = 0; c < w; ++c) {
                double v = data[r * w + c];
                v = std::min(1.0, std::max(0.0, v));
                img.at<uchar>(static_cast<int>(r), static_cast<int>(c)) = static_cast<uchar>(v * 255.0 + 0.5);
            }
        }
    }
    return img;
}

static inline void write_placeholder_png(const fs::path &out_path)
{
    cv::Mat img(512, 512, CV_8UC3, cv::Scalar(255, 255, 255));
    const int step = 48;
    for (int y = 32; y < img.rows; y += step) {
        for (int x = 16; x < img.cols; x += step) {
            cv::putText(img, "?", cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 0, 0), 2);
        }
    }
    cv::imwrite(out_path.string(), img);
}

static inline void all2npz_stub(const fs::path &src, const fs::path &dst)
{
    std::cout << "all2npz尚未开发，目前仅修改后缀" << std::endl;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("all2npz 复制失败: " + ec.message());
}

static inline void all2png_stub(const fs::path &src, const fs::path &dst)
{
    std::cout << "尚未开发nii、dcm转png" << std::endl;
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    write_placeholder_png(dst);
}

static inline void convert_npz_to_pngs(const fs::path &npz_path, const fs::path &png_dir, const fs::path &marked_dir, bool marked)
{
    cnpy::npz_t npz = cnpy::npz_load(npz_path.string());
    if (npz.empty()) throw std::runtime_error("npz为空");

    auto it = npz.begin();
    const cnpy::NpyArray &arr_first = it->second;
    cv::Mat img = npy_to_mat_8u(arr_first);
    fs::create_directories(png_dir);
    fs::path out_png = png_dir / (npz_path.stem().string() + ".png");
    if (!cv::imwrite(out_png.string(), img)) throw std::runtime_error("写入png失败");

    if (marked) {
        fs::create_directories(marked_dir);
        cv::Mat mask;
        if (++it != npz.end()) {
            mask = npy_to_mat_8u(it->second);
        } else {
            mask = img;
        }
        cv::Mat rgba(mask.rows, mask.cols, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        for (int r = 0; r < mask.rows; ++r) {
            for (int c = 0; c < mask.cols; ++c) {
                uchar v = mask.at<uchar>(r, c);
                if (v == 0) {
                    rgba.at<cv::Vec4b>(r, c) = cv::Vec4b(0, 0, 0, 0);
                } else {
                    rgba.at<cv::Vec4b>(r, c) = cv::Vec4b(255, 255, 255, 128);
                }
            }
        }
        fs::path out_marked = marked_dir / (npz_path.stem().string() + "_marked.png");
        if (!cv::imwrite(out_marked.string(), rgba)) throw std::runtime_error("写入markedpng失败");
    }
}

static inline std::vector<fs::path> list_files(const fs::path &dir)
{
    std::vector<fs::path> files;
    if (!fs::exists(dir)) return files;
    for (auto &p : fs::directory_iterator(dir)) {
        if (p.is_regular_file()) files.push_back(p.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

static inline fs::path first_file_or_throw(const fs::path &dir)
{
    auto files = list_files(dir);
    if (files.empty()) throw std::runtime_error("未找到文件");
    return files.front();
}

static inline std::string shell_escape(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static inline fs::path create_zip_store(const fs::path &dir, const std::string &zip_name)
{
    if (!fs::exists(dir)) throw std::runtime_error("目录不存在");
    fs::path tmp = fs::temp_directory_path() / zip_name;
    std::string cmd = "zip -0 -r " + shell_escape(tmp.string()) + " .";
    auto cwd = fs::current_path();
    fs::current_path(dir);
    int rc = std::system(cmd.c_str());
    fs::current_path(cwd);
    if (rc != 0) throw std::runtime_error("zip 失败");
    return tmp;
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

    // 获取 project.json
    CROW_ROUTE(app, "/api/projects/<string>/project.json").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            auto body = store.read_project_json(uuid);
            crow::response r{body};
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"project not found\"}")};
            r.code = 404;
            set_json_headers(r);
            return r;
        }
    });

    // 取消初始化
    CROW_ROUTE(app, "/api/projects/<string>/uninit").methods(crow::HTTPMethod::POST)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path temp_dir = project_dir / "temp";
            std::error_code ec;
            fs::remove_all(temp_dir, ec);
            fs::path project_json = project_dir / "project.json";
            update_project_json_fields(project_json, {{"raw", "false"}});
            crow::response r{"{\"status\":\"ok\"}"};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 上传文件到 temp
    CROW_ROUTE(app, "/api/project/<string>/upload").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path temp_dir = project_dir / "temp";
            fs::create_directories(temp_dir);

            std::string content_type = req.get_header_value("Content-Type");
            if (content_type.find("multipart/form-data") != std::string::npos) {
                crow::multipart::message msg(req);
                int saved = 0;
                for (auto &part : msg.parts) {
                    std::string filename;
                    const auto &cd = part.get_header_object("Content-Disposition");
                    auto it = cd.params.find("filename");
                    if (it != cd.params.end()) filename = it->second;
                    if (filename.empty()) filename = "noname.bin";
                    fs::path out = temp_dir / fs::path(filename).filename();
                    write_text_file(out, part.body);
                    ++saved;
                }
                crow::response r{std::string("{\"saved\":") + std::to_string(saved) + "}"};
                r.code = 200; set_json_headers(r); return r;
            }

            std::string filename = req.get_header_value("X-Filename");
            if (filename.empty()) filename = "noname.bin";
            fs::path out = temp_dir / fs::path(filename).filename();
            write_text_file(out, req.body);
            crow::response r{"{\"saved\":1}"};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 初始化完成
    CROW_ROUTE(app, "/api/project/<string>/inited").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            auto maybe_raw = extract_string_field(req.body, "raw");
            if (!maybe_raw) throw std::runtime_error("missing raw");
            std::string raw = to_lower_copy(*maybe_raw);
            if (raw != "png" && raw != "npz" && raw != "markednpz" && raw != "dcm" && raw != "nii") {
                throw std::runtime_error("invalid raw type");
            }

            fs::path project_dir = store.base_path / uuid;
            fs::path temp_dir = project_dir / "temp";
            fs::path npz_dir = project_dir / "npz";
            fs::path png_dir = project_dir / "png";
            fs::path marked_dir = project_dir / "markedpng";
            fs::create_directories(temp_dir);

            auto temp_files = list_files(temp_dir);
            if (temp_files.empty()) throw std::runtime_error("temp 为空");

            if (raw != "npz" && raw != "markednpz") {
                fs::create_directories(npz_dir);
                for (const auto &src : temp_files) {
                    fs::path dst = npz_dir / (src.stem().string() + ".npz");
                    all2npz_stub(src, dst);
                }
            }

            if (raw != "png") {
                if (raw == "npz") {
                    for (const auto &src : temp_files) {
                        convert_npz_to_pngs(src, png_dir, marked_dir, false);
                    }
                } else if (raw == "markednpz") {
                    for (const auto &src : temp_files) {
                        convert_npz_to_pngs(src, png_dir, marked_dir, true);
                    }
                } else if (raw == "dcm" || raw == "nii") {
                    for (const auto &src : temp_files) {
                        fs::path dst = png_dir / (src.stem().string() + ".png");
                        all2png_stub(src, dst);
                    }
                } else if (raw == "png") {
                    // no-op
                }
            }

            std::string raw_dir = (raw == "markednpz") ? "npz" : raw;
            fs::path target_dir = project_dir / raw_dir;
            std::error_code ec;
            if (fs::exists(target_dir)) fs::remove_all(target_dir, ec);
            fs::rename(temp_dir, target_dir, ec);
            if (ec) throw std::runtime_error("重命名 temp 失败: " + ec.message());

            fs::path project_json = project_dir / "project.json";
            std::map<std::string, std::string> kv;
            kv["raw"] = "\"" + raw + "\"";
            if (raw == "dcm") kv["dcm"] = "\"raw\"";
            if (raw == "nii") kv["nii"] = "\"raw\"";
            update_project_json_fields(project_json, kv);

            crow::response r{"{\"status\":\"ok\"}"};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 获取 png 列表
    CROW_ROUTE(app, "/api/project/<string>/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path png_dir = store.base_path / uuid / "png";
            auto files = list_files(png_dir);
            std::string body = "[";
            for (size_t i = 0; i < files.size(); ++i) {
                body += "\"" + files[i].filename().string() + "\"";
                if (i + 1 < files.size()) body += ",";
            }
            body += "]";
            crow::response r{body};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 获取单张 png
    CROW_ROUTE(app, "/api/project/<string>/png/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid, const std::string &filename){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            fs::path png_path = store.base_path / uuid / "png" / filename;
            if (!fs::exists(png_path)) throw std::runtime_error("png not found");
            crow::response r{read_text_file(png_path)};
            r.set_header("Content-Type", "image/png");
            r.set_header("Access-Control-Allow-Origin", "*");
            r.code = 200;
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 获取 markedpng 列表
    CROW_ROUTE(app, "/api/project/<string>/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path marked_dir = store.base_path / uuid / "markedpng";
            auto files = list_files(marked_dir);
            std::string body = "[";
            for (size_t i = 0; i < files.size(); ++i) {
                body += "\"" + files[i].filename().string() + "\"";
                if (i + 1 < files.size()) body += ",";
            }
            body += "]";
            crow::response r{body};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 获取单张 markedpng
    CROW_ROUTE(app, "/api/project/<string>/markedpng/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid, const std::string &filename){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            fs::path png_path = store.base_path / uuid / "markedpng" / filename;
            if (!fs::exists(png_path)) throw std::runtime_error("markedpng not found");
            crow::response r{read_text_file(png_path)};
            r.set_header("Content-Type", "image/png");
            r.set_header("Access-Control-Allow-Origin", "*");
            r.code = 200;
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 更新 semi 裁剪参数
    CROW_ROUTE(app, "/api/projects/<string>/semi").methods(crow::HTTPMethod::PATCH)([&store](const crow::request &req, const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            auto xl = extract_int_field(req.body, "semi-xL");
            auto xr = extract_int_field(req.body, "semi-xR");
            auto yl = extract_int_field(req.body, "semi-yL");
            auto yr = extract_int_field(req.body, "semi-yR");
            if (!xl || !xr || !yl || !yr) throw std::runtime_error("invalid json");
            fs::path project_json = store.base_path / uuid / "project.json";
            update_project_json_fields(project_json, {
                {"semi-xL", std::to_string(*xl)},
                {"semi-xR", std::to_string(*xr)},
                {"semi-yL", std::to_string(*yl)},
                {"semi-yR", std::to_string(*yr)}
            });
            crow::response r{"{\"status\":\"ok\"}"};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 png
    CROW_ROUTE(app, "/api/project/<string>/download/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path dir = store.base_path / uuid / "png";
            auto zip_path = create_zip_store(dir, uuid + "_png.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"png.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 npz
    CROW_ROUTE(app, "/api/project/<string>/download/npz").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path dir = store.base_path / uuid / "npz";
            auto zip_path = create_zip_store(dir, uuid + "_npz.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"npz.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 dcm
    CROW_ROUTE(app, "/api/project/<string>/download/dcm").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path project_json = project_dir / "project.json";
            std::string json = read_text_file(project_json);
            if (json.find("\"dcm\": false") != std::string::npos) {
                std::cout << "npz2dcm尚未开发，目前仅修改后缀" << std::endl;
                fs::path npz_dir = project_dir / "npz";
                fs::path dcm_dir = project_dir / "dcm";
                fs::create_directories(dcm_dir);
                for (const auto &src : list_files(npz_dir)) {
                    fs::path dst = dcm_dir / (src.stem().string() + ".dcm");
                    std::error_code ec;
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                    if (ec) throw std::runtime_error("npz2dcm 复制失败: " + ec.message());
                }
                update_project_json_fields(project_json, {{"dcm", "true"}});
            }
            fs::path dir = project_dir / "dcm";
            auto zip_path = create_zip_store(dir, uuid + "_dcm.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"dcm.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 nii
    CROW_ROUTE(app, "/api/project/<string>/download/nii").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path project_json = project_dir / "project.json";
            std::string json = read_text_file(project_json);
            if (json.find("\"nii\": false") != std::string::npos) {
                std::cout << "npz2nii尚未开发，目前仅修改后缀" << std::endl;
                fs::path npz_dir = project_dir / "npz";
                fs::path nii_dir = project_dir / "nii";
                fs::create_directories(nii_dir);
                for (const auto &src : list_files(npz_dir)) {
                    fs::path dst = nii_dir / (src.stem().string() + ".nii");
                    std::error_code ec;
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                    if (ec) throw std::runtime_error("npz2nii 复制失败: " + ec.message());
                }
                update_project_json_fields(project_json, {{"nii", "true"}});
            }
            fs::path dir = project_dir / "nii";
            auto zip_path = create_zip_store(dir, uuid + "_nii.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"nii.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
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

    // 修改备注（PATCH）
    CROW_ROUTE(app, "/api/projects/<string>/note").methods(crow::HTTPMethod::PATCH)([&store](const crow::request &req, const std::string &uuid){
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
        r.set_header("Access-Control-Allow-Methods", "GET, DELETE, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/projects/<string>/project.json").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/projects/<string>/uninit").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/upload").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type, X-Filename");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/inited").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/png").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/png/<string>").methods(crow::HTTPMethod::OPTIONS)([](const std::string &, const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/markedpng").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/markedpng/<string>").methods(crow::HTTPMethod::OPTIONS)([](const std::string &, const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/projects/<string>/semi").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "PATCH, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/download/<string>").methods(crow::HTTPMethod::OPTIONS)([](const std::string &, const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/projects/<string>/note").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
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
        r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });
}
