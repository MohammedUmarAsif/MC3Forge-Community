#include "EquipmentMenu.h"
#include "Money.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace MC3Forge;
    const std::string row="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|engine|epic|none|power=1,drift_nitro=1,drift_charge_rate=0.5,luck=0.2,loot_quantity=0.2,money=0.25|none|dev-kit\n";
    InventoryView view;
    assert(InventoryView::parse("MC3FORGE_INVENTORY_V1\n1\n0\nReady\n"+row,view));
    assert(view.items.size()==1 && view.items[0].affixes.size()==6);
    assert(!InventoryView::parse("MC3FORGE_INVENTORY_V1\n1\n0\nReady\n"+row+row,view));
    InventoryView build;
    for (unsigned slot=0;slot<8;++slot)
    {
        GearItem item; item.id=std::to_string(slot); item.kind=slot<6 ? gear_slots[slot] : "accessory";
        item.slot=gear_slots[slot]; item.affixes={{"power",1},{"drift_nitro",1},{"drift_charge_rate",.5f},{"money",.25f}};
        build.items.push_back(item);
    }
    const auto totals=build_totals(build);
    assert(totals[0]==8 && totals[1]*(1+totals[2])==40 && totals[6]==2);
    const auto moved=build_totals(build,&build.items[6],7);
    assert(moved[0]==7); // Moving accessory1 into accessory2 removes its previous occupant.
    EquipmentMenu menu; menu.open=true; menu.page=EquipmentPage::Parts; menu.back();
    assert(menu.open && menu.page==EquipmentPage::Slots); menu.back(); assert(!menu.open);
    EquipmentInputGate gate; bool request=false;
    assert(!gate.filter(false,false,(1u<<9)|(1u<<10),request));
    gate.filter(true,false,0,request);
    assert(gate.filter(true,false,(1u<<9)|(1u<<10),request) && request);
    request=false; assert(gate.filter(true,true,1u<<6,request));
    assert(gate.filter(true,false,1u<<6,request));
    gate.filter(true,false,0,request); assert(!gate.filter(true,false,0,request));
    std::cout<<"Native protocol, aggregation, accessory move, Back and input-gate checks passed\n";
}
