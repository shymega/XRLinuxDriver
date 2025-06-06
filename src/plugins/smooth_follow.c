#include "buffer.h"
#include "config.h"
#include "features/breezy_desktop.h"
#include "features/smooth_follow.h"
#include "imu.h"
#include "ipc.h"
#include "logging.h"
#include "memory.h"
#include "plugins/gamescope_reshade_wayland.h"
#include "plugins/smooth_follow.h"
#include "runtime_context.h"
#include "state.h"
#include "strings.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef enum {
    FOLLOW_STATE_NONE,
    FOLLOW_STATE_INIT,
    FOLLOW_STATE_WAITING,
    FOLLOW_STATE_SLERPING
} follow_state_type;

const smooth_follow_params init_params = {
    .lower_angle_threshold = 2.0,
    .upper_angle_threshold = 2.0,
    .delay_ms = 0,
    .return_to_angle = 2.0,

    // moves 99.9% of the way to the center in 1 second
    .interpolation_ratio_ms = 1-pow(1 - 0.999, 1.0/1000.0)
};

const smooth_follow_params sticky_params = {
    .lower_angle_threshold = 0.5,
    .upper_angle_threshold = 0.5,
    .delay_ms = 0,
    .return_to_angle = 0.5,

    // moves 99% of the way to the center in 1 second
    .interpolation_ratio_ms = 1-pow(1 - 0.99, 1.0/1000.0)
};

const smooth_follow_params loose_follow_params = {
    .lower_angle_threshold = 20.0,
    .upper_angle_threshold = 40.0,
    .delay_ms = 2000,
    .return_to_angle = 5.0,

    // moves 99% of the way to the center in 1.5 seconds
    .interpolation_ratio_ms = 1-pow(1 - 0.99, 1.0/1500.0)
};

uint32_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

smooth_follow_config* sf_config = NULL;
smooth_follow_params* sf_params = NULL;

void *smooth_follow_default_config_func() {
    smooth_follow_config *config = calloc(1, sizeof(smooth_follow_config));
    config->virtual_display_enabled = false;
    config->virtual_display_follow_enabled = false;
    config->sideview_enabled = false;
    config->sideview_follow_enabled = false;
    config->sideview_follow_threshold = 0.5;
    config->sideview_display_size = 1.0;
    config->breezy_desktop_enabled = false;
    config->virtual_display_size = 1.0;
    config->sbs_display_distance = 1.0;
    config->sbs_display_size = 1.0;
    config->track_roll = false;
    config->track_pitch = true;
    config->track_yaw = true;

    return config;
};

// TODO -   This has become a bit of a mess with smooth_follow having to be aware of how it interacts with
//          3 other plugins and their configs and control flags. Smooth follow should just become a utility 
//          function that any plugin can utilize however it deems appropriate.
void smooth_follow_handle_config_line_func(void* config, char* key, char* value) {
    smooth_follow_config* temp_config = (smooth_follow_config*) config;
    if (equal(key, "virtual_display_smooth_follow_enabled")) {
        boolean_config(key, value, &temp_config->virtual_display_follow_enabled);
    } else if (equal(key, "sideview_smooth_follow_enabled")) {
        boolean_config(key, value, &temp_config->sideview_follow_enabled);
    } else if (equal(key, "sideview_follow_threshold")) {
        float_config(key, value, &temp_config->sideview_follow_threshold);
    } else if (equal(key, "sideview_display_size")) {
        float_config(key, value, &temp_config->sideview_display_size);
    } else if (equal(key, "external_mode")) {
        if (equal(value, "virtual_display")) {
            temp_config->virtual_display_enabled = true;
        } else if (equal(value, "sideview")) {
            temp_config->sideview_enabled = true;
        } else if (equal(value, "breezy_desktop")) {
            temp_config->breezy_desktop_enabled = true;
        }
    } else if (equal(key, "display_zoom")) {
        float_config(key, value, &temp_config->virtual_display_size);
    } else if (equal(key, "sbs_display_distance")) {
        float_config(key, value, &temp_config->sbs_display_distance);
    } else if (equal(key, "sbs_display_size")) {
        float_config(key, value, &temp_config->sbs_display_size);
    } else if (equal(key, "smooth_follow_track_roll")) {
        boolean_config(key, value, &temp_config->track_roll);
    } else if (equal(key, "smooth_follow_track_pitch")) {
        boolean_config(key, value, &temp_config->track_pitch);
    } else if (equal(key, "smooth_follow_track_yaw")) {
        boolean_config(key, value, &temp_config->track_yaw);
    }
}

static bool smooth_follow_enabled=false;
follow_state_type follow_state = FOLLOW_STATE_NONE;
uint32_t last_timestamp_ms = -1;
static imu_quat_type *origin_quat = NULL;
static bool snap_back_to_center = false;
uint32_t start_snap_back_timestamp_ms = -1;
static bool was_sbs_mode_enabled = false;
static void update_smooth_follow_params() {
    if (!sf_params) sf_params = calloc(1, sizeof(smooth_follow_params));
    bool virtual_display_follow = sf_config->virtual_display_enabled && sf_config->virtual_display_follow_enabled;
    bool smooth_follow = sf_config->sideview_enabled && sf_config->sideview_follow_enabled;
    bool breezy_desktop_follow = sf_config->breezy_desktop_enabled && state()->breezy_desktop_smooth_follow_enabled;
    float display_distance = 1.0;
    if (state()->sbs_mode_enabled) {
        display_distance = sf_config->sbs_display_distance;
    }
    device_properties_type* device = device_checkout();
    if (device != NULL) {
        float half_fov = device->fov / 2.0;
        if (virtual_display_follow) {
            *sf_params = loose_follow_params;
            float display_size = state()->sbs_mode_enabled ? sf_config->sbs_display_size : sf_config->virtual_display_size;
            float device_fov_threshold = device->fov * display_size / display_distance;

            sf_params->lower_angle_threshold = device_fov_threshold;
            sf_params->upper_angle_threshold = device_fov_threshold * 2.0;
        } else if (smooth_follow) {
            *sf_params = sticky_params;
            bool widescreen = state()->sbs_mode_enabled && is_gamescope_reshade_ipc_connected();
            float threshold = sf_params->lower_angle_threshold;
            if (sf_config->sideview_follow_threshold) threshold = sf_config->sideview_follow_threshold;
            float display_size = fmax(1.0, sf_config->sideview_display_size * (widescreen ? 2.0 : 1.0));

            // this calculation tends to fall short for sizes > 1.0, so increase by 25%
            threshold += half_fov * (display_size  - 1.0) * 1.25;

            threshold = fmax(sf_params->lower_angle_threshold, threshold);

            sf_params->lower_angle_threshold = threshold;
            sf_params->upper_angle_threshold = threshold;
            sf_params->return_to_angle = threshold;
        } else if (breezy_desktop_follow) {
            *sf_params = sticky_params;
            display_distance = 1.0;
            float threshold = sf_params->lower_angle_threshold;
            if (state()->breezy_desktop_display_distance) display_distance = state()->breezy_desktop_display_distance;
            if (state()->breezy_desktop_follow_threshold) threshold = state()->breezy_desktop_follow_threshold;
            threshold += half_fov * (1.0 / display_distance - 1.0);
            threshold = fmax(sf_params->lower_angle_threshold, threshold);
            
            sf_params->lower_angle_threshold = threshold;
            sf_params->upper_angle_threshold = threshold;
            sf_params->return_to_angle = threshold;
        }
    }
    device_checkin(device);

    was_sbs_mode_enabled = state()->sbs_mode_enabled;
    bool was_smooth_follow_enabled = smooth_follow_enabled;
    smooth_follow_enabled = false;
    if (is_smooth_follow_granted() && (virtual_display_follow || smooth_follow)) {
        smooth_follow_enabled = true;
    } else if (is_productivity_granted() && breezy_desktop_follow) {
        smooth_follow_enabled = true;
        if (!was_smooth_follow_enabled) {
            // we'll capture the screen center on the next modify_screen_center call, before it changes
            snap_back_to_center = true;
            start_snap_back_timestamp_ms = -1;
        }
    } else if (was_smooth_follow_enabled && origin_quat && snap_back_to_center) {
        // we'll want to return as close to the original center as possible
        *sf_params = sticky_params;
    }

    if (smooth_follow_enabled && !was_smooth_follow_enabled) {
        last_timestamp_ms = -1;
        *sf_params = init_params;
        follow_state = FOLLOW_STATE_INIT;
    }
}

void smooth_follow_set_config_func(void* config) {
    if (!config) return;
    smooth_follow_config* temp_config = (smooth_follow_config*) config;

    if (sf_config) {
        if (temp_config->virtual_display_enabled && (
                sf_config->virtual_display_follow_enabled != temp_config->virtual_display_follow_enabled ||
                sf_config->virtual_display_enabled != temp_config->virtual_display_enabled))
            log_message("Virtual display follow has been %s\n", temp_config->virtual_display_follow_enabled ? "enabled" : "disabled");

        if (temp_config->sideview_enabled && 
                sf_config->sideview_follow_enabled != temp_config->sideview_follow_enabled ||
            sf_config->sideview_enabled != temp_config->sideview_enabled && temp_config->sideview_follow_enabled)
            log_message("Sideview follow has been %s\n", temp_config->sideview_follow_enabled ? "enabled" : "disabled");

        if (temp_config->sideview_follow_threshold != sf_config->sideview_follow_threshold)
            log_message("Sideview follow threshold has been changed to %f\n", temp_config->sideview_follow_threshold);

        free(sf_config);
    }

    sf_config = temp_config;
    update_smooth_follow_params();
}

uint32_t follow_wait_time_start_ms = -1;
follow_state_type next_state_for_angle(float angle_degrees) {
    if (follow_state == FOLLOW_STATE_INIT) {
        // double the return-to-angle so the movement will be more aggressive towards the center than usual
        if (!isnan(angle_degrees) && angle_degrees < sf_params->return_to_angle * 2.0) {
            follow_state = FOLLOW_STATE_NONE;
            update_smooth_follow_params();
        }
    } else {
        if (isnan(angle_degrees) || angle_degrees < sf_params->return_to_angle ||
            follow_state != FOLLOW_STATE_SLERPING && angle_degrees < sf_params->lower_angle_threshold) {
            follow_state = FOLLOW_STATE_NONE;
        } else if (angle_degrees > sf_params->lower_angle_threshold &&
                angle_degrees < sf_params->upper_angle_threshold &&
                sf_params->delay_ms > 0) {
            if (follow_state == FOLLOW_STATE_NONE) {
                follow_state = FOLLOW_STATE_WAITING;
                follow_wait_time_start_ms = get_time_ms();
            } else if (follow_state == FOLLOW_STATE_WAITING) {
                if (get_time_ms() - follow_wait_time_start_ms > sf_params->delay_ms) {
                    follow_state = FOLLOW_STATE_SLERPING;
                }
            }
        } else {
            follow_state = FOLLOW_STATE_SLERPING;
        }
    }

    return follow_state;
}

float percent_adjust(float multiplier, float percent, bool inverse) {
    float percent_compliment = 1.0 - percent;
    if (!inverse) return multiplier * percent_compliment + percent;

    float multiplier_compliment = 1.0 - multiplier;
    return 1.0 - (multiplier_compliment * percent_compliment + percent); 
}

// ported and modified from https://github.com/g-truc/glm/blob/master/glm/ext/quaternion_common.inl
// if slerping threshold is met, this returns a quaternion that's "a" percent of the way between "from" and "to"
imu_quat_type slerp(imu_quat_type from, imu_quat_type to, float a) {
    imu_quat_type target = to;
    if (origin_quat && smooth_follow_enabled) {
        // origin_quat helps us understand the user's frame of reference, so by adjusting
        // the target, we can modify it from that frame of reference and get more accurate axis adjustments
        imu_quat_type to_rel = multiply_quaternions(conjugate(*origin_quat), to);
        imu_euler_type to_euler = quaternion_to_euler_zyx(to_rel);
        
        // ignore tracking preferences if smooth_follow_enabled is false, since we want to fully return to
        // the original center
        imu_euler_type target_euler;
        target_euler.roll = (sf_config->track_roll || !smooth_follow_enabled) ? to_euler.roll : 0.0;
        target_euler.pitch = (sf_config->track_pitch || !smooth_follow_enabled) ? to_euler.pitch : 0.0;
        target_euler.yaw = (sf_config->track_yaw || !smooth_follow_enabled) ? to_euler.yaw : 0.0;

        // the result is relative to origin_quat, so we need to reapply it
        target = multiply_quaternions(*origin_quat, euler_to_quaternion_zyx(target_euler));
    }

    float cosTheta = from.w * target.w + from.x * target.x + from.y * target.y + from.z * target.z;
    if (cosTheta < 0) {
        imu_quat_type tmp = {
            .w = -target.w,
            .x = -target.x,
            .y = -target.y,
            .z = -target.z
        };
        target = tmp;
        cosTheta = -cosTheta;
    }
    float half_angle = acos(cosTheta);
    follow_state_type next_state = next_state_for_angle(radian_to_degree(2 * half_angle));
    if (next_state == FOLLOW_STATE_SLERPING || next_state == FOLLOW_STATE_INIT) {
        // our return-to-angle gives us a margin around the center of the target, and we want to stop when
        // we hit that margin, not the center. if we don't factor that margin into the target, then we may still
        // be slerping pretty quickly when we hit the return-to-angle, then stop very abruptly. the below factors
        // return-to-angle into the target so we accelerate very smoothly to the edge of that margin.

        // target 95% of the return-to-angle, otherwise it becomes an asymptote and we'll never stop slerping
        float target_half_angle = degree_to_radian(sf_params->return_to_angle * 0.95) / 2.0;

        // how much of the current angle is the target angle, 100% means we're already at the desired return-to-angle
        float target_percent = target_half_angle / half_angle;

        // half of the remaining angle to the target
        half_angle -= target_half_angle;

        float a_compliment = 1 - a;
        float sin_of_angle = sin(half_angle);
        float from_weight = percent_adjust(sin(a_compliment * half_angle) / sin_of_angle, target_percent, false);
        float target_weight = percent_adjust(sin(a * half_angle) / sin_of_angle, target_percent, true);
        imu_quat_type result = {
            .w = from_weight * from.w + target_weight * target.w,
            .x = from_weight * from.x + target_weight * target.x,
            .y = from_weight * from.y + target_weight * target.y,
            .z = from_weight * from.z + target_weight * target.z
        };

        return result;
    }

    return from;
}

imu_buffer_type *smooth_follow_imu_buffer = NULL;
imu_quat_type smooth_follow_modify_screen_center_func(uint32_t timestamp_ms, imu_quat_type quat, imu_quat_type screen_center) {
    if (!smooth_follow_enabled && !origin_quat || !sf_params) {
        return screen_center;
    }

    if (last_timestamp_ms == -1) {
        last_timestamp_ms = timestamp_ms;
        return screen_center;
    }

    if (follow_state == FOLLOW_STATE_NONE) {
        last_timestamp_ms = timestamp_ms;
    }

    uint32_t elapsed_ms = timestamp_ms - last_timestamp_ms;
    last_timestamp_ms = timestamp_ms;

    // apps using snapback also want to track distance from the origin,
    // we could use a different config boolean for this if it ever needs to be untied from snapback
    if (origin_quat && snap_back_to_center) {
        if (!smooth_follow_imu_buffer) {
            device_properties_type* device = device_checkout();
            if (device != NULL) smooth_follow_imu_buffer = create_imu_buffer(device->imu_buffer_size);
            device_checkin(device);
        }

        if (smooth_follow_imu_buffer) {
            // capture how far we are from the original screen center
            imu_buffer_response_type* response = push_to_imu_buffer(
                smooth_follow_imu_buffer, 
                multiply_quaternions(conjugate(*origin_quat), quat),
                timestamp_ms
            );
            state()->smooth_follow_origin_ready = response && response->ready;
            if (state()->smooth_follow_origin_ready) {
                memcpy(state()->smooth_follow_origin, response->data, sizeof(float) * 16);
            }
            free(response);
        }
    }

    // smooth follow has been disabled, slerp the screen back to its original center
    if (!smooth_follow_enabled) {
        if (origin_quat && snap_back_to_center) {
            if (start_snap_back_timestamp_ms == -1) {
                start_snap_back_timestamp_ms = timestamp_ms;
            }
            uint32_t elapsed_snap_back_ms = timestamp_ms - start_snap_back_timestamp_ms;
            imu_quat_type result = slerp(screen_center, *origin_quat, 1 - pow(1 - sf_params->interpolation_ratio_ms, elapsed_ms));
            
            // our return-to-angle is so small it will never hit the target, kill the snap-back slerp after 2 seconds
            if (follow_state == FOLLOW_STATE_NONE || elapsed_snap_back_ms > 2000) {
                follow_state = FOLLOW_STATE_NONE;
                result = *origin_quat;

                // we've reached our destination, clear this out to stop slerping
                free_and_clear(&origin_quat);
                snap_back_to_center = false;

                free_and_clear(&state()->smooth_follow_origin);
                state()->smooth_follow_origin_ready = false;

                start_snap_back_timestamp_ms = -1;
            }

            return result;
        } else {
            free_and_clear(&origin_quat);
        }

        return screen_center;
    } else if (!origin_quat) {
        // smooth follow was just enabled, capture the origin before it changes
        origin_quat = calloc(1, sizeof(imu_quat_type));
        *origin_quat = screen_center;

        if (snap_back_to_center) {
            if (!state()->smooth_follow_origin) state()->smooth_follow_origin = calloc(16, sizeof(float));
            state()->smooth_follow_origin_ready = false;
        }
    }

    return slerp(screen_center, quat, 1 - pow(1 - sf_params->interpolation_ratio_ms, elapsed_ms));
}

// unlike gaming, smooth follow for desktop is a temporary state, handled via control flags rather than persistent config
void smooth_follow_handle_control_flag_line_func(char* key, char* value) {
    if (is_productivity_granted() && sf_config->breezy_desktop_enabled) {
        bool was_enabled = state()->breezy_desktop_smooth_follow_enabled == true;
        if (equal(key, "enable_breezy_desktop_smooth_follow")) {
            boolean_config(key, value, &state()->breezy_desktop_smooth_follow_enabled);
        } else if (equal(key, "toggle_breezy_desktop_smooth_follow")) {
            state()->breezy_desktop_smooth_follow_enabled = !state()->breezy_desktop_smooth_follow_enabled;
        }

        // these will be applied to the thresholds on the next call to update_smooth_follow_params()
        if (equal(key, "breezy_desktop_follow_threshold")) {
            float_config(key, value, &state()->breezy_desktop_follow_threshold);
        }
        if (equal(key, "breezy_desktop_display_distance")) {
            float_config(key, value, &state()->breezy_desktop_display_distance);
        }

        if (was_enabled != state()->breezy_desktop_smooth_follow_enabled)
            log_message("Breezy Desktop follow has been %s\n", state()->breezy_desktop_smooth_follow_enabled ? "enabled" : "disabled");
        
        update_smooth_follow_params();
    }
}

static void smooth_follow_handle_state_func() {
    if (was_sbs_mode_enabled != state()->sbs_mode_enabled) {
        update_smooth_follow_params();
    }
}

static void handle_device_disconnect() {
    snap_back_to_center = false;
    last_timestamp_ms = -1;
    follow_state = FOLLOW_STATE_NONE;
    follow_wait_time_start_ms = -1;
    start_snap_back_timestamp_ms = -1;

    free_and_clear(&origin_quat);
    free_and_clear(&state()->smooth_follow_origin);
    state()->smooth_follow_origin_ready = false;
}

const plugin_type smooth_follow_plugin = {
    .id = "smooth_follow",
    .default_config = smooth_follow_default_config_func,
    .handle_config_line = smooth_follow_handle_config_line_func,
    .handle_control_flag_line = smooth_follow_handle_control_flag_line_func,
    .set_config = smooth_follow_set_config_func,
    .handle_state = smooth_follow_handle_state_func,
    .handle_ipc_change = update_smooth_follow_params,
    .modify_screen_center = smooth_follow_modify_screen_center_func,
    .handle_device_connect = update_smooth_follow_params,
    .handle_device_disconnect = handle_device_disconnect
};