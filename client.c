/*
 * client.c - A simple HTTP client in C
 *
 * Build  : gcc -Wall -Wextra -o client client.c
 * Usage  : ./client <hostname | ip address>[:port] [path]
 * Example: ./client example.com
 *          ./client 127.0.0.1 /index.html
 *          ./client localhost:8080
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

#define DEFAULT_PORT      "80"   /* standard HTTP port */
#define HOST_SIZE         256    /* max length of the host part */
#define REQUEST_SIZE      2048   /* max length of the HTTP request */
#define BUFFER_SIZE       4096   /* size of the receive buffer */
#define RECV_TIMEOUT_SEC  15     /* give up if the server stays silent */

int create_socket_and_connect(const char *hostname) {
    // This function creates a socket and connects to the given hostname.
    // hostname can be a domain name or an IP address (optionally ":port").
    // It returns the connected socket, or -1 on error.
    char host[HOST_SIZE];
    const char *port = DEFAULT_PORT;
    const char *colon;
    size_t host_len;
    struct addrinfo hints, *result, *rp;
    int sockfd = -1;
    int status;
    int sock_err = 0, conn_err = 0;

    if (hostname == NULL || *hostname == '\0') {
        fprintf(stderr, "Error: hostname is empty\n");
        return -1;
    }

    // One colon means "host:port". More than one means an IPv6 address.
    colon = strchr(hostname, ':');
    if (colon != NULL && strchr(colon + 1, ':') == NULL) {
        host_len = (size_t)(colon - hostname);
        if (colon[1] != '\0')
            port = colon + 1;
    } else {
        host_len = strlen(hostname);
    }
    if (host_len == 0 || host_len >= sizeof(host)) {
        fprintf(stderr, "Error: invalid hostname '%s'\n", hostname);
        return -1;
    }
    memcpy(host, hostname, host_len);
    host[host_len] = '\0';

    // Resolve the name or IP address (TCP only, IPv4 or IPv6)
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo(host, port, &hints, &result);
    if (status != 0) {
        fprintf(stderr, "Error: cannot resolve '%s': %s\n",
                hostname, gai_strerror(status));
        return -1;
    }

    // Try each address until one connects
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) {
            sock_err = errno;
            continue;
        }
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;                       // connected
        conn_err = errno;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(result);

    if (sockfd < 0) {
        if (conn_err != 0)
            fprintf(stderr, "Error: cannot connect to %s: %s\n",
                    hostname, strerror(conn_err));
        else
            fprintf(stderr, "Error: cannot create socket: %s\n",
                    strerror(sock_err));
        return -1;
    }
    return sockfd;
}

void send_http_get_request(int sockfd, const char *hostname, const char *path) {
    // This function sends an HTTP GET request to the specified hostname
    // and asks for the object given in path.
    char request[REQUEST_SIZE];
    const char *slash;
    size_t total = 0;
    int len;

    if (path == NULL)
        path = "";
    slash = (path[0] == '/') ? "" : "/";   // path must start with '/'

    // "Connection: close" makes the server close the socket after replying,
    // so the receive loop knows when the response is complete.
    len = snprintf(request, sizeof(request),
                   "GET %s%s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: SimpleHTTPClient/1.0\r\n"
                   "Accept: */*\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   slash, path, hostname);
    if (len < 0 || (size_t)len >= sizeof(request)) {
        fprintf(stderr, "Error: HTTP request is too long\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // send() may write only part of the data, so loop until all is sent
    while (total < (size_t)len) {
        ssize_t sent = send(sockfd, request + total, (size_t)len - total, 0);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Error: failed to send request: %s\n",
                    strerror(errno));
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        total += (size_t)sent;
    }

    printf("HTTP GET request sent to %s\n", hostname);
    fflush(stdout);
}

void receive_http_response(int sockfd) {
    // This function receives and prints the HTTP response from the socket.
    char buffer[BUFFER_SIZE];
    struct timeval timeout;
    size_t total = 0;
    char last = '\n';
    ssize_t n;

    // Do not wait forever for a silent server
    timeout.tv_sec = RECV_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0)
        fprintf(stderr, "Warning: cannot set receive timeout: %s\n",
                strerror(errno));

    // Read until the server closes the connection (recv returns 0)
    while ((n = recv(sockfd, buffer, sizeof(buffer), 0)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && total > 0)
                break;                   // got a reply, server kept socket open
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                fprintf(stderr, "Error: timed out waiting for the server\n");
            else
                fprintf(stderr, "Error: failed to receive response: %s\n",
                        strerror(errno));
            close(sockfd);
            exit(EXIT_FAILURE);
        }
        fwrite(buffer, 1, (size_t)n, stdout);   // fwrite is safe for any bytes
        last = buffer[n - 1];
        total += (size_t)n;
    }

    if (total == 0) {
        fprintf(stderr, "Error: server closed the connection without a response\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    if (last != '\n')
        putchar('\n');                   // keep the shell prompt on its own line
    fflush(stdout);
}

void interact_with_server(const char *hostname, const char *path) {
    // This function interacts with an HTTP server using the flow of
    // function calls below.
    printf("Interacting with server %s...\n", hostname);
    fflush(stdout);

    // Call function to create a socket and connect to the server
    int sockfd = create_socket_and_connect(hostname);
    if (sockfd < 0)
        exit(EXIT_FAILURE);              // error message already printed

    // Call function to send an HTTP GET request
    send_http_get_request(sockfd, hostname, path);

    // Call function to receive and print the HTTP response
    receive_http_response(sockfd);

    // Call function to close the socket
    close(sockfd);
}

int main(int argc, char *argv[]) {
    // This is the main() function. It takes the server's hostname (or IP)
    // from the command line, with an optional path, and talks to the server.
    const char *hostname;
    const char *path = "/";

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <hostname | ip address>[:port] [path]\n",
                argv[0]);
        return 1;
    }
    hostname = argv[1];
    if (argc == 3)
        path = argv[2];

    // If the server closes early, send() should fail with an error
    // instead of killing the program with SIGPIPE.
    signal(SIGPIPE, SIG_IGN);

    // Call function to interact with the server
    interact_with_server(hostname, path);

    return 0;
}
