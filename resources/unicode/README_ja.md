# Unicodeの単語境界データ

[English](./README.md)

`18.0.0/`には、[UAX #29の既定の単語境界](https://www.unicode.org/reports/tr29/tr29-49.html)を実装・検証するための固定データを収録しています。
coreで使うutf8procと同じUnicode 18.0.0を使います。

原本の取得先は次のとおりです。

- https://www.unicode.org/Public/18.0.0/ucd/auxiliary/WordBreakProperty.txt
- https://www.unicode.org/Public/18.0.0/ucd/auxiliary/WordBreakTest.txt
- https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt
- https://www.unicode.org/license.txt

`node tools/unicode/generate-word-break.mjs --download`は原本を取得し、固定したSHA-256と照合してから`src/ui/word_break_data.inc`を再生成します。
通常の再生成は保存済みの入力を使い、ネットワークには接続しません。
`--check`では入力のSHA-256と生成結果を検証し、ファイルは変更しません。

データと生成したテーブルには、`18.0.0/LICENSE.txt`に記載したUnicode License V3が適用されます。
