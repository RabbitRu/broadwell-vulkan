# Checkpoint — 2026-11-16

Разработка приостановлена по просьбе владельца для сохранения состояния в GitHub.
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

Последний полный регрессионный прогон записан в `native/artifacts/progress.json`.
Он предшествует последним изменениям graphics/WSI. После подключения graphics
compute/storage регрессия прошла отдельно; после подключения WSI прошёл
`test-graphics.ps1`. Точные версии бинарников различаются и отражены в SHA-256
соответствующих `icd-validation.json` и `graphics-validation.json`.
Свежий полный прогон после WSI ещё не выполнялся.

## Подключено, но ещё не проверено выполнением

Win32 WSI Mesa, `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`;
BGRA8/FIFO/GDI. GPU должен копировать изображение в CPU-visible buffer, WSI
ожидает fence перед переносом кадра CPU в DIB/GDI. DXGI/D3D12 callback отсутствует.
Стандартный `vkcube` v1.3.301 собран без изменения upstream-исходников, но ещё
не запускался. Resize/minimize, 10 минут работы, validation layers и CTS не проверены.
Полной реализации/сертификации Vulkan пока нет.

## Продолжение

1. Выполнить полный `tools/test-all.ps1` на текущем состоянии.
2. Проверить создание Win32 surface/swapchain, acquire, GPU-copy и present.
3. Запустить неизменённый `vkcube` через локальный `VK_DRIVER_FILES`.
4. Исправить выявленные ошибки и выполнить 10-минутный прогон с resize/minimize.

Исходный C#-прототип и локальные контрольные fixtures сохранены в рабочем
каталоге. Драйверы, захваченный Intel-код, SDK, инструменты и сборки в Git не включены.
