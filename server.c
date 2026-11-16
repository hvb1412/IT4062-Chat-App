#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

#define PORT 8080
#define MAX_CLIENTS 64
#define BUFFER_SIZE 1024

int main() {
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

  while(1) {
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

    /* 6. Chờ cho tới khi có socket nào đó sẵn sàng đọc */
    int activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);

    if(activity < 0) {
      perror("select");
      break;
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

      char buffer[BUFFER_SIZE];
      ssize_t bytes_received = recv(clients[i], buffer, sizeof(buffer), 0);

      if(bytes_received <= 0) {
        printf("Client %d disconnected!\n", clients[i]);
        close(clients[i]);
        clients[i] = -1;
      } else {
        ssize_t bytes_sent = send(clients[i], buffer, bytes_received, 0);

        if(bytes_sent < 0) {
          perror("send");
          close(clients[i]);
          clients[i] = -1;
        }
      }
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
