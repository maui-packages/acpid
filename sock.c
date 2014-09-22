/*
 *  sock.c - ACPI daemon socket interface
 *
 *  Portions Copyright (C) 2000 Andrew Henroid
 *  Portions Copyright (C) 2001 Sun Microsystems
 *  Portions Copyright (C) 2004 Tim Hockin (thockin@hockin.org)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <grp.h>

#include "acpid.h"
#include "log.h"
#include "event.h"
#include "ud_socket.h"
#include "connection_list.h"

const char *socketfile = ACPID_SOCKETFILE;
const char *socketgroup;
mode_t socketmode = ACPID_SOCKETMODE;
int clientmax = ACPID_CLIENTMAX;

/* the number of non-root clients that are connected */
int non_root_clients;

/* determine if a file descriptor is in fact a socket */
int
is_socket(int fd)
{
	int v;
	socklen_t l = sizeof(int);

	return (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&v, &l) == 0);
}

/* accept a new client connection */
static void
process_sock(int fd)
{
	int cli_fd;
	struct ucred creds;
	char buf[32];
	static int accept_errors;

	/* accept and add to our lists */
	cli_fd = ud_accept(fd, &creds);
	if (cli_fd < 0) {
		acpid_log(LOG_ERR, "can't accept client: %s",
			  strerror(errno));
		accept_errors++;
		if (accept_errors >= 5) {
			acpid_log(LOG_ERR, "giving up");
			clean_exit_with_status(EXIT_FAILURE);
		}
		return;
	}
	accept_errors = 0;

	/* don't allow too many non-root clients  */
	if (creds.uid != 0 && non_root_clients >= clientmax) {
		close(cli_fd);
		acpid_log(LOG_ERR, "too many non-root clients");
		return;
	}
	if (creds.uid != 0) {
		non_root_clients++;
	}

    /* don't leak fds when execing */
	if (fcntl(cli_fd, F_SETFD, FD_CLOEXEC) < 0) {
		close(cli_fd);
		acpid_log(LOG_ERR, "fcntl() on client for FD_CLOEXEC: %s", 
            strerror(errno));
		return;
    }

    /* don't allow clients to block this */
    if (fcntl(cli_fd, F_SETFL, O_NONBLOCK) < 0) {
		close(cli_fd);
		acpid_log(LOG_ERR, "fcntl() on client for O_NONBLOCK: %s", 
            strerror(errno));
		return;
    }

    snprintf(buf, sizeof(buf)-1, "%d[%d:%d]",
		 creds.pid, creds.uid, creds.gid);
	acpid_add_client(cli_fd, buf);
}

/* set up the socket for client connections */
void
open_sock()
{
	int fd;
	struct connection c;

	/* if this is a socket passed in via stdin by systemd */
	if (is_socket(STDIN_FILENO)) {
		fd = STDIN_FILENO;
	} else {
		/* create our own socket */
		fd = ud_create_socket(socketfile);
		if (fd < 0) {
			acpid_log(LOG_ERR, "can't open socket %s: %s",
				socketfile, strerror(errno));
			exit(EXIT_FAILURE);
		}

		if (chmod(socketfile, socketmode) < 0) {
			close(fd);
			acpid_log(LOG_ERR, "chmod() on socket %s: %s", 
		        socketfile, strerror(errno));
			return;
		}

		/* if we need to change the socket's group, do so */
		if (socketgroup) {
			struct group *gr;
			struct stat buf;

		    gr = getgrnam(socketgroup);
			if (!gr) {
				acpid_log(LOG_ERR, "group %s does not exist", socketgroup);
				exit(EXIT_FAILURE);
			}
			if (stat(socketfile, &buf) < 0) {
				acpid_log(LOG_ERR, "can't stat %s: %s", 
		            socketfile, strerror(errno));
				exit(EXIT_FAILURE);
			}
			if (chown(socketfile, buf.st_uid, gr->gr_gid) < 0) {
				acpid_log(LOG_ERR, "can't chown %s: %s", 
		            socketfile, strerror(errno));
				exit(EXIT_FAILURE);
			}
		}
	}

	/* don't leak fds when execing */
	if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
		close(fd);
		acpid_log(LOG_ERR, "fcntl() on socket %s for FD_CLOEXEC: %s", 
		          socketfile, strerror(errno));
		return;
	}

	/* avoid a potential hang */
	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
		close(fd);
		acpid_log(LOG_ERR, "fcntl() on socket %s for O_NONBLOCK: %s", 
		          socketfile, strerror(errno));
		return;
	}
	
	/* add a connection to the list */
	c.fd = fd;
	c.process = process_sock;
	c.pathname = NULL;
	c.kybd = 0;
	add_connection(&c);
}