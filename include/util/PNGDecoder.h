#pragma once

extern void initPNGDecoder(void);
extern bool decodeImgGrey(const void *data, size_t size, lv_img_dsc_t **img);
extern bool decodeImgColor(const void *data, size_t size, lv_img_dsc_t **img);
// one part of a decoded tile, enlarged to the size of the whole tile; nullptr when that cannot be done
extern lv_image_dsc_t *stretchImg(const lv_image_dsc_t *src, uint8_t levels, uint32_t column, uint32_t row);
