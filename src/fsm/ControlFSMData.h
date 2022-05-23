#ifndef CONTROLFSMDATA_H
#define CONTROLFSMDATA_H

#include "Controllers/DesiredStateCommand.h"
#include "Controllers/GaitScheduler.h"
#include "Controllers/LegController.h"
#include "Controllers/StateEstimatorContainer.h"
#include "Dynamics/Quadruped.h"
#include <ControlParameters/RobotParameters.h>
#include <be2r_cmpc_unitree/ros_dynamic_paramsConfig.h>
#include <debug.hpp>

/**
 *
 */
template<typename T>
struct ControlFSMData
{
  // EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  Quadruped<T>* _quadruped;
  StateEstimatorContainer<T>* _stateEstimator;
  LegController<T>* _legController;
  GaitScheduler<T>* _gaitScheduler;
  DesiredStateCommand<T>* _desiredStateCommand;
  RobotControlParameters* controlParameters;
  be2r_cmpc_unitree::ros_dynamic_paramsConfig* userParameters;
  VisualizationData* visualizationData;
  Debug* debug;
};

template struct ControlFSMData<double>;
template struct ControlFSMData<float>;

#endif // CONTROLFSM_H
