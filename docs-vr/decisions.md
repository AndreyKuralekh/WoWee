# WoWee VR — принятые решения

Кратко: дата, решение, причина. Новые решения — внизу.

## 2026-10-11 — этап 0

1. **Ветка `vr` от `upstream/master` (`b8b6242d`, v3.1.42).** Upstream очень активен (десятки коммитов в неделю),
   поэтому подтягивать его нужно часто, а вставки в чужие файлы держать минимальными.

2. **Форк `AndreyKuralekh/WoWee` (публичный) как `origin`, `Kelsidavis/WoWee` как `upstream`.** Код MIT. Данных
   Blizzard в репозитории нет и не будет; `.gitignore` upstream уже исключает `Data/*`, `*.MPQ`, `*.log`, `build/`,
   мы добавили только `build-vr/`.

3. **Тулчейн: clang-cl 23.1.3 (портативный, `..\tools\llvm`) поверх окружения Visual Studio 2022 (заголовки, CRT и
   `link.exe` MSVC 14.44) + vcpkg `x64-windows` + LunarG Vulkan SDK 1.4.363; генератор Ninja, RelWithDebInfo.**
   Причины выбора окружения MSVC: отладчик VS и PDB, RenderDoc, validation layers, единообразие с ArxFatalis.
   Почему не `cl.exe`: CI upstream собирает Windows только в MSYS2, и текущий `master` компилятором MSVC не
   собирается — `C2026` (строковые литералы Lua-сниппетов длиннее 16 КБ, `addon_lua_snippets.hpp`) и `C1061`
   (вложенность блоков, `game_screen_minimap.cpp`, `lua_system_api.cpp`) — жёсткие лимиты `cl.exe`, флагами не
   обходятся. clang-cl даёт тот же ABI и те же библиотеки без правки кода. `asset_extract` собирается и `cl.exe`.
   Настройки вместо правок кода: `/D_USE_MATH_DEFINES` (иначе нет `M_PI` в заголовках MSVC) и
   `-DWOWEE_WARNINGS_AS_ERRORS=OFF` (194 предупреждения clang-cl, upstream этим компилятором не проверяет).
   Генератор Ninja, а не VS: с ним проще подставить портативный clang-cl; сборка с нуля — около минуты.
   Запасной путь — MSYS2 UCRT64, как в CI.

4. **vcpkg — локальный клон в `..\tools\vcpkg`, классический режим** (`-DVCPKG_MANIFEST_MODE=OFF`), а не манифест
   из `vcpkg.json` репозитория. Причины: в манифесте нет `stormlib`, нужного для `asset_extract`; в режиме манифеста
   зависимости собирались бы заново для каждой папки сборки (`build` и `build-vr`).

5. **Данные игры вне репозитория и вне диска C:** извлечение в `D:\WoWVR\gamedata` (на C: 54 ГБ свободно, MPQ
   клиента с пользовательскими патчами — 24,4 ГБ). Клиент находит данные через `WOW_DATA_PATH`. Пользовательские
   файлы — в `..\userdata\` через `WOWEE_CONFIG_ROOT`; в `login.cfg` лежит хеш, равносильный паролю, поэтому
   `userdata\` никуда не выкладывается.

6. **OpenXR loader — vcpkg-порт `openxr-loader`** (1.1.63, та же версия, что в OpenXR-SDK-Source). Причины: тот же
   механизм, что у остальных зависимостей (`find_package(OpenXR CONFIG)`, DLL кладётся рядом с exe автоматически),
   одна команда на новой машине. Клон `..\tools\OpenXR-SDK-Source` остаётся для `hello_xr` и слоёв проверки API.
   На Android loader в любом случае берётся отдельно (AAR Khronos), поэтому выбор для Windows на этап 4 не влияет.

7. **Документация — в репозитории, `docs-vr/`**, версионируется вместе с веткой `vr`; имя не пересекается с
   `docs/` upstream.
