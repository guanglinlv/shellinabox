// server.c -- Generic server that can deal with HTTP connections
// Copyright (C) 2008-2009 Markus Gutschke <markus@shellinabox.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
// In addition to these license terms, the author grants the following
// additional rights:
//
// If you modify this program, or any covered work, by linking or
// combining it with the OpenSSL project's OpenSSL library (or a
// modified version of that library), containing parts covered by the
// terms of the OpenSSL or SSLeay licenses, the author
// grants you additional permission to convey the resulting work.
// Corresponding Source for a non-source form of such a combination
// shall include the source code for the parts of OpenSSL used as well
// as that of the covered work.
//
// You may at your option choose to remove this additional permission from
// the work, or from any part of it.
//
// It is possible to build this program in a way that it loads OpenSSL
// libraries at run-time. If doing so, the following notices are required
// by the OpenSSL and SSLeay licenses:
//
// This product includes software developed by the OpenSSL Project
// for use in the OpenSSL Toolkit. (http://www.openssl.org/)
//
// This product includes cryptographic software written by Eric Young
// (eay@cryptsoft.com)
//
//
// The most up-to-date version of this program is always available from
// http://shellinabox.com

#include "config.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "libhttp/server.h"
#include "libhttp/httpconnection.h"
#include "libhttp/ssl.h"
#include "logging/logging.h"

#define INITIAL_TIMEOUT    (10*60)

// Maximum amount of payload (e.g. form values that have been POST'd) that we
// read into memory. If the application needs any more than this, the streaming
// API should be used, instead.
#define MAX_PAYLOAD_LENGTH (64<<10)

time_t currentTime;

struct PayLoad {
  int (*handler)(struct HttpConnection *, void *, const char *, int);
  void *arg;
  int  len;
  char *bytes;
};

static int serverCollectFullPayload(struct HttpConnection *http,
                                    void *payload_, const char *buf, int len) {
  int rc                        = HTTP_READ_MORE;
  struct PayLoad *payload       = (struct PayLoad *)payload_;
  if (buf && len) {
    if (payload->len + len > MAX_PAYLOAD_LENGTH) {
      httpSendReply(http, 400, "Bad Request", NO_MSG);
      return HTTP_DONE;
    }
    check(len > 0);
    check(payload->bytes        = realloc(payload->bytes, payload->len + len));
    memcpy(payload->bytes + payload->len, buf, len);
    payload->len               += len;
  }
  const char *contentLength     = getFromHashMap(httpGetHeaders(http),
                                                 "content-length");
  if (!contentLength ||
      (payload->bytes &&
       ((contentLength && atoi(contentLength) <= payload->len) || !buf))) {
    rc = payload->handler(http, payload->arg,
                          payload->bytes ? payload->bytes : "", payload->len);
    free(payload->bytes);
    payload->bytes              = NULL;
    payload->len                = 0;
  }
  if (!buf) {
    if (rc == HTTP_SUSPEND || rc == HTTP_PARTIAL_REPLY) {
      // Tell the other party that the connection is getting torn down, even
      // though it requested it to be suspended.
      payload->handler(http, payload->arg, NULL, 0);
      rc                        = HTTP_DONE;
    }
    free(payload);
  }
  return rc;
  
}

static int serverCollectHandler(struct HttpConnection *http, void *handler_) {
  struct HttpHandler *handler = handler_;
  struct PayLoad *payload;
  check(payload               = malloc(sizeof(struct PayLoad)));
  payload->handler            = handler->streamingHandler;
  payload->arg                = handler->streamingArg;
  payload->len                = 0;
  payload->bytes              = malloc(0);
  httpSetCallback(http, serverCollectFullPayload, payload);
  return HTTP_READ_MORE;

}

static void serverDestroyHandlers(void *arg, char *value) {
  free(value);
}

void serverRegisterHttpHandler(struct Server *server, const char *url,
                               int (*handler)(struct HttpConnection *, void *,
                                              const char *, int), void *arg) {
  if (!handler) {
    addToTrie(&server->handlers, url, NULL);
  } else {
    struct HttpHandler *h;
    check(h             = malloc(sizeof(struct HttpHandler)));
    h->handler          = serverCollectHandler;
    h->arg              = h;
    h->streamingHandler = handler;
    h->streamingArg     = arg;
    addToTrie(&server->handlers, url, (char *)h);
  }
}

void serverRegisterStreamingHttpHandler(struct Server *server, const char *url,
                               int (*handler)(struct HttpConnection *, void *),
                               void *arg) {
  if (!handler) {
    addToTrie(&server->handlers, url, NULL);
  } else {
    struct HttpHandler *h;
    check(h             = malloc(sizeof(struct HttpHandler)));
    h->handler          = handler;
    h->streamingHandler = NULL;
    h->streamingArg     = NULL;
    h->arg              = arg;
    addToTrie(&server->handlers, url, (char *)h);
  }
}

static int serverQuitHandler(struct HttpConnection *http, void *arg) {
  httpSendReply(http, 200, "Good Bye", NO_MSG);
  httpExitLoop(http, 1);
  return HTTP_DONE;
}

struct Server *newCGIServer(int portMin, int portMax, int timeout) {
  struct Server *server;
  check(server = malloc(sizeof(struct Server)));
  initServer(server, portMin, portMax, timeout);
  return server;
}

struct Server *newServer(int port) {
  return newCGIServer(port, port, -1);
}

void initServer(struct Server *server, int portMin, int portMax, int timeout) {
  server->looping               = 0;
  server->exitAll               = 0;
  server->serverTimeout         = timeout;
  server->numericHosts          = 0;
  server->connections           = NULL;
  server->numConnections        = 0;

  int true                      = 1;
  server->serverFd              = socket(PF_INET, SOCK_STREAM, 0);
  check(server->serverFd >= 0);
  check(!setsockopt(server->serverFd, SOL_SOCKET, SO_REUSEADDR,
                    &true, sizeof(true)));
  struct sockaddr_in serverAddr = { 0 };
  serverAddr.sin_family         = AF_INET;
  serverAddr.sin_addr.s_addr    = INADDR_ANY;

  // Linux unlike BSD does not have support for picking a local port range.
  // So, we have to randomly pick a port from our allowed port range, and then
  // keep iterating until we find an unused port.
  if (portMin || portMax) {
    struct timeval tv;
    check(!gettimeofday(&tv, NULL));
    srand((int)(tv.tv_usec ^ tv.tv_sec));
    check(portMin > 0);
    check(portMax < 65536);
    check(portMax >= portMin);
    int portStart               = rand() % (portMax - portMin + 1) + portMin;
    for (int p = 0; p <= portMax-portMin; p++) {
      int port                  = (p+portStart)%(portMax-portMin+1)+ portMin;
      serverAddr.sin_port       = htons(port);
      if (!bind(server->serverFd, (struct sockaddr *)&serverAddr,
                sizeof(serverAddr))) {
        break;
      }
      serverAddr.sin_port       = 0;
    }
    if (!serverAddr.sin_port) {
      fatal("Failed to find any available port");
    }
  }

  check(!listen(server->serverFd, SOMAXCONN));
  socklen_t socklen             = (socklen_t)sizeof(serverAddr);
  check(!getsockname(server->serverFd, (struct sockaddr *)&serverAddr,
                     &socklen));
  check(socklen == sizeof(serverAddr));
  server->port                  = ntohs(serverAddr.sin_port);
  info("Listening on port %d", server->port);

  check(server->pollFds         = malloc(sizeof(struct pollfd)));
  server->pollFds->fd           = server->serverFd;
  server->pollFds->events       = POLLIN;

  initTrie(&server->handlers, serverDestroyHandlers, NULL);
  serverRegisterStreamingHttpHandler(server, "/quit", serverQuitHandler, NULL);
  initSSL(&server->ssl);
}

void destroyServer(struct Server *server) {
  if (server) {
    if (server->serverFd >= 0) {
      info("Shutting down server");
      close(server->serverFd);
    }
    for (int i = 0; i < server->numConnections; i++) {
      server->connections[i].destroyConnection(server->connections[i].arg);
    }
    free(server->connections);
    free(server->pollFds);
    destroyTrie(&server->handlers);
    destroySSL(&server->ssl);
  }
}

void deleteServer(struct Server *server) {
  destroyServer(server);
  free(server);
}

int serverGetListeningPort(struct Server *server) {
  return server->port;
}

int serverGetFd(struct Server *server) {
  return server->serverFd;
}

struct ServerConnection *serverAddConnection(struct Server *server, int fd,
                         int (*handleConnection)(struct ServerConnection *c,
                                                 void *arg, short *events,
                                                 short revents),
                         void (*destroyConnection)(void *arg),
                         void *arg) {
  check(server->connections     = realloc(server->connections,
                                          ++server->numConnections*
                                          sizeof(struct ServerConnection)));
  check(server->pollFds         = realloc(server->pollFds,
                                          (server->numConnections + 1) *
                                          sizeof(struct pollfd)));
  server->pollFds[server->numConnections].fd     = fd;
  server->pollFds[server->numConnections].events = POLLIN;
  struct ServerConnection *connection            =
                              server->connections + server->numConnections - 1;
  connection->deleted           = 0;
  connection->timeout           = 0;
  connection->handleConnection  = handleConnection;
  connection->destroyConnection = destroyConnection;
  connection->arg               = arg;
  return connection;
}

void serverDeleteConnection(struct Server *server, int fd) {
  for (int i = 0; i < server->numConnections; i++) {
    if (fd == server->pollFds[i + 1].fd && !server->connections[i].deleted) {
      server->connections[i].deleted = 1;
      server->connections[i].destroyConnection(server->connections[i].arg);
      return;
    }
  }
}

void serverSetTimeout(struct ServerConnection *connection, time_t timeout) {
  if (!currentTime) {
    currentTime       = time(NULL);
  }
  connection->timeout = timeout > 0 ? timeout + currentTime : 0;
}

time_t serverGetTimeout(struct ServerConnection *connection) {
  if (connection->timeout) {
    // Returns <0 if expired, 0 if not set, and >0 if still pending.
    if (!currentTime) {
      currentTime = time(NULL);
    }
    int remaining = connection->timeout - currentTime;
    if (!remaining) {
      remaining--;
    }
    return remaining;
  } else {
    return 0;
  }
}

struct ServerConnection *serverGetConnection(struct Server *server,
                                             struct ServerConnection *hint,
                                             int fd) {
  if (hint &&
      server->connections <= hint &&
      server->connections + server->numConnections > hint &&
      &server->connections[hint - server->connections] == hint &&
      !hint->deleted &&
      server->pollFds[hint - server->connections + 1].fd == fd) {
    return hint;
  }
  for (int i = 0; i < server->numConnections; i++) {
    if (server->pollFds[i + 1].fd == fd && !server->connections[i].deleted) {
      return server->connections + i;
    }
  }
  return NULL;
}

short serverConnectionSetEvents(struct Server *server,
                                struct ServerConnection *connection,
                                short events) {
  dcheck(server);
  dcheck(connection);
  dcheck(connection >= server->connections);
  dcheck(connection < server->connections + server->numConnections);
  dcheck(connection == &server->connections[connection - server->connections]);
  dcheck(!connection->deleted);
  int   idx                       = connection - server->connections;
  short oldEvents                 = server->pollFds[idx + 1].events;
  server->pollFds[idx + 1].events = events;
  return oldEvents;
}

void serverExitLoop(struct Server *server, int exitAll) {
  server->looping--;
  server->exitAll |= exitAll;
}

void serverLoop(struct Server *server) {
  check(server->serverFd >= 0);
  time_t lastTime;
  currentTime                             = time(&lastTime);
  int loopDepth                           = ++server->looping;
  while (server->looping >= loopDepth && !server->exitAll) {
    // TODO: There probably should be some limit on the maximum number
    // of concurrently opened HTTP connections, as this could lead to
    // memory exhaustion and a DoS attack.
    time_t timeout                        = -1;
    int numFds                            = server->numConnections + 1;

    for (int i = 0; i < server->numConnections; i++) {
      while (i < numFds - 1 && !server->pollFds[i + 1].events) {
        // Sort filedescriptors that currently do not expect any events to
        // the end of the list.
        check(--numFds > 0);
        struct pollfd tmpPollFd;
        memcpy(&tmpPollFd, server->pollFds + numFds, sizeof(struct pollfd));
        memcpy(server->pollFds + numFds, server->pollFds + i + 1,
               sizeof(struct pollfd));
        memcpy(server->pollFds + i + 1, &tmpPollFd, sizeof(struct pollfd));
        struct ServerConnection tmpConnection;
        memcpy(&tmpConnection, server->connections + numFds - 1,
               sizeof(struct ServerConnection));
        memcpy(server->connections + numFds - 1, server->connections + i,
               sizeof(struct ServerConnection));
        memcpy(server->connections + i, &tmpConnection,
               sizeof(struct ServerConnection));
      }

      if (server->connections[i].timeout &&
          (timeout < 0 || timeout > server->connections[i].timeout)) {
        timeout                           = server->connections[i].timeout;
      }
    }

    // serverTimeout is always a delta value, unlike connection timeouts
    // which are absolute times.
    if (server->serverTimeout >= 0) {
      if (timeout < 0 || timeout > server->serverTimeout + currentTime) {
        timeout                           = server->serverTimeout+currentTime;
      }
    }

    if (timeout >= 0) {
      // Wait at least one second longer than needed, so that even if
      // poll() decides to return a second early (due to possible rounding
      // errors), we still correctly detect a timeout condition.
      if (timeout >= lastTime) {
        timeout                           = (timeout - lastTime + 1) * 1000;
      } else {
        timeout                           = 1000;
      }
    }

    int eventCount                        = NOINTR(poll(server->pollFds,
                                                        numFds,
                                                        timeout));
    check(eventCount >= 0);
    if (timeout >= 0) {
      timeout                            += lastTime;
    }
    currentTime                           = time(&lastTime);
    int isTimeout                         = timeout >= 0 &&
                                            timeout/1000 <= lastTime;
    if (server->pollFds[0].revents) {
      eventCount--;
      if (server->pollFds[0].revents && POLLIN) {
        struct sockaddr_in clientAddr;
        socklen_t sockLen                 = sizeof(clientAddr);
        int clientFd                      = accept(
                   server->serverFd, (struct sockaddr *)&clientAddr, &sockLen);
        dcheck(clientFd >= 0);
        if (clientFd >= 0) {
          check(!fcntl(clientFd, F_SETFL, O_RDWR | O_NONBLOCK));
          struct HttpConnection *http;
          http                            = newHttpConnection(
                                     server, clientFd, server->port,
                                     server->ssl.enabled ? &server->ssl : NULL,
                                     server->numericHosts);
          serverSetTimeout(
            serverAddConnection(server, clientFd, httpHandleConnection,
                                (void (*)(void *))deleteHttpConnection,
                                http),
            INITIAL_TIMEOUT);
        }
      }
    } else {
      if (server->serverTimeout > 0 && !server->numConnections) {
        // In CGI mode, exit the server, if we haven't had any active
        // connections in a while.
        break;
      }
    }
    for (int i = 1;
         (isTimeout || eventCount > 0) && i <= server->numConnections;
         i++) {
      struct ServerConnection *connection = server->connections + i - 1;
      if (connection->deleted) {
        continue;
      }
      if (!eventCount) {
        server->pollFds[i].revents        = 0;
      }
      if (server->pollFds[i].revents ||
          (connection->timeout && lastTime >= connection->timeout)) {
        if (server->pollFds[i].revents) {
          eventCount--;
        }
        short events                      = server->pollFds[i].events;
        if (!connection->handleConnection(connection, connection->arg,
                                         &events, server->pollFds[i].revents)){
          connection                      = server->connections + i - 1;
          connection->destroyConnection(connection->arg);
          connection->deleted             = 1;
        } else {
          server->pollFds[i].events       = events;
        }
      }
    }
    for (int i = 1; i <= server->numConnections; i++) {
      if (server->connections[i-1].deleted) {
        memmove(server->pollFds + i, server->pollFds + i + 1,
                (server->numConnections - i) * sizeof(struct pollfd));
        memmove(server->connections + i - 1, server->connections + i,
                (server->numConnections - i)*sizeof(struct ServerConnection));
        check(--i >= 0);
        check(--server->numConnections >= 0);
      }
    }
  }
  // Even if multiple clients requested for us to exit the loop, we only
  // ever exit the outer most loop.
  server->looping                         = loopDepth - 1;
}

void serverEnableSSL(struct Server *server, int flag) {
  if (flag) {
    check(serverSupportsSSL());
  }
  sslEnable(&server->ssl, flag);
}

void serverSetCertificate(struct Server *server, const char *filename,
                          int autoGenerateMissing) {
  sslSetCertificate(&server->ssl, filename, autoGenerateMissing);
}

void serverSetCertificateFd(struct Server *server, int fd) {
  sslSetCertificateFd(&server->ssl, fd);
}

void serverSetNumericHosts(struct Server *server, int numericHosts) {
  server->numericHosts = numericHosts;
}

struct Trie *serverGetHttpHandlers(struct Server *server) {
  return &server->handlers;
}
