# Зависимости, инструменты и независимость

Обновлено 18.09.2026. Здесь перечислены **выбранные направления интеграции**, а не уже подключённые библиотеки движка. Точные версии закрепляются на [M0](../PLAN.md); commits [исследовательского манифеста](studies/source-manifest.json) не заменяют dependency lock.

## Runtime и инструменты Faset

- **EnTT:** runtime ECS, за публичным API Faset. [MIT](https://github.com/skypjack/entt/blob/master/LICENSE).
- **SDL3:** окна, ввод и платформенные службы; Vulkan renderer остаётся собственным. [zlib](https://www.libsdl.org/license.php).
- **Box2D / Box3D:** независимые физические миры. [Box2D MIT](https://github.com/erincatto/box2d/blob/main/LICENSE), [Box3D MIT](https://github.com/erincatto/box3d/blob/main/LICENSE).
- **Slang:** компилятор шейдеров для редактора/cook, SPIR-V и собственные метаданные для Player. [Apache-2.0 WITH LLVM-exception](https://github.com/shader-slang/slang/blob/master/LICENSE). Зависимости конкретного compiler package учитываются отдельно.
- **Dear ImGui:** отладка, профилирование и диагностические инструменты. [MIT](https://github.com/ocornut/imgui/blob/master/LICENSE.txt).
- **Lua:** обязательный последующий модуль, необязательный в каждой игре. Конкретная реализация и версия выбираются при интеграции; выбор Lua не означает автоматически выбор LuaJIT. [Лицензия Lua](https://www.lua.org/license.html).

Вспомогательные библиотеки для текста/shaping, изображений, glTF и упаковки ещё не интегрированы. Их выбор — задача реализации M0/M5, с учётом состава распространяемого пакета и транзитивных лицензий.

## Сборочная среда

CMake и Ninja управляют native build graph; Clang компилирует C++, а Slang — шейдеры. Windows-конфигурация clang-cl использует Windows SDK, UCRT и Visual C++ Runtime; установка одного LLVM не заменяет эти компоненты. Версии и допустимое распространение каждого комплекта фиксируются перед подготовкой SDK.

- [CMake: лицензирование](https://cmake.org/licensing/).
- [Ninja: Apache 2.0](https://github.com/ninja-build/ninja/blob/master/COPYING).
- [LLVM: лицензия и исключения](https://llvm.org/docs/DeveloperPolicy.html#copyright-license-and-patents).
- [Clang: Windows system headers and libraries](https://clang.llvm.org/docs/UsersManual.html#windows-system-headers-and-library-lookup).

Первоначально Windows и Linux имеют собственные сборочные и проверочные workers. Платформенные SDK и runtime — реальные зависимости. Офлайн-сборка означает доступный локальный набор инструментов/исходников, а не отсутствие первоначальной установки.

## Исследуемые движки не являются зависимостями Faset

Unreal Engine, Godot, UnityCsReference и Blender изучались как источники архитектурных идей и наблюдений. Их source tree не входит в этот репозиторий; ссылки указывают на upstream snapshots. Реализация Faset должна учитывать происхождение любого фактически добавляемого кода, а не считать изучение разрешением на копирование.

- [Unreal Engine EULA](https://www.unrealengine.com/eula/unreal) — условия Epic; source-доступ может требовать авторизации.
- [Godot MIT](https://github.com/godotengine/godot/blob/master/LICENSE.txt).
- [UnityCsReference: reference-source terms](https://github.com/Unity-Technologies/UnityCsReference/blob/master/LICENSE.md).
- [Blender: лицензирование](https://www.blender.org/about/license/).

Blender остаётся отдельной официальной программой. Файловый обмен GLB и metadata не требует включения Blender в Player. У будущего дополнения Blender будет явно указанная лицензия и проверенный способ распространения; лицензия Faset не переопределяет условия API/кода Blender.

## Текущая браузерная карта

Карта использует React, React DOM, react-markdown, remark-gfm и esbuild; точный npm-граф закреплён в [package-lock.json](studies/map/package-lock.json). Это зависимости просмотрщика исследований, не выбранная технология UI игрового редактора. Их код не vendored в Git: node_modules и dist исключены. При отдельном распространении собранного приложения нужно сохранить notices включённых компонентов.

## Правила независимости

Собственные форматы, schema IDs и API; закреплённые версии; source-доступ к ключевым зависимостям; воспроизводимая интеграция; возможность исправить или fork-нуть библиотеку; отсутствие обязательной облачной активации. Замена библиотеки всё равно имеет техническую стоимость.

Для каждого поставляемого компонента хранить исходный URL, pin, лицензию, notices, местные изменения, назначение и способ попадания в runtime/SDK. Наличие permissive license не освобождает от её уведомлений и условий. Лицензия собственного содержимого Faset описана отдельно в [PUBLICATION.md](PUBLICATION.md).
