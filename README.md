# DisplayRescue

キーを押すだけで、画面構成(拡張・複製など)を元に戻せるWindows用のツールです。

画面が何も映っていなくても使えます。ログイン画面やロック画面でも効きます。

## こんなときに

「サブモニターだけに映す」設定にしたまま、そのモニターの入力をゲーム機などに切り替えてしまうと、Windowsの画面がどこにも映らなくなります。こうなると設定アプリは操作できず、ログイン画面には構成を変える手段がそもそもありません。

DisplayRescueを入れておけば、キーを押すだけで見える状態に戻せます。

## 使えるキー

インストールした時点で、次のキーが使えます。

| キー | 画面構成 |
|---|---|
| `Ctrl` + `Alt` + `F8` | 拡張 |
| `Ctrl` + `Alt` + `F9` | 複製 |
| `Ctrl` + `Alt` + `F10` | メインのみ |
| `Ctrl` + `Alt` + `F11` | サブのみ |

- 普段のデスクトップ、ロック画面、再起動直後のログイン画面のどこでも効きます。
- 割り当ては変えられます(「[キーの割り当てを変える](#キーの割り当てを変える)」)。
- 切り替えの仕組みは`Win` + `P`と同じです。「メインのみ」「サブのみ」でどちらのモニターが残るかは、PCによって異なります。
- ネットワーク、ほかのアプリ、追加のランタイムは要りません。

## 動作環境

- Windows 10 / 11(64ビット)
- インストールと設定の変更には管理者権限が要ります

## インストール

1. [Releases](../../releases)から`DisplayRescue-<バージョン>-x64.zip`をダウンロードし、好きな場所に展開します。
2. 展開したフォルダの`DisplayRescue.exe`(設定アプリ)を開きます(管理者の確認が出ます)。
3. 「全般」のページで「インストール」を押します。

これでキーが使えるようになります。PCを再起動しても自動で動きます。

開くのは`DisplayRescue.exe`だけです。`bin`フォルダには設定アプリが使うものが入っているので、一緒に置いたままにしてください。インストールでは、`bin`の`DisplayRescueService.exe`と`DisplayRescueMascot.exe`を`C:\Program Files\DisplayRescue`にコピーします。

| ファイル | 役目 |
|---|---|
| `DisplayRescue.exe` | 設定アプリ。展開したフォルダから開きます(インストールはしません) |
| `bin\DisplayRescueService.exe` | サービス本体と、キーを待ち受ける役。コマンドでも使えます |
| `bin\DisplayRescueMascot.exe` | ロック画面のマスコット |
| `bin\DisplayRescueInterop.dll` | 設定アプリが使う部品 |
| `bin\mascot\` | 同梱のマスコットのモデルとモーション |

> [!NOTE]
> exeにはまだコード署名をしていないため、SmartScreenの警告が出ることがあります。気になる場合は、ソースからビルドしてください(「[ソースからビルドする](#ソースからビルドする)」)。

### 新しいバージョンに更新する

新しいzipを展開し、その中の`DisplayRescue.exe`を開いて「再インストール」を押します。キーの割り当てとマスコットの設定はそのまま引き継がれます。

### アンインストール

展開したフォルダの`DisplayRescue.exe`を開いて「アンインストール」を押します。

サービス、設定(キーの割り当てとマスコット)、`C:\Program Files\DisplayRescue`が削除されます。次のものは残るので、不要なら手で消してください。

- 展開したフォルダ
- `C:\ProgramData\DisplayRescue`(ログと、取り込んだマスコットのモデル・モーション)

## キーの割り当てを変える

1. 展開したフォルダの`DisplayRescue.exe`を開き、「キーの割り当て」のページを開きます(フォルダを消してしまった場合は、zipをもう一度ダウンロードして展開します)。
2. 変えたい操作の入力欄を選び、割り当てたいキーを押します。`Backspace`で割り当てなしにできます。
3. 「保存」を押します。変更はすぐに反映されます。

> [!WARNING]
> `Ctrl`も`Alt`も付けないキー(`A`だけ、`Shift` + `A`など)は、なるべく避けてください。割り当てたキーはWindows全体で横取りされるため、普段の入力に使えなくなります。ログイン画面でパスワードが打てなくなると、自力では戻せません。それでも割り当てる場合は、確認のためにもう一度「保存」を押します。

`F13`〜`F24`は普段の入力に使わないので、単体で割り当てても問題ありません。マクロキーや自作キーボードなど、キーボード側で`F13`〜`F24`を送るように設定できるなら、そのボタン1つで切り替えられます(ログイン画面でも効きます)。ただし、PowerToysなどのソフトで割り当て直したキーは、サインインした後でないと効きません。

`F12`はWindowsがデバッガ用に予約しているため、ホットキーには使わないでください(1.0.0の既定は`Ctrl` + `Alt` + `F9`〜`F12`でしたが、1.0.1で`F8`〜`F11`に変えました)。

## 画面切り替えキーの案内

ロック画面とログイン画面では、画面構成を切り替えるキーの一覧を各モニターの左上に表示します。画面が映っているモニターで、復旧に使うキーを確認できます。

既定でオンです。不要な場合は、「マスコット」のページで「ロック画面に画面切り替えキーを表示する」をオフにします。

## マスコット

ロック画面とログイン画面に、3Dのキャラクタ(VRM)を出せます。既定ではオフです。

1. 展開したフォルダの`DisplayRescue.exe`を開き、「マスコット」のページを開きます。
2. 「ロック画面にマスコットを出す」をオンにして、「保存」を押します。

zipにはモデル(VRoidのサンプルモデル)と、待機とモーション2本(きょろきょろ見る・うとうとする)が入っていて、インストールしたときに、まだ何も入っていなければ自動で入ります。同梱のものの出どころは`bin\mascot\README.md`にあります。

- 自分のモデル(`.vrm` / `.glb`)やモーション(`.vrma`)に変更できます。待機中に再生するモーションと、一定間隔で再生するモーションを個別に設定できます。
- 「配置を調整」で、デスクトップにマスコットを出して、ドラッグで位置、ホイールで大きさ、右ドラッグで向きを決められます。`Enter`で保存、`Esc`でやめます。
- モーションは一覧から選んで「再生」すると、デスクトップで試せます。
- 「普段のデスクトップにも出す」をオンにすると、普段のデスクトップにも出ます。
- マスコットは1つで200 MBほどのメモリを使うので、ロックしている間だけ動かし、ロックしてから読み込みます(出てくるまで2秒ほどかかります)。「常駐させる」をオンにすると、いつも動かしておくのですぐ出ます。

## コマンドで使う

展開したフォルダの`bin\DisplayRescueService.exe`を使うと、キーと画面構成のことはコマンドでもできます。「管理者」と書いたものは、管理者のPowerShellで実行してください。

| コマンド | 内容 |
|---|---|
| `.\bin\DisplayRescueService.exe install` | インストール(管理者) |
| `.\bin\DisplayRescueService.exe uninstall` | アンインストール(管理者) |
| `.\bin\DisplayRescueService.exe bindings` | 今の割り当てを表示 |
| `.\bin\DisplayRescueService.exe bind clone Ctrl+Alt+D` | 割り当てを変える(管理者) |
| `.\bin\DisplayRescueService.exe unbind external` | 割り当てを解除する(管理者) |
| `.\bin\DisplayRescueService.exe extend` | 今すぐ切り替える |
| `.\bin\DisplayRescueService.exe dump` | 今の画面構成を表示 |
| `.\bin\DisplayRescueService.exe version` | バージョンを表示 |

`bind` / `unbind` / 切り替えで使う操作の名前は次のとおりです。

| 名前 | 画面構成 |
|---|---|
| `extend` | 拡張 |
| `clone` | 複製 |
| `internal` | メインのみ |
| `external` | サブのみ |

`bind`のキーは、`Ctrl` `Alt` `Shift`を`+`でつなぎ、最後にキーを1つ書きます。

- 使えるキーの名前: `F1`〜`F24`、`A`〜`Z`、`0`〜`9`、`Space`、`Enter`、`Left`、`Pause`、`Num0`など
- 名前の無いキーは、仮想キーコードで書けます(例: `0xBA`)
- `Ctrl`も`Alt`も付けないキーは、末尾に`--force`が要ります(理由は上の注意を参照)

## ログ

ログは`C:\ProgramData\DisplayRescue\rescue.log`(UTF-8)に残ります。1 MBを超えると`rescue.log.1`に移します。

```powershell
Get-Content C:\ProgramData\DisplayRescue\rescue.log -Wait
```

## 制限

- ロックした直後は、少しのあいだキーが効かないことがあります。
- UACの確認画面が出ている間は、キーが効きません。
- リモートデスクトップで接続しているときのキー入力は、リモート側に届きます。手元の物理モニターは切り替わりません。
- メッセージは日本語だけです。

## セキュリティ

- サービスはSYSTEM(LocalSystem)の権限で動きます。
- 受け取るのは、割り当てたキーの組み合わせだけです。ほかのキー入力は記録しません。
- 設定(`HKLM\SOFTWARE\DisplayRescue`)と`C:\ProgramData\DisplayRescue`に書き込めるのは、管理者とSYSTEMだけです。ログは一般ユーザーも読めます。
- サインイン中のユーザーは誰でも`sc.exe control DisplayRescue 130`を送れます。これでできるのは、設定の読み直しだけです。
- マスコットは、パスワード欄・ログイン画面ではSYSTEMの権限で、ロック画像の上ではサインイン中のユーザーの権限で動きます。サービスが起動するのは、`C:\Program Files\DisplayRescue`にある自分のexeだけです。
- モデルとモーション(他人が作ったファイル)は、設定アプリが取り込むときに単純な形式に変換します。ログイン画面で動くマスコットはその形式だけを読み、VRMや画像そのものは解析しません。

## 仕組み

1. キー入力は、そのとき操作中のデスクトップ(入力デスクトップ)にしか届きません。サインイン中は通常のデスクトップ(`Default`)、ロック画面とログイン画面は`Winlogon`デスクトップです。サービス(セッション0)には、どちらの入力も届きません。
2. そこでサービスは、入力デスクトップにホットキー監視プロセス(`DisplayRescueService.exe watch`)を起動します。通常のデスクトップではサインイン中のユーザー権限で、ロック画面とログイン画面ではSYSTEM権限で動作します。ロック、ロック解除、サインインのたびに監視プロセスを再起動します。
3. ホットキー監視プロセスは、割り当てたキーだけを`RegisterHotKey`で登録します。キーが押されたら、`SetDisplayConfig`にトポロジ(`SDC_TOPOLOGY_*`)を指定して切り替えます。
4. ロック画面は、ロック直後の画像(`Default`デスクトップの`LockApp`)と、パスワード欄(`Winlogon`デスクトップ)の2つでできています。マスコットはそれぞれに1つずつ表示します。ロック画像の上に表示するプロセスは、ユーザーの権限にUIAccessを付けて起動します。2つのマスコットは、OSの時計を基準に同じモーションを同じ時刻から再生するため、画面が切り替わっても姿勢が飛びません。

## ソースからビルドする

必要なもの:

- Visual Studio 2026(ツールセットv145)と「C++によるデスクトップ開発」「.NETデスクトップ開発」ワークロード
- .NET 10 SDK(設定アプリ。バージョンは`global.json`で指定)
- [WIL](https://github.com/microsoft/wil)(NuGetで自動的に復元されます)

C++20、x64でビルドします。C++のランタイムは静的リンク(`/MT`)です。設定アプリはC#(WPF)です。

```powershell
msbuild DisplayRescue.slnx /t:restore /p:RestorePackagesConfig=true /p:Platform=x64
msbuild DisplayRescue.slnx /p:Configuration=Release /p:Platform=x64
```

出力は`x64\Release\`です。この設定アプリは.NETが入っているPCで動きます。配布しているzipと同じ、.NETを同梱した1つのexeを作る手順は`.github/workflows/release.yml`にあります。

## ライセンス

[MIT](LICENSE)

同梱しているもの:

- [cgltf](https://github.com/jkuhlmann/cgltf)(MIT): glTF / VRMの読み込み
- [WIL](https://github.com/microsoft/wil)(MIT): Windowsの関数を使うための部品
- [.NET](https://github.com/dotnet/runtime)(MIT): 設定アプリのランタイム(配布しているzipの`DisplayRescue.exe`に含まれます)
- マスコットのモデル`Shibu.vrm`: VRoidの公式サンプルモデル「β Ver AvatarSample_1」のダークネスバージョン(CC0、pixiv Inc.)
- マスコットの待機モーション: [CMU Graphics Lab Motion Capture Database](http://mocap.cs.cmu.edu/)のデータから作成。The data used in this project was obtained from mocap.cs.cmu.edu. The database was created with funding from NSF EIA-0196217.
- マスコットのモーション2本: DisplayRescueのために作成(MIT)
