## 基本说明
- 所有接口均以 `/api/` 为前缀（服务器已配置为使用该前缀）。

## JSON 模式（info.json）
- `uuid`: 字符串（RFC UUID）
- `name`: 字符串
- `createdAt`: ISO-8601 UTC 时间字符串
- `updatedAt`: ISO-8601 UTC 时间字符串
- `note`: 字符串


## JSON 模式（project.json）
- `uuid`: UUID（和文件夹名称保持一致）
- `raw`: png、npz、markednpz、dcm、nii、false（是否上传raw源文件）
- `nii`: true、false、raw（是否转为nii，raw为png）
- `dcm`: true、false、raw（是否转为dcm，raw为png）
- `semi`: true、false（是否增强）
- `semi-xL`: int（-1为不修改）（增强后取用的x轴像素的起始点）
- `semi-xR`: int（-1为不修改）（增强后取用的x轴像素的起终点）
- `semi-yL`: int（-1为不修改）（增强后取用的y轴像素的起始点）
- `semi-yR`: int（-1为不修改）（增强后取用的y轴像素的起终点）
- `processed`: false、raw、semi（是否已处理完成，raw/semi 分别表示处理模式）
- `PD`: false、raw、semi（是否经过推理，raw使用raw进行推理、semi使用增强过的文件进行推理）
- `PD-nii`: true、false（推理后文件是否转为nii，每次推理时删除转换后文件并改为false）
- `PD-dcm`: true、false（推理后文件是否转为dcm，每次推理时删除转换后文件并改为false）
- `PD-3d`: true、false（是否生成3d文件，每次推理时删除转换后文件并改为false）

## 接口列表

1) 列表 — 获取所有项目
- 方法：GET /api/projects/info.json
- 返回：200，JSON 数组，按 `updatedAt` 降序排序
- 示例：`[]` 或 `[ {"uuid":"...","name":"..."} ]`

2) 获取单个项目
- 方法：GET /api/projects/{uuid}
- 返回：200（项目对象）
- 未找到：404，`{ "error": "project not found" }`

2.1) 获取 project.json
- 方法：GET /api/projects/{uuid}/project.json
- 返回：200（project.json 对象）
- 未找到：404，`{ "error": "project not found" }`

2.2) 取消初始化
- 方法：POST /api/projects/{uuid}/uninit
- 作用：删除 `db/{uuid}/temp`，并将 `project.json` 的 `raw` 设为 `false`
- 返回：200，`{ "status": "ok" }`

3) 删除项目
- 方法：DELETE /api/projects/{uuid}
- 返回：200 或 204（幂等）
- 未找到：404

4) 新建项目
- 方法：POST /api/projects
- 请求体：`{ "name": "string", "note": "string" }`
- 返回：201，创建的项目对象（包含 `uuid`、`createdAt`、`updatedAt`）
- 错误：400（无效请求体）

5) 修改备注（专用接口）
- 方法：PATCH /api/projects/{uuid}/note
- 请求体：`{ "note": "string" }`
- 返回：200，更新后的项目对象
- 未找到：404

6) 上传文件
- 方法：POST /api/project/{uuid}/upload
- 作用：上传文件到 `db/{uuid}/temp`
- 支持：`multipart/form-data` 或直接二进制（使用 `X-Filename` 指定文件名；若未命名则使用 `noname.bin`）
- 返回：200，`{ "saved": <count> }`

7) 初始化完成
- 方法：POST /api/project/{uuid}/inited
- 请求体：`{ "raw": "png|npz|markednpz|dcm|nii" }`
- 说明：
	- 非 `npz/markednpz` 会先转为 `npz` 并保存到 `db/{uuid}/npz`
	- 非 `png` 会转为 `png` 并保存到 `db/{uuid}/png`
	- 当 `raw` 为 `dcm/nii` 时，`png` 由“先转 `npz` 再转 `png`”链路生成
	- `markednpz` 额外输出一张到 `db/{uuid}/markedpng`
	- `temp` 文件夹会重命名为 `png/npz/dcm/nii`（`markednpz` 也保存到 `npz`）
	- `project.json` 的 `raw` 更新为传入参数；若为 `dcm/nii`，对应字段设为 `raw`
- 返回：200，`{ "status": "ok" }`

8) 获取 png 列表
- 方法：GET /api/project/{uuid}/png
- 返回：200，PNG 文件名数组

8.1) 获取单张 png
- 方法：GET /api/project/{uuid}/png/{filename}
- 返回：200，PNG 文件（二进制）

8.2) 获取 markedpng 列表
- 方法：GET /api/project/{uuid}/markedpng
- 返回：200，markedpng 文件名数组

8.3) 获取单张 markedpng
- 方法：GET /api/project/{uuid}/markedpng/{filename}
- 返回：200，markedpng 文件（二进制）

9.1) 下载 png
- 方法：GET /api/project/{uuid}/download/png
- 返回：ZIP（存储模式）

9.2) 下载 npz
- 方法：GET /api/project/{uuid}/download/npz
- 返回：ZIP（存储模式）

9.3) 下载 dcm
- 方法：GET /api/project/{uuid}/download/dcm
- 返回：ZIP（存储模式）

9.4) 下载 nii
- 方法：GET /api/project/{uuid}/download/nii
- 返回：ZIP（存储模式）

10) 更新裁剪参数（semi）
- 方法：PATCH /api/projects/{uuid}/semi
- 请求体：`{ "semi-xL": int, "semi-xR": int, "semi-yL": int, "semi-yR": int }`
- 规则：当四个参数均为 -1 时，将 `project.json` 的 `semi` 设为 `false`；否则设为 `true`
- 返回：200，`{ "status": "ok" }`

11) 开始处理（推理）
- 方法：POST /api/project/{uuid}/start_analysis
- 请求体：`{ "mode": "raw|semi" }`（也兼容 `PD` 或 `type`）
- 说明：
	- 处理完成后保存到 `db/{uuid}/processed/npzs`、`db/{uuid}/processed/pngs`、`db/{uuid}/processed/dcm`、`db/{uuid}/processed/nii`
	- `processed/dcm` 由 `npz -> dcm` 生成，`processed/nii` 由 `npz -> nii` 生成
	- `project.json` 的 `processed` 设为 `raw` 或 `semi`，并将 `PD-dcm`、`PD-nii` 设为 `true`
- 返回：200，`{ "status": "ok" }`

12) 获取处理过的图片列表
- 方法：GET /api/project/{uuid}/processed/png
- 返回：200，PNG 文件名数组

12.1) 获取单张处理过的图片
- 方法：GET /api/project/{uuid}/processed/png/{filename}
- 返回：200，PNG 文件（二进制）

13) 下载处理过的图片
- 方法：GET /api/project/{uuid}/download/processed/png
- 返回：ZIP（存储模式）

14) 下载处理过的 npz
- 方法：GET /api/project/{uuid}/download/processed/npz
- 返回：ZIP（存储模式）

15) 下载处理过的 dcm
- 方法：GET /api/project/{uuid}/download/processed/dcm
- 返回：ZIP（存储模式）

16) 下载处理过的 nii
- 方法：GET /api/project/{uuid}/download/processed/nii
- 返回：ZIP（存储模式）

17) 转换为 3d 模型
- 方法：POST /api/project/{uuid}/to_3d_model
- 说明：使用 `db/{uuid}/processed/npzs` 生成 3d 模型，保存到 `db/{uuid}/3d`
- 说明：若 `project.json` 的 `raw` 为 `markednpz`，额外生成原始 3d 模型到 `db/{uuid}/OG3d`
- 返回：200，`{ "status": "ok" }`

18) 下载 3d 模型
- 方法：GET /api/project/{uuid}/download/3d
- 返回：GLB（二进制，model.glb）

19) 下载原始 3d 模型
- 方法：GET /api/project/{uuid}/download/OG3d
- 返回：GLB（二进制，model.glb）

20) 获取 LLM 配置
- 方法：GET /api/llm/settings
- 返回：200，`{ "base_url": "...", "api_key": "...", "model": "...", "temperature": 0.2, "top_k": 4, "system_prompt": "..." }`

21) 保存/更新 LLM 配置
- 方法：POST /api/llm/settings
- 请求体：可包含 `base_url`、`api_key`、`model`、`temperature`、`top_k`、`system_prompt`
- 说明：配置会保存到 `db/llm.json`
- 返回：200，`{ "status": "ok" }`

22) 上传 RAG 文档
- 方法：POST /api/llm/rag/upload
- 支持：`multipart/form-data` 或直接二进制（`X-Filename`）
- 说明：文档保存到 `db/llmdb/`
- 返回：200，`{ "saved": <count>, "uploaded": ["name1", "name2"] }`

23) 获取 RAG 文档列表
- 方法：GET /api/llm/rag/documents
- 返回：200，`{ "documents": [{"name":"...","size":123}], "count": 1 }`

23.1) 删除单个 RAG 文档
- 方法：DELETE /api/llm/rag/documents/{name}
- 说明：按文档名删除（即列表接口中的 `name`）
- 返回：200，`{ "status": "ok", "deleted": 1, "name": "..." }`
- 未找到：404，`{ "error": "document not found" }`

23.2) 下载单个 RAG 文档
- 方法：GET /api/llm/rag/download/{name}
- 说明：按文档名下载原始文件（即列表接口中的 `name`）
- 返回：200，文件二进制（`Content-Disposition: attachment`）
- 未找到：404，`{ "error": "document not found" }`

24) 下载 RAG 文档
- 方法：GET /api/llm/rag/download
- 返回：ZIP（二进制，`llm_rag_documents.zip`）

25) RAG + 大模型问答
- 方法：POST /api/llm/chat
- 请求体：`{ "question": "...", "top_k": 4, "temperature": 0.2, "system_prompt": "..." }`
- 说明：
	- 默认使用 `db/llm.json` 中的 `base_url`、`api_key`、`model`
	- 会自动检索 `db/llmdb/` 文档并注入上下文
- 返回：200，`{ "answer": "...", "chunks": 12, "contexts": ["..."] }`

## 请求/响应头
- 请求：POST/PATCH 请使用 `Content-Type: application/json`
- 响应：`Content-Type: application/json`
- CORS（开发环境）：`Access-Control-Allow-Origin: *`

## 磁盘存储布局
- `db/info.json` — 单一 JSON 数组文件，包含所有项目对象（首次运行时自动创建）。
- `db/{uuid}/project.json` — 项目创建时生成，记录项目处理相关状态。
- `db/llm.json` — 大模型配置（`base_url`、`api_key`、`model`、`temperature`、`top_k`、`system_prompt`）。
- `db/llmdb/` — RAG 文档目录（上传的文本/PDF等文档持久化存储）。

## 本地快速上手
- 构建：在项目根目录运行 `./BuildmacOS.sh`
- 启动：`./main`

## 注意与限制
- 示例级别的持久化：使用单文件 JSON 存储，若有多进程并发访问需谨慎。
- 列表按 `updatedAt` 降序返回。
- 本示例不包含认证（仅用于本地/演示）。
