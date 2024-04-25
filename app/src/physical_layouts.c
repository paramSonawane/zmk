/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/drivers/kscan.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/physical_layouts.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#define DT_DRV_COMPAT zmk_physical_layout

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZKPA_INIT(i, n)                                                                            \
    (const struct zmk_key_physical_attrs) {                                                        \
        .width = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, width),                          \
        .height = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, height),                        \
        .x = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, x),                                  \
        .y = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, y),                                  \
        .rx = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, rx),                                \
        .ry = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, ry),                                \
        .r = (int16_t)(int32_t)DT_INST_PHA_BY_IDX(n, keys, i, r),                                  \
    }

#define ZMK_LAYOUT_INST(n)                                                                         \
    static const struct zmk_key_physical_attrs const _CONCAT(                                      \
        _zmk_physical_layout_keys_, n)[DT_INST_PROP_LEN_OR(n, keys, 0)] = {                        \
        LISTIFY(DT_INST_PROP_LEN_OR(n, keys, 0), ZKPA_INIT, (, ), n)};                             \
    ZMK_MATRIX_TRANSFORM_EXTERN(DT_INST_PHANDLE(n, transform));                                    \
    static const struct zmk_physical_layout const _CONCAT(_zmk_physical_layout_,                   \
                                                          DT_DRV_INST(n)) = {                      \
        .display_name = DT_INST_PROP_OR(n, display_name, "Layout #" #n),                           \
        .matrix_transform = ZMK_MATRIX_TRANSFORM_T_FOR_NODE(DT_INST_PHANDLE(n, transform)),        \
        .keys = _CONCAT(_zmk_physical_layout_keys_, n),                                            \
        .keys_len = DT_INST_PROP_LEN_OR(n, keys, 0),                                               \
        COND_CODE_1(DT_INST_PROP_LEN(n, kscan),                                                    \
                    (.kscan = DEVICE_DT_GET(DT_INST_PHANDLE(n, kscan)), ), ())};

DT_INST_FOREACH_STATUS_OKAY(ZMK_LAYOUT_INST)

#define ZMK_LAYOUT_REF(n) &_CONCAT(_zmk_physical_layout_, DT_DRV_INST(n)),

static const struct zmk_physical_layout *const layouts[] = {
    DT_INST_FOREACH_STATUS_OKAY(ZMK_LAYOUT_REF)};

#elif DT_HAS_CHOSEN(zmk_matrix_transform)

ZMK_MATRIX_TRANSFORM_EXTERN(DT_CHOSEN(zmk_matrix_transform));

static const struct zmk_physical_layout _CONCAT(_zmk_physical_layout_, chosen) = {
    .display_name = "Default",
    .matrix_transform = ZMK_MATRIX_TRANSFORM_T_FOR_NODE(DT_CHOSEN(zmk_matrix_transform)),
    COND_CODE_1(DT_HAS_CHOSEN(zmk_kscan), (.kscan = DEVICE_DT_GET(DT_CHOSEN(zmk_kscan)), ), ())};

static const struct zmk_physical_layout *const layouts[] = {
    &_CONCAT(_zmk_physical_layout_, chosen)};

#elif DT_HAS_CHOSEN(zmk_kscan)

ZMK_MATRIX_TRANSFORM_DEFAULT_EXTERN();
static const struct zmk_physical_layout _CONCAT(_zmk_physical_layout_, chosen) = {
    .display_name = "Default",
    .matrix_transform = &zmk_matrix_transform_default,
    .kscan = DEVICE_DT_GET(DT_CHOSEN(zmk_kscan)),
};

static const struct zmk_physical_layout *const layouts[] = {
    &_CONCAT(_zmk_physical_layout_, chosen)};

#endif

const struct zmk_physical_layout *active;

size_t zmk_physical_layouts_get_list(struct zmk_physical_layout const *const **dest_layouts) {
    *dest_layouts = &layouts[0];

    return ARRAY_SIZE(layouts);
}

#define ZMK_KSCAN_EVENT_STATE_PRESSED 0
#define ZMK_KSCAN_EVENT_STATE_RELEASED 1

struct zmk_kscan_event {
    uint32_t row;
    uint32_t column;
    uint32_t state;
};

static struct zmk_kscan_msg_processor { struct k_work work; } msg_processor;

K_MSGQ_DEFINE(physical_layouts_kscan_msgq, sizeof(struct zmk_kscan_event),
              CONFIG_ZMK_KSCAN_EVENT_QUEUE_SIZE, 4);

static void zmk_physical_layout_kscan_callback(const struct device *dev, uint32_t row,
                                               uint32_t column, bool pressed) {
    if (dev != active->kscan) {
        return;
    }

    struct zmk_kscan_event ev = {
        .row = row,
        .column = column,
        .state = (pressed ? ZMK_KSCAN_EVENT_STATE_PRESSED : ZMK_KSCAN_EVENT_STATE_RELEASED)};

    k_msgq_put(&physical_layouts_kscan_msgq, &ev, K_NO_WAIT);
    k_work_submit(&msg_processor.work);
}

static void zmk_physical_layouts_kscan_process_msgq(struct k_work *item) {
    struct zmk_kscan_event ev;

    while (k_msgq_get(&physical_layouts_kscan_msgq, &ev, K_NO_WAIT) == 0) {
        bool pressed = (ev.state == ZMK_KSCAN_EVENT_STATE_PRESSED);
        int32_t position = zmk_matrix_transform_row_column_to_position(active->matrix_transform,
                                                                       ev.row, ev.column);

        if (position < 0) {
            LOG_WRN("Not found in transform: row: %d, col: %d, pressed: %s", ev.row, ev.column,
                    (pressed ? "true" : "false"));
            continue;
        }

        LOG_DBG("Row: %d, col: %d, position: %d, pressed: %s", ev.row, ev.column, position,
                (pressed ? "true" : "false"));
        raise_zmk_position_state_changed(
            (struct zmk_position_state_changed){.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
                                                .state = pressed,
                                                .position = position,
                                                .timestamp = k_uptime_get()});
    }
}

int zmk_physical_layouts_select_layout(const struct zmk_physical_layout *layout) {
    if (!layout) {
        return -ENODEV;
    }

    if (layout == active) {
        return 0;
    }

    if (active) {
        if (active->kscan) {
            kscan_disable_callback(active->kscan);
#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
            pm_device_runtime_put(active->kscan);
#elif IS_ENABLED(CONFIG_PM_DEVICE)
            pm_device_action_run(active->kscan, PM_DEVICE_ACTION_SUSPEND);
#endif
        }
    }

    active = layout;

    if (active->kscan) {
#if IS_ENABLED(CONFIG_PM_DEVICE_RUNTIME)
        int err = pm_device_runtime_get(active->kscan);
        if (err < 0) {
            LOG_WRN("PM runtime get of kscan device to enable it %d", err);
            return err;
        }
#elif IS_ENABLED(CONFIG_PM_DEVICE)
        pm_device_action_run(active->kscan, PM_DEVICE_ACTION_RESUME);
#endif
        kscan_config(active->kscan, zmk_physical_layout_kscan_callback);
        kscan_enable_callback(active->kscan);
    }

    return 0;
}

int zmk_physical_layouts_select(uint8_t index) {
    if (index >= ARRAY_SIZE(layouts)) {
        return -EINVAL;
    }

    return zmk_physical_layouts_select_layout(layouts[index]);
}

int zmk_physical_layouts_get_selected(void) {
    for (int i = 0; i < ARRAY_SIZE(layouts); i++) {
        if (layouts[i] == active) {
            return i;
        }
    }

    return -ENODEV;
}

int zmk_physical_layouts_select_initial(void) {
    const struct zmk_physical_layout *initial;

#if DT_HAS_CHOSEN(zmk_physical_layout)
    initial = &_CONCAT(_zmk_physical_layout_, DT_CHOSEN(zmk_physical_layout));
#else
    initial = layouts[0];
#endif

    int ret = zmk_physical_layouts_select_layout(initial);

#if IS_ENABLED(CONFIG_SETTINGS)
    ret = settings_load_subtree("physical_layouts");
#endif

    return ret;
}

int zmk_physical_layouts_save_selected(void) {
    uint8_t val = (uint8_t)zmk_physical_layouts_get_selected();

#if IS_ENABLED(CONFIG_SETTINGS)
    return settings_save_one("physical_layouts/selected", &val, sizeof(val));
#else
    return -ENOTSUP;
#endif
}

int zmk_physical_layouts_revert_selected(void) { return zmk_physical_layouts_select_initial(); }

#if IS_ENABLED(CONFIG_SETTINGS)

static int physical_layouts_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                       void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "selected", &next) && !next) {
        if (len != sizeof(uint8_t)) {
            return -EINVAL;
        }

        uint8_t selected_index;
        int err = read_cb(cb_arg, &selected_index, len);
        if (err <= 0) {
            LOG_ERR("Failed to handle selected physical layout from settings (err %d)", err);
            return err;
        }

        return zmk_physical_layouts_select(selected_index);
    }

    return 0;
};

SETTINGS_STATIC_HANDLER_DEFINE(physical_layouts, "physical_layouts", NULL,
                               physical_layouts_handle_set, NULL, NULL);

#endif // IS_ENABLED(CONFIG_SETTINGS)

static int zmk_physical_layouts_init(void) {
    k_work_init(&msg_processor.work, zmk_physical_layouts_kscan_process_msgq);

#if IS_ENABLED(CONFIG_PM_DEVICE)
    for (int l = 0; l < ARRAY_SIZE(layouts); l++) {
        const struct zmk_physical_layout *pl = layouts[l];
        if (pl->kscan) {
            if (pm_device_wakeup_is_capable(pl->kscan)) {
                pm_device_wakeup_enable(pl->kscan, true);
            }
        }
    }
#endif // IS_ENABLED(CONFIG_PM_DEVICE)

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
#endif

    return zmk_physical_layouts_select_initial();
}

SYS_INIT(zmk_physical_layouts_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);