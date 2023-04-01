//
// Created by ljt666666 on 22-10-9.
//

#include "../include/forecast_node.h"
#include <pluginlib/class_list_macros.h>
#include <ros/callback_queue.h>

using namespace std;

PLUGINLIB_EXPORT_CLASS(rm_forecast::Forecast_Node, nodelet::Nodelet)

namespace rm_forecast
{
void Forecast_Node::onInit()
{
  ros::NodeHandle& nh = getMTPrivateNodeHandle();
  this->status_change_srv_ = nh.advertiseService("status_switch", &Forecast_Node::changeStatusCB, this);
  static ros::CallbackQueue my_queue;
  nh.setCallbackQueue(&my_queue);
  initialize(nh);
  my_thread_ = std::thread([]() {
    ros::SingleThreadedSpinner spinner;
    spinner.spin(&my_queue);
  });
}

void Forecast_Node::initialize(ros::NodeHandle& nh)
{
  nh_ = ros::NodeHandle(nh, "rm_forecast");
  it_ = make_shared<image_transport::ImageTransport>(nh_);

  ROS_INFO("starting ProcessorNode!");

  // Kalman Filter initial matrix
  // A - state transition matrix
  // clang-format off
        Eigen::Matrix<double, 6, 6> f;
        f <<  1,  0,  0, dt_, 0,  0,
                0,  1,  0,  0, dt_, 0,
                0,  0,  1,  0,  0, dt_,
                0,  0,  0,  1,  0,  0,
                0,  0,  0,  0,  1,  0,
                0,  0,  0,  0,  0,  1;
  // clang-format on

  // H - measurement matrix
  Eigen::Matrix<double, 3, 6> h;
  h.setIdentity(); /***把矩阵左上角3x3赋值为对角为1其余为0***/

  // Q - process noise covariance matrix
  Eigen::DiagonalMatrix<double, 6> q;
  q.diagonal() << 0.01, 0.01, 0.01, 0.1, 0.1, 0.1;

  // R - measurement noise covariance matrix
  Eigen::DiagonalMatrix<double, 3> r;
  r.diagonal() << 0.05, 0.05, 0.05;

  // P - error estimate covariance matrix
  Eigen::DiagonalMatrix<double, 6> p;
  p.setIdentity();

  kf_matrices_ = KalmanFilterMatrices{ f, h, q, r, p }; /***初始化卡尔曼滤波初始参数***/

  if (!nh.getParam("max_match_distance", max_match_distance_))
    ROS_WARN("No max match distance specified");
  ROS_INFO("66%lf", max_match_distance_);
  if (!nh.getParam("tracking_threshold", tracking_threshold_))
    ROS_WARN("No tracking threshold specified");
  if (!nh.getParam("lost_threshold", lost_threshold_))
    ROS_WARN("No lost threshold specified");

  if (!nh.getParam("max_jump_angle", max_jump_angle_))
    ROS_WARN("No max_jump_angle specified");
  if (!nh.getParam("max_jump_period", max_jump_period_))
    ROS_WARN("No max_jump_period_ specified");
  if (!nh.getParam("allow_following_range", allow_following_range_))
    ROS_WARN("No allow_following_range specified");
  if (!nh.getParam("y_thred", y_thred_))
    ROS_WARN("No y_thred specified");
  if (!nh.getParam("time_thred", time_thred_))
    ROS_WARN("No time_thred specified");
  if (!nh.getParam("allow_following_range", allow_following_range_))
    ROS_WARN("No allow_following_range specified");

  XmlRpc::XmlRpcValue xml_rpc_value;
  if (!nh.getParam("interpolation_fly_time", xml_rpc_value))
    ROS_ERROR("Fly time no defined (namespace: %s)", nh.getNamespace().c_str());
  else
    interpolation_fly_time_.init(xml_rpc_value);

  tracker_ = std::make_unique<Tracker>(kf_matrices_);

  spin_observer_ = std::make_unique<SpinObserver>();

  forecast_cfg_srv_ = new dynamic_reconfigure::Server<rm_forecast::ForecastConfig>(ros::NodeHandle(nh_, "rm_forecast"));
  forecast_cfg_cb_ = boost::bind(&Forecast_Node::forecastconfigCB, this, _1, _2);
  forecast_cfg_srv_->setCallback(forecast_cfg_cb_);

  tf_buffer_ = new tf2_ros::Buffer(ros::Duration(10));
  tf_listener_ = new tf2_ros::TransformListener(*tf_buffer_);
  ROS_INFO("armor type is : %d", armor_type_);

  enemy_targets_sub_ = nh.subscribe("/detection", 1, &Forecast_Node::speedCallback, this);
  outpost_targets_sub_ = nh.subscribe("/detection", 1, &Forecast_Node::outpostCallback, this);
  track_pub_ = nh.advertise<rm_msgs::TrackData>("/track", 10);
  suggest_fire_pub_ = nh.advertise<std_msgs::Bool>("suggest_fire", 1);
  fly_time_sub_ =
      nh.subscribe("/controllers/gimbal_controller/bullet_solver/fly_time", 10, &Forecast_Node::flyTimeCB, this);
}

void Forecast_Node::forecastconfigCB(rm_forecast::ForecastConfig& config, uint32_t level)
{
  //          target_type_ = config.target_color;
  /// track
  max_match_distance_ = config.max_match_distance;
  tracking_threshold_ = config.tracking_threshold;
  lost_threshold_ = config.lost_threshold;

  /// spin_observer
  max_jump_angle_ = config.max_jump_angle;
  max_jump_period_ = config.max_jump_period;
  allow_following_range_ = config.allow_following_range;

  /// outpost
  forecast_readied_ = config.forecast_readied;
  min_target_quantity_ = config.min_target_quantity;
  y_thred_ = config.y_thred;
  time_thred_ = config.time_thred;
  time_offset_ = config.time_offset;
}

void Forecast_Node::outpostCallback(const rm_msgs::TargetDetectionArray::Ptr& msg)
{
  if (!armor_type_)
    return;

  // Initialize track data
  rm_msgs::TrackData track_data;
  track_data.header.frame_id = "base_link";
  track_data.header.stamp = target_array_.header.stamp;
  track_data.id = 0;

  double duration = (ros::Time::now() - last_min_time_).toSec();
  std_msgs::Bool circle_suggest_fire;
  circle_suggest_fire.data = false;

  if (abs(fly_time_ - duration) < time_thred_)
  {
    circle_suggest_fire.data = true;
  }
  suggest_fire_pub_.publish(circle_suggest_fire);

  // No target
  if (msg->detections.empty())
  {
    track_pub_.publish(track_data);
    return;
  }

  // Tranform armor position from image frame to world coordinate
  this->target_array_.detections.clear();
  target_array_.header = msg->header;
  for (const auto& detection : msg->detections)
  {
    rm_msgs::TargetDetection detection_temp;
    geometry_msgs::PoseStamped pose_in;
    geometry_msgs::PoseStamped pose_out;
    pose_in.header.frame_id = msg->header.frame_id;
    pose_in.header.stamp = msg->header.stamp;
    pose_in.pose = detection.pose;
    try
    {
      geometry_msgs::TransformStamped transform =
          tf_buffer_->lookupTransform("base_link", pose_in.header.frame_id, msg->header.stamp, ros::Duration(1));
      tf2::doTransform(pose_in.pose, pose_out.pose, transform);
    }
    catch (tf2::TransformException& ex)
    {
      ROS_WARN("%s", ex.what());
    }

    detection_temp.id = detection.id;
    detection_temp.confidence = detection.confidence;
    detection_temp.pose = pose_out.pose;
    target_array_.detections.emplace_back(detection_temp);
  }

  if (forecast_readied_)
  {
    geometry_msgs::PoseStamped pose_in;
    geometry_msgs::PoseStamped pose_out;
    pose_in.header.frame_id = "base_link";
    pose_in.header.stamp = msg->header.stamp;
    pose_in.pose = target_array_.detections[0].pose;
    try
    {
      geometry_msgs::TransformStamped transform =
          tf_buffer_->lookupTransform("base_link", pose_in.header.frame_id, msg->header.stamp, ros::Duration(1));
      tf2::doTransform(pose_in.pose, pose_out.pose, transform);
    }
    catch (tf2::TransformException& ex)
    {
      ROS_WARN("%s", ex.what());
    }
    target_array_.detections[0].pose = pose_out.pose;

    geometry_msgs::Quaternion qua2;
    qua2.x = msg->detections[0].pose.orientation.x;
    qua2.y = msg->detections[0].pose.orientation.y;
    qua2.z = msg->detections[0].pose.orientation.z;
    qua2.w = msg->detections[0].pose.orientation.w;

    double roll, pitch, yaw;
    quatToRPY(qua2, roll, pitch, yaw);

    if (std::abs(pitch) < y_thred_)
    {
      min_distance_x_ = target_array_.detections[0].pose.position.x;
      min_distance_y_ = target_array_.detections[0].pose.position.y;
      min_distance_z_ = target_array_.detections[0].pose.position.z;

      target_quantity_++;
      if (target_quantity_ > min_target_quantity_)
      {
        target_quantity_ = 0;
        last_min_time_ = ros::Time::now();
      }
    }

    fly_time_ = (time_offset_ - bullet_solver_fly_time_ - 0.239);

    track_data.id = 6;
    track_data.header.frame_id = "base_link";
    track_data.target_pos.x = min_distance_x_;
    track_data.target_pos.y = min_distance_y_;
    track_data.target_pos.z = min_distance_z_;

    track_data.target_vel.x = 0;
    track_data.target_vel.y = 0;
    track_data.target_vel.z = 0;
  }
  else
  {
    track_data.id = 7;
    track_data.target_pos.x = target_array_.detections[0].pose.position.x;
    track_data.target_pos.y = target_array_.detections[0].pose.position.y;
    track_data.target_pos.z = target_array_.detections[0].pose.position.z;
    track_data.target_vel.x = 0;
    track_data.target_vel.y = 0;
    track_data.target_vel.z = 0;
  }

  track_pub_.publish(track_data);
}

void Forecast_Node::speedCallback(const rm_msgs::TargetDetectionArray::Ptr& msg)
{
  if (armor_type_)
    return;

  rm_msgs::TrackData track_data;
  track_data.header.stamp = msg->header.stamp;

  if (msg->detections.empty())
  {
    track_data.id = 0;
    track_pub_.publish(track_data);
    return;
  }

  // Tranform armor position from image frame to world coordinate
  this->target_array_.detections.clear();
  target_array_.header = msg->header;
  for (const auto& detection : msg->detections)
  {
    rm_msgs::TargetDetection detection_temp;
    geometry_msgs::PoseStamped pose_in;
    geometry_msgs::PoseStamped pose_out;
    pose_in.header.frame_id = msg->header.frame_id;
    pose_in.header.stamp = msg->header.stamp;
    pose_in.pose = detection.pose;
    try
    {
      geometry_msgs::TransformStamped transform =
          tf_buffer_->lookupTransform("odom", pose_in.header.frame_id, msg->header.stamp, ros::Duration(1));
      tf2::doTransform(pose_in.pose, pose_out.pose, transform);
    }
    catch (tf2::TransformException& ex)
    {
      ROS_WARN("%s", ex.what());
    }

    detection_temp.id = detection.id;
    detection_temp.confidence = detection.confidence;
    detection_temp.pose = pose_out.pose;
    target_array_.detections.emplace_back(detection_temp);
  }

  /***如果是丢失状态则初始化tracker***/
  if (tracker_->tracker_state == Tracker::LOST)
  {
    tracker_->init(target_array_);
    tracking_ = false;
  }
  /***是其他状态则更新tracker***/
  else
  {
    // Set dt
    dt_ = (msg->header.stamp - last_time_).toSec();
    // Update state
    tracker_->update(target_array_, dt_, max_match_distance_, tracking_threshold_, lost_threshold_);
    /***判断是否处于跟踪状态 ***/
    if (tracker_->tracker_state == Tracker::DETECTING)
    {
      tracking_ = false;
    }
    else if (tracker_->tracker_state == Tracker::TRACKING || tracker_->tracker_state == Tracker::TEMP_LOST)
    {
      tracking_ = true;
    }
  }
  last_time_ = msg->header.stamp;

  if (tracking_)
  {
    //            target_msg.position.x = tracker_->target_state(0);
    //            target_msg.position.y = tracker_->target_state(1);
    //            target_msg.position.z = tracker_->target_state(2);
    //            target_msg.velocity.x = tracker_->target_state(3);
    //            target_msg.velocity.y = tracker_->target_state(4);
    //            target_msg.velocity.z = tracker_->target_state(5);
    track_data.header.frame_id = "odom";
    track_data.header.stamp = msg->header.stamp;  //??
    track_data.id = tracker_->tracking_id;
    track_data.target_pos.x = tracker_->target_state(0);
    track_data.target_pos.y = tracker_->target_state(1);
    track_data.target_pos.z = tracker_->target_state(2);
    track_data.target_vel.x = tracker_->target_state(3);
    track_data.target_vel.y = tracker_->target_state(4);
    track_data.target_vel.z = tracker_->target_state(5);
  }

  /***根据观察旋转的结果决定是否建议开火***/
  if (allow_spin_observer_ && spin_observer_)
  {
    //        spin_observer_->max_jump_angle =
    //        get_parameter("spin_observer.max_jump_angle").as_double();
    //        spin_observer_->max_jump_period =
    //        get_parameter("spin_observer.max_jump_period").as_double();
    //        spin_observer_->allow_following_range =
    //        get_parameter("spin_observer.allow_following_range").as_double();

    spin_observer_->update(track_data, msg->header.stamp, max_jump_angle_, max_jump_period_, allow_following_range_);
    //        spin_info_pub_->publish(spin_observer_->spin_info_msg);
  }

  track_pub_.publish(track_data);
}

geometry_msgs::Pose Forecast_Node::computeCircleCenter(const rm_msgs::TargetDetection point_1,
                                                       const rm_msgs::TargetDetection point_2,
                                                       const rm_msgs::TargetDetection point_3,
                                                       const rm_msgs::TargetDetection point_4)
{
  std::vector<double> p1 = { point_1.pose.position.x, point_1.pose.position.y, point_1.pose.position.z };
  std::vector<double> p2 = { point_2.pose.position.x, point_2.pose.position.y, point_2.pose.position.z };
  std::vector<double> p3 = { point_3.pose.position.x, point_3.pose.position.y, point_3.pose.position.z };
  std::vector<double> p4 = { point_4.pose.position.x, point_4.pose.position.y, point_4.pose.position.z };

  geometry_msgs::Pose center{};
  double a = p1[0] - p2[0], b = p1[1] - p2[1], c = p1[2] - p2[2];
  double a1 = p3[0] - p4[0], b1 = p3[1] - p4[1], c1 = p3[2] - p3[2];
  double a2 = p2[0] - p3[0], b2 = p2[1] - p3[1], c2 = p2[2] - p3[2];
  double D = a * b1 * c2 + a2 * b * c1 + c * a1 * b2 - (a2 * b1 * c + a1 * b * c2 + a * b2 * c1);

  if (D == 0)
  {
    return center;
  }

  double A = p1[0] * p1[0] - p2[0] * p2[0];
  double B = p1[1] * p1[1] - p2[1] * p2[1];
  double C = p1[2] * p1[2] - p2[2] * p2[2];
  double A1 = p3[0] * p3[0] - p4[0] * p4[0];
  double B1 = p3[1] * p3[1] - p4[1] * p4[1];
  double C1 = p3[2] * p3[2] - p4[2] * p4[2];
  double A2 = p2[0] * p2[0] - p3[0] * p3[0];
  double B2 = p2[1] * p2[1] - p3[1] * p3[1];
  double C2 = p2[2] * p2[2] - p3[2] * p3[2];
  double P = (A + B + C) / 2;
  double Q = (A1 + B1 + C1) / 2;
  double R = (A2 + B2 + C2) / 2;

  double Dx = P * b1 * c2 + b * c1 * R + c * Q * b2 - (c * b1 * R + P * c1 * b2 + Q * b * c2);
  double Dy = a * Q * c2 + P * c1 * a2 + c * a1 * R - (c * Q * a2 + a * c1 * R + c2 * P * a1);
  double Dz = a * b1 * R + b * Q * a2 + P * a1 * b2 - (a2 * b1 * P + a * Q * b2 + R * b * a1);

  center.position.x = Dx / D;
  center.position.y = Dy / D;
  center.position.z = Dz / D;

  return center;
}

void Forecast_Node::flyTimeCB(const std_msgs::Float64ConstPtr& msg)
{
  bullet_solver_fly_time_ = msg->data;
}

bool Forecast_Node::changeStatusCB(rm_msgs::StatusChange::Request& change, rm_msgs::StatusChange::Response& res)
{
  this->armor_type_ = change.armor_target == 1;
  //->use_id_cls_ = change.use_id_classification;
  ROS_INFO("armor type is : %d", armor_type_);
  res.switch_is_success = true;
  return true;
}

}  // namespace rm_forecast
