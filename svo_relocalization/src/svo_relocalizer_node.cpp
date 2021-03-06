// This file is part of SVO - Semi-direct Visual Odometry.
//
// Copyright (C) 2014 Christian Forster <forster at ifi dot uzh dot ch>
// (Robotics and Perception Group, University of Zurich, Switzerland).
//
// SVO is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or any later version.
//
// SVO is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <ros/package.h>
#include <string>
#include <svo/frame.h>
#include <svo/frame_handler_mono.h>
#include <svo/feature.h> //Quim
#include <svo/point.h> //Quim
#include <svo/map.h>
#include <svo/config.h>
#include <svo_ros/visualizer.h>
#include <vikit/params_helper.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/String.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <image_transport/image_transport.h>
#include <boost/thread.hpp>
#include <cv_bridge/cv_bridge.h>
#include <Eigen/Core>
#include <vikit/abstract_camera.h>
#include <vikit/camera_loader.h>
#include <vikit/user_input_thread.h>

#include <svo/feature_detection.h>
#include <svo_relocalization/multiple_relocalizer.h>
#include <svo_relocalization/cc_place_finder.h>
#include <svo_relocalization/esm_relpos_finder.h>
#include <svo_relocalization/5pt_relpos_finder.h>
#include <svo_relocalization/3pt_relpos_finder.h>
#include <svo_relocalization/empty_relpos_finder.h>
#include <svo_relocalization/yaml_parse.h>
#include <svo_relocalization/svo_reloc_utils.h>

namespace svo {

using namespace std;
using namespace vk;

/// SVO Interface
class VoNode
{
public:
  svo::FrameHandlerMono* vo_;
  svo::Visualizer visualizer_;
  bool publish_markers_;                 //!< publish only the minimal amount of info (choice for embedded devices)
  bool publish_dense_input_;
  vk::UserInputThread* user_input_thread_;
  ros::Subscriber sub_remote_key_;
  string remote_input_;
  vk::AbstractCamera* cam_;
  bool quit_;

  void writeMapToFile();
  //Relocalizer
  reloc::MultipleRelocalizer *relocalizer_;
  map<int,bool> frames_is_saved;
  VoNode();
  ~VoNode();
  void imgCb(const sensor_msgs::ImageConstPtr& msg);
  void processUserActions();
  void remoteKeyCb(const std_msgs::StringConstPtr& key_input);


};

VoNode::
VoNode() :
  vo_(NULL),
  publish_markers_(vk::getParam<bool>("svo/publish_markers", true)),
  publish_dense_input_(vk::getParam<bool>("svo/publish_dense_input", false)),
  user_input_thread_(new vk::UserInputThread()),
  remote_input_(""),
  cam_(NULL),
  quit_(false)
{

  // Create Camera
  if(!vk::camera_loader::loadFromRosNs("svo", cam_))
    throw std::runtime_error("Camera model not correctly specified.");

  // Create relocalizer
  reloc::AbstractPlaceFinderSharedPtr place_finder (new reloc::CCPlaceFinder());

  reloc::ESMRelposFinder *ESM_rpf = new reloc::ESMRelposFinder(cam_);
  ESM_rpf->options_.pyr_lvl_ = 3;
  ESM_rpf->options_.n_iter_se2_to_se3_ = 3;

  reloc::FivePtRelposFinder *five_rpf = new reloc::FivePtRelposFinder(cam_);
  five_rpf->options_.pyr_lvl_ = 4;
  reloc::ThreePtRelposFinder *three_rpf = new reloc::ThreePtRelposFinder(cam_);
  three_rpf->options_.pyr_lvl_ = 4;
  reloc::EmptyRelposFinder *empty_rpf = new reloc::EmptyRelposFinder();

  reloc::AbstractRelposFinderSharedPtr relpos_finder (three_rpf);

  relocalizer_ = new reloc::MultipleRelocalizer (place_finder, relpos_finder);

  // Set initial position and orientation
  visualizer_.T_world_from_vision_ = Sophus::SE3(
      vk::rpy2dcm(Vector3d(vk::getParam<double>("svo/init_rx", 0.0),
                           vk::getParam<double>("svo/init_ry", 0.0),
                           vk::getParam<double>("svo/init_rz", 0.0))),
      Eigen::Vector3d(vk::getParam<double>("svo/init_tx", 0.0),
                      vk::getParam<double>("svo/init_ty", 0.0),
                      vk::getParam<double>("svo/init_tz", 0.0)));
  
  // Init camera
  vo_ = new svo::FrameHandlerMono(cam_);
  vo_->start();
}


VoNode::
~VoNode()
{
  delete vo_;
  delete user_input_thread_;
  delete cam_;
  delete relocalizer_;
}

void VoNode::writeMapToFile()
{
  list<FramePtr> frames_to_save;

  for (
      auto it_frame = vo_->map().keyframes_.cbegin();
      it_frame != vo_->map().keyframes_.cend();
      ++it_frame)
  {
    if (frames_is_saved.find((*it_frame)->id_) == frames_is_saved.end() &&
        !frames_is_saved[(*it_frame)->id_])
    {
      frames_is_saved[(*it_frame)->id_] = true;
      frames_to_save.push_back((*it_frame));
    }
  }

  writeYaml("/tmp/yaml_test/", frames_to_save);
}

void VoNode::
imgCb(const sensor_msgs::ImageConstPtr& msg)
{
  cv::Mat img;
  try {
    img = cv_bridge::toCvShare(msg, "mono8")->image;
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
  }
  processUserActions();

  if (vo_->stage() == FrameHandlerMono::STAGE_RELOCALIZING)
  {
    FramePtr frame(new Frame(cam_, img, msg->header.stamp.toSec()));

    feature_detection::FastDetector detector(
        img.cols,
        img.rows,
        Config::gridSize(),
        Config::nPyrLevels());

    detector.detect(
        frame.get(),
        frame->img_pyr_,
        Config::triangMinCornerScore(),
        frame->fts_);
    
    // Create new reloc frame from the image (create pyr)
    reloc::FrameSharedPtr data(new reloc::Frame());
    svo2reloc(frame, data);

    // run relocalize
    int found_id;
    Sophus::SE3 pose_out;
    relocalizer_->relocalize(data, pose_out, found_id);

    cout << "Found T_query_template" << endl << pose_out << endl;

    vo_->relocalizeFrameAtPose(
        found_id,
        pose_out,
        img,
        msg->header.stamp.toSec());
  }
  else
  {
    vo_->addImage(img, msg->header.stamp.toSec());

    if(vo_->lastFrame() != NULL && vo_->lastFrame()->isKeyframe())
    {
      // Create frame from svo frame
      FramePtr frame = vo_->lastFrame();
      reloc::FrameSharedPtr data(new reloc::Frame());
      svo2reloc(frame, data);


      if (data->features_.size() > 0)
      { 
        std::cout << "Adding frame to reloc" << std::endl;
        relocalizer_->addFrame(data);
      }
    }
  }

  visualizer_.publishMinimal(img, vo_->lastFrame(), *vo_, msg->header.stamp.toSec());

  if(publish_markers_ && vo_->stage() != FrameHandlerBase::STAGE_PAUSED)
    visualizer_.visualizeMarkers(vo_->lastFrame(), vo_->coreKeyframes(), vo_->map());



  if(vo_->stage() == FrameHandlerMono::STAGE_PAUSED)
    usleep(100000); // avoid busy loop when paused
}

void VoNode::processUserActions()
{
  char input = remote_input_.c_str()[0];
  remote_input_ = "";

  char console_input = user_input_thread_->getInput();
  if(console_input != 0)
    input = console_input;

  switch(input)
  {
    case 'q':
      quit_ = true;
      printf("Svo User Input: QUIT\n");
      break;
    case 'r':
      vo_->reset();
      printf("Svo User Input: RESET\n");
      break;
    case 's':
      vo_->start();
      printf("Svo User Input: START\n");
      break;
    case 'w':
      writeMapToFile();
      printf("Svo User Input: WRITE\n");
      break;
    default: ;
  }
}

void VoNode::remoteKeyCb(const std_msgs::StringConstPtr& key_input)
{
  remote_input_ = key_input->data;
}

} // namespace svo

int main(int argc, char **argv)
{
  ros::init(argc, argv, "svo");
  ros::NodeHandle nh;
  svo::VoNode vo_node;

  // subscribe to cam msgs
  std::string cam_topic(vk::getParam<std::string>("svo/cam_topic", "camera/image_raw"));
  image_transport::ImageTransport it(nh);
  image_transport::Subscriber it_sub = it.subscribe(cam_topic, 5, &svo::VoNode::imgCb, &vo_node);

  // subscribe to remote input
  vo_node.sub_remote_key_ = nh.subscribe("svo/remote_key", 5, &svo::VoNode::remoteKeyCb, &vo_node);

  // start processing callbacks
  while(ros::ok() && !vo_node.quit_)
  {
    ros::spinOnce();
    // TODO check when last image was processed. when too long ago. publish warning that no msgs are received!
  }

  printf("Svo terminated.\n");
  return 0;
}
