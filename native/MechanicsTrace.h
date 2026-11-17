#pragma once
#include "TrainerCore.h"

namespace MC3Forge
{
// Approximate source arithmetic, not a hooked function return or bit-exact MIPS FP.
inline void torque_model(RuntimeTrace& t)
{
    using T = RuntimeTrace;
    t.model_available = false;
    if (!t.engine_available || !t.transmission_available) return;
    const auto& e = t.engine;
    const double omega = e[T::Omega], maximum = e[T::MaxOmega], optimal = e[T::OptOmega];
    if (optimal <= 0 || maximum <= optimal) return;
    t.getter_model = e[T::ConfigPower];
    t.boost_predicate = e[T::BoostTimer] > 0 && t.transmission_gate == 0;
    if (t.boost_predicate)
    {
        if (e[T::BoostDuration] <= 0) return;
        t.getter_model += e[T::BoostPower] * static_cast<double>(e[T::BoostTimer]) / e[T::BoostDuration];
    }
    const double polynomial = e[T::Scale] * t.getter_model * e[T::CurveScale] *
        (e[T::CurveA] * optimal - omega) * (e[T::CurveB] * optimal + omega);
    t.above_max_omega = omega > maximum;
    if (omega <= optimal) t.drive_model = polynomial;
    else if (omega <= maximum)
        t.drive_model = std::fmax(0.0, polynomial * (maximum - omega) * (omega + maximum - 2 * optimal) * e[T::CurveHigh]);
    else t.drive_model = 0;
    const double sign = e[T::IdleOmega] > omega ? 1 : (e[T::IdleOmega] < omega ? -1 : 0);
    t.coast_model = sign * ((e[T::Scale] * t.getter_model * 746.0) / optimal) * (omega / optimal);
    t.blend_model = e[T::Throttle] * t.drive_model + (1 - e[T::Throttle]) * t.coast_model;
    t.model_available = std::isfinite(t.getter_model) && std::isfinite(t.drive_model) &&
        std::isfinite(t.coast_model) && std::isfinite(t.blend_model);
    if (!t.model_available) t.getter_model = t.drive_model = t.coast_model = t.blend_model = 0;
}

inline RuntimeTrace capture_runtime(const Memory& m, bool source_verified, bool clock_verified = true)
{
    RuntimeTrace t; t.source_verified = source_verified; t.clock_source_verified = clock_verified;
    Actor a;
    if (!source_verified || !actor(m, a)) return t;
    const auto profile_config = m.word(m.word(0x619B14) + 0x120);
    for (unsigned i = 0; i < t.installed_upgrade_bytes.size(); ++i) t.installed_upgrade_bytes[i] = m.bytes[profile_config + 0x52 + i];
    const auto runtime = m.word(a.owner + 0x10);
    t.runtime_engine_address = runtime; t.engine_type = m.word(runtime); t.selected_config_address = a.engine;
    if (!m.contains(runtime, 0x60) || m.word(runtime) != 0x6253D8) return t;
    const auto config = m.word(runtime + 4);
    t.runtime_config_address = config;
    if (!m.contains(config, 40)) return t;
    t.engine_config_matches = config == a.engine;
    t.engine = {m.scalar(config + 4), m.scalar(runtime + 8), m.scalar(runtime + 0xC),
        m.scalar(runtime + 0x10), m.scalar(runtime + 0x18), m.scalar(runtime + 0x1C),
        m.scalar(runtime + 0x20), m.scalar(runtime + 0x30), m.scalar(runtime + 0x40),
        m.scalar(runtime + 0x44), m.scalar(runtime + 0x48), m.scalar(runtime + 0x50),
        m.scalar(runtime + 0x54), m.scalar(runtime + 0x58), m.scalar(runtime + 0x5C),
        m.scalar(config + 0x24), m.scalar(config + 0x20), m.scalar(config), m.scalar(config + 0x14), m.scalar(runtime + 0x14)};
    t.engine_available = true;
    for (const auto v : t.engine) if (!std::isfinite(v)) t.engine_available = false;
    if (t.engine[RuntimeTrace::ConfigPower] <= 0 || t.engine[RuntimeTrace::Inertia] <= 0) t.engine_available = false;
    if (!t.engine_available) { t.engine = {}; return t; }
    t.engine_flags = m.word(runtime + 0x2C);
    const auto shaft = m.word(runtime + 0x34);
    if (m.contains(shaft, 0x34))
    {
        t.shaft_type = m.word(shaft); t.shaft_gate_candidate = m.word(shaft + 0xC);
        t.shaft_speed_candidate = m.scalar(shaft + 0x30);
        t.shaft_available = std::isfinite(t.shaft_speed_candidate);
        if (!t.shaft_available) t.shaft_speed_candidate = 0;
    }
    const auto trans = m.word(runtime + 0x38), trans_config = m.word(trans + 4);
    t.transmission_type = m.word(trans);
    if (m.contains(trans, 0x24) && m.word(trans) == 0x625480 && m.contains(trans_config, 40))
    {
        t.transmission_config_matches = trans_config == m.word(a.sim + 0x60);
        const auto count = m.word(trans_config), gear = m.word(trans + 0x14);
        if (count >= 1 && count <= 12 && gear < count && m.contains(trans + 0x24, count * 4))
        {
            t.gears = static_cast<int>(count); t.gear = static_cast<int>(gear);
            t.ratio = m.scalar(trans + 0x24 + gear * 4);
            t.shift_elapsed = m.scalar(trans + 0x1C); t.shift_interval = m.scalar(trans_config + 0x24);
            t.auto_mode = m.word(trans + 0x10); t.shift_state = m.word(trans + 0x18);
            t.transmission_gate = m.bytes[trans + 0x20];
            t.transmission_available = std::isfinite(t.ratio) && std::isfinite(t.shift_elapsed) && std::isfinite(t.shift_interval);
            if (!t.transmission_available) t.ratio = t.shift_elapsed = t.shift_interval = 0;
            const auto wheel_group = m.word(trans + 0xC), count_wheels = m.word(wheel_group + 0x14);
            if (count_wheels >= 1 && count_wheels <= 8 && m.contains(wheel_group + 0x18, count_wheels * 4))
            {
                t.wheel_input_count = count_wheels; t.wheel_inputs_available = true;
                for (unsigned i = 0; i < count_wheels; ++i)
                {
                    const auto wheel = m.word(wheel_group + 0x18 + i * 4);
                    if (!m.contains(wheel, 0x174)) { t.wheel_inputs_available = false; break; }
                    t.wheel_condition_scalars[i] = m.scalar(wheel + 0x168);
                    t.wheel_condition_flags[i] = m.bytes[wheel + 0x171];
                    if (!std::isfinite(t.wheel_condition_scalars[i])) { t.wheel_inputs_available = false; break; }
                }
                if (!t.wheel_inputs_available) { t.wheel_input_count = 0; t.wheel_condition_scalars = {}; t.wheel_condition_flags = {}; }
            }
        }
    }
    if (clock_verified)
    {
        t.clocks = {m.scalar(0x70E524), m.scalar(0x618E20), m.scalar(0x618E24), m.scalar(0x618E54), m.scalar(0x618E58), m.scalar(0x618E50)};
        t.clocks_valid = true;
        for (auto& v : t.clocks) if (!std::isfinite(v)) { v = 0; t.clocks_valid = false; }
        t.clock_flags = {m.bytes[0x618E44], m.bytes[0x618E46], m.word(0x618E48), m.bytes[0x618E4C], m.bytes[0x618E4D], m.bytes[0x618E4E]};
    }
    const auto transform = m.word(a.context + 0x18);
    if (m.contains(transform + 0x34, 12))
    {
        std::memcpy(t.physics_position.data(), m.bytes + transform + 0x34, 12);
        t.physics_position_valid = true;
        for (const auto v : t.physics_position) if (!std::isfinite(v)) t.physics_position_valid = false;
        if (!t.physics_position_valid) t.physics_position = {};
    }
    torque_model(t); return t;
}

struct MotionTrace
{
    Actor previous_actor;
    std::array<float,3> previous_position{};
    double previous_clock = 0;
    bool valid = false;
    void update(const Memory& m, RuntimeTrace& t, bool racing)
    {
        Actor a;
        if (!racing || !t.physics_position_valid || !t.clocks_valid || !actor(m, a)) { valid = false; return; }
        if (valid && a.same(previous_actor))
        {
            t.clock_delta = t.clocks[0] - previous_clock;
            double squared = 0;
            for (unsigned i = 0; i < 3; ++i) { const double d = t.physics_position[i] - previous_position[i]; squared += d * d; }
            t.distance_delta = std::sqrt(squared);
            // Explicit sampling guards, not claimed MC3 movement or SI limits.
            if (t.clock_delta > 0 && t.clock_delta <= 1 && t.distance_delta <= 100)
            { t.speed_per_clock_unit = t.distance_delta / t.clock_delta; t.motion_valid = std::isfinite(t.speed_per_clock_unit); }
        }
        previous_actor = a; previous_clock = t.clocks[0]; previous_position = t.physics_position; valid = true;
    }
};
}
