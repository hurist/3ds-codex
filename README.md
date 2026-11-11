# 3DS Codex

在 Azahar 中浏览本机 Codex 项目，并在已有对话中发送消息。桥接程序读取 Codex 桌面端保存的本地项目和归属，并通过 Codex App Server 读取对话列表与历史。未匹配到本地项目的对话显示在「未归类」。

## 编译客户端

使用包含 libctru、3DS 版 Mbed TLS 和 zlib 的 devkitARM 工具链。

```sh
make -C client
```

也可以用 devkitPro 的 Docker 镜像编译。

```sh
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/project" -w /project devkitpro/devkitarm make -C client
```

编译结果为 `client/3ds-codex.3dsx`。中文字体位图随源码提供，编译无需重新生成。字体许可见 `client/assets/FONT-LICENSE.txt`。

## 编译和安装 CIA

CIA 包含客户端本体，可以从 3DS 的 HOME 菜单启动。已安装 Luma3DS 和 FBI 的真机可将 `3ds-codex.cia` 复制到 SD 卡，用 FBI 的「SD」菜单选择文件并安装。客户端和 `.3dsx` 共用 SD 卡上的配对资料，电脑端仍需运行同一个桥接程序。

本机安装 devkitARM、[makerom](https://github.com/3DSGuy/Project_CTR/releases) 和 [bannertool](https://github.com/carstene1ns/3ds-bannertool/releases) 后，运行：

```sh
make -C client cia
```

如果先用上面的 Docker 命令编译出 ELF，再在电脑端使用 makerom 和 bannertool 打包：

```sh
make -C client/cia
```

输出文件为 `client/3ds-codex.cia`。构建时可以用 `MAKEROM=/工具路径/makerom` 和 `BANNERTOOL=/工具路径/bannertool` 指定工具位置。后续更新沿用同一个应用编号。

## 在模拟器中使用

1. 在使用 Codex 的同一电脑、同一系统账号下运行 `python3 bridge/server.py`。确保 `codex` 在 PATH 上。桥接默认仅监听 `127.0.0.1:8765`。
2. 在同机 Azahar 中打开 `client/3ds-codex.3dsx`，选择「本机模拟器」。
3. 方向键上下选择项目或对话，A 打开，B 返回上一级，L/R 翻页。项目列表按 X 刷新。
4. 打开对话后，上下滚动消息，R 查看更早的页面，L 回到较新的页面。START 退出。

## 换屏与触摸滚动

- 浏览项目、对话或审批详情时按住 ZL，把内容临时移到下方触屏。松开 ZL 后恢复原布局，并保留阅读位置。
- 换屏后在下屏上下拖动。点项目或对话卡片可直接打开。列表滑到边缘后继续滑可翻页，对话则在较早和较新的历史页之间切换。
- 松开 ZL 后就可以继续点键盘、复制或审批等下屏按钮。对话里按 ZR 仍可跳到最新消息。

## 自动搜索与局域网直连

电脑和 3DS 连到同一个局域网后，打开 3DS Codex 就会自动搜索桥接。若桥接启用了直连，客户端找到电脑后直接打开项目列表。已有配对时会优先用已保存的设备凭据。搜索失败时，可以点「重新搜索电脑」，也可以在「手动连接」里填地址和端口，直连时配对码留空。

在使用 Docker Compose 时，在 `compose.env` 里填入自己的局域网网段，例如 `THREEDS_CODEX_DIRECT_NETWORK=192.168.50.0/24`，然后重新创建桥接容器。直接运行桥接时，可用 `--direct-network 192.168.50.0/24`。自动搜索使用 UDP 18765，连接使用原来的桥接 TCP 端口。直连使用加密传输，信任所配置网段内的设备，这些设备可以浏览和操作 Codex。删除该设置并重启桥接可恢复到只允许已配对设备。

## 真机局域网连接与配对

1. 电脑与 3DS 连到同一局域网。把 `client/3ds-codex.3dsx` 放进 SD 卡的 `/3ds/`，用 Homebrew Launcher 打开。
2. 用运行 Codex 的同一个电脑账号启动加密桥接。将下方地址换成电脑当前的局域网 IPv4 地址。电脑需要支持 PSK 的 Python 3.13 或更新版本。

```sh
python3 bridge/server.py --lan-address 192.168.50.50 --pair
```

3. 在 3DS 选择「手动连接」，填电脑地址、端口（默认 `8766`）和终端显示的 20 位配对码。用 SELECT 切换输入项，按 START 配对。配对码十分钟内有效，成功后作废。
4. 配对成功会进入项目列表。重新打开客户端，会搜索桥接并用已保存的凭据重连。已有设备重连时电脑端无需加 `--pair`。若电脑换了地址，可以重新配对。

电脑端可查看和撤销设备。撤销后，该设备的凭据不能再次连接。

```sh
python3 bridge/server.py --list-devices
python3 bridge/server.py --revoke 设备编号
```

局域网模式只绑定指定 IPv4 地址，使用预共享密钥建立 TLS 连接。设备凭据保存在 SD 卡的 `/3ds/3ds-codex.pair`。电脑端默认保存到 `~/.config/3ds-codex/devices.json`，可用 `--devices-file` 指定其他位置。保持凭据文件私有。原本的同机模拟模式继续使用回环地址和端口 `8765`。

## Docker 部署与开机启动

在 Linux 上使用 Docker Compose。桥接容器负责加密局域网连接、图片预览和语音转写。主机上的用户服务通过本地 Unix 套接字启动 Codex，继续使用桌面端所在账号和原来的项目路径。Codex 的登录文件和项目目录不需要挂进容器。

1. 用运行 Codex 的账号创建 `~/.local/state/3ds-codex`，在里面建立 `run`、`devices` 和 `voice` 三个目录，权限设为仅当前用户访问。已有配对可将原来的 `devices.json` 放进 `devices/`。本地 Whisper 模型可放到 `voice/model.bin`。
2. 写入 `~/.local/state/3ds-codex/host.env`，将路径替换为当前仓库的绝对路径。

   ```dotenv
   THREEDS_CODEX_PROJECT="/你的项目路径/3DS Codex"
   ```

3. 写入同一目录的 `compose.env`，将示例值换成当前账号的 UID、GID、状态目录和电脑局域网地址。

   ```dotenv
   THREEDS_CODEX_STATE=/home/你的用户名/.local/state/3ds-codex
   THREEDS_CODEX_UID=1000
   THREEDS_CODEX_GID=1000
   THREEDS_CODEX_IP=192.168.50.50
   THREEDS_CODEX_PORT=18767
   THREEDS_CODEX_DIRECT_NETWORK=192.168.50.0/24
   ```

4. 从项目目录安装用户服务并启动容器。

   ```sh
   install -D -m 644 deploy/3ds-codex-host.service ~/.config/systemd/user/3ds-codex-host.service
   systemctl --user daemon-reload
   systemctl --user enable --now 3ds-codex-host.service
   loginctl enable-linger "$USER"
   docker compose --env-file ~/.local/state/3ds-codex/compose.env up -d --build
   ```

Docker 服务需已启用开机启动。容器使用 `unless-stopped` 重启策略，并在局域网地址尚未就绪时等待。保留原地址、端口和配对文件即可重连。首次配对可发送信号打开十分钟配对窗口：

```sh
docker compose --env-file ~/.local/state/3ds-codex/compose.env kill -s SIGUSR1 bridge
docker compose --env-file ~/.local/state/3ds-codex/compose.env logs --tail 10 bridge
```

## 查看对话图片

安装电脑桥接所用 Python 环境的 Pillow 后，图片附件会在对话里显示缩略图。点下屏「查看图片」，或按住 ZL 把对话移到触屏后点图片。打开后可放大、缩小，在下屏拖动，或用 L/R 查看当前历史页的其他图片。

支持项目内的本地图片、Codex 图片缓存，以及历史里仍保留数据的 PNG、JPEG、WebP、GIF 图片。GIF 预览显示首帧。外部网址和只剩远端文件编号的附件会显示占位提示。

## 在已有对话中发送消息

1. 打开空闲对话后按 X 编辑。默认是中文九宫格拼音。例如依次点 `64426`，再点「你好」，也可按 X 选首词。候选栏两侧的箭头和 L/R 都可翻候选，Y 删除。
2. 下屏可触控，也能用方向键选键、A 输入。检查上屏草稿，按 START 或点发送。B 返回并保留当前草稿。
3. 留在最新历史页即可看到执行状态和回复。若发送回执丢失，按 A 检查或重试同一条发送，桥接会识别重复操作。

下排「中／英」键或 SELECT 切换中文九宫格与英文 26 键。英文键盘用 ↑ 切换大小写。常用逗号、句号可直接点。「符号」或 ZR 打开数字和符号页，可切换中文、英文和代码符号。

界面使用简体中文。桥接发送文字时附上默认用简体中文回复的偏好，掌机对话视图显示消息正文。既有对话、项目名、命令和代码保留原文。拼音词表内置，可离线选择单字和词组。连续输入多个词时，九宫格可自动组合候选。选中前段候选时，余下的数字会留在候选栏里继续组词。词表也会提供「补」标记的补全词和「纠」标记的一键纠错词，选中后才写入草稿。真机访问走已配对的局域网加密连接。

拼音词表来自 [Rime 袖珍简化字拼音](https://github.com/rime/rime-pinyin-simp/tree/0c6861ef7420ee780270ca6d993d18d4101049d0)。项目中的词表按九宫格数字序列建立索引并按词频排序，许可证见 `client/assets/PINYIN-LICENSE.txt`。

## 中文语音听写

电脑安装 `whisper-cli` 并准备一个支持中文的多语言模型后，启动桥接时指定模型。可选安装 OpenCC，使传统字转为简体字。

```sh
python3 bridge/server.py --whisper-model /path/to/ggml-base-q5_1.bin
```

也可通过 `--whisper-cli` 指定程序位置，通过 `--opencc-config` 指定 OpenCC 配置。

1. 打开消息草稿，点「语音听写」。未在组词时，也可按 L。按 A 开始录音。
2. 按 A 结束并转写，或按 B 取消录音。每段最多 30 秒，达到上限会结束并转写。电脑在本地处理音频。
3. 查看中文转写，按 A 插入草稿，按 X 重新录音，或按 B 返回。插入后可以编辑，按 START 才会发送给 Codex。

普通 Azahar 使用其音频设置中的麦克风输入。当前预览使用固定的普通话样本。录音和临时音频不会写入 Codex 对话。

参考 [whisper.cpp](https://github.com/ggml-org/whisper.cpp)。

## 复制与粘贴

- 在对话中按 A 进入「选择文字」。点下屏文字，或用方向键移动光标。按 X 设起点，移动光标选择，可跨当前页的多行，再按 A 复制。按 Y 可复制光标所在的整条消息正文。
- 可切换项目或对话，按 X 打开草稿，再点「粘贴」。若想插在指定位置，按 R 进入「编辑光标」，方向键左右移动，A 粘贴，B 返回键盘。
- 剪贴板仅在应用内共享，退出后清空。一次最多复制 8192 字节，超出时保留原剪贴板或原草稿。跨历史页选区和电脑系统剪贴板同步仍待接入。

## 追加指令与停止

- 对由桥接程序启动的运行，按 X 写追加指令。编辑页显示 「追加给当前轮次」，按 START 将指令发送到原执行轮次。已经结束的轮次会拒绝追加，草稿可继续编辑。
- 按 SELECT 打开停止确认，A 提交，B 返回。提交后等待 Codex 回报停止状态。轮次停止时，属于该轮的待审批请求也会关闭。
- 两个操作均绑定发起时所选的轮次。回执丢失时可用 A 重试同一操作。桌面端独立发起的运行仍需另行验证控制能力。

## 回答 Codex 的提问

从 3DS 发起的任务如果弹出提问，对话页会显示「等待回答」。点「查看问题」或按 Y，选择选项或填写文字。多道题用「上一题」「下一题」切换，答完后点「提交答案」，原来的任务会继续。文字回答可以用键盘、粘贴或语音听写，先确认填入表单，再提交整份答案。密码等秘密字段暂不支持在掌机上填写。

## 处理命令与文件审批

1. 在对话里看到 「等待审批」 后，按 Y 或点下屏的审批入口。
2. 上屏显示命令、工作目录和理由，或文件路径和变更差异。上下滚动可查看完整内容。差异中的新增行用绿色显示，删除行用红色显示。按 X 选择「批准一次」，按 Y 选择「拒绝」。如果 Codex 只支持拒绝并停止本轮，界面会提示 「拒绝并停止」。
3. 按 A 确认，按 B 取消。确认后回到对话，查看 Codex 回执和后续执行结果。

支持由本桥接程序接收的命令审批和单次文件变更审批，选项遵循 Codex 给出的可用决定。文件差异缺失、超过显示容量，或同时要求会话级写权限时，仅显示可用的拒绝选项。网络授权等其他请求可显示为待审批。桌面端独立启动的任务是否能共享审批，仍需联调。

## 已验证

使用 devkitARM 重新编译，并在 Azahar 2126.2 的 Libretro 核心内完成实测。通过方向键和 A/B 在两个真实本地项目之间切换，分别打开一条真实对话，滚动并翻到较早的历史页，再返回项目列表。确认模拟器发出的请求收到成功响应，并检查了双屏实际渲染。

在独立的 Codex 测试对话中，从 Azahar 用屏幕键盘输入并发送 `ping`，显示执行状态后收到真实回复 `PONG`。确认仅提交一次。另模拟回执丢失并验证重试同一操作不会重复启动。

在独立测试对话中，用 Azahar 处理了两条真实命令审批。批准后命令创建了测试文件，拒绝并停止后对应文件未创建。验证了取消确认保持请求待处理，同一决定重试不会再次提交。

在独立测试目录中，通过 Azahar 批准并拒绝了真实的文件变更请求。拒绝后文件保留原内容，批准后才写入预览中显示的修改。

在 Azahar 中向真实的待审批测试轮次追加了 `ping`，同一轮收到指令并回复 `PONG`。随后测试停止确认，确认取消时原请求仍在，正式停止后轮次进入中断，待审批项失效，测试文件保持原样。

在 Azahar 中用拼音输入 `nihao`、选中「你好」并发送，已确认真实 Codex 回复用简体中文回应了问候。

在 Azahar 中从一条演示对话复制整条消息，再选取跨两行的部分文字。随后切到另一条对话，将选区插入草稿中间，核对中文和换行均保留。草稿未发送。

在 Azahar 中通过模拟麦克风录制了固定普通话样本，经电脑本地转写后插入草稿。另确认取消录音不会上传，插入草稿不会发送。样本来自 [Sherpa ONNX 的中文测试录音](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17/tree/main/test_wavs)，测试用的是其中一小段。样本转写存在个别错字，尚未验证物理麦克风与真机录音。

在 Azahar 中用电脑实际 Wi‑Fi 地址完成加密配对并保存凭据。重启电脑端桥接和模拟器后，用保存的设备凭据重新打开了 Codex 对话。验证了旧配对码、错误密钥、已撤销的设备和明文访问被拒绝，且本机模拟模式仍可用。真机 Wi‑Fi、SD 卡和 Homebrew Launcher 的实际组合仍待验收。
