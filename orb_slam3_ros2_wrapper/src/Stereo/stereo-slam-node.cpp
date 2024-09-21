/**
 * @file stereo-slam-node.cpp
 * @brief Implementation of the StereoSlamNode Wrapper class.
 * @author Shivam Sharma (ssharma4@wpi.edu)
 */

#include "stereo-slam-node.hpp"
#include <opencv2/core/core.hpp>

namespace ORB_SLAM3_Wrapper
{
    StereoSlamNode::StereoSlamNode(const std::string &strVocFile,
                                   const std::string &strSettingsFile,
                                   ORB_SLAM3::System::eSensor sensor)
        : Node("ORB_SLAM3_STEREO_ROS2")
    {
        // Declare parameters (topic names)
        this->declare_parameter("left_image_topic_name", rclcpp::ParameterValue("left/image_raw"));
        this->declare_parameter("right_image_topic_name", rclcpp::ParameterValue("right/image_raw"));
        this->declare_parameter("imu_topic_name", rclcpp::ParameterValue("imu"));
        this->declare_parameter("odom_topic_name", rclcpp::ParameterValue("odom"));
        this->declare_parameter("pose_topic_name", rclcpp::ParameterValue("pose"));
        // ROS Subscribers
        leftSub_ = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, this->get_parameter("left_image_topic_name").as_string());
        rightSub_ = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, this->get_parameter("right_image_topic_name").as_string());
        syncApproximate_ = std::make_shared<message_filters::Synchronizer<approximate_sync_policy>>(approximate_sync_policy(10), *leftSub_, *rightSub_);
        syncApproximate_->registerCallback(&StereoSlamNode::StereoCallback, this);
        imuSub_ = this->create_subscription<sensor_msgs::msg::Imu>(this->get_parameter("imu_topic_name").as_string(), 1000, std::bind(&StereoSlamNode::ImuCallback, this, std::placeholders::_1));
        odomSub_ = this->create_subscription<nav_msgs::msg::Odometry>(this->get_parameter("odom_topic_name").as_string(), 1000, std::bind(&StereoSlamNode::OdomCallback, this, std::placeholders::_1));
        // ROS Publishers
        mapDataPub_ = this->create_publisher<slam_msgs::msg::MapData>("map_data", 10);
        mapPointsPub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("map_points", 10);
        visibleLandmarksPub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("visible_landmarks", 10);
        visibleLandmarksPose_ = this->create_publisher<geometry_msgs::msg::PoseStamped>("visible_landmarks_pose", 10);
        // Services
        getMapDataService_ = this->create_service<slam_msgs::srv::GetMap>("orb_slam3_get_map_data", std::bind(&StereoSlamNode::getMapServer, this,
                                                                                                              std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        getMapPointsService_ = this->create_service<slam_msgs::srv::GetLandmarksInView>("orb_slam3_get_landmarks_in_view", std::bind(&StereoSlamNode::getMapPointsInViewServer, this,
                                                                                                                                     std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        // TF
        tfBroadcaster_ = std::make_shared<tf2_ros::TransformBroadcaster>(this);
        tfBuffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
        tfListener_ = std::make_shared<tf2_ros::TransformListener>(*tfBuffer_);

        bool bUseViewer;
        this->declare_parameter("visualization", rclcpp::ParameterValue(true));
        this->get_parameter("visualization", bUseViewer);

        this->declare_parameter("ros_visualization", rclcpp::ParameterValue(true));
        this->get_parameter("ros_visualization", rosViz_);

        this->declare_parameter("robot_base_frame", "base_link");
        this->get_parameter("robot_base_frame", robot_base_frame_id_);

        this->declare_parameter("global_frame", "map");
        this->get_parameter("global_frame", global_frame_);

        this->declare_parameter("odom_frame", "odom");
        this->get_parameter("odom_frame", odom_frame_id_);

        this->declare_parameter("robot_x", rclcpp::ParameterValue(1.0));
        this->get_parameter("robot_x", robot_x_);

        this->declare_parameter("robot_y", rclcpp::ParameterValue(1.0));
        this->get_parameter("robot_y", robot_y_);

        this->declare_parameter("no_odometry_mode", rclcpp::ParameterValue(true));
        this->get_parameter("no_odometry_mode", no_odometry_mode_);

        this->declare_parameter("publish_tf", rclcpp::ParameterValue(true));
        this->get_parameter("publish_tf", publish_tf_);

        this->declare_parameter("map_data_publish_frequency", rclcpp::ParameterValue(1000));
        this->get_parameter("map_data_publish_frequency", map_data_publish_frequency_);

        this->declare_parameter("landmark_publish_frequency", rclcpp::ParameterValue(1000));
        this->get_parameter("landmark_publish_frequency", landmark_publish_frequency_);

        // Timers
        mapDataCallbackGroup_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
        mapDataTimer_ = this->create_wall_timer(std::chrono::milliseconds(map_data_publish_frequency_), std::bind(&StereoSlamNode::publishMapData, this), mapDataCallbackGroup_);
        if (rosViz_)
        {
            mapPointsCallbackGroup_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
            mapPointsTimer_ = this->create_wall_timer(std::chrono::milliseconds(landmark_publish_frequency_), std::bind(&StereoSlamNode::publishMapPointCloud, this), mapDataCallbackGroup_);
        }

        interface_ = std::make_shared<ORB_SLAM3_Wrapper::ORBSLAM3Interface>(strVocFile, strSettingsFile,
                                                                            sensor, bUseViewer, rosViz_, robot_x_,
                                                                            robot_y_, global_frame_, odom_frame_id_, robot_base_frame_id_);

        frequency_tracker_count_ = 0;
        frequency_tracker_clock_ = std::chrono::high_resolution_clock::now();

        RCLCPP_INFO(this->get_logger(), "CONSTRUCTOR END!");
    }

    // Destructor
    StereoSlamNode::~StereoSlamNode()
    {
        leftSub_.reset();
        rightSub_.reset();
        imuSub_.reset();
        odomSub_.reset();
        interface_.reset();
        RCLCPP_INFO(this->get_logger(), "DESTRUCTOR!");
    }
    void StereoSlamNode::ImuCallback(const sensor_msgs::msg::Imu::SharedPtr msgIMU)
    {
        RCLCPP_DEBUG_STREAM(this->get_logger(), "ImuCallback");
        // push value to imu buffer.
        interface_->handleIMU(msgIMU);
    }

    void StereoSlamNode::OdomCallback(const nav_msgs::msg::Odometry::SharedPtr msgOdom)
    {
        if (!no_odometry_mode_ && publish_tf_)
        {
            RCLCPP_DEBUG_STREAM(this->get_logger(), "OdomCallback");
            interface_->getMapToOdomTF(msgOdom, tfMapOdom_);
        }
        else
            RCLCPP_WARN_THROTTLE(this->get_logger(), *this->get_clock(), 4000, "Odometry msg recorded but no odometry mode is true, set to false to use this odometry");
    }

    void StereoSlamNode::StereoCallback(const sensor_msgs::msg::Image::SharedPtr msgLeft, const sensor_msgs::msg::Image::SharedPtr msgRight)
    {
        Sophus::SE3f Tcw;
        // print to consolse using ros2 logger that this function is running
        RCLCPP_INFO(this->get_logger(), "StereoCallback");
        // also do cout
        std::cout << "StereoCallback" << std::endl;
        if (interface_->trackStereo(msgLeft, msgRight, Tcw))
        { // check if tracked
            RCLCPP_INFO(this->get_logger(), "Tracked!");
            // also do cout
            std::cout << "Tracked!" << std::endl;
            isTracked_ = true;
            if (publish_tf_)
            {
                if (no_odometry_mode_)
                    interface_->getDirectMapToRobotTF(msgLeft->header, tfMapOdom_);
                tfBroadcaster_->sendTransform(tfMapOdom_);
            }
            ++frequency_tracker_count_;
        }
    }

    void StereoSlamNode::publishMapPointCloud()
    {
        // print to consolse using ros2 logger that this function is running
        RCLCPP_INFO(this->get_logger(), "publishMapPointCloud");
        // also do cout
        std::cout << "publishMapPointCloud" << std::endl;
        if (isTracked_)
        {
            // cout or print to console that this function is running
            //  also do cout
            std::cout << "publishMapPointCloudrunning" << std::endl;
            // using high_resolution_clock to measure time
            auto start = std::chrono::high_resolution_clock::now();
            sensor_msgs::msg::PointCloud2 mapPCL;
            auto t1 = std::chrono::high_resolution_clock::now();
            auto time_create_mapPCL = std::chrono::duration<double, std::milli>(t1 - start).count();
            RCLCPP_DEBUG_STREAM(this->get_logger(), "Time to create mapPCL object: " << time_create_mapPCL << " seconds");

            interface_->getCurrentMapPoints(mapPCL);

            if (mapPCL.data.size() == 0)
            {
                RCLCPP_DEBUG_STREAM(this->get_logger(), "mapPCL.data.size() == 0");
                // also do cout
                std::cout << "mapPCL.data.size() == 0" << std::endl;

                return;
            }

            // print the size of the mapPCL
            //  also do cout
            std::cout << "mapPCL.data.size() == " << mapPCL.data.size() << std::endl;

            auto t2 = std::chrono::high_resolution_clock::now();
            auto time_get_map_points = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1).count();
            RCLCPP_DEBUG_STREAM(this->get_logger(), "Time to get current map points: " << time_get_map_points << " seconds");

            mapPointsPub_->publish(mapPCL);
            auto t3 = std::chrono::high_resolution_clock::now();
            auto time_publish_map_points = std::chrono::duration_cast<std::chrono::duration<double>>(t3 - t2).count();
            RCLCPP_DEBUG_STREAM(this->get_logger(), "Time to publish map points: " << time_publish_map_points << " seconds");
            RCLCPP_DEBUG_STREAM(this->get_logger(), "=======================");

            // Calculate the time taken for each line

            // Print the time taken for each line
        }
    }

    void StereoSlamNode::publishMapData()
    {
        // print to consolse using ros2 logger that this function is running
        RCLCPP_DEBUG_STREAM(this->get_logger(), "publishMapData");
        // also do cout
        std::cout << "publishMapData" << std::endl;
        if (isTracked_)
        {
            // cout or print to console that this function is running
            //  also do cout
            std::cout << "publishMapDatarunning" << std::endl;

            auto start = std::chrono::high_resolution_clock::now();
            RCLCPP_DEBUG_STREAM(this->get_logger(), "Publishing map data");
            RCLCPP_INFO_STREAM(this->get_logger(), "Current ORB-SLAM3 tracking frequency: " << frequency_tracker_count_ / std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::high_resolution_clock::now() - frequency_tracker_clock_).count() << " frames / sec");
            frequency_tracker_clock_ = std::chrono::high_resolution_clock::now();
            frequency_tracker_count_ = 0;
            // publish the map data (current active keyframes etc)
            slam_msgs::msg::MapData mapDataMsg;
            interface_->mapDataToMsg(mapDataMsg, true, false);
            mapDataPub_->publish(mapDataMsg);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto time_publishMapData = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - start).count();
            RCLCPP_DEBUG_STREAM(this->get_logger(), "Time to create mapdata: " << time_publishMapData << " seconds");
            RCLCPP_INFO_STREAM(this->get_logger(), "*************************");
        }
    }

    void StereoSlamNode::getMapServer(std::shared_ptr<rmw_request_id_t> request_header,
                                      std::shared_ptr<slam_msgs::srv::GetMap::Request> request,
                                      std::shared_ptr<slam_msgs::srv::GetMap::Response> response)
    {
        RCLCPP_INFO(this->get_logger(), "GetMap2 service called.");
        slam_msgs::msg::MapData mapDataMsg;
        interface_->mapDataToMsg(mapDataMsg, false, request->tracked_points, request->kf_id_for_landmarks);
        response->data = mapDataMsg;
    }

    void StereoSlamNode::getMapPointsInViewServer(std::shared_ptr<rmw_request_id_t> request_header,
                                                  std::shared_ptr<slam_msgs::srv::GetLandmarksInView::Request> request,
                                                  std::shared_ptr<slam_msgs::srv::GetLandmarksInView::Response> response)
    {
        RCLCPP_INFO(this->get_logger(), "GetMapPointsInView service called.");
        std::cout << "GetMapPointsInView service called." << endl;
        std::vector<slam_msgs::msg::MapPoint> landmarks;
        std::vector<ORB_SLAM3::MapPoint *> points;
        interface_->mapPointsVisibleFromPose(request->pose, points, 1000, 5.0, 2.0);
        // Populate the pose of the points vector into the ros message
        for (const auto &point : points)
        {
            slam_msgs::msg::MapPoint landmark;
            Eigen::Vector3f landmark_position = point->GetWorldPos();
            auto position = interface_->getTypeConversionPtr()->vector3fORBToROS(landmark_position);
            landmark.position.x = position.x();
            landmark.position.y = position.y();
            landmark.position.z = position.z();
            landmarks.push_back(landmark);
        }
        response->map_points = landmarks;
        auto cloud = interface_->getTypeConversionPtr()->MapPointsToPCL(points);
        std::cout << "Cloud size: " << cloud.width * cloud.height << std::endl;
        visibleLandmarksPub_->publish(cloud);

        // Convert the pose in request to PoseStamped and publish
        geometry_msgs::msg::PoseStamped pose_stamped;
        pose_stamped.header.stamp = this->now();
        pose_stamped.header.frame_id = "map"; // Assuming the frame is "map", adjust if needed
        pose_stamped.pose = request->pose;

        // Publish the PoseStamped
        visibleLandmarksPose_->publish(pose_stamped);
    }
}