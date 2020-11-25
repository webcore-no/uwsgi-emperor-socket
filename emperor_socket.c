#include "poll.h"
#include "pthread.h"
#include "uwsgi.h"
#include <fcntl.h>
#include <sys/socket.h>

// configurable variables
int queue_length = 100;
char *socket_name;

struct uwsgi_option uwsgi_emperor_socket_options[] = {
	{ "empsoc-socket", required_argument, 0,
	  "set the socket name for emperor_socket", uwsgi_opt_set_str,
	  &socket_name, 0 },
	{ "empsoc-queue", required_argument, 0,
	  "set the queue length for emperor_socket", uwsgi_opt_set_int,
	  &queue_length, 0 },
	UWSGI_END_OF_OPTIONS
};

void emperor_del(struct uwsgi_instance *c_ui);

char *socket_addr;

extern struct uwsgi_server uwsgi;

struct filedescriptor_node {
	int fd;
	struct filedescriptor_node *next;
};

struct spawn {
	struct filedescriptor_node *fd;
	char *vassal_socket;
	char *vassal_name;
	time_t last_spawn;
	struct spawn *next;
	pthread_mutex_t spawn_lock;
};

int queue = 0;
struct spawn *spawn_list;

struct socket_monitor_command {
	char *cmd;
	uint16_t cmd_len;
	char *vassal;
	uint16_t vassal_len;
	char *attrs;
	uint16_t attrs_len;
	char *socket;
	uint16_t socket_len;
	char *config;
	uint16_t config_len;
};

static void socket_monitor_command_parser(char *key, uint16_t keylen, char *val,
					  uint16_t vallen, void *data)
{
	struct socket_monitor_command *smc =
		(struct socket_monitor_command *)data;

	if (!uwsgi_strncmp("cmd", 3, key, keylen)) {
		smc->cmd = val;
		smc->cmd_len = vallen;
	}
	else if (!uwsgi_strncmp("vassal", 6, key, keylen)) {
		smc->vassal = val;
		smc->vassal_len = vallen;
	}
	else if (!uwsgi_strncmp("attrs", 5, key, keylen)) {
		smc->attrs = val;
		smc->attrs_len = vallen;
	}
	else if (!uwsgi_strncmp("socket", 6, key, keylen)) {
		smc->socket = val;
		smc->socket_len = vallen;
	}
	else if (!uwsgi_strncmp("config", 6, key, keylen)) {
		smc->config = val;
		smc->config_len = vallen;
	}
}

static void socket_monitor_attrs_parser(char *key, uint16_t keylen, char *val,
					uint16_t vallen, void *data)
{
	/*
	   This is done a bit odd, since uwsgi_dyn_dict_free tries to free
	   only value, hence it must be the start of the allocation.
	   Also, even tho uwsgi_dyn_dict has includes len, fork server
	   socket is used in emperor without len, so it needs terminating 0-byte
	 */

	char *kv = uwsgi_malloc(keylen + vallen + 2);
	memset(kv, 0, keylen + vallen + 2);
	memcpy(kv, val, vallen);
	memcpy(kv + vallen + 1, key, keylen);
	uwsgi_dyn_dict_new(data, kv + vallen + 1, keylen + 1, kv, vallen + 1);
}

void uwsgi_imperial_monitor_socket_event(struct uwsgi_emperor_scanner *ues)
{
	int client_fd;
	client_fd = accept(ues->fd, NULL, NULL);
	if (client_fd < 0) {
		uwsgi_error("uwsgi_imperial_monitor_socket_event()/accept()");
		return;
	}
	while (client_fd > 0) {
		struct uwsgi_dyn_dict *attrs = NULL;
		struct uwsgi_instance *ui_current;

		size_t buf_len = uwsgi.page_size;
		char *buf = uwsgi_malloc(buf_len);

		if (uwsgi_read_with_realloc(client_fd, &buf, &buf_len,
					    uwsgi.socket_timeout, NULL, NULL)) {
			uwsgi_error(
				"uwsgi_imperial_monitor_socket_event()/"
				"uwsgi_read_realloc()");
			goto OK;
		}

		struct socket_monitor_command smc;
		memset(&smc, 0, sizeof(struct socket_monitor_command));

		if (uwsgi_hooked_parse(buf, buf_len,
				       socket_monitor_command_parser, &smc)) {
			uwsgi_log_verbose(
				"[socket-monitor] uwsgi_hooked_parse\n");
		}
		if (!uwsgi_strncmp(smc.cmd, smc.cmd_len, "spawn", 5)) {

			if (!smc.vassal) {
				uwsgi_log_verbose(
					"[socket-monitor] vassal name missing");
				if (write(client_fd, "-vassal missing\n", 16) !=
				    16) {
					uwsgi_error(
						"uwsgi_imperial_monitor_socket_"
						"event()/write()");
				}
			}

			if (smc.attrs) {
				if (uwsgi_hooked_parse(
					    smc.attrs, smc.attrs_len,
					    socket_monitor_attrs_parser,
					    &attrs)) {
					uwsgi_log_verbose(
						"[socket-monitor] invalid "
						"attributes\n");
				}
			}
			char *config = NULL;
			if (smc.config) {
				config = uwsgi_strncopy(smc.config,
							smc.config_len);
			}

			char *socket = NULL;
			if (smc.socket) {
				socket = uwsgi_strncopy(smc.socket,
							smc.socket_len);
			}

			char *vassal_name =
				uwsgi_strncopy(smc.vassal, smc.vassal_len);
			uwsgi_log_verbose(
				"[socket-monitor] spawn request for %s\n",
				vassal_name);

			ui_current = emperor_get(vassal_name);

			if (ui_current) {
				int fd = ui_current->on_demand_fd;
				ui_current->on_demand_fd = -1;
				emperor_stop(ui_current);

				struct uwsgi_instance *n_ui = NULL;

				while (ui_current->ui_next) {
					ui_current = ui_current->ui_next;
				}

				n_ui = uwsgi_calloc(
					sizeof(struct uwsgi_instance));

				if (config) {
					n_ui->use_config = 1;
					n_ui->config = config;
					n_ui->config_len = smc.config_len;
				}

				ui_current->ui_next = n_ui;
				n_ui->ui_prev = ui_current;

				n_ui->scanner = ues;
				memcpy(n_ui->name, vassal_name, strlen(vassal_name));
				n_ui->born = uwsgi_now();
				n_ui->uid = 0;
				n_ui->gid = 0;
				n_ui->last_mod = n_ui->born;
				// start non-ready
				n_ui->last_ready = 0;
				n_ui->ready = 0;
				// start without loyalty
				n_ui->last_loyal = 0;
				n_ui->loyal = 0;
				n_ui->suspended = 0;

				n_ui->attrs = attrs;

				n_ui->first_run = uwsgi_now();
				n_ui->last_run = n_ui->first_run;
				n_ui->on_demand_fd = -1;
				if (socket_name) {
					n_ui->socket_name =
						uwsgi_str(socket_name);
				}

				n_ui->pid = -1;
				n_ui->pipe[0] = -1;
				n_ui->pipe[1] = -1;

				n_ui->pipe_config[0] = -1;
				n_ui->pipe_config[1] = -1;

				// Check if the Emperor has to wait for a
				// command before spawning a vassal
				if (uwsgi.emperor_command_socket) {
					if (uwsgi.emperor_wait_for_command &&
					    !uwsgi_string_list_has_item(
						    uwsgi.emperor_wait_for_command_ignore,
						    vassal_name, strlen(vassal_name))) {
						n_ui->suspended = 1;
						uwsgi_log(
							"[uwsgi-emperor] %s -> "
							"\"wait-for-command\" "
							"instance detected, "
							"waiting for the spawn "
							"command ...\n",
							vassal_name);
						return;
					}
				}

				// ok here we check if we need to bind to the
				// specified socket or continue with the
				// activation
				n_ui->on_demand_fd = fd;

				event_queue_add_fd_read(uwsgi.emperor_queue,
							n_ui->on_demand_fd);
				uwsgi_log(
					"[uwsgi-emperor] %s -> \"on demand\" "
					"instance detected, waiting for "
					"connections on socket \"%s\" ...\n",
					vassal_name, socket_name);
				if (uwsgi_hooks_run_and_return(
					    uwsgi.hook_as_on_demand_vassal,
					    "as-on-demand-vassal", vassal_name, 0)) {
					emperor_del(n_ui);
				}
			}
			else {
				emperor_add_with_attrs(
					ues, vassal_name, uwsgi_now(),
					config, smc.config_len, 0, 0,
					socket, attrs);
			}

			if (write(client_fd, "+OK\n", 4) < 0) {
				uwsgi_error("uwsgi_imperial_monitor_socket_event()/write()");
			}
			close(client_fd);
			free(vassal_name);
			free(socket);
		}
		else {
			if (write(client_fd, "-IC\n", 4) != 4) {
				uwsgi_error(
					"uwsgi_imperial_monitor_socket_event()/"
					"write()");
			}
			close(client_fd);
		}
	OK:
		free(buf);
		client_fd = accept(ues->fd, NULL, NULL);
	}
}

void uwsgi_imperial_monitor_socket_init(struct uwsgi_emperor_scanner *ues)
{
	if (socket_name == NULL) {
		socket_name = uwsgi_str("@emperor");
	}
	uwsgi_log_verbose("[emperor_socket]: listen_queue %d",
			  uwsgi.listen_queue);
	char *addr = socket_name;
	if (strncmp("unix:", addr, 5) == 0) {
		uwsgi_log("unix socket\n");
		ues->fd =
			bind_to_unix(addr + 5, uwsgi.listen_queue,
				     uwsgi.chmod_socket, uwsgi.abstract_socket);
	}
	else {
		char *port = strchr(addr, ':');
		if (port) {
			port[0] = 0;
			ues->fd =
				bind_to_tcp(addr, uwsgi.listen_queue, port + 1);
			port[0] = ':';
		}
		else {
			uwsgi_error(
				"uwsgi_imperial_monitor_socket_init()/"
				"strchr()");
			return;
		}
	}
	if (listen(ues->fd, queue_length) == -1) {
		uwsgi_error("uwsgi_imperial_monitor_socket_init()/listen()");
	}
	if (fcntl(ues->fd, F_SETFL, O_NONBLOCK) == -1) {
		uwsgi_error("uwsgi_imperial_monitor_socket_init()/fcntl()");
	}
	uwsgi_log_verbose("[emepror_socket] ready at %s\n", addr);

	ues->event_func = uwsgi_imperial_monitor_socket_event;
	event_queue_add_fd_read(uwsgi.emperor_queue, ues->fd);
}

void uwsgi_imperial_monitor_socket(
	__attribute__((unused)) struct uwsgi_emperor_scanner *ues)
{
}
void emperor_socket_init(void)
{
	// Registering the a monitor that handels requests
	uwsgi_register_imperial_monitor("socket",
					uwsgi_imperial_monitor_socket_init,
					uwsgi_imperial_monitor_socket);
}

struct uwsgi_plugin emperor_socket_plugin = {
	.name = "emperor_socket",
	.on_load = emperor_socket_init,
	.options = uwsgi_emperor_socket_options,
};
