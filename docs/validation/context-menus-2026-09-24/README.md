# Editor context menus — Linux, 2026-09-24

`scene-context.png` is a GPU capture of the implemented Editor at 1280 × 900 with
the Scene-root context menu open. It was produced by the headless
`editor_ui_context_menu` test using `FASET_CONTEXT_CAPTURE`.

The test also exercises entity Duplicate/Delete with Undo, inherited-instance
parenting, contextual file actions, menu dismissal, and the viewport's click vs.
right-drag camera gesture. It checks that selecting an unavailable imported asset
cannot use toolbar actions on a previously selected file. See
`tests/editor_ui_context_menu.cpp` for the exact acceptance assertions. The
generated visual exploration lives separately in
`docs/design/editor-context-menu-reference.png`.
