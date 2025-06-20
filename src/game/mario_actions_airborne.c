#include <PR/ultratypes.h>

#include "sm64.h"
#include "area.h"
#include "audio/external.h"
#include "camera.h"
#include "engine/graph_node.h"
#include "engine/math_util.h"
#include "game_init.h"
#include "interaction.h"
#include "level_update.h"
#include "mario.h"
#include "mario_step.h"
#include "save_file.h"
#include "rumble_init.h"
#include "bettercamera.h"
#include "behavior_table.h"
#include "object_helpers.h"
#include "pc/debuglog.h"
#include "pc/configfile.h"
#include "pc/network/network.h"
#include "pc/lua/smlua.h"
#include "hardcoded.h"

/* |description|
Plays a spinning sound at specific animation frames for flips (usually side flips or certain jump flips).
If the current animation frame matches any of the specified frames, it triggers `SOUND_ACTION_SPIN`
|descriptionEnd| */
void play_flip_sounds(struct MarioState *m, s16 frame1, s16 frame2, s16 frame3) {
    if (!m) { return; }
    s32 animFrame = m->marioObj->header.gfx.animInfo.animFrame;
    if (animFrame == frame1 || animFrame == frame2 || animFrame == frame3) {
        play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
    }
}

/* |description|
Plays a unique sound when Mario has fallen a significant distance without being invulnerable, twirling, or flying.
If the fall exceeds a threshold, triggers a "long fall" exclamation. Also sets a flag to prevent repeated triggering
|descriptionEnd| */
void play_far_fall_sound(struct MarioState *m) {
    if (!m) { return; }
    u32 action = m->action;
    if (!(action & ACT_FLAG_INVULNERABLE) && action != ACT_TWIRLING && action != ACT_FLYING
        && !(m->flags & MARIO_UNKNOWN_18)) {
        if (m->peakHeight - m->pos[1] > 1150.0f) {
            play_character_sound(m, CHAR_SOUND_WAAAOOOW);
            m->flags |= MARIO_UNKNOWN_18;
        }
    }
}

#ifndef VERSION_JP
/* |description|
Plays a knockback sound effect if Mario is hit or knocked back with significant velocity. The specific sound differs
depending on whether Mario's forward velocity is high enough to be considered a strong knockback
|descriptionEnd| */
void play_knockback_sound(struct MarioState *m) {
    if (!m) { return; }
    if (m->actionArg == 0 && (m->forwardVel <= -28.0f || m->forwardVel >= 28.0f)) {
        play_character_sound_if_no_flag(m, CHAR_SOUND_DOH, MARIO_MARIO_SOUND_PLAYED);
    } else {
        play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
    }
}
#endif

/* |description|
Allows Mario to 'lava boost' off a lava wall, reorienting him to face away from the wall and adjusting forward velocity.
Increases Mario's hurt counter if he's not metal, plays a burning sound, and transitions his action to `ACT_LAVA_BOOST`.
Useful for handling collisions with lava walls, giving Mario a strong upward/forward boost at the cost of health
|descriptionEnd| */
s32 lava_boost_on_wall(struct MarioState *m) {
    if (!m) { return 0; }
    bool allow = true;
    smlua_call_event_hooks_mario_param_and_int_ret_bool(HOOK_ALLOW_HAZARD_SURFACE, m, HAZARD_TYPE_LAVA_WALL, &allow);
    if ((!allow) || gDjuiInMainMenu) { return FALSE; }
    m->faceAngle[1] = atan2s(m->wallNormal[2], m->wallNormal[0]);

    if (m->forwardVel < 24.0f) {
        m->forwardVel = 24.0f;
    }

    if (!(m->flags & MARIO_METAL_CAP)) {
        m->hurtCounter += (m->flags & MARIO_CAP_ON_HEAD) ? 12 : 18;
    }

    play_character_sound(m, CHAR_SOUND_ON_FIRE);
    update_mario_sound_and_camera(m);
    return drop_and_set_mario_action(m, ACT_LAVA_BOOST, 1);
}

/* |description|
Evaluates whether Mario should take fall damage based on the height difference between his peak and current position.
If the fall is large enough and does not occur over burning surfaces or while twirling, Mario may get hurt or enter
a hard fall action. If the fall is significant but not extreme, minimal damage and a squish effect may be applied.
Useful for determining if Mario's fall warrants a health penalty or a special landing action
|descriptionEnd| */
s32 check_fall_damage(struct MarioState *m, u32 hardFallAction) {
    if (!m) { return 0; }
    
    f32 fallHeight;
    f32 damageHeight;

    fallHeight = m->peakHeight - m->pos[1];

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"

    //! Never true
    /*if (m->actionState == ACT_GROUND_POUND) {
        damageHeight = 600.0f;
    } else {*/
        damageHeight = 1150.0f;
    //}

#pragma GCC diagnostic pop

    if (m->action != ACT_TWIRLING && m->floor && m->floor->type != SURFACE_BURNING) {
        if (m->vel[1] < -55.0f) {
            if (fallHeight > 3000.0f) {
                m->hurtCounter += (m->flags & MARIO_CAP_ON_HEAD) ? 16 : 24;
                queue_rumble_data_mario(m, 5, 80);
                if (m->playerIndex == 0) { set_camera_shake_from_hit(SHAKE_FALL_DAMAGE); }
                play_character_sound(m, CHAR_SOUND_ATTACKED);
                return drop_and_set_mario_action(m, hardFallAction, 4);
            } else if (fallHeight > damageHeight && !mario_floor_is_slippery(m)) {
                m->hurtCounter += (m->flags & MARIO_CAP_ON_HEAD) ? 8 : 12;
                m->squishTimer = 30;

                queue_rumble_data_mario(m, 5, 80);
                if (m->playerIndex == 0) { set_camera_shake_from_hit(SHAKE_FALL_DAMAGE); }
                play_character_sound(m, CHAR_SOUND_ATTACKED);
            }
        }
    }

    return FALSE;
}

/* |description|
Checks if Mario should perform a kick or a dive while in mid-air, depending on his current forward velocity.
Pressing the B button in the air can trigger a jump kick (at lower speeds) or a dive (at higher speeds)
|descriptionEnd| */
s32 check_kick_or_dive_in_air(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, m->forwardVel > 28.0f ? ACT_DIVE : ACT_JUMP_KICK, 0);
    }
    return FALSE;
}

/* |description|
Determines whether Mario should become stuck in the ground after landing, specifically for soft terrain such as snow
or sand, provided certain conditions are met (height of the fall, normal of the floor, etc.).
Returns true if Mario should be stuck, false otherwise
|descriptionEnd| */
s32 should_get_stuck_in_ground(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->floor == NULL) { return FALSE; }

    u32 terrainType = m->area->terrainType & TERRAIN_MASK;
    struct Surface *floor = m->floor;
    s32 flags = floor->flags;
    s32 type = floor->type;

    if ((terrainType == TERRAIN_SNOW || terrainType == TERRAIN_SAND)
        && type != SURFACE_BURNING && SURFACE_IS_NOT_HARD(type)) {
        if (!(flags & 0x01) && m->peakHeight - m->pos[1] > 1000.0f && floor->normal.y >= 0.8660254f) {
            return TRUE;
        }
    }

    return FALSE;
}

/* |description|
Checks if Mario should get stuck in the ground after a large fall onto soft terrain (like snow or sand) or if he
should just proceed with regular fall damage calculations. If the terrain and height conditions are met, Mario's
action changes to being stuck in the ground. Otherwise, normal fall damage logic applies
|descriptionEnd| */
s32 check_fall_damage_or_get_stuck(struct MarioState *m, u32 hardFallAction) {
    if (!m) { return 0; }
    if (should_get_stuck_in_ground(m)) {
#ifdef VERSION_JP
        play_character_sound(m, CHAR_SOUND_OOOF);
#else
        play_character_sound(m, CHAR_SOUND_OOOF2);
#endif
        set_mario_particle_flags(m, PARTICLE_MIST_CIRCLE, FALSE);
        drop_and_set_mario_action(m, ACT_FEET_STUCK_IN_GROUND, 0);

        queue_rumble_data_mario(m, 5, 80);
        return TRUE;
    }

    return check_fall_damage(m, hardFallAction);
}

/* |description|
Checks for the presence of a horizontal wind surface under Mario. If found, applies a push force to Mario's horizontal
velocity. Caps speed at certain thresholds, updates Mario's forward velocity and yaw for sliding/wind movement
|descriptionEnd| */
s32 check_horizontal_wind(struct MarioState *m) {
    if (!m) { return 0; }
    struct Surface *floor;
    f32 speed;
    s16 pushAngle;
    bool allow = true;
    smlua_call_event_hooks_mario_param_and_int_ret_bool(HOOK_ALLOW_HAZARD_SURFACE, m, HAZARD_TYPE_HORIZONTAL_WIND, &allow);
    if (!allow) {
    	return FALSE;
    }

    floor = m->floor;
    

    if (floor && floor->type == SURFACE_HORIZONTAL_WIND) {
        pushAngle = floor->force << 8;

        m->slideVelX += 1.2f * sins(pushAngle);
        m->slideVelZ += 1.2f * coss(pushAngle);

        speed = sqrtf(m->slideVelX * m->slideVelX + m->slideVelZ * m->slideVelZ);

        if (speed > 48.0f) {
            m->slideVelX = m->slideVelX * 48.0f / speed;
            m->slideVelZ = m->slideVelZ * 48.0f / speed;
            speed = 32.0f; //! This was meant to be 48?
        } else if (speed > 32.0f) {
            speed = 32.0f;
        }

        m->vel[0] = m->slideVelX;
        m->vel[2] = m->slideVelZ;
        m->slideYaw = atan2s(m->slideVelZ, m->slideVelX);
        m->forwardVel = speed * coss(m->faceAngle[1] - m->slideYaw);

#ifdef VERSION_JP
        play_sound(SOUND_ENV_WIND2, m->marioObj->header.gfx.cameraToObject);
#endif
        return TRUE;
    }

    return FALSE;
}

/* |description|
Updates Mario's air movement while allowing him to turn. Checks horizontal wind and applies a moderate amount of drag,
approaches the forward velocity toward zero if no input is pressed, and modifies forward velocity/angle based on stick input
|descriptionEnd| */
void update_air_with_turn(struct MarioState *m) {
    if (!m) { return; }
    f32 dragThreshold;
    s16 intendedDYaw;
    f32 intendedMag;

    if (!check_horizontal_wind(m)) {
        dragThreshold = m->action == ACT_LONG_JUMP ? 48.0f : 32.0f;
        m->forwardVel = approach_f32(m->forwardVel, 0.0f, 0.35f, 0.35f);

        if (m->input & INPUT_NONZERO_ANALOG) {
            intendedDYaw = m->intendedYaw - m->faceAngle[1];
            intendedMag = m->intendedMag / 32.0f;

            m->forwardVel += 1.5f * coss(intendedDYaw) * intendedMag;
            m->faceAngle[1] += 512.0f * sins(intendedDYaw) * intendedMag;
        }

        //! Uncapped air speed. Net positive when moving forward.
        if (m->forwardVel > dragThreshold) {
            m->forwardVel -= 1.0f;
        }
        if (m->forwardVel < -16.0f) {
            m->forwardVel += 2.0f;
        }

        m->vel[0] = m->slideVelX = m->forwardVel * sins(m->faceAngle[1]);
        m->vel[2] = m->slideVelZ = m->forwardVel * coss(m->faceAngle[1]);
    }
}

/* |description|
Updates Mario's air movement without directly turning his facing angle to match his intended yaw. Instead, Mario can
move sideways relative to his current facing direction. Also checks horizontal wind and applies drag
|descriptionEnd| */
void update_air_without_turn(struct MarioState *m) {
    if (!m) { return; }
    f32 sidewaysSpeed = 0.0f;
    f32 dragThreshold;
    s16 intendedDYaw;
    f32 intendedMag;

    if (!check_horizontal_wind(m)) {
        dragThreshold = m->action == ACT_LONG_JUMP ? 48.0f : 32.0f;
        m->forwardVel = approach_f32(m->forwardVel, 0.0f, 0.35f, 0.35f);

        if (m->input & INPUT_NONZERO_ANALOG) {
            intendedDYaw = m->intendedYaw - m->faceAngle[1];
            intendedMag = m->intendedMag / 32.0f;

            m->forwardVel += intendedMag * coss(intendedDYaw) * 1.5f;
            sidewaysSpeed = intendedMag * sins(intendedDYaw) * 10.0f;
        }

        //! Uncapped air speed. Net positive when moving forward.
        if (m->forwardVel > dragThreshold) {
            m->forwardVel -= 1.0f;
        }
        if (m->forwardVel < -16.0f) {
            m->forwardVel += 2.0f;
        }

        m->slideVelX = m->forwardVel * sins(m->faceAngle[1]);
        m->slideVelZ = m->forwardVel * coss(m->faceAngle[1]);

        m->slideVelX += sidewaysSpeed * sins(m->faceAngle[1] + 0x4000);
        m->slideVelZ += sidewaysSpeed * coss(m->faceAngle[1] + 0x4000);

        m->vel[0] = m->slideVelX;
        m->vel[2] = m->slideVelZ;
    }
}


/* |description|
Updates Mario's movement when in actions like lava boost or twirling in mid-air. Applies player input to adjust forward velocity
and facing angle, but in a more restricted manner compared to standard jump movement. Used by `ACT_LAVA_BOOST` and `ACT_TWIRLING`
|descriptionEnd| */
void update_lava_boost_or_twirling(struct MarioState *m) {
    if (!m) { return; }
    s16 intendedDYaw;
    f32 intendedMag;

    if (m->input & INPUT_NONZERO_ANALOG) {
        intendedDYaw = m->intendedYaw - m->faceAngle[1];
        intendedMag = m->intendedMag / 32.0f;

        m->forwardVel += coss(intendedDYaw) * intendedMag;
        m->faceAngle[1] += sins(intendedDYaw) * intendedMag * 1024.0f;

        if (m->forwardVel < 0.0f) {
            m->faceAngle[1] += 0x8000;
            m->forwardVel *= -1.0f;
        }

        if (m->forwardVel > 32.0f) {
            m->forwardVel -= 2.0f;
        }
    }

    m->vel[0] = m->slideVelX = m->forwardVel * sins(m->faceAngle[1]);
    m->vel[2] = m->slideVelZ = m->forwardVel * coss(m->faceAngle[1]);
}

/* |description|
Calculates and applies a change in Mario's yaw while flying, based on horizontal stick input. Approaches a target yaw velocity
and sets Mario's roll angle to simulate banking turns. This results in a more natural, curved flight path
|descriptionEnd| */
void update_flying_yaw(struct MarioState *m) {
    if (!m) { return; }
    s16 targetYawVel = -(s16)(m->controller->stickX * (m->forwardVel / 4.0f));

    if (targetYawVel > 0) {
        if (m->angleVel[1] < 0) {
            m->angleVel[1] += 0x40;
            if (m->angleVel[1] > 0x10) {
                m->angleVel[1] = 0x10;
            }
        } else {
            m->angleVel[1] = approach_s32(m->angleVel[1], targetYawVel, 0x10, 0x20);
        }
    } else if (targetYawVel < 0) {
        if (m->angleVel[1] > 0) {
            m->angleVel[1] -= 0x40;
            if (m->angleVel[1] < -0x10) {
                m->angleVel[1] = -0x10;
            }
        } else {
            m->angleVel[1] = approach_s32(m->angleVel[1], targetYawVel, 0x20, 0x10);
        }
    } else {
        m->angleVel[1] = approach_s32(m->angleVel[1], 0, 0x40, 0x40);
    }

    m->faceAngle[1] += m->angleVel[1];
    m->faceAngle[2] = 20 * -m->angleVel[1];
}

/* |description|
Calculates and applies a change in Mario's pitch while flying, based on vertical stick input. Approaches a target pitch velocity
and clamps the final pitch angle to a certain range, simulating a smooth flight control
|descriptionEnd| */
void update_flying_pitch(struct MarioState *m) {
    if (!m) { return; }
    s16 targetPitchVel = -(s16)(m->controller->stickY * (m->forwardVel / 5.0f));

    if (targetPitchVel > 0) {
        if (m->angleVel[0] < 0) {
            m->angleVel[0] += 0x40;
            if (m->angleVel[0] > 0x20) {
                m->angleVel[0] = 0x20;
            }
        } else {
            m->angleVel[0] = approach_s32(m->angleVel[0], targetPitchVel, 0x20, 0x40);
        }
    } else if (targetPitchVel < 0) {
        if (m->angleVel[0] > 0) {
            m->angleVel[0] -= 0x40;
            if (m->angleVel[0] < -0x20) {
                m->angleVel[0] = -0x20;
            }
        } else {
            m->angleVel[0] = approach_s32(m->angleVel[0], targetPitchVel, 0x40, 0x20);
        }
    } else {
        m->angleVel[0] = approach_s32(m->angleVel[0], 0, 0x40, 0x40);
    }
}

/* |description|
Handles the complete flying logic for Mario (usually with the wing cap). Continuously updates pitch and yaw based on controller input,
applies drag, and adjusts forward velocity. Also updates Mario's model angles for flight animations
|descriptionEnd| */
void update_flying(struct MarioState *m) {
    if (!m) { return; }
    UNUSED u32 unused;

    update_flying_pitch(m);
    update_flying_yaw(m);

    m->forwardVel -= 2.0f * ((f32) m->faceAngle[0] / 0x4000) + 0.1f;
    m->forwardVel -= 0.5f * (1.0f - coss(m->angleVel[1]));

    if (m->forwardVel < 0.0f) {
        m->forwardVel = 0.0f;
    }

    if (m->forwardVel > 16.0f) {
        m->faceAngle[0] += (m->forwardVel - 32.0f) * 6.0f;
    } else if (m->forwardVel > 4.0f) {
        m->faceAngle[0] += (m->forwardVel - 32.0f) * 10.0f;
    } else {
        m->faceAngle[0] -= 0x400;
    }

    m->faceAngle[0] += m->angleVel[0];

    if (m->faceAngle[0] > 0x2AAA) {
        m->faceAngle[0] = 0x2AAA;
    }
    if (m->faceAngle[0] < -0x2AAA) {
        m->faceAngle[0] = -0x2AAA;
    }

    m->vel[0] = m->forwardVel * coss(m->faceAngle[0]) * sins(m->faceAngle[1]);
    m->vel[1] = m->forwardVel * sins(m->faceAngle[0]);
    m->vel[2] = m->forwardVel * coss(m->faceAngle[0]) * coss(m->faceAngle[1]);

    m->slideVelX = m->vel[0];
    m->slideVelZ = m->vel[2];
}

/* |description|
Performs a standard step update for air actions without knockback, typically used for jumps or freefalls.
Updates Mario's velocity (and possibly checks horizontal wind), then calls `perform_air_step` with given `stepArg`.
Handles how Mario lands, hits walls, grabs ledges, or grabs ceilings. Optionally sets an animation
|descriptionEnd| */
u32 common_air_action_step(struct MarioState *m, u32 landAction, s32 animation, u32 stepArg) {
    if (!m) { return 0; }
    u32 stepResult;

    update_air_without_turn(m);

    stepResult = perform_air_step(m, stepArg);
    if (m->action == ACT_BUBBLED && stepResult == AIR_STEP_HIT_LAVA_WALL) {
        stepResult = AIR_STEP_HIT_WALL;
    }

    switch (stepResult) {
        case AIR_STEP_NONE:
            set_character_animation(m, animation);
            break;

        case AIR_STEP_LANDED:
            if (!check_fall_damage_or_get_stuck(m, ACT_HARD_BACKWARD_GROUND_KB)) {
                set_mario_action(m, landAction, 0);
            }
            break;

        case AIR_STEP_HIT_WALL:
            set_character_animation(m, animation);

            if (m->forwardVel > 16.0f) {
                if (gServerSettings.bouncyLevelBounds == BOUNCY_LEVEL_BOUNDS_OFF || m->wall != NULL) {
                    queue_rumble_data_mario(m, 5, 40);
                    mario_bonk_reflection(m, FALSE);
                    m->faceAngle[1] += 0x8000;
                }

                if (m->wall != NULL) {
                    set_mario_action(m, ACT_AIR_HIT_WALL, 0);
                } else {
                    if (m->vel[1] > 0.0f) {
                        m->vel[1] = 0.0f;
                    }

                    //! Hands-free holding. Bonking while no wall is referenced
                    // sets Mario's action to a non-holding action without
                    // dropping the object, causing the hands-free holding
                    // glitch. This can be achieved using an exposed ceiling,
                    // out of bounds, grazing the bottom of a wall while
                    // falling such that the final quarter step does not find a
                    // wall collision, or by rising into the top of a wall such
                    // that the final quarter step detects a ledge, but you are
                    // not able to ledge grab it.
                    if (gServerSettings.bouncyLevelBounds == BOUNCY_LEVEL_BOUNDS_OFF) {
                        if (m->forwardVel >= 38.0f) {
                            set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
                            set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
                        } else {
                            if (m->forwardVel > 8.0f) {
                                mario_set_forward_vel(m, -8.0f);
                            }
                            return set_mario_action(m, ACT_SOFT_BONK, 0);
                        }
                    }
                }
            } else {
                mario_set_forward_vel(m, 0.0f);
            }
            break;

        case AIR_STEP_GRABBED_LEDGE:
            set_character_animation(m, CHAR_ANIM_IDLE_ON_LEDGE);
            drop_and_set_mario_action(m, ACT_LEDGE_GRAB, 0);
            break;

        case AIR_STEP_GRABBED_CEILING:
            set_mario_action(m, ACT_START_HANGING, 0);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return stepResult;
}

s32 act_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (check_kick_or_dive_in_air(m)) {
        return TRUE;
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    common_air_action_step(m, ACT_JUMP_LAND, CHAR_ANIM_SINGLE_JUMP,
                           AIR_STEP_CHECK_LEDGE_GRAB | AIR_STEP_CHECK_HANG);
    return FALSE;
}

s32 act_double_jump(struct MarioState *m) {
    if (!m) { return 0; }
    s32 animation = (m->vel[1] >= 0.0f)
        ? CHAR_ANIM_DOUBLE_JUMP_RISE
        : CHAR_ANIM_DOUBLE_JUMP_FALL;

    if (check_kick_or_dive_in_air(m)) {
        return TRUE;
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_HOOHOO);
    common_air_action_step(m, ACT_DOUBLE_JUMP_LAND, animation,
                           AIR_STEP_CHECK_LEDGE_GRAB | AIR_STEP_CHECK_HANG);
    return FALSE;
}

s32 act_triple_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (m == &gMarioStates[0] && m->specialTripleJump) {
        return set_mario_action(m, ACT_SPECIAL_TRIPLE_JUMP, 0);
    }

    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

#ifndef VERSION_JP
    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
#else
    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_YAHOO);
#endif

    common_air_action_step(m, ACT_TRIPLE_JUMP_LAND, CHAR_ANIM_TRIPLE_JUMP, 0);
    if (m->action == ACT_TRIPLE_JUMP_LAND) {
        queue_rumble_data_mario(m, 5, 40);
    }
    play_flip_sounds(m, 2, 8, 20);
    return FALSE;
}

s32 act_backflip(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_YAH_WAH_HOO);
    common_air_action_step(m, ACT_BACKFLIP_LAND, CHAR_ANIM_BACKFLIP, 0);

    if (m->action == ACT_BACKFLIP_LAND) {
        queue_rumble_data_mario(m, 5, 40);
    }
    play_flip_sounds(m, 2, 3, 17);
    return FALSE;
}

s32 act_freefall(struct MarioState *m) {
    if (!m) { return 0; }
    s32 animation = 0;

    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    switch (m->actionArg) {
        case 0:
            animation = CHAR_ANIM_GENERAL_FALL;
            break;
        case 1:
            animation = CHAR_ANIM_FALL_FROM_SLIDE;
            break;
        case 2:
            animation = CHAR_ANIM_FALL_FROM_SLIDE_KICK;
            break;
    }

    common_air_action_step(m, ACT_FREEFALL_LAND, animation, AIR_STEP_CHECK_LEDGE_GRAB);
    return FALSE;
}

s32 act_hold_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->marioObj->oInteractStatus & INT_STATUS_MARIO_DROP_OBJECT) {
        return drop_and_set_mario_action(m, ACT_FREEFALL, 0);
    }

    if ((m->input & INPUT_B_PRESSED) && !(m->heldObj != NULL && m->heldObj->oInteractionSubtype & INT_SUBTYPE_HOLDABLE_NPC)) {
        return set_mario_action(m, ACT_AIR_THROW, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return drop_and_set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    common_air_action_step(m, ACT_HOLD_JUMP_LAND, CHAR_ANIM_JUMP_WITH_LIGHT_OBJ,
                           AIR_STEP_CHECK_LEDGE_GRAB);
    return FALSE;
}

s32 act_hold_freefall(struct MarioState *m) {
    if (!m) { return 0; }
    s32 animation;
    if (m->actionArg == 0) {
        animation = CHAR_ANIM_FALL_WITH_LIGHT_OBJ;
    } else {
        animation = CHAR_ANIM_FALL_FROM_SLIDING_WITH_LIGHT_OBJ;
    }

    if (m->marioObj->oInteractStatus & INT_STATUS_MARIO_DROP_OBJECT) {
        return drop_and_set_mario_action(m, ACT_FREEFALL, 0);
    }

    if ((m->input & INPUT_B_PRESSED) && !(m->heldObj != NULL && m->heldObj->oInteractionSubtype & INT_SUBTYPE_HOLDABLE_NPC)) {
        return set_mario_action(m, ACT_AIR_THROW, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return drop_and_set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    common_air_action_step(m, ACT_HOLD_FREEFALL_LAND, animation, AIR_STEP_CHECK_LEDGE_GRAB);
    return FALSE;
}

s32 act_side_flip(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);

    if (common_air_action_step(m, ACT_SIDE_FLIP_LAND, CHAR_ANIM_SLIDEFLIP, AIR_STEP_CHECK_LEDGE_GRAB)
        != AIR_STEP_GRABBED_LEDGE) {
        m->marioObj->header.gfx.angle[1] += 0x8000;
    }

    // This must be one line to match on -O2
    // clang-format off
    if (m->marioObj->header.gfx.animInfo.animFrame == 6) play_sound(SOUND_ACTION_SIDE_FLIP_UNK, m->marioObj->header.gfx.cameraToObject);
    // clang-format on
    return FALSE;
}

s32 act_wall_kick_air(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_jump_sound(m);
    common_air_action_step(m, ACT_JUMP_LAND, CHAR_ANIM_SLIDEJUMP, AIR_STEP_CHECK_LEDGE_GRAB);
    return FALSE;
}

s32 act_long_jump(struct MarioState *m) {
    if (!m) { return 0; }
    s32 animation;
    if (!m->marioObj->oMarioLongJumpIsSlow) {
        animation = CHAR_ANIM_FAST_LONGJUMP;
    } else {
        animation = CHAR_ANIM_SLOW_LONGJUMP;
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_YAHOO);

    if (m->floor->type == SURFACE_VERTICAL_WIND && m->actionState == 0) {
        play_character_sound(m, CHAR_SOUND_HERE_WE_GO);
        m->actionState = 1;
    }

    common_air_action_step(m, ACT_LONG_JUMP_LAND, animation, AIR_STEP_CHECK_LEDGE_GRAB);

    if (m->action == ACT_LONG_JUMP_LAND) {
        queue_rumble_data_mario(m, 5, 40);
    }
    return FALSE;
}

s32 act_riding_shell_air(struct MarioState *m) {
    if (!m) { return 0; }
    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    set_character_animation(m, CHAR_ANIM_JUMP_RIDING_SHELL);

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_RIDING_SHELL_GROUND, 1);
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, 0.0f);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    m->marioObj->header.gfx.pos[1] += 42.0f;
    return FALSE;
}

s32 act_twirling(struct MarioState *m) {
    if (!m) { return 0; }
    s16 startTwirlYaw = m->twirlYaw;
    s16 yawVelTarget;

    if (m->input & INPUT_A_DOWN) {
        yawVelTarget = 0x2000;
    } else {
        yawVelTarget = 0x1800;
    }

    m->angleVel[1] = approach_s32(m->angleVel[1], yawVelTarget, 0x200, 0x200);
    m->twirlYaw += m->angleVel[1];

    set_character_animation(m, m->actionArg == 0 ? CHAR_ANIM_START_TWIRL : CHAR_ANIM_TWIRL);
    if (is_anim_past_end(m)) {
        m->actionArg = 1;
    }

    if (startTwirlYaw > m->twirlYaw) {
        play_sound(SOUND_ACTION_TWIRL, m->marioObj->header.gfx.cameraToObject);
    }

    update_lava_boost_or_twirling(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_TWIRL_LAND, 0);
            break;

        case AIR_STEP_HIT_WALL:
            mario_bonk_reflection(m, FALSE);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    m->marioObj->header.gfx.angle[1] += m->twirlYaw;
#ifdef VERSION_SH
    reset_rumble_timers();
#endif
    return FALSE;
}

s32 act_dive(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->actionArg == 0) {
        play_mario_sound(m, SOUND_ACTION_THROW, CHAR_SOUND_HOOHOO);
    } else {
        play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    }

    set_character_animation(m, CHAR_ANIM_DIVE);
    if (mario_check_object_grab(m)) {
        mario_grab_used_object(m);
        if (m->heldObj != NULL) {
            m->marioBodyState->grabPos = GRAB_POS_LIGHT_OBJ;
            if (m->action != ACT_DIVE) {
                return TRUE;
            }
        }
    }

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_NONE:
            if (m->vel[1] < 0.0f && m->faceAngle[0] > -0x2AAA) {
                m->faceAngle[0] -= 0x200;
                if (m->faceAngle[0] < -0x2AAA) {
                    m->faceAngle[0] = -0x2AAA;
                }
            }
            m->marioObj->header.gfx.angle[0] = -m->faceAngle[0];
            break;

        case AIR_STEP_LANDED:
            if (should_get_stuck_in_ground(m) && m->faceAngle[0] == -0x2AAA) {
                queue_rumble_data_mario(m, 5, 80);
#ifdef VERSION_JP
                play_character_sound(m, CHAR_SOUND_OOOF);
#else
                play_character_sound(m, CHAR_SOUND_OOOF2);
#endif
                set_mario_particle_flags(m, PARTICLE_MIST_CIRCLE, FALSE);
                drop_and_set_mario_action(m, ACT_HEAD_STUCK_IN_GROUND, 0);
            } else if (!check_fall_damage(m, ACT_HARD_FORWARD_GROUND_KB)) {
                if (m->heldObj == NULL) {
                    set_mario_action(m, ACT_DIVE_SLIDE, 0);
                } else {
                    set_mario_action(m, ACT_DIVE_PICKING_UP, 0);
                }
            }
            m->faceAngle[0] = 0;
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }

            mario_bonk_reflection(m, TRUE);
            m->faceAngle[0] = 0;

            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }

            set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
            drop_and_set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return FALSE;
}

s32 act_air_throw(struct MarioState *m) {
    if (!m) { return 0; }
    if (++(m->actionTimer) == 4) {
        mario_throw_held_object(m);
    }

    play_character_sound_if_no_flag(m, CHAR_SOUND_WAH2, MARIO_MARIO_SOUND_PLAYED);
    set_character_animation(m, CHAR_ANIM_THROW_LIGHT_OBJECT);
    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (!check_fall_damage_or_get_stuck(m, ACT_HARD_BACKWARD_GROUND_KB)) {
                m->action = ACT_AIR_THROW_LAND;
            }
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, 0.0f);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return FALSE;
}

s32 act_water_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->forwardVel < 15.0f) {
        mario_set_forward_vel(m, 15.0f);
    }

    play_mario_sound(m, SOUND_ACTION_UNKNOWN432, 0);
    set_character_animation(m, CHAR_ANIM_SINGLE_JUMP);

    switch (perform_air_step(m, AIR_STEP_CHECK_LEDGE_GRAB)) {
        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_JUMP_LAND, 0);
            if (m->playerIndex == 0) { set_camera_mode(m->area->camera, m->area->camera->defMode, 1); }
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, 15.0f);
            break;

        case AIR_STEP_GRABBED_LEDGE:
#ifndef VERSION_JP
            set_character_animation(m, CHAR_ANIM_IDLE_ON_LEDGE);
#endif
            set_mario_action(m, ACT_LEDGE_GRAB, 0);
            if (m->playerIndex == 0) { set_camera_mode(m->area->camera, m->area->camera->defMode, 1); }
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return FALSE;
}

s32 act_hold_water_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->marioObj->oInteractStatus & INT_STATUS_MARIO_DROP_OBJECT) {
        return drop_and_set_mario_action(m, ACT_FREEFALL, 0);
    }

    if (m->forwardVel < 15.0f) {
        mario_set_forward_vel(m, 15.0f);
    }

    play_mario_sound(m, SOUND_ACTION_UNKNOWN432, 0);
    set_character_animation(m, CHAR_ANIM_JUMP_WITH_LIGHT_OBJ);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_HOLD_JUMP_LAND, 0);
            if (m->playerIndex == 0) { set_camera_mode(m->area->camera, m->area->camera->defMode, 1); }
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, 15.0f);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return FALSE;
}

s32 act_steep_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    mario_set_forward_vel(m, 0.98f * m->forwardVel);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (!check_fall_damage_or_get_stuck(m, ACT_HARD_BACKWARD_GROUND_KB)) {
                m->faceAngle[0] = 0;
                set_mario_action(m, m->forwardVel < 0.0f ? ACT_BEGIN_SLIDING : ACT_JUMP_LAND, 0);
            }
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, 0.0f);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    set_character_animation(m, CHAR_ANIM_SINGLE_JUMP);
    m->marioObj->header.gfx.angle[1] = m->marioObj->oMarioSteepJumpYaw;
    return FALSE;
}

s32 act_ground_pound(struct MarioState *m) {
    if (!m) { return 0; }
    u32 stepResult;
    f32 yOffset;

    play_sound_if_no_flag(m, SOUND_ACTION_THROW, MARIO_ACTION_SOUND_PLAYED);

    if (m->actionState == 0) {
        if (m->actionTimer < 10) {
            yOffset = 20 - 2 * m->actionTimer;
            if (m->pos[1] + yOffset + 160.0f < m->ceilHeight) {
                m->pos[1] += yOffset;
                m->peakHeight = m->pos[1];
                vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
            }
        }

        m->vel[1] = -50.0f;
        mario_set_forward_vel(m, 0.0f);

        set_character_animation(m, m->actionArg == 0 ? CHAR_ANIM_START_GROUND_POUND
                                                 : CHAR_ANIM_TRIPLE_JUMP_GROUND_POUND);
        if (m->actionTimer == 0) {
            play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
        }

        m->actionTimer++;
        if (m->actionTimer >= m->marioObj->header.gfx.animInfo.curAnim->loopEnd + 4) {
            play_character_sound(m, CHAR_SOUND_GROUND_POUND_WAH);
            m->actionState = 1;
        }
    } else {
        set_character_animation(m, CHAR_ANIM_GROUND_POUND);

        stepResult = perform_air_step(m, 0);
        if (stepResult == AIR_STEP_LANDED) {
            if (should_get_stuck_in_ground(m)) {
                queue_rumble_data_mario(m, 5, 80);
#ifdef VERSION_JP
                play_character_sound(m, CHAR_SOUND_OOOF);
#else
                play_character_sound(m, CHAR_SOUND_OOOF2);
#endif
                set_mario_particle_flags(m, PARTICLE_MIST_CIRCLE, FALSE);
                set_mario_action(m, ACT_BUTT_STUCK_IN_GROUND, 0);
            } else {
                play_mario_heavy_landing_sound(m, SOUND_ACTION_TERRAIN_HEAVY_LANDING);
                if (!check_fall_damage(m, ACT_HARD_BACKWARD_GROUND_KB)) {
                    set_mario_particle_flags(m, (PARTICLE_MIST_CIRCLE | PARTICLE_HORIZONTAL_STAR), FALSE);
                    set_mario_action(m, ACT_GROUND_POUND_LAND, 0);
                }
            }
            if (m->playerIndex == 0) { set_camera_shake_from_hit(SHAKE_GROUND_POUND); }
        } else if (stepResult == AIR_STEP_HIT_WALL) {
            if (gLevelValues.fixCollisionBugs && gLevelValues.fixCollisionBugsGroundPoundBonks) {
                // do nothing
            } else {
                mario_set_forward_vel(m, -16.0f);
                if (m->vel[1] > 0.0f) {
                    m->vel[1] = 0.0f;
                }

                set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
                set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
            }
        }
    }

    return FALSE;
}

s32 act_burning_jump(struct MarioState *m) {
    if (!m) { return 0; }
    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, m->actionArg == 0 ? 0 : -1);
    mario_set_forward_vel(m, m->forwardVel);

    if (perform_air_step(m, 0) == AIR_STEP_LANDED) {
        play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
        set_mario_action(m, ACT_BURNING_GROUND, 0);
    }

    set_character_animation(m, m->actionArg == 0 ? CHAR_ANIM_SINGLE_JUMP : CHAR_ANIM_FIRE_LAVA_BURN);
    set_mario_particle_flags(m, PARTICLE_FIRE, FALSE);
    play_sound(SOUND_MOVING_LAVA_BURN, m->marioObj->header.gfx.cameraToObject);

    m->marioObj->oMarioBurnTimer += 3;

    m->health -= 10;
    if (m->health < 0x100) {
        if (m != &gMarioStates[0]) {
            // never kill remote marios
            m->health = 0x100;
        } else {
            m->health = 0xFF;
        }
    }

    reset_rumble_timers(m);
    return FALSE;
}

s32 act_burning_fall(struct MarioState *m) {
    if (!m) { return 0; }
    mario_set_forward_vel(m, m->forwardVel);

    if (perform_air_step(m, 0) == AIR_STEP_LANDED) {
        play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
        set_mario_action(m, ACT_BURNING_GROUND, 0);
    }

    set_character_animation(m, CHAR_ANIM_GENERAL_FALL);
    set_mario_particle_flags(m, PARTICLE_FIRE, FALSE);
    m->marioObj->oMarioBurnTimer += 3;

    m->health -= 10;
    if (m->health < 0x100) {
        if (m != &gMarioStates[0]) {
            // never kill remote marios
            m->health = 0x100;
        } else {
            m->health = 0xFF;
        }
    }

    reset_rumble_timers(m);
    return FALSE;
}

s32 act_crazy_box_bounce(struct MarioState *m) {
    if (!m) { return 0; }
    f32 minSpeed = 32;

    if (m->actionTimer == 0) {
        switch (m->actionArg) {
            case 0:
                m->vel[1] = 45.0f;
                minSpeed = 32.0f;
                break;

            case 1:
                m->vel[1] = 60.0f;
                minSpeed = 36.0f;
                break;

            case 2:
                m->vel[1] = 100.0f;
                minSpeed = 48.0f;
                break;
        }

        play_sound(minSpeed < 40.0f ? SOUND_GENERAL_BOING1 : SOUND_GENERAL_BOING2,
                   m->marioObj->header.gfx.cameraToObject);

        if (m->forwardVel < minSpeed) {
            mario_set_forward_vel(m, minSpeed);
        }

        m->actionTimer = 1;
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);
    set_character_animation(m, CHAR_ANIM_DIVE);

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (m->actionArg < 2) {
                set_mario_action(m, ACT_CRAZY_BOX_BOUNCE, m->actionArg + 1);
            } else {
                if (m->heldObj != NULL && m->playerIndex == 0) {
                    m->heldObj->oInteractStatus = INT_STATUS_STOP_RIDING;
                    m->heldObj = NULL;
                }
                set_mario_action(m, ACT_STOMACH_SLIDE, 0);
            }
            queue_rumble_data_mario(m, 5, 80);
            set_mario_particle_flags(m, PARTICLE_MIST_CIRCLE, FALSE);
            break;

        case AIR_STEP_HIT_WALL:
            mario_bonk_reflection(m, FALSE);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    m->marioObj->header.gfx.angle[0] = atan2s(m->forwardVel, -m->vel[1]);
    return FALSE;
}

/* |description|
A shared step update used for airborne knockback states (both forward and backward). Updates velocity, calls `perform_air_step`,
and handles wall collisions or landing transitions to appropriate ground knockback actions. Also sets animation and speed
|descriptionEnd| */
u32 common_air_knockback_step(struct MarioState *m, u32 landAction, u32 hardFallAction, s32 animation, f32 speed) {
    if (!m) { return 0; }
    u32 stepResult;

    if (m->knockbackTimer == 0) {
        if (m->interactObj == NULL || !(m->interactObj->oInteractType & INTERACT_PLAYER)) {
            mario_set_forward_vel(m, speed);
        }
    } else if (m->knockbackTimer < 0) {
        // do nothing
    } else {
        m->knockbackTimer = PVP_ATTACK_KNOCKBACK_TIMER_DEFAULT;
    }

    stepResult = perform_air_step(m, 0);
    switch (stepResult) {
        case AIR_STEP_NONE:
            set_character_animation(m, animation);
            break;

        case AIR_STEP_LANDED:
            if (m->action == ACT_SOFT_BONK) {
                queue_rumble_data_mario(m, 5, 40);
            }
            if (!check_fall_damage_or_get_stuck(m, hardFallAction)) {
#ifndef VERSION_JP
                if (m->action == ACT_THROWN_FORWARD || m->action == ACT_THROWN_BACKWARD) {
                    set_mario_action(m, landAction, m->hurtCounter);
                } else {
                    set_mario_action(m, landAction, m->actionArg);
                }
#else
                set_mario_action(m, landAction, m->actionArg);
#endif
            }
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }

            set_character_animation(m, CHAR_ANIM_BACKWARD_AIR_KB);
            mario_bonk_reflection(m, FALSE);

            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }

            mario_set_forward_vel(m, -speed);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return stepResult;
}

/* |description|
Checks if Mario should wall kick after performing an air hit against a wall. If the input conditions (e.g., pressing A)
and the `wallKickTimer` allow, Mario transitions to `ACT_WALL_KICK_AIR`
|descriptionEnd| */
s32 check_wall_kick(struct MarioState *m) {
    if (!m) { return 0; }
    if ((m->input & INPUT_A_PRESSED) && m->wallKickTimer != 0 && m->prevAction == ACT_AIR_HIT_WALL) {
        m->faceAngle[1] += 0x8000;
        return set_mario_action(m, ACT_WALL_KICK_AIR, 0);
    }

    return FALSE;
}

s32 act_backward_air_kb(struct MarioState *m) {
    if (check_wall_kick(m)) {
        return TRUE;
    }

#ifndef VERSION_JP
    play_knockback_sound(m);
#else
    play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
#endif
    common_air_knockback_step(m, ACT_BACKWARD_GROUND_KB, ACT_HARD_BACKWARD_GROUND_KB, 0x0002, -16.0f);
    return FALSE;
}

s32 act_forward_air_kb(struct MarioState *m) {
    if (check_wall_kick(m)) {
        return TRUE;
    }

#ifndef VERSION_JP
    play_knockback_sound(m);
#else
    play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
#endif
    common_air_knockback_step(m, ACT_FORWARD_GROUND_KB, ACT_HARD_FORWARD_GROUND_KB, 0x002D, 16.0f);
    return FALSE;
}

s32 act_hard_backward_air_kb(struct MarioState *m) {
#ifndef VERSION_JP
    play_knockback_sound(m);
#else
    play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
#endif
    common_air_knockback_step(m, ACT_HARD_BACKWARD_GROUND_KB, ACT_HARD_BACKWARD_GROUND_KB, 0x0002,
                              -16.0f);
    return FALSE;
}

s32 act_hard_forward_air_kb(struct MarioState *m) {
#ifndef VERSION_JP
    play_knockback_sound(m);
#else
    play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
#endif
    common_air_knockback_step(m, ACT_HARD_FORWARD_GROUND_KB, ACT_HARD_FORWARD_GROUND_KB, 0x002D, 16.0f);
    return FALSE;
}

s32 act_thrown_backward(struct MarioState *m) {
    if (!m) { return 0; }
    u32 landAction;
    if (m->actionArg != 0) {
        landAction = ACT_HARD_BACKWARD_GROUND_KB;
    } else {
        landAction = ACT_BACKWARD_GROUND_KB;
    }

    play_character_sound_if_no_flag(m, CHAR_SOUND_WAAAOOOW, MARIO_MARIO_SOUND_PLAYED);

    common_air_knockback_step(m, landAction, ACT_HARD_BACKWARD_GROUND_KB, 0x0002, m->forwardVel);

    m->forwardVel *= 0.98f;
    return FALSE;
}

s32 act_thrown_forward(struct MarioState *m) {
    if (!m) { return 0; }
    s16 pitch;

    u32 landAction;
    if (m->actionArg != 0) {
        landAction = ACT_HARD_FORWARD_GROUND_KB;
    } else {
        landAction = ACT_FORWARD_GROUND_KB;
    }

    play_character_sound_if_no_flag(m, CHAR_SOUND_WAAAOOOW, MARIO_MARIO_SOUND_PLAYED);

    if (common_air_knockback_step(m, landAction, ACT_HARD_FORWARD_GROUND_KB, 0x002D, m->forwardVel)
        == AIR_STEP_NONE) {
        pitch = atan2s(m->forwardVel, -m->vel[1]);
        if (pitch > 0x1800) {
            pitch = 0x1800;
        }

        m->marioObj->header.gfx.angle[0] = pitch + 0x1800;
    }

    m->forwardVel *= 0.98f;
    return FALSE;
}

s32 act_soft_bonk(struct MarioState *m) {
    if (check_wall_kick(m)) {
        return TRUE;
    }

#ifndef VERSION_JP
    play_knockback_sound(m);
#else
    play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
#endif

    common_air_knockback_step(m, ACT_FREEFALL_LAND, ACT_HARD_BACKWARD_GROUND_KB, 0x0056, m->forwardVel);
    return FALSE;
}

s32 act_getting_blown(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->actionState == 0) {
        if (m->forwardVel > -60.0f) {
            m->forwardVel -= 6.0f;
        } else {
            m->actionState = 1;
        }
    } else {
        if (m->forwardVel < -16.0f) {
            m->forwardVel += 0.8f;
        }

        if (m->vel[1] < 0.0f && m->unkC4 < 4.0f) {
            m->unkC4 += 0.05f;
        }
    }

    if (++(m->actionTimer) == 20) {
        mario_blow_off_cap(m, 50.0f);
    }

    mario_set_forward_vel(m, m->forwardVel);
#ifdef VERSION_JP
    play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
#endif
    set_character_animation(m, CHAR_ANIM_BACKWARD_AIR_KB);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_HARD_BACKWARD_AIR_KB, 0);
            break;

        case AIR_STEP_HIT_WALL:
            set_character_animation(m, CHAR_ANIM_AIR_FORWARD_KB);
            mario_bonk_reflection(m, FALSE);

            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }

            mario_set_forward_vel(m, -m->forwardVel);
            break;
    }

    return FALSE;
}

s32 act_air_hit_wall(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->heldObj != NULL) {
        mario_drop_held_object(m);
    }

    if (++(m->actionTimer) <= 2) {
        if (m->input & INPUT_A_PRESSED) {
            m->vel[1] = 52.0f;
            m->faceAngle[1] += 0x8000;
            return set_mario_action(m, ACT_WALL_KICK_AIR, 0);
        }
    } else if (m->forwardVel >= 38.0f) {
        m->wallKickTimer = 5;
        if (m->vel[1] > 0.0f) {
            m->vel[1] = 0.0f;
        }

        set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
        return set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
    } else {
        m->wallKickTimer = 5;
        if (m->vel[1] > 0.0f) {
            m->vel[1] = 0.0f;
        }

        if (m->forwardVel > 8.0f) {
            mario_set_forward_vel(m, -8.0f);
        }
        return set_mario_action(m, ACT_SOFT_BONK, 0);
    }

#ifdef AVOID_UB
    return
#endif
    set_character_animation(m, CHAR_ANIM_START_WALLKICK);

    //! Missing return statement. The returned value is the result of the call
    // to set_character_animation. In practice, this value is nonzero.
    // This results in this action "cancelling" into itself. It is supposed to
    // execute on two frames, but instead it executes twice on the same frame.
    // This results in firsties only being possible for a single frame, instead
    // of two.
}

s32 act_forward_rollout(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->actionState == 0) {
        m->vel[1] = 30.0f;
        m->actionState = 1;
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_NONE:
            if (m->actionState == 1) {
                if (set_character_animation(m, CHAR_ANIM_FORWARD_SPINNING) == 4) {
                    play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
                }
            } else {
                set_character_animation(m, CHAR_ANIM_GENERAL_FALL);
            }
            break;

        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_FREEFALL_LAND_STOP, 0);
            play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }
            mario_set_forward_vel(m, 0.0f);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    if (m->actionState == 1 && is_anim_past_end(m)) {
        m->actionState = 2;
    }
    return FALSE;
}

s32 act_backward_rollout(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->actionState == 0) {
        m->vel[1] = 30.0f;
        m->actionState = 1;
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, 0);

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_NONE:
            if (m->actionState == 1) {
                if (set_character_animation(m, CHAR_ANIM_BACKWARD_SPINNING) == 4) {
                    play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
                }
            } else {
                set_character_animation(m, CHAR_ANIM_GENERAL_FALL);
            }
            break;

        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_FREEFALL_LAND_STOP, 0);
            play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }
            mario_set_forward_vel(m, 0.0f);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    if (m->actionState == 1 && m->marioObj->header.gfx.animInfo.animFrame == 2) {
        m->actionState = 2;
    }
    return FALSE;
}

s32 act_butt_slide_air(struct MarioState *m) {
    if (!m) { return 0; }
    if (++(m->actionTimer) > 30 && m->pos[1] - m->floorHeight > 500.0f) {
        return set_mario_action(m, ACT_FREEFALL, 1);
    }

    update_air_with_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (m->actionState == 0 && m->vel[1] < 0.0f && m->floor->normal.y >= 0.9848077f) {
                m->vel[1] = -m->vel[1] / 2.0f;
                m->actionState = 1;
            } else {
                set_mario_action(m, ACT_BUTT_SLIDE, 0);
            }
            play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }

            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }
            set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
            set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    set_character_animation(m, CHAR_ANIM_SLIDE);
    return FALSE;
}

s32 act_hold_butt_slide_air(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->marioObj->oInteractStatus & INT_STATUS_MARIO_DROP_OBJECT) {
        return drop_and_set_mario_action(m, ACT_HOLD_FREEFALL, 1);
    }

    if (++m->actionTimer > 30 && m->pos[1] - m->floorHeight > 500.0f) {
        return set_mario_action(m, ACT_HOLD_FREEFALL, 1);
    }

    update_air_with_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (m->actionState == 0 && m->vel[1] < 0.0f && m->floor->normal.y >= 0.9848077f) {
                m->vel[1] = -m->vel[1] / 2.0f;
                m->actionState = 1;
            } else {
                set_mario_action(m, ACT_HOLD_BUTT_SLIDE, 0);
            }
            play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }

            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }

            mario_drop_held_object(m);
            set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
            set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    set_character_animation(m, CHAR_ANIM_SLIDING_ON_BOTTOM_WITH_LIGHT_OBJ);
    return FALSE;
}

s32 act_lava_boost(struct MarioState *m) {
    if (!m) { return 0; }
    if (!(m->flags & MARIO_MARIO_SOUND_PLAYED)) {
        play_character_sound_if_no_flag(m, CHAR_SOUND_ON_FIRE, MARIO_MARIO_SOUND_PLAYED);
        queue_rumble_data_mario(m, 5, 80);
    }
    play_character_sound_if_no_flag(m, CHAR_SOUND_ON_FIRE, MARIO_MARIO_SOUND_PLAYED);

    if (!(m->input & INPUT_NONZERO_ANALOG)) {
        m->forwardVel = approach_f32(m->forwardVel, 0.0f, 0.35f, 0.35f);
    }

    update_lava_boost_or_twirling(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (m->floor->type == SURFACE_BURNING) {
                m->actionState = 0;
                if (!(m->flags & MARIO_METAL_CAP)) {
                    m->hurtCounter += (m->flags & MARIO_CAP_ON_HEAD) ? 12 : 18;
                }
                m->vel[1] = 84.0f;
                play_character_sound(m, CHAR_SOUND_ON_FIRE);
                queue_rumble_data_mario(m, 5, 80);
            } else {
                play_mario_heavy_landing_sound(m, SOUND_ACTION_TERRAIN_BODY_HIT_GROUND);
                if (m->actionState < 2 && m->vel[1] < 0.0f) {
                    m->vel[1] = -m->vel[1] * 0.4f;
                    mario_set_forward_vel(m, m->forwardVel * 0.5f);
                    m->actionState += 1;
                } else {
                    set_mario_action(m, ACT_LAVA_BOOST_LAND, 0);
                }
            }
            break;

        case AIR_STEP_HIT_WALL:
            mario_bonk_reflection(m, FALSE);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    set_character_animation(m, CHAR_ANIM_FIRE_LAVA_BURN);
    if ((m->area->terrainType & TERRAIN_MASK) != TERRAIN_SNOW && !(m->flags & MARIO_METAL_CAP)
        && m->vel[1] > 0.0f) {
        set_mario_particle_flags(m, PARTICLE_FIRE, FALSE);
        if (m->actionState == 0) {
            play_sound(SOUND_MOVING_LAVA_BURN, m->marioObj->header.gfx.cameraToObject);
        }
    }

    if (m->health < 0x100) {
        if (m != &gMarioStates[0]) {
            // never kill remote marios
            m->health = 0x100;
        } else {
            bool allowDeath = true;
            smlua_call_event_hooks_mario_param_ret_bool(HOOK_ON_DEATH, m, &allowDeath);
            if (!allowDeath) {
                reset_rumble_timers(m);
                return FALSE;
            }

            if (mario_can_bubble(m)) {
                m->health = 0xFF;
                mario_set_bubbled(m);
            } else {
                level_trigger_warp(m, WARP_OP_DEATH);
            }
        }
    }

    m->marioBodyState->eyeState = MARIO_EYES_DEAD;

    reset_rumble_timers(m);
    return FALSE;
}

s32 act_slide_kick(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->actionTimer == 0) {
        play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_HOOHOO);
        set_character_animation(m, CHAR_ANIM_SLIDE_KICK);
    }

    if (++(m->actionTimer) > 30 && m->pos[1] - m->floorHeight > 500.0f) {
        return set_mario_action(m, ACT_FREEFALL, 2);
    }

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_NONE:
            if (m->actionState == 0) {
                m->marioObj->header.gfx.angle[0] = atan2s(m->forwardVel, -m->vel[1]);
                if (m->marioObj->header.gfx.angle[0] > 0x1800) {
                    m->marioObj->header.gfx.angle[0] = 0x1800;
                }
            }
            break;

        case AIR_STEP_LANDED:
            if (m->actionState == 0 && m->vel[1] < 0.0f) {
                m->vel[1] = -m->vel[1] / 2.0f;
                m->actionState = 1;
                m->actionTimer = 0;
            } else {
                set_mario_action(m, ACT_SLIDE_KICK_SLIDE, 0);
            }
            play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }

            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }

            set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);

            set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return FALSE;
}

s32 act_jump_kick(struct MarioState *m) {
    if (!m) { return 0; }
    s32 animFrame;

    if (m->actionState == 0) {
        play_character_sound_if_no_flag(m, CHAR_SOUND_PUNCH_HOO, MARIO_ACTION_SOUND_PLAYED);
        m->marioObj->header.gfx.animInfo.animID = -1;
        set_character_animation(m, CHAR_ANIM_AIR_KICK);
        m->actionState = 1;
    }

    animFrame = m->marioObj->header.gfx.animInfo.animFrame;
    if (animFrame == 0) {
        m->marioBodyState->punchState = (2 << 6) | 6;
    }
    if (animFrame >= 0 && animFrame < 8) {
        m->flags |= MARIO_KICKING;
    }

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (!check_fall_damage_or_get_stuck(m, ACT_HARD_BACKWARD_GROUND_KB)) {
                set_mario_action(m, ACT_FREEFALL_LAND, 0);
            }
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall == NULL && gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) { break; }
            mario_set_forward_vel(m, 0.0f);
            break;
    }

    return FALSE;
}

s32 act_shot_from_cannon(struct MarioState *m) {
    if (!m) { return 0; }
    // only allow for local player
    u8 allowCameraChange = (m->playerIndex == 0);

    if (allowCameraChange && m->area->camera->mode != CAMERA_MODE_BEHIND_MARIO) {
        m->statusForCamera->cameraEvent = CAM_EVENT_SHOT_FROM_CANNON;
    }

    mario_set_forward_vel(m, m->forwardVel);

    play_character_sound_if_no_flag(m, CHAR_SOUND_YAHOO, MARIO_MARIO_SOUND_PLAYED);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_NONE:
            set_character_animation(m, CHAR_ANIM_AIRBORNE_ON_STOMACH);
            m->faceAngle[0] = atan2s(m->forwardVel, m->vel[1]);
            m->marioObj->header.gfx.angle[0] = -m->faceAngle[0];
            break;

        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_DIVE_SLIDE, 0);
            m->faceAngle[0] = 0;
            if (allowCameraChange) {
                if (!gNewCamera.isActive) {
                    set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
                } else {
                    m->area->camera->mode = CAMERA_MODE_NEWCAM;
                    gLakituState.mode = CAMERA_MODE_NEWCAM;
                }
            }
            queue_rumble_data_mario(m, 5, 80);
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, -16.0f);

            m->faceAngle[0] = 0;
            if (m->vel[1] > 0.0f) {
                m->vel[1] = 0.0f;
            }

            set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
            set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);
            if (allowCameraChange) {
                if (!gNewCamera.isActive) {
                    set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
                } else {
                    m->area->camera->mode = CAMERA_MODE_NEWCAM;
                    gLakituState.mode = CAMERA_MODE_NEWCAM;
                }
            }
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    if ((m->flags & MARIO_WING_CAP) && m->vel[1] < 0.0f) {
        set_mario_action(m, ACT_FLYING, 0);
    }

    if ((m->forwardVel -= 0.05) < 10.0f) {
        mario_set_forward_vel(m, 10.0f);
    }

    if (m->vel[1] > 0.0f) {
        set_mario_particle_flags(m, PARTICLE_DUST, FALSE);
    }

    reset_rumble_timers(m);
    return FALSE;
}

s32 act_flying(struct MarioState *m) {
    if (!m) { return 0; }
    s16 startPitch = m->faceAngle[0];

    if (m->input & INPUT_Z_PRESSED) {
        if (m->area->camera->mode == CAMERA_MODE_BEHIND_MARIO) {
            if (m->playerIndex == 0) {
                if (!gNewCamera.isActive) {
                    set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
                } else {
                    m->area->camera->mode = CAMERA_MODE_NEWCAM;
                    gLakituState.mode = CAMERA_MODE_NEWCAM;
                }
            }
        }
        return set_mario_action(m, ACT_GROUND_POUND, 1);
    }

    if (!(m->flags & MARIO_WING_CAP)) {
        if (m->area->camera->mode == CAMERA_MODE_BEHIND_MARIO) {
            if (m->playerIndex == 0) {
                if (!gNewCamera.isActive) {
                    set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
                } else {
                    m->area->camera->mode = CAMERA_MODE_NEWCAM;
                    gLakituState.mode = CAMERA_MODE_NEWCAM;
                }
            }
        }
        return set_mario_action(m, ACT_FREEFALL, 0);
    }

    if (m->area->camera->mode != CAMERA_MODE_BEHIND_MARIO) {
        if (m->playerIndex == 0) {
            if (!gNewCamera.isActive) {
                set_camera_mode(m->area->camera, CAMERA_MODE_BEHIND_MARIO, 1);
                // note: EX sets it to the following line instead, but I have
                //       no idea why... possibly copy/paste error?
                // set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
            } else {
                m->area->camera->mode = CAMERA_MODE_NEWCAM;
                gLakituState.mode = CAMERA_MODE_NEWCAM;
            }
        }
    }

    if (m->actionState == 0) {
        if (m->actionArg == 0) {
            set_character_animation(m, CHAR_ANIM_FLY_FROM_CANNON);
        } else {
            set_character_animation(m, CHAR_ANIM_FORWARD_SPINNING_FLIP);
            if (m->marioObj->header.gfx.animInfo.animFrame == 1) {
                play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
            }
        }

        if (is_anim_at_end(m)) {
            if (m->actionArg == 2) {
                if (m == &gMarioStates[0]) {
                    load_level_init_text(0);
                }
                m->actionArg = 1;
            }

            set_character_animation(m, CHAR_ANIM_WING_CAP_FLY);
            m->actionState = 1;
        }
    }

    update_flying(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_NONE:
            m->marioObj->header.gfx.angle[0] = -m->faceAngle[0];
            m->marioObj->header.gfx.angle[2] = m->faceAngle[2];
            m->actionTimer = 0;
            break;

        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_DIVE_SLIDE, 0);

            set_character_animation(m, CHAR_ANIM_DIVE);
            set_anim_to_frame(m, 7);

            m->faceAngle[0] = 0;

            if (m->playerIndex == 0) {
                if (!gNewCamera.isActive) {
                    set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
                } else {
                    m->area->camera->mode = CAMERA_MODE_NEWCAM;
                    gLakituState.mode = CAMERA_MODE_NEWCAM;
                }
            }
            queue_rumble_data_mario(m, 5, 60);
            break;

        case AIR_STEP_HIT_WALL:
            if (m->wall != NULL) {
                mario_set_forward_vel(m, -16.0f);
                m->faceAngle[0] = 0;

                if (m->vel[1] > 0.0f) {
                    m->vel[1] = 0.0f;
                }

                play_sound((m->flags & MARIO_METAL_CAP) ? SOUND_ACTION_METAL_BONK
                                                        : SOUND_ACTION_BONK,
                           m->marioObj->header.gfx.cameraToObject);

                set_mario_particle_flags(m, PARTICLE_VERTICAL_STAR, FALSE);
                set_mario_action(m, ACT_BACKWARD_AIR_KB, 0);

                if (m->playerIndex == 0) {
                    if (!gNewCamera.isActive) {
                        set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
                    } else {
                        m->area->camera->mode = CAMERA_MODE_NEWCAM;
                        gLakituState.mode = CAMERA_MODE_NEWCAM;
                    }
                }
            } else {
                if (m->actionTimer++ == 0) {
                    play_sound(SOUND_ACTION_HIT, m->marioObj->header.gfx.cameraToObject);
                }

                if (m->actionTimer == 30) {
                    m->actionTimer = 0;
                }

                m->faceAngle[0] -= 0x200;
                if (m->faceAngle[0] < -0x2AAA) {
                    m->faceAngle[0] = -0x2AAA;
                }

                m->marioObj->header.gfx.angle[0] = -m->faceAngle[0];
                m->marioObj->header.gfx.angle[2] = m->faceAngle[2];
            }
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    if (m->faceAngle[0] > 0x800 && m->forwardVel >= 48.0f) {
        set_mario_particle_flags(m, PARTICLE_DUST, FALSE);
    }

    if (startPitch <= 0 && m->faceAngle[0] > 0 && m->forwardVel >= 48.0f) {
        play_sound(SOUND_ACTION_FLYING_FAST, m->marioObj->header.gfx.cameraToObject);
#ifndef VERSION_JP
        play_character_sound_offset(m, CHAR_SOUND_YAHOO_WAHA_YIPPEE, ((gAudioRandom % 5) << 16));
#endif
        queue_rumble_data_mario(m, 50, 40);
    }

    play_sound(SOUND_MOVING_FLYING, m->marioObj->header.gfx.cameraToObject);
    adjust_sound_for_speed(m);
    return FALSE;
}

s32 act_riding_hoot(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->usedObj == NULL || m->usedObj->behavior != smlua_override_behavior(bhvHoot)) {
        m->usedObj = cur_obj_nearest_object_with_behavior(bhvHoot);
        if (m->usedObj == NULL) { return FALSE; }
        m->usedObj->heldByPlayerIndex = m->playerIndex;
    }

    if (m->usedObj == NULL) { return FALSE; }

    if (!(m->input & INPUT_A_DOWN) || (m->marioObj->oInteractStatus & INT_STATUS_MARIO_UNK7)) {
        m->usedObj->oInteractStatus = 0;
        m->usedObj->oHootMarioReleaseTime = gGlobalTimer;

        play_character_sound_if_no_flag(m, CHAR_SOUND_UH, MARIO_MARIO_SOUND_PLAYED);
        queue_rumble_data_mario(m, 4, 40);
        return set_mario_action(m, ACT_FREEFALL, 0);
    }

    m->pos[0] = m->usedObj->oPosX;
    m->pos[1] = m->usedObj->oPosY - 92.5f;
    m->pos[2] = m->usedObj->oPosZ;

    m->faceAngle[1] = 0x4000 - m->usedObj->oMoveAngleYaw;

    if (m->actionState == 0) {
        set_character_animation(m, CHAR_ANIM_HANG_ON_CEILING);
        if (is_anim_at_end(m)) {
            set_character_animation(m, CHAR_ANIM_HANG_ON_OWL);
            m->actionState = 1;
        }
    }

    vec3f_set(m->vel, 0.0f, 0.0f, 0.0f);
    vec3f_set(m->marioObj->header.gfx.pos, m->pos[0], m->pos[1], m->pos[2]);
    vec3s_set(m->marioObj->header.gfx.angle, 0, 0x4000 - m->faceAngle[1], 0);
    return FALSE;
}

s32 act_flying_triple_jump(struct MarioState *m) {
    if (!m) { return 0; }
#ifndef VERSION_JP
    if (m->input & (INPUT_B_PRESSED | INPUT_Z_PRESSED)) {
        if (m->playerIndex == 0 && m->area->camera->mode == CAMERA_MODE_BEHIND_MARIO) {
            if (!gNewCamera.isActive) {
                set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
            } else {
                m->area->camera->mode = CAMERA_MODE_NEWCAM;
                gLakituState.mode = CAMERA_MODE_NEWCAM;
            }
        }
        if (m->input & INPUT_B_PRESSED) {
            return set_mario_action(m, ACT_DIVE, 0);
        } else {
            return set_mario_action(m, ACT_GROUND_POUND, 0);
        }
    }
#else
    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }
#endif

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_YAHOO);
    if (m->actionState == 0) {
        set_character_animation(m, CHAR_ANIM_TRIPLE_JUMP_FLY);

        if (m->marioObj->header.gfx.animInfo.animFrame == 7) {
            play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
        }

        if (is_anim_past_end(m)) {
            set_character_animation(m, CHAR_ANIM_FORWARD_SPINNING);
            queue_rumble_data_mario(m, 8, 80);
            m->actionState = 1;
        }
    }

    if (m->actionState == 1 && m->marioObj->header.gfx.animInfo.animFrame == 1) {
        play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
    }

    if (m->vel[1] < 4.0f) {
        if (m->playerIndex == 0 && m->area->camera->mode != CAMERA_MODE_BEHIND_MARIO) {
            if (!gNewCamera.isActive) {
                set_camera_mode(m->area->camera, m->area->camera->defMode, 1);
            } else {
                m->area->camera->mode = CAMERA_MODE_NEWCAM;
                gLakituState.mode = CAMERA_MODE_NEWCAM;
            }
        }

        if (m->forwardVel < 32.0f) {
            mario_set_forward_vel(m, 32.0f);
        }

        set_mario_action(m, ACT_FLYING, 1);
    }

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (!check_fall_damage_or_get_stuck(m, ACT_HARD_BACKWARD_GROUND_KB)) {
                set_mario_action(m, ACT_DOUBLE_JUMP_LAND, 0);
            }
            break;

        case AIR_STEP_HIT_WALL:
            mario_bonk_reflection(m, FALSE);
            break;

        case AIR_STEP_HIT_LAVA_WALL:
            lava_boost_on_wall(m);
            break;
    }

    return FALSE;
}

s32 act_top_of_pole_jump(struct MarioState *m) {
    play_mario_jump_sound(m);
    common_air_action_step(m, ACT_FREEFALL_LAND, CHAR_ANIM_HANDSTAND_JUMP, AIR_STEP_CHECK_LEDGE_GRAB);
    return FALSE;
}

s32 act_vertical_wind(struct MarioState *m) {
    if (!m) { return 0; }
    s16 intendedDYaw = m->intendedYaw - m->faceAngle[1];
    f32 intendedMag = m->intendedMag / 32.0f;

    play_character_sound_if_no_flag(m, CHAR_SOUND_HERE_WE_GO, MARIO_MARIO_SOUND_PLAYED);
    if (m->actionState == 0) {
        set_character_animation(m, CHAR_ANIM_FORWARD_SPINNING_FLIP);
        if (m->marioObj->header.gfx.animInfo.animFrame == 1) {
            play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
            queue_rumble_data_mario(m, 8, 80);
        }

        if (is_anim_past_end(m)) {
            m->actionState = 1;
        }
    } else {
        set_character_animation(m, CHAR_ANIM_AIRBORNE_ON_STOMACH);
    }

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            set_mario_action(m, ACT_DIVE_SLIDE, 0);
            break;

        case AIR_STEP_HIT_WALL:
            mario_set_forward_vel(m, -16.0f);
            break;
    }

    m->marioObj->header.gfx.angle[0] = (s16)(6144.0f * intendedMag * coss(intendedDYaw));
    m->marioObj->header.gfx.angle[2] = (s16)(-4096.0f * intendedMag * sins(intendedDYaw));
    return FALSE;
}

s32 act_special_triple_jump(struct MarioState *m) {
    if (!m) { return 0; }
    if (m->input & INPUT_B_PRESSED) {
        return set_mario_action(m, ACT_DIVE, 0);
    }

    if (m->input & INPUT_Z_PRESSED) {
        return set_mario_action(m, ACT_GROUND_POUND, 0);
    }

    play_mario_sound(m, SOUND_ACTION_TERRAIN_JUMP, CHAR_SOUND_YAHOO);

    update_air_without_turn(m);

    switch (perform_air_step(m, 0)) {
        case AIR_STEP_LANDED:
            if (m->actionState++ == 0) {
                m->vel[1] = 42.0f;
            } else {
                set_mario_action(m, ACT_FREEFALL_LAND_STOP, 0);
            }
            play_mario_landing_sound(m, SOUND_ACTION_TERRAIN_LANDING);
            break;

        case AIR_STEP_HIT_WALL:
            mario_bonk_reflection(m, TRUE);
            break;
    }

    if (m->actionState == 0 || m->vel[1] > 0.0f) {
        if (set_character_animation(m, CHAR_ANIM_FORWARD_SPINNING) == 0) {
            play_sound(SOUND_ACTION_SPIN, m->marioObj->header.gfx.cameraToObject);
        }
    } else {
        set_character_animation(m, CHAR_ANIM_GENERAL_FALL);
    }

    set_mario_particle_flags(m, PARTICLE_SPARKLES, FALSE);
    return FALSE;
}

/* |description|
Checks for and handles common conditions that would cancel Mario's current air action. This includes transitioning
to a water plunge if below the water level, becoming squished if appropriate, or switching to vertical wind action
if on certain wind surfaces. Also resets `m.quicksandDepth`
|descriptionEnd| */
s32 check_common_airborne_cancels(struct MarioState *m) {
    if (!m) { return 0; }
    bool allow = true;
    if (m->pos[1] < m->waterLevel - 100) {
        smlua_call_event_hooks_mario_param_and_bool_ret_bool(HOOK_ALLOW_FORCE_WATER_ACTION, m, false, &allow);
        if (allow) {
            return set_water_plunge_action(m);
        }
    }
    allow = true;

    if (m->input & INPUT_SQUISHED) {
        return drop_and_set_mario_action(m, ACT_SQUISHED, 0);
    }

    
    if (m->floor && m->floor->type == SURFACE_VERTICAL_WIND && (m->action & ACT_FLAG_ALLOW_VERTICAL_WIND_ACTION)) {
        smlua_call_event_hooks_mario_param_and_int_ret_bool(HOOK_ALLOW_HAZARD_SURFACE, m, HAZARD_TYPE_VERTICAL_WIND, &allow);
        if (allow) {
            return drop_and_set_mario_action(m, ACT_VERTICAL_WIND, 0);
        }
    }

    m->quicksandDepth = 0.0f;
    return FALSE;
}

/* |description|
Executes Mario's current airborne action by first checking common airborne cancels, then playing a far-fall sound if needed.
Dispatches to the appropriate action function, such as jump, double jump, freefall, etc
|descriptionEnd| */
s32 mario_execute_airborne_action(struct MarioState *m) {
    if (!m) { return FALSE; }
    u32 cancel;

    if (check_common_airborne_cancels(m)) {
        return TRUE;
    }

    play_far_fall_sound(m);

    if (!smlua_call_action_hook(ACTION_HOOK_EVERY_FRAME, m, (s32*)&cancel)) {
        /* clang-format off */
        switch (m->action) {
            case ACT_JUMP:                 cancel = act_jump(m);                 break;
            case ACT_DOUBLE_JUMP:          cancel = act_double_jump(m);          break;
            case ACT_FREEFALL:             cancel = act_freefall(m);             break;
            case ACT_HOLD_JUMP:            cancel = act_hold_jump(m);            break;
            case ACT_HOLD_FREEFALL:        cancel = act_hold_freefall(m);        break;
            case ACT_SIDE_FLIP:            cancel = act_side_flip(m);            break;
            case ACT_WALL_KICK_AIR:        cancel = act_wall_kick_air(m);        break;
            case ACT_TWIRLING:             cancel = act_twirling(m);             break;
            case ACT_WATER_JUMP:           cancel = act_water_jump(m);           break;
            case ACT_HOLD_WATER_JUMP:      cancel = act_hold_water_jump(m);      break;
            case ACT_STEEP_JUMP:           cancel = act_steep_jump(m);           break;
            case ACT_BURNING_JUMP:         cancel = act_burning_jump(m);         break;
            case ACT_BURNING_FALL:         cancel = act_burning_fall(m);         break;
            case ACT_TRIPLE_JUMP:          cancel = act_triple_jump(m);          break;
            case ACT_BACKFLIP:             cancel = act_backflip(m);             break;
            case ACT_LONG_JUMP:            cancel = act_long_jump(m);            break;
            case ACT_RIDING_SHELL_JUMP:
            case ACT_RIDING_SHELL_FALL:    cancel = act_riding_shell_air(m);     break;
            case ACT_DIVE:                 cancel = act_dive(m);                 break;
            case ACT_AIR_THROW:            cancel = act_air_throw(m);            break;
            case ACT_BACKWARD_AIR_KB:      cancel = act_backward_air_kb(m);      break;
            case ACT_FORWARD_AIR_KB:       cancel = act_forward_air_kb(m);       break;
            case ACT_HARD_FORWARD_AIR_KB:  cancel = act_hard_forward_air_kb(m);  break;
            case ACT_HARD_BACKWARD_AIR_KB: cancel = act_hard_backward_air_kb(m); break;
            case ACT_SOFT_BONK:            cancel = act_soft_bonk(m);            break;
            case ACT_AIR_HIT_WALL:         cancel = act_air_hit_wall(m);         break;
            case ACT_FORWARD_ROLLOUT:      cancel = act_forward_rollout(m);      break;
            case ACT_SHOT_FROM_CANNON:     cancel = act_shot_from_cannon(m);     break;
            case ACT_BUTT_SLIDE_AIR:       cancel = act_butt_slide_air(m);       break;
            case ACT_HOLD_BUTT_SLIDE_AIR:  cancel = act_hold_butt_slide_air(m);  break;
            case ACT_LAVA_BOOST:           cancel = act_lava_boost(m);           break;
            case ACT_GETTING_BLOWN:        cancel = act_getting_blown(m);        break;
            case ACT_BACKWARD_ROLLOUT:     cancel = act_backward_rollout(m);     break;
            case ACT_CRAZY_BOX_BOUNCE:     cancel = act_crazy_box_bounce(m);     break;
            case ACT_SPECIAL_TRIPLE_JUMP:  cancel = act_special_triple_jump(m);  break;
            case ACT_GROUND_POUND:         cancel = act_ground_pound(m);         break;
            case ACT_THROWN_FORWARD:       cancel = act_thrown_forward(m);       break;
            case ACT_THROWN_BACKWARD:      cancel = act_thrown_backward(m);      break;
            case ACT_FLYING_TRIPLE_JUMP:   cancel = act_flying_triple_jump(m);   break;
            case ACT_SLIDE_KICK:           cancel = act_slide_kick(m);           break;
            case ACT_JUMP_KICK:            cancel = act_jump_kick(m);            break;
            case ACT_FLYING:               cancel = act_flying(m);               break;
            case ACT_RIDING_HOOT:          cancel = act_riding_hoot(m);          break;
            case ACT_TOP_OF_POLE_JUMP:     cancel = act_top_of_pole_jump(m);     break;
            case ACT_VERTICAL_WIND:        cancel = act_vertical_wind(m);        break;
            default:
                LOG_ERROR("Attempted to execute unimplemented action '%04X'", m->action);
                set_mario_action(m, ACT_FREEFALL, 0);
                return false;
        }
        /* clang-format on */
    }

    return cancel;
}
