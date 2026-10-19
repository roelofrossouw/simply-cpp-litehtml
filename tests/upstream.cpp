// litehtml's own render tests (litehtml-tests, at the commit our litehtml pins), laid out by
// sc::web::html and drawn without cairo, pango or GTK: each page is drawn into pixels by
// upstream/raster.h and compared with litehtml's reference picture of it. Text in fonts other than
// Ahem is left out of the comparison where it lies (the references' text is pango's), and pages with
// an image other than a PNG are left out.
//
// The environment says where things are (CMake sets it for ctest):
//   SC_LITEHTML_TESTS     litehtml-tests' render folder; without it the test is skipped
//   SC_UPSTREAM_PAGES     the pages checked, one a line (upstream/pages.txt): each must be drawn as
//                         its reference
//   SC_UPSTREAM_ALL       set: every page instead, reporting how many are drawn as their references
//                         and failing none (to explore, and to choose pages for the list)
//   SC_UPSTREAM_PASSING   a file to write the pages drawn as their references to
//   SC_UPSTREAM_FAILED    a folder to write what each differing page drew, as <page>.png
//   SC_UPSTREAM_ONLY      only the pages whose path contains this
#include "upstream/raster.h"

#include <html.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
    std::string environment(const char *name) {
        const char *value = std::getenv(name);
        return value ? value : "";
    }

    // The pages, as litehtml-tests finds them: .htm and .html files, not in or named "support",
    // and not starting with '-' (those are left out there too).
    std::vector<std::string> pages_in(const fs::path &root) {
        std::vector<std::string> out;
        for (auto it = fs::recursive_directory_iterator{root}; it != fs::recursive_directory_iterator{}; ++it) {
            const auto name = it->path().filename().string();
            if (it->is_directory()) {
                if (name.starts_with('-') || name == "support") it.disable_recursion_pending();
                continue;
            }
            const auto extension = it->path().extension();
            if (!name.starts_with('-') && (extension == ".htm" || extension == ".html")) out.push_back(fs::relative(it->path(), root).generic_string());
        }
        std::ranges::sort(out);
        return out;
    }

    // A page's .cfg: "bestfit: false", "width: 300", "height: 200".
    struct config {
        bool best_fit = true;
        std::optional<int> width, height;
    };

    config config_of(const fs::path &page) {
        config out;
        std::ifstream in{page.string() + ".cfg"};
        for (std::string line; std::getline(in, line);) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon), value = line.substr(colon + 1);
            std::erase_if(key, [](const unsigned char c) { return std::isspace(c); });
            std::erase_if(value, [](const unsigned char c) { return std::isspace(c); });
            if (key == "bestfit") out.best_fit = value != "false";
            else if (key == "width") out.width = std::stoi(value);
            else if (key == "height") out.height = std::stoi(value);
        }
        return out;
    }

    std::string read(const fs::path &file) {
        std::ifstream in{file, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    enum class outcome { passed, differs, no_reference, left_out };

    // Whether a pixel lies in any of the boxes.
    std::vector<bool> grid_of(const std::vector<sc::rect> &boxes, const int width, const int height) {
        std::vector<bool> out(static_cast<std::size_t>(width) * height, false);
        for (const auto &box: boxes)
            for (int y = std::max(0, static_cast<int>(box.top())); y < std::min(height, static_cast<int>(std::ceil(box.bottom()))); ++y)
                for (int x = std::max(0, static_cast<int>(box.left())); x < std::min(width, static_cast<int>(std::ceil(box.right()))); ++x)
                    out[static_cast<std::size_t>(y) * width + x] = true;
        return out;
    }

    struct result {
        outcome what;
        std::string detail;
    };

    // The page drawn as litehtml-tests' render2png draws it: 800 px wide, laid out again at the
    // width it needs, on white, in a picture as large as the document.
    result run(const fs::path &root, const std::string &name, const fs::path &failed) {
        const fs::path page = root / name;
        const auto reference = render_test::load_png(page.string() + ".png");
        if (!reference) return {outcome::no_reference, ""};
        const auto settings_of_page = config_of(page);

        sc::web::html::options settings;
        settings.base_url = page.string();
        settings.media = "screen";
        settings.font = sc::font{"serif", 16};
        settings.viewport_height = 600;
        sc::web::html document{read(page), settings};
        document.layout(800);
        // Rounded up: our text widths are fractional (pango's, in the references, are whole pixels),
        // so a width rounded down would wrap the widest line.
        if (settings_of_page.best_fit && document.fitted_width() > 0)
            document.layout(settings_of_page.width.value_or(static_cast<int>(std::ceil(document.fitted_width()))));
        // Rounded up too: our line heights are fractional, the references' whole pixels.
        const int width = settings_of_page.width.value_or(std::max(1, static_cast<int>(std::ceil(document.width()))));
        const int height = settings_of_page.height.value_or(std::max(1, static_cast<int>(std::ceil(document.height()))));

        render_test::raster drawn{width, height};
        drawn.draw(document.drawing(sc::rect{0, 0, static_cast<double>(width), static_cast<double>(height)}));
        if (drawn.what() == render_test::unsupported::image) return {outcome::left_out, "an image other than a PNG"};
        const auto masked = grid_of(drawn.masked(), width, height);

        const auto ours = drawn.result();
        std::string detail;
        // A page sized to its widest line comes out a few pixels wider or narrower, as our text
        // measures a little differently from pango's: such pages are compared where both are.
        constexpr int size_slack = 4;
        if (std::abs(ours.width - reference->width) > size_slack || std::abs(ours.height - reference->height) > size_slack) {
            detail = std::to_string(ours.width) + "x" + std::to_string(ours.height) + ", the reference " +
                     std::to_string(reference->width) + "x" + std::to_string(reference->height);
        } else {
            // Antialiased edges come out a little differently from cairo's: a pixel differs when
            // a channel is more than a tenth off.
            std::size_t different = 0;
            for (int y = 0; y < std::min(ours.height, reference->height); ++y)
                for (int x = 0; x < std::min(ours.width, reference->width); ++x) {
                    const auto at = static_cast<std::size_t>(y) * ours.width + x;
                    if (masked[at]) continue;
                    const auto mine = at * 3, theirs = (static_cast<std::size_t>(y) * reference->width + x) * 3;
                    for (int c = 0; c < 3; ++c)
                        if (std::abs(ours.rgb[mine + c] - reference->rgb[theirs + c]) > 26) {
                            ++different;
                            break;
                        }
                }
            if (different > 0) detail = std::to_string(different) + " pixels differ";
        }
        if (detail.empty()) return {outcome::passed, drawn.masked().empty() ? "" : "masked"};
        if (!failed.empty()) {
            const fs::path file = failed / (name + ".png");
            fs::create_directories(file.parent_path());
            render_test::save_png(ours, file);
        }
        return {outcome::differs, detail};
    }
}

int main() {
    const fs::path root = environment("SC_LITEHTML_TESTS");
    if (root.empty() || !fs::is_directory(root)) {
        std::cout << "litehtml-tests isn't here (SC_LITEHTML_TESTS): skipped\n";
        return 0;
    }
    const fs::path list_file = environment("SC_UPSTREAM_PAGES"), failed = environment("SC_UPSTREAM_FAILED"),
                   passing_file = environment("SC_UPSTREAM_PASSING");
    const std::string only = environment("SC_UPSTREAM_ONLY");
    const bool all = !environment("SC_UPSTREAM_ALL").empty();

    std::vector<std::string> listed;
    {
        std::ifstream in{list_file};
        for (std::string line; std::getline(in, line);)
            if (!line.empty() && !line.starts_with('#')) listed.push_back(line);
    }
    if (!all && listed.empty()) {
        std::cerr << "no pages listed (SC_UPSTREAM_PAGES: " << list_file << ")\n";
        return 1;
    }

    auto pages = all ? pages_in(root) : listed;
    if (!only.empty()) std::erase_if(pages, [&](const std::string &name) { return name.find(only) == std::string::npos; });

    std::vector<result> results(pages.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < std::max(1u, std::thread::hardware_concurrency()); ++n)
        workers.emplace_back([&] {
            for (std::size_t i; (i = next++) < pages.size();) {
                try {
                    if (!fs::exists(root / pages[i])) results[i] = {outcome::differs, "no such page in litehtml-tests"};
                    else results[i] = run(root, pages[i], failed);
                } catch (const std::exception &error) {
                    results[i] = {outcome::differs, std::string{"threw: "} + error.what()};
                }
            }
        });
    for (auto &worker: workers) worker.join();

    std::size_t masked = 0, left_out = 0, no_reference = 0;
    std::vector<std::string> passed, differ;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto &[what, detail] = results[i];
        if (what == outcome::passed) {
            passed.push_back(pages[i]);
            if (!detail.empty()) ++masked;
        } else if (what == outcome::left_out) ++left_out;
        else if (what == outcome::no_reference) ++no_reference;
        else differ.push_back(pages[i] + ": " + detail);
    }

    std::cout << pages.size() << " pages: " << passed.size() << " drawn as their references (" << masked
              << " with text in other fonts left out), " << differ.size() << " differ, " << left_out
              << " left out (images other than PNGs), " << no_reference << " without a reference\n";
    if (!passing_file.empty()) {
        std::ofstream out{passing_file};
        for (const auto &name: passed) out << name << '\n';
        std::cout << "wrote the " << passed.size() << " pages drawn as their references to " << passing_file << '\n';
    }
    if (all) return 0; // exploring: every page, nothing required
    // A listed page must be drawn as its reference; one left out or without a reference no longer
    // checks anything, so it fails too.
    for (const auto &line: differ) std::cout << "FAILED: " << line << '\n';
    for (std::size_t i = 0; i < pages.size(); ++i)
        if (results[i].what == outcome::left_out || results[i].what == outcome::no_reference)
            std::cout << "FAILED: " << pages[i] << ": not compared\n";
    return passed.size() == pages.size() ? 0 : 1;
}
