# 文字処理の処理量と保持量

[English](../en/text-workload-measurements.md)

検索、出現監視、正規表現認識、コピー・貼り付けで使う処理量とメモリ量を調べる方法を説明します。
計測ドライバーは、長い履歴や多数の一致を含む入力を作り、訪問したセルやバイト、確保量、通知キューの最大件数を出力します。
APIの使い方と短いコード例は[文字列のガイド](./text.md)を参照してください。

## 対象機能

完全一致検索の再開、出現監視の初期走査と変更通知、正規表現による認識、文字列のコピーと貼り付けを対象にします。
処理したセルやバイト、公開APIのwork unit、確保量を数え、処理量と保持量の上限を確認します。
通常テストの合否に経過時間は使いません。

## 実行方法

リポジトリのルートで実行します。ビルド環境は[ビルドガイド](./building.md)を参照してください。

```sh
make measure-text-workloads
```

各workloadを別プロセスで起動し、`MEASURE text workload=...`から始まる行を標準出力へ出します。
計測用ビルドだけが`BOXRIG_MEASURE_TEXT_WORKLOADS`を定義し、通常ライブラリにはcounterや計測分岐を含めません。
既定のコンパイル設定は`-O2 -g`で、ASan／UBSanは付けません。
入力の作成と計測処理は[measure_text_workloads.c](../../tests/measure_text_workloads.c)、ビルドと実行対象は[Makefile](../../Makefile)で確認できます。

多数の既存の一致を保持したまま更新する条件は、ビルド後に個別に実行できます。

```sh
build/text-measurement/measure_text_workloads recognizer-retained-matches
```

全体テストでは、このドライバーの`self-test`を使い、計数と結果の整合性を小さな入力で確かめます。
結果を保存する場合は、対象コミット、入力、ビルド設定、実行環境も一緒に記録してください。

## 入力と指標

| workload | 入力・目的 |
| --- | --- |
| `maximum-scrollback` | 1列、履歴1,048,576行で不一致検索 |
| `long-soft-wrap` | 262,145個の`a`からなる1論理行で不一致検索 |
| `many-matches` | 65,536行の`hit`を初期監視で列挙 |
| `continuous-output` | 1 MiBの初期データを走査する前に、256行の`hit`を追加 |
| `large-copy-paste` | 1 MiBのsnapshotを64 KiBずつ16断片で送信 |
| `recognizer-long-line` | 1,048,576個の`a`を`a+`で1件に認識 |
| `recognizer-many-patterns` | 4,096個の`a`へ32認識器を登録 |
| `recognizer-continuous-output` | 同じ論理行を4,096回`hit`／`miss`へ交互に変更 |
| `recognizer-off-viewport` | 262,143行の履歴先頭にある`target`を認識 |
| `recognizer-retained-matches` | 1,024行の既存の一致を保持したまま別の行を更新し、一致の識別子を確認 |

最後の条件は個別実行用で、`make measure-text-workloads`は先頭の9条件を実行します。
先頭の9条件では、画像や通知など、対象外の容量を0にします。
イベント容量には必要件数と終端の余裕を設け、overflowの確認と保持量の測定を区別します。
このうち認識器の4条件はdirty line容量を1とし、不一致の履歴全体を保持しないことも確認します。

| 出力項目 | 意味 |
| --- | --- |
| `fragments`、`projected_bytes` | 履歴と表示中の物理行断片数、対象となる論理本文のUTF-8バイト数 |
| `visited_cells`、`visited_bytes` | hash、射影、検索・監視で実際に訪問した量 |
| `work_units`、`dispatches` | 公開budgetから消費した量、budget 4,096での呼び出し回数 |
| `recognizers`、`events`、`matches` | 登録した認識器、取り出したイベント、一致の件数 |
| `input_bytes`、`paste_chunks` | 処理対象の入力バイト数、貼り付けで送信した断片数 |
| `allocations`、`allocation_peak_bytes` | 計測区間での確保回数、その区間で始めた確保の同時生存量の最大値 |
| `peak_rss_kib` | 入力の構築を含むプロセス生存期間の最大RSS |
| `event_queue_high_water`、`staging_queue_high_water` | 監視の公開・保留キューに同時に保持したイベントの最大件数 |
| `recognizer_event_queue_high_water` | 認識のイベントキューの最大保持件数 |
| `recognizer_prepare_steps`、`recognizer_unbudgeted_prepare_steps` | 認識の行末・UTF-8・境界・準備処理のstep数、そのうちbudgetへ計上しなかった数 |
| `recognizer_cursor_creations` | NFAの状態領域を新しく確保した回数 |
| `recognizer_match_identity_comparisons` | 保持済みの一致と識別子を照合した回数 |

条件ごとに計測する項目が異なり、値を設定しない項目は0になります。
例えば`recognizer-retained-matches`は識別子の維持と比較回数を調べる条件で、RSSは計測しません。

## 処理量と保持量の読み方

完全一致検索は、追加のindexを持たず、budget付きの線形走査を続けます。
長い1論理行も複数回に分けて処理でき、消費量の合計と再開回数を`work_units`と`dispatches`で確認できます。
監視では変更されない初期snapshotを走査し、その間の将来イベントを固定容量で保持します。
内容のhashは変更行について再計算します。

認識器は行末の探索やUTF-8の準備処理もbudgetへ計上し、NFAの状態領域を再利用します。
`recognizer_unbudgeted_prepare_steps`は、準備処理に計上漏れがないかを調べる指標です。
`recognizer-retained-matches`では、更新しなかった行の一致の識別子が保たれることも確かめます。

コピーでは必要範囲のデータを所有するsnapshotを作り、貼り付けでは既存のtransactionへ分割して送ります。
この条件の`work_units`が0でも、コピーや送信がなかったという意味ではありません。
検索の再開処理が消費する単位と、コピー・貼り付けのバイト数や断片数を分けて読んでください。

確保量はlinker wrapperで数えます。
初期データを構築した後から計測する条件では、そのデータの確保量を`allocation_peak_bytes`へ含めません。
RSSは初期データも含むため、両者は測定範囲が異なります。

## 限界と関連API

この計測で確認できるのは、指定した入力と容量設定での処理量、保持量、結果の整合性です。
RSSはOSやallocatorにも依存します。
一般の操作時間や、ほかの端末との速度の比較に読み替えないでください。

予算や容量の指定、結果の取得は[文字列のガイド](./text.md)、現在の管理方法は[内部設計](./development/internals.md)を参照してください。
Unicode変換の試作と公開機能の違いは[検索変換の試作評価](./search-capability-prototype.md)で説明しています。

[ドキュメント目次](./index.md)
