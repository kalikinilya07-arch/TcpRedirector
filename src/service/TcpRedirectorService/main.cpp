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
                    SC_HANDLE service = CreateServiceW(scm,
                        L"TcpRedirectorService",
                        L"TcpRedirector Service",
                        SERVICE_ALL_ACCESS,
                        SERVICE_WIN32_OWN_PROCESS,
                        SERVICE_AUTO_START,
                        SERVICE_ERROR_NORMAL,
                        path,
                        NULL, NULL, NULL, NULL, NULL);
                    if (service) {
                        printf("Service installed successfully\n");
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