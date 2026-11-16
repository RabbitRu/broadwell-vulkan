# Checkpoint — 2026-11-16 (после запуска vkcube)

GPU: Intel Iris 6100 `8086:162B`, Windows 11 x64, Intel `20.19.15.5171`.

## Проверено на аппаратуре

- WDDM transport: устройство, контекст, выделения, CPU/GPU mapping, residency,
  GPU VA, отправка и реальное завершение команд.
- HasVK SPIR-V → NIR → ELK: 342 Vulkan compute случая, повторная отправка
  каждого command buffer, 72 создания pipeline, roundtrip pipeline cache.
- 18 Vulkan storage-image случаев: shader write/read и независимая CPU-распаковка.
- 6 offscreen Vulkan-треугольников: SPIR-V VS/FS, render pass, framebuffer, draw.
- 12 случаев texture/sampler/dynamic UBO/D16: перекрытие геометрии с включённой
  и выключенной глубиной, сравнение цветов и D16, проверки защитных областей.
- В проверяемых процессах не загружаются Intel UMD и D3D12.
- Win32 WSI/GDI: `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`,
  BGRA8/FIFO. Неизменённый `vkcube` v1.3.301 через локальный `VK_DRIVER_FILES`:
  600 с, 92 изменения размера, 45 циклов minimize/restore, 137 непустых снимков
  окна, закрытие по `WM_CLOSE`, код выхода 0
  (`native/artifacts/vkcube-validation.json`).

- Queries: 6 случаев occlusion (точное совпадение с числом закрашенных
  пикселей), pipeline statistics, timestamp, CPU- и GPU-чтение результатов,
  reset и повторное использование (`native/tests/icd_query_test.hpp`).

Полный `tools/test-all.ps1 -SkipBuild` прошёл на текущей DLL с queries
(`native/artifacts/progress.json`). Десятиминутный прогон vkcube выполнен на
DLL до подключения queries; на текущей — прогон 60 с с resize/minimize.
Длинные прогоны на этом этапе владелец считает излишними.

## Что было исправлено

`vkcube` рисовал кадры, но падал при уничтожении swapchain: Mesa WSI безусловно
вызывает `DestroyQueryPool`, а точки входа в порте не было (нулевой указатель).
Подключён штатный `genX_query.c` HasVK (`mesa/graphics_sources.py` →
`anv_wddm_graphics_query.c`); ветки performance-query отключены макросами.

Отладка: `lldb` из `.tools\llvm\bin` запускается, если добавить в `PATH`
`.tools\python310-debug`; адреса кадров разбирает `llvm-symbolizer --obj=<dll>`.

## Не проверено

- Снимки vkcube проверяются на непустоту, без попиксельного эталона.
- Белая нижняя полоса на снимках окна 1024×768: причина не установлена.
- Размеры окна меньше 500×500, validation layers, CTS, игры.
- Queries в secondary command buffers, `inheritedQueries`, compute-статистика.
- Полной реализации/сертификации Vulkan нет.

## Продолжение

1. Разобраться с полосой на 1024×768 (сравнить extent swapchain и клиентскую область).
2. Запустить vkcube с `--validate` и исправить замечания validation layers.
3. Подготовить выборочный прогон CTS (`dEQP-VK.api.smoke`, `wsi.win32`).

Исходный C#-прототип и локальные контрольные fixtures сохранены в рабочем
каталоге. Драйверы, захваченный Intel-код, SDK, инструменты и сборки в Git не включены.
