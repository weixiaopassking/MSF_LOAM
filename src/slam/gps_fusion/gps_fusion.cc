#include "slam/gps_fusion/gps_fusion.h"
#include "slam/gps_fusion/gps_factor.h"

namespace {

bool CompareTimeLT(const FixedPoint& l, const LocalPose& r) {
  return l.timestamp < r.timestamp;
}

}  // namespace

GpsFusion::GpsFusion() { LOG(INFO) << "GpsFusion started!"; }

GpsFusion::~GpsFusion() { LOG(INFO) << "GpsFusion finished."; }

void GpsFusion::AddFixedPoint(const Time& time,
                              const Eigen::Vector3d& translation) {
  CHECK(fixed_points_.empty() || fixed_points_.back().timestamp < time);
  fixed_points_.push_back({time, translation});
}

void GpsFusion::AddLocalPose(const Time& time, const Rigid3d& pose) {
  CHECK(local_poses_.empty() || local_poses_.back().timestamp < time);
  local_poses_.push_back({time, pose});
}

void GpsFusion::Optimize() {
  for (auto& lp : local_poses_) {
    LOG(INFO) << "local pose before gps: " << lp.timestamp << " "
              << lp.pose.translation().transpose();
  }

  if (fixed_points_.size() < 2) {
    LOG(WARNING) << "Number of fixed points less than 2!";
    return;
  }
  CHECK_GT(local_poses_.size(), 2);
  CHECK_LE(local_poses_.front().timestamp, fixed_points_.front().timestamp);
  CHECK_LE(fixed_points_.back().timestamp, local_poses_.back().timestamp);

  ceres::Problem problem;
  ceres::Solver::Options options;
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  options.minimizer_progress_to_stdout = false;
  options.num_threads = 4;
  options.max_num_iterations = 6;
  ceres::Solver::Summary summary;
  ceres::LossFunction* loss_function = new ceres::HuberLoss(1.0);
  ceres::LocalParameterization* local_parameterization =
      new ceres::EigenQuaternionParameterization();

  for (auto& lp : local_poses_) {
    problem.AddParameterBlock(lp.pose.translation().data(), 3);
    problem.AddParameterBlock(lp.pose.rotation().coeffs().data(), 4,
                              local_parameterization);
  }

  for (auto& fp : fixed_points_) {
    auto local_pose_j = std::upper_bound(local_poses_.begin(),
                                         local_poses_.end(), fp, CompareTimeLT);
    auto local_pose_i = std::prev(local_pose_j);
    if (fp.timestamp == local_poses_.end()->timestamp)
      local_pose_j = local_pose_i;
    double t = (fp.timestamp - local_pose_i->timestamp).count() * 1.0 /
               (local_pose_j->timestamp - local_pose_i->timestamp).count();
    CHECK(t >= 0 && t <= 1);
    auto cost_function = GpsFactor::Create(fp.translation, t, 0.01);
    problem.AddResidualBlock(cost_function, loss_function,
                             local_pose_i->pose.translation().data(),
                             local_pose_j->pose.translation().data());
  }

  for (size_t i = 0; i < local_poses_.size() - 1; ++i) {
    auto& local_pose_i = local_poses_[i];
    auto& local_pose_j = local_poses_[i + 1];
    auto cost_function = RelativePoseFactor::Create(
        local_pose_i.pose, local_pose_j.pose, 0.01, 0.1);
    problem.AddResidualBlock(cost_function, loss_function,
                             local_pose_i.pose.rotation().coeffs().data(),
                             local_pose_i.pose.translation().data(),
                             local_pose_j.pose.rotation().coeffs().data(),
                             local_pose_j.pose.translation().data());
  }

  ceres::Solve(options, &problem, &summary);

  for (auto& fp : fixed_points_) {
    LOG(INFO) << "gps point: " << fp.timestamp << " "
              << fp.translation.transpose();
  }

  for (auto& lp : local_poses_) {
    LOG(INFO) << "local pose after gps: " << lp.timestamp << " "
              << lp.pose.translation().transpose();
  }

  LOG(INFO) << summary.FullReport();
}
