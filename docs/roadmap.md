# План реализации Vulkan для Iris 6100 (Broadwell) под Windows

Состояние на 2026-11-17. Цель — ICD на основе Mesa HasVK поверх WDDM-драйвера
Intel `20.19.15.5171`, без Intel UMD и D3D12. Потолок для этой GPU — Vulkan 1.3:
именно его объявляет upstream HasVK для Gen8 (Vulkan 1.4 HasVK не поддерживает).

Документ разделён на две части: **что уже сделано** (раздел 1, только готовое и
проверенное на железе) и **что осталось** (раздел 2, только несделанное, по
этапам). Порядок этапов в разделе 2 — текущий, его можно менять.

## 1. Сделано

Сейчас ICD объявляет **Vulkan 1.2** и 48 расширений устройства. Выборочный
прогон CTS по 22 группам (~2660 тестов): 1384 Pass, 0 Fail
(`native/artifacts/cts-sample.json`).

### 1.1 Транспорт WDDM
- Устройство, контекст, paging queue, выделения, CPU/GPU-отображения,
  residency, фиксированные GPU-адреса, отправка команд и подтверждённое
  завершение.
- Раскладка адресного пространства HasVK: окно 16 ГиБ, пулы состояния по
  фиксированным адресам.
- Одно выделение — до 1 ГиБ (минимум спецификации 1.1+), бюджет устройства
  3 ГиБ, до 65536 выделений; проверены одно выделение в 1 ГиБ и 768 МиБ живых
  выделений с записью GPU.
- Сброс кэша CPU только для служебных BO и памяти приложения, отображённой в
  данный момент (раньше — для всей памяти): отправка при 512 МиБ живой памяти
  стоит 3,4 мс вместо 651 мс. Отображения кэшируемые (18 ГБ/с на чтение),
  поэтому сброс нужен.

### 1.2 Ядро ICD и возможности
- Instance, physical device, device, одна очередь графики и compute; fences,
  бинарные и timeline-семафоры (эмуляция Mesa поверх бинарного WDDM sync).
- Лимиты, features и расширения — штатные таблицы HasVK
  (`anv_wddm_capabilities.c`), поверх них ограничения порта: без внешних
  дескрипторов, без sparse, без capture/replay адресов, YCbCr выключен,
  `conformanceVersion` = 0. Расширения включаются только по проверенному
  списку (`verified_device_extensions` в `anv_wddm_device.c`).
- Все команды Vulkan 1.0, 1.1 и 1.2 разрешаются; манифест ICD объявляет 1.2.
- Командные буферы: цепочка BO без ограничения размера, secondary command
  buffers (копируются в primary).
- Events: отдельный BO; `MI_SEMAPHORE_WAIT` в WDDM-контексте не работает,
  поэтому `vkCmdWaitEvents` вне рендеринга разрезает батч, и очередь ждёт
  события на CPU.
- Адреса буферов (`VK_KHR_buffer_device_address`, A64-доступ).

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
- `VK_KHR_surface`, `VK_KHR_win32_surface`, `VK_KHR_swapchain`, BGRA8, FIFO;
  кадр копируется GPU в CPU-буфер и выводится через GDI.
- Неизменённый `vkcube` 10 минут с resize и minimize; окна от 200×150 до
  1024×768.

### 1.5 Инструменты проверки
- Аппаратные пробы (`tools/test-all.ps1`, `test-graphics.ps1`) с побайтной или
  попиксельной сверкой и защитными областями.
- Validation layers (`tools/test-validation.ps1`): пробы и vkcube без ошибок.
- CTS 1.4.5.5 (`C:\VulkanCTS-local`): сборка (`tools/build-cts.ps1`), запуск
  без окон с переживанием падений (`tools/run-cts.ps1`), срезы по группам
  (`tools/run-cts-sample.ps1`, `tools/run-cts-extensions.ps1`).
- Отладка падений через lldb (`tools/debug-icd.ps1`), профиль отправки
  (`HASVK_WDDM_PROFILE=1`), дамп регистров контекста (`HASVK_WDDM_DUMP_REGISTERS=1`).

## 2. Осталось

### Этап 1. Отправка команд и синхронизация
1. Сократить сброс кэша CPU: сейчас 6,6 мс из ~7 мс отправки — `clflush`
   ~21 МиБ служебных BO; ожидание GPU — 0,26 мс. Сбрасывать только то, что CPU
   менял (до отправки) и будет читать (после).
2. Асинхронная отправка: не ждать GPU после каждого командного буфера,
   объединять несколько буферов в одну отправку.
3. Точные barriers вместо полного flush.
4. Потокобезопасность: параллельная запись командных буферов.
5. Обработка потери устройства и TDR: корректный `VK_ERROR_DEVICE_LOST`,
   восстановление без перезапуска процесса.
6. Несколько типов памяти (например, device-local без отображения).
7. Найти причину редких сбоев compute: 257 элементов без изменений
   (`build/failures`) и один сброс GPU в сыром SPIR-V-тесте.

### Этап 2. Vulkan 1.3
1. Включить и проверить по CTS обязательные для 1.3 расширения:
   `inline_uniform_block`, `private_data`, `subgroup_size_control`,
   `texel_buffer_alignment`, `zero_initialize_workgroup_memory`,
   `shader_integer_dot_product`, `pipeline_creation_cache_control`,
   `pipeline_creation_feedback`, `shader_demote_to_helper_invocation`,
   `shader_terminate_invocation`, `image_robustness`, `tooling_info`.
2. Проверить CTS для уже включённого `buffer_device_address`.
3. Поднять `apiVersion` до 1.3, пройти проверки CTS обязательных features и
   лимитов.

### Этап 3. Полнота относительно HasVK
1. YCbCr и многоплоскостные изображения.
2. Остальные расширения HasVK: `maintenance5/6`, `push_descriptor`,
   `robustness2`, `custom_border_color`, `line_rasterization`,
   `conditional_rendering`, `transform_feedback`, `descriptor_indexing`.
3. MSAA 2x/8x, `vkCmdResolveImage`, blit с линейным фильтром.
4. Вспомогательные поверхности HiZ/CCS/MCS (скорость, не корректность).
5. Обобщить профиль WDDM-выделений на другие версии драйвера и GPU
   Broadwell/Haswell.

### Этап 4. Полный прогон CTS
1. Полный mustpass для объявленной версии по группам (сейчас — только
   выборки); параллельный запуск нескольких процессов `deqp-vk`.
2. Список ожидаемых отказов с причинами.
3. Известное: `tessellation.misc_draw.tess_factor_barrier_bug` нестабилен
   (два сброса GPU, один Pass) — вероятно, на грани 2-секундного TDR;
   ошибка CTS 1.4.5.5 с `shaderDrawParameters` на устройствах 1.1.

### Этап 5. Вывод на экран
1. `VK_ERROR_OUT_OF_DATE_KHR`/`SUBOPTIMAL`, пересоздание с `oldSwapchain`,
   режим IMMEDIATE, форматы SRGB.
2. Попиксельная проверка кадра swapchain.
3. Вывод без копии через CPU (DXGI/DComp) — требует исследования, в Mesa этот
   путь завязан на D3D12.

### Этап 6. Пригодность для приложений
1. Системная регистрация ICD (реестр) с установщиком и удалением.
2. Проверка на реальных приложениях: демо → движки → DXVK/VKD3D.
3. Устойчивость: длительные прогоны, утечки, нехватка памяти, сон и
   пробуждение.
4. Релизный пакет, инструкция, список поддерживаемых GPU и драйверов.

## 3. Риски

- **Закрытый интерфейс Intel.** Формат описателей выделений и отправки
  восстановлен для одной версии драйвера; обновление драйвера может сломать
  транспорт.
- **Вывод без CPU-копии** может оказаться невозможным без D3D.
- **Ограничения ядра.** `MI_SEMAPHORE_WAIT` уже оказался недоступен; другие
  команды или регистры тоже могут быть запрещены.
- **Объём CTS.** Полный прогон — миллионы тестов; нужны параллельность и
  хранение результатов между сессиями.
