#pragma once

#include "FloatingBaseModel.h"
#include "Gait_contact.h"
#include "Utilities/SpiralIterator.hpp"
#include "cppTypes.h"
#include "grid_map_ros/grid_map_ros.hpp"
#include <ControlFSMData.h>
#include <FootSwingTrajectory.h>
#include <SparseCMPC/SparseCMPC.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Path.h>
#include <ros/ros.h>
#include <visualization_msgs/Marker.h>

#include <cstdio>

using Eigen::Array4f;
using Eigen::Array4i;

class CMPCLocomotion_Cv
{
public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  CMPCLocomotion_Cv(float _dt, int _iterations_between_mpc, ControlFSMData<float>* data);
  void initialize();

  void run(ControlFSMData<float>& data, const grid_map::GridMap& height_map, const grid_map::GridMap& height_map_raw);
  void original(ControlFSMData<float>& data);
  void myVersion(ControlFSMData<float>& data, const grid_map::GridMap& height_map, const grid_map::GridMap& height_map_raw);
  bool currently_jumping = false;

  Vec3<float> pBody_des;
  Vec3<float> vBody_des;
  Vec3<float> aBody_des;

  Vec3<float> pBody_RPY_des;
  Vec3<float> vBody_Ori_des;

  Vec3<float> pFoot_des[4];
  Vec3<float> vFoot_des[4];
  Vec3<float> aFoot_des[4];

  Vec3<float> Fr_des[4];

  Vec4<float> contact_state;

private:
  void _SetupCommand(ControlFSMData<float>& data);
  void _recompute_timing(int iterations_per_mpc);
  void _updateMPCIfNeeded(int* mpcTable, ControlFSMData<float>& data, bool omniMode);
  void _solveDenseMPC(int* mpcTable, ControlFSMData<float>& data);
  void _solveSparseMPC(int* mpcTable, ControlFSMData<float>& data);
  void _initSparseMPC();
  void _updateModel(const StateEstimate<float>& state_est, const LegControllerData<float>* leg_data);

  void _updateFoothold(Vec3<float>& pf,
                       const Vec3<float>& body_pos,
                       const grid_map::GridMap& height_map,
                       const grid_map::GridMap& height_map_raw,
                       int leg);

  void _idxMapChecking(Vec3<float>& Pf,
                       int x_idx,
                       int y_idx,
                       int& x_idx_selected,
                       int& y_idx_selected,
                       const grid_map::GridMap& height_map,
                       const grid_map::GridMap& height_map_raw,
                       int leg);

  // Parameters
  ControlFSMData<float>* _data;
  be2r_cmpc_unitree::ros_dynamic_paramsConfig* _parameters = nullptr;

  // Gait
  int _gait_period;
  int _gait_period_long;
  OffsetDurationGaitContact trotting, trot_long, standing, walking;
  int current_gait;
  int _gait_des;

  Vec4<float> swingTimes;
  FootSwingTrajectory<float> footSwingTrajectories[4];
  float swingTimeRemaining[4];
  float _swing_trajectory_hight = 0.09;
  float _body_height = 0.29;

  float _yaw_turn_rate;
  float _yaw_des = 0;
  float _roll_des;
  float _pitch_des;
  float _pitch_cmd = 0;

  Vec3<float> world_position_desired;
  float _x_vel_des = 0.;
  float _y_vel_des = 0.;

  Vec3<float> rpy_int;
  Vec3<float> rpy_comp;

  FloatingBaseModel<float> _model;
  FBModelState<float> _state;
  DMat<float> _A;
  DMat<float> _Ainv;
  DVec<float> _grav;
  DVec<float> _coriolis;

  int iterationCounter = 0;
  int iterationsBetweenMPC;

  int horizonLength;
  int default_iterations_between_mpc;
  float dt;
  float dtMPC;
  Vec3<float> f_ff[4];

  Mat3<float> Kp, Kd, Kp_stance, Kd_stance;
  bool firstRun = true;
  bool firstSwing[4];
  float stand_traj[6];
  int gaitNumber;

  float x_comp_integral = 0;
  Vec3<float> pFoot[4];
  float trajAll[12 * 36];

  ros::NodeHandle _nh;

  vectorAligned<Vec12<double>> _sparseTrajectory;
  SparseCMPC _sparseCMPC;
};

Eigen::Array2i checkBoundariess(const grid_map::GridMap& map, int col, int row);