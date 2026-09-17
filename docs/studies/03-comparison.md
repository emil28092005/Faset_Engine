# 03. Сравнение существующих подходов

**Статус на 18.09.2026.** Исследование и проектные решения, не описание реализованного движка. Канонические решения — [ARCHITECTURE.md](../ARCHITECTURE.md), порядок до и после MVP — [PLAN.md](../../PLAN.md). Сведения о других движках сохраняются как исследовательский контекст; прежние рекомендации, помеченные **superseded**, не задают стек Faset.

Матрица ниже — история оценки чужих подходов, не обещание реализовать все перечисленные возможности. Выбор Faset уже сделан: C++/EnTT, собственные retained C++ UI и Vulkan 1.3 renderer/RenderGraph, Slang/SDL3, CMake/Ninja/Clang и clang-cl, Box2D/Box3D; Lua добавляется отдельным обязательным этапом и остаётся необязательным для конкретной игры. MCP ограничен редакторским authoring/import/build/PlayStop/logs, отсутствует в Player и не инспектирует runtime world.

Это не таблица «лучший engine». У каждого проекта другая целевая цена сложности. Вопрос: какую идею забрать в новый движок, а какую не импортировать.

## 1. Сводная матрица

| Движок/подход | Сильный mental model | Что особенно полезно | Цена/риск | Вывод для нового движка |
|---|---|---|---|---|
| Godot | Node + Scene + Resource | композиция сцен, единый editor, простая project structure, remote scene/debug | Node легко превращается в object soup; 2D/3D имеют отдельные типы; физика не обещает determinism | взять scene/resource UX и explicit 2D/3D parity; remote inspection оставить сравнением, без runtime MCP |
| Unity | GameObject + Component + Prefab + Asset Database | быстрый authoring, prefab variants, package model, большой ecosystem | скрытое состояние import/cache, сложный execution order, heavy editor/runtime coupling | взять prefab overrides, actions, frame debugger; сделать source/cache boundaries более прозрачными |
| Unreal | Actor/Component + Level/Asset + отражение | мощные editor tools, world/level authoring, Details/Outliner, trace/Insights, source-control UX | высокая когнитивная и build complexity, tick/GC/reflection pitfalls | взять details/outliner/trace patterns; не следовать enterprise-scale breadth |
| O3DE | Entity Component System + Editor/Runtime/System components + Gems | четкая component composition, Asset Processor, modular Gems, editor/runtime split | большой surface area, много abstractions, тяжелый onboarding | взять component categories, asset pipeline, plugins; держать API меньше |
| Bevy | ECS World + Resources + Systems + plugins | data-driven composition, explicit schedules, modular render/asset systems, code-first ergonomics | code-first UX, evolving APIs, ECS complexity для новичка | взять explicit schedule/resources/plugin boundaries; добавить визуальный authoring layer |

## 2. Godot: композиция и маленький проект

Godot явно строит философию вокруг object-oriented composition, scenes как reusable units и возможности сочетать editor и code ([design philosophy](https://docs.godotengine.org/en/stable/getting_started/introduction/godot_design_philosophy.html)). Scene Tree и FileSystem легко объяснить новичку. Inspector, Project Settings и editor plugins превращают metadata в UX surface ([Inspector](https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html), [Project Settings](https://docs.godotengine.org/en/stable/tutorials/editor/project_settings.html), [EditorPlugin](https://docs.godotengine.org/en/stable/classes/class_editorplugin.html)).

**Перенять:**

- любой node/subtree может стать сценой;
- сцена открывается и запускается независимо;
- ресурсы имеют собственное редактирование;
- import errors видны рядом с ассетами; remote scene Godot сохраняется здесь как сравнение, не требование runtime MCP Faset.

**Не перенять без критики:**

- смешивание lifecycle, hierarchy и behaviour в огромном количестве Node types;
- неявные глобальные singletons как основной способ коммуникации;
- отдельные 2D/3D API там, где общий concept был бы удобнее.

## 3. Unity: скорость результата и prefab contract

Unity показывает, насколько powerful один consistent loop `GameObject → Component → Inspector → Prefab → Play`. Prefab поддерживает nested instances и variations ([Prefabs](https://docs.unity3d.com/6000.1/Documentation/Manual/Prefabs.html)); контекстный Prefab Mode и breadcrumbs уменьшают риск редактировать не тот уровень ([Prefab Mode](https://docs.unity3d.com/6000.5/Documentation/Manual/EditingInPrefabMode.html)). Asset Database разделяет source, meta identity и cached artifacts ([Asset Workflow](https://docs.unity3d.com/2019.3/Documentation/Manual/AssetWorkflow.html)).

**Перенять:**

- inspector-driven authoring;
- prefab instance source/override visualization;
- named action input вместо raw keys;
- Frame Debugger, который делает render pipeline пошагово обозримым ([Frame Debugger](https://docs.unity3d.com/Manual/FrameDebugger.html)).

**Не перенять без критики:**

- dependency на opaque generated Library/cache;
- «магический» execution order; scheduling should be explicit;
- authoring-правки компонентов в обход общего command/history layer. Gameplay изменяет runtime-компоненты через runtime API без редакторского Undo; structural changes применяются через отдельный runtime command buffer.

## 4. Unreal: масштабируемая authoring-система

Unreal показывает ценность богатого Outliner + Details panel: actor можно найти в иерархии и редактировать свойства в контексте ([Level Editor](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-in-unreal-engine), [Details panel](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-details-panel-in-unreal-engine)). Source Control встроен в Content Browser и показывает checkout/history/diff ([Source Control](https://dev.epicgames.com/documentation/en-us/unreal-engine/source-control-in-unreal-engine)). Tick groups и dependencies делают порядок работы подсистем явным ([Actor Ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine)). Unreal Insights демонстрирует, что trace должен включать gameplay, objects, physics и cook, а не только FPS ([Insights](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-reference-in-unreal-engine-5)).

**Перенять:**

- Outliner/Details split;
- human-readable reflection metadata;
- explicit runtime/editor distinction;
- trace channels и frame/object history;
- asset source-control status.

**Не перенять:**

- broad enterprise feature set;
- повсеместный per-actor ticking;
- сложную рефлексию без генерации schema и compile-time validation.

## 5. O3DE: component boundaries и pipeline

O3DE формулирует entity/component model, причем компоненты могут объявлять required/provided services, что предотвращает невалидное состояние entity. Отдельные editor components и system components — полезная архитектурная граница ([ECS overview](https://www.docs.o3de.org/docs/user-guide/programming/components/overview)). O3DE также явно выделяет Asset Processor, authoring tools, plugins и build system ([Key Concepts](https://docs.o3de.org/docs/welcome-guide/key-concepts)).

**Перенять:**

- schema-level component dependencies;
- editor component, runtime component, system service;
- importer/processors as first-class pipeline;
- modules/plugins with declared dependencies.

**Осторожно:**

- dependency graph component-услуг не должен быть непостижимым; показывать его в Inspector;
- не создавать micro-service архитектуру внутри маленького движка.

## 6. Bevy: explicit data flow и systems

Bevy предлагает code-first ECS, typed Assets и plugin composition. В релизных материалах подчеркиваются modularity, render phases, entity/component-driven draw functions и explicit resources ([Bevy 0.6](https://bevy.org/news/bevy-0-6)); новый scene system делает сцены composable, patchable и dependency-aware ([Bevy 0.19](https://bevy.org/news/bevy-0-19)).

**Перенять:**

- schedule phases и typed `Res`/components;
- asset handles + async loading;
- plugins as composition units;
- render extraction вместо обращения gameplay напрямую к GPU.

**Не перенять:**

- code-only authoring как единственный путь;
- нестабильную публичную surface без compatibility policy;
- предположение, что ECS автоматически делает every workload faster.

## 7. Синтез

Сильнейший общий паттерн не «ECS» и не «scene graph», а наличие **понятного authoring contract**:

```text
Object in editor
  = stable identity
  + inspectable schema
  + composable parts
  + explicit references
  + reversible changes
  + runtime mapping
```

Для нового движка разумно выбрать:

- Godot-подобную простоту сцен;
- Unity-подобный prefab/Inspector loop с nested composition и явными overrides; variants и Apply to template — после MVP;
- Unreal-подобные trace/Outliner/Details;
- O3DE-подобное разделение editor/runtime/system и Asset Processor;
- Bevy-подобные schedules/plugins/render extraction.

И сознательно не переносить их масштаб, legacy и неявные соглашения.

Прежние варианты C#/Flecs, готовый UI toolkit и прямое редактирование Player через automation **superseded**. Blender используется в официальной сборке: обычный glTF/GLB импорт самодостаточен; optional Python add-on добавляет устойчивые IDs и удобную кнопку экспорта. Его отсутствие не блокирует импорт, но ограничивает гарантии reimport после rename частей. MVP — два маленьких 2D/3D-демо с baseline PBR/тенями и самостоятельным экспортом под Linux/Windows; передовые графические технологии развиваются после него.
