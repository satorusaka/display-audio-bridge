#define _GNU_SOURCE

#include <ddcutil_c_api.h>
#include <pulse/pulseaudio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define VCP_VOLUME 0x62
#define VCP_MUTE 0x8d
#define MAX_WATCHERS 32
#define DDC_RECOVERY_MIN_MS 2000
#define DDC_RECOVERY_MAX_MS 30000

struct shared_state {
	pthread_mutex_t mutex;
	pthread_cond_t changed;
	int volume;
	int maximum;
	bool muted;
	bool available;
	bool monitor_active;
	bool ddc_refresh;
	bool stopping;
	unsigned long generation;
	unsigned long confirmed_generation;
	int notify_fd;
	int poll_ms;
	int bus;
	char display_serial[128];
	char monitor_sink[256];
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signo) {
	(void)signo;
	stop_requested = 1;
}

static int clamp_int(int value, int minimum, int maximum) {
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return value;
}

static void notify_main(struct shared_state *state) {
	const uint8_t byte = 1;
	ssize_t ignored = write(state->notify_fd, &byte, sizeof(byte));
	(void)ignored;
}

static void log_ddc_error(const char *operation, DDCA_Status status) {
	fprintf(stderr, "ddc-volume-control: %s failed: %s (%s)\n",
	        operation, ddca_rc_name(status), ddca_rc_desc(status));
}

static DDCA_Status open_display(DDCA_Display_Ref display_ref,
                                DDCA_Display_Handle *handle) {
	return ddca_open_display2(display_ref, true, handle);
}

struct display_connection {
	DDCA_Display_Ref ref;
	bool invalid;
	int retry_ms;
	int64_t retry_after_ms;
};

static int64_t monotonic_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static DDCA_Status select_display(int bus, const char *serial,
                                  DDCA_Display_Ref *display_ref) {
	DDCA_Display_Identifier identifier = NULL;
	DDCA_Status status;
	if (serial && *serial)
		status = ddca_create_mfg_model_sn_display_identifier(
		    NULL, NULL, serial, &identifier);
	else
		status = ddca_create_busno_display_identifier(bus, &identifier);
	if (status == 0)
		status = ddca_get_display_ref(identifier, display_ref);
	ddca_free_display_identifier(identifier);
	if (status == 0)
		ddca_set_display_sleep_multiplier(*display_ref, 0.0);
	return status;
}

static void display_failed(struct display_connection *display) {
	display->invalid = true;
	display->retry_after_ms = monotonic_ms() + display->retry_ms;
	if (display->retry_ms < DDC_RECOVERY_MAX_MS) {
		display->retry_ms *= 2;
		if (display->retry_ms > DDC_RECOVERY_MAX_MS)
			display->retry_ms = DDC_RECOVERY_MAX_MS;
	}
}

static void display_succeeded(struct display_connection *display) {
	display->invalid = false;
	display->retry_ms = DDC_RECOVERY_MIN_MS;
	display->retry_after_ms = 0;
}

static bool prepare_display(struct display_connection *display, int bus,
                            const char *serial) {
	if (!display->invalid)
		return true;
	if (monotonic_ms() < display->retry_after_ms)
		return false;

	fprintf(stderr,
	        "ddc-volume-control: redetecting monitor after DDC failure\n");
	DDCA_Status status = ddca_redetect_displays();
	if (status == 0)
		status = select_display(bus, serial, &display->ref);
	if (status != 0) {
		log_ddc_error("redetect display", status);
		display_failed(display);
		return false;
	}
	/*
	 * A successful rescan only proves that the display was enumerated.  Keep
	 * the current backoff until a real VCP transaction succeeds; some monitors
	 * reappear in DRM several seconds before their DDC controller is awake.
	 */
	display->invalid = false;
	return true;
}

static int read_non_table(DDCA_Display_Handle handle, uint8_t code,
                          int *current, int *maximum) {
	DDCA_Non_Table_Vcp_Value value = {0};
	DDCA_Status status = ddca_get_non_table_vcp_value(handle, code, &value);
	if (status != 0) {
		log_ddc_error("read VCP value", status);
		return -1;
	}
	if (current)
		*current = ((int)value.sh << 8) | value.sl;
	if (maximum)
		*maximum = ((int)value.mh << 8) | value.ml;
	return 0;
}

static int read_hardware_state(DDCA_Display_Ref display_ref, int *volume,
                               int *maximum, bool *muted) {
	DDCA_Display_Handle handle = NULL;
	DDCA_Status status = open_display(display_ref, &handle);
	if (status != 0) {
		log_ddc_error("open display", status);
		return -1;
	}

	int mute_value = 2;
	int result = read_non_table(handle, VCP_VOLUME, volume, maximum);
	if (result == 0)
		result = read_non_table(handle, VCP_MUTE, &mute_value, NULL);

	status = ddca_close_display(handle);
	if (status != 0)
		log_ddc_error("close display", status);

	if (result == 0)
		*muted = mute_value == 1;
	return result;
}

static int write_hardware_state(DDCA_Display_Ref display_ref, int volume,
                                bool muted, bool write_volume,
                                bool write_mute) {
	DDCA_Display_Handle handle = NULL;
	DDCA_Status status = open_display(display_ref, &handle);
	if (status != 0) {
		log_ddc_error("open display", status);
		return -1;
	}

	int result = 0;
	if (write_volume) {
		status = ddca_set_non_table_vcp_value2(
		    handle, VCP_VOLUME, (uint8_t)((volume >> 8) & 0xff),
		    (uint8_t)(volume & 0xff));
		if (status != 0) {
			log_ddc_error("set monitor volume", status);
			result = -1;
		}
	}

	if (result == 0 && write_mute) {
		status = ddca_set_non_table_vcp_value2(
		    handle, VCP_MUTE, 0, muted ? 1 : 2);
		if (status != 0) {
			log_ddc_error("set monitor mute", status);
			result = -1;
		}
	}

	status = ddca_close_display(handle);
	if (status != 0)
		log_ddc_error("close display", status);
	return result;
}

static struct timespec deadline_after_ms(int milliseconds) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += milliseconds / 1000;
	deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}
	return deadline;
}

static void *ddc_worker(void *opaque) {
	struct shared_state *state = opaque;
	DDCA_Status status = ddca_init2(
	    NULL, DDCA_SYSLOG_WARNING, DDCA_INIT_OPTIONS_DISABLE_CONFIG_FILE, NULL);
	if (status != 0) {
		log_ddc_error("initialize libddcutil", status);
		return NULL;
	}

	ddca_enable_verify(false);
	ddca_enable_dynamic_sleep(false);
	/*
	 * ddca_redetect_displays() writes progress diagnostics through libddcutil's
	 * per-thread streams.  libddcutil 2.2.7 dereferences a NULL stream there,
	 * so keep valid streams installed for automatic hotplug recovery.
	 */
	ddca_set_fout(stderr);
	ddca_set_ferr(stderr);

	struct display_connection display = {
	    .ref = NULL,
	    .invalid = false,
	    .retry_ms = DDC_RECOVERY_MIN_MS,
	    .retry_after_ms = 0,
	};
	status = select_display(state->bus, state->display_serial, &display.ref);
	if (status != 0) {
		log_ddc_error("select display by I2C bus", status);
		display_failed(&display);
	}

	int confirmed_volume = 0;
	int confirmed_maximum = 100;
	bool confirmed_muted = false;
	unsigned long handled_generation = 0;
	int written_volume = 0;
	bool written_muted = false;

	for (;;) {
		pthread_mutex_lock(&state->mutex);
		while (!state->stopping && !state->monitor_active)
			pthread_cond_wait(&state->changed, &state->mutex);
		if (state->stopping) {
			pthread_mutex_unlock(&state->mutex);
			break;
		}

		bool refresh = state->ddc_refresh;
		state->ddc_refresh = false;
		if (refresh) {
			pthread_mutex_unlock(&state->mutex);
			int initial_volume = 0;
			int initial_maximum = 100;
			bool initial_muted = false;
			int result = -1;
			if (prepare_display(&display, state->bus,
			                    state->display_serial))
				result = read_hardware_state(
				    display.ref, &initial_volume, &initial_maximum,
				    &initial_muted);
			if (result == 0)
				display_succeeded(&display);
			else if (!display.invalid)
				display_failed(&display);
			pthread_mutex_lock(&state->mutex);
			if (state->monitor_active) {
				if (result == 0) {
					confirmed_volume = initial_volume;
					confirmed_maximum =
					    initial_maximum > 0 ? initial_maximum : 100;
					confirmed_muted = initial_muted;
					written_volume = initial_volume;
					written_muted = initial_muted;
					handled_generation = state->generation;
					state->volume = initial_volume;
					state->maximum = confirmed_maximum;
					state->muted = initial_muted;
					state->available = true;
					state->confirmed_generation = state->generation;
				} else {
					state->available = false;
				}
			}
			pthread_mutex_unlock(&state->mutex);
			notify_main(state);
			continue;
		}

		struct timespec deadline = deadline_after_ms(state->poll_ms);
		while (!state->stopping && state->monitor_active &&
		       state->generation == handled_generation &&
		       !state->ddc_refresh) {
			int wait_result =
			    pthread_cond_timedwait(&state->changed, &state->mutex, &deadline);
			if (wait_result == ETIMEDOUT)
				break;
		}
		if (state->stopping) {
			pthread_mutex_unlock(&state->mutex);
			break;
		}
		if (!state->monitor_active || state->ddc_refresh) {
			pthread_mutex_unlock(&state->mutex);
			continue;
		}

		if (state->generation != handled_generation) {
			unsigned long target_generation = state->generation;
			int target_volume = state->volume;
			bool target_muted = state->muted;
			pthread_mutex_unlock(&state->mutex);

			bool change_volume = target_volume != written_volume;
			bool change_mute = target_muted != written_muted;
			int result = -1;
			if (prepare_display(&display, state->bus,
			                    state->display_serial))
				result = write_hardware_state(
				    display.ref, target_volume, target_muted,
				    change_volume, change_mute);
			if (result == 0)
				display_succeeded(&display);
			else if (!display.invalid)
				display_failed(&display);

			pthread_mutex_lock(&state->mutex);
			handled_generation = target_generation;
			if (result == 0 && state->monitor_active) {
				written_volume = target_volume;
				written_muted = target_muted;
				confirmed_volume = target_volume;
				confirmed_muted = target_muted;
				state->available = true;
				state->confirmed_generation = target_generation;
			} else if (state->monitor_active) {
				state->available = false;
				if (state->generation == target_generation) {
					state->volume = confirmed_volume;
					state->muted = confirmed_muted;
				}
			}
			pthread_mutex_unlock(&state->mutex);
			notify_main(state);
			continue;
		}
		pthread_mutex_unlock(&state->mutex);

		int polled_volume = 0;
		int polled_maximum = 100;
		bool polled_muted = false;
		int poll_result = -1;
		if (prepare_display(&display, state->bus, state->display_serial))
			poll_result =
			    read_hardware_state(display.ref, &polled_volume,
			                        &polled_maximum, &polled_muted);
		if (poll_result == 0)
			display_succeeded(&display);
		else if (!display.invalid)
			display_failed(&display);
		if (poll_result == 0) {
			pthread_mutex_lock(&state->mutex);
			if (state->monitor_active &&
			    state->generation == handled_generation) {
				confirmed_volume = polled_volume;
				confirmed_muted = polled_muted;
				written_volume = polled_volume;
				written_muted = polled_muted;
				state->volume = polled_volume;
				state->maximum =
				    polled_maximum > 0 ? polled_maximum : state->maximum;
				state->muted = polled_muted;
				state->available = true;
			}
			pthread_mutex_unlock(&state->mutex);
			notify_main(state);
		} else {
			pthread_mutex_lock(&state->mutex);
			if (state->monitor_active)
				state->available = false;
			pthread_mutex_unlock(&state->mutex);
			notify_main(state);
		}
	}
	return NULL;
}

struct pulse_control {
	pa_threaded_mainloop *mainloop;
	pa_context *context;
	struct shared_state *state;
};

static bool pulse_sink_is_monitor(const struct pulse_control *pulse,
                                  const pa_sink_info *info) {
	return pulse->state->monitor_sink[0] && info->name &&
	       strcmp(info->name, pulse->state->monitor_sink) == 0;
}

static void default_sink_info(pa_context *context, const pa_sink_info *info,
                              int eol, void *userdata) {
	struct pulse_control *pulse = userdata;
	if (eol || !info)
		return;

	bool monitor = pulse_sink_is_monitor(pulse, info);
	bool became_monitor = false;

	pthread_mutex_lock(&pulse->state->mutex);
	if (monitor) {
		if (!pulse->state->monitor_active) {
			pulse->state->monitor_active = true;
			pulse->state->ddc_refresh = true;
			pulse->state->available = false;
			became_monitor = true;
			pthread_cond_signal(&pulse->state->changed);
		}
	} else {
		pulse->state->monitor_active = false;
		pulse->state->ddc_refresh = false;
		pulse->state->volume = clamp_int(
		    (int)(((uint64_t)pa_cvolume_avg(&info->volume) * 100 +
		           PA_VOLUME_NORM / 2) /
		          PA_VOLUME_NORM),
		    0, 100);
		pulse->state->maximum = 100;
		pulse->state->muted = info->mute != 0;
		pulse->state->available = true;
		pthread_cond_signal(&pulse->state->changed);
	}
	pthread_mutex_unlock(&pulse->state->mutex);
	notify_main(pulse->state);

	if (!monitor)
		return;

	bool volume_is_normal = true;
	for (uint8_t channel = 0; channel < info->volume.channels; channel++)
		if (info->volume.values[channel] != PA_VOLUME_NORM)
			volume_is_normal = false;

	if (!volume_is_normal) {
		pa_cvolume normal;
		pa_cvolume_set(&normal, info->channel_map.channels, PA_VOLUME_NORM);
		pa_operation *operation =
		    pa_context_set_sink_volume_by_index(context, info->index, &normal,
		                                        NULL, NULL);
		if (operation)
			pa_operation_unref(operation);
	}
	if (info->mute) {
		pa_operation *operation =
		    pa_context_set_sink_mute_by_index(context, info->index, 0, NULL, NULL);
		if (operation)
			pa_operation_unref(operation);
	}
	if (became_monitor)
		fprintf(stderr,
		        "ddc-volume-control: selected configured monitor backend (DDC)\n");
}

static void default_server_info(pa_context *context,
                                const pa_server_info *info, void *userdata) {
	if (!info || !info->default_sink_name)
		return;
	pa_operation *operation =
	    pa_context_get_sink_info_by_name(context, info->default_sink_name,
	                                     default_sink_info, userdata);
	if (operation)
		pa_operation_unref(operation);
}

static void request_default_sink(pa_context *context,
                                 struct pulse_control *pulse) {
	pa_operation *operation =
	    pa_context_get_server_info(context, default_server_info, pulse);
	if (operation)
		pa_operation_unref(operation);
}

static void pulse_subscribe(pa_context *context,
                            pa_subscription_event_type_t event_type,
                            uint32_t index, void *userdata) {
	(void)index;
	(void)userdata;
	pa_subscription_event_type_t facility =
	    event_type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	if (facility == PA_SUBSCRIPTION_EVENT_SINK ||
	    facility == PA_SUBSCRIPTION_EVENT_SERVER)
		request_default_sink(context, userdata);
}

static void pulse_state_changed(pa_context *context, void *userdata) {
	struct pulse_control *pulse = userdata;
	switch (pa_context_get_state(context)) {
	case PA_CONTEXT_READY: {
		pa_context_set_subscribe_callback(context, pulse_subscribe, pulse);
		pa_operation *operation = pa_context_subscribe(
		    context, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER,
		    NULL, NULL);
		if (operation)
			pa_operation_unref(operation);
		request_default_sink(context, pulse);
		break;
	}
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		if (!stop_requested)
			fprintf(stderr,
			        "ddc-volume-control: PipeWire/PulseAudio connection unavailable\n");
		break;
	default:
		break;
	}
}

static int start_pulse_control(struct pulse_control *pulse,
                               struct shared_state *state) {
	memset(pulse, 0, sizeof(*pulse));
	pulse->state = state;
	pulse->mainloop = pa_threaded_mainloop_new();
	if (!pulse->mainloop)
		return -1;
	pa_mainloop_api *api = pa_threaded_mainloop_get_api(pulse->mainloop);
	pulse->context = pa_context_new(api, "ddc-volume-control");
	if (!pulse->context)
		return -1;
	pa_context_set_state_callback(pulse->context, pulse_state_changed, pulse);
	if (pa_context_connect(pulse->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
		return -1;
	if (pa_threaded_mainloop_start(pulse->mainloop) < 0)
		return -1;
	return 0;
}

static void stop_pulse_control(struct pulse_control *pulse) {
	if (!pulse->mainloop)
		return;
	pa_threaded_mainloop_stop(pulse->mainloop);
	if (pulse->context) {
		pa_context_disconnect(pulse->context);
		pa_context_unref(pulse->context);
	}
	pa_threaded_mainloop_free(pulse->mainloop);
}

static const char *socket_path(void) {
	static char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !*runtime) {
		fprintf(stderr, "ddc-volume-control: XDG_RUNTIME_DIR is not set\n");
		return NULL;
	}
	if (snprintf(path, sizeof(path), "%s/ddc-volume-control.sock", runtime) >=
	    (int)sizeof(path)) {
		fprintf(stderr, "ddc-volume-control: socket path is too long\n");
		return NULL;
	}
	return path;
}

static int create_server_socket(void) {
	const char *path = socket_path();
	if (!path)
		return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    listen(fd, 16) < 0) {
		perror("ddc-volume-control: create socket");
		close(fd);
		return -1;
	}
	chmod(path, 0600);
	return fd;
}

static void format_state(struct shared_state *state, char *buffer,
                         size_t size) {
	pthread_mutex_lock(&state->mutex);
	snprintf(buffer, size, "STATE %d %d %d %d %lu %s\n", state->volume,
	         state->maximum, state->muted ? 1 : 0,
	         state->available ? 1 : 0, state->generation,
	         state->monitor_active ? "ddc" : "pipewire");
	pthread_mutex_unlock(&state->mutex);
}

static void send_state(int fd, struct shared_state *state) {
	char buffer[128];
	format_state(state, buffer, sizeof(buffer));
	send(fd, buffer, strlen(buffer), MSG_NOSIGNAL);
}

static void broadcast_state(int watchers[], struct shared_state *state) {
	for (int index = 0; index < MAX_WATCHERS; index++) {
		if (watchers[index] < 0)
			continue;
		char buffer[128];
		format_state(state, buffer, sizeof(buffer));
		if (send(watchers[index], buffer, strlen(buffer),
		         MSG_NOSIGNAL | MSG_DONTWAIT) < 0) {
			close(watchers[index]);
			watchers[index] = -1;
		}
	}
}

enum command_result {
	COMMAND_INVALID,
	COMMAND_DDC,
	COMMAND_PIPEWIRE,
};

static enum command_result apply_command(struct shared_state *state,
                                         const char *command) {
	pthread_mutex_lock(&state->mutex);
	int volume = state->volume;
	bool muted = state->muted;
	bool recognized = true;

	if (strcmp(command, "UP") == 0)
		volume += 5;
	else if (strcmp(command, "DOWN") == 0)
		volume -= 5;
	else if (strcmp(command, "MUTE") == 0)
		muted = !muted;
	else if (strncmp(command, "SET ", 4) == 0) {
		char *end = NULL;
		long requested = strtol(command + 4, &end, 10);
		if (!end || *end != '\0')
			recognized = false;
		else
			volume = (int)requested;
	} else
		recognized = false;

	if (!recognized) {
		pthread_mutex_unlock(&state->mutex);
		return COMMAND_INVALID;
	}
	if (!state->monitor_active) {
		pthread_mutex_unlock(&state->mutex);
		return COMMAND_PIPEWIRE;
	}

	state->volume = clamp_int(volume, 0, state->maximum);
	state->muted = muted;
	state->generation++;
	pthread_cond_signal(&state->changed);
	pthread_mutex_unlock(&state->mutex);
	notify_main(state);
	return COMMAND_DDC;
}

static int run_daemon(int bus, int poll_ms, const char *display_serial,
                      const char *monitor_sink) {
	int notify_pipe[2];
	if (pipe2(notify_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
		perror("ddc-volume-control: pipe");
		return 1;
	}

	struct shared_state state = {
	    .mutex = PTHREAD_MUTEX_INITIALIZER,
	    .changed = PTHREAD_COND_INITIALIZER,
	    .volume = 0,
	    .maximum = 100,
	    .muted = false,
	    .available = false,
	    .notify_fd = notify_pipe[1],
	    .poll_ms = poll_ms,
	    .bus = bus,
	};
	snprintf(state.display_serial, sizeof(state.display_serial), "%s",
	         display_serial ? display_serial : "");
	snprintf(state.monitor_sink, sizeof(state.monitor_sink), "%s",
	         monitor_sink ? monitor_sink : "");

	int server = create_server_socket();
	if (server < 0)
		return 1;

	pthread_t worker;
	if (pthread_create(&worker, NULL, ddc_worker, &state) != 0) {
		perror("ddc-volume-control: create DDC worker");
		return 1;
	}

	struct pulse_control pulse;
	if (start_pulse_control(&pulse, &state) < 0)
		fprintf(stderr,
		        "ddc-volume-control: unable to enforce PipeWire master volume\n");

	int watchers[MAX_WATCHERS];
	for (int index = 0; index < MAX_WATCHERS; index++)
		watchers[index] = -1;

	if (state.display_serial[0])
		fprintf(stderr,
		        "ddc-volume-control: running for display serial %s, sink %s, idle poll %d ms\n",
		        state.display_serial,
		        state.monitor_sink[0] ? state.monitor_sink : "legacy auto-match",
		        poll_ms);
	else
		fprintf(stderr,
		        "ddc-volume-control: running on I2C bus %d, sink %s, idle poll %d ms\n",
		        bus,
		        state.monitor_sink[0] ? state.monitor_sink : "legacy auto-match",
		        poll_ms);

	while (!stop_requested) {
		struct pollfd fds[2] = {
		    {.fd = server, .events = POLLIN},
		    {.fd = notify_pipe[0], .events = POLLIN},
		};
		int ready = poll(fds, 2, 500);
		if (ready < 0 && errno != EINTR) {
			perror("ddc-volume-control: poll");
			break;
		}
		if (fds[1].revents & POLLIN) {
			uint8_t bytes[64];
			while (read(notify_pipe[0], bytes, sizeof(bytes)) > 0)
				;
			broadcast_state(watchers, &state);
		}
		if (!(fds[0].revents & POLLIN))
			continue;

		int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
		if (client < 0)
			continue;
		char command[128] = {0};
		ssize_t length = recv(client, command, sizeof(command) - 1, 0);
		if (length <= 0) {
			close(client);
			continue;
		}
		command[strcspn(command, "\r\n")] = '\0';

		if (strcmp(command, "WATCH") == 0) {
			bool added = false;
			for (int index = 0; index < MAX_WATCHERS; index++) {
				if (watchers[index] < 0) {
					watchers[index] = client;
					added = true;
					break;
				}
			}
			if (added)
				send_state(client, &state);
			else
				close(client);
		} else {
			if (strcmp(command, "GET") == 0) {
				send_state(client, &state);
			} else {
				enum command_result result = apply_command(&state, command);
				if (result == COMMAND_INVALID)
					send(client, "ERROR invalid command\n", 22,
					     MSG_NOSIGNAL);
				else if (result == COMMAND_PIPEWIRE)
					send(client, "PIPEWIRE\n", 9, MSG_NOSIGNAL);
				else
					send_state(client, &state);
			}
			close(client);
		}
	}

	pthread_mutex_lock(&state.mutex);
	state.stopping = true;
	pthread_cond_signal(&state.changed);
	pthread_mutex_unlock(&state.mutex);
	pthread_join(worker, NULL);
	stop_pulse_control(&pulse);
	for (int index = 0; index < MAX_WATCHERS; index++)
		if (watchers[index] >= 0)
			close(watchers[index]);
	close(server);
	close(notify_pipe[0]);
	close(notify_pipe[1]);
	unlink(socket_path());
	return 0;
}

static int connect_client(void) {
	const char *path = socket_path();
	if (!path)
		return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		fprintf(stderr, "ddc-volume-control: daemon unavailable: %s\n",
		        strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void show_osd_from_state(const char *state_line) {
	int volume = 0, maximum = 100, muted = 0, available = 0;
	if (sscanf(state_line, "STATE %d %d %d %d", &volume, &maximum, &muted,
	           &available) != 4 ||
	    !available)
		return;
	int percent = maximum > 0 ? (volume * 100 + maximum / 2) / maximum : 0;
	if (muted)
		percent = 0;
	char value[16];
	snprintf(value, sizeof(value), "%d", percent);
	pid_t child = fork();
	if (child == 0) {
		execlp("noctalia", "noctalia", "msg", "volume-osd", value,
		       (char *)NULL);
		_exit(127);
	}
}

static int run_client(const char *command, bool watch, bool osd) {
	for (;;) {
		int fd = connect_client();
		if (fd < 0) {
			if (!watch)
				return 1;
			sleep(1);
			continue;
		}
		if (send(fd, command, strlen(command), MSG_NOSIGNAL) < 0) {
			close(fd);
			return 1;
		}
		char response[256];
		ssize_t length;
		while ((length = recv(fd, response, sizeof(response) - 1, 0)) > 0) {
			response[length] = '\0';
			fputs(response, stdout);
			fflush(stdout);
			if (osd)
				show_osd_from_state(response);
			if (!watch)
				break;
		}
		close(fd);
		if (!watch)
			return length > 0 ? 0 : 1;
		sleep(1);
	}
}

static void usage(FILE *stream) {
	fprintf(stream,
	        "Usage:\n"
	        "  ddc-volume-control daemon [--display-serial SERIAL] "
	        "[--monitor-sink NAME] [--bus N] [--poll-ms N]\n"
	        "  ddc-volume-control get|watch|up|down|mute [--osd]\n"
	        "  ddc-volume-control set PERCENT [--osd]\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		usage(stderr);
		return 2;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGCHLD, SIG_IGN);

	if (strcmp(argv[1], "daemon") == 0) {
		int bus = -1;
		int poll_ms = 2000;
		const char *display_serial = NULL;
		const char *monitor_sink = NULL;
		for (int index = 2; index < argc; index++) {
			if (strcmp(argv[index], "--bus") == 0 && index + 1 < argc)
				bus = atoi(argv[++index]);
			else if (strcmp(argv[index], "--display-serial") == 0 &&
			         index + 1 < argc)
				display_serial = argv[++index];
			else if (strcmp(argv[index], "--monitor-sink") == 0 &&
			         index + 1 < argc)
				monitor_sink = argv[++index];
			else if (strcmp(argv[index], "--poll-ms") == 0 &&
			         index + 1 < argc)
				poll_ms = atoi(argv[++index]);
			else {
				usage(stderr);
				return 2;
			}
		}
		if (((!display_serial || !*display_serial) && bus < 0) ||
		    !monitor_sink || !*monitor_sink || poll_ms < 250) {
			fprintf(stderr,
			        "ddc-volume-control: a display serial or bus and a monitor sink are required\n");
			return 2;
		}
		return run_daemon(bus, poll_ms, display_serial, monitor_sink);
	}

	bool osd = false;
	for (int index = 2; index < argc; index++)
		if (strcmp(argv[index], "--osd") == 0)
			osd = true;

	if (strcmp(argv[1], "get") == 0)
		return run_client("GET", false, false);
	if (strcmp(argv[1], "watch") == 0)
		return run_client("WATCH", true, false);
	if (strcmp(argv[1], "up") == 0)
		return run_client("UP", false, osd);
	if (strcmp(argv[1], "down") == 0)
		return run_client("DOWN", false, osd);
	if (strcmp(argv[1], "mute") == 0)
		return run_client("MUTE", false, osd);
	if (strcmp(argv[1], "set") == 0 && argc >= 3) {
		char command[64];
		snprintf(command, sizeof(command), "SET %s", argv[2]);
		return run_client(command, false, osd);
	}

	usage(stderr);
	return 2;
}
