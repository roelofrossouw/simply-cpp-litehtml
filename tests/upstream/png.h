#pragma once

// PNG files read and written with zlib alone, for the render tests: 8-bit RGB pixels out, from
// grey, grey + alpha, RGB, RGBA and palette images of any bit depth, not interlaced (alpha
// composed over white, as the references are opaque).

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace render_test {
    struct pixels {
        int width = 0, height = 0;
        std::vector<std::uint8_t> rgb; // width * height * 3

        [[nodiscard]] std::uint8_t *at(const int x, const int y) { return &rgb[(static_cast<std::size_t>(y) * width + x) * 3]; }
        [[nodiscard]] const std::uint8_t *at(const int x, const int y) const { return &rgb[(static_cast<std::size_t>(y) * width + x) * 3]; }
    };

    // An image with its alpha, for images in documents.
    struct picture {
        int width = 0, height = 0;
        std::vector<std::uint8_t> rgba; // width * height * 4
    };

    namespace detail {
        inline std::uint32_t big_endian(const std::string &bytes, const std::size_t at) {
            return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 3]));
        }

        inline int paeth(const int a, const int b, const int c) {
            const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            if (pa <= pb && pa <= pc) return a;
            return pb <= pc ? b : c;
        }
    }

    // A PNG's pixels with their alpha; nothing for what this can't read (interlaced, broken).
    inline std::optional<picture> decode_png(const std::string &bytes) {
        using detail::big_endian;
        if (bytes.size() < 8 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0) return std::nullopt;
        int width = 0, height = 0, depth = 0, type = 0, interlace = 0;
        std::string data, palette, transparency;
        for (std::size_t at = 8; at + 12 <= bytes.size();) {
            const auto length = big_endian(bytes, at);
            if (at + 12 + length > bytes.size()) return std::nullopt;
            const std::string kind = bytes.substr(at + 4, 4);
            const std::string body = bytes.substr(at + 8, length);
            if (kind == "IHDR" && body.size() >= 13) {
                width = static_cast<int>(big_endian(body, 0));
                height = static_cast<int>(big_endian(body, 4));
                depth = static_cast<std::uint8_t>(body[8]);
                type = static_cast<std::uint8_t>(body[9]);
                interlace = static_cast<std::uint8_t>(body[12]);
            } else if (kind == "PLTE") palette = body;
            else if (kind == "tRNS") transparency = body;
            else if (kind == "IDAT") data += body;
            else if (kind == "IEND") break;
            at += 12 + length;
        }
        if (width <= 0 || height <= 0 || interlace != 0) return std::nullopt;
        const int channels = type == 0 ? 1 : type == 2 ? 3 : type == 3 ? 1 : type == 4 ? 2 : type == 6 ? 4 : 0;
        if (channels == 0) return std::nullopt;
        const std::size_t bits_per_pixel = static_cast<std::size_t>(channels) * depth;
        const std::size_t stride = (static_cast<std::size_t>(width) * bits_per_pixel + 7) / 8;
        const std::size_t step = std::max<std::size_t>(1, bits_per_pixel / 8); // the filters' byte distance

        std::vector<std::uint8_t> raw((stride + 1) * height);
        uLongf size = raw.size();
        if (uncompress(raw.data(), &size, reinterpret_cast<const Bytef *>(data.data()), data.size()) != Z_OK || size != raw.size())
            return std::nullopt;

        std::vector<std::uint8_t> rows(stride * height), previous(stride, 0);
        for (int y = 0; y < height; ++y) {
            const std::uint8_t filter = raw[y * (stride + 1)];
            const std::uint8_t *in = &raw[y * (stride + 1) + 1];
            std::uint8_t *out = &rows[y * stride];
            for (std::size_t i = 0; i < stride; ++i) {
                const int a = i >= step ? out[i - step] : 0, b = previous[i], c = i >= step ? previous[i - step] : 0;
                int value = in[i];
                switch (filter) {
                    case 1: value += a; break;
                    case 2: value += b; break;
                    case 3: value += (a + b) / 2; break;
                    case 4: value += detail::paeth(a, b, c); break;
                    default: break;
                }
                out[i] = static_cast<std::uint8_t>(value);
            }
            std::copy(out, out + stride, previous.begin());
        }

        // A sample, scaled to 8 bits.
        const auto sample = [&](const int y, const std::size_t index) -> int {
            const std::uint8_t *row = &rows[y * stride];
            if (depth == 8) return row[index];
            if (depth == 16) return row[index * 2];
            const std::size_t bit = index * depth;
            const int value = (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & ((1 << depth) - 1);
            return type == 3 ? value : value * 255 / ((1 << depth) - 1);
        };
        picture out{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                std::uint8_t *p = &out.rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                const std::size_t first = static_cast<std::size_t>(x) * channels;
                int r, g, b, alpha = 255;
                if (type == 3) {
                    const int index = sample(y, first);
                    if (static_cast<std::size_t>(index) * 3 + 2 >= palette.size()) return std::nullopt;
                    r = static_cast<std::uint8_t>(palette[index * 3]);
                    g = static_cast<std::uint8_t>(palette[index * 3 + 1]);
                    b = static_cast<std::uint8_t>(palette[index * 3 + 2]);
                    if (static_cast<std::size_t>(index) < transparency.size()) alpha = static_cast<std::uint8_t>(transparency[index]);
                } else if (type == 0 || type == 4) {
                    r = g = b = sample(y, first);
                    if (type == 4) alpha = sample(y, first + 1);
                } else {
                    r = sample(y, first);
                    g = sample(y, first + 1);
                    b = sample(y, first + 2);
                    if (type == 6) alpha = sample(y, first + 3);
                }
                p[0] = static_cast<std::uint8_t>(r);
                p[1] = static_cast<std::uint8_t>(g);
                p[2] = static_cast<std::uint8_t>(b);
                p[3] = static_cast<std::uint8_t>(alpha);
            }
        return out;
    }

    inline std::optional<pixels> load_png(const std::filesystem::path &file) {
        std::ifstream in{file, std::ios::binary};
        if (!in) return std::nullopt;
        const auto image = decode_png({std::istreambuf_iterator<char>{in}, {}});
        if (!image) return std::nullopt;
        pixels out{image->width, image->height, std::vector<std::uint8_t>(static_cast<std::size_t>(image->width) * image->height * 3)};
        for (std::size_t i = 0; i < static_cast<std::size_t>(image->width) * image->height; ++i) {
            const int alpha = image->rgba[i * 4 + 3];
            for (int c = 0; c < 3; ++c) out.rgb[i * 3 + c] = static_cast<std::uint8_t>((image->rgba[i * 4 + c] * alpha + 255 * (255 - alpha) + 127) / 255);
        }
        return out;
    }

    // Writes pixels as an RGB PNG, to look at what a failed test drew.
    inline bool save_png(const pixels &image, const std::filesystem::path &file) {
        const auto chunk = [](std::string &out, const char *kind, const std::string &body) {
            const auto length = static_cast<std::uint32_t>(body.size());
            for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((length >> shift) & 0xFF);
            const std::string typed = std::string{kind} + body;
            out += typed;
            const auto crc = static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(typed.data()), static_cast<uInt>(typed.size())));
            for (int shift = 24; shift >= 0; shift -= 8) out += static_cast<char>((crc >> shift) & 0xFF);
        };
        std::string header;
        for (const int value: {image.width, image.height})
            for (int shift = 24; shift >= 0; shift -= 8) header += static_cast<char>((value >> shift) & 0xFF);
        header += std::string{"\x08\x02\x00\x00\x00", 5};
        std::string raw;
        for (int y = 0; y < image.height; ++y) {
            raw += '\0';
            raw.append(reinterpret_cast<const char *>(image.at(0, y)), static_cast<std::size_t>(image.width) * 3);
        }
        uLongf size = compressBound(static_cast<uLong>(raw.size()));
        std::string packed(size, '\0');
        if (compress(reinterpret_cast<Bytef *>(packed.data()), &size, reinterpret_cast<const Bytef *>(raw.data()), raw.size()) != Z_OK) return false;
        packed.resize(size);
        std::string out = "\x89PNG\r\n\x1a\n";
        chunk(out, "IHDR", header);
        chunk(out, "IDAT", packed);
        chunk(out, "IEND", "");
        std::ofstream{file, std::ios::binary} << out;
        return true;
    }
}
