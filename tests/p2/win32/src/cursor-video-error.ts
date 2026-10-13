/** Pixel rectangle excluded because the cursor may change there. */
export interface CursorRegion {
  /** Left edge in frame pixels. */
  readonly x: number;
  /** Top edge in frame pixels. */
  readonly y: number;
  /** Width in frame pixels. */
  readonly width: number;
  /** Height in frame pixels. */
  readonly height: number;
}

/**
 * Measures luminance changes outside the cursor against the source screenshot.
 * @param frame Decoded RGB24 frame with the specified dimensions.
 * @param reference RGBA32 screenshot with the same dimensions.
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @param cursor Cursor bounds including the codec's surrounding pixel margin.
 * @returns Maximum mean absolute luminance error per 2×2 block, on the 0–255 scale.
 * @remarks H.264 edge reconstruction can change individual samples at a fixed
 * color boundary. Average absolute errors over 2×2 blocks without allowing
 * opposite changes to cancel. Only cursor pixels are excluded from the metric.
 */
export const cursorVideoStaticError = (
  frame: Uint8Array,
  reference: Uint8Array,
  width: number,
  height: number,
  cursor: CursorRegion
): number => {
  let maximum = 0;
  for (let y = 0; y < height; y += 2)
    for (let x = 0; x < width; x += 2) {
      let sum = 0;
      let count = 0;
      for (let yy = y; yy < Math.min(y + 2, height); yy++)
        for (let xx = x; xx < Math.min(x + 2, width); xx++) {
          if (
            xx >= cursor.x &&
            xx < cursor.x + cursor.width &&
            yy >= cursor.y &&
            yy < cursor.y + cursor.height
          )
            continue;
          const pixel = yy * width + xx;
          sum += Math.abs(
            0.299 * (frame[pixel * 3] - reference[pixel * 4]) +
              0.587 * (frame[pixel * 3 + 1] - reference[pixel * 4 + 1]) +
              0.114 * (frame[pixel * 3 + 2] - reference[pixel * 4 + 2])
          );
          count++;
        }
      if (count !== 0) maximum = Math.max(maximum, sum / count);
    }
  return maximum;
};
