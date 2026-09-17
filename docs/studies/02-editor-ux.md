# 02. UX редактора и повседневный workflow

**Статус на 18.09.2026.** Исследование и проектные решения, не описание реализованного движка. Канонические решения — [ARCHITECTURE.md](../ARCHITECTURE.md), порядок до и после MVP — [PLAN.md](../../PLAN.md). Сведения о других движках сохраняются как исследовательский контекст; прежние рекомендации, помеченные **superseded**, не задают стек Faset.

Принята собственная retained UI-система на C++: декларативные layout/styles, тёмная тема по умолчанию, schema-driven Inspector. ImGui используется для debug tools. Ни UI Toolkit Unity, ни Qt/web UI не являются выбранной основой. Описанные ниже расширенные инструменты — backlog; граница MVP определяется [планом](../../PLAN.md).

## 1. Основная UX-гипотеза

Пользователь движка не хочет «управлять ECS». Он хочет ответить на вопросы:

- где находится объект;
- из чего он состоит;
- почему он выглядит/движется именно так;
- что произойдет, если я изменю параметр;
- как вернуться назад;
- что попадет в экспорт.

Поэтому хороший UX строится вокруг **объекта, намерения и обратной связи**, а не вокруг внутренней терминологии engine.

## 2. Главный цикл: observe → edit → run → inspect → repeat

Время цикла — главный product metric личного движка:

```text
1. Open project
2. Find/create scene
3. Add entity or prefab
4. Select it
5. Change one property
6. Press Play / run current scene
7. Observe game
8. Read editor/build diagnostics or debug the Player separately
9. Stop safely; edit scene remains independent
10. Save and export
```

Каждый шаг должен иметь одну очевидную точку входа. Если для добавления Sprite нужно вручную создать entity, зарегистрировать component type, загрузить texture и прописать renderer binding — это внутренний API, а не UX.

### Метрики

Отслеживать без телеметрии пользователя, локально:

- first pixel: от Create Project до первого кадра;
- first interaction: до управляемого объекта;
- edit-to-visible: от изменения Inspector до результата;
- error-to-cause: от красной ошибки до исправления;
- clean export: от проекта до запускаемого build;
- recovery: насколько легко отменить ошибку.

Целевые значения для MVP лучше задать позже после baseline, но каждый релиз должен уменьшать хотя бы один цикл.

## 3. Информационная архитектура редактора

Стартовый layout:

```text
┌ Project / Scene tabs / Play / Stop / Build ┐
├────────────┬──────────────────────────┬─────────────┤
│ Scene Tree │       Scene View         │ Inspector   │
│            │   2D | 3D | Game | Debug │             │
├────────────┴──────────────────────────┴─────────────┤
│ Asset Browser | Console | Profiler | History        │
└──────────────────────────────────────────────────────┘
```

Панели не должны быть обязательным лабиринтом: сохранить workspace per role (programmer, designer, debugger), tabs и reset layout. Главные действия доступны меню и keyboard shortcuts; mouse-only workflow недопустим.

Полезные концепции из Godot:

- Scene Tree и FileSystem дают две разные оси: composition и assets;
- Inspector автоматически показывает свойства выбранного объекта, поддерживает поиск, секции, revert и sub-resources ([Inspector Dock](https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html));
- Project Settings группирует общие настройки, Input Map, Localization, Globals, Plugins и Import Defaults, имеет поиск и reset ([Project Settings](https://docs.godotengine.org/en/stable/tutorials/editor/project_settings.html)).

Новый движок должен повторить эти **mental models**, но сделать их более согласованными с command journal и automation tools.

## 4. Scene Tree и selection

Scene Tree — не просто список entity IDs. Он должен показывать:

- display name + stable ID по запросу;
- prefab/scene boundary;
- disabled/hidden/locked state;
- missing component/resource badges;
- parent/child transform relation;
- только authoring objects; runtime hierarchy не является частью MCP/Scene Tree MVP;
- search by name, type, tag, component, asset reference.

Selection — глобальный контекст editor. Выделение в Scene View, Tree, Asset Browser и Inspector синхронно. Multi-select должен позволять массовое изменение через transaction с preview.

Не полагаться только на hierarchy: для больших сцен нужен flat search/results view и reverse reference view «кто использует этот asset/entity». Hierarchy удобна для transform и чтения композиции, но не является полноценной базой данных.

## 5. Inspector: главный ergonomic leverage

Inspector строится по общей явной C++ metadata schema (`TypeId`/`FieldId`) для компонента, ресурса и editor-only данных.

Каждое поле имеет:

- label + tooltip с единицами измерения;
- valid range/enum и validation message;
- default, current и inherited value;
- source: template/scene/instance и provenance импортированной основы;
- reset/revert;
- copy path / copy value;
- searchable category;
- optional advanced section;
- link to references and documentation.

Важно отличать:

```text
invalid value      → не принять или показать inline error
valid but risky    → принять, warning и explain
valid override     → показать source и revert
Player state      → отдельная сессия; source меняется только authoring-командой
```

Godot показывает revert icon для измененных относительно оригинала значений и умеет делать sub-resources unique ([Inspector Dock](https://docs.godotengine.org/en/stable/tutorials/editor/inspector_dock.html)). Unity Prefab Mode использует context/isolation и breadcrumb, чтобы автор понимал, редактирует ли asset или instance ([Editing Prefab Mode](https://docs.unity3d.com/6000.5/Documentation/Manual/EditingInPrefabMode.html)). Для нового движка критичен аналогичный `source breadcrumb`:

```text
Level01 > Player instance > Weapon prefab > Transform
```

## 6. Scene View и gizmos

### Общие требования

- frame selected, focus, orbit/pan/zoom;
- snapping и привязки с понятным modifier;
- local/world pivot;
- визуальные bounds/colliders/nav/audio;
- solo/isolate, hide, lock;
- orthographic/perspective toggle;
- camera bookmarks;
- safe preview before commit;
- одинаковые shortcuts для 2D и 3D, где это возможно.

### 2D

- canvas coordinates, pixels/world units и origin видимы;
- zoom не меняет смысл snapping;
- tilemap, sprite pivot, nine-patch и z/layer order редактируются в контексте;
- draw order объясним: layer, z-index, material/order override.

### 3D

- transform gizmo и numeric entry;
- grid и axis labels;
- frustum/camera preview;
- selection outline и object bounds;
- light/physics probes как debug overlays, не как неявные runtime objects.

### Hybrid

Переключатель должен быть не «2D editor против 3D editor», а режимом представления: 2D camera view, 3D perspective, game preview. Один world может иметь Sprite2D и Mesh3D.

## 7. Commands, undo/redo и история

Любое изменение через UI — команда:

```text
SetProperty(documentId, objectAddress, componentId, fieldId, newValue, expectedRevision)
AddComponent(objectAddress, componentId, typeId, initialData)
Instantiate(prefabId, parentId, instanceId)
Delete(selection, reversiblePayload)
```

Команды группируются в transaction («переместить объект мышью» — один undo, а не сотни micro-steps). История содержит timestamp, source (`Inspector`, `MCP`, `script`, `importer`) и human-readable summary.

Требования:

- undo/redo работает в Scene Tree, Inspector, import settings и material editor;
- destructive action имеет preview/confirmation или reversible trash;
- изменения Player не попадают в edit history; обратный Apply из runtime не входит в MVP;
- autosave и recovery snapshot не уничтожают undo history;
- command journal экспортируется для воспроизведения bug.

## 8. Play Mode и hot reload

Принятый Play запускает отдельный Player со snapshot редактируемой сцены и статически собранным gameplay:

```text
Edit document → validated snapshot → separate Player → Stop
      ↓                                    ↓
Further authoring edits              temporary runtime state
```

Сцена может продолжать редактироваться независимо от snapshot запуска; для применения её новой версии к игре выполняется новый запуск. Начальный цикл C++: Stop → incremental build → Play. Редактор сохраняет контекст проекта и показывает ошибки компиляции с файлом и строкой. Кнопки MVP — Play, Stop, Restart, Pause и single-step; специальные runtime debugger UI относятся к дальнейшим инструментам. Управление сессией не предоставляет MCP доступ к runtime world.

MCP вызывает editor Play/Stop, импорт и build, читает authoring state и editor logs. Он не получает runtime hierarchy, значения компонентов Player, runtime mutation tools или MCP endpoint в игре. Отдельная диагностика Player не превращается в MCP bridge.

**Историческое сравнение, superseded для MVP Faset:** Godot поддерживает remote scene inspection, изменение параметров живой игры и reload ([debugging overview](https://docs.godotengine.org/en/stable/tutorials/scripting/debug/overview_of_debugging_tools.html)). Ранее здесь предлагались live edits и Apply выбранных runtime changes обратно в source. Эти сценарии не входят в принятый первый цикл; универсальный state-preserving native hot reload также отложен.

Shader/data reload можно исследовать отдельно после надёжного жизненного цикла ресурсов. Lua — обязательный последующий этап, его reload требует собственного контракта.

## 9. Debugging UX: от симптома к причине

Следующие вопросы задают направление будущих локальных debug tools. Это исследовательский backlog после базовой диагностики MVP; runtime-инспекция не предоставляется через MCP.

### «Почему объекта нет?»

Один action `Explain Visibility` показывает:

- entity active/disabled;
- parent visibility/layer;
- camera/frustum result;
- renderer extraction status;
- missing/failed asset;
- material/shader failure;
- draw pass and culling reason.

### «Почему объект движется неправильно?»

Показать:

- owning systems;
- last writer per property;
- fixed vs variable tick;
- physics body mapping;
- incoming commands/events;
- transform parent chain;
- timeline of values.

### «Почему frame slow?»

Показать hierarchy:

```text
Frame 16.6 ms
  Systems 4.1 ms
  Physics 2.8 ms
  Render extraction 1.2 ms
  GPU passes 7.4 ms
  Asset/IO 0.3 ms
```

Нужны toggles для collision shapes, bounds, nav, lights, overdraw/draw calls и entity IDs. Godot предоставляет debugger/profilers, remote inspection и визуализацию collision/navigation ([debugging tools](https://docs.godotengine.org/en/stable/tutorials/scripting/debug/overview_of_debugging_tools.html)); Unity Frame Debugger умеет остановить кадр и пошагово показывать render events ([Frame Debugger](https://docs.unity3d.com/Manual/FrameDebugger.html)); Unreal Insights строит трассу CPU/gameplay/cook channels ([Insights reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-reference-in-unreal-engine-5)). Новому движку стоит взять эти идеи с единым correlation ID.

## 10. Asset Browser и project hygiene

Asset Browser должен показывать не только thumbnails:

- тип, размер, импортированный статус;
- dependency/reverse-dependency graph;
- source path и AssetId;
- importer settings;
- platform variants;
- cache status;
- errors/warnings;
- usages и open-in-context.

Drag-and-drop создает предсказуемый результат: texture в Sprite добавляет/заменяет компонент или resource, а не молча копирует файл. При rename сохраняется AssetId и обновляется registry path; устойчивые ссылки не переписываются ради нового имени. Редактор показывает реально изменённые файлы.

Source control awareness полезна и в solo project: modified/untracked/generated/conflict badges. Unreal встроенно показывает checkout/history/diff и может checkout on save ([Source Control](https://dev.epicgames.com/documentation/en-us/unreal-engine/source-control-in-unreal-engine)); Новый движок может начать с простого read-only Git status и текстовых diff.

## 11. Input UX

В gameplay коде не использовать raw `Key.W` как единственный API. Использовать named actions:

```text
Move (Vector2)
Look (Vector2)
Jump (Button)
Interact (Button)
```

Action Map хранит bindings для keyboard, mouse, gamepad, touch и rebinding. UI navigation — отдельный context с приоритетом над gameplay. Godot InputMap управляет actions и несколькими InputEvents, включая deadzone ([InputMap](https://docs.godotengine.org/en/stable/classes/class_inputmap.html)); Unity Input System строит named actions и UI events поверх action asset ([Runtime UI event system](https://docs.unity3d.com/6000.2/Documentation/Manual/UIE-Runtime-Event-System.html)).

Editor должен показывать live input monitor: action, raw device event, resolved value, active context и deadzone. Это устраняет классическую ошибку «клавиша работает в editor, но не в game».

## 12. Accessibility и cognitive load

Даже internal editor должен иметь:

- keyboard-first command palette;
- все действия с menu/shortcut и discoverability;
- масштабируемый UI и high contrast;
- не использовать цвет как единственный сигнал;
- readable error text и copyable diagnostics;
- focus order и screen-reader labels для важных controls;
- reduced motion в editor preview;
- единые единицы/локализация чисел;
- сохраненные workspace и пользовательские shortcuts.

UX «удобного личного движка» — не меньше функций, а меньше скрытого состояния, модальных ловушек и неочевидных разрушительных операций.

## 13. Шаблоны и Blender: принятые границы UX

Вложенный экземпляр явно отделён в дереве; Inspector показывает источник, локальный override и Revert. В MVP доступны field overrides, добавление объектов/компонентов, suppression и reparent внутри одного экземпляра; перенос через nested boundary, variant inheritance и Apply to template отложены. Patch использует instance chain/ObjectId/ComponentId/FieldId; display name и transform path не заменяют ID. Удалённая цель и несовместимая схема дают конфликт с сохранением пользовательских данных. [Полные правила](01-architecture.md).

Обычный официальный Blender не модифицируется. Стандартный импорт glTF/GLB работает без дополнений. Необязательный Python add-on упрощает экспорт, сохраняет source IDs и manifest; без устойчивых IDs нельзя гарантировать matching внутренних частей после rename/reorder. Gameplay, physics settings и instance overrides принадлежат Faset и не перезаписываются новым экспортом.
