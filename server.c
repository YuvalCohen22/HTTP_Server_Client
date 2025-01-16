#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "threadpool.h"

#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
#define BUFFER_SIZE 4000

int handle_client(const int *client_sock);
int check_request(char *request);
bool isValidHttpVersion(const char *version);
void send_response(int client_sock, const char *status, const char *body, const char *content_type);
char *get_mime_type(char *name);

int main(int argc, char *argv[]) {

    if (argc != 5) {
        printf("Usage: server <port> <pool-size> <max-queue-size> <max-number-of-request>");
        exit(1);
    }

    int port = atoi(argv[1]);
    int pool_size = atoi(argv[2]);
    int max_queue_size = atoi(argv[3]);
    int num_of_request = atoi(argv[4]);
    int server_sock;
    struct sockaddr_in srv;
    struct sockaddr_in cli;
    socklen_t client_len = sizeof(cli);

    // Create socket
    if ((server_sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(port);

    if(bind(server_sock, (struct sockaddr*) &srv, sizeof(srv)) < 0) {
        perror("bind"); exit(1);
    }

    if(listen(server_sock, 5) < 0) {
        perror("listen");
        exit(1);
    }

    int counter = 0;

    while (counter++ < num_of_request) {
        int* client_sock = malloc(sizeof(int));
        (*client_sock) = accept(server_sock, (struct sockaddr *)&cli, &client_len);
        if ((*client_sock) < 0) {
            perror("accept");
            // free
            exit(1);
        }
        handle_client(client_sock);
        close(*client_sock);
        free(client_sock);
    }

    close(server_sock);
    return 0;

}

// handle given request. return -1 if fails, 0 on successes.
int handle_client(const int *client_sock) {
    char request[BUFFER_SIZE];
    ssize_t bytes_read = read(*client_sock, request, BUFFER_SIZE);
    if (bytes_read <= 0) {
        perror("read");
        return -1;
    }
    char* end_of_first_line = strstr(request, "\r\n");
    if (end_of_first_line == NULL) {
        send_response(*client_sock, "400 Bad Request", NULL, NULL);
        return -1;
    }
    end_of_first_line[0] = '\0';
    if (check_request(request) == 400) {
        send_response(*client_sock, "400 Bad Request", NULL, NULL);
    }

    send_response(*client_sock, "200 OK", "<h1>Welcome</h1>", "text/html");
    return 0;
}

// check if request is a bad request. return 400 on bad request and 0 if good.
int check_request(char *request) {
    if (request == NULL) {
        return 400;
    }
    char buffer[BUFFER_SIZE];
    strncpy(buffer, request, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *method = strtok(buffer, " ");
    char *path = strtok(NULL, " ");
    char *protocol = strtok(NULL, " ");
    char *extra = strtok(NULL, " ");

    if (method == NULL || path == NULL || protocol == NULL || extra != NULL) {
        return 400;
    }

    if (!isValidHttpVersion(protocol)) {
        return 400;
    }

    return 0;
}

void send_response(int client_sock, const char *status, const char *body, const char *content_type) {
    char time_buffer[128];
    time_t now = time(NULL);
    strftime(time_buffer, sizeof(time_buffer), RFC1123FMT, gmtime(&now));

    char response[BUFFER_SIZE];
    int content_length = body ? strlen(body) : 0;

    snprintf(response, sizeof(response),
             "HTTP/1.0 %s\r\n"
             "Server: webserver/1.0\r\n"
             "Date: %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             status, time_buffer, content_type ? content_type : "text/plain", content_length, body ? body : "");

    send(client_sock, response, strlen(response), 0);
}

char *get_mime_type(char *name) {
    char *ext = strrchr(name, '.');
    if (!ext) return NULL;
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".au") == 0) return "audio/basic";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcmp(ext, ".mpeg") == 0 || strcmp(ext, ".mpg") == 0) return "video/mpeg";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    return NULL;
}

bool isValidHttpVersion(const char *version) {
    const char *validVersions[] = {
            "HTTP/1.0",
            "HTTP/1.1",
            "HTTP/2.0",
            "HTTP/3.0"
    };
    size_t numVersions = sizeof(validVersions) / sizeof(validVersions[0]);
    for (size_t i = 0; i < numVersions; ++i) {
        if (strcmp(version, validVersions[i]) == 0) {
            return true;
        }
    }
    return false;
}


