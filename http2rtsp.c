/*
 * http2rtsp.c - Lightweight HTTP to RTSP/RTP proxy for OpenWRT
 * URL format: http://router:554/rtsp://server:554/path
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>

#define VERSION "1.1"
#define DEFAULT_PORT 8090
#define DEFAULT_BUF_SIZE (32*1024)
#define MAX_CLIENTS 10
#define MAX_URL_LEN 2048
#define MAX_HEADER_LEN 4096
#define RTP_INTERLEAVED 0x24

#define BUILD_DATE __DATE__
#define BUILD_TIME __TIME__

static int g_port = DEFAULT_PORT;
static int g_max_clients = MAX_CLIENTS;
static int g_buf_size = DEFAULT_BUF_SIZE;
static int g_verbose = 0;
static int g_daemon = 1;

#define HTTP_200_OK "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n"
#define HTTP_400_BAD "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
#define HTTP_404_NOT "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
#define HTTP_500_ERR "HTTP/1.0 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
#define HTTP_503_BUSY "HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

#define LOG(fmt, ...) do { if (g_verbose) fprintf(stderr, "[%d] " fmt "\n", getpid(), ##__VA_ARGS__); } while(0)

static void sigchld_handler(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static void set_tcp_nodelay(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

static int url_decode(const char *src, char *dst, int dst_len) {
    int i, j;
    for (i = 0, j = 0; src[i] && j < dst_len - 1; i++, j++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            unsigned int hex;
            sscanf(src + i + 1, "%2x", &hex);
            dst[j] = (char)hex;
            i += 2;
        } else if (src[i] == '+') {
            dst[j] = ' ';
        } else {
            dst[j] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

static int parse_rtsp_url(const char *url, char *host, int *port, char *path, int path_len) {
    const char *p = url;
    *port = 554;
    
    if (strncmp(p, "rtsp://", 7) != 0) return -1;
    p += 7;
    
    const char *path_start = strchr(p, '/');
    if (!path_start) {
        strncpy(host, p, 255);
        strcpy(path, "/");
    } else {
        int host_len = path_start - p;
        if (host_len >= 256) host_len = 255;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        strncpy(path, path_start, path_len - 1);
        path[path_len - 1] = '\0';
    }
    
    char *port_ptr = strchr(host, ':');
    if (port_ptr) {
        *port_ptr = '\0';
        *port = atoi(port_ptr + 1);
    }
    
    return 0;
}

static int send_all(int fd, const char *buf, int len, int timeout_sec) {
    int total = 0;
    fd_set fds;
    struct timeval tv;
    
    while (total < len) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        
        if (select(fd + 1, NULL, &fds, NULL, &tv) <= 0)
            return -1;
        
        int n = send(fd, buf + total, len - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return total;
}

static int recv_line(int fd, char *buf, int max_len, int timeout_sec) {
    int i = 0;
    fd_set fds;
    struct timeval tv;
    
    while (i < max_len - 1) {
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0)
            return -1;
        
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        
        buf[i++] = c;
        if (c == '\n' && i >= 2 && buf[i-2] == '\r') {
            buf[i] = '\0';
            return i;
        }
    }
    buf[i] = '\0';
    return i;
}

static int send_rtsp_request(int rtsp_fd, const char *method, const char *url, 
                             const char *session, int cseq, char *extra_headers) {
    char req[MAX_HEADER_LEN];
    int len = snprintf(req, sizeof(req),
        "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: http2rtsp/%s\r\n%s%s%s%s\r\n",
        method, url, cseq, VERSION,
        extra_headers ? extra_headers : "",
        session ? "Session: " : "",
        session ? session : "",
        session ? "\r\n" : ""
    );
    return send_all(rtsp_fd, req, len, 5);
}

static int parse_rtsp_response(int rtsp_fd, char *session, int sess_len, 
                               int *content_length, int timeout, char *location, int loc_len) {
    char line[MAX_HEADER_LEN];
    int cseq = -1, status = 0;
    *content_length = 0;
    if (session) session[0] = '\0';
    if (location) location[0] = '\0';
    
    if (recv_line(rtsp_fd, line, sizeof(line), timeout) <= 0)
        return -1;
    
    sscanf(line, "RTSP/1.0 %d", &status);
    LOG("RTSP status: %d ;CSeq: %d", status,cseq);
    
    while (recv_line(rtsp_fd, line, sizeof(line), timeout) > 0) {
        if (line[0] == '\r' && line[1] == '\n') break;
        
        if (strncasecmp(line, "Session:", 8) == 0 && session) {
            char *p = line + 8;
            while (*p == ' ' || *p == '\t') p++;
            char *end = strchr(p, ';');
            if (!end) end = strchr(p, '\r');
            if (end) {
                int len = end - p;
                if (len >= sess_len) len = sess_len - 1;
                strncpy(session, p, len);
                session[len] = '\0';
            }
        }
        else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            *content_length = atoi(line + 15);
        }
        else if (strncasecmp(line, "Location:", 9) == 0 && location) {
            char *p = line + 9;
            while (*p == ' ' || *p == '\t') p++;
            char *end = strchr(p, '\r');
            if (end) {
                int len = end - p;
                if (len >= loc_len) len = loc_len - 1;
                strncpy(location, p, len);
                location[len] = '\0';
            }
        }
    }
    
    return status;
}

static void skip_body(int rtsp_fd, int content_length) {
    char buf[1024];
    while (content_length > 0) {
        int n = recv(rtsp_fd, buf, content_length > 1024 ? 1024 : content_length, 0);
        if (n <= 0) break;
        content_length -= n;
    }
}

static int rtsp_setup_play(int rtsp_fd, const char *url, int *rtp_channel, int *rtcp_channel) {
    char session[256] = "";
    int cseq = 1;
    int content_len;
    char current_url[MAX_URL_LEN];
    strncpy(current_url, url, sizeof(current_url)-1);
    
    LOG("Sending OPTIONS");
    if (send_rtsp_request(rtsp_fd, "OPTIONS", current_url, NULL, cseq++, NULL) < 0)
        return -1;
    if (parse_rtsp_response(rtsp_fd, NULL, 0, &content_len, 5, NULL, 0) != 200)
        return -1;
    
    LOG("Sending DESCRIBE");
    char extra[256];
    snprintf(extra, sizeof(extra), "Accept: application/sdp\r\n");
    if (send_rtsp_request(rtsp_fd, "DESCRIBE", current_url, NULL, cseq++, extra) < 0)
        return -1;
    
    char location[MAX_URL_LEN];
    int status = parse_rtsp_response(rtsp_fd, NULL, 0, &content_len, 10, location, sizeof(location));
    
    // Handle 302 redirect
    if (status == 302) {
        LOG("Received 302 redirect to: %s", location);
        if (location[0] == '\0') {
            LOG("No Location header in redirect response");
            return -1;
        }
        
        // For 302 redirect, we need to reconnect to the new server
        // Close current connection
        close(rtsp_fd);
        
        // Parse redirect URL to get new host and port
        char new_host[256];
        int new_port;
        char new_path[MAX_URL_LEN];
        if (parse_rtsp_url(location, new_host, &new_port, new_path, sizeof(new_path)) < 0) {
            LOG("Failed to parse redirect URL");
            return -1;
        }
        
        // Connect to new server
        int new_rtsp_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (new_rtsp_fd < 0) {
            LOG("Failed to create socket for redirect");
            return -1;
        }
        
        struct hostent *he = gethostbyname(new_host);
        if (!he) {
            LOG("Failed to resolve new host: %s", new_host);
            close(new_rtsp_fd);
            return -1;
        }
        
        struct sockaddr_in new_rtsp_addr;
        memset(&new_rtsp_addr, 0, sizeof(new_rtsp_addr));
        new_rtsp_addr.sin_family = AF_INET;
        new_rtsp_addr.sin_port = htons(new_port);
        memcpy(&new_rtsp_addr.sin_addr, he->h_addr, he->h_length);
        
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(new_rtsp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        if (connect(new_rtsp_fd, (struct sockaddr*)&new_rtsp_addr, sizeof(new_rtsp_addr)) < 0) {
            LOG("Failed to connect to new server: %s:%d", new_host, new_port);
            close(new_rtsp_fd);
            return -1;
        }
        
        set_tcp_nodelay(new_rtsp_fd);
        rtsp_fd = new_rtsp_fd;
        
        // Update current URL
        strncpy(current_url, location, sizeof(current_url)-1);
        LOG("Connected to new server: %s:%d", new_host, new_port);
        
        // Restart RTSP flow with new connection
        cseq = 1;
        
        // Send OPTIONS to new server
        LOG("Sending OPTIONS to new server");
        if (send_rtsp_request(rtsp_fd, "OPTIONS", current_url, NULL, cseq++, NULL) < 0) {
            close(rtsp_fd);
            return -1;
        }
        if (parse_rtsp_response(rtsp_fd, NULL, 0, &content_len, 5, NULL, 0) != 200) {
            close(rtsp_fd);
            return -1;
        }
        
        // Send DESCRIBE to new server
        LOG("Sending DESCRIBE to new server");
        if (send_rtsp_request(rtsp_fd, "DESCRIBE", current_url, NULL, cseq++, extra) < 0) {
            close(rtsp_fd);
            return -1;
        }
        
        status = parse_rtsp_response(rtsp_fd, NULL, 0, &content_len, 10, NULL, 0);
        if (status != 200) {
            LOG("DESCRIBE failed after redirect: %d", status);
            close(rtsp_fd);
            return -1;
        }
    } else if (status != 200) {
        LOG("DESCRIBE failed: %d", status);
        return -1;
    }
    
    char *sdp = malloc(content_len + 1);
    if (!sdp) return -1;
    
    int total = 0;
    while (total < content_len) {
        int n = recv(rtsp_fd, sdp + total, content_len - total, 0);
        if (n <= 0) { free(sdp); return -1; }
        total += n;
    }
    sdp[content_len] = '\0';
    LOG("SDP:\n%s", sdp);
    
    char control_url[MAX_URL_LEN];
    strncpy(control_url, current_url, sizeof(control_url)-1);
    
    char *a = strstr(sdp, "a=control:");
    if (a) {
        a += 10;
        char *end = strchr(a, '\n');
        if (end) {
            char ctrl[512];
            int len = end - a;
            if (len > 511) len = 511;
            strncpy(ctrl, a, len);
            ctrl[len] = '\0';
            while (len > 0 && (ctrl[len-1] == '\r' || ctrl[len-1] == ' ')) 
                ctrl[--len] = '\0';
            
            if (strncmp(ctrl, "rtsp://", 7) == 0)
                strncpy(control_url, ctrl, sizeof(control_url)-1);
            else {
                int url_len = strlen(current_url);
                if (url_len > sizeof(current_url)-1) url_len = sizeof(current_url)-1;
                if (current_url[url_len-1] == '/' || ctrl[0] == '/')
                    snprintf(control_url, sizeof(control_url), "%s%s", current_url, ctrl);
                else
                    snprintf(control_url, sizeof(control_url), "%s/%s", current_url, ctrl);
            }
        }
    }
    free(sdp);
    
    LOG("Sending SETUP for %s", control_url);
    snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    
    if (send_rtsp_request(rtsp_fd, "SETUP", control_url, NULL, cseq++, extra) < 0)
        return -1;
    
    if (parse_rtsp_response(rtsp_fd, session, sizeof(session), &content_len, 10, NULL, 0) != 200) {
        LOG("SETUP failed");
        return -1;
    }
    skip_body(rtsp_fd, content_len);
    LOG("Session: %s", session);
    
    *rtp_channel = 0;
    *rtcp_channel = 1;
    
    LOG("Sending PLAY");
    snprintf(extra, sizeof(extra), "Range: npt=0.000-\r\n");
    if (send_rtsp_request(rtsp_fd, "PLAY", control_url, session, cseq++, extra) < 0)
        return -1;
    
    if (parse_rtsp_response(rtsp_fd, NULL, 0, &content_len, 10, NULL, 0) != 200) {
        LOG("PLAY failed");
        return -1;
    }
    skip_body(rtsp_fd, content_len);
    
    return 0;
}

static int relay_rtp_data(int rtsp_fd, int client_fd) {
    unsigned char *buf = malloc(g_buf_size);
    if (!buf) return -1;
    
    fd_set rfds;
    struct timeval tv;
    int max_fd = (rtsp_fd > client_fd ? rtsp_fd : client_fd) + 1;
    int running = 1;
    int error_count = 0;
    
    while (running && error_count < 10) {
        FD_ZERO(&rfds);
        FD_SET(rtsp_fd, &rfds);
        
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        
        int ret = select(max_fd, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;
        
        if (FD_ISSET(rtsp_fd, &rfds)) {
            unsigned char header[4];
            int n = recv(rtsp_fd, header, 1, MSG_PEEK);
            if (n <= 0) { error_count++; continue; }
            
            if (header[0] != RTP_INTERLEAVED) {
                char line[MAX_HEADER_LEN];
                if (recv_line(rtsp_fd, line, sizeof(line), 2) <= 0)
                    error_count++;
                continue;
            }
            
            n = recv(rtsp_fd, header, 4, MSG_WAITALL);
            if (n != 4) { error_count++; continue; }
            
            int channel = header[1];
            int length = (header[2] << 8) | header[3];
            
            if (length > g_buf_size - 4) length = g_buf_size - 4;
            
            int received = 0;
            while (received < length) {
                n = recv(rtsp_fd, buf + 4 + received, length - received, 0);
                if (n <= 0) { error_count++; break; }
                received += n;
            }
            if (received < length) continue;
            
            if (channel == 0) {
                if (send_all(client_fd, (char*)buf + 4, received, 2) < 0) {
                    LOG("Client disconnected");
                    running = 0;
                    break;
                }
            }
            error_count = 0;
        }
    }
    
    free(buf);
    return 0;
}

/* ============ 修改部分：URL解析逻辑 ============ */

static void handle_client(int client_fd, struct sockaddr_in *client_addr) {
    char request[MAX_HEADER_LEN];
    char rtsp_url[MAX_URL_LEN];
    char host[256], path[MAX_URL_LEN];
    int rtsp_port;
    
    LOG("Client connected from %s:%d", 
        inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));
    
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    /* 读取HTTP请求头 */
    int total = 0;
    int header_complete = 0;
    while (total < sizeof(request) - 1) {
        int n = recv(client_fd, request + total, sizeof(request) - 1 - total, 0);
        if (n <= 0) goto cleanup;
        total += n;
        request[total] = '\0';
        
        if (strstr(request, "\r\n\r\n")) {
            header_complete = 1;
            break;
        }
    }
    
    if (!header_complete) {
        send_all(client_fd, HTTP_400_BAD, strlen(HTTP_400_BAD), 2);
        goto cleanup;
    }
    
    LOG("Request: %.100s", request);
    
    /* ============ 新的URL解析逻辑 ============ */
    /*
     * 支持的格式：
     * GET /rtsp://server:554/path HTTP/1.x
     * GET /rtsp://server:554/path?query HTTP/1.x
     * 
     * 从请求行中提取 rtsp:// 开头的URL
     */
    
    /* 找到请求行中的第一个空格（GET之后） */
    char *space1 = strchr(request, ' ');
    if (!space1) {
        send_all(client_fd, HTTP_400_BAD, strlen(HTTP_400_BAD), 2);
        goto cleanup;
    }
    space1++;  /* 指向URL开始位置，应该是 /rtsp://... */
    
    /* 找到第二个空格（HTTP版本之前） */
    char *space2 = strchr(space1, ' ');
    if (!space2) {
        send_all(client_fd, HTTP_400_BAD, strlen(HTTP_400_BAD), 2);
        goto cleanup;
    }
    
    /* 提取路径部分 */
    int path_len = space2 - space1;
    if (path_len <= 0 || path_len >= MAX_URL_LEN) {
        send_all(client_fd, HTTP_400_BAD, strlen(HTTP_400_BAD), 2);
        goto cleanup;
    }
    
    char path_part[MAX_URL_LEN];
    strncpy(path_part, space1, path_len);
    path_part[path_len] = '\0';
    
    LOG("Path part: %s", path_part);
    
    /* 检查是否以 /rtsp:// 开头 */
    if (strncmp(path_part, "/rtsp://", 8) != 0) {
        LOG("Invalid URL format, must start with /rtsp://");
        send_all(client_fd, HTTP_404_NOT, strlen(HTTP_404_NOT), 2);
        goto cleanup;
    }
    
    /* 提取 rtsp://... 部分（去掉开头的 /） */
    char *url_start = path_part + 1;  /* 跳过第一个 /，得到 rtsp://... */
    
    /* 处理可能的URL编码 */
    url_decode(url_start, rtsp_url, sizeof(rtsp_url));
    
    LOG("Target RTSP: %s", rtsp_url);
    
    /* 验证RTSP URL格式 */
    if (strncmp(rtsp_url, "rtsp://", 7) != 0) {
        send_all(client_fd, HTTP_400_BAD, strlen(HTTP_400_BAD), 2);
        goto cleanup;
    }
    
    /* ============ 后续逻辑不变 ============ */
    
    if (parse_rtsp_url(rtsp_url, host, &rtsp_port, path, sizeof(path)) < 0) {
        send_all(client_fd, HTTP_400_BAD, strlen(HTTP_400_BAD), 2);
        goto cleanup;
    }
    
    LOG("Connecting to %s:%d%s", host, rtsp_port, path);
    
    int rtsp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rtsp_fd < 0) {
        send_all(client_fd, HTTP_500_ERR, strlen(HTTP_500_ERR), 2);
        goto cleanup;
    }
    
    struct hostent *he = gethostbyname(host);
    if (!he) {
        send_all(client_fd, HTTP_404_NOT, strlen(HTTP_404_NOT), 2);
        close(rtsp_fd);
        goto cleanup;
    }
    
    struct sockaddr_in rtsp_addr;
    memset(&rtsp_addr, 0, sizeof(rtsp_addr));
    rtsp_addr.sin_family = AF_INET;
    rtsp_addr.sin_port = htons(rtsp_port);
    memcpy(&rtsp_addr.sin_addr, he->h_addr, he->h_length);
    
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(rtsp_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(rtsp_fd, (struct sockaddr*)&rtsp_addr, sizeof(rtsp_addr)) < 0) {
        send_all(client_fd, HTTP_500_ERR, strlen(HTTP_500_ERR), 2);
        close(rtsp_fd);
        goto cleanup;
    }
    
    set_tcp_nodelay(rtsp_fd);
    
    int rtp_ch, rtcp_ch;
    if (rtsp_setup_play(rtsp_fd, rtsp_url, &rtp_ch, &rtcp_ch) < 0) {
        send_all(client_fd, HTTP_500_ERR, strlen(HTTP_500_ERR), 2);
        close(rtsp_fd);
        goto cleanup;
    }
    
    if (send_all(client_fd, HTTP_200_OK, strlen(HTTP_200_OK), 2) < 0) {
        close(rtsp_fd);
        goto cleanup;
    }
    
    LOG("Starting relay...");
    relay_rtp_data(rtsp_fd, client_fd);
    
    LOG("Closing RTSP connection");
    close(rtsp_fd);
    
cleanup:
    close(client_fd);
    LOG("Client handler exiting");
}

static int run_server(void) {
    int listen_fd;
    struct sockaddr_in serv_addr;
    
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(g_port);
    
    if (bind(listen_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }
    
    LOG("http2rtsp v%s (built on %s %s) listening on port %d", VERSION, BUILD_DATE, BUILD_TIME, g_port);
    LOG("Buffer size: %d KB, Max clients: %d", g_buf_size/1024, g_max_clients);
    LOG("URL format: http://host:%d/rtsp://server:554/path", g_port);
    
    signal(SIGCHLD, sigchld_handler);
    signal(SIGPIPE, SIG_IGN);
    
    int active_clients = 0;
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        
        if (active_clients >= g_max_clients) {
            send_all(client_fd, HTTP_503_BUSY, strlen(HTTP_503_BUSY), 2);
            close(client_fd);
            continue;
        }
        
        set_tcp_nodelay(client_fd);
        
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
        } else if (pid == 0) {
            close(listen_fd);
            handle_client(client_fd, &client_addr);
            exit(0);
        } else {
            close(client_fd);
            active_clients++;
            LOG("Forked handler pid=%d, active=%d/%d", pid, active_clients, g_max_clients);
        }
        
        while (waitpid(-1, NULL, WNOHANG) > 0) {
            if (active_clients > 0) active_clients--;
        }
    }
    
    close(listen_fd);
    return 0;
}

static void usage(const char *prog) {
    printf("http2rtsp v%s (built on %s %s) - Lightweight HTTP to RTSP proxy\n", VERSION, BUILD_DATE, BUILD_TIME);
    printf("Usage: %s [-p port] [-c clients] [-B sizeK] [-v] [-T]\n", prog);
    printf("  -p port     : HTTP listen port (default: %d)\n", DEFAULT_PORT);
    printf("  -c clients  : max concurrent clients (default: %d)\n", MAX_CLIENTS);
    printf("  -B sizeK    : buffer size in KB (default: %d)\n", DEFAULT_BUF_SIZE/1024);
    printf("  -v          : verbose mode\n");
    printf("  -T          : do not run as daemon\n");
    printf("\nURL format: http://host:%d/rtsp://server:port/path\n", DEFAULT_PORT);
    printf("Example: http://192.168.1.201:8090/rtsp://183.59.156.166:554/PLTV/...\n");
}

int main(int argc, char *argv[]) {
    int opt;
    
    while ((opt = getopt(argc, argv, "p:c:B:vTh")) != -1) {
        switch (opt) {
            case 'p': g_port = atoi(optarg); break;
            case 'c': g_max_clients = atoi(optarg); break;
            case 'B': g_buf_size = atoi(optarg) * 1024; break;
            case 'v': g_verbose = 1; break;
            case 'T': g_daemon = 0; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }
    
    if (g_daemon && daemon(0, 0) < 0) {
        perror("daemon");
        return 1;
    }
    
    return run_server();
}