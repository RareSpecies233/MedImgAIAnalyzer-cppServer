# info.json API — 项目选择器（示例 / 演示）

## 基本说明
- 所有接口均以 `/api/` 为前缀（服务器已配置为使用该前缀）。

## JSON 模式（project 对象）
- `uuid`: 字符串（RFC UUID）
- `name`: 字符串
- `createdAt`: ISO-8601 UTC 时间字符串
- `updatedAt`: ISO-8601 UTC 时间字符串
- `note`: 字符串

## 接口列表

1) 列表 — 获取所有项目
- 方法：GET /api/projects/info.json
- 返回：200，JSON 数组，按 `updatedAt` 降序排序
- 示例：`[]` 或 `[ {"uuid":"...","name":"..."} ]`

2) 获取单个项目
- 方法：GET /api/projects/{uuid}
- 返回：200（项目对象）
- 未找到：404，`{ "error": "project not found" }`

3) 删除项目
- 方法：DELETE /api/projects/{uuid}
- 返回：200 或 204（幂等）
- 未找到：404

4) 新建项目
- 方法：POST /api/projects
- 请求体：`{ "name": "string", "note": "string" }`
- 返回：201，创建的项目对象（包含 `uuid`、`createdAt`、`updatedAt`）
- 错误：400（无效请求体）

5) 更新项目（部分更新）
- 方法：PATCH /api/projects/{uuid}
- 请求体：`{ "note": "string" }`1
- 返回：200，更新后的项目对象
- 未找到：404

6) 修改备注（专用接口）
- 方法：PATCH /api/projects/{uuid}/note
- 请求体：`{ "note": "string" }`
- 返回：200，更新后的项目对象
- 未找到：404

## 请求/响应头
- 请求：POST/PATCH 请使用 `Content-Type: application/json`
- 响应：`Content-Type: application/json`
- CORS（开发环境）：`Access-Control-Allow-Origin: *`

## 磁盘存储布局
- `db/info.json` — 单一 JSON 数组文件，包含所有项目对象（首次运行时自动创建）。

## 本地快速上手
- 构建：在项目根目录运行 `./BuildmacOS.sh`
- 启动：`./main`
- 快速验证：运行 `./tests/manual_info_api.sh`

## 注意与限制
- 示例级别的持久化：使用单文件 JSON 存储，若有多进程并发访问需谨慎。
- 列表按 `updatedAt` 降序返回。
- 本示例不包含认证（仅用于本地/演示）。
