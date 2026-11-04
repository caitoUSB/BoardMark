# release/ · 发布版本存放处

这里放**已经打好包的、可以直接拷给老师用的版本**。

## 命名规范

```
BoardMark-v0.15.0-win64.zip
          │     │     └── 平台：win64（目前只有这一个）
          │     └──────── 版本号，和 CMakeLists.txt 里的 project(... VERSION x.y.z) 一致
          └────────────── 项目名
```

> **改名记录（2026-10-08）**：项目原名 **ClassMind**，更名为 **BoardMark**
> （中文名 智课笔记）。**从下一个版本开始，发布包和里面的 exe 都叫 BoardMark。**
>
> 所以这个目录里会**同时存在两种前缀**，这是正常的：
>
> | 前缀 | 时期 |
> |---|---|
> | `ClassMind-v0.9.0` … `ClassMind-v0.14.0` | 改名之前的历史发布，**保留原名不追改** |
> | `BoardMark-v0.15.0` 起 | 改名之后 |
>
> 旧包里的 exe 仍叫 `ClassMind.exe`，这是事实记录，不去动它。
> 详见仓库根的 `README.md` 和 `CHANGELOG.md`。

**从 BoardMark 起，每个版本的压缩包里包含**：`BoardMark.exe` + Qt 运行库 + 其它依赖 DLL + 一份**默认配置**（共 47 个文件）。

**关键区别是 `settings.json` 里有没有 Key**（实测过，别凭印象）：

| 打包方式 | 包里的 `settings.json` | 能不能对外发 |
|---|---|---|
| `deploy.ps1`（默认） | ✅ 有，但是**出厂默认值，`ai_api_key` 是空串** | 能 |
| `deploy.ps1 -IncludeSettings` | ❌ **带你本机的真实 API Key** | **绝对不能** |

那份默认 `settings.json` 是**程序自己写出来的**：`deploy.ps1` 第 4 步要清空 PATH
跑一次 `--info` 做验证，而 `Settings::load()` 发现配置文件不存在时会写一份默认的
出去（[settings.cpp:147](src/app/settings.cpp#L147)）——目的是让老师有个文件可以
直接用记事本打开改。**默认值里没有 Key，所以是安全的。**

## 为什么这些 zip 没有被提交进 Git

`.gitignore` 里排除了 `release/*.zip`。原因是**二进制文件进了 git 就永久留在历史里**：

- 每个版本包约 33 MB
- 十个版本就是 330 MB
- 而且**删不掉** —— 即使后来 `git rm`，历史里还在，别人克隆时要全部下载

所以：

| 场景 | 做法 |
|---|---|
| 本地留档、给老师拷贝 | 就放在这个目录里 |
| 对外分发 | **走 GitHub Releases**（专门放二进制，不进 git 历史） |
| 长期备份 | 网盘 / 移动硬盘，别指望 git |

**如果确实需要把某个版本钉进 git 历史**，
单独 `git add -f release/BoardMark-v0.15.1-win64.zip` 强制加进去。
但请想清楚：**这是唯一一次机会，加了就删不掉了。**

## 打一个发布版的流程

```powershell
# 1. 先把版本号改好（只改 CMakeLists.txt 里 project(... VERSION x.y.z)）
# 2. 重新配置 + 编译
cmake -S E:\BoardMark -B E:\BoardMark\build -G Ninja `
      -DCMAKE_PREFIX_PATH=E:/Environment/mingw-w64/ucrt64 -DCMAKE_BUILD_TYPE=Release
cmake --build E:\BoardMark\build

# 3. 打包（注意：不要带 -IncludeSettings，那会把你的 API Key 打进去）
E:\BoardMark\deploy.ps1

# 4. 压成 zip 放进这里
Compress-Archive -Path E:\BoardMark\dist\* `
  -DestinationPath E:\BoardMark\release\BoardMark-v0.15.0-win64.zip

# 5. 提交 + 打标签
git add -A; git commit -m "release: v0.15.0"
git tag -a v0.15.0 -m "BoardMark v0.15.0"
```

> **`release/` 里还躺着改名前的包**（`ClassMind-v0.9.0` … `ClassMind-v0.14.0`），
> 那是事实记录，按上面的命名规范**保留原名不追改**。
>
> `cmake -S` 的路径要和仓库实际所在目录一致。**目录改过名的话必须重新
> configure**（`build/CMakeCache.txt` 里写死了绝对路径，对不上会直接报
> 「The current CMakeCache.txt directory … is different」）。

> ⚠️ **第 3 步千万别加 `-IncludeSettings`。**
> 那个参数会把 `settings.json`（里面有你的 API Key）一起打进发布包。
> 自己测试时用一下没问题，**对外分发的包绝对不能带**。

## 版本号怎么定

语义化版本 `MAJOR.MINOR.PATCH`：

- **MAJOR** — 有不兼容改动（配置文件格式变了、用户得重新设置）
- **MINOR** — 加了新功能，旧配置还能用
- **PATCH** — 只修 bug

**1.0.0 的门槛是「经过真实课堂验证」。** 在那之前一直留在 0.x ——
这是诚实的做法，也让老师知道"这个东西还在打磨"。
