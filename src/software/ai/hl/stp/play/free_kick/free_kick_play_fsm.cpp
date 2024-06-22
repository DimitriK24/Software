#include "free_kick_play_fsm.h"

FreeKickPlayFSM::FreeKickPlayFSM(TbotsProto::AiConfig ai_config)
    : ai_config(ai_config),
      best_pass_and_score_so_far(
          PassWithRating{.pass = Pass(Point(), Point(), 0), .rating = 0}),
      align_to_ball_tactic(std::make_shared<MoveTactic>()),
      shoot_tactic(std::make_shared<KickTactic>()),
      chip_tactic(std::make_shared<ChipTactic>()),
      passer_tactic(std::make_shared<KickTactic>()),
      receiver_tactic(std::make_shared<ReceiverTactic>()),
      offensive_positioning_tactics(
          {std::make_shared<MoveTactic>(), std::make_shared<MoveTactic>()}),
      crease_defender_tactics({std::make_shared<CreaseDefenderTactic>(
                                   ai_config.robot_navigation_obstacle_config()),
                               std::make_shared<CreaseDefenderTactic>(
                                   ai_config.robot_navigation_obstacle_config())}),
      pass_generator(
          PassGenerator<EighteenZoneId>(std::make_shared<const EighteenZonePitchDivision>(
                                            Field::createSSLDivisionBField()),
                                        ai_config.passing_config()))
{
}

void FreeKickPlayFSM::setupPosition(const Update &event)
{
    if (ranked_zones.empty())
    {
        ranked_zones =
            pass_generator.generatePassEvaluation(*event.common.world_ptr)
                .rankZonesForReceiving(*event.common.world_ptr,
                                       best_pass_and_score_so_far.pass.receiverPoint());
    }
    PriorityTacticVector tactics_to_run = {{}};

    updateAlignToBallTactic(event.common.world_ptr);
    tactics_to_run[0].emplace_back(align_to_ball_tactic);

    updateOffensivePositioningTactics(event.common.world_ptr);
    tactics_to_run[0].emplace_back(offensive_positioning_tactics[0]);
    tactics_to_run[0].emplace_back(offensive_positioning_tactics[1]);

    crease_defender_tactics[0]->updateControlParams(
        event.common.world_ptr->ball().position(), TbotsProto::CreaseDefenderAlignment::LEFT);
    crease_defender_tactics[1]->updateControlParams(
        event.common.world_ptr->ball().position(), TbotsProto::CreaseDefenderAlignment::RIGHT);
    tactics_to_run[0].emplace_back(crease_defender_tactics[0]);
    tactics_to_run[0].emplace_back(crease_defender_tactics[1]);

    event.common.set_tactics(tactics_to_run);
}

bool FreeKickPlayFSM::setupDone(const Update &event)
{
    if (align_to_ball_tactic->done())
    {
        LOG(DEBUG) << "Finished aligning to ball.";
    }
    return align_to_ball_tactic->done();
}

void FreeKickPlayFSM::updateOffensivePositioningTactics(const WorldPtr &world_ptr)
{
    using Zones = std::unordered_set<EighteenZoneId>;

    auto pass_eval = pass_generator.generatePassEvaluation(*world_ptr);

    for (unsigned int i = 0; i < offensive_positioning_tactics.size(); i++)
    {
        Zones zone = {ranked_zones[i]};
        auto pass  = pass_eval.getBestPassInZones(zone).pass;

        offensive_positioning_tactics[i]->updateControlParams(
            pass.receiverPoint(), pass.receiverOrientation(), 0.0);
    }
}

void FreeKickPlayFSM::updateAlignToBallTactic(const WorldPtr &world_ptr)
{
    // We want the kicker to get into position behind the ball facing the enemy net
    Point ball_pos                = world_ptr->ball().position();
    Vector ball_to_enemy_goal_vec = world_ptr->field().enemyGoalCenter() - ball_pos;
    align_to_ball_tactic->updateControlParams(
        ball_pos - ball_to_enemy_goal_vec.normalize(ROBOT_MAX_RADIUS_METERS * 2),
        ball_to_enemy_goal_vec.orientation(), 0);
}

bool FreeKickPlayFSM::shotFound(const Update &event)
{
    shot = calcBestShotOnGoal(event.common.world_ptr->field(),
                              event.common.world_ptr->friendlyTeam(),
                              event.common.world_ptr->enemyTeam(),
                              event.common.world_ptr->ball().position(), TeamType::ENEMY);
    return shot &&
           shot->getOpenAngle() >
               Angle::fromDegrees(
                   ai_config.attacker_tactic_config().min_open_angle_for_shot_deg());
}

void FreeKickPlayFSM::shootBall(const Update &event)
{
    LOG(DEBUG) << "Shooting ball...";
    PriorityTacticVector tactics_to_run = {{}};

    Point ball_pos = event.common.world_ptr->ball().position();

    shoot_tactic->updateControlParams(
        ball_pos, (shot->getPointToShootAt() - ball_pos).orientation(),
        BALL_MAX_SPEED_METERS_PER_SECOND - 0.5);
    tactics_to_run[0].emplace_back(shoot_tactic);

    event.common.set_tactics(tactics_to_run);
}

void FreeKickPlayFSM::startLookingForPass(const FreeKickPlayFSM::Update &event)
{
    pass_optimization_start_time = event.common.world_ptr->getMostRecentTimestamp();
    // Generate the best zones for receiving a pass
    // Only generate the zones once to avoid oscillatory behaviour
    ranked_zones =
        pass_generator.generatePassEvaluation(*event.common.world_ptr)
            .rankZonesForReceiving(*event.common.world_ptr,
                                   best_pass_and_score_so_far.pass.receiverPoint());
}

bool FreeKickPlayFSM::timeExpired(const FreeKickPlayFSM::Update &event)
{
    Duration time_since_pass_optimization_start =
        event.common.world_ptr->getMostRecentTimestamp() - pass_optimization_start_time;
    return time_since_pass_optimization_start.toSeconds() >
           ai_config.free_kick_play_config().max_time_commit_to_pass_seconds();
}

void FreeKickPlayFSM::chipBall(const Update &event)
{
    LOG(DEBUG) << "Time to look for pass expired. Chipping ball...";
    PriorityTacticVector tactics_to_run = {{}};

    Point chip_target = event.common.world_ptr->field().enemyGoalCenter();

    chip_tactic->updateControlParams(event.common.world_ptr->ball().position(),
                                     chip_target);
    tactics_to_run[0].emplace_back(chip_tactic);

    event.common.set_tactics(tactics_to_run);
}

void FreeKickPlayFSM::lookForPass(const FreeKickPlayFSM::Update &event)
{
    PriorityTacticVector tactics_to_run = {{}};

    // Keep the kicker aligned to the ball
    updateAlignToBallTactic(event.common.world_ptr);
    tactics_to_run[0].emplace_back(align_to_ball_tactic);

    // Set robots to roam around the field to try to receive a pass
    updateOffensivePositioningTactics(event.common.world_ptr);
    tactics_to_run[0].emplace_back(offensive_positioning_tactics[0]);
    tactics_to_run[0].emplace_back(offensive_positioning_tactics[1]);

    // Maintain crease defenders
    crease_defender_tactics[0]->updateControlParams(
        event.common.world_ptr->ball().position(), TbotsProto::CreaseDefenderAlignment::LEFT);
    crease_defender_tactics[1]->updateControlParams(
        event.common.world_ptr->ball().position(), TbotsProto::CreaseDefenderAlignment::RIGHT);
    tactics_to_run[0].emplace_back(crease_defender_tactics[0]);
    tactics_to_run[0].emplace_back(crease_defender_tactics[1]);

    best_pass_and_score_so_far =
        pass_generator.generatePassEvaluation(*event.common.world_ptr)
            .getBestPassOnField();

    event.common.set_tactics(tactics_to_run);
}

bool FreeKickPlayFSM::passFound(const Update &event)
{
    double time_since_pass_optimization_start_seconds =
        (event.common.world_ptr->getMostRecentTimestamp() - pass_optimization_start_time).toSeconds();

    // To get the best pass possible we start by aiming for a perfect one and then
    // decrease the minimum score over time
    double min_score = 1 - (1 - ai_config.free_kick_play_config().min_acceptable_pass_score()) * (time_since_pass_optimization_start_seconds / ai_config.free_kick_play_config().max_time_commit_to_pass_seconds());
    LOG(DEBUG) << "Score: " << best_pass_and_score_so_far.rating << " Min score: " << min_score;

    return best_pass_and_score_so_far.rating > min_score;
}

void FreeKickPlayFSM::passBall(const Update &event)
{
    PriorityTacticVector tactics_to_run = {{}};

    // We have committed to the pass
    LOG(DEBUG) << "Found pass with score: " << best_pass_and_score_so_far.rating
               << ". Passing...";

    Pass pass = best_pass_and_score_so_far.pass;

    chip_tactic->updateControlParams(event.common.world_ptr->ball().position(),
                                     pass.receiverPoint());
    // passer_tactic->updateControlParams(pass.passerPoint(), pass.passerOrientation(),
    //                                    pass.speed());
    receiver_tactic->updateControlParams(pass);
    tactics_to_run[0].emplace_back(chip_tactic);
    // tactics_to_run[0].emplace_back(passer_tactic);
    tactics_to_run[0].emplace_back(receiver_tactic);

    event.common.set_tactics(tactics_to_run);
}

bool FreeKickPlayFSM::shotDone(const Update &event)
{
    if (shoot_tactic->done())
    {
        LOG(DEBUG) << "Finished shot.";
    }
    return shoot_tactic->done();
}

bool FreeKickPlayFSM::passDone(const FreeKickPlayFSM::Update &event)
{
    if (receiver_tactic->done())
    {
        LOG(DEBUG) << "Finished pass.";
    }
    return receiver_tactic->done();
}

bool FreeKickPlayFSM::chipDone(const FreeKickPlayFSM::Update &event)
{
    if (chip_tactic->done())
    {
        LOG(DEBUG) << "Finished chip.";
    }
    return chip_tactic->done();
}
