# boxrigと第三者コンポーネントのライセンス

boxrig自身のコードはMITライセンスです。本文は`LICENSE`にあります。
組み込んだライブラリや生成データには、それぞれの著作権表示と配布条件が適用されます。
再配布する成果物に対応する本文と表示を保持してください。
このディレクトリには同じOS向けSDKの各構成で使う表示をまとめて収録しています。
個別のライブラリだけを使う場合、表にあるすべてのコンポーネントを使うとは限りません。

## 共通の組み込みコードとデータ

| コンポーネント | 使用箇所 | ライセンス本文 |
| --- | --- | --- |
| [utf8proc](https://github.com/JuliaStrings/utf8proc) | coreのUnicode処理 | `utf8proc-LICENSE.md`。MITとUnicodeデータの表示を含む全文 |
| [libpng](https://github.com/pnggroup/libpng) | coreのPNG処理 | `libpng-LICENSE`。PNG Reference Library License |
| [zlib](https://github.com/madler/zlib) | coreの圧縮処理 | `zlib-LICENSE` |
| [Unicode 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/) | UIの単語境界テーブル | `Unicode-LICENSE.txt`。Unicode License V3 |
| [WHATWG Encoding](https://encoding.spec.whatwg.org/) | coreのShift_JIS変換テーブル | `WHATWG-Encoding-LICENSE`。コードへ組み込んだ部分はBSD-3-Clause |

libpngとzlibのバイナリ配布時の謝辞は必須ではありませんが、SDKには本文を収録しています。
utf8procに含まれるUnicodeデータの表示と、UIのUnicode License V3は両方を保持します。
WHATWGの本文には仕様文書向けのCC BY 4.0と、コードへ組み込む部分向けのBSD-3-Clauseが含まれます。
これらのファイルのライセンスをboxrigのMITへ置き換えるものではありません。

## Linux

Linux向けには、デモのURL起動処理で使う[cardio](https://github.com/kekyo/cardio)の`cardio-LICENSE`と、
[Waylandプロトコル](https://gitlab.freedesktop.org/wayland/wayland-protocols)の`Wayland-protocols-COPYING`、
`Wayland-protocols-NOTICES.md`を収録します。後者には生成コードの元になった各XMLの著作権表示と条件を残しています。

GTK 3、GTK 4、Pango、GLib/GObject/GIO、IBus、ATK、glibcなどのシステム共有ライブラリを使用します。
これらのライブラリとその使用にはLGPLが適用されます。本文は`LGPL-2.1.txt`にあります。
CairoはLGPL 2.1またはMPL 1.1から選択でき、この配布ではLGPL 2.1を選択します。
参照先は[GTK](https://docs.gtk.org/gtk4/)、[Cairo](https://www.cairographics.org/)、
[LGPL 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)です。

配布する実行ファイルは、利用者のシステムにある共有ライブラリへ動的リンクする構成を前提とします。
インターフェイスに互換性がある改変版への差し替えと、その動作確認のための改変・リバースエンジニアリングを妨げません。
システムライブラリ自体を同梱したり、静的に取り込んだりする場合は、対応ソースや再リンク手段などの配布条件を別途満たす必要があります。

## WindowsとGCCランタイム

Windows向けには、MinGW-w64ランタイムの表示を`MinGW-w64-LICENSE.txt`として収録します。
GCCのランタイムについては`GPL-3.0.txt`と`GCC-Runtime-Library-Exception.txt`を収録します。
LinuxのC++製デモでもGCCランタイムを使用します。
[GCCの例外条項](https://www.gnu.org/licenses/gcc-exception-3.1.html)を満たすビルドでは、
boxrig自身のコードをMITのまま配布できます。ランタイムのライブラリ自体を再配布する場合は、
そのライブラリについてソース提供などの条件を満たしてください。[GCCの説明](https://www.gnu.org/licenses/gcc-exception-3.1-faq.html)を参照してください。

通常のWindows構成はOS付属のConPTYを使い、Microsoftの実行ファイルを同梱しません。
同梱版ConPTYを選んだ場合は、実行ファイルとともに`LICENSE.conpty.txt`と`NOTICE.conpty.md`を保持してください。
上流のNOTICEにはWindows Terminalのほかの構成向けの表示も含まれます。
対象は[ConPTY 1.24.260710001](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.24.260710001)と、
対応する[Windows Terminal v1.24.11911.0](https://github.com/microsoft/terminal/releases/tag/v1.24.11911.0)です。

## 出自とソースからの利用

リポジトリの`packaging/licenses/`へ複製した上流の本文は、`sources.json`に取得元URLとSHA-256を記録しています。
MinGW-w64の本文は11.0.1、GCCの本文は13.2.0の上流配布物に対応します。
異なるツールチェーンで配布物を作る場合は、その版の条件を照合してください。
依存コードの版はリポジトリのsubmodule参照、生成データの版は各データの出自情報で確認できます。

boxrigをsubmoduleやGitHubのソースZIPから使う場合も、依存コードとデータに付属するライセンスを保持してください。
成果物を再配布する際は、利用した構成に対応する表示を同梱します。
SDKの部分的なアンインストールでは、ほかのモジュールも使うライセンス表示を残します。
