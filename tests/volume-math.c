#include <pulse/volume.h>

#include <stdio.h>
#include <stdlib.h>

#include "../display-audio-volume.h"

int main(void) {
	for (int percent = 1; percent <= 100; percent++) {
		pa_volume_t requested =
		    (pa_volume_t)(((uint64_t)percent * PA_VOLUME_NORM + 50) / 100);
		pa_volume_t compensation =
		    pa_sw_volume_divide(PA_VOLUME_NORM, requested);
		pa_volume_t combined =
		    pa_sw_volume_multiply(requested, compensation);
		long error = labs((long)combined - PA_VOLUME_NORM);
		if (error > 1) {
			fprintf(stderr,
			        "%d%% compensation did not return unity: %u\n",
			        percent, combined);
			return 1;
		}
	}
	for (int curve_index = 1; curve_index <= 8; curve_index++) {
		double curve = curve_index * 0.5;
		int previous = 10;
		for (int percent = 0; percent <= 100; percent++) {
			int hardware =
			    da_percent_to_hardware(10, 80, curve, percent);
			if (hardware < previous || hardware < 10 || hardware > 80)
				return 1;
			previous = hardware;
		}
		if (da_percent_to_hardware(10, 80, curve, 100) != 80)
			return 1;
		int previous_percent = 0;
		for (int hardware = 10; hardware <= 80; hardware++) {
			int percent =
			    da_hardware_to_percent(10, 80, curve, hardware);
			if (percent < previous_percent || percent < 0 ||
			    percent > 100)
				return 1;
			previous_percent = percent;
		}
	}
	return 0;
}
