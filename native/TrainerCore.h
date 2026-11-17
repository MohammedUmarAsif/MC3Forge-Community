// Original MC3Forge diagnostic. Integration inherits PCSX2's GPL-3.0-or-later.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace MC3Forge
{
struct Memory
{
    std::uint8_t* bytes;
    std::size_t size;
    bool contains(std::uint32_t a, std::size_t n) const
    { return a >= 0x100000 && n <= size && a <= size - n; }
    bool object(std::uint32_t a, std::size_t n) const
    { return a % 4 == 0 && contains(a, n); }
    std::uint32_t word(std::uint32_t a) const
    {
        std::uint32_t v = 0;
        if (a % 4 == 0 && contains(a, 4)) std::memcpy(&v, bytes + a, 4);
        return v;
    }
    float scalar(std::uint32_t a) const
    { const auto v = word(a); float f; std::memcpy(&f, &v, 4); return f; }
    bool put(std::uint32_t a, float f)
    {
        if (a % 4 || !contains(a, 4) || !std::isfinite(f)) return false;
        std::memcpy(bytes + a, &f, 4); return scalar(a) == f;
    }
    std::string car(std::uint32_t record) const
    {
        if (!contains(record, 0x104)) return {};
        std::string value;
        for (unsigned i = 0; i < 20; ++i)
        {
            const char c = static_cast<char>(bytes[record + 0xDF + i]);
            if (!c) break;
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return {};
            value.push_back(c);
        }
        return value.rfind("vp_", 0) == 0 ? value : std::string{};
    }
};

struct Actor
{
    std::uint32_t model = 0, context = 0, owner = 0, sim = 0, engine = 0;
    std::string car;
    bool same(const Actor& b) const
    { return model == b.model && context == b.context && owner == b.owner && sim == b.sim && engine == b.engine && car == b.car; }
};

inline bool actor(const Memory& m, Actor& out)
{
    const auto mode = m.word(0x619B10), table = m.word(0x61B1E0);
    if (!m.object(mode, 0x400) || !m.object(table, 12) || m.word(mode + 0x4C) != 1 ||
        (m.bytes[mode + 0x108] && m.word(mode + 0x3D8) != 0) || m.word(table) != 0x634B68) return false;
    const auto player = m.word(table + 8);
    if (!m.object(player, 0x18) || m.word(player + 0xC) != 0x634AC8) return false;
    out.context = m.word(player + 0x14); out.model = m.word(out.context + 8);
    if (!m.object(out.context, 0x68) || !m.object(out.model, 0xD4) || m.word(out.model + 4) != out.context) return false;
    const auto descriptor = m.word(out.model + 0xD0);
    out.owner = m.word(out.context + 4); out.sim = m.word(out.owner + 0xB4);
    if (!m.object(descriptor, 0x2C) || !m.object(out.owner, 0xB8) || !m.object(out.sim, 0x84) ||
        m.word(descriptor + 0x28) != 0x635938 || m.word(out.owner) != 0x6278F8 || m.word(out.sim) != 0x6278C8) return false;
    out.engine = m.word(out.sim + 0x5C);
    if (!m.object(out.engine, 40) || m.word(m.word(out.sim + 0x54)) != 0x6253A8 || m.word(m.word(out.sim + 0x58)) != 0x6253A8) return false;
    out.car = m.car(m.word(out.context + 0x64));
    const auto profile = m.word(0x619B14);
    if (!m.object(profile, 0x124) || out.car.empty() || m.car(m.word(profile + 0x120)) != out.car || !m.contains(0x6E08FC, 4)) return false;
    const unsigned count = m.bytes[0x6E08FC], selected = m.bytes[0x6E08FD];
    return count > 0 && count <= 94 && selected < count && m.car(0x6E0900 + selected * 0x104) == out.car;
}

// One scan on admission, not per frame. A shared simulation configuration refuses.
inline bool exclusive_engine(const Memory& m, const Actor& a)
{
    for (std::uint32_t p = 0x100000; m.contains(p, 0x64); p += 4)
        if (p != a.sim && m.word(p) == 0x6278C8 && m.word(p + 0x5C) == a.engine) return false;
    return true;
}

struct DynamoReport
{
    unsigned source_mask = 0, wheels = 0, contacts = 0;
    bool geometry_valid = false, nitro_valid = false, eligible = false;
    bool input_suppressed = false, handbrake_suppressed = false, infinite = false;
    bool active = false, enabled = false, grant_possible = false;
    bool counter_boundary_safe = true;
    std::uint32_t ee_pc = 0;
    int maximum = 0, remaining = 0, refill_limit = 0;
    std::uint32_t controller = 0;
    std::array<float,3> velocity{}, right{}, forward_basis{};
    float forward = 0, lateral = 0, slip_degrees = 0, fraction = 0;
    float lateral_threshold = 5, required_seconds = 2, progress = 0;
    double clock = 0, dt = 0, qualifying_seconds = 0;
    unsigned grants = 0, manual_grants = 0, episodes = 0, activations = 0, spent = 0;
    unsigned progress_resets = 0;
    std::string last_reset_reason;
    bool fault = false;
    std::string reason = "Waiting for supported driving";
};

struct RuntimeTrace
{
    enum Field { ConfigPower, Omega, OutputPower, TorqueBlend, Throttle, BoostTimer,
        Scale, ShiftTimer, MaxOmega, OptOmega, IdleOmega, CurveHigh, CurveScale,
        CurveA, CurveB, BoostPower, BoostDuration, Inertia, EngageRPM, TorqueFeedback, FieldCount };
    bool source_verified = false, engine_available = false, transmission_available = false;
    bool engine_config_matches = false, transmission_config_matches = false;
    bool model_available = false, boost_predicate = false, above_max_omega = false;
    bool physics_position_valid = false, motion_valid = false;
    bool clocks_valid = false;
    bool clock_source_verified = false;
    unsigned source_body_mask = 0;
    std::uint32_t runtime_engine_address = 0, runtime_config_address = 0, selected_config_address = 0;
    std::uint32_t engine_type = 0, transmission_type = 0;
    std::array<float, FieldCount> engine{};
    std::array<float,6> clocks{};
    std::array<std::uint32_t,6> clock_flags{};
    std::array<std::uint32_t,13> installed_upgrade_bytes{};
    std::array<float,8> wheel_condition_scalars{};
    std::array<std::uint32_t,8> wheel_condition_flags{};
    unsigned wheel_input_count = 0;
    bool wheel_inputs_available = false;
    bool input_available = false;
    std::uint32_t buttons_active_low = 0;
    std::array<std::uint32_t,8> button_pressures{};
    std::array<std::uint32_t,4> stick_axes{};
    bool shaft_available = false;
    std::uint32_t shaft_type = 0, shaft_gate_candidate = 0;
    float shaft_speed_candidate = 0;
    std::array<float,3> physics_position{};
    float ratio = 0, shift_elapsed = 0, shift_interval = 0;
    int gear = -1, gears = 0;
    std::uint32_t auto_mode = 0, shift_state = 0, transmission_gate = 0, engine_flags = 0;
    double getter_model = 0, drive_model = 0, coast_model = 0, blend_model = 0;
    double clock_delta = 0, distance_delta = 0, speed_per_clock_unit = 0;
};

struct MoneyReport
{
    unsigned attempt=0, credits=0, before=0, native_after=0, base=0, bonus=0, after=0, permille=0, source_mask=0;
    bool fault=false;
    std::string reason="Money waiting for supported paid race";
};

struct Report
{
    float base = 0, current = 0, bonus = 0;
    float requested_percent = 0;
    bool driving = false, cruising = false, effects_requested = false;
    unsigned loot_revision = 0, inventory_count = 0, loot_power = 0;
    float dynamo_charge_scale=0;
    bool inventory_ready=false;
    bool using_loot = false;
    bool loot_valid = false, dynamo_equipped = false;
    std::string dynamo_item;
    std::string loot_item, loot_error;
    unsigned completed = 0, applications = 0, restorations = 0;
    bool equipped = false, applied = false, fault = false;
    int state = -1, rank = 0;
    std::uint32_t raw_mode = 0, race_type = 0;
    std::array<float, 13> mechanics{};
    bool mechanics_valid = false;
    unsigned racing_vsyncs = 0, boosted_vsyncs = 0;
    unsigned cruising_vsyncs = 0;
    float peak_power = 0;
    std::array<float,3> position{};
    bool position_valid = false;
    unsigned trace_rows = 0;
    bool trace_limit_reached = false;
    bool trace_io_failed = false;
    bool ee_cache_enabled = false, ram_mapping_identity = false, memory_view_ok = false;
    RuntimeTrace drivetrain;
    DynamoReport dynamo;
    MoneyReport money;
    std::string car, reason = "Waiting for supported race";
};

class Engine
{
    Actor bound;
    std::uint32_t race_seen = 0;
    float base = 0, applied = 0;
    bool active = false;
public:
    Report report;
    bool owned = false;
    void suspend(Memory& m)
    {
        report.bonus = 0;
        if (!active) return;
        Actor now;
        if (actor(m, now) && now.same(bound))
        {
            const auto value = m.scalar(bound.engine + 4);
            if (value != applied && value != base)
            { report.fault = true; report.reason = "Unexpected power; restart isolated game"; }
            else if (m.put(bound.engine + 4, base)) { ++report.restorations; report.current = base; report.reason = "Effect suspended; baseline restored"; }
            else { report.fault = true; report.reason = "Restoration readback failed"; }
        }
        else { report.fault = true; report.reason = "Owner lost; no stale write. Restart isolated game"; }
        active = false; report.applied = false;
    }
    void invalidate(Memory& m)
    { suspend(m); race_seen = 0; report.fault = true; report.reason = "Reset/state load: effect disabled until restart"; }
    void abandon()
    { active = false; race_seen = 0; report.applied = false; report.bonus = 0; report.driving = false; report.fault = true; report.reason = "Game code changed; no unqualified write"; }
    bool tick(Memory& m, bool armed, bool enable, float percent, bool aggregate=false)
    {
        bool delivered = false;
        report.driving = false; report.cruising = false;
        report.requested_percent = std::isfinite(percent) ? percent : 0;
        report.effects_requested = enable;
        report.position_valid = false; report.mechanics_valid = false;
        report.equipped = owned;
        const auto manager = m.word(0x619DB8), root = m.word(0x618AE0);
        const auto race = m.word(root + 4);
        report.raw_mode = m.word(0x619B00); report.race_type = m.word(race);
        const bool ordered = m.object(race, 0x12CC) && m.word(race) == 0x630770;
        const bool cruise = m.object(race, 4) && m.word(root) == 0x630258 && m.word(race) == 0x630120;
        if (report.raw_mode != 2 || !m.object(manager, 0x38) || !m.object(root, 8) ||
            m.word(manager) != 0x6304D8 || (!ordered && !cruise))
        { suspend(m); race_seen = 0; report.state = -1; return false; }
        const auto state = m.word(manager + 4), dispatch = m.word(manager + 0x34);
        if (state == 4 && dispatch == 4)
        { if (ordered) ++report.racing_vsyncs; else ++report.cruising_vsyncs; }
        report.state = static_cast<int>(state); report.rank = static_cast<int>(m.word(race + 0xA8));
        const bool finished = ordered && m.contains(race, 0x12CC) && m.bytes[race + 0x98] == 1;
        report.cruising = cruise;
        report.driving = state == 4 && dispatch == 4 && !finished;
        Actor observed;
        report.mechanics_valid = actor(m, observed);
        if (report.mechanics_valid)
        {
            report.position_valid = m.contains(observed.model + 0xC4, 12) && m.contains(observed.owner + 0x170, 12) &&
                std::memcmp(m.bytes + observed.model + 0xC4, m.bytes + observed.owner + 0x170, 12) == 0;
            if (report.position_valid)
            {
                std::memcpy(report.position.data(), m.bytes + observed.model + 0xC4, 12);
                for (const auto value : report.position) if (!std::isfinite(value)) report.position_valid = false;
            }
            if (!report.position_valid) report.position = {};
            const auto front = m.word(observed.sim + 0x54), rear = m.word(observed.sim + 0x58), nitro = m.word(observed.sim + 0x80);
            report.mechanics = {m.scalar(observed.engine), m.scalar(observed.engine + 20), m.scalar(observed.engine + 36), m.scalar(observed.engine + 32), m.scalar(observed.sim + 0x24), m.scalar(front + 52), m.scalar(front + 104), m.scalar(front + 108), m.scalar(rear + 52), m.scalar(rear + 104), m.scalar(rear + 108), m.scalar(nitro + 8), m.scalar(nitro + 12)};
            for (const auto value : report.mechanics) if (!std::isfinite(value)) report.mechanics_valid = false;
            if (!report.mechanics_valid) report.mechanics = {};
            if (!active)
            {
                const float value = m.scalar(observed.engine + 4);
                if (std::isfinite(value) && value > 0 && value <= 100000)
                { report.base = value; report.current = value; report.bonus = 0; report.car = observed.car; }
            }
        }
        if (ordered && report.driving && report.mechanics_valid) race_seen = race;
        if (!ordered) race_seen = 0;
        if (ordered && state == 7 && dispatch == 7 && finished && race_seen == race && report.mechanics_valid &&
            report.rank >= 1 && report.rank <= 8 && m.word(race + 0x12C0) >= 1 && (m.word(race + 0x12C8) & 1))
        {
            race_seen = 0; ++report.completed;
            if (armed && !owned && !report.fault) { owned = true; delivered = true; report.equipped = true; report.reason = "Test part equipped; next race applies"; }
        }
        if (!report.driving || !owned || !enable || report.fault)
        { suspend(m); return delivered; }
        if (!std::isfinite(percent) || percent < 0 || percent > (aggregate ? 800 : 100))
        { suspend(m); report.fault = true; report.reason = "Invalid diagnostic dose"; return delivered; }
        Actor now;
        if (!actor(m, now)) { suspend(m); report.reason = "Current car ownership unsupported"; return delivered; }
        if (active && !now.same(bound)) suspend(m);
        if (report.fault) return delivered;
        if (!active)
        {
            if (!exclusive_engine(m, now)) { report.reason = "Shared engine configuration refused"; return delivered; }
            base = m.scalar(now.engine + 4);
            if (!std::isfinite(base) || base <= 0 || base > 100000)
            { report.reason = "Invalid current-car baseline"; return delivered; }
            bound = now; applied = base; active = true;
        }
        const float current = m.scalar(bound.engine + 4);
        if (current != applied && current != base)
        { report.fault = true; report.reason = "Unexpected power; no overwrite"; suspend(m); return delivered; }
        const float target = base * (1.0f + percent / 100.0f);
        if (!std::isfinite(target) || target>100000)
        { suspend(m); report.fault=true; report.reason="Aggregate exceeds validated target range"; return delivered; }
        if (current != target)
        {
            if (!m.put(bound.engine + 4, target))
            { report.fault = true; report.reason = "Apply readback failed"; suspend(m); return delivered; }
            ++report.applications;
        }
        applied = target;
        report.base = base; report.current = m.scalar(bound.engine + 4); report.bonus = percent;
        report.applied = true; report.car = now.car; report.reason = "Current-car bonus active";
        if (percent > 0) ++report.boosted_vsyncs;
        if (report.current > report.peak_power) report.peak_power = report.current;
        return delivered;
    }
};
} // namespace MC3Forge
