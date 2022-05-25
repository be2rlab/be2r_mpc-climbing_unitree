#include <Utilities/Timer.h>
#include <Utilities/Utilities_print.h>
#include <iostream>

#include "CMPC_Locomotion.h"
#include "GraphSearch.h"
#include "convexMPC_interface.h"

#include "Gait.h"

//оригинальные параметры MPC+WBC
// #define GAIT_PERIOD 14
#define HORIZON 14

// #define GAIT_PERIOD 16
#define GAIT_PERIOD 25
// #define GAIT_PERIOD 34 //1000 Hz

//лучшие параметры для только MPC
// #define GAIT_PERIOD 18
// #define HORIZON 5

#define STEP_HEIGHT 0.06
#define BODY_HEIGHT 0.24

// #define SHOW_MPC_SOLVE_TIME

using namespace std;

////////////////////
// Controller
////////////////////

CMPCLocomotion::CMPCLocomotion(float _dt, int _iterations_between_mpc, be2r_cmpc_unitree::ros_dynamic_paramsConfig* parameters) : iterationsBetweenMPC(_iterations_between_mpc),
                                                                                                                                  _parameters(parameters),
                                                                                                                                  _gait_period(_parameters->gait_period),
                                                                                                                                  horizonLength(HORIZON),
                                                                                                                                  dt(_dt),
                                                                                                                                  trotting(_gait_period, Vec4<int>(0, _gait_period / 2.0, _gait_period / 2.0, 0), Vec4<int>(_gait_period / 2.0, _gait_period / 2.0, _gait_period / 2.0, _gait_period / 2.0), "Trotting"),
                                                                                                                                  trot_contact(GAIT_PERIOD, Vec4<int>(0, GAIT_PERIOD / 2.0, GAIT_PERIOD / 2.0, 0), Vec4<int>(GAIT_PERIOD * 0.25, GAIT_PERIOD * 0.25, GAIT_PERIOD * 0.25, GAIT_PERIOD * 0.25), "Trot contact"),
                                                                                                                                  standing(GAIT_PERIOD, Vec4<int>(0, 0, 0, 0), Vec4<int>(GAIT_PERIOD, GAIT_PERIOD, GAIT_PERIOD, GAIT_PERIOD), "Standing"),
                                                                                                                                  two_leg_balance(_gait_period, Vec4<int>(0, 0, 0, 0), Vec4<int>(_gait_period, 0, _gait_period, 0), "Two legs balance")

{
  dtMPC = dt * iterationsBetweenMPC;
  default_iterations_between_mpc = iterationsBetweenMPC;
  printf("[Convex MPC] dt: %.3f iterations: %d, dtMPC: %.3f\n", dt, iterationsBetweenMPC, dtMPC);
  setup_problem(dtMPC, horizonLength, 0.4, 120); // original
  rpy_comp[0] = 0;
  rpy_comp[1] = 0;
  rpy_comp[2] = 0;
  rpy_int[0] = 0;
  rpy_int[1] = 0;
  rpy_int[2] = 0;

  for (int i = 0; i < 4; i++)
  {
    firstSwing[i] = true;
  }

  initSparseMPC();

  pBody_des.setZero();
  vBody_des.setZero();
  aBody_des.setZero();
}

void CMPCLocomotion::initialize()
{
  for (int i = 0; i < 4; i++)
  {
    firstSwing[i] = true;
  }

  firstRun = true;
}

void CMPCLocomotion::recompute_timing(int iterations_per_mpc)
{
  iterationsBetweenMPC = iterations_per_mpc;
  dtMPC = dt * iterations_per_mpc;
}

void CMPCLocomotion::_SetupCommand(ControlFSMData<float>& data)
{
  _body_height = _parameters->body_height;

  float x_vel_cmd, y_vel_cmd;
  float filter(0.1);

  _yaw_turn_rate = data._desiredStateCommand->rightAnalogStick[0];
  x_vel_cmd = data._desiredStateCommand->leftAnalogStick[1];
  y_vel_cmd = data._desiredStateCommand->leftAnalogStick[0];

  _x_vel_des = _x_vel_des * (1 - filter) + x_vel_cmd * filter;
  _y_vel_des = _y_vel_des * (1 - filter) + y_vel_cmd * filter;

  _yaw_des = data._stateEstimator->getResult().rpy[2] + dt * _yaw_turn_rate;
  // _yaw_des += dt * _yaw_turn_rate;
  _roll_des = 0.;
  _pitch_des = 0.;

  // Update PD coefs
  // for sim
  Kp = Vec3<float>(_parameters->Kp_cartesian_0, _parameters->Kp_cartesian_1, _parameters->Kp_cartesian_2).asDiagonal();
  Kp_stance = Kp;

  Kd = Vec3<float>(_parameters->Kd_cartesian_0, _parameters->Kd_cartesian_1, _parameters->Kd_cartesian_2).asDiagonal();
  Kd_stance = Kd;

  // for real
  //  Kp << 150, 0, 0, 0, 150, 0, 0, 0, 150;
  //  Kp_stance = 0 * Kp;

  //  Kd << 3, 0, 0, 0, 3, 0, 0, 0, 3;
  //  Kd_stance = Kd;
}

template <>
void CMPCLocomotion::run(ControlFSMData<float>& data)
{
  bool omniMode = false;

  // Command Setup
  _SetupCommand(data);

  gaitNumber = data.userParameters->cmpc_gait;

  auto& seResult = data._stateEstimator->getResult();

  // Check if transition to standing
  if (((gaitNumber == 4) && current_gait != 4) || firstRun)
  {
    stand_traj[0] = seResult.position[0];
    stand_traj[1] = seResult.position[1];
    stand_traj[2] = 0.21;
    stand_traj[3] = 0;
    stand_traj[4] = 0;
    stand_traj[5] = seResult.rpy[2];
    world_position_desired[0] = stand_traj[0];
    world_position_desired[1] = stand_traj[1];
  }

  // pick gait
  Gait_contact* gait = &trotting;
  current_gait = gaitNumber;

  // cout << "vx: " << _x_vel_des << " vy: " << _y_vel_des << " yaw: " << _yaw_turn_rate << endl;

  // if ((abs(_x_vel_des) > 0.005) || (abs(_y_vel_des) > 0.005) || (abs(_yaw_turn_rate) > 0.005))
  // {
  //   current_gait = gaitNumber;
  // }
  // else
  // {
  //   current_gait = 4;
  // }

  if (current_gait == 13)
  {
    // gait = &two_leg_balance;
    gait = &trot_contact;
  }
  else if (current_gait == 4)
  {
    gait = &standing;
  }
  else if (current_gait == 9)
  {
    gait = &trotting;
  }

  gait->restoreDefaults();
  gait->setIterations(iterationsBetweenMPC, iterationCounter);
  // gait->earlyContactHandle(seResult.contactSensor, iterationsBetweenMPC, iterationCounter);
  gait->earlyContactHandle(data._stateEstimator->getContactSensorData(), iterationsBetweenMPC, iterationCounter);
  //  std::cout << "iterationCounter " << iterationCounter << std::endl;

  recompute_timing(default_iterations_between_mpc);

  // integrate position setpoint
  Vec3<float> v_des_robot(_x_vel_des, _y_vel_des, 0);
  Vec3<float> v_des_world = omniMode ? v_des_robot : seResult.rBody.transpose() * v_des_robot;
  Vec3<float> v_robot = seResult.vWorld;

  // Integral-esque pitche and roll compensation
  if (fabs(v_robot[0]) > .2) // avoid dividing by zero
  {
    rpy_int[1] += dt * (_pitch_des - seResult.rpy[1]) / v_robot[0];
  }
  if (fabs(v_robot[1]) > 0.1)
  {
    rpy_int[0] += dt * (_roll_des - seResult.rpy[0]) / v_robot[1];
  }

  rpy_int[0] = fminf(fmaxf(rpy_int[0], -.25), .25);
  rpy_int[1] = fminf(fmaxf(rpy_int[1], -.25), .25);
  rpy_comp[1] = v_robot[0] * rpy_int[1];
  rpy_comp[0] = v_robot[1] * rpy_int[0] * (gaitNumber != 8); // turn off for pronking

  for (int i = 0; i < 4; i++)
  {
    pFoot[i] = seResult.position + seResult.rBody.transpose() * (data._quadruped->getHipLocation(i) + data._legController->datas[i].p);
  }

  world_position_desired += dt * Vec3<float>(v_des_world[0], v_des_world[1], 0);

  // some first time initialization
  if (firstRun)
  {
    world_position_desired[0] = seResult.position[0];
    world_position_desired[1] = seResult.position[1];
    world_position_desired[2] = seResult.rpy[2];

    for (int i = 0; i < 4; i++)
    {
      footSwingTrajectories[i].setHeight(_parameters->Swing_traj_height);

      footSwingTrajectories[i].setInitialPosition(pFoot[i]);
      data.debug->all_legs_info.leg[i].swing_ps.x = pFoot[i](0);
      data.debug->all_legs_info.leg[i].swing_ps.y = pFoot[i](1);
      data.debug->all_legs_info.leg[i].swing_ps.z = pFoot[i](2);

      footSwingTrajectories[i].setFinalPosition(pFoot[i]);
      data.debug->all_legs_info.leg[i].swing_pf.x = pFoot[i](0);
      data.debug->all_legs_info.leg[i].swing_pf.y = pFoot[i](1);
      data.debug->all_legs_info.leg[i].swing_pf.z = pFoot[i](2);
    }

    firstRun = false;
  }

  // foot placement
  for (int l = 0; l < 4; l++)
  {
    swingTimes[l] = gait->getCurrentSwingTime(dtMPC, l);
  }

  float side_sign[4] = {-1, 1, -1, 1};
  float interleave_y[4] = {-0.08, 0.08, 0.02, -0.02};
  float interleave_gain = -0.2;
  float v_abs = std::fabs(v_des_robot[0]);

  // cout << "iter: " << iterationCounter << " first swing leg 0" << firstSwing[0] << endl;

  static float z_des[4] = {0};

  for (int i = 0; i < 4; i++)
  {
    if (firstSwing[i])
    {
      swingTimeRemaining[i] = swingTimes[i];
    }
    else
    {
      swingTimeRemaining[i] -= dt;
    }

    footSwingTrajectories[i].setHeight(_parameters->Swing_traj_height);

    Vec3<float> offset(0, side_sign[i] * data._quadruped->_abadLinkLength, 0);

    Vec3<float> pRobotFrame = (data._quadruped->getHipLocation(i) + offset);

    pRobotFrame[1] += interleave_y[i] * v_abs * interleave_gain;
    float stance_time = gait->getCurrentStanceTime(dtMPC, i);

    Vec3<float> pYawCorrected = coordinateRotation(CoordinateAxis::Z, -_yaw_turn_rate * stance_time / 2) * pRobotFrame;

    Vec3<float> des_vel;
    des_vel[0] = _x_vel_des;
    des_vel[1] = _y_vel_des;
    des_vel[2] = 0.0;

    Vec3<float> Pf = seResult.position + seResult.rBody.transpose() * (pYawCorrected + des_vel * swingTimeRemaining[i]);

    float p_rel_max = 0.3f;

    // Using the estimated velocity is correct
    float pfx_rel = seResult.vWorld[0] * (.5 + _parameters->cmpc_bonus_swing) * stance_time + .03f * (seResult.vWorld[0] - v_des_world[0]) + (0.5f * seResult.position[2] / 9.81f) * (seResult.vWorld[1] * _yaw_turn_rate);
    float pfy_rel = seResult.vWorld[1] * .5 * stance_time * dtMPC + .03f * (seResult.vWorld[1] - v_des_world[1]) + (0.5f * seResult.position[2] / 9.81f) * (-seResult.vWorld[0] * _yaw_turn_rate);

    pfx_rel = fminf(fmaxf(pfx_rel, -p_rel_max), p_rel_max);
    pfy_rel = fminf(fmaxf(pfy_rel, -p_rel_max), p_rel_max);
    Pf[0] += pfx_rel;
    Pf[1] += pfy_rel;
    // Pf[2] = 0.0;
    Pf[2] = z_des[i];

    footSwingTrajectories[i].setFinalPosition(Pf);
    data.debug->all_legs_info.leg[i].swing_pf.x = Pf(0);
    data.debug->all_legs_info.leg[i].swing_pf.y = Pf(1);
    data.debug->all_legs_info.leg[i].swing_pf.z = Pf(2);
  }

  // calc gait
  iterationCounter++;

  // gait
  //trot leg 0 starts in stance because offset is 0
  Vec4<float> contactStates = gait->getContactState();
  Vec4<float> swingStates = gait->getSwingState();
  int* mpcTable = gait->getMpcTable();

  for (size_t leg_num = 0; leg_num < 4; leg_num++)
  {
    data.debug->all_legs_info.leg[leg_num].stance_time = contactStates[leg_num];
    data.debug->all_legs_info.leg[leg_num].swing_time = swingStates[leg_num];
    data.debug->all_legs_info.leg[leg_num].phase = gait->getCurrentGaitPhase();
    data.debug->all_legs_info.leg[leg_num].is_contact = data._stateEstimator->getContactSensorData()(leg_num);
  }

  // std::cout << "sensor data: " << data._stateEstimator->getContactSensorData()(0) << std::endl;
  static Vec3<float> pDesFootWorldStance[4] = {pFoot[0], pFoot[1], pFoot[2], pFoot[3]};

  //p front mid, p back mid
  Vec3<float> p_fm = (pDesFootWorldStance[0] + pDesFootWorldStance[1]) / 2;
  Vec3<float> p_bm = (pDesFootWorldStance[2] + pDesFootWorldStance[3]) / 2;
  float des_pitch = 0;
  float des_roll = 0;

  //XZ plane
  float L_xz = sqrt((p_fm(2) - p_bm(2)) * (p_fm(2) - p_bm(2)) + (p_fm(0) - p_bm(0)) * (p_fm(0) - p_bm(0)));

  if (abs(L_xz) < 0.0001)
  {
    des_pitch = 0;
  }
  else
  {
    des_pitch = des_pitch * (1 - 0.8) - asin((p_fm(2) - p_bm(2)) / (L_xz)) * 0.8;
  }

  // cout << "pfm z: " << p_fm(2) << " pbm z: " << p_bm(2) << endl;

  // //YZ plane
  // float L_yz = sqrt((p_fm(2) - p_bm(2)) * (p_fm(2) - p_bm(2)) + (p_fm(1) - p_bm(1)) * (p_fm(1) - p_bm(1)));

  // if (abs(L_yz) < 0.0001)
  // {
  //   des_roll = 0;
  // }
  // else
  // {
  //   des_roll = asin((p_fm(2) - p_bm(2)) / (L_yz));
  // }

  // cout << "des pitch correction: " << des_pitch << endl;

  data.debug->all_legs_info.leg[0].force_raw = des_pitch;

  updateMPCIfNeeded(mpcTable, data, omniMode);

  Vec4<float> se_contactState(0, 0, 0, 0);
  se_contactState = data._stateEstimator->getContactSensorData().cast<float>();

  // ROS_INFO_STREAM("is contact: " << se_contactState(0));

  static bool is_stance[4] = {0, 0, 0, 0};

  for (int foot = 0; foot < 4; foot++)
  {
    float contactState = contactStates[foot];
    float swingState = swingStates[foot];

    //if first stance
    // if ((is_stance[foot] == 0) && (se_contactState[foot] == 1) && (swingState > 0.65))
    if ((is_stance[foot] == 0) && !(swingState > 0))
    {
      is_stance[foot] = 1;

      //foot position in world frame at contanct
      pDesFootWorldStance[foot] = pFoot[foot] + footSwingTrajectories[foot].getPosition();
    }

    // if ((se_contactState(foot) == 1) && (swingState > 0) && (is_stance[foot]
    // == 0)) if ((se_contactState(foot) == 2) && (swingState > 0))
    // {
    //   swingState = 1;
    //   is_stance[foot] = 2;
    //   ROS_INFO_STREAM("Foot " << foot << " in contact early: " <<
    //   swingState);
    // }

    // if(foot == 1)
    // {
    //   ROS_INFO_STREAM("contact: " << contactState);
    //   ROS_INFO_STREAM("swing: " << swingState);
    // }

    // contactState = data._stateEstimator->getResult().contactEstimate[foot];
    // swingState = 1 - data._stateEstimator->getResult().contactEstimate[foot];

    if (swingState > 0) // foot is in swing
    {
      if (firstSwing[foot])
      {
        firstSwing[foot] = false;
        is_stance[foot] = 0;
        footSwingTrajectories[foot].setInitialPosition(pFoot[foot]);
        data.debug->all_legs_info.leg[foot].swing_ps.x = pFoot[foot](0);
        data.debug->all_legs_info.leg[foot].swing_ps.y = pFoot[foot](1);
        data.debug->all_legs_info.leg[foot].swing_ps.z = pFoot[foot](2);

        z_des[foot] = pFoot[foot][2];
      }

      footSwingTrajectories[foot].computeSwingTrajectoryBezier(swingState, swingTimes[foot]);

      Vec3<float> pDesFootWorld = footSwingTrajectories[foot].getPosition();
      Vec3<float> vDesFootWorld = footSwingTrajectories[foot].getVelocity();
      Vec3<float> pDesLeg = seResult.rBody * (pDesFootWorld - seResult.position) - data._quadruped->getHipLocation(foot);
      Vec3<float> vDesLeg = seResult.rBody * (vDesFootWorld - seResult.vWorld);
      // Vec3<float> pActFootWorld = seResult.rBody.inverse() * (data._legController->datas[foot].p + data._quadruped->getHipLocation(foot)) + seResult.position;
      // Vec3<float> vActFootWorld = seResult.rBody.inverse() * (data._legController->datas[foot].v) + seResult.vWorld;

      // Update for WBC
      pFoot_des[foot] = pDesFootWorld;
      vFoot_des[foot] = vDesFootWorld;
      aFoot_des[foot] = footSwingTrajectories[foot].getAcceleration();

      data.debug->all_legs_info.leg[foot].p_des.x = pDesLeg[0];
      data.debug->all_legs_info.leg[foot].p_des.y = pDesLeg[1];
      data.debug->all_legs_info.leg[foot].p_des.z = pDesLeg[2];

      data.debug->all_legs_info.leg[foot].v_des.x = vDesLeg[0];
      data.debug->all_legs_info.leg[foot].v_des.y = vDesLeg[1];
      data.debug->all_legs_info.leg[foot].v_des.z = vDesLeg[2];

      data.debug->all_legs_info.leg[foot].p_w_act.x = pFoot[foot][0];
      data.debug->all_legs_info.leg[foot].p_w_act.y = pFoot[foot][1];
      data.debug->all_legs_info.leg[foot].p_w_act.z = pFoot[foot][2];
      // data.debug->all_legs_info.leg[foot].p_w_act.x = pActFootWorld[0];
      // data.debug->all_legs_info.leg[foot].p_w_act.y = pActFootWorld[1];
      // data.debug->all_legs_info.leg[foot].p_w_act.z = pActFootWorld[2];

      // data.debug->all_legs_info.leg[foot].v_w_act.x = vActFootWorld[0];
      // data.debug->all_legs_info.leg[foot].v_w_act.y = vActFootWorld[1];
      // data.debug->all_legs_info.leg[foot].v_w_act.z = vActFootWorld[2];

      data.debug->all_legs_info.leg[foot].p_w_des.x = pDesFootWorld[0];
      data.debug->all_legs_info.leg[foot].p_w_des.y = pDesFootWorld[1];
      data.debug->all_legs_info.leg[foot].p_w_des.z = pDesFootWorld[2];

      data.debug->all_legs_info.leg[foot].v_w_des.x = vDesFootWorld[0];
      data.debug->all_legs_info.leg[foot].v_w_des.y = vDesFootWorld[1];
      data.debug->all_legs_info.leg[foot].v_w_des.z = vDesFootWorld[2];

      if (!data.userParameters->use_wbc)
      {
        // Update leg control command regardless of the usage of WBIC
        data._legController->commands[foot].pDes = pDesLeg;
        data._legController->commands[foot].vDes = vDesLeg;
        data._legController->commands[foot].kpCartesian = Kp;
        data._legController->commands[foot].kdCartesian = Kd;
      }
    }
    else // foot is in stance
    {
      firstSwing[foot] = true;

      Vec3<float> pDesFootWorld = footSwingTrajectories[foot].getPosition();
      Vec3<float> vDesFootWorld = footSwingTrajectories[foot].getVelocity();
      // Vec3<float> vDesFootWorld(0, 0, 0);
      // Vec3<float> pDesLeg = seResult.rBody * (pDesFootWorldStance[foot] - seResult.position) - data._quadruped->getHipLocation(foot);
      Vec3<float> pDesLeg = seResult.rBody * (pDesFootWorld - seResult.position) - data._quadruped->getHipLocation(foot);
      Vec3<float> vDesLeg = seResult.rBody * (vDesFootWorld - seResult.vWorld);
      // Vec3<float> pActFootWorld = seResult.rBody.inverse() * (data._legController->datas[foot].p + data._quadruped->getHipLocation(foot)) + seResult.position;
      Vec3<float> vActFootWorld = seResult.rBody.inverse() * (data._legController->datas[foot].v) + seResult.vWorld;

      if (!data.userParameters->use_wbc) // wbc off
      {
        data._legController->commands[foot].pDes = pDesLeg;
        data._legController->commands[foot].vDes = vDesLeg;
        data._legController->commands[foot].kpCartesian = Kp_stance;
        data._legController->commands[foot].kdCartesian = Kd_stance;

        data._legController->commands[foot].forceFeedForward = f_ff[foot];
        data._legController->commands[foot].kdJoint = Vec3<float>(_parameters->Kd_joint_0, _parameters->Kd_joint_1, _parameters->Kd_joint_2).asDiagonal();
      }
      else
      { // Stance foot damping
        data._legController->commands[foot].pDes = pDesLeg;
        data._legController->commands[foot].vDes = vDesLeg;
        data._legController->commands[foot].kpCartesian = 0. * Kp_stance;
        data._legController->commands[foot].kdCartesian = Kd_stance;
      }

      se_contactState[foot] = contactState;

      data.debug->all_legs_info.leg[foot].p_des.x = pDesLeg[0];
      data.debug->all_legs_info.leg[foot].p_des.y = pDesLeg[1];
      data.debug->all_legs_info.leg[foot].p_des.z = pDesLeg[2];

      data.debug->all_legs_info.leg[foot].v_des.x = vDesLeg[0];
      data.debug->all_legs_info.leg[foot].v_des.y = vDesLeg[1];
      data.debug->all_legs_info.leg[foot].v_des.z = vDesLeg[2];

      data.debug->all_legs_info.leg[foot].p_w_act.x = pFoot[foot][0];
      data.debug->all_legs_info.leg[foot].p_w_act.y = pFoot[foot][1];
      data.debug->all_legs_info.leg[foot].p_w_act.z = pFoot[foot][2];

      data.debug->all_legs_info.leg[foot].v_w_act.x = vActFootWorld[0];
      data.debug->all_legs_info.leg[foot].v_w_act.y = vActFootWorld[1];
      data.debug->all_legs_info.leg[foot].v_w_act.z = vActFootWorld[2];

      // data.debug->all_legs_info.leg[foot].p_w_des.x = pDesFootWorldStance[foot][0];
      // data.debug->all_legs_info.leg[foot].p_w_des.y = pDesFootWorldStance[foot][1];
      // data.debug->all_legs_info.leg[foot].p_w_des.z = pDesFootWorldStance[foot][2];
      data.debug->all_legs_info.leg[foot].p_w_des.x = pDesFootWorld[0];
      data.debug->all_legs_info.leg[foot].p_w_des.y = pDesFootWorld[1];
      data.debug->all_legs_info.leg[foot].p_w_des.z = pDesFootWorld[2];

      data.debug->all_legs_info.leg[foot].v_w_des.x = vDesFootWorld[0];
      data.debug->all_legs_info.leg[foot].v_w_des.y = vDesFootWorld[1];
      data.debug->all_legs_info.leg[foot].v_w_des.z = vDesFootWorld[2];
    }
  }

  data._stateEstimator->setContactPhase(se_contactState);
  data._stateEstimator->setSwingPhase(gait->getSwingState());

  // Update For WBC
  pBody_des[0] = world_position_desired[0];
  pBody_des[1] = world_position_desired[1];
  pBody_des[2] = _body_height;

  vBody_des[0] = v_des_world[0];
  vBody_des[1] = v_des_world[1];
  vBody_des[2] = 0.;

  aBody_des.setZero();

  pBody_RPY_des[0] = 0.;
  pBody_RPY_des[1] = 0.;
  pBody_RPY_des[2] = _yaw_des;

  vBody_Ori_des[0] = 0.;
  vBody_Ori_des[1] = 0.;
  vBody_Ori_des[2] = _yaw_turn_rate;

  data.debug->body_info.pos_des.x = pBody_des[0];
  data.debug->body_info.pos_des.y = pBody_des[1];
  data.debug->body_info.pos_des.z = pBody_des[2];

  data.debug->body_info.vel_des.linear.x = vBody_des[0];
  data.debug->body_info.vel_des.linear.y = vBody_des[1];
  data.debug->body_info.vel_des.linear.z = vBody_des[2];

  data.debug->body_info.euler_des.x = pBody_RPY_des[0];
  data.debug->body_info.euler_des.y = pBody_RPY_des[1];
  data.debug->body_info.euler_des.z = pBody_RPY_des[2];

  data.debug->body_info.vel_des.angular.x = vBody_Ori_des[0];
  data.debug->body_info.vel_des.angular.y = vBody_Ori_des[1];
  data.debug->body_info.vel_des.angular.z = vBody_Ori_des[2];

  contact_state = gait->getContactState();
}

template <>
void CMPCLocomotion::run(ControlFSMData<double>& data)
{
  (void)data;
  printf("call to old CMPC with double!\n");
}

void CMPCLocomotion::updateMPCIfNeeded(int* mpcTable, ControlFSMData<float>& data, bool omniMode)
{
  // iterationsBetweenMPC = 30;
  if ((iterationCounter % iterationsBetweenMPC) == 0)
  {
    auto seResult = data._stateEstimator->getResult();
    float* p = seResult.position.data();

    Vec3<float> v_des_robot(_x_vel_des, _y_vel_des, 0);
    Vec3<float> v_des_world = omniMode ? v_des_robot : seResult.rBody.transpose() * v_des_robot;
    // float trajInitial[12] = {0,0,0, 0,0,.25, 0,0,0,0,0,0};

    // printf("Position error: %.3f, integral %.3f\n", pxy_err[0],
    // x_comp_integral);

    // Stand gait
    if (current_gait == 4)
    // if (current_gait == 30)
    {
      float trajInitial[12] = {
          _roll_des,
          _pitch_des /*-hw_i->state_estimator->se_ground_pitch*/,
          (float)stand_traj[5] /*+(float)stateCommand->data.stateDes[11]*/,
          (float)stand_traj[0] /*+(float)fsm->main_control_settings.p_des[0]*/,
          (float)stand_traj[1] /*+(float)fsm->main_control_settings.p_des[1]*/,
          (float)_body_height /*fsm->main_control_settings.p_des[2]*/,
          0,
          0,
          0,
          0,
          0,
          0};

      for (int i = 0; i < horizonLength; i++)
        for (int j = 0; j < 12; j++)
          trajAll[12 * i + j] = trajInitial[j];
    }
    else
    {
      const float max_pos_error = .1;
      float xStart = world_position_desired[0];
      float yStart = world_position_desired[1];

      if (xStart - p[0] > max_pos_error)
        xStart = p[0] + max_pos_error;
      if (p[0] - xStart > max_pos_error)
        xStart = p[0] - max_pos_error;

      if (yStart - p[1] > max_pos_error)
        yStart = p[1] + max_pos_error;
      if (p[1] - yStart > max_pos_error)
        yStart = p[1] - max_pos_error;

      world_position_desired[0] = xStart;
      world_position_desired[1] = yStart;

      float trajInitial[12] = {(float)rpy_comp[0], // 0
                               (float)rpy_comp[1], // 1
                               _yaw_des,           // 2
                               // yawStart,    // 2
                               xStart,              // 3
                               yStart,              // 4
                               (float)_body_height, // 5
                               0,                   // 6
                               0,                   // 7
                               _yaw_turn_rate,      // 8
                               v_des_world[0],      // 9
                               v_des_world[1],      // 10
                               0};                  // 11

      for (int i = 0; i < horizonLength; i++)
      {
        for (int j = 0; j < 12; j++)
          trajAll[12 * i + j] = trajInitial[j];

        if (i == 0) // start at current position  TODO consider not doing this
        {
          // trajAll[3] = hw_i->state_estimator->se_pBody[0];
          // trajAll[4] = hw_i->state_estimator->se_pBody[1];
          trajAll[2] = seResult.rpy[2];
        }
        else
        {
          trajAll[12 * i + 3] = trajAll[12 * (i - 1) + 3] + dtMPC * v_des_world[0];
          trajAll[12 * i + 4] = trajAll[12 * (i - 1) + 4] + dtMPC * v_des_world[1];
          trajAll[12 * i + 2] = trajAll[12 * (i - 1) + 2] + dtMPC * _yaw_turn_rate;
        }
      }
    }
    Timer solveTimer;

    if (_parameters->cmpc_use_sparse > 0.5)
    {
      solveSparseMPC(mpcTable, data);
    }
    else
    {
      solveDenseMPC(mpcTable, data);
    }
    // printf("TOTAL SOLVE TIME: %.3f\n", solveTimer.getMs());
  }
}

void CMPCLocomotion::solveDenseMPC(int* mpcTable, ControlFSMData<float>& data)
{
  auto seResult = data._stateEstimator->getResult();

  // float Q[12] = {0.25, 0.25, 10, 2, 2, 20, 0, 0, 0.3, 0.2, 0.2, 0.2};

  // float Q[12] = {0.25, 0.25, 10, 2, 2, 50, 0, 0, 0.3, 0.2, 0.2, 0.1};
  // //original
  float Q[12] = {2.5, 2.5, 10, 50, 50, 100, 0, 0, 0.5, 0.2, 0.2, 0.1};

  // float Q[12] = {0.25, 0.25, 10, 2, 2, 40, 0, 0, 0.3, 0.2, 0.2, 0.2};
  float yaw = seResult.rpy[2];
  float* weights = Q;
  float alpha = 4e-5; // make setting eventually
  // float alpha = 4e-7; // make setting eventually: DH
  float* p = seResult.position.data();
  float* v = seResult.vWorld.data();
  float* w = seResult.omegaWorld.data();
  float* q = seResult.orientation.data();

  float r[12];
  for (int i = 0; i < 12; i++)
  {
    r[i] = pFoot[i % 4][i / 4] - seResult.position[i / 4];
  }

  // printf("current posistion: %3.f %.3f %.3f\n", p[0], p[1], p[2]);

  if (alpha > 1e-4)
  {
    std::cout << "Alpha was set too high (" << alpha << ") adjust to 1e-5\n";
    alpha = 1e-5;
  }

  Vec3<float> pxy_act(p[0], p[1], 0);
  Vec3<float> pxy_des(world_position_desired[0], world_position_desired[1], 0);
  // Vec3<float> pxy_err = pxy_act - pxy_des;
  float pz_err = p[2] - _body_height;
  Vec3<float> vxy(seResult.vWorld[0], seResult.vWorld[1], 0);

  Timer t1;
  dtMPC = dt * iterationsBetweenMPC;
  setup_problem(dtMPC, horizonLength, 0.4, 120);
  // setup_problem(dtMPC,horizonLength,0.4,650); //DH
  update_x_drag(x_comp_integral);

  if (vxy[0] > 0.3 || vxy[0] < -0.3)
  {
    // x_comp_integral += _parameters->cmpc_x_drag * pxy_err[0] * dtMPC /
    // vxy[0];
    x_comp_integral += _parameters->cmpc_x_drag * pz_err * dtMPC / vxy[0];
  }

  // printf("pz err: %.3f, pz int: %.3f\n", pz_err, x_comp_integral);

  update_solver_settings(_parameters->jcqp_max_iter, _parameters->jcqp_rho, _parameters->jcqp_sigma,
                         _parameters->jcqp_alpha, _parameters->jcqp_terminate,
                         _parameters->use_jcqp);
  // t1.stopPrint("Setup MPC");
  // printf("MPC Setup time %f ms\n", t1.getMs());

  Timer t2;
  // cout << "dtMPC: " << dtMPC << "\n";
  update_problem_data_floats(p, v, q, w, r, yaw, weights, trajAll, alpha, mpcTable);
  // t2.stopPrint("Run MPC");
  // printf("MPC Solve time %f ms\n", t2.getMs());

  for (int leg = 0; leg < 4; leg++)
  {
    Vec3<float> f;
    for (int axis = 0; axis < 3; axis++)
      f[axis] = get_solution(leg * 3 + axis);

    // printf("[%d] %7.3f %7.3f %7.3f\n", leg, f[0], f[1], f[2]);

    f_ff[leg] = -seResult.rBody * f;
    // Update for WBC
    Fr_des[leg] = f;
  }
}

void CMPCLocomotion::solveSparseMPC(int* mpcTable, ControlFSMData<float>& data)
{
  // X0, contact trajectory, state trajectory, feet, get result!
  (void)mpcTable;
  (void)data;
  auto seResult = data._stateEstimator->getResult();

  std::vector<ContactState> contactStates;
  for (int i = 0; i < horizonLength; i++)
  {
    contactStates.emplace_back(mpcTable[i * 4 + 0], mpcTable[i * 4 + 1], mpcTable[i * 4 + 2],
                               mpcTable[i * 4 + 3]);
  }

  for (int i = 0; i < horizonLength; i++)
  {
    for (u32 j = 0; j < 12; j++)
    {
      _sparseTrajectory[i][j] = trajAll[i * 12 + j];
    }
  }

  Vec12<float> feet;
  for (u32 foot = 0; foot < 4; foot++)
  {
    for (u32 axis = 0; axis < 3; axis++)
    {
      feet[foot * 3 + axis] = pFoot[foot][axis] - seResult.position[axis];
    }
  }

  _sparseCMPC.setX0(seResult.position, seResult.vWorld, seResult.orientation, seResult.omegaWorld);
  _sparseCMPC.setContactTrajectory(contactStates.data(), contactStates.size());
  _sparseCMPC.setStateTrajectory(_sparseTrajectory);
  _sparseCMPC.setFeet(feet);
  _sparseCMPC.run();

  Vec12<float> resultForce = _sparseCMPC.getResult();

  for (u32 foot = 0; foot < 4; foot++)
  {
    Vec3<float> force(resultForce[foot * 3], resultForce[foot * 3 + 1], resultForce[foot * 3 + 2]);
    // printf("[%d] %7.3f %7.3f %7.3f\n", foot, force[0], force[1], force[2]);
    f_ff[foot] = -seResult.rBody * force;
    Fr_des[foot] = force;
  }
}

void CMPCLocomotion::initSparseMPC()
{
  Mat3<double> baseInertia;
  baseInertia << 0.07, 0, 0, 0, 0.26, 0, 0, 0, 0.242;
  double mass = 9;
  double maxForce = 120;

  std::vector<double> dtTraj;
  for (int i = 0; i < horizonLength; i++)
  {
    dtTraj.push_back(dtMPC);
  }

  Vec12<double> weights;
  weights << 0.25, 0.25, 10, 2, 2, 20, 0, 0, 0.3, 0.2, 0.2, 0.2;
  // weights << 0,0,0,1,1,10,0,0,0,0.2,0.2,0;

  _sparseCMPC.setRobotParameters(baseInertia, mass, maxForce);
  _sparseCMPC.setFriction(1.0);
  // _sparseCMPC.setFriction(0.4);
  _sparseCMPC.setWeights(weights, 4e-5);
  _sparseCMPC.setDtTrajectory(dtTraj);

  _sparseTrajectory.resize(horizonLength);
}