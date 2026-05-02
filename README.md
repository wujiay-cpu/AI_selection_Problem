# Selection System

一个用于求解与展示集合覆盖问题（Set Cover）的全栈项目。

- 前端：React + Vite
- 后端：FastAPI
- 加速：pybind11 + C++（可选）

## 1. 项目结构

```text
Selection_system/
├─ UI/                       # 前端源码
├─ backend/                  # 后端源码
│  ├─ api.py                 # FastAPI 入口
│  ├─ algorithm.py           # 算法调度与回退逻辑
│  └─ cover_core/            # C++ 扩展源码（可选编译）
├─ index.html
├─ vite.config.ts
├─ requirements.txt
└─ README.md
```

## 2. 环境要求

- Node.js 16+
- Python 3.9+
- Windows 下建议安装 Visual Studio 2022（用于 C++ 扩展编译）

## 3. 安装依赖

在项目根目录执行：

```bash
npm install
pip install -r requirements.txt
```

如果 `requirements.txt` 未包含完整后端依赖，可补充：

```bash
pip install fastapi uvicorn pybind11
```

## 4. （可选）编译 C++ 加速模块

```bash
cd backend/cover_core
python setup.py build_ext --inplace
```

编译成功后会生成 `cover_core_ext*.pyd`（Windows）或 `.so`（Linux/Mac）。

## 5. 启动项目

建议开两个终端。

### 终端 A：启动后端（推荐在项目根目录）

```bash
uvicorn backend.api:app --host 0.0.0.0 --port 8000
```

也支持在 `backend` 目录启动：

```bash
cd backend
set PYTHONPATH=.
uvicorn api:app --host 0.0.0.0 --port 8000
```

### 终端 B：启动前端

```bash
npm run dev
```

默认访问地址：

- 前端：`http://localhost:3000`
- 后端：`http://127.0.0.1:8000`

## 6. 常见问题

### 6.1 `WinError 10048`（端口被占用）

说明 8000 端口已有服务在运行，不要重复启动。或先杀进程再启动：

```powershell
Get-NetTCPConnection -LocalPort 8000 -State Listen |
  Select-Object -ExpandProperty OwningProcess |
  ForEach-Object { Stop-Process -Id $_ -Force }
```

### 6.2 `ModuleNotFoundError: No module named 'backend'`

通常是启动目录和启动命令不匹配：

- 在项目根目录用：`uvicorn backend.api:app ...`
- 在 `backend` 目录用：`uvicorn api:app ...`

### 6.3 前端报 `ERR_CONNECTION_RESET / Failed to fetch`

通常是后端进程异常退出导致。先确认后端是否仍在监听 8000，再查看后端日志。

## 7. 上传 GitHub 建议

提交前建议确保以下内容不进入仓库：

- `node_modules/`
- `build/`、`__pycache__/`
- `*.pyd`、`*.obj`、`*.lib`、`*.exp` 等编译产物

本仓库已在 `.gitignore` 中配置上述规则。
