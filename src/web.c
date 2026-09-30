#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#define CROW_WEB_VERSION "0.9"
#ifndef CROW_WEB_PORT
#define CROW_WEB_PORT 8080
#endif
#define EVENT_ROOT "/root/crow_events"
#define RECORDING_ROOT "/root/crow_recordings"
#define STATUS_FILE "/root/crow_status.json"

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int respond_bytes(int fd, int code, const char *status,
                         const char *content_type,
                         const void *body, size_t body_len)
{
    char header[768];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET,HEAD,POST,OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type,Range\r\n"
        "Connection: close\r\n\r\n",
        code, status, content_type, body_len);
    if (n < 0 || (size_t)n >= sizeof(header))
        return -1;
    if (send_all(fd, header, (size_t)n) != 0)
        return -1;
    if (body_len > 0 && body)
        return send_all(fd, body, body_len);
    return 0;
}

static int respond_text(int fd, int code, const char *status,
                        const char *content_type, const char *body)
{
    return respond_bytes(fd, code, status, content_type,
                         body, body ? strlen(body) : 0);
}

static pid_t crow_pid(void)
{
    FILE *fp;
    char buf[64];
    long value;

    fp = popen("pidof crow_camera 2>/dev/null", "r");
    if (!fp)
        return -1;
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    value = strtol(buf, NULL, 10);
    return value > 0 ? (pid_t)value : -1;
}

static int count_dirs(const char *root, const char *prefix)
{
    DIR *dir = opendir(root);
    struct dirent *entry;
    int count = 0;
    size_t n = strlen(prefix);

    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, n) == 0)
            count++;
    }
    closedir(dir);
    return count;
}

static void appendf(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*used >= cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= cap - *used)
        *used = cap;
    else
        *used += (size_t)n;
}

static int valid_component(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!s || !*s)
        return 0;
    if (strstr(s, ".."))
        return 0;
    while (*p) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.' || *p == '/'))
            return 0;
        p++;
    }
    return 1;
}

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (strcmp(dot, ".h265") == 0)
        return "video/h265";
    if (strcmp(dot, ".pcm") == 0)
        return "application/octet-stream";
    if (strcmp(dot, ".json") == 0)
        return "application/json";
    if (strcmp(dot, ".txt") == 0)
        return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static const char *find_header(const char *request, const char *name)
{
    size_t name_len = strlen(name);
    const char *p = request;

    while (p && *p) {
        const char *line_end = strstr(p, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len > name_len + 1 &&
            strncasecmp(p, name, name_len) == 0 &&
            p[name_len] == ':') {
            p += name_len + 1;
            while (*p == ' ' || *p == '\t')
                p++;
            return p;
        }
        if (!line_end)
            break;
        p = line_end + 2;
    }
    return NULL;
}

static int parse_range(const char *request, off_t size,
                       off_t *start_out, off_t *end_out)
{
    const char *value = find_header(request, "Range");
    long long start = -1;
    long long end = -1;

    if (!value || strncmp(value, "bytes=", 6) != 0)
        return 0;
    value += 6;

    if (sscanf(value, "%lld-%lld", &start, &end) == 2) {
        if (start < 0 || end < start)
            return -1;
    } else if (sscanf(value, "%lld-", &start) == 1) {
        if (start < 0)
            return -1;
        end = (long long)size - 1;
    } else if (sscanf(value, "-%lld", &end) == 1) {
        if (end <= 0)
            return -1;
        if (end > size)
            end = size;
        start = (long long)size - end;
        end = (long long)size - 1;
    } else {
        return -1;
    }

    if (start >= size)
        return -1;
    if (end >= size)
        end = (long long)size - 1;

    *start_out = (off_t)start;
    *end_out = (off_t)end;
    return 1;
}

static int serve_file(int fd, const char *path, const char *request, int head_only)
{
    int file_fd;
    struct stat st;
    char header[1024];
    char chunk[64 * 1024];
    const char *name;
    ssize_t nread;
    off_t start = 0;
    off_t end;
    off_t remaining;
    int range_result;
    int code = 200;
    const char *status = "OK";
    int n;

    file_fd = open(path, O_RDONLY);
    if (file_fd < 0)
        return respond_text(fd, 404, "Not Found", "text/plain", "Not found\n");
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(file_fd);
        return respond_text(fd, 404, "Not Found", "text/plain", "Not found\n");
    }

    end = st.st_size > 0 ? st.st_size - 1 : 0;
    range_result = parse_range(request, st.st_size, &start, &end);
    if (range_result < 0) {
        n = snprintf(header, sizeof(header),
            "HTTP/1.1 416 Range Not Satisfiable\r\n"
            "Content-Range: bytes */%lld\r\n"
            "Content-Length: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n",
            (long long)st.st_size);
        close(file_fd);
        return (n > 0 && (size_t)n < sizeof(header))
            ? send_all(fd, header, (size_t)n) : -1;
    }
    if (range_result > 0) {
        code = 206;
        status = "Partial Content";
    }

    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    remaining = st.st_size == 0 ? 0 : (end - start + 1);

    n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Accept-Ranges: bytes\r\n"
        "%s"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Cache-Control: no-store\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        code, status, mime_for(path), (long long)remaining,
        range_result > 0 ? "Content-Range: PLACEHOLDER\r\n" : "",
        name);
    if (n < 0 || (size_t)n >= sizeof(header)) {
        close(file_fd);
        return -1;
    }

    if (range_result > 0) {
        char range_line[160];
        char *placeholder = strstr(header, "Content-Range: PLACEHOLDER\r\n");
        if (!placeholder) {
            close(file_fd);
            return -1;
        }
        snprintf(range_line, sizeof(range_line),
                 "Content-Range: bytes %lld-%lld/%lld\r\n",
                 (long long)start, (long long)end, (long long)st.st_size);
        {
            char rebuilt[1024];
            size_t prefix = (size_t)(placeholder - header);
            const char *tail = placeholder + strlen("Content-Range: PLACEHOLDER\r\n");
            if (prefix + strlen(range_line) + strlen(tail) >= sizeof(rebuilt)) {
                close(file_fd);
                return -1;
            }
            memcpy(rebuilt, header, prefix);
            rebuilt[prefix] = '\0';
            strcat(rebuilt, range_line);
            strcat(rebuilt, tail);
            strcpy(header, rebuilt);
            n = (int)strlen(header);
        }
    }

    if (send_all(fd, header, (size_t)n) != 0) {
        close(file_fd);
        return -1;
    }
    if (head_only || remaining == 0) {
        close(file_fd);
        return 0;
    }

    if (lseek(file_fd, start, SEEK_SET) < 0) {
        close(file_fd);
        return -1;
    }

    while (remaining > 0) {
        size_t want = remaining > (off_t)sizeof(chunk)
            ? sizeof(chunk) : (size_t)remaining;
        nread = read(file_fd, chunk, want);
        if (nread <= 0)
            break;
        if (send_all(fd, chunk, (size_t)nread) != 0) {
            close(file_fd);
            return -1;
        }
        remaining -= nread;
    }
    close(file_fd);
    return 0;
}

static int serve_plain_file(int fd, const char *path, const char *content_type)
{
    int file_fd;
    struct stat st;
    char *buf;
    ssize_t nread;
    int result;

    file_fd = open(path, O_RDONLY);
    if (file_fd < 0)
        return respond_text(fd, 404, "Not Found", "application/json", "{\"error\":\"not_ready\"}\n");
    if (fstat(file_fd, &st) != 0 || st.st_size < 0 || st.st_size > 1024 * 1024) {
        close(file_fd);
        return respond_text(fd, 500, "Internal Server Error", "application/json", "{\"error\":\"status_read_failed\"}\n");
    }
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(file_fd);
        return respond_text(fd, 500, "Internal Server Error", "application/json", "{\"error\":\"oom\"}\n");
    }
    nread = read(file_fd, buf, (size_t)st.st_size);
    close(file_fd);
    if (nread < 0) {
        free(buf);
        return respond_text(fd, 500, "Internal Server Error", "application/json", "{\"error\":\"status_read_failed\"}\n");
    }
    result = respond_bytes(fd, 200, "OK", content_type, buf, (size_t)nread);
    free(buf);
    return result;
}

static void append_media_json_array(char *json, size_t cap, size_t *used,
                                    const char *root, const char *prefix,
                                    const char *kind)
{
    DIR *dir;
    struct dirent *entry;
    int first = 1;

    appendf(json, cap, used, "[");
    dir = opendir(root);
    if (!dir) {
        appendf(json, cap, used, "]");
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char video[PATH_MAX];
        char audio[PATH_MAX];
        struct stat vst;
        struct stat ast;
        int have_audio;

        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
            continue;
        snprintf(video, sizeof(video), "%s/%s/%s.h265",
                 root, entry->d_name, entry->d_name);
        if (stat(video, &vst) != 0 || !S_ISREG(vst.st_mode))
            continue;
        snprintf(audio, sizeof(audio), "%s/%s/%s.pcm",
                 root, entry->d_name, entry->d_name);
        have_audio = stat(audio, &ast) == 0 && S_ISREG(ast.st_mode);

        if (!first)
            appendf(json, cap, used, ",");
        first = 0;

        appendf(json, cap, used,
            "{\"id\":\"%s\",\"kind\":\"%s\",\"video_bytes\":%lld,"
            "\"audio_bytes\":%lld,\"has_audio\":%s,"
            "\"video_url\":\"/media/%ss/%s/%s.h265\",",
            entry->d_name, kind, (long long)vst.st_size,
            have_audio ? (long long)ast.st_size : 0LL,
            have_audio ? "true" : "false",
            kind, entry->d_name, entry->d_name);

        if (have_audio) {
            appendf(json, cap, used,
                "\"audio_url\":\"/media/%ss/%s/%s.pcm\"}",
                kind, entry->d_name, entry->d_name);
        } else {
            appendf(json, cap, used, "\"audio_url\":null}");
        }
    }

    closedir(dir);
    appendf(json, cap, used, "]");
}

static int serve_media_json(int fd)
{
    char *json;
    size_t used = 0;
    const size_t cap = 256 * 1024;

    json = calloc(1, cap);
    if (!json)
        return respond_text(fd, 500, "Internal Server Error", "application/json", "{\"error\":\"oom\"}\n");

    appendf(json, cap, &used, "{\"events\":");
    append_media_json_array(json, cap, &used, EVENT_ROOT, "event_", "event");
    appendf(json, cap, &used, ",\"recordings\":");
    append_media_json_array(json, cap, &used, RECORDING_ROOT, "recording_", "recording");
    appendf(json, cap, &used, "}\n");

    if (used >= cap)
        used = cap - 1;
    {
        int result = respond_bytes(fd, 200, "OK", "application/json", json, used);
        free(json);
        return result;
    }
}

static void append_media_list(char *html, size_t cap, size_t *used,
                              const char *root, const char *prefix,
                              const char *url_prefix)
{
    DIR *dir;
    struct dirent *entry;
    char assembled[1024];
    char audio[1024];
    struct stat st;
    struct stat audio_st;

    dir = opendir(root);
    if (!dir) {
        appendf(html, cap, used, "<p>None yet.</p>");
        return;
    }

    appendf(html, cap, used, "<ul>");
    while ((entry = readdir(dir)) != NULL) {
        int have_audio;
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
            continue;

        snprintf(assembled, sizeof(assembled), "%s/%s/%s.h265",
                 root, entry->d_name, entry->d_name);
        if (stat(assembled, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        snprintf(audio, sizeof(audio), "%s/%s/%s.pcm",
                 root, entry->d_name, entry->d_name);
        have_audio = stat(audio, &audio_st) == 0 && S_ISREG(audio_st.st_mode);

        appendf(html, cap, used,
            "<li><strong>%s</strong> &mdash; %.1f MiB%s &mdash; "
            "<a href=\"/media/%s/%s/%s.h265\">H.265</a>",
            entry->d_name,
            (double)st.st_size / (1024.0 * 1024.0),
            have_audio ? " + mic" : "",
            url_prefix, entry->d_name, entry->d_name);

        if (have_audio) {
            appendf(html, cap, used,
                " &middot; <a href=\"/media/%s/%s/%s.pcm\">PCM</a>",
                url_prefix, entry->d_name, entry->d_name);
        }
        appendf(html, cap, used, "</li>");
    }
    appendf(html, cap, used, "</ul>");
    closedir(dir);
}

static int serve_home(int fd)
{
    char *html;
    const size_t cap = 64 * 1024;
    size_t used = 0;
    struct statvfs fs;
    pid_t pid = crow_pid();
    int events = count_dirs(EVENT_ROOT, "event_");
    int recordings = count_dirs(RECORDING_ROOT, "recording_");
    double free_mib = 0.0;

    html = calloc(1, cap);
    if (!html)
        return respond_text(fd, 500, "Internal Server Error", "text/plain", "Out of memory\n");

    if (statvfs("/root", &fs) == 0) {
        unsigned long long free_bytes =
            (unsigned long long)fs.f_bavail * (unsigned long long)fs.f_frsize;
        free_mib = (double)free_bytes / (1024.0 * 1024.0);
    }

    appendf(html, cap, &used,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"8\">"
        "<title>Crow</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;max-width:900px;margin:30px auto;padding:0 18px;background:#111;color:#eee}"
        "button,a{font-size:16px}button{padding:12px 18px;margin:5px;border-radius:10px;border:0}"
        "a{color:#8ecbff}.card{background:#1c1c1e;padding:18px;border-radius:14px;margin:14px 0}"
        "code{background:#2a2a2d;padding:2px 5px;border-radius:5px}"
        "</style></head><body>"
        "<h1>Crow v%s Wireless Media</h1>"
        "<div class=\"card\"><p>Camera: <strong>%s</strong> (PID %d)</p>"
        "<p>Saved events: %d &nbsp; Saved recordings: %d &nbsp; Free: %.0f MiB</p>"
        "<form method=\"post\" action=\"/api/hindsight\"><button>Hindsight</button></form>"
        "<form method=\"post\" action=\"/api/record\"><button>Toggle recording</button></form>"
        "<form method=\"post\" action=\"/api/shutdown\"><button>Stop Crow</button></form>"
        "</div>",
        CROW_WEB_VERSION, pid > 0 ? "running" : "stopped", (int)pid,
        events, recordings, free_mib);

    appendf(html, cap, &used,
        "<div class=\"card\"><h2>Hindsight events</h2>");
    append_media_list(html, cap, &used, EVENT_ROOT, "event_", "events");
    appendf(html, cap, &used, "</div>");

    appendf(html, cap, &used,
        "<div class=\"card\"><h2>Normal recordings</h2>");
    append_media_list(html, cap, &used, RECORDING_ROOT, "recording_", "recordings");
    appendf(html, cap, &used, "</div>");

    appendf(html, cap, &used,
        "<div class=\"card\"><h2>Phone-app API</h2>"
        "<p><code>GET /api/status</code> &middot; <code>GET /api/media</code> &middot; "
        "<code>POST /api/hindsight</code> &middot; <code>POST /api/record</code></p>"
        "<p>Media endpoints now support HTTP byte ranges, so wireless transfers can resume and the future app can seek/stream efficiently.</p>"
        "<p>MP4 muxing is intentionally moving to the phone/app rather than burning hat CPU and battery. The board provides H.265 + PCM over Wi-Fi; the app will turn that pair into a normal MP4.</p>"
        "<p>USB: <code>http://10.31.2.1:%d</code>. Use the same port at the board's Wi-Fi IP.</p>"
        "</div></body></html>", CROW_WEB_PORT);

    if (used >= cap)
        used = cap - 1;
    {
        int result = respond_bytes(fd, 200, "OK", "text/html; charset=utf-8", html, used);
        free(html);
        return result;
    }
}

static int signal_crow(int fd, int signo, const char *message)
{
    pid_t pid = crow_pid();
    if (pid <= 0)
        return respond_text(fd, 409, "Conflict", "application/json", "{\"ok\":false,\"error\":\"camera_not_running\"}\n");
    if (kill(pid, signo) != 0)
        return respond_text(fd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"signal_failed\"}\n");

    {
        char body[256];
        snprintf(body, sizeof(body), "{\"ok\":true,\"message\":\"%s\"}\n", message);
        return respond_text(fd, 200, "OK", "application/json", body);
    }
}

static int handle_media(int fd, const char *url, const char *request, int head_only)
{
    const char *rest;
    char path[4096];

    if (strncmp(url, "/media/events/", 14) == 0) {
        rest = url + 14;
        if (!valid_component(rest))
            return respond_text(fd, 400, "Bad Request", "text/plain", "Invalid path\n");
        snprintf(path, sizeof(path), "%s/%s", EVENT_ROOT, rest);
        return serve_file(fd, path, request, head_only);
    }
    if (strncmp(url, "/media/recordings/", 18) == 0) {
        rest = url + 18;
        if (!valid_component(rest))
            return respond_text(fd, 400, "Bad Request", "text/plain", "Invalid path\n");
        snprintf(path, sizeof(path), "%s/%s", RECORDING_ROOT, rest);
        return serve_file(fd, path, request, head_only);
    }

    return respond_text(fd, 404, "Not Found", "text/plain", "Not found\n");
}

static void handle_client(int fd)
{
    char request[8192];
    ssize_t n;
    char method[16] = {0};
    char path[2048] = {0};

    n = recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0)
        return;
    request[n] = '\0';

    if (sscanf(request, "%15s %2047s", method, path) != 2) {
        respond_text(fd, 400, "Bad Request", "text/plain", "Bad request\n");
        return;
    }

    {
        char *query = strchr(path, '?');
        if (query)
            *query = '\0';
    }

    if (strcmp(method, "OPTIONS") == 0) {
        respond_text(fd, 204, "No Content", "text/plain", "");
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        serve_home(fd);
        return;
    }
    if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
        strncmp(path, "/media/", 7) == 0) {
        handle_media(fd, path, request, strcmp(method, "HEAD") == 0);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ping") == 0) {
        respond_text(fd, 200, "OK", "application/json", "{\"ok\":true,\"service\":\"crow-web\",\"version\":\"0.9\"}\n");
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        serve_plain_file(fd, STATUS_FILE, "application/json");
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/media") == 0) {
        serve_media_json(fd);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/capabilities") == 0) {
        respond_text(fd, 200, "OK", "application/json",
            "{\"wireless_media\":true,\"range_requests\":true,\"audio\":true,\"hindsight_seconds\":60,\"mp4_on_board\":false,\"mp4_target\":\"phone_app\"}\n");
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/hindsight") == 0) {
        signal_crow(fd, SIGUSR1, "hindsight_requested");
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/record") == 0) {
        signal_crow(fd, SIGHUP, "record_toggle_sent");
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/status") == 0) {
        signal_crow(fd, SIGUSR2, "terminal_status_requested");
        return;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/shutdown") == 0) {
        signal_crow(fd, SIGTERM, "shutdown_requested");
        return;
    }

    respond_text(fd, 404, "Not Found", "application/json", "{\"error\":\"not_found\"}\n");
}

int main(void)
{
    int server_fd;
    int one = 1;
    struct sockaddr_in addr;

    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(CROW_WEB_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 8) != 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("Crow web v%s listening on port %d\n", CROW_WEB_VERSION, CROW_WEB_PORT);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
