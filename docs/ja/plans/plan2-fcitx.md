# 直接接続IMEのfcitx対応計画

IBusへ直接接続する経路に、fcitx 5とfcitx 4のバックエンドを追加します。実装順は、fcitx 5の入力・候補表示・自動検出を完成させた後に、fcitx 4へ進む形です。GTKやWaylandの標準経路がIMEを処理する場合は、その経路を維持します。

調査日は2026年10月14日、プロジェクトの調査対象は`8026270`です。本書には初回の実装調査、Debian／Ubuntuの収録版調査、fcitx 4の追加分析をまとめました。以下は調査時点の設計と実装計画です。実装の進捗は第10節に記録します。

## 1. 目的と対応範囲

目的は、OSでfcitxを使用している場合にも、現在の直接IBus経路に相当する場所でIMEを使えるようにすることです。IBus／fcitxの選択に加え、fcitx 4と5の通信方式を区別します。パッケージがインストールされているだけでは接続先を決めず、環境設定と稼働中のサービスを確認します。

| 環境 | 現在の実装 | 今回の対応範囲 |
|---|---|---|
| native Wayland | text-input経由、または直接IBus | 直接接続の部分と、その自動選択 |
| GTK 3／Wayland、GTK 4／Wayland | GtkIMContext経由、または条件に応じて直接IBus | 直接接続の部分と、その自動選択 |
| GTK 3／X11 | GtkIMContext経由、または条件に応じて直接IBus | 直接接続の部分と、その自動選択 |
| GTK 4／X11 | IBus指定時もGTKのIBusモジュール経由 | GTKへの委譲を維持 |
| native X11 | `XOpenIM()`によるXIM経由 | 直接接続の追加対象外 |

選択処理は[GTKのime_probe.c](../../../src/gtk/ime_probe.c)と[Waylandのibus.c](../../../src/wayland/ibus.c)、直接接続の要求管理は[共通クライアント](../../../src/ime/client.c)、IBusの通信処理は[IBusバックエンド](../../../src/ibus/client.c)にあります。GTKで実際にfcitxモジュールを使用している場合や、`GTK_IM_MODULE`で明示指定している場合は、AUTOによって独自接続へ切り替えません。

native X11については、[IMEドキュメント](../input-ime.md)に直接IBusという記載がありますが、[実装](../../../src/x11/input.c)はXIMです。実装時の文書更新でこの不一致も修正します。XIM経由のfcitx動作を、今回追加する直接接続バックエンドの動作実績には数えません。

## 2. Debian／Ubuntuの収録バージョン

以下は公式アーカイブで確認した本体の収録版です。ディストリビューション固有のパッケージ改訂番号は省略しています。標準で選択されるIMEや、各OSのサポート期間を示す表ではありません。PPAとBackportsは対象外です。

### Debian

| リリース | `fcitx`：4系 | `fcitx5`：5系 |
|---|---|---|
| Debian 10（buster） | [4.2.9.6](https://sources.debian.org/src/fcitx/) | [2018-11-28の開発スナップショット](https://sources.debian.org/src/fcitx5/) |
| Debian 11（bullseye） | [4.2.9.8](https://packages.debian.org/bullseye/fcitx) | [5.0.5](https://packages.debian.org/bullseye/fcitx5) |
| Debian 12（bookworm） | [4.2.9.9](https://packages.debian.org/bookworm/fcitx) | [5.0.21](https://packages.debian.org/bookworm/fcitx5) |
| Debian 13（trixie） | [4.2.9.9](https://packages.debian.org/trixie/fcitx) | [5.1.12](https://packages.debian.org/trixie/fcitx5) |

Debian 10のfcitx5パッケージは`0~20181128+ds1-1`です。正式な5.0リリースより前の開発版なので、後続の5系と同じ互換性を前提にしません。

Debian 12ではfcitx 4と5を同時インストールできず、利用者がどちらかを選択します。Debian 13ではfcitx 4が非推奨となり、Debian 14のforkyで削除予定です。ただし、Debian 13に4系が存在しないという意味ではありません。[Debian 12リリースノート](https://www.debian.org/releases/bookworm/amd64/release-notes/ch-information.en.html)、[Debian 13リリースノート](https://www.debian.org/releases/trixie/release-notes/issues.en.html#deprecated-components-for-trixie)

### Ubuntu

20.04以降のLTSと、24.04以降の中間リリースを確認しました。表のパッケージはいずれもUniverseに属します。公開終了後のリリースはLaunchpadの発行履歴を参照しています。

| リリース | `fcitx`：4系 | `fcitx5`：5系 |
|---|---|---|
| Ubuntu 20.04（focal） | [4.2.9.7](https://launchpad.net/ubuntu/focal/+source/fcitx) | [2020-01-28の開発スナップショット](https://launchpad.net/ubuntu/focal/+source/fcitx5) |
| Ubuntu 22.04（jammy） | [4.2.9.8](https://packages.ubuntu.com/jammy/fcitx) | [5.0.14](https://packages.ubuntu.com/jammy/fcitx5) |
| Ubuntu 24.04（noble） | [4.2.9.9](https://packages.ubuntu.com/noble/fcitx) | [5.1.7](https://packages.ubuntu.com/noble/fcitx5) |
| Ubuntu 24.10（oracular） | [4.2.9.9](https://launchpad.net/ubuntu/oracular/+source/fcitx) | [5.1.10](https://launchpad.net/ubuntu/oracular/+source/fcitx5) |
| Ubuntu 25.04（plucky） | [4.2.9.9](https://launchpad.net/ubuntu/plucky/+source/fcitx) | [5.1.12](https://launchpad.net/ubuntu/plucky/+source/fcitx5) |
| Ubuntu 25.10（questing） | [4.2.9.9](https://packages.ubuntu.com/questing/fcitx) | [5.1.14](https://packages.ubuntu.com/questing/fcitx5) |
| Ubuntu 26.04（resolute） | [4.2.9.9](https://packages.ubuntu.com/resolute/fcitx) | [5.1.19](https://packages.ubuntu.com/resolute/fcitx5) |

Ubuntu 20.04のfcitx5パッケージは`0.0~git20200128.9e3bc8d+ds1-3`です。こちらも正式な5.0より前の開発版です。

### boxrigの検証対象

[現在の配布対象](../building.md)はDebian 13、Ubuntu 24.04、Ubuntu 26.04です。fcitx 5は5.1.7を最低検証版とし、5.1.12と5.1.19も検証します。fcitx 4の検証対象は、これらのOSに共通する4.2.9.9です。

この範囲でも4系と5系の両方が提供されています。OS名やリリース番号による分岐では、実行中のIMEを選択できません。古いOSの収録版を調査したことと、古いOSまで動作保証を広げることは別に扱います。本計画には5.0系、4.2.9.8以前、正式リリース前のfcitx5への対応を含めません。

## 3. 共通IME層の設計

現在の[IBusクライアントAPI](../../../src/ime/client.h)から、接続先に依存しない部分を`src/ime/`へ整理します。入力要求の順序、フォーカスと文書の世代、入力先の寿命、確定・削除・転送キーの反映を共通化します。IBus固有のオブジェクト、fcitxのD-Bus型と能力フラグは、各バックエンドの担当です。

ホスト側は、共通の操作と通知を通じてバックエンドを使用します。関数ポインターを持つインターフェイスと所有データを組み合わせ、GTKとnative Waylandで同じ接続処理を使います。既存のIBusバックエンドもこのインターフェイスへ接続し、IBus用ライブラリを残します。

接続・セッション・非同期コールバックはクライアントの所有物です。文字列と装飾データのコピーは、従来どおり要求が所有します。ビューは借用参照とし、破棄前に要求とセッションを切り離します。切断、入力禁止、文書更新、フォーカス変更で古い応答が別の入力先へ届かないことを共通契約にします。

[現在のイベントループ](../../../src/ime/source.c)と同様に、専用の`GMainContext`をホストの待機処理へ接続します。接続、メソッド呼び出し、サービス監視、アドレスファイル監視を非同期で処理します。GTKのデフォルトコンテキストを再入実行したり、独自のワーカースレッドや定期ポーリングを追加したりしません。

fcitx 5と4の通信には、既存のGLib／GIOを利用したGDBusバックエンドを追加します。公式の`Fcitx5GClient`には、デフォルトコンテキストへ登録する再接続タイマーと、キー処理の通信エラーを未処理と同じ`FALSE`にする完了APIがあります。boxrigでは専用コンテキストの使用と、結果不明のキーを再送しない区別が必要です。[公式クライアント実装](https://github.com/fcitx/fcitx5-gtk/blob/master/fcitx-gclient/fcitxgclient.c)、[GLibタイマーの仕様](https://docs.gtk.org/glib/func.timeout_add_full.html)

fcitx 4の`FcitxClient`は、通信失敗を`-1`として未処理の`0`と区別します。この点は5系のクライアントと異なります。ただし、接続管理には同様のタイマーがあり、詳細な`GError`も完了APIから取得できません。4系だけ別の接続管理ライブラリを導入せず、共通層で非同期完了と寿命を管理します。[4系の完了APIとコメント](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.c#L471)、[4系の接続実装](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxconnection.c#L125)

GDBusの呼び出しは、専用コンテキストをthread-defaultにした範囲で登録します。返信型を指定し、完了時の通信エラーと正常な未処理応答を区別します。接続前に確認するのは、サービスの所有者です。探索のための自動起動は無効にします。[GDBus非同期呼び出し](https://docs.gtk.org/gio/method.DBusConnection.call.html)、[自動起動を抑止する呼び出しフラグ](https://docs.gtk.org/gio/flags.DBusCallFlags.html)

公開設定にはfcitx 5／4の明示指定を追加します。現在のIBus固有の接続状態、エラー件数、候補表示の理由、サンプルの起動設定も、選択したバックエンドを表せる形へ変更します。利用者向け診断の項目は、要求した経路、実際の経路、接続状態、候補表示の所有者、選択・フォールバックの理由です。それぞれを区別して表示します。

## 4. fcitx 5の実装分析

fcitx 5には、非同期キー処理、確定文字列、装飾付き未確定文字、フォーカス、カーソル位置、周辺文字列と削除要求、候補一覧の通知があります。初回調査の機能対応に加え、最低検証版5.1.7のD-Bus定義でも必要な経路を確認しました。[公式クライアントAPI](https://github.com/fcitx/fcitx5-gtk/blob/master/fcitx-gclient/fcitxgclient.h)、[5.1.7のD-Bus実装](https://github.com/fcitx/fcitx5/blob/5.1.7/src/frontend/dbusfrontend/dbusfrontend.cpp)

セッションバスの`org.fcitx.Fcitx5`に接続し、`org.fcitx.Fcitx.InputMethod1`の`CreateInputContext`で入力コンテキストを生成します。入力コンテキストのインターフェイスは`org.fcitx.Fcitx.InputContext1`です。生成時にアプリケーションと表示環境を伝え、キー・能力・座標を各メソッドの型へ変換します。

fcitx 5のIBus互換機能だけでは、現在のboxrigと同じ候補表示を保証できません。調査した互換実装はIBusの候補一覧・補助文字列の能力指定を、fcitxのクライアント側候補表示へ反映していません。直接fcitx 5へ接続するバックエンドを使用します。[IBus互換実装](https://github.com/fcitx/fcitx5/blob/master/src/frontend/ibusfrontend/ibusfrontend.cpp)

候補通知は、表示中の候補、ページ内の選択位置、前後ページの有無、上下の補助文字列を持ちます。[現在の候補モデル](../../../include/boxrig/ui/candidates.h)は、候補全体と絶対位置を前提にしています。全体の候補数を推測して埋めず、受信したページだけを表せる形式を追加します。IBusの全体表、fcitx 5のページ、後述するfcitx 4の文字列パネルは別形式です。描画側が形式と情報の有無を判断できる構造にします。

候補表示の所有者は、要求した能力だけでは確定できません。fcitx 5.1.7では、X11の設定やWaylandのパネル環境に応じて`ClientSideInputPanel`が抑制されます。実際の通知と環境を確認し、未確認の状態をクライアント表示成功として診断しません。システム表示とクライアント表示をそれぞれ実測します。[5.1.7の候補表示能力の調整](https://github.com/fcitx/fcitx5/blob/5.1.7/src/frontend/dbusfrontend/dbusfrontend.cpp#L373)

カーソル位置では、X11の絶対座標とWaylandの相対座標を区別します。5.1.7にある`SetCursorRectV2`と倍率の扱いを確認し、論理座標と出力ピクセルを二重に変換しないようにします。キーコード、解放イベント、未確定文字の位置単位、フォーカス喪失時の処理もIBusから機械的に流用しません。

## 5. fcitx 4の実装分析

### 5.1. 調査対象と実装方針

fcitx 4.2.9.9の公式タグを対象に、公開クライアントAPIのコメント、IPC実装、GTKクライアント、候補表示処理を確認しました。基本的な変換・確定・キャンセル・周辺文字列編集は実装可能です。fcitx 5とは別のD-Busバックエンドが必要です。

公式のフロントエンド説明でも、確定・転送キー・未確定文字・クライアントUIの通知と、能力フラグによる使い分けが示されています。本計画では、この通信契約をGDBusで扱います。fcitx内部の非公開ヘルパーを呼び出したり、上流の実装を書き換えたりする構成にはしません。[公式開発ガイド](https://fcitx.github.io/developer-handbook/article-implement-frontend.html)、[4.2.9.9の公開クライアントヘッダー](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.h)

### 5.2. 接続先と入力コンテキスト

4系は`org.fcitx.Fcitx-<表示番号>`を使用します。例えば`DISPLAY=:0`なら`org.fcitx.Fcitx-0`です。表示番号は`DISPLAY`から取り、未設定時は公式実装と同じく0を候補にします。`-0`だけを固定探索する実装にはしません。[表示番号の取得](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-utils/utils.c#L416)、[サービス登録](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/module/dbus/dbusstuff.c#L180)

接続先のアドレスには、セッションバス以外もあります。公式クライアントは`FCITX_DBUS_ADDRESS`、設定ディレクトリ内の`fcitx/dbus/<machine-id>-<表示番号>`、セッションバスを扱います。アドレスファイルにはNUL終端アドレスとPID情報が入るため、通常のテキスト設定ファイルとして読みません。[接続先探索の実装](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxconnection.c)

4系バックエンドでは、この探索に対応します。ファイルのサイズ・形式と接続先を検証し、古いPIDや消失したアドレスを稼働中サービスとみなしません。ファイル変更とサービス所有者の変更を監視し、再接続時には入力コンテキストを作り直します。環境変数は選択の手掛かりであり、接続成功の代わりにはしません。

入力メソッドのオブジェクトは`/inputmethod`、インターフェイスは`org.fcitx.Fcitx.InputMethod`です。`CreateICv3`にアプリケーション名とPIDを渡し、返された整数IDから`/inputcontext_<ID>`を使用します。入力コンテキストのインターフェイスは`org.fcitx.Fcitx.InputContext`です。[IPCの名前定義](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/ipc/ipc.h)、[生成処理](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.c#L785)

### 5.3. fcitx 5との通信上の差

| 操作・通知 | fcitx 4 | fcitx 5 | boxrigでの扱い |
|---|---|---|---|
| コンテキスト生成 | `CreateICv3(si) → ibuuuu` | `CreateInputContext(a(ss)) → oay` | 生成と破棄をバックエンドごとに実装 |
| キー処理 | `ProcessKeyEvent(uuuiu) → i` | `ProcessKeyEvent(uuubu) → b` | 解放フラグと返信型を個別に変換 |
| 能力指定 | `SetCapacity(u)` | `SetCapability(t)` | 32bit／64bitと能力の意味を区別 |
| カーソル矩形 | `SetCursorRect(iiii)` | 同メソッドと`SetCursorRectV2(iiiid)` | 座標系と倍率を個別に扱う |
| キー転送 | `ForwardKey(uui)` | `ForwardKey(uub)` | 通常のホスト入力へ戻し、IMEへ再投入しない |
| 装飾付き未確定文字 | `UpdateFormattedPreedit(a(si)i)` | 同じ基本形式 | 内容の意味と能力指定を確認して共通表現へ変換 |
| 候補UI | `UpdateClientSideUI(sssssi)` | 装飾付き文字列、候補配列、選択位置、ページ情報 | 同じ候補構造として復号しない |

型の根拠は[4系IPC定義](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/ipc/ipc.c#L134)と[5.1.7のD-Bus定義](https://github.com/fcitx/fcitx5/blob/5.1.7/src/frontend/dbusfrontend/dbusfrontend.cpp#L437)です。4系のキー種別は押下が0、解放が1で、処理結果は通常0が未処理、1が処理済みです。通信失敗や不正な返信は未処理に変換せず、結果不明として扱います。

現在の共通IBus入口が受け取るのは、IBus用修飾フラグとキーコードです。GTK側はハードウェアキーコードから8を引き、native Wayland側はevdevコードをそのまま渡します。一方、4系の公式GTKクライアントはハードウェアキーコードを渡します。共通入口には物理コードの基準、押下／解放、修飾状態、時刻を保持し、各バックエンドの境界で変換します。[GTK 3の入力処理](../../../src/gtk3/input.c)、[GTK 4の入力処理](../../../src/gtk4/input.c)、[fcitx 4のキー送信](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/gtk2/fcitximcontext.c#L650)

[現在のnative Wayland入力処理](../../../src/wayland/input.c)はキーイベントの`time`を破棄しています。fcitx用にはこの値を入力キューへ保持します。変換後の意味を確認する対象は、通常キー、修飾キー、解放、リピート、転送キーです。IBusとfcitxの両方で検証します。

### 5.4. 未確定文字と周辺文字列

システム側で候補を表示する場合は、`CAPACITY_PREEDIT`と`CAPACITY_FORMATTED_PREEDIT`を指定します。`UpdateFormattedPreedit`の装飾とUTF-8バイト単位のカーソル位置を、既存の[未確定文字処理](../../../src/ime/preedit.c)へ正規化します。4系の装飾フラグとIBusの属性値を同じ数値として扱いません。[通知のAPIコメント](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.c#L1114)、[公式GTKクライアントの装飾変換](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/gtk2/fcitximcontext.c#L704)

周辺文字列は`SetSurroundingText(suu)`で送信し、`DeleteSurroundingText(iu)`を受けます。周辺文字列のカーソル・選択位置と削除長は文字単位で扱われるため、boxrigのUTF-8バイト範囲との変換が必要です。既存の文書スナップショットと世代確認を再利用し、変更された文書へ古い削除要求を適用しません。[周辺文字列と削除処理](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/ipc/ipc.c#L1106)

周辺文字列の能力は、編集可能な文書を実際に提供できる場合だけ通知します。端末に表示された過去の出力を、自由に編集できる入力文書として送らない契約を維持します。テスト対象は、日本語、結合文字、絵文字、選択範囲、範囲外削除です。

### 5.5. 候補UIの制約と表示方法

4系の`UpdateClientSideUI`は、上側の補助文字列、下側の補助文字列、未確定文字列、候補文字列、IME名、カーソル位置を送ります。候補は配列ではなく、選択キー・候補・補足を連結した一つの文字列です。APIコメントにも一行の候補文字列と記載されています。[通知のAPIコメント](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.c#L1097)、[候補文字列の生成](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx/ui.c#L934)

この通知から、候補ごとの境界、現在の選択項目、総候補数、前後ページの有無を確実には復元できません。空白や数字で文字列を分割すると、候補そのものに含まれる文字との区別ができません。候補文字列はそのまま表示し、選択やページ移動は通常のキー入力でIMEに処理させます。候補クリックや個別の選択強調を、この文字列から推測して実装しません。

さらに、`CAPACITY_CLIENT_SIDE_UI`を指定すると、4系の通常のUI更新処理は`UpdateClientSideUI`を通知して戻ります。この経路では、その後にある通常の`UpdatePreedit`を呼びません。候補通知と装飾付き未確定文字の両方が常に届く設計にはできません。[UI更新の分岐](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx/ui.c#L977)

したがって、4系のクライアント表示には、未確定文字と候補をまとめて扱う文字列パネルを追加します。保持する情報は、上下の補助文字列、パネル用未確定文字とカーソル、候補文字列です。候補が空でも未確定文字や補助情報があれば表示できるようにします。本文内の未確定文字表示との二重描画を防ぎ、パネルの文字列をそのまま確定本文として扱いません。

4系のクライアント表示では、個別候補の選択強調や文節装飾を5系と同じ精度で再現できない場合があります。これは対応範囲として明記します。完了条件は、受信した候補と未確定文字を欠落なく表示し、キーボードで候補選択・ページ移動・確定・キャンセルができることです。装飾が必要なX11環境では、SYSTEM指定によるシステム候補表示と装飾付き未確定文字も検証します。

[既存の候補スナップショット](../../../include/boxrig/ui/candidates.h)は、補助文字列だけの表示を想定せず、候補の配列と絶対カーソル位置を持ちます。4系のパネルを候補一件として詰め込まず、文字列パネルとして明示的に区別します。長い通知の上限、不正UTF-8、空の通知による非表示、フォーカス移動時の消去も定義が必要です。

### 5.6. フォーカス喪失と座標

4系では`FocusOut`の処理中に、設定に応じて未確定文字が確定されます。現在のboxrigはIBusのpreedit modeを見てローカルで確定し、その後に入力コンテキストを切り離します。この順序をそのまま4系へ適用すると、二重確定や確定通知の取りこぼしが起こり得ます。[4系のFocusOut](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/ipc/ipc.c#L998)、[4系の確定規則](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx/ui.c#L701)、[現在のboxrigのフォーカス処理](../../../src/ime/client.c)

4系ではサーバー側の確定を受け取る方式を基本にします。確定を許可するフォーカス喪失では、旧入力先を参照する終了要求を作り、`FocusOut`完了までの正当な確定通知を旧入力先へ適用してから破棄します。新しい入力先の世代へ通知を移しません。キャンセル、入力禁止、入力先破棄では`Reset`などを先行させ、保留応答を破棄します。通信結果が不明なら、ローカル文字列による確定やキー再送で補いません。

`CAPACITY_CLIENT_UNFOCUS_COMMIT`は、クライアント側で確定する別の契約です。サーバー確定と併用しません。特に4系のパネル用未確定文字からは、確定してよい部分を常に判断できるとは限らないため、このフラグを便宜的に有効にしない方針です。

座標について、4系にも`CAPACITY_RELATIVE_CURSOR_RECT`があり、公式GTKクライアントはWaylandで指定します。ただし、その扱いはUI側にも依存します。4.2.9.9のKimpanelには相対指定を渡す処理がありますが、これだけで全てのシステム候補ウィンドウの配置を保証できません。また、4系の矩形通知には5系のV2に相当する倍率引数がありません。[能力フラグ](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx/frontend.h#L55)、[GTKの能力設定](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/frontend/gtk2/fcitximcontext.c#L1147)、[Kimpanelの座標処理](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/ui/kimpanel/kimpanel.c#L488)

このため、直接fcitx 4を使うWaylandのAUTOでは、描画可能ならクライアント側の文字列パネルを使用します。GTK 3／X11では既存の座標取得を使い、SYSTEMとCLIENTをそれぞれ検証します。明示したSYSTEM指定は維持し、配置を保証できない環境や描画機能の不足は診断へ出します。Waylandの相対座標をX11の絶対座標として送る実装にはしません。

### 5.7. fcitx 5の4系互換サービスとの区別

fcitx 5は4系のD-Busプロトコルも提供します。そのため、`org.fcitx.Fcitx-0`が存在しても、本物の4系デーモンが動作しているとは限りません。[公式の通信方式説明](https://fcitx-im.org/wiki/How_does_an_application_talk_to_Fcitx)

さらに、5.1.7の4系互換実装は`UpdateClientSideUI`を実装しない旨をコード中に明記しています。4系互換経路で通常入力が成功しても、4系のクライアントパネルが動く保証にはなりません。[fcitx 5.1.7の4系互換実装](https://github.com/fcitx/fcitx5/blob/5.1.7/src/frontend/fcitx4frontend/fcitx4frontend.cpp#L253)

AUTOでは、同じサービスの5系ネイティブ経路が利用できる場合はそちらを選びます。同一バス上の所有者とインターフェイスを確認し、互換サービスを別のIMEとして数えません。異なるバスのunique nameを同一性判定に流用しません。4系経路しか確認できない場合は通信能力を検証し、デーモンの実体を根拠なく4系と断定しない診断にします。

4系の受け入れ試験には、実際のfcitx 4.2.9.9デーモンを使用します。5系の互換フロントエンドだけで試験を済ませません。4系パネルの通知がない互換実装に対して、能力指定が成功しただけでクライアント表示を有効扱いにしないことも検証します。

## 6. 自動検出と経路選択

### 6.1. フレームワークへの委譲と直接接続を分ける

最初の判断は、GTK／Waylandへの委譲可否です。直接接続が必要な場合だけIMEの種類を選びます。既存のGTKモジュール指定と、利用可能な標準経路を尊重します。候補をクライアントで表示したいという理由だけで、明示されたGTKモジュールを置き換えません。

native Waylandの現在のライブラリ既定値はWAYLANDで、[付属ターミナル](../../../examples/boxrig_term/wayland/native_host.c)はAUTOを明示しています。本計画では[設定初期化関数](../../../src/wayland/context.c)の既定値をAUTOに変更します。明示的なWAYLAND指定は維持し、標準経路を優先する条件もテストします。

### 6.2. 直接接続するバックエンドの選択

| 条件 | 選択方針 |
|---|---|
| 直接接続の対象環境で、アプリケーションがIBus／fcitx 5／fcitx 4を明示 | 指定した方式に接続。失敗時に別の直接接続先へ黙って切り替えない |
| GTKが処理する経路、またはGTKホストのAUTOで明示的な`GTK_IM_MODULE`がある | GTK経由を維持。下記の直接接続探索を開始しない |
| 直接接続が必要で、fcitxを示す設定がある | 稼働中のfcitx 5を優先し、次に対応するfcitx 4経路を確認 |
| 直接接続が必要で、IBusを示す設定がある | 既存のIBusアドレス取得方式で接続先を確認 |
| 設定がなく、有効なサービスが一つだけある | そのサービスを選択 |
| 設定がなく、独立したサービスが複数ある | 有効な既存選択を維持。初回はfcitx 5、IBus、fcitx 4の順で選び、曖昧だったことと選択理由を診断へ出す |
| 接続先がない、または指定先へ接続できない | 利用可能なGTK／Waylandの経路へ戻し、直接IMEが未接続であることを診断へ出す |

この表は第1節の対応範囲内で適用します。GTK 4／X11に直接接続経路を追加するものではありません。明示的なバックエンド指定と、AUTOでの環境変数によるヒントも区別します。

環境設定では、直接接続の判断に使える`GTK_IM_MODULE`、`XMODIFIERS`、明示アドレスを確認します。`fcitx`という値は4系と5系の両方で使われるため、世代の判定には使いません。設定が競合する場合は、アプリケーション指定、当該ホストに適用されるモジュール指定、`XMODIFIERS`、明示アドレスの順で意図を解釈し、競合も診断に残します。

Waylandでは`GTK_IM_MODULE`を設定しない構成も公式に案内されています。環境変数がないだけで探索を打ち切りません。逆に、環境変数だけが残っている場合もあるため、実際の所有者と入力コンテキスト生成の成功まで確認します。[Waylandでの公式設定](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland)

fcitx 5はセッションバスの`org.fcitx.Fcitx5`、fcitx 4は表示番号を含むサービスと4系のアドレス探索、IBusは既存のアドレス取得処理を使います。存在確認のためにデーモンを起動せず、実行ファイルの有無やパッケージ一覧を判定条件にしません。fcitxのIBus互換サービスも、独立したIBusと混同しないように扱います。

### 6.3. 切断・再接続と入力の所有者

一つの入力先に対し、キーを処理する経路は常に一つにします。GTK、Wayland text-input、直接接続の間で同じキーを二重配送しません。新しいサービスが起動しただけでは、処理中の変換を別のIMEへ移しません。

所有者の消失や接続断を検出したら、未確定文字と候補を消去し、旧世代の応答を無効にします。再接続では能力、フォーカス、周辺文字列、カーソル位置を送り直します。結果不明の送信済みキーを再送しないことは、[既存の契約](../../../src/ime/client.c)です。これも維持します。サービス名が同じでも、所有者が変われば別の接続として扱います。

## 7. 段階的な実装計画

各段階でビルド可能な状態を保ち、付属ターミナルまたはテストドライバーから成果を観測できるようにします。挙動を確認するテストを先に追加し、REDを確認してから実装し、GREENを確認します。コミットの単位も、各段階で確認できる成果物です。段階3の完了条件を満たすまで、fcitx 4の実装には着手しません。

| 段階 | 成果物 | 完了条件 | コミットの例 |
|---|---|---|---|
| 1 | IBusを維持した共通IME層 | IBusの変換、候補、周辺文字列編集、フォーカス、切断・再接続が既存ドライバーで動作し、専用イベントループと入力世代の契約を維持 | `refactor: extract shared direct IME transport` |
| 2 | fcitx 5の明示接続と入力 | 対象のGTK／native Waylandで、明示指定による変換・確定・キャンセル・キー転送・周辺文字列編集を観測できる。通信失敗時に二重入力しない | `feat: add direct fcitx5 input` |
| 3 | fcitx 5の候補表示と自動検出の完成 | ページ形式の候補、候補所有者、座標・倍率、自動選択、GTK委譲、停止・再起動を検証。5.1.7／5.1.12／5.1.19の実デーモン試験と全体テストが成功し、5系の公開設定・診断・利用者向け文書がそろう | `feat: complete fcitx5 candidates and detection` |
| 4 | fcitx 4の明示接続と入力 | 4.2.9.9の実デーモンへ接続し、GTK 3／X11のSYSTEM表示で変換・確定・キャンセル・装飾付き未確定文字・周辺文字列編集が動作。表示番号、接続アドレス、終了時の確定も検証 | `feat: add direct fcitx4 input` |
| 5 | fcitx 4の文字列パネルと自動検出 | Waylandを含む対象経路でパネル、未確定文字、キーによる候補選択・ページ移動が動作。4／5の区別、互換サービス、切断・再接続、入力禁止とフォーカス変更を検証 | `feat: add fcitx4 panels and detection` |
| 6 | 配布環境での統合確認と文書の確定 | 現在の配布対象で両世代をそれぞれ検証。IBus、GTK委譲、native XIMの回帰を含む全体テストが成功し、公開API・診断・サンプル・文書の対応範囲が一致 | `doc: document direct fcitx support and limits` |

段階1の共通化は、IBusの振る舞いを変えずに完了させます。fcitx 4の候補パネルなど、その段階では使用しない機能を先に実装しません。段階2ではシステム候補を利用できる構成と入力ドライバーを使い、段階3でクライアント候補を含む5系の対応を完成させます。

段階4の成果物は、実際の4系で基本入力を確認できるものにします。Waylandでの4系クライアント候補を後回しにした状態は、4系対応全体の完了とは扱いません。段階5で文字列パネルを接続し、段階6で配布先ごとの再現結果と表示上の制約を確定します。

各段階では必要な対象テストを実行します。全体テストの実行時点は、5系対応の完了時と全作業の完了時です。[Makefile](../../../Makefile)の`make test`を使用します。新しい失敗が出た場合は、個別の条件分岐を重ねる前に、共通化や選択規則に問題がないか計画へ戻って確認します。

## 8. 受け入れ試験

### 通信と自動選択

| 試験条件 | 確認する結果 |
|---|---|
| IBusのみ、fcitx 5のみ、fcitx 4のみ | 環境設定あり／なしの両方で選択と入力が成立 |
| IBusとfcitxの独立稼働、fcitx 5と4系互換サービス | 優先規則どおりに選択し、互換サービスを独立したIMEと誤認しない |
| 矛盾した設定、古い明示アドレス、インストール済みだが停止中 | 未接続の理由を診断し、検出によってデーモンを起動しない |
| 4系の表示番号が0以外、`DISPLAY`なし、専用アドレス | 想定した4系の接続先だけを使い、別セッションの候補へ誤接続しない |
| 4系アドレスファイルの変更・削除・不正形式・古いPID | 停止や再起動に追従し、不正な情報で接続成功と判定しない |
| 返信前の切断、タイムアウト、異常な返信型、所有者の交代 | 結果不明のキーを再送せず、次世代に古い通知を適用しない |
| 明示されたGTKモジュール、GTK 4／X11、標準Wayland経路 | 委譲経路を維持し、キーや確定文字を二重配送しない |

### 入力と表示

通常の日本語変換だけでなく、押下と解放、修飾キー、リピート、ショートカット、転送キーを確認します。確定文字と周辺文字列の削除が同じ入力要求から届く場合も、既存の順序で一度だけ反映することを検証します。

フォーカス移動中、入力禁止中、ビュー破棄中に遅延返信を発生させます。4系では、FocusOut時に確定する設定と確定しない設定の両方を確認します。旧入力先に適用すべき終了時の確定と、破棄すべき遅延キー応答を区別できることが完了条件です。

5系はページ単位の候補と上下の補助情報を検証します。4系は、候補中に空白・数字・句読点が含まれる場合も文字列を変更しないこと、候補なしの未確定文字や補助情報も表示できることを確認します。長い候補、狭いビュー、空通知、入力キャンセル、フォーカス移動での表示と消去も対象です。

各対応レンダラーで、候補とカーソルの位置を倍率変更、ウィンドウ移動、ビュー端で確認します。表示内容と配置を実行結果から検証し、ソースコードや画像ファイルの存在だけを確認するテストにはしません。時間差による二重表示や消去遅れは、イベント列の検証と必要な動画記録で確認します。マスター画像を追加する場合は目視確認します。

### 試験環境

既存の[GTK経路テスト](../../../tests/gtk/test_ime_protocol.c)、[IBus候補テスト](../../../tests/wayland/test_ibus_candidates.c)、[Waylandテストドライバー](../../../tests/wayland/run_ibus_context.mjs)を基に、fcitx用の試験を追加します。決められた応答を返す模擬D-Busサービスで、異常系と通知順を再現します。実デーモンには入力エンジンを用意します。そこで確認するのは、変換・候補表示・設定切り替えです。

準備完了や応答完了は、サービス所有権や明示的な完了通知で同期します。起動完了の判定に必要なのは実際の通知であり、一定時間のsleepではありません。5系と4系のパッケージが競合する環境では、独立したコンテナまたはVMとセッションバスを用意します。Waylandの候補配置は実際のコンポジター上で検証します。

実装・実測に必要なデーモン、入力エンジン、表示サーバーがない場合は、必要なパッケージの導入を案内します。fcitx 5の互換サービスで代用して4系の実測を完了扱いにしません。外部コードの変更は行わず、上流の通知だけでは再現できない表示情報は、実測結果と出典を利用者向け文書に記載します。

## 9. 調査の完了条件と現在の状態

今回の調査の完了条件は、リリース別の収録版を保存すること、fcitx 5の完成後に4系へ進む順序を明記すること、4系の接続・入力・候補表示・自動検出の分析を根拠付きで示すこと、実行可能な段階と受け入れ条件を保存することです。本書への反映により、これらの条件を満たしました。

調査段階で変更したのは本書だけです。プロジェクトのコードと公式APIコメント・実装・ドキュメントを照合しましたが、fcitx 4／5の実デーモンでの動作は未検証です。調査時点では、この環境に両デーモンはありませんでした。機能実装、ビルド、全体テストは今回の調査には含めていません。本書はビルド入力ではないため、文書更新に対するビルド確認は不要です。

## 10. 実装の進捗

### 段階1完了

入力要求の所有権、通知順序、文書世代、確定と周辺文字列編集、候補のコピー、専用イベントループを`src/ime/`へ移しました。IBus固有の型、通知の変換、接続処理は`src/ibus/client.c`に残し、関数ポインターのインターフェイスで共通層へ接続しています。GTK 3／4とnative Waylandは共通APIを使用します。公開設定と経路選択の挙動は、この段階では変更していません。

共通APIを使う候補ドライバーに、選択したバックエンドの診断と、接続待ちの間にデフォルトのGLibコンテキストを実行しない検証を追加しました。共通APIがない状態でビルドが失敗することを確認し、実装後に成功しています。

検証コマンドは`make -j4 test-ibus-candidates test-wayland-input test-gtk-ibus-direct test-gtk-ibus-auto test-gtk-ime-x11 test-gtk3-candidates test-gtk4-candidates`です。全ターゲットが成功し、IBusの変換・確定・キャンセル、候補、周辺文字列編集、フォーカス移動、遅延応答、停止・再接続と、GTK 4／X11への委譲を確認しました。専用イベントループと入力世代の検証を含め、段階1の完了条件を満たしています。

fcitx 5の実装は段階2から進めます。段階3の全体テストと3バージョンの実デーモン試験が完了するまでは、fcitx 4を実装しません。

### 段階2の通信バックエンドを実装

`src/fcitx5/client.c`に、fcitx 5のD-Bus接続、入力コンテキストの生成、キー送信、装飾付き未確定文字、確定、転送キー、周辺文字列と削除要求を追加しました。テストドライバーから共通APIへfcitx 5を明示指定できます。この通信バックエンドに続いて、下記の公開設定とGTK経路を追加しました。

キーの共通形式では、evdevコード、修飾状態、押下・解放、リピート、時刻を分けています。GTKとWaylandが受信した時刻を保持し、fcitx側でXKBコードと専用フラグへ変換します。フォーカス喪失時にローカルで確定する文字列には、fcitxが`DontCommit`を付けた部分を含めません。

接続先の監視とコールバックは専用GLibコンテキストで処理します。サービスを自動起動せず、入力コンテキストはサービス所有者のunique nameに接続します。共有セッションバスはクライアント破棄時にも閉じません。監視解除には完了通知を設け、初回接続の完了前に破棄する場合も確認しました。[GLibの名前監視](https://docs.gtk.org/gio/func.bus_watch_name.html)、[監視解除後の完了通知](https://docs.gtk.org/gio/func.bus_unwatch_name.html)

`make test-fcitx5`では模擬D-Busサービスを使い、装飾とUTF-8バイト位置、確定禁止部分、キー転送、未処理キー、押下・解放とリピート、削除と確定をまとめた文書編集、フォーカス移動後の遅延応答、結果不明のキーを再送しない処理を検証しています。共通APIがない状態の失敗に加え、リピート能力フラグを送信しない状態での実行失敗も確認し、実装後に成功しました。

キー形式の変更後も、`test-ibus-candidates`、`test-wayland-input`、`test-gtk-ibus-direct`、`test-gtk-ibus-auto`は成功しています。段階2は継続中です。公開設定とGTK経路の検証は下記まで進みました。native Waylandのfcitx入力試験と、実際のfcitx 5.1.7とMozcによる動作確認が残っています。段階3の候補、倍率、自動選択、所有者交代を含む再接続試験、全体テストも未完了です。

### 段階2の公開設定とGTK経路を実装

GTKの`BOXRIG_GTK_IME_FCITX5`とnative Waylandの`BOXRIG_WAYLAND_IME_FCITX5`を追加しました。付属ターミナルでは`BOXRIG_GTK_IME=fcitx5`または`BOXRIG_WAYLAND_IME=fcitx5`で指定でき、経路の診断にもfcitx5を表示します。現在のfcitx 5経路ではSYSTEM候補を使用します。クライアント候補と自動検出は段階3の作業です。

`make test-gtk-fcitx5`でGTK 3／X11、GTK 3／Wayland、GTK 4／Waylandの明示接続を検証しました。確認したのは、装飾付き未確定文字とカーソル、キャンセル、確定、IMEが処理しないキーの通常入力、フォーカス喪失時の確定です。GTK 4／X11はGTKのfcitxモジュールへ委譲し、直接接続の入力コンテキストを作らないことも確認しています。

3種類の付属ターミナルのビルドと、`test-gtk-ibus-auto`、`test-wayland-input`、`test-fcitx5`は成功しました。native Waylandのfcitx入力、実デーモン、倍率を伴う候補位置、診断項目のIME共通化は未検証または未完了です。段階2全体の完了とは扱いません。

### 段階2完了

インストール済みのfcitx5 5.1.7とMozcで、GTK 3／X11、GTK 3／Wayland、GTK 4／Wayland、native Waylandの入力を確認しました。実デーモンの検証コマンドは`make test-fcitx5-real`です。「きしゃ」の未確定文字、スペースによる漢字変換、選択中の文字列の確定、Escapeによるキャンセル、フォーカス移動時の「あ」の確定が成功しました。native Waylandでは、テスト用コンポジターから実際のWaylandキーイベントを送信しています。

テストは専用のセッションバスと一時的な設定ディレクトリで実行します。Mozcの学習データも分離し、デスクトップのIME設定は変更しません。デーモンのサービス所有権を取得してから入力ドライバーを起動し、起動前に終了した場合も検出します。Mozcが旧形式の`~/.mozc`を使う環境では分離できないため、このドライバーは実行を中止します。[Mozcの設定ディレクトリ選択](https://github.com/google/mozc/blob/master/src/base/system_util.cc)

模擬サービスの検証には`make test-wayland-fcitx5 test-gtk-fcitx5 test-fcitx5`を使用しました。対象の各ホストで装飾付き未確定文字、キャンセル、確定、未処理キー、IMEからの転送キーを確認しています。「前かな後」の「かな」を削除して「記者」を挿入する操作は、1回の文書編集となり、端末への文字出力は発生しません。共通層では、結果不明の送信済みキーを再送しない試験も成功しています。

これらの結果により段階2の完了条件を満たしました。候補ウィンドウの表示・配置は今回の実デーモンドライバーでは検証していません。候補の所有者、倍率、自動検出、所有者交代時の再接続、他の検証対象バージョンと全体テストは、続く段階3で確認します。

### 段階3の所有者交代を検証

別のD-Bus接続がfcitx 5のサービス名を取得した後に、旧所有者がキーのエラー応答を返す試験を追加しました。修正前は新しい接続まで未接続扱いとなり、入力を再開できませんでした。入力要求に接続世代を記録し、旧世代の失敗で新しい接続を破棄しないように修正しています。

この試験は修正前に失敗し、修正後に成功しました。旧所有者の確定通知と結果不明のキーは破棄され、新しいコンテキストで入力を再開できます。`make -j4 test-fcitx5 test-ibus-candidates test-wayland-input test-gtk-ibus-direct`も成功しています。候補表示と自動検出を含む段階3全体は継続中です。
