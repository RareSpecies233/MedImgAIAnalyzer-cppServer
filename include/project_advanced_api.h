#pragma once

template <typename App>
inline void register_project_advanced_routes(App &app, InfoStore &store)
{
    CROW_ROUTE(app, "/api/project/<string>/start_enhdb").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &uuid){
        try {
            RuntimeLogger::info("[高级增强] 开始: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");

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

            fs::path project_dir = store.base_path / uuid;
            fs::path input_npz_dir = project_dir / "npz";
            auto files = list_files(input_npz_dir);
            if (files.empty()) {
                throw std::runtime_error("npz为空");
            }

            fs::path enh_dir = project_dir / "enhDBprocessed";
            fs::path enh_npz_dir = enh_dir / "npzs";
            fs::path enh_png_dir = enh_dir / "pngs";
            fs::path enh_marked_dir = enh_dir / "markedpngs";
            std::error_code ec;
            fs::remove_all(enh_dir, ec);
            fs::create_directories(enh_npz_dir);
            fs::create_directories(enh_png_dir);
            fs::create_directories(enh_marked_dir);
            RuntimeLogger::info("[高级增强] 已重置输出目录: " + enh_dir.string());
            RuntimeLogger::info("[高级增强] 参数: scale_x=" + std::to_string(args.scale_x) +
                                ", scale_y=" + std::to_string(args.scale_y) +
                                ", rotate=" + std::to_string(args.rotate_deg) +
                                ", contrast=" + std::to_string(args.contrast) +
                                ", gamma=" + std::to_string(args.gamma) +
                                ", preserve_resolution=" + std::string(args.preserve_resolution ? "true" : "false"));

            size_t processed_count = 0;
            for (const auto &src : files) {
                if (to_lower_copy(src.extension().string()) != ".npz") continue;
                npzproc::Args file_args = args;
                file_args.input = src;
                file_args.output = enh_npz_dir / src.filename();
                RuntimeLogger::info("[高级增强] 处理文件: " + src.filename().string());
                npzproc::process_npz_file(file_args);
                convert_npz_to_pngs(file_args.output, enh_png_dir, enh_marked_dir, true, true, "_marked");
                RuntimeLogger::info("[高级增强] 输出完成: " + file_args.output.filename().string());
                ++processed_count;
            }

            if (processed_count == 0) {
                throw std::runtime_error("npz目录中没有可处理的 npz 文件");
            }

            crow::json::wvalue res;
            res["status"] = "ok";
            res["processed"] = static_cast<int>(processed_count);
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

    CROW_ROUTE(app, "/api/project/<string>/enhdb/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_file_list_response(store.base_path / uuid / "enhDBprocessed" / "pngs");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/enhdb/png/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid, const std::string &filename){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            return make_binary_file_response(store.base_path / uuid / "enhDBprocessed" / "pngs" / filename, "image/png");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/enhdb/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_file_list_response(store.base_path / uuid / "enhDBprocessed" / "markedpngs");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/enhdb/markedpng/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid, const std::string &filename){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            return make_binary_file_response(store.base_path / uuid / "enhDBprocessed" / "markedpngs" / filename, "image/png");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/enhdb/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求高级增强 PNG 压缩包: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_zip_response(store.base_path / uuid / "enhDBprocessed" / "pngs", uuid + "_enhdb_png.zip", "enhdb_png.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/enhdb/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求高级增强 markedPNG 压缩包: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_zip_response(store.base_path / uuid / "enhDBprocessed" / "markedpngs", uuid + "_enhdb_markedpng.zip", "enhdb_markedpng.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/enhdb/fused/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        fs::path fused_dir;
        try {
            RuntimeLogger::info("[下载] 请求高级增强 PNG 与 markedPNG 融合压缩包: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fused_dir = build_fused_png_temp_dir(project_dir / "enhDBprocessed" / "pngs",
                                                project_dir / "enhDBprocessed" / "markedpngs",
                                                uuid + "_enhdb_fused_png");
            auto response = make_zip_response(fused_dir, uuid + "_enhdb_fused_png.zip", "enhdb_png_markedpng_fused.zip");
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

    CROW_ROUTE(app, "/api/project/<string>/download/enhdb/npz").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求高级增强 NPZ 压缩包: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_zip_response(store.base_path / uuid / "enhDBprocessed" / "npzs", uuid + "_enhdb_npz.zip", "enhdb_npz.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/enhdb/dcm").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求高级增强 DCM 压缩包: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path dir = project_dir / "enhDBprocessed" / "dcm";
            if (list_files(dir).empty()) {
                RuntimeLogger::info("[下载触发转换] [npz转dcm] 高级增强目录为空，开始按需转换: uuid=" + uuid);
                ensure_converted_from_npz_dir(project_dir / "enhDBprocessed" / "npzs", dir, "dcm", "image");
            }
            return make_zip_response(dir, uuid + "_enhdb_dcm.zip", "enhdb_dcm.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/enhdb/nii").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            RuntimeLogger::info("[下载] 请求高级增强 NII 压缩包: uuid=" + uuid);
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path dir = project_dir / "enhDBprocessed" / "nii";
            if (list_files(dir).empty()) {
                RuntimeLogger::info("[下载触发转换] [npz转nii] 高级增强目录为空，开始按需转换: uuid=" + uuid);
                ensure_converted_from_npz_dir(project_dir / "enhDBprocessed" / "npzs", dir, "nii", "image");
            }
            return make_zip_response(dir, uuid + "_enhdb_nii.zip", "enhdb_nii.zip");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/to_3d_model").methods(crow::HTTPMethod::POST)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return build_3d_model_project_dir_response(store.base_path / uuid, uuid, false);
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/3d").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_glb_download_response(store.base_path / uuid / "3d" / "model.glb");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });

    CROW_ROUTE(app, "/api/project/<string>/download/OG3d").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            return make_glb_download_response(store.base_path / uuid / "OG3d" / "model.glb");
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400;
            set_json_headers(r);
            return r;
        }
    });
}
