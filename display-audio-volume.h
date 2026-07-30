#ifndef DISPLAY_AUDIO_VOLUME_H
#define DISPLAY_AUDIO_VOLUME_H

#include <math.h>

static inline int da_clamp(int value, int minimum, int maximum) {
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

static inline int da_percent_to_hardware(int minimum, int maximum,
                                         double curve, int percent) {
	percent = da_clamp(percent, 0, 100);
	if (percent == 0)
		return minimum;
	double normalized = pow((double)percent / 100.0, curve);
	return da_clamp(
	    (int)lround(minimum + (maximum - minimum) * normalized),
	    minimum, maximum);
}

static inline int da_hardware_to_percent(int minimum, int maximum,
                                         double curve, int value) {
	value = da_clamp(value, minimum, maximum);
	double range = maximum - minimum;
	if (range <= 0.0)
		return 0;
	double normalized = (value - minimum) / range;
	return da_clamp((int)lround(pow(normalized, 1.0 / curve) * 100.0),
	                0, 100);
}

#endif
