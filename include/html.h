#pragma once

#include <canvas.h>
#include <drawing.h>
#include <font.h>
#include <rect.h>
#include <transform.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc::web {
    namespace detail {
        struct html_data;
    }

    /// Where an HTML document finds what it links to (stylesheets, images), by the URL resolved
    /// against its base; nothing when it can't. The default reads data: URIs, local files and http(s).
    using html_fetcher = std::function<std::optional<std::string>(const std::string &resolved)>;

    struct html_options {
        /// CSS after the browser's defaults and before the document's own.
        std::string css;
        /// What relative URLs are resolved against: a folder or file, or an http(s) URL.
        std::string base_url;
        /// The font for text that names none (its size in px; 16 as in browsers).
        sc::font font{"sans-serif", 16};
        /// "print" (the default) or "screen", for @media rules.
        std::string media = "print";
        /// The viewport's height in px: what position: fixed, vh and the initial containing block's
        /// percentages see, and @media's height. Without it, the layout width and an A-series page's
        /// proportions (1.414 times as high).
        std::optional<double> viewport_height;
        /// Text in PDF's built-in fonts, measured with Adobe's metrics for them (sc::standard_font):
        /// CSS families map to Helvetica (Arial, sans-serif and the like), Times (Times New Roman,
        /// serif) or Courier (Courier New, monospace); another family stays itself when it's
        /// installed. Draw on an sc::pdf with builtin_fonts(true), which writes them unembedded.
        bool builtin_fonts = false;
        /// Fetches stylesheets and images; empty for the default (html::default_fetch()).
        html_fetcher fetch;
    };

    /// An HTML document with its CSS, laid out to a width and drawn on any sc::canvas: an sc::pdf,
    /// an sc::image, an sc::drawing (and so SVG). Lengths are CSS pixels (96 to the inch; a point
    /// is 4/3 of one), from the document's top-left corner, y down. Fonts are found and measured
    /// through sc::font (fontconfig), as sc-pdf and sc-mupdf draw them.
    ///
    ///     sc::web::html page{"<h1>Invoice</h1><p>Due in <b>30</b> days.</p>"};
    ///     page.layout(800);                                  // 800 px wide; height() is now known
    ///     page.draw(pdf, sc::transform::scale(0.75));        // px to points, on page 1
    ///
    /// The engine is litehtml, vendored in this module and kept out of its headers, so another can
    /// take its place behind the same class.
    class html {
    public:
        using fetcher = html_fetcher;
        using options = html_options;

        /// An element found by select(): its name, attributes and text, and its box after layout.
        class element {
        public:
            [[nodiscard]] std::string name() const;

            /// An attribute's value, or nothing when it has none.
            [[nodiscard]] std::optional<std::string> attribute(std::string_view name) const;

            [[nodiscard]] std::string text() const;

            /// Its border box in the document, as last laid out.
            [[nodiscard]] rect box() const;

        private:
            friend class html;
            std::shared_ptr<const void> node_; // litehtml::element, keeping the document alive
        };

        /// Parses the HTML (a whole page or a fragment); it's laid out on the first layout().
        explicit html(std::string_view text, options settings = {});

        html(html &&) noexcept;
        html &operator=(html &&) noexcept;
        ~html();

        /// Lays it out width px wide and returns its height. Lay out again for another width.
        double layout(double width);

        [[nodiscard]] double width() const;

        /// The width the content needs, at most the width last laid out at: lay out again at it
        /// for a page no wider than what's in it (a browser's shrink-to-fit). 0 before layout().
        [[nodiscard]] double fitted_width() const;

        [[nodiscard]] double height() const;

        /// Draws it, at its own coordinates or through placement (scaling px to the canvas's units,
        /// moving it on the page). clip, in the document's coordinates, leaves out what's wholly
        /// outside it. Lays it out 800 px wide first when it hasn't been.
        void draw(sc::canvas &canvas, const sc::transform &placement = {}, const std::optional<rect> &clip = {});

        /// What draw() would draw, as an sc::drawing in the document's coordinates.
        [[nodiscard]] sc::drawing drawing(const std::optional<rect> &clip = {});

        /// The elements matching a CSS selector ("a[href]", "h1, h2", "#total"), in document order.
        [[nodiscard]] std::vector<element> select(std::string_view selector) const;

        /// Its <title>, empty when it has none.
        [[nodiscard]] std::string title() const;

        /// The default fetch: a data: URI decoded, a local file read, an http(s) URL got; nothing
        /// when that fails.
        [[nodiscard]] static std::optional<std::string> default_fetch(const std::string &resolved);

        /// url resolved against base: kept when it has a scheme (http:, data:, ...); else joined
        /// to base's URL (or, for a local base, its folder).
        [[nodiscard]] static std::string resolve(std::string_view url, std::string_view base);

    private:
        std::shared_ptr<detail::html_data> data_;
    };
}
