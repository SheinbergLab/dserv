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
#include <condition_variable>

// Tcl and dataserver headers
extern "C" {
#include <tcl.h>
#include "Datapoint.h"
#include "tclserver_api.h"
}

// Add JPEG type if not already defined
#ifndef DSERV_JPEG
#define DSERV_JPEG 14
#endif

// Forward declaration
class CameraCapture;

// Camera info structure used by both implementations
typedef struct camera_info_s {
  CameraCapture *capture;
  tclserver_t *tclserver;
  char *dpoint_prefix;
  int initialized;
  int configured;
  int jpeg_quality;
  int available;
} camera_info_t;

// Global camera info
static camera_info_t g_cameraInfo;

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

class CameraCapture {
private:
  std::unique_ptr<CameraManager> cm_;
  std::shared_ptr<Camera> camera_;
  std::unique_ptr<CameraConfiguration> config_;
  std::unique_ptr<FrameBufferAllocator> allocator_;
  std::vector<std::unique_ptr<Request>> requests_;
  std::atomic<bool> capture_complete_{false};
  std::atomic<int> frames_captured_{0};
  std::vector<uint8_t> image_data_;
  std::vector<uint8_t> jpeg_data_;
  Stream *stream_ = nullptr;
  int target_frames_ = 30;
  int capture_frame_ = 31;
    
  // Camera parameters
  unsigned int width_ = 1920;
  unsigned int height_ = 1080;
  int settling_frames_ = 30;
  float brightness_ = 0.0f;
  float contrast_ = 1.0f;
  int jpeg_quality_ = 85;
  
  bool streaming_mode_ = false;
  std::mutex capture_mutex_;
  std::condition_variable capture_cv_;
  bool frame_ready_ = false;
  int frame_skip_counter_ = 0;
  int frame_skip_rate_ = 1;  // 1 = every frame, 10 = every 10th frame
  
public:
  CameraCapture() {
    cm_ = std::make_unique<CameraManager>();
  }

  ~CameraCapture() {
    if (camera_) {
      camera_->stop();
      camera_->release();
    }
  }

  bool initialize() {
    int ret = cm_->start();
    if (ret) {
      return false;
    }

    auto cameras = cm_->cameras();
    if (cameras.empty()) {
      return false;
    }

    std::string camera_id = cameras[0]->id();
    camera_ = cm_->get(camera_id);
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
    width_ = width;
    height_ = height;
        
    config_ = camera_->generateConfiguration({ StreamRole::StillCapture });
    if (!config_ || config_->size() == 0) {
      return false;
    }

    StreamConfiguration &stream_config = config_->at(0);
    stream_config.size.width = width;
    stream_config.size.height = height;
    stream_config.pixelFormat = formats::RGB888;
    stream_config.bufferCount = 4;

    CameraConfiguration::Status validation = config_->validate();
    if (validation == CameraConfiguration::Invalid) {
      return false;
    }

    int ret = camera_->configure(config_.get());
    if (ret) {
      return false;
    }

    stream_ = stream_config.stream();
    return true;
  }

  bool allocate_buffers() {
    allocator_ = std::make_unique<FrameBufferAllocator>(camera_);

    int ret = allocator_->allocate(stream_);
    if (ret < 0) {
      return false;
    }

    const std::vector<std::unique_ptr<FrameBuffer>> &buffers = 
      allocator_->buffers(stream_);
        
    for (unsigned int i = 0; i < buffers.size(); ++i) {
      std::unique_ptr<Request> request = camera_->createRequest();
      if (!request) {
	return false;
      }

      const std::unique_ptr<FrameBuffer> &buffer = buffers[i];
      ret = request->addBuffer(stream_, buffer.get());
      if (ret < 0) {
	return false;
      }

      // Set controls
      ControlList &controls = request->controls();
        // Set controls only if supported
        ControlList &controls = request->controls();
        
        // Check what controls are actually available
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
	  controls.set(controls::AeMeteringMode,
		       controls::MeteringCentreWeighted);
        }
        if (available_controls.find(&controls::Brightness) !=
	    available_controls.end()) {
	  controls.set(controls::Brightness, brightness_);
        }
        if (available_controls.find(&controls::Contrast) !=
	    available_controls.end()) {
	  controls.set(controls::Contrast, contrast_);
        }
	
	requests_.push_back(std::move(request));
    }
    
    return true;
  }

  bool capture_image() {
    target_frames_ = settling_frames_;
    capture_frame_ = settling_frames_ + 1;
    frames_captured_ = 0;
    capture_complete_ = false;
        
    camera_->requestCompleted.connect(this, &CameraCapture::request_complete);

    int ret = camera_->start();
    if (ret) {
      return false;
    }

    for (auto &request : requests_) {
      ret = camera_->queueRequest(request.get());
      if (ret) {
	camera_->stop();
	return false;
      }
    }

    auto start = std::chrono::steady_clock::now();
    while (!capture_complete_) {
      std::this_thread::sleep_for(10ms);
            
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (elapsed > 10s) {
	camera_->stop();
	return false;
      }
    }

    camera_->stop();
    return !image_data_.empty();
  }

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

  bool encode_jpeg() {
#ifdef HAS_JPEG
    if (!stream_ || image_data_.empty()) {
      return false;
    }

    const StreamConfiguration &cfg = stream_->configuration();
        
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
    return false;  // No JPEG support
#endif
  }

  bool start_streaming() {
    if (!stream_ || !allocator_) return false;
    
    streaming_mode_ = true;
    frames_captured_ = 0;
    frame_ready_ = false;
    
    camera_->requestCompleted.connect(this, &CameraCapture::streaming_request_complete);
    
    int ret = camera_->start();
    if (ret) return false;
    
    // Queue all requests
    for (auto &request : requests_) {
      ret = camera_->queueRequest(request.get());
      if (ret) {
	camera_->stop();
	return false;
      }
    }
    
    // Only settle once at startup if requested
    if (settling_frames_ > 0) {
      // Wait for initial settling
      std::this_thread::sleep_for(std::chrono::milliseconds(settling_frames_ * 33));
    }
    
    return true;
  }


  bool stop_streaming() {
    if (!streaming_mode_) return false;
    
    streaming_mode_ = false;
    camera_->stop();
    return true;
  }

  bool grab_frame() {
    if (!streaming_mode_) return false;
    
    // Just lock and copy whatever frame is current
    std::lock_guard<std::mutex> lock(capture_mutex_);
    
    // image_data_ always has the latest frame
    // No need to wait - it's constantly being updated
    return true;
  }

  void streaming_request_complete(Request *request) {
    if (request->status() == Request::RequestCancelled) {
      request->reuse(Request::ReuseBuffers);
      camera_->queueRequest(request);
      return;
    }
    
    // Skip frames if configured
    if (++frame_skip_counter_ < frame_skip_rate_) {
      request->reuse(Request::ReuseBuffers);
      camera_->queueRequest(request);
      return;
    }
    frame_skip_counter_ = 0;
    
    // Just update the buffer - no signaling needed
    const Request::BufferMap &buffers = request->buffers();
    for (auto buffer_pair : buffers) {
      FrameBuffer *buffer = buffer_pair.second;
      const FrameBuffer::Plane &plane = buffer->planes()[0];
        
      void *data = mmap(nullptr, plane.length, PROT_READ, MAP_SHARED,
                        plane.fd.get(), 0);
      if (data == MAP_FAILED) continue;
        
      {
	std::lock_guard<std::mutex> lock(capture_mutex_);
	image_data_.resize(plane.length);
	std::memcpy(image_data_.data(), data, plane.length);
      }
        
      munmap(data, plane.length);
      break;
    }
    
    request->reuse(Request::ReuseBuffers);
    camera_->queueRequest(request);
  }

  void set_frame_skip_rate(int rate) {
    frame_skip_rate_ = std::max(1, rate);
  }

  // Getters and setters
  void set_settling_frames(int frames) { settling_frames_ = frames; }
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

private:
  void request_complete(Request *request) {
    if (request->status() == Request::RequestCancelled) {
      return;
    }

    frames_captured_++;

    if (frames_captured_ == capture_frame_) {
      const Request::BufferMap &buffers = request->buffers();
            
      for (auto buffer_pair : buffers) {
	FrameBuffer *buffer = buffer_pair.second;
	const FrameMetadata &metadata = buffer->metadata();
	const FrameBuffer::Plane &plane = buffer->planes()[0];
                
	void *data = mmap(nullptr, plane.length, PROT_READ, MAP_SHARED,
			  plane.fd.get(), 0);
                
	if (data == MAP_FAILED) {
	  continue;
	}

	image_data_.resize(plane.length);
	std::memcpy(image_data_.data(), data, plane.length);
	munmap(data, plane.length);
                
	// Print metadata if available
	const ControlList &requestMetadata = request->metadata();
	if (requestMetadata.contains(controls::ExposureTime.id())) {
	  auto exp_time = requestMetadata.get(controls::ExposureTime);
	  std::cout << "  Exposure: " << *exp_time << " µs" << std::endl;
	}
	if (requestMetadata.contains(controls::AnalogueGain.id())) {
	  auto gain = requestMetadata.get(controls::AnalogueGain);
	  std::cout << "  Gain: " << *gain << "x" << std::endl;
	}
                
	capture_complete_ = true;
      }
    }
        
    if (frames_captured_ < capture_frame_) {
      request->reuse(Request::ReuseBuffers);
      camera_->queueRequest(request);
    }
  }
};

/*****************************************************************************
 * STUB IMPLEMENTATION
 * Minimal implementation when libcamera is not available
 *****************************************************************************/
#else  // !HAS_LIBCAMERA

class CameraCapture {
public:
  CameraCapture() {}
  ~CameraCapture() {}
    
  bool initialize() { return false; }
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
  void set_frame_skip_rate(int rate) {}};

#endif  // HAS_LIBCAMERA

/*****************************************************************************
 * TCL COMMAND IMPLEMENTATIONS
 * These work with either implementation above
 *****************************************************************************/

extern "C" {

  static int camera_init_command(ClientData data,
				 Tcl_Interp *interp,
				 int objc, Tcl_Obj *objv[])
  {
    camera_info_t *info = (camera_info_t *) data;
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available on this platform", NULL);
      return TCL_ERROR;
    }
    
    if (info->initialized) {
      Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
      return TCL_OK;
    }

    try {
      info->capture = new CameraCapture();
        
      if (!info->capture->initialize()) {
	delete info->capture;
	info->capture = nullptr;
	Tcl_AppendResult(interp, "Failed to initialize camera", NULL);
	return TCL_ERROR;
      }
        
      info->initialized = 1;
      Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
      return TCL_OK;
        
    } catch (const std::exception& e) {
      Tcl_AppendResult(interp, "Exception during initialization: ", e.what(), NULL);
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
    
    if (!info->available) {
      Tcl_AppendResult(interp, "Camera support not available", NULL);
      return TCL_ERROR;
    }
    
    if (!info->initialized || !info->capture) {
      Tcl_AppendResult(interp, "Camera not initialized", NULL);
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

    try {
      if (!info->capture->configure(width, height)) {
	Tcl_AppendResult(interp, "Failed to configure camera", NULL);
	return TCL_ERROR;
      }
        
      if (!info->capture->allocate_buffers()) {
	Tcl_AppendResult(interp, "Failed to allocate buffers", NULL);
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
				      tclserver_now(info->tclserver),
				      (ds_datatype_t)DSERV_JPEG,
				      info->capture->get_jpeg_size(),
				      (unsigned char *)info->capture->get_jpeg_data());
        
      tclserver_set_point(info->tclserver, dp);
        
      char meta_name[256];
      snprintf(meta_name, sizeof(meta_name), "%s/meta", point_name);
        
      char meta_str[256];
      snprintf(meta_str, sizeof(meta_str), 
	       "{\"width\":%d,\"height\":%d,\"size\":%zu,\"format\":\"jpeg\"}",
	       info->capture->get_width(),
	       info->capture->get_height(),
	       info->capture->get_jpeg_size());
        
      ds_datapoint_t *meta_dp = dpoint_new(meta_name,
					   tclserver_now(info->tclserver),
					   DSERV_STRING,
					   strlen(meta_str) + 1,
					   (unsigned char *)meta_str);
        
      tclserver_set_point(info->tclserver, meta_dp);
        
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
  
  /*****************************************************************************
   * MODULE INITIALIZATION
   *****************************************************************************/

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

    g_cameraInfo.tclserver = tclserver_get();
    g_cameraInfo.dpoint_prefix = (char *)"camera";
    g_cameraInfo.capture = nullptr;
    g_cameraInfo.initialized = 0;
    g_cameraInfo.configured = 0;
    g_cameraInfo.jpeg_quality = 85;
    
#ifdef HAS_LIBCAMERA
    g_cameraInfo.available = 1;
#else
    g_cameraInfo.available = 0;
#endif
    
    Tcl_CreateObjCommand(interp, "cameraInit",
			 (Tcl_ObjCmdProc *) camera_init_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraConfigure",
			 (Tcl_ObjCmdProc *) camera_configure_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraCapture",
			 (Tcl_ObjCmdProc *) camera_capture_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraCaptureDatapoint",
			 (Tcl_ObjCmdProc *) camera_capture_datapoint_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetSettlingFrames",
			 (Tcl_ObjCmdProc *) camera_set_settling_frames_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetJpegQuality",
			 (Tcl_ObjCmdProc *) camera_set_jpeg_quality_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetBrightness",
			 (Tcl_ObjCmdProc *) camera_set_brightness_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraSetContrast",
			 (Tcl_ObjCmdProc *) camera_set_contrast_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraRelease",
			 (Tcl_ObjCmdProc *) camera_release_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStatus",
			 (Tcl_ObjCmdProc *) camera_status_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStartStreaming",
			 (Tcl_ObjCmdProc *) camera_start_streaming_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraStopStreaming",
			 (Tcl_ObjCmdProc *) camera_stop_streaming_command,
			 &g_cameraInfo, NULL);
    Tcl_CreateObjCommand(interp, "cameraGrabFrame",
			 (Tcl_ObjCmdProc *) camera_grab_frame_command,
			 &g_cameraInfo, NULL);    
    Tcl_CreateObjCommand(interp, "cameraSetFrameSkipRate",
			 (Tcl_ObjCmdProc *) camera_set_frame_skip_rate_command,
			 &g_cameraInfo, NULL);
    return TCL_OK;
  }

} // extern "C"
