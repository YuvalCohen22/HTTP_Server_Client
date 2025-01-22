#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
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

void handle_client(const int *client_sock);
int check_bad_request(const char *request, char **path);
bool isValidHttpVersion(const char *version);
int check_path(char *path);
void send_response(int client_sock, char* status, int status_code, char* path);
char *get_mime_type(const char *name);
bool does_file_exist(const char *path, struct stat *stat_buf);
bool check_permission(const char *path);
int is_index_html_in_directory(char *directory_path);
char* create_response(char* status, int status_code, char* path, char* body, size_t body_size, size_t* total_size);
char* get_response_body(int status_code, char* path, size_t* bytes_read);
bool is_directory(const char* path);
int send_file_to_socket(const char *file_path, size_t file_size, int socket_fd);

int main(int argc, char *argv[]) {

    if (argc != 5) {
        printf("Usage: server <port> <pool-size> <max-queue-size> <max-number-of-request>\n");
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
        exit(1);
    }

    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = htonl(INADDR_ANY);
    srv.sin_port = htons(port);

    if(bind(server_sock, (struct sockaddr*) &srv, sizeof(srv)) < 0) {
        perror("bind");
        exit(1);
    }

    if(listen(server_sock, 5) < 0) {
        perror("listen");
        exit(1);
    }

    int counter = 0;

    //struct _threadpool_st* threadpool_st = create_threadpool(pool_size, max_queue_size);

    while (counter++ < num_of_request) {
        int* client_sock = malloc(sizeof(int));
        if (client_sock == NULL) {
            perror("malloc");
            exit(1);
        }
        *client_sock = accept(server_sock, (struct sockaddr *)&cli, &client_len);
        if (*client_sock < 0) {
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

// handle given request.
void handle_client(const int *client_sock) {
    char request[MAX_FIRST_LINE];
    ssize_t bytes_read = read(*client_sock, request, MAX_FIRST_LINE);

    if (bytes_read <= 0) {
        send_response(*client_sock, "500 Internal Server Error", 500, NULL);
        perror("read");
        return;
    }

    char* end_of_first_line = strstr(request, "\r\n");
    if (end_of_first_line == NULL) {
        send_response(*client_sock, "400 Bad Request", 400, NULL);
        return;
    }
    end_of_first_line[0] = '\0';
    DEBUG_PRINT("%s\n", request);
    char* path;

    const int check_req = check_bad_request(request, &path);
    DEBUG_PRINT("PATH: %s\n", path);

    if (check_req== 400) {
        send_response(*client_sock, "400 Bad Request", 400, path);
        return;
    }
    if (check_req == 501) {
        send_response(*client_sock, "501 Not Implemented", 501, path);
        return;
    }

    const int checked_path = check_path(path);

    if (checked_path == 404) {
        send_response(*client_sock, "404 Not Found", 404, path);
        return;
    }

    if (checked_path == 302) {
        send_response(*client_sock, "302 Found", 302, path);
        return;
    }

    if (checked_path == 403) {
        send_response(*client_sock, "403 Forbidden", 403, path);
        return;
    }

    if (checked_path == 200) {
        send_response(*client_sock, "200 OK", 200, path);
    }
}

// check what status code based on path
int check_path(char *path) {
    struct stat stat_buf;

    if (strlen(path) == 1 && *path == '/')
        stat(".", &stat_buf);
    else {
        if (!does_file_exist(path, &stat_buf)) {
            return 404;
        }
    }

    if (S_ISDIR(stat_buf.st_mode)) {

        if (path[strlen(path) - 1] != '/')
            return 302;

        const int check_index_html = is_index_html_in_directory(path);

        if (check_index_html < 0)
            return 500;

        if (check_index_html == 1) {
            strcat(path, "index.html");
            stat(path+1, &stat_buf);
            if (!(stat_buf.st_mode & S_IROTH))
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

        if (!(stat_buf.st_mode & S_IROTH) || !check_permission(path))
            return 403;

        return 200;
    }

    return 404;
}

// check if request is a bad request. return 400 on bad request, 501 on not GET method and 0 if good.
int check_bad_request(const char *request, char **path) {
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

// send response to client
void send_response(const int client_sock, char* status, const int status_code, char* path) {
    size_t body_size;
    size_t total_size;
    char* response;
    char* body = get_response_body(status_code, path, &body_size);
    if (status_code == 200 && !is_directory(path))
        response = create_response(status, status_code, path, NULL, body_size, &total_size);
    else
        response = create_response(status, status_code, path, body, body_size, &total_size);
    if (response == NULL)
        send_response(client_sock, "500 Internal Server Error", 500, NULL);
    // for (int i =0; i < total_size; i++) {
    //     DEBUG_PRINT("%c", response[i]);
    // }
    DEBUG_PRINT("%d\n", (int)total_size);
    DEBUG_PRINT("bytes: %zu\n", body_size);
    send(client_sock, response, total_size, 0);
    if (status_code == 200 && !is_directory(path)) {
        if (send_file_to_socket(path + 1, body_size, client_sock) == -1)
            send_response(client_sock, "500 Internal Server Error", 500, NULL);
    }
    else
        free(body);
    free(response);
}


// check what type is a file
char *get_mime_type(const char *name) {
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

// create and return response
char* create_response(char* status, const int status_code, char* path, char* body, size_t body_size, size_t* total_size) {
    char time_buffer[128];
    time_t now = time(NULL);
    strftime(time_buffer, sizeof(time_buffer), RFC1123FMT, gmtime(&now));

    const char* content_type;
    if (status_code != 200) {
        content_type = "text/html";
    } else {
        content_type = is_directory(path) ? "text/html" : get_mime_type(path);
    }

    char location_header[512] = "";
    if (status_code == 302) {
        snprintf(location_header, sizeof(location_header), "Location: %s/\r\n", path);
    }

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



    *total_size = response_size + 1;
    if (body != NULL)
        *total_size += body_size;

    char* response = (char*)malloc(*total_size);

    if (!response) {
        perror("malloc");
        free(body);
        return NULL;
    }

     snprintf(
        response, *total_size,
        "HTTP/1.0 %s\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "%s" // Location header
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, time_buffer, location_header, content_type ? content_type : "text/plain", body_size);

    if (body != NULL)
        memcpy(response+response_size, body, body_size);

    response[*total_size] = '\0';

    return response;
}

// check if request uses valid http version
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
bool does_file_exist(const char *path, struct stat *stat_buf) {
    return stat(path+1, stat_buf) == 0;
}

// check permission for all in path
bool check_permission(const char *path) {
    path++;
    const size_t path_size = strlen(path) + 1;
    char path_copy[path_size];
    strncpy(path_copy, path, path_size);
    path_copy[path_size - 1] = '\0';


    char* current_path = path_copy;
    char* directory = NULL;
    struct stat dir_buf;

    do {
        directory = dirname(current_path);

        if (stat(directory, &dir_buf) != 0 || !(dir_buf.st_mode & S_IXOTH))
            return false;

        if (strcmp(directory, "/") != 0) {
            current_path = directory;
        }
    } while (strcmp(directory, ".") != 0);

    DEBUG_PRINT("permission check passed\n");
    return true;

}

// check if directory has file index.html
int is_index_html_in_directory(char *directory_path) {
    char copied_path[MAX_FIRST_LINE];
    if (sprintf(copied_path, "%sindex.html", directory_path) < 0) {
        perror("sprintf");
        return -1;
    }
    struct stat file_stat;
    return does_file_exist(copied_path, &file_stat) ? 1 : 0;
}

// returns response body
char* get_response_body(int status_code, char* path, size_t* bytes_read) {
    char* body;
    DEBUG_PRINT("%d %s\n", status_code, path);
    if (status_code == 200 && is_directory(path)) {
        DIR* dir;
        struct dirent* entry;
        struct stat file_stat;
        char* html_template = "<HTML>\n<HEAD><TITLE>Index of %s</TITLE></HEAD>\n\n<BODY>\n<H4>Index of %s</H4>\n\n<table CELLSPACING=8>\n<tr><th>Name</th><th>Last Modified</th><th>Size</th></tr>\n";
        char* footer = "</table>\n<HR>\n<ADDRESS>webserver/1.0</ADDRESS>\n</BODY></HTML>\n";

        size_t body_size = 0;
        size_t template_size = snprintf(NULL, 0, html_template, path, path) + 1;

        body = (char*)malloc(template_size);
        if (!body) {
            perror("malloc");
            return NULL;
        }

        snprintf(body, template_size, html_template, path, path);
        body_size = strlen(body);
        bool is_cwd = strlen(path) == 1 && *path == '/';
        path++;
        DEBUG_PRINT("path in dir listing: %s\n", path);

        if (is_cwd)
        {
            dir = opendir(".");
            strcpy(path, "");
        }
        else
            dir = opendir(path);

        if (!dir) {
            perror("opendir");
            free(body);
            return NULL;
        }

        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            size_t filepath_len = strlen(path) + strlen(entry->d_name) + 2;
            char* filepath = malloc(filepath_len);
            if (!filepath) {
                perror("malloc");
                closedir(dir);
                free(body);
                return NULL;
            }


            snprintf(filepath, filepath_len, "%s%s", path, entry->d_name);
            if (stat(filepath, &file_stat) == -1) {
                perror("stat");
                free(filepath);
                continue;
            }

            char mod_time[20];
            strftime(mod_time, sizeof(mod_time), "%Y-%m-%d %H:%M:%S", localtime(&file_stat.st_mtime));

            char* row_template = "<tr><td><A HREF=\"%s\">%s%s</A></td><td>%s</td><td>%s</td></tr>\n";
            char size_str[32] = "";

            if (S_ISDIR(file_stat.st_mode)) {
                size_str[0] = '\0';
            } else {
                snprintf(size_str, sizeof(size_str), "%ld", file_stat.st_size);
            }

            size_t row_size = snprintf(NULL, 0, row_template, entry->d_name, entry->d_name, S_ISDIR(file_stat.st_mode) ? "/" : "", mod_time, size_str) + 1;
            body = realloc(body, body_size + row_size);
            if (!body) {
                perror("realloc");
                free(filepath);
                closedir(dir);
                return NULL;
            }

            snprintf(body + body_size, row_size, row_template, entry->d_name, entry->d_name, S_ISDIR(file_stat.st_mode) ? "/" : "", mod_time, size_str);
            body_size += row_size - 1;

            free(filepath);
        }

        if (is_cwd)
            strcpy(path, "/");
        closedir(dir);

        size_t footer_size = strlen(footer);
        body = realloc(body, body_size + footer_size + 1);
        if (!body) {
            perror("realloc");
            return NULL;
        }

        strcat(body, footer);
        *bytes_read = body_size + footer_size;
    }

    else if (status_code == 200) {
        path++;
        FILE *file = fopen(path, "rb");
        if (!file) {
            perror("open file");
            *bytes_read = 0;
            return NULL;
        }

        fseek(file, 0, SEEK_END);
        *bytes_read = ftell(file);

        rewind(file);

        fclose(file);

        return "";
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
    }

    return body;
}

// check if path end in directory
bool is_directory(const char* path) {
    return path[strlen(path) - 1] == '/';
}

// send file contents to client
int send_file_to_socket(const char *path, size_t file_size, int client_socket) {
    DEBUG_PRINT("bytes: %zu\n", file_size);
    int file_descriptor = open(path, O_RDONLY);
    if (file_descriptor < 0) {
        perror("Failed to open file");
        return -1;
    }

    char buffer[4096]; // Adjust buffer size based on your needs
    ssize_t bytes_read;

    while ((bytes_read = read(file_descriptor, buffer, sizeof(buffer))) > 0) {
        ssize_t total_written = 0;
        while (total_written < bytes_read) {
            ssize_t bytes_written = write(client_socket, buffer + total_written, bytes_read - total_written);
            if (bytes_written < 0) {
                perror("Failed to send data to socket");
                close(file_descriptor);
                return -1;
            }
            total_written += bytes_written;
        }
        DEBUG_PRINT("bytes read and sent: %zd\n", bytes_read);
    }

    if (bytes_read < 0) {
        perror("Failed to read from file");
        close(file_descriptor);
        return -1;
    }

    close(file_descriptor);
    return 0;
}



