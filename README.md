# simply-cpp-litehtml

HTML and CSS laid out by [litehtml](https://github.com/litehtml/litehtml) and
drawn on any `sc::canvas` from
[sc-graphics](https://github.com/roelofrossouw/simply-cpp-graphics): an
`sc::pdf`, an `sc::image`, an `sc::drawing` (and so SVG).
[sc-html2pdf](https://github.com/roelofrossouw/simply-cpp-html2pdf) builds on
it to write HTML as pages of a PDF.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has an [HTML to PDF guide](https://github.com/roelofrossouw/simply-cpp/wiki/HTML-to-PDF) and the [sc-litehtml reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-litehtml), plus [getting started](https://github.com/roelofrossouw/simply-cpp/wiki/Getting-Started) for the whole suite.

## Licence, and the engine

**sc-litehtml is Apache-2.0.** litehtml (BSD-3-Clause) and the gumbo HTML
parser it ships (Apache-2.0) are vendored in `third_party/litehtml`, a git
subtree of [roelofrossouw/litehtml](https://github.com/roelofrossouw/litehtml)
(upstream plus a fix that leaves a justified paragraph's last line alone), and
compiled into this module's libraries. Their headers stay out of the installed
ones: `sc::web::html` is all a program sees, so another engine can take
litehtml's place behind it.

## Usage

```cmake
find_package(sc-litehtml CONFIG REQUIRED)

target_link_libraries(myapp PRIVATE sc::sc-litehtml-shared)
```

```cpp
#include <html.h>

sc::web::html::options settings;
settings.css = "body { font-family: sans-serif }";        // after the defaults, before the page's own
settings.base_url = "/srv/site/index.html";               // relative links: a folder, file or http(s) URL
sc::web::html page{"<h1>Invoice</h1><p>Due in <b>30</b> days.</p>", settings};

page.layout(800);                                         // 800 CSS px wide; returns the height
page.draw(pdf, sc::transform::scale(0.75));               // px to points, on any canvas
const sc::drawing picture = page.drawing();               // or as data: measure it, save it as SVG

for (const auto &link: page.select("a[href]"))            // CSS selectors, lists too ("h1, h2")
    std::cout << link.attribute("href").value_or("") << " at " << link.box() << '\n';
```

Lengths are CSS pixels, 96 to the inch (a point is 4/3 of one), from the
document's top-left corner, y down. Fonts are found and measured through
`sc::font` (fontconfig), as sc-pdf and sc-mupdf draw them; a CSS font list
takes its first installed family, else its generic one. Stylesheets
(`<link>`, `@import`) and images come through `options.fetch`, by default from
data: URIs, local files and http(s) (sc-core's `sc::rest`).

What's drawn: text with its weight, style and decoration (underline, overline,
line-through), backgrounds (colours, linear gradients, images, repeated as CSS
says), borders (solid, double, dashed, dotted, inset and outset; rounded
corners), list markers, and clips (`overflow: hidden`, rounded too). Images
are PNG, JPEG (`sc::bitmap`) or SVG (drawn as shapes, staying vector). Not yet:
radial and conic gradients (drawn in their middle colour), groove and ridge
borders' two tones. Characters a font lacks come from a face that has them,
and emoji are drawn in colour (sc-graphics 5.4.0).

## Demo

`sc-litehtml-demo [--width PX] [-o FILE.svg] [FILE.html]` lays out a page
(its own invoice without FILE), saves it as SVG and lists what it found by
selector:

<!-- sc-example: examples/sc-litehtml-demo.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Building locally

Run `bash scripts/install.sh` to configure, build, test, package and install.
It needs sc-core and sc-graphics (installed if missing).

```bash
sudo curl -fsSL https://apt.roelof.co.za/setup.sh | bash
sudo apt -y install simply-cpp-litehtml-dev
```
