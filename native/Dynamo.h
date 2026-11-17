// Source-backed observation and explicitly experimental capped resource operation.
#pragma once
#include "TrainerCore.h"
#include <algorithm>

namespace MC3Forge
{
enum class DynamoMeter { Unequipped, Unavailable, BoostActive, Full, Charging, Waiting };
inline DynamoMeter dynamo_meter(const DynamoReport& d)
{
    if (!d.enabled) return DynamoMeter::Unequipped;
    if (!d.nitro_valid || !d.geometry_valid || d.fault) return DynamoMeter::Unavailable;
    if (d.active) return DynamoMeter::BoostActive;
    if (d.remaining>=d.maximum) return DynamoMeter::Full;
    return d.progress>0 ? DynamoMeter::Charging : DynamoMeter::Waiting;
}

inline int signed_half(const Memory& m, std::uint32_t address)
{
    std::int16_t value = 0;
    if (address % 2 == 0 && m.contains(address, 2)) std::memcpy(&value, m.bytes + address, 2);
    return value;
}

inline DynamoReport capture_dynamo(const Memory& m, unsigned source_mask)
{
    DynamoReport t; t.source_mask = source_mask;
    Actor a;
    if (!actor(m, a) || !m.object(a.owner, 0x184)) return t;
    const auto controller = m.word(a.owner + 0x180), config = m.word(controller + 4);
    if ((source_mask & 60) == 60 && m.object(controller, 0x24) && m.word(controller) == 0x6278A8 &&
        m.word(controller + 8) == a.context && config == m.word(a.sim + 0x80) && m.object(config, 16))
    {
        t.controller = controller; t.maximum = signed_half(m, controller + 0xE);
        t.refill_limit = signed_half(m, controller + 0x10); t.remaining = signed_half(m, controller + 0x12);
        const unsigned active = m.bytes[controller + 0x20];
        t.active = active == 1; t.fraction = m.scalar(controller + 0x1C);
        t.infinite = m.bytes[0x61624E] != 0;
        // <=5 is a bounded diagnostic/HUD range, not a newly discovered engine cap.
        t.nitro_valid = t.maximum >= 0 && t.maximum <= 5 && t.remaining >= 0 && t.remaining <= t.maximum &&
            t.refill_limit >= t.maximum && t.refill_limit <= 100 && active <= 1 &&
            std::isfinite(t.fraction) && t.fraction >= -0.00001f && t.fraction <= 1.00001f;
        if (!std::isfinite(t.fraction)) t.fraction = 0;
        t.grant_possible = t.nitro_valid && t.maximum > 0 && t.remaining < t.maximum && !t.active && !t.infinite;
    }
    if ((source_mask & 3) != 3) return t;
    const auto bridge = m.word(a.owner + 0x68), body = m.word(bridge + 0xC);
    const auto matrix = m.word(a.context + 0x18), input = m.word(a.context + 0x24);
    t.wheels = m.word(a.owner + 0x18);
    const auto wheel_table = m.word(a.owner + 0x1C);
    if (!m.object(bridge, 16) || !m.object(body, 0x4C) || !m.object(matrix, 0x34) ||
        !m.object(input, 0x30) || !t.wheels || t.wheels > 8 || !m.object(wheel_table, t.wheels * 4)) return t;
    const float mass = m.scalar(body), inverse = m.scalar(body + 4);
    if (!std::isfinite(mass) || !std::isfinite(inverse) || mass <= 0 || inverse <= 0 || std::abs(mass * inverse - 1) > 0.01f) return t;
    float right_norm = 0, forward_norm = 0, dot = 0;
    for (unsigned i = 0; i < 3; ++i)
    {
        t.velocity[i] = m.scalar(body + 0x40 + 4 * i);
        t.right[i] = m.scalar(matrix + 0x10 + 4 * i);
        t.forward_basis[i] = m.scalar(matrix + 0x28 + 4 * i);
        // Explicit parser sanity bounds, never claimed physics/velocity caps.
        if (!std::isfinite(t.velocity[i]) || !std::isfinite(t.right[i]) || !std::isfinite(t.forward_basis[i]) ||
            std::abs(t.velocity[i]) > 10000 || std::abs(t.right[i]) > 2 || std::abs(t.forward_basis[i]) > 2)
        { t.velocity = {}; t.right = {}; t.forward_basis = {}; return t; }
        right_norm += t.right[i]*t.right[i]; forward_norm += t.forward_basis[i]*t.forward_basis[i]; dot += t.right[i]*t.forward_basis[i];
        t.forward -= t.velocity[i]*t.forward_basis[i];
    }
    if (std::abs(right_norm-1) > 0.05f || std::abs(forward_norm-1) > 0.05f || std::abs(dot) > 0.05f) return t;
    for (unsigned i = 0; i < t.wheels; ++i)
    {
        const auto wheel = m.word(wheel_table + i*4);
        if (!m.object(wheel, 0x174)) return t;
        if (m.bytes[wheel+0x171]) ++t.contacts;
    }
    const float combined = m.scalar(a.owner+0x150), handbrake = m.scalar(a.owner+0x78);
    if (!std::isfinite(combined) || !std::isfinite(handbrake)) return t;
    t.handbrake_suppressed = combined > 0 && handbrake > 0;
    t.input_suppressed = m.word(input+0x2C) != 0;
    t.lateral = std::abs(t.velocity[0]*t.right[0] + t.velocity[2]*t.right[2]);
    t.slip_degrees = static_cast<float>(std::atan2(t.lateral, std::max(0.0001f,t.forward)) * 57.29577951308232);
    t.geometry_valid = std::isfinite(t.forward) && std::isfinite(t.lateral) && std::isfinite(t.slip_degrees);
    // Borrowed Roar geometry at the previously observed stock threshold5, not a native event hook or unlock.
    t.eligible = t.geometry_valid && t.forward > 1 && t.lateral > t.lateral_threshold &&
        static_cast<float>(t.contacts)/t.wheels > 0.51f && !t.handbrake_suppressed && !t.input_suppressed;
    return t;
}

class Dynamo
{
    Actor previous;
    bool have_previous = false, awarded_episode = false, in_episode = false;
    double previous_clock = 0, gap = 0;
    float progress = 0;
    unsigned manual_seen = 0, grants = 0, manual_grants = 0, episodes = 0, activations = 0, spent = 0;
    int previous_remaining = 0;
    bool previous_active = false, previous_nitro_valid = false, fault = false;
    bool previous_enabled = false;
    double qualifying_seconds = 0;
    unsigned progress_resets = 0;
    std::string last_reset_reason;
public:
    DynamoReport report;
    void suspend()
    { have_previous = false; progress = 0; gap = 0; in_episode = false; awarded_episode = false; report.progress = 0; report.dt = 0; report.eligible = false; report.geometry_valid = false; report.nitro_valid = false; report.enabled = false; report.grant_possible = false; report.reason = "Suspended; no counter rollback or grant"; }
    bool grant(Memory& m, DynamoReport& t)
    {
        Actor a;
        if (fault || grants >= 10 || !t.grant_possible || !actor(m,a)) return false;
        const auto fresh = capture_dynamo(m,t.source_mask);
        if (!fresh.grant_possible || fresh.controller != t.controller || fresh.remaining != t.remaining || fresh.maximum != t.maximum) return false;
        // Counter-only reward: reject aliasing another owner, never reset capacity/resources.
        // Preserve the whole-RAM alias check. libc skips nonmatching bytes in blocks;
        // only aligned candidate vtables need typed reads, instead of millions per grant.
        if (!m.contains(0x100000,0x184)) return false;
        const auto* cursor=m.bytes+0x100000;
        const auto* end=m.bytes+m.size-0x184+1;
        while (cursor<end)
        {
            const auto* hit=static_cast<const std::uint8_t*>(std::memchr(cursor,0xF8,static_cast<std::size_t>(end-cursor)));
            if (!hit) break;
            const auto p=static_cast<std::uint32_t>(hit-m.bytes);
            if (p%4==0 && p!=a.owner && m.word(p)==0x6278F8 && m.word(p+0x180)==t.controller) return false;
            cursor=hit+1;
        }
        const std::int16_t next = static_cast<std::int16_t>(t.remaining + 1);
        std::memcpy(m.bytes + t.controller + 0x12, &next, 2);
        if (signed_half(m,t.controller+0x12) != next) { fault = true; return false; }
        t.remaining = next; ++grants;
        t.grant_possible = false; return true;
    }
    void tick(Memory& m, unsigned mask, bool clock_verified, bool driving, bool enabled, unsigned manual_request, std::uint32_t ee_pc=0, float charge_scale=1.0f)
    {
        auto t = capture_dynamo(m,mask); t.enabled = enabled;
        const float before_progress=progress;
        const unsigned before_grants=grants;
        if (enabled!=previous_enabled)
        { have_previous=false; progress=0; gap=0; in_episode=false; awarded_episode=false; previous_enabled=enabled; }
        bool clock_jump=false;
        t.ee_pc=ee_pc; const auto physical_pc=ee_pc & 0x1FFFFFFF;
        t.counter_boundary_safe = !((physical_pc>=0x2DA408 && physical_pc<0x2DAD00) ||
            (physical_pc>=0x2DB588 && physical_pc<0x2DB6EC) || (physical_pc>=0x2F2E18 && physical_pc<0x2F2E48));
        const bool manual = manual_request != manual_seen;
        manual_seen = manual_request; // Explicit requests are consumed once, including refusals.
        Actor a;
        const double clock = m.scalar(0x70E524);
        const float scale = m.scalar(0x618E50);
        const bool valid = driving && actor(m,a) && clock_verified && std::isfinite(clock) && clock >= 0 &&
            std::isfinite(scale) && std::abs(scale-1) < 0.001f && !fault;
        const bool same = valid && have_previous && a.same(previous);
        t.grant_possible = t.grant_possible && valid && grants < 10 && t.counter_boundary_safe;
        if (!valid || !t.geometry_valid || !t.nitro_valid)
        { progress = 0; gap = 0; in_episode = false; awarded_episode = false; }
        t.clock = std::isfinite(clock) ? clock : 0;
        if (same)
        {
            const double dt = clock-previous_clock;
            if (dt > 0 && dt <= 0.1) t.dt = dt;
            else if (dt != 0) { progress = 0; gap = 0; in_episode = false; awarded_episode = false; clock_jump=true; }
            if (t.nitro_valid && previous_nitro_valid)
            {
                if (t.active && !previous_active) ++activations;
                if (t.active && t.remaining < previous_remaining) spent += static_cast<unsigned>(previous_remaining-t.remaining);
            }
        }
        else { progress = 0; gap = 0; in_episode = false; awarded_episode = false; }
        if (!valid) { t.reason = "Waiting: driving/normal clock/owner guard"; t.grant_possible = false; }
        else if (!t.nitro_valid) t.reason = "Nitro layout/source unavailable";
        else if (manual)
        {
            if (grant(m,t)) { ++manual_grants; t.reason = "Manual capped +1; no native pickup feedback"; }
            else t.reason = "Manual +1 refused: capacity/active/cheat/owner/phase/budget";
        }
        else if (!t.counter_boundary_safe) t.reason = "Nitro writer in flight; progress frozen, no edit";
        else if (!t.geometry_valid) t.reason = "Drift geometry/source unavailable";
        else if (t.eligible && t.dt > 0)
        {
            if (!in_episode) { in_episode = true; ++episodes; }
            gap = 0; qualifying_seconds += t.dt;
            if (enabled && !awarded_episode && t.grant_possible && grants < 10)
            {
                progress += static_cast<float>(t.dt) * (std::isfinite(charge_scale) && charge_scale>0 && charge_scale<=40 ? charge_scale : 0);
                if (progress >= t.required_seconds)
                {
                    if (grant(m,t)) { awarded_episode = true; progress = 0; t.reason = "Drift earned +1; use normal R2"; }
                    else { fault = true; t.reason = "Grant refused; stop and report"; }
                }
                else t.reason = "Drift candidate: earning diagnostic charge";
            }
            else { if (!t.grant_possible) progress = 0; t.reason = awarded_episode ? "One reward this drift; straighten to rearm" : "Drift candidate; full/active/disabled/budget"; }
        }
        else
        {
            gap += t.dt;
            if (gap >= 0.5) { progress = 0; awarded_episode = false; in_episode = false; }
            t.reason = "No qualifying slide; telemetry still recording";
        }
        if (!enabled) progress = 0;
        if (before_progress>0 && progress==0 && grants==before_grants)
        {
            ++progress_resets;
            last_reset_reason = !enabled ? "Unequipped/disabled" : (!valid || clock_jump || !same) ? "Clock/owner interruption" :
                !t.nitro_valid ? "Nitro unavailable" : t.active ? "Boost active" : t.remaining>=t.maximum ? "Capacity full" : "Drift break exceeded 0.5s";
        }
        have_previous = valid; previous_clock = clock; previous = a;
        previous_remaining = t.remaining; previous_active = t.active;
        previous_nitro_valid = valid && t.nitro_valid;
        t.progress = progress; t.grants = grants; t.manual_grants = manual_grants;
        t.episodes = episodes; t.activations = activations; t.spent = spent;
        t.qualifying_seconds = qualifying_seconds; t.fault = fault;
        t.progress_resets=progress_resets; t.last_reset_reason=last_reset_reason;
        report = t;
    }
};
} // namespace MC3Forge
