// sc::web::html: layout, what it draws (recorded on an sc::drawing), elements found by selector,
// images from data: URIs, and how URLs resolve.
#include <html.h>

#include "sc_test.h"

#include <base64.h>
#include <sc_qrencode.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <variant>

namespace {
    // A 3 by 2 PNG.
    const std::string png = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAIAAAASFvFNAAAAEUlEQVR4nGP4z8DAgMAgyAAAO9gF+4ea2nQAAAAASUVORK5CYII=";

    template<typename Item>
    std::size_t count(const sc::drawing &picture) {
        return static_cast<std::size_t>(std::ranges::count_if(picture.items(), [](const auto &item) { return std::holds_alternative<Item>(item); }));
    }

    bool has_text(const sc::drawing &picture, const std::string &word) {
        return std::ranges::any_of(picture.items(), [&](const auto &item) {
            const auto *text = std::get_if<sc::label>(&item);
            return text && text->text.content == word;
        });
    }
}

int main() {
    SECTION("Layout: a width in, a height out");
    {
        sc::web::html page{"<html><head><title>A test</title></head><body style='margin: 0'><h1>Hello</h1><p>Some text.</p></body></html>"};
        const double height = page.layout(600);
        CHECK_LT(20.0, height);
        CHECK_EQ(page.height(), height);
        CHECK_EQ(page.title(), std::string{"A test"});
        sc::web::html narrow{"<p>one two three four five six seven eight nine ten</p>"};
        CHECK_LT(narrow.layout(800), narrow.layout(60)); // wrapped onto more lines
    }

    SECTION("Drawing: words as labels, boxes as shapes");
    {
        sc::web::html page{"<div style='background: #2a6f97; border: 2px solid black; padding: 4px'><b>Bold</b> words</div>"};
        page.layout(400);
        const auto picture = page.drawing();
        CHECK(has_text(picture, "Bold"));
        CHECK(has_text(picture, "words"));
        const auto bold = std::ranges::find_if(picture.items(), [](const auto &item) {
            const auto *text = std::get_if<sc::label>(&item);
            return text && text->text.content == "Bold";
        });
        CHECK_EQ(std::get<sc::label>(*bold).text.font.weight, 700);
        CHECK_LT(std::size_t{1}, count<sc::drawable>(picture)); // the background and the border
        CHECK_EQ(picture.page(), (sc::size{page.width(), page.height()}));
    }

    SECTION("Selecting elements: attributes, text and boxes");
    {
        sc::web::html page{"<p id='intro'>See <a href='https://example.com'>the site</a>.</p><h2>One</h2><h2>Two</h2>"};
        page.layout(500);
        const auto links = page.select("a[href]");
        CHECK_EQ(links.size(), std::size_t{1});
        CHECK_EQ(links[0].name(), std::string{"a"});
        CHECK_EQ(links[0].attribute("href").value_or(""), std::string{"https://example.com"});
        CHECK(!links[0].attribute("title"));
        CHECK_EQ(links[0].text(), std::string{"the site"});
        CHECK_LT(10.0, links[0].box().width());
        const auto headings = page.select("h2");
        CHECK_EQ(headings.size(), std::size_t{2});
        CHECK_LT(headings[0].box().top(), headings[1].box().top());
        CHECK_EQ(page.select("#intro").size(), std::size_t{1});
        const auto list = page.select("h2, #intro, a"); // a list, in document order
        CHECK_EQ(list.size(), std::size_t{4});
        CHECK_EQ(list[0].name(), std::string{"p"});
        CHECK_EQ(list[1].name(), std::string{"a"});
        CHECK_EQ(list[3].text(), std::string{"Two"});
        CHECK(page.select("h2[").empty()); // not a selector: nothing
    }

    SECTION("Images: a data: URI's PNG, at its size or the one asked for");
    {
        sc::web::html page{"<img src='" + png + "'><img src='" + png + "' width='30' height='20'><img src='missing.png'>"};
        page.layout(300);
        const auto picture = page.drawing();
        CHECK_EQ(count<sc::figure>(picture), std::size_t{2});
        const auto images = page.select("img");
        CHECK_EQ(images[0].box().width(), 3.0);
        CHECK_EQ(images[1].box().width(), 30.0);
        CHECK_EQ(images[1].box().height(), 20.0);
    }

    SECTION("An image cache: documents sharing it fetch an image once and draw the same bitmap");
    {
        int fetched = 0;
        sc::web::html::options settings;
        settings.fetch = [&](const std::string &url) -> std::optional<std::string> {
            ++fetched;
            return sc::web::html::default_fetch(url);
        };
        const auto first_bitmap = [](sc::web::html page) {
            page.layout(300);
            const auto picture = page.drawing();
            for (const auto &item: picture.items())
                if (const auto *shown = std::get_if<sc::figure>(&item)) return shown->bitmap;
            throw std::runtime_error("no image drawn");
        };
        const auto alone = first_bitmap(sc::web::html{"<img src='" + png + "'>", settings});
        settings.images = sc::web::image_cache{};
        const auto one = first_bitmap(sc::web::html{"<img src='" + png + "'>", settings});
        const auto two = first_bitmap(sc::web::html{"<p>Again</p><img src='" + png + "'>", settings});
        CHECK_EQ(fetched, 2);
        CHECK(&one.bytes() == &two.bytes());
        CHECK(&alone.bytes() != &one.bytes());
    }

    SECTION("An image given as a file on disk is read from there, even with a web base");
    {
        const auto file = std::filesystem::temp_directory_path() / ("sc-litehtml-test-" + std::to_string(std::random_device{}()) + ".png");
        { std::ofstream{file, std::ios::binary} << sc::base64::decode(png.substr(png.find(',') + 1)); }
        sc::web::html::options settings;
        settings.base_url = "https://example.invalid/site/";
        settings.fetch = [](const std::string &url) -> std::optional<std::string> {
            if (url.starts_with("https://")) return std::nullopt; // no network in the test
            return sc::web::html::default_fetch(url);
        };
        sc::web::html page{"<img src='" + file.string() + "'><img src='/no/such/logo.png'>", settings};
        page.layout(300);
        CHECK_EQ(count<sc::figure>(page.drawing()), std::size_t{1}); // the missing one stays a web path
        CHECK_EQ(page.select("img")[0].box().width(), 3.0);
        std::filesystem::remove(file);
    }

    SECTION("An SVG image is drawn as shapes");
    {
        const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="red"/></svg>)";
        sc::web::html page{"<img src='data:image/svg+xml;base64," + sc::base64::encode(svg) + "'>"};
        page.layout(300);
        CHECK_EQ(page.select("img")[0].box().width(), 40.0);
        CHECK_LT(std::size_t{0}, count<sc::drawable>(page.drawing()));
    }

    SECTION("qr: images: a QR code from the URI's data, drawn as shapes, shaped as a data: URI");
    {
        const std::string link = "https://example.com/invoice/123";
        const int modules = sc::qr::code{link}.size();
        // Percent-encoded text, 4 px a module with a quiet zone of 4.
        sc::web::html page{"<img src='qr:,https%3A%2F%2Fexample.com%2Finvoice%2F123'>"};
        page.layout(600);
        CHECK_EQ(page.select("img")[0].box().width(), 4.0 * (modules + 8));
        CHECK_LT(std::size_t{0}, count<sc::drawable>(page.drawing())); // shapes, not a bitmap
        CHECK_EQ(count<sc::figure>(page.drawing()), std::size_t{0});
        // Base64, settings, the size CSS gives it, no comma at all; and a custom fetch doesn't stop it.
        sc::web::html::options offline;
        offline.fetch = [](const std::string &) -> std::optional<std::string> { return std::nullopt; };
        sc::web::html more{"<img src='qr:;base64," + sc::base64::encode(link) + "'>"
                           "<img src='qr:margin=0;correction=h,https%3A%2F%2Fexample.com%2Finvoice%2F123'>"
                           "<img src='qr:,hello' style='width: 25mm'><img src='qr:hello'>", offline};
        more.layout(600);
        const auto images = more.select("img");
        CHECK_EQ(images[0].box().width(), 4.0 * (modules + 8));
        CHECK_EQ(images[1].box().width(), 4.0 * sc::qr::code(link, sc::qr::correction::high).size()); // no quiet zone
        CHECK_NEAR(images[2].box().width(), 25 * 96 / 25.4, 0.01);
        CHECK_EQ(images[3].box().width(), 4.0 * (sc::qr::code{"hello"}.size() + 8));
        // Mistakes are errors naming the image, not a document with a code missing.
        CHECK_THROWS_AS(sc::web::html("<img src='qr:colour=red,x'>"), std::invalid_argument);
        CHECK_THROWS_AS(sc::web::html("<img src='qr:correction=Z,x'>"), std::invalid_argument);
        CHECK_THROWS_AS(sc::web::html("<img src='qr:margin=-1,x'>"), std::invalid_argument);
        CHECK_THROWS_AS(sc::web::html("<img src='qr:,'>"), std::invalid_argument);
        CHECK_THROWS_AS(sc::web::html("<img src='qr:," + std::string(3000, '7') + std::string(3000, 'x') + "'>"), std::invalid_argument);
        try {
            sc::web::html bad{"<p>Text</p><img src='qr:colour=red,x'>"};
        } catch (const std::invalid_argument &problem) {
            CHECK(std::string{problem.what()}.find("qr:colour=red,x: unknown setting 'colour=red'") != std::string::npos);
        }
    }

    SECTION("Stylesheets: the options' CSS, and one fetched");
    {
        sc::web::html::options settings;
        settings.css = "p { font-size: 30px }";
        settings.fetch = [](const std::string &url) -> std::optional<std::string> {
            if (url == "/styles/site.css") return "h1 { font-weight: 300 }";
            return std::nullopt;
        };
        settings.base_url = "/styles/";
        sc::web::html page{"<link rel='stylesheet' href='site.css'><h1>Light</h1><p>Big</p>", settings};
        page.layout(400);
        const auto picture = page.drawing();
        for (const auto &item: picture.items()) {
            if (const auto *text = std::get_if<sc::label>(&item)) {
                if (text->text.content == "Big") CHECK_EQ(text->text.font.size, 30.0);
                if (text->text.content == "Light") CHECK_EQ(text->text.font.weight, 300);
            }
        }
    }

    SECTION("A table's text is the size of the text around it, as in browsers");
    {
        sc::web::html page{"<!DOCTYPE html><body style='font-size: 10pt'><p>Words</p><table><tr><td>Cell</td></tr></table></body>"};
        page.layout(400);
        double words = 0, cell = 0;
        const auto picture = page.drawing(); // kept: items() refers into it
        for (const auto &item: picture.items())
            if (const auto *text = std::get_if<sc::label>(&item)) (text->text.content == "Cell" ? cell : words) = text->text.font.size;
        CHECK_LT(0.0, words);
        CHECK_EQ(cell, words);
    }

    SECTION("Built-in fonts: CSS families as Helvetica, Times or Courier, Adobe's widths");
    {
        sc::web::html::options settings;
        settings.builtin_fonts = true;
        sc::web::html page{"<p style='font-family: Arial'>Sans</p><p style='font-family: serif'>Serif</p>"
                           "<p style='font-family: monospace'>Mono</p><p style='font-family: Unheardof, sans-serif'>Fallen</p>"
                           "<p style='font-family: Helvetica; font-size: 10px'><span id='wai'>Wai</span></p>", settings};
        page.layout(400);
        const auto picture = page.drawing();
        std::map<std::string, std::string> families;
        for (const auto &item: picture.items())
            if (const auto *text = std::get_if<sc::label>(&item)) families[text->text.content] = text->text.font.family;
        CHECK_EQ(families["Sans"], std::string{"Helvetica"});
        CHECK_EQ(families["Serif"], std::string{"Times"});
        CHECK_EQ(families["Mono"], std::string{"Courier"});
        CHECK_EQ(families["Fallen"], std::string{"Helvetica"});
        const auto wai = page.select("#wai");
        CHECK_EQ(wai.size(), std::size_t{1});
        CHECK_LT(std::abs(wai[0].box().width() - (944 + 556 + 222) * 10 / 1000.0), 0.01); // Adobe's, whatever is installed
    }

    SECTION("position: relative on an inline element moves its text and background together, once");
    {
        sc::web::html page{"<!DOCTYPE html><body style='font-size: 10pt'><p>plain <span style='position: relative; top: 20px; left: 40px; "
                           "background: yellow'>moved</span> after</p><p>plain <span style='background: yellow'>moved</span> after</p></body>"};
        page.layout(400);
        const auto picture = page.drawing();
        std::vector<sc::point> words;
        std::vector<sc::rect> backgrounds;
        for (const auto &item: picture.items()) {
            if (const auto *text = std::get_if<sc::label>(&item); text && text->text.content == "moved") words.push_back(text->text.position);
            if (const auto *box = std::get_if<sc::drawable>(&item)) backgrounds.push_back(box->bounds());
        }
        CHECK_EQ(words.size(), std::size_t{2});
        CHECK_EQ(backgrounds.size(), std::size_t{2});
        if (words.size() == 2 && backgrounds.size() == 2) {
            // The relative span is drawn after the rest; sorted by x, the moved one comes second.
            std::ranges::sort(words, {}, [](const sc::point &p) { return p.x(); });
            std::ranges::sort(backgrounds, {}, [](const sc::rect &r) { return r.left(); });
            CHECK_EQ(words[1].x() - words[0].x(), 40.0);
            CHECK_EQ(backgrounds[1].left() - backgrounds[0].left(), 40.0);
            // Each word sits on its background the same way: they moved together.
            CHECK_EQ(words[1].x() - backgrounds[1].left(), words[0].x() - backgrounds[0].left());
            CHECK_EQ(words[1].y() - backgrounds[1].top(), words[0].y() - backgrounds[0].top());
        }
    }

    SECTION("Drawing through a transform: px to points");
    {
        sc::web::html page{"<div style='width: 100px; height: 40px; background: black'></div>"};
        page.layout(200);
        sc::drawing points;
        page.draw(points, sc::transform::scale(0.75));
        const auto box = points.bounds();
        CHECK_LT(std::abs(box.width() - 75), 0.01);
        CHECK_LT(std::abs(box.height() - 30), 0.01);
    }

    SECTION("Resolving URLs");
    {
        using sc::web::html;
        CHECK_EQ(html::resolve("img/a.png", "https://example.com/docs/page.html"), std::string{"https://example.com/docs/img/a.png"});
        CHECK_EQ(html::resolve("../a.png", "https://example.com/docs/sub/"), std::string{"https://example.com/docs/a.png"});
        CHECK_EQ(html::resolve("/a.png", "https://example.com/docs/"), std::string{"https://example.com/a.png"});
        CHECK_EQ(html::resolve("//cdn.example.com/a.png", "https://example.com/"), std::string{"https://cdn.example.com/a.png"});
        CHECK_EQ(html::resolve("https://other.org/x", "https://example.com/"), std::string{"https://other.org/x"});
        CHECK_EQ(html::resolve("data:image/png;base64,AA", "https://example.com/"), std::string{"data:image/png;base64,AA"});
        CHECK_EQ(html::resolve("a.png", "/srv/site/index.html"), std::string{"/srv/site/a.png"});
        CHECK_EQ(html::resolve("a.png", ""), std::string{"a.png"});
        CHECK_EQ(html::default_fetch("data:text/plain,hi").value_or(""), std::string{"hi"});
        CHECK(!html::default_fetch("/no/such/file.css"));
    }
    TEST_SUMMARY();
}
