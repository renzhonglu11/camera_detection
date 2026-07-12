#include "utils/signal_utils.h"

#include <csignal>

namespace camera_detection {
namespace {

std::atomic<bool> g_should_stop{false};

void handle_signal(int)
{
	g_should_stop = true;
}

} // namespace

void install_signal_handlers()
{
	std::signal(SIGINT, handle_signal);
	std::signal(SIGTERM, handle_signal);
}

std::atomic<bool> &stop_requested()
{
	return g_should_stop;
}

} // namespace camera_detection
