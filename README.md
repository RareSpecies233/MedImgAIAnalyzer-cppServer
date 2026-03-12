# MedImgAIAnalyzer C++ 后端

这是一个基于 Crow 的医学影像后端服务，负责项目管理、医学影像格式转换、PNG/标注图输出、ONNX 推理、3D 模型生成，以及全局/项目级 LLM RAG 能力。

## 当前能力

- 项目增删改查与 project.json 状态管理
- png、npz、dcm、nii 初始化与格式转换
- 原始 png、markedpng、处理后标注图的列表与单图访问
- png、markedpng、处理后标注图、融合图、npz、dcm、nii 的 ZIP 下载
- ONNX 推理与处理结果输出到 processed 目录
- 3D 模型生成与下载
- 全局 LLM 配置、全局 RAG 文档管理
- 项目级临时 RAG 文档、项目级问答、项目级历史对话
- 运行时详细日志与请求日志

## 构建与运行

- macOS 构建：执行 [BuildmacOS.sh](BuildmacOS.sh)
- Windows 脚本：仓库中保留了 [build-windows.ps1](build-windows.ps1)
- 启动：执行 `./main`
- 如需推理功能：启动时传入 `--onnx <model.onnx>`
- 如需关闭日志文件保存：启动时传入 `--nolog`
- 如需开启 Crow 全量日志：启动时传入 `--crowdebug`

## 目录结构

```text
db/
├─ info.json                    # 项目索引
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
   ├─ 3d/                       # 处理后 3D 模型
   ├─ OG3d/                     # 原始 3D 模型（markednpz 场景）
   ├─ llmdoc/                   # 当前项目临时 RAG 文档
   └─ llm_history.json          # 当前项目 LLM/RAG 历史记录
```

## PNG 与标注图说明

- `png/` 存放原始灰度图或普通 PNG
- `markedpng/` 存放初始化阶段导出的透明标注图
- `processed/pngs/` 当前实现中存放处理后的透明标注图
- 融合图不持久化落盘，下载时由后端临时把基础 png 与透明标注图进行 alpha 融合，再打包返回

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
