#ifndef MSF_LOAM_VELODYNE_LASER_ODOMETRY_H
#define MSF_LOAM_VELODYNE_LASER_ODOMETRY_H

#include <ros/node_handle.h>

#include "common/timestamped_pointcloud.h"
#include "laser_mapping.h"
#include "slam/imu_fusion/imu_tracker.h"

class LaserOdometry {
 public:
  explicit LaserOdometry(bool is_offline_mode);

  ~LaserOdometry();

  void AddLaserScan(TimestampedPointCloud scan_curr);

  void AddImu(const ImuData &imu_data);

 private:
  std::shared_ptr<LaserMapping> laser_mapper_handler_;
  std::unique_ptr<ImuTracker> imu_tracker_;

  TimestampedPointCloud scan_last_;

  // Transformation from scan to map
  Rigid3d pose_scan2world_;
  // Transformation from current scan to previous scan
  Rigid3d pose_curr2last_;

  ros::Publisher laser_odom_publisher_;
  ros::Publisher laser_path_publisher_;

  nav_msgs::Path laser_path_;
};

#endif  // MSF_LOAM_VELODYNE_LASER_ODOMETRY_H
