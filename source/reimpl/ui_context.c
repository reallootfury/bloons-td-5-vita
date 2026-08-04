/*
 * Read-only BTD5 native screen-context detection.
 *
 * These layouts are pinned to the two libnative.so fingerprints accepted by
 * game.c. Every pointer crossing from the reverse-engineered Android object
 * graph is checked against a live Vita memory block before it is read. A
 * failed or transitional scan preserves the last good screen so it cannot
 * manufacture a false transition in the middle of a touch gesture.
 */

#include "reimpl/ui_context.h"

#include "game.h"
#include "utils/logger.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

#include <so_util/so_util.h>

#define UI_MAX_PATH       8
#define UI_MAX_RTTI_DEPTH 16

/* Android's 32-bit libc++ std::string layout used by both supported builds. */
#define ANDROID_STRING_SIZE 12U

/* Resolved by libsupc++/libstdc++. Their Itanium RTTI address point is +8. */
extern const unsigned char _ZTVN10__cxxabiv117__class_type_infoE[];
extern const unsigned char _ZTVN10__cxxabiv120__si_class_type_infoE[];
extern const unsigned char _ZTVN10__cxxabiv121__vmi_class_type_infoE[];

extern so_module so_mod;
extern void *__malloc_sbrk_base;

typedef struct UiNativeLayout {
    uint32_t app_slot_rva;
    uint16_t children_begin;
    uint16_t children_end;
    uint16_t input_flags;
    uint16_t screen_name;
    uint16_t screen_state;
    uint16_t child_priority;

    uint32_t ti_game;
    uint32_t ti_pause;
    uint32_t ti_settings;
    uint32_t ti_main;
    uint32_t ti_main_phone;
    uint32_t ti_popup;
    uint32_t ti_alert;
    uint32_t ti_confirmation;
    uint32_t ti_generic_confirmation;
    uint32_t ti_game_hud_phone;
    uint32_t ti_screen_manager;
    uint32_t cxx_class_vtable;
    uint32_t cxx_si_vtable;
    uint32_t cxx_vmi_vtable;
} UiNativeLayout;

typedef struct UiResolvedScreen {
    uintptr_t deepest;
    uintptr_t path[UI_MAX_PATH];
    int path_count;
} UiResolvedScreen;

typedef struct UiContextState {
    uintptr_t active_screen;
    uint32_t epoch;
    UiContext context;
    char screen_name[48];
    bool touch_cancel_required;
} UiContextState;

static UiContextState ui_context;
static uint32_t resolve_failure_stages;

enum {
    UI_RESOLVE_FAIL_MODULE = 1U << 0,
    UI_RESOLVE_FAIL_APP_SLOT = 1U << 1,
    UI_RESOLVE_FAIL_APP_OBJECT = 1U << 2,
    UI_RESOLVE_FAIL_MANAGER_SLOT = 1U << 3,
    UI_RESOLVE_FAIL_MANAGER_OBJECT = 1U << 4,
    UI_RESOLVE_FAIL_MANAGER_TYPE = 1U << 5,
};

static const UiNativeLayout layout_47 = {
    .app_slot_rva = 0x00889cec,
    .children_begin = 0x7c,
    .children_end = 0x80,
    .input_flags = 0x88,
    .screen_name = 0x8c,
    .screen_state = 0x9c,
    .child_priority = 0xa0,
    .ti_game = 0x0081a340,
    .ti_pause = 0x0081af98,
    .ti_settings = 0x0081f5e8,
    .ti_main = 0x0081e864,
    .ti_main_phone = 0x00821b44,
    .ti_popup = 0x0081f1a0,
    .ti_alert = 0x0081b77c,
    .ti_confirmation = 0x0081c124,
    .ti_generic_confirmation = 0x0081d4e8,
    .ti_game_hud_phone = 0x00821374,
    .ti_screen_manager = 0x00826400,
    .cxx_class_vtable = 0x0083b750,
    .cxx_si_vtable = 0x0083b778,
    .cxx_vmi_vtable = 0x0083b7ac,
};

static const UiNativeLayout layout_337 = {
    .app_slot_rva = 0x0099bccc,
    .children_begin = 0x80,
    .children_end = 0x84,
    .input_flags = 0x8c,
    .screen_name = 0x90,
    .screen_state = 0xa0,
    .child_priority = 0xa4,
    .ti_game = 0x0093c320,
    .ti_pause = 0x0093cfac,
    .ti_settings = 0x00941670,
    .ti_main = 0x009408b0,
    .ti_main_phone = 0x00943dc0,
    .ti_popup = 0x00941220,
    .ti_alert = 0x0093d7a0,
    .ti_confirmation = 0x0093e16c,
    .ti_generic_confirmation = 0x0093f640,
    .ti_game_hud_phone = 0x009435d0,
    .ti_screen_manager = 0x009481d0,
    .cxx_class_vtable = 0x00950eec,
    .cxx_si_vtable = 0x00950f14,
    .cxx_vmi_vtable = 0x00950f48,
};

static const UiNativeLayout *native_layout(void) {
    return btd5_game_version() == BTD5_VERSION_47 ? &layout_47 : &layout_337;
}

static bool bounded_range_valid(uintptr_t address, size_t size,
                                uintptr_t base, size_t range_size) {
    return base != 0 && range_size != 0 && address >= base &&
           size <= range_size && address - base <= range_size - size;
}

/* libnative.so's fixed-address segments are allocated by the kernel helper.
 * sceKernelFindMemBlockByAddr() cannot reliably discover those blocks from
 * user mode, so validate SO addresses against the loader's recorded segment
 * ranges before falling back to the normal heap-block query. */
static bool module_range_valid(uintptr_t address, size_t size) {
    if (address < 0x10000U || size == 0 || size > UINT32_MAX ||
        address > UINTPTR_MAX - size) {
        return false;
    }
    if (bounded_range_valid(address, size, so_mod.exec_base,
                            so_mod.exec_size)) {
        return true;
    }
    int data_count = so_mod.n_data;
    if (data_count < 0) {
        data_count = 0;
    } else if (data_count > MAX_DATA_SEG) {
        data_count = MAX_DATA_SEG;
    }
    for (int i = 0; i < data_count; ++i) {
        if (bounded_range_valid(address, size, so_mod.data_base[i],
                                so_mod.data_size[i])) {
            return true;
        }
    }
    return false;
}

/* BTD5's imported malloc/free resolve to newlib. Vita's user-mode memblock
 * query does not report interior addresses from that process heap on retail
 * hardware. Newlib keeps an exported first-break pointer, and sbrk(0) gives
 * the current committed end, so this is tighter than accepting an arbitrary
 * user address. */
static bool newlib_heap_range_valid(uintptr_t address, size_t size) {
    uintptr_t base = (uintptr_t)__malloc_sbrk_base;
    uintptr_t current = (uintptr_t)sbrk(0);
    if (base == UINTPTR_MAX || current == UINTPTR_MAX || current <= base) {
        return false;
    }
    return bounded_range_valid(address, size, base, current - base);
}

static bool range_valid(uintptr_t address, size_t size) {
    if (address < 0x10000U || size == 0 || size > UINT32_MAX ||
        address > UINTPTR_MAX - size) {
        return false;
    }
    if (module_range_valid(address, size)) {
        return true;
    }
    if (newlib_heap_range_valid(address, size)) {
        return true;
    }
    return sceKernelFindMemBlockByAddr((const void *)address, (SceSize)size) >= 0;
}

static bool read_bytes(uintptr_t address, void *output, size_t size) {
    if (!output || !range_valid(address, size)) {
        return false;
    }
    sceClibMemcpy(output, (const void *)address, size);
    return true;
}

static bool read_u8(uintptr_t address, uint8_t *output) {
    return read_bytes(address, output, sizeof(*output));
}

static bool read_u32(uintptr_t address, uint32_t *output) {
    return read_bytes(address, output, sizeof(*output));
}

static bool ascii_equal_fold(char a, char b) {
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    return a == b;
}

static bool contains_fold(const char *text, const char *fragment) {
    if (!text || !fragment || fragment[0] == '\0') {
        return false;
    }
    for (const char *start = text; *start; ++start) {
        const char *left = start;
        const char *right = fragment;
        while (*left && *right && ascii_equal_fold(*left, *right)) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

static bool read_android_string(uintptr_t string_address, char *output,
                                size_t output_size) {
    uint8_t first;
    uint32_t length;
    uint32_t pointer;
    uintptr_t characters;

    if (!output || output_size == 0 ||
        !range_valid(string_address, ANDROID_STRING_SIZE) ||
        !read_u8(string_address, &first)) {
        return false;
    }
    output[0] = '\0';

    if ((first & 1U) == 0) {
        length = (uint32_t)(first >> 1);
        characters = string_address + 1U;
        if (length > ANDROID_STRING_SIZE - 1U) {
            return false;
        }
    } else {
        if (!read_u32(string_address + 4U, &length) ||
            !read_u32(string_address + 8U, &pointer)) {
            return false;
        }
        characters = (uintptr_t)pointer;
        if (length > 255U) {
            return false;
        }
    }

    size_t copy_length = length;
    if (copy_length >= output_size) {
        copy_length = output_size - 1U;
    }
    if (copy_length > 0 &&
        !read_bytes(characters, output, copy_length)) {
        output[0] = '\0';
        return false;
    }
    output[copy_length] = '\0';

    /* Reject control-filled data if an offset ever drifts between versions. */
    for (size_t i = 0; i < copy_length; ++i) {
        unsigned char c = (unsigned char)output[i];
        if (c < 0x20U || c > 0x7eU) {
            output[0] = '\0';
            return false;
        }
    }
    return true;
}

static bool object_typeinfo(uintptr_t object, uintptr_t *typeinfo) {
    uint32_t vptr;
    uint32_t ti;
    if (!typeinfo || !read_u32(object, &vptr) ||
        !module_range_valid((uintptr_t)vptr, sizeof(uint32_t)) ||
        vptr < sizeof(uint32_t) ||
        !read_u32((uintptr_t)vptr - sizeof(uint32_t), &ti) ||
        !module_range_valid((uintptr_t)ti, 12U)) {
        return false;
    }
    *typeinfo = (uintptr_t)ti;
    return true;
}

static bool find_base_offset_r(uintptr_t typeinfo, uintptr_t target,
                               ptrdiff_t accumulated, int depth,
                               ptrdiff_t *result) {
    const UiNativeLayout *layout = native_layout();
    uint32_t kind;
    if (!result || depth >= UI_MAX_RTTI_DEPTH ||
        !module_range_valid(typeinfo, 12U)) {
        return false;
    }
    if (typeinfo == target) {
        *result = accumulated;
        return true;
    }
    if (!read_u32(typeinfo, &kind)) {
        return false;
    }

    uintptr_t class_kind =
        (uintptr_t)_ZTVN10__cxxabiv117__class_type_infoE + 8U;
    uintptr_t si_kind =
        (uintptr_t)_ZTVN10__cxxabiv120__si_class_type_infoE + 8U;
    uintptr_t vmi_kind =
        (uintptr_t)_ZTVN10__cxxabiv121__vmi_class_type_infoE + 8U;

    bool is_class = (uintptr_t)kind == class_kind ||
                    (uintptr_t)kind == so_mod.text_base +
                                       layout->cxx_class_vtable + 8U;
    bool is_si = (uintptr_t)kind == si_kind ||
                 (uintptr_t)kind == so_mod.text_base +
                                    layout->cxx_si_vtable + 8U;
    bool is_vmi = (uintptr_t)kind == vmi_kind ||
                  (uintptr_t)kind == so_mod.text_base +
                                     layout->cxx_vmi_vtable + 8U;

    /* Both pinned APKs define their own libc++abi RTTI vtables. The host
     * comparisons cover loader variants that resolve those symbols instead. */
    if (is_class) {
        return false;
    }
    if (is_si) {
        uint32_t base;
        return read_u32(typeinfo + 8U, &base) &&
               module_range_valid((uintptr_t)base, 12U) &&
               find_base_offset_r((uintptr_t)base, target, accumulated,
                                  depth + 1, result);
    }
    if (is_vmi) {
        uint32_t count;
        if (!read_u32(typeinfo + 12U, &count) || count > 32U ||
            !module_range_valid(typeinfo + 16U, (size_t)count * 8U)) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t base;
            uint32_t offset_flags;
            uintptr_t entry = typeinfo + 16U + (uintptr_t)i * 8U;
            if (!read_u32(entry, &base) ||
                !read_u32(entry + 4U, &offset_flags) ||
                !module_range_valid((uintptr_t)base, 12U)) {
                continue;
            }
            /* IButtonDelegate is non-virtual in the supported screen graph. */
            if ((offset_flags & 1U) != 0) {
                continue;
            }
            int32_t signed_flags = (int32_t)offset_flags;
            ptrdiff_t base_offset = (ptrdiff_t)(signed_flags >> 8);
            if (find_base_offset_r((uintptr_t)base, target,
                                   accumulated + base_offset, depth + 1,
                                   result)) {
                return true;
            }
        }
    }
    return false;
}

static bool object_has_base(uintptr_t object, uint32_t target_rva,
                            ptrdiff_t *offset) {
    uintptr_t typeinfo;
    if (!target_rva || !object_typeinfo(object, &typeinfo)) {
        return false;
    }
    return find_base_offset_r(typeinfo, so_mod.text_base + target_rva, 0, 0,
                              offset);
}

static bool screen_object_valid(uintptr_t screen) {
    uintptr_t ignored;
    return range_valid(screen, 4U) && object_typeinfo(screen, &ignored);
}

static bool read_screen_vector(uintptr_t screen, const UiNativeLayout *layout,
                               uint32_t *begin, uint32_t *end) {
    if (!read_u32(screen + layout->children_begin, begin) ||
        !read_u32(screen + layout->children_end, end)) {
        return false;
    }
    if (*begin == *end) {
        return true;
    }
    if (*begin == 0 || *end < *begin || ((*end - *begin) & 3U) != 0 ||
        (*end - *begin) / 4U > 128U) {
        return false;
    }
    return range_valid((uintptr_t)*begin, (size_t)(*end - *begin));
}

static uintptr_t choose_active_child(uintptr_t screen,
                                     const UiNativeLayout *layout) {
    uint32_t begin;
    uint32_t end;
    uintptr_t first_valid = 0;
    if (!read_screen_vector(screen, layout, &begin, &end) || begin == end) {
        return 0;
    }

    for (uint32_t item = begin; item < end; item += 4U) {
        uint32_t child32;
        uint32_t flags;
        if (!read_u32(item, &child32) || !screen_object_valid(child32)) {
            continue;
        }
        if (!first_valid) {
            first_valid = (uintptr_t)child32;
        }
        /* The native recursive dispatcher visits every child in vector order;
         * state and priority only affect whether the parent is handled before
         * or after that loop. Prefer the first input-enabled child and retain
         * the first valid object as a fallback for transitional screens. */
        if (read_u32((uintptr_t)child32 + layout->input_flags, &flags) &&
            (flags & 0x20U) == 0) {
            return (uintptr_t)child32;
        }
    }
    return first_valid;
}

static void report_resolve_failure_once(uint32_t stage, const char *name,
                                        uintptr_t address) {
    if ((resolve_failure_stages & stage) != 0) {
        return;
    }
    resolve_failure_stages |= stage;
    l_warn("Vita UI scan stopped at %s (0x%08x).", name,
           (unsigned int)address);
    (void)name;
    (void)address;
}

static bool resolve_active_screen(UiResolvedScreen *resolved) {
    const UiNativeLayout *layout = native_layout();
    uint32_t app;
    uint32_t manager;
    ptrdiff_t manager_base;
    if (!resolved) {
        return false;
    }
    if (!so_mod.text_base) {
        report_resolve_failure_once(UI_RESOLVE_FAIL_MODULE, "module", 0);
        return false;
    }
    uintptr_t app_slot = so_mod.text_base + layout->app_slot_rva;
    if (!read_u32(app_slot, &app)) {
        report_resolve_failure_once(UI_RESOLVE_FAIL_APP_SLOT, "app slot",
                                    app_slot);
        return false;
    }
    if (!range_valid((uintptr_t)app, 0x20U)) {
        report_resolve_failure_once(UI_RESOLVE_FAIL_APP_OBJECT, "app object",
                                    (uintptr_t)app);
        return false;
    }
    if (!read_u32((uintptr_t)app + 0x1cU, &manager)) {
        report_resolve_failure_once(UI_RESOLVE_FAIL_MANAGER_SLOT,
                                    "screen-manager slot",
                                    (uintptr_t)app + 0x1cU);
        return false;
    }
    if (!screen_object_valid((uintptr_t)manager)) {
        report_resolve_failure_once(UI_RESOLVE_FAIL_MANAGER_OBJECT,
                                    "screen-manager object",
                                    (uintptr_t)manager);
        return false;
    }
    if (!object_has_base((uintptr_t)manager, layout->ti_screen_manager,
                         &manager_base)) {
        report_resolve_failure_once(UI_RESOLVE_FAIL_MANAGER_TYPE,
                                    "screen-manager RTTI",
                                    (uintptr_t)manager);
        return false;
    }

    resolved->path_count = 0;
    uintptr_t current = (uintptr_t)manager;
    while (resolved->path_count < UI_MAX_PATH) {
        resolved->path[resolved->path_count++] = current;
        uintptr_t child = choose_active_child(current, layout);
        if (!child || child == current) {
            break;
        }
        current = child;
    }
    resolved->deepest = current;
    return true;
}

static bool screen_name(uintptr_t screen, char *output, size_t output_size) {
    return read_android_string(screen + native_layout()->screen_name, output,
                               output_size);
}

static UiContext classify_one_screen(uintptr_t screen) {
    const UiNativeLayout *layout = native_layout();
    ptrdiff_t ignored;
    char name[48] = {0};

    /* Pause and Settings also derive from popup-like bases; test them first. */
    if (object_has_base(screen, layout->ti_settings, &ignored)) {
        return UI_CONTEXT_SETTINGS;
    }
    if (object_has_base(screen, layout->ti_pause, &ignored)) {
        return UI_CONTEXT_PAUSE;
    }
    if (object_has_base(screen, layout->ti_main_phone, &ignored) ||
        object_has_base(screen, layout->ti_main, &ignored)) {
        return UI_CONTEXT_MAIN;
    }
    if (object_has_base(screen, layout->ti_game_hud_phone, &ignored) ||
        object_has_base(screen, layout->ti_game, &ignored)) {
        return UI_CONTEXT_GAME;
    }
    if (object_has_base(screen, layout->ti_confirmation, &ignored) ||
        object_has_base(screen, layout->ti_generic_confirmation, &ignored) ||
        object_has_base(screen, layout->ti_alert, &ignored) ||
        object_has_base(screen, layout->ti_popup, &ignored)) {
        return UI_CONTEXT_POPUP;
    }

    (void)screen_name(screen, name, sizeof(name));
    if (contains_fold(name, "setting") || contains_fold(name, "option")) {
        return UI_CONTEXT_SETTINGS;
    }
    if (contains_fold(name, "pause")) {
        return UI_CONTEXT_PAUSE;
    }
    if (contains_fold(name, "mainmenu") || contains_fold(name, "main menu")) {
        return UI_CONTEXT_MAIN;
    }
    if (contains_fold(name, "popup") || contains_fold(name, "alert") ||
        contains_fold(name, "confirm") || contains_fold(name, "dialog")) {
        return UI_CONTEXT_POPUP;
    }
    if (contains_fold(name, "gamehud") || contains_fold(name, "gamescreen") ||
        contains_fold(name, "towerselection") ||
        contains_fold(name, "towerupgrade") ||
        contains_fold(name, "target") || contains_fold(name, "ingame")) {
        return UI_CONTEXT_GAME;
    }
    return name[0] != '\0' ? UI_CONTEXT_MENU : UI_CONTEXT_UNKNOWN;
}

static UiContext classify_path(const UiResolvedScreen *resolved) {
    UiContext generic = UI_CONTEXT_UNKNOWN;
    for (int i = resolved->path_count - 1; i >= 0; --i) {
        UiContext result = classify_one_screen(resolved->path[i]);
        if (result == UI_CONTEXT_MENU) {
            generic = result;
            continue;
        }
        if (result != UI_CONTEXT_UNKNOWN) {
            return result;
        }
    }
    return generic;
}

static bool placement_touch_continues(UiContext old_context,
                                      const char *old_name,
                                      UiContext new_context,
                                      const char *new_name) {
    if (old_context != UI_CONTEXT_GAME || new_context != UI_CONTEXT_GAME) {
        return false;
    }

    bool old_borders = contains_fold(old_name, "InGameBorders");
    bool new_borders = contains_fold(new_name, "InGameBorders");
    bool old_placement = contains_fold(old_name, "TowerPlacementScreen");
    bool new_placement = contains_fold(new_name, "TowerPlacementScreen");


    return (old_borders && new_placement) ||
           (old_placement && new_borders);
}

void ui_context_init(void) {
    sceClibMemset(&ui_context, 0, sizeof(ui_context));
    resolve_failure_stages = 0;
}

bool ui_context_update(void) {
    UiResolvedScreen resolved;
    char next_screen_name[48] = {0};
    if (!resolve_active_screen(&resolved) ||
        resolved.deepest == ui_context.active_screen) {
        return false;
    }

    UiContext next_context = classify_path(&resolved);
    (void)screen_name(resolved.deepest, next_screen_name,
                      sizeof(next_screen_name));

    ui_context.touch_cancel_required = !placement_touch_continues(
        ui_context.context, ui_context.screen_name,
        next_context, next_screen_name);
    ui_context.context = next_context;
    ui_context.active_screen = resolved.deepest;
    ++ui_context.epoch;
    sceClibMemcpy(ui_context.screen_name, next_screen_name,
                  sizeof(ui_context.screen_name));

    l_info("Vita UI context: screen=%s (%s), epoch=%u.",
           ui_context.screen_name[0] ? ui_context.screen_name : "<unnamed>",
           ui_context_name(ui_context.context), ui_context.epoch);
    return true;
}

UiContext ui_context_current(void) {
    return ui_context.context;
}

bool ui_context_touch_cancel_required(void) {
    return ui_context.touch_cancel_required;
}

const char *ui_context_name(UiContext context) {
    switch (context) {
        case UI_CONTEXT_MAIN: return "main";
        case UI_CONTEXT_MENU: return "menu";
        case UI_CONTEXT_GAME: return "game";
        case UI_CONTEXT_PAUSE: return "pause";
        case UI_CONTEXT_SETTINGS: return "settings";
        case UI_CONTEXT_POPUP: return "popup";
        default: return "unknown";
    }
}
