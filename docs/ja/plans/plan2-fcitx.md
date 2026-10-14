対応可能です。現在の構造なら、直接IBusを使う部分を共通のIME接続層に整理し、fcitx用バックエンドと自動選択を追加できます。まずfcitx 5を対象にするのが妥当です。fcitx 4も対応は可能ですが、通信仕様が異なるため、別の対応範囲として扱う必要があります。

現在のコードで確認した経路は次のとおりです。

| 環境 | 現在の実装 | 今回の対応範囲 |
|---|---|---|
| native Wayland | text-input経由、または直接IBus | 直接接続の部分 |
| GTK 3／Wayland、GTK 4／Wayland | GtkIMContext経由、または条件に応じて直接IBus | 直接接続の部分 |
| GTK 3／X11 | GtkIMContext経由、または条件に応じて直接IBus | 直接接続の部分 |
| GTK 4／X11 | IBus指定時もGTKのIBusモジュール経由 | 対象外 |
| native X11 | `XOpenIM()`によるXIM経由 | 直接IBusではないため対象外 |

選択処理は[GTK側のime_probe.c](/home/kouji/Projects/boxrig/src/gtk/ime_probe.c:172)と[Wayland側のibus.c](/home/kouji/Projects/boxrig/src/wayland/ibus.c:129)、直接通信は[共通のIBusクライアント](/home/kouji/Projects/boxrig/src/ibus/client.c:18)にあります。native X11については、ドキュメントに直接IBusという記載がありますが、[実装はXIMです](/home/kouji/Projects/boxrig/src/x11/input.c:3932)。

fcitx 5の公式APIには、非同期キー処理、確定文字列、装飾付き未確定文字、フォーカス、カーソル位置、周辺文字列と削除要求、候補一覧の通知がそろっています。boxrigの既存機能に対応付けられるため、技術的な障害は見当たりません。[公式クライアントAPI](https://github.com/fcitx/fcitx5-gtk/blob/master/fcitx-gclient/fcitxgclient.h)、[通知と引数の説明](https://github.com/fcitx/fcitx5-gtk/blob/master/fcitx-gclient/fcitxgclient.c)

ただし、fcitx 5のIBus互換機能を利用するだけでは十分ではありません。確認した互換実装では、IBusの候補一覧・補助文字列の能力指定を、fcitxのクライアント側候補表示へ反映していません。基本入力が動く構成はあり得ますが、現在のboxrigが持つ候補表示まで保証するには、fcitxのネイティブなD-Bus経路を追加する方が確実です。[IBus互換実装](https://github.com/fcitx/fcitx5/blob/master/src/frontend/ibusfrontend/ibusfrontend.cpp)

自動検出は、「GTKやWaylandに任せるか」と「直接接続するIMEはどれか」を分けて判断する設計を勧めます。

1. アプリケーションの明示設定と、既存のGTK委譲経路を優先する。例えばGTKがfcitxモジュールを使っている場合は、そのままGTKへ任せます。
2. 直接接続が必要な場合、`XMODIFIERS=@im=fcitx`、`GTK_IM_MODULE=fcitx`、`IBUS_ADDRESS`などを選択の手掛かりにする。
3. 実際の接続先を非同期で確認する。fcitx 5はセッションバスの`org.fcitx.Fcitx5`、IBusは既存のアドレス取得処理で得た接続先を確認します。インストール済みというだけでは選択しません。
4. 環境変数がなくても、利用可能な接続先が一つなら自動選択する。両方が独立して稼働している場合は、AUTOの優先順と明示指定方法を定め、選択理由を診断へ出します。
5. サービスの終了・再起動を監視し、再接続時には古い入力応答を破棄する。検出のために別のIMEデーモンを起動することは避けます。

環境変数だけで判断しない点が重要です。fcitx公式でも、Waylandでは`GTK_IM_MODULE`を設定しない構成を案内しています。また、fcitxがIBus互換サービスを提供する場合があるため、IBusのサービス名が見えることだけでも判定できません。[Waylandでの公式設定](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland)

実装方式としては、既存のGDBusを使うfcitxバックエンドを推奨します。公式の`Fcitx5GClient`も候補ですが、調査した実装には次の相違があります。

- 接続の再確認に、デフォルトのGLibコンテキストへ登録されるタイマーを使っています。boxrigは[専用のGMainContextだけを処理する設計](/home/kouji/Projects/boxrig/src/ibus/client.h:75)です。
- キー処理の完了APIが、通信エラーと「IMEがキーを処理しなかった」を同じ`FALSE`で返します。boxrigは[通信結果が不明なキーを再送しない](/home/kouji/Projects/boxrig/src/ibus/client.c:1005)ため、この区別が必要です。

このため、そのライブラリをそのまま組み込むより、既存の非同期処理・入力順序管理を使ってD-Busへ接続する方が、現在の設計に合います。[公式クライアント実装](https://github.com/fcitx/fcitx5-gtk/blob/master/fcitx-gclient/fcitxgclient.c)、[GLibタイマーの仕様](https://docs.gtk.org/glib/func.timeout_add_full.html)

変更時に特に注意するのは、次の部分です。

- 入力データの変換：キーコード、キー解放の表現、未確定文字の位置単位、フォーカス喪失時の確定規則にはIBusとの差があります。
- 候補表示：fcitxは表示中の候補と前後ページの有無、上下の補助文字列を通知します。[現在の候補モデル](/home/kouji/Projects/boxrig/include/boxrig/ui/candidates.h:48)との調整が必要です。Waylandでは候補位置の座標・倍率も含めて対応します。
- 候補表示の所有者：fcitx側が環境に応じてクライアント表示を抑制する場合があるため、要求した設定だけで「クライアント表示中」と判定できません。[fcitx側の候補表示処理](https://github.com/fcitx/fcitx5/blob/master/src/frontend/dbusfrontend/dbusfrontend.cpp)
- 公開設定と診断：現在のIBus固有のenum、接続状態、エラー件数、サンプルの起動オプションを、fcitxも表現できる形へ変更します。

なお、native Waylandのライブラリ既定値は[WAYLAND固定](/home/kouji/Projects/boxrig/src/wayland/context.c:564)で、付属ターミナルは[AUTOを明示指定](/home/kouji/Projects/boxrig/examples/boxrig_term/wayland/native_host.c:1844)しています。ライブラリ利用者にも自動検出を既定で提供するなら、この既定値も検討対象です。

実装を進める場合は、以下の単位で完了を確認できます。

1. 共通IME層への整理：既存IBusの入力・候補・再接続テストが通ること。
2. fcitx 5の明示接続：対象のGTK／Waylandで、変換・確定・キャンセル・候補表示・周辺文字列編集が動くこと。
3. 自動選択：IBusのみ、fcitxのみ、環境変数なし、両方存在、停止・再起動を検証し、GTK委譲経路の維持、入力の二重配送防止、全体テストの成功を確認すること。

fcitx 4まで対象にする場合は、その後に別バックエンドとして追加するのが自然です。従来の`CreateICv3`やキー処理の型などがfcitx 5と異なり、fcitx 5対応だけで両方をカバーしたとは扱えません。[fcitx 4の通信実装](https://github.com/fcitx/fcitx/blob/master/src/lib/fcitx-gclient/fcitxclient.c)

今回はコードと公式資料による調査までで、ファイル変更はありません。この環境にはfcitxがインストールされていないため、実デーモンでの動作は未検証です。
