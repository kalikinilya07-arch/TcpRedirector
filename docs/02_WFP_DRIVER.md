# WFP Redirect Driver — детальная архитектура

## 1. Используемые WFP слои

### Основной слой перехвата

| Слой | GUID | Направление | Назначение |
|------|------|-------------|------------|
| `FWPM_LAYER_ALE_AUTH_CONNECT_V4` | `FWPM_LAYER_ALE_AUTH_CONNECT_V4` | Outbound | Перехват исходящих TCP-соединений IPv4 |
| `FWPM_LAYER_ALE_AUTH_CONNECT_V6` | `FWPM_LAYER_ALE_AUTH_CONNECT_V6` | Outbound | Перехват исходящих TCP-соединений IPv6 |

**Обоснование выбора ALE_AUTH_CONNECT:**
- Этот слой вызывается один раз на соединение (при установке), а не на каждый пакет
- Предоставляет доступ к полной информации о процессе (PID, путь)
- Поддерживает штатный механизм Connection Redirect через `FwpsRedirectHandleCreate0`
- Не требует модификации пакетов
- Минимальное влияние на производительность

### Дополнительные слои (мониторинг состояния)

| Слой | GUID | Назначение |
|------|------|------------|
| `FWPM_LAYER_ALE_CONNECT_REDIRECT_V4` | `FWPM_LAYER_ALE_CONNECT_REDIRECT_V4` | Обработка редиректа на V4 |
| `FWPM_LAYER_ALE_CONNECT_REDIRECT_V6` | `FWPM_LAYER_ALE_CONNECT_REDIRECT_V6` | Обработка редиректа на V6 |
| `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4` | `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4` | Соединение установлено (начало отсчёта) |
| `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6` | `FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6` | Соединение установлено V6 |
| `FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V4` | `FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V4` | Соединение закрыто |
| `FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V6` | `FWPM_LAYER_ALE_ENDPOINT_CLOSURE_V6` | Соединение закрыто V6 |

## 2. Callout Drivers

### Callout: `TcpRedirectCallout`

**Функции callout:**

```c
// Классификация — основная логика перехвата
void NTAPI TcpRedirectClassify(
    _In_ const FWPS_INCOMING_VALUES* inFixedValues,
    _In_ const FWPS_INCOMING_METADATA_VALUES* inMetaValues,
    _Inout_opt_ void* layerData,
    _In_opt_ const void* classifyContext,
    _In_ const FWPS_FILTER* filter,
    _In_ UINT64 flowContext,
    _Inout_ FWPS_CLASSIFY_OUT* classifyOut
);

// Уведомление — вызывается при добавлении/удалении фильтра
NTSTATUS NTAPI TcpRedirectNotify(
    _In_ FWPS_CALLOUT_NOTIFY_TYPE notifyType,
    _In_ const GUID* filterKey,
    _Inout_ FWPS_FILTER* filter
);

// Процесс завершения — для flow established
NTSTATUS NTAPI TcpRedirectFlowDelete(
    _In_ UINT16 layerId,
    _In_ UINT32 calloutId,
    _In_ UINT64 flowContext
);
```

**Callout GUID:**

```
DEFINE_GUID(TCP_REDIRECT_CALLOUT_GUID,
    0x12345678, 0xABCD, 0xEF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01);
```

### Callout: `TcpFlowEstablishedCallout`

Используется для отслеживания установленных соединений и сбора статистики.

```
DEFINE_GUID(TCP_FLOW_ESTABLISHED_CALLOUT_GUID,
    0x12345678, 0xABCD, 0xEF01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x02);
```

## 3. Регистрация callout и фильтров

```c
// Пример регистрации callout
NTSTATUS RegisterTcpRedirectCallout(
    _In_ HANDLE engineHandle,
    _Out_ UINT32* calloutId
) {
    FWPS_CALLOUT callout = {0};
    callout.calloutKey = TCP_REDIRECT_CALLOUT_GUID;
    callout.flags = 0;
    callout.classifyFn = TcpRedirectClassify;
    callout.notifyFn = TcpRedirectNotify;
    callout.flowDeleteFn = TcpRedirectFlowDelete;

    return FwpsCalloutRegister(
        (void*)DeviceObject,  // передаём наш DeviceObject
        &callout,
        calloutId
    );
}

// Добавление фильтра на слой ALE_AUTH_CONNECT_V4
NTSTATUS AddRedirectFilter(
    _In_ HANDLE engineHandle,
    _In_ UINT32 calloutId
) {
    FWPM_FILTER filter = {0};
    filter.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
    filter.displayData.name = L"TcpRedirector ALE Connect Filter";
    filter.displayData.description = L"Redirects TCP connections through HTTP proxy";
    filter.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filter.action.calloutId = calloutId;
    filter.filterCondition = NULL;  // все соединения
    filter.numFilterConditions = 0;
    filter.weight.type = FWP_EMPTY;
    
    return FwpmFilterAdd(engineHandle, &filter, NULL, NULL);
}
```

## 4. Механизм Connection Redirect

### Последовательность редиректа

```
1. Приложение вызывает connect(socket, (struct sockaddr*)&addr, sizeof(addr))
2. WFP вызывает TcpRedirectClassify на слое ALE_AUTH_CONNECT
3. В classifyFn:
   a. Извлекаем PID из inMetaValues->remoteId
   b. Извлекаем Process Name через ZwQueryInformationProcess
   c. Проверяем соответствие правилам (кэш правил хранится в драйвере)
   d. Если редирект нужен:
      - Создаём redirect handle: FwpsRedirectHandleCreate0(...)
      - Сохраняем в контексте: original_ip, original_port, PID
      - Устанавливаем FWP_ALE_FLAG_REDIRECT_TCP_CONNECTION
      - RedirectHandle = созданный handle
      - RedirectContext = указатель на структуру с оригинальными данными
   e. Если редирект не нужен: FWP_ACTION_PERMIT
4. WFP создаёт локальный сокет и связывает его с приложением
5. После установки redirect срабатывает ALE_CONNECT_REDIRECT
6. Сервис принимает соединение от драйвера (на redirect-порту)
7. Сервис подключается к HTTP Proxy и отправляет CONNECT
```

### Критические структуры данных

```c
// Структура для хранения информации о редиректе
typedef struct _REDIRECT_INFO {
    LIST_ENTRY ListEntry;           // связный список
    UINT64 redirectId;              // уникальный ID редиректа
    HANDLE processId;               // PID процесса
    UINT16 originalPort;            // оригинальный порт назначения (network order)
    UINT16 redirectPort;            // порт, на который сделан редирект
    UINT32 originalAddressV4;       // оригинальный IPv4 адрес (network order)
    UINT16 originalAddressV6[8];    // оригинальный IPv6 адрес
    BOOLEAN isIPv6;                 // флаг IPv6
    UINT64 timestamp;               // время создания
    UINT64 flowHandle;              // handle потока
    KSPIN_LOCK lock;                // спин-лок для синхронизации
    WCHAR processPath[260];         // полный путь к процессу
} REDIRECT_INFO, *PREDIRECT_INFO;
```

## 5. Коммуникация Kernel Mode ↔ User Mode

### Механизм: IOCTL + Shared Event Queue

```
┌──────────────────┐          ┌─────────────────────────────┐
│  User Mode       │          │   Kernel Mode Driver        │
│  (Service)       │          │                             │
│                  │  IOCTL   │                             │
│  DeviceIoControl ├─────────►│  DriverEntry -> IRP_MJ_DEVICE_CONTROL
│  (control cmd)   │          │                             │
│                  │◄─────────┤                             │
│                  │  return  │                             │
│                  │          │                             │
│  WaitForSingleObject         │  KeSetEvent (redirect event│
│  (redirectEvent)◄═══════════╪═►)                         │
│                  │          │                             │
│                  │  IOCTL   │                             │
│  DeviceIoControl ├─────────►│  Чтение REDIRECT_INFO       │
│  (read redirect) │          │  из внутренней очереди      │
│                  │◄─────────┤                             │
│                  │  data    │                             │
└──────────────────┘          └─────────────────────────────┘
```

### IOCTL коды

| IOCTL | Код | Направление | Назначение |
|-------|-----|-------------|------------|
| `IOCTL_REDIRECTOR_SET_PROXY_CONFIG` | `CTL_CODE(0x8000, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)` | Service → Driver | Обновление кэша правил |
| `IOCTL_REDIRECTOR_GET_PENDING` | `CTL_CODE(0x8000, 0x801, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)` | Service → Driver | Получение списка ожидающих редиректов |
| `IOCTL_REDIRECTOR_ACK_REDIRECT` | `CTL_CODE(0x8000, 0x802, METHOD_IN_DIRECT, FILE_ANY_ACCESS)` | Service → Driver | Подтверждение обработки редиректа |
| `IOCTL_REDIRECTOR_GET_STATS` | `CTL_CODE(0x8000, 0x803, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)` | Service → Driver | Получение статистики |
| `IOCTL_REDIRECTOR_SET_RULES` | `CTL_CODE(0x8000, 0x804, METHOD_IN_DIRECT, FILE_ANY_ACCESS)` | Service → Driver | Обновление правил |
| `IOCTL_REDIRECTOR_QUERY_PROCESS` | `CTL_CODE(0x8000, 0x805, METHOD_IN_DIRECT, FILE_ANY_ACCESS)` | Service → Driver | Запрос информации о процессе по PID |

### Именование устройств

```
\\.\TcpRedirectorDriver
\Device\TcpRedirectorDriver
\DosDevices\TcpRedirectorDriver
```

### Событийный механизм

Драйвер использует `KeSetEvent` для уведомления сервиса о новых редиректах. Сервис ожидает событие через `WaitForSingleObject` на handle устройства.

```c
// Kernel: Создание и сигнализация события
KEVENT g_redirectEvent;
KeInitializeEvent(&g_redirectEvent, NotificationEvent, FALSE);

// При новом редиректе:
KeSetEvent(&g_redirectEvent, IO_NO_INCREMENT, FALSE);

// User mode:
HANDLE hDevice = CreateFileW(L"\\\\.\\TcpRedirectorDriver", ...);
HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
// ... привязка события через IOCTL или OVERLAPPED
WaitForSingleObject(hEvent, INFINITE);
```

## 6. Windows API для драйвера

### WFP API (fwpsk.h)

| Функция | Назначение |
|---------|------------|
| `FwpsCalloutRegister` | Регистрация callout в WFP |
| `FwpsCalloutUnregisterById` | Отмена регистрации callout |
| `FwpsRedirectHandleCreate0` | Создание handle для редиректа TCP |
| `FwpsRedirectHandleDestroy0` | Уничтожение handle редиректа |
| `FwpsAcquireClassifyHandle0` | Получение handle классификации |
| `FwpsReleaseClassifyHandle0` | Освобождение handle классификации |
| `FwpsAcquireWritableLayerDataPointer0` | Получение указателя на данные слоя (для установки redirect) |
| `FwpsApplyModifiedLayerData0` | Применение изменённых данных слоя |
| `FwpsQueryConnectionRedirectState0` | Проверка состояния редиректа |
| `FwpsStreamAsyncNotify0` | Асинхронное уведомление потока |

### WFP Management API (fwpmu.h)

| Функция | Назначение |
|---------|------------|
| `FwpmEngineOpen` | Открытие движка WFP |
| `FwpmEngineClose` | Закрытие движка WFP |
| `FwpmTransactionBegin` | Начало транзакции WFP |
| `FwpmTransactionCommit` | Фиксация транзакции WFP |
| `FwpmFilterAdd` | Добавление фильтра WFP |
| `FwpmFilterDeleteByKey` | Удаление фильтра WFP |
| `FwpmCalloutAdd` | Добавление callout в WFP |
| `FwpmCalloutDeleteByKey` | Удаление callout из WFP |
| `FwpmProviderAdd` | Добавление провайдера WFP |
| `FwpmSubLayerAdd` | Добавление подслоя WFP |

### Kernel API (ntddk.h, wdm.h)

| Функция | Назначение |
|---------|------------|
| `IoCreateDevice` | Создание device object |
| `IoCreateSymbolicLink` | Создание символической ссылки |
| `IoDeleteDevice` | Удаление device object |
| `IoDeleteSymbolicLink` | Удаление символической ссылки |
| `IoGetCurrentIrpStackLocation` | Получение текущего IRP стека |
| `ZwQueryInformationProcess` | Получение информации о процессе |
| `ZwQueryInformationFile` | Получение пути процесса |
| `PsGetCurrentProcessId` | Получение PID текущего процесса |
| `PsGetProcessImageFileName` | Получение имени процесса |
| `SeLocateProcessImageName` | Получение полного пути процесса |
| `KeInitializeEvent` | Инициализация события |
| `KeSetEvent` | Сигнализация события |
| `KeWaitForSingleObject` | Ожидание объекта |
| `ExAllocatePool2` | Выделение памяти (новый API) |
| `ExFreePool` | Освобождение памяти |
| `InitializeListHead` | Инициализация списка |
| `InsertTailList` | Вставка в конец списка |
| `RemoveHeadList` | Извлечение из головы списка |
| `RtlIpv4AddressToStringA` | Преобразование IPv4 адреса в строку |
| `RtlIpv6AddressToStringA` | Преобразование IPv6 адреса в строку |
| `RtlCopyMemory` | Копирование памяти |

## 7. Структура драйвера (файлы)

```
src/driver/TcpRedirectorDriver/
├── driver.h                     # Заголовочный файл драйвера
├── driver.c                     # DriverEntry, DriverUnload
├── trace.h                      # Трассировка (WPP)
├── callouts/
│   ├── redirect_callout.h       # Заголовок callout
│   ├── redirect_callout.c       # Реализация классификации/редиректа
│   ├── flow_callout.h           # Callout для flow established
│   └── flow_callout.c           # Реализация flow established
├── common/
│   ├── redirect_info.h          # REDIRECT_INFO структура
│   ├── redirect_queue.h         # Очередь редиректов
│   ├── redirect_queue.c         # Реализация очереди
│   ├── process_info.h           # Утилиты для работы с процессами
│   └── process_info.c           # Реализация
├── communication/
│   ├── device_io.h              # IOCTL обработчики
│   ├── device_io.c              # Реализация IOCTL
│   ├── event_channel.h          # Канал событий
│   └── event_channel.c          # Реализация событий
├── rules_cache.h                # Кэш правил для принятия решений
├── rules_cache.c                # Реализация кэша правил
├── TcpRedirectorDriver.vcxproj  # Проект Visual Studio
├── TcpRedirectorDriver.inf      # INF-файл установки
└── TcpRedirectorDriver.sln      # Solution file
```

## 8. Критически важные аспекты реализации

### 8.1 Redirect Handle Lifetime
- Handle создаётся один раз при инициализации драйвера
- Используется для всех редиректов (один handle на процессор)
- Уничтожается при выгрузке драйвера

### 8.2 Потокобезопасность
- Все операции с очередью редиректов защищены спин-локом
- Используется `ExInterlocked...` для атомарных операций
- Callout функция должна быть re-entrant

### 8.3 Ограничение размера очереди
- Максимум 4096 ожидающих редиректов
- При переполнении — новые соединения блокируются
- Timeout ожидания обработки — 30 секунд

### 8.4 Кэш правил
- Дубликат правил из сервиса для быстрой проверки в callout
- Размер кэша: до 1024 правил
- Обновляется через IOCTL из сервиса

### 8.5 Обработка ошибок
- При невозможности выполнить редирект → FWP_ACTION_BLOCK
- При ошибке определения процесса → FWP_ACTION_PERMIT (безопасное поведение)
- При переполнении очереди → FWP_ACTION_BLOCK