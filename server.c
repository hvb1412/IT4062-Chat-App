#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main() {
  int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if(listener < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

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

  if(listen(listener, 5) < 0) {
    perror("listen");
    close(listener);
    return EXIT_FAILURE;
  }

  printf("Server is listening on port 8080...\n");

  while(1) {
    int client_socket = accept(listener, NULL, NULL);

    if(client_socket < 0) {
      perror("accept");
      continue;
    }

    printf("Client connected!\n");

    while(1) {
      char buffer[10240];

      ssize_t bytes_received = recv(client_socket, buffer, sizeof(buffer), 0);

      if(bytes_received <= 0) {
        break;
      }

      ssize_t bytes_sent = send(client_socket, buffer, bytes_received, 0);

      if(bytes_sent < 0) {
        perror("send");
        break;
      }
    }

    printf("Client disconnected!\n");
    close(client_socket);
  }

  close(listener);

  return EXIT_SUCCESS;
}
