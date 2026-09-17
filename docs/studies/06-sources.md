# 06. Источники и аннотации

Актуализировано 18.09.2026. Принятый стек описан в [архитектуре](../ARCHITECTURE.md), этапы реализации — в [PLAN.md](../../PLAN.md). Версии изученных исходников закреплены в [манифесте](source-manifest.json). Сравнительная библиография ниже сохраняет историю выбора; наличие ссылки не означает зависимость Faset.

Ссылки ниже использованы как проверяемые опорные материалы. Приоритет отдан официальной документации проектов и оригинальным справочным материалам; они описывают конкретные решения, но не доказывают, что это единственный или лучший дизайн для нового движка.

## Godot

1. [Godot design philosophy](https://docs.godotengine.org/en/stable/getting_started/introduction/godot_design_philosophy.html) — сцены как композиционные/reusable units, сочетание editor и code, object-oriented composition.
2. [Overview of Godot key concepts](https://docs.godotengine.org/en/stable/getting_started/introduction/key_concepts_overview.html) — Scene Tree, Nodes, Scenes, Resources и единый mental model для gameplay/UI.
3. [Introduction to 3D](https://docs.godotengine.org/en/stable/tutorials/3d/introduction_to_3d.html) — Node2D/Node3D, похожие APIs, orthographic 2D-in-3D и гибридные сценарии.
4. [Idle and physics processing](https://docs.godotengine.org/en/stable/tutorials/scripting/idle_and_physics_processing.html) — разделение variable per-frame processing и fixed physics processing.
5. [Physics introduction](https://docs.godotengine.org/en/stable/tutorials/physics/physics_introduction.html) — fixed-rate physics и важное предупреждение: engine physics не гарантирует determinism.
6. [Import process](https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/import_process.html) — source files, hidden imported resources, import settings и различие ResourceLoader/FileAccess в export.
7. [Inspector Dock](https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html) — searchable property inspector, sections, revert icon, sub-resources и resource editing.
8. [Project Settings](https://docs.godotengine.org/en/stable/tutorials/editor/project_settings.html) — категории, search/reset, Input Map, Localization, Plugins, Import Defaults и human-readable `project.godot`.
9. [EditorPlugin API](https://docs.godotengine.org/en/stable/classes/class_editorplugin.html) — расширение inspector, import/export/scene format plugins.
10. [Debugging tools overview](https://docs.godotengine.org/en/stable/tutorials/scripting/debug/overview_of_debugging_tools.html) — remote scene inspection, profiler/debugger, collision/navigation visualization и reload workflow.
11. [InputMap](https://docs.godotengine.org/en/stable/classes/class_inputmap.html) — named actions, multiple input events и deadzone.
12. [Default editor shortcuts](https://docs.godotengine.org/en/4.3/tutorials/editor/default_key_mapping.html) — discoverable keyboard-first operations and configurable shortcuts.

## Unity

13. [Prefabs](https://docs.unity3d.com/6000.1/Documentation/Manual/Prefabs.html) — GameObject + Components, nested prefabs и variations как reusable authoring contract.
14. [Editing Prefab Mode](https://docs.unity3d.com/6000.5/Documentation/Manual/EditingInPrefabMode.html) — context/isolation, breadcrumbs и визуальное отделение prefab contents.
15. [Asset Workflow](https://docs.unity3d.com/2019.3/Documentation/Manual/AssetWorkflow.html) — `.meta`, processing и превращение одного source file в несколько imported assets.
16. [Customizing Asset Database workflow](https://docs.unity3d.com/2020.3/Documentation/Manual/AssetDatabaseCustomizingWorkflow.html) — source/meta/artifact separation, cache regeneration и importer settings.
17. [Event function execution order](https://docs.unity3d.com/6000.5/Documentation/Manual/execution-order.html) — конкретный execution loop и место physics simulation.
18. [Runtime UI event system and input handling](https://docs.unity3d.com/6000.2/Documentation/Manual/UIE-Runtime-Event-System.html) — action-based input и UI navigation events.
19. [Frame Debugger](https://docs.unity3d.com/Manual/FrameDebugger.html) — остановка кадра и пошаговое исследование render events/state.

## Unreal Engine

20. [Level Editor](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-in-unreal-engine) — Outliner как hierarchical scene view и selection/editing в контексте.
21. [Level Editor Details Panel](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-details-panel-in-unreal-engine) — Details panel как schema-driven property surface для Actor.
22. [Actor ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine) — tick groups и dependencies для упорядочивания gameplay/physics.
23. [Unreal Insights reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-reference-in-unreal-engine-5) — trace channels для CPU, gameplay, objects, physics и cook.
24. [Source Control in Unreal](https://dev.epicgames.com/documentation/en-us/unreal-engine/source-control-in-unreal-engine) — checkout, history, diff и asset-aware source control.
25. [Using Source Control in the Unreal Editor](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-source-control-in-the-unreal-editor) — checkout/add-on-save и status feedback в editor workflow.

## O3DE

26. [Overview of Entities and Components](https://www.docs.o3de.org/docs/user-guide/programming/components/overview) — composition, component services/dependencies, editor/runtime/system components.
27. [Key Concepts: How O3DE Works](https://docs.o3de.org/docs/welcome-guide/key-concepts) — ECS, editor tools, Gems/plugins, Asset Pipeline, Asset Processor и build system.
28. [Material Editor](https://docs.o3de.org/docs/atom-guide/look-dev/tools/material-editor) — undo/redo, parent/child materials и automatic asset processing.

## Bevy и общие паттерны

29. [Bevy 0.6: modular render architecture](https://bevy.org/news/bevy-0-6) — plugin composition, render phases, entity/component-driven draw functions и render graph lessons.
30. [Bevy 0.19: scenes and app settings](https://bevy.org/news/bevy-0-19) — composable/patchable/dependency-aware scenes, typed assets/resources.
31. [Game Programming Patterns — contents](https://gameprogrammingpatterns.com/contents.html) — Game Loop, Component, Event Queue, Data Locality, Double Buffer, Command и другие patterns с компромиссами.
32. [Entity Component System FAQ](https://github.com/SanderMertens/ecs-faq) — data-oriented design как подбор layout по access patterns, а не обязательная идеология archetype ECS.
33. [The Essence of Entity Component System](https://arxiv.org/html/2606.14919v1) — технический обзор archetype ECS, SoA и cache locality; использовать как исследовательский материал, проверяя актуальность и peer-review status.

## Как проверять выводы

- Документация engine описывает intended behavior, но не гарантирует одинаковые performance/UX outcomes в другом проекте.
- Claims о determinism, hot reload и производительности проверять экспериментом в новом движке.
- Любую выбранную abstraction оценивать по стоимости сопровождения, debugging и schema migration, а не только по benchmark.
- Перед принятием dependency проверить license, поддерживаемые платформы, ABI/runtime requirements и возможность заменить backend.

## Официальные источники принятого стека

- [EnTT: ECS и многопоточность](https://github.com/skypjack/entt/wiki/Entity-Component-System) — storage/views, ограничение thread safety и organizer; собственный scheduler остаётся задачей Faset.
- [SDL3](https://wiki.libsdl.org/SDL3/FrontPage), [Vulkan surface](https://wiki.libsdl.org/SDL3/CategoryVulkan), [DPI](https://wiki.libsdl.org/SDL3/README-highdpi) — граница платформенного слоя.
- [Vulkan versions](https://docs.vulkan.org/guide/latest/versions.html), [features/limits](https://docs.vulkan.org/guide/latest/querying_extensions_features.html) — Vulkan 1.3 и отдельная проверка возможностей устройства.
- [Slang introduction](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/00-introduction.html), [reflection](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/09-reflection.html) — модульность, SPIR-V и параметры шейдеров; совместимость с большей частью HLSL, а не со всеми engine-specific shaders.
- [Box2D simulation](https://box2d.org/documentation/md_simulation.html), [Box3D](https://github.com/erincatto/box3d) — физические миры, handles и fixed-step интеграция.
- [CMake presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html), [Ninja manual](https://ninja-build.org/manual.html), [Clang toolchain](https://clang.llvm.org/docs/Toolchain.html) — роли инструментов сборки.
- [clang-cl и Windows SDK/runtime](https://clang.llvm.org/docs/UsersManual.html#windows-system-headers-and-library-lookup), [cross-compilation](https://clang.llvm.org/docs/CrossCompilation.html) — платформенные зависимости сохраняются.
- [Blender glTF exporter](https://github.com/KhronosGroup/glTF-Blender-IO) — внешний экспортёр; profile и устойчивые IDs задаются интеграцией Faset.
- [Dear ImGui](https://github.com/ocornut/imgui) — диагностический UI, не выбранная основа редактора.

Лицензии и границы публикации собраны в [DEPENDENCIES.md](../DEPENDENCIES.md) и [PUBLICATION.md](../PUBLICATION.md). URL официальных руководств могут развиваться; findings по исходникам привязаны к commits. Ни один источник не заменяет будущие приёмочные проверки Faset.
