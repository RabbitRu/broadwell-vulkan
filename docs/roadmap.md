# План реализации Vulkan для Iris 6100 (Broadwell) под Windows

Состояние на 2026-11-17, коммит `e075b50` и последующие. Цель — ICD на основе Mesa HasVK
поверх WDDM-драйвера Intel `20.19.15.5171`, без Intel UMD и D3D12.

Обозначения:

- **[x]** — работает и проверено на железе автоматическим тестом;
- **[~]** — частично: собрано, но не проверено, или работает с ограничениями;
- **[ ]** — не сделано.

Ориентир «полноценной реализации»: upstream HasVK для Gen8 объявляет
Vulkan 1.3 (`anv_device.c`, ветка `use_softpin`). Сейчас ICD объявляет 1.2.
Дорога делится на три рубежа: **A** — корректный Vulkan 1.0, **B** — паритет
с HasVK (1.1–1.3 и расширения), **C** — пригодность для реальных приложений.

## Сводка

| Область | Состояние |
|---|---|
| WDDM-транспорт (устройство, память, отправка команд) | [x] для одного профиля GPU/драйвера |
| Компилятор шейдеров SPIR-V → NIR → ELK → Gen8 | [x] compute, VS, FS |
| Compute | [~] 3D-группы, shared memory, indirect dispatch; лимиты HasVK |
| Графический конвейер | [~] vertex input, все формы draw, blending, stencil, MSAA 4x, scissor |
| Изображения и форматы | [~] 47 форматов, 1D/2D/3D/cube, mip-уровни, слои; без aux-поверхностей |
| Команды копирования | [x] fill/update/copy buffer, buffer↔image, copy/blit image, clears |
| Командные буферы | [x] цепочка BO без ограничения размера, secondary |
| Queries | [x] occlusion, pipeline statistics, timestamp |
| WSI / swapchain | [x] Win32, BGRA8, FIFO, вывод через CPU/GDI |
| Стандартные приложения | [x] `vkcube` 10 минут с resize/minimize |
| Validation layers | [x] пробы и vkcube без ошибок |
| CTS | [~] выборка ~2660 тестов из 22 групп на 1.2: 1384 Pass, 0 Fail; остальное NotSupported |
| Vulkan 1.1–1.3, расширения | [~] 1.2 и 47 расширений; 1.3 — нет |
| Системная регистрация ICD, игры | [ ] |

## 1. Что уже сделано

### Транспорт WDDM
- [x] Устройство, контекст, paging queue, выделения 4 КиБ – 4 МиБ, CPU/GPU
  mapping, residency, GPU VA, отправка и подтверждённое завершение команд.
- [x] Раскладка адресного пространства HasVK (окно 16 ГиБ, пулы состояния по
  фиксированным адресам), переходы между командными BO.
- [~] Профиль 1096-байтного описателя выделения подобран под одну пару
  GPU/драйвер. Одно выделение до 128 МиБ, бюджет устройства 2 ГиБ; проверено
  12 живых выделений по 64 МиБ.
- [~] Каждая отправка синхронная: очередь ждёт GPU после каждого command buffer.

### Ядро ICD
- [x] Instance, physical device, device, одна очередь; fences, бинарные
  семафоры; загрузка через системный `vulkan-1.dll` и `VK_DRIVER_FILES`.
- [x] Память и буферы на реальных BO WDDM, один тип памяти (host-visible).
- [x] Descriptor sets и pipeline layout HasVK, pipeline cache с сохранением и
  импортом.
- [x] Командный буфер — цепочка BO по 64 КиБ через `MI_BATCH_BUFFER_START`;
  secondary command buffers копируются в primary (режим emit HasVK), проверены
  оба варианта наследования framebuffer и secondary длиной в несколько BO.
- [x] Все 137 команд Vulkan 1.0 разрешаются в реальные функции (кроме глобальной
  `vkEnumerateInstanceLayerProperties`, её обслуживает loader).
- [~] Barriers делают полный flush.
- [x] Events: отдельный BO, установка/сброс хостом и GPU. `MI_SEMAPHORE_WAIT` в
  WDDM-контексте не завершается никогда, поэтому `vkCmdWaitEvents` вне render
  pass разрезает командный буфер, и поток очереди ждёт события на CPU.
- [x] Push-буфер 0 адресуется относительно dynamic state base: бит `INSTPM`
  «CONSTANT_BUFFER Address Offset Disable» в WDDM-контексте сброшен.

### Compute
- [x] 342 случая на железе: 12 SPIR-V-модулей, группы 1/7/32/64, до 16 381
  элемента, сравнение с независимым расчётом на CPU.
- [x] 18 случаев storage image (RGBA8 linear и Y-tiled).
- [x] Группы 8×4×2 с dispatch 3×2×2 (прямой и indirect), shared memory с
  barrier; объявлены лимиты HasVK (до 1024 потоков в группе).
- [~] Scratch (большие локальные массивы) разрешён, но тестом не покрыт.

### Графика
- [x] Render pass, framebuffer, VS/FS из SPIR-V, draw: 6 треугольников.
- [x] Текстура, sampler, динамический UBO, D16 depth test: 12 случаев с
  попиксельной сверкой цвета и глубины.
- [x] Vertex input (два binding, смещения, UNORM8-атрибут), draw, indexed 16/32
  бит, instanced, оба indirect: попиксельная сверка.
- [x] Additive blending, stencil write + stencil-masked draw, MSAA 4x с resolve
  в render pass, динамический scissor.
- [x] Fill/update/copy buffer, buffer↔image с padded row length, copy image,
  blit 2x nearest, clear color/depth image, `vkCmdClearAttachments`: побайтно.
- [x] 47 форматов (цветные, depth, stencil): запись и чтение каждого mip-уровня
  и слоя бит в бит; 1D, 3D, cube, полная mip-цепочка.
- [x] Возможности форматов и лимиты — штатные таблицы HasVK; объявлены все
  features Vulkan 1.0, которые объявляет HasVK для Gen8.
- [~] Объявленные, но не покрытые тестами features: геометрические и
  тесселяционные шейдеры, сжатые текстуры, cube arrays, несколько viewport,
  sample shading, логические операции, depth clamp/bias, wide lines.

### Queries
- [x] Штатный `genX_query.c`: occlusion (точный счёт), pipeline statistics,
  timestamp; чтение на CPU и копирование на GPU; reset и повторное использование.
- [~] `inheritedQueries` объявлен вместе с secondary command buffers, отдельного
  теста нет. Performance queries не поддерживаются.

### Вывод на экран
- [x] `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`; BGRA8, FIFO;
  кадр копируется GPU в CPU-буфер и выводится через GDI.
- [x] Неизменённый `vkcube` v1.3.301: 600 с, 92 изменения размера, 45 циклов
  minimize/restore, выход 0.
- [x] Размеры окна от 200×150 до 1024×768; скрипт проверяет, что края окна
  закрашены. Прежняя белая полоса объяснилась: окно выходило за экран, а GDI
  не рисует невидимую часть.
- [~] Снимки окна проверяются на непустоту, без попиксельного эталона.

## 2. Рубеж A — корректный Vulkan 1.0

Порядок шагов выбран так, чтобы каждый следующий опирался на инструмент,
полученный на предыдущем.

### A1. Инструменты проверки
- [x] `tools/test-validation.ps1`: обе пробы и vkcube под
  `VK_LAYER_KHRONOS_validation` (SDK 1.4.363.1 в `C:\VulkanSDK-local`, без
  регистрации в системе), отрицательный контроль через `--force_errors`.
- [x] `tools/build-cts.ps1` собирает `deqp-vk` (CTS 1.4.5.5) в
  `C:\VulkanCTS-local` (около 5 часов). `tools/run-cts.ps1` запускает тесты без
  окон и диалогов, переживает падения и зависания, умеет выборку `-Every` и
  повтор списка `-CaseFile`; `tools/run-cts-sample.ps1` — срез по 22 группам.
- [x] Первый срез нашёл 13 отказов; все разобраны и исправлены (раскладка
  mip-уровней HALIGN, адресация push-буфера, события, texel-буферы, сцепление
  командных BO). Повторный срез: 791 Pass, 0 Fail
  (`native/artifacts/cts-sample.json`).
- [~] Нестабильный тест: `tessellation.misc_draw.tess_factor_barrier_bug`
  дважды сбросил GPU (срезы 1.0 и 1.1) и прошёл в срезе 1.2 — похоже на работу
  на грани 2-секундного таймаута Windows TDR; не доказано.
- [~] Ошибка CTS 1.4.5.5: на устройстве 1.1 не запрашивается
  `shaderDrawParameters` (фильтр `"core_feature"` против метки `"core_features"`);
  на 1.2 не проявляется.
- [ ] Полный (а не выборочный) прогон mustpass Vulkan 1.0.
- [ ] Попиксельная проверка кадра swapchain.

### A2. Обязательные возможности 1.0
- [x] Форматы: обязательный набор Vulkan 1.0 открыт штатными таблицами HasVK и
  проверен записью/чтением (47 форматов).
- [x] Изображения 1D/3D/cube, mip-уровни, массивы слоёв.
- [~] MSAA: 4x с resolve проверен; 2x/8x, `vkCmdResolveImage`, sample shading — нет.
- [ ] Вспомогательные поверхности HiZ/CCS/MCS отключены (на корректность не
  влияет, только на скорость); включение — отдельная задача рубежа C.
- [~] Stencil: тест и запись проверены; clear stencil-аспекта и раздельные
  front/back операции — нет.
- [x] Compute: 3D-группы, shared memory, indirect dispatch, лимиты HasVK.
- [x] Лимиты устройства Vulkan 1.0 — как у HasVK (кроме памяти, см. A3).
- [x] Команды копирования и clear с побайтной проверкой.
- [ ] `vkCmdResolveImage`, blit с линейным фильтром и между разными форматами.
- [x] Draw: indexed, instanced, indirect; вершинные буферы и атрибуты.
- [~] Состояние конвейера: blending (additive), scissor (dynamic), stencil, MSAA
  проверены. Без тестов: остальные режимы blending, топологии кроме
  triangle list, culling, polygon mode, depth bias, несколько render target,
  несколько subpass, input attachments.
- [~] Push constants, texel buffers, массивы дескрипторов, несколько descriptor
  sets — покрыты выборкой CTS (`binding_model`, `ubo`, `ssbo`), своих тестов нет.
- [~] Геометрические и тесселяционные шейдеры: выборки CTS `geometry` (85 Pass) и
  `tessellation` проходят, кроме проблемы с TDR выше.
- [x] Secondary command buffers и `vkCmdExecuteCommands`.
- [x] Командные буферы больше 64 КиБ.
- [x] Events: свой тест на 5 сценариев, CTS `synchronization.basic.event` и
  выборка `synchronization.op.single_queue.event` проходят.
- [ ] Sparse-память не обязательна — оставлена выключенной.
### A3. Память и синхронизация
- [~] Бюджет 2 ГиБ, проверено 768 МиБ живых выделений. Несколько типов памяти
  и одно выделение больше 128 МиБ — нет.
- [ ] Асинхронная отправка: не ждать GPU на каждом submit, сигнализировать
  fence/semaphore по событию завершения. Сейчас это главное ограничение скорости.
- [ ] Точные barriers вместо полного flush.
- [ ] Потокобезопасность: параллельная запись command buffers, публикация
  состояния из CPU shadow при записи из нескольких потоков.
- [ ] Обработка потери устройства и TDR: корректный `VK_ERROR_DEVICE_LOST`,
  восстановление без перезапуска процесса.
- [ ] Найти причину единичного сбоя compute (257 элементов без изменений),
  зафиксированного в `build/failures`.

### A4. WSI
- [ ] `VK_ERROR_OUT_OF_DATE_KHR`/`SUBOPTIMAL` во всех сценариях, пересоздание
  с `oldSwapchain`, режим IMMEDIATE, форматы SRGB.
- [ ] Вывод без копии через CPU: общий ресурс с DWM (DXGI/DComp) вместо GDI.
  Требует исследования — в Mesa этот путь завязан на D3D12, которого мы избегаем.

**Критерий рубежа A:** проходит обязательный набор CTS для Vulkan 1.0
(mustpass) либо остаётся короткий документированный список отказов;
validation layers не дают ошибок на тестах проекта и `vkcube`.

## 3. Рубеж B — паритет с HasVK (Vulkan 1.1–1.3)

- [ ] Включать возможности HasVK по одной через физические features и
  расширения, каждую — с тестом или разделом CTS. Основные группы:
  - геометрические и тесселяционные шейдеры, transform feedback;
  - multiview, variable pointers, 16-bit storage, shader draw parameters;
  - `KHR_maintenance1–4`, `get_memory_requirements2`, `bind_memory2`,
    `descriptor_update_template`, `sampler_ycbcr_conversion`;
  - timeline semaphores, `synchronization2`, `dynamic_rendering`,
    `imageless_framebuffer`, `separate_depth_stencil_layouts`;
  - buffer device address, descriptor indexing (в объёме Gen8),
    host query reset, расширенный dynamic state;
  - сжатые текстуры BC1–BC7, анизотропная фильтрация, sample shading,
    depth clamp, dual-source blending, wide lines, large points.
- [x] Возможности устройства — штатные `get_device_extensions`/`get_features`/
  `get_properties` HasVK (`anv_wddm_capabilities.c`) с ограничениями порта:
  выделение до 128 МиБ, без внешних дескрипторов, без sparse, YCbCr выключен,
  `conformanceVersion` = 0.
- [x] Vulkan 1.1: все команды 1.1 разрешаются; расширения по белому списку
  (`verified_device_extensions` в `anv_wddm_device.c`): вошедшие в ядро 1.1 и
  `driver_properties`, `host_query_reset`, `sampler_mirror_clamp_to_edge`.
  Манифест ICD объявляет `api_version` 1.1.0 — с 1.0.0 loader скрывал от
  приложений все структуры features 1.1. Срез CTS: 1093 Pass вместо 791.
- [x] Вторая партия: 24 расширения HasVK без новой инфраструктуры (хранение
  8/16 бит, раскладки, float controls, SPIR-V 1.4, memory model, renderpass2,
  resolve, imageless framebuffer, copy_commands2, multi_draw, extended dynamic
  state, maintenance4 и др.); CTS по их группам: 1180 Pass, 0 Fail.
- [x] Timeline-семафоры через эмуляцию Mesa поверх бинарного WDDM sync;
  свой тест на 5 сценариев, CTS 729 Pass.
- [x] `dynamic_rendering` и `synchronization2`; признак «внутри рендеринга» для
  событий — `gfx.layer_count`. CTS 733 Pass.
- [x] Vulkan 1.2: проверки CTS свойств/лимитов/форматов 2607 Pass. Одно
  выделение памяти теперь до 1 ГиБ (минимум спецификации), бюджет 3 ГиБ.
- [ ] YCbCr и многоплоскостные изображения.
- [ ] Vulkan 1.3: нужны `bufferDeviceAddress` (адреса A64 буферов),
  `maintenance5`, `inline_uniform_block`, `pipeline_creation_cache_control`,
  `private_data`, `subgroup_size_control`, `texel_buffer_alignment`,
  `zero_initialize_workgroup_memory` и проверка обязательных features.
- [ ] Внешняя память и семафоры для Windows (`external_memory_win32`) —
  в HasVK этого нет, решать отдельно, если понадобится приложениям.
- [ ] Обобщить профиль WDDM-выделений: другие версии драйвера Intel и другие
  Broadwell/Haswell GPU, реальные маски топологии вместо статических.

**Критерий рубежа B:** набор features и расширений совпадает с HasVK на Linux
для того же GPU, CTS для объявленной версии проходит с документированными
исключениями.

## 4. Рубеж C — пригодность для приложений

- [ ] Системная регистрация ICD (реестр) с установщиком и удалением; до этого
  момента ICD подключается только через `VK_DRIVER_FILES`.
- [ ] Производительность: асинхронная очередь, вывод без CPU-копии, HiZ/CCS,
  кэш шейдеров на диске.
- [ ] Проверка на реальных приложениях по возрастанию сложности: демо
  Sascha Willems → эмуляторы и движки → DXVK/VKD3D (требуют 1.3 и многих
  расширений из рубежа B).
- [ ] Устойчивость: длительные прогоны, утечки памяти и хэндлов, поведение при
  нехватке памяти, смене режима экрана, сне и пробуждении.
- [ ] Сборка релизного пакета, инструкция по установке, перечень поддерживаемых
  GPU и версий драйвера.

## 5. Риски и открытые вопросы

- **Закрытый интерфейс Intel.** Формат приватных данных выделений и submit
  восстановлен для одного драйвера. Обновление драйвера может сломать
  транспорт; обобщение — отдельное исследование.
- **Вывод без CPU-копии** может оказаться невозможным без D3D-пути.
- **Защитный парсер команд ядра.** Пока все нужные команды (включая чтение
  регистров статистики и TIMESTAMP) проходят, но для новых возможностей
  отдельные регистры или команды могут быть запрещены драйвером.
- **Объём CTS.** Полный прогон занимает много часов даже на быстром GPU;
  нужен выборочный запуск и хранение результатов между сессиями.

## 6. Ближайшие шаги

1. B: Vulkan 1.3 — расширения из списка выше, затем `apiVersion` 1.3 и
   проверка обязательных features и лимитов CTS.
2. B: YCbCr и многоплоскостные изображения.
3. A1: полный прогон mustpass по группам (параллельный запуск CTS ускорит).
4. A3: асинхронная отправка команд; потокобезопасность.
5. A4: `VK_ERROR_OUT_OF_DATE_KHR`, `oldSwapchain`, режим IMMEDIATE.