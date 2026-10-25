/*
 * music_api —— 在线音乐搜索（iTunes Search API 代理）
 *
 * 为什么走 iTunes：
 *   - 苹果的公开接口，**不需要注册、不需要 key**，稳定性有保障
 *   - 能搜到中文歌（关键：地区参数要用 us，cn 区不提供试听）
 *   - 每个结果自带 30 秒试听地址、封面图、时长
 *
 * 为什么由 ESP32 代理而不是让浏览器直接请求：
 *   浏览器的跨域策略（CORS）会挡住 iTunes 的响应，必须由设备中转。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 搜索歌曲
 * @param keyword  搜索关键词（中文会被自动 URL 编码）
 * @param limit    每页返回几首（建议 15~25）
 * @param offset   从第几条开始（翻页用，0 表示第一页）
 * @return 用 malloc 分配的 JSON 字符串，调用者负责 free()；失败返回 NULL
 *
 * 返回格式：
 *   {"ok":true,"offset":0,"count":20,"songs":[{"name":"..","artist":"..","preview":"..","cover":"..","duration":185}]}
 *   或 {"ok":false,"message":".."}
 */
char *music_search(const char *keyword, int limit, int offset);

/**
 * @brief 热门推荐（用一个轮换的种子词，让首页有内容）
 * @return 同上
 */
char *music_suggest(int limit);

#ifdef __cplusplus
}
#endif
