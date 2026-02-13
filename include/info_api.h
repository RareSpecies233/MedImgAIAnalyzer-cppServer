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
#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <onnxruntime/onnxruntime_cxx_api.h>
#include "cnpy.h"
#include "info_store.h"
#include "npz_to_glb.h"

#ifdef _WIN32
#ifdef DELETE
#undef DELETE
#endif
#endif

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

inline constexpr const char* kNpzEmbedMagic = "NPZ_ROUNDTRIP_V1\0";
inline constexpr size_t kNpzEmbedMagicSize = 17;

static inline std::string base64_encode(const std::vector<uint8_t> &input)
{
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= input.size()) {
        const uint32_t v = (static_cast<uint32_t>(input[i]) << 16) |
                           (static_cast<uint32_t>(input[i + 1]) << 8) |
                           static_cast<uint32_t>(input[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }

    if (i < input.size()) {
        uint32_t v = static_cast<uint32_t>(input[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        if (i + 1 < input.size()) {
            v |= static_cast<uint32_t>(input[i + 1]) << 8;
            out.push_back(kTable[(v >> 12) & 0x3F]);
            out.push_back(kTable[(v >> 6) & 0x3F]);
            out.push_back('=');
        } else {
            out.push_back(kTable[(v >> 12) & 0x3F]);
            out.push_back('=');
            out.push_back('=');
        }
    }
    return out;
}

static inline std::vector<uint8_t> base64_decode(const std::string &input)
{
    static std::array<int, 256> decode_table = [] {
        std::array<int, 256> table{};
        table.fill(-1);
        const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i = 0; i < chars.size(); ++i) {
            table[static_cast<uint8_t>(chars[i])] = static_cast<int>(i);
        }
        return table;
    }();

    std::vector<uint8_t> out;
    uint32_t accum = 0;
    int bits = 0;

    for (char c : input) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int value = decode_table[static_cast<uint8_t>(c)];
        if (value < 0) {
            throw std::runtime_error("base64 解码失败: 非法字符");
        }
        accum = (accum << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }

    return out;
}

static inline std::vector<uint8_t> pack_embedded_npz(const std::vector<uint8_t> &npz_bytes)
{
    const std::string b64 = base64_encode(npz_bytes);
    std::vector<uint8_t> out;
    out.reserve(kNpzEmbedMagicSize + 8 + b64.size());
    out.insert(out.end(), kNpzEmbedMagic, kNpzEmbedMagic + kNpzEmbedMagicSize);

    uint64_t len = static_cast<uint64_t>(b64.size());
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
    }
    out.insert(out.end(), b64.begin(), b64.end());
    return out;
}

static inline bool unpack_embedded_npz(const std::vector<uint8_t> &payload, std::vector<uint8_t> *npz_bytes)
{
    if (payload.size() < kNpzEmbedMagicSize + 8) {
        return false;
    }
    if (std::memcmp(payload.data(), kNpzEmbedMagic, kNpzEmbedMagicSize) != 0) {
        return false;
    }
    uint64_t len = 0;
    for (int i = 0; i < 8; ++i) {
        len |= (static_cast<uint64_t>(payload[kNpzEmbedMagicSize + i]) << (8 * i));
    }
    const size_t start = kNpzEmbedMagicSize + 8;
    if (payload.size() < start + static_cast<size_t>(len)) {
        return false;
    }
    const std::string b64(reinterpret_cast<const char *>(payload.data() + start), static_cast<size_t>(len));
    *npz_bytes = base64_decode(b64);
    return true;
}

static inline bool try_extract_embedded_npz_from_bytes(const std::vector<uint8_t> &all_bytes,
                                                       std::vector<uint8_t> *npz_bytes)
{
    for (size_t i = 0; i + kNpzEmbedMagicSize + 8 <= all_bytes.size(); ++i) {
        if (std::memcmp(all_bytes.data() + i, kNpzEmbedMagic, kNpzEmbedMagicSize) == 0) {
            std::vector<uint8_t> slice(all_bytes.begin() + static_cast<long>(i), all_bytes.end());
            if (unpack_embedded_npz(slice, npz_bytes)) {
                return true;
            }
        }
    }
    return false;
}

static inline uint16_t read_u16_le(const std::vector<uint8_t> &b, size_t off)
{
    return static_cast<uint16_t>(b[off]) | (static_cast<uint16_t>(b[off + 1]) << 8);
}

static inline uint32_t read_u32_le(const std::vector<uint8_t> &b, size_t off)
{
    return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

static inline std::vector<uint8_t> read_tag_value_explicit_vr(const std::vector<uint8_t> &dcm,
                                                               uint16_t target_group,
                                                               uint16_t target_elem,
                                                               bool *found = nullptr)
{
    if (found != nullptr) *found = false;
    size_t off = 132;
    while (off + 8 <= dcm.size()) {
        const uint16_t group = read_u16_le(dcm, off);
        const uint16_t elem = read_u16_le(dcm, off + 2);
        const char vr0 = static_cast<char>(dcm[off + 4]);
        const char vr1 = static_cast<char>(dcm[off + 5]);
        const std::string vr{vr0, vr1};

        bool long_vr = (vr == "OB" || vr == "OW" || vr == "OF" || vr == "SQ" || vr == "UT" || vr == "UN");
        uint32_t len = 0;
        size_t value_off = 0;
        if (long_vr) {
            if (off + 12 > dcm.size()) break;
            len = read_u32_le(dcm, off + 8);
            value_off = off + 12;
            off += 12;
        } else {
            len = read_u16_le(dcm, off + 6);
            value_off = off + 8;
            off += 8;
        }
        if (value_off + len > dcm.size()) break;

        if (group == target_group && elem == target_elem) {
            if (found != nullptr) *found = true;
            return std::vector<uint8_t>(dcm.begin() + static_cast<long>(value_off),
                                        dcm.begin() + static_cast<long>(value_off + len));
        }
        off = value_off + len;
    }
    return {};
}

static inline std::vector<size_t> require_shape_2d(const cnpy::NpyArray &arr)
{
    if (arr.shape.size() != 2) {
        throw std::runtime_error("仅支持二维数组，当前维度=" + std::to_string(arr.shape.size()));
    }
    return arr.shape;
}

static inline std::vector<double> npy_to_double_2d_strict(const cnpy::NpyArray &arr)
{
    int h = 0;
    int w = 0;
    return npy_to_double_2d(arr, h, w);
}

static inline std::vector<uint16_t> to_uint16_clipped(const std::vector<double> &input)
{
    std::vector<uint16_t> out(input.size(), 0);
    for (size_t i = 0; i < input.size(); ++i) {
        double v = input[i];
        if (!std::isfinite(v)) {
            v = 0.0;
        }
        v = std::clamp(v, 0.0, 65535.0);
        out[i] = static_cast<uint16_t>(std::llround(v));
    }
    return out;
}

static inline std::vector<float> to_float32(const std::vector<double> &input)
{
    std::vector<float> out(input.size(), 0.0F);
    for (size_t i = 0; i < input.size(); ++i) {
        out[i] = static_cast<float>(input[i]);
    }
    return out;
}

static inline std::vector<double> image_from_gray_u8(const cv::Mat &gray)
{
    if (gray.type() != CV_8UC1) {
        throw std::runtime_error("png 需要为单通道 8 位图像");
    }
    const int rows = gray.rows;
    const int cols = gray.cols;
    std::vector<double> out(static_cast<size_t>(rows) * static_cast<size_t>(cols), 0.0);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            out[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)] =
                static_cast<double>(gray.at<uint8_t>(r, c)) / 255.0;
        }
    }
    return out;
}

static inline void save_onnx_compatible_npz(const fs::path &out_path,
                                            const std::vector<size_t> &image_shape,
                                            const std::vector<double> &image_data)
{
    if (image_shape.size() != 2) {
        throw std::runtime_error("save_onnx_compatible_npz 仅支持 2D image");
    }
    const size_t n = image_shape[0] * image_shape[1];
    if (image_data.size() != n) {
        throw std::runtime_error("image 数据长度与 shape 不匹配");
    }

    std::vector<uint8_t> label(n, 0);
    cnpy::npz_save(out_path.string(), "image", image_data.data(), image_shape, "w");
    cnpy::npz_save(out_path.string(), "label", label.data(), image_shape, "a");
}

static inline std::string uid_like()
{
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint64_t> dist(1000000ULL, 999999999ULL);
    return "2.25." + std::to_string(dist(rng));
}

static inline void append_u16_le(std::vector<uint8_t> &out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

static inline void append_u32_le(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static inline std::vector<uint8_t> str_bytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

static inline std::vector<uint8_t> u16_bytes(uint16_t v)
{
    std::vector<uint8_t> out;
    append_u16_le(out, v);
    return out;
}

static inline void append_tag(std::vector<uint8_t> &out,
                              uint16_t group,
                              uint16_t element,
                              const std::string &vr,
                              const std::vector<uint8_t> &value)
{
    append_u16_le(out, group);
    append_u16_le(out, element);
    out.push_back(static_cast<uint8_t>(vr[0]));
    out.push_back(static_cast<uint8_t>(vr[1]));

    const bool long_vr = (vr == "OB" || vr == "OW" || vr == "OF" || vr == "SQ" || vr == "UT" || vr == "UN");
    std::vector<uint8_t> val = value;
    if (val.size() % 2 != 0) {
        const uint8_t pad = (vr == "UI" || vr == "LO" || vr == "PN" || vr == "CS" || vr == "DA" || vr == "TM")
                                ? static_cast<uint8_t>(' ')
                                : static_cast<uint8_t>(0);
        val.push_back(pad);
    }

    if (long_vr) {
        out.push_back(0);
        out.push_back(0);
        append_u32_le(out, static_cast<uint32_t>(val.size()));
    } else {
        append_u16_le(out, static_cast<uint16_t>(val.size()));
    }
    out.insert(out.end(), val.begin(), val.end());
}

#pragma pack(push, 1)
struct Nifti1Header {
    int32_t sizeof_hdr;
    char data_type[10];
    char db_name[18];
    int32_t extents;
    int16_t session_error;
    char regular;
    char dim_info;
    int16_t dim[8];
    float intent_p1;
    float intent_p2;
    float intent_p3;
    int16_t intent_code;
    int16_t datatype;
    int16_t bitpix;
    int16_t slice_start;
    float pixdim[8];
    float vox_offset;
    float scl_slope;
    float scl_inter;
    int16_t slice_end;
    char slice_code;
    char xyzt_units;
    float cal_max;
    float cal_min;
    float slice_duration;
    float toffset;
    int32_t glmax;
    int32_t glmin;
    char descrip[80];
    char aux_file[24];
    int16_t qform_code;
    int16_t sform_code;
    float quatern_b;
    float quatern_c;
    float quatern_d;
    float qoffset_x;
    float qoffset_y;
    float qoffset_z;
    float srow_x[4];
    float srow_y[4];
    float srow_z[4];
    char intent_name[16];
    char magic[4];
};
#pragma pack(pop)

static_assert(sizeof(Nifti1Header) == 348, "Nifti1Header 大小必须为 348 字节");

static inline void dcm_to_npz(const fs::path &input_path, const fs::path &out_path)
{
    const std::vector<uint8_t> dcm_bytes = [&] {
        std::string raw = read_text_file(input_path);
        return std::vector<uint8_t>(raw.begin(), raw.end());
    }();

    std::vector<uint8_t> embedded_npz;
    if (try_extract_embedded_npz_from_bytes(dcm_bytes, &embedded_npz)) {
        write_text_file(out_path, std::string(embedded_npz.begin(), embedded_npz.end()));
        return;
    }

    bool ok_rows = false;
    bool ok_cols = false;
    bool ok_pixel = false;
    const auto rows_buf = read_tag_value_explicit_vr(dcm_bytes, 0x0028, 0x0010, &ok_rows);
    const auto cols_buf = read_tag_value_explicit_vr(dcm_bytes, 0x0028, 0x0011, &ok_cols);
    const auto pixel_buf = read_tag_value_explicit_vr(dcm_bytes, 0x7FE0, 0x0010, &ok_pixel);

    if (!ok_rows || !ok_cols || !ok_pixel || rows_buf.size() < 2 || cols_buf.size() < 2) {
        throw std::runtime_error("无法从 DICOM 读取像素，也未找到嵌入的 NPZ 载荷");
    }

    const uint16_t rows = static_cast<uint16_t>(rows_buf[0]) | (static_cast<uint16_t>(rows_buf[1]) << 8);
    const uint16_t cols = static_cast<uint16_t>(cols_buf[0]) | (static_cast<uint16_t>(cols_buf[1]) << 8);
    const size_t n = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    if (pixel_buf.size() < n * sizeof(uint16_t)) {
        throw std::runtime_error("DICOM PixelData 长度不足");
    }

    std::vector<uint16_t> image(n, 0);
    std::memcpy(image.data(), pixel_buf.data(), n * sizeof(uint16_t));
    std::vector<uint8_t> label(n, 0);
    const std::vector<size_t> shape{rows, cols};
    cnpy::npz_save(out_path.string(), "image", image.data(), shape, "w");
    cnpy::npz_save(out_path.string(), "label", label.data(), shape, "a");
}

static inline void npz_to_dcm(const fs::path &input_path, const fs::path &out_path, const std::string &key)
{
    const auto npz_map = cnpy::npz_load(input_path.string());
    auto it = npz_map.find(key);
    if (it == npz_map.end()) {
        throw std::runtime_error("npz 中找不到键: " + key);
    }
    const auto shape = require_shape_2d(it->second);
    const auto image_f64 = npy_to_double_2d_strict(it->second);
    const auto image_u16 = to_uint16_clipped(image_f64);

    const uint16_t rows = static_cast<uint16_t>(shape[0]);
    const uint16_t cols = static_cast<uint16_t>(shape[1]);

    const std::string npz_raw = read_text_file(input_path);
    const std::vector<uint8_t> npz_bytes(npz_raw.begin(), npz_raw.end());
    const auto packed_npz = pack_embedded_npz(npz_bytes);

    std::vector<uint8_t> out(128, 0);
    out.push_back('D');
    out.push_back('I');
    out.push_back('C');
    out.push_back('M');

    std::vector<uint8_t> file_meta;
    const std::string sop_class = "1.2.840.10008.5.1.4.1.1.7";
    const std::string sop_instance = uid_like();
    const std::string transfer_syntax = "1.2.840.10008.1.2.1";
    const std::string impl_uid = uid_like();

    append_tag(file_meta, 0x0002, 0x0001, "OB", {0x00, 0x01});
    append_tag(file_meta, 0x0002, 0x0002, "UI", str_bytes(sop_class));
    append_tag(file_meta, 0x0002, 0x0003, "UI", str_bytes(sop_instance));
    append_tag(file_meta, 0x0002, 0x0010, "UI", str_bytes(transfer_syntax));
    append_tag(file_meta, 0x0002, 0x0012, "UI", str_bytes(impl_uid));

    append_tag(out,
               0x0002,
               0x0000,
               "UL",
               std::vector<uint8_t>{
                   static_cast<uint8_t>(file_meta.size() & 0xFF),
                   static_cast<uint8_t>((file_meta.size() >> 8) & 0xFF),
                   static_cast<uint8_t>((file_meta.size() >> 16) & 0xFF),
                   static_cast<uint8_t>((file_meta.size() >> 24) & 0xFF),
               });
    out.insert(out.end(), file_meta.begin(), file_meta.end());

    append_tag(out, 0x0008, 0x0060, "CS", str_bytes("OT"));
    append_tag(out, 0x0010, 0x0010, "PN", str_bytes("Converted^FromNPZ"));
    append_tag(out, 0x0010, 0x0020, "LO", str_bytes("NPZ0001"));
    append_tag(out, 0x0028, 0x0010, "US", u16_bytes(rows));
    append_tag(out, 0x0028, 0x0011, "US", u16_bytes(cols));
    append_tag(out, 0x0028, 0x0002, "US", u16_bytes(1));
    append_tag(out, 0x0028, 0x0004, "CS", str_bytes("MONOCHROME2"));
    append_tag(out, 0x0028, 0x0100, "US", u16_bytes(16));
    append_tag(out, 0x0028, 0x0101, "US", u16_bytes(16));
    append_tag(out, 0x0028, 0x0102, "US", u16_bytes(15));
    append_tag(out, 0x0028, 0x0103, "US", u16_bytes(0));
    append_tag(out, 0x0011, 0x0010, "LO", str_bytes("NPZ_ROUNDTRIP"));
    append_tag(out, 0x0011, 0x1010, "OB", packed_npz);

    std::vector<uint8_t> pixel_bytes(image_u16.size() * sizeof(uint16_t));
    std::memcpy(pixel_bytes.data(), image_u16.data(), pixel_bytes.size());
    append_tag(out, 0x7FE0, 0x0010, "OW", pixel_bytes);

    write_text_file(out_path, std::string(out.begin(), out.end()));
}

static inline void npz_to_nii(const fs::path &input_path, const fs::path &out_path, const std::string &key)
{
    const auto npz_map = cnpy::npz_load(input_path.string());
    auto it = npz_map.find(key);
    if (it == npz_map.end()) {
        throw std::runtime_error("npz 中找不到键: " + key);
    }

    std::vector<size_t> shape = it->second.shape;
    std::vector<float> image_f32;
    if (shape.size() == 2) {
        const auto image_f64 = npy_to_double_2d_strict(it->second);
        image_f32 = to_float32(image_f64);
        shape = {shape[0], shape[1], 1};
    } else if (shape.size() == 3) {
        const size_t n = shape[0] * shape[1] * shape[2];
        image_f32.resize(n, 0.0F);
        if (it->second.word_size == sizeof(float)) {
            const auto *p = it->second.data<float>();
            std::copy(p, p + n, image_f32.begin());
        } else if (it->second.word_size == sizeof(double)) {
            const auto *p = it->second.data<double>();
            for (size_t i = 0; i < n; ++i) image_f32[i] = static_cast<float>(p[i]);
        } else {
            throw std::runtime_error("3D NIfTI 仅支持 float32/float64 输入");
        }
    } else {
        throw std::runtime_error("仅支持 2D/3D 写入 NIfTI");
    }

    const std::string npz_raw = read_text_file(input_path);
    const std::vector<uint8_t> npz_bytes(npz_raw.begin(), npz_raw.end());
    const auto packed_npz = pack_embedded_npz(npz_bytes);

    Nifti1Header hdr{};
    hdr.sizeof_hdr = 348;
    hdr.dim[0] = 3;
    hdr.dim[1] = static_cast<int16_t>(shape[0]);
    hdr.dim[2] = static_cast<int16_t>(shape[1]);
    hdr.dim[3] = static_cast<int16_t>(shape[2]);
    hdr.datatype = 16;
    hdr.bitpix = 32;
    hdr.pixdim[1] = 1.0F;
    hdr.pixdim[2] = 1.0F;
    hdr.pixdim[3] = 1.0F;

    int32_t ext_size = static_cast<int32_t>(8 + packed_npz.size());
    const int32_t rem = ext_size % 16;
    if (rem != 0) ext_size += (16 - rem);
    hdr.vox_offset = static_cast<float>(352 + ext_size);
    std::strncpy(hdr.descrip, "ConvertedFromNPZ", sizeof(hdr.descrip) - 1);
    hdr.sform_code = 1;
    hdr.srow_x[0] = 1.0F;
    hdr.srow_y[1] = 1.0F;
    hdr.srow_z[2] = 1.0F;
    hdr.magic[0] = 'n';
    hdr.magic[1] = '+';
    hdr.magic[2] = '1';
    hdr.magic[3] = '\0';

    std::vector<uint8_t> out;
    out.resize(348);
    std::memcpy(out.data(), &hdr, 348);

    out.push_back(1);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);

    append_u32_le(out, static_cast<uint32_t>(ext_size));
    append_u32_le(out, 40);
    out.insert(out.end(), packed_npz.begin(), packed_npz.end());
    while ((out.size() - 352) % 16 != 0) {
        out.push_back(0);
    }

    const auto data_offset = static_cast<size_t>(hdr.vox_offset);
    if (out.size() < data_offset) {
        out.resize(data_offset, 0);
    }
    const size_t data_bytes = image_f32.size() * sizeof(float);
    const size_t base = out.size();
    out.resize(base + data_bytes);
    std::memcpy(out.data() + static_cast<long>(base), image_f32.data(), data_bytes);

    write_text_file(out_path, std::string(out.begin(), out.end()));
}

static inline void nii_to_npz(const fs::path &input_path, const fs::path &out_path, int slice_index = -1)
{
    const std::string raw = read_text_file(input_path);
    const std::vector<uint8_t> all(raw.begin(), raw.end());
    if (all.size() < 352) {
        throw std::runtime_error("NIfTI 文件过小");
    }

    std::vector<uint8_t> embedded_npz;
    if (try_extract_embedded_npz_from_bytes(all, &embedded_npz)) {
        write_text_file(out_path, std::string(embedded_npz.begin(), embedded_npz.end()));
        return;
    }

    Nifti1Header hdr{};
    std::memcpy(&hdr, all.data(), 348);
    if (hdr.sizeof_hdr != 348) {
        throw std::runtime_error("不支持的 NIfTI 头部");
    }

    const int ndim = hdr.dim[0];
    const int d1 = std::max<int>(1, hdr.dim[1]);
    const int d2 = std::max<int>(1, hdr.dim[2]);
    const int d3 = std::max<int>(1, hdr.dim[3]);

    if (ndim < 2) {
        throw std::runtime_error("NIfTI 维度不足");
    }

    const size_t vox_offset = static_cast<size_t>(hdr.vox_offset);
    if (vox_offset >= all.size()) {
        throw std::runtime_error("NIfTI vox_offset 越界");
    }

    const size_t n = static_cast<size_t>(d1) * static_cast<size_t>(d2) * static_cast<size_t>(d3);
    std::vector<double> volume(n, 0.0);

    if (hdr.datatype == 16 && hdr.bitpix == 32) {
        const size_t need = n * sizeof(float);
        if (vox_offset + need > all.size()) throw std::runtime_error("NIfTI 数据长度不足");
        const auto *p = reinterpret_cast<const float *>(all.data() + static_cast<long>(vox_offset));
        for (size_t i = 0; i < n; ++i) volume[i] = static_cast<double>(p[i]);
    } else if (hdr.datatype == 64 && hdr.bitpix == 64) {
        const size_t need = n * sizeof(double);
        if (vox_offset + need > all.size()) throw std::runtime_error("NIfTI 数据长度不足");
        const auto *p = reinterpret_cast<const double *>(all.data() + static_cast<long>(vox_offset));
        std::copy(p, p + n, volume.begin());
    } else if (hdr.datatype == 512 && hdr.bitpix == 16) {
        const size_t need = n * sizeof(uint16_t);
        if (vox_offset + need > all.size()) throw std::runtime_error("NIfTI 数据长度不足");
        const auto *p = reinterpret_cast<const uint16_t *>(all.data() + static_cast<long>(vox_offset));
        for (size_t i = 0; i < n; ++i) volume[i] = static_cast<double>(p[i]);
    } else {
        throw std::runtime_error("当前仅支持读取 float32/float64/uint16 的 NIfTI");
    }

    const int use_slice = (d3 == 1) ? 0 : (slice_index >= 0 ? slice_index : (d3 / 2));
    if (use_slice < 0 || use_slice >= d3) {
        throw std::runtime_error("slice_index 越界");
    }

    const size_t hw = static_cast<size_t>(d1) * static_cast<size_t>(d2);
    std::vector<double> image(hw, 0.0);
    const size_t z_off = static_cast<size_t>(use_slice) * hw;
    std::copy(volume.begin() + static_cast<long>(z_off),
              volume.begin() + static_cast<long>(z_off + hw),
              image.begin());

    save_onnx_compatible_npz(out_path, {static_cast<size_t>(d1), static_cast<size_t>(d2)}, image);
}

static inline void png_to_npz(const fs::path &input_path, const fs::path &out_path)
{
    const cv::Mat gray = cv::imread(input_path.string(), cv::IMREAD_GRAYSCALE);
    if (gray.empty()) {
        throw std::runtime_error("读取 png 失败: " + input_path.string());
    }
    const auto image = image_from_gray_u8(gray);
    save_onnx_compatible_npz(out_path,
                             {static_cast<size_t>(gray.rows), static_cast<size_t>(gray.cols)},
                             image);
}

static inline void npz_to_png(const fs::path &input_path, const fs::path &out_path, const std::string &key = "image")
{
    const auto npz_map = cnpy::npz_load(input_path.string());
    auto it = npz_map.find(key);
    if (it == npz_map.end()) {
        throw std::runtime_error("npz 中找不到键: " + key);
    }
    int h = 0;
    int w = 0;
    std::vector<double> image_data = npy_to_double_2d(it->second, h, w);
    cv::Mat image = normalize_to_u8(image_data, h, w);
    if (!cv::imwrite(out_path.string(), image)) {
        throw std::runtime_error("写入 png 失败: " + out_path.string());
    }
}

static inline void all2npz(const fs::path &src, const fs::path &dst)
{
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    const std::string ext = to_lower_copy(src.extension().string());
    if (ext == ".npz") {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("npz 复制失败: " + ec.message());
        return;
    }
    if (ext == ".dcm") {
        dcm_to_npz(src, dst);
        return;
    }
    if (ext == ".nii" || ext == ".gz" || ext == ".nii.gz") {
        nii_to_npz(src, dst, -1);
        return;
    }
    if (ext == ".png") {
        png_to_npz(src, dst);
        return;
    }
    throw std::runtime_error("不支持转换为npz的文件类型: " + src.extension().string());
}

static inline void all2png(const fs::path &src, const fs::path &dst)
{
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    const std::string ext = to_lower_copy(src.extension().string());
    if (ext == ".png") {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) throw std::runtime_error("png 复制失败: " + ec.message());
        return;
    }
    if (ext == ".npz") {
        npz_to_png(src, dst, "image");
        return;
    }
    if (ext == ".dcm" || ext == ".nii" || ext == ".gz" || ext == ".nii.gz") {
        fs::path tmp_npz = dst.parent_path() / (src.stem().string() + ".__tmp_convert__.npz");
        all2npz(src, tmp_npz);
        npz_to_png(tmp_npz, dst, "image");
        fs::remove(tmp_npz, ec);
        return;
    }
    throw std::runtime_error("不支持转换为png的文件类型: " + src.extension().string());
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
#ifdef _WIN32
    auto onnx_path_w = onnx_path.wstring();
    Ort::Session session(env, onnx_path_w.c_str(), opts);
#else
    Ort::Session session(env, onnx_path.string().c_str(), opts);
#endif

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

static inline std::string powershell_single_quote_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    return out;
}

static inline fs::path create_zip_store(const fs::path &dir, const std::string &zip_name)
{
    if (!fs::exists(dir)) throw std::runtime_error("目录不存在");
    fs::path tmp = fs::temp_directory_path() / zip_name;
    if (fs::exists(tmp)) fs::remove(tmp);

    int rc = 0;
#ifdef _WIN32
    std::string src_glob = powershell_single_quote_escape((dir / "*").string());
    std::string dst_zip = powershell_single_quote_escape(tmp.string());
    std::string cmd =
        "powershell -NoProfile -NonInteractive -Command \"Compress-Archive -Path '" +
        src_glob +
        "' -DestinationPath '" +
        dst_zip +
        "' -CompressionLevel NoCompression -Force\"";
    rc = std::system(cmd.c_str());
#else
    std::string cmd = "zip -0 -r " + shell_escape(tmp.string()) + " .";
    auto cwd = fs::current_path();
    fs::current_path(dir);
    try {
        rc = std::system(cmd.c_str());
        fs::current_path(cwd);
    } catch (...) {
        fs::current_path(cwd);
        throw;
    }
#endif

    if (rc != 0 || !fs::exists(tmp)) throw std::runtime_error("zip 失败");
    return tmp;
}

static inline void ensure_converted_from_npz_dir(const fs::path &npz_dir,
                                                 const fs::path &out_dir,
                                                 const std::string &target,
                                                 const std::string &npz_key)
{
    auto existing = list_files(out_dir);
    if (!existing.empty()) return;

    if (!fs::exists(npz_dir)) {
        throw std::runtime_error("源npz目录不存在，无法转换");
    }

    fs::create_directories(out_dir);
    auto npz_files = list_files(npz_dir);
    size_t converted = 0;
    for (const auto &src : npz_files) {
        if (to_lower_copy(src.extension().string()) != ".npz") continue;
        if (target == "dcm") {
            fs::path dst = out_dir / (src.stem().string() + ".dcm");
            npz_to_dcm(src, dst, npz_key);
            ++converted;
        } else if (target == "nii") {
            fs::path dst = out_dir / (src.stem().string() + ".nii");
            npz_to_nii(src, dst, npz_key);
            ++converted;
        } else {
            throw std::runtime_error("未知转换目标: " + target);
        }
    }

    if (converted == 0) {
        throw std::runtime_error("源npz目录为空，无法转换");
    }
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
                    all2npz(src, dst);
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
                        all2png(src, dst);
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

                fs::path out_dcm = processed_dcm_dir / (src.stem().string() + "-PD.dcm");
                fs::path out_nii = processed_nii_dir / (src.stem().string() + "-PD.nii");
                npz_to_dcm(out_npz, out_dcm, "label");
                npz_to_nii(out_npz, out_nii, "label");
            }

            update_project_json_fields(project_json, {
                {"processed", "\"" + mode_val + "\""},
                {"PD", "\"" + mode_val + "\""},
                {"PD-nii", "true"},
                {"PD-dcm", "true"},
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
            fs::path project_dir = store.base_path / uuid;
            fs::path dir = project_dir / "dcm";
            if (list_files(dir).empty()) {
                ensure_converted_from_npz_dir(project_dir / "npz", dir, "dcm", "image");
                update_project_json_fields(project_dir / "project.json", {{"dcm", "true"}});
            }
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
            fs::path dir = project_dir / "nii";
            if (list_files(dir).empty()) {
                ensure_converted_from_npz_dir(project_dir / "npz", dir, "nii", "image");
                update_project_json_fields(project_dir / "project.json", {{"nii", "true"}});
            }
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
            fs::path project_dir = store.base_path / uuid;
            fs::path dir = project_dir / "processed" / "dcm";
            if (list_files(dir).empty()) {
                ensure_converted_from_npz_dir(project_dir / "processed" / "npzs", dir, "dcm", "label");
                update_project_json_fields(project_dir / "project.json", {{"PD-dcm", "true"}});
            }
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
            fs::path project_dir = store.base_path / uuid;
            fs::path dir = project_dir / "processed" / "nii";
            if (list_files(dir).empty()) {
                ensure_converted_from_npz_dir(project_dir / "processed" / "npzs", dir, "nii", "label");
                update_project_json_fields(project_dir / "project.json", {{"PD-nii", "true"}});
            }
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
