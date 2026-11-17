#pragma once
#include "TrainerCore.h"
#include <locale>
#include <ostream>
#include <iomanip>
#include <limits>

namespace MC3Forge
{
inline void write_dynamo(std::ostream& output, const DynamoReport& t)
{
    output << "{\"source_mask\":" << t.source_mask
        << ",\"geometry_valid\":" << (t.geometry_valid ? "true" : "false")
        << ",\"nitro_valid\":" << (t.nitro_valid ? "true" : "false")
        << ",\"eligible\":" << (t.eligible ? "true" : "false")
        << ",\"enabled\":" << (t.enabled ? "true" : "false")
        << ",\"grant_possible\":" << (t.grant_possible ? "true" : "false")
        << ",\"counter_boundary_safe\":" << (t.counter_boundary_safe ? "true" : "false")
        << ",\"ee_pc\":" << t.ee_pc
        << ",\"input_suppressed\":" << (t.input_suppressed ? "true" : "false")
        << ",\"handbrake_suppressed\":" << (t.handbrake_suppressed ? "true" : "false")
        << ",\"infinite\":" << (t.infinite ? "true" : "false")
        << ",\"active\":" << (t.active ? "true" : "false")
        << ",\"fault\":" << (t.fault ? "true" : "false")
        << ",\"maximum\":" << t.maximum << ",\"remaining\":" << t.remaining
        << ",\"refill_limit\":" << t.refill_limit << ",\"controller\":" << t.controller
        << ",\"fraction\":" << t.fraction << ",\"wheels\":" << t.wheels << ",\"contacts\":" << t.contacts
        << ",\"forward\":" << t.forward << ",\"lateral\":" << t.lateral << ",\"slip_degrees\":" << t.slip_degrees
        << ",\"lateral_threshold\":" << t.lateral_threshold << ",\"required_seconds\":" << t.required_seconds
        << ",\"progress\":" << t.progress << ",\"clock\":" << t.clock << ",\"dt\":" << t.dt
        << ",\"qualifying_seconds\":" << t.qualifying_seconds << ",\"grants\":" << t.grants
        << ",\"manual_grants\":" << t.manual_grants << ",\"episodes\":" << t.episodes
        << ",\"activations\":" << t.activations << ",\"spent\":" << t.spent;
    output << ",\"progress_resets\":" << t.progress_resets << ",\"last_reset_reason\":\"" << t.last_reset_reason << "\"";
    const std::array<const std::array<float,3>*,3> arrays{&t.velocity,&t.right,&t.forward_basis};
    constexpr std::array<const char*,3> names{"velocity","right","forward_basis"};
    for (unsigned i=0; i<arrays.size(); ++i)
    { const auto& a=*arrays[i]; output << ",\"" << names[i] << "\":[" << a[0] << ',' << a[1] << ',' << a[2] << ']'; }
    output << ",\"reason\":\"" << t.reason << "\"}";
}

inline void write_drivetrain(std::ostream& output, const RuntimeTrace& t)
{
    output << "{\"source_verified\":" << (t.source_verified ? "true" : "false")
        << ",\"clock_source_verified\":" << (t.clock_source_verified ? "true" : "false")
        << ",\"source_body_mask\":" << t.source_body_mask
        << ",\"engine_available\":" << (t.engine_available ? "true" : "false")
        << ",\"transmission_available\":" << (t.transmission_available ? "true" : "false")
        << ",\"engine_config_matches\":" << (t.engine_config_matches ? "true" : "false")
        << ",\"transmission_config_matches\":" << (t.transmission_config_matches ? "true" : "false")
        << ",\"runtime_engine_address\":" << t.runtime_engine_address << ",\"runtime_config_address\":" << t.runtime_config_address
        << ",\"selected_config_address\":" << t.selected_config_address << ",\"engine_type\":" << t.engine_type
        << ",\"transmission_type\":" << t.transmission_type;
    constexpr std::array<const char*,RuntimeTrace::FieldCount> names{"consumed_config_power", "engine_angular_speed", "output_power_signal", "engine_torque_blend", "effective_throttle", "launch_boost_timer", "engine_scale_candidate", "shift_cut_timer", "max_angular_speed_cache", "optimal_angular_speed_cache", "idle_angular_speed_cache", "curve_high", "curve_scale", "curve_shape_a", "curve_shape_b", "consumed_boost_strength", "consumed_boost_duration", "consumed_inertia", "consumed_engage_rpm", "torque_feedback_candidate"};
    for (unsigned i = 0; i < names.size(); ++i) output << ",\"" << names[i] << "\":" << t.engine[i];
    output << ",\"shaft_available\":" << (t.shaft_available ? "true" : "false")
        << ",\"shaft_type\":" << t.shaft_type << ",\"shaft_gate_candidate\":" << t.shaft_gate_candidate
        << ",\"shaft_speed_candidate\":" << t.shaft_speed_candidate
        << ",\"gear_index\":" << t.gear << ",\"gear_count\":" << t.gears << ",\"gear_ratio\":" << t.ratio
        << ",\"auto_mode\":" << t.auto_mode << ",\"shift_state_candidate\":" << t.shift_state
        << ",\"transmission_gate_byte\":" << t.transmission_gate << ",\"engine_flags_word\":" << t.engine_flags
        << ",\"shift_elapsed\":" << t.shift_elapsed << ",\"shift_interval\":" << t.shift_interval
        << ",\"model_available\":" << (t.model_available ? "true" : "false")
        << ",\"model_is_estimate\":true,\"boost_predicate\":" << (t.boost_predicate ? "true" : "false")
        << ",\"above_max_angular_speed\":" << (t.above_max_omega ? "true" : "false")
        << ",\"getter_power_model\":" << t.getter_model << ",\"drive_torque_model\":" << t.drive_model
        << ",\"coast_torque_model\":" << t.coast_model << ",\"blended_torque_model\":" << t.blend_model;
    constexpr std::array<const char*,6> clocks{"simulation_clock_candidate", "physics_dt_candidate", "scaled_dt_candidate", "raw_dt_candidate", "host_dt_candidate", "time_scale_candidate"};
    for (unsigned i = 0; i < clocks.size(); ++i) output << ",\"" << clocks[i] << "\":" << t.clocks[i];
    output << ",\"clocks_valid\":" << (t.clocks_valid ? "true" : "false") << ",\"clock_flags\":[";
    for (unsigned i = 0; i < t.clock_flags.size(); ++i) { if (i) output << ','; output << t.clock_flags[i]; }
    output << "],\"installed_upgrade_bytes_candidate\":[";
    for (unsigned i = 0; i < t.installed_upgrade_bytes.size(); ++i) { if (i) output << ','; output << t.installed_upgrade_bytes[i]; }
    output << "],\"wheel_inputs_available\":" << (t.wheel_inputs_available ? "true" : "false")
        << ",\"wheel_condition_scalars_candidate\":[";
    for (unsigned i = 0; i < t.wheel_input_count; ++i) { if (i) output << ','; output << t.wheel_condition_scalars[i]; }
    output << "],\"wheel_condition_flags_candidate\":[";
    for (unsigned i = 0; i < t.wheel_input_count; ++i) { if (i) output << ','; output << t.wheel_condition_flags[i]; }
    output << "],\"input_available\":" << (t.input_available ? "true" : "false")
        << ",\"buttons_active_low\":" << t.buttons_active_low << ",\"button_pressures\":[";
    for (unsigned i = 0; i < t.button_pressures.size(); ++i) { if (i) output << ','; output << t.button_pressures[i]; }
    output << "],\"stick_axes\":[";
    for (unsigned i = 0; i < t.stick_axes.size(); ++i) { if (i) output << ','; output << t.stick_axes[i]; }
    output << "],\"physics_position_valid\":" << (t.physics_position_valid ? "true" : "false")
        << ",\"physics_position\":[" << t.physics_position[0] << ',' << t.physics_position[1] << ',' << t.physics_position[2]
        << "],\"motion_valid\":" << (t.motion_valid ? "true" : "false")
        << ",\"clock_delta\":" << t.clock_delta << ",\"distance_delta\":" << t.distance_delta
        << ",\"speed_native_per_clock_unit\":" << t.speed_per_clock_unit << '}';
}
// Shared by the actual runtime and the grouped-locale regression harness.
inline void write_report(std::ostream& output, const Report& r, unsigned source_mask, bool owned)
{
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "{\"diagnostic\":true,\"code_checks_mask\":" << source_mask << ",\"raw_mode\":" << r.raw_mode << ",\"race_type\":" << r.race_type << ",\"state\":" << r.state << ",\"rank\":" << r.rank
        << ",\"base\":" << r.base << ",\"current\":" << r.current << ",\"percent\":" << r.bonus
        << ",\"requested_percent\":" << r.requested_percent
        << ",\"effects_requested\":" << (r.effects_requested ? "true" : "false")
        << ",\"driving\":" << (r.driving ? "true" : "false")
        << ",\"cruising\":" << (r.cruising ? "true" : "false")
        << ",\"loot_revision\":" << r.loot_revision << ",\"inventory_count\":" << r.inventory_count
        << ",\"loot_power\":" << r.loot_power << ",\"using_loot\":" << (r.using_loot ? "true" : "false")
        << ",\"loot_item\":\"" << r.loot_item << "\",\"loot_error\":\"" << r.loot_error << "\""
        << ",\"loot_valid\":" << (r.loot_valid ? "true" : "false")
        << ",\"inventory_ready\":" << (r.inventory_ready ? "true" : "false")
        << ",\"dynamo_charge_scale\":" << r.dynamo_charge_scale
        << ",\"dynamo_equipped\":" << (r.dynamo_equipped ? "true" : "false")
        << ",\"dynamo_item\":\"" << r.dynamo_item << "\""
        << ",\"equipped\":" << (owned ? "true" : "false")
        << ",\"applied\":" << (r.applied ? "true" : "false")
        << ",\"fault\":" << (r.fault ? "true" : "false")
        << ",\"completed\":" << r.completed << ",\"applications\":" << r.applications
        << ",\"restorations\":" << r.restorations << ",\"racing_vsyncs\":" << r.racing_vsyncs
        << ",\"boosted_vsyncs\":" << r.boosted_vsyncs << ",\"peak_power\":" << r.peak_power
        << ",\"cruising_vsyncs\":" << r.cruising_vsyncs
        << ",\"trace_rows\":" << r.trace_rows << ",\"trace_limit_reached\":" << (r.trace_limit_reached ? "true" : "false")
        << ",\"trace_io_failed\":" << (r.trace_io_failed ? "true" : "false")
        << ",\"ee_cache_enabled\":" << (r.ee_cache_enabled ? "true" : "false")
        << ",\"ram_mapping_identity\":" << (r.ram_mapping_identity ? "true" : "false")
        << ",\"memory_view_ok\":" << (r.memory_view_ok ? "true" : "false")
        << ",\"position_valid\":" << (r.position_valid ? "true" : "false")
        << ",\"position\":[" << r.position[0] << ',' << r.position[1] << ',' << r.position[2]
        << "],\"car\":\"" << r.car << "\",\"reason\":\"" << r.reason
        << "\",\"mechanics_valid\":" << (r.mechanics_valid ? "true" : "false") << ",\"mechanics\":[";
    for (unsigned i = 0; i < r.mechanics.size(); ++i) { if (i) output << ','; output << r.mechanics[i]; }
    output << "],\"drivetrain\":";
    write_drivetrain(output, r.drivetrain);
    output << ",\"dynamo\":";
    write_dynamo(output, r.dynamo);
    output << ",\"money\":{\"attempt\":" << r.money.attempt << ",\"credits\":" << r.money.credits
        << ",\"before\":" << r.money.before << ",\"native_after\":" << r.money.native_after
        << ",\"base\":" << r.money.base << ",\"bonus\":" << r.money.bonus << ",\"after\":" << r.money.after
        << ",\"permille\":" << r.money.permille << ",\"source_mask\":" << r.money.source_mask << ",\"fault\":" << (r.money.fault ? "true" : "false")
        << ",\"reason\":\"" << r.money.reason << "\"}";
    output << "}\n";
}
}
