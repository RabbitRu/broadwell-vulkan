# Iris 6100: прямой доступ к GPU через WDDM

Проверены нативные операции памяти и вычислительный шейдер на Intel Iris Graphics 6100, PCI `8086:162B`,
Windows 11, Intel `20.19.15.5171`, WDDM 2.0.

Программа сформировала команды Gen8, отправила их через
`D3DKMTSubmitCommand` и прочитала записанные GPU значения.
В режиме `-NativeOnly` в процесс не загружаются D3D12 и `igd12umd64.dll`.
Это проверяется по фактически загруженным модулям после выполнения теста.

## Что работает

- Собственные WDDM-устройство, виртуальный контекст и очередь подкачки.
- Выделение двух буферов через `D3DKMTCreateAllocation2`, CPU mapping через
  `D3DKMTLock2`, GPU mapping через `D3DKMTMapGpuVirtualAddress` и residency.
- Собственный 72-байтный пакет команд Gen8: четыре `MI_STORE_DWORD_IMM`
  и `MI_BATCH_BUFFER_END`.
- Отправка через `D3DKMTSubmitCommand` с настоящим выполнением, без NullRendering.
- Ожидание GPU через `D3DKMTSignalSynchronizationObject2 / EnqueueCpuEvent`.
- Проверка всех 1024 слов результата: четыре заданных значения и 1020
  неизменённых защитных слов.
- Освобождение CPU/GPU mappings, буферов, контекста, очереди подкачки и устройства.

Самостоятельный режим проверен в трёх отдельных процессах со случайными
входными значениями. Все три запуска прошли; D3D12-модули отсутствовали.

## Нативный вычислительный шейдер

Режим `-NativeCompute` создаёт собственные ресурсы и отправляет свой compute-пакет
Gen8 размером 328 байт. Он содержит `PIPELINE_SELECT`, `STATE_BASE_ADDRESS`,
`MEDIA_VFE_STATE`, `MEDIA_INTERFACE_DESCRIPTOR_LOAD`, `GPGPU_WALKER`, сбросы кэшей
и завершение пакета. Полный пакет D3D12 не воспроизводится; 3D-команды копирования,
отрисовка и служебный `MI_ATOMIC` из исходной трассы не используются.

Машинный код шейдера размером 896 байт получен от компилятора Intel во время
bootstrap-прогона нашего HLSL. Снимок содержит необходимые описания состояния и
ресурсов. При самостоятельном запуске наш код выделяет новую GPU-память,
перенастраивает адреса баз, указатели на шейдер, входной и выходной буферы,
параметры CURBE и количество рабочих групп.

Входной массив, seed и salt меняются при каждом запуске. CPU отдельно рассчитывает
`Mix(Input[i] ^ Salt ^ (i * 0x9e3779b9))` и проверяет все слова результата.
Три отдельных процесса прошли проверки:

| Элементов | Рабочих групп по 64 | Проверенных защитных слов |
| ---: | ---: | ---: |
| 16 381 | 256 | 3 |
| 257 | 5 | 16 127 |
| 1 | 1 | 16 383 |

В каждом процессе дополнительно подтверждено отсутствие D3D12 и `igd12umd64.dll`.
Сейчас поддерживается именно этот тестовый шейдер, с числом элементов 1..16381.

Отдельный базовый тест через штатный D3D12-драйвер также проходит:
копирование 65 536 байт, вычислительный шейдер `cs_5_1` с проверкой
16 381 результата, треугольник RGBA8 512×384. Шесть пикселей треугольника
сравниваются с CPU-расчётом барицентрических координат.

## Граница результата

Это ещё не Vulkan ICD и не порт HasVK. Вычислительный шейдер уже запускается
самостоятельным WDDM-путём; треугольник пока выполняется базовым тестом D3D12.
Компиляция произвольного SPIR-V в Gen8, генерация произвольных состояний pipeline
и нативная графика ещё не реализованы.

Есть зависимость от трёх сохранённых блоков параметров Intel:
шаблоны описаний буферов 8192/4096 байт по 1096 байт и заголовок отправки
80 байт. Они сняты со штатного UMD в bootstrap-прогоне. Самостоятельный процесс
читает их из файлов и работает через штатный KMD, не загружая UMD.

Шаблоны памяти закреплены SHA-256; перед использованием проверяется версия драйвера
`20.19.15.5171`. Общий генератор описаний GMM, произвольные ресурсы, форматы
текстур и поддержка других драйверов ещё не реализованы.

Compute также использует сохранённые ISA шейдера и состояния, подготовленные
Intel UMD, с проверкой SHA-256 и размера файлов. Считывание состояния и команд
выполняется через `-CaptureCompute`; Mesa genxml используется для разбора пакетов.
Некоторые GPU-only буферы недоступны `Lock2`: в снимке они помечены
`cpuReadSucceeded=false`, их файлы не считаются достоверным содержимым памяти.
Самостоятельный compute использует только успешно прочитанные состояния,
а входной и выходной массивы создаёт заново.

## Запуск

Нужны Windows x64, конкретная Iris 6100 и установленный .NET Framework 4.x.
Для сборки используется штатный `csc.exe`; SDK и пакеты не устанавливаются.

Из корня проекта:

```powershell
# Полный bootstrap: D3D12 baseline, трасса, прямые команды, собственный WDDM allocator.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -Raw -OutputDirectory .\gpu-probe\artifacts\native-bootstrap

# Самостоятельный процесс: только WDDM, сохранённые параметры Intel и наши команды.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -NativeOnly -OutputDirectory .\gpu-probe\artifacts\native-only

# Подготовить снимок нашего compute-шейдера и разобрать пакеты Gen8.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\tools\fetch-genxml.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -CaptureCompute -OutputDirectory .\gpu-probe\artifacts\compute-bootstrap
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\tools\decode-compute.ps1

# Самостоятельный compute: без D3D12, с новыми данными и перенастроенными адресами.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -NativeCompute -ElementCount 16381 -OutputDirectory .\gpu-probe\artifacts\native-compute

# Только сборка.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\gpu-probe\run.ps1 -BuildOnly
```

Другую папку шаблонов можно передать через `-BootstrapDirectory`, снимок compute —
через `-CaptureDirectory`. Если файлы уже подготовлены, повторно запускать
`-CaptureCompute` и скачивать XML не требуется. `fetch-genxml.ps1` сохраняет URL
и SHA-256 определений; описание берётся из зеркала открытых исходников Mesa.
Выполнение ограничено конкретным PCI ID; программного рендерера как запасного
варианта нет. Ожидания GPU и paging fence ограничены 15 секундами.
Тест работает с несколькими килобайтами собственной GPU-памяти.

Параметры `-Raw` и `-Trace` включают трассировщик: он временно перехватывает
девять GPU-вызовов `win32u.dll` только в адресном пространстве своего процесса,
сохраняет аргументы и восстанавливает точки входа. Он принимает только
проверенный x64 layout syscall stub. В `-NativeOnly` перехват не включается.
Файлы системных DLL и другие процессы не меняются.

Smart App Control был отключён самим пользователем после подтверждения.
Программа не меняет политики безопасности Windows. `-ExecutionPolicy Bypass`
задаёт режим только запуска PowerShell и не управляет Smart App Control.

## Результаты

- `artifacts/native-compute/report.json` — самостоятельный compute для 16 381 элемента.
- `artifacts/native-compute-257/report.json`, `native-compute-1/report.json` — граничные проверки.
- `artifacts/native-compute/native-compute-batch.bin` — наш пакет Gen8, 328 байт.
- `artifacts/native-compute/gen8-compute-kernel.bin` — ISA вычислительного шейдера, 896 байт.
- `artifacts/native-compute/native-compute-readback.bin` — фактический выходной массив и защитные слова.
- `artifacts/compute-bootstrap/compute-capture/capture.json` — снимок и хеши ресурсов.
- `artifacts/compute-bootstrap/compute-capture/decoded-packets.json` — разбор исходного пакета.
- `artifacts/compute-regression/report.json` — повторная проверка прежних стадий после добавления compute.
- `artifacts/native-store-regression/report.json` — проверка самостоятельных команд записи текущей сборкой.
- `artifacts/native-only/report.json` — самостоятельный успешный запуск.
- `artifacts/native-only-repeat2/report.json`, `native-only-repeat3/report.json` — повторы.
- `artifacts/native-only/native-memory-batch.bin` — созданные программой команды.
- `artifacts/native-only/native-memory-readback.bin` — фактические данные с GPU.
- `artifacts/native-bootstrap/report.json` — все стадии вместе с D3D12 baseline.
- `artifacts/native-bootstrap/bootstrap/` — три блока параметров Intel.
- `artifacts/native-bootstrap/wddm/calls.jsonl` — трасса с кодами возврата.
- `artifacts/native-bootstrap/triangle.png` — треугольник базового теста.
- `artifacts/native-bootstrap/build.json` — SHA-256 EXE, исходников и шейдера.
- `artifacts/status.json` — сводный статус и следующие шаги.

Папки `native-map-diagnostic` и ранние `trace/raw` содержат промежуточные
эксперименты. Актуальные успешные результаты перечислены выше.
При первом запросе mapping был указан CPU-диапазон VA; рабочая трасса показала,
что этот драйвер использует GPU-диапазон выше `0x800000000000`.
После исправления mapping самостоятельный тест прошёл.

## Исходники

- `src/NativeCompute.cs` — собственный compute-пакет, перенастройка состояний, проверка результатов.
- `src/ComputeCapture.cs` — снимок буферов, машинного кода и аргументов compute.
- `src/NativeMemory.cs` — самостоятельный WDDM allocator, mappings, residency и проверка.
- `src/NativeOnly.cs` — запуск без D3D12, проверка аппаратного адаптера и модулей.
- `src/Wddm.cs` — контекст, прямая отправка пакета, ожидание завершения.
- `src/KernelTrace.cs` — диагностика GPU-вызовов и получение параметров Intel.
- `src/Native.cs` — COM ABI D3D12/DXGI и проверки размеров структур.
- `src/Program.cs` — D3D12 baseline, тесты прямых команд, проверка результатов.
- `src/Imports.cs` — чтение списка модулей и обычных импортов.
- `shaders/probe.hlsl` — шейдеры базового теста.

## Дальнейшая работа

1. Подключить компилятор Mesa Gen8 для произвольного SPIR-V и создавать состояния pipeline без снимка Intel UMD.
2. Заменить сохранённые GMM-шаблоны созданием полноценных описаний ресурсов.
3. Перенести рабочие операции памяти, submit, synchronization и compute в Windows backend HasVK.
4. Реализовать нативную графику, Vulkan loader ICD, Win32 presentation и аппаратный `vkcube`.
5. Проверять Vulkan CTS и требования конкретных приложений.

Эти пункты пока не выполнены. Результат не является сертификацией Vulkan,
драйвером для произвольных игр или тестом производительности.

## Первичные источники

- Microsoft WDDM headers: https://github.com/microsoft/win32metadata/tree/main/generation/WinSDK/RecompiledIdlHeaders/shared
- Microsoft DirectX headers: https://github.com/microsoft/DirectX-Headers
- Intel WDDM implementation: https://github.com/intel/compute-runtime/tree/master/shared/source/os_interface/windows/wddm
- Intel command encodings in i915: https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_gpu_commands.h
- Mesa HasVK: https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/intel/vulkan_hasvk
