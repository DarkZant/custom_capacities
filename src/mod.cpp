#include "mods/svc/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"

// Game includes
#include "d/dolzel_rel.h" // IWYU pragma: keep
#include "d/d_save.h"
#include "d/d_com_inf_game.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter_HIO.h"
#include "d/d_meter2_info.h"
#include "d/d_menu_ring.h"

#include "f_op/f_op_actor_mng.h" // For hooks

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

// Hooks the wallet-specific rupee maximum.
DEFINE_HOOK_SYMBOL("dSv_player_status_a_c::getRupeeMax", u16(const dSv_player_status_a_c*), MaxRupee);
// Hooks the current quiver's arrow maximum.
DEFINE_HOOK_SYMBOL("dComIfGs_getArrowMax", u8(), MaxArrow);
// Hooks the maximum for a specific bomb type.
DEFINE_HOOK_SYMBOL("dSv_player_item_max_c::getBombNum", u8(const dSv_player_item_max_c*, u8), MaxBomb);
// Hooks slingshot pellet count updates.
DEFINE_HOOK(&dMeter2_c::movePachinkoNum, MovePachinkoNum);
// Replaces item-wheel count rendering to support three-digit values.
DEFINE_HOOK(&dMenu_Ring_c::drawNumber, RingDrawNumber);
// Hooks the item-wheel capacity used for its full-state color.
DEFINE_HOOK(&dMenu_Ring_c::getItemMaxNum, RingGetItemMax);

struct LimitSetting {
    const char* name;
    const char* label;
    const char* suffix;
    u16 max;
    ConfigVarHandle var{};
    u16 value{};
};

static LimitSetting s_walletLimits[] = {
    {"wallet_limit", "Wallet", " rupees", 9999},
    {"big_wallet_limit", "Big Wallet", " rupees", 9999},
    {"giant_wallet_limit", "Giant Wallet", " rupees", 9999},
};
static LimitSetting s_quiverLimits[] = {
    {"quiver_limit", "Quiver", " arrows", 255},
    {"big_quiver_limit", "Big Quiver", " arrows", 255},
    {"giant_quiver_limit", "Giant Quiver", " arrows", 255},
};
static LimitSetting s_bombLimits[] = {
    {"regular_bombs_no_giant_bag", "Regular Bombs (Base Bomb Bag)", " bombs", 255},
    {"water_bombs_no_giant_bag", "Water Bombs (Base Bomb Bag)", " water bombs", 255},
    {"bomblings_no_giant_bag", "Bomblings (Base Bomb Bag)", " bomblings", 255},
    {"regular_bombs_giant_bag", "Regular Bombs (Giant Bomb Bag)", " bombs", 255},
    {"water_bombs_giant_bag", "Water Bombs (Giant Bomb Bag)", " water bombs", 255},
    {"bomblings_giant_bag", "Bomblings (Giant Bomb Bag)", " bomblings", 255},
};
static LimitSetting s_pelletLimit[] = {
    {"slingshot_pellets", "Slingshot Pellets", " pellets", 255},
};

static constexpr size_t kWalletCount = sizeof(s_walletLimits) / sizeof(s_walletLimits[0]);
static constexpr size_t kQuiverCount = sizeof(s_quiverLimits) / sizeof(s_quiverLimits[0]);
static constexpr size_t kBombCount = sizeof(s_bombLimits) / sizeof(s_bombLimits[0]);
static constexpr size_t kPelletCount = sizeof(s_pelletLimit) / sizeof(s_pelletLimit[0]);
static bool s_pelletMoveCaptured = false;
static u8 s_pelletCountBefore = 0;
static s16 s_pelletDeltaBefore = 0;
static u8 s_pelletDisplayBefore = 0;
static u8 s_pelletLimitBefore = 0;

// Clamps a configured capacity to its supported range.
static u16 clamp_limit(int64_t value, u16 max) {
    if (value < 0) {
        return 0;
    }
    if (value > max) {
        return max;
    }
    return static_cast<u16>(value);
}

// Updates a capacity setting when its saved configuration changes.
static void on_limit_changed(
    ModContext*, ConfigVarHandle, const ConfigVarValue* value, const ConfigVarValue*, void* userData) {
    auto& setting = *static_cast<LimitSetting*>(userData);
    setting.value = clamp_limit(value->int_value, setting.max);
}

// Registers capacity settings and loads their current configured values.
static ModResult register_limit_settings(LimitSetting* settings, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
        desc.name = settings[i].name;
        desc.type = CONFIG_VAR_INT;
        desc.default_int = 0;

        ModResult result = svc_config->register_var(mod_ctx, &desc, &settings[i].var);
        if (result != MOD_OK) {
            return result;
        }

        int64_t value = 0;
        result = svc_config->get_int(mod_ctx, settings[i].var, &value);
        if (result != MOD_OK) {
            return result;
        }
        settings[i].value = clamp_limit(value, settings[i].max);

        result = svc_config->subscribe(
            mod_ctx, settings[i].var, on_limit_changed, &settings[i], nullptr);
        if (result != MOD_OK) {
            return result;
        }
    }
    return MOD_OK;
}

// Registers all wallet, quiver, bomb, and pellet capacity settings.
static ModResult register_all_limit_settings() {
    ModResult result = register_limit_settings(s_walletLimits, kWalletCount);
    if (result != MOD_OK) {
        return result;
    }
    result = register_limit_settings(s_quiverLimits, kQuiverCount);
    if (result != MOD_OK) {
        return result;
    }
    result = register_limit_settings(s_bombLimits, kBombCount);
    if (result != MOD_OK) {
        return result;
    }
    return register_limit_settings(s_pelletLimit, kPelletCount);
}

// Adds a group of configurable capacity controls to the settings panel.
static ModResult add_limit_controls(
    UiElementHandle panel, const char* title, LimitSetting* settings, size_t count) {
    ModResult result = svc_ui->pane_add_section(mod_ctx, panel, title);
    if (result != MOD_OK) {
        return result;
    }

    for (size_t i = 0; i < count; ++i) {
        UiControlDesc control = UI_CONTROL_DESC_INIT;
        control.kind = UI_CONTROL_NUMBER;
        control.label = settings[i].label;
        control.binding = UI_BINDING_CONFIG_VAR;
        control.config_var = settings[i].var;
        control.min = 0;
        control.max = settings[i].max;
        control.step = 1;
        control.suffix = settings[i].suffix;

        result = svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
        if (result != MOD_OK) {
            return result;
        }
    }
    return MOD_OK;
}

// Builds the in-game panel containing all capacity controls.
static ModResult build_capacity_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    ModResult result = add_limit_controls(panel, "Rupee Limits", s_walletLimits,
        kWalletCount);
    if (result != MOD_OK) {
        return result;
    }
    result = add_limit_controls(panel, "Quiver Limits", s_quiverLimits,
        kQuiverCount);
    if (result != MOD_OK) {
        return result;
    }
    result = add_limit_controls(panel, "Bomb Bag Limits", s_bombLimits,
        kBombCount);
    if (result != MOD_OK) {
        return result;
    }
    return add_limit_controls(
        panel, "Slingshot Limit", s_pelletLimit, kPelletCount);
}

// Registers the capacity settings panel with the Mods UI.
static ModResult register_capacity_panel() {
    UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
    panel.build = build_capacity_panel;
    return svc_ui->register_mods_panel(mod_ctx, &panel);
}

// Overrides the rupee maximum for the player's current wallet when configured.
static void on_max_rupee_post(ModContext*, void* args, void* retval, void*) {
    const auto* playerStatus = mods::arg<const dSv_player_status_a_c*>(args, 0);
    const u8 walletSize = playerStatus->getWalletSize();
    if (walletSize < kWalletCount && s_walletLimits[walletSize].value > 0) {
        *static_cast<u16*>(retval) = s_walletLimits[walletSize].value;
    }
}

// Overrides the arrow maximum for the player's current quiver when configured.
static void on_max_arrow_post(ModContext*, void*, void* retval, void*) {
    auto& currentMax = *static_cast<u8*>(retval);
    size_t quiverIndex;
    switch (currentMax) {
    case QUIVER_MAX:
        quiverIndex = 0;
        break;
    case BIG_QUIVER_MAX:
        quiverIndex = 1;
        break;
    case GIANT_QUIVER_MAX:
        quiverIndex = 2;
        break;
    default:
        return;
    }

    if (s_quiverLimits[quiverIndex].value > 0) {
        currentMax = static_cast<u8>(s_quiverLimits[quiverIndex].value);
    }
}

// Overrides the bomb maximum for the bomb type and bag currently in use.
static void on_max_bomb_post(ModContext*, void* args, void* retval, void*) {
    const u8 bombType = mods::arg<u8>(args, 1);
    size_t bombIndex;
    switch (bombType) {
    case dItemNo_NORMAL_BOMB_e:
        bombIndex = 0;
        break;
    case dItemNo_WATER_BOMB_e:
        bombIndex = 2;
        break;
    case dItemNo_POKE_BOMB_e:
        bombIndex = 4;
        break;
    default:
        return;
    }

    if (dComIfGs_isItemFirstBit(dItemNo_BOMB_BAG_LV2_e)) {
        ++bombIndex;
    }
    if (s_bombLimits[bombIndex].value > 0) {
        *static_cast<u8*>(retval) = static_cast<u8>(s_bombLimits[bombIndex].value);
    }
}

// Supplies the configured pellet maximum to the item wheel's full-capacity check.
static void on_ring_get_item_max_post(ModContext*, void* args, void* retval, void*) {
    if (s_pelletLimit[0].value == 0) {
        return;
    }

    const u8 itemSlot = mods::arg<u8>(args, 1);
    if (dComIfGs_getItem(itemSlot, false) == dItemNo_PACHINKO_e) {
        *static_cast<u8*>(retval) = static_cast<u8>(s_pelletLimit[0].value);
    }
}

// Draws item-wheel counts above 100 without the game's two-digit clamp.
static void on_ring_draw_number_replace(ModContext*, void* args, void*, void*) {
    auto* ring = mods::arg<dMenu_Ring_c*>(args, 0);
    int itemNum = mods::arg<int>(args, 1);
    const int itemMaxNum = mods::arg<int>(args, 2);
    const f32 posX = mods::arg<f32>(args, 3);
    const f32 posY = mods::arg<f32>(args, 4);

    if (itemNum <= 100) {
        RingDrawNumber::g_orig(ring, itemNum, itemMaxNum, posX, posY);
        return;
    }

    JUtility::TColor colorBlack;
    JUtility::TColor colorWhite;
    if (itemNum == itemMaxNum) {
        colorBlack.set(30, 30, 30, 0);
        colorWhite.set(255, 200, 50, 255);
    } else if (itemNum == 0) {
        colorBlack.set(30, 30, 30, 0);
        colorWhite.set(180, 180, 180, 255);
    } else {
        colorBlack.set(0, 0, 0, 0);
        colorWhite.set(255, 255, 255, 255);
    }
    for (int i = 0; i < 3; ++i) {
        ring->mpItemNumTex[i]->setBlackWhite(colorBlack, colorWhite);
    }

    ResTIMG* texture = static_cast<ResTIMG*>(dComIfGp_getMain2DArchive()->getResource(
        'TIMG', dMeter2Info_getNumberTextureName(itemNum / 100)));
    ring->mpItemNumTex[0]->changeTexture(texture, 0);
    const int remainder = itemNum % 100;
    texture = static_cast<ResTIMG*>(dComIfGp_getMain2DArchive()->getResource(
        'TIMG', dMeter2Info_getNumberTextureName(remainder / 10)));
    ring->mpItemNumTex[1]->changeTexture(texture, 0);
    texture = static_cast<ResTIMG*>(dComIfGp_getMain2DArchive()->getResource(
        'TIMG', dMeter2Info_getNumberTextureName(remainder % 10)));
    ring->mpItemNumTex[2]->changeTexture(texture, 0);
    ring->mpItemNumTex[2]->show();

    const u8 itemIconAlpha = ring->mPlayerIsWolf != 0 ? g_ringHIO.mItemIconAlpha_Wolf
                                                       : g_ringHIO.mItemIconAlpha;
    for (int i = 0; i < 3; ++i) {
        ring->mpItemNumTex[i]->setAlpha(itemIconAlpha * ring->mAlphaRate);
        ring->mpItemNumTex[i]->draw(posX + i * 16.0f, posY - 16.0f, 16.0f, 16.0f, 0, 0, 0);
    }
}

// Captures pellet state before the game applies its vanilla count limit.
static HookAction on_pachinko_move_pre(ModContext*, void* args, void*, void*) {
    s_pelletMoveCaptured = false;
    if (s_pelletLimit[0].value == 0) {
        return HOOK_CONTINUE;
    }

    s_pelletCountBefore = dComIfGs_getPachinkoNum();
    s_pelletDeltaBefore = dComIfGp_getItemPachinkoNumCount();
    auto* meter = mods::arg<dMeter2_c*>(args, 0);
    s_pelletDisplayBefore = meter->mPachinkoNum;
    s_pelletLimitBefore = static_cast<u8>(s_pelletLimit[0].value);
    s_pelletMoveCaptured = true;
    return HOOK_CONTINUE;
}

// Applies the configured pellet limit and refreshes the meter display.
static void on_pachinko_move_post(ModContext*, void* args, void*, void*) {
    if (!s_pelletMoveCaptured) {
        return;
    }

    auto* meter = mods::arg<dMeter2_c*>(args, 0);
    int pelletCount = s_pelletCountBefore + s_pelletDeltaBefore;
    if (pelletCount < 0) {
        pelletCount = 0;
    } else if (pelletCount > s_pelletLimitBefore) {
        pelletCount = s_pelletLimitBefore;
    }

    dComIfGs_setPachinkoNum(static_cast<u8>(pelletCount));
    meter->mPachinkoNum = s_pelletDisplayBefore;
    if (meter->mPachinkoNum < pelletCount) {
        ++meter->mPachinkoNum;
    } else if (meter->mPachinkoNum > pelletCount) {
        --meter->mPachinkoNum;
    }
    meter->mItemMaxNum[dMeter2_c::PACHINKO_MAX] = s_pelletLimitBefore;

    for (u8 i = 0; i < 2; ++i) {
        if (meter->mItemStatus[i * 2] == dItemNo_PACHINKO_e) {
            meter->mpMeterDraw->setItemNum(i, meter->mPachinkoNum, s_pelletLimitBefore);
        }
    }
    s_pelletMoveCaptured = false;
}

extern "C" {

// Registers settings, UI controls, and all capacity-related game hooks.
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ModResult result = register_all_limit_settings();
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to register capacity settings");
        return result;
    }

    result = register_capacity_panel();
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to register capacity panel");
        return result;
    }

    result = mods::hook::add_post<MaxRupee>(on_max_rupee_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install MaxRupee post hook");
        return result;
    }
    result = mods::hook::add_post<MaxArrow>(on_max_arrow_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install MaxArrow post hook");
        return result;
    }
    result = mods::hook::add_post<MaxBomb>(on_max_bomb_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install MaxBomb post hook");
        return result;
    }
    result = mods::hook::replace<RingDrawNumber>(on_ring_draw_number_replace);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to replace item-wheel number drawing");
        return result;
    }
    result = mods::hook::add_post<RingGetItemMax>(on_ring_get_item_max_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install item-wheel capacity hook");
        return result;
    }
    result = mods::hook::add_pre<MovePachinkoNum>(on_pachinko_move_pre);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install PachinkoNumMove pre hook");
        return result;
    }
    result = mods::hook::add_post<MovePachinkoNum>(on_pachinko_move_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install PachinkoNumMove post hook");
        return result;
    }

    svc_log->info(mod_ctx, "Custom Capacities initialized");
    return MOD_OK;
}

// Performs per-frame mod updates; no recurring work is currently required.
MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

// Shuts down the mod; no explicit cleanup is currently required.
MOD_EXPORT ModResult mod_shutdown(ModError*) {
    return MOD_OK;
}

}
