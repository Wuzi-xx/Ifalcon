#include <stdio.h>      // 标准输入输出
#include <stdlib.h>     // 标准库函数
#include <unistd.h>     // UNIX 标准函数定义，如 read/write/usleep
#include <fcntl.h>      // 文件控制定义 open()
#include <sys/types.h>  // 系统数据类型
#include <sys/wait.h>   // 进程等待 wait()
#include <errno.h>      // 错误处理 errno
#include <string.h>     // 字符串处理函数
#include <time.h>       // 时间与延时
#include <sys/stat.h>   // 文件属性定义 mkdir/stat
#include <arpa/inet.h>  // IP 地址转换
#include <netinet/in.h> // 网络结构定义
#include <sys/socket.h> // 套接字接口
#include <dirent.h>     // 目录操作（opendir/readdir）

// ----------------- 参数定义区 -----------------
#define GPIO_A            33           // 控制线圈上电的 GPIO（GPIO33）
#define GPIO_B            32           // 控制线圈断电的 GPIO（GPIO32）
#define TRIGGER_COUNT     5            // 连续检测到阈值电压 5 次后触发
#define VOLTAGE_THRESH    0.0f         // 电压阈值（V），超过该值触发
#define HOLD_TIME_US      500000       // 拍摄持续时间（微秒）
#define HOLD_TIME_SEC     5            // 线圈保持通电时间（秒）
#define SAMPLE_US         5000         // ADC 采样周期（微秒）

#define MJPG_HOME  "/root/mjpg"        // mjpg_streamer 主目录
#define WWW_DIR    "/root/mjpg/www"    // HTTP 输出目录
#define CAPS_DIR   WWW_DIR              // 图像保存目录
#define HTTP_PORT  8080                // HTTP 端口号

#define TARGET_FPS 15                  // 每秒抓取帧数
#define SNAPSHOT_INTERVAL_US (1000000 / TARGET_FPS) // 每帧间隔时间

static int g_total_saved = 0;          // 全局变量：记录已保存的总帧数

// ----------------- 工具函数：目录与文件管理 -----------------

/**
 * @brief 递归创建目录（行为类似 shell 命令 `mkdir -p`）
 *
 * 功能：
 *  - 输入一个完整的路径字符串（如 /root/mjpg/www）
 *  - 自动检查路径中每一级子目录是否存在，不存在则逐级创建
 *  - 可用于初始化程序运行前的输出目录（如 CAPS_DIR）
 *
 * @param path  需要创建的完整路径（如 "/root/mjpg/www"）
 * @param mode  创建目录的权限（通常为 0755）
 * @return int  返回 0 表示成功，-1 表示失败
 */
static int mkdir_p(const char *path, mode_t mode){
    // 定义一个临时字符串缓冲区 tmp，用于构造路径
    // snprintf 确保不会越界复制
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    // 获取路径长度
    size_t len = strlen(tmp);

    // 若路径为空（长度为 0），直接返回错误
    if(!len) return -1;

    // 如果路径以 “/” 结尾，则去掉最后一个 “/”，避免多余空目录
    if(tmp[len-1]=='/') tmp[len-1]='\0';

    /**
     * 从路径的第二个字符开始遍历 tmp（例如 "/root/mjpg/www"）：
     * 每当遇到 '/' 时：
     *   - 临时截断字符串
     *   - 调用 mkdir 创建当前级目录
     *   - 若已存在 (errno == EEXIST)，则忽略错误继续
     *   - 恢复 '/' 字符，继续下一层
     *
     * 例如：
     *   /root/mjpg/www
     *   第一次循环：创建 "/root"
     *   第二次循环：创建 "/root/mjpg"
     *   第三次循环：创建 "/root/mjpg/www"
     */
    for(char *p=tmp+1; *p; ++p){
        if(*p=='/'){
            *p='\0'; // 暂时截断路径（如 "/root"）
            if(mkdir(tmp,mode) && errno != EEXIST)
                return -1; // 若 mkdir 失败且不是“目录已存在”，返回错误
            *p='/'; // 恢复为原路径（继续下一层）
        }
    }

    // 最后创建完整路径（最后一级目录）
    if(mkdir(tmp,mode) && errno != EEXIST)
        return -1;

    // 全部创建成功则返回 0
    return 0;
}

/**
 * @brief 删除 CAPS_DIR（通常为 /root/mjpg/www）目录下旧的 .jpg 文件和 index.html 文件
 *
 * 功能：
 *   - 程序启动前执行，清空上次运行残留的截图文件
 *   - 保证新的拍摄内容不会混入旧数据
 *
 * 注意：
 *   - 仅删除指定类型文件（.jpg 与 index.html）
 *   - 不会删除其他文件或子目录，安全性较高
 *
 * @return int  始终返回 0（即使目录不存在也不会报错）
 */
static int rm_caps_dir_contents(void) {
    // 尝试打开目标目录（CAPS_DIR 通常为 "/root/mjpg/www"）
    DIR *d = opendir(CAPS_DIR);

    // 若目录不存在（返回 NULL），则无需删除，直接返回
    if (!d) return 0;

    struct dirent *ent;  // 用于保存目录项信息（文件名等）
    char path[512];      // 临时路径缓冲区，用于拼接完整路径

    // 循环读取目录中的每一个文件条目
    while ((ent = readdir(d))) {

        // 跳过 "." 与 ".."（当前目录与上级目录）
        if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,".."))
            continue;

        // 获取当前文件名长度
        size_t L = strlen(ent->d_name);

        // 判断文件是否是 ".jpg" 结尾（不区分大小写）
        int is_jpg = (L >= 4 && (strcasecmp(ent->d_name + L - 4, ".jpg") == 0));

        // 判断文件是否为 "index.html"
        int is_index = (strcasecmp(ent->d_name, "index.html") == 0);

        // 如果既不是 jpg，也不是 index.html，则跳过不删
        if (!(is_jpg || is_index))
            continue;

        // 拼接完整文件路径，如 "/root/mjpg/www/000.jpg"
        snprintf(path, sizeof(path), "%s/%s", CAPS_DIR, ent->d_name);

        // 调用 unlink() 删除该文件
        unlink(path);
    }

    // 遍历结束后关闭目录指针
    closedir(d);

    // 返回 0 表示成功（无论是否有文件被删）
    return 0;
}

/**
 * @brief 自动生成一个简易网页 (index.html)，用于在浏览器中查看捕获到的图像帧
 *
 * 功能：
 *   - 生成 /root/mjpg/www/index.html 文件
 *   - 每个已保存的 jpg 图像自动在网页中以缩略图形式显示
 *   - 点击缩略图可放大查看原图
 *   - 页面支持自动排版（CSS Flex 布局）
 *
 * @param count 当前已有的帧数（用于决定生成多少个 <img> 标签）
 */
static void write_index_html(int count){
    // ---------------- HTML 页面头部模板 ----------------
    // 包含页面标题、CSS 样式、图像布局和 JS 逻辑
    const char *head =
            "<!doctype html><meta charset='utf-8'><title>Caps</title>"
            "<style>"
            "body{font-family:sans-serif;margin:20px}"           // 设置字体和边距
            "img{max-width:320px;margin:8px;border:1px solid #ddd;border-radius:8px}" // 图像样式（缩略图效果）
            ".wrap{display:flex;flex-wrap:wrap}"                 // 使用 Flex 布局使图片自动换行
            "</style>"
            "<h3>Captured Frames</h3><div class='wrap' id='g'></div>" // 页面标题与图片容器
            "<script>\n"
            "const g=document.getElementById('g');\n";           // JS 获取图片容器元素

    // ---------------- HTML 页面尾部模板 ----------------
    const char *tail = "</script>\n";

    // 打开（或新建）index.html 文件，写入模式为覆盖写（O_TRUNC）
    int fd = open(CAPS_DIR "/index.html", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if(fd < 0) return; // 若创建失败则直接退出

    // 写入页面头部 HTML 内容
    write(fd, head, strlen(head));

    // 临时缓冲区，用于存放每一行 JS 语句（生成单个 <img> 标签）
    char line[256];

    // ---------------- 动态生成图片展示区 ----------------
    for(int i = 0; i < count; i++){
        /**
         * 每张图片都用一段 JS 代码生成一个 <a><img></a> 元素：
         *   - n='%03d.jpg'   ：格式化文件名，如 000.jpg、001.jpg
         *   - <a> 标签       ：点击图片后打开原图（新窗口）
         *   - <img> 标签     ：显示缩略图
         *   - appendChild()  ：把图片插入到网页的 #g 容器中
         *
         * 最终 HTML 效果：
         *   <a href="000.jpg" target="_blank"><img src="000.jpg"></a>
         */
        int n = snprintf(line, sizeof(line),
                         "(()=>{const n='%03d.jpg';"
                         "const a=document.createElement('a');"
                         "a.href=n;a.target='_blank';a.title=n;"
                         "const img=new Image();"
                         "img.src=n;img.alt=n;"
                         "a.appendChild(img);"
                         "g.appendChild(a);})();\n", i);

        // 将生成的 JS 代码写入 HTML 文件
        write(fd, line, n);
    }

    // 写入 HTML 尾部内容（结束 script 标签）
    write(fd, tail, strlen(tail));

    // 关闭文件句柄
    close(fd);
}

// ----------------- GPIO 控制 -----------------

/**
 * @brief 向指定 GPIO 引脚写入电平值（高/低）
 *
 * 功能：
 *   - 通过 sysfs 接口 `/sys/class/gpio/gpioX/value` 向内核导出的 GPIO 引脚写值
 *   - 常用于控制线圈上电/断电
 *
 * @param pin   GPIO 引脚编号（例如 32、33）
 * @param value 输出电平值（0=低电平，1=高电平）
 *
 * 调用前提：
 *   - 该 GPIO 必须已经被导出（/sys/class/gpio/export）
 *   - 并设置为输出模式（direction=out）
 *
 * 示例：
 *   gpio_write(33, 1);   // 设置 GPIO33 输出高电平
 *   gpio_write(33, 0);   // 设置 GPIO33 输出低电平
 */
static void gpio_write(int pin, int value){
    // 1️⃣ 构造 GPIO 对应的 value 文件路径，如：
    //    "/sys/class/gpio/gpio33/value"
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);

    // 2️⃣ 打开该文件，模式为写（"w"）
    FILE *f = fopen(path, "w");

    // 3️⃣ 若打开失败，打印错误并返回
    //    通常是因为未导出该 GPIO 或权限不足
    if (!f) {
        perror("gpio_write");
        return;
    }

    // 4️⃣ 将电平值写入文件
    //    内核驱动会自动将其映射为硬件电平输出
    fprintf(f, "%d", value);

    // 5️⃣ 写入完成后关闭文件
    fclose(f);
}

/**
 * @brief 初始化指定 GPIO 引脚为输出模式，并写入初始电平值
 *
 * 功能：
 *   - 将目标引脚导出到用户空间（/sys/class/gpio/gpioX）
 *   - 设置为输出方向（direction=out）
 *   - 写入初始电平（高/低）
 *
 * @param pin       GPIO 引脚编号（如 32、33）
 * @param init_val  初始电平值（0=低电平，1=高电平）
 *
 * 示例：
 *   gpio_init_out(33, 0);  // 导出 GPIO33，并设置为输出低电平
 *   gpio_init_out(32, 1);  // 导出 GPIO32，并设置为输出高电平
 *
 * 典型用途：
 *   - 控制线圈、继电器、LED 等的开关
 *   - 系统启动时设置默认电平状态，防止误触发
 */
static void gpio_init_out(int pin, int init_val){
    // 1️⃣ 导出 GPIO 引脚，使其在 /sys/class/gpio/ 下可见
    //    写入 pin 编号到 "/sys/class/gpio/export" 文件即可完成导出
    FILE *f = fopen("/sys/class/gpio/export", "w");
    if (f) {
        fprintf(f, "%d", pin);  // 写入引脚编号
        fclose(f);
    }
    // ⚠️ 若该 GPIO 已被导出，则此步骤可能返回失败（忽略即可）

    // 2️⃣ 构造 direction 文件路径，例如：
    //    "/sys/class/gpio/gpio33/direction"
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);

    // 3️⃣ 打开方向配置文件
    f = fopen(path, "w");
    if (!f) {
        perror("gpio direction");  // 打印错误信息（可能是权限问题或未导出）
        exit(1);                   // 致命错误时退出程序
    }

    // 4️⃣ 设置方向为 “out”（输出模式）
    fprintf(f, "out");
    fclose(f);

    // 5️⃣ 调用 gpio_write()，设置引脚初始电平（高/低）
    gpio_write(pin, init_val);
}

// ----------------- 启动 mjpg_streamer HTTP 服务 -----------------

/**
 * @brief 等待 HTTP 服务端口（如 mjpg_streamer）启动就绪
 *
 * 功能：
 *   - 不断尝试连接指定的 host:port
 *   - 若连接成功，说明 HTTP 服务已启动
 *   - 若在超时时间内未连通，则认为启动失败
 *
 * @param host        主机地址（如 "127.0.0.1"）
 * @param port        端口号（如 8080）
 * @param timeout_ms  超时时间（单位：毫秒）
 * @return 0 表示 HTTP 服务可用，-1 表示超时未就绪
 *
 * 示例：
 *   if (wait_http_ready("127.0.0.1", 8080, 5000) == 0)
 *       printf("HTTP ready!\n");
 *   else
 *       printf("Timeout: mjpg_streamer not responding.\n");
 */
static int wait_http_ready(const char *host, int port, int timeout_ms) {
    int elapsed = 0;  // 已等待的时间（毫秒）

    // 在超时时间内循环检测端口是否能连通
    while (elapsed < timeout_ms) {

        // 1️⃣ 创建一个 TCP 套接字
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {

            // 2️⃣ 填充服务器地址结构
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;        // IPv4
            a.sin_port = htons(port);      // 设置端口（网络字节序）

            // 3️⃣ 将字符串 IP 转换为网络地址
            if (inet_pton(AF_INET, host, &a.sin_addr) == 1) {

                // 4️⃣ 尝试连接服务器
                if (connect(sock, (struct sockaddr*)&a, sizeof(a)) == 0) {
                    // ✅ 若连接成功，说明 HTTP 服务已启动
                    close(sock);
                    return 0; // HTTP ready
                }
            }

            // 5️⃣ 若连接失败，关闭 socket 并稍后重试
            close(sock);
        }

        // 6️⃣ 等待 100ms 再尝试（降低 CPU 占用）
        usleep(100 * 1000);
        elapsed += 100;  // 累计等待时间
    }

    // 超时未能连接，返回失败
    return -1;
}

/**
 * @brief 杀掉系统中已存在的 mjpg_streamer 进程，避免端口冲突
 *
 * 功能：
 *   - 调用 shell 命令 `killall -q mjpg_streamer` 终止旧实例
 *   - 若不执行该操作，重新启动 mjpg_streamer 可能因端口（8080）占用而失败
 *
 * 说明：
 *   - `killall` 根据进程名结束进程
 *   - `-q` 参数表示安静模式，不输出提示
 *   - `2>/dev/null` 将错误输出重定向到空设备，避免在控制台打印多余信息
 *
 * 调用场景：
 *   - 每次重新启动 mjpg_streamer 前调用
 *   - 例如在 `start_mjpg_streamer()` 内部自动执行
 *
 * 示例：
 *   kill_old_http();   // 清理旧的 mjpg_streamer 实例
 *   start_mjpg_streamer(); // 启动新的 HTTP 服务
 */
static void kill_old_http(void){
    // 调用 Linux shell 命令结束 mjpg_streamer 进程
    // 若没有正在运行的进程，则不会输出任何错误信息
    system("killall -q mjpg_streamer 2>/dev/null");
}

/**
 * @brief 启动 mjpg_streamer 视频推流服务（基于 HTTP 输出）
 *
 * 功能：
 *   - 启动独立的 mjpg_streamer 进程，将 /dev/video0 摄像头采集的图像通过 HTTP 推流
 *   - 典型推流地址为：http://<BOARD_IP>:8080/?action=stream
 *   - 启动后检测 HTTP 端口是否可用（最多等待 5 秒）
 *
 * @return 子进程 PID（>0 表示成功），-1 表示 fork 失败
 *
 * 工作流程：
 *   1️⃣ 杀掉旧的 mjpg_streamer 实例，防止端口冲突
 *   2️⃣ 使用 fork() 创建子进程
 *   3️⃣ 子进程调用 execl() 执行 mjpg_streamer 程序
 *   4️⃣ 父进程等待 HTTP 服务端口启动成功（通过 wait_http_ready() 检测）
 *
 * 示例：
 *   pid_t pid = start_mjpg_streamer();
 *   if (pid > 0)
 *       printf("mjpg_streamer started, pid=%d\n", pid);
 */
static pid_t start_mjpg_streamer(void) {
    // 1️⃣ 清理旧实例，防止端口（8080）被占用
    kill_old_http();

    // 2️⃣ 创建子进程
    pid_t pid = fork();

    // ---------------- 子进程逻辑 ----------------
    if (pid == 0) {
        // 切换工作目录到 mjpg_streamer 主目录
        // 该目录下应包含 input_uvc.so、output_http.so、www 文件夹
        chdir(MJPG_HOME);

        // 调用 execl() 启动 mjpg_streamer
        // 参数说明：
        //   - "-i" 指定输入插件（USB 摄像头采集模块）
        //   - "-d /dev/video0" 表示使用主摄像头设备节点
        //   - "-r 640x480" 设置分辨率
        //   - "-f 15" 设置帧率（FPS）
        //   - "-o" 指定输出插件（HTTP 推流模块）
        //   - "-p 8080" 设置 HTTP 服务端口号
        //   - "-w ./www" 指定网页根目录（用于展示）
        execl("./mjpg_streamer", "./mjpg_streamer",
              "-i", "./input_uvc.so -d /dev/video0 -r 640x480 -f 15", // 输入模块参数
              "-o", "./output_http.so -p 8080 -w ./www",              // 输出模块参数
              (char*)NULL);

        // 若 execl 执行失败则打印错误并退出
        perror("exec mjpg_streamer");
        _exit(127); // 使用 _exit 避免执行父进程的清理逻辑
    }

        // ---------------- fork 失败 ----------------
    else if (pid < 0) {
        perror("fork mjpg_streamer"); // 打印错误信息
        return -1;                    // 返回失败
    }

    // ---------------- 父进程逻辑 ----------------
    // 等待 HTTP 服务端口就绪（最多等待 5 秒）
    if (wait_http_ready("127.0.0.1", HTTP_PORT, 5000) != 0) {
        fprintf(stderr, "ERROR: http on 8080 not ready\n");
    } else {
        printf("HTTP ready on 8080\n");
    }

    // 返回子进程 PID
    return pid;
}

// ----------------- HTTP 抓帧函数 -----------------

/**
 * @brief 从 mjpg_streamer 的 HTTP 服务器抓取单帧图像并保存为 JPEG 文件
 *
 * 功能：
 *   - 通过 HTTP/1.0 协议发送 GET 请求，例如：
 *       GET /?action=snapshot HTTP/1.0
 *   - 读取返回的 HTTP 响应，解析响应头（Content-Length）
 *   - 将 JPEG 数据体保存为指定路径文件
 *
 * 典型用途：
 *   http_get_snapshot_save("127.0.0.1", 8080, "/?action=snapshot", "/root/mjpg/www/001.jpg");
 *
 * @param host       HTTP 服务主机名（一般为 "127.0.0.1"）
 * @param port       HTTP 服务端口号（一般为 8080）
 * @param path       请求路径（例如 "/?action=snapshot"）
 * @param out_path   输出文件路径（例如 "/root/mjpg/www/001.jpg"）
 * @return           0 表示成功，-1 表示失败
 *
 * 调用关系：
 *   capture_snapshots() → http_get_snapshot_save()
 */
static int http_get_snapshot_save(const char* host, int port, const char* path, const char* out_path) {

    // 1️⃣ 建立 TCP 套接字，用于连接 mjpg_streamer HTTP 服务
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    // 2️⃣ 填充服务器地址信息
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;            // 使用 IPv4
    addr.sin_port = htons(port);           // 设置端口号（转为网络字节序）

    // 将主机字符串（如 "127.0.0.1"）转换为二进制地址
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    // 3️⃣ 尝试连接 mjpg_streamer 的 HTTP 端口
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    // 4️⃣ 发送 HTTP GET 请求
    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n",
                      path, host);
    if (write(sock, req, rl) != rl) {
        close(sock);
        return -1;
    }

    // 5️⃣ 开始读取 HTTP 响应数据（包括头部与内容）
    char header[8192];        // 存放 HTTP 头部的缓冲区
    size_t hpos = 0;          // 当前头部写入位置
    int header_done = 0;      // 标志：是否解析完 HTTP 头
    ssize_t n;
    char buf[4096];           // 通用数据缓冲区
    size_t content_length = 0; // 内容长度（若 HTTP 响应带 Content-Length）
    int have_len = 0;          // 是否已读取到 Content-Length

    // 6️⃣ 读取循环，直到检测到 HTTP 头结束标志 “\r\n\r\n”
    while (!header_done && (n = read(sock, buf, sizeof(buf))) > 0) {
        size_t copy = (size_t)n;
        if (hpos + copy > sizeof(header)) copy = sizeof(header) - hpos;
        memcpy(header + hpos, buf, copy);
        hpos += copy;

        // 扫描是否已到达头部结束
        for (size_t i = 3; i < hpos; i++) {
            if (header[i-3]=='\r' && header[i-2]=='\n' &&
                header[i-1]=='\r' && header[i]=='\n') {
                header_done = 1;   // 检测到 \r\n\r\n —— HTTP 头结束
                header[i+1] = '\0'; // 字符串结尾符

                // 解析 Content-Length 字段（如果存在）
                char *p = strcasestr(header, "Content-Length:");
                if (p) {
                    p += 15; // 跳过关键字
                    while (*p == ' ' || *p == '\t') ++p;
                    content_length = (size_t)strtoull(p, NULL, 10);
                    have_len = 1;
                }

                // header_len: 头部长度；body0: 第一包中数据体的起始位置
                size_t header_len = i + 1;
                size_t body0 = hpos - header_len;

                // 打开输出文件
                FILE *fp = fopen(out_path, "wb");
                if (!fp) { close(sock); return -1; }

                // 7️⃣ 写入第一包中可能包含的图像数据部分
                if (body0) {
                    size_t w = body0;
                    if (have_len && w > content_length) w = content_length;
                    fwrite(header + header_len, 1, w, fp);
                    if (have_len) content_length -= w;
                }

                // 8️⃣ 若已知内容长度，则按长度继续读取剩余数据
                if (have_len) {
                    while (content_length > 0) {
                        n = read(sock, buf, sizeof(buf));
                        if (n <= 0) break;
                        size_t w = (size_t)n;
                        if (w > content_length) w = content_length;
                        fwrite(buf, 1, w, fp);
                        content_length -= w;
                    }
                }
                    // 若未提供 Content-Length，则一直读到 EOF
                else {
                    while ((n = read(sock, buf, sizeof(buf))) > 0)
                        fwrite(buf, 1, (size_t)n, fp);
                }

                // 9️⃣ 完成写入与资源释放
                fclose(fp);
                close(sock);
                return 0; // 成功保存一帧
            }
        }

        // 若头部过大（异常情况），中止读取
        if (hpos == sizeof(header)) {
            close(sock);
            return -1;
        }
    }

    // 若循环结束仍未检测到 HTTP 头，返回错误
    close(sock);
    return -1;
}

/**
 * @brief 批量抓帧函数：在指定持续时间内连续从 HTTP 服务抓取图像帧
 *
 * 功能：
 *   - 在指定的时间段内（单位：微秒）循环调用 http_get_snapshot_save()
 *   - 按顺序保存为 /root/mjpg/www/000.jpg、001.jpg 等文件
 *   - 每帧之间按目标帧率（TARGET_FPS）间隔采样
 *   - 抓取结束后自动更新网页 index.html 文件
 *
 * 参数：
 *   @param duration_us —— 抓取时长（微秒），例如 500000 表示持续 0.5 秒
 *
 * 返回：
 *   @return 成功抓取的帧数
 *
 * 调用关系：
 *   main() → capture_snapshots() → http_get_snapshot_save()
 *
 * 举例：
 *   capture_snapshots(500000);  // 连续抓取 0.5 秒的图像 (~7~8 帧)
 */
static int capture_snapshots(useconds_t duration_us) {
    int saved = 0;                      // 当前抓取帧数计数器
    struct timespec ts0, ts1;           // 用于时间测量
    clock_gettime(CLOCK_MONOTONIC, &ts0); // 获取抓取起始时间（单调时钟）

    while (1) {
        // ① 构造文件名，例如：000.jpg、001.jpg ...
        char name[256];
        snprintf(name, sizeof(name), CAPS_DIR "/%03d.jpg", g_total_saved);

        // ② 从 HTTP 抓取一帧图像并保存
        if (http_get_snapshot_save("127.0.0.1", HTTP_PORT, "/?action=snapshot", name) == 0) {
            ++saved;         // 成功抓取一帧
            ++g_total_saved; // 全局计数器累加
        }

        // ③ 计算当前时间与起始时间的差值（单位：微秒）
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        long long el = (ts1.tv_sec - ts0.tv_sec) * 1000000LL +
                       (ts1.tv_nsec - ts0.tv_nsec) / 1000LL;

        // 若已达到指定持续时间则退出循环
        if (el >= (long long)duration_us)
            break;

        // ④ 控制帧率（休眠间隔）
        // SNAPSHOT_INTERVAL_US = 1000000 / TARGET_FPS，例如 15fps → 间隔约 66ms
        usleep(SNAPSHOT_INTERVAL_US);
    }

    // ⑤ 抓取完毕后更新 index.html，便于网页展示所有帧
    write_index_html(g_total_saved);

    // 返回本次抓取的帧数
    return saved;
}

// ----------------- 主程序入口 -----------------
/**
 * @brief 程序主入口：基于 ADC 电压触发拍照与推流
 *
 * 功能概述：
 *   - 初始化目录与网页文件
 *   - 启动 mjpg_streamer 视频推流服务
 *   - 读取 ADC 电压信号，当电压超过阈值时触发自动拍照
 *   - 抓取多帧图像（连续截图）保存至 /root/mjpg/www 目录
 *   - 通过 GPIO 控制线圈通断，模拟打击或采样动作
 *   - 抓取结果可通过浏览器访问 mjpg_streamer 的 HTTP 端口查看
 *
 * 核心逻辑：
 *   1️⃣ 系统初始化（目录、HTTP、ADC、GPIO）
 *   2️⃣ ADC 实时采样电压
 *   3️⃣ 达到电压门限 → 自动触发图像抓取
 *   4️⃣ GPIO 执行通断动作
 *   5️⃣ 抓取结果可网页实时预览
 */
int main(void) {

    // ----------------- 1️⃣ 准备工作目录 -----------------
    // 创建网页目录（/root/mjpg/www），并清空旧文件
    mkdir_p(CAPS_DIR, 0755);
    rm_caps_dir_contents();
    g_total_saved = 0;
    write_index_html(0); // 生成初始 index.html（空页面）

    // ----------------- 2️⃣ 启动视频推流服务 -----------------
    pid_t mjpg_pid = start_mjpg_streamer();
    if (mjpg_pid < 0)
        fprintf(stderr, "WARN: mjpg_streamer start failed\n");
    // 成功后，HTTP 服务运行于 http://<BOARD_IP>:8080

    // ----------------- 3️⃣ 打开 ADC 通道文件 -----------------
    // /sys/bus/iio/devices/iio:device0/in_voltage1_raw   —— ADC 原始值
    // /sys/bus/iio/devices/iio:device0/in_voltage_scale —— ADC 转换比例
    int fd_raw = open("/sys/bus/iio/devices/iio:device0/in_voltage1_raw", O_RDONLY);
    int fd_scale = open("/sys/bus/iio/devices/iio:device0/in_voltage_scale", O_RDONLY);
    if (fd_raw < 0 || fd_scale < 0) {
        perror("open ADC");
        return -1;
    }

    // ----------------- 4️⃣ 读取 ADC 电压转换比例 -----------------
    char buf[32];
    float scale = 1.0f; // ADC 单位比例因子（mV/bit）
    lseek(fd_scale, 0, SEEK_SET);
    int len = read(fd_scale, buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        scale = strtof(buf, NULL); // 读取到的 scale 值通常为 0.732 等
    } else {
        perror("read scale");
    }

    // ----------------- 5️⃣ 初始化 GPIO 输出 -----------------
    // GPIO_A = 33, GPIO_B = 32
    // 控制线圈上电与断电：A 控制正向上电，B 控制反向或关闭
    gpio_init_out(GPIO_A, 0); // 初始为低电平（关）
    gpio_init_out(GPIO_B, 1); // 初始为高电平（稳态）

    // ----------------- 6️⃣ 主循环：ADC 电压监测与触发逻辑 -----------------
    int count = 0; // 连续检测计数器

    while (1) {
        // ① 读取 ADC 原始值
        lseek(fd_raw, 0, SEEK_SET);
        int rlen = read(fd_raw, buf, sizeof(buf) - 1);
        if (rlen > 0) {
            buf[rlen] = '\0';
            int raw = atoi(buf); // 转换为整数原始值
            float voltage = (raw * scale) / 1000.0f; // 转换为电压值（V）
            printf("Raw=%d Voltage=%.6f V\n", raw, voltage);

            // ② 判断是否超过阈值
            if (voltage > VOLTAGE_THRESH) {
                if (++count >= TRIGGER_COUNT) {
                    // 触发拍照逻辑
                    printf("[Trigger] capture %.3fs into %s ...\n",
                           HOLD_TIME_US / 1e6, CAPS_DIR);

                    // 连续抓取多帧图像
                    int saved = capture_snapshots(HOLD_TIME_US);
                    printf("Captured %d frames. Total=%d. "
                           "Browse: http://<BOARD_IP>:%d/caps/index.html\n",
                           saved, g_total_saved, HTTP_PORT);

                    // ③ 执行 GPIO 控制时序（线圈通断）
                    // 先断开 GPIO_B，再上电 GPIO_A
                    gpio_write(GPIO_B, 0);
                    gpio_write(GPIO_A, 1);
                    sleep(HOLD_TIME_SEC); // 保持上电一段时间
                    gpio_write(GPIO_A, 0);
                    gpio_write(GPIO_B, 1);

                    count = 0; // 重置计数器
                }
            } else {
                count = 0; // 未触发则清零
            }
        } else {
            perror("read raw");
        }

        // ④ 控制 ADC 采样间隔（例如 5000us = 5ms）
        usleep(SAMPLE_US);
    }

    // ----------------- 7️⃣ 程序收尾 -----------------
    close(fd_raw);
    close(fd_scale);
    return 0;
}

