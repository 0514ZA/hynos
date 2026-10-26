# firmware/ —— 第三方固件（非本项目自研）

本目录存放**仅用于本地测试启动**的 UEFI 参考固件，来自 TianoCore EDK II 的 OVMF 构建。
它不是本工程的著作权范围，适用其自身许可。

| 文件 | 说明 |
| --- | --- |
| `OVMF_CODE.fd` | 固件代码卷 |
| `OVMF_VARS.fd` | 固件变量存储。`run.sh` 使用时会**复制一份到 `build/`** 再挂载，不改写本文件 |

- **许可**：BSD-2-Clause-Patent（EDK II 的 `License.txt`）。再分发请保留其许可与版权声明。
- **用途**：`run.sh uefi` 在找不到系统 OVMF 时回退到本目录，使仓库开箱即可验证 UEFI 引导
  路径（见 README 第 3 节「依赖查找顺序」）。
- **可选**：如不希望仓库携带二进制固件，可在发布前删除本目录；`run.sh` 会改从
  `$OVMF_CODE` / `$OVMF_VARS` 或系统路径查找。

时效更新的版本可从以下位置获取：

- <https://github.com/tianocore/edk2/releases>
- Linux 发行版的 `ovmf` / `edk2-ovmf` 包
- Windows 可使用 MSYS2 的 `mingw-w64-x86_64-edk2-ovmf` 包