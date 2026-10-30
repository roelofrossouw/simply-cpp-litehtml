#include "container.h"

#include <base64.h>
#include <core.h>
#include <paint.h>
#include <path.h>
#include <sc_qrencode.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace sc::web::detail {
    namespace {
        sc::color colour_of(const litehtml::web_color &c) { return {c.red / 255.0, c.green / 255.0, c.blue / 255.0, c.alpha / 255.0}; }

        rect rect_of(const litehtml::position &p) { return {p.x, p.y, p.width, p.height}; }

        // The horizontal and vertical radius of each corner, from the top left, clockwise.
        using radii = std::array<std::pair<double, double>, 4>;

        radii radii_of(const litehtml::border_radiuses &r) {
            return {{{r.top_left_x, r.top_left_y}, {r.top_right_x, r.top_right_y}, {r.bottom_right_x, r.bottom_right_y}, {r.bottom_left_x, r.bottom_left_y}}};
        }

        bool rounded(const radii &corners) {
            return std::ranges::any_of(corners, [](const auto &corner) { return corner.first > 0 && corner.second > 0; });
        }

        // A box with elliptical corners (each scaled down, as CSS does, when two would overlap),
        // clockwise from the top left when forward, anticlockwise when not; nonzero fills of an
        // outer box and an inner one the other way make a ring.
        path box_path(const rect &box, radii corners, const bool forward = true) {
            if (box.width() <= 0 || box.height() <= 0) return {};
            double scale = 1;
            const auto fit = [&](const double a, const double b, const double length) {
                if (a + b > length && a + b > 0) scale = std::min(scale, length / (a + b));
            };
            fit(corners[0].first, corners[1].first, box.width());
            fit(corners[3].first, corners[2].first, box.width());
            fit(corners[0].second, corners[3].second, box.height());
            fit(corners[1].second, corners[2].second, box.height());
            for (auto &[x, y]: corners) {
                x = std::max(0.0, x * scale);
                y = std::max(0.0, y * scale);
                if (x == 0 || y == 0) x = y = 0;
            }
            constexpr double k = quarter_circle;
            const double l = box.left(), t = box.top(), r = box.right(), b = box.bottom();
            const auto &[tlx, tly] = corners[0];
            const auto &[trx, try_] = corners[1];
            const auto &[brx, bry] = corners[2];
            const auto &[blx, bly] = corners[3];
            path out;
            if (forward) {
                out.move_to({l + tlx, t}).line_to({r - trx, t});
                if (trx > 0) out.cubic_to({r - trx * (1 - k), t}, {r, t + try_ * (1 - k)}, {r, t + try_});
                out.line_to({r, b - bry});
                if (brx > 0) out.cubic_to({r, b - bry * (1 - k)}, {r - brx * (1 - k), b}, {r - brx, b});
                out.line_to({l + blx, b});
                if (blx > 0) out.cubic_to({l + blx * (1 - k), b}, {l, b - bly * (1 - k)}, {l, b - bly});
                out.line_to({l, t + tly});
                if (tlx > 0) out.cubic_to({l, t + tly * (1 - k)}, {l + tlx * (1 - k), t}, {l + tlx, t});
            } else {
                out.move_to({l + tlx, t});
                if (tlx > 0) out.cubic_to({l + tlx * (1 - k), t}, {l, t + tly * (1 - k)}, {l, t + tly});
                out.line_to({l, b - bly});
                if (blx > 0) out.cubic_to({l, b - bly * (1 - k)}, {l + blx * (1 - k), b}, {l + blx, b});
                out.line_to({r - brx, b});
                if (brx > 0) out.cubic_to({r - brx * (1 - k), b}, {r, b - bry * (1 - k)}, {r, b - bry});
                out.line_to({r, t + try_});
                if (trx > 0) out.cubic_to({r, t + try_ * (1 - k)}, {r - trx * (1 - k), t}, {r - trx, t});
                out.line_to({l + tlx, t});
            }
            return out.close();
        }

        path rect_path(const rect &box) { return box_path(box, {}); }

        // The inside of a box, widths (left, top, right, bottom) in from its edges.
        rect inset(const rect &box, const std::array<double, 4> &widths) {
            return {box.left() + widths[0], box.top() + widths[1], std::max(0.0, box.width() - widths[0] - widths[2]),
                    std::max(0.0, box.height() - widths[1] - widths[3])};
        }

        // The corners' radii widths in, as an inner edge's are.
        radii inset(radii corners, const std::array<double, 4> &widths) {
            const double left = widths[0], top = widths[1], right = widths[2], bottom = widths[3];
            corners[0] = {corners[0].first - left, corners[0].second - top};
            corners[1] = {corners[1].first - right, corners[1].second - top};
            corners[2] = {corners[2].first - right, corners[2].second - bottom};
            corners[3] = {corners[3].first - left, corners[3].second - bottom};
            return corners;
        }

        struct family {
            std::string name, lower;
        };

        // A family list ("Roboto, 'Helvetica Neue', sans-serif") as its names, unquoted.
        std::vector<family> families(const std::string &list) {
            std::vector<family> out;
            std::size_t start = 0;
            while (start <= list.size()) {
                auto end = list.find(',', start);
                if (end == std::string::npos) end = list.size();
                const std::string name = list.substr(start, end - start);
                start = end + 1;
                const auto first = name.find_first_not_of(" \t\"'"), last = name.find_last_not_of(" \t\"'");
                if (first == std::string::npos) continue;
                auto trimmed = name.substr(first, last - first + 1);
                auto lower = sc::lowercase(trimmed);
                out.push_back({std::move(trimmed), std::move(lower)});
            }
            return out;
        }

        // The first installed family in a list, or a generic one, or fallback.
        std::string family_of(const std::string &list, const std::string &fallback) {
            std::string generic;
            for (const auto &[name, lower]: families(list)) {
                if (lower == "serif" || lower == "sans-serif" || lower == "monospace" || lower == "cursive" || lower == "fantasy") {
                    if (generic.empty()) generic = lower;
                    continue;
                }
                if (lower == "system-ui" || lower == "-apple-system" || lower == "ui-sans-serif") {
                    if (generic.empty()) generic = "sans-serif";
                    continue;
                }
                if (sc::font{name}.available()) return name;
            }
            return generic.empty() ? fallback : generic;
        }

        // With built-in fonts: a family list as Helvetica, Times or Courier, or a family of its own
        // that's installed (embedded then); Helvetica when nothing in it is either.
        std::string builtin_family_of(const std::string &list) {
            static const std::map<std::string, std::string> builtin{
                {"helvetica", "Helvetica"}, {"arial", "Helvetica"}, {"liberation sans", "Helvetica"}, {"arimo", "Helvetica"},
                {"nimbus sans", "Helvetica"}, {"sans-serif", "Helvetica"}, {"system-ui", "Helvetica"}, {"-apple-system", "Helvetica"},
                {"ui-sans-serif", "Helvetica"}, {"cursive", "Helvetica"}, {"fantasy", "Helvetica"},
                {"times", "Times"}, {"times new roman", "Times"}, {"times-roman", "Times"}, {"liberation serif", "Times"},
                {"tinos", "Times"}, {"nimbus roman", "Times"}, {"serif", "Times"}, {"ui-serif", "Times"},
                {"courier", "Courier"}, {"courier new", "Courier"}, {"liberation mono", "Courier"}, {"cousine", "Courier"},
                {"nimbus mono ps", "Courier"}, {"monospace", "Courier"}, {"ui-monospace", "Courier"}};
            for (const auto &[name, lower]: families(list)) {
                if (const auto found = builtin.find(lower); found != builtin.end()) return found->second;
                if (sc::font{name}.available()) return name;
            }
            return "Helvetica";
        }

        // Pushes and pops a canvas clip around what's drawn while it lives.
        class clip_scope {
        public:
            clip_scope(sc::canvas &canvas, const path &shape) : canvas_(canvas), on_(!shape.empty()) {
                if (on_) canvas_.push_clip(shape);
            }

            ~clip_scope() {
                if (on_) canvas_.pop_clip();
            }

            clip_scope(const clip_scope &) = delete;
            clip_scope &operator=(const clip_scope &) = delete;

        private:
            sc::canvas &canvas_;
            bool on_;
        };

        // Where a background shows: its clip box within its border box.
        rect visible_of(const litehtml::background_layer &layer) { return rect_of(layer.clip_box).intersect(rect_of(layer.border_box)); }

        bool inside(const rect &box, const rect &area) {
            return box.left() >= area.left() - 1e-6 && box.top() >= area.top() - 1e-6 && box.right() <= area.right() + 1e-6 && box.bottom() <= area.bottom() + 1e-6;
        }

        // Where a background's tiles go: its origin box, repeated as it says, those that show.
        std::vector<rect> tiles(const litehtml::background_layer &layer) {
            const rect origin = rect_of(layer.origin_box), visible = visible_of(layer);
            if (origin.width() <= 0 || origin.height() <= 0 || visible.width() <= 0 || visible.height() <= 0) return {};
            const bool across = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_x;
            const bool down = layer.repeat == litehtml::background_repeat_repeat || layer.repeat == litehtml::background_repeat_repeat_y;
            double x0 = origin.left(), y0 = origin.top(), x1 = origin.right(), y1 = origin.bottom();
            if (across) {
                x0 -= std::ceil((x0 - visible.left()) / origin.width() - 1e-9) * origin.width();
                x1 = visible.right();
            }
            if (down) {
                y0 -= std::ceil((y0 - visible.top()) / origin.height() - 1e-9) * origin.height();
                y1 = visible.bottom();
            }
            std::vector<rect> out;
            for (double y = y0; y < y1 - 1e-6; y += origin.height())
                for (double x = x0; x < x1 - 1e-6; x += origin.width()) {
                    const rect tile{x, y, origin.width(), origin.height()};
                    if (tile.right() > visible.left() && tile.left() < visible.right() && tile.bottom() > visible.top() && tile.top() < visible.bottom())
                        out.push_back(tile);
                }
            return out;
        }

        // The one clip a background needs: its rounded border box, or else where it shows when a
        // tile reaches past it.
        path background_clip(const litehtml::background_layer &layer, const std::vector<rect> &placed) {
            const radii corners = radii_of(layer.border_radius);
            if (rounded(corners)) return box_path(rect_of(layer.border_box), corners);
            const rect visible = visible_of(layer);
            if (std::ranges::all_of(placed, [&](const rect &tile) { return inside(tile, visible); })) return {};
            return rect_path(visible);
        }

        bool is_svg(const std::string &bytes) {
            const auto start = bytes.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
            if (start == std::string::npos) return false;
            const auto head = std::string_view{bytes}.substr(start, 512);
            return head.starts_with("<svg") || ((head.starts_with("<?xml") || head.starts_with("<!")) && head.find("<svg") != std::string_view::npos);
        }

        void draw_picture(sc::canvas &canvas, const picture &image, const rect &at) {
            if (image.raster) canvas.draw(*image.raster, at);
            else canvas.draw(*image.vector, at);
        }
    }

    // qr:[settings][;base64],data, as a data: URI is written: the data's QR code as a drawing, 4 px
    // a module. Settings: correction=L, M (the default), Q or H (or low ... high) and margin=N, the
    // quiet zone in modules (4 by default). Without a comma, everything after "qr:" is the data.
    // Throws std::invalid_argument for a setting it doesn't know, std::length_error for data too
    // long for a QR code, std::runtime_error for base64 that isn't.
    picture container::qr_picture(const std::string_view url) {
        auto rest = url.substr(3);
        std::string_view header;
        if (const auto comma = rest.find(','); comma != std::string_view::npos) {
            header = rest.substr(0, comma);
            rest = rest.substr(comma + 1);
        }
        auto level = sc::qr::correction::medium;
        int margin = 4;
        bool base64 = false;
        std::size_t start = 0;
        while (start <= header.size()) {
            auto end = header.find(';', start);
            if (end == std::string_view::npos) end = header.size();
            auto setting = std::string{header.substr(start, end - start)};
            start = end + 1;
            std::ranges::transform(setting, setting.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (setting.empty()) continue;
            if (setting == "base64") {
                base64 = true;
            } else if (setting.starts_with("correction=")) {
                static const std::map<std::string, sc::qr::correction> levels{
                        {"l", sc::qr::correction::low},          {"low", sc::qr::correction::low},     {"m", sc::qr::correction::medium},
                        {"medium", sc::qr::correction::medium}, {"q", sc::qr::correction::quartile}, {"quartile", sc::qr::correction::quartile},
                        {"h", sc::qr::correction::high},        {"high", sc::qr::correction::high}};
                const auto found = levels.find(setting.substr(11));
                if (found == levels.end()) throw std::invalid_argument("correction is L, M, Q or H");
                level = found->second;
            } else if (setting.starts_with("margin=")) {
                const auto text = setting.substr(7);
                const auto [end_of, error] = std::from_chars(text.data(), text.data() + text.size(), margin);
                if (error != std::errc{} || end_of != text.data() + text.size() || margin < 0 || margin > 100)
                    throw std::invalid_argument("margin is 0 to 100 modules");
            } else {
                throw std::invalid_argument("unknown setting '" + setting + "' (correction, margin, base64)");
            }
        }
        std::string data;
        if (base64) {
            data = sc::base64::decode(rest);
        } else { // percent-decoded, as a URL's text
            for (std::size_t i = 0; i < rest.size(); ++i) {
                const auto hex = [](const char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
                if (rest[i] == '%' && i + 2 < rest.size() && hex(rest[i + 1]) && hex(rest[i + 2])) {
                    data += static_cast<char>(std::stoi(std::string{rest.substr(i + 1, 2)}, nullptr, 16));
                    i += 2;
                } else {
                    data += rest[i];
                }
            }
        }
        if (data.empty()) throw std::invalid_argument("no data to encode");
        const auto code = data.find('\0') == std::string::npos ? sc::qr::code{data, level} : sc::qr::code::binary(data, level);
        auto drawing = sc::drawing::from_svg(code.svg(std::nullopt, margin));
        const double side = 4.0 * (code.size() + 2 * margin);
        return picture{std::nullopt, std::move(drawing), sc::size{side, side}};
    }

    container::container(html::options settings)
        : settings_(std::move(settings)), base_url_(settings_.base_url),
          pictures_(settings_.images ? settings_.images->store_ : std::make_shared<picture_store>()) {
        if (!settings_.fetch) settings_.fetch = html::default_fetch;
    }

    std::string container::resolved(const char *src, const char *baseurl) const {
        const std::string base = baseurl && *baseurl ? std::string{baseurl} : base_url_;
        // A file on this machine's disk, as server-side code gives them ("/var/www/logo.png"): read
        // from there even under a web base, which would make it a page on that site.
        const std::string path{src ? src : ""};
        if (path.starts_with('/') && !path.starts_with("//") && (base.starts_with("http://") || base.starts_with("https://"))) {
            std::error_code ignored;
            if (std::filesystem::is_regular_file(path, ignored)) return path;
        }
        return html::resolve(path, base);
    }

    const picture *container::find(const std::string &url) const {
        const auto found = pictures_->pictures.find(url);
        return found != pictures_->pictures.end() && found->second ? &*found->second : nullptr;
    }


    litehtml::uint_ptr container::create_font(const litehtml::font_description &description, const litehtml::document *, litehtml::font_metrics *metrics) {
        font_entry entry;
        const auto family = settings_.builtin_fonts ? builtin_family_of(description.family) : family_of(description.family, settings_.font.family);
        entry.font = sc::font{family, description.size, description.weight, description.style == litehtml::font_style_italic};
        if (settings_.builtin_fonts) entry.standard = sc::standard_font::of(entry.font);
        entry.decoration = description.decoration_line;
        entry.decoration_colour = description.decoration_color;
        const auto &font = entry.font;
        const double size = font.size;
        // Estimated from the size when no font is installed at all.
        entry.ascent = font.ascender() > 0 ? font.ascender() : size * 0.8;
        entry.descent = font.ascender() > 0 ? font.descender() : size * 0.2;
        entry.x_height = font.x_height() > 0 ? font.x_height() : size * 0.5;
        if (entry.standard) { // Adobe's metrics, whatever is installed
            entry.ascent = entry.standard->ascender(size);
            entry.descent = entry.standard->descender(size);
            entry.x_height = entry.standard->x_height(size);
        }
        if (metrics) {
            metrics->font_size = px(size);
            metrics->ascent = px(entry.ascent);
            metrics->descent = px(entry.descent);
            // A built-in font's normal line height is 1.15 of its size, as browsers give Arial,
            // Times New Roman and Courier New (the same metrics).
            const double line = entry.standard ? size * 1.15 : font.line_height();
            metrics->height = px(line > 0 ? std::max(line, entry.ascent + entry.descent) : entry.ascent + entry.descent);
            metrics->x_height = px(entry.x_height);
            const double zero = entry.standard ? entry.standard->width("0", size) : font.width("0");
            metrics->ch_width = px(zero > 0 ? zero : size * 0.5);
            metrics->draw_spaces = false; // words are placed one by one; spaces only move them
            metrics->sub_shift = px(size / 5);
            metrics->super_shift = px(size / 3);
        }
        fonts_.emplace_back(std::move(entry));
        return static_cast<litehtml::uint_ptr>(fonts_.size());
    }

    // A built-in font's width: Adobe's for what Windows-1252 has, the installed font's for the rest
    // (embedded when drawn, as sc::pdf does).
    double container::standard_width(const font_entry &entry, const std::string_view text) {
        double width = 0;
        std::string other;
        for (std::size_t i = 0; i < text.size();) {
            const auto first = static_cast<unsigned char>(text[i]);
            const std::size_t length = first < 0x80 ? 1 : (first >> 5) == 6 ? 2 : (first >> 4) == 14 ? 3 : (first >> 3) == 30 ? 4 : 1;
            char32_t code = length == 1 ? first : first & (0x7F >> length);
            for (std::size_t k = 1; k < length && i + k < text.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            if (sc::standard_font::encode(code)) width += entry.standard->width(text.substr(i, length), entry.font.size);
            else other.append(text.substr(i, length));
            i += length;
        }
        if (!other.empty()) width += entry.font.width(other);
        return width;
    }

    void container::delete_font(const litehtml::uint_ptr font) {
        if (font > 0 && font <= fonts_.size()) fonts_[font - 1].reset();
    }

    litehtml::pixel_t container::text_width(const char *text, const litehtml::uint_ptr font) {
        if (font == 0 || font > fonts_.size() || !fonts_[font - 1] || !text || !*text) return 0;
        const auto &entry = *fonts_[font - 1];
        if (entry.standard) return px(standard_width(entry, text));
        const double measured = entry.font.width(text);
        return px(measured > 0 ? measured : std::char_traits<char>::length(text) * entry.font.size * 0.5);
    }

    void container::draw_text(const litehtml::uint_ptr hdc, const char *text, const litehtml::uint_ptr font, const litehtml::web_color colour,
                              const litehtml::position &pos) {
        if (font == 0 || font > fonts_.size() || !fonts_[font - 1] || !text) return;
        auto &canvas = canvas_of(hdc);
        const auto &entry = *fonts_[font - 1];
        const double baseline = value(pos.y + pos.height) - entry.descent;
        canvas.draw(sc::text{text, {pos.x, baseline}, entry.font}, colour_of(colour));
        if (entry.decoration == litehtml::text_decoration_line_none) return;
        const double thickness = std::max(1.0, entry.font.size / 14);
        const double width = text_width(text, font);
        const auto line_colour = entry.decoration_colour == litehtml::web_color::current_color ? colour : entry.decoration_colour;
        const auto line_at = [&](const double middle) {
            canvas.draw(rect{pos.x, middle - thickness / 2, width, thickness}, sc::appearance::filled(colour_of(line_colour)));
        };
        if (entry.decoration & litehtml::text_decoration_line_underline) line_at(baseline + std::max(thickness, entry.descent / 2));
        if (entry.decoration & litehtml::text_decoration_line_line_through) line_at(baseline - entry.x_height / 2);
        if (entry.decoration & litehtml::text_decoration_line_overline) line_at(baseline - entry.ascent);
    }

    litehtml::pixel_t container::pt_to_px(const float pt) const { return pt * 96.0f / 72.0f; }

    litehtml::pixel_t container::get_default_font_size() const { return px(settings_.font.size); }

    const char *container::get_default_font_name() const { return settings_.font.family.c_str(); }

    void container::transform_text(std::string &text, const litehtml::text_transform tt) {
        // ASCII letters only: others stay as written.
        bool start = true;
        for (auto &c: text) {
            const auto u = static_cast<unsigned char>(c);
            if (tt == litehtml::text_transform_uppercase) c = static_cast<char>(std::toupper(u));
            else if (tt == litehtml::text_transform_lowercase) c = static_cast<char>(std::tolower(u));
            else if (tt == litehtml::text_transform_capitalize && start) c = static_cast<char>(std::toupper(u));
            start = std::isspace(u) != 0;
        }
    }


    void container::load_image(const char *src, const char *baseurl, bool) {
        const auto url = resolved(src, baseurl);
        if (url.empty()) return;
        if (const auto known = pictures_->problems.find(url); known != pictures_->problems.end()) {
            if (problem_.empty()) problem_ = known->second;
            return;
        }
        if (pictures_->pictures.contains(url)) return;
        auto &slot = pictures_->pictures[url];
        if (url.starts_with("qr:")) { // made here, whatever fetches the rest
            try {
                slot = qr_picture(url);
            } catch (const std::exception &failure) {
                const auto shown = url.size() > 60 ? url.substr(0, 57) + "..." : url;
                pictures_->problems[url] = "sc::web::html: image " + shown + ": " + failure.what();
                if (problem_.empty()) problem_ = pictures_->problems[url];
            }
            return;
        }
        const auto bytes = settings_.fetch(url);
        if (!bytes || bytes->empty()) return;
        try {
            if (sc::bitmap::recognised(*bytes)) {
                sc::bitmap raster{*bytes};
                slot = picture{raster, std::nullopt, raster.size()};
            } else if (is_svg(*bytes)) {
                auto vector = sc::drawing::from_svg(*bytes);
                const auto page = vector.page().area() > 0 ? vector.page() : sc::size{vector.bounds().right(), vector.bounds().bottom()};
                slot = picture{std::nullopt, std::move(vector), page};
            }
        } catch (const std::runtime_error &) {
            // Not an image we can read: left out, as a browser shows nothing for it.
        }
    }

    void container::get_image_size(const char *src, const char *baseurl, litehtml::size &size) {
        const auto *found = find(resolved(src, baseurl));
        size.width = found ? px(found->size.width()) : px(0);
        size.height = found ? px(found->size.height()) : px(0);
    }

    void container::draw_image(const litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const std::string &url, const std::string &base_url) {
        const auto *found = find(resolved(url.c_str(), base_url.c_str()));
        if (!found || value(layer.clip_box.width) <= 0 || value(layer.clip_box.height) <= 0) return;
        auto &canvas = canvas_of(hdc);
        const auto placed = tiles(layer);
        clip_scope clip{canvas, background_clip(layer, placed)};
        for (const auto &tile: placed) draw_picture(canvas, *found, tile);
    }

    void container::draw_list_marker(const litehtml::uint_ptr hdc, const litehtml::list_marker &marker) {
        auto &canvas = canvas_of(hdc);
        const rect box = rect_of(marker.pos);
        if (!marker.image.empty()) {
            load_image(marker.image.c_str(), marker.baseurl, false);
            if (const auto *found = find(resolved(marker.image.c_str(), marker.baseurl))) {
                draw_picture(canvas, *found, {box.left(), box.top(), found->size.width(), found->size.height()});
            }
            return;
        }
        const auto colour = colour_of(marker.color);
        const sc::circle dot{box.center(), std::min(box.width(), box.height()) / 2};
        switch (marker.marker_type) {
            case litehtml::list_style_type_disc: canvas.draw(dot, sc::appearance::filled(colour)); break;
            case litehtml::list_style_type_circle: canvas.draw(dot, sc::stroke{colour, 1}); break;
            case litehtml::list_style_type_square: canvas.draw(box, sc::appearance::filled(colour)); break;
            default: break; // numbers and letters are text litehtml draws itself
        }
    }


    void container::draw_solid_fill(const litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const litehtml::web_color &colour) {
        if (colour.alpha == 0) return;
        auto &canvas = canvas_of(hdc);
        const radii corners = radii_of(layer.border_radius);
        const rect visible = visible_of(layer);
        if (!rounded(corners) || inside(rect_of(layer.border_box), visible)) {
            canvas.draw(box_path(rounded(corners) ? rect_of(layer.border_box) : visible, corners), sc::appearance::filled(colour_of(colour)));
            return;
        }
        clip_scope clip{canvas, rect_path(visible)};
        canvas.draw(box_path(rect_of(layer.border_box), corners), sc::appearance::filled(colour_of(colour)));
    }

    void container::draw_linear_gradient(const litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                                         const litehtml::background_layer::linear_gradient &gradient) {
        if (gradient.color_points.empty()) return;
        auto &canvas = canvas_of(hdc);
        const radii corners = radii_of(layer.border_radius);
        const rect visible = visible_of(layer);
        clip_scope clip{canvas, rounded(corners) ? box_path(rect_of(layer.border_box), corners) : path{}};
        std::vector<sc::linear_gradient::stop> stops;
        for (const auto &point: gradient.color_points) stops.push_back({point.offset, colour_of(point.color)});
        // The gradient is placed for the origin box's tile; each other tile has it moved along, and
        // is cut to where the background shows.
        for (const auto &tile: tiles(layer)) {
            const double dx = tile.left() - value(layer.origin_box.x), dy = tile.top() - value(layer.origin_box.y);
            const sc::linear_gradient paint{{gradient.start.x + dx, gradient.start.y + dy}, {gradient.end.x + dx, gradient.end.y + dy}, stops};
            canvas.draw(rect_path(tile.intersect(visible)), sc::appearance::filled(paint));
        }
    }

    namespace {
        // A gradient sc::canvas can't draw yet, as the colour halfway through it.
        // TODO: radial and conic gradients.
        litehtml::web_color middle_colour(const litehtml::background_layer::gradient_base &gradient) {
            if (gradient.color_points.empty()) return {0, 0, 0, 0};
            std::vector<sc::linear_gradient::stop> stops;
            for (const auto &point: gradient.color_points) stops.push_back({point.offset, colour_of(point.color)});
            const auto middle = sc::linear_gradient{{0, 0}, {1, 0}, stops}.colour_at(0.5);
            return {static_cast<litehtml::byte>(middle.red() * 255), static_cast<litehtml::byte>(middle.green() * 255),
                    static_cast<litehtml::byte>(middle.blue() * 255), static_cast<litehtml::byte>(middle.alpha() * 255)};
        }
    }

    void container::draw_radial_gradient(const litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                                         const litehtml::background_layer::radial_gradient &gradient) {
        draw_solid_fill(hdc, layer, middle_colour(gradient));
    }

    void container::draw_conic_gradient(const litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                                        const litehtml::background_layer::conic_gradient &gradient) {
        draw_solid_fill(hdc, layer, middle_colour(gradient));
    }


    namespace {
        bool shows(const litehtml::border &side) {
            return value(side.width) > 0 && side.color.alpha > 0 && side.style != litehtml::border_style_none && side.style != litehtml::border_style_hidden;
        }

        // The band between fractions from and to of the border's width, as a ring.
        path ring(const rect &box, const radii &corners, const std::array<double, 4> &widths, const double from, const double to) {
            const auto scaled = [&](const double fraction) {
                return std::array<double, 4>{widths[0] * fraction, widths[1] * fraction, widths[2] * fraction, widths[3] * fraction};
            };
            path out = box_path(inset(box, scaled(from)), inset(corners, scaled(from)));
            out.append(box_path(inset(box, scaled(to)), inset(corners, scaled(to)), false));
            return out;
        }

        // One side's part of the band (a trapezoid, its ends cut on the corners' diagonals).
        polygon side_band(const rect &box, const std::array<double, 4> &widths, const int side, const double from, const double to) {
            const auto at = [&](const double fraction) {
                return inset(box, {widths[0] * fraction, widths[1] * fraction, widths[2] * fraction, widths[3] * fraction});
            };
            const rect outer = at(from), inner = at(to);
            switch (side) {
                case 0: return polygon{{outer.left(), outer.bottom()}, {outer.left(), outer.top()}, {inner.left(), inner.top()}, {inner.left(), inner.bottom()}};
                case 1: return polygon{{outer.left(), outer.top()}, {outer.right(), outer.top()}, {inner.right(), inner.top()}, {inner.left(), inner.top()}};
                case 2: return polygon{{outer.right(), outer.top()}, {outer.right(), outer.bottom()}, {inner.right(), inner.bottom()}, {inner.right(), inner.top()}};
                default: return polygon{{outer.right(), outer.bottom()}, {outer.left(), outer.bottom()}, {inner.left(), inner.bottom()}, {inner.right(), inner.bottom()}};
            }
        }

        // A side's colour for its style: inset darkens the top and left, outset the bottom and right.
        sc::color side_colour(const litehtml::border &side, const int which) {
            sc::color colour = colour_of(side.color);
            const bool top_left = which == 0 || which == 1;
            const bool darker = (side.style == litehtml::border_style_inset && top_left) || (side.style == litehtml::border_style_outset && !top_left);
            if (darker) colour = {colour.red() * 0.6, colour.green() * 0.6, colour.blue() * 0.6, colour.alpha()};
            return colour;
        }

        // Dots or dashes along one side, as squares or bars of its width, clipped to its band.
        void dashes(sc::canvas &canvas, const rect &box, const std::array<double, 4> &widths, const int side, const litehtml::border &border) {
            const double w = border.width;
            const bool dotted = border.style == litehtml::border_style_dotted;
            const double on = dotted ? w : 3 * w, period = dotted ? 2 * w : 4 * w;
            const bool across = side == 1 || side == 3;
            const double length = across ? box.width() : box.height();
            const int count = std::max(1, static_cast<int>(std::round((length + period - on) / period)));
            const double step = count > 1 ? (length - on) / (count - 1) : 0; // the ends land on the corners
            sc::group marks;
            for (int i = 0; i < count; ++i) {
                const double along = i * step;
                switch (side) {
                    case 0: marks.push_back(rect{box.left(), box.top() + along, w, on}); break;
                    case 1: marks.push_back(rect{box.left() + along, box.top(), on, w}); break;
                    case 2: marks.push_back(rect{box.right() - w, box.top() + along, w, on}); break;
                    default: marks.push_back(rect{box.left() + along, box.bottom() - w, on, w}); break;
                }
            }
            canvas.draw(marks, sc::appearance::filled(colour_of(border.color)));
        }
    }

    void container::draw_borders(const litehtml::uint_ptr hdc, const litehtml::borders &borders, const litehtml::position &draw_pos, bool) {
        const std::array<const litehtml::border *, 4> sides{&borders.left, &borders.top, &borders.right, &borders.bottom};
        if (std::ranges::none_of(sides, [](const auto *side) { return shows(*side); })) return;
        auto &canvas = canvas_of(hdc);
        const rect box = rect_of(draw_pos);
        const radii corners = radii_of(borders.radius);
        std::array<double, 4> widths{};
        for (int i = 0; i < 4; ++i) widths[i] = shows(*sides[i]) ? value(sides[i]->width) : 0;

        // One colour and style all round, solid or double: the ring itself.
        const auto &first = *std::ranges::find_if(sides, [](const auto *side) { return shows(*side); });
        const bool uniform = std::ranges::all_of(sides, [&](const auto *side) {
            return !shows(*side) || (side->color == first->color && side->style == first->style);
        });
        const bool solid = first->style == litehtml::border_style_solid || first->style == litehtml::border_style_groove ||
                           first->style == litehtml::border_style_ridge;
        const bool doubled = first->style == litehtml::border_style_double;
        if (uniform && (solid || doubled)) {
            const auto fill = sc::appearance::filled(colour_of(first->color));
            if (doubled) {
                canvas.draw(ring(box, corners, widths, 0, 1.0 / 3), fill);
                canvas.draw(ring(box, corners, widths, 2.0 / 3, 1), fill);
            } else {
                canvas.draw(ring(box, corners, widths, 0, 1), fill);
            }
            return;
        }

        // Otherwise side by side, each within the ring.
        clip_scope ring_clip{canvas, rounded(corners) ? ring(box, corners, widths, 0, 1) : path{}};
        for (int i = 0; i < 4; ++i) {
            const auto &side = *sides[i];
            if (!shows(side)) continue;
            switch (side.style) {
                case litehtml::border_style_dotted:
                case litehtml::border_style_dashed: {
                    clip_scope band{canvas, to_path(side_band(box, widths, i, 0, 1))};
                    dashes(canvas, box, widths, i, side);
                    break;
                }
                case litehtml::border_style_double:
                    canvas.draw(side_band(box, widths, i, 0, 1.0 / 3), sc::appearance::filled(colour_of(side.color)));
                    canvas.draw(side_band(box, widths, i, 2.0 / 3, 1), sc::appearance::filled(colour_of(side.color)));
                    break;
                default: // solid; groove and ridge drawn so too (TODO: their two tones)
                    canvas.draw(side_band(box, widths, i, 0, 1), sc::appearance::filled(side_colour(side, i)));
                    break;
            }
        }
    }


    void container::set_clip(const litehtml::position &pos, const litehtml::border_radiuses &radius) {
        if (!target) return;
        target->push_clip(box_path(rect_of(pos), radii_of(radius)));
        ++clips_;
    }

    void container::del_clip() {
        if (!target || clips_ == 0) return;
        target->pop_clip();
        --clips_;
    }


    void container::set_caption(const char *text) { caption = text ? text : ""; }

    void container::set_base_url(const char *base_url) {
        if (base_url && *base_url) base_url_ = html::resolve(base_url, settings_.base_url);
    }

    void container::link(const std::shared_ptr<litehtml::document> &, const litehtml::element::ptr &) {
    }

    void container::on_anchor_click(const char *, const litehtml::element::ptr &) {
    }

    void container::on_mouse_event(const litehtml::element::ptr &, litehtml::mouse_event) {
    }

    void container::set_cursor(const char *) {
    }

    void container::import_css(std::string &text, const std::string &url, std::string &baseurl) {
        const auto where = resolved(url.c_str(), baseurl.c_str());
        text = settings_.fetch(where).value_or("");
        baseurl = where; // what the stylesheet's own URLs are relative to
    }

    void container::get_viewport(litehtml::position &viewport) const {
        viewport = {0, 0, px(layout_width), px(viewport_height())};
    }

    litehtml::element::ptr container::create_element(const char *, const litehtml::string_map &, const std::shared_ptr<litehtml::document> &) {
        return nullptr; // litehtml's own elements
    }

    void container::get_media_features(litehtml::media_features &media) const {
        media.type = settings_.media == "screen" ? litehtml::media_type_screen : litehtml::media_type_print;
        media.width = px(layout_width);
        media.height = px(viewport_height());
        media.device_width = media.width;
        media.device_height = media.height;
        media.color = 8;
        media.monochrome = 0;
        media.color_index = 256;
        media.resolution = 96;
    }

    void container::get_language(std::string &language, std::string &culture) const {
        language = "en";
        culture.clear();
    }
}
