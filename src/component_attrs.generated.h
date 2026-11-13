// GÉNÉRÉ — ne pas éditer. Source : stormworks-modding-kit/formats/component_physics_record.json
// Régénérer avec : python tools/gen_component_attrs.py
//
// 120 champs de component_physics_record (parseur 0x3168d0).
// Les largeurs viennent du kit, jamais d'une supposition : flags = 8 octets.
// isElement distingue un élément XML (vecteur) d'un attribut scalaire ;
// les 2 champs internes (max_item_capacity, lua_filename) ont isElement=false.
#pragma once
#include <cstddef>
#include <cstdint>

#include "../include/sw_sdk_p0.h"

inline constexpr SwComponentAttrInfo kComponentAttrs[] = {
    {"voxel_min", 0xC0u, 12, SW_ATTR_INT3, true},
    {"voxel_max", 0xCCu, 12, SW_ATTR_INT3, true},
    {"voxel_physics_min", 0xD8u, 12, SW_ATTR_INT3, true},
    {"voxel_physics_max", 0xE4u, 12, SW_ATTR_INT3, true},
    {"bb_physics_min", 0x11Cu, 12, SW_ATTR_FLOAT3, true},
    {"bb_physics_max", 0x128u, 12, SW_ATTR_FLOAT3, true},
    {"phys_collision_dampen", 0x134u, 4, SW_ATTR_FLOAT, false},
    {"compartment_sample_pos", 0x138u, 12, SW_ATTR_INT3, true},
    {"audio_filename_start", 0x148u, 16, SW_ATTR_STRING, false},
    {"audio_filename_loop", 0x158u, 16, SW_ATTR_STRING, false},
    {"audio_filename_end", 0x168u, 16, SW_ATTR_STRING, false},
    {"audio_filename_start_b", 0x1C0u, 16, SW_ATTR_STRING, false},
    {"audio_filename_loop_b", 0x1D0u, 16, SW_ATTR_STRING, false},
    {"audio_filename_end_b", 0x1E0u, 16, SW_ATTR_STRING, false},
    {"audio_gain", 0x238u, 4, SW_ATTR_FLOAT, false},
    {"name", 0x298u, 16, SW_ATTR_STRING, false},
    {"mass", 0x2B0u, 4, SW_ATTR_FLOAT, false},
    {"value", 0x2B4u, 4, SW_ATTR_INT, false},
    {"flags", 0x2B8u, 8, SW_ATTR_UINT64, false},
    {"reward_tier", 0x2E0u, 4, SW_ATTR_INT, false},
    {"tags", 0x2E8u, 16, SW_ATTR_STRING, false},
    {"revision", 0x310u, 4, SW_ATTR_INT, false},
    {"constraint_pos_parent", 0x340u, 12, SW_ATTR_FLOAT3, true},
    {"constraint_pos_child", 0x34Cu, 12, SW_ATTR_FLOAT3, true},
    {"voxel_location_child", 0x358u, 12, SW_ATTR_INT3, true},
    {"child_name", 0x368u, 16, SW_ATTR_STRING, false},
    {"extender_name", 0x378u, 16, SW_ATTR_STRING, false},
    {"constraint_range_of_motion", 0x38Cu, 4, SW_ATTR_FLOAT, false},
    {"max_motor_force", 0x390u, 4, SW_ATTR_FLOAT, false},
    {"max_motor_speed", 0x394u, 4, SW_ATTR_FLOAT, false},
    {"cable_radius", 0x398u, 4, SW_ATTR_FLOAT, false},
    {"cable_length", 0x39Cu, 4, SW_ATTR_FLOAT, false},
    {"mesh_data_name", 0x760u, 16, SW_ATTR_STRING, false},
    {"mesh_0_name", 0x770u, 16, SW_ATTR_STRING, false},
    {"mesh_1_name", 0x780u, 16, SW_ATTR_STRING, false},
    {"mesh_2_name", 0x790u, 16, SW_ATTR_STRING, false},
    {"mesh_editor_only_name", 0x7A0u, 16, SW_ATTR_STRING, false},
    {"seat_health_per_sec", 0x7FCu, 4, SW_ATTR_FLOAT, false},
    {"seat_offset", 0x800u, 12, SW_ATTR_FLOAT3, true},
    {"seat_front", 0x80Cu, 12, SW_ATTR_FLOAT3, true},
    {"seat_up", 0x818u, 12, SW_ATTR_FLOAT3, true},
    {"seat_camera", 0x824u, 12, SW_ATTR_FLOAT3, true},
    {"seat_render", 0x830u, 12, SW_ATTR_FLOAT3, true},
    {"buoy_radius", 0x83Cu, 4, SW_ATTR_FLOAT, false},
    {"buoy_factor", 0x840u, 4, SW_ATTR_FLOAT, false},
    {"buoy_force", 0x844u, 4, SW_ATTR_FLOAT, false},
    {"force_emitter_max_force", 0x848u, 4, SW_ATTR_FLOAT, false},
    {"force_emitter_max_vector", 0x84Cu, 4, SW_ATTR_FLOAT, false},
    {"force_dir", 0x850u, 12, SW_ATTR_FLOAT3, true},
    {"force_emitter_default_pitch", 0x85Cu, 4, SW_ATTR_INT, false},
    {"force_emitter_blade_height", 0x860u, 4, SW_ATTR_FLOAT, false},
    {"force_emitter_rotation_speed", 0x864u, 4, SW_ATTR_FLOAT, false},
    {"force_emitter_blade_physics_length", 0x868u, 4, SW_ATTR_FLOAT, false},
    {"force_emitter_blade_efficiency", 0x86Cu, 4, SW_ATTR_FLOAT, false},
    {"force_emitter_efficiency", 0x870u, 4, SW_ATTR_FLOAT, false},
    {"engine_max_force", 0x874u, 4, SW_ATTR_FLOAT, false},
    {"engine_frictionless_force", 0x878u, 4, SW_ATTR_FLOAT, false},
    {"wheel_wishbone_length", 0x880u, 4, SW_ATTR_FLOAT, false},
    {"wheel_suspension_height", 0x884u, 4, SW_ATTR_FLOAT, false},
    {"wheel_wishbone_margin", 0x888u, 4, SW_ATTR_FLOAT, false},
    {"wheel_suspension_offset", 0x88Cu, 4, SW_ATTR_FLOAT, false},
    {"wheel_wishbone_offset", 0x890u, 4, SW_ATTR_FLOAT, false},
    {"wheel_radius", 0x894u, 4, SW_ATTR_FLOAT, false},
    {"wheel_width", 0x898u, 4, SW_ATTR_FLOAT, false},
    {"light_position", 0xDA8u, 12, SW_ATTR_INT3, true},
    {"light_color", 0xDB4u, 12, SW_ATTR_FLOAT3, true},
    {"light_intensity", 0xDC0u, 4, SW_ATTR_FLOAT, false},
    {"light_range", 0xDC4u, 4, SW_ATTR_FLOAT, false},
    {"light_fov", 0xDC8u, 4, SW_ATTR_FLOAT, false},
    {"light_ies_map", 0xDD0u, 16, SW_ATTR_STRING, false},
    {"light_forward", 0xDE4u, 12, SW_ATTR_INT3, true},
    {"door_size", 0xDF0u, 12, SW_ATTR_FLOAT3, true},
    {"door_lower_limit", 0xDFCu, 4, SW_ATTR_FLOAT, false},
    {"door_upper_limit", 0xE00u, 4, SW_ATTR_FLOAT, false},
    {"door_flipped", 0xE04u, 1, SW_ATTR_BOOL, false},
    {"door_normal", 0xE0Cu, 12, SW_ATTR_INT3, true},
    {"door_side", 0xE18u, 12, SW_ATTR_INT3, true},
    {"door_up", 0xE24u, 12, SW_ATTR_INT3, true},
    {"door_side_dist", 0xE30u, 4, SW_ATTR_INT, false},
    {"door_up_dist", 0xE34u, 4, SW_ATTR_INT, false},
    {"door_base_pos", 0xE38u, 12, SW_ATTR_INT3, true},
    {"dynamic_body_position", 0xE44u, 12, SW_ATTR_INT3, true},
    {"dynamic_rotation_axes", 0xE50u, 12, SW_ATTR_FLOAT3, true},
    {"dynamic_side_axis", 0xE5Cu, 12, SW_ATTR_FLOAT3, true},
    {"dynamic_min_rotation", 0xE68u, 4, SW_ATTR_FLOAT, false},
    {"dynamic_max_rotation", 0xE6Cu, 4, SW_ATTR_FLOAT, false},
    {"logic_gate_subtype", 0xE74u, 4, SW_ATTR_INT, false},
    {"magnet_offset", 0xE80u, 12, SW_ATTR_FLOAT3, true},
    {"magnet_force", 0xE8Cu, 4, SW_ATTR_FLOAT, false},
    {"connector_axis", 0xE90u, 12, SW_ATTR_FLOAT3, true},
    {"connector_up", 0xE9Cu, 12, SW_ATTR_FLOAT3, true},
    {"rudder_surface_area", 0xEC4u, 4, SW_ATTR_FLOAT, false},
    {"pump_pressure", 0xEC8u, 4, SW_ATTR_FLOAT, false},
    {"particle_direction", 0xF08u, 12, SW_ATTR_INT3, true},
    {"particle_offset", 0xF14u, 12, SW_ATTR_FLOAT3, true},
    {"particle_bounds", 0xF20u, 12, SW_ATTR_FLOAT3, true},
    {"particle_speed", 0xF2Cu, 4, SW_ATTR_FLOAT, false},
    {"inventory_default_item", 0xF34u, 4, SW_ATTR_INT, false},
    {"electric_charge_capacity", 0x141Cu, 4, SW_ATTR_FLOAT, false},
    {"electric_magnitude", 0x1420u, 4, SW_ATTR_FLOAT, false},
    {"camera_fov_min", 0x2920u, 4, SW_ATTR_FLOAT, false},
    {"camera_fov_max", 0x2924u, 4, SW_ATTR_FLOAT, false},
    {"monitor_border", 0x2928u, 4, SW_ATTR_FLOAT, false},
    {"monitor_inset", 0x292Cu, 4, SW_ATTR_FLOAT, false},
    {"weapon_ammo_capacity", 0x293Cu, 4, SW_ATTR_INT, false},
    {"weapon_ammo_feed", 0x2C60u, 1, SW_ATTR_BOOL, false},
    {"weapon_barrel_length_voxels", 0x2C64u, 4, SW_ATTR_INT, false},
    {"weapon_breech_position", 0x2C68u, 12, SW_ATTR_INT3, true},
    {"weapon_breech_normal", 0x2C74u, 12, SW_ATTR_INT3, true},
    {"weapon_cart_position", 0x2C80u, 12, SW_ATTR_FLOAT3, true},
    {"weapon_cart_velocity", 0x2C8Cu, 12, SW_ATTR_FLOAT3, true},
    {"rx_range", 0x2C98u, 4, SW_ATTR_FLOAT, false},
    {"rx_length", 0x2C9Cu, 4, SW_ATTR_FLOAT, false},
    {"radar_range", 0x2CA4u, 4, SW_ATTR_FLOAT, false},
    {"radar_speed", 0x2CA8u, 4, SW_ATTR_FLOAT, false},
    {"rope_hook_offset", 0x2CACu, 12, SW_ATTR_FLOAT3, true},
    {"piston_len", 0x2CD8u, 4, SW_ATTR_FLOAT, false},
    {"piston_cam", 0x2CDCu, 4, SW_ATTR_FLOAT, false},
    {"max_item_capacity", 0x2CE4u, 4, SW_ATTR_INT, false},
    {"lua_filename", 0x2CE8u, 16, SW_ATTR_STRING, false},
};

inline constexpr size_t kComponentAttrCount = 120;
inline constexpr size_t kComponentAttrAttributeCount = 80;  // attributs XML
inline constexpr size_t kComponentAttrElementCount = 38;    // éléments XML (vecteurs)
inline constexpr size_t kComponentAttrInternalCount = 2;   // champs internes
inline constexpr size_t kComponentAttrMaxEnd = 0x2CF8u;       // fin du dernier champ
inline constexpr size_t kComponentAttrWidth1Count = 2;
inline constexpr size_t kComponentAttrWidth4Count = 62;
inline constexpr size_t kComponentAttrWidth8Count = 1;
inline constexpr size_t kComponentAttrWidth12Count = 38;
inline constexpr size_t kComponentAttrWidth16Count = 17;

// Largeur attendue d'un type scalaire/vecteur de la table.
constexpr uint8_t SwComponentAttrWidthOf(uint8_t type) {
    switch (type) {
        case SW_ATTR_FLOAT:  return 4;
        case SW_ATTR_INT:    return 4;
        case SW_ATTR_BOOL:   return 1;
        case SW_ATTR_STRING: return 16;
        case SW_ATTR_UINT64: return 8;
        case SW_ATTR_FLOAT3: return 12;
        case SW_ATTR_INT3:   return 12;
        default:             return 0;
    }
}

// Vrai pour un type que componentAttrRead/Write acceptent (scalaire numérique).
constexpr bool SwComponentAttrIsScalarNumeric(uint8_t type) {
    return type == SW_ATTR_FLOAT || type == SW_ATTR_INT ||
           type == SW_ATTR_BOOL || type == SW_ATTR_UINT64;
}

constexpr bool SwComponentAttrsConsistent() {
    for (size_t i = 0; i < kComponentAttrCount; ++i) {
        if (kComponentAttrs[i].width != SwComponentAttrWidthOf(kComponentAttrs[i].type))
            return false;
        if (kComponentAttrs[i].offset + kComponentAttrs[i].width > kComponentAttrMaxEnd)
            return false;
        if (i > 0 && kComponentAttrs[i - 1].offset + kComponentAttrs[i - 1].width >
                         kComponentAttrs[i].offset)
            return false;
    }
    return true;
}

constexpr size_t SwComponentAttrCountElements() {
    size_t count = 0;
    for (size_t i = 0; i < kComponentAttrCount; ++i)
        if (kComponentAttrs[i].isElement) ++count;
    return count;
}

constexpr size_t SwComponentAttrCountWidth(uint8_t width) {
    size_t count = 0;
    for (size_t i = 0; i < kComponentAttrCount; ++i)
        if (kComponentAttrs[i].width == width) ++count;
    return count;
}

static_assert(sizeof(kComponentAttrs) / sizeof(kComponentAttrs[0]) == kComponentAttrCount,
              "kComponentAttrs size does not match kComponentAttrCount");
static_assert(kComponentAttrAttributeCount + kComponentAttrElementCount +
                  kComponentAttrInternalCount == kComponentAttrCount,
              "attribute/element/internal counts do not add up");
static_assert(SwComponentAttrCountElements() == kComponentAttrElementCount,
              "element count mismatch");
static_assert(SwComponentAttrCountWidth(1) == kComponentAttrWidth1Count,
              "width 1 count mismatch");
static_assert(SwComponentAttrCountWidth(4) == kComponentAttrWidth4Count,
              "width 4 count mismatch");
static_assert(SwComponentAttrCountWidth(8) == kComponentAttrWidth8Count,
              "width 8 count mismatch");
static_assert(SwComponentAttrCountWidth(12) == kComponentAttrWidth12Count,
              "width 12 count mismatch");
static_assert(SwComponentAttrCountWidth(16) == kComponentAttrWidth16Count,
              "width 16 count mismatch");
static_assert(SwComponentAttrsConsistent(),
              "component attribute table is inconsistent");
