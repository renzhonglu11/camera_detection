#include "hardware/PCA9685.h"

static int fd = -1;
uint8_t ServoUpDegree = 90;	  // Servo Up Angle
uint8_t ServoDownDegree = 90; // Servo Down Angle

static int close_after_error(int opened_fd) {
	const int saved_errno = errno;
	close(opened_fd);
	errno = saved_errno;
	return -1;
}

static int validate_io(void *buffer, int len) {
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}

	if (buffer == NULL || len <= 0) {
		errno = EINVAL;
		return -1;
	}

	return 0;
}
/*
// functions：i2c_init
// parameters：i2c_dev：i2c device file path，i2c_addr：i2c address
// return：0 success，-1 failed
*/
int i2c_init(const char *i2c_dev, unsigned char i2c_addr) {
	if (i2c_dev == NULL || i2c_dev[0] == '\0' || i2c_addr > 0x7f) {
		errno = EINVAL;
		return -1;
	}

	if (fd >= 0) {
		errno = EALREADY;
		return -1;
	}

	const int opened_fd = open(i2c_dev, O_RDWR);
	if (opened_fd < 0)
		return -1;

	if (ioctl(opened_fd, I2C_TENBIT, 0) < 0)
		return close_after_error(opened_fd);

	if (ioctl(opened_fd, I2C_SLAVE, i2c_addr) < 0)
		return close_after_error(opened_fd);

	fd = opened_fd;
	return 0;
}

int i2c_close(void) {
	if (fd < 0)
		return 0;

	const int opened_fd = fd;
	fd = -1;
	return close(opened_fd);
}
/*
// functions：i2c reading data
// parameters：buf：Data buffer，len：length
// return: 0 success, -1 failed
*/
int i2c_readNbyte(unsigned char *buf, int len) {
	if (validate_io(buf, len) < 0)
		return -1;

	const ssize_t bytes_read = read(fd, buf, (size_t)len);
	if (bytes_read < 0)
		return -1;
	if (bytes_read != (ssize_t)len) {
		errno = EIO;
		return -1;
	}

	return 0;
}
/*
// functions：i2c writing data
// parameters：buf：Data buffer，len：length
// return: 0 success, -1 failed
*/
int i2c_writeNbyte(unsigned char *buf, int len) {
	if (validate_io(buf, len) < 0)
		return -1;

	const ssize_t bytes_written = write(fd, buf, (size_t)len);
	if (bytes_written < 0)
		return -1;
	if (bytes_written != (ssize_t)len) {
		errno = EIO;
		return -1;
	}

	return 0;
}
/*
// functions：read at specified address
// parameters：reg_addr： register address，buf：Data buffer
// return: 0 success, -1 failed
*/
int i2c_readReg(uint8_t reg_addr, uint8_t *data) {
	if (i2c_writeNbyte(&reg_addr, 1) < 0)
		return -1;

	return i2c_readNbyte(data, 1);
}
/*
// functions：write at specified address
// parameters：reg_addr： register address，buf：Data buffer
// return: 0 success, -1 failed
*/
int i2c_writeReg(uint8_t reg_addr, uint8_t data) {
	unsigned char buffer[2] = {reg_addr, data};
	return i2c_writeNbyte(buffer, 2);
}
/*
// functions：set frequency
// parameters：freq：value
// return: 0 success, -1 failed
*/
int PCA9685_setPWMFreq(float freq) {
	if (!(freq > 0.0f)) {
		errno = EINVAL;
		return -1;
	}

	freq *= 0.8449f; // Correct the overshoot of the frequency setting
	float prescaleval = 25000000; // 25MHz
	prescaleval /= 4096;		  // 4096 PWM cycles, 12-bit
	prescaleval /= freq;		  // Calculate the frequency division value
	prescaleval -= 1;
	const float rounded_prescale = prescaleval + 0.5f;
	if (rounded_prescale < 3.0f || rounded_prescale > 255.0f) {
		errno = EINVAL;
		return -1;
	}

	const uint8_t prescale = (uint8_t)rounded_prescale;
	uint8_t oldmode;
	if (i2c_readReg(PCA9685_MODE1, &oldmode) < 0)
		return -1;

	uint8_t newmode = (oldmode & 0x7F) | 0x10;
	if (i2c_writeReg(PCA9685_MODE1, newmode) < 0) // sleep
		return -1;
	if (i2c_writeReg(PCA9685_PRESCALE, prescale) < 0)
		return -1;
	if (i2c_writeReg(PCA9685_MODE1, oldmode) < 0)
		return -1;

	usleep(5000);
	return i2c_writeReg(
		PCA9685_MODE1,
		oldmode | 0xa0); // Set MODE1 to restart and enable autoincrement
}
/*
// functions：Set PWM value
// parameters：num：channel number，on：duty cycle high bytes，off： duty cycle
low bytes
// return: 0 success, -1 failed
*/
int PCA9685_setPWM(uint8_t num, uint16_t on, uint16_t off) {
	if (num > 15 || on > 4095 || off > 4095) {
		errno = EINVAL;
		return -1;
	}

	if (i2c_writeReg(LED0_ON_L + 4 * num, (uint8_t)on) < 0)
		return -1;
	if (i2c_writeReg(LED0_ON_L + 4 * num + 1, (uint8_t)(on >> 8)) < 0)
		return -1;
	if (i2c_writeReg(LED0_ON_L + 4 * num + 2, (uint8_t)off) < 0)
		return -1;

	return i2c_writeReg(LED0_ON_L + 4 * num + 3, (uint8_t)(off >> 8));
}
/*
// functions：Set servo pulse
// parameters：num：channel number，pulse：pulse value
// return: 0 success, -1 failed
*/
int setServoPulse(uint8_t n, double pulse) {
	if (!(pulse > 0.0)) {
		errno = EINVAL;
		return -1;
	}

	double pulselength;
	pulselength = 1000; // 1,000 ms per second
	pulselength /= 60;	// 60 Hz
	pulselength /= 4096;
	pulse *= 1000; // ms
	pulse /= pulselength;
	if (pulse > 4095.0) {
		errno = EINVAL;
		return -1;
	}

	return PCA9685_setPWM(n, 0, (uint16_t)pulse);
}
/*
// functions：Set the Angle of rotation of the servo
// parameters：n：channel number，Degree：angle value
// return: 0 success, -1 failed
*/
int setServoDegree(uint8_t n, uint8_t Degree) {
	if (Degree > 180) {
		errno = EINVAL;
		return -1;
	}

	// GH-S37D: 0deg=0.5ms, 90deg=1.5ms, 180deg=2.5ms
	// setServoPulse expects seconds, so divide by 1000
	double pulse_s = (0.5 + (2.0 * Degree / 180.0)) / 1000.0;
	return setServoPulse(n, pulse_s);
}
/*
// functions：getting key values
// parameters：no
// return：  key values
*/
int get_key_board_from_termios() {
	int key_value;
	struct termios new_config;
	struct termios old_config;

	tcgetattr(0, &old_config);
	new_config = old_config;
	new_config.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(0, TCSANOW, &new_config);
	key_value = getchar();
	tcsetattr(0, TCSANOW, &old_config);
	return key_value;
}
/*
// functions：Increases the degree of the specified channel
// parameters：channel：通道号，step：步数
// retrun：   key values
*/
int ServoDegreeIncrease(uint8_t Channel, uint8_t Step) {
	uint8_t *current_degree;
	int maximum_degree;

	switch (Channel) {
	case SERVO_UP_CH:
		current_degree = &ServoUpDegree;
		maximum_degree = SERVO_UP_MAX;
		break;
	case SERVO_DOWN_CH:
		current_degree = &ServoDownDegree;
		maximum_degree = SERVO_DOWN_MAX;
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	int requested_degree = (int)*current_degree + (int)Step;
	if (requested_degree > maximum_degree)
		requested_degree = maximum_degree;

	if (setServoDegree(Channel, (uint8_t)requested_degree) < 0)
		return -1;

	*current_degree = (uint8_t)requested_degree;
	usleep(STEP_DELAY * 1000);
	return 0;
}
/*
// functions：Increases the degree of the specified channel
// parameters：channel：通道号，step：步数
// retrun：   key values
*/
int ServoDegreeDecrease(uint8_t Channel, uint8_t Step) {
	uint8_t *current_degree;
	int minimum_degree;

	switch (Channel) {
	case SERVO_UP_CH:
		current_degree = &ServoUpDegree;
		minimum_degree = SERVO_UP_MIN;
		break;
	case SERVO_DOWN_CH:
		current_degree = &ServoDownDegree;
		minimum_degree = SERVO_DOWN_MIN;
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	int requested_degree = (int)*current_degree - (int)Step;
	if (requested_degree < minimum_degree)
		requested_degree = minimum_degree;

	if (setServoDegree(Channel, (uint8_t)requested_degree) < 0)
		return -1;

	*current_degree = (uint8_t)requested_degree;
	usleep(STEP_DELAY * 1000);
	return 0;
}
/*
// functions：keys control the steering gear rotation
// parameters：no
// retrun：no
*/
void processKeyboardEvent(void) {
	int keyVal = 0;
	while (1) {
		usleep(100);
		tcflush(0, TCIOFLUSH);
		keyVal = get_key_board_from_termios();
		if (keyVal == 27) {
			keyVal = get_key_board_from_termios();
			if (keyVal == 91) {
				keyVal = get_key_board_from_termios();
				switch (keyVal) {
				case 65 /* up */:
					ServoDegreeIncrease(SERVO_UP_CH, STEP);
					break;
				case 66 /* down */:
					ServoDegreeDecrease(SERVO_UP_CH, STEP);
					break;
				case 67 /* right */:
					ServoDegreeIncrease(SERVO_DOWN_CH, STEP);
					break;
				case 68 /* left */:
					ServoDegreeDecrease(SERVO_DOWN_CH, STEP);
					break;
				default:
					break;
				}
			}
		}
	}
}
