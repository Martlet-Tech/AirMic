#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_http_server.h"

// HTTP 服务器相关常量
#define FILE_PATH_MAX 265
#define SCRATCH_BUFSIZE 1024
#define MAX_CHUNK_SIZE 655360 // 最大分块大小 64KB

// 服务器数据结构
typedef struct {
    char base_path[FILE_PATH_MAX];
    char scratch[SCRATCH_BUFSIZE];
} file_server_data_t;

// 函数声明
esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);

#endif // HTTP_SERVER_H
