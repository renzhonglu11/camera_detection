#pragma once

#include <atomic>

namespace camera_detection {

void install_signal_handlers();
std::atomic<bool> &stop_requested();

} // namespace camera_detection
