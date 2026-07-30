#include "golden.hpp"

#include <1bit/io/braille.hpp>
#include "doctest.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

void appendSink(const char* data, size_t len, void* ctx) {
    static_cast<std::string*>(ctx)->append(data, len);
}

std::string pathFor(const std::string& name) {
    return std::string(GOLDEN_DIR) + "/" + name + ".txt";
}

bool updating() {
    const char* e = std::getenv("ONEBIT_UPDATE_GOLDENS");
    return e && *e && std::string(e) != "0";
}

/// First differing line, rendered with a few lines of context either side.
/// A whole-frame dump is unreadable at 70 rows; the neighbourhood of the change
/// is what tells you what broke.
std::string firstDiff(const std::string& want, const std::string& got) {
    std::istringstream a(want), b(got);
    std::string la, lb;
    int line = 0;
    while (true) {
        const bool ha = static_cast<bool>(std::getline(a, la));
        const bool hb = static_cast<bool>(std::getline(b, lb));
        if (!ha && !hb) return "identical";
        ++line;
        if (!ha) return "baseline ended at line " + std::to_string(line);
        if (!hb) return "render ended at line " + std::to_string(line);
        if (la != lb) {
            return "line " + std::to_string(line) + "\n  want: " + la + "\n  got:  " + lb;
        }
    }
}

} // namespace

std::string toBraille(const onebit::IFramebuffer& fb) {
    std::string out;
    onebit::encodeBraille(fb, appendSink, &out);
    return out;
}

void checkGolden(const onebit::IFramebuffer& fb, const std::string& name) {
    const std::string got = toBraille(fb);
    const std::string path = pathFor(name);

    if (updating()) {
        std::ofstream o(path, std::ios::binary);
        REQUIRE_MESSAGE(o.good(), "cannot write baseline " << path);
        o << got;
        return;
    }

    std::ifstream in(path, std::ios::binary);
    // A missing baseline must fail rather than be created, or the first broken
    // render silently becomes the thing everything else is compared against.
    REQUIRE_MESSAGE(in.good(),
                    "missing baseline " << path
                    << " -- regenerate deliberately with ONEBIT_UPDATE_GOLDENS=1 and read the diff");

    std::stringstream buf;
    buf << in.rdbuf();
    const std::string want = buf.str();

    CHECK_MESSAGE(got == want, name << " differs from baseline:\n" << firstDiff(want, got));
}
