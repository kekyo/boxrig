# カーソルと選択範囲の見た目を変える

[English](../en/decorations.md)

boxrig UIでは、カーソルを細い縦棒へ変えたり、選択範囲を色で塗ったりできます。
形や色は表示先ごとに指定できるため、同じ端末を異なる見た目で表示することもできます。
boxrig coreが保持する文字属性や、コピーする範囲そのものは変更しません。

## カーソルを設定する

以下は、カーソルを縦棒にし、点滅を止める関数です。
作成済みのUI表示先を渡し、通常のフレーム処理を進めると見た目が変わります。
表示先の作成と描画は、[UI向けstep-by-step](./step-by-step/step-by-step-ui.md)を参照してください。

```c
#include <boxrig/ui.h>

boxrig_ui_status use_steady_bar(boxrig_ui_view *view)
{
    boxrig_ui_cursor_style style = boxrig_ui_cursor_style_default();
    style.shape = BOXRIG_UI_CURSOR_SHAPE_BAR;
    style.blink = BOXRIG_UI_CURSOR_BLINK_STEADY;
    style.line_width_dip = 2.0f;
    return boxrig_ui_view_set_cursor_style(view, &style);
}
```

通常の描画後、論理的に表示可能なカーソルが2 DIPの縦棒になります。
線幅はDPIと出力倍率に応じて画素へ丸め、セルの範囲へ収めます。
端末からのカーソル非表示、IMEによる抑制、フォーカスによる条件は引き続き有効です。

## 端末からの形状指定

既定のstyleは、形、色、点滅を端末の指定へ追従させます。
端末のDECSCUSRによるblock、underline、barを使う場合はshapeをTERMINALにします。

blockの既定表示は輪郭です。
`block_presentation`をFILLにするとセルを塗り、文字を読める色で重ねます。
明示したOUTLINE、UNDERLINE、BARではこの項目を使いません。
色は端末のカーソル色へ追従するか、固定RGBへ変更できます。

`reset_cursor_style()`で表示先の既定設定へ戻せます。
端末のDECSCUSRやOSC 12の状態をリセットする操作ではありません。

## 選択範囲の見た目を変える

`boxrig_ui_selection_style`で、輪郭、塗り、両方から選び、背景色、文字色、枠の色と不透明度を指定します。
既定は青い輪郭です。
`set_selection_style()`でコピーされ、`reset_selection_style()`で既定へ戻せます。

塗りのalphaと枠のalphaは独立しています。
文字色を元のままにするか、固定色にするかも選べます。
これらは表示の変更なので、選択の論理範囲とスナップショットの本文は維持します。

## 色補正や点滅と組み合わせる

装飾の色を決めた後で[色補正](./color-filters.md)を適用します。
カーソルのFILLでは補正後の色と下地を使って文字を描きます。
選択の塗りと枠も、それぞれ対応する補正対象になります。

点滅の有効・無効と間隔は[点滅設定](./blinking.md)で管理します。
styleが点滅を要求していても、表示先のカーソル点滅を無効にしていれば固定表示です。
concealedや文字点滅で非表示の文字を、選択やカーソルの塗りで露出させることはありません。

## 表示先ごとに変更する

同じ端末とモデルを共有していても、styleは各表示先が保持します。
例えば、操作する表示先だけ縦棒にし、別の表示先は端末の指定を使えます。
変更後は通常のフレーム要求に従って描画してください。

デモの切り替え操作は[boxrig_term](./boxrig_term.md)にまとめています。
styleの全項目と値の条件は[公開ヘッダー](../../include/boxrig/ui/decoration.h)を参照してください。

[文書目次](./index.md)
