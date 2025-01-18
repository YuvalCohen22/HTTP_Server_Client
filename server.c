#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <unistd.h>
#include "threadpool.h"

#define DEBUG 1
#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT"
#define MAX_FIRST_LINE 4000

#if DEBUG
#define DEBUG_PRINT(fmt, ...) \
        fprintf(stderr, "DEBUG: " fmt, ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...) \
        do { } while (0)
#endif

int handle_client(const int *client_sock);
int check_bad_request(char *request, char **path);
bool isValidHttpVersion(const char *version);
int check_path(char *path);
void send_response(int client_sock, const char *status, const char *body, const char *content_type);
char *get_mime_type(char *name);
bool does_file_exist(char *path, struct stat *stat_buf);
bool check_permission(char *path);
int is_index_html_in_directory(const char *directory_path);

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
    char request[MAX_FIRST_LINE];
    ssize_t bytes_read = read(*client_sock, request, MAX_FIRST_LINE);

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
    DEBUG_PRINT("%s\n", request);
    char* path;

    int check_req = check_bad_request(request, &path);

    if (check_req== 400) {
        send_response(*client_sock, "400 Bad Request", NULL, NULL);
    }

    else if (check_req == 501) {
        send_response(*client_sock, "501 Not Implemented", NULL, NULL);
    }

    int checked_path = check_path(path);

    if (checked_path == 404) {
        send_response(*client_sock, "404 Not Found", NULL, NULL);
    }

    if (checked_path == 302) {
        send_response(*client_sock, "302 Found", NULL, NULL);
    }

    if (checked_path == 403) {
        send_response(*client_sock, "403 Forbidden", NULL, NULL);
    }

    if (checked_path == 200) {
        send_response(*client_sock, "200 OK", NULL, NULL);
    }

    return 0;
}

int check_path(char *path) {
    struct stat stat_buf;

    if (!does_file_exist(path, &stat_buf)) {
        return 404;
    }

    if (S_ISDIR(stat_buf.st_mode)) {

        if (path[strlen(path) - 1] != '/') {
            return 302;
        }
        if (!check_permission(path))
            return 403;
        int check_index_html = is_index_html_in_directory(path);
        if (is_index_html_in_directory(path) == -1)
            return 501;
        if (check_index_html == 1) {
            strcat(path, "index.html");
            if (check_permission(path))
                return 200;
            return 403;
        }
        if (check_permission(path))
            return 200;
        return 403;
    }

    if (S_ISREG(stat_buf.st_mode)) {

        DEBUG_PRINT("path: %s\n", path);

        if (check_permission(path) == 0) {
            return 403;
        }
        return 200;
    }

    return 404;
}

// check if request is a bad request. return 400 on bad request, 501 on not GET method and 0 if good.
int check_bad_request(char *request, char **path) {
    if (request == NULL) {
        return 400;
    }
    char buffer[MAX_FIRST_LINE];
    strncpy(buffer, request, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char *method = strtok(buffer, " ");
    char *found_path = strtok(NULL, " ");
    char *protocol = strtok(NULL, " ");
    char *extra = strtok(NULL, " ");

    if (method == NULL || found_path == NULL || protocol == NULL || extra != NULL) {
        return 400;
    }

    if (!isValidHttpVersion(protocol)) {
        return 400;
    }

    if (strcmp(method, "GET") != 0) {
        return 501;
    }

    (*path) = found_path;

    return 0;
}

void send_response(int client_sock, const char *status, const char *body, const char *content_type) {
    char time_buffer[128];
    time_t now = time(NULL);
    strftime(time_buffer, sizeof(time_buffer), RFC1123FMT, gmtime(&now));

    char response[MAX_FIRST_LINE];
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

    send(client_sock, status, strlen(status), 0);
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

int create_response(int status_code, char* method) {
    char *firs_line = "HTTP/1.0";
    strcat(firs_line, method);
    strcat(firs_line, "\r\n");

    char* server = "Server: webserver/1.0";

    char* date = "Date: ";
    char time_buffer[128];
    time_t now = time(NULL);
    strftime(time_buffer, sizeof(time_buffer), RFC1123FMT, gmtime(&now));
    strcat(date, time_buffer);

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


// check if file exists.
bool does_file_exist(char *path, struct stat *stat_buf) {
    DEBUG_PRINT("path: %s\n", path);
    return stat(path+1, stat_buf) == 0;
}

bool check_permission(char *path) {
    char path_copy[MAX_FIRST_LINE];
    strncpy(path_copy, path, sizeof(path));
    char* directory = dirname(path_copy);
    struct stat dir_buf;
    while (strcmp(directory, "/") != 0) {
        if (stat(directory, &dir_buf) != 0 || !(dir_buf.st_mode & S_IROTH)){
            return false;
        }
        directory = dirname(directory);
    }
    return true;

}

int is_index_html_in_directory(const char *directory_path) {
    struct dirent *entry;
    DIR *directory = opendir(directory_path);

    if (directory == NULL) {
        perror("Unable to open directory");
        return -1; // Indicate an error
    }

    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, "index.html") == 0) {
            closedir(directory);
            return 1; // File found
        }
    }

    closedir(directory);
    return 0; // File not found
}



