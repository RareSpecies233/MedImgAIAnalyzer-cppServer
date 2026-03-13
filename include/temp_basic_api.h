#pragma once

template <typename App>
inline void register_temp_basic_routes(App &app,
                                       InfoStore &store,
                                       const std::string &onnx_path,
                                       int infer_threads)
{
    CROW_ROUTE(app, "/api/temp/create").methods(crow::HTTPMethod::POST)([&store](const crow::request &req) {
        try {
            const std::string temp_uuid = generate_uuid_v4();
            fs::path dir = temp_project_dir(store, temp_uuid);
            fs::create_directories(dir);
            write_default_project_json_file(dir / "project.json", temp_uuid);
            crow::json::wvalue res;
            res["tempUUID"] = temp_uuid;
            auto maybe_name = extract_string_field(req.body, "name");
            if (maybe_name) res["name"] = *maybe_name;
            crow::response r{res};
            r.code = 201;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/convert").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &temp_uuid) {
        try {
            fs::path src_dir = require_temp_project_dir(store, temp_uuid);
            auto maybe_name = extract_string_field(req.body, "name");
            auto maybe_note = extract_string_field(req.body, "note");
            const std::string name = maybe_name ? *maybe_name : (std::string("temp-") + temp_uuid.substr(0, 8));
            const std::string note = maybe_note ? *maybe_note : std::string("");
            Project created = store.create(name, note);
            fs::path dst_dir = store.base_path / created.uuid;
            std::error_code ec;
            fs::remove_all(dst_dir, ec);
            fs::rename(src_dir, dst_dir, ec);
            if (ec) throw std::runtime_error("转换临时项目失败: " + ec.message());
            update_project_json_fields(dst_dir / "project.json", {{"uuid", "\"" + created.uuid + "\""}});
            crow::response r{created.to_json()};
            r.code = 201;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/project.json").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            crow::response r{read_text_file(dir / "project.json")};
            r.code = 200;
            set_json_headers(r);
            return r;
        } catch (const std::exception &e) {
            return make_json_error_response(e.what(), 404);
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/uninit").methods(crow::HTTPMethod::POST)([&store](const std::string &temp_uuid) {
        try {
            return uninit_project_dir_response(require_temp_project_dir(store, temp_uuid));
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/upload").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &temp_uuid) {
        try {
            return upload_to_project_dir_response(req, require_temp_project_dir(store, temp_uuid));
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/inited").methods(crow::HTTPMethod::POST)([&store](const crow::request &req, const std::string &temp_uuid) {
        try {
            return inited_project_dir_response(req, require_temp_project_dir(store, temp_uuid), std::string("temp:") + temp_uuid);
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/start_analysis").methods(crow::HTTPMethod::POST)([&store, onnx_path, infer_threads](const crow::request &req, const std::string &temp_uuid) {
        try {
            return start_analysis_project_dir_response(req,
                                                       require_temp_project_dir(store, temp_uuid),
                                                       std::string("temp:") + temp_uuid,
                                                       onnx_path,
                                                       infer_threads);
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        } catch (...) {
            return make_json_error_response("start_analysis发生未知错误", 500);
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/semi").methods(crow::HTTPMethod::PATCH)([&store](const crow::request &req, const std::string &temp_uuid) {
        try {
            return patch_semi_project_dir_response(req, require_temp_project_dir(store, temp_uuid));
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_file_list_response(require_temp_project_dir(store, temp_uuid) / "png");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/png/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid, const std::string &filename) {
        try {
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            return make_binary_file_response(require_temp_project_dir(store, temp_uuid) / "png" / filename, "image/png");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_file_list_response(require_temp_project_dir(store, temp_uuid) / "markedpng");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/markedpng/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid, const std::string &filename) {
        try {
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            return make_binary_file_response(require_temp_project_dir(store, temp_uuid) / "markedpng" / filename, "image/png");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/processed/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_file_list_response(require_temp_project_dir(store, temp_uuid) / "processed" / "pngs");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/processed/png/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid, const std::string &filename) {
        try {
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            return make_binary_file_response(require_temp_project_dir(store, temp_uuid) / "processed" / "pngs" / filename, "image/png");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_zip_response(require_temp_project_dir(store, temp_uuid) / "png", temp_uuid + "_temp_png.zip", "png.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_zip_response(require_temp_project_dir(store, temp_uuid) / "markedpng", temp_uuid + "_temp_markedpng.zip", "markedpng.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/fused/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        fs::path fused_dir;
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            fused_dir = build_fused_png_temp_dir(dir / "png", dir / "markedpng", temp_uuid + "_temp_fused_png");
            auto response = make_zip_response(fused_dir, temp_uuid + "_temp_fused_png.zip", "png_markedpng_fused.zip");
            std::error_code ec;
            fs::remove_all(fused_dir, ec);
            return response;
        } catch (const std::exception &e) {
            std::error_code ec;
            if (!fused_dir.empty()) fs::remove_all(fused_dir, ec);
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/npz").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_zip_response(require_temp_project_dir(store, temp_uuid) / "npz", temp_uuid + "_temp_npz.zip", "npz.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/dcm").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            fs::path dcm_dir = dir / "dcm";
            if (list_files(dcm_dir).empty()) {
                ensure_converted_from_npz_dir(dir / "npz", dcm_dir, "dcm", "image");
                update_project_json_fields(dir / "project.json", {{"dcm", "true"}});
            }
            return make_zip_response(dcm_dir, temp_uuid + "_temp_dcm.zip", "dcm.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/nii").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            fs::path nii_dir = dir / "nii";
            if (list_files(nii_dir).empty()) {
                ensure_converted_from_npz_dir(dir / "npz", nii_dir, "nii", "image");
                update_project_json_fields(dir / "project.json", {{"nii", "true"}});
            }
            return make_zip_response(nii_dir, temp_uuid + "_temp_nii.zip", "nii.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/processed/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_zip_response(require_temp_project_dir(store, temp_uuid) / "processed" / "pngs", temp_uuid + "_temp_processed_png.zip", "processed_png.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/processed/markedpng").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_zip_response(require_temp_project_dir(store, temp_uuid) / "processed" / "pngs", temp_uuid + "_temp_processed_markedpng.zip", "processed_markedpng.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/processed/fused/png").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        fs::path fused_dir;
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            fused_dir = build_fused_png_temp_dir(dir / "png", dir / "processed" / "pngs", temp_uuid + "_temp_processed_fused_png");
            auto response = make_zip_response(fused_dir, temp_uuid + "_temp_processed_fused_png.zip", "processed_png_markedpng_fused.zip");
            std::error_code ec;
            fs::remove_all(fused_dir, ec);
            return response;
        } catch (const std::exception &e) {
            std::error_code ec;
            if (!fused_dir.empty()) fs::remove_all(fused_dir, ec);
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/processed/npz").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            return make_zip_response(require_temp_project_dir(store, temp_uuid) / "processed" / "npzs", temp_uuid + "_temp_processed_npz.zip", "processed_npz.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/processed/dcm").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            fs::path dcm_dir = dir / "processed" / "dcm";
            if (list_files(dcm_dir).empty()) {
                ensure_converted_from_npz_dir(dir / "processed" / "npzs", dcm_dir, "dcm", "label");
                update_project_json_fields(dir / "project.json", {{"PD-dcm", "true"}});
            }
            return make_zip_response(dcm_dir, temp_uuid + "_temp_processed_dcm.zip", "processed_dcm.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });

    CROW_ROUTE(app, "/api/temp/<string>/download/processed/nii").methods(crow::HTTPMethod::GET)([&store](const std::string &temp_uuid) {
        try {
            fs::path dir = require_temp_project_dir(store, temp_uuid);
            fs::path nii_dir = dir / "processed" / "nii";
            if (list_files(nii_dir).empty()) {
                ensure_converted_from_npz_dir(dir / "processed" / "npzs", nii_dir, "nii", "label");
                update_project_json_fields(dir / "project.json", {{"PD-nii", "true"}});
            }
            return make_zip_response(nii_dir, temp_uuid + "_temp_processed_nii.zip", "processed_nii.zip");
        } catch (const std::exception &e) {
            return make_json_error_response(e.what());
        }
    });
}