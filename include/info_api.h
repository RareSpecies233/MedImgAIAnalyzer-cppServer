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
#include <cmath>
#include <cctype>
#include <onnxruntime/onnxruntime_cxx_api.h>
#include "cnpy.h"
#include "info_store.h"
#include "npz_to_glb.h"

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

static inline void ensure_project_json_field(const fs::path &project_json,
                                             const std::string &key,
                                             const std::string &value_literal)
{
    std::string json = read_text_file(project_json);
    std::string k = "\"" + key + "\"";
    if (json.find(k) != std::string::npos) return;
    auto pos = json.rfind('}');
    if (pos == std::string::npos) throw std::runtime_error("project.json 字段格式错误");
    std::string insert = ",\n  \"" + key + "\": " + value_literal + "\n";
    json.insert(pos, insert);
    write_text_file(project_json, json);
}

static inline std::vector<double> npy_to_double_2d(const cnpy::NpyArray &arr, int &height, int &width)
{
    if (arr.shape.size() != 2) {
        throw std::runtime_error("Only 2D arrays supported");
    }
    const size_t h = arr.shape[0];
    const size_t w = arr.shape[1];
    height = static_cast<int>(h);
    width = static_cast<int>(w);
    std::vector<double> out(h * w, 0.0);

    auto read_value = [&](size_t idx) -> double {
        if (arr.word_size == sizeof(double)) {
            return arr.data<double>()[idx];
        }
        if (arr.word_size == sizeof(float)) {
            return static_cast<double>(arr.data<float>()[idx]);
        }
        if (arr.word_size == sizeof(uint8_t)) {
            return static_cast<double>(arr.data<uint8_t>()[idx]);
        }
        if (arr.word_size == sizeof(uint16_t)) {
            return static_cast<double>(arr.data<uint16_t>()[idx]);
        }
        if (arr.word_size == sizeof(int16_t)) {
            return static_cast<double>(arr.data<int16_t>()[idx]);
        }
        if (arr.word_size == sizeof(int32_t)) {
            return static_cast<double>(arr.data<int32_t>()[idx]);
        }
        throw std::runtime_error("Unsupported npy data type");
    };

    if (arr.fortran_order) {
        for (size_t r = 0; r < h; ++r) {
            for (size_t c = 0; c < w; ++c) {
                out[r * w + c] = read_value(c * h + r);
            }
        }
    } else {
        for (size_t r = 0; r < h; ++r) {
            for (size_t c = 0; c < w; ++c) {
                out[r * w + c] = read_value(r * w + c);
            }
        }
    }
    return out;
}

static inline cv::Mat normalize_to_u8(const std::vector<double> &data, int height, int width)
{
    if (data.empty()) throw std::runtime_error("Empty array");
    double min_v = data[0];
    double max_v = data[0];
    for (double v : data) {
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    const bool already_unit = (min_v >= 0.0 && max_v <= 1.0);
    cv::Mat img(height, width, CV_8UC1);
    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            double v = data[static_cast<size_t>(r) * width + c];
            if (already_unit) {
                v = std::min(1.0, std::max(0.0, v));
                img.at<uchar>(r, c) = static_cast<uchar>(v * 255.0 + 0.5);
            } else if (max_v > min_v) {
                double nv = (v - min_v) / (max_v - min_v);
                nv = std::min(1.0, std::max(0.0, nv));
                img.at<uchar>(r, c) = static_cast<uchar>(nv * 255.0 + 0.5);
            } else {
                img.at<uchar>(r, c) = static_cast<uchar>(std::min(255.0, std::max(0.0, v)));
            }
        }
    }
    return img;
}

static inline const cnpy::NpyArray *find_npz_array(const cnpy::npz_t &npz,
                                                   const std::vector<std::string> &keys)
{
    for (const auto &k : keys) {
        auto it = npz.find(k);
        if (it != npz.end()) return &it->second;
    }
    return nullptr;
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

static inline void convert_npz_to_pngs(const fs::path &npz_path,
                                       const fs::path &png_dir,
                                       const fs::path &marked_dir,
                                       bool marked,
                                       bool write_raw_png = true,
                                       const std::string &marked_suffix = "_marked")
{
    static const std::vector<std::string> kRawKeys = {
        "image", "img", "raw", "ct", "data", "slice", "input"
    };
    static const std::vector<std::string> kAnnKeys = {
        "label", "mask", "seg", "annotation", "gt"
    };

    cnpy::npz_t npz = cnpy::npz_load(npz_path.string());
    if (npz.empty()) throw std::runtime_error("npz为空");

    const cnpy::NpyArray *raw_arr = find_npz_array(npz, kRawKeys);
    const cnpy::NpyArray *ann_arr = find_npz_array(npz, kAnnKeys);
    if (!raw_arr) {
        raw_arr = &npz.begin()->second;
    }
    if (!ann_arr) {
        for (const auto &kv : npz) {
            if (&kv.second != raw_arr) {
                ann_arr = &kv.second;
                break;
            }
        }
    }

    int h = 0;
    int w = 0;
    std::vector<double> raw_data = npy_to_double_2d(*raw_arr, h, w);
    cv::Mat raw_u8 = normalize_to_u8(raw_data, h, w);

    if (write_raw_png) {
        fs::create_directories(png_dir);
        fs::path out_png = png_dir / (npz_path.stem().string() + ".png");
        if (!cv::imwrite(out_png.string(), raw_u8)) throw std::runtime_error("写入png失败");
    }

    if (marked) {
        fs::create_directories(marked_dir);
        std::vector<double> ann_data;
        int ann_h = 0;
        int ann_w = 0;
        if (ann_arr) {
            ann_data = npy_to_double_2d(*ann_arr, ann_h, ann_w);
        } else {
            ann_h = h;
            ann_w = w;
            ann_data.assign(static_cast<size_t>(h) * w, 0.0);
        }
        if (ann_h != h || ann_w != w) {
            throw std::runtime_error("标注尺寸与原图不一致");
        }

        const uchar alpha = 160;
        cv::Mat rgba(h, w, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        for (int r = 0; r < h; ++r) {
            for (int c = 0; c < w; ++c) {
                double v = ann_data[static_cast<size_t>(r) * w + c];
                if (v > 1.0) {
                    rgba.at<cv::Vec4b>(r, c) = cv::Vec4b(0, 212, 255, alpha);
                } else if (v > 0.0) {
                    rgba.at<cv::Vec4b>(r, c) = cv::Vec4b(59, 59, 255, alpha);
                }
            }
        }
        fs::path out_marked = marked_dir / (npz_path.stem().string() + marked_suffix + ".png");
        if (!cv::imwrite(out_marked.string(), rgba)) throw std::runtime_error("写入markedpng失败");
    }
}

static inline bool is_valid_crop(int xL, int xR, int yL, int yR, int width, int height)
{
    return xL >= 0 && yL >= 0 && xR > xL && yR > yL && xR <= width && yR <= height;
}

template <typename T>
static inline std::vector<T> crop2d(const T *src,
                                    int height,
                                    int width,
                                    int xL,
                                    int xR,
                                    int yL,
                                    int yR,
                                    bool fortran_order)
{
    int out_w = xR - xL;
    int out_h = yR - yL;
    std::vector<T> out(static_cast<size_t>(out_h) * out_w);
    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            int src_x = xL + x;
            int src_y = yL + y;
            size_t idx = fortran_order ? static_cast<size_t>(src_x) * height + src_y
                                       : static_cast<size_t>(src_y) * width + src_x;
            out[static_cast<size_t>(y) * out_w + x] = src[idx];
        }
    }
    return out;
}

static inline std::vector<int64_t> resize_mask_nearest_from_double(const std::vector<double> &mask,
                                                                    int height,
                                                                    int width,
                                                                    int size)
{
    cv::Mat src(height, width, CV_64FC1, const_cast<double *>(mask.data()));
    cv::Mat dst;
    cv::resize(src, dst, cv::Size(size, size), 0, 0, cv::INTER_NEAREST);
    std::vector<int64_t> out(static_cast<size_t>(size) * size, 0);
    const double *p = dst.ptr<double>();
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<int64_t>(std::lround(p[i]));
    }
    return out;
}

static inline std::vector<int64_t> resize_mask_nearest_from_int(const std::vector<int64_t> &mask,
                                                                 int height,
                                                                 int width,
                                                                 int size)
{
    std::vector<float> tmp(static_cast<size_t>(height) * width, 0.0f);
    for (size_t i = 0; i < tmp.size(); ++i) {
        tmp[i] = static_cast<float>(mask[i]);
    }
    cv::Mat src(height, width, CV_32FC1, tmp.data());
    cv::Mat dst;
    cv::resize(src, dst, cv::Size(size, size), 0, 0, cv::INTER_NEAREST);
    std::vector<int64_t> out(static_cast<size_t>(size) * size, 0);
    const float *p = dst.ptr<float>();
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<int64_t>(std::lround(p[i]));
    }
    return out;
}

static inline void normalize_image_inplace(std::vector<double> &image)
{
    double max_val = 0.0;
    for (double v : image) {
        if (v > max_val) max_val = v;
    }
    if (max_val > 1.0) {
        for (double &v : image) {
            v /= 255.0;
        }
    }
}

static inline std::vector<double> resize_image(const std::vector<double> &image,
                                                int height,
                                                int width,
                                                int size)
{
    cv::Mat src(height, width, CV_64FC1, const_cast<double *>(image.data()));
    cv::Mat dst;
    cv::resize(src, dst, cv::Size(size, size), 0, 0, cv::INTER_LINEAR);
    std::vector<double> out(static_cast<size_t>(size) * size, 0.0);
    std::memcpy(out.data(), dst.ptr<double>(), out.size() * sizeof(double));
    return out;
}

static inline std::vector<float> make_input_tensor_chw(const std::vector<double> &image,
                                                       int size,
                                                       int channels)
{
    std::vector<float> input(static_cast<size_t>(channels) * size * size, 0.0f);
    const size_t hw = static_cast<size_t>(size) * size;
    for (int c = 0; c < channels; ++c) {
        for (size_t i = 0; i < hw; ++i) {
            input[static_cast<size_t>(c) * hw + i] = static_cast<float>(image[i]);
        }
    }
    return input;
}

static inline std::vector<int64_t> run_onnx_inference_mask(const fs::path &onnx_path,
                                                           const cnpy::NpyArray &raw_arr,
                                                           int img_size,
                                                           int out_size)
{
    int height = 0;
    int width = 0;
    std::vector<double> raw = npy_to_double_2d(raw_arr, height, width);
    normalize_image_inplace(raw);
    std::vector<double> resized = resize_image(raw, height, width, img_size);
    std::vector<float> input_chw = make_input_tensor_chw(resized, img_size, 3);

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "medimg_infer");
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    Ort::Session session(env, onnx_path.string().c_str(), opts);

    Ort::AllocatorWithDefaultOptions allocator;
    Ort::AllocatedStringPtr input_name = session.GetInputNameAllocated(0, allocator);
    std::vector<int64_t> input_shape = {1, 3, img_size, img_size};
    Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        mem_info, input_chw.data(), input_chw.size(), input_shape.data(), input_shape.size());

    size_t output_count = session.GetOutputCount();
    std::vector<Ort::AllocatedStringPtr> output_names;
    std::vector<const char *> output_name_cstrs;
    output_names.reserve(output_count);
    output_name_cstrs.reserve(output_count);
    for (size_t i = 0; i < output_count; ++i) {
        output_names.emplace_back(session.GetOutputNameAllocated(i, allocator));
        output_name_cstrs.push_back(output_names.back().get());
    }

    const char *input_name_cstr = input_name.get();
    std::vector<Ort::Value> outputs = session.Run(Ort::RunOptions{nullptr},
                                                  &input_name_cstr,
                                                  &input_tensor,
                                                  1,
                                                  output_name_cstrs.data(),
                                                  output_count);
    if (outputs.empty()) {
        throw std::runtime_error("ONNX输出为空");
    }

    Ort::Value &out = outputs[0];
    auto out_info = out.GetTensorTypeAndShapeInfo();
    auto out_shape = out_info.GetShape();
    if (out_shape.size() != 4) {
        throw std::runtime_error("ONNX输出维度不符合预期");
    }

    int64_t out_n = out_shape[0];
    int64_t out_c = out_shape[1];
    int64_t out_h = out_shape[2];
    int64_t out_w = out_shape[3];
    if (out_n != 1) {
        throw std::runtime_error("ONNX输出batch不为1");
    }

    auto half_to_float = [](uint16_t h) -> float {
        uint32_t sign = (h & 0x8000u) << 16;
        uint32_t exp = (h & 0x7C00u) >> 10;
        uint32_t mant = (h & 0x03FFu);
        uint32_t f;
        if (exp == 0) {
            if (mant == 0) {
                f = sign;
            } else {
                exp = 1;
                while ((mant & 0x0400u) == 0) {
                    mant <<= 1;
                    exp--;
                }
                mant &= 0x03FFu;
                exp = exp + (127 - 15);
                f = sign | (exp << 23) | (mant << 13);
            }
        } else if (exp == 0x1F) {
            f = sign | 0x7F800000u | (mant << 13);
        } else {
            exp = exp + (127 - 15);
            f = sign | (exp << 23) | (mant << 13);
        }
        float out_f;
        std::memcpy(&out_f, &f, sizeof(float));
        return out_f;
    };

    std::vector<float> out_fallback;
    const float *out_data = nullptr;
    auto out_type = out_info.GetElementType();
    if (out_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        out_data = out.GetTensorData<float>();
    } else if (out_type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
        const uint16_t *src = out.GetTensorData<uint16_t>();
        size_t total = static_cast<size_t>(out_n * out_c * out_h * out_w);
        out_fallback.resize(total);
        for (size_t i = 0; i < total; ++i) {
            out_fallback[i] = half_to_float(src[i]);
        }
        out_data = out_fallback.data();
    } else {
        throw std::runtime_error("不支持的ONNX输出数据类型");
    }

    std::vector<int64_t> pred(static_cast<size_t>(out_h) * out_w, 0);
    float out_min = out_data[0];
    float out_max = out_data[0];
    size_t total_vals = static_cast<size_t>(out_n * out_c * out_h * out_w);
    for (size_t i = 1; i < total_vals; ++i) {
        float v = out_data[i];
        if (v < out_min) out_min = v;
        if (v > out_max) out_max = v;
    }

    if (out_c == 1) {
        bool already_prob = (out_min >= 0.0f && out_max <= 1.0f);
        for (int64_t y = 0; y < out_h; ++y) {
            for (int64_t x = 0; x < out_w; ++x) {
                float v = out_data[y * out_w + x];
                float prob = already_prob ? v : (1.0f / (1.0f + std::exp(-v)));
                pred[static_cast<size_t>(y) * out_w + x] = (prob >= 0.5f) ? 1 : 0;
            }
        }
    } else {
        for (int64_t y = 0; y < out_h; ++y) {
            for (int64_t x = 0; x < out_w; ++x) {
                int64_t best_c = 0;
                float best_v = out_data[(0 * out_c + 0) * out_h * out_w + y * out_w + x];
                for (int64_t c = 1; c < out_c; ++c) {
                    float v = out_data[(0 * out_c + c) * out_h * out_w + y * out_w + x];
                    if (v > best_v) {
                        best_v = v;
                        best_c = c;
                    }
                }
                pred[static_cast<size_t>(y) * out_w + x] = best_c;
            }
        }
    }

    return resize_mask_nearest_from_int(pred, static_cast<int>(out_h), static_cast<int>(out_w), out_size);
}

static inline void save_npz_with_same_keys(const std::string &src_npz,
                                           const std::string &out_npz,
                                           const std::vector<int64_t> &pred,
                                           int pred_h,
                                           int pred_w,
                                           const std::string &label_key,
                                           int crop_xL,
                                           int crop_xR,
                                           int crop_yL,
                                           int crop_yR)
{
    cnpy::npz_t npz = cnpy::npz_load(src_npz);
    bool first = true;
    bool has_valid_crop = false;
    int crop_w = pred_w;
    int crop_h = pred_h;

    auto resolve_crop = [&](int width, int height) {
        if (is_valid_crop(crop_xL, crop_xR, crop_yL, crop_yR, width, height)) {
            has_valid_crop = true;
            crop_w = crop_xR - crop_xL;
            crop_h = crop_yR - crop_yL;
        }
    };

    for (const auto &kv : npz) {
        const std::string &key = kv.first;
        const cnpy::NpyArray &arr = kv.second;
        const std::string mode = first ? "w" : "a";
        first = false;

        if (key == label_key) {
            if (arr.shape.size() != 2) throw std::runtime_error("label应为2D数组");
            resolve_crop(static_cast<int>(arr.shape[1]), static_cast<int>(arr.shape[0]));
            std::vector<size_t> shape = {static_cast<size_t>(crop_h), static_cast<size_t>(crop_w)};
            std::vector<int64_t> out = pred;
            if (has_valid_crop) {
                out = crop2d(pred.data(), pred_h, pred_w, crop_xL, crop_xR, crop_yL, crop_yR, false);
            }
            switch (arr.word_size) {
                case sizeof(double): {
                    std::vector<double> tmp(out.size());
                    for (size_t i = 0; i < out.size(); ++i) tmp[i] = static_cast<double>(out[i]);
                    cnpy::npz_save(out_npz, key, tmp.data(), shape, mode);
                    break;
                }
                case sizeof(float): {
                    std::vector<float> tmp(out.size());
                    for (size_t i = 0; i < out.size(); ++i) tmp[i] = static_cast<float>(out[i]);
                    cnpy::npz_save(out_npz, key, tmp.data(), shape, mode);
                    break;
                }
                case sizeof(uint16_t): {
                    std::vector<uint16_t> tmp(out.size());
                    for (size_t i = 0; i < out.size(); ++i) tmp[i] = static_cast<uint16_t>(out[i]);
                    cnpy::npz_save(out_npz, key, tmp.data(), shape, mode);
                    break;
                }
                case sizeof(uint8_t): {
                    std::vector<uint8_t> tmp(out.size());
                    for (size_t i = 0; i < out.size(); ++i) tmp[i] = static_cast<uint8_t>(out[i]);
                    cnpy::npz_save(out_npz, key, tmp.data(), shape, mode);
                    break;
                }
                default:
                    throw std::runtime_error("不支持的label数据类型");
            }
            continue;
        }

        if (arr.shape.size() == 2) {
            resolve_crop(static_cast<int>(arr.shape[1]), static_cast<int>(arr.shape[0]));
            int height = static_cast<int>(arr.shape[0]);
            int width = static_cast<int>(arr.shape[1]);
            int out_h = has_valid_crop ? crop_h : height;
            int out_w = has_valid_crop ? crop_w : width;
            std::vector<size_t> shape = {static_cast<size_t>(out_h), static_cast<size_t>(out_w)};
            switch (arr.word_size) {
                case sizeof(double): {
                    std::vector<double> out = has_valid_crop
                                              ? crop2d(arr.data<double>(), height, width, crop_xL, crop_xR, crop_yL, crop_yR,
                                                       arr.fortran_order)
                                              : std::vector<double>(arr.data<double>(), arr.data<double>() + arr.num_vals);
                    cnpy::npz_save(out_npz, key, out.data(), shape, mode);
                    break;
                }
                case sizeof(float): {
                    std::vector<float> out = has_valid_crop
                                             ? crop2d(arr.data<float>(), height, width, crop_xL, crop_xR, crop_yL, crop_yR,
                                                      arr.fortran_order)
                                             : std::vector<float>(arr.data<float>(), arr.data<float>() + arr.num_vals);
                    cnpy::npz_save(out_npz, key, out.data(), shape, mode);
                    break;
                }
                case sizeof(uint16_t): {
                    std::vector<uint16_t> out = has_valid_crop
                                                ? crop2d(arr.data<uint16_t>(), height, width, crop_xL, crop_xR, crop_yL, crop_yR,
                                                         arr.fortran_order)
                                                : std::vector<uint16_t>(arr.data<uint16_t>(), arr.data<uint16_t>() + arr.num_vals);
                    cnpy::npz_save(out_npz, key, out.data(), shape, mode);
                    break;
                }
                case sizeof(uint8_t): {
                    std::vector<uint8_t> out = has_valid_crop
                                               ? crop2d(arr.data<uint8_t>(), height, width, crop_xL, crop_xR, crop_yL, crop_yR,
                                                        arr.fortran_order)
                                               : std::vector<uint8_t>(arr.data<uint8_t>(), arr.data<uint8_t>() + arr.num_vals);
                    cnpy::npz_save(out_npz, key, out.data(), shape, mode);
                    break;
                }
                default:
                    throw std::runtime_error("不支持的npz数据类型");
            }
        } else {
            switch (arr.word_size) {
                case sizeof(double):
                    cnpy::npz_save(out_npz, key, arr.data<double>(), arr.shape, mode);
                    break;
                case sizeof(float):
                    cnpy::npz_save(out_npz, key, arr.data<float>(), arr.shape, mode);
                    break;
                case sizeof(uint16_t):
                    cnpy::npz_save(out_npz, key, arr.data<uint16_t>(), arr.shape, mode);
                    break;
                case sizeof(uint8_t):
                    cnpy::npz_save(out_npz, key, arr.data<uint8_t>(), arr.shape, mode);
                    break;
                default:
                    throw std::runtime_error("不支持的npz数据类型");
            }
        }
    }

    if (npz.find(label_key) == npz.end()) {
        int out_h = pred_h;
        int out_w = pred_w;
        std::vector<int64_t> out = pred;
        if (is_valid_crop(crop_xL, crop_xR, crop_yL, crop_yR, pred_w, pred_h)) {
            out_h = crop_yR - crop_yL;
            out_w = crop_xR - crop_xL;
            out = crop2d(pred.data(), pred_h, pred_w, crop_xL, crop_xR, crop_yL, crop_yR, false);
        }
        std::vector<size_t> shape = {static_cast<size_t>(out_h), static_cast<size_t>(out_w)};
        cnpy::npz_save(out_npz, label_key, out.data(), shape, first ? "w" : "a");
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

inline void register_info_routes(crow::SimpleApp &app, InfoStore &store, const std::string &onnx_path) {
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
                        convert_npz_to_pngs(src, png_dir, marked_dir, true, true, "_marked");
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

    // 开始推理（处理 npz）
    CROW_ROUTE(app, "/api/project/<string>/start_analysis").methods(crow::HTTPMethod::POST)([&store, onnx_path](const crow::request &req, const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            if (onnx_path.empty()) {
                throw std::runtime_error("未指定onnx文件，无法使用推理功能");
            }
            if (!fs::exists(onnx_path)) {
                throw std::runtime_error("onnx文件不存在: " + onnx_path);
            }
            auto mode = extract_string_field(req.body, "mode");
            if (!mode) mode = extract_string_field(req.body, "PD");
            if (!mode) mode = extract_string_field(req.body, "type");
            if (!mode) throw std::runtime_error("missing mode");
            std::string mode_val = to_lower_copy(*mode);
            if (mode_val != "raw" && mode_val != "semi") {
                throw std::runtime_error("invalid mode");
            }

            fs::path project_dir = store.base_path / uuid;
            fs::path project_json = project_dir / "project.json";
            std::string json = read_text_file(project_json);
            ensure_project_json_field(project_json, "processed", "false");
            int semi_xL = extract_int_field(json, "semi-xL").value_or(-1);
            int semi_xR = extract_int_field(json, "semi-xR").value_or(-1);
            int semi_yL = extract_int_field(json, "semi-yL").value_or(-1);
            int semi_yR = extract_int_field(json, "semi-yR").value_or(-1);

            fs::path input_npz_dir = project_dir / "npz";
            auto npz_files = list_files(input_npz_dir);
            if (npz_files.empty()) throw std::runtime_error("npz为空");

            fs::path processed_dir = project_dir / "processed";
            fs::path processed_npz_dir = processed_dir / "npzs";
            fs::path processed_png_dir = processed_dir / "pngs";
            fs::path processed_dcm_dir = processed_dir / "dcm";
            fs::path processed_nii_dir = processed_dir / "nii";
            std::error_code ec;
            fs::remove_all(processed_dir, ec);
            fs::remove_all(project_dir / "3d", ec);
            fs::remove_all(project_dir / "OG3d", ec);
            fs::create_directories(processed_npz_dir);
            fs::create_directories(processed_png_dir);
            fs::create_directories(processed_dcm_dir);
            fs::create_directories(processed_nii_dir);

            const int out_size = 512;
            const int img_size = 224;
            for (const auto &src : npz_files) {
                cnpy::npz_t npz = cnpy::npz_load(src.string());
                const cnpy::NpyArray *raw_arr = find_npz_array(npz, {"image", "img", "raw", "ct", "data", "slice", "input"});
                if (!raw_arr) {
                    raw_arr = &npz.begin()->second;
                }
                if (!raw_arr || raw_arr->shape.size() != 2) {
                    throw std::runtime_error("npz中未找到2D原始图像");
                }

                std::vector<int64_t> pred = run_onnx_inference_mask(onnx_path, *raw_arr, img_size, out_size);

                bool has_crop = (mode_val == "semi") && is_valid_crop(semi_xL, semi_xR, semi_yL, semi_yR, out_size, out_size);
                int crop_xL = has_crop ? semi_xL : -1;
                int crop_xR = has_crop ? semi_xR : -1;
                int crop_yL = has_crop ? semi_yL : -1;
                int crop_yR = has_crop ? semi_yR : -1;

                fs::path out_npz = processed_npz_dir / (src.stem().string() + "-PD.npz");
                save_npz_with_same_keys(src.string(),
                                        out_npz.string(),
                                        pred,
                                        out_size,
                                        out_size,
                                        "label",
                                        crop_xL,
                                        crop_xR,
                                        crop_yL,
                                        crop_yR);

                convert_npz_to_pngs(out_npz, processed_png_dir, processed_png_dir, true, false, "");
            }

            update_project_json_fields(project_json, {
                {"processed", "\"" + mode_val + "\""},
                {"PD", "\"" + mode_val + "\""},
                {"PD-nii", "false"},
                {"PD-dcm", "false"},
                {"PD-3d", "false"}
            });

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

    // 获取 processed png 列表
    CROW_ROUTE(app, "/api/project/<string>/processed/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path png_dir = store.base_path / uuid / "processed" / "pngs";
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

    // 获取单张 processed png
    CROW_ROUTE(app, "/api/project/<string>/processed/png/<string>").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid, const std::string &filename){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos || filename.find('\\') != std::string::npos) {
                throw std::runtime_error("invalid filename");
            }
            fs::path png_path = store.base_path / uuid / "processed" / "pngs" / filename;
            if (!fs::exists(png_path)) throw std::runtime_error("processed png not found");
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
            bool all_minus_one = (*xl == -1 && *xr == -1 && *yl == -1 && *yr == -1);
            update_project_json_fields(project_json, {
                {"semi-xL", std::to_string(*xl)},
                {"semi-xR", std::to_string(*xr)},
                {"semi-yL", std::to_string(*yl)},
                {"semi-yR", std::to_string(*yr)},
                {"semi", all_minus_one ? "false" : "true"}
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
            fs::path dir = store.base_path / uuid / "dcm";
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
            fs::path dir = store.base_path / uuid / "nii";
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

    // 下载 processed png
    CROW_ROUTE(app, "/api/project/<string>/download/processed/png").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path dir = store.base_path / uuid / "processed" / "pngs";
            auto zip_path = create_zip_store(dir, uuid + "_processed_png.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"processed_png.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 processed npz
    CROW_ROUTE(app, "/api/project/<string>/download/processed/npz").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path dir = store.base_path / uuid / "processed" / "npzs";
            auto zip_path = create_zip_store(dir, uuid + "_processed_npz.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"processed_npz.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 processed dcm
    CROW_ROUTE(app, "/api/project/<string>/download/processed/dcm").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path dir = store.base_path / uuid / "processed" / "dcm";
            auto zip_path = create_zip_store(dir, uuid + "_processed_dcm.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"processed_dcm.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 processed nii
    CROW_ROUTE(app, "/api/project/<string>/download/processed/nii").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path dir = store.base_path / uuid / "processed" / "nii";
            auto zip_path = create_zip_store(dir, uuid + "_processed_nii.zip");
            crow::response r{read_text_file(zip_path)};
            r.set_header("Content-Type", "application/zip");
            r.set_header("Content-Disposition", "attachment; filename=\"processed_nii.zip\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 转换为 3d 模型
    CROW_ROUTE(app, "/api/project/<string>/to_3d_model").methods(crow::HTTPMethod::POST)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path project_dir = store.base_path / uuid;
            fs::path project_json = project_dir / "project.json";
            std::string json = read_text_file(project_json);

            fs::path processed_npz_dir = project_dir / "processed" / "npzs";
            if (list_files(processed_npz_dir).empty()) {
                throw std::runtime_error("processed npz 为空");
            }

            fs::path out_dir = project_dir / "3d";
            std::error_code ec;
            fs::remove_all(out_dir, ec);
            fs::create_directories(out_dir);

            npz_to_glb::Options opts;
            opts.use_raw_threshold = true;
            fs::path out_glb = out_dir / "model.glb";
            npz_to_glb::convert_directory_to_glb(processed_npz_dir, out_glb, opts);

            auto raw_val = extract_string_field(json, "raw");
            if (raw_val && to_lower_copy(*raw_val) == "markednpz") {
                fs::path og_dir = project_dir / "OG3d";
                fs::remove_all(og_dir, ec);
                fs::create_directories(og_dir);
                fs::path og_glb = og_dir / "model.glb";
                fs::path raw_npz_dir = project_dir / "npz";
                npz_to_glb::convert_directory_to_glb(raw_npz_dir, og_glb, opts);
            }

            update_project_json_fields(project_json, {{"PD-3d", "true"}});

            crow::response r{"{\"status\":\"ok\"}"};
            r.code = 200; set_json_headers(r); return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 3d 模型
    CROW_ROUTE(app, "/api/project/<string>/download/3d").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path glb_path = store.base_path / uuid / "3d" / "model.glb";
            if (!fs::exists(glb_path)) throw std::runtime_error("3d model not found");
            crow::response r{read_text_file(glb_path)};
            r.set_header("Content-Type", "model/gltf-binary");
            r.set_header("Content-Disposition", "attachment; filename=\"model.glb\"");
            return r;
        } catch (const std::exception &e) {
            crow::response r{std::string("{\"error\":\"") + e.what() + "\"}"};
            r.code = 400; set_json_headers(r); return r;
        }
    });

    // 下载 OG3d 模型
    CROW_ROUTE(app, "/api/project/<string>/download/OG3d").methods(crow::HTTPMethod::GET)([&store](const std::string &uuid){
        try {
            if (!store.exists(uuid)) throw std::runtime_error("project not found");
            fs::path glb_path = store.base_path / uuid / "OG3d" / "model.glb";
            if (!fs::exists(glb_path)) throw std::runtime_error("OG3d model not found");
            crow::response r{read_text_file(glb_path)};
            r.set_header("Content-Type", "model/gltf-binary");
            r.set_header("Content-Disposition", "attachment; filename=\"model.glb\"");
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

    CROW_ROUTE(app, "/api/project/<string>/start_analysis").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/to_3d_model").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
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

    CROW_ROUTE(app, "/api/project/<string>/processed/png").methods(crow::HTTPMethod::OPTIONS)([](const std::string &){
        crow::response r;
        r.set_header("Access-Control-Allow-Origin", "*");
        r.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        r.set_header("Access-Control-Allow-Headers", "Content-Type");
        r.code = 204;
        return r;
    });

    CROW_ROUTE(app, "/api/project/<string>/processed/png/<string>").methods(crow::HTTPMethod::OPTIONS)([](const std::string &, const std::string &){
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

    CROW_ROUTE(app, "/api/project/<string>/download/processed/<string>").methods(crow::HTTPMethod::OPTIONS)([](const std::string &, const std::string &){
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
