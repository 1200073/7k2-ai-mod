# 发布包

二进制不放仓库（`git` 不适合存 DLL），请到 **Releases** 页下载。

## 包里有什么

| 文件 | 作用 |
|---|---|
| `DINPUT.dll` | ASI Loader 代理（**必须叫这个名字**，`dsound.dll` 会被 Windows KnownDLL 机制忽略） |
| `scripts/sk2_zh.asi` | 插件本体（中文渲染 + AI 改造） |
| `update/` | 中文文本资源（覆盖用，不改原文件） |
| `install.bat` | 一键安装 |
| `uninstall.bat` | 一键卸载 |
| `使用说明.md` | 给玩家的说明书 |

## 安装（三步）

1. 解压到**游戏目录**
2. 双击 `install.bat`
3. 运行 `7K2.exe`

## 校验

每个 Release 都附带 `文件校验.txt`（SHA256 清单）。
**联机前务必确认双方 `.asi` 的 sha256 完全一致。**

## 版本要求

- **Steam / GOG 版《七王国 2 HD》2.10F**
- 其他版本大概率不适用（所有 hook 的字节指纹会校验失败）