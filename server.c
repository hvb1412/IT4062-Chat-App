
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

int parse_port(const char *text, unsigned short *port);
int send_all(int fd, const char *data, size_t length);
static int open_listener(int argc, char **argv);
int main(int argc, char **argv);


int parse_port(const char *text, unsigned short *port)
{
    if (!*text)
        return -1;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || !value || value > 65535)
        return -1;
    *port = (unsigned short)value;
    return 0;
}


int send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (n > 0)
            sent += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 0;
}



/* Kiểm tra địa chỉ/cổng rồi tạo socket lắng nghe. */
static int open_listener(int argc, char **argv)
{
    unsigned short port;
    if (argc != 3 || parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s <bind-ipv4> <port>\n", argv[0]);
        return -1;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(fd, 16) < 0) {
        perror("listener");
        close(fd);
        return -1;
    }
    return fd;
}


int main(int argc, char **argv)
{
    int listener = open_listener(argc, argv);
    if (listener < 0) return EXIT_FAILURE;
    int peer = accept(listener, NULL, NULL);
    if (peer < 0) {
        perror("accept");
        close(listener);
        return EXIT_FAILURE;
    }
    puts("Da ket noi mot client");
    close(peer);
    close(listener);
    return EXIT_SUCCESS;
}
