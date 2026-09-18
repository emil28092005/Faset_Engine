#pragma once
/* Exact-build native Editor SDK. No STL types or ownership cross this ABI. */
#include <faset/editor/sdk_build.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FASET_EDITOR_API_VERSION 1u
#if defined(_WIN32)
#define FASET_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FASET_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
/* UTF-8 JSON is borrowed for the duration of a call. write() copies output in
   the receiving module. Return zero on success; errors use {code,message}. */
typedef void (*FasetWrite)(void* receiver, const char* utf8, uint64_t length);
typedef int (*FasetCommand)(void* user, const char* arguments_json, FasetWrite write,
                            void* receiver);
typedef struct FasetEditorHost {
    uint32_t api_version;
    uint32_t struct_size;
    const char* build_fingerprint;
    void* context;
    int (*register_command)(void* context, const char* descriptor_json, FasetCommand callback,
                            void* user);
    int (*register_panel)(void* context, const char* panel_json);
    int (*invoke_command)(void* context, const char* name, const char* arguments_json,
                          FasetWrite write, void* receiver);
    void (*log)(void* context, const char* utf8);
} FasetEditorHost;
typedef struct FasetEditorPlugin {
    uint32_t api_version;
    uint32_t struct_size;
    const char* build_fingerprint;
    void* user;
    void (*shutdown)(void* user);
} FasetEditorPlugin;
/* Required exported symbol. Called once at startup, after dependencies load. */
typedef int (*FasetPluginEntry)(const FasetEditorHost* host, FasetEditorPlugin* plugin);
#ifdef __cplusplus
}
#endif
