import { Stack, Row, Grid, H1, H2, H3, Text, Pill, Button, Table, Divider, Card, CardHeader, CardBody, CollapsibleSection, useHostTheme, useCanvasState, useCanvasAction } from "cursor/canvas";

const root = "docs/studies/";
const ue = "../UnrealEngine/Engine/";
const reports = [
  { name: "Выводы и устройство нашего движка", path: "14-engine-blueprint.md" },
  { name: "Unreal · графика", path: "07-unreal-graphics-source-study.md" },
  { name: "Godot · UX", path: "08-godot-ux-source-study.md" },
  { name: "Unity · UX и расширения", path: "09-unity-ux-extensibility-study.md" },
  { name: "Blender · инструменты", path: "10-blender-editor-patterns.md" },
  { name: "ECS · применение и удобство", path: "11-ecs-and-ergonomics.md" },
  { name: "MCP и Blender integration", path: "12-mcp-and-blender-integration.md" },
  { name: "Сборка игры и выбор стека", path: "13-build-pipeline-and-stack.md" },
  { name: "15 · GPU renderer: проходы, барьеры и история", path: "15-renderer-implementation-notes.md" },
  { name: "16 · C++: компоненты, метаданные и языковые модули", path: "16-native-gameplay-and-metadata.md" },
  { name: "17 · Asset pipeline и Blender roundtrip", path: "17-asset-pipeline-and-blender-roundtrip.md" },
  { name: "18 · Сборка, cook и доставка игры", path: "18-build-cook-and-delivery.md" },
];

const graphics = [
  { id: "hzb", name: "GPU Scene + two-pass HZB", stage: "P2 · после MVP", benefit: "GPU сам выбирает видимые объекты и повторно проверяет ранее скрытые.", fact: "Main cull/raster использует temporal visibility; текущая глубина строит новый HZB для post pass. Постоянные таблицы instances обновляются частично.", start: "Resident instance tables → frustum culling → indirect draw → HZB → очередь повторной проверки. Обычный hardware raster достаточно.", risk: "Консервативные bounds, previous transforms, near plane, reverse-Z и переполнение очередей. В открытой сцене дополнительный проход может проиграть.", check: "Закрытая и открытая сцены, быстрый pan, дверь и teleport. Сравнить depth/ID с HZB-off и полное GPU время.", source: "Source/Runtime/Renderer/Private/Nanite/NaniteCullRaster.cpp", line: 7008 },
  { id: "visibility", name: "Visibility buffer + material bins", stage: "Дальнейшее исследование", benefit: "Разделяет обработку геометрии и дорогой material shading.", fact: "Nanite сохраняет surface ID/depth, восстанавливает атрибуты и группирует shading work. Материалам с derivatives нужны корректные quads.", start: "Hardware depth + integer ID target, фиксированные PBR-классы, затем compute resolve и material bins.", risk: "UV derivatives, mip selection и деформация. Binning и вспомогательные buffers имеют стоимость; не каждый материал выигрывает.", check: "Сравнение geometry + binning + resolve с G-buffer при росте overdraw и числа классов материалов.", source: "Shaders/Private/Nanite/NaniteShadeBinning.usf", line: 959 },
  { id: "geometry", name: "Кластерный LOD и streaming", stage: "P4 · после обычного mesh LOD", benefit: "Детализация выбирается по экранной ошибке, память ограничивается пулом страниц.", fact: "Согласованные группы упрощаются offline; runtime держит drawable coarse fallback и учитывает зависимости страниц при eviction.", start: "Сначала обычный mesh LOD с hysteresis. Отдельно исследовать resident cluster LOD с корректным cut; затем coarse root, pool, page table, feedback и budgeted upload.", risk: "Независимый LOD соседних meshlets даёт щели. LRU без зависимостей ломает decoding. Software raster — отдельный поздний этап.", check: "Контент больше пула, задержки I/O, поворот камеры: нет дыр, ограничена VRAM, видимы page churn и LOD error.", source: "Source/Runtime/Engine/Private/Rendering/NaniteStreamingManager.cpp", line: 2579 },
  { id: "lumen", name: "Lumen: hybrid tracing + caches", stage: "P4 · исследование GI", benefit: "Сокращает дорогие world traces и повторную оценку освещения.", fact: "Экранные hits проверяются; незавершённые rays уплотняются и передаются HWRT или software-пути. Probes/cache получают бюджет обновлений.", start: "Сначала baked indirect light и reflection probes. Затем screen-space дополнения с fallback; world tracer, diffuse probes и budgeted cache — отдельное исследование.", risk: "Screen/world mismatch, stale lighting, утечки через стены, teleport. HWRT и mesh SDF — альтернативы, не обязательная цепочка.", check: "Тонкая стена, свет за камерой, дверь и смена emissive. Замерить переходы backend и кадры до сходимости.", source: "Source/Runtime/Renderer/Private/Lumen/LumenScreenProbeTracing.cpp", line: 678 },
  { id: "mega", name: "MegaLights: выборка света", stage: "После clustered lights и temporal", benefit: "Ограничивает число дорогих shadow samples и оценивает свет стохастически.", fact: "Weighted sampling и history visibility guiding выбирают lights; confidence-aware temporal/spatial denoising восстанавливает сигнал.", start: "Clustered light lists, небольшой sample budget, visibility backend, корректная компенсация вероятностей, rejection и denoiser.", risk: "Обход кандидатов остаётся. Фиксированный бюджет лучей не равен постоянной стоимости всей системы; при плотном свете растёт шум.", check: "10/100/1000 lights, включение яркого света, glossy floor. Отдельно считать selection, tracing, denoise и temporal error.", source: "Shaders/Private/MegaLights/MegaLightsSampling.usf", line: 409 },
  { id: "vsm", name: "Virtual Shadow Maps", stage: "P4 · после shadow atlas", benefit: "Обновляются востребованные теневые страницы, статика переиспользуется.", fact: "GPU отмечает pages, выделяет physical backing и отдельно учитывает static/dynamic invalidation. Dirty flags сохраняются вне видимого кадра.", start: "Один directional light, page table, фиксированный atlas, dirty list и обычный depth raster. Затем clipmaps.", risk: "Поворот солнца, деформации и маленький пул могут уничтожить пользу cache. Нужна диагностика invalidations.", check: "Прогрев, движущийся объект вне кадра, возврат камеры, teleport и pool exhaustion; сравнить с full refresh.", source: "Shaders/Private/VirtualShadowMaps/VirtualShadowMapPhysicalPageManagement.usf", line: 280 },
  { id: "tsr", name: "TSR: доверие к истории", stage: "P3 · после MVP", benefit: "Восстанавливает детализацию и подавляет aliasing при меньшем render resolution.", fact: "Depth-aware velocity dilation, disocclusion/rejection, clamp и validity дополняют историю цвета. Мерцание анализируется отдельно.", start: "Правильные velocity/jitter, reprojection, depth rejection, neighborhood clamp, validity и spatial fallback.", risk: "Ghosting против стабильности. Нельзя исправить неверные motion vectors одним фильтром. TSR не заменяет lighting denoiser.", check: "Провода, pan, раскрытие фона, particles, emissive, exposure и camera cut. Проверять последовательности кадров.", source: "Shaders/Private/TemporalSuperResolution/TSRUpdateHistory.usf", line: 1249 },
  { id: "substrate", name: "Substrate: материал по бюджету", stage: "Дальнейшее исследование", benefit: "Простые области используют дешёвый shading, сложность материалов ограничена.", fact: "Компилятор упрощает граф под bytes/closures budget; GPU классифицирует тайлы simple/single/complex и строит indirect lists.", start: "Два класса PBR/clear coat, feature flags и simple/complex tiles. Небольшое число closures вместо произвольного графа.", risk: "Один complex pixel может усложнить тайл; scattered materials уменьшают пользу. Compile-time budget не равен runtime timing.", check: "Simple-only, локальные complex и шахматная смесь: classification + lighting целиком, качество и память.", source: "Shaders/Private/Substrate/SubstrateMaterialClassification.usf", line: 358 },
  { id: "glints", name: "Glints: фильтруемые микроблики", stage: "После MVP · BRDF эксперимент", benefit: "Характерный вид краски с частицами, блестящих покрытий и снега.", fact: "Распределение микрофасеток фильтруется с учётом UV footprint и LOD; интеграция содержит переход к GGX.", start: "Один opaque материал, tangent basis, derivatives, LUT и один свет. Система слоёв Substrate не обязательна.", risk: "Фильтрация математически сложнее случайных ярких точек. Устойчивость видна только при движении и изменении масштаба.", check: "Движение света/камеры, roughness/density sweep и supersampled reference; энергия, мерцание, добавочное GPU время.", source: "Shaders/Private/Substrate/Glint/GlintThirdParty.ush", line: 425 },
  { id: "rdg", name: "Render graph", stage: "M2 · минимальный Render Graph", benefit: "Описывает зависимости проходов, барьеры и время жизни временных ресурсов.", fact: "RDG удаляет ненужные проходы, компилирует transitions и учитывает overlap GPU очередей при работе с памятью.", start: "MVP: одна очередь, явные reads/writes, barriers, external outputs и lifetime. Позже pass culling, resource reuse, aliasing и async compute.", risk: "CPU-порядок не гарантирует завершение GPU. Persistent history и frames in flight требуют отдельного lifetime.", check: "MVP: корректные transitions и deferred destruction, validation чиста, видны CPU/GPU cost и bytes. После pass culling — удаление ненужных предшественников.", source: "Source/Runtime/RenderCore/Private/RenderGraphBuilder.cpp", line: 1327 },
];

const decisions = [
  ["Платформа и графика", "Linux/Windows, 2D/3D. SDL3; собственные Vulkan 1.3 backend и Render Graph. Slang → SPIR-V, совместимый HLSL. Базовый путь без обязательного RT."],
  ["Ядро и gameplay", "C++, EnTT; Box2D для 2D и Box3D (erincatto/box3d) для 3D. Gameplay — static library в dev/release Player. После MVP обязательно добавить Lua; каждой игре разрешено обходиться без Lua."],
  ["Редактор", "Собственный retained UI на C++: декларативные layout/styles, тёмная тема по умолчанию. Dear ImGui — только инструменты отладки. Inspector, Undo и MCP используют один authoring service."],
  ["Данные", "JSON authoring со stable IDs; бинарные cooked assets. Вложенные scene templates со sparse overrides. Generated outputs не заменяют вручную настроенные экземпляры."],
  ["Процессы и расширения", "Player — отдельный процесс и окно. C++: stop → build → restart. Editor plugins — DLL/SO под точный SDK, смена с перезапуском; schema export — helper process."],
  ["Сборка", "CMake + Ninja; Clang на Linux, clang-cl + Windows SDK на Windows. Code compile, shader compile, cook и package — отдельные стадии с логами и отменой."],
];

const steps = [
  { name: "M0 · Воспроизводимый фундамент", task: "C++ targets Core/Runtime/Editor/Player/SchemaExporter, CMake/Ninja, Clang/clang-cl, dev/release, dependency pins и CI Linux/Windows.", pass: "Чистая сборка и запуск на обеих ОС; Runtime/Player не зависят от Editor/MCP, версии и notices записаны." },
  { name: "M1 · Документы, метаданные и команды", task: "Stable IDs, типизированные C++ schemas, JSON, AuthoringService, revisions, transactions, Undo/Redo и миграции без GUI.", pass: "Roundtrip сохраняет смысл/ID; неверная транзакция не применяется частично; старый revision даёт конфликт." },
  { name: "M2 · Платформа и базовая графика", task: "SDL3, Vulkan 1.3, Render Graph на одной очереди, Slang/reflection; direct draws/CPU frustum, 2D layers/sprites, 3D simple PBR и обычные тени.", pass: "Работают resize/minimize и безопасная замена shader; validation без ошибок. Player получает SPIR-V/metadata без compiler." },
  { name: "M3 · Runtime, gameplay, физика и Player", task: "EnTT, gameplay static library, schema helper, lifecycle/tick и Box2D/Box3D. Player — отдельные процесс/окно, C++ stop/build/restart.", pass: "C++-компонент виден в Inspector, влияет на объект/физику; Stop сохраняет authoring-сцену. Ошибка build не выдаёт старую сборку за новую; MCP в Player нет." },
  { name: "M4 · Собственная UI-основа", task: "Retained tree, input/focus, текст/DPI, declarative layout/styles dark-first, widgets и минимальный docking. ImGui только для debug.", pass: "Клавиатура, кириллица/IME и DPI проверены; layout/styles обновляются без C++ rebuild; один drag — один Undo." },
  { name: "M5 · Ресурсы и Blender", task: "AssetId, dependency/cache/artifact manifests, обычный GLB без addon; optional Blender addon для стабильных IDs и согласованного export generation.", pass: "Reimport сохраняет компоненты/overrides; rename со stable ID сохраняет связь, удаление даёт конфликт. Ошибка/отмена сохраняет последний рабочий artifact." },
  { name: "M6 · Редактор и повторно используемые сцены", task: "Scene Tree, Inspector, Asset Browser, viewports и Console; nested templates, sparse overrides, origin/Revert, duplicate с remap внутренних ссылок.", pass: "Без ручного JSON создать два экземпляра, изменить один, обновить источник, отменить и переоткрыть без потери identity. Apply/variants позже." },
  { name: "M7 · MCP редактора и расширения", task: "Authoring/query/schema, transactions, jobs/import/build, Play/Stop и editor diagnostics. Headless authoring/build; editor DLL/SO под exact SDK с restart.", pass: "Ручные и MCP-операции эквивалентны. Нет runtime-entity read/write через MCP; transport отсутствует в Player/SchemaExporter. Неизвестные данные пакета сохраняются." },
  { name: "M8 · Экспорт законченных примеров", task: "BuildService: validate → C++/schema → Slang/cook → package. Обе небольшие 2D/3D-игры с C++ gameplay и понятным управлением.", pass: "Все четыре сочетания 2D/3D × Linux/Windows запускаются без Editor, MCP, Blender и исходного дерева; runtime-зависимости документированы." },
  { name: "M9 · Приёмка MVP", task: "Новая установка, authoring руками/MCP, conflict/Undo/recovery, reimport и ошибка Player; измерить startup/edit/build/run, CPU/GPU и память.", pass: "Устранены блокеры UX/форматов/export, записаны оборудование, результаты и ограничения. Только после проверок — MVP tag." },
  { name: "P1 · Lua и скорость итераций", task: "Обязательный Lua-модуль после MVP, optional per game: API/handles/schemas, diagnostics и lifecycle reload. Улучшать измеренный build/edit цикл.", pass: "C++ и Lua используют одни данные/фазы; C++-only export без Lua runtime. P1 не обязан ждать завершения графических веток." },
  { name: "P2 · GPU-driven visibility и LOD", task: "GPU instance IDs/frustum → fixed indirect batches → current HZB/debug view → two-pass occlusion → обычный mesh LOD/hysteresis. Direct reference сохраняется.", pass: "Нет дыр при открытии двери/исчезновении заслона/cut; проверены пустые/полные bins, удаление, teleport/resize. Полный кадр измерен на открытой и закрытой сценах." },
  { name: "P3 · Освещение, тени и temporal reconstruction", task: "Local lights, clustered/Forward+ по необходимости, cascaded sun shadows и local atlas; затем motion vectors/jitter/rejection → TAA → upscaling.", pass: "Shadow views имеют собственную видимость/бюджеты. Проверены disocclusion, тонкая геометрия, движение, resize/cut; видны ghosting и полная цена эффекта." },
  { name: "P4 · Непрямой свет и продвинутая геометрия", task: "Baked indirect/reflection probes → исследование динамической GI. Cluster LOD, streaming, visibility buffer, VSM/VT — самостоятельные проекты после профилирования.", pass: "Проверены свет вне экрана, заслоны, утечки и бюджеты. RT дополнительный; эквивалент Nanite/Lumen не обещается, порядок веток зависит от игры." },
  { name: "P5 · Инструменты создания игр", task: "Animation/skinning, игровой UI/audio, tilemaps/2D tools, material editor, variants/Apply и task scheduler после профилирования.", pass: "Объём выбирается по нуждам игр; новые системы сохраняют общий authoring-контракт и согласованные фазы. Это не обязательства MVP." },
  { name: "P6 · Доставка и экосистема", task: "Проверяемый native ABI/SDK, migrations, accessibility/localization, несколько окон, embedded Player, Blender live link; другие backend/targets отдельно.", pass: "Совместимость подтверждается для выбранных сценариев. Multiplayer/rollback требуют отдельного контракта; MCP остаётся редакторской интеграцией." },
];

function Architecture() {
  const t = useHostTheme();
  const boxes = [
    [20, 15, 245, "Ручной C++ редактор", "Retained UI · Inspector · gizmos"],
    [290, 15, 245, "MCP / CLI редактора", "Authoring · import · build · Play/Stop"],
    [560, 15, 245, "Официальный Blender", "Обычный GLB + optional addon"],
    [20, 120, 515, "AuthoringService", "Schema · validation · revision · transaction · Undo"],
    [560, 120, 245, "Asset pipeline", "Source + recipe → cooked artifacts"],
    [20, 225, 245, "JSON scene templates", "Stable IDs · sparse overrides"],
    [290, 225, 245, "Scene compiler", "Authoring → binary runtime data"],
    [560, 225, 245, "Player · отдельный процесс", "EnTT · C++ · physics · без MCP"],
    [560, 330, 245, "Vulkan 1.3 renderer", "MVP: direct draws + CPU frustum"],
  ] as const;
  return <div style={{ overflowX: "auto" }}><svg viewBox="0 0 825 425" role="img" aria-label="Принятая архитектура Faset; реализация запланирована" style={{ width: "100%", minWidth: 650 }}>
    <defs><marker id="engine-arrow" markerWidth="7" markerHeight="7" refX="6" refY="3.5" orient="auto"><path d="M0 0 L7 3.5 L0 7" fill={t.text.tertiary}/></marker></defs>
    <g fill="none" stroke={t.stroke.primary} strokeWidth="1.5" markerEnd="url(#engine-arrow)">
      <path d="M140 80 V118 M410 80 V118 M685 80 V118 M560 155 H537 M140 185 V223 M265 258 H288 M535 258 H558 M685 185 V223 M685 290 V328"/>
    </g>
    {boxes.map(([x,y,w,title,sub]) => <g key={title}>
      <rect x={x} y={y} width={w} height={65} rx={5} fill={title === "AuthoringService" ? t.fill.secondary : t.bg.elevated} stroke={t.stroke.secondary}/>
      <text x={x + 13} y={y + 25} fontSize="13" fontWeight="600" fill={title === "AuthoringService" ? t.accent.primary : t.text.primary}>{title}</text>
      <text x={x + 13} y={y + 47} fontSize="10.5" fill={t.text.secondary}>{sub}</text>
    </g>)}
    <text x="20" y="350" fontSize="12" fill={t.text.secondary}>MCP читает и меняет только authoring-документы.</text>
    <text x="20" y="373" fontSize="12" fill={t.text.secondary}>Нет доступа к runtime worlds / игровым сессиям.</text>
    <text x="20" y="413" fontSize="11" fill={t.text.tertiary}>Запланированный поток данных; Play/Stop — управление дочерним процессом со стороны Editor.</text>
  </svg></div>;
}

export default function EngineResearch() {
  const t = useHostTheme();
  const dispatch = useCanvasAction();
  const [tab, setTab] = useCanvasState("engine-research-tab", "Решение");
  const [graphicsId, setGraphicsId] = useCanvasState("engine-graphics-id", "rdg");
  const [client, setClient] = useCanvasState("engine-client", "Руками");
  const selected = graphics.find(g => g.id === graphicsId) ?? graphics[0];
  const open = (path: string, line?: number) => dispatch({ type: "openFile", path, ...(line ? { selection: { startLineNumber: line, startColumn: 1, endLineNumber: line, endColumn: 1 } } : {}) });
  const sourceUrl = "https://github.com/EpicGames/UnrealEngine/blob/16d75d84714512edfb744e1fd0a59e9c74d57873/Engine/" + selected.source + "#L" + selected.line;
  return <Stack gap={20} style={{ maxWidth: 1120, margin: "0 auto", padding: 24, color: t.text.primary }}>
    <Row justify="space-between" align="start" wrap>
      <Stack gap={5}><Text size="small" tone="tertiary">FASET ENGINE · РЕШЕНИЯ 18.09.2026</Text><H1>Архитектура, MVP и развитие графики</H1><Text tone="secondary">Linux + Windows · 2D / 3D · C++ Editor · Blender</Text></Stack>
      <Row gap={8} wrap><Button onClick={() => open("README.md")}>О проекте</Button><Button onClick={() => open("PLAN.md")}>План до / после MVP</Button><Button onClick={() => open("docs/ARCHITECTURE.md")}>Архитектура</Button></Row>
    </Row>
    <Card><CardBody><Text><strong>Архитектура принята; идёт приёмка реализации MVP.</strong> Эта карта — работающий просмотрщик исследования, не редактор Faset. Результаты сборок, проверок и измерений опубликованы в docs/IMPLEMENTATION.md и docs/validation; исследовательские графические направления не считаются готовыми функциями. MCP предусмотрен строго в Editor; Player не содержит MCP и не предоставляет ему runtime worlds или игровые сессии.</Text></CardBody></Card>
    <Row gap={7} wrap>{["Решение", "Графика", "ECS и данные", "MCP и Blender", "Стек и экспорт", "Прототипы", "Источники"].map(name => <span key={name}><Pill active={tab === name} onClick={() => setTab(name)}>{name}</Pill></span>)}</Row>
    <Divider/>

    {tab === "Решение" && <Stack gap={22}>
      <H2>Принятые решения</H2>
      <Grid columns="repeat(auto-fit, minmax(280px, 1fr))" gap={24}>{decisions.map(([name, description]) => <Stack gap={8} key={name}><H3>{name}</H3><Text>{description}</Text></Stack>)}</Grid>
      <H2>Зачем изучались другие движки</H2>
      <Table headers={["Источник", "Механизм", "Применение в Faset"]} rows={[
        ["Godot", "Scene/Resource, overrides, короткий Play loop", "Scene templates, shared property/Undo слой"],
        ["Unity", "Serialized edits, tools, binding, plugin boundaries", "Multi-edit Inspector и явные границы editor plugins"],
        ["Unreal", "GPU scheduling, budgets, temporal validity", "Минимальный graph в MVP; GPU visibility и сложная графика позже"],
        ["Blender", "Operator registry, modal preview, отмена", "Общие authoring-команды для меню, hotkey и MCP редактора"],
      ]}/>
      <Card><CardHeader>Запланированный пользовательский контракт</CardHeader><CardBody><Stack gap={8}><H3>Понятно, откуда пришло значение и что изменится</H3><Text>Shared/local asset, наследование template, sparse override, preview и Undo видны в редакторе. Диагностика импорта и сборки объясняет входы, ошибки и результат. Вложенность сцен не требует знать внутреннее устройство ECS.</Text></Stack></CardBody></Card>
      <Button onClick={() => open(root + reports[0].path)}>Обоснование архитектуры по исходникам</Button>
    </Stack>}

    {tab === "Графика" && <Stack gap={18}>
      <H2>В MVP — direct renderer; сложные механизмы после него</H2>
      <Text>Vulkan 1.3 + собственный Render Graph, CPU frustum, sprites/layers/HUD для 2D и static meshes/simple PBR/обычные тени для 3D. Далее: GPU frustum/indirect → HZB → обычный LOD → расширение света/теней → temporal → GI. Карточки ниже сохраняют результаты изучения UE, а не список уже реализованных функций.</Text>
      <Row gap={6} wrap>{graphics.map(g => <span key={g.id}><Pill active={g.id === selected.id} onClick={() => setGraphicsId(g.id)}>{g.name}</Pill></span>)}</Row>
      <Row justify="space-between" wrap><H2>{selected.name}</H2><Pill>{selected.stage}</Pill></Row>
      <Text weight="bold">{selected.benefit}</Text>
      <Grid columns="repeat(auto-fit, minmax(300px, 1fr))" gap={24}>
        <Stack gap={8}><H3>Подтверждено кодом UE</H3><Text>{selected.fact}</Text><a href={sourceUrl} target="_blank" rel="noreferrer">Upstream · строка {selected.line}</a><Button onClick={() => open(ue + selected.source, selected.line)}>Опциональный локальный checkout</Button><Text size="small" tone="tertiary">{selected.source} · Для upstream UE требуется доступ Epic.</Text></Stack>
        <Stack gap={8}><H3>Планируемый минимальный эксперимент</H3><Text>{selected.start}</Text><H3>Ограничение</H3><Text>{selected.risk}</Text></Stack>
      </Grid>
      <Divider/><H3>Будущий критерий проверки</H3><Text>{selected.check}</Text>
      <Row gap={8} wrap><Button onClick={() => open(root + reports[1].path)}>Полное исследование UE</Button><Button onClick={() => open(root + reports[8].path)}>Проходы, барьеры и история GPU renderer</Button><Button onClick={() => open("PLAN.md")}>Граница MVP и порядок этапов</Button></Row>
      <Text size="small" tone="tertiary">UE 5.8.2 · commit 16d75d847145 · статический разбор C++ / HLSL. Полное воспроизведение Nanite/Lumen не обещается; минимальные GPU и driver matrix ещё требуют проверки.</Text>
    </Stack>}

    {tab === "ECS и данные" && <Stack gap={20}>
      <H2>EnTT в runtime; сцены и метаданные в authoring</H2>
      <Architecture/>
      <Grid columns="repeat(auto-fit, minmax(290px, 1fr))" gap={24}>
        <Stack gap={8}><H3>Authoring — принято</H3><Text>JSON, stable SceneEntityId и asset IDs, nested scene templates, sparse overrides, revision, Undo и save. Generated imports отделены от пользовательских настроек. Схема компонентов экспортируется helper process.</Text></Stack>
        <Stack gap={8}><H3>Runtime — запланировано</H3><Text>EnTT, generational handles, компактные компоненты, queries и structural commands. Scene compiler выдаёт бинарные cooked данные; authoring ID не равен временной позиции entity в ECS. UI/Undo/GPU allocator не обязаны храниться в ECS.</Text></Stack>
      </Grid>
      <Table headers={["Операция", "Контракт", "Граница"]} rows={[
        ["Изменить template в Inspector/MCP", "Authoring transaction + revision + Undo", "Меняет документ; не редактирует запущенную игру"],
        ["Обновить Position в C++ системе", "Runtime-компоненты по контракту scheduler", "Текущая фаза / render extraction внутри Player"],
        ["Spawn/despawn во время query", "Runtime structural command buffer", "Документированный sync point; не editor Undo"],
        ["Изменить C++ gameplay", "Gameplay static library → Player link", "Stop → build → restart; без обещания C++ hot reload"],
      ]}/>
      <Text>Lua обязательно появится после MVP как отдельный модуль с ограниченным API. Использование каждой игрой необязательно; произвольный C++ не получает bindings автоматически.</Text>
      <Row gap={8} wrap><Button onClick={() => open(root + reports[5].path)}>Исследование ECS</Button><Button onClick={() => open(root + reports[9].path)}>C++ и метаданные</Button></Row>
    </Stack>}

    {tab === "MCP и Blender" && <Stack gap={20}>
      <H2>MCP — только редактор и его authoring-сервисы</H2>
      <Text>Разрешённый планируемый охват: документы и транзакции, validation, import/build jobs, Play/Stop и логи Editor; headless — для тех же доступных без UI операций. Play запускает отдельный Player, Stop завершает его. Ни runtime world inspection/mutation, ни API игровых сессий, ни MCP server в Player не предусмотрены.</Text>
      <Row gap={8}>{["Руками", "Через MCP"].map(v => <span key={v}><Pill active={client === v} onClick={() => setClient(v)}>{v}</Pill></span>)}</Row>
      <Grid columns="repeat(auto-fit, minmax(290px, 1fr))" gap={24}>
        <Stack gap={8}><H3>{client === "Руками" ? "Переместить пять источников в документе" : "Сместить пять lights authoring batch-операцией"}</H3><Text>{client === "Руками" ? "Выбрать объекты → начать drag → видеть preview → отпустить мышь. Один commit хранит исходные и конечные transforms." : "Прочитать IDs/revision → отправить batch → получить новую revision и affected IDs. Конфликт не должен оставлять частичные правки; повтор проверяется по idempotency key и payload в пределах срока журнала."}</Text></Stack>
        <Stack gap={8}><H3>Общий запланированный результат</H3><Text>Один валидный authoring-документ, одно Undo и обновлённые panels. MCP не получает доступа к запущенным runtime entities; изменения входят в следующий подготовленный запуск игры.</Text></Stack>
      </Grid>
      <Divider/><H2>Официальный Blender; addon необязателен</H2>
      <Table headers={["Этап", "Контракт", "Критерий"]} rows={[
        ["Обычный GLB import", "Работает без addon и особой сборки Blender", "Units, axes, transforms и поддерживаемый material profile"],
        ["Optional addon / manifest", "Stable source/subasset IDs для надёжного matching", "Переименование и перестановка не меняют identity; имён недостаточно"],
        ["Import", "Recipe, dependencies, versioned derived artifacts", "Ошибка сохраняет последний валидный результат"],
        ["Scene instances / reimport", "References + sparse local overrides отдельно от cache", "Удалённые/неоднозначные outputs видны; нет молчаливой подмены"],
        ["Game build", "Готовые бинарные runtime assets", "Blender, addon и Editor не нужны Player"],
      ]}/>
      <Text>Live link — возможное дальнейшее развитие после надёжного файлового roundtrip; plain GLB не даёт автоматической гарантии matching любых изменённых subassets.</Text>
      <Row gap={8} wrap><Button onClick={() => open(root + reports[6].path)}>MCP и Blender: границы</Button><Button onClick={() => open(root + reports[10].path)}>Импорт, UID и roundtrip</Button></Row>
    </Stack>}

    {tab === "Стек и экспорт" && <Stack gap={20}>
      <H2>Принятый стек; интеграция запланирована</H2>
      <Table headers={["Область", "Выбор", "Граница"]} rows={[
        ["Platform / runtime", "SDL3 · C++ · EnTT · Box2D/Box3D", "Linux + Windows desktop, 2D/3D"],
        ["Renderer", "Собственный Vulkan 1.3 backend + Render Graph", "База без обязательного RT; features/formats/limits проверяются"],
        ["Shaders", "Slang → SPIR-V, совместимый HLSL", "Один pinned compiler в editor/cook; .spv + Faset metadata в Player"],
        ["Editor UI", "Собственный retained C++ UI, declarative layout/styles", "Dark-first; Dear ImGui только debug"],
        ["Native build", "CMake + Ninja; Clang / clang-cl + Windows SDK", "Сборка и запуск проверяются на каждой целевой ОС"],
        ["Gameplay", "Static library, dev/release Player", "Отдельные процесс/окно; C++ stop/build/restart"],
        ["Editor plugins", "DLL/SO под точный SDK", "Restart при смене; schema export helper process"],
        ["Lua после MVP", "Обязательный этап развития, optional per game", "C++-игра сможет собираться без Lua runtime"],
      ]}/>
      <Divider/><H2>Четыре разные стадии сборки</H2>
      <Table headers={["Стадия", "Результат", "Входы для invalidation"]} rows={[
        ["Compile code", "Gameplay static library + linked dev/release Player", "Код, flags, dependencies, toolchain"],
        ["Compile shaders", "SPIR-V + reflection metadata", "Shader/includes, permutations, compiler version, profile"],
        ["Cook assets", "Binary mesh/texture/scene data", "Source, recipe, dependencies, target profile"],
        ["Package + validate", "Автономный каталог игры + manifest", "Состав и версии готовых outputs"],
      ]}/>
      <Text>UI, CLI и MCP редактора запускают один BuildRequest. Компиляторы работают в дочерних процессах; staging публикуется после проверки. Планируемый контракт отмены сохраняет последнюю успешную сборку; cache корректность и повторяемость требуют тестов, а не только hash-ключей.</Text>
      <Text>Windows и Linux сборки проверяются в своих окружениях. Player не включает Editor/MCP, shader compiler или editor plugins. Драйвер всё равно создаёт GPU pipelines; shader hot reload требует собственной проверки bindings/layout и безопасной замены ресурсов.</Text>
      <Text size="small" tone="secondary">Версии закреплены в dependencies.lock.json и документации toolchain; фактически проверенные GPU/driver окружения указаны в docs/validation. C#/.NET и Rust + wgpu остаются историей сравнения вариантов, не параллельными реализациями Faset.</Text>
      <Row gap={8} wrap><Button onClick={() => open(root + reports[7].path)}>Исследование сборки и вариантов стека</Button><Button onClick={() => open(root + reports[11].path)}>Build service, cache и delivery</Button></Row>
    </Stack>}

    {tab === "Прототипы" && <Stack gap={18}>
      <H2>M0–M9 / P1–P6 · все этапы запланированы</H2>
      <Text>Этапы синхронизированы с PLAN.md: M0–M9 составляют MVP; P1–P6 идут после него. P1–P3 могут развиваться параллельно, P4 требует зрелого renderer. Обе демки и доставка на двух ОС обязательны для MVP.</Text>
      {steps.map((s, i) => <div key={s.name}><CollapsibleSection title={s.name} defaultOpen={i === 0}><Stack gap={8}><Text>{s.task}</Text><Text weight="bold">Будущий критерий приёмки</Text><Text>{s.pass}</Text></Stack></CollapsibleSection></div>)}
      <Divider/><H3>Общие измерения</H3><Text>Время от правки до результата, cold/warm Play, build/import latency, ошибки UX; CPU/GPU времена, память и качество последовательностей кадров. Каждый эффект сравнивается с воспроизводимым baseline. Даты и ускорения пока не обещаются.</Text>
      <Button onClick={() => open("PLAN.md")}>Полный PLAN.md с критериями этапов</Button>
    </Stack>}

    {tab === "Источники" && <Stack gap={18}>
      <H2>Исследовательские snapshots и границы доступа</H2>
      <Table headers={["Репозиторий", "Версия / commit", "Объём доступа"]} rows={[
        ["UnrealEngine", "5.8.2 · 16d75d847145", "Выбранные локальные C++/shaders; upstream требует доступа Epic"],
        ["godot", "4.8.0 dev · 9c776068d6ed", "Source checkout, shallow Git history"],
        ["UnityCsReference", "6000.7.0a6 · 6b50e5544f6e", "C# reference source; native internals не включены"],
        ["blender-source", "5.3.0 alpha · 28d47268bddc", "Shallow/sparse; выбранные редакторские подсистемы"],
      ]}/>
      <Text tone="secondary">Карта и отчёты открываются без локальных копий движков. Optional source viewer использует sibling checkout; исходники чужих движков не входят в Faset. Dev/alpha snapshots — материалы исследования, не выбранные production-зависимости.</Text>
      <Row gap={8} wrap><Button onClick={() => open("README.md")}>README</Button><Button onClick={() => open("PLAN.md")}>PLAN</Button><Button onClick={() => open("docs/ARCHITECTURE.md")}>Архитектура</Button><Button onClick={() => open(root + "source-manifest.json")}>Полные source SHA и охват</Button></Row>
      <Stack gap={7}>{reports.map(r => <div key={r.path}><Row><Button onClick={() => open(root + r.path)}>{r.name}</Button></Row></div>)}</Stack>
      <Text size="small" tone="tertiary">Прочитаны выбранные тела функций и официальные документы. Движки не собирались; UX-сравнение и GPU-бенчмарки не выполнены. Принятые решения не означают готовую реализацию.</Text>
    </Stack>}
  </Stack>;
}
