# 完成项（摘要）

## 已实现内容
- 添加了一个简单的头文件级别 JSON 存储（在首次启动时若缺失会创建 `db/info.json`）。
- 实现了 REST 接口（均位于 `/api/` 前缀下）：
  - GET `/api/projects/info.json`（列出项目）
  - GET `/api/projects/{uuid}`（获取单个项目）
  - DELETE `/api/projects/{uuid}`（删除项目）
  - POST `/api/projects`（创建项目）
  - PATCH `/api/projects/{uuid}`（部分更新）
- 更新 `main.cpp`，在启动时初始化存储并注册路由。
- 添加了文档（`infoJsonApi.md`）、手动测试脚本（`tests/manual_info_api.sh`）和示例 JSON 文件。

## 新增文件
- `include/info_store.h`
- `include/info_api.h`
- `include/uuid_utils.h`
- `include/time_utils.h`
- `examples/info.example.json`
- `tests/manual_info_api.sh`
- `infoJsonApi.md`、`done.md`

## 手动验证方式
- 编写并运行了路由处理逻辑及一个包含 6 条 curl 用例的脚本以验证 API 行为。

## 后续建议
- 添加单元测试并在 CI 中执行 `tests/manual_info_api.sh`。
- 若多个服务器实例会共享同一 `db/`，建议实现跨进程文件锁定。
- 生产环境建议考虑迁移到嵌入式数据库（例如 SQLite）。

## 本地运行（快速）
1. 构建：`./BuildmacOS.sh`
2. 启动：`./main`
3. 验证：`./tests/manual_info_api.sh`

