# 検索変換の試作評価

[English](../en/search-capability-prototype.md)

文字列の検索へ大文字小文字の変換やUnicode正規化を加える試作について、評価結果と採否を記録しています。
先に採否を示し、その後で変換、元の文字位置との対応、処理量、正規表現の順に根拠を説明します。
現在利用できる検索と認識のAPIは[文字列のガイド](./text.md)を参照してください。

## 評価の目的と採否

Unicodeの大文字小文字変換、正規化、正規表現を、処理量とメモリ量を制限した検索へ加えられるか評価した記録です。
現在の完全一致検索はUTF-8のバイト一致を使い、case foldingや正規化のflagを持ちません。

試作では変換後の一致と元の文字範囲への復元を確認しました。
ただし、一つの変換を整数の処理量で中断・再開し、外部ライブラリ内の確保も上限内に収める条件を満たさなかったため、完全一致検索への追加を採用していません。

## 試作と公開機能の違い

試作は[search_transform_prototype.c](../../tests/search_transform_prototype.c)にあり、製品ライブラリへリンクしません。
固定したutf8proc 2.11.3とUnicode 17.0.0を使い、全1,112,064 Unicode scalarの変換もテストします。

後から追加した正規表現認識は、別の公開機能です。
範囲を制限した言語と処理量を持つNFAを用い、完全一致検索のflagを増やす方式ではありません。
現在の利用方法は[文字列の取得・選択・検索](./text.md)を参照してください。

## 大文字小文字変換と正規化

| 評価した変換 | 意味 | 含めないこと |
| --- | --- | --- |
| default case fold | Unicode既定の大文字小文字の同一視 | locale固有の変換、正規化 |
| NFC | canonicalな分解と合成 | compatibility mapping |
| canonical caseless | default case foldとcanonical合成 | locale固有の変換 |

NFKC、accent除去、`UTF8PROC_IGNORE`は対象外です。
例えばNFCは`①`と`1`を同一視しません。
default case foldで`İ`は`i`と結合dotになり、ASCIIの`i`だけとは一致しません。
意味と所有権は[固定版のヘッダー](https://github.com/JuliaStrings/utf8proc/blob/v2.11.3/utf8proc.h)と[公式C API](https://juliastrings.github.io/utf8proc/doc/utf8proc_8h.html)を照合しています。

## 元の文字位置との対応

元の書記素境界と、同じ位置まで変換した後のバイト境界を二つの配列へ保持します。
一致の開始・終了が両方とも対応表にある場合だけ、元の範囲を返します。

| 元の文字列 | 検索文字列・変換 | 結果 |
| --- | --- | --- |
| `Straße` | `STRASSE`、case fold | 一致。元の0〜7バイト |
| `ß` | `ss`、case fold | 一致。元の0〜2バイト |
| `ß` | `s`、case fold | 変換途中で終わるため不一致 |
| `Cafe`＋U+0301 | `Café`、NFC | 一致。元の0〜6バイト |
| `E`＋U+0301 | `é`、canonical caseless | 一致。元の0〜3バイト |

これらの範囲は終端を含みません。
製品へ採用するには、書記素ごとの変換を連結した結果と文書全体の変換が一致することを、固定Unicode版のNormalizationTestと境界全体で確認する必要があります。
代表例の成功だけでは公開契約を確定しません。

## 処理量とメモリの条件

元と変換後のUTF-8はそれぞれ65,536バイト、一度の変換へ渡す書記素は4,096バイト、境界は最大65,537件です。
保持量の上限は`65,537 + 2 × 65,537 × sizeof(size_t)`で、64-bitなら1,114,129バイト、32-bitなら589,833バイトです。
失敗時は途中のオブジェクトを返しません。

全scalarを単独で変換した最大出力は、case foldがU+0390の2→6バイト、NFCとcanonical caselessがU+1D160の4→12バイトでした。
これは固定データ版の実測であり、将来版の上限として固定しません。

`utf8proc_map()`は内部で必要量を確保し、callerのallocatorや確保前のhard limitを受け取りません。
一度の処理の途中状態を返す再開APIもありません。
この[公開APIの条件](https://github.com/JuliaStrings/utf8proc/blob/v2.11.3/utf8proc.h)により、現在の検索と同じ処理量・確保の契約を実現できません。

再検討する場合は、scalar変換、canonical ordering、合成を整数単位で再開できるcursorと、caller所有の固定容量storage、元の境界への対応を先に設計します。
文字列の変換結果だけを追加して済む変更ではありません。

## 正規表現

POSIXの[regexec](https://pubs.opengroup.org/onlinepubs/9799919799/functions/regexec.html)は、標準APIに長さ付きsubject、中断・再開、stack／heapと処理量の上限がなく、Win32との共通実装にもならないため採用しませんでした。

[PCRE2](https://www.pcre.org/current/doc/html/pcre2api.html)には長さ付きsubject、match・depth・heap limit、calloutによる中断があります。
しかし中断した状態から継続するcursorではなく、再呼び出しでは探索をやり直します。
JITでは制限の意味も変わります。
全本文の上限付きコピーを渡す方法は、全履歴のコピーを避ける検索の条件と一致しません。

現在の認識機能には、boxrig独自のThompson NFAを使っています。
state、処理量、論理行、結果、snapshotの上限とpull eventを公開しています。
後方・折り返し付きの対話的regex search、capture、backreference、lookaround、正規化、case foldingは追加していません。

## 再検証方法と出典

`make test`に試作の機能・確保・全scalar検査が含まれます。
個別の接続は[Makefile](../../Makefile)と[試作テスト](../../tests/test_search_transform_prototype.c)で確認できます。
試作を本番へリンクして試す必要はありません。

正規表現の外部条件は[PCRE2の制限](https://www.pcre.org/current/doc/html/pcre2limits.html)と[build・resource limits](https://www.pcre.org/current/doc/html/pcre2build.html)、採用した認識器の処理量は[文字処理の測定](./text-workload-measurements.md)を参照してください。

[ドキュメント目次](./index.md)
