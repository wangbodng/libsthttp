#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http_stream.h"

#define SEC2USEC(s) ((s)*1000000LL)

void *handle_connection(void *arg) {
  st_netfd_t client_nfd = (st_netfd_t)arg;
  struct http_stream *s = http_stream_create(HTTP_SERVER, SEC2USEC(5));
  char buf[4*1024];
  int error = 0;
  struct http_stream *cs = NULL;
  uri_t *u = uri_new();
  int should_close = 1;
  for (;;) {
    should_close = 1;
    if (s->status != HTTP_STREAM_OK) break;
    cs = NULL;
    error = 0;
    s->timeout = SEC2USEC(5);
    int status = http_stream_request_read(s, client_nfd);
    s->timeout = SEC2USEC(30); // longer timeout for the rest
    if (status != HTTP_STREAM_OK) {
      if (s->status == HTTP_STREAM_CLOSED || s->status == HTTP_STREAM_TIMEOUT) {
        error = 1;
      } else {
        error = 400;
      }
      goto release;
    }
    cs = http_stream_create(HTTP_CLIENT, SEC2USEC(30));
    //http_request_debug_print(s->req);

    fprintf(stderr, "request uri: %s\n", s->req->uri);
    const char *error_at = NULL;
    uri_clear(u);
    if (uri_parse(u, s->req->uri, strlen(s->req->uri), &error_at) == 0) {
      fprintf(stderr, "uri_parse error: %s\n", error_at);
      error = 400;
      goto release;
    }
    uri_normalize(u);
    if (http_stream_connect(cs, u->host, u->port) != HTTP_STREAM_OK) { error = 504; goto release; }
    http_request_header_remove(s->req, "Accept-Encoding");
    http_request_header_remove(s->req, "Proxy-Connection");
    /* TODO: need to expose a copy api for http message */
    http_request_t *tmp_req = cs->req;
    cs->req = s->req;
    char *request_uri = uri_compose_partial(u);
    char *tmp_uri = s->req->uri;
    cs->req->uri = request_uri;
    if (http_stream_request_send(cs) != HTTP_STREAM_OK) { error = 504; goto release; }
    cs->req = tmp_req;
    s->req->uri = tmp_uri;
    free(request_uri);

    /* TODO: fix this. post might not contain data. probably move this logic into stream */
    size_t total = 0;
    if (g_strcmp0("POST", s->req->method) == 0) {
      for (;;) {
        ssize_t nr = sizeof(buf);
        status = http_stream_read(s, buf, &nr);
        fprintf(stderr, "server http_stream_read nr: %zd\n", nr);
        if (nr < 0 || status != HTTP_STREAM_OK) { error = 1; goto release; }
        if (nr == 0) break;
        /*fwrite(buf, sizeof(char), nr, stdout);*/
        ssize_t nw = st_write(cs->nfd, buf, nr, s->timeout);
        if (nw != nr) { error=1; goto release; }
        fprintf(stderr, "st_write nw: %zd\n", nr);
        total += nr;
      }
      fprintf(stderr, "http_stream_read total: %zu\n", total);
    }

    if (http_stream_response_read(cs) != HTTP_STREAM_OK) { error = 502; goto release; }

    /* TODO: properly create a new response and copy headers */
    http_response_t *tmp_resp = s->resp;
    s->resp = cs->resp;
    s->resp->http_version = "HTTP/1.1";
    http_response_header_remove(s->resp, "Content-Length");
    http_response_header_remove(s->resp, "Transfer-Encoding");
    if (s->resp->status_code != 204)
        http_response_header_append(s->resp, "Transfer-Encoding", "chunked");
    ssize_t nw = http_stream_response_send(s, 0);
    s->resp = tmp_resp;

    fprintf(stderr, "http_stream_response_send: %zd\n", nw);
    if (s->resp->status_code != 204 &&
           (cs->content_size > 0 || cs->transfer_encoding == TE_CHUNKED)) {
      total = 0;
      fprintf(stderr, "content size: %zd\n", cs->content_size);
      for (;;) {
        ssize_t nr = sizeof(buf);
        status = http_stream_read(cs, buf, &nr);
        fprintf(stderr, "client http_stream_read nr: %zd\n", nr);
        if (nr <= 0 || status != HTTP_STREAM_OK) break;
        /*fwrite(buf, sizeof(char), nr, stdout);*/
        total += nr;
        if (http_stream_send_chunk(s, buf, nr) != HTTP_STREAM_OK) break;
      }
      fprintf(stderr, "written to client: %zu\n", total);
      if (total > 0 && s->status == HTTP_STREAM_OK) {
        http_stream_send_chunk_end(s);
      } else {
        fprintf(stderr, "for request: %s status: %d\n", s->req->uri, s->status);
      }
    }
release:
    if (!error) {
        if ((g_strcmp0("HTTP/1.1", s->req->http_version) == 0) &&
          (g_strcmp0(http_request_header_getstr(s->req, "Connection"), "close") != 0)) {
          // if HTTP/1.1 client and no Connection: close, then don't close
          should_close = 0;
        } else if (g_strcmp0(http_request_header_getstr(s->req, "Connection"), "keepalive") == 0) {
          should_close = 0;
        }
    }
    http_request_clear(s->req);
    uri_clear(u);
    if (cs) http_stream_close(cs);
    /* TODO: break loop if HTTP/1.0 and not keep-alive */
    if (error) {
      fprintf(stderr, "ERROR: %d STATUS: %d, exiting\n", error, s->status);
      /* TODO: use reason string */
      if (error >= 400 && s->status != HTTP_STREAM_CLOSED) {
        http_response_free(s->resp);
        s->resp = http_response_new(error, "Error");
        http_response_header_append(s->resp, "Content-Length", "0");
        s->status = HTTP_STREAM_OK; /* TODO: might want to move this logic into http_stream */
        http_stream_response_send(s, 0);
      }
      break;
    }
    if (should_close) break;
  }
  fprintf(stderr, "exiting handle_connection (should_close: %u)\n", should_close);
  uri_free(u);
  http_stream_close(s);
  return NULL;
}

int main(int argc, char *argv[]) {

  g_assert(st_set_eventsys(ST_EVENTSYS_ALT) == 0);
  st_init();
  int status = ares_library_init(ARES_LIB_INIT_ALL);
  if (status != ARES_SUCCESS)
  {
    fprintf(stderr, "ares_library_init: %s\n", ares_strerror(status));
    return 1;
  }

  int sock;
  int n;
  struct sockaddr_in serv_addr;

  if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
    perror("socket");
  }

  n = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
    perror("setsockopt SO_REUSEADDR");
  }

  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(8080);
  serv_addr.sin_addr.s_addr = inet_addr("0.0.0.0");

  if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    perror("bind");
  }

  if (listen(sock, 10) < 0) {
    perror("listen");
  }

  st_netfd_t server_nfd = st_netfd_open_socket(sock);
  st_netfd_t client_nfd;
  struct sockaddr_in from;
  int fromlen = sizeof(from);

  for (;;) {
    client_nfd = st_accept(server_nfd,
      (struct sockaddr *)&from, &fromlen, ST_UTIME_NO_TIMEOUT);
    printf("accepted\n");
    if (st_thread_create(handle_connection,
      (void *)client_nfd, 0, 1024 * 1024) == NULL)
    {
      fprintf(stderr, "st_thread_create error\n");
    }
  }
  ares_library_cleanup();
  return EXIT_SUCCESS;
}
