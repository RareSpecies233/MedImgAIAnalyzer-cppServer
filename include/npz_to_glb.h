#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <zlib.h>

#include "cnpy.h"

namespace npz_to_glb {
namespace fs = std::filesystem;

struct MeshData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
    float min_pos[3] = {0.0f, 0.0f, 0.0f};
    float max_pos[3] = {0.0f, 0.0f, 0.0f};
};

struct PrimitiveData {
    MeshData mesh;
    bool use_texture = false;
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct Options {
    std::string raw_key;
    std::string ann_key;
    float ann_threshold = 0.5f;
    bool use_raw_threshold = false;
};

static inline bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static inline bool natural_less(const fs::path &a, const fs::path &b) {
    const std::string sa = a.filename().string();
    const std::string sb = b.filename().string();
    size_t i = 0;
    size_t j = 0;
    while (i < sa.size() && j < sb.size()) {
        if (is_digit(sa[i]) && is_digit(sb[j])) {
            size_t i0 = i;
            size_t j0 = j;
            while (i < sa.size() && is_digit(sa[i])) {
                ++i;
            }
            while (j < sb.size() && is_digit(sb[j])) {
                ++j;
            }
            const std::string na = sa.substr(i0, i - i0);
            const std::string nb = sb.substr(j0, j - j0);
            const long long va = std::stoll(na);
            const long long vb = std::stoll(nb);
            if (va != vb) {
                return va < vb;
            }
        } else {
            const char ca = static_cast<char>(std::tolower(sa[i]));
            const char cb = static_cast<char>(std::tolower(sb[j]));
            if (ca != cb) {
                return ca < cb;
            }
            ++i;
            ++j;
        }
    }
    return sa.size() < sb.size();
}

static inline size_t compute_index(const std::vector<size_t> &idx,
                                   const std::vector<size_t> &shape,
                                   bool fortran_order) {
    size_t n = shape.size();
    size_t index = 0;
    if (fortran_order) {
        size_t stride = 1;
        for (size_t k = 0; k < n; ++k) {
            index += idx[k] * stride;
            stride *= shape[k];
        }
    } else {
        size_t stride = 1;
        for (size_t k = n; k-- > 0;) {
            index += idx[k] * stride;
            stride *= shape[k];
        }
    }
    return index;
}

static inline float read_value_as_float(const cnpy::NpyArray &arr, size_t idx) {
    const unsigned char *data = reinterpret_cast<const unsigned char *>(arr.data_holder->data());
    switch (arr.word_size) {
        case 1:
            return static_cast<float>(data[idx]);
        case 2: {
            uint16_t v;
            std::memcpy(&v, data + idx * 2, sizeof(uint16_t));
            return static_cast<float>(v);
        }
        case 4: {
            float v;
            std::memcpy(&v, data + idx * 4, sizeof(float));
            return v;
        }
        case 8: {
            double v;
            std::memcpy(&v, data + idx * 8, sizeof(double));
            return static_cast<float>(v);
        }
        default:
            return 0.0f;
    }
}

static inline const cnpy::NpyArray *find_array_by_key(const cnpy::npz_t &npz,
                                                      const std::string &key) {
    auto it = npz.find(key);
    if (it == npz.end()) {
        return nullptr;
    }
    return &it->second;
}

static inline const cnpy::NpyArray *find_array_by_keys(const cnpy::npz_t &npz,
                                                       const std::vector<std::string> &keys) {
    for (const auto &k : keys) {
        auto it = npz.find(k);
        if (it != npz.end()) {
            return &it->second;
        }
    }
    return nullptr;
}

static inline void squeeze_shape(std::vector<size_t> &shape) {
    std::vector<size_t> out;
    for (size_t v : shape) {
        if (v != 1) {
            out.push_back(v);
        }
    }
    shape.swap(out);
}

static inline bool extract_2d(const cnpy::NpyArray &arr,
                              std::vector<float> &out,
                              size_t &height,
                              size_t &width) {
    std::vector<size_t> shape = arr.shape;
    squeeze_shape(shape);

    if (shape.size() == 2) {
        height = shape[0];
        width = shape[1];
        out.resize(height * width);
        for (size_t y = 0; y < height; ++y) {
            for (size_t x = 0; x < width; ++x) {
                size_t idx = compute_index({y, x}, shape, arr.fortran_order);
                out[y * width + x] = read_value_as_float(arr, idx);
            }
        }
        return true;
    }

    if (shape.size() == 3) {
        size_t c0 = shape[0];
        size_t c2 = shape[2];
        if (c0 <= 4) {
            height = shape[1];
            width = shape[2];
            out.resize(height * width);
            for (size_t y = 0; y < height; ++y) {
                for (size_t x = 0; x < width; ++x) {
                    size_t idx = compute_index({0, y, x}, shape, arr.fortran_order);
                    out[y * width + x] = read_value_as_float(arr, idx);
                }
            }
            return true;
        }
        if (c2 <= 4) {
            height = shape[0];
            width = shape[1];
            out.resize(height * width);
            for (size_t y = 0; y < height; ++y) {
                for (size_t x = 0; x < width; ++x) {
                    size_t idx = compute_index({y, x, 0}, shape, arr.fortran_order);
                    out[y * width + x] = read_value_as_float(arr, idx);
                }
            }
            return true;
        }
    }

    return false;
}

static inline bool load_slices(const fs::path &input_dir,
                               const Options &opts,
                               std::vector<float> &raw_volume,
                               std::vector<float> &ann_volume,
                               size_t &z_count,
                               size_t &height,
                               size_t &width,
                               bool &has_ann) {
    if (!fs::exists(input_dir)) {
        throw std::runtime_error("Input dir not found: " + input_dir.string());
    }

    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(input_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() == ".npz") {
            files.push_back(entry.path());
        }
    }

    if (files.empty()) {
        throw std::runtime_error("No .npz files found in: " + input_dir.string());
    }

    std::sort(files.begin(), files.end(), natural_less);

    std::vector<float> raw_slice;
    std::vector<float> ann_slice;
    size_t slice_h = 0;
    size_t slice_w = 0;

    std::vector<float> raws;
    std::vector<float> anns;
    has_ann = false;

    static const std::vector<std::string> kRawKeys = {
        "image", "img", "raw", "ct", "data", "slice", "input"
    };
    static const std::vector<std::string> kAnnKeys = {
        "label", "mask", "seg", "annotation", "gt"
    };

    for (const auto &file : files) {
        cnpy::npz_t npz = cnpy::npz_load(file.string());
        const cnpy::NpyArray *raw_arr = nullptr;
        const cnpy::NpyArray *ann_arr = nullptr;

        if (!opts.raw_key.empty()) {
            raw_arr = find_array_by_key(npz, opts.raw_key);
        }
        if (!opts.ann_key.empty()) {
            ann_arr = find_array_by_key(npz, opts.ann_key);
        }
        if (!raw_arr) {
            raw_arr = find_array_by_keys(npz, kRawKeys);
        }
        if (!ann_arr) {
            ann_arr = find_array_by_keys(npz, kAnnKeys);
        }

        if (!raw_arr && !npz.empty()) {
            raw_arr = &npz.begin()->second;
        }

        if (!raw_arr) {
            throw std::runtime_error("No raw array found in " + file.string());
        }

        raw_slice.clear();
        if (!extract_2d(*raw_arr, raw_slice, slice_h, slice_w)) {
            throw std::runtime_error("Failed to extract 2D raw from " + file.string());
        }

        if (raws.empty()) {
            height = slice_h;
            width = slice_w;
        } else if (slice_h != height || slice_w != width) {
            throw std::runtime_error("Slice size mismatch in " + file.string());
        }

        if (ann_arr) {
            ann_slice.clear();
            size_t ah = 0;
            size_t aw = 0;
            if (!extract_2d(*ann_arr, ann_slice, ah, aw)) {
                throw std::runtime_error("Failed to extract 2D ann from " + file.string());
            }
            if (ah != height || aw != width) {
                throw std::runtime_error("Annotation size mismatch in " + file.string());
            }
            has_ann = true;
        } else {
            ann_slice.assign(height * width, 0.0f);
        }

        raws.insert(raws.end(), raw_slice.begin(), raw_slice.end());
        anns.insert(anns.end(), ann_slice.begin(), ann_slice.end());
    }

    z_count = files.size();
    raw_volume.swap(raws);
    ann_volume.swap(anns);
    return true;
}

static inline std::vector<unsigned char> build_texture_from_raw(const std::vector<float> &raw_volume,
                                                                size_t z_count,
                                                                size_t height,
                                                                size_t width) {
    std::vector<float> acc(height * width, 0.0f);
    const size_t slice_size = height * width;
    for (size_t z = 0; z < z_count; ++z) {
        const float *slice = raw_volume.data() + z * slice_size;
        for (size_t i = 0; i < slice_size; ++i) {
            acc[i] += slice[i];
        }
    }
    for (float &v : acc) {
        v /= static_cast<float>(z_count);
    }

    float vmin = acc[0];
    float vmax = acc[0];
    for (float v : acc) {
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }

    std::vector<unsigned char> rgb(height * width * 3, 0);
    for (size_t i = 0; i < acc.size(); ++i) {
        float v = acc[i];
        float t = (vmax > vmin) ? (v - vmin) / (vmax - vmin) : 0.0f;
        unsigned char g = static_cast<unsigned char>(std::round(t * 255.0f));
        rgb[i * 3 + 0] = g;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = g;
    }

    auto write_u32_be = [](std::vector<unsigned char> &out, uint32_t v) {
        out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
        out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
        out.push_back(static_cast<unsigned char>(v & 0xFF));
    };

    auto write_chunk = [&](std::vector<unsigned char> &out,
                           const char type[4],
                           const std::vector<unsigned char> &data) {
        write_u32_be(out, static_cast<uint32_t>(data.size()));
        out.insert(out.end(), type, type + 4);
        if (!data.empty()) {
            out.insert(out.end(), data.begin(), data.end());
        }
        uint32_t crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const unsigned char *>(type), 4);
        if (!data.empty()) {
            crc = crc32(crc, data.data(), data.size());
        }
        write_u32_be(out, crc);
    };

    std::vector<unsigned char> raw;
    raw.resize(height * (width * 3 + 1));
    for (size_t y = 0; y < height; ++y) {
        raw[y * (width * 3 + 1)] = 0;
        std::memcpy(raw.data() + y * (width * 3 + 1) + 1,
                    rgb.data() + y * width * 3,
                    width * 3);
    }

    uLongf comp_bound = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(comp_bound);
    uLongf comp_size = comp_bound;
    if (compress2(compressed.data(), &comp_size, raw.data(), static_cast<uLong>(raw.size()), Z_BEST_SPEED) != Z_OK) {
        return {};
    }
    compressed.resize(comp_size);

    std::vector<unsigned char> png;
    const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), sig, sig + 8);

    std::vector<unsigned char> ihdr;
    write_u32_be(ihdr, static_cast<uint32_t>(width));
    write_u32_be(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);
    ihdr.push_back(2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);

    write_chunk(png, "IHDR", ihdr);
    write_chunk(png, "IDAT", compressed);
    write_chunk(png, "IEND", {});
    return png;
}

static inline void update_minmax(MeshData &mesh, float x, float y, float z, bool init) {
    if (init) {
        mesh.min_pos[0] = mesh.max_pos[0] = x;
        mesh.min_pos[1] = mesh.max_pos[1] = y;
        mesh.min_pos[2] = mesh.max_pos[2] = z;
        return;
    }
    mesh.min_pos[0] = std::min(mesh.min_pos[0], x);
    mesh.min_pos[1] = std::min(mesh.min_pos[1], y);
    mesh.min_pos[2] = std::min(mesh.min_pos[2], z);
    mesh.max_pos[0] = std::max(mesh.max_pos[0], x);
    mesh.max_pos[1] = std::max(mesh.max_pos[1], y);
    mesh.max_pos[2] = std::max(mesh.max_pos[2], z);
}

static inline void normalize_vec3(float &x, float &y, float &z) {
    float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-8f) {
        x /= len;
        y /= len;
        z /= len;
    }
}

static inline MeshData build_mesh_from_scalar(const std::vector<float> &vol,
                                              size_t z_count,
                                              size_t height,
                                              size_t width) {
    MeshData mesh;
    const size_t slice_size = height * width;

    static const int edgeTable[256] = {
        0x0, 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
        0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
        0x190, 0x99, 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
        0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
        0x230, 0x339, 0x33, 0x13a, 0x636, 0x73f, 0x435, 0x53c,
        0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
        0x3a0, 0x2a9, 0x1a3, 0xaa, 0x7a6, 0x6af, 0x5a5, 0x4ac,
        0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
        0x460, 0x569, 0x663, 0x76a, 0x66, 0x16f, 0x265, 0x36c,
        0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
        0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff, 0x3f5, 0x2fc,
        0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
        0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55, 0x15c,
        0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
        0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc,
        0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
        0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
        0xcc, 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
        0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
        0x15c, 0x55, 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
        0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
        0x2fc, 0x3f5, 0xff, 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
        0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
        0x36c, 0x265, 0x16f, 0x66, 0x76a, 0x663, 0x569, 0x460,
        0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
        0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa, 0x1a3, 0x2a9, 0x3a0,
        0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
        0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33, 0x339, 0x230,
        0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
        0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99, 0x190,
        0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
        0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0
    };

    static const int triTable[256][16] = {
        {-1},
        {0, 8, 3, -1},
        {0, 1, 9, -1},
        {1, 8, 3, 9, 8, 1, -1},
        {1, 2, 10, -1},
        {0, 8, 3, 1, 2, 10, -1},
        {9, 2, 10, 0, 2, 9, -1},
        {2, 8, 3, 2, 10, 8, 10, 9, 8, -1},
        {3, 11, 2, -1},
        {0, 11, 2, 8, 11, 0, -1},
        {1, 9, 0, 2, 3, 11, -1},
        {1, 11, 2, 1, 9, 11, 9, 8, 11, -1},
        {3, 10, 1, 11, 10, 3, -1},
        {0, 10, 1, 0, 8, 10, 8, 11, 10, -1},
        {3, 9, 0, 3, 11, 9, 11, 10, 9, -1},
        {9, 8, 10, 10, 8, 11, -1},
        {4, 7, 8, -1},
        {4, 3, 0, 7, 3, 4, -1},
        {0, 1, 9, 8, 4, 7, -1},
        {4, 1, 9, 4, 7, 1, 7, 3, 1, -1},
        {1, 2, 10, 8, 4, 7, -1},
        {3, 4, 7, 3, 0, 4, 1, 2, 10, -1},
        {9, 2, 10, 9, 0, 2, 8, 4, 7, -1},
        {2, 10, 9, 2, 9, 7, 2, 7, 3, 7, 9, 4, -1},
        {8, 4, 7, 3, 11, 2, -1},
        {11, 4, 7, 11, 2, 4, 2, 0, 4, -1},
        {9, 0, 1, 8, 4, 7, 2, 3, 11, -1},
        {4, 7, 11, 9, 4, 11, 9, 11, 2, 9, 2, 1, -1},
        {3, 10, 1, 3, 11, 10, 7, 8, 4, -1},
        {1, 11, 10, 1, 4, 11, 1, 0, 4, 7, 11, 4, -1},
        {4, 7, 8, 9, 0, 11, 9, 11, 10, 11, 0, 3, -1},
        {4, 7, 11, 4, 11, 9, 9, 11, 10, -1},
        {9, 5, 4, -1},
        {9, 5, 4, 0, 8, 3, -1},
        {0, 5, 4, 1, 5, 0, -1},
        {8, 5, 4, 8, 3, 5, 3, 1, 5, -1},
        {1, 2, 10, 9, 5, 4, -1},
        {3, 0, 8, 1, 2, 10, 4, 9, 5, -1},
        {5, 2, 10, 5, 4, 2, 4, 0, 2, -1},
        {2, 10, 5, 3, 2, 5, 3, 5, 4, 3, 4, 8, -1},
        {9, 5, 4, 2, 3, 11, -1},
        {0, 11, 2, 0, 8, 11, 4, 9, 5, -1},
        {0, 5, 4, 0, 1, 5, 2, 3, 11, -1},
        {2, 1, 5, 2, 5, 8, 2, 8, 11, 4, 8, 5, -1},
        {10, 3, 11, 10, 1, 3, 9, 5, 4, -1},
        {4, 9, 5, 0, 8, 1, 8, 10, 1, 8, 11, 10, -1},
        {5, 4, 0, 5, 0, 11, 5, 11, 10, 11, 0, 3, -1},
        {5, 4, 8, 5, 8, 10, 10, 8, 11, -1},
        {9, 7, 8, 5, 7, 9, -1},
        {9, 3, 0, 9, 5, 3, 5, 7, 3, -1},
        {0, 7, 8, 0, 1, 7, 1, 5, 7, -1},
        {1, 5, 3, 3, 5, 7, -1},
        {9, 7, 8, 9, 5, 7, 10, 1, 2, -1},
        {10, 1, 2, 9, 5, 0, 5, 3, 0, 5, 7, 3, -1},
        {8, 0, 2, 8, 2, 5, 8, 5, 7, 10, 5, 2, -1},
        {2, 10, 5, 2, 5, 3, 3, 5, 7, -1},
        {7, 9, 5, 7, 8, 9, 3, 11, 2, -1},
        {9, 5, 7, 9, 7, 2, 9, 2, 0, 2, 7, 11, -1},
        {2, 3, 11, 0, 1, 8, 1, 7, 8, 1, 5, 7, -1},
        {11, 2, 1, 11, 1, 7, 7, 1, 5, -1},
        {9, 5, 8, 8, 5, 7, 10, 1, 3, 10, 3, 11, -1},
        {5, 7, 0, 5, 0, 9, 7, 11, 0, 1, 0, 10, 11, 10, 0, -1},
        {11, 10, 0, 11, 0, 3, 10, 5, 0, 8, 0, 7, 5, 7, 0, -1},
        {11, 10, 5, 7, 11, 5, -1},
        {10, 6, 5, -1},
        {0, 8, 3, 5, 10, 6, -1},
        {9, 0, 1, 5, 10, 6, -1},
        {1, 8, 3, 1, 9, 8, 5, 10, 6, -1},
        {1, 6, 5, 2, 6, 1, -1},
        {1, 6, 5, 1, 2, 6, 3, 0, 8, -1},
        {9, 6, 5, 9, 0, 6, 0, 2, 6, -1},
        {5, 9, 8, 5, 8, 2, 5, 2, 6, 3, 2, 8, -1},
        {2, 3, 11, 10, 6, 5, -1},
        {11, 0, 8, 11, 2, 0, 10, 6, 5, -1},
        {0, 1, 9, 2, 3, 11, 5, 10, 6, -1},
        {5, 10, 6, 1, 9, 2, 9, 11, 2, 9, 8, 11, -1},
        {6, 3, 11, 6, 5, 3, 5, 1, 3, -1},
        {0, 8, 11, 0, 11, 5, 0, 5, 1, 5, 11, 6, -1},
        {3, 11, 6, 0, 3, 6, 0, 6, 5, 0, 5, 9, -1},
        {6, 5, 9, 6, 9, 11, 11, 9, 8, -1},
        {5, 10, 6, 4, 7, 8, -1},
        {4, 3, 0, 4, 7, 3, 6, 5, 10, -1},
        {1, 9, 0, 5, 10, 6, 8, 4, 7, -1},
        {10, 6, 5, 1, 9, 7, 1, 7, 3, 7, 9, 4, -1},
        {6, 1, 2, 6, 5, 1, 4, 7, 8, -1},
        {1, 2, 5, 5, 2, 6, 3, 0, 4, 3, 4, 7, -1},
        {8, 4, 7, 9, 0, 5, 0, 6, 5, 0, 2, 6, -1},
        {7, 3, 9, 7, 9, 4, 3, 2, 9, 5, 9, 6, 2, 6, 9, -1},
        {3, 11, 2, 7, 8, 4, 10, 6, 5, -1},
        {5, 10, 6, 4, 7, 2, 4, 2, 0, 2, 7, 11, -1},
        {0, 1, 9, 4, 7, 8, 2, 3, 11, 5, 10, 6, -1},
        {9, 2, 1, 9, 11, 2, 9, 4, 11, 7, 11, 4, 5, 10, 6, -1},
        {8, 4, 7, 3, 11, 5, 3, 5, 1, 5, 11, 6, -1},
        {5, 1, 11, 5, 11, 6, 1, 0, 11, 7, 11, 4, 0, 4, 11, -1},
        {0, 5, 9, 0, 6, 5, 0, 3, 6, 11, 6, 3, 8, 4, 7, -1},
        {6, 5, 9, 6, 9, 11, 4, 7, 9, 7, 11, 9, -1},
        {10, 4, 9, 6, 4, 10, -1},
        {4, 10, 6, 4, 9, 10, 0, 8, 3, -1},
        {10, 0, 1, 10, 6, 0, 6, 4, 0, -1},
        {8, 3, 1, 8, 1, 6, 8, 6, 4, 6, 1, 10, -1},
        {1, 4, 9, 1, 2, 4, 2, 6, 4, -1},
        {3, 0, 8, 1, 2, 9, 2, 4, 9, 2, 6, 4, -1},
        {0, 2, 4, 4, 2, 6, -1},
        {8, 3, 2, 8, 2, 4, 4, 2, 6, -1},
        {10, 4, 9, 10, 6, 4, 11, 2, 3, -1},
        {0, 8, 2, 2, 8, 11, 4, 9, 10, 4, 10, 6, -1},
        {3, 11, 2, 0, 1, 6, 0, 6, 4, 6, 1, 10, -1},
        {6, 4, 1, 6, 1, 10, 4, 8, 1, 2, 1, 11, 8, 11, 1, -1},
        {9, 6, 4, 9, 3, 6, 9, 1, 3, 11, 6, 3, -1},
        {8, 11, 1, 8, 1, 0, 11, 6, 1, 9, 1, 4, 6, 4, 1, -1},
        {3, 11, 6, 3, 6, 0, 0, 6, 4, -1},
        {6, 4, 8, 11, 6, 8, -1},
        {7, 10, 6, 7, 8, 10, 8, 9, 10, -1},
        {0, 7, 3, 0, 10, 7, 0, 9, 10, 6, 7, 10, -1},
        {10, 6, 7, 1, 10, 7, 1, 7, 8, 1, 8, 0, -1},
        {10, 6, 7, 10, 7, 1, 1, 7, 3, -1},
        {1, 2, 6, 1, 6, 8, 1, 8, 9, 8, 6, 7, -1},
        {2, 6, 9, 2, 9, 1, 6, 7, 9, 0, 9, 3, 7, 3, 9, -1},
        {7, 8, 0, 7, 0, 6, 6, 0, 2, -1},
        {7, 3, 2, 6, 7, 2, -1},
        {2, 3, 11, 10, 6, 8, 10, 8, 9, 8, 6, 7, -1},
        {2, 0, 7, 2, 7, 11, 0, 9, 7, 6, 7, 10, 9, 10, 7, -1},
        {1, 8, 0, 1, 7, 8, 1, 10, 7, 6, 7, 10, 2, 3, 11, -1},
        {11, 2, 1, 11, 1, 7, 10, 6, 1, 6, 7, 1, -1},
        {8, 9, 6, 8, 6, 7, 9, 1, 6, 11, 6, 3, 1, 3, 6, -1},
        {0, 9, 1, 11, 6, 7, -1},
        {7, 8, 0, 7, 0, 6, 3, 11, 0, 11, 6, 0, -1},
        {7, 11, 6, -1},
        {7, 6, 11, -1},
        {3, 0, 8, 11, 7, 6, -1},
        {0, 1, 9, 11, 7, 6, -1},
        {8, 1, 9, 8, 3, 1, 11, 7, 6, -1},
        {10, 1, 2, 6, 11, 7, -1},
        {1, 2, 10, 3, 0, 8, 6, 11, 7, -1},
        {2, 9, 0, 2, 10, 9, 6, 11, 7, -1},
        {6, 11, 7, 2, 10, 3, 10, 8, 3, 10, 9, 8, -1},
        {7, 2, 3, 6, 2, 7, -1},
        {7, 0, 8, 7, 6, 0, 6, 2, 0, -1},
        {2, 7, 6, 2, 3, 7, 0, 1, 9, -1},
        {1, 6, 2, 1, 8, 6, 1, 9, 8, 8, 7, 6, -1},
        {10, 7, 6, 10, 1, 7, 1, 3, 7, -1},
        {10, 7, 6, 1, 7, 10, 1, 8, 7, 1, 0, 8, -1},
        {0, 3, 7, 0, 7, 10, 0, 10, 9, 6, 10, 7, -1},
        {7, 6, 10, 7, 10, 8, 8, 10, 9, -1},
        {6, 8, 4, 11, 8, 6, -1},
        {3, 6, 11, 3, 0, 6, 0, 4, 6, -1},
        {8, 6, 11, 8, 4, 6, 9, 0, 1, -1},
        {9, 4, 6, 9, 6, 3, 9, 3, 1, 11, 3, 6, -1},
        {6, 8, 4, 6, 11, 8, 2, 10, 1, -1},
        {1, 2, 10, 3, 0, 11, 0, 6, 11, 0, 4, 6, -1},
        {4, 11, 8, 4, 6, 11, 0, 2, 9, 2, 10, 9, -1},
        {10, 9, 3, 10, 3, 2, 9, 4, 3, 11, 3, 6, 4, 6, 3, -1},
        {8, 2, 3, 8, 4, 2, 4, 6, 2, -1},
        {0, 4, 2, 4, 6, 2, -1},
        {1, 9, 0, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1},
        {1, 9, 4, 1, 4, 2, 2, 4, 6, -1},
        {8, 1, 3, 8, 6, 1, 8, 4, 6, 6, 10, 1, -1},
        {10, 1, 0, 10, 0, 6, 6, 0, 4, -1},
        {4, 6, 3, 4, 3, 8, 6, 10, 3, 0, 3, 9, 10, 9, 3, -1},
        {10, 9, 4, 6, 10, 4, -1},
        {4, 9, 5, 7, 6, 11, -1},
        {0, 8, 3, 4, 9, 5, 11, 7, 6, -1},
        {5, 0, 1, 5, 4, 0, 7, 6, 11, -1},
        {11, 7, 6, 8, 3, 4, 3, 5, 4, 3, 1, 5, -1},
        {9, 5, 4, 10, 1, 2, 7, 6, 11, -1},
        {6, 11, 7, 1, 2, 10, 0, 8, 3, 4, 9, 5, -1},
        {7, 6, 11, 5, 4, 10, 4, 2, 10, 4, 0, 2, -1},
        {3, 4, 8, 3, 5, 4, 3, 2, 5, 10, 5, 2, 11, 7, 6, -1},
        {7, 2, 3, 7, 6, 2, 5, 4, 9, -1},
        {9, 5, 4, 0, 8, 6, 0, 6, 2, 6, 8, 7, -1},
        {3, 6, 2, 3, 7, 6, 1, 5, 0, 5, 4, 0, -1},
        {6, 2, 8, 6, 8, 7, 2, 1, 8, 4, 8, 5, 1, 5, 8, -1},
        {9, 5, 4, 10, 1, 6, 1, 7, 6, 1, 3, 7, -1},
        {1, 6, 10, 1, 7, 6, 1, 0, 7, 8, 7, 0, 9, 5, 4, -1},
        {4, 0, 10, 4, 10, 5, 0, 3, 10, 6, 10, 7, 3, 7, 10, -1},
        {7, 6, 10, 7, 10, 8, 5, 4, 10, 4, 8, 10, -1},
        {6, 9, 5, 6, 11, 9, 11, 8, 9, -1},
        {3, 6, 11, 0, 6, 3, 0, 5, 6, 0, 9, 5, -1},
        {0, 11, 8, 0, 5, 11, 0, 1, 5, 5, 6, 11, -1},
        {6, 11, 3, 6, 3, 5, 5, 3, 1, -1},
        {1, 2, 10, 9, 5, 11, 9, 11, 8, 11, 5, 6, -1},
        {0, 11, 3, 0, 6, 11, 0, 9, 6, 5, 6, 9, 1, 2, 10, -1},
        {11, 8, 5, 11, 5, 6, 8, 0, 5, 10, 5, 2, 0, 2, 5, -1},
        {6, 11, 3, 6, 3, 5, 2, 10, 3, 10, 5, 3, -1},
        {5, 8, 9, 5, 2, 8, 5, 6, 2, 3, 8, 2, -1},
        {9, 5, 6, 9, 6, 0, 0, 6, 2, -1},
        {1, 5, 8, 1, 8, 0, 5, 6, 8, 3, 8, 2, 6, 2, 8, -1},
        {1, 5, 6, 2, 1, 6, -1},
        {1, 3, 6, 1, 6, 10, 3, 8, 6, 5, 6, 9, 8, 9, 6, -1},
        {10, 1, 0, 10, 0, 6, 9, 5, 0, 5, 6, 0, -1},
        {0, 3, 8, 5, 6, 10, -1},
        {10, 5, 6, -1},
        {11, 5, 10, 7, 5, 11, -1},
        {11, 5, 10, 11, 7, 5, 8, 3, 0, -1},
        {5, 11, 7, 5, 10, 11, 1, 9, 0, -1},
        {10, 7, 5, 10, 11, 7, 9, 8, 1, 8, 3, 1, -1},
        {11, 1, 2, 11, 7, 1, 7, 5, 1, -1},
        {0, 8, 3, 1, 2, 7, 1, 7, 5, 7, 2, 11, -1},
        {9, 7, 5, 9, 2, 7, 9, 0, 2, 2, 11, 7, -1},
        {7, 5, 2, 7, 2, 11, 5, 9, 2, 3, 2, 8, 9, 8, 2, -1},
        {2, 5, 10, 2, 3, 5, 3, 7, 5, -1},
        {8, 2, 0, 8, 5, 2, 8, 7, 5, 10, 2, 5, -1},
        {9, 0, 1, 5, 10, 3, 5, 3, 7, 3, 10, 2, -1},
        {9, 8, 2, 9, 2, 1, 8, 7, 2, 10, 2, 5, 7, 5, 2, -1},
        {1, 3, 5, 3, 7, 5, -1},
        {0, 8, 7, 0, 7, 1, 1, 7, 5, -1},
        {9, 0, 3, 9, 3, 5, 5, 3, 7, -1},
        {9, 8, 7, 5, 9, 7, -1},
        {5, 8, 4, 5, 10, 8, 10, 11, 8, -1},
        {5, 0, 4, 5, 11, 0, 5, 10, 11, 11, 3, 0, -1},
        {0, 1, 9, 8, 4, 10, 8, 10, 11, 10, 4, 5, -1},
        {10, 11, 4, 10, 4, 5, 11, 3, 4, 9, 4, 1, 3, 1, 4, -1},
        {2, 5, 1, 2, 8, 5, 2, 11, 8, 4, 5, 8, -1},
        {0, 4, 11, 0, 11, 3, 4, 5, 11, 2, 11, 1, 5, 1, 11, -1},
        {0, 2, 5, 0, 5, 9, 2, 11, 5, 4, 5, 8, 11, 8, 5, -1},
        {9, 4, 5, 2, 11, 3, -1},
        {2, 5, 10, 3, 5, 2, 3, 4, 5, 3, 8, 4, -1},
        {5, 10, 2, 5, 2, 4, 4, 2, 0, -1},
        {3, 10, 2, 3, 5, 10, 3, 8, 5, 4, 5, 8, 0, 1, 9, -1},
        {5, 10, 2, 5, 2, 4, 1, 9, 2, 9, 4, 2, -1},
        {8, 4, 5, 8, 5, 3, 3, 5, 1, -1},
        {0, 4, 5, 1, 0, 5, -1},
        {8, 4, 5, 8, 5, 3, 9, 0, 5, 0, 3, 5, -1},
        {9, 4, 5, -1},
        {4, 11, 7, 4, 9, 11, 9, 10, 11, -1},
        {0, 8, 3, 4, 9, 7, 9, 11, 7, 9, 10, 11, -1},
        {1, 10, 11, 1, 11, 4, 1, 4, 0, 7, 4, 11, -1},
        {3, 1, 4, 3, 4, 8, 1, 10, 4, 7, 4, 11, 10, 11, 4, -1},
        {4, 11, 7, 9, 11, 4, 9, 2, 11, 9, 1, 2, -1},
        {9, 7, 4, 9, 11, 7, 9, 1, 11, 2, 11, 1, 0, 8, 3, -1},
        {11, 7, 4, 11, 4, 2, 2, 4, 0, -1},
        {11, 7, 4, 11, 4, 2, 8, 3, 4, 3, 2, 4, -1},
        {2, 9, 10, 2, 7, 9, 2, 3, 7, 7, 4, 9, -1},
        {9, 10, 7, 9, 7, 4, 10, 2, 7, 8, 7, 0, 2, 0, 7, -1},
        {3, 7, 10, 3, 10, 2, 7, 4, 10, 1, 10, 0, 4, 0, 10, -1},
        {1, 10, 2, 8, 7, 4, -1},
        {4, 9, 1, 4, 1, 7, 7, 1, 3, -1},
        {4, 9, 1, 4, 1, 7, 0, 8, 1, 8, 7, 1, -1},
        {4, 0, 3, 7, 4, 3, -1},
        {4, 8, 7, -1},
        {9, 10, 8, 10, 11, 8, -1},
        {3, 0, 9, 3, 9, 11, 11, 9, 10, -1},
        {0, 1, 10, 0, 10, 8, 8, 10, 11, -1},
        {3, 1, 10, 11, 3, 10, -1},
        {1, 2, 11, 1, 11, 9, 9, 11, 8, -1},
        {3, 0, 9, 3, 9, 11, 1, 2, 9, 2, 11, 9, -1},
        {0, 2, 11, 8, 0, 11, -1},
        {3, 2, 11, -1},
        {2, 3, 8, 2, 8, 10, 10, 8, 9, -1},
        {9, 10, 2, 0, 9, 2, -1},
        {2, 3, 8, 2, 8, 10, 0, 1, 8, 1, 10, 8, -1},
        {1, 10, 2, -1},
        {1, 3, 8, 9, 1, 8, -1},
        {0, 9, 1, -1},
        {0, 3, 8, -1},
        {-1}
    };

    const float iso = 0.5f;
    const float cx = (width - 1) * 0.5f;
    const float cy = (height - 1) * 0.5f;
    const float cz = (z_count - 1) * 0.5f;

    auto sample = [&](size_t z, size_t y, size_t x) -> float {
        return vol[z * slice_size + y * width + x];
    };

    const int edgeToVertex[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };

    const int vertexOffset[8][3] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}
    };

    auto add_vertex = [&](float x, float y, float z, float nx, float ny, float nz) {
        size_t idx = mesh.positions.size() / 3;
        mesh.positions.push_back(x);
        mesh.positions.push_back(y);
        mesh.positions.push_back(z);
        mesh.normals.push_back(nx);
        mesh.normals.push_back(ny);
        mesh.normals.push_back(nz);

        float u = (width > 1) ? (x + cx) / (width - 1) : 0.0f;
        float v = (height > 1) ? (y + cy) / (height - 1) : 0.0f;
        mesh.uvs.push_back(u);
        mesh.uvs.push_back(1.0f - v);

        bool init = (idx == 0);
        update_minmax(mesh, x, y, z, init);
        mesh.indices.push_back(static_cast<uint32_t>(idx));
    };

    for (size_t z = 0; z + 1 < z_count; ++z) {
        for (size_t y = 0; y + 1 < height; ++y) {
            for (size_t x = 0; x + 1 < width; ++x) {
                float cube[8];
                for (int i = 0; i < 8; ++i) {
                    cube[i] = sample(z + vertexOffset[i][2],
                                     y + vertexOffset[i][1],
                                     x + vertexOffset[i][0]);
                }

                int cubeIndex = 0;
                if (cube[0] > iso) cubeIndex |= 1;
                if (cube[1] > iso) cubeIndex |= 2;
                if (cube[2] > iso) cubeIndex |= 4;
                if (cube[3] > iso) cubeIndex |= 8;
                if (cube[4] > iso) cubeIndex |= 16;
                if (cube[5] > iso) cubeIndex |= 32;
                if (cube[6] > iso) cubeIndex |= 64;
                if (cube[7] > iso) cubeIndex |= 128;

                if (edgeTable[cubeIndex] == 0) {
                    continue;
                }

                float vertList[12][3];
                for (int e = 0; e < 12; ++e) {
                    if (!(edgeTable[cubeIndex] & (1 << e))) {
                        continue;
                    }
                    int v0 = edgeToVertex[e][0];
                    int v1 = edgeToVertex[e][1];
                    float val0 = cube[v0];
                    float val1 = cube[v1];
                    float t = 0.5f;
                    if (std::fabs(val1 - val0) > 1e-6f) {
                        t = (iso - val0) / (val1 - val0);
                    }

                    float x0 = static_cast<float>(x + vertexOffset[v0][0]);
                    float y0 = static_cast<float>(y + vertexOffset[v0][1]);
                    float z0 = static_cast<float>(z + vertexOffset[v0][2]);
                    float x1 = static_cast<float>(x + vertexOffset[v1][0]);
                    float y1 = static_cast<float>(y + vertexOffset[v1][1]);
                    float z1 = static_cast<float>(z + vertexOffset[v1][2]);

                    vertList[e][0] = x0 + t * (x1 - x0) - cx;
                    vertList[e][1] = y0 + t * (y1 - y0) - cy;
                    vertList[e][2] = z0 + t * (z1 - z0) - cz;
                }

                const int *tri = triTable[cubeIndex];
                for (int i = 0; tri[i] != -1; i += 3) {
                    float ax = vertList[tri[i]][0];
                    float ay = vertList[tri[i]][1];
                    float az = vertList[tri[i]][2];
                    float bx = vertList[tri[i + 1]][0];
                    float by = vertList[tri[i + 1]][1];
                    float bz = vertList[tri[i + 1]][2];
                    float cxp = vertList[tri[i + 2]][0];
                    float cyp = vertList[tri[i + 2]][1];
                    float czp = vertList[tri[i + 2]][2];

                    float ux = bx - ax;
                    float uy = by - ay;
                    float uz = bz - az;
                    float vx = cxp - ax;
                    float vy = cyp - ay;
                    float vz = czp - az;

                    float nx = uy * vz - uz * vy;
                    float ny = uz * vx - ux * vz;
                    float nz = ux * vy - uy * vx;
                    normalize_vec3(nx, ny, nz);

                    add_vertex(ax, ay, az, nx, ny, nz);
                    add_vertex(bx, by, bz, nx, ny, nz);
                    add_vertex(cxp, cyp, czp, nx, ny, nz);
                }
            }
        }
    }

    return mesh;
}

static inline std::vector<float> build_raw_threshold_mask(const std::vector<float> &raw_volume) {
    std::vector<float> mask(raw_volume.size(), 0.0f);
    if (raw_volume.empty()) {
        return mask;
    }
    float vmin = raw_volume[0];
    float vmax = raw_volume[0];
    for (float v : raw_volume) {
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
    float thr = (vmin + vmax) * 0.5f;
    for (size_t i = 0; i < raw_volume.size(); ++i) {
        mask[i] = raw_volume[i] > thr ? 1.0f : 0.0f;
    }
    return mask;
}

static inline size_t append_aligned(std::vector<unsigned char> &buffer,
                                    const void *data,
                                    size_t bytes,
                                    size_t align = 4) {
    size_t offset = buffer.size();
    size_t padded = (offset + (align - 1)) & ~(align - 1);
    if (padded > offset) {
        buffer.resize(padded, 0);
    }
    const unsigned char *src = reinterpret_cast<const unsigned char *>(data);
    buffer.insert(buffer.end(), src, src + bytes);
    return padded;
}

static inline void write_u32(std::ofstream &out, uint32_t v) {
    out.write(reinterpret_cast<const char *>(&v), sizeof(uint32_t));
}

static inline bool write_glb(const fs::path &output_path,
                             const std::vector<PrimitiveData> &primitives,
                             const std::vector<unsigned char> &png) {
    if (primitives.empty()) {
        throw std::runtime_error("No mesh generated. Nothing to write.");
    }

    bool include_texture = false;
    for (const auto &prim : primitives) {
        if (prim.use_texture) {
            include_texture = true;
        }
    }
    if (include_texture && png.empty()) {
        throw std::runtime_error("PNG texture encode failed.");
    }

    struct BufferViewInfo {
        size_t offset = 0;
        size_t length = 0;
        int target = 0;
    };
    struct AccessorInfo {
        int buffer_view = -1;
        int component_type = 0;
        size_t count = 0;
        std::string type;
        bool has_minmax = false;
        float min[3] = {0.0f, 0.0f, 0.0f};
        float max[3] = {0.0f, 0.0f, 0.0f};
    };
    struct PrimitiveInfo {
        int pos_accessor = -1;
        int nrm_accessor = -1;
        int uv_accessor = -1;
        bool has_uv = false;
        int idx_accessor = -1;
        int material_index = -1;
    };

    std::vector<unsigned char> bin;
    std::vector<BufferViewInfo> buffer_views;
    std::vector<AccessorInfo> accessors;
    std::vector<PrimitiveInfo> prim_infos;

    for (size_t p = 0; p < primitives.size(); ++p) {
        const MeshData &mesh = primitives[p].mesh;
        if (mesh.positions.empty() || mesh.indices.empty()) {
            throw std::runtime_error("Empty mesh at primitive index " + std::to_string(p) + ".");
        }

        size_t pos_offset = append_aligned(bin, mesh.positions.data(), mesh.positions.size() * sizeof(float));
        buffer_views.push_back({pos_offset, mesh.positions.size() * sizeof(float), 34962});
        int pos_view = static_cast<int>(buffer_views.size() - 1);
        AccessorInfo pos_acc;
        pos_acc.buffer_view = pos_view;
        pos_acc.component_type = 5126;
        pos_acc.count = mesh.positions.size() / 3;
        pos_acc.type = "VEC3";
        pos_acc.has_minmax = true;
        pos_acc.min[0] = mesh.min_pos[0];
        pos_acc.min[1] = mesh.min_pos[1];
        pos_acc.min[2] = mesh.min_pos[2];
        pos_acc.max[0] = mesh.max_pos[0];
        pos_acc.max[1] = mesh.max_pos[1];
        pos_acc.max[2] = mesh.max_pos[2];
        accessors.push_back(pos_acc);
        int pos_accessor = static_cast<int>(accessors.size() - 1);

        size_t nrm_offset = append_aligned(bin, mesh.normals.data(), mesh.normals.size() * sizeof(float));
        buffer_views.push_back({nrm_offset, mesh.normals.size() * sizeof(float), 34962});
        int nrm_view = static_cast<int>(buffer_views.size() - 1);
        AccessorInfo nrm_acc;
        nrm_acc.buffer_view = nrm_view;
        nrm_acc.component_type = 5126;
        nrm_acc.count = mesh.normals.size() / 3;
        nrm_acc.type = "VEC3";
        accessors.push_back(nrm_acc);
        int nrm_accessor = static_cast<int>(accessors.size() - 1);

        int uv_accessor = -1;
        bool has_uv = false;
        if (primitives[p].use_texture) {
            size_t uv_offset = append_aligned(bin, mesh.uvs.data(), mesh.uvs.size() * sizeof(float));
            buffer_views.push_back({uv_offset, mesh.uvs.size() * sizeof(float), 34962});
            int uv_view = static_cast<int>(buffer_views.size() - 1);
            AccessorInfo uv_acc;
            uv_acc.buffer_view = uv_view;
            uv_acc.component_type = 5126;
            uv_acc.count = mesh.uvs.size() / 2;
            uv_acc.type = "VEC2";
            accessors.push_back(uv_acc);
            uv_accessor = static_cast<int>(accessors.size() - 1);
            has_uv = true;
        }

        size_t idx_offset = append_aligned(bin, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
        buffer_views.push_back({idx_offset, mesh.indices.size() * sizeof(uint32_t), 34963});
        int idx_view = static_cast<int>(buffer_views.size() - 1);
        AccessorInfo idx_acc;
        idx_acc.buffer_view = idx_view;
        idx_acc.component_type = 5125;
        idx_acc.count = mesh.indices.size();
        idx_acc.type = "SCALAR";
        accessors.push_back(idx_acc);
        int idx_accessor = static_cast<int>(accessors.size() - 1);

        PrimitiveInfo prim_info;
        prim_info.pos_accessor = pos_accessor;
        prim_info.nrm_accessor = nrm_accessor;
        prim_info.uv_accessor = uv_accessor;
        prim_info.has_uv = has_uv;
        prim_info.idx_accessor = idx_accessor;
        prim_info.material_index = static_cast<int>(p);
        prim_infos.push_back(prim_info);
    }

    int image_buffer_view = -1;
    if (include_texture) {
        size_t img_offset = append_aligned(bin, png.data(), png.size());
        buffer_views.push_back({img_offset, png.size(), 0});
        image_buffer_view = static_cast<int>(buffer_views.size() - 1);
    }

    std::ostringstream json;
    json.setf(std::ios::fixed);
    json.precision(6);

    json << "{";
    json << "\"asset\":{\"version\":\"2.0\",\"generator\":\"npz_to_glb\"},";
    json << "\"scene\":0,";
    json << "\"scenes\":[{\"nodes\":[0]}],";
    json << "\"nodes\":[{\"mesh\":0}],";
    json << "\"meshes\":[{\"primitives\":[";
    for (size_t i = 0; i < prim_infos.size(); ++i) {
        const auto &prim = prim_infos[i];
        json << "{";
        json << "\"attributes\":{\"POSITION\":" << prim.pos_accessor
             << ",\"NORMAL\":" << prim.nrm_accessor;
        if (prim.has_uv) {
            json << ",\"TEXCOORD_0\":" << prim.uv_accessor;
        }
        json << "},\"indices\":" << prim.idx_accessor
             << ",\"material\":" << prim.material_index << "}";
        if (i + 1 < prim_infos.size()) {
            json << ",";
        }
    }
    json << "]}],";

    json << "\"materials\":[";
    for (size_t i = 0; i < primitives.size(); ++i) {
        const auto &prim = primitives[i];
        json << "{";
        json << "\"pbrMetallicRoughness\":{";
        if (prim.use_texture) {
            json << "\"baseColorTexture\":{\"index\":0},";
        } else {
            json << "\"baseColorFactor\":[" << prim.base_color[0] << "," << prim.base_color[1] << "," << prim.base_color[2] << "," << prim.base_color[3] << "],";
        }
        json << "\"metallicFactor\":0.0,\"roughnessFactor\":0.9";
        json << "},\"doubleSided\":true}";
        if (i + 1 < primitives.size()) {
            json << ",";
        }
    }
    json << "],";

    if (include_texture) {
        json << "\"textures\":[{\"source\":0}],";
        json << "\"images\":[{\"bufferView\":" << image_buffer_view << ",\"mimeType\":\"image/png\"}],";
    }

    json << "\"buffers\":[{\"byteLength\":" << bin.size() << "}],";

    json << "\"bufferViews\":[";
    for (size_t i = 0; i < buffer_views.size(); ++i) {
        const auto &view = buffer_views[i];
        json << "{\"buffer\":0,\"byteOffset\":" << view.offset << ",\"byteLength\":" << view.length;
        if (view.target != 0) {
            json << ",\"target\":" << view.target;
        }
        json << "}";
        if (i + 1 < buffer_views.size()) {
            json << ",";
        }
    }
    json << "],";

    json << "\"accessors\":[";
    for (size_t i = 0; i < accessors.size(); ++i) {
        const auto &acc = accessors[i];
        json << "{\"bufferView\":" << acc.buffer_view
             << ",\"componentType\":" << acc.component_type
             << ",\"count\":" << acc.count
             << ",\"type\":\"" << acc.type << "\"";
        if (acc.has_minmax) {
            json << ",\"min\":[" << acc.min[0] << "," << acc.min[1] << "," << acc.min[2] << "]";
            json << ",\"max\":[" << acc.max[0] << "," << acc.max[1] << "," << acc.max[2] << "]";
        }
        json << "}";
        if (i + 1 < accessors.size()) {
            json << ",";
        }
    }
    json << "]";
    json << "}";

    std::string json_str = json.str();
    size_t json_len = (json_str.size() + 3) & ~static_cast<size_t>(3);
    json_str.resize(json_len, ' ');

    size_t bin_len = (bin.size() + 3) & ~static_cast<size_t>(3);
    bin.resize(bin_len, 0);

    uint32_t total_len = 12 + 8 + static_cast<uint32_t>(json_str.size()) + 8 + static_cast<uint32_t>(bin.size());

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to open output: " + output_path.string());
    }

    write_u32(out, 0x46546C67);
    write_u32(out, 2);
    write_u32(out, total_len);

    write_u32(out, static_cast<uint32_t>(json_str.size()));
    write_u32(out, 0x4E4F534A);
    out.write(json_str.data(), json_str.size());

    write_u32(out, static_cast<uint32_t>(bin.size()));
    write_u32(out, 0x004E4942);
    out.write(reinterpret_cast<const char *>(bin.data()), bin.size());

    return true;
}

static inline void convert_directory_to_glb(const fs::path &input_dir,
                                            const fs::path &output_path,
                                            const Options &opts) {
    std::vector<float> raw_volume;
    std::vector<float> ann_volume;
    size_t z_count = 0;
    size_t height = 0;
    size_t width = 0;
    bool has_ann = false;

    load_slices(input_dir, opts, raw_volume, ann_volume, z_count, height, width, has_ann);

    std::vector<PrimitiveData> primitives;
    std::vector<unsigned char> png;

    if (has_ann) {
        std::vector<float> yellow_mask(ann_volume.size(), 0.0f);
        std::vector<float> red_mask(ann_volume.size(), 0.0f);
        for (size_t i = 0; i < ann_volume.size(); ++i) {
            float v = ann_volume[i];
            if (v > 1.0f) {
                yellow_mask[i] = 1.0f;
            } else if (v > opts.ann_threshold) {
                red_mask[i] = 1.0f;
            }
        }

        MeshData yellow_mesh = build_mesh_from_scalar(yellow_mask, z_count, height, width);
        if (!yellow_mesh.positions.empty()) {
            PrimitiveData prim;
            prim.mesh = std::move(yellow_mesh);
            prim.use_texture = false;
            prim.base_color[0] = 1.0f;
            prim.base_color[1] = 0.831f;
            prim.base_color[2] = 0.0f;
            prim.base_color[3] = 1.0f;
            primitives.push_back(std::move(prim));
        }

        MeshData red_mesh = build_mesh_from_scalar(red_mask, z_count, height, width);
        if (!red_mesh.positions.empty()) {
            PrimitiveData prim;
            prim.mesh = std::move(red_mesh);
            prim.use_texture = false;
            prim.base_color[0] = 1.0f;
            prim.base_color[1] = 0.231f;
            prim.base_color[2] = 0.231f;
            prim.base_color[3] = 1.0f;
            primitives.push_back(std::move(prim));
        }

        if (primitives.empty()) {
            throw std::runtime_error("Mesh is empty. Check your annotation or threshold.");
        }
    } else {
        if (!opts.use_raw_threshold) {
            throw std::runtime_error("No annotation found. Enable raw threshold to build mesh.");
        }
        std::vector<float> raw_mask = build_raw_threshold_mask(raw_volume);
        MeshData mesh = build_mesh_from_scalar(raw_mask, z_count, height, width);
        if (mesh.positions.empty()) {
            throw std::runtime_error("Mesh is empty. Check your annotation or threshold.");
        }
        PrimitiveData prim;
        prim.mesh = std::move(mesh);
        prim.use_texture = true;
        primitives.push_back(std::move(prim));

        png = build_texture_from_raw(raw_volume, z_count, height, width);
    }

    write_glb(output_path, primitives, png);
}

} // namespace npz_to_glb
