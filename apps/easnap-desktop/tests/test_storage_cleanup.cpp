// Storage cleanup regression: real production deletion and synthetic owned files.
#include "platform/platform_utils.h"

#include <winioctl.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* expression, int line) {
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL line %d: %s (Win32 %lu)\n", line, expression, ::GetLastError());
    }
}
#define CHECK(value) Check((value), #value, __LINE__)

bool WriteSentinel(const std::wstring& path) {
    easnap::platform::UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    constexpr char data[] = "storage-cleanup-sentinel";
    DWORD written = 0;
    return ::WriteFile(file.get(), data, sizeof(data), &written, nullptr) != FALSE &&
           written == sizeof(data);
}

bool SentinelIntact(const std::wstring& path) {
    easnap::platform::UniqueHandle file(::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) return false;
    constexpr char expected[] = "storage-cleanup-sentinel";
    char actual[sizeof(expected)]{};
    DWORD read = 0;
    return ::ReadFile(file.get(), actual, sizeof(actual), &read, nullptr) != FALSE &&
           read == sizeof(actual) && std::memcmp(actual, expected, sizeof(actual)) == 0;
}

bool CreateJunction(const std::wstring& link, const std::wstring& target) {
    if (!::CreateDirectoryW(link.c_str(), nullptr)) return false;
    easnap::platform::UniqueHandle handle(::CreateFileW(link.c_str(), GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return false;
    // Mount-point reparse data uses byte offsets relative to PathBuffer.
    struct MountPointData {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
    };
    const std::wstring substitute = L"\\??\\" + target;
    const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const std::size_t print_bytes = target.size() * sizeof(wchar_t);
    const std::size_t path_bytes = substitute_bytes + print_bytes + 2 * sizeof(wchar_t);
    if (path_bytes > 16000) return false;
    MountPointData header{};
    header.tag = IO_REPARSE_TAG_MOUNT_POINT;
    header.data_length = static_cast<WORD>(8 + path_bytes);
    header.substitute_length = static_cast<WORD>(substitute_bytes);
    header.print_offset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    header.print_length = static_cast<WORD>(print_bytes);
    std::vector<unsigned char> buffer(sizeof(header) + path_bytes, 0);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.data(), substitute_bytes);
    std::memcpy(buffer.data() + sizeof(header) + header.print_offset, target.data(), print_bytes);
    DWORD returned = 0;
    return ::DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
}
void TestPinnedAncestor(const std::wstring& fixture) {
    const std::wstring directory = fixture + L"\\pin-target";
    const std::wstring file = directory + L"\\sentinel.bin";
    const std::wstring renamed = fixture + L"-renamed";
    CHECK(::CreateDirectoryW(directory.c_str(), nullptr) != FALSE);
    CHECK(WriteSentinel(file));
    easnap::platform::UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    easnap::platform::UniqueHandle oplock(::CreateFileW(file.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr));
    CHECK(event.get() != nullptr && oplock.get() != INVALID_HANDLE_VALUE);
    if (event && oplock.get() != INVALID_HANDLE_VALUE) {
        OVERLAPPED pending{};
        pending.hEvent = event.get();
        DWORD bytes = 0;
        const BOOL completed = ::DeviceIoControl(oplock.get(), FSCTL_REQUEST_BATCH_OPLOCK,
            nullptr, 0, nullptr, 0, &bytes, &pending);
        const bool registered = !completed && ::GetLastError() == ERROR_IO_PENDING;
        CHECK(registered);
        if (registered) {
            // A batch oplock pauses the actual production deletion at this file.
            // The break event proves its ancestor guards are already held, so
            // this checks sharing/rename behavior without a timing race or hook.
            bool removed = false;
            std::thread cleanup([&] { removed = easnap::platform::RemoveDirectoryTree(directory); });
            const DWORD wait = ::WaitForSingleObject(event.get(), 5000);
            CHECK(wait == WAIT_OBJECT_0);
            if (wait == WAIT_OBJECT_0) {
                for (const DWORD access : {DWORD{DELETE}, DWORD{GENERIC_WRITE}}) {
                    easnap::platform::UniqueHandle mutator(::CreateFileW(fixture.c_str(), access,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr));
                    const DWORD error = ::GetLastError();
                    CHECK(mutator.get() == INVALID_HANDLE_VALUE);
                    CHECK(error == ERROR_SHARING_VIOLATION);
                }
                const BOOL moved = ::MoveFileExW(fixture.c_str(), renamed.c_str(), 0);
                const DWORD error = ::GetLastError();
                CHECK(moved == FALSE);
                CHECK(error == ERROR_SHARING_VIOLATION);
                if (moved) {
                    // Restore only our exact GUID-owned fixture before releasing
                    // the oplock if an unfixed implementation allowed the move.
                    CHECK(::MoveFileExW(renamed.c_str(), fixture.c_str(), 0) != FALSE);
                }
            }
            oplock.reset(); // Closing acknowledges the break and lets cleanup finish.
            cleanup.join();
            CHECK(removed);
            CHECK(::GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES);
        }
        if (oplock) {
            ::CancelIoEx(oplock.get(), &pending);
            ::GetOverlappedResult(oplock.get(), &pending, &bytes, TRUE);
            oplock.reset();
        }
    }
    ::DeleteFileW(file.c_str());
    ::RemoveDirectoryW(directory.c_str());
}

} // namespace

int main() {
    using easnap::platform::RemoveDirectoryTree;
    const std::wstring storage = easnap::platform::AppDataDirectory();
    GUID guid{};
    wchar_t identifier[40]{};
    if (storage.empty() || FAILED(::CoCreateGuid(&guid)) ||
        ::StringFromGUID2(guid, identifier, 40) == 0) return 1;
    const std::wstring fixture = storage + L"\\cleanup-regression-" + identifier;
    const std::wstring outside = storage + L"-cleanup-regression-" + identifier;
    // Only newly created, absolute, GUID-named test directories are touched.
    // Teardown uses exact file/directory names; it never recursively follows a link.
    if (storage.size() < 3 || storage[1] != L':' || storage[2] != L'\\' ||
        storage.find(L"..") != std::wstring::npos ||
        !::CreateDirectoryW(fixture.c_str(), nullptr)) return 1;
    if (!::CreateDirectoryW(outside.c_str(), nullptr)) {
        ::RemoveDirectoryW(fixture.c_str());
        return 1;
    }
    const std::wstring sentinel = outside + L"\\sentinel.bin";
    const std::wstring nested = outside + L"\\nested";
    const std::wstring nested_sentinel = nested + L"\\sentinel.bin";
    const std::wstring blocked = fixture + L"\\blocked";
    const std::wstring link = blocked + L"\\junction";
    const std::wstring ordinary = fixture + L"\\ordinary";
    const std::wstring child = ordinary + L"\\child";
    const std::wstring normal_file = child + L"\\sentinel.bin";
    const std::wstring locked = fixture + L"\\locked";
    const std::wstring locked_file = locked + L"\\sentinel.bin";
    CHECK(WriteSentinel(sentinel));
    CHECK(::CreateDirectoryW(nested.c_str(), nullptr) != FALSE);
    CHECK(WriteSentinel(nested_sentinel));
    CHECK(::CreateDirectoryW(blocked.c_str(), nullptr) != FALSE);
    CHECK(CreateJunction(link, outside));
    CHECK(!RemoveDirectoryTree(storage));
    CHECK(!RemoveDirectoryTree(outside));
    CHECK(SentinelIntact(sentinel));
    CHECK(!RemoveDirectoryTree(blocked));
    CHECK(SentinelIntact(sentinel));
    CHECK(SentinelIntact(nested_sentinel));
    CHECK(!RemoveDirectoryTree(link));
    CHECK(!RemoveDirectoryTree(link + L"\\nested"));
    CHECK(SentinelIntact(nested_sentinel));
    CHECK(!RemoveDirectoryTree(fixture + L"\\..\\..\\" +
          outside.substr(outside.find_last_of(L'\\') + 1)));
    CHECK(SentinelIntact(sentinel));
    CHECK(::CreateDirectoryW(ordinary.c_str(), nullptr) != FALSE);
    CHECK(::CreateDirectoryW(child.c_str(), nullptr) != FALSE);
    CHECK(WriteSentinel(normal_file));
    CHECK(::SetFileAttributesW(normal_file.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    CHECK(RemoveDirectoryTree(ordinary));
    CHECK(::GetFileAttributesW(ordinary.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(::CreateDirectoryW(locked.c_str(), nullptr) != FALSE);
    CHECK(WriteSentinel(locked_file));
    {
        easnap::platform::UniqueHandle hold(::CreateFileW(locked_file.c_str(), GENERIC_READ,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        CHECK(hold.get() != INVALID_HANDLE_VALUE);
        CHECK(!RemoveDirectoryTree(locked));
    }
    CHECK(SentinelIntact(locked_file));
    CHECK(RemoveDirectoryTree(locked));

    TestPinnedAncestor(fixture);

    // These operations unlink only exact owned entries, even if assertions fail.
    ::RemoveDirectoryW(link.c_str());
    ::RemoveDirectoryW(blocked.c_str());
    ::SetFileAttributesW(normal_file.c_str(), FILE_ATTRIBUTE_NORMAL);
    ::DeleteFileW(normal_file.c_str());
    ::RemoveDirectoryW(child.c_str());
    ::RemoveDirectoryW(ordinary.c_str());
    ::DeleteFileW(locked_file.c_str());
    ::RemoveDirectoryW(locked.c_str());
    CHECK(::RemoveDirectoryW(fixture.c_str()) != FALSE);
    ::DeleteFileW(nested_sentinel.c_str());
    ::RemoveDirectoryW(nested.c_str());
    ::DeleteFileW(sentinel.c_str());
    CHECK(::RemoveDirectoryW(outside.c_str()) != FALSE);
    std::printf("Storage cleanup: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
