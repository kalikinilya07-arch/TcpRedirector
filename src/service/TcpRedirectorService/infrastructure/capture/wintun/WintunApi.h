#pragma once

/**
 * @file WintunApi.h
 * @brief WP8 — Динамический загрузчик wintun.dll с таблицей указателей на функции.
 *
 * ЧТО ЭТО:
 *   Тонкая RAII-обёртка вокруг LoadLibraryExW/GetProcAddress для wintun.dll.
 *   Не создаёт адаптеров, не запускает сессий, не пампит пакеты — этим
 *   занимаются WintunAdapter / WintunSession, использующие эту таблицу.
 *
 * ГДЕ ИЩЕТ DLL:
 *   По умолчанию — <exeDir>\.bin\wintun\<arch>\wintun.dll, где <arch> =
 *   "x64" при sizeof(void*)==8, иначе "x86".  Путь берётся из
 *   paths::GetBinDirectoryW() (см. AppPaths.h).  Можно передать явный путь.
 *
 * СОБСТВЕННОСТЬ:
 *   Каждый экземпляр WintunApi владеет собственным HMODULE и вызывает
 *   FreeLibrary в деструкторе.  Экземпляры не копируются; расшариваются
 *   через std::shared_ptr, поскольку WintunAdapter / WintunSession должны
 *   удерживать API живым, пока живы они.
 *
 * ОШИБКИ:
 *   Load() возвращает пустой shared_ptr при провале и заполняет *outError
 *   человекочитаемым сообщением с GetLastError.  Ни одного throw.
 *
 * WP8 не подключает Logger — эта задача WP10 (façade).  Здесь ошибки
 * только возвращаются через out-параметр.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

// Через AdditionalIncludeDirectories подключён каталог external/, что
// делает "wintun/wintun.h" каноническим публичным путём.  Не используем
// относительные "../../../.." — они хрупкие при переносе.
#include "wintun/wintun.h"

namespace tcp_redirector {
namespace infrastructure {
namespace capture {
namespace wintun {

/**
 * @brief Таблица указателей на экспортируемые функции wintun.dll + владение HMODULE.
 *
 * Поля Create/Open/Close/... — публичные (это, по сути, POD-таблица
 * функторов).  Использование:
 *
 *     std::string err;
 *     auto api = WintunApi::Load(&err);
 *     if (!api) { LOG_ERROR(err); return false; }
 *     auto h = api->CreateAdapter(L"MyTun", L"MyApp", nullptr);
 *     ...
 */
class WintunApi {
public:
    /**
     * @brief Загрузить wintun.dll из стандартного места (<exeDir>\.bin\wintun\<arch>\wintun.dll).
     * @param outError [out, обязателен] Диагностическое сообщение при провале.
     * @return shared_ptr на API-таблицу либо пустой shared_ptr при неудаче.
     */
    static std::shared_ptr<WintunApi> Load(std::string* outError);

    /**
     * @brief Загрузить wintun.dll из явно указанного пути.
     * @param dllPath   Полный путь к wintun.dll (UTF-16).
     * @param outError  [out, обязателен] Диагностическое сообщение при провале.
     */
    static std::shared_ptr<WintunApi> LoadFromPath(const std::wstring& dllPath,
                                                    std::string* outError);

    ~WintunApi();

    WintunApi(const WintunApi&) = delete;
    WintunApi& operator=(const WintunApi&) = delete;
    WintunApi(WintunApi&&) = delete;
    WintunApi& operator=(WintunApi&&) = delete;

    // Публичные указатели на функции wintun.dll.
    // Все поля инициализируются в Load(); если Load() успешен, ни одно
    // из них не может быть nullptr.
    WINTUN_CREATE_ADAPTER_FUNC              CreateAdapter           = nullptr;
    WINTUN_OPEN_ADAPTER_FUNC                OpenAdapter             = nullptr;
    WINTUN_CLOSE_ADAPTER_FUNC               CloseAdapter            = nullptr;
    WINTUN_DELETE_DRIVER_FUNC               DeleteDriver            = nullptr;
    WINTUN_GET_ADAPTER_LUID_FUNC            GetAdapterLUID          = nullptr;
    WINTUN_GET_RUNNING_DRIVER_VERSION_FUNC  GetRunningDriverVersion = nullptr;
    WINTUN_START_SESSION_FUNC               StartSession            = nullptr;
    WINTUN_END_SESSION_FUNC                 EndSession              = nullptr;
    WINTUN_GET_READ_WAIT_EVENT_FUNC         GetReadWaitEvent        = nullptr;
    WINTUN_RECEIVE_PACKET_FUNC              ReceivePacket           = nullptr;
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC      ReleaseReceivePacket    = nullptr;
    WINTUN_ALLOCATE_SEND_PACKET_FUNC        AllocateSendPacket      = nullptr;
    WINTUN_SEND_PACKET_FUNC                 SendPacket              = nullptr;

    /**
     * @brief Версия загруженного драйвера wintun (major<<16 | minor).
     * @return 0, если недоступно / вызов не удался.
     */
    uint32_t RunningDriverVersion() const;

    /**
     * @brief Путь, откуда фактически загружена DLL (для логов/диагностики).
     */
    const std::wstring& DllPath() const { return m_path; }

    /**
     * @brief Резолвит ожидаемый путь до wintun.dll (<exeDir>\.bin\wintun\<arch>\wintun.dll).
     *
     * Отдельный статический метод, чтобы WP7-preflight и WP8-loader
     * согласованно смотрели в одно место.  Может бросить runtime_error
     * из GetExecutableDirectoryW().
     */
    static std::wstring DefaultDllPath();

private:
    WintunApi() = default;

    HMODULE       m_module = nullptr;
    std::wstring  m_path;
};

} // namespace wintun
} // namespace capture
} // namespace infrastructure
} // namespace tcp_redirector
