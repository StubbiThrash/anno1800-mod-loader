#include "interface.h"

#include "version.h"

#if defined(INTERNAL_ENABLED)
#include "libs/internal/debuggable/include/debuggable.h"
#endif
#include "libs/external-file-loader/include/external-file-loader.h"
#include "libs/external-file-loader/include/mod_manager.h"

#include "nlohmann/json.hpp"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include <WinInet.h>
#include <Windows.h>

#pragma comment(lib, "Wininet.lib")

#include <cstdio>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

// Global events instance
static Events events;

static std::string GetLatestVersion()
{
    char data[1024] = {0};

    // initialize WinInet
    HINTERNET hInternet =
        ::InternetOpen(TEXT("Anno 1800 Mod Loader"), INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (hInternet != nullptr) {
        // 5 second timeout for now
        DWORD timeout = 5 * 1000;
        auto  result  = InternetSetOption(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout,
                                        sizeof(timeout));
        InternetSetOption(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOption(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));

        // open HTTP session
        HINTERNET hConnect =
            ::InternetConnectA(hInternet, "xforce.dev", INTERNET_DEFAULT_HTTPS_PORT, 0, 0,
                               INTERNET_SERVICE_HTTP, 0, 0);
        if (hConnect != nullptr) {
            HINTERNET hRequest = ::HttpOpenRequestA(
                hConnect, "GET", "/anno1800-mod-loader/latest.json", 0, 0, 0,
                INTERNET_FLAG_SECURE | INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_RELOAD, 0);
            if (hRequest != nullptr) {
                // send request
                const auto was_sent = ::HttpSendRequest(hRequest, nullptr, 0, nullptr, 0);

                if (was_sent) {
                    DWORD status_code      = 0;
                    DWORD status_code_size = sizeof(status_code);
                    HttpQueryInfo(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                                  &status_code, &status_code_size, 0);

                    if (status_code != 200) {
                        throw std::runtime_error("Failed to read version info");
                    }
                    for (;;) {
                        // reading data
                        DWORD      bytes_read;
                        const auto was_read =
                            ::InternetReadFile(hRequest, data, sizeof(data) - 1, &bytes_read);

                        // break cycle if error or end
                        if (was_read == FALSE || bytes_read == 0) {
                            if (was_read == FALSE) {
                                throw std::runtime_error("Failed to read version info");
                            }
                            break;
                        }

                        // saving result
                        data[bytes_read] = 0;
                    }
                }
                ::InternetCloseHandle(hRequest);
            }
            ::InternetCloseHandle(hConnect);
        }
        ::InternetCloseHandle(hInternet);
    }

    return data;
}

HANDLE CreateFileW_S(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                     LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                     DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (std::wstring(lpFileName).find(L"assets.xml") != std::wstring::npos) {
        __debugbreak();
    }
    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

FARPROC GetProcAddress_S(HMODULE hModule, LPCSTR lpProcName)
{
    // A call to GetProcAddres indicates that all the copy protection is done
    // and we started loading all the dynamic imports
    // those would have usually been in the import table.
    // This means we are ready to do some hooking
    // But only do hooking once.
    static std::once_flag flag1;
    std::call_once(flag1, []() { events.DoHooking(); });

    /*  if ((int)lpProcName > 0x1000 && lpProcName == std::string("CreateFileW")) {
          return (FARPROC)CreateFileW_S;
      }*/

    if ((uintptr_t)lpProcName > 0x1000) {
        auto procs = events.GetProcAddress(lpProcName);
        for (auto& proc : procs) {
            if (proc > 0) {
                return (FARPROC)proc;
            }
        }
    }

    return GetProcAddress(hModule, lpProcName);
}

bool set_import(const std::string& name, uintptr_t func);

bool set_import(const std::string& name, uintptr_t func)
{
    static uint64_t image_base = 0;

    if (image_base == 0) {
        image_base = uint64_t(GetModuleHandleA(NULL));
    }

    bool result    = false;
    auto dosHeader = (PIMAGE_DOS_HEADER)(image_base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        throw std::runtime_error("Invalid DOS Signature");
    }

    auto header = (PIMAGE_NT_HEADERS)((image_base + (dosHeader->e_lfanew * sizeof(char))));
    if (header->Signature != IMAGE_NT_SIGNATURE) {
        throw std::runtime_error("Invalid NT Signature");
    }

    // BuildImportTable
    PIMAGE_DATA_DIRECTORY directory =
        &header->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (directory->Size > 0) {
        auto importDesc = (PIMAGE_IMPORT_DESCRIPTOR)(header->OptionalHeader.ImageBase
                                                     + directory->VirtualAddress);
        for (; !IsBadReadPtr(importDesc, sizeof(IMAGE_IMPORT_DESCRIPTOR)) && importDesc->Name;
             importDesc++) {
            wchar_t      buf[0xFFF] = {};
            auto         name2      = (LPCSTR)(header->OptionalHeader.ImageBase + importDesc->Name);
            std::wstring sname;
            size_t       converted;
            mbstowcs_s(&converted, buf, name2, sizeof(buf));
            sname       = buf;
            auto csname = sname.c_str();

            HMODULE handle = LoadLibraryW(csname);

            if (handle == nullptr) {
                SetLastError(ERROR_MOD_NOT_FOUND);
                break;
            }

            auto* thunkRef =
                (uintptr_t*)(header->OptionalHeader.ImageBase + importDesc->OriginalFirstThunk);
            auto* funcRef = (FARPROC*)(header->OptionalHeader.ImageBase + importDesc->FirstThunk);

            if (!importDesc->OriginalFirstThunk) // no hint table
            {
                thunkRef = (uintptr_t*)(header->OptionalHeader.ImageBase + importDesc->FirstThunk);
            }

            for (; *thunkRef, *funcRef; thunkRef++, (void)funcRef++) {
                if (!IMAGE_SNAP_BY_ORDINAL(*thunkRef)) {
                    std::string import =
                        (LPCSTR)
                        & ((PIMAGE_IMPORT_BY_NAME)(header->OptionalHeader.ImageBase + (*thunkRef)))
                              ->Name;

                    if (import == name) {
                        DWORD oldProtect;
                        VirtualProtect((void*)funcRef, sizeof(FARPROC), PAGE_EXECUTE_READWRITE,
                                       &oldProtect);

                        *funcRef = (FARPROC)func;

                        VirtualProtect((void*)funcRef, sizeof(FARPROC), oldProtect, &oldProtect);
                        result = true;
                    }
                }
            }
        }
    }
    return result;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hModule);

#if defined(INTERNAL_ENABLED)
            EnableDebugging(events);
#endif

            std::wstring command_line(GetCommandLineW());
            if (command_line.find(L"gamelauncher_wait_handle") != std::wstring::npos
                && command_line.find(L"upc_uplay_id") != std::wstring::npos) {
                return TRUE;
            }

            // Version Check
            events.DoHooking.connect([]() {
                // Let's start loading the list of files we want to have
                HMODULE module;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                           | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPWSTR)&EnableExtenalFileLoading, &module)) {
                    WCHAR path[0x7FFF] = {}; // Support for long paths, in theory
                    GetModuleFileNameW(module, path, sizeof(path));
                    fs::path dll_file(path);
                    try {
                        auto logs_parent    = fs::canonical(dll_file.parent_path() / ".." / "..");
                        auto logs_directory = logs_parent / "logs";

                        fs::create_directories(logs_directory);

                        // Set the default logger to file logger
                        auto file_logger = spdlog::basic_logger_mt(
                            "default", (logs_directory / "mod-loader.log").wstring());
                        spdlog::set_default_logger(file_logger);

                        file_logger->sinks().push_back(
                            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

                        spdlog::flush_on(spdlog::level::info);
                        spdlog::set_pattern("[%Y-%m-%d %T.%e] [%^%l%$] %v");
                    } catch (const fs::filesystem_error& e) {
                        // TODO(alexander): Logs
                        return;
                    }
                } else {
                    spdlog::error("Failed to get current module directory {}", GetLastError());
                    return;
                }

                try {
                    auto        body        = GetLatestVersion();
                    const auto& data        = nlohmann::json::parse(body);
                    const auto& version_str = data["version"].get<std::string>();

                    static int32_t current_version[3] = {VERSION_MAJOR, VERSION_MINOR,
                                                         VERSION_REVISION};

                    int32_t latest_version[3] = {0};
                    std::sscanf(version_str.c_str(), "%d.%d.%d", &latest_version[0],
                                &latest_version[1], &latest_version[2]);

                    if (std::lexicographical_compare(current_version, current_version + 3,
                                                     latest_version, latest_version + 3)) {
                        std::string msg = "Verion " + version_str + " of "
                                          + VER_FILE_DESCRIPTION_STR
                                          + " is available for download.\n\n";
                        msg.append("Do you want to go to the release page on GitHub?\n(THIS IS "
                                   "HIGHLY RECOMMENDED!!!)");

                        if (MessageBoxA(NULL, msg.c_str(), VER_FILE_DESCRIPTION_STR,
                                        MB_ICONQUESTION | MB_YESNO | MB_SYSTEMMODAL)
                            == IDYES) {
                            auto result = ShellExecuteA(
                                nullptr, "open",
                                "https://github.com/xforce/anno1800-mod-loader/releases/latest",
                                nullptr, nullptr, SW_SHOWNORMAL);
                            result = result;
                            TerminateProcess(GetCurrentProcess(), 0);
                        }
                    }
                } catch (...) {
                    // TODO(alexander): Logging
                }
            });

            EnableExtenalFileLoading(events);
            // TODO(alexander): Add code that can load other dll libraries here
            // that offer more features later on
            set_import("GetProcAddress", (uintptr_t)GetProcAddress_S);
        }
        case DLL_THREAD_ATTACH:
            break;
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_DETACH: {
            FreeConsole();
            ModManager::instance().Shutdown();
        } break;
    }
    return TRUE;
}
