#include <ultra64.h>

#include "sm64.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "mario.h"
#include "audio/external.h"
#include "game_init.h"
#include "interaction.h"
#include "mario_step.h"
#include "pc/lua/smlua.h"
#include "game/hardcoded.h"

static s16 sMovingSandSpeeds[] = { 12, 8, 4, 0 };

struct Surface gWaterSurfacePseudoFloor = {
    .type = SURFACE_VERY_SLIPPERY,
    .flags = 0,
    .room = 0,
    .force = 0,
    .lowerY = 0,
    .upperY = 0,
    .vertex1 = { 0, 0, 0 },
    .vertex2 = { 0, 0, 0 },
    .vertex3 = { 0, 0, 0 },
    .prevVertex1 = { 0, 0, 0 },
    .prevVertex2 = { 0, 0, 0 },
    .prevVertex3 = { 0, 0, 0 },
    .normal = { 0.0f, 1.0f, 0.0f },
    .originOffset = 0.0f,
    .modifiedTimestamp = 0,
    .object = NULL
};

/**
 * Always returns zero. This may have been intended
 * to be used for the beta trampoline. Its return value
 * is used by set_mario_y_vel_based_on_fspeed as a constant
 * addition to Mario's Y velocity. Given the closeness of
 * this function to stub_mario_step_2, it is probable that this
 * was intended to check whether a trampoline had made itself
 * known through stub_mario_step_2 and whether Mario was on it,
 * and if so return a higher value than 0.
 */
f32 get_additive_y_vel_for_jumps(void) {
    return 0.0f;
}

/**
 * Does nothing, but takes in a MarioState. This is only ever
 * called by update_mario_inputs, which is called as part of Mario's
 * update routine. Due to its proximity to stub_mario_step_2, an
 * incomplete trampoline function, and get_additive_y_vel_for_jumps,
 * a potentially trampoline-related function, it is plausible that
 * this could be used for checking if Mario was on the trampoline.
 * It could, for example, make him bounce.
 */
void stub_mario_step_1(UNUSED struct MarioState *x) {
}

/**
 * Does nothing. This is only called by the beta trampoline.
 * Due to its proximity to get_additive_y_vel_for_jumps, another
 * currently-pointless function, it is probable that this was used
 * by the trampoline to make itself known to get_additive_y_vel_for_jumps,
 * or to set a variable with its intended additive Y vel.
 */
void stub_mario_step_2(void) {
}

void transfer_bully_speed(struct BullyCollisionData *obj1, struct BullyCollisionData *obj2) {
    f32 rx = obj2->posX - obj1->posX;
    f32 rz = obj2->posZ - obj1->posZ;

    //! Bully NaN crash
    f32 projectedV1 = (rx * obj1->velX + rz * obj1->velZ) / (rx * rx + rz * rz);
    f32 projectedV2 = (-rx * obj2->velX - rz * obj2->velZ) / (rx * rx + rz * rz);

    // Kill speed along r. Convert one object's speed along r and transfer it to
    // the other object.
    obj2->velX += obj2->conversionRatio * projectedV1 * rx - projectedV2 * -rx;
    obj2->velZ += obj2->conversionRatio * projectedV1 * rz - projectedV2 * -rz;

    obj1->velX += -projectedV1 * rx + obj1->conversionRatio * projectedV2 * -rx;
    obj1->velZ += -projectedV1 * rz + obj1->conversionRatio * projectedV2 * -rz;

    //! Bully battery
}

BAD_RETURN(s32) init_bully_collision_data(struct BullyCollisionData *data, f32 posX, f32 posZ,
                               f32 forwardVel, s16 yaw, f32 conversionRatio, f32 radius) {
    if (forwardVel < 0.0f) {
        forwardVel *= -1.0f;
        yaw += 0x8000;
    }

    data->radius = radius;
    data->conversionRatio = conversionRatio;
    data->posX = posX;
    data->posZ = posZ;
    data->velX = forwardVel * sins(yaw);
    data->velZ = forwardVel * coss(yaw);
}

void mario_bonk_reflection(struct MarioState *m, u8 negateSpeed) {
    if (!m) { return; }
    if (m->wall != NULL) {
        s16 wallAngle = atan2s(m->wallNormal[2], m->wallNormal[0]);
        m->faceAngle[1] = wallAngle - (s16)(m->faceAngle[1] - wallAngle);

        play_sound((m->flags & MARIO_METAL_CAP) ? SOUND_ACTION_METAL_BONK : SOUND_ACTION_BONK,
                   m->marioObj->header.gfx.cameraToObject);
    } else {
        play_sound(SOUND_ACTION_HIT, m->marioObj->header.gfx.cameraToObject);
    }

    if (negateSpeed) {
        mario_set_forward_vel(m, -m->forwardVel);
    } else {
        m->faceAngle[1] += 0x8000;
    }
}

u32 mario_update_quicksand(struct MarioState *m, f32 sinkingSpeed) {
    if (!m) { return 0; }
    bool allow = true;
    smlua_call_event_hooks_mario_param_and_int_ret_bool(HOOK_ALLOW_HAZARD_SURFACE, m, HAZARD_TYPE_QUICKSAND, &allow);
    extern bool gDjuiInMainMenu;
    if (m->action & ACT_FLAG_RIDING_SHELL || (!allow) || gDjuiInMainMenu) {
        m->quicksandDepth = 0.0f;
    } else {
        if (m->quicksandDepth < 1.1f) {
            m->quicksandDepth = 1.1f;
        }

        u32 floorType = m->floor ? m->floor->type : SURFACE_DEFAULT;

        switch (floorType) {
            case SURFACE_SHALLOW_QUICKSAND:
                if ((m->quicksandDepth += sinkingSpeed) >= 10.0f) {
                    m->quicksandDepth = 10.0f;
                }
                break;

            case SURFACE_SHALLOW_MOVING_QUICKSAND:
                if ((m->quicksandDepth += sinkingSpeed) >= 25.0f) {
                    m->quicksandDepth = 25.0f;
                }
                break;

            case SURFACE_QUICKSAND:
            case SURFACE_MOVING_QUICKSAND:
                if ((m->quicksandDepth += sinkingSpeed) >= 60.0f) {
                    m->quicksandDepth = 60.0f;
                }
                break;

            case SURFACE_DEEP_QUICKSAND:
            case SURFACE_DEEP_MOVING_QUICKSAND:
                if ((m->quicksandDepth += sinkingSpeed) >= m->marioObj->hitboxHeight) {
                    update_mario_sound_and_camera(m);
                    return drop_and_set_mario_action(m, ACT_QUICKSAND_DEATH, 0);
                }
                break;

            case SURFACE_INSTANT_QUICKSAND:
            case SURFACE_INSTANT_MOVING_QUICKSAND:
                update_mario_sound_and_camera(m);
                return drop_and_set_mario_action(m, ACT_QUICKSAND_DEATH, 0);
                break;

            default:
                m->quicksandDepth = 0.0f;
                break;
        }
    }

    return FALSE;
}

u32 mario_push_off_steep_floor(struct MarioState *m, u32 action, u32 actionArg) {
    if (!m) { return 0; }
    s16 floorDYaw = m->floorAngle - m->faceAngle[1];

    if (floorDYaw > -0x4000 && floorDYaw < 0x4000) {
        m->forwardVel = 16.0f;
        m->faceAngle[1] = m->floorAngle;
    } else {
        m->forwardVel = -16.0f;
        m->faceAngle[1] = m->floorAngle + 0x8000;
    }

    return set_mario_action(m, action, actionArg);
}

u32 mario_update_moving_sand(struct MarioState *m) {
    if (!m) { return 0; }
    struct Surface *floor = m->floor;
    if (!floor) { return 0; }
    s32 floorType = floor->type;

    if (floorType == SURFACE_DEEP_MOVING_QUICKSAND || floorType == SURFACE_SHALLOW_MOVING_QUICKSAND
        || floorType == SURFACE_MOVING_QUICKSAND || floorType == SURFACE_INSTANT_MOVING_QUICKSAND) {
        s16 pushAngle = floor->force << 8;
        f32 pushSpeed = sMovingSandSpeeds[floor->force >> 8];

        m->vel[0] += pushSpeed * sins(pushAngle);
        m->vel[2] += pushSpeed * coss(pushAngle);

        return TRUE;
    }

    return FALSE;
}

u32 mario_update_windy_ground(struct MarioState *m) {
    if (!m) { return 0; }
    struct Surface *floor = m->floor;
    if (!floor) { return 0; }
    bool allow = true;
    smlua_call_event_hooks_mario_param_and_int_ret_bool(HOOK_ALLOW_HAZARD_SURFACE, m, HAZARD_TYPE_HORIZONTAL_WIND, &allow);
    if (!allow) {
    	return FALSE;
    }
    
    extern bool gDjuiInMainMenu;
    if (floor->type == SURFACE_HORIZONTAL_WIND && !gDjuiInMainMenu) {
        f32 pushSpeed;
        s16 pushAngle = floor->force << 8;

        if (m->action & ACT_FLAG_MOVING) {
            s16 pushDYaw = m->faceAngle[1] - pushAngle;

            pushSpeed = m->forwardVel > 0.0f ? -m->forwardVel * 0.5f : -8.0f;

            if (pushDYaw > -0x4000 && pushDYaw < 0x4000) {
                pushSpeed *= -1.0f;
            }

            pushSpeed *= coss(pushDYaw);
        } else {
            pushSpeed = 3.2f + (gGlobalTimer % 4);
        }

        m->vel[0] += pushSpeed * sins(pushAngle);
        m->vel[2] += pushSpeed * coss(pushAngle);

#ifdef VERSION_JP
        play_sound(SOUND_ENV_WIND2, m->marioObj->header.gfx.cameraToObject);
#endif
        return TRUE;
    }

    return FALSE;
}

void stop_and_set_height_to_floor(struct MarioState *m) {
    if (!m) { return; }
    struct Object *marioObj = m->marioObj;

    mario_set_forward_vel(m, 0.0f);
    m->vel[1] = 0.0f;

    //! This is responsible for some downwarps.
    m->pos[1] = m->floorHeight;

    vec3f_copy(marioObj->header.gfx.pos, m->pos);
    vec3s_set(marioObj->header.gfx.angle, 0, m->faceAngle[1], 0);
}

s32 stationary_ground_step(struct MarioState *m) {
    if (!m) { return 0; }
    u32 takeStep;
    struct Object *marioObj = m->marioObj;
    u32 stepResult = GROUND_STEP_NONE;

    mario_set_forward_vel(m, 0.0f);

    takeStep = mario_update_moving_sand(m);
    takeStep |= mario_update_windy_ground(m);
    if (takeStep) {
        stepResult = perform_ground_step(m);
    } else {
        //! This is responsible for several stationary downwarps.
        m->pos[1] = m->floorHeight;

        vec3f_copy(marioObj->header.gfx.pos, m->pos);
        vec3s_set(marioObj->header.gfx.angle, 0, m->faceAngle[1], 0);
    }

    return stepResult;
}

static s32 perform_ground_quarter_step(struct MarioState *m, Vec3f nextPos) {
    if (!m) { return 0; }
    struct WallCollisionData lowerWcd = { 0 };
    struct WallCollisionData upperWcd = { 0 };
    struct Surface *ceil;
    struct Surface *floor;
    f32 ceilHeight;
    f32 floorHeight;
    f32 waterLevel;

    resolve_and_return_wall_collisions_data(nextPos, 30.0f, 24.0f, &lowerWcd);
    resolve_and_return_wall_collisions_data(nextPos, 60.0f, 50.0f, &upperWcd);

    floorHeight = find_floor(nextPos[0], nextPos[1], nextPos[2], &floor);
    ceilHeight = vec3f_mario_ceil(nextPos, floorHeight, &ceil);

    waterLevel = find_water_level(nextPos[0], nextPos[2]);

    mario_update_wall(m, &upperWcd);

    if (floor == NULL) {
        if (gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) {
            m->faceAngle[1] += 0x8000;
            mario_set_forward_vel(m, gServerSettings.bouncyLevelBounds == BOUNCY_LEVEL_BOUNDS_ON_CAP ? clamp(1.5f * m->forwardVel, -500, 500) : 1.5f * m->forwardVel);
        }
        smlua_call_event_hooks_mario_param(HOOK_ON_COLLIDE_LEVEL_BOUNDS, m);
        return GROUND_STEP_HIT_WALL_STOP_QSTEPS;
    }

    if ((m->action & ACT_FLAG_RIDING_SHELL) && floorHeight < waterLevel) {
        bool allow = true;
        smlua_call_event_hooks_mario_param_and_bool_ret_bool(HOOK_ALLOW_FORCE_WATER_ACTION, m, false, &allow);
        if (allow) {
            floorHeight = waterLevel;
            floor = &gWaterSurfacePseudoFloor;
            floor->originOffset = floorHeight; //! Wrong origin offset (no effect)
        }
    }

    if (nextPos[1] > floorHeight + 100.0f) {
        if (nextPos[1] + m->marioObj->hitboxHeight >= ceilHeight) {
            return GROUND_STEP_HIT_WALL_STOP_QSTEPS;
        }

        vec3f_copy(m->pos, nextPos);
        m->floor = floor;
        m->floorHeight = floorHeight;
        return GROUND_STEP_LEFT_GROUND;
    }

    if (floorHeight + m->marioObj->hitboxHeight >= ceilHeight) {
        return GROUND_STEP_HIT_WALL_STOP_QSTEPS;
    }

    vec3f_set(m->pos, nextPos[0], floorHeight, nextPos[2]);
    m->floor = floor;
    m->floorHeight = floorHeight;

    if (upperWcd.numWalls > 0) {
        for (u8 i = 0; i < upperWcd.numWalls; i++) {
            if (!gLevelValues.fixCollisionBugs) {
                i = (upperWcd.numWalls - 1);
            }
            struct Surface* wall = upperWcd.walls[i];
            s16 wallDYaw = atan2s(wall->normal.z, wall->normal.x) - m->faceAngle[1];

            if (wallDYaw >= 0x2AAA && wallDYaw <= 0x5555) {
                // nothing
            } else if (wallDYaw <= -0x2AAA && wallDYaw >= -0x5555) {
                // nothing
            } else {
                return GROUND_STEP_HIT_WALL_CONTINUE_QSTEPS;
            }
        }
    }

    return GROUND_STEP_NONE;
}

s32 perform_ground_step(struct MarioState *m) {
    if (!m) { return 0; }
    s32 i;
    u32 stepResult;
    Vec3f intendedPos;

    s32 returnValue = 0;
    if (smlua_call_event_hooks_mario_param_and_int_ret_int(HOOK_BEFORE_PHYS_STEP, m, STEP_TYPE_GROUND, &returnValue)) return returnValue;

    for (i = 0; i < 4; i++) {
        Vec3f step = { 0 };
        if (m->floor) {
            f32 floorNormal;
            if (!smlua_call_event_hooks_mario_param_ret_float(HOOK_MARIO_OVERRIDE_PHYS_STEP_DEFACTO_SPEED, m, &floorNormal)) {
                floorNormal = m->floor->normal.y;
            }
            step[0] = floorNormal * (m->vel[0] / 4.0f);
            step[2] = floorNormal * (m->vel[2] / 4.0f);
        }

        intendedPos[0] = m->pos[0] + step[0];
        intendedPos[1] = m->pos[1];
        intendedPos[2] = m->pos[2] + step[2];

        vec3f_normalize(step);

        vec3f_copy(gFindWallDirection, step);

        gFindWallDirectionActive = true;
        stepResult = perform_ground_quarter_step(m, intendedPos);
        gFindWallDirectionActive = false;

        if (stepResult == GROUND_STEP_LEFT_GROUND || stepResult == GROUND_STEP_HIT_WALL_STOP_QSTEPS) {
            break;
        }
    }

    m->terrainSoundAddend = mario_get_terrain_sound_addend(m);
    vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
    vec3s_set(m->marioObj->header.gfx.angle, 0, m->faceAngle[1], 0);

    if (stepResult == GROUND_STEP_HIT_WALL_CONTINUE_QSTEPS) {
        stepResult = GROUND_STEP_HIT_WALL;
    }
    return stepResult;
}

u32 check_ledge_grab(struct MarioState *m, struct Surface *wall, Vec3f intendedPos, Vec3f nextPos) {
    if (!m) { return 0; }
    struct Surface *ledgeFloor;
    Vec3f ledgePos;
    f32 displacementX;
    f32 displacementZ;

    if (m->vel[1] > 0) {
        return FALSE;
    }

    displacementX = nextPos[0] - intendedPos[0];
    displacementZ = nextPos[2] - intendedPos[2];

    // Only ledge grab if the wall displaced Mario in the opposite direction of
    // his velocity.
    if (displacementX * m->vel[0] + displacementZ * m->vel[2] > 0.0f) {
        return FALSE;
    }

    //! Since the search for floors starts at y + m->marioObj->hitboxHeight (160.0f), we will sometimes grab
    // a higher ledge than expected (glitchy ledge grab)
    ledgePos[0] = nextPos[0] - wall->normal.x * 60.0f;
    ledgePos[2] = nextPos[2] - wall->normal.z * 60.0f;
    ledgePos[1] = find_floor(ledgePos[0], nextPos[1] + m->marioObj->hitboxHeight, ledgePos[2], &ledgeFloor);

    if (!ledgeFloor) { return FALSE; }

    if (gLevelValues.fixCollisionBugs && gLevelValues.fixCollisionBugsFalseLedgeGrab) {
        // fix false ledge grabs
        if (!ledgeFloor || ledgeFloor->normal.y < 0.90630779f) {
            return FALSE;
        }
    }

    if (ledgePos[1] - nextPos[1] <= 100.0f) {
        return FALSE;
    }

    vec3f_copy(m->pos, ledgePos);
    m->floor = ledgeFloor;
    m->floorHeight = ledgePos[1];

    m->floorAngle = atan2s(ledgeFloor->normal.z, ledgeFloor->normal.x);

    m->faceAngle[0] = 0;
    m->faceAngle[1] = atan2s(wall->normal.z, wall->normal.x) + 0x8000;
    return TRUE;
}

s32 perform_air_quarter_step(struct MarioState *m, Vec3f intendedPos, u32 stepArg) {
    if (!m) { return 0; }
    s16 wallDYaw;
    Vec3f nextPos;
    struct WallCollisionData lowerWcd = { 0 };
    struct WallCollisionData upperWcd = { 0 };
    struct Surface *ceil;
    struct Surface *floor;
    f32 ceilHeight;
    f32 floorHeight;
    f32 waterLevel;

    vec3f_copy(nextPos, intendedPos);

    resolve_and_return_wall_collisions_data(nextPos, 150.0f, 50.0f, &upperWcd);
    resolve_and_return_wall_collisions_data(nextPos, 30.0f, 50.0f, &lowerWcd);

    floorHeight = find_floor(nextPos[0], nextPos[1], nextPos[2], &floor);
    ceilHeight = vec3f_mario_ceil(nextPos, floorHeight, &ceil);

    waterLevel = find_water_level(nextPos[0], nextPos[2]);

    m->wall = NULL;

    //! The water pseudo floor is not referenced when your intended qstep is
    // out of bounds, so it won't detect you as landing.

    if (floor == NULL) {
        if (nextPos[1] <= m->floorHeight) {
            m->pos[1] = m->floorHeight;
            return AIR_STEP_LANDED;
        }

        m->pos[1] = nextPos[1];
        if (gServerSettings.bouncyLevelBounds != BOUNCY_LEVEL_BOUNDS_OFF) {
            m->faceAngle[1] += 0x8000;
            mario_set_forward_vel(m, gServerSettings.bouncyLevelBounds == BOUNCY_LEVEL_BOUNDS_ON_CAP ? clamp(1.5f * m->forwardVel, -500, 500) : 1.5f * m->forwardVel);
        }
        smlua_call_event_hooks_mario_param(HOOK_ON_COLLIDE_LEVEL_BOUNDS, m);
        return AIR_STEP_HIT_WALL;
    }

    if ((m->action & ACT_FLAG_RIDING_SHELL) && floorHeight < waterLevel) {
        bool allow = true;
        smlua_call_event_hooks_mario_param_and_bool_ret_bool(HOOK_ALLOW_FORCE_WATER_ACTION, m, false, &allow);
        if (allow) {
            floorHeight = waterLevel;
            floor = &gWaterSurfacePseudoFloor;
            floor->originOffset = floorHeight; //! Incorrect origin offset (no effect)
        }
    }

    //! This check uses f32, but findFloor uses short (overflow jumps)
    if (nextPos[1] <= floorHeight) {
        if (ceilHeight - floorHeight > m->marioObj->hitboxHeight) {
            m->pos[0] = nextPos[0];
            m->pos[2] = nextPos[2];
            m->floor = floor;
            m->floorHeight = floorHeight;
        }

        //! When ceilHeight - floorHeight <= m->marioObj->hitboxHeight (160.0f), the step result says that
        // Mario landed, but his movement is cancelled and his referenced floor
        // isn't updated (pedro spots)
        m->pos[1] = floorHeight;
        return AIR_STEP_LANDED;
    }

    if (nextPos[1] + m->marioObj->hitboxHeight > ceilHeight) {
        if (m->vel[1] >= 0.0f) {
            m->vel[1] = 0.0f;

            //! Uses referenced ceiling instead of ceil (ceiling hang upwarp)
            if ((stepArg & AIR_STEP_CHECK_HANG) && m->ceil != NULL
                && m->ceil->type == SURFACE_HANGABLE) {
                return AIR_STEP_GRABBED_CEILING;
            }

            return AIR_STEP_NONE;
        }

        //! Potential subframe downwarp->upwarp?
        if (nextPos[1] <= m->floorHeight) {
            m->pos[1] = m->floorHeight;
            return AIR_STEP_LANDED;
        }

        m->pos[1] = nextPos[1];
        return AIR_STEP_HIT_WALL;
    }

    //! When the wall is not completely vertical or there is a slight wall
    // misalignment, you can activate these conditions in unexpected situations
    if ((stepArg & AIR_STEP_CHECK_LEDGE_GRAB) && upperWcd.numWalls == 0 && lowerWcd.numWalls > 0) {
        for (u8 i = 0; i < lowerWcd.numWalls; i++) {
            if (!gLevelValues.fixCollisionBugs) {
                i = (lowerWcd.numWalls - 1);
            }
            struct Surface* wall = lowerWcd.walls[i];
            if (check_ledge_grab(m, wall, intendedPos, nextPos)) {
                return AIR_STEP_GRABBED_LEDGE;
            }
        }

        vec3f_copy(m->pos, nextPos);
        m->floor = floor;
        m->floorHeight = floorHeight;
        return AIR_STEP_NONE;
    }

    vec3f_copy(m->pos, nextPos);
    m->floor = floor;
    m->floorHeight = floorHeight;

    if (upperWcd.numWalls > 0) {
        mario_update_wall(m, &upperWcd);

        for (u8 i = 0; i < upperWcd.numWalls; i++) {
            if (!gLevelValues.fixCollisionBugs) {
                i = (upperWcd.numWalls - 1);
            }

            struct Surface* wall = upperWcd.walls[i];
            wallDYaw = atan2s(wall->normal.z, wall->normal.x) - m->faceAngle[1];

            if (wall->type == SURFACE_BURNING) {
                m->wall = wall;
                return AIR_STEP_HIT_LAVA_WALL;
            }

            if (wallDYaw < -0x6000 || wallDYaw > 0x6000) {
                m->wall = wall;
                m->flags |= MARIO_UNKNOWN_30;
                return AIR_STEP_HIT_WALL;
            }
        }
    } else if (lowerWcd.numWalls > 0) {
        mario_update_wall(m, &lowerWcd);

        for (u8 i = 0; i < lowerWcd.numWalls; i++) {
            if (!gLevelValues.fixCollisionBugs) {
                i = (lowerWcd.numWalls - 1);
            }

            struct Surface* wall = lowerWcd.walls[i];
            wallDYaw = atan2s(wall->normal.z, wall->normal.x) - m->faceAngle[1];

            if (wall->type == SURFACE_BURNING) {
                m->wall = wall;
                return AIR_STEP_HIT_LAVA_WALL;
            }

            if (wallDYaw < -0x6000 || wallDYaw > 0x6000) {
                m->wall = wall;
                m->flags |= MARIO_UNKNOWN_30;
                return AIR_STEP_HIT_WALL;
            }
        }
    }

    return AIR_STEP_NONE;
}

void apply_twirl_gravity(struct MarioState *m) {
    if (!m) { return; }
    f32 terminalVelocity;
    f32 heaviness = 1.0f;

    if (m->angleVel[1] > 1024) {
        heaviness = 1024.0f / m->angleVel[1];
    }

    terminalVelocity = -75.0f * heaviness;

    m->vel[1] -= 4.0f * heaviness;
    if (m->vel[1] < terminalVelocity) {
        m->vel[1] = terminalVelocity;
    }
}

u32 should_strengthen_gravity_for_jump_ascent(struct MarioState *m) {
    if (!m) { return 0; }
    if (!(m->flags & MARIO_UNKNOWN_08)) {
        return FALSE;
    }

    if (m->action & (ACT_FLAG_INTANGIBLE | ACT_FLAG_INVULNERABLE)) {
        return FALSE;
    }

    if (!(m->input & INPUT_A_DOWN) && m->vel[1] > 20.0f) {
        return (m->action & ACT_FLAG_CONTROL_JUMP_HEIGHT) != 0;
    }

    return FALSE;
}

void apply_gravity(struct MarioState *m) {
    if (!m) { return; }
    s32 result;

    if (smlua_call_action_hook(ACTION_HOOK_GRAVITY, m, &result)) {
        
    } else if (m->action == ACT_TWIRLING && m->vel[1] < 0.0f) {
        apply_twirl_gravity(m);
    } else if (m->action == ACT_SHOT_FROM_CANNON) {
        m->vel[1] -= 1.0f;
        if (m->vel[1] < -75.0f) {
            m->vel[1] = -75.0f;
        }
    } else if (m->action == ACT_LONG_JUMP || m->action == ACT_SLIDE_KICK
               || m->action == ACT_BBH_ENTER_SPIN) {
        m->vel[1] -= 2.0f;
        if (m->vel[1] < -75.0f) {
            m->vel[1] = -75.0f;
        }
    } else if (m->action == ACT_LAVA_BOOST || m->action == ACT_FALL_AFTER_STAR_GRAB) {
        m->vel[1] -= 3.2f;
        if (m->vel[1] < -65.0f) {
            m->vel[1] = -65.0f;
        }
    } else if (m->action == ACT_GETTING_BLOWN) {
        m->vel[1] -= m->unkC4;
        if (m->vel[1] < -75.0f) {
            m->vel[1] = -75.0f;
        }
    } else if (should_strengthen_gravity_for_jump_ascent(m)) {
        m->vel[1] /= 4.0f;
    } else if (m->action & ACT_FLAG_METAL_WATER) {
        m->vel[1] -= 1.6f;
        if (m->vel[1] < -16.0f) {
            m->vel[1] = -16.0f;
        }
    } else if ((m->flags & MARIO_WING_CAP) && m->vel[1] < 0.0f && (m->input & INPUT_A_DOWN)) {
        m->marioBodyState->wingFlutter = TRUE;

        m->vel[1] -= 2.0f;
        if (m->vel[1] < -37.5f) {
            if ((m->vel[1] += 4.0f) > -37.5f) {
                m->vel[1] = -37.5f;
            }
        }
    } else {
        m->vel[1] -= 4.0f;
        if (m->vel[1] < -75.0f) {
            m->vel[1] = -75.0f;
        }
    }
}

void apply_vertical_wind(struct MarioState *m) {
    if (!m) { return; }
    f32 maxVelY;
    f32 offsetY;
    bool allow = true;
    smlua_call_event_hooks_mario_param_and_int_ret_bool(HOOK_ALLOW_HAZARD_SURFACE, m, HAZARD_TYPE_VERTICAL_WIND, &allow);
    if (m->action != ACT_GROUND_POUND && allow) {
        offsetY = m->pos[1] - -1500.0f;

        if (m->floor && m->floor->type == SURFACE_VERTICAL_WIND && -3000.0f < offsetY && offsetY < 2000.0f) {
            if (offsetY >= 0.0f) {
                maxVelY = 10000.0f / (offsetY + 200.0f);
            } else {
                maxVelY = 50.0f;
            }

            if (m->vel[1] < maxVelY) {
                if ((m->vel[1] += maxVelY / 8.0f) > maxVelY) {
                    m->vel[1] = maxVelY;
                }
            }

#ifdef VERSION_JP
            play_sound(SOUND_ENV_WIND2, m->marioObj->header.gfx.cameraToObject);
#endif
        }
    }
}

s32 perform_air_step(struct MarioState *m, u32 stepArg) {
    if (!m) { return 0; }
    Vec3f intendedPos;
    s32 i;
    s32 quarterStepResult;
    s32 stepResult = AIR_STEP_NONE;

    s32 returnValue = 0;
    if (smlua_call_event_hooks_mario_param_and_int_and_int_ret_int(HOOK_BEFORE_PHYS_STEP, m, STEP_TYPE_AIR, stepArg, &returnValue)) return returnValue;

    m->wall = NULL;

    for (i = 0; i < 4; i++) {
        Vec3f step = {
            m->vel[0] / 4.0f,
            m->vel[1] / 4.0f,
            m->vel[2] / 4.0f,
        };

        intendedPos[0] = m->pos[0] + step[0];
        intendedPos[1] = m->pos[1] + step[1];
        intendedPos[2] = m->pos[2] + step[2];

        vec3f_normalize(step);
        vec3f_copy(gFindWallDirection, step);

        gFindWallDirectionActive = true;
        gFindWallDirectionAirborne = true;
        quarterStepResult = perform_air_quarter_step(m, intendedPos, stepArg);
        gFindWallDirectionAirborne = false;
        gFindWallDirectionActive = false;

        //! On one qf, hit OOB/ceil/wall to store the 2 return value, and continue
        // getting 0s until your last qf. Graze a wall on your last qf, and it will
        // return the stored 2 with a sharply angled reference wall. (some gwks)

        if (quarterStepResult != AIR_STEP_NONE) {
            stepResult = quarterStepResult;
        }

        if (quarterStepResult == AIR_STEP_LANDED || quarterStepResult == AIR_STEP_GRABBED_LEDGE
            || quarterStepResult == AIR_STEP_GRABBED_CEILING
            || quarterStepResult == AIR_STEP_HIT_LAVA_WALL) {
            break;
        }
    }

    if (m->vel[1] >= 0.0f) {
        m->peakHeight = m->pos[1];
    }

    m->terrainSoundAddend = mario_get_terrain_sound_addend(m);

    if (m->action != ACT_FLYING && m->action != ACT_BUBBLED) {
        apply_gravity(m);
    }
    apply_vertical_wind(m);

    vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
    vec3s_set(m->marioObj->header.gfx.angle, 0, m->faceAngle[1], 0);

    return stepResult;
}

// They had these functions the whole time and never used them? Lol

void set_vel_from_pitch_and_yaw(struct MarioState *m) {
    if (!m) { return; }
    m->vel[0] = m->forwardVel * coss(m->faceAngle[0]) * sins(m->faceAngle[1]);
    m->vel[1] = m->forwardVel * sins(m->faceAngle[0]);
    m->vel[2] = m->forwardVel * coss(m->faceAngle[0]) * coss(m->faceAngle[1]);
}

void set_vel_from_yaw(struct MarioState *m) {
    if (!m) { return; }
    m->vel[0] = m->slideVelX = m->forwardVel * sins(m->faceAngle[1]);
    m->vel[1] = 0.0f;
    m->vel[2] = m->slideVelZ = m->forwardVel * coss(m->faceAngle[1]);
}
