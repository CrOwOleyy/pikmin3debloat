#include "image.h"
#include <cstdio>
#include <vector>
#include <fstream>
// Stub pour satisfaire l'éditeur de liens (ParseImage référence Xex2LoadImage pour 'XEX2').
Image Xex2LoadImage(const uint8_t*, size_t) { return {}; }
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    printf("read %zu bytes, e_phnum check...\n", data.size());
    Image img = Image::ParseImage(data.data(), data.size());
    printf("OK base=0x%zx entry=0x%zx size=0x%x sections=%zu\n",
           (size_t)img.base, (size_t)img.entry_point, img.size, img.sections.size());
    for (auto& s : img.sections)
        printf("  %-26s base=0x%zx size=0x%x\n", s.name.c_str(), (size_t)s.base, s.size);
    return 0;
}
