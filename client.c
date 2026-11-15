
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
static int open_connection(int argc, char **argv);
int main(int argc, char **argv);


/* Chỉ chấp nhận cổng có chữ số và nằm trong khoảng 1–65535. */
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


/* Kết nối bằng IPv4/cổng được truyền từ dòng lệnh. */
static int open_connection(int argc, char **argv)
{
    unsigned short port;
    if (argc != 3 || parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s <server-ipv4> <port>\n", argv[0]);
        return -1;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    int fd = open_connection(argc, argv);
    if (fd < 0) return EXIT_FAILURE;
    puts("Ket noi thanh cong");
    close(fd);
    return EXIT_SUCCESS;
}
