# MedImgAiAnalyzer的cpp后端代码仓库

小型项目，Crow的函数直接写在头文件即可

### 由于使用了Crow，在windows上编译可能会是灾难，建议其他平台交叉编译windows发行版
不解答任何在windows平台编译此项目的问题（Just Use WSL2）  
如果你也在macOS上使用微软大战代码搭配Xcode的clang，可以直接使用我的.vscode中的配置（应该）

### **请注意：Crow的输出会出现`[INFO    ]`，这是Crow的Feature，不是bug**(注：我美化了一下)

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
   ├─ temp/          # 临时文件缓存
   ├─ pngs/
   │  ├─ 1.png
   │  ├─ *.png
   │  └─ *.png
   ├─ npzs/
   │  ├─ 1.npz
   │  ├─ *.npz
   │  └─ *.npz
   ├─ dcms/
   │  ├─ 1.dcm
   │  ├─ *.dcm
   │  └─ *.dcm
   ├─ nii
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
   │  ├─ 3d/            # 推理后生成的3d模型
   │  ├─ OG3d/          # 推理后生成的3d模型（原始）
   │  └─ project.json   # 存储了当前project的所有状态的json
   └ info.json          # 存储了当前database的所有project的json
```

---

## db.json中保存的数据
uuid  
名称（前端可更改）  
创建时间  
修改时间  
备注（前端可更改）  


## 每个info.json中记录了什么
uuid（和文件夹名称保持一致）：UUID  
raw（是否上传raw源文件）：png、npz、dcm、nii、false  
nii（是否转为nii）：true、false、raw（raw为png）  
dcm（是否转为dcm）：true、false、raw（raw为png）  
semi（是否增强）：true、false  
semi-xL（增强后取用的x轴像素的起始点）：int（-1为不修改）  
semi-xR（增强后取用的x轴像素的起终点）：int（-1为不修改）  
semi-yL（增强后取用的y轴像素的起始点）：int（-1为不修改）  
semi-yR（增强后取用的y轴像素的起终点）：int（-1为不修改）  
PD（是否经过推理）：false、raw（使用raw进行推理）、semi（使用增强过的文件进行推理）  
PD-nii（推理后文件是否转为nii）：true、false（每次推理时删除转换后文件并改为false）  
PD-dcm（推理后文件是否转为dcm）：true、false（每次推理时删除转换后文件并改为false）  
PD-3d（是否生成3d文件）：true、false（每次推理时删除转换后文件并改为false）  