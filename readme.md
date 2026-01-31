# http2rtsp

Lightweight HTTP to RTSP/RTP proxy for OpenWRT

## 功能特点
- 支持通过 HTTP URL 访问 RTSP 流
- 自动处理 RTSP 302 重定向
- 支持 RTP/AVP/TCP 传输模式
- 轻量级设计，适合在 OpenWRT 等嵌入式设备上运行

## 编译

### 依赖项
- gcc
- make
- 标准 C 库

### 编译命令
```bash
# 直接编译
gcc -Wall -Os -s -o http2rtsp http2rtsp.c

# 交叉编译（以 OpenWRT 为例）
mipsel-openwrt-linux-gcc -Wall -Os -s -o http2rtsp-mipsel http2rtsp.c
```

## 使用方法

### 基本用法
```bash
./http2rtsp
```

### 命令行参数
- `-p <port>`: 指定 HTTP 监听端口（默认：8090）
- `-c <max>`: 指定最大客户端连接数（默认：10）
- `-B <size>`: 指定缓冲区大小（默认：32KB）
- `-v`: 启用详细日志
- `-T`: 以非守护进程模式运行

### 示例
```bash
# 在端口 8090 上运行，启用详细日志，非守护进程模式
./http2rtsp -p 8090 -v -T

# 限制最大 5 个客户端连接，缓冲区大小 64KB
./http2rtsp -c 5 -B 64
```

## URL 格式

使用以下格式访问 RTSP 流：
```
http://<host>:<port>/rtsp://<server>:<port>/<path>
```

### 示例
```
# 访问默认端口上的 RTSP 流
http://192.168.1.1:8090/rtsp://183.59.156.166/PLTV/88888888/224/3221229774/10000100000000060000000008842383_0.smil

# 访问指定端口的 RTSP 流
http://192.168.1.1:8090/rtsp://192.168.0.100:554/stream1
```

## 工作原理

1. 接收 HTTP 请求，解析其中的 RTSP URL
2. 连接到 RTSP 服务器，发送 OPTIONS 请求
3. 发送 DESCRIBE 请求，获取 SDP 信息
4. 自动处理 302 重定向（如果需要）
5. 发送 SETUP 请求，建立 RTP/AVP/TCP 传输通道
6. 发送 PLAY 请求，开始流媒体传输
7. 将 RTP 数据转发给 HTTP 客户端

## 日志说明

启用详细日志（`-v` 参数）后，会输出以下信息：
- 客户端连接信息
- RTSP 请求和响应状态
- 重定向处理过程
- 错误信息

## 常见问题

### 无法播放
- 检查 RTSP URL 是否正确，确保包含端口号
- 确认 RTSP 服务器是否可访问
- 查看详细日志以获取更多信息

### 编译告警
- 编译时可能会出现 `-Wformat-truncation=` 告警，这是由于 URL 长度计算导致的
- 已将 `MAX_URL_LEN` 设置为 8192，足以容纳大多数情况

### 重定向失败
- 确保重定向 URL 格式正确
- 检查网络是否能够访问重定向目标服务器

## 技术参数

- 默认 HTTP 端口：8090
- 默认缓冲区大小：32KB
- 最大客户端连接数：10
- 支持的 RTSP 版本：RTSP/1.0
- 支持的传输模式：RTP/AVP/TCP

## 许可证

MIT License

## 版本

当前版本：1.2
