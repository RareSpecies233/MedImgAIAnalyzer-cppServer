#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <zlib.h>

namespace npzproc {

namespace fs = std::filesystem;

struct CropRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct Args {
    fs::path input;
    fs::path output;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double rotate_deg = 0.0;
    std::optional<CropRect> crop;
    double contrast = 1.0;
    double gamma = 1.0;
    bool preserve_resolution = false;
};

struct NpyMeta {
    std::string descr;
    bool fortran_order = false;
    std::vector<size_t> shape;
    size_t data_offset = 0;
};

struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
};

struct DTypeInfo {
    char kind = '?';
    size_t item_size = 0;
    bool little_endian = true;
};

inline size_t element_count(const std::vector<size_t>& shape) {
    return std::accumulate(shape.begin(), shape.end(), static_cast<size_t>(1), std::multiplies<size_t>());
}

inline uint16_t read_u16_le(const uint8_t* ptr) {
    return static_cast<uint16_t>(ptr[0]) | (static_cast<uint16_t>(ptr[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t* ptr) {
    return static_cast<uint32_t>(ptr[0]) | (static_cast<uint32_t>(ptr[1]) << 8) |
           (static_cast<uint32_t>(ptr[2]) << 16) | (static_cast<uint32_t>(ptr[3]) << 24);
}

inline void append_u16_le(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

inline void append_u32_le(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

inline std::vector<uint8_t> read_file_bytes(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        throw std::runtime_error("无法打开文件: " + path.string());
    }
    ifs.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(ifs.tellg());
    ifs.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(size);
    if (size > 0) {
        ifs.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    }
    return bytes;
}

inline void write_file_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("无法写入文件: " + path.string());
    }
    if (!bytes.empty()) {
        ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

inline std::vector<uint8_t> inflate_raw_deflate(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    std::vector<uint8_t> out(uncompressed_size);
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(compressed_size);
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(uncompressed_size);

    const int init_rc = inflateInit2(&stream, -MAX_WBITS);
    if (init_rc != Z_OK) {
        throw std::runtime_error("inflateInit2 失败");
    }
    const int rc = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (rc != Z_STREAM_END) {
        throw std::runtime_error("解压 NPZ 条目失败");
    }
    return out;
}

inline std::vector<ZipEntry> load_npz_entries(const fs::path& path) {
    const auto bytes = read_file_bytes(path);
    std::vector<ZipEntry> entries;
    size_t offset = 0;

    while (offset + 4 <= bytes.size()) {
        const uint32_t sig = read_u32_le(bytes.data() + offset);
        if (sig == 0x02014B50 || sig == 0x06054B50) {
            break;
        }
        if (sig != 0x04034B50) {
            throw std::runtime_error("无效的 NPZ/ZIP 本地文件头");
        }
        if (offset + 30 > bytes.size()) {
            throw std::runtime_error("NPZ 本地文件头不完整");
        }

        const uint16_t flags = read_u16_le(bytes.data() + offset + 6);
        const uint16_t method = read_u16_le(bytes.data() + offset + 8);
        const uint32_t compressed_size = read_u32_le(bytes.data() + offset + 18);
        const uint32_t uncompressed_size = read_u32_le(bytes.data() + offset + 22);
        const uint16_t name_len = read_u16_le(bytes.data() + offset + 26);
        const uint16_t extra_len = read_u16_le(bytes.data() + offset + 28);

        if ((flags & 0x08U) != 0U) {
            throw std::runtime_error("暂不支持带数据描述符的 NPZ 条目");
        }

        const size_t header_size = 30 + static_cast<size_t>(name_len) + static_cast<size_t>(extra_len);
        if (offset + header_size + compressed_size > bytes.size()) {
            throw std::runtime_error("NPZ 条目长度越界");
        }

        const std::string name(reinterpret_cast<const char*>(bytes.data() + offset + 30), name_len);
        const uint8_t* payload = bytes.data() + offset + header_size;

        std::vector<uint8_t> data;
        if (method == 0) {
            data.assign(payload, payload + compressed_size);
        } else if (method == 8) {
            data = inflate_raw_deflate(payload, compressed_size, uncompressed_size);
        } else {
            throw std::runtime_error("不支持的 NPZ 压缩方式");
        }

        entries.push_back(ZipEntry{name, std::move(data)});
        offset += header_size + compressed_size;
    }

    if (entries.empty()) {
        throw std::runtime_error("未在 NPZ 中解析到任何条目");
    }
    return entries;
}

inline std::vector<uint8_t> save_npz_entries(const std::vector<ZipEntry>& entries) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> central_directory;
    out.reserve(1024);

    for (const auto& entry : entries) {
        const uint32_t crc = crc32(0L, reinterpret_cast<const Bytef*>(entry.data.data()), static_cast<uInt>(entry.data.size()));
        const uint32_t local_header_offset = static_cast<uint32_t>(out.size());
        const uint16_t name_len = static_cast<uint16_t>(entry.name.size());
        const uint32_t data_size = static_cast<uint32_t>(entry.data.size());

        append_u32_le(out, 0x04034B50);
        append_u16_le(out, 20);
        append_u16_le(out, 0);
        append_u16_le(out, 0);
        append_u16_le(out, 0);
        append_u16_le(out, 0);
        append_u32_le(out, crc);
        append_u32_le(out, data_size);
        append_u32_le(out, data_size);
        append_u16_le(out, name_len);
        append_u16_le(out, 0);
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.insert(out.end(), entry.data.begin(), entry.data.end());

        append_u32_le(central_directory, 0x02014B50);
        append_u16_le(central_directory, 20);
        append_u16_le(central_directory, 20);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u32_le(central_directory, crc);
        append_u32_le(central_directory, data_size);
        append_u32_le(central_directory, data_size);
        append_u16_le(central_directory, name_len);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u16_le(central_directory, 0);
        append_u32_le(central_directory, 0);
        append_u32_le(central_directory, local_header_offset);
        central_directory.insert(central_directory.end(), entry.name.begin(), entry.name.end());
    }

    const uint32_t central_directory_offset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), central_directory.begin(), central_directory.end());
    append_u32_le(out, 0x06054B50);
    append_u16_le(out, 0);
    append_u16_le(out, 0);
    append_u16_le(out, static_cast<uint16_t>(entries.size()));
    append_u16_le(out, static_cast<uint16_t>(entries.size()));
    append_u32_le(out, static_cast<uint32_t>(central_directory.size()));
    append_u32_le(out, central_directory_offset);
    append_u16_le(out, 0);
    return out;
}

inline DTypeInfo parse_dtype(const std::string& descr) {
    if (descr.size() < 3) {
        throw std::runtime_error("不支持的 dtype 描述");
    }
    DTypeInfo info;
    info.little_endian = descr[0] != '>';
    info.kind = descr[1];
    info.item_size = static_cast<size_t>(std::stoul(descr.substr(2)));
    return info;
}

inline NpyMeta parse_npy_meta(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 10) {
        throw std::runtime_error("NPY 数据过短");
    }
    static const uint8_t magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (!std::equal(std::begin(magic), std::end(magic), bytes.begin())) {
        throw std::runtime_error("无效的 NPY 魔数");
    }

    const uint8_t major = bytes[6];
    const size_t len_field_size = major == 1 ? 2 : 4;
    const size_t prefix = 8 + len_field_size;
    if (bytes.size() < prefix) {
        throw std::runtime_error("NPY 头部不完整");
    }

    size_t header_len = 0;
    if (major == 1) {
        header_len = read_u16_le(bytes.data() + 8);
    } else {
        header_len = read_u32_le(bytes.data() + 8);
    }

    if (bytes.size() < prefix + header_len) {
        throw std::runtime_error("NPY 头部长度越界");
    }

    const std::string header(reinterpret_cast<const char*>(bytes.data() + prefix), header_len);
    NpyMeta meta;
    meta.data_offset = prefix + header_len;

    const auto descr_pos = header.find("'descr'");
    const auto fortran_pos = header.find("'fortran_order'");
    const auto shape_pos = header.find("'shape'");
    if (descr_pos == std::string::npos || fortran_pos == std::string::npos || shape_pos == std::string::npos) {
        throw std::runtime_error("NPY 头部缺少必要字段");
    }

    const auto descr_colon = header.find(':', descr_pos);
    const auto descr_quote_1 = header.find('\'', descr_colon);
    const auto descr_quote_2 = header.find('\'', descr_quote_1 + 1);
    if (descr_colon == std::string::npos || descr_quote_1 == std::string::npos || descr_quote_2 == std::string::npos) {
        throw std::runtime_error("NPY descr 解析失败");
    }
    meta.descr = header.substr(descr_quote_1 + 1, descr_quote_2 - descr_quote_1 - 1);

    const auto bool_start = header.find_first_not_of(" :", fortran_pos + 15);
    if (bool_start == std::string::npos) {
        throw std::runtime_error("NPY fortran_order 解析失败");
    }
    meta.fortran_order = header.compare(bool_start, 4, "True") == 0;

    const auto l_paren = header.find('(', shape_pos);
    const auto r_paren = header.find(')', l_paren);
    if (l_paren == std::string::npos || r_paren == std::string::npos) {
        throw std::runtime_error("NPY shape 解析失败");
    }
    std::string shape_text = header.substr(l_paren + 1, r_paren - l_paren - 1);
    std::stringstream ss(shape_text);
    while (ss.good()) {
        size_t value = 0;
        ss >> value;
        if (!ss.fail()) {
            meta.shape.push_back(value);
        }
        if (ss.peek() == ',') {
            ss.ignore();
        } else if (std::isspace(ss.peek())) {
            ss.ignore();
        } else if (ss.fail()) {
            ss.clear();
            ss.ignore();
        }
    }
    if (meta.shape.empty()) {
        throw std::runtime_error("NPY shape 为空");
    }
    return meta;
}

inline std::vector<uint8_t> make_npy_bytes(const NpyMeta& meta, const std::vector<uint8_t>& raw_data) {
    std::ostringstream shape_stream;
    shape_stream << '(';
    for (size_t i = 0; i < meta.shape.size(); ++i) {
        shape_stream << meta.shape[i];
        if (meta.shape.size() == 1) {
            shape_stream << ',';
            break;
        }
        if (i + 1 < meta.shape.size()) {
            shape_stream << ", ";
        }
    }
    shape_stream << ')';

    std::string header = "{'descr': '" + meta.descr + "', 'fortran_order': " +
                         std::string(meta.fortran_order ? "True" : "False") +
                         ", 'shape': " + shape_stream.str() + ", }";
    const size_t preamble = 10;
    size_t header_len = header.size() + 1;
    const size_t padding = (16 - ((preamble + header_len) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');

    std::vector<uint8_t> out;
    out.reserve(preamble + header.size() + raw_data.size());
    out.push_back(0x93);
    out.insert(out.end(), {'N', 'U', 'M', 'P', 'Y'});
    out.push_back(1);
    out.push_back(0);
    append_u16_le(out, static_cast<uint16_t>(header.size()));
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), raw_data.begin(), raw_data.end());
    return out;
}

inline double dtype_min_value(const DTypeInfo& info) {
    switch (info.kind) {
        case 'i':
            switch (info.item_size) {
                case 1: return static_cast<double>(std::numeric_limits<int8_t>::min());
                case 2: return static_cast<double>(std::numeric_limits<int16_t>::min());
                case 4: return static_cast<double>(std::numeric_limits<int32_t>::min());
                case 8: return static_cast<double>(std::numeric_limits<int64_t>::min());
                default: break;
            }
            break;
        case 'u':
        case 'b':
            return 0.0;
        default:
            break;
    }
    return -std::numeric_limits<double>::infinity();
}

inline double dtype_max_value(const DTypeInfo& info) {
    switch (info.kind) {
        case 'i':
            switch (info.item_size) {
                case 1: return static_cast<double>(std::numeric_limits<int8_t>::max());
                case 2: return static_cast<double>(std::numeric_limits<int16_t>::max());
                case 4: return static_cast<double>(std::numeric_limits<int32_t>::max());
                case 8: return static_cast<double>(std::numeric_limits<int64_t>::max());
                default: break;
            }
            break;
        case 'u':
            switch (info.item_size) {
                case 1: return static_cast<double>(std::numeric_limits<uint8_t>::max());
                case 2: return static_cast<double>(std::numeric_limits<uint16_t>::max());
                case 4: return static_cast<double>(std::numeric_limits<uint32_t>::max());
                case 8: return static_cast<double>(std::numeric_limits<uint64_t>::max());
                default: break;
            }
            break;
        case 'b':
            return 1.0;
        default:
            break;
    }
    return std::numeric_limits<double>::infinity();
}

inline bool dtype_is_integer_like(const DTypeInfo& info) {
    return info.kind == 'i' || info.kind == 'u' || info.kind == 'b';
}

inline std::vector<double> reorder_fortran_to_c(const std::vector<double>& input, const std::vector<size_t>& shape) {
    if (shape.empty()) {
        return input;
    }
    const size_t count = element_count(shape);
    std::vector<double> output(count, 0.0);
    std::vector<size_t> coords(shape.size(), 0);

    for (size_t c_index = 0; c_index < count; ++c_index) {
        size_t tmp = c_index;
        for (size_t dim = shape.size(); dim-- > 0;) {
            coords[dim] = tmp % shape[dim];
            tmp /= shape[dim];
        }

        size_t f_index = 0;
        size_t stride = 1;
        for (size_t dim = 0; dim < shape.size(); ++dim) {
            f_index += coords[dim] * stride;
            stride *= shape[dim];
        }
        output[c_index] = input[f_index];
    }
    return output;
}

inline std::vector<double> decode_numeric_data(const std::vector<uint8_t>& bytes, const NpyMeta& meta) {
    const auto dtype = parse_dtype(meta.descr);
    if (!dtype.little_endian) {
        throw std::runtime_error("暂不支持大端序 NPY 数据");
    }
    const size_t count = element_count(meta.shape);
    const size_t expected = count * dtype.item_size;
    if (meta.data_offset + expected > bytes.size()) {
        throw std::runtime_error("NPY 数据区长度不足");
    }
    const uint8_t* data = bytes.data() + meta.data_offset;
    std::vector<double> out(count, 0.0);

    auto copy_typed = [&](auto tag) {
        using T = decltype(tag);
        const T* ptr = reinterpret_cast<const T*>(data);
        for (size_t i = 0; i < count; ++i) {
            out[i] = static_cast<double>(ptr[i]);
        }
    };

    if (dtype.kind == 'f') {
        switch (dtype.item_size) {
            case 4: copy_typed(float{}); break;
            case 8: copy_typed(double{}); break;
            default: throw std::runtime_error("不支持的浮点 dtype");
        }
    } else if (dtype.kind == 'i') {
        switch (dtype.item_size) {
            case 1: copy_typed(int8_t{}); break;
            case 2: copy_typed(int16_t{}); break;
            case 4: copy_typed(int32_t{}); break;
            case 8: copy_typed(int64_t{}); break;
            default: throw std::runtime_error("不支持的有符号整数 dtype");
        }
    } else if (dtype.kind == 'u') {
        switch (dtype.item_size) {
            case 1: copy_typed(uint8_t{}); break;
            case 2: copy_typed(uint16_t{}); break;
            case 4: copy_typed(uint32_t{}); break;
            case 8: copy_typed(uint64_t{}); break;
            default: throw std::runtime_error("不支持的无符号整数 dtype");
        }
    } else if (dtype.kind == 'b') {
        copy_typed(uint8_t{});
    } else {
        throw std::runtime_error("不支持的 NPY dtype 类型");
    }

    if (meta.fortran_order) {
        out = reorder_fortran_to_c(out, meta.shape);
    }
    return out;
}

inline std::vector<uint8_t> encode_numeric_data(const std::vector<double>& values, const NpyMeta& meta) {
    const auto dtype = parse_dtype(meta.descr);
    const size_t count = element_count(meta.shape);
    if (values.size() != count) {
        throw std::runtime_error("编码数据长度与 shape 不匹配");
    }

    std::vector<uint8_t> raw(count * dtype.item_size, 0);

    auto cast_and_store = [&](auto tag) {
        using T = decltype(tag);
        T* ptr = reinterpret_cast<T*>(raw.data());
        for (size_t i = 0; i < count; ++i) {
            ptr[i] = static_cast<T>(values[i]);
        }
    };

    if (dtype.kind == 'f') {
        switch (dtype.item_size) {
            case 4: cast_and_store(float{}); break;
            case 8: cast_and_store(double{}); break;
            default: throw std::runtime_error("不支持的浮点 dtype");
        }
    } else if (dtype.kind == 'i') {
        switch (dtype.item_size) {
            case 1: cast_and_store(int8_t{}); break;
            case 2: cast_and_store(int16_t{}); break;
            case 4: cast_and_store(int32_t{}); break;
            case 8: cast_and_store(int64_t{}); break;
            default: throw std::runtime_error("不支持的有符号整数 dtype");
        }
    } else if (dtype.kind == 'u') {
        switch (dtype.item_size) {
            case 1: cast_and_store(uint8_t{}); break;
            case 2: cast_and_store(uint16_t{}); break;
            case 4: cast_and_store(uint32_t{}); break;
            case 8: cast_and_store(uint64_t{}); break;
            default: throw std::runtime_error("不支持的无符号整数 dtype");
        }
    } else if (dtype.kind == 'b') {
        cast_and_store(uint8_t{});
    } else {
        throw std::runtime_error("不支持的 NPY dtype 类型");
    }
    return raw;
}

inline std::tuple<int, int, int, int> clip_crop_rect(int x, int y, int w, int h, int width, int height) {
    x = std::max(0, x);
    y = std::max(0, y);
    w = std::max(1, w);
    h = std::max(1, h);
    int x2 = std::min(width, x + w);
    int y2 = std::min(height, y + h);
    if (x2 <= x) {
        x = 0;
        x2 = width;
    }
    if (y2 <= y) {
        y = 0;
        y2 = height;
    }
    return {x, y, x2 - x, y2 - y};
}

inline void apply_gamma_inplace(std::vector<double>& values, const DTypeInfo& dtype, double gamma) {
    if (gamma <= 0.0) {
        throw std::runtime_error("gamma 必须大于 0");
    }

    if (values.empty()) {
        return;
    }

    if (dtype_is_integer_like(dtype)) {
        const double min_value = dtype_min_value(dtype);
        const double max_value = dtype_max_value(dtype);
        const double range = max_value - min_value;
        if (!(range > 0.0)) {
            return;
        }
        for (double& value : values) {
            double normalized = (value - min_value) / range;
            normalized = std::clamp(normalized, 0.0, 1.0);
            value = std::pow(normalized, gamma) * range + min_value;
        }
        return;
    }

    const auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
    const double min_value = *min_it;
    const double max_value = *max_it;
    if (!(max_value > min_value)) {
        return;
    }

    if (min_value >= 0.0 && max_value <= 1.0) {
        for (double& value : values) {
            value = std::pow(std::clamp(value, 0.0, 1.0), gamma);
        }
        return;
    }

    const double range = max_value - min_value;
    for (double& value : values) {
        double normalized = (value - min_value) / range;
        normalized = std::clamp(normalized, 0.0, 1.0);
        value = std::pow(normalized, gamma) * range + min_value;
    }
}

inline void clip_to_dtype_inplace(std::vector<double>& values, const DTypeInfo& dtype) {
    if (!dtype_is_integer_like(dtype)) {
        return;
    }
    const double min_value = dtype_min_value(dtype);
    const double max_value = dtype_max_value(dtype);
    for (double& value : values) {
        value = std::round(std::clamp(value, min_value, max_value));
    }
}

inline cv::Mat make_mat_2d(const std::vector<double>& values, int height, int width) {
    cv::Mat mat(height, width, CV_64F);
    std::memcpy(mat.ptr<double>(), values.data(), static_cast<size_t>(height * width) * sizeof(double));
    return mat;
}

inline std::vector<double> mat_to_vector_2d(const cv::Mat& mat) {
    cv::Mat contiguous = mat.isContinuous() ? mat : mat.clone();
    const double* ptr = contiguous.ptr<double>();
    return std::vector<double>(ptr, ptr + static_cast<size_t>(contiguous.rows * contiguous.cols));
}

inline cv::Mat make_mat_hwc(const std::vector<double>& values, int height, int width, int channels) {
    cv::Mat mat(height, width, CV_MAKETYPE(CV_64F, channels));
    std::memcpy(mat.ptr<double>(), values.data(), static_cast<size_t>(height * width * channels) * sizeof(double));
    return mat;
}

inline std::vector<double> mat_to_vector_hwc(const cv::Mat& mat) {
    cv::Mat contiguous = mat.isContinuous() ? mat : mat.clone();
    const double* ptr = contiguous.ptr<double>();
    const size_t count = static_cast<size_t>(contiguous.rows) * static_cast<size_t>(contiguous.cols) * static_cast<size_t>(contiguous.channels());
    return std::vector<double>(ptr, ptr + count);
}

inline cv::Mat resize_to_shape(const cv::Mat& arr, int target_h, int target_w, int interpolation) {
    cv::Mat resized;
    cv::resize(arr, resized, cv::Size(target_w, target_h), 0.0, 0.0, interpolation);
    return resized;
}

inline cv::Mat rotate_keep_size(const cv::Mat& image, double rotate_deg, int interpolation) {
    cv::Mat rotated;
    cv::Point2f center(static_cast<float>(image.cols) / 2.0F, static_cast<float>(image.rows) / 2.0F);
    cv::Mat matrix = cv::getRotationMatrix2D(center, rotate_deg, 1.0);
    cv::warpAffine(image, rotated, matrix, image.size(), interpolation, cv::BORDER_CONSTANT, cv::Scalar());
    return rotated;
}

inline cv::Mat process_2d_or_hwc_mat(
    const cv::Mat& image,
    double scale_x,
    double scale_y,
    double rotate_deg,
    const std::optional<CropRect>& crop,
    double contrast,
    double gamma,
    const DTypeInfo& dtype) {
    if (scale_x <= 0.0 || scale_y <= 0.0) {
        throw std::runtime_error("scale-x 和 scale-y 必须大于 0");
    }

    cv::Mat out = image.clone();
    if (scale_x != 1.0 || scale_y != 1.0) {
        cv::resize(out, out, cv::Size(), scale_x, scale_y, cv::INTER_LINEAR);
    }

    if (rotate_deg != 0.0) {
        out = rotate_keep_size(out, rotate_deg, cv::INTER_LINEAR);
    }

    if (crop.has_value()) {
        const auto [x, y, w, h] = clip_crop_rect(crop->x, crop->y, crop->w, crop->h, out.cols, out.rows);
        out = out(cv::Rect(x, y, w, h)).clone();
    }

    std::vector<double> values;
    if (out.channels() == 1) {
        values = mat_to_vector_2d(out);
    } else {
        values = mat_to_vector_hwc(out);
    }

    if (contrast != 1.0) {
        for (double& value : values) {
            value *= contrast;
        }
    }

    if (gamma != 1.0) {
        apply_gamma_inplace(values, dtype, gamma);
    }
    clip_to_dtype_inplace(values, dtype);

    if (out.channels() == 1) {
        return make_mat_2d(values, out.rows, out.cols);
    }
    return make_mat_hwc(values, out.rows, out.cols, out.channels());
}

inline cv::Mat process_label_mat(
    const cv::Mat& label,
    double scale_x,
    double scale_y,
    double rotate_deg,
    const std::optional<CropRect>& crop) {
    if (scale_x <= 0.0 || scale_y <= 0.0) {
        throw std::runtime_error("scale-x 和 scale-y 必须大于 0");
    }

    cv::Mat out = label.clone();
    if (scale_x != 1.0 || scale_y != 1.0) {
        cv::resize(out, out, cv::Size(), scale_x, scale_y, cv::INTER_NEAREST);
    }
    if (rotate_deg != 0.0) {
        out = rotate_keep_size(out, rotate_deg, cv::INTER_NEAREST);
    }
    if (crop.has_value()) {
        const auto [x, y, w, h] = clip_crop_rect(crop->x, crop->y, crop->w, crop->h, out.cols, out.rows);
        out = out(cv::Rect(x, y, w, h)).clone();
    }
    return out;
}

inline std::vector<double> chw_to_hwc(const std::vector<double>& values, int channels, int height, int width) {
    std::vector<double> out(static_cast<size_t>(channels) * height * width, 0.0);
    for (int c = 0; c < channels; ++c) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t chw_index = (static_cast<size_t>(c) * height + y) * width + x;
                const size_t hwc_index = (static_cast<size_t>(y) * width + x) * channels + c;
                out[hwc_index] = values[chw_index];
            }
        }
    }
    return out;
}

inline std::vector<double> hwc_to_chw(const std::vector<double>& values, int channels, int height, int width) {
    std::vector<double> out(static_cast<size_t>(channels) * height * width, 0.0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < channels; ++c) {
                const size_t hwc_index = (static_cast<size_t>(y) * width + x) * channels + c;
                const size_t chw_index = (static_cast<size_t>(c) * height + y) * width + x;
                out[chw_index] = values[hwc_index];
            }
        }
    }
    return out;
}

inline std::pair<std::vector<double>, std::vector<size_t>> process_image_array(
    const std::vector<double>& image,
    const std::vector<size_t>& shape,
    const DTypeInfo& dtype,
    const Args& args) {
    if (shape.size() == 2) {
        const int height = static_cast<int>(shape[0]);
        const int width = static_cast<int>(shape[1]);
        cv::Mat out = process_2d_or_hwc_mat(make_mat_2d(image, height, width),
                                            args.scale_x,
                                            args.scale_y,
                                            args.rotate_deg,
                                            args.crop,
                                            args.contrast,
                                            args.gamma,
                                            dtype);
        if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
            out = resize_to_shape(out, height, width, cv::INTER_LINEAR);
        }
        return {mat_to_vector_2d(out), {static_cast<size_t>(out.rows), static_cast<size_t>(out.cols)}};
    }

    if (shape.size() == 3) {
        if (shape[2] <= 4) {
            const int height = static_cast<int>(shape[0]);
            const int width = static_cast<int>(shape[1]);
            const int channels = static_cast<int>(shape[2]);
            cv::Mat out = process_2d_or_hwc_mat(make_mat_hwc(image, height, width, channels),
                                                args.scale_x,
                                                args.scale_y,
                                                args.rotate_deg,
                                                args.crop,
                                                args.contrast,
                                                args.gamma,
                                                dtype);
            if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
                out = resize_to_shape(out, height, width, cv::INTER_LINEAR);
            }
            return {mat_to_vector_hwc(out), {static_cast<size_t>(out.rows), static_cast<size_t>(out.cols), static_cast<size_t>(out.channels())}};
        }

        if (shape[0] <= 4) {
            const int channels = static_cast<int>(shape[0]);
            const int height = static_cast<int>(shape[1]);
            const int width = static_cast<int>(shape[2]);
            std::vector<double> hwc = chw_to_hwc(image, channels, height, width);
            cv::Mat out = process_2d_or_hwc_mat(make_mat_hwc(hwc, height, width, channels),
                                                args.scale_x,
                                                args.scale_y,
                                                args.rotate_deg,
                                                args.crop,
                                                args.contrast,
                                                args.gamma,
                                                dtype);
            if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
                out = resize_to_shape(out, height, width, cv::INTER_LINEAR);
            }
            std::vector<double> out_hwc = mat_to_vector_hwc(out);
            return {hwc_to_chw(out_hwc, out.channels(), out.rows, out.cols),
                    {static_cast<size_t>(out.channels()), static_cast<size_t>(out.rows), static_cast<size_t>(out.cols)}};
        }

        const int count = static_cast<int>(shape[0]);
        const int height = static_cast<int>(shape[1]);
        const int width = static_cast<int>(shape[2]);
        const size_t plane_size = static_cast<size_t>(height) * width;
        std::vector<double> output;
        std::vector<size_t> out_shape = {shape[0], shape[1], shape[2]};
        for (int index = 0; index < count; ++index) {
            const auto begin = image.begin() + static_cast<long>(index * plane_size);
            const auto end = begin + static_cast<long>(plane_size);
            std::vector<double> slice(begin, end);
            cv::Mat out = process_2d_or_hwc_mat(make_mat_2d(slice, height, width),
                                                args.scale_x,
                                                args.scale_y,
                                                args.rotate_deg,
                                                args.crop,
                                                args.contrast,
                                                args.gamma,
                                                dtype);
            if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
                out = resize_to_shape(out, height, width, cv::INTER_LINEAR);
            }
            if (index == 0) {
                out_shape = {shape[0], static_cast<size_t>(out.rows), static_cast<size_t>(out.cols)};
            }
            std::vector<double> slice_out = mat_to_vector_2d(out);
            output.insert(output.end(), slice_out.begin(), slice_out.end());
        }
        return {output, out_shape};
    }

    throw std::runtime_error("不支持的 image 维度，仅支持 2D 或 3D");
}

inline std::pair<std::vector<double>, std::vector<size_t>> process_label_array(
    const std::vector<double>& label,
    const std::vector<size_t>& shape,
    const Args& args) {
    if (shape.size() == 2) {
        const int height = static_cast<int>(shape[0]);
        const int width = static_cast<int>(shape[1]);
        cv::Mat out = process_label_mat(make_mat_2d(label, height, width),
                                        args.scale_x,
                                        args.scale_y,
                                        args.rotate_deg,
                                        args.crop);
        if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
            out = resize_to_shape(out, height, width, cv::INTER_NEAREST);
        }
        return {mat_to_vector_2d(out), {static_cast<size_t>(out.rows), static_cast<size_t>(out.cols)}};
    }

    if (shape.size() == 3) {
        if (shape[2] <= 4) {
            const int height = static_cast<int>(shape[0]);
            const int width = static_cast<int>(shape[1]);
            const int channels = static_cast<int>(shape[2]);
            cv::Mat out = process_label_mat(make_mat_hwc(label, height, width, channels),
                                            args.scale_x,
                                            args.scale_y,
                                            args.rotate_deg,
                                            args.crop);
            if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
                out = resize_to_shape(out, height, width, cv::INTER_NEAREST);
            }
            return {mat_to_vector_hwc(out), {static_cast<size_t>(out.rows), static_cast<size_t>(out.cols), static_cast<size_t>(out.channels())}};
        }

        if (shape[0] <= 4) {
            const int channels = static_cast<int>(shape[0]);
            const int height = static_cast<int>(shape[1]);
            const int width = static_cast<int>(shape[2]);
            std::vector<double> hwc = chw_to_hwc(label, channels, height, width);
            cv::Mat out = process_label_mat(make_mat_hwc(hwc, height, width, channels),
                                            args.scale_x,
                                            args.scale_y,
                                            args.rotate_deg,
                                            args.crop);
            if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
                out = resize_to_shape(out, height, width, cv::INTER_NEAREST);
            }
            std::vector<double> out_hwc = mat_to_vector_hwc(out);
            return {hwc_to_chw(out_hwc, out.channels(), out.rows, out.cols),
                    {static_cast<size_t>(out.channels()), static_cast<size_t>(out.rows), static_cast<size_t>(out.cols)}};
        }

        const int count = static_cast<int>(shape[0]);
        const int height = static_cast<int>(shape[1]);
        const int width = static_cast<int>(shape[2]);
        const size_t plane_size = static_cast<size_t>(height) * width;
        std::vector<double> output;
        std::vector<size_t> out_shape = {shape[0], shape[1], shape[2]};
        for (int index = 0; index < count; ++index) {
            const auto begin = label.begin() + static_cast<long>(index * plane_size);
            const auto end = begin + static_cast<long>(plane_size);
            std::vector<double> slice(begin, end);
            cv::Mat out = process_label_mat(make_mat_2d(slice, height, width),
                                            args.scale_x,
                                            args.scale_y,
                                            args.rotate_deg,
                                            args.crop);
            if (args.preserve_resolution && (out.rows != height || out.cols != width)) {
                out = resize_to_shape(out, height, width, cv::INTER_NEAREST);
            }
            if (index == 0) {
                out_shape = {shape[0], static_cast<size_t>(out.rows), static_cast<size_t>(out.cols)};
            }
            std::vector<double> slice_out = mat_to_vector_2d(out);
            output.insert(output.end(), slice_out.begin(), slice_out.end());
        }
        return {output, out_shape};
    }

    throw std::runtime_error("不支持的 label 维度，仅支持 2D 或 3D");
}

inline fs::path derive_output_path(const fs::path& input_path, const fs::path& output_path) {
    if (!output_path.empty()) {
        return output_path;
    }
    return input_path.parent_path() / (input_path.stem().string() + "_processed.npz");
}

inline void process_npz_file(const Args& args) {
    if (!fs::exists(args.input)) {
        throw std::runtime_error("输入文件不存在: " + args.input.string());
    }

    auto entries = load_npz_entries(args.input);
    std::map<std::string, size_t> name_to_index;
    for (size_t i = 0; i < entries.size(); ++i) {
        name_to_index[entries[i].name] = i;
    }
    if (!name_to_index.count("image.npy")) {
        throw std::runtime_error("输入 NPZ 中缺少 image 键");
    }

    auto& image_entry = entries[name_to_index["image.npy"]];
    const NpyMeta image_meta = parse_npy_meta(image_entry.data);
    const DTypeInfo image_dtype = parse_dtype(image_meta.descr);
    const auto image_values = decode_numeric_data(image_entry.data, image_meta);
    const auto [processed_image, image_shape] = process_image_array(image_values, image_meta.shape, image_dtype, args);
    NpyMeta new_image_meta = image_meta;
    new_image_meta.shape = image_shape;
    new_image_meta.fortran_order = false;
    image_entry.data = make_npy_bytes(new_image_meta, encode_numeric_data(processed_image, new_image_meta));

    if (name_to_index.count("label.npy")) {
        auto& label_entry = entries[name_to_index["label.npy"]];
        const NpyMeta label_meta = parse_npy_meta(label_entry.data);
        const auto label_values = decode_numeric_data(label_entry.data, label_meta);
        const auto [processed_label, label_shape] = process_label_array(label_values, label_meta.shape, args);
        NpyMeta new_label_meta = label_meta;
        new_label_meta.shape = label_shape;
        new_label_meta.fortran_order = false;
        label_entry.data = make_npy_bytes(new_label_meta, encode_numeric_data(processed_label, new_label_meta));
    }

    const fs::path output_path = derive_output_path(args.input, args.output);
    write_file_bytes(output_path, save_npz_entries(entries));
}

}  // namespace npzproc
