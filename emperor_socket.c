/*

Emperor unix socket monitor

syntax: unix://path/to/socket

TODO:
race condition ved flere kommandoer mot samme vassal


handling av restart ved fork-server (jeg viser deg fork-server oppsett)
exit -> curse -> die


*/

#include "../../uwsgi.h"
#include "pthread.h"

pthread_mutex_t emperor_mutex = PTHREAD_MUTEX_INITIALIZER;

extern struct uwsgi_server uwsgi;

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
	struct socket_monitor_command *smc = (struct socket_monitor_command *) data;

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
                                        uint16_t vallen, void *data)
{
	/*
		This is done a bit odd, since uwsgi_dyn_dict_free tries to free
		only value, hence it must be the start of the allocation.
		Also, even tho uwsgi_dyn_dict has includes len, fork server socket
		is used in emperor without len, so it needs terminating 0-byte
	*/

	char *kv = uwsgi_malloc(keylen + vallen + 2);
	memset(kv, 0, keylen + vallen + 2);
	memcpy(kv, val, vallen);
	memcpy(kv + vallen + 1, key, keylen);
	uwsgi_dyn_dict_new(data, kv + vallen + 1, keylen + 1, kv, vallen + 1);
}

void uwsgi_imperial_monitor_socket_event(struct uwsgi_emperor_scanner *ues)
{	
	struct uwsgi_dyn_dict *attrs = NULL;
	struct uwsgi_instance *ui_current;

	int client_fd = accept(ues->fd, NULL, NULL);
	if (client_fd < 0) {
		uwsgi_error("accept()");
		return;
	}

	size_t buf_len = uwsgi.page_size;
	char *buf = uwsgi_malloc(buf_len);

	if (uwsgi_read_with_realloc(client_fd, &buf, &buf_len, uwsgi.socket_timeout,
		                        NULL, NULL)) {
		uwsgi_log_verbose("[socket-monitor] unable to read socket message %d\n");
		goto OK;
	}

	struct socket_monitor_command smc;
	memset(&smc, 0, sizeof(struct socket_monitor_command));
	
	if(uwsgi_hooked_parse(buf, buf_len, socket_monitor_command_parser, &smc)) {
		uwsgi_log_verbose("[socket-monitor] uwsgi_hooked_parse\n");
	}

	if(!uwsgi_strncmp(smc.cmd, smc.cmd_len, "spawn", 5)) {
		if(!smc.vassal) {
			uwsgi_log_verbose("[socket-monitor] vassal name missing");
			write(client_fd, "-vassal missing\n", 16);
		}

		if(smc.attrs) {
			if (uwsgi_hooked_parse(smc.attrs, smc.attrs_len,
				                   socket_monitor_attrs_parser, &attrs)) {
				uwsgi_log_verbose("[socket-monitor] invalid attributes\n");
			}
		}
		char *config = NULL;
		if(smc.config) {
			config = uwsgi_strncopy(smc.config, smc.config_len);
		}

		char *socket = NULL;
		if(smc.socket) {
			socket = uwsgi_strncopy(smc.socket, smc.socket_len);
		}

		char *vassal_name = uwsgi_strncopy(smc.vassal, smc.vassal_len);
		
		//race
		pthread_mutex_lock(&emperor_mutex);
		ui_current = emperor_get(vassal_name);

		if (ui_current) {

			if (write(ui_current->pipe[0], "\1", 1) != 1) {
				uwsgi_string_list *dead_hook = uwsgi_malloc();
				dead_hook->
				// the vassal could be already dead, better to curse it
				uwsgi_error("emperor_respawn/write()");
				emperor_curse(ui_current);
				goto OK;
			}

			//TODO: Change attrs or  keep attrs
			struct uwsgi_dyn_dict *attrs_old = ui_current->attrs;
			while(attrs_old) {
				if(uwsgi_strncmp(attrs_old->key,attrs_old->keylen,
										"fork-server",11)){
				
					emperor_del(ui_current);
					emperor_add_with_attrs(ues,vassal_name,uwsgi_now(),config,
									smc.config_len,0,0,socket,attrs);
				}
				goto CK;
				attrs_old = attrs_old->next;
			}
				free(ui_current->config);
				ui_current->config = config;
				ui_current->config_len = smc.config_len;

				emperor_respawn(ui_current,uwsgi_now());
				//TODO ADD TIME
				//while(!(ui_current->ready)){}
		} else {
			emperor_add_with_attrs(ues, vassal_name, uwsgi_now(), config,
			                       smc.config_len, 0, 0, socket, attrs);
		}
CK:
		if(emperor_get(vassal_name) != NULL){

			write(client_fd, "+OK\n", 4);

		} else {				
			write(client_fd, "+Er\n", 4);

		}	
		pthread_mutex_unlock(&emperor_mutex);
		//vassal and socket is copied
		free(vassal_name);
		free(socket);
	}
OK:
	free(buf);
	close(client_fd);
}

void uwsgi_imperial_monitor_socket_init(struct uwsgi_emperor_scanner *ues)
{
	ues->fd = bind_to_unix("/tmp/emperor.sock", uwsgi.listen_queue,
	                       uwsgi.chmod_socket, uwsgi.abstract_socket);

	ues->event_func = uwsgi_imperial_monitor_socket_event;
	event_queue_add_fd_read(uwsgi.emperor_queue, ues->fd);
}

void uwsgi_imperial_monitor_socket(
	__attribute__((unused)) struct uwsgi_emperor_scanner *ues)
{
}

void emperor_socket_init(void)
{
	uwsgi_register_imperial_monitor("socket",
	                                uwsgi_imperial_monitor_socket_init,
	                                uwsgi_imperial_monitor_socket);
}

struct uwsgi_plugin emperor_socket_plugin = {
	.name = "emperor_socket",
	.on_load = emperor_socket_init,
};
