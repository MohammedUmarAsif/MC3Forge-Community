#pragma once
#include "EquipmentMenu.h"
#include "imgui.h"

namespace MC3Forge
{
inline ImVec4 rarity_colour(const std::string& rarity)
{
    if (rarity=="uncommon") return {1,.91f,.48f,1};
    if (rarity=="rare") return {.39f,.68f,1,1};
    if (rarity=="epic") return {.79f,.51f,1,1};
    if (rarity=="divine") return {.35f,.91f,.53f,1};
    if (rarity=="ethereal") return {1,.65f,.20f,1};
    return {.94f,.94f,.92f,1};
}
struct EquipmentKeys { bool up=false,down=false,confirm=false,back=false,tune=false,sell=false,help=false,close=false,previous=false,next=false,scroll_up=false,scroll_down=false; };
inline void item_details(const GearItem* item)
{
    if (!item) { ImGui::TextDisabled("Empty slot"); return; }
    ImGui::PushStyleColor(ImGuiCol_Text,rarity_colour(item->rarity));
    ImGui::TextWrapped("%s",part_name(*item).c_str()); ImGui::PopStyleColor();
    ImGui::TextDisabled("%s / %s",rarity_name(item->rarity),item->kind.c_str());
    if (item->slot!="none") ImGui::TextDisabled("EQUIPPED / %s",item->slot.c_str());
    ImGui::Separator();
    for (const auto& a:item->affixes)
        ImGui::TextWrapped("%s  +%.0f%s",affix_name(a.key),a.value*(a.key=="drift_nitro" ? 1 : 100),a.key=="drift_nitro" ? "" : "%");
}
inline void equipment_help(EquipmentMenu& menu)
{
    constexpr std::array<const char*,3> tabs{{"Effects","Controls","Progression"}};
    ImGui::Text("LABEL HELP / %s",tabs[menu.help_tab]);
    ImGui::TextDisabled("L1 / R1: help tabs"); ImGui::Separator();
    if (menu.help_tab==0)
    {
        constexpr std::array<const char*,7> explanations{{
            "Power bonuses add across equipped parts, then apply once to the captured car baseline. This is an equipment bonus, not a measured horsepower or speed prediction.",
            "Dynamo units add. Recharge multiplier = units x (1 + total charge-rate bonus). Two units and +50% charge rate calculate to 3x progress, not three nitro bottles. Faster stacked recharge still needs live qualification.",
            "Charge rate scales equipped Dynamo units. Without Dynamo units this affix alone gives no recharge. Capacity and guarded grants remain unchanged.",
            "Luck modifies Rare / Exotic rarity weights. +10% luck is not ten probability points. Newly awarded parts affect future rewards.",
            "Quantity bonuses add. Each whole +100% gives another part; the remainder is the chance of one further part. Newly awarded parts affect future rewards.",
            "Pity acceleration adds across equipped parts. Exotic guarantee threshold = ceil(100 / (1 + total acceleration)) awarded parts without an Exotic. +100% means 50 parts. Equipping never awards anything; only a future qualified reward evaluates the threshold.",
            "Money bonuses add across equipped parts. Experimental paid-finish integration observes the native wallet credit, then adds floor(base payout x bonus). +200% means 3x the base payout. Native source, ownership, finish, journal and readback guards must pass. HUD and local receipt show base, bonus and wallet before/after. No paid reroll or selling support is implied."}};
        ImGui::TextWrapped("%s",affix_names[menu.help_label%7]);
        ImGui::TextWrapped("%s",explanations[menu.help_label%7]);
        ImGui::TextDisabled("Up / Down: choose an effect label");
    }
    else if (menu.help_tab==1)
        ImGui::TextWrapped("Start closes. Select toggles help. Cross browses slots, equips a candidate, or rolls a focused affix. Triangle enters affix tuning. Circle goes back. Square requests Sell, which is unavailable until native wallet credit is qualified. L1 + Start opens only in qualified free roam. Keyboard: arrows, Enter, Esc, T, S, H, Q/E and tilde. Camera orbit is not enabled.");
    else
        ImGui::TextWrapped("Rerolls are free during testing. Later fees will depend on rarity; prices and wallet debit are not implemented. Sets, fixed piece perks, requirement -1, item-level scaling and upgrades are planned. Divine / Ethereal are progression-gated and are not in the current pool. A future upgrade path should preserve favourite affixes while raising a part's base level.");
}
// Same renderer is used by the game and the offline preview harness; no disk/guest access.
inline std::optional<GearAction> draw_equipment(EquipmentMenu& menu,const InventoryView& view,const EquipmentKeys& keys,bool available)
{
    std::optional<GearAction> action;
    if (keys.close) { menu.open=false; menu.help=false; return action; }
    if (keys.help) menu.help=!menu.help;
    if (keys.back) menu.back();
    if (!menu.open) return action;
    if (menu.waiting && view.revision!=menu.submitted_revision) { menu.waiting=false; menu.notice=view.feedback; }
    if (menu.waiting && ImGui::GetTime()-menu.submitted_at>10)
        menu.notice="Still awaiting save acknowledgement. Close and reopen to inspect; no automatic retry.";
    auto send=[&](const char* verb,const GearItem* item) {
        if (!available || menu.waiting || !item) return;
        GearAction request; request.action=verb; request.item=item->id; request.slot=gear_slots[menu.slot];
        request.affix=menu.affix; request.revision=view.revision;
        request.offer=item->offer_id.empty() ? "none" : item->offer_id;
        action=std::move(request);
    };
    std::vector<const GearItem*> parts;
    auto refresh=[&]() {
        parts.clear(); for (const auto& item:view.items) if (fits_slot(item,menu.slot)) parts.push_back(&item);
        if (menu.page!=EquipmentPage::Slots && !selected_part(view,menu))
        { menu.selected=parts.empty() ? "" : parts.front()->id; menu.affix=0; }
    };
    if (menu.help)
    {
        if (keys.previous) menu.help_tab=(menu.help_tab+2)%3;
        if (keys.next) menu.help_tab=(menu.help_tab+1)%3;
        if (keys.up) menu.help_label=(menu.help_label+6)%7;
        if (keys.down) menu.help_label=(menu.help_label+1)%7;
    }
    else
    {
        if (!menu.waiting && (keys.previous || keys.next))
        { menu.slot=(menu.slot+(keys.previous ? 7 : 1))%8; menu.selected.clear(); menu.affix=0; }
        refresh();
        if (menu.page==EquipmentPage::Slots)
        {
            if (keys.up) menu.slot=(menu.slot+7)%8;
            if (keys.down) menu.slot=(menu.slot+1)%8;
            if (keys.confirm) { menu.page=EquipmentPage::Parts; const auto* worn=equipped_part(view,menu.slot); menu.selected=worn ? worn->id : ""; refresh(); }
        }
        else if (menu.page==EquipmentPage::Parts)
        {
            auto at=std::find_if(parts.begin(),parts.end(),[&](const auto* p){return p->id==menu.selected;});
            if (!parts.empty() && (keys.up || keys.down))
            { const auto index=at==parts.end() ? 0 : static_cast<unsigned>(at-parts.begin()); menu.selected=parts[(index+parts.size()+(keys.up ? -1 : 1))%parts.size()]->id; menu.affix=0; }
            if (keys.confirm) send("equip",selected_part(view,menu));
            if (keys.tune) { menu.page=EquipmentPage::Affixes; menu.affix=0; }
        }
        else if (const auto* candidate=selected_part(view,menu))
        {
            const auto count=static_cast<unsigned>(candidate->affixes.size());
            if (!candidate->offer_id.empty())
            {
                if (keys.up || keys.down) menu.offer_choice=1-menu.offer_choice;
                if (keys.confirm) send(menu.offer_choice ? "accept" : "keep",candidate);
            }
            else
            {
                menu.offer_choice=0;
                if (count && keys.up) menu.affix=(menu.affix+count-1)%count;
                if (count && keys.down) menu.affix=(menu.affix+1)%count;
                if (keys.confirm && count) send("offer",candidate);
            }
        }
        if (keys.sell && menu.page!=EquipmentPage::Slots) menu.notice="Sell unavailable: native wallet credit is not qualified. Your part is unchanged.";
    }
    refresh();
    const auto* worn=equipped_part(view,menu.slot);
    const auto* candidate=menu.page==EquipmentPage::Slots ? nullptr : selected_part(view,menu);
    const auto* viewport=ImGui::GetMainViewport();
    const auto origin=viewport->WorkPos, size=viewport->WorkSize;
    const float scale=std::clamp(size.y/900.0f,.85f,1.25f), margin=18*scale;
    const bool mobile=size.x<640;
    const bool compact=size.x<900;
    const float usable=size.x-margin*2, left=usable*(compact ? .40f : .29f), middle=usable*.35f;
    const float top=origin.y+margin+65*scale, height=size.y-150*scale;
    const float footer=mobile ? 100*scale : 40*scale;
    const float content_height=height-(mobile ? 60*scale : 0);
    const bool third=menu.help || candidate;
    const float mobile_left=third ? content_height*.26f : content_height*.48f;
    const float mobile_middle=third ? content_height*.32f : content_height-mobile_left-8*scale;
    constexpr auto flags=ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNavInputs;
    ImGui::PushFont(nullptr,18*scale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,{.045f,.052f,.064f,.97f});
    ImGui::PushStyleColor(ImGuiCol_Header,{.32f,.21f,.09f,1});
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,{.39f,.29f,.14f,1});
    ImGui::PushStyleColor(ImGuiCol_Border,{.29f,.30f,.32f,1});
    ImGui::SetNextWindowPos({origin.x+margin,origin.y+margin}); ImGui::SetNextWindowSize({usable,55*scale});
    ImGui::Begin("equipment-header",nullptr,flags);
    ImGui::TextColored({1,.77f,.32f,1},"MC3FORGE / EQUIPMENT");
    ImGui::TextWrapped("FREE ROAM / %zu parts / FREE TEST rerolls",view.items.size()); ImGui::End();
    ImGui::SetNextWindowPos({origin.x+margin,top}); ImGui::SetNextWindowSize({mobile ? usable : left-8*scale,mobile ? mobile_left : height});
    ImGui::Begin("equipment-slots",nullptr,flags);
    ImGui::TextDisabled("%s",menu.page==EquipmentPage::Slots ? "EQUIPPED LOADOUT" : slot_names[menu.slot]); ImGui::Separator();
    ImGui::BeginDisabled(menu.help || menu.waiting || !available);
    if (menu.page==EquipmentPage::Slots)
    {
        for (unsigned s=0;s<8;++s)
        {
            const auto* item=equipped_part(view,s); ImGui::PushID(static_cast<int>(s));
            ImGui::PushFont(nullptr,14*scale); ImGui::TextDisabled("%s",slot_names[s]); ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text,item ? rarity_colour(item->rarity) : ImVec4(.5f,.52f,.55f,1));
            if (ImGui::Selectable(item ? part_name(*item).c_str() : "Empty slot",menu.slot==s,0,{0,24*scale}))
            { menu.slot=s; menu.page=EquipmentPage::Parts; menu.selected=item ? item->id : ""; }
            if (ImGui::IsItemHovered()) menu.slot=s;
            if (menu.slot==s && (keys.up || keys.down || keys.previous || keys.next)) ImGui::SetScrollHereY(.5f);
            ImGui::PopStyleColor(); ImGui::PopID();
        }
    }
    else
    {
        if (parts.empty()) ImGui::TextWrapped("No compatible owned parts.");
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(parts.size()));
        for (unsigned i=0;i<parts.size();++i) if (parts[i]->id==menu.selected) clipper.IncludeItemByIndex(static_cast<int>(i));
        while (clipper.Step()) for (int index=clipper.DisplayStart;index<clipper.DisplayEnd;++index)
        {
            const auto* item=parts[index]; ImGui::PushID(item->id.c_str());
            ImGui::TextDisabled("%s%s",rarity_name(item->rarity),item->slot!="none" ? " / EQUIPPED" : "");
            ImGui::PushStyleColor(ImGuiCol_Text,rarity_colour(item->rarity));
            const auto y=ImGui::GetCursorPosY();
            if (ImGui::Selectable(part_name(*item).c_str(),menu.selected==item->id,0,{0,26*scale})) { menu.selected=item->id; menu.affix=0; }
            if (ImGui::IsItemHovered() && menu.page==EquipmentPage::Parts) { menu.selected=item->id; menu.affix=0; }
            if (item->id==menu.selected && (keys.up || keys.down || keys.previous || keys.next)) ImGui::SetScrollHereY(.5f);
            ImGui::PopStyleColor(); ImGui::SetCursorPosY(y+40*scale); ImGui::Dummy({0,0}); ImGui::PopID();
        }
    }
    ImGui::EndDisabled(); ImGui::End();
    // Re-resolve after mouse focus: preview never retains pointers across revisions.
    worn=equipped_part(view,menu.slot); candidate=menu.page==EquipmentPage::Slots ? nullptr : selected_part(view,menu);
    const auto current=build_totals(view), proposed=build_totals(view,candidate,menu.slot);
    ImGui::SetNextWindowPos(mobile ? ImVec2(origin.x+margin,top+mobile_left+8*scale) : ImVec2(origin.x+margin+left,top));
    ImGui::SetNextWindowSize({mobile ? usable : compact ? usable-left : middle-8*scale,mobile ? mobile_middle : height});
    ImGui::Begin("equipment-current",nullptr,flags);
    ImGui::TextDisabled("%s",menu.page==EquipmentPage::Affixes ? "TUNE SELECTED PART" : "CURRENTLY EQUIPPED");
    if (menu.page!=EquipmentPage::Affixes) item_details(worn);
    else if (candidate)
    {
        ImGui::TextColored(rarity_colour(candidate->rarity),"%s",part_name(*candidate).c_str());
        ImGui::TextDisabled("Reroll fee: FREE TEST"); ImGui::Separator();
        ImGui::BeginDisabled(!available || menu.waiting || menu.help);
        for (unsigned i=0;i<candidate->affixes.size();++i)
        {
            const auto& a=candidate->affixes[i]; ImGui::PushID(static_cast<int>(i));
            const std::string label=std::string(affix_name(a.key))+" +"+std::to_string(static_cast<int>(std::round(a.value*(a.key=="drift_nitro" ? 1 : 100))))+(a.key=="drift_nitro" ? "" : "%");
            if (ImGui::Selectable(label.c_str(),menu.affix==i)) menu.affix=i;
            if (ImGui::IsItemHovered()) menu.affix=i;
            ImGui::PopID();
        }
        if (candidate->offer_id.empty()) { if (ImGui::Button("Cross / Roll focused affix")) send("offer",candidate); }
        else
        {
            ImGui::Separator();
            ImGui::TextWrapped("PENDING OFFER / %s +%.0f%s",affix_name(candidate->offer_key),candidate->offer_value*(candidate->offer_key=="drift_nitro" ? 1 : 100),candidate->offer_key=="drift_nitro" ? "" : "%");
            ImGui::TextWrapped("Original stays until you Apply. Closing preserves this offer.");
            if (ImGui::Selectable("Keep current",menu.offer_choice==0)) send("keep",candidate);
            if (ImGui::Selectable("Apply offer",menu.offer_choice==1)) send("accept",candidate);
        }
        ImGui::EndDisabled();
    }
    ImGui::BeginDisabled(!available || menu.waiting || menu.help || !worn);
    if (ImGui::Button("Unequip this slot")) send("unequip",worn);
    ImGui::EndDisabled();
    ImGui::Separator(); ImGui::TextDisabled("BUILD / EQUIPMENT BONUSES");
    for (unsigned i=0;i<gear_keys.size();++i)
    {
        const float before=i==1 ? current[1]*(1+current[2]) : current[i]*100;
        const float after=i==1 ? proposed[1]*(1+proposed[2]) : proposed[i]*100;
        if (candidate)
        {
            ImGui::TextWrapped("%s: %.2f -> %.2f",i==1 ? "Dynamo recharge" : affix_names[i],before,after);
            const float delta=after-before;
            ImGui::TextColored(delta>0 ? ImVec4(.39f,.68f,1,1) : delta<0 ? ImVec4(1,.38f,.35f,1) : ImVec4(.65f,.65f,.65f,1),"%+.2f%s %s",delta,i==1 ? "x" : " pp",delta>0 ? "UP" : delta<0 ? "DOWN" : "UNCHANGED");
        }
        else ImGui::TextWrapped("%s: +%.1f%s",i==1 ? "Dynamo recharge" : affix_names[i],before,i==1 ? "x" : "%");
    }
    if (candidate && candidate->slot!="none" && candidate->slot!=gear_slots[menu.slot]) ImGui::TextWrapped("Moves from %s; that accessory slot becomes empty.",candidate->slot.c_str());
    if (!available) ImGui::TextWrapped("Inventory unavailable or stale. Changes disabled.");
    if (menu.waiting) ImGui::TextColored({1,.77f,.32f,1},"Saving request...");
    if (keys.scroll_up) ImGui::SetScrollY(ImGui::GetScrollY()-80*scale);
    if (keys.scroll_down) ImGui::SetScrollY(ImGui::GetScrollY()+80*scale);
    if (!menu.notice.empty()) ImGui::TextWrapped("%s",menu.notice.c_str());
    else if (!view.feedback.empty()) ImGui::TextWrapped("%s",view.feedback.c_str());
    ImGui::End();
    if (menu.help || candidate)
    {
        ImGui::SetNextWindowPos(mobile ? ImVec2(origin.x+margin,top+mobile_left+mobile_middle+16*scale) : compact ? ImVec2(origin.x+margin,top+height*.48f) : ImVec2(origin.x+margin+left+middle,top));
        ImGui::SetNextWindowSize(mobile ? ImVec2(usable,content_height-mobile_left-mobile_middle-16*scale) : compact ? ImVec2(left-8*scale,height*.52f) : ImVec2(usable-left-middle,height));
        ImGui::Begin("equipment-candidate",nullptr,flags);
        if (menu.help) equipment_help(menu);
        else
        {
            ImGui::TextDisabled("HIGHLIGHTED REPLACEMENT"); item_details(candidate);
            ImGui::Separator(); ImGui::TextWrapped("Compare first. Cross explicitly equips.");
            ImGui::BeginDisabled(!available || menu.waiting);
            if (ImGui::Button("Cross / Equip")) send("equip",candidate);
            if (ImGui::Button("Triangle / Tune affixes")) { menu.page=EquipmentPage::Affixes; menu.affix=0; }
            if (ImGui::Button("Square / Sell (unavailable)")) menu.notice="Sell unavailable: native wallet credit is not qualified. Part unchanged.";
            ImGui::EndDisabled();
        }
        if (keys.scroll_up) ImGui::SetScrollY(ImGui::GetScrollY()-80*scale);
        if (keys.scroll_down) ImGui::SetScrollY(ImGui::GetScrollY()+80*scale);
        ImGui::End();
    }
    ImGui::SetNextWindowPos({origin.x+margin,origin.y+size.y-footer-15*scale}); ImGui::SetNextWindowSize({usable,footer});
    ImGui::Begin("equipment-legend",nullptr,flags);
    ImGui::TextWrapped("Cross / Enter: %s   Triangle / T: affixes   Square / S: sell unavailable   Circle / Esc: back   Select / H: help   Start: close",menu.page==EquipmentPage::Slots ? "browse" : menu.page==EquipmentPage::Parts ? "equip" : "reroll");
    ImGui::End(); ImGui::PopStyleColor(4); ImGui::PopFont();
    return action;
}
}
