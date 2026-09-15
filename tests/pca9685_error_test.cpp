#include "hardware/PCA9685.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct FakeSystemCalls {
	int open_result = 42;
	int open_errno = ENOENT;
	int open_calls = 0;
	std::vector<int> ioctl_results;
	std::size_t ioctl_index = 0;
	int ioctl_calls = 0;
	std::vector<ssize_t> read_results;
	std::size_t read_index = 0;
	int read_calls = 0;
	std::uint8_t read_value = 0x20;
	std::vector<ssize_t> write_results;
	std::size_t write_index = 0;
	int write_calls = 0;
	std::vector<std::vector<std::uint8_t>> writes;
	int close_result = 0;
	int close_calls = 0;
	int usleep_calls = 0;
};

FakeSystemCalls fake;

void reset_test() {
	i2c_close();
	fake = FakeSystemCalls{};
	errno = 0;
}

bool initialize_driver() {
	return i2c_init("/dev/fake-i2c", I2C_ADDR) == 0;
}

#define CHECK(condition)                                                       \
	do {                                                                       \
		if (!(condition)) {                                                     \
			std::cerr << __func__ << ':' << __LINE__                             \
					  << ": check failed: " #condition << '\n';                    \
			return false;                                                        \
		}                                                                      \
	} while (false)

bool test_validation_and_idempotent_close() {
	reset_test();
	CHECK(i2c_init(nullptr, I2C_ADDR) == -1);
	CHECK(errno == EINVAL);
	CHECK(i2c_init("/dev/fake-i2c", 0x80) == -1);
	CHECK(errno == EINVAL);

	std::uint8_t byte = 0;
	CHECK(i2c_readNbyte(&byte, 1) == -1);
	CHECK(errno == EBADF);
	CHECK(i2c_close() == 0);
	CHECK(fake.close_calls == 0);

	CHECK(initialize_driver());
	CHECK(i2c_init("/dev/fake-i2c", I2C_ADDR) == -1);
	CHECK(errno == EALREADY);
	CHECK(fake.open_calls == 1);
	CHECK(i2c_close() == 0);
	CHECK(fake.close_calls == 1);
	CHECK(i2c_close() == 0);
	CHECK(fake.close_calls == 1);
	return true;
}

bool test_initialization_failures_close_the_temporary_fd() {
	reset_test();
	fake.open_result = -1;
	fake.open_errno = EACCES;
	CHECK(i2c_init("/dev/fake-i2c", I2C_ADDR) == -1);
	CHECK(errno == EACCES);
	CHECK(fake.close_calls == 0);

	reset_test();
	fake.ioctl_results = {-1};
	CHECK(i2c_init("/dev/fake-i2c", I2C_ADDR) == -1);
	CHECK(errno == EIO);
	CHECK(fake.ioctl_calls == 1);
	CHECK(fake.close_calls == 1);
	CHECK(i2c_close() == 0);
	CHECK(fake.close_calls == 1);

	reset_test();
	fake.ioctl_results = {0, -1};
	CHECK(i2c_init("/dev/fake-i2c", I2C_ADDR) == -1);
	CHECK(errno == EIO);
	CHECK(fake.ioctl_calls == 2);
	CHECK(fake.close_calls == 1);
	CHECK(i2c_close() == 0);
	CHECK(fake.close_calls == 1);
	return true;
}

bool test_register_io_rejects_short_transfers() {
	reset_test();
	CHECK(initialize_driver());
	fake.write_results = {1};
	CHECK(i2c_writeReg(0x10, 0x55) == -1);
	CHECK(errno == EIO);
	CHECK(fake.writes.size() == 1);
	CHECK(fake.writes[0] == std::vector<std::uint8_t>({0x10, 0x55}));

	reset_test();
	CHECK(initialize_driver());
	fake.write_results = {0};
	std::uint8_t data = 0;
	CHECK(i2c_readReg(0x20, &data) == -1);
	CHECK(errno == EIO);
	CHECK(fake.read_calls == 0);

	reset_test();
	CHECK(initialize_driver());
	fake.read_results = {0};
	CHECK(i2c_readReg(0x20, &data) == -1);
	CHECK(errno == EIO);
	CHECK(fake.write_calls == 1);
	CHECK(fake.read_calls == 1);

	reset_test();
	CHECK(initialize_driver());
	fake.read_value = 0x5a;
	CHECK(i2c_readReg(0x20, &data) == 0);
	CHECK(data == 0x5a);
	return true;
}

bool test_compound_operations_stop_at_the_first_error() {
	reset_test();
	CHECK(initialize_driver());
	fake.write_results = {2, 2, -1};
	CHECK(PCA9685_setPWM(1, 0, 400) == -1);
	CHECK(errno == EIO);
	CHECK(fake.write_calls == 3);

	reset_test();
	CHECK(initialize_driver());
	fake.read_results = {-1};
	CHECK(PCA9685_setPWMFreq(60.0f) == -1);
	CHECK(errno == EIO);
	CHECK(fake.write_calls == 1);
	CHECK(fake.read_calls == 1);

	reset_test();
	CHECK(initialize_driver());
	CHECK(PCA9685_setPWMFreq(60.0f) == 0);
	CHECK(fake.write_calls == 5);
	CHECK(fake.read_calls == 1);
	CHECK(fake.usleep_calls == 1);

	reset_test();
	CHECK(initialize_driver());
	CHECK(PCA9685_setPWMFreq(0.0f) == -1);
	CHECK(errno == EINVAL);
	CHECK(fake.write_calls == 0);
	CHECK(setServoDegree(1, 181) == -1);
	CHECK(errno == EINVAL);
	CHECK(fake.write_calls == 0);
	CHECK(setServoDegree(16, 90) == -1);
	CHECK(errno == EINVAL);
	CHECK(fake.write_calls == 0);
	return true;
}

bool test_legacy_degree_state_changes_only_after_success() {
	reset_test();
	CHECK(initialize_driver());
	ServoDownDegree = 90;
	fake.write_results = {-1};
	CHECK(ServoDegreeIncrease(SERVO_DOWN_CH, 1) == -1);
	CHECK(ServoDownDegree == 90);

	fake.write_results.clear();
	fake.write_index = 0;
	CHECK(ServoDegreeIncrease(SERVO_DOWN_CH, 1) == 0);
	CHECK(ServoDownDegree == 91);
	return true;
}

} // namespace

extern "C" int __wrap_open(const char *, int, ...) {
	++fake.open_calls;
	if (fake.open_result < 0)
		errno = fake.open_errno;
	return fake.open_result;
}

extern "C" int __wrap_ioctl(int, unsigned long, ...) {
	++fake.ioctl_calls;
	int result = 0;
	if (fake.ioctl_index < fake.ioctl_results.size())
		result = fake.ioctl_results[fake.ioctl_index++];
	if (result < 0)
		errno = EIO;
	return result;
}

extern "C" ssize_t __wrap_read(int, void *buffer, size_t count) {
	++fake.read_calls;
	ssize_t result = static_cast<ssize_t>(count);
	if (fake.read_index < fake.read_results.size())
		result = fake.read_results[fake.read_index++];
	if (result < 0) {
		errno = EIO;
		return result;
	}
	if (result > 0)
		static_cast<std::uint8_t *>(buffer)[0] = fake.read_value;
	return result;
}

extern "C" ssize_t __wrap_write(int, const void *buffer, size_t count) {
	++fake.write_calls;
	const auto *bytes = static_cast<const std::uint8_t *>(buffer);
	fake.writes.emplace_back(bytes, bytes + count);

	ssize_t result = static_cast<ssize_t>(count);
	if (fake.write_index < fake.write_results.size())
		result = fake.write_results[fake.write_index++];
	if (result < 0)
		errno = EIO;
	return result;
}

extern "C" int __wrap_close(int) {
	++fake.close_calls;
	if (fake.close_result < 0)
		errno = EIO;
	return fake.close_result;
}

extern "C" int __wrap_usleep(useconds_t) {
	++fake.usleep_calls;
	return 0;
}

int main() {
	const std::vector<std::pair<std::string, bool (*)()>> tests = {
		{"validation_and_idempotent_close",
		 test_validation_and_idempotent_close},
		{"initialization_failures_close_the_temporary_fd",
		 test_initialization_failures_close_the_temporary_fd},
		{"register_io_rejects_short_transfers",
		 test_register_io_rejects_short_transfers},
		{"compound_operations_stop_at_the_first_error",
		 test_compound_operations_stop_at_the_first_error},
		{"legacy_degree_state_changes_only_after_success",
		 test_legacy_degree_state_changes_only_after_success},
	};

	for (const auto &[name, test] : tests) {
		if (!test()) {
			std::cerr << "FAILED: " << name << '\n';
			return 1;
		}
		std::cout << "PASSED: " << name << '\n';
	}

	i2c_close();
	return 0;
}
