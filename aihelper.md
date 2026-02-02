# AI Helper

## 项目概览
本项目是一个基于 C++/Crow 的本地 API 服务，负责医学影像项目的管理与文件处理。主要提供：
- 项目创建、查询、删除、备注更新
- 文件上传与初始化流程（raw/npz/markednpz/dcm/nii）
- PNG 与 markedpng 的列表/单文件获取
- 归档下载（png/npz/dcm/nii）

服务默认监听：`http://0.0.0.0:18080`

## 关键目录
- `main.cpp`：应用入口与路由注册
- `include/info_api.h`：所有 API 路由与核心逻辑
- `include/info_store.h`：项目元数据存取
- `db/`：本地数据目录
  - `db/info.json`：项目列表
  - `db/{uuid}/project.json`：项目状态
  - `db/{uuid}/png`：PNG 输出
  - `db/{uuid}/npz`：NPZ 文件（包含 `markednpz` 的原始上传）
  - `db/{uuid}/markedpng`：markednpz 额外输出的标注 PNG

## 初始化流程要点
- `POST /api/project/{uuid}/inited` 传入 `raw`：`png|npz|markednpz|dcm|nii`
- 非 `npz/markednpz` 会先转换为 `npz` 并存入 `db/{uuid}/npz`
- 非 `png` 会生成 `png` 并存入 `db/{uuid}/png`
- `markednpz` 会额外生成 `db/{uuid}/markedpng` 下的叠加图
- 初始化结束后：`temp` 文件夹会重命名为 `png/npz/dcm/nii`，其中 `markednpz` 也存入 `npz`

## 运行与构建
- 构建：`./BuildmacOS.sh`
- 启动：`./main`

## 主要 API 入口
详情参见 `Api.md`，常用接口包括：
- 项目列表：`GET /api/projects/info.json`
- 单项目：`GET /api/projects/{uuid}`
- 上传：`POST /api/project/{uuid}/upload`
- 初始化：`POST /api/project/{uuid}/inited`
- PNG 列表/文件：`GET /api/project/{uuid}/png` / `GET /api/project/{uuid}/png/{filename}`
- markedpng 列表/文件：`GET /api/project/{uuid}/markedpng` / `GET /api/project/{uuid}/markedpng/{filename}`

## 备注
- 转换函数中 `all2npz_stub` 与 `all2png_stub` 目前为占位逻辑。
- CORS 已在路由层统一添加。