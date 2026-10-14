# WoWee VR — карта модулей

Снято с ветки `vr` = `upstream/master` `b8b6242d` (клиент называет себя v3.1.42), 2026-10-11. Пути — от корня репозитория, номера строк
актуальны для этого коммита и съедут после слияний с upstream: искать по именам функций.
Кода OpenXR, multiview и `viewMask` в клиенте нет. Единственное упоминание — комментарий в
`src/rendering/vk_context.cpp:837`: dynamic rendering включён как первый шаг к multiview-стерео.

Пометка «не проверено» означает: по коду не трассировалось, перед использованием на этапе 1 перечитать.

## 0. Единицы и координаты

- 1 единица рендера = 1 ярд WoW (0,9144 м), без масштабирования. Скорости в ярдах/с: бег 7,0, шаг 2,5, назад 4,5
  (`include/rendering/camera_controller.hpp:763`).
- Рендер Z-up: `renderX = wowY`, `renderY = wowX`, `renderZ = wowZ` (`include/core/coordinates.hpp:15`);
  `canonicalToRender` / `renderToCanonical` (`:98`), сервер ↔ canonical — `serverToCanonical` (`:38`).
- Yaw рендера (градусы) = canonical (радианы) + 90°: `characterYawDegToCanonical` / `canonicalToCharacterYawDeg` (`:89`).
- Рост: человек-мужчина 2,13 ярда, кость головы на 1,843; гном 1,17 (`camera_controller.hpp:563`). Высота точки
  обзора берётся из кости головы: `pivotHeightFor()` (`camera_controller.hpp:510`),
  `CharacterRenderer::getInstanceKeyBonePivotZ(kKeyBoneHead)` (`src/rendering/camera_controller.cpp:2447`).
- OpenXR: Y-up, метры, −Z вперёд. Нужна одна матрица «XR → рендер» (поворот осей + масштаб 1/0,9144 × масштаб роста).

## 1. Главный цикл

| Что | Где |
|---|---|
| `main()` | `src/main.cpp` (конец файла): `initialize(); run(); shutdown();` |
| Цикл | `Application::run()` — `src/core/application.cpp:1306`, `while` на `:1372` |
| События | `SDL_PollEvent` `:1447-1803` |
| Геймпад, `Input::update` | `:1813-1828` |
| Логика | `Application::update()` `:3874` (здесь же `ImGui::NewFrame`, `src/ui/ui_manager.cpp:367`) |
| Рендер | `Application::render()` `:4017`: `beginFrame` `:4064` → `renderWorld` `:4068` (только `IN_GAME`) → FrameXML → `uiManager->render` `:4876` → `finishImGuiFrame` `:4896` → `endFrame` `:4901` |
| Ограничитель кадров | `pacer.waitForCap` `:1402`, `sleep_until` `:1880-1887` (16,667 мс с vsync, 4,167 мс без) |
| Acquire / submit / present | `VkContext::beginFrame` `src/rendering/vk_context.cpp:2828` (`vkAcquireNextImageKHR` `:2869`), `VkContext::endFrame` `:2933` (`vkQueueSubmit` `:2992`, `vkQueuePresentKHR` `:3051`) |
| Кадров в полёте | `MAX_FRAMES_IN_FLIGHT = 2` (`include/rendering/vk_context.hpp:19`), ожидание слота `:2838` (timeline-семафор или fence, таймаут 5 с) |

- Отдельного потока рендера нет, всё на главном. Воркеры: запись secondary command buffers
  (`renderer.cpp:2963-3105`), анимация костей, видимость M2, стриминг террейна, сеть.
- Логин, выбор realm'а и персонажа идут в этом же цикле (мир не рисуется, UI в overlay-проходе).
- **Экран загрузки — отдельный вложенный цикл.** `WorldLoader` блокирует главный поток, сам качает
  `SDL_PollEvent` (`src/core/world_loader.cpp:616, 730, 1070`) и зовёт `LoadingScreen::render()`
  (`src/rendering/loading_screen.cpp:338`), который напрямую вызывает `vkCtx->beginFrame/endFrame` (`:442, :471`),
  минуя `Renderer`. Кадры идут нерегулярно, между шагами загрузки.

**Для VR**
- Общая точка обоих путей — `VkContext::beginFrame/endFrame`. `xrWaitFrame` + `xrBeginFrame` — в начало
  `beginFrame`; `xrEndFrame` — в `endFrame` после `vkQueueSubmit`. Так экран загрузки попадает в шлем без отдельного
  кода (решение ArxFatalis п. 12: Wait/Begin сразу после End предыдущего кадра).
- `xrPollEvent` — рядом с `SDL_PollEvent` (`application.cpp:1447`) и в трёх циклах `world_loader.cpp`.
- `xrLocateViews` — до `updatePerFrameUBO()` (`renderer.cpp:1319`) и до compute-cull (`:1331, :1340`).
- В VR отключить `sleep_until` и `FramePacer` (темп задаёт `xrWaitFrame`), зеркало в окне — без vsync.
- `Renderer::beginFrame` делает ранний `return` при свёрнутом окне (`:1273`): `xrEndFrame` обязан состояться
  и в этом случае.
- Готовый образец «вторая цель в том же кадре»: `VkContext::ExtraPresent` (`vk_context.hpp:101`), `AuxSwapchain`
  (`src/rendering/aux_swapchain.cpp`), хук `Renderer::setAfterInterfaceRecorder` (`include/rendering/renderer.hpp:96`,
  вызов `renderer.cpp:1486`).

## 2. VkContext

`src/rendering/vk_context.cpp`, `include/rendering/vk_context.hpp`; владелец — `core::Window` (`src/core/window.cpp:220`).
`initialize` `:82`: `createInstance` `:381` → `createSurface` → `selectPhysicalDevice` `:509` → `createLogicalDevice` `:677`
→ `createAllocator` `:1062` (VMA) → `createSwapchain` `:1232` → пулы, синхронизация → `createImGuiResources`.

- **vk-bootstrap** (`extern/vk-bootstrap`): `InstanceBuilder` `:389`, `PhysicalDeviceSelector` `:517`, `DeviceBuilder` `:758`.
- **Версия:** `require_api_version(1,3,0)` (`:400`). Устройство только с Vulkan 1.2 отвергается при старте.
- **Instance extensions:** только список `SDL_Vulkan_GetInstanceExtensions` (`:386`, цикл `:403-405`) + debug utils.
- **Device extensions:** все опциональные, `enable_extension_if_present` (`:688-750`): host image copy, device fault,
  ray query и др. Должны быть добавлены до `:758` (комментарий `:678`).
- **Features:** базовые `:552-580`; 1.1/1.2 `:766-814`; `dynamicRendering` `:840`, `synchronization2` `:864`.
  `multiview` не включается.
- **Выбор GPU:** `prefer_gpu_device_type(discrete)` + `selector.select()` (`:522`) — первое подходящее. Выбора по имени,
  индексу или переменной окружения нет. На ML350 две RTX 3090.
- **Swapchain:** `B8G8R8A8_UNORM`, usage `COLOR_ATTACHMENT | TRANSFER_SRC`, FIFO при vsync, иначе IMMEDIATE.
  Пересоздание по `swapchainDirty` (`recreateSwapchain` `:2607`, внутри `vkDeviceWaitIdle`).
- **MSAA:** resolve цвета внутри прохода сцены прямо в изображение swapchain (`:1853`), depth resolve `:1905`;
  смена — `Renderer::applyMsaaChange` (`renderer.cpp:1090`), пересоздаёт все пайплайны.

Render pass'ы (классические `VkRenderPass`; dynamic rendering — только тени):

| Проход | Владелец | Где |
|---|---|---|
| `imguiRenderPass` — **главный проход сцены** (цвет + D32, при MSAA resolve; `finalLayout = PRESENT_SRC`), `swapchainFramebuffers` | VkContext | `vk_context.cpp:1938-2062` |
| `overlayRenderPass` (load) / `overlayClearRenderPass` (clear), 1x, только цвет | VkContext | `:2110-2190` |
| `sceneContinueRenderPass` (вода после копии сцены; нет при MSAA) | VkContext | `:2192` |
| `shadowRenderPass`, `shadowFramebuffer[2]` | Renderer | `renderer.cpp:216` |
| `reflectionRenderPass` (фиксированный размер) | WaterRenderer | `water_renderer.cpp:1722` |
| `water1xRenderPass` + FB на каждое изображение swapchain | WaterRenderer | `water_renderer.cpp:2120` |
| Цели FSR1/FSR2/FXAA | PostProcessPipeline | `post_process_pipeline.cpp:637, 929, 1711` |
| `VkRenderTarget` (миникарта, карта мира, превью персонажа) | свои классы | `vk_render_target.cpp:157` |

`getImGuiRenderPass()` используется в 37 местах в 22 файлах: все пайплайны сцены собраны против него.

**Для VR**
- Instance extensions от OpenXR — в цикл `:403`; device extensions — до `:758`; `multiview` — в блок `:766-814`.
- GPU: вместо `selector.select()` — `select_devices()` и фильтр по `VkPhysicalDevice` от
  `xrGetVulkanGraphicsDevice2KHR`.
- **`XR_KHR_vulkan_enable2` не ложится на vk-bootstrap напрямую:** `vkCreateInstance`/`vkCreateDevice` вызываются
  внутри `builder.build()` (`:437`) и `deviceBuilder.build()` (`:953`), хука нет. Варианты:
  1. `XR_KHR_vulkan_enable` (v1): рантайм отдаёт списки имён расширений, код остаётся как есть;
  2. enable2: собрать `Vk*CreateInfo` вручную, вызвать `xrCreateVulkanInstanceKHR/DeviceKHR` и заполнить
     `vkb::Instance` / `vkb::Device` (структуры публичные).
  Что из этого поддерживает VDXR, покажет `hello_xr` (шаг 7 этапа 0).
- Режима без окна нет: SDL-swapchain обязателен (`beginFrame` без него возвращает null). Окно остаётся зеркалом.
- Формат целей сцены привязан к `swapchainFormat` (BGRA8 UNORM), а `finalLayout = PRESENT_SRC` зашит в проход сцены
  и в переходы пост-обработки (`post_process_pipeline.cpp:217, 314, 356`). Для XrSwapchain (обычно `*_SRGB`) нужен
  свой набор проходов или рендер в свою промежуточную цель с копией в XrSwapchain.

## 3. Камера

`rendering::Camera` — `include/rendering/camera.hpp:15`, `src/rendering/camera.cpp`.
- Хранит `position`, `yaw`, `pitch` (градусы), `fov`, `nearPlane = 0.5`, `farPlane = 30000`. Roll невозможен.
- View: `glm::lookAt(pos, pos + front, (0,0,1))` (`camera.cpp:13`). Projection: `glm::perspective` + переворот Y
  (`:19`), глубина 0..1, не reversed-Z. **Сеттера произвольной матрицы и асимметричного FOV нет.**
- Jitter: `Camera::setJitter/clearJitter` (`:58`), ставится в `PostProcessPipeline::applyJitter`
  (`post_process_pipeline.cpp:98`, вызов `renderer.cpp:1313`); выходит сразу при выключенном FSR2.
- Луч из экрана: `screenToWorldRay` (`camera.cpp:76`), структура `Ray{origin, direction}` (`camera.hpp:10`).

`rendering::CameraController` — `include/rendering/camera_controller.hpp:18`, `src/rendering/camera_controller.cpp`
(3800 строк): `update()` `:2715`, `updateOrbitCamera()` `:1986`, итоговый `camera->setPosition` `:2435`.
- Отдельного режима первого лица нет: это зум до минимума, `isFirstPersonView()` (`hpp:113`). Модель игрока при этом
  скрывается: `setInstanceVisible(playerInstanceId, !isFirstPersonView())` (`cpp:2495`).
- Коллизия камеры: `wmoRenderer->raycastBoundingBoxes` (`cpp:2130`), `raymarchTerrainCameraLimit` (`cpp:205`).
- Двигают вид помимо игрока: shake и «пьяное» покачивание (`cpp:3296`), intro / idle-orbit (`cpp:2822`),
  сглаживание `smoothedCamPos`.

Кто читает камеру: per-frame UBO (`renderer.cpp:559`), frustum culling во всех рендерерах (см. раздел 4),
стриминг террейна (`renderer.cpp:2105`), аудио-listener (`:1965`), проекция в экран для табличек, пузырей и боевого
текста (раздел 5), picking, небо, подводный туман (`:577`). Тени центрируются на персонаже, не на камере.

**Для VR**
- Добавить в `Camera` режим подмены: `setViewProjectionOverride(view, proj)`; `getPosition/getForward/getRight/getUp`
  в этом режиме выводить из `inverse(view)` — ими пользуются звук, вода, частицы, picking.
- Projection глаза — из тангенсов `XrFovf`, с тем же переворотом Y и глубиной 0..1. Near уменьшить (0,5 ярда много).
- View глаза = `inverse(T(глаза персонажа) · Rz(yaw тела) · M(XR→рендер) · поза глаза)`.
- В VR выключить: jitter/FSR2, shake, sway, intro/idle-orbit, сглаживание, отъезд камеры от коллизии.
- `CameraController::yaw` одновременно задаёт направление движения и facing: в VR вести его как «yaw тела»
  (решение ArxFatalis п. 11), голова добавляется сверху и в пакеты не идёт.
- Плавание и полёт берут pitch из `camera->getForward()` (`application.cpp:2594-2612`): в VR не отдавать туда pitch
  головы без явного решения.

## 4. Рендер кадра

`Renderer` — `src/rendering/renderer.cpp`: `beginFrame` `:1192`, `renderWorld` `:2857`, `endFrame` `:1410`,
`buildFrameGraph` `:4769`.

Порядок:
1. `beginFrame`: ресурсы пост-обработки, пересоздание swapchain, `vkCtx->beginFrame`, `applyJitter` `:1313`,
   `computeLightSpaceMatrix` `:1316`, `updatePerFrameUBO` `:1319`, compute-cull M2 `:1331` и травы `:1340`.
2. Pre-проходы (`:1347`): композит миникарты, карты мира, превью; тени (`renderShadowPass` `:4394`);
   volumetric fog (`:4667`, compute); отражение воды (`renderReflectionPass` `:4310`).
3. Открытие прохода сцены (`:1354-1394`): цель — `postProcessPipeline_->getSceneFramebuffer()` при FSR1/FSR2/FXAA,
   иначе **прямо framebuffer swapchain**. Режим secondary command buffers.
4. `renderWorld` (`:3186`): террейн + трава → небо → WMO → круг выделения → персонажи → M2 (дым, частицы, ленты) →
   вода, погода, молнии, эффекты плавания, пыль, следы, маркеры квестов, подводный оверлей.
5. Без MSAA (`:3325-3393`): проход закрывается → RT lighting → `captureSceneHistory` (копия цвета и глубины для
   рефракции) → проход продолжения: вода, брызги, **миникарта**.
6. `endFrame`: `executePostProcessing` (`post_process_pipeline.cpp:177`: FSR2 / FXAA / FSR1 → swapchain) →
   sun shafts → overlay-проход: композит sun shafts + ImGui (`:1477`) → `afterInterface_` (`:1486`) → `vkCtx->endFrame`.

| Проход | Зависит от размера экрана | Данные прошлого кадра |
|---|---|---|
| Тени (одна орто-карта 512–4096, каскадов нет) | нет | нет |
| Volumetric fog (froxel по фрустуму камеры) | привязан к фрустуму | да, `prevViewProj_` |
| Отражение воды (фиксированная текстура, одна камера) | нет | нет |
| Сцена | да | M2-cull хранит `prevVP_` |
| Вода, рефракция (экранные UV) | да | копия того же кадра |
| RT lighting | да | да, результат читается на следующем кадре |
| FSR2 / FSR3 framegen | да | да (история, motion vectors, `prevJitter`) |
| FSR1, FXAA, sun shafts | да | нет (sun shafts — не проверено) |

- HDR-цели и отдельного тонмаппинга нет; TAA как отдельного прохода нет — только FSR2.
- **Per-frame UBO:** один `GPUPerFrameData` на слот кадра (`include/rendering/vk_frame_data.hpp:14`), set 0 binding 0,
  одна пара `view`/`projection`, пишется раз за кадр. UBO читается в момент исполнения draw, а не записи
  (предупреждение `vk_frame_data.hpp:75`): перезаписать один буфер между глазами нельзя.
- Прецедент «та же сцена другим view через другой set 0» — `reflPerFrameUBO` для отражения (`renderer.cpp:430`).
- **Culling на CPU по `camera.getViewProjectionMatrix()`** в каждом рендерере: `terrain_renderer.cpp:937`,
  `wmo_renderer.cpp:1641` и порталы `:2308`, `m2_renderer_render.cpp:357/859/1150` + compute `m2_cull.comp`,
  `character_renderer.cpp:2587`, `water_renderer.cpp:1124`, `grass_renderer.cpp:492` + `grass_cull.comp`,
  следы, маркеры квестов. HiZ есть в коде, но не подключён.
- Шейдеры (`assets/shaders`, 90 `.glsl`): 23 вершинных читают блок `PerFrame`, 20 из них пишут
  `gl_Position = projection * view * …`; около 10 фрагментных читают `view`/`viewPos`. Экранные координаты
  используют `m2.frag`, `water.frag`, `water.vert`. Сборка — `glslc` (`CMakeLists.txt:464-560`), `.spv` в git нет
  (кроме двух footprint).

**Для VR** — три пути, от дешёвого к правильному:
1. **Моно-копия в шлем** (проверка цикла кадра): блит готового кадра в XrSwapchain через хук `afterInterface_`.
2. **Два прохода на глаз:** второй набор UBO + set 0 по образцу `reflPerFrame*`; повторить блок `:1354-1394`,
   `renderWorld` и проход воды с другим framebuffer. Secondary-буферы наследуют конкретный framebuffer
   (`beginSecondary`, `:4274`) — на второй глаз нужен второй комплект.
3. **Multiview:** `view`/`projection` в UBO → массивы `[2]`, `gl_ViewIndex` в ~23 вершинных и ~10 фрагментных
   шейдерах, `viewMask` во всех проходах сцены, двухслойные цели. Объём правок в чужих файлах большой — это
   главный риск конфликтов с upstream.
- Culling в любом варианте — по объединённому фрустуму двух глаз (все места из списка выше).
- Pre-проходы (тени, туман, отражение, миникарта) — один раз на кадр.
- В VR-профиле выключить всё темпоральное и экранное: FSR2/FSR3, RT lighting, volumetric fog, sun shafts,
  отражение воды и рефракцию проверить отдельно (одна камера, экранные UV).
- MSAA делает resolve прямо в swapchain и меняет путь воды (`water1xRenderPass`): для VR нужен свой resolve в цель глаза.

## 5. 2D: ImGui и FrameXML

- ImGui: `UIManager::initialize` (`src/ui/ui_manager.cpp:102-145`), бэкенды SDL3 + Vulkan, пайплайн собран против
  `vkCtx->getOverlayRenderPass()`; `update` → `NewFrame` (`:363`), `render` (`:372`), `finishImGuiFrame` (`:461`).
  Единственный вызов отрисовки — `ImGui_ImplVulkan_RenderDrawData` в `Renderer::endFrame` (`renderer.cpp:1477`),
  overlay-проход **прямо на изображении swapchain**. Промежуточной цели для UI нет.
- **FrameXML/Lua своего GPU-рендерера не имеет:** `WidgetRenderer::draw` (`src/ui/widget_renderer.cpp:2024`) пишет в
  `ImGui::GetBackgroundDrawList()` (`:2250`), предмет на курсоре — в foreground (`:2131`). Весь 2D — один `ImDrawData`.
  Раскладка — `src/ui/widget_tree.cpp`, Lua — `src/addons/lua_engine.cpp`; описание — `docs/widget-system.md`.
- Экраны: логин, realm, персонажи — `PaperUI` (`include/ui/paper_ui.hpp`) в ImDrawList; загрузка —
  `LoadingScreen` (свой кадр, раздел 1); игровой HUD (панели, чат, фреймы, сумки, тултипы) — FrameXML;
  поверх — окна ImGui из `GameScreen::render` (настройки, диалоги).
- **Проекция мира в пиксели экрана** (в VR на плоской панели не совпадёт с объектами):
  таблички — `GameScreen::renderNameplates` (`src/ui/game_screen_hud.cpp:895`, клики `:1566`);
  боевой текст — `src/ui/combat_ui.cpp:196`; пузыри чата — `src/ui/chat/chat_bubble_manager.cpp:50`;
  рейд-метки — `game_screen_hud.cpp:1619`.
- **Миникарта рисуется не в UI, а отдельным Vulkan-проходом внутри сцены:** `Renderer::renderMinimapOverlay`
  (`renderer.cpp:3575`) → `Minimap::render` (`minimap.cpp:503`); FrameXML только передаёт прямоугольник
  (`setScreenRect`, `application.cpp:4160`). Метки на ней — ImGui.
- Карта мира — offscreen-композит + окно ImGui; есть ещё отдельное ОС-окно `ui::MapWindow` (в VR не нужно).
- Портреты и превью персонажа — свои offscreen-цели, буфер сцены не читают. Blur за окнами не найден.
- **Размер UI:** `io.DisplaySize` каждый кадр ставит бэкенд из размера окна; FrameXML —
  `uiScale_ = pixelH / 768 * userScale_` (`widget_tree.cpp:738`). Независимо от окна размер сейчас не задать:
  `window->getWidth()/getHeight()` читаются напрямую в `combat_ui.cpp:81,196,658`, `chat_bubble_manager.cpp:50`,
  `game_screen_hud.cpp:790,908`, `game_screen.cpp:1831-2107`, `toast_manager.cpp:187-403`, `renderer.cpp:3596`.
- Готовый помощник для offscreen-цели: `VkRenderTarget` (`include/rendering/vk_render_target.hpp:18`).

**Для VR** (решение ArxFatalis п. 10: весь 2D в панель фиксированного размера, quad-слой, premultiplied alpha)
1. UI-цель RGBA8 фиксированного размера + проход с очисткой в (0,0,0,0).
2. `ui_manager.cpp:140` — отдать ImGui этот проход (пайплайн привязан к совместимости прохода и формату).
3. `renderer.cpp:1453-1482` — в VR открывать UI-цель вместо overlay-framebuffer'а swapchain.
4. Подменить `io.DisplaySize` после `ImGui_ImplSDL3_NewFrame` (`ui_manager.cpp:368`, `loading_screen.cpp:351`) и
   свести все прямые `window->getWidth/Height` из списка выше к одному источнику «размер UI».
5. Альфа: блендинг ImGui по альфе `ONE / ONE_MINUS_SRC_ALPHA` поверх прозрачной очистки должен давать
   premultiplied-результат — вывод из коэффициентов, на практике не проверено.
6. Миникарту перенести из прохода сцены в UI-цель.
7. Таблички, боевой текст, пузыри — на этапе 1 проецировать «камерой головы» в панель или выключить; в 3D — этап 3.
8. Пересоздание swapchain пересоздаёт overlay-проход (`renderer.cpp:1172`): UI-цель от этого отвязать.

## 6. Ввод

- Насос: `Application::run` (`application.cpp:1447`), порядок: `mapWindow_->handleEvent` `:1470` → геймпад `:1475` →
  `uiManager->processEvent` `:1512` (ImGui) → камера, если `!WantCaptureMouse` `:1516` → текст `:1583` → клавиши `:1600`.
- `core::Input` — `include/core/input.hpp`, `src/core/input.cpp`; `update()` — `application.cpp:1828`. Позицию мыши
  каждый кадр берёт из `SDL_GetMouseState` (`input.cpp:28`), клавиши — из `SDL_GetKeyboardState` (`:19`).
- Движение — опрос `Input::isKeyPressed` по жёстким скан-кодам W/A/S/D/Q/E/Space (`camera_controller.cpp:2807`).
- Биндинги: EditBox с фокусом получает `dispatchKey` (`:1648`); иначе `dispatchFrameKey` → `dispatchBindingKey`
  (`lua_engine.cpp:9865`). Клиентские действия — `KeybindingManager::isActionPressed` (`keybinding_manager.cpp:70`).
- Мышь для FrameXML не событийная: раз в кадр `engine->dispatchMouse(io.MousePos, …)` (`application.cpp:4827`).
- «UI съел событие»: окна ImGui — `io.WantCaptureMouse`; FrameXML — `ui::frameXmlOwnsMouse()`
  (`framexml_takeover.cpp:177`); клавиатура — `WantCaptureKeyboard || ui::interfaceTakingTypedInput()` (`:1759`).
- ЛКМ/ПКМ над миром включает относительный режим мыши (`camera_controller.cpp:3396`) и поворот камеры.
- Курсор системный SDL (управляет бэкенд ImGui); особые курсоры — BLP в foreground draw list у `io.MousePos`.
- Текст: `SDL_EVENT_TEXT_INPUT` → `engine->dispatchText(utf8)` (`application.cpp:1583`); на логине — очередь символов
  ImGui (`paper_ui.cpp:865`). Готовой экранной клавиатуры нет.

**Готовый образец — геймпад:** `core::Gamepad` (`src/core/gamepad.cpp`) + `ui::GamepadControls`
(`src/ui/gamepad_controls.cpp`): стик → `Input::setVirtualKey(W/S/Q/E)` (`:98`), обзор → `applyLookDelta` (`:123`),
клики → `Input::setVirtualMouseButton` + `io.AddMouseButtonEvent` (`:153`), клавиши → `io.AddKeyEvent` (`:368`),
кнопки → биндинги через `keyRouter_` (`:267`). Вызывается в `application.cpp:1813-1825`.

**Для VR** — слой `xr::VrControls` по образцу `GamepadControls`, вызываемый там же:
- клавиши, стики, кнопки мыши — готовые `setVirtualKey` / `setVirtualMouseButton` / `io.Add*Event`
  (оба канала обязательны: мир опрашивает `Input`, панели — ImGui);
- **позиция луча — слабое место:** абстракции позиции мыши нет, геймпад двигает настоящий курсор через
  `SDL_WarpMouseInWindow`. Для VR нужно добавить `Input::setVirtualMousePosition`, подавать
  `io.AddMousePosEvent` в координатах UI-цели после `ImGui_ImplSDL3_NewFrame` и не пропускать события мыши ОС
  в `processEvent` (бэкенд иначе перезапишет позицию, `imgui_impl_sdl3.cpp:666`);
- модификаторы Shift/Ctrl/Alt Lua читает из `SDL_GetModState` (`lua_action_api.cpp:1312`) — виртуальные клавиши их
  не меняют;
- текст с экранной клавиатуры: `engine->dispatchText` / `dispatchKey`, на логине `io.AddInputCharactersUTF8`.

## 7. Персонаж и движение

**Ввод, физика и коллизии живут в `CameraController`, а не в `src/game`.**
- Клавиши `cpp:2807-2820`; автобег `:2868`; без ПКМ A/D — поворот, с ПКМ — стрейф (`:2936`); поворот клавишами
  `yaw ± turnSpeed·dt`, `facingYaw = yaw` (`:2952`); вектор движения `:3123`; скорость `:2981` (run / walk / back).
- Пакеты: определение фронтов в `cpp:3170-3272` → `movementCallback(opcode)` → `GameHandler::sendMovement` →
  `MovementHandler::sendMovement` (`src/game/movement_handler.cpp:486`, флаги `:559-680`).
- Позиция и ориентация → сервер каждый кадр (`application.cpp:3146`); `MSG_MOVE_SET_FACING` при изменении > 3°, не чаще
  10 Гц (`:3163`); `MSG_MOVE_HEARTBEAT` каждые 0,5 с (`src/game/game_handler.cpp:792`).
- Yaw модели — из `getFacingYaw()` (`renderer.cpp:2046`); внешний поворот: `CameraController::setFacingYaw`
  (`hpp:263`), `GameHandler::faceCanonicalYaw`.
- Плавание `cpp:345`; полёт `flightPitchDeg_` (`cpp:3055`), Space вверх / X вниз; маунты `setMounted`; такси —
  `externalFollow_` (`cpp:2724`); транспорт — `application.cpp:2639-3030`, `include/game/transport_manager.hpp`.
- Коллизии: стены `sweepAgainstWalls` (`cpp:284`), пол `sampleFloorUnderFeet` (`cpp:892`),
  `groundFollowedCharacter` (`cpp:1368`); шаг вверх 0,60, уклон 50° (`include/rendering/movement_limits.hpp:11`);
  гравитация −19,29, прыжок 7,96.
- **Аналоговой скорости нет:** стик квантуется в виртуальные клавиши, скорость дискретна.

**Для VR**
- Минимальный путь, пакеты идентичны клавиатуре: повторить `GamepadControls` — `setVirtualKey(W/S/Q/E)`,
  `cc->setSteering(true)`, snap-поворот через `cc->applyLookDelta(dx, 0)`, прыжок — виртуальный Space.
- Чище: добавить в `CameraController` `setMoveIntent(axes, jump, walk)` и `addBodyYaw(deg)`, подставляемые вместо
  клавиш (`cpp:2925-2944`); фронты и пакеты остаются прежними.
- VR-ввод обязан проходить те же гейты: `uiWantsKeyboard`, `sitting`, `movementSuppressTimer_`, `introActive`.
- Шаг в комнате персонажа не двигает (в ArxFatalis — п. 36, этап 2+).

## 8. Выбор цели

- **Функция с произвольным лучом уже есть:** `ui::pickScene(GameHandler&, const Ray&, const ScenePickParams&, onHit)` —
  `include/ui/scene_pick.hpp:136`, `src/ui/scene_pick.cpp:37`. Перебирает сущности (UNIT / PLAYER / GAMEOBJECT),
  тест луч–сфера; `ScenePick::resolve()` (`hpp:116`) — приоритет враждебных и живых.
  **Геометрия мира не проверяется: цель выбирается сквозь стены.**
- Клики — `src/ui/game_screen.cpp`: hover `:1840-1861`, ЛКМ `:1865-2010` (`setTarget`, `interactWithGameObject`),
  ПКМ `:2016-2220` (`lootTarget`, `interactWithNpc`, атака).
- Пакеты: `CombatHandler::setTarget` (`src/game/combat_handler.cpp:1332`, CMSG_SET_SELECTION),
  `interactWithGameObject` / `interactWithNpc` / `lootTarget` / `startAutoAttack`
  (`src/game/game_handler_callbacks.cpp:2888, 2798, 2684, 2101`).
- **Наземных AoE с указанием точки в клиенте нет:** `CastSpellPacket` строится только с целью-GUID
  (`include/game/world_packets.hpp:2189`), ретикла и raycast по земле нет.
- Лучи по геометрии: `WMORenderer::raycastBoundingBoxes` (`wmo_renderer.hpp:415`), `M2Renderer::raycastBoundingBoxes`
  (`m2_renderer.hpp:627`); по террейну — только `getHeightAt(x, y)`.

**Для VR**
- Луч контроллера в координатах рендера — прямо в `ui::pickScene`, затем те же `setTarget` / `interact*`.
- Логику ЛКМ/ПКМ из `game_screen.cpp:1865-2220` (перемешана с проверками мыши и ImGui) выделить в
  `selectAlongRay(ray)` / `interactAlongRay(ray)`, чтобы мышь и контроллер вызывали одно и то же.
- Окклюзию добавить маршем по `raycastBoundingBoxes` и `getHeightAt`.
- AoE по земле (этап 2) писать с нуля: билдер CMSG_CAST_SPELL с DEST_LOCATION, луч → террейн/WMO, ретикл.

## 9. Настройки, пути и параметры запуска

- **CLI-аргументов у клиента нет:** `int main([[maybe_unused]] int argc, …)` (`src/main.cpp:132`), других обращений к
  `argv` нет. Всё управление — переменными окружения. Автологина, выбора экспансии и выбора GPU через окружение нет.
- **Данные:** `WOW_DATA_PATH` (`main.cpp:124`) → иначе `%LOCALAPPDATA%\Wowee\Data`, если там есть извлечение
  (`include/core/data_paths.hpp:38, 89`) → иначе `./Data` от **рабочего каталога** (`application.cpp:368`).
  `WOW_DATA_PATH` указывает на корень, внутри которого `expansions\wotlk\manifest.json` (`application.cpp:440`).
  JSON-профили (`expansion.json`, `opcodes.json`, `update_fields.json`, `dbc_layouts.json`) при старте копируются из
  `./Data/expansions/<id>/` рабочего каталога в `<WOW_DATA_PATH>/expansions/<id>/` — `syncClientTables`
  (`application.cpp:376`, `data_paths.hpp:136`).
- **На Windows `main()` не делает chdir в каталог exe:** `./Data`, `assets/`, `addons/`, `logs/` ищутся от каталога
  запуска. Запускать с рабочим каталогом, где есть `assets\` и `Data\expansions\` (корень репозитория).
- **Конфиг:** `getConfigRoot()` (`src/core/config_paths.cpp:83`): `WOWEE_CONFIG_ROOT` → `<exe>\config` при наличии
  `portable.txt` → `%APPDATA%\wowee`. Там `settings.cfg`, `login.cfg`, `cvars.cfg`, `bindings.cfg`, `addons.cfg`,
  `characters/`, `lua_errors.txt`. Мимо него, жёстко в `%APPDATA%\wowee`: `last_world.cfg`, `pipeline_cache.bin`,
  `guild_names.tsv`, `warden_cache\`. Скриншоты — `%USERPROFILE%\.wowee\screenshots`.
- **Пароль:** в `login.cfg` хранится `password_hash` = hex `SHA1(UPPER(user):UPPER(pass))` (`auth_screen.cpp:334`),
  открытым текстом в файле. Для SRP6 это эквивалент пароля: `userdata\` не публиковать.
- **Лог:** `logs/wowee.log` от рабочего каталога, перезаписывается при запуске; `WOWEE_LOG_FILE`, `WOWEE_LOG_LEVEL`
  (`debug|info|warn|error|fatal`, по умолчанию warn), сброс на диск раз в 250 мс, ошибки сразу (`src/core/logger.cpp:129`).
- **Полезные переменные:** `WOWEE_REALM_HOST_OVERRIDE` (`src/core/ui_screen_callback_handler.cpp:61`),
  `WOWEE_VULKAN_VALIDATION=1`, `WOWEE_VULKAN_GPU_VALIDATION=1` (`vk_context.cpp:411`), `WOWEE_SKIP_{WMO,CHARS,M2,
  TERRAIN,SKY,SHADOWS}` (`renderer.cpp:2865`), `WOWEE_FRAME_PROFILE`, `WOWEE_TIME_OF_DAY=HH:MM` (держать час суток для сравнения снимков), `WOWEE_NO_BAKED_SHADOW=1` (без запечённых теней земли), `WOWEE_GROUND_CAST_AT_FEET=1` (наземные заклинания — под ноги, без прицеливания), `WOWEE_VR_EYE_REPLAY=0` (каждый глаз записывается сам), `WOWEE_SPELL_MISSILE_DIAG=<visual>[:<speed>]`, `WOWEE_M2_PARTICLE_DISTANCE=<yd>`, `WOWEE_SCREENSHOT=<png>` (снимок и выход),
  `WOWEE_RECORD=<mp4>` (`application.cpp:4910`), `WOWEE_VK_NO_DYNAMIC_RENDERING`.
- **Локаль:** в AUTH уходит `profile->locale` (`auth_screen.cpp:1216`), по умолчанию `enUS`; задаётся полем `locale` в
  `expansion.json` (`src/game/expansion_profile.cpp:239`). Строки DBC клиент читает из колонки enUS
  (`game_handler.cpp:2791, 2975, 3040`), смещения по локали нет — с DBC ruRU названия могут оказаться пустыми
  (проверяется запуском). Шрифты `*_CYR.TTF` не ищутся (`src/ui/ui_manager.cpp:199`).

**Для VR**
- Флаги `--vr`, `--vr-debug`, `--vr-dump-frames N` из плана потребуют либо разбора `argv` в `main.cpp` (сейчас его нет),
  либо переменных `WOWEE_VR=1`, `WOWEE_VR_DEBUG=1`, `WOWEE_VR_DUMP_FRAMES=N` в стиле клиента. Второе — меньше правок
  в чужом коде и работает на Android (активити выставляет окружение). Решить в начале этапа 1.
- Настройки: схема `include/ui/settings_schema.hpp:41` + таблица `kSchema[]` (`src/ui/settings_schema.cpp:27`);
  рецепт из семи шагов — `settings_schema.hpp:156`; вкладка — `ImGui::BeginTabItem` в `renderSettingsWindow`
  (`src/ui/settings_panel.cpp:515`) + `drawSchemaCategory("VR", save)` (`:707`). Соответствие сторожат тесты
  `settings_apply_on_load` и `settings_schema_consistency`.
- CMake: опции — блок `CMakeLists.txt:27-48`; исходники — явный список `WOWEE_SOURCES` с `:607` (glob нет), цель на
  `:1259`. Образец необязательной зависимости — Unicorn (`:442`, `:1440`) и Tracy (`:1263`, `target_sources`).
  Для VR: `option(WOWEE_VR … OFF)` и после создания цели `if(WOWEE_VR)` с `target_sources(wowee PRIVATE src/xr/…)`,
  `find_package(OpenXR)`, `target_compile_definitions(wowee PRIVATE WOWEE_VR)`.

## 10. Android (заметка для этапа 4)

- Gradle (`android/app/build.gradle`) → `externalNativeBuild.cmake` на тот же корневой `CMakeLists.txt`, цель `wowee`
  как `SHARED` (`CMakeLists.txt:1256`), ABI `arm64-v8a`, `minSdk 33`, NDK 28.2. Все `WOWEE_SOURCES` общие.
- SDL3 и glm — через FetchContent (`:276`), FFmpeg отключён (`:411`), OpenSSL — `tools/build-android-deps.sh`.
- Жизненный цикл: `com.wowee.client.WoweeActivity extends SDLActivity` (не NativeActivity). Активити выставляет
  `WOWEE_RESOURCE_ROOT`, `WOW_DATA_PATH`, `WOWEE_CONFIG_ROOT` (`WoweeActivity.java:79`).
- `#ifdef __ANDROID__`: `main.cpp:18-168`, `application.cpp:1476, 3643, 4018, 4056` (тач, пропуск кадра без
  поверхности), `window.cpp:69-227`, `renderer.cpp:746` (тени 1024), `asset_manager.cpp:122`, `ui_manager.cpp:52`.
- План upstream — `docs/plan-android.md`. Для OpenXR на Quest понадобится своя активити с intent-категорией VR и
  `xrInitializeLoaderKHR` — это и есть слой `XrPlatform`.

## 11. Кадр в городе по RenderDoc

Штормград, у ворот Торгового квартала, кадр 803, стоковые данные, сцена 1920×1020, MSAA выключен, настройки по
умолчанию (тени 2048, отражения воды, RT lighting и volumetric fog включены). Снимок —
`D:\WoWVR\rdoc\stormwind_frame803.rdc` (1,1 ГБ), сводка — `..\logs\rdoc\stormwind-summary.json`, получена
`..\tools\rdoc-summary.py`. Время — сумма длительностей событий при воспроизведении в RenderDoc на RTX 3090:
годится для сравнения проходов между собой, а не как точное время кадра.

| Проход | Цель | Draw calls | Индексов | GPU, мс |
|---|---|---|---|---|
| Композит миникарты | 768×768 RGBA8 | 9 | — | 0,01 |
| Портрет персонажа | 85×85 RGBA8 + D32, MSAA 4× | 10 | 4 тыс. | 0,02 |
| **Тени** (dynamic rendering) | 2048×2048 D32 | **10 603** | 3,7 млн | **9,8** |
| Compute между проходами (туман, culling) | — | 2 dispatch | — | 0,1 |
| **Отражение воды** | 512×512 BGRA8 + D32 | **10 721** | 7,0 млн | **9,7** |
| Сцена: террейн и трава | 1920×1020 BGRA8 + D32 | 2 992 | 2,3 млн | 3,0 |
| Сцена: небо | то же | 3 | — | ~0 |
| Сцена: WMO | то же | 7 726 | 4,7 млн | 7,4 |
| Сцена: персонажи | то же | 553 | 0,2 млн | 0,6 |
| Сцена: M2 (доодады, частицы) | то же | 1 996 | 0,8 млн | 1,9 |
| Продолжение сцены: вода, миникарта | 1920×1020 BGRA8 + D32 | 97 | 0,1 млн | 0,1 |
| Overlay: sun shafts + ImGui/FrameXML | 1920×1020 BGRA8 | 91 | — | 0,1 |
| **Итого** | 6 render pass'ов + compute | **34 801** | ~19 млн | **~33** |

Разбивка сцены по семи secondary-буферам сопоставлена с порядком из раздела 4 по числу вызовов, а не по именам
(меток в снимке нет): для точности на этапе 1 добавить debug-метки или проверить в интерфейсе RenderDoc.

**Выводы для VR**
- Проходы, не зависящие от разрешения, — **тени и отражение воды — дают ~19,5 из ~33 мс и 21 тыс. из 35 тыс. draw
  calls.** Отражение рисует сцену целиком в 512×512 ради воды, которой в кадре почти нет. В VR-профиле отражение
  выключить, тени уменьшить или упростить — это первое, что даёт бюджет.
- Сама сцена — ~13 мс при 2 Мп. Глаза Quest 3 — 2×2688×2880 = 15,5 Мп, бюджет 11,1 мс при 90 Гц. Пиксельная
  нагрузка вырастет до 8 раз, вершинная при двух проходах — вдвое. Без профиля качества в 90 Гц не уложиться;
  WMO (7,4 мс, 7,7 тыс. вызовов) — главный кандидат на оптимизацию.
- 13 тыс. draw calls сцены на глаз — нагрузка на CPU; при двух проходах на глаз удваивается (довод за multiview
  или за общий список видимости на оба глаза).
- Тени, туман, отражение и миникарта выполняются один раз на кадр и на число глаз не умножаются.
- UI — 91 вызов в один проход: перенос в отдельную цель дёшев.
