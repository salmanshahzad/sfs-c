#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define DEFAULT_PORT 1024
#define HTTP_DATE_LEN 30
#define LOCALHOST "127.0.0.1"

#define STATUS_BAD_REQUEST 400
#define STATUS_INTERNAL_SERVER_ERROR 500
#define STATUS_METHOD_NOT_ALLOWED 405
#define STATUS_NOT_FOUND 404
#define STATUS_OK 200
#define STATUS_URI_TOO_LONG 414

struct req_thread_args {
    int client_fd;
    int dir_fd;
};

struct response {
    int status;
    int body_fd;
    size_t content_length;
    const char *content_type;
};

void decode_path(char *decoded, size_t decoded_len, const char *encoded) {
    const size_t len = strlen(encoded);
    size_t i = 0;
    size_t j = 0;
    while (i < len && j < decoded_len - 1) {
        if (encoded[i] == '%' && i + 2 < len) {
            const char hex[] = { encoded[i + 1], encoded[i + 2], '\0' };
            char *end;
            const long value = strtol(hex, &end, 16);
            if (*end == '\0' && value >= 0x20 && value <= 0x7F) {
                decoded[j++] = value;
                i += 3;
            } else {
                decoded[j++] = encoded[i++];
            }
        } else if (encoded[i] == '?' || encoded[i] == '#') {
            break;
        } else {
            decoded[j++] = encoded[i++];
        }
    }
    decoded[j] = '\0';
}

const char *get_mime_type(const char *file) {
    const char *ext = strrchr(file, '.');
    if (ext == NULL) return "application/octet-stream";
    if (strcmp(ext, ".htm") == 0 || strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".xml") == 0) return "application/xml";
    if (strcmp(ext, ".csv") == 0) return "text/csv";
    if (strcmp(ext, ".tsv") == 0) return "text/tsv";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".webp") == 0) return "image/webp";
    if (strcmp(ext, ".bmp") == 0) return "image/bmp";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".pdf") == 0) return "application/pdf";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    if (strcmp(ext, ".md") == 0) return "text/markdown";
    if (strcmp(ext, ".mp3") == 0) return "audio/mpeg";
    if (strcmp(ext, ".wav") == 0) return "audio/wav";
    if (strcmp(ext, ".ogg") == 0) return "audio/ogg";
    if (strcmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcmp(ext, ".webm") == 0) return "video/webm";
    if (strcmp(ext, ".mov") == 0) return "video/quicktime";
    if (strcmp(ext, ".zip") == 0) return "application/zip";
    if (strcmp(ext, ".tar") == 0) return "application/x-tar";
    if (strcmp(ext, ".gz") == 0) return "application/gzip";
    if (strcmp(ext, ".rar") == 0) return "application/vnd.rar";
    if (strcmp(ext, ".7z") == 0) return "application/x-7z-compressed";
    return "application/octet-stream";
}

void trim(char **str, char s) {
    const size_t len = strlen(*str);
    if (len == 0) return;
    char *start = *str;
    while (*start == s) start++;
    *str = start;
    char *end = *str + len - 1;
    while (*end == s) end--;
    *(end + 1) = '\0';
}

bool write_all(int fd, const void *data, size_t nbytes) {
    size_t total_written = 0;
    while (total_written < nbytes) {
        const ssize_t bytes_written = write(
            fd,
            (char*)data + total_written,
            nbytes - total_written
        );
        if (bytes_written == -1) {
            perror("write");
            return false;
        }
        total_written += bytes_written;
    }
    return true;
}

bool send_file(int from_fd, int to_fd, off_t nbytes) {
    off_t total_written = 0;
    while (total_written < nbytes) {
        off_t bytes_written = 0;
        if (sendfile(from_fd, to_fd, total_written, &bytes_written, NULL, 0) == -1) {
            perror("sendfile");
            return false;
        }
        total_written += bytes_written;
    }
    return true;
}

bool send_response(int fd, const struct response *res) {
    const char *status_text = "";
    switch (res->status) {
        case STATUS_BAD_REQUEST: status_text = "Bad Request"; break;
        case STATUS_INTERNAL_SERVER_ERROR: status_text = "Internal Server Error"; break;
        case STATUS_METHOD_NOT_ALLOWED: status_text = "Method Not Allowed"; break;
        case STATUS_NOT_FOUND: status_text = "Not Found"; break;
        case STATUS_OK: status_text = "OK"; break;
        case STATUS_URI_TOO_LONG: status_text = "URI Too Long"; break;
    }

    char date[HTTP_DATE_LEN];
    const time_t now = time(NULL);
    const struct tm *time_info = gmtime(&now);
    strftime(date, HTTP_DATE_LEN, "%a, %d %b %Y %H:%M:%S GMT", time_info);

    char headers[BUFFER_SIZE];
    const char *allow_header = res->status == STATUS_METHOD_NOT_ALLOWED
        ? "Allow: GET, HEAD\r\n"
        : "";
    char *content_type_header = "";
    if (res->content_type != NULL) {
        char header[BUFFER_SIZE];
        content_type_header = header;
        snprintf(content_type_header, sizeof(header), "Content-Type: %s\r\n", res->content_type);
    }
    const size_t headers_len = snprintf(
        headers,
        sizeof(headers),
        "HTTP/1.1 %d %s\r\n%sConnection: close\r\nContent-Length: %lu\r\n%sDate: %s\r\nServer: sfs\r\n\r\n",
        res->status,
        status_text,
        allow_header,
        res->content_length,
        content_type_header,
        date
    );
    return write_all(fd, headers, headers_len) &&
        (res->body_fd == -1 || send_file(res->body_fd, fd, res->content_length));
}

void *handle_request(void *_args) {
    struct req_thread_args *args = (struct req_thread_args*)_args;
    struct response res = {0};
    int fd = -1;

    char req[BUFFER_SIZE];
    const ssize_t bytes_read = read(args->client_fd, req, sizeof(req));
    if (bytes_read == -1) {
        perror("read");
        res.status = STATUS_INTERNAL_SERVER_ERROR;
        send_response(args->client_fd, &res);
        goto close;
    }

    if (bytes_read == sizeof(req)) {
        res.status = STATUS_URI_TOO_LONG;
        send_response(args->client_fd, &res);
        goto close;
    }
    req[bytes_read] = '\0';

    const char *method = strtok(req, " ");
    if (method == NULL) {
        res.status = STATUS_BAD_REQUEST;
        send_response(args->client_fd, &res);
        goto close;
    }

    const bool is_method_get = strcmp(method, "GET") == 0;
    const bool is_method_head = strcmp(method, "HEAD") == 0;
    if (!is_method_get && !is_method_head) {
        res.status = STATUS_METHOD_NOT_ALLOWED;
        send_response(args->client_fd, &res);
        goto close;
    }

    char *resource = strtok(NULL, " ");
    if (resource == NULL) {
        res.status = STATUS_BAD_REQUEST;
        send_response(args->client_fd, &res);
        goto close;
    }

    trim(&resource, '/');
    if (*resource == '\0') {
        resource = "index.html";
    }
    char path[BUFFER_SIZE];
    decode_path(path, sizeof(path), resource);

    if (strnstr(path, "..", strlen(path)) != NULL) {
        res.status = STATUS_NOT_FOUND;
        send_response(args->client_fd, &res);
        goto close;
    }

    fd = openat(args->dir_fd, path, O_RDONLY);
    if (fd == -1) {
        res.status = STATUS_NOT_FOUND;
        send_response(args->client_fd, &res);
        goto close;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("stat");
        res.status = STATUS_INTERNAL_SERVER_ERROR;
        send_response(args->client_fd, &res);
        goto close;
    }

    if (!S_ISREG(st.st_mode)) {
        res.status = STATUS_NOT_FOUND;
        send_response(args->client_fd, &res);
        goto close;
    }

    res.content_length = st.st_size;
    res.content_type = get_mime_type(path);
    res.body_fd = is_method_get ? fd : -1;
    res.status = STATUS_OK;
    send_response(args->client_fd, &res);

close:
    if (fd != -1 && close(fd) == -1) {
        perror("close");
    }
    if (close(args->client_fd) == -1) {
        perror("close");
    }
    free(args);
    return NULL;
}

int main(int argc, char **argv) {
    const char *directory = ".";
    const char *host = LOCALHOST;
    const char *port = getenv("PORT");

    int c;
    while ((c = getopt(argc, argv, "d:h:p:")) != -1) {
        switch (c) {
            case 'd':
                directory = optarg;
                break;
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = optarg;
                break;
        }
    }

    const int dir_fd = open(directory, O_DIRECTORY | O_RDONLY);
    if (dir_fd == -1) {
        fputs("Invalid directory\n", stderr);
        return EXIT_FAILURE;
    }

    const int port_num = port ? atoi(port) : DEFAULT_PORT;
    if (port_num < 0 || port_num > 65535) {
        fputs("Invalid port\n", stderr);
        return EXIT_FAILURE;
    }

    const int server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) == 0) {
        fputs("Invalid host\n", stderr);
        return EXIT_FAILURE;
    }

    const int reuse_addr = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) == -1) {
        perror("setsockopt");
    }

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in)) == -1) {
        perror("bind");
        return EXIT_FAILURE;
    }

    if (listen(server_fd, SOMAXCONN) == -1) {
        perror("listen");
        return EXIT_FAILURE;
    }

    while (true) {
        struct sockaddr client_addr;
        socklen_t client_addr_size = sizeof(struct sockaddr);
        const int client_fd = accept(server_fd, &client_addr, &client_addr_size);
        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        struct req_thread_args *args =
            (struct req_thread_args*)malloc(sizeof(struct req_thread_args));
        if (args == NULL) {
            perror("malloc");
            continue;
        }
        args->client_fd = client_fd;
        args->dir_fd = dir_fd;

        pthread_t thread;
        const int err = pthread_create(&thread, NULL, handle_request, args);
        if (err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(err));
            if (close(client_fd) == -1) {
                perror("close");
            }
        } else {
            const int err = pthread_detach(thread);
            if (err != 0) {
                fprintf(stderr, "pthread_detach: %s\n", strerror(err));
                if (close(client_fd) == -1) {
                    perror("close");
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
