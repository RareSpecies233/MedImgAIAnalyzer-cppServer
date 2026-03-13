#pragma once

template <typename App>
inline void register_project_basic_routes(App &app,
                                          InfoStore &store,
                                          const std::string &onnx_path,
                                          int infer_threads)
{
    auto require_project_dir = [&store](const std::string &uuid) -> fs::path {
        if (!store.exists(uuid)) throw std::runtime_error("project not found");
        return store.base_path / uuid;
    };

    auto ensure_safe_filename = [](const std::string &filename) {
        if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
            throw std::runtime_error("invalid filename");
        }
    };

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

    CROW_ROUTE(app, "/api/projects/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            crow::response r{store.get(uuid).to_json()};
            set_json_headers(r);
            return r;
        } catch (const std::exception &) {
            crow::response r{"{\"error\":\"project not found\"}"};
            r.code = 404;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/projects/<string>").methods(crow::HTTPMethod::DELETE)([&store](const std::string &uuid){
        try {
            bool ok = store.remove(uuid);
            if (!ok) {
                crow::response r{"{\"error\":\"not found\"}"};
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

    CROW_ROUTE(app, "/api/projects/<string>/project.json").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            crow::response r{store.read_project_json(uuid)};
            set_json_headers(r);
            return r;
        } catch (const std::exception &) {
            crow::response r{"{\"error\":\"project not found\"}"};
            r.code = 404;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/projects").methods(crow::HTTPMethod::POST)([&store](const crow::request &req){
        try {
            auto maybe_name = extract_string_field(req.body, "name");
            if (!maybe_name) {
                crow::response r{"{\"error\":\"invalid json or missing name\"}"};
                r.code = 400;
                set_json_headers(r);
                return r;
            }
            auto maybe_note = extract_string_field(req.body, "note");
            std::string note = maybe_note ? *maybe_note : std::string("");
            auto created = store.create(*maybe_name, note);
            crow::response r{created.to_json()};
            r.code = 201;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/projects/<string>/note").methods(crow::HTTPMethod::PATCH)([&store](const crow::request &req, const std::string &uuid){
        try {
            auto maybe_note = extract_string_field(req.body, "note");
            if (!maybe_note) {
                crow::response r{"{\"error\":\"invalid json\"}"};
                r.code = 400;
                set_json_headers(r);
                return r;
            }
            auto updated = store.patch(uuid, *maybe_note);
            crow::response r{updated.to_json()};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::runtime_error &re) {
            crow::response r{std::string("{\"error\":\"") + re.what() + "\"}"};
            r.code = 404;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/projects/<string>/uninit").methods(crow::HTTPMethod::POST)([require_project_dir](const std::string &uuid){
        try {
            return uninit_project_dir_response(require_project_dir(uuid));
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/upload").methods(crow::HTTPMethod::POST)([require_project_dir](const crow::request &req, const std::string &uuid){
        try {
            return upload_to_project_dir_response(req, require_project_dir(uuid));
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/inited").methods(crow::HTTPMethod::POST)([require_project_dir](const crow::request &req, const std::string &uuid){
        try {
            return inited_project_dir_response(req, require_project_dir(uuid), uuid);
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/start_analysis").methods(crow::HTTPMethod::POST)([require_project_dir, onnx_path, infer_threads](const crow::request &req, const std::string &uuid){
        try {
            return start_analysis_project_dir_response(req, require_project_dir(uuid), uuid, onnx_path, infer_threads);
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/projects/<string>/semi").methods(crow::HTTPMethod::PATCH)([require_project_dir](const crow::request &req, const std::string &uuid){
        try {
            return patch_semi_project_dir_response(req, require_project_dir(uuid));
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/png").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            return make_file_list_response(require_project_dir(uuid) / "png");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/png/<string>").methods(crow::HTTPMethod::GET)([require_project_dir, ensure_safe_filename](const std::string &uuid, const std::string &filename){
        try {
            ensure_safe_filename(filename);
            return make_binary_file_response(require_project_dir(uuid) / "png" / filename, "image/png");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/markedpng").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            return make_file_list_response(require_project_dir(uuid) / "markedpng");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/markedpng/<string>").methods(crow::HTTPMethod::GET)([require_project_dir, ensure_safe_filename](const std::string &uuid, const std::string &filename){
        try {
            ensure_safe_filename(filename);
            return make_binary_file_response(require_project_dir(uuid) / "markedpng" / filename, "image/png");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/processed/png").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            return make_file_list_response(require_project_dir(uuid) / "processed" / "pngs");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/processed/png/<string>").methods(crow::HTTPMethod::GET)([require_project_dir, ensure_safe_filename](const std::string &uuid, const std::string &filename){
        try {
            ensure_safe_filename(filename);
            return make_binary_file_response(require_project_dir(uuid) / "processed" / "pngs" / filename, "image/png");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/png").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求PNG压缩包: uuid=" + uuid);
            return make_zip_response(require_project_dir(uuid) / "png", uuid + "_png.zip", "png.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/markedpng").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求markedPNG压缩包: uuid=" + uuid);
            return make_zip_response(require_project_dir(uuid) / "markedpng", uuid + "_markedpng.zip", "markedpng.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/fused/png").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        fs::path fused_dir;
        try {
            RuntimeLogger::info("[下载] 请求PNG与markedPNG融合压缩包: uuid=" + uuid);
            fs::path project_dir = require_project_dir(uuid);
            fused_dir = build_fused_png_temp_dir(project_dir / "png", project_dir / "markedpng", uuid + "_fused_png");
            auto response = make_zip_response(fused_dir, uuid + "_fused_png.zip", "png_markedpng_fused.zip");
            std::error_code ec;
            fs::remove_all(fused_dir, ec);
            return response;
        } catch (const std::exception &e) {
            std::error_code ec;
            if (!fused_dir.empty()) fs::remove_all(fused_dir, ec);
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/npz").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求NPZ压缩包: uuid=" + uuid);
            return make_zip_response(require_project_dir(uuid) / "npz", uuid + "_npz.zip", "npz.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/dcm").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求DCM压缩包: uuid=" + uuid);
            fs::path project_dir = require_project_dir(uuid);
            fs::path dir = project_dir / "dcm";
            if (list_files(dir).empty()) {
                RuntimeLogger::info("[下载触发转换] [npz转dcm] 目录为空，开始按需转换: uuid=" + uuid);
                ensure_converted_from_npz_dir(project_dir / "npz", dir, "dcm", "image");
                update_project_json_fields(project_dir / "project.json", {{"dcm", "true"}});
            }
            return make_zip_response(dir, uuid + "_dcm.zip", "dcm.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/nii").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求NII压缩包: uuid=" + uuid);
            fs::path project_dir = require_project_dir(uuid);
            fs::path dir = project_dir / "nii";
            if (list_files(dir).empty()) {
                RuntimeLogger::info("[下载触发转换] [npz转nii] 目录为空，开始按需转换: uuid=" + uuid);
                ensure_converted_from_npz_dir(project_dir / "npz", dir, "nii", "image");
                update_project_json_fields(project_dir / "project.json", {{"nii", "true"}});
            }
            return make_zip_response(dir, uuid + "_nii.zip", "nii.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/processed/png").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            return make_zip_response(require_project_dir(uuid) / "processed" / "pngs", uuid + "_processed_png.zip", "processed_png.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/processed/markedpng").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求processed markedPNG压缩包: uuid=" + uuid);
            return make_zip_response(require_project_dir(uuid) / "processed" / "pngs", uuid + "_processed_markedpng.zip", "processed_markedpng.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/processed/fused/png").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        fs::path fused_dir;
        try {
            RuntimeLogger::info("[下载] 请求processed PNG与markedPNG融合压缩包: uuid=" + uuid);
            fs::path project_dir = require_project_dir(uuid);
            fused_dir = build_fused_png_temp_dir(project_dir / "png", project_dir / "processed" / "pngs", uuid + "_processed_fused_png");
            auto response = make_zip_response(fused_dir, uuid + "_processed_fused_png.zip", "processed_png_markedpng_fused.zip");
            std::error_code ec;
            fs::remove_all(fused_dir, ec);
            return response;
        } catch (const std::exception &e) {
            std::error_code ec;
            if (!fused_dir.empty()) fs::remove_all(fused_dir, ec);
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/processed/npz").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            return make_zip_response(require_project_dir(uuid) / "processed" / "npzs", uuid + "_processed_npz.zip", "processed_npz.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/processed/dcm").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求processed DCM压缩包: uuid=" + uuid);
            fs::path project_dir = require_project_dir(uuid);
            fs::path dir = project_dir / "processed" / "dcm";
            if (list_files(dir).empty()) {
                RuntimeLogger::info("[下载触发转换] [npz转dcm] processed目录为空，开始按需转换: uuid=" + uuid);
                ensure_converted_from_npz_dir(project_dir / "processed" / "npzs", dir, "dcm", "label");
                update_project_json_fields(project_dir / "project.json", {{"PD-dcm", "true"}});
            }
            return make_zip_response(dir, uuid + "_processed_dcm.zip", "processed_dcm.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/processed/nii").methods(crow::HTTPMethod::GET)([require_project_dir](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求processed NII压缩包: uuid=" + uuid);
            fs::path project_dir = require_project_dir(uuid);
            fs::path dir = project_dir / "processed" / "nii";
            if (list_files(dir).empty()) {
                RuntimeLogger::info("[下载触发转换] [npz转nii] processed目录为空，开始按需转换: uuid=" + uuid);
                ensure_converted_from_npz_dir(project_dir / "processed" / "npzs", dir, "nii", "label");
                update_project_json_fields(project_dir / "project.json", {{"PD-nii", "true"}});
            }
            return make_zip_response(dir, uuid + "_processed_nii.zip", "processed_nii.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });
}
