#pragma once

// An sc::drawing drawn into pixels, for the render tests: shapes filled with antialiased coverage
// (exact across a row, 4 rows a pixel), by their fill rule, in colours or linear gradients, through
// their clips; images placed by their transforms; and text in the Ahem font as the boxes its glyphs
// are. Other text isn't drawn: where it would be is masked(), to leave out of the comparison (its
// glyphs, shaped and drawn by pango in the references, can't be matched without pango).

#include "png.h"

#include <drawing.h>
#include <font.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace render_test {
    // What kept a document from being drawn as its reference was: nothing, or an image that isn't
    // a PNG this reads.
    enum class unsupported { none, image };

    class raster {
    public:
        raster(const int width, const int height) : image_{width, height, std::vector<std::uint8_t>()}, colour_(static_cast<std::size_t>(width) * height * 3, 1.0f) {}

        [[nodiscard]] unsupported what() const { return unsupported_; }

        // Where text in fonts other than Ahem is: its line box, with room for another shaping's
        // glyphs (a little wider, a pixel or two taller).
        [[nodiscard]] const std::vector<sc::rect> &masked() const { return masked_; }

        void draw(const sc::drawing &picture) {
            for (const auto &item: picture.items()) std::visit([&](const auto &each) { draw(each); }, item);
        }

        // The pixels, 8 bits a channel.
        [[nodiscard]] pixels result() const {
            pixels out = image_;
            out.rgb.resize(colour_.size());
            for (std::size_t i = 0; i < colour_.size(); ++i) out.rgb[i] = static_cast<std::uint8_t>(std::lround(std::clamp(colour_[i], 0.0f, 1.0f) * 255));
            return out;
        }

    private:
        // How much of each pixel in a box a shape covers, 0 to 1.
        struct coverage {
            int left = 0, top = 0, width = 0, height = 0;
            std::vector<float> amount;

            [[nodiscard]] float at(const int x, const int y) const {
                if (x < left || y < top || x >= left + width || y >= top + height) return 0;
                return amount[static_cast<std::size_t>(y - top) * width + (x - left)];
            }
        };

        struct edge {
            double x0, y0, x1, y1;
        };

        pixels image_;
        std::vector<float> colour_; // RGB, 0 to 1, over white
        unsupported unsupported_ = unsupported::none;
        std::vector<sc::rect> masked_;

        void mask(const sc::label &item) {
            const sc::rect box = item.bounds();
            const double wider = 0.1 * box.width() + 3;
            masked_.push_back({box.left() - wider, box.top() - 2, box.width() + 2 * wider, box.height() + 4});
        }

        static constexpr int rows_per_pixel = 4;

        static std::vector<edge> edges_of(const sc::path &shape) {
            std::vector<edge> out;
            for (const auto &outline: shape.flattened(0.05)) {
                const auto &points = outline.points;
                for (std::size_t i = 0; i < points.size(); ++i) { // open outlines fill as if closed
                    const auto &a = points[i], &b = points[(i + 1) % points.size()];
                    if (a.y() != b.y()) out.push_back({a.x(), a.y(), b.x(), b.y()});
                }
            }
            return out;
        }

        [[nodiscard]] coverage cover(const std::vector<edge> &edges, const sc::fill_rule rule) const {
            coverage out;
            if (edges.empty()) return out;
            double top = edges[0].y0, bottom = top, left = edges[0].x0, right = left;
            for (const auto &e: edges) {
                top = std::min({top, e.y0, e.y1});
                bottom = std::max({bottom, e.y0, e.y1});
                left = std::min({left, e.x0, e.x1});
                right = std::max({right, e.x0, e.x1});
            }
            out.left = std::max(0, static_cast<int>(std::floor(left)));
            out.top = std::max(0, static_cast<int>(std::floor(top)));
            const int last_x = std::min(image_.width, static_cast<int>(std::ceil(right))), last_y = std::min(image_.height, static_cast<int>(std::ceil(bottom)));
            out.width = std::max(0, last_x - out.left);
            out.height = std::max(0, last_y - out.top);
            out.amount.assign(static_cast<std::size_t>(out.width) * out.height, 0.0f);
            std::vector<std::pair<double, int> > crossings;
            for (int y = out.top; y < out.top + out.height; ++y)
                for (int row = 0; row < rows_per_pixel; ++row) {
                    const double at = y + (row + 0.5) / rows_per_pixel;
                    crossings.clear();
                    for (const auto &e: edges) {
                        const double low = std::min(e.y0, e.y1), high = std::max(e.y0, e.y1);
                        if (at < low || at >= high) continue;
                        crossings.emplace_back(e.x0 + (at - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.y1 > e.y0 ? 1 : -1);
                    }
                    std::ranges::sort(crossings);
                    int winding = 0;
                    for (std::size_t i = 0; i + 1 < crossings.size(); ++i) {
                        winding += crossings[i].second;
                        const bool inside = rule == sc::fill_rule::even_odd ? (winding % 2) != 0 : winding != 0;
                        if (!inside) continue;
                        const double from = std::max(crossings[i].first, static_cast<double>(out.left));
                        const double to = std::min(crossings[i + 1].first, static_cast<double>(out.left + out.width));
                        for (int x = static_cast<int>(std::floor(from)); x < to; ++x) {
                            const double part = std::min<double>(x + 1, to) - std::max<double>(x, from);
                            if (part > 0) out.amount[static_cast<std::size_t>(y - out.top) * out.width + (x - out.left)] += static_cast<float>(part / rows_per_pixel);
                        }
                    }
                }
            return out;
        }

        [[nodiscard]] std::optional<coverage> clip_of(const std::optional<sc::path> &clip) const {
            if (!clip) return std::nullopt;
            return cover(edges_of(*clip), sc::fill_rule::nonzero);
        }

        void blend(const int x, const int y, const sc::color &colour, const float amount) {
            const float alpha = static_cast<float>(colour.alpha()) * std::min(amount, 1.0f);
            if (alpha <= 0) return;
            float *p = &colour_[(static_cast<std::size_t>(y) * image_.width + x) * 3];
            p[0] = p[0] * (1 - alpha) + static_cast<float>(colour.red()) * alpha;
            p[1] = p[1] * (1 - alpha) + static_cast<float>(colour.green()) * alpha;
            p[2] = p[2] * (1 - alpha) + static_cast<float>(colour.blue()) * alpha;
        }

        void paint(const coverage &shape, const sc::fill &fill, const std::optional<coverage> &clip) {
            for (int y = shape.top; y < shape.top + shape.height; ++y)
                for (int x = shape.left; x < shape.left + shape.width; ++x) {
                    float amount = shape.at(x, y);
                    if (clip) amount *= std::min(clip->at(x, y), 1.0f);
                    if (amount <= 0) continue;
                    const sc::color colour = fill.gradient() ? fill.gradient()->colour_at(fill.gradient()->offset_of({x + 0.5, y + 0.5})) : fill.colour();
                    blend(x, y, colour, amount);
                }
        }

        // A stroke as one shape: each segment a rectangle as wide as the stroke, reaching half
        // its width past both ends (square corners where segments meet at right angles).
        static sc::path outline_of(const sc::path &shape, const double width) {
            sc::path out;
            const double half = width / 2;
            for (const auto &outline: shape.flattened(0.05)) {
                const auto &points = outline.points;
                const std::size_t segments = outline.closed ? points.size() : points.size() - 1;
                for (std::size_t i = 0; i < segments; ++i) {
                    const auto &a = points[i], &b = points[(i + 1) % points.size()];
                    const double dx = b.x() - a.x(), dy = b.y() - a.y(), length = std::hypot(dx, dy);
                    if (length <= 0) continue;
                    const double ux = dx / length * half, uy = dy / length * half; // along, half the width
                    const double nx = -uy, ny = ux;                                // across
                    out.move_to({a.x() - ux + nx, a.y() - uy + ny})
                            .line_to({b.x() + ux + nx, b.y() + uy + ny})
                            .line_to({b.x() + ux - nx, b.y() + uy - ny})
                            .line_to({a.x() - ux - nx, a.y() - uy - ny})
                            .close();
                }
            }
            return out;
        }

        void draw(const sc::drawable &item) {
            const auto clip = clip_of(item.clip);
            const sc::path shape = to_path(item.shape);
            if (item.appearance.fill().visible()) paint(cover(edges_of(shape), item.appearance.fill().rule()), item.appearance.fill(), clip);
            if (item.appearance.stroke().visible())
                paint(cover(edges_of(outline_of(shape, item.appearance.stroke().width())), sc::fill_rule::nonzero), sc::fill{item.appearance.stroke().colour()}, clip);
        }

        // Ahem's glyphs: most are a box an em square from its ascent (0.8 em above the baseline)
        // to its descent (0.2 em below); 'p' only the part below the baseline, 'É' only the part
        // above; the spaces nothing.
        void draw(const sc::label &item) {
            const auto &text = item.text;
            const bool blank = std::ranges::all_of(text.content, [](const char c) { return c == ' ' || c == '\t' || c == '\n'; });
            std::string family = text.font.family;
            std::ranges::transform(family, family.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (family != "ahem") {
                if (!blank) mask(item);
                return;
            }
            sc::path boxes;
            const double size = text.font.size;
            double x = text.position.x();
            const double baseline = text.position.y();
            for (std::size_t i = 0; i < text.content.size();) {
                const auto first = static_cast<unsigned char>(text.content[i]);
                const std::size_t length = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
                const std::string glyph = text.content.substr(i, length);
                i += length;
                char32_t code = first;
                if (length > 1) {
                    code = first & (0xFF >> (length + 1));
                    for (std::size_t k = 1; k < glyph.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(glyph[k]) & 0x3F);
                }
                const double advance = text.font.width(glyph);
                const bool space = code == U' ' || code == 0xA0 || (code >= 0x2002 && code <= 0x200D) || code == 0x3000 || code == 0xFEFF;
                if (code > 0x7E && code != 0xC9 && !space) { // not one of Ahem's: another font's glyph
                    mask(item);
                    return;
                }
                if (!space) {
                    const double top = code == U'p' ? baseline : baseline - 0.8 * size;
                    const double bottom = code == 0xC9 ? baseline : baseline + 0.2 * size;
                    boxes.append(sc::to_path(sc::rect{x, top, advance, bottom - top}));
                }
                x += advance;
            }
            paint(cover(edges_of(boxes), sc::fill_rule::nonzero), item.fill, clip_of(item.clip));
        }

        void draw(const sc::figure &item) {
            const auto decoded = item.bitmap.type() == sc::bitmap::format::png ? decode_png(item.bitmap.bytes()) : std::nullopt;
            if (!decoded) {
                unsupported_ = unsupported::image;
                return;
            }
            const auto clip = clip_of(item.clip);
            const sc::rect area = item.bounds();
            const sc::transform back = item.placement.inverse();
            const int x0 = std::max(0, static_cast<int>(std::floor(area.left()))), y0 = std::max(0, static_cast<int>(std::floor(area.top())));
            const int x1 = std::min(image_.width, static_cast<int>(std::ceil(area.right()))), y1 = std::min(image_.height, static_cast<int>(std::ceil(area.bottom())));
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const sc::point unit = back.apply(sc::point{x + 0.5, y + 0.5});
                    if (unit.x() < 0 || unit.y() < 0 || unit.x() >= 1 || unit.y() >= 1) continue;
                    const int sx = std::min(decoded->width - 1, static_cast<int>(unit.x() * decoded->width));
                    const int sy = std::min(decoded->height - 1, static_cast<int>(unit.y() * decoded->height));
                    const std::uint8_t *p = &decoded->rgba[(static_cast<std::size_t>(sy) * decoded->width + sx) * 4];
                    const float amount = clip ? std::min(clip->at(x, y), 1.0f) : 1.0f;
                    blend(x, y, sc::color{p[0] / 255.0, p[1] / 255.0, p[2] / 255.0, p[3] / 255.0}, amount);
                }
        }
    };
}
