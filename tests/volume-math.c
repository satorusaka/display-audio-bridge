#include <pulse/volume.h>

#include <stdio.h>
#include <stdlib.h>

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
	return 0;
}
