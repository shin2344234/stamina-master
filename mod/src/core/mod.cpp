#include "core/mod.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

#include "core/log.h"
#include "core/paths.h"
#include "game/mem.h"
#include "game/stamina.h"
#include "ini_default.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;

    // UnlimitedStamina.ini next to the plugin, [settings] section.
    float ReadSetting(const wchar_t* key, const wchar_t* fallback)
    {
        const std::wstring ini = us::Paths::File(US_INI);
        wchar_t buf[64] = {};
        GetPrivateProfileStringW(L"settings", key, fallback, buf, 64, ini.c_str());
        return static_cast<float>(_wtof(buf));
    }

    // A DMM install is the plugin on its own, because the plugin is all DMM
    // registers, so there is no ini beside it and nothing for anybody to edit.
    // Every default is compiled in and the mod runs correctly without one,
    // which is exactly why the absence is confusing. Write the documented ini
    // out when there is none there. An existing file is never touched, however
    // old or however empty.
    void WriteDefaultIni()
    {
        const std::wstring path = us::Paths::File(US_INI);
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return;

        FILE* f = nullptr;
        const errno_t e = _wfopen_s(&f, path.c_str(), L"wb");
        if (e != 0 || !f)
        {
            LOG("[ini] %ls is not there and could not be written (errno %d). Every default is compiled "
                "in, so the mod still runs; there is just no file to change one in.", US_INI, e);
            return;
        }
        const bool ok = fwrite(kDefaultIni, 1, kDefaultIniSize, f) == kDefaultIniSize;
        fclose(f);
        LOG(ok ? "[ini] no %ls beside the plugin, so one was written with every setting at its default "
                 "and a note on each. Edit it and restart the game."
               : "[ini] %ls was created but not written in full. Delete it and it will be written again.",
            US_INI);
    }

    struct Settings
    {
        bool probe;   // the research report; the only thing this build does
    };

    Settings ReadSettings()
    {
        Settings s;
        s.probe = ReadSetting(L"Probe", L"1") != 0.0f;
        return s;
    }

    DWORD WINAPI Worker(LPVOID)
    {
        WriteDefaultIni();
        const Settings s = ReadSettings();

        LOG("[mod] %s %s for Crimson Desert 2.03.00 (exe 1.0.0.2944). Probe=%d", US_NAME, US_VERSION,
            s.probe ? 1 : 0);
        LOG("[mod] This build changes nothing in the game. It reports what the running tables hold so the "
            "two designs in FEASIBILITY.md can be told apart. Play for a minute or two with a mount, a "
            "climb and a swim, then send the log.");
        LOG("[mod] game image at 0x%p, %zu bytes",
            reinterpret_cast<void*>(us::mem::Game().base), us::mem::Game().size);

        if (!s.probe)
        {
            LOG("[mod] Probe is 0 and this build has nothing else to do, so it is stopping here.");
            return 0;
        }

        int waited = 0;
        bool reported = false;
        while (!g_stop.load())
        {
            if (!reported)
            {
                reported = us::stamina::Probe();
                if (!reported && ++waited == 120)
                    LOG_ERR("[table] neither statusinfo nor skill was loaded after two minutes. The probe "
                            "keeps trying. If this line is the last one in the log, the table machinery "
                            "has changed on this build and no part of the report ran.");
            }
            for (int i = 0; i < 2 && !g_stop.load(); ++i) Sleep(500);
        }
        LOG("[mod] worker stopped");
        return 0;
    }
}

namespace us::Mod
{
    // The game is not the only process that loads this plugin.
    // crashpad_handler.exe does too, with a 671,744-byte image on this build,
    // which is how it is recognised here. That instance names its own log, says
    // why it is doing nothing, and touches nothing.
    static constexpr size_t kMinGameImage = 64ull * 1024 * 1024;

    void Initialize(HMODULE module)
    {
        Paths::Init(module);
        const size_t size = mem::Game().size;
        if (!mem::Game().base || size < kMinGameImage)
        {
            // One fixed name, not one per process id. crashpad_handler.exe
            // starts and exits repeatedly, and naming the file after the pid
            // leaves a new one behind every time with nothing to clear them.
            wchar_t name[96];
            _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s.other", US_FILEBASE);
            Log::ClaimSingle(name);

            wchar_t exe[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            const wchar_t* leaf = wcsrchr(exe, L'\\');
            LOG("[mod] %ls (pid %lu) has a %zu byte image, which is not the game, so nothing is changed "
                "here. The game's own log is %ls.log.", leaf ? leaf + 1 : exe,
                GetCurrentProcessId(), size, US_FILEBASE);
            Log::Shutdown();
            return;
        }
        Log::Claim(US_FILEBASE);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Shutdown(bool processExiting)
    {
        g_stop.store(true);
        if (processExiting)
        {
            Log::Shutdown();
            return;
        }
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        Log::Shutdown();
    }
}
