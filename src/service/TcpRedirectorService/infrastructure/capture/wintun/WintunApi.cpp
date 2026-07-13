/**
 * @file WintunApi.cpp
 * @brief WP8 — реализация загрузчика wintun.dll.
 */

#include "WintunApi.h"

#include "../../paths/AppPaths.h"

#include <sstream>
#include <string>
#include <type_traits>   // std::remove_reference_t

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

namespace {

/**
 * @brief Мини-UTF16→UTF8 через WideCharToMultiByte (без исключений).
 *
 * Используется, только когда нужно вклеить путь в out-строку ошибки;
 * при провале возвращаем "<invalid-utf16-path>".  Логгер не подключаем —
 * это задача WP10.
 */
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0,
                                     w.data(), static_cast<int>(w.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "<invalid-utf16-path>";
    std::string out(static_cast<size_t>(needed), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0,
                                      w.data(), static_cast<int>(w.size()),
                                      out.data(), needed, nullptr, nullptr);
    if (written <= 0) return "<invalid-utf16-path>";
    return out;
}

/**
 * @brief Сформировать сообщение "wintun.dll not loaded from <path>: GLE=<code>".
 */
std::string FormatLoadError(const std::wstring& path, DWORD gle,
                             const char* stage) {
    std::ostringstream oss;
    oss << "wintun.dll " << stage << " failed at '"
        << WideToUtf8(path) << "': GLE=" << gle;
    return oss.str();
}

} // namespace

// ============================================================================
// WintunApi::DefaultDllPath
// ============================================================================

std::wstring WintunApi::DefaultDllPath() {
    // paths::GetBinDirectoryW() может кинуть runtime_error; пропускаем
    // это исключение наверх — Load() ловит и превращает в out-error.
    const std::wstring binDir = paths::GetBinDirectoryW();

    // В x64-сборке — "x64"; в x86 — "x86".  Проект нативно x64, но
    // определяем по sizeof(void*), чтобы не завязываться на _M_X64.
    const wchar_t* arch = (sizeof(void*) == 8) ? L"x64" : L"x86";

    std::wstring out;
    out.reserve(binDir.size() + 32);
    out.append(binDir);
    out.append(L"wintun\\");
    out.append(arch);
    out.append(L"\\wintun.dll");
    return out;
}

// ============================================================================
// WintunApi::Load / LoadFromPath
// ============================================================================

std::shared_ptr<WintunApi> WintunApi::Load(std::string* outError) {
    std::wstring path;
    try {
        path = DefaultDllPath();
    } catch (const std::exception& e) {
        if (outError) {
            *outError = std::string("Cannot resolve wintun.dll path: ") + e.what();
        }
        return nullptr;
    }
    return LoadFromPath(path, outError);
}

std::shared_ptr<WintunApi> WintunApi::LoadFromPath(const std::wstring& dllPath,
                                                    std::string* outError) {
    // Явный проверяющий out-параметр: мы контрактно обещаем заполнить
    // *outError при провале, поэтому запрещаем его отсутствие.
    // (Функция стремится быть «безболезненной» — не бросает.)
    static std::string s_dummy;
    std::string& err = outError ? *outError : s_dummy;
    err.clear();

    // LOAD_WITH_ALTERED_SEARCH_PATH заставляет Windows брать зависимости
    // DLL из её собственного каталога (полезно, если wintun.dll в будущем
    // получит побочные зависимости).  Требует абсолютного пути — у нас
    // именно такой, из GetExecutableDirectoryW().
    HMODULE mod = LoadLibraryExW(dllPath.c_str(), nullptr,
                                  LOAD_WITH_ALTERED_SEARCH_PATH);
    if (mod == nullptr) {
        DWORD gle = GetLastError();
        err = FormatLoadError(dllPath, gle, "LoadLibraryExW");
        return nullptr;
    }

    // Ниже — new WintunApi, но без std::make_shared, потому что конструктор
    // приватный.  make_shared требует public-конструктора; для WintunApi мы
    // используем shared_ptr<WintunApi>(new WintunApi(), ...) + private ctor +
    // друзей не объявляем: обходим кастомным deleter'ом.
    struct WintunApiDeleter {
        void operator()(WintunApi* p) const noexcept { delete p; }
    };

    // Инстанс создаём заранее, чтобы поля заполнялись прямо в него.
    // Владение сразу передаём shared_ptr — при любом раннем return
    // отработает FreeLibrary через деструктор WintunApi.
    std::shared_ptr<WintunApi> api(new WintunApi(), WintunApiDeleter{});
    api->m_module = mod;
    api->m_path   = dllPath;

    // Ресолвим все нужные экспортированные функции.  Если хотя бы одна
    // отсутствует — считаем DLL несовместимой версии и откатываемся.
    // Используем лямбду вместо локальной struct-шаблона (C2892: templates
    // не разрешены как члены локальных классов).
    const char* missing = nullptr;
    auto resolve = [&](auto& dst, const char* name) {
        if (missing) return;
        using Fn = std::remove_reference_t<decltype(dst)>;
        dst = reinterpret_cast<Fn>(GetProcAddress(mod, name));
        if (!dst) missing = name;
    };

    resolve(api->CreateAdapter,           "WintunCreateAdapter");
    resolve(api->OpenAdapter,             "WintunOpenAdapter");
    resolve(api->CloseAdapter,            "WintunCloseAdapter");
    resolve(api->DeleteDriver,            "WintunDeleteDriver");
    resolve(api->GetAdapterLUID,          "WintunGetAdapterLUID");
    resolve(api->GetRunningDriverVersion, "WintunGetRunningDriverVersion");
    resolve(api->StartSession,            "WintunStartSession");
    resolve(api->EndSession,              "WintunEndSession");
    resolve(api->GetReadWaitEvent,        "WintunGetReadWaitEvent");
    resolve(api->ReceivePacket,           "WintunReceivePacket");
    resolve(api->ReleaseReceivePacket,    "WintunReleaseReceivePacket");
    resolve(api->AllocateSendPacket,      "WintunAllocateSendPacket");
    resolve(api->SendPacket,              "WintunSendPacket");

    if (missing) {
        std::ostringstream oss;
        oss << "wintun.dll loaded but missing export '" << missing
            << "' (path: " << WideToUtf8(dllPath)
            << ") — DLL likely from an incompatible version";
        err = oss.str();
        // api будет разрушен по выходу; FreeLibrary вызовется в деструкторе.
        return nullptr;
    }

    return api;
}

// ============================================================================
// WintunApi::~WintunApi
// ============================================================================

WintunApi::~WintunApi() {
    // FreeLibrary безопасен для nullptr, но проверим явно ради ясности.
    if (m_module) {
        // Игнорируем результат: разгрузка либо удастся, либо WinAPI выставит
        // GLE, но нам нечего с ним делать — процесс уходит.
        FreeLibrary(m_module);
        m_module = nullptr;
    }
}

// ============================================================================
// WintunApi::RunningDriverVersion
// ============================================================================

uint32_t WintunApi::RunningDriverVersion() const {
    if (!GetRunningDriverVersion) return 0u;
    // API возвращает 0 при неудаче (см. wintun.h); мы это транслируем «как есть».
    return static_cast<uint32_t>(GetRunningDriverVersion());
}

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
