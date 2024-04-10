/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(keymap)

#define KEYMAP_RESPONSE(type, ...) ZMK_RPC_RESPONSE(keymap, type, __VA_ARGS__)

static bool encode_layer_bindings(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const uint8_t layer_idx = *(uint8_t *)*arg;

    for (int b = 0; b < ZMK_KEYMAP_LEN; b++) {
        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(layer_idx, b);

        zmk_keymap_BehaviorBinding bb = zmk_keymap_BehaviorBinding_init_zero;

        bb.behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
        bb.param1 = binding->param1;
        bb.param2 = binding->param2;

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_keymap_BehaviorBinding_msg, &bb)) {
            return false;
        }
    }

    return true;
}

static bool encode_layer_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const uint8_t layer_idx = *(uint8_t *)*arg;

    const char *name = zmk_keymap_layer_name(layer_idx);

    if (!name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, name, strlen(name));
}

static bool encode_keymap_layers(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    for (int l = 0; l < ZMK_KEYMAP_LAYERS_LEN; l++) {

        if (!pb_encode_tag_for_field(stream, field)) {
            LOG_DBG("Failed to encode tag");
            return false;
        }

        zmk_keymap_Layer layer = zmk_keymap_Layer_init_zero;

        layer.name.funcs.encode = encode_layer_name;
        layer.name.arg = &l;

        layer.bindings.funcs.encode = encode_layer_bindings;
        layer.bindings.arg = &l;

        if (!pb_encode_submessage(stream, &zmk_keymap_Layer_msg, &layer)) {
            LOG_DBG("Failed to encode layer submessage");
            return false;
        }
    }

    return true;
}

zmk_Response get_keymap(const zmk_Request *req) {
    zmk_keymap_Keymap resp = zmk_keymap_Keymap_init_zero;

    resp.layers.funcs.encode = encode_keymap_layers;

    return KEYMAP_RESPONSE(get_keymap, resp);
}

zmk_Response set_layer_binding(const zmk_Request *req) {
    const zmk_keymap_SetLayerBindingRequest *set_req =
        &req->subsystem.keymap.request_type.set_layer_binding;

    zmk_behavior_local_id_t bid = set_req->binding.behavior_id;

    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(bid);

    if (!behavior_name) {
        return KEYMAP_RESPONSE(set_layer_binding,
                               zmk_keymap_SetLayerBindingResponse_INVALID_BEHAVIOR);
    }

    struct zmk_behavior_binding binding = (struct zmk_behavior_binding){
        .behavior_dev = behavior_name,
        .param1 = set_req->binding.param1,
        .param2 = set_req->binding.param2,
    };

    int ret = zmk_behavior_validate_binding(&binding);
    if (ret < 0) {
        return KEYMAP_RESPONSE(set_layer_binding,
                               zmk_keymap_SetLayerBindingResponse_INVALID_PARAMETERS);
    }

    ret = zmk_keymap_set_layer_binding_at_idx(set_req->layer, set_req->key_position, binding);

    if (ret < 0) {
        LOG_DBG("Setting the binding failed with %d", ret);
        switch (ret) {
        case -EINVAL:
            return KEYMAP_RESPONSE(set_layer_binding,
                                   zmk_keymap_SetLayerBindingResponse_INVALID_LOCATION);
        default:
            return ZMK_RPC_SIMPLE_ERR(GENERIC);
        }
    }

    return KEYMAP_RESPONSE(set_layer_binding, zmk_keymap_SetLayerBindingResponse_SUCCESS);
}

zmk_Response save_changes(const zmk_Request *req) {
    int ret = zmk_keymap_save_changes();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return KEYMAP_RESPONSE(save_changes, true);
}

zmk_Response discard_changes(const zmk_Request *req) {
    int ret = zmk_keymap_discard_changes();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return KEYMAP_RESPONSE(discard_changes, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(keymap, get_keymap, true);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, set_layer_binding, true);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, save_changes, true);
ZMK_RPC_SUBSYSTEM_HANDLER(keymap, discard_changes, true);

static int event_mapper(const zmk_event_t *eh, zmk_Notification *n) { return 0; }

ZMK_RPC_EVENT_MAPPER(keymap, event_mapper);
