# WoWee VR — журнал этапов

## Этап 0. Подготовка (начат 2026-10-11)

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

### Осталось / ждёт пользователя
- [ ] Запустить сервер и войти: авторизация, список realm'ов, вход в мир, ошибки опкодов в логе.
- [ ] Игровая проверка: бег, разговор с NPC, квест, бой. Отдельно — кириллица и названия из DBC (локаль ruRU).
- [ ] `hello_xr -g Vulkan2` и `-g Vulkan` в шлеме: картинка, рекомендуемый размер swapchain'ов.
- [ ] RenderDoc-снимок кадра в городе → раздел в `module-map.md`.
- [ ] `git push` ветки `vr` — только с согласия пользователя.

### Известные проблемы и заметки
- Сборка перекомпилирует отслеживаемые `assets/shaders/*.spv` новым `glslc`; возвращать `git checkout -- assets/shaders`.
- Проверка обновлений клиента при старте: `TLS handshake failed: certificate verify failed` — на работу не влияет.
- У клиента нет аргументов командной строки: флаги `--vr*` из плана — либо разбор `argv`, либо `WOWEE_VR*`.
- `XR_KHR_vulkan_enable2` не встраивается в vk-bootstrap напрямую (`module-map.md`, раздел 2).
- В `login.cfg` хранится хеш, равносильный паролю: `userdata\` не выкладывать.
