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
void send_response(int client_sock, char* status, int status_code, char* path);
char *get_mime_type(char *name);
bool does_file_exist(char *path, struct stat *stat_buf);
bool check_permission(char *path);
int is_index_html_in_directory(char *directory_path);
char* create_response(char* status, int status_code, char* path, char* body, size_t body_size, size_t* total_size);
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
        send_response(*client_sock, "400 Bad Request", 400, NULL);
        return -1;
    }
    end_of_first_line[0] = '\0';
    DEBUG_PRINT("%s\n", request);
    char* path;

    int check_req = check_bad_request(request, &path);

    if (check_req== 400) {
        send_response(*client_sock, "400 Bad Request", 400, path);
        return 0;
    }
    if (check_req == 501) {
        send_response(*client_sock, "501 Not Implemented", 501, path);
        return 0;
    }

    DEBUG_PRINT("path: %s\n", path);
    int checked_path = check_path(path);

    if (checked_path == 404) {
        send_response(*client_sock, "404 Not Found", 404, path);
        return 0;
    }

    if (checked_path == 302) {
        send_response(*client_sock, "302 Found", 302, path);
        return 0;
    }

    if (checked_path == 403) {
        send_response(*client_sock, "403 Forbidden", 403, path);
        return 0;
    }

    if (checked_path == 200) {
        send_response(*client_sock, "200 OK", 200, path);
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

void send_response(int client_sock, char* status, int status_code, char* path) {
    size_t body_size;
    size_t total_size;
    char* body = get_response_body(status_code, path, &body_size);
    char* response = create_response(status, status_code, path, body, body_size, &total_size);
    // for (int i =0; i < total_size; i++) {
    //     DEBUG_PRINT("%c", response[i]);
    // }
    DEBUG_PRINT("%d\n", (int)total_size);
    send(client_sock, response, total_size, 0);
    free(body);
    free(response);
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

char* create_response(char* status, int status_code, char* path, char* body, size_t body_size, size_t* total_size) {
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
        status, time_buffer, location_header, content_type ? content_type : "text/plain", body_size);

    // Allocate memory for the response
    char* response = malloc(response_size + body_size + 1);
    if (!response) {
        perror("malloc");
        free(body);
        return NULL;
    }
    //DEBUG_PRINT("body: %s\n", body);

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
        status, time_buffer, location_header, content_type ? content_type : "text/plain", body_size);
    memcpy(response+response_size, body, body_size);
    *total_size = response_size + body_size + 1;
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


    char* current_path = path_copy;
    char* directory = NULL;
    struct stat dir_buf;

    do {
        directory = dirname(current_path);

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
    char* body;
    DEBUG_PRINT("%d %s\n", status_code, path);
    if (status_code == 200 && is_directory(path)) {
    }
    else if (status_code == 200) {
        FILE* file = fopen(path+1, "rb"); // Open the file in binary mode
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
        body = (char*)malloc(file_size + 1); // +1 for null-terminator
        if (!body) {
            perror("Failed to allocate memory");
            fclose(file);
            *bytes_read = 0;
            return NULL;
        }

        // Read the file content into the buffer
        *bytes_read = fread(body, 1, file_size, file);
        if (*bytes_read != file_size) {
            if (ferror(file)) {
                perror("Failed to read file");
                free(body);
                fclose(file);
                *bytes_read = 0;
                return NULL;
            }
        }

        body[*bytes_read] = '\0'; // Null-terminate the buffer

        fclose(file);
        return body;
    }
    else {
        if (status_code == 302) {
            body = (char*)malloc(125);
            snprintf(
                body, 125,
                "<HTML><HEAD><TITLE>302 Found</TITLE></HEAD>\r\n"
                    "<BODY><H4>302 Found</H4>\r\n"
                    "Directories must end with a slash.\r\n"
                    "</BODY></HTML>\r\n");
            *bytes_read = 125;
        }
        if (status_code == 400) {
            body = (char*)malloc(114);
            snprintf(
                body, 114,
                "<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\r\n"
                    "<BODY><H4>400 Bad request</H4>\r\n"
                    "Bad Request.\r\n"
                    "</BODY></HTML>\r\n");
            *bytes_read = 114;
        }
        if (status_code == 403) {
            DEBUG_PRINT("no permision");
            body = (char*)malloc(113);
            snprintf(
                body, 113,
                "<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\r\n"
                    "<BODY><H4>403 Forbidden</H4>\r\n"
                    "Access denied.\r\n"
                    "</BODY></HTML>\r\n");
            *bytes_read = 113;
        }
        if (status_code == 404) {
            body = (char*)malloc(113);
            snprintf(
                body, 113,
                "<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\r\n"
                    "<BODY><H4>404 Not Found</H4>\r\n"
                    "File not found.\r\n"
                    "</BODY></HTML>\r\n");
            *bytes_read = 113;
        }
        if (status_code == 500) {
            body = (char*)malloc(145);
            snprintf(
                body, 145,
                "<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\r\n"
                    "<BODY><H4>500 Internal Server Error</H4>\r\n"
                    "Some server side error.\r\n"
                    "</BODY></HTML>\r\n");
            *bytes_read = 145;
        }
        if (status_code == 501) {
            body = (char*)malloc(130);
            snprintf(
                body, 130,
                "<HTML><HEAD><TITLE>501 Not supported</TITLE></HEAD>\r\n"
                    "<BODY><H4>501 Not supported</H4>\r\n"
                    "Method is not supported.\r\n"
                    "</BODY></HTML>\r\n");
            *bytes_read = 130;
        }
        return body;
    }
    return NULL;

}

bool is_directory(char* path) {
    return path[strlen(path) - 1] == '/';
}


