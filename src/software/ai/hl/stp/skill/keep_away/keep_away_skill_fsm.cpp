#include "software/ai/hl/stp/skill/keep_away/keep_away_skill_fsm.h"

#include "software/ai/evaluation/keep_away.h"

bool KeepAwaySkillFSM::isPossessionThreatened(const Update& event)
{
    const TbotsProto::AiConfig& ai_config = event.common.strategy->getAiConfig();

    if (!event.common.robot.isNearDribbler(
            event.common.world_ptr->ball().position(),
            ai_config.dribble_config().lose_ball_control_threshold()))
    {
        return true;
    }

    return shouldKeepAway(
        event.common.robot, event.common.world_ptr->enemyTeam(),
        ai_config.attacker_tactic_config().enemy_about_to_steal_ball_radius());
}

void KeepAwaySkillFSM::keepAway(
    const Update& event, boost::sml::back::process<DribbleSkillFSM::Update> processEvent)
{
    Pass best_pass = event.common.strategy->getBestPass().pass;
    auto keepaway_dribble_dest =
        findKeepAwayTargetPoint(*event.common.world_ptr, best_pass,
                                event.common.strategy->getAiConfig().passing_config());

    const Team& enemy_team = event.common.world_ptr->enemyTeam();
    const Ball& ball       = event.common.world_ptr->ball();

    auto final_dribble_orientation = best_pass.passerOrientation();

    // If there is a robot on the enemy team close to us, face away from the nearest one
    auto nearest_enemy_robot = enemy_team.getNearestRobot(event.common.robot.position());
    if (nearest_enemy_robot.has_value() &&
        distance(ball.position(), nearest_enemy_robot->position()) <
            event.common.strategy->getAiConfig()
                .attacker_tactic_config()
                .enemy_about_to_steal_ball_radius())
    {
        auto dribble_orientation_vec = ball.position() - nearest_enemy_robot->position();
        final_dribble_orientation    = dribble_orientation_vec.orientation();
    }

    DribbleSkillFSM::ControlParams control_params = {
        .dribble_destination       = keepaway_dribble_dest,
        .final_dribble_orientation = final_dribble_orientation,
        .allow_excessive_dribbling = false};

    processEvent(DribbleSkillFSM::Update(control_params, event.common));
}
