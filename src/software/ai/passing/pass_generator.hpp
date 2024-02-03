#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>

#include "eighteen_zone_pitch_division.h"
#include "proto/message_translation/tbots_protobuf.h"
#include "proto/parameters.pb.h"
#include "shared/constants.h"
#include "software/ai/passing/cost_function.h"
#include "software/ai/passing/pass.h"
#include "software/ai/passing/pass_evaluation.hpp"
#include "software/ai/passing/pass_with_rating.h"
#include "software/logger/logger.h"
#include "software/optimization/gradient_descent_optimizer.hpp"
#include "software/time/timestamp.h"
#include "software/world/world.h"

// The random seed to initialize the random number generator
static const int PASS_GENERATOR_SEED = 14;

template <class ZoneEnum>
using ZonePassMap = std::unordered_map<ZoneEnum, PassWithRating>;

/**
 * This class is responsible for generating passes for us to perform
 */
template <class ZoneEnum>
class PassGenerator
{
    static_assert(std::is_enum<ZoneEnum>::value,
                  "PassGenerator: ZoneEnum must be a zone id enum");

   public:
    /**
     * Creates a new PassGenerator with the given pitch_division.
     *
     * The PassGenerator will use this pitch division to guide initial random samples
     * in each zone after the pitch has been divided.
     *
     * @param pitch_division The pitch division to use when looking for passes
     */
    explicit PassGenerator(
        std::shared_ptr<const FieldPitchDivision<ZoneEnum>> pitch_division,
        TbotsProto::PassingConfig passing_config);

    /**
     * Creates a PassEvaluation given a world and a field pitch division.
     *
     * NOTE: If we want to run our AI at 30hz, it gives us 1/30 = 33ms between ticks.
     * This function needs to run in less than 1/3 of that time (< 10ms) to allow
     * for other modules in our AI to have enough time to run.
     *
     * Passes are evaluated on the provided world. If the evaluation takes longer than
     * the time between two vision frames, we will be evaluating on an outdated world.
     *
     * Because of this, it is extremely important that the pass generator runs fast
     * enough. It is recommended that all testing of things involving the PassGenerator
     * be done with executables built in "Release" in order to maximize performance
     * ("Release" can be 2-10x faster then "Debug").
     *
     * @param world The world to compute the pass evaluation on
     *
     * @return The best currently known pass and the rating of that pass (in [0-1])
     */
    PassEvaluation<ZoneEnum> generatePassEvaluation(const World& world);


   private:
    // Weights used to normalize the parameters that we pass to GradientDescent
    // (see the GradientDescent documentation for details)
    // These weights are *very* roughly the step that gradient descent will take
    // in each respective dimension for a single iteration. They are tuned to
    // ensure passes converge as fast as possible, but are also as stable as
    // possible
    static constexpr double PASS_SPACE_WEIGHT                          = 0.1;
    std::array<double, NUM_PARAMS_TO_OPTIMIZE> optimizer_param_weights = {
        PASS_SPACE_WEIGHT, PASS_SPACE_WEIGHT};

    /**
     * Randomly samples a receive point across every zone and assigns a random
     * speed to each pass.
     *
     * @returns a mapping of the Zone Id to the sampled pass
     */
    ZonePassMap<ZoneEnum> samplePasses(const World& world);

    /**
     * Determines the speed at which a pass should be executed
     * Such that it reaches its destination at the given destination speed
     * Takes into account friction
     * @param ball_position the current ball position (starting point of the pass)
     * @param pass_destination the destination of the pass
     * @return the speed the pass should start with
     */
    double getPassSpeed(const Point& ball_position, const Point& pass_destination);

    /**
     * Given a map of passes, runs a gradient descent optimizer to find
     * better passes.
     *
     * @param The world
     * @param The passes to be optimized mapped to the zone
     * @returns a mapping of the Zone id to the optimized pass
     */
    ZonePassMap<ZoneEnum> optimizePasses(const World& world,
                                         const ZonePassMap<ZoneEnum>& initial_passes);

    /**
     * Re-evaluates ratePass on the previous world's passes and keeps the better pass
     * w/ the higher score in current_best_passes_;
     *
     * @param The world
     * @param optimized_passes The optimized_passes to update our internal cached
     * passes with.
     */
    void updatePasses(const World& world, const ZonePassMap<ZoneEnum>& optimized_passes);

    // All the passes that we are currently trying to optimize in gradient descent
    ZonePassMap<ZoneEnum> current_best_passes_;

    // The optimizer we're using to find passes
    GradientDescentOptimizer<NUM_PARAMS_TO_OPTIMIZE> optimizer_;

    // Pitch division
    std::shared_ptr<const FieldPitchDivision<ZoneEnum>> pitch_division_;

    // Passing configuration
    TbotsProto::PassingConfig passing_config_;

    // A random number generator for use across the class
    std::mt19937 random_num_gen_;

    // A constant used in the calculation of a pass's speed
    // Explanation in the getPassSpeed() method docs
    double pass_speed_calc_constant;
};
template <class ZoneEnum>
PassGenerator<ZoneEnum>::PassGenerator(
    std::shared_ptr<const FieldPitchDivision<ZoneEnum>> pitch_division,
    TbotsProto::PassingConfig passing_config)
    : optimizer_(optimizer_param_weights),
      pitch_division_(pitch_division),
      passing_config_(passing_config),
      random_num_gen_(PASS_GENERATOR_SEED)
{
    // calculating the constant value used in determining pass speed
    double sq_friction_trans_factor = pow(FRICTION_TRANSITION_FACTOR, 2);
    pass_speed_calc_constant =
        sq_friction_trans_factor -
        ((BALL_ROLLING_FRICTION_DECELERATION_METERS_PER_SECOND_SQUARED *
          sq_friction_trans_factor) /
         BALL_SLIDING_FRICTION_DECELERATION_METERS_PER_SECOND_SQUARED) +
        (BALL_ROLLING_FRICTION_DECELERATION_METERS_PER_SECOND_SQUARED /
         BALL_SLIDING_FRICTION_DECELERATION_METERS_PER_SECOND_SQUARED);
}

template <class ZoneEnum>
PassEvaluation<ZoneEnum> PassGenerator<ZoneEnum>::generatePassEvaluation(
    const World& world)
{
    // Generate sample passes for cost visualization
    if (passing_config_.cost_vis_config().generate_sample_passes())
    {
        samplePassesForVisualization(world, passing_config_);
    }

    auto generated_passes = samplePasses(world);
    if (current_best_passes_.empty())
    {
        current_best_passes_ = generated_passes;
    }
    auto optimized_passes = optimizePasses(world, generated_passes);

    updatePasses(world, optimized_passes);


    std::vector<PassWithRating> passes;
    passes.reserve(current_best_passes_.size());

    for (auto zone_and_pass : current_best_passes_)
    {
        passes.push_back(zone_and_pass.second);
    }

    LOG(VISUALIZE) << *createPassVisualization(passes);

    return PassEvaluation<ZoneEnum>(pitch_division_, current_best_passes_,
                                    passing_config_, world.getMostRecentTimestamp());
}

template <class ZoneEnum>
ZonePassMap<ZoneEnum> PassGenerator<ZoneEnum>::samplePasses(const World& world)
{
    std::uniform_real_distribution speed_distribution(
        passing_config_.min_pass_speed_m_per_s(),
        passing_config_.max_pass_speed_m_per_s());

    ZonePassMap<ZoneEnum> passes;

    // Randomly sample a pass in each zone
    for (ZoneEnum zone_id : pitch_division_->getAllZoneIds())
    {
        auto zone = pitch_division_->getZone(zone_id);

        std::uniform_real_distribution x_distribution(zone.xMin(), zone.xMax());
        std::uniform_real_distribution y_distribution(zone.yMin(), zone.yMax());

        auto pass_destination =
            Point(x_distribution(random_num_gen_), y_distribution(random_num_gen_));
        auto pass_speed_m_per_s = getPassSpeed(world.ball().position(), pass_destination);

        auto pass = Pass(world.ball().position(), pass_destination, pass_speed_m_per_s);

        auto rating =
            ratePass(world, pass, pitch_division_->getZone(zone_id), passing_config_);

        passes.emplace(zone_id, PassWithRating{pass, rating});
    }

    return passes;
}

template <class ZoneEnum>
double PassGenerator<ZoneEnum>::getPassSpeed(const Point& ball_position,
                                             const Point& pass_destination)
{
    // We have
    //      - destination speed (m/s)       -> vf
    //      - rolling deceleration (m/s^2)  -> r
    //      - sliding deceleration (m/s^2)  -> s
    //      - length of pass (m)            -> D
    //      - friction transition factor    -> c
    //          - this dictates at what speed friction goes from sliding to rolling
    // We want to find
    //      - initial starting speed (m/s) -> x
    //
    // Ball decelerates with sliding from x -> cx, then with rolling from cx -> vf
    // Slide Distance (m) -> d1
    // Roll Distance (m) -> d2
    // d2 = D - d1
    //
    // (cx)^2 = x^2 + 2sd1
    // d1 = (c^2 - 1)x^2 / 2s
    // d2 = D + (1 - c^2)x^2 / 2s
    //
    // vf^2 = (cx)^2 + 2rd2 = (cx)^2 + 2r(D + (1 - c^2)x^2 / 2s)
    // Simplify to get
    // x = sqrt((vf^2 - 2rD) / (c^2 - rc^2/s + b/s))
    double dest_speed_m_per_s     = passing_config_.max_receive_speed();
    Vector pass_distance          = Vector(pass_destination.x() - ball_position.x(),
                                  pass_destination.y() - ball_position.y());
    double pass_distance_length_m = pass_distance.length();
    double squared_pass_speed =
        (pow(dest_speed_m_per_s, 2) -
         2 * -BALL_ROLLING_FRICTION_DECELERATION_METERS_PER_SECOND_SQUARED *
             pass_distance_length_m) /
        pass_speed_calc_constant;
    double pass_speed_m_per_s = sqrt(squared_pass_speed);

    double min_pass_speed = passing_config_.min_pass_speed_m_per_s();
    double max_pass_speed = passing_config_.max_pass_speed_m_per_s();
    double clamped_pass_speed_m_per_s =
        std::max(min_pass_speed, std::min(max_pass_speed, pass_speed_m_per_s));
    return clamped_pass_speed_m_per_s;
}

template <class ZoneEnum>
ZonePassMap<ZoneEnum> PassGenerator<ZoneEnum>::optimizePasses(
    const World& world, const ZonePassMap<ZoneEnum>& generated_passes)
{
    // Run gradient descent to optimize the passes to for the requested number
    // of iterations
    ZonePassMap<ZoneEnum> optimized_passes;

    for (ZoneEnum zone_id : pitch_division_->getAllZoneIds())
    {
        // The objective function we minimize in gradient descent to improve each pass
        // that we're optimizing
        const auto objective_function =
            [this, &world,
             zone_id](const std::array<double, NUM_PARAMS_TO_OPTIMIZE>& pass_array) {
                // get the new appropriate speed using the new destination
                double pass_speed_m_per_s = getPassSpeed(
                    world.ball().position(), Point(pass_array[0], pass_array[1]));
                return ratePass(world,
                                Pass::fromPassArray(world.ball().position(), pass_array,
                                                    pass_speed_m_per_s),
                                pitch_division_->getZone(zone_id), passing_config_);
            };

        auto pass_array = optimizer_.maximize(
            objective_function, generated_passes.at(zone_id).pass.toPassArray(),
            passing_config_.number_of_gradient_descent_steps_per_iter());

        // get the new appropriate speed using the new destination
        double pass_speed_m_per_s =
            getPassSpeed(world.ball().position(), Point(pass_array[0], pass_array[1]));
        auto new_pass =
            Pass::fromPassArray(world.ball().position(), pass_array, pass_speed_m_per_s);
        auto score =
            ratePass(world, new_pass, pitch_division_->getZone(zone_id), passing_config_);

        optimized_passes.emplace(zone_id, PassWithRating{new_pass, score});
    }

    return optimized_passes;
}



template <class ZoneEnum>
void PassGenerator<ZoneEnum>::updatePasses(const World& world,
                                           const ZonePassMap<ZoneEnum>& optimized_passes)
{
    for (ZoneEnum zone_id : pitch_division_->getAllZoneIds())
    {
        auto pass_array = current_best_passes_.at(zone_id).pass.toPassArray();
        double pass_speed_m_per_s =
            getPassSpeed(world.ball().position(), Point(pass_array[0], pass_array[1]));
        // update the passer point of the current best pass
        current_best_passes_.at(zone_id).pass =
            Pass::fromPassArray(world.ball().position(), pass_array, pass_speed_m_per_s);

        if (ratePass(world, current_best_passes_.at(zone_id).pass,
                     pitch_division_->getZone(zone_id),
                     passing_config_) < optimized_passes.at(zone_id).rating)
        {
            current_best_passes_.at(zone_id) = optimized_passes.at(zone_id);
        }
    }
}
