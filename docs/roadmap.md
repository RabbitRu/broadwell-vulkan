# План реализации Vulkan для Iris 6100 (Broadwell) под Windows

Состояние на 2026-11-18. Цель — ICD на основе Mesa HasVK поверх WDDM-драйвера
Intel `20.19.15.5171`, без Intel UMD и D3D12. Потолок для этой GPU — Vulkan 1.3:
именно его объявляет upstream HasVK для Gen8 (Vulkan 1.4 HasVK не поддерживает).

Документ разделён на две части: **что уже сделано** (раздел 1, только готовое и
проверенное на железе) и **что осталось** (раздел 2, только несделанное, по
этапам). Порядок этапов в разделе 2 — текущий, его можно менять.

**Приоритет:** сначала снимались риски — то, что может сделать проект
невозможным или потребовать другой архитектуры. Этап 0 выполнен (вывод без
копии — раздел 1.4, ограничения ядра — 1.5, закрытый интерфейс Intel — 1.6).
Этап 1 (отправка команд и синхронизация) и этап 2 (Vulkan 1.3) тоже
выполнены (разделы 1.1–1.2); дальше — этапы 3–6.

## 1. Сделано

Сейчас ICD объявляет **Vulkan 1.3** и 104 расширения устройства. Выборочный
прогон CTS по 22 группам (~2660 тестов): 1574 Pass, 1 Fail — известное
зависание `tess_factor_barrier_bug` (этап 4) и ResourceError следующего за ним
теста, который отдельно проходит (`native/artifacts/cts-sample.json`; свежий
итог выборки — `build/cts-sample.json`).

### 1.1 Транспорт WDDM
- Устройство, контекст, paging queue, выделения, CPU/GPU-отображения,
  residency, фиксированные GPU-адреса, отправка команд и подтверждённое
  завершение.
- Раскладка адресного пространства HasVK: окно 16 ГиБ, пулы состояния по
  фиксированным адресам.
- Одно выделение — до 1 ГиБ (минимум спецификации 1.1+), бюджет устройства
  3 ГиБ, до 65536 выделений; проверены одно выделение в 1 ГиБ и 768 МиБ живых
  выделений с записью GPU.
- Сброс кэша CPU по способу использования буфера: память приложения — только
  пока отображена; пулы состояния — только перед отправкой и только до их
  верхней отметки (выделенная часть, а не все 1,8 МиБ); командные буферы —
  только свои и только перед отправкой (после — одна строка маркера). Пустая
  отправка: 0,72 мс (было 3,5 мс), при 512 МиБ живой памяти — 0,75 мс (было
  651 мс), восемь буферов в одной отправке — 0,52 мс на буфер. Отображения
  кэшируемые (чтение ~18 ГБ/с), поэтому сброс нужен.

- Все командные буферы одного `vkQueueSubmit` и запись счётчика завершения
  идут одной отправкой в ядро: хвост каждого буфера при отправке становится
  переходом на следующий. Отдельные отправки — только у ожиданий событий и
  повторно отправленного (simultaneous-use) буфера. Восемь буферов в одной
  отправке: 0,1–0,25 мс на буфер (было 0,52 мс). Проверка порядка — тест
  `submitOrder` в `IrisGraphicsProbe`.
- Неблокирующая очередь: поток очереди не ждёт GPU после отправки;
  отдельный поток завершения ждёт её, проверяет маркеры, защитные области и
  счётчик и сигналит все синхронизации отправки разом. Entry-BO — кольцо из
  64 слотов; командный буфер переиспользуется только после проверки его
  прошлого выполнения. Отправки подряд: 0,77 мс против 1,24–1,30 мс с
  ожиданием каждой (`HASVK_WDDM_SYNC_SUBMIT=1` включает прежний режим).
  Основная цена отправки — планирование в драйвере ядра, поэтому объединение
  буферов даёт больше, чем асинхронность.
- Вывод окна тоже не ждёт GPU: копия кадра в поверхность окна ждётся и
  проверяется перед следующим кадром (и перед уничтожением swapchain).
  `vkcube`, 1000 кадров: 8,3 с против 8,7 с с ожиданием каждого кадра
  (`HASVK_WDDM_SYNC_PRESENT=1`) и 12,1–12,5 с до неблокирующей очереди.
  Отправки, разрезанные ожиданием события, остаются блокирующими: CPU всё
  равно должен увидеть событие перед продолжением.

### 1.2 Ядро ICD и возможности
- Instance, physical device, device, одна очередь графики и compute; fences,
  бинарные и timeline-семафоры (эмуляция Mesa поверх бинарного WDDM sync).
- Лимиты, features и расширения — штатные таблицы HasVK
  (`anv_wddm_capabilities.c`), поверх них ограничения порта: без внешних
  дескрипторов, без sparse, без capture/replay адресов, YCbCr выключен,
  `conformanceVersion` = 0. Расширения включаются только по проверенному
  списку (`verified_device_extensions` в `anv_wddm_device.c`).
- **Vulkan 1.3**: `apiVersion`, `vkEnumerateInstanceVersion` и манифест ICD
  объявляют 1.3; включены обязательные для 1.3 расширения (`inline_uniform_block`,
  `private_data`, `subgroup_size_control`, `texel_buffer_alignment`,
  `zero_initialize_workgroup_memory`, `shader_integer_dot_product`,
  `pipeline_creation_cache_control`, `pipeline_creation_feedback`,
  `shader_demote_to_helper_invocation`, `shader_terminate_invocation`,
  `image_robustness`, `tooling_info`, `shader_non_semantic_info`).
  `tools/run-cts-vk13.ps1`: проверки features и лимитов 1.3
  (`api.info.vulkan1p3*`) и группы этих расширений — 1853 Pass, 0 Fail.
- Дополнительные расширения HasVK (этап 3), прицельный CTS
  (`tools/run-cts-groups.ps1`), 0 Fail:
  - шейдерные и отчётные (19): `EXT_shader_subgroup_ballot/vote`,
    `EXT_shader_atomic_float`, `KHR_shader_clock`, `KHR_shader_expect_assume`,
    `KHR_shader_relaxed_extended_instruction`,
    `KHR_shader_subgroup_uniform_control_flow`, `EXT_shader_replicated_composites`,
    `NV_compute_shader_derivatives`, `KHR_workgroup_memory_explicit_layout`,
    `INTEL_shader_integer_functions2`, `GOOGLE_decorate_string`,
    `GOOGLE_hlsl_functionality1`, `GOOGLE_user_type`, `EXT_shader_module_identifier`,
    `KHR_pipeline_executable_properties`, `EXT_debug_marker`,
    `KHR_deferred_host_operations`, `VALVE_mutable_descriptor_type` —
    1701 Pass;
  - состояние и растеризация (16): `EXT_border_color_swizzle`,
    `EXT_custom_border_color` (с пулом цветов границы, как в upstream),
    `EXT_color_write_enable`, `EXT_depth_clamp_control`, `EXT_depth_clamp_zero_one`,
    `EXT_depth_clip_enable`, `EXT_image_2d_view_of_3d`, `EXT_image_view_min_lod`,
    `EXT_line_rasterization`, `EXT_non_seamless_cube_map`,
    `EXT_primitive_topology_list_restart`, `EXT_provoking_vertex`,
    `EXT_vertex_attribute_divisor`, `KHR_push_descriptor`, `KHR_maintenance5`,
    `EXT_buffer_device_address`;
  - предикаты и transform feedback (3): `EXT_conditional_rendering`,
    `EXT_transform_feedback`, `EXT_primitives_generated_query` (в белые списки
    `vkCreateBuffer` и `vkCreateQueryPool` добавлены их флаги буферов и типы
    запросов) — 1620 Pass (`conditional_rendering`, `transform_feedback.simple`,
    каждый 40-й из `transform_feedback.primitives_generated_query`);
  - выключенные ранее из-за отказов CTS (5), исправлены ошибки порта:
    `EXT_depth_clip_control` — driconf на Windows не читается, и
    `drirc.quality.lower_depth_range_rate` оставался 0 вместо 1, обнуляя
    смещение глубины viewport (m32); то же чинит depth clamp с четырьмя
    viewport. `EXT/KHR_robustness2` — порт не выставлял
    `device->robust_buffer_access`, и шейдеры собирались без проверки границ
    UBO/SSBO. `KHR_maintenance6` — `vkBindBufferMemory2` не заполнял
    `VkBindMemoryStatus`. `EXT_sample_locations` — не было
    `vkGetPhysicalDeviceMultisamplePropertiesEXT` (функция из upstream
    `anv_device.c`, который порт заменяет своим кодом). Для разбора таких
    случаев есть `HASVK_WDDM_EXTRA_EXTENSIONS=VK_EXT_a,VK_EXT_b`: включает
    выключенные расширения upstream без пересборки. Прицельный CTS
    (`depth_clip_control`, `depth_clamp`, `maintenance6`, каждый 20-й из
    `robustness2`, каждый 2-й из `robustness.buffer_access` и
    `sample_locations_ext`) — 2778 Pass, 0 Fail.
- Командные буферы: цепочка BO без ограничения размера, secondary command
  buffers (копируются в primary).
- Events: отдельный BO; `MI_SEMAPHORE_WAIT` в WDDM-контексте не работает,
  поэтому `vkCmdWaitEvents` вне рендеринга разрезает батч, и очередь ждёт
  события на CPU. Внутри прохода рендеринга ждать нечего: события там могут
  быть установлены только раньше по порядку отправки (установка с хоста после
  отправки запрещена, `VUID-vkCmdWaitEvents-pEvents-01163`), а очередь
  выполняет всё строго по порядку, поэтому остаётся только барьер памяти.
  Проверено режимом 8 теста отрисовки: инстансы записываются передачами,
  событие ставится после них, отрисовка ждёт его внутри прохода — все пиксели
  совпадают.
- Барьеры — штатные HasVK: сбросы и инвалидации кэшей по маскам стадий и
  доступа, а не полный сброс на каждый барьер. CTS `memory.pipeline_barrier`,
  барьеры `synchronization`/`synchronization2`, `synchronization.basic`:
  580 Pass, 0 Fail.
- Потокобезопасность: параллельная запись командных буферов из нескольких
  потоков, выделение памяти, буферов и событий, отправка под мьютексом
  приложения (тест `threads` в `IrisGraphicsProbe`: 4 потока × 40 отправок).
- Два типа памяти, как у upstream HasVK для GPU с общим LLC: тип 0 —
  `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT | HOST_CACHED` (кэшируемое
  отображение, когерентность — сбросом кэша вокруг отправок), тип 1 — только
  `DEVICE_LOCAL`: выделяется без отображения в CPU (нет `Lock2`, не занимает
  адресное пространство процесса, никогда не сбрасывается). Тест
  `deviceLocalMemory`: `vkMapMemory` отказывает, GPU пишет и копирует.
- Редкий сбой compute 16.11 (`build/failures`, «mismatch at word 16») на
  нынешнем коде не воспроизводится: 1200 итераций того же случая (1000 из
  них без сброса кэша CPU) и 5 прогонов SPIR-V-теста подряд — без сбоев
  (`native/artifacts/compute-rootcause.json`). Сброс кэша на этот случай не
  влияет, то есть тогда помог не он, а вероятнее ожидание настоящего
  завершения GPU, добавленное тем же изменением; исходный код до сбоя в git
  не сохранился, поэтому причина не доказана.
- Потеря устройства (`tools/test-device-lost.ps1`): при настоящем зависании
  GPU (бесконечный compute-шейдер, Windows сбрасывает GPU по TDR) ожидание
  забора возвращает `VK_ERROR_DEVICE_LOST` через ~4,5 с, дальнейшие вызовы
  сразу сообщают о потере и не блокируются, устройство уничтожается, новое
  `VkDevice` в том же процессе работает.
- Адреса буферов (`VK_KHR_buffer_device_address`, A64-доступ); CTS
  `buffer_device_address`, `ssbo.phys`, `physical_pointers`: 629 Pass, 0 Fail.

### 1.3 Шейдеры, compute, графика
- SPIR-V → NIR → ELK → Gen8 для всех стадий: VS, TCS, TES, GS, FS, CS.
- Compute: 3D-группы до 1024 потоков, shared memory, indirect dispatch.
- Графика: vertex input, все формы draw (indexed, instanced, indirect,
  indirect count, multi draw), blending, stencil, depth, MSAA 4x с resolve,
  dynamic state, dynamic rendering, renderpass2, imageless framebuffer.
- 47 форматов (цвет, depth, stencil) и 1D/2D/3D/cube-изображения с
  mip-уровнями и слоями; команды копирования, blit и clear.
- Queries: occlusion, pipeline statistics, timestamp.
- Исправленные ошибки порта, найденные CTS: выравнивание mip-уровней (HALIGN),
  относительная адресация push-буфера 0, события, texel-буферы.

### 1.4 Вывод на экран
- `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`, BGRA8, FIFO.
- **Вывод без копии через CPU и без D3D** (`anv_wddm_present.c`): GPU копирует
  кадр (BLORP) в разделяемую поверхность окна, отправка идёт с флагом
  `PresentRedirected` и токеном `(0x80000000 | HWND) << 32 | дескриптор`,
  DWM показывает поверхность. Так выводит кадры OpenGL-драйвер Intel того же
  пакета (снято трассировкой, `tools/trace-gl-present.ps1`). Поверхность
  создаёт наш транспорт по снятому описателю (1096 байт Intel + 104 байта
  данных рантайма): B8G8R8A8, Y-тайлинг, шаг `align(ширина × 4, 128)`, высота
  кратна 32, без отображения в CPU; размер — всё окно с рамкой, кадр
  копируется со смещением клиентской области. Условие — приватные данные
  контекста как у OpenGL-драйвера (без них DWM показывает чёрное); выборка CTS
  с ними — 0 Fail. D3D, Intel UMD и ICD в процессе не загружаются.
- Откат на прежний путь (копия в CPU-буфер + GDI) кадр за кадром, если ядро
  отказывает в выводе (`PRESENT_OCCLUDED` при спящем экране);
  `HASVK_WDDM_PRESENT=gdi` включает его принудительно.
- Неизменённый `vkcube`: 40 с с resize и minimize под validation layers —
  3063 кадра через поверхность окна, 0 через GDI, 0 ошибок; окна от 200×150
  до 1280×720, в том числе частично за краем экрана (GDI там рисовал пусто).
  Скорость пока равна GDI-пути: узкое место — синхронная отправка (этап 1).
- Проверки: `tools/test-present-redirect.ps1` (своя поверхность, цвета и
  раскладка по пикселям экрана), `tools/capture-vkcube.ps1 -Present redirect|gdi`.

### 1.5 Проверенные ограничения ядра
`tools/test-kernel-limits.ps1` (`native/artifacts/kernel-limits.json`): всё, что
HasVK пишет в регистры и чем пользуется из MI-команд, работает в нашем
WDDM-контексте (непривилегированный PPGTT-батч), 0 запретов из 47 проверок.
- Регистры на запись (`MI_LOAD_REGISTER_IMM/MEM`, чтение
  `MI_STORE_REGISTER_MEM`): CS_GPR0–15, `MI_PREDICATE_SRC0/1/DATA`,
  `3DPRIM_*`, `GPGPU_DISPATCHDIMX/Y/Z`, `SO_WRITE_OFFSET0–3`, счётчики SO,
  `CACHE_MODE_1` и `INSTPM` (маскируемые), `L3CNTLREG` (переключение между
  двумя проверенными конфигурациями Broadwell и обратно).
- На чтение: `TIMESTAMP`, счётчики статистики конвейера.
- Команды: `MI_LOAD_REGISTER_REG`, `MI_MATH` (ADD/SUB/AND/OR/XOR),
  `MI_PREDICATE`, `MI_COPY_MEM_MEM`, `MI_STORE_DATA_IMM`,
  `MI_REPORT_PERF_COUNT`, батч второго уровня `MI_BATCH_BUFFER_START`,
  самомодифицирующийся батч (как в `mi_builder`).
- Не работает только `MI_SEMAPHORE_WAIT` (ожидание не завершается); обойдено
  ожиданием событий на CPU (раздел 1.2).

### 1.6 Закрытый интерфейс Intel
- Всё, что порт передаёт ядру сверх документированных структур D3DKMT,
  описано в `docs/intel-interface.md`: описатели выделений и поверхности
  окна, данные рантайма, заголовок отправки, приватные данные контекста,
  токен вывода — откуда снято и какой пробой проверяется.
- ICD работает только с драйвером `20.19.15.5171` и адаптером `8086:162b`; на
  другом не показывает физических устройств и ничего не отправляет в ядро.
  Порядок переноса на другую версию — там же.

### 1.7 Инструменты проверки
- Аппаратные пробы (`tools/test-all.ps1`, `test-graphics.ps1`) с побайтной или
  попиксельной сверкой и защитными областями.
- Validation layers (`tools/test-validation.ps1`): пробы и vkcube без ошибок.
- CTS 1.4.5.5 (`C:\VulkanCTS-local`): сборка (`tools/build-cts.ps1`), запуск
  без окон с переживанием падений (`tools/run-cts.ps1`), срезы по группам
  (`tools/run-cts-sample.ps1`, `tools/run-cts-extensions.ps1`).
- Отладка падений через lldb (`tools/debug-icd.ps1`), профиль отправки
  (`HASVK_WDDM_PROFILE=1`), дамп регистров контекста (`HASVK_WDDM_DUMP_REGISTERS=1`).

## 2. Осталось

### Этап 3. Полнота относительно HasVK
1. YCbCr и многоплоскостные изображения (`KHR_sampler_ycbcr_conversion`,
   `EXT_ycbcr_image_arrays`). Остальные выключенные расширения upstream —
   только для Linux: dma-buf, DRM, fd, `pci_bus_info`, `queue_family_foreign`.
2. MSAA 2x/8x, `vkCmdResolveImage`, blit с линейным фильтром.
3. Вспомогательные поверхности HiZ/CCS/MCS (скорость, не корректность).
4. Обобщить профиль WDDM-выделений на другие версии драйвера и GPU
   Broadwell/Haswell.

### Этап 4. Полный прогон CTS
1. Полный mustpass для объявленной версии по группам (сейчас — только
   выборки); параллельный запуск нескольких процессов `deqp-vk`.
2. Список ожидаемых отказов с причинами.
3. Известное: `tessellation.misc_draw.tess_factor_barrier_bug` вешает GPU
   (сброс по TDR, тест получает `VK_ERROR_DEVICE_LOST` через ~4 с; раньше
   изредка проходил) — выяснить, зависание это или нагрузка дольше 2 с;
   ошибка CTS 1.4.5.5 с `shaderDrawParameters` на устройствах 1.1.

### Этап 5. Вывод на экран
1. `VK_ERROR_OUT_OF_DATE_KHR`/`SUBOPTIMAL`, пересоздание с `oldSwapchain`,
   режим IMMEDIATE, форматы SRGB.
2. Попиксельная проверка кадра swapchain.
3. Копия кадра в поверхность окна в той же отправке, что и работа
   приложения (сейчас — отдельная отправка на кадр).
4. Поверхность для окон с нестандартной геометрией (без рамки DWM,
   дочерние окна) и для нескольких мониторов с разным DPI.

### Этап 6. Пригодность для приложений
1. Системная регистрация ICD (реестр) с установщиком и удалением.
2. Проверка на реальных приложениях: демо → движки → DXVK/VKD3D.
3. Устойчивость: длительные прогоны, утечки, нехватка памяти, сон и
   пробуждение.
4. Релизный пакет, инструкция, список поддерживаемых GPU и драйверов.

## 3. Риски

- **Закрытый интерфейс Intel** — описан и ограничен одной версией драйвера
  (раздел 1.6): на другой ICD безопасно не включается. Остаётся неизвестным,
  насколько значения стабильны между версиями, — проверить нельзя без другой
  версии драйвера; перенос — по `docs/intel-interface.md`.
- **Вывод без CPU-копии** — снят: вывод через перенаправленную отправку
  работает в ICD без D3D (раздел 1.4). Остаточный риск — описатель
  поверхности снят с одного драйвера, как и остальные описатели.
- **Ограничения ядра** — проверены (раздел 1.5): кроме `MI_SEMAPHORE_WAIT`
  запретов нет. Остаточный риск — команды, которых HasVK пока не использует
  в нашей конфигурации (perf-запросы с настройкой OA-регистров).
- **Объём CTS.** Полный прогон — миллионы тестов; нужны параллельность и
  хранение результатов между сессиями.
