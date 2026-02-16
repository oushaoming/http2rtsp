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
- `-m <max>`: 指定最大客户端连接数（默认：10）
- `-b <size>`: 指定缓冲区大小（默认：32KB）
- `-v`: 启用详细日志（调试时使用，输出到终端）
- `-T`: 以非守护进程模式运行

### 示例
```bash
# 在端口 8090 上运行，启用详细日志，非守护进程模式
./http2rtsp -p 8090 -v -T

# 限制最大 5 个客户端连接，缓冲区大小 64KB
./http2rtsp -m 5 -b 65536
```

**注意**：
- 默认运行模式（不使用 -v 参数）不输出任何日志，避免磁盘 IO，适合生产环境
- 使用 -v 参数时，日志会输出到终端（stderr），用于调试
- 所有日志仅输出到终端，不写入文件

## URL 格式

支持两种 URL 格式访问 RTSP 流：

### 格式 1：完整 RTSP URL（推荐）
```
http://<host>:<port>/rtsp://<server>:<port>/<path>
```

### 格式 2：简化 RTSP URL
```
http://<host>:<port>/rtsp/<server>:<port>/<path>
```

### 示例

#### 使用完整 RTSP URL 格式
```
# 访问默认端口上的 RTSP 流
http://192.168.1.1:8090/rtsp://183.59.156.166/PLTV/88888888/224/3221229774/10000100000000060000000008842383_0.smil

# 访问指定端口的 RTSP 流
http://192.168.1.1:8090/rtsp://192.168.0.100:554/stream1
```

#### 使用简化 RTSP URL 格式
```
# 访问默认端口上的 RTSP 流
http://192.168.1.1:8090/rtsp/183.59.156.166/PLTV/88888888/224/3221229774/10000100000000060000000008842383_0.smil

# 访问指定端口的 RTSP 流
http://192.168.1.1:8090/rtsp/192.168.0.100:554/stream1
```

**说明**：两种格式功能完全相同，简化格式省略了 `rtsp://` 中的 `://` 部分，代理会自动转换为标准格式发送给 RTSP 服务器。

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
- 使用 GCC 12.3 编译时可能会出现 `-Wformat-truncation` 告警
- 这是由于编译器严格检查 `snprintf` 缓冲区大小导致的
- 不影响功能，可以忽略或使用 GCC 7.4 编译

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

当前版本：1.3

### 版本历史

#### v1.3 (2026-02-16)
- 新增支持简化 RTSP URL 格式（`/rtsp/`）
- 现在同时支持 `/rtsp://` 和 `/rtsp/` 两种 URL 格式
- 代理自动将简化格式转换为标准 `rtsp://` 格式发送给 RTSP 服务器
- 修复了编译警告（未使用的变量和函数）
- 完善了 RTSP 会话建立流程（SETUP 和 PLAY）
- **增强 URL 解析**：
  - 支持 IPv6 地址格式（[::1]:port）
  - 支持认证信息（user:pass@host:port/path）
  - 改进的参数验证和错误检查
- **新增配置常量**：
  - RTSP_MAX_REDIRECTS：最大重定向次数（默认 5）
  - RTSP_CONNECT_TIMEOUT_SEC：连接超时（默认 10 秒）
  - RTSP_REQUEST_TIMEOUT_SEC：请求超时（默认 10 秒）
  - RTSP_RESPONSE_TIMEOUT_SEC：响应超时（默认 10 秒）
- **改进错误处理**：
  - 更详细的日志输出（仅在 -v 模式下）
  - 更好的错误消息
  - 参数验证
- **性能优化**：
  - 默认模式不输出日志（避免磁盘 IO）
  - 使用 -v 参数时，日志输出到终端（stderr）
  - 所有日志仅输出到终端，不写入文件
  - 专为低性能 OpenWRT 路由器优化
  - 生产环境零磁盘 IO，调试环境可启用日志

#### v1.2
- 优化了 RTSP 302 重定向处理
- 改进了错误处理和日志输出
- 增加了连接超时机制

#### v1.1
- 初始版本
- 支持基本的 HTTP 到 RTSP 代理功能
- 支持 RTP/AVP/TCP 传输模式
