#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG 256
#define BUFFER_SIZE 1024

#define INTERNAL_SERVER_ERROR "HTTP/1.1 500 Internal Server Error\r\n\r\n"
#define METHOD_NOT_ALLOWED "HTTP/1.1 405 Method Not Allowed\r\n\r\n"
#define NOT_FOUND "HTTP/1.1 404 Not Found\r\n\r\n"

void trim(char **str, char s) {
    char *end = *str + strlen(*str) - 1;
    while (*end == s) end--;
    *(end + 1) = '\0';
    char *start = *str;
    while (*start == s) start++;
    *str = start;
}

void join_path(char *dst, const char *dir, const char *file) {
    const size_t dir_len = strlen(dir);
    const size_t file_len = strlen(file);
    const size_t path_len = dir_len + file_len + 1;

    memcpy(dst, dir, dir_len);
    dst[dir_len] = '/';
    memcpy(dst + dir_len + 1, file, file_len);
    dst[path_len] = '\0';
}

void write_str(int fd, const char *str) {
    if (write(fd, str, strlen(str)) == -1) {
        perror("write");
    }
}

int main(int argc, char **argv) {
    char *directory = ".";
    const char *host = "127.0.0.1";
    unsigned short port = 1024;

    char c;
    while ((c = getopt(argc, argv, "d:h:p:")) != -1) {
        switch (c) {
            case 'd':
                directory = optarg;
                trim(&directory, '/');
                break;
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
        }
    }

    const int socket_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_aton(host, &addr.sin_addr);

    if (bind(socket_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(socket_fd, BACKLOG) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    struct sockaddr client_addr;
    socklen_t client_addr_size = sizeof(struct sockaddr);
    char buffer[BUFFER_SIZE];
    char path[BUFFER_SIZE];
    char res[BUFFER_SIZE];
    FILE *file = NULL;
    void *file_contents = NULL;

    for (;;) {
        const int client_fd = accept(socket_fd, &client_addr, &client_addr_size);
        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        memset(buffer, 0, BUFFER_SIZE);
        if (read(client_fd, buffer, BUFFER_SIZE) == -1) {
            perror("read");
            write_str(client_fd, INTERNAL_SERVER_ERROR);
            goto close;
        }

        const char *method = strtok(buffer, " ");
        const size_t method_len = strlen(method);
        char *resource = strtok(NULL, " ");
        trim(&resource, '/');
        if (strlen(resource) == 0) {
            resource = "index.html";
        }
        join_path(path, directory, resource);

        if (access(path, R_OK) == -1) {
            write_str(client_fd, NOT_FOUND);
            goto close;
        }

        const bool is_method_get = strncmp(method, "GET", method_len) == 0;
        const bool is_method_head = strncmp(method, "HEAD", method_len) == 0;
        if (!is_method_get && !is_method_head) {
            write_str(client_fd, METHOD_NOT_ALLOWED);
            goto close;
        }

        FILE *file = fopen(path, "r");
        if (file == NULL) {
            perror("fopen");
            write_str(client_fd, INTERNAL_SERVER_ERROR);
            goto close;
        }

        if (fseek(file, 0, SEEK_END) == -1) {
            perror("fseek");
            write_str(client_fd, INTERNAL_SERVER_ERROR);
            goto close;
        }

        const long file_size = ftell(file);
        if (file_size == -1) {
            perror("ftell");
            write_str(client_fd, INTERNAL_SERVER_ERROR);
            goto close;
        }

        file_contents = malloc(file_size);
        if (file_contents == NULL) {
            perror("malloc");
            write_str(client_fd, INTERNAL_SERVER_ERROR);
            goto close;
        }

        sprintf(res, "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", file_size);
        if (is_method_head) {
            write_str(client_fd, res);
        } else if (is_method_get) {
            if (fseek(file, 0, SEEK_SET) == -1) {
                perror("fseek");
                write_str(client_fd, INTERNAL_SERVER_ERROR);
                goto close;
            }
            if (fread(file_contents, 1, file_size, file) != file_size) {
                perror("fread");
                write_str(client_fd, INTERNAL_SERVER_ERROR);
                goto close;
            }
            write_str(client_fd, res);
            if (write(client_fd, file_contents, file_size) == -1) {
                perror("write");
            }
        }

close:
        if (file != NULL) {
            fclose(file);
            file = NULL;
        }
        if (file_contents != NULL) {
            free(file_contents);
            file_contents = NULL;
        }
        if (close(client_fd) == -1) {
            perror("close");
        }
    }

    return EXIT_SUCCESS;
}
