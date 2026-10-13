// Easnap unit tests - what the shipped executable needs before it can start.
//
// Windows N and KN editions have no Media Foundation until the Media Feature
// Pack is installed. Easnap starts there only while those libraries are bound
// on first use, not by the loader. This reads the import tables of the built
// Easnap.exe, given as the first argument, so that a change to the link
// options cannot silently take that away.

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

std::string Lower(const char* text) {
    std::string name(text);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return name;
}

/// The library names of one import directory of an image mapped at `base`.
std::vector<std::string> Libraries(const BYTE* base, DWORD directory) {
    std::vector<std::string> names;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || headers->Signature != IMAGE_NT_SIGNATURE ||
        headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
        return names;
    }
    const IMAGE_DATA_DIRECTORY& entry = headers->OptionalHeader.DataDirectory[directory];
    if (entry.VirtualAddress == 0) { return names; }
    if (directory == IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT) {
        for (auto* item = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + entry.VirtualAddress);
             item->DllNameRVA != 0; ++item) {
            names.push_back(Lower(reinterpret_cast<const char*>(base + item->DllNameRVA)));
        }
    } else {
        for (auto* item = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + entry.VirtualAddress);
             item->Name != 0; ++item) {
            names.push_back(Lower(reinterpret_cast<const char*>(base + item->Name)));
        }
    }
    return names;
}

bool Contains(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_executable_imports <Easnap.exe>\n");
        return 2;
    }
    // Mapped as an image and never run: the directories are then at their
    // addresses relative to the start of the mapping.
    HMODULE image = ::LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    CHECK(image != nullptr);
    if (image == nullptr) { return 1; }
    const BYTE* base = reinterpret_cast<const BYTE*>(reinterpret_cast<ULONG_PTR>(image) & ~static_cast<ULONG_PTR>(3));

    const std::vector<std::string> loaded_at_start = Libraries(base, IMAGE_DIRECTORY_ENTRY_IMPORT);
    const std::vector<std::string> loaded_on_use = Libraries(base, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    CHECK(Contains(loaded_at_start, "kernel32.dll") && Contains(loaded_at_start, "user32.dll"));  // The tables were read.

    // Nothing of Media Foundation may be needed to start.
    for (const std::string& name : loaded_at_start) {
        const bool media_foundation = name.rfind("mf", 0) == 0 || name == "evr.dll";
        if (media_foundation) { std::fprintf(stderr, "needed at start: %s\n", name.c_str()); }
        CHECK(!media_foundation);
    }
    // What the recorder and the exporters call is bound when first used.
    CHECK(Contains(loaded_on_use, "mfplat.dll"));
    CHECK(Contains(loaded_on_use, "mfreadwrite.dll"));

    ::FreeLibrary(image);
    std::printf("executable imports: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
