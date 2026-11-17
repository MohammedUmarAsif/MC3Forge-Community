// Original MC3Forge integration for the project-local PCSX2 diagnostic build.
// GPL-3.0-or-later, matching the host PCSX2 derivative.
#include "MC3ForgeTrainer.h"
#include "TrainerCore.h"
#include "TrainerReport.h"
#include "MechanicsTrace.h"
#include "LootWire.h"
#include "InventoryWire.h"
#include "EquipmentUI.h"
#include <memory>
#include <optional>
#include "Dynamo.h"
#include "Money.h"
#include "MoneyGuard.h"
#include "DynamoGuard.h"
#include "BuildGuard.h"
#include "TelemetryGuard.h"
#include "Common.h"
#include "common/FileSystem.h"
#include "Memory.h"
#include "Config.h"
#include "R5900.h"
#include "VMManager.h"
#include "imgui.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <locale>
#include <mutex>

namespace MC3ForgeTrainer
{
namespace
{
MC3Forge::Engine engine;
MC3Forge::Dynamo dynamo;
MC3Forge::MoneyBonus money;
std::atomic<bool> dynamo_override{false};
std::atomic<unsigned> equipment_command{0};
unsigned equipment_sequence=0;
bool loadout_valid=false, loadout_fault=false;
std::atomic<unsigned> manual_nitro{0};
std::mutex display_mutex;
MC3Forge::Report display;
std::shared_ptr<const MC3Forge::InventoryView> inventory_view;
std::optional<MC3Forge::GearAction> gear_command;
std::atomic<bool> ready{false}, effects{false};
std::atomic<bool> equipment_allowed{false}, equipment_open{false}, equipment_requested{false};
std::atomic<unsigned> equipment_held{0}, equipment_pressed{0};
MC3Forge::EquipmentInputGate equipment_input_gate;
unsigned previous_equipment_held=0;
std::atomic<float> dose{100.0f};
std::atomic<bool> loot_mode{true}, reward_any{true}, rewards{true};
MC3Forge::RolledLoadout loadout;
std::string loot_session;
std::filesystem::path home;
bool initialized = false, source_was_valid = false;
unsigned frames = 0;
unsigned source_mask = 0;
MC3Forge::MotionTrace motion_trace;
std::ofstream mechanics_stream;
constexpr const char* part =
    "MC3FORGE_TEST_PART_V1\npower=100\nkind=engine\nrarity=common\nequipped=1\nsource=forced-first-finish\n";

void publish()
{
    std::scoped_lock lock(display_mutex);
    display = engine.report;
}

bool initialize()
{
    if (initialized) return ready.load();
    initialized = true;
    home = std::filesystem::u8path(EmuFolders::DataRoot);
    std::ifstream marker(home / "MC3Forge-trainer.enable");
    std::string line;
    std::getline(marker, line);
    if (line != "MC3FORGE_CPU_TRAINER_V1") return false;
    std::error_code error;
    const auto path = home / "MC3Forge-test-part.ini";
    if (std::filesystem::exists(path, error))
    {
        if (std::filesystem::file_size(path, error) <= 1024 && !error)
        {
            std::ifstream input(path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            engine.owned = text == part;
        }
        if (!engine.owned) { engine.report.fault = true; engine.report.reason = "Test part invalid; no effect"; }
    }
    ready.store(true); return true;
}

void read_loadout()
{
    if (loadout_fault) return;
    const auto path = home / "MC3Forge-rolled-loadout.ini";
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        loadout_valid=false;
        if (loadout.revision) { loadout_fault=true; effects.store(false); engine.report.loot_error="Loadout unavailable; restart after recovery"; }
        return;
    }
    if (error || std::filesystem::file_size(path, error) > 512 || error)
    { loadout_valid=false; loadout_fault=true; engine.report.loot_error = "Invalid loadout size; restart after recovery"; effects.store(false); return; }
    std::ifstream input(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    MC3Forge::RolledLoadout next;
    if (!input || !MC3Forge::RolledLoadout::parse(text, next) || next.revision < loadout.revision || next.items < loadout.items ||
        (loadout_valid && next.revision == loadout.revision && !next.same(loadout)))
    { loadout_valid=false; loadout_fault=true; engine.report.loot_error = "Invalid/conflicting loadout; restart after recovery"; effects.store(false); return; }
    if (!loadout_valid || next.revision > loadout.revision)
    {
        const bool new_item = next.item != loadout.item;
        loadout = next; engine.owned = !next.item.empty();
        if (new_item) { loot_mode.store(true); effects.store(!next.item.empty()); }
        engine.report.loot_error.clear();
    }
    const auto inventory_path=home / "MC3Forge-inventory.txt";
    if (std::filesystem::exists(inventory_path,error) && !error && std::filesystem::file_size(inventory_path,error)<=1024*1024 && !error)
    {
        // Snapshot is immutable once published; the render thread never opens files.
        std::shared_ptr<const MC3Forge::InventoryView> current;
        { std::scoped_lock lock(display_mutex); current=inventory_view; }
        if (!current || current->revision!=loadout.revision)
        {
            std::ifstream stream(inventory_path,std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
            auto view=std::make_shared<MC3Forge::InventoryView>();
            if (stream && MC3Forge::InventoryView::parse(text,*view) && view->revision==loadout.revision && view->items.size()==loadout.items)
            { std::scoped_lock lock(display_mutex); inventory_view=std::move(view); }
        }
    }
    { std::scoped_lock lock(display_mutex); engine.report.inventory_ready=inventory_view && inventory_view->revision==loadout.revision; }
    engine.report.loot_revision = loadout.revision;
    engine.report.inventory_count = loadout.items;
    engine.report.loot_power = loadout.power;
    engine.report.loot_item = loadout.item;
    loadout_valid=true;
    engine.report.dynamo_item=loadout.dynamo_item;
    engine.report.dynamo_equipped=loadout.dynamo;
}

template<class Writer> bool append_journal(const char* name,Writer write)
{
    const auto path = home / name;
    std::error_code error;
    if (std::filesystem::exists(path, error))
    {
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > 16 * 1024 * 1024) return false;
        if (size)
        {
            std::ifstream prior(path, std::ios::binary); prior.seekg(-1, std::ios::end);
            if (prior.get() != '\n') return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::app);
    write(output);
    output.flush(); return output.good();
}

bool record_finish()
{
    if (!MC3Forge::hex_id(loot_session) || engine.report.completed>1000) return false;
    return append_journal("MC3Forge-race-results.jsonl",[](std::ostream& output) {
        MC3Forge::write_finish(output,loot_session,engine.report.completed,engine.report.rank,reward_any.load() ? 8 : 3,engine.report.car);
    });
}

void status_file()
{
    // Fixed fields only; no runtime credentials, memory dumps or host handles.
    const auto path=home / "MC3Forge-trainer-status.json";
    const auto temporary=home / "MC3Forge-trainer-status.json.tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        MC3Forge::write_report(output, engine.report, source_mask, engine.owned);
        output.flush(); if (!output.good()) return;
    }
    const auto from=temporary.u8string(), to=path.u8string();
    FileSystem::RenamePath(reinterpret_cast<const char*>(from.c_str()),reinterpret_cast<const char*>(to.c_str()));
}

MC3Forge::Memory memory()
{ return {eeMem->Main, 0x2000000}; }

bool identity_ram_mapping(const MC3Forge::Memory& m)
{
    using namespace vtlb_private;
    if (!vtlbdata.vmap) return false;
    for (std::uint32_t a = 0x100000; a < m.size; a += 4096)
    {
        const auto mapping = vtlbdata.vmap[a >> VTLB_PAGE_BITS];
        if (mapping.isHandler(a) || mapping.assumePtr(a) != reinterpret_cast<uptr>(m.bytes + a)) return false;
    }
    return true;
}

bool refresh_memory_view(const MC3Forge::Memory& m)
{
    engine.report.ee_cache_enabled = EmuConfig.Cpu.Recompiler.EnableEECache;
    engine.report.ram_mapping_identity = identity_ram_mapping(m);
    engine.report.memory_view_ok = !engine.report.ee_cache_enabled && engine.report.ram_mapping_identity;
    return engine.report.memory_view_ok;
}

bool source_valid(const MC3Forge::Memory& m)
{
    source_mask = 0;
    unsigned bit = 1;
    for (const auto& check : MC3ForgeBuildGuard::checks)
    {
        if (m.contains(check.address, check.bytes.size()) && std::memcmp(m.bytes + check.address, check.bytes.data(), check.bytes.size()) == 0) source_mask |= bit;
        bit <<= 1;
    }
    return source_mask == 7;
}

unsigned telemetry_source_mask(const MC3Forge::Memory& m)
{
    unsigned mask = 0, bit = 1;
    for (const auto& check : MC3ForgeTelemetryGuard::checks)
    {
        if (m.contains(check.address, check.length) && std::memcmp(m.bytes + check.address, check.bytes, check.length) == 0) mask |= bit;
        bit <<= 1;
    }
    return mask;
}

unsigned dynamo_source_mask(const MC3Forge::Memory& m)
{
    unsigned mask = 0, bit = 1;
    for (const auto& check : MC3DynamoGuard::checks)
    {
        if (m.contains(check.address, check.length) && std::memcmp(m.bytes+check.address,check.bytes,check.length)==0) mask |= bit;
        bit <<= 1;
    }
    return mask;
}
} // namespace

void Tick()
{
    if (!eeMem || !initialize()) { equipment_allowed.store(false); return; }
    auto m = memory();
    if (!refresh_memory_view(m))
    {
        equipment_allowed.store(false);
        dynamo.suspend(); engine.report.dynamo = dynamo.report;
        if (engine.report.applied) engine.abandon();
        engine.report.reason = "Unsupported EE memory view; effects refused";
        source_was_valid = false; source_mask = 0;
        publish(); if (++frames % 60 == 0) status_file(); return;
    }
    if (!source_valid(m))
    {
        equipment_allowed.store(false);
        dynamo.suspend(); engine.report.dynamo = dynamo.report;
        if (source_was_valid) engine.abandon();
        else engine.report.reason = "Waiting for pinned VIP game code (checks " + std::to_string(source_mask) + "/7)";
        source_was_valid = false; publish(); if (++frames % 60 == 0) status_file(); return;
    }
    if (!source_was_valid && !engine.report.fault) engine.report.reason = engine.owned ? "Test part loaded; waiting for supported driving" : "Ready; complete first supported race";
    source_was_valid = true;
    if (frames == 0 || frames % 15 == 0)
    {
        if (loot_session.empty())
        {
            std::ifstream input(home / "MC3Forge-loot-session.txt");
            std::getline(input, loot_session);
        }
        read_loadout();
    }
    std::optional<MC3Forge::GearAction> pending;
    { std::scoped_lock lock(display_mutex); pending=std::move(gear_command); gear_command.reset(); }
    const auto menu_root=m.word(0x618AE0), menu_scene=m.word(menu_root+4);
    const bool menu_scene_valid=m.object(menu_root,8) && m.word(menu_root)==0x630258 &&
        m.object(menu_scene,4) && m.word(menu_scene)==0x630120 && m.word(0x619B00)==2 && !engine.report.fault;
    if (pending && !menu_scene_valid) { engine.report.loot_error="Equipment request refused outside qualified free roam"; pending.reset(); }
    if (pending && (!MC3Forge::hex_id(loot_session) || equipment_sequence>=1000 || !append_journal("MC3Forge-equipment-actions.jsonl",[&](std::ostream& output) {
        MC3Forge::write_gear_action(output,loot_session,++equipment_sequence,*pending);
    }))) engine.report.loot_error="Inventory request failed; stop and report";
    const auto command=equipment_command.exchange(0);
    if (command)
    {
        if (!MC3Forge::hex_id(loot_session) || equipment_sequence>=1000 || !append_journal("MC3Forge-equipment-actions.jsonl",[&](std::ostream& output) {
            MC3Forge::write_equipment(output,loot_session,++equipment_sequence,command==1);
        })) engine.report.loot_error="Equipment request failed; stop and report";
    }
    engine.report.loot_valid=loadout_valid;
    engine.report.dynamo_equipped=loadout_valid && loadout.dynamo;
    engine.report.dynamo_charge_scale=loadout_valid && loadout.dynamo ? static_cast<float>(loadout.dynamo_milli)/1000.0f : dynamo_override.load() ? 1.0f : 0;
    engine.report.using_loot = loot_mode.load() && loadout_valid && !loadout.item.empty();
    const auto before = engine.report.completed;
    // Fixed legacy item is preserved on disk; it is not minted again as a rolled reward.
    engine.tick(m, false, effects.load(), engine.report.using_loot ? static_cast<float>(loadout.power) : dose.load(), engine.report.using_loot);
    // Root type, ownership and current code must agree; results/countdown/unknown scenes refuse.
    equipment_allowed.store(engine.report.cruising && !engine.report.fault && engine.report.mechanics_valid &&
        engine.report.raw_mode==2 && (engine.report.state==4 || engine.report.state==9));
    if (engine.report.completed != before && rewards.load() && !engine.report.fault && !record_finish())
    { engine.report.loot_error = "Result persistence failed; no roll. Stop and report"; }
    const auto telemetry_mask = telemetry_source_mask(m);
    unsigned money_mask=0, money_bit=1;
    for (const auto& check:MC3MoneyGuard::checks)
    {
        if (m.contains(check.address,check.length) && std::memcmp(m.bytes+check.address,check.bytes,check.length)==0) money_mask|=money_bit;
        money_bit<<=1;
    }
    unsigned money_permille=0;
    { std::scoped_lock lock(display_mutex);
      if (inventory_view && inventory_view->revision==loadout.revision && loadout_valid)
          for (const auto& item:inventory_view->items) if (item.slot!="none")
              for (const auto& a:item.affixes) if (a.key=="money") money_permille+=static_cast<unsigned>(std::lround(a.value*1000)); }
    const auto previous_credits=money.report.credits;
    money.report.source_mask=money_mask;
    money.tick(m,money_mask==127 && loadout_valid && !engine.report.fault,money_permille,[&](bool applied,const MC3Forge::MoneyReport& credit) {
        if (!MC3Forge::hex_id(loot_session)) return false;
        return append_journal("MC3Forge-money-credits.jsonl",[&](std::ostream& out) {
            out << "{\"session\":\"" << loot_session << "\",\"attempt\":" << credit.attempt
                << ",\"stage\":\"" << (applied ? "applied" : "prepared") << "\",\"before\":" << credit.before
                << ",\"native_after\":" << credit.native_after << ",\"base\":" << credit.base << ",\"bonus\":" << credit.bonus
                << ",\"after\":" << credit.after << ",\"permille\":" << credit.permille << "}\n";
        });
    });
    engine.report.money=money.report;
    const auto previous_grants=dynamo.report.grants;
    const int previous_remaining=dynamo.report.remaining;
    const bool previous_active=dynamo.report.active;
    dynamo.tick(m,dynamo_source_mask(m),(telemetry_mask & 96)==96,
        engine.report.driving && !engine.report.fault,
        (loadout_valid && loadout.dynamo) || dynamo_override.load(),manual_nitro.load(),cpuRegs.pc,
        loadout_valid && loadout.dynamo ? static_cast<float>(loadout.dynamo_milli)/1000.0f : 1.0f);
    engine.report.dynamo = dynamo.report;
    const bool resource_edge = previous_credits!=money.report.credits || previous_grants!=dynamo.report.grants || previous_remaining!=dynamo.report.remaining || previous_active!=dynamo.report.active;
    engine.report.drivetrain = MC3Forge::capture_runtime(m, (telemetry_mask & 31) == 31, (telemetry_mask & 96) == 96);
    engine.report.drivetrain.source_body_mask = telemetry_mask;
    if (Pad::HasConnectedPad(0))
    {
        const auto* pad = Pad::GetPad(0, 0);
        if (pad && pad->GetType() == Pad::ControllerType::DualShock2)
        {
            auto& t = engine.report.drivetrain; t.input_available = true;
            t.buttons_active_low = pad->GetButtons();
            constexpr std::array<unsigned,8> inputs{PadDualshock2::Inputs::PAD_CROSS, PadDualshock2::Inputs::PAD_SQUARE,
                PadDualshock2::Inputs::PAD_CIRCLE, PadDualshock2::Inputs::PAD_TRIANGLE,
                PadDualshock2::Inputs::PAD_L1, PadDualshock2::Inputs::PAD_L2,
                PadDualshock2::Inputs::PAD_R1, PadDualshock2::Inputs::PAD_R2};
            for (unsigned i = 0; i < inputs.size(); ++i) t.button_pressures[i] = pad->GetRawInput(inputs[i]);
            const auto left = pad->GetRawLeftAnalog(), right = pad->GetRawRightAnalog();
            t.stick_axes = {std::get<0>(left), std::get<1>(left), std::get<0>(right), std::get<1>(right)};
        }
    }
    motion_trace.update(m, engine.report.drivetrain, engine.report.driving);
    if (engine.report.driving && (frames % 6 == 0 || resource_edge) && !engine.report.trace_io_failed)
    {
        if (engine.report.trace_rows < 6000)
        {
            if (!mechanics_stream.is_open()) mechanics_stream.open(home / "MC3Forge-mechanics-trace.jsonl", std::ios::trunc);
            if (mechanics_stream.good())
            {
                ++engine.report.trace_rows;
                MC3Forge::write_report(mechanics_stream, engine.report, source_mask, engine.owned);
                if (!mechanics_stream.good()) { --engine.report.trace_rows; engine.report.trace_io_failed = true; }
            }
            else engine.report.trace_io_failed = true;
        }
        else engine.report.trace_limit_reached = true;
    }
    publish();
    if (++frames % 60 == 0) { status_file(); if (mechanics_stream.is_open()) mechanics_stream.flush(); }
}

void Suspend()
{
    equipment_allowed.store(false); equipment_open.store(false);
    if (!ready.load() || !eeMem) return;
    auto m = memory();
    if (refresh_memory_view(m)) engine.suspend(m); else engine.abandon();
    dynamo.suspend(); engine.report.dynamo = dynamo.report;
    publish(); status_file(); if (mechanics_stream.is_open()) mechanics_stream.flush();
}

void Invalidate()
{
    money.invalidate();
    equipment_allowed.store(false); equipment_open.store(false);
    if (!ready.load() || !eeMem) return;
    auto m = memory();
    if (refresh_memory_view(m)) engine.invalidate(m); else engine.abandon();
    dynamo.suspend(); dynamo_override.store(false); engine.report.dynamo = dynamo.report;
    publish(); status_file();
}

bool CaptureEquipmentInput(std::uint32_t held)
{
    if (!ready.load()) return false;
    equipment_held.store(held);
    equipment_pressed.fetch_or(held & ~previous_equipment_held);
    previous_equipment_held=held;
    bool request=equipment_requested.load();
    const bool capture=equipment_input_gate.filter(equipment_allowed.load(),equipment_open.load(),held,request);
    equipment_requested.store(request);
    return capture;
}

void Render()
{
    if (!ready.load()) return;
    static bool open = false, hud = true;
    static MC3Forge::EquipmentMenu equipment_menu;
    static float pending_dose = 100;
    const bool toggle=ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false);
    MC3Forge::Report r;
    std::shared_ptr<const MC3Forge::InventoryView> gear;
    { std::scoped_lock lock(display_mutex); r = display; gear=inventory_view; }
    const unsigned pressed=equipment_pressed.exchange(0);
    const bool requested=equipment_requested.exchange(false);
    const bool allowed=equipment_allowed.load();
    if (!allowed) { equipment_menu.open=false; equipment_menu.help=false; }
    else if (requested || toggle) equipment_menu.open=requested || !equipment_menu.open;
    if (equipment_menu.open)
    {
        auto key=[&](unsigned bit,ImGuiKey keyboard) { return (pressed&(1u<<bit)) || ImGui::IsKeyPressed(keyboard,false); };
        MC3Forge::EquipmentKeys keys;
        keys.up=key(0,ImGuiKey_UpArrow); keys.down=key(2,ImGuiKey_DownArrow);
        keys.confirm=key(6,ImGuiKey_Enter); keys.back=key(5,ImGuiKey_Escape);
        keys.tune=key(4,ImGuiKey_T); keys.sell=key(7,ImGuiKey_S);
        keys.help=key(8,ImGuiKey_H); keys.close=!requested && (pressed&(1u<<9));
        keys.previous=key(10,ImGuiKey_Q); keys.next=key(12,ImGuiKey_E);
        keys.scroll_up=key(11,ImGuiKey_PageUp); keys.scroll_down=key(13,ImGuiKey_PageDown);
        if (requested) keys={}; // Opening chord must not close or cycle a slot.
        MC3Forge::InventoryView empty;
        const bool available=gear && r.loot_valid && gear->revision==r.loot_revision;
        if (auto action=MC3Forge::draw_equipment(equipment_menu,gear ? *gear : empty,keys,available))
        {
            std::scoped_lock lock(display_mutex);
            if (!gear_command)
            {
                equipment_menu.waiting=true; equipment_menu.submitted_revision=action->revision;
                equipment_menu.submitted_at=ImGui::GetTime(); gear_command=std::move(*action);
            }
        }
    }
    equipment_open.store(equipment_menu.open);
    // Existing research controls stay available through a deliberate debug shortcut.
    if (!allowed || equipment_menu.open) open=false;
    else if (ImGui::IsKeyPressed(ImGuiKey_F10,false)) open=!open;
    static unsigned seen_grants=0,seen_manual=0,seen_resets=0;
    static double grant_notice_until=0,reset_notice_until=0;
    static std::string grant_notice;
    const double now=ImGui::GetTime();
    if (r.dynamo.grants>seen_grants)
    { grant_notice=r.dynamo.manual_grants>seen_manual ? "+1 NITRO (MANUAL TEST)" : "+1 NITRO - DRIFT DYNAMO"; grant_notice_until=now+3; }
    if (r.dynamo.progress_resets>seen_resets) reset_notice_until=now+2;
    seen_grants=r.dynamo.grants; seen_manual=r.dynamo.manual_grants; seen_resets=r.dynamo.progress_resets;
    auto meter=[&](float width)
    {
        const auto& d=r.dynamo;
        const auto state=MC3Forge::dynamo_meter(d);
        float fill=d.required_seconds>0 ? std::clamp(d.progress/d.required_seconds,0.0f,1.0f) : 0;
        std::string label=std::to_string(static_cast<unsigned>(fill*100))+"% drift charge";
        if (state==MC3Forge::DynamoMeter::Unequipped) { fill=0; label="DYNAMO UNEQUIPPED"; }
        else if (state==MC3Forge::DynamoMeter::Unavailable) { fill=0; label="WAITING / UNAVAILABLE"; }
        else if (state==MC3Forge::DynamoMeter::Full) { fill=1; label="NITRO FULL"; }
        else if (state==MC3Forge::DynamoMeter::BoostActive) label="BOOST ACTIVE";
        else if (state==MC3Forge::DynamoMeter::Waiting) label="0% - SLIDE TO CHARGE";
        if (r.fault || d.fault) { fill=0; label="RESTART REQUIRED"; }
        ImGui::ProgressBar(fill,ImVec2(width,0),label.c_str());
        if (now<grant_notice_until) ImGui::TextColored(ImVec4(0.4f,1,0.6f,1),"%s",grant_notice.c_str());
        else if (now<reset_notice_until) ImGui::TextWrapped("Charge reset: %s",d.last_reset_reason.c_str());
    };
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.35f);
    if (hud && !equipment_menu.open)
    {
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12, viewport->WorkPos.y + 12), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::Begin("MC3Forge power", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(1, 0.78f, 0.23f, 1), "MC3FORGE / TEST POWER");
        ImGui::Text("Base %.2f   Current %.2f", r.base, r.current);
        ImGui::Text("%s | requested +%.0f%%", r.driving ? (r.cruising ? "CRUISE" : "RACE") : "SUSPENDED", r.requested_percent);
        ImGui::Text("Stored parts %u | equipped +%u%%", r.inventory_count, r.loot_power);
    if (!r.loot_item.empty()) ImGui::Text("Part %.12s | %s", r.loot_item.c_str(), r.using_loot ? "ROLLED" : "MANUAL OVERRIDE");
        if (!r.loot_error.empty()) ImGui::TextWrapped("%s", r.loot_error.c_str());
        const auto& d = r.dynamo;
        ImGui::Separator();
        ImGui::Text("DRIFT DYNAMO | %s", r.dynamo_equipped ? "EQUIPPED" : d.enabled ? "RESEARCH OVERRIDE" : "UNEQUIPPED");
        meter(320);
        ImGui::Text("Nitro %d/%d | granted %u | used %u", d.remaining,d.maximum,d.grants,d.spent);
        ImGui::Text("Progress %.1f/%.1fs | lateral %.2f", d.progress,d.required_seconds,d.lateral);
        ImGui::TextWrapped("%s",d.reason.c_str());
        ImGui::Text("Peak %.2f | boosted frames %u", r.peak_power, r.boosted_vsyncs);
        ImGui::Text("+%.0f%% | %s | completed %u", r.bonus, r.applied ? "ACTIVE" : (r.equipped ? "EQUIPPED / SUSPENDED" : "WAITING FOR FIRST FINISH"), r.completed);
        ImGui::TextWrapped("%s", r.reason.c_str());
        ImGui::Separator();
        ImGui::Text("MONEY: base %u + bonus %u = %u",r.money.base,r.money.bonus,r.money.base+r.money.bonus);
        ImGui::Text("Wallet %u -> %u | bonus +%.0f%%",r.money.before,r.money.after,r.money.permille/10.0f);
        ImGui::Text("Money code checks %u/127 | credited races %u",r.money.source_mask,r.money.credits);
        ImGui::TextWrapped("%s",r.money.reason.c_str());
        ImGui::End();
    }
    if (!open) { ImGui::PopFont(); return; }
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 20, viewport->WorkPos.y + 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(720, 590), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.40f, 0.06f, 0.06f, 1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(1, 0.78f, 0.23f, 1));
    ImGui::Begin("MC3Forge | experimental trainer (`)", &open);
    ImGui::TextColored(ImVec4(1, 0.78f, 0.23f, 1), "REPEATABLE LOOT / COPIED SAVE EXPERIMENT");
    if (ImGui::BeginTabBar("trainer"))
    {
        if (ImGui::BeginTabItem("Inventory"))
        {
            static std::string selected;
            static int filter=0,accessory_slot=0,affix_index=0;
            static bool waiting=false;
            static unsigned submitted_revision=0;
            if (gear && gear->revision!=submitted_revision) waiting=false;
            const bool available=gear && gear->revision==r.loot_revision && r.loot_valid;
            if (!available) ImGui::TextWrapped("Waiting for the matching saved inventory. Do not change equipment yet.");
            else
            {
                auto send=[&](const char* action,const MC3Forge::GearItem* item,const std::string& slot)
                {
                    if (waiting) return;
                    MC3Forge::GearAction request;
                    request.action=action; request.item=item ? item->id : "none"; request.slot=slot;
                    request.affix=static_cast<unsigned>(affix_index); request.revision=gear->revision;
                    request.offer=item && !item->offer_id.empty() ? item->offer_id : "none";
                    { std::scoped_lock lock(display_mutex); if (gear_command) return; gear_command=std::move(request); }
                    submitted_revision=gear->revision; waiting=true;
                };
                ImGui::Text("Saved parts %zu / 1000 | revision %u",gear->items.size(),gear->revision);
                ImGui::Text("Epic pity: %u / 100 parts | test rates, free rerolls",gear->dry);
                ImGui::TextWrapped("Last action: %s",gear->feedback.c_str());
                ImGui::TextWrapped("One part per slot. Matching bonuses add across slots. New drops only fill empty slots.");
                if (waiting) ImGui::TextColored(ImVec4(1,.78f,.23f,1),"Saving request... wait for the revision to change.");
                ImGui::BeginDisabled(waiting);
                if (ImGui::Button("Create optional +5% tires / brakes test kit")) send("test_kit",nullptr,"engine");
                ImGui::TextDisabled("Test kit is not race-earned; added once, never auto-equipped.");
                ImGui::Separator();
                const char* filters[]={"All parts","engine","nitrous","tires","brakes","transmission","suspension","accessory"};
                ImGui::Combo("Part filter",&filter,filters,8);
                const float width=ImGui::GetContentRegionAvail().x;
                ImGui::BeginChild("part-list",ImVec2(width*.43f,260),true);
                for (const auto& item:gear->items)
                {
                    if (filter && item.kind!=filters[filter]) continue;
                    const auto label=(item.slot!="none" ? "[E] " : "")+item.rarity+" "+item.kind+" / "+item.id.substr(0,6)+"##"+item.id;
                    if (ImGui::Selectable(label.c_str(),selected==item.id)) { selected=item.id; affix_index=0; }
                }
                ImGui::EndChild(); ImGui::SameLine();
                ImGui::BeginChild("part-details",ImVec2(0,260),true);
                const MC3Forge::GearItem* item=nullptr;
                for (const auto& entry:gear->items) if (entry.id==selected) item=&entry;
                if (!item) ImGui::TextWrapped("Select a part to equip or reroll an affix.");
                else
                {
                    ImGui::Text("%s %s",item->rarity.c_str(),item->kind.c_str());
                    ImGui::TextDisabled("%s | %.12s",item->label.c_str(),item->id.c_str());
                    for (unsigned i=0;i<item->affixes.size();++i)
                    {
                        const auto& a=item->affixes[i];
                        const auto label=a.key+" +"+std::to_string(static_cast<int>(std::round(a.key=="drift_nitro" ? a.value : a.value*100)))+(a.key=="drift_nitro" ? " Dynamo" : "%")+"##affix"+std::to_string(i);
                        if (ImGui::Selectable(label.c_str(),affix_index==static_cast<int>(i))) affix_index=static_cast<int>(i);
                    }
                    if (item->kind=="accessory") ImGui::Combo("Accessory slot",&accessory_slot,"accessory_1\0accessory_2\0");
                    const std::string slot=item->kind=="accessory" ? MC3Forge::gear_slots[6+accessory_slot] : item->kind;
                    if (ImGui::Button("Equip / swap")) send("equip",item,slot);
                    if (item->slot!="none") { ImGui::SameLine(); if (ImGui::Button("Unequip")) send("unequip",item,item->slot); }
                    ImGui::Separator();
                    if (ImGui::Button("Roll selected affix offer (free test)")) send("offer",item,slot);
                    if (!item->offer_id.empty())
                    {
                        ImGui::TextWrapped("Offer for affix %u: %s +%.0f%%",item->offer_index+1,item->offer_key.c_str(),item->offer_value*100);
                        if (ImGui::Button("Keep current")) send("keep",item,slot);
                        ImGui::SameLine(); if (ImGui::Button("Apply offer")) send("accept",item,slot);
                    }
                }
                ImGui::EndChild();
                ImGui::EndDisabled();
                ImGui::Separator();
                std::array<float,7> totals{};
                for (const auto& part:gear->items) if (part.slot!="none")
                    for (const auto& a:part.affixes) for (unsigned i=0;i<MC3Forge::gear_keys.size();++i) if (a.key==MC3Forge::gear_keys[i]) totals[i]+=a.value;
                ImGui::Text("BUILD: power +%.0f%% | Dynamo %.2fx charge",totals[0]*100,totals[1]*(1+totals[2]));
                ImGui::Text("Rarity luck +%.0f%% | extra-drop chance +%.0f%%",totals[3]*100,totals[4]*100);
                if (ImGui::CollapsingHeader("Equipped slots")) for (const auto* slot:MC3Forge::gear_slots)
                {
                    const MC3Forge::GearItem* worn=nullptr;
                    for (const auto& part:gear->items) if (part.slot==slot) worn=&part;
                    ImGui::Text("%s: %s",slot,worn ? worn->id.substr(0,8).c_str() : "empty");
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loot + power"))
        {
            bool a = rewards.load(), any = reward_any.load(), e = effects.load();
            if (ImGui::Checkbox("Roll a part after an eligible finish", &a)) rewards.store(a);
            if (ImGui::Checkbox("Test policy: reward any finish (off = top 3)", &any)) reward_any.store(any);
            if (ImGui::Checkbox("Enable equipped power (race + cruise)", &e)) effects.store(e);
            ImGui::Checkbox("Top-right power HUD", &hud);
            ImGui::Text("Pending power (%%) - press Apply to commit");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##dose", &pending_dose, 0, 100, "+%.0f%%");
            if (ImGui::Button("0%")) pending_dose = 0;
            ImGui::SameLine(); if (ImGui::Button("2%")) pending_dose = 2;
            ImGui::SameLine(); if (ImGui::Button("10%")) pending_dose = 10;
            ImGui::SameLine(); if (ImGui::Button("100%")) pending_dose = 100;
            if (ImGui::Button("Apply")) { loot_mode.store(false); dose.store(pending_dose); effects.store(true); }
            ImGui::SameLine(); if (ImGui::Button("Disable / restore")) effects.store(false);
            if (ImGui::Button("Use rolled part")) { loot_mode.store(true); effects.store(r.loot_valid && !r.loot_item.empty()); }
            ImGui::Text("Committed +%.0f%% | applied +%.0f%%", r.requested_percent, r.bonus);
            ImGui::Separator();
            ImGui::Text("Stored parts: %u | latest Common engine +%u%%", r.inventory_count, r.loot_power);
            if (!r.loot_item.empty()) ImGui::Text("Equipped ID: %s", r.loot_item.c_str());
            ImGui::Text("Effect source: %s", r.using_loot ? "saved rolled part" : "manual diagnostic");
            if (!r.loot_error.empty()) ImGui::TextWrapped("%s", r.loot_error.c_str());
            ImGui::Text("Car: %s", r.car.empty() ? "waiting" : r.car.c_str());
            ImGui::Text("Power: %.2f -> %.2f", r.base, r.current);
            ImGui::Text("Applications %u | Restorations %u", r.applications, r.restorations);
            ImGui::TextWrapped("%s", r.reason.c_str());
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Drift research"))
        {
            const auto& d = r.dynamo;
            ImGui::Text("Drift Dynamo nitrous part: %s",r.dynamo_equipped ? "EQUIPPED" : "UNEQUIPPED");
            meter(-1);
            if (gear) ImGui::TextWrapped("Use Inventory to swap or unequip Dynamo-bearing parts; their charge contributions stack.");
            else if (!r.dynamo_item.empty() && r.loot_valid)
            {
                ImGui::Text("Owned ID: %s",r.dynamo_item.c_str());
                if (ImGui::Button("Equip Dynamo")) { dynamo_override.store(false); equipment_command.store(1); }
                ImGui::SameLine();
                if (ImGui::Button("Unequip Dynamo")) { dynamo_override.store(false); equipment_command.store(2); }
                ImGui::TextWrapped("Equipment changes save automatically; allow a moment for confirmation above.");
            }
            else if (!r.loot_valid) ImGui::TextWrapped("Saved loadout unavailable; check the error above before driving.");
            else ImGui::TextWrapped("Complete one eligible race for the guaranteed first Dynamo nitrous part. Your engine part stays equipped.");
            bool enabled = dynamo_override.load();
            if (ImGui::Checkbox("Research override (ignores equipment)", &enabled)) dynamo_override.store(enabled);
            ImGui::Text("Candidate %.1f/%.1fs | grants %u (manual %u)", d.progress,d.required_seconds,d.grants,d.manual_grants);
            ImGui::Text("Nitro %d/%d | active %s | fraction %.3f", d.remaining,d.maximum,d.active ? "yes" : "no",d.fraction);
            ImGui::Text("Forward %.2f | lateral %.2f | slip %.1f deg", d.forward,d.lateral,d.slip_degrees);
            ImGui::Text("Wheel contact %u/%u | source checks %u/63",d.contacts,d.wheels,d.source_mask);
            ImGui::Text("Geometry %s | Nitro %s | infinite cheat %s",d.geometry_valid ? "valid" : "unavailable",d.nitro_valid ? "valid" : "unavailable",d.infinite ? "ON / refused" : "off");
            if (ImGui::Button("Manual +1 nitro (capped diagnostic)")) manual_nitro.fetch_add(1);
            ImGui::TextWrapped("%s",d.reason.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("Equipped drift_nitro affixes enable Drift Dynamo; copies add charge rate and drift_charge_rate scales their total. Reference lateral threshold 5, two qualifying native seconds and a 0.5s break reset remain unchanged. Release the handbrake after initiating the slide. Full capacity cannot bank charge; one grant per episode. Unequip disables automatic grants and clears partial charge; it does not remove already earned bottles.");
            ImGui::TextWrapped("Manual +1 tests the charge operation separately if no slide qualifies. This mirrors the capped pickup counter change; it does not call the guest pickup function or reproduce feedback. Disabling clears progress; spent charges are never rolled back. Ten grants per launch is a diagnostic guard, not balance or an engine limit.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Telemetry"))
        {
            ImGui::Text("Race state %d | position %d", r.state, r.rank);
            ImGui::Text("Mode %u | race type %08X", r.raw_mode, r.race_type);
            ImGui::Text("Mechanics: %s", r.mechanics_valid ? "typed candidates" : "unavailable / last observation");
            constexpr std::array<const char*,13> names{"Engine inertia", "Engage RPM", "Launch burst power", "Launch burst duration", "Configured mass", "Front brake", "Front static grip", "Front sliding grip", "Rear brake", "Rear static grip", "Rear sliding grip", "Nitro amount", "Nitro duration"};
            for (unsigned i = 0; i < names.size(); ++i) ImGui::Text("%s: %.3f", names[i], r.mechanics[i]);
            ImGui::TextWrapped("Read-only mechanic candidates. This trainer applies engine power and guarded Dynamo charges; labels alone do not qualify other affixes.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Help"))
        {
            ImGui::TextWrapped("Inventory prototype: each eligible finish rolls parts across six components and accessories, Common to Epic. Position affects rarity; 100 awarded parts without Epic guarantees one. Luck affects rarity; quantity affects extra drops. New parts fill empty slots only. Free reroll offers preserve the original until accepted. Test policy rewards any finish; off uses top-three. Manual power Apply and Manual +1 remain separate diagnostics.");
            ImGui::Separator();
            ImGui::TextWrapped("Tilde hides/shows this trainer. Use the Inventory tab to swap parts and reroll ordinary affixes; changes save automatically. Cold restart keeps that state. The drift bar shows full/reset/unequipped states and +1 notifications. Do not load savestates: reset/state-load or lost ownership requires a full process restart. Cruise can earn bottles through equipped Dynamo; parts come from race finishes. Native pause-menu entry and multiplayer remain future work.");
            ImGui::Separator();
            ImGui::TextWrapped("This is an explicit diagnostic override, not the admitted earned-loot pipeline. Shared engine configurations, incompatible layouts and unexpected field changes refuse. General car-relative binding is implemented, but all cars are not gameplay-qualified. Jump and new cheats are deferred until this loop passes.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End(); ImGui::PopStyleColor(2); ImGui::PopFont();
}
} // namespace MC3ForgeTrainer
