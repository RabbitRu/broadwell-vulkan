# HasVK/WDDM — Vulkan 1.3 для Intel Iris 6100 под Windows

Экспериментальный Vulkan-драйвер (ICD) на основе Mesa HasVK, работающий
поверх штатного WDDM-драйвера Intel без Intel UMD и без D3D.

## Поддерживаемая конфигурация

Только одна:

| | |
|---|---|
| GPU | Intel Iris Graphics 6100 (Broadwell GT3, PCI `8086:162b`) |
| Драйвер Intel | `20.19.15.5171` |
| ОС | Windows 10/11, 64 бит |
| Приложения | 64-битные |

На другом GPU или другой версии драйвера ICD не находит устройство и ничего
не делает: закрытый интерфейс драйвера Intel снят с этой версии.

## Возможности

- Vulkan 1.3, 108 расширений устройства (список — `vulkaninfo`).
- Вывод в окно без копирования через CPU (как у OpenGL-драйвера Intel);
  форматы B8G8R8A8/R8G8B8A8 в UNORM и SRGB; режимы FIFO, MAILBOX, IMMEDIATE.
- Известные отказы CTS и их причины — `cts-expected-failures.md`.

## Установка

В PowerShell **от имени администратора**, в распакованной папке пакета:

```powershell
.\tools\install-icd.ps1 -WhatIf     # показать шаги, ничего не меняя
.\tools\install-icd.ps1             # установить
```

Скрипт копирует драйвер в `%ProgramFiles%\HasVK-WDDM`, регистрирует его в
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers` и проверяет установленную копию
пробой `IrisIcdProbe` через системный загрузчик Vulkan. Уже
зарегистрированные драйверы не трогаются.

Без установки драйвер можно использовать для одного приложения:

```powershell
$env:VK_DRIVER_FILES = "<папка пакета>\icd\intel_hasvk_wddm_icd.json"
& "<приложение>.exe"
```

## Удаление

```powershell
.\tools\install-icd.ps1 -Uninstall
```

## Переменные окружения для диагностики

| Переменная | Действие |
|---|---|
| `HASVK_WDDM_PRESENT=gdi` | вывод через GDI с копией в CPU (обходной путь) |
| `HASVK_WDDM_PRESENT_STATS=1` | сколько кадров прошло через окно и через GDI |
| `HASVK_WDDM_NO_AUX=1` | без вспомогательных поверхностей HiZ/MCS |
| `HASVK_WDDM_SYNC_SUBMIT=1` | ждать каждую отправку (медленнее, для отладки) |

## Ограничения

- Одна очередь графики и compute; отдельной transfer-очереди нет.
- Работа GPU дольше 2 секунд одной отправкой сбрасывается Windows (TDR),
  приложение получает `VK_ERROR_DEVICE_LOST`.
- Нет sparse-ресурсов, внешней памяти и семафоров между процессами.
