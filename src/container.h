#pragma once

// litehtml's document_container on an sc::canvas: fonts through sc::font, text, backgrounds (colours,
// linear gradients, images), borders, list markers and clips as canvas calls. Not installed.

#include "html.h"

#include <litehtml.h>

#include <bitmap.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sc::web::detail {
    // An image a document uses: a PNG or JPEG, or an SVG read as a drawing; its size in px.
    struct picture {
        std::optional<sc::bitmap> raster;
        std::optional<sc::drawing> vector;
        sc::size size;
    };

    class container final : public litehtml::document_container {
    public:
        explicit container(html::options settings);

        // The canvas draw() paints on, while it does.
        sc::canvas *target = nullptr;
        double layout_width = 0;
        std::string caption;

        [[nodiscard]] const html::options &settings() const { return settings_; }

        litehtml::uint_ptr create_font(const litehtml::font_description &description, const litehtml::document *doc,
                                       litehtml::font_metrics *metrics) override;

        void delete_font(litehtml::uint_ptr font) override;

        litehtml::pixel_t text_width(const char *text, litehtml::uint_ptr font) override;

        void draw_text(litehtml::uint_ptr hdc, const char *text, litehtml::uint_ptr font, litehtml::web_color colour, const litehtml::position &pos) override;

        litehtml::pixel_t pt_to_px(float pt) const override;

        litehtml::pixel_t get_default_font_size() const override;

        const char *get_default_font_name() const override;

        void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker &marker) override;

        void load_image(const char *src, const char *baseurl, bool redraw_on_ready) override;

        void get_image_size(const char *src, const char *baseurl, litehtml::size &size) override;

        void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const std::string &url, const std::string &base_url) override;

        void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer &layer, const litehtml::web_color &colour) override;

        void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                                  const litehtml::background_layer::linear_gradient &gradient) override;

        void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                                  const litehtml::background_layer::radial_gradient &gradient) override;

        void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer &layer,
                                 const litehtml::background_layer::conic_gradient &gradient) override;

        void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders &borders, const litehtml::position &draw_pos, bool root) override;

        void set_caption(const char *text) override;

        void set_base_url(const char *base_url) override;

        void link(const std::shared_ptr<litehtml::document> &doc, const litehtml::element::ptr &el) override;

        void on_anchor_click(const char *url, const litehtml::element::ptr &el) override;

        void on_mouse_event(const litehtml::element::ptr &el, litehtml::mouse_event event) override;

        void set_cursor(const char *cursor) override;

        void transform_text(litehtml::string &text, litehtml::text_transform tt) override;

        void import_css(litehtml::string &text, const litehtml::string &url, litehtml::string &baseurl) override;

        void set_clip(const litehtml::position &pos, const litehtml::border_radiuses &radius) override;

        void del_clip() override;

        void get_viewport(litehtml::position &viewport) const override;

        litehtml::element::ptr create_element(const char *tag_name, const litehtml::string_map &attributes,
                                              const std::shared_ptr<litehtml::document> &doc) override;

        void get_media_features(litehtml::media_features &media) const override;

        void get_language(litehtml::string &language, litehtml::string &culture) const override;

    private:
        struct font_entry {
            sc::font font;
            int decoration = 0; // litehtml::text_decoration_line flags
            litehtml::web_color decoration_colour;
            double ascent = 0, descent = 0, x_height = 0;
        };

        html::options settings_;
        std::string base_url_; // a <base href>, else the settings'
        std::vector<std::optional<font_entry> > fonts_; // a font's handle is its index + 1
        std::map<std::string, std::optional<picture> > pictures_; // by resolved URL; nothing when it didn't load
        int clips_ = 0; // pushed on the target and not yet popped

        [[nodiscard]] std::string resolved(const char *src, const char *baseurl) const;

        [[nodiscard]] const picture *find(const std::string &url) const;

        [[nodiscard]] static sc::canvas &canvas_of(litehtml::uint_ptr hdc) { return *reinterpret_cast<sc::canvas *>(hdc); }
    };
}
