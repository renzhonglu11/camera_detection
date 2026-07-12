#include "camera/camera_pipeline.h"

#include <sstream>

namespace camera_detection {

std::string build_capture_pipeline(const Options &options)
{
	std::ostringstream pipeline;
	pipeline << "libcamerasrc ! "
			 << "video/x-raw,width=" << options.width
			 << ",height=" << options.height
			 << ",framerate=" << options.fps << "/1 ! "
			 << "queue max-size-buffers=1 leaky=downstream ! "
			 << "videoconvert ! video/x-raw,format=BGR ! "
			 << "appsink drop=true max-buffers=1 sync=false";
	return pipeline.str();
}

} // namespace camera_detection
