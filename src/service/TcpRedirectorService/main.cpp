#include <windows.h>
#include "adapters/driving/ServiceMain.h"

tcp_redirector::service::TcpRedirectorService g_Service;

//
// Service Control Handler  (M10: correct LPHANDLER_FUNCTION_EX signature)
//
DWORD WINAPI ServiceControlHandlerEx(DWORD controlCode,
                                      DWORD /*eventType*/,
                                      LPVOID /*eventData*/,
                                      LPVOID /*context*/) {
    switch (controlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_Service.ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        g_Service.Stop();
        g_Service.ReportStatus(SERVICE_STOPPED);
        break;
    case SERVICE_CONTROL_INTERROGATE:
        g_Service.ReportStatus(
            g_Service.IsRunning() ? SERVICE_RUNNING : SERVICE_STOPPED);
        break;
    }
    return NO_ERROR;
}

//
// Service Entry Point (M5: wrapped in try/catch)
//
VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    try {
        SERVICE_STATUS_HANDLE statusHandle = RegisterServiceCtrlHandlerExW(
            L"TcpRedirectorService",
            ServiceControlHandlerEx,   // M10: no cast needed — correct signature
            NULL);

        if (!statusHandle) return;

        g_Service.SetServiceStatusHandle(statusHandle);
        g_Service.ReportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

        if (!g_Service.Initialize()) {
            g_Service.ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
            return;
        }

        g_Service.ReportStatus(SERVICE_RUNNING);
        g_Service.Run();
        g_Service.ReportStatus(SERVICE_STOPPED);
    } catch (const std::exception& e) {
        // Logger IS available at this point (initialized inside Initialize())
        // Access via global g_Service if needed; fallback to stderr.
        fprintf(stderr, "ServiceMain fatal error: %s\n", e.what());
        g_Service.ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    } catch (...) {
        fprintf(stderr, "ServiceMain fatal unknown error\n");
        g_Service.ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    }
}

//
// Console mode entry (M5: wrapped in try/catch)
//
int main(int argc, char* argv[]) {
    // Disable stdout buffering for real-time console output
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    try {
        if (argc > 1) {
            std::string arg = argv[1];
            if (arg == "--install") {
                SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
                if (scm) {
                    wchar_t path[MAX_PATH];
                    GetModuleFileNameW(NULL, path, MAX_PATH);
                    // Quote the ImagePath. Without quotes an install path that
                    // contains a space (e.g. "C:\Program Files\TcpRedirector\...")
                    // produces an unquoted service path: SCM would try to launch
                    // "C:\Program.exe" first (unquoted-service-path issue) and the
                    // service could fail to start. Quoting makes it unambiguous.
                    std::wstring quotedPath = L"\"";
                    quotedPath += path;
                    quotedPath += L"\"";
                    SC_HANDLE service = CreateServiceW(scm,
                        L"TcpRedirectorService",
                        L"TcpRedirector Service",
                        SERVICE_ALL_ACCESS,
                        SERVICE_WIN32_OWN_PROCESS,
                        SERVICE_AUTO_START,
                        SERVICE_ERROR_NORMAL,
                        quotedPath.c_str(),
                        NULL, NULL, NULL, NULL, NULL);
                    if (service) {
                        printf("Service installed successfully\n");

                        // Auto-recovery: настраиваем SCM failure actions, чтобы
                        // служба АВТОМАТИЧЕСКИ перезапускалась после любого
                        // аварийного завершения процесса (напр. access violation).
                        // Без этого падение оставляет службу в состоянии Stopped
                        // (exit 1067) и она больше не поднимается сама.
                        // Политика: 3 попытки рестарта с задержкой 5 с / 10 с / 30 с,
                        // счётчик ошибок сбрасывается через 1 час безаварийной работы.
                        SC_ACTION actions[3];
                        actions[0].Type  = SC_ACTION_RESTART; actions[0].Delay =  5000;
                        actions[1].Type  = SC_ACTION_RESTART; actions[1].Delay = 10000;
                        actions[2].Type  = SC_ACTION_RESTART; actions[2].Delay = 30000;

                        SERVICE_FAILURE_ACTIONSW fa = {};
                        fa.dwResetPeriod = 3600;           // 1 час (секунды)
                        fa.lpRebootMsg   = NULL;
                        fa.lpCommand     = NULL;
                        fa.cActions      = 3;
                        fa.lpsaActions   = actions;
                        if (ChangeServiceConfig2W(service,
                                SERVICE_CONFIG_FAILURE_ACTIONS, &fa)) {
                            printf("Auto-recovery configured (restart on crash: 5s/10s/30s)\n");
                        } else {
                            printf("Warning: failed to set auto-recovery: %lu\n",
                                   GetLastError());
                        }

                        // Считать крашем и ненулевой exit code (не только аварийное
                        // завершение процесса), чтобы failure actions срабатывали
                        // и при SERVICE_STOPPED с ERROR_SERVICE_SPECIFIC_ERROR.
                        SERVICE_FAILURE_ACTIONS_FLAG faFlag = {};
                        faFlag.fFailureActionsOnNonCrashFailures = TRUE;
                        ChangeServiceConfig2W(service,
                            SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &faFlag);

                        CloseServiceHandle(service);
                    } else {
                        printf("Failed to install service: %lu\n", GetLastError());
                    }
                    CloseServiceHandle(scm);
                }
                return 0;
            }
            if (arg == "--uninstall") {
                SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
                if (scm) {
                    SC_HANDLE service = OpenServiceW(scm,
                        L"TcpRedirectorService",
                        SERVICE_STOP | DELETE);
                    if (service) {
                        SERVICE_STATUS status;
                        ControlService(service, SERVICE_CONTROL_STOP, &status);
                        DeleteService(service);
                        printf("Service uninstalled successfully\n");
                        CloseServiceHandle(service);
                    }
                    CloseServiceHandle(scm);
                }
                return 0;
            }
            if (arg == "--console") {
                printf("Starting TcpRedirector Service in console mode...\n");
                if (g_Service.Initialize()) {
                    printf("Service initialized. Press Ctrl+C to stop.\n");
                    g_Service.Run();
                } else {
                    fprintf(stderr, "FATAL: Failed to initialize service — WinDivert driver not loaded or config error\n");
                    fprintf(stderr, "FATAL: Ensure WinDivert64.sys and WinDivert.dll are in the same directory\n");
                }
                return 0;
            }
        }

        // Normal service mode
        SERVICE_TABLE_ENTRYW serviceTable[] = {
            { const_cast<LPWSTR>(L"TcpRedirectorService"), ServiceMain },
            { NULL, NULL }
        };

        if (!StartServiceCtrlDispatcherW(serviceTable)) {
            printf("Starting in console mode...\n");
            if (g_Service.Initialize()) {
                printf("Service initialized. Press Ctrl+C to stop.\n");
                g_Service.Run();
            }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "Fatal unknown error\n");
        return 1;
    }
    return 0;
}