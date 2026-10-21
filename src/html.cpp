#include "html.h"

#include "container.h"

#include <litehtml/render_item.h>

#include <base64.h>
#include <rest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace sc::web {
    namespace detail {
        struct html_data {
            container host;
            litehtml::document::ptr document;
            bool laid_out = false;
            double fitted = 0; // what render() found the content needs

            html_data(const std::string_view text, html::options settings) : host(std::move(settings)) {
                const auto &css = host.settings().css;
                // litehtml's own defaults, but tables inherit their font size, as browsers do in standards
                // mode (litehtml always gives them "medium", as browsers do only in quirks mode).
                static const std::string defaults = std::string{litehtml::master_css} + "\ntable { font-size: inherit; }\n";
                document = litehtml::document::createFromString(litehtml::estring{std::string{text}, litehtml::encoding::utf_8}, &host, defaults, css);
            }
        };
    }

    namespace {
        bool has_scheme(const std::string_view url) {
            const auto colon = url.find(':');
            if (colon == std::string_view::npos || colon < 2) return false; // C:\ is a drive, not a scheme
            return std::ranges::all_of(url.substr(0, colon), [](const unsigned char c) { return std::isalnum(c) || c == '+' || c == '-' || c == '.'; });
        }

        // a/b/../c as a/c, ./ dropped.
        std::string normalised(const std::string &path) {
            std::vector<std::string> parts;
            std::size_t start = 0;
            const bool absolute = path.starts_with('/');
            while (start <= path.size()) {
                auto end = path.find('/', start);
                if (end == std::string::npos) end = path.size();
                const std::string part = path.substr(start, end - start);
                start = end + 1;
                if (part == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else if (part != "." && !(part.empty() && end != path.size())) {
                    parts.push_back(part);
                }
            }
            std::string out = absolute ? "/" : "";
            for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? "/" : "") + parts[i];
            return out;
        }

        rect border_box(const litehtml::element::ptr &element) {
            const auto item = element->get_render_item();
            const auto placed = element->get_placement();
            rect box{placed.x, placed.y, placed.width, placed.height};
            if (item) {
                const double left = item->padding_left() + item->border_left(), top = item->padding_top() + item->border_top();
                const double right = item->padding_right() + item->border_right(), bottom = item->padding_bottom() + item->border_bottom();
                box = rect{box.left() - left, box.top() - top, box.width() + left + right, box.height() + top + bottom};
            }
            if (box.width() > 0 && box.height() > 0) return box;
            // An inline element (a link around text, say) has its children's boxes.
            bool any = false;
            rect covered = box;
            for (const auto &child: element->children()) {
                const rect inner = border_box(child);
                if (inner.width() <= 0 && inner.height() <= 0) continue;
                if (any) covered.include(inner);
                else covered = inner;
                any = true;
            }
            return covered;
        }

        const litehtml::element &node_of(const std::shared_ptr<const void> &node) { return *static_cast<const litehtml::element *>(node.get()); }
    }

    // element

    std::string html::element::name() const {
        const char *tag = node_of(node_).get_tagName();
        return tag ? tag : "";
    }

    std::optional<std::string> html::element::attribute(const std::string_view name) const {
        const char *value = node_of(node_).get_attr(std::string{name}.c_str());
        if (!value) return std::nullopt;
        return std::string{value};
    }

    std::string html::element::text() const {
        std::string all;
        node_of(node_).get_text(all);
        return all;
    }

    rect html::element::box() const {
        auto *node = const_cast<litehtml::element *>(&node_of(node_));
        return border_box(node->shared_from_this());
    }

    // html

    html::html(const std::string_view text, options settings) : data_(std::make_shared<detail::html_data>(text, std::move(settings))) {
    }

    html::html(html &&) noexcept = default;
    html &html::operator=(html &&) noexcept = default;
    html::~html() = default;

    double html::layout(const double width) {
        data_->host.layout_width = width;
        data_->document->media_changed(); // @media rules see the new width
        data_->fitted = data_->document->render(static_cast<litehtml::pixel_t>(width));
        data_->laid_out = true;
        return height();
    }

    double html::width() const { return data_->document->width(); }

    double html::height() const { return data_->document->height(); }

    double html::fitted_width() const { return data_->fitted; }

    void html::draw(sc::canvas &canvas, const sc::transform &placement, const std::optional<rect> &clip) {
        if (placement != sc::transform{}) {
            auto picture = drawing(clip);
            picture.apply(placement);
            canvas.draw(picture);
            return;
        }
        if (!data_->laid_out) layout(800);
        auto &host = data_->host;
        host.target = &canvas;
        litehtml::position limit;
        if (clip) limit = {static_cast<litehtml::pixel_t>(clip->left()), static_cast<litehtml::pixel_t>(clip->top()),
                           static_cast<litehtml::pixel_t>(clip->width()), static_cast<litehtml::pixel_t>(clip->height())};
        try {
            data_->document->draw(reinterpret_cast<litehtml::uint_ptr>(&canvas), 0, 0, clip ? &limit : nullptr);
        } catch (...) {
            host.target = nullptr;
            throw;
        }
        host.target = nullptr;
    }

    sc::drawing html::drawing(const std::optional<rect> &clip) {
        sc::drawing picture;
        draw(picture, {}, clip);
        picture.page({width(), height()});
        return picture;
    }

    namespace {
        // A selector list ("h1, h2.title") as its selectors, split at the commas outside brackets,
        // parentheses and quotes; one that doesn't parse is left out.
        std::vector<litehtml::css_selector> selectors_of(const std::string_view list, const litehtml::document_mode mode) {
            std::vector<litehtml::css_selector> out;
            int depth = 0;
            char quote = 0;
            std::size_t start = 0;
            const auto take = [&](const std::size_t end) {
                litehtml::css_selector selector;
                if (selector.parse(std::string{list.substr(start, end - start)}, mode)) out.push_back(std::move(selector));
                start = end + 1;
            };
            for (std::size_t i = 0; i < list.size(); ++i) {
                const char c = list[i];
                if (quote) {
                    if (c == quote) quote = 0;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '(' || c == '[') {
                    ++depth;
                } else if ((c == ')' || c == ']') && depth > 0) {
                    --depth;
                } else if (c == ',' && depth == 0) {
                    take(i);
                }
            }
            take(list.size());
            return out;
        }

        void collect(const litehtml::element::ptr &node, std::vector<litehtml::css_selector> &selectors, std::vector<litehtml::element::ptr> &found) {
            for (auto &selector: selectors) {
                if (node->select(selector, true) != litehtml::select_no_match) {
                    found.push_back(node);
                    break;
                }
            }
            for (const auto &child: node->children()) collect(child, selectors, found);
        }
    }

    std::vector<html::element> html::select(const std::string_view selector) const {
        std::vector<element> found;
        const auto root = data_->document->root();
        if (!root) return found;
        auto selectors = selectors_of(selector, data_->document->mode());
        std::vector<litehtml::element::ptr> matched;
        collect(root, selectors, matched); // in document order, each once
        for (const auto &node: matched) {
            element each;
            each.node_ = std::shared_ptr<const void>{data_, node.get()}; // aliasing: the document keeps it
            found.push_back(std::move(each));
        }
        return found;
    }

    std::string html::title() const { return data_->host.caption; }

    std::string html::resolve(const std::string_view url, const std::string_view base) {
        if (url.empty()) return std::string{base};
        if (has_scheme(url) || base.empty()) return std::string{url};
        const std::string text{url};
        if (base.starts_with("http://") || base.starts_with("https://")) {
            const auto authority = base.find("//") + 2;
            const auto path_start = base.find('/', authority);
            const std::string origin{base.substr(0, path_start)};
            if (text.starts_with("//")) return std::string{base.substr(0, base.find(':'))} + ":" + text;
            if (text.starts_with('/')) return origin + normalised(text);
            std::string folder = path_start == std::string_view::npos ? "/" : std::string{base.substr(path_start)};
            folder = folder.substr(0, folder.find_first_of("?#"));
            folder = folder.substr(0, folder.rfind('/') + 1);
            return origin + normalised(folder + text);
        }
        if (text.starts_with('/')) return text;
        // A local base: a folder, or a file in one.
        const std::filesystem::path local{std::string{base}};
        const auto folder = std::filesystem::is_directory(local) || base.ends_with('/') ? local : local.parent_path();
        return (folder / text).lexically_normal().string();
    }

    std::optional<std::string> html::default_fetch(const std::string &resolved) {
        if (resolved.starts_with("data:")) {
            const auto comma = resolved.find(',');
            if (comma == std::string::npos) return std::nullopt;
            const auto header = std::string_view{resolved}.substr(5, comma - 5);
            const auto data = std::string_view{resolved}.substr(comma + 1);
            if (!header.ends_with(";base64")) return std::string{data};
            try {
                return sc::base64::decode(data);
            } catch (const std::runtime_error &) {
                return std::nullopt;
            }
        }
        if (resolved.starts_with("http://") || resolved.starts_with("https://")) {
            sc::rest request{resolved};
            auto body = request.get();
            if (request.status() < 200 || request.status() >= 300) return std::nullopt;
            return body;
        }
        auto path = resolved.starts_with("file://") ? resolved.substr(7) : resolved;
        std::ifstream in{path, std::ios::binary};
        if (!in) return std::nullopt;
        return std::string{std::istreambuf_iterator<char>{in}, {}};
    }
}
