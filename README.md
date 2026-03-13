# MedImgAIAnalyzer C++ 后端

这是一个基于 Crow 的医学影像后端服务，负责项目管理、医学影像格式转换、PNG/标注图输出、ONNX 推理、高级数据增强、3D 模型生成，以及全局/项目级 LLM RAG 能力。

## 当前能力

- 项目增删改查与 project.json 状态管理
- temp 临时项目创建、启动时自动清理、转正式项目
- png、npz、dcm、nii 初始化与格式转换
- 原始 png、markedpng、处理后标注图的列表与单图访问
- png、markedpng、处理后标注图、融合图、npz、dcm、nii 的 ZIP 下载
- ONNX 推理与处理结果输出到 processed 目录
- 项目级 NPZ 高级数据增强，结果输出到 enhDBprocessed 目录
- 3D 模型生成与下载
- 全局 LLM 配置、全局 RAG 文档管理
- 项目级临时 RAG 文档、项目级问答、项目级历史对话
- 运行时详细日志与请求日志

## 构建与运行

- macOS 构建：执行 [BuildmacOS.sh](BuildmacOS.sh)
- Windows 脚本：仓库中保留了 [build-windows.ps1](build-windows.ps1)
- 启动：执行 `./main`
- 如需推理功能：启动时传入 `--onnx <model.onnx>`
- HTTP 服务当前使用单监听实例启动；推理并行度仍由 `--infer-threads <N>` 单独控制
- 如需关闭日志文件保存：启动时传入 `--nolog`
- 如需开启 Crow 全量日志：启动时传入 `--crowdebug`

## 目录结构

```text
db/
├─ info.json                    # 项目索引
├─ temp/                        # 临时项目根目录（程序每次启动时会整体清空）
│  └─ {temp-UUID}/
│     ├─ project.json           # 临时项目状态
│     ├─ temp/                  # 上传暂存目录
│     ├─ png/
│     ├─ markedpng/
│     ├─ npz/
│     ├─ dcm/
│     ├─ nii/
│     ├─ processed/
│     ├─ enhDBprocessed/
│     ├─ 3d/
│     └─ OG3d/
├─ llm.json                     # 全局 LLM 配置
├─ llmdb/                       # 全局 RAG 文档
│  └─ __cache__/                # 全局 RAG 文本缓存
└─ {project-UUID}/
   ├─ project.json              # 当前项目状态
   ├─ temp/                     # 上传暂存目录
   ├─ png/                      # 原始 PNG
   ├─ markedpng/                # 原始标注 PNG（透明图层）
   ├─ npz/                      # 原始 NPZ
   ├─ dcm/                      # 原始 DCM
   ├─ nii/                      # 原始 NII
   ├─ processed/
   │  ├─ pngs/                  # 处理后的标注 PNG
   │  ├─ npzs/                  # 推理结果 NPZ
   │  ├─ dcm/                   # 按需生成的处理后 DCM
   │  └─ nii/                   # 按需生成的处理后 NII
   ├─ enhDBprocessed/
   │  ├─ npzs/                  # 高级增强后的 NPZ
   │  ├─ pngs/                  # 高级增强后的普通 PNG
   │  ├─ markedpngs/            # 高级增强后的透明标注 PNG
   │  ├─ dcm/                   # 按需生成的高级增强 DCM
   │  └─ nii/                   # 按需生成的高级增强 NII
   ├─ 3d/                       # 处理后 3D 模型
   ├─ OG3d/                     # 原始 3D 模型（markednpz 场景）
   ├─ llmdoc/                   # 当前项目临时 RAG 文档
   └─ llm_history.json          # 当前项目 LLM/RAG 历史记录
```

## Temp 临时项目说明

- 临时项目目录固定为 `db/temp/{tempUUID}`
- 程序每次启动时都会删除整个 `db/temp/`，用于清理上一次运行残留的临时项目
- 临时项目使用独立的 temp 前缀接口，不会写入 `db/info.json`
- 当需要保留临时项目结果时，可调用 `POST /api/temp/{tempUUID}/convert` 将其转为正式项目并写入项目索引
- temp 路由已与正式项目的核心能力保持对称：
   - 初始化与推理流程（upload/inited/start_analysis/semi）
   - 原始与 processed 的 PNG/markedpng 列表、单图访问与下载
   - 高级增强（start_enhdb、enhdb 访问与下载）
   - 3D（to_3d_model、download/3d、download/OG3d）
   - 项目级 LLM/RAG（llm/doc、llm/chat、llm/history、llm/history/delete）

## 代码结构说明

- 为减少超长文件维护成本，temp 的基础项目路由已拆分到 `include/temp_basic_api.h`。
- temp 的高级能力路由已拆分到 `include/temp_advanced_api.h`。
- 正式项目与 temp 项目的 3D 生成逻辑已收敛到共享实现，避免两套逻辑漂移。

## PNG 与标注图说明

- `png/` 存放原始灰度图或普通 PNG
- `markedpng/` 存放初始化阶段导出的透明标注图
- `processed/pngs/` 当前实现中存放处理后的透明标注图
- `enhDBprocessed/pngs/` 存放高级增强后的普通 PNG
- `enhDBprocessed/markedpngs/` 存放高级增强后的透明标注图
- 融合图不持久化落盘，下载时由后端临时把基础 png 与透明标注图进行 alpha 融合，再打包返回

## 高级数据增强说明

- 入口接口为 `POST /api/project/{uuid}/start_enhdb`
- 输入来源固定为 `db/{uuid}/npz/` 中的项目级 NPZ 文件
- 增强结果固定输出到 `db/{uuid}/enhDBprocessed/`
- 当前支持的增强参数与独立 `cppnpzprocess` 工具保持一致：缩放、旋转、裁切、对比度、伽马、恢复原始分辨率
- 当 NPZ 中存在 `label` 键时，几何变换会同步作用到 `label`，保证和 `image` 的空间对应关系不变
- 增强完成后可继续通过现有风格的接口获取普通 PNG、标注 PNG、融合 PNG、NPZ、DCM、NII

## LLM / RAG 说明

- 全局 RAG 文档保存在 `db/llmdb/`
- 项目临时 RAG 文档保存在 `db/{uuid}/llmdoc/`
- 项目上传的临时文档会在上传后立即解析，并在同目录生成同名 `.json` 解析结果
- 项目级问答会联合检索全局文档与当前项目文档，不会读取其他项目的临时文档
- 项目级历史对话保存在 `db/{uuid}/llm_history.json`

## 日志说明

- 运行日志默认保存到 `db/logs/`
- 请求日志会记录 URL、Content-Type、body 大小和响应状态
- 二进制请求体不会原样写入日志，而会以摘要形式记录，避免乱码污染日志文件

## 参考文档

- 接口定义见 [Api.md](Api.md)
