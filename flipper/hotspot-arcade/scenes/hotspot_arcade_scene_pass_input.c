#include "../hotspot_arcade_i.h"
#include "../helpers/ha_storage.h"

static void ha_pass_input_cb(void* context) {
    HotspotArcadeApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, HaEventPassDone);
}

static void ha_pass_dialog(HotspotArcadeApp* app, const char* header, const char* text) {
    DialogMessage* m = dialog_message_alloc();
    dialog_message_set_header(m, header, 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(m, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(m, NULL, "OK", NULL);
    dialog_message_show(app->dialogs, m);
    dialog_message_free(m);
}

void hotspot_arcade_scene_pass_input_on_enter(void* context) {
    HotspotArcadeApp* app = context;
    strncpy(app->pass_buf, furi_string_get_cstr(app->pass), HA_PASS_MAX - 1);
    app->pass_buf[HA_PASS_MAX - 1] = '\0';

    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Password (8-63, empty = open)");
    text_input_set_result_callback(
        app->text_input, ha_pass_input_cb, app, app->pass_buf, HA_PASS_MAX, false);
    view_dispatcher_switch_to_view(app->view_dispatcher, HaViewTextInput);
}

bool hotspot_arcade_scene_pass_input_on_event(void* context, SceneManagerEvent event) {
    HotspotArcadeApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == HaEventPassDone) {
        size_t len = strlen(app->pass_buf);
        if(len > 0 && len < 8) {
            ha_pass_dialog(
                app,
                "Invalid password",
                "Needs 8-63 chars,\nor leave empty\nfor an open network.");
            scene_manager_previous_scene(app->scene_manager);
            return true;
        }
        furi_string_set(app->pass, app->pass_buf);
        ha_storage_save_config(app);
        if(app->session_active) {
            ha_pass_dialog(app, "Password saved", "Restart session\nto apply.");
        }
        scene_manager_previous_scene(app->scene_manager);
        return true;
    }
    return false;
}

void hotspot_arcade_scene_pass_input_on_exit(void* context) {
    UNUSED(context);
}
