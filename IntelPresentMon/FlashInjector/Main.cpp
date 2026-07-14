#include "../CommonUtilities/win/WinAPI.h"
#include "../CommonUtilities/str/String.h"
#include "../CommonUtilities/win/Utilities.h"
#include "../CommonUtilities/win/Event.h"
#include "CliOptions.h"

#include <set>
#include <unordered_set>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <format>
#include <atomic>

#include "LibraryInject.h"
#include "Logging.h"

namespace stdfs = std::filesystem;
using namespace pmon::util;
using namespace std::literals;

// null logger factory to satisfy linking requirements for CommonUtilities
namespace pmon::util::log
{
    std::shared_ptr<class IChannel> GetDefaultChannel() noexcept
    {
        return {};
    }
}

int main(int argc, char** argv)
{
    try {
        // Initial logging
        LOGI << "Injector process started" << std::endl;

        // Initialize arguments
        if (auto res = clio::Options::Init(argc, argv, true)) {
            return *res;
        }
        auto& opts = clio::Options::Get();

        stdfs::path injectorPath;
        {
            std::vector<char> buffer(MAX_PATH);
            auto size = GetModuleFileNameA(NULL, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0) {
                LOGE << "Failed to get this executable path." << std::endl;;
            }
            injectorPath = std::string(buffer.begin(), buffer.begin() + size);
            injectorPath = injectorPath.parent_path();
        }

        // DLL to inject
        const stdfs::path libraryPath = injectorPath / std::format("FlashInjectorLibrary-{}.dll", PM_BUILD_PLATFORM);

        // This injector only attaches to processes whose architecture matches its own:
        // the injected library runs inside the target and the remote-thread LoadLibrary
        // technique requires a matching kernel32 base. On Windows-on-ARM the kernel spawns
        // one injector per architecture (native ARM64 plus emulated x64/x86) so that every
        // possible target is covered. Map our own build platform to a process architecture.
        const auto ourArchitecture = [] {
            const std::string platform = PM_BUILD_PLATFORM;
            if (platform == "Win32") return win::ProcessArchitecture::x86;
            if (platform == "x64")   return win::ProcessArchitecture::x64;
            if (platform == "ARM64") return win::ProcessArchitecture::Arm64;
            return win::ProcessArchitecture::Unknown;
        }();

        if (!stdfs::exists(libraryPath)) {
            LOGE << "Cannot find library: " << libraryPath << std::endl;;
            exit(1);
        }

        LOGI << "Waiting for processes that match executable name..." << std::endl;

        std::mutex targetModuleNameMtx;
        std::string targetModuleName;
        // thread whose sole job is to read from stdin without blocking the main thread
        std::thread{ [&] {
            std::string line;
            while (true) {
                std::getline(std::cin, line);
                std::lock_guard lk{ targetModuleNameMtx };
                targetModuleName = str::ToLower(line);
            }
        } }.detach();

        // keep a set of processes already attached so we don't attempt multiple attachments per process
        std::unordered_set<DWORD> processesAttached;
        while (true) {
            // atomic load target name and skip if empty string
            const auto tgt = [&] { std::lock_guard lk{ targetModuleNameMtx }; return targetModuleName; }();
            if (!tgt.empty()) {
                for (auto&& [processId, processName] : LibraryInject::GetProcessNames()) {
                    const auto processNameLower = str::ToLower(processName);
                    if (processNameLower == tgt && !processesAttached.contains(processId)) {
                        auto hProcTarget = win::OpenProcess(processId, PROCESS_QUERY_LIMITED_INFORMATION);
                        // Only attach when the target's architecture positively matches ours. An
                        // Unknown result (unrecognized machine, or an unmapped build platform)
                        // must never count as a match, or we would inject a mismatched-ABI DLL.
                        const auto targetArchitecture = win::GetProcessArchitecture(hProcTarget);
                        if (targetArchitecture != win::ProcessArchitecture::Unknown
                            && targetArchitecture == ourArchitecture) {
                            LibraryInject::Attach(processId, libraryPath);
                            LOGI << "    Injected DLL to process with PID: " << processId << std::endl;
                            processesAttached.insert(processId);
                            // inform kernel of attachment so it can connect the action client
                            std::cout << processId << std::endl;
                        }
                    }
                }
            }
            // check for new processes only every N ms to reduce CPU load
            std::this_thread::sleep_for(40ms);
        }

        return 0;
    }
    catch (const std::exception& e) {
        LOGE << "Exception in Main: " << e.what() << std::endl;
        return -1;
    }
    catch (...) {
        LOGE << "Exception in Main: Unidentified error" << std::endl;
        return -1;
    }
}