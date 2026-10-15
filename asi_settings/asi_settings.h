/*
    ASI Settings 1.0
    https://github.com/ThirteenAG/FusionFixUtils/tree/master/asi_settings

    Copy this file into your plugin. It only declares the runtime interface and the names of the resources;
    the description format, the settings file and the rules are in SPEC.md next to it.

    Copies of different versions work together: the interface only grows (see SPEC.md, The runtime interface,
    Versions, and Compatibility).
*/
#ifndef ASI_SETTINGS_H
#define ASI_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

#define ASI_SETTINGS_VERSION_MAJOR 1
#define ASI_SETTINGS_VERSION_MINOR 0
/* Interface version passed to and returned by AsiSettings_GetApi: major * 1000 + minor (1.0 = 1000). Tables of the
   0.x drafts (version < 1000) have the same layout as 1.0 and count as 1.0 */
#define ASI_SETTINGS_VERSION (ASI_SETTINGS_VERSION_MAJOR * 1000 + ASI_SETTINGS_VERSION_MINOR)
#define ASI_SETTINGS_VERSION_MAJOR_OF(version) ((version) < 1000 ? 1 : (version) / 1000)

/* Resources: the description (JSON) and one translation (JSON) per BCP 47 language tag */
#define ASI_SETTINGS_RESOURCE_TYPE      L"ASISETTINGS"
#define ASI_SETTINGS_RESOURCE_NAME      L"SETTINGS"
#define ASI_SETTINGS_TEXT_RESOURCE_TYPE L"ASISETTINGS_TEXT"   /* name: language tag, such as L"en" or L"pt-BR" */

/* The export every plugin with settings provides */
#define ASI_SETTINGS_EXPORT_NAME "AsiSettings_GetApi"

/* Results */
#define ASI_SETTINGS_OK               0
#define ASI_SETTINGS_UNKNOWN_ID      -1
#define ASI_SETTINGS_INVALID_VALUE   -2
#define ASI_SETTINGS_NOT_NOW         -3
#define ASI_SETTINGS_UNSUPPORTED     -4
#define ASI_SETTINGS_INTERNAL        -5

/* Flags of GetState; callers ignore bits they don't know */
#define ASI_SETTINGS_STATE_HIDDEN     1

/* The live link (SPEC.md, Live link): one named pipe per game process, served by one of its plugins, through which
   editors read and change the running plugins' settings. Followed by the process id in decimal. */
#define ASI_SETTINGS_LINK_PIPE        L"\\\\.\\pipe\\AsiSettings."
#define ASI_SETTINGS_LINK_MUTEX       L"Local\\AsiSettingsLink."    /* owned by the plugin that serves */
#define ASI_SETTINGS_LINK_PROTOCOL    1

#ifndef ASI_SETTINGS_CALL
#define ASI_SETTINGS_CALL __cdecl
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Called after a setting changed; id is the setting's id, valid during the call */
typedef void (ASI_SETTINGS_CALL* AsiSettingsChanged)(void* context, const char* id);

/*
    The table a plugin returns. Fields are only ever added at the end, in minor versions. A caller uses a table only
    when ASI_SETTINGS_API_COMPATIBLE says so, and a function only when ASI_SETTINGS_API_HAS says the plugin's table has
    it (a table built with an older header is shorter; a NULL function counts as ASI_SETTINGS_UNSUPPORTED):
        if (ASI_SETTINGS_API_HAS(api, Reload)) api->Reload();
*/
typedef struct AsiSettingsApi
{
    uint32_t size;              /* sizeof(AsiSettingsApi) as the plugin was built */
    uint32_t version;           /* ASI_SETTINGS_VERSION of the plugin */
    const char* pluginId;       /* plugin.id of the description; NULL or empty: the id editors work out */

    /* Current value as text in the settings file's syntax; snprintf-like: returns the full length */
    int32_t (ASI_SETTINGS_CALL* Get)(const char* id, char* value, uint32_t valueSize);
    /* Checks, applies and stores a value given as text */
    int32_t (ASI_SETTINGS_CALL* Set)(const char* id, const char* value);
    /* Values usable now (or the choices of a dynamic setting) as a JSON array; snprintf-like */
    int32_t (ASI_SETTINGS_CALL* GetChoices)(const char* id, char* json, uint32_t jsonSize);
    /* Runs an action setting */
    int32_t (ASI_SETTINGS_CALL* Invoke)(const char* id);
    /* Change notifications; Subscribe returns a token > 0 */
    int32_t (ASI_SETTINGS_CALL* Subscribe)(AsiSettingsChanged callback, void* context);
    int32_t (ASI_SETTINGS_CALL* Unsubscribe)(int32_t token);
    /* Reads the settings file again */
    int32_t (ASI_SETTINGS_CALL* Reload)(void);
    /* 0, or ASI_SETTINGS_STATE_HIDDEN when no value is usable now; a reason (text) snprintf-like */
    int32_t (ASI_SETTINGS_CALL* GetState)(const char* id, char* reason, uint32_t reasonSize);
} AsiSettingsApi;

typedef const AsiSettingsApi* (ASI_SETTINGS_CALL* AsiSettingsGetApi)(uint32_t hostVersion);

/* The size of the 1.0 table, whatever this header's version */
#define ASI_SETTINGS_API_SIZE_1_0 ((uint32_t)(offsetof(AsiSettingsApi, GetState) + sizeof(void*)))

/* Whether a table can be used by a caller built with this header: same major version (0.x drafts count as 1), and
   at least the fields up to Set */
#define ASI_SETTINGS_API_COMPATIBLE(api) \
    ((api) != NULL && ASI_SETTINGS_VERSION_MAJOR_OF((api)->version) == ASI_SETTINGS_VERSION_MAJOR && \
     (api)->size >= offsetof(AsiSettingsApi, Set) + sizeof(void*))

/* Whether the plugin's table has a function: its size covers the field and the pointer isn't NULL */
#define ASI_SETTINGS_API_HAS(api, function) \
    ((api)->size >= offsetof(AsiSettingsApi, function) + sizeof((api)->function) && (api)->function != NULL)

/*
    In the plugin (the table it returns doesn't depend on hostVersion within a major version):
        extern "C" __declspec(dllexport) const AsiSettingsApi* ASI_SETTINGS_CALL AsiSettings_GetApi(uint32_t hostVersion)
        {
            return &myApi;
        }

    In a menu host, for each loaded module:
        auto getApi = (AsiSettingsGetApi)GetProcAddress(module, ASI_SETTINGS_EXPORT_NAME);
        const AsiSettingsApi* api = getApi ? getApi(ASI_SETTINGS_VERSION) : nullptr;
        if (!ASI_SETTINGS_API_COMPATIBLE(api)) api = nullptr;
*/

#ifdef __cplusplus
}
#endif

#endif /* ASI_SETTINGS_H */
