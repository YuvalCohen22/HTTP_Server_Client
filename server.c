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
#include <tgmath.h>
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
int is_index_html_in_directory(char *directory_path);
char* create_response(char* status, int status_code, char* method, char* path);
char* get_response_body(int status_code, char* path, size_t* bytes_read);
bool is_directory(char* path);

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
        if (client_sock == NULL) {
            perror("malloc");
            exit(1);
        }
        (*client_sock) = accept(server_sock, (struct sockaddr *)&cli, &client_len);
        if ((*client_sock) < 0) {
            perror("accept");
            free(client_sock);
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
        return 0;
    }
    if (check_req == 501) {
        send_response(*client_sock, "501 Not Implemented", NULL, NULL);
        return 0;
    }

    int checked_path = check_path(path);
    DEBUG_PRINT("checked_path: %d\n", checked_path);

    if (checked_path == 404) {
        send_response(*client_sock, "404 Not Found", NULL, NULL);
        return 0;
    }

    if (checked_path == 302) {
        send_response(*client_sock, "302 Found", NULL, NULL);
        return 0;
    }

    if (checked_path == 403) {
        send_response(*client_sock, "403 Forbidden", NULL, NULL);
        return 0;
    }

    if (checked_path == 200) {
        send_response(*client_sock, "200 OK", NULL, NULL);
        return 0;
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
            return 500;
        if (check_index_html == 1) {
            strcat(path, "index.html");
            if (!(stat_buf.st_mode & S_IXOTH) || !(stat_buf.st_mode & S_IXGRP) || !(stat_buf.st_mode & S_IXUSR))
                return 403;
            if (check_permission(path))
                return 200;
            return 403;
        }
        if (check_permission(path))
            return 200;
        return 403;
    }

    if (S_ISREG(stat_buf.st_mode)) {

        if (!(stat_buf.st_mode & S_IXOTH) || !(stat_buf.st_mode & S_IXGRP) || !(stat_buf.st_mode & S_IXUSR)) {
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

    *path = found_path;

    return 0;
}

void send_response(int client_sock, const char *status, const char *body, const char *content_type) {


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

char* create_response(char* status, int status_code, char* method, char* path) {
    size_t content_length = 0;
    char* body = get_response_body(status_code, path, &content_length);
    char time_buffer[128];
    time_t now = time(NULL);
    strftime(time_buffer, sizeof(time_buffer), RFC1123FMT, gmtime(&now));

    const char* content_type;
    if (status_code != 200) {
        content_type = "text/html";
    } else {
        content_type = is_directory(path) ? "text/html" : get_mime_type(path);
    }

    // Prepare a Location header if status code is 302
    char location_header[512] = "";
    if (status_code == 302) {
        snprintf(location_header, sizeof(location_header), "Location: %s/\r\n", path);
    }

    // Calculate total response size
    size_t response_size = snprintf(
        NULL, 0,
        "HTTP/1.0 %s\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "%s" // Location header
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, time_buffer, location_header, content_type ? content_type : "text/plain", content_length);

    // Allocate memory for the response
    char* response = malloc(response_size + 1);
    if (!response) {
        perror("malloc");
        free(body);
        return NULL;
    }

    // Build the response string
    snprintf(
        response, response_size + 1,
        "HTTP/1.0 %s\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "%s" // Location header
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, time_buffer, location_header, content_type ? content_type : "text/plain", content_length);
    return response;
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
    return stat(path+1, stat_buf) == 0;
}

bool check_permission(char *path) {
    size_t path_size = strlen(path) + 1;
    char path_copy[path_size];
    strncpy(path_copy, path, path_size);
    path_copy[path_size - 1] = '\0'; // Ensure null termination

    DEBUG_PRINT("path_copy: %s\n", path_copy);

    char* current_path = path_copy;
    char* directory = NULL;
    struct stat dir_buf;

    do {
        directory = dirname(current_path);
        DEBUG_PRINT("directory: %s\n", directory);

        if (stat(directory, &dir_buf) != 0 ||
            !(dir_buf.st_mode & S_IXOTH) ||
            !(dir_buf.st_mode & S_IXGRP) ||
            !(dir_buf.st_mode & S_IXUSR)) {
            return false;
            }

        // Prepare for next iteration, ensuring not to pass NULL to dirname
        if (strcmp(directory, "/") != 0) {
            current_path = directory;
        }
    } while (strcmp(directory, "/") != 0);

    // Final check for "/"
    if (stat("/", &dir_buf) != 0 ||
        !(dir_buf.st_mode & S_IXOTH) ||
        !(dir_buf.st_mode & S_IXGRP) ||
        !(dir_buf.st_mode & S_IXUSR)) {
        return false;
        }
    DEBUG_PRINT("permission check passed\n");
    return true;

}

int is_index_html_in_directory(char *directory_path) {
    char* copied_dir = dirname(directory_path);
    struct dirent *entry;
    DIR *directory = opendir(copied_dir);

    DEBUG_PRINT("directory_path: %s\n", copied_dir);

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

char* get_response_body(int status_code, char* path, size_t* bytes_read) {
    if (status_code == 200 && is_directory(path)) {

    }
    else if (status_code == 200) {
        FILE* file = fopen(path, "rb"); // Open the file in binary mode
        if (!file) {
            perror("Failed to open file");
            *bytes_read = 0;
            return NULL;
        }

        // Seek to the end of the file to get its size
        if (fseek(file, 0, SEEK_END) != 0) {
            perror("Failed to seek in file");
            fclose(file);
            *bytes_read = 0;
            return NULL;
        }

        long file_size = ftell(file);
        if (file_size == -1) {
            perror("Failed to get file size");
            fclose(file);
            *bytes_read = 0;
            return NULL;
        }

        rewind(file); // Go back to the beginning of the file

        // Allocate memory for the file content
        char* buffer = (char*)malloc(file_size + 1); // +1 for null-terminator
        if (!buffer) {
            perror("Failed to allocate memory");
            fclose(file);
            *bytes_read = 0;
            return NULL;
        }

        // Read the file content into the buffer
        *bytes_read = fread(buffer, 1, file_size, file);
        if (*bytes_read != file_size) {
            if (ferror(file)) {
                perror("Failed to read file");
                free(buffer);
                fclose(file);
                *bytes_read = 0;
                return NULL;
            }
        }

        buffer[*bytes_read] = '\0'; // Null-terminate the buffer

        fclose(file);
        return buffer;
    }
    else {
        if (status_code == 302) {

        }
        if (status_code == 400) {

        }
        if (status_code == 404) {

        }
        if (status_code == 500) {

        }
        if (status_code == 501) {

        }
    }
    return NULL;

}

bool is_directory(char* path) {
    return path[strlen(path) - 1] == '/';
}


