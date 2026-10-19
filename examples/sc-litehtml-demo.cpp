// Lays out HTML with sc::web::html (litehtml), draws it on an sc::drawing and saves that as SVG,
// then shows what it found by selector. With no FILE it lays out a small page of its own; --help
// lists the options.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <html.h>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-litehtml-demo"},
        {"description", "Lays out an HTML file (or, without FILE, a page of its own) with sc::web::html and saves it as SVG."},
        {"options", {
            {"width", {{"short", "w"}, {"type", "number"}, {"default", 600}, {"help", "the width to lay out to, in CSS px"}}},
            {"output", {{"short", "o"}, {"default", "sc-litehtml-demo.svg"}, {"help", "the SVG to write"}}}}},
        {"positional", {
            {"file", {{"required", false}, {"help", "the HTML file to lay out"}}}}}}};

    try {
        sc::console::title("simply-cpp litehtml: HTML on any canvas");
        const auto *file = args.lookup("file");

        // [readme]
        sc::web::html::options settings;
        settings.css = "body { font-family: sans-serif; margin: 16px }";
        std::string text;
        if (file) {
            const std::filesystem::path path = file->get<std::string>();
            text = sc::web::html::default_fetch(path.string()).value_or("");
            settings.base_url = path.string(); // its images and stylesheets are next to it
        } else {
            text = R"html(<h1 style="color: #2a6f97">An invoice</h1>
                      <p>Thank you for your order. It is due in <b>30 days</b>; see <a href="https://example.com/terms">the terms</a>.</p>
                      <table style="border-collapse: collapse; width: 100%">
                        <tr style="background: #e8f1f8"><th align="left">Item</th><th align="right">Amount</th></tr>
                        <tr><td style="border-top: 1px solid #999">Widgets</td><td align="right" style="border-top: 1px solid #999">120.00</td></tr>
                        <tr><td style="border-top: 1px dashed #999">Gadgets</td><td align="right" style="border-top: 1px dashed #999">80.00</td></tr>
                      </table>
                      <ul><li>Bank transfer</li><li>Card</li></ul>
                      <div style="border: 3px double #2a6f97; border-radius: 12px; padding: 8px; background: linear-gradient(to right, #fff, #e8f1f8)">Total: <b>200.00</b></div>)html";
        }
        sc::web::html page{text, settings};
        SC_SHOW(page.layout(args.as<double>("width")));       // the height, in px
        auto picture = page.drawing();                          // what it draws, as data
        SC_SHOW(picture.size());
        picture.save_svg(args.as<std::string>("output"));
        sc::console::note("saved " + args.as<std::string>("output"));

        sc::console::heading("Elements by selector");
        for (const auto &link: page.select("a[href]"))
            sc::console::note("link to " + link.attribute("href").value_or("") + " at " + sc::console::format(link.box()));
        for (const auto &heading: page.select("h1, h2, h3")) sc::console::note(heading.name() + ": " + heading.text());
        // [/readme]
    } catch (const std::exception &error) {
        std::cerr << "sc-litehtml-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
