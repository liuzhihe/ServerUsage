/*
//=============================================================================+
// File name   : serverusage_tcpreceiver.c
// Begin       : 2012-02-14
// Last Update : 2012-08-08
//
// Website     : https://github.com/fubralimited/ServerUsage
//
// Description : This program listen on specified IP:PORT for incoming
//               TCP connections from serverusage_tcpsender.bin, and store the
//               data on a SQLite database table.
//
// Author: Nicola Asuni
//
// (c) Copyright:
//               Fubra Limited
//               Manor Coach House
//               Church Hill
//               Aldershot
//               Hampshire
//               GU12 4RQ
//				 UK
//               http://www.fubra.com
//               support@fubra.com
//
// License:
//    Copyright (C) 2012-2012 Fubra Limited
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License as
//    published by the Free Software Foundation, either version 3 of the
//    License, or (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//    See LICENSE.TXT file for more information.
//=============================================================================+
*/

// TO COMPILE (requires sqlite-devel):
// gcc -O3 -g -pipe -Wp,-D_THREAD_SAFE -D_FORTIFY_SOURCE=2 -fexceptions -fstack-protector -fno-strict-aliasing -fwrapv -fPIC --param=ssp-buffer-size=4 -D_GNU_SOURCE -o serverusage_tcpreceiver.bin serverusage_tcpreceiver.c -lpthread -lsqlite3

// USAGE EXAMPLES:
// ./serverusage_tcpreceiver.bin PORT MAX_CONNECTIONS "database"
// ./serverusage_tcpreceiver.bin 9930 100 "/var/lib/serverusage/serverusage.db"

// NOTE: For the SQLite table used to to store data, please consult the SQL file on this project.

#include <pthread.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sqlite3.h>

// DEBUG OPTION TO PRINT EXECUTION TIME
//#define _DEBUG
#ifdef _DEBUG
	#include <time.h>
	time_t starttime;
	time_t endtime;
#endif

/**
 * Max size of the TCP buffer lenght.
 */
#define BUFLEN 512

/**
 * Read size for TCP buffer
 */
#define READBUFLEN 510

/**
 * Max size of the SQL query.
 */
#define QUERYLEN 1024

/**
 * SQLite database connection
 */
sqlite3 *db;

/**
 * SQLite statement
 */
sqlite3_stmt *stmt;

/**
 * Semaphore used to synchronise SQLite transactions.
 */
sem_t mutex;

/**
 * Global variable to count active threads.
 */
int threadcounter = 0;

/**
 * Struct to contain thread arguments.
 */
typedef struct _targs {
	int socket_conn;
	char clientip[INET6_ADDRSTRLEN];
	unsigned int clientport;
} targs;

/**
 * Report error message and close the program.
 */
void diep(const char *s) {
	perror(s);
	exit(1);
}

/**
 * Insert row on database.
 * @param row Raw row to insert.
 * @param clientip Client IP address.
 * @param clientport Client TCP Port.
 */
void insert_row(char *row, const char *clientip, unsigned int clientport) {

	// query parameters
	char param[11][64];

	// iterator for parameters
	int i;

	// pointer for strtok_r
	char *endstr = NULL;

	// check if the line is correctly formatted
	if ((row[0] != '@') || (row[1] != '@') || (row[2] != '\t')) {
		perror("ServerUsage-Server (insert_row : invalid line)");
		return;
	}

	// remove first 3 characters "@@\t"
	memmove(row, (row + 3), strlen(row));

	// parse parameters
	strcpy(param[0], strtok_r(row, "\t", &endstr));
	for (i = 1; i < 11; i++) {
		strcpy(param[i], strtok_r(NULL, "\t", &endstr));
	}

	// bind parameters ('%s',%d,%d,%d,'%s','%s',%d,'%s',%d,%d,%d,%d,%d)
	sqlite3_bind_text(stmt, 1, clientip, -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 2, clientport);
	sqlite3_bind_int(stmt, 3, strtoul(param[0], NULL, 10));
	sqlite3_bind_int(stmt, 4, strtoul(param[1], NULL, 10));
	sqlite3_bind_text(stmt, 5, param[2], -1, SQLITE_TRANSIENT);
	sqlite3_bind_text(stmt, 6, param[3], -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 7, strtoul(param[4], NULL, 10));
	sqlite3_bind_text(stmt, 8, param[5], -1, SQLITE_TRANSIENT);
	sqlite3_bind_int(stmt, 9, strtoul(param[6], NULL, 10));
	sqlite3_bind_int(stmt, 10, strtoul(param[7], NULL, 10));
	sqlite3_bind_int(stmt, 11, strtoul(param[8], NULL, 10));
	sqlite3_bind_int(stmt, 12, strtoul(param[9], NULL, 10));
	sqlite3_bind_int(stmt, 13, strtoul(param[10], NULL, 10));

	//execute statement
	sqlite3_step(stmt);

	// reset statement
	sqlite3_clear_bindings(stmt);
	sqlite3_reset(stmt);
}

/**
 * Thread to handle one connection.
 * @param cargs Arguments.
 */
void *connection_thread(void *cargs) {

	// decode parameters
	targs arg = *((targs *) cargs);

	// buffer used to receive TCP messages
	char buf[BUFLEN];

	// store last row portion
	char lastrow[BUFLEN];

	// line delimiter
	char delims[] = "\n";

	// single log line
	char *row = NULL;

	// string pointer for strtok_r
	char *endstr = NULL;

	// position inside the row buffer
	int pos = 0;

	// check if a row is splitted across two reads
	int splitline = 0;

	// lenght of buffer
	int buflen = 0;

	// sqlite error messages
	char *ErrMsg = NULL;

	// clear (initialize) the strings
	memset(buf, 0, BUFLEN);
	memset(lastrow, 0, BUFLEN);

	sem_wait(&mutex);
	++threadcounter;
	if (threadcounter == 1) {
		#ifdef _DEBUG
			starttime = time(NULL);
			printf("  START TIME [sec]: %d\n", starttime);
		#endif
		// begin the transaction when the first thread is created (we use transactions to improve performances)
		if (sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &ErrMsg) != SQLITE_OK) {
			perror(ErrMsg);
			sqlite3_free(ErrMsg);
		}
	}
	sem_post(&mutex);

	// receive a message from ns and put data int buf (limited to BUFLEN characters)
	while ((buflen = read(arg.socket_conn, buf, READBUFLEN)) > 0) {

		// mark the end of buffer (used to recompose splitted lines)
		buf[buflen] = 2;

		// split stream into lines
		row = strtok_r(buf, delims, &endstr);

		// for each line on buf
		while (row != NULL) {

			pos = (strlen(row) - 1);
			if (row[pos] == 2) { // the line is incomplete
				if (pos > 0) { // avoid lines that contains only the terminator character
					// store uncompleted row part
					strcpy(lastrow, row);
					lastrow[pos] = '\0';
					splitline = 1;
				}
			} else { // we reached the end of a line
				if (splitline == 1) { // reconstruct splitted line
					if ((row[0] != '@') || (row[1] != '@') || (row[2] != '\t')) {
						// recompose the line
						strcat(lastrow, row);
						// insert row on database
						insert_row(lastrow, arg.clientip, arg.clientport);
					} else { // the lastrow contains a full line (only the newline character was missing)
						// insert row on database
						insert_row(lastrow, arg.clientip, arg.clientport);
						// insert row on database
						insert_row(row, arg.clientip, arg.clientport);
					}
				} else { // we have an entire line
					// insert row on database
					insert_row(row, arg.clientip, arg.clientport);
				}
				memset(lastrow, 0, BUFLEN);
				splitline = 0;
			}

			// read next line
			row = strtok_r(NULL, delims, &endstr);

		} // end for each row

		// clear the buffer
		memset(buf, 0, BUFLEN);

	} // end read TCP

	sem_wait(&mutex);
	--threadcounter;
	if (threadcounter == 0) {
		// end the transaction when the last thread is closed
		if (sqlite3_exec(db, "END TRANSACTION", NULL, NULL, &ErrMsg) != SQLITE_OK) {
			perror(ErrMsg);
			sqlite3_free(ErrMsg);
		}
		#ifdef _DEBUG
			endtime = time(NULL);
			printf("    END TIME [sec]: %d\n", endtime);
			printf("ELAPSED TIME [sec]: %d\n\n", (endtime - starttime));
		#endif
	}
	sem_post(&mutex);

	// close connection
	close(arg.socket_conn);

	// free memory
	free(row);
	free(ErrMsg);

} // end connection_thread

/**
 * Main function.
 * @param argc Argument counter.
 * @param argv[] Array of arguments: port, max_connections, sqlite_database.
 */
int main(int argc, char *argv[]) {

	// decode arguments
	if (argc != 4) {
		diep("This program listen on specified IP:PORT for incoming TCP messages from serverusage_tcpsender.bin, and store the data on a SQLite database memory table.\nYou must provide 3 arguments: port, max_conenctions, sqlite_database\nFOR EXAMPLE:\n./serverusage_tcpreceiver.bin 9930 100 \"/var/lib/serverusage/serverusage.db\"");
	}

	// set input values
	int port = atoi(argv[1]);
	int maxconn = atoi(argv[2]);
	char *database = (char *)argv[3];

	// thread identifier
	pthread_t tid;

	// thread attributes
	pthread_attr_t tattr;

	// true option for setsockopt
	int opttrue = 1;
	
	// false option for setsockopt
	//int optfalse = 0;

	// thread number
	int tn = 0;

	// arguments be passes on thread
	targs cargs[maxconn];

	char *ErrMsg;

	// connect to database
	if (sqlite3_open(database, &db) != SQLITE_OK) {
		diep(sqlite3_errmsg(db));
	}

	// performance: instruct SQLite to simply hand-off the data to the OS for writing and then continue
	if (sqlite3_exec(db, "PRAGMA synchronous = OFF", NULL, NULL, &ErrMsg) != SQLITE_OK) {
		diep(ErrMsg);
	}

	// performance: store the rollback journal in memory
	if (sqlite3_exec(db, "PRAGMA journal_mode = MEMORY", NULL, NULL, &ErrMsg) != SQLITE_OK) {
		diep(ErrMsg);
	}

	// compile the sql statement
	if (sqlite3_prepare_v2(db, "INSERT INTO log_raw VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)", BUFLEN, &stmt, NULL) != SQLITE_OK) {
		diep(sqlite3_errmsg(db));
	}

	// structure containing an Internet socket address: an address family (always AF_INET for our purposes), a port number, an IP address
	// si_server defines the socket where the server will listen.
	struct sockaddr_in6 si_server;

	// defines the socket at the other end of the link (that is, the client)
	struct sockaddr_in6 si_client;

	// size of si_client
	int slen = sizeof(si_client);

	// socket
	int s = -1;

	// new socket
	int ns = -1;

	// initialize the si_server structure filling it with binary zeros
	memset((char *) &si_server, 0, slen);

	// use internet address
	si_server.sin6_family = AF_INET6;

	// listen to any IP address
	si_server.sin6_addr = in6addr_any;

	// set the port to listen to, and ensure the correct byte order
	si_server.sin6_port = htons(port);

	// Create a network socket.
	// AF_INET says that it will be an Internet socket.
	// SOCK_STREAM Provides sequenced, reliable, two-way, connection-based byte streams.
	if ((s = socket(si_server.sin6_family, SOCK_STREAM, 0)) == -1) {
		diep("ServerUsage-Server (socket)");
	}

	// set socket to listen on IPv6 and IPv4
	if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &opttrue, sizeof(opttrue)) == -1) {
		diep("ServerUsage-Server (setsockopt : IPPROTO_IPV6 - IPV6_V6ONLY)");
	}

	// set SO_REUSEADDR on socket to true (1):
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opttrue, sizeof(opttrue)) == -1) {
		diep("ServerUsage-Server (setsockopt : SOL_SOCKET - SO_REUSEADDR)");
	}

	// bind the socket s to the address in si_server.
	if (bind(s, (struct sockaddr *) &si_server, slen) == -1) {
		diep("ServerUsage-Server (bind)");
	}

	// listen for connections
	listen(s, maxconn);

	// initialize mutex semaphore
	sem_init(&mutex, 0, 1);

	// forever
	while (1)  {

		// accept a connection on a socket
		if ((ns = accept(s, (struct sockaddr *) &si_client, &slen)) == -1) {
			// print an error message
			perror("ServerUsage-Server (accept)");
			// retry after 1 second
			sleep(1);
		} else {

			// prepare data for the thread
			cargs[tn].socket_conn = ns;
			memset(cargs[tn].clientip, 0, INET6_ADDRSTRLEN);
			inet_ntop(si_server.sin6_family, &si_client.sin6_addr, cargs[tn].clientip, INET6_ADDRSTRLEN);
			cargs[tn].clientport = ntohs(si_client.sin6_port);

			// handle each connection on a separate thread
			pthread_attr_init(&tattr);
			pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_DETACHED);
			pthread_create(&tid, &tattr, connection_thread, (void *)&cargs[tn]);

			tn++;

			if (tn >= maxconn) {
				// reset connection number
				tn = 0;
			}
		}

	} // end of for loop

	// close socket
	close(s);

	// terminate statement
	sqlite3_finalize(stmt);

	// close SQLite connection
	sqlite3_close(db);

	// free resources
	free(db);
	free(stmt);
	free(ErrMsg);
	free(database);

	// close program and return 0
	return 0;
}

//=============================================================================+
// END OF FILE
//=============================================================================+
