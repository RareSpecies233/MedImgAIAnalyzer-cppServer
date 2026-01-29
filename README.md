# MedImgAiAnalyzer的cpp后端代码仓库

小型项目，Crow的函数直接写在头文件即可

### 由于使用了Crow，在windows上编译可能会是灾难，建议其他平台交叉编译windows发行版
不解答任何在windows平台编译此项目的问题（Just Use WSL2）  
如果你也在macOS上使用微软大战代码搭配Xcode的clang，可以直接使用我的.vscode中的配置（应该）

### **请注意：Crow的输出会出现`[INFO    ]`，这是Crow的Feature，不是bug**

已经写好了BuildMacOS.sh以供直接编译代码

---

main.cpp 为主程序
其他均为测试用程序，不保证其可用性

---

关于Crow的语法：见main.cpp即可

---
后台数据库文件结构

```text
db/
└─ {project-UUID}/      # 使用UUID管理各个project
   ├─ raw/              # 原始上传（正常情况只会存在一种raw文件）
   │  ├─ pngs/
   │  │  ├─ 1.png
   │  │  ├─ *.png
   │  │  └─ *.png
   │  ├─ npzs/
   │  │  ├─ 1.npz
   │  │  ├─ *.npz
   │  │  └─ *.npz
   │  ├─ dcms/
   │  │  ├─ 1.dcm
   │  │  ├─ *.dcm
   │  │  └─ *.dcm
   │  └─ nii
   ├─ semi-processed/   # 用户增强后的文件（也就是用于推理的文件）
   │  └─ npzs/          # 前端传来参数后增强好的npz文件（供推理使用）
   │     ├─ 1-semi.npz
   │     ├─ *-semi.npz
   │     └─ *-semi.npz
   ├─ processed/        # 最终产物
   │  ├─ pngs/          # 推理后的图片（供前端显示）
   │  │  ├─ 1-PD.png
   │  │  ├─ *-PD.png
   │  │  └─ *-PD.png
   │  ├─ npzs/          # 推理后的npz（推理产物）
   │  │  ├─ 1-PD.npz
   │  │  ├─ *-PD.npz
   │  │  └─ *-PD.npz
   │  ├─ dcms/          # 推理后的npz转换为dcm（前端需求时才转换）
   │  │  ├─ 1-PD.dcm
   │  │  ├─ *-PD.dcm
   │  │  └─ *-PD.dcm
   │  ├─ nii-PD         # 推理后的npz转换为nii（前端需求时才转换）
   │  ├─ 3d             # 推理后生成的3d模型
   └ info.json          存储了当前project的所有状态的json
```

---

## db.json中保存的数据
uuid  
名称  
创建时间  
修改时间  
备注  


## 每个info.json中记录了什么
uuid（和文件夹名称保持一致）
是否上传raw源文件
