
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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


struct client_state {
    struct out_queue output;
    int input_done;
};

int line_feed(struct line_reader *reader, const char *data, size_t length, line_callback callback,
    void *context);
int queue_line(struct out_queue *queue, const char *line);
int parse_port(const char *text, unsigned short *port);
int set_nonblocking(int fd);
int flush_queue(int fd, struct out_queue *queue);
static int print_line(const char *line, void *context);
static int submit_input(char *line, struct client_state *state);
static int submit_ping(struct client_state *state);
static int submit_echo(const char *text, struct client_state *state);
static int request_disconnect(struct client_state *state);
int main(int argc, char **argv);

int line_feed(struct line_reader *reader, const char *data, size_t length,
              line_callback callback, void *context)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)data[i];
        /* Từ chối byte NUL và ký tự CR không được theo ngay sau bởi LF. */
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
    if (*text == '\0')
        return -1;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char *end;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value == 0 || value > 65535)
        return -1;
    *port = (unsigned short)value;
    return 0;
}

/* Giữ các cờ hiện tại và thêm O_NONBLOCK để socket không chờ dữ liệu vô hạn. */
int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Lặp gửi dữ liệu; khi gửi thiếu thì dồn phần chưa gửi về đầu hàng đợi. */
int flush_queue(int fd, struct out_queue *queue)
{
    while (queue->used > 0) {
        ssize_t sent = send(fd, queue->data, queue->used, MSG_NOSIGNAL);
        if (sent > 0) {
            queue->used -= (size_t)sent;
            /* Chỉ loại phần đã gửi thành công, giữ nguyên thứ tự các byte còn lại. */
            memmove(queue->data, queue->data + sent, queue->used);
        } else if (sent < 0 && errno == EINTR) {
            /* Tín hiệu ngắt lời gọi hệ thống: thử gửi lại. */
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
    return 0;
}

/* Xử lý một dòng hoàn chỉnh từ server và in ngay ra terminal. */
static int print_line(const char *line, void *context)
{
    (void)context;
    puts(line);
    fflush(stdout);
    return 0;
}

/* Xử lý lệnh cục bộ hoặc đổi lệnh nhập thành thông điệp gửi server. */
static int submit_ping(struct client_state *state)
{
    if (queue_line(&state->output, "PING") < 0) {
        fprintf(stderr, "Khong du cho de gui PING\n");
        return -1;
    }
    return 0;
}

/* Kiểm tra nội dung và độ dài trước khi dựng frame ECHO. */
static int submit_echo(const char *text, struct client_state *state)
{
    char frame[LINE_MAX_BYTES + 1];
    if (*text == '\0' || strlen(text) > LINE_MAX_BYTES - 5) {
        fprintf(stderr, "Dung: /echo <noi dung>, toi da %d byte\n", LINE_MAX_BYTES - 5);
        return 0; /* Lỗi nhập: người dùng có thể thử lại. */
    }
    snprintf(frame, sizeof(frame), "ECHO %s", text);
    return queue_line(&state->output, frame);
}


/* Chỉ dừng đọc bàn phím khi QUIT đã được xếp thành công; vẫn chờ phản hồi server. */
static int request_disconnect(struct client_state *state)
{
    if (queue_line(&state->output, "QUIT") < 0)
        return -1;
    state->input_done = 1;
    return 0;
}

static int submit_input(char *line, struct client_state *state)
{
    if (strcmp(line, "/quit") == 0 || strcmp(line, "QUIT") == 0)
        return request_disconnect(state);
    else if (strcmp(line, "/ping") == 0)
        return submit_ping(state);
    if (strcmp(line, "/echo") == 0 || strncmp(line, "/echo ", 6) == 0)
        return submit_echo(line[5] == ' ' ? line + 6 : "", state);
    if (*line == '\0')
        return 0;
    if (queue_line(&state->output, line) < 0) {
        fprintf(stderr, "Input/output buffer limit reached\n");
        return -1;
    }
    return 0;
}



int main(int argc, char **argv)
{
    unsigned short port;
    if (argc != 3 || parse_port(argv[2], &port) < 0) {
        fprintf(stderr, "Usage: %s <server-ipv4> <port 1..65535>\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct sockaddr_in server = {0};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, argv[1], &server.sin_addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address\n");
        return EXIT_FAILURE;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (fd >= FD_SETSIZE || connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0 ||
        set_nonblocking(fd) < 0) {
        perror("connect/setup");
        close(fd);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "Connected. Commands: /ping, /quit (or raw protocol lines)\n");
    struct client_state state = {0};
    struct line_reader incoming = {0};
    char input[LINE_MAX_BYTES + 1];
    size_t input_used = 0;
    int write_closed = 0;
    int status = EXIT_SUCCESS;
    while (1) {
        fd_set reads, writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(fd, &reads);
        if (!state.input_done)
            FD_SET(STDIN_FILENO, &reads);
        if (state.output.used)
            FD_SET(fd, &writes);
        if (select(fd + 1, &reads, &writes, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            status = EXIT_FAILURE;
            break;
        }
        if (FD_ISSET(fd, &reads)) {
            char buffer[2048];
            ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                if (line_feed(&incoming, buffer, (size_t)count, print_line, NULL) < 0) {
                    fprintf(stderr, "Invalid server frame\n");
                    status = EXIT_FAILURE;
                    break;
                }
            } else if (count == 0) {
                fprintf(stderr, "Server closed connection\n");
                if (incoming.used || state.output.used || !state.input_done)
                    status = EXIT_FAILURE;
                break;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recv");
                status = EXIT_FAILURE;
                break;
            }
        }
        if (!state.input_done && FD_ISSET(STDIN_FILENO, &reads)) {
            char buffer[1024];
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0 && errno != EINTR) {
                perror("stdin");
                status = EXIT_FAILURE;
                break;
            }
            if (count == 0) {
                if (input_used) {
                    input[input_used] = '\0';
                    if (submit_input(input, &state) < 0) {
                        status = EXIT_FAILURE;
                        break;
                    }
                }
                state.input_done = 1;
            }
            for (ssize_t i = 0; i < count && !state.input_done; ++i) {
                if (buffer[i] == '\n') {
                    if (input_used && input[input_used - 1] == '\r')
                        --input_used;
                    input[input_used] = '\0';
                    if (submit_input(input, &state) < 0) {
                        status = EXIT_FAILURE;
                        break;
                    }
                    input_used = 0;
                } else if (buffer[i] == '\0' || input_used >= LINE_MAX_BYTES) {
                    fprintf(stderr, "Invalid or oversized input\n");
                    status = EXIT_FAILURE;
                    break;
                } else {
                    input[input_used++] = buffer[i];
                }
            }
            if (status != EXIT_SUCCESS)
                break;
        }
        if (FD_ISSET(fd, &writes) && flush_queue(fd, &state.output) < 0) {
            perror("send");
            status = EXIT_FAILURE;
            break;
        }
        if (state.input_done && state.output.used == 0 && !write_closed) {
            if (shutdown(fd, SHUT_WR) < 0) {
                perror("shutdown");
                status = EXIT_FAILURE;
                break;
            }
            write_closed = 1;
        }
    }
    close(fd);
    return status;
}
