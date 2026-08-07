/*
 * Copyright (C) Volition, Inc. 1999.  All rights reserved.
 *
 * All source code herein is the property of Volition, Inc. You may not sell 
 * or otherwise commercially exploit the source or things you created based on the 
 * source.
 *
*/



#include "bmpman/bmpman.h"
#include "debugconsole/console.h"
#include "freespace.h"
#include "gamesnd/gamesnd.h"
#include "globalincs/linklist.h"
#include "graphics/font.h"
#include "iff_defs/iff_defs.h"
#include "io/timer.h"
#include "jumpnode/jumpnode.h"
#include "localization/localize.h"
#include "network/multi.h"
#include "object/object.h"
#include "options/Option.h"
#include "playerman/player.h"
#include "radar/radar.h"
#include "radar/radarorb.h"
#include "radar/radarsetup.h"
#include "ship/awacs.h"
#include "ship/ship.h"
#include "ship/subsysdamage.h"
#include "weapon/emp.h"
#include "weapon/weapon.h"
#include "mod_table/mod_table.h"

#include <cmath>

sound_handle Radar_static_looping = sound_handle::invalid();

rcol Radar_color_rgb[MAX_RADAR_COLORS][MAX_RADAR_LEVELS] =
{
	// homing missile (yellow)
	{	
		{ 0x40, 0x40, 0x00 },		// dim
		{ 0x7f, 0x7f, 0x00 },		// bright
	},

	// navbuoy or cargo (gray)
	{
		{ 0x40, 0x40, 0x40 },		// dim
		{ 0x7f, 0x7f, 0x7f },		// bright
	},

	// warping ship (blue)
	{
		{ 0x00, 0x00, 0x7f },		// dim
		{ 0x00, 0x00, 0xff },		// bright
	},

	// jump node (gray)
	{
		{ 0x40, 0x40, 0x40 },		// dim
		{ 0x7f, 0x7f, 0x7f },		// bright
	},

	// tagged (yellow)
	{
		{ 0x7f, 0x7f, 0x00 },		// dim
		{ 0xff, 0xff, 0x00 },		// bright
	},
};

int		radar_target_id_flags = 0;

color Radar_colors[MAX_RADAR_COLORS][MAX_RADAR_LEVELS];

blip	Blip_bright_list[MAX_BLIP_TYPES];		// linked list of bright blips
blip	Blip_dim_list[MAX_BLIP_TYPES];			// linked list of dim blips

blip	Blips[MAX_BLIPS];								// blips pool
int	N_blips;											// next blip index to take from pool

SCP_map<int, TIMESTAMP> Blip_last_update;	// map of objnums to timestamps

float	Radar_bright_range;					// range at which we start dimming the radar blips
TIMESTAMP	Radar_calc_bright_dist_timer;		// timestamp at which we recalc Radar_bright_range

extern int radar_iff_color[5][2][4];

int See_all = 0;

DCF_BOOL(see_all, See_all);

RadarIconMode Radar_2d_icon_mode = RadarIconMode::On;

static auto RadarIconModeOption __UNUSED = options::OptionBuilder<RadarIconMode>("HUD.Radar2dIconMode",
	std::pair<const char*, int>{"Radar 2D Icons", 1915},
	std::pair<const char*, int>{"Controls how custom 2D ship icons are displayed on the radar", 1916})
	.category(std::make_pair("Game", 1824))
	.values({{RadarIconMode::Off,        {"Off", 1286}},
	         {RadarIconMode::On,         {"On", 1285}},
	         {RadarIconMode::TargetOnly, {"Target Only", 1917}}})
	.default_val(RadarIconMode::On)
	.bind_to(&Radar_2d_icon_mode)
	.importance(56)
	.finish();

void radar_check_2d_icon_options()
{
	bool has_icons = std::any_of(Ship_info.begin(), Ship_info.end(), [](const ship_info& sip) {
		return sip.radar_image_2d_idx >= 0 || sip.radar_color_image_2d_idx >= 0;
	});

	if (!has_icons) {
		options::OptionsManager::instance()->removeOption(RadarIconModeOption);
	}
}

void radar_stuff_blip_info(object *objp, int is_bright, color **blip_color, int *blip_type)
{
	ship *shipp = NULL;

	switch(objp->type)
	{
		case OBJ_SHIP:
			shipp = &Ships[objp->instance];

			if (shipp->is_arriving(ship::warpstage::STAGE1, false))
			{
				*blip_color = &Radar_colors[RCOL_WARPING_SHIP][is_bright];
				*blip_type = BLIP_TYPE_WARPING_SHIP;
			}
			else if (ship_is_tagged(objp))
			{
				*blip_color = &Radar_colors[RCOL_TAGGED][is_bright];
				*blip_type = BLIP_TYPE_TAGGED_SHIP;
			}
            else if (Ship_info[shipp->ship_info_index].flags[Ship::Info_Flags::Navbuoy] || Ship_info[shipp->ship_info_index].flags[Ship::Info_Flags::Cargo])
			{
				*blip_color = &Radar_colors[RCOL_NAVBUOY_CARGO][is_bright];
				*blip_type = BLIP_TYPE_NAVBUOY_CARGO;
			}
			else
			{
				*blip_color = iff_get_color_by_team_and_object(shipp->team, Player_ship->team, is_bright, objp);
				*blip_type = BLIP_TYPE_NORMAL_SHIP;
			}

			break;

		case OBJ_WEAPON:
			if ((Weapons[objp->instance].lssm_stage == 2) || (Weapons[objp->instance].lssm_stage == 4))
			{
				*blip_color = &Radar_colors[RCOL_WARPING_SHIP][is_bright];
				*blip_type = BLIP_TYPE_WARPING_SHIP;
			}
			else
			{
				*blip_color = &Radar_colors[RCOL_BOMB][is_bright];
				*blip_type = BLIP_TYPE_BOMB;
			}

			break;

		case OBJ_JUMP_NODE:
			*blip_color = &Radar_colors[RCOL_JUMP_NODE][is_bright];
			*blip_type = BLIP_TYPE_JUMP_NODE;

			break;

		default:
			Error(LOCATION, "Illegal blip type in radar.");
			break;
	}
}

bool radar_project_contact(object* objp, RadarContactProjection& projection)
{
	projection = {};
	if (objp == nullptr || Player_obj == nullptr || Player_ship == nullptr ||
		Player_ai == nullptr || objp->flags[Object::Object_Flags::Should_be_dead]) {
		return false;
	}
	if ((Game_mode & GM_STANDALONE_SERVER) || (Game_mode & GM_LAB) ||
		(MULTIPLAYER_CLIENT &&
		 (Net_player == nullptr ||
		  (Net_player->flags & NETINFO_FLAG_INGAME_JOIN)))) {
		return false;
	}

	auto world_pos = objp->pos;
	auto awacs_level = 1.5f;
	const auto ship_is_visible =
		objp->type == OBJ_SHIP &&
		ship_is_visible_by_team(objp, Player_ship);
	if (!ship_is_visible) {
		awacs_level = awacs_get_level(objp, Player_ship);
	}
	if (awacs_level < 0.0f && !See_all) {
		return false;
	}

	bool mine_in_targetable_range = true;
	switch (objp->type) {
	case OBJ_SHIP:
		if (objp->instance < 0 || objp->instance >= MAX_SHIPS) {
			return false;
		}
		break;
	case OBJ_JUMP_NODE: {
		const auto node = jumpnode_get_by_objp(objp);
		if (node == nullptr || node->IsHidden()) {
			return false;
		}
		break;
	}
	case OBJ_WEAPON: {
		if (objp->instance < 0 || objp->instance >= MAX_WEAPONS) {
			return false;
		}
		const auto& weapon = Weapons[objp->instance];
		if (weapon.weapon_info_index < 0 ||
			weapon.weapon_info_index >= weapon_info_size()) {
			return false;
		}
		const auto& info = Weapon_info[weapon.weapon_info_index];
		if (info.wi_flags[Weapon::Info_Flags::Dont_show_on_radar] ||
			(!info.wi_flags[Weapon::Info_Flags::Show_friendly] &&
			 !iff_x_attacks_y(Player_ship->team, obj_team(objp)))) {
			return false;
		}
		if (!info.is_mine() &&
			!info.wi_flags[Weapon::Info_Flags::Shown_on_radar] &&
			!info.wi_flags[Weapon::Info_Flags::Bomb]) {
			return false;
		}
		if (weapon.lssm_stage == 3) {
			return false;
		}
		if (info.wi_flags[Weapon::Info_Flags::Corkscrew]) {
			world_pos = objp->last_pos;
		}
		break;
	}
	default:
		return false;
	}

	if (HUD_config.rp_dist < 0 || HUD_config.rp_dist >= RR_MAX_RANGES) {
		return false;
	}
	const auto distance = vm_vec_dist(&world_pos, &Player_obj->pos);
	if (!std::isfinite(distance) ||
		distance > Radar_ranges[HUD_config.rp_dist]) {
		return false;
	}
	if (objp->type == OBJ_WEAPON) {
		const auto& info =
			Weapon_info[Weapons[objp->instance].weapon_info_index];
		if (info.is_mine()) {
			if (distance > info.mine_sensors_range) {
				return false;
			}
			mine_in_targetable_range =
				distance <= info.mine_targetable_range;
		}
	}

	auto visibility = VISIBLE;
	if (!mine_in_targetable_range ||
		(objp->type == OBJ_SHIP &&
		 (Ships[objp->instance].flags[
			  Ship::Ship_Flags::Hidden_from_sensors] ||
		  awacs_level < 1.0f))) {
		visibility = DISTORTED;
	}
	if (Player_ship->flags[Ship::Ship_Flags::Primitive_sensors] &&
		!(The_mission.flags[Mission::Mission_Flags::Fullneb])) {
		visibility = VISIBLE;
	}

	projection.visibility = visibility;
	projection.world_position = world_pos;
	projection.world_velocity = objp->phys_info.vel;
	projection.distance = distance;
	return true;
}

void radar_plot_object(object* objp)
{
	RadarContactProjection projection;
	if (!radar_project_contact(objp, projection)) {
		return;
	}

	vec3d pos, tempv;
	matrix eye_orient;
	object_get_eye(&tempv, &eye_orient, Player_obj, false);
	vm_vec_sub(&tempv, &projection.world_position, &Player_obj->pos);
	vm_vec_rotate(&pos, &tempv, &eye_orient);

	if (timestamp_elapsed(Radar_calc_bright_dist_timer)) {
		Radar_calc_bright_dist_timer = _timestamp(1000);
		Radar_bright_range = player_farthest_weapon_range();
		if (Radar_bright_range <= 0) {
			Radar_bright_range = 1500.0f;
		}
	}
	if (N_blips >= MAX_BLIPS) {
		return;
	}

	const auto objnum = OBJ_INDEX(objp);
	auto* b = &Blips[N_blips];
	b->rad = 0;
	b->flags = 0;
	auto blip_bright = projection.distance <= Radar_bright_range ? 1 : 0;
	if (objnum == Player_ai->target_objnum) {
		b->flags |= BLIP_CURRENT_TARGET;
		blip_bright = 1;
	}

	int blip_type = 0;
	radar_stuff_blip_info(
		objp, blip_bright, &b->blip_color, &blip_type);
	if (blip_bright) {
		list_append(&Blip_bright_list[blip_type], b);
	} else {
		list_append(&Blip_dim_list[blip_type], b);
	}

	b->position = pos;
	b->radar_image_2d = -1;
	b->radar_color_image_2d = -1;
	b->radar_image_size = -1;
	b->radar_projection_size = 1.0f;
	b->dist = projection.distance;
	b->objnum = objnum;
	if (Blip_last_update.find(objnum) == Blip_last_update.end()) {
		Blip_last_update.emplace(objnum, TIMESTAMP::never());
	}
	if (projection.visibility == DISTORTED) {
		b->flags |= BLIP_DRAW_DISTORTED;
	}
	if (objp->type == OBJ_SHIP) {
		auto* info =
			&Ship_info[Ships[objp->instance].ship_info_index];
		if (info->radar_image_2d_idx >= 0 ||
			info->radar_color_image_2d_idx >= 0) {
			b->radar_image_2d = info->radar_image_2d_idx;
			b->radar_color_image_2d =
				info->radar_color_image_2d_idx;
			b->radar_image_size = info->radar_image_size;
			b->radar_projection_size =
				info->radar_projection_size_mult;
		}
	}
	++N_blips;
}

void radar_mission_init()
{
	Blip_last_update.clear();

	for (int i=0; i<MAX_RADAR_COLORS; i++ )	{
		for (int j=0; j<MAX_RADAR_LEVELS; j++ )	{
			if (radar_iff_color[i][j][0] >= 0) {
				gr_init_alphacolor( &Radar_colors[i][j], radar_iff_color[i][j][0], radar_iff_color[i][j][1], radar_iff_color[i][j][2], radar_iff_color[i][j][3] );
			} else {
				gr_init_alphacolor( &Radar_colors[i][j], Radar_color_rgb[i][j].r, Radar_color_rgb[i][j].g, Radar_color_rgb[i][j].b, 255 );
			}
		}
	}

	Radar_calc_bright_dist_timer = TIMESTAMP::immediate();
}

void radar_null_nblips()
{
	int i;

	N_blips=0;

	for (i=0; i<MAX_BLIP_TYPES; i++) {
		list_init(&Blip_bright_list[i]);
		list_init(&Blip_dim_list[i]);
	}
}

void radar_frame_init()
{
	radar_null_nblips();
}

HudGaugeRadar::HudGaugeRadar():
HudGauge(HUD_OBJECT_RADAR_STD, HUD_RADAR, false, false, (VM_EXTERNAL | VM_DEAD_VIEW | VM_WARP_CHASE | VM_PADLOCK_ANY | VM_OTHER_SHIP), 255, 255, 255)
{
}

HudGaugeRadar::HudGaugeRadar(int _gauge_object, int r, int g, int b):
HudGauge(_gauge_object, HUD_RADAR, false, false, (VM_EXTERNAL | VM_DEAD_VIEW | VM_WARP_CHASE | VM_PADLOCK_ANY | VM_OTHER_SHIP), r, g, b)
{
}

void HudGaugeRadar::initInfinityIcon()
{
	ubyte sc = lcl_get_font_index(font_num);
	// default to a '*' if the font has no special chars
	// nothing is really close to the infinity symbol...
	if (sc == 0) {
		Radar_infinity_icon = ubyte ('*');
	} else {
		Radar_infinity_icon = sc;
	}
}

void HudGaugeRadar::initRadius(int w, int h)
{
	Radar_radius[0] = w;
	Radar_radius[1] = h;
}

void HudGaugeRadar::initBlipRadius(int normal, int target)
{
	Radar_blip_radius_normal = normal;
	Radar_blip_radius_target = target;
}

void HudGaugeRadar::initDistanceShortOffsets(int x, int y)
{
	Radar_dist_offsets[RR_SHORT][0] = x;
	Radar_dist_offsets[RR_SHORT][1] = y;
}

void HudGaugeRadar::initDistanceLongOffsets(int x, int y)
{
	Radar_dist_offsets[RR_LONG][0] = x;
	Radar_dist_offsets[RR_LONG][1] = y;
}

void HudGaugeRadar::initDistanceInfinityOffsets(int x, int y)
{
	Radar_dist_offsets[RR_INFINITY][0] = x;
	Radar_dist_offsets[RR_INFINITY][1] = y;
}

void HudGaugeRadar::render(float  /*frametime*/, bool /*config*/)
{
}

void HudGaugeRadar::pageIn()
{
}

void HudGaugeRadar::initialize()
{
	int i;

	Radar_death_timer			= TIMESTAMP::never();
	Radar_static_playing		= false;
	Radar_static_next			= TIMESTAMP::never();
	Radar_avail_prev_frame		= true;
	Radar_calc_bright_dist_timer = TIMESTAMP::immediate();

	for ( i=0; i<NUM_FLICKER_TIMERS; i++ ) {
		Radar_flicker_timer[i]=TIMESTAMP::immediate();
		Radar_flicker_on[i]=false;
	}

	HudGauge::initialize();
}

void HudGaugeRadar::drawRange(bool config)
{
	// hud_set_bright_color();
	setGaugeColor(HUD_C_BRIGHT, config);

	int x = position[0];
	int y = position[1];
	float scale = 1.0;

	if (config) {
		std::tie(x, y, scale) = hud_config_convert_coord_sys(position[0], position[1], base_w, base_h);
	}

	int range = config ? RR_INFINITY : HUD_config.rp_dist;

	switch ( range ) {

	case RR_SHORT:
		renderPrintf(x + fl2i(Radar_dist_offsets[RR_SHORT][0] * scale), y + fl2i(Radar_dist_offsets[RR_SHORT][1] * scale), scale, config, "%s", XSTR( "2k", 467));
		break;

	case RR_LONG:
		renderPrintf(x + fl2i(Radar_dist_offsets[RR_LONG][0] * scale), y + fl2i(Radar_dist_offsets[RR_LONG][1] * scale), scale, config, "%s", XSTR( "10k", 468));
		break;

	case RR_INFINITY:
		if (Unicode_text_mode) {
			// This escape sequence is the UTF-8 encoding of the infinity symbol. We can't use u8 yet since VS2013 doesn't support it
			renderPrintf(x + fl2i(Radar_dist_offsets[RR_INFINITY][0] * scale), y + fl2i(Radar_dist_offsets[RR_INFINITY][1] * scale), scale, config, "\xE2\x88\x9E");
		} else {
			renderPrintf(x + fl2i(Radar_dist_offsets[RR_INFINITY][0] * scale), y + fl2i(Radar_dist_offsets[RR_INFINITY][1] * scale), scale, config, "%c", Radar_infinity_icon);
		}
		break;

	default:
		Error(LOCATION, "Unknown radar range: %d!\n", HUD_config.rp_dist);
		break;
	}
}

/**
 * @brief Return if the specified object is visible on the radar
 *
 * @param objp The object which should be checked
 * @return A RadarVisibility enum specifying the visibility of the specified object
 */
RadarVisibility radar_is_visible( object *objp )
{
	Assert( objp != nullptr );

	if (objp->flags[Object::Object_Flags::Should_be_dead])
	{
		return NOT_VISIBLE;
	}

	if (Player_obj == nullptr)
		return NOT_VISIBLE;

	vec3d pos, tempv;
	float awacs_level, dist, max_radar_dist;
	vec3d world_pos = objp->pos;

	// get team-wide awacs level for the object if not ship
	int ship_is_visible = 0;
	if (objp->type == OBJ_SHIP) {
		if (Player_ship != nullptr) {
			if (ship_is_visible_by_team(objp, Player_ship)) {
				ship_is_visible = 1;
			}
		}
	}

	// only check awacs level if ship is not visible by team
	awacs_level = 1.5f;
	if (Player_ship != nullptr && !ship_is_visible) {
		awacs_level = awacs_get_level(objp, Player_ship);
	}

	// if the awacs level is unviewable - bail
	if(awacs_level < 0.0f && !See_all){
		return NOT_VISIBLE;
	}

	// Apply object type filters	
	switch (objp->type)
	{
		case OBJ_SHIP:
			if (Ships[objp->instance].flags[Ship::Ship_Flags::Stealth])
				return NOT_VISIBLE;

			// Ships that are warp in in are not visible on the radar
			if (Ships[objp->instance].is_arriving(ship::warpstage::STAGE1, false))
				return NOT_VISIBLE;

			break;
		
		case OBJ_JUMP_NODE:
		{
			auto jnp = jumpnode_get_by_objp(objp);
			
			// don't plot missing or hidden jump nodes
			if ( !jnp || jnp->IsHidden() )
				return NOT_VISIBLE;

			// filter jump nodes here if required
			break;
		}

		case OBJ_WEAPON:
		{
			// if not a bomb, return
			if ( !(Weapon_info[Weapons[objp->instance].weapon_info_index].wi_flags[Weapon::Info_Flags::Shown_on_radar]) )
				if ( !(Weapon_info[Weapons[objp->instance].weapon_info_index].wi_flags[Weapon::Info_Flags::Bomb]) )
					return NOT_VISIBLE;

			// if explicitly hidden, return
			if (Weapon_info[Weapons[objp->instance].weapon_info_index].wi_flags[Weapon::Info_Flags::Dont_show_on_radar])
				return NOT_VISIBLE;

			// if we don't attack the bomb, return
			if ( (!(Weapon_info[Weapons[objp->instance].weapon_info_index].wi_flags[Weapon::Info_Flags::Show_friendly])) && (!iff_x_attacks_y(Player_ship->team, obj_team(objp))))
				return NOT_VISIBLE;

			// if a local ssm is in subspace, return
			if (Weapons[objp->instance].lssm_stage == 3)
				return NOT_VISIBLE;

			break;
		}

		// if any other kind of object, don't show it on radar
		default:
			return NOT_VISIBLE;
	}
	
	vm_vec_sub(&tempv, &world_pos, &Player_obj->pos);
	vm_vec_rotate(&pos, &tempv, &Player_obj->orient);

	// Apply range filter
	dist = vm_vec_dist(&world_pos, &Player_obj->pos);
	max_radar_dist = Radar_ranges[HUD_config.rp_dist];
	if (dist > max_radar_dist) {
		return NOT_VISIBLE;
	}
	
	if (objp->type == OBJ_SHIP)
	{
		// ships specifically hidden from sensors
		if (Ships[objp->instance].flags[Ship::Ship_Flags::Hidden_from_sensors])
			return DISTORTED;

		// determine if its AWACS distorted
		if (awacs_level < 1.0f)
			return DISTORTED;
	}
	
	return VISIBLE;
}
