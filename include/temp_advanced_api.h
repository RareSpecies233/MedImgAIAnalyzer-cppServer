#pragma once

static inline fs::path temp_project_llm_doc_dir(const InfoStore &store, const std::string &temp_uuid)
{
    return temp_project_dir(store, temp_uuid) / "llmdoc";
}

static inline fs::path temp_project_llm_history_path(const InfoStore &store, const std::string &temp_uuid)
{
    return temp_project_dir(store, temp_uuid) / "llm_history.json";
}

static inline fs::path temp_project_llm_parsed_json_path(const fs::path &doc_path)
{
    return doc_path.parent_path() / (doc_path.stem().string() + ".json");
}

static inline std::vector<fs::path> list_temp_rag_source_docs(const InfoStore &store,
                                                              const std::string &temp_uuid)
{
    std::vector<fs::path> docs;
    const fs::path dir = temp_project_llm_doc_dir(store, temp_uuid);
    if (!fs::exists(dir)) return docs;

    for (auto &entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const fs::path p = entry.path();
        const std::string ext = to_lower_copy(p.extension().string());
        if (ext == ".json") continue;
        docs.push_back(p);
    }
    std::sort(docs.begin(), docs.end(), [](const fs::path &a, const fs::path &b){
        return a.filename().string() < b.filename().string();
    });
    return docs;
}

static inline std::string parse_and_store_temp_rag_doc(const InfoStore &store,
                                                       const std::string &temp_uuid,
                                                       const fs::path &doc_path)
{
    RuntimeLogger::info("[Temp项目RAG][解析] 开始: uuid=" + temp_uuid +
                        ", file=" + doc_path.filename().string());
    const std::string parsed = extract_text_for_rag(doc_path);
    const fs::path json_path = temp_project_llm_parsed_json_path(doc_path);
    write_text_file(json_path, build_project_rag_parsed_json(doc_path.filename().string(), parsed));
    RuntimeLogger::info("[Temp项目RAG][解析] 完成: uuid=" + temp_uuid +
                        ", file=" + doc_path.filename().string() +
                        ", parsed_len=" + std::to_string(parsed.size()));
    return parsed;
}

static inline std::vector<std::string> load_temp_rag_documents(const InfoStore &store,
                                                               const std::string &temp_uuid)
{
    std::vector<std::string> docs;
    auto files = list_temp_rag_source_docs(store, temp_uuid);
    docs.reserve(files.size());
    for (const auto &doc_path : files) {
        const fs::path json_path = temp_project_llm_parsed_json_path(doc_path);
        std::string parsed = load_project_rag_parsed_text(json_path);
        if (parsed.empty()) {
            RuntimeLogger::info("[Temp项目RAG][解析] 缓存缺失，重新解析: uuid=" + temp_uuid +
                                ", file=" + doc_path.filename().string());
            parsed = parse_and_store_temp_rag_doc(store, temp_uuid, doc_path);
        }
        if (!parsed.empty()) {
            docs.push_back(std::move(parsed));
        }
    }
    return docs;
}

static inline void append_temp_llm_history(const InfoStore &store,
                                           const std::string &temp_uuid,
                                           const std::string &question,
                                           const std::string &answer,
                                           const std::vector<std::string> &contexts)
{
    const fs::path path = temp_project_llm_history_path(store, temp_uuid);
    std::string current = "[]\n";
    if (fs::exists(path)) {
        current = read_text_file(path);
    }

    const std::string updated = append_history_entry_json(current,
                                                          now_iso8601_utc(),
                                                          question,
                                                          answer,
                                                          contexts);
    write_text_file(path, updated);
}

static inline std::string load_temp_llm_history_json(const InfoStore &store,
                                                     const std::string &temp_uuid)
{
    const fs::path path = temp_project_llm_history_path(store, temp_uuid);
    if (!fs::exists(path)) {
        return "[]\n";
    }
    return read_text_file(path);
}

static inline void clear_temp_llm_state(const InfoStore &store, const std::string &temp_uuid)
{
    std::error_code ec;
    fs::remove(temp_project_llm_history_path(store, temp_uuid), ec);
    ec.clear();
    fs::remove_all(temp_project_llm_doc_dir(store, temp_uuid), ec);
}

static inline crow::json::wvalue execute_temp_llm_chat_request(InfoStore &store,
                                                               const crow::json::rvalue &body,
                                                               const std::string &temp_uuid)
{
    if (!body.has("question") || body["question"].t() != crow::json::type::String) {
        throw std::runtime_error("missing question");
    }

    std::string question = trim_copy(body["question"].s());
    if (question.empty()) throw std::runtime_error("question 不能为空");

    auto cfg = load_llm_settings(store);
    if (body.has("top_k") && body["top_k"].t() == crow::json::type::Number) {
        cfg.top_k = static_cast<int>(body["top_k"].i());
    }
    if (body.has("temperature") && body["temperature"].t() == crow::json::type::Number) {
        cfg.temperature = body["temperature"].d();
    }
    if (body.has("system_prompt") && body["system_prompt"].t() == crow::json::type::String) {
        cfg.system_prompt = body["system_prompt"].s();
    }
    if (cfg.top_k < 1) cfg.top_k = 1;
    if (cfg.top_k > 10) cfg.top_k = 10;

    auto files = list_rag_docs_for_index(store);
    std::vector<std::string> docs;
    int parse_failed = 0;
    std::string first_error;

    for (const auto &p : files) {
        try {
            std::string text = load_or_build_rag_cached_text(store, p);
            if (!text.empty()) docs.push_back(std::move(text));
        } catch (const std::exception &ex) {
            ++parse_failed;
            if (first_error.empty()) {
                first_error = p.filename().string() + ": " + ex.what();
            }
        }
    }

    auto project_docs = load_temp_rag_documents(store, temp_uuid);
    for (auto &doc : project_docs) {
        docs.push_back(std::move(doc));
    }

    if (!files.empty() && docs.empty()) {
        std::string msg = "RAG 文档存在，但未提取到可索引文本";
        msg += "（失败 " + std::to_string(parse_failed) + "/" + std::to_string(files.size()) + "）";
        if (!first_error.empty()) {
            msg += "，示例错误: " + first_error;
        }
        throw std::runtime_error(msg);
    }

    RagIndex index;
    int chunks = index.index_documents(docs);
    auto contexts = index.retrieve(question, cfg.top_k);
    std::string answer = llm_chat_completion(cfg, question, contexts);
    append_temp_llm_history(store, temp_uuid, question, answer, contexts);

    crow::json::wvalue context_arr;
    context_arr = crow::json::wvalue::list();
    for (std::size_t i = 0; i < contexts.size(); ++i) {
        context_arr[static_cast<unsigned int>(i)] = contexts[i];
    }

    crow::json::wvalue res;
    res["answer"] = answer;
    res["chunks"] = chunks;
    res["contexts"] = std::move(context_arr);
    return res;
}

template <typename App>
inline void register_temp_advanced_routes(App &app, InfoStore &store)
{
    CROW_ROUTE(app, "/api/temp/<string>/start_enhdb").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &temp_uuid){
        try {
            RuntimeLogger::info("[高级增强][Temp] 开始: uuid=" + temp_uuid);
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);

            auto lookup_double = [&](std::initializer_list<const char*> keys, double fallback) {
                for (const char* key : keys) {
                    auto value = extract_double_field(req.body, key);
                    if (value.has_value()) return *value;
                }
                return fallback;
            };
            auto lookup_int = [&](std::initializer_list<const char*> keys) -> std::optional<int> {
                for (const char* key : keys) {
                    auto value = extract_int_field(req.body, key);
                    if (value.has_value()) return value;
                }
                return std::nullopt;
            };
            auto lookup_bool = [&](std::initializer_list<const char*> keys, bool fallback) {
                for (const char* key : keys) {
                    auto value = extract_bool_field(req.body, key);
                    if (value.has_value()) return *value;
                }
                return fallback;
            };

            npzproc::Args args;
            args.scale_x = lookup_double({"scale-x", "scale_x", "scaleX"}, 1.0);
            args.scale_y = lookup_double({"scale-y", "scale_y", "scaleY"}, 1.0);
            args.rotate_deg = lookup_double({"rotate", "rotate_deg", "rotateDeg"}, 0.0);
            args.contrast = lookup_double({"contrast"}, 1.0);
            args.gamma = lookup_double({"gamma"}, 1.0);
            args.preserve_resolution = lookup_bool({"preserve-resolution", "preserve_resolution", "preserveResolution"}, false);

            const auto crop_x = lookup_int({"crop-x", "crop_x", "cropX"});
            const auto crop_y = lookup_int({"crop-y", "crop_y", "cropY"});
            const auto crop_w = lookup_int({"crop-w", "crop_w", "cropW"});
            const auto crop_h = lookup_int({"crop-h", "crop_h", "cropH"});
            const bool has_any_crop = crop_x.has_value() || crop_y.has_value() || crop_w.has_value() || crop_h.has_value();
            const bool has_full_crop = crop_x.has_value() && crop_y.has_value() && crop_w.has_value() && crop_h.has_value();
            if (has_any_crop && !has_full_crop) {
                throw std::runtime_error("crop 参数需要同时提供 x/y/w/h");
            }
            if (has_full_crop) {
                args.crop = npzproc::CropRect{*crop_x, *crop_y, *crop_w, *crop_h};
            }

            fs::path input_npz_dir = project_dir / "npz";
            auto files = list_files(input_npz_dir);
            if (files.empty()) throw std::runtime_error("npz为空");

            fs::path enh_dir = project_dir / "enhDBprocessed";
            fs::path enh_npz_dir = enh_dir / "npzs";
            fs::path enh_png_dir = enh_dir / "pngs";
            fs::path enh_marked_dir = enh_dir / "markedpngs";
            std::error_code ec;
            fs::remove_all(enh_dir, ec);
            fs::create_directories(enh_npz_dir);
            fs::create_directories(enh_png_dir);
            fs::create_directories(enh_marked_dir);

            size_t processed_count = 0;
            for (const auto &src : files) {
                if (to_lower_copy(src.extension().string()) != ".npz") continue;
                npzproc::Args file_args = args;
                file_args.input = src;
                file_args.output = enh_npz_dir / src.filename();
                npzproc::process_npz_file(file_args);
                convert_npz_to_pngs(file_args.output, enh_png_dir, enh_marked_dir, true, true, "_marked");
                ++processed_count;
            }

            if (processed_count == 0) {
                throw std::runtime_error("npz目录中没有可处理的 npz 文件");
            }

            crow::json::wvalue res;
            res["status"] = "ok";
            res["processed"] = static_cast<int>(processed_count);
            crow::response r{res};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/enhdb/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            crow::response r{json_filename_array(list_files(project_dir / "enhDBprocessed" / "pngs"))};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/enhdb/png/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid, const std::string &filename){
        try {
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            fs::path png_path = project_dir / "enhDBprocessed" / "pngs" / filename;
            if (!fs::exists(png_path)) throw std::runtime_error("enhdb png not found");
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

    CROW_ROUTE(app, "/api/temp/<string>/enhdb/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            crow::response r{json_filename_array(list_files(project_dir / "enhDBprocessed" / "markedpngs"))};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/enhdb/markedpng/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid, const std::string &filename){
        try {
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            fs::path png_path = project_dir / "enhDBprocessed" / "markedpngs" / filename;
            if (!fs::exists(png_path)) throw std::runtime_error("enhdb markedpng not found");
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

    CROW_ROUTE(app, "/api/temp/<string>/download/enhdb/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            auto zip_path = create_zip_store(project_dir / "enhDBprocessed" / "pngs", temp_uuid + "_temp_enhdb_png.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"enhdb_png.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/enhdb/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            auto zip_path = create_zip_store(project_dir / "enhDBprocessed" / "markedpngs", temp_uuid + "_temp_enhdb_markedpng.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"enhdb_markedpng.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/enhdb/fused/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        fs::path fused_dir;
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            fused_dir = build_fused_png_temp_dir(project_dir / "enhDBprocessed" / "pngs",
                                                project_dir / "enhDBprocessed" / "markedpngs",
                                                temp_uuid + "_temp_enhdb_fused_png");
            auto zip_path = create_zip_store(fused_dir, temp_uuid + "_temp_enhdb_fused_png.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"enhdb_png_markedpng_fused.zip\"");
            std::error_code ec;
            fs::remove_all(fused_dir, ec);
            return r;
        } catch (const std::exception &e) {
            std::error_code ec;
            if (!fused_dir.empty()) fs::remove_all(fused_dir, ec);
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/enhdb/npz").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            auto zip_path = create_zip_store(project_dir / "enhDBprocessed" / "npzs", temp_uuid + "_temp_enhdb_npz.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"enhdb_npz.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/enhdb/dcm").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            fs::path dir = project_dir / "enhDBprocessed" / "dcm";
            if (list_files(dir).empty()) {
                ensure_converted_from_npz_dir(project_dir / "enhDBprocessed" / "npzs", dir, "dcm", "image");
            }
            auto zip_path = create_zip_store(dir, temp_uuid + "_temp_enhdb_dcm.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"enhdb_dcm.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/enhdb/nii").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            fs::path dir = project_dir / "enhDBprocessed" / "nii";
            if (list_files(dir).empty()) {
                ensure_converted_from_npz_dir(project_dir / "enhDBprocessed" / "npzs", dir, "nii", "image");
            }
            auto zip_path = create_zip_store(dir, temp_uuid + "_temp_enhdb_nii.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"enhdb_nii.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/to_3d_model").methods(crow::HTTPMethod::POST)([&store](const std::string &temp_uuid){
        try {
            return build_3d_model_project_dir_response(require_temp_project_dir(store, temp_uuid), std::string("temp:") + temp_uuid, true);
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/3d").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            return make_glb_download_response(project_dir / "3d" / "model.glb");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/OG3d").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid){
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            return make_glb_download_response(project_dir / "OG3d" / "model.glb");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/llm/doc").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &temp_uuid) {
        try {
            fs::path project_dir = require_temp_project_dir(store, temp_uuid);
            (void)project_dir;
            fs::path dir = temp_project_llm_doc_dir(store, temp_uuid);
            fs::create_directories(dir);
            std::string content_type = req.get_header_value("Content-Type");

            auto save_doc = [&](const std::string &raw_name, const std::string &content) {
                std::string filename = sanitize_filename(raw_name);
                if (filename.empty()) filename = "document.txt";
                fs::path out = dir / filename;
                write_text_file(out, content);
                parse_and_store_temp_rag_doc(store, temp_uuid, out);
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
                if (!saved) throw std::runtime_error("missing file");
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
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/llm/chat").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &temp_uuid) {
        try {
            require_temp_project_dir(store, temp_uuid);
            auto body = crow::json::load(req.body);
            if (!body) throw std::runtime_error("invalid json");
            auto res = execute_temp_llm_chat_request(store, body, temp_uuid);
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

    CROW_ROUTE(app, "/api/temp/<string>/llm/history").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            require_temp_project_dir(store, temp_uuid);
            crow::response r{load_temp_llm_history_json(store, temp_uuid)};
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

    CROW_ROUTE(app, "/api/temp/<string>/llm/history/delete").methods(crow::HTTPMethod::POST)([&store](const std::string &temp_uuid) {
        try {
            require_temp_project_dir(store, temp_uuid);
            clear_temp_llm_state(store, temp_uuid);
            crow::response r{"{\"message\":\"历史记录已删除\"}"};
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
}
