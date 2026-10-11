# WoWee VR — журнал этапов

## Этап 0. Подготовка (2026-10-11, завершён)

Решения — `decisions.md` п. 1–7. Команды — `..\CLAUDE.md`, раздел «Команды».

### Сделано
- **Окружение.** Уже были: Git 2.55, CMake 4.2, VS 2022 Enterprise 17.14 (MSVC 14.44), Ninja 1.13, Python 3.14, gh.
  Vulkan 1.4 (драйвер 581.57), две RTX 3090. Поставлено с согласия пользователя: LunarG Vulkan SDK 1.4.363.0
  (`C:\VulkanSDK`), RenderDoc 1.46. Без установки в систему, в `..\tools\`: vcpkg, clang-cl 23.1.3, OpenXR-SDK-Source.
- **Исходники.** Форк `AndreyKuralekh/WoWee` в `WoWee\`, `upstream` = `Kelsidavis/WoWee`, ветка `vr` от
  `upstream/master` (`b8b6242d`). Сабмодуль `extern/FidelityFX-SDK` не скачался (коммит недоступен); он нужен только
  выключенным по умолчанию опциям FSR3. В `.gitignore` добавлен `build-vr/`.
- **Сборка.** `cl.exe` клиент не собирает (18 ошибок: `M_PI`, `C2026`, `C1061` — см. `decisions.md` п. 3).
  clang-cl + Ninja + vcpkg: `wowee.exe` и `asset_extract.exe`, RelWithDebInfo, 0 ошибок, 194 предупреждения,
  около минуты с нуля. Код не менялся.
- **Данные.** `asset_extract … --expansion wotlk --expansion-subdir --locale ruRU` → `D:\WoWVR\gamedata`:
  254 228 файлов, 34,7 ГБ, `expansions\wotlk\manifest.json` (35,5 МБ), 5,5 минуты, 0 ошибок. Взяты все архивы,
  включая пользовательские патчи (`patch-4/8/C/Z`, `Patch-ruRU-A…X`). Папка клиента не изменилась: размеры и даты
  всех 176 файлов `Data\` до и после совпадают.
- **Запуск.** `..\run.cmd`: клиент стартует на RTX 3090 (Vulkan 1.4), находит данные
  («Wrath of the Lich King — 254,228 files»), загружает 5 из 5 шрифтов интерфейса, показывает экран логина.
  Проверено без человека через `WOWEE_SCREENSHOT`. Конфиг уходит в `..\userdata\`, лог — в `..\logs\wowee-<дата>-<время>.log`.
- **OpenXR.** `hello_xr` (SDK 1.1.63) собран. Активный рантайм — Virtual Desktop, VDXR 1.0.10 (реестр не менялся).
  Список расширений рантайма получен без шлема (`..\logs\hello_xr-Vulkan2-noheadset.log`), всего 31:
  - графика: `XR_KHR_vulkan_enable2` (v2), `XR_KHR_vulkan_enable` (v8), `XR_KHR_D3D11_enable`, `XR_KHR_D3D12_enable`,
    `XR_KHR_opengl_enable`;
  - слои: `XR_KHR_composition_layer_depth`, `XR_KHR_composition_layer_cylinder`, `XR_KHR_visibility_mask`;
  - руки: `XR_EXT_hand_tracking`, `XR_EXT_hand_tracking_data_source`, `XR_FB_hand_tracking_aim`, `XR_EXT_palm_pose`;
  - `XR_FB_*`: `display_refresh_rate`, `eye_tracking_social`, `face_tracking`, `face_tracking2`, `body_tracking`;
  - `XR_META_*`: `body_tracking_full_body`, `body_tracking_fidelity`, `body_tracking_calibration`, `headset_id`;
  - прочее: `XR_EXT_eye_gaze_interaction`, `XR_EXT_active_action_set_priority`, `XR_HTCX_vive_tracker_interaction`,
    `XR_MND_headless`, `XR_EXT_uuid`, `XR_EXT_debug_utils`, `XR_OCULUS_audio_device_guid`, время (`win32_convert_…`,
    `convert_timespec_time`), `XR_EXT_win32_appcontainer_compatible`.
  - **Нет** `XR_FB_foveation*`, `XR_FB_swapchain_update_state`, `XR_KHR_composition_layer_equirect*`,
    `XR_FB_passthrough`.
  Без шлема `xrGetSystem` отвечает `XR_ERROR_FORM_FACTOR_UNAVAILABLE` — как и в ArxFatalis.
- **Карта модулей** — `module-map.md` (10 разделов, по четырём проходам разведки кода).

### Итог
Этап 0 завершён: клиент собирается из форка, играбелен на локальном сервере со стоковыми данными (с двумя обходами
дефектов upstream), `hello_xr` на Vulkan виден в Quest 3, карта модулей готова. Перенесено на этап 1: русские тексты
квестов (локаль клиента и база сервера), сглаживание без MSAA, эффекты частиц (если upstream не исправит).

### Чек-лист
- [x] Вход на сервер: авторизация, список realm'ов, вход в мир — работают; ошибок Warden и опкодов нет.
- [x] Игровая проверка пользователем на стоковых данных: бег, NPC, квест, бой — «всё выглядит нормально», кроме
      пламени свечей и снаряда файрбола (см. «Эффекты частиц»). Тексты квестов английские — отложено на этап 1.
- [x] `hello_xr -g Vulkan2` и `-g Vulkan` в шлеме: кубики видны, 2688×2880 на глаз.
- [x] RenderDoc-снимок кадра в Штормграде → `module-map.md`, раздел 11 (35 тыс. draw calls, ~33 мс GPU).
- [x] `git push origin vr` — с согласия пользователя.

### hello_xr в шлеме (2026-10-11, Quest 3 через Virtual Desktop)
- `hello_xr -g Vulkan2` и `-g Vulkan`: в обоих режимах сессия создана и дошла до `XR_SESSION_STATE_FOCUSED`,
  ошибок нет, `Vulkan2` завершился штатно с кодом 0. Логи — `..\logs\hello_xr-Vulkan2-headset.log`, `…-Vulkan-headset.log`.
- Система `Meta Quest 3`, `PRIMARY_STEREO`, `FovMutable=True`, рекомендуемый размер глаза **2688×2880**, 1 сэмпл
  (максимум 16384×16384, 4 сэмпла), длина swapchain 2, до 16 слоёв, blend mode только `OPAQUE`.
- Форматы swapchain (VkFormat): цвет 43 `R8G8B8A8_SRGB` (выбран), 50 `B8G8R8A8_SRGB`, 37 `R8G8B8A8_UNORM`,
  44 `B8G8R8A8_UNORM`, 97 `R16G16B16A16_SFLOAT`; глубина 126 `D32_SFLOAT` (выбран), 130, 129, 124.
- Reference spaces: `VIEW`, `LOCAL`, `STAGE`. Контроллеры: профиль Touch (grip, grip pose, menu, haptics).
- `XR_KHR_vulkan_enable` (v1) просит у приложения instance-расширения `VK_KHR_external_memory_capabilities`,
  `…_semaphore_capabilities`, `…_fence_capabilities`, `VK_KHR_get_physical_device_properties2` и device-расширения
  `VK_KHR_dedicated_allocation`, `VK_KHR_get_memory_requirements2`, `VK_KHR_external_memory(+_win32)`,
  `VK_KHR_timeline_semaphore`, `VK_KHR_external_semaphore(+_win32)` — всё это ядро Vulkan 1.1/1.2 или есть у NVIDIA.
- Картинка в шлеме подтверждена пользователем: кубики видны.

### Потеря устройства на экране выбора персонажа (2026-10-11)
- Симптом: через секунду после появления экрана выбора персонажа — `Device lost`, `fault 0: invalid write at 0x0`.
  Авторизация, список realm'ов и список персонажей к этому моменту проходят.
- Это известная ошибка upstream: issue #154 «Device lost at character select with MSAA on» (RTX 4070 SUPER).
  Коммит `b8b6242d` (наш базовый) заявлен как исправление, но у нас на RTX 3090 (драйвер 581.57) не помог.
- **Обход: `antialiasing=0` в `..\userdata\settings.cfg`.** С ним экран выбора живёт и на стоковых данных, и на
  полном наборе с модами (проверено автоматическим входом, по 20 секунд).
- Что исключено: наша сборка clang-cl (официальный `wowee-v3.1.42-windows-x86-64.zip` падает так же); HD-моды клиента
  (на данных только из стоковых архивов падает так же); `VK_EXT_host_image_copy`, диагностические расширения NVIDIA,
  ray query (скрывались слоем `VK_LAYER_KHRONOS_profiles`); `WOWEE_VK_NO_UPLOAD_BATCH`, `…_SYNC_UPLOAD_ON_GRAPHICS`,
  `…_NO_ROBUST_BUFFERS`, `…_NO_DYNAMIC_RENDERING`, `WOWEE_RT_FORCE_SOFTWARE`, `WOWEE_SINGLE_THREAD_RECORD`.
  Validation layers (включая GPU-assisted) причину не называют.
- Что найдено временной диагностической сборкой (в репозиторий не попала): устройство теряется только если в проходе
  предпросмотра (`CharacterPreview::compositePass`, цель 640×800 MSAA 4×) выполняется `CharacterRenderer::render`;
  пустой проход безопасен, а падает любой нарисованный экземпляр — персонаж, оружие или фон.
- Инструмент: `..\tools\repro-login.ps1` — запуск с сохранённым логином, Enter на экране входа, отчёт
  «вошёл / предпросмотр / device lost». Годится для проверки будущих исправлений upstream.
- Для VR это важно: MSAA в основном проходе пока включать нельзя; сглаживание глаз придётся решать отдельно.

### Первый вход в мир (2026-10-11)
Пользователь на полном наборе данных (с модами): вход в мир, бег, разговор с NPC, взятие квеста и бой работают;
не хватало деревьев и деталей моделей, файрбол не виден, текст квеста английский при русских кнопках.
- **Моды клиента WoWee не подходят.** HD-модели и изменённые DBC ссылаются на файлы, которых в извлечённых данных
  нет (текстуры `world\expansion05…09\…`, `spells\cfx_mage_fire_casthands.m2` для файрбола): 258 `Texture not found`
  за полминуты в мире. Вероятная причина — в архивах модов нет полного listfile, а экстрактор берёт файлы только по
  именам (оригинальный клиент открывает их по хешу). На стоковых данных — 0 ненайденных текстур, деревья, повозки
  и палатки на месте (снимки `..\logs\world-stock-nohc.png` и `world-mods-nohc.png`).
  **Рабочий набор — стоковый:** `D:\WoWVR\gamedata-stock` (18 архивов от 2024-11-10 через ссылки в
  `..\tools\stock-mpq`, 198 612 файлов, 18,2 ГБ). Он теперь по умолчанию в `..\run.cmd`; полный — `run.cmd mods`.
- **Второй дефект клиента: `Failed to create VMA image (64x64)`** — десятки тысяч раз за сессию. С
  `VK_EXT_host_image_copy` клиент создаёт текстуры `R8_UNORM` с `HOST_TRANSFER` (`vk_texture.cpp:73-101`), драйвер
  NVIDIA 581.57 такое изображение не создаёт, запасного пути нет. Теряются карты смешивания слоёв террейна
  (`TerrainRenderer::createAlphaTexture` подставляет непрозрачную) и тайлы миникарты.
  **Обход без правки кода:** слой `VK_LAYER_KHRONOS_profiles` из Vulkan SDK с
  `VK_KHRONOS_PROFILES_EXCLUDE_DEVICE_EXTENSIONS=VK_EXT_host_image_copy` скрывает расширение от клиента — 0 ошибок,
  миникарта и смешивание террейна появились. Прописано в `..\run.cmd`.
- Validation layers в мире показывают несовместимость render pass'ов по `pDependencies` (`vkCmdBeginRenderPass`,
  `vkCmdDrawIndexed`): пайплайны собраны против прохода с другими зависимостями, чем тот, в котором рисуют. Появилось
  с коммитом `b8b6242d`; возможно, связано с потерей устройства при MSAA.
- Текст квестов английский: клиент представляется серверу локалью `enUS` (поле `locale` профиля экспансии), сервер
  отдаёт английские тексты. Кнопки русские, потому что FrameXML взят из ruRU-архивов. Не исправлялось.
- `..\tools\repro-login.ps1 -EnterWorld -Screenshot <png>` — автоматический вход в мир первым персонажем и снимок
  экрана; в отчёте число ошибок создания изображений и ненайденных текстур.

### Эффекты частиц — дефект upstream (2026-10-11)
- На стоковых данных пламя свечей (`GENERALCANDELABRA01.m2`) рисуется непрозрачными жёлтыми квадратами, а снаряд
  файрбола не виден совсем; костёр во дворе аббатства при этом выглядит правильно. Ошибок в логе нет, частицы
  создаются. Снимок — `..\Docs\Screenshots\WrongCandlesFire.png`.
- Официальная сборка upstream v3.1.42 (`..\run.cmd official`) на тех же данных показывает то же самое — проверено
  пользователем. Значит, это дефект upstream, а не нашей сборки clang-cl. По решению пользователя на этапе 0 не чиним.

### Известные проблемы и заметки
- В клиенте пользователя стоят HD-моды (архивы от 2024-04-28: `patch-4/8/C/Z`, `Patch-ruRU-A…X`). Модель персонажа
  из них — 248 костей при лимите клиента `MAX_BONES = 240` (`character_renderer.hpp:501`).
- Сборка перекомпилирует отслеживаемые `assets/shaders/*.spv` новым `glslc`; возвращать `git checkout -- assets/shaders`.
- Проверка обновлений клиента при старте: `TLS handshake failed: certificate verify failed` — на работу не влияет.
- У клиента нет аргументов командной строки: флаги `--vr*` из плана — либо разбор `argv`, либо `WOWEE_VR*`.
- `XR_KHR_vulkan_enable2` не встраивается в vk-bootstrap напрямую (`module-map.md`, раздел 2).
- В `login.cfg` хранится хеш, равносильный паролю: `userdata\` не выкладывать.

## Этап 1. Минимальный VR (начат 2026-10-12)

План (согласован с пользователем): 1) каркас; 2) сессия OpenXR и выбор GPU; 3) цикл кадра и моно-копия в шлем;
4) стерео двумя проходами; 5) VR-профиль качества; 6) UI-панель; 7) контроллеры; 8) отладка (статистика, дамп кадров).
Решения — `decisions.md` п. 8–11.

### Пункт 1 — каркас (2026-10-12)
- Опция CMake `WOWEE_VR` (OFF по умолчанию): при ON собираются `src/xr/xr_system.cpp`, `src/xr/xr_platform.cpp`,
  определяется `WOWEE_VR`, линкуется `OpenXR::openxr_loader` (vcpkg). При OFF в сборке нет ни файлов, ни loader'а.
- `xr::XrSystem` (`xr::system()`): при `WOWEE_VR=1` создаёт instance с `XR_KHR_vulkan_enable`, пишет в лог рантайм,
  шлем, рекомендуемый размер глаза; при `WOWEE_VR_DEBUG=1` — полный список расширений рантайма. Любая неудача —
  одно предупреждение и плоский режим.
- Вставки в чужой код: `CMakeLists.txt` (опция и блок `if(WOWEE_VR)`), `src/core/application.cpp` — include,
  `xr::system().initialize()` перед созданием окна и `shutdown()` после `window.reset()`, всё под `#ifdef WOWEE_VR`.
- Лаунчеры: `..\run-vr.cmd` = `run.cmd vr` (сборка `build-vr`, `WOWEE_VR=1`, `WOWEE_VR_DEBUG=1`, лог `wowee-vr-*.log`).
- Сообщено автору upstream: комментарий в issue #154, новые issue #155 (R8 и `VK_EXT_host_image_copy`) и #156
  (пламя свечей и снаряд файрбола).

Проверено:
- обе конфигурации собираются (`build`, `build-vr`), новых предупреждений в `src/xr` нет;
- `run-vr.cmd` без шлема: `VR: OpenXR runtime VirtualDesktopXR 1.0.10` → `no headset is connected to the runtime` →
  `not available - continuing as the flat client`, клиент доходит до экрана логина;
- плоская сборка и VR-сборка без `WOWEE_VR` не пишут ни одной строки `VR:`.

Не проверено:
- ветка «шлем подключён» (строки `VR: headset …` и `VR: recommended eye image …`) — нужен надетый шлем;
- Android-ветка `xr_platform.cpp` не компилировалась.
