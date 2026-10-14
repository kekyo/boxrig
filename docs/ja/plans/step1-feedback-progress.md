# フィードバック取り込みの実装・検証記録

[plan1-feedback.md](./plan1-feedback.md)の12段階を順番に実装します。
各段階で利用側の動作を確認し、コミットを分けます。
全段階の完了後に全体テストを実行することが、この作業の完了条件です。
以下は2026年10月14日時点の記録で、未完了の段階も含みます。

## 進捗と完了条件

| 段階 | 状態 | 確認する成果物・条件 |
| --- | --- | --- |
| 1 | 完了 | 公開ヘッダーを通常includeしたC99/C++20利用側のリンク・実行 |
| 2 | 完了 | 独立regex APIとCLI。NUL、不正UTF-8、診断位置、容量制限、分割再開、不一致時の処理量 |
| 3 | 完了 | 番号・名前付きキャプチャ、認識器とのプログラム共有、同長時の優先、反復、未参加と空文字、結合文字、revisionと不変コピーの寿命、保持量 |
| 4 | 完了 | Enter・矢印・両消去キーの設定交換。画面保持、Ctrl/Alt、keypad、Kitty、失敗時の旧設定保持、UIの押下系列との境界 |
| 5 | 完了 | キー処理時のTTY消去文字取得。変更、無効値、NUL、未取得、接続切替、他の通信路のAutoへの復帰 |
| 6 | 実装中 | 入力禁止と停止完了。遅延IME・clipboard・pasteの無効化、paste終端、選択コピーと端末応答の継続 |
| 7 | 未着手 | 行列数変更のhost通知。分割受信、0と省略、キュー満杯、拒否、layoutへの接続 |
| 8 | 未着手 | POSIX起動診断FDと終了結果、Win32の既存診断と終了HANDLE。異常起動と127/259、fallback、回収と資源の寿命、実Windows検証 |
| 9 | 未着手 | GUIを止めない起動・中止・後始末。完了との競合、世代、二重生成・二重回収、ConPTY終了処理をドライバーで実測 |
| 10 | 未着手 | Linux共有ビルド。公開symbol、SONAME、pkg-config、PIC、private codecの可視性、クリーン・増分・外部ビルド、配布先でのロード |
| 11 | 未着手 | 共通本文snapshotとGTK3 AtkText。VTE・AT-SPI・Orcaとの比較、本文・位置・caret・選択・通知順序、履歴・代替画面・reflow・複数表示・破棄 |
| 12 | 未着手 | GTK4 GtkAccessibleText。最低版4.14での本文・caret・選択・通知、GTK4版VTEとの比較、4.16/4.22以降の操作の採否と検証 |

## 段階1

修正前のC++利用側で、UI・session・POSIX・X11・Wayland・OpenGLの未解決シンボルを再現しました。
Cリンケージが不足していた公開ヘッダー54件へ宣言の保護を追加しています。
各公開ヘッダーを個別にコンパイルする試験と、Cのライブラリへリンクして実行する利用側試験を追加しました。

`make -j8 test-public-headers`が成功しました。
Linuxの通常・GTK3・GTK4利用側と、MinGWのamd64/i686利用側をC99/C++20で確認しています。
Win32の実行環境はWineです。コミットは`0382534`です。

## 段階2

`boxrig/regex.h`を独立した公開入口とし、共通の状態コードを`boxrig/status.h`へ分けました。
正規表現の容量型には画面・端末の設定を含めません。
コンパイル済みプログラムは不変で、照合器が参照を保持します。
入力を作成時に検証・コピーする契約にしたため、作成後に呼び出し側のバッファを変更・解放できます。

入力・結果の位置はUTF-8のバイト単位です。全体一致の両端は書記素境界に限定しました。
UTF-8継続バイトからの開始はエラーにし、書記素内部のスカラー境界からの開始は次の書記素まで進めます。
照合器の作成は同期処理で、照合の再開に処理量の上限を指定できます。
ユーザー向けの契約とCLIの使い方は[正規表現照合](../regex.md)に記載しました。

REDでは公開APIの不足に加え、NULを含むパターンの誤検出と不一致時の過大な処理量を確認しました。
前者は`strchr()`が文字列終端のNULにも一致するためです。
後者は開始位置ごとの再走査をやめ、各入力位置で先行する候補をまとめる方式へ変更しました。
`a+b`と8192個の`a`の不一致では、修正前は1,000万処理に達しても終わらず、修正後は90114処理で終了しています。
入力長を倍にしたときの処理量にも上限を設け、実時間に依存せず検証します。
連続した量指定子は非対応の構文として診断することもRED/GREENで確認しました。

`make test-regex`でAPIとCLIが成功しました。
`make test-regex-win32`もamd64/i686の両方で成功し、サニタイザー付きのregex試験でも問題は見つかりませんでした。
既存の`build/tests/test_terminal`は101項目が成功しています。
`make test-public-headers`と、GTK3/GTK4のインストール先を使うpackage試験も成功しました。
コード生成用依存は`npm ci --prefix tools/codegen`で導入済みです。
GTK4の実行時にはXvfbのDRI3非対応を示すlibEGLの警告が出ましたが、描画方式の選択とfallbackの試験は成功しています。
コンパイル時の警告はありません。
コミットは`f9eace9`です。

## 段階3

番号付き・名前付きキャプチャと、キャプチャしないグループを追加しました。
全体一致は最左最長を維持します。同じ範囲なら左側の選択肢と貪欲な反復を優先し、反復グループには最後に参加した値を残す契約です。
未参加と空文字は別に返します。全体一致の両端は書記素境界ですが、内部のキャプチャ範囲はUTF-8バイト単位です。

公開APIがない状態でテストのREDを確認し、NFAのキャプチャ記録と結果の不変コピーを実装しました。
空の分岐を含む優先経路を保つため、状態の訪問済み判定は処理時に行います。
キャプチャ情報は状態数とグループ数で上限を定め、入力長とともに増えない構造です。
画面認識器はコンパイル済みプログラムを共有でき、認識時に保存した結果をIDとrevisionで返します。
再照合は行いません。取得済みの参照は更新・再走査・認識器解除・端末破棄後も利用でき、現在の画面に対する取得は`STALE`で検出できます。
結果のバイト容量を超えた場合は部分的な結果を公開せず、不完全の通知を返します。

CLIの`--captures`は、番号ごとの範囲と未参加を表示するオプションです。
CLIのRED/GREEN、独立APIとprimary/alternate画面の同一結果、予算1と4096での一貫性、NUL、結合文字、反復、容量制限を確認済みです。
amd64/i686のWine試験と、core全体を計装したAddressSanitizer/UndefinedBehaviorSanitizerの試験も成功しました。
reflow・解除後の寿命試験を含め、`make test-regex`、`make test-regex-win32`、`make test-public-headers`が成功しました。
coreの既存101試験と、追加試験を含むサニタイザー確認も成功しています。
キャプチャ付きの不一致検索でも、入力長を倍にしたときの処理量の上限を検証しました。

## 段階4

生成時と実行中に共通の`boxrig_input_config`を使い、Enter・矢印・Backspace・Deleteを一括交換できるようにしました。
生成時の入口は`boxrig_terminal_config.input`です。従来の個別メンバーを使う利用側も更新しました。
coreのsetterは全項目を検証し、矢印のバイト列をコピーしてから交換します。
取得APIの借用値をそのままsetterへ戻す場合も有効です。

API不足のREDを確認した後、両消去キーの全組み合わせ、6種類の修飾キー、press・repeat・releaseを送信バイトで確認しました。
固定Enterとmode 20追従、keypad、Kitty push/pop、画面revisionと端末モードの保持、入力バッファのコピーを検証しています。
不正値・上限超過・`malloc`失敗を注入した試験でも旧設定を保持できました。
サニタイザー付きのcore試験も成功しています。

GTK3・GTK4・X11・Wayland・Win32の各UIにも設定APIを追加しました。
対象コンポーネントの押下系列・保留キー・IME・貼り付け処理が残る場合は`BUSY`です。
複数コンポーネントで端末を共有する場合は、ホストが全入力系列の終了を確認する契約を記載しました。
GTKのnativeキー入力では変更前後のLF／CRを確認でき、保留pasteとpreedit中の変更も拒否できています。
Waylandの追加試験では既存の一括受信を途中で処理してしまう問題があり、検証位置を修正しました。既存のキュー最大深度の条件は維持しています。

`test-input-config`、`test-terminal-newline`、`test-terminal-key-mapping`、GTK3/GTK4のcodec試験、Wayland入力試験、X11入力試験が成功しました。
Win32の新しいcore試験と既存input adapter試験は、amd64/i686の両方でWine上で成功しています。
公開ヘッダーのC99/C++20試験と既存coreの101試験も成功しました。
実行中の設定交換は[キー設定の実行例](../../../examples/core/key_mapping.c)で観察できます。

## 段階5

`BOXRIG_ERASE_TTY`と、通信路の取得状態・1バイトの値を渡すAPIを追加しました。
NULと無効値も取得済みとして扱い、未取得の場合だけ該当キーのAutoへ戻ります。
Ctrlによる反転はBSとDELに限定し、任意のTTY文字は変換しません。
TTY情報は画面リセットで保持します。接続を切り替えるホストは明示的に破棄する契約です。

POSIX補助層の`boxrig_posix_get_tty_erase()`は、現在の`VERASE`を取得します。
失敗時は過去の値を残さず未取得へ戻します。
同梱のGTK3・GTK4・X11・Waylandドライバーが問い合わせるのは、対象キーの処理時です。
子の終了確認時にも情報を破棄し、起動前の値やタイマーによる監視に依存しません。

公開APIとPOSIXドライバーの不足をREDで確認した後、実PTYの子プロセスで値を変更する試験を追加しました。
BS・DEL・任意文字・NUL・無効値、Ctrl/Alt、Backspace/Delete/keypad Delete、press/repeatを子の受信バイトで確認できています。
legacy releaseは出力せず、Kittyの優先と画面リセット後の状態もcore試験で確認しました。
未取得と接続情報の破棄によるAuto復帰は、Linuxとamd64/i686のWineで成功しています。

`test-tty-erase`、`test-input-config`、`test-input-config-win32`、POSIX共通ドライバー試験が成功しました。
core・UI・POSIXを計装したAddressSanitizer/UndefinedBehaviorSanitizer試験でも問題は見つかりませんでした。
4種類のPOSIX GUIドライバーは警告なしでビルドできています。
`test-public-headers`も成功し、新しいAPIを通常includeするC99/C++20利用側でリンク・実行を確認しました。

## 段階6：共通UI・GTK・X11

入力の状態を`ENABLED`、`STOPPING`、`DISABLED`で表す共通型と、停止処理を追加しました。
禁止・再開のたびに入力世代を進め、開始済みpasteの終端がcoreへ投入されるまで再開を拒否します。
送信バッファに残るデータは保持し、端末からの問い合わせにも応答できます。
GTK3・GTK4・X11へ接続しました。Win32・Waylandの実装は続く作業です。

公開API不足のREDから実装を進め、未開始paste、遅いIME確定、押下中キー、出力容量不足を検証しました。
GTK3では、IME処理中の再入要求と、禁止をまたぐキーreleaseでIMEが確定するケースもREDで再現しています。
前者は状態を変えず`BUSY`を返し、後者はキーを離すまでIMEへ渡さないことで解消しました。
入力禁止中の選択・コピーと、再開後に到着するclipboard応答の破棄は、両版のウィジェット試験で確認済みです。
IBusの受信済み通知と未処理通知についても、禁止・再開後に端末出力へ混ざらないことを確認しました。
GTK3のX11経路と、GTK3・GTK4のWayland経路で成功しています。

`test-input-control`、`test-terminal-codec-frontends`とGTK3・GTK4のウィジェット試験が成功しました。
core・共通UI・GTKを計装したAddressSanitizer/UndefinedBehaviorSanitizerの試験も成功しています。
`test-public-headers`では、追加したGTK APIのC99/C++20利用側もリンク・実行できました。
GTK4のX11実行では、ヘッドレス環境のDRI3非対応と、既存AT-SPIソケットへの接続失敗が報告されました。
入力試験は成功しており、後続の入力専用試験では`GTK_A11Y=none`を指定しています。
アクセシビリティの受け入れ試験は、専用D-Bus環境で別途行う予定です。

X11にもアダプターとコンポーネントの公開APIを追加しました。
未開始paste、IME確定、押下中キーを破棄し、開始済みpasteの終端を再試行で投入します。
停止中も選択・コピー・focus表示を操作でき、端末の問い合わせには応答します。
古いclipboard応答が再開後の新しい要求を取り消す問題をREDで再現しました。
受信要求ごとに専用ウィンドウを用意し、古い応答と区別する修正で解消しています。
既存の大容量clipboard転送を含む入力試験とコンポーネント試験が成功しました。
X11・core・共通UIを計装したAddressSanitizer/UndefinedBehaviorSanitizerの試験と、公開ヘッダーのnative試験も成功しています。

## 後続段階の事前確認

GTK 3.24.41、GTK 4.14.5、GTK3/GTK4向けVTE 0.76.0、AT-SPI 2.52、Orca、MinGWのamd64/i686とWineが利用できます。
VTE 0.76.0はGTK3でAtkTextを実装していますが、GTK4でGtkAccessibleTextを実装していないことを実行時の型検査で確認しました。
比較用にVTE 0.78.2を`/tmp/boxrig-vte-sdk`へ導入し、GTK 4.14.5でGtkAccessibleTextが有効なことも確認済みです。
[VTE 0.78のGTK4公開API](https://gnome.pages.gitlab.gnome.org/vte/gtk4/class.Terminal.html)に対応する比較対象です。
外部ソースは変更していません。比較用のビルドでは無効にしたGNUTLS分岐と未使用の変数・ラベルに警告が出ました。
追加で、GTK3のVTE 0.76.0とGTK4のVTE 0.78.2を、独立したD-Bus・Xvfb環境で観測しました。
GTK4は公開APIの[`set_enable_a11y()`](https://gnome.pages.gitlab.gnome.org/vte/gtk4/method.Terminal.set_enable_a11y.html)でアクセシビリティを有効にしています。
型の検査に加え、AT-SPIのText、caret、選択、挿入・削除イベントを取得しました。
比較用のC/Node.jsドライバーとログは`/tmp/boxrig-vte-baseline*`に保存しています。

両版とも表示中の本文を返し、soft wrapでは改行を追加せず、実改行と末尾の空行は本文に含めていました。
日本語・結合文字・絵文字を含む`A日本é🙂\nline two\n\n\n`は、18文字でcaret位置は15です。
履歴へ移動すると本文も表示範囲に追従し、表示外のcaretは本文末尾へ制限されました。
行の置換、幅変更、代替画面との切り替えでも、native APIとAT-SPIで同じ本文を取得できています。
配置変更直後には古い本文が見えるため、観測はGTKのフレーム処理後に行いました。

VTE側にも比較上の注意点があります。
GTK3のAT-SPI文字数は、この18文字の本文に対してUTF-8バイト数の26を返しました。
GTK4の全選択は、本文を選択していても範囲17–17を返しました。
boxrigではこれらの不整合を再現せず、本文のUnicodeスカラー数と実際の選択内容を基準に検証します。
比較した実装は[VTE 0.76.0のAtkText](https://gitlab.gnome.org/GNOME/vte/-/blob/0.76.0/src/vteaccess.cc)と[VTE 0.78.2のGtkAccessibleText](https://gitlab.gnome.org/GNOME/vte/-/blob/0.78.2/src/vteaccess-gtk4.cc)です。

Orca 46.1では、両版とも追加出力`Hello boxrig.`に対する読み上げ文字列をログで確認しました。
GTK3はSpeech Dispatcherへの送信まで確認できています。
GTK4ではヘッドレス環境の音声サーバー待機を避けるため、比較専用設定で音声出力先を無効にしました。
そのため、GTK4で確認できたのはOrcaが生成した読み上げ文字列までで、実際の音声再生は未確認です。
ログは`/tmp/boxrig-vte-orca-gtk3.log`と`/tmp/boxrig-vte-orca-gtk4.log`です。
ユーザーのOrca設定は変更していません。

非同期起動には、[Cardioの公開`promises::start_new()`](../../../deps/cardio/README_ja.md)を使う方式を検証しました。
[APIコメント](../../../deps/cardio/include/cardio.h)でも、呼び出しごとの実行スレッドと、元のdispatcherへ完了を返す契約を確認しています。
MinGW amd64の小さなGUIドライバーでは、待機中の操作とは独立してウィンドウメッセージを処理できました。
GUIから取消を要求し、操作側の資源回収が完了してから完了通知を受けることもWineで確認しました。
ドライバーは`/tmp/boxrig-cardio-spawn-probe.cpp`、ログは`/tmp/boxrig-cardio-spawn-probe.log`です。
これは実行基盤の確認であり、実際のConPTY起動・終了処理や実Windowsでの確認ではありません。
段階9では、同時実行数を制限した操作の所有権、接続世代、取消と完了の競合を実装して検証します。
POSIXのfork後処理を別スレッドから安全に実行できるかも、段階8の起動経路と合わせて確認します。
段階8だけで段階9を完了としません。
全体テストも未実施です。
