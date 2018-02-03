/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2017, Bielefeld + Hamburg University
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

/* Authors: Robert Haschke, Michael Goerner */

#include <moveit/task_constructor/stages/generate_grasp_pose.h>
#include <moveit/task_constructor/storage.h>
#include <moveit/task_constructor/marker_tools.h>
#include <rviz_marker_tools/marker_creation.h>

#include <moveit/planning_scene/planning_scene.h>

#include <Eigen/Geometry>
#include <eigen_conversions/eigen_msg.h>

namespace moveit { namespace task_constructor { namespace stages {

GenerateGraspPose::GenerateGraspPose(std::string name)
: Generator(name)
{
	auto& p = properties();
	p.declare<std::string>("eef", "name of end-effector group");
	p.declare<std::string>("eef_named_pose");
	p.declare<std::string>("object");
	p.declare<geometry_msgs::TransformStamped>("tool_to_grasp_tf", geometry_msgs::TransformStamped(), "transform from robot tool frame to grasp frame");
	p.declare<double>("angle_delta", 0.1, "angular steps (rad)");
}

void GenerateGraspPose::init(const planning_scene::PlanningSceneConstPtr &scene)
{
	Generator::init(scene);
	scene_ = scene->diff();
}

void GenerateGraspPose::setEndEffector(const std::string &eef){
	setProperty("eef", eef);
}

void GenerateGraspPose::setGripperGraspPose(const std::string &pose_name){
	setProperty("eef_named_pose", pose_name);
}

void GenerateGraspPose::setObject(const std::string &object){
	setProperty("object", object);
}

void GenerateGraspPose::setToolToGraspTF(const geometry_msgs::TransformStamped &transform){
	setProperty("tool_to_grasp_tf", transform);
}

void GenerateGraspPose::setToolToGraspTF(const Eigen::Affine3d &transform, const std::string &link){
	geometry_msgs::TransformStamped stamped;
	stamped.header.frame_id = link;
	stamped.child_frame_id = "grasp_frame";
	tf::transformEigenToMsg(transform, stamped.transform);
	setToolToGraspTF(stamped);
}

void GenerateGraspPose::setAngleDelta(double delta){
	setProperty("angle_delta", delta);
}

bool GenerateGraspPose::canCompute() const{
	return current_angle_ < 2*M_PI && current_angle_ > -2*M_PI;
}

bool GenerateGraspPose::compute(){
	const auto& props = properties();
	const std::string& eef = props.get<std::string>("eef");

	assert(scene_->getRobotModel()->hasEndEffector(eef) && "The specified end effector is not defined in the srdf");
	const moveit::core::JointModelGroup* jmg = scene_->getRobotModel()->getEndEffector(eef);

	robot_state::RobotState &robot_state = scene_->getCurrentStateNonConst();
	const std::string& eef_named_pose = props.get<std::string>("eef_named_pose");
	if(!eef_named_pose.empty()){
		robot_state.setToDefaultValues(jmg , eef_named_pose);
	}

	geometry_msgs::TransformStamped tool2grasp_msg = props.get<geometry_msgs::TransformStamped>("tool_to_grasp_tf");
	const std::string &link_name = jmg ->getEndEffectorParentGroup().second;
	if (tool2grasp_msg.header.frame_id.empty()) // if no frame_id is given
		tool2grasp_msg.header.frame_id = link_name; // interpret the transform w.r.t. eef link frame
	Eigen::Affine3d to_grasp;
	Eigen::Affine3d grasp2tool, grasp2link;
	tf::transformMsgToEigen(tool2grasp_msg.transform, to_grasp);
	grasp2tool = to_grasp.inverse();

	if (tool2grasp_msg.header.frame_id != link_name) {
		// convert to_grasp into transform w.r.t. link (instead of tool frame_id)
		const Eigen::Affine3d link_pose = scene_->getFrameTransform(link_name);
		if(link_pose.matrix().cwiseEqual(Eigen::Affine3d::Identity().matrix()).all())
			throw std::runtime_error("requested link does not exist or could not be retrieved");
		const Eigen::Affine3d tool_pose = scene_->getFrameTransform(tool2grasp_msg.header.frame_id);
		if(tool_pose.matrix().cwiseEqual(Eigen::Affine3d::Identity().matrix()).all())
			throw std::runtime_error("requested frame does not exist or could not be retrieved");
		to_grasp = link_pose.inverse() * tool_pose * to_grasp;
		grasp2link = to_grasp.inverse();
	} else
		grasp2link = grasp2tool;

	const Eigen::Affine3d object_pose = scene_->getFrameTransform(props.get<std::string>("object"));
	if(object_pose.matrix().cwiseEqual(Eigen::Affine3d::Identity().matrix()).all())
		throw std::runtime_error("requested object does not exist or could not be retrieved");

	while( canCompute() ){
		// rotate object pose about z-axis
		Eigen::Affine3d grasp_pose = object_pose * Eigen::AngleAxisd(current_angle_, Eigen::Vector3d::UnitZ());
		Eigen::Affine3d link_pose = grasp_pose * grasp2link;
		current_angle_ += props.get<double>("angle_delta");

		InterfaceState state(scene_);
		geometry_msgs::PoseStamped goal_pose_msg;
		goal_pose_msg.header.frame_id = link_name;
		tf::poseEigenToMsg(link_pose, goal_pose_msg.pose);
		state.properties().set("target_pose", goal_pose_msg);

		SubTrajectory trajectory;
		trajectory.setCost(0.0);
		trajectory.setName(std::to_string(current_angle_));

		// add an arrow marker
		visualization_msgs::Marker m;
		m.header.frame_id = scene_->getPlanningFrame();
		m.ns = "grasp pose";
		rviz_marker_tools::setColor(m.color, rviz_marker_tools::LIME_GREEN);
		double scale = 0.1;
		rviz_marker_tools::makeArrow(m, scale);
		tf::poseEigenToMsg(grasp_pose * grasp2tool *
		                   // arrow should point along z (instead of x)
		                   Eigen::AngleAxisd(-M_PI / 2.0, Eigen::Vector3d::UnitY()) *
		                   // arrow tip at goal_pose
		                   Eigen::Translation3d(-scale, 0, 0), m.pose);
		trajectory.markers().push_back(m);

		// add end-effector marker
		robot_state.updateStateWithLinkAt(link_name, link_pose);
		auto appender = [&trajectory](visualization_msgs::Marker& marker, const std::string& name) {
			marker.ns = "grasp eef";
			marker.color.a *= 0.5;
			trajectory.markers().push_back(marker);
		};
		generateVisualMarkers(robot_state, appender, jmg->getLinkModelNames());

		spawn(std::move(state), std::move(trajectory));
		return true;
	}

	return false;
}

} } }