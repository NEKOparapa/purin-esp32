#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RECORDING_STORE_PATH_MAX 256

/* 分段写入器内部使用的会话索引；不作为应用层公开接口。
 * 调用方可读取编号和累计样本，不能修改字段；文件内容只由本模块追加。
 */
typedef struct {
    int index_fd;
    uint32_t session_id;
    uint32_t next_sequence;
    uint64_t next_sample;
    char directory[RECORDING_STORE_PATH_MAX];
} recording_store_t;

// 创建 recordings/六位编号目录及 index.jsonl。传入已经挂载的 VFS 路径。
// 所有接口成功返回 0，失败返回 errno 值；不会格式化、覆盖或修复旧录音。
// open 接收未初始化的结构体；不能对尚未 close 的会话再次调用。
int recording_store_open(recording_store_t *store, const char *mount_point);

// 获取当前分段的临时路径与完成路径；路径过长返回 ENAMETOOLONG。
// 编码器必须排他创建 part 文件，不能以覆盖模式打开已有路径。
int recording_store_paths(const recording_store_t *store,
                          char *part, size_t part_capacity,
                          char *completed, size_t completed_capacity);

/* 调用前编码器已完成音频收尾、同步并关闭临时文件。
 * 先重命名，再追加并同步索引；只有全部成功才推进 next_sequence/next_sample。
 * 失败必须结束本会话，不得重试 commit 或录入下一段；未提交文件保留供检查。
 * 索引使用一行一个 JSON；未来读取器只接收以换行结尾的完整记录，忽略残缺尾行。
 */
int recording_store_commit(recording_store_t *store, uint32_t sequence,
                           uint64_t samples, uint64_t bytes);

// 关闭索引；无论关闭结果如何，index_fd 均变为 -1。可重复关闭。
int recording_store_close(recording_store_t *store);

#ifdef __cplusplus
}
#endif
