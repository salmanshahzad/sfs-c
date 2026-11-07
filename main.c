#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum status : unsigned short {
    STATUS_BAD_REQUEST = 400,
    STATUS_INTERNAL_SERVER_ERROR = 500,
    STATUS_METHOD_NOT_ALLOWED = 405,
    STATUS_NOT_FOUND = 404,
    STATUS_OK = 200,
    STATUS_URI_TOO_LONG = 414,
};

struct response {
    enum status status;
    int body_fd;
    off_t content_length;
    const char *content_type;
};

constexpr unsigned short BUFFER_SIZE = 4096;
int dir_fd = -1;

static void decode_path(char *decoded, size_t decoded_len, const char *encoded) {
    const auto len = strlen(encoded);
    size_t i = 0;
    size_t j = 0;
    while (i < len && j < decoded_len - 1) {
        if (encoded[i] == '%' && i + 2 < len) {
            const char hex[] = { encoded[i + 1], encoded[i + 2], '\0' };
            char *end;
            const auto value = (char)strtol(hex, &end, 16);
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

static const char *get_mime_type(const char *file) {
    const auto ext = strrchr(file, '.');
    if (ext == nullptr) return "application/octet-stream";
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

static void trim(char **str, char s) {
    const auto len = strlen(*str);
    if (len == 0) return;
    auto start = *str;
    while (*start == s) start++;
    *str = start;
    auto end = *str + len - 1;
    while (*end == s) end--;
    *(end + 1) = '\0';
}

[[nodiscard]]
static bool write_all(int fd, const void *data, size_t nbytes) {
    size_t total_written = 0;
    while (total_written < nbytes) {
        const auto bytes_written = write(
            fd,
            (const char*)data + total_written,
            nbytes - total_written
        );
        if (bytes_written == -1) {
            perror("write");
            return false;
        }
        total_written += (size_t)bytes_written;
    }
    return true;
}

[[nodiscard]]
static bool send_file(int from_fd, int to_fd, off_t nbytes) {
    off_t total_written = 0;
    while (total_written < nbytes) {
#ifdef __APPLE__
        off_t bytes_written = 0;
        if (sendfile(from_fd, to_fd, total_written, &bytes_written, nullptr, 0) == -1) {
            perror("sendfile");
            return false;
        }
        total_written += bytes_written;
#else
        const auto bytes_written = sendfile(
            to_fd,
            from_fd,
            &total_written,
            nbytes - total_written
        );
        if (bytes_written == -1) {
            perror("sendfile");
            return false;
        }
#endif
    }
    return true;
}

static bool send_response(int fd, const struct response *res) {
    const char *status_text = "";
    switch (res->status) {
        case STATUS_BAD_REQUEST: status_text = "Bad Request"; break;
        case STATUS_INTERNAL_SERVER_ERROR: status_text = "Internal Server Error"; break;
        case STATUS_METHOD_NOT_ALLOWED: status_text = "Method Not Allowed"; break;
        case STATUS_NOT_FOUND: status_text = "Not Found"; break;
        case STATUS_OK: status_text = "OK"; break;
        case STATUS_URI_TOO_LONG: status_text = "URI Too Long"; break;
        default: unreachable();
    }

    constexpr unsigned char HTTP_DATE_LEN = 30;
    char date[HTTP_DATE_LEN];
    const auto now = time(nullptr);
    const auto time_info = gmtime(&now);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", time_info);

    char headers[BUFFER_SIZE];
    const auto allow_header = res->status == STATUS_METHOD_NOT_ALLOWED
        ? "Allow: GET, HEAD\r\n"
        : "";
    char content_type_header[BUFFER_SIZE];
    if (res->content_type == nullptr) {
        content_type_header[0] = '\0';
    } else {
        snprintf(
            content_type_header,
            sizeof(content_type_header),
            "Content-Type: %s\r\n",
            res->content_type
        );
    }
    const auto headers_len = snprintf(
        headers,
        sizeof(headers),
        "HTTP/1.1 %d %s\r\n%sConnection: close\r\nContent-Length: %lld\r\n%sDate: %s\r\nServer: sfs\r\n\r\n",
        res->status,
        status_text,
        allow_header,
        res->content_length,
        content_type_header,
        date
    );
    return write_all(fd, headers, (size_t)headers_len) &&
        (res->body_fd == -1 || send_file(res->body_fd, fd, res->content_length));
}

static bool send_status(int fd, enum status status) {
    const struct response res = {
        .body_fd = -1,
        .content_length = 0,
        .content_type = nullptr,
        .status = status,
    };
    return send_response(fd, &res);
}

static void *handle_request(void *_client_fd) {
    const auto client_fd = *(int*)_client_fd;
    int fd = -1;

    char req[BUFFER_SIZE];
    const auto bytes_read = read(client_fd, req, sizeof(req));
    if (bytes_read == -1) {
        perror("read");
        send_status(client_fd, STATUS_INTERNAL_SERVER_ERROR);
        goto close;
    }

    if (bytes_read == sizeof(req)) {
        send_status(client_fd, STATUS_URI_TOO_LONG);
        goto close;
    }
    req[bytes_read] = '\0';

    char *ctx;
    const auto method = strtok_r(req, " ", &ctx);
    if (method == nullptr) {
        send_status(client_fd, STATUS_BAD_REQUEST);
        goto close;
    }

    const auto is_method_get = strcmp(method, "GET") == 0;
    const auto is_method_head = strcmp(method, "HEAD") == 0;
    if (!is_method_get && !is_method_head) {
        send_status(client_fd, STATUS_METHOD_NOT_ALLOWED);
        goto close;
    }

    auto resource = strtok_r(nullptr, " ", &ctx);
    if (resource == nullptr) {
        send_status(client_fd, STATUS_BAD_REQUEST);
        goto close;
    }

    trim(&resource, '/');
    const auto filename = *resource == '\0' ? "index.html" : resource;
    char path[BUFFER_SIZE];
    decode_path(path, sizeof(path), filename);

    if (strstr(path, "..") != nullptr) {
        send_status(client_fd, STATUS_NOT_FOUND);
        goto close;
    }

    fd = openat(dir_fd, path, O_RDONLY);
    if (fd == -1) {
        send_status(client_fd, STATUS_NOT_FOUND);
        goto close;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("stat");
        send_status(client_fd, STATUS_INTERNAL_SERVER_ERROR);
        goto close;
    }

    if (!S_ISREG(st.st_mode)) {
        send_status(client_fd, STATUS_NOT_FOUND);
        goto close;
    }

    const struct response res = {
        .body_fd = is_method_get ? fd : -1,
        .content_length = st.st_size,
        .content_type = get_mime_type(path),
        .status = STATUS_OK,
    };
    send_response(client_fd, &res);

close:
    if (fd != -1 && close(fd) == -1) {
        perror("close");
    }
    if (close(client_fd) == -1) {
        perror("close");
    }
    free(_client_fd);
    return nullptr;
}

int main(int argc, char **argv) {
    const char *directory = ".";
    const char *host = "127.0.0.1";
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

    dir_fd = open(directory, O_DIRECTORY | O_RDONLY);
    if (dir_fd == -1) {
        fputs("Invalid directory\n", stderr);
        return EXIT_FAILURE;
    }

    constexpr unsigned short DEFAULT_PORT = 1024;
    const auto port_num = port ? atoi(port) : DEFAULT_PORT;
    if (port_num < 0 || port_num > 65535) {
        fputs("Invalid port\n", stderr);
        return EXIT_FAILURE;
    }

    const auto server_fd = socket(PF_INET, SOCK_STREAM, 0);
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

    const auto reuse_addr = 1;
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

    pthread_attr_t attr;
    const auto attr_init_err = pthread_attr_init(&attr);
    if (attr_init_err != 0) {
        fprintf(stderr, "pthread_attr_init: %s\n", strerror(attr_init_err));
        return EXIT_FAILURE;
    }

    const auto detach_err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (detach_err != 0) {
        fprintf(stderr, "pthread_attr_setdetachstate: %s\n", strerror(detach_err));
        return EXIT_FAILURE;
    }

    while (true) {
        struct sockaddr client_addr;
        socklen_t client_addr_size = sizeof(struct sockaddr);
        const auto client_fd = accept(server_fd, &client_addr, &client_addr_size);
        if (client_fd == -1) {
            perror("accept");
            continue;
        }

        auto cfd = malloc(sizeof(int));
        if (cfd == nullptr) {
            perror("malloc");
            goto error;
        }

        pthread_t thread;
        const auto thread_err = pthread_create(&thread, &attr, handle_request, cfd);
        if (thread_err != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(thread_err));
            goto error;
        } else {
            continue;
        }

error:
        send_status(client_fd, STATUS_INTERNAL_SERVER_ERROR);
        if (close(client_fd) == -1) {
            perror("close");
        }
        if (cfd != nullptr) {
            free(cfd);
        }
    }

    const auto destroy_err = pthread_attr_destroy(&attr);
    if (destroy_err != 0) {
        fprintf(stderr, "pthread_attr_destroy: %s\n", strerror(destroy_err));
    }
    return EXIT_SUCCESS;
}
