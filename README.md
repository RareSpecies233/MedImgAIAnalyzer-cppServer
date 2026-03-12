# MedImgAiAnalyzer的cpp后端代码仓库

小型项目，Crow的函数直接写在头文件即可

### **请注意：Crow的输出会出现`[INFO    ]`，这是Crow的Feature，不是bug**(注：我美化了一下)


---

- [Api说明及json说明](Api.md)

编译macOS版本请使用[BuildmacOS.sh](BuildmacOS.sh)(请确保有1GB的空闲空间)  
编译Windows版本请使用[build-windows.ps1](build-windows.ps1)(请确保有50GB的空闲空间)  

编译Windows/Linux请原生编译，项目使用了onnx和opencv，交叉编译很麻烦

如果你不信邪，可以使用[BuildWindows.sh](BuildWindows.sh)试试看（不建议使用）



---
后台数据库文件结构

```text
db/
└─ {project-UUID}/      # 使用UUID管理各个project
   ├─ temp/             # 临时文件缓存
   ├─ 3d/               # 推理后生成的3d模型
   ├─ OG3d/             # 推理后生成的3d模型（原始）
   ├─ llmdoc/           # 当前项目临时 RAG 文档
   ├─ llm_history.json  # 当前项目的 LLM/RAG 历史对话
   ├─ pngs/
   ├─ npzs/
   ├─ dcms/
   ├─ nii
   ├─ processed/        # 最终产物
   │  ├─ pngs/          # 推理后的图片（供前端显示）
   │  ├─ npzs/          # 推理后的npz（推理产物）
   │  ├─ dcms/          # 推理后的npz转换为dcm（前端需求时才转换）
   │  ├─ nii-PD         # 推理后的npz转换为nii（前端需求时才转换）
   │  └─ project.json   # 存储了当前project的所有状态的json
   └ info.json          # 存储了当前database的所有project的json
```

项目级 LLM / RAG 说明

- 全局 RAG 文档仍然保存在 `db/llmdb/`。
- 某个项目临时上传的 RAG 文档保存在 `db/{uuid}/llmdoc/`。
- 上传到 `db/{uuid}/llmdoc/` 的文档会在上传后立即解析，并在同目录生成同名 `.json` 解析结果。
- 项目级问答接口会同时检索全局 RAG 文档与当前项目 `llmdoc/` 中的文档，不会使用其他项目的 `llmdoc/` 文档。
- 项目级历史对话保存在 `db/{uuid}/llm_history.json`。
