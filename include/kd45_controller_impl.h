/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2020, Bielefeld University
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Bielefeld University nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Luca Lach
*/

#ifndef KD45_CONTROLLER_KD45_CONTROLLER_IMPL_H
#define KD45_CONTROLLER_KD45_CONTROLLER_IMPL_H

#include <numeric>
#include <chrono>
#include <math.h>
#include "kd45_controller.h"
#include <type_traits>
#include <cmath>

namespace kd45_controller {
template <class TactileSensors>
inline bool KD45TrajectoryController<TactileSensors>::init(hardware_interface::PositionJointInterface* hw,
                                                           ros::NodeHandle& root_nh, ros::NodeHandle& controller_nh) {
    ROS_INFO_NAMED(name_, "Initializing KD45TrajectoryController.");
    forces_= std::make_shared<std::vector<float>>(2, 0.0);
    sensors_ = std::make_shared<TactileSensors>(root_nh, forces_);

	bool ret = JointTrajectoryController::init(hw, root_nh, controller_nh);
	return ret;
}

template <class TactileSensors>
inline void KD45TrajectoryController<TactileSensors>::goalCB(GoalHandle gh) {
	ROS_DEBUG_STREAM_NAMED(name_, "Received new action goal");

	// Precondition: Running controller
	if (!this->isRunning()) {
		ROS_ERROR_NAMED(name_, "Can't accept new action goals. Controller is not running.");
		control_msgs::FollowJointTrajectoryResult result;
		result.error_code =
		    control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;  // TODO: Add better error status to msg?
		gh.setRejected(result);
		return;
	}

	// If partial joints goals are not allowed, goal should specify all controller joints
	if (!allow_partial_joints_goal_) {
		if (gh.getGoal()->trajectory.joint_names.size() != joint_names_.size()) {
			ROS_ERROR_NAMED(name_, "Joints on incoming goal don't match the controller joints.");
			control_msgs::FollowJointTrajectoryResult result;
			result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_JOINTS;
			gh.setRejected(result);
			return;
		}
	}

	// Goal should specify valid controller joints (they can be ordered differently). Reject if this is not the case
	using joint_trajectory_controller::internal::mapping;
	std::vector<unsigned int> mapping_vector = mapping(gh.getGoal()->trajectory.joint_names, joint_names_);

	if (mapping_vector.empty()) {
		ROS_ERROR_NAMED(name_, "Joints on incoming goal don't match the controller joints.");
		control_msgs::FollowJointTrajectoryResult result;
		result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_JOINTS;
		gh.setRejected(result);
		return;
	}

	// Try to update new trajectory
	RealtimeGoalHandlePtr rt_goal(new RealtimeGoalHandle(gh));
	std::string error_string = "";  // todo upstream passed this one to updateTrajctoryCommand
	const bool update_ok = updateTrajectoryCommand(
	    joint_trajectory_controller::internal::share_member(gh.getGoal(), gh.getGoal()->trajectory), rt_goal);
	rt_goal->preallocated_feedback_->joint_names = joint_names_;

	if (update_ok) {
		// Accept new goal
		preemptActiveGoal();
		gh.setAccepted();
		rt_active_goal_ = rt_goal;

		// Setup goal status checking timer
		goal_handle_timer_ =
		    controller_nh_.createTimer(action_monitor_period_, &RealtimeGoalHandle::runNonRealtime, rt_goal);
		goal_handle_timer_.start();
	} else {
		// Reject invalid goal
		control_msgs::FollowJointTrajectoryResult result;
		result.error_code = control_msgs::FollowJointTrajectoryResult::INVALID_GOAL;
		result.error_string = error_string;
		gh.setRejected(result);
	}
}

template <class TactileSensors>
inline void KD45TrajectoryController<TactileSensors>::update(const ros::Time& time, const ros::Duration& period) {
    ROS_DEBUG_STREAM_NAMED(name_ + ".forces", "Forces: [" << (*forces_)[0] << ", " << (*forces_)[1] << "]");

	realtime_busy_ = true;
	// Get currently followed trajectory
	TrajectoryPtr curr_traj_ptr;
	curr_trajectory_box_.get(curr_traj_ptr);
	Trajectory& curr_traj = *curr_traj_ptr;

	// Update time data
	TimeData time_data;
	time_data.time = time;  // Cache current time
	time_data.period = period;  // Cache current control period
	time_data.uptime = time_data_.readFromRT()->uptime + period;  // Update controller uptime
	time_data_.writeFromNonRT(time_data);  // TODO: Grrr, we need a lock-free data structure here!

	// NOTE: It is very important to execute the two above code blocks in the specified sequence: first get current
	// trajectory, then update time data. Hopefully the following paragraph sheds a bit of light on the rationale.
	// The non-rt thread responsible for processing new commands enqueues trajectories that can start at the _next_
	// control cycle (eg. zero start time) or later (eg. when we explicitly request a start time in the future).
	// If we reverse the order of the two blocks above, and update the time data first; it's possible that by the time we
	// fetch the currently followed trajectory, it has been updated by the non-rt thread with something that starts in
	// the
	// next control cycle, leaving the current cycle without a valid trajectory.

	// Update current state and state error
	for (unsigned int i = 0; i < joints_.size(); ++i) {
		current_state_.position[i] = joints_[i].getPosition();
		current_state_.velocity[i] = joints_[i].getVelocity();
		// There's no acceleration data available in a joint handle

		typename TrajectoryPerJoint::const_iterator segment_it =
		    sample(curr_traj[i], time_data.uptime.toSec(), desired_joint_state_);
		if (curr_traj[i].end() == segment_it) {
			// Non-realtime safe, but should never happen under normal operation
			ROS_ERROR_NAMED(
			    name_, "Unexpected error: No trajectory defined at current time. Please contact the package maintainer.");
			return;
		}
		desired_state_.position[i] = desired_joint_state_.position[0];
		desired_state_.velocity[i] = desired_joint_state_.velocity[0];
		desired_state_.acceleration[i] = desired_joint_state_.acceleration[0];
		;

		state_joint_error_.position[0] =
		    angles::shortest_angular_distance(current_state_.position[i], desired_joint_state_.position[0]);
		state_joint_error_.velocity[0] = desired_joint_state_.velocity[0] - current_state_.velocity[i];
		state_joint_error_.acceleration[0] = 0.0;

		state_error_.position[i] =
		    angles::shortest_angular_distance(current_state_.position[i], desired_joint_state_.position[0]);
		state_error_.velocity[i] = desired_joint_state_.velocity[0] - current_state_.velocity[i];
		state_error_.acceleration[i] = 0.0;

		// Check tolerances
		const RealtimeGoalHandlePtr rt_segment_goal = segment_it->getGoalHandle();
		if (rt_segment_goal && rt_segment_goal == rt_active_goal_) {
			// Check tolerances
			if (time_data.uptime.toSec() < segment_it->endTime()) {
				// Currently executing a segment: check path tolerances
				const joint_trajectory_controller::SegmentTolerancesPerJoint<Scalar>& joint_tolerances =
				    segment_it->getTolerances();
				if (!checkStateTolerancePerJoint(state_joint_error_, joint_tolerances.state_tolerance)) {
					if (verbose_) {
						ROS_ERROR_STREAM_NAMED(name_, "Path tolerances failed for joint: " << joint_names_[i]);
						checkStateTolerancePerJoint(state_joint_error_, joint_tolerances.state_tolerance, true);
					}

					if (rt_segment_goal && rt_segment_goal->preallocated_result_) {
						rt_segment_goal->preallocated_result_->error_code =
						    control_msgs::FollowJointTrajectoryResult::PATH_TOLERANCE_VIOLATED;
						rt_segment_goal->setAborted(rt_segment_goal->preallocated_result_);
						rt_active_goal_.reset();
						successful_joint_traj_.reset();
					} else {
						ROS_ERROR_STREAM("rt_segment_goal->preallocated_result_ NULL Pointer");
					}
				}
			} else if (segment_it == --curr_traj[i].end()) {
				if (verbose_)
					ROS_DEBUG_STREAM_THROTTLE_NAMED(1, name_, "Finished executing last segment, checking goal tolerances");

				// Controller uptimegit st
				const ros::Time uptime = time_data_.readFromRT()->uptime;

				// Checks that we have ended inside the goal tolerances
				const joint_trajectory_controller::SegmentTolerancesPerJoint<Scalar>& tolerances = segment_it->getTolerances();
				const bool inside_goal_tolerances =
				    checkStateTolerancePerJoint(state_joint_error_, tolerances.goal_state_tolerance);

				if (inside_goal_tolerances) {
					successful_joint_traj_[i] = 1;
				} else if (uptime.toSec() < segment_it->endTime() + tolerances.goal_time_tolerance) {
					// Still have some time left to meet the goal state tolerances
				} else {
					if (verbose_) {
						ROS_ERROR_STREAM_NAMED(name_, "Goal tolerances failed for joint: " << joint_names_[i]);
						// Check the tolerances one more time to output the errors that occurs
						checkStateTolerancePerJoint(state_joint_error_, tolerances.goal_state_tolerance, true);
					}

					if (rt_segment_goal) {
						rt_segment_goal->preallocated_result_->error_code =
						    control_msgs::FollowJointTrajectoryResult::GOAL_TOLERANCE_VIOLATED;
						rt_segment_goal->setAborted(rt_segment_goal->preallocated_result_);
					} else {
						ROS_ERROR_STREAM("rt_segment_goal->preallocated_result_ NULL Pointer");
					}
					rt_active_goal_.reset();
					successful_joint_traj_.reset();
				}
			}
		}
	}

	// If there is an active goal and all segments finished successfully then set goal as succeeded
	RealtimeGoalHandlePtr current_active_goal(rt_active_goal_);
	if (current_active_goal && current_active_goal->preallocated_result_ &&
	    successful_joint_traj_.count() == joints_.size()) {
		current_active_goal->preallocated_result_->error_code = control_msgs::FollowJointTrajectoryResult::SUCCESSFUL;
		current_active_goal->setSucceeded(current_active_goal->preallocated_result_);
		rt_active_goal_.reset();
		successful_joint_traj_.reset();
	}

	// Hardware interface adapter: Generate and send commands
	hw_iface_adapter_.updateCommand(time_data.uptime, time_data.period, desired_state_, state_error_);

	// Set action feedback
	if (rt_active_goal_ && rt_active_goal_->preallocated_feedback_) {
		rt_active_goal_->preallocated_feedback_->header.stamp = time_data_.readFromRT()->time;
		rt_active_goal_->preallocated_feedback_->desired.positions = desired_state_.position;
		rt_active_goal_->preallocated_feedback_->desired.velocities = desired_state_.velocity;
		rt_active_goal_->preallocated_feedback_->desired.accelerations = desired_state_.acceleration;
		rt_active_goal_->preallocated_feedback_->actual.positions = current_state_.position;
		rt_active_goal_->preallocated_feedback_->actual.velocities = current_state_.velocity;
		rt_active_goal_->preallocated_feedback_->error.positions = state_error_.position;
		rt_active_goal_->preallocated_feedback_->error.velocities = state_error_.velocity;
		rt_active_goal_->setFeedback(rt_active_goal_->preallocated_feedback_);
	}

	// Publish state
	publishState(time_data.uptime);
	realtime_busy_ = false;
}
}

#endif  // KD45_CONTROLLER_KD45_CONTROLLER_IMPL_H
