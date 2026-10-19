// sc::web::html: layout, what it draws (recorded on an sc::drawing), elements found by selector,
// images from data: URIs, and how URLs resolve.
#include <html.h>

#include "sc_test.h"

#include <base64.h>

#include <algorithm>
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

    SECTION("An SVG image is drawn as shapes");
    {
        const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="red"/></svg>)";
        sc::web::html page{"<img src='data:image/svg+xml;base64," + sc::base64::encode(svg) + "'>"};
        page.layout(300);
        CHECK_EQ(page.select("img")[0].box().width(), 40.0);
        CHECK_LT(std::size_t{0}, count<sc::drawable>(page.drawing()));
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
