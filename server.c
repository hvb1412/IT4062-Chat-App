#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define PORT 8080
#define MAX_CLIENTS 64
#define BUFFER_SIZE 1024

static volatile sig_atomic_t stop_requested = 0;
static void handle_stop(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

static int send_all(int fd, const char *buffer, size_t length) {
  size_t total_sent = 0;

  while(total_sent < length && !stop_requested) {
    ssize_t sent = send(fd, buffer + total_sent, length - total_sent, MSG_NOSIGNAL);

    if(sent < 0) {
      if(errno == EINTR) {
        continue;
      }

      perror("send");
      return -1;
    }

    if(sent == 0) {
      return -1;
    }

    total_sent += (size_t)sent;
  }

  return total_sent == length ? 0 : -1;
}

int main() {
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

  /* Cho phép bind lại cổng ngay khi vừa tắt server */
  int reuse = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  /* 2. Gắn socket vào địa chỉ và cổng */
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(8080);
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

  printf("Server is listening on port 8080...\n");

  /* 4. Mảng lưu các client đang kết nối, -1 là trống */
  int clients[MAX_CLIENTS];
  for(int i = 0; i < MAX_CLIENTS; i++) {
    clients[i] = -1;
  }

  int exit_status = EXIT_SUCCESS;

  while(!stop_requested) {
    /* 5. Dựng lại tập socket cần theo dõi ở mỗi vòng lặp */
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listener, &read_fds);
    int max_fd = listener;

    for(int i = 0; i < MAX_CLIENTS; i++) {
      if(clients[i] != -1) {
        FD_SET(clients[i], &read_fds);
        if(clients[i] > max_fd) {
          max_fd = clients[i];
        }
      }
    }

    struct timeval timeout = {1, 0};
    int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

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

    /* 7. Có client mới kết nối */
    if(FD_ISSET(listener, &read_fds)) {
      int client_socket = accept(listener, NULL, NULL);

      if(client_socket < 0) {
        perror("accept");
      } else {
        int slot = -1;
        for(int i = 0; i < MAX_CLIENTS; i++) {
          if(clients[i] == -1) {
            slot = i;
            break;
          }
        }

        if(slot == -1) {
          printf("Server is full, reject client %d\n", client_socket);
          close(client_socket);
        } else {
          clients[slot] = client_socket;
          printf("Client %d connected!\n", client_socket);
        }
      }
    }

    /* 8. Client nào gửi dữ liệu thì echo lại cho client đó */
    for(int i = 0; i < MAX_CLIENTS; i++) {
      if(clients[i] == -1) {
        continue;
      }

      if(!FD_ISSET(clients[i], &read_fds)) {
        continue;
      }

      int fd = clients[i];
      char buffer[BUFFER_SIZE];
      ssize_t received = recv(fd, buffer, sizeof(buffer), 0);

      if(received < 0 && errno == EINTR) {
        continue;
      }

      if(received > 0 && send_all(fd, buffer, (size_t)received) == 0) {
        continue;
      }

      if(received == 0) {
        printf("Clients %d disconnected!\n", fd);
      } else if (received < 0) {
        perror("recv");
      }

      close(fd);
      clients[i] = -1;
    }
  }

  for(int i = 0; i < MAX_CLIENTS; i++) {
    if(clients[i] != -1) {
      close(clients[i]);
    }
  }

  close(listener);

  return EXIT_FAILURE;
}
