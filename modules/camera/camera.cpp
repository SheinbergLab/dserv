/*
 * NAME
 *   camera.cpp
 *
 * DESCRIPTION
 *   Raspberry Pi Camera Module 3 interface for data server
 *   Provides Tcl commands for camera initialization and image capture
 *   Builds on all platforms but only functional with libcamera support
 *
 * AUTHOR
 *   Adapted for Tcl data server module
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <cstring>
#include <sstream>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <algorithm>

// Tcl and dataserver headers
extern "C" {
#include <tcl.h>
#include "Datapoint.h"
}
#include "TclServer.h"

// Add JPEG type if not already defined
#ifndef DSERV_JPEG
#define DSERV_JPEG 14
#endif

// Forward declaration
class CameraCapture;

// Camera info structure used by both implementations
typedef struct camera_info_s {
  CameraCapture *capture;
  TclServer *tclserver;
  char *dpoint_prefix;
  int camera_index;  // Which camera to use
  int initialized;
  int configured;
  int jpeg_quality;
  int available;
} camera_info_t;

/*****************************************************************************
 * LIBCAMERA IMPLEMENTATION
 * Full implementation when libcamera is available
 *****************************************************************************/
#ifdef HAS_LIBCAMERA

#include <chrono>
#include <thread>
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#include <libcamera/libcamera.h>
#include <libcamera/control_ids.h>

#ifdef HAS_JPEG
#include <jpeglib.h>
#endif

using namespace libcamera;
using namespace std::chrono_literals;

enum class CameraState {
    IDLE,
    STREAMING,
    CAPTURING
};

class CameraCapture {
private:
  std::unique_ptr<CameraManager> cm_;
  std::shared_ptr<Camera> camera_;
  std::unique_ptr<CameraConfiguration> config_;
  std::unique_ptr<FrameBufferAllocator> allocator_;
  std::vector<std::unique_ptr<Request>> requests_;
  
  std::atomic<CameraState> state_{CameraState::IDLE};
  std::atomic<bool> capture_complete_{false};
  std::atomic<int> frames_captured_{0};
  std::vector<uint8_t> image_data_;
  std::vector<uint8_t> jpeg_data_;
  Stream *stream_ = nullptr;
  
  // Camera parameters
  unsigned int width_ = 1920;
  unsigned int height_ = 1080;
  int settling_frames_ = 10;  // Reduced from 30
  float brightness_ = 0.0f;
  float contrast_ = 1.0f;
  int jpeg_quality_ = 85;
  
  // Threading and synchronization
  std::mutex capture_mutex_;
  std::condition_variable capture_cv_;
  std::mutex state_mutex_;
  bool frame_ready_ = false;
  int frame_skip_counter_ = 0;
  int frame_skip_rate_ = 1;
  
  // Auto-exposure tracking
  std::atomic<bool> ae_settled_{false};
  int ae_settle_count_ = 0;
  static constexpr int AE_SETTLE_FRAMES = 5;  // Frames to wait for AE to settle
  
  // Continuous mode parameters
  bool continuous_mode_ = false;
  bool save_to_disk_ = false;
  bool publish_to_dataserver_ = false;
  bool use_tcl_callback_ = false;
  std::string save_directory_ = "/tmp/camera_frames/";
  std::string datapoint_prefix_ = "camera";
  std::string tcl_callback_proc_ = "";
  std::atomic<int> frame_counter_{0};
  int publish_interval_ = 1;  // Publish every Nth frame

  // FPS management
  double target_fps_ = 0.0;  // 0 = use camera default
  double configured_fps_ = 0.0;
  bool hardware_fps_supported_ = false;
  double actual_fps_ = 0.0;  // Measured FPS for fallback calculations
  bool software_throttling_active_ = false;
  std::chrono::steady_clock::time_point last_frame_time_;
  std::chrono::microseconds target_frame_interval_{0};
  
  // Tcl interpreter for callbacks
  Tcl_Interp *tcl_interp_ = nullptr;
  TclServer *tclserver_ = nullptr;
  
  struct CameraFrameBuffer {
    std::vector<uint8_t> jpeg_data;
    int frame_id;
    int64_t timestamp_ms;
    bool valid;
    
    CameraFrameBuffer() : frame_id(-1), timestamp_ms(0), valid(false) {}
};
  
	static constexpr int RING_BUFFER_SIZE = 16;
	std::array<CameraFrameBuffer, RING_BUFFER_SIZE> frame_ring_buffer_;
	std::atomic<int> ring_write_index_{0};
	std::mutex ring_buffer_mutex_;

  
  // Background save thread for continuous mode
  std::queue<std::pair<std::vector<uint8_t>, std::string>> save_queue_;
  std::thread save_worker_thread_;
  std::mutex save_queue_mutex_;
  std::atomic<bool> save_worker_running_{false};

public:
  CameraCapture() {
    cm_ = std::make_unique<CameraManager>();
  }

  ~CameraCapture() {
    stop_continuous_mode();
    cleanup();
  }

  void set_tclserver(TclServer *server) { 
    tclserver_ = server; 
  }
  
  bool initialize(int index = -1) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    int ret = cm_->start();
    if (ret) return false;
    
    auto cameras = cm_->cameras();
    if (cameras.empty()) return false;
    
    // Use specified index or default to 0
    int use_index = (index >= 0 && index < cameras.size()) ? index : 0;
    
    std::cout << "Using camera " << use_index << ": " 
              << cameras[use_index]->id() << std::endl;
    
    camera_ = cm_->get(cameras[use_index]->id());

    if (!camera_) {
      return false;
    }

    ret = camera_->acquire();
    if (ret) {
      return false;
    }

    return true;
  }

bool configure(unsigned int width, unsigned int height) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  
  if (state_ != CameraState::IDLE) {
    std::cerr << "Cannot configure camera while in use" << std::endl;
    return false;
  }
  
  width_ = width;
  height_ = height;
  ae_settled_ = false;
  ae_settle_count_ = 0;
      
  config_ = camera_->generateConfiguration({ StreamRole::StillCapture });
  if (!config_ || config_->size() == 0) {
    return false;
  }

  StreamConfiguration &stream_config = config_->at(0);
  
  // Check what formats are available
  const StreamFormats &formats = stream_config.formats();
  auto format_list = formats.pixelformats();
  
  std::cout << "Available formats for this camera:" << std::endl;
  for (const auto& format : format_list) {
    std::cout << "  " << format.toString() << std::endl;
  }
  
  // Detect camera type based on ID and available formats
  std::string camera_id = camera_->id();
  bool is_usb_camera = (camera_id.find("PCI0") != std::string::npos ||  // USB via PCI
                       camera_id.find("usb") != std::string::npos);      // Direct USB
  bool is_csi_camera = (camera_id.find("csi") != std::string::npos ||   // CSI interface
                       camera_id.find("ov") != std::string::npos ||      // OmniVision sensors
                       camera_id.find("imx") != std::string::npos);      // Sony IMX sensors
  
  std::cout << "Camera ID: " << camera_id << std::endl;
  std::cout << "Detected as: " << (is_usb_camera ? "USB" : is_csi_camera ? "CSI" : "Unknown") << " camera" << std::endl;
  
  PixelFormat preferred_format;
  bool found_format = false;
  
  if (is_usb_camera) {
    // USB cameras: Prefer MJPEG (already compressed), fall back to YUV formats, avoid RGB888
    std::cout << "Using USB camera strategy..." << std::endl;
    
    if (std::find(format_list.begin(), format_list.end(), formats::MJPEG) != format_list.end()) {
      preferred_format = formats::MJPEG;
      found_format = true;
      std::cout << "Selected MJPEG (optimal for USB cameras)" << std::endl;
    }
    else if (std::find(format_list.begin(), format_list.end(), formats::YUYV) != format_list.end()) {
      preferred_format = formats::YUYV;
      found_format = true;
      std::cout << "Selected YUYV (good for USB cameras)" << std::endl;
    }
    else if (std::find(format_list.begin(), format_list.end(), formats::RGB888) != format_list.end()) {
      preferred_format = formats::RGB888;
      found_format = true;
      std::cout << "Selected RGB888 (may be slow for USB)" << std::endl;
    }
  } else {
    // CSI/Pi cameras: Prefer RGB888 (ISP processed), avoid compressed formats
    std::cout << "Using CSI camera strategy..." << std::endl;
    
    if (std::find(format_list.begin(), format_list.end(), formats::RGB888) != format_list.end()) {
      preferred_format = formats::RGB888;
      found_format = true;
      std::cout << "Selected RGB888 (optimal for CSI cameras)" << std::endl;
    }
    else if (std::find(format_list.begin(), format_list.end(), formats::YUV420) != format_list.end()) {
      preferred_format = formats::YUV420;
      found_format = true;
      std::cout << "Selected YUV420 (good for CSI cameras)" << std::endl;
    }
    else if (std::find(format_list.begin(), format_list.end(), formats::MJPEG) != format_list.end()) {
      preferred_format = formats::MJPEG;
      found_format = true;
      std::cout << "Selected MJPEG (fallback for CSI)" << std::endl;
    }
  }
  
  // Last resort: use first available format
  if (!found_format && !format_list.empty()) {
    preferred_format = format_list[0];
    found_format = true;
    std::cout << "WARNING: Using first available format " << preferred_format.toString() 
              << " - may cause issues!" << std::endl;
  }

  if (!found_format) {
    std::cerr << "No pixel formats available" << std::endl;
    return false;
  }

  stream_config.size.width = width;
  stream_config.size.height = height;
  stream_config.pixelFormat = preferred_format;
  stream_config.bufferCount = 4;

  CameraConfiguration::Status validation = config_->validate();
  if (validation == CameraConfiguration::Invalid) {
    std::cerr << "Camera configuration invalid" << std::endl;
    return false;
  }
  
  if (validation == CameraConfiguration::Adjusted) {
    std::cout << "Configuration adjusted:" << std::endl;
    std::cout << "  Size: " << stream_config.size.width << "x" << stream_config.size.height << std::endl;
    std::cout << "  Format: " << stream_config.pixelFormat.toString() << std::endl;
    
    // Update our stored dimensions
    width_ = stream_config.size.width;
    height_ = stream_config.size.height;
  }

  int ret = camera_->configure(config_.get());
  if (ret) {
    std::cerr << "Camera configure failed: " << ret << std::endl;
    return false;
  }

  stream_ = stream_config.stream();
  return allocate_buffers();
}

bool encode_jpeg() {
#ifdef HAS_JPEG
  if (!stream_) {
    return false;
  }
  
  if (image_data_.empty()) {
    return false;
  }
  
  const StreamConfiguration &cfg = stream_->configuration();
  
  // If already MJPEG, just copy directly (your existing logic)
  if (cfg.pixelFormat == formats::MJPEG) {
    jpeg_data_ = image_data_;
    return true;
  }
  
  // For RGB888, use your existing JPEG compression code
  if (cfg.pixelFormat == formats::RGB888) {
    return encode_rgb888_to_jpeg(cfg);
  }
  
  // For YUYV (common USB format), convert to RGB first then compress
  if (cfg.pixelFormat == formats::YUYV) {
    std::vector<uint8_t> rgb_data;
    if (!convert_yuyv_to_rgb(image_data_, rgb_data, cfg.size.width, cfg.size.height)) {
      std::cerr << "Failed to convert YUYV to RGB" << std::endl;
      return false;
    }
    
    // Temporarily swap the data for encoding
    std::vector<uint8_t> original_data = image_data_;
    image_data_ = rgb_data;
    
    bool result = encode_rgb888_to_jpeg(cfg);
    
    // Restore original data
    image_data_ = original_data;
    return result;
  }
  
  // Add more formats as needed
  std::cerr << "JPEG encoding not supported for format: " << cfg.pixelFormat.toString() << std::endl;
  return false;
#else
  return false;  // No JPEG support
#endif
}

// Extract your existing JPEG compression code into a separate method
bool encode_rgb888_to_jpeg(const StreamConfiguration &cfg) {
#ifdef HAS_JPEG
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
      
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
      
  unsigned char *jpeg_buffer = nullptr;
  unsigned long jpeg_size = 0;
      
  jpeg_mem_dest(&cinfo, &jpeg_buffer, &jpeg_size);
      
  cinfo.image_width = cfg.size.width;
  cinfo.image_height = cfg.size.height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
      
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, jpeg_quality_, TRUE);
      
  jpeg_start_compress(&cinfo, TRUE);
      
  JSAMPROW row_pointer[1];
  int row_stride = cfg.size.width * 3;
      
  while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = &image_data_[cinfo.next_scanline * row_stride];
    jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }
      
  jpeg_finish_compress(&cinfo);
      
  jpeg_data_.clear();
  jpeg_data_.resize(jpeg_size);
  std::memcpy(jpeg_data_.data(), jpeg_buffer, jpeg_size);
      
  if (jpeg_buffer) {
    free(jpeg_buffer);
  }
  jpeg_destroy_compress(&cinfo);
      
  return true;
#else
  return false;
#endif
}

// YUYV to RGB conversion helper
bool convert_yuyv_to_rgb(const std::vector<uint8_t>& yuyv_data, 
                        std::vector<uint8_t>& rgb_data,
                        unsigned int width, unsigned int height) {
  
  if (yuyv_data.size() < width * height * 2) {
    std::cerr << "YUYV data too small" << std::endl;
    return false;
  }
  
  rgb_data.resize(width * height * 3);
  
  for (unsigned int i = 0; i < height; i++) {
    for (unsigned int j = 0; j < width; j += 2) {
      unsigned int yuyv_idx = (i * width + j) * 2;
      unsigned int rgb_idx1 = (i * width + j) * 3;
      unsigned int rgb_idx2 = (i * width + j + 1) * 3;
      
      if (yuyv_idx + 3 >= yuyv_data.size()) break;
      
      uint8_t y1 = yuyv_data[yuyv_idx];
      uint8_t u  = yuyv_data[yuyv_idx + 1];
      uint8_t y2 = yuyv_data[yuyv_idx + 2];
      uint8_t v  = yuyv_data[yuyv_idx + 3];
      
      // Convert YUV to RGB for both pixels
      yuv_to_rgb(y1, u, v, rgb_data[rgb_idx1], rgb_data[rgb_idx1+1], rgb_data[rgb_idx1+2]);
      if (j + 1 < width) {
        yuv_to_rgb(y2, u, v, rgb_data[rgb_idx2], rgb_data[rgb_idx2+1], rgb_data[rgb_idx2+2]);
      }
    }
  }
  
  return true;
}

// YUV to RGB color space conversion
void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
  int c = y - 16;
  int d = u - 128;
  int e = v - 128;
  
  int r_val = (298 * c + 409 * e + 128) >> 8;
  int g_val = (298 * c - 100 * d - 208 * e + 128) >> 8;
  int b_val = (298 * c + 516 * d + 128) >> 8;
  
  r = (uint8_t)std::max(0, std::min(255, r_val));
  g = (uint8_t)std::max(0, std::min(255, g_val));
  b = (uint8_t)std::max(0, std::min(255, b_val));
}  

  bool allocate_buffers() {
    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);

    int ret = allocator_->allocate(stream_);
    if (ret < 0) {
      return false;
    }

    // Use libcamera::FrameBuffer correctly
    const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers = 
      allocator_->buffers(stream_);
    
    requests_.clear();
        
    for (unsigned int i = 0; i < buffers.size(); ++i) {
      std::unique_ptr<Request> request = camera_->createRequest();
      if (!request) {
        return false;
      }

      const std::unique_ptr<libcamera::FrameBuffer> &buffer = buffers[i];
      ret = request->addBuffer(stream_, buffer.get());
      if (ret < 0) {
        return false;
      }

      // Set camera controls including FPS if specified
      set_camera_controls(request->controls());
      
      requests_.push_back(std::move(request));
    }
    
    return true;
}
  
  void set_camera_controls(ControlList &controls) {
    const ControlInfoMap &available_controls = camera_->controls();
    
    // Only set controls that exist
    if (available_controls.find(&controls::AeEnable) !=
	available_controls.end()) {
      controls.set(controls::AeEnable, true);
    }
    if (available_controls.find(&controls::AwbEnable) !=
	available_controls.end()) {
      controls.set(controls::AwbEnable, true);
    }
    if (available_controls.find(&controls::AeExposureMode) !=
	available_controls.end()) {
      controls.set(controls::AeExposureMode, controls::ExposureNormal);
    }
    if (available_controls.find(&controls::AeMeteringMode) !=
	available_controls.end()) {
      controls.set(controls::AeMeteringMode, controls::MeteringCentreWeighted);
    }
    if (available_controls.find(&controls::Brightness) !=
	available_controls.end()) {
      controls.set(controls::Brightness, brightness_);
    }
    if (available_controls.find(&controls::Contrast) !=
	available_controls.end()) {
      controls.set(controls::Contrast, contrast_);
    }
    
    // Add FPS control if target_fps_ is set
    if (target_fps_ > 0.0) {
      configured_fps_ = target_fps_;
      if (available_controls.find(&controls::FrameDurationLimits) !=
	  available_controls.end()) {
	int64_t frame_duration_us =
	  static_cast<int64_t>(1000000.0 / target_fps_);
	controls.set(controls::FrameDurationLimits, 
		     Span<const int64_t, 2>({frame_duration_us,
			 frame_duration_us}));
	hardware_fps_supported_ = true;
	software_throttling_active_ = false;
      } else {
	hardware_fps_supported_ = false;
	software_throttling_active_ = true;
      }
    }
  }
  
  bool capture_image() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_ != CameraState::IDLE) {
        std::cerr << "Camera busy - cannot capture" << std::endl;
        return false;
      }
      state_ = CameraState::CAPTURING;
    }
    
    frames_captured_ = 0;
    capture_complete_ = false;
    ae_settled_ = false;
    ae_settle_count_ = 0;
    
    // Disconnect any existing callbacks
    camera_->requestCompleted.disconnect();
    
    // Connect the capture callback
    camera_->requestCompleted.connect(this, &CameraCapture::capture_request_complete);
    
    int ret = camera_->start();
    if (ret) {
      state_ = CameraState::IDLE;
      return false;
    }
    
    for (auto &request : requests_) {
      request->reuse(Request::ReuseBuffers);
      ret = camera_->queueRequest(request.get());
      if (ret) {
        camera_->stop();
        state_ = CameraState::IDLE;
        return false;
      }
    }
    
    // Wait for capture completion with timeout
    auto start = std::chrono::steady_clock::now();
    while (!capture_complete_) {
      std::this_thread::sleep_for(10ms);
      
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed > 10s) {
        std::cerr << "Capture timeout" << std::endl;
        camera_->stop();
        state_ = CameraState::IDLE;
        return false;
      }
    }
    
    camera_->stop();
    state_ = CameraState::IDLE;
    
    return !image_data_.empty();
  }
  
  bool start_streaming() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_ != CameraState::IDLE) {
        std::cerr << "Camera busy - cannot start streaming" << std::endl;
        return false;
      }
      state_ = CameraState::STREAMING;
    }
    
    if (!stream_ || !allocator_) {
      state_ = CameraState::IDLE;
      return false;
    }
    
    frames_captured_ = 0;
    frame_ready_ = false;
    frame_skip_counter_ = 0;
    ae_settled_ = false;
    ae_settle_count_ = 0;
    last_frame_time_ = std::chrono::steady_clock::now();
 
    // Disconnect any existing callbacks
    camera_->requestCompleted.disconnect();
    
    // Connect streaming callback
    camera_->requestCompleted.connect(this, &CameraCapture::streaming_request_complete);
    
    int ret = camera_->start();
    if (ret) {
      std::cerr << "Failed to start camera for streaming" << std::endl;
      state_ = CameraState::IDLE;
      return false;
    }
    
    // Queue initial requests
    for (auto &request : requests_) {
      request->reuse(Request::ReuseBuffers);
      ret = camera_->queueRequest(request.get());
      if (ret) {
        std::cerr << "Failed to queue request for streaming" << std::endl;
        camera_->stop();
        state_ = CameraState::IDLE;
        return false;
      }
    }
    
    // Brief settling period for streaming
    if (settling_frames_ > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(settling_frames_ * 33));
    }
    
    return true;
  }

  bool stop_streaming() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (state_ != CameraState::STREAMING) {
        return true;  // Already stopped
      }
      state_ = CameraState::IDLE;
    }
    
    // Small delay to let callbacks finish processing
    std::this_thread::sleep_for(50ms);
    
    // Disconnect callbacks before stopping
    camera_->requestCompleted.disconnect();
    
    camera_->stop();
    
    return true;
  }
  
  bool grab_frame() {
    if (state_ != CameraState::STREAMING) {
      return false;
    }
    
    // In streaming mode, just return current frame
    std::lock_guard<std::mutex> lock(capture_mutex_);
    return !image_data_.empty();
  }

  // File saving methods
  bool save_ppm(const std::string &filename) {
    if (!stream_ || image_data_.empty()) {
      return false;
    }

    const StreamConfiguration &cfg = stream_->configuration();
        
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
      return false;
    }

    file << "P6\n";
    file << cfg.size.width << " " << cfg.size.height << "\n";
    file << "255\n";
    file.write(reinterpret_cast<const char*>(image_data_.data()), 
               image_data_.size());
    file.close();
    return true;
  }

  bool save_jpeg(const std::string &filename) {
    const StreamConfiguration &cfg = stream_->configuration();
    
    if (cfg.pixelFormat == formats::MJPEG) {
        // Already JPEG - write directly
        std::ofstream file(filename, std::ios::binary);
        if (!file) return false;
        
        file.write(reinterpret_cast<const char*>(image_data_.data()), 
                   image_data_.size());
        file.close();
        return true;
    }
    
    if (!encode_jpeg()) {
      return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file) {
      return false;
    }

    file.write(reinterpret_cast<const char*>(jpeg_data_.data()), 
               jpeg_data_.size());
    file.close();
    return true;
  }


 void streaming_request_complete(Request *request) {
   // std::cerr << "DEBUG: Entering streaming_request_complete" << std::endl;
   
  // Early exit if not streaming
  if (state_ != CameraState::STREAMING) {
    return;
  }
  
  if (request->status() == Request::RequestCancelled) {
    if (state_ == CameraState::STREAMING) {
      request->reuse(Request::ReuseBuffers);
      camera_->queueRequest(request);
    }
    return;
  }
  
  frames_captured_++;
  
  // Check auto-exposure convergence
  check_ae_convergence(request);
  
  // Skip frames if configured and not yet settled
  if (!ae_settled_ || (++frame_skip_counter_ < frame_skip_rate_)) {
    if (frame_skip_counter_ >= frame_skip_rate_) {
      frame_skip_counter_ = 0;
    }
    
    if (state_ == CameraState::STREAMING) {
      request->reuse(Request::ReuseBuffers);
      camera_->queueRequest(request);
    }
    return;
  }
  frame_skip_counter_ = 0;
  
  // Process frame - use libcamera::FrameBuffer correctly
  const Request::BufferMap &buffers = request->buffers();
  for (auto buffer_pair : buffers) {
    libcamera::FrameBuffer *buffer = buffer_pair.second;  // Use libcamera::FrameBuffer
    const libcamera::FrameBuffer::Plane &plane = buffer->planes()[0];  // Use libcamera types
      
    void *data = mmap(nullptr, plane.length, PROT_READ, MAP_SHARED,
                      plane.fd.get(), 0);
    if (data == MAP_FAILED) continue;
      
    {
      std::lock_guard<std::mutex> lock(capture_mutex_);
      image_data_.resize(plane.length);
      std::memcpy(image_data_.data(), data, plane.length);
      frame_ready_ = true;
    }
    capture_cv_.notify_one();
      
    munmap(data, plane.length);
    break;
  }

  if (continuous_mode_ && ae_settled_) {
    // If hardware FPS isn't supported and we have a target FPS, throttle in software
    if (!hardware_fps_supported_ && target_fps_ > 0.0) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = now - last_frame_time_;
      
      if (elapsed < target_frame_interval_) {
        // Skip this frame - too soon for target FPS
        if (state_ == CameraState::STREAMING) {
          request->reuse(Request::ReuseBuffers);
          camera_->queueRequest(request);
        }
        return;
      }
      last_frame_time_ = now;
    }
    
    handle_continuous_frame();
  }
  
  if (state_ == CameraState::STREAMING) {
    request->reuse(Request::ReuseBuffers);
    camera_->queueRequest(request);
  }
 }
  
void capture_request_complete(Request *request) {
    if (request->status() == Request::RequestCancelled) {
      return;
    }
    
    frames_captured_++;
    
    // Check auto-exposure convergence
    check_ae_convergence(request);
    
    // Capture after settling and AE convergence
    if (ae_settled_ && frames_captured_ >= settling_frames_) {
      const Request::BufferMap &buffers = request->buffers();
      
      for (auto buffer_pair : buffers) {
        libcamera::FrameBuffer *buffer = buffer_pair.second;  // Use libcamera::FrameBuffer
        const libcamera::FrameBuffer::Plane &plane = buffer->planes()[0];  // Use libcamera types
        
        const StreamConfiguration &cfg = stream_->configuration();
        
        void *data = mmap(nullptr, plane.length, PROT_READ, MAP_SHARED,
                          plane.fd.get(), 0);
        
        if (data == MAP_FAILED) {
          std::cerr << "mmap failed" << std::endl;
          continue;
        }
        
        // Store the data regardless of format
        image_data_.resize(plane.length);
        std::memcpy(image_data_.data(), data, plane.length);
        munmap(data, plane.length);
        
        // If it's MJPEG, the data is already JPEG compressed
        if (cfg.pixelFormat == formats::MJPEG) {
          jpeg_data_ = image_data_;
        }
        
        capture_complete_ = true;
        return;
      }
    }
    
    // Continue capturing if not done
    if (!capture_complete_) {
      request->reuse(Request::ReuseBuffers);
      camera_->queueRequest(request);
    }
}

void handle_continuous_frame() {
  //   std::cerr << "DEBUG: FIRST LINE of handle_continuous_frame" << std::endl;
  
  // Check if we should process this frame based on interval
  int current_frame = frame_counter_.load();
  int interval = publish_interval_;
  
  if (interval <= 0) {
    std::cerr << "ERROR: Invalid publish_interval_: " << interval << std::endl;
    return;
  }
  
  if ((current_frame % interval) != 0) {
    frame_counter_++;
    return;
  }
  
  //  std::cerr << "DEBUG: Past interval check" << std::endl;
  
  // Add back encode_jpeg() to test
  //  std::cerr << "DEBUG: About to call encode_jpeg()" << std::endl;
  if (!encode_jpeg()) {
    std::cerr << "Failed to encode JPEG for continuous mode" << std::endl;
    frame_counter_++;
    return;
  }
  //  std::cerr << "DEBUG: encode_jpeg() completed successfully" << std::endl;
  
  store_frame_in_ring_buffer();

  // Handle via Tcl callback if specified
  if (use_tcl_callback_ && !tcl_callback_proc_.empty() && tcl_interp_) {
    call_tcl_frame_callback();
  } else {
    // Use built-in handlers
    if (save_to_disk_) {
      queue_frame_for_save();
    }
    
    if (publish_to_dataserver_ && tclserver_) {
      publish_frame_to_dataserver();
    }
  }

  
  frame_counter_++;
  //  std::cerr << "DEBUG: handle_continuous_frame completed" << std::endl;
}
  
  bool start_continuous_mode(bool save_disk, bool publish_dataserver, 
                          const std::string& save_dir,
                          const std::string& datapoint_prefix,
                          int interval) {
  if (state_ != CameraState::STREAMING) {
    std::cerr << "Must be streaming to start continuous mode" << std::endl;
    return false;
  }
  
  continuous_mode_ = true;
  save_to_disk_ = save_disk;
  publish_to_dataserver_ = publish_dataserver;
  use_tcl_callback_ = false;  // Using built-in handlers
  save_directory_ = save_dir;
  datapoint_prefix_ = datapoint_prefix;
  publish_interval_ = std::max(1, interval);
  frame_counter_ = 0;
  
  // Create save directory if needed
  if (save_to_disk_) {
    std::system(("mkdir -p " + save_directory_).c_str());
    start_save_worker();
  }
  
  return true;
}

bool start_continuous_callback_mode(const std::string& tcl_proc,
                                   const std::string& datapoint_prefix,
                                   int interval) {
  if (state_ != CameraState::STREAMING) {
    std::cerr << "Must be streaming to start continuous callback mode" << std::endl;
    return false;
  }
  
  if (tcl_proc.empty() || !tcl_interp_) {
    std::cerr << "Tcl callback proc name required and Tcl interpreter must be set" << std::endl;
    return false;
  }
  
  continuous_mode_ = true;
  save_to_disk_ = false;
  publish_to_dataserver_ = false;
  use_tcl_callback_ = true;
  tcl_callback_proc_ = tcl_proc;
  datapoint_prefix_ = datapoint_prefix;
  publish_interval_ = std::max(1, interval);
  frame_counter_ = 0;
  
  return true;
}

bool stop_continuous_mode() {
  if (!continuous_mode_) return true;
  
  continuous_mode_ = false;
  stop_save_worker();
  return true;
}

bool get_frame_by_id(int frame_id, std::vector<uint8_t> &frame_data, int64_t &timestamp_ms) {
  std::lock_guard<std::mutex> lock(ring_buffer_mutex_);
  
  // Search ring buffer for the requested frame_id
  for (int i = 0; i < RING_BUFFER_SIZE; i++) {
    if (frame_ring_buffer_[i].valid && frame_ring_buffer_[i].frame_id == frame_id) {
      frame_data = frame_ring_buffer_[i].jpeg_data;
      timestamp_ms = frame_ring_buffer_[i].timestamp_ms;
      return true;
    }
  }
  
  return false;  // Frame not found (too old or invalid frame_id)
}

bool save_callback_frame(int frame_id, const std::string& filename) {
  std::lock_guard<std::mutex> lock(ring_buffer_mutex_);
  
  // Find the frame in ring buffer
  for (int i = 0; i < RING_BUFFER_SIZE; i++) {
    if (frame_ring_buffer_[i].valid && frame_ring_buffer_[i].frame_id == frame_id) {
      // Direct write to disk - no intermediate copying
      std::ofstream file(filename, std::ios::binary);
      if (!file) {
        return false;
      }
      
      file.write(reinterpret_cast<const char*>(frame_ring_buffer_[i].jpeg_data.data()),
                 frame_ring_buffer_[i].jpeg_data.size());
      file.close();
      return true;
    }
  }
  
  return false;  // Frame not found
}

bool publish_callback_frame(int frame_id, const std::string& datapoint_name) {
  if (!tclserver_) return false;
  
  std::lock_guard<std::mutex> lock(ring_buffer_mutex_);
  
  // Find the frame in ring buffer
  for (int i = 0; i < RING_BUFFER_SIZE; i++) {
    if (frame_ring_buffer_[i].valid && frame_ring_buffer_[i].frame_id == frame_id) {
      // Safe to pass ring buffer pointer because dpoint_new() makes internal copy
      ds_datapoint_t *dp = dpoint_new(
          (char*)datapoint_name.c_str(),
          frame_ring_buffer_[i].timestamp_ms * 1000, // Convert to microseconds
          (ds_datatype_t)DSERV_JPEG,
          frame_ring_buffer_[i].jpeg_data.size(),
          (unsigned char *)frame_ring_buffer_[i].jpeg_data.data()
      );
      
      tclserver_->set_point(dp);
      return true;
    }
  }
  
  return false;  // Frame not found
}
  
  void get_ring_buffer_status(int &oldest_frame_id, int &newest_frame_id, int &valid_frames) {
    std::lock_guard<std::mutex> lock(ring_buffer_mutex_);
    
    oldest_frame_id = -1;
    newest_frame_id = -1;
    valid_frames = 0;
    
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
      if (frame_ring_buffer_[i].valid) {
	valid_frames++;
	if (oldest_frame_id == -1 || frame_ring_buffer_[i].frame_id < oldest_frame_id) {
	  oldest_frame_id = frame_ring_buffer_[i].frame_id;
	}
	if (newest_frame_id == -1 || frame_ring_buffer_[i].frame_id > newest_frame_id) {
	  newest_frame_id = frame_ring_buffer_[i].frame_id;
	}
      }
    }
  }
  
  void set_target_fps(double fps) { 
    target_fps_ = fps;
    if (fps > 0.0) {
      target_frame_interval_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / fps));
    }
  }
  
  void set_tcl_interp(Tcl_Interp *interp) { 
    tcl_interp_ = interp; 
  }
  
  int get_frame_count() const { 
    return frame_counter_; 
  }
  
bool is_continuous_mode() const { 
  return continuous_mode_; 
}


private:
  void check_ae_convergence(Request *request) {
    // Simple AE convergence check based on frame count
    // In a more sophisticated implementation, you could check
    // the actual exposure values from request metadata
    if (!ae_settled_) {
      ae_settle_count_++;
      if (ae_settle_count_ >= AE_SETTLE_FRAMES) {
        ae_settled_ = true;
        std::cout << "Auto-exposure settled after " << ae_settle_count_ << " frames" << std::endl;
      }
    }
  }

void cleanup() {
  stop_continuous_mode();
  stop_streaming();
  
  if (camera_) {
    camera_->requestCompleted.disconnect();
    if (state_ != CameraState::IDLE) {
      camera_->stop();
    }
    camera_->release();
  }
  
  state_ = CameraState::IDLE;
}

// Helper method for queuing frames for background save
void queue_frame_for_save() {
  // Create filename with timestamp and frame number
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  
  char filename[512];
  snprintf(filename, sizeof(filename), "%s/frame_%06d_%lld.jpg", 
           save_directory_.c_str(), frame_counter_.load(), timestamp);
  
  // Queue for background saving
  {
    std::lock_guard<std::mutex> lock(save_queue_mutex_);
    save_queue_.push({jpeg_data_, std::string(filename)});
  }
}

// Helper method for publishing frames to dataserver
void publish_frame_to_dataserver() {
  if (!tclserver_) return;
  
  // Create datapoint name
  char point_name[256];
  snprintf(point_name, sizeof(point_name), "%s/live_frame", datapoint_prefix_.c_str());
  
  // Create and publish frame datapoint
  ds_datapoint_t *dp = dpoint_new(
      point_name,
      tclserver_->now(),
      (ds_datatype_t)DSERV_JPEG,
      jpeg_data_.size(),
      (unsigned char *)jpeg_data_.data()
  );
  
  tclserver_->set_point(dp);
  
  // Also publish metadata
  publish_frame_metadata(point_name);
}

// Helper method for publishing frame metadata
void publish_frame_metadata(const char* base_name) {
  char meta_name[256];
  snprintf(meta_name, sizeof(meta_name), "%s/meta", base_name);
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  
  char meta_json[512];
  snprintf(meta_json, sizeof(meta_json), 
          "{\"frame_id\":%d,\"timestamp\":%lld,\"width\":%d,\"height\":%d,"
          "\"size\":%zu,\"fps\":%.2f,\"ae_settled\":%s,\"continuous_mode\":%s}",
          frame_counter_.load(), timestamp,
          width_, height_, jpeg_data_.size(),
          target_fps_ > 0 ? target_fps_ : 30.0,
          ae_settled_ ? "true" : "false",
          continuous_mode_ ? "true" : "false");
  
  ds_datapoint_t *meta_dp = dpoint_new(meta_name,
				       tclserver_->now(),
                                      DSERV_STRING,
                                      strlen(meta_json) + 1,
                                      (unsigned char *)meta_json);
  
  tclserver_->set_point(meta_dp);
}
void store_frame_in_ring_buffer() {
  std::lock_guard<std::mutex> lock(ring_buffer_mutex_);
  
  int write_idx = ring_write_index_.load() % RING_BUFFER_SIZE;
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  
  // Store frame data
  frame_ring_buffer_[write_idx].jpeg_data = jpeg_data_;
  frame_ring_buffer_[write_idx].frame_id = frame_counter_.load();
  frame_ring_buffer_[write_idx].timestamp_ms = timestamp;
  frame_ring_buffer_[write_idx].valid = true;
  
  // Advance write index
  ring_write_index_++;
}

void call_tcl_frame_callback() {
  if (!tclserver_ || tcl_callback_proc_.empty()) return;
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  
  // Create a Tcl script that calls the callback
  char tcl_command[2048];
  int len = snprintf(tcl_command, sizeof(tcl_command),
           "%s %d %lld %d %d %zu %s %s",
           tcl_callback_proc_.c_str(),
           frame_counter_.load(),           // frame_id
           timestamp,                       // timestamp_ms
           width_,                         // width
           height_,                        // height
           jpeg_data_.size(),              // jpeg_size
           ae_settled_ ? "true" : "false", // ae_settled
           datapoint_prefix_.c_str()       // datapoint_prefix
  );
  
  if (len >= sizeof(tcl_command)) {
    std::cerr << "Tcl command too long, truncated" << std::endl;
    return;
  }

  // std::cerr << "DEBUG: About to queue tcl command: " << tcl_command << std::endl;
  
  // Use TclServer's existing REQ_SCRIPT_NOREPLY mechanism
  client_request_t req;
  req.type = REQ_SCRIPT_NOREPLY;  // No reply needed for callbacks
  req.script = std::string(tcl_command);
  
  // Push directly to the TclServer queue - this is thread-safe
  tclserver_->queue.push_back(req);
  //  std::cerr << "DEBUG: Tcl command queued successfully" << std::endl;
  
}

void start_save_worker() {
  if (save_worker_running_) return;
  
  save_worker_running_ = true;
  save_worker_thread_ = std::thread(&CameraCapture::save_worker_loop, this);
}

void stop_save_worker() {
  if (!save_worker_running_) return;
  
  save_worker_running_ = false;
  if (save_worker_thread_.joinable()) {
    save_worker_thread_.join();
  }
  
  // Clear any remaining items in queue
  std::lock_guard<std::mutex> lock(save_queue_mutex_);
  while (!save_queue_.empty()) {
    save_queue_.pop();
  }
}

void save_worker_loop() {
  while (save_worker_running_) {
    std::pair<std::vector<uint8_t>, std::string> save_item;
    bool has_item = false;
    
    {
      std::lock_guard<std::mutex> lock(save_queue_mutex_);
      if (!save_queue_.empty()) {
        save_item = save_queue_.front();
        save_queue_.pop();
        has_item = true;
      }
    }
    
    if (has_item) {
      // Write to disk
      std::ofstream file(save_item.second, std::ios::binary);
      if (file) {
        file.write(reinterpret_cast<const char*>(save_item.first.data()), 
                  save_item.first.size());
        file.close();
      } else {
        std::cerr << "Failed to save frame: " << save_item.second << std::endl;
      }
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}
public:
  // Getters and setters
  void set_frame_skip_rate(int rate) {
    frame_skip_rate_ = std::max(1, rate);
  }

  void set_settling_frames(int frames) { 
    settling_frames_ = std::max(0, std::min(frames, 100)); 
  }
  void set_brightness(float b) { brightness_ = b; }
  void set_contrast(float c) { contrast_ = c; }
  void set_resolution(unsigned int w, unsigned int h) { width_ = w; height_ = h; }
  void set_jpeg_quality(int q) { jpeg_quality_ = q; }
    
  unsigned int get_width() const { return width_; }
  unsigned int get_height() const { return height_; }
  size_t get_image_size() const { return image_data_.size(); }
  size_t get_jpeg_size() const { return jpeg_data_.size(); }
    
  const uint8_t* get_jpeg_data() const { return jpeg_data_.data(); }
  const uint8_t* get_rgb_data() const { return image_data_.data(); }
  
  CameraState get_state() const { return state_; }
  bool is_ae_settled() const { return ae_settled_; }
  bool is_hardware_fps_supported() const { return hardware_fps_supported_; }
  bool is_software_throttling_active() const { return software_throttling_active_; }
  double get_configured_fps() const { return configured_fps_; }  
};

/*****************************************************************************
 * STUB IMPLEMENTATION
 * Minimal implementation when libcamera is not available
 *****************************************************************************/
#else  // !HAS_LIBCAMERA

enum class CameraState {
    IDLE,
    STREAMING,
    CAPTURING
};

class CameraCapture {
public:
  CameraCapture() {}
  ~CameraCapture() {}
    
  bool initialize(int index = -1) { return false; }
  bool configure(unsigned int w, unsigned int h) { return false; }
  bool allocate_buffers() { return false; }
  bool capture_image() { return false; }
  bool save_ppm(const std::string &filename) { return false; }
  bool save_jpeg(const std::string &filename) { return false; }
  bool encode_jpeg() { return false; }
    
  void set_settling_frames(int frames) {}
  void set_brightness(float b) {}
  void set_contrast(float c) {}
  void set_resolution(unsigned int w, unsigned int h) {}
  void set_jpeg_quality(int q) {}
    
  unsigned int get_width() const { return 0; }
  unsigned int get_height() const { return 0; }
  size_t get_image_size() const { return 0; }
  size_t get_jpeg_size() const { return 0; }
    
  const uint8_t* get_jpeg_data() const { return nullptr; }
  const uint8_t* get_rgb_data() const { return nullptr; }

  bool start_streaming() { return false; }
  bool stop_streaming() { return false; }
  bool grab_frame() { return false; }
  void set_frame_skip_rate(int rate) {}

  void set_tcl_interp(Tcl_Interp *interp) {}
  void set_tclserver(TclServer *server) {}
  void set_target_fps(double fps) {}
  double get_configured_fps() const { return 0.0; }
  bool is_hardware_fps_supported() const { return false; }
  bool is_software_throttling_active() const { return false; }
  
  CameraState get_state() const { return CameraState::IDLE; }
  bool is_ae_settled() const { return false; }
};

#endif  // HAS_LIBCAMERA

/*****************************************************************************
 * TCL COMMAND IMPLEMENTATIONS
 * These work with either implementation above
 *****************************************************************************/

extern "C" {

  static int camera_list_command(ClientData data,
                                 Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
  {
#ifdef HAS_LIBCAMERA
    CameraManager cm;
    cm.start();
    auto cameras = cm.cameras();
    
    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
    for (size_t i = 0; i < cameras.size(); i++) {
      Tcl_Obj *cam_info = Tcl_NewDictObj();
      Tcl_DictObjPut(interp, cam_info, 
                     Tcl_NewStringObj("index", -1),
                     Tcl_NewIntObj(i));
      Tcl_DictObjPut(interp, cam_info,
                     Tcl_NewStringObj("id", -1),
                     Tcl_NewStringObj(cameras[i]->id().c_str(), -1));
      Tcl_ListObjAppendElement(interp, list, cam_info);
    }
    
    Tcl_SetObjResult(interp, list);
    return TCL_OK;
#else
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
#endif
  }
  
  static int camera_init_command(ClientData data,
                                 Tcl_Interp *interp,
                                 int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    int camera_index = 0; 
    
    if (objc > 1) {
        if (Tcl_GetIntFromObj(interp, objv[1], &camera_index) != TCL_OK)
            return TCL_ERROR;
    }
    
    if (!info->available) {
      Tcl_AppendResult(interp,
                       "Camera support not available on this platform", NULL);
      return TCL_ERROR;
    }
    
    if (info->initialized) {
      Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
      return TCL_OK;
    }
    
    try {
      info->capture = new CameraCapture();
        
      if (!info->capture->initialize(camera_index)) {
        delete info->capture;
        info->capture = nullptr;
        Tcl_AppendResult(interp, "Failed to initialize camera", NULL);
        return TCL_ERROR;
      }

      info->capture->set_tcl_interp(interp);
      info->capture->set_tclserver(info->tclserver);
  
      info->initialized = 1;
      Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
      return TCL_OK;
      
    } catch (const std::exception& e) {
      Tcl_AppendResult(interp, "Exception during initialization: ",
                       e.what(), NULL);
      return TCL_ERROR;
    }
  }

static int camera_configure_command(ClientData data,
                                    Tcl_Interp *interp,
                                    int objc, Tcl_Obj *objv[])
{
  camera_info_t *info = (camera_info_t *) data;
  int width = 1920;
  int height = 1080;
  double fps = 0.0;  // 0 = use camera default
  
  if (!info->available) {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }
  
  if (!info->initialized || !info->capture) {
    Tcl_AppendResult(interp, "Camera not initialized", NULL);
    return TCL_ERROR;
  }
  
  // Check if camera is busy
  if (info->capture->get_state() != CameraState::IDLE) {
    Tcl_AppendResult(interp, "Camera is busy - stop streaming first", NULL);
    return TCL_ERROR;
  }
  
  if (objc > 1) {
    if (Tcl_GetIntFromObj(interp, objv[1], &width) != TCL_OK)
      return TCL_ERROR;
  }
  if (objc > 2) {
    if (Tcl_GetIntFromObj(interp, objv[2], &height) != TCL_OK)
      return TCL_ERROR;
  }
  if (objc > 3) {
    if (Tcl_GetDoubleFromObj(interp, objv[3], &fps) != TCL_OK)
      return TCL_ERROR;
  }

  try {
    // Set target FPS before configuring
    if (fps > 0.0) {
      info->capture->set_target_fps(fps);
    }
    
    if (!info->capture->configure(width, height)) {
      Tcl_AppendResult(interp, "Failed to configure camera", NULL);
      return TCL_ERROR;
    }
      
    info->configured = 1;
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
      
  } catch (const std::exception& e) {
    Tcl_AppendResult(interp, "Exception during configuration: ", e.what(), NULL);
    return TCL_ERROR;
  }
}

  static int camera_capture_command(ClientData data,
                                    Tcl_Interp *interp,
                                    int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available", NULL);
      return TCL_ERROR;
    }
    
    if (!info->configured || !info->capture) {
      Tcl_AppendResult(interp, "Camera not configured", NULL);
      return TCL_ERROR;
    }
    
    // Cannot capture while streaming
    if (info->capture->get_state() == CameraState::STREAMING) {
      Tcl_AppendResult(interp, "Cannot capture while streaming - stop streaming first", NULL);
      return TCL_ERROR;
    }
    
    const char *filename = nullptr;
    if (objc > 1) {
      filename = Tcl_GetString(objv[1]);
    }
    
    try {
      if (!info->capture->capture_image()) {
        Tcl_AppendResult(interp, "Failed to capture image", NULL);
        return TCL_ERROR;
      }
        
      if (filename) {
        std::string fname(filename);
        bool success = false;
            
        if (fname.size() > 4) {
          std::string ext = fname.substr(fname.size() - 4);
          if (ext == ".jpg" || ext == ".jpeg" || 
              fname.substr(fname.size() - 5) == ".jpeg") {
            success = info->capture->save_jpeg(fname);
          } else if (ext == ".ppm") {
            success = info->capture->save_ppm(fname);
          } else {
            success = info->capture->save_jpeg(fname);
          }
        } else {
          success = info->capture->save_jpeg(fname);
        }
            
        if (!success) {
          Tcl_AppendResult(interp, "Failed to save image", NULL);
          return TCL_ERROR;
        }
            
        Tcl_SetObjResult(interp, Tcl_NewStringObj(filename, -1));
      } else {
        Tcl_SetObjResult(interp, Tcl_NewIntObj(info->capture->get_image_size()));
      }
        
      return TCL_OK;
        
    } catch (const std::exception& e) {
      Tcl_AppendResult(interp, "Exception during capture: ", e.what(), NULL);
      return TCL_ERROR;
    }
  }

  static int camera_capture_datapoint_command(ClientData data,
                                              Tcl_Interp *interp,
                                              int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available", NULL);
      return TCL_ERROR;
    }
    
    if (!info->configured || !info->capture) {
      Tcl_AppendResult(interp, "Camera not configured", NULL);
      return TCL_ERROR;
    }
    
    // Cannot capture while streaming
    if (info->capture->get_state() == CameraState::STREAMING) {
      Tcl_AppendResult(interp, "Cannot capture while streaming - stop streaming first", NULL);
      return TCL_ERROR;
    }
    
    const char *point_name = "camera/image";
    if (objc > 1) {
      point_name = Tcl_GetString(objv[1]);
    }
    
    try {
      if (!info->capture->capture_image()) {
        Tcl_AppendResult(interp, "Failed to capture image", NULL);
        return TCL_ERROR;
      }
        
      if (!info->capture->encode_jpeg()) {
        Tcl_AppendResult(interp, "Failed to encode JPEG", NULL);
        return TCL_ERROR;
      }
        
      ds_datapoint_t *dp = dpoint_new((char *)point_name,
                                      info->tclserver->now(),
                                      (ds_datatype_t)DSERV_JPEG,
                                      info->capture->get_jpeg_size(),
                                      (unsigned char *)info->capture->get_jpeg_data());
        
      info->tclserver->set_point(dp);
        
      char meta_name[256];
      snprintf(meta_name, sizeof(meta_name), "%s/meta", point_name);
        
      char meta_str[256];
      snprintf(meta_str, sizeof(meta_str), 
               "{\"width\":%d,\"height\":%d,\"size\":%zu,\"format\":\"jpeg\",\"ae_settled\":%s}",
               info->capture->get_width(),
               info->capture->get_height(),
               info->capture->get_jpeg_size(),
               info->capture->is_ae_settled() ? "true" : "false");
        
      ds_datapoint_t *meta_dp = dpoint_new(meta_name,
                                           info->tclserver->now(),
                                           DSERV_STRING,
                                           strlen(meta_str) + 1,
                                           (unsigned char *)meta_str);
        
      info->tclserver->set_point(meta_dp);
        
      Tcl_SetObjResult(interp, Tcl_NewIntObj(info->capture->get_jpeg_size()));
      return TCL_OK;
        
    } catch (const std::exception& e) {
      Tcl_AppendResult(interp, "Exception during capture: ", e.what(), NULL);
      return TCL_ERROR;
    }
  }

  static int camera_set_settling_frames_command(ClientData data,
                                                Tcl_Interp *interp,
                                                int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    int frames;
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available", NULL);
      return TCL_ERROR;
    }
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "frames");
      return TCL_ERROR;
    }
    
    if (Tcl_GetIntFromObj(interp, objv[1], &frames) != TCL_OK)
      return TCL_ERROR;
    
    if (frames < 0 || frames > 100) {
      Tcl_AppendResult(interp, "Invalid settling frames (0-100)", NULL);
      return TCL_ERROR;
    }
    
    info->capture->set_settling_frames(frames);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(frames));
    return TCL_OK;
  }

  static int camera_set_jpeg_quality_command(ClientData data,
                                             Tcl_Interp *interp,
                                             int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    int quality;
    
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "quality");
      return TCL_ERROR;
    }
    
    if (Tcl_GetIntFromObj(interp, objv[1], &quality) != TCL_OK)
      return TCL_ERROR;
    
    if (quality < 1 || quality > 100) {
      Tcl_AppendResult(interp, "Invalid JPEG quality (1-100)", NULL);
      return TCL_ERROR;
    }
    
    info->jpeg_quality = quality;
    if (info->capture) {
      info->capture->set_jpeg_quality(quality);
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(quality));
    return TCL_OK;
  }

  static int camera_set_brightness_command(ClientData data,
                                           Tcl_Interp *interp,
                                           int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    double brightness;
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available", NULL);
      return TCL_ERROR;
    }
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "brightness");
      return TCL_ERROR;
    }
    
    if (Tcl_GetDoubleFromObj(interp, objv[1], &brightness) != TCL_OK)
      return TCL_ERROR;
    
    if (brightness < -1.0 || brightness > 1.0) {
      Tcl_AppendResult(interp, "Invalid brightness (-1.0 to 1.0)", NULL);
      return TCL_ERROR;
    }
    
    info->capture->set_brightness(brightness);
    Tcl_SetObjResult(interp, Tcl_NewDoubleObj(brightness));
    return TCL_OK;
  }

  static int camera_set_contrast_command(ClientData data,
                                         Tcl_Interp *interp,
                                         int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    double contrast;
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available", NULL);
      return TCL_ERROR;
    }
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "contrast");
      return TCL_ERROR;
    }
    
    if (Tcl_GetDoubleFromObj(interp, objv[1], &contrast) != TCL_OK)
      return TCL_ERROR;
    
    if (contrast < 0.0 || contrast > 2.0) {
      Tcl_AppendResult(interp, "Invalid contrast (0.0 to 2.0)", NULL);
      return TCL_ERROR;
    }
    
    info->capture->set_contrast(contrast);
    Tcl_SetObjResult(interp, Tcl_NewDoubleObj(contrast));
    return TCL_OK;
  }

  static int camera_release_command(ClientData data,
                                    Tcl_Interp *interp,
                                    int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (info->capture) {
      delete info->capture;
      info->capture = nullptr;
      info->initialized = 0;
      info->configured = 0;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
  }

  static int camera_status_command(ClientData data,
                                   Tcl_Interp *interp,
                                   int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    Tcl_Obj *result = Tcl_NewDictObj();
    
    Tcl_DictObjPut(interp, result, 
                   Tcl_NewStringObj("available", -1),
                   Tcl_NewBooleanObj(info->available));
    
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("initialized", -1),
                   Tcl_NewBooleanObj(info->initialized));
    
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("configured", -1),
                   Tcl_NewBooleanObj(info->configured));

    if (info->capture) {
      const char* state_str = "idle";
      switch (info->capture->get_state()) {
      case CameraState::STREAMING: state_str = "streaming"; break;
      case CameraState::CAPTURING: state_str = "capturing"; break;
      default: state_str = "idle"; break;
      }
      
      Tcl_DictObjPut(interp, result,
                     Tcl_NewStringObj("state", -1),
                     Tcl_NewStringObj(state_str, -1));
      
      Tcl_DictObjPut(interp, result,
                     Tcl_NewStringObj("ae_settled", -1),
                     Tcl_NewBooleanObj(info->capture->is_ae_settled()));
      double configured_fps = info->capture->get_configured_fps();
      if (configured_fps > 0.0) {
	Tcl_DictObjPut(interp, result,
		       Tcl_NewStringObj("configured_fps", -1),
		       Tcl_NewDoubleObj(configured_fps));
	
	Tcl_DictObjPut(interp, result,
		       Tcl_NewStringObj("hardware_fps_supported", -1),
		       Tcl_NewBooleanObj(info->capture->is_hardware_fps_supported()));
	
	Tcl_DictObjPut(interp, result,
		       Tcl_NewStringObj("software_throttling_active", -1),
		       Tcl_NewBooleanObj(info->capture->is_software_throttling_active()));
	
	const char* fps_method = info->capture->is_hardware_fps_supported() ? 
	  "hardware" : "software_throttling";
	Tcl_DictObjPut(interp, result,
		       Tcl_NewStringObj("fps_control_method", -1),
		       Tcl_NewStringObj(fps_method, -1));
      }
    }
    
#ifdef HAS_LIBCAMERA
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("libcamera", -1),
                   Tcl_NewStringObj("yes", -1));
#else
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("libcamera", -1),
                   Tcl_NewStringObj("no", -1));
#endif

#ifdef HAS_JPEG
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("jpeg_support", -1),
                   Tcl_NewStringObj("yes", -1));
#else
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("jpeg_support", -1),
                   Tcl_NewStringObj("no", -1));
#endif
    
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
  }

  static int camera_start_streaming_command(ClientData data,
                                            Tcl_Interp *interp,
                                            int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
#ifndef HAS_LIBCAMERA
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
#else
    if (!info->configured || !info->capture) {
      Tcl_AppendResult(interp, "Camera not configured", NULL);
      return TCL_ERROR;
    }
    
    if (!info->capture->start_streaming()) {
      Tcl_AppendResult(interp, "Failed to start streaming", NULL);
      return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
#endif
  }

  static int camera_stop_streaming_command(ClientData data,
                                           Tcl_Interp *interp,
                                           int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
#ifndef HAS_LIBCAMERA
    return TCL_OK;
#else
    if (!info->capture) {
      return TCL_OK;
    }
    
    info->capture->stop_streaming();
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
#endif
  }

static int camera_set_frame_skip_rate_command(ClientData data,
                                              Tcl_Interp *interp,
                                              int objc, Tcl_Obj *objv[])
{
    camera_info_t *info = (camera_info_t *) data;
    
#ifndef HAS_LIBCAMERA
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
#else
    if (!info->capture) {
        Tcl_AppendResult(interp, "Camera not initialized", NULL);
        return TCL_ERROR;
    }
    
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "skip_rate");
        return TCL_ERROR;
    }
    
    int rate;
    if (Tcl_GetIntFromObj(interp, objv[1], &rate) != TCL_OK)
        return TCL_ERROR;
    
    if (rate < 1 || rate > 100) {
        Tcl_AppendResult(interp, "Invalid skip rate (1-100)", NULL);
        return TCL_ERROR;
    }
    
    info->capture->set_frame_skip_rate(rate);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(rate));
    return TCL_OK;
#endif
}
  
  static int camera_grab_frame_command(ClientData data,
                                       Tcl_Interp *interp,
                                       int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
#ifndef HAS_LIBCAMERA
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
#else
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (!info->capture->grab_frame()) {
      Tcl_AppendResult(interp, "Failed to grab frame", NULL);
      return TCL_ERROR;
    }
    
    if (objc > 1) {
      const char *filename = Tcl_GetString(objv[1]);
      if (!info->capture->save_jpeg(filename)) {
        Tcl_AppendResult(interp, "Failed to save frame", NULL);
        return TCL_ERROR;
      }
      Tcl_SetObjResult(interp, Tcl_NewStringObj(filename, -1));
    } else {
      Tcl_SetObjResult(interp, Tcl_NewIntObj(info->capture->get_image_size()));
    }
    
    return TCL_OK;
#endif
  }  
  
  
  // Add these 8 new Tcl command implementations to your camera.cpp file
// Replace the existing versions with these properly #ifdef-guarded ones

#ifdef HAS_LIBCAMERA
  static int camera_start_continuous_command(ClientData data,
                                            Tcl_Interp *interp,
                                            int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (info->capture->get_state() != CameraState::STREAMING) {
      Tcl_AppendResult(interp, "Camera must be streaming to start continuous mode", NULL);
      return TCL_ERROR;
    }
    
    // Parse arguments: save_to_disk publish_to_dataserver [save_directory] [datapoint_prefix] [interval]
    if (objc < 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "save_to_disk publish_to_dataserver ?save_directory? ?datapoint_prefix? ?interval?");
      return TCL_ERROR;
    }
    
    int save_to_disk, publish_to_dataserver;
    if (Tcl_GetBooleanFromObj(interp, objv[1], &save_to_disk) != TCL_OK)
      return TCL_ERROR;
    if (Tcl_GetBooleanFromObj(interp, objv[2], &publish_to_dataserver) != TCL_OK)
      return TCL_ERROR;
    
    std::string save_directory = "/tmp/camera_frames/";
    if (objc > 3) {
      save_directory = Tcl_GetString(objv[3]);
    }
    
    std::string datapoint_prefix = "camera";
    if (objc > 4) {
      datapoint_prefix = Tcl_GetString(objv[4]);
    }
    
    int interval = 1;
    if (objc > 5) {
      if (Tcl_GetIntFromObj(interp, objv[5], &interval) != TCL_OK)
        return TCL_ERROR;
    }
    
    if (!info->capture->start_continuous_mode(save_to_disk, publish_to_dataserver, 
                                             save_directory, datapoint_prefix, interval)) {
      Tcl_AppendResult(interp, "Failed to start continuous mode", NULL);
      return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
  }

  static int camera_start_continuous_callback_command(ClientData data,
                                                     Tcl_Interp *interp,
                                                     int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (info->capture->get_state() != CameraState::STREAMING) {
      Tcl_AppendResult(interp, "Camera must be streaming to start continuous callback mode", NULL);
      return TCL_ERROR;
    }
    
    // Parse arguments: tcl_proc [datapoint_prefix] [interval]
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "tcl_proc ?datapoint_prefix? ?interval?");
      return TCL_ERROR;
    }
    
    const char *tcl_proc = Tcl_GetString(objv[1]);
    
    std::string datapoint_prefix = "camera";
    if (objc > 2) {
      datapoint_prefix = Tcl_GetString(objv[2]);
    }
    
    int interval = 1;
    if (objc > 3) {
      if (Tcl_GetIntFromObj(interp, objv[3], &interval) != TCL_OK)
        return TCL_ERROR;
    }
    
    if (!info->capture->start_continuous_callback_mode(tcl_proc, datapoint_prefix, interval)) {
      Tcl_AppendResult(interp, "Failed to start continuous callback mode", NULL);
      return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
  }

  static int camera_stop_continuous_command(ClientData data,
                                           Tcl_Interp *interp,
                                           int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      return TCL_OK;
    }
    
    info->capture->stop_continuous_mode();
    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
    return TCL_OK;
  }

  static int camera_set_target_fps_command(ClientData data,
                                          Tcl_Interp *interp,
                                          int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "fps");
      return TCL_ERROR;
    }
    
    double fps;
    if (Tcl_GetDoubleFromObj(interp, objv[1], &fps) != TCL_OK)
      return TCL_ERROR;
    
    if (fps < 0.0 || fps > 120.0) {
      Tcl_AppendResult(interp, "Invalid FPS (0.0-120.0)", NULL);
      return TCL_ERROR;
    }
    
    info->capture->set_target_fps(fps);
    Tcl_SetObjResult(interp, Tcl_NewDoubleObj(fps));
    return TCL_OK;
  }

  static int camera_get_callback_frame_command(ClientData data,
                                              Tcl_Interp *interp,
                                              int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 2) {
      Tcl_WrongNumArgs(interp, 1, objv, "frame_id");
      return TCL_ERROR;
    }
    
    int frame_id;
    if (Tcl_GetIntFromObj(interp, objv[1], &frame_id) != TCL_OK)
      return TCL_ERROR;
    
    std::vector<uint8_t> frame_data;
    int64_t timestamp_ms;
    
    if (!info->capture->get_frame_by_id(frame_id, frame_data, timestamp_ms)) {
      Tcl_AppendResult(interp, "Frame not found in ring buffer (too old or invalid frame_id)", NULL);
      return TCL_ERROR;
    }
    
    // Return the JPEG data as a Tcl byte array
    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(frame_data.data(), frame_data.size()));
    return TCL_OK;
  }

  static int camera_save_callback_frame_command(ClientData data,
                                               Tcl_Interp *interp,
                                               int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "frame_id filename");
      return TCL_ERROR;
    }
    
    int frame_id;
    if (Tcl_GetIntFromObj(interp, objv[1], &frame_id) != TCL_OK)
      return TCL_ERROR;
    
    const char *filename = Tcl_GetString(objv[2]);
    
    if (!info->capture->save_callback_frame(frame_id, filename)) {
      Tcl_AppendResult(interp, "Failed to save frame (not found or I/O error)", NULL);
      return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(filename, -1));
    return TCL_OK;
  }

  static int camera_publish_callback_frame_command(ClientData data,
                                                  Tcl_Interp *interp,
                                                  int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    if (objc < 3) {
      Tcl_WrongNumArgs(interp, 1, objv, "frame_id datapoint_name");
      return TCL_ERROR;
    }
    
    int frame_id;
    if (Tcl_GetIntFromObj(interp, objv[1], &frame_id) != TCL_OK)
      return TCL_ERROR;
    
    const char *datapoint_name = Tcl_GetString(objv[2]);
    
    if (!info->capture->publish_callback_frame(frame_id, datapoint_name)) {
      Tcl_AppendResult(interp, "Failed to publish frame (not found or dataserver error)", NULL);
      return TCL_ERROR;
    }
    
    Tcl_SetObjResult(interp, Tcl_NewStringObj(datapoint_name, -1));
    return TCL_OK;
  }

  static int camera_get_ring_buffer_status_command(ClientData data,
                                                  Tcl_Interp *interp,
                                                  int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
      return TCL_ERROR;
    }
    
    int oldest_frame_id, newest_frame_id, valid_frames;
    info->capture->get_ring_buffer_status(oldest_frame_id, newest_frame_id, valid_frames);
    
    Tcl_Obj *result = Tcl_NewDictObj();
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("oldest_frame_id", -1),
                   Tcl_NewIntObj(oldest_frame_id));
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("newest_frame_id", -1), 
                   Tcl_NewIntObj(newest_frame_id));
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("valid_frames", -1),
                   Tcl_NewIntObj(valid_frames));
    Tcl_DictObjPut(interp, result,
                   Tcl_NewStringObj("buffer_size", -1),
                   Tcl_NewIntObj(16));  // RING_BUFFER_SIZE
    
    Tcl_SetObjResult(interp, result);
    return TCL_OK;
  }

#else  // !HAS_LIBCAMERA - Stub implementations for systems without libcamera

  static int camera_start_continuous_command(ClientData data,
                                            Tcl_Interp *interp,
                                            int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_start_continuous_callback_command(ClientData data,
                                                     Tcl_Interp *interp,
                                                     int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_stop_continuous_command(ClientData data,
                                           Tcl_Interp *interp,
                                           int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_set_target_fps_command(ClientData data,
                                          Tcl_Interp *interp,
                                          int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_get_callback_frame_command(ClientData data,
                                              Tcl_Interp *interp,
                                              int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_save_callback_frame_command(ClientData data,
                                               Tcl_Interp *interp,
                                               int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_publish_callback_frame_command(ClientData data,
                                                  Tcl_Interp *interp,
                                                  int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

  static int camera_get_ring_buffer_status_command(ClientData data,
                                                  Tcl_Interp *interp,
                                                  int objc, Tcl_Obj *objv[])
  {
    Tcl_AppendResult(interp, "Camera support not available", NULL);
    return TCL_ERROR;
  }

#endif  // HAS_LIBCAMERA
  

  // Called when the interpreter exits
  static void camera_cleanup(ClientData clientData, Tcl_Interp *interp) {
    camera_info_t *info = (camera_info_t *)clientData;
    if (info->capture) {
      delete info->capture;
    }
    if (info->dpoint_prefix) {
      free(info->dpoint_prefix);
    }
    free(info);
  }
  
  /**************************************************************************
   * MODULE INITIALIZATION
   **************************************************************************/

#ifdef WIN32
  EXPORT(int,Dserv_camera_Init) (Tcl_Interp *interp)
#else
  int Dserv_camera_Init(Tcl_Interp *interp)
#endif
  {
    if (
#ifdef USE_TCL_STUBS
        Tcl_InitStubs(interp, "8.6-", 0)
#else
        Tcl_PkgRequire(interp, "Tcl", "8.6-", 0)
#endif
        == NULL) {
      return TCL_ERROR;
    }

    // Allocate camera info dynamically
    camera_info_t *cameraInfo =
      (camera_info_t*) calloc(1, sizeof(camera_info_t));
    cameraInfo->tclserver = 
      (TclServer*)Tcl_GetAssocData(interp, "tclserver_instance", NULL);
    cameraInfo->dpoint_prefix = strdup("camera");
    cameraInfo->capture = nullptr;
    cameraInfo->initialized = 0;
    cameraInfo->configured = 0;
    cameraInfo->jpeg_quality = 85;
    
#ifdef HAS_LIBCAMERA
    cameraInfo->available = 1;
#else
    cameraInfo->available = 0;
#endif

    // Store with cleanup function
    Tcl_SetAssocData(interp, "camera_info", camera_cleanup, cameraInfo);
    
    Tcl_CreateObjCommand(interp, "cameraList",
                         (Tcl_ObjCmdProc *) camera_list_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraInit",
                         (Tcl_ObjCmdProc *) camera_init_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraConfigure",
                         (Tcl_ObjCmdProc *) camera_configure_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraCapture",
                         (Tcl_ObjCmdProc *) camera_capture_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraCaptureDatapoint",
                         (Tcl_ObjCmdProc *) camera_capture_datapoint_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetSettlingFrames",
                         (Tcl_ObjCmdProc *) camera_set_settling_frames_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetJpegQuality",
                         (Tcl_ObjCmdProc *) camera_set_jpeg_quality_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetBrightness",
                         (Tcl_ObjCmdProc *) camera_set_brightness_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetContrast",
                         (Tcl_ObjCmdProc *) camera_set_contrast_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraRelease",
                         (Tcl_ObjCmdProc *) camera_release_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStatus",
                         (Tcl_ObjCmdProc *) camera_status_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStartStreaming",
                         (Tcl_ObjCmdProc *) camera_start_streaming_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStopStreaming",
                         (Tcl_ObjCmdProc *) camera_stop_streaming_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraGrabFrame",
                         (Tcl_ObjCmdProc *) camera_grab_frame_command,
                         cameraInfo, NULL);    
    Tcl_CreateObjCommand(interp, "cameraSetFrameSkipRate",
                         (Tcl_ObjCmdProc *) camera_set_frame_skip_rate_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStartContinuous",
                         (Tcl_ObjCmdProc *) camera_start_continuous_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStartContinuousCallback",
                         (Tcl_ObjCmdProc *) camera_start_continuous_callback_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStopContinuous",
                         (Tcl_ObjCmdProc *) camera_stop_continuous_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetTargetFPS",
                         (Tcl_ObjCmdProc *) camera_set_target_fps_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraGetCallbackFrame",
                         (Tcl_ObjCmdProc *) camera_get_callback_frame_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSaveCallbackFrame",
                         (Tcl_ObjCmdProc *) camera_save_callback_frame_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraPublishCallbackFrame",
                         (Tcl_ObjCmdProc *) camera_publish_callback_frame_command,
                         cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraGetRingBufferStatus",
                         (Tcl_ObjCmdProc *) camera_get_ring_buffer_status_command,
                         cameraInfo, NULL);
    return TCL_OK;
  }

} // extern "C"
