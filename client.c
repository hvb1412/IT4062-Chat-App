
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define LINE_MAX_BYTES 4096
#define OUT_CAPACITY 16384


struct line_reader {
    char data[LINE_MAX_BYTES + 2];
    size_t used;
};

struct out_queue {
    char data[OUT_CAPACITY];
    size_t used;
};

typedef int (*line_callback)(const char *line, void *context);

int line_feed(struct line_reader *reader, const char *data, size_t length, line_callback callback,
    void *context);
int queue_line(struct out_queue *queue, const char *line);
int parse_port(const char *text, unsigned short *port);
int send_all(int fd, const char *data, size_t length);
int main(int argc, char **argv);

int line_feed(struct line_reader *reader, const char *data, size_t length,
              line_callback callback, void *context)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)data[i];
        if (byte == 0 || (reader->used > 0 &&
            reader->data[reader->used - 1] == '\r' && byte != '\n'))
            return -1;
        if (byte == '\n') {
            if (reader->used == 0 || reader->data[reader->used - 1] != '\r')
                return -1;
            reader->data[reader->used - 1] = '\0';
            reader->used = 0;
            int result = callback(reader->data, context);
            if (result != 0)
                return result;
        } else {
            if (reader->used >= LINE_MAX_BYTES + 1 ||
                (byte != '\r' && reader->used >= LINE_MAX_BYTES))
                return -1;
            reader->data[reader->used++] = (char)byte;
        }
    }
    return 0;
}

int queue_line(struct out_queue *queue, const char *line)
{
    size_t length = strlen(line);
    if (length > LINE_MAX_BYTES || strpbrk(line, "\r\n") != NULL ||
        length + 2 > OUT_CAPACITY - queue->used)
        return -1;
    memcpy(queue->data + queue->used, line, length);
    memcpy(queue->data + queue->used + length, "\r\n", 2);
    queue->used += length + 2;
    return 0;
}

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



int main(int argc, char **argv)
{
    unsigned short port;
    if (argc != 3 || parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s <server-ipv4> <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &server.sin_addr) != 1)
        return EXIT_FAILURE;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return EXIT_FAILURE;
    if (fd >= FD_SETSIZE || connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }
    int input_done = 0, status = EXIT_SUCCESS;
    while (1) {
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        if (!input_done)
            FD_SET(STDIN_FILENO, &reads);
        if (select(fd + 1, &reads, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            status = EXIT_FAILURE;
            break;
        }
        if (FD_ISSET(fd, &reads)) {
            char buffer[1024];
            ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                if (count < 0)
                    status = EXIT_FAILURE;
                break;
            }
            fwrite(buffer, 1, (size_t)count, stdout);
            fflush(stdout);
        }
        if (!input_done && FD_ISSET(STDIN_FILENO, &reads)) {
            char buffer[1024];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                status = EXIT_FAILURE;
                break;
            }
            if (!count) {
                input_done = 1;
                if (shutdown(fd, SHUT_WR) < 0) {
                    status = EXIT_FAILURE;
                    break;
                }
            } else if (send_all(fd, buffer, (size_t)count) < 0) {
                status = EXIT_FAILURE;
                break;
            }
        }
    }
    close(fd);
    return status;
}
