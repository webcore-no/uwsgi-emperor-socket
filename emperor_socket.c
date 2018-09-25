/*

Emperor unix socket monitor

syntax: unix://path/to/socket

TODO:
race condition ved flere kommandoer mot samme vassal


handling av restart ved fork-server (jeg viser deg fork-server oppsett)
exit -> curse -> die


*/

#include "../../uwsgi.h"
#include "poll.h"
#include "pthread.h"
#include <sys/socket.h>
#include <fcntl.h>


#define TIMEOUT 30  // timeout 30 sec
extern struct uwsgi_server uwsgi;

struct spawn {
	int fd;
	char *vassal_name;
	time_t last_spawn;
	int queue_fd;
	char *queue_config;
	uint16_t queue_config_len;
	struct spawn *next;
	pthread_mutex_t spawn_lock;
};

struct spawn *spawn_list;
int add_spawn(struct spawn **spp, int fd, char *vassal_name, char *config,
	      uint16_t config_len) {
	struct spawn *sp = *spp;
	if (!sp) {
		*spp = uwsgi_calloc(sizeof(struct spawn));
		sp = *spp;
	} else {
		while (sp) {
			if (strcmp(sp->vassal_name, vassal_name) == 0) {
				if(sp->fd < 0){
						sp->fd = fd;
						sp->last_spawn = uwsgi_now();
						return 0;	
				}
				if (sp->queue_fd > 0) {
					free(sp->queue_config);
					if (write(sp->queue_fd, "+OV\n", 4)) {
					}
					close(sp->queue_fd);
				}
				sp->queue_fd = fd;
				sp->queue_config = uwsgi_str(config);
				sp->queue_config_len = config_len;
				return -1;
			}
			if (!sp->next) {
				sp->next = uwsgi_calloc(sizeof(struct spawn));
				sp = sp->next;
				break;
			}
			sp = sp->next;
		}
	}
	sp->fd = fd;
	sp->vassal_name = uwsgi_str(vassal_name);
	sp->last_spawn = uwsgi_now();
	sp->next = NULL;
	return 0;
}

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
					  uint16_t vallen, void *data) {
	struct socket_monitor_command *smc =
	    (struct socket_monitor_command *)data;

	if (!uwsgi_strncmp("cmd", 3, key, keylen)) {
		smc->cmd = val;
		smc->cmd_len = vallen;
	} else if (!uwsgi_strncmp("vassal", 6, key, keylen)) {
		smc->vassal = val;
		smc->vassal_len = vallen;
	} else if (!uwsgi_strncmp("attrs", 5, key, keylen)) {
		smc->attrs = val;
		smc->attrs_len = vallen;
	} else if (!uwsgi_strncmp("socket", 6, key, keylen)) {
		smc->socket = val;
		smc->socket_len = vallen;
	} else if (!uwsgi_strncmp("config", 6, key, keylen)) {
		smc->config = val;
		smc->config_len = vallen;
	}
}

static void socket_monitor_attrs_parser(char *key, uint16_t keylen, char *val,
					uint16_t vallen, void *data) {
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


int i;
void uwsgi_imperial_monitor_socket_event(struct uwsgi_emperor_scanner *ues) {
	int client_fd;
	client_fd = accept(ues->fd, NULL, NULL);
	if (client_fd < 0) {
		uwsgi_error("ERR");
		return;
	}
	while (client_fd > 0) {


		int now = uwsgi_micros();
		uwsgi_log("ACCEPTED:%d\n",now);
		i = i + 1;
		struct uwsgi_dyn_dict *attrs = NULL;
		struct uwsgi_instance *ui_current;

		size_t buf_len = uwsgi.page_size;
		char *buf = uwsgi_malloc(buf_len);

		if (uwsgi_read_with_realloc(client_fd, &buf, &buf_len,
					    uwsgi.socket_timeout, NULL, NULL)) {
			uwsgi_log_verbose(
			    "[socket-monitor] unable to read socket message "
			    "%d\n");
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
					// TODO:Handle write error
				}
			}

			if (smc.attrs) {
				if (uwsgi_hooked_parse(
					smc.attrs, smc.attrs_len,
					socket_monitor_attrs_parser, &attrs)) {
					uwsgi_log_verbose(
					    "[socket-monitor] invalid "
					    "attributes\n");
				}
			}
			char *config = NULL;
			if (smc.config) {
				config =
				    uwsgi_strncopy(smc.config, smc.config_len);
			}

			char *socket = NULL;
			if (smc.socket) {
				socket =
				    uwsgi_strncopy(smc.socket, smc.socket_len);
			}

			char *vassal_name =
			    uwsgi_strncopy(smc.vassal, smc.vassal_len);

			// race
			ui_current = emperor_get(vassal_name);

			// vassal and socket is copied
			if (add_spawn(&spawn_list, client_fd, vassal_name,
				      config, smc.config_len) == 0) {
				if (ui_current) {
					free(ui_current->config);
					ui_current->config = config;
					ui_current->config_len = smc.config_len;
					emperor_respawn(ui_current,
							uwsgi_now());
				} else {
					int now = uwsgi_micros();
					uwsgi_log("LEAVING PLUGIN:%d\n",now);
					emperor_add_with_attrs(
					    ues, vassal_name, uwsgi_now(),
					    config, smc.config_len, 0, 0,
					    socket, attrs);
				}
			}
			free(vassal_name);
			free(socket);
		}
	OK:
		free(buf);
		client_fd = accept(ues->fd, NULL,NULL);
	}
}

void uwsgi_imperial_monitor_socket_init(struct uwsgi_emperor_scanner *ues) {
	/*ues->fd = bind_to_unix("/tmp/emperor.sock", uwsgi.listen_queue,
			       uwsgi.chmod_socket, uwsgi.abstract_socket);
*/
	char *addr = uwsgi_str("127.0.0.1");
	char *port = uwsgi_str(":7769");

	ues->fd = bind_to_tcp(addr, uwsgi.listen_queue, port);
	if (listen(ues->fd, 32) == -1) {
		uwsgi_error("Failed to listen to fd.");
	}
	if(fcntl(ues->fd,F_SETFL,O_NONBLOCK)  == -1) {
			uwsgi_error("[EMPEROR SOCKET]:Failed to set fd to O_NONBLOCK");
	}
	uwsgi_log("\e[7mTCP SOCKET AT:%d\e[27m\n", ues->fd);
	free(addr);
	free(port);
	ues->event_func = uwsgi_imperial_monitor_socket_event;
	event_queue_add_fd_read(uwsgi.emperor_queue, ues->fd);
}

void uwsgi_imperial_monitor_socket(
    __attribute__((unused)) struct uwsgi_emperor_scanner *ues) {
	struct spawn *sp;
	struct uwsgi_instance *ui_current;
	uwsgi_foreach(sp, spawn_list) {
		ui_current = emperor_get(sp->vassal_name);
		if (ui_current && ui_current->accepting == 1) {
			if (write(sp->fd, "+OK\n", 4) < 0) {
				// failed to write OK back
			}
			close(sp->fd);
			sp->fd = -1;
			if (sp->queue_fd > 0) {
				free(ui_current->config);
				ui_current->config = sp->queue_config;
				ui_current->config_len = sp->queue_config_len;
				emperor_respawn(ui_current, uwsgi_now());
				sp->fd = sp->queue_fd;
				sp->queue_fd = -1;
				sp->queue_config_len = 0;
				sp->last_spawn = uwsgi_now();
			} 
		} /*else if (uwsgi_now() - sp->last_spawn > TIMEOUT) {
			if (write(sp->fd, "+TO\n", 4) < 0) {
				// failed to write OK back
			}
			close(sp->fd);
			sp->fd = -1;
		}*/
		ui_current = NULL;
	}
}
void emperor_socket_init(void) {
	// Registering the a monitor that handels requests
	uwsgi_register_imperial_monitor("socket",
					uwsgi_imperial_monitor_socket_init,
					uwsgi_imperial_monitor_socket);
}

struct uwsgi_plugin emperor_socket_plugin = {
    .name = "emperor_socket",
    .on_load = emperor_socket_init,
};
