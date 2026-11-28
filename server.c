#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <fcntl.h>

#define PORT 8080
#define MAX_CLIENTS 64
#define BUFFER_SIZE 1024
#define LINE_MAX_BYTES 4096
#define OUT_CAPACITY 16384
#define NICKNAME_MAX 24

struct line_reader {
  char data[LINE_MAX_BYTES + 1];
  size_t used;
  int saw_cr;
};

struct out_queue {
  char data[OUT_CAPACITY];
  size_t used;
};

struct client {
  int fd;
  int read_closed; /* Dừng đọc sau EOF hoặc QUIT. */
  char nickname[NICKNAME_MAX + 1];
  struct line_reader input;
  struct out_queue output;
};

static volatile sig_atomic_t stop_requested = 0;
static void handle_stop(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

int read_line_byte(struct line_reader *reader, char byte) {
  if (reader->saw_cr) {
    if (byte != '\n') {
      return -1;
    }

    reader->data[reader->used] = '\0';
    reader->used = 0;
    reader->saw_cr = 0;
    return 1;
  }

  if (byte == '\r') {
    reader->saw_cr = 1;
    return 0;
  }

  if (byte == '\n' || byte == '\0' || reader->used >= LINE_MAX_BYTES) {
    return -1;
  }

  reader->data[reader->used] = byte;
  reader->used++;
  return 0;
}

int queue_line(struct out_queue *queue, const char *line) {
  size_t length = strlen(line);

  if (length > LINE_MAX_BYTES ||
      strchr(line, '\r') != NULL ||
      strchr(line, '\n') != NULL) {
    return -1;
  }

  size_t frame_size = length + 2;
  if (frame_size > OUT_CAPACITY - queue->used) {
    return -1;
  }

  memcpy(queue->data + queue->used, line, length);
  queue->data[queue->used + length] = '\r';
  queue->data[queue->used + length + 1] = '\n';
  queue->used += frame_size;
  return 0;
}

int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }

  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int flush_queue(int fd, struct out_queue *queue) {
  while (queue->used > 0) {
    ssize_t sent = send(fd, queue->data, queue->used, MSG_NOSIGNAL);

    if (sent > 0) {
      queue->used -= (size_t)sent;
      memmove(queue->data, queue->data + sent, queue->used);
      continue;
    }

    if (sent < 0 && errno == EINTR) {
      continue;
    }

    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return 0;
    }

    return -1;
  }

  return 0;
}

static int join_client(struct client *client, const char *name) {
  size_t length = strlen(name);
  if (length < 3 || length > NICKNAME_MAX) {
    return queue_line(&client->output, "400 INVALID NICKNAME");
  }

  for (size_t i = 0; i < length; i++) {
    char ch = name[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
      return queue_line(&client->output, "400 INVALID NICKNAME");
    }
  }

  if (client->nickname[0] != '\0') {
    return queue_line(&client->output, "409 NAME IN USE");
  }
  if (queue_line(&client->output, "100 JOINED") < 0) {
    return -1;
  }

  memcpy(client->nickname, name, length + 1);
  return 0;
}

static int read_client(struct client *client) {
  char buffer[BUFFER_SIZE];
  ssize_t received = recv(client->fd, buffer, sizeof(buffer), 0);

  if (received < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    }
    perror("recv");
    return -1;
  }

  if (received == 0) {
    if (client->input.used > 0 || client->input.saw_cr) {
      return -1;
    }
    /* Client đóng chiều gửi, server vẫn gửi nốt phản hồi đang chờ. */
    client->read_closed = 1;
    return 0;
  }

  /* Mỗi socket giữ riêng phần dòng còn dang dở giữa các lần recv(). */
  for (ssize_t i = 0; i < received; i++) {
    int result = read_line_byte(&client->input, buffer[i]);
    if (result < 0) {
      return -1;
    }
    if (result == 1) {
      const char *line = client->input.data;
      if (strcmp(line, "QUIT") == 0) {
        if (queue_line(&client->output, "100 BYE") < 0) {
          return -1;
        }
        client->read_closed = 1;
        return 0; /* Dừng xử lý các lệnh sau QUIT trong lần đọc này. */
      }

      if (strncmp(line, "JOIN ", 5) == 0) {
        if (join_client(client, line + 5) < 0) {
          return -1;
        }
        continue;
      }

      const char *response = "400 UNKNOWN COMMAND";
      if (strcmp(line, "PING") == 0) {
        response = "100 PONG";
      } else if (strncmp(line, "ECHO ", 5) == 0) {
        response = line + 5;
      } else if (strcmp(line, "JOIN") == 0) {
        response = "400 INVALID NICKNAME";
      } else if (*line != '\0' && client->nickname[0] == '\0') {
        response = "401 JOIN REQUIRED";
      }
      if (queue_line(&client->output, response) < 0) {
        return -1;
      }
    }
  }

  return 0;
}

static void close_client(struct client *client) {
  printf("Client %d disconnected!\n", client->fd);
  close(client->fd);
  client->fd = -1;
}

int main(void) {
  if(signal(SIGINT, handle_stop) == SIG_ERR || signal(SIGTERM, handle_stop) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  /* 1. Tạo socket lắng nghe */
  int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if(listener < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  if (listener >= FD_SETSIZE) {
    fprintf(stderr, "Listener fd exceeds select() limit\n");
    close(listener);
    return EXIT_FAILURE;
  }
  if (set_nonblocking(listener) < 0) {
    perror("set_nonblocking listener");
    close(listener);
    return EXIT_FAILURE;
  }

  /* Cho phép bind lại cổng ngay khi vừa tắt server */
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  /* 2. Gắn socket vào địa chỉ và cổng */
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(PORT);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if(bind(listener, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind");
    close(listener);
    return EXIT_FAILURE;
  }

  /* 3. Bắt đầu lắng nghe */
  if(listen(listener, 5) < 0) {
    perror("listen");
    close(listener);
    return EXIT_FAILURE;
  }

  printf("Server is listening on port %d...\n", PORT);

  /* 4. Mảng lưu các client đang kết nối, -1 là trống */
  static struct client clients[MAX_CLIENTS];
  for(int i = 0; i < MAX_CLIENTS; i++) {
    clients[i].fd = -1;
  }

  int exit_status = EXIT_SUCCESS;

  while(!stop_requested) {
    /* 5. Theo dõi đọc và chỉ theo dõi ghi khi còn dữ liệu chờ. */
    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_SET(listener, &read_fds);
    int max_fd = listener;

    for (int i = 0; i < MAX_CLIENTS; i++) {
      struct client *client = &clients[i];
      if (client->fd < 0) {
        continue;
      }
      if (!client->read_closed) {
        FD_SET(client->fd, &read_fds);
      }
      if (client->output.used > 0) {
        FD_SET(client->fd, &write_fds);
      }
      if (client->fd > max_fd) {
        max_fd = client->fd;
      }
    }

    struct timeval timeout = {1, 0};
    int activity = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);

    if(activity < 0) {
      if(errno == EINTR) {
        continue;
      }

      perror("select");
      exit_status = EXIT_FAILURE;
      break;
    }

    if(stop_requested) {
      break;
    }

    if(activity == 0) {
      continue;
    }

    /* 7. Gửi phản hồi đang chờ rồi đọc dữ liệu mới nếu socket sẵn sàng. */
    for (int i = 0; i < MAX_CLIENTS && !stop_requested; i++) {
      struct client *client = &clients[i];
      if (client->fd < 0) {
        continue;
      }

      if (FD_ISSET(client->fd, &write_fds) &&
          flush_queue(client->fd, &client->output) < 0) {
        close_client(client);
        continue;
      }

      if (FD_ISSET(client->fd, &read_fds) && read_client(client) < 0) {
        close_client(client);
        continue;
      }

      if (client->read_closed && client->output.used == 0) {
        close_client(client);
      }
    }

    /* 8. Đưa kết nối mới vào vòng select() kế tiếp. */
    if (!stop_requested && FD_ISSET(listener, &read_fds)) {
      int client_socket = accept(listener, NULL, NULL);
      if (client_socket < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          perror("accept");
        }
        continue;
      }
      if (client_socket >= FD_SETSIZE) {
        fprintf(stderr, "Client fd exceeds select() limit\n");
        close(client_socket);
        continue;
      }

      int slot = -1;
      for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == -1) {
          slot = i;
          break;
        }
      }

      if (slot == -1) {
        printf("Server is full, reject client %d\n", client_socket);
        close(client_socket);
      } else if (set_nonblocking(client_socket) < 0) {
        perror("set_nonblocking client");
        close(client_socket);
      } else {
        memset(&clients[slot], 0, sizeof(clients[slot]));
        clients[slot].fd = client_socket;
        printf("Client %d connected!\n", client_socket);
      }
    }
  }

  for(int i = 0; i < MAX_CLIENTS; i++) {
    if(clients[i].fd != -1) {
      close(clients[i].fd);
    }
  }

  close(listener);

  return exit_status;
}
