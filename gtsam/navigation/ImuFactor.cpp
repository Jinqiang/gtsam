/* ----------------------------------------------------------------------------

 * GTSAM Copyright 2010, Georgia Tech Research Corporation, 
 * Atlanta, Georgia 30332-0415
 * All Rights Reserved
 * Authors: Frank Dellaert, et al. (see THANKS for the full author list)

 * See LICENSE for the license information

 * -------------------------------------------------------------------------- */

/**
 *  @file  ImuFactor.cpp
 *  @author Luca Carlone
 *  @author Stephen Williams
 *  @author Richard Roberts
 *  @author Vadim Indelman
 *  @author David Jensen
 *  @author Frank Dellaert
 **/

#include <gtsam/navigation/ImuFactor.h>

/* External or standard includes */
#include <ostream>

namespace gtsam {

using namespace std;

//------------------------------------------------------------------------------
// Inner class PreintegratedMeasurements
//------------------------------------------------------------------------------
ImuFactor::PreintegratedMeasurements::PreintegratedMeasurements(
    const imuBias::ConstantBias& bias, const Matrix3& measuredAccCovariance,
    const Matrix3& measuredOmegaCovariance, const Matrix3& integrationErrorCovariance,
    const bool use2ndOrderIntegration) :
        PreintegrationBase(bias, use2ndOrderIntegration)
{
  measurementCovariance_.setZero();
  measurementCovariance_.block<3,3>(0,0) = integrationErrorCovariance;
  measurementCovariance_.block<3,3>(3,3) = measuredAccCovariance;
  measurementCovariance_.block<3,3>(6,6) = measuredOmegaCovariance;
  preintMeasCov_.setZero();
}

//------------------------------------------------------------------------------
void ImuFactor::PreintegratedMeasurements::print(const string& s) const {
  PreintegrationBase::print(s);
  cout << "  measurementCovariance = \n [ " << measurementCovariance_ << " ]" << endl;
  cout << "  preintMeasCov = \n [ " << preintMeasCov_ << " ]" << endl;
}

//------------------------------------------------------------------------------
bool ImuFactor::PreintegratedMeasurements::equals(const PreintegratedMeasurements& expected, double tol) const {
  return equal_with_abs_tol(measurementCovariance_, expected.measurementCovariance_, tol)
  && equal_with_abs_tol(preintMeasCov_, expected.preintMeasCov_, tol)
  && PreintegrationBase::equals(expected, tol);
}

//------------------------------------------------------------------------------
void ImuFactor::PreintegratedMeasurements::resetIntegration(){
  PreintegrationBase::resetIntegration();
  preintMeasCov_.setZero();
}

//------------------------------------------------------------------------------
void ImuFactor::PreintegratedMeasurements::integrateMeasurement(
    const Vector3& measuredAcc, const Vector3& measuredOmega, double deltaT,
    boost::optional<const Pose3&> body_P_sensor,
    boost::optional<Matrix&> Fout, boost::optional<Matrix&> Gout) {

  // NOTE: order is important here because each update uses old values (i.e., we have to update
  // jacobians and covariances before updating preintegrated measurements).

  Vector3 correctedAcc, correctedOmega;
  correctMeasurementsByBiasAndSensorPose(measuredAcc, measuredOmega, correctedAcc, correctedOmega, body_P_sensor);

  const Vector3 theta_incr = correctedOmega * deltaT; // rotation vector describing rotation increment computed from the current rotation rate measurement
  const Rot3 Rincr = Rot3::Expmap(theta_incr); // rotation increment computed from the current rotation rate measurement
  const Matrix3 Jr_theta_incr = Rot3::rightJacobianExpMapSO3(theta_incr); // Right jacobian computed at theta_incr

  // Update Jacobians
  /* ----------------------------------------------------------------------------------------------------------------------- */
  updatePreintegratedJacobians(correctedAcc, Jr_theta_incr, Rincr, deltaT);

  // Update preintegrated measurements covariance
  // as in [2] we consider a first order propagation that can be seen as a prediction phase in an EKF framework
  /* ----------------------------------------------------------------------------------------------------------------------- */
  const Vector3 theta_i = thetaRij(); // super-expensive parametrization of so(3)
  const Matrix3 R_i = deltaRij();
  const Matrix3 Jr_theta_i = Rot3::rightJacobianExpMapSO3(theta_i);

  // Update preintegrated measurements. TODO Frank moved from end of this function !!!
  // Even though Luca says has to happen after ? Don't understand why.
  updatePreintegratedMeasurements(correctedAcc, Rincr, deltaT);

  const Vector3 theta_j = thetaRij(); // super-expensive parametrization of so(3)
  const Matrix3 Jrinv_theta_j = Rot3::rightJacobianExpMapSO3inverse(theta_j);

  Matrix H_pos_pos    = I_3x3;
  Matrix H_pos_vel    = I_3x3 * deltaT;
  Matrix H_pos_angles = Z_3x3;

  Matrix H_vel_pos    = Z_3x3;
  Matrix H_vel_vel    = I_3x3;
  Matrix H_vel_angles = - R_i * skewSymmetric(correctedAcc) * Jr_theta_i * deltaT;
  // analytic expression corresponding to the following numerical derivative
  // Matrix H_vel_angles = numericalDerivative11<Vector3, Vector3>(boost::bind(&PreIntegrateIMUObservations_delta_vel, correctedOmega, correctedAcc, deltaT, _1, deltaVij), theta_i);

  Matrix H_angles_pos   = Z_3x3;
  Matrix H_angles_vel    = Z_3x3;
  Matrix H_angles_angles = Jrinv_theta_j * Rincr.inverse().matrix() * Jr_theta_i;
  // analytic expression corresponding to the following numerical derivative
  // Matrix H_angles_angles = numericalDerivative11<Vector3, Vector3>(boost::bind(&PreIntegrateIMUObservations_delta_angles, correctedOmega, deltaT, _1), thetaij);

  // overall Jacobian wrt preintegrated measurements (df/dx)
  Matrix F(9,9);
  F << H_pos_pos, H_pos_vel,  H_pos_angles,
      H_vel_pos, H_vel_vel, H_vel_angles,
      H_angles_pos, H_angles_vel, H_angles_angles;

  // first order uncertainty propagation:
  // the deltaT allows to pass from continuous time noise to discrete time noise
  // measurementCovariance_discrete = measurementCovariance_contTime * (1/deltaT)
  // Gt * Qt * G =(approx)= measurementCovariance_discrete * deltaT^2 = measurementCovariance_contTime * deltaT
  preintMeasCov_ = F * preintMeasCov_ * F.transpose() + measurementCovariance_ * deltaT ;

  // Fout and Gout are used for testing purposes and are not needed by the factor
  if(Fout){
    Fout->resize(9,9);
    (*Fout) << F;
  }
  if(Gout){
    // Extended version, without approximation: Gt * Qt * G =(approx)= measurementCovariance_contTime * deltaT
    // This in only kept for testing.
    Gout->resize(9,9);
    (*Gout) << I_3x3 * deltaT, Z_3x3,        Z_3x3,
               Z_3x3,          R_i * deltaT, Z_3x3,
               Z_3x3,          Z_3x3,        Jrinv_theta_j * Jr_theta_incr * deltaT;
    //preintMeasCov = F * preintMeasCov * F.transpose() + Gout * (1/deltaT) * measurementCovariance * Gout.transpose();
  }
}

//------------------------------------------------------------------------------
// ImuFactor methods
//------------------------------------------------------------------------------
ImuFactor::ImuFactor() :
    ImuFactorBase(), _PIM_(imuBias::ConstantBias(), Z_3x3, Z_3x3, Z_3x3) {}

//------------------------------------------------------------------------------
ImuFactor::ImuFactor(
    Key pose_i, Key vel_i, Key pose_j, Key vel_j, Key bias,
    const PreintegratedMeasurements& preintegratedMeasurements,
    const Vector3& gravity, const Vector3& omegaCoriolis,
    boost::optional<const Pose3&> body_P_sensor,
    const bool use2ndOrderCoriolis) :
        Base(noiseModel::Gaussian::Covariance(preintegratedMeasurements.preintMeasCov_),
            pose_i, vel_i, pose_j, vel_j, bias),
        ImuFactorBase(gravity, omegaCoriolis, body_P_sensor, use2ndOrderCoriolis),
        _PIM_(preintegratedMeasurements) {}

//------------------------------------------------------------------------------
gtsam::NonlinearFactor::shared_ptr ImuFactor::clone() const {
  return boost::static_pointer_cast<gtsam::NonlinearFactor>(
      gtsam::NonlinearFactor::shared_ptr(new This(*this)));
}

//------------------------------------------------------------------------------
void ImuFactor::print(const string& s, const KeyFormatter& keyFormatter) const {
  cout << s << "ImuFactor("
      << keyFormatter(this->key1()) << ","
      << keyFormatter(this->key2()) << ","
      << keyFormatter(this->key3()) << ","
      << keyFormatter(this->key4()) << ","
      << keyFormatter(this->key5()) << ")\n";
  ImuFactorBase::print("");
  _PIM_.print("  preintegrated measurements:");
  this->noiseModel_->print("  noise model: ");
}

//------------------------------------------------------------------------------
bool ImuFactor::equals(const NonlinearFactor& expected, double tol) const {
  const This *e =  dynamic_cast<const This*> (&expected);
  return e != NULL && Base::equals(*e, tol)
  && _PIM_.equals(e->_PIM_, tol)
  && ImuFactorBase::equals(*e, tol);
}

//------------------------------------------------------------------------------
Vector ImuFactor::evaluateError(const Pose3& pose_i, const Vector3& vel_i,
    const Pose3& pose_j, const Vector3& vel_j,
    const imuBias::ConstantBias& bias_i, boost::optional<Matrix&> H1,
    boost::optional<Matrix&> H2, boost::optional<Matrix&> H3,
    boost::optional<Matrix&> H4, boost::optional<Matrix&> H5) const {

  return _PIM_.computeErrorAndJacobians(pose_i, vel_i, pose_j, vel_j, bias_i,
      gravity_, omegaCoriolis_, use2ndOrderCoriolis_, H1, H2, H3, H4, H5);
}

} /// namespace gtsam
