#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

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

static int connect_client(void) {
	const char *path = socket_path();
	if (!path)
		return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
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

static void show_pipewire_osd(void) {
	pid_t child = fork();
	if (child == 0) {
		execlp("noctalia", "noctalia", "msg", "volume-osd", (char *)NULL);
		_exit(127);
	}
}

static int run_pipewire_command(const char *command, bool osd) {
	const char *program = "wpctl";
	char value[32];
	pid_t child = fork();
	if (child < 0)
		return 1;
	if (child == 0) {
		if (strcmp(command, "UP") == 0)
			execlp(program, program, "set-volume", "-l", "1",
			       "@DEFAULT_AUDIO_SINK@", "5%+", (char *)NULL);
		else if (strcmp(command, "DOWN") == 0)
			execlp(program, program, "set-volume", "-l", "1",
			       "@DEFAULT_AUDIO_SINK@", "5%-", (char *)NULL);
		else if (strcmp(command, "MUTE") == 0)
			execlp(program, program, "set-mute", "@DEFAULT_AUDIO_SINK@",
			       "toggle", (char *)NULL);
		else if (sscanf(command, "SET %31s", value) == 1) {
			strncat(value, "%", sizeof(value) - strlen(value) - 1);
			execlp(program, program, "set-volume", "-l", "1",
			       "@DEFAULT_AUDIO_SINK@", value, (char *)NULL);
		}
		_exit(127);
	}

	int status = 0;
	if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)
		return 1;
	if (osd)
		show_pipewire_osd();
	return 0;
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
			if (!watch && strcmp(response, "PIPEWIRE\n") == 0) {
				close(fd);
				return run_pipewire_command(command, osd);
			}
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
	        "  ddc-volume-control get|watch|up|down|mute [--osd]\n"
	        "  ddc-volume-control set PERCENT [--osd]\n");
}

int main(int argc, char **argv) {
	if (argc < 2) {
		usage(stderr);
		return 2;
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
		char *end = NULL;
		long requested = strtol(argv[2], &end, 10);
		if (!end || *end != '\0' || requested < 0 || requested > 100) {
			fprintf(stderr, "ddc-volume-control: volume must be 0-100\n");
			return 2;
		}
		snprintf(command, sizeof(command), "SET %ld", requested);
		return run_client(command, false, osd);
	}

	usage(stderr);
	return 2;
}
