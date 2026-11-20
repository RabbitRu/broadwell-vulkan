# Broadwell Vulkan: HasVK/WDDM для Intel Iris 6100

Экспериментальный Vulkan-драйвер (ICD) для Windows x64 на основе Mesa HasVK
26.2.7. Он работает поверх штатного WDDM-драйвера ядра Intel
`20.19.15.5171` на Intel Iris 6100 (`8086:162b`); Intel UMD и D3D12 в процесс
не загружаются. Другие GPU и версии драйвера не поддерживаются: на них ICD не
показывает ни одного устройства.

## Состояние

- Vulkan 1.3, 108 расширений устройства. Собственный транспорт WDDM, штатные
  SPIR-V → NIR → ELK → Gen8, ISL, BLORP и runtime Mesa.
- Вывод в окно без копии через CPU, режимы FIFO, MAILBOX, IMMEDIATE.
- CTS 1.4.5.5: полный прогон mustpass выполнен 2026-11-20 (2 849 001 тест:
  Pass 669 179, NotSupported 2 179 726, не прошли 90; после исправлений из них
  проходят 89, остался один ожидаемый отказ). Драйвер не сертифицирован.
  Отказы и их причины —
  [docs/cts-expected-failures.md](docs/cts-expected-failures.md).
- Проверенные приложения: `vulkaninfo`, `vkcube`, `vkcubepp`, The Talos
  Principle.
- Производительность в The Talos Principle: 70–83 FPS (чаще 75–79) против
  79–87 у DX11 на драйвере Intel в тех же сериях прогонов (в начале работы —
  27,8 FPS). На главном потоке игры драйвер обходится дешевле драйвера Intel
  примерно на 1 мс на кадр; разрыв задаёт путь Vulkan самой игры, и
  универсальными исправлениями драйвера он не закрывается — разбор в
  [docs/optimizations-vs-dx11.md](docs/optimizations-vs-dx11.md). Без
  построения карт теней Vulkan не медленнее DX11: 114–124 против 114–117 FPS
  (замер F6) — весь разрыв даёт то, как путь Vulkan игры строит тени.
- Конечная цель — Dota 2 под Vulkan быстрее, чем под DX11. Текущая сборка
  (clang 23.1.6 с PGO, все исправления CTS, `VK_EXT_memory_budget`;
  пакет `…-46d83ae`, 2026-11-21) установлена в систему как ICD; сама
  игра не установлена — ей нужно около 60 ГиБ, свободно 35
  ([docs/roadmap.md](docs/roadmap.md), этап 8).
- Зависание GPU на этой версии драйвера ядра Intel дважды закончилось
  критической ошибкой системы; обе найденные причины зависаний исправлены,
  известных зависающих тестов CTS не осталось (roadmap, раздел 3).

![vkcube на Iris 6100 через HasVK/WDDM ICD](native/artifacts/vkcube-preview.png)

## Документация

- [docs/roadmap.md](docs/roadmap.md) — что сделано и что осталось.
- [docs/optimizations-vs-dx11.md](docs/optimizations-vs-dx11.md) —
  производительность: итог, измерения, что изменено, статус гипотез,
  отклонённые опыты, команды для повторения каждого замера, методика.
- [docs/install.md](docs/install.md) — установка, кэш шейдеров, переменные
  окружения, ограничения (эта инструкция входит в релизный пакет).
- [docs/cts-expected-failures.md](docs/cts-expected-failures.md) — известные
  отказы CTS.
- [docs/intel-interface.md](docs/intel-interface.md) — закрытый интерфейс
  Intel, от которого зависит порт, и порядок переноса на другую версию
  драйвера.
- [gpu-probe/README.md](gpu-probe/README.md) — исходный C#-прототип прямого
  доступа к GPU.

## Сборка

Нужны минимум 25 ГиБ свободного места перед установкой зависимостей.
Все команды выполняются из корня проекта в PowerShell x64:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\bootstrap.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\test-all.ps1
```

Bootstrap устанавливает MSVC/SDK и Python, получает Mesa 26.2.7 и зависимости
с закреплёнными SHA-256. Драйвер (Mesa) собирается clang-cl из LLVM 23.1.6:
официальный архив выпуска `clang+llvm-23.1.6-x86_64-pc-windows-msvc.tar.xz`
(SHA-256 в `tools/dependencies.json`) распаковывается в `.tools\llvm-23.1.6`
без установщика и прав администратора; оттуда же lld-link,
llvm-symbolizer и llvm-profdata. В архиве нет lldb: `tools/debug-icd.ps1`
берёт lldb 19.1.5, оставшийся в `.tools\llvm` от прежней установки LLVM
(остальное оттуда удалено), с Python 3.10 из `.tools\python310-debug`;
bootstrap их не ставит. Mesa собирается с `-march=broadwell`
(AVX2, BMI2, FMA): драйвер работает только рядом с GPU Broadwell, значит
и процессор всегда Broadwell. Сборка оптимизируется по профилю (PGO):
`tools/pgo/hasvk.profdata` снят `tools/pgo-profile.ps1` на стенде записи
команд, vkcubepp и бенчмарке Talos; поведение драйвера он не меняет, только
раскладку кода (`build-mesa-compiler.ps1 -Pgo none` — без него). Отладочной информации в сборке по умолчанию
нет; для профилирования и отладки с именами функций (`rip_sampler`,
`tools/debug-icd.ps1`) — `tools/build-mesa-compiler.ps1 -DebugInfo`, затем
`tools/build-icd.ps1 -SkipBuild` (PDB ляжет рядом с DLL в `build\icd`).
Версии Python-пакетов закреплены в `tools/requirements.txt`.
MSVC 14.44.35207, SDK 10.0.26100.0 и хеши компиляторов/заголовков KMT закреплены
в `tools/toolchain-lock.json`; сборка останавливается при несовпадении.
Онлайн-установщик Visual Studio может менять состав пакетов: для повторной
установки именно этой версии следует сохранить локальные инструменты/пакеты.
`ExecutionPolicy Bypass` действует только для запускаемого процесса.

Установленные инструменты находятся в `.tools`, исходники зависимостей —
в `third_party`, сборки и подробные журналы — в `build`. Windows SDK и служебный
установщик Visual Studio используют стандартные системные каталоги.

Изменения Mesa воспроизводятся скриптом `mesa/apply-compiler-port.py` и
сохраняются в `mesa/patches/compiler-port.patch`. ICD собирает
`tools/build-icd.ps1`; готовый комплект (DLL и JSON-манифест) лежит в
`build/icd`.

## Проверки

Все проверки идут на настоящей GPU и не требуют участия пользователя.

- `tools/test-all.ps1` — общий прогон аппаратных проб с побайтной или
  попиксельной сверкой результатов; ключ `-SkipBuild` запускает его на уже
  собранных файлах.
- Отдельные пробы: `test-native.ps1` (транспорт WDDM), `test-elk.ps1`,
  `test-spirv.ps1`, `test-images.ps1`, `test-icd.ps1`, `test-graphics.ps1`,
  `test-wsi.ps1` (вывод в окно), `test-oom.ps1`, `test-leaks.ps1`,
  `test-device-lost.ps1`, `test-kernel-limits.ps1`,
  `test-present-redirect.ps1`, `test-validation.ps1` (validation layers),
  `test-stability.ps1`.
- CTS: `build-cts.ps1`, `run-cts.ps1` и срезы по группам
  (`run-cts-sample.ps1`, `run-cts-groups.ps1`, `run-cts-vk13.ps1`,
  `run-cts-extensions.ps1`); полный mustpass (2,89 млн тестов, 8–10 часов,
  возобновляемый) — `run-cts-full.ps1`, ход в `build\cts-full\progress.log`.
- `run-talos.ps1` — встроенный бенчмарк The Talos Principle под Vulkan и DX11
  (`-Sample` — потоки, частота CPU и загрузка GPU в окне замера;
  `-RipModule` — сэмплы стеков; `-D3D11Proxy` — разбивка кадра DX11).
- Диагностика производительности: `build-probes.ps1` (пробы, `rip_sampler`,
  прокси `d3d11.dll`), журнал кадра `HASVK_WDDM_TIMELINE` с разбором
  `timeline-report.py`, таблицы кадров игры — `bench-frames.py`, секции
  кадра игры в миллисекундах по API — `talos-sections.py`, сравнение
  выборок кода двух прогонов — `rip-compare.py`.

Что именно проверяет каждая проба и с каким результатом — в
[docs/roadmap.md](docs/roadmap.md), раздел 1. Компактные отчёты прогонов —
в `native/artifacts`.

## Использование

Для одного приложения, без установки:

```powershell
$env:VK_DRIVER_FILES = "<корень проекта>\build\icd\intel_hasvk_wddm_icd.json"
& "<приложение>.exe"
```

Системная установка и удаление — `tools/install-icd.ps1`, инструкция в
[docs/install.md](docs/install.md). Релизный пакет собирает
`tools/make-release.ps1`.

## Состав репозитория

В Git сохраняются исходники, патч к закреплённой Mesa, скрипты и компактные
отчёты. `.tools`, `third_party`, `build` и захваты `gpu-probe/artifacts` остаются
локальными. Bootstrap получает внешние зависимости заново. Сохранённые Intel
ISA/описатели контрольного C# и раннего raw-теста в репозиторий не входят:
для `test-all.ps1` нужны локальные контрольные fixtures. Самостоятельные Vulkan
compute/graphics тесты (`test-icd.ps1`, `test-graphics.ps1`) их не используют.
