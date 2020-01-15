#include <common/tic_toc.h>
#include <geometry_msgs/PoseStamped.h>
#include <nav_msgs/Odometry.h>
#include <pcl_conversions/pcl_conversions.h>
#include <ros/init.h>
#include <tf/transform_broadcaster.h>
#include <random>

#include "slam/local/laser_mapping.h"
#include "slam/local/scan_matching/mapping_scan_matcher.h"
#include "slam/msg_conversion.h"

namespace {

bool g_is_offline_mode;
bool g_should_exit = false;

}  // namespace

LaserMapping::LaserMapping(bool is_offline_mode)
    : gps_fusion_handler_(std::make_shared<GpsFusion>()),
      frame_idx_cur_(0),
      hybrid_grid_map_corner_(3.0),
      hybrid_grid_map_surf_(3.0) {
  g_is_offline_mode = is_offline_mode;
  // NodeHandle uses reference counting internally,
  // thus a local variable can be created here
  ros::NodeHandle nh;

  LOG(INFO) << "LaserMapping initializing ...";
  // get leaf size
  float line_res = 0;
  float plane_res = 0;
  LOG_IF(WARNING, !nh.param<float>("mapping_line_resolution", line_res, 0.2))
      << "Use default mapping_line_resolution: 0.2";
  LOG_IF(WARNING, !nh.param<float>("mapping_plane_resolution", plane_res, 0.4))
      << "Use default mapping_plane_resolution: 0.4";
  LOG(INFO) << "[MAP]"
            << " line resolution " << line_res << " plane resolution "
            << plane_res;
  downsize_filter_corner_.setLeafSize(line_res, line_res, line_res);
  downsize_filter_surf_.setLeafSize(plane_res, plane_res, plane_res);
  // set publishers
  cloud_scan_publisher_ =
      nh.advertise<sensor_msgs::PointCloud2>("/velodyne_cloud_2", 100);
  cloud_corner_publisher_ =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_sharp", 100);
  cloud_corner_less_publisher_ =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_less_sharp", 100);
  cloud_surf_publisher_ =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_flat", 100);
  cloud_surf_less_publisher_ =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_less_flat", 100);

  cloud_surround_publisher_ =
      nh.advertise<sensor_msgs::PointCloud2>("/laser_cloud_surround", 100);
  aftmapped_odom_publisher_ =
      nh.advertise<nav_msgs::Odometry>("/aft_mapped_to_init", 100);
  aftmapped_odom_highfrec_publisher_ =
      nh.advertise<nav_msgs::Odometry>("/aft_mapped_to_init_high_frec", 100);
  aftmapped_path_publisher_ =
      nh.advertise<nav_msgs::Path>("/aft_mapped_path", 100);

  // RUN
  thread_ = std::thread([this] { this->Run(); });
}

LaserMapping::~LaserMapping() {
  {
    std::unique_lock<std::mutex> ul(mutex_);
    g_should_exit = true;
  }
  thread_.join();
  gps_fusion_handler_->Optimize();
  LOG(INFO) << "LaserMapping finished.";
}

void LaserMapping::AddLaserOdometryResult(
    const LaserOdometryResultType &laser_odometry_result) {
  std::unique_lock<std::mutex> ul(mutex_);
  odometry_result_queue_.push(laser_odometry_result);
  cv_.notify_one();
  // publish odom tf
  // high frequence publish
  nav_msgs::Odometry aftmapped_odom;
  aftmapped_odom.child_frame_id = "/aft_mapped";
  aftmapped_odom.header.frame_id = "/camera_init";
  aftmapped_odom.header.stamp = ToRos(laser_odometry_result.timestamp);
  aftmapped_odom.pose = ToRos(pose_odom2map_ * laser_odometry_result.odom_pose);
  aftmapped_odom_highfrec_publisher_.publish(aftmapped_odom);
}

void LaserMapping::Run() {
  while (ros::ok()) {
    LaserOdometryResultType odom_result;
    {
      std::unique_lock<std::mutex> ul(mutex_);
      // Try to get new messages in 50 ms, return false if failed
      bool has_new_msg = cv_.wait_for(
          ul, std::chrono::milliseconds(50),
          [this] { return !this->odometry_result_queue_.empty(); });
      if (!has_new_msg) {
        if (g_should_exit)
          break;
        else
          continue;
      }
      odom_result = odometry_result_queue_.front();
      odometry_result_queue_.pop();
      if (!g_is_offline_mode) {
        while (!odometry_result_queue_.empty()) {
          LOG(WARNING)
              << "[MAP] drop lidar frame in mapping for real time performance";
          odometry_result_queue_.pop();
        }
      }
    }

    // scan match
    // input: from odom
    PointCloudConstPtr laserCloudCornerLast =
        odom_result.cloud_corner_less_sharp;
    PointCloudConstPtr laserCloudSurfLast = odom_result.cloud_surf_less_flat;
    PointCloudConstPtr laserCloudFullRes = odom_result.cloud_full_res;

    pose_odom_scan2world_ = odom_result.odom_pose;

    TicToc t_whole;

    transformAssociateToMap();

    TicToc t_shift;
    PointCloudPtr laserCloudCornerFromMap =
        hybrid_grid_map_corner_.GetSurroundedCloud(laserCloudCornerLast,
                                                   pose_map_scan2world_);
    PointCloudPtr laserCloudSurfFromMap =
        hybrid_grid_map_surf_.GetSurroundedCloud(laserCloudSurfLast,
                                                 pose_map_scan2world_);
    LOG_STEP_TIME("MAP", "Collect surround cloud", t_shift.toc());

    PointCloudPtr laserCloudCornerLastStack(new PointCloud);
    downsize_filter_corner_.setInputCloud(laserCloudCornerLast);
    downsize_filter_corner_.filter(*laserCloudCornerLastStack);

    PointCloudPtr laserCloudSurfLastStack(new PointCloud);
    downsize_filter_surf_.setInputCloud(laserCloudSurfLast);
    downsize_filter_surf_.filter(*laserCloudSurfLastStack);

    LOG(INFO) << "[MAP]"
              << " corner=" << laserCloudCornerFromMap->size()
              << ", surf=" << laserCloudSurfFromMap->size();
    if (laserCloudCornerFromMap->size() > 10 &&
        laserCloudSurfFromMap->size() > 50) {
      TimestampedPointCloud cloud_map, scan_curr;
      cloud_map.cloud_corner_less_sharp = laserCloudCornerFromMap;
      cloud_map.cloud_surf_less_flat = laserCloudSurfFromMap;
      scan_curr.cloud_corner_less_sharp = laserCloudCornerLastStack;
      scan_curr.cloud_surf_less_flat = laserCloudSurfLastStack;
      MappingScanMatcher::Match(cloud_map, scan_curr, &pose_map_scan2world_);
    } else {
      LOG(WARNING) << "[MAP] time Map corner and surf num are not enough";
    }
    transformUpdate();

    TicToc t_add;

    hybrid_grid_map_corner_.InsertScan(
        TransformPointCloud(laserCloudCornerLastStack, pose_map_scan2world_),
        downsize_filter_corner_);

    hybrid_grid_map_surf_.InsertScan(
        TransformPointCloud(laserCloudSurfLastStack, pose_map_scan2world_),
        downsize_filter_surf_);

    LOG_STEP_TIME("MAP", "add points", t_add.toc());
    LOG_STEP_TIME("MAP", "whole mapping", t_whole.toc());

    // publish surround map for every 5 frame
    if (frame_idx_cur_ % 5 == 0) {
      PointCloudPtr laserCloudSurround(new PointCloud);
      *laserCloudSurround += *laserCloudCornerFromMap;
      *laserCloudSurround += *laserCloudSurfFromMap;

      sensor_msgs::PointCloud2 laserCloudSurround3;
      pcl::toROSMsg(*laserCloudSurround, laserCloudSurround3);
      laserCloudSurround3.header.stamp = ToRos(odom_result.timestamp);
      laserCloudSurround3.header.frame_id = "/camera_init";
      cloud_surround_publisher_.publish(laserCloudSurround3);
    }

    nav_msgs::Odometry aftmapped_odom;
    aftmapped_odom.header.frame_id = "/camera_init";
    aftmapped_odom.header.stamp = ToRos(odom_result.timestamp);
    aftmapped_odom.child_frame_id = "/aft_mapped";
    aftmapped_odom.pose = ToRos(pose_map_scan2world_);
    aftmapped_odom_publisher_.publish(aftmapped_odom);

    geometry_msgs::PoseStamped laserAfterMappedPose;
    laserAfterMappedPose.header = aftmapped_odom.header;
    laserAfterMappedPose.pose = aftmapped_odom.pose.pose;
    aftmapped_path_.header.stamp = aftmapped_odom.header.stamp;
    aftmapped_path_.header.frame_id = "/camera_init";
    aftmapped_path_.poses.push_back(laserAfterMappedPose);
    aftmapped_path_publisher_.publish(aftmapped_path_);

    gps_fusion_handler_->AddLocalPose(odom_result.timestamp,
                                      pose_map_scan2world_);

#ifdef _SIM_GPS
    /**
     * Simulate GPS data for GPS fusion
     */
    if (frame_idx_cur_ % 50 == 0) {
      static std::default_random_engine g;
      static std::uniform_real_distribution<double> dist(-1.0, 1.0);
      static Quaternion<double> rotation(
          Eigen::AngleAxis<double>(dist(g) * M_PI, Eigen::Vector3d::UnitZ()));
      Rigid3d pose(
          Vector<double>(1.0, 3.3, 4.2) +
              0.01 / std::sqrt(3) * Vector<double>(dist(g), dist(g), dist(g)),
          rotation);
      gps_fusion_handler_->AddFixedPoint(
          odom_result.timestamp + std::chrono::milliseconds(int(dist(g) * 50)),
          pose * pose_map_scan2world_.translation());
    }
#endif

    PublishScan(odom_result);

    tf::Transform transform;
    transform.setOrigin({pose_map_scan2world_.translation().x(),
                         pose_map_scan2world_.translation().y(),
                         pose_map_scan2world_.translation().z()});
    transform.setRotation({pose_map_scan2world_.rotation().x(),
                           pose_map_scan2world_.rotation().y(),
                           pose_map_scan2world_.rotation().z(),
                           pose_map_scan2world_.rotation().w()});
    transform_broadcaster_.sendTransform(tf::StampedTransform(
        transform, aftmapped_odom.header.stamp, "/camera_init", "/aft_mapped"));

    frame_idx_cur_++;
  }
}

void LaserMapping::AddImu(const ImuData &imu_data) {
  // TODO
  // calib imu and lidar
  LOG(FATAL) << "AddIMU not implemented yet.";
}

void LaserMapping::PublishScan(const TimestampedPointCloud &scan) {
  sensor_msgs::PointCloud2 laser_cloud_out_msg;
  pcl::toROSMsg(*scan.cloud_full_res, laser_cloud_out_msg);
  laser_cloud_out_msg.header.stamp = ToRos(scan.timestamp);
  laser_cloud_out_msg.header.frame_id = "/aft_mapped";
  cloud_scan_publisher_.publish(laser_cloud_out_msg);

  sensor_msgs::PointCloud2 cloud_corner_sharp_msg;
  pcl::toROSMsg(*scan.cloud_corner_sharp, cloud_corner_sharp_msg);
  cloud_corner_sharp_msg.header.stamp = ToRos(scan.timestamp);
  cloud_corner_sharp_msg.header.frame_id = "/aft_mapped";
  cloud_corner_publisher_.publish(cloud_corner_sharp_msg);

  sensor_msgs::PointCloud2 cloud_corner_less_sharp_msg;
  pcl::toROSMsg(*scan.cloud_corner_less_sharp, cloud_corner_less_sharp_msg);
  cloud_corner_less_sharp_msg.header.stamp = ToRos(scan.timestamp);
  cloud_corner_less_sharp_msg.header.frame_id = "/aft_mapped";
  cloud_corner_less_publisher_.publish(cloud_corner_less_sharp_msg);

  sensor_msgs::PointCloud2 cloud_surf_flat_msg;
  pcl::toROSMsg(*scan.cloud_surf_flat, cloud_surf_flat_msg);
  cloud_surf_flat_msg.header.stamp = ToRos(scan.timestamp);
  cloud_surf_flat_msg.header.frame_id = "/aft_mapped";
  cloud_surf_publisher_.publish(cloud_surf_flat_msg);

  sensor_msgs::PointCloud2 cloud_surf_less_flat_msg;
  pcl::toROSMsg(*scan.cloud_surf_less_flat, cloud_surf_less_flat_msg);
  cloud_surf_less_flat_msg.header.stamp = ToRos(scan.timestamp);
  cloud_surf_less_flat_msg.header.frame_id = "/aft_mapped";
  cloud_surf_less_publisher_.publish(cloud_surf_less_flat_msg);
}
