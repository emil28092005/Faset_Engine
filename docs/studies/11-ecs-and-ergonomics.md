# ECS: стоит ли применять и как сделать удобно

Исследование от 17.09.2026. Основание: тела UE 5.8.2 MassEntity в локальном commit `16d75d84714512edfb744e1fd0a59e9c74d57873`, официальные документы Flecs 4.1, Bevy ECS 0.19.1, Unity Entities 1.4 и репозиторий EnTT. Производительность вариантов не измерялась. Ниже есть подтверждённые механизмы и отдельно предлагаемые решения для нового движка.

**Актуализация 18.09.2026:** runtime ECS и **EnTT приняты**, ядро/gameplay сначала на C++, Lua добавляется следующим языковым этапом. Текущие контракты — в [архитектуре](../ARCHITECTURE.md), реализация и проверки — в [PLAN.md](../../PLAN.md). Разбор Mass/Flecs/Bevy ниже сохраняет исследовательское обоснование; это не продолжающийся конкурс библиотек. Код Faset ещё не создан.

## Принятое решение для нашего движка

**Используем EnTT для runtime с собственным авторским слоем сцен, объектов и компонентов.** Не делать архетипы, chunks и command buffers обязательными понятиями для художника или автора простого скрипта. ECS нужен как способ организовать состояние и обработку, а удобство должно обеспечиваться Inspector, шаблонами, типизированными API, понятным расписанием и диагностикой.

Библиотечные детали закрываются API Faset; собственный storage не входит в первый этап. EnTT выбран ради ограниченной интеграции storage/views с нашим API, а не доказанного преимущества скорости. Проверки прототипа проверяют реализацию выбранного решения. Безболезненная замена backend не обещается: semantics queries, ownership и relationships влияют на дизайн.

## Что здесь означает ECS

- **Entity** — идентификатор объекта с жизненным циклом.
- **Component** — типизированные данные, принадлежащие entity.
- **System** — обработчик набора данных, например движения всех объектов с Position и Velocity.
- **Query** — описание набора компонентов и фильтров, по которому система получает данные.

Компонентная композиция сама по себе ещё не даёт cache-friendly ECS. Тысяча объектов, у каждого из которых массив виртуальных `Update()`-компонентов, отличается по исполнению от одного пакетного прохода по Position/Velocity. Но ECS тоже не равен обязательным archetypes: существуют sparse-set, table/archetype и гибридные способы хранения. Bevy документирует tradeoff: table storage ориентировано на итерацию, sparse sets — на добавление/удаление компонентов; конкретный результат зависит от нагрузки. [Bevy ECS: storage и системы](https://docs.rs/bevy_ecs/0.19.1/bevy_ecs/).

## Где ECS полезен, а где его не стоит навязывать

**Сильные кандидаты:** множество похожих движущихся объектов, снаряды, толпы, состояние AI-агентов, массовое обновление transforms, подготовка данных для renderer, фильтрация объектов по составу компонентов. Общий признак — повторяемая обработка похожих данных, которую можно выполнять пакетами.

**Слабые кандидаты для обязательного ECS API:** layout редактора, диалог импорта, undo stack, compiler graph, сетевой клиент, asset database, GPU allocator. Они могут взаимодействовать с ECS, оставаясь обычными сервисами и структурами данных. Даже игровую логику единичной двери или меню необязательно писать как несколько глобальных систем.

**ECS не гарантирует** ускорение маленькой сцены, deterministic physics, масштабирование по потокам, хорошую архитектуру или простые скрипты. Случайные lookup по связанным entity могут вернуть cache misses; избыточное дробление данных — множество queries; частые смены состава — миграции между хранилищами. Пользу нужно проверять на полном кадре, включая adapters, physics и render extraction.

## Что реально видно в UE MassEntity

### 1. Данные упаковываются в массивы компонентов внутри chunk

`ConfigureFragments` считает размер entity, доступный объём chunk, вместимость и offsets выровненных массивов каждого fragment. Это конкретная реализация структуры данных с массивом на тип компонента, а не только декларация data-oriented design. [MassArchetypeData.cpp:320](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassArchetypeData.cpp#L320).

**Вывод для нас:** hot-компоненты делать компактными и группировать по реальным проходам. Не обязательно разносить каждое поле Transform по отдельному компоненту: сначала выяснить, какие системы читают и изменяют поля вместе. В authoring Inspector можно показывать один блок Transform, даже если runtime layout иной.

### 2. Смена архетипа имеет реальную стоимость

`MoveEntityToAnotherArchetype` выделяет место в новом архетипе, переносит fragments и освобождает старое. Перенос копирует значения общих fragments для переносимых entities, инициализирует добавленные fragments и уничтожает исчезнувшие. [Миграция](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassArchetypeData.cpp#L775), [перенос данных](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassArchetypeData.cpp#L1759).

**Вывод для нас:** состояние, меняющееся каждый кадр, не стоит автоматически кодировать add/remove набора tags. Для частых переключений рассмотреть поле state или enable mask. Это уменьшает миграции, но добавляет стоимость фильтрации. Unity отдельно предоставляет enableable components именно для частых переключений без структурных изменений. [Unity Entities: enableable components](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-enableable-intro.html).

### 3. Запросы кешируют подходящие архетипы

`CacheArchetypes` проверяет версию набора архетипов, дополняет список подходящих и сохраняет mapping компонентов. При изменении requirements либо world сбрасывает кэш. Выполнение затем идёт по найденным archetypes/chunks. [Кэш запроса](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassEntityQuery.cpp#L138), [выполнение](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassEntityQuery.cpp#L368).

Flecs также различает долгоживущие cached queries и одноразовые uncached queries: выбирать следует по частоте повторного выполнения и стоимости поддержки кэша. [Flecs: Queries](https://www.flecs.dev/flecs/Queries.html).

**Вывод для Faset:** регистрировать требования систем заранее; EnTT views создавать по месту выполнения, не предполагая наличия Mass-подобной компиляции запросов. До параллельного запуска подготовить нужные component storage. Сложный динамический поиск в редакторе не должен навязывать ту же цену каждому объекту gameplay.

### 4. Структурные изменения откладываются и исполняются по явным правилам

`FMassCommandBuffer::Flush` группирует операции, стабильно сортирует группы и выполняет batch-команды. Порядок определяется типом операции; это не просто FIFO. Даже совместимость observer callbacks со временем удаления данных требует отдельного кода. [MassCommandBuffer.cpp:94](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassCommandBuffer.cpp#L94), [исполнение](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassCommandBuffer.cpp#L217).

**Принято для Faset:** runtime structural changes применять одним владельцем в начале следующего fixed tick после завершения прежних задач. Определить поведение `create → add → delete` в одном tick, повторного delete, конфликтных записей и событий удаления. Не копировать порядок Mass вслепую: это часть публичной семантики нашего runtime. В Unity структурные изменения также могут вызывать synchronization points; entity command buffers позволяют записать изменения для последующего воспроизведения. [Структурные изменения Unity](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-structural-changes.html), [Entity command buffers](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffers.html).

### 5. Handle не является вечной ссылкой на объект

Mass handle содержит index и serial; хранилище сравнивает serial с текущим при проверке, а при выдаче слота назначает новый. При этом `FMassEntityHandle::IsValid()` сам по себе только проверяет заполненность, не существование entity. [Handle](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/Mass/MassCore/Public/Mass/EntityHandle.h#L12), [проверка и выдача](https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/Source/Runtime/MassEntity/Private/MassEntityManagerStorage.cpp#L97).

**Вывод для нас:** runtime handle должен проверяться world; сериализуемый SceneEntityId должен быть отдельным типом. В разных worlds одинаковый локальный index не должен случайно обозначать один объект. Долгоживущие ссылки не должны хранить указатель на компонент, который переедет при structural change.

## Как сделать ECS удобным

### Автор работает со сценой, runtime — с оптимизированным представлением

В редакторе: `Enemy.scene`, иерархия, mesh, collider, health, script, overrides. При запуске compiler создаёт runtime entities и компоненты, сохраняя карту происхождения `SceneEntityId → RuntimeEntity[]`. Связь может быть один-ко-многим: один авторский персонаж содержит несколько runtime объектов. Обратное отображение тоже нужно для выбора объекта мышью и сообщений об ошибках.

Unity baking — полезный прецедент разделения человекочитаемой authoring-модели и оптимизированных runtime-данных; документация прямо предупреждает, что преобразование не обратимо. Faset не переносит runtime-изменения в авторскую сцену автоматически; возможное будущее GUI-действие такого переноса потребует отдельного контракта и не предоставляется через MCP. [Unity: baking overview](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/baking-overview.html).

Принятый Inspector показывает authoring-состояние, источник значения и sparse overrides вложенных шаблонов. Runtime существует в отдельном Player. Идея вкладки «Игра» и выборочного применения значений из раннего исследования не входит в начальный контракт; MCP runtime inspection/mutation исключён. Редакторский объект не становится неявной двусторонне синхронизируемой копией компонента.

### Два уровня программирования, одно состояние игры

Для простых сценариев — знакомый фасад: получить entity по типизированной ссылке, прочитать компонент, подписаться на событие, создать экземпляр сцены. Например, сценарий двери реагирует на взаимодействие и меняет целевой угол. Для массовой логики — типизированная system/query с `Read<T>` и `Write<T>`, обрабатывающая весь набор.

Фасад — представление над теми же данными и проверяемыми handles, а не отдельный объектный мир, который требуется синхронизировать. В hot loop система пишет компоненты напрямую в пределах выданного доступа. Создание и удаление entity идут через runtime command buffer. Editor tool, консоль автора и автоматизация используют authoring-транзакции с validation/undo для изменений авторских документов. MCP ограничен авторскими документами и сервисами редактора: он не читает и не меняет runtime world/session. Native debugger остаётся отдельным инструментом программирования. **Gameplay structural commands не объединяются с editor Undo.**

### Явное расписание без обязательного знакомства с scheduler

C++-поведения, а позднее Lua, используют `OnStart`, `FixedUpdate`, `Update`, `LateUpdate`, `OnDestroy`. `OnStart` выполняется после создания и до первого обновления; `OnDestroy` — при применении удаления до освобождения допустимых данных. Для систем доступны read/write declarations, dependencies и фазы до/после физики. Начальный scheduler последовательный; параллельное выполнение вводится после профилирования и проверки доступов.

Fixed tick по умолчанию 60 Гц, настраивается проектом. В начале tick применяются structural commands предыдущего tick; затем gameplay до физики, команды Box2D/Box3D, завершение шага, перенос transforms/events и реакции после физики. После фиксированных ticks идут `Update`, подготовка интерполированных presentation transforms, `LateUpdate` (камера и зависимые визуальные объекты), затем финализация render snapshot. `Update/LateUpdate` вызываются один раз за игровой кадр. Catch-up ограничен; начальный лимит четыре ticks за проход, избыточные целые интервалы отбрасываются с диагностикой. Render interpolation между завершёнными ticks не пишет результат обратно в физику; spawn/teleport сбрасывают историю.

EnTT registry не является целиком thread-safe. До распараллеливания создаются storage; чтение/запись компонентов и внешних ресурсов объявляется явно. `ENTT_USE_ATOMIC` не заменяет синхронизацию пользовательских данных. `organizer` может дать граф зависимостей, но scheduling остаётся обязанностью Faset. [EnTT: multithreading и organizer](https://github.com/skypjack/entt/wiki/Entity-Component-System).

Нужен Inspector системы: фаза, reads/writes, dependencies, сколько entities совпало, почему query пуста, время, выделения, последняя ошибка. Сообщение «не найден Velocity» полезнее молчаливого отсутствия движения. Bevy показывает, что типизированные параметры функций могут задавать доступ к данным, а порядок задаётся явными зависимостями; это хорошая идея интерфейса, даже при выборе другого языка. [Bevy ECS: systems/schedules](https://docs.rs/bevy_ecs/0.19.1/bevy_ecs/).

### Иерархия остаётся понятной

Parent/child нужны для организации сцены, трансформаций и ownership. Это не значит, что все они обязаны иметь одну семантику удаления. Нужно отдельно определить transform parent, owner сцены и attachment. Массовое удаление parent, перепривязка child с сохранением world transform и unload сцены должны иметь предсказуемые правила.

В runtime transform propagation можно делать специальным проходом в порядке зависимостей. Renderer и physics не обязаны обходить дерево ради каждого запроса. В EnTT иерархия оформляется собственными компонентами/индексом Faset; transform parent и ownership не обязаны совпадать. Контракт вложенных шаблонов хранится в authoring, а стоимость runtime traversal измеряется отдельно.

### События и изменения должны быть видимы

Разделить события домена (`Damage`, `DoorOpened`) и служебные lifecycle уведомления (`component added`). Доменные события лучше начать с явных очередей и фазы доставки. Цепь скрытых observers, меняющих друг друга рекурсивно, плохо объясняется и тестируется. Flecs прямо различает немедленное emit и enqueue в deferred mode. [Flecs: observers](https://www.flecs.dev/flecs/ObserversManual.html).

Change detection полезна для transform uploads и перестроения инструментов, но «был mutable-доступ» не всегда равно «значение изменилось». Предлагаю version/dirty markers с документированной семантикой и debug-кнопку «кто изменил». Не журналировать все значения в shipping build по умолчанию; подробную историю включать для выбранного объекта/системы.

## Что взять готовым и что написать самим

**История выбора от 17.09:** сравнивались **Flecs** (queries, relationships, phases/modules), **EnTT** (registry, views/groups, настройка storage) и, для варианта Rust, **bevy_ecs**. **Решение 18.09: C++ и EnTT.** Сильная сторона Flecs — готовая согласованная модель отношений и исполнения; Faset выбрал более узкую роль ECS и собственные authoring/metadata/API. Это архитектурный выбор, не рейтинг скорости. [Flecs design guide](https://www.flecs.dev/flecs/DesignWithFlecs.html), [EnTT repository](https://github.com/skypjack/entt), [bevy_ecs](https://docs.rs/bevy_ecs/0.19.1/bevy_ecs/).

**Самим стоит написать слой, который отличает наш движок:** schema metadata, scene compiler, stable IDs, runtime mapping, Inspector, удобный scripting façade, system debugger, authoring transactions и tools SDK. Не стоит первой задачей писать свой parallel archetype allocator, borrow checker и универсальный query language.

Если цель отдельно учебная — простой sparse-set ECS хорош как эксперимент. Но решать, нужен ли он в продукте, следует после сравнения с готовой библиотекой, с учётом tooling и жизненного цикла native/managed данных.

## Прототип, который проверит решение

1. **Обычная игра.** Создать сцену с игроком, дверью, несколькими врагами и UI. Простой script должен читаться без ручных archetype/chunk API.
2. **Массовая обработка.** 1 тыс., 10 тыс. и 100 тыс. entities с одной реальной системой движения/выбора целей. Сравнить простой packed-array baseline, выбранную ECS и удобный façade. Это параметры будущего теста, а не заявленные границы производительности.
3. **Изменение состава.** Варианты со стабильными компонентами и частыми spawn/despawn/add/remove. Проверить pointer/handle lifetime, очереди, latency и worst frame.
4. **Редактор.** Multi-edit, undo одного drag, duplicate вложенного шаблона, sparse overrides и одинаковая authoring-правка через GUI/MCP; отсутствие MCP-доступа к runtime. Инкрементальная пересборка должна давать семантически тот же результат, что и полная, без требования одинакового порядка сущностей в памяти.
5. **Расширение.** Отдельный пакет добавляет компонент Health, систему Damage, Inspector decoration и prefab preset без изменения ядра.

Измерять whole-frame p50/p95, allocations, bytes/component, размеры component pools и эффективность views, время structural playback, query count, render extraction и authoring→visible latency. Для UX — число действий, ошибок и обращений к документации при одинаковой задаче. Для параллельного выполнения — корректность порядка и отсутствие data races; сам факт запуска на нескольких потоках не критерий успеха.

**Критерий готовности интеграции EnTT:** она даёт полезную производительность и композицию, сохраняя простой authoring workflow. Если пользователь движка должен изучить устройство allocator, чтобы добавить фонарь или дверь, интерфейс ещё не готов.
