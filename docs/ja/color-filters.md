# 表示先ごとに色を補正する

[English](../en/color-filters.md)

boxrig UIの色補正を使うと、同じ端末を通常色と高いコントラストで同時に表示できます。
boxrig coreが保持する文字や色を変えずに、表示先ごとの描画色を変更する機能です。

## 色補正でできること

セル、背景、カーソル、選択や検索の枠、リンクのhover、IMEなど、描画する対象に応じて色を変更できます。
文字や属性、コピーする本文は変わりません。
背景のalpha、Kitty画像、カラー文字そのものの画素は、この補正の対象ではありません。

## 補正関数を登録する

以下は、セルとIMEの文字色を白へ変更する例です。
`white_text()`が色を選ぶコールバック、`use_white_text()`が作成済みのUI表示先へ登録する関数です。
登録後もアプリケーションで通常のフレーム処理を進めてください。

```c
#include <boxrig/ui.h>

static boxrig_ui_color_filter_result white_text(
    const boxrig_ui_color_filter_input *input, void *context)
{
    (void)context;
    /* Keep the original colors for targets that are not changed below. */
    boxrig_ui_color_filter_result colors =
        boxrig_ui_color_filter_result_default(input);

    if (input->target == BOXRIG_UI_COLOR_CELL ||
        input->target == BOXRIG_UI_COLOR_IME)
        colors.foreground = (boxrig_rgb){255, 255, 255};
    return colors;
}

boxrig_ui_status use_white_text(boxrig_ui_view *view)
{
    return boxrig_ui_view_set_color_filter(view, white_text, NULL);
}
```

通常のフレーム処理を進めると、対象の前景色が白になります。
まずdefault関数で元の色を受け取り、変更したい項目だけを上書きします。
隠す属性の文字を、補正によって表示することはありません。

## 文字・背景・カーソルの色

入力には、解決済みの元の色、色の出自、属性、選択やフォーカスの状態が含まれます。
RGBはalphaを乗算していないsRGBです。
反転などが適用済みなので、同じ反転を補正側で繰り返さないでください。

下線が前景色に追従する設定なら、返した前景色へ合わせて解決します。
カーソルや枠の補正では、下地の補正済みセル色も参照できます。
点滅の位相は補正関数へ渡さず、位相だけの変化では補正済みの色を再利用します。

## 設定の更新と解放

同じ入力と同じ設定なら、同じ結果を返す関数にしてください。
呼び出し回数や順番は保証しません。
通知していない外部状態や現在時刻で結果を変える使い方には適しません。

contextはアプリ所有です。
内容を変えた場合は`boxrig_ui_view_invalidate_colors()`で再計算を要求します。
解除は`set_color_filter(view, NULL, NULL)`です。
解除または表示先の破棄が終わるまでcontextを保持してください。
コールバック中に同じ表示先やモデルを更新、描画、破棄しないでください。

## 独自描画での利用と制限

独自レンダラーでは、`boxrig_ui_color_resolve_cell()`で元の表示色を解決し、選択などの状態をそろえてから`boxrig_ui_view_resolve_colors()`へ渡します。
結果は対応する描画処理と一緒に保持し、次回の補正入力へ再投入しません。

[実行可能な例](../../examples/ui/color_filter.c)では背景の明るさに応じて白黒を選び、設定更新と解除を確認できます。
coreのセルに保存された色が変わらないことも検証します。
保持量が設定上限を超える場合や、補正中の再入は失敗として扱ってください。

[色補正API](../../include/boxrig/ui/color.h)、[表示先API](../../include/boxrig/ui/view.h)、[文書目次](./index.md)
