# 01. Архитектурные концепции

**Статус на 18.09.2026.** Исследование и проектные решения, не описание реализованного движка. Канонические решения — [ARCHITECTURE.md](../ARCHITECTURE.md), порядок до и после MVP — [PLAN.md](../../PLAN.md). Сведения о других движках сохраняются как исследовательский контекст; прежние рекомендации, помеченные **superseded**, не задают стек Faset.

Принято: C++ и EnTT runtime; собственный retained UI на C++ с декларативными layout/styles и тёмной темой по умолчанию, ImGui для отладки; SDL3, Vulkan 1.3, собственный renderer/RenderGraph, Slang; CMake + Ninja, Clang на Linux и clang-cl на Windows; Box2D/Box3D. Lua — обязательный следующий этап развития, необязательная зависимость конкретной игры. Первое MVP доводит маленькие 2D- и 3D-демо до самостоятельного экспорта на обе ОС; advanced graphics — после MVP.

## 1. Архитектура должна следовать пользовательским действиям

У движка есть два связанных, но не одинаковых продукта:

- **runtime** — запускает игру, симулирует мир и выводит кадр;
- **authoring environment** — помогает человеку (и при необходимости automation-инструментам) строить, исследовать и исправлять мир.

Авторские документы и работающая игра имеют разные владельцы состояния. Общая схема описывает сериализуемые данные, но редакторские транзакции не перехватывают каждое изменение компонента в игре.

```text
Editor UI / CLI / MCP / editor plugins
                 ↓
AuthoringService: JSON documents, schema, revisions, transactions, Undo
                 ↓ validated scene snapshot + cooked assets
Separate Player: statically linked C++ gameplay → EnTT runtime
                 ↓
Physics / render extraction / renderer / runtime diagnostics
```

MCP работает только на стороне редактора: authoring, импорт, сборка, Play/Stop и editor logs. Он не читает и не редактирует runtime world, отсутствует в Player и экспортированных играх. Игровой C++ использует runtime API и command buffer; редакторские расширения используют AuthoringService. Snapshot запуска не меняется вслед за последующими правками исходной сцены.

## 2. Рекомендуемые слои

Не делать одну гигантскую `Engine`-сборку. Полезная граница модулей:

```text
Engine.Editor                 # окна, selection, gizmos, asset browser, commands
Engine.Authoring              # scenes, prefabs, transactions, reflection, validation
Engine.Runtime                 # world, systems, events, scheduling, time, services
Engine.Spatial                 # transforms, bounds, hierarchy, queries, cameras
Engine.Physics                 # 2D/3D adapters, fixed simulation, contacts
Engine.Graphics                # render extraction, render graph interface, resources
Engine.Assets                  # IDs, importer, cache, dependency graph, hot reload
Engine.Input                   # devices -> actions -> gameplay/UI contexts
Engine.Scripting               # public scripting API, reload boundary, diagnostics
Engine.Platform                # window, filesystem, threads, clocks, processes
Engine.Diagnostics             # typed logs/traces; editor and runtime boundaries
Engine.Automation              # editor-only MCP; authoring/import/build/PlayStop/logs
Engine.App                      # composition root and game-specific code
```

Направление зависимостей — сверху вниз только через стабильные interfaces/data contracts. `Editor` может ссылаться на `Authoring`, но runtime не должен зависеть от ImGui или конкретного editor UI. `Graphics.Vulkan` — backend `Graphics`, а не центр, от которого зависит gameplay.

### Правило владения

Каждый объект имеет одного явного владельца:

- authoring document владеет сохраняемыми объектами; runtime `World` — своими EnTT entities и компонентами;
- `AssetRegistry` владеет жизненным циклом загруженных ресурсов;
- `TransformGraph` владеет локальными/мировыми transform-вычислениями;
- physics backend владеет native body и рассчитанным transform динамического тела; teleport и kinematic movement задаются явно;
- renderer владеет GPU-копиями, но не авторским asset;
- editor transaction владеет историей изменений.

Ссылки между подсистемами — стабильные IDs/handles, а не сохраняемые указатели на C++-объекты. Это облегчает hot reload, сохранение, remote tools и диагностику dangling references.

## 3. World model: composition, а не одна иерархия

### Entity + components

Авторский объект имеет постоянный `ObjectId`; EnTT entity — временный handle внутри конкретного runtime world. Components — данные и явно определённые lifecycle hooks. Systems читают и изменяют наборы компонентов. Это дает композицию: объект может иметь `Transform2D`, `Sprite`, `Collider2D`, `AudioSource`, `Script` или их 3D-аналоги без дерева наследования.

O3DE описывает тот же принцип как «has-a», а не «is-a», и разделяет runtime/editor/system components ([O3DE ECS overview](https://www.docs.o3de.org/docs/user-guide/programming/components/overview)). Это полезное различие для нового движка:

- `TransformComponent` runtime существует в игре;
- `TransformEditorComponent` добавляет gizmo и authoring metadata;
- `AssetSystem`/`UndoSystem` — system services, не компоненты сущности.

### Отдельный transform graph

Не смешивать entity hierarchy с универсальным механизмом поведения. Нужна отдельная иерархия parent/child только там, где она имеет смысл:

- локальные transform для руки, камеры, дочернего объекта;
- UI layout;
- skeleton/кости;
- 2D-вложенность и canvas layers.

Логика gameplay, ownership ресурсов, physics constraints и spatial partition — другие графы. Один parent не должен неявно означать все типы связи. Это предотвращает циклы и неожиданное каскадное изменение.

### Авторская сущность и runtime entity

Сохраненная сцена содержит authoring IDs и компоненты. При запуске может появиться runtime entity mapping:

```text
AuthoringId 42 -> RuntimeEntity 918
PrefabId     -> instance overrides
AssetId      -> loaded resource handle
```

Это позволяет Player менять runtime без порчи исходной сцены. Mapping полезен внутренней диагностике, но не создаёт runtime MCP API; обратное применение произвольного состояния игры в документы не входит в MVP.

## 4. Scenes, prefabs и resources

Godot делает сцену универсальной композиционной единицей: сценой может быть weapon, character, door или уровень, а вложенные сцены ведут себя как переиспользуемые композиции ([design philosophy](https://docs.godotengine.org/en/stable/getting_started/introduction/godot_design_philosophy.html), [key concepts](https://docs.godotengine.org/en/stable/getting_started/introduction/key_concepts_overview.html)). Unity решает близкую задачу через prefab: GameObject с компонентами можно сохранять, вкладывать и делать вариации ([Prefabs](https://docs.unity3d.com/6000.1/Documentation/Manual/Prefabs.html)).

Для нового движка полезно объединить сильные стороны:

- **Scene** — редактируемое дерево/граф entities, которое можно открыть и запустить отдельно;
- **Prefab** — сцена как reusable asset с instance overrides;
- **Resource** — именованный типизированный asset (mesh, texture, material, animation, sound, script, data asset);
- **Spawnable** — подготовленная runtime-последовательность создания prefab/scene с dependency manifest.

### Принятые правила экземпляров и overrides

1. Шаблон — отдельный scene resource с постоянными `ObjectId` и `ComponentId`. Размещение хранит `TemplateAssetId`, собственный `InstanceId` и override layer; имя — только подпись.
2. Идентичность унаследованного объекта определяется цепочкой `InstanceId` вложенных размещений и исходным `ObjectId` в документе шаблона. Это цепочка инстанцирования, не путь transform-родителей: допустимый reparent не меняет ID.
3. Patch адресуется через instance chain, `ObjectId`, `ComponentId`, `FieldId`; `TypeId` проверяет совместимость. В v1 массив изменяется целиком, без хрупких index/name-based patches.
4. Порядок разрешения: данные шаблона → overrides вложенного экземпляра в содержащем шаблоне → overrides внешнего экземпляра в сцене. Последний явный override побеждает, включая намеренное совпадение с текущим default; неизменённые поля не записываются.
5. Добавленный объект или компонент получает новый ID в документе-владельце override. Сохраняются provenance и ссылка на родителя. Дублирование экземпляра создаёт новый `InstanceId`, переназначает внутренние ссылки и сохраняет внешние.
6. Reparent в v1 разрешён внутри одного экземпляра; циклы и перенос через границу вложенного экземпляра отклоняются. Перемещение экземпляра целиком допустимо. Команда явно выбирает сохранение local или world transform.
7. Удаление унаследованного объекта записывает suppression объекта и его разрешённого поддерева; шаблон остаётся прежним. Локально добавленный объект удаляется из документа-владельца. Ссылки и другие patches на подавленные цели требуют разрешения конфликта, а не молчаливого удаления.
8. Новая версия шаблона обновляет поля без overrides. Совместимые явные overrides сохраняются. Исчезнувшая цель, несовместимый тип, цикл и повреждённая обязательная ссылка дают конфликт; активной остаётся последняя согласованная версия до исправления.
9. Структурный batch проверяет revision, строит кандидат, валидирует итоговые связи и публикует один результат с одним Undo. `Revert` удаляет выбранный override и возвращает нижележащее значение; UI и MCP используют один authoring contract.
10. В MVP входят обычные и ациклично вложенные экземпляры, overrides полей, добавление объектов/компонентов, suppression и ограниченный reparent. Variant inheritance, `Apply overrides to template`, перенос через границы экземпляров и поэлементное слияние массивов — после MVP.

Inspector показывает источник и локальные отличия, переход к шаблону, Revert и конкретные конфликтующие targets. Импортированный шаблон использует тот же принцип разделения baseline и пользовательского слоя; детали публикации поколения — в [исследовании импорта](17-asset-pipeline-and-blender-roundtrip.md).

### Два режима связи

- **strong reference** — asset должен присутствовать; ошибка блокирует export или явно видна;
- **soft reference** — путь/ID загружается по требованию; полезен для больших уровней и optional content.

References хранятся по `AssetId`, а путь — метаданные/alias. Переименование файла не должно ломать проект.

## 5. 2D и 3D: единое ядро, специализированные представления

Не выбирать между «два полностью отдельных движка» и «2D — это 3D с нулевой координатой». Нужны три уровня:

### Общие концепции

- entity, components, resources, scenes, prefabs;
- transform hierarchy и local/world conversion;
- camera, visibility, layers/masks;
- input actions, animation, audio, particles;
- fixed/variable time, events, diagnostics;
- serialization, asset references, editor commands.

### Специализированные типы

| Общее | 2D | 3D |
|---|---|---|
| Transform | `Vector2`, `Rotation2D`, depth/layer | `Vector3`, quaternion, parent basis |
| Renderable | Sprite, tilemap, canvas item, 2D text | mesh, skinned mesh, terrain, 3D text |
| Camera | orthographic + canvas transform | perspective/orthographic + frustum |
| Physics | 2D bodies/shapes/joints | 3D bodies/shapes/joints |
| Spatial query | rect/grid/quadtree | AABB/BVH/octree/grid |
| Lighting | 2D lights/masks, optional normal maps | lights/shadows/environment |

Godot прямо отмечает, что APIs и tutorial patterns 2D/3D во многом аналогичны, но имеет отдельные Node2D/Node3D и физические пространства ([Introduction to 3D](https://docs.godotengine.org/en/stable/tutorials/3d/introduction_to_3d.html), [physics introduction](https://docs.godotengine.org/en/stable/tutorials/physics/physics_introduction.html)). Это хороший UX-паттерн: одинаковые названия и операции, но не скрывать важные различия.

### Гибридные игры — критерий архитектуры

После двух базовых MVP-демо проверять расширенные сочетания:

- 2D gameplay на 3D backdrop;
- 3D world-space UI и 2D overlay;
- orthographic camera в 3D world;
- Sprite/quad в 3D;
- particles и audio независимо от dimensionality.

Это заставляет правильно отделить world transform, camera projection и render representation.

## 6. Game loop и расписание систем

Минимально понятный цикл:

```text
FrameStart
  input.poll_and_buffer()
  fixed_accumulator += real_delta
  ticks = 0
  while fixed_accumulator >= fixed_dt and ticks < max_catch_up_ticks:
      runtime_commands.apply_structural_barrier()
      input.consume_for_tick()
      gameplay.fixed_update(fixed_dt)
      physics.apply_body_commands()
      physics.step_and_wait(fixed_dt)
      physics.readback_transforms_and_queue_events()
      gameplay.process_physics_events_and_reactions()
      fixed_accumulator -= fixed_dt
      ticks += 1
  discard_excess_whole_ticks_with_diagnostic(fixed_accumulator)
  gameplay.update(variable_delta)
  display_state.prepare_interpolated(previous_tick, current_tick,
                                     fixed_accumulator / fixed_dt)
  gameplay.late_update(display_state, variable_delta)
  render.extract_snapshot(display_state)
  render.submit()
  diagnostics.end_frame()
FrameEnd
```

Structural commands, созданные во время tick, применяются только на барьере следующего tick, включая несколько ticks одного кадра. Начальная загрузка проходит отдельный явный барьер инициализации. Начальный лимит catch-up — четыре ticks; лишние целые интервалы отбрасываются с диагностикой, дробный остаток сохраняется. После pause/reset накопитель сбрасывается. Interpolated display state отделён от симуляции: Update/LateUpdate не переписывают рассчитанный physics transform динамического тела.

Physics и код, зависящий от столкновений, должны идти на fixed step; render/UI — variable step. Godot документирует отдельные `_physics_process` и `_process`, причем physics rate независим от framerate ([Idle and Physics Processing](https://docs.godotengine.org/en/stable/tutorials/scripting/idle_and_physics_processing.html)). Unity также подчеркивает, что `FixedUpdate` может вызываться несколько раз за кадр или не вызываться между кадрами и что physics simulation имеет определенное место в loop ([execution order](https://docs.unity3d.com/6000.5/Documentation/Manual/execution-order.html)).

### Не обещать ложную determinism

Fixed timestep стабилизирует расписание, но не делает автоматически всю физику детерминированной. Godot прямо предупреждает, что физика не гарантированно детерминирована ([physics introduction](https://docs.godotengine.org/en/stable/tutorials/physics/physics_introduction.html)). Для Faset fixed physics и variable frame являются базой; следующие режимы описывают дальнейшие возможности:

- `Realtime` — максимально плавный runtime;
- `Fixed` — фиксированный simulation step;
- `Record/Replay` — записывать input и seed, а не обещать byte-for-byte replay без теста backend;
- `Lockstep` — отдельная будущая возможность с жесткими ограничениями.

Системы имеют декларативные зависимости (`reads/writes`, `before/after`), а не неявный порядок регистрации. Unreal показывает практическую ценность tick groups и tick dependencies для physics/gameplay ordering ([Actor Ticking](https://dev.epicgames.com/documentation/unreal-engine/actor-ticking-in-unreal-engine)). Для нового движка достаточно сначала детерминированных фаз, затем parallel scheduling.

## 7. ECS, объектный код и data-oriented design

ECS дает cache-friendly iteration и уменьшает связанность при больших однородных наборах. Но это не религия и не универсальный public API. Data-oriented design — выбор layout по access patterns, а не обязательное использование archetype ECS ([ECS FAQ](https://github.com/SanderMertens/ecs-faq)). Game Programming Patterns отдельно рассматривает Component, Event Queue, Data Locality, Game Loop и другие паттерны ([catalog](https://gameprogrammingpatterns.com/contents.html)).

**Принято для Faset:** EnTT runtime, typed C++ gameplay API, явная metadata schema с постоянными `TypeId`/`FieldId`, удобные object/component descriptors в редакторе. Layout горячих данных и batching выбираются по измерениям. EnTT handles не записываются в JSON.

**Superseded, история сравнения:** первоначально предлагались Flecs/archetype queries и C# façade. Они не входят в выбранный стек. Lua добавляется после C++-основы отдельным модулем; наличие схемы само по себе не реализует Lua bindings.

### Сигналы и события

Не вызывать произвольные методы десятка объектов во время системного прохода. Использовать typed events/commands:

- события контактов, input actions, asset loaded;
- command buffer для structural changes;
- double-buffered event queues, чтобы producer и consumer имели понятный кадр доставки;
- логирование origin/target/sequence для отладки.

Event Queue и Double Buffer — проверенные декомпозирующие паттерны, но их нужно ограничивать schema и lifetime ([Game Programming Patterns](https://gameprogrammingpatterns.com/contents.html)).

## 8. Asset pipeline: source не равен runtime

Pipeline должен быть видимым графом:

```text
source file + importer settings + engine version
                    │
                    ▼
              importer/cache
                    │
          typed artifact + dependencies
                    │
                    ▼
          cooked platform package
```

Godot импортирует файлы в скрытые внутренние ресурсы и предупреждает, что ResourceLoader учитывает импорт, тогда как прямой FileAccess может работать в editor, но сломаться в export ([import process](https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/import_process.html)). Unity разделяет source asset, `.meta` с идентичностью и кэшируемые artifacts, которые можно регенерировать ([Asset Workflow](https://docs.unity3d.com/2019.3/Documentation/Manual/AssetWorkflow.html), [Asset Database](https://docs.unity3d.com/2020.3/Documentation/Manual/AssetDatabaseCustomizingWorkflow.html)). O3DE называет преобразование source в optimized product assets Asset Pipeline и выполняет его Asset Processor ([key concepts](https://docs.o3de.org/docs/welcome-guide/key-concepts)).

### Минимальный контракт importer

```text
ImporterId
Input extensions
Settings schema + defaults
Output asset type(s)
Dependency discovery
Content hash / importer version
Diagnostics (error/warning/info + source span)
Cancel/progress
Deterministic output
```

Нужно хранить рядом или в проектном manifest:

- стабильный `AssetId`;
- importer settings, не только результат;
- dependency graph;
- source hash и importer version;
- platform variants;
- last import diagnostics.

Ошибка импорта — first-class asset state: красный badge, понятная причина, путь к offending dependency и кнопка retry/open log.

## 9. Serialization и эволюция формата

Авторские сцены и data assets должны быть:

- текстовыми и diff-friendly;
- с явными типами и версиями schema;
- устойчивыми к reorder полей;
- способными сохранять неизвестные поля хотя бы в compatibility window;
- защищенными от случайного хранения runtime handles, GPU pointers и absolute paths.

Принят JSON с постоянными object/resource IDs, `TypeId`, `FieldId` и schema versions. Имена типов и полей могут сопровождать данные для чтения человеком, но не являются ключами идентичности. Конкретный envelope определяется реализацией и fixtures миграции; прежний YAML-пример с name-based references **superseded**.

Binary artifacts допустимы для cooked output и больших blobs, но исходный truth должен быть читаемым. Autosave пишет recovery snapshot, а не перезаписывает source без undo/history.

Миграция schema — отдельная команда, dry-run preview и резервный backup. Версию формата не связывать жестко с версией renderer.

## 10. Потоки и async

Безопасная базовая модель для личного engine:

- AuthoringService сериализует изменения документов; runtime world отдельно владеет structural changes EnTT в разрешённых фазах;
- worker threads импортируют assets, компилируют shaders, готовят jobs;
- physics/render native calls выполняются на thread, который требует backend;
- cross-thread изменения приходят через typed queue и применяются в определенной фазе;
- cancellation и progress обязательны для долгих задач.

Не делать «все async» API без ясной точки commit. Loader может background-load, но переход `Loading → Ready` должен быть наблюдаемым и происходить в transaction-safe фазе.

## 11. Расширения и backend abstraction

Редакторские C++ plugins собираются как DLL/SO под точную версию SDK/toolchain; стабильный произвольный C++ ABI и hot-unload не обещаются. Gameplay статически включается в отдельный Player. Регистры расширений могут добавлять:

- component/schema;
- system;
- importer;
- editor panel/tool;
- export step;
- render feature/backend.

Плагин не должен подменять core ID/lifetime/serialization conventions. Registry должен проверять версии API, зависимости и capability.

Backend interface абстрагирует window/surface/device/resource submission, но не должен прятать концепции, которые нужны debugging. Например, render graph может сообщать pass/resource barriers в trace, даже если gameplay видит только `Renderable`.

## 12. Диагностика и тестируемость — не надстройка

Минимальная observability schema:

- structured log: category, severity, entity/asset ID, frame, correlation ID;
- frame trace: systems, durations, allocations, render passes;
- asset trace: importer, dependencies, cache hit/miss;
- authoring snapshot/diff; runtime traces остаются отдельной внутренней диагностикой и не публикуются через MCP;
- command journal: кто, что, когда и почему изменил;
- validation: missing resources, invalid transforms, duplicate IDs, unowned native handles.

Тестовые уровни:

1. pure math/serialization/schema migrations;
2. system tests на headless world;
3. asset importer golden tests;
4. physics adapter tests с tolerance;
5. render extraction tests без GPU;
6. GPU/backend smoke tests;
7. editor command/undo integration tests;
8. маленькие end-to-end projects: 2D, 3D, hybrid.

Главная цель — ошибка из графического кадра должна быть трассируема обратно к asset/entity/component/command.
