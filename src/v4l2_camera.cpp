// Copyright 2019 Bold Hearts
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ros2_v4l2_camera/v4l2_camera.hpp"

#include <rclcpp/rclcpp.hpp>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <sensor_msgs/image_encodings.hpp>

using namespace ros2_v4l2_camera;
using namespace sensor_msgs::msg;

V4l2Camera::V4l2Camera(std::string device)
: device_{std::move(device)}
{
}

bool V4l2Camera::open()
{
  fd_ = ::open(device_.c_str(), O_RDWR);

  if (fd_ < 0)
  {
    auto msg = std::ostringstream{};
    msg << "Failed opening device " << device_ << ": " << strerror(errno) << " (" << errno << ")";
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"), msg.str());
    return false;
  }

  // List capabilities
  ioctl(fd_, VIDIOC_QUERYCAP, &capabilities_);

  auto canRead = capabilities_.capabilities & V4L2_CAP_READWRITE;
  auto canStream = capabilities_.capabilities & V4L2_CAP_STREAMING;
  
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), std::string{"Driver: "} + (char*)capabilities_.driver);
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), std::string{"Device: "} + (char*)capabilities_.card);
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), "Capabilities:");
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), std::string{"  Read/write: "} + (canRead ? "YES" : "NO"));
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), std::string{"  Streaming:  "} + (canStream ? "YES" : "NO"));

  // Get current data (pixel) format
  auto formatReq = v4l2_format{};
  formatReq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(fd_, VIDIOC_G_FMT, &formatReq);
  cur_data_format_ = PixelFormat{formatReq.fmt.pix};

  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), "Current pixel format: " + cur_data_format_.pixelFormatString());

  // List all available image formats and controls
  listImageFormats();
  listControls();
  
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), "Available image formats: ");
  for (auto const& format : image_formats_) {
    RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), "  "  + format.description);
  }
  
  RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"), "Available controls: ");
  for (auto const& control : controls_) {
    RCLCPP_INFO(rclcpp::get_logger("v4l2_camera"),
      "  "  + control.name + " (" + std::to_string(static_cast<unsigned>(control.type)) + ") = " +
      std::to_string(getControlValue(control.id)));
  }
  
  return true;
}

bool V4l2Camera::start()
{
  if (!initMemoryMapping())
    return false;

  // Queue the buffers
  for (auto const& buffer : buffers_)
  {
    auto buf = v4l2_buffer{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buffer.index;

    if (-1 == ioctl(fd_, VIDIOC_QBUF, &buf))
    {
      RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"),
        std::string{"Buffer failure on capture start: "}
        + strerror(errno) + " (" + std::to_string(errno) + ")");
      return false;
    }
  }

  // Start stream
  unsigned type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == ioctl(fd_, VIDIOC_STREAMON, &type))
  {
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"),
      std::string{"Failed stream start: "}
      + strerror(errno) + " (" + std::to_string(errno) + ")");
    return false;
  }
  return true;
}

bool V4l2Camera::stop()
{
  // Stop stream
  unsigned type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == ioctl(fd_, VIDIOC_STREAMOFF, &type))
  {
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"),
      std::string{"Failed stream stop"});
    return false;
  }

  return true;
}

Image V4l2Camera::capture()
{
  auto buf = v4l2_buffer{};

  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  // Dequeue buffer with new image
  if (-1 == ioctl(fd_, VIDIOC_DQBUF, &buf))
  {
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"),
      std::string{"Error dequeueing buffer: "}
      + strerror(errno) + " (" + std::to_string(errno) + ")");
    return Image{};
  }

  // Requeue buffer to be reused for new captures
  if (-1 == ioctl(fd_, VIDIOC_QBUF, &buf))
  {
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"),
      std::string{"Error re-queueing buffer: "}
      + strerror(errno) + " (" + std::to_string(errno) + ")");
    return Image{};
  }

  // Create image object
  auto img = Image{};
  img.width = cur_data_format_.width;
  img.height = cur_data_format_.height;
  img.step = cur_data_format_.bytesPerLine;
  if (cur_data_format_.pixelFormat == V4L2_PIX_FMT_YUYV)
    img.encoding = sensor_msgs::image_encodings::YUV422;
  else
  {
    RCLCPP_WARN(rclcpp::get_logger("v4l2_camera"), "Current pixel format is not supported yet");
  }
  img.data.resize(cur_data_format_.imageByteSize);

  auto const& buffer = buffers_[buf.index];
  std::copy(buffer.start, buffer.start + img.data.size(), img.data.begin());
  return img;
}

int32_t V4l2Camera::getControlValue(uint32_t id) {
  auto ctrl = v4l2_control{};
  ctrl.id = id;
  if (-1 == ioctl(fd_, VIDIOC_G_CTRL, &ctrl))
  {
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"),
      std::string{"Failed getting value for control "} + std::to_string(id)
      + ": " + strerror(errno) + " (" + std::to_string(errno) + "); returning 0!");
    return 0;
  }
  return ctrl.value;
}

void V4l2Camera::listImageFormats()
{
  image_formats_.clear();

  struct v4l2_fmtdesc fmtDesc;
  fmtDesc.index = 0;
  fmtDesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  while (ioctl(fd_, VIDIOC_ENUM_FMT, &fmtDesc) == 0)
  {
    image_formats_.emplace_back(fmtDesc);
    fmtDesc.index++;
  }
}

void V4l2Camera::listControls()
{
  controls_.clear();

  auto queryctrl = v4l2_queryctrl{};
  queryctrl.id = V4L2_CID_USER_CLASS | V4L2_CTRL_FLAG_NEXT_CTRL;

  while (ioctl(fd_, VIDIOC_QUERYCTRL, &queryctrl) == 0)
  {
    // Ignore disabled controls
    if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
      continue;

    auto menuItems = std::map<int, std::string>{};
    if (queryctrl.type == (unsigned)ControlType::MENU)
    {
      auto querymenu = v4l2_querymenu{};
      querymenu.id = queryctrl.id;

      // Query all enum values
      for (auto i = queryctrl.minimum; i <= queryctrl.maximum; i++)
      {
        querymenu.index = i;
        if (ioctl(fd_, VIDIOC_QUERYMENU, &querymenu) == 0)
          menuItems[i] = (const char*)querymenu.name;
      }
    }

    auto control = Control{};
    control.id = queryctrl.id;
    control.name = std::string{reinterpret_cast<char*>(queryctrl.name)};
    control.type = static_cast<ControlType>(queryctrl.type);
    control.minimum = queryctrl.minimum;
    control.maximum = queryctrl.maximum;
    control.defaultValue = queryctrl.default_value;
    control.menuItems = std::move(menuItems);
    
    controls_.push_back(control);

    // Get ready to query next item
    queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
  }

}

bool V4l2Camera::initMemoryMapping()
{
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));

  // Request 4 buffers
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  ioctl(fd_, VIDIOC_REQBUFS, &req);

  // Didn't get more than 1 buffer
  if (req.count < 2)
  {
    RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"), "Insufficient buffer memory");
    return false;
  }

  buffers_ = std::vector<Buffer>(req.count);

  for (auto i = 0u; i < req.count; ++i)
  {
    auto buf = v4l2_buffer{};

    buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory      = V4L2_MEMORY_MMAP;
    buf.index       = i;

    ioctl(fd_, VIDIOC_QUERYBUF, &buf);

    buffers_[i].index = buf.index;
    buffers_[i].length = buf.length;
    buffers_[i].start =
      static_cast<unsigned char*>(
        mmap(NULL /* start anywhere */,
          buf.length,
          PROT_READ | PROT_WRITE /* required */,
          MAP_SHARED /* recommended */,
          fd_, buf.m.offset));

    if (MAP_FAILED == buffers_[i].start)
    {
      RCLCPP_ERROR(rclcpp::get_logger("v4l2_camera"), "Failed mapping device memory");
      return false;
    }
  }

  return true;
}
