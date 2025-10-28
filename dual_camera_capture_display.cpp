/*
 * ================================================================
 * 文件名: dual_camera_capture_display.c
 * 功能概述:
 *   本程序运行于 RV1106 Linux 平台，用于实现双摄像头图像采集、
 *   实时显示与按键拍照功能。两个摄像头的图像以左右分屏方式
 *   显示在 800×480 LCD 屏幕上，按下指定按键后同步保存为 JPEG 文件。
 *
 * 系统组成:
 *   ├── 主控芯片 : RV1106 (ARM Cortex-A7)
 *   ├── 左摄像头 : /dev/video21
 *   ├── 右摄像头 : /dev/video23
 *   ├── LCD显示屏 : /dev/fb0  (800×480 RGB接口)
 *   ├── 按键输入 : /dev/input/event1
 *   ├── 左图保存路径 : /root/left/
 *   ├── 右图保存路径 : /root/right/
 *
 * 编译命令:
 *   arm-rockchip830-linux-uclibcgnueabihf-gcc dual_camera_capture_display.c -o dual_camera_capture_display -lpthread -ljpeg
 *
 * 运行说明:
 *   1. 程序启动后自动清空 /root/left 与 /root/right 下的旧照片；
 *   2. 两路摄像头画面实时显示于LCD左右两半；
 *   3. 按下按键（event1设备）时同时保存左右图像为 JPEG；
 *   4. 文件名自动编号，例如 /root/left/0.jpg、/root/right/0.jpg；
 *   5. 程序循环运行，可连续拍摄。
 *
 * ================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <jpeglib.h>

// ======================== 宏定义区 ==============================
#define WIDTH 640                     // 摄像头图像采集宽度
#define HEIGHT 480                    // 摄像头图像采集高度
#define LEFT_FOLDER "/root/left"      // 左摄像头照片保存路径
#define RIGHT_FOLDER "/root/right"    // 右摄像头照片保存路径
#define INPUT_DEVICE "/dev/input/event1"  // 按键输入设备路径
#define CAM_LEFT  "/dev/video21"      // 左摄像头设备节点
#define CAM_RIGHT "/dev/video23"      // 右摄像头设备节点

// 拍照标志位（全局变量，由按键线程设置，主循环清零）
volatile int photo_flag = 0;

// ======================== 结构体定义 ==============================
struct buffer {
    void *start;                      // 映射内存起始地址
    size_t length;                    // 映射内存长度
};

// ======================== 函数声明区 ==============================
void yuyv_to_jpeg(void *yuyv, int width, int height, const char *filename);
void clear_jpg_files(const char *folder);
int init_camera(const char *dev, struct buffer *bufs, int *vfd);
void draw_on_lcd(unsigned int *fb, int stride, int fb_w, int fb_h, void *yuyv, int x_offset);
void *event_listener(void *arg);

// ======================== 函数实现 ==============================

/**
 * 函数名称: yuyv_to_jpeg
 * 功能描述:
 *   将摄像头采集到的 YUYV 格式原始帧数据转换为 JPEG 格式并保存至文件。
 *
 * 输入参数:
 *   yuyv     - 指向原始 YUYV 格式图像缓冲区的指针
 *   width    - 图像宽度（像素）
 *   height   - 图像高度（像素）
 *   filename - JPEG 文件保存路径（例如 "/root/left/0.jpg"）
 *
 * 实现原理:
 *   1. 摄像头采集到的原始帧是 YUYV 格式（每两个像素共 4 字节）：
 *      字节序为：Y0 U Y1 V
 *      表示第1个像素(Y0,U,V)，第2个像素(Y1,U,V) 共用同一对U、V分量。
 *   2. 本函数将 YUYV 转换为 RGB 三通道数据（每像素3字节）。
 *   3. 使用 libjpeg 库进行压缩编码，并输出为 JPEG 文件。
 *   4. 最后释放动态内存并关闭文件。
 *
 * 注意事项:
 *   - JPEG 压缩库需链接 -ljpeg；
 *   - 输入缓冲必须完整且按帧对齐；
 *   - 函数内部使用 malloc 分配 RGB 临时缓存，处理后自动释放；
 *   - 适用于单帧静态图像保存。
 */
void yuyv_to_jpeg(void *yuyv, int width, int height, const char *filename) {
    // ----------- 输入有效性检查 -----------
    if (!yuyv) return;                                    // 若输入为空指针，直接返回（防止段错误）

    // ----------- 变量初始化与内存分配 -----------
    unsigned char *p = (unsigned char *)yuyv;             // 指向输入的 YUYV 数据
    unsigned char *rgb = malloc(width * height * 3);      // 分配 RGB 格式缓冲区 (3字节/像素)
    if (!rgb) return;                                     // 若内存分配失败直接返回
    unsigned char *out = rgb;                             // 输出指针，用于逐字节写入 RGB 数据

    // ----------- YUYV 转 RGB -----------
    // 每 4 个字节代表两个像素，遍历整个帧缓冲
    for (int i = 0; i < width * height * 2; i += 4) {
        int y0 = p[i];                                    // 第1像素亮度Y0
        int u  = p[i+1];                                  // 共享色度分量U
        int y1 = p[i+2];                                  // 第2像素亮度Y1
        int v  = p[i+3];                                  // 共享色度分量V
        int r, g, b;                                      // 临时存放RGB结果
        int c, d, e;                                      // 色度计算中间变量

        // ================= 第1个像素 =================
        c = y0 - 16;                                      // 去除亮度偏移
        d = u - 128;                                      // 去除蓝色色度偏移
        e = v - 128;                                      // 去除红色色度偏移

        // 按ITU-R BT.601标准公式计算RGB分量
        r = (298 * c + 409 * e + 128) >> 8;               // R = 1.164*(Y-16) + 1.596*(V-128)
        g = (298 * c - 100 * d - 208 * e + 128) >> 8;     // G = 1.164*(Y-16) - 0.392*(U-128) - 0.813*(V-128)
        b = (298 * c + 516 * d + 128) >> 8;               // B = 1.164*(Y-16) + 2.017*(U-128)

        // RGB分量裁剪到[0,255]范围，防止溢出
        *out++ = (r > 255) ? 255 : (r < 0 ? 0 : r);
        *out++ = (g > 255) ? 255 : (g < 0 ? 0 : g);
        *out++ = (b > 255) ? 255 : (b < 0 ? 0 : b);

        // ================= 第2个像素 =================
        // 第2个像素共用相同的U、V分量，只更换Y1
        c = y1 - 16;
        r = (298 * c + 409 * e + 128) >> 8;
        g = (298 * c - 100 * d - 208 * e + 128) >> 8;
        b = (298 * c + 516 * d + 128) >> 8;

        // 同样裁剪并存储RGB值
        *out++ = (r > 255) ? 255 : (r < 0 ? 0 : r);
        *out++ = (g > 255) ? 255 : (g < 0 ? 0 : g);
        *out++ = (b > 255) ? 255 : (b < 0 ? 0 : b);
    }

    // ----------- 打开文件准备输出JPEG -----------
    FILE *outf = fopen(filename, "wb");                   // 以二进制方式写入新文件
    if (!outf) {                                          // 文件打开失败
        perror("fopen");
        free(rgb);
        return;
    }

    // ----------- 初始化 JPEG 压缩结构体 -----------
    struct jpeg_compress_struct cinfo;                    // 主控制结构
    struct jpeg_error_mgr jerr;                           // 错误处理结构
    cinfo.err = jpeg_std_error(&jerr);                    // 初始化错误处理
    jpeg_create_compress(&cinfo);                         // 创建压缩对象

    // ----------- 绑定输出目标文件 -----------
    jpeg_stdio_dest(&cinfo, outf);                        // 指定输出文件为压缩目标

    // ----------- 设置JPEG图像参数 -----------
    cinfo.image_width = width;                            // 图像宽度
    cinfo.image_height = height;                          // 图像高度
    cinfo.input_components = 3;                           // 每个像素3个通道（RGB）
    cinfo.in_color_space = JCS_RGB;                       // 指定输入色彩空间
    jpeg_set_defaults(&cinfo);                            // 使用默认压缩参数（质量约75%）
    jpeg_start_compress(&cinfo, TRUE);                    // 开始压缩过程

    // ----------- 逐行写入RGB数据 -----------
    // libjpeg按扫描线（行）方式写入，每次调用写入1行像素
    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < height) {
        row_pointer[0] = &rgb[cinfo.next_scanline * width * 3];  // 定位当前行起始地址
        jpeg_write_scanlines(&cinfo, row_pointer, 1);             // 写入一行数据
    }

    // ----------- 压缩完成，清理资源 -----------
    jpeg_finish_compress(&cinfo);                       // 结束压缩流程
    fclose(outf);                                       // 关闭文件流
    jpeg_destroy_compress(&cinfo);                      // 释放JPEG结构体资源
    free(rgb);                                          // 释放临时RGB缓冲区

    // 至此，一帧YUYV图像已成功保存为JPEG文件
}

/**
 * 函数名称: clear_jpg_files
 * 功能描述:
 *   检查指定目录是否存在：
 *     - 若不存在，则创建该目录；
 *     - 若存在且为目录，则清空其中所有扩展名为 ".jpg" 的文件；
 *     - 若存在但不是目录（例如是一个同名文件），则打印错误信息并退出。
 *
 * 使用场景:
 *   程序启动时调用，用于保证 /root/left 和 /root/right 目录干净，
 *   防止新拍摄的照片与旧照片混淆。
 *
 * 输入参数:
 *   folder - 目标文件夹路径（如 "/root/left" 或 "/root/right"）
 *
 * 实现逻辑步骤:
 *   1. 使用 stat() 检查路径是否存在；
 *   2. 若不存在则 mkdir() 创建；
 *   3. 若存在但不是目录则报错；
 *   4. 若为目录则使用 opendir() + readdir() 遍历其中所有文件；
 *   5. 逐一判断文件名是否包含 ".jpg" 后缀；
 *   6. 匹配成功则调用 remove() 删除该文件；
 *   7. 最后打印清理数量统计信息。
 */
void clear_jpg_files(const char *folder) {
    struct stat st;

    // ---------- 1. 检查目录是否存在 ----------
    // stat() 函数用于获取文件或目录的状态信息。
    // 返回值：0 表示存在；-1 表示不存在或出错。
    if (stat(folder, &st) != 0) {
        // 若目录不存在，则尝试创建
        if (mkdir(folder, 0755) == 0) {                     // 权限：rwxr-xr-x
            printf("Created folder: %s\n", folder);         // 打印提示：目录已创建
        } else {
            perror("mkdir failed");                         // 若创建失败，输出错误原因
        }
        return;                                             // 创建后直接返回（新建目录为空，无需清理）
    }

    // ---------- 2. 检查路径类型 ----------
    // S_ISDIR() 宏用于判断路径是否为目录类型。
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s exists but is not a directory!\n", folder);
        return;                                             // 若为普通文件则终止执行，避免误删
    }

    // ---------- 3. 打开目录并准备遍历 ----------
    DIR *dir = opendir(folder);                             // 打开目录，返回DIR指针
    if (!dir) {                                             // 若打开失败
        perror("opendir failed");                           // 打印错误信息
        return;
    }

    struct dirent *entry;                                   // dirent结构体保存目录项信息
    char path[512];                                         // 存放完整路径名的缓冲区
    int count = 0;                                          // 统计已删除文件数量

    // ---------- 4. 遍历目录 ----------
    // readdir() 每调用一次返回一个目录项指针，直到返回 NULL。
    while ((entry = readdir(dir)) != NULL) {
        // 忽略当前目录"." 和上级目录".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // ---------- 5. 判断文件扩展名 ----------
        // strstr() 判断文件名中是否包含 ".jpg" 子串
        if (strstr(entry->d_name, ".jpg")) {
            // ---------- 6. 拼接完整路径并删除 ----------
            snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
            if (remove(path) == 0) {
                count++;                                    // 删除成功则计数+1
            } else {
                perror("remove failed");                    // 删除失败打印错误
            }
        }
    }

    // ---------- 7. 清理与统计 ----------
    closedir(dir);                                          // 关闭目录句柄，释放资源
    printf("Cleared %d .jpg files in %s\n", count, folder); // 输出清理结果
}

/**
 * 函数名称: init_camera
 * 功能描述:
 *   初始化指定摄像头设备，包括：
 *     1. 打开 /dev/videoX 节点；
 *     2. 设置视频格式（分辨率、像素格式等）；
 *     3. 申请帧缓冲并进行内存映射；
 *     4. 启动视频流采集；
 *   最终返回缓冲区数量，并通过参数输出文件描述符。
 *
 * 输入参数:
 *   dev  - 摄像头设备节点路径，如 "/dev/video21" 或 "/dev/video23"
 *   bufs - 用户自定义 buffer 数组，用于存储 mmap 映射信息（起始地址 + 长度）
 *   vfd  - 指向整型变量的指针，用于返回打开的摄像头文件描述符
 *
 * 返回值:
 *   成功时返回缓冲区数量（通常为2），出错时程序退出。
 *
 * 依赖:
 *   <linux/videodev2.h>  —— V4L2 视频接口定义
 *   <sys/ioctl.h>        —— ioctl 系统调用
 *   <sys/mman.h>         —— mmap 内存映射函数
 *
 * 注意事项:
 *   - 摄像头必须支持 YUYV 格式，否则需调整 pixelformat；
 *   - 若 ioctl() 返回 -1，通常为驱动不支持或设备未连接；
 *   - 必须在 STREAMON 之后，才能从摄像头读取帧。
 */
int init_camera(const char *dev, struct buffer *bufs, int *vfd) {
    // ---------------------- 1. 打开摄像头设备 ----------------------
    *vfd = open(dev, O_RDWR);                                // 以读写模式打开设备节点
    if (*vfd < 0) {                                          // 若打开失败，输出错误并终止
        perror("open camera");
        exit(1);
    }

    // ---------------------- 2. 设置视频格式 ----------------------
    // 定义 v4l2_format 结构体并指定视频捕获模式参数
    struct v4l2_format fmt = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,                 // 指定为视频捕获类型
            .fmt.pix.width = WIDTH,                              // 图像宽度 (640)
            .fmt.pix.height = HEIGHT,                            // 图像高度 (480)
            .fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV,            // 像素格式：YUYV（常见于UVC摄像头）
            .fmt.pix.field = V4L2_FIELD_NONE                     // 非交错扫描
    };

    // 向驱动发送 VIDIOC_S_FMT 命令，设置视频采集格式
    if (ioctl(*vfd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT failed");                       // 例如设备不支持指定分辨率或格式
        exit(1);
    }

    // ---------------------- 3. 申请视频缓冲区 ----------------------
    // 通常摄像头驱动使用环形缓冲机制（循环采集帧），此处申请2个缓冲区
    struct v4l2_requestbuffers req = {
            .count = 2,                                          // 请求分配2个缓冲区
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,                 // 视频捕获类型
            .memory = V4L2_MEMORY_MMAP                           // 使用内存映射模式（效率高）
    };

    // VIDIOC_REQBUFS 命令申请缓冲区，驱动分配 DMA 内存供采集使用
    if (ioctl(*vfd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS failed");
        exit(1);
    }

    // ---------------------- 4. 映射缓冲区到用户空间 ----------------------
    // 驱动将每个 buffer 映射到进程虚拟地址，使得应用程序可直接访问帧数据
    for (int i = 0; i < 2; ++i) {
        // 查询第 i 个缓冲区的偏移量与大小
        struct v4l2_buffer buf = {
                .type = req.type,
                .memory = req.memory,
                .index = i
        };

        if (ioctl(*vfd, VIDIOC_QUERYBUF, &buf) < 0) {        // 获取缓冲区属性
            perror("VIDIOC_QUERYBUF failed");
            exit(1);
        }

        // 记录缓冲区长度
        bufs[i].length = buf.length;

        // 建立内存映射（内核空间 → 用户空间）
        bufs[i].start = mmap(
                NULL,                                             // 让内核自动分配虚拟地址
                buf.length,                                       // 映射长度
                PROT_READ | PROT_WRITE,                           // 允许读写
                MAP_SHARED,                                       // 与内核共享
                *vfd,                                             // 设备文件描述符
                buf.m.offset                                      // 内核提供的偏移地址
        );

        if (bufs[i].start == MAP_FAILED) {
            perror("mmap failed");
            exit(1);
        }

        // 将映射好的缓冲区重新放入输入队列，等待驱动采集数据填充
        if (ioctl(*vfd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF failed");
            exit(1);
        }
    }

    // ---------------------- 5. 启动视频流采集 ----------------------
    enum v4l2_buf_type type = req.type;                      // 缓冲区类型
    // VIDIOC_STREAMON 命令启动视频采集流
    if (ioctl(*vfd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON failed");                    // 若失败，通常是驱动未准备好
        exit(1);
    }

    // ---------------------- 6. 输出初始化结果 ----------------------
    printf("[OK] Camera %s initialized (width=%d, height=%d, buffers=%d)\n",
           dev, WIDTH, HEIGHT, req.count);

    // 返回成功申请的缓冲区数量（通常为2）
    return req.count;
}


/**
 * 函数名: draw_on_lcd
 * 功能描述:
 *   将摄像头输出的 YUYV 格式帧数据转换为 RGB888 格式，
 *   并绘制到 LCD 帧缓冲（framebuffer）上，用于实时预览。
 *   支持左右分屏显示（左、右摄像头各占半屏）。
 *
 * 输入参数:
 *   fb        - LCD帧缓冲首地址（/dev/fb0 映射得到的内存起始地址）
 *   stride    - 每行字节跨度（等于 finfo.line_length）
 *   fb_w      - LCD 屏幕宽度（例如 800）
 *   fb_h      - LCD 屏幕高度（例如 480）
 *   yuyv      - 摄像头采集的 YUYV 格式图像帧缓冲指针
 *   x_offset  - 横向偏移像素（用于分屏显示）
 *               例如: 左摄像头 x_offset = 0, 右摄像头 x_offset = fb_w / 2
 *
 * 工作原理:
 *   1. 每两个像素由4字节(Y0,U,Y1,V)组成，U/V分量共用；
 *   2. 函数计算目标显示尺寸(半屏)，并做比例缩放；
 *   3. 遍历每个目标像素点，根据映射位置取对应源像素；
 *   4. 按 YUV→RGB 标准公式转换；
 *   5. 拼装 ARGB8888 格式像素写入 framebuffer；
 *   6. LCD 控制器自动刷新显示。
 *
 * 注意事项:
 *   - framebuffer 通常是 32 位色深 (ARGB8888)，高字节为 Alpha 通道；
 *   - 左右分屏同时显示时，必须保证左右图像不会越界；
 *   - 若显示比例不一致，可调整 scale_x / scale_y；
 *   - LCD 的实际刷新频率由内核 framebuffer 驱动控制。
 */
void draw_on_lcd(unsigned int *fb, int stride, int fb_w, int fb_h, void *yuyv, int x_offset) {
    // ---------------------- 1. 计算目标显示尺寸 ----------------------
    int target_w = fb_w / 2;                        // 每个摄像头占屏幕宽度的一半
    int target_h = fb_h;                            // 全屏高度显示
    float scale_x = (float)WIDTH / target_w;        // 横向缩放比例 (例如 640/400 = 1.6)
    float scale_y = (float)HEIGHT / target_h;       // 纵向缩放比例 (例如 480/480 = 1.0)

    // ---------------------- 2. YUYV 数据指针初始化 ----------------------
    unsigned char *p = (unsigned char *)yuyv;       // 摄像头原始帧缓冲

    // ---------------------- 3. 按行扫描绘制 ----------------------
    for (int y = 0; y < target_h; y++) {
        // 计算当前 LCD 行在 framebuffer 中的首地址
        unsigned int *row = (unsigned int *)((unsigned char *)fb + y * stride);

        for (int x = 0; x < target_w; x++) {
            // ----------- 3.1 映射源像素坐标（带缩放） -----------
            int src_x = (int)(x * scale_x);          // 映射后的原图像 X 坐标
            int src_y = (int)(y * scale_y);          // 映射后的原图像 Y 坐标
            if (src_x % 2 != 0) src_x--;             // 保证偶数列，U/V 对齐正确（每2像素共享U/V）
            int idx = (src_y * WIDTH + src_x) * 2;   // 计算源图像中该像素的字节偏移量

            // ----------- 3.2 提取YUV分量 -----------
            int y0 = p[idx];                         // 当前像素亮度分量Y
            int u  = p[idx + 1];                     // 蓝色色度分量U
            int v  = p[idx + 3];                     // 红色色度分量V
            int c = y0 - 16;                         // 去除偏移 (标准YUV偏移)
            int d = u - 128;
            int e = v - 128;

            // ----------- 3.3 YUV→RGB颜色空间转换 (BT.601标准) -----------
            int r = (298 * c + 409 * e + 128) >> 8;  // R = 1.164*(Y-16) + 1.596*(V-128)
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;  // G = 1.164*(Y-16) - 0.392*(U-128) - 0.813*(V-128)
            int b = (298 * c + 516 * d + 128) >> 8;  // B = 1.164*(Y-16) + 2.017*(U-128)

            // ----------- 3.4 限幅操作，确保RGB在[0,255]范围内 -----------
            r = (r > 255) ? 255 : (r < 0 ? 0 : r);
            g = (g > 255) ? 255 : (g < 0 ? 0 : g);
            b = (b > 255) ? 255 : (b < 0 ? 0 : b);

            // ----------- 3.5 组装 ARGB8888 像素并写入帧缓冲 -----------
            // LCD通常使用32位色深，每像素4字节: A(8) R(8) G(8) B(8)
            // Alpha通道设为0xFF表示不透明。
            row[x + x_offset] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}


/**
 * 函数名: event_listener
 * 功能描述:
 *   该函数运行于独立线程中，用于监听 Linux 输入事件设备（/dev/input/event1）。
 *   一旦检测到按键按下事件 (EV_KEY + value=1)，则将全局变量 photo_flag 置为 1，
 *   通知主线程执行拍照逻辑。
 *
 * 工作原理:
 *   Linux 输入子系统会将所有键盘、按钮、触摸屏等输入设备统一抽象为 /dev/input/eventX。
 *   其中每个事件以 struct input_event 结构体表示，包含事件类型(type)、代码(code)和数值(value)。
 *   该函数通过阻塞式 read() 持续读取事件流，实现低延迟的键值监听。
 *
 * 参数:
 *   arg - 线程参数指针（此处未使用，预留扩展）
 *
 * 返回值:
 *   无实际返回值，返回 NULL 以结束线程。
 *
 * 使用场景:
 *   在主函数 main() 中使用 pthread_create() 创建该线程，实现并行监听。
 *
 * 注意事项:
 *   - 该函数为常驻线程，不会主动退出；
 *   - 输入设备路径必须正确（常见为 /dev/input/event1）；
 *   - 若按键为 GPIO 键，需确认驱动已注册到 input 子系统；
 *   - photo_flag 为全局变量，应设为 volatile 以防编译优化错误；
 *   - 若系统中有多个输入设备，可用 "cat /proc/bus/input/devices" 查看对应编号。
 */

void *event_listener(void *arg) {
    // ---------------------- 1. 打开输入设备 ----------------------
    // O_RDONLY：只读模式打开输入事件文件，例如 /dev/input/event1。
    // 成功返回文件描述符，失败返回 -1。
    int fd = open(INPUT_DEVICE, O_RDONLY);
    if (fd < 0) {                                   // 打开失败则立即退出程序
        perror("open input device failed");
        exit(1);
    }
    printf("[INIT] Listening for key events on %s ...\n", INPUT_DEVICE);

    // ---------------------- 2. 定义输入事件结构 ----------------------
    // 每个 input_event 结构包含一个输入事件的信息：
    // struct timeval time;   // 时间戳（秒+微秒）
    // unsigned short type;   // 事件类型 (EV_KEY, EV_REL, EV_ABS 等)
    // unsigned short code;   // 事件代码 (键值，如 KEY_ENTER)
    // unsigned int value;    // 事件值 (1=按下, 0=释放, 2=长按)
    struct input_event ev;

    // ---------------------- 3. 持续读取事件 ----------------------
    // read() 将阻塞等待直到输入事件到来，适合长驻监听线程。
    while (read(fd, &ev, sizeof(ev)) > 0) {
        // ----------- 3.1 判断是否为按键事件 -----------
        if (ev.type == EV_KEY) {                    // 仅处理按键类事件
            // value == 1 表示“按下”；0 表示“释放”；2 表示“长按”
            if (ev.value == 1) {
                photo_flag = 1;                     // 设置全局拍照标志位
                printf("[KEY] Button pressed (code=%d, time=%ld.%06ld)\n",
                       ev.code, ev.time.tv_sec, ev.time.tv_usec);
            }
            else if (ev.value == 0) {
                // 可选扩展：检测松开事件
                // printf("[KEY] Button released (code=%d)\n", ev.code);
            }
        }
    }

    // ---------------------- 4. 异常与清理 ----------------------
    // 若 read() 返回 <= 0，说明设备被拔出或文件描述符无效。
    perror("[ERROR] Input event read failed or device disconnected");
    close(fd);                                      // 关闭输入设备文件
    return NULL;                                    // 线程退出
}


/**
 * 函数名: main
 * 功能描述:
 *   系统主入口，完成整个双目摄像系统的初始化、实时显示与按键拍照逻辑。
 *   运行流程如下：
 *     1. 清空历史照片文件；
 *     2. 打开LCD帧缓冲设备并建立内存映射；
 *     3. 初始化左右摄像头（/dev/video21, /dev/video23）；
 *     4. 创建独立线程监听按键输入；
 *     5. 主循环中实时采集图像并显示；
 *     6. 检测到按键事件后执行双目同步拍照并保存JPEG文件。
 *
 * 系统依赖:
 *   - LCD 显示设备: /dev/fb0 (分辨率800×480)
 *   - 左右摄像头: /dev/video21 和 /dev/video23 (640×480, YUYV格式)
 *   - 按键输入设备: /dev/input/event1
 *
 * 输出目录:
 *   - 左摄像头图像: /root/left/
 *   - 右摄像头图像: /root/right/
 *
 * 注意事项:
 *   - 必须先加载摄像头驱动 (v4l2 模块)；
 *   - 摄像头与LCD分辨率不同，draw_on_lcd() 自动缩放；
 *   - 主循环采用 30ms 延时 (约33fps)，保持流畅预览；
 *   - 按键事件通过全局变量 photo_flag 进行线程间通信。
 */

int main() {
    // ============================================================
    // 1. 初始化阶段：清空历史照片目录
    // ============================================================
    clear_jpg_files(LEFT_FOLDER);                  // 清空 /root/left 下的旧 .jpg 文件
    clear_jpg_files(RIGHT_FOLDER);                 // 清空 /root/right 下的旧 .jpg 文件
    printf("[INIT] Old photos cleared. Press the button to take a photo.\n");

    // ============================================================
    // 2. 打开 LCD 帧缓冲设备 (/dev/fb0)
    // ============================================================
    int fb_fd = open("/dev/fb0", O_RDWR);          // 以读写模式打开 LCD 设备
    if (fb_fd < 0) {                               // 打开失败直接退出
        perror("open fb0");
        exit(1);
    }

    // 定义两个结构体用于获取 LCD 参数：
    struct fb_var_screeninfo vinfo;                // 可变参数 (分辨率、颜色深度)
    struct fb_fix_screeninfo finfo;                // 固定参数 (行长度、显存大小)
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);     // 获取屏幕分辨率等信息
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);     // 获取帧缓冲长度与布局信息

    // 使用 mmap() 将帧缓冲映射到用户空间，便于直接写入像素数据
    unsigned int *fb_mem = (unsigned int *)mmap(
            0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { perror("mmap fb"); exit(1); }

    printf("[INIT] LCD framebuffer mapped. Resolution: %dx%d\n", vinfo.xres, vinfo.yres);

    // ============================================================
    // 3. 初始化双摄像头设备 (/dev/video21 & /dev/video23)
    // ============================================================
    struct buffer buf1[2], buf2[2];                // 每个摄像头分配两个缓冲区用于循环采集
    int vfd1, vfd2;                                // 摄像头文件描述符
    init_camera(CAM_LEFT, buf1, &vfd1);            // 初始化左摄像头
    init_camera(CAM_RIGHT, buf2, &vfd2);           // 初始化右摄像头
    printf("[INIT] Both cameras initialized.\n");

    // ============================================================
    // 4. 创建独立线程监听按键事件 (/dev/input/event1)
    // ============================================================
    pthread_t tid;
    pthread_create(&tid, NULL, event_listener, NULL);
    printf("[INIT] Key listener thread started.\n");

    // ============================================================
    // 5. 进入主循环：实时显示 + 拍照逻辑
    // ============================================================
    int photo_idx = 0;                             // 拍照编号计数器
    while (1) {
        // --------------------------------------------------------
        // 5.1 从左右摄像头取出一帧视频数据
        // --------------------------------------------------------
        struct v4l2_buffer b1 = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP
        };
        struct v4l2_buffer b2 = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP
        };

        // VIDIOC_DQBUF：从视频输入队列取出一个填充完成的缓冲区
        if (ioctl(vfd1, VIDIOC_DQBUF, &b1) == 0 &&
            ioctl(vfd2, VIDIOC_DQBUF, &b2) == 0) {

            // --------------------------------------------------------
            // 5.2 将两路图像分别绘制到 LCD 左右半屏
            // --------------------------------------------------------
            draw_on_lcd(fb_mem, finfo.line_length, vinfo.xres, vinfo.yres,
                        buf1[b1.index].start, 0);                      // 左半屏绘制左图
            draw_on_lcd(fb_mem, finfo.line_length, vinfo.xres, vinfo.yres,
                        buf2[b2.index].start, vinfo.xres / 2);         // 右半屏绘制右图

            // 将缓冲重新放回采集队列以供下一帧使用
            ioctl(vfd1, VIDIOC_QBUF, &b1);
            ioctl(vfd2, VIDIOC_QBUF, &b2);
        }

        // --------------------------------------------------------
        // 5.3 检测拍照标志位
        // --------------------------------------------------------
        if (photo_flag) {
            printf("[TRIGGER] Capture event detected.\n");

            // 再次从摄像头队列取出当前帧作为保存帧
            struct v4l2_buffer cap1 = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .memory = V4L2_MEMORY_MMAP
            };
            struct v4l2_buffer cap2 = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .memory = V4L2_MEMORY_MMAP
            };
            ioctl(vfd1, VIDIOC_DQBUF, &cap1);
            ioctl(vfd2, VIDIOC_DQBUF, &cap2);

            // 生成左右图像文件路径
            char left_path[256], right_path[256];
            snprintf(left_path, sizeof(left_path), LEFT_FOLDER"/%d.jpg", photo_idx);
            snprintf(right_path, sizeof(right_path), RIGHT_FOLDER"/%d.jpg", photo_idx);

            // 执行 YUYV→JPEG 转换并保存
            yuyv_to_jpeg(buf1[cap1.index].start, WIDTH, HEIGHT, left_path);
            yuyv_to_jpeg(buf2[cap2.index].start, WIDTH, HEIGHT, right_path);
            printf("[SAVE] Photo %d saved:\n       Left: %s\n       Right: %s\n",
                   photo_idx, left_path, right_path);

            // 将采集缓冲重新入队
            ioctl(vfd1, VIDIOC_QBUF, &cap1);
            ioctl(vfd2, VIDIOC_QBUF, &cap2);

            // 清除拍照标志，递增编号
            photo_flag = 0;
            photo_idx++;
        }

        // --------------------------------------------------------
        // 5.4 控制刷新帧率 (约33fps)
        // --------------------------------------------------------
        usleep(30000);                            // 延时30ms，防止CPU过载
    }

    // ============================================================
    // 6. 程序退出清理（一般不会执行到）
    // ============================================================
    munmap(fb_mem, finfo.smem_len);               // 释放帧缓冲映射
    close(fb_fd);
    close(vfd1);
    close(vfd2);
    printf("[EXIT] Program terminated.\n");

    return 0;                                     // 正常结束
}

