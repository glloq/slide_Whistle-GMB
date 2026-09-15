/*
 * tests/tools/gmb_descriptor_dump.cpp — regenerate the reference descriptor
 * fixture from the REAL capability builder.
 *
 *   make -C tests fixture
 *
 * The fixture is checked in and compared byte-for-byte by
 * tests/test_gmb_capabilities.cpp, so the documented example can never drift
 * away from what the firmware actually serves.
 */
#include "../test_gmb_support.h"

#include <cstdio>

int main(int argc, char** argv) {
    const swc::RuntimeConfig cfg = gmbtest::referenceConfig();
    const std::string json = gmbtest::descriptorFor(cfg, /*revision=*/1);
    if (argc > 1) {
        FILE* f = std::fopen(argv[1], "wb");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", argv[1]); return 1; }
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "wrote %s (%zu bytes)\n", argv[1], json.size());
        return 0;
    }
    std::fwrite(json.data(), 1, json.size(), stdout);
    return 0;
}
