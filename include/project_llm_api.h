#pragma once

template <typename App>
inline void register_project_llm_routes(App &app, InfoStore &store)
{
    CROW_ROUTE(app, "/api/llm/settings").methods(crow::HTTPMethod::GET)([&store]() {
        try {
            auto cfg = load_llm_settings(store);
            crow::json::wvalue res;
            res["base_url"] = cfg.base_url;
            res["api_key"] = cfg.api_key;
            res["model"] = cfg.model;
            res["temperature"] = cfg.temperature;
            res["top_k"] = cfg.top_k;
            res["system_prompt"] = cfg.system_prompt;
            crow::response r{res};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/settings").methods(crow::HTTPMethod::POST)([&store](const crow::request &req) {
        try {
            auto body = crow::json::load(req.body);
            if (!body) throw std::runtime_error("invalid json");

            auto cfg = load_llm_settings(store);
            if (body.has("base_url") && body["base_url"].t() == crow::json::type::String) cfg.base_url = body["base_url"].s();
            if (body.has("api_key") && body["api_key"].t() == crow::json::type::String) cfg.api_key = body["api_key"].s();
            if (body.has("model") && body["model"].t() == crow::json::type::String) cfg.model = body["model"].s();
            if (body.has("system_prompt") && body["system_prompt"].t() == crow::json::type::String) cfg.system_prompt = body["system_prompt"].s();
            if (body.has("temperature") && body["temperature"].t() == crow::json::type::Number) cfg.temperature = body["temperature"].d();
            if (body.has("top_k") && body["top_k"].t() == crow::json::type::Number) cfg.top_k = static_cast<int>(body["top_k"].i());

            if (cfg.temperature < 0.0) cfg.temperature = 0.0;
            if (cfg.temperature > 2.0) cfg.temperature = 2.0;
            if (cfg.top_k < 1) cfg.top_k = 1;
            if (cfg.top_k > 10) cfg.top_k = 10;

            save_llm_settings(store, cfg);
            crow::response r{"{\"status\":\"ok\"}"};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/rag/upload").methods(crow::HTTPMethod::POST)([&store](const crow::request &req) {
        try {
            RuntimeLogger::info("[RAG][上传] 收到请求: content_type=" + req.get_header_value("Content-Type") +
                                ", body_bytes=" + std::to_string(req.body.size()));
            fs::path dir = rag_db_dir(store);
            fs::create_directories(dir);

            std::string content_type = req.get_header_value("Content-Type");
            crow::json::wvalue uploaded;
            uploaded = crow::json::wvalue::list();

            int saved = 0;
            if (content_type.find("multipart/form-data") != std::string::npos) {
                crow::multipart::message msg(req);
                for (auto &part : msg.parts) {
                    std::string filename;
                    const auto &cd = part.get_header_object("Content-Disposition");
                    auto it = cd.params.find("filename");
                    if (it != cd.params.end()) filename = it->second;
                    filename = sanitize_filename(filename);
                    if (filename.empty()) filename = "document.txt";

                    fs::path out = dir / (random_hex_id() + "__" + filename);
                    write_text_file(out, part.body);
                    std::string parsed = load_or_build_rag_cached_text(store, out);
                    RuntimeLogger::info("[RAG][上传] 保存文档: name=" + filename +
                                        ", bytes=" + std::to_string(part.body.size()));
                    RuntimeLogger::info("[RAG][上传] 文档解析完成: name=" + filename +
                                        ", parsed_len=" + std::to_string(parsed.size()));
                    uploaded[saved] = filename;
                    ++saved;
                }
            } else {
                std::string filename = sanitize_filename(req.get_header_value("X-Filename"));
                if (filename.empty()) filename = "document.txt";
                fs::path out = dir / (random_hex_id() + "__" + filename);
                write_text_file(out, req.body);
                std::string parsed = load_or_build_rag_cached_text(store, out);
                RuntimeLogger::info("[RAG][上传] 保存文档: name=" + filename +
                                    ", bytes=" + std::to_string(req.body.size()));
                RuntimeLogger::info("[RAG][上传] 文档解析完成: name=" + filename +
                                    ", parsed_len=" + std::to_string(parsed.size()));
                uploaded[0] = filename;
                saved = 1;
            }

            RuntimeLogger::info("[RAG][上传] 完成: saved=" + std::to_string(saved));

            crow::json::wvalue res;
            res["saved"] = saved;
            res["uploaded"] = std::move(uploaded);
            crow::response r{res};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[RAG][上传] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/rag/documents").methods(crow::HTTPMethod::GET)([&store]() {
        try {
            auto files = list_rag_docs(store);
            RuntimeLogger::info("[RAG][文档列表] 查询: count=" + std::to_string(files.size()));
            crow::json::wvalue docs;
            docs = crow::json::wvalue::list();
            for (std::size_t i = 0; i < files.size(); ++i) {
                crow::json::wvalue item;
                item["name"] = original_name_from_rag_file(files[i]);
                item["size"] = static_cast<uint64_t>(fs::file_size(files[i]));
                docs[static_cast<unsigned int>(i)] = std::move(item);
            }
            crow::json::wvalue res;
            res["documents"] = std::move(docs);
            res["count"] = static_cast<int>(files.size());
            crow::response r{res};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[RAG][文档列表] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/rag/documents/<path>").methods(crow::HTTPMethod::DELETE)([&store](const std::string &name) {
        try {
            RuntimeLogger::info("[RAG][删除文档] 请求: name=" + name);
            if (name.empty() || name.find("..") != std::string::npos || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid name");
            }

            auto files = list_rag_docs(store);
            int deleted = 0;
            for (const auto &path : files) {
                std::string raw_name = original_name_from_rag_file(path);
                std::string disk_name = path.filename().string();
                if (raw_name == name || disk_name == name) {
                    const fs::path cache_path = rag_cache_path_for_doc(store, path);
                    std::error_code ec;
                    fs::remove(path, ec);
                    if (ec) {
                        throw std::runtime_error("删除文件失败: " + ec.message());
                    }
                    std::error_code cache_ec;
                    fs::remove(cache_path, cache_ec);
                    ++deleted;
                }
            }

            if (deleted == 0) {
                RuntimeLogger::warn("[RAG][删除文档] 未命中: name=" + name);
                crow::response r{"{\"error\":\"document not found\"}"};
                r.code = 404;
                set_json_headers(r);
                return r;
            }

            RuntimeLogger::info("[RAG][删除文档] 完成: name=" + name + ", deleted=" + std::to_string(deleted));

            crow::json::wvalue res;
            res["status"] = "ok";
            res["deleted"] = deleted;
            res["name"] = name;
            crow::response r{res};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[RAG][删除文档] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/rag/download/<path>").methods(crow::HTTPMethod::GET)([&store](const std::string &name) {
        try {
            RuntimeLogger::info("[RAG][下载文档] 请求: name=" + name);
            if (name.empty() || name.find("..") != std::string::npos || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid name");
            }

            auto files = list_rag_docs(store);
            fs::path target_path;
            std::string download_name;
            for (const auto &path : files) {
                std::string raw_name = original_name_from_rag_file(path);
                std::string disk_name = path.filename().string();
                if (raw_name == name || disk_name == name) {
                    target_path = path;
                    download_name = raw_name;
                    break;
                }
            }

            if (target_path.empty() || !fs::exists(target_path)) {
                RuntimeLogger::warn("[RAG][下载文档] 未找到: name=" + name);
                crow::response r{"{\"error\":\"document not found\"}"};
                r.code = 404;
                set_json_headers(r);
                return r;
            }

            RuntimeLogger::info("[RAG][下载文档] 命中: path=" + target_path.string());

            crow::response r{read_text_file(target_path)};
            r.set_header("Content-Type", "application/octet-stream");
            r.set_header("Content-Disposition", "attachment; filename=\"" + download_name + "\"");
            r.set_header("Access-Control-Allow-Origin", "*");
            r.code = 200;
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[RAG][下载文档] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/rag/download").methods(crow::HTTPMethod::GET)([&store]() {
        try {
            fs::path dir = rag_db_dir(store);
            RuntimeLogger::info("[RAG][下载全集] 开始打包: dir=" + dir.string());
            auto zip_path = create_zip_store(dir, "llm_rag_documents.zip");
            RuntimeLogger::info("[RAG][下载全集] 打包完成: zip=" + zip_path.string());
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"llm_rag_documents.zip\"");
            r.set_header("Access-Control-Allow-Origin", "*");
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[RAG][下载全集] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/llm/chat").methods(crow::HTTPMethod::POST)([&store](const crow::request &req) {
        try {
            RuntimeLogger::info("[RAG][问答] 收到请求: body_bytes=" + std::to_string(req.body.size()));
            auto body = crow::json::load(req.body);
            if (!body) throw std::runtime_error("invalid json");
            auto res = execute_llm_chat_request(store, body, std::nullopt);
            crow::response r{res};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[RAG][问答] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/llm/doc").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &uuid) {
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            RuntimeLogger::info("[项目RAG][上传] 收到请求: uuid=" + uuid +
                                ", body_bytes=" + std::to_string(req.body.size()));

            fs::path dir = project_llm_doc_dir(store, uuid);
            fs::create_directories(dir);
            std::string content_type = req.get_header_value("Content-Type");

            auto save_doc = [&](const std::string &raw_name, const std::string &content) {
                std::string filename = sanitize_filename(raw_name);
                if (filename.empty()) filename = "document.txt";
                fs::path out = dir / filename;
                write_text_file(out, content);
                parse_and_store_project_rag_doc(store, uuid, out);
                RuntimeLogger::info("[项目RAG][上传] 保存成功: uuid=" + uuid +
                                    ", file=" + filename +
                                    ", bytes=" + std::to_string(content.size()));
            };

            if (content_type.find("multipart/form-data") != std::string::npos) {
                crow::multipart::message msg(req);
                bool saved = false;
                for (auto &part : msg.parts) {
                    std::string filename;
                    const auto &cd = part.get_header_object("Content-Disposition");
                    auto it = cd.params.find("filename");
                    if (it != cd.params.end()) filename = it->second;
                    if (filename.empty()) continue;
                    save_doc(filename, part.body);
                    saved = true;
                }
                if (!saved) {
                    throw std::runtime_error("missing file");
                }
            } else {
                std::string filename = req.get_header_value("X-Filename");
                if (filename.empty()) filename = "document.txt";
                save_doc(filename, req.body);
            }

            crow::response r{"{\"message\":\"文档上传成功\"}"};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[项目RAG][上传] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/llm/chat").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &uuid) {
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            RuntimeLogger::info("[项目RAG][问答] 收到请求: uuid=" + uuid +
                                ", body_bytes=" + std::to_string(req.body.size()));
            auto body = crow::json::load(req.body);
            if (!body) throw std::runtime_error("invalid json");
            auto res = execute_llm_chat_request(store, body, uuid);
            crow::response r{res};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[项目RAG][问答] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/llm/history").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid) {
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            crow::response r{load_project_llm_history_json(store, uuid)};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/llm/history/delete").methods(crow::HTTPMethod::POST)([&store](const std::string &uuid) {
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            clear_project_llm_state(store, uuid);
            RuntimeLogger::info("[项目RAG][清理] 完成: uuid=" + uuid);
            crow::response r{"{\"message\":\"历史记录已删除\"}"};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            RuntimeLogger::error(std::string("[项目RAG][清理] 失败: ") + e.what());
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });
}
